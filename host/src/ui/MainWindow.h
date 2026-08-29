#pragma once

// Main-window layout and controls.
// Three-pane layout:
//   [ Source   ][ Preview         ][ Device    ]
//   [          ][                 ][           ]
//   [ Start/Stop bar at bottom                 ]
// Backing logic lives in the source/device stacks; this header only exposes
// the ImGui entry point used by App.

namespace cr::ui
{

void draw_main_window();

// Joins background UI work before App tears down ADB and ImGui.
void shutdown_main_window() noexcept;

// True while UI-only transitions are still settling. App uses this to disable
// vsync temporarily so short animations get more frames on fast displays.
bool ui_animation_active() noexcept;

} // namespace cr::ui
