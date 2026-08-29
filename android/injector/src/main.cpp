/*
 * Назначение файла:
 *   cr_injector выполняет root/ptrace загрузку phone-side .so в системный
 *   процесс Android и вызывает экспортированную init-функцию внутри target
 *   address space.
 *
 * ABI/совместимость:
 *   CLI arguments и exported init names не меняются. Важный контракт: return
 *   value init-функции теперь считается частью deploy status. Нулевой return
 *   означает установленный hook; ненулевой return приводит к non-zero exit,
 *   чтобы host не показывал ложный success.
 *
 * Ограничения:
 *   Injector не патчит target code сам по себе; он только вызывает dlopen,
 *   dlsym и init. Оригинальные binaries/firmware не изменяются.
 */

// cr_injector — real ptrace-based .so loader.
// Flow (all inside the target's address space after the first remote_call):
//   1. attach → ptrace-stop target (must run as root)
//   2. save regs
//   3. locate libdl.so inside target, resolve dlopen + dlsym addresses
//   4. write the library path + "cr_camhook_init" + feed URL into the remote
//      stack region (below the saved SP)
//   5. remote call dlopen(path, RTLD_NOW)  → handle
//   6. remote call dlsym(handle, "cr_camhook_init") → entry
//   7. remote call entry(feed_url) → ignore return
//   8. restore saved regs, detach
// The target resumes as if nothing happened, but its libcr_camhook.so is
// loaded and its init has already run.
// CLI:
//   cr_injector --target <pid|procname>
//               --lib    <path-to-libcr_camhook.so>
//               --feed   <feed URL, e.g. srt://... or photo:/path>

#include "ptrace_arm64.h"
#include "cr_feed.h"

#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include "util/Obf.h"
#define TAG "cr_injector"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace inj = cr::inj;

namespace
{

#if defined(__clang__) || defined(__GNUC__)
#define CR_NOINLINE __attribute__((noinline))
#else
#define CR_NOINLINE
#endif

constexpr int RTLD_NOW_CONST = 2; // matches bionic libdl

struct Args
{
  const char* target = nullptr;
  const char* lib = nullptr;
  const char* feed = nullptr;
  // Symbol the loaded library exposes as its post-dlopen entry point.
  // Defaults to the camera hook for backwards compatibility; the audio
  // hook overrides it with --init cr_audhook_init.
  const char* init = "cr_camhook_init";
};

const char* feed_mode_arg(int argc, char** argv)
{
  const char* feed = nullptr;
  bool injector_arg = false;
  for (int i = 1; i < argc; ++i)
  {
    if (!strcmp(argv[i], "--feed") && i + 1 < argc)
    {
      feed = argv[++i];
      continue;
    }
    if (!strcmp(argv[i], "--target") || !strcmp(argv[i], "--lib") || !strcmp(argv[i], "--init"))
      injector_arg = true;
  }
  if (injector_arg)
    return nullptr;
  return feed;
}

CR_NOINLINE bool parse(int argc, char** argv, Args& out)
{
  for (int i = 1; i < argc; ++i)
  {
    auto take = [&](const char* f, const char*& dst)
    {
      if (!strcmp(argv[i], f) && i + 1 < argc)
      {
        dst = argv[++i];
        return true;
      }
      return false;
    };
    if (take("--target", out.target))
      continue;
    if (take("--lib", out.lib))
      continue;
    if (take("--feed", out.feed))
      continue;
    if (take("--init", out.init))
      continue;
    LOGE(OBF("unknown arg: %s").c_str(), argv[i]);
    return false;
  }
  return out.target && out.lib && out.feed && out.init;
}

CR_NOINLINE pid_t resolve_target(const char* spec)
{
  // Numeric → use as-is, else treat as /proc/*/comm name.
  char* end = nullptr;
  long p = strtol(spec, &end, 10);
  if (end && *end == '\0' && p > 0)
    return (pid_t) p;
  return inj::find_pid(spec);
}

CR_NOINLINE bool loader_symbols_available(uintptr_t t_dlopen, uintptr_t t_dlsym)
{
  return t_dlopen && t_dlsym;
}

CR_NOINLINE bool dlopen_handle_valid(uint64_t handle)
{
  return handle != 0;
}

CR_NOINLINE bool dlsym_entry_valid(uint64_t entry)
{
  return entry != 0;
}

CR_NOINLINE bool init_return_ok(uint64_t init_ret)
{
  return static_cast<int64_t>(init_ret) == 0;
}

// Pack three NUL-terminated strings into one buffer and return their
// relative offsets. Makes a single poke_mem enough to stage all parameters.
struct StringPack
{
  std::string blob;
  uintptr_t lib_off = 0;
  uintptr_t init_off = 0;
  uintptr_t feed_off = 0;
};
StringPack pack_strings(const char* lib, const char* init, const char* feed)
{
  StringPack p;
  p.lib_off = p.blob.size();
  p.blob.append(lib);
  p.blob.push_back(0);
  p.init_off = p.blob.size();
  p.blob.append(init);
  p.blob.push_back(0);
  p.feed_off = p.blob.size();
  p.blob.append(feed);
  p.blob.push_back(0);
  // Pad to 8 bytes so poke_mem's word writes don't clobber adjacent data.
  while (p.blob.size() % 8)
    p.blob.push_back(0);
  return p;
}

} // namespace

int main(int argc, char** argv)
{
  if (const char* feed = feed_mode_arg(argc, argv))
    return cr_feed_run(feed);

  Args a;
  if (!parse(argc, argv, a))
  {
    fprintf(stderr, "usage: cr_injector --target <pid|procname> --lib <path> --feed <url>\n");
    return 2;
  }

  pid_t pid = resolve_target(a.target);
  if (pid <= 0)
  {
    LOGE(OBF("cannot resolve target '%s'").c_str(), a.target);
    return 3;
  }
  LOGI(OBF("target=%s (pid=%d) lib=%s feed=%s").c_str(), a.target, pid, a.lib,
       a.feed ? a.feed : OBF("(null)").c_str());

  if (inj::attach(pid) < 0)
    return 4;

  // --- remember original registers so we can restore cleanly ------------
  inj::RemoteRegs saved{};
  if (inj::get_regs(pid, saved) < 0)
  {
    inj::detach(pid);
    return 5;
  }
  LOGI(OBF("saved pc=%#llx sp=%#llx x30=%#llx").c_str(), (unsigned long long) saved.pc, (unsigned long long) saved.sp, (unsigned long long) saved.x[30]);

  // --- resolve the loader's internal dlopen / dlsym ---------------------
  // The public `dlopen` in libdl.so passes `__builtin_return_address(0)` as
  // the caller hint, i.e. our lr. If lr=0 (the classic trampoline) bionic's
  // namespace lookup fails and dlopen returns NULL. So we call
  // __loader_dlopen directly and pass saved.pc as the caller_addr — that
  // puts the load in the same namespace as whatever code the target was
  // executing when we attached, which on cameraserver is the default one.
  uintptr_t t_dlopen = inj::lookup_symbol(pid, "linker64", "__loader_dlopen");
  uintptr_t t_dlsym = inj::lookup_symbol(pid, "linker64", "__loader_dlsym");
  if (!loader_symbols_available(t_dlopen, t_dlsym))
  {
    // Format strings include PRIxPTR macro expansion (typically "lx").
    // Adjacent-literal concat at compile time would normally turn
    // `"foo=%#" PRIxPTR " bar=%#" PRIxPTR` into one literal, but with
    // OBF the LHS is now a runtime const char* — adjacent concat
    // doesn't apply. Build the format string at runtime instead.
    LOGE(OBF("linker64 lookup failed - __loader_dlopen=%#llx __loader_dlsym=%#llx").c_str(),
         (unsigned long long) t_dlopen,
         (unsigned long long) t_dlsym);
    inj::set_regs(pid, saved);
    inj::detach(pid);
    return 6;
  }
  {
    LOGI(OBF("__loader_dlopen=%#llx __loader_dlsym=%#llx").c_str(),
         (unsigned long long) t_dlopen,
         (unsigned long long) t_dlsym);
  }

  // --- stage strings in target's stack ----------------------------------
  StringPack pk = pack_strings(a.lib, a.init, a.feed);

  uintptr_t stack_start = 0;
  uintptr_t stack_end = 0;
  if (!inj::find_writable_mapping(pid, saved.sp, stack_start, stack_end))
  {
    LOGE(OBF("cannot find writable stack mapping for saved.sp=%#llx").c_str(), (unsigned long long) saved.sp);
    inj::set_regs(pid, saved);
    inj::detach(pid);
    return 7;
  }

  constexpr uintptr_t kStringGuardBytes = 0x1000;
  constexpr uintptr_t kCallStackBytes = 64 * 1024;
  constexpr uintptr_t kStackLowGuardBytes = 0x1000;
  const uintptr_t saved_sp = saved.sp & ~0xfULL;
  const uintptr_t blob_bytes = (uintptr_t) pk.blob.size();
  if (saved_sp <= stack_start + kStackLowGuardBytes + kCallStackBytes + kStringGuardBytes + blob_bytes ||
      saved_sp > stack_end)
  {
    LOGE(OBF("stack layout rejected: map=%#llx-%#llx saved_sp=%#llx blob=%zu").c_str(),
         (unsigned long long) stack_start,
         (unsigned long long) stack_end,
         (unsigned long long) saved_sp,
         pk.blob.size());
    inj::set_regs(pid, saved);
    inj::detach(pid);
    return 7;
  }

  const uintptr_t scratch = (saved_sp - kStringGuardBytes - blob_bytes) & ~0xfULL;
  const uintptr_t call_sp = (scratch - kCallStackBytes) & ~0xfULL;
  if (scratch < stack_start ||
      scratch + blob_bytes > stack_end ||
      scratch + blob_bytes > saved_sp - kStringGuardBytes ||
      call_sp < stack_start + kStackLowGuardBytes ||
      call_sp + kCallStackBytes > scratch)
  {
    LOGE(OBF("stack layout bounds failed: map=%#llx-%#llx scratch=%#llx call_sp=%#llx blob=%zu").c_str(),
         (unsigned long long) stack_start,
         (unsigned long long) stack_end,
         (unsigned long long) scratch,
         (unsigned long long) call_sp,
         pk.blob.size());
    inj::set_regs(pid, saved);
    inj::detach(pid);
    return 7;
  }

  if (inj::poke_mem(pid, scratch, pk.blob.data(), pk.blob.size()) < 0)
  {
    LOGE(OBF("staging poke failed").c_str());
    inj::set_regs(pid, saved);
    inj::detach(pid);
    return 7;
  }
  // Adjacent-literal concat with the PRIxPTR macro requires literals
  // on both sides — OBF returns runtime const char*, can't concat.
  LOGI(OBF("strings staged at %#llx (%zu bytes); call_sp=%#llx stack=%#llx-%#llx").c_str(),
       (unsigned long long) scratch,
       pk.blob.size(),
       (unsigned long long) call_sp,
       (unsigned long long) stack_start,
       (unsigned long long) stack_end);

  uintptr_t path_addr = scratch + pk.lib_off;
  uintptr_t init_addr = scratch + pk.init_off;
  uintptr_t feed_addr = scratch + pk.feed_off;

  int rc = 0;

  // lr = 0 keeps the classic "ret to NULL -> SIGSEGV/SIGBUS" flow. The
  // helper now treats only that controlled null return as success.
  const uintptr_t return_trap = 0;
  uint64_t handle = 0;

  // --- __loader_dlopen(path, RTLD_NOW, caller_addr=saved.pc) ------------
  if (inj::remote_call(pid, saved, OBF("dlopen-lib").c_str(), t_dlopen, path_addr, RTLD_NOW_CONST,
                       (uintptr_t) saved.pc, // caller_addr for namespace
                       0, return_trap, call_sp, &handle) < 0)
  {
    LOGE(OBF("remote __loader_dlopen failed").c_str());
    rc = 8;
    goto cleanup;
  }
  LOGI(OBF("__loader_dlopen -> %#llx").c_str(), (unsigned long long) handle);
  if (!dlopen_handle_valid(handle))
  {
    LOGE(OBF("target dlopen returned NULL - check .so path and SELinux label").c_str());
    rc = 9;
    goto cleanup;
  }

  // --- __loader_dlsym(handle, init_symbol, caller_addr) ---------------
  uint64_t entry;
  if (inj::remote_call(pid, saved, OBF("dlsym-init").c_str(), t_dlsym, (uintptr_t) handle, init_addr, (uintptr_t) saved.pc, 0, return_trap, call_sp, &entry) < 0)
  {
    LOGE(OBF("remote __loader_dlsym failed").c_str());
    rc = 14;
    goto cleanup;
  }
  LOGI(OBF("__loader_dlsym(\"%s\") -> %#llx").c_str(), a.init, (unsigned long long) entry);
  if (!dlsym_entry_valid(entry))
  {
    LOGE(OBF("dlsym returned NULL - bad symbol name?").c_str());
    rc = 15;
    goto cleanup;
  }

  // --- call init(feed_url) ---------------------------------------------
  uint64_t init_ret;
  if (inj::remote_call(pid, saved, OBF("call-init").c_str(), (uintptr_t) entry, feed_addr, 0, 0, 0, return_trap, call_sp, &init_ret) < 0)
  {
    LOGE(OBF("remote %s failed").c_str(), a.init);
    rc = 16;
    goto cleanup;
  }
  LOGI(OBF("%s -> %lld").c_str(), a.init, (long long) (int64_t) init_ret);
  if (!init_return_ok(init_ret))
  {
    LOGE(OBF("%s returned non-zero: %lld").c_str(), a.init, (long long) (int64_t) init_ret);
    rc = 17;
  }

cleanup:
  // Restore the original registers even on error so the target keeps
  // running. This is the single most important invariant — otherwise the
  // camera server might get SIGSEGV the moment we detach.
  if (inj::set_regs(pid, saved) < 0)
  {
    LOGE(OBF("restore regs failed - target may crash on resume").c_str());
  }
  inj::detach(pid);

  if (rc == 0)
  {
    printf("cr_injector: OK (handle=%#llx, init ret=%d)\n", (unsigned long long) handle, (int) init_ret);
  }
  else
  {
    fprintf(stderr, "cr_injector: FAIL rc=%d\n", rc);
  }
  return rc;
}
