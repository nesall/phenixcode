#pragma once

#include "json_shim.h"
#include "database.h"
#include "inference_messages.h"
#include <functional>
#include <string_view>

struct ApiConfig;
class Settings;

namespace chat_tools {
  inline constexpr size_t kMaxAgentSteps = 10;
  inline constexpr size_t kMaxCallsPerStep = 8;
  inline constexpr size_t kMaxResultBytes = 8192;
  inline constexpr size_t kMaxPathFilterBytes = 1024;

  struct ToolContext {
    const std::vector<std::string> &allowedFiles;
    std::vector<std::string> &usedSources;
  };

  bool enabled(bool agentEnabled, const ApiConfig &api, bool freeChat, bool attachedOnly);
  nlohmann::json grepDefinition();
  nlohmann::json readFileDefinition();
  nlohmann::json listFilesDefinition();
  nlohmann::json searchContextDefinition();
  nlohmann::json projectInfoDefinition();
  nlohmann::json projectSettingsDefinition();
  nlohmann::json executeProjectInfo(const nlohmann::json &call, const Settings &settings,
    const ToolContext &ctx, size_t indexedChunks, const std::vector<std::string> &availableTools);
  nlohmann::json executeProjectSettings(const nlohmann::json &call, const Settings &settings, const ApiConfig &selectedApi);
  using Search = std::function<std::vector<SearchResult>(const std::string &, const std::vector<std::string> &)>;
  struct ContextSearchResult {
    std::vector<SearchResult> matches;
    bool textFallback = false;
  };
  using ContextSearch = std::function<ContextSearchResult(const std::string &)>;
  nlohmann::json executeGrep(const nlohmann::json &call, const ToolContext &ctx, const Search &search);
  nlohmann::json executeReadFile(const nlohmann::json &call, const ToolContext &ctx);
  nlohmann::json executeListFiles(const nlohmann::json &call, const ToolContext &ctx);
  nlohmann::json executeSearchContext(const nlohmann::json &call, const ToolContext &ctx, const ContextSearch &search);

  struct Turn {
    std::string text;
    nlohmann::json assistant;
    bool limitReached = false;
    bool incomplete = false;
  };
  using Generate = std::function<Turn(const nlohmann::json &)>;
  using Status = std::function<void(std::string_view)>;
  // Keep call/result pairs intact while shortening older results for a final answer.
  bool fitToolResults(nlohmann::json &messages, size_t budget, const inference_contant::TokenCounter &count);
  // All tool exchanges stay in this request. Only the final assistant is returned.
  Turn run(nlohmann::json messages, const Generate &generate,
    const std::function<nlohmann::json(const nlohmann::json &)> &execute,
    const std::function<bool()> &connected, size_t maxSteps = kMaxAgentSteps, const Status &onStatus = {});
}
