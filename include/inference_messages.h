#pragma once

#include "settings.h"
#include <list>
#include <mutex>
#include <set>
#include <unordered_map>

// Shared parsing for complete messages and streamed deltas. Only readable text
// contributes to answers or reasoning token estimates, never opaque signatures.
namespace inference_contant {

  std::string stringField(const nlohmann::json &object, const char *name);
  void appendText(const nlohmann::json &value, std::string &text);
  void appendBlock(const nlohmann::json &block, std::string &answer, std::string &reasoning);
  std::string extractMessageContent(const nlohmann::json &message, std::string &reasoning);
  const nlohmann::json *firstChoiceContent(const nlohmann::json &response, const char *name);
  std::string extractDeltaContent(const ApiConfig &cfg, const nlohmann::json &chunk, std::string &reasoning);
  std::string extractFullContent(const ApiConfig &cfg, const nlohmann::json &response, std::string &reasoning);

} // namespace inference_contant

namespace reasoning_history {

  // Accumulates the provider's assistant message before any display filtering.
  class AssistantMessageAccumulator {
  public:
    explicit AssistantMessageAccumulator(ApiConfig::ApiStyle style);
    void add(const nlohmann::json &event);
    void finish();
    void invalidate();
    nlohmann::json message() const;

  private:
    ApiConfig::ApiStyle style_;
    nlohmann::json message_ = {{"role", "assistant"}};
    std::map<size_t, nlohmann::json> blocks_;
    std::map<size_t, std::string> toolInputs_;
    std::set<size_t> openBlocks_;
    bool complete_ = false;
    bool valid_ = true;
  };

  // App-owned, process-local cache. Tickets prevent an older concurrent request
  // from publishing a response after a newer request has changed the conversation.
  class ReasoningHistory {
  public:
    struct Ticket {
      std::string sessionId;
      std::string responseId;
      std::uint64_t revision = 0;
      ApiConfig provider;
    };

    struct Prepared {
      nlohmann::json messages;
      Ticket ticket;
    };

    Prepared prepare(const std::string &sessionId, const std::string &responseId,
                     const ApiConfig &provider, const nlohmann::json &messages);
    bool remember(const Ticket &ticket, const nlohmann::json &message);

  private:
    struct Response {
      nlohmann::json message;
      std::string providerId;
      std::string apiUrl;
      std::string model;
      ApiConfig::ApiStyle style;
    };
    struct Session {
      nlohmann::json history = nlohmann::json::array();
      std::unordered_map<std::string, Response> responses;
      std::uint64_t revision = 0;
      std::list<std::string>::iterator lru;
    };

    static constexpr size_t kMaxSessions = 512;
    std::mutex mutex_;
    std::uint64_t nextRevision_ = 0;
    std::list<std::string> lru_;
    std::unordered_map<std::string, Session> sessions_;
  };

} // namespace reasoning_history
