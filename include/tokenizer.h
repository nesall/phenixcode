#ifndef _TOKENIZER_H_
#define _TOKENIZER_H_

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <mutex>

class SimpleTokenizer {
  struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view text) const { return std::hash<std::string_view>{}(text); }
  };
  struct NormalizerStep { bool clean, chinese, lower, accents; };
  mutable std::mutex mutex_;
  mutable std::unordered_map<std::string, size_t, StringHash, std::equal_to<>> cache_;
  std::unordered_set<std::string, StringHash, std::equal_to<>> vocab_;
  std::vector<NormalizerStep> normalizers_;
  size_t maxInputCharsPerWord_ = 100;
  std::string normalize(std::string_view text) const;
  size_t simulateWordpiece(std::string_view word) const;
public:
  explicit SimpleTokenizer(const std::string &configPath);
  size_t estimateTokenCount(std::string_view text, bool addSpecialTokens = false) const;
  size_t countTokensWithVocab(std::string_view text, bool addSpecialTokens = false) const;
};

#endif // _TOKENIZER_H_
