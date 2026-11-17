#include "mcp_credentials.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#else
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
using namespace mcp::credentials;

namespace {

struct TempDir {
  fs::path path;
  explicit TempDir(const std::string &prefix) {
    path = fs::temp_directory_path() / (prefix + "-" + generateSecureTokenHex(8));
    std::error_code ec;
    fs::remove_all(path, ec);
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

void setEnvVar(const char *name, const char *value) {
#ifdef _WIN32
  if (value) _putenv_s(name, value);
  else _putenv_s(name, "");
#else
  if (value) ::setenv(name, value, 1);
  else ::unsetenv(name);
#endif
}

std::string readWholeFile(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int gChecks = 0;
void check(bool cond, const std::string &msg) {
  ++gChecks;
  if (!cond) throw std::runtime_error("Check " + std::to_string(gChecks) + " failed: " + msg);
}

template <typename Fn>
ErrorCode expectStoreError(Fn &&fn, const std::string &msg) {
  ++gChecks;
  try {
    fn();
  } catch (const StoreError &e) {
    return e.code();
  }
  throw std::runtime_error("Check " + std::to_string(gChecks) + " expected StoreError: " + msg);
}

void testValidationAndGeneration() {
  check(!isValidProjectId(""), "empty project_id rejected");
  check(!isValidProjectId("   \t  "), "whitespace-only project_id rejected");
  check(!isValidProjectId("."), "dot project_id rejected");
  check(!isValidProjectId(".."), "dotdot project_id rejected");
  check(!isValidProjectId(std::string("bad\nid")), "newline in project_id rejected");
  check(!isValidProjectId(std::string(513, 'x')), "overlong project_id rejected");
  check(isValidProjectId("alpha-316e366b"), "standard project_id accepted");
  check(isValidProjectId("My Project-1234abcd"), "project_id with interior spaces accepted");

  const std::string t1 = generateSecureTokenHex();
  const std::string t2 = generateSecureTokenHex();
  check(t1.size() == 64 && t2.size() == 64, "32 random bytes hex-encoded to 64 chars");
  check(t1 != t2, "generated tokens are distinct");
  check(isValidToken(t1) && isValidToken(t2), "generated tokens pass validation");
  check(!isValidToken(std::string(31, 'a')), "short token rejected");
  check(!isValidToken(std::string(32, 'a') + " "), "whitespace in token rejected");
  check(!isValidToken(std::string(32, 'a') + "\n"), "control char in token rejected");
}

void testPersistenceAndIsolation() {
  TempDir root("phenixcode-cred-persist");
  const fs::path storeDir = root.path / "creds";

  Store store1(storeDir);
  check(!fs::exists(storeDir), "Store construction does not eagerly create directory");

  const auto stMissing = store1.status("proj-a");
  check(!stMissing.hasCredential && stMissing.generation == 0, "status reports missing without creating");
  check(!fs::exists(storeDir), "status does not create directory or credential");
  check(expectStoreError([&] { (void)store1.get("proj-a"); }, "get missing") == ErrorCode::NotFound,
        "get on missing credential throws NotFound");

  const auto createdA = store1.ensure("proj-a");
  check(createdA.created, "first ensure marks created=true");
  check(createdA.generation == 1, "initial generation is 1");
  check(isValidToken(createdA.token), "created token is valid");
  check(fs::exists(storeDir), "ensure creates directory");

#ifndef _WIN32
  struct stat dirSt{};
  check(::lstat(storeDir.c_str(), &dirSt) == 0 && (dirSt.st_mode & 0777) == 0700, "Unix dir mode is 0700");
  struct stat fileSt{};
  const fs::path fileA = store1.credentialFilePath("proj-a");
  check(::lstat(fileA.c_str(), &fileSt) == 0 && (fileSt.st_mode & 0777) == 0600, "Unix file mode is 0600");
#endif

  // Reuse across Store instances.
  Store store2(storeDir);
  const auto stA = store2.status("proj-a");
  check(stA.hasCredential && stA.generation == 1, "status sees existing credential");
  const auto reusedA = store2.ensure("proj-a");
  check(!reusedA.created && reusedA.token == createdA.token && reusedA.generation == 1,
        "ensure reuses persisted credential");
  check(store2.get("proj-a").token == createdA.token, "get matches persisted credential");

  // Project isolation.
  const auto createdB = store2.ensure("proj-b");
  check(createdB.token != createdA.token, "distinct projects get distinct tokens");
  check(store2.verify("proj-a", createdA.token).status == VerifyStatus::Match, "verify accepts proj-a token");
  check(store2.verify("proj-a", createdB.token).status == VerifyStatus::Mismatch, "proj-b token rejected for proj-a");
  check(store2.verify("proj-c", createdA.token).status == VerifyStatus::Missing, "missing project returns Missing");
}

void testConcurrentCreation() {
  TempDir root("phenixcode-cred-conc");
  const fs::path storeDir = root.path / "creds";

  constexpr int kThreads = 8;
  std::vector<std::string> tokens(kThreads);
  std::vector<uint64_t> gens(kThreads);
  std::atomic<int> ready{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i] {
      Store s(storeDir);
      ready.fetch_add(1);
      while (ready.load() < kThreads) std::this_thread::yield();
      const auto rec = s.ensure("concurrent-proj");
      tokens[i] = rec.token;
      gens[i] = rec.generation;
    });
  }
  for (auto &t : threads) t.join();
  for (int i = 0; i < kThreads; ++i) {
    check(tokens[i] == tokens[0] && gens[i] == 1, "concurrent ensure produces identical token and generation 1");
  }
}

void testRotationAndConcurrentVerification() {
  TempDir root("phenixcode-cred-rotate");
  const fs::path storeDir = root.path / "creds";
  Store store(storeDir);

  const auto first = store.ensure("rotate-proj");
  const auto second = store.regenerate("rotate-proj");
  check(second.token != first.token, "regenerate changes token");
  check(second.generation == 2, "regenerate increments generation");
  check(store.verify("rotate-proj", first.token).status == VerifyStatus::Mismatch, "old token rejected after rotation");
  check(store.verify("rotate-proj", second.token).status == VerifyStatus::Match, "new token accepted after rotation");

  // Unrelated project remains intact.
  const auto other = store.ensure("other-proj");
  (void)store.regenerate("rotate-proj");
  check(store.get("other-proj").token == other.token, "rotating one project leaves others untouched");

  // Concurrent rotation + verification stress test must never report VerifyStatus::Error.
  std::atomic<bool> stop{false};
  std::atomic<int> errors{0};
  std::thread reader([&] {
    Store rs(storeDir);
    while (!stop.load()) {
      const auto r = rs.verify("rotate-proj", first.token);
      if (r.status != VerifyStatus::Mismatch) ++errors;
    }
  });
  for (int i = 0; i < 20; ++i) {
    const auto rotated = store.regenerate("rotate-proj");
    check(store.verify("rotate-proj", rotated.token).status == VerifyStatus::Match, "latest rotated token verifies");
  }
  stop.store(true);
  reader.join();
  check(errors.load() == 0, "no errors during concurrent rotation and verification");
}

void testCorruptionAndPermissions() {
  TempDir root("phenixcode-cred-corrupt");
  const fs::path storeDir = root.path / "creds";
  Store store(storeDir);
  (void)store.ensure("corrupt-proj");
  const fs::path credFile = store.credentialFilePath("corrupt-proj");

  // 1. Corrupt JSON must fail without being silently replaced.
  const std::string corruptBytes = "{not-valid-json";
  {
    std::ofstream out(credFile, std::ios::binary | std::ios::trunc);
    out << corruptBytes;
  }
  check(expectStoreError([&] { (void)store.status("corrupt-proj"); }, "corrupt status") == ErrorCode::CorruptCredential,
        "status detects corrupt JSON");
  check(expectStoreError([&] { (void)store.ensure("corrupt-proj"); }, "corrupt ensure") == ErrorCode::CorruptCredential,
        "ensure refuses to overwrite corrupt JSON");
  check(expectStoreError([&] { (void)store.regenerate("corrupt-proj"); }, "corrupt regen") == ErrorCode::CorruptCredential,
        "regenerate refuses to overwrite corrupt JSON");
  check(readWholeFile(credFile) == corruptBytes, "corrupt file preserved untouched");
  check(store.verify("corrupt-proj", std::string(64, 'a')).status == VerifyStatus::Error,
        "verify fails closed on corrupt credential");

  // 2. Project ID mismatch inside JSON.
  {
    std::ofstream out(credFile, std::ios::binary | std::ios::trunc);
    out << R"({"version":1,"project_id":"wrong-proj","generation":1,"token":")" << std::string(64, 'b') << R"("})";
  }
  check(expectStoreError([&] { (void)store.get("corrupt-proj"); }, "mismatched project_id") == ErrorCode::CorruptCredential,
        "mismatched project_id rejected");

  // 3. Overly permissive permissions rejected.
  fs::remove(credFile);
  (void)store.ensure("perm-proj");
  const fs::path permFile = store.credentialFilePath("perm-proj");
#ifdef _WIN32
  // Recreate the credential file with an Everyone (S-1-1-0) ACE at creation time.
  const std::string permData = readWholeFile(permFile);
  fs::remove(permFile);
  PSID worldSid = nullptr;
  check(ConvertStringSidToSidW(L"S-1-1-0", &worldSid) != FALSE, "SID for Everyone");
  HANDLE hToken = nullptr;
  check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken) != FALSE, "OpenProcessToken");
  DWORD tokLen = 0;
  GetTokenInformation(hToken, TokenUser, nullptr, 0, &tokLen);
  std::vector<uint8_t> tokBuf(tokLen);
  check(GetTokenInformation(hToken, TokenUser, tokBuf.data(), tokLen, &tokLen) != FALSE, "GetTokenInformation");
  CloseHandle(hToken);
  PSID ownerSid = reinterpret_cast<TOKEN_USER *>(tokBuf.data())->User.Sid;

  EXPLICIT_ACCESSW eas[2]{};
  eas[0].grfAccessPermissions = FILE_ALL_ACCESS;
  eas[0].grfAccessMode = SET_ACCESS;
  eas[0].grfInheritance = NO_INHERITANCE;
  eas[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
  eas[0].Trustee.TrusteeType = TRUSTEE_IS_USER;
  eas[0].Trustee.ptstrName = static_cast<LPWSTR>(ownerSid);
  eas[1].grfAccessPermissions = GENERIC_READ;
  eas[1].grfAccessMode = SET_ACCESS;
  eas[1].grfInheritance = NO_INHERITANCE;
  eas[1].Trustee.TrusteeForm = TRUSTEE_IS_SID;
  eas[1].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
  eas[1].Trustee.ptstrName = static_cast<LPWSTR>(worldSid);

  PACL newDacl = nullptr;
  check(SetEntriesInAclW(2, eas, nullptr, &newDacl) == ERROR_SUCCESS, "SetEntriesInAclW creates permissive ACL");
  SECURITY_DESCRIPTOR sd{};
  check(InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) &&
        SetSecurityDescriptorDacl(&sd, TRUE, newDacl, FALSE) &&
        SetSecurityDescriptorControl(&sd, SE_DACL_PROTECTED, SE_DACL_PROTECTED),
        "Initialize permissive SECURITY_DESCRIPTOR");
  SECURITY_ATTRIBUTES sa{sizeof(sa), &sd, FALSE};
  HANDLE hPerm = CreateFileW(permFile.c_str(), GENERIC_WRITE, 0, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  check(hPerm != INVALID_HANDLE_VALUE, "CreateFileW creates permissive file");
  DWORD written = 0;
  check(WriteFile(hPerm, permData.data(), static_cast<DWORD>(permData.size()), &written, nullptr) != FALSE, "WriteFile permissive file");
  CloseHandle(hPerm);
  LocalFree(newDacl);
  LocalFree(worldSid);
  check(expectStoreError([&] { (void)store.get("perm-proj"); }, "permissive ACL") == ErrorCode::PermissionError,
        "overly permissive Windows ACL rejected");
#else
  check(::chmod(permFile.c_str(), 0644) == 0, "chmod 0644 succeeds");
  check(expectStoreError([&] { (void)store.get("perm-proj"); }, "permissive mode") == ErrorCode::PermissionError,
        "overly permissive Unix mode rejected");
  ::chmod(permFile.c_str(), 0600);
#endif

  // 4. Storage failure when store path is a regular file.
  const fs::path badDir = root.path / "not-a-dir";
  {
    std::ofstream out(badDir);
    out << "x";
  }
  Store badStore(badDir);
  check(expectStoreError([&] { (void)badStore.ensure("x"); }, "file as store dir") == ErrorCode::InvalidStoreDir,
        "regular file store dir rejected");

  // 5. PHENIXCODE_MCP_CREDENTIALS_DIR override.
  const fs::path envDir = root.path / "env-creds";
  const std::string envDirStr = envDir.string();
  setEnvVar(kDirEnvVar, envDirStr.c_str());
  check(defaultCredentialsDirectory() == envDir, "PHENIXCODE_MCP_CREDENTIALS_DIR overrides default dir");
#ifndef _WIN32
  setEnvVar(kDirEnvVar, "");
  check(expectStoreError([] { (void)defaultCredentialsDirectory(); }, "empty dir env") == ErrorCode::InvalidStoreDir,
        "empty PHENIXCODE_MCP_CREDENTIALS_DIR rejected");
#endif
  setEnvVar(kDirEnvVar, nullptr);
  check(defaultCredentialsDirectory() != envDir, "unsetting PHENIXCODE_MCP_CREDENTIALS_DIR restores user profile dir");
}

} // namespace

int main() {
  try {
    testValidationAndGeneration();
    testPersistenceAndIsolation();
    testConcurrentCreation();
    testRotationAndConcurrentVerification();
    testCorruptionAndPermissions();
    std::cout << gChecks << " MCP credential checks PASSED\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "FAILED: " << e.what() << '\n';
    return 1;
  }
}
