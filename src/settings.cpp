#include "settings.h"
#include <fstream>
#include <stdexcept>
#include <filesystem>
#include <sstream>
#include <cstdlib>

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

  void fetchApiConfigFromItem(const nlohmann::json &item, ApiConfig &cfg, const nlohmann::json &section) {
    cfg.id = item.value("id", "");
    cfg.name = item.value("name", "");
    cfg.apiUrl = item.value("api_url", item.value("apiUrl", ""));
    cfg.apiKey = expandEnvVar(item.value("api_key", item.value("apiKey", "")));
    cfg.model = item.value("model", "");
    cfg.maxTokensName = item.value("max_tokens_name", section.value("default_max_tokens_name", "max_tokens"));

    if (item.contains("fim") && item["fim"].is_object()) {
      auto fim = item["fim"];
      cfg.fim.apiUrl = fim.value("api_url", cfg.apiUrl);
      cfg.fim.prefixName = fim.value("prefix_name", "");
      cfg.fim.suffixName = fim.value("suffix_name", "");
      if (fim.contains("stop_tokens") && fim["stop_tokens"].is_array()) {
        for (const auto &st : fim["stop_tokens"]) {
          if (st.is_string())
            cfg.fim.stopTokens.push_back(st.get<std::string>());
        }
      }
    }
    
    cfg.documentFormat = item.value("document_format", "");
    cfg.queryFormat = item.value("query_format", "");
    cfg.temperatureSupport = item.value("temperature_support", true);
    cfg.enabled = item.value("enabled", true);
    cfg.stream = item.value("stream", true);
    cfg.contextLength = item.value("context_length", section.value("max_context_tokens", 32000));
    if (item.contains("pricing_tpm")) {
      auto pricing = item["pricing_tpm"];
      if (pricing.is_object()) {
        cfg.pricing.input = pricing.value("input", 0.f);
        cfg.pricing.output = pricing.value("output", 0.f);
        cfg.pricing.cachedInput = pricing.value("cached_input", 0.f);
      }
    }
  }

  std::vector<ApiConfig> getApiConfigList(const nlohmann::json &section) {
    std::vector<ApiConfig> v;
    if (!section.contains("apis") || !section["apis"].is_array()) return v;
    for (const auto &item : section["apis"]) {
      if (!item.is_object()) continue;
      ApiConfig cfg;
      fetchApiConfigFromItem(item, cfg, section);
      if (cfg.enabled)
        v.push_back(cfg);
    }
    return v;
  }

  ApiConfig getCurrentApiConfig(const nlohmann::json &section) {
    ApiConfig cfg;
    if (!section.is_object()) return cfg;
    std::string current = section.value("current_api", "");
    if (!section.contains("apis") || !section["apis"].is_array()) return cfg;
    for (const auto &item : section["apis"]) {
      if (!item.is_object()) continue;
      std::string id = item.value("id", "");
      if (current.empty() || id == current) {
        fetchApiConfigFromItem(item, cfg, section);
        return cfg;
      }
    }
    if (!section["apis"].empty()) {
      fetchApiConfigFromItem(section["apis"][0], cfg, section);
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

Settings::Settings(const std::string &path)
{
  updateFromPath(path);
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

ApiConfig Settings::embeddingCurrentApi() const
{
  if (!config_.contains("embedding")) return {};
  return getCurrentApiConfig(config_["embedding"]);
}

std::vector<ApiConfig> Settings::embeddingApis() const
{
  if (!config_.contains("embedding")) return {};
  return getApiConfigList(config_["embedding"]);
}

ApiConfig Settings::generationCurrentApi() const
{
  if (!config_.contains("generation")) return {};
  return getCurrentApiConfig(config_["generation"]);
}

std::vector<ApiConfig> Settings::generationApis() const
{
  if (!config_.contains("generation")) return {};
  return getApiConfigList(config_["generation"]);
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
