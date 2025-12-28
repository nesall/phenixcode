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
#include <iostream>
#include <fstream>
#include <algorithm>
#include <filesystem>
#include <vector>
#include <sstream>
#include <ctime>
#include <cmath>
#include <iomanip>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <stdexcept>
#include <chrono>
#include <ranges>
//#include <format>
#include <cctype>
#include <cassert>
#include <csignal>
#include <nlohmann/json.hpp>
#include <utils_log/logger.hpp>
#include "3rdparty/CLI11.hpp"
#include "3rdparty/fmt/core.h"
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

std::string utils::currentTimestamp()
{
  auto now = std::chrono::system_clock::now();
  auto time = std::chrono::system_clock::to_time_t(now);
  std::stringstream ss;
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &time);
#else
  localtime_r(&time, &tm);
#endif
  ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
  return ss.str();
}

#if 0
time_t utils::getFileModificationTime(const std::string &path)
{
  //auto ftime = fs::last_write_time(path);
  //auto sctp = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
  //return std::chrono::system_clock::to_time_t(sctp);
  // Implementation compatible with C++17
  auto ftime = fs::last_write_time(path);
  using namespace std::chrono;
  auto sctp = time_point_cast<system_clock::duration>(
    ftime - fs::file_time_type::clock::now() + system_clock::now()); // Race timing drift bug because of the two `nows`
  return system_clock::to_time_t(sctp);
}
#endif

// Returns deterministic UTC seconds since Unix epoch.
time_t utils::getFileModificationTime(const std::string &path)
{
#if defined(_WIN32)

  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data))
    return 0;

  ULARGE_INTEGER ull;
  ull.LowPart = data.ftLastWriteTime.dwLowDateTime;
  ull.HighPart = data.ftLastWriteTime.dwHighDateTime;

  // Convert Windows FILETIME (100ns ticks since 1601) to Unix epoch seconds
  constexpr uint64_t WINDOWS_TO_UNIX_EPOCH = 116444736000000000ULL;
  return static_cast<time_t>((ull.QuadPart - WINDOWS_TO_UNIX_EPOCH) / 10000000ULL);

#else

  struct stat st;
  if (stat(path.c_str(), &st) != 0)
    return 0;

  return static_cast<time_t>(st.st_mtime);

#endif
}


int utils::safeStoI(const std::string &s, int def)
{
  try {
    return std::stoi(s);
  } catch (...) {
  }
  return def;
}

std::string utils::trimmed(std::string_view sv)
{
  auto wsfront = std::find_if_not(sv.begin(), sv.end(), ::isspace);
  auto wsback = std::find_if_not(sv.rbegin(), sv.rend(), ::isspace).base();
  return (wsfront < wsback ? std::string(wsfront, wsback) : std::string{});
}


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

  size_t addEmbedChunks(const std::vector<Chunk> &chunks, size_t batchSize, const EmbeddingClient &ec, VectorDatabase &db, std::string_view prependlabelFmt) {
    size_t totalTokens = 0;
    size_t iBatch = 1;
    const size_t nofBatches = static_cast<size_t>(std::ceil(chunks.size() / double(batchSize)));
    for (size_t i = 0; i < chunks.size(); i += batchSize) {
      size_t end = (std::min)(i + batchSize, chunks.size());
      std::vector<Chunk> batch(chunks.begin() + i, chunks.begin() + end);
      std::vector<std::vector<float>> embeddings;
      std::vector<std::string> texts;
      for (const auto &chunk : batch) {
        auto text = chunk.text;
        if (!prependlabelFmt.empty()) {
          std::string info;
          try {
            info = std::filesystem::path(chunk.docUri).filename().string();
          } catch (...) {
            info = chunk.docUri;
          }
          auto label = fmt::vformat(prependlabelFmt, fmt::make_format_args(info));
          text = label + "\n\n" + text;
        }
        texts.push_back(std::move(text));
        totalTokens += chunk.metadata.tokenCount;
      }
      std::cout << "GENERATING embeddings for batch " << iBatch++ << "/" << nofBatches << "\r" << std::flush;
      ec.generateEmbeddings(texts, embeddings, EmbeddingClient::EncodeType::Document);
      db.addDocuments(batch, embeddings);
      std::cout << "  Processed all chunks.                     \r" << std::flush;
    }
    return totalTokens;
  }


  class IncrementalUpdater {
  private:
    App &app_;
    VectorDatabase *db_;
    size_t batchSize_;
    // Failure tracking
    std::unordered_map<std::string, int> failureCounts_;
    std::unordered_set<std::string> ignoredFiles_;

  public:
    IncrementalUpdater(App *app, size_t batchSize) : app_(*app), db_(&app->db()), batchSize_(batchSize) {
    }

    ~IncrementalUpdater() {
    }

    struct UpdateInfo {
      std::vector<std::string> newFiles;
      std::vector<std::string> modifiedFiles;
      std::vector<std::string> deletedFiles;
      std::vector<std::string> unchangedFiles;
    };

    UpdateInfo detectChanges(const std::vector<std::string> &currentFiles) {
      UpdateInfo info;
      auto trackedFiles = db_->getTrackedFiles();
      std::unordered_map<std::string, FileMetadata> trackedMap;
      for (const auto &meta : trackedFiles) {
        trackedMap[meta.path] = meta;
      }
      for (const auto &filepath : currentFiles) {
        if (shouldIgnore(filepath)) {
          LOG_MSG << "Skipping ignored file: " << filepath;
          continue;
        }
        if (!fs::exists(filepath)) continue;
        auto currentModTime = utils::getFileModificationTime(filepath);
        auto currentSize = fs::file_size(filepath);
        auto it = trackedMap.find(filepath);
        if (it == trackedMap.end()) {
          info.newFiles.push_back(filepath);
        } else {
          if (it->second.lastModified != currentModTime || it->second.fileSize != currentSize) {
            info.modifiedFiles.push_back(filepath);
          } else {
            info.unchangedFiles.push_back(filepath);
          }
          trackedMap.erase(it);
        }
      }

      // Remaining files in trackedMap are deleted
      for (const auto &[path, _] : trackedMap) {
        info.deletedFiles.push_back(path);
      }

      return info;
    }

    bool needsUpdate(const UpdateInfo &info) {
      return !info.newFiles.empty() || !info.modifiedFiles.empty() || !info.deletedFiles.empty();
    }

    // Update database incrementally
    size_t updateDatabase(EmbeddingClient &client, Chunker &chunker, const UpdateInfo &info) {
      size_t totalUpdated = 0;
      if (!info.deletedFiles.empty()) {
        try {
          db_->beginTransaction();
          for (const auto &filepath : info.deletedFiles) {
            LOG_MSG << "Deleting chunks for:" << filepath;
            db_->deleteDocumentsBySource(filepath);
            db_->removeFileMetadata(filepath);
            totalUpdated++;
          }
          db_->commit();
        } catch (const std::exception &e) {
          db_->rollback();
          LOG_MSG << "  Error during deletions:" << e.what();
          return totalUpdated;
        }
      }

      // Handle modifications (delete old, insert new)
      for (const auto &filepath : info.modifiedFiles) {
        if (shouldIgnore(filepath)) continue; // Skip ignored files (shouldn't happen due to detectChanges, but safety check)
        LOG_MSG << "Updating:" << filepath;
        try {
          db_->beginTransaction();
          db_->deleteDocumentsBySource(filepath);
          std::string content;
          SourceProcessor::readFile(filepath, content);
          if (content.empty()) {
            LOG_MSG << "  Empty file" << filepath << ".Skipped.";
            db_->rollback();
            continue;
          }
          auto chunks = chunker.chunkText(content, filepath);
          addEmbedChunks(chunks, batchSize_, client, *db_, app_.settings().embeddingPrependLabelFormat());
          totalUpdated++;
          clearFailure(filepath);
          LOG_MSG << "  Updated with" << chunks.size() << " chunks";
          db_->commit();
          db_->persist();
        } catch (const std::exception &e) {
          db_->rollback();
          LOG_MSG << "  Error:" << e.what();
          recordFailure(filepath);
        }
      }

      // Handle new files
      for (const auto &filepath : info.newFiles) {
        if (shouldIgnore(filepath)) continue;
        LOG_MSG << "Adding new file:" << filepath;
        try {
          db_->beginTransaction();
          std::string content;
          SourceProcessor::readFile(filepath, content);
          if (content.empty()) {
            LOG_MSG << "  Empty file" << filepath << ".Skipped.";
            db_->rollback();
            continue;
          }
          auto chunks = chunker.chunkText(content, filepath);
          addEmbedChunks(chunks, batchSize_, client, *db_, app_.settings().embeddingPrependLabelFormat());
          totalUpdated++;
          clearFailure(filepath);
          LOG_MSG << "  Added with" << chunks.size() << " chunks";
          db_->commit();
          db_->persist();
        } catch (const std::exception &e) {
          LOG_MSG << "  Error:" << e.what();
          db_->rollback();
          recordFailure(filepath);
        }
      }

      if (0 < totalUpdated) {
        db_->persist();
      }
      return totalUpdated;
    }

    void printUpdateSummary(const UpdateInfo &info) {
      std::cout << "\n=== Update Summary ===" << std::endl;
      std::cout << "New files: " << info.newFiles.size() << std::endl;
      std::cout << "Modified files: " << info.modifiedFiles.size() << std::endl;
      std::cout << "Deleted files: " << info.deletedFiles.size() << std::endl;
      std::cout << "Unchanged files: " << info.unchangedFiles.size() << std::endl;

      if (!info.newFiles.empty()) {
        std::cout << "\nNew:" << std::endl;
        for (const auto &file : info.newFiles) {
          std::cout << "  + " << file << std::endl;
        }
      }

      if (!info.modifiedFiles.empty()) {
        std::cout << "\nModified:" << std::endl;
        for (const auto &file : info.modifiedFiles) {
          std::cout << "  * " << file << std::endl;
        }
      }

      if (!info.deletedFiles.empty()) {
        std::cout << "\nDeleted:" << std::endl;
        for (const auto &file : info.deletedFiles) {
          std::cout << "  - " << file << std::endl;
        }
      }
    }

    bool shouldIgnore(const std::string &filepath) const {
      return ignoredFiles_.count(filepath) > 0;
    }

    void recordFailure(const std::string &filepath) {
      failureCounts_[filepath]++;
      if (failureCounts_[filepath] >= 3) {
        ignoredFiles_.insert(filepath);
        LOG_MSG << "Added to ignore list after 3 failures: " << filepath;
      }
    }

    void clearFailure(const std::string &filepath) {
      failureCounts_.erase(filepath);
    }
  };

  class SignalHandler {
  private:
    static volatile std::sig_atomic_t shutdownRequested;

  public:
    static void handleSignal(int) {
      shutdownRequested = 1;
    }

    static bool shouldShutdown() {
      return shutdownRequested != 0;
    }

    static void requestShutdown() {
      shutdownRequested = 1;
    }

    static void setup() {
      // Only use standard C++ signal function
      std::signal(SIGINT, handleSignal);
      std::signal(SIGTERM, handleSignal);

      // Note: SIGKILL cannot be caught on any platform
      // Note: Some signals are platform-specific
    }
  };
  volatile std::sig_atomic_t SignalHandler::shutdownRequested = 0;

  std::string detectLanguage(const std::string &path) {
    std::string ext = std::filesystem::path(path).extension().string();

    static const std::map<std::string, std::string> ext_map = {
        {".cpp", "C++"}, {".hpp", "C++"}, {".h", "C++"},
        {".c", "C"},
        {".py", "Python"},
        {".js", "JavaScript"}, {".ts", "TypeScript"},
        {".java", "Java"},
        {".go", "Go"},
        {".rs", "Rust"},
        {".md", "Markdown"}, {".txt", "Text"}
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
          {"path", file.path},
          {"lines", lines},
          {"size_bytes", size},
          {"language", lang},
          {"chunks", chunkCount},
          {"last_modified", file.lastModified}
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
        {"total_files", trackedFiles.size()},
        {"total_lines", totalLines},
        {"total_size_bytes", totalSize},
        {"by_language", byLanguage},
        {"by_directory", byDirectory},
        {"top_files", ar}
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

} // anonymous namespace


struct App::Impl {
  std::unique_ptr<Settings> settings_;
  std::unique_ptr<AdminAuth> auth_;
  std::unique_ptr<VectorDatabase> db_;
  std::unique_ptr<SimpleTokenizer> tokenizer_;
  std::unique_ptr<Chunker> chunker_;
  std::unique_ptr<SourceProcessor> processor_;
  std::unique_ptr<IncrementalUpdater> updater_;
  std::unique_ptr<HttpServer> httpServer_;

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
}

void App::initialize(/*const std::string &configPath*/)
{
  assert(imp->settings_);
  //imp->settings_ = std::make_unique<Settings>(configPath);
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
  imp->updater_ = std::make_unique<IncrementalUpdater>(this, imp->settings_->embeddingBatchSize());

  imp->httpServer_ = std::make_unique<HttpServer>(*this);
}

bool App::testSettings() const
{
  bool ok = true;
  ApiConfig api;
  try {
    {
      api = settings().embeddingCurrentApi();
      LOG_MSG << "Testing embedding client" << api.model;
      LOG_MSG << "  document format - '" << utils_log::LOGNOSPACE << api.documentFormat << "'";
      LOG_MSG << "  query format - '" << utils_log::LOGNOSPACE << api.queryFormat << "'";
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
      LOG_MSG << "  Embedding client works fine." << "[ l2norm" << l2Norm << "]";
      LOG_MSG << "  Testing similarities:";

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

      LOG_MSG << "    A0-A1 (similar):    " << simA0A1 << yesNo(simA0A1, 0.8f, 0.95f);
      LOG_MSG << "    A0-B0 (different):  " << simA0B0 << yesNo(simA0B0, 0.3f, 0.7f);
      LOG_MSG << "    A0-B1 (typo):       " << simA0B1 << yesNo(simA0B1, 0.3f, 0.7f);
      LOG_MSG << "    B0-B1 (similar):    " << simB0B1 << yesNo(simB0B1, 0.6f, 0.9f);
      LOG_MSG << "    A0-C0 (different):  " << simA0C0 << yesNo(simA0C0, 0.1f, 0.5f);
      LOG_MSG << "    Doc-Query (Similar):" << simSelf << yesNo(simSelf, 0.75f, 0.95f);
    }
    {
      api = {};
      api = settings().generationCurrentApi();
      LOG_MSG << "\nTesting completion client" << api.model;
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
        LOG_MSG << "  Completion client works fine.";
      } else {
        throw std::runtime_error("Completion client not working");
      }
    }
  } catch (const std::exception &ex) {
    LOG_MSG << " " << ex.what();
    LOG_MSG << " [" << api.apiUrl << "]";
    LOG_MSG << " [" << api.model << "]";
    LOG_MSG << "  Please, check settings file and edit it manually if needed.";
    LOG_MSG << "   [ settings file path" << std::filesystem::absolute(settings().configPath()) << "]";
    ok = false;
  }
  return ok;
}

void App::embed(bool noPrompt)
{
  LOG_MSG << "Starting embedding process...";
  auto sources = imp->processor_->collectSources(false);
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

  //LOG_MSG << "Sources with empty text: " << emptyTextCount;

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
        LOG_MSG << "File not found: " << source << ". Skipped.";
        skippedFiles++;
        continue;
      }
      if (imp->db_->fileExistsInMetadata(source)) {
        LOG_MSG << "Duplicate source" << source << ". Skipped.";
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

      auto chunks = imp->chunker_->chunkText(content, sourceId);

      LOG_MSG << "  Generated" << chunks.size() << "chunks";
      const size_t batchSize = imp->settings_->embeddingBatchSize();
      totalTokens += addEmbedChunks(chunks, batchSize, embeddingClient, *imp->db_, settings().embeddingPrependLabelFormat());
      std::cout << std::endl;
      totalChunks += chunks.size();
      totalFiles++;
      imp->db_->commit();
      imp->db_->persist();
    } catch (const std::exception &e) {
      imp->db_->rollback();
      skippedFiles++;
      LOG_MSG << "Error processing" << source << ": " << e.what();
    }
  }
  imp->db_->persist();
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

void App::search(const std::string &query, size_t topK)
{
  std::cout << "Searching for: " << query << std::endl;

  EmbeddingClient embeddingClient{ settings().embeddingCurrentApi(), settings().embeddingTimeoutMs() };
  std::vector<float> queryEmbedding;
  embeddingClient.generateEmbeddings(query, queryEmbedding, EmbeddingClient::EncodeType::Query);
  auto results = imp->db_->search(queryEmbedding, topK);

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

void App::stats()
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

void App::chat()
{
  auto apiCfg = imp->settings_->generationCurrentApi();
  std::cout << "Using model: " << apiCfg.model << " at " << apiCfg.apiUrl << std::endl;
  std::cout << "Entering chat mode. Type 'exit' to quit." << std::endl;
  std::vector<json> messages;
  messages.push_back({ {"role", "system"}, {"content", "You are a helpful assistant."} });
  EmbeddingClient embeddingClient{ settings().embeddingCurrentApi(), settings().embeddingTimeoutMs() };
  CompletionClient completionClient{ apiCfg, settings().generationTimeoutMs(), *this };

  while (true) {
    try {
      std::cout << "\nYou: ";
      std::string userInput;
      std::getline(std::cin, userInput);
      if (userInput == "exit") break;
      messages.push_back({ {"role", "user"}, {"content", userInput} });
      // Generate embedding for the user input
      std::vector<float> queryEmbedding;
      embeddingClient.generateEmbeddings(userInput, queryEmbedding, EmbeddingClient::EncodeType::Query);
      auto searchResults = imp->db_->search(queryEmbedding, 5);
      std::cout << "\nAssistant: " << std::flush;
      std::string assistantResponse = completionClient.generateCompletion(
        messages, searchResults, 0.0f, settings().generationDefaultMaxTokens(),
        [](const std::string &chunk) {
          std::cout << chunk << std::flush;
        }
      );
      std::cout << std::endl;
      messages.push_back({ {"role", "assistant"}, {"content", assistantResponse} });
    } catch (const std::exception &e) {
      std::cout << "Error: " << e.what() << "\n";
    }
  }
  std::cout << "Exiting chat mode." << std::endl;
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
            LOG_MSG << "Warning: Unable to open info file for writing: " << infoFile;
          }
        } catch (const std::exception &e) {
          LOG_MSG << "Warning: Error saving instance info: " << e.what();
        }
      }
      if (0 < newPort) {
        imp->registry_ = std::make_unique<InstanceRegistry>(newPort, watch ? interval : 0, settings());
        try {
          imp->registry_->startHeartbeat();
          LOG_MSG << "\nStarting HTTP API server on port " << newPort << "...";
          imp->httpServer_->startServer();
        } catch (const std::exception &e) {
          LOG_MSG << "[ERROR] Failed to register instance: " << e.what();
          SignalHandler::requestShutdown();
        }
      } else {
        LOG_MSG << "\nHTTP server was unable to bind to ports in the range [" << suggestedPort << "to" << newPort << "]";
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

void App::providers(const std::string &testProvider)
{
  auto vc = settings().generationApis();

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
    Settings settingsNew{ imp->settings_->configPath() };
    imp->processor_->setSettings(settingsNew);
    LOG_MSG << "Read settings from" << settingsNew.configPath();
  } catch (const std::exception &ex) {
    LOG_MSG << ex.what();
    LOG_MSG << "Unable to re-read settings. Skipped.";
  }
  auto sources = imp->processor_->collectSources(false);
  std::vector<std::string> currentFiles;
  for (const auto &source : sources) {
    currentFiles.push_back(source.source);
  }
  auto info = imp->updater_->detectChanges(currentFiles);
  imp->updater_->printUpdateSummary(info);
  if (!imp->updater_->needsUpdate(info)) {
    LOG_MSG << "No updates needed. Database is up to date.";
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
    LOG_MSG << "Error getting index size: " << e.what();
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
  std::cout << "Usage: embedder <command> [options]\n\n";
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
  std::cout << "\nServe options:\n";
  std::cout << "  --port <port>      - Server port (default: 8081)\n";
  std::cout << "  --watch [--interval seconds]  - Enable auto-update (default: 60s)\n";
  std::cout << "\nGeneral options:\n";
  std::cout << "  --config <path>    - Config file path (default: settings.json)\n";
  std::cout << "  --top <k>          - Number of results for search (default: 5)\n";
  std::cout << "\nPassword Management:\n";
  std::cout << "  reset-password --pass <pwd> - Reset admin password\n";
  std::cout << "  reset-password-interactive  - Reset password (interactive)\n";
  std::cout << "  password-status             - Check password status\n";
  std::cout << "\nExamples:\n";
  std::cout << "  embedder serve --port 8081 --watch --interval 30   # Run server and update every 30 seconds\n";
  std::cout << "  embedder serve --watch    # Use defaults\n";
  std::cout << "  embedder watch 120    # Watch mode without server\n";
  std::cout << std::endl;

  std::cout << std::endl;
}

namespace {
  void findPlaceholders(const json &j, std::set<std::string> &placeholders) {
    if (j.is_object()) {
      for (const auto &[k, v] : j.items())
        findPlaceholders(v, placeholders);
    } else if (j.is_array()) {
      for (const auto &el : j)
        findPlaceholders(el, placeholders);
    } else if (j.is_string()) {
      std::string val = j.get<std::string>();
      if (val.rfind("_PL_", 0) == 0)
        placeholders.insert(val);
    }
  }

  void replacePlaceholders(json &j, const std::map<std::string, std::string> &values) {
    if (j.is_object()) {
      for (auto &[k, v] : j.items())
        replacePlaceholders(v, values);
    } else if (j.is_array()) {
      for (auto &el : j)
        replacePlaceholders(el, values);
    } else if (j.is_string()) {
      std::string val = j.get<std::string>();
      if (auto it = values.find(val); it != values.end())
        j = it->second;
    }
  }

  std::string promptPassword(const std::string &prompt) {
    std::cout << prompt;
    std::string password;

#ifdef _WIN32
    // Windows: Hide input
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode;
    GetConsoleMode(hStdin, &mode);
    SetConsoleMode(hStdin, mode & (~ENABLE_ECHO_INPUT));
    std::getline(std::cin, password);
    SetConsoleMode(hStdin, mode);
#else
    // Linux/Mac: Use termios
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    std::getline(std::cin, password);
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
#endif

    std::cout << "\n";
    return password;
  }

} // anonymous namespace

json App::sourceStats() const
{
  return imp->statsCache_.getStats(*imp->db_);
}

//std::string App::describeProjectTitle() const
//{
//  if (imp->projectTitle_.empty()) {
//    //auto api = settings().generationCurrentApi();
//    //CompletionClient cl{ api, settings().generationTimeoutMs(), *this };
//    //std::vector<json> messages;
//    //messages.push_back({ {"role", "system"}, {"content", "You are a helpful assistant."} });
//    //messages.push_back({ {"role", "user"}, {"content", "Give a 2 to 5 word description title based on sources being tracked."} });
//    //std::string fullResponse = cl.generateCompletion(
//    //  messages, {}, 0.0f, settings().generationDefaultMaxTokens(),
//    //  [](const std::string &chunk) {
//    //    //std::cout << chunk << std::flush;
//    //  }
//    //);
//    //imp->projectTitle_ = fullResponse;
//    imp->projectTitle_ = settings().getProjectTitle();
//  }
//  return imp->projectTitle_;
//}

std::string App::runSetupWizard(AdminAuth &auth)
{
  std::cout << "\n";
  std::cout << "|=========================================|\n";
  std::cout << "|   Embedder RAG - Configuration Wizard   |\n";
  std::cout << "|=========================================|\n\n";

  // Check password status
  if (auth.isDefaultPassword()) {
    std::cout << "  SECURITY WARNING\n";
    std::cout << "You are using the default admin password.\n";
    std::cout << "Would you like to change it now? [y/N]: ";

    std::string response;
    std::getline(std::cin, response);

    if (response.empty() || response == "y" || response == "Y") {
      std::string password = promptPassword("Enter new password (min 8 chars): ");
      std::string confirm = promptPassword("Confirm password: ");

      if (password == confirm && password.length() >= 8 && password != "admin") {
        auth.setPassword(password);
        std::cout << " Password changed successfully\n\n";
      } else {
        std::cout << " Password change failed. Using default password.\n";
        std::cout << fmt::format("You can change it later with: {} reset-password\n\n", Impl::binaryName_);
      }
    }
  }

  LOG_MSG << "Creating default settings.json file";
  LOG_MSG << "Reading template settings.json file...";

  std::ifstream f("settings.template.json");
  if (!f.is_open()) {
    f.open("../settings.template.json");
    if (!f.is_open()) {
      f.open("../../settings.template.json");
      if (!f.is_open()) {
        throw std::runtime_error("Cannot open settings.template.json file");
      }
    }
  }
  nlohmann::json j;
  f >> j;
  std::string str = j.dump();

  json descriptions;
  if (j.contains("placeholder_descriptions")) {
    descriptions = j["placeholder_descriptions"];
    j.erase("placeholder_descriptions");
  }

  std::set<std::string> placeholders;
  findPlaceholders(j, placeholders);

  std::map<std::string, std::string> values;
  LOG_MSG << "Detected configuration placeholders:\n";

  for (const auto &ph : placeholders) {
    std::string prompt = descriptions.count(ph) ? descriptions[ph].get<std::string>() : ph;
    LOG_MSG << "Enter" << prompt << " (" << ph << "): ";
    std::string val;
    std::getline(std::cin, val);
    values[ph] = val;
  }

  replacePlaceholders(j, values);

  LOG_MSG << "\nSource directories to index (one per line, empty to finish):\n";
  std::vector<json> sources;
  while (true) {
    std::cout << "  Path: ";
    std::string path;
    std::cin >> path;
    if (path.empty()) break;
    if (!std::filesystem::exists(path)) {
      std::cout << "Path entered does not exist. Do you want to keep it [y/N]: ";
      std::string yn;
      std::cin >> yn;
      if (yn != "y") continue;
    }
    json item;
    item["type"] = "directory";
    item["path"] = path;
    item["recursive"] = true;
    j["source"]["paths"].push_back(item);
  }

  std::filesystem::path path("settings.json");
  std::ofstream out(path);
  out << std::setw(2) << j;
  LOG_MSG << "\nConfiguration saved to settings.json\n";
  std::cout << "\nNext steps:\n";
  std::cout << "  1. Review settings.json (optional)\n";
  std::cout << fmt::format("  2. Run: {} embed\n", Impl::binaryName_);
  std::cout << fmt::format("  3. Start server: {} serve\n", Impl::binaryName_);
  std::cout << "  or install as service: scripts/install-service\n\n";

  return path.string();
}

std::string App::findConfigFile(const std::string &filename)
{
  std::vector<std::string> searchPaths = {
      filename,                          // Current dir
      "../" + filename,                  // Parent
      "../../" + filename,               // Grandparent
      std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") + "/.config/embedder/" + filename,  // User config
      "/etc/embedder/" + filename        // System-wide (Linux)
  };

  for (const auto &path : searchPaths) {
    if (std::filesystem::exists(path)) {
      //LOG_MSG << "Found config at:" << path;
      return path;
    }
  }

  // Not found, run wizard
  LOG_MSG << "Config file not found, running setup wizard...";
  AdminAuth auth;
  return runSetupWizard(auth);
}

int App::handleInteractivePasswordReset()
{
  std::cout << "===================================\n";
  LOG_MSG << "   Reset Admin Password             ";
  std::cout << "===================================\n\n";

  int nofTries = 3;
  std::string newPassword;
  do {
    newPassword = promptPassword("Enter new password (min 8 chars): ");
    std::string confirm = promptPassword("Confirm password: ");
    if (newPassword != confirm) {
      LOG_MSG << "Error: Passwords do not match\n";
      continue;
    }
    if (newPassword.length() < 8) {
      LOG_MSG << "Error: Password must be at least 8 characters\n";
      continue;
    }
    if (newPassword == "admin") {
      LOG_MSG << "Error: Cannot use 'admin' as password\n";
      continue;
    }
    break;
  } while (--nofTries);

  if (0 == nofTries) {
    LOG_MSG << "Unable to reset admin password. Exiting.";
    return 1;
  }

  AdminAuth auth;
  auth.setPassword(newPassword);
  LOG_MSG << "\nPassword updated successfully!";
  return 0;
}

int App::handlePasswordStatus()
{
  AdminAuth auth;
  std::cout << "Admin Password Status:\n";
  std::cout << "-------------------------\n";
  if (auth.isDefaultPassword()) {
    LOG_MSG << "Status: Using default password 'admin'\n";
    LOG_MSG << "  WARNING: Please change the default password!\n";
    LOG_MSG << fmt::format("Run: {} reset-password --pass <your_password>\n", Impl::binaryName_);
  } else {
    LOG_MSG << "Status: Custom password set \n";
    LOG_MSG << "Last modified: " << auth.fileLastModifiedTime() << "\n";
  }
  return 0;
}

int App::run(int argc, char *argv[])
{
  Impl::binaryName_ = argv[0];

  SignalHandler::setup();

  CLI::App app{ "PhenixCode RAG System" };
  app.set_version_flag("--version,-v", 
    fmt::format("PhenixCode RAG System\nAuthor: Arman Sahakyan\nVersion: {}\nBuild date: {} {}", EMBEDDER_VERSION, __DATE__, __TIME__));
  app.require_subcommand(0, 1); // Allow 0 or 1 subcommand (0 shows help)

  std::string configPath = "settings.json";
  app.add_option("-c,--config", configPath, "Config file path")->envname("EMBEDDER_CONFIG")->check(CLI::ExistingFile);

  bool noStartupTests = false;
  app.add_flag("--no-startup-tests", noStartupTests, "Skip startup model test calls");

  // Password management commands
  auto cmdValidatePass = app.add_subcommand("validate-password", "Validate admin password");
  std::string validPassword;
  cmdValidatePass->add_option("--pass", validPassword, "Valid password")->required();

  auto cmdResetPass = app.add_subcommand("reset-password", "Reset admin password");
  std::string newPassword;
  cmdResetPass->add_option("--pass", newPassword, "New password")
    ->required()
    ->check(CLI::IsMember({ "admin" }, CLI::ignore_case).description("Cannot be 'admin'"));  
  auto cmdResetInteractive = app.add_subcommand("reset-password-interactive", "Reset password interactively");
  auto cmdPasswordStatus = app.add_subcommand("password-status", "Check password status");

  auto passwordGroup = app.add_option_group("password", "Password Management");
  passwordGroup->add_subcommand(cmdResetPass);
  passwordGroup->add_subcommand(cmdResetInteractive);
  passwordGroup->add_subcommand(cmdPasswordStatus);
  passwordGroup->require_option(0, 1); // Max one password command

  // Main commands
  auto cmdEmbed = app.add_subcommand("embed", "Process and embed all configured sources");
  bool embedForce = false;
  cmdEmbed->add_flag("--force", embedForce, "Force re-embed all files");

  auto cmdUpdate = app.add_subcommand("update", "Incrementally update changed files only");

  auto cmdWatch = app.add_subcommand("watch", "Continuously monitor and update");
  int watchInterval = 60;
  cmdWatch->add_option("--interval", watchInterval, "Update interval in seconds")->default_val(60);

  auto cmdSearch = app.add_subcommand("search", "Search for similar chunks");
  std::string searchQuery;
  size_t searchTopk = 5;
  cmdSearch->add_option("query", searchQuery, "Search query")->required();
  cmdSearch->add_option("--top", searchTopk, "Number of results")->default_val(5);

  auto cmdStats = app.add_subcommand("stats", "Show database statistics");

  auto cmdClear = app.add_subcommand("clear", "Clear all data");
  bool clearNoConfirm = false;
  cmdClear->add_flag("--yes,-y", clearNoConfirm, "Skip confirmation prompt");

  auto cmdCompact = app.add_subcommand("compact", "Reclaim deleted space");

  auto cmdChat = app.add_subcommand("chat", "Chat mode");

  auto cmdServe = app.add_subcommand("serve", "Start HTTP API server");
  bool serveNoConfirm = false;
  cmdServe->add_flag("--yes,-y", serveNoConfirm, "Skip confirmation prompt");

  int servePort = 8590;
  bool serveWatch = false;
  int serveWatchInterval = 60;
  std::string privateAppKey;
  std::string infoFile;
  cmdServe->add_option("-p,--port", servePort, "Server port")
    ->default_val(8590)
    ->envname("EMBEDDER_PORT")
    ->check(CLI::Range(0, 65535));
  cmdServe->add_flag("--watch", serveWatch, "Enable auto-update");
  cmdServe->add_option("--interval", serveWatchInterval, "Watch interval in seconds")->default_val(60);
  cmdServe->add_option("--appkey", privateAppKey, "Caller-provided key for privileged operations (e.g., exit requests through the instance registry)");
  cmdServe->add_option("--info-file", infoFile, "Path to store instance info such as port server started");

  auto cmdProviders = app.add_subcommand("providers", "List embedding and completion providers");
  std::string testProvider;
  cmdProviders->add_option("--test", testProvider, "Test call to a given provider");

  try {
    app.parse(argc, argv);

    configPath = findConfigFile(configPath);
    std::unique_ptr<Settings> settings;
    try {
      settings = std::make_unique<Settings>(configPath);
      settings->initProjectIdIfMissing(true);
      settings->initProjectTitleIfMissing(true);
    } catch (const std::exception &ex) {
      LOG_MSG << ex.what();
      std::cerr << "Unable to read settings file " << configPath << "\n";
      throw;
    }
    SET_LOG_OUTPUT_FILE_PATH(settings->loggingLoggingFile());
    SET_LOG_DIAGNOSTICS_FILE_PATH(settings->loggingDiagnosticsFile());
    SET_LOG_TO_FILE(settings->loggingLogToFile());
    SET_LOG_TO_CONSOLE(settings->loggingLogToConsole());
    LOG_MSG << "Build Date:" << __DATE__ << __TIME__;
    LOG_MSG << "Read settings from" << std::filesystem::absolute(configPath);

    // Handle password commands first (no app initialization needed)
    if (cmdValidatePass->parsed()) {
      AdminAuth auth;
      std::string jwtToken;
      if (auth.authenticate({ validPassword, "Basic" }, jwtToken)) {
        LOG_MSG << "Password is valid";
      } else {
        LOG_MSG << "Invalid password";
      }
      return 0;
    }

    if (cmdResetPass->parsed()) {
      AdminAuth auth;
      if (newPassword.length() < 8) {
        LOG_MSG << "Error: Password must be at least 8 characters";
        return 1;
      }
      if (newPassword == "admin") {
        LOG_MSG << "Error: Cannot use 'admin' as password";
        return 1;
      }
      auth.setPassword(newPassword);
      LOG_MSG << "Admin password has been reset";
      return 0;
    }

    if (cmdResetInteractive->parsed()) {
      return handleInteractivePasswordReset();
    }

    if (cmdPasswordStatus->parsed()) {
      return handlePasswordStatus();
    }

    App appInstance;
    appInstance.imp->settings_ = std::move(settings);
    appInstance.imp->privateAppKey_ = std::move(privateAppKey);
    appInstance.initialize();

    if (!noStartupTests) {
      if (!appInstance.testSettings()) {
        LOG_MSG << "Wrong/incomplete settings. Exiting.";
        return 1;
      }
    }

    // Handle main commands
    if (cmdEmbed->parsed()) {
      appInstance.embed(embedForce);
    } else if (cmdUpdate->parsed()) {
      appInstance.update();
    } else if (cmdWatch->parsed()) {
      appInstance.watch(watchInterval);
    } else if (cmdSearch->parsed()) {
      appInstance.search(searchQuery, searchTopk);
    } else if (cmdStats->parsed()) {
      appInstance.stats();
    } else if (cmdClear->parsed()) {
      appInstance.clear(clearNoConfirm);
    } else if (cmdCompact->parsed()) {
      appInstance.compact();
    } else if (cmdChat->parsed()) {
      appInstance.chat();
    } else if (cmdProviders->parsed()) {
      appInstance.providers(testProvider);
    } else if (cmdServe->parsed()) {
      if (!serveNoConfirm && appInstance.auth().isDefaultPassword()) {
        std::cout << "\n  WARNING: You are using the default admin password!\n";
        std::cout << "This is a security risk. Please change it:\n";
        std::cout << "  " << Impl::binaryName_ << " reset-password --pass <new_password>\n\n";
        std::cout << "Continue anyway? [y/N]: ";

        std::string response;
        std::getline(std::cin, response);

        if (response != "y" && response != "Y") {
          LOG_MSG << "Server start cancelled. Please reset password first.";
          return 1;
        }
      }
      appInstance.serve(servePort, serveWatch, serveWatchInterval, infoFile);
    } else {
      // No command specified, show help
      std::cout << app.help() << std::endl;
      return 1;
    }

  } catch (const CLI::ParseError &e) {
    // CLI11 has built-in formatting
    return app.exit(e);
  } catch (const std::filesystem::filesystem_error &e) {
    LOG_MSG << "Filesystem error: " << e.what();
    LOG_MSG << "Path: " << e.path1();
    return 1;
  } catch (const std::exception &e) {
    LOG_MSG << "Error: " << e.what();
    LOG_MSG << "Run with --help for usage information";
    return 1;
  }
  return 0;
}
