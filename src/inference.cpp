#include "inference.h"
#include "app.h"
#include "cutils.h"
#include "database.h"
#include "settings.h"
#include "tokenizer.h"
#include <stdexcept>
#include <cassert>
#include <unordered_map>
#include <filesystem>
#include <cmath>  // for std::sqrt
#include <httplib.h>
#include <utils_log/logger.hpp>
#include "3rdparty/fmt/core.h"

namespace {

  bool jsonStrFromSSEEvent(const std::string &event, std::string &jsonStr) {
    size_t dataPos = event.find("data: ");
    if (dataPos != std::string::npos) {
      size_t lineStart = dataPos + 6; // length of "data: "
      size_t lineEnd = event.find('\n', lineStart);
      jsonStr = (lineEnd == std::string::npos)
        ? event.substr(lineStart)
        : event.substr(lineStart, lineEnd - lineStart);
      return true;
    }
    return false;
  }

  bool isAnthropic(const ApiConfig &cfg) {
    return cfg.apiStyle == ApiConfig::ApiStyle::Anthropic;
  }

  httplib::Headers buildHeaders(const ApiConfig &cfg, bool streaming) {
    httplib::Headers h = isAnthropic(cfg)
      ? httplib::Headers{ {"x-api-key", cfg.apiKey}, {"anthropic-version", "2023-06-01"}, {"Connection", "keep-alive"} }
    : httplib::Headers{ {"Authorization", "Bearer " + cfg.apiKey}, {"Connection", "keep-alive"} };
    h.insert({ "Accept", streaming ? "text/event-stream" : "application/json" });
    return h;
  }

  nlohmann::json buildRequestBody(const ApiConfig &cfg, const nlohmann::json &messages, float temperature, size_t maxTokens, bool skipMessagesIfEmpty = false) {
    nlohmann::json body;
    body["model"] = cfg.model;
    if (isAnthropic(cfg)) {
      std::string system;
      nlohmann::json rest = nlohmann::json::array();
      for (const auto &m : messages) {
        if (m.value("role", "") == "system") system += (system.empty() ? "" : "\n") + m.value("content", "");
        else rest.push_back(m);
      }
      if (!system.empty()) body["system"] = system;
      body["messages"] = rest;
      body["max_tokens"] = maxTokens; // Anthropic requires this, always
      if (cfg.temperatureSupport) body["temperature"] = temperature;
    } else {
      if (!skipMessagesIfEmpty && 0 != messages.size())
        body["messages"] = messages;
      if (cfg.temperatureSupport) body["temperature"] = temperature;
      body[cfg.maxTokensName] = maxTokens;
    }
    body["stream"] = cfg.stream;
    return body;
  }

  std::string extractDeltaContent(const ApiConfig &cfg, const nlohmann::json &chunk) {
    if (isAnthropic(cfg)) {
      if (chunk.value("type", "") == "content_block_delta") {
        const auto &delta = chunk["delta"];
        if (delta.value("type", "") == "text_delta") return delta.value("text", "");
      }
      return {};
    }
    if (chunk.contains("choices") && !chunk["choices"].empty()) {
      const auto &d = chunk["choices"][0].value("delta", nlohmann::json::object());
      if (d.contains("content") && !d["content"].is_null()) return d["content"].get<std::string>();
      // We do not propagate reasoning to the client. Maybe a future feature.
      // if (d.contains("reasoning_content") && !d["reasoning_content"].is_null()) return d["reasoning_content"].get<std::string>();
    }
    return {};
  }

  std::string extractFullContent(const ApiConfig &cfg, const nlohmann::json &res) {
    if (isAnthropic(cfg)) {
      if (res.contains("content") && res["content"].is_array() && !res["content"].empty())
        return res["content"][0].value("text", "");
      return {};
    }
    if (!res["choices"].empty()) {
      const auto &c = res["choices"][0];
      if (c.contains("message") && c["message"].contains("content")) return c["message"]["content"].get<std::string>();
    }
    return {};
  }
} // anonymous namespace


struct InferenceClient::Impl {
  ApiConfig apiCfg_;
  size_t timeoutMs_ = 1000;
  std::unordered_map<std::string, std::unique_ptr<httplib::Client>> urlToClient_;
  std::pair<httplib::Client *, std::string> httpClientForUrl(const std::string &url);
};

std::pair<httplib::Client *, std::string> InferenceClient::Impl::httpClientForUrl(const std::string &url)
{
  size_t protocolEnd = url.find("://");
  if (protocolEnd == std::string::npos) {
    throw std::runtime_error("Invalid server URL format");
  }
  size_t hostStart = protocolEnd + 3;
  size_t pathStart = url.find("/", hostStart);
  if (pathStart == std::string::npos) {
    pathStart = url.size();
  }
  auto schemaHostPort = url.substr(0, pathStart);
  auto path = url.substr(pathStart);
  auto it = urlToClient_.find(schemaHostPort);
  if (it != urlToClient_.end()) {
    return { it->second.get(), path };
  } else {
    try {
      auto client = std::make_unique<httplib::Client>(schemaHostPort);
      client->set_connection_timeout(0, timeoutMs_ * 1000);
      client->set_read_timeout(timeoutMs_ / 1000, (timeoutMs_ % 1000) * 1000);
      auto ptr = client.get();
      urlToClient_.emplace(schemaHostPort, std::move(client));
      return { ptr, path };
    } catch (const std::exception &e) {
      LOG_MSG << "Error initializing http client for" << schemaHostPort << ":" << e.what();
      return { nullptr, "" };
    }
  }
}

InferenceClient::InferenceClient(const ApiConfig &cfg, size_t timeout) : imp(new Impl)
{
  imp->apiCfg_ = cfg;
  imp->timeoutMs_ = timeout;
}

InferenceClient::~InferenceClient()
{
}

const ApiConfig &InferenceClient::cfg() const
{
  return imp->apiCfg_;
}

size_t InferenceClient::timeoutMs() const
{
  return imp->timeoutMs_;
}

//---------------------------------------------------------------------------


EmbeddingClient::EmbeddingClient(const ApiConfig &cfg, size_t timeout)
  : InferenceClient(cfg, timeout)
{
}

void EmbeddingClient::generateEmbeddings(const std::vector<std::string> &texts, std::vector<std::vector<float>> &embeddingsList, EmbeddingClient::EncodeType et) const
{
  embeddingsList.reserve(texts.size());
  try {
    const auto [httpClient, path] = imp->httpClientForUrl(cfg().apiUrl);
    if (!httpClient) {
      throw std::runtime_error("Failed to initialize http client");
    }

    nlohmann::json requestBody;
    requestBody["content"] = prepareContent(texts, et);
    std::string bodyStr = requestBody.dump();

    httplib::Headers headers = {
      {"Content-Type", "application/json"},
      {"Authorization", "Bearer " + cfg().apiKey},
      {"Connection", "keep-alive"}
    };
    auto res = httpClient->Post(path.c_str(), headers, bodyStr, "application/json");
    if (!res) {
      throw std::runtime_error("Failed to connect to embedding server");
    }
    if (res->status != 200) {
      throw std::runtime_error("Server returned error: " + std::to_string(res->status) + " - " + res->body);
    }
    nlohmann::json response = nlohmann::json::parse(res->body);
    if (!response.is_array() || response.size() != texts.size()) {
      throw std::runtime_error("Unexpected embedding response format");
    }
    for (size_t j = 0; j < texts.size(); j ++) {
      assert(j < response.size());
      if (response.size() <= j) {
        LOG_MSG << "Not enough entries in the embedding response (asked for" << texts.size() << "but got" << response.size() << "). Skipped";
        break;
      }
      const auto &item = response[j];
      if (!item.contains("embedding") || !item["embedding"].is_array()) {
        throw std::runtime_error("Missing or invalid 'embedding' field in response");
      }
      const auto &embeddingArray = item["embedding"];
      if (embeddingArray.empty() || !embeddingArray[0].is_array()) {
        throw std::runtime_error("Invalid embedding structure");
      }
      const auto &embeddingData = embeddingArray[0];
      std::vector<float> embedding;
      embedding.reserve(1024);
      for (const auto &value : embeddingData) {
        if (value.is_number()) {
          embedding.push_back(value.get<float>());
        } else {
          throw std::runtime_error("Non-numeric value in embedding data");
        }
      }
      embeddingsList.push_back(embedding);
    }
    //float l2Norm = calculateL2Norm(embedding);
    //std::cout << "[l2norm] " << l2Norm << std::endl;
  } catch (const nlohmann::json::exception &e) {
    LOG_MSG << "JSON parsing error:" << e.what();
    throw std::runtime_error("Failed to parse server response");
  } catch (const std::exception &e) {
    throw;
  }
}

void EmbeddingClient::generateEmbeddings(const std::string &text, std::vector<float> &embeddings, EmbeddingClient::EncodeType et) const
{
  std::vector<std::vector<float>> embs;
  generateEmbeddings({ text }, embs, et);
  if (!embs.empty()) embeddings = std::move(embs.front());
}

float EmbeddingClient::calculateL2Norm(const std::vector<float> &vec)
{
  float sum = 0.0f;
  for (float val : vec) {
    sum += val * val;
  }
  return std::sqrt(sum);
}

std::vector<std::string> EmbeddingClient::prepareContent(const std::vector<std::string> &texts, EmbeddingClient::EncodeType et) const
{
  std::vector<std::string> res{ texts };
  const auto &fmtDoc = imp->apiCfg_.documentFormat;
  const auto &fmtQry = imp->apiCfg_.queryFormat;
  switch (et) {
  case EmbeddingClient::EncodeType::Document:
    if (!fmtDoc.empty() && fmtDoc.find("{}") != std::string::npos) {
      for (auto &t : res) {
        t = fmt::vformat(fmtDoc, fmt::make_format_args(t));
      }
    }
    break;
  case EmbeddingClient::EncodeType::Query:
    if (!fmtQry.empty() && fmtQry.find("{}") != std::string::npos) {
      for (auto &t : res) {
        t = fmt::vformat(fmtQry, fmt::make_format_args(t));
      }
    }
    break;
  }
  return res;
}


//---------------------------------------------------------------------------


namespace {
  const std::string &_queryTemplate{ R"(
  You're a helpful software developer assistant, please use the provided context to base your answers on
  for user questions. Answer to the best of your knowledge. Keep your responses short and on point.
  Context:
  __CONTEXT__

  Question:
  __QUESTION__

  Answer:
  )" };

#if 0
  const std::string &_fimTemplate{ R"(
  You are a helpful coding assistant. When asked to fill the missing middle between a prefix and a suffix, 
  produce only the middle content - do not repeat the prefix or suffix, do not add explanation.
  PREFIX:
  __PREFIX__

  SUFFIX:
  __SUFFIX__
  )" };
#else
  const std::string _fimTemplate{ R"(
  [CONTEXT]
  __CONTEXT__

  [INSTRUCTION]
  Complete the code between the PREFIX and SUFFIX below. 
  Output ONLY the missing code. No markdown blocks, no explanations.

  [PREFIX]
  __PREFIX__
  [SUFFIX]
  __SUFFIX__

  [OUTPUT]
  )" };
#endif

  std::string processSSEData(const char *data, size_t len, std::function<void(const std::string &)> onStream) {
    std::string fullResponse;
    std::string buffer;
    buffer.append(data, len);
    size_t pos;
    while ((pos = buffer.find("\n\n")) != std::string::npos) {
      std::string event = buffer.substr(0, pos); // one SSE event
      buffer.erase(0, pos + 2);
      std::string jsonStr;
      if (jsonStrFromSSEEvent(event, jsonStr)) {
        std::string jsonStr = event.substr(6);
        if (jsonStr == "[DONE]") {
          break;
        }
        try {
          nlohmann::json chunkJson = nlohmann::json::parse(jsonStr);
          if (chunkJson.contains("choices") && !chunkJson["choices"].empty()) {
            const auto &choice = chunkJson["choices"][0];
            if (choice.contains("delta") && choice["delta"].contains("content")) {
              // Either choice["delta"]["content"] or choice["delta"]["reasoning_content"]
              std::string content;
              if (!choice["delta"]["content"].is_null())
                content = choice["delta"]["content"];
              else if (choice["delta"].contains("reasoning_content") && !choice["delta"]["reasoning_content"].is_null()) {
                // We do not propagate reasinging replies.
                //content = choice["delta"]["reasoning_content"];
              }
              fullResponse += content;
              if (onStream) {
                onStream(content);
              }
            }
          }
        } catch (const std::exception &e) {
          LOG_MSG << "Error parsing chunk" << e.what() << " in" << jsonStr;
        }
      }
    }
    if (buffer.find("Unauthorized") != std::string::npos) {
      if (onStream) onStream(buffer);
    }
    return fullResponse;
  }

} // anonymous namespace

CompletionClient::CompletionClient(const ApiConfig &cfg, size_t timeout, const App &a)
  : InferenceClient(cfg, timeout)
  , app_(a)
{
}

std::string CompletionClient::generateCompletion(
  const nlohmann::json &messagesJson,
  const std::vector<SearchResult> &searchRes,
  float temperature,
  size_t maxTokens,
  std::function<void(const std::string &)> onStream) const
{
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
  if (schemaHostPort().starts_with("https://")) {
    throw std::runtime_error("HTTPS not supported in this build");
  }
#endif
  const auto [httpClient, path] = imp->httpClientForUrl(cfg().apiUrl);
  if (!httpClient) {
    throw std::runtime_error("Failed to initialize http client");
  }

  /*
  * Json Request body format.
  {
    "model": "",
    "messages": [
      {"role": "system", "content": "Keep it short."},
      {"role": "user", "content": "What is the capital of France?"}
    ],
    "temperature": 0.7
   }
  */

  if (onStream) {
    onStream("[meta]Working on the response");
  }

  std::string context = buildContext(searchRes);

  std::string prompt = _queryTemplate;
  size_t pos = prompt.find("__CONTEXT__");
  assert(pos != std::string::npos);
  prompt.replace(pos, std::string("__CONTEXT__").length(), context);

  pos = prompt.find("__QUESTION__");
  assert(pos != std::string::npos);
  std::string question = messagesJson.back()["content"].get<std::string>();
  prompt.replace(pos, std::string("__QUESTION__").length(), question);

  // Assign propmt to the last messagesJson's content field
  nlohmann::json modifiedMessages = messagesJson;
  modifiedMessages.back()["content"] = prompt;

  //std::cout << "Full context: " << modifiedMessages.dump() << "\n";
  
  nlohmann::json requestBody = buildRequestBody(cfg(), modifiedMessages, temperature, maxTokens);
  httplib::Headers headers = buildHeaders(cfg(), cfg().stream);

  std::string fullResponse;
  httplib::Result res;

  if (cfg().stream) {
    auto requestStr = requestBody.dump();

    std::string buffer; // holds leftover partial data

    res = httpClient->Post(
      path.c_str(),
      headers,
      std::move(requestStr),
      "application/json",
      [&fullResponse, &onStream, &buffer, this](const char *data, size_t len) {
        // llama-server sends SSE format: "data: {...}\n\n"
        buffer.append(data, len);
        size_t pos;
        while ((pos = buffer.find("\n\n")) != std::string::npos) {
          std::string event = buffer.substr(0, pos); // one SSE event
          buffer.erase(0, pos + 2);
          std::string jsonStr;
          if (jsonStrFromSSEEvent(event, jsonStr)) {
            if (jsonStr == "[DONE]") {
              break;
            }
            try {
              nlohmann::json chunkJson = nlohmann::json::parse(jsonStr);
              std::string content = extractDeltaContent(cfg(), chunkJson);
              if (!content.empty()) {
                fullResponse += content;
                if (onStream) onStream(content);
              }
            } catch (const std::exception &e) {
              LOG_MSG << "Error parsing chunk" << e.what() << "in" << jsonStr;
            }
          }
        }
        if (utils::strFindIn(buffer, "unauthorized", false) != std::string::npos 
          || utils::strFindIn(buffer, "invalid", false) != std::string::npos
          || utils::strFindIn(buffer, "error", false) != std::string::npos
          ) {
          if (onStream) onStream(buffer);
        }
        return true; // Continue receiving
      }
    );

  } else {
    res = httpClient->Post(
      path.c_str(),
      headers,
      requestBody.dump(),
      "application/json"
    );

    if (res && res->status == 200) {
      try {
        nlohmann::json jsonRes = nlohmann::json::parse(res->body);
        fullResponse = extractFullContent(cfg(), jsonRes);
        if (onStream) onStream(fullResponse);
      } catch (...) { /* ignore parse errors */ }
    }
  }

  if (!res) {
    throw std::runtime_error("Failed to connect to completion server");
  }

  if (res->status != 200) {
    std::string msg = fmt::format("Server returned error: {} - {}", res->status, res->body);
    if (onStream) onStream(msg);
    throw std::runtime_error(msg);
  }

  return fullResponse;
}

std::string CompletionClient::generateFim(
  const std::string &prefix, 
  const std::string &suffix, 
  const std::vector<std::string> &stops,
  float temperature, 
  size_t maxTokens,
  const std::vector<SearchResult> &searchRes) const
{
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
  if (schemaHostPort().starts_with("https://")) {
    throw std::runtime_error("HTTPS not supported in this build");
  }
#endif
  const auto &cfg = this->cfg();
  std::string fimPrefixName = utils::trimmed(cfg.fim.prefixName);
  std::string fimSuffixName = utils::trimmed(cfg.fim.suffixName);
  std::string fimFormat = utils::trimmed(cfg.fim.format);
  std::vector<std::string> fimStopTokens = cfg.fim.stopTokens;
  const bool bFimMode = !fimPrefixName.empty() || !fimFormat.empty();
  std::string apiUrl = bFimMode ? cfg.fim.apiUrl : cfg.apiUrl;

  fimStopTokens.insert(fimStopTokens.end(), stops.begin(), stops.end());

  const auto [httpClient, path] = imp->httpClientForUrl(apiUrl);
  if (!httpClient) {
    throw std::runtime_error("Failed to initialize http client");
  }

  std::string context = buildContext(searchRes, true, cfg.fim.fileDivider);

  bool genericFim = false;
  nlohmann::json messages = nlohmann::json::array();
  if (fimPrefixName.empty() && fimFormat.empty()) {
    auto prompt = _fimTemplate;
    size_t pos = prompt.find("__CONTEXT__");
    assert(pos != std::string::npos);
    prompt.replace(pos, std::string("__CONTEXT__").length(), context);
    pos = prompt.find("__PREFIX__");
    assert(pos != std::string::npos);
    prompt.replace(pos, std::string("__PREFIX__").length(), prefix);
    pos = prompt.find("__SUFFIX__");
    assert(pos != std::string::npos);
    prompt.replace(pos, std::string("__SUFFIX__").length(), suffix);
    messages.push_back({
      {"role", "system"},
      {"content", "You are a code completion assistant."}
      });
    messages.push_back({ {"role", "user"}, {"content", prompt} });
    genericFim = true;
  }

  nlohmann::json requestBody = buildRequestBody(cfg, messages, temperature, maxTokens, true);

  if (!genericFim) {
    if (!fimPrefixName.empty()) {
      requestBody[fimPrefixName] = context + "\n\n" + prefix;
      requestBody[fimSuffixName] = suffix;
      if (!fimStopTokens.empty())
        requestBody["stop"] = fimStopTokens;
    } else if (!fimFormat.empty() && fimFormat.find("{}") != std::string::npos) {
      auto prompt = fmt::vformat(fimFormat, fmt::make_format_args(prefix, suffix));
      requestBody["prompt"] = prompt;
      if (!fimStopTokens.empty())
        requestBody["stop"] = fimStopTokens;
    } else {
      assert(!"Should not be here.");
    }
  }
  httplib::Headers headers = buildHeaders(cfg, false);

  std::string fullResponse;
  httplib::Result res;

  std::string requestStr = requestBody.dump();

  res = httpClient->Post(
    path.c_str(),
    headers,
    requestStr,
    "application/json"
  );

  if (res && res->status == 200) {
    try {
      nlohmann::json jsonRes = nlohmann::json::parse(res->body);
      //LOG_MSG << "FIM Response:" << res->body;
      if (!jsonRes["choices"].empty()) {
        const auto &choice = jsonRes["choices"][0];
        if (choice.contains("message") && choice["message"].contains("content")) {
          fullResponse = choice["message"]["content"];
        } else if (choice.contains("text")) {
          fullResponse = choice["text"];
        }
      }
    } catch (...) {
      try {
        // fallback, assuming the response is SSE stream
        fullResponse = processSSEData(res->body.c_str(), res->body.length(), nullptr);
      } catch (const std::exception &ex) {
        LOG_MSG << "Error processing response:" << ex.what();
      }
    }
    if (!fullResponse.empty()) {
      fullResponse = utils::stripMarkdownFromCodeBlock(fullResponse);
      for (const auto &stopToken : fimStopTokens) {
        if (fullResponse.ends_with(stopToken)) {
          fullResponse.erase(fullResponse.size() - stopToken.size());
          break;
        }
      }
    }
  }

  if (!res) {
    throw std::runtime_error("Failed to connect to completion server");
  }

  if (res->status != 200) {
    std::string msg = fmt::format("Server returned error: {} - {}", res->status, res->body);
    throw std::runtime_error(msg);
  }
  return fullResponse;
}

std::string CompletionClient::buildContext(const std::vector<SearchResult> &searchRes, bool commentOut, const std::string &fileDivider) const
{
  const auto labelFmt = app_.settings().generationPrependLabelFormat();
  const auto maxContextTokens = cfg().contextLength;
  size_t nofTokens = app_.tokenizer().countTokensWithVocab(_queryTemplate);
  std::string context;
  for (const auto &r : searchRes) {
    std::string filename = std::filesystem::path(r.sourceId).filename().string();
    if (filename.empty()) filename = r.sourceId.empty() ? "source" : r.sourceId;
    std::string label = fmt::vformat(labelFmt, fmt::make_format_args(filename));
    // avoid double-labeling
    bool alreadyLabeled = (r.content.rfind(label, 0) == 0);

    size_t contentTokens = app_.tokenizer().countTokensWithVocab(r.content);
    size_t labelTokens = alreadyLabeled ? 0 : app_.tokenizer().countTokensWithVocab(label);

    if (maxContextTokens < nofTokens + labelTokens + contentTokens) {
      size_t remaining = (nofTokens < maxContextTokens) ? (maxContextTokens - nofTokens) : 0;
      if (remaining <= labelTokens) {
        // can't fit label + content excerpt -> stop
        break;
      }
      size_t remainingContentTokens = remaining - labelTokens;
      if (remainingContentTokens == 0) break;

      // approximate characters for remainingContentTokens
      size_t approxCharCount = r.content.length();
      if (0 < contentTokens) {
        approxCharCount = r.content.length() * remainingContentTokens / contentTokens;
      }

      std::string excerpt = r.content.substr(0, approxCharCount);

      std::string labeledExcerpt = alreadyLabeled ? excerpt : (label + excerpt);
      if (commentOut) 
        labeledExcerpt = utils::addLineComments(labeledExcerpt, filename);
      context += fileDivider + labeledExcerpt + "\n\n";
      nofTokens += app_.tokenizer().countTokensWithVocab(labeledExcerpt);
      break;
    }
    // full add
    std::string labeledFull = alreadyLabeled ? r.content : (label + r.content);
    if (commentOut) 
      labeledFull = utils::addLineComments(labeledFull, filename);
    nofTokens += labelTokens + contentTokens;
    context += fileDivider + labeledFull + "\n\n";
  }
  LOG_MSG << "[context] Total tokens in context:" << nofTokens;
  return context;
}
