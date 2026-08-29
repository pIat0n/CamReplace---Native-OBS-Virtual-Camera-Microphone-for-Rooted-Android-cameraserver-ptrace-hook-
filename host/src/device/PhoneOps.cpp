/*
 * Назначение файла:
 *   Реализация PhoneOps: безопасная последовательность adb/su операций для
 *   rooted Pixel, включая push artifacts, запуск feed process, управление
 *   shared memory в /data/cr и injection в cameraserver/audioserver.
 *
 * Hook point и ABI assumptions:
 *   host вызывает только стабильные exported init functions
 *   cr_camhook_init/cr_audhook_init через cr_injector. Shared memory v1,
 *   feed URL protocol и имена on-phone файлов не меняются. Проверки
 *   readiness диагностируют процессную инфраструктуру, но не подменяют
 *   camera/audio success criteria.
 *
 * Ограничения Pixel/Android:
 *   Android init respawn-ит cameraserver/audioserver асинхронно, а pgrep/pidof
 *   могут видеть shell wrappers и несколько PID. Поэтому все lifecycle checks
 *   фильтруют /proc/PID/exe или /proc/PID/maps и ждут реальное состояние,
 *   а не fixed sleeps. /data/cr destructive cleanup допустим только в явном
 *   uninstall/wipe flow после остановки hooks.
 *
 * Подтверждено:
 *   Поддерживается CameraServer/AudioServer injection, tcp_h264/tcp_nv21/tcp_pcm
 *   feed process и AIDL/HIDL/tinyalsa-facing audio replacement. Tensor Pixel
 *   не должен зависеть от Qualcomm decoder names на host-side orchestration.
 *
 * Diagnostic-only:
 *   wait_feed_ready/wait_host_proc_alive/hook_loaded подтверждают запуск и map
 *   библиотеки; фактическая production-подмена подтверждается phone-side
 *   primary hook и runtime statistics.
 */

#include "device/PhoneOps.h"
#include "device/AdbClient.h"
#include "resources/EmbeddedFs.h"
#include "resources/Resources.h"
#include <cstdio>
#include <exception>
#include <filesystem>
#include <utility>

namespace fs = std::filesystem;

namespace cr::device
{

namespace
{

// Same shape FeedController used to use locally вЂ” promoted here so
// every PhoneOps method emits cb lines uniformly.
bool step(const std::string& serial, std::string label, std::vector<std::string> args, const DeployCallback& cb)
{
  if (cb)
    cb(DeployStatus::info(label));
  auto r = AdbClient::instance().run_sync_to(serial, std::move(args));
  if (r.exit_code == 0)
  {
    if (cb)
      cb(DeployStatus::ok(std::move(label), r.stdout_));
    return true;
  }
  if (cb)
    cb(DeployStatus::err(std::move(label), r.stderr_.empty() ? r.stdout_ : r.stderr_));
  return false;
}

void step_optional(const std::string& serial, std::string label, std::vector<std::string> args, const DeployCallback& cb)
{
  if (cb)
    cb(DeployStatus::info(label));
  auto r = AdbClient::instance().run_sync_to(serial, std::move(args));
  std::string detail = r.exit_code == 0 ? r.stdout_ : (r.stderr_.empty() ? r.stdout_ : r.stderr_);
  for (char& c : detail)
    if (c == '\n' || c == '\r')
      c = ' ';
  if (cb)
    cb(DeployStatus::ok(std::move(label), std::move(detail)));
}

// Scheme tags as Obfuscated runtime strings. Every call to scheme_tag()
// creates a fresh per-string-encrypted Decoded buffer; the returned
// std::string copies the bytes once. Plaintext never touches `.rodata`.
std::string scheme_tag(FeedScheme s)
{
  switch (s)
  {
  case FeedScheme::H264:
    return "tcp_h264";
  case FeedScheme::Nv21:
    return "tcp_nv21";
  case FeedScheme::Pcm:
    return "tcp_pcm";
  }
  return "";
}

std::string shm_path(ShmKind k)
{
  return k == ShmKind::Feed ? std::string("/data/cr/feed") : std::string("/data/cr/audio");
}

std::string host_proc_name(HostProc p)
{
  return p == HostProc::CameraServer ? std::string("cameraserver") : std::string("audioserver");
}

std::string hook_lib_basename(HostProc p)
{
  (void)p;
  return "libcr_hooks";
}

std::string hook_ready_path(HostProc p)
{
  return p == HostProc::CameraServer ? std::string("/data/cr/camhook.ready") : std::string("/data/cr/audhook.ready");
}

std::string default_init_fn(HostProc p)
{
  return p == HostProc::CameraServer ? std::string("cr_camhook_init") : std::string("cr_audhook_init");
}

int wait_loop_count(int timeout_ms)
{
  return timeout_ms <= 0 ? 1 : (timeout_ms + 99) / 100;
}

bool safe_basename_char(char c)
{
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
}

std::string safe_remote_basename(const std::string& name)
{
  if (name.empty() || name.size() > 96)
    return {};
  for (char c : name)
  {
    if (!safe_basename_char(c))
      return {};
  }
  return name;
}

std::string shell_single_quote(const std::string& value)
{
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('\'');
  for (char c : value)
  {
    if (c == '\'')
      out += "'\\''";
    else
      out.push_back(c);
  }
  out.push_back('\'');
  return out;
}

bool feed_launch_claims(const std::string& feed_url, std::string& stream_kind, std::string& feed_scheme)
{
  if (feed_url.rfind("tcp_h264:listen:", 0) == 0)
  {
    stream_kind = "video";
    feed_scheme = "tcp_h264";
    return true;
  }
  if (feed_url.rfind("tcp_nv21:listen:", 0) == 0)
  {
    stream_kind = "video";
    feed_scheme = "tcp_nv21";
    return true;
  }
  if (feed_url.rfind("tcp_pcm:listen:", 0) == 0)
  {
    stream_kind = "audio";
    feed_scheme = "tcp_pcm";
    return true;
  }
  return false;
}

std::string camera_client_packages_probe_script()
{
  return std::string("ds=$(dumpsys media.camera 2>/dev/null); pkgs=$( "
                     "  (echo \"$ds\" | sed -nE 's/.*Client Package Name: *([A-Za-z0-9._]+).*/\\1/p'; "
                     "   echo \"$ds\" | sed -nE 's/.*client for package ([A-Za-z0-9._]+).*/\\1/p') "
                     "  | head -n 3 | sort -u "
                     "); ");
}

std::string trim_copy(std::string s)
{
  const auto first = s.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return {};
  const auto last = s.find_last_not_of(" \t\r\n");
  return s.substr(first, last - first + 1);
}

} // namespace

// Artifact lifecycle

bool PhoneOps::push_artifacts(const DeployCallback& cb)
{
  namespace res = cr::resources;

  struct LocalAndroidArtifacts
  {
    ~LocalAndroidArtifacts()
    {
      cr::resources::cleanup_android_artifacts();
    }
  } local_android_artifacts;

  try
  {
    res::extract_android_artifacts();
  }
  catch (const std::exception& e)
  {
    if (cb)
      cb(DeployStatus::err("extract Android artifacts", e.what()));
    return false;
  }
  catch (...)
  {
    if (cb)
      cb(DeployStatus::err("extract Android artifacts", "unknown extraction error"));
    return false;
  }

  // Wipe step. /data/cr/feed is deliberately PRESERVED вЂ” cameraserver
  // may still hold an mmap on it from a previous Start, and replacing
  // the inode would leave the hook reading a now-detached page.
  std::string wipe = std::string("for p in $(pgrep -f cr_feed_proc 2>/dev/null); do   exe=$(readlink /proc/$p/exe 2>/dev/null); ") +
                     std::string("  case \"$exe\" in */cr_feed_proc) kill -9 $p 2>/dev/null ;; esac; done; sleep 0.15; rm -f ") +
                     "/data/local/tmp/cr_feed_proc " + "/data/local/tmp/libcr_hooks.so " + "/data/local/tmp/cr_injector " + "/data/local/tmp/cr_feed.log " + "/data/local/tmp/cr_feed_audio.log;" +
                     " rm -rf /data/cr/auth 2>/dev/null; true";
  if (!step(serial_, "clean previous session", AdbClient::su_shell(wipe), cb))
    return false;

  auto push = [&](const fs::path& local, const std::string& remote) { return step(serial_, std::string("push ") + local.filename().string(), {"push", local.string(), remote}, cb); };
  if (!push(res::cr_injector_path(), "/data/local/tmp/cr_injector"))
    return false;
  if (!push(res::cr_feed_proc_path(), "/data/local/tmp/cr_feed_proc"))
    return false;
  if (!push(res::libcr_hooks_path(), "/data/local/tmp/libcr_hooks.so"))
    return false;
  // chmod + chcon. Build the script in chunks so each path literal
  // gets its own per-string encryption key.
  std::string perm = std::string("chmod 755 ") + "/data/local/tmp/cr_injector " + "/data/local/tmp/cr_feed_proc " + "/data/local/tmp/libcr_hooks.so" + "; chcon u:object_r:system_file:s0 " + "/data/local/tmp/libcr_hooks.so";
  return step(serial_, "chmod + chcon", AdbClient::su_shell(perm), cb);
}

bool PhoneOps::wipe_artifacts(const DeployCallback& cb)
{
  std::string cmd = std::string("rm -f ") + "/data/local/tmp/cr_feed_proc " + "/data/local/tmp/libcr_hooks.so " + "/data/local/tmp/cr_injector " + "/data/local/tmp/cr_feed.log " + "/data/local/tmp/cr_feed_audio.log " + "/data/local/tmp/cr_last_cam_pkgs " + "2>/dev/null; rm -rf /data/cr 2>/dev/null; true";
  return step(serial_, "remove artifacts + /data/cr", AdbClient::su_shell(cmd), cb);
}

// cr_feed_proc lifecycle

bool PhoneOps::launch_feed(const std::string& feed_url, const std::string& log_basename, const DeployCallback& cb)
{
  std::string prep = std::string("mkdir -p /data/cr && chmod 0777 /data/cr && touch /data/cr/feed && chmod 0666 /data/cr/feed && "
                                 "touch /data/cr/audio && chmod 0666 /data/cr/audio && chcon u:object_r:system_data_file:s0 /data/cr && "
                                 "chcon u:object_r:system_data_file:s0 /data/cr/feed && chcon u:object_r:system_data_file:s0 /data/cr/audio; "
                                 "setenforce 0; echo ok");
  step(serial_, "prep /data/cr + setenforce 0", AdbClient::su_shell(prep), cb);

  std::string stream_kind;
  std::string feed_scheme;
  if (!feed_launch_claims(feed_url, stream_kind, feed_scheme))
  {
    if (cb)
      cb(DeployStatus::err("authorize Android launch", "unsupported feed scheme"));
    return false;
  }
  std::string launch = std::string("LD_LIBRARY_PATH=") + "/data/local/tmp " + "nohup " + "/data/local/tmp/cr_feed_proc " + "--feed " + shell_single_quote(feed_url);
  launch += " >" + std::string("/data/local/tmp/") + log_basename + " " + "2>&1 &";
  return step(serial_, std::string("launch cr_feed_proc (") + log_basename + ")", AdbClient::su_shell(launch), cb);
}

bool PhoneOps::wait_feed_ready(FeedScheme scheme, const std::string& log_basename, int timeout_ms, const DeployCallback& cb) const
{
  const std::string log = safe_remote_basename(log_basename);
  if (log.empty())
  {
    if (cb)
      cb(DeployStatus::err("wait cr_feed_proc ready", "unsafe log basename"));
    return false;
  }

  const std::string tag = scheme_tag(scheme);
  // Feed logs via __android_log_print (logcat), not the redirected
  // stdout file — so grepping the file for "listening" is unreliable
  // after OBF/VMP builds. Prefer: live cr_feed_proc + TCP LISTEN.
  const int port = (scheme == FeedScheme::Pcm) ? 8902 : 8901;
  char port_hex[8];
  std::snprintf(port_hex, sizeof(port_hex), "%04X", port & 0xffff);

  const int loops = wait_loop_count(timeout_ms);
  std::string listen_probe = std::string("  listening=NO;   if grep -qiE ':") +
                             std::string(port_hex) +
                             std::string(" .*0A' /proc/net/tcp /proc/net/tcp6 2>/dev/null; then     listening=YES; ");
  listen_probe += std::string("  elif grep -q listening /data/local/tmp/") + log + std::string(" 2>/dev/null; then     listening=YES;   fi; ");

  std::string cmd = std::string("i=0; while [ $i -lt ") + std::to_string(loops) +
                    std::string(" ]; do   found=NO; "
                                "  for p in $(pgrep -f cr_feed_proc 2>/dev/null); do     exe=$(readlink /proc/$p/exe 2>/dev/null); "
                                "    case \"$exe\" in */cr_feed_proc) ;; *) continue ;; esac;     c=$(tr '\\0' ' ' < /proc/$p/cmdline 2>/dev/null); "
                                "    case \"$c\" in *") +
                    tag +
                    std::string("*) found=YES; break ;; esac;   done; ") +
                    listen_probe +
                    std::string("  if [ \"$found\" = YES ] && [ \"$listening\" = YES ]; then "
                                "    echo READY; exit 0;   fi; "
                                "  i=$((i+1)); sleep 0.1; done; echo NOT_READY; exit 1");

  return step(serial_, std::string("wait cr_feed_proc ready (") + tag + ")", AdbClient::su_shell(cmd), cb);
}

bool PhoneOps::wait_feed_stopped(FeedScheme scheme, int timeout_ms, const DeployCallback& cb) const
{
  const std::string tag = scheme_tag(scheme);
  const int loops = wait_loop_count(timeout_ms);
  std::string cmd = std::string("i=0; while [ $i -lt ") + std::to_string(loops) +
                    std::string(" ]; do   found=NO; "
                                "  for p in $(pgrep -f cr_feed_proc 2>/dev/null); do     exe=$(readlink /proc/$p/exe 2>/dev/null); "
                                "    case \"$exe\" in */cr_feed_proc) ;; *) continue ;; esac;     c=$(tr '\\0' ' ' < /proc/$p/cmdline 2>/dev/null); "
                                "    case \"$c\" in *") +
                    tag +
                    std::string("*) found=YES; break ;; esac;   done; "
                                "  [ \"$found\" = NO ] && echo STOPPED && exit 0;   i=$((i+1)); sleep 0.1; "
                                "done; echo STILL_RUNNING; exit 1");

  return step(serial_, std::string("wait cr_feed_proc stopped (") + tag + ")", AdbClient::su_shell(cmd), cb);
}

bool PhoneOps::wait_host_proc_alive(HostProc target, int timeout_ms, const DeployCallback& cb) const
{
  const std::string proc = host_proc_name(target);
  const int loops = wait_loop_count(timeout_ms);
  std::string cmd = std::string("i=0; while [ $i -lt ") + std::to_string(loops) + std::string(" ]; do   for p in $(pidof ") + proc +
                    std::string(" 2>/dev/null); do     [ -r /proc/$p/cmdline ] && echo ALIVE:$p && exit 0; "
                                "  done;   i=$((i+1)); sleep 0.1; "
                                "done; echo NOT_ALIVE; exit 1");

  return step(serial_, std::string("wait ") + proc + " alive", AdbClient::su_shell(cmd), cb);
}

bool PhoneOps::feed_proc_running(FeedScheme scheme) const
{
  // Filter by /proc/$p/exe вЂ” must end with /cr_feed_proc, the actual
  // binary. See kill_feed_proc for the full bug story (pgrep self-
  // match killed the dd that was supposed to zero shm magic).
  const std::string tag = scheme_tag(scheme);
  std::string cmd = std::string("found=NO; for p in $(pgrep -f cr_feed_proc 2>/dev/null); do "
                                "  exe=$(readlink /proc/$p/exe 2>/dev/null);   case \"$exe\" in */cr_feed_proc) ;; *) continue ;; esac; "
                                "  c=$(tr '\\0' ' ' < /proc/$p/cmdline 2>/dev/null);   case \"$c\" in *") +
                    tag +
                    std::string("*) found=YES; break ;; esac; done; "
                                "echo $found");
  auto r = AdbClient::instance().run_sync_to(serial_, AdbClient::su_shell(cmd));
  return r.exit_code == 0 && r.stdout_.find("YES") != std::string::npos;
}

void PhoneOps::kill_feed_proc(FeedScheme scheme, const DeployCallback& cb)
{
  const std::string tag = scheme_tag(scheme);
  std::string cmd = std::string("for p in $(pgrep -f cr_feed_proc 2>/dev/null); do   exe=$(readlink /proc/$p/exe 2>/dev/null); "
                                "  case \"$exe\" in */cr_feed_proc) ;; *) continue ;; esac;   c=$(tr '\\0' ' ' < /proc/$p/cmdline 2>/dev/null); "
                                "  case \"$c\" in *") +
                    tag + std::string("*) kill -9 $p 2>/dev/null ;; esac; done; true");
  step(serial_, std::string("kill cr_feed_proc(") + tag + ")", AdbClient::su_shell(cmd), cb);
}

void PhoneOps::kill_all_feed_procs(const DeployCallback& cb)
{
  std::string cmd = std::string("for p in $(pgrep -f cr_feed_proc 2>/dev/null); do   exe=$(readlink /proc/$p/exe 2>/dev/null); "
                                "  case \"$exe\" in */cr_feed_proc) kill -9 $p 2>/dev/null ;; esac; done; sleep 0.15; true");
  step(serial_, "kill all cr_feed_proc", AdbClient::su_shell(cmd), cb);
}

// shm management

void PhoneOps::zero_shm_magic(ShmKind kind, const DeployCallback& cb)
{
  const std::string path = shm_path(kind);
  std::string cmd = std::string("[ -f ") + path + std::string(" ] && dd if=/dev/zero of=") + path + std::string(" bs=1 count=4 conv=notrunc 2>/dev/null; true");
  step(serial_, std::string("invalidate shm magic (") + path + ")", AdbClient::su_shell(cmd), cb);
}

void PhoneOps::disable_video_replace_passthrough(const DeployCallback& cb)
{
  // cr_feed_header::channel_state is at offset 48 in the fixed 64-byte
  // v1 header. Keep magic/header intact so the mapped camhook observes
  // a clean READY->NONE transition and simply stops touching buffers.
  std::string cmd = std::string("if [ -f /data/cr/feed ]; then "
                                "dd if=/dev/zero of=/data/cr/feed bs=1 seek=48 count=4 conv=notrunc 2>/dev/null; "
                                "echo pass-through; "
                                "else echo no feed shm; fi; true");
  step(serial_, "disable video replacement (pass-through)", AdbClient::su_shell(cmd), cb);
}

// adb-forward

bool PhoneOps::forward(int port, const DeployCallback& cb)
{
  return step(serial_, std::string("adb forward tcp:") + std::to_string(port), {"forward", std::string("tcp:") + std::to_string(port), std::string("tcp:") + std::to_string(port)}, cb);
}

void PhoneOps::unforward(int port, const DeployCallback& cb)
{
  step_optional(serial_, std::string("adb forward --remove tcp:") + std::to_string(port), {"forward", "--remove", std::string("tcp:") + std::to_string(port)}, cb);
}

// Hook lifecycle

bool PhoneOps::hook_loaded(HostProc target) const
{
  const std::string proc = host_proc_name(target);
  const std::string lib = hook_lib_basename(target);
  const std::string ready = hook_ready_path(target);
  std::string cmd = std::string("found=NO; for p in $(pidof ") + proc +
                    std::string(" 2>/dev/null); do   [ -r /proc/$p/maps ] || continue; "
                                "  if grep -q ") +
                    lib + std::string(" /proc/$p/maps 2>/dev/null && [ -r ") + ready +
                    std::string(" ] && [ \"$(cat ") + ready + std::string(" 2>/dev/null | tr -dc '0-9')\" = \"$p\" ]; then found=YES; break; fi; done; echo $found");
  auto r = AdbClient::instance().run_sync_to(serial_, AdbClient::su_shell(cmd));
  return r.exit_code == 0 && r.stdout_.find("YES") != std::string::npos;
}

bool PhoneOps::inject_lib(HostProc target, const std::string& lib_basename, const std::string& init_fn, const std::string& feed_url, const DeployCallback& cb)
{
  if (hook_loaded(target))
  {
    step(serial_, host_proc_name(target) + std::string(" hook already loaded - reusing"), AdbClient::su_shell("echo reused"), cb);
    return true;
  }

  bool live_camera_inject = false;
  std::string live_camera_clients;
  if (target == HostProc::CameraServer)
  {
    live_camera_clients = active_camera_client_packages();
    live_camera_inject = !live_camera_clients.empty();
    if (live_camera_inject)
    {
      if (cb)
      {
        cb(DeployStatus::info("active camera client detected - live inject"));
        cb(DeployStatus::ok("skip cameraserver restart", live_camera_clients));
      }
    }
    else
    {
      close_camera_clients(/*remember_for_relaunch=*/true, cb);
      std::string restart_cmd = std::string("killall -q cameraserver 2>/dev/null; true");
      if (!step(serial_, "restart cameraserver", AdbClient::su_shell(restart_cmd), cb))
        return false;
      if (!wait_host_proc_alive(target, 3000, cb))
        return false;
    }
  }

  const std::string fn = init_fn.empty() ? default_init_fn(target) : init_fn;
  std::string stream_kind;
  std::string feed_scheme;
  if (!feed_launch_claims(feed_url, stream_kind, feed_scheme))
  {
    if (cb)
      cb(DeployStatus::err("authorize Android launch", "unsupported feed scheme"));
    return false;
  }

  std::string hook_feed_url = feed_url;
  std::string inject = std::string("setenforce 0; LD_LIBRARY_PATH=") + "/data/local/tmp " + "/data/local/tmp/cr_injector" + std::string(" --target ") + host_proc_name(target) + std::string(" --lib /data/local/tmp/") + lib_basename + std::string(" --init ") + fn + std::string(" --feed ") + shell_single_quote(hook_feed_url);

  if (!step(serial_, std::string("inject into ") + host_proc_name(target), AdbClient::su_shell(inject), cb))
  {
    if (live_camera_inject && cb)
      cb(DeployStatus::err("live camera inject failed", "camera app was left running; close the camera app or press Start software before opening it"));
    return false;
  }
  const std::string ready = hook_ready_path(target);
  std::string mark_ready = std::string("mkdir -p /data/cr && chmod 0777 /data/cr && (chcon u:object_r:system_data_file:s0 /data/cr 2>/dev/null || true); set -- $(pidof ") + host_proc_name(target) +
                           std::string(" 2>/dev/null); [ -n \"$1\" ] && echo \"$1\" > ") + ready +
                           std::string(" && chmod 0666 ") + ready +
                           std::string(" && chcon u:object_r:system_data_file:s0 ") + ready;
  if (!step(serial_, std::string("mark ") + host_proc_name(target) + std::string(" hook ready"), AdbClient::su_shell(mark_ready), cb))
    return false;

  if (!hook_loaded(target))
  {
    if (cb)
      cb(DeployStatus::err(std::string("verify hook mapped in ") + host_proc_name(target), std::string("injector exited successfully but ") + hook_lib_basename(target) + " is absent from /proc maps"));
    return false;
  }
  if (cb)
    cb(DeployStatus::ok(std::string("verify hook mapped in ") + host_proc_name(target), hook_lib_basename(target)));

  if (target == HostProc::CameraServer)
  {
    if (live_camera_inject)
    {
      if (cb)
        cb(DeployStatus::ok("live camera inject complete", live_camera_clients));
    }
    else
    {
      relaunch_remembered_camera_clients(cb);
    }
  }
  return true;
}

void PhoneOps::restart_services(std::initializer_list<const char*> names, const DeployCallback& cb)
{
  std::string list;
  for (const char* n : names)
  {
    if (!list.empty())
      list += " ";
    list += n;
  }
  std::string cmd = std::string("killall -q ") + list + std::string(" 2>/dev/null; sleep 0.5; true");
  step(serial_, std::string("restart ") + list, AdbClient::su_shell(cmd), cb);
}

// Camera clients

std::string PhoneOps::active_camera_client_packages() const
{
  std::string cmd = camera_client_packages_probe_script() + std::string("if [ -n \"$pkgs\" ]; then echo \"$pkgs\" | tr '\\n' ' '; else echo ''; fi; true");
  auto r = AdbClient::instance().run_sync_to(serial_, AdbClient::su_shell(cmd));
  if (r.exit_code != 0)
    return {};
  return trim_copy(r.stdout_);
}

bool PhoneOps::close_camera_clients(bool remember_for_relaunch, const DeployCallback& cb)
{
  const std::string persist = remember_for_relaunch ? std::string("echo \"$pkgs\" > /data/local/tmp/cr_last_cam_pkgs;") : ":;";
  const std::string clear_on_empty = remember_for_relaunch ? std::string(": > /data/local/tmp/cr_last_cam_pkgs;") : ":;";
  std::string cmd = camera_client_packages_probe_script() + std::string("if [ -n \"$pkgs\" ]; then ") +
                    persist + " " +
                    std::string("  for p in $pkgs; do am force-stop \"$p\" 2>/dev/null; done;   echo \"closed: $(echo $pkgs | tr '\\n' ' ')\"; "
                                "else ") +
                    clear_on_empty + " " +
                    std::string("  echo 'no camera client'; fi; "
                                "sleep 0.3; true");
  return step(serial_, "close camera clients", AdbClient::su_shell(cmd), cb);
}

void PhoneOps::relaunch_remembered_camera_clients(const DeployCallback& cb)
{
  std::string relaunch = std::string("sleep 0.6; if [ -s /data/local/tmp/cr_last_cam_pkgs ]; then "
                                     "  while read p; do     [ -z \"$p\" ] && continue; "
                                     "    monkey -p \"$p\" -c android.intent.category.LAUNCHER 1       >/dev/null 2>&1 && echo \"launched $p\" || echo \"launch fail: $p\"; "
                                     "  done < /data/local/tmp/cr_last_cam_pkgs; else echo 'nothing to relaunch'; fi; true");
  step(serial_, "relaunch camera app", AdbClient::su_shell(relaunch), cb);
}

// Photo mode sentinel

bool PhoneOps::enable_photo_mode(const DeployCallback& cb)
{
  std::string cmd = std::string("mkdir -p /data/cr && chmod 0777 /data/cr && chcon u:object_r:system_data_file:s0 /data/cr; "
                                "echo 1 > /data/cr/photo_mode && chmod 0644 /data/cr/photo_mode && "
                                "chcon u:object_r:system_data_file:s0 /data/cr/photo_mode && echo ok");
  return step(serial_, "install photo_mode flag", AdbClient::su_shell(cmd), cb);
}

bool PhoneOps::disable_photo_mode(const DeployCallback& cb)
{
  return step(serial_, "remove photo_mode flag", AdbClient::su_shell("rm -f /data/cr/photo_mode 2>/dev/null; echo ok"), cb);
}

} // namespace cr::device
