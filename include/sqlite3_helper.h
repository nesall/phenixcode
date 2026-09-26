#pragma once
#include <string>
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

    void bindText(int i, const std::string &v);
    void bindInt64(int i, int64_t v);
  };

}