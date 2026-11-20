#include "symbol_service.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace symbol_service::detail {

  std::string toLowerAscii(std::string_view text) {
    std::string out(text.size(), '\0');
    for (size_t i = 0; i < text.size(); ++i) {
      const unsigned char c = static_cast<unsigned char>(text[i]);
      out[i] = static_cast<char>((c >= 'A' && c <= 'Z') ? (c + ('a' - 'A')) : c);
    }
    return out;
  }

  std::string_view trimView(std::string_view sv) {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) sv.remove_prefix(1);
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) sv.remove_suffix(1);
    return sv;
  }

  std::string collapseWhitespace(std::string_view sv, size_t maxBytes) {
    std::string out;
    out.reserve((std::min)(sv.size(), maxBytes));
    bool prevSpace = true;
    for (const char ch : sv) {
      const unsigned char uch = static_cast<unsigned char>(ch);
      if (std::isspace(uch)) {
        if (!prevSpace) {
          out.push_back(' ');
          prevSpace = true;
        }
      } else {
        out.push_back(ch);
        prevSpace = false;
      }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (out.size() > maxBytes) {
      size_t cut = maxBytes > 3 ? maxBytes - 3 : 0;
      while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xc0) == 0x80) --cut;
      out.resize(cut);
      out += "...";
    }
    return out;
  }

  ParserHandle::ParserHandle() : raw(ts_parser_new()) {
    if (!raw) throw std::runtime_error("Unable to allocate Tree-sitter parser");
  }

  ParserHandle::~ParserHandle() {
    if (raw) ts_parser_delete(raw);
  }

  TreeHandle::TreeHandle(TSTree *t) : raw(t) {}

  TreeHandle::~TreeHandle() {
    if (raw) ts_tree_delete(raw);
  }

  CursorGuard::CursorGuard(TSNode root) : raw(ts_tree_cursor_new(root)) {}

  CursorGuard::~CursorGuard() {
    ts_tree_cursor_delete(&raw);
  }

  std::string_view nodeText(TSNode node, std::string_view src) {
    if (ts_node_is_null(node)) return {};
    const uint32_t s = ts_node_start_byte(node);
    const uint32_t e = ts_node_end_byte(node);
    if (s > e || e > src.size()) return {};
    return src.substr(s, e - s);
  }

  bool isType(TSNode node, const char *typeName) {
    return !ts_node_is_null(node) && std::strcmp(ts_node_type(node), typeName) == 0;
  }

  TSNode effectiveRange(TSNode inner, TSNode rangeOwner) {
    return ts_node_is_null(rangeOwner) ? inner : rangeOwner;
  }

  std::string joinScope(const std::vector<std::string> &parts, std::string_view separator) {
    std::string out;
    for (const auto &part : parts) {
      if (part.empty()) continue;
      if (!out.empty()) out += separator;
      out += part;
    }
    return out;
  }

  void fillSpan(TSNode rangeNode, size_t maxBytes, SymbolInfo &sym) {
    const uint32_t sb = ts_node_start_byte(rangeNode);
    const uint32_t eb = ts_node_end_byte(rangeNode);
    sym.start_byte = (std::min)(static_cast<size_t>(sb), maxBytes);
    sym.end_byte = (std::min)(static_cast<size_t>(eb), maxBytes);
    if (sym.end_byte < sym.start_byte) sym.end_byte = sym.start_byte;

    const TSPoint sp = ts_node_start_point(rangeNode);
    const TSPoint ep = ts_node_end_point(rangeNode);
    sym.start_line = static_cast<size_t>(sp.row) + 1;
    sym.start_col = static_cast<size_t>(sp.column) + 1;
    if (ep.column == 0 && ep.row > sp.row) {
      sym.end_line = static_cast<size_t>(ep.row);
      sym.end_col = 1;
    } else {
      sym.end_line = static_cast<size_t>(ep.row) + 1;
      sym.end_col = static_cast<size_t>(ep.column) + 1;
    }
  }

  std::string sliceUntilByte(std::string_view src, TSNode startNode, uint32_t endByte, char trimTrail) {
    const uint32_t startByte = ts_node_start_byte(startNode);
    if (startByte >= endByte || endByte > src.size()) return {};
    std::string_view raw = src.substr(startByte, endByte - startByte);
    while (!raw.empty() && (raw.back() == trimTrail || std::isspace(static_cast<unsigned char>(raw.back())))) {
      raw.remove_suffix(1);
    }
    return collapseWhitespace(raw);
  }

  std::string cleanDeclarationSignature(std::string_view src, TSNode rangeNode) {
    std::string_view raw = nodeText(rangeNode, src);
    while (!raw.empty() && (raw.back() == ';' || std::isspace(static_cast<unsigned char>(raw.back())))) {
      raw.remove_suffix(1);
    }
    return collapseWhitespace(raw);
  }

  void emitSymbol(std::vector<SymbolInfo> &symbols, std::string_view src, std::string_view separator,
                  SymbolInfo sym, TSNode rangeNode) {
    if (sym.name.empty()) return;
    fillSpan(rangeNode, src.size(), sym);
    sym.qualified_name = sym.container.empty() ? sym.name : (sym.container + std::string(separator) + sym.name);
    symbols.push_back(std::move(sym));
  }

  void assignUniqueSelectors(std::vector<SymbolInfo> &symbols) {
    std::unordered_map<std::string, size_t> seen;
    seen.reserve(symbols.size());
    for (auto &sym : symbols) {
      std::string base = sym.qualified_name + "@" + std::to_string(sym.start_line) + ":" + std::to_string(sym.start_col);
      const size_t ordinal = ++seen[base];
      sym.selector = ordinal == 1 ? std::move(base) : (base + "#" + std::to_string(ordinal));
    }
  }

  bool isPythonStubEllipsis(TSNode body) {
    if (ts_node_is_null(body)) return false;
    uint32_t nonComment = 0;
    TSNode single{};
    const uint32_t n = ts_node_named_child_count(body);
    for (uint32_t i = 0; i < n; ++i) {
      TSNode child = ts_node_named_child(body, i);
      if (isType(child, "comment")) continue;
      ++nonComment;
      single = child;
    }
    if (nonComment != 1 || !isType(single, "expression_statement")) return false;
    const uint32_t sn = ts_node_named_child_count(single);
    for (uint32_t i = 0; i < sn; ++i) {
      if (isType(ts_node_named_child(single, i), "ellipsis")) return true;
    }
    return false;
  }

} // namespace symbol_service::detail
