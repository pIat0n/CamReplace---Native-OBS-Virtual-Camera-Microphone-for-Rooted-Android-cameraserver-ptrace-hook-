#pragma once

// Obf.h — compile-time string obfuscation with per-string unique keys
// and four different cipher algorithms picked polymorphically.
// Design goals (in priority order):
//   1. Defeat IDA's "Strings" tab and `strings(1)` over the binary.
//      Plaintext exists only on the stack inside a Decoded<N> RAII
//      buffer for the duration of a single use, then is zeroed.
//      `.rodata` carries ONLY ciphertext.
//   2. Defeat one-shot deobfuscator scripts. Each call site picks
//      a different algorithm based on a compile-time seed; that
//      seed is derived from (file-name FNV1a, line, __COUNTER__,
//      build-constant), so two identical strings at different sites
//      get totally different ciphertext + key + algorithm.
//   3. Polymorphic code-gen for decryption. Every OBF() expands to
//      a separate template instantiation, so the decompiler sees N
//      different decryptor routines instead of a single function
//      that frida can hook centrally.
//   4. Survive the project's flag set: -fvisibility=hidden,
//      -fno-exceptions, -fno-rtti, -Os. No allocations, no RTTI,
//      no virtual functions, no STL containers.
// USAGE
// -----
//   foo.bar(OBF("/data/cr/feed").c_str());        // OK — temp lives till `;`
//   const char* p = OBF("dangerous").c_str();     // DANGLING. don't.
//   std::string s = OBF("safe to copy").c_str();  // OK — copies before destroy.
// Decoded<N> destructor zeroes the buffer, so any read after the full
// expression sees zeros. That's the whole point — keep plaintext lifetime
// minimal so a memory dump captured later finds nothing.
// The four algorithms (selected per-string via `seed % 4`):
//   0. Multi-round XOR/ROL/ADD/SUB chain. 5-8 rounds (round count is
//      itself derived from the seed). Each byte mixes with 4 distinct
//      per-byte keys produced by an LCG seeded with the per-string
//      key. Symmetric (decrypt = encrypt with reversed round order).
//   1. XTEA-16. Real Tiny-Encryption-Algorithm variant. 16 Feistel
//      rounds, 64-bit blocks, 128-bit key derived from per-string
//      seed via splitmix32 expansion. Strings padded to 8-byte blocks
//      (PKCS#7-ish — last byte stores the pad count). Block-level
//      diffusion: a 1-byte plaintext change propagates across 8
//      ciphertext bytes.
//   2. RC4-style stream cipher. KSA expands per-string seed into a
//      256-byte permutation; PRGA generates a keystream that XORs
//      with plaintext. Compile-time KSA + PRGA produce ciphertext
//      bytes inline in `.rodata`.
//   3. SPN (substitution-permutation network) with a per-string
//      S-box. S-box is a Fisher-Yates shuffle of 0..255 driven by
//      the per-string seed — every site has a unique substitution
//      table baked into the encrypted bytes. 4 rounds of
//      substitute-then-XOR with round keys.
// The reverser's path of least resistance: pick one site, identify
// the algorithm, extract its seed and ciphertext, write a decoder.
// Now they have ONE string. To get the rest, they either repeat per
// site, or write a generic engine that reads the seed (and only the
// seed) from each call site's machine code and dispatches to the
// right inverse. With 4 algorithms × per-byte derived keys × per-
// string round counts, building that engine is a real engineering
// project (estimated 1-2 days for a skilled reverser, vs. 5 minutes
// for plain XOR).

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace cr::obf
{

// Global OBF integrity counter. Bumped from inside .decode() when
// the runtime fingerprint of the ciphertext doesn't match the
// compile-time fingerprint — i.e. somebody patched our .rodata bytes
// at rest. This is retained as low-level diagnostic state after the
// old custom watchdog layers were removed.
// The counter is process-wide and shared across all OBF instantiations
// thanks to `inline` (C++17 — single object across TUs). We intentionally
// don't reset it: once tampered, stay tampered.
inline std::atomic<uint32_t> g_decode_mismatch{0};

// Compile-time helpers — hashes, PRNGs, bit ops.

// FNV-1a 32-bit. Deterministic compile-time string hash.
constexpr uint32_t fnv1a32(const char* s, std::size_t n, uint32_t seed = 0x811c9dc5u) noexcept
{
  uint32_t h = seed;
  for (std::size_t i = 0; i < n; ++i)
  {
    h ^= static_cast<uint8_t>(s[i]);
    h *= 0x01000193u;
  }
  return h;
}

constexpr uint32_t fnv1a32_lit(const char* s, uint32_t seed = 0x811c9dc5u) noexcept
{
  uint32_t h = seed;
  while (*s)
  {
    h ^= static_cast<uint8_t>(*s++);
    h *= 0x01000193u;
  }
  return h;
}

// xorshift32 — fast deterministic PRNG. Used for stream key generation
// inside Algorithms 0 and 3 and for the splitmix32 expansion in Alg 1.
constexpr uint32_t xorshift32(uint32_t& s) noexcept
{
  uint32_t x = s ? s : 0xdeadbeefu; // never zero — would lock at 0
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  s = x;
  return x;
}

// splitmix32 — better mixing for key expansion. Used to derive XTEA
// key material from a single seed.
constexpr uint32_t splitmix32(uint32_t& s) noexcept
{
  s += 0x9e3779b9u;
  uint32_t z = s;
  z = (z ^ (z >> 16)) * 0x85ebca6bu;
  z = (z ^ (z >> 13)) * 0xc2b2ae35u;
  return z ^ (z >> 16);
}

constexpr uint8_t rotl8(uint8_t v, int n) noexcept
{
  n &= 7;
  return static_cast<uint8_t>((v << n) | (v >> (8 - n)));
}
constexpr uint8_t rotr8(uint8_t v, int n) noexcept
{
  n &= 7;
  return static_cast<uint8_t>((v >> n) | (v << (8 - n)));
}

// Pick which algorithm this string uses. Bits 8..9 of the seed → 0..3.
constexpr int pick_algo(uint32_t seed) noexcept
{
  return static_cast<int>((seed >> 8) & 0x3u);
}

// Round count for Algorithm 0. Derived from bits 16..17 of the seed
// → 5..8 rounds. Adds one more axis the reverser has to recover.
constexpr int pick_rounds_alg0(uint32_t seed) noexcept
{
  return static_cast<int>(((seed >> 16) & 0x3u) + 5);
}

// Round count for Algorithm 3 (SPN). 3..6 rounds.
constexpr int pick_rounds_alg3(uint32_t seed) noexcept
{
  return static_cast<int>(((seed >> 18) & 0x3u) + 3);
}

// FixedString — class-type non-type template parameter (C++20 structural
// type). Lets us pass the literal as a template argument so the per-call-
// site instantiation is unique even for two identical strings.
template <std::size_t N> struct FixedString
{
  char data[N];
  constexpr FixedString(const char (&s)[N]) : data{}
  {
    for (std::size_t i = 0; i < N; ++i)
      data[i] = s[i];
  }
  constexpr std::size_t size() const noexcept
  {
    return N;
  }
};

// CTAD guide so `FixedString{"foo"}` deduces to `FixedString<4>`.
template <std::size_t N> FixedString(const char (&)[N]) -> FixedString<N>;

// Decoded<N> — RAII stack buffer. Decryption writes plaintext here; the
// destructor zeroes it. C++ guarantees temporaries live until the end of
// the full expression containing the call, so OBF(...).c_str() inside a
// function-call argument list is safe (plaintext available during the
// call, gone immediately after).
// `volatile` writes in the destructor prevent the optimizer from
// removing the zero-fill as a dead store ("you don't read it after
// you write it" — true, but we WANT the bytes overwritten).
template <std::size_t N> struct Decoded
{
  char buf[N];

  // Default ctor leaves buf uninit. The decode function fills it.
  // Copy is deleted to prevent `auto x = OBF(...);` accidentally
  // duplicating plaintext. Move IS allowed so NRVO can elide the
  // return from decode() — in optimized builds the move never runs;
  // even if it does, char[] move == copy, lifetime still bounded by
  // full-expression rules.
  Decoded() = default;
  Decoded(const Decoded&) = delete;
  Decoded& operator=(const Decoded&) = delete;
  Decoded(Decoded&&) noexcept = default;
  Decoded& operator=(Decoded&&) noexcept = default;

  ~Decoded() noexcept
  {
    volatile char* p = buf;
    for (std::size_t i = 0; i < N; ++i)
      p[i] = 0;
  }

  const char* c_str() const noexcept
  {
    return buf;
  }
  operator const char*() const noexcept
  {
    return buf;
  }
  std::size_t size() const noexcept
  {
    return N - 1;
  } // exclude NUL

  // We deliberately do NOT define `operator std::string()`. With both
  // `operator const char*` and `operator std::string()` present, the
  // compiler can't choose during `std::string s = OBF("foo");` — both
  // are equally good candidates and the call is ambiguous.
  // The recommended idiom for storage that outlives the full expression:
  //   std::string caption = OBF("Camera Replace").c_str();  // copy via
  //                                                           // const char*
  //                                                           // → std::string
  //                                                           // ctor
  // The OBF temporary lives until the `;` of the declaration. The
  // std::string ctor reads through .c_str() and copies the bytes
  // into its own (heap or SSO) storage BEFORE the Decoded
  // destructor wipes the buffer. After that line runs the plaintext
  // lifetime is owned entirely by `caption`.
};

// Algorithm 0 — Multi-round XOR/ROL/ADD/SUB chain.
// Per-byte derives 4 keys from the per-string seed, applied in a 5-8 round
// chain. Symmetric: encryption and decryption are the same function with
// the round order reversed. We bake the encrypted bytes at compile time
// and run the decrypt at the call site.
namespace alg0
{

constexpr uint8_t derive_key(uint32_t seed, std::size_t i, int slot) noexcept
{
  uint32_t s = seed ^ (static_cast<uint32_t>(i) * 0x9e3779b9u) ^ (static_cast<uint32_t>(slot) * 0x85ebca6bu);
  return static_cast<uint8_t>(splitmix32(s) >> 24);
}

constexpr uint8_t encrypt_byte(uint8_t b, std::size_t i, uint32_t seed, int rounds) noexcept
{
  for (int r = 0; r < rounds; ++r)
  {
    const uint8_t k0 = derive_key(seed, i, r * 4 + 0);
    const uint8_t k1 = derive_key(seed, i, r * 4 + 1);
    const uint8_t k2 = derive_key(seed, i, r * 4 + 2);
    const uint8_t k3 = derive_key(seed, i, r * 4 + 3);
    b = static_cast<uint8_t>(b + k0);
    b ^= rotl8(k1, k2 & 7);
    b = static_cast<uint8_t>(b - k2);
    b ^= k3;
  }
  return b;
}

constexpr uint8_t decrypt_byte(uint8_t b, std::size_t i, uint32_t seed, int rounds) noexcept
{
  for (int r = rounds - 1; r >= 0; --r)
  {
    const uint8_t k0 = derive_key(seed, i, r * 4 + 0);
    const uint8_t k1 = derive_key(seed, i, r * 4 + 1);
    const uint8_t k2 = derive_key(seed, i, r * 4 + 2);
    const uint8_t k3 = derive_key(seed, i, r * 4 + 3);
    b ^= k3;
    b = static_cast<uint8_t>(b + k2);
    b ^= rotl8(k1, k2 & 7);
    b = static_cast<uint8_t>(b - k0);
  }
  return b;
}

} // namespace alg0

// Algorithm 1 — XTEA-16 (Tiny Encryption Algorithm extended).
// 16 Feistel rounds over 64-bit blocks with a 128-bit key. 1-byte plaintext
// change diffuses across 8 ciphertext bytes. Strings get padded to an
// 8-byte boundary; pad count is appended in the last byte (PKCS#7-ish).
namespace alg1
{

constexpr int kRounds = 16; // 16 rounds = 32 cycles, plenty for short strings.

constexpr void expand_key(uint32_t seed, uint32_t k[4]) noexcept
{
  uint32_t s = seed;
  for (int i = 0; i < 4; ++i)
    k[i] = splitmix32(s);
}

constexpr void xtea_encrypt_block(uint32_t v[2], const uint32_t k[4]) noexcept
{
  constexpr uint32_t kDelta = 0x9e3779b9u;
  uint32_t v0 = v[0], v1 = v[1], sum = 0;
  for (int i = 0; i < kRounds; ++i)
  {
    v0 += (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + k[sum & 3]);
    sum += kDelta;
    v1 += (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (sum + k[(sum >> 11) & 3]);
  }
  v[0] = v0;
  v[1] = v1;
}

constexpr void xtea_decrypt_block(uint32_t v[2], const uint32_t k[4]) noexcept
{
  constexpr uint32_t kDelta = 0x9e3779b9u;
  uint32_t v0 = v[0], v1 = v[1];
  uint32_t sum = static_cast<uint32_t>(kDelta) * static_cast<uint32_t>(kRounds);
  for (int i = 0; i < kRounds; ++i)
  {
    v1 -= (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (sum + k[(sum >> 11) & 3]);
    sum -= kDelta;
    v0 -= (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + k[sum & 3]);
  }
  v[0] = v0;
  v[1] = v1;
}

// Padded length: round up to next 8-byte boundary, always producing at
// least one full pad byte (so we always know how to strip on decrypt).
constexpr std::size_t padded_len(std::size_t n) noexcept
{
  return (n + 8) & ~static_cast<std::size_t>(7);
}

} // namespace alg1

// Algorithm 2 — RC4-style stream cipher.
// KSA expands the per-string seed into a 256-byte permutation, then PRGA
// generates a keystream of length matching the plaintext. XOR plaintext
// with keystream = ciphertext. Symmetric (XOR is its own inverse).
namespace alg2
{

struct State
{
  uint8_t S[256];
  int i = 0;
  int j = 0;
};

constexpr void ksa(State& st, uint32_t seed) noexcept
{
  for (int k = 0; k < 256; ++k)
    st.S[k] = static_cast<uint8_t>(k);
  // Derive a 16-byte key from the seed via splitmix32.
  uint8_t key[16];
  {
    uint32_t s = seed;
    for (int k = 0; k < 16; k += 4)
    {
      uint32_t w = splitmix32(s);
      key[k + 0] = static_cast<uint8_t>(w);
      key[k + 1] = static_cast<uint8_t>(w >> 8);
      key[k + 2] = static_cast<uint8_t>(w >> 16);
      key[k + 3] = static_cast<uint8_t>(w >> 24);
    }
  }
  int j = 0;
  for (int k = 0; k < 256; ++k)
  {
    j = (j + st.S[k] + key[k & 15]) & 0xff;
    uint8_t t = st.S[k];
    st.S[k] = st.S[j];
    st.S[j] = t;
  }
}

constexpr uint8_t prga(State& st) noexcept
{
  st.i = (st.i + 1) & 0xff;
  st.j = (st.j + st.S[st.i]) & 0xff;
  uint8_t t = st.S[st.i];
  st.S[st.i] = st.S[st.j];
  st.S[st.j] = t;
  return st.S[(st.S[st.i] + st.S[st.j]) & 0xff];
}

} // namespace alg2

// Algorithm 3 — SPN with a per-string S-box.
// Compile-time generates a 256-byte S-box from the seed via Fisher-Yates.
// Each round substitutes through the S-box and XORs with a round key.
// Inverse uses the inverted S-box (also generated at CT). 3-6 rounds.
namespace alg3
{

struct SBox
{
  uint8_t fwd[256];
  uint8_t inv[256];
};

constexpr SBox make_sbox(uint32_t seed) noexcept
{
  SBox b{};
  for (int k = 0; k < 256; ++k)
    b.fwd[k] = static_cast<uint8_t>(k);
  // Fisher-Yates shuffle driven by xorshift32 of seed.
  uint32_t s = seed | 1; // never zero
  for (int k = 255; k > 0; --k)
  {
    uint32_t r = xorshift32(s);
    int p = static_cast<int>(r % static_cast<uint32_t>(k + 1));
    uint8_t tmp = b.fwd[k];
    b.fwd[k] = b.fwd[p];
    b.fwd[p] = tmp;
  }
  for (int k = 0; k < 256; ++k)
    b.inv[b.fwd[k]] = static_cast<uint8_t>(k);
  return b;
}

constexpr uint8_t round_key(uint32_t seed, int r, std::size_t i) noexcept
{
  uint32_t s = seed ^ (static_cast<uint32_t>(r) * 0xc2b2ae35u) ^ (static_cast<uint32_t>(i) * 0x27d4eb2fu);
  return static_cast<uint8_t>(splitmix32(s) >> 24);
}

} // namespace alg3

// ObfStr<S, Seed> — the heart of it. Holds ciphertext (computed at
// compile time from S and Seed via the algorithm picked by Seed).
// .decode() runs the inverse at use site, writing into a Decoded<N> stack
// buffer and returning it. Decoded's destructor zeroes the buffer.
template <FixedString S, uint32_t Seed> struct ObfStr
{
  static constexpr std::size_t kPlainN = S.size();

  // Algorithm 1 (XTEA) needs padded storage. The other algorithms keep
  // ciphertext at exactly kPlainN bytes.
  static constexpr int kAlgo = pick_algo(Seed);
  static constexpr std::size_t kCtN = (kAlgo == 1) ? alg1::padded_len(kPlainN) : kPlainN;

  uint8_t ct[kCtN];
  uint8_t pad_count; // alg1 only — how many bytes of padding at the end

  // === COMPILE-TIME ENCRYPT ===
  // Computes ct[] from S.data at instantiation. The static-constexpr
  // member of the wrapper-fn (see make_obf below) lands in .rodata, so
  // the binary contains ONLY the encrypted bytes, never plaintext.
  constexpr ObfStr() noexcept : ct{}, pad_count(0)
  {
    if constexpr (kAlgo == 0)
    {
      const int rounds = pick_rounds_alg0(Seed);
      for (std::size_t i = 0; i < kPlainN; ++i)
      {
        ct[i] = alg0::encrypt_byte(static_cast<uint8_t>(S.data[i]), i, Seed, rounds);
      }
    }
    else if constexpr (kAlgo == 1)
    {
      uint32_t k[4]{};
      alg1::expand_key(Seed, k);
      // PKCS#7-ish: pad to 8-byte boundary; pad byte = pad count.
      const std::size_t plen = alg1::padded_len(kPlainN);
      const std::size_t npad = plen - kPlainN;
      pad_count = static_cast<uint8_t>(npad);
      uint8_t buf[kCtN]{};
      for (std::size_t i = 0; i < kPlainN; ++i)
        buf[i] = static_cast<uint8_t>(S.data[i]);
      for (std::size_t i = kPlainN; i < plen; ++i)
        buf[i] = static_cast<uint8_t>(npad);
      // Encrypt block-by-block.
      for (std::size_t off = 0; off < plen; off += 8)
      {
        uint32_t v[2] = {
            static_cast<uint32_t>(buf[off + 0]) | (static_cast<uint32_t>(buf[off + 1]) << 8) | (static_cast<uint32_t>(buf[off + 2]) << 16) | (static_cast<uint32_t>(buf[off + 3]) << 24),
            static_cast<uint32_t>(buf[off + 4]) | (static_cast<uint32_t>(buf[off + 5]) << 8) | (static_cast<uint32_t>(buf[off + 6]) << 16) | (static_cast<uint32_t>(buf[off + 7]) << 24),
        };
        alg1::xtea_encrypt_block(v, k);
        ct[off + 0] = static_cast<uint8_t>(v[0]);
        ct[off + 1] = static_cast<uint8_t>(v[0] >> 8);
        ct[off + 2] = static_cast<uint8_t>(v[0] >> 16);
        ct[off + 3] = static_cast<uint8_t>(v[0] >> 24);
        ct[off + 4] = static_cast<uint8_t>(v[1]);
        ct[off + 5] = static_cast<uint8_t>(v[1] >> 8);
        ct[off + 6] = static_cast<uint8_t>(v[1] >> 16);
        ct[off + 7] = static_cast<uint8_t>(v[1] >> 24);
      }
    }
    else if constexpr (kAlgo == 2)
    {
      alg2::State st{};
      alg2::ksa(st, Seed);
      for (std::size_t i = 0; i < kPlainN; ++i)
        ct[i] = static_cast<uint8_t>(S.data[i]) ^ alg2::prga(st);
    }
    else /* kAlgo == 3 */
    {
      const auto sbox = alg3::make_sbox(Seed);
      const int rounds = pick_rounds_alg3(Seed);
      for (std::size_t i = 0; i < kPlainN; ++i)
      {
        uint8_t b = static_cast<uint8_t>(S.data[i]);
        for (int r = 0; r < rounds; ++r)
        {
          b ^= alg3::round_key(Seed, r, i);
          b = sbox.fwd[b];
        }
        ct[i] = b;
      }
    }
  }

  // === RUN-TIME DECRYPT ===
  // Polymorphic via if-constexpr; each instantiation compiles to its
  // own machine code. No runtime branch on `kAlgo` — the compiler picks
  // the body at compile time.
  Decoded<kPlainN> decode() const noexcept
  {
    Decoded<kPlainN> out{};
    if constexpr (kAlgo == 0)
    {
      const int rounds = pick_rounds_alg0(Seed);
      for (std::size_t i = 0; i < kPlainN; ++i)
      {
        out.buf[i] = static_cast<char>(alg0::decrypt_byte(ct[i], i, Seed, rounds));
      }
    }
    else if constexpr (kAlgo == 1)
    {
      uint32_t k[4]{};
      alg1::expand_key(Seed, k);
      uint8_t buf[kCtN];
      for (std::size_t i = 0; i < kCtN; ++i)
        buf[i] = ct[i];
      for (std::size_t off = 0; off < kCtN; off += 8)
      {
        uint32_t v[2] = {
            static_cast<uint32_t>(buf[off + 0]) | (static_cast<uint32_t>(buf[off + 1]) << 8) | (static_cast<uint32_t>(buf[off + 2]) << 16) | (static_cast<uint32_t>(buf[off + 3]) << 24),
            static_cast<uint32_t>(buf[off + 4]) | (static_cast<uint32_t>(buf[off + 5]) << 8) | (static_cast<uint32_t>(buf[off + 6]) << 16) | (static_cast<uint32_t>(buf[off + 7]) << 24),
        };
        alg1::xtea_decrypt_block(v, k);
        buf[off + 0] = static_cast<uint8_t>(v[0]);
        buf[off + 1] = static_cast<uint8_t>(v[0] >> 8);
        buf[off + 2] = static_cast<uint8_t>(v[0] >> 16);
        buf[off + 3] = static_cast<uint8_t>(v[0] >> 24);
        buf[off + 4] = static_cast<uint8_t>(v[1]);
        buf[off + 5] = static_cast<uint8_t>(v[1] >> 8);
        buf[off + 6] = static_cast<uint8_t>(v[1] >> 16);
        buf[off + 7] = static_cast<uint8_t>(v[1] >> 24);
      }
      // Copy plaintext bytes (strip padding).
      for (std::size_t i = 0; i < kPlainN; ++i)
        out.buf[i] = static_cast<char>(buf[i]);
      // Wipe the local buf — it held plaintext for an instant.
      volatile uint8_t* p = buf;
      for (std::size_t i = 0; i < kCtN; ++i)
        p[i] = 0;
    }
    else if constexpr (kAlgo == 2)
    {
      alg2::State st{};
      alg2::ksa(st, Seed);
      for (std::size_t i = 0; i < kPlainN; ++i)
      {
        out.buf[i] = static_cast<char>(ct[i] ^ alg2::prga(st));
      }
    }
    else /* kAlgo == 3 */
    {
      const auto sbox = alg3::make_sbox(Seed);
      const int rounds = pick_rounds_alg3(Seed);
      for (std::size_t i = 0; i < kPlainN; ++i)
      {
        uint8_t b = ct[i];
        for (int r = rounds - 1; r >= 0; --r)
        {
          b = sbox.inv[b];
          b ^= alg3::round_key(Seed, r, i);
        }
        out.buf[i] = static_cast<char>(b);
      }
    }
    return out;
  }
};

// Per-call-site seed. Mixes the file path's FNV1a, line number, counter
// and a build-time pepper. __COUNTER__ guarantees uniqueness even for
// two OBF()s on the same line.
// The build pepper is a single anchor constant — change it once per
// build and ALL ciphertext rotates without code changes. We don't hook
// __TIME__/__DATE__ here because that would break ccache; if you want
// per-build rotation, edit `kBuildPepper` manually.
constexpr uint32_t kBuildPepper = 0xCB1DBEEFu;

#define CR_OBF_SEED(extra) ((::cr::obf::fnv1a32_lit(__FILE__)) ^ (static_cast<uint32_t>(__LINE__) * 0x01000193u) ^ (static_cast<uint32_t>(__COUNTER__) * 0x9e3779b9u) ^ ::cr::obf::kBuildPepper ^ static_cast<uint32_t>(extra))

// User-facing macro. Wraps a fresh ObfStr in a function template so the
// compile-time-encrypted ciphertext lives as a `static constexpr` member
// in the function — i.e., in `.rodata`.
// Returns a Decoded<N> by value. The temporary is bound to the full
// expression, so:
//   foo.bar(OBF("...").c_str());        // OK — Decoded lives till `;`
//   const char* p = OBF("...").c_str(); // DANGLING — Decoded already gone
// To stash a string for longer than the call, copy into std::string:
//   std::string s = OBF("...").c_str();
// Compile-time FNV-1a over a byte array — used to fingerprint the
// ciphertext at build time AND at the first runtime use. If the two
// fingerprints disagree, somebody patched our .rodata bytes at rest
// (debugger memory-write, code-injection inline replacement, …). Bump
// the global OBF integrity counter for diagnostics.
template <size_t N> constexpr uint32_t fingerprint_bytes(const uint8_t (&b)[N]) noexcept
{
  uint32_t h = 0x811c9dc5u;
  for (size_t i = 0; i < N; ++i)
  {
    h ^= b[i];
    h *= 0x01000193u;
  }
  return h;
}

template <FixedString S, uint32_t Seed> inline auto _decode_at_site() noexcept
{
  static constexpr ObfStr<S, Seed> kEnc{};

  // Verify the ciphertext .rodata bytes haven't been patched by a
  // memory-write debugger. Done on EVERY decode, not just the first
  // — late-stage tampering (e.g. frida.WriteByteArray after the
  // string was already decoded once) would otherwise slip through.
  // Cost: ~32-byte FNV pass per OBF call ≈ a few ns. The decode
  // itself runs the cipher, far more expensive.
  constexpr uint32_t kExpected = fingerprint_bytes(kEnc.ct);
  // Volatile pointer prevents the compiler from folding the runtime
  // read against the constexpr value (the bytes ARE the same at
  // compile time, so the compiler could prove h==kExpected always).
  const volatile uint8_t* vp = kEnc.ct;
  uint32_t h = 0x811c9dc5u;
  for (size_t i = 0; i < sizeof(kEnc.ct); ++i)
  {
    h ^= vp[i];
    h *= 0x01000193u;
  }
  if (h != kExpected)
  {
    g_decode_mismatch.fetch_add(1, std::memory_order_relaxed);
  }

  return kEnc.decode();
}

#define OBF(s) ::cr::obf::_decode_at_site<::cr::obf::FixedString{s}, CR_OBF_SEED(0)>()

inline std::wstring widen_ascii(const char* s) noexcept
{
  std::wstring out;
  if (!s)
    return out;
  while (*s)
  {
    out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*s++)));
  }
  return out;
}

#define OBFW(s) ::cr::obf::widen_ascii(OBF(s).c_str())

// Variant: returns std::string-friendly char-pointer with a known lifetime
// guarantee for callers that need to capture the value into longer storage.
// Same expansion, just spelled differently for clarity at call sites that
// are about to copy the bytes anyway.
#define OBF_C(s) (OBF(s).c_str())

} // namespace cr::obf
