#include "mcp_protocol.h"
#include "mcp_credentials.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
using json = nlohmann::json;
int main() {
  try {
    mcp::Handler handler(std::string(32, 'a'), 8765);
    const auto tools = json::array({{{"function", {{"name", "read_file"},
      {"description", "Read scoped text"}, {"parameters", {{"type", "object"}}}}}}});
    int executions = 0;
    auto execute = [&](const json &call) {
      ++executions;
      if (call["function"]["arguments"] == "{\"fail\":true}") return json{{"error", "denied"}};
      return json{{"content", "evidence"}, {"truncated", true}};
    };
    auto request = [](const std::string &method) {
      httplib::Request r;
      r.headers = {{"Host", "127.0.0.1:8765"}, {"Authorization", "Bearer " + std::string(32, 'a')},
        {"Content-Type", "application/json"}, {"Accept", "application/json, text/event-stream"},
        {"MCP-Protocol-Version", "2026-07-28"}, {"Mcp-Method", method}};
      r.body = json({{"jsonrpc", "2.0"}, {"id", 1}, {"method", method},
        {"params", {{"_meta", {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
          {"io.modelcontextprotocol/clientCapabilities", json::object()}}}}}}).dump();
      return r;
    };
    auto run = [&](const httplib::Request &r) {
      httplib::Response res;
      res.status = 200;
      handler.handle(r, res, tools, execute);
      return res;
    };
    int checks = 0;
    auto check = [&](bool value) { ++checks; if (!value) throw std::runtime_error("check " + std::to_string(checks)); };
    auto r = request("server/discover");
    check(json::parse(run(r).body)["result"]["supportedVersions"] == json::array({"2026-07-28"}));
    r.headers.erase("Authorization"); check(run(r).status == 401);
    r = request("server/discover"); r.headers.emplace("Origin", "https://evil.example"); check(run(r).status == 403);
    r = request("server/discover"); r.headers.erase("Host"); check(run(r).status == 403);
    r = request("server/discover"); r.headers.emplace("Origin", "http://localhost:8765"); check(run(r).status == 200);
    r = request("tools/list");
    check(json::parse(run(r).body)["result"]["tools"][0]["inputSchema"]["type"] == "object");
    r.body = "{"; check(json::parse(run(r).body)["error"]["code"] == -32700);
    r.body = "[]"; check(json::parse(run(r).body)["error"]["code"] == -32600);
    r = request("tools/list"); auto body = json::parse(r.body);
    body["params"]["_meta"] = {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}};
    r.body = body.dump(); check(json::parse(run(r).body)["error"]["code"] == -32602);
    body["params"]["_meta"] = "bad"; r.body = body.dump();
    check(json::parse(run(r).body)["error"]["code"] == -32602);

    // Stateful / legacy client flow: initialize, notification, ping, tools/list, tools/call without Mcp-Name, unknown method 200.
    auto legacyReq = [&](const std::string &method, const json &params = json::object(), bool notification = false) {
      httplib::Request req;
      req.headers = {{"Host", "127.0.0.1:8765"}, {"Authorization", "bearer " + std::string(32, 'a')},
        {"Content-Type", "application/json"}, {"Accept", "application/json, text/event-stream"}};
      json payload = {{"jsonrpc", "2.0"}, {"method", method}, {"params", params}};
      if (!notification) payload["id"] = 1;
      req.body = payload.dump();
      return req;
    };
    auto initRes = run(legacyReq("initialize", {{"protocolVersion", "2025-06-18"}, {"capabilities", json::object()}}));
    auto initResult = json::parse(initRes.body)["result"];
    check(initRes.status == 200 && initResult["protocolVersion"] == "2025-06-18" &&
      initResult["serverInfo"]["name"] == "phenixcode" && initResult.contains("instructions") &&
      !initResult.contains("resultType") && !initResult.contains("_meta"));
    const auto instr = initResult["instructions"].get<std::string>();
    check(instr.find("symbol_outline") != std::string::npos && instr.find("read_symbol") != std::string::npos);
    auto initDefault = json::parse(run(legacyReq("initialize", {{"protocolVersion", "unknown"}})).body)["result"];
    check(initDefault["protocolVersion"] == "2025-11-25");
    auto notifRes = run(legacyReq("notifications/initialized", json::object(), true));
    check(notifRes.status == 202 && notifRes.body.empty());
    auto pingRes = run(legacyReq("ping"));
    check(pingRes.status == 200 && json::parse(pingRes.body)["result"] == json::object());
    auto legacyListRes = run(legacyReq("tools/list"));
    check(legacyListRes.status == 200 && !json::parse(legacyListRes.body)["result"].contains("resultType"));
    auto legacyCallRes = run(legacyReq("tools/call", {{"name", "read_file"}, {"arguments", json::object()}}));
    check(legacyCallRes.status == 200 && json::parse(legacyCallRes.body)["result"]["structuredContent"]["truncated"] == true);
    auto legacyMissing = run(legacyReq("missing"));
    check(legacyMissing.status == 200 && json::parse(legacyMissing.body)["error"]["code"] == -32601);

    r = request("tools/list"); r.headers.erase("Mcp-Method"); check(json::parse(run(r).body)["error"]["code"] == -32020);
    r = request("tools/list"); body = json::parse(r.body);
    body["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"] = "unknown";
    r.body = body.dump(); r.headers.erase("MCP-Protocol-Version"); r.headers.emplace("MCP-Protocol-Version", "unknown");
    check(json::parse(run(r).body)["error"]["code"] == -32022);
    check(run(request("missing")).status == 404);
    r = request("tools/call"); body = json::parse(r.body);
    body["params"]["name"] = "read_file"; body["params"]["arguments"] = json::object();
    r.body = body.dump(); check(json::parse(run(r).body)["error"]["code"] == -32020);
    r.headers.emplace("Mcp-Name", "read_file");
    auto result = json::parse(run(r).body)["result"];
    check(result["resultType"] == "complete" && result["structuredContent"]["truncated"] == true);
    check(json::parse(result["content"][0]["text"].get<std::string>()) == result["structuredContent"]);
    body["params"]["arguments"] = {{"fail", true}}; r.body = body.dump();
    check(json::parse(run(r).body)["result"]["isError"] == true);
    body["params"]["name"] = "missing"; r.body = body.dump();
    r.headers.erase("Mcp-Name"); r.headers.emplace("Mcp-Name", "missing");
    check(json::parse(run(r).body)["error"]["code"] == -32602);
    check(executions == 3);
    // Non-object tool output wraps structuredContent; json::exception inside execute yields 500 -32603.
    {
      mcp::Handler scalarHandler(std::string(32, 'a'), 8765);
      auto scalarExec = [](const json &call) -> json {
        if (call["function"]["arguments"] == "{\"throw\":true}") return json::array({1, 2}).at(99);
        return json::array({"a", "b"});
      };
      auto callScalar = [&](const json &args) {
        httplib::Response res;
        res.status = 200;
        auto req = legacyReq("tools/call", {{"name", "read_file"}, {"arguments", args}});
        scalarHandler.handle(req, res, tools, scalarExec);
        return res;
      };
      auto arrRes = callScalar(json::object());
      check(arrRes.status == 200 && json::parse(arrRes.body)["result"]["structuredContent"] == json({{"value", json::array({"a", "b"})}}) &&
        json::parse(arrRes.body)["result"]["isError"] == false);
      auto errRes = callScalar({{"throw", true}});
      check(errRes.status == 500 && json::parse(errRes.body)["error"]["code"] == -32603);
    }
    bool limited = false;
    for (int i = 0; i < 305; ++i) limited |= run(request("server/discover")).status == 429;
    check(limited);

    // Verify dynamic multi-listener live rotation and fail-closed corruption handling.
    const auto credDir = std::filesystem::temp_directory_path() /
      ("phenixcode-mcp-proto-" + mcp::credentials::generateSecureTokenHex(8));
    struct Cleanup { std::filesystem::path p; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(p, ec); } } cleanup{credDir};
    auto store = std::make_shared<mcp::credentials::Store>(credDir);
    const std::string projectId = "proto-live-proj";
    const auto initial = store->ensure(projectId);
    auto makeVerifier = [&](std::shared_ptr<mcp::credentials::Store> s) {
      return [s, projectId](const std::string &presented) -> mcp::AuthDecision {
        const auto r = s->verify(projectId, presented);
        if (r.status == mcp::credentials::VerifyStatus::Match) return mcp::AuthDecision::Allow;
        if (r.status == mcp::credentials::VerifyStatus::Error) return mcp::AuthDecision::Unavailable;
        return mcp::AuthDecision::Deny;
      };
    };
    mcp::Handler listener1(makeVerifier(store), 8765);
    mcp::Handler listener2(makeVerifier(std::make_shared<mcp::credentials::Store>(credDir)), 8766);
    auto runWithToken = [&](mcp::Handler &h, int port, const std::string &tok) {
      auto req = request("server/discover");
      req.headers.erase("Host");
      req.headers.emplace("Host", "127.0.0.1:" + std::to_string(port));
      req.headers.erase("Authorization");
      req.headers.emplace("Authorization", "Bearer " + tok);
      httplib::Response res;
      res.status = 200;
      h.handle(req, res, tools, execute);
      return res;
    };
    check(runWithToken(listener1, 8765, initial.token).status == 200);
    check(runWithToken(listener2, 8766, initial.token).status == 200);
    const auto rotated = store->regenerate(projectId);
    check(rotated.token != initial.token);
    check(runWithToken(listener1, 8765, initial.token).status == 401);
    check(runWithToken(listener2, 8766, initial.token).status == 401);
    check(runWithToken(listener1, 8765, rotated.token).status == 200);
    check(runWithToken(listener2, 8766, rotated.token).status == 200);
    {
      std::ofstream corrupt(store->credentialFilePath(projectId), std::ios::binary | std::ios::trunc);
      corrupt << "{bad";
    }
    check(runWithToken(listener1, 8765, rotated.token).status == 503);
    check(runWithToken(listener2, 8766, rotated.token).status == 503);

    std::cout << checks << " MCP protocol checks PASSED\n";
  } catch (const std::exception &e) { std::cerr << "FAILED: " << e.what() << '\n'; return 1; }
}
