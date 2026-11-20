#include "chat_tools.h"
#include "cutils.h"
#include <utils_log/logger.hpp>
#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <unordered_map>
#include <re2/re2.h>

using nlohmann::json;

namespace chat_tools {
  json executeSearchContext(const json &call, const ToolContext &ctx, const ContextSearch &search)
  {
    LOG_MSG_DEBUG << "executeSearchContext: " << call.dump();
    LOG_MSG_INFO << "[tool] search_context called";
    try {
      const auto &function = call.at("function");
      if (function.at("name") != "search_context") throw std::invalid_argument("Unknown tool");
      const auto arguments = function.at("arguments").get<std::string>();
      if (arguments.size() > 4096) throw std::invalid_argument("Tool arguments exceed the size limit");
      const auto args = json::parse(arguments);
      if (!args.is_object()) throw std::invalid_argument("Tool arguments must be an object");
      for (const auto &[key, value] : args.items())
        if (key != "query") throw std::invalid_argument("Unknown search_context argument: " + key);
      const auto query = args.at("query").get<std::string>();
      if (query.empty() || query.size() > 512 || query.find('\0') != std::string::npos ||
          query.find_first_not_of(" \t\r\n") == std::string::npos)
        throw std::invalid_argument("query must be nonblank text, 1 to 512 UTF-8 bytes");
      json result = {{"matches", json::array()}, {"mode", "none"}, {"truncated", false}};
      if (ctx.allowedFiles.empty()) return result;
      // The index may still contain removed/excluded files or URLs. Never expose
      // those results, even if the retriever ranks them above current sources.
      const auto normalizePath = [](std::string path) {
        std::replace(path.begin(), path.end(), '\\', '/');
        return std::filesystem::path(path).lexically_normal().generic_string();
      };
      std::unordered_map<std::string, std::string> allowed;
      for (const auto &file : ctx.allowedFiles) allowed.emplace(normalizePath(file), file);
      const auto retrieved = search(query);
      result["mode"] = retrieved.textFallback ? "text" : "hybrid";
      std::vector<std::string> returnedSources;
      for (const auto &hit : retrieved.matches) {
        const auto source = allowed.find(normalizePath(hit.sourceId));
        if (source == allowed.end()) continue;
        std::string content{ utils::sanitizeUtf8(hit.content) };
        const bool shortened = content.size() > 2048;
        if (shortened) {
          size_t end = 2048;
          while (end && (static_cast<unsigned char>(content[end]) & 0xc0) == 0x80) --end;
          content.resize(end);
        }
        json match = {{"source", source->second}, {"content", content}, {"truncated", shortened}};
        if (hit.chunkUnit == "line") {
          // Indexed line ranges are zero-based/end-exclusive; grep is already 1-based/inclusive.
          match["start_line"] = retrieved.textFallback ? hit.start : hit.start + 1;
          match["end_line"] = hit.end;
        } else if (hit.chunkUnit == "char") {
          match["start_byte"] = hit.start;
          match["end_byte"] = hit.end;
        }
        result["matches"].push_back(std::move(match));
        if (result["matches"].size() > 8 || result.dump().size() > kMaxResultBytes) {
          result["matches"].erase(result["matches"].end() - 1);
          result["truncated"] = true;
          break;
        }
        returnedSources.push_back(source->second);
      }
      for (const auto &source : returnedSources)
        if (std::find(ctx.usedSources.begin(), ctx.usedSources.end(), source) == ctx.usedSources.end()) ctx.usedSources.push_back(source);
      return result;
    } catch (const std::exception &e) {
      return {{"error", detail::preview(e.what(), 512)}};
    }
  }

  json executeGrep(const json &call, const ToolContext &ctx, const Search &search)
  {
    return executeGrepWithOptions(call, ctx, SearchWithOptions([&](const GrepQuery &query, const std::vector<std::string> &paths) {
      return search(query.pattern, paths);
    }));
  }

  json executeGrepWithOptions(const json &call, const ToolContext &ctx, const SearchWithOptions &search)
  {
    LOG_MSG_DEBUG << "executeGrep: " << call.dump();
    LOG_MSG_INFO << "[tool] grep called";
    try {
      const auto &function = call.at("function");
      if (function.at("name") != "grep") throw std::invalid_argument("Unknown tool");
      const auto arguments = function.at("arguments").get<std::string>();
      if (arguments.size() > 4096) throw std::invalid_argument("Tool arguments exceed the size limit");
      const auto args = json::parse(arguments);
      if (!args.is_object()) throw std::invalid_argument("Tool arguments must be an object");
      for (const auto &[key, value] : args.items())
        if (key != "pattern" && key != "path_filter" && key != "glob" && key != "regex" &&
            key != "case_sensitive" && key != "whole_word" && key != "context_lines")
          throw std::invalid_argument("Unknown grep argument: " + detail::preview(key, 128));
      const auto pattern = args.at("pattern").get<std::string>();
      if (pattern.size() < 2 || pattern.size() > kMaxGrepPatternBytes)
        throw std::invalid_argument("pattern must be single-line text, 2 to " +
          std::to_string(kMaxGrepPatternBytes) + " bytes (got " + std::to_string(pattern.size()) +
          (pattern.size() == 1 ? " byte)" : " bytes)"));
      if (pattern.find_first_not_of(" \t\r\n") == std::string::npos ||
          pattern.find('\0') != std::string::npos || pattern.find_first_of("\r\n") != std::string::npos)
        throw std::invalid_argument("pattern must be nonblank single-line text, 2 to " +
          std::to_string(kMaxGrepPatternBytes) + " bytes");
      const auto boolArg = [&](const char *key) -> bool {
        const auto it = args.find(key);
        if (it == args.end()) return false;
        if (!it->is_boolean()) throw std::invalid_argument(std::string(key) + " must be a boolean");
        return it->get<bool>();
      };
      const bool useRegex = boolArg("regex");
      const bool caseSensitive = boolArg("case_sensitive");
      const bool wholeWord = boolArg("whole_word");
      size_t contextLines = 2;
      if (args.contains("context_lines")) {
        const auto &val = args["context_lines"];
        if (!val.is_number_integer() || val < 0 || val > 10)
          throw std::invalid_argument("context_lines must be an integer between 0 and 10; use read_file or read_symbol for larger ranges");
        contextLines = val.get<size_t>();
      }
      if (useRegex) {
        re2::RE2::Options opt;
        opt.set_log_errors(false);
        opt.set_max_mem(4 << 20);
        opt.set_case_sensitive(caseSensitive);
        opt.set_one_line(true);
        re2::RE2 re(pattern, opt);
        if (!re.ok()) throw std::invalid_argument("Invalid regular expression: " + re.error());
        if (wholeWord) {
          re2::RE2 wordRe("(?:\\b(?:" + pattern + ")\\b)", opt);
          if (!wordRe.ok()) throw std::invalid_argument("Invalid whole-word regular expression: " + wordRe.error());
        }
      }
      const auto filter = detail::pathFilter(args);
      std::string globText;
      const auto globRe = detail::compileGlob(args, globText);
      const auto paths = detail::filterAllowedFiles(ctx.allowedFiles, filter, globRe);
      // Reserve the final outcome field before enforcing the serialized byte limit.
      json result = {{"matches", json::array()}, {"truncated", false}, {"path_filter", filter}, {"outcome", "incomplete_search"},
        {"matched_files", paths.size()}, {"searched_files", 0}, {"unreadable_files", 0}, {"search_limited", false}};
      if (!globText.empty()) result["glob"] = globText;
      // SourceProcessor interprets an empty list as all sources; never pass it through.
      if (paths.empty()) { result["outcome"] = "no_files_matched_filter"; return result; }
      const GrepQuery query{pattern, useRegex, caseSensitive, wholeWord, contextLines};
      const auto found = search(query, paths);
      result["searched_files"] = found.searchedFiles;
      result["unreadable_files"] = found.failedFiles;
      result["search_limited"] = found.limitReached;
      result["truncated"] = found.limitReached;
      for (const auto &hit : found.matches) {
        auto content = hit.content;
        bool shortened = content.size() > 2048;
        if (shortened) {
          result["truncated"] = true;
          size_t end = 2048;
          while (end && (static_cast<unsigned char>(content[end]) & 0xc0) == 0x80) --end;
          content.resize(end);
        }
        json match = {{"source", hit.sourceId}, {"start_line", hit.start}, {"end_line", hit.end},
          {"content", content}, {"truncated", shortened}};
        result["matches"].push_back(std::move(match));
        if (result.dump().size() > kMaxResultBytes || result["matches"].size() > 8) {
          result["matches"].erase(result["matches"].end() - 1);
          result["truncated"] = true;
          break;
        }
        if (std::find(ctx.usedSources.begin(), ctx.usedSources.end(), hit.sourceId) == ctx.usedSources.end()) ctx.usedSources.push_back(hit.sourceId);
      }
      result["outcome"] = !result["matches"].empty() ? "matches" :
        found.failedFiles || result["truncated"].get<bool>() ? "incomplete_search" : "no_matches";
      return result;
    } catch (const std::exception &e) {
      return {{"error", detail::preview(e.what(), 512)}};
    }
  }
} // namespace chat_tools
