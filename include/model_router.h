#pragma once
#include "json_shim.h"
#include <string>
#include <vector>

class App;
struct ApiConfig;

namespace model_router {
  ApiConfig getTargetApi(const nlohmann::json &, const App &);
  ApiConfig selectApi(const nlohmann::json &, const nlohmann::json &, const std::string &, const App &, std::vector<std::string> &);
}
