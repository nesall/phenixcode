#include "symbol_service.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace symbol_service {
  std::string sha256Hex(std::string_view data) {
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
    auto rotr = [](uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); };

    std::array<uint32_t, 8> state = {
      0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
    };

    const uint64_t totalBits = static_cast<uint64_t>(data.size()) * 8ULL;
    const size_t rem = data.size() % 64;
    const size_t paddedSize = data.size() + (rem < 56 ? (56 - rem) : (120 - rem)) + 8;

    auto readByte = [&](size_t idx) -> uint8_t {
      if (idx < data.size()) return static_cast<uint8_t>(data[idx]);
      if (idx == data.size()) return 0x80u;
      if (idx >= paddedSize - 8) {
        const size_t shift = (paddedSize - 1 - idx) * 8;
        return static_cast<uint8_t>((totalBits >> shift) & 0xffu);
      }
      return 0u;
    };

    std::array<uint32_t, 64> w{};
    for (size_t offset = 0; offset < paddedSize; offset += 64) {
      for (size_t i = 0; i < 16; ++i) {
        const size_t b = offset + i * 4;
        w[i] = (static_cast<uint32_t>(readByte(b)) << 24) |
               (static_cast<uint32_t>(readByte(b + 1)) << 16) |
               (static_cast<uint32_t>(readByte(b + 2)) << 8) |
               static_cast<uint32_t>(readByte(b + 3));
      }
      for (size_t i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
      }
      uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
      uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
      for (size_t i = 0; i < 64; ++i) {
        const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t temp1 = h + S1 + ch + k[i] + w[i];
        const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = S0 + maj;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
      }
      state[0] += a; state[1] += b; state[2] += c; state[3] += d;
      state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }

    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.resize(64);
    for (size_t i = 0; i < 8; ++i) {
      for (size_t j = 0; j < 8; ++j) {
        out[i * 8 + j] = hex[(state[i] >> ((7 - j) * 4)) & 0xfu];
      }
    }
    return out;
  }

  SymbolService::SymbolService(size_t maxCacheEntries)
    : maxCacheEntries_((std::max)(size_t(1), maxCacheEntries)) {
    detail::ParserHandle parser;
    for (const Language lang : {Language::C, Language::Cpp, Language::Python,
                                Language::JavaScript, Language::TypeScript, Language::Tsx}) {
      detail::ensureLanguage(parser.raw, lang);
    }
  }

  size_t SymbolService::cachedFileCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return index_.size();
  }

  void SymbolService::clearCache() {
    std::lock_guard<std::mutex> lock(mutex_);
    lru_.clear();
    index_.clear();
  }

} // namespace symbol_service
