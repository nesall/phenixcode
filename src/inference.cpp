#include "inference.h"
#include "app.h"
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
      LOG_MSG << "Error initializing http client for " << schemaHostPort << ": " << e.what();
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
        LOG_MSG << "Not enough entries in the embedding response (asked for" << texts.size() << " but got" << response.size() << "). Skipped";
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
    LOG_MSG << "JSON parsing error: " << e.what();
    throw std::runtime_error("Failed to parse server response");
  } catch (const std::exception &e) {
    //LOG_MSG << "Error generating embeddings: " << e.what();
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

  const std::string &_fimTemplate{ R"(
    You are a helpful coding assistant. When asked to fill the missing middle between a prefix and a suffix, 
    produce only the middle content - do not repeat the prefix or suffix, do not add explanation.
    Prefix:
    __PREFIX__

    Suffix:
    __SUFFIX__
    )" };

  std::string processSSEData(const char *data, size_t len, std::function<void(const std::string &)> onStream) {
    std::string fullResponse;
    std::string buffer;
    buffer.append(data, len);
    size_t pos;
    while ((pos = buffer.find("\n\n")) != std::string::npos) {
      std::string event = buffer.substr(0, pos); // one SSE event
      buffer.erase(0, pos + 2);
      if (event.find("data: ", 0) == 0) {
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
              else if (choice["delta"].contains("reasoning_content") && !choice["delta"]["reasoning_content"].is_null())
                content = choice["delta"]["reasoning_content"];
              fullResponse += content;
              if (onStream) {
                onStream(content);
              }
            }
          }
        } catch (const std::exception &e) {
          LOG_MSG << "Error parsing chunk: " << e.what() << " in: " << jsonStr;
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


  const auto labelFmt = app_.settings().generationPrependLabelFormat();
  const auto maxContextTokens = cfg().contextLength;
  size_t nofTokens = app_.tokenizer().countTokensWithVocab(_queryTemplate);
  std::string context;
  for (const auto &r : searchRes) {
    std::string filename = std::filesystem::path(r.sourceId).filename().string();
    if (filename.empty()) filename = r.sourceId.empty() ? "source" : r.sourceId;
    // format label (fmt::vformat requires C++20)
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
      context += labeledExcerpt + "\n\n";
      nofTokens += app_.tokenizer().countTokensWithVocab(labeledExcerpt);
      break;
    }
    // full add
    std::string labeledFull = alreadyLabeled ? r.content : (label + r.content);
    nofTokens += labelTokens + contentTokens;
    context += labeledFull + "\n\n";
  }

  //std::cout << "Generating completions with context length of " << nofTokens << " tokens \n";

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

  nlohmann::json requestBody;
  requestBody["model"] = cfg().model;
  requestBody["messages"] = modifiedMessages;
  if (cfg().temperatureSupport)
    requestBody["temperature"] = temperature;
  requestBody[cfg().maxTokensName] = maxTokens;
  requestBody["stream"] = cfg().stream;

  httplib::Headers headers = {
    {"Authorization", "Bearer " + cfg().apiKey},
    {"Connection", "keep-alive"}
  };

  std::string fullResponse;
  httplib::Result res;

  if (cfg().stream) {
    headers.insert({ "Accept", "text/event-stream" });

    std::string buffer; // holds leftover partial data

    res = httpClient->Post(
      path.c_str(),
      headers,
      requestBody.dump(),
      "application/json",
      [&fullResponse, &onStream, &buffer](const char *data, size_t len) {
        // llama-server sends SSE format: "data: {...}\n\n"
        buffer.append(data, len);
        size_t pos;
        while ((pos = buffer.find("\n\n")) != std::string::npos) {
          std::string event = buffer.substr(0, pos); // one SSE event
          buffer.erase(0, pos + 2);
          if (event.find("data: ", 0) == 0) {
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
                  else if (choice["delta"].contains("reasoning_content") && !choice["delta"]["reasoning_content"].is_null())
                    content = choice["delta"]["reasoning_content"];
                  fullResponse += content;
                  if (onStream) {
                    onStream(content);
                  }
                }
              }
            } catch (const std::exception &e) {
              LOG_MSG << "Error parsing chunk: " << e.what() << " in: " << jsonStr;
            }
          }
        }
        if (buffer.find("Unauthorized") != std::string::npos) {
          if (onStream) onStream(buffer);
        }
        return true; // Continue receiving
      }
    );

  } else {
    headers.insert({ "Accept", "application/json" });

    res = httpClient->Post(
      path.c_str(),
      headers,
      requestBody.dump(),
      "application/json"
    );

    if (res && res->status == 200) {
      try {
        nlohmann::json jsonRes = nlohmann::json::parse(res->body);
        if (!jsonRes["choices"].empty()) {
          const auto &choice = jsonRes["choices"][0];
          if (choice.contains("message") && choice["message"].contains("content")) {
            fullResponse = choice["message"]["content"];
            if (onStream) onStream(fullResponse); // optional callback for consistency
          }
        }
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
  float temperature, 
  size_t maxTokens) const
{
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
  if (schemaHostPort().starts_with("https://")) {
    throw std::runtime_error("HTTPS not supported in this build");
  }
#endif

  std::string fimPrefixName = utils::trimmed(cfg().fim.prefixName);
  std::string fimSuffixName = utils::trimmed(cfg().fim.suffixName);
  std::vector<std::string> fimStopTokens = cfg().fim.stopTokens;
  std::string apiUrl = fimPrefixName.empty() ? cfg().apiUrl : cfg().fim.apiUrl;

  const auto [httpClient, path] = imp->httpClientForUrl(apiUrl);
  if (!httpClient) {
    throw std::runtime_error("Failed to initialize http client");
  }

  nlohmann::json requestBody;
  requestBody["model"] = cfg().model;

  if (!fimPrefixName.empty()) {
    requestBody[fimPrefixName] = prefix;
    requestBody[fimSuffixName] = suffix;
    if (!fimStopTokens.empty())
      requestBody["stop"] = fimStopTokens;
  } else {
    std::string prompt = _fimTemplate;
    size_t pos = prompt.find("__PREFIX__");
    assert(pos != std::string::npos);
    prompt.replace(pos, std::string("__PREFIX__").length(), prefix);
    pos = prompt.find("__SUFFIX__");
    assert(pos != std::string::npos);
    prompt.replace(pos, std::string("__SUFFIX__").length(), suffix);
    nlohmann::json messages = nlohmann::json::array();
    messages.push_back({ {"role", "user"}, {"content", prompt} });
    requestBody["messages"] = messages;
    requestBody["stream"] = false;
  }

  if (cfg().temperatureSupport)
    requestBody["temperature"] = temperature;
  requestBody[cfg().maxTokensName] = maxTokens;

  httplib::Headers headers = {
    {"Authorization", "Bearer " + cfg().apiKey},
    {"Connection", "keep-alive"},
    {"Accept", "application/json"}
  };

  std::string fullResponse;
  httplib::Result res;

  res = httpClient->Post(
    path.c_str(),
    headers,
    requestBody.dump(),
    "application/json"
  );

  if (res && res->status == 200) {
    try {
      nlohmann::json jsonRes = nlohmann::json::parse(res->body);
      if (!jsonRes["choices"].empty()) {
        const auto &choice = jsonRes["choices"][0];
        if (choice.contains("message") && choice["message"].contains("content")) {
          fullResponse = choice["message"]["content"];
        }
      }
    } catch (...) {
      try {
        // fallback, assuming the response is SSE stream
        fullResponse = processSSEData(res->body.c_str(), res->body.length(), nullptr);
      } catch (const std::exception &ex) {
        LOG_MSG << "Error processing response: " << ex.what();
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
