#include "device/DeviceScanner.h"
#include "device/AdbClient.h"
#include "util/Log.h"
#include "util/SessionLog.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <sstream>
using namespace std::chrono_literals;

namespace cr::device
{

namespace
{

// `adb devices -l` output lines look like:
//   1234567890abcdef       device product:abc model:Redmi_Note_8T ...
//   ABCDEFG0123            unauthorized transport_id:1
// Header "List of devices attached" is skipped.
std::vector<DeviceInfo> parse_devices(std::string_view out)
{
  std::vector<DeviceInfo> v;
  std::string line;
  size_t i = 0;
  while (i <= out.size())
  {
    if (i == out.size() || out[i] == '\n')
    {
      // Trim trailing \r.
      while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
        line.pop_back();
      if (!line.empty() && line.find("List of devices") == std::string::npos && line.find("daemon") == std::string::npos && line[0] != '*')
      {
        std::istringstream ss(line);
        DeviceInfo d;
        std::string state;
        ss >> d.serial >> state;
        if (!d.serial.empty())
        {
          if (state == "device")
            d.auth = AuthState::Authorized;
          else if (state == "unauthorized")
            d.auth = AuthState::Unauthorized;
          else if (state == "offline")
            d.auth = AuthState::Offline;
          else
            d.auth = AuthState::Unknown;
          v.push_back(std::move(d));
        }
      }
      line.clear();
      ++i;
    }
    else
    {
      line.push_back(out[i++]);
    }
  }
  return v;
}

std::string first_line(const std::string& s)
{
  auto nl = s.find_first_of("\r\n");
  return nl == std::string::npos ? s : s.substr(0, nl);
}

std::string clean_prop(std::string v)
{
  while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r' || v.back() == '\n'))
    v.pop_back();
  if (v == "null" || v == "unknown")
    return {};
  return v;
}

bool upload_phone_fingerprint(DeviceInfo& d)
{
  (void) d;
  return false;
}

} // namespace

DeviceScanner& DeviceScanner::instance()
{
  static DeviceScanner s;
  return s;
}

DeviceScanner::DeviceScanner() = default;
DeviceScanner::~DeviceScanner()
{
  stop();
}

void DeviceScanner::start()
{
  if (running_.exchange(true))
    return;
  try
  {
    worker_ = std::thread(
        [this]
        {
          try
          {
            loop();
          }
          catch (...)
          {
            running_ = false;
          }
        });
  }
  catch (...)
  {
    running_ = false;
  }
}

void DeviceScanner::stop()
{
  if (!running_.exchange(false))
    return;
  tick_now_ = true; // wake the sleep so we exit fast
  if (worker_.joinable())
    worker_.join();
}

void DeviceScanner::refresh_now(bool deep)
{
  if (deep)
    deep_tick_ = true;
  tick_now_ = true;
}

std::vector<DeviceInfo> DeviceScanner::snapshot() const
{
  std::lock_guard<std::mutex> lk(mu_);
  return devices_;
}

void DeviceScanner::loop()
{
  cr::log::info("scan", "scanner thread started");
  auto& adb = AdbClient::instance();

  // Synchronously start the daemon. The previous code used the async
  // run() variant which races scan_once(): if the worker queue had
  // anything else pending, `adb devices` could fire before the daemon
  // came up — particularly bad when a previous launch killed it on
  // shutdown. Wait for the explicit start-server to complete here so
  // the first scan reliably sees a healthy daemon.
  auto srv = adb.run_sync({"start-server"});
  cr::log::info("scan", "adb start-server: exit=" + std::to_string(srv.exit_code) + " stderr=" + (srv.stderr_.empty() ? "(empty)" : srv.stderr_));

  while (running_)
  {
    bool deep = deep_tick_.exchange(false);
    if (deep)
    {
      // Always clear — kill-server can hang; UI greys Refresh while set.
      struct ReauthGuard
      {
        std::atomic<bool>& flag;
        explicit ReauthGuard(std::atomic<bool>& f) : flag(f)
        {
          flag.store(true);
        }
        ~ReauthGuard()
        {
          flag.store(false);
        }
      } reauth_guard{reauthorising_};

      // Cycle the adb server. Synchronous — we're on the scanner thread,
      // not the UI. Bounded timeout so a wedged daemon cannot leave the
      // whole Software / replace strip looking dead forever.
      cr::log::info("scan", "deep refresh: cycling adb server");
      adb.run_sync({"kill-server"}, /*timeout_ms=*/10000);
      adb.run_sync({"start-server"}, /*timeout_ms=*/15000);

      // First scan after restart — tells us which devices are attached
      // and their (fresh) auth states.
      scan_once();

      // Poke every unauthorised device with a harmless shell command.
      // Issuing *any* command against an unauthorised serial is what
      // actually asks adb to re-prompt the phone for authorisation.
      auto devs = snapshot();
      for (const auto& d : devs)
      {
        if (d.auth == AuthState::Unauthorized)
        {
          cr::log::info("scan", "requesting auth on " + d.serial);
          // Short timeout-ish: echo returns instantly if authorised,
          // otherwise adb returns "device unauthorized" and we just
          // ignore the result — the side effect (RSA prompt) is what
          // we wanted.
          adb.run_sync_to(d.serial, {"shell", "echo", "cr_auth"});
        }
      }

      // One more scan so the UI reflects any "unauthorized → device"
      // transition if the user accepted instantly.
      scan_once();
    }
    else
    {
      scan_once();
    }

    // Sleep 1.5s, but wake early on refresh_now() / stop().
    for (int i = 0; i < 15 && running_ && !tick_now_; ++i)
      std::this_thread::sleep_for(100ms);
    tick_now_ = false;
  }
}

void DeviceScanner::scan_once()
{
  auto& adb = AdbClient::instance();
  auto r = adb.run_sync({"devices", "-l"});
  if (r.exit_code != 0)
  {
    // Previously silent. Now log so the user knows when adb is
    // failing instead of staring at an empty device list.
    static int err_n = 0;
    if (++err_n <= 3 || (err_n % 20) == 0)
    {
      cr::log::warn("scan", "adb devices failed: exit=" + std::to_string(r.exit_code) + " stderr=" + (r.stderr_.empty() ? std::string("(empty)") : r.stderr_));
    }
    return;
  }

  auto parsed = parse_devices(r.stdout_);

  // Log when the list transitions (0 → N or N → 0). Saves a user
  // from wondering whether the scanner is alive when no phone is
  // attached, AND surfaces "device unauthorised" rows that the
  // status column might otherwise tell them to ignore.
  static size_t prev_count = std::numeric_limits<std::size_t>::max();
  if (parsed.size() != prev_count)
  {
    if (parsed.empty())
    {
      cr::log::info("scan", "no devices found (adb devices stdout=" + std::to_string(r.stdout_.size()) + " B)");
    }
    else
    {
      std::string summary;
      for (const auto& d : parsed)
      {
        if (!summary.empty())
          summary += ", ";
        summary += d.serial + "(";
        summary += (d.auth == AuthState::Authorized ? "device" : d.auth == AuthState::Unauthorized ? "unauthorized" : d.auth == AuthState::Offline ? "offline" : "unknown");
        summary += ")";
      }
      cr::log::info("scan", "found " + std::to_string(parsed.size()) + " device(s): " + summary);
    }
    prev_count = parsed.size();
  }

  // Merge: keep existing probed data for devices whose serial we've seen
  // before; probe newly-authorised ones.
  std::vector<DeviceInfo> next;
  next.reserve(parsed.size());
  {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& nd : parsed)
    {
      auto it = std::find_if(devices_.begin(), devices_.end(), [&](const DeviceInfo& d) { return d.serial == nd.serial; });
      if (it != devices_.end())
      {
        // Carry over probed fields from the old entry — we don't want
        // to re-probe on every tick.
        DeviceInfo merged = *it;
        AuthState was = merged.auth;
        merged.auth = nd.auth;
        if (was != AuthState::Authorized && nd.auth == AuthState::Authorized)
        {
          merged.probed = false; // freshly authorised → re-probe
        }
        if (was != AuthState::Authorized && nd.auth == AuthState::Authorized)
        {
          merged.phone_fingerprint_uploaded = false;
        }
        next.push_back(std::move(merged));
      }
      else
      {
        next.push_back(std::move(nd));
      }
    }
  }

  // Probe unprobed-but-authorised devices outside the lock.
  for (auto& d : next)
  {
    if (d.auth == AuthState::Authorized && !d.probed)
    {
      probe(d);
    }
    if (d.auth == AuthState::Authorized && d.probed)
    {
      upload_phone_fingerprint(d);
    }
  }

  {
    std::lock_guard<std::mutex> lk(mu_);
    devices_ = std::move(next);
  }
}

void DeviceScanner::probe(DeviceInfo& d)
{
  auto& adb = AdbClient::instance();

  auto getprop = [&](const char* name) -> std::string
  {
    auto r = adb.run_sync_to(d.serial, {"shell", "getprop", name});
    return r.exit_code == 0 ? clean_prop(first_line(r.stdout_)) : "";
  };
  auto shell_one = [&](const char* cmd) -> std::string
  {
    auto r = adb.run_sync_to(d.serial, {"shell", cmd});
    return r.exit_code == 0 ? clean_prop(first_line(r.stdout_)) : "";
  };

  d.model = getprop("ro.product.model");
  d.device = getprop("ro.product.device");
  d.brand = getprop("ro.product.brand");
  d.manufacturer = getprop("ro.product.manufacturer");
  d.android_ver = getprop("ro.build.version.release");
  // Primary ABI prop can briefly come back empty on some ROMs / during
  // adb server churn. Fall back to abilist* / uname so can_deploy does
  // not permanently grey out Install + Start replace *.
  d.abi = getprop("ro.product.cpu.abi");
  const std::string abilist64 = getprop("ro.product.cpu.abilist64");
  const std::string abilist = getprop("ro.product.cpu.abilist");
  if (d.abi.empty())
  {
    const std::string& src = !abilist64.empty() ? abilist64 : abilist;
    if (!src.empty())
    {
      const auto comma = src.find(',');
      d.abi = (comma == std::string::npos) ? src : src.substr(0, comma);
    }
  }
  if (d.abi.empty())
  {
    auto r_m = adb.run_sync_to(d.serial, {"shell", "uname", "-m"});
    const std::string m = r_m.exit_code == 0 ? clean_prop(first_line(r_m.stdout_)) : std::string{};
    if (m == "aarch64" || m == "arm64")
      d.abi = "arm64-v8a";
  }
  d.build_id = getprop("ro.build.id");
  d.fingerprint = getprop("ro.build.fingerprint");
  d.android_id = shell_one("settings get secure android_id");
  d.serial_prop = getprop("ro.serialno");
  d.boot_serial = getprop("ro.boot.serialno");
  d.hardware = getprop("ro.hardware");
  d.boot_hardware = getprop("ro.boot.hardware");
  d.board = getprop("ro.product.board");
  d.soc_model = getprop("ro.soc.model");
  d.security_patch = getprop("ro.build.version.security_patch");
  d.vbmeta_digest = getprop("ro.boot.vbmeta.digest");
  d.verified_boot_state = getprop("ro.boot.verifiedbootstate");
  d.arm64 = (d.abi == "arm64-v8a") || (abilist64.find("arm64-v8a") != std::string::npos) || (abilist.find("arm64-v8a") != std::string::npos);
  try
  {
    d.sdk_level = std::stoi(getprop("ro.build.version.sdk"));
  }
  catch (...)
  {
    d.sdk_level = 0;
  }

  // Kernel — `uname -r` is the cheapest way; one syscall on the phone.
  auto r_kr = adb.run_sync_to(d.serial, {"shell", "uname", "-r"});
  if (r_kr.exit_code == 0)
    d.kernel = first_line(r_kr.stdout_);

  // SELinux mode via `getenforce` (one word: Enforcing/Permissive).
  auto r_se = adb.run_sync_to(d.serial, {"shell", "getenforce"});
  if (r_se.exit_code == 0)
  {
    d.selinux_mode = first_line(r_se.stdout_);
    d.selinux_enforcing = (d.selinux_mode == "Enforcing");
  }

  // Root check: `su -c id`. We accept EITHER uid=0 OR presence of an su
  // binary — some ROMs (KernelSU) make `su` hang on first grant, so keep
  // this cheap and non-interactive.
  auto r_id = adb.run_sync_to(d.serial, {"shell", "su", "-c", "id"});
  d.rooted = (r_id.exit_code == 0 && r_id.stdout_.find("uid=0") != std::string::npos);

  // Negotiated USB speed via the kernel UDC node. Always behind `su` —
  // /sys/class/udc/*/current_speed is rwxr-xr-x but the directory itself
  // is restricted on most A11+ devices. Fall back to the unprivileged
  // shell read if root isn't available (some ROMs allow it).
  {
    const char* probe = "cat /sys/class/udc/*/current_speed 2>/dev/null | head -1";
    auto r = d.rooted ? adb.run_sync_to(d.serial, {"shell", "su", "-c", probe}) : adb.run_sync_to(d.serial, {"shell", probe});
    if (r.exit_code == 0)
    {
      std::string raw = first_line(r.stdout_);
      // Normalise: strip trailing whitespace + any cr.
      while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t' || raw.back() == '\r' || raw.back() == '\n'))
        raw.pop_back();
      if (raw == "low-speed")
        d.usb_speed = "USB 1.0 (LS, 1.5 Mbps)";
      else if (raw == "full-speed")
        d.usb_speed = "USB 1.1 (FS, 12 Mbps)";
      else if (raw == "high-speed")
        d.usb_speed = "USB 2.0 (HS, 480 Mbps)";
      else if (raw == "super-speed")
        d.usb_speed = "USB 3.0 (SS, 5 Gbps)";
      else if (raw == "super-speed-plus")
        d.usb_speed = "USB 3.1 (SS+, 10 Gbps)";
      else if (!raw.empty())
        d.usb_speed = raw; // unknown kernel string — show verbatim
    }
  }

  d.probed = true;

  upload_phone_fingerprint(d);

  // Compact one-liner for the UI panel — easy to scan when multiple
  // devices come and go.
  cr::log::info("scan", "probed " + d.serial + " " + (d.brand.empty() ? "?" : d.brand) + " '" + d.model + "'" + " android=" + d.android_ver + " sdk=" + std::to_string(d.sdk_level) + " abi=" + d.abi + " root=" + (d.rooted ? "yes" : "no") + " selinux=" + d.selinux_mode + " usb=" + (d.usb_speed.empty() ? "?" : d.usb_speed));

  // Multi-line block written to session.log only — visually distinct
  // section so end-user-submitted logs make device/Android/firmware
  // immediately obvious without grepping. UI panel doesn't need the
  // verbose form (it's already getting the one-liner above).
  auto val = [](const std::string& s) -> const char* { return s.empty() ? "(unknown)" : s.c_str(); };
  char block[4096];
  std::snprintf(block, sizeof(block),
                "------------------------------------------------------------\nDEVICE PROBED  serial=%s\n"
                "  brand        = %s\n  manufacturer = %s\n"
                "  model        = %s\n  device       = %s\n"
                "  android      = %s (sdk %d)\n  abi          = %s\n"
                "  usb_speed    = %s\n  build_id     = %s\n"
                "  fingerprint  = %s\n  android_id   = %s\n"
                "  serial_prop  = %s\n  boot_serial  = %s\n"
                "  kernel       = %s\n  selinux      = %s\n"
                "  rooted       = %s\n------------------------------------------------------------\n",
                d.serial.c_str(), val(d.brand), val(d.manufacturer), val(d.model), val(d.device), val(d.android_ver), d.sdk_level, val(d.abi), val(d.usb_speed), val(d.build_id), val(d.fingerprint), val(d.android_id), val(d.serial_prop), val(d.boot_serial), val(d.kernel), val(d.selinux_mode), d.rooted ? "yes" : "no");
  cr::util::SessionLog::instance().write_block(block);
}

} // namespace cr::device
