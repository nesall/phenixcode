#include "app.h"
#include "settings.h"
#include "database.h"
#include "inference.h"
#include "chunker.h"
#include "tokenizer.h"
#include "sourceproc.h"
#include "httpserver.h"
#include "auth.h"
#include "instregistry.h"
#include "cutils.h"
#include "monitor.h"
#include "updater.h"
#include "evaluator.h"
#include <iostream>
#include <fstream>
#include <algorithm>
#include <filesystem>
#include <vector>
#include <cmath>
#include <iomanip>
#include <unordered_map>
#include <thread>
#include <stdexcept>
#include <chrono>
#include <cstdint>
#include <tuple>
#include <memory>
#include <cassert>
#include <map>
#include <json_shim.h>
#include <utils_log/logger.hpp>
#include "3rdparty/fmt/format.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#include <termios.h>
#include <sys/stat.h>
#endif

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

  std::string stripUrlQueryAndAnchor(const std::string &url) {
    size_t qpos = url.find('?');
    size_t apos = url.find('#');
    size_t pos = (std::min)(
      qpos != std::string::npos ? qpos : url.size(),
      apos != std::string::npos ? apos : url.size()
      );
    return url.substr(0, pos);
  }

  std::string detectLanguage(const std::string &path) {
    std::string ext = std::filesystem::path(path).extension().string();

    static const std::map<std::string, std::string> ext_map = {
        { ".cpp", "C++" },{ ".hpp", "C++" },{ ".h", "C++" },
        { ".c", "C" },
        { ".py", "Python" },
        { ".js", "JavaScript" },{ ".ts", "TypeScript" },
        { ".java", "Java" },
        { ".go", "Go" },
        { ".rs", "Rust" },
        { ".md", "Markdown" },{ ".txt", "Text" }
    };

    auto it = ext_map.find(ext);
    return it != ext_map.end() ? it->second : "Other";
  }

  json computeStats(VectorDatabase &db) {
    auto trackedFiles = db.getTrackedFiles();

    auto chunkCounts = db.getChunkCountsBySources();

    std::map<std::string, int> byLanguage;
    std::map<std::string, int> byDirectory;
    size_t totalLines = 0;
    size_t totalSize = 0;

    std::vector<json> fileDetails;

    for (const auto &file : trackedFiles) {
      if (!std::filesystem::exists(file.path)) continue;

      size_t lines = file.nofLines;
      size_t size = file.fileSize;

      std::string lang = detectLanguage(file.path);
      std::string dir = std::filesystem::path(file.path).parent_path().string();

      byLanguage[lang]++;
      byDirectory[dir]++;
      totalLines += lines;
      totalSize += size;

      size_t chunkCount = chunkCounts.count(file.path) ? chunkCounts[file.path] : 0ull;

      fileDetails.push_back({
          { "path", file.path },
          { "lines", lines },
          { "size_bytes", size },
          { "language", lang },
          { "chunks", chunkCount },
          { "last_modified", file.lastModified }
        });
    }

    // Sort by chunk count (most chunked files)
    std::sort(fileDetails.begin(), fileDetails.end(),
      [](const json &a, const json &b) {
        return a["chunks"] > b["chunks"];
      });

    json ar = json::array();
    for (auto it = fileDetails.begin(); it != fileDetails.begin() + (std::min)(size_t(10), fileDetails.size()); ++it) {
      ar.push_back(*it);
    }

    return {
        { "total_files", trackedFiles.size() },
        { "total_lines", totalLines },
        { "total_size_bytes", totalSize },
        { "by_language", byLanguage },
        { "by_directory", byDirectory },
        { "top_files", ar }
    };
  }

  class StatsCache {
  private:
    json cachedStats_;
  public:
    void clear() {
      cachedStats_ = json{};
    }
    json getStats(VectorDatabase &db, bool forceRefresh = false) {
      if (forceRefresh /*|| CACHE_TTL_SECONDS < elapsed*/ || cachedStats_.empty()) {
        cachedStats_ = computeStats(db);
      }
      return cachedStats_;
    }
  };


  void addQualifiedLastComponents(std::vector<std::string> &terms) {
    const size_t n = terms.size();
    for (size_t i = 0; i < n; ++i) {
      const size_t pos = terms[i].rfind("::");
      if (pos == std::string::npos || pos + 2 >= terms[i].size()) continue;
      std::string last = terms[i].substr(pos + 2);          // `Cls::name` -> `name`, `Cls::~Cls` -> `~Cls`
      if (last.size() < 3) continue;
      if (std::find(terms.begin(), terms.end(), last) == terms.end())
        terms.push_back(std::move(last));
    }
  }

} // anonymous namespace


struct App::Impl {
  std::unique_ptr<Settings> settings_;
  std::unique_ptr<AdminAuth> auth_;
  std::unique_ptr<VectorDatabase> db_;
  std::unique_ptr<SimpleTokenizer> tokenizer_;
  std::unique_ptr<Chunker> chunker_;
  std::unique_ptr<SourceProcessor> processor_;
  std::unique_ptr<updater::IncrementalUpdater> updater_;
  std::unique_ptr<HttpServer> httpServer_;
  std::unique_ptr<MonitorTracker> monitor_;

  std::chrono::system_clock::time_point appStartTime_;
  std::chrono::system_clock::time_point lastUpdateTime_;

  std::unique_ptr<InstanceRegistry> registry_;
  std::string privateAppKey_;

  StatsCache statsCache_;
  //std::string projectTitle_;

  static std::string binaryName_;
};
std::string App::Impl::binaryName_ = "";

App::App() : imp(new Impl)
{
  imp->auth_ = std::make_unique<AdminAuth>();
}

App::~App()
{
  if (imp->httpServer_) imp->httpServer_->stop();
  if (imp->monitor_) imp->monitor_->stopPersistTimer();
}

void App::setSettings(std::unique_ptr<Settings> settings)
{
  imp->settings_ = std::move(settings);
}

void App::setPrivateAppKey(const std::string &key)
{
  imp->privateAppKey_ = key;
}

void App::setBinaryName(const std::string &bname)
{
  App::Impl::binaryName_ = bname;
}

std::string App::binaryName()
{
  return App::Impl::binaryName_;
}

void App::initialize(/*const std::string &configPath*/)
{
  assert(imp->settings_);
  imp->appStartTime_ = std::chrono::system_clock::now();

  auto &ss = *imp->settings_;

  std::string dbPath = ss.databaseSqlitePath();
  std::string indexPath = ss.databaseIndexPath();
  size_t vectorDim = ss.databaseVectorDim();
  size_t maxElements = ss.databaseMaxElements();
  VectorDatabase::DistanceMetric metric = ss.databaseDistanceMetric() == "cosine" ? VectorDatabase::DistanceMetric::Cosine : VectorDatabase::DistanceMetric::L2;

  imp->db_ = std::make_unique<HnswSqliteVectorDatabase>(dbPath, indexPath, vectorDim, maxElements, metric);

  imp->tokenizer_ = std::make_unique<SimpleTokenizer>(ss.tokenizerConfigPath());

  size_t minTokens = ss.chunkingMinTokens();
  size_t maxTokens = ss.chunkingMaxTokens();
  float overlap = ss.chunkingOverlap();

  imp->chunker_ = std::make_unique<Chunker>(*imp->tokenizer_, minTokens, maxTokens, overlap);
  imp->processor_ = std::make_unique<SourceProcessor>(*imp->settings_);
  imp->updater_ = std::make_unique<updater::IncrementalUpdater>(this, imp->settings_->embeddingBatchSize());

  imp->httpServer_ = std::make_unique<HttpServer>(*this);

  imp->monitor_ = std::make_unique<MonitorTracker>(*imp->settings_);
  imp->monitor_->startPersistTimer(5);
  imp->monitor_->setState(MonitorState::Ready);

#ifdef _DEBUG
  {
    const std::vector<std::string> queries{ 
      "What does sanitizeUtf8 function do?", 
      "What does looksLikeOpaqueBlob function do?", 
      "What does detectApiStyle function do?", 
      "Where is isPunctuation function defined?", 
      "What is CODE_RATIO_STRONG used for?", 
      "how is registry stored" 
    };
    for (const auto &query : queries) {
      EmbeddingClient embeddingClient{ settings().embeddingCurrentApi(), settings().embeddingTimeoutMs() };
      std::vector<float> queryEmbedding;
      embeddingClient.generateEmbeddings(query, queryEmbedding, EmbeddingClient::EncodeType::Query);
      auto testRes = retrieve(query, queryEmbedding, 5);
      LOG_MSG_DEBUG << "\nquery:" << query << ", nof res:" << testRes.size();
      for (const auto &res : testRes) {
        LOG_MSG_DEBUG << "Test search result:" << res.chunkId << res.sourceId << "(similarityScore:" << res.similarityScore << ", fusedScore:" << res.fusedScore << ")";
      }
    }
  }
#endif
}

bool App::testSettings() const
{
  bool ok = true;
  ApiConfig api;
  try {
    {
      api = settings().embeddingCurrentApi();
      LOG_MSG_DEBUG << "Testing embedding client" << api.model;
      LOG_MSG_DEBUG << "  document format - '" << utils_log::LOGNOSPACE << api.documentFormat << "'";
      LOG_MSG_DEBUG << "  query format - '" << utils_log::LOGNOSPACE << api.queryFormat << "'";
      std::string textA0 = "int main() {}";
      std::string textA1 = "int main() { return 0; }";
      std::string textB0 = "double main() { return 0.0; }";
      std::string textB1 = "float main() { reutrn 0.f; }";
      std::string textC0 = "class Foo { void bar() { std::cout << \"hello\"; } };";
      EmbeddingClient cl{ api, settings().embeddingTimeoutMs() };
      std::vector<float> vA0;
      cl.generateEmbeddings(textA0, vA0, EmbeddingClient::EncodeType::Query);
      if (vA0.size() == 0) {
        throw std::runtime_error("Embedding client not working");
      }
      float l2Norm = EmbeddingClient::calculateL2Norm(vA0);
      LOG_MSG_DEBUG << "  Embedding client works fine." << "[ l2norm" << l2Norm << "]";
      LOG_MSG_DEBUG << "  Testing similarities:";

      std::vector<float> vA1, vB0, vB1, vC0;
      cl.generateEmbeddings(textA1, vA1, EmbeddingClient::EncodeType::Query);
      cl.generateEmbeddings(textB0, vB0, EmbeddingClient::EncodeType::Query);
      cl.generateEmbeddings(textB1, vB1, EmbeddingClient::EncodeType::Query);
      cl.generateEmbeddings(textC0, vC0, EmbeddingClient::EncodeType::Query);

      std::vector<float> vA0_doc, vA0_query;
      cl.generateEmbeddings(textA0, vA0_doc, EmbeddingClient::EncodeType::Document);
      cl.generateEmbeddings(textA0, vA0_query, EmbeddingClient::EncodeType::Query);

      // Calculate cosine similarities
      auto cosine = [](const std::vector<float> &a, const std::vector<float> &b) {
        float dot = 0.0f;
        for (size_t i = 0; i < a.size(); ++i) dot += a[i] * b[i];
        return dot; // Already normalized if l2norm=1
        };

      float simA0A1 = cosine(vA0, vA1); // Similar code - expect ~0.8-0.95
      float simA0B0 = cosine(vA0, vB0); // Different return type - expect ~0.6-0.8
      float simA0B1 = cosine(vA0, vB1); // Typo + different - expect ~0.5-0.7
      float simB0B1 = cosine(vB0, vB1); // Similar but typo - expect ~0.7-0.9
      float simA0C0 = cosine(vA0, vC0); // Different
      float simSelf = cosine(vA0_doc, vA0_query); // Should be 0.7-0.9, not 1.0

      auto yesNo = [](float val, float a, float b) {
        if (a <= val && val <= b) return "";// "PASS";
        return "";// "FAIL";
        };

      LOG_MSG_DEBUG << "    A0-A1 (similar):    " << simA0A1 << yesNo(simA0A1, 0.8f, 0.95f);
      LOG_MSG_DEBUG << "    A0-B0 (different):  " << simA0B0 << yesNo(simA0B0, 0.3f, 0.7f);
      LOG_MSG_DEBUG << "    A0-B1 (typo):       " << simA0B1 << yesNo(simA0B1, 0.3f, 0.7f);
      LOG_MSG_DEBUG << "    B0-B1 (similar):    " << simB0B1 << yesNo(simB0B1, 0.6f, 0.9f);
      LOG_MSG_DEBUG << "    A0-C0 (different):  " << simA0C0 << yesNo(simA0C0, 0.1f, 0.5f);
      LOG_MSG_DEBUG << "    Doc-Query (Similar):" << simSelf << yesNo(simSelf, 0.75f, 0.95f);
    }
    {
      api = {};
      const bool isAuto = settings().generationIsAuto();
      if (isAuto) {
        auto router = settings().autoRouterConfig();
        const ApiConfig *clfCfg = settings().providers().findGeneration(router.classifier.apiId);
        if (!clfCfg)
          clfCfg = settings().providers().findGeneration(router.fallbackModelId);
        if (!clfCfg) {
          throw std::runtime_error("No valid generation API found for testing");
        }
        api = *clfCfg;
      } else {
        api = settings().generationCurrentApi();
      }
      LOG_MSG_DEBUG << "\nTesting completion client" << api.model;
      CompletionClient cl{ api, settings().generationTimeoutMs(), *this };
      std::vector<json> messages;
      messages.push_back({ {"role", "system"}, {"content", "You are a helpful assistant."} });
      messages.push_back({ {"role", "user"}, {"content", "Answer in one word only - what is the capital of France?"} });

      std::string fullResponse = cl.generateCompletion(
        messages, {}, 0.0f, settings().generationDefaultMaxTokens(),
        [](const std::string &chunk) {
          //std::cout << chunk << std::flush;
        }
      );
      if (fullResponse.find("Paris") != std::string::npos) {
        LOG_MSG_DEBUG << "  Completion client works fine.";
      } else {
        throw std::runtime_error("Completion client not working");
      }
    }
  } catch (const std::exception &ex) {
    LOG_MSG_ERROR << " " << ex.what();
    LOG_MSG_ERROR << " [" << api.apiUrl << "]";
    LOG_MSG_ERROR << " [" << api.model << "]";
    LOG_MSG_ERROR << "  Please, check settings file and edit it manually if needed.";
    LOG_MSG_ERROR << "   [ settings file path" << std::filesystem::absolute(settings().configPath()) << "]";
    ok = false;
  }
  return ok;
}

void App::embed(bool noPrompt)
{
  LOG_MSG << "Starting embedding process...";
  imp->monitor_->beginPhase(MonitorPhase::Walk);
  auto sources = imp->processor_->collectSources(false);
  imp->monitor_->updateProgress(MonitorPhase::Walk, 0, sources.size(), 0, 0);
  LOG_MSG << "Total" << sources.size() << "sources collected";

  // Print number of sources by extension
  std::unordered_map<std::string, size_t> extCount;
  std::unordered_map<std::string, size_t> dirCount;
  size_t urlCount = 0;
  size_t emptyTextCount = 0;
  for (const auto &[isUrl, text, source] : sources) {
    if (!isUrl) {
      std::filesystem::path p(source);
      extCount[p.extension().string()]++;
      dirCount[p.parent_path().string()]++;
    } else {
      urlCount ++;
      if (text.empty()) emptyTextCount++;
    }
  }

  LOG_MSG << "Sources by extension:";
  for (const auto &[ext, count] : extCount) {
    LOG_MSG << "  " << (ext.empty() ? "[no extension]" : ext) << ":" << count;
  }

  LOG_MSG << "Sources by directory:";
  for (const auto &[dir, count] : dirCount) {
    LOG_MSG << "  " << (dir.empty() ? "[root]" : dir) << ":" << count;
  }

  LOG_MSG << "URLs:" << urlCount;

  if (!noPrompt) {
    std::cout << "Proceed? [y/N]: ";
    std::string confirm;
    std::cin >> confirm;
    if (confirm != "Y" && confirm != "y") {
      LOG_MSG << "Exitted.";
      return;
    }
  }

  size_t totalChunks = 0;
  size_t totalFiles = 0;
  size_t totalTokens = 0;
  size_t skippedFiles = 0;
  EmbeddingClient embeddingClient{ settings().embeddingCurrentApi(), settings().embeddingTimeoutMs() };
  for (size_t i = 0; i < sources.size(); ++i) {
    const auto &source = sources[i].source;
    try {
      if (!sources[i].isUrl && !fs::exists(source)) {
        LOG_MSG_WARN << "File not found: " << source << ". Skipped.";
        skippedFiles++;
        continue;
      }
      if (imp->db_->fileExistsInMetadata(source)) {
        LOG_MSG_WARN << "Duplicate source" << source << ". Skipped.";
        skippedFiles ++;
        continue;
      }

      imp->db_->beginTransaction();
      LOG_MSG << "PROCESSING" << source << fmt::format("({}/{})", i + 1, sources.size());

      std::string content{ sources[i].content };

      if (!sources[i].isUrl) {
        assert(content.empty());
        SourceProcessor::readFile(source, content);
        if (content.empty()) {
          LOG_MSG << "  Empty file. Skipped.";
          imp->db_->rollback();
          skippedFiles++;
          continue;
        }
      }

      const std::string sourceId = sources[i].isUrl ? stripUrlQueryAndAnchor(source) : std::filesystem::path(source).string();

      imp->monitor_->updateProgress(MonitorPhase::Chunk, totalFiles, sources.size(), totalChunks, 0);
      auto chunks = imp->chunker_->chunkText(content, sourceId);

      LOG_MSG << "  Generated" << chunks.size() << "chunks";
      const size_t batchSize = imp->settings_->embeddingBatchSize();
      totalTokens += updater::addEmbedChunks(
        chunks, batchSize, embeddingClient, *imp->db_,
        settings().embeddingPrependLabelFormat(),
        imp->monitor_.get(), totalFiles, sources.size(), &settings());
      std::cout << std::endl;
      totalChunks += chunks.size();
      totalFiles++;
      imp->monitor_->updateProgress(MonitorPhase::Persist, totalFiles, sources.size(), totalChunks, 0);
      imp->db_->commit();
      imp->db_->persist();
    } catch (const std::exception &e) {
      imp->db_->rollback();
      skippedFiles++;
      LOG_MSG_ERROR << "Error processing" << source << ": " << e.what();
    }
  }
  imp->db_->persist();
  imp->monitor_->finishIndexing(true);
  imp->monitor_->persist();
  LOG_MSG << "\nCompleted!";
  LOG_MSG << "  Files processed:" << totalFiles;
  LOG_MSG << "  Files skipped:" << skippedFiles;
  LOG_MSG << "  Total chunks:" << totalChunks;
  LOG_MSG << "  Total tokens:" << totalTokens;
}

void App::compact()
{
  LOG_MSG << "Compacting vector index...";
  imp->db_->compact();
  imp->db_->persist();
  LOG_MSG << "Done!";
}

void App::search(const std::string &query, size_t topK, SearchMode mode) const
{
  LOG_MSG << "Searching for:" << query;
  EmbeddingClient embeddingClient{ settings().embeddingCurrentApi(), settings().embeddingTimeoutMs() };
  std::vector<float> queryEmbedding;
  if (mode != SearchMode::Grep && 0 < db().chunkCount()) {
    try {
      embeddingClient.generateEmbeddings(query, queryEmbedding, EmbeddingClient::EncodeType::Query);
    } catch (const std::exception &e) {
      LOG_MSG_WARN << "Unable to generate embeddings" << e.what() << "| Falling back to grep.";
      queryEmbedding.clear();
    }
  }
  auto results = retrieve(query, queryEmbedding, topK);

  std::cout << "\nFound " << results.size() << " results:" << std::endl;
  std::cout << std::string(80, '-') << std::endl;

  for (size_t i = 0; i < results.size(); ++i) {
    const auto &result = results[i];
    std::cout << "\n[" << (i + 1) << "] Score: " << result.similarityScore << std::endl;
    std::cout << "Source: " << result.sourceId << std::endl;
    std::cout << "Type: " << result.chunkType << std::endl;
    std::cout << "Content: " << result.content.substr(0, 200);
    if (result.content.length() > 200) std::cout << "...";
    std::cout << std::endl;
  }
}

void App::stats() const
{
  LOG_MSG << "\n=== Database Statistics ===";
  auto j = sourceStats();
  auto s = j.dump(2);
  std::cout << s << "\n";
}

void App::clear(bool noPrompt)
{
  if (noPrompt) {
    imp->db_->clear();
    LOG_MSG << "Database cleared.";
  } else {
    std::cout << "Are you sure you want to clear all data? [y/N]: ";
    std::string confirm;
    std::cin >> confirm;
    if (confirm == "y") {
      imp->db_->clear();
      LOG_MSG << "Database cleared.";
    } else {
      std::cout << "Cancelled." << std::endl;
    }
  }
}

void App::chat() const
{
  const bool isAuto = settings().generationIsAuto();
  ApiConfig staticCfg;
  if (!isAuto)
    staticCfg = settings().generationCurrentApi();

  if (isAuto)
    std::cout << "Using auto-router.\n";
  else
    std::cout << "Using model: " << staticCfg.model << " at " << staticCfg.apiUrl << "\n";
  std::cout << "Entering chat mode. Type 'exit' to quit.\n";

  std::vector<json> messages;
  messages.push_back({ {"role", "system"}, {"content", "You are a helpful assistant."} });
  EmbeddingClient embeddingClient{ settings().embeddingCurrentApi(), settings().embeddingTimeoutMs() };

  while (true) {
    try {
      std::cout << "\nYou: ";
      std::string userInput;
      std::getline(std::cin, userInput);
      if (userInput == "exit") break;
      if (userInput.empty()) continue;

      messages.push_back({ {"role", "user"}, {"content", userInput} });

      ApiConfig apiCfg = staticCfg;
      if (isAuto) {
        const AutoRouterConfig router = settings().autoRouterConfig();
        std::string modelId = router.fallbackModelId;
        try {
          const ApiConfig *clfCfg = settings().providers().findGeneration(router.classifier.apiId);
          if (!clfCfg)
            throw std::runtime_error("classifier api_id not in generation_providers");

          InferenceClient clf(*clfCfg, router.classifier.timeoutMs);
          const nlohmann::json clfMsgs = nlohmann::json::array({
            {{"role", "system"}, {"content", router.classifier.prompt}},
            {{"role", "user"}, {"content", userInput}}
            });
          const std::string raw = clf.generateChat(clfMsgs, router.classifier.temperature, router.classifier.maxTokens);
          modelId = router.resolveRoutedModelId(raw);
          std::cout << "[router] tag=" << AutoRouterConfig::normalizeTierTag(raw) << " -> " << modelId << "\n";
        } catch (const std::exception &e) {
          std::cout << "[router] classifier failed (" << e.what() << "), fallback=" << modelId << "\n";
        }
        const ApiConfig *routed = settings().providers().findGeneration(modelId);
        if (!routed)
          throw std::runtime_error("auto-router resolved unknown id '" + modelId + "'");
        apiCfg = *routed;
      }

      std::vector<float> queryEmbedding;
      if (0 < db().chunkCount()) {
        try {
          embeddingClient.generateEmbeddings(userInput, queryEmbedding, EmbeddingClient::EncodeType::Query);
        } catch (const std::exception &e) {
          LOG_MSG_WARN << "Unable to generate embeddings" << e.what() << "| Falling back to grep.";
          queryEmbedding.clear();
        }
      }
      auto searchResults = retrieve(userInput, queryEmbedding, 5, SearchMode::Auto);

      CompletionClient completionClient{ apiCfg, settings().generationTimeoutMs(), *this };
      std::cout << "\nAssistant: " << std::flush;
      const std::string assistantResponse = completionClient.generateCompletion(
        messages, searchResults, 0.0f, settings().generationDefaultMaxTokens(),
        [](const std::string &chunk) { std::cout << chunk << std::flush; });
      std::cout << std::endl;
      messages.push_back({ {"role", "assistant"}, {"content", assistantResponse} });
    } catch (const std::exception &e) {
      LOG_MSG_ERROR << e.what();
    }
  }
  std::cout << "Exiting chat mode.\n";
}

void App::serve(int suggestedPort, bool watch, int interval, const std::string &infoFile)
{
  std::thread watchThread;
  std::thread serverThread;

  // Use scope guard for cleanup
  auto cleanup = [&]() {
    LOG_MSG << "Shutting down gracefully...";

    imp->httpServer_->stop();
    imp->db_->persist();
    if (serverThread.joinable()) serverThread.join();
    if (watchThread.joinable()) watchThread.join();

    LOG_MSG << "Shutdown complete.";
    };

  try {
    if (watch) {
      LOG_MSG << "Auto-update: enabled (every" << interval << "s)";
      watchThread = std::thread([this, interval]() {
        LOG_MSG << "[Watch] Background monitoring started (interval:" << interval << "s)";
        auto nextUpdate = std::chrono::steady_clock::now() + std::chrono::seconds(interval);
        while (!SignalHandler::shouldShutdown()) {
          auto now = std::chrono::steady_clock::now();
          if (now < nextUpdate) {
            // Sleep for the remaining time or 100ms, whichever is smaller
            auto r = std::chrono::duration_cast<std::chrono::milliseconds>(nextUpdate - now);
            auto d = (std::min)(r, std::chrono::milliseconds(100));
            std::this_thread::sleep_for(d);
            continue;
          }
          try {
            update();
          } catch (const std::exception &e) {
            LOG_MSG << "[Watch] Error during update:" << e.what();
          }
          nextUpdate = std::chrono::steady_clock::now() + std::chrono::seconds(interval);
        }
        LOG_MSG << "[Watch] Background monitoring stopped";
        });
    } else {
      LOG_MSG << "  Auto-update: disabled";
    }

    serverThread = std::thread([this, suggestedPort, watch, interval, infoFile]() {
      const int newPort = imp->httpServer_->bindToPortIncremental(suggestedPort);
      if (!infoFile.empty()) {
        try {
          json infoData;
          infoData["port"] = newPort;
          infoData["timestamp"] = static_cast<size_t>(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
          infoData["watch_enabled"] = watch;
          infoData["watch_interval"] = interval;
          infoData["pid"] =
#ifdef _WIN32
            GetCurrentProcessId();
#else
            getpid();
#endif
          ;
          infoData["exec"] = Impl::binaryName_;

          std::ofstream outFile(infoFile);
          if (outFile.is_open()) {
            outFile << infoData.dump(2);
            outFile.close();
            LOG_MSG << "Instance info saved to --info-file" << infoFile;
          } else {
            LOG_MSG_WARN << "Unable to open info file for writing: " << infoFile;
          }
        } catch (const std::exception &e) {
          LOG_MSG_WARN << "Error saving instance info: " << e.what();
        }
      }
      if (0 < newPort) {
        imp->registry_ = std::make_unique<InstanceRegistry>(newPort, watch ? interval : 0, settings());
        imp->monitor_->setSessionId(imp->registry_->getInstanceId());
        imp->monitor_->persist();
        try {
          LOG_MSG << "\nStarting HTTP API server on port " << newPort << "...";
          imp->httpServer_->startServer();
        } catch (const std::exception &e) {
          LOG_MSG_ERROR << "[ERROR] Failed to register instance: " << e.what();
          SignalHandler::requestShutdown();
        }
      } else {
        LOG_MSG_ERROR << "\nHTTP server was unable to bind to ports in the range [" << suggestedPort << "to" << newPort << "]";
      }
      });

    while (!SignalHandler::shouldShutdown()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    cleanup();

  } catch (...) {
    cleanup();
    throw;
  }
}

void App::providers(const std::string &testProvider) const
{
  const auto &vc = settings().providers().generationProviders();

  if (testProvider.empty()) {
    size_t x = 0;
    size_t y = 0;
    for (const auto &a : vc) {
      x = (std::max)(a.id.length(), x);
      y = (std::max)(a.apiUrl.length(), y);
    }
    x += 4;
    y += 4;

    std::cout << std::left << std::setw(x) << "Id"
      << std::left << std::setw(y) << "Url"
      << std::left << std::setw(16) << "Enabled" << '\n';
    auto cur = settings().generationCurrentApi();
    for (const auto &a : vc) {
      std::string scur;
      if (cur.id == a.id) scur = " current";
      std::cout << std::left << std::setw(x) << a.id
        << std::left << std::setw(y) << a.apiUrl
        << std::left << std::setw(16) << std::to_string((int)a.enabled) + scur
        << '\n';
    }

  } else {
    ApiConfig api;
    for (const auto &a : vc) {
      if (a.id == testProvider) {
        api = a;
      }
    }
    if (api.id.empty()) {
      LOG_MSG << "Unable to find a provider by id =" << testProvider;
      return;
    }
    LOG_MSG << "Testing completion client" << api.apiUrl;
    CompletionClient cl{ api, settings().generationTimeoutMs(), *this };
    std::vector<json> messages;
    messages.push_back({ {"role", "system"}, {"content", "You are a helpful assistant."} });
    messages.push_back({ {"role", "user"}, {"content", "What is the capital of France?"} });

    std::string fullResponse = cl.generateCompletion(
      messages, {}, 0.0f, settings().generationDefaultMaxTokens(),
      [](const std::string &chunk) {
        //std::cout << chunk << std::flush;
      }
    );
    LOG_MSG << "Question:" << messages[1]["content"];
    LOG_MSG << "Answer:" << fullResponse;
  }
}

size_t App::update()
{
  LOG_MSG << "Checking for changes...";

  imp->lastUpdateTime_ = std::chrono::system_clock::now();

  // Check if index database is missing or empty
  auto dbStats = imp->db_->getStats();
  if (dbStats.totalChunks == 0) {
    LOG_MSG << "No index found. Performing full embedding...";
    embed(true);
    return dbStats.totalChunks; // After embed, totalChunks will be updated
  }

  try {
    Settings settingsNew{ imp->settings_->configPath(), imp->settings_->providersConfigPath()};
    imp->processor_->setSettings(settingsNew);
    LOG_MSG_DEBUG << "Read settings from" << settingsNew.configPath();
  } catch (const std::exception &ex) {
    LOG_MSG_WARN << ex.what();
    LOG_MSG_WARN << "Unable to re-read settings. Skipped.";
  }
  auto sources = imp->processor_->collectSources(false);
  std::vector<std::string> currentFiles;
  for (const auto &source : sources) {
    currentFiles.push_back(source.source);
  }
  auto info = imp->updater_->detectChanges(currentFiles);
  imp->updater_->printUpdateSummary(info);
  if (!imp->updater_->needsUpdate(info)) {
    LOG_MSG_DEBUG << "No updates needed. Database is up to date.";
    return 0;
  }
  LOG_MSG << "Applying updates...";
  EmbeddingClient embeddingClient{ settings().embeddingCurrentApi(), settings().embeddingTimeoutMs() };
  size_t updated = imp->updater_->updateDatabase(embeddingClient, *imp->chunker_, info);
  LOG_MSG << "Update completed! " << updated << " file(s) processed.";

  imp->lastUpdateTime_ = std::chrono::system_clock::now();
  imp->statsCache_.clear();
  return updated;
}

void App::watch(int intervalSeconds)
{
  std::cout << "Starting watch mode (checking every " << intervalSeconds << " seconds)" << std::endl;
  std::cout << "Press Ctrl+C to stop" << std::endl;
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(intervalSeconds));
    try {
      if (update()) {
        std::cout << "[" << utils::currentTimestamp() << "] updates detected and applied.\n";
      }
    } catch (const std::exception &e) {
      LOG_MSG << "Error during update: " << e.what();
    }
  }
}

std::vector<SearchResult> App::retrieve(const std::string &text, const std::vector<float> &embedding, size_t topK, 
  SearchMode mode, float bm25Weight, float grepWeight, eval::RetrieveDiagnostics *diag) const
{
  const int64_t nChunks = db().chunkCount();
  
  if (mode == SearchMode::Grep || nChunks == 0) {
    auto terms = utils::extractGrepTerms(text);   // no plan available yet
    if (diag) { diag->grepTerms = terms; diag->grepRan = true; }
    auto res = grepSearch(text, topK, terms);
    for (size_t i = 0; i < res.size(); ++i)
      if (diag) diag->grepRanking.push_back({ res[i].sourceId, i, res[i].fusedScore });
    return res;
  }

  if (mode == SearchMode::Base) {
    return db().search(embedding, topK);
  }

  const QueryPlan plan = db().planQuery(text); // chunk df lives here, and only here
  std::vector<std::string> grepTerms = plan.rareTerms;
  for (const auto &literal : utils::extractGrepLiterals(text)) {
    if (std::find(grepTerms.begin(), grepTerms.end(), literal) == grepTerms.end())
      grepTerms.push_back(literal);
  }
  addQualifiedLastComponents(grepTerms);
  if (mode == SearchMode::Auto) {
    const bool literalMarkers = utils::containsAny(text, { "`", "\"", "'", "::", "->", "()" });
    mode = (plan.hasRareIdentifier || literalMarkers) ? SearchMode::HybridGrep : SearchMode::Hybrid;
  }
  if (embedding.empty()) {
    auto res = grepSearch(text, topK, grepTerms);
    if (diag) diag->grepTerms = grepTerms;
    for (size_t i = 0; i < res.size(); ++i)
      if (diag) diag->grepRanking.push_back({ res[i].sourceId, i, res[i].fusedScore });
    return res;
  }

  const auto intent = utils::queryIntent(text);

  const auto pool = (intent != utils::QueryIntent::Neutral ? 2 * topK : topK);
  auto hybridRes = db().hybridSearch(embedding, text, pool, bm25Weight, &plan);

  constexpr float kIntentEpsilon = 0.0003f;

  auto intentBonus = [&](const SearchResult &r) -> float {
    const bool header = utils::isHeader(r.sourceId);
    if (intent == utils::QueryIntent::Implementation) return header ? 0.0f : kIntentEpsilon;
    if (intent == utils::QueryIntent::Declaration)    return header ? kIntentEpsilon : 0.0f;
    return 0.0f;
    };
  for (auto &c : hybridRes) {
    c.fusedScore += intentBonus(c);
  }

  std::sort(hybridRes.begin(), hybridRes.end(), [](const SearchResult &a, const SearchResult &b) {
    return std::tie(b.fusedScore, a.sourceId, a.chunkId) < std::tie(a.fusedScore, b.sourceId, b.chunkId);
    });

  if (topK < hybridRes.size()) hybridRes.resize(topK);
  
  if (mode != SearchMode::HybridGrep || grepTerms.empty()) {
    return hybridRes;
  }

  if (diag) diag->grepTerms = grepTerms;
  auto grepRes = grepSearch(text, topK, grepTerms);
  if (grepRes.empty()) {
    return hybridRes;
  }
  if (diag) diag->grepRan = true;

  constexpr float rrfK = 60.0f;
  std::unordered_map<std::string, size_t> bestRank; // stem -> best grep rank

  for (size_t i = 0; i < grepRes.size(); ++i) {
    bestRank.try_emplace(utils::stemKey(grepRes[i].sourceId), i); // first = best
    if (diag) diag->grepRanking.push_back({ grepRes[i].sourceId, i, 0.0f });
  }
  for (auto &c : hybridRes) {
    if (auto it = bestRank.find(utils::stemKey(c.sourceId)); it != bestRank.end())
      c.fusedScore += grepWeight / (rrfK + static_cast<float>(it->second) + 1.0f);
    if (diag) {
      for (auto &entry : diag->grepRanking) {
        if (entry.docId == c.sourceId) {
          entry.boost = grepWeight / (rrfK + static_cast<float>(bestRank[c.sourceId]) + 1.0f);
          break;
        }
      }
    }
  }
  // Re-sort so boosted items can rise into topK
  std::sort(hybridRes.begin(), hybridRes.end(), [](const SearchResult &a, const SearchResult &b) {
    return std::tie(b.fusedScore, a.sourceId, a.chunkId) < std::tie(a.fusedScore, b.sourceId, b.chunkId);
    });
  return hybridRes;
}

std::vector<SearchResult> App::grepSearch(const std::string &query, size_t topK, const std::vector<std::string> &preExtractedTerms) const
{
  std::vector<std::string> terms = preExtractedTerms.empty() ? utils::extractGrepTerms(query) : preExtractedTerms;
  if (terms.empty()) {
    std::string trimmed = utils::trimmed(query);
    if (!trimmed.empty()) terms.push_back(std::move(trimmed));
  }
  std::vector<std::string> paths;
  if (0 < db().chunkCount()) {
    for (const auto &f : db().getTrackedFiles()) {
      paths.push_back(f.path);
    }
  }
  SourceProcessor::GrepOptions opts;
  //opts.idfPruning = (0 == db().chunkCount());
  opts.idfPruning = true;
  auto hits = sourceProcessor().grep(terms, paths, opts);
  if (hits.empty()) {
    return {};
  }
  // Keep only the best matching window per file.
  std::unordered_map<std::string, size_t> bestIndexByFile;
  for (size_t i = 0; i < hits.size(); ++i) {
    const auto &hit = hits[i];
    auto [it, inserted] = bestIndexByFile.try_emplace(hit.sourceId, i);
    if (!inserted) {
      const auto &best = hits[it->second];
      if (hit.bm25Score > best.bm25Score ||
        (hit.bm25Score == best.bm25Score && hit.start < best.start)) {
        it->second = i;
      }
    }
  }

  std::vector<SearchResult> bestPerFile;
  bestPerFile.reserve(bestIndexByFile.size());
  for (const auto &[_, idx] : bestIndexByFile) {
    bestPerFile.push_back(hits[idx]);
  }

  std::sort(bestPerFile.begin(), bestPerFile.end(),
    [](const SearchResult &a, const SearchResult &b) {
      if (a.bm25Score != b.bm25Score) return a.bm25Score > b.bm25Score;
      if (a.similarityScore != b.similarityScore) return a.similarityScore > b.similarityScore;
      return a.sourceId < b.sourceId;
    });

  if (bestPerFile.size() > topK) {
    bestPerFile.resize(topK);
  }

  return bestPerFile;
}

const Settings &App::settings() const
{
  return *imp->settings_;
}

Settings &App::refSettings()
{
  return *imp->settings_;
}

const SimpleTokenizer &App::tokenizer() const
{
  return *imp->tokenizer_;
}

const SourceProcessor &App::sourceProcessor() const
{
  return *imp->processor_;
}

const Chunker &App::chunker() const
{
  return *imp->chunker_;
}

const VectorDatabase &App::db() const
{
  return *imp->db_;
}

VectorDatabase &App::db()
{
  return *imp->db_;
}

const AdminAuth &App::auth() const
{
  return *imp->auth_;
}

AdminAuth &App::auth()
{
  return *imp->auth_;
}

const InstanceRegistry &App::registry() const
{
  return *imp->registry_;
}

const MonitorTracker &App::monitor() const
{
  return *imp->monitor_;
}

MonitorTracker &App::monitor()
{
  return *imp->monitor_;
}

bool App::isValidPrivateAppKey(const std::string &appKey)
{
  return appKey == imp->privateAppKey_;
}

void App::requestShutdownAsync()
{
  SignalHandler::requestShutdown();
}

float App::dbSizeMB() const
{
  try {
    auto &ss = *imp->settings_;
    std::string dbPath = ss.databaseSqlitePath();
    if (std::filesystem::exists(dbPath)) {
      auto sizeBytes = std::filesystem::file_size(dbPath);
      return std::round(sizeBytes / (1024.0f * 1024.0f) * 100.f) / 100.f;
    }
  } catch (const std::exception &e) {
    LOG_MSG << "Error getting database size: " << e.what();
  }
  return 0;
}

float App::indSizeMB() const
{
  try {
    auto &ss = *imp->settings_;
    std::string indexPath = ss.databaseIndexPath();
    if (std::filesystem::exists(indexPath)) {
      auto sizeBytes = std::filesystem::file_size(indexPath);
      return std::round(sizeBytes / (1024.0f * 1024.0f) * 100.f) / 100.f;
    }
  } catch (const std::exception &e) {
    LOG_MSG_ERROR << "Error getting index size: " << e.what();
  }
  return 0;
}

size_t App::uptimeSeconds() const
{
  auto now = std::chrono::system_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - imp->appStartTime_);
  return static_cast<size_t>(duration.count());
}

size_t App::startTimestamp() const
{
  auto timeT = std::chrono::system_clock::to_time_t(imp->appStartTime_);
  return static_cast<size_t>(timeT);
}

size_t App::lastUpdateTimestamp() const
{
  return static_cast<size_t>(std::chrono::system_clock::to_time_t(imp->lastUpdateTime_));
}

void App::printUsage()
{
  std::cout << "Usage: phenixcode-core <command> [options]\n\n";
  std::cout << "Commands:\n";
  std::cout << "  embed              - Process and embed all configured sources\n";
  std::cout << "  update             - Incrementally update changed files only\n";
  std::cout << "  watch [--interval seconds]    - Continuously monitor and update (default: 60s)\n";
  std::cout << "  search <query>     - Search for similar chunks\n";
  std::cout << "  stats              - Show database statistics\n";
  std::cout << "  clear              - Clear all data\n";
  std::cout << "  compact            - Reclaim deleted space\n";
  std::cout << "  chat               - Chat mode\n";
  std::cout << "  serve [options]    - Start HTTP API server\n";
  std::cout << "  providers [--test openai]   - List (or test) embedding and completion providers\n";
  std::cout << "  eval --init <output.json> [--samples N] - Generate evaluation dataset template\n";
  std::cout << "  eval --list        - List available document IDs\n";
  std::cout << "  eval --detail <doc_id> - Show details for a specific document ID\n";
  std::cout << "  eval <dataset.json> [--top <k>] - Run evaluation on a dataset\n";
  std::cout << "\nServe options:\n";
  std::cout << "  --port <port>      - Server port (default: 8081)\n";
  std::cout << "  --watch [--interval seconds]  - Enable auto-update (default: 60s)\n";
  std::cout << "\nGeneral options:\n";
  std::cout << "  --providers-config <path> - Config file path for API proviers (default: ./providers.json)\n";
  std::cout << "  --config <path>    - Config file path (default: settings.json)\n";
  std::cout << "  --top <k>          - Number of results for search (default: 5)\n";
  std::cout << "\nPassword Management:\n";
  std::cout << "  reset-password --pass <pwd> - Reset admin password\n";
  std::cout << "  reset-password-interactive  - Reset password (interactive)\n";
  std::cout << "  password-status             - Check password status\n";
  std::cout << "\nExamples:\n";
  std::cout << "  phenixcode-core serve --port 8081 --watch --interval 30   # Run server and update every 30 seconds\n";
  std::cout << "  phenixcode-core serve --watch    # Use defaults\n";
  std::cout << "  phenixcode-core watch 120    # Watch mode without server\n";
  std::cout << std::endl;

  std::cout << std::endl;
}

json App::sourceStats() const
{
  return imp->statsCache_.getStats(*imp->db_);
}
