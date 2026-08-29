#pragma once

// FeedController — orchestrates the on-phone camera-replace stack and the
// PC-side OBS source.
// The stack has two halves:
//   Software (device-side, persistent):
//     1. Push artifacts (cr_feed_proc, libcr_feed.so, libcr_camhook.so,
//        cr_injector) to /data/local/tmp.
//     2. Launch cr_feed_proc with `tcp_h264:listen:8901` so it's ready to
//        receive H.264 NAL units over the adb-forward tunnel.
//     3. Inject libcr_camhook.so into cameraserver via cr_injector. After
//        this the phone's camera APIs return our shm-backed frames.
//   Replace camera (PC-side, per-session):
//     Open a local RTMP listener on 127.0.0.1:1935. OBS publishes H.264
//     to that endpoint and we pass each NAL unit straight through to
//     cr_feed_proc on the phone — no decode/re-encode on PC.
// All entry points are fire-and-forget; progress lines flow back via the
// DeployCallback so the UI panel can render them.

#include "device/DeployStatus.h"
#include "source/ObsRtmpSource.h"

#include <string>

namespace cr::source
{

// How OBS hands video frames to us.
//   Compressed — current default. OBS encodes H.264+AAC via its own
//                encoder, publishes RTMP to 127.0.0.1:1935. PC RtmpServer
//                parses NALUs, ships them over CRH2 TCP to the phone,
//                cr_feed_proc decodes via AMediaCodec into NV21.
//                ~80 Mbps wire, fits USB 2.0; ~80–120 ms encode+decode
//                latency; 4:2:0 DCT artefacts.
//   Raw        — OBS uses Custom-Output (FFmpeg) with the yuv4mpegpipe
//                muxer + rawvideo codec to a separate TCP endpoint. PC
//                receives Y4M-framed yuv420p frames, converts to NV21
//                in-place, ships them over CRH2 TCP to the phone, and
//                cr_feed_proc on the new tcp_nv21 scheme memcpy's
//                straight into shm. No encode/decode, no DCT artefacts;
//                ~750 Mbps for 1080p30 — does NOT fit a USB 2.0 link.
// Audio is unaffected by this enum: AAC over RTMP either way (the user
// asked for it explicitly — voice path was already low-latency enough).
enum class Transport
{
  Compressed,
  Raw,
};

class FeedController
{
public:
  // --- Transport selection ---------------------------------------------
  // Process-wide; default Compressed. Must only be flipped while no
  // replace_* mode is active (UI enforces this — the dropdown next to
  // Launch OBS is disabled while camera/sound/photo are running).
  static void set_transport(Transport t);
  static Transport transport();

  // --- "Software" lifecycle --------------------------------------------
  // Push binaries + .so + relabel. Doesn't run anything. Safe to repeat.
  static void install_software(std::string serial, cr::device::DeployCallback cb);

  // Inverse of install_software — kills any running cr_feed_proc, drops
  // injected hooks (by restarting cameraserver + audioserver so their
  // process maps lose libcr_camhook.so / libcr_audhook.so), and wipes
  // every artifact we ever pushed. Idempotent: safe to call when nothing
  // was installed.
  static void delete_software(std::string serial, cr::device::DeployCallback cb);

  // Push (idempotently) + launch cr_feed_proc + inject the camhook into
  // cameraserver. After this the phone is wired up: a later
  // start_replace_camera just hooks the OBS source up to the existing
  // tunnel without restarting the device stack.
  static void start_software(std::string serial, cr::device::DeployCallback cb);

  // Tear down both the device stack and any PC-side OBS source.
  static void stop_software(std::string serial, cr::device::DeployCallback cb);

  // --- "Replace camera" lifecycle --------------------------------------
  // Bring up the device stack (if not already) and start the local OBS
  // RTMP listener. Once OBS connects, frames flow through to the phone.
  static void start_replace_camera(std::string serial, cr::device::DeployCallback cb);

  // Mirror of stop_software — kept as its own name for UI symmetry.
  static void stop_replace_camera(std::string serial, cr::device::DeployCallback cb);

  // --- "Replace photo (still capture)" lifecycle -----------------------
  // Source is the same OBS RTMP stream that camera replacement uses:
  // cr_feed_proc decodes the H.264 frames into /data/cr/feed shm, and
  // the camhook in photo-mode catches BLOB still-capture buffers and
  // fills them with a freshly-encoded JPEG of the latest NV21 slot.
  // Pipeline:
  //   1. Bring up the RTMP listener (idempotent — reuses what
  //      Start replace camera already started, if any).
  //   2. Touch /data/cr/photo_mode (sentinel file the hook checks at
  //      init) so the next inject enables BLOB substitution.
  //   3. (Re-)inject libcr_camhook.so so it picks up photo_mode.
  // Coexists with camera+sound replacement — preview YUV and saved
  // JPEG both get OBS content from the same shm.
  static void start_replace_photo(std::string serial, cr::device::DeployCallback cb);

  static void stop_replace_photo(std::string serial, cr::device::DeployCallback cb);

  // --- "Replace sound" lifecycle ---------------------------------------
  // Forwards on the existing OBS RTMP listener and additionally:
  //   * adb forward tcp:8902
  //   * launches a second cr_feed_proc instance with `tcp_pcm:listen:8902`
  //     that fills /data/cr/audio
  //   * injects libcr_audhook.so into audioserver
  //   * arms the AudioPump so AAC frames from the OBS stream are
  //     decoded → S16LE PCM → tcp_pcm tunnel
  // Independent of camera replacement: each can be on without the other,
  // sharing the same OBS RTMP listener.
  static void start_replace_sound(std::string serial, cr::device::DeployCallback cb);

  // Tear down the audio side: stop the audio pump, kill the audio
  // cr_feed_proc instance, leave camera path untouched.
  static void stop_replace_sound(std::string serial, cr::device::DeployCallback cb);

  // UI snapshot of the active OBS source (empty if not running). Used by
  // the status panel to render the RTMP URL + connection indicator.
  static ObsRtmpSource::Status obs_status();

  // Synchronous teardown: stops the active ObsRtmpSource and joins
  // its worker threads, then wipes phone-side artifacts on every known
  // authorized device. No callbacks are emitted to UI (it is going away).
  // Called from App::run() during app shutdown so adb subprocesses
  // don't outlive the main loop and no Android payload remains behind.
  static void shutdown_all();

  // Fail-closed cleanup for transport failure and crash
  // recovery. Synchronous and idempotent.
  static void cleanup_known_devices(const char* reason);
};

} // namespace cr::source
