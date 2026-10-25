#ifndef PHENIXCODE_UPDATER_H
#define PHENIXCODE_UPDATER_H

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <string_view>
#include <vector>

class App;
class VectorDatabase;
class EmbeddingClient;
class Chunker;
class Settings;
class MonitorTracker;
struct Chunk;

namespace updater {

  size_t addEmbedChunks(
    const std::vector<Chunk> &chunks, 
    size_t batchSize, 
    const EmbeddingClient &ec, 
    VectorDatabase &db, 
    std::string_view prependlabelFmt, 
    MonitorTracker *monitor, 
    size_t filesDone, 
    size_t filesTotal, 
    const Settings *settings);

  class IncrementalUpdater {
  public:
    struct UpdateInfo {
      std::vector<std::string> newFiles;
      std::vector<std::string> modifiedFiles;
      std::vector<std::string> deletedFiles;
      std::vector<std::string> unchangedFiles;
    };

    IncrementalUpdater(App *app, size_t batchSize);
    ~IncrementalUpdater() = default;

    UpdateInfo detectChanges(const std::vector<std::string> &currentFiles);

    bool needsUpdate(const UpdateInfo &info);

    size_t updateDatabase(
      EmbeddingClient &client,
      Chunker &chunker,
      const UpdateInfo &info);

    void printUpdateSummary(const UpdateInfo &info);

    bool shouldIgnore(const std::string &filepath) const;
    void recordFailure(const std::string &filepath);
    void clearFailure(const std::string &filepath);

  private:
    App &app_;
    VectorDatabase *db_;
    size_t batchSize_;

    std::unordered_map<std::string, int> failureCounts_;
    std::unordered_set<std::string> ignoredFiles_;
  };

} // namespace updater

#endif // PHENIXCODE_UPDATER_H
