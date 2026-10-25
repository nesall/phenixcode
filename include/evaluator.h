#ifndef PHENIXCODE_EVALUATOR_H
#define PHENIXCODE_EVALUATOR_H

#include <3rdparty/nlohmann/json_fwd.hpp>
#include <string>
#include <vector>

class App;

namespace eval {

  struct GrepDiagEntry {
    std::string docId;    // sourceId of the hybrid candidate
    size_t grepRank;      // position in grepRes (0-based); npos if not from grep
    float boost;          // grepWeight / (rrfK + grepRank + 1) actually added
  };

  struct RetrieveDiagnostics {
    std::vector<std::string> grepTerms;
    std::vector<GrepDiagEntry> grepRanking;  // every grep hit, in grep order
    bool grepRan = false;
  };

  class Evaluator {
    const App &app_;
  public:
    explicit Evaluator(const App &app);
    ~Evaluator() = default;

    void run(const std::string &datasetPath, size_t topK, float bm25Weight, float grepWeight) const;
    void init(const std::string &outputPath, size_t numSamples = 10);
    void list();
    void detail(const std::string &docId);
    void sweep(const std::string &ds,
      const std::vector<size_t> &topKs,
      const std::vector<float> &bm25Weights,
      const std::vector<float> &grepWeights) const;
    void addVectors(const std::string &datasetPath, const std::string &outputPath) const;
  };

} // namespace eval

#endif // PHENIXCODE_EVALUATOR_H
