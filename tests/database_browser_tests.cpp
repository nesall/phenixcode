#include "database_browser.h"
#include "cutils.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using nlohmann::json;
using database_browser::Reader;

namespace {
  int checks = 0;
  void check(bool value, const char *description) {
    ++checks;
    if (!value) throw std::runtime_error(description);
  }
  struct Fixture {
    fs::path directory = fs::temp_directory_path() /
      ("phenixcode-browser-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::path database = directory / "indexed.sqlite";
    sqlite3 *writer = nullptr;
    Fixture() {
      if (!fs::create_directory(directory)) throw std::runtime_error("Unable to create isolated test directory");
      if (sqlite3_open(database.string().c_str(), &writer) != SQLITE_OK) throw std::runtime_error("Unable to create test database");
      sql("CREATE TABLE files_metadata(path TEXT PRIMARY KEY,last_modified INTEGER,file_size INTEGER,nof_lines INTEGER,indexed_at TEXT DEFAULT CURRENT_TIMESTAMP);");
      sql("CREATE TABLE chunks(id INTEGER PRIMARY KEY,source_id TEXT,content TEXT,start_pos INTEGER,end_pos INTEGER,token_count INTEGER,unit TEXT,type TEXT);");
      sql("CREATE INDEX idx_chunks_source_id ON chunks(source_id);");
    }
    ~Fixture() {
      sqlite3_close(writer);
      // Only the newly created, uniquely named fixture directory is removed.
      if (directory.parent_path() == fs::temp_directory_path() && directory.filename().string().starts_with("phenixcode-browser-test-")) {
        std::error_code error;
        fs::remove_all(directory, error);
      }
    }
    void sql(const std::string &query) {
      char *error = nullptr;
      const int rc = sqlite3_exec(writer, query.c_str(), nullptr, nullptr, &error);
      if (rc != SQLITE_OK) {
        const std::string message = error ? error : "Fixture SQL failed";
        sqlite3_free(error);
        throw std::runtime_error(message);
      }
    }
    void chunk(const std::string &source, const std::string &content = "int x = 1;", int start = 1) {
      sqlite3_stmt *stmt = nullptr;
      sqlite3_prepare_v2(writer, "INSERT INTO chunks(source_id,content,start_pos,end_pos,token_count,unit,type) VALUES(?,?,?,3,5,'line','code')", -1, &stmt, nullptr);
      sqlite3_bind_text(stmt, 1, source.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(stmt, 2, content.data(), static_cast<int>(content.size()), SQLITE_TRANSIENT);
      sqlite3_bind_int(stmt, 3, start);
      const int result = sqlite3_step(stmt);
      sqlite3_finalize(stmt);
      if (result != SQLITE_DONE) throw std::runtime_error("Fixture insert failed");
    }
  };

  template<class Action> void expectError(const std::string &code, Action action) {
    try { action(); } catch (const database_browser::Error &error) {
      check(error.code == code, "expected browser error code");
      return;
    }
    throw std::runtime_error("Expected browser error was not raised");
  }
}

int main() {
  try {
    Fixture fixture;
    expectError("not_created", [&] { Reader missing(fixture.directory / "absent.sqlite", fixture.directory); });
    check(!fs::exists(fixture.directory / "absent.sqlite"), "reader never creates missing databases");
    { std::ofstream invalid(fixture.directory / "invalid.sqlite"); invalid << "not SQLite"; }
    expectError("unsupported_schema", [&] { Reader invalid(fixture.directory / "invalid.sqlite", fixture.directory); });
    {
      Reader reader(fixture.database, fixture.directory);
      check(reader.list("", 0, 50)["total"] == 0, "empty database is supported");
    }
    for (int i = 0; i < 125; ++i) fixture.chunk("src/file" + std::to_string(i) + ".cpp");
    fixture.chunk("https://example.com/docs");
    fixture.chunk("src/100%_literal.cpp");
    fixture.chunk("src/O'Brien.cpp");
    fixture.chunk("src/file0.cpp", "second chunk", 4);
    fixture.sql("INSERT INTO files_metadata(path,last_modified,file_size,nof_lines) VALUES('metadata-only.cpp',0,10,2)");
    Reader reader(fixture.database, fixture.directory);
    const auto first = reader.list("", -1, 50);
    check(first["total"] == 129 && first["sources"].size() == 50, "union includes missing-metadata and metadata-only sources without duplicates");
    check(first["offset"] == 0, "negative offset is bounded");
    const auto last = reader.list("", 9999, 50);
    check(last["offset"] == 100 && last["sources"].size() == 29, "page is clamped after list shrinks");
    check(reader.list("", 0, 999)["sources"].size() == 100, "page size is bounded");
    check(reader.list("100%_", 0, 50)["total"] == 1, "search treats wildcard characters literally");
    check(reader.list("O'Brien", 0, 50)["total"] == 1, "search binds apostrophes safely");
    check(reader.list("FILE0.CPP", 0, 50)["total"] == 1, "path search is case insensitive");
    const auto source = reader.list("file0.cpp", 0, 50)["sources"][0];
    check(source["chunk_count"] == 2 && source["token_count"] == 10 && source["file_size"].is_null(), "aggregates are available without file metadata");
    const auto chunks = reader.chunks("src/file0.cpp", 0, 1);
    check(chunks["chunks"].size() == 1 && chunks["has_more"] == true, "chunks paginate with an extra-row probe");
    check(reader.chunks("src/file0.cpp", 1, 1)["has_more"] == false, "last chunk page is detected");
    check(reader.chunks("metadata-only.cpp", 0, 5)["chunks"].empty(), "metadata-only source has empty chunk detail");
    expectError("source_missing", [&] { reader.readFile("outside.cpp"); });
    expectError("not_local", [&] { reader.sourcePath("https://example.com/docs"); });
    expectError("file_missing", [&] { reader.readFile("src/file0.cpp"); });
    check(reader.sourcePath("src/file0.cpp") == fixture.directory / "src/file0.cpp", "relative sources use indexing working directory");
    Reader noBase(fixture.database, {});
    expectError("location_required", [&] { noBase.sourcePath("src/file0.cpp"); });

    const auto filePath = fixture.directory / "current.cpp";
    { std::ofstream file(filePath, std::ios::binary); file << "<script>alert(1)</script>\n"; }
    fixture.chunk("current.cpp");
    const auto file = reader.readFile("current.cpp");
    check(file["content"] == "<script>alert(1)</script>\n" && file["may_have_changed"].is_null(), "current source text is returned without markup interpretation");
    fixture.sql("INSERT INTO files_metadata(path,last_modified,file_size,nof_lines) VALUES('current.cpp',0,0,0)");
    check(reader.readFile("current.cpp")["may_have_changed"] == true, "changed file metadata is reported");
    fixture.sql("UPDATE files_metadata SET last_modified=" + std::to_string(utils::getFileModificationTime(filePath.string())) +
      ",file_size=" + std::to_string(fs::file_size(filePath)) + " WHERE path='current.cpp'");
    check(reader.readFile("current.cpp")["may_have_changed"] == false, "matching metadata is reported");
    { std::ofstream large(filePath, std::ios::binary); large << std::string(300000, 'a'); }
    const auto large = reader.readFile("current.cpp");
    check(large["truncated"] == true && large["content"].get<std::string>().size() == 256 * 1024, "file preview is bounded");
    { std::ofstream binary(filePath, std::ios::binary); binary.put('\0'); }
    expectError("binary_file", [&] { reader.readFile("current.cpp"); });
    fixture.chunk("large-chunk", std::string(20000, 'x'));
    const auto chunk = reader.chunks("large-chunk", 0, 5)["chunks"][0];
    check(chunk["truncated"] == true && chunk["content"].get<std::string>().size() == 16384, "indexed chunk preview is bounded");
    fixture.sql("BEGIN EXCLUSIVE");
    expectError("busy", [&] { reader.list("", 0, 50); });
    fixture.sql("ROLLBACK");
    check(reader.list("current.cpp", 0, 50)["total"] == 1, "same reader recovers after writer contention");
    const std::string unicodeSource = "src/\xCE\xB2.cpp";
    fs::create_directory(fixture.directory / "src");
    { std::ofstream unicodeFile(fixture.directory / fs::u8path(unicodeSource), std::ios::binary); unicodeFile << "Unicode path preview"; }
    fixture.chunk(unicodeSource);
    check(reader.list("\xCE\xB2", 0, 50)["total"] == 1, "Unicode source names can be searched");
    const auto unicodeFile = reader.readFile(unicodeSource);
    check(unicodeFile["content"] == "Unicode path preview" && unicodeFile["path"].get<std::string>().find(unicodeSource) != std::string::npos,
      "Unicode paths survive filesystem access and JSON serialization");
    const auto absoluteSource = database_browser::pathText(fixture.directory / fs::u8path(unicodeSource));
    fixture.chunk(absoluteSource);
    check(noBase.readFile(absoluteSource)["content"] == "Unicode path preview", "absolute sources work without a saved working directory");
    fixture.sql("PRAGMA journal_mode=WAL");
    fixture.sql("BEGIN IMMEDIATE");
    fixture.chunk("uncommitted.cpp");
    check(reader.list("uncommitted", 0, 50)["total"] == 0, "readers do not see uncommitted indexing writes");
    fixture.sql("COMMIT");
    check(reader.list("uncommitted", 0, 50)["total"] == 1, "refresh sees committed indexing writes");
    fixture.sql("ALTER TABLE files_metadata DROP COLUMN indexed_at");
    Reader older(fixture.database, fixture.directory);
    check(older.list("current.cpp", 0, 50)["sources"][0]["indexed_at"].is_null(), "older metadata without index timestamp is supported");
    fixture.sql("DROP TABLE chunks");
    expectError("unsupported_schema", [&] { Reader unsupported(fixture.database, fixture.directory); });
    std::cout << checks << " database browser checks passed.\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
