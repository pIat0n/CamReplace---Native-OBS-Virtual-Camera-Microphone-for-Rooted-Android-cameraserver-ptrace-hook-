#pragma once

// AudioPump — OBS audio side-channel.
// OBS publishes a single FLV stream that carries video AND audio. The video
// half is parsed by ObsRtmpSource and forwarded as H.264 NALUs to the
// phone. AudioPump handles the audio half: it receives raw AAC frames +
// the AudioSpecificConfig CSD blob from RtmpServer, decodes them into
// 16-bit signed PCM via Media Foundation, optionally resamples when the
// caller requests a concrete output rate/channel count, and ships the bytes
// over an adb-forwarded
// TCP tunnel to cr_feed_proc on the phone (which writes them into the
// /data/cr/audio shm that libcr_audhook reads on every mic poll).
// Lifetime: owned by FeedController for the duration of one
// "Start replace sound" session. Thread-safety: feed_*() may be called
// from the RtmpServer worker; start/stop are called from the UI thread.

#include "device/DeployStatus.h"
#include "transport/TcpFeedClient.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct IMFTransform;

namespace cr::source
{

class AudioPump
{
public:
  struct Config
  {
    int adb_port = 8902; // matches tcp_pcm:listen:8902 on phone
    int target_sr = 0;   // 0 = keep decoder-native sample rate
    int target_ch = 0;   // 0 = keep decoder-native channel count
  };

  bool start(std::string serial, const Config& cfg, cr::device::DeployCallback cb);
  void stop();
  bool running() const noexcept
  {
    return running_.load();
  }

  // Called once per session by RtmpServer when the AAC AudioSpecificConfig
  // arrives (FLV AAC packet type 0). Resets the decoder + sends the PCM
  // header to the phone.
  void on_aac_csd(const uint8_t* csd, size_t len);

  // Called for every raw AAC access unit (FLV AAC packet type 1).
  void on_aac_frame(const uint8_t* aac, size_t len, int64_t pts_us);

  ~AudioPump()
  {
    stop();
  }

private:
  bool ensure_decoder_(const uint8_t* csd, size_t len);
  bool ensure_resampler_();
  void teardown_decoder_();
  void teardown_resampler_();
  void process_(const uint8_t* aac, size_t len, int64_t pts_us);
  bool send_pcm_header_();
  // Push raw PCM at decoder rate/channels through the resampler MFT,
  // when needed, then ship the output to the phone. Returns true on
  // any successful send.
  bool resample_and_send_(const uint8_t* pcm, size_t bytes);

  Config cfg_{};
  std::string serial_;
  cr::device::DeployCallback cb_;
  cr::transport::TcpFeedClient tx_;

  std::mutex mu_;
  IMFTransform* dec_ = nullptr;
  IMFTransform* rs_ = nullptr; // Resampler MFT
  bool mf_init_ = false;
  bool csd_seen_ = false;
  bool tx_header_sent_ = false;
  int dec_sr_ = 0;
  int dec_ch_ = 0;
  int out_sr_ = 0;
  int out_ch_ = 0;

  std::atomic<bool> running_{false};
  std::atomic<uint64_t> bytes_sent_{0};
};

} // namespace cr::source
