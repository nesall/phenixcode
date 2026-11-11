#include "settings.h"
#include "cutils.h"
#include <utils_log/logger.hpp>
#include <fstream>
#include <stdexcept>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace {

  ApiConfig::ApiStyle detectApiStyle(std::string_view url, const nlohmann::json &item) {
    if (item.contains("api_style")) {
      std::string s = item.value("api_style", "");
      if (s == "anthropic") return ApiConfig::ApiStyle::Anthropic;
      if (s == "openai")    return ApiConfig::ApiStyle::OpenAI;
    }
    if (utils::strFindIn(url, "anthropic.com", false) != std::string::npos
      || utils::strFindIn(url, "/anthropic", false) != std::string::npos
      || utils::strFindIn(url, "/v1/messages", false) != std::string::npos
      ) {
      return ApiConfig::ApiStyle::Anthropic;
    }
    return ApiConfig::ApiStyle::OpenAI;
  }

  void fetchApiConfigFromItem(const nlohmann::json &item, ApiConfig &cfg, const nlohmann::json &section) {
    cfg.id = item.value("id", "");
    cfg.name = item.value("name", "");
    cfg.apiUrl = item.value("api_url", item.value("apiUrl", ""));
    cfg.apiKey = utils::expandEnvVar(item.value("api_key", item.value("apiKey", "")));
    cfg.model = item.value("model", "");
    cfg.maxTokensName = item.value("max_tokens_name", "");
    cfg.maxTokens = item.value("max_tokens", 0.f);
    if (cfg.maxTokensName.empty()) {
      cfg.maxTokensName = section.value("default_max_tokens_name", "max_tokens");
    }

    if (item.contains("fim") && item["fim"].is_object()) {
      const auto &fim = item["fim"];
      cfg.fim.apiUrl = fim.value("api_url", cfg.apiUrl);
      cfg.fim.prefixName = fim.value("prefix_name", "");
      cfg.fim.suffixName = fim.value("suffix_name", "");
      cfg.fim.format = fim.value("format", std::string{});
      cfg.fim.fileDivider = fim.value("file_divider_token", std::string{});
      if (fim.contains("stop_tokens") && fim["stop_tokens"].is_array()) {
        for (const auto &st : fim["stop_tokens"]) {
          if (st.is_string())
            cfg.fim.stopTokens.push_back(st.get<std::string>());
        }
      }
    }

    if (item.contains("reasoning") && item["reasoning"].is_object()) {
      const auto &rj = item["reasoning"];
      if (rj.contains("history_replay")) {
        const auto &replay = rj["history_replay"];
        if (!replay.is_number_integer() || replay < -1 || replay > ApiConfig::kMaxHistoryReplay)
          throw std::runtime_error("reasoning.history_replay in provider '" + cfg.id + "' must be -1 or an integer from 0 to 255");
        cfg.historyReplay = replay == -1 ? ApiConfig::kMaxHistoryReplay : replay.get<std::uint8_t>();
      }
      if (rj.contains("parameters") && rj["parameters"].is_array()) {
        for (const auto &p : rj["parameters"]) {
          if (!p.is_object()) continue;
          ReasoningParam rp;
          rp.name = p.value("name", "");
          rp.valueType = p.value("value_type", "scalar");
          rp.valueField = p.value("value_field", "");
          rp.defaultValue = p.value("default", ""); // or size_t index variant
          if (p.contains("values") && p["values"].is_array()) {
            for (const auto &v : p["values"])
              if (v.is_string()) rp.values.push_back(v.get<std::string>());
          }
          if (p.contains("constant_fields") && p["constant_fields"].is_object()) {
            for (const auto &[k, v] : p["constant_fields"].items())
              if (v.is_string()) rp.constantFields[k] = v.get<std::string>();
          }
          if (rp.defaultValue.empty() && !rp.values.empty())
            rp.defaultValue = rp.values.front();
          cfg.reasoning.push_back(std::move(rp));
        }
      }
    }
    
    cfg.documentFormat = item.value("document_format", "");
    cfg.queryFormat = item.value("query_format", "");
    cfg.temperatureSupport = item.value("temperature_support", false);
    cfg.temperature = item.value("temperature", 0.f);
    cfg.enabled = item.value("enabled", true);
    cfg.stream = item.value("stream", true);
    cfg.contextLength = item.value("context_length", section.value("default_context_length", 32000));
    if (item.contains("pricing_tpm")) {
      auto pricing = item["pricing_tpm"];
      if (pricing.is_object()) {
        cfg.pricing.input = pricing.value("input", 0.f);
        cfg.pricing.output = pricing.value("output", 0.f);
        if (pricing.contains("cached_input") && pricing["cached_input"].is_number()) {
          cfg.pricing.cachedInput = (std::max)(pricing.value("cached_input", 0.f), 0.f);
        } else {
          cfg.pricing.cachedInput = 0.f;
        }
      }
    }
    cfg.apiStyle = detectApiStyle(cfg.apiUrl, item);
  }

  std::vector<ApiConfig> getApiConfigList(const nlohmann::json &section, std::string_view key) {
    std::vector<ApiConfig> v;
    if (!section.contains(key) || !section[key].is_array()) return v;
    for (const auto &item : section[key]) {
      if (!item.is_object()) continue;
      ApiConfig cfg;
      fetchApiConfigFromItem(item, cfg, section);
      if (cfg.enabled)
        v.push_back(cfg);
    }
    return v;
  }

  ApiConfig getCurrentApiConfig(const nlohmann::json &section, const std::vector<ApiConfig> &apis, bool checkAllowed = true) {
    ApiConfig cfg;
    if (!section.is_object()) return cfg;
    std::string current = section.value("current_api", std::string{});
    if (checkAllowed) {
      std::vector<std::string> enabledApis = section.value("enabled_providers", nlohmann::json::array());
      bool allowed = std::find(enabledApis.cbegin(), enabledApis.cend(), current) != enabledApis.cend();
      if (!allowed) {
        throw std::runtime_error("current_api '" + current + "' is not in this project's enabled_providers");
      }
    }
    for (const auto &cfgItem : apis) {
      if (cfgItem.id == current) {
        cfg = cfgItem;
        break;
      }
    }
    return cfg;
  }

  std::string hashString(const std::string &str) {
    std::hash<std::string> hasher;
    std::stringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(16) << hasher(str);
    return ss.str();
  }

  std::string jsonStr(const nlohmann::json &j, const char *key, std::string def = {}) {
    if (!j.contains(key) || !j[key].is_string()) return def;
    return j[key].get<std::string>();
  }

  size_t jsonSize(const nlohmann::json &j, const char *key, size_t def) {
    if (!j.contains(key) || !j[key].is_number_unsigned()) return def;
    return j[key].get<size_t>();
  }

  float jsonFloat(const nlohmann::json &j, const char *key, float def) {
    if (!j.contains(key) || !j[key].is_number()) return def;
    return j[key].get<float>();
  }

  AutoRouterConfig parseAutoRouterConfig(const nlohmann::json &j) {
    AutoRouterConfig res;
    if (!j.is_object()) return res;
    res.enabled = j.value("enabled", false);
    if (j.contains("classifier") && j["classifier"].is_object()) {
      const auto &c = j["classifier"];
      res.classifier.type = jsonStr(c, "type");
      res.classifier.apiId = jsonStr(c, "api_id");
      res.classifier.timeoutMs = jsonSize(c, "timeout_ms", res.classifier.timeoutMs);
      res.classifier.maxTokens = jsonSize(c, "max_tokens", res.classifier.maxTokens);
      res.classifier.temperature = jsonFloat(c, "temperature", res.classifier.temperature);
      res.classifier.prompt = jsonStr(c, "prompt");
    }
    if (j.contains("routing_rules") && j["routing_rules"].is_object()) {
      for (auto it = j["routing_rules"].begin(); it != j["routing_rules"].end(); ++it) {
        if (!it.value().is_object()) continue;
        const auto &r = it.value();
        AutoRouterConfig::Rule rule;
        rule.strategy = jsonStr(r, "strategy");
        rule.directModelId = jsonStr(r, "direct_model_id");
        rule.synthesizerModelId = jsonStr(r, "synthesizer_model_id");
        if (r.contains("draft_model_ids") && r["draft_model_ids"].is_array()) {
          for (const auto &id : r["draft_model_ids"]) {
            if (id.is_string()) rule.draftModelIds.push_back(id.get<std::string>());
          }
        }
        res.rules.emplace(it.key(), std::move(rule));
      }
    }
    if (j.contains("fallback") && j["fallback"].is_object())
      res.fallbackModelId = jsonStr(j["fallback"], "default_model_id");
    return res;
  }

} // anonymous namespace


//----------------------------------------------------------------------------------------


std::pair<double, double> AutoRouterConfig::estimateCostRange(const Settings &s)
{
  double minCost = 0.0;
  double maxCost = 0.0;
  for (const auto &[tier, rule] : rules) {
    const std::string modelId = resolveRoutedModelId(tier);
    if (modelId.empty()) continue;
    const ApiConfig *cfg = s.providers().findGeneration(modelId);
    if (!cfg) continue;
    double cost = cfg->combinedPrice();
    if (minCost == 0.0 || cost < minCost) minCost = cost;
    if (cost > maxCost) maxCost = cost;
  }
  const ApiConfig *clfCfg = s.providers().findGeneration(classifier.apiId);
  if (clfCfg) {
    double clfCost = clfCfg->combinedPrice();
    minCost += clfCost;
    maxCost += clfCost;
  }
  return { minCost, maxCost };
}


//----------------------------------------------------------------------------------------


ProvidersSettings::ProvidersSettings(const std::string &path)
{
  if (path.length())
    loadFromFile(path);
}

void ProvidersSettings::loadFromFile(const std::string &path)
{
  std::ifstream file(path);
  if (!file.is_open())
    throw std::runtime_error("Cannot open providers file: " + path);
  nlohmann::json j;
  file >> j;
  updateFromConfig(j);
  if (embeddingProviders_.empty())
    LOG_MSG_WARN << "No embedding_providers defined in " << path;
  if (generationProviders_.empty())
    LOG_MSG_WARN << "No generation_providers defined in " << path;
  path_ = path;
}

void ProvidersSettings::updateFromConfig(const nlohmann::json &config)
{
  if (!config.is_object()) throw std::runtime_error("Invalid providers config: not a JSON object");
  if (!config.contains("embedding_providers")) throw std::runtime_error("Invalid providers config: missing embedding_providers");
  if (!config.contains("generation_providers")) throw std::runtime_error("Invalid providers config: missing generation_providers");
  embeddingProviders_ = getApiConfigList(config, "embedding_providers");
  generationProviders_ = getApiConfigList(config, "generation_providers");
  config_ = config;
}

const ApiConfig *ProvidersSettings::findEmbedding(const std::string &id) const
{
  for (const auto &cfg : embeddingProviders_) {
    if (cfg.id == id) return &cfg;
  }
  return nullptr;
}

const ApiConfig *ProvidersSettings::findGeneration(const std::string &id) const
{
  for (const auto &cfg : generationProviders_) {
    if (cfg.id == id) return &cfg;
  }
  return nullptr;
}

void ProvidersSettings::save()
{
  std::ofstream file(path_);
  if (file.is_open()) {
    file << config_.dump(2);
  }
}


//----------------------------------------------------------------------------------------

Settings::ReconcileResult Settings::reconcileProject(const nlohmann::json &project, const ProvidersSettings &providers,
  const ProviderRenames &renames, ReconcileMode mode)
{
  if (!project.is_object()) throw std::invalid_argument("Project settings must be an object");
  ReconcileResult result;
  result.jsonData = project;
  auto &cfg = result.jsonData;
  for (const auto *section : {"embedding", "generation"}) {
    if (!cfg.contains(section) || cfg[section].is_null()) cfg[section] = nlohmann::json::object();
    if (!cfg[section].is_object()) throw std::invalid_argument(std::string(section) + " must be an object");
  }
  const bool creating = mode == ReconcileMode::NewProject;
  std::vector<std::string> embeddingIds, generationIds;
  for (const auto &provider : providers.embeddingProviders())
    if (!provider.id.empty() && std::find(embeddingIds.begin(), embeddingIds.end(), provider.id) == embeddingIds.end())
      embeddingIds.push_back(provider.id);
  for (const auto &provider : providers.generationProviders())
    if (!provider.id.empty() && provider.id != kAutoApiId &&
        std::find(generationIds.begin(), generationIds.end(), provider.id) == generationIds.end())
      generationIds.push_back(provider.id);

  auto rename = [](const std::string &id, const std::map<std::string, std::string> &mapping) {
    const auto found = mapping.find(id);
    return found == mapping.end() ? id : found->second;
  };
  auto &embedding = cfg["embedding"];
  const auto previousEmbedding = rename(embedding.value("current_api", std::string{}), renames.embedding);
  std::string embeddingId = previousEmbedding;
  if (std::find(embeddingIds.begin(), embeddingIds.end(), embeddingId) == embeddingIds.end())
    embeddingId = creating && !embeddingIds.empty() ? embeddingIds.front() : std::string{};
  embedding["current_api"] = embeddingId;
  if (embeddingId.empty()) result.needsSelection.push_back("embedding.current_api");
  result.needsReindex = !creating && previousEmbedding != embeddingId;

  auto &generation = cfg["generation"];
  const auto oldEnabled = generation.value("enabled_providers", std::vector<std::string>{});
  std::vector<std::string> enabled;
  for (const auto &oldId : oldEnabled) {
    const auto id = rename(oldId, renames.generation);
    if (std::find(generationIds.begin(), generationIds.end(), id) == generationIds.end()) {
      result.dropped.push_back(id);
    } else if (std::find(enabled.begin(), enabled.end(), id) == enabled.end()) {
      enabled.push_back(id);
    }
  }
  if (creating && enabled.empty() && !generationIds.empty()) enabled.push_back(generationIds.front());
  generation["enabled_providers"] = enabled;
  auto current = rename(generation.value("current_api", std::string{}), renames.generation);
  if (std::find(enabled.begin(), enabled.end(), current) == enabled.end())
    current = enabled.empty() ? std::string{} : enabled.front();
  generation["current_api"] = current;
  if (enabled.empty()) result.needsSelection.push_back("generation.enabled_providers");

  auto fixGenerationId = [&](nlohmann::json &object, const char *field) {
    const auto id = rename(object.value(field, std::string{}), renames.generation);
    object[field] = std::find(enabled.begin(), enabled.end(), id) == enabled.end() ? current : id;
  };
  const auto router = generation.find("auto_router");
  if (router != generation.end() && !router->is_null()) {
    if (!router->is_object()) throw std::invalid_argument("generation.auto_router must be an object");
    if (router->contains("fallback")) fixGenerationId(router->at("fallback"), "default_model_id");
    if (router->contains("classifier")) {
      auto &classifier = router->at("classifier");
      if (classifier.value("type", std::string{}) != "internal") fixGenerationId(classifier, "api_id");
    }
    if (router->contains("routing_rules")) {
      if (!router->at("routing_rules").is_object()) throw std::invalid_argument("auto_router.routing_rules must be an object");
      for (auto &[tag, rule] : router->at("routing_rules").items()) {
        if (!rule.is_object()) throw std::invalid_argument("Auto-router rules must be objects");
        if (rule.contains("direct_model_id")) fixGenerationId(rule, "direct_model_id");
        if (rule.contains("synthesizer_model_id")) fixGenerationId(rule, "synthesizer_model_id");
        if (rule.contains("draft_model_ids")) {
          std::vector<std::string> drafts;
          for (const auto &oldId : rule.at("draft_model_ids").get<std::vector<std::string>>()) {
            const auto id = rename(oldId, renames.generation);
            if (std::find(enabled.begin(), enabled.end(), id) != enabled.end() &&
                std::find(drafts.begin(), drafts.end(), id) == drafts.end()) drafts.push_back(id);
          }
          rule["draft_model_ids"] = drafts;
        }
        if (rule.value("strategy", std::string{}) == "ensemble" && rule.value("draft_model_ids", std::vector<std::string>{}).empty()) {
          rule["strategy"] = "direct";
          rule["direct_model_id"] = current;
        }
      }
    }
    if (enabled.empty() && router->value("enabled", false)) (*router)["enabled"] = false;
  }
  result.changed = result.jsonData != project;
  return result;
}

Settings Settings::createBlank(const ProvidersSettings &providers)
{
  const auto defaults = nlohmann::json::parse(R"defaults({
  "_comment": "PhenixCode RAG minimal configuration",
  "_version": "1.0.0",
  "_docs": "https://github.com/nesall/embedder_cpp/wiki/configuration",
  "tokenizer": {
    "config_path": "./bge_tokenizer.json"
  },
  "embedding": {
    "current_api": "",
    "batch_size": 4,
    "timeout_ms": 30000,
    "retry_attempts": 3,
    "top_k": 5,
    "prepend_label_format": "[Source: {}]\n"
  },
  "generation": {
    "auto_router": {
      "classifier": {
        "max_tokens": 20,
        "prompt": "Classify the user programming task into exactly one tier:\n[TIER_1_SIMPLE]: quick syntax, single function, lookup, explanation\n[TIER_2_MEDIUM]: multi-file changes, bug fixing, medium edits\n[TIER_3_COMPLEX]: deep architectural reasoning, tricky algorithms, math/threading\nAnswer ONLY with the tag.",
        "temperature": 0,
        "timeout_ms": 3500,
        "type": "internal"
      },
      "enabled": true,
      "fallback": {
        "_comment": "used on classifier timeout, transport error, or unparseable tag",
        "default_model_id": ""
      },
      "routing_rules": {
        "tier_1_simple": {
          "direct_model_id": "",
          "strategy": "direct"
        },
        "tier_2_medium": {
          "direct_model_id": "",
          "strategy": "direct"
        },
        "tier_3_complex": {
          "direct_model_id": "",
          "strategy": "direct"
        }
      }
    },
    "enabled_providers": [],
    "current_api": "",
    "timeout_ms": 120000,
    "max_chunks": 7,
    "max_full_sources": 2,
    "max_related_per_source": 3,
    "default_context_length": 64000,
    "default_temperature": 0.1,
    "default_max_tokens": 2048,
    "default_max_tokens_name": "max_tokens",
    "prepend_label_format": "[Source: {}]\n",
    "excerpt": {
      "enabled": true,
      "min_chunks": 3,
      "max_chunks": 15,
      "threshold_ratio": 0.75
    }
  },
  "database": {
    "sqlite_path": "./db_metadata.db",
    "index_path": "./db_embeddings.index",
    "vector_dim": 768,
    "max_elements": 100000,
    "distance_metric": "cosine",
    "_comment": "For distance_metric use either cosine (default) or l2"
  },
  "chunking": {
    "semantic": true,
    "nof_min_tokens": 50,
    "nof_max_tokens": 450,
    "overlap_percentage": 0.2
  },
  "source": {
    "_comment_project_id": "Leave empty to auto-generate from config path, or set a custom stable project_id",
    "project_id": "",
    "project_title": "Default Project Title",
    "project_description": "",
    "paths": [
      {
        "exclude": [],
        "extensions": [],
        "path": "./",
        "recursive": true,
        "type": "directory"
      }
    ],
    "default_extensions": [
      ".c",
      ".cpp",
      ".h",
      ".hpp",
      ".py",
      ".js",
      ".ts",
      ".java",
      ".rs",
      ".cs",
      ".xaml",
      ".php",
      ".jsp",
      ".html",
      ".css",
      ".md"
    ],
    "global_exclude": [
      "*/node_modules/*",
      "*.min.js",
      "*.log",
      "*/build/*",
      "*/.git/*",
      ".gitignore",
      "*/CVS/*",
      "*/debug/*",
      "*/release/*",
      "*/lib/*",
      "*/docker/*",
      "*/fonts/*",
      "*/images/*",
      "*/test/*",
      "*/tests/*",
      "*/example/*",
      "*/examples/*",
      "*/obj/*",
      "*/build_dbg/*",
      "*/build_rel/*",
      "*/bin/*",
      "*/dist/*",
      "*/test/*",
      "*/3rdparty/*",
      "*/cmake-build/*",
      "*/assets/*",
      "*/.vscode/*",
      "*/.vs/*",
      "*/build_check/*"
    ],
    "encoding": "utf-8",
    "max_file_size_mb": 10
  },
  "logging": {
    "log_to_console": true,
    "log_to_file": true,
    "logging_file": "embedder.log",
    "diagnostics_file": "embedder_d.log"
  }
})defaults");
  const auto result = reconcileProject(defaults, providers, {}, ReconcileMode::NewProject);
  if (!result.needsSelection.empty())
    throw std::runtime_error("Cannot create project. At least one enabled embedding and generation provider is required");
  Settings settings(result.jsonData, providers.configJson());
  settings.providers_ = providers;
  return settings;
}

Settings::Settings(const nlohmann::json &prj, const nlohmann::json &prv)
{
  updateFromConfig(prj);
  providers_.updateFromConfig(prv);
  validate();
}

Settings::Settings(const nlohmann::json &prj, const std::string &providersPath)
{
  updateFromConfig(prj);
  providers_.loadFromFile(providersPath);
  validate();
}

Settings::Settings(const std::string &path, const std::string &providersPath)
{
  updateFromPath(path);
  providers_.loadFromFile(providersPath);
  validate();
}

Settings::Settings(const std::string &path)
{
  updateFromPath(path);
}

void Settings::updateFromConfig(const nlohmann::json &config)
{
  if (config.is_object()) {
    config_.merge_patch(config);
  }
}

void Settings::updateFromPath(const std::string &path)
{
  std::ifstream file(path);
  if (!file.is_open()) {
    throw std::runtime_error("Cannot open settings file: " + path);
  }
  file >> config_;
  path_ = path;
}

void Settings::save()
{
  if (path_.empty()) throw std::runtime_error("Cannot save settings: path is empty");
  saveToPath(path_);
}

void Settings::saveToPath(std::string_view path)
{
  std::ofstream file(path.data());
  if (!file.is_open()) {
    throw std::runtime_error("Cannot open settings file for writing: " + std::string(path));
  }
  file << config_.dump(2);
  file.flush();
  if (!file) throw std::runtime_error("Cannot write settings file: " + std::string(path));
}

void Settings::validateProjectJson(nlohmann::json j)
{
  if (!j.is_object()) throw std::runtime_error("Settings root is not a JSON object");
  static const char *requiredSections[] = { "source", "chunking", "embedding", "generation", "database" };
  for (const char *section : requiredSections) {
    if (!j.contains(section) || !j[section].is_object()) {
      throw std::runtime_error(std::string("Missing or invalid settings section: ") + section);
    }
  }
}

void Settings::validate()
{
  validateProjectJson(config_);

  // Validate a provider section: current_api must be listed in enabled_providers
  // and must exist in the providers list loaded from the providers file.
  auto validateProviderSection = [&](const char *sectionName, const std::vector<ApiConfig> &providers, bool checkEnabledPrv = true) {
    const auto &section = config_[sectionName];
    const std::string currentApi = section.value("current_api", std::string{});
    if (currentApi.empty()) {
      throw std::runtime_error(std::string("Missing 'current_api' in settings section: ") + sectionName);
    }
    if (checkEnabledPrv) {
      std::vector<std::string> enabledApis = section.value("enabled_providers", nlohmann::json::array());
      if (std::find(enabledApis.cbegin(), enabledApis.cend(), currentApi) == enabledApis.cend()) {
        LOG_MSG_WARN << "current_api '" << currentApi << "' is not in this project's enabled_providers";
        throw std::runtime_error("current_api '" + currentApi + "' is not in this project's enabled_providers");
      }
    }
    //const bool isAuto = generationIsAuto();
    //if (isAuto) {
    //  if (std::strcmp(sectionName, "generation") != 0)
    //    throw std::runtime_error("'auto' is only valid as generation.current_api");
    //  if (!config_["generation"].contains("auto_router") || !config_["generation"]["auto_router"].is_object())
    //    throw std::runtime_error("'auto' requires generation.auto_router");
    //  return; // skip catalog lookup
    //}

    bool found = false;
    for (const auto &cfg : providers) {
      if (cfg.id == currentApi) {
        found = true;
        break;
      }
    }
    if (!found) {
      throw std::runtime_error("current_api '" + currentApi + "' not found in providers list for section '" + sectionName + "'");
    }
    };

  validateProviderSection("embedding", providers_.embeddingProviders(), false);
  validateProviderSection("generation", providers_.generationProviders());

  for (const auto &cfg : providers_.generationProviders()) {
    for (const auto &p : cfg.reasoning) {
      if (p.name.empty()) throw std::runtime_error("reasoning parameter in provider '" + cfg.id + "' has empty 'name'");
      if (p.values.empty()) throw std::runtime_error("reasoning parameter '" + p.name + "' in provider '" + cfg.id + "' has empty 'values'");
      if (std::find(p.values.begin(), p.values.end(), p.defaultValue) == p.values.end())
        throw std::runtime_error("reasoning parameter '" + p.name + "' default not in values for provider '" + cfg.id + "'");
      if (p.isObject() && p.valueField.empty())
        throw std::runtime_error("reasoning parameter '" + p.name + "' is object but 'value_field' is empty in provider '" + cfg.id + "'");
    }
  }

  if (generationIsAuto()) {
    const AutoRouterConfig router = autoRouterConfig();
    const auto enabled = enabledGenerationProviders();
    const auto &catalog = providers().generationProviders();

    auto requireRealGenId = [&](const std::string &id, const std::string &what) {
      if (id.empty())
        throw std::runtime_error("auto_router: " + what + " is empty");
      if (id == kAutoApiId)
        throw std::runtime_error("auto_router: " + what + " cannot be 'auto'");
      if (std::find(enabled.begin(), enabled.end(), id) == enabled.end())
        throw std::runtime_error("auto_router: " + what + " '" + id + "' is not in generation.enabled_providers");
      const bool inCatalog = std::any_of(catalog.cbegin(), catalog.cend(), [&](const ApiConfig &a) { return a.id == id; });
      if (!inCatalog)
        throw std::runtime_error("auto_router: " + what + " '" + id + "' is not in generation_providers");
      };

    if (router.classifier.prompt.empty())
      throw std::runtime_error("auto_router: classifier.prompt is empty");
    if (!router.classifier.isInternal())
      requireRealGenId(router.classifier.apiId, "classifier.api_id");

    if (router.fallbackModelId.empty())
      throw std::runtime_error("auto_router: fallback.default_model_id is empty");
    requireRealGenId(router.fallbackModelId, "fallback.default_model_id");

    if (router.rules.empty())
      throw std::runtime_error("auto_router: routing_rules is empty");

    for (const auto &[tag, rule] : router.rules) {
      if (tag.empty())
        throw std::runtime_error("auto_router: routing_rules contains an empty key");
      if (rule.strategy != "direct") {
        throw std::runtime_error("auto_router: routing_rules." + tag + " strategy must be 'direct' (ensemble is not supported yet)");
      }
      requireRealGenId(rule.directModelId, "routing_rules." + tag + ".direct_model_id");
    }
  }
}

ApiConfig Settings::embeddingCurrentApi() const
{
  if (!config_.contains("embedding")) return {};
  return getCurrentApiConfig(config_["embedding"], providers_.embeddingProviders(), false);
}

bool Settings::generationIsAuto() const
{
  return config_["generation"].contains("auto_router") && config_["generation"]["auto_router"].value("enabled", false);
}

ApiConfig Settings::generationCurrentApi() const
{
  if (!config_.contains("generation")) return {};
  return getCurrentApiConfig(config_["generation"],providers_.generationProviders());
}

void Settings::initProjectIdIfMissing(bool hydrateFile)
{
  std::string s;
  s = config_["source"].value("project_id", "");
  if (s.empty()) {
    // Auto-generate
    auto absPath = std::filesystem::absolute(configPath()).lexically_normal();
    std::string dirName = absPath.parent_path().filename().string();
    std::string pathHash = hashString(absPath.generic_string()).substr(0, 8);
    s = dirName + "-" + pathHash;
    config_["source"]["project_id"] = s;
    if (hydrateFile) this->save();
  }
}

void Settings::initProjectTitleIfMissing(bool hydrateFile)
{
  auto s = config_["source"].value("project_title", "");
  if (s.empty()) {
    auto sources = this->sources();
    for (const auto &si : sources) {
      if (!s.empty()) s += "/";
      s += std::filesystem::path(si.path).lexically_normal().stem().string();
      if (12 < s.length()) break;
    }
    if (s.empty()) {
      s = "Unnamed Project";
    }
    config_["source"]["project_title"] = s;
    if (hydrateFile) this->save();
  }
}

std::vector<Settings::SourceItem> Settings::sources() const
{
  std::vector<SourceItem> res;
  const auto &source = config_["source"];
  for (const auto &item : source["paths"]) {
    SourceItem si;
    si.type = item["type"];
    if (si.type == "directory" || si.type == "file") {
      si.path = item["path"];
    }
    if (si.type == "directory") {
      si.recursive = item.value("recursive", true);
      si.extensions = item.value("extensions", std::vector<std::string>{});
      si.exclude = item.value("exclude", std::vector<std::string>{});
      auto f = filesDefaultExtensions();
      if (si.extensions.empty() && !f.empty()) {
        si.extensions = f;
      }
      auto x = filesGlobalExclusions();
      if (!x.empty()) {
        si.exclude.insert(si.exclude.end(), x.begin(), x.end());
      }
    }
    if (si.type == "url") {
      si.url = item["url"];
      if (item.contains("headers")) {
        for (const auto &[key, value] : item["headers"].items()) {
          si.headers[key] = utils::expandEnvVar(value);
        }
      }
      si.urlTimeoutMs = item.value("timeout_ms", 10000);
    }
    res.push_back(si);
  }
  return res;
}

AutoRouterConfig Settings::autoRouterConfig() const
{
  if (!config_.contains("generation") ||
    !config_["generation"].contains("auto_router") ||
    !config_["generation"]["auto_router"].is_object())
    return {};
  return parseAutoRouterConfig(config_["generation"]["auto_router"]);
}
