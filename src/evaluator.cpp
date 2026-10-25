#include "evaluator.h"
#include "app.h"
#include "cutils.h"
#include "settings.h"
#include "database.h"
#include "inference.h"
#include "sourceproc.h"

#include <json_shim.h>
#include <utils_log/logger.hpp>
#include "3rdparty/fmt/format.h"
#include <cmath>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

using json = nlohmann::json;
namespace fs = std::filesystem;


namespace {

  struct EvalMetrics {
    double recallAtK = 0;
    double mrr = 0;
    double ndcg = 0;
  };

  EvalMetrics computeMetrics(const std::vector<SearchResult> &results, const std::set<std::string> &expectedIds) {
    EvalMetrics m;
    if (expectedIds.empty()) return m;

    // Deduplicate by sourceId — keep first (best-ranked) occurrence
    std::vector<std::string> uniqueSourceIds;
    std::unordered_set<std::string> seen;
    for (const auto &r : results) {
      if (seen.insert(r.sourceId).second) {
        uniqueSourceIds.push_back(r.sourceId);
      }
    }

    // Recall@k
    size_t found = 0;
    for (const auto &id : uniqueSourceIds) {
      if (expectedIds.count(id)) found++;
    }
    m.recallAtK = static_cast<double>(found) / expectedIds.size();

    // MRR — rank of first relevant result
    for (size_t j = 0; j < uniqueSourceIds.size(); ++j) {
      if (expectedIds.count(uniqueSourceIds[j])) {
        m.mrr = 1.0 / (j + 1);
        break;
      }
    }

    // NDCG — binary relevance on deduplicated list
    double dcg = 0;
    for (size_t j = 0; j < uniqueSourceIds.size(); ++j) {
      if (expectedIds.count(uniqueSourceIds[j])) {
        dcg += 1.0 / std::log2(static_cast<double>(j) + 2.0);
      }
    }
    double idealDcg = 0;
    size_t idealCount = (std::min)(expectedIds.size(), uniqueSourceIds.size());
    for (size_t j = 0; j < idealCount; ++j) {
      idealDcg += 1.0 / std::log2(static_cast<double>(j) + 2.0);
    }
    m.ndcg = idealDcg > 0 ? dcg / idealDcg : 0;

    return m;
  }

  std::string generateQueryFromContent(const std::string &content, const std::string &filename) {
    // Look for common patterns: function signatures, comments, class names
    std::istringstream stream(content);
    std::string line;
    std::vector<std::string> candidates;

    while (std::getline(stream, line)) {
      // Match patterns like: "void App::embed(", "class Chunker", "// Search for..."
      std::string trimmed = line;
      // trim leading whitespace
      trimmed.erase(0, trimmed.find_first_not_of(" \t"));

      if (trimmed.rfind("//", 0) == 0 && trimmed.length() > 10) {
        // Comment — strip "// " and use as query
        std::string comment = trimmed.substr(2);
        // trim
        comment.erase(0, comment.find_first_not_of(" "));
        if (comment.length() > 10)
          candidates.push_back(comment);
      }
      // Look for function-like declarations
      if (trimmed.find("::") != std::string::npos &&
        trimmed.find("(") != std::string::npos &&
        trimmed.find(";") == std::string::npos) {
        // e.g. "void App::embed(bool noPrompt)" → "embed"
        auto colons = trimmed.find("::");
        auto paren = trimmed.find("(");
        if (colons != std::string::npos && paren != std::string::npos) {
          auto nameStart = trimmed.rfind(' ', colons);
          std::string funcName = trimmed.substr(
            colons + 2, paren - colons - 2);
          if (funcName.length() > 3)
            candidates.push_back(
              "What does the " + funcName + " function do?");
        }
      }
    }

    if (!candidates.empty()) {
      // Pick the first meaningful candidate
      return candidates[0];
    }
    // Fallback
    return "What is the purpose of " + filename + "?";
  }


  nlohmann::json runRobust(const App &app, const std::string &datasetPath, size_t topK, SearchMode mode, float bm25Weight, float grepWeight)
  {
    std::ifstream f(datasetPath);
    if (!f.is_open()) {
      LOG_MSG << "Cannot open dataset file:" << datasetPath;
      return {};
    }

    json dataset;
    try {
      f >> dataset;
    } catch (const std::exception &e) {
      LOG_MSG << "Invalid JSON in dataset:" << e.what();
      return {};
    }

    if (!dataset.is_array() || dataset.empty()) {
      LOG_MSG << "Dataset must be a non-empty JSON array.";
      return {};
    }

    const auto mode2str = [](SearchMode m) {
      switch (m) {
      case SearchMode::Grep:       return "grep";
      case SearchMode::Base:       return "base";
      case SearchMode::Hybrid:     return "hybrid";
      case SearchMode::HybridGrep: return "hybridGrep";
      default:                     return "auto";
      }
      };

    LOG_MSG << "\nRunning evaluation on" << dataset.size()
      << "queries (top_k =" << topK
      << ", mode =" << mode2str(mode) << ")";

    EmbeddingClient ec{
      app.settings().embeddingCurrentApi(),
      app.settings().embeddingTimeoutMs()
    };

    double totalRecall = 0.0;
    double totalMRR = 0.0;
    double totalNDCG = 0.0;
    double totalLatency = 0.0;
    size_t validEntries = 0;

    json perQuery = json::array();
    std::set<std::string> universe;

    // Serialize unique retrieved documents in rank order.
    const auto serializeResults = [](const auto &results) {
      json retrieved = json::array();
      std::unordered_set<std::string> seen;

      for (const auto &r : results) {
        if (!seen.insert(r.sourceId).second)
          continue;

        retrieved.push_back({
          { "doc_id", r.sourceId },
          { "similarityScore", r.similarityScore },
          { "fusedScore", r.fusedScore }
          });
      }

      return retrieved;
      };

    for (size_t i = 0; i < dataset.size(); ++i) {
      const auto &entry = dataset[i];

      if (!entry.contains("query") ||
        !entry.contains("expected_doc_ids")) {
        LOG_MSG << "Skipping entry" << i
          << ": missing query or expected_doc_ids";
        continue;
      }

      const std::string query = entry["query"].get<std::string>();

      std::set<std::string> expectedIds;
      for (const auto &id : entry["expected_doc_ids"])
        expectedIds.insert(id.get<std::string>());

      if (expectedIds.empty()) {
        LOG_MSG << "Skipping entry" << i
          << ": empty expected_doc_ids";
        continue;
      }

      // Embed + search.
      const auto start = std::chrono::steady_clock::now();

      std::vector<float> queryEmbedding;
      if (entry.contains("embedding")) {
        queryEmbedding = entry.value("embedding", std::vector<float>{});
        if (queryEmbedding.empty())
          LOG_MSG_WARN << "Entry" << i
          << "has empty embedding. Will re-generate.";
      }

      if (queryEmbedding.empty()) {
        ec.generateEmbeddings(
          query, queryEmbedding, EmbeddingClient::EncodeType::Query);
      }

      eval::RetrieveDiagnostics diag;
      auto results = app.retrieve(query, queryEmbedding, topK, mode, bm25Weight, grepWeight, &diag);
      assert(results.size() <= topK);

#ifdef _DEBUG
      LOG_MSG_DEBUG << "evalRobust before results explicit sort";
      for (size_t j = 0; j < results.size(); j ++) {
        LOG_MSG_DEBUG << "evalRobust [" << j << "] bm25Score" << results[j].bm25Score << ", fusedScore" << results[j].fusedScore << ", similarityScore" << results[j].similarityScore;
      }
#endif

      json ranking = json::array();
      for (const auto &g : diag.grepRanking) {
        ranking.push_back({ { "doc_id", g.docId },{ "grep_rank", g.grepRank },{ "boost", g.boost } });
      }

      const double latencyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

      const auto m = computeMetrics(results, expectedIds);

      json retrieved = serializeResults(results);
      for (const auto &r : retrieved)
        universe.insert(r["doc_id"].get<std::string>());

      universe.insert(expectedIds.begin(), expectedIds.end());

      totalRecall += m.recallAtK;
      totalMRR += m.mrr;
      totalNDCG += m.ndcg;
      totalLatency += latencyMs;
      ++validEntries;

      const bool idealHit =
        m.recallAtK == 1.0 && m.mrr == 1.0 && m.ndcg == 1.0;

      json outEntry = {
        { "index", entry.value("index", -1) },
        { "query", query },
        { "recall_at_k", m.recallAtK },
        { "mrr", m.mrr },
        { "ndcg", m.ndcg },
        { "latency_ms", latencyMs },
        { "results_count", results.size() },
        { "retrieved", std::move(retrieved) },
        { "hit", idealHit },
        { "grepTerms", diag.grepTerms },
        { "grep_ran", diag.grepRan },
        { "grep_ranking", std::move(ranking) },
        { "grep_weight_used", grepWeight }   // makes reports self-describing
      };

      if (!idealHit) {
        json missInfo;
        missInfo["expected_doc_ids"] = json::array();

        for (const auto &id : expectedIds)
          missInfo["expected_doc_ids"].push_back(id);

        if (m.recallAtK < 1.0) {
          // Probe deeper to find where expected docs actually rank.
          const size_t probeK = topK * 10;
          const auto wideResults = app.retrieve(query, queryEmbedding, probeK, mode, bm25Weight, grepWeight);

          const json probeResults = serializeResults(wideResults);
          missInfo["probe_results"] = probeResults;

          json actualRanks = json::object();

          for (const auto &id : expectedIds) {
            for (size_t j = 0; j < probeResults.size(); ++j) {
              if (probeResults[j]["doc_id"] != id)
                continue;

              actualRanks[id] = {
                { "rank", j + 1 },
                { "similarityScore", probeResults[j]["similarityScore"] },
                { "fusedScore", probeResults[j]["fusedScore"] }
              };
              break;
            }

            if (!actualRanks.contains(id))
              actualRanks[id] = {
                { "rank", -1 },
                { "score", 0.0 }
            };
          }

          missInfo["actual_ranks"] = std::move(actualRanks);
        }

        outEntry["miss"] = std::move(missInfo);
      }

      perQuery.push_back(std::move(outEntry));

      std::cout << "[" << i + 1 << "/" << dataset.size() << "]"
        << " recall=" << m.recallAtK
        << " mrr=" << m.mrr
        << " ndcg=" << m.ndcg
        << " latency=" << latencyMs << "ms\n";
    }

    const std::string prefix = utils::commonDirPrefix(universe);
    if (!prefix.empty()) {
      for (auto &q : perQuery) {
        for (const char *key : { "retrieved_doc_ids", "expected_doc_ids" }) {
          if (!q.contains(key)) continue;

          for (auto &id : q[key])
            utils::stripPrefix(id.get_ref<std::string &>(), prefix);
        }
      }
    }

    if (validEntries == 0) {
      LOG_MSG << "No valid entries to evaluate.";
      return {};
    }

    // Summary report.
    json report = {
      { "summary",{
        { "total_queries", validEntries },
        { "top_k", topK },
        { "recall_at_k", totalRecall / validEntries },
        { "mrr", totalMRR / validEntries },
        { "ndcg", totalNDCG / validEntries },
        { "mean_latency_ms", totalLatency / validEntries },
        { "bm25Weight", bm25Weight },
        { "grepWeight", grepWeight }
      } },
      { "per_query", std::move(perQuery) }
    };

    std::cout << "\n=== Evaluation Report Summary ===\n"
      << std::setw(2) << report["summary"] << "\n";

    // Include weights in the report filename.
    const auto formatFloat = [](float value) {
      char buffer[64];
      const auto result = std::to_chars(
        buffer, buffer + sizeof(buffer), value);

      return result.ec == std::errc{}
        ? std::string(buffer, result.ptr)
        : std::to_string(value);
      };

    const std::filesystem::path datasetFile(datasetPath);
    const std::string reportName = fmt::format(
      "{}_report_{}_topk_{}_bm25_{}_grep_{}.json",
      datasetFile.stem().string(),
      mode2str(mode),
      topK,
      formatFloat(bm25Weight),
      formatFloat(grepWeight));

    const auto reportPath = std::filesystem::current_path() / reportName;

    std::ofstream out(reportPath);
    out << std::setw(2) << report;
    out.close();

    LOG_MSG << "Report saved to" << reportPath;
    return report;
  }

} // namespace


eval::Evaluator::Evaluator(const App &app) : app_(app)
{

}

void eval::Evaluator::init(const std::string &outputPath, size_t numSamples)
{
  auto trackedFiles = app_.db().getTrackedFiles();
  if (trackedFiles.empty()) {
    LOG_MSG << "No indexed files found. Run 'embed' first.";
    return;
  }

  size_t count = (std::min)(numSamples, trackedFiles.size());
  json evalArray = json::array();

  for (size_t i = 0; i < count; ++i) {
    size_t idx = i * trackedFiles.size() / count;
    const auto &file = trackedFiles[idx];
    std::string filename = fs::path(file.path).filename().string();

    // Extract meaningful tokens from the file content
    // to generate a more realistic query
    std::string content;
    SourceProcessor::readFile(file.path, content);

    // Pull function/class names or comments as query seeds
    std::string query = generateQueryFromContent(content, filename);

    json expectedIds = json::array();
    expectedIds.push_back(file.path);

    evalArray.push_back({
      { "query", query },
      { "expected_doc_ids", expectedIds }
      });
  }

  std::ofstream out(outputPath);
  out << std::setw(2) << evalArray;
  out.close();

  LOG_MSG << "Generated template:" << outputPath;
  LOG_MSG << "Created" << evalArray.size() << "sample queries.";
  LOG_MSG << "Please review and correct expected_doc_ids before running eval.";
  LOG_MSG << "Use 'eval --list' to see available document IDs.";
}

void eval::Evaluator::list()
{
  auto trackedFiles = app_.db().getTrackedFiles();
  auto chunkCounts = app_.db().getChunkCountsBySources();

  std::cout << "\nAvailable document IDs (" << trackedFiles.size() << " files):\n";
  std::cout << std::string(80, '-') << "\n";
  for (const auto &file : trackedFiles) {
    size_t chunks = chunkCounts.count(file.path) ? chunkCounts.at(file.path) : 0;
    std::cout << "  " << file.path << "  (" << chunks << " chunks)\n";
  }
  std::cout << std::endl;
}

void eval::Evaluator::detail(const std::string &docId)
{
  auto trackedFiles = app_.db().getTrackedFiles();
  auto chunkCounts = app_.db().getChunkCountsBySources();

  for (const auto &file : trackedFiles) {
    if (file.path == docId) {
      size_t chunks = chunkCounts.count(file.path) ? chunkCounts.at(file.path) : 0;
      std::cout << "\nDocument ID:     " << file.path << "\n";
      std::cout << "Chunks:          " << chunks << "\n";
      std::cout << "Size:            " << file.fileSize << " bytes\n";
      std::cout << "Lines:           " << file.nofLines << "\n";
      std::cout << "Last modified:   " << file.lastModified << "\n";
      return;
    }
  }
  LOG_MSG << "Document not found:" << docId;
  LOG_MSG << "Use 'eval --list' to see available IDs.";
}

void eval::Evaluator::sweep(const std::string &ds,
  const std::vector<size_t> &topKs,
  const std::vector<float> &bm25Weights,
  const std::vector<float> &grepWeights) const
{
  char c = 'y';
  std::cout << "Proceed? [y/N]: ";
  std::cin >> c;
  if (c != 'y' && c != 'Y') {
    LOG_MSG << "Evaluation cancelled.";
    return;
  }

  for (size_t topK : topKs)
    for (float b : bm25Weights)
      for (float g : grepWeights) {
        LOG_MSG << "grid top_k=" << topK << " bm25=" << b << " grep=" << g;
        run(ds, topK, b, g);
      }
}

void eval::Evaluator::run(const std::string &datasetPath, size_t topK, float bm25Weight, float grepWeight) const
{
  LOG_MSG << "Starting evaluation, dataset:" << datasetPath;

  const std::map<std::string, json> reports = {
    { "hyprid",     runRobust(app_, datasetPath, topK, SearchMode::Hybrid,     bm25Weight, grepWeight) },
    { "hybridGrep", runRobust(app_, datasetPath, topK, SearchMode::HybridGrep, bm25Weight, grepWeight) },
    { "auto",       runRobust(app_, datasetPath, topK, SearchMode::Auto,       bm25Weight, grepWeight) },
    { "grep",       runRobust(app_, datasetPath, topK, SearchMode::Grep,       bm25Weight, grepWeight) }
  };

  const auto now = std::chrono::system_clock::now();
  const auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
    now.time_since_epoch()).count();

  json masterReport = {
    { "top_k", topK },
    { "generated_at", static_cast<unsigned long long>(timestamp) },
    { "by_mode", json::object() },
    { "per_query", json::object() }
  };

  // Aggregate mode summaries and per-query metrics.
  for (const auto &[modeName, report] : reports) {
    if (!report.is_object()) continue;

    masterReport["by_mode"][modeName] = report.value("summary", json::object());

    for (const auto &entry : report.value("per_query", json::array())) {
      if (!entry.is_object()) continue;

      const std::string query = entry.value("query", "");
      if (query.empty()) continue;

      auto &queryEntry = masterReport["per_query"][query];

      if (!queryEntry.is_object()) {
        queryEntry = {
            { "query", query },
            { "index", entry.value("index", -1) },
            { "grepTerms", json::object() },
            { "modes", json::object() }
        };
      }

      queryEntry["grepTerms"][modeName] = entry.value("grepTerms", json::array());

      auto &modes = queryEntry["modes"];
      if (!modes.is_object())
        modes = json::object();

      modes[modeName] = {
        { "recall_at_k", entry.value("recall_at_k", 0.0) },
        { "mrr", entry.value("mrr", 0.0) },
        { "ndcg", entry.value("ndcg", 0.0) },
        { "latency_ms", entry.value("latency_ms", 0.0) },
        { "hit", entry.value("hit", false) },
        { "grepTerms", entry.value("grepTerms", json::array()) },
        { "retrieved_top_k", entry.value("retrieved", json::array()) },
        { "grep_ran", entry.value("grep_ran", false) },
        { "grep_ranking", entry.value("grep_ranking", json::object()) },
        { "grep_weight_used", entry.value("grep_weight_used", 0.0) }
      };
    }
  }

  // Determine the best mode per query and calculate metric deltas.
  for (auto &[query, queryEntry] : masterReport["per_query"].items()) {
    if (!queryEntry.contains("modes") ||
      !queryEntry["modes"].is_object())
      continue;

    auto &modes = queryEntry["modes"];

    std::string bestMode;
    double bestRecall = -1.0;
    double bestMRR = -1.0;
    double bestNDCG = -1.0;

    for (const auto &[modeName, metrics] : modes.items()) {
      const double recall = metrics.value("recall_at_k", 0.0);
      const double mrr = metrics.value("mrr", 0.0);

      if (recall > bestRecall ||
        (recall == bestRecall && mrr > bestMRR)) {
        bestRecall = recall;
        bestMRR = mrr;
        bestNDCG = metrics.value("ndcg", 0.0);
        bestMode = modeName;
      }
    }

    if (bestMode.empty()) continue;

    queryEntry["best_mode"] = bestMode;

    for (auto &[modeName, metrics] : modes.items()) {
      metrics["recall_delta_vs_best"] = metrics.value("recall_at_k", 0.0) - bestRecall;
      metrics["mrr_delta_vs_best"] = metrics.value("mrr", 0.0) - bestMRR;
      metrics["ndcg_delta_vs_best"] = metrics.value("ndcg", 0.0) - bestNDCG;
    }
  }

  // Calculate aggregate scores for each mode.
  auto &modeScores = masterReport["summary"]["mode_scores"];
  modeScores = json::object();

  for (const auto &[modeName, report] : reports) {
    if (!report.is_object()) continue;

    const auto summary = report.value("summary", json::object());
    if (!summary.is_object()) continue;

    modeScores[modeName] = {
      { "recall_at_k", summary.value("recall_at_k", 0.0) },
      { "mrr", summary.value("mrr", 0.0) },
      { "ndcg", summary.value("ndcg", 0.0) }
    };
  }

  std::string bestOverallMode;
  double bestOverallScore = -1.0;

  for (const auto &[modeName, scores] : modeScores.items()) {
    const double score =
      scores.value("recall_at_k", 0.0) +
      scores.value("mrr", 0.0) +
      scores.value("ndcg", 0.0);

    if (score > bestOverallScore) {
      bestOverallScore = score;
      bestOverallMode = modeName;
    }
  }

  masterReport["summary"]["best_overall_mode"] = bestOverallMode;
  masterReport["summary"]["best_overall_score"] = bestOverallScore;

  // Use round-trip-safe float strings in the output filename.
  auto formatFloat = [](float value) {
    char buffer[64];
    const auto result = std::to_chars(
      buffer, buffer + sizeof(buffer), value);

    return result.ec == std::errc{}
      ? std::string(buffer, result.ptr)
      : std::to_string(value);
    };

  const std::string filename =
    "master_report_topk_" + std::to_string(topK) +
    "_bm25_" + formatFloat(bm25Weight) +
    "_grep_" + formatFloat(grepWeight) + ".json";

  const auto outPath = std::filesystem::current_path() / filename;
  std::ofstream out(outPath);
  out << std::setw(2) << masterReport;
  out.close();

  LOG_MSG << "Master evaluation report saved to" << outPath;
}

void eval::Evaluator::addVectors(const std::string &datasetPath, const std::string &outputPath) const
{
  std::ifstream f(datasetPath);
  if (!f.is_open()) {
    LOG_MSG << "Cannot open dataset file:" << datasetPath;
    return;
  }
  json dataset;
  try {
    f >> dataset;
  } catch (const std::exception &e) {
    LOG_MSG_ERROR << "Invalid JSON in dataset:" << e.what();
    return;
  }
  f.close();

  if (!dataset.is_array() || dataset.empty()) {
    LOG_MSG << "Dataset must be a non-empty JSON array.";
    return;
  }
  if (fs::absolute(outputPath) == fs::absolute(datasetPath)) {
    LOG_MSG << "Refusing to overwrite the input dataset. Use --output <other.json>.";
    return;
  }

  EmbeddingClient ec{ app_.settings().embeddingCurrentApi(), app_.settings().embeddingTimeoutMs() };

  size_t added = 0, skipped = 0, failed = 0;
  for (size_t i = 0; i < dataset.size(); ++i) {
    auto &entry = dataset[i];
    if (!entry.is_object() || !entry.contains("query") || !entry["query"].is_string()) {
      LOG_MSG << "Skipping entry" << i << ": missing query";
      ++skipped;
      continue;
    }
    // Idempotent: keep vectors already present (unless you prefer to regenerate)
    if (entry.contains("embedding") && entry["embedding"].is_array() && !entry["embedding"].empty()) {
      ++skipped;
      continue;
    }
    try {
      std::vector<float> emb;
      ec.generateEmbeddings(entry["query"].get<std::string>(), emb, EmbeddingClient::EncodeType::Query);
      if (emb.empty())
        throw std::runtime_error("empty embedding returned");
      entry["embedding"] = emb;
      ++added;
    } catch (const std::exception &e) {
      LOG_MSG_ERROR << "Embedding failed for entry" << i << ":" << e.what();
      ++failed;
    }
  }

  if (added == 0) {
    LOG_MSG << "No new embeddings generated. Output not written.";
    return;
  }

  std::ofstream out(outputPath);
  if (!out.is_open()) {
    LOG_MSG << "Cannot open output file:" << outputPath;
    return;
  }
  out << std::setw(2) << dataset;
  out.close();
  LOG_MSG << "Wrote" << added << "embeddings to" << outputPath << "(skipped" << skipped << ", failed" << failed << ")";
  LOG_MSG << "File saved at" << fs::absolute(outputPath);
}
