#ifndef _PHENIXCODE_UTILS_H_
#define _PHENIXCODE_UTILS_H_

#include <string>
#include <string_view>
#include <vector>
#include <set>
#include <initializer_list>
#include <filesystem>
#include <cstdint>

namespace utils {

  template <typename T> constexpr bool hasFlag(T mode, T flag) { return (mode & flag) == flag; }

  [[nodiscard]] const std::filesystem::path &getExecutableDir();
  [[nodiscard]] std::string currentTimestamp();
  [[nodiscard]] time_t getFileModificationTime(const std::string &path);
  [[nodiscard]] int safeStoI(const std::string &s, int def = 0);
  [[nodiscard]] std::string trimmed(std::string_view sv);
  [[nodiscard]] std::string addLineComments(std::string_view code, std::string_view filename);
  [[nodiscard]] std::string stripMarkdownFromCodeBlock(std::string_view code);
  [[nodiscard]] std::size_t strFindIn(std::string_view in, std::string_view t, bool caseSensitive);
  [[nodiscard]] bool containsAny(std::string_view text, std::initializer_list<std::string_view> markers);
  [[nodiscard]] std::string commonDirPrefix(const std::set<std::string> &paths);
  void stripPrefix(std::string &s, const std::string &prefix);
  void stripCommonPathPrefix(std::vector<std::string> &paths);
  void stripCommonPathPrefix(std::set<std::string> &paths);
  [[nodiscard]] std::filesystem::path monitorPathFor(std::filesystem::path settingsFilePath);
  [[nodiscard]] std::vector<std::string> extractGrepTerms(const std::string &text);
  [[nodiscard]] std::vector<std::string> extractGrepLiterals(const std::string &text);
  [[nodiscard]] bool looksCodeLike(const std::string &text);
  [[nodiscard]] bool isIdentifierLike(const std::string &text);
  [[nodiscard]] bool isHeader(const std::string &path);
  [[nodiscard]] std::string stemKey(const std::string &path);
  [[nodiscard]] std::string expandEnvVar(const std::string var);

  enum class QueryIntent : uint8_t { Neutral, Declaration, Implementation };
  [[nodiscard]] QueryIntent queryIntent(const std::string &query);
} // namespace utils

#endif // _PHENIXCODE_UTILS_H_