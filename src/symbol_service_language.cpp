#include "symbol_service.h"

#include <stdexcept>
#include <string>
#include <utility>

extern "C" {
  const TSLanguage *tree_sitter_c(void);
  const TSLanguage *tree_sitter_cpp(void);
  const TSLanguage *tree_sitter_python(void);
  const TSLanguage *tree_sitter_javascript(void);
  const TSLanguage *tree_sitter_typescript(void);
  const TSLanguage *tree_sitter_tsx(void);
}

namespace symbol_service {
  namespace {
    const TSLanguage *tsLanguageFor(Language lang) {
      switch (lang) {
        case Language::C: return tree_sitter_c();
        case Language::Cpp: return tree_sitter_cpp();
        case Language::Python: return tree_sitter_python();
        case Language::JavaScript: return tree_sitter_javascript();
        case Language::TypeScript: return tree_sitter_typescript();
        case Language::Tsx: return tree_sitter_tsx();
        case Language::Unsupported: return nullptr;
      }
      return nullptr;
    }
  } // namespace

  namespace detail {
    void ensureLanguage(TSParser *parser, Language lang) {
      const TSLanguage *tsLang = tsLanguageFor(lang);
      if (!tsLang) {
        throw std::invalid_argument("Unsupported language for symbol extraction");
      }
      const uint32_t abi = ts_language_abi_version(tsLang);
      if (abi < TREE_SITTER_MIN_COMPATIBLE_LANGUAGE_VERSION || abi > TREE_SITTER_LANGUAGE_VERSION) {
        throw std::runtime_error("Incompatible Tree-sitter ABI version for " +
          std::string(languageName(lang)) + ": " + std::to_string(abi));
      }
      if (!ts_parser_set_language(parser, tsLang)) {
        throw std::runtime_error("ts_parser_set_language() failed for " + std::string(languageName(lang)));
      }
    }
  } // namespace detail

  Language detectLanguage(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    const auto filename = (slash == std::string_view::npos) ? path : path.substr(slash + 1);
    const auto dot = filename.rfind('.');
    if (dot == std::string_view::npos) return Language::Unsupported;
    const std::string ext = detail::toLowerAscii(filename.substr(dot));
    if (ext == ".c") return Language::C;
    if (ext == ".cpp" || ext == ".cc" || ext == ".cxx" ||
        ext == ".hpp" || ext == ".hh" || ext == ".hxx" ||
        ext == ".ipp" || ext == ".tpp" || ext == ".h") {
      return Language::Cpp;
    }
    if (ext == ".py" || ext == ".pyi") return Language::Python;
    if (ext == ".js" || ext == ".jsx" || ext == ".mjs" || ext == ".cjs") return Language::JavaScript;
    if (ext == ".ts" || ext == ".mts" || ext == ".cts") return Language::TypeScript;
    if (ext == ".tsx") return Language::Tsx;
    return Language::Unsupported;
  }

  std::string_view languageName(Language lang) {
    switch (lang) {
      case Language::C: return "c";
      case Language::Cpp: return "cpp";
      case Language::Python: return "python";
      case Language::JavaScript: return "javascript";
      case Language::TypeScript: return "typescript";
      case Language::Tsx: return "tsx";
      case Language::Unsupported: return "unsupported";
    }
    return "unsupported";
  }

  std::string_view kindName(SymbolKind kind) {
    switch (kind) {
      case SymbolKind::Namespace: return "namespace";
      case SymbolKind::Class: return "class";
      case SymbolKind::Struct: return "struct";
      case SymbolKind::Union: return "union";
      case SymbolKind::Interface: return "interface";
      case SymbolKind::Enum: return "enum";
      case SymbolKind::Concept: return "concept";
      case SymbolKind::TypeAlias: return "type_alias";
      case SymbolKind::Function: return "function";
      case SymbolKind::Method: return "method";
      case SymbolKind::Constructor: return "constructor";
      case SymbolKind::Destructor: return "destructor";
    }
    return "function";
  }

  std::optional<SymbolKind> parseKind(std::string_view name) {
    static const std::pair<std::string_view, SymbolKind> table[] = {
      {"namespace", SymbolKind::Namespace},
      {"class", SymbolKind::Class},
      {"struct", SymbolKind::Struct},
      {"union", SymbolKind::Union},
      {"interface", SymbolKind::Interface},
      {"enum", SymbolKind::Enum},
      {"concept", SymbolKind::Concept},
      {"type_alias", SymbolKind::TypeAlias},
      {"function", SymbolKind::Function},
      {"method", SymbolKind::Method},
      {"constructor", SymbolKind::Constructor},
      {"destructor", SymbolKind::Destructor}
    };
    for (const auto &[k, v] : table) {
      if (k == name) return v;
    }
    return std::nullopt;
  }

  std::string_view roleName(SymbolRole role) {
    switch (role) {
      case SymbolRole::Definition: return "definition";
      case SymbolRole::Declaration: return "declaration";
    }
    return "definition";
  }
} // namespace symbol_service
