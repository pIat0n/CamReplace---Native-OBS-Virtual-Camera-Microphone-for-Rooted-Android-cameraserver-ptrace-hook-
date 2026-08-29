#include "device/LogTail.h"
#include "util/Log.h"
#include "util/SessionLog.h"

#include <string_view>

namespace cr::device
{

LogTail& LogTail::instance()
{
  static LogTail t;
  return t;
}

LogTail::~LogTail()
{
  stop();
}

void LogTail::start(std::string serial)
{
  std::lock_guard<std::mutex> lk(mu_);
  if (serial_ == serial && our_stream_.is_running())
    return;

  our_stream_.stop();
  cam_stream_.stop();
  serial_ = std::move(serial);

  // --- our stream -> UI + session.log -------------------------------------
  // Only the three tags we produce; silence everything else with *:S so
  // the reader thread doesn't spend its time on noise.
  std::vector<std::string> our_args = {
      "-s", serial_, "logcat", "-v", "brief", "-T", "1", "cr_injector:*", "cr_feed:*", "cr_camhook:*", "cr_audhook:*", "cr_launch:*", "*:S",
  };
  our_stream_ = AdbClient::instance().run_streaming(our_args, [this](std::string line) { on_our_line(std::move(line)); }, [](int code) { cr::log::info("logcat", "our logcat stream ended code=" + std::to_string(code)); });

  // --- camera stream -> session log only ----------------------------------
  // Broad net on Qualcomm phones: cameraserver, camera HAL tags, QCamera
  // components. `-v time` for easier correlation with our log.
  std::vector<std::string> cam_args = {
      "-s", serial_, "logcat", "-v", "time", "-T", "1", "cameraserver:*", "CameraService:*", "CAM_*:*", "QCamera*:*", "QCamera3*:*", "Camera3-Stream:*", "Camera3-Device:*", "CamX:*", "CHI:*", "ImageReader*:*", "*:S",
  };
  cam_stream_ = AdbClient::instance().run_streaming(cam_args, [this](std::string line) { on_camera_line(std::move(line)); }, nullptr);
}

void LogTail::stop()
{
  std::lock_guard<std::mutex> lk(mu_);
  our_stream_.stop();
  cam_stream_.stop();
  serial_.clear();
}

bool LogTail::running() const noexcept
{
  return our_stream_.is_running() || cam_stream_.is_running();
}

void LogTail::on_our_line(std::string line)
{
  // Tag with synthetic "phone" component so it lines up visually with
  // host-side `[component] msg` entries.
  std::string composed = "[phone] ";
  composed.append(line);
  cr::util::SessionLog::instance().push(cr::util::LogKind::Device, std::move(composed));
}

void LogTail::on_camera_line(std::string line)
{
  // File-only sink. "camera" vs "phone" lets a reader tell our cr_*
  // tag stream apart from the verbose cameraserver chatter.
  std::string composed = "[camera] ";
  composed.append(line);
  cr::util::SessionLog::instance().append_camera(std::move(composed));
}

} // namespace cr::device
