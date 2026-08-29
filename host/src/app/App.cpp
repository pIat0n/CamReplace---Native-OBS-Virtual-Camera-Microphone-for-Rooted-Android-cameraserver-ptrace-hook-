#include "app/App.h"
#include "device/AdbClient.h"
#include "device/DeviceScanner.h"
#include "device/LogTail.h"
#include "render/D3D11Host.h"
#include "resources/EmbeddedFs.h"
#include "source/FeedController.h"
#include "ui/MainWindow.h"
#include "ui/StartupIntro.h"
#include "ui/Theme.h"
#include "util/Log.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <mmsystem.h>
#include <tlhelp32.h>
#include "util/WinApiDyn.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace cr::app
{

namespace
{

// Borderless-window plumbing globals. Keeping them file-local (not on App) so
// the Win32 callback can reach them without a lambda capture.
cr::render::D3D11Host g_d3d;
App* g_app = nullptr;
HWND g_hwnd = nullptr;
std::atomic<bool> g_imgui_wants_mouse{false};

// Width reserved on the right side of the title bar for the three traffic-
// light buttons + padding. Must stay in sync with MainWindow.cpp's layout.
// A little slack lets you reach the buttons even if ImGui's reported item
// rect is slightly smaller than the click area.
constexpr int kTrafficLightsReserve = 110;
constexpr int kTitleCenterButtonReserveHalf = 42;

enum class WindowMotionKind
{
  Idle,
  Maximize,
  Restore,
  MinimizeOut,
  CloseOut,
  RestoreIn,
};

struct WindowMotion
{
  WindowMotionKind kind = WindowMotionKind::Idle;
  RECT from{};
  RECT to{};
  RECT normal_rect{};
  float t = 0.0f;
  float duration = 0.20f;
  float alpha = 1.0f;
  float scale = 1.0f;
  bool logical_maximized = false;
  bool minimized_by_motion = false;
  bool was_minimized = false;
};

class TimerResolutionScope
{
public:
  TimerResolutionScope() noexcept
  {
    active_ = (::timeBeginPeriod(1) == TIMERR_NOERROR);
  }

  ~TimerResolutionScope() noexcept
  {
    if (active_)
      ::timeEndPeriod(1);
  }

  TimerResolutionScope(const TimerResolutionScope&) = delete;
  TimerResolutionScope& operator=(const TimerResolutionScope&) = delete;

private:
  bool active_ = false;
};

WindowMotion g_motion;
std::mutex g_host_runtime_mu;
bool g_host_runtime_ready = false;

float clamp01(float v) noexcept
{
  return std::max(0.0f, std::min(1.0f, v));
}

float ease_out_cubic(float t) noexcept
{
  t = clamp01(t);
  const float inv = 1.0f - t;
  return 1.0f - inv * inv * inv;
}

float smoothstep(float t) noexcept
{
  t = clamp01(t);
  return t * t * (3.0f - 2.0f * t);
}

LONG lerp_long(LONG a, LONG b, float t) noexcept
{
  return static_cast<LONG>(a + (b - a) * t + (b >= a ? 0.5f : -0.5f));
}

RECT lerp_rect(const RECT& a, const RECT& b, float t) noexcept
{
  return {
      lerp_long(a.left, b.left, t),
      lerp_long(a.top, b.top, t),
      lerp_long(a.right, b.right, t),
      lerp_long(a.bottom, b.bottom, t),
  };
}

RECT current_window_rect(HWND hwnd) noexcept
{
  RECT r{};
  if (hwnd)
    ::GetWindowRect(hwnd, &r);
  return r;
}

RECT work_rect_for_window(HWND hwnd) noexcept
{
  HMONITOR mon = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{sizeof(mi)};
  if (::GetMonitorInfo(mon, &mi))
    return mi.rcWork;
  return current_window_rect(hwnd);
}

bool effectively_maximized(HWND hwnd) noexcept
{
  return hwnd && (g_motion.logical_maximized || ::IsZoomed(hwnd));
}

void set_window_rect(HWND hwnd, const RECT& r, UINT extra_flags = 0) noexcept
{
  if (!hwnd)
    return;
  const int w = static_cast<int>(r.right - r.left);
  const int h = static_cast<int>(r.bottom - r.top);
  ::SetWindowPos(hwnd, nullptr, r.left, r.top, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | extra_flags);
}

void begin_rect_motion(WindowMotionKind kind, const RECT& from, const RECT& to, float duration) noexcept
{
  g_motion.kind = kind;
  g_motion.from = from;
  g_motion.to = to;
  g_motion.t = 0.0f;
  g_motion.duration = duration;
  g_motion.alpha = 1.0f;
  g_motion.scale = 1.0f;
}

void begin_fade_motion(WindowMotionKind kind, float duration) noexcept
{
  g_motion.kind = kind;
  g_motion.t = 0.0f;
  g_motion.duration = duration;
  g_motion.alpha = 1.0f;
  g_motion.scale = 1.0f;
}

void update_window_motion(float dt) noexcept
{
  HWND hwnd = g_hwnd;
  if (!hwnd || g_motion.kind == WindowMotionKind::Idle)
    return;

  g_motion.t += dt > 0.0f ? dt : (1.0f / 60.0f);
  const float p = clamp01(g_motion.t / std::max(0.001f, g_motion.duration));

  switch (g_motion.kind)
  {
  case WindowMotionKind::Maximize:
  case WindowMotionKind::Restore:
  {
    const RECT r = lerp_rect(g_motion.from, g_motion.to, ease_out_cubic(p));
    set_window_rect(hwnd, r, p >= 1.0f ? SWP_FRAMECHANGED : 0);
    if (p >= 1.0f)
    {
      g_motion.logical_maximized = (g_motion.kind == WindowMotionKind::Maximize);
      g_motion.kind = WindowMotionKind::Idle;
      g_motion.alpha = 1.0f;
      g_motion.scale = 1.0f;
    }
    break;
  }
  case WindowMotionKind::MinimizeOut:
  case WindowMotionKind::CloseOut:
  {
    const float e = smoothstep(p);
    g_motion.alpha = 1.0f - e;
    g_motion.scale = 1.0f - 0.035f * e;
    if (p >= 1.0f)
    {
      g_motion.alpha = 1.0f;
      g_motion.scale = 1.0f;
      const bool close = (g_motion.kind == WindowMotionKind::CloseOut);
      g_motion.kind = WindowMotionKind::Idle;
      if (close)
      {
        ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
      }
      else
      {
        g_motion.minimized_by_motion = true;
        ::ShowWindow(hwnd, SW_MINIMIZE);
      }
    }
    break;
  }
  case WindowMotionKind::RestoreIn:
  {
    const float e = smoothstep(p);
    g_motion.alpha = e;
    g_motion.scale = 0.985f + 0.015f * e;
    if (p >= 1.0f)
    {
      g_motion.kind = WindowMotionKind::Idle;
      g_motion.alpha = 1.0f;
      g_motion.scale = 1.0f;
    }
    break;
  }
  case WindowMotionKind::Idle:
    break;
  }
}

std::wstring comparable_path(std::filesystem::path path) noexcept
{
  std::error_code ec;
  std::filesystem::path abs = std::filesystem::absolute(path, ec);
  if (!ec)
    path = std::move(abs);
  return path.lexically_normal().wstring();
}

void terminate_private_adb_processes() noexcept
{
  const std::wstring target = comparable_path(cr::resources::adb_exe_path());
  if (target.empty())
    return;

  HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE)
    return;

  PROCESSENTRY32W pe{};
  pe.dwSize = sizeof(pe);
  if (!::Process32FirstW(snapshot, &pe))
  {
    ::CloseHandle(snapshot);
    return;
  }

  do
  {
    if (::_wcsicmp(pe.szExeFile, L"adb.exe") != 0)
      continue;

    HANDLE proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
    if (!proc)
      continue;

    wchar_t image[MAX_PATH * 4]{};
    DWORD image_len = static_cast<DWORD>(sizeof(image) / sizeof(image[0]));
    if (::QueryFullProcessImageNameW(proc, 0, image, &image_len))
    {
      const std::wstring actual = comparable_path(std::filesystem::path(image));
      if (::_wcsicmp(actual.c_str(), target.c_str()) == 0)
      {
        if (::TerminateProcess(proc, 0))
          ::WaitForSingleObject(proc, 2000);
      }
    }
    ::CloseHandle(proc);
  } while (::Process32NextW(snapshot, &pe));

  ::CloseHandle(snapshot);
}

} // namespace

HWND App::hwnd() noexcept
{
  return g_hwnd;
}
void App::set_imgui_wants_mouse(bool v) noexcept
{
  g_imgui_wants_mouse = v;
}
float App::window_motion_alpha() noexcept
{
  return g_motion.alpha;
}
float App::window_motion_scale() noexcept
{
  return g_motion.scale;
}

bool App::host_runtime_ready() noexcept
{
  std::lock_guard<std::mutex> lk(g_host_runtime_mu);
  return g_host_runtime_ready;
}

bool App::ensure_host_runtime_ready() noexcept
{
  std::lock_guard<std::mutex> lk(g_host_runtime_mu);
  if (g_host_runtime_ready)
    return true;
  try
  {
    auto dir = cr::resources::extract_adb_suite();
    cr::log::info("init", std::string("adb suite extracted to: ") + dir.string());
    cr::device::AdbClient::instance().set_adb_path(cr::resources::adb_exe_path());
    cr::device::DeviceScanner::instance().start();
    g_host_runtime_ready = true;
    return true;
  }
  catch (const std::exception& e)
  {
    cr::log::error("init", std::string("host runtime start failed: ") + e.what());
  }
  catch (...)
  {
    cr::log::error("init", "host runtime start failed");
  }
  cr::resources::cleanup_bin_dir();
  return false;
}

void App::cleanup_host_runtime(const char* reason, bool cleanup_android) noexcept
{
  std::lock_guard<std::mutex> lk(g_host_runtime_mu);
  try
  {
    if (cleanup_android && g_host_runtime_ready)
      cr::source::FeedController::cleanup_known_devices(reason);

    cr::device::DeviceScanner::instance().stop();
    cr::device::LogTail::instance().stop();

    if (g_host_runtime_ready)
    {
      const auto kill = cr::device::AdbClient::instance().kill_server_sync(10000);
      cr::log::info("adb", "adb kill-server: exit=" + std::to_string(kill.exit_code) +
                               " stderr=" + (kill.stderr_.empty() ? "(empty)" : kill.stderr_));
    }
    terminate_private_adb_processes();
    cr::device::AdbClient::instance().clear_adb_path();
    cr::resources::cleanup_bin_dir();
  }
  catch (const std::exception& e)
  {
    cr::log::warn("cleanup", std::string(reason ? reason : "cleanup") + ": host cleanup failed: " + e.what());
  }
  catch (...)
  {
    cr::log::warn("cleanup", std::string(reason ? reason : "cleanup") + ": host cleanup failed");
  }
  g_host_runtime_ready = false;
}

void App::request_minimize_animated() noexcept
{
  if (!g_hwnd)
    return;
  if (g_motion.kind != WindowMotionKind::Idle)
    return;
  begin_fade_motion(WindowMotionKind::MinimizeOut, 0.16f);
}

void App::request_close_animated() noexcept
{
  if (!g_hwnd)
    return;
  if (g_motion.kind != WindowMotionKind::Idle)
    return;
  begin_fade_motion(WindowMotionKind::CloseOut, 0.14f);
}

void App::request_toggle_maximize_animated() noexcept
{
  HWND hwnd = g_hwnd;
  if (!hwnd || g_motion.kind != WindowMotionKind::Idle)
    return;

  if (effectively_maximized(hwnd))
  {
    RECT from = current_window_rect(hwnd);
    RECT to = g_motion.normal_rect;
    if (to.right <= to.left || to.bottom <= to.top)
    {
      WINDOWPLACEMENT wp{sizeof(wp)};
      if (::GetWindowPlacement(hwnd, &wp) && wp.rcNormalPosition.right > wp.rcNormalPosition.left && wp.rcNormalPosition.bottom > wp.rcNormalPosition.top)
      {
        to = wp.rcNormalPosition;
      }
      else
      {
        to = {0, 0, 1280, 760};
      }
    }
    g_motion.logical_maximized = false;
    begin_rect_motion(WindowMotionKind::Restore, from, to, 0.22f);
  }
  else
  {
    g_motion.normal_rect = current_window_rect(hwnd);
    RECT to = work_rect_for_window(hwnd);
    begin_rect_motion(WindowMotionKind::Maximize, g_motion.normal_rect, to, 0.24f);
  }
}

// Expose the shared device pointer so UI-side texture code doesn't need
// to thread its own copy through every call.
ID3D11Device* d3d_device() noexcept
{
  return g_d3d.device();
}

// Window procedure.
// Implements the standard "borderless but resizable" recipe:
//   - WS_POPUP | WS_THICKFRAME on the hwnd
//   - WM_NCCALCSIZE returns 0 → no non-client frame; the whole window is
//     client area, so we draw our own title bar.
//   - WM_NCHITTEST picks HTTOP/HTLEFT/... within a 6-px border so the user
//     can still resize by dragging edges.
//   - WM_NCHITTEST picks HTCAPTION for the top row (minus the buttons)
//     so the user can drag the window by the title bar.
//   - WM_NCACTIVATE returns TRUE to suppress Windows repainting an inactive
//     frame over our custom chrome.
//   - WM_GETMINMAXINFO clamps the maximised size to rcWork (no taskbar).
LRESULT CALLBACK App::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
  if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
    return 0;

  switch (msg)
  {
  // Swallow the entire non-client frame.
  case WM_NCCALCSIZE:
  {
    if (wp == TRUE)
    {
      auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);

      // When the window is maximised, Windows expects the client
      // area to extend to the monitor edges, which causes the
      // title bar to sit under the taskbar. Clamp to rcWork.
      HMONITOR mon = ::MonitorFromRect(&p->rgrc[0], MONITOR_DEFAULTTONEAREST);
      MONITORINFO mi{sizeof(mi)};
      ::GetMonitorInfo(mon, &mi);
      LONG mon_w = mi.rcMonitor.right - mi.rcMonitor.left;
      LONG mon_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
      LONG prop_w = p->rgrc[0].right - p->rgrc[0].left;
      LONG prop_h = p->rgrc[0].bottom - p->rgrc[0].top;
      bool looks_maximised = (prop_w >= mon_w) && (prop_h >= mon_h);
      if (looks_maximised || ::IsZoomed(hwnd))
      {
        p->rgrc[0] = mi.rcWork;
      }
      return 0;
    }
    break;
  }

  // Defends against our window getting sized beyond rcWork during drags.
  case WM_WINDOWPOSCHANGING:
  {
    auto* pos = reinterpret_cast<WINDOWPOS*>(lp);
    if (!(pos->flags & SWP_NOMOVE) && !(pos->flags & SWP_NOSIZE))
    {
      HMONITOR mon = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
      MONITORINFO mi{sizeof(mi)};
      ::GetMonitorInfo(mon, &mi);
      LONG mon_w = mi.rcMonitor.right - mi.rcMonitor.left;
      LONG mon_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
      if (pos->cx >= mon_w && pos->cy >= mon_h)
      {
        pos->x = mi.rcWork.left;
        pos->y = mi.rcWork.top;
        pos->cx = mi.rcWork.right - mi.rcWork.left;
        pos->cy = mi.rcWork.bottom - mi.rcWork.top;
      }
    }
    break;
  }

  case WM_SIZE:
    if (wp == SIZE_MINIMIZED)
    {
      g_motion.was_minimized = true;
    }
    else if (wp == SIZE_RESTORED && g_motion.was_minimized)
    {
      g_motion.was_minimized = false;
      g_motion.minimized_by_motion = false;
      g_motion.kind = WindowMotionKind::RestoreIn;
      g_motion.t = 0.0f;
      g_motion.duration = 0.15f;
      g_motion.alpha = 0.0f;
      g_motion.scale = 0.985f;
    }
    if (wp != SIZE_MINIMIZED)
      g_d3d.on_resize(LOWORD(lp), HIWORD(lp));
    return 0;

  case WM_SYSCOMMAND:
    if ((wp & 0xfff0) == SC_KEYMENU)
      return 0;
    break;

  case WM_DESTROY:
    if (g_app)
      g_app->running_ = false;
    ::PostQuitMessage(0);
    return 0;

  // Without this, Windows paints a thin inactive-frame sliver over our
  // custom title bar whenever focus changes.
  case WM_NCACTIVATE:
    return TRUE;

  case WM_NCHITTEST:
  {
    // Start with DefWindow's answer, then override HTCLIENT regions
    // that we want to behave as resize borders or caption.
    LRESULT hit = ::DefWindowProcW(hwnd, msg, wp, lp);
    if (hit == HTCLIENT)
    {
      POINT pt{(int) (short) LOWORD(lp), (int) (short) HIWORD(lp)};
      ::ScreenToClient(hwnd, &pt);
      RECT rc;
      ::GetClientRect(hwnd, &rc);

      const bool maximised = effectively_maximized(hwnd);
      if (!maximised)
      {
        const int b = 6;
        if (pt.y < b && pt.x < b)
          return HTTOPLEFT;
        if (pt.y < b && pt.x > rc.right - b)
          return HTTOPRIGHT;
        if (pt.y > rc.bottom - b && pt.x < b)
          return HTBOTTOMLEFT;
        if (pt.y > rc.bottom - b && pt.x > rc.right - b)
          return HTBOTTOMRIGHT;
        if (pt.y < b)
          return HTTOP;
        if (pt.y > rc.bottom - b)
          return HTBOTTOM;
        if (pt.x < b)
          return HTLEFT;
        if (pt.x > rc.right - b)
          return HTRIGHT;
      }
      // Drag region: title bar row, excluding the centered brand button,
      // right traffic lights, and hovered ImGui widgets.
      const int title_cx = rc.right / 2;
      const bool in_title_center_button = pt.x >= title_cx - kTitleCenterButtonReserveHalf && pt.x <= title_cx + kTitleCenterButtonReserveHalf;
      if (pt.y < kTitleBarHeight &&
          pt.x < rc.right - kTrafficLightsReserve &&
          !in_title_center_button &&
          !g_imgui_wants_mouse.load())
      {
        return HTCAPTION;
      }
    }
    return hit;
  }

  case WM_NCLBUTTONDBLCLK:
    if (wp == HTCAPTION)
    {
      App::request_toggle_maximize_animated();
      return 0;
    }
    break;

  // Keep Aero-Snap'd maximise inside rcWork.
  case WM_GETMINMAXINFO:
  {
    auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
    HMONITOR mon = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    if (::GetMonitorInfo(mon, &mi))
    {
      mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
      mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
      mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
      mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
      mmi->ptMaxTrackSize.x = mi.rcWork.right - mi.rcWork.left;
      mmi->ptMaxTrackSize.y = mi.rcWork.bottom - mi.rcWork.top;
    }
    mmi->ptMinTrackSize.x = 820;
    mmi->ptMinTrackSize.y = 540;
    return 0;
  }
  }
  return ::DefWindowProcW(hwnd, msg, wp, lp);
}

bool App::init_window(HINSTANCE hInstance, int nShowCmd)
{
  const std::wstring class_name = L"CameraReplaceWnd";
  const std::wstring window_title = L"Camera Replace";

  WNDCLASSEXW wc{sizeof(wc)};
  wc.style = CS_CLASSDC;
  wc.lpfnWndProc = &App::wnd_proc;
  wc.hInstance = hInstance;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = class_name.c_str();
  ::RegisterClassExW(&wc);

  // Centre the window on the primary monitor's work area (excludes taskbar).
  const int init_w = 1280, init_h = 760;
  RECT work{0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN)};
  {
    HMONITOR mon = ::MonitorFromPoint({work.right / 2, work.bottom / 2}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    if (::GetMonitorInfo(mon, &mi))
      work = mi.rcWork;
  }
  const int pos_x = work.left + ((work.right - work.left) - init_w) / 2;
  const int pos_y = work.top + ((work.bottom - work.top) - init_h) / 2;
  initial_x_ = pos_x;
  initial_y_ = pos_y;
  initial_w_ = init_w;
  initial_h_ = init_h;

  // Borderless but resizable: WS_POPUP kills the default frame,
  // WS_THICKFRAME re-enables resize-hit-testing by DefWindowProc, and the
  // SYSMENU/MINIMIZE/MAXIMIZE bits let Aero-Snap and the taskbar preview work.
  hwnd_ = ::CreateWindowExW(0, wc.lpszClassName, window_title.c_str(), WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU, pos_x, pos_y, init_w, init_h, nullptr, nullptr, wc.hInstance, nullptr);

  if (!hwnd_)
    return false;
  g_hwnd = hwnd_;

  // Dark title bar on anything that listens (older Win10 ignores it; fine).
  BOOL dark = TRUE;
  ::DwmSetWindowAttribute(hwnd_, /*DWMWA_USE_IMMERSIVE_DARK_MODE=*/20, &dark, sizeof(dark));

  // Rounded corners on Win11 (attribute 33 = DWMWA_WINDOW_CORNER_PREFERENCE,
  // value 2 = DWMWCP_ROUND). Attribute is silently ignored on Win10.
  DWORD corner_pref = 2;
  ::DwmSetWindowAttribute(hwnd_, 33, &corner_pref, sizeof(corner_pref));

  // Nudge the window after creation so WM_NCCALCSIZE fires and our chrome
  // recalculates. Without this the first frame sometimes has a 1-px sliver
  // of the default frame visible.
  ::SetWindowPos(hwnd_, nullptr, pos_x, pos_y, init_w, init_h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);

  ::ShowWindow(hwnd_, nShowCmd);
  ::UpdateWindow(hwnd_);
  return true;
}

void App::init_imgui()
{
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.IniFilename = nullptr;

  cr::ui::apply_dark_theme();

  ImGui_ImplWin32_Init(hwnd_);
  ImGui_ImplDX11_Init(g_d3d.device(), g_d3d.context());
}

void App::shutdown()
{
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  g_d3d.shutdown();
  if (hwnd_)
  {
    ::DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    g_hwnd = nullptr;
  }
}

// Defense-in-depth admin check. The embedded manifest requests
// `requireAdministrator` so Windows normally shows UAC and refuses to
// launch the process at all if the user clicks No — but if the manifest
// somehow didn't take effect (corrupted build, side-loaded into a
// loader that ignores it), refuse to run from main() too. Avoids a
// half-broken state where the app starts but every adb command fails
// silently for permission reasons.
static bool is_process_elevated()
{
  HANDLE token = nullptr;
  if (!cr::winapi::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
  {
    return false;
  }
  TOKEN_ELEVATION elev{};
  DWORD got = 0;
  bool elevated = false;
  if (cr::winapi::GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &got))
  {
    elevated = (elev.TokenIsElevated != 0);
  }
  ::CloseHandle(token);
  return elevated;
}

int App::run(HINSTANCE hInstance, int nShowCmd)
{
  g_app = this;

  if (!is_process_elevated())
  {
    const std::wstring msg = L"Camera Replace requires administrator privileges.\n\nRight-click the .exe and choose \"Run as administrator\", "
                             "or accept the UAC prompt the next time you launch.";
    const std::wstring title = L"Camera Replace - admin required";
    ::MessageBoxW(nullptr, msg.c_str(), title.c_str(), MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
    return 3;
  }

  TimerResolutionScope timer_resolution;
  cr::log::info("init", "Camera Replace starting");

  terminate_private_adb_processes();
  cr::resources::cleanup_bin_dir();

  cr::ui::bootstrap_startup_intro();
  if (!init_window(hInstance, nShowCmd))
  {
    cr::log::error("init", "init_window failed");
    return 1;
  }
  if (!g_d3d.init(hwnd_))
  {
    cr::log::error("init", "D3D11Host::init failed");
    return 2;
  }
  init_imgui();
  ::SetWindowPos(hwnd_, nullptr, initial_x_, initial_y_, initial_w_, initial_h_, SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  ::ShowWindow(hwnd_, nShowCmd);
  ::UpdateWindow(hwnd_);

  // Start the host runtime before drawing the main window so USB devices are
  // discovered immediately in the public build.
  if (!App::ensure_host_runtime_ready())
    cr::log::error("init", "host runtime failed to start");

  // Background colour of the swap chain below our ImGui layer. Matches
  // WindowBg so no seam is visible at the rounded corners.
  const float clear[4] = {0.078f, 0.078f, 0.078f, 1.0f};

  MSG msg{};
  while (running_)
  {
    while (::PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
    {
      ::TranslateMessage(&msg);
      ::DispatchMessage(&msg);
      if (msg.message == WM_QUIT)
        running_ = false;
    }
    if (!running_)
      break;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    update_window_motion(ImGui::GetIO().DeltaTime);

    if (cr::ui::startup_intro_active())
    {
      cr::ui::draw_startup_intro();
    }
    else
    {
      cr::ui::draw_main_window();
    }

    // Let wnd_proc know whether the user is interacting with an ImGui
    // widget right now — so a drag on a hovered button doesn't turn
    // into a window-move.
    App::set_imgui_wants_mouse(ImGui::GetIO().WantCaptureMouse && ImGui::IsAnyItemHovered());

    ImGui::Render();
    g_d3d.begin_frame(clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    const bool animating = g_motion.kind != WindowMotionKind::Idle || cr::ui::startup_intro_active() || cr::ui::ui_animation_active();
    g_d3d.end_frame(/*vsync=*/!animating);
  }

  // Orderly teardown so the adb daemon and its child processes don't
  // leave file handles open against bin/adb.exe — that's what blocks
  // the user from deleting the install folder afterwards.
  // 1. Stop UI-owned background workers before ImGui and adb teardown.
  cr::ui::shutdown_main_window();
  cr::ui::shutdown_startup_intro();
  // 2. Stop scanner/adb, wipe phone-side artifacts, and delete the private
  //    Windows extraction directory after adb has been killed synchronously.
  App::cleanup_host_runtime("app shutdown", true);


  shutdown();
  g_app = nullptr;
  return 0;
}

} // namespace cr::app
