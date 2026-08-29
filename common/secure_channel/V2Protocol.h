#pragma once

#include "Status.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cr::secure_channel::v2
{

inline constexpr std::uint8_t kVersion = 2;
inline constexpr std::size_t kKeyBytes = 32;
inline constexpr std::size_t kNonceBytes = 24;
inline constexpr std::size_t kSessionIdBytes = 16;
inline constexpr std::size_t kTagBytes = 16;
inline constexpr std::size_t kTicketUnsignedBytes = 128;
inline constexpr std::size_t kTicketBytes = 192;
inline constexpr std::size_t kRecordHeaderBytes = 36;
inline constexpr std::size_t kMaxCiphertextBytes = 1024 * 1024;

struct Bytes
{
  const std::uint8_t* data{nullptr};
  std::size_t size{0};
  [[nodiscard]] constexpr bool valid() const noexcept
  {
    return data || size == 0;
  }
};
template <std::size_t N> [[nodiscard]] constexpr Bytes bytes_of(const std::array<std::uint8_t, N>& v) noexcept
{
  return {v.data(), v.size()};
}
[[nodiscard]] inline Bytes bytes_of(const std::vector<std::uint8_t>& v) noexcept
{
  return {v.data(), v.size()};
}

enum class StreamKind : std::uint8_t
{
  Video = 1,
  Audio = 2
};
enum class Role : std::uint8_t
{
  Host = 1,
  Phone = 2
};
enum class RecordType : std::uint8_t
{
  Application = 1,
  Finished = 2,
  Close = 3
};

struct Ticket
{
  StreamKind stream{StreamKind::Video};
  std::uint64_t issued_at_ms{0};
  std::uint64_t expires_at_ms{0};
  std::array<std::uint8_t, kSessionIdBytes> session_id{};
  std::array<std::uint8_t, kKeyBytes> host_ephemeral_public{};
  std::array<std::uint8_t, kKeyBytes> phone_ephemeral_public{};
  std::array<std::uint8_t, kNonceBytes> phone_nonce{};
  std::array<std::uint8_t, 64> signature{};
};

struct TicketView
{
  Ticket ticket{};
  Bytes unsigned_bytes{};
  Bytes signature{};
};

struct RecordHeader
{
  RecordType type{RecordType::Application};
  std::uint16_t flags{0};
  std::array<std::uint8_t, kSessionIdBytes> session_id{};
  std::uint64_t sequence{0};
  std::uint32_t ciphertext_size{0};
};
struct RecordView
{
  RecordHeader header{};
  Bytes ciphertext{};
  Bytes tag{};
};

namespace detail
{
inline void put_u16(std::vector<std::uint8_t>& out, std::uint16_t v)
{
  out.push_back(static_cast<std::uint8_t>(v));
  out.push_back(static_cast<std::uint8_t>(v >> 8));
}
inline void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v)
{
  for (int s = 0; s < 32; s += 8)
    out.push_back(static_cast<std::uint8_t>(v >> s));
}
inline void put_u64(std::vector<std::uint8_t>& out, std::uint64_t v)
{
  for (int s = 0; s < 64; s += 8)
    out.push_back(static_cast<std::uint8_t>(v >> s));
}
inline void put(std::vector<std::uint8_t>& out, Bytes in)
{
  if (in.size)
    out.insert(out.end(), in.data, in.data + in.size);
}
class Reader
{
public:
  explicit Reader(Bytes in) : in_(in) {}
  Status u8(std::uint8_t& v) noexcept
  {
    return take(1, &v);
  }
  Status u16(std::uint16_t& v) noexcept
  {
    std::uint8_t b[2];
    if (auto s = take(2, b); !s)
      return s;
    v = static_cast<std::uint16_t>(b[0] | (std::uint16_t{b[1]} << 8));
    return {};
  }
  Status u32(std::uint32_t& v) noexcept
  {
    std::uint8_t b[4];
    if (auto s = take(4, b); !s)
      return s;
    v = std::uint32_t{b[0]} | (std::uint32_t{b[1]} << 8) | (std::uint32_t{b[2]} << 16) | (std::uint32_t{b[3]} << 24);
    return {};
  }
  Status u64(std::uint64_t& v) noexcept
  {
    std::uint8_t b[8];
    if (auto s = take(8, b); !s)
      return s;
    v = 0;
    for (int i = 7; i >= 0; --i)
      v = (v << 8) | b[i];
    return {};
  }
  Status bytes(std::size_t n, Bytes& v) noexcept
  {
    if (!in_.valid() || n > in_.size - pos_)
      return Status::failure(Error::Truncated, pos_);
    v = {in_.data + pos_, n};
    pos_ += n;
    return {};
  }
  [[nodiscard]] std::size_t pos() const noexcept
  {
    return pos_;
  }
  [[nodiscard]] std::size_t remaining() const noexcept
  {
    return in_.valid() ? in_.size - pos_ : 0;
  }

private:
  Status take(std::size_t n, std::uint8_t* out) noexcept
  {
    Bytes b;
    if (auto s = bytes(n, b); !s)
      return s;
    for (std::size_t i = 0; i < n; ++i)
      out[i] = b.data[i];
    return {};
  }
  Bytes in_;
  std::size_t pos_{0};
};
inline bool valid_stream(std::uint8_t v) noexcept
{
  return v == 1 || v == 2;
}
inline bool valid_type(std::uint8_t v) noexcept
{
  return v >= 1 && v <= 3;
}
} // namespace detail

// Canonical ticket serialization. Numeric fields are little-endian; magic bytes
// are literal ASCII. The signature is always over the first 128 bytes.
[[nodiscard]] inline Status encode_ticket(const Ticket& ticket, std::vector<std::uint8_t>& out)
{
  if (ticket.expires_at_ms <= ticket.issued_at_ms)
    return Status::failure(Error::InvalidField);
  out.clear();
  out.reserve(kTicketBytes);
  detail::put_u32(out, 0x32544353U); // bytes "SCT2"
  out.push_back(kVersion);
  out.push_back(static_cast<std::uint8_t>(ticket.stream));
  detail::put_u16(out, 0);
  detail::put_u64(out, ticket.issued_at_ms);
  detail::put_u64(out, ticket.expires_at_ms);
  detail::put(out, bytes_of(ticket.session_id));
  detail::put(out, bytes_of(ticket.host_ephemeral_public));
  detail::put(out, bytes_of(ticket.phone_ephemeral_public));
  detail::put(out, bytes_of(ticket.phone_nonce));
  detail::put(out, bytes_of(ticket.signature));
  return {};
}

[[nodiscard]] inline Status parse_ticket(Bytes in, TicketView& out) noexcept
{
  if (!in.valid() || in.size != kTicketBytes)
    return Status::failure(Error::LengthOutOfRange);
  detail::Reader r(in);
  std::uint32_t magic;
  std::uint8_t version, stream;
  std::uint16_t flags;
  Ticket t;
  if (auto s = r.u32(magic); !s)
    return s;
  if (magic != 0x32544353U)
    return Status::failure(Error::BadMagic);
  if (auto s = r.u8(version); !s)
    return s;
  if (version != kVersion)
    return Status::failure(Error::UnsupportedVersion, 4);
  if (auto s = r.u8(stream); !s)
    return s;
  if (!detail::valid_stream(stream))
    return Status::failure(Error::InvalidField, 5);
  if (auto s = r.u16(flags); !s)
    return s;
  if (flags != 0)
    return Status::failure(Error::NonCanonicalEncoding, 6);
  if (auto s = r.u64(t.issued_at_ms); !s)
    return s;
  if (auto s = r.u64(t.expires_at_ms); !s)
    return s;
  if (t.expires_at_ms <= t.issued_at_ms)
    return Status::failure(Error::InvalidField, 16);
  auto read_array = [&r](auto& a) -> Status
  {
    Bytes b;
    if (auto s = r.bytes(a.size(), b); !s)
      return s;
    for (std::size_t i = 0; i < a.size(); ++i)
      a[i] = b.data[i];
    return {};
  };
  if (auto s = read_array(t.session_id); !s)
    return s;
  if (auto s = read_array(t.host_ephemeral_public); !s)
    return s;
  if (auto s = read_array(t.phone_ephemeral_public); !s)
    return s;
  if (auto s = read_array(t.phone_nonce); !s)
    return s;
  if (r.pos() != kTicketUnsignedBytes)
    return Status::failure(Error::NonCanonicalEncoding, r.pos());
  if (auto s = read_array(t.signature); !s)
    return s;
  if (r.remaining())
    return Status::failure(Error::TrailingData, r.pos());
  t.stream = static_cast<StreamKind>(stream);
  out = {t, {in.data, kTicketUnsignedBytes}, {in.data + kTicketUnsignedBytes, 64}};
  return {};
}

[[nodiscard]] inline Status encode_record_header(const RecordHeader& h, std::vector<std::uint8_t>& out)
{
  if (!detail::valid_type(static_cast<std::uint8_t>(h.type)) || h.flags != 0 || h.ciphertext_size > kMaxCiphertextBytes)
    return Status::failure(Error::InvalidField);
  out.clear();
  out.reserve(kRecordHeaderBytes);
  detail::put_u32(out, 0x32524353U); // "SCR2"
  out.push_back(kVersion);
  out.push_back(static_cast<std::uint8_t>(h.type));
  detail::put_u16(out, h.flags);
  detail::put(out, bytes_of(h.session_id));
  detail::put_u64(out, h.sequence);
  detail::put_u32(out, h.ciphertext_size);
  return {};
}

[[nodiscard]] inline Status parse_record(Bytes in, RecordView& out) noexcept
{
  if (!in.valid() || in.size < kRecordHeaderBytes + kTagBytes)
    return Status::failure(Error::Truncated);
  detail::Reader r(in);
  std::uint32_t magic;
  std::uint8_t version, type;
  RecordHeader h;
  if (auto s = r.u32(magic); !s)
    return s;
  if (magic != 0x32524353U)
    return Status::failure(Error::BadMagic);
  if (auto s = r.u8(version); !s)
    return s;
  if (version != kVersion)
    return Status::failure(Error::UnsupportedVersion, 4);
  if (auto s = r.u8(type); !s)
    return s;
  if (!detail::valid_type(type))
    return Status::failure(Error::InvalidField, 5);
  if (auto s = r.u16(h.flags); !s)
    return s;
  if (h.flags != 0)
    return Status::failure(Error::NonCanonicalEncoding, 6);
  Bytes id, ct, tag;
  if (auto s = r.bytes(kSessionIdBytes, id); !s)
    return s;
  for (std::size_t i = 0; i < id.size; ++i)
    h.session_id[i] = id.data[i];
  if (auto s = r.u64(h.sequence); !s)
    return s;
  if (auto s = r.u32(h.ciphertext_size); !s)
    return s;
  if (h.ciphertext_size > kMaxCiphertextBytes)
    return Status::failure(Error::LengthOutOfRange, r.pos());
  if (r.remaining() != std::size_t{h.ciphertext_size} + kTagBytes)
    return Status::failure(r.remaining() < std::size_t{h.ciphertext_size} + kTagBytes ? Error::Truncated : Error::TrailingData, r.pos());
  if (auto s = r.bytes(h.ciphertext_size, ct); !s)
    return s;
  if (auto s = r.bytes(kTagBytes, tag); !s)
    return s;
  h.type = static_cast<RecordType>(type);
  out = {h, ct, tag};
  return {};
}

} // namespace cr::secure_channel::v2
