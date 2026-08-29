#pragma once

// Dark theme for Camera Replace.
// - Neutral charcoal panels (#1a1a1a / #242424)
// - Single accent colour for "live / recording" state (#00c851 green)
// - Readable mid-gray text (#e0e0e0)
// Call apply() once after ImGui::CreateContext().

namespace cr::ui
{

void apply_dark_theme();

// Accent colors used by panels (kept here so they're consistent everywhere).
struct Accents
{
  // ImU32 (packed 0xAABBGGRR — ImGui's native format).
  unsigned int live_green;
  unsigned int warn_amber;
  unsigned int error_red;
  unsigned int dim_text;
};
const Accents& accents();

} // namespace cr::ui
