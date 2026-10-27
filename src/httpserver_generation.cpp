#include "httpserver.h"
#include "httpserver_internal.h"
#include "app.h"
#include "context_builder.h"
#include "model_router.h"
#include "database.h"
#include "inference.h"
#include "settings.h"
#include "tokenizer.h"
#include "auth.h"
#include <3rdparty/base64.h>
#include <3rdparty/fmt/format.h>
#include <3rdparty/nlohmann/json.hpp>
#include "monitor.h"
#include <httplib.h>
#include <utils_log/logger.hpp>
#include <chrono>
#include <exception>
#include <algorithm>
#include <set>
#include <vector>
#include <string>
#include <atomic>
#include <functional>
#include <string_view>
#include <filesystem>
#include <utility>

using json = nlohmann::json;

using namespace httpserver_detail;
using namespace context;
using model_router::getTargetApi;

namespace {
  template <class T>
  bool vecContains(const std::vector<T> &vec, const T &t) {
    return std::find(vec.begin(), vec.end(), t) != vec.end();
  }

  template <class T>
  bool vecAddIfUnique(std::vector<T> &vec, const T &t) {
    if (!vecContains(vec, t)) {
      vec.push_back(t);
      return true;
    }
    return false;
  }

  template <class T>
  bool vecAddIfUnique(std::vector<T> &vec, const std::vector<T> &t) {
    bool a = false;
    for (const auto &item : t) {
      if (vecAddIfUnique(vec, item)) a = true;
    }
    return a;
  }

}

void HttpServer::registerGenerationRoutes(AdminAuth &auth)
{
  auto &server = imp->server_;
  server.Post("/api/chat", [this](const httplib::Request &req, httplib::Response &res) {
    const auto start = std::chrono::steady_clock::now();
    try {
      LOG_MSG << "POST /api/chat";

      // format for messages field in request
      /*
      {
        "messages": [
          {"role": "system", "content": "Keep it short."},
          {"role": "user", "content": "What is the capital of France?"}
        ],
        "sourceids": [
          "../embedder_cpp/src/main.cpp", "../embedder_cpp/include/settings.h"
        ],
        "attachments": [
          { "filename": "filename1.cpp", "content": "..text file content 1.."},
          { "filename": "filename2.cpp", "content": "..text file content 2.."},
        ],
        "temperature": 0.2,
        "max_tokens": 800,
        "targetapi": "xai",
        "ctxratio": 0.5,
        "attachedonly": false
      }
      */
      json request = json::parse(req.body);
      if (!request.contains("messages") || !request["messages"].is_array() || request["messages"].empty()) {
        throw std::invalid_argument("'messages' field required and must be non-empty array");
      }
      const auto messagesJson = request["messages"];
      if (0 == messagesJson.size()) {
        throw std::invalid_argument("'messages' array must be non-empty");
      }
      if (!messagesJson.back().contains("role") || !messagesJson.back().contains("content")) {
        throw std::invalid_argument("Last message must have 'role' and 'content' fields");
      }

      std::string role = messagesJson.back()["role"];
      if (role != "user") {
        throw std::invalid_argument("Last message role must be 'user', got: " + role);
      }
      std::string question = messagesJson.back()["content"].get<std::string>();


      std::vector<std::string> pendingInfoVec;
      ApiConfig apiConfig = model_router::selectApi(request, messagesJson, question, imp->app_, pendingInfoVec);
      LOG_MSG << "[" << apiConfig.model << "]" << apiConfig.apiUrl;

      auto attachmentsJson = request["attachments"];
      auto attachments = parseAttachments(attachmentsJson);

      std::vector<std::string> sources;
      if (request.contains("sourceids")) {
        auto sourceidsJson = request["sourceids"];
        for (const auto &sid : sourceidsJson) {
          if (sid.is_string()) {
            vecAddIfUnique(sources, sid.get<std::string>());
          }
        }
      }

      float temperature = 0;
      if (apiConfig.temperatureSupport) {
        if (request.contains("temperature") && request["temperature"].is_number_float()) {
          temperature = request["temperature"];
          LOG_MSG << "Using request's temperature limit" << temperature;
        } else if (0.0f <= apiConfig.temperature && apiConfig.temperature <= 2.0f) {
          temperature = apiConfig.temperature;
          LOG_MSG << "Using model's temperature limit" << temperature;
        } else {
          temperature = imp->app_.settings().generationDefaultTemperature();
          LOG_MSG << "Using default temperature limit" << temperature;
        }
      }

      size_t maxTokens = imp->app_.settings().generationDefaultMaxTokens();
      if (request.contains("max_tokens") && request["max_tokens"].is_number_float()) {
        maxTokens = request["max_tokens"];
        LOG_MSG << "Using request's max_tokens limit" << maxTokens;
      } else if (0 < apiConfig.maxTokens && apiConfig.maxTokens != size_t(-1)) {
        maxTokens = apiConfig.maxTokens;
        LOG_MSG << "Using model's max_tokens limit" << maxTokens;
      } else {
        LOG_MSG << "Using default max_tokens limit" << maxTokens;
      }

      const float contextSizeRatio = request.value("ctxratio", 0.9f);
      const bool attachedOnly = request.value("attachedonly", false);

      res.set_header("Content-Type", "text/event-stream");
      res.set_header("Cache-Control", "no-cache");
      res.set_header("Connection", "keep-alive");

      res.set_chunked_content_provider(
        "text/event-stream",
        [this, messagesJson, question, temperature, contextSizeRatio, attachedOnly, attachments, sources, maxTokens, request, apiConfig, pendingInfoVec, start]
        (size_t offset, httplib::DataSink &sink) {

          auto packPayload = [](std::string data) {
            // SSE format requires "data: <payload>\n\n"
            nlohmann::json payload = { { "content", std::move(data) } };
            return "data: " + payload.dump() + "\n\n";
            };

          auto initialInfo = packPayload("[meta]Searching for relevant content");
          sink.write(initialInfo.data(), initialInfo.size());

          auto onInfo = [packPayload, &sink](std::string_view info)
            {
              auto s = packPayload(std::string{ "[meta]" } + std::string{ info.data(), info.size() });
              sink.write(s.data(), s.size());
            };


          try {
            size_t initialTokens = imp->app_.tokenizer().countTokensWithVocab(CompletionClient::queryTemplate());
            initialTokens += messagesNofTokens(messagesJson, imp->app_); // history + question
            const auto [orderedResults, usedTokens] = processInputResults(imp->app_, apiConfig, initialTokens, question, attachments, sources,
              contextSizeRatio, attachedOnly, onInfo
            );

            for (const auto &s : pendingInfoVec) {
              onInfo(s);
            }

            CompletionClient completionClient(apiConfig, imp->app_.settings().generationTimeoutMs(), imp->app_);
            const std::string fullResponse = completionClient.generateCompletion(
              messagesJson, orderedResults, temperature, maxTokens,
              [&sink, packPayload](const std::string &chunk) {
#ifdef _DEBUG2
                LOG_MSG_DEBUG << chunk;
#endif
                std::string sse = packPayload(chunk);
                bool success = sink.write(sse.data(), sse.size());
                if (!success) {
                  return; // Client disconnected
                }
              });

#ifdef _DEBUG2
            testStreaming([&sink](const std::string &chunk) {
              if (!sink.write(chunk.data(), chunk.size())) {
                return; // client disconnected
              }
              });
#endif
            size_t resTokens = imp->app_.tokenizer().countTokensWithVocab(fullResponse);
            onInfo(fmt::format("Response token count {}", resTokens));

            auto costReq = apiConfig.inputTokensPrice(usedTokens);
            auto costRes = apiConfig.outputTokensPrice(resTokens);
            auto costTotal = costReq + costRes;
            if (costTotal == 0)
              onInfo("Total cost incurred: 0");
            else
              onInfo(fmt::format("Approx. cost incurred: ${:.4f} (input: {:.4f}, output: {:.4f})", costTotal, costReq, costRes));

            double streamMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            imp->app_.monitor().recordRequest("chat", streamMs, false, usedTokens, resTokens);
            imp->app_.monitor().recordGenerationCost(apiConfig.id, usedTokens, resTokens, imp->app_.settings().providers());

            // If generation produced no content, surface a visible fallback so the
            // client doesn't render a blank reply. Sources still go out below.
            bool emptyResult = fullResponse.empty();
            if (emptyResult) {
              std::string fallback =
                "The model returned an empty response. "
                "This can happen when it hits the token limit, produces only internal "
                "reasoning, or refuses the request. Try rephrasing, increasing max_tokens, "
                "or selecting a different model.";
              std::string sse = packPayload(fallback);
              sink.write(sse.data(), sse.size());
            }


            // Add sources information
            nlohmann::json sourcesJson;
            std::set<std::string> distinctSources;
            for (const auto &result : orderedResults) {
              if (distinctSources.insert(result.sourceId).second) {
                sourcesJson.push_back(result.sourceId);
              }
            }

            // Send sources information as a separate SSE message
            nlohmann::json sourcesPayload = {
              { "sources", sourcesJson },
              { "type", "context_sources" },
              { "empty_result", emptyResult },
              { "model", apiConfig.model }
            };
            std::string sourcesSse = "data: " + sourcesPayload.dump() + "\n\n";
            sink.write(sourcesSse.data(), sourcesSse.size());


            std::string done = "data: [DONE]\n\n";
            sink.write(done.data(), done.size());
            sink.done();
          } catch (const std::exception &e) {
            std::string error = "data: {\"error\": \"" + std::string(e.what()) + "\"}\n\n";
            sink.write(error.data(), error.size());
            sink.done();
            LOG_MSG_ERROR << "Chat generation failed: " << e.what();
          }
          return true;
        }
      );

    } catch (const std::exception &e) {
      json error = { { "error", e.what() } };
      res.status = 400;
      res.set_content(error.dump(), "application/json");
      Impl::errorCounter_++;
      double streamMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      imp->app_.monitor().recordRequest("chat", streamMs, true, 0, 0);
      LOG_MSG_ERROR << "Chat generation failed: " << e.what();
    }
    Impl::requestCounter_++;
    Impl::chatCounter_++;
    recordDuration(start, Impl::avgChatTimeMs_);
    });

  server.Post("/api/fim", [this](const httplib::Request &req, httplib::Response &res) {
    const auto start = std::chrono::steady_clock::now();
    try {
      LOG_MSG << "POST /api/fim";
      json request = json::parse(req.body);

      if (!request.contains("prefix") || !request["prefix"].is_string()) {
        throw std::invalid_argument("'prefix' field required and must be a string");
      }
      if (!request.contains("suffix") || !request["suffix"].is_string()) {
        throw std::invalid_argument("'suffix' field required and must be a string");
      }

      std::string prefix = request["prefix"].get<std::string>();
      std::string suffix = request["suffix"].get<std::string>();
      std::string filename = request.value("filename", std::string{});
      filename = std::filesystem::path(filename).lexically_normal().generic_string();

      if (request.value("encoding", "") == "base64") {
        prefix = base64_decode(prefix);
        suffix = base64_decode(suffix);
      }

      auto apiConfig = getTargetApi(request, imp->app_);

      LOG_MSG << "[" << apiConfig.model << "]" << apiConfig.apiUrl;

      const float temperature = request.value("temperature", imp->app_.settings().generationDefaultTemperature());
      const size_t maxTokens = request.value("max_tokens", imp->app_.settings().generationDefaultMaxTokens());
      const float contextSizeRatio = request.value("ctxratio", 0.5f);
      std::vector<std::string> stops = request.value("stop", std::vector<std::string>{});

      size_t initialTokens = imp->app_.tokenizer().countTokensWithVocab(CompletionClient::fimTemplate());
      initialTokens += imp->app_.tokenizer().countTokensWithVocab(suffix) + imp->app_.tokenizer().countTokensWithVocab(prefix);
      const auto searchResults = processInputResults(imp->app_, apiConfig, initialTokens, prefix, {}, { filename }, contextSizeRatio, {}, nullptr);


      LOG_MSG << "Generating FIM with prefix length" << prefix.size() << "and suffix length" << suffix.size();
      CompletionClient completionClient(apiConfig, imp->app_.settings().generationTimeoutMs(), imp->app_);
      std::string fullResponse = completionClient.generateFim(prefix, suffix, stops, temperature, maxTokens, searchResults.first);
      LOG_MSG << "[FIM] Generated tokens:" << imp->app_.tokenizer().countTokensWithVocab(fullResponse);
      json response = { { "completion", fullResponse } };
      size_t outTokens = imp->app_.tokenizer().countTokensWithVocab(fullResponse);
      double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      imp->app_.monitor().recordRequest("fim", ms, false, searchResults.second, outTokens);
      imp->app_.monitor().recordGenerationCost(apiConfig.id, searchResults.second, outTokens, imp->app_.settings().providers());
      res.set_content(response.dump(), "application/json");
      Impl::requestCounter_++;
    } catch (const std::exception &e) {
      json error = { { "error", e.what() } };
      res.status = 400;
      res.set_content(error.dump(), "application/json");
      Impl::errorCounter_++;
      double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      imp->app_.monitor().recordRequest("chat", ms, true, 0, 0);
      LOG_MSG_ERROR << "FIM generation failed: " << e.what();
    }
    recordDuration(start, Impl::avgChatTimeMs_); // reuse chat timing metric
    });

}
