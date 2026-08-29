#include "util/SessionLog.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string_view>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
namespace fs = std::filesystem;

namespace cr::util
{

namespace
{

const char* tag_for(LogKind k)
{
  switch (k)
  {
  case LogKind::Info:
    return "INFO";
  case LogKind::Warn:
    return "WARN";
  case LogKind::Error:
    return "ERROR";
  case LogKind::Ok:
    return "OK";
  case LogKind::Device:
    return "DEV";
  }
  return "?";
}

std::string timestamp_now()
{
  SYSTEMTIME st;
  GetLocalTime(&st);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
  return buf;
}

fs::path resolve_log_dir()
{
  // Prefer %LOCALAPPDATA%\CameraReplace if set; fall back to %TEMP%.
  wchar_t buf[MAX_PATH];
  fs::path base;
  const std::wstring local_appdata = L"LOCALAPPDATA";
  DWORD n = GetEnvironmentVariableW(local_appdata.c_str(), buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH)
    base = buf;
  else
  {
    n = GetTempPathW(MAX_PATH, buf);
    base = (n > 0 && n < MAX_PATH) ? fs::path(buf) : fs::current_path();
  }
  fs::path dir = base / L"CameraReplace";
  std::error_code ec;
  fs::create_directories(dir, ec);
  return dir;
}

// Pull the actual Windows version directly from ntdll — GetVersionExW
// got version-shimmed in 8.1+ and now lies about ≥10. RtlGetVersion is
// the documented escape hatch and reports real major/minor/build.
struct WinOsInfo
{
  DWORD major = 0;
  DWORD minor = 0;
  DWORD build = 0;
  const char* arch = "?";
};

WinOsInfo collect_os_info()
{
  WinOsInfo o;
  typedef LONG(WINAPI * RtlGetVersionPtr)(LPOSVERSIONINFOEXW);
  const std::wstring ntdll = L"ntdll.dll";
  HMODULE nt = GetModuleHandleW(ntdll.c_str());
  if (nt)
  {
    auto fn = (RtlGetVersionPtr) GetProcAddress(nt, "RtlGetVersion");
    if (fn)
    {
      OSVERSIONINFOEXW vi{};
      vi.dwOSVersionInfoSize = sizeof(vi);
      if (fn(&vi) == 0)
      {
        o.major = vi.dwMajorVersion;
        o.minor = vi.dwMinorVersion;
        o.build = vi.dwBuildNumber;
      }
    }
  }
  SYSTEM_INFO si{};
  GetNativeSystemInfo(&si);
  switch (si.wProcessorArchitecture)
  {
  case PROCESSOR_ARCHITECTURE_AMD64:
    o.arch = "x64";
    break;
  case PROCESSOR_ARCHITECTURE_ARM64:
    o.arch = "arm64" /*OBF_SKIP*/;
    break;
  case PROCESSOR_ARCHITECTURE_INTEL:
    o.arch = "x86";
    break;
  default:
    o.arch = "?";
    break;
  }
  return o;
}

} // namespace

SessionLog& SessionLog::instance()
{
  static SessionLog s;
  return s;
}

SessionLog::SessionLog()
{
  const fs::path dir = resolve_log_dir();
  session_path_ = dir / "session.log";

  // `wb` truncates on open — a fresh run starts clean. Previous run is
  // preserved as session.log.1.
  std::error_code ec;
  fs::rename(session_path_, dir / "session.log.1", ec);
  const std::wstring write_mode = L"wb";
  session_fp_ = _wfopen(session_path_.wstring().c_str(), write_mode.c_str());

  write_session_header_();
}

SessionLog::~SessionLog()
{
  if (session_fp_)
    std::fclose(session_fp_);
}

void SessionLog::write_session_header_()
{
  if (!session_fp_)
    return;

  const WinOsInfo os = collect_os_info();

  // Multi-line banner — easy to spot when scanning a user-submitted
  // log. Format kept stable so future tooling can grep `^HOST ` /
  // `^DEVICE ` to extract the diagnostic preamble.
  std::fprintf(session_fp_,
               "============================================================\nCameraReplace session  %s\n"
               "HOST  windows=%lu.%lu.%lu arch=%s built=%s %s\n============================================================\n",
               timestamp_now().c_str(), (unsigned long) os.major, (unsigned long) os.minor, (unsigned long) os.build, os.arch, __DATE__, __TIME__);
  std::fflush(session_fp_);
}

void SessionLog::write_file_line_(std::FILE* fp, LogKind k, const std::string& s)
{
  if (!fp)
    return;
  std::fprintf(fp, "%s %-5s %s\n", timestamp_now().c_str(), tag_for(k), s.c_str());
  std::fflush(fp); // survive crashes
}

void SessionLog::push(LogKind k, std::string line)
{
  // Strip any trailing newline / CR for consistent rendering.
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
    line.pop_back();

  std::lock_guard<std::mutex> lk(mu_);
  write_file_line_(session_fp_, k, line);

  // Keep the on-screen panel scannable: drop low-value Info chatter,
  // but always preserve warnings, errors, completed steps, and phone
  // logcat lines. Full unfiltered stream stays in the session log on disk.
  if (!should_show_in_ui_(k, line))
    return;

  ring_.push_back({k, std::move(line)});
  while (ring_.size() > kCapacity)
    ring_.pop_front();
}

bool SessionLog::should_show_in_ui_(LogKind k, const std::string& s)
{
  // Anything that isn't routine "Info" lands in the panel unconditionally.
  if (k != LogKind::Info)
    return true;

  // Lines pushed by cr::log::* start with `[<component>] ` when a
  // component is set, or just the body otherwise. Strip the bracket
  // prefix so the denylist matches against the user-facing text.
  std::size_t body_start = 0;
  if (!s.empty() && s[0] == '[')
  {
    auto cb = s.find(']');
    if (cb != std::string::npos && cb + 1 < s.size() && s[cb + 1] == ' ')
    {
      body_start = cb + 2;
    }
  }
  std::string_view body(s.data() + body_start, s.size() - body_start);

  // Prefix denylist for housekeeping / per-frame info that drowns out
  // the few lines a user actually reads.
  static const std::initializer_list<std::string_view> kDropPrefix = {
      "embedded blob empty", "extracted ", "adb path: ",  "probed ", "deep refresh:", "requesting auth", "our logcat stream ended", "RtmpServer: listening", "RtmpServer: unexpected C0", "TcpFeedClient: ", "ObsRtmpSource: meta", "ObsRtmpSource: h264 header", "H264PreviewDecoder:", "binaries extracted to:",
      "listening", // post-extract "rtmp" component body
      "unexpected C0",       "meta",       "h264 header",
  };
  for (const auto& p : kDropPrefix)
  {
    if (body.size() >= p.size() && std::equal(p.begin(), p.end(), body.begin()))
    {
      return false;
    }
  }
  return true;
}

void SessionLog::append_camera(std::string line)
{
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
    line.pop_back();

  std::lock_guard<std::mutex> lk(mu_);
  // Phone-side cameraserver / Camera* chatter goes to the session log
  // tagged DEV but bypasses the UI ring — it's too verbose to scan
  // visually but invaluable for post-mortem.
  write_file_line_(session_fp_, LogKind::Device, line);
}

void SessionLog::write_block(std::string text)
{
  if (!text.empty() && text.back() != '\n')
    text.push_back('\n');
  std::lock_guard<std::mutex> lk(mu_);
  if (session_fp_)
  {
    std::fputs(text.c_str(), session_fp_);
    std::fflush(session_fp_);
  }
}

bool SessionLog::append_encrypted_android_record_base64(std::string_view encoded)
{
  (void) encoded;
  return false;
}

std::vector<LogEntry> SessionLog::snapshot() const
{
  std::lock_guard<std::mutex> lk(mu_);
  return {ring_.begin(), ring_.end()};
}

void SessionLog::clear()
{
  std::lock_guard<std::mutex> lk(mu_);
  ring_.clear();
}

} // namespace cr::util
