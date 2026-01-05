#include "database.h"
#include "cutils.h"
#include <hnswlib/hnswlib.h>
#include <sqlite3.h>
#include <algorithm>
#include <stdexcept>
#include <filesystem>
#include <mutex>
#include <fstream>
#include <iterator>
#include "utils_log/logger.hpp"
#include "3rdparty/fmt/core.h"


namespace {

  size_t countLines(const std::string &path) {
    std::ifstream file(path);
    return std::count(
      std::istreambuf_iterator<char>(file),
      std::istreambuf_iterator<char>(),
      '\n'
    );
  }

  struct SqliteErrorChecker {
    sqlite3 *sq_ = nullptr;
    SqliteErrorChecker &operator=(sqlite3 *sq) { sq_ = sq; return *this; }
    SqliteErrorChecker &operator=(int rc) {
      if (rc == SQLITE_OK || rc == SQLITE_DONE || rc == SQLITE_ROW) return *this;
      const char *msg = sq_ ? sqlite3_errmsg(sq_) : "SQLite error (no handle)";
      throw std::runtime_error(std::string("SQLite error: ") + msg);
    }
  };

  SqliteErrorChecker _checkErr;

} // anonymous namespace


struct HnswSqliteVectorDatabase::Impl {
  std::unique_ptr<hnswlib::HierarchicalNSW<float>> index_;
  std::unique_ptr<hnswlib::SpaceInterface<float>> space_;

  DistanceMetric metric_ = DistanceMetric::L2;

  sqlite3 *db_ = nullptr;

  size_t vectorDim_ = 0;
  size_t maxElements_ = 0;
  std::string dbPath_;
  std::string indexPath_;
};


HnswSqliteVectorDatabase::HnswSqliteVectorDatabase(
  const std::string &dbPath, const std::string &indexPath, size_t vectorDim, size_t maxElements, VectorDatabase::DistanceMetric metric)
  : imp(std::make_unique<Impl>())
{
  imp->metric_ = metric;
  imp->dbPath_ = dbPath;
  imp->indexPath_ = indexPath;
  imp->vectorDim_ = vectorDim;
  imp->maxElements_ = maxElements;

  initializeDatabase();
  initializeVectorIndex();
}

HnswSqliteVectorDatabase::~HnswSqliteVectorDatabase() {
  if (imp->db_) {
    sqlite3_close(imp->db_);
    _checkErr = nullptr;
  }
}

size_t HnswSqliteVectorDatabase::addDocument(const Chunk &chunk, const std::vector<float> &embedding)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (embedding.size() != imp->vectorDim_) {
    throw std::runtime_error(fmt::format("Embedding dimension mismatch: actual {}, claimed {}", embedding.size(), imp->vectorDim_));
  }
  size_t chunkId = insertMetadata(chunk);
  try {
    size_t nofLines = countLines(chunk.docUri);
    upsertFileMetadata(chunk.docUri, utils::getFileModificationTime(chunk.docUri), std::filesystem::file_size(chunk.docUri), nofLines);
  } catch (const std::exception &ex) {
    LOG_MSG << "Error during upserting a chunk:" << ex.what();
  }
  imp->index_->addPoint(embedding.data(), chunkId, true);
  return chunkId;
}

std::vector<size_t> HnswSqliteVectorDatabase::addDocuments(const std::vector<Chunk> &chunks, const std::vector<std::vector<float>> &embeddings)
{
  if (chunks.size() != embeddings.size()) {
    throw std::runtime_error("Chunks and embeddings count mismatch");
  }
  std::vector<size_t> chunkIds;
  for (size_t i = 0; i < chunks.size(); ++i) {
    size_t id = addDocument(chunks[i], embeddings[i]);
    chunkIds.push_back(id);
  }
  return chunkIds;
}

std::vector<SearchResult> HnswSqliteVectorDatabase::search(const std::vector<float> &queryEmbedding, size_t topK) const
{
  if (queryEmbedding.size() != imp->vectorDim_) {
    throw std::runtime_error(fmt::format("Query embedding dimension mismatch: actual {}, claimed {}", queryEmbedding.size(), imp->vectorDim_));
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (imp->index_->getCurrentElementCount() == 0) {
    return {};
  }
  auto result = imp->index_->searchKnn(queryEmbedding.data(), topK);
  std::vector<SearchResult> searchResults;
  while (!result.empty()) {
    const auto [distance, label] = result.top();
    result.pop();

    float similarity = 0;
    if (imp->metric_ == DistanceMetric::Cosine) {
      // InnerProduct returns negative dot product
      // For normalized vectors: similarity = (1 + dot_product) / 2
      // Or simply: similarity = -distance (if vectors normalized to [-1,1])
      similarity = 1.0f - distance; // Higher = more similar
    } else {
      // L2 distance
      similarity = 1.0f / (1.0f + distance);
    }

    auto chunkData = getChunkData(label);
    if (chunkData.has_value()) {
      SearchResult sr = chunkData.value();
      sr.similarityScore = similarity;
      sr.chunkId = label;
      sr.distance = distance;
      searchResults.push_back(sr);
    }
  }
  std::sort(searchResults.begin(), searchResults.end(),
    [](const SearchResult &a, const SearchResult &b) {
      return a.similarityScore > b.similarityScore;
    });
  return searchResults;
}

std::vector<SearchResult> HnswSqliteVectorDatabase::searchWithFilter(const std::vector<float> &queryEmbedding,
  const std::string &sourceFilter,
  const std::string &typeFilter,
  size_t topK) const
{
  auto results = search(queryEmbedding, topK * 2);
  std::vector<SearchResult> filtered;
  for (const auto &result : results) {
    bool matches = true;
    if (!sourceFilter.empty() && result.sourceId.find(sourceFilter) == std::string::npos) {
      matches = false;
    }
    if (!typeFilter.empty() && result.chunkType != typeFilter) {
      matches = false;
    }
    if (matches) {
      filtered.push_back(result);
      if (filtered.size() >= topK) break;
    }
  }
  return filtered;
}

void HnswSqliteVectorDatabase::clear()
{
  std::lock_guard<std::mutex> lock(mutex_);
  try {
    beginTransaction();
    executeSql("DELETE FROM chunks");
    executeSql("DELETE FROM files_metadata");
    // Just recreate index - simpler than unmarking everything
    if (imp->metric_ == DistanceMetric::Cosine) {
      imp->space_ = std::make_unique<hnswlib::InnerProductSpace>(imp->vectorDim_);
    } else {
      imp->space_ = std::make_unique<hnswlib::L2Space>(imp->vectorDim_);
    }
    imp->index_ = std::make_unique<hnswlib::HierarchicalNSW<float>>(
      imp->space_.get(), imp->maxElements_, 16, 200, 42, true
    );
    commit();
  } catch (...) {
    rollback();
  }
}

void HnswSqliteVectorDatabase::initializeDatabase()
{
  {
    std::lock_guard<std::mutex> lock(mutex_);
    LOG_MSG << "Initializing database at" << std::filesystem::absolute(imp->dbPath_);
    int rc = sqlite3_open(imp->dbPath_.c_str(), &imp->db_);
    if (rc != SQLITE_OK) {
      throw std::runtime_error("Cannot open database: " + std::string(sqlite3_errmsg(imp->db_)));
    }
    _checkErr = imp->db_;
    const char *chunksTable = R"(
        CREATE TABLE IF NOT EXISTS chunks (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            content TEXT NOT NULL,
            source_id TEXT NOT NULL,
            start_pos INTEGER NOT NULL,
            end_pos INTEGER NOT NULL,
            token_count INTEGER NOT NULL,
            unit TEXT NOT NULL,
            type TEXT NOT NULL,
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP
        )
    )";
    executeSql(chunksTable);

    const char *filesTable = R"(
        CREATE TABLE IF NOT EXISTS files_metadata (
            path TEXT PRIMARY KEY,
            last_modified INTEGER NOT NULL,
            file_size INTEGER NOT NULL,
            nof_lines INTEGER NOT NULL,
            indexed_at DATETIME DEFAULT CURRENT_TIMESTAMP
        )
    )";
    executeSql(filesTable);
  }
  auto files = getTrackedFiles();
  LOG_MSG << "Loaded metadata with" << files.size() << "files";
}

void HnswSqliteVectorDatabase::initializeVectorIndex()
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (imp->metric_ == DistanceMetric::Cosine) {
    imp->space_ = std::make_unique<hnswlib::InnerProductSpace>(imp->vectorDim_);
  } else {
    imp->space_ = std::make_unique<hnswlib::L2Space>(imp->vectorDim_);
  }
  if (std::filesystem::exists(imp->indexPath_)) {
    try {
      imp->index_ = std::make_unique<hnswlib::HierarchicalNSW<float>>(imp->space_.get(), imp->indexPath_, false, imp->maxElements_, true);
      LOG_MSG << "Loaded index with"
        << (imp->metric_ == DistanceMetric::Cosine ? "Cosine" : "L2") << "distance,"
        << imp->index_->getCurrentElementCount() << "total vectors,"
        << imp->index_->getDeletedCount() << "deleted";
      return;
    } catch (const std::exception &e) {
      LOG_MSG << "Failed to load existing index at" << std::filesystem::absolute(indexPath()) << "|" << e.what();
      LOG_MSG << "Creating new index...";
    }
  }
  imp->index_ = std::make_unique<hnswlib::HierarchicalNSW<float>>(imp->space_.get(), imp->maxElements_, 16, 200, 42, true);
}

void HnswSqliteVectorDatabase::executeSql(const std::string &sql)
{
  char *errorMessage = nullptr;
  int rc = sqlite3_exec(imp->db_, sql.c_str(), nullptr, nullptr, &errorMessage);
  if (rc != SQLITE_OK) {
    std::string error = errorMessage ? errorMessage : "Unknown error";
    if (errorMessage) sqlite3_free(errorMessage);
    throw std::runtime_error("SQL error: " + error);
  }
}

size_t HnswSqliteVectorDatabase::insertMetadata(const Chunk &chunk)
{
  const char *insertSql = R"(
        INSERT INTO chunks (content, source_id, start_pos, end_pos, token_count, unit, type)
        VALUES (?, ?, ?, ?, ?, ?, ?)
    )";

  utils::SqliteStmt stmt;
  _checkErr = sqlite3_prepare_v2(imp->db_, insertSql, -1, &stmt.ref(), nullptr);
  int k = 1;
  sqlite3_bind_text(stmt.ref(), k++, chunk.text.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt.ref(), k++, chunk.docUri.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt.ref(), k++, chunk.metadata.start);
  sqlite3_bind_int64(stmt.ref(), k++, chunk.metadata.end);
  sqlite3_bind_int64(stmt.ref(), k++, chunk.metadata.tokenCount);
  sqlite3_bind_text(stmt.ref(), k++, chunk.metadata.unit.c_str(), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt.ref(), k++, chunk.metadata.type.c_str(), -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt.ref());
  if (rc != SQLITE_DONE) {
    throw std::runtime_error("Failed to insert chunk metadata: " + std::string(sqlite3_errmsg(imp->db_)));
  }
  size_t chunkId = sqlite3_last_insert_rowid(imp->db_);
  return chunkId;
}

std::optional<SearchResult> HnswSqliteVectorDatabase::getChunkData(size_t chunkId) const
{
  const char *selectSql = R"(
        SELECT content, source_id, unit, type, start_pos, end_pos
        FROM chunks WHERE id = ?
    )";
  utils::SqliteStmt stmt;
  _checkErr = sqlite3_prepare_v2(imp->db_, selectSql, -1, &stmt.ref(), nullptr);
  _checkErr = sqlite3_bind_int64(stmt.ref(), 1, chunkId);
  SearchResult result;
  bool found = false;
  if (sqlite3_step(stmt.ref()) == SQLITE_ROW) {
    int k = 0;
    result.content = reinterpret_cast<const char *>(sqlite3_column_text(stmt.ref(), k++));
    result.sourceId = reinterpret_cast<const char *>(sqlite3_column_text(stmt.ref(), k++));
    result.chunkUnit = reinterpret_cast<const char *>(sqlite3_column_text(stmt.ref(), k++));
    result.chunkType = reinterpret_cast<const char *>(sqlite3_column_text(stmt.ref(), k++));
    result.start = sqlite3_column_int64(stmt.ref(), k++);
    result.end = sqlite3_column_int64(stmt.ref(), k++);
    found = true;
  }
  return found ? std::optional<SearchResult>(result) : std::nullopt;
}

std::vector<size_t> HnswSqliteVectorDatabase::getChunkIdsBySource(const std::string &sourceId) const
{
  std::vector<size_t> ids;
  utils::SqliteStmt stmt;
  const char *sql = "SELECT id FROM chunks WHERE source_id = ?";
  _checkErr = sqlite3_prepare_v2(imp->db_, sql, -1, &stmt.ref(), nullptr);
  _checkErr = sqlite3_bind_text(stmt.ref(), 1, sourceId.c_str(), -1, SQLITE_STATIC);
  while (sqlite3_step(stmt.ref()) == SQLITE_ROW) {
    ids.push_back(sqlite3_column_int64(stmt.ref(), 0));
  }
  return ids;
}

size_t HnswSqliteVectorDatabase::deleteDocumentsBySource(const std::string &sourceId)
{
  std::lock_guard<std::mutex> lock(mutex_);
  auto chunkIds = getChunkIdsBySource(sourceId);
  if (chunkIds.empty()) return 0;
  utils::SqliteStmt stmt;
  const char *sql = "DELETE FROM chunks WHERE source_id = ?";
  _checkErr = sqlite3_prepare_v2(imp->db_, sql, -1, &stmt.ref(), nullptr);
  _checkErr = sqlite3_bind_text(stmt.ref(), 1, sourceId.c_str(), -1, SQLITE_STATIC);
  _checkErr = sqlite3_step(stmt.ref());
  size_t n = sqlite3_changes(imp->db_);
  for (size_t id : chunkIds) {
    try {
      imp->index_->markDelete(id);
    } catch (const std::runtime_error &e) {
      LOG_MSG << "Label" << id << "might already be deleted or not exist." << e.what();
    }
  }
  return n;
}

void HnswSqliteVectorDatabase::removeFileMetadata(const std::string &filepath)
{
  std::lock_guard<std::mutex> lock(mutex_);
  utils::SqliteStmt stmt;
  const char *sql = "DELETE FROM files_metadata WHERE path = ?";
  _checkErr = sqlite3_prepare_v2(imp->db_, sql, -1, &stmt.ref(), nullptr);
  _checkErr = sqlite3_bind_text(stmt.ref(), 1, filepath.c_str(), -1, SQLITE_STATIC);
  _checkErr = sqlite3_step(stmt.ref());
}

void HnswSqliteVectorDatabase::upsertFileMetadata(const std::string &filepath, std::time_t mtime, size_t size, size_t lines)
{
  utils::SqliteStmt stmt;
  const char *sql = "INSERT OR REPLACE INTO files_metadata (path, last_modified, file_size, nof_lines) VALUES (?, ?, ?, ?)";
  _checkErr = sqlite3_prepare_v2(imp->db_, sql, -1, &stmt.ref(), nullptr);
  _checkErr = sqlite3_bind_text(stmt.ref(), 1, filepath.c_str(), -1, SQLITE_STATIC);
  _checkErr = sqlite3_bind_int64(stmt.ref(), 2, mtime);
  _checkErr = sqlite3_bind_int64(stmt.ref(), 3, size);
  _checkErr = sqlite3_bind_int64(stmt.ref(), 4, lines);
  _checkErr = sqlite3_step(stmt.ref());
}

std::vector<FileMetadata> HnswSqliteVectorDatabase::getTrackedFiles() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<FileMetadata> files;
  utils::SqliteStmt stmt;
  const char *sql = "SELECT path, last_modified, file_size, nof_lines FROM files_metadata";
  _checkErr = sqlite3_prepare_v2(imp->db_, sql, -1, &stmt.ref(), nullptr);
  while (sqlite3_step(stmt.ref()) == SQLITE_ROW) {
    FileMetadata meta;
    meta.path = reinterpret_cast<const char *>(sqlite3_column_text(stmt.ref(), 0));
    meta.lastModified = sqlite3_column_int64(stmt.ref(), 1);
    meta.fileSize = sqlite3_column_int64(stmt.ref(), 2);
    meta.nofLines = sqlite3_column_int64(stmt.ref(), 3);
    files.push_back(meta);
  }
  return files;
}

std::unordered_map<std::string, size_t> HnswSqliteVectorDatabase::getChunkCountsBySources() const
{
  std::unordered_map<std::string, size_t> counts;
  utils::SqliteStmt stmt;
  const char *sql = "SELECT source_id, COUNT(*) FROM chunks GROUP BY source_id";
  if (sqlite3_prepare_v2(imp->db_, sql, -1, &stmt.ref(), nullptr) != SQLITE_OK) {
    // optionally log: sqlite3_errmsg(imp->db_);
    return counts;
  }
  while (sqlite3_step(stmt.ref()) == SQLITE_ROW) {
    const unsigned char *src = sqlite3_column_text(stmt.ref(), 0);
    size_t cnt = static_cast<size_t>(sqlite3_column_int64(stmt.ref(), 1));
    if (src)
      counts.emplace(reinterpret_cast<const char *>(src), cnt);
  }
  return counts;
}

std::vector<float> HnswSqliteVectorDatabase::getEmbeddingVector(size_t chunkId) const
{
  return imp->index_->getDataByLabel<float>(chunkId);
}


bool HnswSqliteVectorDatabase::fileExistsInMetadata(const std::string &path) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  utils::SqliteStmt stmt;
  const char *sql = "SELECT 1 FROM files_metadata WHERE path = ?";
  _checkErr = sqlite3_prepare_v2(imp->db_, sql, -1, &stmt.ref(), nullptr);
  _checkErr = sqlite3_bind_text(stmt.ref(), 1, path.c_str(), -1, SQLITE_STATIC);
  bool exists = (sqlite3_step(stmt.ref()) == SQLITE_ROW);
  return exists;
}

DatabaseStats HnswSqliteVectorDatabase::getStats() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  DatabaseStats stats;
  stats.vectorCount = imp->index_->getCurrentElementCount();
  stats.deletedCount = imp->index_->getDeletedCount();
  stats.activeCount = imp->index_->getCurrentElementCount() - imp->index_->getDeletedCount();
  {
    utils::SqliteStmt stmt;
    _checkErr = sqlite3_prepare_v2(imp->db_, "SELECT COUNT(*) FROM chunks", -1, &stmt.ref(), nullptr);
    if (sqlite3_step(stmt.ref()) == SQLITE_ROW) {
      stats.totalChunks = sqlite3_column_int64(stmt.ref(), 0);
    }
  }
  {
    utils::SqliteStmt stmt;
    _checkErr = sqlite3_prepare_v2(imp->db_, "SELECT source_id, COUNT(*) FROM chunks GROUP BY source_id", -1, &stmt.ref(), nullptr);
    while (sqlite3_step(stmt.ref()) == SQLITE_ROW) {
      std::string source = reinterpret_cast<const char *>(sqlite3_column_text(stmt.ref(), 0));
      size_t count = sqlite3_column_int64(stmt.ref(), 1);
      stats.sources.emplace_back(source, count);
    }
  }
  return stats;
}

void HnswSqliteVectorDatabase::persist()
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (0 < imp->index_->getCurrentElementCount()) {
    imp->index_->saveIndex(imp->indexPath_);
    //LOG_MSG << "Saved vector index with " << imp->index_->getCurrentElementCount() << " vectors";
  } else {
    LOG_MSG << "Saving with no vectors in the index db. Skipped.";
  }
}

std::string HnswSqliteVectorDatabase::dbPath() const
{
  return imp->dbPath_;
}

std::string HnswSqliteVectorDatabase::indexPath() const
{
  return imp->indexPath_;
}

#if 0
void HnswSqliteVectorDatabase::compactIndex()
{
  std::lock_guard<std::mutex> lock(mutex_);
  size_t deleted_count = imp->index_->getDeletedCount();

  if (deleted_count == 0) {
    LOG_MSG << "No deleted items to compact.";
    return;
  }

  LOG_MSG << "Compacting index (" << deleted_count << " deleted items)...";

  // Get all active chunks
  std::vector<std::pair<size_t, std::vector<float>>> activeItems;

  {
    SqliteStmt stmt;
    const char *sql = "SELECT id FROM chunks";
    _checkErr = sqlite3_prepare_v2(imp->db_, sql, -1, &stmt.ref(), nullptr);

    while (sqlite3_step(stmt.ref()) == SQLITE_ROW) {
      size_t chunkId = sqlite3_column_int64(stmt.ref(), 0);
      if (!imp->index_->isMarkedDeleted(static_cast<unsigned>(chunkId))) {
        auto embedding = imp->index_->getDataByLabel<float>(chunkId);
        activeItems.emplace_back(chunkId, std::move(embedding));
      }
    }
  }

  // Create new index
  std::unique_ptr<hnswlib::SpaceInterface<float>> newSpace;
  if (imp->metric_ == DistanceMetric::Cosine)
    newSpace = std::make_unique<hnswlib::InnerProductSpace>(imp->vectorDim_);
  else
    newSpace = std::make_unique<hnswlib::L2Space>(imp->vectorDim_);
  auto newIndex = std::make_unique<hnswlib::HierarchicalNSW<float>>(newSpace.get(), imp->maxElements_, 16, 200, 42, true);

  // Add all active items
  for (const auto &[id, embedding] : activeItems) {
    newIndex->addPoint(embedding.data(), id);
  }

  // Replace old index
  imp->space_ = std::move(newSpace);
  imp->index_ = std::move(newIndex);

  LOG_MSG << "Compaction complete. Active items: " << imp->index_->getCurrentElementCount();
}
#endif