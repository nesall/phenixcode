#include "updater.h"
#include "app.h"
#include "cutils.h"
#include "chunker.h"
#include "database.h"
#include "inference.h"
#include "sourceproc.h"
#include "monitor.h"
#include "3rdparty/fmt/format.h"
#include <utils_log/logger.hpp>

#include <exception>
#include <filesystem>
#include <iostream>
#include <unordered_map>

namespace fs = std::filesystem;

size_t updater::addEmbedChunks(
  const std::vector<Chunk> &chunks,
  size_t batchSize,
  const EmbeddingClient &ec,
  VectorDatabase &db,
  std::string_view prependlabelFmt,
  MonitorTracker *monitor = nullptr,
  size_t filesDone = 0,
  size_t filesTotal = 0,
  const Settings *settings = nullptr)
{
  size_t totalTokens = 0;
  size_t iBatch = 1;
  const size_t nofBatches = static_cast<size_t>(std::ceil(chunks.size() / double(batchSize)));
  if (monitor) {
    monitor->addChunksTotal(chunks.size());
  }
  for (size_t i = 0; i < chunks.size(); i += batchSize) {
    size_t end = (std::min)(i + batchSize, chunks.size());
    std::vector<Chunk> batch(chunks.begin() + i, chunks.begin() + end);
    std::vector<std::vector<float>> embeddings;
    std::vector<std::string> texts;
    size_t batchTokens = 0;
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
      batchTokens += chunk.metadata.tokenCount;
    }
    totalTokens += batchTokens;
    std::cout << "GENERATING embeddings for batch " << iBatch << "/" << nofBatches << "\r" << std::flush;

    auto t0 = std::chrono::steady_clock::now();
    ec.generateEmbeddings(texts, embeddings, EmbeddingClient::EncodeType::Document);
    double latencyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    db.addDocuments(batch, embeddings);

    if (monitor) {
      monitor->addChunksDone(batch.size());
      monitor->updateProgress(
        MonitorPhase::Embed,
        filesDone,
        filesTotal,
        0, // preserved or updated via addChunksDone
        0,
        iBatch,
        batch.size());
      monitor->recordRequest("embed", latencyMs, false, batchTokens, 0);
      if (settings) {
        monitor->recordEmbeddingCost(settings->embeddingCurrentApi().id, batchTokens, settings->providers());
      }
    }
    iBatch++;
    std::cout << "  Processed all chunks.                     \r" << std::flush;
  }
  return totalTokens;
}

using namespace updater;

IncrementalUpdater::IncrementalUpdater(App *app, size_t batchSize)
  : app_(*app),
  db_(&app->db()),
  batchSize_(batchSize)
{
}

IncrementalUpdater::UpdateInfo IncrementalUpdater::detectChanges(const std::vector<std::string> &currentFiles)
{

  UpdateInfo info;

  auto trackedFiles = db_->getTrackedFiles();
  std::unordered_map<std::string, FileMetadata> trackedMap;

  for (const auto &meta : trackedFiles) {
    trackedMap[meta.path] = meta;
  }

  for (const auto &filepath : currentFiles) {
    if (shouldIgnore(filepath)) {
      LOG_MSG_WARN << "Skipping ignored file: " << filepath;
      continue;
    }

    if (!fs::exists(filepath)) {
      continue;
    }

    auto currentModTime = utils::getFileModificationTime(filepath);
    auto currentSize = fs::file_size(filepath);

    auto it = trackedMap.find(filepath);

    if (it == trackedMap.end()) {
      info.newFiles.push_back(filepath);
    } else {
      if (it->second.lastModified != currentModTime ||
        it->second.fileSize != currentSize) {
        info.modifiedFiles.push_back(filepath);
      } else {
        info.unchangedFiles.push_back(filepath);
      }

      trackedMap.erase(it);
    }
  }

  for (const auto &[path, _] : trackedMap) {
    info.deletedFiles.push_back(path);
  }

  return info;
}

bool IncrementalUpdater::needsUpdate(const UpdateInfo &info)
{
  return !info.newFiles.empty() ||
    !info.modifiedFiles.empty() ||
    !info.deletedFiles.empty();
}

size_t IncrementalUpdater::updateDatabase(EmbeddingClient &client, Chunker &chunker, const UpdateInfo &info)
{

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
      LOG_MSG_ERROR << "  Error during deletions:" << e.what();
      return totalUpdated;
    }
  }

  const size_t totalWork =
    info.modifiedFiles.size() + info.newFiles.size();

  size_t doneFiles = 0;

  app_.monitor().beginPhase(MonitorPhase::Embed, totalWork, 0);

  // Handle modified files.
  for (const auto &filepath : info.modifiedFiles) {
    if (shouldIgnore(filepath)) {
      continue;
    }

    LOG_MSG << "Updating:" << filepath;

    try {
      db_->beginTransaction();
      db_->deleteDocumentsBySource(filepath);

      std::string content;
      SourceProcessor::readFile(filepath, content);

      if (content.empty()) {
        LOG_MSG_WARN << "  Empty file" << filepath << ".Skipped.";
        db_->rollback();
        continue;
      }

      auto chunks = chunker.chunkText(content, filepath);

      updater::addEmbedChunks(
        chunks,
        batchSize_,
        client,
        *db_,
        app_.settings().embeddingPrependLabelFormat(),
        &app_.monitor(),
        ++doneFiles,
        totalWork,
        &app_.settings());

      totalUpdated++;
      clearFailure(filepath);

      LOG_MSG << "  Updated with" << chunks.size() << " chunks";

      db_->commit();
      db_->persist();
    } catch (const std::exception &e) {
      db_->rollback();
      LOG_MSG_ERROR << "  Error:" << e.what();
      recordFailure(filepath);
    }
  }

  // Handle new files.
  for (const auto &filepath : info.newFiles) {
    if (shouldIgnore(filepath)) {
      continue;
    }

    LOG_MSG << "Adding new file:" << filepath;

    try {
      db_->beginTransaction();

      std::string content;
      SourceProcessor::readFile(filepath, content);

      if (content.empty()) {
        LOG_MSG_WARN << "  Empty file" << filepath << ".Skipped.";
        db_->rollback();
        continue;
      }

      auto chunks = chunker.chunkText(content, filepath);

      addEmbedChunks(
        chunks,
        batchSize_,
        client,
        *db_,
        app_.settings().embeddingPrependLabelFormat(),
        &app_.monitor(),
        ++doneFiles,
        totalWork,
        &app_.settings());

      totalUpdated++;
      clearFailure(filepath);

      LOG_MSG << "  Added with" << chunks.size() << " chunks";

      db_->commit();
      db_->persist();
    } catch (const std::exception &e) {
      LOG_MSG_ERROR << "  Error:" << e.what();
      db_->rollback();
      recordFailure(filepath);
    }
  }

  app_.monitor().finishIndexing(true);
  app_.monitor().persist();

  if (totalUpdated > 0) {
    db_->persist();
  }

  return totalUpdated;
}

void IncrementalUpdater::printUpdateSummary(const UpdateInfo &info)
{
  if (info.newFiles.empty() &&
    info.modifiedFiles.empty() &&
    info.deletedFiles.empty()) {
    std::cout << "No changes detected." << std::endl;
    return;
  }

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

bool IncrementalUpdater::shouldIgnore(const std::string &filepath) const
{
  return ignoredFiles_.count(filepath) > 0;
}

void IncrementalUpdater::recordFailure(const std::string &filepath)
{
  failureCounts_[filepath]++;

  if (failureCounts_[filepath] >= 3) {
    ignoredFiles_.insert(filepath);
    LOG_MSG_WARN << "Added to ignore list after 3 failures: " << filepath;
  }
}

void IncrementalUpdater::clearFailure(const std::string &filepath)
{
  failureCounts_.erase(filepath);
}