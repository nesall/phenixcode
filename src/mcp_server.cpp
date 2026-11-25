#include "mcp_server.h"
#include "mcp_credentials.h"
#include "mcp_protocol.h"
#include "tool_service.h"
#include "app.h"
#include "monitor.h"
#include "settings.h"
#include <httplib.h>
#include <thread>
#include <stdexcept>
#include <utils_log/logger.hpp>

using json = nlohmann::json;
struct McpServer::Impl {
  App &app;
  httplib::Server server;
  std::thread thread;
  std::unique_ptr<mcp::Handler> handler;
  int port = 0;
  explicit Impl(App &a) : app(a) {}
};

McpServer::McpServer(App &app) : imp_(std::make_unique<Impl>(app)) {}

McpServer::~McpServer() { stop(); }

void McpServer::start()
{
  if (!imp_->app.settings().mcpEnabled()) {
    LOG_MSG_DEBUG << "MCP is disabled in settings; not starting MCP endpoint";
    return;
  }
  imp_->port = imp_->app.settings().mcpPort();

  const std::string projectId = imp_->app.settings().getProjectId();
  if (!mcp::credentials::isValidProjectId(projectId)) {
    LOG_MSG_ERROR << "MCP requires a valid non-empty source.project_id";
    throw std::runtime_error("MCP requires a valid non-empty source.project_id");
  }
  auto store = std::make_shared<mcp::credentials::Store>();
  (void)store->ensure(projectId);
  imp_->handler = std::make_unique<mcp::Handler>(
    [store, projectId](const std::string &presented) -> mcp::AuthDecision {
      const auto result = store->verify(projectId, presented);
      switch (result.status) {
        case mcp::credentials::VerifyStatus::Match:
          return mcp::AuthDecision::Allow;
        case mcp::credentials::VerifyStatus::Mismatch:
        case mcp::credentials::VerifyStatus::Missing:
          return mcp::AuthDecision::Deny;
        case mcp::credentials::VerifyStatus::Error:
          LOG_MSG_ERROR << "MCP credential verification failed for project" << projectId << ":" << result.error;
          return mcp::AuthDecision::Unavailable;
      }
      return mcp::AuthDecision::Unavailable;
    },
    imp_->port);

  imp_->server.new_task_queue = [] { return new httplib::ThreadPool(2, 32); };
  imp_->server.set_payload_max_length(16384);
  imp_->server.set_read_timeout(10, 0);
  imp_->server.set_write_timeout(10, 0);

  imp_->server.Post("/mcp", [this](const auto &req, auto &res) {
    chat_tools::Service service(imp_->app, MonitorToolCaller::Mcp);
    const auto tools = chat_tools::definitions();
    imp_->handler->handle(req, res, tools, [&](const json &call) { return service.execute(call, tools); });
  });
  const auto unsupported = [](const httplib::Request &, httplib::Response &res) {
    res.status = 405;
    res.set_header("Allow", "POST");
  };
  imp_->server.Get("/mcp", unsupported);
  imp_->server.Delete("/mcp", unsupported);
  if (!imp_->server.bind_to_port("127.0.0.1", imp_->port)) {
    LOG_MSG_ERROR << "Unable to bind configured MCP loopback port " << imp_->port;
    throw std::runtime_error("Unable to bind configured MCP loopback port");
  }
  imp_->thread = std::thread([this] { imp_->server.listen_after_bind(); });
  imp_->server.wait_until_ready();
  LOG_MSG << "MCP endpoint: http://127.0.0.1:" << imp_->port << "/mcp (2026-07-28)";
}

void McpServer::stop()
{
  imp_->server.stop();
  if (imp_->thread.joinable()) imp_->thread.join();
  LOG_MSG_DEBUG << "MCP endpoint stopped";
}
