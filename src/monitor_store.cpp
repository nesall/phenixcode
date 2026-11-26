#include "monitor_store.h"
#include "sqlite3_helper.h"
#include <sqlite3.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <unordered_map>
#include <utils_log/logger.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace monitor_store {

  namespace {

    bool toLocalTm(int64_t epochSeconds, std::tm &out) {
      std::time_t t = static_cast<std::time_t>(epochSeconds);
#ifdef _WIN32
      return localtime_s(&out, &t) == 0;
#else
      return localtime_r(&t, &out) != nullptr;
#endif
    }

    std::string formatDay(const std::tm &tmVal) {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                    tmVal.tm_year + 1900, tmVal.tm_mon + 1, tmVal.tm_mday);
      return std::string(buf);
    }

    std::string formatMonth(int year, int month1Based) {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%04d-%02d", year, month1Based);
      return std::string(buf);
    }

    std::string shiftLocalDay(const std::tm &baseTm, int deltaDays) {
      std::tm shifted = baseTm;
      shifted.tm_hour = 12;
      shifted.tm_min = 0;
      shifted.tm_sec = 0;
      shifted.tm_isdst = -1;
      shifted.tm_mday += deltaDays;
      std::time_t res = std::mktime(&shifted);
      if (res == static_cast<std::time_t>(-1)) {
        return formatDay(baseTm);
      }
      std::tm out{};
      if (!toLocalTm(static_cast<int64_t>(res), out)) {
        return formatDay(shifted);
      }
      return formatDay(out);
    }

    bool parseDay(const std::string &day, int &year, int &month, int &mday) {
      if (day.size() != 10 || day[4] != '-' || day[7] != '-') return false;
      int y = 0, m = 0, d = 0;
      if (std::sscanf(day.c_str(), "%4d-%2d-%2d", &y, &m, &d) != 3) return false;
      if (m < 1 || m > 12 || d < 1 || d > 31) return false;
      year = y;
      month = m;
      mday = d;
      return true;
    }

    std::time_t localNoonEpoch(int year, int month, int mday) {
      std::tm tmVal{};
      tmVal.tm_year = year - 1900;
      tmVal.tm_mon = month - 1;
      tmVal.tm_mday = mday;
      tmVal.tm_hour = 12;
      tmVal.tm_min = 0;
      tmVal.tm_sec = 0;
      tmVal.tm_isdst = -1;
      return std::mktime(&tmVal);
    }

    int64_t daysBetween(const std::string &fromDay, const std::string &toDay) {
      int y1 = 0, m1 = 0, d1 = 0;
      int y2 = 0, m2 = 0, d2 = 0;
      if (!parseDay(fromDay, y1, m1, d1) || !parseDay(toDay, y2, m2, d2)) return 0;
      std::time_t t1 = localNoonEpoch(y1, m1, d1);
      std::time_t t2 = localNoonEpoch(y2, m2, d2);
      if (t1 == static_cast<std::time_t>(-1) || t2 == static_cast<std::time_t>(-1)) return 0;
      return static_cast<int64_t>(std::llround(std::difftime(t2, t1) / 86400.0));
    }

    int daysInMonth(int year, int month1Based) {
      static constexpr int kDays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
      if (month1Based < 1 || month1Based > 12) return 30;
      if (month1Based == 2) {
        bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
        return leap ? 29 : 28;
      }
      return kDays[month1Based - 1];
    }

    std::string columnText(sqlite3_stmt *stmt, int col) {
      const unsigned char *txt = sqlite3_column_text(stmt, col);
      return txt ? reinterpret_cast<const char *>(txt) : std::string{};
    }

    bool execSql(sqlite3 *db, const char *sql, const char *context) {
      char *errMsg = nullptr;
      int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
      if (rc != SQLITE_OK) {
        LOG_MSG_ERROR << "[Monitor] " << context << ": " << (errMsg ? errMsg : sqlite3_errmsg(db));
        if (errMsg) sqlite3_free(errMsg);
        return false;
      }
      return true;
    }

  } // anonymous namespace

  std::string defaultMetricsDbPath() {
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

  bool ensureSchema(sqlite3 *db) {
    if (!db) return false;

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

      CREATE TABLE IF NOT EXISTS provider_cost_daily (
        project_id TEXT NOT NULL,
        day TEXT NOT NULL,
        provider_id TEXT NOT NULL,
        kind TEXT NOT NULL,
        calls INTEGER NOT NULL DEFAULT 0,
        tokens_in INTEGER NOT NULL DEFAULT 0,
        tokens_out INTEGER NOT NULL DEFAULT 0,
        cost_input REAL NOT NULL DEFAULT 0.0,
        cost_output REAL NOT NULL DEFAULT 0.0,
        cost_cached REAL NOT NULL DEFAULT 0.0,
        cost_total REAL NOT NULL DEFAULT 0.0,
        PRIMARY KEY (project_id, day, provider_id, kind)
      );
      CREATE INDEX IF NOT EXISTS idx_provider_cost_daily_project_day ON provider_cost_daily(project_id, day);

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

      CREATE TABLE IF NOT EXISTS tool_usage (
        project_id TEXT NOT NULL,
        tool TEXT NOT NULL,
        caller TEXT NOT NULL,
        calls INTEGER NOT NULL,
        errors INTEGER NOT NULL,
        last_used INTEGER NOT NULL,
        PRIMARY KEY (project_id, tool, caller)
      );

      CREATE TABLE IF NOT EXISTS session_heartbeats (
        session_id TEXT PRIMARY KEY,
        project_id TEXT NOT NULL,
        state TEXT NOT NULL,
        phase TEXT NOT NULL,
        last_heartbeat INTEGER NOT NULL
      );
      CREATE INDEX IF NOT EXISTS idx_session_heartbeats_project ON session_heartbeats(project_id);
    )";

    return execSql(db, sqlCreateTables, "Failed to create tables");
  }

  std::string localDay(int64_t epochSeconds) {
    std::tm tmVal{};
    if (!toLocalTm(epochSeconds, tmVal)) {
      return "1970-01-01";
    }
    return formatDay(tmVal);
  }

  bool recordCost(sqlite3 *db, const CostEntry &e) {
    if (!db) return false;

    const bool isEmbedding = (e.kind == "embedding");
    const std::string kindStr = isEmbedding ? "embedding" : "generation";
    const int64_t tokensOut = isEmbedding ? 0 : e.tokensOut;
    const double costOutput = isEmbedding ? 0.0 : e.costOutput;
    const double costCached = isEmbedding ? 0.0 : e.costCached;
    const double totalCost = e.costInput + costOutput + costCached;
    const std::string day = localDay(e.ts);

    if (!execSql(db, "BEGIN IMMEDIATE;", "Failed to begin cost transaction")) {
      return false;
    }

    auto rollback = [&]() {
      char *errMsg = nullptr;
      if (sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, &errMsg) != SQLITE_OK) {
        if (errMsg) sqlite3_free(errMsg);
      }
    };

    if (isEmbedding) {
      const char *upsertLegacySql = R"(
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
      utils::SqliteStmt stmt(db);
      if (sqlite3_prepare_v2(db, upsertLegacySql, -1, &stmt.ref(), nullptr) != SQLITE_OK) {
        LOG_MSG_ERROR << "[Monitor] Failed to prepare embedding cost upsert: " << sqlite3_errmsg(db);
        rollback();
        return false;
      }
      sqlite3_bind_text(stmt.ref(), 1, e.projectId.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt.ref(), 2, e.providerId.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt.ref(), 3, e.model.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(stmt.ref(), 4, e.tokensIn);
      sqlite3_bind_double(stmt.ref(), 5, e.costInput);
      sqlite3_bind_double(stmt.ref(), 6, totalCost);
      sqlite3_bind_int64(stmt.ref(), 7, e.ts);
      if (sqlite3_step(stmt.ref()) != SQLITE_DONE) {
        LOG_MSG_ERROR << "[Monitor] Failed to execute embedding cost upsert: " << sqlite3_errmsg(db);
        rollback();
        return false;
      }
    } else {
      const char *upsertLegacySql = R"(
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
      utils::SqliteStmt stmt(db);
      if (sqlite3_prepare_v2(db, upsertLegacySql, -1, &stmt.ref(), nullptr) != SQLITE_OK) {
        LOG_MSG_ERROR << "[Monitor] Failed to prepare generation cost upsert: " << sqlite3_errmsg(db);
        rollback();
        return false;
      }
      sqlite3_bind_text(stmt.ref(), 1, e.projectId.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt.ref(), 2, e.providerId.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt.ref(), 3, e.model.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(stmt.ref(), 4, e.tokensIn);
      sqlite3_bind_int64(stmt.ref(), 5, tokensOut);
      sqlite3_bind_double(stmt.ref(), 6, e.costInput);
      sqlite3_bind_double(stmt.ref(), 7, costOutput);
      sqlite3_bind_double(stmt.ref(), 8, costCached);
      sqlite3_bind_double(stmt.ref(), 9, totalCost);
      sqlite3_bind_int64(stmt.ref(), 10, e.ts);
      if (sqlite3_step(stmt.ref()) != SQLITE_DONE) {
        LOG_MSG_ERROR << "[Monitor] Failed to execute generation cost upsert: " << sqlite3_errmsg(db);
        rollback();
        return false;
      }
    }

    const char *upsertDailySql = R"(
      INSERT INTO provider_cost_daily (
        project_id, day, provider_id, kind, calls, tokens_in, tokens_out,
        cost_input, cost_output, cost_cached, cost_total
      ) VALUES (?, ?, ?, ?, 1, ?, ?, ?, ?, ?, ?)
      ON CONFLICT(project_id, day, provider_id, kind) DO UPDATE SET
        calls = calls + 1,
        tokens_in = tokens_in + excluded.tokens_in,
        tokens_out = tokens_out + excluded.tokens_out,
        cost_input = cost_input + excluded.cost_input,
        cost_output = cost_output + excluded.cost_output,
        cost_cached = cost_cached + excluded.cost_cached,
        cost_total = cost_total + excluded.cost_total;
    )";
    {
      utils::SqliteStmt dailyStmt(db);
      if (sqlite3_prepare_v2(db, upsertDailySql, -1, &dailyStmt.ref(), nullptr) != SQLITE_OK) {
        LOG_MSG_ERROR << "[Monitor] Failed to prepare daily cost upsert: " << sqlite3_errmsg(db);
        rollback();
        return false;
      }
      sqlite3_bind_text(dailyStmt.ref(), 1, e.projectId.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(dailyStmt.ref(), 2, day.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(dailyStmt.ref(), 3, e.providerId.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(dailyStmt.ref(), 4, kindStr.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(dailyStmt.ref(), 5, e.tokensIn);
      sqlite3_bind_int64(dailyStmt.ref(), 6, tokensOut);
      sqlite3_bind_double(dailyStmt.ref(), 7, e.costInput);
      sqlite3_bind_double(dailyStmt.ref(), 8, costOutput);
      sqlite3_bind_double(dailyStmt.ref(), 9, costCached);
      sqlite3_bind_double(dailyStmt.ref(), 10, totalCost);
      if (sqlite3_step(dailyStmt.ref()) != SQLITE_DONE) {
        LOG_MSG_ERROR << "[Monitor] Failed to execute daily cost upsert: " << sqlite3_errmsg(db);
        rollback();
        return false;
      }
    }

    if (!execSql(db, "COMMIT;", "Failed to commit cost transaction")) {
      rollback();
      return false;
    }

    return true;
  }

  json readPersistedMetrics(sqlite3 *db, const std::string &projectId) {
    size_t totalRequests = 0;
    size_t totalErrors = 0;
    size_t totalTokensIn = 0;
    size_t totalTokensOut = 0;
    json byEndpointJson = json::object();
    json toolUsageJson = json::array();

    if (db) {
      const char *selectEpSql = R"(
        SELECT endpoint, requests, errors, tokens_in, tokens_out
        FROM endpoint_metrics
        WHERE project_id = ?
      )";
      utils::SqliteStmt epStmt(db);
      if (sqlite3_prepare_v2(db, selectEpSql, -1, &epStmt.ref(), nullptr) == SQLITE_OK) {
        sqlite3_bind_text(epStmt.ref(), 1, projectId.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(epStmt.ref()) == SQLITE_ROW) {
          std::string name = columnText(epStmt.ref(), 0);
          size_t reqs = static_cast<size_t>(sqlite3_column_int64(epStmt.ref(), 1));
          size_t errs = static_cast<size_t>(sqlite3_column_int64(epStmt.ref(), 2));
          size_t tIn = static_cast<size_t>(sqlite3_column_int64(epStmt.ref(), 3));
          size_t tOut = static_cast<size_t>(sqlite3_column_int64(epStmt.ref(), 4));

          totalRequests += reqs;
          totalErrors += errs;
          totalTokensIn += tIn;
          totalTokensOut += tOut;

          byEndpointJson[name] = {
            { "requests", reqs },
            { "errors", errs },
            { "tokens_in", tIn },
            { "tokens_out", tOut }
          };
        }
      }

      const char *selectToolSql = R"(
        SELECT tool, caller, calls, errors, last_used
        FROM tool_usage
        WHERE project_id = ?
        ORDER BY tool, caller
      )";
      utils::SqliteStmt toolStmt(db);
      if (sqlite3_prepare_v2(db, selectToolSql, -1, &toolStmt.ref(), nullptr) == SQLITE_OK) {
        sqlite3_bind_text(toolStmt.ref(), 1, projectId.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(toolStmt.ref()) == SQLITE_ROW) {
          toolUsageJson.push_back({
            { "tool", columnText(toolStmt.ref(), 0) },
            { "caller", columnText(toolStmt.ref(), 1) },
            { "calls", sqlite3_column_int64(toolStmt.ref(), 2) },
            { "errors", sqlite3_column_int64(toolStmt.ref(), 3) },
            { "last_used", sqlite3_column_int64(toolStmt.ref(), 4) }
          });
        }
      }
    }

    return {
      { "requests", totalRequests },
      { "errors", totalErrors },
      { "tokens_in", totalTokensIn },
      { "tokens_out", totalTokensOut },
      { "by_endpoint", byEndpointJson },
      { "tool_usage", toolUsageJson }
    };
  }

  json readCost(sqlite3 *db, const std::string &projectId, const std::string &pricingVersion, int64_t nowEpoch) {
    double genInputCost = 0.0;
    double genOutputCost = 0.0;
    double genCachedInputCost = 0.0;
    double embeddingCost = 0.0;
    double totalCost = 0.0;
    json byProviderJson = json::object();

    if (db) {
      const char *selectCostSql = R"(
        SELECT provider_id, model, calls, tokens_in, tokens_out,
               cost_input, cost_output, cost_cached, cost_total
        FROM provider_costs
        WHERE project_id = ?
      )";
      utils::SqliteStmt costStmt(db);
      if (sqlite3_prepare_v2(db, selectCostSql, -1, &costStmt.ref(), nullptr) == SQLITE_OK) {
        sqlite3_bind_text(costStmt.ref(), 1, projectId.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(costStmt.ref()) == SQLITE_ROW) {
          std::string providerId = columnText(costStmt.ref(), 0);
          std::string model = columnText(costStmt.ref(), 1);
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

    json costJson = {
      { "generation_input", genInputCost },
      { "generation_output", genOutputCost },
      { "generation_cached_input", genCachedInputCost },
      { "embedding", embeddingCost },
      { "total", totalCost },
      { "pricing_version", pricingVersion },
      { "by_provider", byProviderJson }
    };

    if (embeddingCost == 0.0) {
      costJson["note"] = "embedding cost not included";
    }

    if (!db) {
      return costJson;
    }

    const char *selectSummarySql = R"(
      SELECT MIN(day), COALESCE(SUM(cost_total), 0.0),
        COALESCE(SUM(CASE WHEN day = ?2 THEN cost_total END), 0.0),
        COALESCE(SUM(CASE WHEN day BETWEEN ?3 AND ?2 THEN cost_total END), 0.0),
        COALESCE(SUM(CASE WHEN day BETWEEN ?4 AND ?2 THEN cost_total END), 0.0),
        COALESCE(SUM(CASE WHEN substr(day, 1, 7) = ?5 THEN cost_total END), 0.0),
        COALESCE(SUM(CASE WHEN substr(day, 1, 4) = ?6 AND day <= ?2 THEN cost_total END), 0.0)
      FROM provider_cost_daily
      WHERE project_id = ?1 AND kind = 'generation'
    )";

    utils::SqliteStmt summaryStmt(db);
    if (sqlite3_prepare_v2(db, selectSummarySql, -1, &summaryStmt.ref(), nullptr) != SQLITE_OK) {
      // Legacy DB without provider_cost_daily table: omit summary/monthly silently.
      return costJson;
    }

    std::tm nowTm{};
    if (!toLocalTm(nowEpoch, nowTm)) {
      return costJson;
    }

    const std::string today = formatDay(nowTm);
    const std::string start7 = shiftLocalDay(nowTm, -6);
    const std::string start30 = shiftLocalDay(nowTm, -29);
    const std::string monthPrefix = today.substr(0, 7);
    const std::string yearPrefix = today.substr(0, 4);

    sqlite3_bind_text(summaryStmt.ref(), 1, projectId.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(summaryStmt.ref(), 2, today.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(summaryStmt.ref(), 3, start7.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(summaryStmt.ref(), 4, start30.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(summaryStmt.ref(), 5, monthPrefix.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(summaryStmt.ref(), 6, yearPrefix.c_str(), -1, SQLITE_TRANSIENT);

    std::string historySince;
    double trackedTotal = 0.0;
    double todayCost = 0.0;
    double last7d = 0.0;
    double last30d = 0.0;
    double thisMonth = 0.0;
    double ytd = 0.0;

    if (sqlite3_step(summaryStmt.ref()) == SQLITE_ROW &&
        sqlite3_column_type(summaryStmt.ref(), 0) != SQLITE_NULL) {
      historySince = columnText(summaryStmt.ref(), 0);
      trackedTotal = sqlite3_column_double(summaryStmt.ref(), 1);
      todayCost = sqlite3_column_double(summaryStmt.ref(), 2);
      last7d = sqlite3_column_double(summaryStmt.ref(), 3);
      last30d = sqlite3_column_double(summaryStmt.ref(), 4);
      thisMonth = sqlite3_column_double(summaryStmt.ref(), 5);
      ytd = sqlite3_column_double(summaryStmt.ref(), 6);
    }

    int monthsTracked = 0;
    double avgMonthly = 0.0;
    double avgDaily30d = 0.0;
    double projectedMonth = 0.0;
    json monthlyJson = json::array();

    int histY = 0, histM = 0, histD = 0;
    if (!historySince.empty() && parseDay(historySince, histY, histM, histD)) {
      const int curY = nowTm.tm_year + 1900;
      const int curM = nowTm.tm_mon + 1;
      const int curD = nowTm.tm_mday;

      monthsTracked = (std::max)(1, (curY * 12 + curM) - (histY * 12 + histM) + 1);
      avgMonthly = trackedTotal / static_cast<double>(monthsTracked);

      const int64_t elapsedDays = (std::max)(int64_t(1), daysBetween(historySince, today) + 1);
      const int64_t avgWindow = (std::min)(int64_t(30), elapsedDays);
      avgDaily30d = last30d / static_cast<double>(avgWindow);

      int elapsedInMonth = curD;
      if (histY == curY && histM == curM) {
        elapsedInMonth = curD - (histD - 1);
      }
      elapsedInMonth = (std::max)(1, elapsedInMonth);
      projectedMonth = (thisMonth / static_cast<double>(elapsedInMonth)) * static_cast<double>(daysInMonth(curY, curM));

      const int monthCount = (std::min)(12, monthsTracked);
      const int curMonthZero = curY * 12 + (curM - 1);
      const int startMonthZero = curMonthZero - (monthCount - 1);
      const std::string startMonth = formatMonth(startMonthZero / 12, (startMonthZero % 12) + 1);

      const char *selectMonthlySql = R"(
        SELECT substr(day, 1, 7) AS m,
               COALESCE(SUM(calls), 0),
               COALESCE(SUM(tokens_in), 0),
               COALESCE(SUM(tokens_out), 0),
               COALESCE(SUM(cost_input), 0.0),
               COALESCE(SUM(cost_cached), 0.0),
               COALESCE(SUM(cost_output), 0.0),
               COALESCE(SUM(cost_total), 0.0)
        FROM provider_cost_daily
        WHERE project_id = ?1 AND kind = 'generation'
          AND substr(day, 1, 7) >= ?2
          AND substr(day, 1, 7) <= ?3
        GROUP BY m
      )";
      std::unordered_map<std::string, json> monthRows;
      utils::SqliteStmt monthlyStmt(db);
      if (sqlite3_prepare_v2(db, selectMonthlySql, -1, &monthlyStmt.ref(), nullptr) == SQLITE_OK) {
        sqlite3_bind_text(monthlyStmt.ref(), 1, projectId.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(monthlyStmt.ref(), 2, startMonth.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(monthlyStmt.ref(), 3, monthPrefix.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(monthlyStmt.ref()) == SQLITE_ROW) {
          std::string m = columnText(monthlyStmt.ref(), 0);
          monthRows[m] = {
            { "month", m },
            { "calls", sqlite3_column_int64(monthlyStmt.ref(), 1) },
            { "tokens_in", sqlite3_column_int64(monthlyStmt.ref(), 2) },
            { "tokens_out", sqlite3_column_int64(monthlyStmt.ref(), 3) },
            { "input", sqlite3_column_double(monthlyStmt.ref(), 4) },
            { "cached_input", sqlite3_column_double(monthlyStmt.ref(), 5) },
            { "output", sqlite3_column_double(monthlyStmt.ref(), 6) },
            { "total", sqlite3_column_double(monthlyStmt.ref(), 7) }
          };
        }
      }

      for (int i = 0; i < monthCount; ++i) {
        const int idx = startMonthZero + i;
        const std::string m = formatMonth(idx / 12, (idx % 12) + 1);
        auto it = monthRows.find(m);
        if (it != monthRows.end()) {
          monthlyJson.push_back(std::move(it->second));
        } else {
          monthlyJson.push_back({
            { "month", m },
            { "calls", 0 },
            { "tokens_in", 0 },
            { "tokens_out", 0 },
            { "input", 0.0 },
            { "cached_input", 0.0 },
            { "output", 0.0 },
            { "total", 0.0 }
          });
        }
      }
    }

    costJson["summary"] = {
      { "history_since", historySince },
      { "today", todayCost },
      { "last_7d", last7d },
      { "last_30d", last30d },
      { "this_month", thisMonth },
      { "ytd", ytd },
      { "avg_daily_30d", avgDaily30d },
      { "avg_monthly", avgMonthly },
      { "months_tracked", monthsTracked },
      { "projected_month", projectedMonth },
      { "tracked_total", trackedTotal }
    };
    costJson["monthly"] = std::move(monthlyJson);

    return costJson;
  }

  json buildOfflineSnapshot(sqlite3 *db, const std::string &projectId, int64_t nowEpoch) {
    int64_t lastHeartbeat = 0;
    if (db) {
      const char *hbSql = "SELECT MAX(last_heartbeat) FROM session_heartbeats WHERE project_id = ?";
      utils::SqliteStmt hbStmt(db);
      if (sqlite3_prepare_v2(db, hbSql, -1, &hbStmt.ref(), nullptr) == SQLITE_OK) {
        sqlite3_bind_text(hbStmt.ref(), 1, projectId.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(hbStmt.ref()) == SQLITE_ROW &&
            sqlite3_column_type(hbStmt.ref(), 0) != SQLITE_NULL) {
          lastHeartbeat = sqlite3_column_int64(hbStmt.ref(), 0);
        }
      }
    }

    json metrics = readPersistedMetrics(db, projectId);
    metrics["latency_p50_ms"] = 0.0;
    metrics["latency_p95_ms"] = 0.0;
    metrics["latency_samples"] = 0;
    if (metrics.contains("by_endpoint") && metrics["by_endpoint"].is_object()) {
      for (auto &[name, ep] : metrics["by_endpoint"].items()) {
        (void)name;
        ep["latency_p50_ms"] = 0.0;
        ep["latency_p95_ms"] = 0.0;
        ep["latency_samples"] = 0;
      }
    }

    return {
      { "project_id", projectId },
      { "session_id", "offline" },
      { "seq", 0 },
      { "ts", nowEpoch },
      { "state", "idle" },
      { "last_heartbeat", lastHeartbeat },
      { "progress", {
        { "phase", "idle" },
        { "files_total", 0 },
        { "files_done", 0 },
        { "current_batch", 0 },
        { "current_batch_size", 0 },
        { "chunks_total", 0 },
        { "chunks_done", 0 },
        { "eta_seconds", -1 },
        { "started_at", 0 }
      } },
      { "metrics", std::move(metrics) },
      { "cost", readCost(db, projectId, "", nowEpoch) },
      { "offline", true }
    };
  }

} // namespace monitor_store
