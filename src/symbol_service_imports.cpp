#include "symbol_service.h"
#include "cutils.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace symbol_service {
  namespace {
    std::string stripQuotedLiteral(std::string_view raw) {
      raw = detail::trimView(raw);
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
  } // namespace

  namespace detail {
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
                pushEdge(stripQuotedLiteral(nodeText(p, src)), "include", line, false);
              } else if (std::strcmp(pt, "system_lib_string") == 0) {
                pushEdge(stripQuotedLiteral(nodeText(p, src)), "include", line, true);
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
              if (isType(child, "dotted_name")) {
                pushEdge(std::string(nodeText(child, src)), "import", line, false);
              } else if (isType(child, "aliased_import")) {
                TSNode name = ts_node_child_by_field_name(child, "name", 4);
                if (!ts_node_is_null(name)) {
                  pushEdge(std::string(nodeText(name, src)), "import", line, false);
                }
              }
            }
            return false;
          }
          if (std::strcmp(t, "import_from_statement") == 0) {
            TSNode modNode = ts_node_child_by_field_name(node, "module_name", 11);
            const std::string_view modText = trimView(nodeText(modNode, src));
            if (modText.empty()) return false;
            if (isAllDots(modText)) {
              bool emitted = false;
              const uint32_t n = ts_node_named_child_count(node);
              for (uint32_t i = 0; i < n; ++i) {
                TSNode child = ts_node_named_child(node, i);
                if (ts_node_eq(child, modNode)) continue;
                if (isType(child, "dotted_name")) {
                  pushEdge(std::string(modText) + std::string(nodeText(child, src)), "from_import", line, false);
                  emitted = true;
                } else if (isType(child, "aliased_import")) {
                  TSNode name = ts_node_child_by_field_name(child, "name", 4);
                  if (!ts_node_is_null(name)) {
                    pushEdge(std::string(modText) + std::string(nodeText(name, src)), "from_import", line, false);
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
            if (isType(s, "string")) {
              pushEdge(stripQuotedLiteral(nodeText(s, src)), "import", line, false);
            }
            return false;
          }
          if (std::strcmp(t, "export_statement") == 0) {
            TSNode s = ts_node_child_by_field_name(node, "source", 6);
            if (!ts_node_is_null(s) && isType(s, "string")) {
              pushEdge(stripQuotedLiteral(nodeText(s, src)), "reexport", line, false);
            }
            return true;
          }
          if (std::strcmp(t, "call_expression") == 0) {
            TSNode fn = ts_node_child_by_field_name(node, "function", 8);
            std::string kind;
            if (isType(fn, "identifier") && nodeText(fn, src) == "require") {
              kind = "require";
            } else if (isType(fn, "import")) {
              kind = "dynamic_import";
            }
            if (!kind.empty()) {
              TSNode args = ts_node_child_by_field_name(node, "arguments", 9);
              if (!ts_node_is_null(args) && ts_node_named_child_count(args) >= 1) {
                TSNode arg0 = ts_node_named_child(args, 0);
                if (isType(arg0, "string")) {
                  pushEdge(stripQuotedLiteral(nodeText(arg0, src)), kind, line, false);
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
  } // namespace detail

  std::vector<ImportEdge> extractImportsForContent(Language lang, std::string_view content) {
    if (lang == Language::Unsupported) return {};
    detail::ParserHandle parser;
    detail::ensureLanguage(parser.raw, lang);
    detail::TreeHandle tree(ts_parser_parse_string(parser.raw, nullptr, content.data(), static_cast<uint32_t>(content.size())));
    if (!tree.raw) return {};
    return detail::extractImportsFromRoot(lang, ts_tree_root_node(tree.raw), content);
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
        byLower.emplace(detail::toLowerAscii(k), k);
      }
    }
    std::sort(keys.begin(), keys.end());

    auto lookupExact = [&](const std::string &k) -> std::optional<std::string> {
      if (const auto it = byExact.find(k); it != byExact.end()) return it->second;
#ifdef _WIN32
      if (const auto it = byLower.find(detail::toLowerAscii(k)); it != byLower.end()) return it->second;
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
          const std::string lcCand = detail::toLowerAscii(cand);
          const std::string lcSuf = detail::toLowerAscii(suf);
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
        if (base.size() > elen && detail::toLowerAscii(std::string_view(base).substr(base.size() - elen)) == ext) {
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
} // namespace symbol_service
