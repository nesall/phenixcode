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

    std::string pathFilter(const json &args) {
      if (!args.contains("path_filter")) return {};
      if (!args["path_filter"].is_string()) throw std::invalid_argument("path_filter must be a string");
      auto filter = args["path_filter"].get<std::string>();
      if (filter.size() > kMaxPathFilterBytes)
        throw std::invalid_argument("path_filter must be at most " + std::to_string(kMaxPathFilterBytes) + " bytes");
      if (filter.find('\0') != std::string::npos || filter.find_first_of("\r\n") != std::string::npos)
        throw std::invalid_argument("path_filter contains invalid characters");
      std::replace(filter.begin(), filter.end(), '\\', '/');
      for (const auto &part : std::filesystem::path(filter))
        if (part == "..") throw std::invalid_argument("path_filter must not contain parent traversal");
      filter = std::filesystem::path(filter).lexically_normal().generic_string();
      return filter == "." ? std::string{} : filter;
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
        {"path_filter", {{"type", "string"}, {"description", "Optional path prefix, up to " + std::to_string(kMaxPathFilterBytes) + " UTF-8 bytes, e.g. src/; empty means no filter. Only configured project files are searched"}}}
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
              { "description", "Optional path prefix, up to " + std::to_string(kMaxPathFilterBytes) + " UTF-8 bytes, e.g. 'src'; empty means no filter. Must be inside project sources." }
            } },
            { "extension",{
              { "type", "string" },
              { "description", "Optional file extension filter, e.g. '.cpp'; up to 16 bytes including the dot." }
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
  namespace {
    const json &projectSections() {
      static const json sections = json::array({"project", "models", "retrieval", "reasoning", "all"});
      return sections;
    }

    json projectArguments(const json &call, const char *name, bool sections) {
      if (!call.contains("function") || !call["function"].is_object())
        throw std::invalid_argument("Tool call requires a function object");
      const auto &function = call["function"];
      if (!function.contains("name") || function["name"] != name) throw std::invalid_argument("Unknown tool");
      if (!function.contains("arguments") || !function["arguments"].is_string())
        throw std::invalid_argument("Tool arguments must be a JSON string");
      const auto &raw = function["arguments"].get_ref<const std::string &>();
      if (raw.size() > 4096) throw std::invalid_argument("Tool arguments exceed the size limit");
      const auto args = json::parse(raw, nullptr, false);
      if (args.is_discarded()) throw std::invalid_argument("Malformed JSON in tool arguments");
      if (!args.is_object()) throw std::invalid_argument("Tool arguments must be an object");
      for (const auto &[key, value] : args.items())
        if (!sections || key != "section") throw std::invalid_argument("Unknown argument: " + preview(key, 128));
      if (sections && args.contains("section")) {
        if (!args["section"].is_string()) throw std::invalid_argument("section must be a string");
        if (std::find(projectSections().begin(), projectSections().end(), args["section"]) == projectSections().end())
          throw std::invalid_argument("Unknown section; use project, models, retrieval, reasoning or all");
      }
      return args;
    }

    // Bound serialized output, including escaping, without returning partial JSON.
    json boundedProjectResult(json result) {
      result["truncated"] = false;
      result["truncated_fields"] = json::array();
      while (result.dump().size() > kMaxResultBytes) {
        json *largest = nullptr;
        size_t largestSize = 0;
        std::string largestPath;
        const auto visit = [&](const auto &self, json &value, const std::string &path) -> void {
          if (value.is_array() && !value.empty()) {
            const auto size = value.dump().size();
            if (size > largestSize) { largest = &value; largestSize = size; largestPath = path; }
          } else if (value.is_string() && value.get_ref<const std::string &>().size() > 128) {
            const auto size = value.dump().size();
            if (size > largestSize) { largest = &value; largestSize = size; largestPath = path; }
          } else if (value.is_object()) {
            for (auto &[key, child] : value.items()) self(self, child, preview(path.empty() ? key : path + "." + key, 128));
          }
        };
        for (auto &[key, value] : result.items())
          if (key != "truncated" && key != "truncated_fields") visit(visit, value, key);
        if (!largest) return {{"error", "Project summary exceeds the result limit"}};
        if (largest->is_array()) largest->erase(largest->end() - 1);
        else *largest = preview(utils::sanitizeUtf8(largest->get<std::string>()), 128);
        result["truncated"] = true;
        auto &fields = result["truncated_fields"];
        if (std::find(fields.begin(), fields.end(), largestPath) == fields.end()) {
          if (fields.size() < 15) fields.push_back(largestPath);
          else if (fields.size() == 15) fields.push_back("*"); // Additional shortened fields.
        }
      }
      return result;
    }

    json projectSummary(const Settings &settings) {
      json paths = json::array();
      for (const auto &source : settings.sources()) {
        // URLs and headers may contain credentials. Do not expose either.
        json item = {{"type", source.type}};
        if (source.type == "directory" || source.type == "file") item["path"] = source.path;
        if (source.type == "directory") {
          item["recursive"] = source.recursive;
          item["extensions"] = source.extensions;
          item["exclude"] = source.exclude;
        }
        paths.push_back(std::move(item));
      }
      return {{"id", settings.getProjectId()}, {"title", settings.getProjectTitle()}, {"sources", paths},
        {"encoding", settings.filesEncoding()}, {"max_file_size_mb", settings.filesMaxFileSizeMb()}};
    }

    json providerSummary(const ApiConfig &api) {
      // Explicit allowlist: never serialize ApiConfig or catalog objects wholesale.
      return {{"id", api.id}, {"model", api.model},
        {"api_style", api.apiStyle == ApiConfig::ApiStyle::OpenAI ? "openai" : "anthropic"},
        {"context_length", api.contextLength}, {"max_tokens", api.maxTokens}, {"supports_tools", api.supportsTools},
        {"stream", api.stream}};
    }
  }

  json projectInfoDefinition() {
    return {{"type", "function"}, {"function", {
      {"name", "project_info"},
      {"description", "Summarize the active project's identity, configured source roots, current local source count, language extensions, indexed chunk count and tool capabilities. Extensions are lowercase with leading dots; extensionless files are counted separately. Index freshness is unknown. Arrays or strings may be shortened; truncated_fields identifies affected fields (* means additional fields)."},
      {"parameters", {{"type", "object"}, {"properties", json::object()}, {"additionalProperties", false}}}
    }}};
  }

  json projectSettingsDefinition() {
    return {{"type", "function"}, {"function", {
      {"name", "project_settings"},
      {"description", "Inspect a filtered view of loaded project/provider settings, not files on disk. Credentials, URLs and prompts are omitted. Includes the actual generation provider selected for this request. Database locations are filenames only. Reasoning choices are configured defaults, not request selections. Arrays or strings may be shortened; truncated_fields identifies affected fields (* means additional fields)."},
      {"parameters", {{"type", "object"}, {"properties", {
        {"section", {{"type", "string"}, {"enum", projectSections()}, {"description", "Section to inspect; defaults to all"}}}
      }}, {"additionalProperties", false}}}
    }}};
  }

  json executeProjectInfo(const json &call, const Settings &settings, const ToolContext &ctx, size_t indexedChunks, const std::vector<std::string> &availableTools) {
    try { projectArguments(call, "project_info", false); }
    catch (const std::invalid_argument &e) { return {{"error", preview(e.what(), 512)}}; }
    try {
      std::set<std::string> extensions;
      size_t extensionless = 0;
      for (const auto &file : ctx.allowedFiles) {
        auto extension = std::filesystem::path(file).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension.empty()) ++extensionless;
        else extensions.insert(std::move(extension));
      }
      return boundedProjectResult({{"configuration", "loaded"}, {"project", projectSummary(settings)},
        {"local_source_count", ctx.allowedFiles.size()}, {"source_extensions", extensions}, {"extensionless_file_count", extensionless},
        {"index", {{"chunk_count", indexedChunks}, {"freshness", "unknown"}}},
        {"capabilities", {{"agent_enabled", settings.providers().agentEnabled()},
          {"available_tools", availableTools}}}});
    } catch (const std::exception &e) {
      // Validation errors above are actionable; internal failures may contain
      // sensitive configuration values, so keep their details in backend logs.
      LOG_MSG_WARN << "project_info summary failed: " << e.what();
      return {{"error", "Unable to summarize loaded project; see backend logs"}};
    }
  }

  json executeProjectSettings(const json &call, const Settings &settings, const ApiConfig &selectedApi) {
    json args;
    try { args = projectArguments(call, "project_settings", true); }
    catch (const std::invalid_argument &e) { return {{"error", preview(e.what(), 512)}}; }
    try {
      const auto section = args.value("section", std::string("all"));
      json result = {{"configuration", "loaded"}, {"section", section}};
      if (section == "all" || section == "project") result["project"] = projectSummary(settings);
      if (section == "all" || section == "models") {
        json providers = json::array();
        // Only generation providers enabled for this project belong in this summary.
        const auto enabled = settings.enabledGenerationProviders();
        for (const auto &api : settings.providers().generationProviders())
          if (std::find(enabled.begin(), enabled.end(), api.id) != enabled.end())
            providers.push_back(providerSummary(api));
        const auto router = settings.autoRouterConfig();
        json rules = json::object();
        for (const auto &[tier, rule] : router.rules)
          rules[tier] = {{"strategy", rule.strategy}, {"direct_model_id", rule.directModelId},
            {"draft_model_ids", rule.draftModelIds}, {"synthesizer_model_id", rule.synthesizerModelId}};
        const auto embedding = settings.embeddingCurrentApi();
        result["models"] = {{"configured_generation_id", settings.generationCurrentApiId()},
          {"selected_generation", providerSummary(selectedApi)}, {"embedding", embedding.id.empty() ? json(nullptr) : providerSummary(embedding)},
          {"enabled_generation", providers}, {"agent_enabled", settings.providers().agentEnabled()},
          {"default_max_tokens", settings.generationDefaultMaxTokens()},
          {"default_temperature", settings.generationDefaultTemperature()},
          {"auto_router", {{"enabled", router.enabled}, {"classifier_type", router.classifier.type},
            {"fallback_model_id", router.fallbackModelId}, {"rules", rules}}}};
      }
      if (section == "all" || section == "retrieval") result["retrieval"] = {
        {"chunking", {{"min_tokens", settings.chunkingMinTokens()}, {"max_tokens", settings.chunkingMaxTokens()},
          {"overlap", settings.chunkingOverlap()}, {"semantic", settings.chunkingSemantic()}}},
        {"embedding_top_k", settings.embeddingTopK()}, {"embedding_batch_size", settings.embeddingBatchSize()},
        {"max_chunks", settings.generationMaxChunks()}, {"max_full_sources", settings.generationMaxFullSources()},
        {"max_related_per_source", settings.generationMaxRelatedPerSource()},
        {"database", {{"sqlite_filename", std::filesystem::path(settings.databaseSqlitePath()).filename().string()}, {"index_filename", std::filesystem::path(settings.databaseIndexPath()).filename().string()},
          {"vector_dim", settings.databaseVectorDim()}, {"distance_metric", settings.databaseDistanceMetric()}}}};
      if (section == "all" || section == "reasoning") {
        json parameters = json::array();
        for (const auto &param : selectedApi.reasoning) parameters.push_back({{"name", param.name},
          {"value_type", param.valueType}, {"value_field", param.valueField}, {"values", param.values}, {"default", param.defaultValue}});
        result["reasoning"] = {{"provider_id", selectedApi.id}, {"parameters", parameters},
          {"history_replay", selectedApi.historyReplay}, {"replay_storage", "backend_session_memory"},
          {"parameter_values", "configured_defaults_not_request_selections"}};
      }
      return boundedProjectResult(std::move(result));
    } catch (const std::exception &e) {
      // Do not forward internal configuration exception text to the provider.
      LOG_MSG_WARN << "project_settings summary failed: " << e.what();
      return {{"error", "Unable to summarize loaded settings; see backend logs"}};
    }
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
        throw std::invalid_argument("arguments must be an object");

      for (auto it = args.begin(); it != args.end(); ++it) {
        if (it.key() != "path_filter" && it.key() != "extension")
          throw std::invalid_argument("unknown argument: " + preview(it.key(), 128));
      }

      const auto filter = pathFilter(args);

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
      auto &files = result["files"];
      constexpr size_t kMaxListedFiles = 200;
      for (const std::string &path : ctx.allowedFiles) {
        std::string rel = path;
        std::replace(rel.begin(), rel.end(), '\\', '/');
        if (!filter.empty()) {
          const auto normalized = std::filesystem::path(rel).lexically_normal().generic_string();
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
      const auto filter = pathFilter(args);
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
            const auto key = name == "grep" ? "pattern" : name == "list_files" ? "path_filter" : name == "search_context" ? "query" : name == "project_settings" ? "section" : "path";
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
          else if (name == "project_info" || name == "project_settings") onStatus(name + ": loaded summary returned" +
            (output.value("truncated", false) ? " (truncated)" : ""));
          else if (name == "git_diff" || name == "git_log" || name == "git_show") onStatus(name + ": inspection returned" +
            (output.value("truncated", false) ? " (truncated)" : ""));
          else if (name == "read_file") onStatus("read_file: returned lines " + std::to_string(output.at("start_line").get<size_t>()) +
            "-" + std::to_string(output.at("end_line").get<size_t>()) + (output.value("truncated", false) ? " (truncated)" : ""));
        }
        messages.push_back({{"role", "tool"}, {"tool_call_id", call.at("id")}, {"content", output.dump()}});
      }
    }
    return result;
  }
} // namespace chat_tools
