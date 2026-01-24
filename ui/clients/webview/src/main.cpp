#include <httplib.h>
#include "json_shim.h"
#include <utils_log/logger.hpp>
#include "wb.h"
#include "procmngr.h"
#include "utils.h"
#include <filesystem>
#include <string>
#include <cassert>
#include <thread>
#include <atomic>
#include <unordered_map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include "WinDarkTitlebarImpl.h"
#else
#include <limits.h>
#include <unistd.h>
#endif


namespace fs = std::filesystem;

namespace {

  const std::string CONFIG_FNAME = "appconfig.json";

  struct AppConfigEx : public shared::AppConfig {
    int port = 8590;
    std::string host = "127.0.0.1";
    nlohmann::json toJson() const override {
      nlohmann::json j = shared::AppConfig::toJson();
      j["api"] = {
        {"host", host},
        {"port", port}
      };
      return j;
    }
  };

} // anonymous namespace

int main() {
  LOG_START;
  const std::string assetsPath = Webview::findWebAssets(WEB_ASSETS_BASE);
  if (assetsPath.empty()) {
    LOG_MSG << "Error: Could not find web assets (index.html)";
    LOG_MSG << "Please build the SPA client first:";
    LOG_MSG << "  cd ../spa-svelte && npm run build";
    return 1;
  }

  shared::ProcessesHolder procUtil;

  AppConfigEx prefs;
  shared::fetchOrCreatePrefsJson(prefs, CONFIG_FNAME, 
    [&prefs](nlohmann::json j) {
      if (j.contains("api") && j["api"].is_object()) {
        const auto &w = j["api"];
        if (w.contains("host") && w["host"].is_string()) {
          prefs.host = w["host"];
        }
        if (w.contains("port") && w["port"].is_number_integer()) {
          prefs.port = w["port"].get<int>();
        }
      }
    });
  if (prefs.host == "localhost") prefs.host = "127.0.0.1";

  LOG_MSG << "Loading Svelte app from: " << fs::absolute(assetsPath).string();

  httplib::Server svr;
  svr.set_mount_point("/", fs::absolute(assetsPath).string().c_str());

  svr.set_logger([](const auto &req, const auto &res) {
    LOG_MSG << req.method << req.path << "->" << res.status;
    });

  svr.Get("/api/.*", [&prefs](const httplib::Request &req, httplib::Response &res) {
    LOG_START;
    LOG_MSG << "svr.Get" << req.method << req.path;
    std::string host;
    int port;
    {
      std::lock_guard<std::mutex> lock(prefs.mutex_);
      host = prefs.host;
      port = prefs.port;
    }

    httplib::Client cli(host, port);
    cli.set_connection_timeout(0, 60 * 1000ull);
    auto result = cli.Get(req.path.c_str());
    if (result) {
      res.status = result->status;
      res.set_content(result->body, result->get_header_value("Content-Type"));
    } else {
      res.status = 503;
      res.set_content("{\"error\": \"Backend unavailable\"}", "application/json");
    }
    });

  svr.Post("/api/.*", [&prefs](const httplib::Request &req, httplib::Response &res) {
    LOG_START;
    LOG_MSG << "svr.Post" << req.method << req.path;

    std::string contentType = req.get_header_value("Content-Type");
    if (contentType.empty()) {
      contentType = "application/json";
    }

    // Special case for /api/chat - handle streaming
    if (req.path.find("/api/chat") != std::string::npos) {

      // Set up streaming response headers
      res.set_header("Content-Type", "text/event-stream");
      res.set_header("Cache-Control", "no-cache");
      res.set_header("Connection", "keep-alive");

      res.set_chunked_content_provider(
        "text/event-stream",
        [&prefs, req, &res, contentType](size_t offset, httplib::DataSink &sink) {
          LOG_MSG << "Starting chunked content provider, offset:" << offset;
          std::string host;
          int port;
          {
            std::lock_guard<std::mutex> lock(prefs.mutex_);
            host = prefs.host;
            port = prefs.port;
          }

          httplib::Client cli(host, port);
          cli.set_connection_timeout(0, 60 * 1000ull);

          httplib::Headers headers = { {"Accept", "text/event-stream"} };
          auto postRes = cli.Post(
            req.path.c_str(),
            headers,
            req.body,
            contentType,
            [&sink](const char *data, size_t len) -> bool {
              //LOG_MSG << "Received chunk: " << len << " bytes";
              return sink.write(data, len);
            }
          );

          sink.done();
          
          if (!postRes) {
            LOG_MSG << "Error: Backend streaming unavailable";
            res.status = 503;
            res.set_content("{\"error\": \"Backend streaming unavailable\"}", "application/json");
            return false;
          }
          if (postRes->status != 200) {
            LOG_MSG << "Error: Backend streaming returned status" << postRes->status;
            res.status = postRes->status;
          }
          
          LOG_MSG << "Streaming completed successfully";

          return true;
        });
      
    } else {
      // Regular POST handling for non-streaming endpoints
      std::string host;
      int port;
      {
        std::lock_guard<std::mutex> lock(prefs.mutex_);
        host = prefs.host;
        port = prefs.port;
      }

      httplib::Client cli(host, port);
      cli.set_connection_timeout(0, 60 * 1000ull);

      auto result = cli.Post(req.path.c_str(), req.body, contentType);

      if (result) {
        res.status = result->status;
        res.set_content(result->body, result->get_header_value("Content-Type"));
      } else {
        res.status = 503;
        res.set_content("{\"error\": \"Backend unavailable\"}", "application/json");
      }
    }
    });

  std::atomic<bool> serverReady{ false };

  const int serverPort = svr.bind_to_any_port("127.0.0.1");
  std::thread serverThread([&svr, &serverReady, serverPort]() {
    LOG_START;
    LOG_MSG << "Starting HTTP server on http://127.0.0.1:" << serverPort;
    serverReady = true;
    svr.listen_after_bind();
    LOG_MSG << "HTTP server stopped";
    });

  // Wait for server to be ready
  while (!serverReady.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  try {
    LOG_MSG << "Using window size, w" << prefs.width << ", h" << prefs.height;


    Webview w(
#ifdef _DEBUG
      true
#else
      false
#endif
      , nullptr);
    w.setAppIcon(WEB_ASSETS_BASE, "logo");
    w.set_title("PhenixCode Assistant - v" EMBEDDER_VERSION " [build date: " __DATE__ "]");
    w.set_size(prefs.width, prefs.height, WEBVIEW_HINT_NONE);
    w.onDestroyCallback_ = [&w, &prefs]
      {
        auto [width, height] = w.getWindowSize();
        LOG_MSG << "Saving window size [" << LOG_NOSPACE << width << ", " << height << "]";
        {
          std::lock_guard<std::mutex> lock(prefs.mutex_);
          prefs.width = width;
          prefs.height = height;
          savePrefsToFile(prefs, CONFIG_FNAME);
        }
      };
#ifdef _WIN32
    HWND hWnd = static_cast<HWND>(w.window().value());
    WinDarkTitlebarImpl winDarkImpl;
    winDarkImpl.init();
    auto changeTheme = [&winDarkImpl, hWnd](bool dark) {
      winDarkImpl.setTitleBarTheme(hWnd, dark);
      };
    changeTheme(prefs.uiPrefs["darkOrLight"] == "dark");
#else
    auto changeTheme = [](bool) {};
#endif

    w.bind("setPersistentKey", [&prefs, &svr, changeTheme](const std::string &id, const std::string &data, void *)
      {
        LOG_MSG << "setPersistentKey:" << id << data;
        try {
          auto j = nlohmann::json::parse(data);
          if (j.is_array() && 2 == j.size()) {
            std::string key = j[0];
            std::string val = j[1];
            LOG_MSG << key << val;
            if (!key.empty()) {
              std::lock_guard<std::mutex> lock(prefs.mutex_);
              prefs.uiPrefs[key] = val;
              savePrefsToFile(prefs, CONFIG_FNAME);
              LOG_MSG << "Saved persistent key:" << key;
#ifdef _WIN32
              if (key == "darkOrLight") {
                changeTheme(val == "dark");
              }
#endif
              return;
            }
          }
        } catch (const std::exception &ex) {
          LOG_MSG << ex.what();
        }
      }, nullptr
    );

    w.bind("getPersistentKey", [&prefs, &svr](const std::string &data) -> std::string
      {
        LOG_MSG << "getPersistentKey:" << data;
        try {
          auto j = nlohmann::json::parse(data);
          if (j.is_array() && 0 < j.size()) {
            std::string key = j[0].get<std::string>();
            std::lock_guard<std::mutex> lock(prefs.mutex_);
            auto it = prefs.uiPrefs.find(key);
            if (it != prefs.uiPrefs.end()) {
              return nlohmann::json(it->second).dump();
            }
          }
        } catch (const std::exception &ex) {
          LOG_MSG << ex.what();
        }
        return "null";
      }
    );

    w.bind("setServerUrl", [&prefs, &svr](const std::string &url) -> std::string
      {
        LOG_MSG << "setServerUrl:" << url;
        try {
          size_t hostStart = url.find("://") + 3;
          size_t portStart = url.find(":", hostStart);
          size_t pathStart = url.find("/", hostStart);
          std::lock_guard<std::mutex> lock(prefs.mutex_);
          std::string newHost;
          int newPort = prefs.port;
          if (portStart != std::string::npos) {
            newHost = url.substr(hostStart, portStart - hostStart);
            std::string portStr = url.substr(portStart + 1, pathStart - portStart - 1);
            newPort = std::stoi(portStr);
          } else {
            newHost = url.substr(hostStart, pathStart - hostStart);
          }
          if (newHost == "localhost") newHost = "127.0.0.1";
          prefs.host = newHost;
          prefs.port = newPort;
          savePrefsToFile(prefs, CONFIG_FNAME);
          return "{\"status\": \"success\", \"message\": \"Server connection updated\"}";
        } catch (const std::exception &e) {
          LOG_MSG << "Error updating server connection:" << e.what();
          return "{\"status\": \"error\", \"message\": \"" + std::string(e.what()) + "\"}";
        }
      }
    );

    w.bind("getServerUrl", [&prefs, &svr](const std::string &) -> std::string
      {
        std::lock_guard<std::mutex> lock(prefs.mutex_);
        LOG_MSG << "getServerUrl" << prefs.host << prefs.port;
        try {
          //std::string url = std::format("http://{}:{}", prefs.host, prefs.port);
          std::string url = "http://" + prefs.host + ":" + std::to_string(prefs.port);
          return nlohmann::json(url).dump();
        } catch (const std::exception &ex) {
          LOG_MSG << ex.what();
        }
        return "null";
      }
    );

    w.bind("getSettingsFileProjectId", [](const std::string &data) -> std::string
      {
        LOG_MSG << "getSettingsFileProjectId";
        try {
          auto j = nlohmann::json::parse(data);
          if (j.is_array() && 0 < j.size()) {
            auto id = shared::getProjectId(j[0]);
            LOG_MSG << "  \"" << LOG_NOSPACE << id << "\"";
            return nlohmann::json(id).dump();
          }
        } catch (const std::exception &ex) {
          LOG_MSG << ex.what();
        }
        return "null";
      }
    );

    w.bind("startEmbedder", [&procUtil](const std::string &data) -> std::string
      {
        LOG_MSG << "startEmbedder:" << data;
        nlohmann::json res;
        try {
          auto j = nlohmann::json::parse(data);
          if (j.is_array() && 1 < j.size()) {
            std::string exePath = j[0].get<std::string>();
            std::string configPath = j[1].get<std::string>();
            if (!std::filesystem::exists(exePath))
              throw std::runtime_error("Embedder executable not found: " + exePath);
            if (!std::filesystem::exists(configPath))
              throw std::runtime_error("Embedder config file not found: " + configPath);
            auto appKey = shared::generateAppKey();
            auto projectId = shared::getProjectId(configPath);
            auto proc = procUtil.getOrCreateProcess(appKey, projectId);
            assert(proc);
            if (proc->startProcess(exePath, { "--config", configPath, "serve", "--appkey", appKey })) {
              res["status"] = "success";
              res["message"] = "Embedder started successfully";
              res["projectId"] = projectId;
              res["appKey"] = appKey; // use to id proc
              LOG_MSG << "Started embedder process" << proc->getProcessId() << "for projectId" << projectId;
            } else {
              procUtil.discardProcess(appKey);
              throw std::runtime_error("Failed to start embedder process");
            }
          } else {
            throw std::runtime_error("Invalid parameters for startEmbedder");
          }
        } catch (const std::exception &ex) {
          LOG_MSG << ex.what();
          res["status"] = "error";
          res["message"] = ex.what();
        }
        return res.dump();
      }
    );

    w.bind("stopEmbedder", [&prefs, &procUtil](const std::string &data) -> std::string
      {
        LOG_MSG << "stopEmbedder:" << data;
        nlohmann::json res;
        try {
          auto j = nlohmann::json::parse(data);
          if (j.is_array() && 2 < j.size()) {
            const std::string appKey = j[0].get<std::string>();
            auto proc = procUtil.getProcessWithApiKey(appKey);
            if (!proc)
              throw std::runtime_error("Embedder appKey not found: " + appKey);
            std::string host = j[1].get<std::string>();
            if (host.empty())
              throw std::runtime_error("Invalid host for embedder shutdown");
            const int port = j[2].get<int>();
            if (port <= 0)
              throw std::runtime_error("Invalid port for embedder shutdown");
            if (host == "localhost") host = "127.0.0.1";
            assert(proc);
            httplib::Client cli(host, port);
            httplib::Headers headers = { {"X-App-Key", appKey} };
            auto result = cli.Post("/api/shutdown", headers, "", "application/json");
            if (result && result->status == 200) {
              LOG_MSG << "Shutdown request sent to embedder process" << proc->getProcessId();
            } else {
              LOG_MSG << "Failed to send shutdown request to embedder process" << proc->getProcessId();
            }
            if (proc->waitForCompletion(10000)) {
              LOG_MSG << "Embedder process" << proc->getProcessId() << "exited cleanly";
            } else {
              LOG_MSG << "Embedder process" << proc->getProcessId() << "did not exit in time, terminating...";
              proc->stopProcess();
            }
            procUtil.discardProcess(appKey);
            res["status"] = "success";
            res["message"] = "Embedder stopped successfully";
          } else {
            throw std::runtime_error("Invalid parameters for startEmbedder");
          }
        } catch (const std::exception &ex) {
          LOG_MSG << ex.what();
          res["status"] = "error";
          res["message"] = ex.what();
        }
        return res.dump();
      }
    );
    
    w.init(R"(
      window.cppApi = {
        setServerUrl,
        getServerUrl,
        setPersistentKey,
        getPersistentKey,
        getSettingsFileProjectId,
        startEmbedder,
        stopEmbedder,
      };
      window.addEventListener('error', function(e) {
        console.error('JS Error:', e.message, e.filename, e.lineno);
      });
      console.log('Webview initialized, location:', window.location.href);
    )");

    const std::string url = "http://127.0.0.1:" + std::to_string(serverPort);
    LOG_MSG << "Navigating to:" << url;

    w.navigate(url);
    w.run();

    LOG_MSG << "Webview closed by user.";

    LOG_MSG << "Stopping HTTP server...";
  } catch (const std::exception &e) {
    LOG_MSG << "Webview error:" << e.what();
  }
  
  // Graceful shutdown of self-started processes
  {
    std::string host;
    int port;
    {
      std::lock_guard<std::mutex> lock(prefs.mutex_);
      host = prefs.host;
      port = prefs.port;
    }
    httplib::Client cli(host, port);
    auto result = cli.Get("/api/instances");
    if (result && result->status == 200) {
      try {
        auto j = nlohmann::json::parse(result->body);
        if (j.is_object()) {
          j = j["instances"];
          for (const auto &item : j) {
            if (item.contains("project_id") && item["project_id"].is_string()) {
              std::string project_id = item["project_id"].get<std::string>();
              std::string host = item.value("host", "");
              int port = item.value("port", 0);
              if (host.empty() || port <= 0) {
                LOG_MSG << "Invalid host/port for instance with project_id:" << project_id;
                continue;
              }
              std::string appKey = procUtil.getApiKeyFromProjectId(project_id);
              if (appKey.empty()) {
                LOG_MSG << "Embedder process" << project_id << "not started by this client. Skipped.";
                continue;
              }
              if (host == "localhost") host = "127.0.0.1";
              httplib::Client cli(host, port);
              httplib::Headers headers = { {"X-App-Key", appKey} };
              auto result = cli.Post("/api/shutdown", headers, "", "application/json");
              if (result && result->status == 200) {
                LOG_MSG << "Shutdown request sent to embedder process for project_id:" << project_id;
              } else {
                LOG_MSG << "Failed to send shutdown request to embedder process for project_id:" << project_id;
              }
            }
          }
        }
      } catch (const std::exception &e) {
        LOG_MSG << "Error parsing /api/instances response:" << e.what();
      }
    } else {
      LOG_MSG << "Failed to query /api/instances";
    }
    procUtil.waitToStopThenTerminate();
  }

  svr.stop();
  if (serverThread.joinable()) {
    serverThread.join();
    LOG_MSG << "HTTP server thread joined cleanly";
  }

  return 0;
}