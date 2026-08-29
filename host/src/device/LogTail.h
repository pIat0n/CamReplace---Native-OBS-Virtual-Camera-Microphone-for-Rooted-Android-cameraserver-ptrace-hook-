#pragma once

// LogTail — two parallel `adb logcat` streams per device.
//   * "our" stream  (tags: cr_camhook, cr_feed, cr_injector) →
//     pushed into cr::util::SessionLog as Device-kind entries so the
//     unified log panel and session log both see them.
//   * "camera" stream (tags: cameraserver, Camera*, QCamera*, etc.) →
//     appended to the session log only, never shown in the UI. Keeps the
//     phone's verbose camera chatter out of the user's face but still
//     available for post-mortem.
// Both streams run on their own reader threads inside AdbClient. Starting
// the tail on a new serial stops the previous one, so we never have two
// overlapping subscriptions.

#include "device/AdbClient.h"

#include <mutex>
#include <string>

namespace cr::device
{

class LogTail
{
public:
  static LogTail& instance();

  // Start tailing on the given serial. Idempotent — restarts if already
  // running for a different serial.
  void start(std::string serial);
  void stop();

  bool running() const noexcept;

private:
  LogTail() = default;
  ~LogTail();

  void on_our_line(std::string line);
  void on_camera_line(std::string line);

  mutable std::mutex mu_;
  std::string serial_;
  AdbStream our_stream_;
  AdbStream cam_stream_;
};

} // namespace cr::device
