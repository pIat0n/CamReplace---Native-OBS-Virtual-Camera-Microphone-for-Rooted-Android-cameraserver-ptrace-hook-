#pragma once

// Single shared log sink for everything the app produces:
//   * cr::log::{info,warn,error}  → pushed via util/Log.cpp
//   * DeployStatus steps          → pushed from UI/controllers
//   * "our" logcat (cr_* tags)    → pushed from LogTail (UI + file)
//   * "camera" logcat (cameraserver, Camera*, ...) → pushed from LogTail
//                                  via append_camera (file only, NOT UI)
// Keeps a bounded in-memory ring the UI reads for display, AND mirrors
// every line to one log artifact at %LOCALAPPDATA%/CameraReplace so
// end-users hitting trouble can hand over a single file that contains
// host events + phone-side chatter together. Header at the top of the
// file captures the host OS environment; DEVICE PROBED blocks landed by
// DeviceScanner capture model / Android / build / SELinux / root state.
// Together they let a maintainer answer "which device, which Android,
// which firmware, what went wrong" from one artifact.

#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace cr::util
{

enum class LogKind : uint8_t
{
  Info,
  Warn,
  Error,
  Ok,     // completed step / positive status (green in the UI)
  Device, // logcat line from the phone
};

struct LogEntry
{
  LogKind kind = LogKind::Info;
  std::string text; // single line, no trailing newline
};

class SessionLog
{
public:
  static SessionLog& instance();

  // Push a line to the UI ring + session log artifact. Thread-safe.
  void push(LogKind k, std::string line);

  // Write a phone-side camera-tag line to the session log only — bypasses
  // the UI ring because cameraserver / QCamera / etc. chatter would
  // drown out the events the user actually reads. Still ends up in
  // the same file so end-user-submitted logs are self-contained.
  void append_camera(std::string line);

  // Write a free-form raw block (no timestamp / level prefix) to
  // the session log only. Used for the device-probed multi-line section
  // so the report has visually distinct framing. UI ring untouched.
  void write_block(std::string text);

  // Release-only: append an already encrypted Android CRLG record captured
  // from neutral logcat transport. Dev builds return false and keep the
  // plaintext LogTail flow.
  bool append_encrypted_android_record_base64(std::string_view encoded);

  // Latest N entries for the UI, oldest first.
  std::vector<LogEntry> snapshot() const;

  // Clear the UI ring (file is left alone).
  void clear();

  // Ring capacity.
  static constexpr std::size_t kCapacity = 1000;

  // Path — exposed so the UI can show "logs at …" hints if useful.
  std::filesystem::path session_log_path() const
  {
    return session_path_;
  }

private:
  SessionLog();
  ~SessionLog();
  SessionLog(const SessionLog&) = delete;
  SessionLog& operator=(const SessionLog&) = delete;

  void write_file_line_(std::FILE* fp, LogKind k, const std::string& s);
  void write_session_header_();

  // True if `line` is interesting enough to display in the UI panel.
  // Errors / OK / Device / Warn always pass; Info is filtered against a
  // denylist of housekeeping/per-frame chatter. The full line is still
  // written to the session log either way.
  static bool should_show_in_ui_(LogKind k, const std::string& line);

  mutable std::mutex mu_;
  std::deque<LogEntry> ring_;
  std::filesystem::path session_path_;
  std::FILE* session_fp_ = nullptr;
};

} // namespace cr::util
