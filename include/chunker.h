#ifndef _CHUNKER_H_
#define _CHUNKER_H_

#include <vector>
#include <string>
#include <cstddef>
#include <utility>
#include <mutex>
#include <unordered_map>
#include "tokenizer.h"


struct Chunk {
  std::string docUri;
  std::string chunkId;
  std::string text;
  std::string raw;
  struct {
    size_t tokenCount;
    size_t start;
    size_t end;
    std::string unit; // "char": UTF-8 byte offsets; "line": zero-based lines. Both end-exclusive.
    std::string type; // e.g. code or text
  } metadata;
};


class Chunker {
private:
  const SimpleTokenizer &tokenizer_;
  size_t maxTokens_;
  size_t minTokens_;
  size_t overlapTokens_;

  mutable std::mutex cacheMutex_;
  mutable std::unordered_map<std::string, size_t> tokenCache_;

public:
  enum class ContentType { Code, Text, Binary };

  Chunker(const SimpleTokenizer &tok, size_t minTok = 50, size_t maxTok = 500, float overlap = 0.1f);

  std::vector<Chunk> chunkText(const std::string &text, const std::string &uri = "", bool semantic = true) const;

private:
  std::vector<Chunk> postProcessChunks(const std::vector<Chunk> &chunks, ContentType chunkType) const;
  size_t tokenCount(const std::string &text) const;
  std::vector<Chunk> splitIntoTextChunks(const std::string &text, const std::string &docId,
    const std::vector<std::pair<size_t, size_t>> &protectedRanges) const;
  std::vector<Chunk> splitIntoLineChunks(const std::string &text, const std::string &docId,
    const std::vector<std::pair<size_t, size_t>> &protectedRanges) const;
  std::vector<std::string> splitIntoLines(const std::string &text) const;

public:
  static std::string contentTypeToStr(Chunker::ContentType t);
  static Chunker::ContentType detectContentType(const std::string &text, const std::string &uri);
  static std::string maskOpaqueBlobs(const std::string &str);
};



#endif // _CHUNKER_H_
