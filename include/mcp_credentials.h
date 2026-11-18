#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mcp::credentials {

inline constexpr size_t kMinTokenLength = 32;
inline constexpr size_t kMaxTokenLength = 4096;
inline constexpr size_t kGeneratedRandomBytes = 32;
inline constexpr const char *kDirEnvVar = "PHENIXCODE_MCP_CREDENTIALS_DIR";

enum class ErrorCode {
  NoError,
  InvalidProjectId,
  InvalidStoreDir,
  PermissionError,
  CorruptCredential,
  NotFound,
  IoError,
  CryptoError
};

class StoreError : public std::runtime_error {
  ErrorCode code_;
public:
  StoreError(ErrorCode code, const std::string &message)
    : std::runtime_error(message), code_(code) {}
  ErrorCode code() const noexcept { return code_; }
};

struct CredentialStatus {
  bool hasCredential = false;
  uint64_t generation = 0;
  std::string projectId;
};

struct CredentialRecord {
  std::string projectId;
  std::string token;
  uint64_t generation = 0;
  bool created = false;
};

enum class VerifyStatus {
  Match,
  Mismatch,
  Missing,
  Error
};

struct VerifyResult {
  VerifyStatus status = VerifyStatus::Error;
  std::string error;
};

[[nodiscard]] bool isValidProjectId(std::string_view projectId);
[[nodiscard]] bool isValidToken(std::string_view token);
[[nodiscard]] bool constantTimeEqual(std::string_view a, std::string_view b);
[[nodiscard]] std::filesystem::path defaultCredentialsDirectory();
[[nodiscard]] std::string generateSecureTokenHex(size_t randomBytes = kGeneratedRandomBytes);

// Persistent per-OS-user, per-project MCP credential store.
// Platform ACL/permission enforcement and cross-process locking live behind this abstraction.
class Store {
public:
  explicit Store(std::filesystem::path directory = {});

  const std::filesystem::path &directory() const noexcept { return dir_; }
  std::filesystem::path credentialFilePath(std::string_view projectId) const;

  CredentialStatus status(std::string_view projectId) const;
  CredentialRecord get(std::string_view projectId) const;
  CredentialRecord ensure(std::string_view projectId);
  CredentialRecord regenerate(std::string_view projectId);
  VerifyResult verify(std::string_view projectId, std::string_view presentedToken) const;

private:
  std::filesystem::path dir_;
};

} // namespace mcp::credentials
