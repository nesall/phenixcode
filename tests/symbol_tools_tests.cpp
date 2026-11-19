#include "chat_tools.h"
#include "symbol_service.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using nlohmann::json;
namespace fs = std::filesystem;

namespace {
  int checks = 0;

  void check(bool condition, const char *message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
  }

  json call(std::string id, json args, std::string name) {
    return {{"id", std::move(id)}, {"type", "function"}, {"function", {{"name", std::move(name)}, {"arguments", args.dump()}}}};
  }

  struct Fixture {
    fs::path directory = fs::temp_directory_path() / ("phenixcode-sym-tools-test-" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { fs::create_directory(directory); }
    ~Fixture() { std::error_code error; fs::remove_all(directory, error); }
  };
}

int main(int argc, char *argv[]) {
  try {
    const fs::path repoRoot = fs::weakly_canonical(argc > 1 ? fs::path(argv[1]) : fs::current_path());

    check(chat_tools::symbolOutlineDefinition()["function"]["parameters"]["additionalProperties"] == false,
      "symbol_outline schema rejects unknown arguments");
    check(chat_tools::readSymbolDefinition()["function"]["parameters"]["additionalProperties"] == false,
      "read_symbol schema rejects unknown arguments");
    check(chat_tools::findSymbolDefinition()["function"]["parameters"]["additionalProperties"] == false,
      "find_symbol schema rejects unknown arguments");

    Fixture fixture;
    const auto symHeader = (fixture.directory / "sym_api.h").generic_string();
    const auto symImpl = (fixture.directory / "sym_api.cpp").generic_string();
    const auto symCFile = (fixture.directory / "legacy.c").generic_string();
    const auto symPyFile = (fixture.directory / "worker.py").generic_string();
    const auto symTsFile = (fixture.directory / "api.ts").generic_string();
    const auto symSvelteFile = (fixture.directory / "Widget.svelte").generic_string();
    const auto symBig = (fixture.directory / "many_syms.cpp").generic_string();
    {
      std::ofstream f(symHeader);
      f << "#pragma once\n"
           "namespace demo {\n"
           "  int compute(int a);\n"
           "  int compute(double a);\n"
           "}\n";
    }
    {
      std::ofstream f(symImpl);
      f << "namespace demo {\n"
           "  int helper();\n"
           "  int compute(int a) { return a + 1; }\n"
           "  int compute(double a) { return static_cast<int>(a); }\n"
           "  int helper() { return compute(1); }\n"
           "}\n"
           "void oneLineA() { int x = 1; } void oneLineB() { int y = 2; }\n"
           "void giantFn() {\n";
      for (int i = 0; i < 250; ++i) f << "  int v" << i << " = " << i << ";\n";
      f << "}\n";
    }
    { std::ofstream f(symCFile); f << "int c_func(int x);\nint c_func(int x) { return x + 1; }\n"; }
    {
      std::ofstream f(symPyFile);
      f << "@cached\n"
           "def py_greet(name: str) -> str:\n"
           "    return name\n"
           "class Worker:\n"
           "    def run(self) -> int:\n"
           "        return 42\n";
    }
    {
      std::ofstream f(symTsFile);
      f << "export interface ClientCfg { host: string; }\n"
           "export const createHandler = (port: number) => `${port}`;\n";
    }
    { std::ofstream f(symSvelteFile); f << "<script>export let title = 'x';</script><h1>{title}</h1>\n"; }
    {
      std::ofstream f(symBig);
      for (int i = 0; i < 120; ++i) f << "void generatedSymbolNumber_" << i << "_with_long_suffix_padding() {}\n";
    }

    symbol_service::SymbolService symService;
    std::vector<std::string> symUsed;
    const std::vector<std::string> symAllowed{symHeader, symImpl, symCFile, symPyFile, symTsFile, symSvelteFile, symBig};

    // 1. .h defaults to C++ and symbol_outline shows declarations + tracks usedSources.
    auto hdrOutline = chat_tools::executeSymbolOutline(
      call("so_h", {{"path", "sym_api.h"}}, "symbol_outline"), {symAllowed, symUsed}, &symService);
    check(!hdrOutline.contains("error") && hdrOutline["language"] == "cpp" &&
          hdrOutline["symbol_count"] == 3 && hdrOutline["truncated"] == false &&
          hdrOutline["symbols"][1]["role"] == "declaration" &&
          hdrOutline["symbols"][2]["role"] == "declaration" &&
          symUsed == std::vector<std::string>{symHeader},
          ".h defaults to C++ and symbol_outline returns declaration roles and tracks usedSources");

    // 2. Cache hits must still enforce request-local source allowlist.
    std::vector<std::string> narrowedUsed;
    auto blockedCached = chat_tools::executeSymbolOutline(
      call("so_denied", {{"path", "sym_api.h"}}, "symbol_outline"), {{symImpl}, narrowedUsed}, &symService);
    check(blockedCached.contains("error") && narrowedUsed.empty(),
      "allowlist blocks symbol_outline on cached file outside current request scope");
    check(chat_tools::executeReadSymbol(
      call("rs_denied", {{"path", "sym_api.h"}, {"symbol", "compute"}}, "read_symbol"), {{symImpl}, narrowedUsed}, &symService).contains("error") &&
      narrowedUsed.empty(),
      "allowlist blocks read_symbol on cached file outside current request scope");

    // 3. .c routes to C, while .svelte remains unsupported.
    auto cOutline = chat_tools::executeSymbolOutline(
      call("so_c", {{"path", "legacy.c"}}, "symbol_outline"), {symAllowed, narrowedUsed}, &symService);
    check(!cOutline.contains("error") && cOutline["language"] == "c" && cOutline["symbol_count"] == 2,
      ".c outlines via tree-sitter-c with language=c");
    check(chat_tools::executeSymbolOutline(
      call("so_svelte", {{"path", "Widget.svelte"}}, "symbol_outline"), {symAllowed, narrowedUsed}, &symService).contains("error"),
      ".svelte remains unsupported for symbol_outline");
    narrowedUsed.clear();

    // 4. Declaration vs definition filtering.
    check(chat_tools::executeReadSymbol(
      call("rs_hdef", {{"path", "sym_api.h"}, {"symbol", "compute"}, {"role", "definition"}}, "read_symbol"),
      {symAllowed, narrowedUsed}, &symService).contains("error"),
      "role=definition on header declarations fails clearly without matching definitions");

    auto cppOutline = chat_tools::executeSymbolOutline(
      call("so_cpp", {{"path", "sym_api.cpp"}, {"role", "definition"}}, "symbol_outline"),
      {symAllowed, symUsed}, &symService);
    check(!cppOutline.contains("error") && cppOutline["symbol_count"] >= 6,
      "symbol_outline filters to definitions in cpp");

    // helper has declaration (line 2), definition (line 5), and call sites.
    auto helperAmbig = chat_tools::executeReadSymbol(
      call("rs_helper_ambig", {{"path", "sym_api.cpp"}, {"symbol", "helper"}}, "read_symbol"),
      {symAllowed, narrowedUsed}, &symService);
    check(helperAmbig["ambiguous"] == true && helperAmbig["candidate_count"] == 2 && narrowedUsed.empty(),
      "same-file declaration + definition returns candidates without polluting usedSources");

    auto helperDef = chat_tools::executeReadSymbol(
      call("rs_helper_def", {{"path", "sym_api.cpp"}, {"symbol", "helper"}, {"role", "definition"}}, "read_symbol"),
      {symAllowed, narrowedUsed}, &symService);
    check(!helperDef.contains("error") && !helperDef.value("ambiguous", false) &&
          helperDef["role"] == "definition" && helperDef["start_line"] == 5 &&
          helperDef["content"].get<std::string>().find("return compute(1);") != std::string::npos &&
          narrowedUsed == std::vector<std::string>{symImpl},
          "role=definition selects the exact implementation");

    // 5. Overloads return candidates and selector resolves exact overload.
    auto ambigCompute = chat_tools::executeReadSymbol(
      call("rs_over", {{"path", "sym_api.cpp"}, {"symbol", "demo::compute"}}, "read_symbol"),
      {symAllowed, narrowedUsed}, &symService);
    check(ambigCompute["ambiguous"] == true && ambigCompute["candidate_count"] == 2 &&
          ambigCompute["candidates"][0]["selector"] != ambigCompute["candidates"][1]["selector"],
      "overloaded name returns structured candidates with distinct selectors");

    const std::string doubleSelector = ambigCompute["candidates"][1]["selector"].get<std::string>();
    auto pickedOverload = chat_tools::executeReadSymbol(
      call("rs_sel", {{"path", "sym_api.cpp"}, {"symbol", doubleSelector}}, "read_symbol"),
      {symAllowed, narrowedUsed}, &symService);
    check(!pickedOverload.contains("error") &&
          pickedOverload["content"].get<std::string>().find("static_cast<int>(a)") != std::string::npos &&
          pickedOverload["content"].get<std::string>().find("a + 1") == std::string::npos,
      "selector reads the exact chosen overload");

    // 6. Two definitions on one line slice exact half-open byte ranges.
    auto sameLineB = chat_tools::executeReadSymbol(
      call("rs_sameline", {{"path", "sym_api.cpp"}, {"symbol", "oneLineB"}}, "read_symbol"),
      {symAllowed, narrowedUsed}, &symService);
    check(sameLineB["start_line"] == 7 && sameLineB["end_line"] == 7 &&
          sameLineB["content"] == "7: void oneLineB() { int y = 2; }\n" &&
          sameLineB["content"].get<std::string>().find("oneLineA") == std::string::npos,
      "same-line symbol slices exact byte range without leaking preceding definition on the same line");

    // 7. Giant function and huge outline enforce output caps & pagination.
    auto giantRead = chat_tools::executeReadSymbol(
      call("rs_giant", {{"path", "sym_api.cpp"}, {"symbol", "giantFn"}}, "read_symbol"),
      {symAllowed, narrowedUsed}, &symService);
    check(giantRead["truncated"] == true && giantRead.dump().size() <= chat_tools::kMaxResultBytes &&
          giantRead["end_line"].get<size_t>() < giantRead["start_line"].get<size_t>() + 200 &&
          giantRead.contains("next_start_line"),
      "read_symbol enforces output limits and reports next_start_line");

    auto bigOutlinePage1 = chat_tools::executeSymbolOutline(
      call("so_big1", {{"path", "many_syms.cpp"}}, "symbol_outline"),
      {symAllowed, narrowedUsed}, &symService);
    check(bigOutlinePage1["truncated"] == true && bigOutlinePage1.dump().size() <= chat_tools::kMaxResultBytes &&
          bigOutlinePage1.contains("next_offset") && bigOutlinePage1["total_symbols"] == 120,
      "symbol_outline enforces 8 KiB budget and exposes next_offset for pagination");

    auto bigOutlinePage2 = chat_tools::executeSymbolOutline(
      call("so_big2", {{"path", "many_syms.cpp"}, {"offset", bigOutlinePage1["next_offset"]}, {"limit", 5}}, "symbol_outline"),
      {symAllowed, narrowedUsed}, &symService);
    check(bigOutlinePage2["symbol_count"] == 5 && bigOutlinePage2["truncated"] == true,
      "symbol_outline pagination with offset and limit returns next slice");

    // 8. PhenixCode repo fixtures via chat_tools executors & SSE run loop.
    const std::string repoH = (repoRoot / "include" / "chat_tools.h").generic_string();
    const std::string repoCpp = (repoRoot / "src" / "chat_tools.cpp").generic_string();
    std::vector<std::string> repoUsed;
    const std::vector<std::string> repoScope{repoH, repoCpp};

    auto repoDecl = chat_tools::executeReadSymbol(
      call("rs_repoh", {{"path", repoH}, {"symbol", "executeGrepWithOptions"}, {"role", "declaration"}}, "read_symbol"),
      {repoScope, repoUsed}, &symService);
    check(!repoDecl.contains("error") && repoDecl["role"] == "declaration" &&
          repoDecl["content"].get<std::string>().find("executeGrepWithOptions") != std::string::npos,
      "read_symbol resolves executeGrepWithOptions declaration in include/chat_tools.h");

    auto repoDef = chat_tools::executeReadSymbol(
      call("rs_repocpp", {{"path", repoCpp}, {"symbol", "executeGrepWithOptions"}, {"role", "definition"}}, "read_symbol"),
      {repoScope, repoUsed}, &symService);
    check(!repoDef.contains("error") && repoDef["role"] == "definition" &&
          repoDef["content"].get<std::string>().find("executeGrepWithOptions") != std::string::npos &&
          repoDef["end_line"].get<size_t>() > repoDef["start_line"].get<size_t>(),
      "read_symbol resolves executeGrepWithOptions implementation in src/chat_tools.cpp");

    // Verify SSE progress summaries through chat_tools::run().
    size_t rounds = 0;
    std::vector<json> events;
    const json messages = json::array({{{"role", "user"}, {"content", "Inspect symbol"}}});
    const json assistantCalls = {{"role", "assistant"}, {"tool_calls", json::array({
      call("s1", {{"path", repoH}}, "symbol_outline"),
      call("s2", {{"path", repoCpp}, {"symbol", "executeGrepWithOptions"}, {"role", "definition"}}, "read_symbol")
    })}};
    auto outcome = chat_tools::run(messages, [&](const json &) -> chat_tools::Turn {
      if (++rounds == 1) return {"", assistantCalls};
      return {"Done", {{"role", "assistant"}, {"content", "Done"}}};
    }, [&](const json &request) {
      return request["function"]["name"] == "symbol_outline"
        ? chat_tools::executeSymbolOutline(request, {repoScope, repoUsed}, &symService)
        : chat_tools::executeReadSymbol(request, {repoScope, repoUsed}, &symService);
    }, [] { return true; }, 5, [&](const json &ev) { events.push_back(ev); }, repoRoot.generic_string());
    check(outcome.text == "Done" && events.size() == 4 &&
          events[1]["summary"].get<std::string>().find("Outline include/chat_tools.h") != std::string::npos &&
          events[3]["summary"].get<std::string>().find("Read symbol executeGrepWithOptions") != std::string::npos,
      "chat_tools::run formats compact summaries and completion events for symbol tools");

    // 9. Project-wide find_symbol: cold cache, multi-language, filters, incomplete reporting, and repo lookup.
    {
      symbol_service::SymbolService coldService;
      std::vector<std::string> findUsed;

      auto pyOutline = chat_tools::executeSymbolOutline(
        call("so_py", {{"path", "worker.py"}}, "symbol_outline"), {symAllowed, findUsed}, &coldService);
      check(!pyOutline.contains("error") && pyOutline["language"] == "python" && pyOutline["symbol_count"] == 3,
        "symbol_outline reports language=python on .py file");

      auto pyRun = chat_tools::executeReadSymbol(
        call("rs_pyrun", {{"path", "worker.py"}, {"symbol", "Worker.run"}}, "read_symbol"), {symAllowed, findUsed}, &coldService);
      check(!pyRun.contains("error") && pyRun["language"] == "python" &&
            pyRun["content"].get<std::string>().find("return 42") != std::string::npos,
        "read_symbol supports dotted Python qualified name Worker.run");

      symbol_service::SymbolService reallyColdService;
      findUsed.clear();
      auto findCompute = chat_tools::executeFindSymbol(
        call("fs_compute", {{"name", "compute"}}, "find_symbol"), {symAllowed, findUsed}, &reallyColdService);
      check(!findCompute.contains("error") && findCompute["outcome"] == "complete" &&
            findCompute["incomplete"] == false &&
            findCompute["supported_files"] == 6 && findCompute["searched_files"] == 6 &&
            findCompute["match_count"] == 4 &&
            findCompute["matches"][0]["role"] == "definition" &&
            findCompute["matches"][1]["role"] == "definition" &&
            findCompute["matches"][2]["role"] == "declaration" &&
            reallyColdService.cachedFileCount() == 6,
        "find_symbol scans supported files on cold cache (excluding .svelte) and ranks definitions before declarations");

      auto findPrefixPy = chat_tools::executeFindSymbol(
        call("fs_py_prefix", {{"name", "py_"}, {"match", "prefix"}, {"glob", "*.py"}}, "find_symbol"),
        {symAllowed, findUsed}, &reallyColdService);
      check(!findPrefixPy.contains("error") && findPrefixPy["match_count"] == 1 &&
            findPrefixPy["matches"][0]["name"] == "py_greet" &&
            findPrefixPy["matches"][0]["language"] == "python",
        "find_symbol supports prefix match and glob filtering");

      auto findIfaceTs = chat_tools::executeFindSymbol(
        call("fs_ts_iface", {{"name", "Client"}, {"match", "prefix"}, {"kind", "interface"}}, "find_symbol"),
        {symAllowed, findUsed}, &reallyColdService);
      check(!findIfaceTs.contains("error") && findIfaceTs["match_count"] == 1 &&
            findIfaceTs["matches"][0]["name"] == "ClientCfg" &&
            findIfaceTs["matches"][0]["language"] == "typescript",
        "find_symbol filters by kind=interface in TypeScript files");

      auto findTsArrow = chat_tools::executeFindSymbol(
        call("fs_ts_arrow", {{"name", "createHandler"}, {"role", "definition"}}, "find_symbol"),
        {symAllowed, findUsed}, &reallyColdService);
      check(!findTsArrow.contains("error") && findTsArrow["match_count"] == 1,
        "find_symbol locates exported TypeScript arrow function");
      auto readTsArrow = chat_tools::executeReadSymbol(
        call("rs_ts_arrow", {{"path", findTsArrow["matches"][0]["source"]},
                             {"symbol", findTsArrow["matches"][0]["selector"]}}, "read_symbol"),
        {symAllowed, findUsed}, &reallyColdService);
      check(!readTsArrow.contains("error") && readTsArrow["language"] == "typescript" &&
            readTsArrow["content"].get<std::string>().find("createHandler") != std::string::npos,
        "read_symbol accepts selector produced by find_symbol");

      const std::string missingCpp = (fixture.directory / "deleted_after_index.cpp").generic_string();
      std::vector<std::string> partialScope{missingCpp, symTsFile};
      std::vector<std::string> partialUsed;
      auto partialFind = chat_tools::executeFindSymbol(
        call("fs_inc", {{"name", "createHandler"}}, "find_symbol"),
        {partialScope, partialUsed}, &reallyColdService);
      check(!partialFind.contains("error") && partialFind["incomplete"] == true &&
            partialFind["outcome"] == "incomplete_search" &&
            partialFind["unreadable_files"] == 1 && partialFind["searched_files"] == 1 &&
            partialFind["match_count"] == 1,
        "find_symbol reports incomplete=true and unreadable_files when any supported file cannot be read");

      std::vector<std::string> repoFindUsed;
      auto repoFind = chat_tools::executeFindSymbol(
        call("fs_repo_grep", {{"name", "executeGrepWithOptions"}}, "find_symbol"),
        {repoScope, repoFindUsed}, &reallyColdService);
      check(!repoFind.contains("error") && repoFind["incomplete"] == false &&
            repoFind["match_count"] == 2 &&
            repoFind["matches"][0]["source"] == repoCpp && repoFind["matches"][0]["role"] == "definition" &&
            repoFind["matches"][1]["source"] == repoH && repoFind["matches"][1]["role"] == "declaration",
        "find_symbol ranks src/chat_tools.cpp definition before include/chat_tools.h declaration");

      auto repoNoExt = chat_tools::executeFindSymbol(
        call("fs_noext", {{"name", "anything"}, {"glob", "*.svelte"}}, "find_symbol"),
        {symAllowed, repoFindUsed}, &reallyColdService);
      check(!repoNoExt.contains("error") && repoNoExt["outcome"] == "no_supported_files" &&
            repoNoExt["incomplete"] == false,
        "find_symbol reports no_supported_files cleanly when filter leaves only unsupported files");

      size_t fsRounds = 0;
      std::vector<json> fsEvents;
      const json fsCall = {{"role", "assistant"}, {"tool_calls", json::array({
        call("f1", {{"name", "executeGrepWithOptions"}, {"role", "definition"}}, "find_symbol")
      })}};
      auto fsOutcome = chat_tools::run(messages, [&](const json &) -> chat_tools::Turn {
        if (++fsRounds == 1) return {"", fsCall};
        return {"Done", {{"role", "assistant"}, {"content", "Done"}}};
      }, [&](const json &request) {
        return chat_tools::executeFindSymbol(request, {repoScope, repoFindUsed}, &reallyColdService);
      }, [] { return true; }, 5, [&](const json &ev) { fsEvents.push_back(ev); }, repoRoot.generic_string());
      check(fsOutcome.text == "Done" && fsEvents.size() == 2 &&
            fsEvents[1]["summary"].get<std::string>().find("Find symbol executeGrepWithOptions") != std::string::npos,
        "chat_tools::run formats compact summaries for find_symbol");
    }

    std::cout << checks << " symbol tool checks passed.\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
