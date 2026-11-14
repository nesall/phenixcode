Functions where the parameter is only read as a view (no ownership, no null-termination requirement, no return-by-reference):

**Safe to switch to `std::string_view`:**

- `containsAny` — already takes `std::string_view`; no change.
- `addLineComments` — already `std::string_view`.
- `stripMarkdownFromCodeBlock` — already `std::string_view`.

The rest of the list, with caveats:

| Function | Verdict |
|---|---|
| `looksCodeLike(const std::string&)` | Yes — read-only scans, no `c_str()`, no null terminator needed. |
| `isIdentifierLike(const std::string&)` | Yes — index/char loops only. |
| `extractGrepTerms(const std::string&)` | Yes, but the regex iterators need `begin()/end()` on the view — they work since `std::string_view` is contiguous; no `\0` dependence. Safe. |
| `extractGrepLiterals(const std::string&)` | Yes — same reasoning; uses `find` and `substr` only. `substr` on a view returns a view, and those are converted to `std::string` in lambdas, so no dangling. |
| `strFindIn` | Already takes `std::string_view` for `in`, but `t` is also a view — nothing to change. |
| `trimmed(std::string_view)` | Already a view. |
| `isHeader(const std::string&)` | **Careful.** Body uses `fs::path(path)` and `.extension()`. `std::filesystem::path` cannot be constructed from `std::string_view` directly in C++17/20 (no such constructor until C++26-ish proposals), so you'd need `path{std::string(sv)}`. Works, but the temporary means the caller gains nothing. Leave as `const std::string&`, or accept the copy. |
| `stemKey(const std::string&)` | Same `fs::path` issue. Leave as is. |
| `expandEnvVar(const std::string)` — takes **by value** | Change to `std::string_view`: body only does `starts_with`, `ends_with`, `substr`, and `getenv(envVar.c_str())` — **that last one needs a null-terminated string**. So you'd have to construct `std::string` for `c_str()`. Net win is small; keep by-value or switch to view and do one local `std::string` before `getenv`. |
| `sanitizeUtf8(std::string_view)` | Already a view. |
| `queryIntent(const std::string&)` | Yes — copies into a local `std::string q` anyway, so the parameter can become a view and you keep the single copy. |
| `utils::trimmed` / `commonDirPrefix` / `stripPrefix` / `stripCommonPathPrefix` | `stripPrefix(std::string&, const std::string&)` — the prefix is only compared/substr'd via `compare`, so `std::string_view` works. `commonDirPrefix(const std::set<std::string>&)` is a container, not a string; no change. |

**Summary — clean candidates:**
`looksCodeLike`, `isIdentifierLike`, `extractGrepTerms`, `extractGrepLiterals`, `queryIntent`, `stripPrefix` (prefix param).

**Keep as `std::string`:**
`isHeader`, `stemKey` (both feed `std::filesystem::path`), `expandEnvVar` (needs `c_str()` for `getenv`).

Sources:  
*src/cutils.cpp*  
*include/cutils.h*  
*src/tests.cpp*  

<span class="text-surface-500 text-xs">Model: mimo-v2.6-flash</span>  