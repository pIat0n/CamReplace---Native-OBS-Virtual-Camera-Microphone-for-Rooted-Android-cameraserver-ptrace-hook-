#include "ui/Theme.h"
#include <imgui.h>

namespace cr::ui
{

namespace
{

// Helper: IM_COL32 with 0-255 ints.
constexpr ImVec4 rgb(int r, int g, int b, float a = 1.0f)
{
  return ImVec4(r / 255.f, g / 255.f, b / 255.f, a);
}

const Accents g_accents = {
    IM_COL32(0, 200, 81, 255),    // live_green  #00c851
    IM_COL32(255, 179, 48, 255),  // warn_amber  #ffb330
    IM_COL32(229, 57, 53, 255),   // error_red   #e53935
    IM_COL32(140, 140, 140, 255), // dim_text    #8c8c8c
};

} // namespace

const Accents& accents()
{
  return g_accents;
}

void apply_dark_theme()
{
  ImGuiStyle& s = ImGui::GetStyle();

  // Start from ImGui's own dark as a baseline, then override.
  ImGui::StyleColorsDark();

  // --- palette -----------------------------------------------------------
  const ImVec4 bg_deep = rgb(0x14, 0x14, 0x14);     // window/app bg
  const ImVec4 bg_panel = rgb(0x1d, 0x1d, 0x1d);    // child panels
  const ImVec4 bg_raised = rgb(0x26, 0x26, 0x26);   // inputs, frames
  const ImVec4 bg_raised_h = rgb(0x30, 0x30, 0x30); // hovered
  const ImVec4 bg_raised_a = rgb(0x3a, 0x3a, 0x3a); // active
  const ImVec4 border = rgb(0x33, 0x33, 0x33);
  const ImVec4 text_bright = rgb(0xe8, 0xe8, 0xe8);
  const ImVec4 text_dim = rgb(0x8c, 0x8c, 0x8c);
  const ImVec4 accent = rgb(0x00, 0xc8, 0x51); // green
  const ImVec4 accent_h = rgb(0x2e, 0xd8, 0x6e);

  ImVec4* c = s.Colors;
  c[ImGuiCol_Text] = text_bright;
  c[ImGuiCol_TextDisabled] = text_dim;
  c[ImGuiCol_WindowBg] = bg_deep;
  c[ImGuiCol_ChildBg] = bg_panel;
  c[ImGuiCol_PopupBg] = bg_panel;
  c[ImGuiCol_Border] = border;
  c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_FrameBg] = bg_raised;
  c[ImGuiCol_FrameBgHovered] = bg_raised_h;
  c[ImGuiCol_FrameBgActive] = bg_raised_a;
  c[ImGuiCol_TitleBg] = bg_panel;
  c[ImGuiCol_TitleBgActive] = bg_raised;
  c[ImGuiCol_TitleBgCollapsed] = bg_panel;
  c[ImGuiCol_MenuBarBg] = bg_panel;
  c[ImGuiCol_ScrollbarBg] = bg_deep;
  c[ImGuiCol_ScrollbarGrab] = bg_raised;
  c[ImGuiCol_ScrollbarGrabHovered] = bg_raised_h;
  c[ImGuiCol_ScrollbarGrabActive] = bg_raised_a;
  c[ImGuiCol_CheckMark] = accent;
  c[ImGuiCol_SliderGrab] = accent;
  c[ImGuiCol_SliderGrabActive] = accent_h;
  c[ImGuiCol_Button] = bg_raised;
  c[ImGuiCol_ButtonHovered] = bg_raised_h;
  c[ImGuiCol_ButtonActive] = bg_raised_a;
  c[ImGuiCol_Header] = bg_raised;
  c[ImGuiCol_HeaderHovered] = bg_raised_h;
  c[ImGuiCol_HeaderActive] = bg_raised_a;
  c[ImGuiCol_Separator] = border;
  c[ImGuiCol_SeparatorHovered] = accent;
  c[ImGuiCol_SeparatorActive] = accent_h;
  c[ImGuiCol_ResizeGrip] = bg_raised;
  c[ImGuiCol_ResizeGripHovered] = accent;
  c[ImGuiCol_ResizeGripActive] = accent_h;
  c[ImGuiCol_Tab] = bg_panel;
  c[ImGuiCol_TabHovered] = bg_raised_h;
  c[ImGuiCol_TabActive] = bg_raised;
  c[ImGuiCol_TabUnfocused] = bg_panel;
  c[ImGuiCol_TabUnfocusedActive] = bg_raised;
  c[ImGuiCol_PlotLines] = accent;
  c[ImGuiCol_PlotLinesHovered] = accent_h;
  c[ImGuiCol_PlotHistogram] = accent;
  c[ImGuiCol_PlotHistogramHovered] = accent_h;
  c[ImGuiCol_TextSelectedBg] = ImVec4(accent.x, accent.y, accent.z, 0.35f);
  c[ImGuiCol_DragDropTarget] = accent;
  c[ImGuiCol_NavHighlight] = accent;
  c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);

  // --- layout ------------------------------------------------------------
  s.WindowRounding = 6.0f;
  s.ChildRounding = 6.0f;
  s.FrameRounding = 4.0f;
  s.PopupRounding = 6.0f;
  s.ScrollbarRounding = 8.0f;
  s.GrabRounding = 4.0f;
  s.TabRounding = 4.0f;

  s.WindowPadding = ImVec2(12, 12);
  s.FramePadding = ImVec2(8, 6);
  s.ItemSpacing = ImVec2(8, 6);
  s.ItemInnerSpacing = ImVec2(6, 6);

  s.WindowBorderSize = 1.0f;
  s.FrameBorderSize = 0.0f;
  s.TabBorderSize = 0.0f;
  s.ScrollbarSize = 14.0f;
  s.GrabMinSize = 10.0f;
}

} // namespace cr::ui
