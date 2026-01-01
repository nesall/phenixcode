#include "instregistry.h"
#include "settings.h"
#include "utils.h"
#include <sqlite3.h>
#include <filesystem>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <ctime>
#include <sstream>
#include <vector>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#include <sys/utsname.h>
#endif

#include <utils_log/logger.hpp>

namespace {

  std::string getRegistryPath() {
    // Priority:
    // 1. Environment variable
    const char *envPath = std::getenv("EMBEDDER_REGISTRY");
    if (envPath) {
      // Ensure .sqlite extension
      std::string path = envPath;
      if (path.size() < 7 || path.substr(path.size() - 7) != ".sqlite") {
        path += ".sqlite";
      }
      return path;
    }

    // 2. User home directory (shared across projects)
#ifdef _WIN32
    const char *home = std::getenv("USERPROFILE");
#else
    const char *home = std::getenv("HOME");
#endif

    if (home) {
      return std::string(home) + "/.embedder_instances.sqlite";
    }

    // 3. Fallback to current directory
    return "embedder_instances.sqlite";
  }

  std::pair<time_t, std::string> curTimestamp() {
    auto now = std::time(nullptr);
    std::tm tm_now;
#ifdef _WIN32
    localtime_s(&tm_now, &now);
#else
    localtime_r(&now, &tm_now);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_now, "%Y-%m-%d %H:%M:%S");
    std::string nowStr = oss.str();
    return { now, nowStr };
  }

  static int getProcessId() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return getpid();
#endif
  }

  static bool isProcessRunning(int pid) {
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (h) {
      DWORD exitCode;
      GetExitCodeProcess(h, &exitCode);
      CloseHandle(h);
      return exitCode == STILL_ACTIVE;
    }
    return false;
#else
    return kill(pid, 0) == 0;
#endif
  }
}

struct InstanceRegistry::Impl {
  std::string registryPath_;
  std::string instanceId_;
  std::thread heartbeatThread_;
  std::atomic_bool running_{ false };
  sqlite3 *db_{ nullptr };
  mutable std::mutex dbMutex_;
  bool bRegistered_ = false;

  explicit Impl(const std::string &path)
    : registryPath_(path) {
    if (path.empty()) {
      registryPath_ = getRegistryPath();
    }
    instanceId_ = generateInstanceId();
    initializeDatabase();
  }

  ~Impl() {
    stopHeartbeat();
    if (bRegistered_)
      unregister();
    if (db_) {
      sqlite3_close(db_);
    }
  }

  void initializeDatabase() {
    // Create directory if needed
    auto parentPath = std::filesystem::path(registryPath_).parent_path();
    if (!parentPath.empty() && !std::filesystem::exists(parentPath)) {
      std::filesystem::create_directories(parentPath);
    }

    int rc = sqlite3_open(registryPath_.c_str(), &db_);
    if (rc != SQLITE_OK) {
      LOG_MSG << "[REGISTRY] Failed to open registry database: " << sqlite3_errmsg(db_);
      throw std::runtime_error("Failed to open registry database");
    }

    // Enable Write-Ahead Logging for better concurrency
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);

    // Enable foreign keys
    sqlite3_exec(db_, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);

    const char *createTableSQL = R"(
      CREATE TABLE IF NOT EXISTS instances (
        id TEXT PRIMARY KEY,
        pid INTEGER NOT NULL,
        port INTEGER NOT NULL,
        host TEXT NOT NULL DEFAULT 'localhost',
        project_id TEXT,
        name TEXT NOT NULL,
        started_at INTEGER NOT NULL,
        started_at_str TEXT NOT NULL,
        last_heartbeat INTEGER NOT NULL,
        last_heartbeat_str TEXT NOT NULL,
        cwd TEXT NOT NULL,
        config_path TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'healthy',
        created_at INTEGER DEFAULT (strftime('%s', 'now')),
        params TEXT
      );
      
      CREATE INDEX IF NOT EXISTS idx_instances_heartbeat ON instances(last_heartbeat);
      CREATE INDEX IF NOT EXISTS idx_instances_pid ON instances(pid);
      CREATE INDEX IF NOT EXISTS idx_instances_project ON instances(project_id);
    )";

    char *errMsg = nullptr;
    rc = sqlite3_exec(db_, createTableSQL, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
      LOG_MSG << "[REGISTRY] Failed to create tables: " << errMsg;
      sqlite3_free(errMsg);
      throw std::runtime_error("Failed to create database tables");
    }

    // Clean up stale instances on startup
    cleanStaleInstances();
  }

  static std::string generateInstanceId() {
    char hostname[256] = { 0 };
#ifdef _WIN32
    DWORD sz = sizeof(hostname);
    GetComputerNameA(hostname, &sz);
#else
    gethostname(hostname, sizeof(hostname));
#endif
    std::stringstream ss;
    ss << hostname << "-" << getProcessId() << "-" << std::time(nullptr);
    return ss.str();
  }

  std::string detectProjectName() const {
    auto path = std::filesystem::current_path();
    return path.filename().string();
  }

  void cleanStaleInstances() {
    std::lock_guard<std::mutex> lock(dbMutex_);

    const char *cleanSQL = R"(
      DELETE FROM instances 
      WHERE (strftime('%s', 'now') - last_heartbeat) > 60 
         OR NOT is_process_alive(pid)
    )";

    // Note: We'll implement is_process_alive as an application-defined function
    // For now, we'll clean in two steps

    // Step 1: Remove instances with old heartbeats
    const char *cleanOldSQL = "DELETE FROM instances WHERE (strftime('%s', 'now') - last_heartbeat) > 60";

    {
      utils::SqliteStmt stmt(db_);
      int rc = sqlite3_prepare_v2(db_, cleanOldSQL, -1, &stmt.ref(), nullptr);
      if (rc != SQLITE_OK) {
        LOG_MSG << "[REGISTRY] Failed to prepare clean statement: " << sqlite3_errmsg(db_);
        return;
      }

      rc = sqlite3_step(stmt.ref());
      if (rc != SQLITE_DONE) {
        LOG_MSG << "[REGISTRY] Failed to clean old instances: " << sqlite3_errmsg(db_);
      } else {
        int deletedCount = sqlite3_changes(db_);
        if (0 < deletedCount) {
          LOG_MSG << "[REGISTRY] Deleted " << deletedCount << " stale instance(s) with old heartbeats";
        }
      }
    }

    std::vector<std::string> deadInstances;
    {
      // Step 2: Remove instances with dead processes
      // We need to check each process individually
      utils::SqliteStmt stmt(db_);
      const char *selectPidsSQL = "SELECT id, pid FROM instances";
      int rc = sqlite3_prepare_v2(db_, selectPidsSQL, -1, &stmt.ref(), nullptr);
      if (rc != SQLITE_OK) {
        return;
      }

      while (sqlite3_step(stmt.ref()) == SQLITE_ROW) {
        const char *id = reinterpret_cast<const char *>(sqlite3_column_text(stmt.ref(), 0));
        int pid = sqlite3_column_int(stmt.ref(), 1);

        if (!isProcessRunning(pid)) {
          deadInstances.push_back(id);
        }
      }
    }
    // Delete dead instances
    for (const auto &id : deadInstances) {
      const char *deleteSQL = "DELETE FROM instances WHERE id = ?";
      utils::SqliteStmt stmt(db_);
      int rc = sqlite3_prepare_v2(db_, deleteSQL, -1, &stmt.ref(), nullptr);
      if (rc != SQLITE_OK) continue;

      sqlite3_bind_text(stmt.ref(), 1, id.c_str(), -1, SQLITE_STATIC);
      sqlite3_step(stmt.ref());

      if (0 < sqlite3_changes(db_)) {
        LOG_MSG << "[REGISTRY] Deleted stale instance with dead process: " << id;
      }
    }
  }

  void registerInstance(int port, int watchInterval, const Settings &settings) {
    std::lock_guard<std::mutex> lock(dbMutex_);

    const auto [now, nowStr] = curTimestamp();
    auto projectTitle = settings.getProjectTitle();
    std::string name = projectTitle.empty() ? detectProjectName() : projectTitle;

    const char *insertSQL = R"(
      INSERT OR REPLACE INTO instances 
      (id, pid, port, host, project_id, name, started_at, started_at_str, 
       last_heartbeat, last_heartbeat_str, cwd, config_path, status, params)
      VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )";

    utils::SqliteStmt stmt(db_);
    int rc = sqlite3_prepare_v2(db_, insertSQL, -1, &stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
      LOG_MSG << "[REGISTRY] Failed to prepare insert statement: " << sqlite3_errmsg(db_);
      throw std::runtime_error("Failed to register instance");
    }


    // Bind parameters
    sqlite3_bind_text(stmt.ref(), 1, instanceId_.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt.ref(), 2, getProcessId());
    sqlite3_bind_int(stmt.ref(), 3, port);
    sqlite3_bind_text(stmt.ref(), 4, "localhost", -1, SQLITE_STATIC);
    auto projectId = settings.getProjectId();
    sqlite3_bind_text(stmt.ref(), 5, projectId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.ref(), 6, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt.ref(), 7, now);
    sqlite3_bind_text(stmt.ref(), 8, nowStr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt.ref(), 9, now);
    sqlite3_bind_text(stmt.ref(), 10, nowStr.c_str(), -1, SQLITE_TRANSIENT);
    std::string cwd = std::filesystem::current_path().string();
    std::string absConfig = std::filesystem::absolute(settings.configPath()).string();
    sqlite3_bind_text(stmt.ref(), 11, cwd.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.ref(), 12, absConfig.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.ref(), 13, "healthy", -1, SQLITE_STATIC);
    nlohmann::json jsonParams;
    jsonParams["watch_interval"] = watchInterval;
    auto paramsText = jsonParams.dump();
    sqlite3_bind_text(stmt.ref(), 14, paramsText.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt.ref());
    if (rc != SQLITE_DONE) {
      LOG_MSG << "[REGISTRY] Failed to register instance: " << sqlite3_errmsg(db_);
      throw std::runtime_error("Failed to register instance");
    }
    LOG_MSG << "[REGISTRY] Registered instance:" << instanceId_ << "on port" << port;
  }

  void unregister() {
    std::lock_guard<std::mutex> lock(dbMutex_);

    const char *deleteSQL = "DELETE FROM instances WHERE id = ?";
    utils::SqliteStmt stmt(db_);

    int rc = sqlite3_prepare_v2(db_, deleteSQL, -1, &stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
      LOG_MSG << "[REGISTRY] Failed to prepare delete statement: " << sqlite3_errmsg(db_);
      return;
    }

    sqlite3_bind_text(stmt.ref(), 1, instanceId_.c_str(), -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt.ref());
    if (rc != SQLITE_DONE) {
      LOG_MSG << "Failed to unregister instance: " << sqlite3_errmsg(db_);
      return;
    }
    if (0 < sqlite3_changes(db_)) {
      LOG_MSG << "[REGISTRY] Unregistered instance:" << instanceId_;
    } else {
      LOG_MSG << "[REGISTRY] Instance not found for unregistration:" << instanceId_;
    }
  }

  void updateHeartbeat() {
    std::lock_guard<std::mutex> lock(dbMutex_);
    const auto [now, nowStr] = curTimestamp();

    const char *updateSQL = "UPDATE instances SET last_heartbeat = ?, last_heartbeat_str = ?, status = 'healthy' WHERE id = ?";

    utils::SqliteStmt stmt(db_);
    int rc = sqlite3_prepare_v2(db_, updateSQL, -1, &stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
      return;
    }

    sqlite3_bind_int64(stmt.ref(), 1, now);
    sqlite3_bind_text(stmt.ref(), 2, nowStr.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt.ref(), 3, instanceId_.c_str(), -1, SQLITE_STATIC);

    sqlite3_step(stmt.ref());
  }
  
  void startHeartbeat() {
    if (running_) {
      return;
    }
    running_ = true;
    heartbeatThread_ = std::thread([this]() {
      while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        try {
          {
            if (!running_) return;
            updateHeartbeat();
            cleanStaleInstances();
          }
        } catch (const std::exception &ex) {
          LOG_MSG << "[REGISTRY] Heartbeat update failed: " << ex.what();
        }
      }
      });
  }

  void stopHeartbeat() {
    running_ = false;
    if (heartbeatThread_.joinable()) {
      heartbeatThread_.join();
    }
  }

  std::vector<nlohmann::json> getActiveInstances() const {
    std::lock_guard<std::mutex> lock(dbMutex_);

    const char *selectSQL = R"(
      SELECT id, pid, port, host, project_id, name, started_at, 
             started_at_str, last_heartbeat, last_heartbeat_str, 
             cwd, config_path, status, params
      FROM instances 
      WHERE (strftime('%s', 'now') - last_heartbeat) < 30
      ORDER BY last_heartbeat DESC
    )";

    utils::SqliteStmt stmt(db_);
    int rc = sqlite3_prepare_v2(db_, selectSQL, -1, &stmt.ref(), nullptr);
    if (rc != SQLITE_OK) {
      LOG_MSG << "[REGISTRY] Failed to prepare select statement: " << sqlite3_errmsg(db_);
      return {};
    }

    std::vector<nlohmann::json> active;
    while (sqlite3_step(stmt.ref()) == SQLITE_ROW) {
      nlohmann::json instance;
      int j = 0;
      instance["id"] = stmt.getStr(j++);
      instance["pid"] = stmt.getInt(j++);
      instance["port"] = stmt.getInt(j++);
      instance["host"] = stmt.getStr(j++);
      instance["project_id"] = stmt.getStr(j++);
      instance["name"] = stmt.getStr(j++);
      instance["started_at"] = stmt.getInt64(j++);
      instance["started_at_str"] = stmt.getStr(j++);
      instance["last_heartbeat"] = stmt.getInt64(j++);
      instance["last_heartbeat_str"] = stmt.getStr(j++);;
      instance["cwd"] = stmt.getStr(j++);
      instance["config"] = stmt.getStr(j++);
      instance["status"] = stmt.getStr(j++);
      std::string params = stmt.getStr(j++);      
      try {
        instance["params"] = nlohmann::json::parse(params);
      } catch (const nlohmann::json::exception &e) {
        LOG_MSG << "[REGISTRY] Failed to parse params JSON: " << e.what();
        instance["params"] = nlohmann::json::object();
      }
      active.push_back(instance);
    }
    return active;
  }
};

InstanceRegistry::InstanceRegistry(const std::string &registryPath)
  : imp(std::make_unique<Impl>(registryPath))
{
}

InstanceRegistry::InstanceRegistry(int port, int watchInterval, const Settings &settings, const std::string &registryPath)
  : imp(std::make_unique<Impl>(registryPath))
{
  imp->bRegistered_ = true;
  registerInstance(port, watchInterval, settings);
}

InstanceRegistry::~InstanceRegistry() = default;

void InstanceRegistry::registerInstance(int port, int watchInterval, const Settings &settings)
{
  imp->registerInstance(port, watchInterval, settings);
}

void InstanceRegistry::unregister()
{
  imp->unregister();
}

void InstanceRegistry::startHeartbeat()
{
  imp->startHeartbeat();
}

void InstanceRegistry::stopHeartbeat()
{
  imp->stopHeartbeat();
}

std::vector<nlohmann::json> InstanceRegistry::getActiveInstances() const
{
  return imp->getActiveInstances();
}

std::string InstanceRegistry::getInstanceId() const
{
  return imp->instanceId_;
}