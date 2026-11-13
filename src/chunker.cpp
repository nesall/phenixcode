#include "chunker.h"
#include <iterator>
#include <regex>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_set>
#include <cmath>
#include <stdexcept>
#include <utils_log/logger.hpp>

namespace {
  struct Unit {
    size_t startChar;
    size_t endChar;
    size_t tokens;
  };


  class ContentTypeHelper {
  private:
    static constexpr double CODE_RATIO_STRONG = 0.25;
    static constexpr double CODE_RATIO_WEAK = 0.1;
    static constexpr double BRACE_RATIO_STRONG = 0.15;
    static constexpr double BRACE_RATIO_WEAK = 0.05;
    static constexpr double SEMICOLON_RATIO_STRONG = 0.2;
    static constexpr double SEMICOLON_RATIO_WEAK = 0.1;
    static constexpr double INDENT_RATIO_THRESHOLD = 0.6;
    static constexpr double INDENT_RATIO_STRONG = 0.5;
    static constexpr size_t MIN_CODE_INDICATORS = 2;
    static constexpr size_t STRONG_CODE_INDICATORS = 5;
    static constexpr size_t EARLY_EXIT_THRESHOLD = 5;
    static constexpr size_t MAX_LINES_TO_SCAN = 200;
    static constexpr size_t MAX_LINE_BYTES_TO_SCAN = 1024;
    static constexpr size_t MIN_LINES_FOR_ANALYSIS = 3;
    static constexpr double BINARY_THRESHOLD = 0.3;
    static constexpr size_t BINARY_CHECK_BYTES = 1024;

    // Precompiled optimized regexes (thread-safe after initialization)
    struct RegexPatterns {
      std::regex classStruct;
      std::regex functionDef;
      std::regex accessModifiers;
      std::regex imports;
      std::regex varDeclarations;
      std::regex controlFlow;
      std::regex arrowFunctions;
      std::regex loneBraces;
      std::regex comments;
      std::regex markdownFence;

      RegexPatterns() :
        classStruct(R"(\b(class|struct|interface|enum|trait)\s+\w+)",
          std::regex_constants::optimize),
        functionDef(R"(\b(def|function|func|fn|lambda|const\s+\w+\s*=\s*\([^)]*\)\s*=>)\s*)",
          std::regex_constants::optimize),
        accessModifiers(R"(\b(public|private|protected|static|final|virtual|override|async|await)\b)",
          std::regex_constants::optimize),
        imports(R"(^[ \t]*(#include|#import|import\s+\{|from\s+\S+\s+import|using\s+\w+))",
          std::regex_constants::optimize),
        varDeclarations(R"(\b(var|let|const|auto|int|float|double|bool|void|string)\s+\w+\s*[=;:])",
          std::regex_constants::optimize),
        controlFlow(R"(\bif\s*\(.*\)\s*\{|\bfor\s*\(.*\)|\bwhile\s*\()",
          std::regex_constants::optimize),
        arrowFunctions(R"(=>\s*\{|function\s*\(|:\s*function)",
          std::regex_constants::optimize),
        loneBraces(R"(^\s*[\{\}]\s*$)",
          std::regex_constants::optimize),
        comments(R"(^[ \t]*/[/*]|^[ \t]*\*|^[ \t]*//)",
          std::regex_constants::optimize),
        markdownFence(R"(^```)",
          std::regex_constants::optimize)
      {
      }
    };

    static const RegexPatterns &getPatterns() {
      static RegexPatterns patterns;
      return patterns;
    }

    inline static const std::unordered_set<std::string> codeExtensions = {
        ".cpp", ".h", ".hpp", ".c", ".cc", ".cxx",
        ".py", ".js", ".ts", ".jsx", ".tsx",
        ".java", ".cs", ".php", ".rb", ".go", ".rs",
        ".swift", ".kt", ".scala", ".m", ".mm",
        ".html", ".css", ".scss", ".xml", ".json",
        ".yaml", ".yml", ".sh", ".bash", ".sql"
    };

    inline static const std::unordered_set<std::string> textExtensions = {
        ".md", ".txt", ".rst", ".tex", ".org", ".adoc"
    };

    static std::string toLower(std::string str) {
      std::transform(str.begin(), str.end(), str.begin(),
        [](unsigned char c) { return std::tolower(c); });
      return str;
    }

    static bool isBinary(std::string_view text) {
      if (text.find('\0') != std::string_view::npos) {
        return true;
      }
      size_t nonPrintable = 0;
      size_t checked = (std::min)(text.size(), BINARY_CHECK_BYTES);
      for (size_t i = 0; i < checked; ++i) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 32 && c != '\n' && c != '\r' && c != '\t') {
          nonPrintable++;
        }
      }
      return 0 < checked && BINARY_THRESHOLD < (static_cast<double>(nonPrintable) / checked);
    }

    static bool hasMarkdownCodeBlocks(std::string_view text) {
      const auto &patterns = getPatterns();
      int fenceCount = 0;
      size_t pos = 0;
      size_t linesChecked = 0;
      const size_t M = 75;
      while (pos < text.size() && fenceCount < 2 && linesChecked < M) {
        size_t lineEnd = text.find('\n', pos);
        if (lineEnd == std::string_view::npos) lineEnd = text.size();
        std::string_view line = text.substr(pos, (std::min)(lineEnd - pos, MAX_LINE_BYTES_TO_SCAN));
        std::string lineStr(line); // Regex needs std::string
        try {
          if (std::regex_search(lineStr, patterns.markdownFence)) {
            fenceCount++;
          }
        } catch (const std::regex_error &) {
          // UTF-8 issues - bail out
          return false;
        }
        pos = lineEnd + 1;
        linesChecked++;
      }
      return fenceCount >= 2;
    }

    static bool tryRegexSearch(const std::string &line, const std::regex &pattern) {
      try {
        return std::regex_search(line, pattern);
      } catch (const std::regex_error &) {
        // Non-UTF8 or malformed input
        return false;
      }
    }

  public:
    static Chunker::ContentType detectContentType(const std::string &text, const std::string &uri) {
      std::string_view textView(text);
      if (isBinary(textView)) {
        return Chunker::ContentType::Binary;
      }
      std::string ext = std::filesystem::path(uri).extension().string();
      ext = toLower(ext);
      if (codeExtensions.count(ext)) {
        return Chunker::ContentType::Code;
      }
      if (textExtensions.count(ext)) {
        return Chunker::ContentType::Text;
      }
      const auto &patterns = getPatterns();
      size_t totalLines = 0;
      size_t nonEmptyLines = 0;
      size_t codeIndicators = 0;
      size_t indentedLines = 0;
      size_t linesWithSemicolons = 0;
      size_t linesWithBraces = 0;
      size_t pos = 0;
      while (pos < textView.size() && totalLines < MAX_LINES_TO_SCAN) {
        size_t lineEnd = textView.find('\n', pos);
        if (lineEnd == std::string_view::npos) lineEnd = textView.size();

        std::string_view lineView = textView.substr(pos, (std::min)(lineEnd - pos, MAX_LINE_BYTES_TO_SCAN));
        totalLines++;

        size_t firstNonWs = lineView.find_first_not_of(" \t\r\n");
        if (firstNonWs == std::string_view::npos) {
          pos = lineEnd + 1;
          continue;
        }
        nonEmptyLines++;

        // Check indentation
        if (firstNonWs > 0) {
          indentedLines++;
        }

        // Early exit if strong confidence reached
        if (codeIndicators >= EARLY_EXIT_THRESHOLD && nonEmptyLines >= 10) {
          return Chunker::ContentType::Code;
        }

        // Convert to string for regex (only if needed)
        std::string line(lineView);

        // Check patterns with UTF-8 error handling
        bool matched = false;
        matched = matched || tryRegexSearch(line, patterns.classStruct);
        matched = matched || tryRegexSearch(line, patterns.functionDef);
        matched = matched || tryRegexSearch(line, patterns.accessModifiers);
        matched = matched || tryRegexSearch(line, patterns.imports);
        matched = matched || tryRegexSearch(line, patterns.varDeclarations);
        matched = matched || tryRegexSearch(line, patterns.controlFlow);
        matched = matched || tryRegexSearch(line, patterns.arrowFunctions);
        matched = matched || tryRegexSearch(line, patterns.loneBraces);
        matched = matched || tryRegexSearch(line, patterns.comments);

        if (matched) {
          codeIndicators++;
        }

        // Structural indicators - single pass
        for (char c : lineView) {
          if (c == '{' || c == '}') {
            linesWithBraces++;
            break;
          }
        }
        for (char c : lineView) {
          if (c == ';') {
            linesWithSemicolons++;
            break;
          }
        }

        pos = lineEnd + 1;
      }

      // Handle edge cases
      if (nonEmptyLines < MIN_LINES_FOR_ANALYSIS) {
        return (textView.find('{') != std::string_view::npos ||
          textView.find("function") != std::string_view::npos ||
          textView.find("class ") != std::string_view::npos)
          ? Chunker::ContentType::Code : Chunker::ContentType::Text;
      }

      // Check for Markdown with code blocks
      if (hasMarkdownCodeBlocks(textView)) {
        return Chunker::ContentType::Text;
      }

      // Calculate ratios
      double codeRatio = static_cast<double>(codeIndicators) / nonEmptyLines;
      double braceRatio = static_cast<double>(linesWithBraces) / nonEmptyLines;
      double semicolonRatio = static_cast<double>(linesWithSemicolons) / nonEmptyLines;
      double indentRatio = static_cast<double>(indentedLines) / nonEmptyLines;

      // Strong code signals
      if (codeRatio > CODE_RATIO_STRONG ||
        (braceRatio > BRACE_RATIO_STRONG && codeIndicators > MIN_CODE_INDICATORS) ||
        (semicolonRatio > SEMICOLON_RATIO_STRONG && codeIndicators > MIN_CODE_INDICATORS) ||
        (codeIndicators > STRONG_CODE_INDICATORS && indentRatio > INDENT_RATIO_STRONG)) {
        return Chunker::ContentType::Code;
      }

      // Weak code signals
      if (codeRatio > CODE_RATIO_WEAK && indentRatio > INDENT_RATIO_THRESHOLD &&
        (braceRatio > BRACE_RATIO_WEAK || semicolonRatio > SEMICOLON_RATIO_WEAK)) {
        return Chunker::ContentType::Code;
      }

      return Chunker::ContentType::Text;
    }
  };

  std::vector<std::string> splitUnits(const std::string &text) {
    std::vector<std::string> result;
    std::string buf;
    bool lastWasSpace = false;
    auto flushBuf = [&]() {
      if (!buf.empty()) {
        result.push_back(buf);
        buf.clear();
      }
      };
    for (unsigned char c : text) {
      if (std::isspace(c)) {
        flushBuf();
        // group consecutive whitespace into one unit
        if (lastWasSpace) {
          result.back().push_back(c);
        } else {
          result.emplace_back(1, c);
        }
        lastWasSpace = true;
      } else if (std::ispunct(c)) {
        lastWasSpace = false;
        flushBuf();
        // punctuation = its own unit
        result.emplace_back(1, c);
      } else {
        lastWasSpace = false;
        buf.push_back(c);
      }
    }
    flushBuf();
    return result;
  }

  // Split only at UTF-8 boundaries and fail explicitly if one character cannot fit.
  template <typename TokenCounter>
  std::vector<std::string> hardSplitByBudget(const std::string &text, size_t maxTokens, TokenCounter tokenCounter) {
    std::vector<std::string> result;
    size_t pos = 0;
    while (pos < text.size()) {
      const auto remaining = text.size() - pos;
      size_t len = maxTokens > remaining / 4 ? remaining : maxTokens * 4;
      while (true) {
        while (len && pos + len < text.size() && (static_cast<unsigned char>(text[pos + len]) & 0xc0) == 0x80) --len;
        if (!len) {
          len = 1;
          while (pos + len < text.size() && (static_cast<unsigned char>(text[pos + len]) & 0xc0) == 0x80) ++len;
          if (tokenCounter(text.substr(pos, len)) > maxTokens)
            throw std::runtime_error("A source character exceeds the chunk token budget");
        }
        if (tokenCounter(text.substr(pos, len)) <= maxTokens) break;
        if (len == 1) throw std::runtime_error("A source character exceeds the chunk token budget");
        len /= 2;
      }
      result.push_back(text.substr(pos, len));
      pos += len;
    }
    return result;
  }

  template <typename TokenCounter>
  void appendBudgetUnits(const std::string &text, std::vector<Unit> &units, size_t &pos,
    size_t maxTokens, TokenCounter countUnit) {
    for (const auto &unit : splitUnits(text)) {
      const auto tokens = countUnit(unit);
      if (tokens <= maxTokens) {
        units.push_back({pos, pos + unit.size(), tokens});
        pos += unit.size();
        continue;
      }
      for (const auto &piece : hardSplitByBudget(unit, maxTokens, countUnit)) {
        units.push_back({pos, pos + piece.size(), countUnit(piece)});
        pos += piece.size();
      }
    }
  }

  template <typename TokenCounter>
  std::vector<Chunk> packUnits(const std::string &text, const std::string &uri,
    const std::vector<Unit> &units, size_t maxTokens, size_t overlap, TokenCounter tokenCounter) {
    std::vector<Chunk> chunks;
    const auto exact = [&](size_t start, size_t end) {
      return tokenCounter(text.substr(units[start].startChar, units[end - 1].endChar - units[start].startChar));
    };
    size_t start = 0;
    while (start < units.size()) {
      size_t end = start;
      size_t approximate = 0;
      while (end < units.size() && units[end].tokens <= maxTokens - approximate) {
        approximate += units[end].tokens;
        ++end;
      }
      size_t tokens = end > start ? exact(start, end) : 0;
      // Normalization and fragments of oversized words can make counts non-additive.
      while (end > start && tokens > maxTokens) {
        --end;
        tokens = end > start ? exact(start, end) : 0;
      }
      if (end == start) throw std::runtime_error("Unable to fit source text within the chunk token budget");
      const auto beginByte = units[start].startChar;
      const auto endByte = units[end - 1].endChar;
      chunks.push_back({uri, uri + "_" + std::to_string(chunks.size()), text.substr(beginByte, endByte - beginByte),
        {}, {tokens, beginByte, endByte, "char"}});
      if (end == units.size()) break;
      size_t next = end;
      size_t suffixTokens = 0;
      while (next > start + 1 && overlap > 0 && units[next - 1].tokens <= overlap - suffixTokens) {
        suffixTokens += units[next - 1].tokens;
        --next;
      }
      // Skip zero-token prefixes only within already covered overlap. Recheck
      // exact counts after trimming: normalization can make counts non-additive.
      while (next < end) {
        if (units[next].tokens != 0 && exact(next, end) <= overlap && exact(next, end + 1) <= maxTokens) break;
        ++next;
      }
      start = next;
    }
    return chunks;
  }

  struct MaskedSpan {
    size_t sourceStart, sourceEnd, maskedStart, maskedEnd;
  };

  struct MaskedText {
    std::string text;
    std::vector<MaskedSpan> spans;
    size_t sourceOffset(size_t offset) const {
      const auto next = std::upper_bound(spans.begin(), spans.end(), offset,
        [](size_t value, const MaskedSpan &span) { return value < span.maskedStart; });
      if (next == spans.begin()) return offset;
      const auto &span = *std::prev(next);
      if (offset == span.maskedStart) return span.sourceStart;
      if (offset < span.maskedEnd) throw std::logic_error("Chunk boundary splits an opaque-data marker");
      return span.sourceEnd + offset - span.maskedEnd;
    }
  };

  MaskedText maskDataUris(const std::string &source) {
    MaskedText result;
    size_t copied = 0;
    size_t cursor = 0;
    while ((cursor = source.find(':', cursor)) != std::string::npos) {
      const auto colon = cursor++;
      if (colon < 4) continue;
      const std::string_view scheme(source.data() + colon - 4, 4);
      if (!std::equal(scheme.begin(), scheme.end(), "data", [](unsigned char a, char b) {
        return std::tolower(a) == b;
      })) continue;
      if (colon > 4) {
        const auto previous = static_cast<unsigned char>(source[colon - 5]);
        if (std::isalnum(previous) || previous == '_') continue;
      }
      const auto headerView = std::string_view(source).substr(cursor, 257);
      const auto commaOffset = headerView.find(',');
      if (commaOffset == std::string_view::npos) continue;
      const auto comma = cursor + commaOffset;
      auto header = source.substr(cursor, comma - cursor);
      std::transform(header.begin(), header.end(), header.begin(), [](unsigned char c) { return std::tolower(c); });
      if (!header.ends_with(";base64") || header.find_first_of(" \t\r\n\"'<>") != std::string::npos) continue;
      const auto payloadStart = comma + 1;
      size_t end = payloadStart;
      while (end < source.size()) {
        const auto c = static_cast<unsigned char>(source[end]);
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=')) break;
        ++end;
      }
      cursor = end;
      const auto length = end - payloadStart;
      if (length < 100 || length % 4 != 0) continue;
      if (end < source.size() && std::string_view("\"')<> \t\r\n").find(source[end]) == std::string_view::npos) continue;
      const auto payload = std::string_view(source).substr(payloadStart, length);
      const auto padding = payload.find('=');
      if (padding != std::string_view::npos && (length - padding > 2 || payload.find_first_not_of('=', padding) != std::string_view::npos)) continue;
      result.text.append(source, copied, payloadStart - copied);
      const auto begin = result.text.size();
      result.text += "[" + std::to_string(length) + " encoded characters omitted]";
      result.spans.push_back({payloadStart, end, begin, result.text.size()});
      copied = end;
    }
    result.text.append(source, copied, source.size() - copied);
    return result;
  }

} // anonymous namespace


Chunker::Chunker(const SimpleTokenizer &tok, size_t min_tok, size_t max_tok, float overlap)
  : tokenizer_(tok), maxTokens_(max_tok), minTokens_(min_tok), overlapTokens_(0)
{
  if (!max_tok || !min_tok || min_tok > max_tok || !std::isfinite(overlap) || overlap < 0 || overlap >= 1)
    throw std::invalid_argument("Chunking requires 0 < min <= max tokens and overlap in [0, 1)");
  overlapTokens_ = static_cast<size_t>(static_cast<double>(max_tok) * (std::min)(overlap, 0.6f));
}

std::vector<Chunk> Chunker::chunkText(const std::string &text, const std::string &uri, bool semantic) const
{
  const auto masked = maskDataUris(text);
  std::vector<std::pair<size_t, size_t>> protectedRanges;
  for (const auto &span : masked.spans) protectedRanges.emplace_back(span.maskedStart, span.maskedEnd);
  std::vector<Chunk> chunks;
  if (semantic) {
    switch (detectContentType(masked.text, uri)) {
    case ContentType::Text:
      chunks = postProcessChunks(splitIntoTextChunks(masked.text, uri, protectedRanges), ContentType::Text);
      break;
    case ContentType::Code:
      chunks = postProcessChunks(splitIntoLineChunks(masked.text, uri, protectedRanges), ContentType::Code);
      break;
    default:
      LOG_MSG_WARN << "Unsupported content type for URI: " << uri << ". Skipped.";
      break;
    }
  } else {
    chunks = postProcessChunks(splitIntoTextChunks(masked.text, uri, protectedRanges), ContentType::Text);
  }
  for (auto &chunk : chunks) {
    chunk.metadata.start = masked.sourceOffset(chunk.metadata.start);
    chunk.metadata.end = masked.sourceOffset(chunk.metadata.end);
  }
  if (semantic && !chunks.empty() && chunks.front().metadata.type == "code") {
    std::vector<size_t> lineStarts{0};
    for (size_t i = 0; i < text.size(); ++i) if (text[i] == '\n') lineStarts.push_back(i + 1);
    for (auto &chunk : chunks) {
      const auto begin = std::lower_bound(lineStarts.begin(), lineStarts.end(), chunk.metadata.start);
      const auto end = std::lower_bound(lineStarts.begin(), lineStarts.end(), chunk.metadata.end);
      if (begin != lineStarts.end() && *begin == chunk.metadata.start &&
          (chunk.metadata.end == text.size() || (end != lineStarts.end() && *end == chunk.metadata.end))) {
        chunk.metadata.start = begin - lineStarts.begin();
        chunk.metadata.end = end - lineStarts.begin();
        chunk.metadata.unit = "line";
      }
    }
  }
  return chunks;
}

std::string Chunker::contentTypeToStr(Chunker::ContentType t)
{
  switch (t) {
  case Chunker::ContentType::Code: return "code";
  case Chunker::ContentType::Text: return "text";
  case Chunker::ContentType::Binary: return "binary";
  default: return "unknown";
  }
}

Chunker::ContentType Chunker::detectContentType(const std::string &text, const std::string &uri)
{
  return ContentTypeHelper::detectContentType(text, uri);
}

std::vector<Chunk> Chunker::postProcessChunks(const std::vector<Chunk> &chunks, ContentType chunkType) const
{
  std::vector<Chunk> processed;
  for (size_t i = 0; i < chunks.size(); ++i) {
    Chunk chunk = chunks[i];
    chunk.metadata.type = contentTypeToStr(chunkType);
    if (chunk.metadata.tokenCount < minTokens_ && i + 1 < chunks.size()) {
      const Chunk &nextChunk = chunks[i + 1];
      if (chunk.docUri == nextChunk.docUri && chunk.metadata.end == nextChunk.metadata.start) {
        const auto combined = chunk.text + nextChunk.text;
        const auto combinedTokens = tokenCount(combined);
        if (combinedTokens <= maxTokens_) {
          chunk.text = combined;
          chunk.metadata.tokenCount = combinedTokens;
          chunk.metadata.end = nextChunk.metadata.end;
          ++i;
        }
      }
    }
    processed.push_back(chunk);
  }
  return processed;
}

size_t Chunker::tokenCount(const std::string &text) const
{
  {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    const auto found = tokenCache_.find(text);
    if (found != tokenCache_.end()) return found->second;
  }
  const auto count = tokenizer_.countTokensWithVocab(text);
  if (text.size() <= 4096) {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    if (tokenCache_.size() >= 10000) tokenCache_.clear();
    tokenCache_[text] = count;
  }
  return count;
}

std::vector<Chunk> Chunker::splitIntoTextChunks(const std::string &text, const std::string &uri,
  const std::vector<std::pair<size_t, size_t>> &protectedRanges) const
{
  const auto countUnit = [this](const std::string &part) { return tokenizer_.countTokensWithVocab(part); };
  std::vector<Unit> units;
  size_t pos = 0;
  size_t range = 0;
  while (pos < text.size()) {
    if (range < protectedRanges.size() && pos == protectedRanges[range].first) {
      const auto end = protectedRanges[range++].second;
      const auto tokens = countUnit(text.substr(pos, end - pos));
      if (tokens > maxTokens_)
        throw std::runtime_error("The opaque-data marker exceeds the chunk token budget");
      units.push_back({pos, end, tokens});
      pos = end;
      continue;
    }
    const auto end = range < protectedRanges.size() ? protectedRanges[range].first : text.size();
    appendBudgetUnits(text.substr(pos, end - pos), units, pos, maxTokens_, countUnit);
  }
  return packUnits(text, uri, units, maxTokens_, overlapTokens_, [this](const std::string &part) { return tokenCount(part); });
}

std::vector<Chunk> Chunker::splitIntoLineChunks(const std::string &text, const std::string &uri,
  const std::vector<std::pair<size_t, size_t>> &protectedRanges) const
{
  std::vector<Unit> units;
  size_t pos = 0;
  size_t range = 0;
  while (pos < text.size()) {
    if (range < protectedRanges.size() && pos == protectedRanges[range].first) {
      const auto end = protectedRanges[range++].second;
      const auto tokens = tokenizer_.countTokensWithVocab(text.substr(pos, end - pos));
      if (tokens > maxTokens_)
        throw std::runtime_error("The opaque-data marker exceeds the chunk token budget");
      units.push_back({pos, end, tokens});
      pos = end;
      continue;
    }
    const auto newline = text.find('\n', pos);
    auto end = newline == std::string::npos ? text.size() : newline + 1;
    if (range < protectedRanges.size()) end = (std::min)(end, protectedRanges[range].first);
    const auto line = text.substr(pos, end - pos);
    for (const auto &piece : splitIntoLines(line)) {
      units.push_back({pos, pos + piece.size(), tokenCount(piece)});
      pos += piece.size();
    }
  }
  return packUnits(text, uri, units, maxTokens_, overlapTokens_, [this](const std::string &part) { return tokenCount(part); });
}

std::string Chunker::maskOpaqueBlobs(const std::string &text)
{
  return maskDataUris(text).text;
}

std::vector<std::string> Chunker::splitIntoLines(const std::string &text) const
{
  if (tokenCount(text) <= maxTokens_) return {text};
  std::vector<Unit> units;
  size_t pos = 0;
  appendBudgetUnits(text, units, pos, maxTokens_,
    [this](const std::string &part) { return tokenizer_.countTokensWithVocab(part); });
  auto chunks = packUnits(text, {}, units, maxTokens_, 0, [this](const std::string &part) { return tokenCount(part); });
  std::vector<std::string> result;
  result.reserve(chunks.size());
  for (auto &chunk : chunks) result.push_back(std::move(chunk.text));
  return result;
}
