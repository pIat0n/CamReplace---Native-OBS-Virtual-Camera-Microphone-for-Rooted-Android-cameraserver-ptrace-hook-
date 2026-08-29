/*
 * Назначение файла:
 *   Windows TCP client для adb-forward tunnels к cr_feed_proc. Отправляет
 *   CRTP raw NV21, CRH2 H.264 chunks и CRAU PCM stream.
 *
 * ABI/совместимость:
 *   Wire headers не меняются. Video shared-memory ABI v1 на телефоне имеет
 *   fixed 1920x1080 NV21 slot cap; client валидирует dimensions до отправки
 *   header, чтобы телефон не уходил в неочевидный receiver fail.
 *
 * Ограничения:
 *   Это транспортный слой, он не масштабирует и не ресемплит. Исправление
 *   oversized stream делается выше по pipeline или через будущий shm v2.
 */

#include "transport/TcpFeedClient.h"
#include "util/Log.h"
#include "util/WinApiDyn.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <winsock2.h>
#include <ws2tcpip.h>
// Linking against ws2_32 happens via CMake (clang/MinGW ignores MSVC's
// `#pragma comment(lib,...)` directive).

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <climits>
#include <cstring>
#include <string>
#include <vector>

extern "C"
{
#include "secure_channel/vendor/monocypher/monocypher.h"
}

#ifndef NT_SUCCESS
#define NT_SUCCESS(status) (((NTSTATUS) (status)) >= 0)
#endif
namespace cr::transport
{

namespace
{

// Winsock is refcounted — first WSAStartup/last WSACleanup. We track with an
// atomic so the first TcpFeedClient constructor triggers it and it sticks
// around for the app lifetime (Windows cleans up on process exit anyway).
std::atomic<bool> g_wsa_inited{false};

void ensure_wsa()
{
  bool expected = false;
  if (g_wsa_inited.compare_exchange_strong(expected, true))
  {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
  }
}

inline SOCKET as_sock(std::uintptr_t h) noexcept
{
  return (SOCKET) h;
}

constexpr std::uint32_t kFeedMaxWidth = 1920u;
constexpr std::uint32_t kFeedMaxHeight = 1080u;
constexpr std::uint32_t kFeedMaxNv21Bytes = kFeedMaxWidth * kFeedMaxHeight * 3u / 2u;
constexpr std::uint32_t kPhoneHelloMagic = 0x324c4850u; // "PHL2"
constexpr std::size_t kPhoneHelloBytes = 64;
constexpr std::size_t kApplicationChunkBytes = 64 * 1024;
constexpr std::size_t kPcmRecordBytes = 32 * 1024;

bool video_shape_supported(int width, int height, const char* proto)
{
  if (width < 16 || height < 16)
    return false;
  const auto bytes = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 3ull / 2ull;
  if (bytes <= kFeedMaxNv21Bytes)
    return true;
  cr::log::warn("tcp", std::string(proto) + " frame " + std::to_string(width) + "x" + std::to_string(height) + " exceeds phone shm v1 cap 1920x1080");
  return false;
}

bool recv_all(SOCKET socket, void* data, std::size_t bytes) noexcept
{
  auto* p = static_cast<char*>(data);
  while (bytes)
  {
    const int n = ::recv(socket, p, static_cast<int>(std::min<std::size_t>(bytes, INT_MAX)), 0);
    if (n <= 0)
      return false;
    p += n;
    bytes -= static_cast<std::size_t>(n);
  }
  return true;
}

std::uint32_t get_le32(const std::uint8_t* p) noexcept
{
  return std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) | (std::uint32_t{p[3]} << 24);
}

std::uint64_t unix_ms() noexcept
{
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

bool fill_random(std::uint8_t* out, std::size_t bytes) noexcept
{
  return out && NT_SUCCESS(cr::winapi::BCryptGenRandom(nullptr, out, static_cast<ULONG>(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

bool make_dev_ticket(
    cr::secure_channel::v2::StreamKind stream_kind,
    const std::array<std::uint8_t, 32>& host_public,
    const std::array<std::uint8_t, 32>& phone_public,
    const std::array<std::uint8_t, 24>& phone_nonce,
    std::vector<std::uint8_t>& ticket_bytes) noexcept
{
  cr::secure_channel::v2::Ticket ticket{};
  ticket.stream = stream_kind;
  ticket.issued_at_ms = unix_ms();
  ticket.expires_at_ms = ticket.issued_at_ms + 60ull * 60ull * 1000ull;
  ticket.host_ephemeral_public = host_public;
  ticket.phone_ephemeral_public = phone_public;
  ticket.phone_nonce = phone_nonce;
  if (!fill_random(ticket.session_id.data(), ticket.session_id.size()))
    return false;
  ticket.signature.fill(0);
  return static_cast<bool>(cr::secure_channel::v2::encode_ticket(ticket, ticket_bytes));
}

} // namespace

TcpFeedClient::~TcpFeedClient()
{
  close();
}

bool TcpFeedClient::is_open() const noexcept
{
  return sock_ != ~std::uintptr_t{0};
}

bool TcpFeedClient::open(int port, cr::secure_channel::v2::StreamKind stream_kind)
{
  ensure_wsa();
  close();
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET)
  {
    cr::log::error("tcp", "socket() failed");
    return false;
  }

  // No Nagle — each frame is a single write and we want it on the wire
  // now, not after a 40ms coalesce.
  BOOL no_delay = TRUE;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));

  // Keep this buffer large enough for a typical H.264 keyframe but not
  // large enough to hide seconds of stale video when the phone decoder
  // falls behind. Low latency is more important here than smoothing a
  // temporary backlog.
  int snd_buf = 512 * 1024;
  setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&snd_buf), sizeof(snd_buf));

  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons((u_short) port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (connect(s, (sockaddr*) &addr, sizeof(addr)) != 0)
  {
    int err = WSAGetLastError();
    cr::log::warn("tcp", "connect to :" + std::to_string(port) + " failed err=" + std::to_string(err));
    closesocket(s);
    return false;
  }

  sock_ = (std::uintptr_t) s;
  std::array<std::uint8_t, kPhoneHelloBytes> hello{};
  if (!recv_all(s, hello.data(), hello.size()) || get_le32(hello.data()) != kPhoneHelloMagic || hello[4] != cr::secure_channel::v2::kVersion || hello[5] != static_cast<std::uint8_t>(stream_kind) || hello[6] != 0 || hello[7] != 0)
  {
    cr::log::warn("tcp", "secure-channel phone hello rejected");
    close();
    return false;
  }

  std::array<std::uint8_t, 32> phone_public{};
  std::array<std::uint8_t, 24> phone_nonce{};
  std::memcpy(phone_public.data(), hello.data() + 8, phone_public.size());
  std::memcpy(phone_nonce.data(), hello.data() + 40, phone_nonce.size());
  std::array<std::uint8_t, 32> host_private{};
  if (!NT_SUCCESS(cr::winapi::BCryptGenRandom(nullptr, host_private.data(), static_cast<ULONG>(host_private.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
  {
    cr::log::error("tcp", "host X25519 private key generation failed");
    close();
    return false;
  }
  std::array<std::uint8_t, 32> host_public{};
  crypto_x25519_public_key(host_public.data(), host_private.data());

  std::vector<std::uint8_t> dev_ticket;
  if (!make_dev_ticket(stream_kind, host_public, phone_public, phone_nonce, dev_ticket))
  {
    cr::log::error("tcp", "local SCT2 session descriptor creation failed");
    crypto_wipe(host_private.data(), host_private.size());
    close();
    return false;
  }
  if (!secure_.initialize(cr::secure_channel::v2::Role::Host,
                          {host_private.data(), host_private.size()},
                          {dev_ticket.data(), dev_ticket.size()},
                          stream_kind,
                          unix_ms()))
  {
    cr::log::error("tcp", "local secure session init failed");
    crypto_wipe(host_private.data(), host_private.size());
    close();
    return false;
  }
  if (!send_all_(dev_ticket.data(), dev_ticket.size()))
  {
    cr::log::warn("tcp", "local SCT2 session descriptor send failed");
    crypto_wipe(host_private.data(), host_private.size());
    close();
    return false;
  }
  cr::log::info("tcp", "local SCT2 session established");
  crypto_wipe(host_private.data(), host_private.size());

  std::vector<std::uint8_t> finished;
  if (!secure_.make_finished(finished) || !send_all_(finished.data(), finished.size()))
  {
    cr::log::warn("tcp", "secure-channel local finished send failed");
    close();
    return false;
  }
  std::array<std::uint8_t, cr::secure_channel::v2::kRecordHeaderBytes> header{};
  if (!recv_all(s, header.data(), header.size()))
  {
    cr::log::warn("tcp", "secure-channel peer finished header read failed");
    close();
    return false;
  }
  const std::uint32_t ciphertext_size = get_le32(header.data() + 32);
  if (ciphertext_size > cr::secure_channel::v2::kMaxCiphertextBytes)
  {
    cr::log::warn("tcp", "secure-channel peer finished record too large");
    close();
    return false;
  }
  std::vector<std::uint8_t> peer_record(header.begin(), header.end());
  peer_record.resize(peer_record.size() + ciphertext_size + cr::secure_channel::v2::kTagBytes);
  if (!recv_all(s, peer_record.data() + header.size(), peer_record.size() - header.size()))
  {
    cr::log::warn("tcp", "secure-channel peer finished body read failed");
    close();
    return false;
  }
  std::vector<std::uint8_t> ignored;
  cr::secure_channel::v2::RecordType type{};
  if (!secure_.open({peer_record.data(), peer_record.size()}, ignored, type) || type != cr::secure_channel::v2::RecordType::Finished || !secure_.established())
  {
    cr::log::warn("tcp", "secure-channel finished verification failed");
    close();
    return false;
  }
  cr::log::info("tcp", "secure channel established to 127.0.0.1:" + std::to_string(port));
  return true;
}

void TcpFeedClient::close()
{
  if (is_open())
  {
    shutdown(as_sock(sock_), SD_BOTH);
    closesocket(as_sock(sock_));
    sock_ = ~std::uintptr_t{0};
  }
  secure_.wipe();
  pcm_pending_.clear();
  pcm_offset_ = 0;
}

bool TcpFeedClient::send_all_(const void* data, std::size_t bytes) noexcept
{
  if (!is_open() || (!data && bytes))
    return false;
  auto* p = static_cast<const char*>(data);
  while (bytes)
  {
    const int sent = ::send(as_sock(sock_), p, static_cast<int>(std::min<std::size_t>(bytes, INT_MAX)), 0);
    if (sent <= 0)
      return false;
    p += sent;
    bytes -= static_cast<std::size_t>(sent);
  }
  return true;
}

bool TcpFeedClient::send_application_(const void* data, std::size_t bytes)
{
  if (!is_open() || (!data && bytes))
    return false;
  const auto* p = static_cast<const std::uint8_t*>(data);
  if (!secure_.established())
    return false;
  while (bytes)
  {
    const std::size_t chunk = std::min(bytes, kApplicationChunkBytes);
    std::vector<std::uint8_t> record;
    if (!secure_.seal(cr::secure_channel::v2::RecordType::Application, {p, chunk}, record) || !send_all_(record.data(), record.size()))
    {
      close();
      return false;
    }
    p += chunk;
    bytes -= chunk;
  }
  return true;
}

bool TcpFeedClient::flush_pcm_blocks_()
{
  while (pcm_pending_.size() - pcm_offset_ >= kPcmRecordBytes)
  {
    if (!send_application_(pcm_pending_.data() + pcm_offset_, kPcmRecordBytes))
    {
      return false;
    }
    pcm_offset_ += kPcmRecordBytes;
  }
  if (pcm_offset_ != 0 && (pcm_offset_ == pcm_pending_.size() || pcm_offset_ >= kPcmRecordBytes))
  {
    pcm_pending_.erase(pcm_pending_.begin(), pcm_pending_.begin() + static_cast<std::ptrdiff_t>(pcm_offset_));
    pcm_offset_ = 0;
  }
  return true;
}

bool TcpFeedClient::send_header(int width, int height, int fps)
{
  if (!is_open())
    return false;
  if (!video_shape_supported(width, height, "CRTP"))
    return false;
  std::uint32_t hdr[4] = {
      kMagic,
      (std::uint32_t) width,
      (std::uint32_t) height,
      (std::uint32_t) fps,
  };
  return send_application_(hdr, sizeof(hdr));
}

bool TcpFeedClient::send_frame(const void* data, std::size_t bytes)
{
  if (!is_open())
    return false;
  return send_application_(data, bytes);
}

// --- H.264 protocol -----------------------------------------------------

bool TcpFeedClient::send_h264_header(int width, int height, int fps)
{
  if (!is_open())
    return false;
  if (!video_shape_supported(width, height, "CRH2"))
    return false;
  std::uint32_t hdr[4] = {
      kMagicH264,
      (std::uint32_t) width,
      (std::uint32_t) height,
      (std::uint32_t) fps,
  };
  return send_application_(hdr, sizeof(hdr));
}

bool TcpFeedClient::send_h264_chunk(const void* data, std::size_t bytes, int64_t pts_us, bool is_keyframe)
{
  if (!is_open() || !data)
    return false;

  // Prefix: [u32 len][u32 flags][i64 pts_us].
  struct
  {
    std::uint32_t len;
    std::uint32_t flags;
    std::int64_t pts_us;
  } hdr = {
      (std::uint32_t) bytes,
      is_keyframe ? 1u : 0u,
      pts_us,
  };

  return send_application_(&hdr, sizeof(hdr)) && send_application_(data, bytes);
}

// --- PCM audio protocol --------------------------------------------------

bool TcpFeedClient::send_pcm_header(uint32_t sample_rate, uint16_t channels, uint16_t bytes_per_sample)
{
  if (!is_open())
    return false;
  // Same 12-byte header tcp_pcm_feed.cpp parses on the phone.
  struct __attribute__((packed)) WireHdr
  {
    std::uint32_t magic;
    std::uint32_t sample_rate;
    std::uint16_t channels;
    std::uint16_t bps;
  } hdr = {kMagicPcm, sample_rate, channels, bytes_per_sample};
  pcm_pending_.clear();
  pcm_offset_ = 0;
  return send_application_(&hdr, sizeof(hdr));
}

bool TcpFeedClient::send_pcm(const void* data, std::size_t bytes)
{
  if (!data || bytes == 0)
    return false;
  const auto* p = static_cast<const std::uint8_t*>(data);
  pcm_pending_.insert(pcm_pending_.end(), p, p + bytes);
  return flush_pcm_blocks_();
}

} // namespace cr::transport
