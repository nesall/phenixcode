#include "httpserver.h"
#include "httpserver_internal.h"
#include "app.h"
#include "context_builder.h"
#include "model_router.h"
#include "database.h"
#include "inference.h"
#include "inference_messages.h"
#include "chat_tools.h"
#include "tool_service.h"
#include "cutils.h"
#include "sourceproc.h"
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
#include <cassert>
#include <functional>
#include <string_view>
#include <filesystem>
#include <utility>
#include <cmath>

using json = nlohmann::json;

using namespace httpserver_detail;
using namespace context;

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

  std::map<std::string, std::string> getReasoningSelection(const json &request, const ApiConfig &cfg) {
    std::map<std::string, std::string> selection;
    const auto reasoning = request.find("reasoning");
    for (const auto &param : cfg.reasoning) {
      std::string value = param.defaultValue;
      if (reasoning != request.end() && reasoning->is_object()) {
        const auto requested = reasoning->find(param.name);
        if (requested != reasoning->end() && requested->is_string()) {
          const std::string candidate = requested->get<std::string>();
          if (std::find(param.values.begin(), param.values.end(), candidate) != param.values.end())
            value = candidate;
        }
      }
      selection.emplace(param.name, std::move(value));
    }
    return selection;
  }

} // anonymous namespace


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
        "attachedonly": false,
        "free_chat": false,
      }
      */
      json request = json::parse(req.body);
      const bool freeChat = request.value("free_chat", false);
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
      const auto reasoningSelection = getReasoningSelection(request, apiConfig);
      const auto sessionId = request.value("session_id", std::string{});
      const auto responseId = request.value("response_id", std::string{});
      if (sessionId.size() > 128 || responseId.size() > 128)
        throw std::invalid_argument("Session and response IDs must not exceed 128 characters");
      const auto replaySessionId = sessionId.empty() ? sessionId : (freeChat ? "free:" : "context:") + sessionId;
      for (const auto &message : messagesJson) {
        if (!message.is_object() || !message.contains("role") || !message["role"].is_string() ||
            !message.contains("content") || !message["content"].is_string() ||
            (message.contains("replay_id") && !message["replay_id"].is_string()))
          throw std::invalid_argument("Each message requires string role and content fields, with an optional string replay_id");
      }
      LOG_MSG << "[" << apiConfig.model << "]" << apiConfig.apiUrl;

      const bool agentEnabled = imp->app_.settings().providers().agentEnabled();
      const bool toolsEnabled = chat_tools::isToolEnabled(agentEnabled, apiConfig, freeChat, request.value("attachedonly", false));
      if (agentEnabled && !freeChat && !request.value("attachedonly", false)) {
        if (apiConfig.supportsTools == "on" && apiConfig.apiStyle == ApiConfig::ApiStyle::Anthropic)
          pendingInfoVec.push_back("Tools are unavailable for Anthropic API style; continuing without tools");
        else if (apiConfig.supportsTools == "auto")
          pendingInfoVec.push_back("Automatic tool detection is unavailable; choose Tool Support On for an OpenAI-compatible provider");
      }

      if (toolsEnabled) LOG_MSG_INFO << "Tool support is enabled for this request";

      const auto &attachmentsJson = request["attachments"];
      auto attachments = parseAttachments(attachmentsJson);

      std::vector<std::string> sources;
      if (!freeChat && request.contains("sourceids")) {
        const auto &sourceidsJson = request["sourceids"];
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
      if (request.contains("max_tokens")) {
        maxTokens = inference_contant::positiveTokenLimit(request, maxTokens);
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
        [=, this]
        (size_t offset, httplib::DataSink &sink) {

          auto packPayload = [](std::string data) {
            // SSE format requires "data: <payload>\n\n"
            nlohmann::json payload = { { "content", std::move(data) } };
            return "data: " + payload.dump() + "\n\n";
            };

          bool connected = true;
          auto isConnected = [&sink, &connected] {
            if (connected && sink.is_writable && !sink.is_writable()) connected = false;
            return connected;
          };

          auto onInfo = [&sink, &connected](std::string_view info, const json &details = json::object())
            {
              json payload = details;
              payload["type"] = "meta";
              payload["content"] = std::string(info);
              if (!payload.contains("status")) payload["status"] = "info";
              const auto sse = "data: " + payload.dump() + "\n\n";
              if (connected && !sink.write(sse.data(), sse.size())) connected = false;
            };
          onInfo(freeChat ? "Preparing conversation" : "Searching for relevant content");
          if (!connected) return false;

          inference_contant::TokenTotals tokenTotals;
          const auto &totalInputTokens = tokenTotals.input;
          const auto &resTokens = tokenTotals.output;
          try {
            const auto count = [&](std::string_view text) { return imp->app_.tokenizer().countTokensWithVocab(text); };
            const auto inputBudget = inference_contant::inputTokenBudget(apiConfig.contextLength, maxTokens);
            auto prepared = imp->app_.reasoningHistory().prepare(replaySessionId, responseId, apiConfig, messagesJson);
            size_t initialTokens = inference_contant::estimateRequestTokens(prepared.messages, json::array(), count);
            if (!freeChat || !attachments.empty())
              initialTokens += imp->app_.tokenizer().countTokensWithVocab(CompletionClient::queryTemplate(freeChat));

            json tools = toolsEnabled ? chat_tools::definitions(true, /*includeCodeGraph=*/true) : json::array();
            const auto toolReserve = tools.empty() ? 0 : (std::min)(size_t(8192), apiConfig.contextLength / 8);
            auto schemaTokens = tools.empty() ? 0 : count(tools.dump());
            bool omittedCodeGraph = false;
            if (!tools.empty()) {
              bool omitCodeGraph = apiConfig.contextLength < 8192;
              if (!omitCodeGraph) {
                omitCodeGraph = schemaTokens >= inputBudget || toolReserve >= inputBudget - schemaTokens ||
                  (inputBudget - schemaTokens - toolReserve) < initialTokens + 256;
              }
              if (omitCodeGraph) {
                tools = chat_tools::definitions(true, /*includeCodeGraph=*/false);
                schemaTokens = count(tools.dump());
                omittedCodeGraph = true;
                LOG_MSG_INFO << "Omitting dependencies and find_references tools due to model context budget (" << apiConfig.contextLength << " tokens)";
              }
            }
            const auto retrievalBudget = inference_contant::retrievalTokenBudget(
              apiConfig.contextLength, maxTokens, schemaTokens, contextSizeRatio, toolReserve);
            auto [orderedResults, usedTokens] = processInputResults(
              imp->app_, apiConfig, initialTokens, question, attachments, sources, contextSizeRatio, attachedOnly, onInfo, freeChat, retrievalBudget
            );

            for (const auto &s : pendingInfoVec) {
              onInfo(s);
            }
            if (omittedCodeGraph) {
              onInfo("Omitted dependencies and find_references tools due to model context budget");
            }

            CompletionClient cc(apiConfig, imp->app_.settings().generationTimeoutMs(), imp->app_);
            auto generationMessages = cc.prepareMessages(prepared.messages, orderedResults, freeChat);
            // Labels and template expansion must also fit after retrieval selection.
            while (inference_contant::estimateRequestTokens(generationMessages, json::array(), count) > retrievalBudget && !orderedResults.empty()) {
              orderedResults.pop_back();
              generationMessages = cc.prepareMessages(prepared.messages, orderedResults, freeChat);
            }
            if (inference_contant::estimateRequestTokens(generationMessages, json::array(), count) > retrievalBudget)
              throw std::runtime_error("Conversation history exceeds the available input budget");
            chat_tools::Service toolService(imp->app_);
            std::set<std::string> displayRoots;
            if (!tools.empty()) for (const auto &source : imp->app_.settings().sources()) {
              if (source.type != "directory" && source.type != "file") continue;
              auto root = std::filesystem::absolute(std::filesystem::path(std::u8string(source.path.begin(), source.path.end()))).lexically_normal();
              if (source.type == "file") root = root.parent_path();
              const auto bytes = root.generic_u8string();
              std::string text(bytes.begin(), bytes.end());
              if (!text.ends_with('/')) text += '/';
              displayRoots.insert(std::move(text));
            }
            const auto displayRoot = utils::commonDirPrefix(displayRoots);
            size_t roundNumber = 0;
            auto generate = [&](const json &messages) -> chat_tools::Turn {
              ++roundNumber;
              auto roundMessages = messages;
              auto roundTools = tools;
              auto inputTokens = inference_contant::estimateRequestTokens(roundMessages, roundTools, count);
              if (!tools.empty() && (roundNumber == chat_tools::kMaxAgentSteps ||
                  inputTokens > inputBudget || toolReserve > inputBudget - inputTokens)) {
                roundTools = json::array();
                const std::string instruction = "Answer the user now using the evidence already gathered. No further tools are available. State any gaps in the evidence.";
                if (!roundMessages.empty() && roundMessages.front().value("role", "") == "system")
                  roundMessages.front()["content"] = roundMessages.front()["content"].get<std::string>() + "\n\n" + instruction;
                else
                  roundMessages.insert(roundMessages.begin(), {{"role", "system"}, {"content", instruction}});
                if (!chat_tools::fitToolResults(roundMessages, inputBudget, count))
                  throw std::runtime_error("Conversation leaves no room for a final answer; shorten the conversation or reduce max_tokens");
                inputTokens = inference_contant::estimateRequestTokens(roundMessages, roundTools, count);
                onInfo("Finishing with gathered evidence; tool round or context budget reached", {{"status", "warning"}});
              }
              if (inputTokens > inputBudget) throw std::runtime_error("Conversation exceeds the model context budget");
              chat_tools::Turn turn;
              inference_contant::CompletionInfo info;
              const auto accountRound = [&] {
                tokenTotals.add(info, inputTokens);
              };
              const auto roundLabel = tools.empty() ? std::string("Response") :
                "Round " + std::to_string(roundNumber) + "/" + std::to_string(chat_tools::kMaxAgentSteps);
              json roundMeta = {{"event_id", "round:" + std::to_string(roundNumber)}, {"round", roundNumber}, {"status", "running"}, {"summary", "Generating response..."}};
              onInfo(roundLabel + ": generating response", roundMeta);
              try {
              turn.text = cc.generateCompletion(roundMessages, orderedResults, temperature, maxTokens,
                [&sink, packPayload, &connected, bufferAnswer = !tools.empty()](const std::string &chunk) {
                  if (!connected || bufferAnswer) return;
                  const auto sse = packPayload(chunk);
                  if (!sink.write(sse.data(), sse.size())) connected = false;
                },
                reasoningSelection, (!tools.empty() || apiConfig.historyReplay > 0) ? &turn.assistant : nullptr,
                [&sink, &connected](const std::string &chunk) {
                  if (!connected) return;
                  const json payload = { { "type", "reasoning_delta" },{ "content", chunk } };
                  const auto sse = "data: " + payload.dump() + "\n\n";
                  if (!sink.write(sse.data(), sse.size())) connected = false;
                },
                freeChat, roundTools, false, isConnected,
                {}, &info
              );
              } catch (...) {
                accountRound();
                roundMeta["status"] = "error";
                roundMeta["summary"] = "Request failed";
                onInfo(roundLabel + ": request failed", roundMeta);
                throw;
              }
              accountRound();
              turn.incomplete = info.status == inference_contant::CompletionStatus::Incomplete;
              const bool toolRound = info.status == inference_contant::CompletionStatus::ToolCalls;
              std::string summary = turn.incomplete ? "response incomplete" : toolRound ?
                "requested " + std::to_string(turn.assistant.value("tool_calls", json::array()).size()) + " tool calls" : turn.text.empty() ? "no answer text returned" : "response ready";
              roundMeta["status"] = turn.incomplete || (!toolRound && turn.text.empty()) ? "warning" : toolRound ? "info" : "success";
              if (info.reasoningTokens) summary += "; reasoning tokens " + std::to_string(*info.reasoningTokens);
              else if (info.estimatedReasoningTokens) summary += "; reasoning tokens " + std::to_string(*info.estimatedReasoningTokens) + " (estimated)";
              else summary += "; reasoning usage unavailable";
              std::string compact = turn.incomplete ? "Response incomplete" : toolRound ?
                "Round " + std::to_string(roundNumber) + " - " + std::to_string(turn.assistant.at("tool_calls").size()) + (turn.assistant.at("tool_calls").size() == 1 ? " tool" : " tools") :
                turn.text.empty() ? "No answer text returned" : "Response ready";
              if (info.reasoningTokens || info.estimatedReasoningTokens) compact += " - " +
                std::to_string(info.reasoningTokens.value_or(info.estimatedReasoningTokens.value_or(0))) + " reasoning tokens" +
                (info.reasoningTokens ? "" : " (est.)");
              roundMeta["summary"] = compact;
              onInfo(roundLabel + ": " + summary, roundMeta);
              return turn;
              };
            auto execute = [&](const json &call) {
              return toolService.execute(call, tools, &apiConfig, isConnected,
                [&](const std::string &message) { onInfo(message, {{"status", "warning"}}); });
            };
            auto outcome = tools.empty() ? generate(generationMessages) :
              chat_tools::run(generationMessages, generate, execute,
                isConnected, chat_tools::kMaxAgentSteps, [&](const json &event) {
                  onInfo(event.at("content").get_ref<const std::string &>(), event);
                }, displayRoot);
            if (!connected) {
              const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
              imp->app_.monitor().recordRequest("chat", elapsed, true, totalInputTokens, resTokens);
              if (totalInputTokens || resTokens)
                imp->app_.monitor().recordGenerationCost(apiConfig.id, totalInputTokens, resTokens, imp->app_.settings().providers());
              return false;
            }
            const auto &fullResponse = outcome.text;
            if (outcome.limitReached) onInfo("Tool round limit reached", {{"status", "warning"}});
            if (!tools.empty() && !fullResponse.empty()) {
              const auto sse = packPayload(fullResponse);
              if (!sink.write(sse.data(), sse.size())) return false;
            }
            if (outcome.incomplete) onInfo("Model response was incomplete; it was not retained for reasoning replay", {{"status", "warning"}});
            const bool remembered = !outcome.incomplete && !fullResponse.empty() &&
              imp->app_.reasoningHistory().remember(prepared.ticket, outcome.assistant);

#ifdef _DEBUG2
            testStreaming([&sink](const std::string &chunk) {
              if (!sink.write(chunk.data(), chunk.size())) {
                return; // client disconnected
              }
              });
#endif
            onInfo(fmt::format("Token totals{}: input {}, output {}", tokenTotals.estimated ? " (estimated in part)" : " (provider-reported)", totalInputTokens, resTokens));

            std::string reasoningSummary = tokenTotals.reasoningKnownRounds ?
              "Reasoning tokens across all rounds: " + std::to_string(tokenTotals.reasoning) : "Reasoning usage unavailable for all rounds";
            if (tokenTotals.reasoningEstimated) reasoningSummary += " (estimated in part)";
            if (tokenTotals.reasoningKnownRounds && tokenTotals.reasoningUnavailableRounds)
              reasoningSummary += "; usage unavailable for " + std::to_string(tokenTotals.reasoningUnavailableRounds) + " rounds";
            onInfo(reasoningSummary);

            auto costReq = apiConfig.inputTokensPrice(totalInputTokens);
            auto costRes = apiConfig.outputTokensPrice(resTokens);
            auto costTotal = costReq + costRes;
            if (costTotal == 0)
              onInfo("Total cost incurred: 0");
            else
              onInfo(fmt::format("Approx. cost incurred: ${:.4f} (input: {:.4f}, output: {:.4f})", costTotal, costReq, costRes));

            double streamMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            imp->app_.monitor().recordRequest("chat", streamMs, outcome.limitReached || outcome.incomplete, totalInputTokens, resTokens);
            imp->app_.monitor().recordGenerationCost(apiConfig.id, totalInputTokens, resTokens, imp->app_.settings().providers());

            // If generation produced no content, surface a visible fallback so the
            // client doesn't render a blank reply. Sources still go out below.
            bool emptyResult = fullResponse.empty();
            if (emptyResult) {
              std::string fallback = outcome.incomplete ?
                "The model response was incomplete. Try increasing max_tokens or retrying the request." : outcome.limitReached ?
                "The project search reached its step limit. Please narrow the question or try again." :
                "The model returned an empty response. "
                "This can happen when it hits the token limit, produces only internal "
                "reasoning, or refuses the request. Try rephrasing, increasing max_tokens, "
                "or selecting a different model.";
              std::string sse = packPayload(fallback);
              sink.write(sse.data(), sse.size());
            }
            else if (outcome.incomplete) {
              const auto sse = packPayload("\n\n[Response incomplete. Try increasing max_tokens or retrying.]");
              sink.write(sse.data(), sse.size());
            }


            // Add sources information
            nlohmann::json sourcesJson = json::array();
            std::set<std::string> distinctSources;
            for (const auto &result : orderedResults) {
              if (distinctSources.insert(result.sourceId).second) {
                sourcesJson.push_back(result.sourceId);
              }
            }

            for (const auto &source : toolService.usedSources())
              if (distinctSources.insert(source).second) sourcesJson.push_back(source);

            // Send sources information as a separate SSE message
            nlohmann::json sourcesPayload = {
              { "sources", sourcesJson },
              { "type", "context_sources" },
              { "empty_result", emptyResult },
              { "model", apiConfig.model }
            };
            if (remembered) sourcesPayload["replay_id"] = responseId;
            std::string sourcesSse = "data: " + sourcesPayload.dump() + "\n\n";
            sink.write(sourcesSse.data(), sourcesSse.size());


            std::string done = "data: [DONE]\n\n";
            sink.write(done.data(), done.size());
            sink.done();
          } catch (const std::exception &e) {
            std::string error = "data: " + json({{"error", e.what()}}).dump() + "\n\n";
            sink.write(error.data(), error.size());
            sink.done();
            const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            imp->app_.monitor().recordRequest("chat", elapsed, true, totalInputTokens, resTokens);
            if (totalInputTokens || resTokens)
              imp->app_.monitor().recordGenerationCost(apiConfig.id, totalInputTokens, resTokens, imp->app_.settings().providers());
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
    httpserver_detail::recordDuration(start, Impl::avgChatTimeMs_);
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

      auto apiConfig = model_router::getTargetApi(request, imp->app_);
      const auto reasoningSelection = getReasoningSelection(request, apiConfig);

      LOG_MSG << "[" << apiConfig.model << "]" << apiConfig.apiUrl;

      const float temperature = request.value("temperature", imp->app_.settings().generationDefaultTemperature());
      const size_t maxTokens = inference_contant::positiveTokenLimit(request, imp->app_.settings().generationDefaultMaxTokens());
      const float contextSizeRatio = request.value("ctxratio", 0.5f);
      std::vector<std::string> stops = request.value("stop", std::vector<std::string>{});

      size_t initialTokens = imp->app_.tokenizer().countTokensWithVocab(CompletionClient::fimTemplate());
      initialTokens += imp->app_.tokenizer().countTokensWithVocab(suffix) + imp->app_.tokenizer().countTokensWithVocab(prefix);
      const auto searchResults = processInputResults(imp->app_, apiConfig, initialTokens, prefix, {}, { filename }, contextSizeRatio, {}, nullptr);


      LOG_MSG << "Generating FIM with prefix length" << prefix.size() << "and suffix length" << suffix.size();
      CompletionClient cc(apiConfig, imp->app_.settings().generationTimeoutMs(), imp->app_);
      std::string fullResponse = cc.generateFim(prefix, suffix, stops, temperature, maxTokens, searchResults.first, reasoningSelection);
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
    httpserver_detail::recordDuration(start, Impl::avgChatTimeMs_); // reuse chat timing metric
    });

}
