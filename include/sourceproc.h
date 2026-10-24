#ifndef _SOURCEPROC_H_
#define _SOURCEPROC_H_

#include <vector>
#include <string>
#include "settings.h"

struct SearchResult;

class SourceProcessor {
public:
  struct Data {
    bool isUrl = false;
    std::string content;
    std::string source;
  };
  struct GrepOptions {
    size_t contextLines = 3;
    size_t maxMatchesPerFile = 50;
    size_t maxTotalResults = 200;
    bool useRegex = false;
    bool idfPruning = false; // two-pass DF prune; use when no FTS index exists
    float saturationScale = 10.0f;
  };
private:
  Settings settings_;
public:
  SourceProcessor(const Settings &s) : settings_(s) {}
  void setSettings(const Settings &s) { settings_ = s; }
  std::vector<SourceProcessor::Data> collectSources(bool readContent) const;
  SourceProcessor::Data fetchSource(const std::string &uri) const;
  std::vector<std::string> filterRelatedSources(const std::vector<std::string> &sources, const std::string &src) const;
  static bool readFile(const std::string &uri, std::string &data, bool sanitize = true);
  static bool readFileRaw(const std::string &uri, std::string &data, bool normalizeNewlines);
  std::vector<SearchResult> grep(
    const std::vector<std::string> &terms,
    const std::vector<std::string> &filePaths, // if empty, uses collectSources(false)
    const GrepOptions &opts = {}) const;

private:
  void processDirectory(const Settings::SourceItem &source, std::vector<SourceProcessor::Data> &content, bool readContent) const;
  bool processDirItem(const Settings::SourceItem &source, const std::string &filepath, std::vector<SourceProcessor::Data> &content, bool readContent) const;
  void processFile(const std::string &filepath, std::vector<SourceProcessor::Data> &content, bool readContent) const;
  void processUrl(const Settings::SourceItem &source, std::vector<SourceProcessor::Data> &content, bool readContent) const;
  static bool isExcluded(const std::string &filepath, const std::vector<std::string> &patterns);
  static bool hasValidExtension(const std::string &filepath, const std::vector<std::string> &extensions);
};

#endif // _SOURCEPROC_H_