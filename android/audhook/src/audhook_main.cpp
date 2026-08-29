

#include "../../feed/src/audio_shm.h"
#include "shadowhook.h"

#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "util/Obf.h"

/*
 * Installs the audioserver-side microphone PCM replacement hook.
 *
 * cr_audhook_init maps the audio ring and resolves input-read implementations
 * through exact Android HAL symbol candidates and a loaded-library fallback.
 * Every resolved hook is retained so different active HAL paths are covered.
 * Each proxy calls the original read first, then replaces returned S16LE PCM
 * with data from the feed ring when a per-stream format probe is ready.
 *
 * Stream state is keyed by the HAL object and guarded by g_stream_mu; it keeps
 * independent consumer cursors, format probing, and prebuffer state. The feed
 * process owns and publishes the shared-memory header and samples; audioserver
 * only maps them read-only. Invalid headers, format transitions, underruns,
 * and unresolved hooks preserve pass-through microphone behaviour.
 */

#define TAG "cr_audhook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

struct ShmView
{
  const cr_audio_header* header = nullptr;
  const uint8_t* ring = nullptr;
  size_t size = 0;
  size_t ring_bytes = 0;
  int fd = -1;
};

static std::atomic<bool> g_installed{false};
static ShmView g_shm;

static bool audio_header_sane(const cr_audio_header* h, size_t map_size)
{
  if (!h || map_size < sizeof(cr_audio_header))
    return false;
  if (h->magic != CR_AUDIO_MAGIC)
    return false;
  if (h->bytes_per_sample != 2)
    return false;
  if (h->channels == 0 || h->channels > 2)
    return false;
  if (h->sample_rate < 8000 || h->sample_rate > 192000)
    return false;

  const uint32_t ring = h->ring_bytes;
  if (ring < 1024 || (ring & 1u) != 0)
    return false;
  const uint64_t needed = (uint64_t) sizeof(cr_audio_header) + (uint64_t) ring;
  return needed <= map_size;
}

static std::vector<void*> g_hook_stubs;

static std::unordered_set<void*> g_hooked_addrs;

static std::atomic<uint32_t> g_last_seen_gen{0};

static std::atomic<uint64_t> g_stat_calls{0};
static std::atomic<uint64_t> g_stat_total_bytes{0};
static std::atomic<uint64_t> g_stat_underrun{0};
static std::atomic<uint64_t> g_stat_resampled{0};

struct ConsumerFormat
{
  uint32_t sample_rate;
  uint8_t channels;
  bool detected;
};

struct ConsumerProbe
{
  int64_t first_ns = 0;
  int64_t last_ns = 0;
  int count = 0;
  size_t period_bytes = 0;
  ConsumerFormat fmt{0, 0, false};
};

struct ConsumerState
{
  ConsumerProbe probe;
  bool prebuffer_ready = false;
  uint64_t consumer_pos = 0;
};

static std::mutex g_stream_mu;
static std::unordered_map<void*, ConsumerState> g_streams;

static int64_t monotonic_ns_()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t) ts.tv_sec * 1000000000LL + (int64_t) ts.tv_nsec;
}

struct StdFmt
{
  uint32_t sr;
  uint8_t ch;
};

static StdFmt snap_to_standard(uint32_t observed_byte_rate)
{
  static const StdFmt kCandidates[] = {

      {8000, 1}, {12000, 1}, {16000, 1}, {22050, 1}, {24000, 1}, {32000, 1}, {44100, 1}, {48000, 1}, {96000, 1},

      {8000, 2}, {16000, 2}, {24000, 2}, {32000, 2}, {44100, 2}, {48000, 2}, {96000, 2},
  };

  StdFmt best{48000, 1};
  uint32_t best_diff = UINT32_MAX;
  for (const auto& c : kCandidates)
  {
    const uint32_t br = c.sr * (uint32_t) c.ch * 2u;
    const uint32_t diff = (br > observed_byte_rate) ? (br - observed_byte_rate) : (observed_byte_rate - br);

    if (diff < best_diff && (uint64_t) diff * 100 < (uint64_t) br * 8)
    {
      best_diff = diff;
      best = c;
    }
  }

  if (best.sr == 96000 && best.ch == 1 && observed_byte_rate >= 184320 && observed_byte_rate <= 199680)
  {
    best = {48000, 2};
  }
  return best;
}

static ConsumerFormat probe_observe_locked(void* thiz, size_t got, ConsumerProbe& p)
{
  if (p.period_bytes != 0 && p.period_bytes != got)
  {
    p = ConsumerProbe{};
  }
  p.period_bytes = got;

  const int64_t now = monotonic_ns_();
  if (p.count == 0)
    p.first_ns = now;
  p.last_ns = now;
  ++p.count;

  if (!p.fmt.detected && p.count >= 3)
  {
    const int64_t span = p.last_ns - p.first_ns;
    if (span > 0)
    {
      const double avg_period_s = (double) span / 1.0e9 / (double) (p.count - 1);

      if (avg_period_s > 0.001 && avg_period_s < 0.5)
      {
        const uint32_t br = (uint32_t) ((double) got / avg_period_s);
        const StdFmt sf = snap_to_standard(br);
        p.fmt = {sf.sr, sf.ch, true};
        LOGI(OBF("audio detect: thiz=%p period=%zuB avg_t=%.2fms byte_rate=%u -> %u Hz / %u ch").c_str(), thiz, got, avg_period_s * 1000.0, br, sf.sr, sf.ch);
      }
    }
  }
  return p.fmt;
}

static void stream_clear_all_()
{
  std::lock_guard<std::mutex> lk(g_stream_mu);
  g_streams.clear();
}

static ConsumerState& stream_state_locked(void* key)
{
  if (g_streams.size() > 32 && g_streams.find(key) == g_streams.end())
  {
    g_streams.clear();
  }
  return g_streams[key];
}

static size_t resample_pcm_s16(const uint8_t* ring, size_t ring_size, uint64_t src_pos_bytes, uint32_t src_sr, uint8_t src_ch, int16_t* dst, size_t dst_frames, uint32_t dst_sr, uint8_t dst_ch)
{
  if (src_sr == 0 || dst_sr == 0 || src_ch == 0 || dst_ch == 0)
    return 0;

  const uint64_t step_q16 = ((uint64_t) src_sr << 16) / (uint64_t) dst_sr;

  auto sample_at = [&](uint64_t frame_idx, uint8_t ch) -> int16_t
  {
    const uint64_t off = (src_pos_bytes + frame_idx * (uint64_t) src_ch * 2 + (uint64_t) ch * 2) % ring_size;
    const uint8_t* p = ring + off;
    return (int16_t) ((uint16_t) p[0] | ((uint16_t) p[1] << 8));
  };

  uint64_t pos_q16 = 0;
  for (size_t df = 0; df < dst_frames; ++df)
  {
    const uint64_t s_idx = pos_q16 >> 16;
    const uint32_t frac = (uint32_t) (pos_q16 & 0xFFFF);

    int32_t l = sample_at(s_idx, 0);
    int32_t l1 = sample_at(s_idx + 1, 0);
    l = l + (((l1 - l) * (int32_t) frac) >> 16);

    int32_t r;
    if (src_ch >= 2)
    {
      int32_t r0 = sample_at(s_idx, 1);
      int32_t r1 = sample_at(s_idx + 1, 1);
      r = r0 + (((r1 - r0) * (int32_t) frac) >> 16);
    }
    else
    {
      r = l;
    }

    if (dst_ch == 1)
    {
      dst[df] = (int16_t) l;
    }
    else
    {
      dst[df * 2 + 0] = (int16_t) l;
      dst[df * 2 + 1] = (int16_t) r;
    }

    pos_q16 += step_q16;
  }

  const uint64_t src_frames = (pos_q16 >> 16) + ((pos_q16 & 0xFFFF) ? 1 : 0);
  return (size_t) (src_frames * (uint64_t) src_ch * 2);
}

static int open_shm()
{
  int fd = open(OBF("/data/cr/audio").c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0)
  {
    LOGW(OBF("open %s: %s").c_str(), OBF("/data/cr/audio").c_str(), strerror(errno));
    return -1;
  }
  struct stat st;
  if (fstat(fd, &st) < 0 || st.st_size <= 0 || (size_t) st.st_size < sizeof(cr_audio_header) + 1024)
  {
    LOGW(OBF("fstat/size bad: size=%lld").c_str(), (long long) st.st_size);
    close(fd);
    return -1;
  }
  void* map = mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED)
  {
    LOGW(OBF("mmap %s: %s").c_str(), OBF("/data/cr/audio").c_str(), strerror(errno));
    close(fd);
    return -1;
  }
  auto* h = (const cr_audio_header*) map;
  if (!audio_header_sane(h, (size_t) st.st_size))
  {
    LOGW(OBF("bad audio header: magic=0x%x sr=%u ch=%u bps=%u ring=%u").c_str(), h->magic, h->sample_rate, h->channels, h->bytes_per_sample, h->ring_bytes);
    munmap(map, st.st_size);
    close(fd);
    return -1;
  }
  g_shm.header = h;
  g_shm.ring = (const uint8_t*) map + sizeof(cr_audio_header);
  g_shm.size = (size_t) st.st_size;
  g_shm.ring_bytes = h->ring_bytes;
  g_shm.fd = fd;
  LOGI(OBF("shm mapped: sr=%u ch=%u ring=%u (file=%zu bytes)").c_str(), h->sample_rate, h->channels, h->ring_bytes, g_shm.size);
  return 0;
}

static void try_lazy_open()
{
  if (g_shm.header)
    return;
  static std::atomic<int> n{0};
  if ((n.fetch_add(1, std::memory_order_relaxed) % 30) == 0)
  {
    open_shm();
  }
}

static bool deliver_pcm_locked(ConsumerState& state, uint8_t* dst, size_t bytes, ConsumerFormat target)
{
  const auto* h = g_shm.header;
  const uint32_t ring = (uint32_t) g_shm.ring_bytes;
  if (ring < 2u || (ring & 1u) != 0u)
    return false;
  const uint32_t src_sr = h->sample_rate;
  const uint8_t src_ch = (uint8_t) h->channels;
  if (src_sr < 8000 || src_sr > 192000 || src_ch == 0 || src_ch > 2 || h->bytes_per_sample != 2)
  {
    return false;
  }

  const uint64_t prebuf = (src_sr && src_ch) ? ((uint64_t) src_sr * src_ch * h->bytes_per_sample) / 10 : 19200ull;

  const uint64_t wp = atomic_load(&h->write_pos);

  if (!state.prebuffer_ready)
  {
    if (wp < prebuf)
      return false;
    state.prebuffer_ready = true;
    state.consumer_pos = wp - prebuf;
  }

  uint64_t rp = state.consumer_pos;

  if (rp + ring < wp)
  {
    rp = (wp > prebuf) ? (wp - prebuf) : 0;
    ++g_stat_underrun;
  }

  if (rp >= wp)
  {
    return false;
  }

  if (src_sr == target.sample_rate && src_ch == target.channels)
  {
    const uint64_t avail = wp - rp;
    const size_t copy = (avail < bytes) ? (size_t) avail : bytes;
    const size_t off1 = (size_t) (rp % ring);
    const size_t span1 = (off1 + copy <= ring) ? copy : (ring - off1);
    memcpy(dst, g_shm.ring + off1, span1);
    if (span1 < copy)
      memcpy(dst + span1, g_shm.ring, copy - span1);
    state.consumer_pos = rp + copy;
    return true;
  }

  const size_t dst_bytes_per_frame = (size_t) target.channels * 2;
  if (dst_bytes_per_frame == 0)
    return false;
  const size_t dst_frames = bytes / dst_bytes_per_frame;
  if (dst_frames == 0)
    return false;

  const uint64_t src_frames_needed = ((uint64_t) dst_frames * src_sr + target.sample_rate - 1) / target.sample_rate + 1;
  const uint64_t src_bytes_needed = src_frames_needed * (uint64_t) src_ch * 2;
  const uint64_t avail = wp - rp;
  if (avail < src_bytes_needed)
  {
    return false;
  }

  const size_t src_consumed = resample_pcm_s16(g_shm.ring, ring, rp, src_sr, src_ch, (int16_t*) dst, dst_frames, target.sample_rate, target.channels);

  const size_t advance = (src_consumed > avail) ? (size_t) avail : src_consumed;
  state.consumer_pos = rp + advance;
  g_stat_resampled.fetch_add(1, std::memory_order_relaxed);
  return true;
}

static void apply_substitute(void* thiz, uint8_t* buffer, size_t got, const char* path_tag)
{
  if (!g_shm.header)
  {
    try_lazy_open();
    if (!g_shm.header)
      return;
  }
  if (g_shm.header->magic != CR_AUDIO_MAGIC)
  {
    g_last_seen_gen.store(0, std::memory_order_relaxed);
    stream_clear_all_();
    return;
  }
  if (!audio_header_sane(g_shm.header, g_shm.size))
  {
    return;
  }

  if (atomic_load(&g_shm.header->channel_state) != CR_CHANNEL_STATE_READY)
    return;

  const uint32_t cur_gen = atomic_load(&g_shm.header->generation);
  const uint32_t prev_gen = g_last_seen_gen.exchange(cur_gen, std::memory_order_relaxed);
  if (prev_gen != 0 && prev_gen != cur_gen)
  {
    stream_clear_all_();
    LOGI(OBF("format change detected: gen %u -> %u (sr=%u ch=%u bps=%u) - prebuffer + probes reset").c_str(), prev_gen, cur_gen, g_shm.header->sample_rate, g_shm.header->channels, g_shm.header->bytes_per_sample);
  }

  ConsumerFormat target{};
  bool replaced = false;
  {
    std::lock_guard<std::mutex> lk(g_stream_mu);
    ConsumerState& state = stream_state_locked(thiz);
    target = probe_observe_locked(thiz, got, state.probe);
    if (!target.detected)
      return;
    replaced = deliver_pcm_locked(state, buffer, got, target);
  }

  uint64_t n = ++g_stat_calls;
  g_stat_total_bytes.fetch_add(got, std::memory_order_relaxed);
  if (n == 1)
  {
    LOGI(OBF("%s first hit: client_bytes=%zu replaced=%d target_sr=%u target_ch=%u producer_sr=%u producer_ch=%u %s").c_str(),
         path_tag, got, (int) replaced, target.sample_rate, target.channels, g_shm.header->sample_rate, g_shm.header->channels, (target.sample_rate == g_shm.header->sample_rate && target.channels == g_shm.header->channels) ? OBF("(direct memcpy path)").c_str() : OBF("(resample+mux path)").c_str());
  }
  else if ((n % 250) == 0)
  {
    LOGI(OBF("%s hit#%llu bytes=%zu replaced=%d total=%llu underruns=%llu resampled=%llu").c_str(), path_tag, (unsigned long long) n, got, (int) replaced, (unsigned long long) g_stat_total_bytes.load(), (unsigned long long) g_stat_underrun.load(), (unsigned long long) g_stat_resampled.load());
  }
}

static int32_t proxy_streamin_read(void* thiz, void* buffer, size_t bytes, size_t* read_out)
{
  SHADOWHOOK_STACK_SCOPE();
  int32_t rc = SHADOWHOOK_CALL_PREV(proxy_streamin_read, thiz, buffer, bytes, read_out);

  if (rc != 0 || !read_out || !buffer)
    return rc;
  size_t got = *read_out;
  if (got == 0)
    return rc;

  apply_substitute(thiz, (uint8_t*) buffer, got, OBF("stream").c_str());
  return rc;
}

static int proxy_pcm_read(void* pcm, void* data, unsigned int count)
{
  SHADOWHOOK_STACK_SCOPE();
  int rc = SHADOWHOOK_CALL_PREV(proxy_pcm_read, pcm, data, count);

  if (rc != 0 || !data || count == 0)
    return rc;
  apply_substitute(pcm, (uint8_t*) data, count, OBF("pcm").c_str());
  return rc;
}

static bool find_loaded_lib(const char* needle, char* out, size_t out_sz)
{
  FILE* f = fopen(OBF("/proc/self/maps").c_str(), "r");
  if (!f)
    return false;

  char line[4096];
  bool found = false;
  while (fgets(line, sizeof(line), f))
  {
    if (!strstr(line, OBF("r-xp").c_str()))
      continue;
    if (!strstr(line, needle))
      continue;
    char* slash = strchr(line, '/');
    if (!slash)
      continue;

    char* nl = strchr(slash, '\n');
    if (nl)
      *nl = 0;
    strncpy(out, slash, out_sz - 1);
    out[out_sz - 1] = 0;
    found = true;
    break;
  }
  fclose(f);
  return found;
}

static void* resolve_addr(const char* lib, const char* sym)
{
  void* h = dlopen(lib, RTLD_NOW | RTLD_NOLOAD);
  if (!h)
    h = dlopen(lib, RTLD_NOW);
  void* addr = h ? dlsym(h, sym) : nullptr;
  if (!addr)
  {
    void* sh = shadowhook_dlopen(lib);
    if (sh)
    {
      addr = shadowhook_dlsym(sh, sym);
      shadowhook_dlclose(sh);
    }
  }
  return addr;
}

static int try_hook_streamin(const char* lib, const char* sym)
{
  void* addr = resolve_addr(lib, sym);
  if (!addr)
    return -1;
  if (g_hooked_addrs.count(addr))
  {
    return 0;
  }

  void* stub = shadowhook_hook_sym_name(lib, sym, (void*) proxy_streamin_read, nullptr);
  if (stub)
  {
    g_hook_stubs.push_back(stub);
    g_hooked_addrs.insert(addr);
    LOGI(OBF("hooked %s in %s (StreamIn::read)").c_str(), sym, lib);
    return 0;
  }
  return -1;
}

static int try_hook_pcm(const char* lib)
{
  auto pcm_read_sym = OBF("pcm_read");
  void* addr = resolve_addr(lib, pcm_read_sym.c_str());
  if (!addr)
    return -1;
  if (g_hooked_addrs.count(addr))
    return 0;

  void* stub = shadowhook_hook_sym_name(lib, pcm_read_sym.c_str(), (void*) proxy_pcm_read, nullptr);
  if (stub)
  {
    g_hook_stubs.push_back(stub);
    g_hooked_addrs.insert(addr);
    LOGI(OBF("hooked %s in %s (tinyalsa fallback)").c_str(), pcm_read_sym.c_str(), lib);
    return 0;
  }
  return -1;
}

static int try_hook()
{
  shadowhook_init(SHADOWHOOK_MODE_SHARED, false);

  auto pass_a_one = [&](const char* lib, const char* sym) { try_hook_streamin(lib, sym); };

  pass_a_one(OBF("libaudiohal@aidl.so").c_str(), OBF("_ZN7android15StreamInHalAidl4readEPvmPm").c_str());

  pass_a_one(OBF("libaudiohal@7.1.so").c_str(), OBF("_ZN7android15StreamInHalHidl4readEPvmPm").c_str());
  pass_a_one(OBF("libaudiohal@7.0.so").c_str(), OBF("_ZN7android15StreamInHalHidl4readEPvmPm").c_str());
  pass_a_one(OBF("libaudiohal@6.0.so").c_str(), OBF("_ZN7android15StreamInHalHidl4readEPvmPm").c_str());
  pass_a_one(OBF("libaudiohal@5.0.so").c_str(), OBF("_ZN7android15StreamInHalHidl4readEPvmPm").c_str());

  pass_a_one(OBF("libaudiohal@7.1.so").c_str(), OBF("_ZN7android4V7_115StreamInHalHidl4readEPvmPm").c_str());
  pass_a_one(OBF("libaudiohal@7.0.so").c_str(), OBF("_ZN7android4V7_015StreamInHalHidl4readEPvmPm").c_str());
  pass_a_one(OBF("libaudiohal@6.0.so").c_str(), OBF("_ZN7android4V6_015StreamInHalHidl4readEPvmPm").c_str());
  pass_a_one(OBF("libaudiohal@5.0.so").c_str(), OBF("_ZN7android4V5_015StreamInHalHidl4readEPvmPm").c_str());
  pass_a_one(OBF("libaudiohal@4.0.so").c_str(), OBF("_ZN7android4V4_015StreamInHalHidl4readEPvmPm").c_str());

  pass_a_one(OBF("libaudiohal@7.1.so").c_str(), OBF("_ZN7android4V7_116StreamInHalLocal4readEPvmPm").c_str());
  pass_a_one(OBF("libaudiohal@7.0.so").c_str(), OBF("_ZN7android4V7_016StreamInHalLocal4readEPvmPm").c_str());
  pass_a_one(OBF("libaudiohal@6.0.so").c_str(), OBF("_ZN7android4V6_016StreamInHalLocal4readEPvmPm").c_str());

  char path[512];
  auto try_streamin_syms = [&](const char* p) -> bool
  {
    bool any = false;
    auto try_one = [&](const char* sym)
    {
      if (try_hook_streamin(p, sym) == 0)
        any = true;
    };
    try_one(OBF("_ZN7android15StreamInHalAidl4readEPvmPm").c_str());
    try_one(OBF("_ZN7android15StreamInHalHidl4readEPvmPm").c_str());
    try_one(OBF("_ZN7android16StreamInHalLocal4readEPvmPm").c_str());

    try_one(OBF("_ZN7android15StreamInHalAidl4readEPvjPj").c_str());
    try_one(OBF("_ZN7android15StreamInHalHidl4readEPvjPj").c_str());
    try_one(OBF("_ZN7android16StreamInHalLocal4readEPvjPj").c_str());
    return any;
  };

  auto pass_b_one = [&](const char* needle)
  {
    if (!find_loaded_lib(needle, path, sizeof(path)))
      return;
    LOGI(OBF("scan: %s → %s").c_str(), needle, path);

    if (strstr(path, OBF("libtinyalsa").c_str()))
    {
      try_hook_pcm(path);
      return;
    }

    if (!try_streamin_syms(path))
    {
      LOGW(OBF("found %s but none of the StreamIn symbols resolved").c_str(), path);
    }
  };

  pass_b_one(OBF("libaudiohal@aidl").c_str());
  pass_b_one(OBF("libaudiohal@").c_str());
  pass_b_one(OBF("libaudioflinger").c_str());
  pass_b_one(OBF("libtinyalsa").c_str());

  LOGI(OBF("audhook: %zu hook(s) installed").c_str(), g_hook_stubs.size());
  LOGI(OBF("audhook coverage: AudioRecord AIDL/HIDL/tinyalsa; SoundTrigger/AOC not guaranteed").c_str());
  return g_hook_stubs.empty() ? -1 : 0;
}

extern "C" __attribute__((visibility("default" /*OBF_SKIP*/))) int cr_audhook_shutdown();

extern "C" __attribute__((visibility("default" /*OBF_SKIP*/))) int cr_audhook_init(const char* feed_url)
{
  if (g_installed.exchange(true))
  {
    LOGI(OBF("already installed").c_str());
    return 0;
  }
  LOGI(OBF("cr_audhook_init feed_url=%s").c_str(), feed_url ? feed_url : OBF("(null)").c_str());

  open_shm();

  int hook_rc = try_hook();
  if (hook_rc == 0)
  {
    mkdir(OBF("/data/cr").c_str(), 0755);
    chmod(OBF("/data/cr").c_str(), 0777);
    int fd = open(OBF("/data/cr/audhook.ready").c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd >= 0)
    {
      char buf[32];
      int n = snprintf(buf, sizeof(buf), "%ld\n", (long) getpid());
      if (n > 0)
        (void) write(fd, buf, (size_t) n);
      close(fd);
    }
    return 0;
  }

  LOGE(OBF("no supported audio-read symbol found in any loaded HAL - hook not installed").c_str());
  g_installed = false;
  return -1;
}

extern "C" __attribute__((visibility("default" /*OBF_SKIP*/))) int cr_audhook_shutdown()
{
  if (!g_installed.exchange(false))
    return 0;
  for (void* stub : g_hook_stubs)
  {
    if (stub)
      shadowhook_unhook(stub);
  }
  g_hook_stubs.clear();
  g_hooked_addrs.clear();
  stream_clear_all_();
  if (g_shm.fd >= 0)
  {
    munmap((void*) g_shm.header, g_shm.size);
    close(g_shm.fd);
    g_shm = {};
  }
  LOGI(OBF("cr_audhook_shutdown done").c_str());
  return 0;
}
