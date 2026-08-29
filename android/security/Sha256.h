#pragma once

// Sha256 — minimal SHA-256 implementation for self-text integrity.
// Self-contained, no libc dependencies beyond memcpy/memset (which are
// trivially inlinable). We deliberately avoid linking BoringSSL / OpenSSL
// here because that would (a) add a 1.5 MB transitive dep to libcr_*hook.so
// and (b) put recognisable big-name symbols in our binary. A 150-line
// inlined transform compiles to a few KB of code that doesn't show up in
// any "look for libcrypto" reverser heuristic.
// USAGE
//   uint8_t digest[32];
//   cr::sha256::Hash h;
//   h.init();
//   h.update(bytes, n);
//   h.final(digest);
// Or one-shot:
//   cr::sha256::compute(bytes, n, digest);

#include <cstdint>
#include <cstring>

namespace cr::sha256
{

namespace detail
{

constexpr uint32_t kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

inline uint32_t rotr(uint32_t v, int n) noexcept
{
  return (v >> n) | (v << (32 - n));
}

} // namespace detail

struct Hash
{
  uint32_t state[8];
  uint8_t buf[64];
  uint64_t bytes;
  uint32_t buf_len;

  void init() noexcept
  {
    state[0] = 0x6a09e667u;
    state[1] = 0xbb67ae85u;
    state[2] = 0x3c6ef372u;
    state[3] = 0xa54ff53au;
    state[4] = 0x510e527fu;
    state[5] = 0x9b05688cu;
    state[6] = 0x1f83d9abu;
    state[7] = 0x5be0cd19u;
    bytes = 0;
    buf_len = 0;
  }

  void transform(const uint8_t* block) noexcept
  {
    using detail::rotr;
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
    {
      w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) | (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i)
    {
      uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; ++i)
    {
      uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      uint32_t ch = (e & f) ^ (~e & g);
      uint32_t t1 = h + S1 + ch + detail::kK[i] + w[i];
      uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t t2 = S0 + mj;
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
  }

  void update(const void* data, size_t n) noexcept
  {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    bytes += n;
    if (buf_len)
    {
      uint32_t take = 64 - buf_len;
      if (take > n)
        take = static_cast<uint32_t>(n);
      std::memcpy(buf + buf_len, p, take);
      buf_len += take;
      p += take;
      n -= take;
      if (buf_len == 64)
      {
        transform(buf);
        buf_len = 0;
      }
    }
    while (n >= 64)
    {
      transform(p);
      p += 64;
      n -= 64;
    }
    if (n)
    {
      std::memcpy(buf, p, n);
      buf_len = static_cast<uint32_t>(n);
    }
  }

  void final(uint8_t out[32]) noexcept
  {
    uint64_t bits = bytes * 8;
    buf[buf_len++] = 0x80;
    if (buf_len > 56)
    {
      std::memset(buf + buf_len, 0, 64 - buf_len);
      transform(buf);
      buf_len = 0;
    }
    std::memset(buf + buf_len, 0, 56 - buf_len);
    for (int i = 0; i < 8; ++i)
    {
      buf[56 + i] = static_cast<uint8_t>(bits >> (56 - i * 8));
    }
    transform(buf);
    for (int i = 0; i < 8; ++i)
    {
      out[i * 4] = static_cast<uint8_t>(state[i] >> 24);
      out[i * 4 + 1] = static_cast<uint8_t>(state[i] >> 16);
      out[i * 4 + 2] = static_cast<uint8_t>(state[i] >> 8);
      out[i * 4 + 3] = static_cast<uint8_t>(state[i]);
    }
  }
};

inline void compute(const void* data, size_t n, uint8_t out[32]) noexcept
{
  Hash h;
  h.init();
  h.update(data, n);
  h.final(out);
}

} // namespace cr::sha256
