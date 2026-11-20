#include "chat_tools.h"
#include "settings.h"
#include "cutils.h"
#include <utils_log/logger.hpp>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <stdexcept>

using nlohmann::json;

namespace chat_tools {
  namespace detail {
    const json &projectSections() {
      static const json sections = json::array({"project", "models", "retrieval", "reasoning", "all"});
      return sections;
    }
  } // namespace detail

  namespace {
    json projectArguments(const json &call, const char *name, bool sections) {
      if (!call.contains("function") || !call["function"].is_object())
        throw std::invalid_argument("Tool call requires a function object");
      const auto &function = call["function"];
      if (!function.contains("name") || function["name"] != name) throw std::invalid_argument("Unknown tool");
      if (!function.contains("arguments") || !function["arguments"].is_string())
        throw std::invalid_argument("Tool arguments must be a JSON string");
      const auto &raw = function["arguments"].get_ref<const std::string &>();
      if (raw.size() > 4096) throw std::invalid_argument("Tool arguments exceed the size limit");
      const auto args = json::parse(raw, nullptr, false);
      if (args.is_discarded()) throw std::invalid_argument("Malformed JSON in tool arguments");
      if (!args.is_object()) throw std::invalid_argument("Tool arguments must be an object");
      for (const auto &[key, value] : args.items())
        if (!sections || key != "section") throw std::invalid_argument("Unknown argument: " + detail::preview(key, 128));
      if (sections && args.contains("section")) {
        if (!args["section"].is_string()) throw std::invalid_argument("section must be a string");
        if (std::find(detail::projectSections().begin(), detail::projectSections().end(), args["section"]) == detail::projectSections().end())
          throw std::invalid_argument("Unknown section; use project, models, retrieval, reasoning or all");
      }
      return args;
    }

    // Bound serialized output, including escaping, without returning partial JSON.
    json boundedProjectResult(json result) {
      result["truncated"] = false;
      result["truncated_fields"] = json::array();
      while (result.dump().size() > kMaxResultBytes) {
        json *largest = nullptr;
        size_t largestSize = 0;
        std::string largestPath;
        const auto visit = [&](const auto &self, json &value, const std::string &path) -> void {
          if (value.is_array() && !value.empty()) {
            const auto size = value.dump().size();
            if (size > largestSize) { largest = &value; largestSize = size; largestPath = path; }
          } else if (value.is_string() && value.get_ref<const std::string &>().size() > 128) {
            const auto size = value.dump().size();
            if (size > largestSize) { largest = &value; largestSize = size; largestPath = path; }
          } else if (value.is_object()) {
            for (auto &[key, child] : value.items()) self(self, child, detail::preview(path.empty() ? key : path + "." + key, 128));
          }
        };
        for (auto &[key, value] : result.items())
          if (key != "truncated" && key != "truncated_fields") visit(visit, value, key);
        if (!largest) return {{"error", "Project summary exceeds the result limit"}};
        if (largest->is_array()) largest->erase(largest->end() - 1);
        else *largest = detail::preview(utils::sanitizeUtf8(largest->get<std::string>()), 128);
        result["truncated"] = true;
        auto &fields = result["truncated_fields"];
        if (std::find(fields.begin(), fields.end(), largestPath) == fields.end()) {
          if (fields.size() < 15) fields.push_back(largestPath);
          else if (fields.size() == 15) fields.push_back("*"); // Additional shortened fields.
        }
      }
      return result;
    }

    json projectSummary(const Settings &settings) {
      json paths = json::array();
      for (const auto &source : settings.sources()) {
        // URLs and headers may contain credentials. Do not expose either.
        json item = {{"type", source.type}};
        if (source.type == "directory" || source.type == "file") item["path"] = source.path;
        if (source.type == "directory") {
          item["recursive"] = source.recursive;
          item["extensions"] = source.extensions;
          item["exclude"] = source.exclude;
        }
        paths.push_back(std::move(item));
      }
      return {{"id", settings.getProjectId()}, {"title", settings.getProjectTitle()}, {"sources", paths},
        {"encoding", settings.filesEncoding()}, {"max_file_size_mb", settings.filesMaxFileSizeMb()}};
    }

    json providerSummary(const ApiConfig &api) {
      // Explicit allowlist: never serialize ApiConfig or catalog objects wholesale.
      return {{"id", api.id}, {"model", api.model},
        {"api_style", api.apiStyle == ApiConfig::ApiStyle::OpenAI ? "openai" : "anthropic"},
        {"context_length", api.contextLength}, {"max_tokens", api.maxTokens}, {"supports_tools", api.supportsTools},
        {"stream", api.stream}};
    }
  } // namespace

  json executeProjectInfo(const json &call, const Settings &settings, const ToolContext &ctx, size_t indexedChunks, const std::vector<std::string> &availableTools) {
    LOG_MSG_DEBUG << "executeProjectInfo: " << call.dump();
    LOG_MSG_INFO << "[tool] project_info called";
    try { projectArguments(call, "project_info", false); }
    catch (const std::invalid_argument &e) { return {{"error", detail::preview(e.what(), 512)}}; }
    try {
      std::set<std::string> extensions;
      size_t extensionless = 0;
      for (const auto &file : ctx.allowedFiles) {
        auto extension = std::filesystem::path(file).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension.empty()) ++extensionless;
        else extensions.insert(std::move(extension));
      }
      return boundedProjectResult({{"configuration", "loaded"}, {"project", projectSummary(settings)},
        {"local_source_count", ctx.allowedFiles.size()}, {"source_extensions", extensions}, {"extensionless_file_count", extensionless},
        {"index", {{"chunk_count", indexedChunks}, {"freshness", "unknown"}}},
        {"capabilities", {{"agent_enabled", settings.providers().agentEnabled()},
          {"available_tools", availableTools}}}});
    } catch (const std::exception &e) {
      // Validation errors above are actionable; internal failures may contain
      // sensitive configuration values, so keep their details in backend logs.
      LOG_MSG_WARN << "project_info summary failed: " << e.what();
      return {{"error", "Unable to summarize loaded project; see backend logs"}};
    }
  }

  json executeProjectSettings(const json &call, const Settings &settings, const ApiConfig &selectedApi) {
    LOG_MSG_DEBUG << "executeProjectSettings: " << call.dump();
    LOG_MSG_INFO << "[tool] project_settings called";
    json args;
    try { args = projectArguments(call, "project_settings", true); }
    catch (const std::invalid_argument &e) { return {{"error", detail::preview(e.what(), 512)}}; }
    try {
      const auto section = args.value("section", std::string("all"));
      json result = {{"configuration", "loaded"}, {"section", section}};
      if (section == "all" || section == "project") result["project"] = projectSummary(settings);
      if (section == "all" || section == "models") {
        json providers = json::array();
        // Only generation providers enabled for this project belong in this summary.
        const auto enabled = settings.enabledGenerationProviders();
        for (const auto &api : settings.providers().generationProviders())
          if (std::find(enabled.begin(), enabled.end(), api.id) != enabled.end())
            providers.push_back(providerSummary(api));
        const auto router = settings.autoRouterConfig();
        json rules = json::object();
        for (const auto &[tier, rule] : router.rules)
          rules[tier] = {{"strategy", rule.strategy}, {"direct_model_id", rule.directModelId},
            {"draft_model_ids", rule.draftModelIds}, {"synthesizer_model_id", rule.synthesizerModelId}};
        const auto embedding = settings.embeddingCurrentApi();
        result["models"] = {{"configured_generation_id", settings.generationCurrentApiId()},
          {"selected_generation", providerSummary(selectedApi)}, {"embedding", embedding.id.empty() ? json(nullptr) : providerSummary(embedding)},
          {"enabled_generation", providers}, {"agent_enabled", settings.providers().agentEnabled()},
          {"default_max_tokens", settings.generationDefaultMaxTokens()},
          {"default_temperature", settings.generationDefaultTemperature()},
          {"auto_router", {{"enabled", router.enabled}, {"classifier_type", router.classifier.type},
            {"fallback_model_id", router.fallbackModelId}, {"rules", rules}}}};
      }
      if (section == "all" || section == "retrieval") result["retrieval"] = {
        {"chunking", {{"min_tokens", settings.chunkingMinTokens()}, {"max_tokens", settings.chunkingMaxTokens()},
          {"overlap", settings.chunkingOverlap()}, {"semantic", settings.chunkingSemantic()}}},
        {"embedding_top_k", settings.embeddingTopK()}, {"embedding_batch_size", settings.embeddingBatchSize()},
        {"max_chunks", settings.generationMaxChunks()}, {"max_full_sources", settings.generationMaxFullSources()},
        {"max_related_per_source", settings.generationMaxRelatedPerSource()},
        {"database", {{"sqlite_filename", std::filesystem::path(settings.databaseSqlitePath()).filename().string()}, {"index_filename", std::filesystem::path(settings.databaseIndexPath()).filename().string()},
          {"vector_dim", settings.databaseVectorDim()}, {"distance_metric", settings.databaseDistanceMetric()}}}};
      if (section == "all" || section == "reasoning") {
        json parameters = json::array();
        for (const auto &param : selectedApi.reasoning) parameters.push_back({{"name", param.name},
          {"value_type", param.valueType}, {"value_field", param.valueField}, {"values", param.values}, {"default", param.defaultValue}});
        result["reasoning"] = {{"provider_id", selectedApi.id}, {"parameters", parameters},
          {"history_replay", selectedApi.historyReplay}, {"replay_storage", "backend_session_memory"},
          {"parameter_values", "configured_defaults_not_request_selections"}};
      }
      return boundedProjectResult(std::move(result));
    } catch (const std::exception &e) {
      // Do not forward internal configuration exception text to the provider.
      LOG_MSG_WARN << "project_settings summary failed: " << e.what();
      return {{"error", "Unable to summarize loaded settings; see backend logs"}};
    }
  }
} // namespace chat_tools
