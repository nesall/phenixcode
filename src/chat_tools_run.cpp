#include "chat_tools.h"
#include "cutils.h"
#include <chrono>
#include <set>
#include <stdexcept>

using nlohmann::json;

namespace chat_tools {
  namespace {
    const std::string dash = " \xe2\x80\x94 ";
  } // namespace

  bool fitToolResults(json &messages, size_t budget, const inference_contant::TokenCounter &count)
  {
    auto fits = [&] { return inference_contant::estimateRequestTokens(messages, json::array(), count) <= budget; };
    for (auto &message : messages) {
      if (fits()) return true;
      if (message.value("role", "") != "tool" || !message.contains("content") || !message["content"].is_string()) continue;
      const auto original = message["content"].get<std::string>();
      size_t length = original.size();
      do {
        length /= 2;
        while (length && (static_cast<unsigned char>(original[length]) & 0xc0) == 0x80) --length;
        message["content"] = json({{"truncated", true}, {"notice", "Tool result shortened to fit the final answer context"},
          {"content", original.substr(0, length)}}).dump();
      } while (!fits() && length);
    }
    return fits();
  }

  Turn run(json messages, const Generate &generate,
    const std::function<json(const json &)> &execute, const std::function<bool()> &connected, size_t maxSteps, const Status &onStatus, const std::string &displayRoot)
  {
    Turn result;
    for (size_t step = 0; step < maxSteps && connected(); ++step) {
      auto turn = generate(messages);
      if (!connected()) return result;
      if (turn.incomplete || !turn.assistant.is_object()) {
        turn.incomplete = true;
        turn.text.clear(); // A truncated tool request can contain a preamble, not a final answer.
        turn.assistant = nullptr;
        return turn;
      }
      const auto calls = turn.assistant.find("tool_calls");
      if (calls == turn.assistant.end() || (calls->is_array() && calls->empty())) {
        return turn;
      }
      if (!calls->is_array() || calls->size() > kMaxCallsPerStep) throw std::runtime_error("Invalid or excessive tool calls");
      std::set<std::string> ids;
      for (const auto &call : *calls) {
        const auto id = call.at("id").get<std::string>();
        if (id.empty() || id.size() > 128 || !ids.insert(id).second || call.value("type", "") != "function")
          throw std::runtime_error("Invalid tool call identity");
      }
      if (step + 1 == maxSteps) { result.limitReached = true; return result; }
      messages.push_back(turn.assistant);
      size_t callIndex = 0;
      for (const auto &call : *calls) {
        if (!connected()) return result;
        ++callIndex;
        const auto name = call.at("function").value("name", std::string("tool"));
        const auto callId = call.at("id").get<std::string>();
        std::string description = detail::preview(name, 64) + "(";
        json args = json::object();
        std::string argsStatus;
        try {
          const auto &function = call.at("function");
          if (!function.contains("arguments")) argsStatus = "missing";
          else {
            const auto &raw = function.at("arguments").get_ref<const std::string &>();
            if (raw.size() > 4096) argsStatus = "oversized";
            else {
              args = json::parse(raw);
              if (!args.is_object()) argsStatus = "invalid";
            }
          }
          bool first = true;
          if (args.is_object()) for (const auto &[key, value] : args.items()) {
            if (!first) description += ", ";
            first = false;
            description += detail::preview(key, 64) + "=";
            description += value.dump();
          }
          if (name == "grep" && !args.contains("path_filter")) description += (first ? "" : ", ") + std::string("path_filter=<all configured files>");
        } catch (const std::exception &) { argsStatus = "invalid"; description += "invalid arguments"; }
        description = detail::preview(description + ")", 4608);
        const auto prefix = "Round " + std::to_string(step + 1) + "/" + std::to_string(maxSteps) +
          " \xc2\xb7 Call " + std::to_string(callIndex) + "/" + std::to_string(calls->size()) + ": " + description;
        const auto compactPrefix = "Round " + std::to_string(step + 1) + detail::middle +
          std::to_string(callIndex) + "/" + std::to_string(calls->size()) + detail::middle;
        const auto action = detail::compactAction(name, args, displayRoot);
        json event = {{"event_id", "tool:" + std::to_string(step + 1) + ":" + callId}, {"tool", name}, {"action", action},
          {"round", step + 1}, {"max_rounds", maxSteps}, {"call_id", callId}, {"call_index", callIndex}, {"call_count", calls->size()},
          {"status", "running"}, {"content", prefix}, {"summary", compactPrefix + action}};
        if (argsStatus.empty()) event["args"] = args;
        else event["args_status"] = argsStatus;
        if (onStatus) onStatus(event);
        if (!connected()) return result;
        json output;
        const auto started = std::chrono::steady_clock::now();
        try { output = execute(call); }
        catch (const std::exception &e) { output = {{"error", detail::preview(utils::sanitizeUtf8(e.what()), 512)}}; }
        event["duration_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        std::string summary;
        std::string status = "success";
        if (output.contains("error")) { summary = "failed: " + output.at("error").get<std::string>(); status = "error"; }
        else if (name == "grep") {
          if (output.value("outcome", "") == "no_files_matched_filter") { summary = "no files matched the filter"; status = "info"; }
          else {
            summary = std::to_string(output.at("matches").size()) + " snippets; searched " +
              std::to_string(output.value("searched_files", size_t(0))) + "/" + std::to_string(output.value("matched_files", size_t(0))) + " files";
            const auto unreadable = output.value("unreadable_files", size_t(0));
            if (unreadable) { summary += "; " + std::to_string(unreadable) + " unreadable"; status = "warning"; }
            else if (output.at("matches").empty()) status = "info";
          }
        } else if (name == "list_files") {
          summary = std::to_string(output.at("count").get<size_t>()) + " files returned";
          if (output.at("count") == 0) status = "info";
        } else if (name == "search_context") {
          summary = std::to_string(output.at("matches").size()) + " excerpts returned (" + output.at("mode").get<std::string>() + ")";
          if (output.at("matches").empty()) status = "info";
        } else if (name == "read_file") summary = "returned lines " + std::to_string(output.at("start_line").get<size_t>()) + "-" + std::to_string(output.at("end_line").get<size_t>());
        else if (name == "read_files") summary = std::to_string(output.value("count", size_t(0))) + "/" + std::to_string(output.value("requested_count", size_t(0))) + " files returned";
        else if (name == "symbol_outline") {
          summary = std::to_string(output.value("symbol_count", size_t(0))) + " symbols returned";
          if (output.value("symbol_count", size_t(0)) == 0) status = "info";
        } else if (name == "read_symbol") {
          if (output.value("ambiguous", false)) {
            summary = std::to_string(output.value("candidate_count", size_t(0))) + " candidates returned";
            status = "info";
          } else {
            summary = "returned lines " + std::to_string(output.at("start_line").get<size_t>()) + "-" + std::to_string(output.at("end_line").get<size_t>());
          }
        } else if (name == "find_symbol") {
          summary = std::to_string(output.value("match_count", size_t(0))) + " matches; searched " +
            std::to_string(output.value("searched_files", size_t(0))) + "/" +
            std::to_string(output.value("supported_files", size_t(0))) + " files";
          if (output.value("incomplete", false)) {
            summary += " (incomplete)";
            status = "warning";
          } else if (output.value("match_count", size_t(0)) == 0) {
            status = "info";
          }
        } else if (name == "dependencies") {
          summary = std::to_string(output.value("outgoing_count", size_t(0))) + " outgoing, " +
            std::to_string(output.value("incoming_count", size_t(0))) + " incoming (" +
            output.value("graph_source", std::string("live")) + ")";
          if (output.value("incomplete", false)) {
            summary += " (incomplete)";
            status = "warning";
          }
        } else if (name == "find_references") {
          summary = std::to_string(output.value("reference_count", size_t(0))) + " references; searched " +
            std::to_string(output.value("searched_files", size_t(0))) + "/" +
            std::to_string(output.value("supported_files", size_t(0))) + " files";
          if (output.value("incomplete", false)) {
            summary += " (incomplete)";
            status = "warning";
          } else if (output.value("reference_count", size_t(0)) == 0) {
            status = "info";
          }
        }
        else if (name == "project_info" || name == "project_settings") summary = "loaded summary returned";
        else summary = "inspection returned";
        if (output.value("truncated", false)) { summary += " (truncated)"; if (status != "error") status = "warning"; }
        event["status"] = status;
        event["content"] = prefix + " \xe2\x80\x94 " + summary;
        std::string compactResult;
        if (output.contains("error")) compactResult = "failed: " + detail::preview(output.at("error").get<std::string>(), 100);
        else if (name == "grep") {
          if (output.value("outcome", "") == "no_files_matched_filter") compactResult = "no files matched";
          else {
            const auto searched = output.value("searched_files", size_t(0));
            const auto matched = output.value("matched_files", size_t(0));
            compactResult = std::to_string(output.at("matches").size()) + " snippets" + detail::middle + std::to_string(searched);
            if (searched != matched) compactResult += "/" + std::to_string(matched);
            compactResult += " files";
            const auto unreadable = output.value("unreadable_files", size_t(0));
            if (unreadable) compactResult += detail::middle + std::to_string(unreadable) + " unreadable";
          }
        } else if (name == "list_files") compactResult = std::to_string(output.at("count").get<size_t>()) + " files";
        else if (name == "search_context") compactResult = std::to_string(output.at("matches").size()) + " excerpts";
        else if (name == "read_files") compactResult = std::to_string(output.value("count", size_t(0))) + "/" + std::to_string(output.value("requested_count", size_t(0))) + " files";
        else if (name == "symbol_outline") compactResult = std::to_string(output.value("symbol_count", size_t(0))) + " symbols";
        else if (name == "read_symbol" && output.value("ambiguous", false)) compactResult = std::to_string(output.value("candidate_count", size_t(0))) + " candidates";
        else if (name == "find_symbol") {
          compactResult = std::to_string(output.value("match_count", size_t(0))) + " matches" + detail::middle +
            std::to_string(output.value("searched_files", size_t(0))) + "/" +
            std::to_string(output.value("supported_files", size_t(0))) + " files";
          if (output.value("incomplete", false)) compactResult += " (incomplete)";
        } else if (name == "dependencies") {
          compactResult = std::to_string(output.value("outgoing_count", size_t(0))) + " out, " +
            std::to_string(output.value("incoming_count", size_t(0))) + " in";
          if (output.value("incomplete", false)) compactResult += " (incomplete)";
        } else if (name == "find_references") {
          compactResult = std::to_string(output.value("reference_count", size_t(0))) + " refs" + detail::middle +
            std::to_string(output.value("searched_files", size_t(0))) + "/" +
            std::to_string(output.value("supported_files", size_t(0))) + " files";
          if (output.value("incomplete", false)) compactResult += " (incomplete)";
        }
        const auto completedAction = detail::compactAction(name, args, displayRoot, &output);
        auto compact = compactPrefix + completedAction;
        if (!compactResult.empty()) compact += dash + compactResult;
        if (output.value("truncated", false)) compact += " (truncated)";
        event["summary"] = compact;
        event["action"] = completedAction;
        event["outcome_summary"] = compactResult.empty() ? summary : compactResult;
        if (!compactResult.empty() && output.value("truncated", false)) event["outcome_summary"] = compactResult + " (truncated)";
        if (name == "grep" && !output.contains("error")) {
          for (const auto *key : {"outcome", "matched_files", "searched_files", "unreadable_files", "search_limited", "path_filter", "glob"})
            if (output.contains(key)) event[key] = output[key];
        }
        if (onStatus) {
          auto preview = output.dump(2, ' ', false, json::error_handler_t::replace);
          constexpr size_t maxPreviewBytes = 4096;
          event["result_preview_truncated"] = preview.size() > maxPreviewBytes;
          if (preview.size() > maxPreviewBytes) {
            size_t end = maxPreviewBytes;
            while (end && (static_cast<unsigned char>(preview[end]) & 0xc0) == 0x80) --end;
            preview.resize(end);
          }
          event["result_preview"] = std::move(preview);
          onStatus(event);
        }
        messages.push_back({{"role", "tool"}, {"tool_call_id", call.at("id")}, {"content", output.dump()}});
      }
    }
    return result;
  }
} // namespace chat_tools
