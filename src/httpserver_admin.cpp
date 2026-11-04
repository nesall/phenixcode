#include "httpserver.h"
#include "httpserver_internal.h"
#include "app.h"
#include "database.h"
#include "settings.h"
#include "instregistry.h"
#include "auth.h"
#include "cutils.h"
#include "monitor.h"
#include "inference.h"
#include <3rdparty/fmt/format.h>
#include <3rdparty/nlohmann/json.hpp>
#include <httplib.h>
#include <utils_log/logger.hpp>
#include <exception>
#include <algorithm>
#include <vector>
#include <string>
#include <atomic>

using json = nlohmann::json;

using namespace httpserver_detail;

void HttpServer::registerSetupRoutes(AdminAuth &auth)
{
  auto &server = imp->server_;
  server.Post("/api/authenticate", [&](const httplib::Request &req, httplib::Response &res) {
    LOG_MSG << "POST /api/authenticate";
    std::string jwt;
    if (!requireAuth(auth, req, res, &jwt)) return; // Validate password
    json response = {
      { "status", "OK" },
      { "token", jwt }
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
          { "status", "success" },
          { "message", "Configuration generated successfully" }
      };
      res.set_content(response.dump(), "application/json");
    } catch (const std::exception &e) {
      json error = { { "error", e.what() } };
      res.status = 400;
      res.set_content(error.dump(), "application/json");
      LOG_MSG_ERROR << "Error in /api/setup: " << e.what();
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
      json error = { { "error", e.what() } };
      res.status = 500;
      res.set_content(error.dump(), "application/json");
      Impl::errorCounter_++;
      LOG_MSG_ERROR << "Error in /api/setup: " << e.what();
    }
    });

}

void HttpServer::registerAdminRoutes(AdminAuth &auth)
{
  auto &server = imp->server_;
  server.Get("/api/monitor/snapshot", [this](const httplib::Request &, httplib::Response &res) {
    //LOG_MSG << "GET /api/monitor/snapshot";
    try {
      auto snap = imp->app_.monitor().toJson();
      res.set_content(snap.dump(2), "application/json");
    } catch (const std::exception &e) {
      json error = { { "error", e.what() } };
      res.status = 500;
      res.set_content(error.dump(), "application/json");
      Impl::errorCounter_++;
      LOG_MSG_ERROR << "Failed to get monitor snapshot: " << e.what();
    }
    Impl::requestCounter_++;
    });

  server.Get("/api/logs/tail", [this](const httplib::Request &req, httplib::Response &res) {
    //LOG_MSG << "GET /api/logs/tail";
    try {
      std::string path = req.has_param("file") ? req.get_param_value("file") : "";
      MonitorTracker::LogFileType type{ path == "diagnostics" ? MonitorTracker::LogFileType::Diagnostics : MonitorTracker::LogFileType::Logs };
      int64_t offset = 0;
      if (req.has_param("offset")) {
        try { offset = std::stoll(req.get_param_value("offset")); } catch (...) { offset = 0; }
      }
      int64_t count = 500;
      if (req.has_param("count")) {
        try { count = std::stoll(req.get_param_value("count")); } catch (...) { count = 500; }
      }
      count = std::clamp<int64_t>(count, 1, 5000);
      offset = (std::max)(offset, int64_t(-100000)); // bound tail buffer

      json result = MonitorTracker::tailLogFile(type, offset, count, imp->app_.settings());
      if (result.value("status", "") == "error") {
        res.status = 400;
      }
      res.set_content(result.dump(), "application/json");
    } catch (const std::exception &e) {
      json error = {
        { "status", "error" },
        { "message", e.what() },
        { "lines", json::array() },
        { "total_lines", 0 }
      };
      res.status = 500;
      res.set_content(error.dump(), "application/json");
      Impl::errorCounter_++;
      LOG_MSG_ERROR << "Failed to tail logs: " << e.what();
    }
    Impl::requestCounter_++;
    });

  server.Get("/api/settings", [this](const httplib::Request &, httplib::Response &res) {
    try {
      LOG_MSG << "GET /api/settings";
      nlohmann::json apisJson;
      const auto cur = imp->app_.settings().generationCurrentApiId();
      const auto &apis = imp->app_.settings().providers().generationProviders();
      bool isAuto = imp->app_.settings().generationIsAuto();
      if (isAuto) {
        nlohmann::json autoApi;
        autoApi["id"] = kAutoApiId;
        autoApi["name"] = "Auto-router";
        autoApi["url"] = "";
        autoApi["model"] = "auto";
        autoApi["current"] = true;
        auto router = imp->app_.settings().autoRouterConfig();
        auto [minPrice, maxPrice] = router.estimateCostRange(imp->app_.settings());
        autoApi["combinedPrice"] = fmt::format("{:.2f} - {:.2f}", minPrice, maxPrice);
        apisJson.push_back(autoApi);
      }
      const auto &enabledProviders = imp->app_.settings().enabledGenerationProviders();
      for (const auto &api : apis) {
        if (std::find(enabledProviders.cbegin(), enabledProviders.cend(), api.id) == enabledProviders.end()) {
          continue; // skip disabled providers
        }
        nlohmann::json apiObj;
        apiObj["id"] = api.id;
        apiObj["name"] = api.name;
        apiObj["url"] = api.apiUrl;
        apiObj["model"] = api.model;
        apiObj["current"] = !isAuto && (api.id == cur);
        apiObj["combinedPrice"] = api.combinedPrice();
        apisJson.push_back(apiObj);
      }
      nlohmann::json responseJson;
      responseJson["completionApis"] = apisJson;
      responseJson["currentApi"] = isAuto ? std::string(kAutoApiId) : cur;
      res.status = 200;
      res.set_content(responseJson.dump(2), "application/json");
    } catch (const std::exception &e) {
      nlohmann::json errorJson;
      errorJson["error"] = "Failed to load settings";
      errorJson["message"] = e.what();
      res.status = 500;
      res.set_content(errorJson.dump(2), "application/json");
      Impl::errorCounter_++;
      LOG_MSG_ERROR << "Failed to load settings: " << e.what();
    }
    Impl::requestCounter_++;
    });

  server.Get("/api/instances", [this](const httplib::Request &, httplib::Response &res) {
    try {
      LOG_MSG << "GET /api/instances";
      auto instances = imp->app_.registry().getActiveInstances();
      json response = {
          { "instances", instances },
          { "current_instance", imp->app_.registry().getInstanceId() }
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
      LOG_MSG_ERROR << "Failed to fetch instances: " << e.what();
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
          { "status", "success" },
          { "message", "Shutdown initiated" }
      };
      res.set_content(response.dump(), "application/json");
    } catch (const std::exception &e) {
      json error = { { "error", e.what() } };
      res.status = 500;
      res.set_content(error.dump(), "application/json");
      Impl::errorCounter_++;
      LOG_MSG_ERROR << "Failed to initiate shutdown: " << e.what();
    }
    });


  server.Post("/api/providers/test", [this](const httplib::Request &req, httplib::Response &res) {
    try {
      LOG_MSG << "POST /api/providers/test";
      if (!httpserver_detail::requireAuth(imp->app_.auth(), req, res, nullptr)) return;
      json body = req.body.empty() ? json::object() : json::parse(req.body);
      std::string id = body.value("id", body.value("providerId", ""));
      if (id.empty() && req.has_param("id")) id = req.get_param_value("id");
      auto &settings = imp->app_.settings();
      ApiConfig api{};
      if (!id.empty()) {
        if (id == kAutoApiId) throw std::invalid_argument("Auto-router cannot be tested directly");
        const auto &catalog = settings.providers().generationProviders();
        auto it = std::find_if(catalog.begin(), catalog.end(), [&](const ApiConfig &a) { return a.id == id; });
        if (it == catalog.end()) throw std::invalid_argument("Provider not found: " + id);
        api = *it;
      } else {
        api.id = "test";
        api.apiUrl = body.value("url", body.value("apiUrl", ""));
        api.apiKey = utils::expandEnvVar(body.value("apiKey", body.value("api_key", body.value("key", ""))));
        api.model = body.value("model", "");
        api.maxTokensName = body.value("maxTokensName", "max_tokens");
        if (api.apiUrl.empty() || api.model.empty())
          throw std::invalid_argument("Missing url/model for test");
      }
      try {
        InferenceClient client(api, settings.generationTimeoutMs() ? settings.generationTimeoutMs() : 10000);
        json messages = json::array({
          { { "role", "system" },{ "content", "Keep it short." } },
          { { "role", "user" },{ "content", "What is the capital of France?" } }
          });
        std::string response = client.generateChat(messages, 1, 10);
        LOG_MSG_DEBUG << "Test successful, response: " << response;
        json ok = { { "success",true },{ "status","Success" },{ "message","Success" },{ "id",api.id } };
        res.set_content(ok.dump(), "application/json");
      } catch (const std::exception &e) {
        std::string m = e.what();
        std::string d = m;
        if (m.find("401") != std::string::npos || m.find("403") != std::string::npos) d = "Auth failed (bad api-key) - " + m;
        else if (m.find("404") != std::string::npos) d = "Not found (bad url/model) - " + m;
        else if (m.find("400") != std::string::npos) d = "Bad request (bad model/format) - " + m;
        else if (m.find("timed out") != std::string::npos || m.find("timeout") != std::string::npos) d = "Timeout - " + m;
        else if (!m.empty()) d = m;
        json fail = { { "success",false },{ "status","Failure" },{ "error",d },{ "message","Failure - " + d } };
        res.status = 200;
        res.set_content(fail.dump(), "application/json");
      }
    } catch (const std::exception &e) {
      json error = { { "error", e.what() } };
      res.status = 500;
      res.set_content(error.dump(), "application/json");
      Impl::errorCounter_++;
      LOG_MSG_ERROR << "Error in /api/providers/test" << e.what();
    }
    Impl::requestCounter_++;
    });

  server.Get("/api/metrics", [this](const httplib::Request &, httplib::Response &res) {
    LOG_MSG << "GET /api/metrics";

    auto &app = imp->app_;
    auto stats = app.db().getStats();

    json metrics = {
        { "service",{
            { "version", EMBEDDER_VERSION },
            { "uptime_seconds", app.uptimeSeconds() },
            { "started_at", app.startTimestamp() }
        } },
        { "database",{
            { "total_chunks", stats.totalChunks },
            { "vector_count", stats.vectorCount },
            { "deleted_count", stats.deletedCount },
            { "active_count", stats.activeCount },
            { "db_size_mb", app.dbSizeMB() },
            { "index_size_mb", app.indSizeMB() }
        } },
        { "requests",{
            { "total", Impl::requestCounter_.load() },
            { "search", Impl::searchCounter_.load() },
            { "chat", Impl::chatCounter_.load() },
            { "embed", Impl::embedCounter_.load() },
            { "errors", Impl::errorCounter_.load() }
        } },
        { "performance",{
            { "avg_search_ms", Impl::avgSearchTimeMs_.load() },
            { "avg_embedding_ms", Impl::avgEmbedTimeMs_.load() },
            { "avg_chat_ms", Impl::avgChatTimeMs_.load() }
        } },
        { "system",{
            { "last_update", app.lastUpdateTimestamp() },
            { "sources_indexed", stats.sources.size() }
        } }
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
      LOG_MSG_ERROR << "Failed to fetch database metrics: " << e.what();
    }

    res.set_content(prometheus.str(), "text/plain");
    Impl::requestCounter_++;
    });

}
