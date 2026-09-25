#include "chunker.h"
#include <sstream>
#include <algorithm>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utils_log/logger.hpp>

namespace {
  struct Unit {
    std::string text;
    size_t tokens;
    size_t startChar;
    size_t endChar;
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
        std::string_view line = text.substr(pos, lineEnd - pos);
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

        std::string_view lineView = textView.substr(pos, lineEnd - pos);
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
        if (!result.empty() && std::all_of(result.back().begin(), result.back().end(),
          [](unsigned char x) { return std::isspace(x); })) {
          result.back().push_back(c);
        } else {
          result.emplace_back(1, c);
        }
      } else if (std::ispunct(c)) {
        flushBuf();
        // punctuation = its own unit
        result.emplace_back(1, c);
      } else {
        buf.push_back(c);
      }
    }
    flushBuf();
    return result;
  }

  // Hard-splits text into pieces each within maxTokens, using tokenCounter to measure. Always makes progress and consumes the whole string.
  template <typename TokenCounter>
  std::vector<std::string> hardSplitByBudget(const std::string &text, size_t maxTokens, TokenCounter tokenCounter) {
    std::vector<std::string> result;
    size_t pos = 0;
    while (pos < text.size()) {
      size_t len = text.size() - pos;
      std::string candidate = text.substr(pos, len);
      size_t tks = tokenCounter(candidate);
      while (maxTokens < tks && 1 < len) {
        // shrink with margin; guaranteed to strictly decrease len
        len = std::max<size_t>(1, static_cast<size_t>(len * (static_cast<double>(maxTokens) / tks) * 0.9));
        candidate = text.substr(pos, len);
        tks = tokenCounter(candidate);
      }
      result.push_back(candidate);
      pos += candidate.size(); // guaranteed >= 1, so outer loop always progresses
    }
    return result;
  }

  static bool looksLikeOpaqueBlob(const std::string &s) {
    if (s.size() < 100) return false;
    size_t digits = 0, upper = 0, lower = 0;
    for (unsigned char c : s) {
      if (std::isdigit(c)) digits++;
      else if (std::isupper(c)) upper++;
      else if (std::islower(c)) lower++;
    }
    // real identifiers rarely have this much digit density mixed with both cases
    return digits > s.size() / 10 && upper > 0 && lower > 0;
  }

} // anonymous namespace


Chunker::Chunker(const SimpleTokenizer &tok, size_t min_tok, size_t max_tok, float overlap)
  : tokenizer_(tok), minTokens_(min_tok), maxTokens_(max_tok), overlapTokens_(static_cast<size_t>(max_tok * overlap))
{
}

std::vector<Chunk> Chunker::chunkText(const std::string &text, const std::string &uri, bool semantic) const
{
  std::vector<Chunk> chunks;
  if (semantic) {
    switch (detectContentType(text, uri)) {
    case ContentType::Text:
      chunks = postProcessChunks(splitIntoTextChunks(text, uri), ContentType::Text);
      break;
    case ContentType::Code:
      chunks = postProcessChunks(splitIntoLineChunks(text, uri), ContentType::Code);
      break;
    default:
      LOG_MSG << "Unsupported content type for URI: " << uri << ". Skipped.";
      break;
    }
  } else {
    chunks = postProcessChunks(splitIntoTextChunks(text, uri), ContentType::Text);
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
      Chunk nextChunk = chunks[i + 1];
      size_t combined_tokens = tokenCount(chunk.text + nextChunk.text);
      if (combined_tokens <= maxTokens_ && chunk.docUri == nextChunk.docUri) {
        chunk.text += nextChunk.text;
        chunk.metadata.tokenCount = combined_tokens;
        chunk.metadata.end = nextChunk.metadata.end;
        ++i;
      }
    }
    processed.push_back(chunk);
  }
  return processed;
}

size_t Chunker::tokenCount(const std::string &text) const
{
  std::lock_guard<std::mutex> lock(cacheMutex_);
  auto it = tokenCache_.find(text);
  if (it != tokenCache_.end()) {
    return it->second;
  }
  size_t count = tokenizer_.countTokensWithVocab(text);
  if (10'000 < tokenCache_.size()) {
    // TODO: more robust cleanup (freq based, etc)
    tokenCache_.clear();
  }
  tokenCache_[text] = count;
  return count;
}

std::vector<Chunk> Chunker::splitIntoTextChunks(std::string text, const std::string &uri) const
{
  auto overlap = overlapTokens_;
  if (maxTokens_ * 0.6 < overlap) overlap = static_cast<size_t>(maxTokens_ * 0.6);
  text = normalizeWhitespaces(text);
  text = maskOpaqueBlobs(text);
  auto rawUnits = splitUnits(text);
  std::vector<Unit> units;
  size_t charPos = 0;
  for (auto &uText : rawUnits) {
    size_t tks = tokenCount(uText);
    if (maxTokens_ < tks) {
      auto pieces = hardSplitByBudget(uText, maxTokens_, [this](const std::string &s) { return tokenCount(s); });
      for (auto &p : pieces) {
        size_t pTks = tokenCount(p);
        units.push_back({ p, pTks, charPos, charPos + p.size() });
        charPos += p.size();
      }
      continue;
    }
    units.push_back({ uText, tks, charPos, charPos + uText.size() });
    charPos += uText.size();
  }
  std::vector<Chunk> chunks;
  size_t chunkId = 0;
  size_t start = 0;
  while (start < units.size()) {
    size_t tokenCnt = 0;
    size_t end = start;
    while (end < units.size() &&
      tokenCnt + units[end].tokens <= maxTokens_) {
      tokenCnt += units[end].tokens;
      end++;
    }
    if (start < end) {
      size_t startChar = units[start].startChar;
      size_t endChar = units[end - 1].endChar;
      std::string raw;
#ifdef _DEBUG
      raw = text.substr(startChar, endChar - startChar);
      size_t tokensToCheck = tokenCount(raw);
#endif
      std::string chunkText;
      for (size_t i = start; i < end; i++) chunkText += units[i].text;
      chunks.push_back({
          uri,
          uri + "_" + std::to_string(chunkId++),
          chunkText,
          raw,
          {tokenCnt, startChar, endChar, "char"}
        });
    }
    if (units.size() <= end) break;
    if (0 < overlap) {
      size_t overlapTokens = 0;
      size_t overlapUnits = 0;
      while (start + overlapUnits + 1 < end && overlapTokens < overlap) {
        overlapTokens += units[end - 1 - overlapUnits].tokens;
        overlapUnits++;
      }
      start = end - overlapUnits;
    } else {
      start = end;
    }
  }
  return chunks;
}

std::vector<Chunk> Chunker::splitIntoLineChunks(const std::string &text, const std::string &uri) const
{
  std::vector<std::string> lines;
  lines.reserve(100);
  {
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
      auto subLines = splitIntoLines(line); // split into more lines if too wide
      lines.insert(lines.end(),
        std::make_move_iterator(subLines.begin()),
        std::make_move_iterator(subLines.end()));
    }
  }
  if (lines.empty()) return {};
  std::vector<Chunk> chunks;
  size_t chunkId = 0;
  size_t start = 0;
  while (start < lines.size()) {
    size_t tokenCnt = 0;
    size_t end = start;
    std::string chunkText;
    chunkText.reserve(maxTokens_ * 4);
    // Accumulate lines until token budget exceeded
    while (end < lines.size()) {
      auto lineTokens = tokenCount(lines[end]);
      if (tokenCnt + lineTokens > maxTokens_) break;
      tokenCnt += lineTokens;
      chunkText += lines[end];
      end++;
    }
    if (start < end) {
      std::string raw;
#ifdef _DEBUG
      for (size_t i = start; i < end; i++) raw += lines[i];
#endif
      chunks.push_back({
          uri,
          uri + "_" + std::to_string(chunkId++),
          chunkText,
          raw,
          {tokenCnt, start, end, "line"}
        });
    }
    if (lines.size() <= end) break;
    if (0 < overlapTokens_) {
      size_t overlapTokens = 0;
      size_t overlapLines = 0;
      while (start < end - overlapLines - 1) {
        overlapTokens += tokenCount(lines[end - 1 - overlapLines]);
        if (overlapTokens < overlapTokens_) overlapLines++;
        else break;
      }
      start = end - overlapLines;
    } else {
      start = end;
    }
  }
  return chunks;
}

std::vector<Chunk> Chunker::splitIntoSemanticChunks(const std::string &text, const std::string &docId) const
{
  std::vector<Chunk> chunks;

  return chunks;
}

std::string Chunker::normalizeWhitespaces(const std::string &str)
{
  auto start = str.find_first_not_of(" \t\r\n");
  auto end = str.find_last_not_of(" \t\r\n");
  std::string s = (start == std::string::npos) ? "" : str.substr(start, end - start + 1);
  // collapse whitespace except newlines into single space
  s = std::regex_replace(s, std::regex{ "[^\\S\n]+" }, " ");
  // collapse multiple newlines
  s = std::regex_replace(s, std::regex{ "\n\\s*\n" }, "\n");
  return s;
}


std::string Chunker::maskOpaqueBlobs(const std::string &text)
{
  static const std::regex base64Pattern(R"([A-Za-z0-9+/]{100,}={0,2})", std::regex_constants::optimize);
  std::string result;
  result.reserve(text.size());
  auto begin = std::sregex_iterator(text.begin(), text.end(), base64Pattern);
  auto end = std::sregex_iterator();
  size_t lastPos = 0;
  for (auto it = begin; it != end; ++it) {
    const auto &m = *it;
    std::string candidate = m.str();
    if (!looksLikeOpaqueBlob(candidate)) continue; // skip false positives (long plain words)
    result.append(text, lastPos, m.position() - lastPos);
    result += "[opaque data, " + std::to_string(candidate.size()) + " bytes]";
    lastPos = m.position() + m.length();
  }
  result.append(text, lastPos, text.size() - lastPos);
  return result;
}

std::vector<std::string> Chunker::splitIntoLines(const std::string &text) const
{
  auto nTokens = tokenCount(text);
  if (nTokens <= maxTokens_) {
    auto s = text;
    if (!s.ends_with('\n')) s += '\n';
    return { s };
  }
  // Line too long - split by words/punctuation
  auto textUnmasked = maskOpaqueBlobs(text);
  auto units = splitUnits(textUnmasked);
  std::vector<std::string> result;
  std::string current;
  current.reserve(maxTokens_ * 4);
  size_t currentTokens = 0;
  for (const auto &u : units) {
    size_t uTokens = tokenCount(u);
    if (maxTokens_ < currentTokens + uTokens && !current.empty()) {
      if (!current.ends_with('\n')) current += '\n';
      result.push_back(std::move(current));
      current.clear();
      currentTokens = 0;
    }
    current += u;
    currentTokens += uTokens;
  }
  if (!current.empty()) {
    if (!current.ends_with('\n')) current += '\n';
    result.push_back(std::move(current));
  }
  return result;
}
