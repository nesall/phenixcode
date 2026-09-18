#include "utils.h"
#include "cutils.h"
#include "procmngr.h"
#include <utils_log/logger.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <random>
#if defined(__linux__)
#include <gtk/gtk.h>
#endif

namespace fs = std::filesystem;

nlohmann::json shared::AppConfig::toJson() const
{
  nlohmann::json j;
  j["window"] = {
      {"width", width},
      {"height", height}
  };
  j["uiPrefs"] = nlohmann::json::array();
  for (const auto &item : uiPrefs) {
    j["uiPrefs"].push_back({
      {"key", item.first},
      {"value", item.second}
      });
  }
  return j;
}

std::string shared::getExecutableDir()
{
  LOG_START;
  return utils::getExecutableDir().string();
}

std::string shared::findConfigPath(const std::string &filename)
{
  const std::string exeDir = getExecutableDir();
  static std::vector<std::string> paths = {
    exeDir + "/" + filename,
    exeDir + "/../" + filename,
    exeDir + "/../../" + filename
  };
  std::string prefsPath;
  for (const auto &path : paths) {
    if (fs::exists(path)) {
      prefsPath = path;
      break;
    }
  }
  if (!fs::exists(prefsPath)) {
    prefsPath = paths[0];
  }
  return prefsPath;
}

void shared::savePrefsToFile(const AppConfig &prefs, const std::string &filename)
{
  const auto prefsPath = findConfigPath(filename);
  nlohmann::json j = prefs.toJson();
  std::ofstream out(prefsPath);
  if (out.is_open()) {
    out << j.dump(2) << std::endl;
    out.close();
    LOG_MSG << "Updated" << filename << "with new server URL";
  } else {
    LOG_MSG << "Failed to update" << prefsPath;
    throw std::runtime_error("Failed to update " + filename);
  }
}

void shared::fetchOrCreatePrefsJson(AppConfig &prefs, const std::string &filename, std::function<void(nlohmann::json j)> customHandler)
{
  LOG_START;
  const auto prefsPath = findConfigPath(filename);
  std::ifstream f(prefsPath);
  if (f.is_open()) {
    std::stringstream ss;
    ss << f.rdbuf();
    try {
      auto j = nlohmann::json::parse(ss.str());
      if (j.contains("window") && j["window"].is_object()) {
        const auto &w = j["window"];
        if (w.contains("width") && w["width"].is_number_integer()) {
          prefs.width = w["width"].get<int>();
        }
        if (w.contains("height") && w["height"].is_number_integer()) {
          prefs.height = w["height"].get<int>();
        }
      }
      if (customHandler) {
        customHandler(j);
      }
      if (j.contains("uiPrefs") && j["uiPrefs"].is_array()) {
        for (const auto &item : j["uiPrefs"]) {
          if (item.contains("key") && item.contains("value") &&
            item["key"].is_string() && item["value"].is_string()) {
            prefs.uiPrefs.insert({
              item["key"].get<std::string>(),
              item["value"].get<std::string>()
              });
          }
        }
      }
    } catch (const std::exception &e) {
      LOG_MSG << "Error parsing" << filename << ":" << e.what();
    }
  } else {
    nlohmann::json j = prefs.toJson();
    try {
      std::ofstream out(prefsPath);
      if (out.is_open()) {
        out << j.dump(2) << std::endl;
        out.close();
        LOG_MSG << "Created default" << filename <<"at:" << prefsPath;
      } else {
        LOG_MSG << "Failed to create" << filename << "at:" << prefsPath;
      }
    } catch (const std::exception &e) {
      LOG_MSG << "Error writing" << filename << ":" << e.what();
    }
  }
  prefs.width = (std::min)((std::max)(prefs.width, 200), 1400);
  prefs.height = (std::min)((std::max)(prefs.height, 300), 1000);
}

std::string shared::hashString(const std::string &str)
{
  std::hash<std::string> hasher;
  std::stringstream ss;
  ss << std::hex << std::setfill('0') << std::setw(16) << hasher(str);
  return ss.str();
}

std::string shared::getProjectId(const std::string &path)
{
  nlohmann::json j;
  std::ifstream file(path);
  if (!file.is_open()) {
    throw std::runtime_error("Cannot open settings file: " + path);
  }
  file >> j;
  std::string s;
  s = j["source"].value("project_id", "");
  if (s.empty()) {
    // Auto-generate
    auto absPath = std::filesystem::absolute(path).lexically_normal();
    std::string dirName = absPath.parent_path().filename().string();
    std::string pathHash = hashString(absPath.generic_string()).substr(0, 8);
    s = dirName + "-" + pathHash;
  }
  return s;
}

std::string shared::generateAppKey()
{
  // Random 32-character hex string
  std::mt19937 gen(std::random_device{}());
  std::uniform_int_distribution<> dis(0, 255);
  std::stringstream ss;
  for (int i = 0; i < 16; i++) {
    ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(dis(gen));
  }
  return ss.str();
}

std::string shared::generateRandomId(size_t nof)
{
  static constexpr char chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789_";
  thread_local std::mt19937 rng{ std::random_device{}() };
  std::uniform_int_distribution<size_t> dist(0, sizeof(chars) - 2); // excludes terminating '\0'
  std::string out;
  out.reserve(nof);
  for (size_t i = 0; i < nof; ++i)
    out.push_back(chars[dist(rng)]);
  return out;
}


ProcessManager *shared::ProcessesHolder::getOrCreateProcess(const std::string &appKey, const std::string &projectId)
{
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = embedderProcesses_.find(appKey);
  if (it != embedderProcesses_.end()) {
    return it->second.get();
  }
  auto procMgr = std::make_unique<ProcessManager>();
  ProcessManager *procPtr = procMgr.get();
  embedderProcesses_[appKey] = std::move(procMgr);
  projectIdToAppKey_[projectId] = appKey;
  appKeyToProjectId_[appKey] = projectId;
  return procPtr;
}

void shared::ProcessesHolder::discardProcess(const std::string &appKey)
{
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = embedderProcesses_.find(appKey);
  if (it != embedderProcesses_.end()) {
    embedderProcesses_.erase(it);
    auto projIt = appKeyToProjectId_.find(appKey);
    if (projIt != appKeyToProjectId_.end()) {
      projectIdToAppKey_.erase(projIt->second);
      appKeyToProjectId_.erase(projIt);
    }
  }
}

ProcessManager *shared::ProcessesHolder::getProcessWithApiKey(const std::string &appKey) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = embedderProcesses_.find(appKey);
  if (it != embedderProcesses_.end()) {
    return it->second.get();
  }
  return nullptr;
}

std::string shared::ProcessesHolder::getApiKeyFromProjectId(const std::string &projectId) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = projectIdToAppKey_.find(projectId);
  if (it != projectIdToAppKey_.end()) {
    return it->second;
  }
  return "";
}

void shared::ProcessesHolder::waitToStopThenTerminate()
{
  for (auto &proc : embedderProcesses_) {
    if (proc.second->waitForCompletion(10000)) {
      LOG_MSG << "Embedder process" << proc.second->getProcessId() << "exited cleanly";
    } else {
      LOG_MSG << "Embedder process" << proc.second->getProcessId() << "did not exit in time, terminating...";
      proc.second->stopProcess();
    }
  }
}
