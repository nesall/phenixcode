#include "symbol_service.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
  int checks = 0;

  void check(bool condition, const char *message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
  }

  std::string readRepoFile(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Unable to open fixture file: " + path.generic_string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
  }

  std::vector<const symbol_service::SymbolInfo *> findByName(const symbol_service::FileOutline &outline, std::string_view name) {
    std::vector<const symbol_service::SymbolInfo *> out;
    for (const auto &sym : outline.symbols) {
      if (sym.name == name) out.push_back(&sym);
    }
    return out;
  }
}

int main(int argc, char *argv[]) {
  try {
    const fs::path repoRoot = argc > 1 ? fs::path(argv[1]) : fs::current_path();

    // 1. Explicit language extension routing (.h defaults to C++; .c unsupported for now).
    check(symbol_service::detectLanguage("src/main.cpp") == symbol_service::Language::Cpp, ".cpp routes to Cpp");
    check(symbol_service::detectLanguage("INCLUDE/UTILS.H") == symbol_service::Language::Cpp, ".h defaults to Cpp case-insensitively");
    check(symbol_service::detectLanguage("header.hpp") == symbol_service::Language::Cpp, ".hpp routes to Cpp");
    check(symbol_service::detectLanguage("unit.cc") == symbol_service::Language::Cpp, ".cc routes to Cpp");
    check(symbol_service::detectLanguage("sqlite3.c") == symbol_service::Language::Unsupported, ".c stays Unsupported until C grammar arrives");
    check(symbol_service::detectLanguage("script.py") == symbol_service::Language::Unsupported, ".py stays Unsupported");
    check(symbol_service::detectLanguage("README.md") == symbol_service::Language::Unsupported, ".md stays Unsupported");

    symbol_service::SymbolService service(2);

    // 2. PhenixCode header declaration vs implementation definition (executeGrepWithOptions).
    {
      const std::string headerSrc = readRepoFile(repoRoot / "include" / "chat_tools.h");
      const auto headerOutline = service.outlineForContent("include/chat_tools.h", symbol_service::Language::Cpp, headerSrc);
      check(!headerOutline.symbols.empty(), "include/chat_tools.h produces symbols");

      const auto apiFwd = findByName(headerOutline, "ApiConfig");
      check(apiFwd.size() == 1 && apiFwd[0]->kind == symbol_service::SymbolKind::Struct &&
            apiFwd[0]->role == symbol_service::SymbolRole::Declaration,
            "forward declaration struct ApiConfig is extracted as Declaration");

      const auto toolCtx = findByName(headerOutline, "ToolContext");
      check(toolCtx.size() == 1 && toolCtx[0]->kind == symbol_service::SymbolKind::Struct &&
            toolCtx[0]->role == symbol_service::SymbolRole::Definition,
            "struct ToolContext is extracted as Definition");

      const auto declHits = findByName(headerOutline, "executeGrepWithOptions");
      check(declHits.size() == 1 && declHits[0]->role == symbol_service::SymbolRole::Declaration &&
            declHits[0]->container == "chat_tools" &&
            declHits[0]->kind == symbol_service::SymbolKind::Function,
            "header executeGrepWithOptions is Function Declaration in chat_tools");

      const std::string cppSrc = readRepoFile(repoRoot / "src" / "chat_tools.cpp");
      const auto cppOutline = service.outlineForContent("src/chat_tools.cpp", symbol_service::Language::Cpp, cppSrc);
      const auto defHits = findByName(cppOutline, "executeGrepWithOptions");
      check(defHits.size() == 1 && defHits[0]->role == symbol_service::SymbolRole::Definition &&
            defHits[0]->container == "chat_tools",
            "cpp executeGrepWithOptions extracts only definition and skips call inside executeGrep");
      const std::string sliced = cppSrc.substr(defHits[0]->start_byte, defHits[0]->end_byte - defHits[0]->start_byte);
      check(sliced.find("executeGrepWithOptions") != std::string::npos && sliced.back() == '}',
            "exact half-open byte slice returns complete function definition");
    }

    // 3. Overloaded methods and const/non-const overloads in PhenixCode fixtures.
    {
      const std::string spHeader = readRepoFile(repoRoot / "include" / "sourceproc.h");
      const auto spOutline = service.outlineForContent("include/sourceproc.h", symbol_service::Language::Cpp, spHeader);
      const auto greps = findByName(spOutline, "grep");
      check(greps.size() == 2 && greps[0]->container == "SourceProcessor" && greps[1]->container == "SourceProcessor" &&
            greps[0]->selector != greps[1]->selector && greps[0]->signature != greps[1]->signature &&
            greps[0]->role == symbol_service::SymbolRole::Declaration,
            "SourceProcessor::grep overloads share container and name with unique selectors");

      const std::string appHeader = readRepoFile(repoRoot / "include" / "app.h");
      const auto appOutline = service.outlineForContent("include/app.h", symbol_service::Language::Cpp, appHeader);
      const auto dbs = findByName(appOutline, "db");
      check(dbs.size() == 2 && dbs[0]->container == "App" && dbs[0]->selector != dbs[1]->selector,
            "App::db const and non-const overloads are both extracted");
      const auto ops = findByName(appOutline, "operator|");
      check(ops.size() == 1 && ops[0]->role == symbol_service::SymbolRole::Definition,
            "inline operator| in app.h is extracted cleanly");
    }

    // 4. Out-of-class constructor, destructor, methods, #ifndef header guards, [[nodiscard]] & templates.
    {
      const std::string mcpCpp = readRepoFile(repoRoot / "src" / "mcp_server.cpp");
      const auto mcpOutline = service.outlineForContent("src/mcp_server.cpp", symbol_service::Language::Cpp, mcpCpp);
      const auto ctors = findByName(mcpOutline, "McpServer");
      check(ctors.size() == 1 && ctors[0]->kind == symbol_service::SymbolKind::Constructor &&
            ctors[0]->container == "McpServer", "out-of-class constructor detected");
      const auto dtors = findByName(mcpOutline, "~McpServer");
      check(dtors.size() == 1 && dtors[0]->kind == symbol_service::SymbolKind::Destructor &&
            dtors[0]->container == "McpServer", "out-of-class destructor detected");
      const auto starts = findByName(mcpOutline, "start");
      check(starts.size() == 1 && starts[0]->kind == symbol_service::SymbolKind::Method &&
            starts[0]->container == "McpServer", "out-of-class method detected");

      const std::string cutilsH = readRepoFile(repoRoot / "include" / "cutils.h");
      const auto cutilsOutline = service.outlineForContent("include/cutils.h", symbol_service::Language::Cpp, cutilsH);
      const auto trimmedDecls = findByName(cutilsOutline, "trimmed");
      const auto execDirDecls = findByName(cutilsOutline, "getExecutableDir");
      check(trimmedDecls.size() == 1 && execDirDecls.size() == 1,
            "#ifndef header guard, [[nodiscard]], and reference return declarators are extracted");

      const std::string httpGenCpp = readRepoFile(repoRoot / "src" / "httpserver_generation.cpp");
      const auto genOutline = service.outlineForContent("src/httpserver_generation.cpp", symbol_service::Language::Cpp, httpGenCpp);
      const auto vecContains = findByName(genOutline, "vecContains");
      const auto vecAddIfUnique = findByName(genOutline, "vecAddIfUnique");
      check(vecContains.size() == 1 && vecContains[0]->signature.rfind("template <class T>", 0) == 0 &&
            vecAddIfUnique.size() == 2 && vecAddIfUnique[0]->signature.rfind("template <class T>", 0) == 0,
            "template function range starts at template keyword and includes both overloads");
    }

    // 5. Same-line definitions, template class out-of-class method, forward decl + def, and function-pointer filtering.
    {
      const std::string synthetic =
        "void forwardOnly(int a);\n"
        "void forwardAndDef();\n"
        "void forwardAndDef() { int local = 0; (void)local; }\n"
        "void (*fnPtrVar)(int);\n"
        "template <typename T> class Box {\n"
        "public:\n"
        "  T get() const;\n"
        "};\n"
        "template <typename T> T Box<T>::get() const { return T{}; }\n"
        "int alpha() { return 1; } int beta() { return 2; }\n";

      bool hit = true;
      const auto outline = service.outlineForContent("synthetic.cpp", symbol_service::Language::Cpp, synthetic, &hit);
      check(!hit, "initial synthetic parse is a cache miss");
      const auto cached = service.outlineForContent("synthetic.cpp", symbol_service::Language::Cpp, synthetic, &hit);
      check(hit && cached.symbols.size() == outline.symbols.size(), "identical snapshot hits cache");

      const auto fns = findByName(outline, "forwardAndDef");
      check(fns.size() == 2 &&
            fns[0]->role == symbol_service::SymbolRole::Declaration &&
            fns[1]->role == symbol_service::SymbolRole::Definition &&
            fns[0]->selector != fns[1]->selector,
            "same-file declaration and definition have distinct roles and selectors");
      check(findByName(outline, "fnPtrVar").empty(), "function pointer variable is not emitted as function declaration");

      const auto gets = findByName(outline, "get");
      check(gets.size() == 2 &&
            gets[0]->qualified_name == "Box::get" && gets[0]->role == symbol_service::SymbolRole::Declaration &&
            gets[1]->qualified_name == "Box::get" && gets[1]->role == symbol_service::SymbolRole::Definition &&
            gets[1]->signature.rfind("template <typename T>", 0) == 0,
            "template class method declaration and out-of-class definition share Box::get");

      const auto alpha = findByName(outline, "alpha");
      const auto beta = findByName(outline, "beta");
      check(alpha.size() == 1 && beta.size() == 1 &&
            alpha[0]->start_line == beta[0]->start_line &&
            synthetic.substr(alpha[0]->start_byte, alpha[0]->end_byte - alpha[0]->start_byte) == "int alpha() { return 1; }" &&
            synthetic.substr(beta[0]->start_byte, beta[0]->end_byte - beta[0]->start_byte) == "int beta() { return 2; }",
            "two definitions on one line slice cleanly using exact half-open byte ranges");
    }

    // 6. Freshness invalidation on same-byte-size edit & LRU eviction.
    {
      check(service.cachedFileCount() == 2, "LRU cache enforces max capacity 2");
      const std::string v1 = "void firstName() { return; }\n";
      const std::string v2 = "void otherName() { return; }\n";
      check(v1.size() == v2.size(), "same byte size edit fixture");
      bool hit = false;
      const auto o1 = service.outlineForContent("fresh.cpp", symbol_service::Language::Cpp, v1, &hit);
      check(!hit && findByName(o1, "firstName").size() == 1, "first version parsed");
      const auto o2 = service.outlineForContent("fresh.cpp", symbol_service::Language::Cpp, v2, &hit);
      check(!hit && findByName(o2, "firstName").empty() && findByName(o2, "otherName").size() == 1,
            "SHA-256 verification invalidates cache on same-size content changes without relying on mtime");
    }

    std::cout << checks << " symbol service checks passed.\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
