#include "chat_tools.h"

using nlohmann::json;

namespace chat_tools {
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

  json dependenciesDefinition() {
    static const json directions = json::array({"both", "outgoing", "incoming"});
    json props;
    props["path"] = {{"type", "string"}};
    props["direction"] = {{"type", "string"}, {"enum", directions}};
    props["include_external"] = {{"type", "boolean"}};
    props["limit"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}};
    json params = {{"type", "object"}, {"properties", std::move(props)}, {"required", json::array({"path"})}, {"additionalProperties", false}};
    return {{"type", "function"}, {"function", {
      {"name", "dependencies"},
      {"description", "Inspect imports/includes (outgoing) and dependents (incoming) for an allowed file."},
      {"parameters", std::move(params)}
    }}};
  }

  json findReferencesDefinition() {
    json props;
    props["symbol"] = {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}, {"description", "Symbol, Class::m, or Class.m"}};
    props["include_declarations"] = {{"type", "boolean"}};
    props["path_filter"] = {{"type", "string"}};
    props["glob"] = {{"type", "string"}};
    props["limit"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}};
    json params = {{"type", "object"}, {"properties", std::move(props)}, {"required", json::array({"symbol"})}, {"additionalProperties", false}};
    return {{"type", "function"}, {"function", {
      {"name", "find_references"},
      {"description", "Find AST-verified symbol usages across allowed files, skipping comments and strings."},
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
        {"section", {{"type", "string"}, {"enum", detail::projectSections()}, {"description", "Section to inspect (default all)"}}}
      }}, {"additionalProperties", false}}}
    }}};
  }
} // namespace chat_tools
