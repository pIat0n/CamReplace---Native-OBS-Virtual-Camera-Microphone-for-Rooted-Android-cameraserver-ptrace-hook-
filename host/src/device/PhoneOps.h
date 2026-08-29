/*
 * Назначение файла:
 *   PhoneOps — единственная точка сборки adb/su shell-команд для rooted Pixel.
 *   Здесь управляются artifacts, cr_feed_proc, /data/cr shared memory,
 *   adb-forward и ptrace injection в cameraserver/audioserver.
 *
 * Hook point и ABI assumptions:
 *   host не меняет exported Android symbols и не меняет wire format shared
 *   memory v1. Имена cr_camhook_init/cr_audhook_init, feed URL protocol и
 *   файлы /data/cr/feed, /data/cr/audio остаются совместимыми с phone-side
 *   artifacts. Shell-команды обязаны фильтровать реальные процессы через
 *   /proc/PID/exe, потому что pgrep/pidof на Android часто возвращают
 *   несколько PID или сам shell-wrapper.
 *
 * Ограничения Pixel/Android:
 *   Pixel 6-10 на Android 12-16 держит камеры и audio в системных сервисах,
 *   которые init быстро respawn-ит после killall. Поэтому readiness должен
 *   проверять факт жизни процесса, факт запуска cr_feed_proc и лог listener,
 *   а не ждать фиксированным sleep. /data/cr удаляется только через явный
 *   uninstall/wipe path после остановки hooks.
 *
 * Подтверждено:
 *   supported flow — CameraServer/AudioServer injection, AIDL/HIDL/tinyalsa
 *   audio paths, tcp_h264/tcp_nv21/tcp_pcm feed schemes. Sound Trigger/AOC
 *   coverage не гарантируется phone-side hook'ом и диагностируется отдельно.
 *
 * Diagnostic-only:
 *   wait_* методы не доказывают успешную подмену кадра/PCM, они только
 *   фиксируют готовность инфраструктуры и предотвращают ложный success path.
 */

#pragma once

// PhoneOps — typed wrapper around every multi-statement shell command
// we run via `adb shell su -c '...'` against the rooted phone.
// WHY THIS EXISTS
// FeedController used to compose these scripts inline as raw C++ string
// concatenation. Three real bugs found in production this way:
//   * pkill -f '^...regex...' silently failed to match cr_feed_proc on
//     toybox builds (cmdline is NUL-separated; the `^` anchor isn't
//     reliable). cr_feed_proc kept running and the camhook kept
//     replaying the last frame.
//   * pgrep -f cr_feed_proc matched the running shell itself (its
//     argv contains the literal "cr_feed_proc" because we typed it),
//     plus the su / Magisk policy proxies wrapping `su -c`. The kill
//     loop then murdered its own shell before reaching the dd that
//     was supposed to zero the shm magic.
//   * `pidof cameraserver` looked correct but wasn't quoted right at
//     the boundary between adb's argv-joining and su's reparsing,
//     leaving $(pidof) expanded by the host shell against an empty
//     environment.
// Every one of these is a five-character typo away. Centralising the
// shell composition in PhoneOps means we fix each footgun ONCE, here,
// and downstream callers get the safe version forever.
// CONVENTIONS
// * Every method accepts a `DeployCallback` and emits its progress
//   lines via the same `step()` helper FeedController used to use —
//   timeline format unchanged, so dev session.log keeps reading the same.
// * Methods that mutate phone state return `bool` (true = success).
// * Pure queries (`hook_loaded`, `feed_proc_running`) return bool and
//   do NOT emit cb lines — they're read-side only.
// * Wipe-style ops (`unforward`, `wipe_artifacts`) are infallible —
//   "rule wasn't there" or "file already gone" is success.
// * `setenforce 0` is sprinkled liberally; we leave the device in
//   permissive mode for the session. Restoring on shutdown isn't
//   handled here — the user reboots when done with us.

#include "device/DeployStatus.h"

#include <initializer_list>
#include <string>
#include <vector>

namespace cr::device
{

// Remote paths remain local implementation details of PhoneOps.cpp.

// Selected shm files under /data/cr.
enum class ShmKind
{
  Feed,  // /data/cr/feed   — NV21 frames for camhook
  Audio, // /data/cr/audio  — S16LE PCM for audhook
};

// Which system service we're operating on.
enum class HostProc
{
  CameraServer,
  AudioServer,
};

// Which on-phone artifact a `kill_feed_proc` call should target. The
// scheme tag is what we grep `/proc/PID/cmdline` for after the
// `/proc/PID/exe` identity check filters out shells that happen to
// contain the same string in their argv.
enum class FeedScheme
{
  H264, // tcp_h264:listen:<port>  (Compressed transport)
  Nv21, // tcp_nv21:listen:<port>  (Raw transport)
  Pcm,  // tcp_pcm:listen:<port>   (audio PCM)
};

class PhoneOps
{
public:
  explicit PhoneOps(std::string serial) : serial_(std::move(serial)) {}

  // --- Artifact lifecycle ----------------------------------------------
  // Push every binary + .so we ship from %LOCALAPPDATA% to /data/local/tmp,
  // chmod for execute, chcon the camhook/audhook libs to a label
  // cameraserver/audioserver are allowed to dlopen.
  // First step is a `clean previous session` that kills any orphan
  // cr_feed_proc and removes stale files — done idempotently so a
  // re-push survives a previous crash.
  bool push_artifacts(const DeployCallback& cb);

  // Inverse of push_artifacts: rm everything we ever wrote under
  // /data/local/tmp (binaries, .so, logs, the camera-package list)
  // and rm -rf /data/cr (the shm dir). Caller is responsible for
  // unloading hooks first via `restart_services` — otherwise the
  // .so files are unlinked from disk but still mapped in
  // cameraserver/audioserver until the next service kill.
  bool wipe_artifacts(const DeployCallback& cb);

  // --- cr_feed_proc lifecycle ------------------------------------------
  // Launch cr_feed_proc with the given feed URL. Includes the
  // /data/cr prep step (mkdir + chmod + chcon to system_data_file)
  // and `setenforce 0` so the camhook's lazy shm reopen stays
  // permitted long after inject finishes.
  // In dev, `log_basename` is the file under /data/local/tmp that
  // nohup redirects stdout/stderr to. Release secure logs redirect to
  // /dev/null and emit encrypted CRLA1 records through logcat instead.
  bool launch_feed(const std::string& feed_url, const std::string& log_basename, const DeployCallback& cb);

  bool wait_feed_ready(FeedScheme scheme, const std::string& log_basename, int timeout_ms, const DeployCallback& cb) const;

  bool wait_feed_stopped(FeedScheme scheme, int timeout_ms, const DeployCallback& cb) const;

  bool wait_host_proc_alive(HostProc target, int timeout_ms, const DeployCallback& cb) const;

  // True if a cr_feed_proc whose argv contains the scheme tag is
  // currently running on the phone. Const — emits no progress lines.
  // Filters out the running shell via /proc/PID/exe identity check
  // so we never false-positive on the search command's own argv.
  bool feed_proc_running(FeedScheme scheme) const;

  // Kill the cr_feed_proc instance whose argv contains the scheme
  // tag. Same /proc/PID/exe identity filter — never kills our own
  // shell. Other-scheme cr_feed_proc instances (e.g. PCM while we
  // kill H264) survive untouched.
  void kill_feed_proc(FeedScheme scheme, const DeployCallback& cb);

  // Kill all cr_feed_proc instances regardless of scheme. Used by
  // drop_hooks (we're tearing everything down) and wipe_artifacts.
  void kill_all_feed_procs(const DeployCallback& cb);

  // --- shm management --------------------------------------------------
  // Zero the 4-byte CRFD/CRAU magic at offset 0 of the shm file.
  // The hook does a per-frame magic check: a zero magic means the
  // hook tail-calls the original queueBufferToConsumer / read with
  // the real sensor / mic buffer untouched. dd writes through to
  // the MAP_SHARED page that the hook holds, so the very next call
  // sees the real frame.
  void zero_shm_magic(ShmKind kind, const DeployCallback& cb);

  // Soft-disable video substitution without corrupting the feed header.
  // This clears cr_feed_header::channel_state to NONE, so camhook keeps
  // its mapping but immediately pass-throughs real camera buffers.
  void disable_video_replace_passthrough(const DeployCallback& cb);

  // --- adb-forward management ------------------------------------------
  bool forward(int port, const DeployCallback& cb);
  // Idempotent — "rule not found" is success.
  void unforward(int port, const DeployCallback& cb);

  // --- Hook lifecycle (cameraserver / audioserver) ---------------------
  // True if our injected library is currently mapped in the target
  // process. Caller decides how stale that map is acceptable.
  bool hook_loaded(HostProc target) const;

  // ptrace-inject a library into the named process via cr_injector.
  // For camhook the target is restarted first (close clients →
  // killall cameraserver → respawn → inject), then any closed
  // packages get monkey-relaunched. For audhook there's no service
  // restart — shadowhook attaches dynamically.
  // `init_fn` is the symbol cr_injector will call after dlopen
  // (defaults to "cr_camhook_init" for cameraserver and
  // "cr_audhook_init" for audioserver — pass empty string to take
  // the default).
  // `feed_url` is stored on the hook's init for diagnostics. The
  // hook's runtime shm path is hardcoded to /data/cr/feed (or
  // /data/cr/audio) — the URL doesn't pick a backend.
  bool inject_lib(HostProc target, const std::string& lib_basename, const std::string& init_fn, const std::string& feed_url, const DeployCallback& cb);

  // Restart the named system services. init.rc respawns each within
  // ~200 ms; respawned processes don't dlopen our .so files (the
  // injection happens via cr_injector + ptrace, init never knew about
  // them) so they come back hook-free.
  void restart_services(std::initializer_list<const char*> names, const DeployCallback& cb);

  // --- Camera client management ----------------------------------------
  std::string active_camera_client_packages() const;

  // Close any app currently bound to cameraserver. The vendor camera
  // HAL has outstanding HIDL transactions that fail with DEAD_OBJECT
  // when cameraserver dies; without this close, the HAL SIGABRTs on
  // its CHECK macros and a tombstone lands in /data/tombstones/.
  // `remember_for_relaunch=true` writes the closed package list to
  // /data/local/tmp/cr_last_cam_pkgs so a downstream
  // `relaunch_remembered_camera_clients` call can monkey-launch them
  // back. Pass false for destructive intents (Delete software).
  bool close_camera_clients(bool remember_for_relaunch, const DeployCallback& cb);

  // monkey-launch every package in /data/local/tmp/cr_last_cam_pkgs.
  // Idempotent — empty file or missing packages are a no-op.
  void relaunch_remembered_camera_clients(const DeployCallback& cb);

  // --- Photo mode sentinel ---------------------------------------------
  // Touch / remove /data/cr/photo_mode. The camhook re-reads this
  // file every ~500 ms inside its qbtc proxy, so the next still-
  // capture BLOB hits the JPEG-encode path automatically.
  bool enable_photo_mode(const DeployCallback& cb);
  bool disable_photo_mode(const DeployCallback& cb);

private:
  std::string serial_;
};

} // namespace cr::device
