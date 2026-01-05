#ifndef _PHENIXCODE_UTILS_H_
#define _PHENIXCODE_UTILS_H_

#include <string>
#include <string_view>
#include <sqlite3.h>

namespace utils {
  struct SqliteStmt {
    sqlite3 *sq_ = nullptr;
    explicit SqliteStmt(sqlite3 *sq) : sq_(sq) {}
    sqlite3_stmt *stmt_ = nullptr;
    sqlite3_stmt *&ref() { return stmt_; }
    sqlite3_stmt *ref() const { return stmt_; }
    ~SqliteStmt();
    SqliteStmt() = default;
    SqliteStmt(const SqliteStmt &) = delete;
    SqliteStmt &operator=(const SqliteStmt &) = delete;

    std::string getStr(int i) const;
    int getInt(int i) const;
    sqlite3_int64 getInt64(int i) const;

  };


  std::string currentTimestamp();
  time_t getFileModificationTime(const std::string &path);
  int safeStoI(const std::string &s, int def = 0);
  std::string trimmed(std::string_view sv);
  std::string addLineComments(std::string_view code, std::string_view filename);
  std::string stripMarkdownFromCodeBlock(std::string_view code);

} // namespace utils

#endif // _PHENIXCODE_UTILS_H_