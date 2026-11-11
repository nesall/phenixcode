#pragma once

#include "json_shim.h"
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <sqlite3.h>

namespace database_browser {
  std::string pathText(const std::filesystem::path &path);
  class Error : public std::runtime_error {
  public:
    std::string code;
    Error(std::string code, const std::string &message) : std::runtime_error(message), code(std::move(code)) {}
  };

  class Reader {
  public:
    Reader(const std::filesystem::path &database, const std::filesystem::path &baseDirectory);
    nlohmann::json list(const std::string &search, int offset, int limit);
    nlohmann::json chunks(const std::string &source, int offset, int limit);
    nlohmann::json readFile(const std::string &source);
    std::filesystem::path sourcePath(const std::string &source);

  private:
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db_{nullptr, sqlite3_close};
    std::filesystem::path database_, baseDirectory_;
    bool hasIndexedAt_ = false;
    void check(int result) const;
    void requireSource(const std::string &source);
    bool step(sqlite3_stmt *statement) const;
  };
}
