#include "chat_tools.h"
#include "cutils.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

using nlohmann::json;
namespace fs = std::filesystem;

namespace chat_tools {
  namespace {
    constexpr size_t maxCaptureBytes = 32768;
    constexpr size_t maxScopeBytes = 20000;
    fs::path fromText(std::string_view value) { return fs::path(std::u8string(value.begin(), value.end())); }
    std::string pathText(const fs::path &path) {
      const auto value = path.generic_u8string();
      return {value.begin(), value.end()};
    }
    using Deadline = std::chrono::steady_clock::time_point;
    void checkDeadline(Deadline deadline, const char *phase = "preparation") {
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error(std::string("Git ") + phase + " timed out; narrow the request to a source path");
    }
    struct CommandResult { std::string output; int code = -1; bool truncated = false; };

    bool keepEnvironment(std::string key) {
      std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
      return !key.starts_with("GIT_") && key != "LC_ALL";
    }

#ifdef _WIN32
    struct Handle {
      HANDLE value = nullptr;
      ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    };
    std::wstring wide(const std::string &text) {
      const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
      if (!size && !text.empty()) throw std::invalid_argument("Invalid UTF-8 in Git arguments");
      std::wstring result(size, L'\0');
      if (size) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), size);
      return result;
    }
    std::wstring quote(const std::wstring &argument) {
      std::wstring result = L"\"";
      size_t slashes = 0;
      for (const auto c : argument) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result += c;
      }
      result.append(slashes * 2, L'\\');
      return result + L'"';
    }
#endif

    // Direct argv invocation: never interpret model input as a shell command.
    // Each child gets its own sanitized environment; server cwd/environment stay unchanged.
    CommandResult runGit(const fs::path &root, const std::vector<std::string> &arguments, Deadline deadline,
        const std::vector<std::string> &configuration = {}, const char *phase = "inspection") {
      checkDeadline(deadline, phase);
      std::vector<std::string> command = {"git", "--no-pager", "--literal-pathspecs", "--no-optional-locks",
        "-c", "core.fsmonitor=false", "-c", "color.ui=false", "-c", "protocol.allow=never", "-C", pathText(root)};
      command.insert(command.end(), configuration.begin(), configuration.end());
      command.insert(command.end(), arguments.begin(), arguments.end());
      const std::vector<std::string> overrides = {"GIT_CONFIG_NOSYSTEM=1", "GIT_OPTIONAL_LOCKS=0", "GIT_TERMINAL_PROMPT=0",
        "GIT_NO_REPLACE_OBJECTS=1", "GIT_NO_LAZY_FETCH=1", "LC_ALL=C"};
      CommandResult result;
      bool finished = false;
      bool timedOut = false;
      const auto capture = [&](const char *data, size_t size) {
        const auto remaining = maxCaptureBytes - result.output.size();
        result.output.append(data, std::min(size, remaining));
        if (size > remaining) result.truncated = true;
      };
#ifdef _WIN32
      std::vector<std::wstring> environment;
      auto block = GetEnvironmentStringsW();
      if (!block) throw std::runtime_error("Unable to prepare Git environment");
      for (const wchar_t *entry = block; *entry; entry += wcslen(entry) + 1) {
        const std::wstring value(entry);
        const auto key = value.substr(0, value.find(L'='));
        std::string ascii;
        for (const auto c : key) ascii += c <= 127 ? static_cast<char>(c) : '?';
        if (keepEnvironment(ascii)) environment.push_back(value);
      }
      FreeEnvironmentStringsW(block);
      for (const auto &entry : overrides) environment.push_back(wide(entry));
      environment.push_back(L"GIT_CONFIG_GLOBAL=NUL");
      std::sort(environment.begin(), environment.end(), [](const auto &a, const auto &b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
      std::wstring environmentBlock;
      for (const auto &entry : environment) { environmentBlock += entry; environmentBlock += L'\0'; }
      environmentBlock += L'\0';
      std::wstring commandLine;
      for (const auto &entry : command) { if (!commandLine.empty()) commandLine += L' '; commandLine += quote(wide(entry)); }
      if (commandLine.size() >= 30000) throw std::invalid_argument("Git command exceeds the Windows argument limit");
      std::wstring executable(32768, L'\0');
      const auto length = SearchPathW(nullptr, L"git.exe", nullptr, static_cast<DWORD>(executable.size()), executable.data(), nullptr);
      if (!length || length >= executable.size()) throw std::runtime_error("Git executable not found; install Git and put it on PATH");
      executable.resize(length);
      SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
      Handle reader, writer, input;
      if (!CreatePipe(&reader.value, &writer.value, &security, 0) || !SetHandleInformation(reader.value, HANDLE_FLAG_INHERIT, 0))
        throw std::runtime_error("Unable to create Git output pipe");
      input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
      if (input.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Unable to create Git input handle");
      STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
      startup.hStdInput = input.value; startup.hStdOutput = writer.value; startup.hStdError = writer.value;
      PROCESS_INFORMATION process{};
      if (!CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
          environmentBlock.data(), nullptr, &startup, &process)) throw std::runtime_error("Unable to start Git");
      Handle child{process.hProcess}, thread{process.hThread};
      CloseHandle(writer.value); writer.value = nullptr;
      const auto drain = [&] {
        DWORD available = 0;
        char buffer[4096];
        while (PeekNamedPipe(reader.value, nullptr, 0, nullptr, &available, nullptr) && available) {
          DWORD read = 0;
          if (!ReadFile(reader.value, buffer, std::min<DWORD>(available, sizeof(buffer)), &read, nullptr) || !read) break;
          capture(buffer, read);
          if (result.truncated) break;
        }
      };
      while (!finished) {
        drain();
        finished = WaitForSingleObject(child.value, 0) == WAIT_OBJECT_0;
        timedOut = !finished && std::chrono::steady_clock::now() >= deadline;
        if (!finished && (timedOut || result.truncated)) {
          TerminateProcess(child.value, 1); WaitForSingleObject(child.value, INFINITE); finished = true;
        }
        if (!finished) std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      if (!result.truncated) drain();
      DWORD code = 1; GetExitCodeProcess(child.value, &code); result.code = static_cast<int>(code);
#else
      std::vector<std::string> environment;
      for (auto entry = environ; entry && *entry; ++entry) {
        const std::string value(*entry);
        if (keepEnvironment(value.substr(0, value.find('=')))) environment.push_back(value);
      }
      environment.insert(environment.end(), overrides.begin(), overrides.end());
      environment.push_back("GIT_CONFIG_GLOBAL=/dev/null");
      std::vector<char *> argv, envp;
      for (auto &entry : command) argv.push_back(entry.data());
      argv.push_back(nullptr);
      for (auto &entry : environment) envp.push_back(entry.data());
      envp.push_back(nullptr);
      int descriptors[2];
      if (pipe(descriptors)) throw std::runtime_error("Unable to create Git output pipe");
      fcntl(descriptors[0], F_SETFD, FD_CLOEXEC); fcntl(descriptors[1], F_SETFD, FD_CLOEXEC);
      fcntl(descriptors[0], F_SETFL, O_NONBLOCK);
      posix_spawn_file_actions_t actions;
      posix_spawn_file_actions_init(&actions);
      posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDOUT_FILENO);
      posix_spawn_file_actions_adddup2(&actions, descriptors[1], STDERR_FILENO);
      posix_spawn_file_actions_addclose(&actions, descriptors[0]);
      posix_spawn_file_actions_addclose(&actions, descriptors[1]);
      posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
      pid_t child;
      const auto error = posix_spawnp(&child, "git", &actions, nullptr, argv.data(), envp.data());
      posix_spawn_file_actions_destroy(&actions); close(descriptors[1]);
      if (error) { close(descriptors[0]); throw std::runtime_error("Unable to start Git; install Git and put it on PATH"); }
      int status = 0;
      bool haveStatus = false;
      const auto drain = [&] {
        char buffer[4096]; ssize_t readBytes;
        while ((readBytes = read(descriptors[0], buffer, sizeof(buffer))) > 0) {
          capture(buffer, static_cast<size_t>(readBytes));
          if (result.truncated) break;
        }
      };
      while (!finished) {
        drain();
        const auto waited = waitpid(child, &status, WNOHANG);
        haveStatus = waited == child;
        finished = haveStatus || (waited < 0 && errno != EINTR);
        timedOut = !finished && std::chrono::steady_clock::now() >= deadline;
        if (!finished && (timedOut || result.truncated)) {
          kill(child, SIGKILL);
          pid_t reaped;
          do { reaped = waitpid(child, &status, 0); } while (reaped < 0 && errno == EINTR);
          haveStatus = reaped == child;
          finished = true;
        }
        if (!finished) std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      if (!result.truncated) drain();
      close(descriptors[0]);
      result.code = haveStatus && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
      if (!haveStatus) throw std::runtime_error("Unable to wait for Git process");
#endif
      if (timedOut) checkDeadline(deadline, phase);
      return result;
    }

    std::string textArgument(const json &args, const char *key, size_t maximum) {
      if (!args.contains(key)) return {};
      if (!args[key].is_string()) throw std::invalid_argument(std::string(key) + " must be a string");
      const auto text = args[key].get<std::string>();
      if (text.empty() || text.size() > maximum || text.find('\0') != std::string::npos || text.find_first_of("\r\n") != std::string::npos)
        throw std::invalid_argument(std::string(key) + " must be nonempty single-line text within its size limit");
      return text;
    }
    std::string normalized(std::string path) {
      std::replace(path.begin(), path.end(), '\\', '/');
      return pathText(fromText(path).lexically_normal());
    }
    struct Repository {
      fs::path root;
      std::vector<std::string> files;
      std::string source;
      std::string selectedPath;
    };
    bool sameDirectory(const fs::path &a, const fs::path &b) {
      if (a == b) return true;
      std::error_code error;
      const auto same = fs::equivalent(a, b, error);
      if (error) throw std::runtime_error("Unable to compare Git working tree directories");
      return same;
    }
    Repository repository(const ToolContext &ctx, const std::string &requested, GitRepositoryCache &cache, Deadline deadline) {
      Repository result;
      std::vector<std::string> selected;
      if (!requested.empty()) {
        std::string spelling = requested;
        std::replace(spelling.begin(), spelling.end(), '\\', '/');
        for (const auto &part : fromText(spelling)) if (part == "..") throw std::invalid_argument("path must not contain parent traversal");
        const auto request = normalized(spelling);
        for (const auto &file : ctx.allowedFiles) {
          checkDeadline(deadline);
          if (normalized(file) == request) selected.push_back(file);
        }
        if (selected.empty()) for (const auto &file : ctx.allowedFiles) {
          checkDeadline(deadline);
          const auto path = normalized(file);
          if (!fromText(request).is_absolute() && path.size() > request.size() && path.ends_with("/" + request)) selected.push_back(file);
        }
        std::sort(selected.begin(), selected.end());
        selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
        if (selected.empty()) throw std::invalid_argument("path is not among configured local source files");
      }
      if (cache.workingDirectory.empty()) cache.workingDirectory = fs::current_path();
      const auto canonicalFile = [&](const std::string &file) {
        auto known = cache.files.find(file);
        if (known != cache.files.end()) return known->second;
        const auto input = fromText(file);
        const auto path = fs::weakly_canonical(input.is_absolute() ? input : cache.workingDirectory / input);
        checkDeadline(deadline);
        cache.files.emplace(file, path);
        return path;
      };
      if (selected.size() > 1) {
        const auto canonical = canonicalFile(selected.front());
        for (size_t i = 1; i < selected.size(); ++i)
          if (canonicalFile(selected[i]) != canonical) throw std::invalid_argument("Ambiguous source path; use the full path returned by list_files");
        selected.resize(1); // Different spellings of the same resolved pathname.
      }
      const auto findRoot = [&](fs::path directory) {
        std::vector<fs::path> visited;
        fs::path root;
        while (!directory.empty()) {
          checkDeadline(deadline);
          const auto known = cache.roots.find(directory);
          if (known != cache.roots.end()) { root = known->second; break; }
          visited.push_back(directory);
          if (fs::exists(directory / ".git")) { root = directory; break; }
          const auto parent = directory.parent_path();
          if (parent == directory) break;
          directory = parent;
        }
        for (const auto &path : visited) cache.roots[path] = root;
        return root;
      };
      size_t bytes = 0;
      std::set<std::string> processed, relativePaths;
      for (const auto &file : requested.empty() ? ctx.allowedFiles : selected) {
        checkDeadline(deadline);
        if (!processed.insert(file).second) continue;
        const auto canonical = canonicalFile(file);
        const auto root = findRoot(canonical.parent_path());
        if (root.empty()) {
          if (!requested.empty()) throw std::invalid_argument("Selected source is not in a Git working tree");
          continue;
        }
        if (result.root.empty()) result.root = root;
        else if (!sameDirectory(result.root, root)) throw std::invalid_argument("Multiple source repositories; specify a source path");
        // Use the root discovered from this file's own spelling, so a case-only
        // alias of the same Windows directory cannot produce a false ../ path.
        const auto relative = pathText(canonical.lexically_relative(root));
        if (relative.empty() || relative.starts_with("../") || relative == "..") throw std::invalid_argument("Source is outside the Git working tree");
        if (relativePaths.insert(relative).second) {
          bytes += relative.size() * 2 + 3;
          if (bytes > maxScopeBytes) throw std::invalid_argument("Git source scope is too large; specify a source path");
          result.files.push_back(relative);
        }
        if (!requested.empty()) { result.source = file; result.selectedPath = relative; }
      }
      checkDeadline(deadline);
      if (result.root.empty()) throw std::invalid_argument("No Git working tree found for configured local source files");
      return result;
    }
    bool commitHash(const std::string &hash) {
      return (hash.size() == 40 || hash.size() == 64) && hash.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
    }
    std::string commit(const Repository &repo, const std::string &revision, Deadline deadline, const char *label = "revision") {
      if (revision.empty() || revision.front() == '-' || revision.find("..") != std::string::npos ||
          revision.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_./-~^") != std::string::npos)
        throw std::invalid_argument(std::string(label) + " must be a commit ID, branch or tag, optionally with ~ or ^ ancestry; ranges and object paths are not accepted");
      const auto resolved = runGit(repo.root, {"rev-parse", "--verify", "--end-of-options", revision + "^{commit}"}, deadline, {}, "preparation");
      const auto hash = utils::trimmed(resolved.output);
      if (resolved.code || resolved.truncated || !commitHash(hash))
        throw std::invalid_argument(std::string(label) + " does not resolve to a local commit");
      return hash;
    }
    std::vector<std::string> disableFilters(const Repository &repo, Deadline deadline) {
      // --no-ext-diff/--no-textconv do not disable clean filters used when
      // comparing a working-tree file. Read names only and override each driver
      // in the child command, without changing repository configuration.
      const auto keys = runGit(repo.root, {"config", "--null", "--name-only", "--get-regexp", "^filter\\..*\\.(clean|smudge|process|required)$"}, deadline, {}, "preparation");
      if (keys.truncated || (keys.code != 0 && keys.code != 1)) throw std::runtime_error("Unable to inspect Git filter configuration");
      std::set<std::string> drivers;
      for (size_t start = 0; start < keys.output.size();) {
        const auto end = keys.output.find('\0', start);
        if (end == std::string::npos) throw std::runtime_error("Invalid Git filter configuration response");
        const auto key = keys.output.substr(start, end - start);
        drivers.insert(key.substr(0, key.rfind('.')));
        start = end + 1;
      }
      if (drivers.size() > 32) throw std::runtime_error("Too many Git filter drivers for bounded inspection");
      std::vector<std::string> result;
      for (const auto &driver : drivers)
        for (const auto *setting : {".clean=", ".smudge=", ".process=", ".required=false"}) {
          result.push_back("-c"); result.push_back(driver + setting);
        }
      return result;
    }
    json boundedResult(json result, std::string content, bool truncated) {
      result["content"] = ""; result["truncated"] = true;
      if (result.dump().size() > kMaxResultBytes) return {{"error", "Git result metadata exceeds the output limit"}};
      content = utils::sanitizeUtf8(content);
      size_t low = 0, high = content.size();
      while (low < high) {
        const auto middle = low + (high - low + 1) / 2;
        result["content"] = content.substr(0, middle);
        // Intermediate prefixes can end in the middle of UTF-8. Ignore invalid
        // trailing bytes for the sizing probe; the final boundary is repaired below.
        result["truncated"] = truncated || middle < content.size();
        if (result.dump(-1, ' ', false, json::error_handler_t::replace).size() <= kMaxResultBytes) low = middle;
        else high = middle - 1;
      }
      while (low < content.size() && low && (static_cast<unsigned char>(content[low]) & 0xc0) == 0x80) --low;
      result["content"] = content.substr(0, low);
      result["truncated"] = truncated || low < content.size();
      return result;
    }
    json patchContext() {
      return {{"type", "integer"}, {"minimum", 0}, {"maximum", 20}, {"default", 3},
        {"description", "Surrounding lines per patch hunk, default 3. Only applies to patches, not historical file contents."}};
    }
    json definition(const char *name, const char *description, json properties, json required = json::array()) {
      properties["path"] = {{"type", "string"}, {"description", "Optional configured source file path (not a directory). Omit for the scoped repository; required to select among multiple repositories. Use list_files to discover paths."}};
      return {{"type", "function"}, {"function", {{"name", name}, {"description", description},
        {"parameters", {{"type", "object"}, {"properties", properties}, {"required", required}, {"additionalProperties", false}}}}}};
    }
  }

  json gitDiffDefinition() {
    return definition("git_diff", "Read unstaged or staged changes. With base, compare its merge base with HEAD against HEAD (committed branch changes only). Configured untracked filenames are reported separately for read_file; their contents are not in the patch. Only configured sources, up to 8 KiB with truncation.",
      {{"context", patchContext()}, {"staged", {{"type", "boolean"}, {"description", "Default false: unstaged changes; true: staged changes. Cannot be true with base."}}},
        {"base", {{"type", "string"}, {"description", "Optional local base commit, branch or tag (e.g. main). Compare merge-base(base, HEAD) to HEAD; excludes uncommitted changes."}}}});
  }
  json gitLogDefinition() {
    return definition("git_log", "Read recent local commits touching configured source files. Optional path narrows to one file. Returns commit IDs, authors, dates and messages, up to 8 KiB. Does not fetch remote history or follow renames.",
      {{"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 20}, {"description", "Maximum commits to return, default 5"}}}});
  }
  json gitShowDefinition() {
    return definition("git_show", "Inspect a local commit's metadata and patch restricted to configured sources. If path is supplied, return that allowed file's contents at the revision instead. Excluded and no-longer-configured files are unavailable. Up to 8 KiB with explicit truncation.",
      {{"context", patchContext()}, {"revision", {{"type", "string"}, {"description", "Commit ID from git_log, branch, tag, or HEAD ancestry such as HEAD~1"}}}}, json::array({"revision"}));
  }
  json executeGit(const json &call, const ToolContext &ctx, GitRepositoryCache *cache) {
    try {
      const auto preparationDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      if (!call.contains("function") || !call["function"].is_object()) throw std::invalid_argument("Tool call requires a function object");
      const auto &function = call.at("function");
      if (!function.contains("name") || !function["name"].is_string()) throw std::invalid_argument("Tool function requires a string name");
      if (!function.contains("arguments") || !function["arguments"].is_string()) throw std::invalid_argument("Tool arguments must be a JSON string");
      const auto name = function.at("name").get<std::string>();
      if (name != "git_diff" && name != "git_log" && name != "git_show") throw std::invalid_argument("Unknown Git tool");
      const auto raw = function.at("arguments").get<std::string>();
      if (raw.size() > 4096) throw std::invalid_argument("Tool arguments exceed the size limit");
      const auto args = json::parse(raw, nullptr, false);
      if (!args.is_object()) throw std::invalid_argument("Tool arguments must be a valid JSON object");
      for (const auto &[key, value] : args.items())
        if (key != "path" && !(name == "git_diff" && (key == "staged" || key == "base")) && !(name == "git_log" && key == "limit") && !(name == "git_show" && key == "revision") &&
            !((name == "git_diff" || name == "git_show") && key == "context"))
          throw std::invalid_argument("Unknown argument: " + key.substr(0, 128));
      const auto path = textArgument(args, "path", kMaxPathFilterBytes);
      if (args.contains("staged") && !args["staged"].is_boolean()) throw std::invalid_argument("staged must be a boolean");
      if (args.contains("limit") && (!args["limit"].is_number_integer() || args["limit"] < 1 || args["limit"] > 20))
        throw std::invalid_argument("limit must be an integer from 1 to 20");
      if (args.contains("context") && (!args["context"].is_number_integer() || args["context"] < 0 || args["context"] > 20))
        throw std::invalid_argument("context must be an integer from 0 to 20");
      if (name == "git_show" && !path.empty() && args.contains("context"))
        throw std::invalid_argument("context applies only to commit patches; omit path or context");
      const auto contextLines = args.value("context", 3);
      const auto base = textArgument(args, "base", 128);
      if (!base.empty() && args.value("staged", false)) throw std::invalid_argument("base cannot be combined with staged: true");
      const auto revision = textArgument(args, "revision", 128);
      if (name == "git_show" && revision.empty()) throw std::invalid_argument("revision is required");
      GitRepositoryCache localCache;
      const auto repo = repository(ctx, path, cache ? *cache : localCache, preparationDeadline);
      const auto root = runGit(repo.root, {"rev-parse", "--show-toplevel"}, preparationDeadline, {}, "preparation");
      if (root.code || root.truncated || !sameDirectory(fromText(utils::trimmed(root.output)), repo.root))
        throw std::runtime_error("Unable to resolve source Git working tree");
      std::vector<std::string> command;
      std::string hash, baseCommit, headCommit, mergeBase;
      std::vector<std::string> configuration;
      if (name == "git_diff") {
        command = {"diff", "--no-ext-diff", "--no-textconv", "--no-renames", "--ignore-submodules=all", "--no-color", "--unified=" + std::to_string(contextLines)};
        if (!base.empty()) {
          baseCommit = commit(repo, base, preparationDeadline, "base");
          headCommit = commit(repo, "HEAD", preparationDeadline);
          const auto common = runGit(repo.root, {"merge-base", "--all", baseCommit, headCommit}, preparationDeadline, {}, "preparation");
          mergeBase = utils::trimmed(common.output);
          if (common.code || common.truncated || !commitHash(mergeBase)) throw std::invalid_argument("Branch comparison requires a unique local merge base");
          command.push_back(mergeBase); command.push_back(headCommit);
        } else if (args.value("staged", false)) command.push_back("--cached");
        else configuration = disableFilters(repo, preparationDeadline);
      } else if (name == "git_log") {
        command = {"log", "--no-notes", "--no-show-signature", "--no-decorate", "--format=commit %H%nAuthor: %an%nDate: %aI%n%n%B", "-n", std::to_string(args.value("limit", 5))};
      } else {
        hash = commit(repo, revision, preparationDeadline);
        if (!path.empty()) command = {"show", "--no-ext-diff", "--no-textconv", hash + ":" + repo.selectedPath};
        else command = {"show", "--no-notes", "--no-show-signature", "--no-ext-diff", "--no-textconv", "--no-renames", "--ignore-submodules=all", "--no-color", "--format=fuller", "--unified=" + std::to_string(contextLines), hash};
      }
      if (name != "git_show" || path.empty()) {
        command.push_back("--"); command.insert(command.end(), repo.files.begin(), repo.files.end());
      }
      json untracked = json::array();
      bool untrackedTruncated = false;
      if (name == "git_diff") {
        std::vector<std::string> list = {"ls-files", "--others", "--exclude-standard", "-z", "--"};
        list.insert(list.end(), repo.files.begin(), repo.files.end());
        const auto names = runGit(repo.root, list, preparationDeadline, {}, "preparation");
        if (names.code && !names.truncated) throw std::runtime_error("Unable to list configured untracked files");
        untrackedTruncated = names.truncated;
        const std::set<std::string> scope(repo.files.begin(), repo.files.end());
        for (size_t start = 0; start < names.output.size();) {
          const auto end = names.output.find('\0', start);
          if (end == std::string::npos) { untrackedTruncated = true; break; }
          const auto file = names.output.substr(start, end - start);
          if (!scope.contains(file)) throw std::runtime_error("Git returned an untracked path outside the source scope");
          untracked.push_back(file);
          if (untracked.size() > 50 || untracked.dump().size() > 2048) {
            untracked.erase(untracked.end() - 1); untrackedTruncated = true; break;
          }
          start = end + 1;
        }
      }
      checkDeadline(preparationDeadline);
      const auto output = runGit(repo.root, command, std::chrono::steady_clock::now() + std::chrono::seconds(10), configuration);
      if (output.code && !output.truncated) throw std::runtime_error("Git inspection failed; the repository may have no commits or the file may not exist at that revision");
      if (output.output.find('\0') != std::string::npos) throw std::invalid_argument("Git tools support text output only");
      json result = {{"repository", pathText(repo.root.filename())}, {"scope", "configured_local_files"}};
      if (!hash.empty()) result["revision"] = hash;
      if (!repo.source.empty()) result["path"] = repo.source;
      if (name == "git_diff" || (name == "git_show" && path.empty())) result["context"] = contextLines;
      if (name == "git_diff") {
        result["mode"] = !base.empty() ? "branch" : args.value("staged", false) ? "staged" : "unstaged";
        result["untracked_files"] = std::move(untracked);
        result["untracked_truncated"] = untrackedTruncated;
        if (!base.empty()) { result["base"] = base; result["base_commit"] = baseCommit; result["merge_base"] = mergeBase; result["head"] = headCommit; }
      }
      result = boundedResult(std::move(result), output.output, output.truncated || untrackedTruncated);
      if (!result.contains("error") && !repo.source.empty() && name != "git_log" && !result["content"].get_ref<const std::string &>().empty() &&
          std::find(ctx.usedSources.begin(), ctx.usedSources.end(), repo.source) == ctx.usedSources.end()) ctx.usedSources.push_back(repo.source);
      return result;
    } catch (const std::exception &e) {
      auto error = utils::sanitizeUtf8(e.what());
      if (error.size() > 512) { size_t end = 512; while (end && (static_cast<unsigned char>(error[end]) & 0xc0) == 0x80) --end; error.resize(end); }
      return {{"error", error}};
    }
  }
}
