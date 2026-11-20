#pragma once

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <list>

namespace symbol_service {

  enum class Language {
    Unsupported,
    C,
    Cpp,
    Python,
    JavaScript,
    TypeScript,
    Tsx
  };

  enum class SymbolKind {
    Namespace,
    Class,
    Struct,
    Union,
    Interface,
    Enum,
    Concept,
    TypeAlias,
    Function,
    Method,
    Constructor,
    Destructor
  };

  enum class SymbolRole {
    Definition,
    Declaration
  };

  struct SymbolInfo {
    std::string selector;
    std::string name;
    std::string qualified_name;
    std::string container;
    SymbolKind kind = SymbolKind::Function;
    SymbolRole role = SymbolRole::Definition;
    std::string signature;
    size_t start_line = 0;  // 1-based inclusive
    size_t end_line = 0;    // 1-based inclusive
    size_t start_col = 0;   // 1-based UTF-8 byte column
    size_t end_col = 0;     // 1-based UTF-8 byte column
    size_t start_byte = 0;  // 0-based inclusive
    size_t end_byte = 0;    // 0-based exclusive
  };

  struct ImportEdge {
    std::string raw_specifier;
    std::string kind; // "include", "import", "from_import", "reexport", "require", "dynamic_import"
    size_t line = 0;  // 1-based line number
    bool is_system = false;
  };

  struct ResolvedDependency {
    std::string source_path;   // normalized path key
    std::string target_spec;
    std::string resolved_path; // normalized path key, empty if external/unresolved
    std::string kind;
    size_t line = 0;
    bool is_system = false;
  };

  struct ReferenceMatch {
    size_t line = 0;
    size_t column = 0;
    std::string kind; // "call", "type", "import", "reference", "definition", "declaration"
    std::string enclosing_symbol;
    std::string snippet;
    bool exact_qualifier = false;
  };

  struct FileOutline {
    std::string path;
    Language language = Language::Unsupported;
    std::string content_sha256;
    size_t byte_size = 0;
    std::vector<SymbolInfo> symbols;
    std::vector<ImportEdge> imports;
  };

  // Explicit extension mapping across bundled parsers (C, C++, Python, JavaScript/JSX,
  // TypeScript, and TSX). Ambiguous .h headers default to C++; .c routes to C.
  Language detectLanguage(std::string_view path);
  std::string_view languageName(Language lang);
  std::string_view kindName(SymbolKind kind);
  std::optional<SymbolKind> parseKind(std::string_view name);
  std::string_view roleName(SymbolRole role);
  std::string sha256Hex(std::string_view data);

  std::vector<ImportEdge> extractImportsForContent(Language lang, std::string_view content);
  std::vector<ResolvedDependency> resolveImports(std::string_view sourcePath, Language lang,
    const std::vector<ImportEdge> &imports, const std::vector<std::string> &candidatePaths);
  std::vector<ResolvedDependency> extractDependenciesForFile(std::string_view sourcePath,
    std::string_view content, const std::vector<std::string> &candidatePaths);
  std::vector<ReferenceMatch> findReferencesInContent(Language lang, std::string_view content,
    std::string_view query, bool includeDeclarations = false,
    const std::vector<SymbolInfo> *outlineSymbols = nullptr);

  class SymbolService {
  public:
    static constexpr size_t kDefaultMaxCacheEntries = 256;

    explicit SymbolService(size_t maxCacheEntries = kDefaultMaxCacheEntries);
    ~SymbolService() = default;

    SymbolService(const SymbolService &) = delete;
    SymbolService &operator=(const SymbolService &) = delete;

    // Computes or returns cached outline after verifying content SHA-256 and byte length
    // against the supplied source snapshot.
    FileOutline outlineForContent(std::string_view cacheKey, Language lang, std::string_view content, bool *cacheHit = nullptr);

    size_t cachedFileCount() const;
    void clearCache();

  private:
    struct CacheNode {
      std::string key;
      FileOutline outline;
    };

    size_t maxCacheEntries_;
    mutable std::mutex mutex_;
    std::list<CacheNode> lru_;
    std::unordered_map<std::string, std::list<CacheNode>::iterator> index_;
  };

} // namespace symbol_service

#if defined(__has_include) && __has_include(<tree_sitter/api.h>)
#include <tree_sitter/api.h>

// Internal: shared between symbol_*.cpp. Not for callers.
namespace symbol_service::detail {
  std::string toLowerAscii(std::string_view text);
  std::string_view trimView(std::string_view sv);
  std::string collapseWhitespace(std::string_view sv, size_t maxBytes = 200);

  struct ParserHandle {
    TSParser *raw = nullptr;
    ParserHandle();
    ~ParserHandle();
    ParserHandle(const ParserHandle &) = delete;
    ParserHandle &operator=(const ParserHandle &) = delete;
  };

  struct TreeHandle {
    TSTree *raw = nullptr;
    explicit TreeHandle(TSTree *t);
    ~TreeHandle();
    TreeHandle(const TreeHandle &) = delete;
    TreeHandle &operator=(const TreeHandle &) = delete;
  };

  struct CursorGuard {
    TSTreeCursor raw;
    explicit CursorGuard(TSNode root);
    ~CursorGuard();
    CursorGuard(const CursorGuard &) = delete;
    CursorGuard &operator=(const CursorGuard &) = delete;
  };

  template <class Fn>
  void walkAst(TSNode root, Fn &&visit) {
    if (ts_node_is_null(root)) return;
    CursorGuard cursor(root);
    bool reachedRoot = false;
    while (!reachedRoot) {
      TSNode current = ts_tree_cursor_current_node(&cursor.raw);
      if (visit(current) && ts_tree_cursor_goto_first_child(&cursor.raw)) {
        continue;
      }
      if (ts_tree_cursor_goto_next_sibling(&cursor.raw)) {
        continue;
      }
      while (true) {
        if (!ts_tree_cursor_goto_parent(&cursor.raw)) {
          reachedRoot = true;
          break;
        }
        if (ts_tree_cursor_goto_next_sibling(&cursor.raw)) {
          break;
        }
      }
    }
  }

  std::string_view nodeText(TSNode node, std::string_view src);
  bool isType(TSNode node, const char *typeName);
  TSNode effectiveRange(TSNode inner, TSNode rangeOwner);
  std::string joinScope(const std::vector<std::string> &parts, std::string_view separator);
  void fillSpan(TSNode rangeNode, size_t maxBytes, SymbolInfo &sym);
  std::string sliceUntilByte(std::string_view src, TSNode startNode, uint32_t endByte, char trimTrail = ';');
  std::string cleanDeclarationSignature(std::string_view src, TSNode rangeNode);
  void emitSymbol(std::vector<SymbolInfo> &symbols, std::string_view src, std::string_view separator,
                  SymbolInfo sym, TSNode rangeNode);
  void assignUniqueSelectors(std::vector<SymbolInfo> &symbols);
  void ensureLanguage(TSParser *parser, Language lang);
  std::vector<SymbolInfo> extractCpp(TSNode root, std::string_view content);
  std::vector<SymbolInfo> extractC(TSNode root, std::string_view content);
  bool isPythonStubEllipsis(TSNode body);
  std::vector<SymbolInfo> extractPython(TSNode root, std::string_view content, bool isStubFile);
  std::vector<SymbolInfo> extractJsTs(TSNode root, std::string_view content);
  std::vector<SymbolInfo> extractOutlineSymbols(Language lang, std::string_view key, TSNode root, std::string_view content);
  std::vector<ImportEdge> extractImportsFromRoot(Language lang, TSNode root, std::string_view src);
}
#endif
