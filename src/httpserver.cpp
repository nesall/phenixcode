#include "httpserver.h"
#include "httpserver_internal.h"
#include "app.h"
#include "chunker.h"
#include "database.h"
#include "inference.h"
#include "settings.h"
#include "tokenizer.h"
#include "auth.h"
#include "3rdparty/base64.h"
#include "json_shim.h"
#include "monitor.h"
#include <httplib.h>
#include <utils_log/logger.hpp>
#include <iostream>
#include <chrono>
#include <exception>
#include <algorithm>
#include <vector>
#include <string>
#include <atomic>
#include <functional>
#include <filesystem>
#include <utility>

using json = nlohmann::json;
using namespace httpserver_detail;

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
      LOG_MSG_ERROR << "Error extracting password" << e.what();
    }
    return std::nullopt;
  }

} // anonymous namespace

namespace httpserver_detail {

  void recordDuration(const std::chrono::steady_clock::time_point &start, std::atomic<double> &v) {
    std::chrono::duration<double> duration = std::chrono::steady_clock::now() - start;
    double newSearchTimeMs = duration.count() * 1000.0;
    double oldVal = v.load();
    double newVal;
    do {
      newVal = oldVal * 0.9 + newSearchTimeMs * 0.1;
    } while (!v.compare_exchange_weak(oldVal, newVal));
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

} // namespace httpserver_detail



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
  imp->server_.set_default_headers({
      { "Access-Control-Allow-Origin", "*" },
      { "Access-Control-Allow-Methods", "GET, POST, OPTIONS" },
      { "Access-Control-Allow-Headers", "Content-Type, Authorization, X-App-Key" }
    });

  imp->server_.set_error_logger([](const httplib::Error &err, const httplib::Request *req) {
    LOG_MSG_ERROR << httplib::to_string(err) << "while processing request";
    if (req) {
      LOG_MSG_ERROR << ", client:" << req->get_header_value("X-Forwarded-For")
        << ", request: '" << req->method << req->path <<  req->version << "'"
        << ", host:" << req->get_header_value("Host");
    }
    });
}

HttpServer::~HttpServer()
{}

int HttpServer::bindToPortIncremental(int port)
{
  int nofTries = port == 0 ? 0 : 20;
  while (0 < nofTries && !isPortAvailable(port)) {
    if (--nofTries == 0) {
      LOG_MSG_ERROR << "Unable to reserve a port.";
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

  server.Options(".*", [](const httplib::Request &, httplib::Response &res) {
    res.status = 204;
    });

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

  registerSetupRoutes(auth);

  server.Get("/api/health", [](const httplib::Request &, httplib::Response &res) {
    LOG_MSG_DEBUG << "GET /api/health";
    json response = { {"status", "ok"} };
    res.set_content(response.dump(), "application/json");
    Impl::requestCounter_++;
    });

  server.Post("/api/search", [this](const httplib::Request &req, httplib::Response &res) {
    const auto start = std::chrono::steady_clock::now();
    bool isErr = false;
    size_t tokensIn = 0;
    try {
      LOG_MSG << "POST /api/search";
      json request = json::parse(req.body);
      std::string query = request["query"].get<std::string>();
      tokensIn = imp->app_.tokenizer().countTokensWithVocab(query);
      size_t top_k = request.value("top_k", 5);
      std::vector<float> queryEmbedding;
      try {
        EmbeddingClient embeddingClient(imp->app_.settings().embeddingCurrentApi(), imp->app_.settings().embeddingTimeoutMs());
        embeddingClient.generateEmbeddings(query, queryEmbedding, EmbeddingClient::EncodeType::Query);
      } catch (const std::exception &e) {
        LOG_MSG_WARN << "Unable to generate embeddings" << e.what() << "| Falling back to grep.";
        queryEmbedding.clear();
      }
      auto results = imp->app_.retrieve(query, queryEmbedding, top_k);
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
      isErr = true;
      json error = { {"error", e.what()} };
      res.status = 400;
      res.set_content(error.dump(), "application/json");
      Impl::errorCounter_++;
      LOG_MSG_ERROR << "Error in /api/search: " << e.what();
    }
    Impl::requestCounter_++;
    Impl::searchCounter_++;
    recordDuration(start, Impl::avgSearchTimeMs_);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    imp->app_.monitor().recordRequest("search", ms, isErr, tokensIn, 0);
    });

  // (one-off embedding without storage)
  server.Post("/api/embed", [this](const httplib::Request &req, httplib::Response &res) {
    const auto start = std::chrono::steady_clock::now();
    bool isErr = false;
    size_t tokensIn = 0;
    try {
      LOG_MSG << "POST /api/embed";
      json request = json::parse(req.body);
      std::string text = request["text"].get<std::string>();
      auto chunks = imp->app_.chunker().chunkText(text, "api-request", imp->app_.settings().chunkingSemantic());
      std::vector<std::string> texts;
      for (const auto &c : chunks) {
        texts.push_back(c.text);
        tokensIn += c.metadata.tokenCount;
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
      isErr = true;
      json error = { {"error", e.what()} };
      res.status = 400;
      res.set_content(error.dump(), "application/json");
      Impl::errorCounter_++;
      LOG_MSG_ERROR << "Error in /api/embed" << e.what();
    }
    Impl::requestCounter_++;
    Impl::embedCounter_++;
    recordDuration(start, Impl::avgEmbedTimeMs_);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    imp->app_.monitor().recordRequest("embed", ms, isErr, tokensIn, 0);
    imp->app_.monitor().recordEmbeddingCost(imp->app_.settings().embeddingCurrentApi().id, tokensIn, imp->app_.settings().providers());
    });

  server.Post("/api/documents", [this](const httplib::Request &req, httplib::Response &res) {
    try {
      LOG_MSG << "POST /api/documents";
      json request = json::parse(req.body);

      std::string content = request["content"].get<std::string>();
      std::string source_id = request["source_id"].get<std::string>();

      auto chunks = imp->app_.chunker().chunkText(content, source_id, imp->app_.settings().chunkingSemantic());

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
      LOG_MSG_ERROR << "Error in /api/documents" << e.what();
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
      LOG_MSG_ERROR << "Error in /api/documents" << e.what();
    }
    Impl::requestCounter_++;
    });

  server.Get("/api/stats", [this](const httplib::Request &, httplib::Response &res) {
    try {
      LOG_MSG << "GET /api/stats";
      auto stats = imp->app_.db().getStats();
      nlohmann::ordered_json response = {
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
      LOG_MSG_ERROR << "Error in /api/stats" << e.what();
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
      LOG_MSG_ERROR << "Error in /api/update" << e.what();
    }
    Impl::requestCounter_++;
    });

  registerGenerationRoutes(auth);

  registerAdminRoutes(auth);

  server.Get("/api", [](const httplib::Request &, httplib::Response &res) {
    LOG_MSG << "GET /api";
    json info = {
        { "name", "Embeddings RAG API" },
        { "version", EMBEDDER_VERSION },
        { "endpoints",{
            { "GET /api/setup", "Fetch setup configuration" },
            { "GET /api/health", "Health check" },
            { "GET /api/documents", "Get documents" },
            { "GET /api/stats", "Database statistics" },
            { "GET /api/settings", "Available APIs" },
            { "GET /api/instances", "List of running instances" },
            { "GET /api/metrics", "Service and database metrics" },
            { "GET /metrics", "Prometheus-compatible metrics" },
            { "GET /api/monitor/snapshot", "Take a snapshot of the database" },
            { "GET /api/logs/tail", "Tail log files with offset and maxBytes parameters" },
            { "POST /api/setup", "Setup configuration" },
            { "POST /api/search", "Semantic search" },
            { "POST /api/chat", "Chat with context (streaming)" },
            { "POST /api/fim", "Fill-In-Middle / Auto-complete" },
            { "POST /api/embed", "Generate embeddings" },
            { "POST /api/documents", "Add documents" },
            { "POST /api/update", "Trigger manual update" },
            { "POST /api/providers/test", "Test provider API connectivity" },
            { "POST /api/shutdown", "Initiate a shutdown" },
        } }
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
  LOG_MSG << "  GET  /api/monitor/snapshot";
  LOG_MSG << "  GET  /api/logs/tail - params: path, offset, maxBytes";
  LOG_MSG << "  POST /api/setup     - {\"...\"}";
  LOG_MSG << "  POST /api/search    - {\"query\": \"...\", \"top_k\": 5}";
  LOG_MSG << "  POST /api/embed     - {\"text\": \"...\"}";
  LOG_MSG << "  POST /api/documents - {\"content\": \"...\", \"source_id\": \"...\"}";
  LOG_MSG << "  POST /api/chat      - {\"messages\":[\"role\":\"...\", \"content\":\"...\"], \"temperature\": \"...\"}";
  LOG_MSG << "  POST /api/fim       - {\"prefix\": \"...\", \"suffix\":\"...\", \"temperature\": \"...\"}";
  LOG_MSG << "  POST /api/update    - Trigger manual update of sources";
  LOG_MSG << "  POST /api/providers/test - {\"url\": \"...\", \"key\": \"...\", \"model\":\"...\"}";
  LOG_MSG << "  POST /api/shutdown  - Initiate server shutdown (expects X-App-Key header for the key)";
  std::cout << "\nPress Ctrl+C to stop";
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
