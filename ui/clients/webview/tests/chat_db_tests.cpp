#include "chat_db.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <chrono>

// The ID generator lives in the process-management translation unit. Keep this
// persistence test independent of launching processes or a webview.
namespace shared {
  std::string generateRandomId(size_t) { return "generated-test-id"; }
}

void check(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

template<class F> void rejects(F operation) {
  bool rejected = false;
  try { operation(); } catch (const std::exception &) { rejected = true; }
  check(rejected, "Expected operation to reject");
}

void sql(sqlite3 *db, const char *query) {
  if (sqlite3_exec(db, query, nullptr, nullptr, nullptr) != SQLITE_OK)
    throw std::runtime_error(sqlite3_errmsg(db));
}

int main() {
  const auto path = std::filesystem::temp_directory_path() /
    ("phenixcode-chat-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
  try {
    sqlite3 *legacy = nullptr;
    check(sqlite3_open(path.string().c_str(), &legacy) == SQLITE_OK, "Create test fixture");
    sql(legacy, "CREATE TABLE chats(id TEXT PRIMARY KEY, project_id TEXT NOT NULL DEFAULT '', title TEXT NOT NULL DEFAULT '',"
      "created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL, messages_json TEXT NOT NULL);"
      "INSERT INTO chats VALUES('legacy', 'project', 'Original title', 100, 200,"
      "'[{\"role\":\"system\",\"content\":\"System\"},{\"role\":\"user\",\"content\":\"Original content\"}]');");
    sqlite3_close(legacy);
    nlohmann::json original;
    {
      ChatDb db(path.string());
      auto active = db.listChats("")["chats"];
      check(active.size() == 1 && active[0]["id"] == "legacy", "Legacy chat remains active after migration");
      check(active[0]["preview"] == "Original content", "Preview uses first user message");
      check(db.listChats("other")["chats"].empty(), "Project filtering");
      original = db.getChat("legacy")["chat"];
      check(original["updated_at"] == 200, "Migration preserves timestamp");
      db.deleteChat("legacy");
      check(db.listChats("")["chats"].empty(), "Deleted chat excluded from Active");
      rejects([&] { db.getChat("legacy"); });
      auto trash = db.listChats("project", true)["chats"];
      check(trash.size() == 1 && trash[0]["deleted_at"].is_number_integer(), "Deleted chat appears in Trash");
      check(trash[0]["updated_at"] == 200, "Deletion preserves updated_at");
      sqlite3 *inspection = nullptr;
      check(sqlite3_open_v2(path.string().c_str(), &inspection, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK, "Inspect test database");
      sqlite3_stmt *row = nullptr;
      check(sqlite3_prepare_v2(inspection, "SELECT free_chat, messages_json, updated_at FROM chats WHERE id='legacy' AND deleted_at IS NOT NULL",
        -1, &row, nullptr) == SQLITE_OK, "Inspect soft-deleted row");
      check(sqlite3_step(row) == SQLITE_ROW, "Deleted row remains in SQLite");
      check(sqlite3_column_int(row, 0) == 0, "Legacy chat defaults to not free_chat");
      check(nlohmann::json::parse(reinterpret_cast<const char *>(sqlite3_column_text(row, 1))) == original["messages"], "SQLite retains original messages while deleted");
      check(sqlite3_column_int64(row, 2) == 200, "SQLite retains modification timestamp while deleted");
      sqlite3_finalize(row);
      sqlite3_close(inspection);
      rejects([&] { db.deleteChat("legacy"); });
      rejects([&] { db.saveChat(original); });
    }
    {
      ChatDb db(path.string()); // migration must be idempotent; Trash survives restart
      check(db.listChats("", true)["chats"].size() == 1, "Trash persists on disk");
      db.restoreChat("legacy");
      check(db.getChat("legacy")["chat"] == original, "Restore recovers all original fields and messages");
      check(db.listChats("", true)["chats"].empty(), "Restored chat removed from Trash");
      rejects([&] { db.restoreChat("legacy"); });
      db.deleteChat("legacy");
      db.restoreChat("legacy"); // same operation used by Undo
      check(db.getChat("legacy")["chat"] == original, "Undo preserves content and updated_at");
      auto modified = original;
      modified["title"] = "Updated title";
      modified["messages"].push_back({{"role", "assistant"}, {"content", "Reply"}});
      db.saveChat(modified);
      auto saved = db.getChat("legacy")["chat"];
      check(saved["title"] == "Updated title" && saved["messages"].size() == 3, "Existing chats can still be updated");
      check(saved["created_at"] == 100, "Updating preserves creation date");
      modified.erase("id");
      const auto id = db.saveChat(modified)["id"].get<std::string>();
      check(db.getChat(id)["chat"]["messages"] == modified["messages"], "New chats can be created and loaded");
      check(db.listChats("")["chats"][0]["updated_at"].get<int64_t>() >= 200, "Active ordering");
      rejects([&] { db.deleteChat("missing"); });
      rejects([&] { db.restoreChat(""); });
    }
    {
      ChatDb fresh(":memory:");
      check(fresh.listChats("")["chats"].empty(), "Fresh database initializes");
      auto id = fresh.saveChat({{"messages", nlohmann::json::array()}})["id"].get<std::string>();
      check(fresh.getChat(id)["chat"]["messages"].empty(), "Empty chat round trip");
      std::string unicode;
      for (int i = 0; i < 241; ++i) unicode += "\xf0\x9f\x98\x80";
      fresh.saveChat({{"id", id}, {"messages", {{{"role", "user"}, {"content", unicode}}}}});
      const auto preview = fresh.listChats("")["chats"][0]["preview"].get<std::string>();
      check(preview.size() == 100 * 4 + 3, "Preview truncation preserves Unicode boundaries and appends three dots");
      fresh.listChats("").dump(); // Serialization also validates UTF-8.

      auto checkPreview = [&](nlohmann::json::initializer_list_t turns, const std::string &expected) {
        const auto messages = nlohmann::json::array(turns);
        fresh.saveChat({{"id", id}, {"messages", messages}});
        check(fresh.listChats("")["chats"][0]["preview"] == expected, "Active preview matches the two-turn format");
        check(fresh.getChat(id)["chat"]["messages"] == messages, "Building a preview does not alter stored messages");
        fresh.deleteChat(id);
        check(fresh.listChats("", true)["chats"][0]["preview"] == expected, "Trash uses the same preview format");
        fresh.restoreChat(id);
      };
      auto message = [](const char *role, const std::string &content) {
        return nlohmann::json{{"role", role}, {"content", content}};
      };
      checkPreview({}, "");
      checkPreview({message("system", "Instructions"), message("assistant", "No question yet")}, "");
      checkPreview({message("user", "Question\r\nline\rnext\nlast")}, "Question line next last");
      checkPreview({message("system", "Instructions"), message("assistant", "Earlier response"),
        message("user", "Question\r\nline\rnext\nlast"), message("system", "Context"),
        message("assistant", "Answer\r\nline\rnext\nlast"), message("user", "Later question"),
        message("assistant", "Later answer")}, "Question line next last\nAnswer line next last");
      checkPreview({message("user", std::string(99, 'a') + "\r\nz"),
        message("assistant", std::string(101, 'b'))}, std::string(99, 'a') + " z\n" + std::string(101, 'b'));
      checkPreview({message("user", unicode), message("assistant", unicode)},
        unicode.substr(0, 100 * 4) + "...\n" + unicode.substr(0, 100 * 4) + "...");
      for (size_t length = 99; length <= 104; ++length) {
        const auto user = std::string(length, 'a');
        const auto response = unicode.substr(0, length * 4);
        const auto expectedUser = length <= 103 ? user : std::string(100, 'a') + "...";
        const auto expectedResponse = length <= 103 ? response : unicode.substr(0, 100 * 4) + "...";
        checkPreview({message("user", user), message("assistant", response)}, expectedUser + "\n" + expectedResponse);
      }
      checkPreview({message("user", std::string(100, 'a') + "\r\nxyz")}, std::string(100, 'a') + "...");
      checkPreview({message("user", std::string(100, 'a') + "\r\nxy")}, std::string(100, 'a') + " xy");
      checkPreview({message("user", "Unanswered"), message("user", "Later question"),
        message("assistant", "Later answer")}, "Unanswered");
      checkPreview({message("user", ""), message("assistant", "Answer")}, "\nAnswer");
    }
    std::filesystem::remove(path);
    std::cout << "Chat persistence tests passed\n";
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << ex.what() << '\n';
    std::filesystem::remove(path);
    return 1;
  }
}
