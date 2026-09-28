#include "tokenizer.h"
#include <utils_log/logger.hpp>
#include <vector>
#include <string>
#include <string_view>
#include <fstream>
#include <sstream>

using json = nlohmann::json;

namespace {

  bool isPunctuation(char c) {
    return (c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) || (c >= 123 && c <= 126);
  }

  bool isChineseChar(uint32_t c) {
    return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0xF900 && c <= 0xFAFF);
  }

  std::vector<uint32_t> utf8ToUtf32(std::string_view str) {
    std::vector<uint32_t> result;
    for (size_t i = 0; i < str.size();) {
      uint32_t codepoint = 0;
      unsigned char c = str[i];
      if (c < 0x80) {
        codepoint = c;
        i += 1;
      } else if ((c >> 5) == 0x06) {
        codepoint = ((c & 0x1F) << 6) | (str[i + 1] & 0x3F);
        i += 2;
      } else if ((c >> 4) == 0x0E) {
        codepoint = ((c & 0x0F) << 12) |
          ((str[i + 1] & 0x3F) << 6) |
          (str[i + 2] & 0x3F);
        i += 3;
      } else if ((c >> 3) == 0x1E) {
        codepoint = ((c & 0x07) << 18) |
          ((str[i + 1] & 0x3F) << 12) |
          ((str[i + 2] & 0x3F) << 6) |
          (str[i + 3] & 0x3F);
        i += 4;
      } else {
        i += 1; // Skip invalid byte
      }
      if (codepoint != 0)
        result.push_back(codepoint);
    }
    return result;
  }

  std::string uint32ToUtf8(uint32_t c) {
    std::string res;
    if (c < 0x80) {
      res += static_cast<char>(c);
    } else if (c < 0x800) {
      res += static_cast<char>(0xC0 | (c >> 6));
      res += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      res += static_cast<char>(0xE0 | (c >> 12));
      res += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      res += static_cast<char>(0x80 | (c & 0x3F));
    } else {
      res += static_cast<char>(0xF0 | (c >> 18));
      res += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
      res += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      res += static_cast<char>(0x80 | (c & 0x3F));
    }
    return res;
  }

  std::string padChineseChars(std::string_view text) {
    auto utf32Chars = utf8ToUtf32(text);
    std::string result;
    for (auto c : utf32Chars) {
      if (isChineseChar(c)) {
        result += " " + uint32ToUtf8(c) + " ";
      } else {
        result += uint32ToUtf8(c);
      }
    }
    return result;
  }

  std::vector<std::string> splitSimple(const std::string &input) {
    std::istringstream stream(input);
    std::vector<std::string> words;
    words.reserve(input.size() / 3);
    std::string word;
    while (stream >> word) {
      words.push_back(word);
    }
    return words;
  }

  void splitOnPunctSimple(const std::string &text, std::vector<std::string> &result) {
    result.reserve(text.length() / 3);
    std::string current;
    for (char c : text) {
      if (isPunctuation(c)) {
        if (!current.empty()) {
          result.push_back(current);
          current.clear();
        }
        result.emplace_back(1, c);
      } else {
        current += c;
      }
    }
    if (!current.empty()) {
      result.emplace_back(current);
    }
  }

} // anonymous namespace


SimpleTokenizer::SimpleTokenizer(const std::string &configPath)
{
  std::ifstream file(configPath);
  if (file.is_open()) {
    json jsonObj;
    file >> jsonObj;
    if (jsonObj.contains("model") && jsonObj["model"].contains("vocab")) {
      vocab_ = jsonObj["model"]["vocab"];
      LOG_MSG << "Using vocab file" << configPath << "with" << vocab_.size() << "entries.";
    }
  } else {
    LOG_MSG << "Unable to locate vocab file" << configPath << ". Skipped.";
  }
}

size_t SimpleTokenizer::estimateTokenCount(std::string_view text, bool addSpecialTokens) const
{
  if (text.empty() && !addSpecialTokens) return 0;
  std::string padded = padChineseChars(text);
  std::vector<std::string> words = splitSimple(padded);
  size_t totalTokens = addSpecialTokens ? 2 : 0; // [CLS] + [SEP]
  for (const auto &word : words) {
    std::vector<std::string> punctSplit;
    splitOnPunctSimple(word, punctSplit);
    for (const auto &token : punctSplit) {
      if (token.empty()) continue;
      if (token.length() <= 4) {
        totalTokens += 1;
      } else if (token.length() <= 8) {
        totalTokens += 2;
      } else {
        totalTokens += (token.length() + 3) / 4;
      }
    }
  }

  return totalTokens;
}

size_t SimpleTokenizer::countTokensWithVocab(std::string_view text, bool addSpecialTokens) const
{
  if (text.empty() && !addSpecialTokens) return 0;
  if (vocab_.empty()) {
    return estimateTokenCount(text, addSpecialTokens);
  }
  std::string padded = padChineseChars(text);
  std::vector<std::string> words = splitSimple(padded);
  size_t totalTokens = addSpecialTokens ? 2 : 0; // [CLS] + [SEP]
  for (const auto &word : words) {
    std::vector<std::string> punctSplit;
    splitOnPunctSimple(word, punctSplit);
    for (const auto &token : punctSplit) {
      if (token.empty()) continue;
      totalTokens += simulateWordpiece(token, addSpecialTokens);
    }
  }
  return totalTokens;
}

size_t SimpleTokenizer::simulateWordpiece(const std::string &word, bool addSpecialTokens) const
{
  if (word.length() > maxInputCharsPerWord_) {
    return addSpecialTokens ? 1 : 0; // [UNK]
  }
  
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = cache_.find(word); it != cache_.end())
      return it->second;    
  }
  
  size_t tokens = 0;
  size_t start = 0;
  while (start < word.length()) {
    size_t bestEnd = start + 1;
    for (size_t end = word.length(); end > start; --end) {
      std::string substr = word.substr(start, end - start);
      if (start > 0) {
        substr = "##" + substr;
      }
      if (vocab_.contains(substr)) {
        bestEnd = end;
        break;
      }
    }
    tokens++;
    start = bestEnd;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_[word] = tokens;
  }
  return tokens;
}
