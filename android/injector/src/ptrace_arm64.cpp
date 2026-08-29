#include "ptrace_arm64.h"

// NDK's fortify layer redeclares stdio/stdlib outside ::std, so we use the
// global C names directly.
#include <android/log.h>
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util/Obf.h"

#define TAG "cr_injector"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace cr::inj
{

// Attach / detach / wait

// Wait for the traced child to become traced-stopped. Returns the status or
// -1 if something went wrong. Logs a message for every status change so the
// post-mortem trail from logcat tells the story.
static int wait_stop(pid_t pid)
{
  int status = 0;
  if (waitpid(pid, &status, __WALL) < 0)
  {
    LOGE(OBF("waitpid(%d) failed: %s").c_str(), pid, strerror(errno));
    return -1;
  }
  if (WIFSTOPPED(status))
    return status;
  LOGE(OBF("unexpected wait status 0x%x on pid=%d").c_str(), status, pid);
  return -1;
}

int attach(pid_t pid)
{
  if (ptrace(PTRACE_ATTACH, pid, 0, 0) < 0)
  {
    LOGE(OBF("PTRACE_ATTACH(%d): %s").c_str(), pid, strerror(errno));
    return -errno;
  }
  if (wait_stop(pid) < 0)
  {
    LOGW(OBF("attach wait failed for pid=%d; attempting best-effort detach").c_str(), pid);
    detach(pid);
    return -EIO;
  }
  LOGI(OBF("attached to pid=%d").c_str(), pid);
  return 0;
}

void detach(pid_t pid)
{
  if (ptrace(PTRACE_DETACH, pid, 0, 0) < 0)
  {
    LOGW(OBF("PTRACE_DETACH(%d): %s (will still try to recover)").c_str(), pid, strerror(errno));
  }
  else
  {
    LOGI(OBF("detached from pid=%d").c_str(), pid);
  }
}

// Register I/O

int get_regs(pid_t pid, RemoteRegs& out)
{
  struct iovec iov
  {
    &out, sizeof(out)
  };
  if (ptrace(PTRACE_GETREGSET, pid, (void*) NT_PRSTATUS, &iov) < 0)
  {
    LOGE(OBF("GETREGSET: %s").c_str(), strerror(errno));
    return -errno;
  }
  return 0;
}

int set_regs(pid_t pid, const RemoteRegs& r)
{
  struct iovec iov
  {
    const_cast<RemoteRegs*>(&r), sizeof(r)
  };
  if (ptrace(PTRACE_SETREGSET, pid, (void*) NT_PRSTATUS, &iov) < 0)
  {
    LOGE(OBF("SETREGSET: %s").c_str(), strerror(errno));
    return -errno;
  }
  return 0;
}

// Remote memory — 8-byte PEEK/POKE chunks

int peek_mem(pid_t pid, uintptr_t addr, void* out, size_t len)
{
  auto* dst = static_cast<uint8_t*>(out);
  while (len > 0)
  {
    errno = 0;
    long w = ptrace(PTRACE_PEEKDATA, pid, (void*) addr, 0);
    if (errno)
    {
      LOGE(OBF("PEEKDATA @%#llx: %s").c_str(), (unsigned long long) addr, strerror(errno));
      return -errno;
    }
    size_t n = len < 8 ? len : 8;
    memcpy(dst, &w, n);
    dst += n;
    addr += n;
    len -= n;
  }
  return 0;
}

int poke_mem(pid_t pid, uintptr_t addr, const void* in, size_t len)
{
  const auto* src = static_cast<const uint8_t*>(in);
  while (len > 0)
  {
    uint64_t word = 0;
    if (len < 8)
    {
      // Preserve the bytes past our write range by reading first.
      errno = 0;
      long w = ptrace(PTRACE_PEEKDATA, pid, (void*) addr, 0);
      if (errno)
      {
        LOGE(OBF("PEEKDATA-for-merge @%#llx: %s").c_str(), (unsigned long long) addr, strerror(errno));
        return -errno;
      }
      word = (uint64_t) w;
      memcpy(&word, src, len);
    }
    else
    {
      memcpy(&word, src, 8);
    }
    if (ptrace(PTRACE_POKEDATA, pid, (void*) addr, word) < 0)
    {
      LOGE(OBF("POKEDATA @%#llx: %s").c_str(), (unsigned long long) addr, strerror(errno));
      return -errno;
    }
    size_t n = len < 8 ? len : 8;
    src += n;
    addr += n;
    len -= n;
  }
  return 0;
}

static bool get_stop_siginfo(pid_t pid, siginfo_t& si) noexcept
{
  memset(&si, 0, sizeof(si));
  if (ptrace(PTRACE_GETSIGINFO, pid, 0, &si) < 0)
  {
    LOGW(OBF("PTRACE_GETSIGINFO(%d): %s").c_str(), pid, strerror(errno));
    return false;
  }
  return true;
}

static bool expected_return_trap(int sig, const RemoteRegs& after, const siginfo_t* si, uintptr_t return_trap) noexcept
{
  const uintptr_t fault_addr = si ? (uintptr_t) si->si_addr : 0;
  if (return_trap == 0)
    return (sig == SIGSEGV || sig == SIGBUS) && (after.pc == 0 || fault_addr == 0);
  if (sig != SIGTRAP && sig != SIGSEGV && sig != SIGBUS && sig != SIGILL)
    return false;
  return after.pc == return_trap || fault_addr == return_trap;
}

static bool trap_like(int sig) noexcept
{
  return sig == SIGSEGV || sig == SIGILL || sig == SIGBUS || sig == SIGTRAP;
}

// Remote function call.

int remote_call(pid_t pid, const RemoteRegs& base_regs, const char* stage, uintptr_t fn, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t return_trap, uintptr_t call_sp, uint64_t* ret_x0)
{
  RemoteRegs r = base_regs;
  r.sp = (call_sp ? call_sp : base_regs.sp) & ~0xfULL;
  r.x[0] = a0;
  r.x[1] = a1;
  r.x[2] = a2;
  r.x[3] = a3;
  r.x[30] = return_trap;
  r.pc = fn;

  if (set_regs(pid, r) < 0)
    return -EIO;

  if (ptrace(PTRACE_CONT, pid, 0, 0) < 0)
  {
    LOGE(OBF("PTRACE_CONT stage=%s: %s").c_str(), stage ? stage : "?", strerror(errno));
    return -errno;
  }

  for (int stop_count = 0; stop_count < 64; ++stop_count)
  {
    int status = wait_stop(pid);
    if (status < 0)
      return -EIO;

    const int sig = WSTOPSIG(status);
    RemoteRegs after{};
    if (get_regs(pid, after) < 0)
      return -EIO;

    siginfo_t si{};
    const bool have_si = get_stop_siginfo(pid, si);
    const uintptr_t fault_addr = have_si ? (uintptr_t) si.si_addr : 0;
    const bool expected = expected_return_trap(sig, after, have_si ? &si : nullptr, return_trap);
    if (trap_like(sig))
    {
      LOGI(OBF("remote_call[%s] trap sig=%d pc=%#llx lr=%#llx sp=%#llx x0=%#llx si_addr=%#llx expected=%#llx match=%d").c_str(),
           stage ? stage : "?",
           sig,
           (unsigned long long) after.pc,
           (unsigned long long) after.x[30],
           (unsigned long long) after.sp,
           (unsigned long long) after.x[0],
           (unsigned long long) fault_addr,
           (unsigned long long) return_trap,
           expected ? 1 : 0);
      if (expected)
      {
        if (ret_x0)
          *ret_x0 = after.x[0];
        return 0;
      }
      return -EFAULT;
    }

    LOGW(OBF("remote_call[%s]: async stop signal %d (status=0x%x pc=%#llx lr=%#llx sp=%#llx x0=%#llx si_addr=%#llx) - continuing").c_str(),
         stage ? stage : "?",
         sig,
         status,
         (unsigned long long) after.pc,
         (unsigned long long) after.x[30],
         (unsigned long long) after.sp,
         (unsigned long long) after.x[0],
         (unsigned long long) fault_addr);
    const int deliver = (sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU) ? 0 : sig;
    if (ptrace(PTRACE_CONT, pid, 0, (void*) (uintptr_t) deliver) < 0)
    {
      LOGE(OBF("PTRACE_CONT stage=%s after async signal %d: %s").c_str(), stage ? stage : "?", sig, strerror(errno));
      return -errno;
    }
  }

  LOGE(OBF("remote_call[%s]: too many async stops before return trap").c_str(), stage ? stage : "?");
  return -EIO;
}

// /proc/<pid>/maps parser for SO base address

// Find the first executable mapping of a .so whose basename matches `needle`.
// Returns the segment start address, or 0 if not found.
static uintptr_t maps_base(pid_t pid, const char* needle)
{
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/maps", pid);
  FILE* f = fopen(path, "re");
  if (!f)
  {
    LOGE(OBF("fopen(%s): %s").c_str(), path, strerror(errno));
    return 0;
  }

  uintptr_t base = 0;
  char line[512];
  while (fgets(line, sizeof(line), f))
  {
    uintptr_t start = 0, end = 0;
    char perms[8] = {};
    long off = 0;
    int dev_maj = 0, dev_min = 0;
    long inode = 0;
    char file[384] = {};
    int n = sscanf(line, "%" SCNxPTR "-%" SCNxPTR " %7s %lx %x:%x %ld %383[^\n]" /*OBF_SKIP*/, &start, &end, perms, &off, &dev_maj, &dev_min, &inode, file);
    if (n < 8)
      continue;
    // Match executable mappings only; the first one in the file is
    // conventionally the ELF header location.
    if (perms[2] != 'x')
      continue;
    const char* slash = strrchr(file, '/');
    const char* bn = slash ? slash + 1 : file;
    if (strcmp(bn, needle) == 0)
    {
      base = start - off; // subtract offset so 'base' points to ELF header
      break;
    }
  }
  fclose(f);
  return base;
}

bool find_writable_mapping(pid_t pid, uintptr_t addr, uintptr_t& out_start, uintptr_t& out_end)
{
  out_start = 0;
  out_end = 0;
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/maps", pid);
  FILE* f = fopen(path, "re");
  if (!f)
  {
    LOGE(OBF("fopen(%s): %s").c_str(), path, strerror(errno));
    return false;
  }

  char line[512];
  while (fgets(line, sizeof(line), f))
  {
    uintptr_t start = 0, end = 0;
    char perms[8] = {};
    if (sscanf(line, "%" SCNxPTR "-%" SCNxPTR " %7s" /*OBF_SKIP*/, &start, &end, perms) != 3)
      continue;
    if (addr >= start && addr < end && perms[0] == 'r' && perms[1] == 'w')
    {
      out_start = start;
      out_end = end;
      fclose(f);
      return true;
    }
  }
  fclose(f);
  return false;
}

// ELF symbol walking

// Read an ELF symbol's offset-from-base from a SO on disk. We mirror the same
// disk file the target mapped. Much easier than chasing the mapping in memory.
static uintptr_t elf_symbol_off_from_file(const char* so_path, const char* needle)
{
  FILE* f = fopen(so_path, "rbe");
  if (!f)
    return 0;

  Elf64_Ehdr eh{};
  if (fread(&eh, 1, sizeof(eh), f) != sizeof(eh) || memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0)
  {
    fclose(f);
    return 0;
  }

  fseek(f, (long) eh.e_shoff, SEEK_SET);
  Elf64_Shdr* sh = (Elf64_Shdr*) calloc(eh.e_shnum, sizeof(Elf64_Shdr));
  if (!sh)
  {
    fclose(f);
    return 0;
  }
  fread(sh, sizeof(Elf64_Shdr), eh.e_shnum, f);

  // Try SHT_DYNSYM first, then SHT_SYMTAB.
  auto search = [&](uint32_t sym_type) -> uintptr_t
  {
    for (int i = 0; i < eh.e_shnum; ++i)
    {
      if (sh[i].sh_type != sym_type)
        continue;
      int strtab_idx = (int) sh[i].sh_link;
      // Read symbol table.
      size_t ns = sh[i].sh_size / sizeof(Elf64_Sym);
      Elf64_Sym* sy = (Elf64_Sym*) calloc(ns, sizeof(Elf64_Sym));
      fseek(f, (long) sh[i].sh_offset, SEEK_SET);
      fread(sy, sizeof(Elf64_Sym), ns, f);
      // Read string table.
      size_t strsz = sh[strtab_idx].sh_size;
      char* strs = (char*) calloc(1, strsz);
      fseek(f, (long) sh[strtab_idx].sh_offset, SEEK_SET);
      fread(strs, 1, strsz, f);
      uintptr_t r = 0;
      for (size_t k = 0; k < ns; ++k)
      {
        const char* nm = strs + sy[k].st_name;
        if (strcmp(nm, needle) == 0)
        {
          r = (uintptr_t) sy[k].st_value;
          break;
        }
      }
      free(sy);
      free(strs);
      if (r)
        return r;
    }
    return 0;
  };
  uintptr_t off = search(SHT_DYNSYM);
  if (!off)
    off = search(SHT_SYMTAB);
  free(sh);
  fclose(f);
  return off;
}

// Find, on disk, the library mapped at pid's maps under `so_basename`.
// We resolve the full path from /proc/pid/maps.
static int find_so_path(pid_t pid, const char* basename, char* out_path, size_t out_len)
{
  char maps[64];
  snprintf(maps, sizeof(maps), "/proc/%d/maps", pid);
  FILE* f = fopen(maps, "re");
  if (!f)
    return -errno;
  char line[512];
  while (fgets(line, sizeof(line), f))
  {
    char* sl = strchr(line, '/');
    if (!sl)
      continue;
    char* nl = strchr(sl, '\n');
    if (nl)
      *nl = 0;
    const char* bn = strrchr(sl, '/');
    bn = bn ? bn + 1 : sl;
    if (strcmp(bn, basename) == 0)
    {
      snprintf(out_path, out_len, "%s", sl);
      fclose(f);
      return 0;
    }
  }
  fclose(f);
  return -ENOENT;
}

uintptr_t lookup_symbol(pid_t pid, const char* so_basename, const char* sym)
{
  uintptr_t base = maps_base(pid, so_basename);
  if (!base)
  {
    LOGE(OBF("lookup_symbol: %s not mapped in pid=%d").c_str(), so_basename, pid);
    return 0;
  }

  char path[PATH_MAX];
  if (find_so_path(pid, so_basename, path, sizeof(path)) < 0)
  {
    LOGE(OBF("lookup_symbol: no on-disk path for %s").c_str(), so_basename);
    return 0;
  }

  uintptr_t off = elf_symbol_off_from_file(path, sym);
  if (!off)
  {
    LOGE(OBF("lookup_symbol: %s not in %s").c_str(), sym, path);
    return 0;
  }
  return base + off;
}

// PID lookup by /proc/<pid>/comm

pid_t find_pid(const char* comm_name)
{
  DIR* d = opendir("/proc");
  if (!d)
    return 0;

  pid_t found = 0;
  for (dirent* e; (e = readdir(d));)
  {
    if (e->d_name[0] < '0' || e->d_name[0] > '9')
      continue;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
    FILE* f = fopen(path, "re");
    if (!f)
      continue;
    char buf[64] = {};
    fgets(buf, sizeof(buf), f);
    fclose(f);
    // /proc/N/comm has a trailing \n.
    size_t n = strlen(buf);
    if (n && buf[n - 1] == '\n')
      buf[n - 1] = 0;
    if (strcmp(buf, comm_name) == 0)
    {
      found = (pid_t) atoi(e->d_name);
      break;
    }
  }
  closedir(d);
  return found;
}

} // namespace cr::inj
