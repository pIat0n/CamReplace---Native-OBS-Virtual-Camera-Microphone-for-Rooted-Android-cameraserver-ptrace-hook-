#pragma once

// OBS-Studio source. Hosts a local RTMP listener, forwards the incoming
// H.264 stream to cr_feed_proc on the phone, and exposes a little bit of
// state for the UI to render (connection status, current stream meta).
// Unlike VideoSource, we don't decode or re-encode anything on PC — OBS
// already hands us compressed NAL units from its own (hardware) encoder
// and we just repackage them for the phone-side tcp_h264 transport.

#include "device/DeployStatus.h"
#include "render/H264PreviewDecoder.h"
#include "source/AudioPump.h"
#include "source/RtmpServer.h"
#include "transport/TcpFeedClient.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace cr::source
{

// Wire format used between this source and cr_feed_proc on the phone.
// Snapshotted at start() time — UI mutex prevents transport switching
// while a replace mode is active, so the chosen mode is stable for
// the lifetime of one ObsRtmpSource session.
enum class WireMode
{
  H264, // forward Annex-B NALUs as-is (cr_feed_proc tcp_h264_feed.cpp)
  Nv21, // decode on PC, forward NV21 frames (cr_feed_proc tcp_nv21_feed.cpp)
};

class ObsRtmpSource
{
public:
  struct Config
  {
    int adb_port = 8901;
    int rtmp_port = 1935;
    std::string stream_key = "cr"; // OBS stream key (for UX)
    // false → audio-only mode: skip the phone-side video forward
    // entirely. RTMP listener still binds on rtmp_port so OBS can
    // push, AAC frames still feed AudioPump, but video NALUs only
    // go to the local preview decoder — never to cr_feed_proc on
    // the phone (because nothing's listening on adb_port and we
    // do NOT want camhook substituting video when the user only
    // asked for sound replacement).
    bool video_forward = true;
    // What to send over the phone-side video tunnel. Compressed
    // (default) sends H.264 NALUs; Raw decodes on PC via
    // H264PreviewDecoder and ships NV21 instead. Both speak to the
    // same adb-forwarded port — the phone-side scheme picks the
    // matching cr_feed_proc receiver.
    WireMode wire_mode = WireMode::H264;
    // Local preview decode can be enabled before phone forwarding is
    // ready. Camera/photo starts use this to show OBS immediately while
    // Android arm_video is still coming up; sound-only keeps it false.
    bool preview_decode = false;
  };

  bool start(std::string serial, const Config& cfg, cr::device::DeployCallback cb);
  void stop();
  bool running() const noexcept
  {
    return running_.load();
  }

  // Toggle phone-side video forwarding at runtime. Camera replace and
  // photo replace turn this on (open tx_ to cr_feed_proc on tcp:8901);
  // sound-only mode keeps it off so a previously-injected camhook
  // starves on stale shm. Idempotent — calling with the current state
  // is a no-op. Returns true on success (or no-op); false if the tx_
  // open path needed to wait and timed out.
  bool set_video_forward(bool on);
  bool video_forwarding() const noexcept
  {
    return video_forward_.load(std::memory_order_acquire);
  }
  void set_preview_decode(bool on);

  // Independent control of the audio side-channel. Caller passes the
  // adb-forwarded port for tcp_pcm and (optionally) overrides target
  // sample rate / channel count.
  bool start_audio(const AudioPump::Config& cfg, cr::device::DeployCallback cb);
  void stop_audio();
  bool audio_running() const
  {
    return audio_.running();
  }

  // UI snapshot — read only. Thread-safe (short lock).
  struct Status
  {
    bool obs_connected = false;
    int stream_w = 0;
    int stream_h = 0;
    double stream_fps = 0;
    uint64_t frames = 0;
    uint64_t keyframes = 0;
    uint64_t video_tags = 0;
    uint64_t unsupported_video_tags = 0;
    uint64_t avc_sequence_headers = 0;
    uint64_t avc_media_packets = 0;
    uint64_t avc_keyframes = 0;
    int last_video_codec_id = -1;
    int last_avc_packet_type = -1;
    int64_t last_nalu_pts_us = -1;
    std::string rtmp_url; // "rtmp://127.0.0.1:1935/live/cr"
  };
  Status status() const;

private:
  void on_meta_(const cr::transport::RtmpStreamMeta& m);
  void on_video_(const cr::transport::RtmpVideoEvent& event);
  void on_nalu_(const uint8_t* data, size_t len, int64_t pts_us, bool is_keyframe);
  void on_nv12_(const uint8_t* nv12, int w, int h, int stride);
  void on_conn_(bool connected, const std::string& detail);

  std::string serial_;
  Config cfg_;
  cr::device::DeployCallback cb_;

  cr::transport::RtmpServer rtmp_;
  cr::transport::TcpFeedClient tx_;
  mutable std::mutex tx_mu_;
  bool tx_header_sent_ = false;
  std::atomic<bool> video_forward_{true}; // mirrors Config
  std::atomic<bool> preview_decode_{false};
  WireMode wire_mode_ = WireMode::H264;
  cr::render::H264PreviewDecoder preview_dec_;
  // Scratch for NV12 → NV21 chroma swap before tx_.send_frame() in
  // Raw mode. Sized lazily on first decoded frame and protected by tx_mu_.
  std::vector<std::uint8_t> nv21_scratch_;

  // Audio side-channel. Owned here so the same RTMP listener fans the
  // FLV audio tags into PCM that goes over its own adb-forwarded TCP
  // tunnel to cr_feed_proc on tcp_pcm:listen:8902. Started/stopped
  // independently from the video tunnel via FeedController so the user
  // can replace just camera, just sound, or both.
  AudioPump audio_;

  std::atomic<bool> running_{false};

  mutable std::mutex mu_;
  Status status_;
};

} // namespace cr::source
