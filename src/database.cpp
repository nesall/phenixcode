#include "database.h"
#include "chunker.h"
#include "cutils.h"
#include "sqlite3_helper.h"
#include <hnswlib/hnswlib.h>
#include <sqlite3.h>
#include <algorithm>
#include <stdexcept>
#include <filesystem>
#include <mutex>
#include <fstream>
#include <cstdint>
#include <iterator>
#include <cassert>
#include "utils_log/logger.hpp"
#include "3rdparty/fmt/core.h"
#include <cctype>
#include <unordered_set>

namespace {

  bool isIdentifierLike(const std::string &t) {
    bool hasLower = false, hasInnerUpper = false, hasDigit = false, hasAlpha = false;
    for (size_t i = 0; i < t.size(); ++i) {
      unsigned char c = t[i];
      if (c == '_') return true;
      if (std::isdigit(c)) hasDigit = true;
      if (std::isalpha(c)) hasAlpha = true;
      if (std::islower(c)) hasLower = true;
      if (i > 0 && std::isupper(c)) hasInnerUpper = true;
    }
    return (hasLower && hasInnerUpper) || (hasDigit && hasAlpha);
  }

  //std::string buildFtsQuery(const std::string &text) {
    //static const std::unordered_set<std::string> stop = {
    //  "what", "does", "do", "is", "are", "the", "a", "an", "how", "why", "of", "to",
    //  "in", "function", "class", "method", "this", "that", "and", "or", "for", "with" };
  //  std::vector<std::string> ids, words;
  //  std::string tok;
  //  auto flush = [&]() {
  //    if (tok.empty()) return;
  //    if (isIdentifierLike(tok)) {
  //      ids.push_back(tok);
  //    } else {
  //      std::string lower = tok;
  //      std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
  //      if (!stop.count(lower)) words.push_back(tok);
  //    }
  //    tok.clear();
  //    };
  //  for (unsigned char c : text) {
  //    if (std::isalnum(c) || c == '_' || c >= 0x80) tok += static_cast<char>(c);
  //    else flush();
  //  }
  //  flush();
  //  const auto &use = ids.empty() ? words : ids;
  //  std::string out;
  //  for (const auto &t : use) {
  //    if (!out.empty()) out += " OR ";
  //    out += '"' + t + '"';
  //  }
  //  return out;
  //}

  //bool queryHasIdentifier(const std::string &text) {
  //  std::string tok;
  //  bool found = false;
  //  auto flush = [&]() { if (!tok.empty() && isIdentifierLike(tok)) found = true; tok.clear(); };
  //  for (unsigned char c : text) {
  //    if (std::isalnum(c) || c == '_' || c >= 0x80) tok += static_cast<char>(c);
  //    else flush();
  //  }
  //  flush();
  //  return found;
  //}

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

  // For index db
  std::vector<size_t> pendingDeletes_;
  std::vector<std::pair<size_t, std::vector<float>>> pendingAdds_;
  bool inSqlTransaction_ = false;
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

HnswSqliteVectorDatabase::~HnswSqliteVectorDatabase()
{
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
  {
    const char *ftsInsertSql = R"(
      INSERT INTO chunks_fts(rowid, content, source_id) VALUES (?, ?, ?)
    )";
    utils::SqliteStmt ftsStmt;
    _checkErr = sqlite3_prepare_v2(imp->db_, ftsInsertSql, -1, &ftsStmt.ref(), nullptr);
    _checkErr = sqlite3_bind_int64(ftsStmt.ref(), 1, static_cast<int64_t>(chunkId));
    _checkErr = sqlite3_bind_text(ftsStmt.ref(), 2, chunk.text.c_str(), -1, SQLITE_STATIC);
    _checkErr = sqlite3_bind_text(ftsStmt.ref(), 3, chunk.docUri.c_str(), -1, SQLITE_STATIC);
    _checkErr = sqlite3_step(ftsStmt.ref());
  }
  try {
    size_t nofLines = countLines(chunk.docUri);
    upsertFileMetadata(chunk.docUri, utils::getFileModificationTime(chunk.docUri), std::filesystem::file_size(chunk.docUri), nofLines);
  } catch (const std::exception &ex) {
    LOG_MSG << "Error during upserting a chunk:" << ex.what();
  }
  if (imp->inSqlTransaction_) {
    imp->pendingAdds_.emplace_back(chunkId, embedding);
  } else {
    imp->index_->addPoint(embedding.data(), chunkId, true);
  }
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

std::vector<SearchResult> HnswSqliteVectorDatabase::bm25SearchRaw(const std::string &query, size_t top_k) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (query.empty()) return {};

  const char *bm25Sql = R"(
    SELECT f.rowid, c.content, c.source_id, c.unit, c.type,
           c.start_pos, c.end_pos, bm25(chunks_fts) AS rank
    FROM chunks_fts f
    JOIN chunks c ON c.id = f.rowid
    WHERE chunks_fts MATCH ?
    ORDER BY rank
    LIMIT ?
  )";

  utils::SqliteStmt stmt;
  _checkErr = sqlite3_prepare_v2(imp->db_, bm25Sql, -1, &stmt.ref(), nullptr);
  _checkErr = sqlite3_bind_text(stmt.ref(), 1, query.c_str(), -1, SQLITE_STATIC);
  _checkErr = sqlite3_bind_int64(stmt.ref(), 2, static_cast<int64_t>(top_k));

  auto colText = [&](int i) {
    auto p = sqlite3_column_text(stmt.ref(), i);
    return p ? std::string(reinterpret_cast<const char *>(p)) : std::string();
    };
  int rc;

  std::vector<SearchResult> results;
  while ((rc = sqlite3_step(stmt.ref())) == SQLITE_ROW) {
    SearchResult sr;
    sr.chunkId = sqlite3_column_int64(stmt.ref(), 0);
    sr.content = colText(1);
    sr.sourceId = colText(2);
    sr.chunkUnit = colText(3);
    sr.chunkType = colText(4);
    sr.start = sqlite3_column_int64(stmt.ref(), 5);
    sr.end = sqlite3_column_int64(stmt.ref(), 6);
    // bm25() returns negative (lower = better), negate to make higher = better
    sr.bm25Score = -static_cast<float>(sqlite3_column_double(stmt.ref(), 7));
    results.push_back(sr);
  }
  if (rc != SQLITE_DONE) {
    LOG_MSG << "bm25SearchRaw failed:" << sqlite3_errmsg(imp->db_);
  }
  return results;
}

HnswSqliteVectorDatabase::QueryPlan HnswSqliteVectorDatabase::planQuery(const std::string &text) const
{
  static const std::unordered_set<std::string> stop = {
  "what", "does", "do", "is", "are", "the", "a", "an", "how", "why", "of", "to",
  "in", "function", "class", "method", "this", "that", "and", "or", "for", "with" };

  std::vector<std::string> tokens;
  {
    std::string tok;
    auto flush = [&]() {
      if (tok.empty()) return;
      std::string lower = tok;
      std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
      if (!stop.count(lower) && std::find(tokens.begin(), tokens.end(), tok) == tokens.end()) tokens.push_back(tok);
      tok.clear();
      };
    for (unsigned char c : text) {
      if (std::isalnum(c) || c == '_' || c >= 0x80) tok += static_cast<char>(c);
      else flush();
    }
    flush();
  }

  QueryPlan plan;
  std::lock_guard<std::mutex> lock(mutex_);
  auto countRows = [&](const char *sql, const std::string &match) -> int64_t {
    utils::SqliteStmt stmt;
    _checkErr = sqlite3_prepare_v2(imp->db_, sql, -1, &stmt.ref(), nullptr);
    if (!match.empty()) _checkErr = sqlite3_bind_text(stmt.ref(), 1, match.c_str(), -1, SQLITE_TRANSIENT);
    return sqlite3_step(stmt.ref()) == SQLITE_ROW ? sqlite3_column_int64(stmt.ref(), 0) : 0;
    };
  const int64_t nChunks = countRows("SELECT count(*) FROM chunks", "");
  const int64_t maxDf = std::clamp<int64_t>(nChunks / 20, 3, 30);

  plan.fts.clear();
  std::vector<std::string> idTerms, otherTerms;
  for (const auto &tok : tokens) {
    const std::string quoted = '"' + tok + '"';
    const int64_t df = countRows("SELECT count(*) FROM chunks_fts WHERE chunks_fts MATCH ?", quoted);
#ifdef _DEBUG
    //LOG_MSG << "fts token" << tok << "df =" << df;   // calibrate maxDf from this
#endif
    if (df == 0 || df > maxDf) continue;
    (isIdentifierLike(tok) ? idTerms : otherTerms).push_back(quoted);
  }
  plan.hasRareIdentifier = !idTerms.empty();
  const auto &use = plan.hasRareIdentifier ? idTerms : otherTerms;
  for (size_t i = 0; i < use.size(); ++i) {
    if (i) plan.fts += " OR ";
    plan.fts += use[i];
  }
  return plan;
}

std::vector<SearchResult> HnswSqliteVectorDatabase::bm25Search(const std::string &query, size_t top_k) const
{
  const auto plan = planQuery(query);
  if (plan.fts.empty()) return {};
  return bm25SearchRaw(plan.fts, top_k);
}

std::vector<SearchResult> HnswSqliteVectorDatabase::hybridSearch(
  const std::vector<float> &queryEmbedding, const std::string &textQuery, size_t top_k, float bm25Weight) const
{
  if (queryEmbedding.size() != imp->vectorDim_) {
    throw std::runtime_error(fmt::format("Query embedding dimension mismatch: actual {}, claimed {}", queryEmbedding.size(), imp->vectorDim_));
  }
  if (top_k == 0) return {};

  const auto plan = planQuery(textQuery);
  constexpr float kProseBm25Weight = 0.0f; // try 0.15 / 0.3 later
  //float wBm25 = plan.fts.empty() ? 0.0f : std::clamp(bm25Weight, 0.0f, 1.0f);
  //if (0 < wBm25) {
  //  wBm25 = plan.hasRareIdentifier ? (std::max)(wBm25, 0.75f) : (std::min)(wBm25, kProseBm25Weight);
  //}
  //const float wVector = 1.0f - wBm25;
  float wBm25 = plan.fts.empty() ? 0.0f : std::clamp(bm25Weight, 0.0f, 1.0f);
  if (!plan.hasRareIdentifier) {
    wBm25 = (std::min)(wBm25, kProseBm25Weight);   // 0 for now
  }
  const float wVector = 1.0f - wBm25;

  constexpr size_t maxPerDoc = 2;
  const size_t candidateK = top_k * 4;

  std::vector<SearchResult> vectorResults, bm25Results;
  if (wVector > 0.0f) vectorResults = search(queryEmbedding, candidateK);
  if (wBm25 > 0.0f) {
    bm25Results = bm25Search(plan.fts, candidateK);
#ifdef _DEBUG
    //LOG_MSG << "hybridSearch | wBm25" << wBm25 << "| #hits" << bm25Results.size() << "| plan.fts" << plan.fts;
#endif
  }
  if (vectorResults.empty() && bm25Results.empty()) return {};

  constexpr float rrfK = 60.0f;
  struct Merged { SearchResult r; float fused = 0.0f; bool hasVector = false; };
  std::unordered_map<size_t, Merged> merged;

  for (size_t rank = 0; rank < vectorResults.size(); ++rank) {
    auto &m = merged[vectorResults[rank].chunkId];
    m.r = vectorResults[rank];
    m.hasVector = true;
    m.fused += wVector / (rrfK + static_cast<float>(rank) + 1.0f);
  }
  for (size_t rank = 0; rank < bm25Results.size(); ++rank) {
    auto [it, inserted] = merged.try_emplace(bm25Results[rank].chunkId);
    auto &m = it->second;
    if (inserted) m.r = bm25Results[rank];
    m.r.bm25Score = bm25Results[rank].bm25Score;
    m.fused += wBm25 / (rrfK + static_cast<float>(rank) + 1.0f);
  }

  // BM25-only hits have no cosine score; compute it so downstream thresholds stay meaningful.
  // Assumes normalized vectors with the Cosine (inner product) metric, same as search().
  auto cosineTo = [&](size_t id) -> float {
    if (imp->metric_ != DistanceMetric::Cosine) return 0.0f;
    try {
      auto v = getEmbeddingVector(id);
      if (v.size() != queryEmbedding.size()) return 0.0f;
      float dot = 0.0f;
      for (size_t i = 0; i < v.size(); ++i) dot += v[i] * queryEmbedding[i];
      return dot;
    } catch (...) {
      return 0.0f;
    }
    };

  std::vector<SearchResult> ranked;
  ranked.reserve(merged.size());
  for (auto &[id, m] : merged) {
    if (!m.hasVector) m.r.similarityScore = cosineTo(id);
    m.r.fusedScore = m.fused;
    ranked.push_back(std::move(m.r));
  }
  std::sort(ranked.begin(), ranked.end(),
    [](const SearchResult &a, const SearchResult &b) { return a.fusedScore > b.fusedScore; });

  // Per-doc cap, then truncate to top_k
  std::vector<SearchResult> res;
  //std::unordered_map<std::string, size_t> perDoc;
  //for (auto &r : ranked) {
  //  if (perDoc[r.sourceId] >= maxPerDoc) continue;
  //  ++perDoc[r.sourceId];
  //  res.push_back(std::move(r));
  //  if (res.size() >= top_k) break;
  //}

  constexpr size_t reserveVector = 2;
  std::unordered_set<size_t> reserved;
  for (size_t i = 0; i < (std::min)(reserveVector, vectorResults.size()); ++i)
    reserved.insert(vectorResults[i].chunkId);

  std::unordered_map<std::string, size_t> perDoc;
  std::unordered_set<size_t> taken;
  auto take = [&](const SearchResult &r) {
    if (res.size() >= top_k || taken.count(r.chunkId)) return;
    if (perDoc[r.sourceId] >= maxPerDoc) return;
    ++perDoc[r.sourceId];
    taken.insert(r.chunkId);
    res.push_back(r);
    };
  for (const auto &r : ranked) if (reserved.count(r.chunkId)) take(r);  // guaranteed first
  for (const auto &r : ranked) take(r);                                  // then fill by fused score
  std::sort(res.begin(), res.end(),
    [](const SearchResult &a, const SearchResult &b) { return a.fusedScore > b.fusedScore; });

  return res;
}

void HnswSqliteVectorDatabase::clear()
{
  std::lock_guard<std::mutex> lock(mutex_);
  try {
    beginTransaction();
    executeSql("DELETE FROM chunks");
    executeSql("DELETE FROM files_metadata");
    executeSql("DELETE FROM chunks_fts");
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

    const char *ftsTable = R"(
      CREATE VIRTUAL TABLE IF NOT EXISTS chunks_fts USING fts5(
          content, -- chunk text
          source_id UNINDEXED,
          tokenize='porter unicode61'
      )
    )";
    executeSql(ftsTable);
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

  {
    const char *ftsDeleteSql = "DELETE FROM chunks_fts WHERE source_id = ?";
    utils::SqliteStmt ftsStmt;
    _checkErr = sqlite3_prepare_v2(imp->db_, ftsDeleteSql, -1, &ftsStmt.ref(), nullptr);
    _checkErr = sqlite3_bind_text(ftsStmt.ref(), 1, sourceId.c_str(), -1, SQLITE_STATIC);
    _checkErr = sqlite3_step(ftsStmt.ref());
  }

  if (imp->inSqlTransaction_) {
    imp->pendingDeletes_.insert(imp->pendingDeletes_.end(), chunkIds.begin(), chunkIds.end());
  } else {
    for (size_t id : chunkIds) {
      try {
        imp->index_->markDelete(id);
      } catch (const std::runtime_error &e) {
        LOG_MSG << "Label" << id << "might already be deleted or not exist." << e.what();
      }
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

void HnswSqliteVectorDatabase::beginTransaction()
{
  executeSql("BEGIN TRANSACTION"); 
  imp->inSqlTransaction_ = true;
  assert(imp->pendingDeletes_.empty());
  assert(imp->pendingAdds_.empty());
  imp->pendingDeletes_.clear();
  imp->pendingAdds_.clear();
}

void HnswSqliteVectorDatabase::commit()
{
  executeSql("COMMIT");
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto id : imp->pendingDeletes_)
    imp->index_->markDelete(id);
  for (auto &p : imp->pendingAdds_)
    imp->index_->addPoint(p.second.data(), p.first, true);
  imp->pendingDeletes_.clear();
  imp->pendingAdds_.clear();
  imp->inSqlTransaction_ = false;
}

void HnswSqliteVectorDatabase::rollback()
{
  executeSql("ROLLBACK");
  imp->pendingDeletes_.clear();
  imp->pendingAdds_.clear();
  imp->inSqlTransaction_ = false;
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