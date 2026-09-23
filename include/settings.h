#ifndef _SETTINGS_H_
#define _SETTINGS_H_

#define _CRT_SECURE_NO_WARNINGS

#include <string>
#include <vector>
#include <map>
#include "json_shim.h"

inline constexpr std::string_view kAutoApiId = "auto";

struct ApiConfig {
  std::string id;
  std::string name;
  std::string apiUrl;
  std::string apiKey;
  std::string model;
  std::string queryFormat;
  std::string documentFormat;
  std::string maxTokensName; // e.g. max_tokens or max_completion_tokens
  bool temperatureSupport = false;
  bool enabled = true;
  bool stream = true;
  size_t contextLength = 0;
  size_t maxTokens = 0;
  float temperature = 0;
  struct {
    float input = 0;
    float output = 0;
    float cachedInput = 0;
  } pricing;
  struct {
    std::string apiUrl;
    std::string prefixName; // non-empty means model supports FIM mode.
    std::string suffixName;
    std::string format; // either format or prefix/suffix. prefix/suffix takes precedance.
    std::string fileDivider; // Valid if using format. e.g. <|file_separator|> 
    std::vector<std::string> stopTokens;
  } fim;
  enum class ApiStyle { OpenAI, Anthropic };
  ApiStyle apiStyle = ApiStyle::OpenAI;

  // Compute an effective "combined" price per million tokens.
  // hitRatio = fraction of input tokens served from cache (0.0–1.0)
  double combinedPrice(double hitRatio = 0.05) const {
    // Only input can be cached; output is always fully billed
    double effectiveInput = pricing.input;
    if (0 < pricing.cachedInput) {
      effectiveInput = hitRatio * pricing.cachedInput + (1.0 - hitRatio) * pricing.input;
    }
    return effectiveInput + pricing.output;
  }

  double inputTokensPrice(size_t tokens, double hitRatio = 0.05) const {
    double effectiveInput = pricing.input;
    if (0 < pricing.cachedInput) {
      effectiveInput = hitRatio * pricing.cachedInput + (1.0 - hitRatio) * pricing.input;
    }
    return (tokens / 1'000'000.0) * effectiveInput;
  }

  double outputTokensPrice(size_t tokens) const {
    return (tokens / 1'000'000.0) * pricing.output;
  }
};


//----------------------------------------------------------------------------------------

class Settings;

struct AutoRouterConfig {

  bool enabled = false;

  struct Classifier {
    std::string type; // "internal" | "api", empty means "api"
    std::string apiId;
    size_t timeoutMs = 3500;
    size_t maxTokens = 20;
    float temperature = 0.f;
    std::string prompt;
    bool isInternal() const { return type == "internal"; }
  } classifier;

  struct Rule {
    std::string strategy; // "direct" | "ensemble"
    std::string directModelId;          // strategy == direct
    std::vector<std::string> draftModelIds;   // strategy == ensemble
    std::string synthesizerModelId;           // strategy == ensemble
  };
  std::map<std::string, Rule> rules; // key: normalized tier tag, e.g. "tier_1_simple"

  std::string fallbackModelId;

  std::string resolveRoutedModelId(const std::string &raw) const {
    const auto it = rules.find(normalizeTierTag(raw));
    if (it != rules.end() && it->second.strategy == "direct" && !it->second.directModelId.empty())
      return it->second.directModelId;
    return fallbackModelId;
  }
  static std::string normalizeTierTag(const std::string &raw) {
    std::string s;
    s.reserve(raw.size());
    for (unsigned char c : raw) if (std::isalnum(c) || c == '_') s += static_cast<char>(std::tolower(c));    
    return s;
  }
  std::pair<double, double> estimateCostRange(const Settings &s);
};


//----------------------------------------------------------------------------------------

class ProvidersSettings {
public:
  ProvidersSettings(const std::string &path = {});
  void loadFromFile(const std::string &path); // throws on missing/invalid
  void updateFromConfig(const nlohmann::json &config);

  std::string path() const { return path_; }
  nlohmann::json configJson() const { return config_; }

  const ApiConfig *findEmbedding(const std::string &id) const;
  const ApiConfig *findGeneration(const std::string &id) const;

  const std::vector<ApiConfig> &embeddingProviders() const { return embeddingProviders_; }
  const std::vector<ApiConfig> &generationProviders() const { return generationProviders_; }

  void save();

private:
  nlohmann::json config_;
  std::string path_;
  std::vector<ApiConfig> embeddingProviders_;
  std::vector<ApiConfig> generationProviders_;
};


//----------------------------------------------------------------------------------------


class Settings {
private:
  nlohmann::json config_;
  std::string path_;
  ProvidersSettings providers_;

public:
  struct SourceItem {
    std::string type; // "directory", "file", "url"
    std::string path; // for "directory" and "file"
    bool recursive = true; // for "directory"
    std::vector<std::string> extensions; // for "directory"
    std::vector<std::string> exclude; // for "directory"
    std::string url; // for "url"
    std::map<std::string, std::string> headers; // for "url"
    std::size_t urlTimeoutMs = 10000; // default 10s
  };

public:
  explicit Settings(const nlohmann::json &prj, const nlohmann::json &prv);
  explicit Settings(const nlohmann::json &prj, const std::string &providersPath);
  explicit Settings(const std::string &path, const std::string &providersPath);

  void updateFromConfig(const nlohmann::json &config);
  void updateFromPath(const std::string &path);
  void save();
  void saveToPath(std::string_view path);
  std::string configPath() const { return path_; }
  std::string providersConfigPath() const { return providers_.path(); }

  void validate();
  static void validateProjectJson(nlohmann::json j);

  std::string tokenizerConfigPath() const {
    return config_["tokenizer"].value("config_path", "tokenizer.json");
  }

  size_t chunkingMaxTokens() const { return config_["chunking"].value("nof_max_tokens", size_t(500)); }
  size_t chunkingMinTokens() const { return config_["chunking"].value("nof_min_tokens", size_t(50)); }
  float chunkingOverlap() const { return config_["chunking"].value("overlap_percentage", 0.1f); }
  bool chunkingSemantic() const { return config_["chunking"].value("semantic", false); }

  //std::vector<std::string> enabledEmbeddingProviders() const { return config_["embedding"].value("enabled_providers", nlohmann::json::array()); }
  ApiConfig embeddingCurrentApi() const;
  std::vector<ApiConfig> embeddingApis() const { return providers_.embeddingProviders(); }
  size_t embeddingTimeoutMs() const { return config_["embedding"].value("timeout_ms", size_t(10'000)); }
  size_t embeddingBatchSize() const { return config_["embedding"].value("batch_size", size_t(4)); }
  size_t embeddingTopK() const { return config_["embedding"].value("top_k", size_t(5)); }
  std::string embeddingPrependLabelFormat() const {
    return config_["embedding"].value("prepend_label_format", std::string(""));
  }

  std::vector<std::string> enabledGenerationProviders() const { return config_["generation"].value("enabled_providers", nlohmann::json::array()); }
  std::string generationCurrentApiId() const { return config_["generation"].value("current_api", std::string{}); }
  bool generationIsAuto() const;
  ApiConfig generationCurrentApi() const;
  //std::vector<ApiConfig> generationApis() const { return providers_.generationProviders(); }
  size_t generationTimeoutMs() const { return config_["generation"].value("timeout_ms", size_t(20'000)); }
  size_t generationMaxFullSources() const { return config_["generation"].value("max_full_sources", size_t(2)); }
  size_t generationMaxRelatedPerSource() const { return config_["generation"].value("max_related_per_source", size_t(3)); }
  size_t generationMaxChunks() const { return config_["generation"].value("max_chunks", size_t(5)); }
  float generationDefaultTemperature() const { return config_["generation"].value("default_temperature", 0.5f); }
  size_t generationDefaultMaxTokens() const { return config_["generation"].value("default_max_tokens", size_t(2048)); }
  std::string generationPrependLabelFormat() const {
    return config_["generation"].value("prepend_label_format", std::string(""));
  }
  bool generationExcerptEnabled() const {
    return config_["generation"].contains("excerpt") ? config_["generation"]["excerpt"].value("enabled", true) : true;
  }
  size_t generationExcerptMinChunks() const {
    return config_["generation"].contains("excerpt") ? config_["generation"]["excerpt"].value("min_chunks", size_t(3)) : size_t(3);
  }
  size_t generationExcerptMaxChunks() const {
    return config_["generation"].contains("excerpt") ? config_["generation"]["excerpt"].value("max_chunks", size_t(9)) : size_t(9);
  }
  float generationExcerptThresholdRatio() const {
    return config_["generation"].contains("excerpt") ? config_["generation"]["excerpt"].value("threshold_ratio", 0.6f) : 0.6f;
  }

  std::string databaseSqlitePath() const { return config_["database"].value("sqlite_path", "db.sqlite"); }
  std::string databaseIndexPath() const { return config_["database"].value("index_path", "index"); }
  size_t databaseVectorDim() const { return config_["database"].value("vector_dim", size_t(768)); }
  size_t databaseMaxElements() const { return config_["database"].value("max_elements", size_t(100'000)); }
  std::string databaseDistanceMetric() const { return config_["database"].value("distance_metric", "cosine"); }

  size_t filesMaxFileSizeMb() const { return config_["source"].value("max_file_size_mb", size_t(10)); }
  std::string filesEncoding() const { return config_["source"].value("encoding", "utf-8"); }
  std::vector<std::string> filesGlobalExclusions() const { return config_["source"].value("global_exclude", std::vector<std::string>{}); }
  std::vector<std::string> filesDefaultExtensions() const { return config_["source"].value("default_extensions", std::vector<std::string>{".txt", ".md"}); }

  std::string loggingLoggingFile() const {
    return config_.contains("logging") ? config_["logging"].value("logging_file", "output.log") : std::string("output.log");
  }
  std::string loggingDiagnosticsFile() const { 
    return config_.contains("logging") ? config_["logging"].value("diagnostics_file", "diagnostics.log") : std::string("diagnostics.log");
  }
  bool loggingLogToFile() const {
    return config_.contains("logging") ? config_["logging"].value("log_to_file", true) : true;
  }
  bool loggingLogToConsole() const {
    return config_.contains("logging") ? config_["logging"].value("log_to_console", true) : true;
  }

  void initProjectIdIfMissing(bool hydrateFile);
  void initProjectTitleIfMissing(bool hydrateFile);

  std::string getProjectId() const { return config_["source"].value("project_id", ""); }
  std::string getProjectTitle() const { return config_["source"].value("project_title", ""); }
  std::vector<SourceItem> sources() const;
  std::string configDump() const { return config_.dump(2); }
  nlohmann::json configJson() const { return config_; }
  nlohmann::json providersJson() const { return providers_.configJson(); }
  const ProvidersSettings &providers() const { return providers_; }
  AutoRouterConfig autoRouterConfig() const;
};

#endif // _SETTINGS_H_
