#pragma once

// Minimal RTMP 1.0 publisher server — just enough of the protocol to let
// OBS (or any other RTMP-ingest client) stream to us on localhost and
// hand us the raw H.264 NAL units. Audio, goodput ACKs, gotoLive, and
// every "digest" variation of the handshake are intentionally out of
// scope. We accept one publisher at a time; subsequent connections wait
// on accept() until the current session ends.
// Exposed contract:
//   start(port, on_meta, on_nalu) — spawns a background accept loop.
//   stop() — graceful teardown; safe to call on dtor.
//   All callbacks fire on the server's worker thread.

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cr::transport
{

struct RtmpStreamMeta
{
  int width = 0; // advertised by OBS via @setDataFrame
  int height = 0;
  double framerate = 0; // may be float (e.g. 59.94)
  int bitrate_kbps = 0; // hint only; actual bits may differ
};

struct RtmpVideoEvent
{
  uint8_t frame_type = 0;
  uint8_t codec_id = 0;
  int avc_packet_type = -1;
  size_t payload_bytes = 0;
  bool keyframe = false;
};

class RtmpServer
{
public:
  using MetaCb = std::function<void(const RtmpStreamMeta&)>;
  // Called for every complete H.264 NAL access unit (Annex-B form, with
  // 00 00 00 01 start codes). `is_keyframe` is true for IDR pictures so
  // the consumer can flag the matching TCP chunk.
  using NaluCb = std::function<void(const uint8_t* data, size_t len, int64_t pts_us, bool is_keyframe)>;
  // Audio side-channel.
  //   `csd` is the AAC AudioSpecificConfig blob (2-7 bytes), captured
  //         once at session start. Tells the decoder sample rate /
  //         channel count / object type. Consumers can ignore frames
  //         until they see csd at least once.
  //   Each subsequent call passes one raw AAC access unit (no ADTS
  //         header). pts_us is monotonic from session start.
  using AacCsdCb = std::function<void(const uint8_t* csd, size_t len)>;
  using AacFrameCb = std::function<void(const uint8_t* aac, size_t len, int64_t pts_us)>;
  // Fires when OBS connects / disconnects. Useful for UI status text.
  using ConnCb = std::function<void(bool connected, const std::string& detail)>;
  using VideoCb = std::function<void(const RtmpVideoEvent& event)>;

  bool start(int port, MetaCb on_meta, NaluCb on_nalu, ConnCb on_conn, VideoCb on_video = {}, AacCsdCb on_aac_csd = {}, AacFrameCb on_aac_frame = {});
  void stop();
  bool running() const noexcept
  {
    return running_.load();
  }

  ~RtmpServer();

private:
  void accept_loop_();
  bool serve_client_(int client);

  std::atomic<bool> running_{false};
  std::atomic<bool> stop_flag_{false};
  std::thread worker_;

  MetaCb on_meta_;
  NaluCb on_nalu_;
  ConnCb on_conn_;
  VideoCb on_video_;
  AacCsdCb on_aac_csd_;
  AacFrameCb on_aac_frame_;

  int port_ = 0;
  // Listen socket exposed to stop() so it can shutdown() the fd and
  // break accept() on the worker thread.
  std::uintptr_t srv_sock_ = ~std::uintptr_t{0};
  std::mutex mu_;
};

} // namespace cr::transport
