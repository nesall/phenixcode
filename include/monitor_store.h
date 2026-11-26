#pragma once

#include <cstdint>
#include <string>
#include "json_shim.h"

struct sqlite3;

namespace monitor_store {

  struct CostEntry {
    std::string projectId;
    std::string providerId;
    std::string model;
    std::string kind; // "generation" | "embedding"
    int64_t tokensIn = 0;
    int64_t tokensOut = 0;
    double costInput = 0.0;
    double costOutput = 0.0;
    double costCached = 0.0;
    int64_t ts = 0; // epoch seconds
  };

  std::string defaultMetricsDbPath();
  bool ensureSchema(sqlite3 *db);
  std::string localDay(int64_t epochSeconds);
  bool recordCost(sqlite3 *db, const CostEntry &e); // caller holds its own mutex
  nlohmann::json readPersistedMetrics(sqlite3 *db, const std::string &projectId);
  nlohmann::json readCost(sqlite3 *db, const std::string &projectId, const std::string &pricingVersion, int64_t nowEpoch);
  nlohmann::json buildOfflineSnapshot(sqlite3 *db, const std::string &projectId, int64_t nowEpoch);

} // namespace monitor_store
