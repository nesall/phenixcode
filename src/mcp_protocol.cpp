#include "mcp_protocol.h"
#include "3rdparty/base64.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>
#include <utils_log/logger.hpp>

using json = nlohmann::json;

namespace {
  constexpr auto version = "2026-07-28";
  constexpr auto instructions =
    "Read-only context for one loaded PhenixCode project. Indexed excerpts may be stale; "
    "read_file, read_files, symbol_outline, read_symbol, find_symbol, dependencies, and find_references inspect current source files "
    "(prefer read_symbol or read_file over large grep context_lines to read full definitions or longer ranges). "
    "Semantic search may call the configured embedding provider.";
  constexpr size_t kRateLimitPerMinute = 300;

  json identity() { return { { "name", "phenixcode" },{ "version", EMBEDDER_VERSION } }; }

  bool secretEqual(const std::string &a, const std::string &b) {
    const bool sameLen = (a.size() == b.size());
    const std::string &lhs = sameLen ? a : b;
    unsigned char diff = sameLen ? 0 : 1;
    for (size_t i = 0; i < b.size(); ++i) {
      diff |= static_cast<unsigned char>(lhs[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
  }

  bool isBearerScheme(std::string_view header) {
    constexpr std::string_view kBearer = "bearer ";
    if (header.size() <= kBearer.size()) return false;
    for (size_t i = 0; i < kBearer.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(header[i])) != static_cast<unsigned char>(kBearer[i])) {
        return false;
      }
    }
    return true;
  }

  std::string sanitizeLog(std::string_view value, size_t maxLen = 128) {
    std::string out;
    const size_t n = (std::min)(value.size(), maxLen);
    out.reserve(n + 3);
    for (size_t i = 0; i < n; ++i) {
      const unsigned char c = static_cast<unsigned char>(value[i]);
      out.push_back((c < 0x20 || c == 0x7f) ? '?' : static_cast<char>(c));
    }
    if (value.size() > maxLen) out += "...";
    return out;
  }
}

mcp::Handler::Handler(std::string credential, int listenPort)
  : verify([expected = std::move(credential)](const std::string &presented) {
      return secretEqual(presented, expected) ? AuthDecision::Allow : AuthDecision::Deny;
    }),
    port(listenPort) {}

mcp::Handler::Handler(VerifyToken verifier, int listenPort)
  : verify(std::move(verifier)), port(listenPort) {}

void mcp::Handler::handle(const httplib::Request &req, httplib::Response &res, const json &tools, const Execute &execute)
{
  res.set_header("Cache-Control", "no-store");
  const auto host = req.get_header_value("Host");
  if (host != "127.0.0.1:" + std::to_string(port) && host != "localhost:" + std::to_string(port)) {
    LOG_MSG_WARN << "MCP request with invalid host from" << req.remote_addr << "host:" << sanitizeLog(host);
    res.status = 403;
    return;
  }
  // Native clients need not send Origin. Browser origins must match this listener.
  if (req.has_header("Origin")) {
    const auto origin = req.get_header_value("Origin");
    if (origin != "http://127.0.0.1:" + std::to_string(port) &&
      origin != "http://localhost:" + std::to_string(port)) {
      LOG_MSG_WARN << "MCP request with invalid origin from" << req.remote_addr << "origin:" << sanitizeLog(origin);
      res.status = 403;
      return;
    }
  }
  const auto authHeader = req.get_header_value("Authorization");
  constexpr size_t kBearerPrefixLen = 7; // "Bearer "
  if (!isBearerScheme(authHeader)) {
    LOG_MSG_WARN << "MCP request with missing or invalid authorization scheme from" << req.remote_addr;
    res.status = 401;
    res.set_header("WWW-Authenticate", "Bearer realm=\"PhenixCode MCP\"");
    return;
  }
  const auto decision = verify ? verify(authHeader.substr(kBearerPrefixLen)) : AuthDecision::Unavailable;
  if (decision == AuthDecision::Unavailable) {
    LOG_MSG_ERROR << "MCP credential store unavailable while authenticating request from" << req.remote_addr;
    res.status = 503;
    return;
  }
  if (decision != AuthDecision::Allow) {
    LOG_MSG_WARN << "MCP request with invalid token from" << req.remote_addr;
    res.status = 401;
    res.set_header("WWW-Authenticate", "Bearer realm=\"PhenixCode MCP\"");
    return;
  }
  {
    std::lock_guard<std::mutex> lock(rateMutex);
    const auto now = std::chrono::steady_clock::now();
    if (now - window >= std::chrono::minutes(1)) { window = now; calls = 0; }
    if (++calls > kRateLimitPerMinute) {
      LOG_MSG_WARN << "MCP request rate limit exceeded from" << req.remote_addr;
      res.status = 429;
      res.set_header("Retry-After", "60");
      return;
    }
  }
  json id;
  const auto error = [&](int status, int code, const std::string &message, const json &data = json()) {
    json e = { { "code", code },{ "message", message } };
    if (!data.is_null()) e["data"] = data;
    json response = { { "jsonrpc", "2.0" },{ "error", e } };
    if (!id.is_null()) response["id"] = id;
    res.status = status; res.set_content(response.dump(), "application/json");
    };
  const auto contentType = req.get_header_value("Content-Type");
  if (contentType.find("application/json") != 0) {
    LOG_MSG_WARN << "MCP request with invalid content type from" << req.remote_addr << "content-type:" << sanitizeLog(contentType);
    res.status = 415;
    return;
  }
  const auto accept = req.get_header_value("Accept");
  if (accept.find("application/json") == std::string::npos || accept.find("text/event-stream") == std::string::npos) {
    LOG_MSG_WARN << "MCP request with invalid accept header from" << req.remote_addr << "accept:" << sanitizeLog(accept);
    res.status = 406;
    return;
  }
  const auto body = json::parse(req.body, nullptr, false);
  if (body.is_discarded()) { error(400, -32700, "Parse error"); return; }
  if (!body.is_object() || body.value("jsonrpc", json()) != "2.0" ||
    !body.contains("method") || !body["method"].is_string()) {
    LOG_MSG_WARN << "MCP request with invalid JSON-RPC structure from" << req.remote_addr;
    error(400, -32600, "Expected a JSON-RPC message");
    return;
  }
  if (!body.contains("id")) {
    res.status = 202;
    return;
  }
  if (!(body["id"].is_string() || body["id"].is_number_integer())) {
    LOG_MSG_WARN << "MCP request with invalid JSON-RPC id from" << req.remote_addr;
    error(400, -32600, "id must be a string or integer");
    return;
  }
  id = body["id"];
  try {
    const auto method = body["method"].get<std::string>();
    const auto params = body.value("params", json::object());
    if (!params.is_object()) { error(400, -32602, "params must be an object"); return; }
    if (params.contains("_meta") && !params["_meta"].is_object()) {
      LOG_MSG_WARN << "MCP request with non-object _meta from" << req.remote_addr;
      error(400, -32602, "_meta must be an object");
      return;
    }
    const auto meta = params.value("_meta", json::object());
    const bool stateless = meta.contains("io.modelcontextprotocol/protocolVersion");
    if (stateless) {
      if (!meta["io.modelcontextprotocol/protocolVersion"].is_string() ||
        !meta.contains("io.modelcontextprotocol/clientCapabilities") ||
        !meta["io.modelcontextprotocol/clientCapabilities"].is_object()) {
        LOG_MSG_WARN << "MCP request with invalid protocol metadata from" << req.remote_addr;
        error(400, -32602, "Required protocol metadata is missing or invalid");
        return;
      }
      const auto requested = meta["io.modelcontextprotocol/protocolVersion"].get<std::string>();
      if (req.get_header_value("MCP-Protocol-Version") != requested || req.get_header_value("Mcp-Method") != method) {
        LOG_MSG_WARN << "MCP request with mismatched headers from" << req.remote_addr;
        error(400, -32020, "Header mismatch"); return;
      }
      if (requested != version) {
        LOG_MSG_WARN << "MCP request with unsupported protocol version from" << req.remote_addr;
        error(400, -32022, "Unsupported protocol version",
          { { "supported", json::array({ version }) },{ "requested", requested } });
        return;
      }
    }
    json result = stateless
      ? json{ { "resultType", "complete" },{ "_meta",{ { "io.modelcontextprotocol/serverInfo", identity() } } } }
      : json::object();

    if (!stateless && method == "initialize") {
      static const std::array<std::string, 4> legacy = { "2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05" };
      const auto asked = params.value("protocolVersion", std::string());
      const bool known = std::find(legacy.begin(), legacy.end(), asked) != legacy.end();
      result["protocolVersion"] = known ? asked : legacy.front();
      result["capabilities"] = { { "tools", json::object() } };
      result["serverInfo"] = identity();
      result["instructions"] = instructions;
    } else if (!stateless && method == "ping") {
      // empty result object
    } else if (method == "server/discover") {
      result["supportedVersions"] = json::array({ version });
      result["capabilities"] = { { "tools", json::object() } };
      result["instructions"] = instructions;
    } else if (method == "tools/list") {
      if (params.contains("cursor")) {
        LOG_MSG_WARN << "MCP request with unsupported pagination cursor from" << req.remote_addr;
        error(400, -32602, "No pagination cursor is supported");
        return;
      }
      result["tools"] = json::array();
      for (const auto &tool : tools) {
        const auto &f = tool.at("function");
        result["tools"].push_back({ { "name", f.at("name") },{ "description", f.at("description") },
          { "inputSchema", f.at("parameters") },{ "annotations",{ { "readOnlyHint", true },{ "destructiveHint", false } } } });
      }
    } else if (method == "tools/call") {
      if (!params.contains("name") || !params["name"].is_string() ||
        (params.contains("arguments") && !params["arguments"].is_object())) {
        LOG_MSG_WARN << "MCP request with invalid tool call parameters from" << req.remote_addr;
        error(400, -32602, "Expected tool name and object arguments");
        return;
      }
      const auto name = params["name"].get<std::string>();
      if (stateless) {
        auto headerName = req.get_header_value("Mcp-Name");
        if (headerName.starts_with("=?base64?") && headerName.ends_with("?=")) {
          try { headerName = base64_decode(headerName.substr(9, headerName.size() - 11)); } catch (...) {
            LOG_MSG_WARN << "MCP request with invalid base64-encoded tool name from" << req.remote_addr;
            error(400, -32020, "Invalid encoded Mcp-Name");
            return;
          }
        }
        if (headerName != name) {
          LOG_MSG_WARN << "MCP request with mismatched tool name header from" << req.remote_addr;
          error(400, -32020, "Header mismatch");
          return;
        }
      }
      if (std::none_of(tools.begin(), tools.end(), [&](const json &t) { return t["function"]["name"] == name; })) {
        LOG_MSG_WARN << "MCP request with unknown tool from" << req.remote_addr << "tool:" << sanitizeLog(name);
        error(400, -32602, "Unknown tool");
        return;
      }
      const auto args = params.value("arguments", json::object()).dump();
      if (args.size() > 4096) {
        LOG_MSG_WARN << "MCP request with tool arguments exceeding 4096 bytes from" << req.remote_addr << "tool:" << sanitizeLog(name);
        error(400, -32602, "Tool arguments exceed 4096 bytes");
        return;
      }
      json output;
      try {
        output = execute({ { "function",{ { "name", name },{ "arguments", args } } } });
      } catch (const std::exception &e) {
        LOG_MSG_WARN << "MCP tool execution failed:" << e.what();
        error(500, -32603, "Internal error; see backend logs");
        return;
      }
      result["structuredContent"] = output.is_object() ? output : json{ { "value", output } };
      result["content"] = json::array({ { { "type", "text" },{ "text", output.dump() } } });
      result["isError"] = output.is_object() && output.contains("error");
    } else {
      LOG_MSG_WARN << "MCP request with unknown method from" << req.remote_addr << "method:" << sanitizeLog(method);
      error(stateless ? 404 : 200, -32601, "Method not found; supported MCP version: 2026-07-28");
      return;
    }
    res.set_content(json({ { "jsonrpc", "2.0" },{ "id", id },{ "result", result } }).dump(), "application/json");
  } catch (const json::exception &) { error(400, -32602, "Invalid parameter types"); } catch (const std::exception &e) {
    LOG_MSG_WARN << "MCP request failed:" << e.what();
    error(500, -32603, "Internal error; see backend logs");
  }
}
