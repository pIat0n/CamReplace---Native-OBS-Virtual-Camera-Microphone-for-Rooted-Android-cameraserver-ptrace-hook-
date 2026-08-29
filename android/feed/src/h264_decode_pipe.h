/*
 * Назначение файла:
 *   Общий H.264 -> NV21 -> /data/cr/feed decode pipeline для phone-side feed
 *   variants. Транспортный слой отдаёт Annex-B NAL units, а этот wrapper
 *   выбирает Android decoder, конвертирует output format и публикует NV21 в
 *   shared memory v1.
 *
 * Hook point и ABI assumptions:
 *   ABI /data/cr/feed не меняется: cr_feed_header + fixed NV21 slots. Этот
 *   файл не экспортирует C symbols; он header-only helper для feed process.
 *   Decoder выбирается через AMediaCodec_createDecoderByType("video/avc")
 *   как primary path, vendor names остаются только fallback.
 *
 * Ограничения Pixel/Android:
 *   Tensor Pixel не гарантирует Qualcomm codec names. Размер кадра должен
 *   укладываться в текущий shm v1 cap 1920x1080 NV21; расширение требует
 *   отдельной версии shared memory.
 *
 * Подтверждено:
 *   Нормальный путь использует Android media codec selection по MIME type.
 *   Qualcomm names diagnostic/fallback-only для старых vendor builds.
 *
 * Diagnostic-only:
 *   codec_name() нужен только для логов и не является контрактом выбора codec.
 */

#pragma once

// Shared H.264 → NV21 → /data/cr/feed pipeline.
// Both the TCP-frame transport (tcp_h264_feed.cpp) and the legacy opt-in
// direct-RTMP listener (rtmp_h264_feed.cpp) feed the same decoder/SHM
// path: identical AMediaCodec setup, identical NV12→NV21 swap, identical
// triple-buffered shm publishing.
// This header exposes a tiny C++ API that wraps that pipeline so the
// transport-specific layer only has to:
//   1. Construct an H264DecodePipe with width/height (from the wire header
//      or a probed SPS/PPS).
//   2. Push() each Annex-B NAL unit (with PTS in microseconds, plus
//      a key-frame flag).
//   3. Call Stop() on shutdown.
// Both transports already shared 90% of their per-packet logic; pulling it
// here avoids divergence (e.g. someone fixing a colour-space bug in only
// one path).

#include "../include/cr_feed.h"

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

#include <atomic>
#include <string>
#include <vector>

#include "shm.h"
#include "util/Obf.h"

#define CR_FEED_LOGI(...) __android_log_print(ANDROID_LOG_INFO, OBF("cr_feed").c_str(), __VA_ARGS__)
#define CR_FEED_LOGW(...) __android_log_print(ANDROID_LOG_WARN, OBF("cr_feed").c_str(), __VA_ARGS__)
#define CR_FEED_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, OBF("cr_feed").c_str(), __VA_ARGS__)

namespace cr_feed
{

// /data/cr/feed writer. Identical to the inline ShmOut that lived in
// tcp_h264_feed.cpp pre-extraction.
struct ShmOut
{
  int fd = -1;
  void* map = nullptr;
  size_t mapped = 0;
  uint8_t* slot0 = nullptr;
  int w = 0;
  int h = 0;
  size_t slot_bytes = 0;
  uint32_t cur_wi = 0;

  bool init(int ww, int hh)
  {
    mkdir(OBF("/data/cr").c_str(), 0755);
    slot_bytes = (size_t) ww * hh * 3 / 2;
    if (slot_bytes > CR_FEED_MAX_NV21_BYTES)
    {
      CR_FEED_LOGE(OBF("h264 pipe shm init rejected %dx%d: %zu > %u (%ux%u cap)").c_str(), ww, hh, slot_bytes, CR_FEED_MAX_NV21_BYTES, CR_FEED_MAX_WIDTH, CR_FEED_MAX_HEIGHT);
      return false;
    }
    const size_t reserve = sizeof(cr_feed_header) + (size_t) CR_FEED_NUM_SLOTS * CR_FEED_MAX_NV21_BYTES;

    fd = open(OBF("/data/cr/feed").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0664);
    if (fd < 0)
      return false;
    struct stat st;
    if (fstat(fd, &st) != 0)
    {
      close(fd);
      fd = -1;
      return false;
    }
    size_t map_size = reserve;
    if ((size_t) st.st_size < reserve)
    {
      if (ftruncate(fd, (off_t) reserve) != 0)
      {
        close(fd);
        fd = -1;
        return false;
      }
    }
    else
    {
      map_size = (size_t) st.st_size;
    }
    map = mmap(nullptr, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED)
    {
      close(fd);
      fd = -1;
      return false;
    }
    mapped = map_size;
    slot0 = (uint8_t*) map + sizeof(cr_feed_header);
    w = ww;
    h = hh;

    auto* hd = (cr_feed_header*) map;
    const uint32_t prev_gen = atomic_load(&hd->generation);
    hd->magic = 0u;
    __atomic_thread_fence(__ATOMIC_RELEASE);

    hd->version = CR_FEED_SHM_VERSION;
    hd->width = (uint32_t) ww;
    hd->height = (uint32_t) hh;
    hd->stride = (uint32_t) ww;
    hd->slot_size = (uint32_t) slot_bytes;
    hd->num_slots = CR_FEED_NUM_SLOTS;
    hd->format = 0;
    atomic_store(&hd->write_index, 0u);
    atomic_store(&hd->generation, prev_gen + 1u);
    atomic_store(&hd->frame_counter, 0ull);
    cur_wi = 0;

    __atomic_thread_fence(__ATOMIC_RELEASE);
    hd->magic = CR_FEED_MAGIC;
    return true;
  }

  void publish_nv21(const uint8_t* src)
  {
    if (!map)
      return;
    auto* hd = (cr_feed_header*) map;
    const uint32_t next = (cur_wi + 1u) % CR_FEED_NUM_SLOTS;
    memcpy(slot0 + (size_t) next * CR_FEED_MAX_NV21_BYTES, src, slot_bytes);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    atomic_store(&hd->write_index, next);
    atomic_fetch_add(&hd->frame_counter, 1ull);
    cur_wi = next;
  }

  void shutdown()
  {
    if (map)
    {
      munmap(map, mapped);
      map = nullptr;
    }
    if (fd >= 0)
    {
      close(fd);
      fd = -1;
    }
  }
};

// Colour-format converters (NV12 → NV21 / I420 → NV21). Same code that was
// inline in tcp_h264_feed.cpp.
inline void nv12_to_nv21(const uint8_t* src, int ww, int hh, int src_y_stride, int src_uv_stride, uint8_t* dst)
{
  for (int y = 0; y < hh; ++y)
  {
    memcpy(dst + (size_t) y * ww, src + (size_t) y * src_y_stride, (size_t) ww);
  }
  const uint8_t* src_uv = src + (size_t) src_y_stride * hh;
  uint8_t* dst_vu = dst + (size_t) ww * hh;
  const int uv_rows = hh / 2;
  for (int y = 0; y < uv_rows; ++y)
  {
    const uint8_t* s = src_uv + (size_t) y * src_uv_stride;
    uint8_t* d = dst_vu + (size_t) y * ww;
    for (int x = 0; x < ww; x += 2)
    {
      d[x + 0] = s[x + 1];
      d[x + 1] = s[x + 0];
    }
  }
}

inline void i420_to_nv21(const uint8_t* src, int ww, int hh, int src_y_stride, int src_u_stride, int src_v_stride, uint8_t* dst)
{
  for (int y = 0; y < hh; ++y)
  {
    memcpy(dst + (size_t) y * ww, src + (size_t) y * src_y_stride, (size_t) ww);
  }
  const uint8_t* src_u = src + (size_t) src_y_stride * hh;
  const uint8_t* src_v = src_u + (size_t) src_u_stride * (hh / 2);
  uint8_t* dst_vu = dst + (size_t) ww * hh;
  for (int y = 0; y < hh / 2; ++y)
  {
    const uint8_t* u = src_u + (size_t) y * src_u_stride;
    const uint8_t* v = src_v + (size_t) y * src_v_stride;
    uint8_t* out = dst_vu + (size_t) y * ww;
    for (int x = 0; x < ww / 2; ++x)
    {
      out[x * 2 + 0] = v[x];
      out[x * 2 + 1] = u[x];
    }
  }
}

// Pipeline: AMediaCodec H.264 decoder + ShmOut.
// This is the SAME setup tcp_h264_feed::serve_client built inline. After
// construction, callers push() Annex-B NAL units; the pipe internally
// drains the decoder output, converts to NV21, and publishes into shm.
class H264DecodePipe
{
public:
  bool start(int ww, int hh)
  {
    w = ww;
    h = hh;
    dec = AMediaCodec_createDecoderByType(OBF("video/avc").c_str());
    if (dec)
      chosen_name = OBF("video/avc/byType").c_str();
    auto try_named_decoder = [&](const char* name) -> bool
    {
      if (dec)
        return true;
      dec = AMediaCodec_createCodecByName(name);
      if (!dec)
        return false;
      chosen_name = name;
      return true;
    };
    try_named_decoder(OBF("c2.qti.avc.decoder").c_str());
    try_named_decoder(OBF("OMX.qcom.video.decoder.avc").c_str());
    if (!dec)
      return false;

    // Sprint A: pre-allocate the input buffer so the first big
    // keyframe doesn't trigger a mid-stream realloc (visible 100ms
    // stall in the preview). This single key is universally safe:
    // it's been in MediaFormat since API 16, and a vendor that
    // ignores it just wastes a small amount of allocation.
    // Three other knobs (low-latency, operating-rate, priority) were
    // tried earlier in this sprint but on Snapdragon 665 / OMX.qcom
    // (Android 12) they cause AMediaCodec_start to lie — it returns
    // AMEDIA_OK but every subsequent dequeueOutputBuffer fails with
    // sf error -38 (INVALID_OPERATION) at ~100/sec, and no frames
    // ever come out. Until we have a runtime probe to detect that
    // and roll back, those knobs stay off here. They CAN be re-
    // enabled per-device by editing this block — Pixel 6+ and late
    // Snapdragon 8-gen Codec2 do honour them.
    AMediaFormat* fmt = AMediaFormat_new();
    AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, OBF("video/avc").c_str());
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, w);
    AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, h);
    AMediaFormat_setInt32(fmt, OBF("max-input-size").c_str(), 1 << 20);
    media_status_t cs = AMediaCodec_configure(dec, fmt, nullptr, nullptr, 0);
    AMediaFormat_delete(fmt);
    if (cs != AMEDIA_OK)
    {
      CR_FEED_LOGE(OBF("h264 pipe: configure failed status=%d").c_str(), (int) cs);
      AMediaCodec_delete(dec);
      dec = nullptr;
      return false;
    }
    media_status_t ss = AMediaCodec_start(dec);
    if (ss != AMEDIA_OK)
    {
      CR_FEED_LOGE(OBF("h264 pipe: start failed status=%d").c_str(), (int) ss);
      AMediaCodec_delete(dec);
      dec = nullptr;
      return false;
    }
    CR_FEED_LOGI(OBF("h264 pipe: started %dx%d (%s)").c_str(), w, h, chosen_name.c_str());

    if (!shm.init(w, h))
    {
      AMediaCodec_stop(dec);
      AMediaCodec_delete(dec);
      dec = nullptr;
      return false;
    }
    nv21.assign((size_t) w * h * 3 / 2, 0);
    return true;
  }

  void stop()
  {
    if (dec)
    {
      AMediaCodec_stop(dec);
      AMediaCodec_delete(dec);
      dec = nullptr;
    }
    shm.shutdown();
  }

  const char* codec_name() const noexcept
  {
    return chosen_name.c_str();
  }

  // Push one Annex-B NAL access unit (00 00 00 01 prefix included). PTS
  // in microseconds; `is_key` is informational only — MediaCodec rebuilds
  // its CSD from the SPS/PPS prepended on every IDR.
  bool push(const uint8_t* nal, size_t len, int64_t pts_us)
  {
    if (!dec || !nal || !len)
      return false;

    ssize_t in_idx = AMediaCodec_dequeueInputBuffer(dec, 20 * 1000);
    if (in_idx >= 0)
    {
      size_t cap = 0;
      uint8_t* in_buf = AMediaCodec_getInputBuffer(dec, (size_t) in_idx, &cap);
      if (in_buf && cap >= len)
      {
        memcpy(in_buf, nal, len);
        AMediaCodec_queueInputBuffer(dec, (size_t) in_idx, 0, len, (uint64_t) pts_us, 0);
        ++in_count;
      }
      else
      {
        CR_FEED_LOGW(OBF("h264: input buf too small: cap=%zu need=%zu").c_str(), cap, len);
      }
    }
    drain();
    return true;
  }

  // Drain decoder output without queueing new input. Useful for callers
  // that want to flush after a burst.
  void drain()
  {
    if (!dec)
      return;
    for (;;)
    {
      AMediaCodecBufferInfo info;
      ssize_t out_idx = AMediaCodec_dequeueOutputBuffer(dec, &info, 0);
      if (out_idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
        break;
      if (out_idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
      {
        AMediaFormat* of = AMediaCodec_getOutputFormat(dec);
        AMediaFormat_getInt32(of, OBF("color-format").c_str(), &color_format);
        AMediaFormat_getInt32(of, OBF("stride").c_str(), &y_stride);
        AMediaFormat_getInt32(of, OBF("slice-height").c_str(), &slice_height);
        uv_stride = y_stride;
        CR_FEED_LOGI(OBF("h264 out fmt: color=0x%x stride=%d slice=%d").c_str(), color_format, y_stride, slice_height);
        AMediaFormat_delete(of);
        continue;
      }
      if (out_idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
        continue;
      if (out_idx < 0)
        break;

      if (info.size > 0)
      {
        size_t cap = 0;
        uint8_t* out_buf = AMediaCodec_getOutputBuffer(dec, (size_t) out_idx, &cap);
        const int sy = y_stride ? y_stride : w;
        const int suv = uv_stride ? uv_stride : sy;
        (void) slice_height;

        constexpr int32_t kFmtYUV420Planar = 19;
        constexpr int32_t kFmtYUV420SemiPlanar = 21;
        constexpr int32_t kFmtYUV420PackedSemiPlanar = 0x7fa30c04;
        constexpr int32_t kFmtYUV420Flexible = 0x7f420888;

        if (color_format == kFmtYUV420SemiPlanar || color_format == kFmtYUV420PackedSemiPlanar || color_format == kFmtYUV420Flexible)
        {
          nv12_to_nv21(out_buf, w, h, sy, suv, nv21.data());
        }
        else if (color_format == kFmtYUV420Planar)
        {
          i420_to_nv21(out_buf, w, h, sy, sy / 2, sy / 2, nv21.data());
        }
        else
        {
          for (int y = 0; y < h; ++y)
          {
            memcpy(nv21.data() + (size_t) y * w, out_buf + (size_t) y * sy, (size_t) w);
          }
          memset(nv21.data() + (size_t) w * h, 128, (size_t) w * h / 2);
        }
        shm.publish_nv21(nv21.data());
        ++out_count;
        if (out_count <= 3 || (out_count % 120) == 0)
        {
          CR_FEED_LOGI(OBF("h264: decoded frame #%llu (in=%llu)").c_str(), (unsigned long long) out_count, (unsigned long long) in_count);
        }
      }
      AMediaCodec_releaseOutputBuffer(dec, (size_t) out_idx, false);
    }
  }

  uint64_t input_count() const noexcept
  {
    return in_count;
  }
  uint64_t output_count() const noexcept
  {
    return out_count;
  }

private:
  AMediaCodec* dec = nullptr;
  std::string chosen_name;

  int w = 0, h = 0;
  int32_t color_format = 21; // NV12 default
  int32_t y_stride = 0;
  int32_t slice_height = 0;
  int32_t uv_stride = 0;

  ShmOut shm;
  std::vector<uint8_t> nv21;
  uint64_t in_count = 0;
  uint64_t out_count = 0;
};

} // namespace cr_feed
