#include "chat_tools.h"
#include "settings.h"
#include "cutils.h"
#include <utils_log/logger.hpp>
#include <algorithm>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include <3rdparty/utf8.h>

using nlohmann::json;

namespace chat_tools {
  namespace {
    std::string preview(std::string text, size_t maximum) {
      if (text.size() > maximum) {
        while (maximum && (static_cast<unsigned char>(text[maximum]) & 0xc0) == 0x80) --maximum;
        text.resize(maximum);
      }
      std::replace(text.begin(), text.end(), '\n', ' ');
      std::replace(text.begin(), text.end(), '\r', ' ');
      return text;
    }
  }

  bool enabled(bool agentEnabled, const ApiConfig &api, bool freeChat, bool attachedOnly)
  {
    return agentEnabled && !freeChat && !attachedOnly && api.supportsTools == "on" && api.apiStyle == ApiConfig::ApiStyle::OpenAI;
  }

  json grepDefinition()
  {
    return {{"type", "function"}, {"function", {
      {"name", "grep"},
      {"description", "Search configured project source files for literal text (case insensitive). Returns snippets with line numbers."},
      {"parameters", {{"type", "object"}, {"properties", {
        {"pattern", {{"type", "string"}, {"description", "Literal text, 2 to 64 bytes; not a regular expression"}}},
        {"path_filter", {{"type", "string"}, {"description", "Optional path prefix, e.g. src/; only configured project files are searched"}}}
      }}, {"required", json::array({"pattern"})}, {"additionalProperties", false}}}
    }}};
  }

  json readFileDefinition() {
    return {{"type", "function"}, {"function", {
      {"name", "read_file"},
      {"description", "Read a configured project text file, using a source path returned by the search or listing tools. Returns numbered lines, up to 200 lines and 8 KiB per call."},
      {"parameters", {{"type", "object"}, {"properties", {
        {"path", {{"type", "string"}, {"description", "Source path returned by a search or listing tool, or an unambiguous relative path"}}},
        {"start_line", {{"type", "integer"}, {"minimum", 1}, {"description", "First line (1-based), default 1"}}},
        {"end_line", {{"type", "integer"}, {"minimum", 1}, {"description", "Last line (inclusive), default start_line + 199"}}}
      }}, {"required", json::array({"path"})}, {"additionalProperties", false}}}
    }}};
  }

  nlohmann::json listFilesDefinition()
  {
    return {
      { "type", "function" },
      { "function",{
        { "name", "list_files" },
        { "description",
         "List files available in the project source scope. Optionally narrow by path "
         "prefix or file extension. Returns up to 200 files and 8 KiB. "
         "Use to discover file names before read_file or grep." },
        { "parameters",{
          { "type", "object" },
          { "properties",{
            { "path_filter",{
              { "type", "string" },
              { "description", "Optional path prefix to narrow scope, e.g. 'src'. Must be inside project sources." }
            } },
            { "extension",{
              { "type", "string" },
              { "description", "Optional file extension filter including dot, e.g. '.cpp'." }
            } }
          } },
          { "required", json::array() },
          { "additionalProperties", false }
        } }
      } }
    };
  }

  json searchContextDefinition()
  {
    return {{"type", "function"}, {"function", {
      {"name", "search_context"},
      {"description", "Search project context by a natural-language question using the existing hybrid retriever. "
        "Falls back to text search when embeddings or the index are unavailable. Returns up to 8 excerpts and 8 KiB. "
        "Indexed excerpts can be stale; use read_file for current contents. Line ranges are 1-based inclusive; byte ranges are 0-based end-exclusive."},
      {"parameters", {{"type", "object"}, {"properties", {
        {"query", {{"type", "string"}, {"description", "Focused search question, 1 to 512 UTF-8 bytes"}, {"minLength", 1}, {"maxLength", 512}}}
      }}, {"required", json::array({"query"})}, {"additionalProperties", false}}}
    }}};
  }

  json executeSearchContext(const json &call, const ToolContext &ctx, const ContextSearch &search)
  {
    LOG_MSG_DEBUG << "executeSearchContext: " << call.dump();
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
      return {{"error", preview(e.what(), 512)}};
    }
  }

  json executeReadFile(const json &call, const ToolContext &ctx)
  {
    LOG_MSG_DEBUG << "executeReadFile: " << call.dump();
    try {
      const auto &function = call.at("function");
      if (function.at("name") != "read_file") throw std::invalid_argument("Unknown tool");
      const auto arguments = function.at("arguments").get<std::string>();
      if (arguments.size() > 4096) throw std::invalid_argument("Tool arguments exceed the size limit");
      const auto args = json::parse(arguments);
      if (!args.is_object()) throw std::invalid_argument("Tool arguments must be an object");
      for (const auto &[key, value] : args.items())
        if (key != "path" && key != "start_line" && key != "end_line") throw std::invalid_argument("Unknown read_file argument: " + key);
      auto path = args.at("path").get<std::string>();
      if (path.empty() || path.size() > 1024 || path.find('\0') != std::string::npos) throw std::invalid_argument("Invalid file path");
      std::replace(path.begin(), path.end(), '\\', '/');
      for (const auto &part : std::filesystem::path(path))
        if (part == "..") throw std::invalid_argument("File path must not contain parent traversal");
      const auto requested = std::filesystem::path(path).lexically_normal();
      const auto name = requested.generic_string();
      std::string selected;
      for (const auto &file : ctx.allowedFiles) {
        const auto normalized = std::filesystem::path(file).lexically_normal().generic_string();
        if (normalized == name || (!requested.is_absolute() && normalized.ends_with("/" + name))) {
          if (!selected.empty() && selected != file) throw std::invalid_argument("Ambiguous file path; use the full source path");
          selected = file;
        }
      }
      if (selected.empty()) throw std::invalid_argument("File is not among the configured project sources");
      const auto lineNumber = [&](const char *key, size_t fallback) -> size_t {
        const auto value = args.find(key);
        if (value == args.end()) return fallback;
        if (!value->is_number_integer() || *value < 1 || *value > 10000000)
          throw std::invalid_argument("Line numbers must be integers between 1 and 10000000");
        return value->get<size_t>();
      };
      const auto first = lineNumber("start_line", 1);
      const auto requestedLast = lineNumber("end_line", first + 199);
      if (requestedLast < first) throw std::invalid_argument("end_line must be at least start_line");
      const auto last = (std::min)(requestedLast, first + 199);
      constexpr size_t maxFileBytes = 8 * 1024 * 1024;
      std::ifstream file(selected, std::ios::binary);
      if (!file) throw std::runtime_error("Unable to read the source file");
      std::string content(maxFileBytes + 1, '\0');
      file.read(content.data(), static_cast<std::streamsize>(content.size()));
      content.resize(static_cast<size_t>(file.gcount()));
      if (content.size() > maxFileBytes) throw std::invalid_argument("read_file supports files up to 8 MiB");
      if (file.bad()) throw std::runtime_error("Failed reading the source file");
      if (content.find('\0') != std::string::npos) throw std::invalid_argument("read_file supports text files only");
      json result = {{"source", selected}, {"start_line", first}, {"end_line", 0}, {"content", ""}, {"truncated", false}};
      if (result.dump().size() > kMaxResultBytes - 128) throw std::invalid_argument("Source path exceeds the result size limit");
      if (content.empty()) result["start_line"] = 0;
      size_t pos = 0, line = 1;
      while (line < first && pos < content.size()) {
        const auto newline = content.find('\n', pos);
        pos = newline == std::string::npos ? content.size() : newline + 1;
        ++line;
      }
      if (pos == content.size() && first > 1) throw std::invalid_argument("start_line exceeds the file length");
      std::string snippet;
      while (pos < content.size() && line <= last) {
        const auto newline = content.find('\n', pos);
        const auto end = newline == std::string::npos ? content.size() : newline;
        auto text = content.substr(pos, end - pos);
        if (!text.empty() && text.back() == '\r') text.pop_back();
        const std::string valid{ utils::sanitizeUtf8(text) };
        auto numbered = std::to_string(line) + ": " + valid + "\n";
        result["content"] = snippet + numbered;
        result["end_line"] = line;
        if (result.dump().size() > kMaxResultBytes - 128) {
          result["truncated"] = true;
          if (!snippet.empty()) {
            result["content"] = snippet;
            result["end_line"] = line - 1;
            result["next_start_line"] = line;
          } else {
            // A single long line cannot fit even by itself. Return a marked prefix.
            result["line_truncated"] = true;
            result["next_start_line"] = line + 1;
            do {
              auto length = numbered.size() / 2;
              while (length && (static_cast<unsigned char>(numbered[length]) & 0xc0) == 0x80) --length;
              numbered.resize(length);
              result["content"] = numbered;
            } while (result.dump().size() > kMaxResultBytes);
          }
          break;
        }
        snippet += numbered;
        pos = newline == std::string::npos ? content.size() : newline + 1;
        ++line;
      }
      if (!result["truncated"].get<bool>() && pos < content.size()) {
        result["truncated"] = true;
        result["next_start_line"] = line;
      }
      if (std::find(ctx.usedSources.begin(), ctx.usedSources.end(), selected) == ctx.usedSources.end()) ctx.usedSources.push_back(selected);
      return result;
    } catch (const std::exception &e) {
      return {{"error", preview(e.what(), 512)}};
    }
  }

  nlohmann::json executeListFiles(const nlohmann::json &call, const ToolContext &ctx)
  {
    LOG_MSG_DEBUG << "executeListFiles: " << call.dump();
    try {
      const auto &function = call.at("function");
      if (function.at("name") != "list_files") throw std::invalid_argument("Unknown tool");
      const auto arguments = function.at("arguments").get<std::string>();
      if (arguments.size() > 4096) throw std::invalid_argument("Tool arguments exceed the size limit");
      const auto args = json::parse(arguments);
      if (!args.is_object())
        return { { "error", "arguments must be an object" } };

      for (auto it = args.begin(); it != args.end(); ++it) {
        if (it.key() != "path_filter" && it.key() != "extension")
          return { { "error", "unknown argument: " + it.key() } };
      }

      // validate path_filter (same rules as grep/read_file)
      std::string filter;
      if (args.contains("path_filter")) {
        if (!args["path_filter"].is_string())
          return { { "error", "path_filter must be a string" } };
        filter = args["path_filter"].get<std::string>();
        if (filter.empty() || filter.size() > kMaxArgLength)
          return { { "error", "path_filter must be 1.." + std::to_string(kMaxArgLength) + " bytes" } };
        if (filter.find('\0') != std::string::npos || filter.find('\n') != std::string::npos || filter.find('\r') != std::string::npos)
          return { { "error", "path_filter contains invalid characters" } };
        std::replace(filter.begin(), filter.end(), '\\', '/');
        if (filter.find("..") != std::string::npos)
          return { { "error", "path_filter must not contain '..'" } };
        // normalize leading './'
        while (filter.rfind("./", 0) == 0) filter = filter.substr(2);
        if (filter == ".") filter.clear();
      }

      // validate extension
      std::string ext;
      if (args.contains("extension")) {
        if (!args["extension"].is_string())
          return { { "error", "extension must be a string" } };
        ext = args["extension"].get<std::string>();
        if (ext.size() > 16 || ext.find('/') != std::string::npos || ext.find('\\') != std::string::npos)
          return { { "error", "extension is invalid" } };
        if (!ext.empty() && ext[0] != '.')
          ext = "." + ext; // be forgiving: 'cpp' -> '.cpp'
        std::transform(ext.begin(), ext.end(), ext.begin(),
          [](unsigned char c) { return std::tolower(c); });
      }

      json result = {{"files", json::array()}, {"count", 0}, {"truncated", false}};
      auto &files = result["files"];
      constexpr size_t kMaxListedFiles = 200;
      for (const std::string &path : ctx.allowedFiles) {
        std::string rel = path;
        std::replace(rel.begin(), rel.end(), '\\', '/');
        if (!filter.empty()) {
          // same heuristic as grep: match '/filter' anywhere or exact prefix
          if (rel != filter &&
            rel.rfind(filter, 0) != 0 &&
            rel.find("/" + filter) == std::string::npos)
            continue;
        }
        if (!ext.empty()) {
          if (rel.size() < ext.size()) continue;
          std::string tail = rel.substr(rel.size() - ext.size());
          std::transform(tail.begin(), tail.end(), tail.begin(),
            [](unsigned char c) { return std::tolower(c); });
          if (tail != ext) continue;
        }
        if (files.size() >= kMaxListedFiles) {
          result["truncated"] = true;
          break;
        }
        files.push_back(rel);
        result["count"] = files.size();
        if (result.dump().size() > kMaxResultBytes) {
          files.erase(files.end() - 1);
          result["count"] = files.size();
          result["truncated"] = true;
          break;
        }
      }

      // Distinguish "no match" from "error": empty list is a valid result here
      return result;
    } catch (const std::exception &e) {
      return {{"error", preview(e.what(), 512)}};
    }
  }

  json executeGrep(const json &call, const ToolContext &ctx, const Search &search)
  {
    LOG_MSG_DEBUG << "executeGrep: " << call.dump();
    try {
      const auto &function = call.at("function");
      if (function.at("name") != "grep") throw std::invalid_argument("Unknown tool; available tools are grep, read_file, list_files and search_context");
      const auto arguments = function.at("arguments").get<std::string>();
      if (arguments.size() > 4096) throw std::invalid_argument("Tool arguments exceed the size limit");
      const auto args = json::parse(arguments);
      if (!args.is_object()) throw std::invalid_argument("Tool arguments must be an object");
      for (const auto &[key, value] : args.items())
        if (key != "pattern" && key != "path_filter") throw std::invalid_argument("Unknown grep argument: " + key);
      const auto pattern = args.at("pattern").get<std::string>();
      if (pattern.size() < 2 || pattern.size() > 64 || pattern.find_first_not_of(" \t\r\n") == std::string::npos ||
          pattern.find('\0') != std::string::npos || pattern.find_first_of("\r\n") != std::string::npos)
        throw std::invalid_argument("pattern must be single-line literal text, 2 to 64 bytes");
      auto filter = args.value("path_filter", std::string{});
      if (filter.size() > 1024 || filter.find('\0') != std::string::npos) throw std::invalid_argument("Invalid path_filter");
      std::replace(filter.begin(), filter.end(), '\\', '/');
      for (const auto &part : std::filesystem::path(filter))
        if (part == "..") throw std::invalid_argument("path_filter must not contain parent traversal");
      filter = std::filesystem::path(filter).lexically_normal().generic_string();
      if (filter == ".") filter.clear();
      std::vector<std::string> paths;
      for (const auto &file : ctx.allowedFiles) {
        const auto path = std::filesystem::path(file).lexically_normal().generic_string();
        // Relative prefixes may start at any directory boundary in configured absolute paths.
        const bool relative = !std::filesystem::path(filter).is_absolute();
        if (filter.empty() || path.starts_with(filter) || (relative && path.find("/" + filter) != std::string::npos))
          paths.push_back(file);
      }
      json result = {{"matches", json::array()}, {"truncated", false}};
      // SourceProcessor interprets an empty list as "all sources". Never pass it through.
      if (paths.empty()) return result;
      for (const auto &hit : search(pattern, paths)) {
        auto content = hit.content;
        bool shortened = content.size() > 2048;
        if (shortened) {
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
      return result;
    } catch (const std::exception &e) {
      return {{"error", preview(e.what(), 512)}};
    }
  }

  bool fitToolResults(json &messages, size_t budget, const inference_contant::TokenCounter &count)
  {
    auto fits = [&] { return inference_contant::estimateRequestTokens(messages, json::array(), count) <= budget; };
    for (auto &message : messages) {
      if (fits()) return true;
      if (message.value("role", "") != "tool" || !message.contains("content") || !message["content"].is_string()) continue;
      const auto original = message["content"].get<std::string>();
      size_t length = original.size();
      do {
        length /= 2;
        while (length && (static_cast<unsigned char>(original[length]) & 0xc0) == 0x80) --length;
        message["content"] = json({{"truncated", true}, {"notice", "Tool result shortened to fit the final answer context"},
          {"content", original.substr(0, length)}}).dump();
      } while (!fits() && length);
    }
    return fits();
  }

  Turn run(json messages, const Generate &generate,
    const std::function<json(const json &)> &execute, const std::function<bool()> &connected, size_t maxSteps, const Status &onStatus)
  {
    Turn result;
    for (size_t step = 0; step < maxSteps && connected(); ++step) {
      auto turn = generate(messages);
      if (!connected()) return result;
      if (turn.incomplete || !turn.assistant.is_object()) {
        turn.incomplete = true;
        turn.text.clear(); // A truncated tool request can contain a preamble, not a final answer.
        turn.assistant = nullptr;
        return turn;
      }
      const auto calls = turn.assistant.find("tool_calls");
      if (calls == turn.assistant.end() || (calls->is_array() && calls->empty())) {
        return turn;
      }
      if (!calls->is_array() || calls->size() > kMaxCallsPerStep) throw std::runtime_error("Invalid or excessive tool calls");
      std::set<std::string> ids;
      for (const auto &call : *calls) {
        const auto id = call.at("id").get<std::string>();
        if (id.empty() || id.size() > 128 || !ids.insert(id).second || call.value("type", "") != "function")
          throw std::runtime_error("Invalid tool call identity");
      }
      if (step + 1 == maxSteps) { result.limitReached = true; return result; }
      messages.push_back(turn.assistant);
      for (const auto &call : *calls) {
        if (!connected()) return result;
        auto name = call.value("function", json::object()).value("name", std::string("tool"));
        if (onStatus) {
          std::string description = preview(name, 64);
          try {
            const auto &raw = call.at("function").at("arguments").get_ref<const std::string &>();
            const auto args = raw.size() <= 4096 ? json::parse(raw) : json::object();
            const auto key = name == "grep" ? "pattern" : name == "list_files" ? "path_filter" : name == "search_context" ? "query" : "path";
            if (args.contains(key) && args[key].is_string()) description += "(" + preview(args[key].get<std::string>(), 256) + ")";
            if (name == "list_files" && args.contains("extension") && args["extension"].is_string())
              description += " extension=" + preview(args["extension"].get<std::string>(), 16);
          } catch (const std::exception &) { /* Executor reports invalid arguments below. */ }
          std::replace(description.begin(), description.end(), '\n', ' ');
          std::replace(description.begin(), description.end(), '\r', ' ');
          onStatus("Step " + std::to_string(step + 1) + "/" + std::to_string(maxSteps) + ": " + description);
        }
        if (!connected()) return result;
        const auto output = execute(call);
        if (onStatus) {
          if (output.contains("error")) onStatus(name + " failed: " + output.at("error").get<std::string>());
          else if (name == "grep") onStatus("grep: " + std::to_string(output.at("matches").size()) + " snippets returned" +
            (output.value("truncated", false) ? " (truncated)" : ""));
          else if (name == "list_files") onStatus("list_files: " + std::to_string(output.at("count").get<size_t>()) + " files returned" +
            (output.value("truncated", false) ? " (truncated)" : ""));
          else if (name == "search_context") onStatus("search_context: " + std::to_string(output.at("matches").size()) +
            " excerpts returned (" + output.at("mode").get<std::string>() + ")" + (output.value("truncated", false) ? " (truncated)" : ""));
          else if (name == "read_file") onStatus("read_file: returned lines " + std::to_string(output.at("start_line").get<size_t>()) +
            "-" + std::to_string(output.at("end_line").get<size_t>()) + (output.value("truncated", false) ? " (truncated)" : ""));
        }
        messages.push_back({{"role", "tool"}, {"tool_call_id", call.at("id")}, {"content", output.dump()}});
      }
    }
    return result;
  }
} // namespace chat_tools
