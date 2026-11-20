#include "chat_tools.h"
#include "symbol_service.h"
#include "cutils.h"
#include <utils_log/logger.hpp>
#include <algorithm>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <re2/re2.h>

using nlohmann::json;

namespace chat_tools {
  namespace {
    bool matchesFindQuery(const symbol_service::SymbolInfo &sym, const std::string &query, bool prefix) {
      const bool qualified = query.find("::") != std::string::npos || query.find('.') != std::string::npos;
      if (!prefix) {
        if (!qualified) return sym.name == query;
        return sym.selector == query || sym.qualified_name == query ||
          sym.qualified_name.ends_with("::" + query) || sym.qualified_name.ends_with("." + query);
      }
      if (!qualified) return sym.name.starts_with(query);
      return sym.qualified_name.starts_with(query) || sym.selector.starts_with(query) ||
        sym.qualified_name.find("::" + query) != std::string::npos ||
        sym.qualified_name.find("." + query) != std::string::npos;
    }
  } // namespace

  json executeFindSymbol(const json &call, const ToolContext &ctx, symbol_service::SymbolService *symbols) {
    LOG_MSG_DEBUG << "executeFindSymbol: " << call.dump();
    LOG_MSG_INFO << "[tool] find_symbol called";
    try {
      const auto &function = call.at("function");
      if (function.at("name") != "find_symbol") throw std::invalid_argument("Unknown tool");
      const auto arguments = function.at("arguments").get<std::string>();
      if (arguments.size() > 4096) throw std::invalid_argument("Tool arguments exceed the size limit");
      const auto args = json::parse(arguments);
      if (!args.is_object()) throw std::invalid_argument("Tool arguments must be an object");
      for (const auto &[key, value] : args.items()) {
        if (key != "name" && key != "match" && key != "role" && key != "kind" &&
            key != "path_filter" && key != "glob" && key != "limit")
          throw std::invalid_argument("Unknown find_symbol argument: " + detail::preview(key, 128));
      }
      if (!args.contains("name") || !args["name"].is_string())
        throw std::invalid_argument("name must be a string");
      const auto query = args["name"].get<std::string>();
      if (query.empty() || query.size() > 256 || query.find('\0') != std::string::npos ||
          query.find_first_of("\r\n") != std::string::npos ||
          query.find_first_not_of(" \t") == std::string::npos)
        throw std::invalid_argument("name must be single-line nonblank text, 1 to 256 bytes");

      std::string matchMode = "exact";
      if (args.contains("match")) {
        if (!args["match"].is_string()) throw std::invalid_argument("match must be a string");
        matchMode = args["match"].get<std::string>();
        if (matchMode != "exact" && matchMode != "prefix")
          throw std::invalid_argument("match must be exact or prefix");
      }
      const bool prefixMatch = matchMode == "prefix";

      std::optional<symbol_service::SymbolRole> roleFilter;
      if (args.contains("role")) {
        if (!args["role"].is_string()) throw std::invalid_argument("role must be a string");
        const auto &r = args["role"].get_ref<const std::string &>();
        if (r == "definition") roleFilter = symbol_service::SymbolRole::Definition;
        else if (r == "declaration") roleFilter = symbol_service::SymbolRole::Declaration;
        else if (r != "all") throw std::invalid_argument("role must be all, definition, or declaration");
      }

      std::optional<symbol_service::SymbolKind> kindFilter;
      if (args.contains("kind")) {
        if (!args["kind"].is_string()) throw std::invalid_argument("kind must be a string");
        kindFilter = symbol_service::parseKind(args["kind"].get_ref<const std::string &>());
        if (!kindFilter) throw std::invalid_argument("Unknown symbol kind");
      }

      size_t limit = 20;
      if (args.contains("limit")) {
        if (!args["limit"].is_number_integer() || args["limit"] < 1 || args["limit"] > 50)
          throw std::invalid_argument("limit must be an integer between 1 and 50");
        limit = args["limit"].get<size_t>();
      }

      const auto filter = detail::pathFilter(args);
      std::string globText;
      const auto globRe = detail::compileGlob(args, globText);
      const auto filteredFiles = detail::filterAllowedFiles(ctx.allowedFiles, filter, globRe);
      const auto supportedFiles = detail::collectSupportedFiles(filteredFiles);

      json result = {
        {"query", query},
        {"match", matchMode},
        {"outcome", "incomplete_search"},
        {"path_filter", filter},
        {"supported_files", supportedFiles.size()},
        {"searched_files", 0},
        {"unreadable_files", 0},
        {"incomplete", false},
        {"search_limited", false},
        {"match_count", 0},
        {"total_matches", 0},
        {"truncated", false},
        {"matches", json::array()}
      };
      if (!globText.empty()) result["glob"] = globText;

      if (filteredFiles.empty()) {
        result["outcome"] = "no_files_matched_filter";
        return result;
      }
      if (supportedFiles.empty()) {
        result["outcome"] = "no_supported_files";
        return result;
      }

      symbol_service::SymbolService localSymbols;
      symbol_service::SymbolService *activeSymbols = symbols ? symbols : &localSymbols;

      struct MatchEntry {
        std::string source;
        std::string key;
        symbol_service::Language language;
        bool exactName;
        symbol_service::SymbolInfo sym;
      };
      std::vector<MatchEntry> collected;
      size_t searchedFiles = 0;
      size_t unreadableFiles = 0;
      bool searchLimited = false;
      const auto startedAt = std::chrono::steady_clock::now();
      constexpr auto kScanBudget = std::chrono::milliseconds(5000);

      for (const auto &entry : supportedFiles) {
        if (searchedFiles > 0 && (std::chrono::steady_clock::now() - startedAt) >= kScanBudget) {
          searchLimited = true;
          break;
        }
        try {
          const auto content = detail::readTextFileChecked(entry.source);
          const auto outline = activeSymbols->outlineForContent(entry.key, entry.language, content);
          ++searchedFiles;
          for (const auto &sym : outline.symbols) {
            if (kindFilter && sym.kind != *kindFilter) continue;
            if (roleFilter && sym.role != *roleFilter) continue;
            if (!matchesFindQuery(sym, query, prefixMatch)) continue;
            const bool isExact = sym.name == query || sym.qualified_name == query || sym.selector == query;
            collected.push_back({entry.source, entry.key, outline.language, isExact, sym});
          }
        } catch (const std::exception &) {
          ++unreadableFiles;
        }
      }

      std::stable_sort(collected.begin(), collected.end(), [](const MatchEntry &a, const MatchEntry &b) {
        const bool aDef = a.sym.role == symbol_service::SymbolRole::Definition;
        const bool bDef = b.sym.role == symbol_service::SymbolRole::Definition;
        if (aDef != bDef) return aDef;
        if (a.exactName != b.exactName) return a.exactName;
        if (a.key != b.key) return a.key < b.key;
        if (a.sym.start_line != b.sym.start_line) return a.sym.start_line < b.sym.start_line;
        return a.sym.start_byte < b.sym.start_byte;
      });

      const bool incomplete = (searchedFiles + unreadableFiles < supportedFiles.size()) || unreadableFiles > 0 || searchLimited;
      result["searched_files"] = searchedFiles;
      result["unreadable_files"] = unreadableFiles;
      result["search_limited"] = searchLimited;
      result["incomplete"] = incomplete;
      result["outcome"] = incomplete ? "incomplete_search" : "complete";
      result["total_matches"] = collected.size();

      auto &arr = result["matches"];
      for (size_t i = 0; i < collected.size(); ++i) {
        if (arr.size() >= limit) {
          result["truncated"] = true;
          break;
        }
        auto item = detail::symbolEntryJson(collected[i].sym);
        item["source"] = collected[i].source;
        item["language"] = std::string(symbol_service::languageName(collected[i].language));
        arr.push_back(std::move(item));
        result["match_count"] = arr.size();
        if (result.dump().size() > kMaxResultBytes) {
          arr.erase(arr.end() - 1);
          result["match_count"] = arr.size();
          result["truncated"] = true;
          break;
        }
        if (std::find(ctx.usedSources.begin(), ctx.usedSources.end(), collected[i].source) == ctx.usedSources.end())
          ctx.usedSources.push_back(collected[i].source);
      }

      return result;
    } catch (const std::exception &e) {
      return {{"error", detail::preview(e.what(), 512)}};
    }
  }

  json executeFindReferences(const json &call, const ToolContext &ctx, symbol_service::SymbolService *symbols) {
    LOG_MSG_DEBUG << "executeFindReferences: " << call.dump();
    LOG_MSG_INFO << "[tool] find_references called";
    try {
      const auto &function = call.at("function");
      if (function.at("name") != "find_references") throw std::invalid_argument("Unknown tool");
      const auto arguments = function.at("arguments").get<std::string>();
      if (arguments.size() > 4096) throw std::invalid_argument("Tool arguments exceed the size limit");
      const auto args = json::parse(arguments);
      if (!args.is_object()) throw std::invalid_argument("Tool arguments must be an object");
      for (const auto &[key, value] : args.items()) {
        if (key != "symbol" && key != "include_declarations" && key != "path_filter" && key != "glob" && key != "limit")
          throw std::invalid_argument("Unknown find_references argument: " + detail::preview(key, 128));
      }
      if (!args.contains("symbol") || !args["symbol"].is_string())
        throw std::invalid_argument("symbol must be a string");
      const auto query = args["symbol"].get<std::string>();
      if (query.empty() || query.size() > 256 || query.find('\0') != std::string::npos ||
          query.find_first_of("\r\n") != std::string::npos ||
          query.find_first_not_of(" \t") == std::string::npos)
        throw std::invalid_argument("symbol must be single-line nonblank text, 1 to 256 bytes");

      std::string baseName = query;
      const auto cppSep = query.rfind("::");
      const auto dotSep = query.rfind('.');
      if (cppSep != std::string::npos && (dotSep == std::string::npos || cppSep > dotSep)) {
        baseName = query.substr(cppSep + 2);
      } else if (dotSep != std::string::npos) {
        baseName = query.substr(dotSep + 1);
      }
      baseName = utils::trimmed(baseName);
      if (baseName.empty())
        throw std::invalid_argument("symbol must specify a valid identifier name");

      bool includeDeclarations = false;
      if (args.contains("include_declarations")) {
        if (!args["include_declarations"].is_boolean())
          throw std::invalid_argument("include_declarations must be a boolean");
        includeDeclarations = args["include_declarations"].get<bool>();
      }

      size_t limit = 30;
      if (args.contains("limit")) {
        if (!args["limit"].is_number_integer() || args["limit"] < 1 || args["limit"] > 100)
          throw std::invalid_argument("limit must be an integer between 1 and 100");
        limit = args["limit"].get<size_t>();
      }

      const auto filter = detail::pathFilter(args);
      std::string globText;
      const auto globRe = detail::compileGlob(args, globText);
      const auto filteredFiles = detail::filterAllowedFiles(ctx.allowedFiles, filter, globRe);
      const auto supportedFiles = detail::collectSupportedFiles(filteredFiles);

      json result = {
        {"symbol", query},
        {"base_name", baseName},
        {"include_declarations", includeDeclarations},
        {"outcome", "incomplete_search"},
        {"path_filter", filter},
        {"supported_files", supportedFiles.size()},
        {"searched_files", 0},
        {"parsed_files", 0},
        {"unreadable_files", 0},
        {"incomplete", false},
        {"search_limited", false},
        {"reference_count", 0},
        {"total_references", 0},
        {"truncated", false},
        {"references", json::array()}
      };
      if (!globText.empty()) result["glob"] = globText;

      if (filteredFiles.empty()) {
        result["outcome"] = "no_files_matched_filter";
        return result;
      }
      if (supportedFiles.empty()) {
        result["outcome"] = "no_supported_files";
        return result;
      }

      const std::string leafPrefilter = (baseName.size() > 1 && baseName.front() == '~') ? baseName.substr(1) : baseName;
      re2::RE2::Options preOpt;
      preOpt.set_log_errors(false);
      preOpt.set_max_mem(1 << 20);
      preOpt.set_case_sensitive(true);
      re2::RE2 prefilter("(?:^|[^A-Za-z0-9_])" + re2::RE2::QuoteMeta(leafPrefilter) + "(?:$|[^A-Za-z0-9_])", preOpt);
      if (!prefilter.ok()) throw std::invalid_argument("Unable to compile symbol reference filter");

      symbol_service::SymbolService localSymbols;
      symbol_service::SymbolService *activeSymbols = symbols ? symbols : &localSymbols;

      struct RefHit {
        std::string source;
        std::string key;
        symbol_service::Language language;
        symbol_service::ReferenceMatch match;
      };
      std::vector<RefHit> collected;
      size_t searchedFiles = 0;
      size_t parsedFiles = 0;
      size_t unreadableFiles = 0;
      bool searchLimited = false;
      bool fileMatchLimited = false;
      const auto startedAt = std::chrono::steady_clock::now();
      constexpr auto kScanBudget = std::chrono::milliseconds(5000);
      constexpr size_t kMaxReferencesPerFile = 25;

      for (const auto &entry : supportedFiles) {
        if (searchedFiles > 0 && (std::chrono::steady_clock::now() - startedAt) >= kScanBudget) {
          searchLimited = true;
          break;
        }
        try {
          const auto content = detail::readTextFileChecked(entry.source);
          ++searchedFiles;
          if (!re2::RE2::PartialMatch(content, prefilter)) continue;
          const auto outline = activeSymbols->outlineForContent(entry.key, entry.language, content);
          ++parsedFiles;
          auto refs = symbol_service::findReferencesInContent(entry.language, content, query, includeDeclarations, &outline.symbols);
          size_t perFile = 0;
          for (auto &r : refs) {
            if (perFile >= kMaxReferencesPerFile) {
              fileMatchLimited = true;
              break;
            }
            collected.push_back({entry.source, entry.key, outline.language, std::move(r)});
            ++perFile;
          }
        } catch (const std::exception &) {
          ++unreadableFiles;
        }
      }

      std::stable_sort(collected.begin(), collected.end(), [](const RefHit &a, const RefHit &b) {
        if (a.match.exact_qualifier != b.match.exact_qualifier) return a.match.exact_qualifier;
        const bool aIsUsage = a.match.kind != "definition" && a.match.kind != "declaration";
        const bool bIsUsage = b.match.kind != "definition" && b.match.kind != "declaration";
        if (aIsUsage != bIsUsage) return aIsUsage;
        if (a.key != b.key) return a.key < b.key;
        if (a.match.line != b.match.line) return a.match.line < b.match.line;
        return a.match.column < b.match.column;
      });

      const bool incomplete = (searchedFiles + unreadableFiles < supportedFiles.size()) || unreadableFiles > 0 || searchLimited;
      result["searched_files"] = searchedFiles;
      result["parsed_files"] = parsedFiles;
      result["unreadable_files"] = unreadableFiles;
      result["search_limited"] = searchLimited;
      result["file_match_limited"] = fileMatchLimited;
      result["per_file_limit"] = kMaxReferencesPerFile;
      result["incomplete"] = incomplete;
      result["outcome"] = incomplete ? "incomplete_search" : "complete";
      result["total_references"] = collected.size();
      if (fileMatchLimited) result["truncated"] = true;

      auto &arr = result["references"];
      for (size_t i = 0; i < collected.size(); ++i) {
        if (arr.size() >= limit) {
          result["truncated"] = true;
          break;
        }
        arr.push_back({
          {"source", collected[i].source},
          {"language", std::string(symbol_service::languageName(collected[i].language))},
          {"line", collected[i].match.line},
          {"column", collected[i].match.column},
          {"kind", collected[i].match.kind},
          {"enclosing_symbol", std::string{ utils::sanitizeUtf8(collected[i].match.enclosing_symbol) }},
          {"exact_qualifier", collected[i].match.exact_qualifier},
          {"snippet", std::string{ utils::sanitizeUtf8(collected[i].match.snippet) }}
        });
        result["reference_count"] = arr.size();
        if (result.dump().size() > kMaxResultBytes) {
          arr.erase(arr.end() - 1);
          result["reference_count"] = arr.size();
          result["truncated"] = true;
          break;
        }
        detail::recordUsedSource(ctx.usedSources, collected[i].source);
      }

      return result;
    } catch (const std::exception &e) {
      return {{"error", detail::preview(e.what(), 512)}};
    }
  }
} // namespace chat_tools
