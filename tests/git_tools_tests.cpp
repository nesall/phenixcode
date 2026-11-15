#include "chat_tools.h"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <cctype>
#include <3rdparty/utf8.h>
#ifndef _WIN32
#include <csignal>
#endif

using nlohmann::json;
namespace fs = std::filesystem;
namespace {
  int checks = 0;
  void check(bool value, const char *message) { ++checks; if (!value) throw std::runtime_error(message); }
  json call(const std::string &name, const json &args = json::object()) {
    return {{"function", {{"name", name}, {"arguments", args.dump()}}}};
  }
}
int main(int argc, char **argv) {
  try {
    // A copy of this executable named git serves as a deterministic hung child.
    if (std::string(argv[0]) == "git") {
      const auto mode = std::getenv("PHENIXCODE_GIT_HELPER");
      if (!mode) { std::this_thread::sleep_for(std::chrono::seconds(30)); return 0; }
      std::string directory;
      for (int i = 1; i < argc; ++i) {
        const std::string argument(argv[i]);
        if (argument == "-C" && i + 1 < argc) directory = argv[i + 1];
        if (argument == "rev-parse") {
          std::this_thread::sleep_for(std::chrono::seconds(3)); std::cout << directory << '\n'; return 0;
        }
        if (argument == "config") return 1;
        if (argument == "diff") { std::this_thread::sleep_for(std::chrono::seconds(30)); return 0; }
      }
      return 0;
    }
    if (argc != 2 && argc != 3) throw std::runtime_error("Expected temporary fixture directory");
    const std::string rootText(argv[1]);
    const fs::path root = fs::path(std::u8string(rootText.begin(), rootText.end()));
    const auto file = [&](const char *name) {
      const auto prefix = (root / "repo space").generic_u8string();
      return std::string(prefix.begin(), prefix.end()) + "/" + name;
    };
    std::vector<std::string> files{file("src/main.cpp"), file("src/odd [x];$(echo).cpp"), file("src/big.cpp"), file("src/blob.cpp"), file("src/caf\xc3\xa9.cpp")};
    std::vector<std::string> used;
    chat_tools::ToolContext context{files, used};
    if (argc == 3) {
      const auto start = std::chrono::steady_clock::now();
      const auto result = chat_tools::executeGit(call("git_diff"), context);
      const std::string mode(argv[2]);
      const auto expected = mode == "--expect-main-timeout" ? "Git inspection timed out" : mode == "--expect-timeout" ? "Git preparation timed out" : "PATH";
      if (mode == "--expect-main-timeout") check(std::chrono::steady_clock::now() - start >= std::chrono::seconds(12),
        "main command gets its full budget after slow preparation");
      check(result.contains("error") && result["error"].get<std::string>().find(expected) != std::string::npos,
        "Git timeout and missing-executable failures are reported without hanging");
      std::cout << expected << " handling passed.\n";
      return 0;
    }
    const auto execute = [&](const char *name, const json &args = json::object()) {
      auto result = chat_tools::executeGit(call(name, args), context);
      check(result.dump().size() <= chat_tools::kMaxResultBytes, "Git output including JSON escaping respects the result byte limit");
      if (!result.contains("error")) {
        const auto text = result["content"].get<std::string>();
        check(utf8::is_valid(text.begin(), text.end()), "Git output is valid UTF-8");
        check(text.find("EXCLUDED_SECRET") == std::string::npos && text.find("OTHER_REPO_SECRET") == std::string::npos,
          "Git output never includes excluded-file or other-repository content");
      }
      return result;
    };
    const auto text = [](const json &result) { return result.at("content").get<std::string>(); };
    for (const auto &definition : {chat_tools::gitDiffDefinition(), chat_tools::gitLogDefinition(), chat_tools::gitShowDefinition()})
      check(definition["function"]["parameters"]["additionalProperties"] == false, "Git tool schemas reject unrecognized keys");
    auto result = execute("git_diff", {{"path", "src/main.cpp"}});
    check(!result.contains("error") && result["mode"] == "unstaged" && text(result).find("+int unstaged_change;") != std::string::npos &&
      text(result).find("+int staged_change;") == std::string::npos, "default diff compares worktree to index");
    check(used == std::vector<std::string>{files[0]}, "file-specific diff tracks its delivered source");
    check(result["context"] == 3, "diff preserves the default three-line context");
    const auto zeroContext = execute("git_diff", {{"path", "main.cpp"}, {"context", 0}});
    const auto wideContext = execute("git_diff", {{"path", "main.cpp"}, {"context", 20}});
    check(zeroContext["context"] == 0 && text(zeroContext).find("\n int original_content;") == std::string::npos &&
      wideContext["context"] == 20 && text(wideContext).find("\n int original_content;") != std::string::npos,
      "diff context changes surrounding lines at both supported boundaries");
    result = execute("git_diff", {{"staged", true}, {"path", "src/main.cpp"}});
    check(result["mode"] == "staged" && text(result).find("+int staged_change;") != std::string::npos &&
      text(result).find("unstaged_change") == std::string::npos, "staged diff compares index to HEAD");
    result = execute("git_diff", {{"base", "base-branch"}, {"path", "main.cpp"}});
    check(!result.contains("error") && result["mode"] == "branch" && text(result).find("committed_change") != std::string::npos &&
      text(result).find("base_only_change") == std::string::npos && text(result).find("staged_change") == std::string::npos &&
      result["merge_base"] != result["base_commit"], "branch comparison uses the common ancestor and excludes base-only and uncommitted changes");
    check(execute("git_diff", {{"base", "main"}, {"staged", true}}).contains("error"), "base cannot be combined with staged mode");
    check(execute("git_diff", {{"base", "unrelated"}}).contains("error"), "unrelated histories produce an explicit merge-base error");
    for (const auto &base : {"--help", "HEAD:excluded.env", "HEAD..main", "$(echo)", "missing-branch"})
      check(execute("git_diff", {{"base", base}}).contains("error"), "base revisions use the hardened commit resolver");
    result = execute("git_diff");
    check(!result.contains("error") && text(result).find("unstaged_change") != std::string::npos, "repository diff is restricted to allowed file paths");
    result = execute("git_diff", {{"path", "odd [x];$(echo).cpp"}});
    check(!result.contains("error") && text(result).find("literal_filename_change") != std::string::npos,
      "path arguments with spaces and shell/pathspec characters are interpreted literally");
    result = execute("git_show", {{"revision", "HEAD"}, {"path", "src/caf\xc3\xa9.cpp"}});
    if (result.contains("error") || text(result) != "int unicode_filename;\n") std::cerr << result.dump() << '\n';
    check(!result.contains("error") && text(result) == "int unicode_filename;\n", "Git receives Unicode file paths without changing their encoding");
    used.clear();
    result = execute("git_log", {{"limit", 1}, {"path", "main.cpp"}});
    check(!result.contains("error") && text(result).find("second source commit") != std::string::npos &&
      text(result).find("initial source commit") == std::string::npos, "log limit and path scope select the most recent matching commit");
    check(used.empty(), "commit metadata is not cited as file content");
    result = execute("git_log");
    check(!result.contains("error") && text(result).find("initial source commit") != std::string::npos &&
      text(result).find("excluded-only commit") == std::string::npos, "log excludes commits touching only files outside source scope");
    result = execute("git_show", {{"revision", "initial-tag"}, {"path", "main.cpp"}});
    check(text(result) == "int original_content;\n" && result["revision"].get<std::string>().size() == 40,
      "show resolves a tag to a commit and returns original file contents");
    result = execute("git_show", {{"revision", "HEAD~1"}, {"path", files[0]}});
    check(text(result).find("committed_change") != std::string::npos && text(result).find("staged_change") == std::string::npos,
      "show supports ancestry and preserves committed contents independently of the index");
    result = execute("git_show", {{"revision", "HEAD"}});
    check(!result.contains("error") && text(result).empty(), "show of an excluded-only commit exposes no out-of-scope patch or metadata");
    result = execute("git_show", {{"revision", "HEAD~1"}});
    check(text(result).find("second source commit") != std::string::npos && text(result).find("committed_change") != std::string::npos,
      "show without a path returns metadata and scoped commit patch");
    const auto narrowShow = execute("git_show", {{"revision", "HEAD~1"}, {"context", 0}});
    const auto wideShow = execute("git_show", {{"revision", "HEAD~1"}, {"context", 20}});
    check(narrowShow["context"] == 0 && text(narrowShow).find("\n int original_content;") == std::string::npos &&
      wideShow["context"] == 20 && text(wideShow).find("\n int original_content;") != std::string::npos,
      "commit-patch show honors context without changing metadata or source scope");
    for (const auto &contextValue : {json(-1), json(21), json(1.5), json("3"), json(nullptr), json(true)}) {
      check(execute("git_diff", {{"context", contextValue}}).contains("error"), "diff rejects invalid context values");
      check(execute("git_show", {{"revision", "HEAD"}, {"context", contextValue}}).contains("error"), "show rejects invalid context values");
    }
    check(execute("git_show", {{"revision", "HEAD"}, {"path", "main.cpp"}, {"context", 0}}).contains("error"), "historical-file reads reject irrelevant patch context");
    check(execute("git_log", {{"context", 0}}).contains("error"), "log rejects patch-only context");
    check(execute("git_show", {{"revision", "HEAD"}, {"path", "src/blob.cpp"}}).contains("error"), "binary historical files produce a structured error");
    result = execute("git_show", {{"revision", "HEAD"}, {"path", "src/big.cpp"}});
    check(result["truncated"] == true && !text(result).empty(), "large historical files are explicitly truncated");
    for (const auto &revision : {"--help", "HEAD:excluded.env", "HEAD..main", "HEAD;echo", "$(echo)", "HEAD@{1}", "nonexistent-branch"})
      check(execute("git_show", {{"revision", revision}}).contains("error"), "invalid, option-like and unsupported revision forms cannot bypass the tool");
    for (const auto &request : {call("git_diff", {{"staged", "true"}}), call("git_log", {{"limit", 0}}),
        call("git_log", {{"limit", 21}}), call("git_log", {{"limit", 1.5}}), call("git_show"),
        call("git_show", {{"revision", nullptr}}), call("git_diff", {{"unexpected", true}}), call("git_diff", json::array()),
        call("git_diff", {{"path", "../excluded.env"}}), call("git_show", {{"revision", "HEAD"}, {"path", "excluded.env"}}),
        call("git_diff", {{"path", std::string("src\0main", 8)}}), call("unknown")})
      check(chat_tools::executeGit(request, context).contains("error"), "invalid envelopes, arguments and excluded paths return errors");
    auto bad = call("git_diff");
    bad["function"]["arguments"] = std::string(4097, 'x');
    check(chat_tools::executeGit(bad, context).contains("error"), "oversized arguments are rejected");
    bad["function"]["arguments"] = "{";
    check(chat_tools::executeGit(bad, context).contains("error"), "malformed arguments are rejected");
    bad["function"]["arguments"] = json::object();
    check(chat_tools::executeGit(bad, context).contains("error"), "arguments must use provider JSON-string format");
    const auto originalFiles = files;
    files.push_back(files[0]);
    check(!execute("git_show", {{"path", files[0]}, {"revision", "HEAD"}}).contains("error"), "duplicate allowlist entries do not make an exact source ambiguous");
    files = originalFiles;
    files = {file("src/./main.cpp"), file("src/main.cpp")};
    check(!execute("git_show", {{"path", file("src/main.cpp")}, {"revision", "initial-tag"}}).contains("error"),
      "normalization aliases do not make an exact source path ambiguous");
    check(!execute("git_show", {{"path", "main.cpp"}, {"revision", "initial-tag"}}).contains("error"),
      "normalization aliases also resolve as one source for suffix requests");
    files = originalFiles;
    chat_tools::GitRepositoryCache discovery;
    auto cached = chat_tools::executeGit(call("git_log"), context, &discovery);
    const auto cachedFiles = discovery.files.size();
    const auto cachedDirectories = discovery.roots.size();
    check(!cached.contains("error") && cachedFiles == files.size(), "source discovery canonicalizes each allowed file once");
    cached = chat_tools::executeGit(call("git_show", {{"path", files[0]}, {"revision", "HEAD"}}), context, &discovery);
    check(!cached.contains("error") && discovery.files.size() == cachedFiles && discovery.roots.size() == cachedDirectories,
      "subsequent tools reuse request-local discovery without growing the validated mapping");
    files.clear();
    for (int i = 0; i < 10000; ++i) files.push_back(file(("src/" + std::string(70, 'a') + std::to_string(i) + ".cpp").c_str()));
    chat_tools::GitRepositoryCache largeDiscovery;
    cached = chat_tools::executeGit(call("git_diff"), context, &largeDiscovery);
    check(cached.contains("error") && cached["error"].get<std::string>().find("scope is too large") != std::string::npos && largeDiscovery.files.size() < 1000,
      "oversized source scopes are rejected before canonicalizing the full 10k-file project");
#ifdef _WIN32
    const auto alternate = (root / "REPO SPACE" / "src/odd [x];$(echo).cpp").generic_string();
    files = {originalFiles[0], alternate};
    result = execute("git_log");
    check(!result.contains("error"), "case-only root differences do not create a false multiple-repository error");
    result = execute("git_show", {{"path", alternate}, {"revision", "HEAD"}});
    check(!result.contains("error") && text(result).find("literal_filename") != std::string::npos,
      "case-only root differences preserve valid repository-relative paths");
#else
    files = {file("src/colon:name.cpp")};
    check(text(execute("git_show", {{"path", files[0]}, {"revision", "colon-tag"}})) == "int colon_filename;\n", "POSIX colon filenames work in hash:path expressions");
#endif
    files = originalFiles;
    files.clear();
    check(execute("git_diff").contains("error"), "empty scope cannot access a repository implicitly");
    files = {(root / "no git" / "plain.cpp").generic_string()};
    check(execute("git_diff").contains("error"), "non-Git source produces an actionable error");
    files = originalFiles;
    files.push_back((root / "other" / "other.cpp").generic_string());
    check(execute("git_log").contains("error"), "multiple repositories require explicit source selection");
    check(!execute("git_log", {{"path", files[0]}}).contains("error"), "explicit path selects a repository from multi-repository scope");
    files = {originalFiles[0], (root / "worktree space" / "src/main.cpp").generic_string()};
    check(execute("git_log", {{"path", "src/main.cpp"}}).contains("error"), "ambiguous suffix paths require full source paths");
    result = execute("git_show", {{"path", files[1]}, {"revision", "initial-tag"}});
    check(!result.contains("error") && text(result) == "int original_content;\n", "linked worktrees with .git files work correctly");
    files = {file("src/new.cpp")};
    result = execute("git_diff");
    check(!result.contains("error") && text(result).empty() && result["untracked_files"] == json::array({"src/new.cpp"}),
      "configured untracked names are reported separately from the patch");
    files.clear();
    for (int i = 0; i < 60; ++i) files.push_back(file(("src/untracked_" + std::to_string(i) + ".cpp").c_str()));
    result = execute("git_diff");
    check(!result.contains("error") && result["untracked_files"].size() == 50 && result["untracked_truncated"] == true && result["truncated"] == true,
      "untracked reporting has its own explicit limit within the total result budget");
    files = {(root / "many-filters/source.cpp").generic_string()};
    check(!execute("git_log").contains("error") && !execute("git_show", {{"path", files[0]}, {"revision", "HEAD"}}).contains("error") &&
      !execute("git_diff", {{"staged", true}}).contains("error"), "history, historical blobs and staged diffs do not inspect or expand clean filter drivers");
    check(execute("git_diff").contains("error"), "working-tree diff still enforces bounded clean filter suppression");
    files = {(root / "empty" / "unborn.cpp").generic_string()};
    result = execute("git_diff", {{"staged", true}});
    check(!result.contains("error") && text(result).find("unborn_content") != std::string::npos, "staged diff supports an unborn branch");
    check(execute("git_log").contains("error"), "history on an unborn branch reports an error");
#ifndef _WIN32
    // Auto-reaping produces ECHILD: a missing wait status must never become success.
    const auto previous = std::signal(SIGCHLD, SIG_IGN);
    result = execute("git_diff");
    std::signal(SIGCHLD, previous);
    check(result.contains("error") && result["error"] == "Unable to wait for Git process", "POSIX wait failure cannot be reported as a successful command");
#endif
    std::cout << checks << " Git tool checks passed.\n";
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
