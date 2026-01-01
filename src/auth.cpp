#include "auth.h"
#include "app.h"
#include "utils.h"

#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <filesystem>
#include <memory>
#include <random>
#include <cstdlib>
#include <jwt-cpp/jwt.h>
#include <utils_log/logger.hpp>

#if HAS_OPENSSL
#include <openssl/evp.h>
#else
#include <functional>  // fallback hash
#endif

namespace {

  std::string nextRandomId(std::size_t len) {
    if (len == 0) return {};

    static constexpr std::array<char, 62> alphabet = {
        '0','1','2','3','4','5','6','7','8','9',
        'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
        'a','b','c','d','e','f','g','h','i','j','k','l','m','n','o','p','q','r','s','t','u','v','w','x','y','z'
    };

    thread_local std::mt19937_64 rng{ std::random_device{}() };
    std::uniform_int_distribution<std::size_t> dist(0, alphabet.size() - 1);

    std::string s;
    s.reserve(len);
    for (std::size_t i = 0; i < len; ++i)
      s += alphabet[dist(rng)];
    return s;
  }

  std::string getJWTSecret() {
    static std::string secret;
    if (secret.empty()) {
      // Use env var if set, otherwise generate once and persist
      const char *env_secret = std::getenv("EMBEDDER_JWT_SECRET");
      if (env_secret) {
        secret = std::string(env_secret);
      } else {
        // Generate 32-char secret once per installation
        secret = nextRandomId(32);
        // Optional: save to .jwt_secret with restrictive permissions
      }
    }
    return secret;
  }
}

struct AdminAuth::Impl {
  const std::string PASSWORD_FILE = ".admin_password";
  const std::string DEFAULT_PASSWORD = "admin";
  std::string adminPassHash_;
  
  Impl() { loadPassword(); }

  void loadPassword() {
    // Priority: Env var > File > Default
    const char *envPass = std::getenv("EMBEDDER_ADMIN_PASSWORD");
    if (envPass) {
      std::string salt;
      adminPassHash_ = hashPassword(envPass, &salt); // Generate salt$hash
      std::cout << "Using admin password from environment variable" << std::endl;
    } else if (std::filesystem::exists(PASSWORD_FILE)) {
      std::ifstream file(PASSWORD_FILE);
      std::getline(file, adminPassHash_); // Already in salt$hash format
    } else {
      std::string salt;
      adminPassHash_ = hashPassword(DEFAULT_PASSWORD, &salt); // Generate salt$hash
    }
  }

  // pass is {'password|token', 'Bearer|Basic'}
  bool authenticate(const std::pair<std::string, std::string> &pass, std::string &jwtToken) {
    try {
      if (pass.second == "Basic") {
        std::string testHash = hashPassword(pass.first); // Uses stored salt for comparison
        size_t dollarPos = adminPassHash_.find('$');
        if (dollarPos != std::string::npos && testHash == adminPassHash_) {
          jwtToken = jwt::create()
            .set_type("JWT")
            .set_issuer("auth_server")
            .set_subject("admin")
            .set_issued_at(std::chrono::system_clock::now())
            .set_expires_at(std::chrono::system_clock::now() + std::chrono::minutes{ 30 })
            .sign(jwt::algorithm::hs256{ getJWTSecret()});

          return true;
        }
      } else if (pass.second == "Bearer") {
        auto decoded = jwt::decode(pass.first);
        jwt::verify()
          .allow_algorithm(jwt::algorithm::hs256{ getJWTSecret() })
          .with_issuer("auth_server")
          .verify(decoded);
        return true;
      }
    } catch (const std::exception &e) {
      LOG_MSG << "Error in authenticate" << e.what();
    }
    return false;
  }

  bool isDefaultPassword() {
    // Extract hash portion from stored adminPassHash_
    size_t dollarPos = adminPassHash_.find('$');
    if (dollarPos == std::string::npos) return false;

    std::string storedHash = adminPassHash_.substr(dollarPos + 1);

    // Hash DEFAULT_PASSWORD with same salt
    std::string testHash = hashPassword(DEFAULT_PASSWORD); // Uses extracted salt
    dollarPos = testHash.find('$');
    if (dollarPos == std::string::npos) return false;

    return testHash.substr(dollarPos + 1) == storedHash;
  }

  void setPassword(const std::string &new_password) {
    std::string salt;
    auto hash = hashPassword(new_password, &salt); // hash already contains salt$hash
    adminPassHash_ = hash;
    std::ofstream file(PASSWORD_FILE);
    file << adminPassHash_; // Already in salt$hash format

#ifndef _WIN32
    std::filesystem::permissions(
      PASSWORD_FILE,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace
    );
#endif
  }

  std::string hashPassword(const std::string &password, std::string *pSalt = nullptr) {
    std::string salt;

    // If verifying: extract salt from stored hash
    if (!pSalt && !adminPassHash_.empty()) {
      size_t dollarPos = adminPassHash_.find('$');
      if (dollarPos != std::string::npos) {
        salt = adminPassHash_.substr(0, dollarPos);
      }
    }

    // If setting new password: generate new salt
    if (salt.empty()) {
      salt = nextRandomId(12);
      if (pSalt) *pSalt = salt;
    }

    std::string saltedPass = salt + password;

#ifdef HAS_OPENSSL
    unsigned char hash[EVP_MAX_MD_SIZE] = { 0 };
    unsigned int hashLen = 0;

    EVP_MD_CTX *sha256 = EVP_MD_CTX_new();
    EVP_DigestInit_ex(sha256, EVP_sha256(), nullptr);
    EVP_DigestUpdate(sha256, saltedPass.data(), saltedPass.size());
    EVP_DigestFinal_ex(sha256, hash, &hashLen);
    EVP_MD_CTX_free(sha256);

    std::stringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
      ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];

    // Return salt$hash format for storage
    //return pSalt ? (salt + "$" + ss.str()) : ss.str();
    return salt + "$" + ss.str();
#else
    auto ss = fnv1a64(saltedPass);
    //return pSalt ? (salt + "$" + ss) : ss;
    return salt + "$" + ss;
#endif
  }
  std::string fileLastModifiedTime() const {
    if (!std::filesystem::exists(PASSWORD_FILE))
      return {};
    auto cftime = utils::getFileModificationTime(PASSWORD_FILE);
    std::ostringstream ss;
    ss << std::put_time(std::localtime(&cftime), "%Y-%m-%d %H:%M:%S");
    return ss.str();
  }
};

AdminAuth::AdminAuth() : imp(std::make_unique<Impl>()) {}
AdminAuth::~AdminAuth() = default;

bool AdminAuth::authenticate(const std::pair<std::string, std::string> &pass, std::string &jwtToken)
{
  return imp->authenticate(pass, jwtToken);
}

bool AdminAuth::isDefaultPassword()
{
  return imp->isDefaultPassword();
}

void AdminAuth::setPassword(const std::string &new_password)
{
  imp->setPassword(new_password);
}

std::string AdminAuth::fileLastModifiedTime() const
{
  return imp->fileLastModifiedTime();
}

std::string AdminAuth::fnv1a64(const std::string &str)
{
  const uint64_t FNV_prime = 1099511628211ull; // 0x100000001b3
  // Use the standard initial hash value
  uint64_t hash = 0xcbf29ce484222325ull;

  for (unsigned char c : str) {
    // FNV-1a: XOR then Multiply
    hash ^= c;
    hash *= FNV_prime;
    // C++ uint64_t automatically wraps (performs the modulo 2^64)
  }

  std::stringstream ss;
  // Ensure we output exactly 16 hex digits
  ss << std::hex << std::setfill('0') << std::setw(16) << hash;
  return ss.str();
}
