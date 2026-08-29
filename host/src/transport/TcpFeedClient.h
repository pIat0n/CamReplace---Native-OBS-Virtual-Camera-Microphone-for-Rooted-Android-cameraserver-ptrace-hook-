#pragma once

// TCP streaming client — sends NV21 frames from the PC to cr_feed_proc
// running on the phone, via `adb forward tcp:PORT tcp:PORT`.
// Wire format matches android/feed/src/tcp_feed.cpp:
//   header:     uint32 'CRTP', uint32 width, uint32 height, uint32 fps
//   per frame:  width*height*3/2 bytes of NV21
// Typical usage:
//   TcpFeedClient c;
//   c.open(8901);
//   c.send_header(1280, 720, 30);
//   for each frame : c.send_frame(nv21.data(), nv21.size());
//   c.close();

#include <cstddef>
#include <cstdint>
#include <vector>

#include "secure_channel/SecureSession.h"

namespace cr::transport
{

class TcpFeedClient
{
public:
  // Magic constants matching the Android receivers; exposed so tests can
  // validate headers without peeking into the private cpp.
  //   kMagic      — raw NV21 stream (tcp_feed.cpp)
  //   kMagicH264  — H.264 Annex-B stream (tcp_h264_feed.cpp)
  static constexpr uint32_t kMagic = 0x50545243u;     // 'CRTP' LE
  static constexpr uint32_t kMagicH264 = 0x32484243u; // 'CRH2' LE
  static constexpr uint32_t kMagicPcm = 0x55415243u;  // 'CRAU' LE — PCM stream

  // Connect to 127.0.0.1:port. Assumes `adb forward tcp:port tcp:port`
  // has already been set up by the caller. Returns true on success.
  bool open(int port, cr::secure_channel::v2::StreamKind stream_kind);

  // --- raw NV21 protocol ------------------------------------------------
  // Send the NV21 stream header. Call exactly once after open().
  bool send_header(int width, int height, int fps);
  // Send one NV21 frame. `bytes` must equal width*height*3/2.
  bool send_frame(const void* data, std::size_t bytes);

  // --- H.264 Annex-B protocol ------------------------------------------
  // Stream header (magic CRH2, width, height, fps). Once per session.
  bool send_h264_header(int width, int height, int fps);
  // One encoded chunk:  [u32 len][u32 flags][i64 pts_us][len bytes]
  //   flags bit 0 → keyframe
  bool send_h264_chunk(const void* data, std::size_t bytes, int64_t pts_us, bool is_keyframe);

  // --- PCM audio protocol ----------------------------------------------
  // Header (once per session): magic CRAU, sample_rate, channels, bps.
  // Then a continuous stream of S16LE samples (no per-frame framing) —
  // the phone receiver memcpy's bytes straight into a ring buffer.
  bool send_pcm_header(uint32_t sample_rate, uint16_t channels, uint16_t bytes_per_sample);
  bool send_pcm(const void* data, std::size_t bytes);

  void close();
  bool is_open() const noexcept;

  ~TcpFeedClient();

private:
  bool send_application_(const void* data, std::size_t bytes);
  bool flush_pcm_blocks_();
  bool send_all_(const void* data, std::size_t bytes) noexcept;

  // SOCKET is an unsigned pointer-sized handle on Windows; we store it as
  // uintptr_t to avoid dragging <winsock2.h> into the public header.
  std::uintptr_t sock_ = ~std::uintptr_t{0};
  cr::secure_channel::v2::SecureSession secure_{};
  std::vector<std::uint8_t> pcm_pending_{};
  std::size_t pcm_offset_{0};
};

} // namespace cr::transport
