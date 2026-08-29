#include "source/FeedController.h"
#include "source/HostStack.h"
#include "device/DeviceScanner.h"
#include "device/DeviceStack.h"
#include "util/Log.h"

#include <atomic>
#include <exception>
#include <memory>
#include <vector>
#include <thread>
#include <type_traits>
#include <utility>
namespace cr::source
{

namespace
{

cr::device::FeedScheme video_scheme_for(Transport tr)
{
  return tr == Transport::Raw ? cr::device::FeedScheme::Nv21 : cr::device::FeedScheme::H264;
}

void report_worker_failure(const std::string& action, const char* phase, const std::string& detail, const cr::device::DeployCallback& cb)
{
  std::string step = action;
  step.append(phase);

  std::string line = step;
  if (!detail.empty())
  {
    line.append(" - ");
    line.append(detail);
  }
  cr::log::error("deploy", line);

  if (cb)
  {
    cb(cr::device::DeployStatus::err(std::move(step), detail));
  }
}

template <class Fn> void launch_worker(std::string action, cr::device::DeployCallback fail_cb, Fn&& fn)
{
  using FnT = std::decay_t<Fn>;
  struct WorkerJob
  {
    std::string action;
    cr::device::DeployCallback fail_cb;
    FnT fn;

    void operator()()
    {
      try
      {
        fn();
      }
      catch (const std::exception& e)
      {
        report_worker_failure(action, ": operation failed", e.what(), fail_cb);
      }
      catch (...)
      {
        report_worker_failure(action, ": operation failed", "unknown exception", fail_cb);
      }
    }
  };

  auto job = std::make_shared<WorkerJob>(WorkerJob{
      std::move(action),
      std::move(fail_cb),
      std::forward<Fn>(fn),
  });
  auto run = [job]() mutable { (*job)(); };

  try
  {
    std::thread(run).detach();
  }
  catch (const std::exception& e)
  {
    cr::log::warn("deploy", job->action + ": worker thread unavailable; running synchronously - " + e.what());
    run();
  }
  catch (...)
  {
    cr::log::warn("deploy", job->action + ": worker thread unavailable; running synchronously - unknown exception");
    run();
  }
}

void cleanup_log_status(cr::device::DeployStatus s)
{
  std::string line = s.step;
  if (!s.detail.empty())
  {
    std::string detail = s.detail;
    for (char& c : detail)
      if (c == '\n' || c == '\r')
        c = ' ';
    line.append(" - ");
    line.append(detail);
  }
  using K = cr::device::DeployStatus::Kind;
  if (s.kind == K::Err)
    cr::log::warn("cleanup", line);
  else if (s.kind == K::Ok)
    cr::log::ok("cleanup", line);
  else
    cr::log::info("cleanup", line);
}

std::vector<std::string> known_authorized_serials()
{
  std::vector<std::string> out;
  for (const auto& d : cr::device::DeviceScanner::instance().snapshot())
  {
    if (d.auth != cr::device::AuthState::Authorized || d.serial.empty())
      continue;
    bool seen = false;
    for (const auto& serial : out)
    {
      if (serial == d.serial)
      {
        seen = true;
        break;
      }
    }
    if (!seen)
      out.push_back(d.serial);
  }
  return out;
}

} // namespace

// Transport selection (process-wide; UI prevents flip while a mode is up)

namespace
{
std::atomic<Transport> g_transport{Transport::Compressed};
}

void FeedController::set_transport(Transport t)
{
  Transport prev = g_transport.exchange(t);
  if (prev != t)
  {
    cr::log::info("transport", std::string("switched to ") + (t == Transport::Raw ? "Raw (NV21 over PC decode)" : "Compressed (RTMP H.264)"));
  }
}

Transport FeedController::transport()
{
  return g_transport.load();
}

// Software lifecycle (Install / Delete / Start / Stop)

void FeedController::install_software(std::string serial, cr::device::DeployCallback cb)
{
  launch_worker("install software", cb,
                [serial = std::move(serial), cb = std::move(cb)]()
                {
                  if (!cr::device::DeviceStack::instance().install(serial, cb))
                    return;
                  if (cb)
                    cb(cr::device::DeployStatus::ok("install: software pushed", "press Start software to load hooks"));
                });
}

void FeedController::delete_software(std::string serial, cr::device::DeployCallback cb)
{
  HostStack::instance().shutdown();
  cr::device::DeviceStack::instance().reset_session();
  launch_worker("delete software", cb,
                [serial = std::move(serial), cb = std::move(cb)]()
                {
                  if (!cr::device::DeviceStack::instance().uninstall(serial, cb))
                    return;
                  if (cb)
                    cb(cr::device::DeployStatus::ok("delete: software removed"));
                });
}

void FeedController::start_software(std::string serial, cr::device::DeployCallback cb)
{
  launch_worker("start software", cb,
                [serial = std::move(serial), cb = std::move(cb)]()
                {
                  if (!cr::device::DeviceStack::instance().load_hooks(serial, video_scheme_for(transport()), cb))
                    return;
                  if (cb)
                    cb(cr::device::DeployStatus::ok("software running", "hooks loaded; press Start replace * to arm a substitution"));
                });
}

void FeedController::stop_software(std::string serial, cr::device::DeployCallback cb)
{
  HostStack::instance().shutdown();
  cr::device::DeviceStack::instance().reset_session();
  launch_worker("stop software", cb,
                [serial = std::move(serial), cb = std::move(cb)]()
                {
                  cr::device::DeviceStack::instance().unload_hooks(serial,
                                                                   /*relaunch_clients=*/true, cb);
                  if (cb)
                    cb(cr::device::DeployStatus::ok("software stopped", "hooks unloaded; artifacts kept (use Delete to wipe)"));
                });
}

// Replace camera

void FeedController::start_replace_camera(std::string serial, cr::device::DeployCallback cb)
{
  launch_worker("start replace camera", cb,
                [serial = std::move(serial), cb = std::move(cb)]()
                {
                  auto& dev = cr::device::DeviceStack::instance();
                  auto& host = HostStack::instance();

                  if (!host.ensure_running(serial, cb, true))
                    return;
                  if (!dev.arm_video(serial, video_scheme_for(transport()), cb))
                  {
                    host.teardown_if_idle();
                    return;
                  }
                  if (!host.set_camera_active(true))
                  {
                    if (cb)
                      cb(cr::device::DeployStatus::err("camera: enable forward failed"));
                    cr::device::DeviceStack::instance().disarm_video(serial, cb);
                    host.teardown_if_idle();
                    return;
                  }
                  if (cb)
                    cb(cr::device::DeployStatus::ok("OBS RTMP ready", "rtmp://127.0.0.1:1935/live/cr"));
                });
}

void FeedController::stop_replace_camera(std::string serial, cr::device::DeployCallback cb)
{
  launch_worker("stop replace camera", cb,
                [serial = std::move(serial), cb = std::move(cb)]()
                {
                  auto& host = HostStack::instance();
                  const bool also_photo = host.photo_active();
                  host.set_camera_active(false);
                  host.teardown_if_idle();

                  // Photo mode also needs the camhook live + shm filled, so only
                  // disarm the phone-side video pipeline if photo isn't holding it.
                  if (!also_photo)
                  {
                    cr::device::DeviceStack::instance().disarm_video(serial, cb);
                  }
                  if (cb)
                    cb(cr::device::DeployStatus::ok("camera: stopped"));
                });
}

// Replace photo

void FeedController::start_replace_photo(std::string serial, cr::device::DeployCallback cb)
{
  launch_worker("start replace photo", cb,
                [serial = std::move(serial), cb = std::move(cb)]()
                {
                  auto& dev = cr::device::DeviceStack::instance();
                  auto& host = HostStack::instance();

                  if (!host.ensure_running(serial, cb, true))
                    return;
                  if (!dev.arm_video(serial, video_scheme_for(transport()), cb))
                  {
                    host.teardown_if_idle();
                    return;
                  }
                  if (!host.set_photo_active(true))
                  {
                    if (cb)
                      cb(cr::device::DeployStatus::err("photo: enable forward failed"));
                    cr::device::DeviceStack::instance().disarm_video(serial, cb);
                    host.teardown_if_idle();
                    return;
                  }
                  if (!dev.enable_photo_mode(serial, cb))
                    return;

                  if (cb)
                    cb(cr::device::DeployStatus::ok("Photo replace ready", "Take a photo in any camera app - JPEG will be encoded from the live OBS stream."));
                });
}

void FeedController::stop_replace_photo(std::string serial, cr::device::DeployCallback cb)
{
  launch_worker("stop replace photo", cb,
                [serial = std::move(serial), cb = std::move(cb)]()
                {
                  auto& host = HostStack::instance();
                  const bool also_camera = host.camera_active();
                  host.set_photo_active(false);
                  host.teardown_if_idle();

                  auto& dev = cr::device::DeviceStack::instance();
                  dev.disable_photo_mode(serial, cb);
                  if (!also_camera)
                  {
                    dev.disarm_video(serial, cb);
                  }
                  if (cb)
                    cb(cr::device::DeployStatus::ok("photo: stopped"));
                });
}

// Replace sound

void FeedController::start_replace_sound(std::string serial, cr::device::DeployCallback cb)
{
  launch_worker("start replace sound", cb,
                [serial = std::move(serial), cb = std::move(cb)]()
                {
                  auto& dev = cr::device::DeviceStack::instance();
                  auto& host = HostStack::instance();

                  if (!host.ensure_running(serial, cb, false))
                  {
                    if (cb)
                      cb(cr::device::DeployStatus::err("audio: rtmp pipeline bring-up failed"));
                    return;
                  }
                  if (!dev.arm_audio(serial, cb))
                    return;

                  AudioPump::Config ac{};
                  ac.adb_port = cr::device::kAudioTcpPort;
                  // Keep AAC decoder-native PCM on the host. audhook adapts to the
                  // consumer format, so forcing 48 kHz/stereo here just adds an
                  // avoidable resample before the phone may resample again.
                  ac.target_sr = 0;
                  ac.target_ch = 0;
                  if (!host.start_sound(ac, cb))
                  {
                    if (cb)
                      cb(cr::device::DeployStatus::err("audio: pump start failed"));
                    return;
                  }
                  if (cb)
                    cb(cr::device::DeployStatus::ok("OBS audio replace ready"));
                });
}

void FeedController::stop_replace_sound(std::string serial, cr::device::DeployCallback cb)
{
  launch_worker("stop replace sound", cb,
                [serial = std::move(serial), cb = std::move(cb)]()
                {
                  auto& host = HostStack::instance();
                  host.stop_sound();
                  host.teardown_if_idle();
                  cr::device::DeviceStack::instance().disarm_audio(serial, cb);
                  if (cb)
                    cb(cr::device::DeployStatus::ok("sound: stopped"));
                });
}

// Status / shutdown

ObsRtmpSource::Status FeedController::obs_status()
{
  return HostStack::instance().status();
}

void FeedController::shutdown_all()
{
  cleanup_known_devices("app shutdown");
}

void FeedController::cleanup_known_devices(const char* reason)
{
  // Synchronous - joins ObsRtmpSource worker threads so adb subprocesses
  // don't outlive App::run(), then wipes phone-side state while adb is
  // still alive.
  HostStack::instance().shutdown();

  const auto serials = known_authorized_serials();
  if (serials.empty())
  {
    cr::log::info("cleanup", std::string(reason ? reason : "cleanup") + ": no authorized devices");
    cr::device::DeviceStack::instance().reset_session();
    return;
  }

  for (const auto& serial : serials)
  {
    cr::log::info("cleanup", std::string(reason ? reason : "cleanup") + ": wiping " + serial);
    if (!cr::device::DeviceStack::instance().cleanup(serial, cleanup_log_status))
    {
      cr::log::warn("cleanup", "device cleanup reported failure for " + serial);
    }
  }
}

} // namespace cr::source
