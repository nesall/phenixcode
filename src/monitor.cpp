#include "monitor.h"
#include "settings.h"
#include "auth.h"
#include <algorithm>
#include <condition_variable>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
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

  struct EndpointTally {
    size_t requests = 0;
    size_t errors = 0;
    size_t tokensIn = 0;
    size_t tokensOut = 0;
    LatencyReservoir reservoir;
  };

  struct ProviderCostTally {
    double input = 0.0;
    double output = 0.0;
    double cachedInput = 0.0;
    double total = 0.0;
    size_t tokensIn = 0;
    size_t tokensOut = 0;
    std::string model;
  };

} // anonymous namespace

struct MonitorTracker::Impl {
  std::string projectId_;
  std::string sessionId_;
  fs::path monitorFilePath_;
  std::string pricingVersion_;

  mutable std::mutex mtx_;
  mutable std::atomic<uint64_t> seq_{ 0 };

  MonitorState state_ = MonitorState::Idle;
  int64_t lastHeartbeat_ = 0;

  // Progress
  MonitorPhase phase_ = MonitorPhase::Idle;
  size_t filesTotal_ = 0;
  size_t filesDone_ = 0;
  size_t currentBatch_ = 0;
  size_t currentBatchSize_ = 0;
  size_t chunksTotal_ = 0;
  size_t chunksDone_ = 0;
  int64_t startedAt_ = 0;

  // Metrics
  size_t requests_ = 0;
  size_t errors_ = 0;
  size_t tokensIn_ = 0;
  size_t tokensOut_ = 0;
  LatencyReservoir reservoir_;
  std::unordered_map<std::string, EndpointTally> byEndpoint_;

  // Cost
  double genInputCost_ = 0.0;
  double genOutputCost_ = 0.0;
  double genCachedInputCost_ = 0.0;
  double embeddingCost_ = 0.0;
  std::unordered_map<std::string, ProviderCostTally> byProvider_;

  // Timer thread
  std::thread timerThread_;
  std::condition_variable cv_;
  std::mutex cvMtx_;
  bool timerRunning_ = false;

  explicit Impl(const Settings &settings) {
    projectId_ = settings.getProjectId();
    sessionId_ = defaultSessionId();
    lastHeartbeat_ = nowEpochSeconds();

    fs::path cfgPath = settings.configPath().empty()
      ? fs::current_path() / "settings.json"
      : fs::absolute(settings.configPath());
    monitorFilePath_ = cfgPath.parent_path() / "monitor.json";

    std::string prvDump = settings.providersJson().dump();
    pricingVersion_ = AdminAuth::fnv1a64(prvDump);
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

MonitorTracker::MonitorTracker(const Settings &settings)
  : imp(std::make_unique<Impl>(settings))
{}

MonitorTracker::~MonitorTracker() {
  stopPersistTimer();
}

void MonitorTracker::setSessionId(const std::string &sessionId)
{
  std::lock_guard<std::mutex> lock(imp->mtx_);
  if (!sessionId.empty()) {
    imp->sessionId_ = sessionId;
  }
}

void MonitorTracker::setState(MonitorState state)
{
  std::lock_guard<std::mutex> lock(imp->mtx_);
  imp->state_ = state;
  imp->lastHeartbeat_ = nowEpochSeconds();
  imp->seq_++;
}

void MonitorTracker::touchHeartbeat() {
  std::lock_guard<std::mutex> lock(imp->mtx_);
  imp->lastHeartbeat_ = nowEpochSeconds();
}

void MonitorTracker::beginPhase(MonitorPhase phase, size_t filesTotal, size_t chunksTotal)
{
  std::lock_guard<std::mutex> lock(imp->mtx_);
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
  std::lock_guard<std::mutex> lock(imp->mtx_);
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
  std::lock_guard<std::mutex> lock(imp->mtx_);
  imp->chunksTotal_ += delta;
  imp->lastHeartbeat_ = nowEpochSeconds();
  imp->seq_++;
}

void MonitorTracker::addChunksDone(size_t delta)
{
  std::lock_guard<std::mutex> lock(imp->mtx_);
  imp->chunksDone_ += delta;
  imp->lastHeartbeat_ = nowEpochSeconds();
  imp->seq_++;
}

void MonitorTracker::finishIndexing(bool ok)
{
  std::lock_guard<std::mutex> lock(imp->mtx_);
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
  std::lock_guard<std::mutex> lock(imp->mtx_);
  imp->requests_++;
  if (isError) imp->errors_++;
  imp->tokensIn_ += tokensIn;
  imp->tokensOut_ += tokensOut;
  imp->reservoir_.add(latencyMs);

  if (!endpoint.empty()) {
    auto &ep = imp->byEndpoint_[endpoint];
    ep.requests++;
    if (isError) ep.errors++;
    ep.tokensIn += tokensIn;
    ep.tokensOut += tokensOut;
    ep.reservoir.add(latencyMs);
  }
  imp->lastHeartbeat_ = nowEpochSeconds();
  imp->seq_++;
}

void MonitorTracker::recordGenerationCost(
  const std::string &providerId,
  size_t tokensIn,
  size_t tokensOut,
  const ProvidersSettings &providers,
  double cacheHitRatio)
{
  std::lock_guard<std::mutex> lock(imp->mtx_);
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

  imp->genInputCost_ += inCost;
  imp->genCachedInputCost_ += cachedCost;
  imp->genOutputCost_ += outCost;

  auto &p = imp->byProvider_[providerId];
  p.model = modelName;
  p.tokensIn += tokensIn;
  p.tokensOut += tokensOut;
  p.input += inCost;
  p.cachedInput += cachedCost;
  p.output += outCost;
  p.total = p.input + p.cachedInput + p.output;
  imp->seq_++;
}

void MonitorTracker::recordEmbeddingCost(
  const std::string &providerId,
  size_t tokensIn,
  const ProvidersSettings &providers)
{
  std::lock_guard<std::mutex> lock(imp->mtx_);
  const ApiConfig *cfg = providers.findEmbedding(providerId);
  if (cfg && cfg->pricing.input > 0.0f) {
    double cost = (tokensIn / 1'000'000.0) * cfg->pricing.input;
    imp->embeddingCost_ += cost;
  }
  imp->seq_++;
}

json MonitorTracker::toJson() const
{
  std::lock_guard<std::mutex> lock(imp->mtx_);
  int64_t now = nowEpochSeconds();
  uint64_t seq = ++imp->seq_;

  auto [p50, p95] = imp->reservoir_.percentiles();

  json byEndpointJson = json::object();
  for (const auto &[name, ep] : imp->byEndpoint_) {
    auto [epP50, epP95] = ep.reservoir.percentiles();
    byEndpointJson[name] = {
      {"requests", ep.requests},
      {"errors", ep.errors},
      {"tokens_in", ep.tokensIn},
      {"tokens_out", ep.tokensOut},
      {"latency_p50_ms", epP50},
      {"latency_p95_ms", epP95},
      {"latency_samples", ep.reservoir.samples.size()}
    };
  }

  json byProviderJson = json::object();
  for (const auto &[id, pv] : imp->byProvider_) {
    byProviderJson[id] = {
      {"input", pv.input},
      {"output", pv.output},
      {"cached_input", pv.cachedInput},
      {"total", pv.total},
      {"tokens_in", pv.tokensIn},
      {"tokens_out", pv.tokensOut},
      {"model", pv.model}
    };
  }

  double totalCost = imp->genInputCost_ + imp->genOutputCost_ + imp->genCachedInputCost_ + imp->embeddingCost_;

  json costJson = {
    {"generation_input", imp->genInputCost_},
    {"generation_output", imp->genOutputCost_},
    {"generation_cached_input", imp->genCachedInputCost_},
    {"embedding", imp->embeddingCost_},
    {"total", totalCost},
    {"pricing_version", imp->pricingVersion_},
    {"by_provider", byProviderJson}
  };
  if (imp->embeddingCost_ == 0.0) {
    costJson["note"] = "embedding cost not included";
  }

  return {
    {"project_id", imp->projectId_},
    {"session_id", imp->sessionId_},
    {"seq", seq},
    {"ts", now},
    {"state", stateToString(imp->state_)},
    {"last_heartbeat", imp->lastHeartbeat_},
    {"progress", {
      {"phase", phaseToString(imp->phase_)},
      {"files_total", imp->filesTotal_},
      {"files_done", imp->filesDone_},
      {"current_batch", imp->currentBatch_},
      {"current_batch_size", imp->currentBatchSize_},
      {"chunks_total", imp->chunksTotal_},
      {"chunks_done", imp->chunksDone_},
      {"eta_seconds", imp->computeEtaSecondsLocked()},
      {"started_at", imp->startedAt_}
    }},
    {"metrics", {
      {"requests", imp->requests_},
      {"errors", imp->errors_},
      {"tokens_in", imp->tokensIn_},
      {"tokens_out", imp->tokensOut_},
      {"latency_p50_ms", p50},
      {"latency_p95_ms", p95},
      {"latency_samples", imp->reservoir_.samples.size()},
      {"by_endpoint", byEndpointJson}
    }},
    {"cost", costJson}
  };
}

void MonitorTracker::persist() const
{
  try {
    json snap = toJson();
    fs::path tmpPath = imp->monitorFilePath_;
    tmpPath += ".tmp";

    {
      std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
      if (!out.is_open()) return;
      out << snap.dump(2);
      out.flush();
    }
    std::error_code ec;
    fs::rename(tmpPath, imp->monitorFilePath_, ec);
    if (ec) {
      fs::copy_file(tmpPath, imp->monitorFilePath_, fs::copy_options::overwrite_existing, ec);
      fs::remove(tmpPath, ec);
    }
  } catch (const std::exception &e) {
    LOG_MSG << "[Monitor] Failed to persist monitor.json:" << e.what();
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

json MonitorTracker::tailLogFile(LogFileType type, int64_t offset, size_t maxBytes, const Settings &settings)
{
  //std::string target = requestedPath.empty() ? settings.loggingLoggingFile() : requestedPath;

  // Restrict to configured log files or files inside the project config directory
  fs::path cfgDir = settings.configPath().empty()
    ? fs::current_path()
    : fs::absolute(settings.configPath()).parent_path();

  const fs::path logFile = fs::absolute(settings.loggingLoggingFile()).lexically_normal();
  const fs::path diagFile = fs::absolute(settings.loggingDiagnosticsFile()).lexically_normal();
  const fs::path reqFile{ type == LogFileType::Logs ? logFile : diagFile };

  std::error_code ec;
  if (!fs::exists(reqFile, ec)) {
    return {
      {"status", "success"},
      {"path", reqFile.string()},
      {"lines", json::array()},
      {"next_offset", 0},
      {"size", 0}
    };
  }

  int64_t fileSize = static_cast<int64_t>(fs::file_size(reqFile, ec));
  if (ec) {
    return {
      {"status", "error"},
      {"message", ec.message()},
      {"path", reqFile.string()},
      {"lines", json::array()},
      {"next_offset", 0},
      {"size", 0}
    };
  }

  // If file was truncated or negative offset passed, reset to 0 (or tail from end if offset < 0)
  if (offset < 0 || offset > fileSize) {
    offset = (std::max)(int64_t(0), fileSize - static_cast<int64_t>(maxBytes));
  }

  std::ifstream in(reqFile, std::ios::binary);
  if (!in.is_open()) {
    return {
      {"status", "error"},
      {"message", "Unable to open log file"},
      {"path", reqFile.string()},
      {"lines", json::array()},
      {"next_offset", offset},
      {"size", fileSize}
    };
  }

  in.seekg(offset, std::ios::beg);
  size_t toRead = static_cast<size_t>((std::min)(int64_t(maxBytes), fileSize - offset));
  std::string buffer(toRead, '\0');
  in.read(buffer.data(), static_cast<std::streamsize>(toRead));
  size_t bytesRead = static_cast<size_t>(in.gcount());
  buffer.resize(bytesRead);

  int64_t nextOffset = offset + static_cast<int64_t>(bytesRead);

  // Avoid splitting an incomplete trailing line if we hit maxBytes before EOF
  if (nextOffset < fileSize) {
    size_t lastNl = buffer.rfind('\n');
    if (lastNl != std::string::npos) {
      nextOffset = offset + static_cast<int64_t>(lastNl + 1);
      buffer.resize(lastNl + 1);
    }
  }

  json lines = json::array();
  std::istringstream iss(buffer);
  std::string line;
  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(std::move(line));
  }

  return {
    {"status", "success"},
    {"path", reqFile.string()},
    {"lines", std::move(lines)},
    {"next_offset", nextOffset},
    {"size", fileSize}
  };
}
