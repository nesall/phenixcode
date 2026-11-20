#include "chat_tools.h"
#include "cutils.h"
#include <utils_log/logger.hpp>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

using nlohmann::json;

namespace chat_tools {
  namespace detail {
    std::string resolveAllowedFile(std::string path, const std::vector<std::string> &allowedFiles) {
      if (path.empty() || path.size() > 1024 || path.find('\0') != std::string::npos)
        throw std::invalid_argument("Invalid file path");
      std::replace(path.begin(), path.end(), '\\', '/');
      for (const auto &part : std::filesystem::path(path))
        if (part == "..") throw std::invalid_argument("File path must not contain parent traversal");
      const auto requested = std::filesystem::path(path).lexically_normal();
      const auto name = requested.generic_string();
      std::string selected;
      for (const auto &file : allowedFiles) {
        const auto normalized = std::filesystem::path(file).lexically_normal().generic_string();
        if (normalized == name || (!requested.is_absolute() && normalized.ends_with("/" + name))) {
          if (!selected.empty() && selected != file)
            throw std::invalid_argument("Ambiguous file path; use the full source path");
          selected = file;
        }
      }
      if (selected.empty()) throw std::invalid_argument("File is not among the configured project sources");
      return selected;
    }

    std::string readTextFileChecked(const std::string &selected) {
      constexpr size_t maxFileBytes = 8 * 1024 * 1024;
      std::ifstream file(selected, std::ios::binary);
      if (!file) throw std::runtime_error("Unable to read the source file");
      std::string content(maxFileBytes + 1, '\0');
      file.read(content.data(), static_cast<std::streamsize>(content.size()));
      content.resize(static_cast<size_t>(file.gcount()));
      if (content.size() > maxFileBytes) throw std::invalid_argument("read_file supports files up to 8 MiB");
      if (file.bad()) throw std::runtime_error("Failed reading the source file");
      if (content.find('\0') != std::string::npos) throw std::invalid_argument("read_file supports text files only");
      return content;
    }
  } // namespace detail

  namespace {
    std::pair<size_t, size_t> parseLineRange(const json &args) {
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
      return {first, (std::min)(requestedLast, first + 199)};
    }

    json formatFileRange(const std::string &selected, const std::string &content, size_t first, size_t last, size_t maxJsonBytes) {
      json result = {{"source", selected}, {"start_line", first}, {"end_line", 0}, {"content", ""}, {"truncated", false}};
      if (result.dump().size() > maxJsonBytes - 128) throw std::invalid_argument("Source path exceeds the result size limit");
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
        if (result.dump().size() > maxJsonBytes - 128) {
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
            } while (result.dump().size() > maxJsonBytes);
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
      return result;
    }

    bool dropTrailingLines(json &item, size_t targetDropBytes) {
      if (!item.is_object() || item.contains("error") || !item.contains("content") || !item["content"].is_string())
        return false;
      auto content = item["content"].get<std::string>();
      size_t endLine = item.value("end_line", size_t(0));
      size_t dropped = 0;
      bool changed = false;
      while (dropped < targetDropBytes && content.size() > 1) {
        const size_t withoutTrailingNl = content.back() == '\n' ? content.size() - 1 : content.size();
        const size_t prevNl = content.rfind('\n', withoutTrailingNl == 0 ? 0 : withoutTrailingNl - 1);
        if (prevNl == std::string::npos || prevNl + 1 >= content.size() || endLine <= item.value("start_line", size_t(1)))
          break;
        dropped += content.size() - (prevNl + 1);
        content.resize(prevNl + 1);
        --endLine;
        changed = true;
      }
      if (!changed) return false;
      item["content"] = std::move(content);
      item["end_line"] = endLine;
      item["next_start_line"] = endLine + 1;
      item["truncated"] = true;
      return true;
    }
  } // namespace

  json executeReadFile(const json &call, const ToolContext &ctx)
  {
    LOG_MSG_DEBUG << "executeReadFile: " << call.dump();
    LOG_MSG_INFO << "[tool] read_file called";
    try {
      const auto &function = call.at("function");
      if (function.at("name") != "read_file") throw std::invalid_argument("Unknown tool");
      const auto arguments = function.at("arguments").get<std::string>();
      if (arguments.size() > 4096) throw std::invalid_argument("Tool arguments exceed the size limit");
      const auto args = json::parse(arguments);
      if (!args.is_object()) throw std::invalid_argument("Tool arguments must be an object");
      for (const auto &[key, value] : args.items())
        if (key != "path" && key != "start_line" && key != "end_line") throw std::invalid_argument("Unknown read_file argument: " + key);
      const auto selected = detail::resolveAllowedFile(args.at("path").get<std::string>(), ctx.allowedFiles);
      const auto [first, last] = parseLineRange(args);
      const auto content = detail::readTextFileChecked(selected);
      auto result = formatFileRange(selected, content, first, last, kMaxResultBytes);
      if (std::find(ctx.usedSources.begin(), ctx.usedSources.end(), selected) == ctx.usedSources.end()) ctx.usedSources.push_back(selected);
      return result;
    } catch (const std::exception &e) {
      return {{"error", detail::preview(e.what(), 512)}};
    }
  }

  json executeReadFiles(const json &call, const ToolContext &ctx)
  {
    LOG_MSG_DEBUG << "executeReadFiles: " << call.dump();
    LOG_MSG_INFO << "[tool] read_files called";
    try {
      const auto &function = call.at("function");
      if (function.at("name") != "read_files") throw std::invalid_argument("Unknown tool");
      const auto arguments = function.at("arguments").get<std::string>();
      if (arguments.size() > 4096) throw std::invalid_argument("Tool arguments exceed the size limit");
      const auto args = json::parse(arguments);
      if (!args.is_object()) throw std::invalid_argument("Tool arguments must be an object");
      for (const auto &[key, value] : args.items())
        if (key != "files") throw std::invalid_argument("Unknown read_files argument: " + detail::preview(key, 128));
      if (!args.contains("files") || !args["files"].is_array())
        throw std::invalid_argument("files must be an array of 1 to 5 read requests");
      const auto &requests = args["files"];
      if (requests.empty() || requests.size() > 5)
        throw std::invalid_argument("files must contain between 1 and 5 read requests");

      struct ParsedReq {
        std::string path;
        size_t first;
        size_t last;
      };
      std::vector<ParsedReq> parsed;
      parsed.reserve(requests.size());
      for (const auto &item : requests) {
        if (!item.is_object()) throw std::invalid_argument("Each entry in files must be an object");
        for (const auto &[key, value] : item.items())
          if (key != "path" && key != "start_line" && key != "end_line")
            throw std::invalid_argument("Unknown read_files item argument: " + detail::preview(key, 128));
        if (!item.contains("path") || !item["path"].is_string())
          throw std::invalid_argument("Each read_files entry requires a string path");
        const auto [first, last] = parseLineRange(item);
        parsed.push_back({item["path"].get<std::string>(), first, last});
      }

      std::unordered_map<std::string, std::string> fileCache;
      json formatted = json::array();
      size_t successCount = 0;
      size_t errorCount = 0;
      for (const auto &req : parsed) {
        try {
          const auto selected = detail::resolveAllowedFile(req.path, ctx.allowedFiles);
          auto cached = fileCache.find(selected);
          if (cached == fileCache.end()) {
            cached = fileCache.emplace(selected, detail::readTextFileChecked(selected)).first;
          }
          formatted.push_back(formatFileRange(selected, cached->second, req.first, req.last, kMaxResultBytes));
          ++successCount;
        } catch (const std::exception &e) {
          formatted.push_back({{"path", req.path}, {"error", detail::preview(e.what(), 512)}});
          ++errorCount;
        }
      }

      if (successCount == 0) {
        if (formatted.size() == 1) return {{"error", formatted[0]["error"]}};
        return {{"error", "All read_files requests failed"}, {"results", formatted}};
      }

      json result = {
        {"results", std::move(formatted)},
        {"count", parsed.size()},
        {"requested_count", parsed.size()},
        {"truncated", false}
      };
      if (errorCount > 0) result["partial_errors"] = true;

      size_t omitted = 0;
      for (size_t serializedSize = result.dump().size(); serializedSize > kMaxResultBytes; serializedSize = result.dump().size()) {
        auto &items = result["results"];
        const size_t excess = serializedSize - kMaxResultBytes;
        size_t bestIdx = items.size();
        size_t bestLen = 0;
        for (size_t i = 0; i < items.size(); ++i) {
          if (items[i].contains("error") || !items[i].contains("content")) continue;
          const auto &c = items[i]["content"].get_ref<const std::string &>();
          const auto nl = c.find('\n');
          if (nl != std::string::npos && nl + 1 < c.size() && c.size() > bestLen) {
            bestLen = c.size();
            bestIdx = i;
          }
        }
        if (bestIdx < items.size() && dropTrailingLines(items[bestIdx], (std::max)(size_t(1), excess / 2))) {
          result["truncated"] = true;
          continue;
        }
        // No multi-line content left to trim; shorten any oversized single line first.
        bestIdx = items.size();
        bestLen = 0;
        for (size_t i = 0; i < items.size(); ++i) {
          if (items[i].contains("error") || !items[i].contains("content")) continue;
          const auto &c = items[i]["content"].get_ref<const std::string &>();
          if (c.size() > 128 && c.size() > bestLen) {
            bestLen = c.size();
            bestIdx = i;
          }
        }
        if (bestIdx < items.size()) {
          auto c = items[bestIdx]["content"].get<std::string>();
          size_t length = c.size() / 2;
          while (length && (static_cast<unsigned char>(c[length]) & 0xc0) == 0x80) --length;
          c.resize(length);
          items[bestIdx]["content"] = std::move(c);
          items[bestIdx]["line_truncated"] = true;
          items[bestIdx]["truncated"] = true;
          items[bestIdx]["next_start_line"] = items[bestIdx].value("end_line", size_t(0)) + 1;
          result["truncated"] = true;
          continue;
        }
        if (items.empty()) return {{"error", "read_files output exceeds the result size limit"}};
        items.erase(items.end() - 1);
        ++omitted;
        result["count"] = items.size();
        result["omitted_count"] = omitted;
        result["truncated"] = true;
      }

      for (const auto &item : result["results"]) {
        if (!item.contains("error") && item.contains("source")) {
          const auto &src = item["source"].get_ref<const std::string &>();
          if (std::find(ctx.usedSources.begin(), ctx.usedSources.end(), src) == ctx.usedSources.end())
            ctx.usedSources.push_back(src);
        }
      }
      return result;
    } catch (const std::exception &e) {
      return {{"error", detail::preview(e.what(), 512)}};
    }
  }

  nlohmann::json executeListFiles(const nlohmann::json &call, const ToolContext &ctx, const FileMetadataReader &readMetadata)
  {
    LOG_MSG_DEBUG << "executeListFiles: " << call.dump();
    LOG_MSG_INFO << "[tool] list_files called";
    try {
      const auto &function = call.at("function");
      if (function.at("name") != "list_files") throw std::invalid_argument("Unknown tool");
      const auto arguments = function.at("arguments").get<std::string>();
      if (arguments.size() > 4096) throw std::invalid_argument("Tool arguments exceed the size limit");
      const auto args = json::parse(arguments);
      if (!args.is_object())
        throw std::invalid_argument("arguments must be an object");

      for (auto it = args.begin(); it != args.end(); ++it) {
        if (it.key() != "path_filter" && it.key() != "extension" && it.key() != "sort_by" && it.key() != "order" && it.key() != "limit") {
          std::string suggestion{ " | Did you mean path_filter, extension, sort_by, order or limit?" };
          if (utils::containsAny(it.key(), { "search", "filter", "pattern"})) suggestion = " | Did you mean path_filter?";
          else if (utils::containsAny(it.key(), { "sort" })) suggestion = " | Did you mean sort_by?";
          else if (utils::containsAny(it.key(), { "ordering" })) suggestion = " | Did you mean order?";
          else if (utils::containsAny(it.key(), { "maximum", "max", "count", "size" })) suggestion = " | Did you mean limit?";
          throw std::invalid_argument("unknown argument: " + detail::preview(it.key(), 128) + suggestion);
        }
      }

      const auto filter = detail::pathFilter(args);
      const auto sortBy = args.value("sort_by", std::string("path"));
      const auto order = args.value("order", std::string("asc"));
      if (sortBy != "path" && sortBy != "size" && sortBy != "lines") throw std::invalid_argument("sort_by must be path, size or lines");
      if (order != "asc" && order != "desc") throw std::invalid_argument("order must be asc or desc");
      size_t limit = 200;
      if (args.contains("limit")) {
        if (!args["limit"].is_number_integer() || args["limit"] < 1 || args["limit"] > 200)
          throw std::invalid_argument("limit must be an integer between 1 and 200");
        limit = args["limit"].get<size_t>();
      }
      const bool sorted = args.contains("sort_by") || args.contains("order");
      const bool withMetadata = sortBy == "size" || sortBy == "lines";

      // validate extension
      std::string ext;
      if (args.contains("extension")) {
        if (!args["extension"].is_string())
          throw std::invalid_argument("extension must be a string");
        ext = args["extension"].get<std::string>();
        if (ext.find('/') != std::string::npos || ext.find('\\') != std::string::npos)
          throw std::invalid_argument("extension is invalid");
        if (!ext.empty() && ext[0] != '.')
          ext = "." + ext; // be forgiving: 'cpp' -> '.cpp'
        if (ext.size() > 16) throw std::invalid_argument("extension is invalid");
        std::transform(ext.begin(), ext.end(), ext.begin(),
          [](unsigned char c) { return std::tolower(c); });
      }

      json result = {{"files", json::array()}, {"count", 0}, {"truncated", false}};
      struct Entry { std::string path; std::string key; const FileMetadata *metadata = nullptr; };
      std::vector<Entry> entries;
      const auto normalize = [](std::string path) {
        std::replace(path.begin(), path.end(), '\\', '/');
        const auto bytes = std::filesystem::path(std::u8string(path.begin(), path.end())).lexically_normal().generic_u8string();
        return std::string(bytes.begin(), bytes.end());
      };
      for (const std::string &path : ctx.allowedFiles) {
        std::string rel = path;
        std::replace(rel.begin(), rel.end(), '\\', '/');
        if (!filter.empty()) {
          const auto normalized = normalize(rel);
          // same heuristic as grep: match '/filter' anywhere or exact prefix
          if (normalized != filter &&
            normalized.rfind(filter, 0) != 0 &&
            normalized.find("/" + filter) == std::string::npos)
            continue;
        }
        if (!ext.empty()) {
          if (rel.size() < ext.size()) continue;
          std::string tail = rel.substr(rel.size() - ext.size());
          std::transform(tail.begin(), tail.end(), tail.begin(),
            [](unsigned char c) { return std::tolower(c); });
          if (tail != ext) continue;
        }
        entries.push_back({rel, normalize(rel)});
      }
      // Keep the snapshot alive while entries hold pointers into it. Only return
      // metadata for the current allowed scope, never the full tracked-file list.
      const auto metadata = withMetadata && !entries.empty() && readMetadata ? readMetadata() : std::vector<FileMetadata>{};
      if (withMetadata) {
        std::unordered_map<std::string, const FileMetadata *> byPath;
        for (const auto &file : metadata) byPath.emplace(normalize(file.path), &file);
        for (auto &entry : entries) {
          const auto found = byPath.find(entry.key);
          if (found != byPath.end()) entry.metadata = found->second;
        }
        result["file_metadata"] = json::array();
        result["metadata_source"] = "index";
        result["metadata_freshness"] = "unknown";
      }
      if (sorted) {
        std::stable_sort(entries.begin(), entries.end(), [&](const Entry &a, const Entry &b) {
          if (withMetadata) {
            if (bool(a.metadata) != bool(b.metadata)) return a.metadata != nullptr;
            if (a.metadata && b.metadata) {
              const auto av = sortBy == "size" ? a.metadata->fileSize : a.metadata->nofLines;
              const auto bv = sortBy == "size" ? b.metadata->fileSize : b.metadata->nofLines;
              if (av != bv) return order == "asc" ? av < bv : av > bv;
            }
            // Metric ties and unknown values always use ascending source paths.
            return a.key < b.key;
          }
          return order == "asc" ? a.key < b.key : a.key > b.key;
        });
      }
      auto &files = result["files"];
      for (const auto &entry : entries) {
        if (files.size() >= limit) { result["truncated"] = true; break; }
        files.push_back(entry.path);
        if (withMetadata) result["file_metadata"].push_back({{"source", entry.path},
          {"size_bytes", entry.metadata ? json(entry.metadata->fileSize) : json(nullptr)},
          {"line_count", entry.metadata ? json(entry.metadata->nofLines) : json(nullptr)}});
        result["count"] = files.size();
        if (result.dump().size() > kMaxResultBytes) {
          files.erase(files.end() - 1);
          if (withMetadata) result["file_metadata"].erase(result["file_metadata"].end() - 1);
          result["count"] = files.size();
          result["truncated"] = true;
          break;
        }
      }

      // Distinguish "no match" from "error": empty list is a valid result here
      return result;
    } catch (const std::exception &e) {
      return {{"error", detail::preview(e.what(), 512)}};
    }
  }
} // namespace chat_tools
