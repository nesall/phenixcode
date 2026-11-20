#include "chat_tools.h"
#include "symbol_service.h"
#include "cutils.h"
#include <utils_log/logger.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <re2/re2.h>

using nlohmann::json;

namespace chat_tools {
  namespace {
    std::string normalizedSourceKey(const std::string &selected) {
      return utils::normalizePathKey(selected);
    }
  } // namespace

  namespace detail {
    std::vector<SupportedEntry> collectSupportedFiles(const std::vector<std::string> &files) {
      std::vector<SupportedEntry> out;
      out.reserve(files.size());
      for (const auto &file : files) {
        const auto lang = symbol_service::detectLanguage(file);
        if (lang != symbol_service::Language::Unsupported) {
          out.push_back({file, normalizedSourceKey(file), lang});
        }
      }
      return out;
    }

    void recordUsedSource(std::vector<std::string> &usedSources, const std::string &source) {
      if (!source.empty() && std::find(usedSources.begin(), usedSources.end(), source) == usedSources.end()) {
        usedSources.push_back(source);
      }
    }

    json symbolEntryJson(const symbol_service::SymbolInfo &s) {
      return {
        {"selector", s.selector},
        {"name", std::string{ utils::sanitizeUtf8(s.name) }},
        {"qualified_name", std::string{ utils::sanitizeUtf8(s.qualified_name) }},
        {"container", std::string{ utils::sanitizeUtf8(s.container) }},
        {"kind", std::string(symbol_service::kindName(s.kind))},
        {"role", std::string(symbol_service::roleName(s.role))},
        {"signature", std::string{ utils::sanitizeUtf8(s.signature) }},
        {"start_line", s.start_line},
        {"end_line", s.end_line}
      };
    }
  } // namespace detail

  namespace {
    symbol_service::FileOutline computeVerifiedOutline(const std::string &selected, std::string &contentOut,
                                                       symbol_service::SymbolService *symbols) {
      const auto lang = symbol_service::detectLanguage(selected);
      if (lang == symbol_service::Language::Unsupported) {
        throw std::invalid_argument(
          "Unsupported file type for symbol extraction (supported: C, C++, Python, JavaScript/JSX, TypeScript, TSX; .h defaults to C++; .svelte is unsupported)");
      }
      contentOut = detail::readTextFileChecked(selected);
      const std::string key = normalizedSourceKey(selected);
      if (symbols) {
        return symbols->outlineForContent(key, lang, contentOut);
      }
      symbol_service::SymbolService local;
      return local.outlineForContent(key, lang, contentOut);
    }

    json formatSymbolSlice(const std::string &selected, symbol_service::Language lang,
                           const symbol_service::SymbolInfo &sym,
                           const std::string &content, size_t maxJsonBytes) {
      if (sym.start_byte > sym.end_byte || sym.end_byte > content.size()) {
        throw std::runtime_error("Validated symbol byte range exceeds snapshot bounds");
      }
      json result = {
        {"source", selected},
        {"language", std::string(symbol_service::languageName(lang))},
        {"selector", sym.selector},
        {"name", std::string{ utils::sanitizeUtf8(sym.name) }},
        {"qualified_name", std::string{ utils::sanitizeUtf8(sym.qualified_name) }},
        {"container", std::string{ utils::sanitizeUtf8(sym.container) }},
        {"kind", std::string(symbol_service::kindName(sym.kind))},
        {"role", std::string(symbol_service::roleName(sym.role))},
        {"signature", std::string{ utils::sanitizeUtf8(sym.signature) }},
        {"start_line", sym.start_line},
        {"end_line", sym.start_line},
        {"content", ""},
        {"truncated", false}
      };
      if (result.dump().size() > maxJsonBytes - 128) {
        throw std::invalid_argument("Symbol metadata exceeds the result size limit");
      }

      const std::string_view rawSlice(content.data() + sym.start_byte, sym.end_byte - sym.start_byte);
      size_t pos = 0;
      size_t line = sym.start_line;
      size_t linesEmitted = 0;
      std::string snippet;
      while (pos < rawSlice.size() && linesEmitted < 200) {
        const auto newline = rawSlice.find('\n', pos);
        const auto end = newline == std::string_view::npos ? rawSlice.size() : newline;
        std::string text(rawSlice.substr(pos, end - pos));
        if (!text.empty() && text.back() == '\r') text.pop_back();
        const std::string valid{ utils::sanitizeUtf8(text) };
        std::string numbered = std::to_string(line) + ": " + valid + "\n";
        result["content"] = snippet + numbered;
        result["end_line"] = line;
        if (result.dump().size() > maxJsonBytes - 128) {
          result["truncated"] = true;
          if (!snippet.empty()) {
            result["content"] = snippet;
            result["end_line"] = line - 1;
            result["next_start_line"] = line;
          } else {
            result["line_truncated"] = true;
            result["next_start_line"] = line + 1;
            do {
              auto length = numbered.size() / 2;
              while (length && (static_cast<unsigned char>(numbered[length]) & 0xc0) == 0x80) --length;
              numbered.resize(length);
              result["content"] = numbered;
            } while (result.dump().size() > maxJsonBytes);
          }
          break;
        }
        snippet += numbered;
        ++linesEmitted;
        if (newline == std::string_view::npos) {
          pos = rawSlice.size();
        } else {
          pos = newline + 1;
          ++line;
        }
      }
      if (!result["truncated"].get<bool>() && pos < rawSlice.size()) {
        result["truncated"] = true;
        result["next_start_line"] = line;
      }
      return result;
    }

    std::vector<const symbol_service::SymbolInfo *> matchSymbolCandidates(
      const std::vector<symbol_service::SymbolInfo> &symbols,
      const std::string &query,
      std::optional<symbol_service::SymbolRole> roleFilter) {
      std::vector<const symbol_service::SymbolInfo *> pool;
      pool.reserve(symbols.size());
      for (const auto &sym : symbols) {
        if (!roleFilter || sym.role == *roleFilter) pool.push_back(&sym);
      }

      auto collect = [&](auto &&pred) {
        std::vector<const symbol_service::SymbolInfo *> hits;
        for (const auto *sym : pool) {
          if (pred(*sym)) hits.push_back(sym);
        }
        return hits;
      };

      if (auto hits = collect([&](const auto &s) { return s.selector == query; }); !hits.empty()) return hits;
      if (auto hits = collect([&](const auto &s) { return s.qualified_name == query; }); !hits.empty()) return hits;
      if (query.find("::") != std::string::npos || query.find('.') != std::string::npos) {
        const std::string cppSuffix = "::" + query;
        const std::string dotSuffix = "." + query;
        if (auto hits = collect([&](const auto &s) {
          return s.qualified_name.ends_with(cppSuffix) || s.qualified_name.ends_with(dotSuffix);
        }); !hits.empty()) return hits;
      } else {
        if (auto hits = collect([&](const auto &s) { return s.name == query; }); !hits.empty()) return hits;
      }
      return {};
    }
  } // namespace

  json executeSymbolOutline(const json &call, const ToolContext &ctx, symbol_service::SymbolService *symbols) {
    LOG_MSG_DEBUG << "executeSymbolOutline: " << call.dump();
    LOG_MSG_INFO << "[tool] symbol_outline called";
    try {
      const auto &function = call.at("function");
      if (function.at("name") != "symbol_outline") throw std::invalid_argument("Unknown tool");
      const auto arguments = function.at("arguments").get<std::string>();
      if (arguments.size() > 4096) throw std::invalid_argument("Tool arguments exceed the size limit");
      const auto args = json::parse(arguments);
      if (!args.is_object()) throw std::invalid_argument("Tool arguments must be an object");
      for (const auto &[key, value] : args.items()) {
        if (key != "path" && key != "kind" && key != "role" && key != "limit" && key != "offset")
          throw std::invalid_argument("Unknown symbol_outline argument: " + detail::preview(key, 128));
      }
      if (!args.contains("path") || !args["path"].is_string())
        throw std::invalid_argument("path must be a string");

      std::optional<symbol_service::SymbolKind> kindFilter;
      if (args.contains("kind")) {
        if (!args["kind"].is_string()) throw std::invalid_argument("kind must be a string");
        kindFilter = symbol_service::parseKind(args["kind"].get_ref<const std::string &>());
        if (!kindFilter) throw std::invalid_argument("Unknown symbol kind");
      }

      std::optional<symbol_service::SymbolRole> roleFilter;
      if (args.contains("role")) {
        if (!args["role"].is_string()) throw std::invalid_argument("role must be a string");
        const auto &r = args["role"].get_ref<const std::string &>();
        if (r == "definition") roleFilter = symbol_service::SymbolRole::Definition;
        else if (r == "declaration") roleFilter = symbol_service::SymbolRole::Declaration;
        else if (r != "all") throw std::invalid_argument("role must be all, definition, or declaration");
      }

      size_t limit = 200;
      if (args.contains("limit")) {
        if (!args["limit"].is_number_integer() || args["limit"] < 1 || args["limit"] > 200)
          throw std::invalid_argument("limit must be an integer between 1 and 200");
        limit = args["limit"].get<size_t>();
      }

      size_t offset = 0;
      if (args.contains("offset")) {
        if (!args["offset"].is_number_integer() || args["offset"] < 0)
          throw std::invalid_argument("offset must be a non-negative integer");
        offset = args["offset"].get<size_t>();
      }

      const auto selected = detail::resolveAllowedFile(args["path"].get<std::string>(), ctx.allowedFiles);
      std::string content;
      const auto outline = computeVerifiedOutline(selected, content, symbols);

      std::vector<const symbol_service::SymbolInfo *> filtered;
      filtered.reserve(outline.symbols.size());
      for (const auto &sym : outline.symbols) {
        if (kindFilter && sym.kind != *kindFilter) continue;
        if (roleFilter && sym.role != *roleFilter) continue;
        filtered.push_back(&sym);
      }

      json result = {
        {"source", selected},
        {"language", std::string(symbol_service::languageName(outline.language))},
        {"symbol_count", 0},
        {"total_symbols", filtered.size()},
        {"truncated", false},
        {"symbols", json::array()}
      };

      if (offset < filtered.size()) {
        auto &arr = result["symbols"];
        size_t emitted = 0;
        for (size_t i = offset; i < filtered.size(); ++i) {
          if (emitted >= limit) {
            result["truncated"] = true;
            result["next_offset"] = i;
            break;
          }
          arr.push_back(detail::symbolEntryJson(*filtered[i]));
          result["symbol_count"] = arr.size();
          if (result.dump().size() > kMaxResultBytes) {
            arr.erase(arr.end() - 1);
            result["symbol_count"] = arr.size();
            result["truncated"] = true;
            result["next_offset"] = i;
            break;
          }
          ++emitted;
        }
      }

      if (std::find(ctx.usedSources.begin(), ctx.usedSources.end(), selected) == ctx.usedSources.end())
        ctx.usedSources.push_back(selected);
      return result;
    } catch (const std::exception &e) {
      return {{"error", detail::preview(e.what(), 512)}};
    }
  }

  json executeReadSymbol(const json &call, const ToolContext &ctx, symbol_service::SymbolService *symbols) {
    LOG_MSG_DEBUG << "executeReadSymbol: " << call.dump();
    LOG_MSG_INFO << "[tool] read_symbol called";
    try {
      const auto &function = call.at("function");
      if (function.at("name") != "read_symbol") throw std::invalid_argument("Unknown tool");
      const auto arguments = function.at("arguments").get<std::string>();
      if (arguments.size() > 4096) throw std::invalid_argument("Tool arguments exceed the size limit");
      const auto args = json::parse(arguments);
      if (!args.is_object()) throw std::invalid_argument("Tool arguments must be an object");
      for (const auto &[key, value] : args.items()) {
        if (key != "path" && key != "symbol" && key != "role")
          throw std::invalid_argument("Unknown read_symbol argument: " + detail::preview(key, 128));
      }
      if (!args.contains("path") || !args["path"].is_string())
        throw std::invalid_argument("path must be a string");
      if (!args.contains("symbol") || !args["symbol"].is_string())
        throw std::invalid_argument("symbol must be a string");
      const auto query = args["symbol"].get<std::string>();
      if (query.empty() || query.size() > 256 || query.find('\0') != std::string::npos ||
          query.find_first_of("\r\n") != std::string::npos ||
          query.find_first_not_of(" \t") == std::string::npos)
        throw std::invalid_argument("symbol must be single-line nonblank text, 1 to 256 bytes");

      std::optional<symbol_service::SymbolRole> roleFilter;
      if (args.contains("role")) {
        if (!args["role"].is_string()) throw std::invalid_argument("role must be a string");
        const auto &r = args["role"].get_ref<const std::string &>();
        if (r == "definition") roleFilter = symbol_service::SymbolRole::Definition;
        else if (r == "declaration") roleFilter = symbol_service::SymbolRole::Declaration;
        else if (r != "any") throw std::invalid_argument("role must be any, definition, or declaration");
      }

      const auto selected = detail::resolveAllowedFile(args["path"].get<std::string>(), ctx.allowedFiles);
      std::string content;
      const auto outline = computeVerifiedOutline(selected, content, symbols);

      const auto matches = matchSymbolCandidates(outline.symbols, query, roleFilter);
      if (matches.empty()) {
        if (roleFilter) {
          const auto anyMatches = matchSymbolCandidates(outline.symbols, query, std::nullopt);
          if (!anyMatches.empty()) {
            const auto asked = symbol_service::roleName(*roleFilter);
            const auto other = *roleFilter == symbol_service::SymbolRole::Definition ? "declaration" : "definition";
            return {{"error", "No " + std::string(asked) + " found for '" + detail::preview(query, 128) +
              "' in " + detail::preview(selected, 128) + " (found " + std::to_string(anyMatches.size()) + " " + other + ")"}};
          }
        }
        return {{"error", "Symbol not found: " + detail::preview(query, 128)}};
      }

      if (matches.size() > 1) {
        json result = {
          {"source", selected},
          {"language", std::string(symbol_service::languageName(outline.language))},
          {"ambiguous", true},
          {"message", "Multiple symbols matched; pass one of the candidate selectors or filter by role"},
          {"candidate_count", 0},
          {"total_candidates", matches.size()},
          {"truncated", false},
          {"candidates", json::array()}
        };
        auto &arr = result["candidates"];
        for (const auto *sym : matches) {
          arr.push_back(detail::symbolEntryJson(*sym));
          result["candidate_count"] = arr.size();
          if (result.dump().size() > kMaxResultBytes) {
            arr.erase(arr.end() - 1);
            result["candidate_count"] = arr.size();
            result["truncated"] = true;
            break;
          }
        }
        return result;
      }

      auto result = formatSymbolSlice(selected, outline.language, *matches.front(), content, kMaxResultBytes);
      if (std::find(ctx.usedSources.begin(), ctx.usedSources.end(), selected) == ctx.usedSources.end())
        ctx.usedSources.push_back(selected);
      return result;
    } catch (const std::exception &e) {
      return {{"error", detail::preview(e.what(), 512)}};
    }
  }

  json executeDependencies(const json &call, const ToolContext &ctx,
                           symbol_service::SymbolService *symbols, const DependencyReader &readIndexed) {
    LOG_MSG_DEBUG << "executeDependencies: " << call.dump();
    LOG_MSG_INFO << "[tool] dependencies called";
    try {
      const auto &function = call.at("function");
      if (function.at("name") != "dependencies") throw std::invalid_argument("Unknown tool");
      const auto arguments = function.at("arguments").get<std::string>();
      if (arguments.size() > 4096) throw std::invalid_argument("Tool arguments exceed the size limit");
      const auto args = json::parse(arguments);
      if (!args.is_object()) throw std::invalid_argument("Tool arguments must be an object");
      for (const auto &[key, value] : args.items()) {
        if (key != "path" && key != "direction" && key != "include_external" && key != "limit")
          throw std::invalid_argument("Unknown dependencies argument: " + detail::preview(key, 128));
      }
      if (!args.contains("path") || !args["path"].is_string())
        throw std::invalid_argument("path must be a string");

      std::string direction = "both";
      if (args.contains("direction")) {
        if (!args["direction"].is_string()) throw std::invalid_argument("direction must be a string");
        direction = args["direction"].get<std::string>();
        if (direction != "both" && direction != "outgoing" && direction != "incoming")
          throw std::invalid_argument("direction must be both, outgoing, or incoming");
      }
      const bool wantOutgoing = direction == "both" || direction == "outgoing";
      const bool wantIncoming = direction == "both" || direction == "incoming";

      bool includeExternal = true;
      if (args.contains("include_external")) {
        if (!args["include_external"].is_boolean()) throw std::invalid_argument("include_external must be a boolean");
        includeExternal = args["include_external"].get<bool>();
      }

      size_t limit = 50;
      if (args.contains("limit")) {
        if (!args["limit"].is_number_integer() || args["limit"] < 1 || args["limit"] > 100)
          throw std::invalid_argument("limit must be an integer between 1 and 100");
        limit = args["limit"].get<size_t>();
      }

      const auto selected = detail::resolveAllowedFile(args["path"].get<std::string>(), ctx.allowedFiles);
      const auto selectedKey = normalizedSourceKey(selected);
      const auto selectedLang = symbol_service::detectLanguage(selected);

      std::unordered_map<std::string, std::string> allowedByKey;
      allowedByKey.reserve(ctx.allowedFiles.size());
      for (const auto &file : ctx.allowedFiles) {
        allowedByKey.emplace(normalizedSourceKey(file), file);
      }

      IndexedDependencyQuery indexed;
      if (readIndexed) {
        indexed = readIndexed(selectedKey);
      }

      symbol_service::SymbolService localSymbols;
      symbol_service::SymbolService *activeSymbols = symbols ? symbols : &localSymbols;

      const bool useIndexedOutgoing = wantOutgoing && indexed.hasIndexedGraph && indexed.sourceTracked;
      const bool useIndexedIncoming = wantIncoming && indexed.hasIndexedGraph;
      const bool outgoingUsedIndex = wantOutgoing && useIndexedOutgoing;
      const bool outgoingUsedLive = wantOutgoing && !useIndexedOutgoing;
      const bool incomingUsedIndex = wantIncoming && useIndexedIncoming;
      const bool incomingUsedLive = wantIncoming && !useIndexedIncoming;
      const bool anyIndex = outgoingUsedIndex || incomingUsedIndex;
      const bool anyLive = outgoingUsedLive || incomingUsedLive;
      const std::string graphSource = (anyIndex && anyLive) ? "mixed" : (anyIndex ? "index" : "live");
      const std::string freshness = (anyIndex && anyLive) ? "mixed" : (anyIndex ? "unknown" : "current");

      struct OutgoingRow {
        std::string target;
        std::string resolved;
        std::string kind;
        size_t line = 0;
        bool isSystem = false;
        bool isExternal = false;
      };
      struct IncomingRow {
        std::string source;
        std::string target;
        std::string kind;
        size_t line = 0;
      };
      std::vector<OutgoingRow> outgoingRows;
      std::vector<IncomingRow> incomingRows;
      size_t unreadableFiles = 0;
      size_t searchedFiles = 0;
      size_t supportedCount = 0;
      bool searchLimited = false;
      bool incomplete = false;

      if (wantOutgoing) {
        if (useIndexedOutgoing) {
          for (const auto &dep : indexed.outgoing) {
            std::string canonicalResolved;
            if (!dep.resolvedPath.empty()) {
              if (const auto it = allowedByKey.find(normalizedSourceKey(dep.resolvedPath)); it != allowedByKey.end()) {
                canonicalResolved = it->second;
              }
            }
            const bool isExt = canonicalResolved.empty();
            if (isExt && !includeExternal) continue;
            outgoingRows.push_back({dep.targetSpec, canonicalResolved, dep.kind, dep.line, dep.isSystem, isExt});
          }
        } else if (selectedLang != symbol_service::Language::Unsupported) {
          const auto content = detail::readTextFileChecked(selected);
          const auto outline = activeSymbols->outlineForContent(selectedKey, selectedLang, content);
          const auto resolvedDeps = symbol_service::resolveImports(selectedKey, selectedLang, outline.imports, ctx.allowedFiles);
          for (const auto &dep : resolvedDeps) {
            std::string canonicalResolved;
            if (!dep.resolved_path.empty()) {
              if (const auto it = allowedByKey.find(dep.resolved_path); it != allowedByKey.end()) {
                canonicalResolved = it->second;
              }
            }
            const bool isExt = canonicalResolved.empty();
            if (isExt && !includeExternal) continue;
            outgoingRows.push_back({dep.target_spec, canonicalResolved, dep.kind, dep.line, dep.is_system, isExt});
          }
        }
      }

      if (wantIncoming) {
        if (useIndexedIncoming) {
          for (const auto &dep : indexed.incoming) {
            const auto it = allowedByKey.find(normalizedSourceKey(dep.sourcePath));
            if (it == allowedByKey.end()) continue;
            incomingRows.push_back({it->second, dep.targetSpec, dep.kind, dep.line});
          }
        } else {
          const auto selectedPath = std::filesystem::path(std::u8string(selectedKey.begin(), selectedKey.end()));
          const std::string stem = selectedPath.stem().string();
          const std::string parentName = selectedPath.parent_path().filename().string();
          const bool indexOrInit = (stem == "index" || stem == "__init__");
          const std::string needle = (indexOrInit && !parentName.empty()) ? parentName : stem;
          std::unique_ptr<re2::RE2> incomingPrefilter;
          if (!needle.empty()) {
            re2::RE2::Options preOpt;
            preOpt.set_log_errors(false);
            preOpt.set_max_mem(1 << 20);
            preOpt.set_case_sensitive(false);
            const std::string pat = indexOrInit
              ? ("(?:" + re2::RE2::QuoteMeta(needle) + "|['\"]\\.{1,2}(?:/['\"])?)")
              : re2::RE2::QuoteMeta(needle);
            auto re = std::make_unique<re2::RE2>(pat, preOpt);
            if (re->ok()) incomingPrefilter = std::move(re);
          }

          const auto supportedFiles = detail::collectSupportedFiles(ctx.allowedFiles);
          supportedCount = supportedFiles.size();
          const auto startedAt = std::chrono::steady_clock::now();
          constexpr auto kScanBudget = std::chrono::milliseconds(5000);
          for (const auto &entry : supportedFiles) {
            if (searchedFiles > 0 && (std::chrono::steady_clock::now() - startedAt) >= kScanBudget) {
              searchLimited = true;
              break;
            }
            if (entry.key == selectedKey) {
              ++searchedFiles;
              continue;
            }
            try {
              const auto content = detail::readTextFileChecked(entry.source);
              if (incomingPrefilter && !re2::RE2::PartialMatch(content, *incomingPrefilter)) {
                ++searchedFiles;
                continue;
              }
              const auto outline = activeSymbols->outlineForContent(entry.key, entry.language, content);
              ++searchedFiles;
              const auto resolvedDeps = symbol_service::resolveImports(entry.key, outline.language, outline.imports, ctx.allowedFiles);
              for (const auto &dep : resolvedDeps) {
                if (dep.resolved_path == selectedKey) {
                  incomingRows.push_back({entry.source, dep.target_spec, dep.kind, dep.line});
                }
              }
            } catch (const std::exception &) {
              ++unreadableFiles;
            }
          }
          if (searchedFiles + unreadableFiles < supportedCount || unreadableFiles > 0 || searchLimited) {
            incomplete = true;
          }
        }
      }

      std::stable_sort(incomingRows.begin(), incomingRows.end(), [](const IncomingRow &a, const IncomingRow &b) {
        if (a.source != b.source) return a.source < b.source;
        return a.line < b.line;
      });

      json result = {
        {"source", selected},
        {"language", std::string(symbol_service::languageName(selectedLang))},
        {"direction", direction},
        {"graph_source", graphSource},
        {"freshness", freshness},
        {"outgoing_source", wantOutgoing ? (useIndexedOutgoing ? "index" : "live") : "none"},
        {"incoming_source", wantIncoming ? (useIndexedIncoming ? "index" : "live") : "none"},
        {"incomplete", incomplete},
        {"supported_files", incomingUsedLive ? json(supportedCount) : json(nullptr)},
        {"searched_files", incomingUsedLive ? json(searchedFiles) : json(nullptr)},
        {"unreadable_files", unreadableFiles},
        {"search_limited", searchLimited},
        {"outgoing", json::array()},
        {"outgoing_count", 0},
        {"total_outgoing", outgoingRows.size()},
        {"incoming", json::array()},
        {"incoming_count", 0},
        {"total_incoming", incomingRows.size()},
        {"truncated", false}
      };

      detail::recordUsedSource(ctx.usedSources, selected);

      auto &outArr = result["outgoing"];
      for (const auto &row : outgoingRows) {
        if (outArr.size() >= limit) {
          result["truncated"] = true;
          break;
        }
        outArr.push_back({
          {"target", row.target},
          {"resolved", row.resolved.empty() ? json(nullptr) : json(row.resolved)},
          {"kind", row.kind},
          {"line", row.line},
          {"is_system", row.isSystem},
          {"external", row.isExternal}
        });
        result["outgoing_count"] = outArr.size();
        if (result.dump().size() > kMaxResultBytes) {
          outArr.erase(outArr.end() - 1);
          result["outgoing_count"] = outArr.size();
          result["truncated"] = true;
          break;
        }
        if (!row.resolved.empty()) detail::recordUsedSource(ctx.usedSources, row.resolved);
      }

      auto &inArr = result["incoming"];
      for (const auto &row : incomingRows) {
        if (inArr.size() >= limit) {
          result["truncated"] = true;
          break;
        }
        inArr.push_back({
          {"source", row.source},
          {"target", row.target},
          {"kind", row.kind},
          {"line", row.line}
        });
        result["incoming_count"] = inArr.size();
        if (result.dump().size() > kMaxResultBytes) {
          inArr.erase(inArr.end() - 1);
          result["incoming_count"] = inArr.size();
          result["truncated"] = true;
          break;
        }
        detail::recordUsedSource(ctx.usedSources, row.source);
      }

      return result;
    } catch (const std::exception &e) {
      return {{"error", detail::preview(e.what(), 512)}};
    }
  }
} // namespace chat_tools
