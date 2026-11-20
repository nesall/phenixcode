#pragma once
#include "chat_tools.h"
class App;
namespace chat_tools {
  nlohmann::json definitions(bool includeSettings = false, bool includeCodeGraph = true);
  // One instance per request; scope and Git discovery must never cross requests.
  class Service {
    App &app_;
    std::vector<std::string> allowedFiles_, usedSources_;
    bool collected_ = false;
    GitRepositoryCache gitCache_;
  public:
    explicit Service(App &app) : app_(app) {}
    const std::vector<std::string> &usedSources() const { return usedSources_; }
    nlohmann::json execute(const nlohmann::json &call, const nlohmann::json &tools,
      const ApiConfig *selectedApi = nullptr, const std::function<bool()> &connected = {},
      const std::function<void(const std::string &)> &warning = {});
  };
}
