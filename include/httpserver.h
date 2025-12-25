#ifndef _HTTPSERVER_H_
#define _HTTPSERVER_H_

#include <memory>

class App;

class HttpServer {
  struct Impl;
  std::unique_ptr<Impl> imp;

public:
  HttpServer(App &app);
  ~HttpServer();
  // port=0 means bind to any available port
  int bindToPortIncremental(int port);
  bool startServer();
  void stop();

private:
  HttpServer(const HttpServer &) = delete;
  HttpServer &operator =(const HttpServer &) = delete;
};

#endif // _HTTPSERVER_H_