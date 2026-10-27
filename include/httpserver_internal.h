#pragma once
#include "httpserver.h"
#include <httplib.h>
#include <atomic>
#include <chrono>
#include <string>

struct HttpServer::Impl {
  Impl(App &a)
    : app_(a)
  {}

  httplib::Server server_;

  App &app_;

  static std::atomic<size_t> requestCounter_;
  static std::atomic<size_t> searchCounter_;
  static std::atomic<size_t> chatCounter_;
  static std::atomic<size_t> embedCounter_;
  static std::atomic<size_t> errorCounter_;

  static std::chrono::steady_clock::time_point startTime_;

  // Moving averages for performance
  static std::atomic<double> avgSearchTimeMs_;
  static std::atomic<double> avgChatTimeMs_;
  static std::atomic<double> avgEmbedTimeMs_;
};


namespace httpserver_detail {
  void recordDuration(const std::chrono::steady_clock::time_point &, std::atomic<double> &);
  bool requireAuth(AdminAuth &, const httplib::Request &, httplib::Response &, std::string *);
}
