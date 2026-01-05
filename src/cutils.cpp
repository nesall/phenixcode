#include "cutils.h"

#include <vector>
#include <chrono>
#include <algorithm>
#include <filesystem>

#include <utils_log/logger.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#include <termios.h>
#include <sys/stat.h>
#endif

namespace utils {
  SqliteStmt::~SqliteStmt()
  {
    if (stmt_) {
      auto rc = sqlite3_finalize(stmt_);
      if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
        const char *msg = sq_ ? sqlite3_errmsg(sq_) : "'no handle'";
        LOG_MSG << "SQLite finalize error: " << msg;
      }
    }
  }

  std::string SqliteStmt::getStr(int i) const
  {
    auto p = reinterpret_cast<const char *>(sqlite3_column_text(ref(), i));
    return p ? std::string{ p } : std::string{};
  }

  int SqliteStmt::getInt(int i) const
  {
    return sqlite3_column_int(ref(), i);
  }

  sqlite3_int64 SqliteStmt::getInt64(int i) const
  {
    return sqlite3_column_int64(ref(), i);
  }
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
  if (code.length() < 6) {  // too short for ```...\n```
    return std::string(code);
  }

  if (!code.starts_with("```")) {
    return std::string(code);
  }

  // Find the end of the opening fence line
  size_t fence_end = code.find('\n', 3);
  if (fence_end == std::string_view::npos) {
    // No newline after opening ``` -> maybe single-line or malformed
    if (code.ends_with("```")) {
      // ```code```
      return std::string(code.substr(3, code.length() - 6));
    }
    return std::string(code);  // not a proper fence
  }

  // Extract language tag if present (e.g. "cpp", "python", empty)
  std::string_view lang = code.substr(3, fence_end - 3);
  // Trim whitespace from lang (optional, but helps robustness)
  while (!lang.empty() && std::isspace(static_cast<unsigned char>(lang.front()))) {
    lang.remove_prefix(1);
  }
  while (!lang.empty() && std::isspace(static_cast<unsigned char>(lang.back()))) {
    lang.remove_suffix(1);
  }

  // Now find closing fence
  // We look for \n``` at the end, possibly followed by optional whitespace/newline
  size_t closing_pos = code.rfind("\n```");
  if (closing_pos == std::string_view::npos || closing_pos <= fence_end) {
    // No matching closing fence found
    return std::string(code);
  }

  // Check if the closing fence is at the end (or only whitespace after)
  std::string_view tail = code.substr(closing_pos + 4);  // after \n```
  bool tail_is_empty_or_ws = tail.empty() ||
    std::all_of(tail.begin(), tail.end(),
      [](unsigned char c) { return std::isspace(c); });

  if (!tail_is_empty_or_ws) {
    // There's content after closing fence -> treat as not fenced
    return std::string(code);
  }

  // Extract content between opening fence newline and closing fence
  size_t content_start = fence_end + 1;
  size_t content_length = closing_pos - content_start;

  std::string_view inner = code.substr(content_start, content_length);

  // If the inner content ends with newline before closing fence, keep it consistent
  return std::string(inner);
}
