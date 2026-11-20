#include "symbol_service.h"

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace symbol_service {
  namespace {
    class PythonExtractor {
    public:
      PythonExtractor(std::string_view source, bool isStubFile)
        : src_(source), isStubFile_(isStubFile) {}

      std::vector<SymbolInfo> extract(TSNode root) {
        visitBlock(root, {}, false);
        detail::assignUniqueSelectors(symbols_);
        return std::move(symbols_);
      }

    private:
      std::string_view src_;
      std::vector<SymbolInfo> symbols_;
      bool isStubFile_ = false;

      std::string_view nodeText(TSNode node) const {
        return detail::nodeText(node, src_);
      }

      std::string joinScope(const std::vector<std::string> &parts) const {
        return detail::joinScope(parts, ".");
      }

      std::string sliceUntilByte(TSNode startNode, uint32_t endByte, char trimTrail = ';') const {
        return detail::sliceUntilByte(src_, startNode, endByte, trimTrail);
      }

      void emitSymbol(SymbolInfo sym, TSNode rangeNode) {
        detail::emitSymbol(symbols_, src_, ".", std::move(sym), rangeNode);
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
          TSNode outer = detail::effectiveRange(node, rangeOwner);
          TSNode def = ts_node_child_by_field_name(node, "definition", 10);
          if (!ts_node_is_null(def)) visitItem(def, outer, containers, inClass);
          return;
        }

        if (std::strcmp(t, "class_definition") == 0) {
          TSNode nameNode = ts_node_child_by_field_name(node, "name", 4);
          TSNode body = ts_node_child_by_field_name(node, "body", 4);
          const std::string className = detail::collapseWhitespace(nodeText(nameNode));
          if (className.empty()) return;
          TSNode spanNode = detail::effectiveRange(node, rangeOwner);
          SymbolInfo sym;
          sym.name = className;
          sym.container = joinScope(containers);
          sym.kind = SymbolKind::Class;
          sym.role = SymbolRole::Definition;
          sym.signature = !ts_node_is_null(body)
            ? sliceUntilByte(spanNode, ts_node_start_byte(body), ':')
            : detail::collapseWhitespace(nodeText(spanNode));
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
          const std::string fnName = detail::collapseWhitespace(nodeText(nameNode));
          if (fnName.empty()) return;
          TSNode spanNode = detail::effectiveRange(node, rangeOwner);
          const bool declOnly = isStubFile_ && detail::isPythonStubEllipsis(body);
          SymbolInfo sym;
          sym.name = fnName;
          sym.container = joinScope(containers);
          sym.kind = inClass ? SymbolKind::Method : SymbolKind::Function;
          sym.role = declOnly ? SymbolRole::Declaration : SymbolRole::Definition;
          sym.signature = !ts_node_is_null(body)
            ? sliceUntilByte(spanNode, ts_node_start_byte(body), ':')
            : detail::collapseWhitespace(nodeText(spanNode));
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
  } // namespace

  namespace detail {
    std::vector<SymbolInfo> extractPython(TSNode root, std::string_view content, bool isStubFile) {
      PythonExtractor extractor(content, isStubFile);
      return extractor.extract(root);
    }
  } // namespace detail
} // namespace symbol_service
