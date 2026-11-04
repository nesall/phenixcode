#include "chat_db.h"
#include "sqlite3_helper.h"
#include "utils.h"
#include <utils_log/logger.hpp>
#include <filesystem>
#include <ctime>
#include <stdexcept>
#include <string_view>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#else
#include <cstdlib>
#endif

namespace fs = std::filesystem;
namespace {

  constexpr size_t kPreviewSnippetLimit = 100;
  constexpr std::string_view kPreviewEllipsis = "...";

  const char *kSchema =
    "CREATE TABLE IF NOT EXISTS chats("
    " id TEXT PRIMARY KEY,"
    " project_id TEXT NOT NULL DEFAULT '',"
    " title TEXT NOT NULL DEFAULT '',"
    " created_at INTEGER NOT NULL,"
    " updated_at INTEGER NOT NULL,"
    " messages_json TEXT NOT NULL);"
    "CREATE INDEX IF NOT EXISTS idx_chats_proj_upd ON chats(project_id, updated_at DESC);";

  fs::path chatDbPath() {
#ifdef _WIN32
    fs::path dir = fs::path("chats");
    PWSTR known = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &known))) {
      dir = fs::path(known) / L"PhenixCode" / L"chats";
      CoTaskMemFree(known);
    }
#else
    const char *xdg = std::getenv("XDG_DATA_HOME");
    const char *home = std::getenv("HOME");
    fs::path base = (xdg && *xdg) ? fs::path(xdg)
      : (home ? fs::path(home) : fs::path(".")) / ".local" / "share";
    fs::path dir = base / "PhenixCode" / "chats";
#endif
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir / "chats.db";
  }

  sqlite3_stmt *prepareOrThrow(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
      throw std::runtime_error(std::string("sqlite prepare: ") + sqlite3_errmsg(db));
    return stmt;
  }

  std::string previewSnippet(const std::string &content) {
    std::string snippet;
    for (size_t i = 0; i < content.size(); ++i) {
      if (content[i] == '\r') {
        snippet += ' ';
        if (i + 1 < content.size() && content[i + 1] == '\n') ++i;
      } else {
        snippet += content[i] == '\n' ? ' ' : content[i];
      }
    }
    // Only truncate when the ellipsis makes the normalized text shorter.
    // Count Unicode code points without splitting UTF-8.
    size_t end = 0, count = 0, trimEnd = 0;
    while (end < snippet.size()) {
      if ((static_cast<unsigned char>(snippet[end]) & 0xc0) != 0x80) {
        if (count == kPreviewSnippetLimit) trimEnd = end;
        if (++count > kPreviewSnippetLimit + kPreviewEllipsis.size()) {
          snippet.resize(trimEnd);
          snippet += kPreviewEllipsis;
          break;
        }
      }
      ++end;
    }
    return snippet;
  }

  std::string chatPreview(const nlohmann::json &messages) {
    std::string preview;
    bool foundUser = false;
    if (!messages.is_array()) return preview;
    for (const auto &message : messages) {
      if (!message.is_object() || !message.contains("role") ||
        !message.contains("content") || !message["content"].is_string()) continue;
      if (!foundUser) {
        if (message["role"] == "user") {
          preview = previewSnippet(message["content"].get<std::string>());
          foundUser = true;
        }
      } else if (message["role"] == "assistant") {
        return preview + '\n' + previewSnippet(message["content"].get<std::string>());
      } else if (message["role"] == "user") {
        break; // A later question's answer is not the initial question's response.
      }
    }
    return preview;
  }

} // anonymous namespace

ChatDb &ChatDb::instance() { static ChatDb inst; return inst; }

ChatDb::ChatDb() : ChatDb(chatDbPath().string()) {}

ChatDb::ChatDb(const std::string &path) {
  if (sqlite3_open_v2(path.c_str(), &db_,
    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
    std::string msg = db_ ? sqlite3_errmsg(db_) : "cannot open";
    if (db_) { sqlite3_close(db_); db_ = nullptr; }
    LOG_MSG << "ChatDb open failed:" << msg;
    return;
  }
  char *err = nullptr;
  sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &err);
  sqlite3_exec(db_, "PRAGMA busy_timeout=3000;", nullptr, nullptr, &err);
  if (sqlite3_exec(db_, kSchema, nullptr, nullptr, &err) != SQLITE_OK) {
    LOG_MSG << "ChatDb schema failed:" << (err ? err : "?");
    sqlite3_free(err);
    sqlite3_close(db_);
    db_ = nullptr;
  }
  if (db_) {
    try {
      // Older databases have no schema version. Inspect before altering so reopening is safe.
      bool hasDeletedAt = false;
      {
        utils::SqliteStmt columns(db_);
        columns.stmt_ = prepareOrThrow(db_, "PRAGMA table_info(chats)");
        while (sqlite3_step(columns.ref()) == SQLITE_ROW)
          if (columns.getStr(1) == "deleted_at") hasDeletedAt = true;
      }
      if (!hasDeletedAt && sqlite3_exec(db_,
        "ALTER TABLE chats ADD COLUMN deleted_at INTEGER DEFAULT NULL", nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(db_));
    } catch (const std::exception &ex) {
      LOG_MSG << "ChatDb migration failed:" << ex.what();
      sqlite3_close(db_);
      db_ = nullptr;
    }
  }
  if (db_) LOG_MSG << "ChatDb ready at:" << path;
}

ChatDb::~ChatDb() { if (db_) sqlite3_close(db_); }

nlohmann::json ChatDb::saveChat(const nlohmann::json &chat)
{
  std::lock_guard<std::mutex> lock(mtx_);
  if (!db_) throw std::runtime_error("Chat DB not available");
  if (!chat.is_object() || !chat.contains("messages") || !chat["messages"].is_array())
    throw std::runtime_error("chat object with messages array expected");

  std::string id = chat.value("id", std::string{});
  if (id.empty()) id = shared::generateRandomId(12);
  std::string projectId = chat.value("project_id", std::string{});
  std::string title = chat.value("title", std::string{ "Untitled chat" });
  if (title.size() > 200) title.resize(200);
  std::string messages = chat["messages"].dump();
  sqlite3_int64 now = (sqlite3_int64)std::time(nullptr);

  utils::SqliteStmt st(db_);
  st.stmt_ = prepareOrThrow(db_,
    "INSERT INTO chats (id, project_id, title, created_at, updated_at, messages_json) "
    "VALUES (?1, ?2, ?3, ?4, ?4, ?5) "
    "ON CONFLICT(id) DO UPDATE SET project_id=?2, title=?3, updated_at=?4, messages_json=?5 "
    "WHERE chats.deleted_at IS NULL");
  st.bindText(1, id);
  st.bindText(2, projectId);
  st.bindText(3, title);
  st.bindInt64(4, now);
  st.bindText(5, messages);
  if (sqlite3_step(st.ref()) != SQLITE_DONE)
    throw std::runtime_error(std::string("saveChat: ") + sqlite3_errmsg(db_));
  if (sqlite3_changes(db_) == 0)
    throw std::runtime_error("Cannot update a deleted chat: " + id);
  return { {"status", "success"}, {"id", id} };
}

nlohmann::json ChatDb::listChats(const std::string &projectId, bool deleted)
{
  std::lock_guard<std::mutex> lock(mtx_);
  if (!db_) throw std::runtime_error("Chat DB not available");
  utils::SqliteStmt st(db_);
  const std::string sql =
    std::string("SELECT id, title, project_id, updated_at, deleted_at, messages_json FROM chats WHERE deleted_at IS ") +
    (deleted ? "NOT NULL" : "NULL") + (projectId.empty() ? "" : " AND project_id=?1") +
    (deleted ? " ORDER BY deleted_at DESC, id LIMIT 500" : " ORDER BY updated_at DESC, id LIMIT 500");
  st.stmt_ = prepareOrThrow(db_, sql.c_str());
  if (!projectId.empty()) st.bindText(1, projectId);

  nlohmann::json arr = nlohmann::json::array();
  int rc;
  while ((rc = sqlite3_step(st.ref())) == SQLITE_ROW) {
    auto messages = nlohmann::json::parse(st.getStr(5), nullptr, false);
    arr.push_back({ {"id", st.getStr(0)}, {"title", st.getStr(1)}, {"project_id", st.getStr(2)},
      {"updated_at", st.getInt64(3)}, {"deleted_at", deleted ? nlohmann::json(st.getInt64(4)) : nlohmann::json(nullptr)},
      {"preview", chatPreview(messages)} });
  }
  if (rc != SQLITE_DONE) throw std::runtime_error(std::string("listChats: ") + sqlite3_errmsg(db_));
  return { {"status", "success"}, {"chats", arr} };
}

nlohmann::json ChatDb::getChat(const std::string &id)
{
  std::lock_guard<std::mutex> lock(mtx_);
  if (!db_) throw std::runtime_error("Chat DB not available");
  if (id.empty()) throw std::runtime_error("getChat: chat id required");

  utils::SqliteStmt st(db_);
  st.stmt_ = prepareOrThrow(db_,
    "SELECT id, project_id, title, created_at, updated_at, messages_json "
    "FROM chats WHERE id=?1 AND deleted_at IS NULL");
  st.bindText(1, id);

  if (sqlite3_step(st.ref()) != SQLITE_ROW)
    throw std::runtime_error("Chat not found: " + id);

  nlohmann::json chat;
  chat["id"] = st.getStr(0);
  chat["project_id"] = st.getStr(1);
  chat["title"] = st.getStr(2);
  chat["created_at"] = st.getInt64(3);
  chat["updated_at"] = st.getInt64(4);
  try {
    chat["messages"] = nlohmann::json::parse(st.getStr(5));
  } catch (const std::exception &ex) {
    throw std::runtime_error("Corrupt messages JSON for chat " + id + ": " + ex.what());
  }
  return { {"status", "success"}, {"chat", chat} };
}

nlohmann::json ChatDb::deleteChat(const std::string &id)
{
  return setDeleted(id, true);
}

nlohmann::json ChatDb::restoreChat(const std::string &id)
{
  return setDeleted(id, false);
}

nlohmann::json ChatDb::setDeleted(const std::string &id, bool deleted)
{
  std::lock_guard<std::mutex> lock(mtx_);
  if (!db_) throw std::runtime_error("Chat DB not available");
  if (id.empty()) throw std::runtime_error("chat id required");

  utils::SqliteStmt st(db_);
  st.stmt_ = prepareOrThrow(db_, deleted
    ? "UPDATE chats SET deleted_at=?2 WHERE id=?1 AND deleted_at IS NULL"
    : "UPDATE chats SET deleted_at=NULL WHERE id=?1 AND deleted_at IS NOT NULL");
  st.bindText(1, id);
  if (deleted) st.bindInt64(2, static_cast<sqlite3_int64>(std::time(nullptr)));
  if (sqlite3_step(st.ref()) != SQLITE_DONE)
    throw std::runtime_error(std::string("change chat deletion: ") + sqlite3_errmsg(db_));

  const int changed = sqlite3_changes(db_);
  if (0 == changed)
    throw std::runtime_error("Chat not found in requested state: " + id);
  return { {"status", "success"}, {deleted ? "deleted" : "restored", changed} };
}
