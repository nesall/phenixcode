#pragma once
#include <memory>

class App;
// Optional listener. Shares App; never opens another database/index.
class McpServer {
  struct Impl;
  std::unique_ptr<Impl> imp_;
public:
  explicit McpServer(App &app);
  ~McpServer();
  void start();
  void stop();
};
