#include "chat_db.h"
#include "sqlite3_helper.h"
#include "utils.h"
#include <utils_log/logger.hpp>
#include <filesystem>
#include <ctime>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#else
#include <cstdlib>
#endif

namespace fs = std::filesystem;
namespace {

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

} // anonymous namespace

ChatDb &ChatDb::instance() { static ChatDb inst; return inst; }

ChatDb::ChatDb() {
  auto path = chatDbPath();
  if (sqlite3_open_v2(path.string().c_str(), &db_,
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
  LOG_MSG << "ChatDb ready at:" << path.string();
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
    "ON CONFLICT(id) DO UPDATE SET project_id=?2, title=?3, updated_at=?4, messages_json=?5");
  st.bindText(1, id);
  st.bindText(2, projectId);
  st.bindText(3, title);
  st.bindInt64(4, now);
  st.bindText(5, messages);
  if (sqlite3_step(st.ref()) != SQLITE_DONE)
    throw std::runtime_error(std::string("saveChat: ") + sqlite3_errmsg(db_));
  return { {"status", "success"}, {"id", id} };
}

nlohmann::json ChatDb::listChats(const std::string &projectId)
{
  std::lock_guard<std::mutex> lock(mtx_);
  if (!db_) throw std::runtime_error("Chat DB not available");
  utils::SqliteStmt st(db_);
  const char *sql = projectId.empty()
    ? "SELECT id, title, project_id, updated_at FROM chats ORDER BY updated_at DESC LIMIT 500"
    : "SELECT id, title, project_id, updated_at FROM chats WHERE project_id=?1 ORDER BY updated_at DESC LIMIT 500";
  st.stmt_ = prepareOrThrow(db_, sql);
  if (!projectId.empty()) st.bindText(1, projectId);

  nlohmann::json arr = nlohmann::json::array();
  while (sqlite3_step(st.ref()) == SQLITE_ROW) {
    arr.push_back({ {"id", st.getStr(0)}, {"title", st.getStr(1)}, {"project_id", st.getStr(2)}, {"updated_at", st.getInt64(3)} });
  }
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
    "FROM chats WHERE id=?1");
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
  std::lock_guard<std::mutex> lock(mtx_);
  if (!db_) throw std::runtime_error("Chat DB not available");
  if (id.empty()) throw std::runtime_error("deleteChat: chat id required");

  utils::SqliteStmt st(db_);
  st.stmt_ = prepareOrThrow(db_, "DELETE FROM chats WHERE id=?1");
  st.bindText(1, id);
  if (sqlite3_step(st.ref()) != SQLITE_DONE)
    throw std::runtime_error(std::string("deleteChat: ") + sqlite3_errmsg(db_));

  const int deleted = sqlite3_changes(db_);
  if (0 == deleted)
    throw std::runtime_error("Chat not found: " + id);
  return { {"status", "success"}, {"deleted", deleted} };
}
