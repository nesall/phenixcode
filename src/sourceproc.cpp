#include "sourceproc.h"
#include "settings.h"
#include "database.h"
#include "chunker.h"
#include "cutils.h"
#include <string>
#include <tuple>
#include <vector>
#include <fstream>
#include <exception>
#include <cstring>
#include <filesystem>
#include <utility>
#include <iterator>
#include <algorithm>
#include <cmath>
#include <httplib.h>
#include <utils_log/logger.hpp>
#include <3rdparty/utf8.h>

namespace {

  std::string toLowerAscii(std::string_view text) {
    std::string out(text.size(), '\0');
    for (size_t i = 0; i < text.size(); ++i) {
      const unsigned char ch = static_cast<unsigned char>(text[i]);
      out[i] = static_cast<char>((ch >= 'A' && ch <= 'Z') ? ch + ('a' - 'A') : ch);
    }
    return out;
  }

  float occWeight(std::string_view line, size_t p, size_t len)
  {
    auto isId = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    std::string_view pre = line.substr(0, p);
    while (!pre.empty() && std::isspace(static_cast<unsigned char>(pre.back()))) pre.remove_suffix(1);
    auto ends = [&](std::string_view s) { return pre.size() >= s.size() && pre.substr(pre.size() - s.size()) == s; };
    auto lastWord = [](std::string_view s) {
      size_t b = s.size();
      while (b > 0 && !std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
      return s.substr(b);
      };
    if (ends(".") || ends("->")) return 0.5f;
    const auto w = lastWord(pre);                       // line is already lowercased
    if (w == "class" || w == "struct" || w == "enum" || w == "using" || w == "typedef" || w == "#define") return 3.0f;
    const bool call = p + len < line.size() && line[p + len] == '(';
    if (call && line.find(';') == std::string_view::npos && pre.find_first_of("=(") == std::string_view::npos) {
      std::string_view q = pre;
      const bool qualified = ends("::") || ends("::~");
      if (qualified) {                                  // strip `Cls::` / `Cls::~`
        if (q.back() == '~') q.remove_suffix(1);
        while (q.size() >= 2 && q.substr(q.size() - 2) == "::") {
          q.remove_suffix(2);
          while (!q.empty() && isId(q.back())) q.remove_suffix(1);
        }
        while (!q.empty() && std::isspace(static_cast<unsigned char>(q.back()))) q.remove_suffix(1);
      }
      if (qualified && q.empty()) return 3.0f;          // Cls::Cls( / Cls::~Cls(
      const auto lw = lastWord(q);
      if (!q.empty() && (isId(q.back()) || q.back() == '>' || q.back() == '*' || q.back() == '&') &&
        lw != "return" && lw != "new" && lw != "else" && lw != "throw" && lw != "case" && lw != "delete")
        return 3.0f;
    }
    if (!call && (pre.find("const") != std::string_view::npos || pre.find("static") != std::string_view::npos ||
      pre.find("inline") != std::string_view::npos)) return 2.0f;
    return 1.0f;
  }

} // anonymous namespace


std::vector<SourceProcessor::Data> SourceProcessor::collectSources(bool readContent) const
{
  //LOG_START;
  std::vector<SourceProcessor::Data> allContent;
  auto sources = settings_.sources();
  for (const auto &source : sources) {
    std::string type = source.type;
    if (type == "directory") {
      processDirectory(source, allContent, readContent);
    } else if (type == "file") {
      processFile(source.path, allContent, readContent);
    } else if (type == "url") {
      processUrl(source, allContent, readContent);
    }
  }
  return allContent;
}

SourceProcessor::Data SourceProcessor::fetchSource(const std::string &uri) const
{
  LOG_START;
  std::vector<SourceProcessor::Data> res;
  bool isUrl = (uri.find("://") != std::string::npos);
  if (isUrl) {
    auto sources = settings_.sources();
    for (const auto &sr: sources)
      if (sr.url == uri) {
        processUrl(sr, res, true);
        break;
      }
  } else
    processFile(uri, res, true);
  return res.empty() ? Data{} : res[0];
}

std::vector<std::string> SourceProcessor::filterRelatedSources(const std::vector<std::string> &sources, const std::string &uri) const
{
  std::vector<std::string> res;
  std::string base = std::filesystem::path(uri).stem().string();
  const auto maxPerSource = settings_.generationMaxRelatedPerSource();
  for (const auto &s : sources) {
    if (res.size() == maxPerSource) break;
    if (std::filesystem::path(uri) == std::filesystem::path(s)) continue;
    auto t = std::filesystem::path(s).stem().string();
    if (t == base || t.find(base) != std::string::npos) {
      res.push_back(s);
    }
  }
  return res;
}

bool SourceProcessor::readFile(const std::string &uri, std::string &data, bool sanitize)
{
  std::ifstream file(uri);
  if (file.is_open()) {
    std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (sanitize)
      data = std::move(utils::sanitizeUtf8(std::move(raw)));
    else 
      data = std::move(raw);
    return true;
  }
  return false;
}

bool SourceProcessor::readFileRaw(const std::string &uri, std::string &data, bool normalizeNewlines)
{
  std::ifstream f(uri, std::ios::binary | std::ios::ate);
  if (!f) return false;
  const std::streamoff n = f.tellg();
  if (n <= 0) {
    data.clear();
    return false;
  }
  data.resize(static_cast<size_t>(n));
  f.seekg(0);
  f.read(data.data(), n);
  data.resize(static_cast<size_t>(f.gcount()));
  if (normalizeNewlines) {
    // CRLF -> LF, preserving lone '\r'.
    size_t w = 0;
    size_t r = 0;
    while (r < data.size()) {
      const size_t cr = data.find("\r\n", r);
      if (cr == std::string::npos)
        break;
      const size_t len = cr - r;
      if (len != 0) {
        if (w != r)
          std::memmove(data.data() + w, data.data() + r, len);
        w += len;
      }
      // Skip '\r', but retain '\n'.
      r = cr + 1;
    }
    // Copy the remaining suffix.
    const size_t len = data.size() - r;
    if (len != 0) {
      if (w != r)
        std::memmove(data.data() + w, data.data() + r, len);
      w += len;
    }
    data.resize(w);
  }
  return !data.empty();
}


std::vector<SearchResult> SourceProcessor::grep(const std::vector<std::string> &terms, const std::vector<std::string> &filePaths) const
{
  return grep(terms, filePaths, GrepOptions{});
}

std::vector<SearchResult> SourceProcessor::grep(const std::vector<std::string> &termsArg, const std::vector<std::string> &filePaths, const GrepOptions &opts) const
{
  if (termsArg.empty()) return {};

  std::vector<std::string> targets = filePaths;
  if (targets.empty()) {
    for (const auto &d : collectSources(false)) {
      if (!d.isUrl) targets.push_back(d.source);
    }
  }
  if (targets.empty()) return {};

  // Prepare terms and their lowercase equivalents.
  std::vector<std::string> terms;
  std::vector<std::string> loweredTerms;

  for (const auto &term : termsArg) {
    if (term.length() > 1 && term.length() < 65) {
      terms.push_back(term);
      loweredTerms.push_back(toLowerAscii(term));
    }
  }

  if (terms.empty()) return {};

  std::vector<float> idfByTerm;
  float meanIdf = 1.0f;

  // Pass 1: count readable files and document frequency for each term.
  // Files are streamed one at a time; no corpus-wide content cache.
  if (opts.idfPruning) {
    std::vector<size_t> df(terms.size(), 0);
    size_t readable = 0;
    std::string content;

    for (const auto &path : targets) {
      if (!readFileRaw(path, content, true)) continue;

      ++readable;
      const std::string loweredContent = toLowerAscii(content);

      for (size_t t = 0; t < terms.size(); ++t) {
        if (loweredContent.find(loweredTerms[t]) != std::string::npos) {
          ++df[t];
        }
      }
    }

    if (readable == 0) return {};

    // Keep terms appearing in at least one file and no more than
    // maxDf files. The threshold is based on readable files, not
    // the total number of requested paths.
    const size_t maxDf = std::max<size_t>(3, readable / 4);

    std::vector<std::string> keptTerms;
    std::vector<std::string> keptLowered;
    std::vector<size_t> keptDf;

    keptTerms.reserve(terms.size());
    keptLowered.reserve(loweredTerms.size());
    keptDf.reserve(terms.size());

    for (size_t t = 0; t < terms.size(); ++t) {
      if (df[t] == 0 || df[t] > maxDf) continue;

      keptTerms.push_back(std::move(terms[t]));
      keptLowered.push_back(std::move(loweredTerms[t]));
      keptDf.push_back(df[t]);
    }

    if (keptTerms.empty()) return {};

    terms = std::move(keptTerms);
    loweredTerms = std::move(keptLowered);

    // IDF: ln(1 + N / df), where N is the readable-file count.
    idfByTerm.resize(keptDf.size());
    float idfSum = 0.0f;

    for (size_t t = 0; t < keptDf.size(); ++t) {
      idfByTerm[t] = std::log1p(
        static_cast<float>(readable) /
        static_cast<float>(keptDf[t]));

      idfSum += idfByTerm[t];
    }

    meanIdf = idfSum / static_cast<float>(idfByTerm.size());
    if (meanIdf < 1.0f) meanIdf = 1.0f;
  }

  // Pass 2: read each file again and build matching windows.
  size_t syntheticIdCounter = 0;
  std::vector<SearchResult> results;

  for (const auto &path : targets) {
    if (results.size() >= opts.maxTotalResults) break;

    std::string content;
    if (!readFileRaw(path, content, true)) continue;

    const std::string loweredContent = toLowerAscii(content);

    std::vector<std::string_view> lines;
    size_t pos = 0;

    while (pos < content.size()) {
      const size_t next = content.find('\n', pos);

      if (next == std::string::npos) {
        lines.emplace_back(
          content.data() + pos, content.size() - pos);
        pos = content.size();
      } else {
        lines.emplace_back(
          content.data() + pos, next - pos);
        pos = next + 1;
      }
    }

    if (lines.empty()) continue;

    // Matched line ranges [start, end], inclusive and zero-indexed.
    struct Window {
      size_t start;
      size_t end;
      float hits;
      bool exactCase;
    };

    std::vector<Window> windows;
    size_t fileMatches = 0;

    for (size_t i = 0;
      i < lines.size() && fileMatches < opts.maxMatchesPerFile;
      ++i) {
      const size_t offset = static_cast<size_t>(lines[i].data() - content.data());

      const std::string_view loweredLine(loweredContent.data() + offset, lines[i].size());

      float lineScore = 0.0f;
      bool exact = false;

      for (size_t t = 0; t < terms.size(); ++t) {
        if (terms[t].empty()) continue;

        // Count only the first occurrence of each term per line.
        const size_t p = loweredLine.find(loweredTerms[t]);
        if (p == std::string_view::npos) continue;

        const float w = occWeight(loweredLine, p, loweredTerms[t].size());

        lineScore += w * (idfByTerm.empty() ? 1.0f : idfByTerm[t]);

        if (lines[i].find(terms[t]) != std::string_view::npos) {
          exact = true;
        }
      }

      if (lineScore <= 0.0f) continue;

      ++fileMatches;

      const size_t wStart = (i > opts.contextLines) ? i - opts.contextLines : 0;

      const size_t wEnd = std::min(lines.size() - 1, i + opts.contextLines);

      if (!windows.empty() &&
        wStart <= windows.back().end + 1) {
        windows.back().end = std::max(windows.back().end, wEnd);
        windows.back().hits += lineScore;
        windows.back().exactCase |= exact;
      } else {
        windows.push_back({ wStart, wEnd, lineScore, exact });
      }
    }

    if (windows.empty()) continue;

    const auto contentType = Chunker::contentTypeToStr(Chunker::detectContentType(content, path));

    for (const auto &w : windows) {
      if (results.size() >= opts.maxTotalResults) break;

      std::string snippet;

      for (size_t ln = w.start; ln <= w.end; ++ln) {
        snippet += std::to_string(ln + 1) + ": ";
        snippet += lines[ln];
        snippet += '\n';
      }

      SearchResult sr;
      sr.content =
        utf8::find_invalid(snippet.begin(), snippet.end()) !=
        snippet.end()
        ? utils::sanitizeUtf8(snippet)
        : std::move(snippet);

      sr.sourceId = path;
      sr.chunkUnit = "line";
      sr.chunkType = contentType;

      // Synthetic ID keeps separate grep windows distinct.
      sr.chunkId = std::string::npos - (++syntheticIdCounter);
      sr.start = w.start + 1;
      sr.end = w.end + 1;

      const float normalizedHits = w.hits / meanIdf;
      sr.bm25Score = normalizedHits * (w.exactCase ? 1.5f : 1.0f);
      sr.similarityScore = sr.bm25Score / (sr.bm25Score + opts.saturationScale);
      sr.fusedScore = sr.similarityScore;

      results.push_back(std::move(sr));
    }
  }

  std::sort(
    results.begin(), results.end(),
    [](const SearchResult &a, const SearchResult &b) {
      return std::tie(b.bm25Score, a.sourceId, a.chunkId) <
        std::tie(a.bm25Score, b.sourceId, b.chunkId);
    });

  return results;
}


void SourceProcessor::processDirectory(const Settings::SourceItem &source, std::vector<SourceProcessor::Data> &content, bool readContent) const
{
  namespace fs = std::filesystem;
  std::string path = source.path;
  const auto &extensions = source.extensions;
  const auto &exclude = source.exclude;
  try {
    for (const auto &entry : fs::directory_iterator(path)) {
      const auto entryPath = entry.path().string();
      if (isExcluded(entryPath, exclude))
        continue;
      if (entry.is_directory() && source.recursive) {
        Settings::SourceItem subSource = source;
        subSource.path = entryPath;
        processDirectory(subSource, content, readContent);
      } else if (entry.is_regular_file()) {
        processDirItem(source, entryPath, content, readContent);
      }
    }
  } catch (const std::exception &) {
    LOG_MSG_ERROR << "Unable to process resource " << path << ". Skipped.";
  }
}

bool SourceProcessor::processDirItem(const Settings::SourceItem &source, const std::string &filepath, std::vector<SourceProcessor::Data> &content, bool readContent) const
{
  if (isExcluded(filepath, source.exclude)) return false;
  if (!source.extensions.empty() && !hasValidExtension(filepath, source.extensions)) return false;
  processFile(filepath, content, readContent);
  return true;
}

void SourceProcessor::processFile(const std::string &filepath, std::vector<SourceProcessor::Data> &content, bool readContent) const
{
  if (auto maxSize = settings_.filesMaxFileSizeMb(); 0 < maxSize) {
    try {
      auto fsize = std::filesystem::file_size(filepath);
      if (maxSize * 1024 * 1024 < fsize) {
        LOG_MSG_WARN << "File " << filepath << " exceeds max allowed size of " << maxSize << " MB. Skipped.";
        return;
      }
    } catch (const std::exception &) {
      LOG_MSG_ERROR << "Unable to process resource " << filepath << ". Skipped.";
      return;
    }
  }
  std::string text;
  if (!readContent || SourceProcessor::readFile(filepath, text)) {
    content.push_back({ false, std::move(text), std::filesystem::path(filepath).lexically_normal().generic_string() });
  } else {
    LOG_MSG_WARN << "Unable to process resource " << filepath << ". Skipped.";
  }
}

void SourceProcessor::processUrl(const Settings::SourceItem &source, std::vector<SourceProcessor::Data> &content, bool readContent) const
{
  if (!readContent) {
    // Optionally handle the case where URL content shouldn't be read
    content.push_back({ true, "", source.url });
    return;
  }
  std::string url = source.url;
  time_t timeout = static_cast<time_t>(source.urlTimeoutMs);
  try {
    size_t pos = url.find("://");
    if (pos == std::string::npos) return;
    size_t host_start = pos + 3;
    size_t path_start = url.find("/", host_start);
    std::string host = url.substr(host_start, path_start - host_start);
    std::string path = url.substr(path_start);
    httplib::Client client(("https://" + host).c_str());
    client.set_connection_timeout(0, timeout * 1000);
    httplib::Headers headers;
    for (auto &[key, value] : source.headers) {
      headers.emplace(key, value);
    }
    auto res = client.Get(path.c_str(), headers);
    if (res && res->status == 200) {
      content.push_back({ true, res->body, url });
    }
  } catch (const std::exception &) {
    LOG_MSG_ERROR << "Unable to process resource " << url << ". Skipped.";
  }
}

bool SourceProcessor::isExcluded(const std::string &filepath, const std::vector<std::string> &patterns)
{
  auto normalize = [](std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
    };

  auto path = std::filesystem::path(filepath).lexically_normal().generic_string();

  if (std::filesystem::is_directory(path) && !path.ends_with('/')) {
    path += '/';
  }

  for (const auto &p : patterns) {
    std::string pattern = normalize(p);
    if (pattern.empty()) continue;
    if (pattern == "*") return true; // exclude all
    if (pattern.front() == '*' && pattern.back() == '*') {
      auto core = pattern.substr(1, pattern.length() - 2);
      if (!core.empty() && path.find(core) != std::string::npos) return true;
    } else if (pattern.front() == '*') {
      auto core = pattern.substr(1);
      if (!core.empty() && path.ends_with(core)) return true;
    } else if (pattern.back() == '*') {
      auto core = pattern.substr(0, pattern.length() - 1);
      if (!core.empty() && path.starts_with(core)) return true;
    } else {
      if (path == pattern || std::filesystem::path(path).filename() == pattern) return true;
    }
  }
  return false;
}

bool SourceProcessor::hasValidExtension(const std::string &filepath, const std::vector<std::string> &extensions)
{
  for (const auto &ext : extensions) {
    if (filepath.ends_with(ext)) return true;
  }
  return false;
}
