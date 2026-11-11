// Admin Dashboard main application entry point

#include <httplib.h>
#include <utils_log/logger.hpp>
#include "wb.h"
#include "procmngr.h"
#include "cutils.h"
#include "utils.h"
#include "instregistry.h"
#include "settings.h"
#include "database_browser.h"
#include "3rdparty/portable-file-dialogs.h"
#include "json_shim.h"
#include <filesystem>
#include <string>
#include <deque>
#include <cstdint>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <fcntl.h>
#include <system_error>
#include <cerrno>
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
#include <shellapi.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#include "WinDarkTitlebarImpl.h"
#else
#include <limits.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif


namespace fs = std::filesystem;

namespace {

  const std::string CONFIG_FNAME = "admconfig.json";
  const std::string PROJECT_REFS_FNAME = "proj-refs.json";
  // Compile-time default set by CMake (option PROJECTS_FOLDER)
#ifndef PROJECTS_FOLDER_DEFAULT
#define PROJECTS_FOLDER_DEFAULT "phenixcode_projects"
#endif
#define _ProjectsFolderPathKey "ProjectsFolderPath"
  const std::string _projectsFolderName(const shared::AppConfig &p) {
    auto it = p.uiPrefs.find(_ProjectsFolderPathKey);
    if (it != p.uiPrefs.cend() && !it->second.empty()) {
      return it->second;
    }
    if (const char *env = std::getenv("PHENIXCODE_PROJECTS_FOLDER")) {
      if (*env) return std::string(env);
    }
    return PROJECTS_FOLDER_DEFAULT;
  }
  const fs::path projectsFolderPath(const shared::AppConfig &p) {
    return fs::path(shared::getExecutableDir()) / _projectsFolderName(p);
  }
  const fs::path projectRefsPath(const shared::AppConfig &p) {
    return (projectsFolderPath(p) / fs::path(PROJECT_REFS_FNAME));
  }
  const fs::path providersJsonPath(const shared::AppConfig &p) {
    return (projectsFolderPath(p) / fs::path("providers.json"));
  }
  void ensureProjectsFolderExist(const shared::AppConfig &p) {
    const auto projPath = projectsFolderPath(p);
    if (!fs::exists(projPath)) {
      if (fs::create_directories(projPath)) {
        LOG_MSG << "Created projects folder at:" << fs::absolute(projPath).string();
      } else {
        throw std::runtime_error("Failed to create projects folder at: " + fs::absolute(projPath).string());
      }
    }
  }

  void rememberDatabaseDirectory(shared::AppConfig &prefs, const fs::path &settingsPath,
    const std::string &configured, const fs::path &directory) {
    const auto key = "DatabaseLocation|" + database_browser::pathText(fs::weakly_canonical(settingsPath));
    const auto value = json{{"configured", configured}, {"base_directory", database_browser::pathText(directory)}}.dump();
    {
      std::lock_guard<std::mutex> lock(prefs.mutex_);
      const auto previous = prefs.uiPrefs.find(key);
      if (previous != prefs.uiPrefs.end() && previous->second == value) return;
      prefs.uiPrefs[key] = value;
    }
    shared::savePrefsToFile(prefs, CONFIG_FNAME);
  }

  struct DatabaseLocation {
    fs::path database;
    fs::path baseDirectory;
  };

  DatabaseLocation databaseLocation(const json &params, shared::AppConfig &prefs, bool chooseDirectory) {
    const auto settingsName = params.at("settingsFilePath").get<std::string>();
    if (settingsName.empty()) throw std::runtime_error("settingsFilePath is required");
    const auto settingsPath = fs::weakly_canonical(fs::u8path(settingsName));
    std::ifstream input(settingsPath);
    const auto settings = json::parse(input);
    const auto configured = settings.at("database").value("sqlite_path", std::string("db.sqlite"));
    if (configured.empty()) throw std::runtime_error("The project has no database filename configured");
    DatabaseLocation location{fs::u8path(configured), {}};
    InstanceRegistry registry;
    for (const auto &instance : registry.getActiveInstances()) {
      const auto config = instance.value("config", std::string{});
      std::error_code pathError;
      if (!config.empty() && fs::equivalent(fs::u8path(config), settingsPath, pathError)) {
        location.baseDirectory = fs::u8path(instance.at("cwd").get<std::string>());
        const auto parameters = instance.value("params", json::object());
        const auto effectivePath = parameters.value("database_path", std::string{});
        if (!effectivePath.empty()) location.database = fs::u8path(effectivePath);
        break;
      }
    }
    if (chooseDirectory) {
      if (!location.baseDirectory.empty())
        throw database_browser::Error("running", "The running instance supplies its working directory automatically.");
      const auto chosen = pfd::select_folder("Choose the working directory used to index this project", settingsPath.parent_path().string()).result();
      if (chosen.empty()) throw database_browser::Error("cancelled", "Directory selection cancelled.");
      location.baseDirectory = fs::u8path(chosen);
    }
    if (location.baseDirectory.empty()) {
      std::string cached;
      {
        std::lock_guard<std::mutex> lock(prefs.mutex_);
        const auto found = prefs.uiPrefs.find("DatabaseLocation|" + database_browser::pathText(settingsPath));
        if (found != prefs.uiPrefs.end()) cached = found->second;
      }
      if (!cached.empty()) {
        const auto saved = json::parse(cached, nullptr, false);
        if (saved.is_object() && saved.value("configured", std::string{}) == configured)
          location.baseDirectory = fs::u8path(saved.value("base_directory", std::string{}));
      }
    }
    if (!location.baseDirectory.empty()) {
      if (!location.baseDirectory.is_absolute()) throw std::runtime_error("The saved working directory must be absolute");
      rememberDatabaseDirectory(prefs, settingsPath, configured, location.baseDirectory);
    }
    if (location.database.is_relative()) {
      if (location.baseDirectory.empty())
        throw database_browser::Error("location_required", "Choose the working directory used when this project was indexed.");
      location.database = location.baseDirectory / location.database;
    }
    location.database = location.database.lexically_normal();
    return location;
  }

  void openSourceDirectory(const fs::path &source) {
    const auto directory = source.parent_path();
    if (!fs::is_directory(directory)) throw std::runtime_error("The source directory no longer exists");
#ifdef _WIN32
    const auto result = ShellExecuteW(nullptr, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) throw std::runtime_error("Unable to open the source directory");
#else
#ifdef __APPLE__
    const char *command = "open";
#else
    const char *command = "xdg-open";
#endif
    std::string path = directory.string();
    char *arguments[] = {const_cast<char *>(command), path.data(), nullptr};
    pid_t child = 0;
    const int error = posix_spawnp(&child, command, nullptr, nullptr, arguments, environ);
    if (error) throw std::system_error(error, std::generic_category(), "Unable to open the source directory");
    std::thread([child] { int status; while (waitpid(child, &status, 0) < 0 && errno == EINTR) {} }).detach();
#endif
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
          LOG_MSG_ERROR << ex.what();
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
          LOG_MSG_ERROR << ex.what();
        }
        return "null";
      }
    );

    w.bind("getEnv", [&prefs](const std::string &data) -> std::string
      {
        LOG_MSG << "getEnv:" << data;
        try {
          auto j = nlohmann::json::parse(data);
          if (j.is_array() && 0 < j.size()) {
            std::string key = j[0].get<std::string>();
            const char *value = std::getenv(key.c_str());
            if (value != nullptr) {
              return nlohmann::json(value).dump();
            }
          }
        } catch (const std::exception &ex) {
          LOG_MSG_ERROR << ex.what();
        }
        return "null";
      }
    );

    w.bind("setProjectsFolderPath", [&prefs](const std::string &data) -> std::string
      {
        LOG_MSG << "setProjectsFolderPath:" << data;
        try {
          auto j = nlohmann::json::parse(data);
          if (j.is_array() && 0 < j.size()) {
            std::string path = j[0].get<std::string>();
            // TODO:
          }
        } catch (const std::exception &ex) {
          LOG_MSG << ex.what();
        }
        return "null";
      }
    );

    w.bind("reconcileProject", [](const std::string &data) -> std::string
      {
        try {
          const auto args = nlohmann::json::parse(data).at(0);
          ProvidersSettings providers;
          providers.updateFromConfig(args.at("providers"));
          Settings::ProviderRenames renames;
          const auto mapping = args.value("renames", nlohmann::json::object());
          renames.embedding = mapping.value("embedding", std::map<std::string, std::string>{});
          renames.generation = mapping.value("generation", std::map<std::string, std::string>{});
          const auto result = Settings::reconcileProject(args.at("project").at("jsonData"), providers, renames);
          return nlohmann::json{
            {"status", "success"}, {"jsonData", result.jsonData}, {"changed", result.changed},
            {"needsReindex", result.needsReindex}, {"dropped", result.dropped}, {"needsSelection", result.needsSelection}
          }.dump();
        } catch (const std::exception &ex) {
          return nlohmann::json{{"status", "error"}, {"message", ex.what()}}.dump();
        }
      }
    );

    w.bind("createProject", [&prefs](const std::string &data) -> std::string
      {
        LOG_MSG << "createProject";
        nlohmann::json res;
        fs::path createdPath;
        try {
          ensureProjectsFolderExist(prefs);
          const auto prv = providersJsonPath(prefs);
          if (!fs::exists(prv)) {
            throw std::runtime_error("Providers file not found at: " + prv.string());
          }
          ProvidersSettings catalog{ prv.string() };
          Settings checked = Settings::createBlank(catalog);
          const auto args = nlohmann::json::parse(data);
          const auto requested = args.empty() ? std::string{} : args.at(0).value("filename", std::string{});
          std::string fname = requested.empty() ? "settings_" + shared::generateRandomId(12) + ".json" : requested;
          if (fname.find_first_of("<>:\"/\\|?*") != std::string::npos ||
              std::any_of(fname.begin(), fname.end(), [](unsigned char c) { return c < 32; }) ||
              fname.front() == ' ' || fname.back() == ' ' || fname.back() == '.' || fname.size() > 200)
            throw std::runtime_error("Enter a valid settings filename without a folder path (maximum 200 characters)");
          std::string lower = fname;
          std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
          const auto stem = lower.substr(0, lower.find('.'));
          if (stem.empty() || stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
              (stem.size() == 4 && (stem.substr(0, 3) == "com" || stem.substr(0, 3) == "lpt") && stem[3] >= '0' && stem[3] <= '9'))
            throw std::runtime_error("This filename is reserved. Choose another name");
          if (lower.size() < 5 || lower.substr(lower.size() - 5) != ".json") fname += ".json";
          else fname.replace(fname.size() - 5, 5, ".json");
          if (fname.size() > 200) throw std::runtime_error("Settings filename must be at most 200 characters including .json");
          const auto target = projectsFolderPath(prefs) / fs::u8path(fname);
#ifdef _WIN32
          int descriptor = -1;
          const auto createError = _wsopen_s(&descriptor, target.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
            _SH_DENYRW, _S_IREAD | _S_IWRITE);
          if (createError == EEXIST) throw std::runtime_error("A settings file with this name already exists. Choose another name");
          if (createError != 0) throw std::system_error(createError, std::generic_category(), "Cannot create settings file");
          _close(descriptor);
#else
          const int descriptor = ::open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
          if (descriptor < 0 && errno == EEXIST) throw std::runtime_error("A settings file with this name already exists. Choose another name");
          if (descriptor < 0) throw std::system_error(errno, std::generic_category(), "Cannot create settings file");
          ::close(descriptor);
#endif
          createdPath = target;
          checked.saveToPath(createdPath.string());
          Settings ss{ createdPath.string(), prv.string() };
          ss.initProjectIdIfMissing(true);

          res["status"] = "success";
          res["settingsFilePath"] = (fs::absolute(createdPath)).string();
          res["jsonData"] = ss.configJson();
        } catch (const std::exception &ex) {
          if (!createdPath.empty()) {
            std::error_code cleanupError;
            fs::remove(createdPath, cleanupError);
          }
          LOG_MSG_ERROR << ex.what();
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
          LOG_MSG_ERROR << ex.what();
          res["status"] = "error";
          res["message"] = ex.what();
        }
        return res.dump();
      }
    );

    w.bind("importProject", [&prefs](const std::string &data) -> std::string
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
          ensureProjectsFolderExist(prefs);
          const auto projRefsPath = projectRefsPath(prefs);
          nlohmann::json jRefs;
          jRefs["refs"] = nlohmann::json::array();
          if (!fs::exists(projRefsPath)) {
            std::ofstream file(projRefsPath);
            file << jRefs.dump(2) << std::endl;
            LOG_MSG << "Created project refs file at:" << fs::absolute(projRefsPath).string();
          } else {
            std::ifstream file(projRefsPath);
            file >> jRefs;
          }
          if (jRefs.contains("refs")) {
            for (const auto &t : jRefs["refs"]) {
              if (t.contains("path") && fs::path(std::string{ t["path"] }) == fs::path(path)) {
                throw std::runtime_error("Already imported");
              }
            }
          }
          nlohmann::json ref;
          ref["path"] = path;
          jRefs["refs"].push_back(ref);
          std::ofstream file(projRefsPath);
          file << jRefs.dump(2) << std::endl;
          res["status"] = "success";
        } catch (const std::exception &ex) {
          LOG_MSG_ERROR << ex.what();
          res["status"] = "error";
          res["message"] = ex.what();
        }
        return res.dump();
      }
    );

    w.bind("getProjectList", [&prefs](const std::string &) -> std::string
      {
        LOG_MSG << "getProjectList";
        nlohmann::json res;
        try {
          std::set<fs::path> pathSet;
          std::vector<nlohmann::json> projects;
          auto addPath = [&projects, &pathSet](const std::string &path) {
            try {
              if (pathSet.contains(fs::path(path))) return;
              pathSet.insert(fs::path{ path });
              nlohmann::json j;
              std::ifstream file(path);
              file >> j;
              Settings::validateProjectJson(j);
              nlohmann::json proj;
              proj["settingsFilePath"] = path;
              proj["jsonData"] = j;
              projects.push_back(proj);
            } catch (...) {
              LOG_MSG << "Path not a project settings. Skipped. [" << path << "]";
            }
            };
          auto ppath = projectsFolderPath(prefs);
          if (fs::exists(ppath) && fs::is_directory(ppath)) {
            for (const auto &entry : fs::directory_iterator(ppath)) {
              if (entry.is_regular_file() && entry.path().extension() == ".json" 
                && entry.path().filename() != PROJECT_REFS_FNAME
                && entry.path().filename() != "providers.json"
                )
              {
                try {
                  addPath(fs::absolute(entry.path()).string());
                } catch (const std::exception &ex) {
                  LOG_MSG << "Error reading project settings from" << entry.path().string() << ":" << ex.what();
                }
              }
            }
            if (fs::exists(projectRefsPath(prefs))) {
              nlohmann::json jRefs;
              {
                std::ifstream file(projectRefsPath(prefs));
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
          LOG_MSG_ERROR << ex.what();
          res["status"] = "error";
          res["message"] = ex.what();
        }
        return res.dump();
      }
    );

    w.bind("saveProject", [&prefs](const std::string &data) -> std::string
      {
        LOG_MSG << "saveProject";
        nlohmann::json res;
        try {
          const auto prv = providersJsonPath(prefs);
          if (!fs::exists(prv)) {
            throw std::runtime_error("Providers file not found at: " + prv.string());
          }
          auto j = nlohmann::json::parse(data);
          if (!j.is_array() || j.size() == 0) {
            throw std::runtime_error("Invalid parameters for saveProject");
          }
          j = j[0];
          if (validateProjectItemArg(j)) {
            std::string fname = j["settingsFilePath"].get<std::string>();
            Settings ss{ j["jsonData"], prv.string() };
            ss.initProjectIdIfMissing(false);
            ss.saveToPath(fname);
            res["status"] = "success";
            res["message"] = "Project saved successfully";
            LOG_MSG << "Saved project settings to file:" << fname;
          } else {
            throw std::runtime_error("Unable to locate the file");
          }
        } catch (const std::exception &ex) {
          LOG_MSG_ERROR << ex.what();
          res["status"] = "error";
          res["message"] = ex.what();
        }
        return res.dump();
      }
    );

    w.bind("saveProviders", [&prefs](const std::string &data) -> std::string
      {
        LOG_MSG << "saveProviders";
        nlohmann::json res;
        try {
          const auto prv = providersJsonPath(prefs);
          if (!fs::exists(prv)) {
            throw std::runtime_error("Providers file not found at: " + prv.string());
          }
          auto j = nlohmann::json::parse(data);
          if (!j.is_array() || j.size() == 0) {
            throw std::runtime_error("Invalid parameters for saveProviders");
          }
          j = j[0];
          ProvidersSettings ss{ prv.string() };
          ss.updateFromConfig(j);
          ss.save();
          res["status"] = "success";
          res["message"] = "Providers saved successfully";
          LOG_MSG << "Saved providers to file:" << prv.string();
        } catch (const std::exception &ex) {
          LOG_MSG_ERROR << ex.what();
          res["status"] = "error";
          res["message"] = ex.what();
        }
        return res.dump();
      }
    );

    w.bind("getProviders", [&prefs](const std::string &) -> std::string
      {
        LOG_MSG << "getProviders";
        nlohmann::json res;
        try {
          const auto prv = providersJsonPath(prefs);
          if (!fs::exists(prv)) {
            throw std::runtime_error("Providers file not found at: " + prv.string());
          }
          ProvidersSettings ss{ prv.string() };
          res["status"] = "success";
          res["providers"] = ss.configJson();
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
    w.bind("startServe", [&prefs](const std::string &data) -> std::string
      {
        LOG_MSG << "startServe";
        nlohmann::json res;
        try {
          auto j = nlohmann::json::parse(data);
          if (!j.is_array() || j.size() < 2) {
            throw std::runtime_error("Invalid parameters");
          }
          if (validateProjectItemArg(j[0])) {
            auto configPath = j[0]["settingsFilePath"].get<std::string>();
            auto exePath = j[1].get<std::string>();
            auto providersPath = providersJsonPath(prefs).lexically_normal().generic_string();
#ifdef _WIN32
            if (!exePath.empty() && !exePath.ends_with(".exe")) {
              exePath += ".exe";
            }
#endif
            if (!std::filesystem::exists(providersPath))
              throw std::runtime_error("Providers file not found: " + providersPath);
            if (!std::filesystem::exists(exePath))
              throw std::runtime_error("Executable not found: " + exePath);
            if (!std::filesystem::exists(configPath))
              throw std::runtime_error("Config file not found: " + configPath);

            std::vector<std::string> args = { "--no-startup-tests", "-w", providersPath, "--config", configPath, "serve", "--yes" };
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
              try {
                std::ifstream projectFile(fs::u8path(configPath));
                const auto projectJson = json::parse(projectFile);
                rememberDatabaseDirectory(prefs, fs::u8path(configPath),
                  projectJson.at("database").value("sqlite_path", std::string("db.sqlite")), fs::current_path());
              } catch (const std::exception &error) {
                LOG_MSG_ERROR << "Unable to remember the indexing working directory:" << error.what();
              }
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
          LOG_MSG_ERROR << ex.what();
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
          LOG_MSG_ERROR << ex.what();
          res["status"] = "error";
          res["message"] = ex.what();
        }
        return res.dump();
      }
    );

    w.bind("pickSettingsJsonFile", [&prefs](const std::string &) -> std::string
      {
        LOG_MSG << "pickSettingsJsonFile";
        nlohmann::json res;
        try {
          const auto prv = providersJsonPath(prefs);
          if (!fs::exists(prv)) {
            throw std::runtime_error("Providers file not found at: " + prv.string());
          }

          auto result = pfd::open_file("Pick a settings JSON file", {}, { "JSON files", "*.json", "All files", "*" }).result();
          if (!result.empty()) {
            auto path = result[0];
            if (fs::exists(path)) {
              Settings ss{ path, prv.string() };
              ss.initProjectIdIfMissing(false);
              res["project_id"] = ss.getProjectId();
              res["path"] = path;
            } else {
              throw std::runtime_error("Selected file does not exist: " + path);
            }
          }
        } catch (const std::exception &ex) {
          LOG_MSG_ERROR << ex.what();
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
          LOG_MSG_ERROR << ex.what();
          res["status"] = "error";
          res["message"] = ex.what();
        }
        return res.dump();
      }
    );

    for (const auto *operation : {"listDatabaseSources", "getDatabaseSourceChunks", "readDatabaseSourceFile",
         "openDatabaseSourceDirectory", "selectDatabaseWorkingDirectory"}) {
      w.bind(operation, [&prefs, operation = std::string(operation)](const std::string &data) -> std::string {
        try {
          const auto params = json::parse(data).at(0);
          const auto location = databaseLocation(params, prefs, operation == "selectDatabaseWorkingDirectory");
          database_browser::Reader reader(location.database, location.baseDirectory);
          json response;
          if (operation == "listDatabaseSources" || operation == "selectDatabaseWorkingDirectory") {
            response = reader.list(params.value("search", std::string{}), params.value("offset", 0), params.value("limit", 50));
          } else {
            const auto source = params.at("source_id").get<std::string>();
            if (operation == "getDatabaseSourceChunks") response = reader.chunks(source, params.value("offset", 0), params.value("limit", 5));
            else if (operation == "readDatabaseSourceFile") response = reader.readFile(source);
            else openSourceDirectory(reader.sourcePath(source));
          }
          if (response.is_null()) response = json::object();
          response["status"] = "success";
          return response.dump(-1, ' ', false, json::error_handler_t::replace);
        } catch (const database_browser::Error &error) {
          return json{{"status", "error"}, {"code", error.code}, {"message", error.what()}}.dump();
        } catch (const std::exception &error) {
          return json{{"status", "error"}, {"code", "error"}, {"message", error.what()}}.dump();
        }
      });
    }

    w.bind("getMonitorSnapshot", [&w](const std::string &data) -> std::string
      {
        LOG_MSG << "getMonitorSnapshot" << data;
        nlohmann::json res;
        try {
          auto j = nlohmann::json::parse(data);
          if (!j.is_array() || j.size() == 0) {
            throw std::runtime_error("Invalid parameters for getMonitorSnapshot");
          }
          const auto params = j[0];
          const std::string settingsPath = params.value("settingsFilePath", std::string{});
          if (settingsPath.empty()) {
            res = { { "status", "error" },{ "message", "settingsFilePath is required" } };
            return res.dump();
          }

          const auto projectDir = std::filesystem::path(settingsPath).parent_path();
          const auto monitorPath = utils::monitorPathFor(projectDir);

          if (!std::filesystem::exists(monitorPath)) {
            // No snapshot on disk yet — return a minimal idle snapshot
            res = {
              { "status", "success" },
              { "monitor", json::object({
                { "project_id", "" },
                { "session_id", "offline" },
                { "seq", 0 },
                { "ts", 0 },
                { "state", "idle" },
                { "last_heartbeat", 0 },
                { "progress", json::object() },
                { "metrics",  json::object() },
                { "cost",     json::object() },
                { "offline", true }
              }) }
            };
            return res.dump();
          }

          std::ifstream file(monitorPath);
          json snap = json::parse(file);
          snap["offline"] = true;
          res = { { "status", "success" },{ "monitor", std::move(snap) } };
        } catch (const std::exception &ex) {
          LOG_MSG_ERROR << ex.what();
          res = { {"status", "error"}, {"message", ex.what()} };
        }
        return res.dump();
      }
    );

    w.bind("getLogTail", [&w](const std::string &data) -> std::string
      {
        json res;
        try {
          auto j = json::parse(data);
          if (!j.is_array() || j.size() == 0) {
            throw std::runtime_error("Invalid parameters for getLogTail");
          }
          const auto params = j[0];
          const std::string settingsPath = params.value("settingsFilePath", std::string{});
          const std::string file = params.value("file", std::string{});
          int64_t offset = params.value("offset", static_cast<int64_t>(0));
          int64_t count = params.value("count", static_cast<int64_t>(500));
          count = (std::clamp)(count, static_cast<int64_t>(1), static_cast<int64_t>(5000));
          offset = (std::max)(offset, static_cast<int64_t>(-100000)); // bound tail buffer

          auto fail = [&](const std::string &msg) {
            return json{ { "status", "error" },{ "message", msg },
                         { "lines", json::array() },{ "total_lines", 0 } }.dump();
            };
          if (settingsPath.empty()) return fail("settingsFilePath is required");
          if (file != "main" && file != "diagnostics") return fail("file must be 'main' or 'diagnostics'");

          // Resolve log file path from settings JSON
          std::ifstream sfile(settingsPath);
          json settings = json::parse(sfile);
          const auto &logging = settings.value("logging", json::object());
          std::string logPath = file == "diagnostics"
            ? logging.value("diagnostics_file", std::string{})
            : logging.value("logging_file", std::string{});

          if (!logPath.empty() && !fs::path(logPath).is_absolute()) {
            logPath = (fs::path(settingsPath).parent_path() / logPath).lexically_normal().string();
          }

          json lines = json::array();
          int64_t total = 0;

          if (!logPath.empty() && fs::exists(logPath)) {
            std::ifstream logfile(logPath, std::ios::binary);
            const bool fromTail = offset < 0;
            const size_t tailN = fromTail ? static_cast<size_t>(-offset) : 0;
            std::deque<std::string> tail;

            std::string line;
            while (std::getline(logfile, line)) {
              if (logfile.eof()) break; // unterminated last line: still being written, don't count
              if (!line.empty() && line.back() == '\r') line.pop_back();

              if (fromTail) {
                tail.push_back(std::move(line));
                if (tail.size() > tailN) tail.pop_front();
              } else if (total >= offset && static_cast<int64_t>(lines.size()) < count) {
                lines.push_back(std::move(line));
              }
              ++total;
            }

            if (fromTail) {
              const size_t n = (std::min)(tail.size(), static_cast<size_t>(count));
              for (size_t i = 0; i < n; ++i) lines.push_back(std::move(tail[i]));
            }
          }

          res = {
            { "status", "success" },
            { "lines", std::move(lines) },
            { "total_lines", total }
          };
        } catch (const std::exception &ex) {
          LOG_MSG_ERROR << ex.what();
          res = { { "status", "error" },{ "message", ex.what() },
                  { "lines", json::array() },{ "total_lines", 0 } };
        }
        return res.dump();
      }
    );

    w.bind("testProvider", [&w](const std::string &data) -> std::string
      {
        json res;
        try {
          auto j = json::parse(data);
          if (!j.is_array() || j.size() == 0) {
            throw std::runtime_error("Invalid parameters for testProvider");
          }
          const auto &params = j[0];
          if (!params.is_object()) {
            throw std::runtime_error("Invalid parameters for testProvider");
          }

          json request = {
            { "id", params.value("id", std::string{}) },
            { "url", params.value("url", std::string{}) },
            { "apiKey", params.value("apiKey", std::string{}) },
            { "model", params.value("model", std::string{}) }
          };

          InstanceRegistry registry;
          const auto instances = registry.getActiveInstances();
          if (!instances.empty()) {
            const auto &instance = instances.front();
            std::string host = instance.value("host", std::string("127.0.0.1"));
            if (host == "localhost") host = "127.0.0.1";
            const int port = instance.at("port").get<int>();
            httplib::Client cli(host, port);
            auto result = cli.Post("/api/providers/test", request.dump(), "application/json");
            if (!result) {
              throw std::runtime_error("Unable to reach provider test endpoint");
            }
            res = json::parse(result->body);
            if (result->status < 200 || result->status >= 300) {
              res["status"] = "error";
            }
          } else {
            throw std::runtime_error("No active instances found to test provider");
          }
        } catch (const std::exception &ex) {
          LOG_MSG_ERROR << ex.what();
          res = { { "status", "error" },{ "message", ex.what() } };
        }
        return res.dump();
      }
    );


    w.init(R"(
      window.cppApi = {
        setPersistentKey,
        getPersistentKey,
        getEnv,
        setProjectsFolderPath,
        createProject,
        reconcileProject,
        deleteProject,
        importProject,
        getProjectList,
        saveProject,
        saveProviders,
        getProviders,
        getInstances,
        stopServe,
        startServe,
        pickSettingsJsonFile,
        checkPathExists,
        listDatabaseSources,
        getDatabaseSourceChunks,
        readDatabaseSourceFile,
        openDatabaseSourceDirectory,
        selectDatabaseWorkingDirectory,
        getMonitorSnapshot,
        getLogTail,
        testProvider
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
