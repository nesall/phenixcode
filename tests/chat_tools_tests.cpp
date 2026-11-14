#include "chat_tools.h"
#include "inference_messages.h"
#include "sourceproc.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <3rdparty/utf8.h>

using nlohmann::json;
namespace fs = std::filesystem;
namespace {
  int checks = 0;
  void check(bool condition, const char *message) { ++checks; if (!condition) throw std::runtime_error(message); }
  json call(std::string id = "call_1", json args = {{"pattern", "sanitizeUtf8"}}, std::string name = "grep") {
    return {{"id", id}, {"type", "function"}, {"function", {{"name", name}, {"arguments", args.dump()}}}};
  }
  struct Fixture {
    fs::path directory = fs::temp_directory_path() / ("phenixcode-grep-test-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { fs::create_directory(directory); }
    ~Fixture() { std::error_code error; fs::remove_all(directory, error); }
  };
  json streamedToolMessage() {
    reasoning_history::AssistantMessageAccumulator accumulator(ApiConfig::ApiStyle::OpenAI, true);
    const auto events = json::parse(R"([
      {"choices":[{"delta":{"role":"assistant","reasoning_content":"Search first","tool_calls":[{"index":0,"id":"call_1","type":"function","function":{"name":"grep","arguments":"{\"pattern\":\"sanitize"}}]}}]},
      {"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"Utf8\"}"}}]},"finish_reason":"tool_calls"}]}
    ])");
    for (const auto &event : events) accumulator.add(event);
    const auto message = accumulator.message();
    check(message["tool_calls"][0] == call(), "fragmented function arguments are reassembled without transport indexes");
    check(message["reasoning_content"] == "Search first", "tool request keeps provider reasoning");
    reasoning_history::AssistantMessageAccumulator replayOnly(ApiConfig::ApiStyle::OpenAI);
    for (const auto &event : events) replayOnly.add(event);
    check(replayOnly.message().is_null(), "tool calls remain ineligible for standalone reasoning replay");
    reasoning_history::AssistantMessageAccumulator unfinished(ApiConfig::ApiStyle::OpenAI, true);
    unfinished.add(events[0]); unfinished.finish();
    check(unfinished.message().is_null(), "DONE without a tool-call finish reason cannot trigger execution");
    for (const auto &reason : {"length", "content_filter"}) {
      reasoning_history::AssistantMessageAccumulator truncated(ApiConfig::ApiStyle::OpenAI, true);
      truncated.add(events[0]);
      truncated.add({{"choices", json::array({{{"delta", json::object()}, {"finish_reason", reason}}})}});
      check(truncated.message().is_null(), "truncated or refused tool requests cannot execute");
    }
    return message;
  }
}
int main() {
  try {
    ApiConfig api; api.supportsTools = "on";
    check(chat_tools::enabled(true, api, false, false), "explicit tool support enables agent");
    check(!chat_tools::enabled(false, api, false, false), "global off disables agent");
    check(!chat_tools::enabled(true, api, true, false), "free chat cannot search code");
    check(!chat_tools::enabled(true, api, false, true), "attached-only cannot search code");
    api.apiStyle = ApiConfig::ApiStyle::Anthropic;
    check(!chat_tools::enabled(true, api, false, false), "Anthropic execution is deferred");
    api.apiStyle = ApiConfig::ApiStyle::OpenAI;
    for (const auto &support : {"auto", "off"}) { api.supportsTools = support;
      check(!chat_tools::enabled(true, api, false, false), "auto and off do not assume capability"); }

    Fixture fixture;
    const auto source = (fixture.directory / "source.cpp").generic_string();
    const auto other = (fixture.directory / "other.cpp").generic_string();
    { std::ofstream file(source); file << "int unrelated;\nvoid SanitizeUtf8() {}\nconst char *pattern = \"a.*b\";\n"; }
    { std::ofstream file(other); file << "void sanitizeUtf8OutsideScope() {}\n"; }
    ProvidersSettings providers;
    providers.updateFromConfig({{"embedding_providers", json::array({{{"id", "e"}}})},
      {"generation_providers", json::array({{{"id", "g"}}})}});
    auto settings = Settings::createBlank(providers);
    SourceProcessor processor(settings);
    std::vector<std::string> usedSources;
    int searches = 0;
    const auto search = [&](const std::string &pattern, const std::vector<std::string> &paths) {
      ++searches;
      check(paths == std::vector<std::string>{source}, "executor passes only allowed files");
      return processor.grep({pattern}, paths);
    };
    const auto execute = [&](const json &request) {
      return chat_tools::executeGrep(request, {{source}, usedSources}, search);
    };
    auto matches = execute(call());
    check(matches["matches"].size() == 1 && matches["matches"][0]["source"] == source, "real grep matches case-insensitively within scope");
    check(matches["matches"][0]["content"].get<std::string>().find("2: void SanitizeUtf8") != std::string::npos,
      "grep includes original line numbers and content");
    check(usedSources == std::vector<std::string>{source}, "returned sources are tracked");
    check(execute(call("literal", {{"pattern", "a.*b"}}))["matches"].size() == 1, "pattern punctuation is literal");
    int before = searches;
    check(execute(call("filtered", {{"pattern", "sanitizeUtf8"}, {"path_filter", "nonexistent/"}}))["matches"].empty(), "unmatched filter returns no files");
    check(searches == before, "empty scope never invokes grep's all-sources fallback");
    check(execute(call("filtered", {{"pattern", "sanitizeUtf8"}, {"path_filter", "source.cpp"}}))["matches"].size() == 1, "relative path prefix matches configured absolute paths");
    for (const auto &args : json::array({{{"pattern", "x"}}, {{"pattern", 42}}, {{"pattern", "sanitizeUtf8"}, {"path_filter", "../"}},
      {{"pattern", "sanitizeUtf8"}, {"unexpected", true}}}))
      check(execute(call("bad", args)).contains("error"), "invalid tool arguments return structured errors");
    auto malformed = call(); malformed["function"]["arguments"] = "{broken";
    check(execute(malformed).contains("error"), "malformed JSON arguments return an error");
    check(execute(call("unknown", json::object(), "shell")).contains("error"), "unknown tools do not execute");
    check(searches == before + 1, "invalid arguments never invoke search");
    auto bounded = chat_tools::executeGrep(call(), {{source}, usedSources}, [&](const auto &, const auto &) {
      std::vector<SearchResult> hits(20);
      for (auto &hit : hits) { hit.sourceId = source; hit.content = std::string(9000, '\n'); }
      return hits;
    });
    check(bounded.dump().size() <= chat_tools::kMaxResultBytes && bounded["truncated"] == true, "escaped JSON output is bounded");

    const auto assistant = streamedToolMessage();
    auto response = json{{"choices", json::array({{{"message", assistant}, {"finish_reason", "tool_calls"}}})}};
    check(inference_contant::completedAssistantMessage(api, response, true) == assistant, "non-streamed completed tool calls are preserved");
    check(inference_contant::completedAssistantMessage(api, response).is_null(), "non-streamed tool calls are not replayable alone");
    response["choices"][0]["finish_reason"] = "length";
    check(inference_contant::completedAssistantMessage(api, response, true).is_null(), "non-streamed truncated calls are discarded");
    const json messages = json::array({{{"role", "user"}, {"content", "Prepared question and context"}}});
    int rounds = 0;
    auto outcome = chat_tools::run(messages, [&](const json &transcript) -> chat_tools::Turn {
      if (++rounds == 1) return {"I will search first. This is not the answer.", assistant};
      check(transcript[0] == messages[0], "follow-up preserves prepared user context");
      check(transcript[1] == assistant && transcript[2]["role"] == "tool" && transcript[2]["tool_call_id"] == "call_1", "tool result follows its complete assistant request");
      check(json::parse(transcript[2]["content"].get<std::string>())["matches"].size() == 1, "model sees grep result");
      return {"Found it", {{"role", "assistant"}, {"content", "Found it"}}};
    }, execute, [] { return true; });
    check(rounds == 2 && outcome.text == "Found it" && !outcome.assistant.contains("tool_calls"), "tool-only round leads to final answer, not empty fallback");
    check(messages.size() == 1, "tool transcript is request-local");
    int unexpectedExecutions = 0;
    auto incomplete = chat_tools::run(messages, [](const auto &) -> chat_tools::Turn {
      return {"I will search now", nullptr};
    }, [&](const auto &) { ++unexpectedExecutions; return json::object(); }, [] { return true; });
    check(incomplete.incomplete && incomplete.text.empty() && unexpectedExecutions == 0,
      "invalid or truncated tool rounds never publish preambles as successful answers");
    incomplete = chat_tools::run(messages, [&](const auto &) -> chat_tools::Turn {
      return {"Partial", assistant, false, true};
    }, execute, [] { return true; });
    check(incomplete.incomplete && incomplete.text.empty(), "explicit incomplete status prevents execution even with a retained object");
    const auto countBytes = [](std::string_view text) { return text.size(); };
    auto largeTranscript = messages;
    largeTranscript.push_back(assistant);
    largeTranscript.push_back({{"role","tool"},{"tool_call_id","call_1"},{"content",std::string(8000,'x')}});
    auto latest = assistant;
    latest["tool_calls"][0]["id"] = "call_2";
    largeTranscript.push_back(latest);
    largeTranscript.push_back({{"role","tool"},{"tool_call_id","call_2"},{"content","new evidence"}});
    const auto originalTranscript = largeTranscript;
    check(chat_tools::fitToolResults(largeTranscript,1000,countBytes), "oversized tool transcripts fit final answer budget");
    check(largeTranscript[0] == messages[0] && largeTranscript[1] == assistant && largeTranscript[3] == latest &&
      largeTranscript[4] == originalTranscript[4] && largeTranscript[2]["tool_call_id"] == "call_1",
      "budget compaction preserves user context, call identities, and newest evidence");
    check(json::parse(largeTranscript[2]["content"].get<std::string>())["truncated"] == true,
      "shortened tool evidence is explicitly marked");
    auto tinyBudget = messages;
    check(!chat_tools::fitToolResults(tinyBudget,1,countBytes), "oversized user history is rejected without corrupting messages");
    check(outcome.text.find("search first") == std::string::npos, "intermediate prose never enters the final answer or saved history");

    auto multiple = assistant;
    multiple["tool_calls"].push_back(call("call_2", {{"pattern", "missing"}}));
    rounds = 0;
    chat_tools::run(messages, [&](const json &transcript) -> chat_tools::Turn {
      if (++rounds == 1) return {"", multiple};
      check(transcript.size() == 4 && transcript[3]["tool_call_id"] == "call_2", "multiple calls get matching result messages");
      return {"Done", {{"role", "assistant"}, {"content", "Done"}}};
    }, execute, [] { return true; });
    rounds = 0;
    auto limit = chat_tools::run(messages, [&](const auto &) -> chat_tools::Turn { ++rounds; return {"Searching, not a final answer", assistant}; }, execute, [] { return true; }, 2);
    check(rounds == 2 && limit.limitReached && limit.assistant.is_null() && limit.text.empty(), "step limit discards preambles and triggers the visible failure fallback");
    rounds = 0; bool connected = true;
    auto disconnected = chat_tools::run(messages, [&](const auto &) -> chat_tools::Turn { ++rounds; connected = false; return {"", assistant}; },
      [&](const auto &) -> json { throw std::runtime_error("must not execute after disconnect"); }, [&] { return connected; });
    check(rounds == 1 && disconnected.assistant.is_null(), "disconnect prevents execution and another generation");
    auto invalid = assistant; invalid["tool_calls"].push_back(invalid["tool_calls"][0]);
    bool rejected = false;
    try { chat_tools::run(messages, [&](const auto &) -> chat_tools::Turn { return {"", invalid}; }, execute, [] { return true; }); }
    catch (const std::runtime_error &) { rejected = true; }
    check(rejected, "duplicate call IDs are rejected before execution");

    std::vector<std::string> status;
    rounds = 0;
    auto failedCall = assistant;
    failedCall["tool_calls"][0]["function"]["arguments"] = "{malformed";
    auto recovered = chat_tools::run(messages, [&](const json &transcript) -> chat_tools::Turn {
      if (++rounds == 1) return {"Ignore this preamble", failedCall};
      check(json::parse(transcript.back()["content"].get<std::string>()).contains("error"), "model receives the tool error for recovery");
      return {"I could not complete that search", {{"role", "assistant"}, {"content", "I could not complete that search"}}};
    }, execute, [] { return true; }, 3, [&](std::string_view message) { status.emplace_back(message); });
    check(status.size() == 2 && status[0].find("Step 1/3: grep") != std::string::npos && status[1].find("grep failed:") == 0,
      "tool start and failure are exposed as progress independently of final prose");
    check(recovered.text == "I could not complete that search", "model can recover after a tool failure");

    const auto read = [&](json args) { return chat_tools::executeReadFile(call("read", args, "read_file"), {{source}, usedSources}); };
    auto excerpt = read({{"path", source}, {"start_line", 2}, {"end_line", 2}});
    check(excerpt["content"] == "2: void SanitizeUtf8() {}\n" && excerpt["start_line"] == 2 && excerpt["end_line"] == 2,
      "read_file returns the requested 1-based inclusive line range");
    check(excerpt["truncated"] == true && excerpt["next_start_line"] == 3, "read_file points to remaining file content");
    check(!read({{"path", "source.cpp"}}).contains("error"), "read_file resolves an unambiguous relative source path");
    for (const auto &args : json::array({{{"path", other}}, {{"path", "../other.cpp"}}, {{"path", source}, {"start_line", 0}},
      {{"path", source}, {"start_line", -2}}, {{"path", source}, {"start_line", 1.5}}, {{"path", source}, {"start_line", 99}},
      {{"path", source}, {"start_line", 2}, {"end_line", 1}}, {{"path", source}, {"extra", true}}}))
      check(read(args).contains("error"), "read_file rejects out-of-scope paths and invalid ranges");
    check(chat_tools::executeReadFile(call("read", {{"path", "source.cpp"}}, "read_file"),
      {{source, (fixture.directory / "nested/source.cpp").generic_string()}, usedSources}).contains("error"), "ambiguous relative paths are rejected");

    const auto manyLines = (fixture.directory / "many.txt").generic_string();
    { std::ofstream file(manyLines, std::ios::binary); for(int i=0;i<250;++i) file << "x\r\n"; }
    auto capped = chat_tools::executeReadFile(call("read", {{"path", manyLines}}, "read_file"), {{manyLines}, usedSources});
    check(capped["end_line"] == 200 && capped["next_start_line"] == 201 && capped["truncated"] == true,
      "read_file caps line counts and offers a continuation");
    check(capped["content"].get<std::string>().find('\r') == std::string::npos, "read_file normalizes CRLF in its preview");
    { std::ofstream file(manyLines); for(int i=0;i<15000;++i) file << "\xce\xb1"; }
    auto longLine = chat_tools::executeReadFile(call("read", {{"path", manyLines}}, "read_file"), {{manyLines}, usedSources});
    const auto snippet = longLine["content"].get<std::string>();
    check(longLine.dump().size() <= chat_tools::kMaxResultBytes && longLine["line_truncated"] == true &&
      utf8::is_valid(snippet.begin(), snippet.end()), "oversized Unicode lines produce a marked UTF-8-safe bounded prefix");
    { std::ofstream file(manyLines, std::ios::binary); file.write("a\0b", 3); }
    check(chat_tools::executeReadFile(call("read", {{"path", manyLines}}, "read_file"), {{manyLines}, usedSources}).contains("error"),
      "binary files are rejected");
    { std::ofstream file(manyLines, std::ios::binary); file << std::string(8 * 1024 * 1024 + 1, 'x'); }
    check(chat_tools::executeReadFile(call("read", {{"path", manyLines}}, "read_file"), {{manyLines}, usedSources}).contains("error"),
      "read_file bounds file input size");
    { std::ofstream file(manyLines); }
    auto empty = chat_tools::executeReadFile(call("read", {{"path", manyLines}}, "read_file"), {{manyLines}, usedSources});
    check(empty["content"] == "" && empty["start_line"] == 0 && empty["end_line"] == 0, "empty files have an explicit empty range");

    rounds = 0; status.clear();
    auto readAssistant = json{{"role", "assistant"}, {"tool_calls", json::array({call("read_1", {{"path", source}, {"start_line", 2}, {"end_line", 2}}, "read_file")})}};
    auto readOutcome = chat_tools::run(messages, [&](const json &transcript) -> chat_tools::Turn {
      if (++rounds == 1) return {"Searching...", assistant};
      if (rounds == 2) return {"Reading...", readAssistant};
      check(transcript.back()["tool_call_id"] == "read_1" &&
        json::parse(transcript.back()["content"].get<std::string>())["content"] == excerpt["content"], "model receives read_file contents with matching call ID");
      return {"Final answer", {{"role", "assistant"}, {"content", "Final answer"}}};
    }, [&](const json &request) {
      return request["function"]["name"] == "read_file" ? chat_tools::executeReadFile(request, {{source}, usedSources}) : execute(request);
    }, [] { return true; }, 5, [&](std::string_view message) { status.emplace_back(message); });
    check(readOutcome.text == "Final answer" && status.size() == 4, "grep and read_file expose four progress events but only one final answer");
    check(status[0].find("sanitizeUtf8") != std::string::npos && status[1].find("snippets returned") != std::string::npos &&
      status[3].find("read_file: returned lines 2-2") == 0, "progress includes arguments and result summaries");
    std::vector<std::string> listingSources{"D:/project/src/a.cpp", "D:/project/include/b.h", "D:/project/src/c.CPP"};
    std::vector<std::string> listingUsed;
    const chat_tools::ToolContext listingContext{listingSources, listingUsed};
    const auto list = [&](json args) {
      return chat_tools::executeListFiles(call("list_1", args, "list_files"), listingContext);
    };
    check(chat_tools::listFilesDefinition()["function"]["parameters"]["additionalProperties"] == false,
      "list_files schema rejects unknown arguments");
    auto listed = list(json::object());
    check(listed["count"] == 3 && listed["truncated"] == false, "canonical empty arguments list all allowed files");
    listed = list({{"extension", ".h"}});
    check(listed["files"] == json::array({listingSources[1]}), "canonical model calls apply extension filters");
    listed = list({{"path_filter", "./src/"}, {"extension", "cpp"}});
    check(listed["count"] == 2, "path and extension filters combine and extension matching ignores case");
    check(list({{"path_filter", "include"}, {"extension", ".cpp"}})["files"].empty(), "unmatched filters return a valid empty result");
    check(listingUsed.empty(), "listing names does not mark sources as read");
    for (const auto &args : json::array({json::array(), nullptr, {{"unexpected", true}}, {{"path_filter", 42}},
        {{"path_filter", "../src"}}, {{"path_filter", std::string(257,'x')}}, {{"extension", 42}}, {{"extension", "../cpp"}}}))
      check(list(args).contains("error"), "invalid list_files arguments return structured errors");
    auto badList = call("list_1", json::object(), "list_files");
    for (const auto &raw : {std::string("{broken"), std::string(4097,'x')}) {
      badList["function"]["arguments"] = raw;
      check(chat_tools::executeListFiles(badList, listingContext).contains("error"), "malformed or oversized serialized arguments are rejected");
    }
    badList["function"]["arguments"] = json::object();
    check(chat_tools::executeListFiles(badList, listingContext).contains("error"), "arguments must use the provider string format");
    check(chat_tools::executeListFiles(json::object(), listingContext).contains("error"), "missing function envelope is an error");
    check(chat_tools::executeListFiles(call(), listingContext).contains("error"), "wrong function name is rejected");
    listingSources.clear();
    for (size_t i=0;i<200;++i) listingSources.push_back("src/" + std::to_string(i) + ".cpp");
    listed = list(json::object());
    check(listed["count"] == 200 && listed["truncated"] == false, "exactly 200 files fit without false truncation");
    listingSources.push_back("src/extra.cpp");
    listed = list(json::object());
    check(listed["count"] == 200 && listed["truncated"] == true, "file count limit reports omitted results");
    listingSources.assign(200, "D:/project/" + std::string(100,'x') + ".cpp");
    listed = list(json::object());
    check(listed.dump().size() <= chat_tools::kMaxResultBytes && listed["count"] < 200 && listed["truncated"] == true,
      "long paths respect serialized byte budget independently of file count");
    listingSources.assign(200, std::string(100,'"') + "\xce\xb1.cpp");
    listed = list(json::object());
    check(listed.dump().size() <= chat_tools::kMaxResultBytes && listed["count"] == listed["files"].size(),
      "serialized limit includes JSON escaping and keeps whole Unicode paths");
    listingSources = {"src/a.cpp", "src/b.h"};
    rounds = 0; status.clear();
    const auto listAssistant = json{{"role", "assistant"}, {"tool_calls", json::array({
      call("list_1", {{"path_filter", "src/"}, {"extension", ".cpp"}}, "list_files")})}};
    chat_tools::run(messages, [&](const json &transcript) -> chat_tools::Turn {
      if (++rounds == 1) return {"Discovering files", listAssistant};
      check(transcript.back()["tool_call_id"] == "list_1" && json::parse(transcript.back()["content"].get<std::string>())["count"] == 1,
        "list_files result is forwarded to the model with its call identity");
      return {"Found the file", {{"role", "assistant"}, {"content", "Found the file"}}};
    }, [&](const json &request) { return chat_tools::executeListFiles(request, listingContext); }, [] { return true; }, 3,
      [&](std::string_view message) { status.emplace_back(message); });
    check(status.size() == 2 && status[0].find("src/") != std::string::npos && status[0].find(".cpp") != std::string::npos &&
      status[1] == "list_files: 1 files returned", "list_files progress includes filters and result count");
    check(chat_tools::searchContextDefinition()["function"]["parameters"]["required"] == json::array({"query"}),
      "search_context advertises a required natural-language query");
    std::vector<std::string> contextFiles{"D:/project/src/a.cpp", "D:/project/src/b.cpp"}, contextUsed;
    const chat_tools::ToolContext context{contextFiles, contextUsed};
    int retrievals = 0;
    SearchResult indexed;
    indexed.sourceId = "D:\\project\\src\\a.cpp";
    indexed.content = "rollback coordination";
    indexed.chunkUnit = "line";
    indexed.start = 4; indexed.end = 9;
    SearchResult bytes = indexed;
    bytes.sourceId = contextFiles[1]; bytes.chunkUnit = "char"; bytes.start = 42; bytes.end = 99;
    SearchResult stale = indexed;
    stale.sourceId = "D:/excluded/secret.cpp";
    const auto retrieveContext = [&](const std::string &query) -> chat_tools::ContextSearchResult {
      ++retrievals;
      check(query == "how does rollback coordination work?", "natural-language query reaches existing retrieval unchanged");
      return {{stale, indexed, bytes}, false};
    };
    auto searchCall = call("context_1", {{"query", "how does rollback coordination work?"}}, "search_context");
    auto contextResult = chat_tools::executeSearchContext(searchCall, context, retrieveContext);
    check(retrievals == 1 && contextResult["mode"] == "hybrid" && contextResult["matches"].size() == 2,
      "hybrid search filters stale/out-of-scope index entries");
    check(contextResult["matches"][0]["source"] == contextFiles[0] && contextUsed == contextFiles,
      "path normalization preserves current source identities and tracks delivered sources");
    check(contextResult["matches"][0]["start_line"] == 5 && contextResult["matches"][0]["end_line"] == 9,
      "indexed zero-based end-exclusive lines become 1-based inclusive tool ranges");
    check(contextResult["matches"][1]["start_byte"] == 42 && contextResult["matches"][1]["end_byte"] == 99 &&
      !contextResult["matches"][1].contains("start_line"), "UTF-8 byte offsets are never misrepresented as line numbers");
    indexed.start = 5;
    auto fallbackContext = [&](const std::string &) -> chat_tools::ContextSearchResult { return {{indexed}, true}; };
    contextResult = chat_tools::executeSearchContext(searchCall, context, fallbackContext);
    check(contextResult["mode"] == "text" && contextResult["matches"][0]["start_line"] == 5 && contextUsed.size() == 2,
      "text fallback preserves grep line numbering and source tracking is unique");
    const auto beforeRetrievals = retrievals;
    for (const auto &args : json::array({json::object(), json::array(), nullptr, {{"query", ""}}, {{"query", " \t\n"}},
        {{"query", std::string(513,'x')}}, {{"query", std::string("a\0b",3)}}, {{"query", 42}}, {{"query", "q"}, {"extra", true}}}))
      check(chat_tools::executeSearchContext(call("c", args, "search_context"), context, retrieveContext).contains("error"),
        "invalid context queries fail before invoking retrieval");
    auto badContextCall = searchCall;
    badContextCall["function"]["arguments"] = "{broken";
    check(chat_tools::executeSearchContext(badContextCall, context, retrieveContext).contains("error"), "malformed context arguments are structured errors");
    badContextCall["function"]["arguments"] = std::string(4097,'x');
    check(chat_tools::executeSearchContext(badContextCall, context, retrieveContext).contains("error"), "context argument envelope is bounded");
    check(chat_tools::executeSearchContext(call(), context, retrieveContext).contains("error") && retrievals == beforeRetrievals,
      "wrong function cannot trigger retrieval");
    const std::vector<std::string> emptyScope;
    contextResult = chat_tools::executeSearchContext(searchCall, {emptyScope, contextUsed}, retrieveContext);
    check(contextResult["matches"].empty() && contextResult["mode"] == "none" && retrievals == beforeRetrievals,
      "empty scope does not invoke embeddings or global retrieval");
    contextUsed.clear();
    contextResult = chat_tools::executeSearchContext(searchCall, context, [](const auto &) -> chat_tools::ContextSearchResult {
      throw std::runtime_error("retrieval failed");
    });
    check(contextResult.contains("error") && contextUsed.empty(), "retrieval failures become tool errors without source citations");
    auto retrieveMany = [&](const std::string &) -> chat_tools::ContextSearchResult {
      return {std::vector<SearchResult>(10,indexed), false};
    };
    contextResult = chat_tools::executeSearchContext(searchCall, context, retrieveMany);
    check(contextResult["matches"].size() == 8 && contextResult["truncated"] == true, "context search caps the number of excerpts");
    indexed.content = std::string(2047,'a') + "\xce\xb1";
    contextResult = chat_tools::executeSearchContext(searchCall, context, fallbackContext);
    const auto content = contextResult["matches"][0]["content"].get<std::string>();
    check(contextResult["matches"][0]["truncated"] == true && content.size() == 2047 && utf8::is_valid(content.begin(),content.end()),
      "excerpt truncation preserves UTF-8 boundaries");
    indexed.content = std::string(3000,'\n');
    contextResult = chat_tools::executeSearchContext(searchCall, context, retrieveMany);
    check(contextResult.dump().size() <= chat_tools::kMaxResultBytes && contextResult["truncated"] == true &&
      contextResult["matches"].size() < 8, "context result limit includes JSON escaping");
    indexed.content = "retrieved evidence";
    rounds = 0; status.clear();
    const json contextAssistant = {{"role", "assistant"}, {"tool_calls", json::array({searchCall})}};
    chat_tools::run(messages, [&](const json &transcript) -> chat_tools::Turn {
      if (++rounds == 1) return {"Investigating", contextAssistant};
      check(transcript.back()["tool_call_id"] == "context_1" && json::parse(transcript.back()["content"].get<std::string>())["mode"] == "text",
        "context result is available to the next model round with its matching call ID");
      return {"Answer from evidence", {{"role", "assistant"}, {"content", "Answer from evidence"}}};
    }, [&](const json &request) { return chat_tools::executeSearchContext(request, context, fallbackContext); }, [] { return true; }, 3,
      [&](std::string_view message) { status.emplace_back(message); });
    check(status.size() == 2 && status[0].find("rollback coordination") != std::string::npos &&
      status[1] == "search_context: 1 excerpts returned (text)", "context search progress exposes query, count and fallback mode");
    std::cout << checks << " tool checks passed.\n";
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
