#ifndef _MONITOR_H_
#define _MONITOR_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "json_shim.h"

class Settings;
class ProvidersSettings;

enum class MonitorState { Idle, Scanning, Indexing, Ready, Error };
enum class MonitorPhase { Idle, Walk, Chunk, Embed, Persist };

class MonitorTracker {
public:
  explicit MonitorTracker(const Settings &settings);
  ~MonitorTracker();

  MonitorTracker(const MonitorTracker &) = delete;
  MonitorTracker &operator=(const MonitorTracker &) = delete;

  void setSessionId(const std::string &sessionId);
  void setState(MonitorState state);
  void touchHeartbeat();

  // Progress tracking
  void beginPhase(MonitorPhase phase, size_t filesTotal = 0, size_t chunksTotal = 0);
  void updateProgress(
    MonitorPhase phase,
    size_t filesDone,
    size_t filesTotal,
    size_t chunksDone,
    size_t chunksTotal,
    size_t currentBatch = 0,
    size_t currentBatchSize = 0);
  void addChunksTotal(size_t delta);
  void addChunksDone(size_t delta);
  void finishIndexing(bool ok = true);

  // Request & cost metrics
  void recordRequest(
    const std::string &endpoint,
    double latencyMs,
    bool isError,
    size_t tokensIn = 0,
    size_t tokensOut = 0);

  void recordGenerationCost(
    const std::string &providerId,
    size_t tokensIn,
    size_t tokensOut,
    const ProvidersSettings &providers,
    double cacheHitRatio = 0.05);

  void recordEmbeddingCost(
    const std::string &providerId,
    size_t tokensIn,
    const ProvidersSettings &providers);

  // Serialization & persistence
  nlohmann::json toJson() const;
  void persist() const;

  void startPersistTimer(int intervalSeconds = 5);
  void stopPersistTimer();

  enum class LogFileType { Logs, Diagnostics };
  static nlohmann::json tailLogFile(LogFileType type, int64_t offset, int64_t count, const Settings &settings);

private:
  struct Impl;
  std::unique_ptr<Impl> imp;
};

#endif // _MONITOR_H_