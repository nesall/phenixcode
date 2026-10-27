#include "model_router.h"
#include "app.h"
#include "cutils.h"
#include "chunker.h"
#include "inference.h"
#include "settings.h"
#include "json_shim.h"
#include <utils_log/logger.hpp>
#include <classifier/model.h>
#include <exception>
#include <algorithm>
#include <list>
#include <vector>
#include <string>
#include <string_view>
#include <filesystem>
#include <utility>
#include <unordered_map>

using json = nlohmann::json;

namespace {
  classifier::EmbeddedClassifier *getInternalClassifier() {
    static std::unique_ptr<classifier::EmbeddedClassifier> instance = []() -> std::unique_ptr<classifier::EmbeddedClassifier> {
      auto c = std::make_unique<classifier::EmbeddedClassifier>();
      auto modelPath = utils::getExecutableDir() / INTERNAL_MODEL_RELATIVE_PATH;
      if (!std::filesystem::exists(modelPath)) {
        LOG_MSG_ERROR << "Auto-router classifier [internal] model file not found: " << modelPath;
        return nullptr;
      }
      try {
        if (!c->load_from_file(modelPath.string())) {
          LOG_MSG_ERROR << "Auto-router classifier [internal] failed to load model: " << modelPath;
          return nullptr;
        }
      } catch (const std::exception &e) {
        LOG_MSG_ERROR << "Auto-router classifier [internal] exception while loading model: " << e.what();
        return nullptr;
      }
      return c;
      }();
    return instance.get();
  }

  size_t countUserTurns(const json &messages) {
    if (!messages.is_array()) return 0;
    size_t n = 0;
    for (const auto &m : messages) if (m.value("role", "") == "user") ++n;
    return n;
  }

  struct SessionTierCache {
    static constexpr size_t kMaxSessions = 512;

    // priorUserTurns = user turn count *excluding* the current question.
    classifier::Tier lookupPrefix(const std::string &sessionId, size_t priorUserTurns) {
      if (sessionId.empty() || priorUserTurns == 0) return classifier::Tier::Unknown;
      std::lock_guard<std::mutex> lock(mtx_);
      auto it = sessions_.find(sessionId);
      if (it == sessions_.end() || it->second.userTurnCount != priorUserTurns)
        return classifier::Tier::Unknown; // no entry, or client history diverged (edit/regenerate/clear)
      touchLocked(it);
      return it->second.tier;
    }

    void rememberFull(const std::string &sessionId, size_t userTurnCount, classifier::Tier tier) {
      if (sessionId.empty() || userTurnCount == 0) return;
      std::lock_guard<std::mutex> lock(mtx_);
      auto it = sessions_.find(sessionId);
      if (it == sessions_.end()) {
        evictIfFullLocked();
        lru_.push_back(sessionId);
        it = sessions_.emplace(sessionId, SessionState{}).first;
        it->second.lruIt = std::prev(lru_.end());
      } else {
        touchLocked(it);
      }
      it->second.userTurnCount = userTurnCount;
      it->second.tier = tier;
    }

  private:
    struct SessionState {
      size_t userTurnCount = 0;
      classifier::Tier tier = classifier::Tier::Unknown;
      std::list<std::string>::iterator lruIt;
    };

    void touchLocked(std::unordered_map<std::string, SessionState>::iterator it) {
      lru_.splice(lru_.end(), lru_, it->second.lruIt); // move to MRU end
    }

    void evictIfFullLocked() {
      if (sessions_.size() < kMaxSessions) return;
      sessions_.erase(lru_.front());
      lru_.pop_front();
    }

    std::mutex mtx_;
    std::unordered_map<std::string, SessionState> sessions_;
    std::list<std::string> lru_; // front = LRU, back = MRU
  };

  SessionTierCache g_sessionTiers;

} // anonymous namespace

namespace model_router {

  ApiConfig getTargetApi(const json &request, const App &app)
  {
    ApiConfig apiConfig = app.settings().generationCurrentApi();
    if (request.contains("targetapi") && request["targetapi"].is_string()) {
      std::string targetApi = request["targetapi"].get<std::string>();
      if (targetApi == kAutoApiId) {
        LOG_MSG_WARN << "Auto should not be used in getTargetApi, using current API instead.";
        apiConfig = app.settings().generationCurrentApi();
      }
      if (targetApi != apiConfig.id) {
        auto apis = app.settings().providers().generationProviders();
        auto it = std::find_if(apis.begin(), apis.end(), [&targetApi](const ApiConfig &a) { return a.id == targetApi; });
        if (it != apis.end()) apiConfig = *it;
      }
    }
    return apiConfig;
  }

  ApiConfig selectApi(const json &request, const json &messagesJson, const std::string &question, const App &app, std::vector<std::string> &pendingInfoVec)
  {
    std::string targetApi;
    if (request.contains("targetapi") && request["targetapi"].is_string()) {
      targetApi = request["targetapi"].get<std::string>();
    }
    bool runAutoRouter = false;
    if (targetApi == kAutoApiId) {
      // Explicit request-level override to use auto-routing
      runAutoRouter = true;
    } else if (!targetApi.empty()) {
      // Explicit request-level pin to a concrete model (e.g. "anthropic-sonnet-5")
      // Bypasses auto-router even if auto_router.enabled == true in settings
      runAutoRouter = false;
    } else {
      // No request-level preference given; fall back to the project default
      runAutoRouter = app.settings().generationIsAuto();
    }
    ApiConfig apiConfig;
    if (runAutoRouter) {
      const AutoRouterConfig router = app.settings().autoRouterConfig();
      std::string modelId = router.fallbackModelId;
      if (router.classifier.isInternal()) {
        if (auto *clf = getInternalClassifier()) {
          const std::string sessionId = request.value("session_id", std::string{});
          const size_t userTurns = countUserTurns(messagesJson);
          classifier::Tier sessionCtx = g_sessionTiers.lookupPrefix(sessionId, userTurns > 0 ? userTurns - 1 : 0);
          classifier::ClassificationResult res = clf->classify(question, sessionCtx);
          g_sessionTiers.rememberFull(sessionId, userTurns, res.predicted_tier);
          std::string tierName{ "tier_1_simple" };
          switch (res.predicted_tier) {
          case classifier::Tier::Tier2Medium:
            tierName = "tier_2_medium";
            break;
          case classifier::Tier::Tier3Complex:
            tierName = "tier_3_complex";
            break;
          default:
            break;
          }
          modelId = router.resolveRoutedModelId(tierName);
          LOG_MSG << "Auto-router classifier [internal] picked model" << modelId << "(" << AutoRouterConfig::normalizeTierTag(tierName) << ")";
          pendingInfoVec.push_back("Internal router classified task as " + tierName);
        } else {
          LOG_MSG << "Auto-router classifier [internal] unavailable, fallback=" << modelId;
        }
      } else {
        LOG_MSG << "Auto-router enabled, routing to appropriate model. Classifier id is" << router.classifier.apiId;
        try {
          const ApiConfig *clfCfg = app.settings().providers().findGeneration(router.classifier.apiId);
          if (!clfCfg) {
            LOG_MSG << "Classifier API not found:" << router.classifier.apiId;
            throw std::invalid_argument("Classifier API not found: " + router.classifier.apiId);
          }
          InferenceClient clf(*clfCfg, router.classifier.timeoutMs);
          const nlohmann::json clfMsgs = nlohmann::json::array({
            { { "role", "system" },{ "content", router.classifier.prompt } },
            { { "role", "user" },{ "content", question } }
            });
          const std::string raw = clf.generateChat(clfMsgs, router.classifier.temperature, router.classifier.maxTokens);
          modelId = router.resolveRoutedModelId(raw);
          LOG_MSG << "Auto-router classifier picked model" << modelId << "(" << AutoRouterConfig::normalizeTierTag(raw) << ")";
          pendingInfoVec.push_back("LLM router classified task as " + AutoRouterConfig::normalizeTierTag(raw));
        } catch (const std::exception &e) {
          LOG_MSG_WARN << "Classifier failed (" << e.what() << "), fallback=" << modelId << "\n";
        }
      }
      const ApiConfig *routed = app.settings().providers().findGeneration(modelId);
      if (!routed) {
        LOG_MSG << "Auto-router resolved unknown id '" << modelId << "'";
        throw std::runtime_error("auto-router resolved unknown id '" + modelId + "'");
      }
      pendingInfoVec.push_back("Auto routed to model " + modelId);
      apiConfig = *routed;
    } else {
      apiConfig = getTargetApi(request, app);
    }
    return apiConfig;
  }

} // namespace model_router
