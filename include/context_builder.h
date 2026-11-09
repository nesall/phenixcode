#pragma once
#include "json_shim.h"
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class App;
struct ApiConfig;
struct SearchResult;

namespace context {

  struct Attachment {
    std::string filename;
    std::string content;
  };

  std::vector<Attachment> parseAttachments(const nlohmann::json &);
  size_t messagesNofTokens(const nlohmann::json &, const App &);
  std::pair<std::vector<SearchResult>, size_t> processInputResults(
    const App &, const ApiConfig &, size_t, const std::string &,
    std::vector<Attachment>, std::vector<std::string>, float, bool,
    std::function<void(std::string_view)>, bool freeChat = false);

}
