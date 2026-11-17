#pragma once
#include "json_shim.h"
#include <httplib.h>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>

namespace mcp {
  enum class AuthDecision { Allow, Deny, Unavailable };
  using VerifyToken = std::function<AuthDecision(const std::string &)>;
  using Execute = std::function<nlohmann::json(const nlohmann::json &)>;

  class Handler {
    VerifyToken verify;
    int port;
    std::mutex rateMutex;
    size_t calls = 0;
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
  public:
    Handler(std::string credential, int listenPort);
    Handler(VerifyToken verifier, int listenPort);
    void handle(const httplib::Request &, httplib::Response &, const nlohmann::json &tools, const Execute &);
  };
}
