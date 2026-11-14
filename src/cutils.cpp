#include "cutils.h"

#include <vector>
#include <chrono>
#include <algorithm>
#include <filesystem>
#include <utility>
#include <unordered_set>
#include <regex>
#include <cctype>
#include <initializer_list>
#include <string>
#include <3rdparty/utf8.h>

namespace fs = std::filesystem;

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#include <termios.h>
#include <sys/stat.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace {
  void trimmedViewLeading(std::string_view &v) {
    size_t start = 0;
    size_t end = v.size();
    while (start < end && std::isspace(static_cast<unsigned char>(v[start]))) ++start;
    v = v.substr(start, end - start);
  }

  void trimmedViewTrailing(std::string_view &v) {
    size_t end = v.size();
    while (end > 0 && std::isspace(static_cast<unsigned char>(v[end - 1]))) --end;
    v = v.substr(0, end);
  }

  void trimmedView(std::string_view &v) {
    trimmedViewLeading(v);
    trimmedViewTrailing(v);
  }

  template <class Range>
  std::string commonDirPrefix(const Range &paths) {
    if (paths.empty()) return {};
    auto it = paths.begin();
    std::string_view prefix = *it;
    for (++it; it != paths.end() && !prefix.empty(); ++it) {
      std::string_view s = *it;
      size_t j = 0;
      while (j < prefix.size() && j < s.size() && prefix[j] == s[j]) ++j;
      prefix = prefix.substr(0, j);
    }
    auto sep = prefix.find_last_of("/\\");
    if (sep == std::string_view::npos) return {};
    return std::string(prefix.substr(0, sep + 1));
  }

} // anonymous namespace

const std::filesystem::path &utils::getExecutableDir()
{
  static const std::filesystem::path dir = []() -> std::filesystem::path {
#ifdef _WIN32
    char path[MAX_PATH] = { 0 };
    DWORD len = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (len == 0 || len == MAX_PATH)
      throw std::runtime_error("GetModuleFileNameA failed");
    return std::filesystem::path(path).parent_path();
#elif __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size); // first call: get required size
    std::vector<char> buf(size);
    if (_NSGetExecutablePath(buf.data(), &size) != 0)
      throw std::runtime_error("_NSGetExecutablePath failed");
    // resolve symlinks (macOS often gives you one)
    return fs::canonical(fs::path(buf.data())).parent_path();
#else
    return fs::read_symlink("/proc/self/exe").parent_path();
#endif
    }();
  return dir;
}

std::string utils::currentTimestamp()
{
  auto now = std::chrono::system_clock::now();
  auto time = std::chrono::system_clock::to_time_t(now);
  std::stringstream ss;
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &time);
#else
  localtime_r(&time, &tm);
#endif
  ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
  return ss.str();
}

#if 0
time_t utils::getFileModificationTime(const std::string &path)
{
  //auto ftime = fs::last_write_time(path);
  //auto sctp = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
  //return std::chrono::system_clock::to_time_t(sctp);
  // Implementation compatible with C++17
  auto ftime = fs::last_write_time(path);
  using namespace std::chrono;
  auto sctp = time_point_cast<system_clock::duration>(
    ftime - fs::file_time_type::clock::now() + system_clock::now()); // Race timing drift bug because of the two `nows`
  return system_clock::to_time_t(sctp);
}
#endif

// Returns deterministic UTC seconds since Unix epoch.
time_t utils::getFileModificationTime(const std::string &path)
{
#if defined(_WIN32)

  WIN32_FILE_ATTRIBUTE_DATA data;
  if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data))
    return 0;

  ULARGE_INTEGER ull;
  ull.LowPart = data.ftLastWriteTime.dwLowDateTime;
  ull.HighPart = data.ftLastWriteTime.dwHighDateTime;

  // Convert Windows FILETIME (100ns ticks since 1601) to Unix epoch seconds
  constexpr uint64_t WINDOWS_TO_UNIX_EPOCH = 116444736000000000ULL;
  return static_cast<time_t>((ull.QuadPart - WINDOWS_TO_UNIX_EPOCH) / 10000000ULL);

#else

  struct stat st;
  if (stat(path.c_str(), &st) != 0)
    return 0;

  return static_cast<time_t>(st.st_mtime);

#endif
}


int utils::safeStoI(const std::string &s, int def)
{
  try {
    return std::stoi(s);
  } catch (...) {
  }
  return def;
}

std::string utils::trimmed(std::string_view sv)
{
  auto wsfront = std::find_if_not(sv.begin(), sv.end(), ::isspace);
  auto wsback = std::find_if_not(sv.rbegin(), sv.rend(), ::isspace).base();
  return (wsfront < wsback ? std::string(wsfront, wsback) : std::string{});
}

std::string utils::addLineComments(std::string_view code, std::string_view filename)
{
  namespace fs = std::filesystem;

  std::string ext = fs::path(filename).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
    [](unsigned char c) { return std::tolower(c); });

  std::string prefix;
  std::string suffix;  // usually empty, used only for /* ... */ style

  static const std::vector<std::string> hash_style = {
      ".py", ".pyw", ".sh", ".bash", ".zsh", ".rb", ".rbw",
      ".yml", ".yaml", ".toml", ".ini", ".cfg", ".dockerfile", ".env"
  };

  static const std::vector<std::string> slash_style = {
      ".c", ".cpp", ".cc", ".cxx", ".h", ".hpp", ".hh", ".hxx",
      ".js", ".jsx", ".ts", ".tsx", ".vue",
      ".java", ".kt", ".groovy", ".scala", ".cs",
      ".rs", ".go", ".php"
  };

  if (std::find(hash_style.cbegin(), hash_style.cend(), ext) != hash_style.cend()) {
    prefix = "# ";
  } else if (std::find(slash_style.cbegin(), slash_style.cend(), ext) != slash_style.cend()) {
    prefix = "// ";
  } else if (ext == ".css" || ext == ".scss" || ext == ".less") {
    prefix = "/* ";
    suffix = " */";
  } else if (ext == ".lua" || ext == ".sql" || ext == ".pl" || ext == ".sql") {
    prefix = "-- ";
  } else {
    return std::string(code);
  }

  if (code.empty()) {
    return {};
  }

  std::string result;
  result.reserve(code.size() + code.size() / 4);  // rough estimate: +25% space

  std::string_view remaining = code;
  bool firstLine = true;

  while (!remaining.empty()) {
    size_t lineEnd = remaining.find('\n');
    bool has_newline = (lineEnd != std::string_view::npos);

    std::string_view line = has_newline
      ? remaining.substr(0, lineEnd)
      : remaining;

    std::string_view trimmed = line;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) {
      trimmed.remove_suffix(1);
    }

    if (!trimmed.empty()) {
      if (!firstLine) {
        result += '\n';
      }
      result += prefix;
      result += line;           // original line including leading/trailing ws
      result += suffix;
    } else {
      if (!firstLine) {
        result += '\n';
      }
      result += line;
    }

    if (has_newline) {
      remaining.remove_prefix(lineEnd + 1);
    } else {
      remaining.remove_prefix(line.size());
    }

    firstLine = false;
  }

  if (!code.empty() && code.back() == '\n') {
    result += '\n';
  }

  return result;
}

std::string utils::stripMarkdownFromCodeBlock(std::string_view code)
{
  auto codeTrimmed = code;
  trimmedView(codeTrimmed);
  if (codeTrimmed.length() < 6) {
    return std::string(code);
  }
  if (!codeTrimmed.starts_with("```")) {
    return std::string(code);
  }
  size_t fenceEnd = code.find('\n', 3);
  if (fenceEnd == std::string_view::npos) {
    // No newline after opening ``` -> maybe single-line or malformed
    if (code.ends_with("```")) {
      // ```code```
      return std::string(code.substr(3, code.length() - 6));
    }
    return std::string(code);  // not a proper fence
  }

  // Extract language tag if present (e.g. "cpp", "python", empty)
  std::string_view lang = code.substr(3, fenceEnd - 3);
  trimmedView(lang);

  // We look for \n``` at the end, possibly followed by optional whitespace/newline
  size_t closingPos = code.rfind("\n```");
  if (closingPos == std::string_view::npos || closingPos <= fenceEnd) {
    return std::string(code);
  }

  // Check if the closing fence is at the end (or only whitespace after)
  std::string_view tail = code.substr(closingPos + 4);  // after \n```
  bool tail_is_empty_or_ws = tail.empty() ||
    std::all_of(tail.begin(), tail.end(),
      [](unsigned char c) { return std::isspace(c); });

  if (!tail_is_empty_or_ws) {
    // There's content after closing fence -> treat as not fenced
    return std::string(code);
  }

  size_t contentStart = fenceEnd + 1;
  size_t contentLength = closingPos - contentStart;

  std::string_view inner = code.substr(contentStart, contentLength);

  return std::string(inner);
}

std::size_t utils::strFindIn(std::string_view in, std::string_view t, bool caseSensitive)
{
  if (t.empty()) return 0;
  if (caseSensitive) {
    size_t pos = in.find(t);
    return pos;
  } else {
    auto it = std::search(
      in.begin(), in.end(),
      t.begin(), t.end(),
      [](char ch1, char ch2) { return std::tolower(static_cast<unsigned char>(ch1)) == std::tolower(static_cast<unsigned char>(ch2)); }
    );
    if (it != in.end()) {
      return static_cast<size_t>(std::distance(in.begin(), it));
    } else {
      return std::string_view::npos;
    }
  }
}

bool utils::containsAny(std::string_view text, std::initializer_list<std::string_view> markers)
{
  return std::ranges::any_of(markers, [text](std::string_view marker) {
    return text.find(marker) != std::string_view::npos;
    });
}

std::string utils::commonDirPrefix(const std::set<std::string> &paths)
{
  return ::commonDirPrefix(paths); // your existing template, renamed e.g. detail::commonDirPrefixImpl
}

void utils::stripPrefix(std::string &s, const std::string &prefix)
{
  if (!prefix.empty() && s.compare(0, prefix.size(), prefix) == 0) s.erase(0, prefix.size());
}

void utils::stripCommonPathPrefix(std::vector<std::string> &paths)
{
  const std::string prefix = ::commonDirPrefix(paths);
  if (prefix.empty()) return;
  for (auto &s : paths) stripPrefix(s, prefix);
}

void utils::stripCommonPathPrefix(std::set<std::string> &paths)
{
  const std::string prefix = commonDirPrefix(paths);
  if (prefix.empty()) return;
  std::set<std::string> out;
  for (const auto &s : paths) {
    std::string t{ s };
    stripPrefix(t, prefix);
    out.insert(out.end(), t);
  }
  paths = std::move(out);
}

std::filesystem::path utils::monitorPathFor(std::filesystem::path settingsFilePath)
{
  return settingsFilePath.parent_path() / (settingsFilePath.stem().string() + ".monitor.json");
}

std::vector<std::string> utils::extractGrepTerms(const std::string &text)
{
  static const std::unordered_set<std::string> stop = {
    "what", "does", "do", "is", "are", "the", "a", "an", "how", "why", "of", "to",
    "in", "function", "class", "method", "this", "that", "and", "or", "for", "with", "where", "find"
  };

  std::vector<std::string> terms;
  auto addTerm = [&](std::string s) {
    s = utils::trimmed(s);
    if (s.size() < 2) return;
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
    if (stop.count(lower)) return;
    if (std::find(terms.begin(), terms.end(), s) == terms.end()) terms.push_back(std::move(s));
    };

  // 1. Backtick / quoted literals
  static const std::regex quotedRe(
    R"re(`([^`\n]+)`|"([^"\n]+)"|'([^'\n]+)')re"
  );
  for (std::sregex_iterator it(text.begin(), text.end(), quotedRe), end;
    it != end; ++it) {
    for (int g : {1, 2, 3}) {
      if ((*it)[g].matched)
        addTerm((*it)[g].str());
    }
  }

  // 2. Qualified names, call sites, paths, UPPER_SNAKE, camelCase
  static const std::regex codeTokenRe(
    R"(([A-Za-z_][A-Za-z0-9_]*(?:(?:::|->|\.)[A-Za-z_][A-Za-z0-9_]*)+)|([A-Za-z_][A-Za-z0-9_]*\s*\()|([A-Za-z0-9_./\\-]+\.[A-Za-z0-9]+))"
  );
  for (std::sregex_iterator it(text.begin(), text.end(), codeTokenRe), end;
    it != end; ++it) {
    std::string m = (*it).str();
    if (!m.empty() && m.back() == '(')
      m = utils::trimmed(m.substr(0, m.size() - 1));
    addTerm(m);
  }

  // 3. Fallback to general identifiers
  static const std::regex identRe(R"(\b[A-Za-z_][A-Za-z0-9_]{2,}\b)");
  for (std::sregex_iterator it(text.begin(), text.end(), identRe), end; it != end; ++it) {
    addTerm((*it).str());
  }
  return terms;
}

//  Handles only explicit forms: quoted/backticked/fenced spans, and a::b, a->b, a.b(, foo(), paths with /. Deduplicate. 
//  Keep extractGrepTerms for the pre-index / empty-DB path, where there is nothing to df-prune against.
std::vector<std::string> utils::extractGrepLiterals(const std::string &text)
{
  std::vector<std::string> out;
  std::unordered_set<std::string> seen;

  auto pushPart = [&](std::string lit)
    {
      lit = utils::trimmed(lit);
      if (lit.empty()) return;
      while (!lit.empty() &&
        (lit.front() == '`' || lit.front() == '"' || lit.front() == '\'' ||
          lit.front() == '(' || lit.front() == '[' || lit.front() == '{' ||
          lit.front() == '<')) {
        lit.erase(lit.begin());
      }

      while (!lit.empty() &&
        (lit.back() == '`' || lit.back() == '"' || lit.back() == '\'' ||
          lit.back() == ')' || lit.back() == ']' || lit.back() == '}' ||
          lit.back() == ';' || lit.back() == ',' || lit.back() == '>')) {
        lit.pop_back();
      }

      if (lit.empty()) return;

      // Ignore long plain prose, keep explicit code-ish literals.
      if (lit.find_first_of(" \t\r\n") != std::string::npos &&
        lit.find('/') == std::string::npos &&
        lit.find('\\') == std::string::npos &&
        lit.find("::") == std::string::npos &&
        lit.find("->") == std::string::npos &&
        lit.find('.') == std::string::npos) {
        return;
      }

      std::string lower = lit;
      std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

      if (seen.insert(lower).second) {
        out.push_back(std::move(lit));
      }
    };

  auto addLiteral = [&](std::string lit)
    {
      lit = utils::trimmed(lit);
      if (lit.empty()) return;
      if (lit.find('|') != std::string::npos &&
        lit.find_first_of(" \t\r\n") == std::string::npos) {
        size_t start = 0;
        while (start <= lit.size()) {
          size_t p = lit.find('|', start);
          if (p == std::string::npos) p = lit.size();
          pushPart(lit.substr(start, p - start));
          if (p == lit.size()) break;
          start = p + 1;
        }
        return;
      }
      pushPart(std::move(lit));
    };

  // 1) Markdown fenced code blocks: ``` ... ```
  size_t pos = 0;
  while ((pos = text.find("```", pos)) != std::string::npos) {
    const size_t afterOpen = pos + 3;
    const size_t lineBreak = text.find('\n', afterOpen);
    if (lineBreak == std::string::npos) break;

    const size_t close = text.find("```", lineBreak + 1);
    if (close == std::string::npos) break;

    addLiteral(text.substr(lineBreak + 1, close - lineBreak - 1));
    pos = close + 3;
  }

  // 2) Inline quoted / backticked literals
  static const std::regex quotedRe(
    R"re((`([^`\n]+)`|"([^"\n]+)"|'([^'\n]+)'))re"
  );
  for (std::sregex_iterator it(text.begin(), text.end(), quotedRe), end;
    it != end; ++it) {
    for (int g : { 1, 2, 3, 4 }) {
      if ((*it)[g].matched) {
        addLiteral((*it)[g].str());
      }
    }
  }

  // 3) Qualified names and call sites: a::b, a->b, obj.method(), foo()
  static const std::regex symbolRe(
    R"(([A-Za-z_][A-Za-z0-9_]*(?:(?:::|->|\.)[A-Za-z_][A-Za-z0-9_]*)+)|([A-Za-z_][A-Za-z0-9_]*(?:(?:::|->|\.)[A-Za-z_][A-Za-z0-9_]*)*\s*\())"
  );
  for (std::sregex_iterator it(text.begin(), text.end(), symbolRe), end;
    it != end; ++it) {
    for (int g : { 1, 2 }) {
      if ((*it)[g].matched) {
        std::string m = (*it)[g].str();
        if (!m.empty() && m.back() == '(') {
          m.pop_back();
        }
        addLiteral(m);
      }
    }
  }

  // 4) Path-like literals: src/app.cpp, include/cutils.h
  static const std::regex pathRe(
    R"((?:[A-Za-z]:)?(?:[A-Za-z0-9_.-]+[\\/])+[A-Za-z0-9_.-]+)"
  );
  for (std::sregex_iterator it(text.begin(), text.end(), pathRe), end;
    it != end; ++it) {
    addLiteral((*it).str());
  }

  // 5) Explicit literals only: path-like tokens and namespace-qualified symbols.
  static const std::regex explicitRe(
    R"(([A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)+)|(?:[A-Za-z]:)?(?:[A-Za-z0-9_.-]+[\\/])+[A-Za-z0-9_.-]+)"
  );
  for (std::sregex_iterator it(text.begin(), text.end(), explicitRe), end;
    it != end; ++it) {
    addLiteral((*it).str());
  }

  //// 5) Fallback for code-ish identifiers (camelCase, snake_case, UPPER_SNAKE)
  //static const std::regex wordRe(R"(\b[A-Za-z_][A-Za-z0-9_]{1,}\b)");
  //for (std::sregex_iterator it(text.begin(), text.end(), wordRe), end;
  //  it != end; ++it) {
  //  const std::string w = (*it).str();
  //  if (utils::looksCodeLike(w) || w.find('_') != std::string::npos) {
  //    addLiteral(w);
  //  }
  //}

  return out;
}

bool utils::looksCodeLike(std::string_view text)
{
  if (text.find("::") != std::string::npos || text.find("->") != std::string::npos ||
    text.find('`') != std::string::npos || text.find('_') != std::string::npos ||
    text.find("()") != std::string::npos) return true;
  for (size_t i = 1; i < text.size(); ++i) {
    if (std::islower(static_cast<unsigned char>(text[i - 1])) &&
      std::isupper(static_cast<unsigned char>(text[i]))) return true; // camelCase
  }
  return false;
}

bool utils::isIdentifierLike(std::string_view t)
{
  bool hasLower = false, hasInnerUpper = false, hasDigit = false, hasAlpha = false;
  for (size_t i = 0; i < t.size(); ++i) {
    unsigned char c = t[i];
    if (c == '_') return true;
    if (std::isdigit(c)) hasDigit = true;
    if (std::isalpha(c)) hasAlpha = true;
    if (std::islower(c)) hasLower = true;
    if (i > 0 && std::isupper(c)) hasInnerUpper = true;
  }
  return (hasLower && hasInnerUpper) || (hasDigit && hasAlpha);
}

bool utils::isHeader(const std::string &path)
{
  static const std::unordered_set<std::string> headerExts = {
    ".h", ".hpp", ".hh", ".hxx", ".h++", ".inc", ".inl", ".ipp"
  };
  std::string ext = fs::path(path).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
    [](unsigned char c) { return std::tolower(c); });
  return headerExts.count(ext) > 0;
}

std::string utils::stemKey(const std::string &path)
{
  fs::path p(path);
  // "/src/foo.h" and "/src/foo.cpp" -> "/src/foo"
  return (p.parent_path() / p.stem()).string();
}

[[nodiscard]] std::string utils::expandEnvVar(const std::string &var)
{
  // Simple ${VAR} substitution
  if (var.starts_with("${") && var.ends_with("}")) {
    std::string envVar = var.substr(2, var.length() - 3);
    if (const char *envValue = getenv(envVar.c_str())) {
      return std::string(envValue);
    }
  }
  return var;
}

std::string utils::sanitizeUtf8(std::string_view input)
{
  std::string out;
  utf8::replace_invalid(input.begin(), input.end(), std::back_inserter(out), '?');
  return out;
}

utils::QueryIntent utils::queryIntent(std::string_view query)
{
  std::string q{ query };
  std::transform(q.begin(), q.end(), q.begin(),
    [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });

  auto hasWord = [&q](const std::string &w) {
    for (size_t pos = q.find(w);
      pos != std::string::npos;
      pos = q.find(w, pos + 1)) {
      const bool left =
        pos == 0 || !std::isalnum(static_cast<unsigned char>(q[pos - 1]));

      const size_t end = pos + w.size();
      const bool right =
        end == q.size() ||
        !std::isalnum(static_cast<unsigned char>(q[end]));

      if (left && right) {
        return true;
      }
    }
    return false;
    };

  auto count = [&hasWord](std::initializer_list<const char *> words) {
    size_t n = 0;
    for (const char *w : words) {
      if (hasWord(w)) {
        ++n;
      }
    }
    return n;
    };

  const size_t declHits = count({
      "where", "define", "defined", "struct", "class",
      "interface", "declaration", "header", "signature"
    });

  const size_t implHits = count({
      "how", "implement", "does", "work", "working",
      "responsible", "logic", "body", "flow"
    });

  if (declHits > implHits) return QueryIntent::Declaration;
  if (implHits > declHits) return QueryIntent::Implementation;
  return QueryIntent::Neutral;
}

