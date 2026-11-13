#include "context_builder.h"
#include "httpserver.h"
#include "app.h"
#include "chunker.h"
#include "sourceproc.h"
#include "database.h"
#include "inference.h"
#include "settings.h"
#include "tokenizer.h"
#include "3rdparty/fmt/core.h"
#include "json_shim.h"
#include <utils_log/logger.hpp>
#include <hnswlib/hnswlib.h>
#include <chrono>
#include <cassert>
#include <exception>
#include <algorithm>
#include <set>
#include <vector>
#include <string>
#include <functional>
#include <string_view>
#include <filesystem>
#include <utility>
#include <unordered_map>

using json = nlohmann::json;

namespace context {

  std::string truncateToTokens(const SimpleTokenizer &t, const std::string &s, size_t maxTokens) {
    if (!maxTokens) return {};
    const auto masked = Chunker::maskOpaqueBlobs(s);
    if (t.countTokensWithVocab(masked) <= maxTokens) return masked;
    Chunker chunker(t, 1, maxTokens, 0.f);
    const auto chunks = chunker.chunkText(masked, {}, false);
    return chunks.empty() ? std::string{} : chunks.front().text;
  }

  size_t suffixPrefixMatch(const std::string &a, const std::string &b) {
    size_t maxLen = std::min(a.size(), b.size());
    for (size_t len = maxLen; len > 0; --len)
      if (a.compare(a.size() - len, len, b, 0, len) == 0)
        return len;
    return 0;
  }

  std::string stitchChunks(const std::vector<std::string> &chunks) {
    if (chunks.empty()) return {};
    std::string result = chunks.front();
    size_t totalEstimate = 0;
    for (auto &c : chunks) totalEstimate += c.size();
    result.reserve(totalEstimate);
    for (size_t i = 1; i < chunks.size(); ++i) {
      size_t overlap = suffixPrefixMatch(result, chunks[i]);
      result += chunks[i].substr(overlap);
    }
    return result;
  }

  std::vector<size_t> getClosestNeighbors(const std::vector<size_t> &ids, size_t D, size_t M) {
    if (ids.empty() || M == 0) return {};

    auto it = std::lower_bound(ids.begin(), ids.end(), D);
    size_t idx = std::distance(ids.begin(), it);

    // Always include D (insert if not present)
    bool hasD = (it != ids.end() && *it == D);
    size_t half = M / 2;

    size_t start = (idx > half) ? idx - half : 0;
    size_t end = std::min(start + M, ids.size());

    // Adjust window near the end
    if (end - start < M && end == ids.size())
      start = (end > M) ? end - M : 0;

    std::vector<size_t> result(ids.begin() + start, ids.begin() + end);

    // Insert D if not already inside the vector
    if (!hasD) {
      result.insert(std::lower_bound(result.begin(), result.end(), D), D);
      if (result.size() > M) {
        // Trim symmetrically
        if (idx < ids.size() / 2) result.pop_back();
        else result.erase(result.begin());
      }
    }
    return result;
  }

  size_t calculateNeighborCount(size_t excerptBudget, size_t avgChunkTokens, size_t minChunks, size_t maxChunks) {
    size_t neighbors = excerptBudget / avgChunkTokens;
    // Always include at least 3 chunks (before, match, after)
    return std::clamp(std::clamp(neighbors, size_t(minChunks), size_t(maxChunks)), size_t(1), size_t(101));
  }

  bool isWithinThreshold(const App &app, const std::string &content, size_t maxTokenBudget, size_t usedTokens, float thresholdRatio, size_t *pTokens = nullptr) {
    if (maxTokenBudget <= usedTokens) return false;
    const auto excerptBudget = maxTokenBudget - usedTokens;
    const auto tokens = app.tokenizer().countTokensWithVocab(content);
    const auto maxAllowed = static_cast<size_t>(excerptBudget * thresholdRatio);
    if (pTokens) *pTokens = tokens;
    auto threshold = maxAllowed;
    return tokens <= threshold;
  }

  bool processContent(const App &app, std::string &content, const std::string &src, size_t chunkId, size_t maxTokenBudget, size_t &usedTokens, std::vector<size_t> &stitchedIds) {
    if (maxTokenBudget <= usedTokens) return false;
    const auto excerptBudget = maxTokenBudget - usedTokens;
    // If the source file of the best chunk is too large then we fetch an excerpt of it instead.
    const auto avgChunkTokens = app.settings().chunkingMaxTokens();
    float thresholdRatio = app.settings().generationExcerptThresholdRatio();
    size_t contentTokens = 0;
    if (!isWithinThreshold(app, content, maxTokenBudget, usedTokens, thresholdRatio, &contentTokens)) {
      if (!app.settings().generationExcerptEnabled()) {
        return false;
      }
      const auto ids = app.db().getChunkIdsBySource(src);
      if (ids.empty()) return false;
      if (chunkId == -1) {
        chunkId = ids[ids.size() / 2];
      }
      size_t minChunks = app.settings().generationExcerptMinChunks();
      size_t maxChunks = app.settings().generationExcerptMaxChunks();
      size_t maxAllowed = static_cast<size_t>(excerptBudget * thresholdRatio);
      const auto nofNb = calculateNeighborCount(maxAllowed, avgChunkTokens, minChunks, maxChunks);
      const auto betterIds = getClosestNeighbors(ids, chunkId, nofNb);

      std::vector<size_t> order;
      order.reserve(betterIds.size());
      auto centerIt = std::lower_bound(betterIds.begin(), betterIds.end(), chunkId);
      size_t center = std::distance(betterIds.begin(), centerIt);
      if (center < betterIds.size() && betterIds[center] == chunkId) {
        order.push_back(center);
      }
      for (size_t d = 1; order.size() < betterIds.size(); ++d) {
        if (center >= d) order.push_back(center - d);
        if (center + d < betterIds.size()) order.push_back(center + d);
      }

      std::map<size_t, std::string> chunkhoodMap;
      size_t chunkTokens = 0;
      bool stopLeft = false, stopRight = false;
      for (size_t idx : order) {
        if (idx < center && stopLeft) continue;
        if (idx > center && stopRight) continue;
        auto i = betterIds[idx];
        auto opt = app.db().getChunkData(i);
        const auto tokens = opt ? app.tokenizer().countTokensWithVocab(opt->content) : 0;
        if (!opt || maxAllowed < chunkTokens + tokens) {
          if (idx == center) return false;
          if (idx < center) stopLeft = true;
          else stopRight = true;
          continue;
        }
        chunkhoodMap.insert({ i, std::move(opt->content) });
        chunkTokens += tokens;
        stitchedIds.push_back(i);
      }
      std::vector<std::string> chunkhood;
      for (const auto &t : chunkhoodMap) {
        chunkhood.push_back(t.second);
      }
      if (chunkhood.empty()) return false;
      content = stitchChunks(chunkhood); // Also removes overlaps
      contentTokens = app.tokenizer().countTokensWithVocab(content);
    }
    usedTokens += contentTokens;
    return true;
  }

  template <class T>
  bool vecContains(const std::vector<T> &vec, const T &t) {
    return std::find(vec.begin(), vec.end(), t) != vec.end();
  }

  template <class T>
  bool vecAddIfUnique(std::vector<T> &vec, const T &t) {
    if (!vecContains(vec, t)) {
      vec.push_back(t);
      return true;
    }
    return false;
  }

  template <class T>
  bool vecAddIfUnique(std::vector<T> &vec, const std::vector<T> &t) {
    bool a = false;
    for (const auto &item : t) {
      if (vecAddIfUnique(vec, item)) a = true;
    }
    return a;
  }

  void addToSearchResult(std::vector<SearchResult> &v, const std::string &src, const std::string &content) {
    assert(!content.empty());
    if (!content.empty()) {
      v.push_back({
          content,
          src,
          "char",
          Chunker::contentTypeToStr(Chunker::detectContentType(content, "")),
          std::string::npos,
          0,
          content.length(),
          1.0f
        });
    }
  }

  std::vector<Attachment> parseAttachments(const json &attachmentsJson) {
    std::vector<Attachment> res;
    if (!attachmentsJson.is_array()) return res;
    for (const auto &item : attachmentsJson) {
      if (!item.is_object()) continue;
      if (!item.contains("content") || !item["content"].is_string()) {
        continue;
      }
      Attachment a;
      if (item.contains("filename") && item["filename"].is_string()) {
        a.filename = item["filename"].get<std::string>();
      }
      a.content = item["content"].get<std::string>();
      if (!a.filename.empty()) {
        a.content = "[Attachment: " + a.filename + "]\n" + a.content + "\n[/Attachment]";
      }
      res.push_back(std::move(a));
    }
    return res;
  }

  bool isSyntheticGrepId(size_t id) {
    return id > (std::numeric_limits<size_t>::max() - 100000);
  }

  std::pair<std::vector<SearchResult>, size_t> processInputResults(
    const App &app,
    const ApiConfig &apiConfig,
    size_t usedTokens,
    const std::string &question,
    std::vector<Attachment> attachments,
    std::vector<std::string> sources,
    float contextSizeRatio,
    bool attachedOnly,
    std::function<void(std::string_view)> onInfo,
    bool freeChat
  ) {
    LOG_START;
    if (!onInfo) onInfo = [](std::string_view) {};
    // Preferred order
    std::vector<SearchResult> attachmentResults;
    std::vector<SearchResult> directSrcResults;
    std::vector<SearchResult> relatedSrcResults;
    std::vector<SearchResult> filteredChunkResults;
    std::vector<SearchResult> orderedResults; // Final ordered results

    const auto maxTokenBudget = static_cast<size_t>(apiConfig.contextLength * std::clamp(contextSizeRatio, 0.1f, 1.0f));
    assert(0 < maxTokenBudget);

    if (!freeChat && attachedOnly && attachments.empty() && sources.empty()) {
      assert(!"Should not happen");
      attachedOnly = false;
      LOG_MSG_WARN << "'attachedOnly' is set but no attachments or sources provided. Ignored.";
      onInfo("'attachedOnly' is set but no attachments or sources provided; ignoring.");
    }

    //onInfo(fmt::format("Context token budget:", ((maxTokenBudget % 1000) == 0) ? std::to_string(maxTokenBudget) + "k" : std::to_string(maxTokenBudget)));
    const auto questionTokens = app.tokenizer().countTokensWithVocab(question);
    const size_t initialTokens = usedTokens;

    LOG_MSG << "Total context budget:" << maxTokenBudget;
    LOG_MSG << "Budget used for question:" << questionTokens;
    LOG_MSG << "Budget used for history messages:" << usedTokens - questionTokens;

    auto finishResults = [&](std::vector<SearchResult> results) {
      if (app.settings().generationMaxChunks() < results.size()) {
        size_t lastN = results.size() - app.settings().generationMaxChunks();
        for (size_t j = 0; j < lastN; j++)
          usedTokens -= app.tokenizer().countTokensWithVocab(results[results.size() - lastN + j].content);
        results.resize(app.settings().generationMaxChunks());
      }
      onInfo(fmt::format("Context token budget used {}/{}", usedTokens, maxTokenBudget));
      return std::make_pair(std::move(results), usedTokens);
    };

    {
      if (!attachments.empty()) {
        onInfo("Processing attachment(s)");
      }
      const auto maxAttBudget = static_cast<size_t>(maxTokenBudget * 0.8);
      for (size_t j = 0; j < attachments.size(); j ++) {
        const auto &att{ attachments[j] };
        auto content{ att.content };
        size_t tokens = app.tokenizer().countTokensWithVocab(content);
        if (tokens < maxAttBudget * 0.2 && usedTokens + tokens < maxAttBudget) {
          usedTokens += tokens;
          onInfo(fmt::format("Adding attachment {}", att.filename));
          addToSearchResult(attachmentResults, att.filename.empty() ? "attachment" : att.filename, std::move(content));
          attachments.erase(attachments.begin() + j);
          j --;
        }
      }
      for (const auto &att : attachments) {
        if (maxAttBudget <= usedTokens) break;
        onInfo(fmt::format("Adding attachment {}", att.filename));
        auto content{ att.content };
        size_t tokens = app.tokenizer().countTokensWithVocab(content);
        if (usedTokens + tokens < maxAttBudget) {
          usedTokens += tokens;
        } else {
          auto m = content.length();
          content = truncateToTokens(app.tokenizer(), content, maxAttBudget - usedTokens);
          usedTokens += app.tokenizer().countTokensWithVocab(content);
          auto percent = int((content.length() / double(m)) * 100);
          auto info = fmt::format("Attachment too large, truncated to {}% of {}", percent, att.filename);
          LOG_MSG_WARN << info;
          onInfo(fmt::format("{} truncated to {}% ", att.filename, percent));
        }
        addToSearchResult(attachmentResults, att.filename.empty() ? "attachment" : att.filename, std::move(content));
      }
    }

    LOG_MSG << "Budget used for attachments:" << usedTokens - initialTokens;

    if (freeChat) return finishResults(std::move(attachmentResults));

    std::vector<std::vector<float>> questionEmbeddingVectors;
    std::unordered_map<std::string, SearchResult> sourceToChunk;
    std::vector<std::string> relSources;
    std::vector<size_t> usedChunkIds;

    const auto questionChunks = app.chunker().chunkText(question, "", false);
    std::vector<std::string> questionTexts;
    for (const auto &qc : questionChunks) questionTexts.push_back(qc.text);
    if (questionTexts.empty()) questionTexts.push_back(question);

    const bool hasIndex = (0 < app.db().getStats().totalChunks);
    bool embeddingsAvailable = false;

    if (hasIndex) {
      try {
        EmbeddingClient embeddingClient(app.settings().embeddingCurrentApi(), app.settings().embeddingTimeoutMs());
        embeddingClient.generateEmbeddings(questionTexts, questionEmbeddingVectors, EmbeddingClient::EncodeType::Query);
        embeddingsAvailable = (questionEmbeddingVectors.size() == questionTexts.size());
      } catch (const std::exception &e) {
        LOG_MSG_WARN << "Unable to generate embeddings" << e.what() << "| Falling back to grep.";
        onInfo("Embedding server unavailable; falling back to text grep");
        embeddingsAvailable = false;
        questionEmbeddingVectors.clear();
      }
    } else {
      onInfo("Database not indexed; using text grep");
    }

    if (!attachedOnly) {
      std::set<size_t> uniqueChunkResults;
      std::unordered_map<std::string, float> sourcesRank;
      for (size_t j = 0; j < questionTexts.size(); ++j) {
        const std::vector<float> &qVec = embeddingsAvailable ? questionEmbeddingVectors[j] : std::vector<float>{};
        // App::retrieve handles empty embedding as pure grep
        auto res = app.retrieve(questionTexts[j], qVec, app.settings().embeddingTopK());
        for (const auto &r : res) {
          sourcesRank[r.sourceId] += r.similarityScore;
          if (uniqueChunkResults.insert(r.chunkId).second) {
            filteredChunkResults.push_back(r);
          }
        }
      }

      std::sort(filteredChunkResults.begin(), filteredChunkResults.end(), [&sourcesRank](const SearchResult &a, const SearchResult &b) {
        return std::tie(sourcesRank[b.sourceId], a.sourceId, a.chunkId) < std::tie(sourcesRank[a.sourceId], b.sourceId, b.chunkId);
        });

      const auto maxFullSources = app.settings().generationMaxFullSources();
      for (const auto &r : filteredChunkResults) {
        if (maxFullSources <= sources.size()) break;
        if (!isSyntheticGrepId(r.chunkId)) {
          if (vecAddIfUnique(sources, r.sourceId)) {}
          sourceToChunk[r.sourceId] = r;
        }
      }

      const auto trackedFiles = app.db().getTrackedFiles();
      std::vector<std::string> trackedSources;
      for (const auto &tf : trackedFiles) {
        trackedSources.push_back(tf.path);
      }

      for (const auto &src : sources) {
        auto relations = app.sourceProcessor().filterRelatedSources(trackedSources, src);
        for (const auto &rel : relations) {
          if (!vecContains(sources, rel)) {
            vecAddIfUnique(relSources, rel);
          }
        }
      }

      for (const auto &rel : relSources) {
        onInfo(fmt::format("Adding related file {}", std::filesystem::path(rel).filename().string()));
      }
    } else {
      assert(sourceToChunk.empty());
      assert(filteredChunkResults.empty());
      assert(relSources.empty());
    }

    size_t srcTokens = 0;
    for (size_t j = 0; j < sources.size(); j ++) {
      const auto &src = sources[j];
      // src is either a user-set context file, or a chunk's base file (sourceToChunk).
      auto content = app.sourceProcessor().fetchSource(src).content;
      if (content.empty()) {
        continue;
      }
      if (maxTokenBudget <= usedTokens) {
        break;
      }
      size_t contentTokens = 0;
      if (sourceToChunk.count(src)) {
        auto nUsed = usedTokens;
        std::vector<size_t> stitchedIds;
        if (!processContent(app, content, src, sourceToChunk[src].chunkId, maxTokenBudget, usedTokens, stitchedIds)) {
          continue;
        }
        srcTokens += usedTokens - nUsed;
        if (stitchedIds.empty()) {
          auto ids = app.db().getChunkIdsBySource(src);
          usedChunkIds.insert(usedChunkIds.end(), ids.cbegin(), ids.cend());
        } else {
          usedChunkIds.insert(usedChunkIds.end(), stitchedIds.cbegin(), stitchedIds.cend());
        }
      } else {
        float thresholdRatio = app.settings().generationExcerptThresholdRatio();
        if (attachedOnly && j == sources.size() - 1) thresholdRatio = 1.0f;
        if (!isWithinThreshold(app, content, maxTokenBudget, usedTokens, thresholdRatio, &contentTokens)) {
          auto info = fmt::format("Processing large file {}", std::filesystem::path(src).filename().string());
          onInfo(info);
          content.clear();
          contentTokens = 0;
          auto ids = app.db().getChunkIdsBySource(src);
          if (!ids.empty() && embeddingsAvailable && !questionEmbeddingVectors.empty()) {
            const auto remaining = maxTokenBudget - usedTokens;
            const auto avgChunkTokens = app.settings().chunkingMaxTokens();
            const auto nofMaxChunks = remaining / avgChunkTokens;
            hnswlib::InnerProductSpace space{ app.settings().databaseVectorDim() };
            hnswlib::HierarchicalNSW<float> hnswDB(&space, 1000, 16, 200, 42, true);
            ids.resize(999, std::numeric_limits<size_t>::max());
            std::unordered_map<size_t, std::string> idToContent;
            for (auto id : ids) {
              if (id == std::numeric_limits<size_t>::max()) break;
              if (auto opt = app.db().getChunkData(id)) {
                auto vec = app.db().getEmbeddingVector(id);
                hnswDB.addPoint(vec.data(), id);
                idToContent[id] = std::move(opt->content);
              }
            }
            const auto topK = static_cast<size_t>(nofMaxChunks * thresholdRatio);
            if (0 < topK) {
              assert(!questionEmbeddingVectors.empty());
              content.reserve(questionEmbeddingVectors.size() * topK);
              std::set<size_t> uniqueLabels;
              int nofFetched = 0;
              bool bStop = false;
              size_t chunkTokens = 0;
              for (const auto &v : questionEmbeddingVectors) {
                if (bStop) break;
                auto result = hnswDB.searchKnn(v.data(), topK);
                nofFetched = result.size();
                std::vector<SearchResult> searchResults;
                while (!result.empty()) {
                  const auto [distance, label] = result.top();
                  result.pop();
                  if (uniqueLabels.insert(label).second) {
                    auto chunk = idToContent[label];
                    auto cn = app.tokenizer().countTokensWithVocab(chunk);
                    if (maxTokenBudget < usedTokens + chunkTokens + cn) {
                      bStop = true;
                      break;
                    }
                    chunkTokens += cn;
                    content += chunk;
                    usedChunkIds.push_back(static_cast<size_t>(label));
                  }
                }
              }
              onInfo(fmt::format("Adding {} relevant chunks from {}", nofFetched, std::filesystem::path(src).filename().string()));
              auto tokens = app.tokenizer().countTokensWithVocab(content);
              contentTokens += tokens;
              srcTokens += tokens;
            }
          } else {
            // Grep/head fallback: read the head of the file up to the threshold
            std::string fullContent = app.sourceProcessor().fetchSource(src).content;
            size_t remainingTokens = (maxTokenBudget > usedTokens) ? (maxTokenBudget - usedTokens) : 0;
            size_t allowedTokens = static_cast<size_t>(remainingTokens * thresholdRatio);
            content = truncateToTokens(app.tokenizer(), fullContent, allowedTokens);
            contentTokens = app.tokenizer().countTokensWithVocab(content);
            srcTokens += contentTokens;
          }
        } else {
          auto ids = app.db().getChunkIdsBySource(src);
          usedChunkIds.insert(usedChunkIds.end(), ids.cbegin(), ids.cend());
        }
      }
      if (!content.empty()) {
        addToSearchResult(directSrcResults, src, std::move(content));
        usedTokens += contentTokens;
      }
    }
    LOG_MSG << "Budget used for full sources:" << srcTokens;

    if (!attachedOnly) {
      size_t relTokens = 0;
      for (const auto &rel : relSources) {
        auto content = app.sourceProcessor().fetchSource(rel).content;
        if (content.empty()) {
          continue;
        }
        if (maxTokenBudget <= usedTokens) break;
        auto nUsed = usedTokens;
        std::vector<size_t> stitchedIds;
        if (processContent(app, content, rel, -1, maxTokenBudget, usedTokens, stitchedIds)) {
          relTokens += usedTokens - nUsed;
          addToSearchResult(relatedSrcResults, rel, std::move(content));
          if (stitchedIds.empty()) {
            auto ids = app.db().getChunkIdsBySource(rel);
            usedChunkIds.insert(usedChunkIds.end(), ids.cbegin(), ids.cend());
          } else {
            usedChunkIds.insert(usedChunkIds.end(), stitchedIds.cbegin(), stitchedIds.cend());
          }
        }
      }
      LOG_MSG << "Budget used for related sources:" << relTokens;

      filteredChunkResults.erase(std::remove_if(filteredChunkResults.begin(), filteredChunkResults.end(),
        [&usedChunkIds](const SearchResult &r) {
          return r.chunkId != std::string::npos && vecContains(usedChunkIds, r.chunkId);
        }), filteredChunkResults.end());


      for (size_t j = 0; j < filteredChunkResults.size(); j ++) {
        auto chunkTokens = app.tokenizer().countTokensWithVocab(filteredChunkResults[j].content);
        if (usedTokens + chunkTokens <= maxTokenBudget) {
          usedTokens += chunkTokens;
        } else {
          filteredChunkResults.erase(filteredChunkResults.cbegin() + j);
          j --;
        }
      }
    }

    // Assemble final ordered results
    orderedResults.insert(orderedResults.end(), attachmentResults.begin(), attachmentResults.end());
    orderedResults.insert(orderedResults.end(), directSrcResults.begin(), directSrcResults.end());
    orderedResults.insert(orderedResults.end(), relatedSrcResults.begin(), relatedSrcResults.end());
    orderedResults.insert(orderedResults.end(), filteredChunkResults.begin(), filteredChunkResults.end());

    //#ifdef _DEBUG
    //    size_t nn = 0;
    //    for (const auto &tt : orderedResults) {
    //      nn += app.tokenizer().countTokensWithVocab(tt.content);
    //    }
    //    LOG_MSG << "Total context tokens used:" << nn;
    //#endif

    return finishResults(std::move(orderedResults));
  }

  size_t messagesNofTokens(const json &messages, const App &app) {
    size_t n = 0;
    for (const auto &m : messages) {
      if (m.is_object() && ((m.contains("content") && m["content"].is_array()) ||
          m.contains("reasoning_content") || m.contains("reasoning") || m.contains("reasoning_details"))) {
        n += app.tokenizer().countTokensWithVocab(m.dump());
        continue;
      }
      if (m.is_object() && m.contains("content") && m["content"].is_string()) {
        std::string s = m["content"];
        if (!s.empty()) {
          n += app.tokenizer().countTokensWithVocab(s);
        }
      }
    }
    return n;
  }

} // namespace context
