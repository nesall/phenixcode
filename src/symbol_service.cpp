#include "symbol_service.h"

#include <tree_sitter/api.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
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
    switch (lang) {
      case Language::C:
      case Language::Cpp: {
        CppExtractor extractor(content);
        outline.symbols = extractor.extract(root);
        break;
      }
      case Language::Python: {
        const std::string lowerKey = toLowerAscii(key);
        PythonExtractor extractor(content, lowerKey.ends_with(".pyi"));
        outline.symbols = extractor.extract(root);
        break;
      }
      case Language::JavaScript:
      case Language::TypeScript:
      case Language::Tsx: {
        JsTsExtractor extractor(content);
        outline.symbols = extractor.extract(root);
        break;
      }
      case Language::Unsupported:
        break;
    }

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

} // namespace symbol_service
