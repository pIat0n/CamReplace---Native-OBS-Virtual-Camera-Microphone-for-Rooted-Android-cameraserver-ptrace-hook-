#pragma once

// HostStack — the PC-side half of the camera-replace machinery.
// Owns:
//   * the (at most one) live ObsRtmpSource
//   * the three mode flags (camera_on / sound_on / photo_on)
//   * the wire_mode (H264 NAL forward vs. PC-decode + NV21 forward)
// Coexistence rules enforced internally:
//   * camera + sound  : both can run together. video_forward=true,
//                       audio pump on.
//   * camera + photo  : both can run together. video_forward=true.
//   * sound + photo   : both can run together. video_forward=true (photo
//                       relies on the same camhook BLOB substitution).
//   * (the UI also exposes Stop replace * which decrements one flag at
//     a time. video_forward stays on as long as camera OR photo is up.)
// HostStack does NOT touch the phone — it only orchestrates the local
// OBS source. DeviceStack handles the phone-side stuff. FeedController
// glues them.

#include "device/DeployStatus.h"
#include "source/AudioPump.h"
#include "source/FeedController.h"
#include "source/ObsRtmpSource.h"

#include <memory>
#include <mutex>
#include <string>

namespace cr::source
{

class HostStack
{
public:
  static HostStack& instance();

  // Bring up the RTMP listener if it isn't already. Idempotent.
  // Snapshots the current Transport into the ObsRtmpSource config
  // at start time — UI prevents transport switching while a mode
  // is active, so this stays valid for the session.
  bool ensure_running(const std::string& serial, const cr::device::DeployCallback& cb, bool preview_decode);

  // Mode flips. Each call updates the internal flag and drives
  // ObsRtmpSource::set_video_forward / start_audio / stop_audio
  // accordingly. video_forward stays on as long as camera_on || photo_on.
  bool set_camera_active(bool on);
  bool set_photo_active(bool on);
  bool start_sound(const AudioPump::Config& cfg, const cr::device::DeployCallback& cb);
  void stop_sound();

  // State queries (cheap — short lock).
  bool camera_active() const;
  bool photo_active() const;
  bool sound_active() const;
  bool any_active() const;

  // If no mode is active, drop the ObsRtmpSource so the next Start
  // gets a fresh state. No-op when at least one mode is still up.
  void teardown_if_idle();

  // Synchronous full teardown. Used by FeedController::shutdown_all.
  void shutdown();

  // UI snapshot (rtmp URL, connection dot, resolution overlay).
  ObsRtmpSource::Status status() const;

private:
  HostStack() = default;
  void teardown_if_idle_locked();
  bool video_should_be_on_locked() const
  {
    return camera_on_ || photo_on_;
  }

  mutable std::mutex mu_;
  std::shared_ptr<ObsRtmpSource> obs_;
  bool camera_on_ = false;
  bool sound_on_ = false;
  bool photo_on_ = false;
};

} // namespace cr::source
