#include "settings.h"
#include "cutils.h"
#include <utils_log/logger.hpp>
#include <fstream>
#include <stdexcept>
#include <filesystem>
#include <sstream>
#include <cstdlib>
#include <iomanip>

namespace {
  // Simple ${VAR} substitution
  std::string expandEnvVar(const std::string var) {
    if (var.starts_with("${") && var.ends_with("}")) {
      std::string envVar = var.substr(2, var.length() - 3);
      if (const char *envValue = getenv(envVar.c_str())) {
        return std::string(envValue);
      }
    }
    return var;
  }

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
    cfg.apiKey = expandEnvVar(item.value("api_key", item.value("apiKey", "")));
    cfg.model = item.value("model", "");
    cfg.maxTokensName = item.value("max_tokens_name", "");
    if (cfg.maxTokensName.empty()) {
      cfg.maxTokensName = section.value("default_max_tokens_name", "max_tokens");
    }

    if (item.contains("fim") && item["fim"].is_object()) {
      auto fim = item["fim"];
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
    
    cfg.documentFormat = item.value("document_format", "");
    cfg.queryFormat = item.value("query_format", "");
    cfg.temperatureSupport = item.value("temperature_support", false);
    cfg.enabled = item.value("enabled", true);
    cfg.stream = item.value("stream", true);
    cfg.contextLength = item.value("context_length", section.value("max_context_tokens", 32000));
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

  ApiConfig getCurrentApiConfig(const nlohmann::json &section, const std::vector<ApiConfig> &apis) {
    ApiConfig cfg;
    if (!section.is_object()) return cfg;
    std::string current = section.value("current_api", std::string{});
    std::vector<std::string> enabledApis = section.value("enabled_providers", nlohmann::json::array());
    bool allowed = std::find(enabledApis.cbegin(), enabledApis.cend(), current) != enabledApis.cend();
    if (!allowed) {
      throw std::runtime_error("current_api '" + current + "' is not in this project's enabled_providers");
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
} // anonymous namespace


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
    LOG_MSG << "Warning: no embedding_providers defined in " << path;
  if (generationProviders_.empty())
    LOG_MSG << "Warning: no generation_providers defined in " << path;
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


Settings::Settings(const std::string &path, const std::string &providersPath)
{
  updateFromPath(path);
  providers_.loadFromFile(providersPath);
  validate();
}

void Settings::updateFromConfig(const nlohmann::json &config)
{
  config_ = config; // Or merge specific fields
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
  std::ofstream file(path_);
  if (file.is_open()) {
    file << config_.dump(2);
  }
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
  auto validateProviderSection = [&](const char *sectionName,
    const std::vector<ApiConfig> &providers) {
      const auto &section = config_[sectionName];
      const std::string currentApi = section.value("current_api", std::string{});
      if (currentApi.empty()) {
        throw std::runtime_error(std::string("Missing 'current_api' in settings section: ") + sectionName);
      }

      std::vector<std::string> enabledApis =
        section.value("enabled_providers", nlohmann::json::array());
      if (std::find(enabledApis.cbegin(), enabledApis.cend(), currentApi) == enabledApis.cend()) {
        LOG_MSG << "Warning: current_api '" << currentApi << "' is not in this project's enabled_providers";
        throw std::runtime_error("current_api '" + currentApi + "' is not in this project's enabled_providers");
      }

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

  validateProviderSection("embedding", providers_.embeddingProviders());
  validateProviderSection("generation", providers_.generationProviders());
}

ApiConfig Settings::embeddingCurrentApi() const
{
  if (!config_.contains("embedding")) return {};
  return getCurrentApiConfig(config_["embedding"], providers_.embeddingProviders());
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
          std::string headerValue = value;
          // Simple ${VAR} substitution
          if (headerValue.starts_with("${") && headerValue.ends_with("}")) {
            std::string envVar = headerValue.substr(2, headerValue.length() - 3);
            const char *envValue = nullptr;
            envValue = getenv(envVar.c_str());
            if (envValue) {
              headerValue = std::string(envValue);
            }
          }
          si.headers[key] = headerValue;
        }
      }
      si.urlTimeoutMs = item.value("timeout_ms", 10000);
    }
    res.push_back(si);
  }
  return res;
}

