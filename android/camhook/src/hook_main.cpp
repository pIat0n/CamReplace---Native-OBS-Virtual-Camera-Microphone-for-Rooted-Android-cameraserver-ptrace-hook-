

#include "cr_feed.h"
#include "shm.h"
#include "nv21_blit.h"
#include "jpeg_encode.h"
#include "shadowhook.h"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <vector>
#include "util/Obf.h"

/*
 * Installs the cameraserver-side camera-frame replacement hook.
 *
 * cr_camhook_init maps the feed shared-memory file, selects the configured
 * NV21 scaler, resolves compatible camera buffer APIs, and installs a
 * ShadowHook proxy for a supported Camera3OutputStream queue method. The
 * proxy validates the mapped feed header, locks the destination buffer,
 * scales the latest committed NV21 frame into it, and calls the original
 * queue method. Multiple mangled symbol and buffer-mapper layouts are tried
 * to cover Android platform variants without runtime SDK branching.
 *
 * Photo mode writes a JPEG BLOB instead of a preview frame. Shared memory is
 * owned by the feed process; this library maps it read-only and never changes
 * its producer state. Global hook and mapper state is process-local to
 * cameraserver. Invalid headers, unavailable CPU mappings, unsupported
 * layouts, or absent frames fall through to the original camera pipeline.
 */

#define TAG "cr_camhook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

struct ShmView
{
  const cr_feed_header* header = nullptr;
  const uint8_t* slot0 = nullptr;
  size_t size = 0;
  int fd = -1;

  const uint8_t* current_slot() const noexcept
  {
    if (!header || !slot0)
      return nullptr;

    const uint32_t raw_n = header->num_slots;
    const uint32_t n = (raw_n == 0 || raw_n > CR_FEED_NUM_SLOTS) ? 1u : raw_n;
    if (n <= 1)
      return slot0;

    const uint32_t wi = atomic_load(&header->write_index) % n;

    const size_t off = (size_t) wi * CR_FEED_MAX_NV21_BYTES;
    const size_t hdr = sizeof(cr_feed_header);
    if (size < hdr || off > (size - hdr) || CR_FEED_MAX_NV21_BYTES > (size - hdr - off))
    {
      return slot0;
    }
    return slot0 + off;
  }
};

static std::atomic<bool> g_installed{false};
static ShmView g_shm;
static void* g_hook_stub = nullptr;

static bool feed_header_sane(const cr_feed_header* h, size_t map_size)
{
  if (!h || map_size < sizeof(cr_feed_header))
    return false;
  if (h->magic != CR_FEED_MAGIC)
    return false;
  if (h->width == 0 || h->height == 0 || h->width > CR_FEED_MAX_WIDTH || h->height > CR_FEED_MAX_HEIGHT)
  {
    return false;
  }
  if (h->slot_size == 0 || h->slot_size > CR_FEED_MAX_NV21_BYTES)
    return false;
  const uint64_t min_slot = (uint64_t) h->width * (uint64_t) h->height * 3ull / 2ull;
  if (min_slot == 0 || min_slot > h->slot_size)
    return false;

  const uint32_t n = h->num_slots;
  if (n == 0 || n > CR_FEED_NUM_SLOTS)
    return false;

  uint64_t required = sizeof(cr_feed_header);
  if (n <= 1)
  {
    required += (uint64_t) h->slot_size;
  }
  else
  {
    required += (uint64_t) n * (uint64_t) CR_FEED_MAX_NV21_BYTES;
  }
  return required <= map_size;
}

typedef void (*cr_blit_fn)(const uint8_t* src, int src_w, int src_h, uint8_t* dst_y, uint8_t* dst_vu, int dst_w, int dst_h, int dst_y_stride, int dst_uv_stride, int dst_uv_swapped);
static cr_blit_fn g_blit_fn = cr_nv21_blit;
enum class BlitKind
{
  Bilinear,
  Lanczos2,
};
static BlitKind g_blit_kind = BlitKind::Bilinear;

static const char* blit_name()
{
  return g_blit_kind == BlitKind::Lanczos2 ? "lanczos2" : "bilinear";
}

static void resolve_scaler()
{
  g_blit_fn = cr_nv21_blit;
  g_blit_kind = BlitKind::Bilinear;

  int fd = open(OBF("/data/cr/scaler").c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0)
  {
    LOGI(OBF("scaler = bilinear (default; no /data/cr/scaler)").c_str());
    return;
  }
  char buf[32] = {};
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0)
  {
    LOGI(OBF("scaler = bilinear (default; empty /data/cr/scaler)").c_str());
    return;
  }
  while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ' || buf[n - 1] == '\t'))
  {
    buf[--n] = 0;
  }
  if (strcmp(buf, OBF("lanczos").c_str()) == 0 || strcmp(buf, OBF("lanczos2").c_str()) == 0)
  {
    g_blit_fn = cr_nv21_blit_lanczos;
    g_blit_kind = BlitKind::Lanczos2;
  }
  LOGI(OBF("scaler = %s (/data/cr/scaler=\"%s\")").c_str(), blit_name(), buf);
}

static inline void blit_adaptive(const uint8_t* src, int src_w, int src_h, uint8_t* dst_y, uint8_t* dst_vu, int dst_w, int dst_h, int dst_y_stride, int dst_uv_stride, int dst_uv_swapped)
{
  if (g_blit_fn != cr_nv21_blit && (int64_t) dst_w * (int64_t) dst_h > 1920LL * 1080LL)
  {
    cr_nv21_blit(src, src_w, src_h, dst_y, dst_vu, dst_w, dst_h, dst_y_stride, dst_uv_stride, dst_uv_swapped);
  }
  else
  {
    g_blit_fn(src, src_w, src_h, dst_y, dst_vu, dst_w, dst_h, dst_y_stride, dst_uv_stride, dst_uv_swapped);
  }
}

static inline int64_t monotonic_ns()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t) ts.tv_sec * 1000000000LL + (int64_t) ts.tv_nsec;
}

static std::atomic<bool> g_photo_mode{false};

static constexpr int64_t kPhotoFlagPollNs = 500LL * 1000LL * 1000LL;
static std::atomic<int64_t> g_photo_flag_last_check_ns{0};

static inline bool read_photo_flag_file_()
{
  int fd = open(OBF("/data/cr/photo_mode").c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return false;
  char buf[16] = {};
  read(fd, buf, sizeof(buf) - 1);
  close(fd);
  return (buf[0] != 0 && buf[0] != '0');
}

static bool refresh_photo_mode_()
{
  const int64_t now_ns = monotonic_ns();
  const int64_t last = g_photo_flag_last_check_ns.load(std::memory_order_relaxed);
  if (now_ns - last < kPhotoFlagPollNs)
  {
    return g_photo_mode.load(std::memory_order_relaxed);
  }
  g_photo_flag_last_check_ns.store(now_ns, std::memory_order_relaxed);

  const bool on = read_photo_flag_file_();
  const bool prev = g_photo_mode.exchange(on, std::memory_order_relaxed);
  if (prev != on)
  {
    LOGI(OBF("photo mode -> %s (file flag %s)").c_str(), on ? "ON" : OBF("OFF").c_str(), on ? OBF("appeared").c_str() : OBF("removed").c_str());
  }
  return on;
}

struct CR_Camera3JpegBlob
{
  uint16_t blob_id;
  uint32_t jpeg_size;
};
static_assert(sizeof(CR_Camera3JpegBlob) == 8, "trailer must be 8 bytes (uint16 + 2 pad + uint32)" /*OBF_SKIP*/);
static constexpr uint16_t CR_CAMERA3_JPEG_BLOB_ID = 0x00FF;

static void resolve_photo_mode()
{
  g_photo_mode.store(false);

  int fd = open(OBF("/data/cr/photo_mode").c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0)
  {
    LOGI(OBF("photo mode = OFF (no /data/cr/photo_mode)").c_str());
    return;
  }
  char buf[16] = {};
  read(fd, buf, sizeof(buf) - 1);
  close(fd);

  bool on = (buf[0] != 0 && buf[0] != '0');
  if (on)
  {
    g_photo_mode.store(true);
    LOGI(OBF("photo mode = ON — BLOB capture will be JPEG-encoded from the latest /data/cr/feed slot").c_str());
  }
  else
  {
    LOGI(OBF("photo mode = OFF (/data/cr/photo_mode=\"%s\")").c_str(), buf);
  }
}

struct CR_ARect
{
  int32_t left, top, right, bottom;
};

struct CR_AHB_Plane
{
  void* data;
  uint32_t pixelStride;
  uint32_t rowStride;
};
struct CR_AHB_Planes
{
  CR_AHB_Plane planes[4];
  uint32_t planeCount;
};

constexpr uint64_t kAhbUsageCpuRW = 0x2ull | 0x30ull;

typedef int (*AHB_lockPlanes_fn)(void* buffer, uint64_t usage, int32_t fence, const CR_ARect* rect, CR_AHB_Planes* outPlanes);
typedef int (*AHB_unlock_fn)(void* buffer, int32_t* fence);

typedef int (*AHB_lock_fn)(void* buffer, uint64_t usage, int32_t fence, const CR_ARect* rect, void** outVirtualAddress);

static AHB_lockPlanes_fn g_ahb_lockPlanes = nullptr;
static AHB_unlock_fn g_ahb_unlock = nullptr;
static AHB_lock_fn g_ahb_lock = nullptr;

struct Rect
{
  int32_t left, top, right, bottom;
};

struct android_ycbcr
{
  void* y;
  void* cb;
  void* cr;
  size_t ystride;
  size_t cstride;
  size_t chroma_step;
  uint32_t reserved[8];
};

using GBM_get_fn = void* (*) ();
using GBM_lockYCbCr_fn = int (*)(void* self, const void* handle, uint32_t usage, const Rect& bounds, android_ycbcr* out);
using GBM_unlock_v1_fn = int (*)(void* self, const void* handle);
using GBM_unlock_v2_fn = int (*)(void* self, const void* handle, void* outFence);

using GBM_lock_v1_fn = int (*)(void* self, const void* handle, uint32_t usage, const Rect& bounds, void** outVaddr);
using GBM_lock_v2_fn = int (*)(void* self, const void* handle, uint32_t usage, const Rect& bounds, void** outVaddr, int32_t* outBytesPerPixel, int32_t* outBytesPerStride);

static void* g_gbm = nullptr;
static GBM_lockYCbCr_fn g_gbm_lockYCbCr = nullptr;
static GBM_unlock_v1_fn g_gbm_unlock_v1 = nullptr;
static GBM_unlock_v2_fn g_gbm_unlock_v2 = nullptr;
static GBM_lock_v1_fn g_gbm_lock_v1 = nullptr;
static GBM_lock_v2_fn g_gbm_lock_v2 = nullptr;

static int gbm_call_unlock(const void* handle)
{
  if (g_gbm_unlock_v1)
    return g_gbm_unlock_v1(g_gbm, handle);
  if (g_gbm_unlock_v2)
    return g_gbm_unlock_v2(g_gbm, handle, nullptr);
  return -1;
}

static bool gbm_ready()
{
  return g_gbm && g_gbm_lockYCbCr && (g_gbm_unlock_v1 || g_gbm_unlock_v2);
}

static int open_shm()
{
  int fd = open(OBF("/data/cr/feed").c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0)
  {
    LOGW(OBF("open %s: %s").c_str(), OBF("/data/cr/feed").c_str(), strerror(errno));
    return -1;
  }
  struct stat st;
  if (fstat(fd, &st) < 0 || st.st_size <= 0 || (size_t) st.st_size < sizeof(cr_feed_header))
  {
    LOGW(OBF("fstat/size bad: size=%lld").c_str(), (long long) st.st_size);
    close(fd);
    return -1;
  }
  void* map = mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED)
  {
    LOGW(OBF("mmap %s: %s").c_str(), OBF("/data/cr/feed").c_str(), strerror(errno));
    close(fd);
    return -1;
  }
  auto* h = (const cr_feed_header*) map;
  if (!feed_header_sane(h, (size_t) st.st_size))
  {
    LOGW(OBF("bad feed shape: %ux%u slot=%u slots=%u").c_str(), h->width, h->height, h->slot_size, h->num_slots);
    munmap(map, st.st_size);
    close(fd);
    return -1;
  }
  g_shm.header = h;
  g_shm.slot0 = (const uint8_t*) map + sizeof(cr_feed_header);
  g_shm.size = (size_t) st.st_size;
  g_shm.fd = fd;
  LOGI(OBF("shm mapped: %ux%u (%zu bytes)").c_str(), h->width, h->height, g_shm.size);
  return 0;
}

template <typename Fn> static Fn resolve_sym(void* dl_handle, void* sh_handle, const char* sym)
{
  Fn fn = (Fn) dlsym(dl_handle, sym);
  if (!fn && sh_handle)
  {
    fn = (Fn) shadowhook_dlsym(sh_handle, sym);
  }
  return fn;
}

struct elf_dynsym_query
{
  const char* libname;
  const ElfW(Sym) * symtab;
  const char* strtab;
  size_t strtab_size;
  ElfW(Addr) load_bias;
  uint32_t nsyms;
  bool found;
};

static bool loaded_object_matches(const dl_phdr_info* info, const char* libname)
{
  if (!libname || !*libname)
    return false;

  if (strcmp(libname, OBF("cameraserver").c_str()) == 0)
  {
    if (!info->dlpi_name || !*info->dlpi_name)
      return true;
    const char* base = strrchr(info->dlpi_name, '/');
    base = base ? base + 1 : info->dlpi_name;
    return strcmp(base, libname) == 0;
  }

  if (!info->dlpi_name || !*info->dlpi_name)
    return false;

  const char* base = strrchr(info->dlpi_name, '/');
  base = base ? base + 1 : info->dlpi_name;
  return strcmp(base, libname) == 0;
}

static int dynsym_iterate_cb(struct dl_phdr_info* info, size_t, void* data)
{
  auto* q = (elf_dynsym_query*) data;
  if (!loaded_object_matches(info, q->libname))
    return 0;

  const ElfW(Phdr)* dynamic_ph = nullptr;
  for (int i = 0; i < info->dlpi_phnum; ++i)
  {
    if (info->dlpi_phdr[i].p_type == PT_DYNAMIC)
    {
      dynamic_ph = &info->dlpi_phdr[i];
      break;
    }
  }
  if (!dynamic_ph)
    return 0;

  const ElfW(Dyn)* dyn = (const ElfW(Dyn)*) (info->dlpi_addr + dynamic_ph->p_vaddr);
  const ElfW(Sym)* symtab = nullptr;
  const char* strtab = nullptr;
  size_t strtab_size = 0;
  const uint32_t* hash = nullptr;
  const uint32_t* gnu_hash = nullptr;

  for (; dyn->d_tag != DT_NULL; ++dyn)
  {
    switch (dyn->d_tag)
    {
    case DT_SYMTAB:
      symtab = (const ElfW(Sym)*) (info->dlpi_addr + dyn->d_un.d_ptr);
      break;
    case DT_STRTAB:
      strtab = (const char*) (info->dlpi_addr + dyn->d_un.d_ptr);
      break;
    case DT_STRSZ:
      strtab_size = dyn->d_un.d_val;
      break;
    case DT_HASH:
      hash = (const uint32_t*) (info->dlpi_addr + dyn->d_un.d_ptr);
      break;
    case DT_GNU_HASH:
      gnu_hash = (const uint32_t*) (info->dlpi_addr + dyn->d_un.d_ptr);
      break;
    }
  }
  if (!symtab || !strtab)
    return 0;

  uint32_t nsyms = 0;
  if (hash)
  {
    nsyms = hash[1];
  }
  else if (gnu_hash)
  {
    uint32_t nbuckets = gnu_hash[0];
    uint32_t symoffset = gnu_hash[1];
    uint32_t bloom_size = gnu_hash[2];
    const ElfW(Addr)* bloom = (const ElfW(Addr)*) &gnu_hash[4];
    const uint32_t* buckets = (const uint32_t*) &bloom[bloom_size];
    const uint32_t* chains = &buckets[nbuckets];
    uint32_t max_idx = 0;
    for (uint32_t i = 0; i < nbuckets; ++i)
    {
      if (buckets[i] > max_idx)
        max_idx = buckets[i];
    }
    if (max_idx < symoffset)
    {
      nsyms = symoffset;
    }
    else
    {
      uint32_t i = max_idx - symoffset;
      while ((chains[i] & 1u) == 0u)
        ++i;
      nsyms = symoffset + i + 1;
    }
  }
  else
  {
    return 0;
  }

  q->symtab = symtab;
  q->strtab = strtab;
  q->strtab_size = strtab_size;
  q->load_bias = info->dlpi_addr;
  q->nsyms = nsyms;
  q->found = true;
  return 1;
}

static void* find_sym_by_prefix(const char* libname, const char* prefix, char* out_name, size_t out_size)
{
  elf_dynsym_query q{libname, nullptr, nullptr, 0, 0, 0, false};
  dl_iterate_phdr(dynsym_iterate_cb, &q);
  if (!q.found)
    return nullptr;

  const size_t prefix_len = strlen(prefix);
  for (uint32_t i = 0; i < q.nsyms; ++i)
  {
    const ElfW(Sym)* sym = &q.symtab[i];
    if (sym->st_value == 0)
      continue;
    if (sym->st_name >= q.strtab_size)
      continue;
    if (ELF64_ST_TYPE(sym->st_info) != STT_FUNC)
      continue;
    const char* name = q.strtab + sym->st_name;
    if (strncmp(name, prefix, prefix_len) != 0)
      continue;
    if (out_name && out_size)
    {
      strncpy(out_name, name, out_size - 1);
      out_name[out_size - 1] = '\0';
    }
    return (void*) (q.load_bias + sym->st_value);
  }
  return nullptr;
}

struct loaded_lib_query
{
  const char* libname;
  char path[512];
  ElfW(Addr) load_bias;
  bool found;
};

static int loaded_lib_cb(struct dl_phdr_info* info, size_t, void* data)
{
  auto* q = (loaded_lib_query*) data;
  if (!loaded_object_matches(info, q->libname))
    return 0;

  if (info->dlpi_name && *info->dlpi_name)
  {
    strncpy(q->path, info->dlpi_name, sizeof(q->path) - 1);
  }
  else
  {
    ssize_t n = readlink(OBF("/proc/self/exe").c_str(), q->path, sizeof(q->path) - 1);
    if (n <= 0)
      return 0;
    q->path[n] = '\0';
  }
  q->path[sizeof(q->path) - 1] = '\0';
  q->load_bias = info->dlpi_addr;
  q->found = true;
  return 1;
}

static bool pread_exact(int fd, off_t off, void* buf, size_t size)
{
  auto* p = (uint8_t*) buf;
  while (size > 0)
  {
    ssize_t n = pread(fd, p, size, off);
    if (n <= 0)
      return false;
    p += n;
    off += n;
    size -= (size_t) n;
  }
  return true;
}

static void* find_file_sym_by_prefix(const char* libname, const char* prefix, char* out_name, size_t out_size)
{
  loaded_lib_query q{libname, {}, 0, false};
  dl_iterate_phdr(loaded_lib_cb, &q);
  if (!q.found || !q.path[0])
    return nullptr;

  int fd = open(q.path, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return nullptr;

  ElfW(Ehdr) eh{};
  if (!pread_exact(fd, 0, &eh, sizeof(eh)) || memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_ident[EI_DATA] != ELFDATA2LSB || eh.e_shoff == 0 || eh.e_shnum == 0 || eh.e_shentsize != sizeof(ElfW(Shdr)))
  {
    close(fd);
    return nullptr;
  }

  std::vector<ElfW(Shdr)> sections(eh.e_shnum);
  if (!pread_exact(fd, (off_t) eh.e_shoff, sections.data(), sections.size() * sizeof(ElfW(Shdr))))
  {
    close(fd);
    return nullptr;
  }

  const size_t prefix_len = strlen(prefix);
  for (const auto& sh : sections)
  {
    if (sh.sh_type != SHT_SYMTAB && sh.sh_type != SHT_DYNSYM)
      continue;
    if (sh.sh_link >= sections.size() || sh.sh_entsize < sizeof(ElfW(Sym)))
      continue;

    const auto& str_sh = sections[sh.sh_link];
    if (str_sh.sh_size == 0 || str_sh.sh_size > (16u * 1024u * 1024u))
      continue;

    std::vector<char> strtab((size_t) str_sh.sh_size + 1u, '\0');
    if (!pread_exact(fd, (off_t) str_sh.sh_offset, strtab.data(), (size_t) str_sh.sh_size))
    {
      continue;
    }

    const size_t sym_count = (size_t) (sh.sh_size / sh.sh_entsize);
    if (sym_count == 0 || sym_count > (2u * 1024u * 1024u))
      continue;

    std::vector<ElfW(Sym)> syms(sym_count);
    if (!pread_exact(fd, (off_t) sh.sh_offset, syms.data(), sym_count * sizeof(ElfW(Sym))))
    {
      continue;
    }

    for (const auto& sym : syms)
    {
      if (sym.st_value == 0)
        continue;
      if (ELF64_ST_TYPE(sym.st_info) != STT_FUNC)
        continue;
      if (sym.st_name >= strtab.size())
        continue;
      const char* name = strtab.data() + sym.st_name;
      if (strncmp(name, prefix, prefix_len) != 0)
        continue;
      if (out_name && out_size)
      {
        strncpy(out_name, name, out_size - 1);
        out_name[out_size - 1] = '\0';
      }
      close(fd);
      return (void*) (q.load_bias + sym.st_value);
    }
  }

  close(fd);
  return nullptr;
}

static bool resolve_gbm_in_lib(void* dl_handle, void* sh_handle, const char* lib_tag)
{
  if (!dl_handle && !sh_handle)
    return false;

  auto gbm_get = resolve_sym<GBM_get_fn>(dl_handle, sh_handle, "_ZN7android9SingletonINS_19GraphicBufferMapperEE11getInstanceEv");
  auto lockYCbCr = resolve_sym<GBM_lockYCbCr_fn>(dl_handle, sh_handle, "_ZN7android19GraphicBufferMapper9lockYCbCrEPK13native_handlejRKNS_4RectEP13android_ycbcr");
  auto unlock_v1 = resolve_sym<GBM_unlock_v1_fn>(dl_handle, sh_handle, OBF("_ZN7android19GraphicBufferMapper6unlockEPK13native_handle").c_str());
  auto unlock_v2 = resolve_sym<GBM_unlock_v2_fn>(dl_handle, sh_handle, "_ZN7android19GraphicBufferMapper6unlockEPK13native_handlePNS_4base9unique_fdE");

  auto plain_lock_v1 = resolve_sym<GBM_lock_v1_fn>(dl_handle, sh_handle, "_ZN7android19GraphicBufferMapper4lockEPK13native_handlejRKNS_4RectEPPv");

  char lock_v2_sym_buf[256] = {0};
  GBM_lock_v2_fn plain_lock_v2 = (GBM_lock_v2_fn) find_sym_by_prefix(lib_tag, "_ZN7android19GraphicBufferMapper4lockEPK13native_handlejRKNS_4RectEPPvPi", lock_v2_sym_buf, sizeof(lock_v2_sym_buf));
  const char* lock_v2_match = plain_lock_v2 ? lock_v2_sym_buf : nullptr;
  if (!plain_lock_v2)
  {
    static const char* const kV2Suffixes[] = {
        "S5_", "S6_", "S7_", "S8_", "S9_", "SA_", "SB_", "SC_",
    };
    for (const char* suf : kV2Suffixes)
    {
      char sym[256];
      int n = snprintf(sym, sizeof(sym), "_ZN7android19GraphicBufferMapper4lockEPK13native_handlejRKNS_4RectEPPvPi%s", suf);
      if (n <= 0 || (size_t) n >= sizeof(sym))
        continue;
      plain_lock_v2 = resolve_sym<GBM_lock_v2_fn>(dl_handle, sh_handle, sym);
      if (plain_lock_v2)
      {
        strncpy(lock_v2_sym_buf, sym, sizeof(lock_v2_sym_buf) - 1);
        lock_v2_match = lock_v2_sym_buf;
        break;
      }
    }
  }

  void* gbm_inst = gbm_get ? gbm_get() : nullptr;

  if (gbm_inst && lockYCbCr && (unlock_v1 || unlock_v2))
  {
    g_gbm = gbm_inst;
    g_gbm_lockYCbCr = lockYCbCr;
    g_gbm_unlock_v1 = unlock_v1;
    g_gbm_unlock_v2 = unlock_v2;
    g_gbm_lock_v1 = plain_lock_v1;
    g_gbm_lock_v2 = plain_lock_v2;
    LOGI(OBF("resolve: GBM picked from %s (this=%p)").c_str(), lib_tag, gbm_inst);
    LOGI(OBF("resolve: GBM plain lock v1=%p v2=%p%s%s").c_str(), (void*) plain_lock_v1, (void*) plain_lock_v2, lock_v2_match ? OBF(" v2_sym=").c_str() : "", lock_v2_match ? lock_v2_match : "");
    return true;
  }
  LOGW(OBF("resolve: GBM partial in %s — getInst=%p lockYCbCr=%p u_v1=%p u_v2=%p").c_str(), lib_tag, (void*) gbm_get, (void*) lockYCbCr, (void*) unlock_v1, (void*) unlock_v2);
  return false;
}

static void resolve_runtime_ops()
{
  void* libnw = dlopen(OBF("libnativewindow.so").c_str(), RTLD_NOW | RTLD_NOLOAD);
  if (!libnw)
    libnw = dlopen(OBF("libnativewindow.so").c_str(), RTLD_NOW);
  if (libnw)
  {
    g_ahb_lockPlanes = (AHB_lockPlanes_fn) dlsym(libnw, OBF("AHardwareBuffer_lockPlanes").c_str());
    g_ahb_unlock = (AHB_unlock_fn) dlsym(libnw, OBF("AHardwareBuffer_unlock").c_str());

    g_ahb_lock = (AHB_lock_fn) dlsym(libnw, OBF("AHardwareBuffer_lock").c_str());
  }

  static const char* const kGbmLibs[] = {
      "libui.so",
      "libgui.so",
  };
  for (auto* lib_name : kGbmLibs)
  {
    if (g_gbm)
      break;
    void* dl_handle = dlopen(lib_name, RTLD_NOW | RTLD_NOLOAD);
    if (!dl_handle)
      dl_handle = dlopen(lib_name, RTLD_NOW);

    void* sh_handle = shadowhook_dlopen(lib_name);
    if (!dl_handle && !sh_handle)
    {
      LOGW(OBF("resolve: %s not loadable (no dl + no sh handle)").c_str(), lib_name);
      continue;
    }
    resolve_gbm_in_lib(dl_handle, sh_handle, lib_name);
    if (sh_handle)
      shadowhook_dlclose(sh_handle);
  }

  LOGI(OBF("resolve: AHB lock=%p unlock=%p | GBM this=%p lockYCbCr=%p unlock_v1=%p unlock_v2=%p").c_str(), g_ahb_lockPlanes, g_ahb_unlock, g_gbm, g_gbm_lockYCbCr, g_gbm_unlock_v1, g_gbm_unlock_v2);
}

struct ANativeBaseHeader
{
  int32_t magic;
  int32_t version;
  void* reserved[4];
  void* incRef;
  void* decRef;
};

struct ANWB
{
  ANativeBaseHeader common;
  int32_t width;
  int32_t height;
  int32_t stride;
  int32_t format;
  int32_t usage_deprecated;
  int32_t _pad;
  uintptr_t layerCount;
  void* reserved_ptr[1];
  const void* handle;
  uint64_t usage;
};

static_assert(sizeof(ANativeBaseHeader) == 56, "ANB common" /*OBF_SKIP*/);
static_assert(offsetof(ANWB, width) == 56, "ANWB width" /*OBF_SKIP*/);
static_assert(offsetof(ANWB, height) == 60, "ANWB height" /*OBF_SKIP*/);
static_assert(offsetof(ANWB, stride) == 64, "ANWB stride" /*OBF_SKIP*/);
static_assert(offsetof(ANWB, format) == 68, "ANWB format" /*OBF_SKIP*/);
static_assert(offsetof(ANWB, handle) == 96, "ANWB handle" /*OBF_SKIP*/);

constexpr int32_t kAndroidNativeBufferMagic = ((int32_t) '_' << 24) | ((int32_t) 'b' << 16) | ((int32_t) 'f' << 8) | (int32_t) 'r';

struct ANWBInfo
{
  int32_t width = 0;
  int32_t height = 0;
  int32_t stride = 0;
  int32_t format = 0;
  const void* handle = nullptr;
};

static std::atomic<int> g_stat_skip_noshm{0};
static std::atomic<int> g_stat_skip_channel{0};
static std::atomic<int> g_stat_skip_noframe{0};
static std::atomic<int> g_stat_skip_layout{0};
static std::atomic<int> g_stat_skip_nogbm{0};
static std::atomic<int> g_stat_skip_noanb{0};
static std::atomic<int> g_stat_skip_dims{0};
static std::atomic<int> g_stat_skip_fmt{0};
static std::atomic<int> g_stat_skip_handle{0};
static std::atomic<int> g_stat_lock_fail{0};
static std::atomic<int> g_stat_no_vu{0};
static std::atomic<int> g_stat_blit_ok{0};

static bool inspect_anwb(const ANWB* anb, ANWBInfo& out)
{
  if (!anb)
  {
    ++g_stat_skip_noanb;
    return false;
  }
  const int32_t version = anb->common.version;
  if (anb->common.magic != kAndroidNativeBufferMagic || version < (int32_t) (offsetof(ANWB, handle) + sizeof(anb->handle)) || version > 4096)
  {
    int n = ++g_stat_skip_layout;
    if (n <= 5 || (n % 60) == 0)
    {
      LOGW(OBF("ANWB layout rejected magic=0x%x version=%d anb=%p").c_str(), anb->common.magic, version, anb);
    }
    return false;
  }

  out.width = anb->width;
  out.height = anb->height;
  out.stride = anb->stride;
  out.format = anb->format;
  out.handle = anb->handle;
  if (out.width < 1 || out.height < 1)
  {
    ++g_stat_skip_dims;
    return false;
  }
  if (!out.handle)
  {
    ++g_stat_skip_handle;
    return false;
  }
  return true;
}

static void try_lazy_init()
{
  if (!g_shm.header)
  {
    static std::atomic<int> n{0};
    if ((n.fetch_add(1, std::memory_order_relaxed) % 30) == 0)
    {
      open_shm();
    }
  }
  if (!g_ahb_lockPlanes && !gbm_ready())
  {
    static std::atomic<int> n{0};
    if ((n.fetch_add(1, std::memory_order_relaxed) % 30) == 0)
    {
      resolve_runtime_ops();
    }
  }
}

struct LockedView
{
  uint8_t* y = nullptr;
  uint8_t* vu = nullptr;
  size_t ystride = 0;
  size_t uvstride = 0;
  bool uv_swapped = false;
  int lock_path = -1;
};

static bool lock_via_ahb(const void* anb, int W, int H, void*& out_handle, LockedView& view)
{
  if (!g_ahb_lockPlanes || !g_ahb_unlock)
    return false;
  void* ahb = (void*) anb;
  CR_ARect rect{0, 0, W, H};
  CR_AHB_Planes planes{};
  int rc = g_ahb_lockPlanes(ahb, kAhbUsageCpuRW, -1, &rect, &planes);
  if (rc != 0)
    return false;
  if (planes.planeCount < 3)
  {
    g_ahb_unlock(ahb, nullptr);
    return false;
  }

  auto& p0 = planes.planes[0];
  auto& p1 = planes.planes[1];
  auto& p2 = planes.planes[2];

  view.y = (uint8_t*) p0.data;
  view.ystride = p0.rowStride;
  view.uvstride = p1.rowStride;
  if (p1.pixelStride != 2)
  {
    g_ahb_unlock(ahb, nullptr);
    return false;
  }
  if (p2.data < p1.data)
  {
    view.vu = (uint8_t*) p2.data;
    view.uv_swapped = false;
  }
  else
  {
    view.vu = (uint8_t*) p1.data;
    view.uv_swapped = true;
  }
  view.lock_path = 0;
  out_handle = ahb;
  return true;
}

static bool lock_via_gbm(const void* anb, const void* handle, int W, int H, LockedView& view)
{
  if (!gbm_ready())
    return false;
  Rect bounds{0, 0, W, H};
  android_ycbcr yc = {};

  int rc = g_gbm_lockYCbCr(g_gbm, handle, 0x33u, bounds, &yc);
  if (rc != 0)
    return false;

  view.y = (uint8_t*) yc.y;
  view.ystride = yc.ystride;
  view.uvstride = yc.cstride;

  if (yc.chroma_step == 2 && yc.cb && yc.cr)
  {
    if (yc.cr < yc.cb)
    {
      view.vu = (uint8_t*) yc.cr;
      view.uv_swapped = false;
    }
    else
    {
      view.vu = (uint8_t*) yc.cb;
      view.uv_swapped = true;
    }
  }
  else
  {
    gbm_call_unlock(handle);
    return false;
  }
  view.lock_path = 1;
  (void) anb;
  return true;
}

static void unlock(const void* anb, const void* handle, void* ahb, const LockedView& view)
{
  if (view.lock_path == 0 && ahb && g_ahb_unlock)
  {
    g_ahb_unlock(ahb, nullptr);
  }
  else if (view.lock_path == 1)
  {
    gbm_call_unlock(handle);
  }
  (void) anb;
}

static std::atomic<int> g_stat_blob_lock_fail{0};
static std::atomic<int> g_stat_blob_too_big{0};
static std::atomic<int> g_stat_blob_ok{0};
static std::atomic<int> g_stat_blob_cache_hit{0};

static std::mutex g_jpeg_cache_mutex;
static std::vector<uint8_t> g_jpeg_cache;
static uint64_t g_jpeg_cache_frame = 0;
static int g_jpeg_cache_src_w = 0;
static int g_jpeg_cache_src_h = 0;
static int64_t g_jpeg_cache_ts_ns = 0;

static bool lock_blob(const void* anb, const void* handle, int W, int H, void*& out_vaddr, void*& out_ahb_handle, int& out_lock_path)
{
  out_vaddr = nullptr;
  out_ahb_handle = nullptr;
  out_lock_path = -1;

  if (g_ahb_lock)
  {
    void* vaddr = nullptr;
    CR_ARect rect{0, 0, W, H};
    int rc = g_ahb_lock((void*) anb, kAhbUsageCpuRW, -1, &rect, &vaddr);
    if (rc == 0 && vaddr)
    {
      out_vaddr = vaddr;
      out_ahb_handle = (void*) anb;
      out_lock_path = 0;
      return true;
    }
  }
  if (g_gbm && g_gbm_lock_v2)
  {
    Rect bounds{0, 0, W, H};
    void* vaddr = nullptr;
    int rc = g_gbm_lock_v2(g_gbm, handle, 0x33u, bounds, &vaddr, nullptr, nullptr);
    if (rc == 0 && vaddr)
    {
      out_vaddr = vaddr;
      out_lock_path = 1;
      return true;
    }
  }
  if (g_gbm && g_gbm_lock_v1)
  {
    Rect bounds{0, 0, W, H};
    void* vaddr = nullptr;
    int rc = g_gbm_lock_v1(g_gbm, handle, 0x33u, bounds, &vaddr);
    if (rc == 0 && vaddr)
    {
      out_vaddr = vaddr;
      out_lock_path = 1;
      return true;
    }
  }
  return false;
}

static bool overwrite_blob_jpeg(ANWB* anb)
{
  if (!g_shm.header)
    return false;
  if (!feed_header_sane(g_shm.header, g_shm.size))
    return false;
  const uint64_t cur_frame = atomic_load(&g_shm.header->frame_counter);
  if (cur_frame == 0)
    return false;

  const int src_w = (int) g_shm.header->width;
  const int src_h = (int) g_shm.header->height;
  if (src_w < 16 || src_h < 16)
    return false;

  const int W = anb->width;
  const int H = anb->height;
  if (W < 16 || W > 64 * 1024 * 1024)
    return false;
  if (H < 1 || H > 8192)
    return false;
  const size_t buf_capacity = (size_t) W * (size_t) H;
  const size_t trailer_size = sizeof(CR_Camera3JpegBlob);
  if (buf_capacity < trailer_size + 1024)
    return false;
  const size_t encode_cap = buf_capacity - trailer_size;

  const void* handle = anb->handle;
  if (!handle)
    return false;

  void* vaddr = nullptr;
  void* ahb_handle = nullptr;
  int lock_path = -1;
  if (!lock_blob(anb, handle, W, H, vaddr, ahb_handle, lock_path))
  {
    int n = ++g_stat_blob_lock_fail;
    if (n <= 5 || (n % 60) == 0)
    {
      LOGW(OBF("blob lock failed (%dx%d cap=%zu fmt=0x%x ahb_lock=%p gbm_lock_v1=%p gbm_lock_v2=%p)").c_str(), W, H, buf_capacity, anb->format, (void*) g_ahb_lock, (void*) g_gbm_lock_v1, (void*) g_gbm_lock_v2);
    }
    return false;
  }

  bool cache_hit = false;
  size_t jpeg_bytes = 0;
  const int64_t now_ns = monotonic_ns();
  auto unlock_blob = [&]()
  {
    if (lock_path == 0 && g_ahb_unlock)
      g_ahb_unlock(ahb_handle, nullptr);
    else if (lock_path == 1)
      gbm_call_unlock(handle);
  };
  bool need_encode = true;
  {
    std::lock_guard<std::mutex> g(g_jpeg_cache_mutex);

    const bool cache_valid = !g_jpeg_cache.empty() && g_jpeg_cache.size() <= encode_cap && g_jpeg_cache_src_w == src_w && g_jpeg_cache_src_h == src_h;

    const bool same_frame = cache_valid && g_jpeg_cache_frame == cur_frame;
    if (same_frame)
    {
      jpeg_bytes = g_jpeg_cache.size();
      memcpy(vaddr, g_jpeg_cache.data(), jpeg_bytes);
      cache_hit = true;
      ++g_stat_blob_cache_hit;
      need_encode = false;
    }
  }

  if (need_encode)
  {
    const uint8_t* src_slot = g_shm.current_slot();
    if (!src_slot)
      src_slot = g_shm.slot0;
    int rc = cr_nv21_to_jpeg(src_slot, src_w, src_h, (uint8_t*) vaddr, encode_cap, 85, &jpeg_bytes);
    if (rc != 0 || jpeg_bytes == 0)
    {
      int n = ++g_stat_blob_too_big;
      if (n <= 5 || (n % 60) == 0)
      {
        LOGW(OBF("blob: nv21->jpeg failed rc=%d jpeg=%zu cap=%zu (src %dx%d -> buf %dx%d)").c_str(), rc, jpeg_bytes, encode_cap, src_w, src_h, W, H);
      }
      unlock_blob();
      return false;
    }

    std::lock_guard<std::mutex> g(g_jpeg_cache_mutex);
    g_jpeg_cache.assign((const uint8_t*) vaddr, (const uint8_t*) vaddr + jpeg_bytes);
    g_jpeg_cache_frame = cur_frame;
    g_jpeg_cache_src_w = src_w;
    g_jpeg_cache_src_h = src_h;
    g_jpeg_cache_ts_ns = now_ns;
  }

  CR_Camera3JpegBlob trailer;
  trailer.blob_id = CR_CAMERA3_JPEG_BLOB_ID;
  trailer.jpeg_size = (uint32_t) jpeg_bytes;
  memcpy((uint8_t*) vaddr + buf_capacity - trailer_size, &trailer, sizeof(trailer));

  unlock_blob();

  int n = ++g_stat_blob_ok;
  if (n <= 5 || (n % 30) == 0)
  {
    LOGI(OBF("blob#%d substituted: %zu JPEG bytes (src %dx%d -> buf %dx%d) via=%s %s").c_str(), n, jpeg_bytes, src_w, src_h, W, H, lock_path == 0 ? OBF("AHB").c_str() : OBF("GBM").c_str(), cache_hit ? OBF("(cache hit)").c_str() : OBF("(re-encoded)").c_str());
  }
  return true;
}

static void overwrite_nv21(ANWB* anb)
{
  ANWBInfo bi;
  if (!inspect_anwb(anb, bi))
    return;

  if (!g_shm.header || (!g_ahb_lockPlanes && !gbm_ready()))
  {
    try_lazy_init();
  }
  if (!g_shm.header)
  {
    ++g_stat_skip_noshm;
    return;
  }
  if (!feed_header_sane(g_shm.header, g_shm.size))
    return;

  const uint32_t channel_state = atomic_load(&g_shm.header->channel_state);
  if (channel_state != CR_CHANNEL_STATE_READY)
  {
    int n = ++g_stat_skip_channel;
    if (n <= 5 || (n % 60) == 0)
    {
      LOGW(OBF("skip_channel#%d state=%u frame=%llu gen=%u magic=0x%x").c_str(),
           n,
           channel_state,
           (unsigned long long) atomic_load(&g_shm.header->frame_counter),
           atomic_load(&g_shm.header->generation),
           g_shm.header->magic);
    }
    return;
  }

  if (atomic_load(&g_shm.header->frame_counter) == 0)
  {
    int n = ++g_stat_skip_noframe;
    if (n <= 5 || (n % 60) == 0)
    {
      LOGW(OBF("skip_noframe#%d state=%u dims=%ux%u gen=%u").c_str(),
           n,
           channel_state,
           g_shm.header->width,
           g_shm.header->height,
           atomic_load(&g_shm.header->generation));
    }
    return;
  }

  const int fmt = bi.format;

  if (fmt == 0x21)
  {
    const bool photo_on = refresh_photo_mode_();
    static std::atomic<int> blob_seen{0};
    int bn = ++blob_seen;
    if (bn <= 5 || (bn % 30) == 0)
    {
      LOGI(OBF("BLOB qbtc#%d %dx%d photo_mode=%s").c_str(), bn, bi.width, bi.height, photo_on ? "ON" : OBF("OFF").c_str());
    }
    if (photo_on)
    {
      overwrite_blob_jpeg(anb);
    }
    return;
  }

  if (!g_ahb_lockPlanes && !gbm_ready())
  {
    ++g_stat_skip_nogbm;
    return;
  }

  const int W = bi.width;
  const int H = bi.height;
  if (W < 16 || H < 16 || W > 8192 || H > 8192)
  {
    ++g_stat_skip_dims;
    return;
  }

  if (fmt != 0x11 && fmt != 0x22 && fmt != 0x23)
  {
    ++g_stat_skip_fmt;
    return;
  }

  const void* handle = bi.handle;

  LockedView view;
  void* ahb = nullptr;
  bool locked = lock_via_ahb(anb, W, H, ahb, view);
  if (!locked)
  {
    locked = lock_via_gbm(anb, handle, W, H, view);
  }
  if (!locked)
  {
    int n = ++g_stat_lock_fail;
    if (n <= 5 || (n % 60) == 0)
    {
      LOGW(OBF("lock failed (%dx%d fmt=0x%x handle=%p)").c_str(), W, H, fmt, handle);
    }
    return;
  }

  if (view.y && view.vu)
  {
    int n = ++g_stat_blit_ok;
    if (n <= 3 || (n % 60) == 0)
    {
      LOGI(OBF("blit#%d %dx%d via=%s y=%p vu=%p ys=%zu us=%zu order=%s (src %ux%u)").c_str(), n, W, H, view.lock_path == 0 ? OBF("AHB").c_str() : OBF("GBM").c_str(), view.y, view.vu, view.ystride, view.uvstride, view.uv_swapped ? OBF("NV12(U-first)").c_str() : OBF("NV21(V-first)").c_str(), g_shm.header->width, g_shm.header->height);
    }

    const uint8_t* src_slot = g_shm.current_slot();
    if (!src_slot)
      src_slot = g_shm.slot0;

    blit_adaptive(src_slot, (int) g_shm.header->width, (int) g_shm.header->height, view.y, view.vu, W, H, (int) view.ystride, (int) view.uvstride, view.uv_swapped ? 1 : 0);
  }
  else
  {
    ++g_stat_no_vu;
  }

  unlock(anb, handle, ahb, view);
}

typedef void (*qbtc_t)(void* thiz, void* consumer, ANWB* anb, int fence, void* surface_ids);

static qbtc_t g_orig_qbtc = nullptr;

static void proxy_qbtc(void* thiz, void* consumer, ANWB* anb, int fence, void* surface_ids)
{
  static std::atomic<int> call_count{0};
  int n = ++call_count;
  if (n <= 5 || (n % 60) == 0)
  {
    ANWBInfo bi{};
    const bool layout_ok = inspect_anwb(anb, bi);
    LOGI(OBF("qbtc#%d anb=%p %dx%d stride=%d fmt=0x%x handle=%p").c_str(), n, anb, layout_ok ? bi.width : -1, layout_ok ? bi.height : -1, layout_ok ? bi.stride : -1, layout_ok ? bi.format : -1, layout_ok ? bi.handle : nullptr);
  }

  overwrite_nv21(anb);

  if ((n % 60) == 0)
  {
    LOGI(OBF("stats: skip_channel=%d skip_noframe=%d skip_noshm=%d skip_fmt=%d skip_lock=%d blit_ok=%d blob_ok=%d blob_cache_hit=%d noanb=%d layout=%d nogbm=%d dims=%d hnd=%d novu=%d").c_str(),
         g_stat_skip_channel.load(),
         g_stat_skip_noframe.load(),
         g_stat_skip_noshm.load(),
         g_stat_skip_fmt.load(),
         g_stat_lock_fail.load(),
         g_stat_blit_ok.load(),
         g_stat_blob_ok.load(),
         g_stat_blob_cache_hit.load(),
         g_stat_skip_noanb.load(),
         g_stat_skip_layout.load(),
         g_stat_skip_nogbm.load(),
         g_stat_skip_dims.load(),
         g_stat_skip_handle.load(),
         g_stat_no_vu.load());
  }

  if (g_orig_qbtc)
    g_orig_qbtc(thiz, consumer, anb, fence, surface_ids);
}

typedef uint64_t (*rbcl_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

static rbcl_t g_orig_rbcl = nullptr;

static uint64_t proxy_rbcl(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
  static std::atomic<int> call_count{0};
  int n = ++call_count;
  if (n <= 5 || (n % 60) == 0)
    LOGI(OBF("rbcl#%d (fallback pass-through)").c_str(), n);
  return g_orig_rbcl ? g_orig_rbcl(a0, a1, a2, a3, a4, a5, a6, a7) : 0;
}

static int try_hook_primary()
{
  shadowhook_init(SHADOWHOOK_MODE_UNIQUE, false);

  auto try_one = [&](const char* sym) -> bool
  {
    auto libcameraservice = OBF("libcameraservice.so");
    auto cameraserver = OBF("cameraserver");
    const char* libs[] = {
        libcameraservice.c_str(),
        cameraserver.c_str(),
    };
    for (const char* lib : libs)
    {
      void* stub = shadowhook_hook_sym_name(lib, sym, (void*) proxy_qbtc, (void**) &g_orig_qbtc);
      if (stub)
      {
        g_hook_stub = stub;
        LOGI(OBF("hook installed on queueBufferToConsumer (lib=%s sym=%s)").c_str(), lib, sym);
        return true;
      }
    }
    LOGW(OBF("qbtc try %.40s… → %s").c_str(), sym, shadowhook_to_errmsg(shadowhook_get_errno()));
    return false;
  };
  auto try_prefix = [&]() -> bool
  {
    auto try_prefix_in = [&](const char* lib) -> bool
    {
      char matched[512] = {};
      char source[16] = {};
      void* addr = find_sym_by_prefix(lib, OBF("_ZN7android7camera319Camera3OutputStream21queueBufferToConsumer").c_str(), matched, sizeof(matched));
      strncpy(source, OBF("dynsym").c_str(), sizeof(source) - 1);
      if (!addr)
      {
        addr = find_file_sym_by_prefix(lib, OBF("_ZN7android7camera319Camera3OutputStream21queueBufferToConsumer").c_str(), matched, sizeof(matched));
        strncpy(source, OBF("symtab").c_str(), sizeof(source) - 1);
      }
      if (!addr)
        return false;

      void* stub = shadowhook_hook_sym_addr(addr, (void*) proxy_qbtc, (void**) &g_orig_qbtc);
      if (!stub)
      {
        LOGW(OBF("qbtc prefix %s/%s matched %.80s at %p but hook failed: %s").c_str(), lib, source, matched, addr, shadowhook_to_errmsg(shadowhook_get_errno()));
        return false;
      }
      g_hook_stub = stub;
      LOGI(OBF("hook installed on queueBufferToConsumer (lib=%s %s prefix sym=%s addr=%p)").c_str(), lib, source, matched, addr);
      return true;
    };

    if (try_prefix_in(OBF("libcameraservice.so").c_str()))
      return true;
    if (try_prefix_in(OBF("cameraserver").c_str()))
      return true;
    return false;
  };

  if (try_one(OBF("_ZN7android7camera319Camera3OutputStream21queueBufferToConsumerERNS_2spI13ANativeWindowEEP19ANativeWindowBufferiRKNSt3__16vectorImNS8_"
                  "9allocatorImEEEE")
                  .c_str()))
    return 0;

  if (try_one(OBF("_ZN7android7camera319Camera3OutputStream21queueBufferToConsumerERNS_2spI13ANativeWindowEEP19ANativeWindowBufferiRKNSt3__16vectorImNS5_"
                  "9allocatorImEEEE")
                  .c_str()))
    return 0;

  if (try_one(OBF("_ZN7android7camera319Camera3OutputStream21queueBufferToConsumerERNS_2spI13ANativeWindowEEP19ANativeWindowBufferiRKNSt3__16vectorImNS6_"
                  "9allocatorImEEEE")
                  .c_str()))
    return 0;

  if (try_one(OBF("_ZN7android7camera319Camera3OutputStream21queueBufferToConsumerERNS_2spI13ANativeWindowEEP19ANativeWindowBufferiRKNSt3__16vectorImNS7_"
                  "9allocatorImEEEE")
                  .c_str()))
    return 0;

  if (try_one(OBF("_ZN7android7camera319Camera3OutputStream21queueBufferToConsumerERNS_2spI13ANativeWindowEEP19ANativeWindowBufferi").c_str()))
    return 0;

  if (try_prefix())
    return 0;

  return -1;
}

[[maybe_unused]] static int try_hook_fallback()
{
  shadowhook_init(SHADOWHOOK_MODE_UNIQUE, false);

  auto try_one = [&](const char* sym) -> bool
  {
    auto libcameraservice = OBF("libcameraservice.so");
    auto cameraserver = OBF("cameraserver");
    const char* libs[] = {
        libcameraservice.c_str(),
        cameraserver.c_str(),
    };
    for (const char* lib : libs)
    {
      void* stub = shadowhook_hook_sym_name(lib, sym, (void*) proxy_rbcl, (void**) &g_orig_rbcl);
      if (stub)
      {
        g_hook_stub = stub;
        LOGI(OBF("hook installed on returnBufferCheckedLocked fallback (lib=%s)").c_str(), lib);
        return true;
      }
    }
    return false;
  };

  auto try_prefix = [&]() -> bool
  {
    auto try_prefix_in = [&](const char* lib) -> bool
    {
      char matched[512] = {};
      void* addr = find_file_sym_by_prefix(lib, OBF("_ZN7android7camera319Camera3OutputStream25returnBufferCheckedLocked").c_str(), matched, sizeof(matched));
      if (!addr)
        return false;

      void* stub = shadowhook_hook_sym_addr(addr, (void*) proxy_rbcl, (void**) &g_orig_rbcl);
      if (!stub)
        return false;
      g_hook_stub = stub;
      LOGI(OBF("hook installed on returnBufferCheckedLocked fallback (lib=%s sym=%s addr=%p)").c_str(), lib, matched, addr);
      return true;
    };
    if (try_prefix_in(OBF("libcameraservice.so").c_str()))
      return true;
    if (try_prefix_in(OBF("cameraserver").c_str()))
      return true;
    return false;
  };

  if (try_one(OBF("_ZN7android7camera319Camera3OutputStream25returnBufferCheckedLockedERKNS0_20camera_stream_bufferElbiRKNSt3__16vectorImNS8_9allocatorImEEEEPN"
                  "S_2spINS_5FenceEEE")
                  .c_str()))
    return 0;

  if (try_prefix())
    return 0;

  LOGE(OBF("hook fallback: %s").c_str(), shadowhook_to_errmsg(shadowhook_get_errno()));
  return -1;
}
extern "C" __attribute__((visibility("default" /*OBF_SKIP*/))) int cr_camhook_shutdown();

extern "C" __attribute__((visibility("default" /*OBF_SKIP*/))) int cr_camhook_init(const char* feed_url)
{
  if (g_installed.exchange(true))
  {
    LOGI(OBF("already installed").c_str());
    return 0;
  }
  LOGI(OBF("cr_camhook_init feed_url=%s").c_str(), feed_url ? feed_url : OBF("(null)").c_str());

  resolve_scaler();
  resolve_photo_mode();
  open_shm();
  resolve_runtime_ops();

  int hook_rc = -1;
  if (try_hook_primary() == 0)
  {
    hook_rc = 0;
  }
  else
  {
    LOGE(OBF("primary queueBufferToConsumer hook failed; diagnostic fallback is not production success").c_str());
#if defined(CR_CAMHOOK_DIAGNOSTIC_FALLBACK)
    (void) try_hook_fallback();
#endif
  }

  if (hook_rc == 0)
  {
    mkdir(OBF("/data/cr").c_str(), 0755);
    chmod(OBF("/data/cr").c_str(), 0777);
    int fd = open(OBF("/data/cr/camhook.ready").c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
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

  LOGE(OBF("no supported camera symbol found — hook not installed").c_str());
  g_installed = false;
  return -1;
}

extern "C" __attribute__((visibility("default" /*OBF_SKIP*/))) int cr_camhook_shutdown()
{
  if (!g_installed.exchange(false))
    return 0;
  if (g_hook_stub)
  {
    shadowhook_unhook(g_hook_stub);
    g_hook_stub = nullptr;
  }
  if (g_shm.fd >= 0)
  {
    munmap((void*) g_shm.header, g_shm.size);
    close(g_shm.fd);
    g_shm = {};
  }
  LOGI(OBF("cr_camhook_shutdown done").c_str());
  return 0;
}
