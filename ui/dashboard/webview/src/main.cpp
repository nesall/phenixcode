#include <httplib.h>
#include <utils_log/logger.hpp>
#include "wb.h"
#include "procmngr.h"
#include "utils.h"
#include "instregistry.h"
#include "settings.h"
#include "3rdparty/portable-file-dialogs.h"
#include "json_shim.h"
#include <filesystem>
#include <string>
#include <cassert>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <chrono>
#include <functional>
#include <mutex>

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

  const std::string CONFIG_FNAME = "admconfig.json";
  //const std::string PROJECTS_FOLDER_NAME = "phenixcode_projects";
  const std::string PROJECT_REFS_FNAME = "proj-refs.json";
  const fs::path projectsFolderPath() {
    return (fs::path(shared::getExecutableDir()) / fs::path(PROJECTS_FOLDER_NAME));
  }
  const fs::path defaultSettingsJsonPath() {
    return (fs::path(shared::getExecutableDir()) / fs::path("settings.default.json"));
  }
  const fs::path projectRefsPath() {
    return (projectsFolderPath() / fs::path(PROJECT_REFS_FNAME));
  }
  void ensureProjectsFolderExist() {
    if (!fs::exists(projectsFolderPath())) {
      if (fs::create_directories(projectsFolderPath())) {
        LOG_MSG << "Created projects folder at:" << fs::absolute(projectsFolderPath()).string();
      } else {
        throw std::runtime_error("Failed to create projects folder at: " + fs::absolute(projectsFolderPath()).string());
      }
    }
  }

  bool validateProjectItemArg(nlohmann::json j) {
    if (!j.contains("settingsFilePath")) {
      throw std::runtime_error("Missing settingsFilePath");
    }
    if (!j.contains("jsonData")) {
      throw std::runtime_error("Missing jsonData");
    }
    std::string fname = j["settingsFilePath"].get<std::string>();
    if (fs::exists(fname)) {
      if (!j.contains("jsonData")) {
        throw std::runtime_error("Missing jsonData");
      }
      if (!j["jsonData"].contains("source")) {
        throw std::runtime_error("Missing jsonData.source");
      }
#if 0
      auto projectId = j["jsonData"]["source"].value("project_id", std::string{});
      // Note: projectId can be empty (auto-generation)

      nlohmann::json jFile;
      {
        std::ifstream file(fname);
        file >> jFile;
      }
      if (!jFile.contains("source")) {
        throw std::runtime_error("Invalid project settings file, missing source");
      }
      auto fileProjectId = jFile["source"].value("project_id", std::string{});
      if (fileProjectId != projectId) {
        throw std::runtime_error("ProjectId mismatch");
      }
#endif
      return true;
    }
    return false;
  }

  bool testInstanceStatus(bool testAlive, const std::string &configPath, const std::string instanceId, int steps = 16) {
    InstanceRegistry registry;
    int k = (std::max)(steps, 1);
    while (--k) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      auto instances = registry.getActiveInstances();
      bool found = false;
      for (const auto &a : instances) {
        if (
          a["id"].get<std::string>() == instanceId ||
          fs::path(a["config"].get<std::string>()).lexically_normal() == fs::path(configPath).lexically_normal()
          ) {
          LOG_MSG << "[Found] instance" << a["id"].get<std::string>();
          found = true;
          if (testAlive) {
            break;
          }
        }
      }
      if (testAlive) {
        if (found) return true;
      } else {
        if (!found) return true;
      }
    }
    return false;
  }
  bool testInstanceDead(const std::string &configPath, const std::string instanceId, int steps = 16) {
    return testInstanceStatus(false, configPath, instanceId, steps);
  }
  bool testInstanceAlive(const std::string &configPath, const std::string instanceId, int steps = 16) {
    return testInstanceStatus(true, configPath, instanceId, steps);
  }

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

  shared::AppConfig prefs;
  shared::fetchOrCreatePrefsJson(prefs, CONFIG_FNAME);

  LOG_MSG << "Loading Svelte app from: " << fs::absolute(assetsPath).string();

  httplib::Server svr;
  svr.set_mount_point("/", fs::absolute(assetsPath).string().c_str());

  std::atomic<bool> serverReady{ false };

  const int serverPort = svr.bind_to_any_port("127.0.0.1");
  std::thread serverThread([&svr, &serverReady, serverPort]() {
    LOG_START;
    LOG_MSG << "Starting HTTP server on http://127.0.0.1:" << LOG_NOSPACE << serverPort;
    serverReady = true;
    svr.listen_after_bind();
    LOG_MSG << "HTTP server stopped";
    });

  // Wait for server to be ready
  while (!serverReady.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  //std::this_thread::sleep_for(std::chrono::milliseconds(100));

  try {
    LOG_MSG << "Using window size w" << prefs.width << ", h" << prefs.height;
    LOG_MSG << "Loaded prefs" << prefs.toJson().dump();


    Webview w(
#ifdef _DEBUG
      true
#else
      false
#endif
      , nullptr);
    w.setAppIcon(WEB_ASSETS_BASE, "logo");
    w.set_title("PhenixCode Dashboard - v" EMBEDDER_VERSION " [build date: " __DATE__ "]");
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

    w.bind("setPersistentKey", [&prefs, changeTheme](const std::string &id, const std::string &data, void *)
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

    w.bind("getPersistentKey", [&prefs](const std::string &data) -> std::string
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

    w.bind("createProject", [](const std::string &) -> std::string
      {
        LOG_MSG << "createProject";
        nlohmann::json res;
        try {
          ensureProjectsFolderExist();
          const auto src = defaultSettingsJsonPath();
          if (!fs::exists(src)) {
            throw std::runtime_error("Default settings file not found at: " + src.string());
          }
          std::string fname = "settings_" + shared::generateRandomId(12) + ".json";
          auto ffname = [&fname]() {
            return projectsFolderPath() / fs::path(fname);
            };
          size_t n = 0;
          while (fs::exists(ffname())) {
            fname = "settings_" + shared::generateRandomId(12) + ".json";
            if (10 < ++n) {
              throw std::runtime_error("Failed to generate unique project settings filename");
            }
          }
          if (!fs::copy_file(src, ffname())) {
            throw std::runtime_error("Failed to copy default settings file to project folder");
          }
          Settings ss{ ffname().string() };
          ss.initProjectIdIfMissing(true);

          res["status"] = "success";
          res["settingsFilePath"] = (fs::absolute(ffname())).string();
          res["jsonData"] = ss.configJson();
        } catch (const std::exception &ex) {
          LOG_MSG << ex.what();
          res["status"] = "error";
          res["message"] = ex.what();
        }
        return res.dump();
      }
    );

    w.bind("deleteProject", [](const std::string &data) -> std::string
      {
        LOG_MSG << "deleteProject:" << data;
        nlohmann::json res;
        try {
          auto j = nlohmann::json::parse(data);
          if (!j.is_array() || j.size() == 0) {
            throw std::runtime_error("Invalid parameters");
          }
          j = j[0];
          if (validateProjectItemArg(j)) {
            std::string fname = j["settingsFilePath"].get<std::string>();
            if (fs::remove(fname)) {
              res["status"] = "success";
              res["message"] = "Project deleted successfully";
              LOG_MSG << "Deleted project settings file:" << fname;
            } else {
              throw std::runtime_error("Failed to delete project settings file" + fname);
            }
          }
        } catch (const std::exception &ex) {
          LOG_MSG << ex.what();
          res["status"] = "error";
          res["message"] = ex.what();
        }
        return res.dump();
      }
    );

    w.bind("importProject", [](const std::string &data) -> std::string
      {
        LOG_MSG << "importProject:" << data;
        nlohmann::json res;
        try {
          auto j = nlohmann::json::parse(data);
          if (!j.is_array() || j.size() < 2) {
            throw std::runtime_error("Invalid parameters for deleteProject");
          }
          std::string path = j[1];
          if (!fs::exists(path)) {
            throw std::runtime_error("Import settings file not found: " + path);
          }
          ensureProjectsFolderExist();
          nlohmann::json jRefs;
          jRefs["refs"] = nlohmann::json::array();
          if (!fs::exists(projectRefsPath())) {            
            std::ofstream file(projectRefsPath());
            file << jRefs.dump(2) << std::endl;
            LOG_MSG << "Created project refs file at:" << fs::absolute(projectRefsPath()).string();
          } else {
            std::ifstream file(projectRefsPath());
            file >> jRefs;
          }
          if (jRefs.contains("refs")) {
            for (const auto &t : jRefs["refs"]) {
              if (t.contains("path") && fs::path(std::string{t["path"]}) == fs::path(path)) {
                throw std::runtime_error("Already imported");
              }
            }
          }
          nlohmann::json ref;
          ref["path"] = path;
          jRefs["refs"].push_back(ref);
          std::ofstream file(projectRefsPath());
          file << jRefs.dump(2) << std::endl;
          res["status"] = "success";
        } catch (const std::exception &ex) {
          LOG_MSG << ex.what();
          res["status"] = "error";
          res["message"] = ex.what();
        }
        return res.dump();
      }
    );

    w.bind("getProjectList", [](const std::string &) -> std::string
      {
        LOG_MSG << "getProjectList";
        nlohmann::json res;
        try {
          std::vector<nlohmann::json> projects;
          auto addPath = [&projects](const std::string &path) {
            nlohmann::json j;
            std::ifstream file(path);
            file >> j;
            nlohmann::json proj;
            proj["settingsFilePath"] = path;
            proj["jsonData"] = j;
            projects.push_back(proj);
            };
          if (fs::exists(projectsFolderPath()) && fs::is_directory(projectsFolderPath())) {
            for (const auto &entry : fs::directory_iterator(projectsFolderPath())) {
              if (entry.is_regular_file() && entry.path().extension() == ".json" && entry.path().filename() != PROJECT_REFS_FNAME) {
                try {
                  addPath(fs::absolute(entry.path()).string());
                } catch (const std::exception &ex) {
                  LOG_MSG << "Error reading project settings from" << entry.path().string() << ":" << ex.what();
                }
              }
            }
            if (fs::exists(projectRefsPath())) {
              nlohmann::json jRefs;
              {
                std::ifstream file(projectRefsPath());
                file >> jRefs;
              }
              if (jRefs.contains("refs") && jRefs["refs"].is_array()) {
                for (const auto &a : jRefs["refs"]) {
                  if (a.contains("path")) {
                    std::string path = a["path"];
                    try {
                      addPath(path);
                    } catch (const std::exception &ex) {
                      LOG_MSG << "Error reading project settings from" << path << ":" << ex.what();
                    }
                  }
                }
              }
            }
          }
          res["status"] = "success";
          res["projects"] = projects;
        } catch (const std::exception &ex) {
          LOG_MSG << ex.what();
          res["status"] = "error";
          res["message"] = ex.what();
        }
        return res.dump();
      }
    );

    w.bind("saveProject", [](const std::string &data) -> std::string
      {
        LOG_MSG << "saveProject";
        nlohmann::json res;
        try {
          auto j = nlohmann::json::parse(data);
          if (!j.is_array() || j.size() == 0) {
            throw std::runtime_error("Invalid parameters for saveProject");
          }
          j = j[0];
          if (validateProjectItemArg(j)) {
            std::string fname = j["settingsFilePath"].get<std::string>();
            Settings ss{ fname };
            ss.updateFromConfig(j["jsonData"]);
            ss.initProjectIdIfMissing(false);
            ss.save();
            res["status"] = "success";
            res["message"] = "Project saved successfully";
            LOG_MSG << "Saved project settings to file:" << fname;
          } else {
            throw std::runtime_error("Unable to locate the file");
          }
        } catch (const std::exception &ex) {
          LOG_MSG << ex.what();
          res["status"] = "error";
          res["message"] = ex.what();
        }
        return res.dump();
      }
    );

    w.bind("getInstances", [](const std::string &) -> std::string
      {
        LOG_MSG << "getInstances";
        nlohmann::json res;
        try {
          InstanceRegistry registry;
          auto instances = registry.getActiveInstances();
          res["status"] = "success";
          res["instances"] = instances;
        } catch (const std::exception &ex) {
          LOG_MSG << ex.what();
          res["status"] = "error";
          res["message"] = ex.what();
        }
        return res.dump();
      }
    );
    w.bind("startServe", [](const std::string &data) -> std::string
      {
        LOG_MSG << "startServe";
        nlohmann::json res;
        try {
          auto j = nlohmann::json::parse(data);
          if (!j.is_array() || j.size() < 2) {
            throw std::runtime_error("Invalid parameters");
          }
          auto jProj = j[0];
          if (validateProjectItemArg(jProj)) {
            auto configPath = jProj["settingsFilePath"].get<std::string>();
            auto exePath = j[1].get<std::string>();
#ifdef _WIN32
            if (!exePath.empty() && !exePath.ends_with(".exe")) {
              exePath += ".exe";
            }
#endif
            if (!std::filesystem::exists(exePath))
              throw std::runtime_error("Executable not found: " + exePath);
            if (!std::filesystem::exists(configPath))
              throw std::runtime_error("Config file not found: " + configPath);

            std::vector<std::string> args = { "--no-startup-tests", "--config", configPath, "serve", "--yes" };
            bool watch = j[2];
            if (watch) {
              args.push_back("--watch");
              int interval = j[3];
              if (0 < interval) {
                args.push_back("--interval");
                args.push_back(std::to_string(interval));
              } else {
                LOG_MSG << "Invalid interval value, using to default value";
              }
            }
            ProcessManager proc;
            if (proc.startProcess(exePath, args)) {
              res["status"] = "success";
              res["message"] = "Embedder started successfully";
              LOG_MSG << "Started embedder process" << proc.getProcessId() << "for projectId";
              bool alive = testInstanceAlive(configPath, {});
              if (!alive) {
                LOG_MSG << "Warning: Started embedder process but instance not found in registry after timeout";
              }
            } else {
              throw std::runtime_error("Failed to start embedder process");
            }
            proc.detach();
          }
        } catch (const std::exception &ex) {
          LOG_MSG << ex.what();
          res["status"] = "error";
          res["message"] = ex.what();
        }
        return res.dump();
      }
    );

    w.bind("stopServe", [](const std::string &data) -> std::string
      {
        LOG_MSG << "stopServe:" << data;
        nlohmann::json res;
        try {
          auto j = nlohmann::json::parse(data);
          if (!j.is_array() || j.size() == 0) {
            throw std::runtime_error("Invalid parameters for stopServe");
          }
          std::string instanceId = j[0];
          if (instanceId.empty()) {
            throw std::runtime_error("Invalid instance id for stopServe");
          }
          InstanceRegistry registry;
          auto instances = registry.getActiveInstances();
          bool found = false;
          bool down = false;
          for (const auto &a : instances) {
            if (a["id"] == instanceId) {
              found = true;
              int port = a["port"];
              std::string host = a["host"];
              if (host == "localhost") host = "127.0.0.1";
              httplib::Client cli(host, port);
              httplib::Headers headers = { 
                //{"Authorization", "Basic: admin:admin"}
              };
              auto result = cli.Post("/api/shutdown", headers, "", "application/json");
              if (result && result->status == 200) {
                LOG_MSG << "Shutdown request sent to process for instance:" << instanceId;
                down = testInstanceDead({}, instanceId, 20);
              } else {
                LOG_MSG << "Failed to send shutdown request to process for instance:" << instanceId;
              }
              break;
            }
          }
          if (found) {
            if (down) {
              res["status"] = "success";
              res["message"] = "Serve stopped successfully";
            } else {
              res["status"] = "error";
              res["message"] = "Unable to stop the process";
            }
          } else {
            throw std::runtime_error("Instance id not found: " + instanceId);
          }
        } catch (const std::exception &ex) {
          LOG_MSG << ex.what();
          res["status"] = "error";
          res["message"] = ex.what();
        }
        return res.dump();
      }
    );

    w.bind("pickSettingsJsonFile", [](const std::string &) -> std::string
      {
        LOG_MSG << "pickSettingsJsonFile";
        nlohmann::json res;
        try {
          auto result = pfd::open_file("Pick a settings JSON file", {}, { "JSON files", "*.json", "All files", "*" }).result();
          if (!result.empty()) {
            auto path = result[0];
            if (fs::exists(path)) {
              Settings ss{ path };
              ss.initProjectIdIfMissing(false);
              res["project_id"] = ss.getProjectId();
              res["path"] = path;
            } else {
              throw std::runtime_error("Selected file does not exist: " + path);
            }
          }
        } catch (const std::exception &ex) {
          LOG_MSG << ex.what();
          res["status"] = "error";
          res["message"] = ex.what();
        }
        return res.dump();
      }
    );

    w.bind("checkPathExists", [](const std::string &data) -> std::string
      {
        LOG_MSG << "checkPathExists" << data;
        nlohmann::json res;
        try {
          auto j = nlohmann::json::parse(data);
          if (!j.is_array() || j.size() == 0) {
            throw std::runtime_error("Invalid parameters for checkPathExists");
          }
          std::string pathStr = j[0];
          fs::path p;
          if (pathStr.empty()) {
            // Treat empty path as current working directory
            p = fs::current_path();
            LOG_MSG << "checkPathExists: empty path => using current directory:" << fs::absolute(p).string();
          } else {
            p = fs::path(pathStr);
          }
          if (fs::exists(p)) {
            res["status"] = "success";
            res["path"] = fs::absolute(p).string();
          } else {
            throw std::runtime_error("Path does not exist: " + p.string());
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
        setPersistentKey,
        getPersistentKey,
        createProject,
        deleteProject,
        importProject,
        getProjectList,
        saveProject,
        getInstances,
        stopServe,
        startServe,
        pickSettingsJsonFile,
        checkPathExists,
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

  svr.stop();
  if (serverThread.joinable()) {
    serverThread.join();
    LOG_MSG << "HTTP server thread joined cleanly";
  }

  return 0;
}