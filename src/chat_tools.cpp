#include "chat_tools.h"
#include "symbol_service.h"
#include "settings.h"
#include "cutils.h"
#include <utils_log/logger.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <fstream>
#include <memory>
#include <unordered_map>
#include <re2/re2.h>

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

    const std::string middle = " \xc2\xb7 ";
    const std::string dash = " \xe2\x80\x94 ";

    std::string shortPath(std::string value, const std::string &root) {
      std::replace(value.begin(), value.end(), '\\', '/');
      auto path = std::filesystem::path(std::u8string(value.begin(), value.end())).lexically_normal();
      if (path.is_absolute()) {
        const auto base = std::filesystem::path(std::u8string(root.begin(), root.end()));
        const auto relative = root.empty() ? std::filesystem::path{} : path.lexically_relative(base);
        if (!relative.empty() && *relative.begin() != "..") path = relative;
        else {
          // Multiple unrelated roots have no single project-relative spelling.
          path = path.parent_path().filename() / path.filename();
        }
      }
      const auto bytes = path.generic_u8string();
      value.assign(bytes.begin(), bytes.end());
      if (value.size() > 96) {
        size_t start = value.size() - 93;
        while (start < value.size() && (static_cast<unsigned char>(value[start]) & 0xc0) == 0x80) ++start;
        value = "..." + value.substr(start);
      }
      return preview(value, 96);
    }

    std::string compactAction(const std::string &name, const json &args, const std::string &root, const json *output = nullptr) {
      if (!args.is_object()) return preview(name, 64);
      const auto text = [&](const char *key) {
        const auto it = args.find(key);
        return it != args.end() && it->is_string() ? it->get<std::string>() : std::string{};
      };
      const auto quoted = [&](const char *key) { return json(preview(text(key), 64)).dump(); };
      const auto scoped = [&](std::string label) {
        const auto filter = text("path_filter");
        const auto display = shortPath(filter, root);
        return filter.empty() || display == "." ? label : label + " in " + display;
      };
      if (name == "grep") {
        auto label = scoped("Search " + quoted("pattern"));
        if (!text("glob").empty()) label += " (" + preview(text("glob"), 32) + ")";
        return label;
      }
      if (name == "search_context") return "Search context " + quoted("query");
      if (name == "list_files") {
        auto label = scoped("List " + (text("extension").empty() ? std::string{} : preview(text("extension"), 16) + " ") + "files");
        if (!text("sort_by").empty() || !text("order").empty()) label += middle + "by " +
          (text("sort_by").empty() ? "path" : preview(text("sort_by"), 16)) + " " + (text("order").empty() ? "asc" : preview(text("order"), 8));
        return label;
      }
      if (name == "read_file") {
        auto label = "Read " + shortPath(text("path"), root);
        if (output && !output->contains("error")) label += ":" + std::to_string(output->value("start_line", size_t(0))) + "-" + std::to_string(output->value("end_line", size_t(0)));
        return label;
      }
      if (name == "read_files") {
        const size_t n = args.contains("files") && args["files"].is_array() ? args["files"].size() : size_t(0);
        return "Read " + std::to_string(n) + (n == 1 ? " file" : " files");
      }
      if (name == "symbol_outline") {
        auto label = "Outline " + shortPath(text("path"), root);
        if (!text("kind").empty()) label += middle + preview(text("kind"), 24);
        if (!text("role").empty() && text("role") != "all") label += middle + preview(text("role"), 24);
        return label;
      }
      if (name == "read_symbol") {
        auto label = "Read symbol " + preview(text("symbol"), 48) + middle + shortPath(text("path"), root);
        if (output && !output->contains("error") && !output->value("ambiguous", false) && output->contains("start_line"))
          label += ":" + std::to_string(output->value("start_line", size_t(0))) + "-" + std::to_string(output->value("end_line", size_t(0)));
        return label;
      }
      if (name == "find_symbol") {
        auto label = "Find symbol " + preview(text("name"), 48);
        if (!text("path_filter").empty()) label += middle + preview(text("path_filter"), 32);
        return label;
      }
      if (name == "project_info") return "Project info";
      if (name == "project_settings") return "Project settings" + (text("section").empty() || text("section") == "all" ? std::string{} : middle + preview(text("section"), 32));
      std::string label = preview(name, 64);
      if (name == "git_diff") {
        label = "Git diff" + (text("base").empty() ? std::string{} : " vs " + preview(text("base"), 32));
        if (args.contains("staged") && args["staged"].is_boolean() && args["staged"].get<bool>()) label += " (staged)";
      }
      else if (name == "git_log") label = "Git history";
      else if (name == "git_show") label = "Git show " + preview(text("revision"), 32);
      else if (name == "git_blame") label = "Git blame";
      if (!text("path").empty()) label += middle + shortPath(text("path"), root);
      if (name == "git_blame" && output && !output->contains("error") && output->contains("start_line"))
        label += ":" + std::to_string(output->value("start_line", size_t(0))) + "-" + std::to_string(output->value("end_line", size_t(0)));
      return label;
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

    std::unique_ptr<re2::RE2> compileGlob(const json &args, std::string &normalizedGlob) {
      normalizedGlob.clear();
      if (!args.contains("glob")) return nullptr;
      if (!args["glob"].is_string()) throw std::invalid_argument("glob must be a string");
      auto glob = args["glob"].get<std::string>();
      if (glob.empty()) return nullptr;
      if (glob.size() > 256) throw std::invalid_argument("glob must be at most 256 bytes");
      if (glob.find('\0') != std::string::npos || glob.find_first_of("\r\n") != std::string::npos)
        throw std::invalid_argument("glob contains invalid characters");
      std::replace(glob.begin(), glob.end(), '\\', '/');
      for (const auto &part : std::filesystem::path(glob))
        if (part == "..") throw std::invalid_argument("glob must not contain parent traversal");
      while (glob.starts_with("./")) glob.erase(0, 2);
      if (glob.empty()) return nullptr;
      normalizedGlob = glob;

      auto escapeLiteral = [](char c, std::string &out) {
        switch (c) {
          case '.': case '+': case '(': case ')': case '[': case ']':
          case '^': case '$': case '|': case '\\':
            out.push_back('\\');
            [[fallthrough]];
          default:
            out.push_back(c);
        }
      };

      const bool hasSlash = glob.find('/') != std::string::npos;
      const bool anchoredRoot = hasSlash && (glob.front() == '/' || (glob.size() >= 2 && glob[1] == ':'));
      std::string pattern = anchoredRoot ? "^" : "(?:^|/)";
      for (size_t i = 0; i < glob.size(); ++i) {
        const char c = glob[i];
        if (c == '*') {
          if (i + 1 < glob.size() && glob[i + 1] == '*') {
            if (i + 2 < glob.size() && glob[i + 2] == '/') {
              pattern += "(?:.*/)?";
              i += 2;
            } else {
              pattern += ".*";
              ++i;
            }
          } else {
            pattern += "[^/]*";
          }
        } else if (c == '?') {
          pattern += "[^/]";
        } else if (c == '{') {
          const auto close = glob.find('}', i + 1);
          if (close == std::string::npos || close == i + 1)
            throw std::invalid_argument("glob contains invalid brace group");
          const auto group = glob.substr(i + 1, close - i - 1);
          if (group.find('{') != std::string::npos)
            throw std::invalid_argument("glob does not support nested brace groups");
          pattern += "(?:";
          size_t start = 0;
          bool first = true;
          while (start <= group.size()) {
            const auto comma = group.find(',', start);
            const auto alt = group.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (!first) pattern.push_back('|');
            first = false;
            for (const char ch : alt) {
              if (ch == '*') pattern += "[^/]*";
              else if (ch == '?') pattern += "[^/]";
              else escapeLiteral(ch, pattern);
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
          }
          pattern.push_back(')');
          i = close;
        } else if (c == '}') {
          throw std::invalid_argument("glob contains unmatched closing brace");
        } else {
          escapeLiteral(c, pattern);
        }
      }
      pattern.push_back('$');
      re2::RE2::Options opt;
      opt.set_case_sensitive(false);
      opt.set_log_errors(false);
      opt.set_max_mem(1 << 20);
      auto re = std::make_unique<re2::RE2>(pattern, opt);
      if (!re->ok()) throw std::invalid_argument("Invalid glob pattern");
      return re;
    }

    std::vector<std::string> filterAllowedFiles(const std::vector<std::string> &allowedFiles,
                                                const std::string &filter,
                                                const std::unique_ptr<re2::RE2> &globRe) {
      std::vector<std::string> paths;
      std::set<std::string> selected;
      const bool relative = !std::filesystem::path(filter).is_absolute();
      for (const auto &file : allowedFiles) {
        const auto path = std::filesystem::path(file).lexically_normal().generic_string();
        const bool prefixOk = filter.empty() || path.starts_with(filter) ||
          (relative && path.find("/" + filter) != std::string::npos);
        const bool globOk = !globRe || re2::RE2::PartialMatch(path, *globRe);
        if (prefixOk && globOk && selected.insert(path).second) {
          paths.push_back(file);
        }
      }
      return paths;
    }
  }

  bool enabled(bool agentEnabled, const ApiConfig &api, bool freeChat, bool attachedOnly)
  {
    return agentEnabled && !freeChat && !attachedOnly && api.supportsTools == "on" && api.apiStyle == ApiConfig::ApiStyle::OpenAI;
  }

  json grepDefinition()
  {
    json props;
    props["pattern"] = {{"type", "string"}, {"minLength", 2}, {"maxLength", kMaxGrepPatternBytes},
      {"description", "Text or RE2 regex"}};
    props["path_filter"] = {{"type", "string"}};
    props["glob"] = {{"type", "string"}, {"maxLength", 256},
      {"description", "File glob, e.g. '*.cpp'"}};
    props["regex"] = {{"type", "boolean"}};
    props["case_sensitive"] = {{"type", "boolean"}};
    props["whole_word"] = {{"type", "boolean"}};
    props["context_lines"] = {{"type", "integer"}, {"minimum", 0}, {"maximum", 10}};
    json params = {{"type", "object"}, {"properties", std::move(props)}, {"required", json::array({"pattern"})}, {"additionalProperties", false}};
    return {{"type", "function"}, {"function", {
      {"name", "grep"},
      {"description", "Search allowed files for literal text or RE2 regex with filters and context lines (0-10)."},
      {"parameters", std::move(params)}
    }}};
  }

  json readFileDefinition() {
    json props;
    props["path"] = {{"type", "string"}};
    props["start_line"] = {{"type", "integer"}, {"minimum", 1}};
    props["end_line"] = {{"type", "integer"}, {"minimum", 1}};
    json params = {{"type", "object"}, {"properties", std::move(props)}, {"required", json::array({"path"})}, {"additionalProperties", false}};
    return {{"type", "function"}, {"function", {
      {"name", "read_file"},
      {"description", "Read numbered lines from an allowed file (max 200 lines, 8 KiB)."},
      {"parameters", std::move(params)}
    }}};
  }

  json readFilesDefinition() {
    json itemProps;
    itemProps["path"] = {{"type", "string"}};
    itemProps["start_line"] = {{"type", "integer"}, {"minimum", 1}};
    itemProps["end_line"] = {{"type", "integer"}, {"minimum", 1}};
    json itemSchema = {{"type", "object"}, {"properties", std::move(itemProps)}, {"required", json::array({"path"})}, {"additionalProperties", false}};
    json filesSchema = {{"type", "array"}, {"minItems", 1}, {"maxItems", 5}, {"items", std::move(itemSchema)}};
    json props = {{"files", std::move(filesSchema)}};
    json params = {{"type", "object"}, {"properties", std::move(props)}, {"required", json::array({"files"})}, {"additionalProperties", false}};
    return {{"type", "function"}, {"function", {
      {"name", "read_files"},
      {"description", "Batch-read 1 to 5 line ranges from allowed files (8 KiB total)."},
      {"parameters", std::move(params)}
    }}};
  }

  json symbolOutlineDefinition() {
    static const json kinds = json::array({
      "namespace", "class", "struct", "union", "enum", "interface", "concept", "type_alias", "function", "method", "constructor", "destructor"
    });
    static const json roles = json::array({"all", "definition", "declaration"});
    json props;
    props["path"] = {{"type", "string"}};
    props["kind"] = {{"type", "string"}, {"enum", kinds}};
    props["role"] = {{"type", "string"}, {"enum", roles}};
    props["limit"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}};
    props["offset"] = {{"type", "integer"}, {"minimum", 0}};
    json params = {{"type", "object"}, {"properties", std::move(props)}, {"required", json::array({"path"})}, {"additionalProperties", false}};
    return {{"type", "function"}, {"function", {
      {"name", "symbol_outline"},
      {"description", "List symbol declarations and definitions in a supported file (C, C++, Py, JS, TS/TSX) with selectors."},
      {"parameters", std::move(params)}
    }}};
  }

  json readSymbolDefinition() {
    static const json roles = json::array({"any", "definition", "declaration"});
    json props;
    props["path"] = {{"type", "string"}};
    props["symbol"] = {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}, {"description", "Name, Class::m, Class.m, or selector"}};
    props["role"] = {{"type", "string"}, {"enum", roles}};
    json params = {{"type", "object"}, {"properties", std::move(props)}, {"required", json::array({"path", "symbol"})}, {"additionalProperties", false}};
    return {{"type", "function"}, {"function", {
      {"name", "read_symbol"},
      {"description", "Read a symbol or selector from a supported file."},
      {"parameters", std::move(params)}
    }}};
  }

  json findSymbolDefinition() {
    static const json kinds = json::array({
      "namespace", "class", "struct", "union", "enum", "interface", "concept", "type_alias", "function", "method", "constructor", "destructor"
    });
    static const json roles = json::array({"all", "definition", "declaration"});
    static const json matches = json::array({"exact", "prefix"});
    json props;
    props["name"] = {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}, {"description", "Symbol, Class::m, or Class.m"}};
    props["match"] = {{"type", "string"}, {"enum", matches}};
    props["role"] = {{"type", "string"}, {"enum", roles}};
    props["kind"] = {{"type", "string"}, {"enum", kinds}};
    props["path_filter"] = {{"type", "string"}};
    props["glob"] = {{"type", "string"}};
    props["limit"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 50}};
    json params = {{"type", "object"}, {"properties", std::move(props)}, {"required", json::array({"name"})}, {"additionalProperties", false}};
    return {{"type", "function"}, {"function", {
      {"name", "find_symbol"},
      {"description", "Find symbols across allowed project files."},
      {"parameters", std::move(params)}
    }}};
  }

  nlohmann::json listFilesDefinition()
  {
    json props;
    props["path_filter"] = {{"type", "string"}};
    props["extension"] = {{"type", "string"}};
    props["sort_by"] = {{"type", "string"}, {"enum", json::array({"path", "size", "lines"})}};
    props["order"] = {{"type", "string"}, {"enum", json::array({"asc", "desc"})}};
    props["limit"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 200}};
    json params = {{"type", "object"}, {"properties", std::move(props)}, {"required", json::array()}, {"additionalProperties", false}};
    return {{"type", "function"}, {"function", {
      {"name", "list_files"},
      {"description", "List allowed files; filter by path_filter or extension, or sort by path, indexed size, or lines (max 200, 8 KiB)."},
      {"parameters", std::move(params)}
    }}};
  }

  json searchContextDefinition()
  {
    json props;
    props["query"] = {{"type", "string"}, {"minLength", 1}, {"maxLength", 512}};
    json params = {{"type", "object"}, {"properties", std::move(props)}, {"required", json::array({"query"})}, {"additionalProperties", false}};
    return {{"type", "function"}, {"function", {
      {"name", "search_context"},
      {"description", "Hybrid semantic + text search over indexed context (falls back to text). Max 8 excerpts, 8 KiB."},
      {"parameters", std::move(params)}
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
      {"description", "Summarize loaded project metadata, configured sources, local file count, extensions, indexed chunk count, and available tools."},
      {"parameters", {{"type", "object"}, {"properties", json::object()}, {"additionalProperties", false}}}
    }}};
  }

  json projectSettingsDefinition() {
    return {{"type", "function"}, {"function", {
      {"name", "project_settings"},
      {"description", "Inspect filtered loaded project/provider settings (no credentials, URLs, or prompts; filenames and configured defaults only)."},
      {"parameters", {{"type", "object"}, {"properties", {
        {"section", {{"type", "string"}, {"enum", projectSections()}, {"description", "Section to inspect (default all)"}}}
      }}, {"additionalProperties", false}}}
    }}};
  }

  json executeProjectInfo(const json &call, const Settings &settings, const ToolContext &ctx, size_t indexedChunks, const std::vector<std::string> &availableTools) {
    LOG_MSG_DEBUG << "executeProjectInfo: " << call.dump();
    LOG_MSG_INFO << "[tool] project_info called";
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
    LOG_MSG_DEBUG << "executeProjectSettings: " << call.dump();
    LOG_MSG_INFO << "[tool] project_settings called";
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
      return {{"error", preview(e.what(), 512)}};
    }
  }

  namespace {
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
  }

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
      const auto selected = resolveAllowedFile(args.at("path").get<std::string>(), ctx.allowedFiles);
      const auto [first, last] = parseLineRange(args);
      const auto content = readTextFileChecked(selected);
      auto result = formatFileRange(selected, content, first, last, kMaxResultBytes);
      if (std::find(ctx.usedSources.begin(), ctx.usedSources.end(), selected) == ctx.usedSources.end()) ctx.usedSources.push_back(selected);
      return result;
    } catch (const std::exception &e) {
      return {{"error", preview(e.what(), 512)}};
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
        if (key != "files") throw std::invalid_argument("Unknown read_files argument: " + preview(key, 128));
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
            throw std::invalid_argument("Unknown read_files item argument: " + preview(key, 128));
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
          const auto selected = resolveAllowedFile(req.path, ctx.allowedFiles);
          auto cached = fileCache.find(selected);
          if (cached == fileCache.end()) {
            cached = fileCache.emplace(selected, readTextFileChecked(selected)).first;
          }
          formatted.push_back(formatFileRange(selected, cached->second, req.first, req.last, kMaxResultBytes));
          ++successCount;
        } catch (const std::exception &e) {
          formatted.push_back({{"path", req.path}, {"error", preview(e.what(), 512)}});
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
      return {{"error", preview(e.what(), 512)}};
    }
  }

  namespace {
    std::string normalizedSourceKey(const std::string &selected) {
      std::string p = selected;
      std::replace(p.begin(), p.end(), '\\', '/');
      const auto bytes = std::filesystem::path(std::u8string(p.begin(), p.end())).lexically_normal().generic_u8string();
      return std::string(bytes.begin(), bytes.end());
    }

    symbol_service::FileOutline computeVerifiedOutline(const std::string &selected, std::string &contentOut,
                                                       symbol_service::SymbolService *symbols) {
      const auto lang = symbol_service::detectLanguage(selected);
      if (lang == symbol_service::Language::Unsupported) {
        throw std::invalid_argument(
          "Unsupported file type for symbol extraction (supported: C, C++, Python, JavaScript/JSX, TypeScript, TSX; .h defaults to C++; .svelte is unsupported)");
      }
      contentOut = readTextFileChecked(selected);
      const std::string key = normalizedSourceKey(selected);
      if (symbols) {
        return symbols->outlineForContent(key, lang, contentOut);
      }
      symbol_service::SymbolService local;
      return local.outlineForContent(key, lang, contentOut);
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
  }

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
          throw std::invalid_argument("Unknown symbol_outline argument: " + preview(key, 128));
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

      const auto selected = resolveAllowedFile(args["path"].get<std::string>(), ctx.allowedFiles);
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
          arr.push_back(symbolEntryJson(*filtered[i]));
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
      return {{"error", preview(e.what(), 512)}};
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
          throw std::invalid_argument("Unknown read_symbol argument: " + preview(key, 128));
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

      const auto selected = resolveAllowedFile(args["path"].get<std::string>(), ctx.allowedFiles);
      std::string content;
      const auto outline = computeVerifiedOutline(selected, content, symbols);

      const auto matches = matchSymbolCandidates(outline.symbols, query, roleFilter);
      if (matches.empty()) {
        if (roleFilter) {
          const auto anyMatches = matchSymbolCandidates(outline.symbols, query, std::nullopt);
          if (!anyMatches.empty()) {
            const auto asked = symbol_service::roleName(*roleFilter);
            const auto other = *roleFilter == symbol_service::SymbolRole::Definition ? "declaration" : "definition";
            return {{"error", "No " + std::string(asked) + " found for '" + preview(query, 128) +
              "' in " + preview(selected, 128) + " (found " + std::to_string(anyMatches.size()) + " " + other + ")"}};
          }
        }
        return {{"error", "Symbol not found: " + preview(query, 128)}};
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
          arr.push_back(symbolEntryJson(*sym));
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
      return {{"error", preview(e.what(), 512)}};
    }
  }

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
          throw std::invalid_argument("Unknown find_symbol argument: " + preview(key, 128));
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

      const auto filter = pathFilter(args);
      std::string globText;
      const auto globRe = compileGlob(args, globText);
      const auto filteredFiles = filterAllowedFiles(ctx.allowedFiles, filter, globRe);

      struct SupportedEntry {
        std::string source;
        std::string key;
        symbol_service::Language language;
      };
      std::vector<SupportedEntry> supportedFiles;
      supportedFiles.reserve(filteredFiles.size());
      for (const auto &file : filteredFiles) {
        const auto lang = symbol_service::detectLanguage(file);
        if (lang != symbol_service::Language::Unsupported) {
          supportedFiles.push_back({file, normalizedSourceKey(file), lang});
        }
      }

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
          const auto content = readTextFileChecked(entry.source);
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
        auto item = symbolEntryJson(collected[i].sym);
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
      return {{"error", preview(e.what(), 512)}};
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
          throw std::invalid_argument("unknown argument: " + preview(it.key(), 128) + suggestion);
        }
      }

      const auto filter = pathFilter(args);
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
      return {{"error", preview(e.what(), 512)}};
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
          throw std::invalid_argument("Unknown grep argument: " + preview(key, 128));
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
      const auto filter = pathFilter(args);
      std::string globText;
      const auto globRe = compileGlob(args, globText);
      const auto paths = filterAllowedFiles(ctx.allowedFiles, filter, globRe);
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
    const std::function<json(const json &)> &execute, const std::function<bool()> &connected, size_t maxSteps, const Status &onStatus, const std::string &displayRoot)
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
      size_t callIndex = 0;
      for (const auto &call : *calls) {
        if (!connected()) return result;
        ++callIndex;
        const auto name = call.at("function").value("name", std::string("tool"));
        const auto callId = call.at("id").get<std::string>();
        std::string description = preview(name, 64) + "(";
        json args = json::object();
        try {
          const auto &raw = call.at("function").at("arguments").get_ref<const std::string &>();
          args = raw.size() <= 4096 ? json::parse(raw) : json::object();
          bool first = true;
          if (args.is_object()) for (const auto &[key, value] : args.items()) {
            if (!first) description += ", ";
            first = false;
            description += preview(key, 64) + "=";
            description += value.dump();
          }
          if (name == "grep" && !args.contains("path_filter")) description += (first ? "" : ", ") + std::string("path_filter=<all configured files>");
        } catch (const std::exception &) { description += "invalid arguments"; }
        description = preview(description + ")", 4608);
        const auto prefix = "Round " + std::to_string(step + 1) + "/" + std::to_string(maxSteps) +
          " \xc2\xb7 Call " + std::to_string(callIndex) + "/" + std::to_string(calls->size()) + ": " + description;
        const auto compactPrefix = "Round " + std::to_string(step + 1) + middle +
          std::to_string(callIndex) + "/" + std::to_string(calls->size()) + middle;
        json event = {{"event_id", "tool:" + std::to_string(step + 1) + ":" + callId}, {"tool", name},
          {"round", step + 1}, {"max_rounds", maxSteps}, {"call_id", callId}, {"call_index", callIndex}, {"call_count", calls->size()},
          {"status", "running"}, {"content", prefix}, {"summary", compactPrefix + compactAction(name, args, displayRoot)}};
        if (onStatus) onStatus(event);
        if (!connected()) return result;
        json output;
        try { output = execute(call); }
        catch (const std::exception &e) { output = {{"error", preview(utils::sanitizeUtf8(e.what()), 512)}}; }
        std::string summary;
        std::string status = "success";
        if (output.contains("error")) { summary = "failed: " + output.at("error").get<std::string>(); status = "error"; }
        else if (name == "grep") {
          if (output.value("outcome", "") == "no_files_matched_filter") { summary = "no files matched the filter"; status = "info"; }
          else {
            summary = std::to_string(output.at("matches").size()) + " snippets; searched " +
              std::to_string(output.value("searched_files", size_t(0))) + "/" + std::to_string(output.value("matched_files", size_t(0))) + " files";
            const auto unreadable = output.value("unreadable_files", size_t(0));
            if (unreadable) { summary += "; " + std::to_string(unreadable) + " unreadable"; status = "warning"; }
            else if (output.at("matches").empty()) status = "info";
          }
        } else if (name == "list_files") {
          summary = std::to_string(output.at("count").get<size_t>()) + " files returned";
          if (output.at("count") == 0) status = "info";
        } else if (name == "search_context") {
          summary = std::to_string(output.at("matches").size()) + " excerpts returned (" + output.at("mode").get<std::string>() + ")";
          if (output.at("matches").empty()) status = "info";
        } else if (name == "read_file") summary = "returned lines " + std::to_string(output.at("start_line").get<size_t>()) + "-" + std::to_string(output.at("end_line").get<size_t>());
        else if (name == "read_files") summary = std::to_string(output.value("count", size_t(0))) + "/" + std::to_string(output.value("requested_count", size_t(0))) + " files returned";
        else if (name == "symbol_outline") {
          summary = std::to_string(output.value("symbol_count", size_t(0))) + " symbols returned";
          if (output.value("symbol_count", size_t(0)) == 0) status = "info";
        } else if (name == "read_symbol") {
          if (output.value("ambiguous", false)) {
            summary = std::to_string(output.value("candidate_count", size_t(0))) + " candidates returned";
            status = "info";
          } else {
            summary = "returned lines " + std::to_string(output.at("start_line").get<size_t>()) + "-" + std::to_string(output.at("end_line").get<size_t>());
          }
        } else if (name == "find_symbol") {
          summary = std::to_string(output.value("match_count", size_t(0))) + " matches; searched " +
            std::to_string(output.value("searched_files", size_t(0))) + "/" +
            std::to_string(output.value("supported_files", size_t(0))) + " files";
          if (output.value("incomplete", false)) {
            summary += " (incomplete)";
            status = "warning";
          } else if (output.value("match_count", size_t(0)) == 0) {
            status = "info";
          }
        }
        else if (name == "project_info" || name == "project_settings") summary = "loaded summary returned";
        else summary = "inspection returned";
        if (output.value("truncated", false)) { summary += " (truncated)"; if (status != "error") status = "warning"; }
        event["status"] = status;
        event["content"] = prefix + " \xe2\x80\x94 " + summary;
        std::string compactResult;
        if (output.contains("error")) compactResult = "failed: " + preview(output.at("error").get<std::string>(), 100);
        else if (name == "grep") {
          if (output.value("outcome", "") == "no_files_matched_filter") compactResult = "no files matched";
          else {
            const auto searched = output.value("searched_files", size_t(0));
            const auto matched = output.value("matched_files", size_t(0));
            compactResult = std::to_string(output.at("matches").size()) + " snippets" + middle + std::to_string(searched);
            if (searched != matched) compactResult += "/" + std::to_string(matched);
            compactResult += " files";
            const auto unreadable = output.value("unreadable_files", size_t(0));
            if (unreadable) compactResult += middle + std::to_string(unreadable) + " unreadable";
          }
        } else if (name == "list_files") compactResult = std::to_string(output.at("count").get<size_t>()) + " files";
        else if (name == "search_context") compactResult = std::to_string(output.at("matches").size()) + " excerpts";
        else if (name == "read_files") compactResult = std::to_string(output.value("count", size_t(0))) + "/" + std::to_string(output.value("requested_count", size_t(0))) + " files";
        else if (name == "symbol_outline") compactResult = std::to_string(output.value("symbol_count", size_t(0))) + " symbols";
        else if (name == "read_symbol" && output.value("ambiguous", false)) compactResult = std::to_string(output.value("candidate_count", size_t(0))) + " candidates";
        else if (name == "find_symbol") {
          compactResult = std::to_string(output.value("match_count", size_t(0))) + " matches" + middle +
            std::to_string(output.value("searched_files", size_t(0))) + "/" +
            std::to_string(output.value("supported_files", size_t(0))) + " files";
          if (output.value("incomplete", false)) compactResult += " (incomplete)";
        }
        auto compact = compactPrefix + compactAction(name, args, displayRoot, &output);
        if (!compactResult.empty()) compact += dash + compactResult;
        if (output.value("truncated", false)) compact += " (truncated)";
        event["summary"] = compact;
        if (name == "grep" && !output.contains("error")) {
          for (const auto *key : {"outcome", "matched_files", "searched_files", "unreadable_files", "search_limited", "path_filter", "glob"})
            if (output.contains(key)) event[key] = output[key];
        }
        if (onStatus) onStatus(event);
        messages.push_back({{"role", "tool"}, {"tool_call_id", call.at("id")}, {"content", output.dump()}});
      }
    }
    return result;
  }
} // namespace chat_tools
