#pragma once

namespace cr::ui
{

void bootstrap_startup_intro() noexcept;
bool startup_intro_active() noexcept;
void draw_startup_intro();
void shutdown_startup_intro() noexcept;

} // namespace cr::ui
