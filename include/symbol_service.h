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
    Cpp
  };

  enum class SymbolKind {
    Namespace,
    Class,
    Struct,
    Union,
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

  struct FileOutline {
    std::string path;
    Language language = Language::Unsupported;
    std::string content_sha256;
    size_t byte_size = 0;
    std::vector<SymbolInfo> symbols;
  };

  // Explicit extension mapping. Ambiguous .h headers default to C++.
  // .c and non-C++ extensions remain Unsupported until their grammars are bundled.
  Language detectLanguage(std::string_view path);
  std::string_view languageName(Language lang);
  std::string_view kindName(SymbolKind kind);
  std::optional<SymbolKind> parseKind(std::string_view name);
  std::string_view roleName(SymbolRole role);
  std::string sha256Hex(std::string_view data);

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
