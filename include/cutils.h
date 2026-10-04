#ifndef _PHENIXCODE_UTILS_H_
#define _PHENIXCODE_UTILS_H_

#include <string>
#include <string_view>
#include <vector>
#include <set>
#include <filesystem>

namespace utils {

  const std::filesystem::path &getExecutableDir();
  std::string currentTimestamp();
  time_t getFileModificationTime(const std::string &path);
  int safeStoI(const std::string &s, int def = 0);
  std::string trimmed(std::string_view sv);
  std::string addLineComments(std::string_view code, std::string_view filename);
  std::string stripMarkdownFromCodeBlock(std::string_view code);
  std::size_t strFindIn(std::string_view in, std::string_view t, bool caseSensitive);
  std::string commonDirPrefix(const std::set<std::string> &paths);
  void stripPrefix(std::string &s, const std::string &prefix);
  void stripCommonPathPrefix(std::vector<std::string> &paths);
  void stripCommonPathPrefix(std::set<std::string> &paths);

} // namespace utils

#endif // _PHENIXCODE_UTILS_H_