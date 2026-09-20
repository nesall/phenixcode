#include "sqlite3_helper.h"
#include <utils_log/logger.hpp>

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
#if 0
  double SqliteStmt::getDouble(int i) const
  {

  }
#endif
}