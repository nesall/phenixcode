#include "inference_messages.h"

#include <iostream>
#include <stdexcept>

using nlohmann::json;

namespace {
  int checks = 0;

  void check(const std::string &name, const std::string &actual, const std::string &expected) {
    ++checks;
    if (actual != expected)
      throw std::runtime_error(name + ": expected " + json(expected).dump() + ", got " + json(actual).dump());
  }

  void checkMessage(const std::string &name, const json &message,
                    const std::string &answer, const std::string &reasoning) {
    ApiConfig cfg;
    for (bool streaming : {false, true}) {
      json response = {{"choices", json::array({{{streaming ? "delta" : "message", message}}})}};
      std::string extractedReasoning = "prior ";
      const auto extracted = streaming
        ? inference_contant::extractDeltaContent(cfg, response, extractedReasoning)
        : inference_contant::extractFullContent(cfg, response, extractedReasoning);
      const auto label = name + (streaming ? " delta" : " full");
      check(label + " answer", extracted, answer);
      check(label + " reasoning", extractedReasoning, "prior " + reasoning);
    }
  }
}

int main() {
  try {
    checkMessage("plain text", {{"content", "Answer"}}, "Answer", "");
    checkMessage("DeepSeek", {{"content", "Answer"}, {"reasoning_content", "Thinking"}}, "Answer", "Thinking");
    checkMessage("reasoning alias", {{"content", nullptr}, {"reasoning", "Thinking"}}, "", "Thinking");
    const json mistral = json::parse(R"({"content":[
      {"type":"thinking","thinking":[{"type":"text","text":"First "},{"type":"text","text":"thought."}]},
      {"type":"text","text":"First answer. "},
      {"type":"thinking","thinking":[{"type":"text","text":" Second thought."}]},
      {"type":"text","text":"Second answer."}
    ]})");
    checkMessage("Mistral interleaved chunks", mistral, "First answer. Second answer.", "First thought. Second thought.");
    checkMessage("string thinking", json::parse(R"({"content":[
      {"type":"thinking","thinking":"Thinking"},{"type":"text","text":"Answer"}
    ]})"), "Answer", "Thinking");
    checkMessage("reasoning block", json::parse(R"({"content":[
      {"type":"reasoning","thinking":"Thinking"},{"type":"reasoning","text":" more"}
    ]})"), "", "Thinking more");
    checkMessage("OpenRouter details", json::parse(R"({"content":"Answer","reasoning_details":[
      {"type":"reasoning.summary","summary":"Summary. "},
      {"type":"reasoning.text","text":"Thinking","signature":"opaque"},
      {"type":"reasoning.encrypted","data":"opaque"}
    ]})"), "Answer", "Summary. Thinking");
    auto duplicate = mistral;
    duplicate["reasoning_content"] = "Canonical";
    duplicate["reasoning"] = "Canonical";
    duplicate["reasoning_details"] = json::array({{{"type", "reasoning.text"}, {"text", "Canonical"}}});
    checkMessage("duplicate representations", duplicate, "First answer. Second answer.", "Canonical");
    checkMessage("empty alias fallback", {{"reasoning_content", ""}, {"reasoning", "Thinking"}}, "", "Thinking");
    checkMessage("null alias fallback", {{"reasoning_content", nullptr}, {"reasoning", "Thinking"}}, "", "Thinking");
    checkMessage("ignore non-text blocks", json::parse(R"({"content":[
      null,42,"untyped",{"type":null},{"type":"text","text":42},
      {"type":"image_url","text":"hidden"},{"type":"tool_use","text":"hidden"},
      {"type":"redacted_thinking","data":"hidden"},
      {"type":"thinking","thinking":[null,42,{"type":"image","text":"hidden"},{"type":"text","text":"Thinking"}]},
      {"type":"text","text":"Answer"}
    ],"reasoning_details":[null,42,{"type":null}]})"), "Answer", "Thinking");
    checkMessage("malformed fields", {{"content", 12}, {"reasoning_content", json::array()},
      {"reasoning", false}, {"reasoning_details", "invalid"}}, "", "");

    ApiConfig cfg;
    for (const auto &response : json::parse(R"([
      null,42,[],{}, {"choices":null},{"choices":{}},{"choices":[]},
      {"choices":[null]},{"choices":[42]},
      {"choices":[{"delta":null,"message":null}]},
      {"choices":[{"delta":[],"message":"invalid"}]}
    ])")) {
      std::string reasoning = "prior";
      check("empty/malformed full", inference_contant::extractFullContent(cfg, response, reasoning), "");
      check("empty/malformed delta", inference_contant::extractDeltaContent(cfg, response, reasoning), "");
      check("preserve accumulated reasoning", reasoning, "prior");
    }

    json multiple = {{"choices", json::array({
      {{"message", {{"content", "First"}}}, {"delta", {{"content", "First"}}}},
      {{"message", {{"content", "Second"}}}, {"delta", {{"content", "Second"}}}}
    })}};
    std::string reasoning;
    check("first choice full", inference_contant::extractFullContent(cfg, multiple, reasoning), "First");
    check("first choice delta", inference_contant::extractDeltaContent(cfg, multiple, reasoning), "First");

    std::string answer;
    reasoning.clear();
    for (const auto &delta : json::parse(R"([
      {"role":"assistant","content":null},
      {"content":[{"type":"thinking","thinking":[{"type":"text","text":"Think"}]}]},
      {"content":[{"type":"thinking","thinking":[{"type":"text","text":"ing"}]},{"type":"text","text":"An"}]},
      {"content":"swer"}, {}
    ])")) {
      const json chunk = {{"choices", json::array({{{"delta", delta}}})}};
      answer += inference_contant::extractDeltaContent(cfg, chunk, reasoning);
    }
    check("Mistral stream array/string transition", answer, "Answer");
    check("Mistral stream reasoning accumulation", reasoning, "Thinking");

    cfg.apiStyle = ApiConfig::ApiStyle::Anthropic;
    reasoning.clear();
    check("Anthropic full", inference_contant::extractFullContent(cfg, mistral, reasoning), "First answer. Second answer.");
    check("Anthropic full reasoning", reasoning, "First thought. Second thought.");
    answer.clear();
    reasoning.clear();
    for (const auto &chunk : json::parse(R"([
      {"type":"message_start"},
      {"type":"content_block_start","content_block":{"type":"thinking","thinking":"Think"}},
      {"type":"content_block_delta","delta":{"type":"thinking_delta","thinking":"ing"}},
      {"type":"content_block_delta","delta":{"type":"signature_delta","signature":"opaque"}},
      {"type":"content_block_start","content_block":{"type":"text","text":"An"}},
      {"type":"content_block_delta","delta":{"type":"text_delta","text":"swer"}},
      {"type":"content_block_delta","delta":{"type":"input_json_delta","partial_json":"opaque"}},
      {"type":"content_block_start","content_block":{"type":"redacted_thinking","data":"opaque"}},
      {"type":"content_block_delta","delta":null},
      {"type":null},null,42,{"type":"message_stop"}
    ])"))
      answer += inference_contant::extractDeltaContent(cfg, chunk, reasoning);
    check("Anthropic stream answer", answer, "Answer");
    check("Anthropic stream reasoning", reasoning, "Thinking");
    reasoning.clear();
    const json anthropic = json::parse(R"({"content":[
      {"type":"thinking","thinking":"Thinking","signature":"opaque"},
      {"type":"text","text":"Answer"},{"type":"redacted_thinking","data":"opaque"}
    ]})");
    check("Anthropic native full", inference_contant::extractFullContent(cfg, anthropic, reasoning), "Answer");
    check("Anthropic native full reasoning", reasoning, "Thinking");
    std::cout << checks << " extraction checks passed.\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
