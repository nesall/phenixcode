#include "symbol_service.h"

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace symbol_service {
  namespace {
    class JsTsExtractor {
    public:
      explicit JsTsExtractor(std::string_view source) : src_(source) {}

      std::vector<SymbolInfo> extract(TSNode root) {
        visitContainer(root, {}, false, false);
        detail::assignUniqueSelectors(symbols_);
        return std::move(symbols_);
      }

    private:
      std::string_view src_;
      std::vector<SymbolInfo> symbols_;

      std::string_view nodeText(TSNode node) const {
        return detail::nodeText(node, src_);
      }

      std::string joinScope(const std::vector<std::string> &parts) const {
        return detail::joinScope(parts, ".");
      }

      std::string sliceUntilByte(TSNode startNode, uint32_t endByte, char trimTrail = ';') const {
        return detail::sliceUntilByte(src_, startNode, endByte, trimTrail);
      }

      std::string cleanDeclarationSignature(TSNode rangeNode) const {
        return detail::cleanDeclarationSignature(src_, rangeNode);
      }

      void emitSymbol(SymbolInfo sym, TSNode rangeNode) {
        detail::emitSymbol(symbols_, src_, ".", std::move(sym), rangeNode);
      }

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
          if (detail::isType(child, typeFilter)) out.push_back(child);
        }
        return out;
      }

      // Compute byte span covering any leading sibling decorator(s) before a member in class_body.
      void emitWithSpanBounds(SymbolInfo sym, TSNode endNode, TSNode startOverride) {
        if (sym.name.empty()) return;
        detail::fillSpan(endNode, src_.size(), sym);
        if (!ts_node_is_null(startOverride)) {
          const uint32_t sb = ts_node_start_byte(startOverride);
          if (sb < sym.start_byte) {
            const TSPoint sp = ts_node_start_point(startOverride);
            sym.start_byte = sb;
            sym.start_line = static_cast<size_t>(sp.row) + 1;
            sym.start_col = static_cast<size_t>(sp.column) + 1;
          }
        }
        sym.qualified_name = sym.container.empty() ? sym.name : (sym.container + "." + sym.name);
        symbols_.push_back(std::move(sym));
      }

      void visitContainer(TSNode containerNode, const std::vector<std::string> &containers, bool inClass, bool inAmbient) {
        TSNode pendingDecorator{};
        const uint32_t n = ts_node_named_child_count(containerNode);
        for (uint32_t i = 0; i < n; ++i) {
          TSNode child = ts_node_named_child(containerNode, i);
          if (detail::isType(child, "decorator")) {
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
          TSNode outer = detail::effectiveRange(node, rangeOwner);
          TSNode decl = ts_node_child_by_field_name(node, "declaration", 11);
          if (!ts_node_is_null(decl)) {
            visitItem(decl, outer, containers, inClass, inAmbient);
            return;
          }
          TSNode val = ts_node_child_by_field_name(node, "value", 5);
          if (isFunctionValue(val)) {
            TSNode nameNode = ts_node_child_by_field_name(val, "name", 4);
            std::string fnName = detail::collapseWhitespace(nodeText(nameNode));
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
          TSNode outer = detail::effectiveRange(node, rangeOwner);
          const uint32_t n = ts_node_named_child_count(node);
          for (uint32_t i = 0; i < n; ++i) {
            visitItem(ts_node_named_child(node, i), outer, containers, inClass, true);
          }
          return;
        }

        if (std::strcmp(t, "internal_module") == 0 || std::strcmp(t, "module") == 0) {
          TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
          TSNode body = ts_node_child_by_field_name(node, "body", 4);
          const std::string nsName = detail::collapseWhitespace(nodeText(nameNode));
          if (nsName.empty()) return;
          TSNode spanNode = detail::effectiveRange(node, rangeOwner);
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
          const std::string typeName = detail::collapseWhitespace(nodeText(nameNode));
          if (typeName.empty()) return;
          TSNode spanNode = detail::effectiveRange(node, rangeOwner);
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
          const std::string aliasName = detail::collapseWhitespace(nodeText(nameNode));
          if (aliasName.empty()) return;
          TSNode spanNode = detail::effectiveRange(node, rangeOwner);
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
          const std::string enumName = detail::collapseWhitespace(nodeText(nameNode));
          if (enumName.empty()) return;
          TSNode spanNode = detail::effectiveRange(node, rangeOwner);
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
          const std::string fnName = detail::collapseWhitespace(nodeText(nameNode));
          if (fnName.empty()) return;
          TSNode body = ts_node_child_by_field_name(node, "body", 4);
          const bool hasBody = !ts_node_is_null(body) && std::strcmp(t, "function_signature") != 0;
          TSNode spanNode = detail::effectiveRange(node, rangeOwner);
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
          const std::string methodName = detail::collapseWhitespace(nodeText(nameNode));
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
          const std::string propName = detail::collapseWhitespace(nodeText(nameNode));
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
            const std::string varName = detail::collapseWhitespace(nodeText(nameNode));
            if (varName.empty()) continue;
            TSNode spanNode = singleDecl ? detail::effectiveRange(node, rangeOwner) : d;
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

  namespace detail {
    std::vector<SymbolInfo> extractJsTs(TSNode root, std::string_view content) {
      JsTsExtractor extractor(content);
      return extractor.extract(root);
    }
  } // namespace detail
} // namespace symbol_service
