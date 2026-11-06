#include "inference_messages.h"
#include <iostream>
#include <stdexcept>

using nlohmann::json;

namespace {
  int checks = 0;
  void check(bool condition, const char *label) {
    ++checks;
    if (!condition) throw std::runtime_error(label);
  }
  json user(const std::string &text) { return {{"role", "user"}, {"content", text}}; }
  json assistant(const std::string &id, const std::string &text = "Displayed answer with sources") {
    return {{"role", "assistant"}, {"content", text}, {"replay_id", id}};
  }
  const json native = {{"role", "assistant"}, {"content", "Answer"}, {"reasoning_content", "Private thinking"}};

  void cacheTests() {
    ApiConfig cfg;
    cfg.id = "provider";
    cfg.apiUrl = "http://local/chat/completions";
    cfg.model = "model";
    check(cfg.historyReplay == 0, "replay defaults to off");
    cfg.historyReplay = 3;
    reasoning_history::ReasoningHistory cache;
    auto first = cache.prepare("chat", "a1", cfg, json::array({user("Question")}));
    check(cache.remember(first.ticket, native), "retain complete response");
    check(!cache.remember(first.ticket, native), "duplicate completion rejected");
    json history = json::array({user("Question"), assistant("a1"), user("Follow up")});
    auto second = cache.prepare("chat", "a2", cfg, history);
    check(second.messages[1] == native, "fetch exact complete message despite display formatting");
    check(!second.messages[1].contains("replay_id"), "internal references removed from provider body");
    check(cache.remember(second.ticket, native), "retain second response");

    auto edited = history;
    edited[0]["content"] = "Edited question";
    edited.push_back(assistant("a2"));
    edited.push_back(user("Next"));
    auto afterEdit = cache.prepare("chat", "a3", cfg, edited);
    check(!afterEdit.messages[1].contains("reasoning_content") &&
          !afterEdit.messages[3].contains("reasoning_content"), "user edit invalidates entire suffix");
    auto restoredText = cache.prepare("chat", "a4", cfg, history);
    check(!restoredText.messages[1].contains("reasoning_content"), "restoring old text cannot revive invalidated reasoning");

    auto assistantSeed = cache.prepare("assistant-edit", "reply", cfg, json::array({user("Q")}));
    check(cache.remember(assistantSeed.ticket, native), "seed assistant edit");
    auto assistantHistory = json::array({user("Q"), assistant("reply"), user("Next")});
    cache.prepare("assistant-edit", "next", cfg, assistantHistory);
    assistantHistory[1]["content"] = "Edited answer";
    auto assistantEdited = cache.prepare("assistant-edit", "next", cfg, assistantHistory);
    check(!assistantEdited.messages[1].contains("reasoning_content"), "assistant edit invalidates reasoning even with stale reference");

    auto seed = cache.prepare("retry", "old", cfg, json::array({user("Retry")}));
    check(cache.remember(seed.ticket, native), "seed retry");
    auto retry = cache.prepare("retry", "new", cfg, json::array({user("Retry")}));
    check(cache.remember(retry.ticket, native), "retry stores replacement");
    auto oldReference = cache.prepare("retry", "later", cfg,
      json::array({user("Retry"), assistant("old"), user("Next")}));
    check(!oldReference.messages[1].contains("reasoning_content"), "retry invalidates previous response reference");

    auto concurrent1 = cache.prepare("concurrent", "older", cfg, json::array({user("Old")}));
    auto concurrent2 = cache.prepare("concurrent", "newer", cfg, json::array({user("New")}));
    check(!cache.remember(concurrent1.ticket, native), "stale concurrent completion rejected");
    check(cache.remember(concurrent2.ticket, native), "current concurrent completion retained");

    auto providerSeed = cache.prepare("providers", "provider-a", cfg, json::array({user("Question")}));
    check(cache.remember(providerSeed.ticket, native), "seed provider compatibility");
    const auto providerHistory = json::array({user("Question"), assistant("provider-a"), user("Next")});
    for (int changed = 0; changed < 4; ++changed) {
      auto other = cfg;
      if (changed == 0) other.id = "other";
      if (changed == 1) other.model = "other";
      if (changed == 2) other.apiUrl = "http://other";
      if (changed == 3) other.apiStyle = ApiConfig::ApiStyle::Anthropic;
      auto prepared = cache.prepare("providers", "next", other, providerHistory);
      check(!prepared.messages[1].contains("reasoning_content"), "incompatible provider receives plain text");
    }
    auto compatible = cache.prepare("providers", "next", cfg, providerHistory);
    check(compatible.messages[1] == native, "switching back restores compatible replay");
    auto off = cfg;
    off.historyReplay = 0;
    auto disabled = cache.prepare("providers", "off", off, providerHistory);
    check(!disabled.messages[1].contains("reasoning_content"), "off sends plain text");
    check(!cache.remember(disabled.ticket, native), "off does not retain new responses");

    auto noSession = cache.prepare("", "id", cfg, json::array({user("Question")}));
    check(!cache.remember(noSession.ticket, native), "no session means no shared cache entry");
    auto missing = cache.prepare("unknown", "next", cfg, providerHistory);
    check(!missing.messages[1].contains("reasoning_content"), "missing cache falls back to plain text");
    auto failed = cache.prepare("failure", "failed", cfg, json::array({user("Question")}));
    check(!cache.remember(failed.ticket, nullptr), "failed response cannot be retained");

    reasoning_history::ReasoningHistory window;
    json turns = json::array();
    cfg.historyReplay = ApiConfig::kMaxHistoryReplay;
    for (int i = 0; i < 260; ++i) {
      turns.push_back(user(std::to_string(i)));
      const auto id = "reply-" + std::to_string(i);
      auto prepared = window.prepare("long", id, cfg, turns);
      check(window.remember(prepared.ticket, native), "retain long history");
      turns.push_back(assistant(id));
    }
    turns.push_back(user("Next"));
    auto all = window.prepare("long", "all", cfg, turns);
    size_t replayed = 0;
    for (const auto &message : all.messages) if (message.contains("reasoning_content")) ++replayed;
    check(replayed == 255, "all capped at 255 assistant turns");
    cfg.historyReplay = 3;
    auto lastThree = window.prepare("long", "three", cfg, turns);
    replayed = 0;
    for (const auto &message : lastThree.messages) if (message.contains("reasoning_content")) ++replayed;
    check(replayed == 3, "last three counts assistant turns");
    cfg.historyReplay = 5;
    auto lastFive = window.prepare("long", "five", cfg, turns);
    replayed = 0;
    for (const auto &message : lastFive.messages) if (message.contains("reasoning_content")) ++replayed;
    check(replayed == 5, "last five independent of previous replay window");

    reasoning_history::ReasoningHistory lru;
    auto evicted = lru.prepare("evicted", "id", cfg, json::array({user("Q")}));
    for (int i = 0; i < 512; ++i)
      lru.prepare(std::to_string(i), "id", cfg, json::array({user("Q")}));
    check(!lru.remember(evicted.ticket, native), "session eviction rejects old ticket");
    auto recreated = lru.prepare("evicted", "new", cfg, json::array({user("Q")}));
    check(!lru.remember(evicted.ticket, native), "recreated session rejects old revision");
    check(lru.remember(recreated.ticket, native), "recreated session accepts current revision");
  }

  void accumulatorTests() {
    reasoning_history::AssistantMessageAccumulator openai(ApiConfig::ApiStyle::OpenAI);
    for (const auto &delta : json::parse(R"([
      {"role":"assistant","content":null,"reasoning_content":"Private "},
      {"reasoning_content":"thinking","content":"An"}, {"content":"swer"}
    ])")) openai.add({{"choices", json::array({{{"delta", delta}}})}});
    check(openai.message().is_null(), "unfinished stream not replayable");
    openai.finish();
    check(openai.message() == native, "assemble OpenAI-compatible message");

    reasoning_history::AssistantMessageAccumulator mistral(ApiConfig::ApiStyle::OpenAI);
    for (const auto &delta : json::parse(R"([
      {"content":[{"type":"thinking","thinking":[{"type":"text","text":"Thinking"}]}]},
      {"content":[{"type":"text","text":"An"}]},{"content":"swer"}
    ])")) mistral.add({{"choices", json::array({{{"delta", delta}}})}});
    mistral.finish();
    std::string reasoning;
    const auto answer = inference_contant::extractMessageContent(mistral.message(), reasoning);
    check(answer == "Answer" && reasoning == "Thinking", "Mistral array/string stream assembled without losing thinking");

    reasoning_history::AssistantMessageAccumulator details(ApiConfig::ApiStyle::OpenAI);
    for (const auto &delta : json::parse(R"([
      {"reasoning_details":[{"index":0,"type":"reasoning.text","text":"Think","signature":"sig"}]},
      {"reasoning_details":[{"index":0,"type":"reasoning.text","text":"ing","signature":"nature"}]},
      {"reasoning_details":[{"index":1,"type":"reasoning.encrypted","data":"opaque"}]}
    ])")) details.add({{"choices", json::array({{{"delta", delta}}})}});
    details.finish();
    check(details.message()["reasoning_details"][0]["text"] == "Thinking", "indexed reasoning merged");
    check(details.message()["reasoning_details"][0]["signature"] == "signature", "signature fragments preserved");
    check(details.message()["reasoning_details"][1]["data"] == "opaque", "encrypted reasoning preserved");

    reasoning_history::AssistantMessageAccumulator anthropic(ApiConfig::ApiStyle::Anthropic);
    for (const auto &event : json::parse(R"([
      {"type":"content_block_start","index":0,"content_block":{"type":"thinking","thinking":"","signature":""}},
      {"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"Thinking"}},
      {"type":"content_block_delta","index":0,"delta":{"type":"signature_delta","signature":"sig"}},
      {"type":"content_block_delta","index":0,"delta":{"type":"signature_delta","signature":"nature"}},
      {"type":"content_block_stop","index":0},
      {"type":"content_block_start","index":1,"content_block":{"type":"text","text":"Answer"}},
      {"type":"content_block_stop","index":1},{"type":"message_stop"}
    ])")) anthropic.add(event);
    const auto expected = json::parse(R"({"role":"assistant","content":[
      {"type":"thinking","thinking":"Thinking","signature":"signature"},{"type":"text","text":"Answer"}
    ]})");
    check(anthropic.message() == expected, "Anthropic retains signed thinking blocks in order");

    reasoning_history::AssistantMessageAccumulator truncated(ApiConfig::ApiStyle::OpenAI);
    truncated.add({{"choices", json::array({{{"delta", {{"content", "Partial"}}}, {"finish_reason", "length"}}})}});
    truncated.finish();
    check(truncated.message().is_null(), "truncated reasoning cannot be replayed");
    reasoning_history::AssistantMessageAccumulator error(ApiConfig::ApiStyle::OpenAI);
    error.add({{"error", "failure"}});
    error.finish();
    check(error.message().is_null(), "provider errors cannot be replayed");
    reasoning_history::AssistantMessageAccumulator unclosed(ApiConfig::ApiStyle::Anthropic);
    unclosed.add({{"type", "content_block_start"}, {"index", 0}, {"content_block", {{"type", "thinking"}}}});
    unclosed.finish();
    check(unclosed.message().is_null(), "unclosed thinking block cannot be replayed");
  }
}

int main() {
  try {
    cacheTests();
    accumulatorTests();
    std::cout << checks << " reasoning history checks passed.\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
