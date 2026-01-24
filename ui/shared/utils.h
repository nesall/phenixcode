#include <mutex>
#include <memory>
#include <functional>
#include <unordered_map>
#include "json_shim.h"

class ProcessManager;

namespace shared {

  struct AppConfig {
    int width = 700;
    int height = 900;
    std::unordered_map<std::string, std::string> uiPrefs;
    mutable std::mutex mutex_;
    virtual nlohmann::json toJson() const;
  };
  
  std::string getExecutableDir();

  std::string findConfigPath(const std::string &filename);

  void savePrefsToFile(const AppConfig &prefs, const std::string &filename);

  void fetchOrCreatePrefsJson(AppConfig &prefs, const std::string &filename, std::function<void(nlohmann::json j)> customHandler = nullptr);

  std::string hashString(const std::string &str);

  std::string getProjectId(const std::string &path);

  std::string generateAppKey();

  std::string generateRandomId(size_t nof);

  struct ProcessesHolder {
    ProcessManager *getOrCreateProcess(const std::string &appKey, const std::string &projectId);
    void discardProcess(const std::string &appKey);
    ProcessManager *getProcessWithApiKey(const std::string &appKey) const;
    std::string getApiKeyFromProjectId(const std::string &projectId) const;
    void waitToStopThenTerminate();

  private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<ProcessManager>> embedderProcesses_;
    std::unordered_map<std::string, std::string> projectIdToAppKey_; // we assume 1 to 1 relationship
    std::unordered_map<std::string, std::string> appKeyToProjectId_;
  };

} // namespace shared