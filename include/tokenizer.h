#ifndef _TOKENIZER_H_
#define _TOKENIZER_H_

#include <string>
#include <string_view>
#include <unordered_map>
#include <mutex>
#include "json_shim.h"

class SimpleTokenizer {
  mutable std::mutex mutex_;
  mutable std::unordered_map<std::string, size_t> cache_;
private:
  nlohmann::json vocab_;
  size_t maxInputCharsPerWord_ = 100;
  size_t simulateWordpiece(const std::string &word, bool addSpecialTokens) const;
public:
  explicit SimpleTokenizer(const std::string &configPath);
  size_t estimateTokenCount(std::string_view text, bool addSpecialTokens = false) const;
  size_t countTokensWithVocab(std::string_view text, bool addSpecialTokens = false) const;
};

#endif // _TOKENIZER_H_
