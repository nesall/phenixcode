#include "symbol_service.h"

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace symbol_service {
  namespace detail {
    std::vector<SymbolInfo> extractOutlineSymbols(Language lang, std::string_view key, TSNode root, std::string_view content) {
      switch (lang) {
        case Language::C:
          return extractC(root, content);
        case Language::Cpp:
          return extractCpp(root, content);
        case Language::Python: {
          const std::string lowerKey = toLowerAscii(key);
          return extractPython(root, content, lowerKey.ends_with(".pyi"));
        }
        case Language::JavaScript:
        case Language::TypeScript:
        case Language::Tsx:
          return extractJsTs(root, content);
        case Language::Unsupported:
          return {};
      }
      return {};
    }
  } // namespace detail

  FileOutline SymbolService::outlineForContent(std::string_view cacheKey, Language lang, std::string_view content, bool *cacheHit) {
    if (lang == Language::Unsupported) {
      throw std::invalid_argument("Unsupported language for symbol extraction");
    }
    const std::string digest = sha256Hex(content);
    const std::string key(cacheKey);

    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (const auto it = index_.find(key); it != index_.end()) {
        const auto &cached = it->second->outline;
        if (cached.language == lang && cached.byte_size == content.size() && cached.content_sha256 == digest) {
          lru_.splice(lru_.begin(), lru_, it->second);
          if (cacheHit) *cacheHit = true;
          return it->second->outline;
        }
      }
    }

    if (cacheHit) *cacheHit = false;
    detail::ParserHandle parser;
    detail::ensureLanguage(parser.raw, lang);
    detail::TreeHandle tree(ts_parser_parse_string(parser.raw, nullptr, content.data(), static_cast<uint32_t>(content.size())));
    if (!tree.raw) {
      throw std::runtime_error("Tree-sitter failed to parse " + std::string(languageName(lang)) + " source");
    }

    FileOutline outline;
    outline.path = key;
    outline.language = lang;
    outline.content_sha256 = digest;
    outline.byte_size = content.size();
    const TSNode root = ts_tree_root_node(tree.raw);
    outline.symbols = detail::extractOutlineSymbols(lang, key, root, content);
    outline.imports = detail::extractImportsFromRoot(lang, root, content);

    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (const auto it = index_.find(key); it != index_.end()) {
        it->second->outline = outline;
        lru_.splice(lru_.begin(), lru_, it->second);
      } else {
        if (lru_.size() >= maxCacheEntries_) {
          index_.erase(lru_.back().key);
          lru_.pop_back();
        }
        lru_.push_front({ key, outline });
        index_.emplace(key, lru_.begin());
      }
    }

    return outline;
  }
} // namespace symbol_service
