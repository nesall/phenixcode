#pragma once

#include "json_shim.h"
#include "database.h"
#include "inference_messages.h"
#include <functional>
#include <filesystem>
#include <map>
#include <string_view>

struct ApiConfig;
class Settings;
namespace symbol_service { class SymbolService; }

namespace chat_tools {
  inline constexpr size_t kMaxAgentSteps = 15;
  inline constexpr size_t kMaxCallsPerStep = 8;
  inline constexpr size_t kMaxResultBytes = 8192;
  inline constexpr size_t kMaxPathFilterBytes = 1024;
  inline constexpr size_t kMaxGrepPatternBytes = 256;

  struct ToolContext {
    const std::vector<std::string> &allowedFiles;
    std::vector<std::string> &usedSources;
  };

  // Request-local discovery only. Never share across requests or source-scope changes.
  struct GitRepositoryCache {
    std::filesystem::path workingDirectory;
    std::map<std::string, std::filesystem::path> files;
    std::map<std::filesystem::path, std::filesystem::path> roots;
  };

  bool enabled(bool agentEnabled, const ApiConfig &api, bool freeChat, bool attachedOnly);
  nlohmann::json grepDefinition();
  nlohmann::json readFileDefinition();
  nlohmann::json readFilesDefinition();
  nlohmann::json symbolOutlineDefinition();
  nlohmann::json readSymbolDefinition();
  nlohmann::json findSymbolDefinition();
  nlohmann::json dependenciesDefinition();
  nlohmann::json findReferencesDefinition();
  nlohmann::json listFilesDefinition();
  nlohmann::json searchContextDefinition();
  nlohmann::json projectInfoDefinition();
  nlohmann::json projectSettingsDefinition();
  nlohmann::json gitDiffDefinition();
  nlohmann::json gitLogDefinition();
  nlohmann::json gitShowDefinition();
  nlohmann::json gitBlameDefinition();
  nlohmann::json executeGit(const nlohmann::json &call, const ToolContext &ctx, GitRepositoryCache *cache = nullptr);
  nlohmann::json executeProjectInfo(const nlohmann::json &call, const Settings &settings,
    const ToolContext &ctx, size_t indexedChunks, const std::vector<std::string> &availableTools);
  nlohmann::json executeProjectSettings(const nlohmann::json &call, const Settings &settings, const ApiConfig &selectedApi);
  struct GrepSearchResult {
    std::vector<SearchResult> matches;
    size_t searchedFiles = 0;
    size_t failedFiles = 0;
    bool limitReached = false;
  };
  struct GrepQuery {
    std::string pattern;
    bool useRegex = false;
    bool caseSensitive = false;
    bool wholeWord = false;
    size_t contextLines = 2;
  };
  using Search = std::function<GrepSearchResult(const std::string &, const std::vector<std::string> &)>;
  using SearchWithOptions = std::function<GrepSearchResult(const GrepQuery &, const std::vector<std::string> &)>;
  struct ContextSearchResult {
    std::vector<SearchResult> matches;
    bool textFallback = false;
  };
  using ContextSearch = std::function<ContextSearchResult(const std::string &)>;
  nlohmann::json executeGrep(const nlohmann::json &call, const ToolContext &ctx, const Search &search);
  nlohmann::json executeGrepWithOptions(const nlohmann::json &call, const ToolContext &ctx, const SearchWithOptions &search);
  nlohmann::json executeReadFile(const nlohmann::json &call, const ToolContext &ctx);
  nlohmann::json executeReadFiles(const nlohmann::json &call, const ToolContext &ctx);
  nlohmann::json executeSymbolOutline(const nlohmann::json &call, const ToolContext &ctx, symbol_service::SymbolService *symbols = nullptr);
  nlohmann::json executeReadSymbol(const nlohmann::json &call, const ToolContext &ctx, symbol_service::SymbolService *symbols = nullptr);
  nlohmann::json executeFindSymbol(const nlohmann::json &call, const ToolContext &ctx, symbol_service::SymbolService *symbols = nullptr);
  struct IndexedDependencyQuery {
    bool hasIndexedGraph = false;
    bool sourceTracked = false;
    std::vector<FileDependency> outgoing;
    std::vector<FileDependency> incoming;
  };
  using DependencyReader = std::function<IndexedDependencyQuery(const std::string &normalizedPath)>;
  nlohmann::json executeDependencies(const nlohmann::json &call, const ToolContext &ctx,
    symbol_service::SymbolService *symbols = nullptr, const DependencyReader &readIndexed = {});
  nlohmann::json executeFindReferences(const nlohmann::json &call, const ToolContext &ctx,
    symbol_service::SymbolService *symbols = nullptr);
  using FileMetadataReader = std::function<std::vector<FileMetadata>()>;
  nlohmann::json executeListFiles(const nlohmann::json &call, const ToolContext &ctx, const FileMetadataReader &metadata = {});
  nlohmann::json executeSearchContext(const nlohmann::json &call, const ToolContext &ctx, const ContextSearch &search);

  struct Turn {
    std::string text;
    nlohmann::json assistant;
    bool limitReached = false;
    bool incomplete = false;
  };
  using Generate = std::function<Turn(const nlohmann::json &)>;
  // Metadata uses the existing SSE envelope; call IDs correlate start/result updates.
  using Status = std::function<void(const nlohmann::json &)>;
  // Keep call/result pairs intact while shortening older results for a final answer.
  bool fitToolResults(nlohmann::json &messages, size_t budget, const inference_contant::TokenCounter &count);
  // All tool exchanges stay in this request. Only the final assistant is returned.
  Turn run(nlohmann::json messages, const Generate &generate,
    const std::function<nlohmann::json(const nlohmann::json &)> &execute,
    const std::function<bool()> &connected, size_t maxSteps = kMaxAgentSteps, const Status &onStatus = {},
    const std::string &displayRoot = {});
}
