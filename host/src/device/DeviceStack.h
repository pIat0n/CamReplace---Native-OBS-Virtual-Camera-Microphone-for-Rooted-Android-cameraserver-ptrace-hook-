/*
 * Назначение файла:
 *   DeviceStack — публичный host-side фасад lifecycle для установки,
 *   загрузки hooks и запуска video/audio/photo режимов на rooted Pixel.
 *
 * Hook point и ABI assumptions:
 *   класс не раскрывает phone-side structs и не меняет shared memory v1.
 *   Он только выбирает feed scheme, порты и exported init functions, которые
 *   PhoneOps передаёт cr_injector. Public C++ API оставлен совместимым.
 *
 * Ограничения Pixel/Android:
 *   video schemes tcp_h264/tcp_nv21 используют один порт, поэтому перед
 *   переключением нужно останавливать старый cr_feed_proc и ждать факта
 *   остановки. Audio tcp_pcm независим, но readiness также проверяется через
 *   процесс и log listener, а не через fixed sleep.
 *
 * Подтверждено:
 *   Camera replacement идёт через cameraserver/camhook, audio replacement —
 *   через audioserver/audhook. Photo mode — sentinel-файл /data/cr/photo_mode.
 *
 * Diagnostic-only:
 *   load_hooks без feed process оставляет hooks в pass-through; успешный API
 *   вызов не означает, что кадры или PCM уже заменяются.
 */

#pragma once

// DeviceStack — the phone-side half of the camera-replace machinery.
// Owns:
//   * `artifacts_pushed_` per-session flag (idempotent push)
//   * the bring-up mutex that serialises concurrent start_replace_*
//   * the port constants the cr_feed_proc tunnels listen on
// Delegates ALL shell composition to `PhoneOps`. DeviceStack itself is
// just sequencing: "for arm_video — forward port, kill the other-scheme
// cr_feed_proc if running, launch this one, inject camhook". Each step
// is a single PhoneOps call.
// Why split this out of FeedController: the old monolith mixed
// phone-side orchestration with host-side state (ObsRtmpSource lifecycle,
// transport selection). After the split FeedController is ~150 lines of
// pure intent ("the user pressed Start replace camera, do these two
// things and update mode flags") and DeviceStack/HostStack each own
// their own world.

#include "device/PhoneOps.h"
#include "device/DeployStatus.h"

#include <mutex>
#include <string>

namespace cr::device
{

// adb-forward ports for the host↔phone tunnels.
// kVideoTcpPort is reused for both transports (Compressed and Raw) —
// cr_feed_proc on the phone runs ONE scheme at a time, so the two
// can't race on the port. The host-side TcpFeedClient sends a
// different magic + payload format depending on which scheme is
// listening.
constexpr int kVideoTcpPort = 8901;   // → /data/cr/feed
constexpr int kAudioTcpPort = 8902;   // → /data/cr/audio
constexpr int kRtmpListenPort = 1935; // PC-side RtmpServer (OBS dest)

// Build the feed URL string for a given scheme. cr_feed_proc parses
// these in feed_main.cpp.
std::string feed_url_for(FeedScheme scheme);

class DeviceStack
{
public:
  static DeviceStack& instance();

  // Drop the per-session "artifacts_pushed" flag. Call from
  // shutdown_all / uninstall so the next push runs the clean step.
  void reset_session();

  // === Software lifecycle ===
  // Pure file copy. No service restarts, no hooks loaded.
  bool install(const std::string& serial, const DeployCallback& cb);

  // Drop hooks (via service restart) + rm artifacts + unforward all
  // adb-forward rules we ever installed.
  bool uninstall(const std::string& serial, const DeployCallback& cb);

  // Best-effort destructive cleanup. Same phone-side wipe as uninstall,
  // Exposed for app shutdown and crash cleanup.
  // recovery before the next push.
  bool cleanup(const std::string& serial, const DeployCallback& cb);

  // Inject camhook + audhook into their respective system services.
  // Hooks sit in pass-through mode (shm magic == 0 → fall through).
  // No cr_feed_proc launched. video_scheme is just for diagnostic
  // labelling on the camhook init — the runtime shm path is hardcoded.
  bool load_hooks(const std::string& serial, FeedScheme video_scheme, const DeployCallback& cb);

  // Restart cameraserver + audioserver to drop our .so out of their
  // address spaces. `relaunch_clients=true` re-monkey-launches camera
  // apps we closed during the kill.
  bool unload_hooks(const std::string& serial, bool relaunch_clients, const DeployCallback& cb);

  // === Per-mode arming (for replace_*) ===
  // Forward port + launch cr_feed_proc + ensure camhook is loaded.
  // Idempotent on hook load. Kills any other-scheme cr_feed_proc
  // first (Compressed↔Raw share the port).
  bool arm_video(const std::string& serial, FeedScheme scheme, const DeployCallback& cb);

  // Kill cr_feed_proc(video) + zero feed shm magic. Hook stays loaded
  // and falls through to real sensor. Audio side untouched.
  void disarm_video(const std::string& serial, const DeployCallback& cb);

  // Forward port + launch cr_feed_proc(tcp_pcm) + ensure audhook is
  // loaded. Idempotent.
  bool arm_audio(const std::string& serial, const DeployCallback& cb);

  // Kill cr_feed_proc(audio) + zero audio shm magic. Audhook stays
  // loaded and falls through to real mic. Video side untouched.
  void disarm_audio(const std::string& serial, const DeployCallback& cb);

  // Touch / remove the photo_mode sentinel under /data/cr.
  bool enable_photo_mode(const std::string& serial, const DeployCallback& cb);
  bool disable_photo_mode(const std::string& serial, const DeployCallback& cb);

private:
  DeviceStack() = default;

  // Push artifacts if not pushed yet this session.
  bool prepare_artifacts_(PhoneOps& ops, const DeployCallback& cb);
  bool cleanup_device_(PhoneOps& ops, const DeployCallback& cb);

  // Serialises concurrent start_replace_* threads through arm_video /
  // arm_audio so push_artifacts and inject can't interleave.
  std::mutex bring_up_mu_;

  std::mutex state_mu_;
  bool artifacts_pushed_ = false;
};

} // namespace cr::device
