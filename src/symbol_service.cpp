#include "symbol_service.h"
#include "cutils.h"

#include <tree_sitter/api.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

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

    std::string collapseWhitespace(std::string_view sv, size_t maxBytes = 200) {
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

    std::string joinContainers(const std::vector<std::string> &parts) {
      std::string out;
      for (const auto &part : parts) {
        if (part.empty()) continue;
        if (!out.empty()) out += "::";
        out += part;
      }
      return out;
    }

    std::string_view baseClassName(std::string_view name) {
      const auto angle = name.find('<');
      if (angle != std::string_view::npos) name = name.substr(0, angle);
      return trimView(name);
    }

    struct ParserHandle {
      TSParser *raw = nullptr;
      ParserHandle() : raw(ts_parser_new()) {
        if (!raw) throw std::runtime_error("Unable to allocate Tree-sitter parser");
      }
      ~ParserHandle() {
        if (raw) ts_parser_delete(raw);
      }
      ParserHandle(const ParserHandle &) = delete;
      ParserHandle &operator=(const ParserHandle &) = delete;
    };

    struct TreeHandle {
      TSTree *raw = nullptr;
      explicit TreeHandle(TSTree *t) : raw(t) {}
      ~TreeHandle() {
        if (raw) ts_tree_delete(raw);
      }
      TreeHandle(const TreeHandle &) = delete;
      TreeHandle &operator=(const TreeHandle &) = delete;
    };

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

    class ExtractorBase {
    protected:
      std::string_view src_;
      std::string separator_;
      std::vector<SymbolInfo> symbols_;

      explicit ExtractorBase(std::string_view source, std::string separator)
        : src_(source), separator_(std::move(separator)) {}

      std::string_view nodeText(TSNode node) const {
        if (ts_node_is_null(node)) return {};
        const uint32_t s = ts_node_start_byte(node);
        const uint32_t e = ts_node_end_byte(node);
        if (s > e || e > src_.size()) return {};
        return src_.substr(s, e - s);
      }

      static bool isType(TSNode node, const char *typeName) {
        return !ts_node_is_null(node) && std::strcmp(ts_node_type(node), typeName) == 0;
      }

      static TSNode effectiveRange(TSNode inner, TSNode rangeOwner) {
        return ts_node_is_null(rangeOwner) ? inner : rangeOwner;
      }

      std::string joinScope(const std::vector<std::string> &parts) const {
        std::string out;
        for (const auto &part : parts) {
          if (part.empty()) continue;
          if (!out.empty()) out += separator_;
          out += part;
        }
        return out;
      }

      static void fillSpan(TSNode rangeNode, size_t maxBytes, SymbolInfo &sym) {
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

      std::string sliceUntilByte(TSNode startNode, uint32_t endByte, char trimTrail = ';') const {
        const uint32_t startByte = ts_node_start_byte(startNode);
        if (startByte >= endByte || endByte > src_.size()) return {};
        std::string_view raw = src_.substr(startByte, endByte - startByte);
        while (!raw.empty() && (raw.back() == trimTrail || std::isspace(static_cast<unsigned char>(raw.back())))) {
          raw.remove_suffix(1);
        }
        return collapseWhitespace(raw);
      }

      std::string cleanDeclarationSignature(TSNode rangeNode) const {
        std::string_view raw = nodeText(rangeNode);
        while (!raw.empty() && (raw.back() == ';' || std::isspace(static_cast<unsigned char>(raw.back())))) {
          raw.remove_suffix(1);
        }
        return collapseWhitespace(raw);
      }

      void emitSymbol(SymbolInfo sym, TSNode rangeNode) {
        if (sym.name.empty()) return;
        fillSpan(rangeNode, src_.size(), sym);
        sym.qualified_name = sym.container.empty() ? sym.name : (sym.container + separator_ + sym.name);
        symbols_.push_back(std::move(sym));
      }

      void assignUniqueSelectors() {
        std::unordered_map<std::string, size_t> seen;
        seen.reserve(symbols_.size());
        for (auto &sym : symbols_) {
          std::string base = sym.qualified_name + "@" + std::to_string(sym.start_line) + ":" + std::to_string(sym.start_col);
          const size_t ordinal = ++seen[base];
          sym.selector = ordinal == 1 ? std::move(base) : (base + "#" + std::to_string(ordinal));
        }
      }
    };

    class CppExtractor : public ExtractorBase {
    public:
      explicit CppExtractor(std::string_view source) : ExtractorBase(source, "::") {}

      std::vector<SymbolInfo> extract(TSNode root) {
        ScopeCtx ctx;
        visitBlockChildren(root, ctx);
        assignUniqueSelectors();
        return std::move(symbols_);
      }

    private:
      struct ScopeCtx {
        std::vector<std::string> containers;
        bool inClass = false;
        std::string currentClass;
      };

      std::string normalizeScopeSegment(TSNode scopeNode) const {
        if (isType(scopeNode, "template_type")) {
          TSNode base = ts_node_child_by_field_name(scopeNode, "name", 4);
          if (!ts_node_is_null(base)) return collapseWhitespace(nodeText(base));
        }
        return std::string(trimView(nodeText(scopeNode)));
      }

      std::string normalizeLeafName(TSNode leafNode) const {
        if (isType(leafNode, "template_function") || isType(leafNode, "template_method")) {
          TSNode inner = ts_node_child_by_field_name(leafNode, "name", 4);
          if (!ts_node_is_null(inner)) return normalizeLeafName(inner);
        }
        if (isType(leafNode, "destructor_name")) {
          std::string compact;
          for (const char ch : nodeText(leafNode)) {
            if (!std::isspace(static_cast<unsigned char>(ch))) compact.push_back(ch);
          }
          return compact;
        }
        if (isType(leafNode, "operator_name")) {
          return collapseWhitespace(nodeText(leafNode));
        }
        if (isType(leafNode, "operator_cast")) {
          TSNode typeNode = ts_node_child_by_field_name(leafNode, "type", 4);
          if (!ts_node_is_null(typeNode)) {
            return "operator " + collapseWhitespace(nodeText(typeNode));
          }
          return collapseWhitespace(nodeText(leafNode));
        }
        if (isType(leafNode, "nested_namespace_specifier")) {
          std::string compact;
          for (const char ch : nodeText(leafNode)) {
            if (!std::isspace(static_cast<unsigned char>(ch))) compact.push_back(ch);
          }
          return compact;
        }
        return collapseWhitespace(nodeText(leafNode));
      }

      void unpackQualifiedName(TSNode nameNode, std::vector<std::string> &scopeOut, std::string &leafOut) const {
        if (ts_node_is_null(nameNode)) return;
        if (isType(nameNode, "qualified_identifier")) {
          TSNode scope = ts_node_child_by_field_name(nameNode, "scope", 5);
          if (!ts_node_is_null(scope)) {
            std::string seg = normalizeScopeSegment(scope);
            if (!seg.empty()) scopeOut.push_back(std::move(seg));
          }
          TSNode inner = ts_node_child_by_field_name(nameNode, "name", 4);
          unpackQualifiedName(inner, scopeOut, leafOut);
          return;
        }
        leafOut = normalizeLeafName(nameNode);
      }

      // Unwrap pointer/reference/attribute declarators to find the inner function_declarator or operator_cast.
      // Rejects function-pointer variables like `void (*fp)(int)`.
      TSNode findCallableDeclarator(TSNode declarator) const {
        TSNode cur = declarator;
        while (!ts_node_is_null(cur)) {
          const char *t = ts_node_type(cur);
          if (std::strcmp(t, "function_declarator") == 0) {
            TSNode inner = ts_node_child_by_field_name(cur, "declarator", 10);
            if (isType(inner, "parenthesized_declarator")) {
              // Parenthesized inner declarator containing `*` or `&` without another
              // function_declarator inside it denotes a function pointer/reference variable.
              return findCallableDeclarator(inner);
            }
            return cur;
          }
          if (std::strcmp(t, "operator_cast") == 0) {
            return cur;
          }
          if (std::strcmp(t, "pointer_declarator") == 0 ||
              std::strcmp(t, "array_declarator") == 0) {
            TSNode next = ts_node_child_by_field_name(cur, "declarator", 10);
            cur = next;
            continue;
          }
          if (std::strcmp(t, "reference_declarator") == 0 ||
              std::strcmp(t, "attributed_declarator") == 0 ||
              std::strcmp(t, "parenthesized_declarator") == 0) {
            const uint32_t n = ts_node_named_child_count(cur);
            TSNode next{};
            for (uint32_t i = 0; i < n; ++i) {
              TSNode child = ts_node_named_child(cur, i);
              if (!isType(child, "comment") &&
                  !isType(child, "attribute_declaration") &&
                  !isType(child, "attribute_specifier") &&
                  !isType(child, "type_qualifier") &&
                  !isType(child, "ms_call_modifier")) {
                next = child;
                break;
              }
            }
            cur = next;
            continue;
          }
          return {};
        }
        return {};
      }

      void visitBlockChildren(TSNode containerNode, const ScopeCtx &ctx) {
        const uint32_t count = ts_node_named_child_count(containerNode);
        for (uint32_t i = 0; i < count; ++i) {
          TSNode child = ts_node_named_child(containerNode, i);
          visitItem(child, {}, ctx);
        }
      }

      void visitPreprocNode(TSNode node, const ScopeCtx &ctx) {
        const uint32_t total = ts_node_child_count(node);
        for (uint32_t i = 0; i < total; ++i) {
          TSNode child = ts_node_child(node, i);
          if (!ts_node_is_named(child)) continue;
          const char *field = ts_node_field_name_for_child(node, i);
          if (field && (std::strcmp(field, "name") == 0 || std::strcmp(field, "condition") == 0)) {
            continue;
          }
          visitItem(child, {}, ctx);
        }
      }

      void emitCallable(TSNode declOwner, TSNode rangeOwner, TSNode callableDecl, SymbolRole role,
                        bool hasReturnType, const ScopeCtx &ctx, std::string signature) {
        TSNode leafNode = isType(callableDecl, "operator_cast")
          ? callableDecl
          : ts_node_child_by_field_name(callableDecl, "declarator", 10);
        if (ts_node_is_null(leafNode)) return;

        std::vector<std::string> qualScope;
        std::string leafName;
        unpackQualifiedName(leafNode, qualScope, leafName);
        if (leafName.empty()) return;

        std::vector<std::string> fullScope = ctx.containers;
        fullScope.insert(fullScope.end(), qualScope.begin(), qualScope.end());

        const bool isDestructor = isType(leafNode, "destructor_name") ||
          (!leafName.empty() && leafName.front() == '~');

        bool isConstructor = false;
        if (!isDestructor && !hasReturnType) {
          if (!qualScope.empty() && baseClassName(qualScope.back()) == leafName) {
            isConstructor = true;
          } else if (ctx.inClass && qualScope.empty() && baseClassName(ctx.currentClass) == leafName) {
            isConstructor = true;
          }
        }

        SymbolKind kind = SymbolKind::Function;
        if (isDestructor) kind = SymbolKind::Destructor;
        else if (isConstructor) kind = SymbolKind::Constructor;
        else if (ctx.inClass || !qualScope.empty()) kind = SymbolKind::Method;

        SymbolInfo sym;
        sym.name = std::move(leafName);
        sym.container = joinContainers(fullScope);
        sym.kind = kind;
        sym.role = role;
        sym.signature = std::move(signature);
        emitSymbol(std::move(sym), effectiveRange(declOwner, rangeOwner));
      }

      std::string functionDefSignature(TSNode defNode, TSNode rangeNode) const {
        TSNode body = ts_node_child_by_field_name(defNode, "body", 4);
        uint32_t cutByte = ts_node_end_byte(rangeNode);
        if (!ts_node_is_null(body)) {
          cutByte = (std::min)(cutByte, ts_node_start_byte(body));
        }
        const uint32_t n = ts_node_named_child_count(defNode);
        for (uint32_t i = 0; i < n; ++i) {
          TSNode child = ts_node_named_child(defNode, i);
          const char *t = ts_node_type(child);
          if (std::strcmp(t, "field_initializer_list") == 0 ||
              std::strcmp(t, "compound_statement") == 0 ||
              std::strcmp(t, "try_statement") == 0) {
            cutByte = (std::min)(cutByte, ts_node_start_byte(child));
          }
        }
        return sliceUntilByte(rangeNode, cutByte);
      }

      void handleTypeSpecifier(TSNode typeNode, TSNode rangeOwner, const ScopeCtx &ctx, bool emitForwardDecl) {
        const char *t = ts_node_type(typeNode);
        SymbolKind kind = SymbolKind::Struct;
        if (std::strcmp(t, "class_specifier") == 0) kind = SymbolKind::Class;
        else if (std::strcmp(t, "struct_specifier") == 0) kind = SymbolKind::Struct;
        else if (std::strcmp(t, "union_specifier") == 0) kind = SymbolKind::Union;
        else if (std::strcmp(t, "enum_specifier") == 0) kind = SymbolKind::Enum;
        else return;

        TSNode body = ts_node_child_by_field_name(typeNode, "body", 4);
        const bool hasBody = !ts_node_is_null(body);
        if (!hasBody && !emitForwardDecl) return;

        TSNode nameNode = ts_node_child_by_field_name(typeNode, "name", 4);
        if (ts_node_is_null(nameNode)) {
          // Still visit members inside anonymous nested struct/union cleanly?
          // Usually anonymous struct fields belong to enclosing class; skip anonymous tag.
          return;
        }

        std::vector<std::string> qualScope;
        std::string leafName;
        if (isType(nameNode, "template_type")) {
          // Explicit class template specialization like `template <> class Box<int>`.
          leafName = collapseWhitespace(nodeText(nameNode));
        } else {
          unpackQualifiedName(nameNode, qualScope, leafName);
        }
        if (leafName.empty()) return;

        std::vector<std::string> fullScope = ctx.containers;
        fullScope.insert(fullScope.end(), qualScope.begin(), qualScope.end());

        TSNode spanNode = effectiveRange(typeNode, rangeOwner);
        std::string sig = hasBody
          ? sliceUntilByte(spanNode, ts_node_start_byte(body))
          : cleanDeclarationSignature(spanNode);

        SymbolInfo sym;
        sym.name = leafName;
        sym.container = joinContainers(fullScope);
        sym.kind = kind;
        sym.role = hasBody ? SymbolRole::Definition : SymbolRole::Declaration;
        sym.signature = std::move(sig);
        emitSymbol(std::move(sym), spanNode);

        if (hasBody && kind != SymbolKind::Enum) {
          ScopeCtx innerCtx;
          innerCtx.containers = std::move(fullScope);
          innerCtx.containers.push_back(leafName);
          innerCtx.inClass = true;
          innerCtx.currentClass = leafName;
          visitBlockChildren(body, innerCtx);
        }
      }

      void handleDeclarationNode(TSNode declNode, TSNode rangeOwner, const ScopeCtx &ctx) {
        TSNode typeNode = ts_node_child_by_field_name(declNode, "type", 4);
        const uint32_t total = ts_node_child_count(declNode);
        std::vector<TSNode> declarators;
        for (uint32_t i = 0; i < total; ++i) {
          const char *field = ts_node_field_name_for_child(declNode, i);
          if (field && std::strcmp(field, "declarator") == 0) {
            declarators.push_back(ts_node_child(declNode, i));
          }
        }

        if (!ts_node_is_null(typeNode)) {
          const bool forwardOnly = declarators.empty();
          handleTypeSpecifier(typeNode, forwardOnly ? effectiveRange(declNode, rangeOwner) : TSNode{}, ctx, forwardOnly);
        }

        if (declarators.empty()) return;

        TSNode spanNode = effectiveRange(declNode, rangeOwner);
        const std::string sig = cleanDeclarationSignature(spanNode);
        const bool hasReturnType = !ts_node_is_null(typeNode);

        for (TSNode d : declarators) {
          if (isType(d, "init_declarator")) continue; // Variable / object initialization, not a function declaration.
          TSNode callable = findCallableDeclarator(d);
          if (ts_node_is_null(callable)) continue;
          emitCallable(declNode, rangeOwner, callable, SymbolRole::Declaration, hasReturnType, ctx, sig);
        }
      }

      void visitItem(TSNode node, TSNode rangeOwner, const ScopeCtx &ctx) {
        if (ts_node_is_null(node) || !ts_node_is_named(node)) return;
        const char *t = ts_node_type(node);

        if (std::strcmp(t, "preproc_ifdef") == 0 ||
            std::strcmp(t, "preproc_if") == 0 ||
            std::strcmp(t, "preproc_else") == 0 ||
            std::strcmp(t, "preproc_elif") == 0 ||
            std::strcmp(t, "preproc_elifdef") == 0) {
          visitPreprocNode(node, ctx);
          return;
        }

        if (std::strcmp(t, "linkage_specification") == 0) {
          TSNode body = ts_node_child_by_field_name(node, "body", 4);
          if (!ts_node_is_null(body)) {
            if (isType(body, "declaration_list")) {
              visitBlockChildren(body, ctx);
            } else {
              visitItem(body, effectiveRange(node, rangeOwner), ctx);
            }
          }
          return;
        }

        if (std::strcmp(t, "template_declaration") == 0) {
          TSNode outer = effectiveRange(node, rangeOwner);
          const uint32_t count = ts_node_named_child_count(node);
          for (uint32_t i = 0; i < count; ++i) {
            TSNode child = ts_node_named_child(node, i);
            const char *ct = ts_node_type(child);
            if (std::strcmp(ct, "template_parameter_list") == 0 ||
                std::strcmp(ct, "requires_clause") == 0 ||
                std::strcmp(ct, "comment") == 0) {
              continue;
            }
            visitItem(child, outer, ctx);
            break;
          }
          return;
        }

        if (std::strcmp(t, "namespace_definition") == 0) {
          TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
          TSNode body = ts_node_child_by_field_name(node, "body", 4);
          ScopeCtx nextCtx = ctx;
          if (!ts_node_is_null(nameNode)) {
            const std::string nsName = normalizeLeafName(nameNode);
            if (!nsName.empty()) {
              TSNode spanNode = effectiveRange(node, rangeOwner);
              SymbolInfo sym;
              sym.name = nsName;
              sym.container = joinContainers(ctx.containers);
              sym.kind = SymbolKind::Namespace;
              sym.role = SymbolRole::Definition;
              sym.signature = !ts_node_is_null(body)
                ? sliceUntilByte(spanNode, ts_node_start_byte(body))
                : ("namespace " + nsName);
              emitSymbol(std::move(sym), spanNode);
              nextCtx.containers.push_back(nsName);
            }
          } else {
            nextCtx.containers.emplace_back("(anonymous namespace)");
          }
          nextCtx.inClass = false;
          nextCtx.currentClass.clear();
          if (!ts_node_is_null(body)) {
            visitBlockChildren(body, nextCtx);
          }
          return;
        }

        if (std::strcmp(t, "class_specifier") == 0 ||
            std::strcmp(t, "struct_specifier") == 0 ||
            std::strcmp(t, "union_specifier") == 0 ||
            std::strcmp(t, "enum_specifier") == 0) {
          handleTypeSpecifier(node, rangeOwner, ctx, true);
          return;
        }

        if (std::strcmp(t, "function_definition") == 0) {
          TSNode decl = ts_node_child_by_field_name(node, "declarator", 10);
          TSNode callable = findCallableDeclarator(decl);
          if (ts_node_is_null(callable)) return;
          TSNode spanNode = effectiveRange(node, rangeOwner);
          TSNode retType = ts_node_child_by_field_name(node, "type", 4);
          emitCallable(node, rangeOwner, callable, SymbolRole::Definition,
                       !ts_node_is_null(retType), ctx, functionDefSignature(node, spanNode));
          return;
        }

        if (std::strcmp(t, "declaration") == 0 || std::strcmp(t, "field_declaration") == 0) {
          handleDeclarationNode(node, rangeOwner, ctx);
          return;
        }

        if (std::strcmp(t, "alias_declaration") == 0) {
          TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
          const std::string aliasName = collapseWhitespace(nodeText(nameNode));
          if (aliasName.empty()) return;
          TSNode spanNode = effectiveRange(node, rangeOwner);
          SymbolInfo sym;
          sym.name = aliasName;
          sym.container = joinContainers(ctx.containers);
          sym.kind = SymbolKind::TypeAlias;
          sym.role = SymbolRole::Definition;
          sym.signature = cleanDeclarationSignature(spanNode);
          emitSymbol(std::move(sym), spanNode);
          return;
        }

        if (std::strcmp(t, "type_definition") == 0) {
          TSNode typeNode = ts_node_child_by_field_name(node, "type", 4);
          if (!ts_node_is_null(typeNode)) {
            handleTypeSpecifier(typeNode, {}, ctx, false);
          }
          TSNode spanNode = effectiveRange(node, rangeOwner);
          const std::string sig = cleanDeclarationSignature(spanNode);
          const uint32_t total = ts_node_child_count(node);
          for (uint32_t i = 0; i < total; ++i) {
            const char *field = ts_node_field_name_for_child(node, i);
            if (!field || std::strcmp(field, "declarator") != 0) continue;
            TSNode d = ts_node_child(node, i);
            // Only plain typedef alias names (skip function pointer typedefs unless named).
            while (!ts_node_is_null(d) && (isType(d, "pointer_type_declarator") || isType(d, "array_declarator"))) {
              d = ts_node_child_by_field_name(d, "declarator", 10);
            }
            if (isType(d, "type_identifier")) {
              SymbolInfo sym;
              sym.name = collapseWhitespace(nodeText(d));
              sym.container = joinContainers(ctx.containers);
              sym.kind = SymbolKind::TypeAlias;
              sym.role = SymbolRole::Definition;
              sym.signature = sig;
              emitSymbol(std::move(sym), spanNode);
            }
          }
          return;
        }

        if (std::strcmp(t, "concept_definition") == 0) {
          TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
          const std::string conceptName = collapseWhitespace(nodeText(nameNode));
          if (conceptName.empty()) return;
          TSNode spanNode = effectiveRange(node, rangeOwner);
          SymbolInfo sym;
          sym.name = conceptName;
          sym.container = joinContainers(ctx.containers);
          sym.kind = SymbolKind::Concept;
          sym.role = SymbolRole::Definition;
          sym.signature = cleanDeclarationSignature(spanNode);
          emitSymbol(std::move(sym), spanNode);
          return;
        }
      }

      std::string joinContainers(const std::vector<std::string> &parts) const {
        return joinScope(parts);
      }
    };

    class PythonExtractor : public ExtractorBase {
    public:
      PythonExtractor(std::string_view source, bool isStubFile)
        : ExtractorBase(source, "."), isStubFile_(isStubFile) {}

      std::vector<SymbolInfo> extract(TSNode root) {
        visitBlock(root, {}, false);
        assignUniqueSelectors();
        return std::move(symbols_);
      }

    private:
      bool isStubFile_ = false;

      static bool isEllipsisBody(TSNode body) {
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

      void visitBlock(TSNode blockNode, const std::vector<std::string> &containers, bool inClass, TSNode rangeOwner = {}) {
        const uint32_t n = ts_node_named_child_count(blockNode);
        for (uint32_t i = 0; i < n; ++i) {
          visitItem(ts_node_named_child(blockNode, i), rangeOwner, containers, inClass);
        }
      }

      void visitItem(TSNode node, TSNode rangeOwner, const std::vector<std::string> &containers, bool inClass) {
        if (ts_node_is_null(node) || !ts_node_is_named(node)) return;
        const char *t = ts_node_type(node);

        if (std::strcmp(t, "decorated_definition") == 0) {
          TSNode outer = effectiveRange(node, rangeOwner);
          TSNode def = ts_node_child_by_field_name(node, "definition", 10);
          if (!ts_node_is_null(def)) visitItem(def, outer, containers, inClass);
          return;
        }

        if (std::strcmp(t, "class_definition") == 0) {
          TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
          TSNode body = ts_node_child_by_field_name(node, "body", 4);
          const std::string className = collapseWhitespace(nodeText(nameNode));
          if (className.empty()) return;
          TSNode spanNode = effectiveRange(node, rangeOwner);
          SymbolInfo sym;
          sym.name = className;
          sym.container = joinScope(containers);
          sym.kind = SymbolKind::Class;
          sym.role = SymbolRole::Definition;
          sym.signature = !ts_node_is_null(body)
            ? sliceUntilByte(spanNode, ts_node_start_byte(body), ':')
            : collapseWhitespace(nodeText(spanNode));
          emitSymbol(std::move(sym), spanNode);

          if (!ts_node_is_null(body)) {
            auto childContainers = containers;
            childContainers.push_back(className);
            visitBlock(body, childContainers, true);
          }
          return;
        }

        if (std::strcmp(t, "function_definition") == 0) {
          TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
          TSNode body = ts_node_child_by_field_name(node, "body", 4);
          const std::string fnName = collapseWhitespace(nodeText(nameNode));
          if (fnName.empty()) return;
          TSNode spanNode = effectiveRange(node, rangeOwner);
          const bool declOnly = isStubFile_ && isEllipsisBody(body);
          SymbolInfo sym;
          sym.name = fnName;
          sym.container = joinScope(containers);
          sym.kind = inClass ? SymbolKind::Method : SymbolKind::Function;
          sym.role = declOnly ? SymbolRole::Declaration : SymbolRole::Definition;
          sym.signature = !ts_node_is_null(body)
            ? sliceUntilByte(spanNode, ts_node_start_byte(body), ':')
            : collapseWhitespace(nodeText(spanNode));
          emitSymbol(std::move(sym), spanNode);
          return;
        }

        // Recurse into top-level / class-level conditionals (e.g., if TYPE_CHECKING:),
        // never into function bodies.
        if (std::strcmp(t, "if_statement") == 0 ||
            std::strcmp(t, "elif_clause") == 0 ||
            std::strcmp(t, "else_clause") == 0 ||
            std::strcmp(t, "try_statement") == 0 ||
            std::strcmp(t, "except_clause") == 0 ||
            std::strcmp(t, "block") == 0) {
          const uint32_t n = ts_node_named_child_count(node);
          for (uint32_t i = 0; i < n; ++i) {
            TSNode child = ts_node_named_child(node, i);
            const char *ct = ts_node_type(child);
            if (std::strcmp(ct, "block") == 0 ||
                std::strcmp(ct, "elif_clause") == 0 ||
                std::strcmp(ct, "else_clause") == 0 ||
                std::strcmp(ct, "except_clause") == 0) {
              visitBlock(child, containers, inClass);
            }
          }
        }
      }
    };

    class JsTsExtractor : public ExtractorBase {
    public:
      explicit JsTsExtractor(std::string_view source) : ExtractorBase(source, ".") {}

      std::vector<SymbolInfo> extract(TSNode root) {
        visitContainer(root, {}, false, false);
        assignUniqueSelectors();
        return std::move(symbols_);
      }

    private:
      static bool isFunctionValue(TSNode node) {
        if (ts_node_is_null(node)) return false;
        const char *t = ts_node_type(node);
        return std::strcmp(t, "arrow_function") == 0 ||
               std::strcmp(t, "function_expression") == 0 ||
               std::strcmp(t, "function") == 0 ||
               std::strcmp(t, "generator_function") == 0;
      }

      std::string sliceCallableHeader(TSNode spanNode, TSNode fnNode) const {
        TSNode body = ts_node_child_by_field_name(fnNode, "body", 4);
        if (!ts_node_is_null(body)) {
          return sliceUntilByte(spanNode, ts_node_start_byte(body));
        }
        return cleanDeclarationSignature(spanNode);
      }

      static std::vector<TSNode> collectNamedFields(TSNode parent, const char *typeFilter) {
        std::vector<TSNode> out;
        const uint32_t n = ts_node_named_child_count(parent);
        for (uint32_t i = 0; i < n; ++i) {
          TSNode child = ts_node_named_child(parent, i);
          if (isType(child, typeFilter)) out.push_back(child);
        }
        return out;
      }

      // Compute byte span covering any leading sibling decorator(s) before a member in class_body.
      void emitWithSpanBounds(SymbolInfo sym, TSNode endNode, TSNode startOverride) {
        if (sym.name.empty()) return;
        fillSpan(endNode, src_.size(), sym);
        if (!ts_node_is_null(startOverride)) {
          const uint32_t sb = ts_node_start_byte(startOverride);
          if (sb < sym.start_byte) {
            const TSPoint sp = ts_node_start_point(startOverride);
            sym.start_byte = sb;
            sym.start_line = static_cast<size_t>(sp.row) + 1;
            sym.start_col = static_cast<size_t>(sp.column) + 1;
          }
        }
        sym.qualified_name = sym.container.empty() ? sym.name : (sym.container + separator_ + sym.name);
        symbols_.push_back(std::move(sym));
      }

      void visitContainer(TSNode containerNode, const std::vector<std::string> &containers, bool inClass, bool inAmbient) {
        TSNode pendingDecorator{};
        const uint32_t n = ts_node_named_child_count(containerNode);
        for (uint32_t i = 0; i < n; ++i) {
          TSNode child = ts_node_named_child(containerNode, i);
          if (isType(child, "decorator")) {
            if (ts_node_is_null(pendingDecorator)) pendingDecorator = child;
            continue;
          }
          visitItem(child, pendingDecorator, containers, inClass, inAmbient);
          pendingDecorator = {};
        }
      }

      void visitItem(TSNode node, TSNode rangeOwner, const std::vector<std::string> &containers, bool inClass, bool inAmbient) {
        if (ts_node_is_null(node) || !ts_node_is_named(node)) return;
        const char *t = ts_node_type(node);

        if (std::strcmp(t, "export_statement") == 0) {
          TSNode outer = effectiveRange(node, rangeOwner);
          TSNode decl = ts_node_child_by_field_name(node, "declaration", 11);
          if (!ts_node_is_null(decl)) {
            visitItem(decl, outer, containers, inClass, inAmbient);
            return;
          }
          TSNode val = ts_node_child_by_field_name(node, "value", 5);
          if (isFunctionValue(val)) {
            TSNode nameNode = ts_node_child_by_field_name(val, "name", 4);
            std::string fnName = collapseWhitespace(nodeText(nameNode));
            if (fnName.empty()) fnName = "default";
            SymbolInfo sym;
            sym.name = std::move(fnName);
            sym.container = joinScope(containers);
            sym.kind = SymbolKind::Function;
            sym.role = SymbolRole::Definition;
            sym.signature = sliceCallableHeader(outer, val);
            emitSymbol(std::move(sym), outer);
          }
          return;
        }

        if (std::strcmp(t, "ambient_declaration") == 0) {
          TSNode outer = effectiveRange(node, rangeOwner);
          const uint32_t n = ts_node_named_child_count(node);
          for (uint32_t i = 0; i < n; ++i) {
            visitItem(ts_node_named_child(node, i), outer, containers, inClass, true);
          }
          return;
        }

        if (std::strcmp(t, "internal_module") == 0 || std::strcmp(t, "module") == 0) {
          TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
          TSNode body = ts_node_child_by_field_name(node, "body", 4);
          const std::string nsName = collapseWhitespace(nodeText(nameNode));
          if (nsName.empty()) return;
          TSNode spanNode = effectiveRange(node, rangeOwner);
          SymbolInfo sym;
          sym.name = nsName;
          sym.container = joinScope(containers);
          sym.kind = SymbolKind::Namespace;
          sym.role = (!inAmbient && !ts_node_is_null(body)) ? SymbolRole::Definition : SymbolRole::Declaration;
          sym.signature = !ts_node_is_null(body)
            ? sliceUntilByte(spanNode, ts_node_start_byte(body))
            : cleanDeclarationSignature(spanNode);
          emitSymbol(std::move(sym), spanNode);
          if (!ts_node_is_null(body)) {
            auto next = containers;
            next.push_back(nsName);
            visitContainer(body, next, false, inAmbient);
          }
          return;
        }

        if (std::strcmp(t, "class_declaration") == 0 ||
            std::strcmp(t, "abstract_class_declaration") == 0 ||
            std::strcmp(t, "interface_declaration") == 0) {
          TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
          TSNode body = ts_node_child_by_field_name(node, "body", 4);
          const std::string typeName = collapseWhitespace(nodeText(nameNode));
          if (typeName.empty()) return;
          TSNode spanNode = effectiveRange(node, rangeOwner);
          const bool isIface = std::strcmp(t, "interface_declaration") == 0;
          SymbolInfo sym;
          sym.name = typeName;
          sym.container = joinScope(containers);
          sym.kind = isIface ? SymbolKind::Interface : SymbolKind::Class;
          sym.role = (!inAmbient && !isIface && !ts_node_is_null(body))
            ? SymbolRole::Definition
            : SymbolRole::Declaration;
          sym.signature = !ts_node_is_null(body)
            ? sliceUntilByte(spanNode, ts_node_start_byte(body))
            : cleanDeclarationSignature(spanNode);
          emitSymbol(std::move(sym), spanNode);
          if (!ts_node_is_null(body)) {
            auto next = containers;
            next.push_back(typeName);
            visitContainer(body, next, true, inAmbient || isIface);
          }
          return;
        }

        if (std::strcmp(t, "type_alias_declaration") == 0) {
          TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
          const std::string aliasName = collapseWhitespace(nodeText(nameNode));
          if (aliasName.empty()) return;
          TSNode spanNode = effectiveRange(node, rangeOwner);
          SymbolInfo sym;
          sym.name = aliasName;
          sym.container = joinScope(containers);
          sym.kind = SymbolKind::TypeAlias;
          sym.role = SymbolRole::Definition;
          sym.signature = cleanDeclarationSignature(spanNode);
          emitSymbol(std::move(sym), spanNode);
          return;
        }

        if (std::strcmp(t, "enum_declaration") == 0) {
          TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
          TSNode body = ts_node_child_by_field_name(node, "body", 4);
          const std::string enumName = collapseWhitespace(nodeText(nameNode));
          if (enumName.empty()) return;
          TSNode spanNode = effectiveRange(node, rangeOwner);
          SymbolInfo sym;
          sym.name = enumName;
          sym.container = joinScope(containers);
          sym.kind = SymbolKind::Enum;
          sym.role = (!inAmbient && !ts_node_is_null(body)) ? SymbolRole::Definition : SymbolRole::Declaration;
          sym.signature = !ts_node_is_null(body)
            ? sliceUntilByte(spanNode, ts_node_start_byte(body))
            : cleanDeclarationSignature(spanNode);
          emitSymbol(std::move(sym), spanNode);
          return;
        }

        if (std::strcmp(t, "function_declaration") == 0 ||
            std::strcmp(t, "generator_function_declaration") == 0 ||
            std::strcmp(t, "function_signature") == 0) {
          TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
          const std::string fnName = collapseWhitespace(nodeText(nameNode));
          if (fnName.empty()) return;
          TSNode body = ts_node_child_by_field_name(node, "body", 4);
          const bool hasBody = !ts_node_is_null(body) && std::strcmp(t, "function_signature") != 0;
          TSNode spanNode = effectiveRange(node, rangeOwner);
          SymbolInfo sym;
          sym.name = fnName;
          sym.container = joinScope(containers);
          sym.kind = SymbolKind::Function;
          sym.role = (!inAmbient && hasBody) ? SymbolRole::Definition : SymbolRole::Declaration;
          sym.signature = hasBody
            ? sliceUntilByte(spanNode, ts_node_start_byte(body))
            : cleanDeclarationSignature(spanNode);
          emitSymbol(std::move(sym), spanNode);
          return;
        }

        if (std::strcmp(t, "method_definition") == 0 ||
            std::strcmp(t, "method_signature") == 0 ||
            std::strcmp(t, "abstract_method_signature") == 0) {
          TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
          const std::string methodName = collapseWhitespace(nodeText(nameNode));
          if (methodName.empty()) return;
          TSNode body = ts_node_child_by_field_name(node, "body", 4);
          const bool isSig = std::strcmp(t, "method_definition") != 0 || ts_node_is_null(body);
          SymbolInfo sym;
          sym.name = methodName;
          sym.container = joinScope(containers);
          sym.kind = (methodName == "constructor") ? SymbolKind::Constructor : SymbolKind::Method;
          sym.role = (!inAmbient && !isSig) ? SymbolRole::Definition : SymbolRole::Declaration;
          TSNode sigStart = ts_node_is_null(rangeOwner) ? node : rangeOwner;
          sym.signature = !isSig
            ? sliceUntilByte(sigStart, ts_node_start_byte(body))
            : cleanDeclarationSignature(sigStart);
          emitWithSpanBounds(std::move(sym), node, rangeOwner);
          return;
        }

        if (inClass && (std::strcmp(t, "public_field_definition") == 0 || std::strcmp(t, "field_definition") == 0)) {
          TSNode val = ts_node_child_by_field_name(node, "value", 5);
          if (!isFunctionValue(val)) return;
          TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
          if (ts_node_is_null(nameNode)) nameNode = ts_node_child_by_field_name(node, "property", 8);
          const std::string propName = collapseWhitespace(nodeText(nameNode));
          if (propName.empty()) return;
          TSNode sigStart = ts_node_is_null(rangeOwner) ? node : rangeOwner;
          SymbolInfo sym;
          sym.name = propName;
          sym.container = joinScope(containers);
          sym.kind = SymbolKind::Method;
          sym.role = SymbolRole::Definition;
          sym.signature = sliceCallableHeader(sigStart, val);
          emitWithSpanBounds(std::move(sym), node, rangeOwner);
          return;
        }

        if (std::strcmp(t, "lexical_declaration") == 0 || std::strcmp(t, "variable_declaration") == 0) {
          const auto decls = collectNamedFields(node, "variable_declarator");
          if (decls.empty()) return;
          const bool singleDecl = decls.size() == 1;
          for (TSNode d : decls) {
            TSNode val = ts_node_child_by_field_name(d, "value", 5);
            if (!isFunctionValue(val)) continue;
            TSNode nameNode = ts_node_child_by_field_name(d, "name", 4);
            const std::string varName = collapseWhitespace(nodeText(nameNode));
            if (varName.empty()) continue;
            TSNode spanNode = singleDecl ? effectiveRange(node, rangeOwner) : d;
            SymbolInfo sym;
            sym.name = varName;
            sym.container = joinScope(containers);
            sym.kind = inClass ? SymbolKind::Method : SymbolKind::Function;
            sym.role = SymbolRole::Definition;
            sym.signature = sliceCallableHeader(spanNode, val);
            emitSymbol(std::move(sym), spanNode);
          }
          return;
        }
      }
    };

    std::vector<SymbolInfo> extractOutlineSymbols(Language lang, std::string_view key, TSNode root, std::string_view content) {
      switch (lang) {
        case Language::C:
        case Language::Cpp: {
          CppExtractor extractor(content);
          return extractor.extract(root);
        }
        case Language::Python: {
          const std::string lowerKey = toLowerAscii(key);
          PythonExtractor extractor(content, lowerKey.ends_with(".pyi"));
          return extractor.extract(root);
        }
        case Language::JavaScript:
        case Language::TypeScript:
        case Language::Tsx: {
          JsTsExtractor extractor(content);
          return extractor.extract(root);
        }
        case Language::Unsupported:
          return {};
      }
      return {};
    }

    struct CursorGuard {
      TSTreeCursor raw;
      explicit CursorGuard(TSNode root) : raw(ts_tree_cursor_new(root)) {}
      ~CursorGuard() { ts_tree_cursor_delete(&raw); }
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

    std::string_view rawNodeSlice(TSNode node, std::string_view src) {
      if (ts_node_is_null(node)) return {};
      const uint32_t s = ts_node_start_byte(node);
      const uint32_t e = ts_node_end_byte(node);
      if (s > e || e > src.size()) return {};
      return src.substr(s, e - s);
    }

    bool nodeTypeIs(TSNode node, const char *typeName) {
      return !ts_node_is_null(node) && std::strcmp(ts_node_type(node), typeName) == 0;
    }

    std::string stripQuotedLiteral(std::string_view raw) {
      raw = trimView(raw);
      if (raw.size() >= 2) {
        if ((raw.front() == '"' && raw.back() == '"') ||
            (raw.front() == '\'' && raw.back() == '\'') ||
            (raw.front() == '<' && raw.back() == '>')) {
          return std::string(raw.substr(1, raw.size() - 2));
        }
      }
      const auto q = raw.find('"');
      if (q != std::string_view::npos && raw.back() == '"' && q + 1 < raw.size()) {
        return std::string(raw.substr(q + 1, raw.size() - q - 2));
      }
      return std::string(raw);
    }

    bool isAllDots(std::string_view sv) {
      return !sv.empty() && std::all_of(sv.begin(), sv.end(), [](char c) { return c == '.'; });
    }

    std::vector<ImportEdge> extractImportsFromRoot(Language lang, TSNode root, std::string_view src) {
      std::vector<ImportEdge> out;
      if (lang == Language::Unsupported || ts_node_is_null(root)) return out;

      std::set<std::tuple<size_t, std::string, std::string>> seen;
      auto pushEdge = [&](std::string spec, std::string kind, size_t line, bool isSystem) {
        spec = std::string(trimView(spec));
        if (spec.empty()) return;
        if (seen.emplace(line, kind, spec).second) {
          out.push_back({std::move(spec), std::move(kind), line, isSystem});
        }
      };

      walkAst(root, [&](TSNode node) -> bool {
        if (!ts_node_is_named(node)) return false;
        const char *t = ts_node_type(node);
        if (std::strcmp(t, "comment") == 0) return false;
        const size_t line = static_cast<size_t>(ts_node_start_point(node).row) + 1;

        if (lang == Language::C || lang == Language::Cpp) {
          if (std::strcmp(t, "preproc_include") == 0) {
            TSNode p = ts_node_child_by_field_name(node, "path", 4);
            if (!ts_node_is_null(p)) {
              const char *pt = ts_node_type(p);
              if (std::strcmp(pt, "string_literal") == 0) {
                pushEdge(stripQuotedLiteral(rawNodeSlice(p, src)), "include", line, false);
              } else if (std::strcmp(pt, "system_lib_string") == 0) {
                pushEdge(stripQuotedLiteral(rawNodeSlice(p, src)), "include", line, true);
              }
            }
            return false;
          }
          return true;
        }

        if (lang == Language::Python) {
          if (std::strcmp(t, "import_statement") == 0) {
            const uint32_t n = ts_node_named_child_count(node);
            for (uint32_t i = 0; i < n; ++i) {
              TSNode child = ts_node_named_child(node, i);
              if (nodeTypeIs(child, "dotted_name")) {
                pushEdge(std::string(rawNodeSlice(child, src)), "import", line, false);
              } else if (nodeTypeIs(child, "aliased_import")) {
                TSNode name = ts_node_child_by_field_name(child, "name", 4);
                if (!ts_node_is_null(name)) {
                  pushEdge(std::string(rawNodeSlice(name, src)), "import", line, false);
                }
              }
            }
            return false;
          }
          if (std::strcmp(t, "import_from_statement") == 0) {
            TSNode modNode = ts_node_child_by_field_name(node, "module_name", 11);
            const std::string_view modText = trimView(rawNodeSlice(modNode, src));
            if (modText.empty()) return false;
            if (isAllDots(modText)) {
              bool emitted = false;
              const uint32_t n = ts_node_named_child_count(node);
              for (uint32_t i = 0; i < n; ++i) {
                TSNode child = ts_node_named_child(node, i);
                if (ts_node_eq(child, modNode)) continue;
                if (nodeTypeIs(child, "dotted_name")) {
                  pushEdge(std::string(modText) + std::string(rawNodeSlice(child, src)), "from_import", line, false);
                  emitted = true;
                } else if (nodeTypeIs(child, "aliased_import")) {
                  TSNode name = ts_node_child_by_field_name(child, "name", 4);
                  if (!ts_node_is_null(name)) {
                    pushEdge(std::string(modText) + std::string(rawNodeSlice(name, src)), "from_import", line, false);
                    emitted = true;
                  }
                }
              }
              if (!emitted) {
                pushEdge(std::string(modText), "from_import", line, false);
              }
            } else {
              pushEdge(std::string(modText), "from_import", line, false);
            }
            return false;
          }
          return true;
        }

        if (lang == Language::JavaScript || lang == Language::TypeScript || lang == Language::Tsx) {
          if (std::strcmp(t, "import_statement") == 0) {
            TSNode s = ts_node_child_by_field_name(node, "source", 6);
            if (nodeTypeIs(s, "string")) {
              pushEdge(stripQuotedLiteral(rawNodeSlice(s, src)), "import", line, false);
            }
            return false;
          }
          if (std::strcmp(t, "export_statement") == 0) {
            TSNode s = ts_node_child_by_field_name(node, "source", 6);
            if (!ts_node_is_null(s) && nodeTypeIs(s, "string")) {
              pushEdge(stripQuotedLiteral(rawNodeSlice(s, src)), "reexport", line, false);
            }
            return true;
          }
          if (std::strcmp(t, "call_expression") == 0) {
            TSNode fn = ts_node_child_by_field_name(node, "function", 8);
            std::string kind;
            if (nodeTypeIs(fn, "identifier") && rawNodeSlice(fn, src) == "require") {
              kind = "require";
            } else if (nodeTypeIs(fn, "import")) {
              kind = "dynamic_import";
            }
            if (!kind.empty()) {
              TSNode args = ts_node_child_by_field_name(node, "arguments", 9);
              if (!ts_node_is_null(args) && ts_node_named_child_count(args) >= 1) {
                TSNode arg0 = ts_node_named_child(args, 0);
                if (nodeTypeIs(arg0, "string")) {
                  pushEdge(stripQuotedLiteral(rawNodeSlice(arg0, src)), kind, line, false);
                }
              }
            }
          }
          return true;
        }
        return true;
      });
      return out;
    }

    size_t sharedDirectoryPrefixScore(std::string_view a, std::string_view b) {
      size_t score = 0;
      const auto aSlash = a.rfind('/');
      const auto bSlash = b.rfind('/');
      const std::string_view ad = aSlash == std::string_view::npos ? std::string_view{} : a.substr(0, aSlash);
      const std::string_view bd = bSlash == std::string_view::npos ? std::string_view{} : b.substr(0, bSlash);
      size_t i = 0;
      while (i < ad.size() && i < bd.size() && ad[i] == bd[i]) {
        if (ad[i] == '/') ++score;
        ++i;
      }
      if (i > 0 && (i == ad.size() || ad[i] == '/') && (i == bd.size() || bd[i] == '/')) ++score;
      return score;
    }

    std::string joinDirAndSpec(std::string_view dir, std::string_view spec) {
      std::string combined;
      if (!dir.empty()) {
        combined = std::string(dir);
        if (combined.back() != '/') combined.push_back('/');
      }
      combined += spec;
      return utils::normalizePathKey(combined);
    }

    std::pair<std::string_view, std::string_view> splitQualifiedQuery(std::string_view query) {
      const auto cppSep = query.rfind("::");
      const auto dotSep = query.rfind('.');
      size_t sepPos = std::string_view::npos;
      size_t sepLen = 0;
      if (cppSep != std::string_view::npos && (dotSep == std::string_view::npos || cppSep > dotSep)) {
        sepPos = cppSep;
        sepLen = 2;
      } else if (dotSep != std::string_view::npos) {
        sepPos = dotSep;
        sepLen = 1;
      }
      if (sepPos == std::string_view::npos) return {{}, trimView(query)};
      return {trimView(query.substr(0, sepPos)), trimView(query.substr(sepPos + sepLen))};
    }

    bool isLeafIdentifierType(const char *t) {
      return std::strcmp(t, "identifier") == 0 ||
             std::strcmp(t, "field_identifier") == 0 ||
             std::strcmp(t, "type_identifier") == 0 ||
             std::strcmp(t, "namespace_identifier") == 0 ||
             std::strcmp(t, "property_identifier") == 0 ||
             std::strcmp(t, "shorthand_property_identifier") == 0 ||
             std::strcmp(t, "shorthand_property_identifier_pattern") == 0 ||
             std::strcmp(t, "jsx_identifier") == 0;
    }

    bool hasAncestorType(TSNode node, std::initializer_list<const char *> types) {
      for (TSNode p = ts_node_parent(node); !ts_node_is_null(p); p = ts_node_parent(p)) {
        const char *pt = ts_node_type(p);
        for (const char *target : types) {
          if (std::strcmp(pt, target) == 0) return true;
        }
      }
      return false;
    }

    bool isPythonStubEllipsis(TSNode body) {
      if (ts_node_is_null(body)) return false;
      uint32_t nonComment = 0;
      TSNode single{};
      const uint32_t n = ts_node_named_child_count(body);
      for (uint32_t i = 0; i < n; ++i) {
        TSNode child = ts_node_named_child(body, i);
        if (nodeTypeIs(child, "comment")) continue;
        ++nonComment;
        single = child;
      }
      if (nonComment != 1 || !nodeTypeIs(single, "expression_statement")) return false;
      const uint32_t sn = ts_node_named_child_count(single);
      for (uint32_t i = 0; i < sn; ++i) {
        if (nodeTypeIs(ts_node_named_child(single, i), "ellipsis")) return true;
      }
      return false;
    }

    std::string classifyDeclarationRole(Language lang, TSNode idNode) {
      TSNode cur = idNode;
      TSNode parent = ts_node_parent(cur);
      if (ts_node_is_null(parent)) return {};

      if (lang == Language::C || lang == Language::Cpp) {
        if (nodeTypeIs(parent, "destructor_name")) {
          cur = parent;
          parent = ts_node_parent(cur);
        }
        // Climb declarator / qualified name wrappers strictly via declarator/name fields.
        TSNode climb = cur;
        TSNode p = parent;
        while (!ts_node_is_null(p)) {
          const char *pt = ts_node_type(p);
          const bool viaName = (std::strcmp(pt, "qualified_identifier") == 0 ||
                                std::strcmp(pt, "scoped_identifier") == 0 ||
                                std::strcmp(pt, "template_function") == 0) &&
                               ts_node_eq(ts_node_child_by_field_name(p, "name", 4), climb);
          const bool viaDecl = (std::strcmp(pt, "parenthesized_declarator") == 0 ||
                                std::strcmp(pt, "pointer_declarator") == 0 ||
                                std::strcmp(pt, "reference_declarator") == 0 ||
                                std::strcmp(pt, "attributed_declarator") == 0) &&
                               ts_node_eq(ts_node_child_by_field_name(p, "declarator", 10), climb);
          if (std::strcmp(pt, "function_declarator") == 0 &&
              ts_node_eq(ts_node_child_by_field_name(p, "declarator", 10), climb)) {
            TSNode fp = ts_node_parent(p);
            while (!ts_node_is_null(fp)) {
              const char *fpt = ts_node_type(fp);
              if (std::strcmp(fpt, "pointer_declarator") == 0 ||
                  std::strcmp(fpt, "reference_declarator") == 0 ||
                  std::strcmp(fpt, "parenthesized_declarator") == 0) {
                fp = ts_node_parent(fp);
                continue;
              }
              if (std::strcmp(fpt, "function_definition") == 0) return "definition";
              if (std::strcmp(fpt, "declaration") == 0 || std::strcmp(fpt, "field_declaration") == 0)
                return "declaration";
              break;
            }
          }
          if (std::strcmp(pt, "type_definition") == 0 &&
              ts_node_eq(ts_node_child_by_field_name(p, "declarator", 10), climb)) {
            return "definition";
          }
          if (!viaName && !viaDecl) break;
          climb = p;
          p = ts_node_parent(climb);
        }

        const char *pt = ts_node_type(parent);
        if ((std::strcmp(pt, "class_specifier") == 0 ||
             std::strcmp(pt, "struct_specifier") == 0 ||
             std::strcmp(pt, "union_specifier") == 0) &&
            ts_node_eq(ts_node_child_by_field_name(parent, "name", 4), cur)) {
          return ts_node_is_null(ts_node_child_by_field_name(parent, "body", 4)) ? "declaration" : "definition";
        }
        if (std::strcmp(pt, "enum_specifier") == 0 &&
            ts_node_eq(ts_node_child_by_field_name(parent, "name", 4), cur)) {
          return ts_node_is_null(ts_node_child_by_field_name(parent, "body", 4)) ? "declaration" : "definition";
        }
        if ((std::strcmp(pt, "namespace_definition") == 0 ||
             std::strcmp(pt, "alias_declaration") == 0 ||
             std::strcmp(pt, "concept_definition") == 0) &&
            ts_node_eq(ts_node_child_by_field_name(parent, "name", 4), cur)) {
          return "definition";
        }
        return {};
      }

      if (lang == Language::Python) {
        const char *pt = ts_node_type(parent);
        if ((std::strcmp(pt, "function_definition") == 0 || std::strcmp(pt, "class_definition") == 0) &&
            ts_node_eq(ts_node_child_by_field_name(parent, "name", 4), cur)) {
          TSNode body = ts_node_child_by_field_name(parent, "body", 4);
          return isPythonStubEllipsis(body) ? "declaration" : "definition";
        }
        return {};
      }

      if (lang == Language::JavaScript || lang == Language::TypeScript || lang == Language::Tsx) {
        const char *pt = ts_node_type(parent);
        const bool inAmbient = hasAncestorType(cur, {"ambient_declaration"});
        if ((std::strcmp(pt, "function_declaration") == 0 ||
             std::strcmp(pt, "generator_function_declaration") == 0 ||
             std::strcmp(pt, "class_declaration") == 0 ||
             std::strcmp(pt, "abstract_class_declaration") == 0 ||
             std::strcmp(pt, "enum_declaration") == 0 ||
             std::strcmp(pt, "internal_module") == 0 ||
             std::strcmp(pt, "module") == 0) &&
            ts_node_eq(ts_node_child_by_field_name(parent, "name", 4), cur)) {
          TSNode body = ts_node_child_by_field_name(parent, "body", 4);
          return (!inAmbient && !ts_node_is_null(body)) ? "definition" : "declaration";
        }
        if ((std::strcmp(pt, "interface_declaration") == 0 ||
             std::strcmp(pt, "function_signature") == 0 ||
             std::strcmp(pt, "method_signature") == 0 ||
             std::strcmp(pt, "abstract_method_signature") == 0) &&
            ts_node_eq(ts_node_child_by_field_name(parent, "name", 4), cur)) {
          return "declaration";
        }
        if ((std::strcmp(pt, "method_definition") == 0 || std::strcmp(pt, "type_alias_declaration") == 0) &&
            ts_node_eq(ts_node_child_by_field_name(parent, "name", 4), cur)) {
          if (std::strcmp(pt, "type_alias_declaration") == 0) return "definition";
          TSNode body = ts_node_child_by_field_name(parent, "body", 4);
          return (!inAmbient && !ts_node_is_null(body)) ? "definition" : "declaration";
        }
        if (std::strcmp(pt, "variable_declarator") == 0 &&
            ts_node_eq(ts_node_child_by_field_name(parent, "name", 4), cur)) {
          TSNode val = ts_node_child_by_field_name(parent, "value", 5);
          if (nodeTypeIs(val, "arrow_function") ||
              nodeTypeIs(val, "function_expression") ||
              nodeTypeIs(val, "generator_function")) {
            return "definition";
          }
        }
        return {};
      }

      return {};
    }

    std::string classifyUsageKind(TSNode node) {
      TSNode parent = ts_node_parent(node);
      if (hasAncestorType(node, {"preproc_include", "import_statement", "import_from_statement"})) {
        return "import";
      }
      if (!ts_node_is_null(parent) && nodeTypeIs(parent, "export_specifier")) {
        return "import";
      }
      if (!ts_node_is_null(parent) &&
          (nodeTypeIs(parent, "jsx_opening_element") || nodeTypeIs(parent, "jsx_self_closing_element"))) {
        TSNode jsxName = ts_node_child_by_field_name(parent, "name", 4);
        if (ts_node_eq(jsxName, node)) return "call";
      }

      TSNode cur = node;
      TSNode p = parent;
      while (!ts_node_is_null(p)) {
        const char *pt = ts_node_type(p);
        if (std::strcmp(pt, "call_expression") == 0 || std::strcmp(pt, "call") == 0) {
          if (ts_node_eq(ts_node_child_by_field_name(p, "function", 8), cur)) return "call";
          break;
        }
        if (std::strcmp(pt, "new_expression") == 0) {
          if (ts_node_eq(ts_node_child_by_field_name(p, "constructor", 11), cur)) return "call";
          break;
        }
        const bool isChain =
          ((std::strcmp(pt, "qualified_identifier") == 0 ||
            std::strcmp(pt, "scoped_identifier") == 0 ||
            std::strcmp(pt, "template_function") == 0) &&
           ts_node_eq(ts_node_child_by_field_name(p, "name", 4), cur)) ||
          (std::strcmp(pt, "field_expression") == 0 &&
           ts_node_eq(ts_node_child_by_field_name(p, "field", 5), cur)) ||
          (std::strcmp(pt, "member_expression") == 0 &&
           ts_node_eq(ts_node_child_by_field_name(p, "property", 8), cur)) ||
          (std::strcmp(pt, "attribute") == 0 &&
           ts_node_eq(ts_node_child_by_field_name(p, "attribute", 9), cur));
        if (!isChain) break;
        cur = p;
        p = ts_node_parent(cur);
      }

      if (nodeTypeIs(node, "type_identifier") ||
          hasAncestorType(node, {"base_class_clause", "class_heritage", "extends_clause",
                                 "implements_clause", "type_annotation", "type_arguments",
                                 "type_parameter_declaration"})) {
        return "type";
      }
      return "reference";
    }

    std::string lineSnippetAt(std::string_view src, size_t line1) {
      if (line1 == 0) return {};
      size_t pos = 0;
      size_t cur = 1;
      while (cur < line1 && pos < src.size()) {
        const auto nl = src.find('\n', pos);
        if (nl == std::string_view::npos) return {};
        pos = nl + 1;
        ++cur;
      }
      if (pos >= src.size()) return {};
      const auto end = src.find('\n', pos);
      auto line = src.substr(pos, end == std::string_view::npos ? src.size() - pos : end - pos);
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      return collapseWhitespace(line, 160);
    }

    bool matchesContainerSuffix(std::string_view full, std::string_view qualifier) {
      if (full == qualifier) return true;
      if (full.size() <= qualifier.size()) return false;
      if (full.ends_with(qualifier)) {
        const size_t pos = full.size() - qualifier.size();
        if (pos >= 2 && full[pos - 2] == ':' && full[pos - 1] == ':') return true;
        if (pos >= 1 && full[pos - 1] == '.') return true;
      }
      return full.starts_with(std::string(qualifier) + "::") || full.starts_with(std::string(qualifier) + ".");
    }

  } // namespace

  Language detectLanguage(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    const auto filename = (slash == std::string_view::npos) ? path : path.substr(slash + 1);
    const auto dot = filename.rfind('.');
    if (dot == std::string_view::npos) return Language::Unsupported;
    const std::string ext = toLowerAscii(filename.substr(dot));
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

  std::string sha256Hex(std::string_view data) {
    static constexpr std::array<uint32_t, 64> k = {
      0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
      0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
      0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
      0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
      0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
      0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
      0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
      0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };
    auto rotr = [](uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); };

    std::array<uint32_t, 8> state = {
      0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };

    const uint64_t totalBits = static_cast<uint64_t>(data.size()) * 8ULL;
    const size_t rem = data.size() % 64;
    const size_t paddedSize = data.size() + (rem < 56 ? (56 - rem) : (120 - rem)) + 8;

    auto readByte = [&](size_t idx) -> uint8_t {
      if (idx < data.size()) return static_cast<uint8_t>(data[idx]);
      if (idx == data.size()) return 0x80u;
      if (idx >= paddedSize - 8) {
        const size_t shift = (paddedSize - 1 - idx) * 8;
        return static_cast<uint8_t>((totalBits >> shift) & 0xffu);
      }
      return 0u;
    };

    std::array<uint32_t, 64> w{};
    for (size_t offset = 0; offset < paddedSize; offset += 64) {
      for (size_t i = 0; i < 16; ++i) {
        const size_t b = offset + i * 4;
        w[i] = (static_cast<uint32_t>(readByte(b)) << 24) |
               (static_cast<uint32_t>(readByte(b + 1)) << 16) |
               (static_cast<uint32_t>(readByte(b + 2)) << 8) |
               static_cast<uint32_t>(readByte(b + 3));
      }
      for (size_t i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
      }
      uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
      uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
      for (size_t i = 0; i < 64; ++i) {
        const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t temp1 = h + S1 + ch + k[i] + w[i];
        const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = S0 + maj;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
      }
      state[0] += a; state[1] += b; state[2] += c; state[3] += d;
      state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }

    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.resize(64);
    for (size_t i = 0; i < 8; ++i) {
      for (size_t j = 0; j < 8; ++j) {
        out[i * 8 + j] = hex[(state[i] >> ((7 - j) * 4)) & 0xfu];
      }
    }
    return out;
  }

  SymbolService::SymbolService(size_t maxCacheEntries)
    : maxCacheEntries_((std::max)(size_t(1), maxCacheEntries)) {
    ParserHandle parser;
    for (const Language lang : {Language::C, Language::Cpp, Language::Python,
                                Language::JavaScript, Language::TypeScript, Language::Tsx}) {
      ensureLanguage(parser.raw, lang);
    }
  }

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
    ParserHandle parser;
    ensureLanguage(parser.raw, lang);
    TreeHandle tree(ts_parser_parse_string(parser.raw, nullptr, content.data(), static_cast<uint32_t>(content.size())));
    if (!tree.raw) {
      throw std::runtime_error("Tree-sitter failed to parse " + std::string(languageName(lang)) + " source");
    }

    FileOutline outline;
    outline.path = key;
    outline.language = lang;
    outline.content_sha256 = digest;
    outline.byte_size = content.size();
    const TSNode root = ts_tree_root_node(tree.raw);
    outline.symbols = extractOutlineSymbols(lang, key, root, content);
    outline.imports = extractImportsFromRoot(lang, root, content);

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

  size_t SymbolService::cachedFileCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return index_.size();
  }

  void SymbolService::clearCache() {
    std::lock_guard<std::mutex> lock(mutex_);
    lru_.clear();
    index_.clear();
  }

  std::vector<ImportEdge> extractImportsForContent(Language lang, std::string_view content) {
    if (lang == Language::Unsupported) return {};
    ParserHandle parser;
    ensureLanguage(parser.raw, lang);
    TreeHandle tree(ts_parser_parse_string(parser.raw, nullptr, content.data(), static_cast<uint32_t>(content.size())));
    if (!tree.raw) return {};
    return extractImportsFromRoot(lang, ts_tree_root_node(tree.raw), content);
  }

  std::vector<ResolvedDependency> resolveImports(std::string_view sourcePath, Language lang,
      const std::vector<ImportEdge> &imports, const std::vector<std::string> &candidatePaths) {
    std::vector<ResolvedDependency> out;
    if (imports.empty()) return out;

    const std::string srcKey = utils::normalizePathKey(sourcePath);
    const auto slash = srcKey.rfind('/');
    const std::string srcDir = (slash == std::string::npos) ? std::string{} : srcKey.substr(0, slash);

    std::unordered_map<std::string, std::string> byExact;
    std::unordered_map<std::string, std::string> byLower;
    std::vector<std::string> keys;
    byExact.reserve(candidatePaths.size());
    byLower.reserve(candidatePaths.size());
    keys.reserve(candidatePaths.size());
    for (const auto &raw : candidatePaths) {
      std::string k = utils::normalizePathKey(raw);
      if (k.empty()) continue;
      if (byExact.emplace(k, k).second) {
        keys.push_back(k);
        byLower.emplace(toLowerAscii(k), k);
      }
    }
    std::sort(keys.begin(), keys.end());

    auto lookupExact = [&](const std::string &k) -> std::optional<std::string> {
      if (const auto it = byExact.find(k); it != byExact.end()) return it->second;
#ifdef _WIN32
      if (const auto it = byLower.find(toLowerAscii(k)); it != byLower.end()) return it->second;
#endif
      return std::nullopt;
    };

    auto lookupBestSuffix = [&](const std::vector<std::string> &suffixes) -> std::optional<std::string> {
      std::string best;
      size_t bestScore = 0;
      for (const auto &cand : keys) {
        if (cand == srcKey) continue;
        bool matched = false;
        for (const auto &suf : suffixes) {
          if (cand == suf || (cand.size() > suf.size() && cand[cand.size() - suf.size() - 1] == '/' &&
              cand.ends_with(suf))) {
            matched = true;
            break;
          }
#ifdef _WIN32
          const std::string lcCand = toLowerAscii(cand);
          const std::string lcSuf = toLowerAscii(suf);
          if (lcCand == lcSuf || (lcCand.size() > lcSuf.size() && lcCand[lcCand.size() - lcSuf.size() - 1] == '/' &&
              lcCand.ends_with(lcSuf))) {
            matched = true;
            break;
          }
#endif
        }
        if (!matched) continue;
        const size_t score = sharedDirectoryPrefixScore(srcKey, cand) + 1;
        if (best.empty() || score > bestScore || (score == bestScore && cand < best)) {
          best = cand;
          bestScore = score;
        }
      }
      if (best.empty()) return std::nullopt;
      return best;
    };

    auto probeJsTs = [&](const std::string &base) -> std::optional<std::string> {
      if (auto hit = lookupExact(base)) return hit;
      static constexpr const char *kJsExts[] = { ".js", ".mjs", ".cjs", ".jsx" };
      static constexpr const char *kTryExts[] = { ".ts", ".tsx", ".mts", ".cts", ".js", ".jsx", ".mjs", ".cjs" };
      for (const char *ext : kJsExts) {
        const size_t elen = std::strlen(ext);
        if (base.size() > elen && toLowerAscii(std::string_view(base).substr(base.size() - elen)) == ext) {
          const std::string stem = base.substr(0, base.size() - elen);
          for (const char *tryExt : kTryExts) {
            if (auto hit = lookupExact(stem + tryExt)) return hit;
          }
          break;
        }
      }
      for (const char *tryExt : kTryExts) {
        if (auto hit = lookupExact(base + tryExt)) return hit;
      }
      static constexpr const char *kIndexExts[] = { "/index.ts", "/index.tsx", "/index.mts", "/index.cts", "/index.js", "/index.jsx", "/index.mjs", "/index.cjs" };
      for (const char *idxExt : kIndexExts) {
        if (auto hit = lookupExact(base + idxExt)) return hit;
      }
      return std::nullopt;
    };

    out.reserve(imports.size());
    for (const auto &edge : imports) {
      ResolvedDependency dep;
      dep.source_path = srcKey;
      dep.target_spec = edge.raw_specifier;
      dep.kind = edge.kind;
      dep.line = edge.line;
      dep.is_system = edge.is_system;

      std::string spec = edge.raw_specifier;
      std::replace(spec.begin(), spec.end(), '\\', '/');

      std::optional<std::string> resolved;
      if (lang == Language::C || lang == Language::Cpp) {
        while (spec.starts_with("./")) spec.erase(0, 2);
        if (!spec.empty()) {
          if (!edge.is_system) {
            resolved = lookupExact(joinDirAndSpec(srcDir, spec));
          }
          if (!resolved && spec.find('/') != std::string::npos) {
            resolved = lookupBestSuffix({spec});
          } else if (!resolved && !edge.is_system) {
            resolved = lookupBestSuffix({spec});
          }
        }
      } else if (lang == Language::JavaScript || lang == Language::TypeScript || lang == Language::Tsx) {
        if (spec == "." || spec == ".." || spec.starts_with("./") || spec.starts_with("../")) {
          resolved = probeJsTs(joinDirAndSpec(srcDir, spec));
        } else if (spec.find('/') != std::string::npos && !spec.starts_with("@")) {
          static constexpr const char *kTryExts[] = { "", ".ts", ".tsx", ".mts", ".cts", ".js", ".jsx", ".mjs", ".cjs", "/index.ts", "/index.tsx", "/index.js", "/index.jsx" };
          std::vector<std::string> suffixes;
          for (const char *ext : kTryExts) suffixes.push_back(spec + ext);
          resolved = lookupBestSuffix(suffixes);
        }
      } else if (lang == Language::Python) {
        if (!spec.empty() && spec.front() == '.') {
          size_t dots = 0;
          while (dots < spec.size() && spec[dots] == '.') ++dots;
          std::string baseDir = srcDir;
          bool validUp = true;
          for (size_t up = 1; up < dots; ++up) {
            const auto pos = baseDir.rfind('/');
            if (pos == std::string::npos) {
              if (baseDir.empty()) validUp = false;
              baseDir.clear();
              break;
            }
            baseDir.resize(pos);
          }
          if (validUp) {
            std::string rem = spec.substr(dots);
            std::replace(rem.begin(), rem.end(), '.', '/');
            if (rem.empty()) {
              resolved = lookupExact(joinDirAndSpec(baseDir, "__init__.py"));
              if (!resolved) resolved = lookupExact(joinDirAndSpec(baseDir, "__init__.pyi"));
            } else {
              const std::string target = joinDirAndSpec(baseDir, rem);
              for (const auto &cand : { target + ".py", target + ".pyi", target + "/__init__.py", target + "/__init__.pyi" }) {
                if ((resolved = lookupExact(cand))) break;
              }
            }
          }
        } else if (!spec.empty()) {
          std::string rel = spec;
          std::replace(rel.begin(), rel.end(), '.', '/');
          resolved = lookupBestSuffix({ rel + ".py", rel + ".pyi", rel + "/__init__.py", rel + "/__init__.pyi" });
        }
      }

      if (resolved) dep.resolved_path = *resolved;
      out.push_back(std::move(dep));
    }
    return out;
  }

  std::vector<ResolvedDependency> extractDependenciesForFile(std::string_view sourcePath,
      std::string_view content, const std::vector<std::string> &candidatePaths) {
    const Language lang = detectLanguage(sourcePath);
    if (lang == Language::Unsupported) return {};
    const auto imports = extractImportsForContent(lang, content);
    return resolveImports(sourcePath, lang, imports, candidatePaths);
  }

  std::vector<ReferenceMatch> findReferencesInContent(Language lang, std::string_view content,
      std::string_view query, bool includeDeclarations, const std::vector<SymbolInfo> *outlineSymbols) {
    std::vector<ReferenceMatch> out;
    if (lang == Language::Unsupported || query.empty()) return out;

    const auto [qualifier, rawBase] = splitQualifiedQuery(query);
    if (rawBase.empty()) return out;
    const bool destructorQuery = rawBase.size() > 1 && rawBase.front() == '~';
    const std::string_view leafMatch = destructorQuery ? rawBase.substr(1) : rawBase;

    ParserHandle parser;
    ensureLanguage(parser.raw, lang);
    TreeHandle tree(ts_parser_parse_string(parser.raw, nullptr, content.data(), static_cast<uint32_t>(content.size())));
    if (!tree.raw) return out;
    TSNode root = ts_tree_root_node(tree.raw);

    std::vector<SymbolInfo> ownedOutline;
    if (!outlineSymbols) {
      ownedOutline = extractOutlineSymbols(lang, {}, root, content);
      outlineSymbols = &ownedOutline;
    }

    auto findEnclosingAndDecl = [&](size_t startB, size_t endB, bool isDecl) -> std::pair<std::string, const SymbolInfo *> {
      const SymbolInfo *bestOuter = nullptr;
      const SymbolInfo *matchingDecl = nullptr;
      size_t bestSpan = static_cast<size_t>(-1);
      for (const auto &sym : *outlineSymbols) {
        if (sym.start_byte <= startB && endB <= sym.end_byte) {
          if (isDecl && sym.name == rawBase && !matchingDecl) {
            matchingDecl = &sym;
          }
          const size_t span = sym.end_byte - sym.start_byte;
          if (!(isDecl && sym.name == rawBase) && span < bestSpan) {
            bestSpan = span;
            bestOuter = &sym;
          }
        }
      }
      return { bestOuter ? bestOuter->qualified_name : (matchingDecl ? matchingDecl->container : std::string{}), matchingDecl };
    };

    walkAst(root, [&](TSNode node) -> bool {
      if (!ts_node_is_named(node)) return false;
      const char *t = ts_node_type(node);
      if (std::strcmp(t, "comment") == 0 ||
          std::strcmp(t, "char_literal") == 0 ||
          std::strcmp(t, "system_lib_string") == 0 ||
          std::strcmp(t, "string_literal") == 0 ||
          std::strcmp(t, "raw_string_literal") == 0 ||
          std::strcmp(t, "string_content") == 0 ||
          std::strcmp(t, "string_fragment") == 0 ||
          std::strcmp(t, "preproc_include") == 0 ||
          std::strcmp(t, "jsx_closing_element") == 0) {
        return false;
      }

      if (!isLeafIdentifierType(t)) return true;
      const std::string_view text = rawNodeSlice(node, content);
      if (text != leafMatch) return false;
      TSNode parent = ts_node_parent(node);
      if (destructorQuery && !nodeTypeIs(parent, "destructor_name")) return false;
      if (!destructorQuery && nodeTypeIs(parent, "destructor_name")) return false;

      const std::string declRole = classifyDeclarationRole(lang, node);
      const bool isDecl = !declRole.empty();
      if (isDecl && !includeDeclarations) return false;

      const size_t sb = ts_node_start_byte(node);
      const size_t eb = ts_node_end_byte(node);
      const auto [enclosing, declSym] = findEnclosingAndDecl(sb, eb, isDecl);

      bool exactQualifier = true;
      if (!qualifier.empty()) {
        std::string explicitPrefix;
        bool isMemberAccess = false;
        std::string memberReceiver;
        TSNode cur = node;
        TSNode p = parent;
        if (nodeTypeIs(p, "destructor_name")) {
          cur = p;
          p = ts_node_parent(cur);
        }
        while (!ts_node_is_null(p)) {
          const char *pt = ts_node_type(p);
          if ((std::strcmp(pt, "qualified_identifier") == 0 ||
               std::strcmp(pt, "scoped_identifier") == 0 ||
               std::strcmp(pt, "template_function") == 0) &&
              ts_node_eq(ts_node_child_by_field_name(p, "name", 4), cur)) {
            if (std::strcmp(pt, "template_function") == 0) {
              cur = p;
              p = ts_node_parent(cur);
              continue;
            }
            TSNode scope = ts_node_child_by_field_name(p, "scope", 5);
            if (!ts_node_is_null(scope)) {
              std::string raw = collapseWhitespace(rawNodeSlice(scope, content), 256);
              int depth = 0;
              for (char ch : raw) {
                if (ch == '<') { ++depth; continue; }
                if (ch == '>') { if (depth > 0) --depth; continue; }
                if (depth == 0 && !std::isspace(static_cast<unsigned char>(ch))) explicitPrefix.push_back(ch);
              }
            }
            break;
          }
          if (std::strcmp(pt, "field_expression") == 0 &&
              ts_node_eq(ts_node_child_by_field_name(p, "field", 5), cur)) {
            isMemberAccess = true;
            memberReceiver = std::string(trimView(rawNodeSlice(ts_node_child_by_field_name(p, "argument", 8), content)));
            break;
          }
          if (std::strcmp(pt, "member_expression") == 0 &&
              ts_node_eq(ts_node_child_by_field_name(p, "property", 8), cur)) {
            isMemberAccess = true;
            memberReceiver = std::string(trimView(rawNodeSlice(ts_node_child_by_field_name(p, "object", 6), content)));
            break;
          }
          if (std::strcmp(pt, "attribute") == 0 &&
              ts_node_eq(ts_node_child_by_field_name(p, "attribute", 9), cur)) {
            isMemberAccess = true;
            memberReceiver = std::string(trimView(rawNodeSlice(ts_node_child_by_field_name(p, "object", 6), content)));
            break;
          }
          break;
        }

        if (!explicitPrefix.empty()) {
          if (!matchesContainerSuffix(explicitPrefix, qualifier)) return false;
          exactQualifier = true;
        } else if (declSym && matchesContainerSuffix(declSym->container, qualifier)) {
          exactQualifier = true;
        } else if (isMemberAccess) {
          if (memberReceiver == qualifier || matchesContainerSuffix(memberReceiver, qualifier)) {
            exactQualifier = true;
          } else if ((memberReceiver == "this" || memberReceiver == "self" || memberReceiver == "cls") &&
                     !enclosing.empty() && matchesContainerSuffix(enclosing, qualifier)) {
            exactQualifier = true;
          } else {
            exactQualifier = false;
          }
        } else if (!enclosing.empty() && matchesContainerSuffix(enclosing, qualifier)) {
          exactQualifier = true;
        } else {
          return false;
        }
      }

      const TSPoint sp = ts_node_start_point(node);
      const size_t line1 = static_cast<size_t>(sp.row) + 1;
      const size_t col1 = static_cast<size_t>(sp.column) + 1;
      std::string kind = isDecl ? declRole : classifyUsageKind(node);
      out.push_back({line1, col1, std::move(kind), enclosing, lineSnippetAt(content, line1), exactQualifier});
      return false;
    });

    return out;
  }

} // namespace symbol_service
