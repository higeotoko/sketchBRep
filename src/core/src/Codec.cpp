#include "sbr/Codec.h"

#include <array>
#include <cstdint>

namespace sbr::codec {
namespace {

constexpr std::array<std::uint32_t, 64> kRound = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

}  // namespace

std::string sha256Hex(std::string_view bytes) {
  std::array<std::uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::string message(bytes);
  const std::uint64_t bits = static_cast<std::uint64_t>(bytes.size()) * 8;
  message.push_back(static_cast<char>(0x80));
  while (message.size() % 64 != 56) message.push_back('\0');
  for (int i = 7; i >= 0; --i) message.push_back(static_cast<char>((bits >> (8 * i)) & 0xff));
  for (std::size_t block = 0; block < message.size(); block += 64) {
    std::array<std::uint32_t, 64> w{};
    for (int i = 0; i < 16; ++i) {
      for (int k = 0; k < 4; ++k) {
        w[i] = (w[i] << 8) | static_cast<unsigned char>(message[block + 4 * i + k]);
      }
    }
    for (int i = 16; i < 64; ++i) {
      const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::array<std::uint32_t, 8> v = h;
    for (int i = 0; i < 64; ++i) {
      const std::uint32_t s1 = rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25);
      const std::uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
      const std::uint32_t t1 = v[7] + s1 + ch + kRound[i] + w[i];
      const std::uint32_t s0 = rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22);
      const std::uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
      const std::uint32_t t2 = s0 + maj;
      v = {t1 + t2, v[0], v[1], v[2], v[3] + t1, v[4], v[5], v[6]};
    }
    for (int i = 0; i < 8; ++i) h[i] += v[i];
  }
  static const char hex[] = "0123456789abcdef";
  std::string out;
  for (std::uint32_t x : h) {
    for (int i = 7; i >= 0; --i) out.push_back(hex[(x >> (4 * i)) & 0xf]);
  }
  return out;
}

std::string base64Encode(std::string_view bytes) {
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  for (std::size_t i = 0; i < bytes.size(); i += 3) {
    std::uint32_t n = static_cast<unsigned char>(bytes[i]) << 16;
    if (i + 1 < bytes.size()) n |= static_cast<unsigned char>(bytes[i + 1]) << 8;
    if (i + 2 < bytes.size()) n |= static_cast<unsigned char>(bytes[i + 2]);
    out.push_back(kAlphabet[(n >> 18) & 63]);
    out.push_back(kAlphabet[(n >> 12) & 63]);
    out.push_back(i + 1 < bytes.size() ? kAlphabet[(n >> 6) & 63] : '=');
    out.push_back(i + 2 < bytes.size() ? kAlphabet[n & 63] : '=');
  }
  return out;
}

std::string base64Decode(std::string_view text) {
  auto value = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::string out;
  std::uint32_t buffer = 0;
  int bits = 0;
  for (char c : text) {
    if (c == '=' || c == '\n' || c == '\r') continue;
    const int v = value(c);
    if (v < 0) return {};
    buffer = (buffer << 6) | static_cast<std::uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((buffer >> bits) & 0xff));
    }
  }
  return out;
}

}  // namespace sbr::codec
