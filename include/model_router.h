#pragma once
#include "json_shim.h"
#include <string>
#include <vector>

class App;
struct ApiConfig;

namespace model_router {
  ApiConfig getTargetApi(const nlohmann::json &, const App &app);
  ApiConfig getTargetApi(const std::string &targetApi, const App &app);
  ApiConfig selectApi(const nlohmann::json &, const nlohmann::json &, const std::string &, const App &, std::vector<std::string> &);
  ApiConfig selectApi(const std::string &targetApi, const std::string &sessionId, const nlohmann::json &, const std::string &, const App &, std::vector<std::string> &);
}
