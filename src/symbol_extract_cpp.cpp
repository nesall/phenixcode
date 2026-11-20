#include "symbol_service.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace symbol_service {
  namespace {
    std::string_view baseClassName(std::string_view name) {
      const auto angle = name.find('<');
      if (angle != std::string_view::npos) name = name.substr(0, angle);
      return detail::trimView(name);
    }

    class CppExtractor {
    public:
      explicit CppExtractor(std::string_view source) : src_(source) {}

      std::vector<SymbolInfo> extract(TSNode root) {
        ScopeCtx ctx;
        visitBlockChildren(root, ctx);
        detail::assignUniqueSelectors(symbols_);
        return std::move(symbols_);
      }

    private:
      std::string_view src_;
      std::vector<SymbolInfo> symbols_;

      struct ScopeCtx {
        std::vector<std::string> containers;
        bool inClass = false;
        std::string currentClass;
      };

      std::string_view nodeText(TSNode node) const {
        return detail::nodeText(node, src_);
      }

      static bool isType(TSNode node, const char *typeName) {
        return detail::isType(node, typeName);
      }

      static TSNode effectiveRange(TSNode inner, TSNode rangeOwner) {
        return detail::effectiveRange(inner, rangeOwner);
      }

      std::string sliceUntilByte(TSNode startNode, uint32_t endByte, char trimTrail = ';') const {
        return detail::sliceUntilByte(src_, startNode, endByte, trimTrail);
      }

      std::string cleanDeclarationSignature(TSNode rangeNode) const {
        return detail::cleanDeclarationSignature(src_, rangeNode);
      }

      void emitSymbol(SymbolInfo sym, TSNode rangeNode) {
        detail::emitSymbol(symbols_, src_, "::", std::move(sym), rangeNode);
      }

      std::string joinContainers(const std::vector<std::string> &parts) const {
        return detail::joinScope(parts, "::");
      }

      std::string normalizeScopeSegment(TSNode scopeNode) const {
        if (isType(scopeNode, "template_type")) {
          TSNode base = ts_node_child_by_field_name(scopeNode, "name", 4);
          if (!ts_node_is_null(base)) return detail::collapseWhitespace(nodeText(base));
        }
        return std::string(detail::trimView(nodeText(scopeNode)));
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
          return detail::collapseWhitespace(nodeText(leafNode));
        }
        if (isType(leafNode, "operator_cast")) {
          TSNode typeNode = ts_node_child_by_field_name(leafNode, "type", 4);
          if (!ts_node_is_null(typeNode)) {
            return "operator " + detail::collapseWhitespace(nodeText(typeNode));
          }
          return detail::collapseWhitespace(nodeText(leafNode));
        }
        if (isType(leafNode, "nested_namespace_specifier")) {
          std::string compact;
          for (const char ch : nodeText(leafNode)) {
            if (!std::isspace(static_cast<unsigned char>(ch))) compact.push_back(ch);
          }
          return compact;
        }
        return detail::collapseWhitespace(nodeText(leafNode));
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
          leafName = detail::collapseWhitespace(nodeText(nameNode));
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
          const std::string aliasName = detail::collapseWhitespace(nodeText(nameNode));
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
              sym.name = detail::collapseWhitespace(nodeText(d));
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
          const std::string conceptName = detail::collapseWhitespace(nodeText(nameNode));
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
    };
  } // namespace

  namespace detail {
    std::vector<SymbolInfo> extractCpp(TSNode root, std::string_view content) {
      CppExtractor extractor(content);
      return extractor.extract(root);
    }
  } // namespace detail
} // namespace symbol_service
