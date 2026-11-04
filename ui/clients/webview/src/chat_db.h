#pragma once
#include <string>
#include <mutex>
#include <sqlite3.h>
#include "json_shim.h"

// Per-user chat history at a fixed OS app-data location (not project config).
class ChatDb {
public:
  static ChatDb &instance();
  // Explicit path allows isolated databases (including :memory:) without touching user history.
  explicit ChatDb(const std::string &path);
  ~ChatDb();
  ChatDb(const ChatDb &) = delete;
  ChatDb &operator=(const ChatDb &) = delete;

  nlohmann::json saveChat(const nlohmann::json &chat);      // chat = {id?, project_id, title, messages:[]}
  nlohmann::json listChats(const std::string &projectId, bool deleted = false); // "" = all projects
  nlohmann::json getChat(const std::string &id);
  nlohmann::json deleteChat(const std::string &id);
  nlohmann::json restoreChat(const std::string &id);

private:
  ChatDb();
  nlohmann::json setDeleted(const std::string &id, bool deleted);
  sqlite3 *db_ = nullptr;
  std::mutex mtx_;
};
