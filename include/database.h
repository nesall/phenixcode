#ifndef _DATABASE_H_
#define _DATABASE_H_

#include <vector>
#include <string>
#include <memory>
#include <optional>
#include <unordered_map>
#include <mutex>

struct Chunk;

struct SearchResult {
  std::string content;
  std::string sourceId;
  std::string chunkUnit;
  std::string chunkType;
  size_t chunkId = 0;
  size_t start = 0;
  size_t end = 0;
  float similarityScore = 0; // cosine similarity (vector side)
  float distance = 0;
  float bm25Score = 0; // higher = better; 0 if not from BM25
  float fusedScore = 0; // RRF; set only by hybridSearch
};


struct FileMetadata {
  std::string path;
  time_t lastModified = 0;
  size_t fileSize = 0;
  size_t nofLines = 0;
  std::string hash; // Optional: content hash for change detection
};


struct DatabaseStats {
  size_t totalChunks = 0;
  size_t vectorCount = 0;
  size_t deletedCount = 0;
  size_t activeCount = 0;
  size_t totalTokens = 0;
  std::vector<std::pair<std::string, size_t>> sources;
};


class VectorDatabase {
protected:
  mutable std::mutex mutex_;
public:
  enum class DistanceMetric { L2, Cosine };

  virtual ~VectorDatabase() = default;

  virtual size_t addDocument(const Chunk &chunk, const std::vector<float> &embedding) = 0;
  virtual std::vector<size_t> addDocuments(const std::vector<Chunk> &chunks, const std::vector<std::vector<float>> &embeddings) = 0;

  virtual std::vector<SearchResult> search(const std::vector<float> &query, size_t top_k = 10) const = 0;
  virtual std::vector<SearchResult> searchWithFilter(const std::vector<float> &query,
    const std::string &sourceFilter = "",
    const std::string &typeFilter = "",
    size_t top_k = 10) const = 0;
  virtual std::vector<SearchResult> bm25Search(const std::string &query, size_t top_k) const = 0;
  virtual std::vector<SearchResult> hybridSearch(
    const std::vector<float> &queryEmbedding,
    const std::string &textQuery,
    size_t top_k,
    float bm25Weight = 0.75f) const = 0;

  virtual size_t deleteDocumentsBySource(const std::string &sourceId) = 0;
  virtual void clear() = 0;

  virtual void removeFileMetadata(const std::string &path) = 0;
  virtual bool fileExistsInMetadata(const std::string &path) const = 0;

  virtual std::vector<FileMetadata> getTrackedFiles() const = 0;
  virtual std::unordered_map<std::string, size_t> getChunkCountsBySources() const = 0;
  virtual std::optional<SearchResult> getChunkData(size_t chunkId) const = 0;
  virtual std::vector<size_t> getChunkIdsBySource(const std::string &sourceId) const = 0;
  virtual std::vector<float> getEmbeddingVector(size_t chunkId) const = 0;

  virtual DatabaseStats getStats() const = 0;
  virtual void persist() = 0;
  virtual void compact() {}

  virtual void beginTransaction() = 0;
  virtual void commit() = 0;
  virtual void rollback() = 0;
protected:
  virtual void upsertFileMetadata(const std::string &path, std::time_t mtime, size_t size, size_t lines) = 0;
};


class HnswSqliteVectorDatabase : public VectorDatabase {
public:
  HnswSqliteVectorDatabase(
    const std::string &dbPath, 
    const std::string &indexPath, 
    size_t vectorDim, 
    size_t maxElements = 100000,
    VectorDatabase::DistanceMetric metric = VectorDatabase::DistanceMetric::Cosine);
  ~HnswSqliteVectorDatabase();

  size_t addDocument(const Chunk &chunk, const std::vector<float> &embedding) override;
  std::vector<size_t> addDocuments(const std::vector<Chunk> &chunks, const std::vector<std::vector<float>> &embeddings) override;
  std::vector<SearchResult> search(const std::vector<float> &queryEmbedding, size_t topK = 10) const override;
  std::vector<SearchResult> searchWithFilter(const std::vector<float> &queryEmbedding,
    const std::string &sourceFilter = "",
    const std::string &typeFilter = "",
    size_t topK = 10) const override;
  std::vector<SearchResult> bm25Search(const std::string &query, size_t top_k) const override;
  std::vector<SearchResult> hybridSearch(
    const std::vector<float> &queryEmbedding,
    const std::string &textQuery,
    size_t top_k,
    float bm25Weight = 0.75f) const override;
  struct QueryPlan {
    std::string fts;                 // FTS5 MATCH expression, empty if nothing usable
    bool hasRareIdentifier = false;
  };

  DatabaseStats getStats() const override;
  void clear() override;

  size_t deleteDocumentsBySource(const std::string &sourceId) override;
  void removeFileMetadata(const std::string &sourceId) override;
  bool fileExistsInMetadata(const std::string &path) const override;

  std::vector<FileMetadata> getTrackedFiles() const override;
  std::unordered_map<std::string, size_t> getChunkCountsBySources() const override;
  std::vector<float> getEmbeddingVector(size_t chunkId) const override;

  void beginTransaction() override;
  void commit() override;
  void rollback() override;

  void persist() override;
  //void compact() override { compactIndex(); }

protected:
  void upsertFileMetadata(const std::string &sourceId, std::time_t mtime, size_t size, size_t lines) override;
  QueryPlan planQuery(const std::string &text) const;
  std::vector<SearchResult> bm25SearchRaw(const std::string &ftsQuery, size_t top_k) const;

private:
  std::string dbPath() const;
  std::string indexPath() const;

private:
  struct Impl;
  std::unique_ptr<Impl> imp;

  void initializeDatabase();
  void initializeVectorIndex();
  void executeSql(const std::string &sql);
  size_t insertMetadata(const Chunk &chunk);
  std::optional<SearchResult> getChunkData(size_t chunkId) const override;
  std::vector<size_t> getChunkIdsBySource(const std::string &sourceId) const override;
  //void compactIndex();
};

#endif // _DATABASE_H_