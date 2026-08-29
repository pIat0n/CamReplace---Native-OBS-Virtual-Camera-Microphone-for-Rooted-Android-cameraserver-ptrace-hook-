// libcr_feed.so — entry point.
// The host-side stack is OBS-only. Two active transports for video:
//   `tcp_h264:listen:<port>`  — legacy. PC parses RTMP, packs each NAL into
//                               a 16-byte-prefixed TCP frame, sends over
//                               adb-forward; we read frames + decode.
//   `tcp_nv21:listen:<port>`  — raw NV21 from the PC-side decoder.
// And one for audio:
//   `tcp_pcm:listen:<port>`   — PCM frames, hooked up by the audhook.
// The old direct `rtmp:listen:<port>` phone listener is opt-in via
// CR_FEED_ENABLE_RTMP_DIRECT.

#include "../include/cr_feed.h"

#include <android/log.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include "util/Obf.h"

#define TAG "cr_feed"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// Shared with tcp_h264_feed.cpp — flipped by the SIGTERM/SIGINT handler so
// the receive loop unwinds cleanly on shutdown.
std::atomic<bool> g_stop{false};

namespace
{

void on_signal(int /*sig*/)
{
  g_stop.store(true);
}

bool parse_scheme(const char* url, const char* prefix, const char** out_path)
{
  if (!url || !prefix)
    return false;
  size_t plen = strlen(prefix);
  if (strncmp(url, prefix, plen) != 0)
    return false;
  *out_path = url + plen;
  return **out_path != '\0';
}

} // namespace

extern "C" int cr_feed_run_tcp_h264(int port); // tcp_h264_feed.cpp
extern "C" int cr_feed_run_tcp_nv21(int port); // tcp_nv21_feed.cpp (Transport::Raw)
extern "C" int cr_feed_run_tcp_pcm(int port);  // tcp_pcm_feed.cpp
#if defined(CR_FEED_ENABLE_RTMP_DIRECT)
extern "C" int cr_feed_run_rtmp(int port); // rtmp_h264_feed.cpp (sprint B)
#endif

extern "C" __attribute__((visibility("default" /*OBF_SKIP*/))) int cr_feed_run(const char* feed_url)
{
  signal(SIGTERM, on_signal);
  signal(SIGINT, on_signal);

  if (!feed_url)
  {
    LOGE(OBF("cr_feed_run: null URL").c_str());
    return -1;
  }
  LOGI(OBF("cr_feed_run url=%s").c_str(), feed_url);

  const char* arg = nullptr;
  if (parse_scheme(feed_url, OBF("tcp_h264:listen:").c_str(), &arg))
  {
    int port = atoi(arg);
    if (port < 1 || port > 65535)
    {
      LOGE(OBF("tcp_h264:listen:<port> missing or invalid port").c_str());
      return -1;
    }
    return cr_feed_run_tcp_h264(port);
  }
  if (parse_scheme(feed_url, OBF("tcp_nv21:listen:").c_str(), &arg))
  {
    int port = atoi(arg);
    if (port < 1 || port > 65535)
    {
      LOGE(OBF("tcp_nv21:listen:<port> missing or invalid port").c_str());
      return -1;
    }
    return cr_feed_run_tcp_nv21(port);
  }
#if defined(CR_FEED_ENABLE_RTMP_DIRECT)
  if (parse_scheme(feed_url, OBF("rtmp:listen:").c_str(), &arg))
  {
    int port = atoi(arg);
    if (port < 1 || port > 65535)
    {
      LOGE(OBF("rtmp:listen:<port> missing or invalid port").c_str());
      return -1;
    }
    return cr_feed_run_rtmp(port);
  }
#endif
  if (parse_scheme(feed_url, OBF("tcp_pcm:listen:").c_str(), &arg))
  {
    int port = atoi(arg);
    if (port < 1 || port > 65535)
    {
      LOGE(OBF("tcp_pcm:listen:<port> missing or invalid port").c_str());
      return -1;
    }
    return cr_feed_run_tcp_pcm(port);
  }

#if defined(CR_FEED_ENABLE_RTMP_DIRECT)
  LOGE(OBF("unsupported feed URL (tcp_h264:listen:<port>, tcp_nv21:listen:<port>, rtmp:listen:<port>, or tcp_pcm:listen:<port>)").c_str());
#else
  LOGE(OBF("unsupported feed URL (tcp_h264:listen:<port>, tcp_nv21:listen:<port>, or tcp_pcm:listen:<port>)").c_str());
#endif
  return -1;
}
