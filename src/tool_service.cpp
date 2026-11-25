#include "tool_service.h"
#include "app.h"
#include "symbol_service.h"
#include "settings.h"
#include "sourceproc.h"
#include "inference.h"
#include "monitor.h"
#include "tokenizer.h"
#include "cutils.h"
#include <utils_log/logger.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>

using json = nlohmann::json;

namespace chat_tools {

  json definitions(bool includeSettings, bool includeCodeGraph) {
    auto tools = json::array({ grepDefinition(), readFileDefinition(), readFilesDefinition(), symbolOutlineDefinition(), readSymbolDefinition(), findSymbolDefinition(), listFilesDefinition(), searchContextDefinition(), projectInfoDefinition() });
    if (includeSettings) tools.push_back(projectSettingsDefinition());
    for (auto tool : { gitDiffDefinition(), gitLogDefinition(), gitShowDefinition(), gitBlameDefinition() }) tools.push_back(std::move(tool));
    if (includeCodeGraph) {
      tools.push_back(dependenciesDefinition());
      tools.push_back(findReferencesDefinition());
    }
    return tools;
  }

  json Service::execute(const json &call, const json &tools, const ApiConfig *selectedApi,
    const std::function<bool()> &connected, const std::function<void(const std::string &)> &warning) {
    const auto name = call.value("function", json::object()).value("name", "");
    if (std::none_of(tools.begin(), tools.end(), [&](const json &t) { return t.at("function").at("name") == name; }))
      return { { "error", "Tool unavailable" } };
    json output;
    try {
      output = executeAvailable(call, name, tools, selectedApi, connected, warning);
    } catch (...) {
      app_.monitor().recordToolCall(name, caller_, true);
      throw;
    }
    app_.monitor().recordToolCall(name, caller_, output.contains("error"));
    return output;
  }

  json Service::executeAvailable(const json &call, const std::string &funcName, const json &tools,
    const ApiConfig *selectedApi, const std::function<bool()> &connected,
    const std::function<void(const std::string &)> &warning) {
    if (funcName == "project_settings")
      return selectedApi ? chat_tools::executeProjectSettings(call, app_.settings(), *selectedApi) : json{ { "error", "Tool unavailable" } };
    if (!collected_) {
      for (const auto &source : app_.sourceProcessor().collectSources(false))
        if (!source.isUrl) allowedFiles_.push_back(source.source);
      collected_ = true;
    }
    if (funcName == "project_info") {
      std::vector<std::string> availableTools;
      for (const auto &tool : tools) availableTools.push_back(tool.at("function").at("name").get<std::string>());
      return chat_tools::executeProjectInfo(call, app_.settings(), { allowedFiles_, usedSources_ }, app_.db().chunkCount(), availableTools);
    }
    if (funcName == "git_diff" || funcName == "git_log" || funcName == "git_show" || funcName == "git_blame")
      return chat_tools::executeGit(call, { allowedFiles_, usedSources_ }, &gitCache_);
    if (funcName == "read_file")
      return chat_tools::executeReadFile(call, { allowedFiles_, usedSources_ });
    if (funcName == "read_files")
      return chat_tools::executeReadFiles(call, { allowedFiles_, usedSources_ });
    if (funcName == "symbol_outline")
      return chat_tools::executeSymbolOutline(call, { allowedFiles_, usedSources_ }, &app_.symbolService());
    if (funcName == "read_symbol")
      return chat_tools::executeReadSymbol(call, { allowedFiles_, usedSources_ }, &app_.symbolService());
    if (funcName == "find_symbol")
      return chat_tools::executeFindSymbol(call, { allowedFiles_, usedSources_ }, &app_.symbolService());
    if (funcName == "dependencies") {
      auto reader = [&](const std::string &normalizedPath) -> chat_tools::IndexedDependencyQuery {
        chat_tools::IndexedDependencyQuery q;
        q.hasIndexedGraph = app_.db().hasAnyDependencies();
        if (!q.hasIndexedGraph) return q;
        q.sourceTracked = app_.db().isFileTracked(normalizedPath);
        if (q.sourceTracked) q.outgoing = app_.db().getOutgoingDependencies(normalizedPath);
        q.incoming = app_.db().getIncomingDependencies(normalizedPath);
        return q;
      };
      return chat_tools::executeDependencies(call, { allowedFiles_, usedSources_ }, &app_.symbolService(), reader);
    }
    if (funcName == "find_references")
      return chat_tools::executeFindReferences(call, { allowedFiles_, usedSources_ }, &app_.symbolService());
    if (funcName == "grep")
      return chat_tools::executeGrepWithOptions(call, { allowedFiles_, usedSources_ },
        [&](const chat_tools::GrepQuery &query, const std::vector<std::string> &paths) {
          SourceProcessor::GrepOptions opts;
          opts.contextLines = query.contextLines;
          opts.useRegex = query.useRegex;
          opts.caseSensitive = query.caseSensitive;
          opts.wholeWord = query.wholeWord;
          opts.maxMatchesPerFile = 10;
          opts.maxTotalResults = 9;
          opts.maxPatternBytes = chat_tools::kMaxGrepPatternBytes;
          SourceProcessor::GrepStats stats;
          auto matches = app_.sourceProcessor().grep({ query.pattern }, paths, opts, &stats);
          return chat_tools::GrepSearchResult{ std::move(matches), stats.searchedFiles, stats.failedFiles, stats.limitReached };
        });
    if (funcName == "list_files")
      return chat_tools::executeListFiles(call, { allowedFiles_, usedSources_ }, [&] { return app_.db().getTrackedFiles(); });
    if (funcName == "search_context")
      return chat_tools::executeSearchContext(call, { allowedFiles_, usedSources_ },
        [&](const std::string &query) -> chat_tools::ContextSearchResult {
          std::vector<float> embedding;
          if (app_.db().chunkCount() > 0) {
            const auto t0 = std::chrono::steady_clock::now();
            bool embeddingError = false;
            try {
              EmbeddingClient client(app_.settings().embeddingCurrentApi(), app_.settings().embeddingTimeoutMs());
              client.generateEmbeddings(query, embedding, EmbeddingClient::EncodeType::Query);
              if (embedding.size() != app_.settings().databaseVectorDim() ||
                !std::all_of(embedding.begin(), embedding.end(), [](float value) { return std::isfinite(value); }))
                throw std::runtime_error("Invalid embedding returned for search_context");
            } catch (const std::exception &e) {
              embeddingError = true;
              embedding.clear();
              LOG_MSG_WARN << "search_context: unable to generate query embedding:" << e.what();
              if (warning) warning("search_context: embedding server unavailable; falling back to text search");
            }
            const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            const auto queryTokens = app_.tokenizer().countTokensWithVocab(query);
            app_.monitor().recordRequest("embed", elapsed, embeddingError, queryTokens, 0);
            if (!embeddingError)
              app_.monitor().recordEmbeddingCost(app_.settings().embeddingCurrentApi().id, queryTokens, app_.settings().providers());
          }
          if (connected && !connected()) return {};
          // Overfetch a bounded candidate pool before the executor excludes
          // stale/out-of-scope index entries and caps the delivered excerpts.
          return { app_.retrieve(query, embedding, 32, SearchMode::Auto), embedding.empty() };
        });
    else {
      LOG_MSG_WARN << "Unknown tool function: " << funcName;
      return json{ { "error", "Unknown tool function: " + funcName } };
    }
  }

} // namespace chat_tools

