#pragma once

// Application shell.
// Owns the Win32 window, the D3D11Host, and the ImGui backend init/shutdown.
// Runs the message pump and calls ui::draw_main_window() each frame.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace cr::app
{

// Height in pixels of the custom title bar we draw inside ImGui.
// Anything below this row is hit-tested as client; anything inside (minus the
// right-side traffic lights) becomes HTCAPTION so the window can be dragged.
inline constexpr int kTitleBarHeight = 32;

class App
{
public:
  int run(HINSTANCE hInstance, int nShowCmd);

  // The window handle for code (e.g. titlebar buttons) that needs to poke
  // the OS — `::ShowWindow`, `::PostMessage(WM_CLOSE)`, etc. Null before
  // run() completes init.
  static HWND hwnd() noexcept;

  // ImGui signalled that it wants the mouse this frame (a widget is being
  // hovered/interacted with). The wnd_proc uses this to avoid swallowing
  // mouse input as window-drag.
  static void set_imgui_wants_mouse(bool v) noexcept;

  // Animated window-chrome requests used by the custom ImGui title bar.
  // These are UI-thread only; they degrade to direct Win32 calls if no hwnd
  // exists yet.
  static void request_minimize_animated() noexcept;
  static void request_toggle_maximize_animated() noexcept;
  static void request_close_animated() noexcept;

  // Per-frame visual modifiers consumed by the ImGui root window.
  static float window_motion_alpha() noexcept;
  static float window_motion_scale() noexcept;

  // Host-side runtime lifecycle. This gates extraction of adb.exe/DLLs and
  // scanner startup so login can complete without unpacking local binaries.
  static bool ensure_host_runtime_ready() noexcept;
  static void cleanup_host_runtime(const char* reason, bool cleanup_android = true) noexcept;
  static bool host_runtime_ready() noexcept;

private:
  static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  bool init_window(HINSTANCE hInstance, int nShowCmd);
  void init_imgui();
  void shutdown();

  HWND hwnd_ = nullptr;
  bool running_ = true;
  int initial_x_ = 0;
  int initial_y_ = 0;
  int initial_w_ = 1280;
  int initial_h_ = 760;
};

} // namespace cr::app
