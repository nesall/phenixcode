#include "mcp_credentials.h"
#include "json_shim.h"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <system_error>
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
#include <bcrypt.h>
#include <sddl.h>
#include <shlobj.h>
#else
#include <fcntl.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace mcp::credentials {
  namespace {

    constexpr size_t kMaxProjectIdLength = 512;
    constexpr size_t kMaxCredentialFileBytes = 65536;
    constexpr int kRecordVersion = 1;

    std::string toHex(const uint8_t *data, size_t len) {
      static constexpr char kHex[] = "0123456789abcdef";
      std::string out;
      out.resize(len * 2);
      for (size_t i = 0; i < len; ++i) {
        out[i * 2] = kHex[(data[i] >> 4) & 0x0f];
        out[i * 2 + 1] = kHex[data[i] & 0x0f];
      }
      return out;
    }

    // Self-contained SHA-256 so the credential store has no OpenSSL link requirement.
    std::string sha256Hex(std::string_view input) {
      static constexpr std::array<uint32_t, 64> k = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
      };
      auto rotr = [](uint32_t x, uint32_t n) -> uint32_t { return (x >> n) | (x << (32 - n)); };

      std::array<uint32_t, 8> h = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
      };

      std::vector<uint8_t> msg(input.begin(), input.end());
      const uint64_t bitLen = static_cast<uint64_t>(msg.size()) * 8ULL;
      msg.push_back(0x80);
      while ((msg.size() % 64) != 56) msg.push_back(0x00);
      for (int i = 7; i >= 0; --i) msg.push_back(static_cast<uint8_t>((bitLen >> (i * 8)) & 0xff));

      for (size_t offset = 0; offset < msg.size(); offset += 64) {
        std::array<uint32_t, 64> w{};
        for (int i = 0; i < 16; ++i) {
          w[i] = (static_cast<uint32_t>(msg[offset + i * 4]) << 24) |
            (static_cast<uint32_t>(msg[offset + i * 4 + 1]) << 16) |
            (static_cast<uint32_t>(msg[offset + i * 4 + 2]) << 8) |
            static_cast<uint32_t>(msg[offset + i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
          const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
          const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
          w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
          const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
          const uint32_t ch = (e & f) ^ ((~e) & g);
          const uint32_t temp1 = hh + S1 + ch + k[i] + w[i];
          const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
          const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
          const uint32_t temp2 = S0 + maj;
          hh = g; g = f; f = e; e = d + temp1;
          d = c; c = b; b = a; a = temp1 + temp2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
      }

      std::array<uint8_t, 32> digest{};
      for (size_t i = 0; i < 8; ++i) {
        digest[i * 4] = static_cast<uint8_t>((h[i] >> 24) & 0xff);
        digest[i * 4 + 1] = static_cast<uint8_t>((h[i] >> 16) & 0xff);
        digest[i * 4 + 2] = static_cast<uint8_t>((h[i] >> 8) & 0xff);
        digest[i * 4 + 3] = static_cast<uint8_t>(h[i] & 0xff);
      }
      return toHex(digest.data(), digest.size());
    }

    void fillRandomBytes(uint8_t *buf, size_t len) {
      if (len == 0) return;
#ifdef _WIN32
      NTSTATUS status = BCryptGenRandom(nullptr, buf, static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
      if (status < 0) {
        throw StoreError(ErrorCode::CryptoError, "BCryptGenRandom failed to generate random bytes");
      }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
      arc4random_buf(buf, len);
#else
      int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
      if (fd < 0) {
        throw StoreError(ErrorCode::CryptoError, "Unable to open /dev/urandom for random credential generation");
      }
      struct stat st {};
      if (::fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode)) {
        ::close(fd);
        throw StoreError(ErrorCode::CryptoError, "/dev/urandom is not a character device");
      }
      size_t total = 0;
      while (total < len) {
        ssize_t n = ::read(fd, buf + total, len - total);
        if (n < 0) {
          if (errno == EINTR) continue;
          ::close(fd);
          throw StoreError(ErrorCode::CryptoError, "Failed reading random bytes from /dev/urandom");
        }
        if (n == 0) {
          ::close(fd);
          throw StoreError(ErrorCode::CryptoError, "Unexpected EOF reading /dev/urandom");
        }
        total += static_cast<size_t>(n);
      }
      ::close(fd);
#endif
    }

    std::string pathDisplay(const fs::path &p) {
      auto u8 = p.u8string();
      return std::string(reinterpret_cast<const char *>(u8.data()), u8.size());
    }

    std::string projectKeyStem(std::string_view projectId) {
      if (!isValidProjectId(projectId)) {
        throw StoreError(ErrorCode::InvalidProjectId,
          "Invalid project_id: must be non-empty, <= 512 bytes, and contain no control characters");
      }
      return sha256Hex(projectId);
    }

    std::shared_mutex &inProcessMutexFor(const fs::path &lockPath) {
      static std::mutex mapMutex;
      static std::map<std::string, std::unique_ptr<std::shared_mutex>> mutexes;
      std::lock_guard<std::mutex> guard(mapMutex);
      const std::string key = pathDisplay(lockPath.lexically_normal());
      auto &entry = mutexes[key];
      if (!entry) entry = std::make_unique<std::shared_mutex>();
      return *entry;
    }

    std::string serializeRecord(const CredentialRecord &rec) {
      json j = {
        { "version", kRecordVersion },
        { "project_id", rec.projectId },
        { "generation", rec.generation },
        { "token", rec.token }
      };
      return j.dump() + "\n";
    }

    CredentialRecord parseAndValidateRecord(const std::string &raw, std::string_view expectedProjectId, const fs::path &filePath) {
      const auto j = json::parse(raw, nullptr, false);
      if (j.is_discarded() || !j.is_object()) {
        throw StoreError(ErrorCode::CorruptCredential,
          "Stored MCP credential is corrupt (invalid JSON): " + pathDisplay(filePath));
      }
      if (!j.contains("version") || !j["version"].is_number_integer() || j["version"].get<int>() != kRecordVersion) {
        throw StoreError(ErrorCode::CorruptCredential,
          "Stored MCP credential has unsupported version: " + pathDisplay(filePath));
      }
      if (!j.contains("project_id") || !j["project_id"].is_string() ||
        j["project_id"].get<std::string>() != expectedProjectId) {
        throw StoreError(ErrorCode::CorruptCredential,
          "Stored MCP credential project_id mismatch: " + pathDisplay(filePath));
      }
      if (!j.contains("generation") || !j["generation"].is_number_unsigned() || j["generation"].get<uint64_t>() == 0) {
        throw StoreError(ErrorCode::CorruptCredential,
          "Stored MCP credential has invalid generation counter: " + pathDisplay(filePath));
      }
      if (!j.contains("token") || !j["token"].is_string()) {
        throw StoreError(ErrorCode::CorruptCredential,
          "Stored MCP credential is missing token: " + pathDisplay(filePath));
      }
      const auto token = j["token"].get<std::string>();
      if (!isValidToken(token)) {
        throw StoreError(ErrorCode::CorruptCredential,
          "Stored MCP credential contains an invalid token: " + pathDisplay(filePath));
      }
      CredentialRecord rec;
      rec.projectId = std::string(expectedProjectId);
      rec.generation = j["generation"].get<uint64_t>();
      rec.token = token;
      rec.created = false;
      return rec;
    }

#ifdef _WIN32

    std::string winErrorMessage(DWORD err) {
      LPSTR buf = nullptr;
      const DWORD len = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPSTR>(&buf), 0, nullptr);
      std::string msg = (len && buf) ? std::string(buf, len) : ("Windows error " + std::to_string(err));
      if (buf) LocalFree(buf);
      while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n' || msg.back() == ' ')) msg.pop_back();
      return msg;
    }

    std::vector<uint8_t> currentProcessUserSid() {
      HANDLE hToken = nullptr;
      if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        throw StoreError(ErrorCode::PermissionError, "OpenProcessToken failed: " + winErrorMessage(GetLastError()));
      }
      DWORD needed = 0;
      GetTokenInformation(hToken, TokenUser, nullptr, 0, &needed);
      if (needed == 0) {
        const DWORD err = GetLastError();
        CloseHandle(hToken);
        throw StoreError(ErrorCode::PermissionError, "GetTokenInformation size query failed: " + winErrorMessage(err));
      }
      std::vector<uint8_t> buf(needed);
      if (!GetTokenInformation(hToken, TokenUser, buf.data(), needed, &needed)) {
        const DWORD err = GetLastError();
        CloseHandle(hToken);
        throw StoreError(ErrorCode::PermissionError, "GetTokenInformation failed: " + winErrorMessage(err));
      }
      CloseHandle(hToken);
      auto *tu = reinterpret_cast<TOKEN_USER *>(buf.data());
      const DWORD sidLen = GetLengthSid(tu->User.Sid);
      std::vector<uint8_t> sid(sidLen);
      if (!CopySid(sidLen, sid.data(), tu->User.Sid)) {
        throw StoreError(ErrorCode::PermissionError, "CopySid failed: " + winErrorMessage(GetLastError()));
      }
      return sid;
    }

    struct UserOnlySecurity {
      std::vector<uint8_t> userSid;
      PACL pAcl = nullptr;
      SECURITY_DESCRIPTOR sd{};
      SECURITY_ATTRIBUTES sa{};

      explicit UserOnlySecurity(bool isDir) {
        userSid = currentProcessUserSid();
        EXPLICIT_ACCESSW ea{};
        ea.grfAccessPermissions = FILE_ALL_ACCESS;
        ea.grfAccessMode = SET_ACCESS;
        ea.grfInheritance = isDir ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
        ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        ea.Trustee.TrusteeType = TRUSTEE_IS_USER;
        ea.Trustee.ptstrName = reinterpret_cast<LPWSTR>(userSid.data());

        const DWORD aclRes = SetEntriesInAclW(1, &ea, nullptr, &pAcl);
        if (aclRes != ERROR_SUCCESS || !pAcl) {
          throw StoreError(ErrorCode::PermissionError, "SetEntriesInAclW failed: " + winErrorMessage(aclRes));
        }
        if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) ||
          !SetSecurityDescriptorOwner(&sd, userSid.data(), FALSE) ||
          !SetSecurityDescriptorDacl(&sd, TRUE, pAcl, FALSE) ||
          !SetSecurityDescriptorControl(&sd, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) {
          const DWORD err = GetLastError();
          LocalFree(pAcl);
          pAcl = nullptr;
          throw StoreError(ErrorCode::PermissionError, "Failed initializing user-only security descriptor: " + winErrorMessage(err));
        }
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = &sd;
        sa.bInheritHandle = FALSE;
      }

      ~UserOnlySecurity() {
        if (pAcl) LocalFree(pAcl);
      }

      UserOnlySecurity(const UserOnlySecurity &) = delete;
      UserOnlySecurity &operator=(const UserOnlySecurity &) = delete;
    };

    bool isWellKnownBroadSid(PSID sid) {
      return IsWellKnownSid(sid, WinWorldSid) ||
        IsWellKnownSid(sid, WinBuiltinUsersSid) ||
        IsWellKnownSid(sid, WinAuthenticatedUserSid) ||
        IsWellKnownSid(sid, WinAnonymousSid) ||
        IsWellKnownSid(sid, WinInteractiveSid) ||
        IsWellKnownSid(sid, WinNetworkSid);
    }

    void verifyWindowsAcl(const fs::path &path, bool isDir) {
      const DWORD attrs = GetFileAttributesW(path.c_str());
      if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
          throw StoreError(ErrorCode::NotFound, "Credential path not found: " + pathDisplay(path));
        }
        throw StoreError(ErrorCode::IoError, "Unable to inspect attributes for " + pathDisplay(path) + ": " + winErrorMessage(err));
      }
      if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        throw StoreError(ErrorCode::PermissionError, "Refusing symlink/reparse point in credential store: " + pathDisplay(path));
      }
      if (isDir && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        throw StoreError(ErrorCode::InvalidStoreDir, "Credential store path is not a directory: " + pathDisplay(path));
      }
      if (!isDir && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        throw StoreError(ErrorCode::CorruptCredential, "Credential path is a directory instead of a file: " + pathDisplay(path));
      }

      auto userSid = currentProcessUserSid();
      PSID pOwner = nullptr;
      PACL pDacl = nullptr;
      PSECURITY_DESCRIPTOR pSd = nullptr;
      const DWORD res = GetNamedSecurityInfoW(
        path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        &pOwner, nullptr, &pDacl, nullptr, &pSd);
      if (res != ERROR_SUCCESS) {
        throw StoreError(ErrorCode::PermissionError,
          "Unable to inspect ACL on " + pathDisplay(path) + ": " + winErrorMessage(res));
      }
      struct SdGuard { PSECURITY_DESCRIPTOR p; ~SdGuard() { if (p) LocalFree(p); } } guard{ pSd };

      if (!pOwner || !EqualSid(pOwner, userSid.data())) {
        throw StoreError(ErrorCode::PermissionError,
          "Credential path is not owned by the current OS user: " + pathDisplay(path));
      }
      if (!pDacl) {
        throw StoreError(ErrorCode::PermissionError,
          "Credential path has a null DACL (world accessible): " + pathDisplay(path));
      }
      ACL_SIZE_INFORMATION info{};
      if (!GetAclInformation(pDacl, &info, sizeof(info), AclSizeInformation)) {
        throw StoreError(ErrorCode::PermissionError, "GetAclInformation failed for " + pathDisplay(path));
      }
      for (DWORD i = 0; i < info.AceCount; ++i) {
        LPVOID pAce = nullptr;
        if (!GetAce(pDacl, i, &pAce)) continue;
        auto *header = static_cast<PACE_HEADER>(pAce);
        if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
          auto *allow = static_cast<ACCESS_ALLOWED_ACE *>(pAce);
          PSID aceSid = &allow->SidStart;
          if (EqualSid(aceSid, userSid.data())) continue;
          if (!isDir) {
            throw StoreError(ErrorCode::PermissionError,
              "Credential file has overly permissive ACL (non-owner ACE present): " + pathDisplay(path));
          }
          if (isWellKnownBroadSid(aceSid) ||
            (!IsWellKnownSid(aceSid, WinLocalSystemSid) && !IsWellKnownSid(aceSid, WinBuiltinAdministratorsSid))) {
            throw StoreError(ErrorCode::PermissionError,
              "Credential directory has overly permissive ACL: " + pathDisplay(path));
          }
        }
      }
    }

    void ensureDirectoryWindows(const fs::path &dir) {
      const DWORD attrs = GetFileAttributesW(dir.c_str());
      if (attrs != INVALID_FILE_ATTRIBUTES) {
        verifyWindowsAcl(dir, true);
        return;
      }
      const auto parent = dir.parent_path();
      if (!parent.empty() && parent != dir) {
        std::error_code ec;
        fs::create_directories(parent, ec);
        if (ec) {
          throw StoreError(ErrorCode::InvalidStoreDir,
            "Unable to create parent directory for credential store " + pathDisplay(dir) + ": " + ec.message());
        }
      }
      UserOnlySecurity sec(true);
      if (!CreateDirectoryW(dir.c_str(), &sec.sa)) {
        const DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
          throw StoreError(ErrorCode::InvalidStoreDir,
            "Unable to create credential directory " + pathDisplay(dir) + ": " + winErrorMessage(err));
        }
      }
      verifyWindowsAcl(dir, true);
    }

    bool checkDirectoryForReadWindows(const fs::path &dir) {
      const DWORD attrs = GetFileAttributesW(dir.c_str());
      if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) return false;
        throw StoreError(ErrorCode::PermissionError,
          "Unable to access credential directory " + pathDisplay(dir) + ": " + winErrorMessage(err));
      }
      verifyWindowsAcl(dir, true);
      return true;
    }

    class FileLock {
      HANDLE h_ = INVALID_HANDLE_VALUE;
    public:
      FileLock(const fs::path &lockPath, bool exclusive) {
        UserOnlySecurity sec(false);
        h_ = CreateFileW(
          lockPath.c_str(), GENERIC_READ | GENERIC_WRITE,
          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
          &sec.sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) {
          throw StoreError(ErrorCode::IoError,
            "Unable to open credential lock file " + pathDisplay(lockPath) + ": " + winErrorMessage(GetLastError()));
        }
        BY_HANDLE_FILE_INFORMATION fi{};
        if (GetFileInformationByHandle(h_, &fi) && (fi.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
          CloseHandle(h_);
          h_ = INVALID_HANDLE_VALUE;
          throw StoreError(ErrorCode::PermissionError, "Refusing reparse point lock file: " + pathDisplay(lockPath));
        }
        OVERLAPPED ov{};
        const DWORD flags = exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0;
        if (!LockFileEx(h_, flags, 0, MAXDWORD, MAXDWORD, &ov)) {
          const DWORD err = GetLastError();
          CloseHandle(h_);
          h_ = INVALID_HANDLE_VALUE;
          throw StoreError(ErrorCode::IoError,
            "Unable to lock credential store " + pathDisplay(lockPath) + ": " + winErrorMessage(err));
        }
      }
      ~FileLock() {
        if (h_ != INVALID_HANDLE_VALUE) {
          OVERLAPPED ov{};
          UnlockFileEx(h_, 0, MAXDWORD, MAXDWORD, &ov);
          CloseHandle(h_);
        }
      }
      FileLock(const FileLock &) = delete;
      FileLock &operator=(const FileLock &) = delete;
    };

    bool readFileCheckedWindows(const fs::path &filePath, std::string &out) {
      const DWORD attrs = GetFileAttributesW(filePath.c_str());
      if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) return false;
        throw StoreError(ErrorCode::PermissionError,
          "Unable to inspect credential file " + pathDisplay(filePath) + ": " + winErrorMessage(err));
      }
      verifyWindowsAcl(filePath, false);

      HANDLE h = CreateFileW(
        filePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND) return false;
        throw StoreError(ErrorCode::PermissionError,
          "Unable to read credential file " + pathDisplay(filePath) + ": " + winErrorMessage(err));
      }
      struct HandleGuard { HANDLE h; ~HandleGuard() { CloseHandle(h); } } hg{ h };

      LARGE_INTEGER size{};
      if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 || static_cast<uint64_t>(size.QuadPart) > kMaxCredentialFileBytes) {
        throw StoreError(ErrorCode::CorruptCredential, "Credential file size is invalid: " + pathDisplay(filePath));
      }
      out.resize(static_cast<size_t>(size.QuadPart));
      DWORD total = 0;
      while (total < static_cast<DWORD>(out.size())) {
        DWORD chunk = 0;
        if (!ReadFile(h, out.data() + total, static_cast<DWORD>(out.size()) - total, &chunk, nullptr)) {
          throw StoreError(ErrorCode::IoError,
            "Failed reading credential file " + pathDisplay(filePath) + ": " + winErrorMessage(GetLastError()));
        }
        if (chunk == 0) break;
        total += chunk;
      }
      out.resize(total);
      return true;
    }

    void writeFileAtomicWindows(const fs::path &targetPath, const std::string &content) {
      UserOnlySecurity sec(false);
      const auto pid = GetCurrentProcessId();
      fs::path tmpPath;
      HANDLE h = INVALID_HANDLE_VALUE;
      for (int attempt = 0; attempt < 8; ++attempt) {
        tmpPath = targetPath;
        tmpPath += ".tmp." + std::to_string(pid) + "." + generateSecureTokenHex(6);
        h = CreateFileW(
          tmpPath.c_str(), GENERIC_WRITE, 0, &sec.sa, CREATE_NEW,
          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (h != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS) {
          throw StoreError(ErrorCode::IoError,
            "Unable to create temporary credential file " + pathDisplay(tmpPath) + ": " + winErrorMessage(GetLastError()));
        }
      }
      if (h == INVALID_HANDLE_VALUE) {
        throw StoreError(ErrorCode::IoError, "Unable to allocate unique temporary credential file");
      }

      auto cleanupTmp = [&]() {
        if (h != INVALID_HANDLE_VALUE) {
          CloseHandle(h);
          h = INVALID_HANDLE_VALUE;
        }
        std::error_code ec;
        fs::remove(tmpPath, ec);
        };

      size_t written = 0;
      while (written < content.size()) {
        DWORD chunk = 0;
        if (!WriteFile(h, content.data() + written, static_cast<DWORD>(content.size() - written), &chunk, nullptr)) {
          const DWORD err = GetLastError();
          cleanupTmp();
          throw StoreError(ErrorCode::IoError,
            "Failed writing credential file " + pathDisplay(tmpPath) + ": " + winErrorMessage(err));
        }
        written += chunk;
      }
      if (!FlushFileBuffers(h)) {
        const DWORD err = GetLastError();
        cleanupTmp();
        throw StoreError(ErrorCode::IoError,
          "Failed flushing credential file " + pathDisplay(tmpPath) + ": " + winErrorMessage(err));
      }
      CloseHandle(h);
      h = INVALID_HANDLE_VALUE;

      // Enforce explicit user-only protected ACL before making the file visible at targetPath.
      const DWORD secRes = SetNamedSecurityInfoW(
        const_cast<LPWSTR>(tmpPath.c_str()), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        sec.userSid.data(), nullptr, sec.pAcl, nullptr);
      if (secRes != ERROR_SUCCESS) {
        cleanupTmp();
        throw StoreError(ErrorCode::PermissionError,
          "Unable to apply user-only ACL to credential file: " + winErrorMessage(secRes));
      }

      if (!MoveFileExW(tmpPath.c_str(), targetPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD err = GetLastError();
        cleanupTmp();
        throw StoreError(ErrorCode::IoError,
          "Unable to atomically persist credential file " + pathDisplay(targetPath) + ": " + winErrorMessage(err));
      }
    }

#else // POSIX

    void verifyUnixStat(const struct stat &st, const fs::path &path, bool isDir) {
      if (S_ISLNK(st.st_mode)) {
        throw StoreError(ErrorCode::PermissionError, "Refusing symlink in credential store: " + pathDisplay(path));
      }
      if (isDir && !S_ISDIR(st.st_mode)) {
        throw StoreError(ErrorCode::InvalidStoreDir, "Credential store path is not a directory: " + pathDisplay(path));
      }
      if (!isDir && !S_ISREG(st.st_mode)) {
        throw StoreError(ErrorCode::CorruptCredential, "Credential path is not a regular file: " + pathDisplay(path));
      }
      if (st.st_uid != ::geteuid()) {
        throw StoreError(ErrorCode::PermissionError,
          "Credential path is not owned by the current OS user: " + pathDisplay(path));
      }
      if ((st.st_mode & 0077) != 0) {
        throw StoreError(ErrorCode::PermissionError,
          std::string(isDir ? "Credential directory" : "Credential file") +
          " has overly permissive permissions (group/other bits set): " + pathDisplay(path));
      }
    }

    void ensureDirectoryUnix(const fs::path &dir) {
      struct stat st {};
      if (::lstat(dir.c_str(), &st) == 0) {
        verifyUnixStat(st, dir, true);
        return;
      }
      if (errno != ENOENT) {
        throw StoreError(ErrorCode::InvalidStoreDir,
          "Unable to inspect credential directory " + pathDisplay(dir) + ": " + std::strerror(errno));
      }
      const auto parent = dir.parent_path();
      if (!parent.empty() && parent != dir) {
        std::error_code ec;
        fs::create_directories(parent, ec);
        if (ec) {
          throw StoreError(ErrorCode::InvalidStoreDir,
            "Unable to create parent directory for credential store " + pathDisplay(dir) + ": " + ec.message());
        }
      }
      if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        throw StoreError(ErrorCode::InvalidStoreDir,
          "Unable to create credential directory " + pathDisplay(dir) + ": " + std::strerror(errno));
      }
      if (::chmod(dir.c_str(), 0700) != 0) {
        throw StoreError(ErrorCode::PermissionError,
          "Unable to set 0700 permissions on credential directory " + pathDisplay(dir) + ": " + std::strerror(errno));
      }
      if (::lstat(dir.c_str(), &st) != 0) {
        throw StoreError(ErrorCode::InvalidStoreDir,
          "Unable to verify credential directory " + pathDisplay(dir) + ": " + std::strerror(errno));
      }
      verifyUnixStat(st, dir, true);
    }

    bool checkDirectoryForReadUnix(const fs::path &dir) {
      struct stat st {};
      if (::lstat(dir.c_str(), &st) != 0) {
        if (errno == ENOENT) return false;
        throw StoreError(ErrorCode::PermissionError,
          "Unable to access credential directory " + pathDisplay(dir) + ": " + std::strerror(errno));
      }
      verifyUnixStat(st, dir, true);
      return true;
    }

    class FileLock {
      int fd_ = -1;
    public:
      FileLock(const fs::path &lockPath, bool exclusive) {
        fd_ = ::open(lockPath.c_str(), O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd_ < 0) {
          throw StoreError(ErrorCode::IoError,
            "Unable to open credential lock file " + pathDisplay(lockPath) + ": " + std::strerror(errno));
        }
        struct stat st {};
        if (::fstat(fd_, &st) != 0 || !S_ISREG(st.st_mode)) {
          ::close(fd_);
          fd_ = -1;
          throw StoreError(ErrorCode::PermissionError, "Invalid credential lock file: " + pathDisplay(lockPath));
        }
        const int op = exclusive ? LOCK_EX : LOCK_SH;
        while (::flock(fd_, op) != 0) {
          if (errno == EINTR) continue;
          const int err = errno;
          ::close(fd_);
          fd_ = -1;
          throw StoreError(ErrorCode::IoError,
            "Unable to lock credential store " + pathDisplay(lockPath) + ": " + std::strerror(err));
        }
      }
      ~FileLock() {
        if (fd_ >= 0) {
          ::flock(fd_, LOCK_UN);
          ::close(fd_);
        }
      }
      FileLock(const FileLock &) = delete;
      FileLock &operator=(const FileLock &) = delete;
    };

    bool readFileCheckedUnix(const fs::path &filePath, std::string &out) {
      struct stat lst {};
      if (::lstat(filePath.c_str(), &lst) != 0) {
        if (errno == ENOENT) return false;
        throw StoreError(ErrorCode::PermissionError,
          "Unable to inspect credential file " + pathDisplay(filePath) + ": " + std::strerror(errno));
      }
      verifyUnixStat(lst, filePath, false);

      const int fd = ::open(filePath.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
      if (fd < 0) {
        if (errno == ENOENT) return false;
        throw StoreError(ErrorCode::PermissionError,
          "Unable to open credential file " + pathDisplay(filePath) + ": " + std::strerror(errno));
      }
      struct FdGuard { int fd; ~FdGuard() { if (fd >= 0) ::close(fd); } } fg{ fd };

      struct stat fst {};
      if (::fstat(fd, &fst) != 0) {
        throw StoreError(ErrorCode::IoError,
          "Unable to fstat credential file " + pathDisplay(filePath) + ": " + std::strerror(errno));
      }
      verifyUnixStat(fst, filePath, false);
      if (fst.st_size < 0 || static_cast<uint64_t>(fst.st_size) > kMaxCredentialFileBytes) {
        throw StoreError(ErrorCode::CorruptCredential, "Credential file size is invalid: " + pathDisplay(filePath));
      }

      out.clear();
      std::array<char, 4096> buf{};
      while (true) {
        const ssize_t n = ::read(fd, buf.data(), buf.size());
        if (n < 0) {
          if (errno == EINTR) continue;
          throw StoreError(ErrorCode::IoError,
            "Failed reading credential file " + pathDisplay(filePath) + ": " + std::strerror(errno));
        }
        if (n == 0) break;
        if (out.size() + static_cast<size_t>(n) > kMaxCredentialFileBytes) {
          throw StoreError(ErrorCode::CorruptCredential, "Credential file exceeds maximum size: " + pathDisplay(filePath));
        }
        out.append(buf.data(), static_cast<size_t>(n));
      }
      return true;
    }

    void writeFileAtomicUnix(const fs::path &targetPath, const std::string &content) {
      const auto pid = ::getpid();
      fs::path tmpPath;
      int fd = -1;
      for (int attempt = 0; attempt < 8; ++attempt) {
        tmpPath = targetPath;
        tmpPath += ".tmp." + std::to_string(pid) + "." + generateSecureTokenHex(6);
        fd = ::open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd >= 0) break;
        if (errno != EEXIST) {
          throw StoreError(ErrorCode::IoError,
            "Unable to create temporary credential file " + pathDisplay(tmpPath) + ": " + std::strerror(errno));
        }
      }
      if (fd < 0) {
        throw StoreError(ErrorCode::IoError, "Unable to allocate unique temporary credential file");
      }

      auto cleanupTmp = [&]() {
        if (fd >= 0) {
          ::close(fd);
          fd = -1;
        }
        ::unlink(tmpPath.c_str());
        };

      if (::fchmod(fd, 0600) != 0) {
        const int err = errno;
        cleanupTmp();
        throw StoreError(ErrorCode::PermissionError,
          "Unable to enforce 0600 permissions on temporary credential file: " + std::string(std::strerror(err)));
      }

      size_t written = 0;
      while (written < content.size()) {
        const ssize_t n = ::write(fd, content.data() + written, content.size() - written);
        if (n < 0) {
          if (errno == EINTR) continue;
          const int err = errno;
          cleanupTmp();
          throw StoreError(ErrorCode::IoError,
            "Failed writing credential file " + pathDisplay(tmpPath) + ": " + std::strerror(err));
        }
        written += static_cast<size_t>(n);
      }
      if (::fsync(fd) != 0) {
        const int err = errno;
        cleanupTmp();
        throw StoreError(ErrorCode::IoError,
          "Failed flushing credential file " + pathDisplay(tmpPath) + ": " + std::strerror(err));
      }
      ::close(fd);
      fd = -1;

      if (::rename(tmpPath.c_str(), targetPath.c_str()) != 0) {
        const int err = errno;
        cleanupTmp();
        throw StoreError(ErrorCode::IoError,
          "Unable to atomically persist credential file " + pathDisplay(targetPath) + ": " + std::strerror(err));
      }

      const int dirFd = ::open(targetPath.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      if (dirFd >= 0) {
        ::fsync(dirFd);
        ::close(dirFd);
      }
    }

#endif // _WIN32

    void ensureDirectory(const fs::path &dir) {
#ifdef _WIN32
      ensureDirectoryWindows(dir);
#else
      ensureDirectoryUnix(dir);
#endif
    }

    bool checkDirectoryForRead(const fs::path &dir) {
#ifdef _WIN32
      return checkDirectoryForReadWindows(dir);
#else
      return checkDirectoryForReadUnix(dir);
#endif
    }

    bool readFileChecked(const fs::path &filePath, std::string &out) {
#ifdef _WIN32
      return readFileCheckedWindows(filePath, out);
#else
      return readFileCheckedUnix(filePath, out);
#endif
    }

    void writeFileAtomic(const fs::path &targetPath, const std::string &content) {
#ifdef _WIN32
      writeFileAtomicWindows(targetPath, content);
#else
      writeFileAtomicUnix(targetPath, content);
#endif
    }

    fs::path lockPathFor(const fs::path &dir, std::string_view keyStem) {
      return dir / (std::string(keyStem) + ".lock");
    }

    fs::path credPathFor(const fs::path &dir, std::string_view keyStem) {
      return dir / (std::string(keyStem) + ".json");
    }

  } // namespace

  bool isValidProjectId(std::string_view projectId) {
    if (projectId.empty() || projectId.size() > kMaxProjectIdLength) return false;
    if (projectId == "." || projectId == "..") return false;
    bool hasNonSpace = false;
    for (unsigned char c : projectId) {
      if (c < 0x20 || c == 0x7f) return false;
      if (c != ' ' && c != '\t') hasNonSpace = true;
    }
    return hasNonSpace;
  }

  bool isValidToken(std::string_view token) {
    if (token.size() < kMinTokenLength || token.size() > kMaxTokenLength) return false;
    for (unsigned char c : token) {
      if (c < 0x21 || c > 0x7e) return false;
    }
    return true;
  }

  bool constantTimeEqual(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
      diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
  }

  fs::path defaultCredentialsDirectory() {
    if (const char *overrideDir = std::getenv(kDirEnvVar)) {
      if (*overrideDir == '\0') {
        throw StoreError(ErrorCode::InvalidStoreDir,
          "PHENIXCODE_MCP_CREDENTIALS_DIR is set but empty.");
      }
      return fs::u8path(overrideDir);
    }
#ifdef _WIN32
    if (const char *localAppData = std::getenv("LOCALAPPDATA")) {
      if (*localAppData != '\0') {
        return fs::u8path(localAppData) / "PhenixCode" / "mcp-credentials";
      }
    }
    PWSTR known = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &known)) && known) {
      fs::path dir = fs::path(known) / L"PhenixCode" / L"mcp-credentials";
      CoTaskMemFree(known);
      return dir;
    }
    if (known) CoTaskMemFree(known);
    if (const char *userProfile = std::getenv("USERPROFILE")) {
      if (*userProfile != '\0') {
        return fs::u8path(userProfile) / ".phenixcode" / "mcp-credentials";
      }
    }
    throw StoreError(ErrorCode::InvalidStoreDir,
      "Unable to resolve OS user profile directory (LOCALAPPDATA / USERPROFILE) for MCP credentials.");
#elif defined(__APPLE__)
    const char *home = std::getenv("HOME");
    if (!home || *home == '\0') {
      if (struct passwd *pw = ::getpwuid(::getuid())) home = pw->pw_dir;
    }
    if (!home || *home == '\0') {
      throw StoreError(ErrorCode::InvalidStoreDir,
        "Unable to resolve HOME directory for MCP credentials.");
    }
    return fs::u8path(home) / "Library" / "Application Support" / "PhenixCode" / "mcp-credentials";
#else
    if (const char *xdgConfig = std::getenv("XDG_CONFIG_HOME")) {
      if (*xdgConfig != '\0') {
        return fs::u8path(xdgConfig) / "phenixcode" / "mcp-credentials";
      }
    }
    const char *home = std::getenv("HOME");
    if (!home || *home == '\0') {
      if (struct passwd *pw = ::getpwuid(::getuid())) home = pw->pw_dir;
    }
    if (!home || *home == '\0') {
      throw StoreError(ErrorCode::InvalidStoreDir,
        "Unable to resolve HOME directory for MCP credentials.");
    }
    return fs::u8path(home) / ".config" / "phenixcode" / "mcp-credentials";
#endif
  }

  std::string generateSecureTokenHex(size_t randomBytes) {
    if (randomBytes == 0) {
      throw StoreError(ErrorCode::CryptoError, "randomBytes must be greater than zero");
    }
    std::vector<uint8_t> buf(randomBytes);
    fillRandomBytes(buf.data(), buf.size());
    return toHex(buf.data(), buf.size());
  }

  Store::Store(fs::path directory)
    : dir_(directory.empty() ? defaultCredentialsDirectory() : std::move(directory))
  {
    if (dir_.empty()) {
      throw StoreError(ErrorCode::InvalidStoreDir, "Credential store directory path must not be empty");
    }
  }

  fs::path Store::credentialFilePath(std::string_view projectId) const
  {
    return credPathFor(dir_, projectKeyStem(projectId));
  }

  CredentialStatus Store::status(std::string_view projectId) const
  {
    const std::string stem = projectKeyStem(projectId);
    CredentialStatus st;
    st.projectId = std::string(projectId);
    if (!checkDirectoryForRead(dir_)) {
      return st;
    }
    const fs::path target = credPathFor(dir_, stem);
    const fs::path lockPath = lockPathFor(dir_, stem);
    std::shared_lock<std::shared_mutex> memLock(inProcessMutexFor(lockPath));
    // Avoid creating a lock file if neither the credential file nor lock file exists yet.
    std::error_code ec;
    if (!fs::exists(target, ec)) {
      if (ec) {
        throw StoreError(ErrorCode::PermissionError,
          "Unable to inspect credential file " + pathDisplay(target) + ": " + ec.message());
      }
      return st;
    }
    FileLock fileLock(lockPath, false);
    std::string raw;
    if (!readFileChecked(target, raw)) return st;
    const auto rec = parseAndValidateRecord(raw, projectId, target);
    st.hasCredential = true;
    st.generation = rec.generation;
    return st;
  }

  CredentialRecord Store::get(std::string_view projectId) const
  {
    const std::string stem = projectKeyStem(projectId);
    if (!checkDirectoryForRead(dir_)) {
      throw StoreError(ErrorCode::NotFound,
        "No stored MCP credential exists for project '" + std::string(projectId) + "'.");
    }
    const fs::path target = credPathFor(dir_, stem);
    const fs::path lockPath = lockPathFor(dir_, stem);
    std::shared_lock<std::shared_mutex> memLock(inProcessMutexFor(lockPath));
    std::error_code ec;
    if (!fs::exists(target, ec)) {
      if (ec) {
        throw StoreError(ErrorCode::PermissionError,
          "Unable to inspect credential file " + pathDisplay(target) + ": " + ec.message());
      }
      throw StoreError(ErrorCode::NotFound,
        "No stored MCP credential exists for project '" + std::string(projectId) + "'.");
    }
    FileLock fileLock(lockPath, false);
    std::string raw;
    if (!readFileChecked(target, raw)) {
      throw StoreError(ErrorCode::NotFound,
        "No stored MCP credential exists for project '" + std::string(projectId) + "'.");
    }
    return parseAndValidateRecord(raw, projectId, target);
  }

  CredentialRecord Store::ensure(std::string_view projectId)
  {
    const std::string stem = projectKeyStem(projectId);
    ensureDirectory(dir_);
    const fs::path target = credPathFor(dir_, stem);
    const fs::path lockPath = lockPathFor(dir_, stem);
    std::unique_lock<std::shared_mutex> memLock(inProcessMutexFor(lockPath));
    FileLock fileLock(lockPath, true);

    std::string raw;
    if (readFileChecked(target, raw)) {
      return parseAndValidateRecord(raw, projectId, target);
    }

    CredentialRecord rec;
    rec.projectId = std::string(projectId);
    rec.generation = 1;
    rec.token = generateSecureTokenHex(kGeneratedRandomBytes);
    rec.created = true;
    writeFileAtomic(target, serializeRecord(rec));
    return rec;
  }

  CredentialRecord Store::regenerate(std::string_view projectId)
  {
    const std::string stem = projectKeyStem(projectId);
    ensureDirectory(dir_);
    const fs::path target = credPathFor(dir_, stem);
    const fs::path lockPath = lockPathFor(dir_, stem);
    std::unique_lock<std::shared_mutex> memLock(inProcessMutexFor(lockPath));
    FileLock fileLock(lockPath, true);

    uint64_t nextGeneration = 1;
    std::string raw;
    if (readFileChecked(target, raw)) {
      // Refuse to silently overwrite a corrupt or tampered credential file.
      const auto existing = parseAndValidateRecord(raw, projectId, target);
      nextGeneration = existing.generation + 1;
    }

    CredentialRecord rec;
    rec.projectId = std::string(projectId);
    rec.generation = nextGeneration;
    rec.token = generateSecureTokenHex(kGeneratedRandomBytes);
    rec.created = true;
    writeFileAtomic(target, serializeRecord(rec));
    return rec;
  }

  VerifyResult Store::verify(std::string_view projectId, std::string_view presentedToken) const
  {
    VerifyResult res;
    try {
      const std::string stem = projectKeyStem(projectId);
      if (!checkDirectoryForRead(dir_)) {
        res.status = VerifyStatus::Missing;
        res.error = "MCP credentials directory does not exist";
        return res;
      }
      const fs::path target = credPathFor(dir_, stem);
      const fs::path lockPath = lockPathFor(dir_, stem);
      std::shared_lock<std::shared_mutex> memLock(inProcessMutexFor(lockPath));
      std::error_code ec;
      if (!fs::exists(target, ec)) {
        if (ec) {
          res.status = VerifyStatus::Error;
          res.error = "Unable to inspect credential file: " + ec.message();
          return res;
        }
        res.status = VerifyStatus::Missing;
        res.error = "No stored MCP credential for project";
        return res;
      }
      FileLock fileLock(lockPath, false);
      std::string raw;
      if (!readFileChecked(target, raw)) {
        res.status = VerifyStatus::Missing;
        res.error = "No stored MCP credential for project";
        return res;
      }
      const auto rec = parseAndValidateRecord(raw, projectId, target);
      res.status = constantTimeEqual(presentedToken, rec.token) ? VerifyStatus::Match : VerifyStatus::Mismatch;
      return res;
    } catch (const std::exception &e) {
      res.status = VerifyStatus::Error;
      res.error = e.what();
      return res;
    }
  }

} // namespace mcp::credentials
