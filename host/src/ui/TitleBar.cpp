#include "ui/TitleBar.h"

#include "app/App.h"
#include "render/D3D11Host.h"
#include "resources/Resources.h"
#include "ui/Theme.h"
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <d3d11.h>
#include <mmsystem.h>
#include <windows.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace cr::ui
{

namespace
{

constexpr float kBrandIconHit = 26.0f;
constexpr float kBrandIconSize = 22.0f;
constexpr const wchar_t* kBrandMciAlias = L"cr_brand_click";

struct TitleBarMotion
{
  float icon_hover = 0.0f;
  float icon_active = 0.0f;
};

struct BrandTexture
{
  ID3D11Device* device = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  int width = 0;
  int height = 0;
  bool attempted = false;
};

TitleBarMotion& title_motion()
{
  static TitleBarMotion s;
  return s;
}

float motion_dt()
{
  return std::min(ImGui::GetIO().DeltaTime > 0.0f ? ImGui::GetIO().DeltaTime : (1.0f / 60.0f), 0.05f);
}

float approach(float v, float target, float speed)
{
  const float step = speed * motion_dt();
  if (v < target)
    return std::min(target, v + step);
  return std::max(target, v - step);
}

float clamp01(float v)
{
  return std::max(0.0f, std::min(1.0f, v));
}

float mix(float a, float b, float t)
{
  t = clamp01(t);
  return a + (b - a) * t;
}

BrandTexture& brand_texture_state()
{
  static BrandTexture s;
  return s;
}

void release_brand_texture(BrandTexture& tex) noexcept
{
  if (tex.srv)
  {
    tex.srv->Release();
    tex.srv = nullptr;
  }
  tex.device = nullptr;
  tex.width = 0;
  tex.height = 0;
}

ID3D11ShaderResourceView* brand_icon_srv() noexcept
{
  ID3D11Device* dev = cr::app::d3d_device();
  if (!dev)
    return nullptr;

  BrandTexture& tex = brand_texture_state();
  if (tex.srv && tex.device == dev)
    return tex.srv;
  if (tex.device != dev)
  {
    release_brand_texture(tex);
    tex.attempted = false;
  }
  if (tex.attempted)
    return nullptr;
  tex.attempted = true;

  const auto blob = cr::resources::titlebar_brand_png();
  if (!blob.data || blob.size == 0)
    return nullptr;

  int w = 0;
  int h = 0;
  int channels = 0;
  unsigned char* pixels = stbi_load_from_memory(
      blob.data,
      static_cast<int>(blob.size),
      &w,
      &h,
      &channels,
      4);
  if (!pixels || w <= 0 || h <= 0)
  {
    if (pixels)
      stbi_image_free(pixels);
    return nullptr;
  }

  D3D11_TEXTURE2D_DESC td{};
  td.Width = static_cast<UINT>(w);
  td.Height = static_cast<UINT>(h);
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

  D3D11_SUBRESOURCE_DATA sd{};
  sd.pSysMem = pixels;
  sd.SysMemPitch = static_cast<UINT>(w * 4);

  ID3D11Texture2D* d3d_tex = nullptr;
  if (FAILED(dev->CreateTexture2D(&td, &sd, &d3d_tex)) || !d3d_tex)
  {
    stbi_image_free(pixels);
    return nullptr;
  }

  D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
  sv.Format = td.Format;
  sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  sv.Texture2D.MipLevels = 1;
  if (FAILED(dev->CreateShaderResourceView(d3d_tex, &sv, &tex.srv)))
  {
    d3d_tex->Release();
    stbi_image_free(pixels);
    release_brand_texture(tex);
    return nullptr;
  }

  d3d_tex->Release();
  stbi_image_free(pixels);
  tex.device = dev;
  tex.width = w;
  tex.height = h;
  return tex.srv;
}

std::filesystem::path local_appdata_cache_dir()
{
  wchar_t local[MAX_PATH]{};
  DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
  std::filesystem::path base;
  if (n > 0 && n < MAX_PATH)
  {
    base = local;
  }
  else
  {
    wchar_t tmp[MAX_PATH]{};
    DWORD tn = ::GetTempPathW(MAX_PATH, tmp);
    if (tn > 0 && tn < MAX_PATH)
      base = tmp;
  }
  if (base.empty())
    return {};
  return base / L"CameraReplace" / L"assets";
}

std::filesystem::path ensure_brand_click_mp3_file() noexcept
{
  try
  {
    const auto blob = cr::resources::titlebar_brand_click_mp3();
    if (!blob.data || blob.size == 0)
      return {};

    const std::filesystem::path dir = local_appdata_cache_dir();
    if (dir.empty())
      return {};
    std::filesystem::create_directories(dir);

    const std::filesystem::path path = dir / L"brand_click.mp3";
    bool write = true;
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && !ec)
    {
      const auto size = std::filesystem::file_size(path, ec);
      write = ec || size != blob.size;
    }
    if (write)
    {
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      if (!out)
        return {};
      out.write(reinterpret_cast<const char*>(blob.data), static_cast<std::streamsize>(blob.size));
      if (!out)
        return {};
    }
    return path;
  }
  catch (...)
  {
    return {};
  }
}

void play_brand_click_sound() noexcept
{
  const std::filesystem::path path = ensure_brand_click_mp3_file();
  if (path.empty())
    return;

  const std::wstring stop = std::wstring(L"stop ") + kBrandMciAlias;
  const std::wstring close = std::wstring(L"close ") + kBrandMciAlias;
  ::mciSendStringW(stop.c_str(), nullptr, 0, nullptr);
  ::mciSendStringW(close.c_str(), nullptr, 0, nullptr);

  std::wstring open = L"open \"";
  open += path.wstring();
  open += L"\" type mpegvideo alias ";
  open += kBrandMciAlias;
  if (::mciSendStringW(open.c_str(), nullptr, 0, nullptr) != 0)
    return;

  const std::wstring play = std::wstring(L"play ") + kBrandMciAlias + L" from 0";
  ::mciSendStringW(play.c_str(), nullptr, 0, nullptr);
}

bool brand_icon_button(float center_x)
{
  TitleBarMotion& m = title_motion();
  const float title_h = static_cast<float>(cr::app::kTitleBarHeight);
  const float x = center_x - kBrandIconHit * 0.5f;
  const float y = (title_h - kBrandIconHit) * 0.5f;

  ImGui::SetCursorPos(ImVec2(x, y));
  ImVec2 button_pos = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##brand_icon", ImVec2(kBrandIconHit, kBrandIconHit));
  const bool hovered = ImGui::IsItemHovered();
  const bool active = ImGui::IsItemActive();
  const bool clicked = ImGui::IsItemClicked();
  if (hovered)
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

  m.icon_hover = approach(m.icon_hover, hovered ? 1.0f : 0.0f, hovered ? 9.0f : 6.0f);
  m.icon_active = approach(m.icon_active, active ? 1.0f : 0.0f, active ? 18.0f : 12.0f);

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 center(button_pos.x + kBrandIconHit * 0.5f, button_pos.y + kBrandIconHit * 0.5f);
  const float bg_r = kBrandIconHit * 0.5f;
  const float glow_r = bg_r + 2.5f * m.icon_hover;
  const ImU32 glow = IM_COL32(255, 255, 255, static_cast<int>(65 * m.icon_hover));
  const ImU32 border = IM_COL32(255, 255, 255, static_cast<int>(60 + 115 * m.icon_hover));
  dl->AddCircleFilled(center, glow_r, glow, 32);
  dl->AddCircleFilled(center, bg_r, IM_COL32(6, 6, 6, 210), 32);

  const float scale = mix(1.0f, 0.90f, m.icon_active);
  const float size = kBrandIconSize * scale;
  const ImVec2 min(center.x - size * 0.5f, center.y - size * 0.5f);
  const ImVec2 max(center.x + size * 0.5f, center.y + size * 0.5f);
  if (ID3D11ShaderResourceView* srv = brand_icon_srv())
  {
    dl->AddImageRounded((ImTextureID) srv, min, max, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, size * 0.5f);
  }
  else
  {
    dl->AddCircleFilled(center, size * 0.5f, IM_COL32(20, 20, 20, 255), 32);
    dl->AddCircleFilled(ImVec2(center.x - size * 0.18f, center.y - size * 0.16f), size * 0.17f, IM_COL32_WHITE, 18);
  }
  dl->AddCircle(center, bg_r, border, 32, 1.2f + m.icon_hover * 0.8f);

  if (clicked)
    play_brand_click_sound();
  return clicked;
}

// Circular traffic-light button. Draws a filled disc; on hover, overlays a
// 1.5-px dark icon (× / — / ▢). Returns true when clicked.
bool dot_button(const char* id, ImVec4 colour, ImVec4 hover, float radius = 7.0f)
{
  ImGui::PushID(id);
  ImVec2 pos = ImGui::GetCursorScreenPos();
  ImVec2 center{pos.x + radius, pos.y + radius};

  ImGui::InvisibleButton(id, ImVec2(radius * 2, radius * 2));
  bool hovered = ImGui::IsItemHovered();
  bool clicked = ImGui::IsItemClicked();

  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddCircleFilled(center, radius, ImGui::ColorConvertFloat4ToU32(hovered ? hover : colour), 24);

  if (hovered)
  {
    const ImU32 icon = ImGui::ColorConvertFloat4ToU32(ImVec4(0.12f, 0.12f, 0.12f, 1.0f));
    if (!std::strcmp(id, "close"))
    {
      float s = radius * 0.45f;
      dl->AddLine({center.x - s, center.y - s}, {center.x + s, center.y + s}, icon, 1.6f);
      dl->AddLine({center.x + s, center.y - s}, {center.x - s, center.y + s}, icon, 1.6f);
    }
    else if (!std::strcmp(id, "minimize"))
    {
      float s = radius * 0.5f;
      dl->AddLine({center.x - s, center.y}, {center.x + s, center.y}, icon, 1.6f);
    }
    else if (!std::strcmp(id, "maximize"))
    {
      float s = radius * 0.4f;
      dl->AddRect({center.x - s, center.y - s}, {center.x + s, center.y + s}, icon, 0.0f, 0, 1.6f);
    }
  }
  ImGui::PopID();
  return clicked;
}

} // namespace

void draw_title_bar()
{
  const float title_h = static_cast<float>(cr::app::kTitleBarHeight);
  const float win_w = ImGui::GetWindowWidth();

  // --- background strip across the title row ------------------------------
  // A subtle band that visually separates the title from the main content.
  {
    ImVec2 p = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + win_w, p.y + title_h), ImGui::ColorConvertFloat4ToU32(ImVec4(0.11f, 0.11f, 0.11f, 1.0f)));
    // Hairline separator.
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + title_h - 1), ImVec2(p.x + win_w, p.y + title_h - 1), ImGui::ColorConvertFloat4ToU32(ImVec4(0.22f, 0.22f, 0.22f, 1.0f)));
  }

  // --- centered brand icon ------------------------------------------------
  brand_icon_button(win_w * 0.5f);

  // --- right-side traffic lights -----------------------------------------
  // Order (left→right): minimize, maximize, close — matches egt_config_tool
  // and, notably, macOS (close is the rightmost here; swap if you prefer
  // macOS-exact where close is leftmost).
  const float radius = 7.0f;
  const float spacing = 10.0f;
  const float total_w = radius * 2.0f * 3.0f + spacing * 2.0f;
  const float start_x = win_w - total_w - 16.0f;
  const float dot_y = (title_h - radius * 2.0f) * 0.5f;

  HWND hwnd = cr::app::App::hwnd();

  ImGui::SetCursorPos(ImVec2(start_x, dot_y));
  if (dot_button("minimize", ImVec4(0.95f, 0.75f, 0.15f, 1.0f), ImVec4(1.00f, 0.85f, 0.25f, 1.0f), radius))
  {
    if (hwnd)
      cr::app::App::request_minimize_animated();
  }

  ImGui::SameLine(0, spacing);
  if (dot_button("maximize", ImVec4(0.25f, 0.78f, 0.25f, 1.0f), ImVec4(0.35f, 0.90f, 0.35f, 1.0f), radius))
  {
    if (hwnd)
      cr::app::App::request_toggle_maximize_animated();
  }

  ImGui::SameLine(0, spacing);
  if (dot_button("close", ImVec4(0.92f, 0.28f, 0.24f, 1.0f), ImVec4(1.00f, 0.38f, 0.34f, 1.0f), radius))
  {
    if (hwnd)
      cr::app::App::request_close_animated();
  }

  // Advance the cursor past the bar so the caller can draw its content
  // starting on the next row.
  ImGui::SetCursorPosY(title_h);
}

} // namespace cr::ui
