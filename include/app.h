#ifndef _APP_H_
#define _APP_H_

#include <memory>
#include <string>
#include <vector>
#include <cstdint>
#include "json_shim.h"

class Chunker;
class Settings;
class AdminAuth;
class VectorDatabase;
class SourceProcessor;
class EmbeddingClient;
class CompletionClient;
class SimpleTokenizer;
class InstanceRegistry;
class MonitorTracker;
struct SearchResult;

inline constexpr float kHybridBm25Weight = 0.5f;
inline constexpr float kGrepRrfWeight = 0.3f;

struct GrepDiagEntry {
  std::string docId;    // sourceId of the hybrid candidate
  size_t grepRank;      // position in grepRes (0-based); npos if not from grep
  float boost;          // grepWeight / (rrfK + grepRank + 1) actually added
};

struct RetrieveDiagnostics {
  std::vector<std::string> grepTerms;
  std::vector<GrepDiagEntry> grepRanking;  // every grep hit, in grep order
  bool grepRan = false;
};

enum class SearchMode : uint8_t {
  Auto = 0,
  Base = 1, // Vector only
  Hybrid = 2, // Vector + BM25
  Grep = 4,
  HybridGrep = Hybrid | Grep
};
inline SearchMode operator|(SearchMode a, SearchMode b) { return static_cast<SearchMode>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b)); }
inline SearchMode operator&(SearchMode a, SearchMode b) { return static_cast<SearchMode>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b)); }
inline SearchMode &operator|=(SearchMode &a, SearchMode b) { a = a | b; return a; }

class App {
  struct Impl;
  std::unique_ptr<Impl> imp;
public:
  explicit App();
  ~App();

  // CLI commands
  void embed(bool noPrompt);
  void watch(int interval_seconds = 60);
  size_t update();
  void compact();
  void search(const std::string &query, size_t topK = 5, SearchMode mode = SearchMode::Auto) const;
  void stats() const;
  void clear(bool noPrompt);
  void chat() const;
  void serve(int port, bool watch = false, int interval = 60, const std::string &infoFile = {});
  void providers(const std::string &testProvider);
  void eval(const std::string &datasetPath, size_t topK, float bm25Weight, float grepWeight) const;
  void evalInit(const std::string &outputPath, size_t numSamples = 10);
  void evalList();
  void evalDetail(const std::string &docId);
  void evalSweep(const std::string &ds, size_t topK) const;

  // Helper
  std::vector<SearchResult> retrieve(const std::string &text,
    const std::vector<float> &embedding, size_t topK,
    SearchMode mode = SearchMode::HybridGrep, float bm25Weight = kHybridBm25Weight, float grepWeight = kGrepRrfWeight,
    RetrieveDiagnostics *diag = nullptr) const;
  std::vector<SearchResult> grepSearch(const std::string &query, size_t topK, 
    const std::vector<std::string> &preExtractedTerms = {}) const;

  const Settings &settings() const;
  Settings &refSettings();
  const SimpleTokenizer &tokenizer() const;
  const SourceProcessor &sourceProcessor() const;
  const Chunker &chunker() const;
  const VectorDatabase &db() const;
  VectorDatabase &db();
  const AdminAuth &auth() const;
  AdminAuth &auth();
  const InstanceRegistry &registry() const;
  const MonitorTracker &monitor() const;
  MonitorTracker &monitor();

  bool isValidPrivateAppKey(const std::string &appKey);
  void requestShutdownAsync();

public:
  static void printUsage();
  static int run(int argc, char *argv[]);

public:
  float dbSizeMB() const;
  float indSizeMB() const;
  size_t uptimeSeconds() const;
  size_t startTimestamp() const;
  size_t lastUpdateTimestamp() const;
  nlohmann::json sourceStats() const;

  //std::string describeProjectTitle() const;

private:
  void initialize(/*const std::string &configPath*/);
  bool testSettings() const;
  static std::string runSetupWizard(AdminAuth &auth);
  static std::string findConfigFile(const std::string &filename);
  static int handleInteractivePasswordReset();
  static int handlePasswordStatus();
  //static bool shouldRunGrep(const std::string &text, const std::vector<std::string> &terms);
  nlohmann::json evalRobust(const std::string &datasetPath, size_t topK, SearchMode mode, float bm25Weight, float grepWeight) const;
  void evalAddVectors(const std::string &datasetPath, const std::string &outputPath) const;
};

#endif // _APP_H_