#pragma once

// libcr_feed.so — public C entry point.
// Called by the standalone cr_feed_proc helper (and, in principle, by any
// other process that wants to seed /data/cr/feed). The library binds the
// adb-forwarded TCP port, accepts H.264 NAL units from the host, decodes
// them via NDK MediaCodec into NV21, and writes each decoded frame to the
// shm slot the camhook is reading.

#ifdef __cplusplus
extern "C"
{
#endif

  // Only one feed URL is supported:
  //   tcp_h264:listen:<port>   — accept H.264 from 127.0.0.1:<port>
  // Blocks until SIGTERM/SIGINT or an unrecoverable error. Returns 0 on
  // clean shutdown.
  int cr_feed_run(const char* feed_url);

// Writable shared-memory layout version exposed to hooks. Bump whenever
// the header struct changes.
#define CR_FEED_SHM_VERSION 1

#ifdef __cplusplus
}
#endif
