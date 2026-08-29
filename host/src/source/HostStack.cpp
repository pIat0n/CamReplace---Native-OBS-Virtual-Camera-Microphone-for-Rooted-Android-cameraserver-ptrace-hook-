#include "source/HostStack.h"
#include "device/DeviceStack.h" // for kVideoTcpPort / kRtmpListenPort

#include <memory>

namespace cr::source
{

HostStack& HostStack::instance()
{
  static HostStack s;
  return s;
}

bool HostStack::ensure_running(const std::string& serial, const cr::device::DeployCallback& cb, bool preview_decode)
{
  std::lock_guard<std::mutex> lk(mu_);
  if (obs_ && obs_->running())
  {
    if (preview_decode)
      obs_->set_preview_decode(true);
    return true;
  }

  auto src = std::make_shared<ObsRtmpSource>();
  ObsRtmpSource::Config oc{};
  oc.adb_port = cr::device::kVideoTcpPort;
  oc.rtmp_port = cr::device::kRtmpListenPort;
  oc.stream_key = "cr";
  oc.video_forward = false; // armed below by camera/photo flips
  oc.wire_mode = (FeedController::transport() == Transport::Raw) ? WireMode::Nv21 : WireMode::H264;
  oc.preview_decode = preview_decode;
  if (!src->start(serial, oc, cb))
  {
    if (cb)
      cb(cr::device::DeployStatus::err("obs: start failed"));
    return false;
  }
  obs_ = std::move(src);
  return true;
}

bool HostStack::set_camera_active(bool on)
{
  // Never hold mu_ across set_video_forward — it can block for seconds
  // on connect retries, and the UI polls status() on the same mutex
  // every frame (would freeze all clicks, including Start replace *).
  std::shared_ptr<ObsRtmpSource> obs;
  bool want_forward = false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!on)
    {
      camera_on_ = false;
      if (!obs_)
        return true;
      obs = obs_;
      want_forward = video_should_be_on_locked();
    }
    else
    {
      if (!obs_)
        return false;
      obs = obs_;
      want_forward = true; // camera (or photo) enable always needs forward
    }
  }

  if (!obs->set_video_forward(want_forward))
  {
    // Leave camera_on_ false on failed enable so teardown_if_idle
    // can still drop the OBS source and Stop UI stays coherent.
    return false;
  }

  if (on)
  {
    std::lock_guard<std::mutex> lk(mu_);
    camera_on_ = true;
  }
  return true;
}

bool HostStack::set_photo_active(bool on)
{
  std::shared_ptr<ObsRtmpSource> obs;
  bool want_forward = false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!on)
    {
      photo_on_ = false;
      if (!obs_)
        return true;
      obs = obs_;
      want_forward = video_should_be_on_locked();
    }
    else
    {
      if (!obs_)
        return false;
      obs = obs_;
      want_forward = true; // camera (or photo) enable always needs forward
    }
  }

  if (!obs->set_video_forward(want_forward))
  {
    return false;
  }

  if (on)
  {
    std::lock_guard<std::mutex> lk(mu_);
    photo_on_ = true;
  }
  return true;
}

bool HostStack::start_sound(const AudioPump::Config& cfg, const cr::device::DeployCallback& cb)
{
  std::lock_guard<std::mutex> lk(mu_);
  if (!obs_)
    return false;
  if (!obs_->start_audio(cfg, cb))
    return false;
  sound_on_ = true;
  return true;
}

void HostStack::stop_sound()
{
  std::lock_guard<std::mutex> lk(mu_);
  sound_on_ = false;
  if (obs_)
    obs_->stop_audio();
}

bool HostStack::camera_active() const
{
  std::lock_guard<std::mutex> lk(mu_);
  return camera_on_;
}
bool HostStack::photo_active() const
{
  std::lock_guard<std::mutex> lk(mu_);
  return photo_on_;
}
bool HostStack::sound_active() const
{
  std::lock_guard<std::mutex> lk(mu_);
  return sound_on_;
}
bool HostStack::any_active() const
{
  std::lock_guard<std::mutex> lk(mu_);
  return camera_on_ || sound_on_ || photo_on_;
}

void HostStack::teardown_if_idle()
{
  std::lock_guard<std::mutex> lk(mu_);
  teardown_if_idle_locked();
}

void HostStack::teardown_if_idle_locked()
{
  if (camera_on_ || sound_on_ || photo_on_)
    return;
  if (obs_)
  {
    obs_->stop();
    obs_.reset();
  }
}

void HostStack::shutdown()
{
  std::lock_guard<std::mutex> lk(mu_);
  if (obs_)
  {
    obs_->stop();
    obs_.reset();
  }
  camera_on_ = sound_on_ = photo_on_ = false;
}

ObsRtmpSource::Status HostStack::status() const
{
  std::lock_guard<std::mutex> lk(mu_);
  if (obs_)
    return obs_->status();
  return {};
}

} // namespace cr::source
