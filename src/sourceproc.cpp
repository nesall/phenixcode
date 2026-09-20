#include "sourceproc.h"
#include "settings.h"
#include <iostream>
#include <fstream>
#include <exception>
#include <filesystem>
#include <iterator>
#include <httplib.h>
#include <utils_log/logger.hpp>
#include <3rdparty/utf8.h>


namespace {
  std::string sanitizeUtf8(const std::string &input) {
    std::string out;
    utf8::replace_invalid(input.begin(), input.end(), std::back_inserter(out), '?');
    return out;
  }
} // anonymous namespace


std::vector<SourceProcessor::Data> SourceProcessor::collectSources(bool readContent)
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
  for (const auto &a : allContent) {
    sources_.insert(a.source);
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

bool SourceProcessor::readFile(const std::string &uri, std::string &data)
{
  std::ifstream file(uri);
  if (file.is_open()) {
    std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    data = std::move(sanitizeUtf8(std::move(raw)));
    return true;
  }
  return false;
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
    LOG_MSG << "Unable to process resource " << path << ". Skipped.";
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
        LOG_MSG << "File " << filepath << " exceeds max allowed size of " << maxSize << " MB. Skipped.";
        return;
      }
    } catch (const std::exception &) {
      LOG_MSG << "Unable to process resource " << filepath << ". Skipped.";
      return;
    }
  }
  std::string text;
  if (!readContent || SourceProcessor::readFile(filepath, text)) {
    content.push_back({ false, std::move(text), std::filesystem::path(filepath).lexically_normal().generic_string() });
  } else {
    LOG_MSG << "Unable to process resource " << filepath << ". Skipped.";
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
    LOG_MSG << "Unable to process resource " << url << ". Skipped.";
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
