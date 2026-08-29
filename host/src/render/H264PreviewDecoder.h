#pragma once

// Live H.264 → BGRA decoder for the OBS preview pane.
// OBS hands the host pre-encoded H.264 NAL units over RTMP and we forward
// them straight to the phone — there's no decoded copy anywhere in the
// streaming path. This helper plugs into ObsRtmpSource as a side-channel:
// every Annex-B NAL unit is queued to a worker thread that drives a Media
// Foundation H.264 decoder MFT (software, system memory). Decoded NV12
// frames are converted to BGRA and pushed to LivePreview so the user can
// see what OBS is publishing.
// Threading: feed() is called from the RTMP server worker thread and just
// pushes a copy of the NALU into a queue (cheap). All MF work and the
// NV12→BGRA conversion happen on a dedicated decoder thread so they
// never block the network forwarding path. set_enabled(false) makes
// feed() a no-op so the user can pause the preview to save CPU.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

struct IMFTransform;

namespace cr::render
{

class H264PreviewDecoder
{
public:
  using PublishFn = std::function<void(const uint8_t* bgra, int w, int h, int pitch)>;

  // Pre-conversion NV12 tap. Same decoded output as the BGRA path —
  // gets called from the same drain() loop right before NV12→BGRA.
  // Used by Transport::Raw to ship NV21 (NV12 with swapped chroma)
  // straight to the phone without paying for a second decode there.
  // Pitch (stride) may exceed `w` on some MFTs.
  using Nv12PublishFn = std::function<void(const uint8_t* nv12, int w, int h, int stride)>;

  // Stash the publish callback and start the decoder worker thread. The
  // MFT itself is created lazily on the worker (some MFTs are picky
  // about cross-thread activation). `nv12_publish` is optional — pass
  // nullptr (or use the single-arg overload) if no raw tap is needed.
  void arm(PublishFn publish, Nv12PublishFn nv12_publish = {});

  // Stop the worker, tear down the MFT, release MF. Idempotent.
  void stop();

  // Submit one Annex-B access unit. Returns immediately — the actual
  // decode happens on the worker thread.
  void feed(const uint8_t* annexb, size_t len, int64_t pts_us);

  // Global on/off switch shared across all instances. UI flips this from
  // the preview toggle button. While disabled, feed() drops input and
  // the decoder sits idle (no CPU spent on conversion or color convert).
  static void set_enabled(bool e);
  static bool is_enabled();
  static bool start_failed();

  ~H264PreviewDecoder()
  {
    stop();
  }

private:
  void worker_loop_();
  bool ensure_started_();
  bool reconfigure_output_();
  void process_nalu_(const uint8_t* annexb, size_t len, int64_t pts_us);
  void drain_();
  void teardown_();

  PublishFn publish_;
  Nv12PublishFn nv12_publish_;
  IMFTransform* dec_ = nullptr;
  bool mf_init_ = false;
  bool start_failed_ = false;
  int cur_w_ = 0;
  int cur_h_ = 0;
  int cur_stride_ = 0;
  std::vector<uint8_t> bgra_;

  struct NaluPacket
  {
    std::vector<uint8_t> data;
    int64_t pts_us = 0;
  };

  std::mutex q_mu_;
  std::condition_variable q_cv_;
  std::deque<NaluPacket> q_;
  std::atomic<bool> stopping_{false};
  std::thread worker_;

  // Soft cap on outstanding NALUs. If the decoder can't keep up we drop
  // the OLDEST queued NALU rather than letting memory grow unbounded;
  // the resulting glitch heals at the next IDR (≤ a couple of seconds
  // for a typical OBS GOP).
  static constexpr std::size_t kMaxQueue = 90;

  static std::atomic<bool> g_enabled_;
  static std::atomic<bool> g_start_failed_;
};

} // namespace cr::render
