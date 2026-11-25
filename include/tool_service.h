#pragma once
#include "chat_tools.h"
class App;
enum class MonitorToolCaller;
namespace chat_tools {
  nlohmann::json definitions(bool includeSettings = false, bool includeCodeGraph = true);
  // One instance per request; scope and Git discovery must never cross requests.
  class Service {
    App &app_;
    MonitorToolCaller caller_;
    std::vector<std::string> allowedFiles_, usedSources_;
    bool collected_ = false;
    GitRepositoryCache gitCache_;

    nlohmann::json executeAvailable(const nlohmann::json &call, const std::string &funcName,
      const nlohmann::json &tools, const ApiConfig *selectedApi, const std::function<bool()> &connected,
      const std::function<void(const std::string &)> &warning);
  public:
    Service(App &app, MonitorToolCaller caller) : app_(app), caller_(caller) {}
    const std::vector<std::string> &usedSources() const { return usedSources_; }
    nlohmann::json execute(const nlohmann::json &call, const nlohmann::json &tools,
      const ApiConfig *selectedApi = nullptr, const std::function<bool()> &connected = {},
      const std::function<void(const std::string &)> &warning = {});
  };
}
