#pragma once

// Phone-side AAC → S16LE PCM → /data/cr/audio pipe (sprint B follow-up).
// Lives next to H264DecodePipe for the legacy opt-in rtmp_h264_feed.cpp
// path: when an FLV audio tag (type 0x08) arrives, the RTMP server
// hands the AAC payload to AacDecodePipe::push(); when an AAC sequence
// header (AACPacketType=0) arrives we configure the decoder from the
// AudioSpecificConfig and arm the audio shm.
// The output rate matches the AAC stream natively (typically 44100 or
// 48000, stereo). libcr_audhook reads the shm without resampling, so
// whatever rate we publish is what apps see.
// Why this exists in direct-RTMP mode:
//   In legacy mode the PC's RtmpServer demuxed audio out of the FLV
//   stream and fed it to AudioPump (Media Foundation AAC decoder), which
//   shipped raw PCM to a separate cr_feed_proc instance running with
//   tcp_pcm:listen:8902. With direct-RTMP the AAC is now arriving on
//   the phone — there's no PC-side decode any more. So we decode here.

#include "audio_shm.h"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <android/log.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <vector>
#include "util/Obf.h"

#define CR_AUDIO_LOGI(...) __android_log_print(ANDROID_LOG_INFO, OBF("cr_feed").c_str(), __VA_ARGS__)
#define CR_AUDIO_LOGW(...) __android_log_print(ANDROID_LOG_WARN, OBF("cr_feed").c_str(), __VA_ARGS__)
#define CR_AUDIO_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, OBF("cr_feed").c_str(), __VA_ARGS__)

namespace cr_feed
{

class AacDecodePipe
{
public:
  // Open + ftruncate /data/cr/audio so the shm exists (we don't need
  // CSD yet at this point — header is rewritten on each csd push).
  bool open_shm()
  {
    if (audio_fd_ >= 0)
      return true;
    mkdir(OBF("/data/cr").c_str(), 0755);
    const size_t want = sizeof(cr_audio_header) + CR_AUDIO_RING_BYTES;
    int fd = ::open(OBF("/data/cr/audio").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0664);
    if (fd < 0)
    {
      CR_AUDIO_LOGE(OBF("aac: open %s: %s").c_str(), OBF("/data/cr/audio").c_str(), strerror(errno));
      return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0)
    {
      ::close(fd);
      return false;
    }
    if ((size_t) st.st_size < want)
    {
      if (ftruncate(fd, (off_t) want) != 0)
      {
        CR_AUDIO_LOGE(OBF("aac: ftruncate %s: %s").c_str(), OBF("/data/cr/audio").c_str(), strerror(errno));
        ::close(fd);
        return false;
      }
    }
    void* m = mmap(nullptr, want, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED)
    {
      CR_AUDIO_LOGE(OBF("aac: mmap: %s").c_str(), strerror(errno));
      ::close(fd);
      return false;
    }
    audio_hdr_ = (cr_audio_header*) m;
    audio_ring_ = (uint8_t*) m + sizeof(cr_audio_header);
    audio_map_ = m;
    audio_size_ = want;
    audio_fd_ = fd;
    return true;
  }

  // Configure the AAC decoder from the AudioSpecificConfig blob (FLV
  // AAC packet type 0). Replaces any prior decoder so a stream
  // restart with a different format works without leaks.
  bool on_csd(const uint8_t* csd, size_t len)
  {
    if (!csd || len < 2)
      return false;

    // Parse AudioSpecificConfig: object_type (5b), sr_idx (4b),
    // channel_cfg (4b). Same lookup table the PC AudioPump uses.
    const uint8_t b0 = csd[0];
    const uint8_t b1 = csd[1];
    const int sr_idx = ((b0 & 0x07) << 1) | (b1 >> 7);
    const int channels = (b1 >> 3) & 0x0f;
    static const int kSrTable[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350, 0, 0, 0};
    const int sample_rate = (sr_idx >= 0 && sr_idx < 13) ? kSrTable[sr_idx] : 0;
    if (sample_rate <= 0 || channels < 1 || channels > 2)
    {
      CR_AUDIO_LOGW(OBF("aac: bad csd sr_idx=%d ch=%d").c_str(), sr_idx, channels);
      return false;
    }

    teardown_decoder();

    dec_ = AMediaCodec_createDecoderByType(OBF("audio/mp4a-latm").c_str());
    if (!dec_)
    {
      CR_AUDIO_LOGE(OBF("aac: createDecoderByType failed").c_str());
      return false;
    }

    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, OBF("audio/mp4a-latm").c_str());
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_SAMPLE_RATE, sample_rate);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_CHANNEL_COUNT, channels);
    AMediaFormat_setBuffer(fmt, "csd-0", csd, len);

    media_status_t cs = AMediaCodec_configure(dec_, fmt, nullptr, nullptr, 0);
    AMediaFormat_delete(fmt);
    if (cs != AMEDIA_OK)
    {
      CR_AUDIO_LOGE(OBF("aac: configure failed status=%d").c_str(), (int) cs);
      AMediaCodec_delete(dec_);
      dec_ = nullptr;
      return false;
    }
    media_status_t ss = AMediaCodec_start(dec_);
    if (ss != AMEDIA_OK)
    {
      CR_AUDIO_LOGE(OBF("aac: start failed status=%d").c_str(), (int) ss);
      AMediaCodec_delete(dec_);
      dec_ = nullptr;
      return false;
    }

    sample_rate_ = (uint32_t) sample_rate;
    channels_ = (uint16_t) channels;

    if (!open_shm())
      return false;
    publish_audio_header();
    CR_AUDIO_LOGI(OBF("aac: pipe started sr=%u ch=%u").c_str(), sample_rate_, (unsigned) channels_);
    ready_ = true;
    return true;
  }

  // Feed one AAC raw access unit (FLV AAC packet type 1).
  void push(const uint8_t* aac, size_t len, int64_t pts_us)
  {
    if (!ready_ || !dec_ || !aac || !len)
      return;

    ssize_t in_idx = AMediaCodec_dequeueInputBuffer(dec_, 5'000);
    if (in_idx >= 0)
    {
      size_t cap = 0;
      uint8_t* in_buf = AMediaCodec_getInputBuffer(dec_, (size_t) in_idx, &cap);
      if (in_buf && cap >= len)
      {
        std::memcpy(in_buf, aac, len);
        AMediaCodec_queueInputBuffer(dec_, (size_t) in_idx, 0, len, (uint64_t) pts_us, 0);
      }
    }
    drain();
  }

  void drain()
  {
    if (!dec_)
      return;
    for (;;)
    {
      AMediaCodecBufferInfo info;
      ssize_t out_idx = AMediaCodec_dequeueOutputBuffer(dec_, &info, 0);
      if (out_idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
        break;
      if (out_idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
      {
        // Audio format changes on first output; refresh sr/ch
        // from the codec's actual output and republish header.
        AMediaFormat* of = AMediaCodec_getOutputFormat(dec_);
        int32_t sr = sample_rate_, ch = channels_;
        AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_SAMPLE_RATE, &sr);
        AMediaFormat_getInt32(of, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &ch);
        AMediaFormat_delete(of);
        if (sr > 0)
          sample_rate_ = (uint32_t) sr;
        if (ch == 1 || ch == 2)
          channels_ = (uint16_t) ch;
        publish_audio_header();
        continue;
      }
      if (out_idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
        continue;
      if (out_idx < 0)
        break;

      if (info.size > 0 && audio_hdr_)
      {
        size_t cap = 0;
        uint8_t* out_buf = AMediaCodec_getOutputBuffer(dec_, (size_t) out_idx, &cap);
        write_to_ring(out_buf, (size_t) info.size);
      }
      AMediaCodec_releaseOutputBuffer(dec_, (size_t) out_idx, false);
    }
  }

  void stop()
  {
    teardown_decoder();
    if (audio_hdr_)
    {
      // Zero magic so the audhook falls through to real mic.
      audio_hdr_->magic = 0;
      __atomic_thread_fence(__ATOMIC_RELEASE);
      munmap(audio_map_, audio_size_);
      audio_hdr_ = nullptr;
      audio_map_ = nullptr;
      audio_ring_ = nullptr;
    }
    if (audio_fd_ >= 0)
    {
      ::close(audio_fd_);
      audio_fd_ = -1;
    }
    ready_ = false;
  }

  ~AacDecodePipe()
  {
    stop();
  }

private:
  void teardown_decoder()
  {
    if (dec_)
    {
      AMediaCodec_stop(dec_);
      AMediaCodec_delete(dec_);
      dec_ = nullptr;
    }
  }

  void publish_audio_header()
  {
    if (!audio_hdr_)
      return;
    auto* h = audio_hdr_;
    // Same magic-zero / write / fence / magic-restore dance the
    // legacy tcp_pcm path uses, so a partial header rewrite never
    // tricks the audhook into reading half-old fields.
    h->magic = 0;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    h->version = CR_AUDIO_VERSION;
    h->sample_rate = sample_rate_;
    h->channels = channels_;
    h->bytes_per_sample = 2;
    h->ring_bytes = CR_AUDIO_RING_BYTES;
    atomic_store(&h->write_pos, 0ull);
    atomic_fetch_add(&h->generation, 1u);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    h->magic = CR_AUDIO_MAGIC;
  }

  void write_to_ring(const uint8_t* pcm, size_t bytes)
  {
    if (!audio_hdr_ || !audio_ring_ || !pcm || !bytes)
      return;
    const uint32_t ring = audio_hdr_->ring_bytes;
    const uint64_t wp = atomic_load(&audio_hdr_->write_pos);
    const uint32_t off = (uint32_t) (wp % ring);
    const size_t span1 = (off + bytes <= ring) ? bytes : (ring - off);
    std::memcpy(audio_ring_ + off, pcm, span1);
    if (span1 < bytes)
    {
      std::memcpy(audio_ring_, pcm + span1, bytes - span1);
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    atomic_store(&audio_hdr_->write_pos, wp + bytes);
  }

  AMediaCodec* dec_ = nullptr;
  bool ready_ = false;
  uint32_t sample_rate_ = 0;
  uint16_t channels_ = 0;

  // Owned audio shm.
  int audio_fd_ = -1;
  void* audio_map_ = nullptr;
  size_t audio_size_ = 0;
  cr_audio_header* audio_hdr_ = nullptr;
  uint8_t* audio_ring_ = nullptr;
};

} // namespace cr_feed
