#pragma once

// Device discovery + per-device capability probing.
// Thread model: the scanner owns a lightweight background thread that polls
// `adb devices -l` and probes per-device capabilities (root, arch, SDK, model)
// when a new serial appears. The UI thread pulls immutable snapshots via
// snapshot() — cheap, copy-on-write-ish.

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cr::device
{

enum class AuthState
{
  Unknown,
  Unauthorized,
  Authorized,
  Offline
};

struct DeviceInfo
{
  std::string serial;
  AuthState auth = AuthState::Unknown;

  // Populated once authorised.
  std::string model;        // "Pixel 4 XL" / "Redmi Note 8T"
  std::string device;       // "coral" / "willow"
  std::string brand;        // "Google" / "Xiaomi"
  std::string manufacturer; // "Google" / "Xiaomi"
  std::string android_ver;  // "13"
  int sdk_level = 0;        // 33
  std::string abi;          // "arm64-v8a"
  bool arm64 = false;
  std::string build_id;    // "TQ3A.230805.001"
  std::string fingerprint; // "google/coral/coral:13/TQ3A.230805.001/9302419:user/release-keys"
  std::string android_id;
  std::string serial_prop;
  std::string boot_serial;
  std::string hardware;
  std::string boot_hardware;
  std::string board;
  std::string soc_model;
  std::string security_patch;
  std::string vbmeta_digest;
  std::string verified_boot_state;
  std::string kernel;  // "Linux 4.14.276-..."
  bool rooted = false; // "su -c id" -> uid=0
  bool selinux_enforcing = true;
  std::string selinux_mode; // "Enforcing" / "Permissive"

  // Negotiated USB speed of the phone's UDC, read from
  // /sys/class/udc/*/current_speed and mapped to a friendly label.
  // Empty if we couldn't read it (no root, missing path, …).
  //   "USB 1.1 (FS, 12 Mbps)"
  //   "USB 2.0 (HS, 480 Mbps)"
  //   "USB 3.0 (SS, 5 Gbps)"
  //   "USB 3.1 (SS+, 10 Gbps)"
  // The bandwidth ceiling matters because raw-NV21 transport over
  // adb-forward is bottlenecked at exactly this speed.
  std::string usb_speed;

  bool probed = false; // true once capability scan done
  bool phone_fingerprint_uploaded = false;
};

class DeviceScanner
{
public:
  static DeviceScanner& instance();

  // Kick off background polling. Idempotent.
  void start();
  void stop();

  // Thread-safe snapshot copy.
  std::vector<DeviceInfo> snapshot() const;

  // Force a rescan now (without waiting for the periodic tick).
  // `deep == true` cycles the adb server (kill + start) and pokes every
  // unauthorised device with a harmless `shell echo` — this is what makes
  // the "Allow USB debugging?" RSA prompt reappear on the phone when the
  // host key was revoked or the previous prompt was dismissed. Use from
  // user-initiated refresh; the periodic tick stays light-weight.
  void refresh_now(bool deep = false);

  // True while a deep refresh is in progress (server cycle + poke). The UI
  // can read this to show a "Re-authorising..." hint.
  bool reauthorising() const noexcept
  {
    return reauthorising_.load();
  }

private:
  DeviceScanner();
  ~DeviceScanner();
  DeviceScanner(const DeviceScanner&) = delete;
  DeviceScanner& operator=(const DeviceScanner&) = delete;

  void loop();
  void scan_once();
  void probe(DeviceInfo& d);

  mutable std::mutex mu_;
  std::vector<DeviceInfo> devices_;

  std::thread worker_;
  std::atomic<bool> running_{false};
  std::atomic<bool> tick_now_{false};
  std::atomic<bool> deep_tick_{false};
  std::atomic<bool> reauthorising_{false};
};

} // namespace cr::device
