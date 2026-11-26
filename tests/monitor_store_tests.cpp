#include "monitor_store.h"
#include <sqlite3.h>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

  int checks = 0;

  void check(bool value, const char *description) {
    ++checks;
    if (!value) throw std::runtime_error(description);
  }

  void checkNear(double actual, double expected, const char *description) {
    ++checks;
    if (std::abs(actual - expected) > 1e-9) {
      throw std::runtime_error(std::string(description) +
        " (actual=" + std::to_string(actual) + ", expected=" + std::to_string(expected) + ")");
    }
  }

  int64_t localNoon(int year, int month, int day) {
    std::tm tmVal{};
    tmVal.tm_year = year - 1900;
    tmVal.tm_mon = month - 1;
    tmVal.tm_mday = day;
    tmVal.tm_hour = 12;
    tmVal.tm_min = 0;
    tmVal.tm_sec = 0;
    tmVal.tm_isdst = -1;
    std::time_t res = std::mktime(&tmVal);
    check(res != static_cast<std::time_t>(-1), "mktime succeeds for localNoon");
    return static_cast<int64_t>(res);
  }

  struct TempDb {
    fs::path directory = fs::temp_directory_path() /
      ("phenixcode-monitor-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path dbPath = directory / "metrics.sqlite";
    sqlite3 *db = nullptr;

    TempDb() {
      if (!fs::create_directory(directory)) throw std::runtime_error("Unable to create temp directory");
      if (sqlite3_open(dbPath.string().c_str(), &db) != SQLITE_OK) {
        throw std::runtime_error("Unable to open temp sqlite database");
      }
    }

    ~TempDb() {
      if (db) {
        sqlite3_close(db);
        db = nullptr;
      }
      if (directory.parent_path() == fs::temp_directory_path() &&
          directory.filename().string().starts_with("phenixcode-monitor-test-")) {
        std::error_code ec;
        fs::remove_all(directory, ec);
      }
    }

    void exec(const std::string &sql) {
      char *err = nullptr;
      int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
      if (rc != SQLITE_OK) {
        std::string msg = err ? err : "SQL error";
        if (err) sqlite3_free(err);
        throw std::runtime_error(msg);
      }
    }
  };

} // namespace

int main() {
  try {
    // (a) Fresh DB: ensureSchema + repeated recordCost accumulates in both tables
    {
      TempDb fixture;
      check(monitor_store::ensureSchema(fixture.db), "ensureSchema succeeds on fresh DB");

      const int64_t ts = localNoon(2026, 3, 15);
      check(monitor_store::localDay(ts) == "2026-03-15", "localDay formats YYYY-MM-DD");

      monitor_store::CostEntry first{ "proj-a", "prov-1", "model-1", "generation", 1000, 200, 0.10, 0.20, 0.05, ts };
      monitor_store::CostEntry second{ "proj-a", "prov-1", "model-1", "generation", 500, 100, 0.05, 0.10, 0.02, ts };
      check(monitor_store::recordCost(fixture.db, first), "first recordCost succeeds");
      check(monitor_store::recordCost(fixture.db, second), "second recordCost succeeds");

      json cost = monitor_store::readCost(fixture.db, "proj-a", "v1", ts);
      check(cost["by_provider"].contains("prov-1"), "by_provider contains prov-1");
      const auto &p = cost["by_provider"]["prov-1"];
      check(p["calls"] == 2, "legacy provider_costs accumulates calls");
      check(p["tokens_in"] == 1500 && p["tokens_out"] == 300, "legacy provider_costs accumulates tokens");
      checkNear(p["input"].get<double>(), 0.15, "legacy input cost");
      checkNear(p["output"].get<double>(), 0.30, "legacy output cost");
      checkNear(p["cached_input"].get<double>(), 0.07, "legacy cached cost");
      checkNear(p["total"].get<double>(), 0.52, "legacy total cost");

      check(cost.contains("summary"), "summary present");
      checkNear(cost["summary"]["today"].get<double>(), 0.52, "daily summary accumulates same-day calls");
      checkNear(cost["summary"]["tracked_total"].get<double>(), 0.52, "tracked_total accumulates");
      check(cost["monthly"].size() == 1, "one month recorded");
      check(cost["monthly"][0]["calls"] == 2, "monthly calls accumulated");
    }

    // (b) Deterministic local timestamps across months/years + embedding exclusion
    {
      TempDb fixture;
      check(monitor_store::ensureSchema(fixture.db), "ensureSchema succeeds");

      const int64_t nowTs = localNoon(2026, 3, 15);
      // Generation rows:
      // 2025-11-10: $1.00
      // 2026-01-05: $2.00
      // 2026-03-01: $3.00
      // 2026-03-10: $4.00
      // 2026-03-15: $5.00
      check(monitor_store::recordCost(fixture.db, { "proj-b", "gen-1", "m1", "generation", 100, 10, 0.70, 0.25, 0.05, localNoon(2025, 11, 10) }), "2025-11-10 row");
      check(monitor_store::recordCost(fixture.db, { "proj-b", "gen-1", "m1", "generation", 200, 20, 1.40, 0.50, 0.10, localNoon(2026, 1, 5) }), "2026-01-05 row");
      check(monitor_store::recordCost(fixture.db, { "proj-b", "gen-1", "m1", "generation", 300, 30, 2.10, 0.75, 0.15, localNoon(2026, 3, 1) }), "2026-03-01 row");
      check(monitor_store::recordCost(fixture.db, { "proj-b", "gen-1", "m1", "generation", 400, 40, 2.80, 1.00, 0.20, localNoon(2026, 3, 10) }), "2026-03-10 row");
      check(monitor_store::recordCost(fixture.db, { "proj-b", "gen-1", "m1", "generation", 500, 50, 3.50, 1.25, 0.25, localNoon(2026, 3, 15) }), "2026-03-15 row");
      // Embedding row on 2026-03-15: $9.00 (must be excluded from summary/monthly, counted in legacy embedding/total)
      check(monitor_store::recordCost(fixture.db, { "proj-b", "embed-x", "e1", "embedding", 9000, 0, 9.00, 0.0, 0.0, localNoon(2026, 3, 15) }), "embedding row");

      json cost = monitor_store::readCost(fixture.db, "proj-b", "v2", nowTs);
      const auto &s = cost["summary"];
      check(s["history_since"] == "2025-11-10", "history_since matches earliest generation day");
      check(s["months_tracked"] == 5, "months_tracked spans 2025-11..2026-03");
      checkNear(s["today"].get<double>(), 5.0, "today excludes embedding and earlier days");
      checkNear(s["last_7d"].get<double>(), 9.0, "last_7d sums 03-10 + 03-15");
      checkNear(s["last_30d"].get<double>(), 12.0, "last_30d sums 03-01 + 03-10 + 03-15");
      checkNear(s["this_month"].get<double>(), 12.0, "this_month sums March generation rows");
      checkNear(s["ytd"].get<double>(), 14.0, "ytd sums 2026 generation rows");
      checkNear(s["tracked_total"].get<double>(), 15.0, "tracked_total sums all generation rows");
      checkNear(s["avg_monthly"].get<double>(), 15.0 / 5.0, "avg_monthly divides by elapsed calendar months");
      checkNear(s["avg_daily_30d"].get<double>(), 12.0 / 30.0, "avg_daily_30d divides by 30");
      checkNear(s["projected_month"].get<double>(), 12.0 / 15.0 * 31.0, "projected_month projects 15/31");

      checkNear(cost["embedding"].get<double>(), 9.0, "legacy embedding cost includes embed-x");
      checkNear(cost["total"].get<double>(), 24.0, "legacy total includes generation + embedding");

      const auto &monthly = cost["monthly"];
      check(monthly.size() == 5, "monthly has 5 entries");
      check(monthly[0]["month"] == "2025-11" && std::abs(monthly[0]["total"].get<double>() - 1.0) < 1e-9, "2025-11 entry");
      check(monthly[1]["month"] == "2025-12" && monthly[1]["calls"] == 0 && std::abs(monthly[1]["total"].get<double>()) < 1e-9, "2025-12 zero-filled");
      check(monthly[2]["month"] == "2026-01" && std::abs(monthly[2]["total"].get<double>() - 2.0) < 1e-9, "2026-01 entry");
      check(monthly[3]["month"] == "2026-02" && monthly[3]["calls"] == 0 && std::abs(monthly[3]["total"].get<double>()) < 1e-9, "2026-02 zero-filled");
      check(monthly[4]["month"] == "2026-03" && monthly[4]["calls"] == 3 && std::abs(monthly[4]["total"].get<double>() - 12.0) < 1e-9, "2026-03 entry");
    }

    // (c) Legacy DB without provider_cost_daily, then migrated via ensureSchema
    {
      TempDb fixture;
      fixture.exec(R"(
        CREATE TABLE provider_costs (
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
        INSERT INTO provider_costs VALUES ('proj-legacy', 'gen-legacy', 'm-old', 3, 1000, 200, 0.3, 0.2, 0.0, 0.5, 1700000000);
      )");

      const int64_t nowTs = localNoon(2026, 3, 15);
      json legacyCost = monitor_store::readCost(fixture.db, "proj-legacy", "v-old", nowTs);
      check(legacyCost["by_provider"].contains("gen-legacy"), "legacy readCost returns by_provider");
      checkNear(legacyCost["total"].get<double>(), 0.5, "legacy readCost returns total");
      check(!legacyCost.contains("summary"), "legacy readCost omits summary when daily table missing");
      check(!legacyCost.contains("monthly"), "legacy readCost omits monthly when daily table missing");

      check(monitor_store::ensureSchema(fixture.db), "ensureSchema migrates legacy DB");
      json migratedCost = monitor_store::readCost(fixture.db, "proj-legacy", "v-old", nowTs);
      check(migratedCost.contains("summary"), "migrated DB includes summary");
      checkNear(migratedCost["summary"]["tracked_total"].get<double>(), 0.0, "migrated DB has tracked_total 0");
      check(migratedCost["summary"]["history_since"] == "", "migrated DB has empty history_since");
      check(migratedCost["monthly"].is_array() && migratedCost["monthly"].empty(), "migrated DB has empty monthly array");
    }

    // (d) buildOfflineSnapshot shape
    {
      TempDb fixture;
      check(monitor_store::ensureSchema(fixture.db), "ensureSchema succeeds");
      const int64_t nowTs = localNoon(2026, 3, 15);
      fixture.exec("INSERT INTO endpoint_metrics VALUES ('proj-off', 'chat', 4, 1, 100, 50, 1700000000);");
      fixture.exec("INSERT INTO session_heartbeats VALUES ('sess-1', 'proj-off', 'ready', 'idle', 1700000123);");
      check(monitor_store::recordCost(fixture.db, { "proj-off", "gen-1", "m1", "generation", 100, 50, 0.1, 0.2, 0.0, nowTs }), "recordCost for offline test");

      json snap = monitor_store::buildOfflineSnapshot(fixture.db, "proj-off", nowTs);
      check(snap["offline"] == true, "offline snapshot has offline:true");
      check(snap["session_id"] == "offline", "offline snapshot has session_id offline");
      check(snap["last_heartbeat"] == 1700000123, "offline snapshot reads last_heartbeat");
      check(snap["progress"]["phase"] == "idle" && snap["progress"]["eta_seconds"] == -1, "offline snapshot has full progress object");
      checkNear(snap["metrics"]["latency_p50_ms"].get<double>(), 0.0, "offline metrics latency_p50_ms is 0");
      checkNear(snap["metrics"]["latency_p95_ms"].get<double>(), 0.0, "offline metrics latency_p95_ms is 0");
      check(snap["metrics"]["latency_samples"] == 0, "offline metrics latency_samples is 0");
      checkNear(snap["metrics"]["by_endpoint"]["chat"]["latency_p50_ms"].get<double>(), 0.0, "endpoint latency_p50_ms is 0");
      checkNear(snap["metrics"]["by_endpoint"]["chat"]["latency_p95_ms"].get<double>(), 0.0, "endpoint latency_p95_ms is 0");
      check(snap["metrics"]["by_endpoint"]["chat"]["latency_samples"] == 0, "endpoint latency_samples is 0");
      checkNear(snap["cost"]["summary"]["today"].get<double>(), 0.3, "offline snapshot includes cost summary");
    }

    std::cout << "monitor_store_tests passed (" << checks << " checks)\n";
    return 0;
  } catch (const std::exception &ex) {
    std::cerr << "monitor_store_tests FAILED: " << ex.what() << "\n";
    return 1;
  }
}
