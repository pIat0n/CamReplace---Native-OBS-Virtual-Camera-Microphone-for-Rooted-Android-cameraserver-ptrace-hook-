/*
 * Назначение файла:
 *   TCP PCM receiver для sound replacement. Host AudioPump присылает CRAU
 *   header и continuous S16LE PCM stream; receiver публикует bytes в
 *   /data/cr/audio ring для audhook внутри audioserver.
 *
 * ABI/совместимость:
 *   Shared memory остаётся v1: cr_audio_header + fixed 2 MiB ring. Формат
 *   producer stream ограничен sample_rate 8-192 kHz, 1-2 channels, S16LE.
 *   Per-stream consumer cursors живут в audhook, не в shm.
 *
 * Pixel/Android ограничения:
 *   Этот путь покрывает обычный AudioRecord через AIDL/HIDL/tinyalsa. Vendor
 *   Sound Trigger/AOC paths могут не читать из этого ring и не считаются
 *   гарантированным coverage.
 */

// tcp_pcm:listen:<port> transport.
// Accepts one publisher at a time on 127.0.0.1:<port>. The publisher sends
// a 12-byte header followed by a continuous stream of S16LE PCM samples.
// We forward those bytes into /data/cr/audio's ring buffer (mmap'd shm),
// bumping the monotonic `write_pos` so the audhook inside audioserver can
// pull them out and overwrite the StreamInHalHidl::read buffer.
// Wire format (little-endian):
//   bytes  0..3    magic = 'CRAU' (== CR_AUDIO_MAGIC)
//   bytes  4..7    sample_rate (uint32, e.g. 48000)
//   bytes  8..9    channels    (uint16, 1 or 2)
//   bytes 10..11   bytes_per_sample (uint16, must be 2)
//   bytes 12..     raw PCM, contiguous, no per-frame framing

#include "audio_shm.h"
#include "../include/cr_feed.h"
#include "secure_channel_reader.h"

#include <android/log.h>
#include <arpa/inet.h>
#include <atomic>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "util/Obf.h"
#define TAG "cr_feed"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

extern std::atomic<bool> g_stop; // shared with feed_main.cpp

namespace
{

int ensure_shm_dir()
{
  struct stat st;
  if (stat(OBF("/data/cr").c_str(), &st) == 0)
    return S_ISDIR(st.st_mode) ? 0 : -1;
  if (mkdir(OBF("/data/cr").c_str(), 0755) == 0)
    return 0;
  LOGE(OBF("mkdir /data/cr: %s").c_str(), strerror(errno));
  return -1;
}

// Open / size / mmap /data/cr/audio. Reuses the file across reconnects so
// the audhook's existing mapping stays valid (no SIGBUS on truncate).
struct AudioShm
{
  int fd = -1;
  cr_audio_header* hdr = nullptr;
  uint8_t* ring = nullptr;
  size_t map_size = 0;

  bool open()
  {
    if (ensure_shm_dir() != 0)
      return false;
    const size_t want = sizeof(cr_audio_header) + CR_AUDIO_RING_BYTES;
    fd = ::open(OBF("/data/cr/audio").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0664);
    if (fd < 0)
    {
      LOGE(OBF("open %s: %s").c_str(), OBF("/data/cr/audio").c_str(), strerror(errno));
      return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0)
    {
      LOGE(OBF("fstat: %s").c_str(), strerror(errno));
      return false;
    }
    if ((size_t) st.st_size < want)
    {
      if (ftruncate(fd, (off_t) want) != 0)
      {
        LOGE(OBF("ftruncate: %s").c_str(), strerror(errno));
        return false;
      }
    }
    void* m = mmap(nullptr, want, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED)
    {
      LOGE(OBF("mmap: %s").c_str(), strerror(errno));
      return false;
    }
    hdr = (cr_audio_header*) m;
    ring = (uint8_t*) m + sizeof(cr_audio_header);
    map_size = want;
    return true;
  }

  void close_()
  {
    if (hdr)
    {
      munmap((void*) hdr, map_size);
      hdr = nullptr;
    }
    if (fd >= 0)
    {
      ::close(fd);
      fd = -1;
    }
  }
};

// Update header for a new connection. Bumps generation so the audhook can
// notice format changes (it doesn't currently care, but the field is there).
void publish_header(AudioShm& shm, uint32_t sr, uint16_t ch, uint16_t bps)
{
  auto* h = shm.hdr;
  // Clear magic during the metadata rewrite — same trick as the camera
  // shm — so any reader mid-flight sees "no signal" rather than a torn
  // header.
  h->magic = 0;
  __atomic_thread_fence(__ATOMIC_RELEASE);

  h->version = CR_AUDIO_VERSION;
  h->sample_rate = sr;
  h->channels = ch;
  h->bytes_per_sample = bps;
  h->ring_bytes = CR_AUDIO_RING_BYTES;
  atomic_store(&h->write_pos, 0ull);
  atomic_fetch_add(&h->generation, 1u);
  atomic_store(&h->channel_state, CR_CHANNEL_STATE_NONE);

  __atomic_thread_fence(__ATOMIC_RELEASE);
  h->magic = CR_AUDIO_MAGIC;
  LOGI(OBF("audio shm armed: sr=%u ch=%u bps=%u ring=%u").c_str(), sr, (unsigned) ch, (unsigned) bps, (unsigned) CR_AUDIO_RING_BYTES);
}

// Stream PCM from `cli` into the shared ring until the peer disconnects or
// g_stop fires.
int run_stream(cr::feed::SecureChannelReader& channel, AudioShm& shm)
{
  auto* h = shm.hdr;
  const uint32_t ring = h->ring_bytes;
  uint8_t buf[8192];

  uint64_t total = 0;
  while (!g_stop.load(std::memory_order_relaxed))
  {
    if (!channel.read_exact(buf, sizeof(buf)))
    {
      LOGI(OBF("publisher gone after %llu bytes").c_str(), (unsigned long long) total);
      return 0;
    }
    const uint64_t wp = atomic_load(&h->write_pos);
    const uint32_t off = (uint32_t) (wp % ring);
    const size_t n = sizeof(buf);
    const size_t span1 = (off + n <= ring) ? n : (ring - off);
    memcpy(shm.ring + off, buf, span1);
    if (span1 < n)
      memcpy(shm.ring, buf + span1, n - span1);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    atomic_store(&h->channel_state, CR_CHANNEL_STATE_READY);
    atomic_store(&h->write_pos, wp + n);
    total += n;
  }
  return 0;
}

} // namespace

extern "C" int cr_feed_run_tcp_pcm(int port);

int cr_feed_run_tcp_pcm(int port)
{
  AudioShm shm;
  if (!shm.open())
    return -1;

  int srv = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (srv < 0)
  {
    LOGE(OBF("socket: %s").c_str(), strerror(errno));
    shm.close_();
    return -1;
  }
  int yes = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons((uint16_t) port);
  if (bind(srv, (sockaddr*) &addr, sizeof(addr)) < 0)
  {
    LOGE(OBF("bind :%d: %s").c_str(), port, strerror(errno));
    close(srv);
    shm.close_();
    return -1;
  }
  if (listen(srv, 1) < 0)
  {
    LOGE(OBF("listen: %s").c_str(), strerror(errno));
    close(srv);
    shm.close_();
    return -1;
  }
  LOGI(OBF("tcp_pcm: listening on 127.0.0.1:%d").c_str(), port);
  fprintf(stderr, "listening on 127.0.0.1:%d\n", port);
  fflush(stderr);

  while (!g_stop.load(std::memory_order_relaxed))
  {
    int cli = accept(srv, nullptr, nullptr);
    if (cli < 0)
    {
      if (errno == EINTR)
        continue;
      LOGW(OBF("accept: %s").c_str(), strerror(errno));
      break;
    }
    setsockopt(cli, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    LOGI(OBF("tcp_pcm: publisher connected").c_str());
    cr::feed::SecureChannelReader channel;
    if (!channel.handshake(cli, cr::secure_channel::v2::StreamKind::Audio))
    {
      LOGW(OBF("tcp_pcm: secure-channel handshake rejected").c_str());
      close(cli);
      continue;
    }

    // Read the 12-byte header.
    struct __attribute__((packed)) WireHdr
    {
      uint32_t magic;
      uint32_t sample_rate;
      uint16_t channels;
      uint16_t bps;
    } wh;
    if (!channel.read_exact(&wh, sizeof(wh)))
    {
      LOGW(OBF("tcp_pcm: short header — dropping").c_str());
      close(cli);
      continue;
    }
    if (wh.magic != CR_AUDIO_MAGIC)
    {
      LOGW(OBF("tcp_pcm: bad magic 0x%x").c_str(), wh.magic);
      close(cli);
      continue;
    }
    if (wh.bps != 2 || wh.channels < 1 || wh.channels > 2 || wh.sample_rate < 8000 || wh.sample_rate > 192000)
    {
      LOGW(OBF("tcp_pcm: bogus header sr=%u ch=%u bps=%u").c_str(), wh.sample_rate, wh.channels, wh.bps);
      close(cli);
      continue;
    }
    publish_header(shm, wh.sample_rate, wh.channels, wh.bps);

    run_stream(channel, shm);
    close(cli);

    // Invalidate magic between sessions so the hook falls through to
    // real mic until the next publisher arrives.
    shm.hdr->magic = 0;
    LOGI(OBF("tcp_pcm: publisher session ended; waiting for next").c_str());
  }

  close(srv);
  shm.close_();
  return 0;
}
