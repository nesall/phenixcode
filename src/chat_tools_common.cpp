#include "chat_tools.h"
#include "settings.h"
#include <algorithm>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <re2/re2.h>

using nlohmann::json;

namespace chat_tools {
  namespace {
    std::string shortPath(std::string value, const std::string &root) {
      std::replace(value.begin(), value.end(), '\\', '/');
      auto path = std::filesystem::path(std::u8string(value.begin(), value.end())).lexically_normal();
      if (path.is_absolute()) {
        const auto base = std::filesystem::path(std::u8string(root.begin(), root.end()));
        const auto relative = root.empty() ? std::filesystem::path{} : path.lexically_relative(base);
        if (!relative.empty() && *relative.begin() != "..") path = relative;
        else {
          // Multiple unrelated roots have no single project-relative spelling.
          path = path.parent_path().filename() / path.filename();
        }
      }
      const auto bytes = path.generic_u8string();
      value.assign(bytes.begin(), bytes.end());
      if (value.size() > 96) {
        size_t start = value.size() - 93;
        while (start < value.size() && (static_cast<unsigned char>(value[start]) & 0xc0) == 0x80) ++start;
        value = "..." + value.substr(start);
      }
      return detail::preview(value, 96);
    }
  } // namespace

  namespace detail {
    const std::string middle = " \xc2\xb7 ";

    std::string preview(std::string text, size_t maximum) {
      if (text.size() > maximum) {
        while (maximum && (static_cast<unsigned char>(text[maximum]) & 0xc0) == 0x80) --maximum;
        text.resize(maximum);
      }
      std::replace(text.begin(), text.end(), '\n', ' ');
      std::replace(text.begin(), text.end(), '\r', ' ');
      return text;
    }

    std::string compactAction(const std::string &name, const json &args, const std::string &root, const json *output) {
      if (!args.is_object()) return preview(name, 64);
      const auto text = [&](const char *key) {
        const auto it = args.find(key);
        return it != args.end() && it->is_string() ? it->get<std::string>() : std::string{};
      };
      const auto quoted = [&](const char *key) { return json(preview(text(key), 64)).dump(); };
      const auto scoped = [&](std::string label) {
        const auto filter = text("path_filter");
        const auto display = shortPath(filter, root);
        return filter.empty() || display == "." ? label : label + " in " + display;
      };
      if (name == "grep") {
        auto label = scoped("Search " + quoted("pattern"));
        if (!text("glob").empty()) label += " (" + preview(text("glob"), 32) + ")";
        return label;
      }
      if (name == "search_context") return "Search context " + quoted("query");
      if (name == "list_files") {
        auto label = scoped("List " + (text("extension").empty() ? std::string{} : preview(text("extension"), 16) + " ") + "files");
        if (!text("sort_by").empty() || !text("order").empty()) label += middle + "by " +
          (text("sort_by").empty() ? "path" : preview(text("sort_by"), 16)) + " " + (text("order").empty() ? "asc" : preview(text("order"), 8));
        return label;
      }
      if (name == "read_file") {
        auto label = "Read " + shortPath(text("path"), root);
        if (output && !output->contains("error")) label += ":" + std::to_string(output->value("start_line", size_t(0))) + "-" + std::to_string(output->value("end_line", size_t(0)));
        return label;
      }
      if (name == "read_files") {
        const size_t n = args.contains("files") && args["files"].is_array() ? args["files"].size() : size_t(0);
        return "Read " + std::to_string(n) + (n == 1 ? " file" : " files");
      }
      if (name == "symbol_outline") {
        auto label = "Outline " + shortPath(text("path"), root);
        if (!text("kind").empty()) label += middle + preview(text("kind"), 24);
        if (!text("role").empty() && text("role") != "all") label += middle + preview(text("role"), 24);
        return label;
      }
      if (name == "read_symbol") {
        auto label = "Read symbol " + preview(text("symbol"), 48) + middle + shortPath(text("path"), root);
        if (output && !output->contains("error") && !output->value("ambiguous", false) && output->contains("start_line"))
          label += ":" + std::to_string(output->value("start_line", size_t(0))) + "-" + std::to_string(output->value("end_line", size_t(0)));
        return label;
      }
      if (name == "find_symbol") {
        auto label = "Find symbol " + preview(text("name"), 48);
        if (!text("path_filter").empty()) label += middle + preview(text("path_filter"), 32);
        return label;
      }
      if (name == "dependencies") {
        auto label = "Dependencies " + shortPath(text("path"), root);
        if (!text("direction").empty() && text("direction") != "both") label += middle + preview(text("direction"), 16);
        return label;
      }
      if (name == "find_references") {
        auto label = "Find references " + preview(text("symbol"), 48);
        if (!text("path_filter").empty()) label += middle + preview(text("path_filter"), 32);
        return label;
      }
      if (name == "project_info") return "Project info";
      if (name == "project_settings") return "Project settings" + (text("section").empty() || text("section") == "all" ? std::string{} : middle + preview(text("section"), 32));
      std::string label = preview(name, 64);
      if (name == "git_diff") {
        label = "Git diff" + (text("base").empty() ? std::string{} : " vs " + preview(text("base"), 32));
        if (args.contains("staged") && args["staged"].is_boolean() && args["staged"].get<bool>()) label += " (staged)";
      }
      else if (name == "git_log") label = "Git history";
      else if (name == "git_show") label = "Git show " + preview(text("revision"), 32);
      else if (name == "git_blame") label = "Git blame";
      if (!text("path").empty()) label += middle + shortPath(text("path"), root);
      if (name == "git_blame" && output && !output->contains("error") && output->contains("start_line"))
        label += ":" + std::to_string(output->value("start_line", size_t(0))) + "-" + std::to_string(output->value("end_line", size_t(0)));
      return label;
    }

    std::string pathFilter(const json &args) {
      if (!args.contains("path_filter")) return {};
      if (!args["path_filter"].is_string()) throw std::invalid_argument("path_filter must be a string");
      auto filter = args["path_filter"].get<std::string>();
      if (filter.size() > kMaxPathFilterBytes)
        throw std::invalid_argument("path_filter must be at most " + std::to_string(kMaxPathFilterBytes) + " bytes");
      if (filter.find('\0') != std::string::npos || filter.find_first_of("\r\n") != std::string::npos)
        throw std::invalid_argument("path_filter contains invalid characters");
      std::replace(filter.begin(), filter.end(), '\\', '/');
      for (const auto &part : std::filesystem::path(filter))
        if (part == "..") throw std::invalid_argument("path_filter must not contain parent traversal");
      filter = std::filesystem::path(filter).lexically_normal().generic_string();
      return filter == "." ? std::string{} : filter;
    }

    std::unique_ptr<re2::RE2> compileGlob(const json &args, std::string &normalizedGlob) {
      normalizedGlob.clear();
      if (!args.contains("glob")) return nullptr;
      if (!args["glob"].is_string()) throw std::invalid_argument("glob must be a string");
      auto glob = args["glob"].get<std::string>();
      if (glob.empty()) return nullptr;
      if (glob.size() > 256) throw std::invalid_argument("glob must be at most 256 bytes");
      if (glob.find('\0') != std::string::npos || glob.find_first_of("\r\n") != std::string::npos)
        throw std::invalid_argument("glob contains invalid characters");
      std::replace(glob.begin(), glob.end(), '\\', '/');
      for (const auto &part : std::filesystem::path(glob))
        if (part == "..") throw std::invalid_argument("glob must not contain parent traversal");
      while (glob.starts_with("./")) glob.erase(0, 2);
      if (glob.empty()) return nullptr;
      normalizedGlob = glob;

      auto escapeLiteral = [](char c, std::string &out) {
        switch (c) {
          case '.': case '+': case '(': case ')': case '[': case ']':
          case '^': case '$': case '|': case '\\':
            out.push_back('\\');
            [[fallthrough]];
          default:
            out.push_back(c);
        }
      };

      const bool hasSlash = glob.find('/') != std::string::npos;
      const bool anchoredRoot = hasSlash && (glob.front() == '/' || (glob.size() >= 2 && glob[1] == ':'));
      std::string pattern = anchoredRoot ? "^" : "(?:^|/)";
      for (size_t i = 0; i < glob.size(); ++i) {
        const char c = glob[i];
        if (c == '*') {
          if (i + 1 < glob.size() && glob[i + 1] == '*') {
            if (i + 2 < glob.size() && glob[i + 2] == '/') {
              pattern += "(?:.*/)?";
              i += 2;
            } else {
              pattern += ".*";
              ++i;
            }
          } else {
            pattern += "[^/]*";
          }
        } else if (c == '?') {
          pattern += "[^/]";
        } else if (c == '{') {
          const auto close = glob.find('}', i + 1);
          if (close == std::string::npos || close == i + 1)
            throw std::invalid_argument("glob contains invalid brace group");
          const auto group = glob.substr(i + 1, close - i - 1);
          if (group.find('{') != std::string::npos)
            throw std::invalid_argument("glob does not support nested brace groups");
          pattern += "(?:";
          size_t start = 0;
          bool first = true;
          while (start <= group.size()) {
            const auto comma = group.find(',', start);
            const auto alt = group.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (!first) pattern.push_back('|');
            first = false;
            for (const char ch : alt) {
              if (ch == '*') pattern += "[^/]*";
              else if (ch == '?') pattern += "[^/]";
              else escapeLiteral(ch, pattern);
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
          }
          pattern.push_back(')');
          i = close;
        } else if (c == '}') {
          throw std::invalid_argument("glob contains unmatched closing brace");
        } else {
          escapeLiteral(c, pattern);
        }
      }
      pattern.push_back('$');
      re2::RE2::Options opt;
      opt.set_case_sensitive(false);
      opt.set_log_errors(false);
      opt.set_max_mem(1 << 20);
      auto re = std::make_unique<re2::RE2>(pattern, opt);
      if (!re->ok()) throw std::invalid_argument("Invalid glob pattern");
      return re;
    }

    std::vector<std::string> filterAllowedFiles(const std::vector<std::string> &allowedFiles,
                                                const std::string &filter,
                                                const std::unique_ptr<re2::RE2> &globRe) {
      std::vector<std::string> paths;
      std::set<std::string> selected;
      const bool relative = !std::filesystem::path(filter).is_absolute();
      for (const auto &file : allowedFiles) {
        const auto path = std::filesystem::path(file).lexically_normal().generic_string();
        const bool prefixOk = filter.empty() || path.starts_with(filter) ||
          (relative && path.find("/" + filter) != std::string::npos);
        const bool globOk = !globRe || re2::RE2::PartialMatch(path, *globRe);
        if (prefixOk && globOk && selected.insert(path).second) {
          paths.push_back(file);
        }
      }
      return paths;
    }
  } // namespace detail

  bool isToolEnabled(bool agentEnabled, const ApiConfig &api, bool freeChat, bool attachedOnly)
  {
    return agentEnabled && !freeChat && !attachedOnly && api.supportsTools == "on" && api.apiStyle == ApiConfig::ApiStyle::OpenAI;
  }
} // namespace chat_tools
