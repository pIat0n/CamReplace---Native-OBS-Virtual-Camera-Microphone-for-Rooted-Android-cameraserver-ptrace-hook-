// cr_feed_proc — thin helper binary that links libcr_feed.so and calls
// cr_feed_run(). It owns the active feed writer processes for camera
// replacement and mic replacement: video/PCM arrive over adb-forwarded TCP,
// are decoded or copied as needed, then published into /data/cr/feed or
// /data/cr/audio for the injected hooks to consume.
// CLI:
//   cr_feed_proc --feed tcp_h264:listen:8901

#include "../include/cr_feed.h"

#include <stdio.h>
#include <string.h>
#include "util/Obf.h"

int main(int argc, char** argv)
{
  const char* feed = nullptr;
  for (int i = 1; i < argc; ++i)
  {
    if (!strcmp(argv[i], "--feed") && i + 1 < argc)
    {
      feed = argv[++i];
      continue;
    }
  }
  if (!feed)
  {
    fprintf(stderr, OBF("usage: cr_feed_proc --feed <url>\n").c_str());
    return 2;
  }
  return cr_feed_run(feed);
}
