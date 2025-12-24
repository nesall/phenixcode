#ifndef _SETTINGS_H_
#define _SETTINGS_H_

#define _CRT_SECURE_NO_WARNINGS

#include <string>
#include <vector>
#include <map>
#include "nlohmann/json.hpp"

struct ApiConfig {
  std::string id;
  std::string name;
  std::string apiUrl;
  std::string apiKey;
  std::string model;
  std::string queryFormat;
  std::string documentFormat;
  std::string maxTokensName; // e.g. max_tokens or max_completion_tokens
  bool temperatureSupport = true;
  bool enabled = true;
  bool stream = true;
  size_t contextLength = 0;
  struct {
    float input = 0;
    float output = 0;
    float cachedInput = 0;
  } pricing;
  struct {
    std::string apiUrl;
    std::string prefixName; // non-empty means model supports FIM mode.
    std::string suffixName;
    std::vector<std::string> stopTokens;
  } fim;

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

class Settings {
private:
  nlohmann::json config_;
  std::string path_;

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
  explicit Settings(const std::string &path = "settings.json");

  void updateFromConfig(const nlohmann::json &config);
  void updateFromPath(const std::string &path);
  void save();
  std::string configPath() const { return path_; }

  std::string tokenizerConfigPath() const {
    return config_["tokenizer"].value("config_path", "tokenizer.json");
  }

  size_t chunkingMaxTokens() const { return config_["chunking"].value("nof_max_tokens", size_t(500)); }
  size_t chunkingMinTokens() const { return config_["chunking"].value("nof_min_tokens", size_t(50)); }
  float chunkingOverlap() const { return config_["chunking"].value("overlap_percentage", 0.1f); }
  bool chunkingSemantic() const { return config_["chunking"].value("semantic", false); }

  ApiConfig embeddingCurrentApi() const;
  std::vector<ApiConfig> embeddingApis() const;
  size_t embeddingTimeoutMs() const { return config_["embedding"].value("timeout_ms", size_t(10'000)); }
  size_t embeddingBatchSize() const { return config_["embedding"].value("batch_size", size_t(4)); }
  size_t embeddingTopK() const { return config_["embedding"].value("top_k", size_t(5)); }
  std::string embeddingPrependLabelFormat() const {
    return config_["embedding"].value("prepend_label_format", std::string(""));
  }

  ApiConfig generationCurrentApi() const;
  std::vector<ApiConfig> generationApis() const;
  size_t generationTimeoutMs() const { return config_["generation"].value("timeout_ms", size_t(20'000)); }
  size_t generationMaxFullSources() const { return config_["generation"].value("max_full_sources", size_t(2)); }
  size_t generationMaxRelatedPerSource() const { return config_["generation"].value("max_related_per_source", size_t(3)); }
  //size_t generationMaxContextTokens() const { return config_["generation"].value("max_context_tokens", size_t(20'000)); }
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
};


#endif // _SETTINGS_H_
