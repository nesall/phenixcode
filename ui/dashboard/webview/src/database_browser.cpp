#include "database_browser.h"
#include "sqlite3_helper.h"
#include <chrono>
#include <3rdparty/utf8.h>
#include <algorithm>
#include <fstream>
#include <iterator>

using nlohmann::json;
namespace fs = std::filesystem;

namespace database_browser {
  std::string pathText(const fs::path &path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char *>(text.data()), text.size()};
  }
  namespace {
    constexpr size_t maxFileBytes = 256 * 1024;
    constexpr int maxChunkCharacters = 16384;
    const std::string sourcesSql =
      "WITH sources AS (SELECT path AS source_id FROM files_metadata UNION SELECT source_id FROM chunks) ";
  }

  void Reader::check(int result) const {
    if (result == SQLITE_OK || result == SQLITE_ROW || result == SQLITE_DONE) return;
    const int primary = result & 255;
    if (primary == SQLITE_BUSY || primary == SQLITE_LOCKED)
      throw Error("busy", "The database is busy indexing. Try refreshing shortly.");
    if (primary == SQLITE_NOTADB || primary == SQLITE_CORRUPT)
      throw Error("unsupported_schema", "This file is not a readable PhenixCode indexed database.");
    throw Error("database_error", sqlite3_errmsg(db_.get()));
  }

  bool Reader::step(sqlite3_stmt *statement) const {
    const int result = sqlite3_step(statement);
    check(result);
    return result == SQLITE_ROW;
  }

  Reader::Reader(const fs::path &database, const fs::path &baseDirectory)
    : database_(database), baseDirectory_(baseDirectory) {
    if (!fs::exists(database)) throw Error("not_created", "No indexed database exists at this location yet.");
    if (!fs::is_regular_file(database)) throw Error("database_error", "The database location is not a regular file.");
    sqlite3 *connection = nullptr;
    const auto name = database.u8string();
    const int result = sqlite3_open_v2(reinterpret_cast<const char *>(name.c_str()), &connection, SQLITE_OPEN_READONLY, nullptr);
    db_.reset(connection);
    if (result != SQLITE_OK) throw Error("database_error", "Unable to open the indexed database for reading.");
    check(sqlite3_busy_timeout(db_.get(), 250));

    utils::SqliteStmt columns(db_.get());
    check(sqlite3_prepare_v2(db_.get(), "PRAGMA table_info(files_metadata)", -1, &columns.ref(), nullptr));
    while (step(columns.ref())) if (columns.getStr(1) == "indexed_at") hasIndexedAt_ = true;
    utils::SqliteStmt schema(db_.get());
    const char *probe = "SELECT f.path,f.last_modified,f.file_size,f.nof_lines,c.id,c.source_id,c.content,"
      "c.start_pos,c.end_pos,c.token_count,c.unit,c.type FROM files_metadata f LEFT JOIN chunks c ON 0 LIMIT 0";
    const int schemaResult = sqlite3_prepare_v2(db_.get(), probe, -1, &schema.ref(), nullptr);
    if ((schemaResult & 255) == SQLITE_ERROR || (schemaResult & 255) == SQLITE_NOTADB)
      throw Error("unsupported_schema", "This file does not contain a supported PhenixCode indexed database.");
    check(schemaResult);
  }

  json Reader::list(const std::string &search, int offset, int limit) {
    limit = std::clamp(limit, 1, 100);
    offset = (std::max)(offset, 0);
    // Count and page belong to one short committed snapshot, even during indexing.
    check(sqlite3_exec(db_.get(), "BEGIN", nullptr, nullptr, nullptr));
    struct Snapshot {
      sqlite3 *database;
      bool active = true;
      ~Snapshot() { if (active) sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr); }
    } snapshot{db_.get()};
    utils::SqliteStmt count(db_.get());
    const auto countSql = sourcesSql + "SELECT COUNT(*) FROM sources WHERE instr(lower(source_id),lower(?)) > 0";
    check(sqlite3_prepare_v2(db_.get(), countSql.c_str(), -1, &count.ref(), nullptr));
    count.bindText(1, search.substr(0, 512));
    step(count.ref());
    const auto total = count.getInt64(0);
    if (offset >= total) offset = total ? static_cast<int>((total - 1) / limit * limit) : 0;
    json rows = json::array();
    utils::SqliteStmt page(db_.get());
    const auto sql = sourcesSql + "SELECT s.source_id,f.last_modified,f.file_size,f.nof_lines," +
      (hasIndexedAt_ ? "f.indexed_at" : "NULL") +
      ",(SELECT COUNT(*) FROM chunks c WHERE c.source_id=s.source_id),"
      "(SELECT COALESCE(SUM(token_count),0) FROM chunks c WHERE c.source_id=s.source_id) "
      "FROM sources s LEFT JOIN files_metadata f ON f.path=s.source_id "
      "WHERE instr(lower(s.source_id),lower(?)) > 0 ORDER BY s.source_id COLLATE NOCASE,s.source_id LIMIT ? OFFSET ?";
    check(sqlite3_prepare_v2(db_.get(), sql.c_str(), -1, &page.ref(), nullptr));
    page.bindText(1, search.substr(0, 512));
    page.bindInt64(2, limit);
    page.bindInt64(3, offset);
    while (step(page.ref())) {
      const auto source = page.getStr(0);
      rows.push_back({{"source_id", source}, {"is_url", source.find("://") != std::string::npos},
        {"last_modified", sqlite3_column_type(page.ref(), 1) == SQLITE_NULL ? json(nullptr) : json(page.getInt64(1))},
        {"file_size", sqlite3_column_type(page.ref(), 2) == SQLITE_NULL ? json(nullptr) : json(page.getInt64(2))},
        {"line_count", sqlite3_column_type(page.ref(), 3) == SQLITE_NULL ? json(nullptr) : json(page.getInt64(3))},
        {"indexed_at", sqlite3_column_type(page.ref(), 4) == SQLITE_NULL ? json(nullptr) : json(page.getStr(4))},
        {"chunk_count", page.getInt64(5)}, {"token_count", page.getInt64(6)}});
    }
    // Finish the count statement before ending the snapshot.
    check(sqlite3_reset(count.ref()));
    check(sqlite3_exec(db_.get(), "COMMIT", nullptr, nullptr, nullptr));
    snapshot.active = false;
    return {{"sources", rows}, {"total", total}, {"offset", offset}, {"limit", limit},
      {"database_path", pathText(database_)}, {"base_directory", pathText(baseDirectory_)}};
  }

  void Reader::requireSource(const std::string &source) {
    utils::SqliteStmt stmt(db_.get());
    const char *sql = "SELECT 1 FROM files_metadata WHERE path=? UNION ALL SELECT 1 FROM chunks WHERE source_id=? LIMIT 1";
    check(sqlite3_prepare_v2(db_.get(), sql, -1, &stmt.ref(), nullptr));
    stmt.bindText(1, source);
    stmt.bindText(2, source);
    if (!step(stmt.ref())) throw Error("source_missing", "This source is no longer in the indexed database. Refresh the list.");
  }

  json Reader::chunks(const std::string &source, int offset, int limit) {
    requireSource(source);
    offset = (std::max)(offset, 0);
    limit = std::clamp(limit, 1, 10);
    utils::SqliteStmt stmt(db_.get());
    const char *sql = "SELECT id,substr(content,1,?),length(content),start_pos,end_pos,unit,type,token_count "
      "FROM chunks WHERE source_id=? ORDER BY start_pos,id LIMIT ? OFFSET ?";
    check(sqlite3_prepare_v2(db_.get(), sql, -1, &stmt.ref(), nullptr));
    stmt.bindInt64(1, maxChunkCharacters);
    stmt.bindText(2, source);
    stmt.bindInt64(3, limit + 1);
    stmt.bindInt64(4, offset);
    json rows = json::array();
    bool more = false;
    while (step(stmt.ref())) {
      if (rows.size() == static_cast<size_t>(limit)) { more = true; break; }
      rows.push_back({{"id", stmt.getInt64(0)}, {"content", stmt.getStr(1)},
        {"truncated", stmt.getInt64(2) > maxChunkCharacters}, {"start", stmt.getInt64(3)},
        {"end", stmt.getInt64(4)}, {"unit", stmt.getStr(5)}, {"type", stmt.getStr(6)}, {"tokens", stmt.getInt64(7)}});
    }
    return {{"chunks", rows}, {"offset", offset}, {"has_more", more}};
  }

  fs::path Reader::sourcePath(const std::string &source) {
    requireSource(source);
    if (source.find("://") != std::string::npos) throw Error("not_local", "This source is a URL, not a local file.");
    fs::path path = fs::u8path(source);
    if (path.is_relative()) {
      if (baseDirectory_.empty()) throw Error("location_required", "Choose the working directory used when this project was indexed.");
      path = baseDirectory_ / path;
    }
    return fs::absolute(path).lexically_normal();
  }

  json Reader::readFile(const std::string &source) {
    const auto path = sourcePath(source);
    if (!fs::is_regular_file(path)) throw Error("file_missing", "The source file is missing or is not a regular file.");
    std::ifstream file(path, std::ios::binary);
    if (!file) throw Error("file_unreadable", "Unable to read the source file.");
    std::string raw(maxFileBytes + 1, '\0');
    file.read(raw.data(), static_cast<std::streamsize>(raw.size()));
    if (file.bad()) throw Error("file_unreadable", "Reading the source file failed.");
    raw.resize(static_cast<size_t>(file.gcount()));
    const bool truncated = raw.size() > maxFileBytes;
    if (truncated) raw.resize(maxFileBytes);
    if (raw.find('\0') != std::string::npos) throw Error("binary_file", "This file appears to contain binary data. Indexed chunks are still available.");
    std::string text;
    utf8::replace_invalid(raw.begin(), raw.end(), std::back_inserter(text), '?');
    utils::SqliteStmt metadata(db_.get());
    check(sqlite3_prepare_v2(db_.get(), "SELECT last_modified,file_size FROM files_metadata WHERE path=?", -1, &metadata.ref(), nullptr));
    metadata.bindText(1, source);
    json changed = nullptr;
    if (step(metadata.ref())) {
#ifdef _MSC_VER
      const auto modified = std::chrono::clock_cast<std::chrono::system_clock>(fs::last_write_time(path));
#else
      const auto modified = std::chrono::file_clock::to_sys(fs::last_write_time(path));
#endif
      const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(modified.time_since_epoch()).count();
      changed = metadata.getInt64(0) != seconds || metadata.getInt64(1) != static_cast<int64_t>(fs::file_size(path));
    }
    return {{"content", text}, {"path", pathText(path)}, {"truncated", truncated}, {"may_have_changed", changed}};
  }
}
