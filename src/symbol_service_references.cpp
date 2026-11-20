#include "symbol_service.h"

#include <cctype>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace symbol_service {
  namespace {
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
      if (sepPos == std::string_view::npos) return {{}, detail::trimView(query)};
      return {detail::trimView(query.substr(0, sepPos)), detail::trimView(query.substr(sepPos + sepLen))};
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

    std::string classifyDeclarationRole(Language lang, TSNode idNode) {
      TSNode cur = idNode;
      TSNode parent = ts_node_parent(cur);
      if (ts_node_is_null(parent)) return {};

      if (lang == Language::C || lang == Language::Cpp) {
        if (detail::isType(parent, "destructor_name")) {
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
          return detail::isPythonStubEllipsis(body) ? "declaration" : "definition";
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
          if (detail::isType(val, "arrow_function") ||
              detail::isType(val, "function_expression") ||
              detail::isType(val, "generator_function")) {
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
      if (!ts_node_is_null(parent) && detail::isType(parent, "export_specifier")) {
        return "import";
      }
      if (!ts_node_is_null(parent) &&
          (detail::isType(parent, "jsx_opening_element") || detail::isType(parent, "jsx_self_closing_element"))) {
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

      if (detail::isType(node, "type_identifier") ||
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
      return detail::collapseWhitespace(line, 160);
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

  std::vector<ReferenceMatch> findReferencesInContent(Language lang, std::string_view content,
      std::string_view query, bool includeDeclarations, const std::vector<SymbolInfo> *outlineSymbols) {
    std::vector<ReferenceMatch> out;
    if (lang == Language::Unsupported || query.empty()) return out;

    const auto [qualifier, rawBase] = splitQualifiedQuery(query);
    if (rawBase.empty()) return out;
    const bool destructorQuery = rawBase.size() > 1 && rawBase.front() == '~';
    const std::string_view leafMatch = destructorQuery ? rawBase.substr(1) : rawBase;

    detail::ParserHandle parser;
    detail::ensureLanguage(parser.raw, lang);
    detail::TreeHandle tree(ts_parser_parse_string(parser.raw, nullptr, content.data(), static_cast<uint32_t>(content.size())));
    if (!tree.raw) return out;
    TSNode root = ts_tree_root_node(tree.raw);

    std::vector<SymbolInfo> ownedOutline;
    if (!outlineSymbols) {
      ownedOutline = detail::extractOutlineSymbols(lang, {}, root, content);
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

    detail::walkAst(root, [&](TSNode node) -> bool {
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
      const std::string_view text = detail::nodeText(node, content);
      if (text != leafMatch) return false;
      TSNode parent = ts_node_parent(node);
      if (destructorQuery && !detail::isType(parent, "destructor_name")) return false;
      if (!destructorQuery && detail::isType(parent, "destructor_name")) return false;

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
        if (detail::isType(p, "destructor_name")) {
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
              std::string raw = detail::collapseWhitespace(detail::nodeText(scope, content), 256);
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
            memberReceiver = std::string(detail::trimView(detail::nodeText(ts_node_child_by_field_name(p, "argument", 8), content)));
            break;
          }
          if (std::strcmp(pt, "member_expression") == 0 &&
              ts_node_eq(ts_node_child_by_field_name(p, "property", 8), cur)) {
            isMemberAccess = true;
            memberReceiver = std::string(detail::trimView(detail::nodeText(ts_node_child_by_field_name(p, "object", 6), content)));
            break;
          }
          if (std::strcmp(pt, "attribute") == 0 &&
              ts_node_eq(ts_node_child_by_field_name(p, "attribute", 9), cur)) {
            isMemberAccess = true;
            memberReceiver = std::string(detail::trimView(detail::nodeText(ts_node_child_by_field_name(p, "object", 6), content)));
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
