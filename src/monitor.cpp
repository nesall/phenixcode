#include "monitor.h"
#include "settings.h"
#include "auth.h"
#include "sqlite3_helper.h"
#include <sqlite3.h>
#include <algorithm>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <utils_log/logger.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

  constexpr size_t kReservoirSize = 256;

  std::string getMetricsDbPath() {
    const char *envPath = std::getenv("EMBEDDER_METRICS");
    if (envPath) {
      std::string path = envPath;
      if (path.size() < 7 || path.substr(path.size() - 7) != ".sqlite") {
        path += ".sqlite";
      }
      return path;
    }

#ifdef _WIN32
    const char *home = std::getenv("USERPROFILE");
#else
    const char *home = std::getenv("HOME");
#endif

    if (home) {
      return (fs::path(home) / ".embedder_metrics.sqlite").string();
    }

    return "embedder_metrics.sqlite";
  }

  std::string stateToString(MonitorState s) {
    switch (s) {
    case MonitorState::Idle:     return "idle";
    case MonitorState::Scanning: return "scanning";
    case MonitorState::Indexing: return "indexing";
    case MonitorState::Ready:    return "ready";
    case MonitorState::Error:    return "error";
    }
    return "idle";
  }

  std::string phaseToString(MonitorPhase p) {
    switch (p) {
    case MonitorPhase::Idle:    return "idle";
    case MonitorPhase::Walk:    return "walk";
    case MonitorPhase::Chunk:   return "chunk";
    case MonitorPhase::Embed:   return "embed";
    case MonitorPhase::Persist: return "persist";
    }
    return "idle";
  }

  int64_t nowEpochSeconds() {
    return static_cast<int64_t>(std::time(nullptr));
  }

  std::string defaultSessionId() {
    char host[128] = { 0 };
#ifdef _WIN32
    DWORD sz = sizeof(host);
    GetComputerNameA(host, &sz);
    int pid = static_cast<int>(GetCurrentProcessId());
#else
    gethostname(host, sizeof(host));
    int pid = static_cast<int>(getpid());
#endif
    return std::string(host) + "-" + std::to_string(pid) + "-" + std::to_string(nowEpochSeconds());
  }

  struct LatencyReservoir {
    std::vector<double> samples;
    size_t writeIdx = 0;
    size_t totalSeen = 0;

    LatencyReservoir() {
      samples.reserve(kReservoirSize);
    }

    void add(double ms) {
      if (samples.size() < kReservoirSize) {
        samples.push_back(ms);
      } else {
        samples[writeIdx] = ms;
        writeIdx = (writeIdx + 1) % kReservoirSize;
      }
      totalSeen++;
    }

    std::pair<double, double> percentiles() const {
      if (samples.empty()) return { 0.0, 0.0 };
      std::vector<double> sorted = samples;
      std::sort(sorted.begin(), sorted.end());
      auto pick = [&](double q) {
        double pos = q * (sorted.size() - 1);
        size_t lo = static_cast<size_t>(pos);
        size_t hi = (std::min)(lo + 1, sorted.size() - 1);
        double frac = pos - lo;
        return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
        };
      return { pick(0.50), pick(0.95) };
    }
  };

} // anonymous namespace

struct MonitorTracker::Impl {
  std::string projectId_;
  std::string sessionId_;
  std::string pricingVersion_;
  std::string dbPath_;

  sqlite3 *db_ = nullptr;
  mutable std::mutex dbMtx_;

  mutable std::mutex stateMtx_;
  mutable std::atomic<uint64_t> seq_{ 0 };

  MonitorState state_ = MonitorState::Idle;
  int64_t lastHeartbeat_ = 0;

  // Ephemeral progress
  MonitorPhase phase_ = MonitorPhase::Idle;
  size_t filesTotal_ = 0;
  size_t filesDone_ = 0;
  size_t currentBatch_ = 0;
  size_t currentBatchSize_ = 0;
  size_t chunksTotal_ = 0;
  size_t chunksDone_ = 0;
  int64_t startedAt_ = 0;

  // Ephemeral latency sampling
  LatencyReservoir reservoir_;
  std::unordered_map<std::string, LatencyReservoir> endpointReservoirs_;

  // Background timer
  std::thread timerThread_;
  std::condition_variable cv_;
  std::mutex cvMtx_;
  bool timerRunning_ = false;

  Impl(const Settings &settings, const std::string &dbPath)
    : projectId_(settings.getProjectId())
    , sessionId_(defaultSessionId())
    , dbPath_(dbPath.empty() ? getMetricsDbPath() : dbPath)
    , lastHeartbeat_(nowEpochSeconds())
  {
    if (projectId_.empty()) {
      projectId_ = "default";
    }

    std::string prvDump = settings.providersJson().dump();
    pricingVersion_ = AdminAuth::fnv1a64(prvDump);

    initializeDatabase();
  }

  ~Impl() {
    if (db_) {
      sqlite3_close(db_);
      db_ = nullptr;
    }
  }

  void initializeDatabase() {
    auto parentPath = fs::path(dbPath_).parent_path();
    if (!parentPath.empty() && !fs::exists(parentPath)) {
      std::error_code ec;
      fs::create_directories(parentPath, ec);
    }

    int rc = sqlite3_open(dbPath_.c_str(), &db_);
    if (rc != SQLITE_OK) {
      LOG_MSG_ERROR << "[Monitor] Failed to open metrics database: " << (db_ ? sqlite3_errmsg(db_) : "unknown error");
      if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
      }
      return;
    }

    sqlite3_busy_timeout(db_, 5000);
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);

    const char *sqlCreateTables = R"(
      CREATE TABLE IF NOT EXISTS provider_costs (
        project_id TEXT NOT NULL,
        provider_id TEXT NOT NULL,
        model TEXT NOT NULL,
        calls INTEGER NOT NULL DEFAULT 0,
        tokens_in INTEGER NOT NULL DEFAULT 0,
        tokens_out INTEGER NOT NULL DEFAULT 0,
        cost_input REAL NOT NULL DEFAULT 0.0,
        cost_output REAL NOT NULL DEFAULT 0.0,
        cost_cached REAL NOT NULL DEFAULT 0.0,
        cost_total REAL NOT NULL DEFAULT 0.0,
        last_updated INTEGER NOT NULL,
        PRIMARY KEY (project_id, provider_id)
      );
      CREATE INDEX IF NOT EXISTS idx_provider_costs_project ON provider_costs(project_id);

      CREATE TABLE IF NOT EXISTS endpoint_metrics (
        project_id TEXT NOT NULL,
        endpoint TEXT NOT NULL,
        requests INTEGER NOT NULL DEFAULT 0,
        errors INTEGER NOT NULL DEFAULT 0,
        tokens_in INTEGER NOT NULL DEFAULT 0,
        tokens_out INTEGER NOT NULL DEFAULT 0,
        last_updated INTEGER NOT NULL,
        PRIMARY KEY (project_id, endpoint)
      );
      CREATE INDEX IF NOT EXISTS idx_endpoint_metrics_project ON endpoint_metrics(project_id);

      CREATE TABLE IF NOT EXISTS session_heartbeats (
        session_id TEXT PRIMARY KEY,
        project_id TEXT NOT NULL,
        state TEXT NOT NULL,
        phase TEXT NOT NULL,
        last_heartbeat INTEGER NOT NULL
      );
      CREATE INDEX IF NOT EXISTS idx_session_heartbeats_project ON session_heartbeats(project_id);
    )";

    char *errMsg = nullptr;
    rc = sqlite3_exec(db_, sqlCreateTables, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
      LOG_MSG_ERROR << "[Monitor] Failed to create tables: " << (errMsg ? errMsg : "unknown error");
      if (errMsg) sqlite3_free(errMsg);
    }
  }

  int64_t computeEtaSecondsLocked() const {
    if (phase_ == MonitorPhase::Idle || startedAt_ <= 0) return -1;
    int64_t elapsed = nowEpochSeconds() - startedAt_;
    if (elapsed <= 0) return -1;

    if (chunksTotal_ > 0 && chunksDone_ > 0 && chunksDone_ <= chunksTotal_) {
      double rate = static_cast<double>(chunksDone_) / elapsed;
      if (rate > 0.0) {
        return static_cast<int64_t>((chunksTotal_ - chunksDone_) / rate);
      }
    }
    if (filesTotal_ > 0 && filesDone_ > 0 && filesDone_ <= filesTotal_) {
      double rate = static_cast<double>(filesDone_) / elapsed;
      if (rate > 0.0) {
        return static_cast<int64_t>((filesTotal_ - filesDone_) / rate);
      }
    }
    return -1;
  }
};

MonitorTracker::MonitorTracker(const Settings &settings, const std::string &dbPath)
  : imp(std::make_unique<Impl>(settings, dbPath))
{}

MonitorTracker::~MonitorTracker()
{
  stopPersistTimer();
}

void MonitorTracker::setSessionId(const std::string &sessionId)
{
  std::lock_guard<std::mutex> lock(imp->stateMtx_);
  if (!sessionId.empty()) {
    imp->sessionId_ = sessionId;
  }
}

void MonitorTracker::setState(MonitorState state)
{
  std::lock_guard<std::mutex> lock(imp->stateMtx_);
  imp->state_ = state;
  imp->lastHeartbeat_ = nowEpochSeconds();
  imp->seq_++;
}

void MonitorTracker::touchHeartbeat()
{
  std::lock_guard<std::mutex> lock(imp->stateMtx_);
  imp->lastHeartbeat_ = nowEpochSeconds();
}

void MonitorTracker::beginPhase(MonitorPhase phase, size_t filesTotal, size_t chunksTotal)
{
  std::lock_guard<std::mutex> lock(imp->stateMtx_);
  int64_t now = nowEpochSeconds();
  if (imp->phase_ == MonitorPhase::Idle || imp->startedAt_ == 0) {
    imp->startedAt_ = now;
  }
  imp->phase_ = phase;
  if (filesTotal > 0) imp->filesTotal_ = filesTotal;
  if (chunksTotal > 0) imp->chunksTotal_ = chunksTotal;
  imp->currentBatch_ = 0;
  imp->currentBatchSize_ = 0;

  if (phase == MonitorPhase::Walk) {
    imp->state_ = MonitorState::Scanning;
    imp->filesDone_ = 0;
    imp->chunksDone_ = 0;
    imp->chunksTotal_ = 0;
    imp->startedAt_ = now;
  } else if (phase == MonitorPhase::Idle) {
    imp->state_ = MonitorState::Ready;
  } else {
    imp->state_ = MonitorState::Indexing;
  }

  imp->lastHeartbeat_ = now;
  imp->seq_++;
}

void MonitorTracker::updateProgress(
  MonitorPhase phase,
  size_t filesDone,
  size_t filesTotal,
  size_t chunksDone,
  size_t chunksTotal,
  size_t currentBatch,
  size_t currentBatchSize)
{
  std::lock_guard<std::mutex> lock(imp->stateMtx_);
  int64_t now = nowEpochSeconds();
  if (imp->startedAt_ == 0) imp->startedAt_ = now;
  imp->phase_ = phase;
  imp->filesDone_ = filesDone;
  if (filesTotal > 0) imp->filesTotal_ = filesTotal;
  if (chunksDone > 0 || phase == MonitorPhase::Walk) imp->chunksDone_ = chunksDone;
  if (chunksTotal > 0 || phase == MonitorPhase::Walk) imp->chunksTotal_ = chunksTotal;
  imp->currentBatch_ = currentBatch;
  imp->currentBatchSize_ = currentBatchSize;
  imp->state_ = (phase == MonitorPhase::Walk) ? MonitorState::Scanning : MonitorState::Indexing;
  imp->lastHeartbeat_ = now;
  imp->seq_++;
}

void MonitorTracker::addChunksTotal(size_t delta)
{
  std::lock_guard<std::mutex> lock(imp->stateMtx_);
  imp->chunksTotal_ += delta;
  imp->lastHeartbeat_ = nowEpochSeconds();
  imp->seq_++;
}

void MonitorTracker::addChunksDone(size_t delta)
{
  std::lock_guard<std::mutex> lock(imp->stateMtx_);
  imp->chunksDone_ += delta;
  imp->lastHeartbeat_ = nowEpochSeconds();
  imp->seq_++;
}

void MonitorTracker::finishIndexing(bool ok)
{
  std::lock_guard<std::mutex> lock(imp->stateMtx_);
  imp->phase_ = MonitorPhase::Idle;
  imp->state_ = ok ? MonitorState::Ready : MonitorState::Error;
  imp->currentBatch_ = 0;
  imp->currentBatchSize_ = 0;
  imp->lastHeartbeat_ = nowEpochSeconds();
  imp->seq_++;
}

void MonitorTracker::recordRequest(
  const std::string &endpoint,
  double latencyMs,
  bool isError,
  size_t tokensIn,
  size_t tokensOut)
{
  int64_t now = nowEpochSeconds();
  {
    std::lock_guard<std::mutex> lock(imp->stateMtx_);
    imp->reservoir_.add(latencyMs);
    if (!endpoint.empty()) {
      imp->endpointReservoirs_[endpoint].add(latencyMs);
    }
    imp->lastHeartbeat_ = now;
    imp->seq_++;
  }

  std::lock_guard<std::mutex> lock(imp->dbMtx_);
  if (!imp->db_) return;

  const char *upsertSql = R"(
    INSERT INTO endpoint_metrics (
      project_id, endpoint, requests, errors, tokens_in, tokens_out, last_updated
    ) VALUES (?, ?, 1, ?, ?, ?, ?)
    ON CONFLICT(project_id, endpoint) DO UPDATE SET
      requests = requests + 1,
      errors = errors + excluded.errors,
      tokens_in = tokens_in + excluded.tokens_in,
      tokens_out = tokens_out + excluded.tokens_out,
      last_updated = excluded.last_updated;
  )";

  utils::SqliteStmt stmt(imp->db_);
  if (sqlite3_prepare_v2(imp->db_, upsertSql, -1, &stmt.ref(), nullptr) != SQLITE_OK) {
    LOG_MSG_ERROR << "[Monitor] Failed to prepare endpoint metrics upsert: " << sqlite3_errmsg(imp->db_);
    return;
  }

  const std::string epName = endpoint.empty() ? "unknown" : endpoint;
  sqlite3_bind_text(stmt.ref(), 1, imp->projectId_.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.ref(), 2, epName.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt.ref(), 3, isError ? 1 : 0);
  sqlite3_bind_int64(stmt.ref(), 4, static_cast<int64_t>(tokensIn));
  sqlite3_bind_int64(stmt.ref(), 5, static_cast<int64_t>(tokensOut));
  sqlite3_bind_int64(stmt.ref(), 6, now);

  if (sqlite3_step(stmt.ref()) != SQLITE_DONE) {
    LOG_MSG_ERROR << "[Monitor] Failed to execute endpoint metrics upsert: " << sqlite3_errmsg(imp->db_);
  }
}

void MonitorTracker::recordGenerationCost(
  const std::string &providerId,
  size_t tokensIn,
  size_t tokensOut,
  const ProvidersSettings &providers,
  double cacheHitRatio)
{
  const ApiConfig *cfg = providers.findGeneration(providerId);
  double inCost = 0.0;
  double cachedCost = 0.0;
  double outCost = 0.0;
  std::string modelName = providerId;

  if (cfg) {
    modelName = cfg->model;
    if (cfg->pricing.cachedInput > 0.0f) {
      inCost = (tokensIn * (1.0 - cacheHitRatio) / 1'000'000.0) * cfg->pricing.input;
      cachedCost = (tokensIn * cacheHitRatio / 1'000'000.0) * cfg->pricing.cachedInput;
    } else {
      inCost = (tokensIn / 1'000'000.0) * cfg->pricing.input;
    }
    outCost = (tokensOut / 1'000'000.0) * cfg->pricing.output;
  }
  double totalCost = inCost + cachedCost + outCost;
  int64_t now = nowEpochSeconds();

  std::lock_guard<std::mutex> lock(imp->dbMtx_);
  if (!imp->db_) return;

  const char *upsertSql = R"(
    INSERT INTO provider_costs (
      project_id, provider_id, model, calls, tokens_in, tokens_out,
      cost_input, cost_output, cost_cached, cost_total, last_updated
    ) VALUES (?, ?, ?, 1, ?, ?, ?, ?, ?, ?, ?)
    ON CONFLICT(project_id, provider_id) DO UPDATE SET
      model = excluded.model,
      calls = calls + 1,
      tokens_in = tokens_in + excluded.tokens_in,
      tokens_out = tokens_out + excluded.tokens_out,
      cost_input = cost_input + excluded.cost_input,
      cost_output = cost_output + excluded.cost_output,
      cost_cached = cost_cached + excluded.cost_cached,
      cost_total = cost_total + excluded.cost_total,
      last_updated = excluded.last_updated;
  )";

  utils::SqliteStmt stmt(imp->db_);
  if (sqlite3_prepare_v2(imp->db_, upsertSql, -1, &stmt.ref(), nullptr) != SQLITE_OK) {
    LOG_MSG_ERROR << "[Monitor] Failed to prepare generation cost upsert: " << sqlite3_errmsg(imp->db_);
    return;
  }

  sqlite3_bind_text(stmt.ref(), 1, imp->projectId_.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.ref(), 2, providerId.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.ref(), 3, modelName.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt.ref(), 4, static_cast<int64_t>(tokensIn));
  sqlite3_bind_int64(stmt.ref(), 5, static_cast<int64_t>(tokensOut));
  sqlite3_bind_double(stmt.ref(), 6, inCost);
  sqlite3_bind_double(stmt.ref(), 7, outCost);
  sqlite3_bind_double(stmt.ref(), 8, cachedCost);
  sqlite3_bind_double(stmt.ref(), 9, totalCost);
  sqlite3_bind_int64(stmt.ref(), 10, now);

  if (sqlite3_step(stmt.ref()) != SQLITE_DONE) {
    LOG_MSG_ERROR << "[Monitor] Failed to execute generation cost upsert: " << sqlite3_errmsg(imp->db_);
  }
}

void MonitorTracker::recordEmbeddingCost(
  const std::string &providerId,
  size_t tokensIn,
  const ProvidersSettings &providers)
{
  const ApiConfig *cfg = providers.findEmbedding(providerId);
  double inCost = 0.0;
  std::string modelName = providerId;

  if (cfg) {
    modelName = cfg->model;
    if (cfg->pricing.input > 0.0f) {
      inCost = (tokensIn / 1'000'000.0) * cfg->pricing.input;
    }
  }
  int64_t now = nowEpochSeconds();

  std::lock_guard<std::mutex> lock(imp->dbMtx_);
  if (!imp->db_) return;

  const char *upsertSql = R"(
    INSERT INTO provider_costs (
      project_id, provider_id, model, calls, tokens_in, tokens_out,
      cost_input, cost_output, cost_cached, cost_total, last_updated
    ) VALUES (?, ?, ?, 1, ?, 0, ?, 0.0, 0.0, ?, ?)
    ON CONFLICT(project_id, provider_id) DO UPDATE SET
      model = excluded.model,
      calls = calls + 1,
      tokens_in = tokens_in + excluded.tokens_in,
      cost_input = cost_input + excluded.cost_input,
      cost_total = cost_total + excluded.cost_total,
      last_updated = excluded.last_updated;
  )";

  utils::SqliteStmt stmt(imp->db_);
  if (sqlite3_prepare_v2(imp->db_, upsertSql, -1, &stmt.ref(), nullptr) != SQLITE_OK) {
    LOG_MSG_ERROR << "[Monitor] Failed to prepare embedding cost upsert: " << sqlite3_errmsg(imp->db_);
    return;
  }

  sqlite3_bind_text(stmt.ref(), 1, imp->projectId_.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.ref(), 2, providerId.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt.ref(), 3, modelName.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt.ref(), 4, static_cast<int64_t>(tokensIn));
  sqlite3_bind_double(stmt.ref(), 5, inCost);
  sqlite3_bind_double(stmt.ref(), 6, inCost);
  sqlite3_bind_int64(stmt.ref(), 7, now);

  if (sqlite3_step(stmt.ref()) != SQLITE_DONE) {
    LOG_MSG_ERROR << "[Monitor] Failed to execute embedding cost upsert: " << sqlite3_errmsg(imp->db_);
  }
}

json MonitorTracker::toJson() const
{
  std::lock_guard<std::mutex> stateLock(imp->stateMtx_);
  int64_t now = nowEpochSeconds();
  uint64_t seq = ++imp->seq_;

  auto [p50, p95] = imp->reservoir_.percentiles();

  size_t totalRequests = 0;
  size_t totalErrors = 0;
  size_t totalTokensIn = 0;
  size_t totalTokensOut = 0;
  json byEndpointJson = json::object();

  double genInputCost = 0.0;
  double genOutputCost = 0.0;
  double genCachedInputCost = 0.0;
  double embeddingCost = 0.0;
  double totalCost = 0.0;
  json byProviderJson = json::object();

  {
    std::lock_guard<std::mutex> dbLock(imp->dbMtx_);
    if (imp->db_) {
      // 1. Read endpoint metrics
      const char *selectEpSql = R"(
        SELECT endpoint, requests, errors, tokens_in, tokens_out 
        FROM endpoint_metrics 
        WHERE project_id = ?
      )";
      utils::SqliteStmt epStmt(imp->db_);
      if (sqlite3_prepare_v2(imp->db_, selectEpSql, -1, &epStmt.ref(), nullptr) == SQLITE_OK) {
        sqlite3_bind_text(epStmt.ref(), 1, imp->projectId_.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(epStmt.ref()) == SQLITE_ROW) {
          std::string name = reinterpret_cast<const char *>(sqlite3_column_text(epStmt.ref(), 0));
          size_t reqs = static_cast<size_t>(sqlite3_column_int64(epStmt.ref(), 1));
          size_t errs = static_cast<size_t>(sqlite3_column_int64(epStmt.ref(), 2));
          size_t tIn = static_cast<size_t>(sqlite3_column_int64(epStmt.ref(), 3));
          size_t tOut = static_cast<size_t>(sqlite3_column_int64(epStmt.ref(), 4));

          totalRequests += reqs;
          totalErrors += errs;
          totalTokensIn += tIn;
          totalTokensOut += tOut;

          double epP50 = 0.0;
          double epP95 = 0.0;
          size_t samples = 0;
          auto it = imp->endpointReservoirs_.find(name);
          if (it != imp->endpointReservoirs_.end()) {
            std::tie(epP50, epP95) = it->second.percentiles();
            samples = it->second.samples.size();
          }

          byEndpointJson[name] = {
            { "requests", reqs },
            { "errors", errs },
            { "tokens_in", tIn },
            { "tokens_out", tOut },
            { "latency_p50_ms", epP50 },
            { "latency_p95_ms", epP95 },
            { "latency_samples", samples }
          };
        }
      }

      // 2. Read provider costs
      const char *selectCostSql = R"(
        SELECT provider_id, model, calls, tokens_in, tokens_out,
               cost_input, cost_output, cost_cached, cost_total
        FROM provider_costs
        WHERE project_id = ?
      )";
      utils::SqliteStmt costStmt(imp->db_);
      if (sqlite3_prepare_v2(imp->db_, selectCostSql, -1, &costStmt.ref(), nullptr) == SQLITE_OK) {
        sqlite3_bind_text(costStmt.ref(), 1, imp->projectId_.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(costStmt.ref()) == SQLITE_ROW) {
          std::string providerId = reinterpret_cast<const char *>(sqlite3_column_text(costStmt.ref(), 0));
          std::string model = reinterpret_cast<const char *>(sqlite3_column_text(costStmt.ref(), 1));
          size_t calls = static_cast<size_t>(sqlite3_column_int64(costStmt.ref(), 2));
          size_t tIn = static_cast<size_t>(sqlite3_column_int64(costStmt.ref(), 3));
          size_t tOut = static_cast<size_t>(sqlite3_column_int64(costStmt.ref(), 4));
          double cIn = sqlite3_column_double(costStmt.ref(), 5);
          double cOut = sqlite3_column_double(costStmt.ref(), 6);
          double cCached = sqlite3_column_double(costStmt.ref(), 7);
          double cTotal = sqlite3_column_double(costStmt.ref(), 8);

          if (cOut == 0.0 && cCached == 0.0 && providerId.find("embed") != std::string::npos) {
            embeddingCost += cTotal;
          } else {
            genInputCost += cIn;
            genOutputCost += cOut;
            genCachedInputCost += cCached;
          }
          totalCost += cTotal;

          byProviderJson[providerId] = {
            { "calls", calls },
            { "input", cIn },
            { "output", cOut },
            { "cached_input", cCached },
            { "total", cTotal },
            { "tokens_in", tIn },
            { "tokens_out", tOut },
            { "model", model }
          };
        }
      }
    }
  }

  json costJson = {
    { "generation_input", genInputCost },
    { "generation_output", genOutputCost },
    { "generation_cached_input", genCachedInputCost },
    { "embedding", embeddingCost },
    { "total", totalCost },
    { "pricing_version", imp->pricingVersion_ },
    { "by_provider", byProviderJson }
  };

  if (embeddingCost == 0.0) {
    costJson["note"] = "embedding cost not included";
  }

  return {
    { "project_id", imp->projectId_ },
    { "session_id", imp->sessionId_ },
    { "seq", seq },
    { "ts", now },
    { "state", stateToString(imp->state_) },
    { "last_heartbeat", imp->lastHeartbeat_ },
    { "progress",{
      { "phase", phaseToString(imp->phase_) },
      { "files_total", imp->filesTotal_ },
      { "files_done", imp->filesDone_ },
      { "current_batch", imp->currentBatch_ },
      { "current_batch_size", imp->currentBatchSize_ },
      { "chunks_total", imp->chunksTotal_ },
      { "chunks_done", imp->chunksDone_ },
      { "eta_seconds", imp->computeEtaSecondsLocked() },
      { "started_at", imp->startedAt_ }
    } },
    { "metrics",{
      { "requests", totalRequests },
      { "errors", totalErrors },
      { "tokens_in", totalTokensIn },
      { "tokens_out", totalTokensOut },
      { "latency_p50_ms", p50 },
      { "latency_p95_ms", p95 },
      { "latency_samples", imp->reservoir_.samples.size() },
      { "by_endpoint", byEndpointJson }
    } },
    { "cost", costJson }
  };
}

void MonitorTracker::persist() const
{
  std::lock_guard<std::mutex> stateLock(imp->stateMtx_);
  int64_t now = nowEpochSeconds();
  imp->lastHeartbeat_ = now;

  std::lock_guard<std::mutex> dbLock(imp->dbMtx_);
  if (!imp->db_) return;

  const char *upsertSql = R"(
    INSERT INTO session_heartbeats (
      session_id, project_id, state, phase, last_heartbeat
    ) VALUES (?, ?, ?, ?, ?)
    ON CONFLICT(session_id) DO UPDATE SET
      state = excluded.state,
      phase = excluded.phase,
      last_heartbeat = excluded.last_heartbeat;
  )";

  utils::SqliteStmt stmt(imp->db_);
  if (sqlite3_prepare_v2(imp->db_, upsertSql, -1, &stmt.ref(), nullptr) == SQLITE_OK) {
    sqlite3_bind_text(stmt.ref(), 1, imp->sessionId_.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.ref(), 2, imp->projectId_.c_str(), -1, SQLITE_TRANSIENT);
    std::string stateStr = stateToString(imp->state_);
    std::string phaseStr = phaseToString(imp->phase_);
    sqlite3_bind_text(stmt.ref(), 3, stateStr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.ref(), 4, phaseStr.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt.ref(), 5, now);
    sqlite3_step(stmt.ref());
  }
}

void MonitorTracker::startPersistTimer(int intervalSeconds)
{
  std::lock_guard<std::mutex> lock(imp->cvMtx_);
  if (imp->timerRunning_) return;
  imp->timerRunning_ = true;

  imp->timerThread_ = std::thread([this, intervalSeconds]() {
    persist();
    std::unique_lock<std::mutex> lk(imp->cvMtx_);
    while (imp->timerRunning_) {
      if (imp->cv_.wait_for(lk, std::chrono::seconds(intervalSeconds), [this] { return !imp->timerRunning_; })) {
        break;
      }
      lk.unlock();
      touchHeartbeat();
      persist();
      lk.lock();
    }
    });
}

void MonitorTracker::stopPersistTimer()
{
  {
    std::lock_guard<std::mutex> lock(imp->cvMtx_);
    if (!imp->timerRunning_) return;
    imp->timerRunning_ = false;
  }
  imp->cv_.notify_all();
  if (imp->timerThread_.joinable()) {
    imp->timerThread_.join();
  }
  persist();
}

json MonitorTracker::tailLogFile(LogFileType type, int64_t offset, int64_t count, const Settings &settings)
{
  const fs::path logFile = fs::absolute(settings.loggingLoggingFile()).lexically_normal();
  const fs::path diagFile = fs::absolute(settings.loggingDiagnosticsFile()).lexically_normal();
  const fs::path reqFile{ type == LogFileType::Logs ? logFile : diagFile };

  auto fail = [](const std::string &msg) -> json {
    return {
      { "status", "error" },
      { "message", msg },
      { "lines", json::array() },
      { "total_lines", 0 }
    };
    };

  std::error_code ec;
  if (!fs::exists(reqFile, ec)) {
    return {
      { "status", "success" },
      { "lines", json::array() },
      { "total_lines", 0 }
    };
  }

  std::ifstream in(reqFile, std::ios::binary);
  if (!in.is_open()) {
    return fail("Unable to open log file");
  }

  const bool fromTail = offset < 0;
  const size_t tailN = fromTail ? static_cast<size_t>(-offset) : 0;
  std::deque<std::string> tail;
  json lines = json::array();
  int64_t total = 0;

  std::string line;
  while (std::getline(in, line)) {
    if (in.eof()) break; // unterminated line still writing
    if (!line.empty() && line.back() == '\r') line.pop_back();

    if (fromTail) {
      tail.push_back(std::move(line));
      if (tail.size() > tailN) tail.pop_front();
    } else if (total >= offset && static_cast<int64_t>(lines.size()) < count) {
      lines.push_back(std::move(line));
    }
    ++total;
  }

  if (fromTail) {
    const size_t n = (std::min)(tail.size(), static_cast<size_t>(count));
    for (size_t i = 0; i < n; ++i) lines.push_back(std::move(tail[i]));
  }

  return {
    { "status", "success" },
    { "lines", std::move(lines) },
    { "total_lines", total }
  };
}