#pragma once

// Custom title bar drawn at the top of the main ImGui window.
// The three traffic-light dots (yellow = minimize, green = maximize/restore,
// red = close) on the right. A centered "Camera Replace" caption. The rest
// of the row is hit-tested as HTCAPTION by App::wnd_proc, so dragging there
// moves the window.

namespace cr::ui
{

// Call at the very top of your main window. Advances the cursor past the
// title bar. Height is cr::app::kTitleBarHeight.
void draw_title_bar();

} // namespace cr::ui
