#include "httpserver.h"
#include "app.h"
#include "chunker.h"
#include "sourceproc.h"
#include "database.h"
#include "inference.h"
#include "settings.h"
#include "tokenizer.h"
#include "instregistry.h"
#include "auth.h"
#include "3rdparty/base64.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <utils_log/logger.hpp>
#include <hnswlib/hnswlib.h>
#include <chrono>
#include <cassert>
#include <exception>
#include <algorithm>
#include <vector>
#include <string>
#include <atomic>
#include <functional>
#include <string_view>
//#include <format>
#include <filesystem>
#include "3rdparty/fmt/core.h"

using json = nlohmann::json;

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
#define CLOSE_SOCKET closesocket
#define INVALID_SOCKET_VALUE INVALID_SOCKET
#define WSA_STARTUP() \
      WSADATA wsaData; \
      if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return false;
#define WSA_CLEANUP() WSACleanup()
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
using socket_t = int;
#define CLOSE_SOCKET close
#define INVALID_SOCKET_VALUE (-1)
#define WSA_STARTUP()
#define WSA_CLEANUP()
#endif


namespace {

  std::string truncateToTokens(const SimpleTokenizer &t, const std::string &s, size_t maxTokens) {
    std::string res{ s };

    auto tokens = t.countTokensWithVocab(res);
    if (tokens <= maxTokens) return res;

    Chunker chunker(t, 1, 50, 0.f);

    auto chunks = chunker.chunkText(res, {}, false);

    size_t end = 0;
    size_t tokensSoFar = 0;
    for (const auto &chunk : chunks) {
      if (maxTokens < chunk.metadata.tokenCount + tokensSoFar) {
        assert(chunk.metadata.unit == "char");
        end = chunk.metadata.end;
        break;
      }
      tokensSoFar += chunk.metadata.tokenCount;
    }
    res = res.substr(0, end);
    return res;
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
    const auto excerptBudget = maxTokenBudget - usedTokens;
    if (excerptBudget <= 0) return false;
    const auto avgChunkTokens = app.settings().chunkingMaxTokens();
    const auto tokens = app.tokenizer().countTokensWithVocab(content);
    if (pTokens) *pTokens = tokens;
    auto threshold = (std::max)(static_cast<size_t>(excerptBudget * thresholdRatio), avgChunkTokens);
    return tokens <= threshold;
  }

  bool processContent(const App &app, std::string &content, const std::string &src, size_t chunkId, size_t maxTokenBudget, size_t &usedTokens) {
    const auto excerptBudget = maxTokenBudget - usedTokens;
    if (excerptBudget <= 0) return false;
    // If the source file of the best chunk is too large then we fetch an excerpt of it instead.
    const auto avgChunkTokens = app.settings().chunkingMaxTokens();    
    float thresholdRatio = app.settings().generationExcerptThresholdRatio();
    size_t contentTokens = 0;
    if (!isWithinThreshold(app, content, maxTokenBudget, usedTokens, thresholdRatio, &contentTokens)) {
      if (!app.settings().generationExcerptEnabled()) {
        return false;
      }
      const auto ids = app.db().getChunkIdsBySource(src);
      assert(!ids.empty());
      if (chunkId == -1) {
        chunkId = ids[ids.size() / 2];
      }
      size_t minChunks = app.settings().generationExcerptMinChunks();
      size_t maxChunks = app.settings().generationExcerptMaxChunks();
      const auto nofNb = calculateNeighborCount(static_cast<size_t>(excerptBudget * thresholdRatio), avgChunkTokens, minChunks, maxChunks);
      const auto betterIds = getClosestNeighbors(ids, chunkId, nofNb);
      std::vector<std::string> chunkhood;
      for (auto i : betterIds) {
        auto opt = app.db().getChunkData(i);
        if (opt.has_value()) {
          chunkhood.push_back(std::move(opt->content));
        }
      }
      content = stitchChunks(chunkhood); // Also removes overlaps
      contentTokens = app.tokenizer().countTokensWithVocab(content);
    }
    usedTokens += contentTokens;
    return true;
  }

  bool isPortAvailable(int port) {
    WSA_STARTUP();
    socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET_VALUE) {
      WSA_CLEANUP();
      return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    // No SO_REUSEADDR — ensures exclusive bind
    bool available = (bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0);
    CLOSE_SOCKET(sock);
    WSA_CLEANUP();
    return available;
  }

#if 0
  auto testStreaming = [](std::function<void(const std::string &)> onChunk)
    {
      for (int i = 0; i < 25; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        std::ostringstream oss;
        oss << "thread ";
        oss << std::this_thread::get_id();
        oss << ", chunk ";
        oss << std::to_string(i);
        oss << "\n\n";
        onChunk(oss.str());
      }
    };
#endif

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

  struct Attachment {
    std::string filename;
    std::string content;
  };

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

  void recordDuration(const std::chrono::steady_clock::time_point &start, std::atomic<double> &v) {
    std::chrono::duration<double> duration = std::chrono::steady_clock::now() - start;
    double newSearchTimeMs = duration.count() * 1000.0;
    double oldVal = v.load();
    double newVal;
    do {
      newVal = oldVal * 0.9 + newSearchTimeMs * 0.1;
    } while (!v.compare_exchange_weak(oldVal, newVal));
  }

  std::optional<std::pair<std::string, std::string>> extractPassword(const httplib::Request &req) {
    try {
      auto header = req.get_header_value("Authorization");
      if (header.find("Basic ") == 0) {
        std::string encoded = header.substr(6);
        std::string decoded = base64_decode(encoded);
        // Format is "username:password" - we only care about password
        size_t colon = decoded.find(':');
        if (colon == std::string::npos) {
          return std::nullopt;
        }
        return std::pair{ decoded.substr(colon + 1), "Basic" };
      } else if (header.find("Bearer ") == 0) {
        std::string encoded = header.substr(7);
        return std::pair{ encoded, "Bearer" };
      }
    } catch (const std::exception &e) {
      LOG_MSG << "Error extracting password" << e.what();
    }
    return std::nullopt;
  }

  bool requireAuth(AdminAuth &auth, const httplib::Request &req, httplib::Response &res, std::string *jwtToken) {
    // Skip authentication if the request is from localhost
    if (req.remote_addr == "127.0.0.1" || req.remote_addr == "::1") {
      return true;
    }
    auto password = extractPassword(req);
    std::string jwt;
    if (!password.has_value() || !auth.authenticate(password.value(), jwt)) {
      res.status = 401;
      res.set_header("WWW-Authenticate", "Basic realm=\"Embedder Admin\"");
      res.set_content(R"({"error": "Authentication required"})", "application/json");
      return false;
    }
    if (jwtToken) *jwtToken = jwt;
    return true;
  }

  std::pair<std::vector<SearchResult>, size_t> processInputResults(
    const App &app, 
    const ApiConfig &apiConfig,
    const std::string &question, 
    std::vector<Attachment> attachments, 
    std::vector<std::string> sources,
    float contextSizeRatio,
    bool attachedOnly,
    std::function<void(std::string_view)> onInfo
  ) {
    if (!onInfo) onInfo = [](std::string_view) {};
    // Preferred order
    std::vector<SearchResult> attachmentResults;
    std::vector<SearchResult> fullSourceResults;
    std::vector<SearchResult> relatedSrcResults;
    std::vector<SearchResult> filteredChunkResults;
    std::vector<SearchResult> orderedResults; // Final ordered results

    const auto maxTokenBudget = static_cast<size_t>(apiConfig.contextLength * std::clamp(contextSizeRatio, 0.1f, 1.0f));
    assert(0 < maxTokenBudget);

    if (attachedOnly && attachments.empty() && sources.empty()) {
      assert(!"Should not happen");
      attachedOnly = false;
      LOG_MSG << "Warning: 'attachedOnly' is set but no attachments or sources provided. Ignored.";
      onInfo("'attachedOnly' is set but no attachments or sources provided; ignoring.");
    }

    //onInfo(fmt::format("Context token budget:", ((maxTokenBudget % 1000) == 0) ? std::to_string(maxTokenBudget) + "k" : std::to_string(maxTokenBudget)));
    const size_t questionTokens = app.tokenizer().countTokensWithVocab(question);
    size_t usedTokens = questionTokens;

    LOG_MSG << "Total context budget:" << maxTokenBudget;

    LOG_MSG << "Budget used for question:" << questionTokens;

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
          usedTokens = maxAttBudget;
          auto percent = int((content.length() / double(m)) * 100);
          auto info = fmt::format("Warning: Attachment too large, truncated to {}% of {}", percent, att.filename);
          LOG_MSG << info;
          onInfo(fmt::format("{} truncated to {}% ", att.filename, percent));
        }
        addToSearchResult(attachmentResults, att.filename.empty() ? "attachment" : att.filename, std::move(content));
      }
    }

    LOG_MSG << "Budget used for attachments:" << usedTokens - questionTokens;

    std::vector<std::vector<float>> questionEmbeddingVectors;
    std::unordered_map<std::string, SearchResult> sourceToChunk;
    std::vector<std::string> allFullSources;
    std::vector<std::string> relSources;

    EmbeddingClient embeddingClient(app.settings().embeddingCurrentApi(), app.settings().embeddingTimeoutMs());
    const auto questionChunks = app.chunker().chunkText(question, "", false);
    std::vector<std::string> questionTexts;
    for (const auto &qc : questionChunks) questionTexts.push_back(qc.text);
    embeddingClient.generateEmbeddings(questionTexts, questionEmbeddingVectors, EmbeddingClient::EncodeType::Query);

    if (!attachedOnly) {
      std::set<size_t> uniqueChunkResults;
      std::unordered_map<std::string, float> sourcesRank;
      for (const auto &embedding : questionEmbeddingVectors) {
        auto res = app.db().search(embedding, app.settings().embeddingTopK());
        for (const auto &r : res) {
          sourcesRank[r.sourceId] += r.similarityScore;
          if (uniqueChunkResults.insert(r.chunkId).second) {
            filteredChunkResults.push_back(r);
          }
        }
      }
      std::sort(filteredChunkResults.begin(), filteredChunkResults.end(), [&sourcesRank](const SearchResult &a, const SearchResult &b) {
        return sourcesRank[a.sourceId] > sourcesRank[b.sourceId];
        });

      const auto maxFullSources = app.settings().generationMaxFullSources();
      for (const auto &r : filteredChunkResults) {
        if (maxFullSources <= sources.size()) break;
        if (vecAddIfUnique(sources, r.sourceId)) {}
        sourceToChunk[r.sourceId] = r;
      }

      const auto trackedFiles = app.db().getTrackedFiles();
      std::vector<std::string> trackedSources;
      for (const auto &tf : trackedFiles) {
        trackedSources.push_back(tf.path);
      }

      allFullSources = sources;
      for (const auto &src : sources) {
        auto relations = app.sourceProcessor().filterRelatedSources(trackedSources, src);
        //vecAddIfUnique(relSources, relations);
        //vecAddIfUnique(allFullSources, relations);
        for (const auto &rel : relations) {
          if (!vecContains(sources, rel)) {
            vecAddIfUnique(relSources, rel);
            vecAddIfUnique(allFullSources, rel);
          }
        }
      }

      for (const auto &rel : relSources) {
        onInfo(fmt::format("Adding related file {}", std::filesystem::path(rel).filename().string()));
      }
    } else {
      allFullSources = sources;
      assert(sourceToChunk.empty());
      assert(filteredChunkResults.empty());
      assert(relSources.empty());
    }

    size_t srcTokens = 0;
    for (size_t j = 0; j < sources.size(); j ++) {
      const auto &src = sources[j];
      // src is either a user-set context file, or a chunk's base file (sourceToChunk).
      auto content = app.sourceProcessor().fetchSource(src).content;
      if (maxTokenBudget <= usedTokens) break;
      size_t contentTokens = 0;
      if (sourceToChunk.count(src)) {
        auto nUsed = usedTokens;
        if (!processContent(app, content, src, sourceToChunk[src].chunkId, maxTokenBudget, usedTokens)) {
          break;
        }
        srcTokens += usedTokens - nUsed;
      } else {
        float thresholdRatio = app.settings().generationExcerptThresholdRatio();
        if (attachedOnly && j == sources.size() - 1) thresholdRatio = 1.0f;
        if (!isWithinThreshold(app, content, maxTokenBudget, usedTokens, thresholdRatio, &contentTokens)) {
          auto info = fmt::format("Processing large file {}", std::filesystem::path(src).filename().string());
          onInfo(info);
          auto ids = app.db().getChunkIdsBySource(src);
          if (!ids.empty()) {
            const auto remaining = maxTokenBudget - usedTokens;
            const auto avgChunkTokens = app.settings().chunkingMaxTokens();
            const auto nofMaxChunks = remaining / avgChunkTokens;
            hnswlib::InnerProductSpace space{ app.settings().databaseVectorDim() };
            hnswlib::HierarchicalNSW<float> hnswDB(&space, 1000, 16, 200, 42, true);
            ids.resize(999);
            std::unordered_map<size_t, std::string> idToContent;
            for (auto id : ids) {
              if (auto opt = app.db().getChunkData(id)) {
                auto vec = app.db().getEmbeddingVector(id);
                hnswDB.addPoint(vec.data(), id);
                idToContent[id] = std::move(opt->content);
              }
            }
            content.clear();
            contentTokens = 0;
            const auto topK = static_cast<size_t>(nofMaxChunks * thresholdRatio);
            if (0 < topK) {
              assert(!questionEmbeddingVectors.empty());
              content.reserve(questionEmbeddingVectors.size() * topK);
              int nofFetched = 0;
              for (const auto &v : questionEmbeddingVectors) {
                auto result = hnswDB.searchKnn(v.data(), topK);
                nofFetched = result.size();
                std::vector<SearchResult> searchResults;
                while (!result.empty()) {
                  const auto [distance, label] = result.top();
                  result.pop();
                  float similarity = 1.0f - distance; // Higher = more similar
                  auto chunk = idToContent[label];
                  content += chunk;
                }
              }
              onInfo(fmt::format("Adding {} relevant chunks from {}", nofFetched, std::filesystem::path(src).filename().string()));
              auto tokens = app.tokenizer().countTokensWithVocab(content);
              contentTokens += tokens;
              srcTokens += tokens;
            }
          }
        }
      }
      if (!content.empty()) {
        addToSearchResult(fullSourceResults, src, std::move(content));
        usedTokens += contentTokens;
      }
    }
    LOG_MSG << "Budget used for full sources:" << srcTokens;

    if (!attachedOnly) {
      size_t relTokens = 0;
      for (const auto &rel : relSources) {
        auto content = app.sourceProcessor().fetchSource(rel).content;
        auto nUsed = usedTokens;
        if (processContent(app, content, rel, -1, maxTokenBudget, usedTokens)) {
          relTokens += usedTokens - nUsed;
          addToSearchResult(relatedSrcResults, rel, std::move(content));
        }
      }
      LOG_MSG << "Budget used for related sources:" << relTokens;

      filteredChunkResults.erase(std::remove_if(filteredChunkResults.begin(), filteredChunkResults.end(),
        [&allFullSources](const SearchResult &r) {
          return vecContains(allFullSources, r.sourceId) && r.chunkId != std::string::npos;
        }), filteredChunkResults.end());
    }

    // Assemble final ordered results
    orderedResults.insert(orderedResults.end(), attachmentResults.begin(), attachmentResults.end());
    orderedResults.insert(orderedResults.end(), fullSourceResults.begin(), fullSourceResults.end());
    orderedResults.insert(orderedResults.end(), relatedSrcResults.begin(), relatedSrcResults.end());
    orderedResults.insert(orderedResults.end(), filteredChunkResults.begin(), filteredChunkResults.end());
    if (app.settings().generationMaxChunks() < orderedResults.size()) {
      orderedResults.resize(app.settings().generationMaxChunks());
    }
    onInfo(fmt::format("Context token budget used {}/{}", usedTokens, maxTokenBudget));

//#ifdef _DEBUG
//    size_t nn = 0;
//    for (const auto &tt : orderedResults) {
//      nn += app.tokenizer().countTokensWithVocab(tt.content);
//    }
//    LOG_MSG << "Total context tokens used:" << nn;
//#endif

    return { orderedResults, usedTokens };
  }

  ApiConfig getTargetApi(const json &request, const App &app) {
    ApiConfig apiConfig = app.settings().generationCurrentApi();
    if (request.contains("targetapi") && request["targetapi"].is_string()) {
      std::string targetApi = request["targetapi"].get<std::string>();
      if (targetApi != apiConfig.id) {
        auto apis = app.settings().generationApis();
        auto it = std::find_if(apis.begin(), apis.end(), [&targetApi](const ApiConfig &a) { return a.id == targetApi; });
        if (it != apis.end()) apiConfig = *it;
      }
    }
    return apiConfig;
  }

} // anonymous namespace


struct HttpServer::Impl {
  Impl(App &a)
    : app_(a)
  {
  }

  httplib::Server server_;

  App &app_;

  static std::atomic<size_t> requestCounter_;
  static std::atomic<size_t> searchCounter_;
  static std::atomic<size_t> chatCounter_;
  static std::atomic<size_t> embedCounter_;
  static std::atomic<size_t> errorCounter_;

  static std::chrono::steady_clock::time_point startTime_;

  // Moving averages for performance
  static std::atomic<double> avgSearchTimeMs_;
  static std::atomic<double> avgChatTimeMs_;
  static std::atomic<double> avgEmbedTimeMs_;
};

std::atomic<size_t> HttpServer::Impl::requestCounter_{ 0 };
std::atomic<size_t> HttpServer::Impl::searchCounter_{ 0 };
std::atomic<size_t> HttpServer::Impl::chatCounter_{ 0 };
std::atomic<size_t> HttpServer::Impl::embedCounter_{ 0 };
std::atomic<size_t> HttpServer::Impl::errorCounter_{ 0 };
std::chrono::steady_clock::time_point HttpServer::Impl::startTime_;
std::atomic<double> HttpServer::Impl::avgSearchTimeMs_{ 0.0 };
std::atomic<double> HttpServer::Impl::avgChatTimeMs_{ 0.0 };
std::atomic<double> HttpServer::Impl::avgEmbedTimeMs_{ 0.0 };


HttpServer::HttpServer(App &a)
  : imp(new Impl(a))
{
  imp->server_.new_task_queue = [] { return new httplib::ThreadPool(4); };

  imp->server_.set_error_logger([](const httplib::Error &err, const httplib::Request *req) {
    std::cerr << httplib::to_string(err) << " while processing request";
    if (req) {
      std::cerr << ", client: " << req->get_header_value("X-Forwarded-For")
        << ", request: '" << req->method << " " << req->path << " " << req->version << "'"
        << ", host: " << req->get_header_value("Host");
    }
    std::cerr << std::endl;
    });
}

HttpServer::~HttpServer()
{
}

int HttpServer::bindToPortIncremental(int port)
{
  int nofTries = port == 0 ? 0 : 20;
  while (0 < nofTries && !isPortAvailable(port)) {
    if (--nofTries == 0) {
      LOG_MSG << "Unable to reserve a port.";
      return false;
    }
    port ++;
  }
  if (nofTries == 0)
    port = imp->server_.bind_to_any_port("0.0.0.0");
  else if (!imp->server_.bind_to_port("0.0.0.0", port)) {
    port = 0; // failure.
  }
  return port;
}

bool HttpServer::startServer() 
{
  auto &server = imp->server_;
  auto &auth = imp->app_.auth();

  server.set_mount_point("/setup", "./public/setup/");

  server.Get("/", [this](const httplib::Request &, httplib::Response &res) {
    LOG_MSG << "GET /";
    if (!std::filesystem::exists(imp->app_.settings().configPath())) {
      res.set_redirect("/setup/");
    } else {
      res.set_content(R"(
                <h1>PhenixCode Embedder</h1>
                <p>API is running!</p>
                <ul>
                    <li><a href="/api/health">Health Check</a></li>
                    <li><a href="/api/stats">Statistics</a></li>
                    <li><a href="/api/metrics">Metrics</a></li>
                    <li><a href="/setup/">Setup Wizard</a></li>
                </ul>
            )", "text/html");
    }
    Impl::requestCounter_++;
    });

  server.Post("/api/authenticate", [&](const httplib::Request &req, httplib::Response &res) {
    LOG_MSG << "POST /api/authenticate";
    std::string jwt;
    if (!requireAuth(auth, req, res, &jwt)) return; // Validate password
    json response = {
      {"status", "OK"},
      {"token", jwt}
    };
    res.set_content(response.dump(), "application/json");
    Impl::requestCounter_++;
    });

  server.Post("/api/setup", [&](const httplib::Request &req, httplib::Response &res) {
    LOG_MSG << "POST /api/setup";
    Impl::requestCounter_++;
    if (!requireAuth(auth, req, res, nullptr)) return;
    try {
      json config = json::parse(req.body);
      // Loosely validate config
      if (!config.contains("embedding")) {
        throw std::invalid_argument("Missing embedding field");
      }
      if (!config.contains("generation")) {
        throw std::invalid_argument("Missing generation field");
      }
      if (!config.contains("database")) {
        throw std::invalid_argument("Missing database field");
      }
      if (!config.contains("chunking")) {
        throw std::invalid_argument("Missing chunking field");
      }
      auto &settings = imp->app_.refSettings();
      settings.updateFromConfig(config);
      settings.save();
      json response = {
          {"status", "success"},
          {"message", "Configuration generated successfully"}
      };
      res.set_content(response.dump(), "application/json");
    } catch (const std::exception &e) {
      json error = { {"error", e.what()} };
      res.status = 400;
      res.set_content(error.dump(), "application/json");
    }
    });

  server.Get("/api/setup", [&](const httplib::Request &req, httplib::Response &res) {
    LOG_MSG << "GET /api/setup";
    Impl::requestCounter_++;
    if (!requireAuth(auth, req, res, nullptr)) return;
    try {
      const auto &config = imp->app_.settings().configDump();
      res.set_content(config, "application/json");
    } catch (const std::exception &e) {
      json error = { {"error", e.what()} };
      res.status = 500;
      res.set_content(error.dump(), "application/json");
      Impl::errorCounter_++;
    }
    });

  server.Get("/api/health", [](const httplib::Request &, httplib::Response &res) {
    LOG_MSG << "GET /api/health";
    json response = { {"status", "ok"} };
    res.set_content(response.dump(), "application/json");
    Impl::requestCounter_++;
    });

  server.Post("/api/search", [this](const httplib::Request &req, httplib::Response &res) {
    const auto start = std::chrono::steady_clock::now();
    try {
      LOG_MSG << "POST /api/search";
      json request = json::parse(req.body);
      std::string query = request["query"].get<std::string>();
      size_t top_k = request.value("top_k", 5);
      std::vector<float> queryEmbedding;
      EmbeddingClient embeddingClient(imp->app_.settings().embeddingCurrentApi(), imp->app_.settings().embeddingTimeoutMs());
      embeddingClient.generateEmbeddings(query, queryEmbedding, EmbeddingClient::EncodeType::Query);
      auto results = imp->app_.db().search(queryEmbedding, top_k);
      json response = json::array();
      for (const auto &result : results) {
        response.push_back({
            {"content", result.content},
            {"source_id", result.sourceId},
            {"chunk_type", result.chunkType},
            {"chunk_unit", result.chunkUnit},
            {"similarity_score", result.similarityScore},
            {"start_pos", result.start},
            {"end_pos", result.end}
          });
      }
      res.set_content(response.dump(), "application/json");
    } catch (const std::exception &e) {
      json error = { {"error", e.what()} };
      res.status = 400;
      res.set_content(error.dump(), "application/json");
      Impl::errorCounter_++;
    }
    Impl::requestCounter_++;
    Impl::searchCounter_++;
    recordDuration(start, Impl::avgSearchTimeMs_);
    });

  // (one-off embedding without storage)
  server.Post("/api/embed", [this](const httplib::Request &req, httplib::Response &res) {
    const auto start = std::chrono::steady_clock::now();
    try {
      LOG_MSG << "POST /api/embed";
      json request = json::parse(req.body);
      std::string text = request["text"].get<std::string>();
      auto chunks = imp->app_.chunker().chunkText(text, "api-request");
      std::vector<std::string> texts;
      for (const auto &c : chunks) {
        texts.push_back(c.text);
      }
      json response = json::array();
      const auto &ss = imp->app_.settings();
      const auto batchSize = ss.embeddingBatchSize();
      EmbeddingClient embeddingClient(ss.embeddingCurrentApi(), ss.embeddingTimeoutMs());
      for (size_t i = 0; i < chunks.size(); i += batchSize) {
        size_t end = (std::min)(i + batchSize, chunks.size());
        std::vector<std::string> batchTexts(texts.begin() + i, texts.begin() + end);

        std::vector<std::vector<float>> embeddings;
        embeddingClient.generateEmbeddings(batchTexts, embeddings, EmbeddingClient::EncodeType::Query);

        for (const auto &emb : embeddings) {
          response.push_back({ {"embedding", emb}, {"dimension", emb.size()} });
        }
      }
      res.set_content(response.dump(), "application/json");
    } catch (const std::exception &e) {
      json error = { {"error", e.what()} };
      res.status = 400;
      res.set_content(error.dump(), "application/json");
      Impl::errorCounter_++;
    }
    Impl::requestCounter_++;
    Impl::embedCounter_++;
    recordDuration(start, Impl::avgEmbedTimeMs_);
    });

  server.Post("/api/documents", [this](const httplib::Request &req, httplib::Response &res) {
    try {
      LOG_MSG << "POST /api/documents";
      json request = json::parse(req.body);

      std::string content = request["content"].get<std::string>();
      std::string source_id = request["source_id"].get<std::string>();

      auto chunks = imp->app_.chunker().chunkText(content, source_id);

      EmbeddingClient embeddingClient(imp->app_.settings().embeddingCurrentApi(), imp->app_.settings().embeddingTimeoutMs());
      size_t inserted = 0;
      for (const auto &chunk : chunks) {
        std::vector<float> embedding;
        embeddingClient.generateEmbeddings(chunk.text, embedding, EmbeddingClient::EncodeType::Document);
        imp->app_.db().addDocument(chunk, embedding);
        inserted++;
      }

      imp->app_.db().persist();

      json response = {
          {"status", "success"},
          {"chunks_added", inserted}
      };

      res.set_content(response.dump(), "application/json");

    } catch (const std::exception &e) {
      json error = { {"error", e.what()} };
      res.status = 400;
      res.set_content(error.dump(), "application/json");
      Impl::errorCounter_++;
    }
    Impl::requestCounter_++;
    });

  server.Get("/api/documents", [this](const httplib::Request &req, httplib::Response &res) {
    try {
      LOG_MSG << "GET /api/documents";
      auto files = imp->app_.db().getTrackedFiles();
      json response = json::array();
      for (const auto &file : files) {
        response.push_back({
            {"path", file.path},
            {"lastModified", file.lastModified},
            {"size", file.fileSize}
          });
      }
      res.set_content(response.dump(), "application/json");
    } catch (const std::exception &e) {
      json error = { {"error", e.what()} };
      res.status = 500;
      res.set_content(error.dump(), "application/json");
    }
    Impl::requestCounter_++;
    });

  server.Get("/api/stats", [this](const httplib::Request &, httplib::Response &res) {
    try {
      LOG_MSG << "GET /api/stats";
      auto stats = imp->app_.db().getStats();
      json response = {
          {"total_chunks", stats.totalChunks},
          {"vector_count", stats.vectorCount},
          {"sources", imp->app_.sourceStats()}
      };
      res.set_content(response.dump(), "application/json");
    } catch (const std::exception &e) {
      json error = { {"error", e.what()} };
      res.status = 500;
      res.set_content(error.dump(), "application/json");
      Impl::errorCounter_++;
    }
    Impl::requestCounter_++;
    });

  server.Post("/api/update", [this](const httplib::Request &req, httplib::Response &res) {
    try {
      LOG_MSG << "POST /api/update";
      auto nof = imp->app_.update();
      json response = { {"status", "updated"}, {"nof_files", std::to_string(nof)} };
      res.set_content(response.dump(), "application/json");
    } catch (const std::exception &e) {
      json error = { {"error", e.what()} };
      res.status = 500;
      res.set_content(error.dump(), "application/json");
      Impl::errorCounter_++;
    }
    Impl::requestCounter_++;
    });

  server.Post("/api/chat", [this](const httplib::Request &req, httplib::Response &res) {
    const auto start = std::chrono::steady_clock::now();
    try {
      LOG_MSG << "POST /api/chat";
      // format for messages field in request
      /*
      {
        "messages": [
          {"role": "system", "content": "Keep it short."},
          {"role": "user", "content": "What is the capital of France?"}
        ],
        "sourceids": [
          "../embedder_cpp/src/main.cpp", "../embedder_cpp/include/settings.h"
        ],
        "attachments": [
          { "filename": "filename1.cpp", "content": "..text file content 1.."},
          { "filename": "filename2.cpp", "content": "..text file content 2.."},
        ],
        "temperature": 0.2,
        "max_tokens": 800,
        "targetapi": "xai",
        "ctxratio": 0.5,
        "attachedonly": false
      }
      */
      json request = json::parse(req.body);
      if (!request.contains("messages") || !request["messages"].is_array() || request["messages"].empty()) {
        throw std::invalid_argument("'messages' field required and must be non-empty array");
      }
      const auto messagesJson = request["messages"];
      if (0 == messagesJson.size()) {
        throw std::invalid_argument("'messages' array must be non-empty");
      }
      if (!messagesJson.back().contains("role") || !messagesJson.back().contains("content")) {
        throw std::invalid_argument("Last message must have 'role' and 'content' fields");
      }

      std::string role = messagesJson.back()["role"];
      if (role != "user") {
        throw std::invalid_argument("Last message role must be 'user', got: " + role);
      }
      std::string question = messagesJson.back()["content"].get<std::string>();

      auto attachmentsJson = request["attachments"];
      auto attachments = parseAttachments(attachmentsJson);

      std::vector<std::string> sources;
      if (request.contains("sourceids")) {
        auto sourceidsJson = request["sourceids"];
        for (const auto &sid : sourceidsJson) {
          if (sid.is_string()) {
            vecAddIfUnique(sources, sid.get<std::string>());
          }
        }
      }

      auto apiConfig = getTargetApi(request, imp->app_);

      const float temperature = request.value("temperature", imp->app_.settings().generationDefaultTemperature());
      const size_t maxTokens = request.value("max_tokens", imp->app_.settings().generationDefaultMaxTokens());
      const float contextSizeRatio = request.value("ctxratio", 0.9f);
      const bool attachedOnly = request.value("attachedonly", false);

      res.set_header("Content-Type", "text/event-stream");
      res.set_header("Cache-Control", "no-cache");
      res.set_header("Connection", "keep-alive");

      res.set_chunked_content_provider(
        "text/event-stream",
        [this, messagesJson, question, temperature, contextSizeRatio, attachedOnly, attachments, sources, maxTokens, apiConfig]
        (size_t offset, httplib::DataSink &sink) {

          auto packPayload = [](std::string data) {
            // SSE format requires "data: <payload>\n\n"
            nlohmann::json payload = { {"content", std::move(data)} };
            return "data: " + payload.dump() + "\n\n";
            };

          auto initialInfo = packPayload("[meta]Searching for relevant content");
          sink.write(initialInfo.data(), initialInfo.size());

          auto onInfo = [packPayload, &sink](std::string_view info)
            {
              auto s = packPayload(std::string{ "[meta]" } + std::string{ info.data(), info.size() });
              sink.write(s.data(), s.size());
            };

          const auto [orderedResults, usedTokens] = processInputResults(imp->app_, apiConfig, question, attachments, sources, 
            contextSizeRatio, attachedOnly, onInfo
          );

          CompletionClient completionClient(apiConfig, imp->app_.settings().generationTimeoutMs(), imp->app_);
          try {
            const std::string fullResponse = completionClient.generateCompletion(
              messagesJson, orderedResults, temperature, maxTokens,
              [&sink, packPayload](const std::string &chunk) {
#ifdef _DEBUG2
                LOG_MSG << chunk;
#endif
                std::string sse = packPayload(chunk);
                bool success = sink.write(sse.data(), sse.size());
                if (!success) {
                  return; // Client disconnected
                }
              });

#ifdef _DEBUG2
            testStreaming([&sink](const std::string &chunk) {
              if (!sink.write(chunk.data(), chunk.size())) {
                return; // client disconnected
              }
              });
#endif
            size_t resTokens = imp->app_.tokenizer().countTokensWithVocab(fullResponse);
            onInfo(fmt::format("Response token count {}", resTokens));

            auto costReq = apiConfig.inputTokensPrice(usedTokens);
            auto costRes = apiConfig.outputTokensPrice(resTokens);
            auto costTotal = costReq + costRes;
            if (costTotal == 0)
              onInfo("Total cost incurred: 0");
            else
              onInfo(fmt::format("Approx. cost incurred: ${:.4f} (input: {:.4f}, output: {:.4f})", costTotal, costReq, costRes));

            // Add sources information
            nlohmann::json sourcesJson;
            std::set<std::string> distinctSources;
            for (const auto &result : orderedResults) {
              if (distinctSources.insert(result.sourceId).second) {
                sourcesJson.push_back(result.sourceId);
              }
            }

            // Send sources information as a separate SSE message
            nlohmann::json sourcesPayload = {
              {"sources", sourcesJson},
              {"type", "context_sources"}
            };
            std::string sourcesSse = "data: " + sourcesPayload.dump() + "\n\n";
            sink.write(sourcesSse.data(), sourcesSse.size());


            std::string done = "data: [DONE]\n\n";
            sink.write(done.data(), done.size());
            sink.done();
          } catch (const std::exception &e) {
            std::string error = "data: {\"error\": \"" + std::string(e.what()) + "\"}\n\n";
            sink.write(error.data(), error.size());
            sink.done();
          }
          //LOG_MSG << "set_chunked_content_provider: callback DONE.";
          return true;
        }
      );

    } catch (const std::exception &e) {
      json error = { {"error", e.what()} };
      res.status = 400;
      res.set_content(error.dump(), "application/json");
      Impl::errorCounter_++;
    }
    Impl::requestCounter_++;
    Impl::chatCounter_++;
    recordDuration(start, Impl::avgChatTimeMs_);
    });

    server.Post("/api/fim", [this](const httplib::Request &req, httplib::Response &res) {
      const auto start = std::chrono::steady_clock::now();
      try {
        LOG_MSG << "POST /api/fim";
        json request = json::parse(req.body);

        if (!request.contains("prefix") || !request["prefix"].is_string()) {
          throw std::invalid_argument("'prefix' field required and must be a string");
        }
        if (!request.contains("suffix") || !request["suffix"].is_string()) {
          throw std::invalid_argument("'suffix' field required and must be a string");
        }

        std::string prefix = request["prefix"].get<std::string>();
        std::string suffix = request["suffix"].get<std::string>();
        std::string filename = request.value("filename", std::string{});
        filename = std::filesystem::path(filename).lexically_normal().generic_string();

        if (request.value("encoding", "") == "base64") {
          prefix = base64_decode(prefix);
          suffix = base64_decode(suffix);
        }

        auto apiConfig = getTargetApi(request, imp->app_);

        const float temperature = request.value("temperature", imp->app_.settings().generationDefaultTemperature());
        const size_t maxTokens = request.value("max_tokens", imp->app_.settings().generationDefaultMaxTokens());
        const float contextSizeRatio = request.value("ctxratio", 0.5f);
        std::vector<std::string> stops = request.value("stop", std::vector<std::string>{});

        const auto searchResults = processInputResults(imp->app_, apiConfig, prefix, {}, {filename}, contextSizeRatio, {}, nullptr);

        LOG_MSG << "Generating FIM with prefix length" << prefix.size() << "and suffix length" << suffix.size();
        CompletionClient completionClient(apiConfig, imp->app_.settings().generationTimeoutMs(), imp->app_);
        std::string fullResponse = completionClient.generateFim(prefix, suffix, stops, temperature, maxTokens, searchResults.first);
        LOG_MSG << "[FIM] Generated tokens:" << imp->app_.tokenizer().countTokensWithVocab(fullResponse);
        json response = { {"completion", fullResponse} };
        res.set_content(response.dump(), "application/json");
        Impl::requestCounter_++;
      } catch (const std::exception &e) {
        json error = { {"error", e.what()} };
        res.status = 400;
        res.set_content(error.dump(), "application/json");
        Impl::errorCounter_++;
      }
      recordDuration(start, Impl::avgChatTimeMs_); // reuse chat timing metric
      });

  server.Get("/api/settings", [this](const httplib::Request &, httplib::Response &res) {
    try {
      LOG_MSG << "GET /api/settings";
      nlohmann::json apisJson;
      const auto &cur = imp->app_.settings().generationCurrentApi();
      const auto &apis = imp->app_.settings().generationApis();
      for (const auto &api : apis) {
        nlohmann::json apiObj;
        apiObj["id"] = api.id;
        apiObj["name"] = api.name;
        apiObj["url"] = api.apiUrl;
        apiObj["model"] = api.model;
        apiObj["current"] = (api.id == cur.id);
        apiObj["combinedPrice"] = api.combinedPrice();
        apisJson.push_back(apiObj);
      }
      nlohmann::json responseJson;
      responseJson["completionApis"] = apisJson;
      responseJson["currentApi"] = cur.id;
      res.status = 200;
      res.set_content(responseJson.dump(2), "application/json");
    } catch (const std::exception &e) {
      nlohmann::json errorJson;
      errorJson["error"] = "Failed to load settings";
      errorJson["message"] = e.what();
      res.status = 500;
      res.set_content(errorJson.dump(2), "application/json");
      Impl::errorCounter_++;
    }
    Impl::requestCounter_++;
    });

  server.Get("/api/instances", [this](const httplib::Request &, httplib::Response &res) {
    try {
      LOG_MSG << "GET /api/instances";
      auto instances = imp->app_.registry().getActiveInstances();
      json response = {
          {"instances", instances},
          {"current_instance", imp->app_.registry().getInstanceId()}
      };
      res.status = 200;
      res.set_content(response.dump(2), "application/json");
    } catch (const std::exception &e) {
      nlohmann::json errorJson;
      errorJson["error"] = "Failed to fetch instances";
      errorJson["message"] = e.what();
      res.status = 500;
      res.set_content(errorJson.dump(2), "application/json");
      Impl::errorCounter_++;
    }
    Impl::requestCounter_++;
    });

  server.Post("/api/shutdown", [&](const httplib::Request &req, httplib::Response &res) {
    LOG_MSG << "POST /api/shutdown";
    Impl::requestCounter_++;
    auto appKey = req.get_header_value("X-App-Key");
    if (!imp->app_.isValidPrivateAppKey(appKey) && !requireAuth(auth, req, res, nullptr)) return;
    try {
      imp->app_.requestShutdownAsync();
      json response = {
          {"status", "success"},
          {"message", "Shutdown initiated"}
      };
      res.set_content(response.dump(), "application/json");
    } catch (const std::exception &e) {
      json error = { {"error", e.what()} };
      res.status = 500;
      res.set_content(error.dump(), "application/json");
      Impl::errorCounter_++;
    }
    });

  server.Get("/api/metrics", [this](const httplib::Request &, httplib::Response &res) {
    LOG_MSG << "GET /api/metrics";

    auto &app = imp->app_;
    auto stats = app.db().getStats();

    json metrics = {
        {"service", {
            {"version", EMBEDDER_VERSION},
            {"uptime_seconds", app.uptimeSeconds()},
            {"started_at", app.startTimestamp()}
        }},
        {"database", {
            {"total_chunks", stats.totalChunks},
            {"vector_count", stats.vectorCount},
            {"deleted_count", stats.deletedCount},
            {"active_count", stats.activeCount},
            {"db_size_mb", app.dbSizeMB()},
            {"index_size_mb", app.indSizeMB()}
        }},
        {"requests", {
            {"total", Impl::requestCounter_.load()},
            {"search", Impl::searchCounter_.load()},
            {"chat", Impl::chatCounter_.load()},
            {"embed", Impl::embedCounter_.load()},
            {"errors", Impl::errorCounter_.load()}
        }},
        {"performance", {
            {"avg_search_ms", Impl::avgSearchTimeMs_.load()},
            {"avg_embedding_ms", Impl::avgEmbedTimeMs_.load()},
            {"avg_chat_ms", Impl::avgChatTimeMs_.load()}
        }},
        {"system", {
            {"last_update", app.lastUpdateTimestamp()},
            {"sources_indexed", stats.sources.size()}
        }}
    };
    res.set_content(metrics.dump(2), "application/json");
    Impl::requestCounter_++;
    });

  server.Get("/metrics", [this](const httplib::Request &, httplib::Response &res) {
    LOG_MSG << "GET /metrics";

    std::stringstream prometheus;

    // Request counters
    prometheus << "# HELP embedder_requests_total Total requests\n";
    prometheus << "# TYPE embedder_requests_total counter\n";
    prometheus << "embedder_requests_total " << Impl::requestCounter_ << "\n\n";

    prometheus << "# HELP embedder_search_requests_total Total search requests\n";
    prometheus << "# TYPE embedder_search_requests_total counter\n";
    prometheus << "embedder_search_requests_total " << Impl::searchCounter_ << "\n\n";

    prometheus << "# HELP embedder_chat_requests_total Total chat requests\n";
    prometheus << "# TYPE embedder_chat_requests_total counter\n";
    prometheus << "embedder_chat_requests_total " << Impl::chatCounter_ << "\n\n";

    prometheus << "# HELP embedder_embed_requests_total Total embedding requests\n";
    prometheus << "# TYPE embedder_embed_requests_total counter\n";
    prometheus << "embedder_embed_requests_total " << Impl::embedCounter_ << "\n\n";

    prometheus << "# HELP embedder_error_requests_total Total error requests\n";
    prometheus << "# TYPE embedder_error_requests_total counter\n";
    prometheus << "embedder_error_requests_total " << Impl::errorCounter_ << "\n\n";

    // Performance metrics (moving averages)
    prometheus << "# HELP embedder_avg_search_time_ms Average search time in milliseconds\n";
    prometheus << "# TYPE embedder_avg_search_time_ms gauge\n";
    prometheus << "embedder_avg_search_time_ms " << Impl::avgSearchTimeMs_.load() << "\n\n";

    prometheus << "# HELP embedder_avg_chat_time_ms Average chat time in milliseconds\n";
    prometheus << "# TYPE embedder_avg_chat_time_ms gauge\n";
    prometheus << "embedder_avg_chat_time_ms " << Impl::avgChatTimeMs_.load() << "\n\n";

    prometheus << "# HELP embedder_avg_embed_time_ms Average embedding time in milliseconds\n";
    prometheus << "# TYPE embedder_avg_embed_time_ms gauge\n";
    prometheus << "embedder_avg_embed_time_ms " << Impl::avgEmbedTimeMs_.load() << "\n\n";

    // Database metrics
    try {
      auto stats = imp->app_.db().getStats();
      prometheus << "# HELP embedder_database_chunks_total Total chunks in database\n";
      prometheus << "# TYPE embedder_database_chunks_total gauge\n";
      prometheus << "embedder_database_chunks_total " << stats.totalChunks << "\n\n";

      prometheus << "# HELP embedder_database_vectors_total Total vectors in database\n";
      prometheus << "# TYPE embedder_database_vectors_total gauge\n";
      prometheus << "embedder_database_vectors_total " << stats.vectorCount << "\n\n";

      prometheus << "# HELP embedder_database_sources_total Total sources in database\n";
      prometheus << "# TYPE embedder_database_sources_total gauge\n";
      prometheus << "embedder_database_sources_total " << stats.sources.size() << "\n\n";
    } catch (const std::exception &e) {
      prometheus << "# Database metrics unavailable: " << e.what() << "\n\n";
    }

    res.set_content(prometheus.str(), "text/plain");
    Impl::requestCounter_++;
    });

  server.Get("/api", [](const httplib::Request &, httplib::Response &res) {
    LOG_MSG << "GET /api";
    json info = {
        {"name", "Embeddings RAG API"},
        {"version", EMBEDDER_VERSION},
        {"endpoints", {
            {"GET /api/setup", "Fetch setup configuration"},
            {"GET /api/health", "Health check"},
            {"GET /api/documents", "Get documents"},
            {"GET /api/stats", "Database statistics"},
            {"GET /api/settings", "Available APIs"},
            {"GET /api/instances", "List of running instances"},
            {"GET /api/metrics", "Service and database metrics"},
            {"GET /metrics", "Prometheus-compatible metrics"},
            {"POST /api/setup", "Setup configuration"},
            {"POST /api/search", "Semantic search"},
            {"POST /api/chat", "Chat with context (streaming)"},
            {"POST /api/fim", "Fill-In-Middle / Auto-complete"},
            {"POST /api/embed", "Generate embeddings"},
            {"POST /api/documents", "Add documents"},
            {"POST /api/update", "Trigger manual update"},
            {"POST /api/shutdown", "Initiate a shutdown"},
        }}
    };
    res.set_content(info.dump(2), "application/json");
    HttpServer::Impl::requestCounter_++;
    });

  //LOG_MSG << "\nStarting HTTP API server on port " << port << "...";
  LOG_MSG << "\nEndpoints:";
  LOG_MSG << "  GET  /api";
  LOG_MSG << "  GET  /metrics       - Prometheus-compatible format";
  LOG_MSG << "  GET  /api/metrics";
  LOG_MSG << "  GET  /api/instances - Returns currently running instances in 'serve' mode";
  LOG_MSG << "  GET  /api/setup";
  LOG_MSG << "  GET  /api/health";
  LOG_MSG << "  GET  /api/stats";
  LOG_MSG << "  GET  /api/settings";
  LOG_MSG << "  GET  /api/documents";
  LOG_MSG << "  POST /api/setup     - {\"...\"}";
  LOG_MSG << "  POST /api/search    - {\"query\": \"...\", \"top_k\": 5}";
  LOG_MSG << "  POST /api/embed     - {\"text\": \"...\"}";
  LOG_MSG << "  POST /api/documents - {\"content\": \"...\", \"source_id\": \"...\"}";
  LOG_MSG << "  POST /api/chat      - {\"messages\":[\"role\":\"...\", \"content\":\"...\"], \"temperature\": \"...\"}";
  LOG_MSG << "  POST /api/fim       - {\"prefix\": \"...\", \"suffix\":\"...\", \"temperature\": \"...\"}";
  LOG_MSG << "  POST /api/update    - Trigger manual update of sources";
  LOG_MSG << "  POST /api/shutdown  - Initiate server shutdown (expects X-App-Key header for the key)";
  LOG_MSG << "\nPress Ctrl+C to stop";
  return server.listen_after_bind();
}

void HttpServer::stop()
{
  if (imp->server_.is_running()) {
    LOG_MSG << "Server stopping...";
    imp->server_.stop();
    LOG_MSG << "Server stopped!";
  }
}
