#include "inference_messages.h"
#include <algorithm>
#include <set>
#include <cmath>
#include <limits>

namespace inference_contant {

  namespace {
    std::optional<size_t> tokenNumber(const nlohmann::json &object, const char *key) {
      const auto it = object.find(key);
      if (it == object.end() || !it->is_number_integer() || *it < 0) return {};
      return it->get<size_t>();
    }
  }

  void CompletionInfo::observe(const ApiConfig &cfg, const nlohmann::json &response) {
    const auto *root = &response;
    if (cfg.apiStyle == ApiConfig::ApiStyle::Anthropic && stringField(response, "type") == "message_start") {
      const auto message = response.find("message");
      if (message == response.end()) return;
      root = &*message;
    }
    const auto usage = root->find("usage");
    if (usage != root->end() && usage->is_object()) {
      if (cfg.apiStyle == ApiConfig::ApiStyle::Anthropic) {
        if (auto n = tokenNumber(*usage, "input_tokens")) uncachedInput_ = n;
        if (auto n = tokenNumber(*usage, "cache_read_input_tokens")) cacheRead_ = n;
        if (auto n = tokenNumber(*usage, "cache_creation_input_tokens")) cacheWrite_ = n;
        if (uncachedInput_) inputTokens = *uncachedInput_ + cacheRead_.value_or(0) + cacheWrite_.value_or(0);
        if (auto n = tokenNumber(*usage, "output_tokens")) outputTokens = n;
      } else {
        if (auto n = tokenNumber(*usage, "prompt_tokens")) inputTokens = n;
        if (auto n = tokenNumber(*usage, "completion_tokens")) outputTokens = n;
        const auto details = usage->find("completion_tokens_details");
        if (details != usage->end() && details->is_object())
          if (auto n = tokenNumber(*details, "reasoning_tokens")) reasoningTokens = n;
      }
    }
    std::string reason;
    if (cfg.apiStyle == ApiConfig::ApiStyle::Anthropic) {
      reason = stringField(response, "stop_reason");
      const auto delta = response.find("delta");
      if (delta != response.end()) reason = stringField(*delta, "stop_reason");
    } else {
      const auto choices = response.find("choices");
      if (choices != response.end() && choices->is_array() && !choices->empty())
        reason = stringField(choices->front(), "finish_reason");
    }
    if (!reason.empty()) stopReason = reason;
    if (stopReason == "stop" || stopReason == "end_turn" || stopReason == "stop_sequence") status = CompletionStatus::Complete;
    else if (stopReason == "tool_calls") status = CompletionStatus::ToolCalls;
    else status = CompletionStatus::Incomplete;
  }

  std::string toolArguments(const nlohmann::json &message) {
    std::string text;
    const auto calls = message.find("tool_calls");
    if (calls != message.end() && calls->is_array())
      for (const auto &call : *calls) {
        const auto function = call.find("function");
        if (function != call.end()) text += stringField(*function, "arguments");
      }
    return text;
  }

  size_t estimateRequestTokens(const nlohmann::json &messages, const nlohmann::json &tools, const TokenCounter &count) {
    size_t tokens = 3; // Approximate message framing; provider usage is authoritative after generation.
    for (const auto &message : messages) {
      std::string reasoning;
      tokens += 4 + count(stringField(message, "role"));
      tokens += count(extractMessageContent(message, reasoning)) + count(reasoning);
      tokens += count(toolArguments(message));
    }
    if (!tools.empty()) tokens += count(tools.dump());
    return tokens;
  }

  size_t positiveTokenLimit(const nlohmann::json &request, size_t fallback) {
    const auto value = request.find("max_tokens");
    if (value == request.end()) return fallback;
    const auto number = tokenNumber(request, "max_tokens");
    if (number && *number > 0) return *number;
    // Preserve compatibility with clients serializing integral limits as 4096.0.
    if (value->is_number_float()) {
      const auto floating = value->get<double>();
      if (std::isfinite(floating) && floating > 0 && std::trunc(floating) == floating &&
          floating < static_cast<double>((std::numeric_limits<size_t>::max)()))
        return static_cast<size_t>(floating);
    }
    throw std::invalid_argument("max_tokens must be a positive integer");
  }

  size_t inputTokenBudget(size_t contextLength, size_t outputTokens) {
    if (!contextLength || outputTokens >= contextLength)
      throw std::invalid_argument("max_tokens must be smaller than the model context length");
    return contextLength - outputTokens;
  }

  size_t retrievalTokenBudget(size_t contextLength, size_t outputTokens, size_t schemaTokens, float ratio, size_t toolReserve) {
    auto budget = inputTokenBudget(contextLength, outputTokens);
    if (schemaTokens >= budget || toolReserve >= budget - schemaTokens)
      throw std::invalid_argument("Tools and output reservation leave no room for conversation context");
    budget -= schemaTokens + toolReserve;
    if (!std::isfinite(ratio)) throw std::invalid_argument("ctxratio must be finite");
    return (std::min)(budget, static_cast<size_t>(contextLength * std::clamp(ratio, 0.1f, 1.0f)));
  }

  std::string stringField(const nlohmann::json &object, const char *name) {
    const auto field = object.find(name);
    return field != object.end() && field->is_string() ? field->get<std::string>() : std::string{};
  }

  void appendText(const nlohmann::json &value, std::string &text) {
    if (value.is_string()) {
      text += value.get<std::string>();
    } else if (value.is_array()) {
      for (const auto &part : value) {
        if (stringField(part, "type") == "text")
          text += stringField(part, "text");
      }
    }
  }

  void appendBlock(const nlohmann::json &block, std::string &answer, std::string &reasoning) {
    const auto type = stringField(block, "type");
    if (type == "text" || type == "text_delta") {
      answer += stringField(block, "text");
    } else if (type == "thinking" || type == "thinking_delta" || type == "reasoning") {
      const auto thinking = block.find("thinking");
      if (thinking != block.end())
        appendText(*thinking, reasoning);
      else
        reasoning += stringField(block, "text");
    }
  }

  std::string extractMessageContent(const nlohmann::json &message, std::string &reasoning) {
    std::string answer;
    std::string blockReasoning;
    const auto content = message.find("content");
    if (content != message.end()) {
      if (content->is_string()) {
        answer = content->get<std::string>();
      } else if (content->is_array()) {
        for (const auto &block : *content)
          appendBlock(block, answer, blockReasoning);
      }
    }

    // Some compatible APIs expose the same reasoning in multiple fields.
    // Prefer a plain-text field and use structured content as a fallback.
    std::string messageReasoning = stringField(message, "reasoning_content");
    if (messageReasoning.empty())
      messageReasoning = stringField(message, "reasoning");
    if (messageReasoning.empty()) {
      const auto details = message.find("reasoning_details");
      if (details != message.end() && details->is_array()) {
        for (const auto &detail : *details) {
          const auto type = stringField(detail, "type");
          if (type == "reasoning.text")
            messageReasoning += stringField(detail, "text");
          else if (type == "reasoning.summary")
            messageReasoning += stringField(detail, "summary");
        }
      }
    }
    reasoning += messageReasoning.empty() ? blockReasoning : messageReasoning;
    return answer;
  }

  const nlohmann::json *firstChoiceContent(const nlohmann::json &response, const char *name) {
    const auto choices = response.find("choices");
    if (choices == response.end() || !choices->is_array() || choices->empty())
      return nullptr;
    const auto content = choices->front().find(name);
    return content != choices->front().end() && content->is_object() ? &*content : nullptr;
  }

  std::string extractDeltaContent(const ApiConfig &cfg, const nlohmann::json &chunk, std::string &reasoning) {
    if (cfg.apiStyle == ApiConfig::ApiStyle::Anthropic) {
      const auto type = stringField(chunk, "type");
      const char *field = nullptr;
      if (type == "content_block_delta") field = "delta";
      else if (type == "content_block_start") field = "content_block";
      if (!field) return {};
      std::string answer;
      const auto block = chunk.find(field);
      if (block != chunk.end())
        appendBlock(*block, answer, reasoning);
      return answer;
    }
    const auto delta = firstChoiceContent(chunk, "delta");
    return delta ? extractMessageContent(*delta, reasoning) : std::string{};
  }

  std::string extractFullContent(const ApiConfig &cfg, const nlohmann::json &response, std::string &reasoning) {
    if (cfg.apiStyle == ApiConfig::ApiStyle::Anthropic)
      return extractMessageContent(response, reasoning);
    const auto message = firstChoiceContent(response, "message");
    return message ? extractMessageContent(*message, reasoning) : std::string{};
  }

  nlohmann::json completedAssistantMessage(const ApiConfig &cfg, const nlohmann::json &response, bool allowToolCalls) {
    if (cfg.apiStyle == ApiConfig::ApiStyle::Anthropic) {
      const auto stop = stringField(response, "stop_reason");
      if (stop == "end_turn" || stop == "stop_sequence")
        return {{"role", "assistant"}, {"content", response.at("content")}};
      return nullptr;
    }
    const auto message = firstChoiceContent(response, "message");
    if (!message || stringField(*message, "role") != "assistant") return nullptr;
    const auto stop = stringField(response.at("choices").front(), "finish_reason");
    const auto calls = message->find("tool_calls");
    if (calls != message->end() && !calls->empty())
      return allowToolCalls && stop == "tool_calls" && calls->is_array() ? *message : nlohmann::json(nullptr);
    return stop == "stop" ? *message : nlohmann::json(nullptr);
  }

} // namespace inference_contant

namespace reasoning_history {

  using nlohmann::json;
  using inference_contant::stringField;

  namespace {
    void appendContent(json &target, const json &delta) {
      if (delta.is_null()) return;
      if (target.is_null()) {
        target = delta;
        return;
      }
      if (target.is_string() && delta.is_string()) {
        target.get_ref<std::string &>() += delta.get_ref<const std::string &>();
        return;
      }
      if (target.is_string())
        target = json::array({ { { "type", "text" },{ "text", target } } });
      if (!target.is_array()) throw std::runtime_error("Unsupported streamed content");
      if (delta.is_array()) {
        for (const auto &part : delta) target.push_back(part);
      } else if (delta.is_string()) {
        target.push_back({ { "type", "text" },{ "text", delta } });
      } else {
        throw std::runtime_error("Unsupported streamed content delta");
      }
    }

    // Indexed arrays include tool_calls and reasoning_details. Preserve opaque
    // fields as well as readable text, including fragmented thinking signatures.
    void mergeDelta(json &target, const json &delta) {
      for (const auto &[key, value] : delta.items()) {
        if (value.is_null()) continue;
        auto &field = target[key];
        if (key == "content") {
          appendContent(field, value);
        } else if (value.is_object()) {
          if (!field.is_object()) field = json::object();
          mergeDelta(field, value);
        } else if (value.is_array()) {
          if (!field.is_array()) field = json::array();
          for (const auto &part : value) {
            if (!part.is_object()) throw std::runtime_error("Unsupported streamed array item");
            const auto index = part.find("index");
            const auto id = stringField(part, "id");
            auto existing = std::find_if(field.begin(), field.end(), [&](const json &item) {
              if (index != part.end()) return item.contains("index") && item["index"] == *index;
              return !id.empty() && stringField(item, "id") == id;
              });
            if (existing == field.end() && index == part.end() && id.empty() &&
              key == "reasoning_details" && !field.empty() &&
              stringField(field.back(), "type") == stringField(part, "type"))
              existing = std::prev(field.end());
            if (existing == field.end()) field.push_back(part);
            else mergeDelta(*existing, part);
          }
        } else if (value.is_string() && field.is_string() &&
          key != "role" && key != "type" && key != "id" && key != "format") {
          field.get_ref<std::string &>() += value.get_ref<const std::string &>();
        } else {
          field = value;
        }
      }
    }

    bool sameMessage(const json &left, const json &right) {
      if (stringField(left, "role") != stringField(right, "role")) return false;
      const auto id = stringField(left, "replay_id");
      // The first echo binds the reference to the client's final display text.
      // Later echoes must also match that text. The client drops the reference
      // for edits made before the first echo reaches the backend.
      if (stringField(left, "role") == "assistant" && !id.empty())
        return id == stringField(right, "replay_id") &&
        (!right.contains("content") || left.at("content") == right.at("content"));
      return left == right;
    }
  }

  AssistantMessageAccumulator::AssistantMessageAccumulator(ApiConfig::ApiStyle style, bool allowToolCalls)
    : style_(style), allowToolCalls_(allowToolCalls) {}

  void AssistantMessageAccumulator::add(const json &event)
  {
    if (event.contains("error") || stringField(event, "type") == "error") {
      invalidate();
      return;
    }
    if (style_ == ApiConfig::ApiStyle::OpenAI) {
      const auto delta = inference_contant::firstChoiceContent(event, "delta");
      if (delta) mergeDelta(message_, *delta);
      const auto choices = event.find("choices");
      if (choices != event.end() && choices->is_array() && !choices->empty()) {
        const auto reason = stringField(choices->front(), "finish_reason");
        if (!reason.empty()) {
          stopReason_ = reason;
          complete_ = true;
          if (reason != "stop" && !(allowToolCalls_ && reason == "tool_calls")) invalidate();
        }
      }
      return;
    }

    const auto type = stringField(event, "type");
    if (type == "message_stop") {
      finish();
    } else if (type == "message_delta") {
      const auto delta = event.find("delta");
      const auto reason = delta == event.end() ? std::string{} : stringField(*delta, "stop_reason");
      if (!reason.empty() && reason != "end_turn" && reason != "stop_sequence") invalidate();
    } else if (type == "content_block_start" || type == "content_block_delta" || type == "content_block_stop") {
      const auto index = event.at("index").get<size_t>();
      if (type == "content_block_start") {
        if (blocks_.contains(index)) invalidate();
        openBlocks_.insert(index);
        blocks_[index] = event.at("content_block");
      } else if (type == "content_block_stop") {
        if (openBlocks_.erase(index) != 1) invalidate();
        const auto input = toolInputs_.find(index);
        if (input != toolInputs_.end()) blocks_.at(index)["input"] = json::parse(input->second);
      } else {
        if (!openBlocks_.contains(index)) {
          invalidate();
          return;
        }
        auto &block = blocks_.at(index);
        const auto &delta = event.at("delta");
        const auto deltaType = stringField(delta, "type");
        if (deltaType == "text_delta")
          block["text"] = stringField(block, "text") + stringField(delta, "text");
        else if (deltaType == "thinking_delta")
          block["thinking"] = stringField(block, "thinking") + stringField(delta, "thinking");
        else if (deltaType == "signature_delta")
          block["signature"] = stringField(block, "signature") + stringField(delta, "signature");
        else if (deltaType == "input_json_delta")
          toolInputs_[index] += stringField(delta, "partial_json");
        else if (deltaType == "citations_delta")
          block["citations"].push_back(delta.at("citation"));
        else
          invalidate(); // An unknown delta cannot safely be replayed as a complete block.
      }
    }
  }

  void AssistantMessageAccumulator::finish()
  {
    complete_ = true;
    if (!openBlocks_.empty()) invalidate();
  }
  void AssistantMessageAccumulator::invalidate() { valid_ = false; }

  json AssistantMessageAccumulator::message() const
  {
    if (!complete_ || !valid_) return nullptr;
    auto result = message_;
    if (result.contains("tool_calls") && !result["tool_calls"].empty()) {
      if (!allowToolCalls_ || stopReason_ != "tool_calls" || !result["tool_calls"].is_array()) return nullptr;
      for (auto &call : result["tool_calls"]) call.erase("index");
    }
    if (style_ == ApiConfig::ApiStyle::Anthropic) {
      result["content"] = json::array();
      for (const auto &[index, block] : blocks_) result["content"].push_back(block);
    }
    return result;
  }

  ReasoningHistory::Prepared ReasoningHistory::prepare(const std::string &sessionId, const std::string &responseId,
    const ApiConfig &provider, const json &messages)
  {
    Prepared result{ messages,{ sessionId, responseId, 0, provider } };
    for (auto &message : result.messages) message.erase("replay_id");
    if (sessionId.empty()) return result;

    std::lock_guard<std::mutex> lock(mutex_);
    auto found = sessions_.find(sessionId);
    if (found == sessions_.end()) {
      if (sessions_.size() >= kMaxSessions) {
        sessions_.erase(lru_.front());
        lru_.pop_front();
      }
      lru_.push_back(sessionId);
      found = sessions_.emplace(sessionId, Session{}).first;
      found->second.lru = std::prev(lru_.end());
    } else {
      lru_.splice(lru_.end(), lru_, found->second.lru);
    }
    auto &session = found->second;
    size_t common = 0;
    while (common < messages.size() && common < session.history.size() &&
      sameMessage(messages[common], session.history[common])) ++common;

    std::set<std::string> retained;
    size_t turns = 0;
    for (size_t i = messages.size(); i > 0; --i) {
      const auto &message = messages[i - 1];
      if (stringField(message, "role") != "assistant") continue;
      ++turns;
      const auto id = stringField(message, "replay_id");
      if (i > common || id.empty() || turns > ApiConfig::kMaxHistoryReplay) continue;
      retained.insert(id);
      if (turns > provider.historyReplay) continue;
      const auto response = session.responses.find(id);
      if (response == session.responses.end()) continue;
      const auto &stored = response->second;
      if (stored.providerId == provider.id && stored.apiUrl == provider.apiUrl &&
        stored.model == provider.model && stored.style == provider.apiStyle)
        result.messages[i - 1] = stored.message;
    }
    std::erase_if(session.responses, [&](const auto &entry) { return !retained.contains(entry.first); });
    session.history = messages;
    session.revision = ++nextRevision_;
    result.ticket.revision = session.revision;
    return result;
  }

  bool ReasoningHistory::remember(const Ticket &ticket, const json &message)
  {
    if (ticket.revision == 0 || ticket.responseId.empty() || ticket.provider.historyReplay == 0 ||
      !message.is_object() || stringField(message, "role") != "assistant" ||
      (message.contains("tool_calls") && !message["tool_calls"].empty())) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = sessions_.find(ticket.sessionId);
    if (found == sessions_.end() || found->second.revision != ticket.revision) return false;
    auto &session = found->second;
    if (session.responses.contains(ticket.responseId)) return false;
    session.responses[ticket.responseId] = { message, ticket.provider.id, ticket.provider.apiUrl,
      ticket.provider.model, ticket.provider.apiStyle };
    // Include the newly completed turn in the retention limit immediately.
    size_t retained = 1;
    for (size_t i = session.history.size(); i > 0; --i) {
      if (stringField(session.history[i - 1], "role") != "assistant") continue;
      if (++retained > ApiConfig::kMaxHistoryReplay)
        session.responses.erase(stringField(session.history[i - 1], "replay_id"));
    }
    session.history.push_back({ { "role", "assistant" },{ "replay_id", ticket.responseId } });
    return true;
  }

} // namespace reasoning_history
