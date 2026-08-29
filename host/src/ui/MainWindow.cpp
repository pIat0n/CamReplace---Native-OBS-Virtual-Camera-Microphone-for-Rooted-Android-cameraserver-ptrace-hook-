#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/TitleBar.h"
#include "app/App.h"
#include "device/DeviceScanner.h"
#include "device/LogTail.h"
#include "render/D3D11Host.h"
#include "render/H264PreviewDecoder.h"
#include "render/LivePreview.h"
#include "resources/Resources.h"
#include "source/FeedController.h"
#include "util/Log.h"
#include "util/SessionLog.h"


#include <shellapi.h> // ShellExecuteW for "Launch OBS"

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <new>
#include <thread>
#include <unordered_map>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <mmsystem.h>
#include <windows.h>
#include "util/WinApiDyn.h"

namespace cr::ui
{

namespace
{

// --- transient UI state ------------------------------------------------------

struct UiState
{
  bool software_running = false; // hooks loaded / software lifecycle active
  bool streaming = false;       // OBS source running
  bool sound_replacing = false; // audio pump active
  bool photo_replacing = false; // BLOB still-capture sub
  bool preview_on = true;       // toggled by preview button
  bool log_expanded = false; // log collapsed by default

  struct UiMotion
  {
    float root_alpha = 1.0f;
    float root_scale = 1.0f;
    float device_panel_t = 0.0f;
    float device_panel_w = 340.0f;
    float log_t = 0.0f;
    float transport_t = 0.0f;
    bool transport_open = false;
    bool any_button_animating = false;
    float device_link_hover = 0.0f;
    float entrance_t = 1.0f;
    std::string entrance_session_token;
    struct Button
    {
      float hover = 0.0f;
      float active = 0.0f;
    };
    std::unordered_map<ImGuiID, Button> buttons;
  } motion;

  // Selected device serial (empty = first-authorised-wins).
  std::string selected_serial;

};

UiState& state()
{
  static UiState s;
  return s;
}

// Deploy status arrives from background worker threads. Route every step
// into the shared SessionLog so the single log panel sees both host-side
// steps and phone-side logcat lines in order.
void deploy_log_push(cr::device::DeployStatus s)
{
  std::string line = s.step;
  if (!s.detail.empty())
  {
    // Collapse the detail to a single line - multi-line stdout dumps
    // become unreadable in a ring panel. Plain ASCII hyphen so the
    // ImGui default font (no em-dash glyph) renders it correctly.
    std::string d = s.detail;
    for (char& c : d)
      if (c == '\n' || c == '\r')
        c = ' ';
    line.append(" - ");
    line.append(d);
  }
  using DK = cr::device::DeployStatus::Kind;
  switch (s.kind)
  {
  case DK::Ok:
    cr::log::ok("deploy", line);
    break;
  case DK::Err:
    cr::log::error("deploy", line);
    break;
  case DK::Info:
    cr::log::info("deploy", line);
    break;
  }
}

// --- helpers -----------------------------------------------------------------

void status_dot(const char* label, bool ok, bool pending = false)
{
  ImU32 col = pending ? accents().warn_amber : (ok ? accents().live_green : accents().error_red);

  ImVec2 p = ImGui::GetCursorScreenPos();
  float r = ImGui::GetFontSize() * 0.35f;
  ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r + 2, p.y + ImGui::GetTextLineHeight() * 0.5f), r, col);
  ImGui::Dummy(ImVec2(r * 2 + 6, ImGui::GetTextLineHeight()));
  ImGui::SameLine();
  ImGui::TextUnformatted(label);
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

bool needs_motion_frames(float v, float target, float eps = 0.001f)
{
  return std::abs(v - target) > eps;
}

float mix(float a, float b, float t)
{
  t = std::max(0.0f, std::min(1.0f, t));
  return a + (b - a) * t;
}

float ease_out_cubic(float t)
{
  t = std::max(0.0f, std::min(1.0f, t));
  const float inv = 1.0f - t;
  return 1.0f - inv * inv * inv;
}

float panel_reveal(float entrance_t, float start, float duration)
{
  if (duration <= 0.001f)
    return entrance_t >= start ? 1.0f : 0.0f;
  return ease_out_cubic((entrance_t - start) / duration);
}

void update_main_entrance(UiState& ui)
{
  if (ui.motion.entrance_t < 1.0f)
    ui.motion.entrance_t = std::min(1.0f, ui.motion.entrance_t + motion_dt() / 0.60f);
}

void draw_terminal_scanline_overlay(float reveal_t)
{
  if (reveal_t <= 0.001f || reveal_t >= 0.999f)
    return;

  const ImVec2 min = ImGui::GetItemRectMin();
  const ImVec2 max = ImGui::GetItemRectMax();
  if (max.x <= min.x || max.y <= min.y)
    return;

  const float y = mix(min.y, max.y, reveal_t);
  const float tail = std::min(24.0f, (max.y - min.y) * 0.22f);
  const int alpha = static_cast<int>(120.0f * (1.0f - reveal_t));
  const int tail_alpha = static_cast<int>(32.0f * (1.0f - reveal_t));

  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(min, max, true);
  dl->AddRectFilled(
      ImVec2(min.x, std::max(min.y, y - tail)),
      ImVec2(max.x, y),
      IM_COL32(255, 255, 255, tail_alpha));
  dl->AddRectFilled(
      ImVec2(min.x, std::max(min.y, y - 1.0f)),
      ImVec2(max.x, std::min(max.y, y + 1.5f)),
      IM_COL32(255, 255, 255, alpha));
  dl->PopClipRect();
}

template <class Fn>
void draw_revealed_panel(float reveal_t, Fn&& fn)
{
  const float alpha = std::max(0.001f, std::min(1.0f, reveal_t));
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * alpha);
  fn();
  ImGui::PopStyleVar();
  draw_terminal_scanline_overlay(reveal_t);
}

ImU32 mix_col(ImU32 a, ImU32 b, float t)
{
  ImVec4 av = ImGui::ColorConvertU32ToFloat4(a);
  ImVec4 bv = ImGui::ColorConvertU32ToFloat4(b);
  return ImGui::ColorConvertFloat4ToU32(ImVec4(mix(av.x, bv.x, t), mix(av.y, bv.y, t), mix(av.z, bv.z, t), mix(av.w, bv.w, t)));
}

void start_log_tail_if_enabled(const std::string& serial)
{
  cr::device::LogTail::instance().start(serial);
}

void reset_replace_ui_state(bool reset_software)
{
  if (reset_software)
    state().software_running = false;
  state().streaming = false;
  state().sound_replacing = false;
  state().photo_replacing = false;
  cr::render::LivePreview::instance().reset();
}

void apply_video_start_settings()
{
  state().preview_on = true;
  cr::render::H264PreviewDecoder::set_enabled(true);
}

enum class ButtonVariant
{
  Default,
  Primary,
  Live,
  Destructive,
  Warning,
};

bool animated_button(const char* label, ImVec2 size = ImVec2(0, 0), ButtonVariant variant = ButtonVariant::Default)
{
  ImU32 base = IM_COL32(38, 38, 38, 255);
  ImU32 hover = IM_COL32(54, 54, 54, 255);
  ImU32 active = IM_COL32(64, 64, 64, 255);
  ImU32 text = IM_COL32(232, 232, 232, 255);
  ImU32 glow = IM_COL32(120, 120, 120, 100);

  switch (variant)
  {
  case ButtonVariant::Primary:
    base = IM_COL32(0, 120, 54, 255);
    hover = IM_COL32(0, 168, 74, 255);
    active = IM_COL32(0, 196, 88, 255);
    glow = IM_COL32(0, 210, 100, 140);
    break;
  case ButtonVariant::Live:
    base = accents().live_green;
    hover = IM_COL32(0, 230, 100, 255);
    active = IM_COL32(0, 250, 116, 255);
    text = IM_COL32(10, 10, 10, 255);
    glow = IM_COL32(0, 230, 100, 150);
    break;
  case ButtonVariant::Destructive:
    base = IM_COL32(120, 40, 40, 255);
    hover = IM_COL32(160, 50, 50, 255);
    active = IM_COL32(185, 62, 62, 255);
    glow = IM_COL32(229, 57, 53, 150);
    break;
  case ButtonVariant::Warning:
    base = IM_COL32(232, 119, 35, 255);
    hover = IM_COL32(255, 142, 51, 255);
    active = IM_COL32(206, 100, 20, 255);
    text = IM_COL32(255, 255, 255, 255);
    glow = IM_COL32(255, 160, 70, 150);
    break;
  case ButtonVariant::Default:
    break;
  }

  UiState& ui = state();
  ImGuiID id = ImGui::GetID(label);
  UiState::UiMotion::Button& m = ui.motion.buttons[id];
  ImU32 bg = mix_col(base, hover, m.hover);
  bg = mix_col(bg, active, m.active);

  ImGui::PushStyleColor(ImGuiCol_Button, bg);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, mix_col(bg, hover, 0.65f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);
  ImGui::PushStyleColor(ImGuiCol_Text, text);
  bool clicked = ImGui::Button(label, size);
  ImGui::PopStyleColor(4);

  const bool hovered_now = ImGui::IsItemHovered();
  const bool active_now = ImGui::IsItemActive();
  const float hover_target = hovered_now ? 1.0f : 0.0f;
  const float active_target = active_now ? 1.0f : 0.0f;
  m.hover = approach(m.hover, hovered_now ? 1.0f : 0.0f, hovered_now ? 8.0f : 5.0f);
  m.active = approach(m.active, active_now ? 1.0f : 0.0f, active_now ? 14.0f : 9.0f);
  if (needs_motion_frames(m.hover, hover_target, 0.01f) || needs_motion_frames(m.active, active_target, 0.01f))
  {
    ui.motion.any_button_animating = true;
  }

  if (m.hover > 0.01f || m.active > 0.01f)
  {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 min = ImGui::GetItemRectMin();
    ImVec2 max = ImGui::GetItemRectMax();
    const float inset = m.active * 1.5f;
    min.x += inset;
    min.y += inset;
    max.x -= inset;
    max.y -= inset;
    dl->AddRect(min, max, mix_col(IM_COL32(0, 0, 0, 0), glow, m.hover), ImGui::GetStyle().FrameRounding, 0, 1.4f + m.active);
  }

  return clicked;
}

enum class DeviceMenuSound
{
  Granted,
  Error,
};

struct DeviceWaveSample
{
  WAVEFORMATEX format{};
  const unsigned char* pcm = nullptr;
  DWORD pcm_size = 0;
  bool ready = false;
};

struct DeviceWavePlayback
{
  HWAVEOUT out = nullptr;
  WAVEHDR header{};
  HANDLE done = nullptr;
};

bool device_wav_chunk_id_is(const unsigned char* p, const char* id) noexcept
{
  return std::memcmp(p, id, 4) == 0;
}

std::uint16_t device_wav_le16(const unsigned char* p) noexcept
{
  return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t device_wav_le32(const unsigned char* p) noexcept
{
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}

DeviceWaveSample parse_device_wave(cr::resources::Blob blob) noexcept
{
  DeviceWaveSample out{};
  if (!blob.data || blob.size < 12)
    return out;

  const auto* data = blob.data;
  const std::size_t size = blob.size;
  if (!device_wav_chunk_id_is(data, "RIFF") || !device_wav_chunk_id_is(data + 8, "WAVE"))
    return out;

  bool have_fmt = false;
  bool have_data = false;
  for (std::size_t off = 12; off + 8 <= size;)
  {
    const unsigned char* chunk = data + off;
    const std::uint32_t chunk_size = device_wav_le32(chunk + 4);
    const std::size_t payload = off + 8;
    if (payload > size || chunk_size > size - payload)
      break;

    if (device_wav_chunk_id_is(chunk, "fmt ") && chunk_size >= 16)
    {
      const unsigned char* fmt = data + payload;
      const std::uint16_t tag = device_wav_le16(fmt);
      const std::uint16_t channels = device_wav_le16(fmt + 2);
      const std::uint32_t samples_per_sec = device_wav_le32(fmt + 4);
      const std::uint32_t avg_bytes_per_sec = device_wav_le32(fmt + 8);
      const std::uint16_t block_align = device_wav_le16(fmt + 12);
      const std::uint16_t bits_per_sample = device_wav_le16(fmt + 14);
      if (tag == WAVE_FORMAT_PCM && channels > 0 && samples_per_sec > 0 &&
          block_align > 0 && bits_per_sample == 16)
      {
        out.format.wFormatTag = WAVE_FORMAT_PCM;
        out.format.nChannels = channels;
        out.format.nSamplesPerSec = samples_per_sec;
        out.format.nAvgBytesPerSec = avg_bytes_per_sec;
        out.format.nBlockAlign = block_align;
        out.format.wBitsPerSample = bits_per_sample;
        out.format.cbSize = 0;
        have_fmt = true;
      }
    }
    else if (device_wav_chunk_id_is(chunk, "data") && chunk_size > 0)
    {
      out.pcm = data + payload;
      out.pcm_size = static_cast<DWORD>(chunk_size);
      have_data = true;
    }

    off = payload + chunk_size + (chunk_size & 1u);
  }

  out.ready = have_fmt && have_data && out.pcm && out.pcm_size > 0;
  return out;
}

const DeviceWaveSample& device_wave_sample(DeviceMenuSound sound) noexcept
{
  static const DeviceWaveSample granted = parse_device_wave(cr::resources::device_granted_wav());
  static const DeviceWaveSample error = parse_device_wave(cr::resources::device_error_wav());
  return sound == DeviceMenuSound::Error ? error : granted;
}

void cleanup_device_wave_playback(DeviceWavePlayback* playback) noexcept
{
  if (!playback)
    return;

  if (playback->done)
  {
    const DWORD wait = ::WaitForSingleObject(playback->done, 5000);
    if (wait != WAIT_OBJECT_0 && playback->out)
      ::waveOutReset(playback->out);
  }
  if (playback->out && playback->header.lpData)
    ::waveOutUnprepareHeader(playback->out, &playback->header, sizeof(playback->header));
  if (playback->out)
    ::waveOutClose(playback->out);
  if (playback->done)
    ::CloseHandle(playback->done);
  delete playback;
}

void CALLBACK device_wave_done(HWAVEOUT, UINT msg, DWORD_PTR instance, DWORD_PTR, DWORD_PTR) noexcept
{
  if (msg != WOM_DONE)
    return;
  auto* playback = reinterpret_cast<DeviceWavePlayback*>(instance);
  if (playback && playback->done)
    ::SetEvent(playback->done);
}

void play_device_menu_sound(DeviceMenuSound sound) noexcept
{
  const auto& sample = device_wave_sample(sound);
  if (!sample.ready)
    return;

  auto* playback = new (std::nothrow) DeviceWavePlayback();
  if (!playback)
    return;

  playback->done = ::CreateEventA(nullptr, FALSE, FALSE, nullptr);
  if (!playback->done)
  {
    delete playback;
    return;
  }

  MMRESULT rc = ::waveOutOpen(
      &playback->out,
      WAVE_MAPPER,
      &sample.format,
      reinterpret_cast<DWORD_PTR>(&device_wave_done),
      reinterpret_cast<DWORD_PTR>(playback),
      CALLBACK_FUNCTION);
  if (rc != MMSYSERR_NOERROR || !playback->out)
  {
    ::CloseHandle(playback->done);
    delete playback;
    return;
  }

  playback->header.lpData = const_cast<LPSTR>(reinterpret_cast<const char*>(sample.pcm));
  playback->header.dwBufferLength = sample.pcm_size;
  rc = ::waveOutPrepareHeader(playback->out, &playback->header, sizeof(playback->header));
  if (rc != MMSYSERR_NOERROR)
  {
    ::waveOutClose(playback->out);
    ::CloseHandle(playback->done);
    delete playback;
    return;
  }
  rc = ::waveOutWrite(playback->out, &playback->header, sizeof(playback->header));
  if (rc != MMSYSERR_NOERROR)
  {
    ::waveOutUnprepareHeader(playback->out, &playback->header, sizeof(playback->header));
    ::waveOutClose(playback->out);
    ::CloseHandle(playback->done);
    delete playback;
    return;
  }

  try
  {
    std::thread(cleanup_device_wave_playback, playback).detach();
  }
  catch (...)
  {
    ::waveOutReset(playback->out);
    cleanup_device_wave_playback(playback);
  }
}

bool device_menu_button(const char* label, ImVec2 size = ImVec2(0, 0), ButtonVariant variant = ButtonVariant::Default)
{
  if (!animated_button(label, size, variant))
    return false;
  play_device_menu_sound(variant == ButtonVariant::Destructive ? DeviceMenuSound::Error : DeviceMenuSound::Granted);
  return true;
}


void draw_transport_dropdown(bool disabled)
{
  UiState& ui = state();
  const cr::source::Transport cur = cr::source::FeedController::transport();
  const std::string lbl_compressed = "Compressed";
  const std::string lbl_uncompressed = "Uncompressed";
  const bool raw = (cur == cr::source::Transport::Raw);
  const std::string button_label = std::string(raw ? lbl_uncompressed : lbl_compressed) + "  v##transport_anim";

  if (disabled)
  {
    ui.motion.transport_open = false;
    ui.motion.transport_t = approach(ui.motion.transport_t, 0.0f, 10.0f);
  }

  ImGui::BeginDisabled(disabled);
  if (animated_button(button_label.c_str(), ImVec2(150, 0)))
  {
    ui.motion.transport_open = !ui.motion.transport_open;
    if (ui.motion.transport_open)
    {
      ImGui::OpenPopup("##transport_popup");
    }
  }
  ImGui::EndDisabled();

  const float popup_w = ImGui::GetItemRectSize().x;
  ImVec2 popup_pos = ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + 5.0f);
  const bool popup_open = ImGui::IsPopupOpen("##transport_popup");
  if (!popup_open)
    ui.motion.transport_open = false;
  ui.motion.transport_t = approach(ui.motion.transport_t, ui.motion.transport_open ? 1.0f : 0.0f, 9.0f);

  if (ui.motion.transport_t > 0.001f)
  {
    const float t = ui.motion.transport_t;
    ImGui::SetNextWindowPos(ImVec2(popup_pos.x, popup_pos.y - (1.0f - t) * 8.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(popup_w, 0.0f), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * t);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 6));
    if (ImGui::BeginPopup("##transport_popup", ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize))
    {
      auto option = [&](const std::string& label, cr::source::Transport tr)
      {
        const bool selected = (cur == tr);
        if (selected)
        {
          ImGui::PushStyleColor(ImGuiCol_Text, accents().live_green);
        }
        if (animated_button(label.c_str(), ImVec2(-1, 28), selected ? ButtonVariant::Live : ButtonVariant::Default))
        {
          cr::source::FeedController::set_transport(tr);
          ui.motion.transport_open = false;
          ImGui::CloseCurrentPopup();
        }
        if (selected)
          ImGui::PopStyleColor();
      };
      option(lbl_compressed, cr::source::Transport::Compressed);
      option(lbl_uncompressed, cr::source::Transport::Raw);
      ImGui::EndPopup();
    }
    ImGui::PopStyleVar(3);
  }
}

const cr::device::DeviceInfo* resolve_selected(const std::vector<cr::device::DeviceInfo>& v)
{
  if (v.empty())
    return nullptr;

  auto& sel = state().selected_serial;
  if (!sel.empty())
  {
    auto it = std::find_if(v.begin(), v.end(), [&](const auto& d) { return d.serial == sel; });
    if (it != v.end())
      return &*it;
  }
  // First authorised wins by default; fallback to first overall.
  for (auto& d : v)
    if (d.auth == cr::device::AuthState::Authorized)
      return &d;
  return &v.front();
}

// --- panels ------------------------------------------------------------------

// OBS RTMP status card drawn above the preview panel. Shows:
//   - the URL OBS needs to stream to (with a Copy button),
//   - a connection indicator (grey/green/red),
//   - the resolution/fps OBS is currently publishing (if any),
//   - a one-click Launch OBS button when we can find obs64.exe via any of
//     the standard installers, the registry, or a Steam library folder.
// Compact — one row tall — so the preview underneath stays big.
namespace
{

std::string wide_to_utf8(const std::wstring& w)
{
  if (w.empty())
    return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int) w.size(), nullptr, 0, nullptr, nullptr);
  std::string out(n > 0 ? n : 0, 0);
  if (n > 0)
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int) w.size(), out.data(), n, nullptr, nullptr);
  return out;
}

bool file_exists_w(const std::wstring& p)
{
  DWORD a = GetFileAttributesW(p.c_str());
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Read a string value from the registry. Returns empty on any failure.
// `view_flag` lets the caller switch between the 32/64-bit registry views —
// Steam writes its install path to the 32-bit view (WOW6432Node) on x64.
std::wstring reg_read_sz(HKEY root, const wchar_t* sub, const wchar_t* val, REGSAM view_flag = 0)
{
  HKEY k = nullptr;
  if (cr::winapi::RegOpenKeyExW(root, sub, 0, KEY_READ | view_flag, &k) != ERROR_SUCCESS)
    return {};
  wchar_t buf[1024]{};
  DWORD bytes = sizeof(buf) - sizeof(wchar_t);
  DWORD type = 0;
  LSTATUS s = cr::winapi::RegQueryValueExW(k, val, nullptr, &type, reinterpret_cast<BYTE*>(buf), &bytes);
  cr::winapi::RegCloseKey(k);
  if (s != ERROR_SUCCESS)
    return {};
  if (type != REG_SZ && type != REG_EXPAND_SZ)
    return {};
  buf[(bytes / sizeof(wchar_t))] = 0;
  return std::wstring(buf);
}

// Walk every Steam library folder declared in `<SteamRoot>\steamapps\
// libraryfolders.vdf` and return a list of library roots. Each root is the
// directory that contains `steamapps\common\…`. The default library (the
// Steam install itself) is always included first.
std::vector<std::wstring> steam_library_roots()
{
  std::vector<std::wstring> out;

  // Where Steam itself lives.
  std::wstring steam = reg_read_sz(HKEY_CURRENT_USER, L"SOFTWARE\\Valve\\Steam", L"SteamPath");
  if (steam.empty())
  {
    steam = reg_read_sz(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", KEY_WOW64_32KEY);
  }
  if (steam.empty())
    return out;

  // Steam stores forward slashes in HKCU; normalise.
  for (auto& c : steam)
    if (c == L'/')
      c = L'\\';
  out.push_back(steam);

  // Parse libraryfolders.vdf to discover additional library roots.
  std::wstring vdf = steam + L"\\steamapps\\libraryfolders.vdf";
  FILE* f = _wfopen(vdf.c_str(), L"rb");
  if (!f)
    return out;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::string buf(sz > 0 ? (size_t) sz : 0, 0);
  if (sz > 0)
    (void) fread(buf.data(), 1, (size_t) sz, f);
  fclose(f);

  // Crude parse: scan for `"path"  "<value>"` pairs. Good enough — VDF
  // is line-oriented and we only need string values.
  size_t pos = 0;
  while ((pos = buf.find("\"path\"", pos)) != std::string::npos)
  {
    pos += 6;
    while (pos < buf.size() && (buf[pos] == ' ' || buf[pos] == '\t'))
      ++pos;
    if (pos >= buf.size() || buf[pos] != '"')
      continue;
    ++pos;
    size_t end = buf.find('"', pos);
    if (end == std::string::npos)
      break;
    std::string raw = buf.substr(pos, end - pos);
    pos = end + 1;

    // Unescape `\\` → `\` (VDF escapes backslashes inside strings).
    std::string norm;
    norm.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i)
    {
      if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '\\')
      {
        norm += '\\';
        ++i;
      }
      else
      {
        norm += raw[i];
      }
    }
    int wn = MultiByteToWideChar(CP_UTF8, 0, norm.c_str(), (int) norm.size(), nullptr, 0);
    if (wn <= 0)
      continue;
    std::wstring wlib(wn, 0);
    MultiByteToWideChar(CP_UTF8, 0, norm.c_str(), (int) norm.size(), wlib.data(), wn);
    if (wlib != steam)
      out.push_back(std::move(wlib));
  }
  return out;
}

} // namespace

std::string find_obs_exe()
{
  // 1. Standard installer locations.
  const std::wstring standard_paths[] = {
      L"C:\\Program Files\\obs-studio\\bin\\64bit\\obs64.exe",
      L"C:\\Program Files (x86)\\obs-studio\\bin\\64bit\\obs64.exe",
  };
  for (const auto& c : standard_paths)
  {
    if (file_exists_w(c.c_str()))
      return wide_to_utf8(c);
  }

  // 2. App Paths registry (set by the OBS installer for `start obs64.exe`).
  for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER})
  {
    std::wstring p = reg_read_sz(root, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\obs64.exe", nullptr);
    if (!p.empty() && file_exists_w(p))
      return wide_to_utf8(p);
  }

  // 3. Uninstaller InstallLocation (covers OBS installed to a non-default
  //    Program Files prefix).
  for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER})
  {
    for (REGSAM view : {REGSAM(0), REGSAM(KEY_WOW64_32KEY)})
    {
      std::wstring inst = reg_read_sz(root, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\OBS Studio", L"InstallLocation", view);
      if (inst.empty())
        continue;
      std::wstring c = inst + L"\\bin\\64bit\\obs64.exe";
      if (file_exists_w(c))
        return wide_to_utf8(c);
    }
  }

  // 4. Steam install (any library folder).
  for (const auto& root : steam_library_roots())
  {
    std::wstring c = root + L"\\steamapps\\common\\OBS Studio\\bin\\64bit\\obs64.exe";
    if (file_exists_w(c))
      return wide_to_utf8(c);
  }

  return {};
}

void draw_obs_info_panel()
{
  const auto st = cr::source::FeedController::obs_status();

  // Single-row panel: status dot + status text + RTMP URL + Copy + Launch OBS.
  // Optional compact stream stats appended on the same line when connected.
  const float row_h = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().WindowPadding.y * 2.0f;
  ImGui::BeginChild("obs_info", ImVec2(0, row_h), true);

  const bool obs_pipeline_up = state().streaming || state().photo_replacing || state().sound_replacing;

  ImU32 col = accents().dim_text;
  // Store the chosen status until TextUnformatted consumes it below.
  std::string status_text = "Waiting for OBS to connect…";
  if (!obs_pipeline_up)
  {
    col = accents().dim_text;
    status_text = "Press Start, then stream from OBS.";
  }
  else if (st.obs_connected)
  {
    col = accents().live_green;
    status_text = "OBS connected";
  }
  else
  {
    col = accents().warn_amber;
    status_text = "Waiting for OBS…";
  }
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 7, p.y + ImGui::GetFrameHeight() * 0.5f), 5.0f, col);
  ImGui::Dummy(ImVec2(18, ImGui::GetFrameHeight()));
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(status_text.c_str());

  ImGui::SameLine();

  const std::string url = st.rtmp_url.empty() ? "rtmp://127.0.0.1:1935/live/cr" : st.rtmp_url;
  char url_buf[256];
  std::snprintf(url_buf, sizeof(url_buf), "%s", url.c_str());
  ImGui::SetNextItemWidth(280);
  ImGui::InputText("##rtmp_url", url_buf, sizeof(url_buf), ImGuiInputTextFlags_ReadOnly);

  ImGui::SameLine();
  if (animated_button("Copy"))
  {
    ImGui::SetClipboardText(url.c_str());
  }

  ImGui::SameLine();
  static std::string obs_path = find_obs_exe();
  ImGui::BeginDisabled(obs_path.empty());
  if (animated_button("Launch OBS"))
  {
    // Convert UTF-8 → UTF-16 properly so paths with non-ASCII (Steam
    // libraries on a localised drive, user accounts in Cyrillic, …)
    // still resolve. Pass the bin folder as the working directory —
    // OBS resolves its plugin DLLs relative to it.
    int wn = MultiByteToWideChar(CP_UTF8, 0, obs_path.c_str(), (int) obs_path.size(), nullptr, 0);
    std::wstring w(wn > 0 ? wn : 0, 0);
    if (wn > 0)
      MultiByteToWideChar(CP_UTF8, 0, obs_path.c_str(), (int) obs_path.size(), w.data(), wn);
    std::wstring dir = w;
    const size_t slash_fwd = dir.find_last_of(L'/');
    const size_t slash_back = dir.find_last_of(L'\\');
    size_t slash = slash_fwd;
    if (slash == std::wstring::npos || (slash_back != std::wstring::npos && slash_back > slash))
    {
      slash = slash_back;
    }
    if (slash != std::wstring::npos)
      dir.resize(slash);
    const std::wstring open = L"open";
    cr::winapi::ShellExecuteW(nullptr, open.c_str(), w.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
  }
  ImGui::EndDisabled();

  // --- Transport selector ----------------------------------------------
  // Combo right of Launch OBS. Locked while ANY replace mode is active
  // (camera/sound/photo) — switching transport requires teardown of the
  // OBS source and the phone-side cr_feed_proc, both of which already
  // happen on Stop.
  ImGui::SameLine();
  {
    const bool any_active = state().streaming || state().sound_replacing || state().photo_replacing;
    draw_transport_dropdown(any_active);
  }

  // Stream info (resolution / fps) is rendered next to the PREVIEW
  // header — kept out of this panel so OBS-status row stays tight.

  ImGui::EndChild();
}

void draw_preview_panel()
{
  ImGui::BeginChild("preview", ImVec2(0, 0), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  const std::string preview_title = "PREVIEW";
  const ImVec2 header_avail = ImGui::GetContentRegionAvail();
  const float header_x = ImGui::GetCursorPosX();
  const float header_y = ImGui::GetCursorPosY();
  const ImVec2 title_sz = ImGui::CalcTextSize(preview_title.c_str());
  const float title_x = header_x + std::max(0.0f, (header_avail.x - title_sz.x) * 0.5f);
  ImGui::SetCursorPosX(title_x);
  ImGui::TextUnformatted(preview_title.c_str());

  const auto obs = cr::source::FeedController::obs_status();

  // Stream info (resolution / fps) — pulled from the active OBS source
  // status. Rendered as a dim label next to the PREVIEW title so it
  // floats independently of the live image and stays readable when
  // the picture itself is in transition (e.g. waiting for first IDR).
  {
    if (obs.obs_connected && obs.stream_w > 0)
    {
      char info[64]{};
      std::snprintf(info, sizeof(info), "%dx%d @ %d fps", obs.stream_w, obs.stream_h, (int) (obs.stream_fps + 0.5));
      const ImVec2 info_sz = ImGui::CalcTextSize(info);
      const float info_x = header_x + std::max(0.0f, header_avail.x - info_sz.x);
      if (info_x > title_x + title_sz.x + ImGui::GetStyle().ItemSpacing.x)
      {
        ImGui::SameLine();
        ImGui::SetCursorPos(ImVec2(info_x, header_y));
        ImGui::TextDisabled("%s", info);
      }
    }
  }
  ImGui::Separator();

  const ImVec2 avail = ImGui::GetContentRegionAvail();

  // 16:9 frame fit inside the available box.
  float frame_w = avail.x - 8;
  float frame_h = frame_w * 9.0f / 16.0f;
  if (frame_h > avail.y)
  {
    frame_h = avail.y;
    frame_w = frame_h * 16.0f / 9.0f;
  }
  ImVec2 pos = ImGui::GetCursorScreenPos();
  pos.x += (avail.x - frame_w) * 0.5f;

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 bg_col = IM_COL32(10, 10, 10, 255);
  const ImU32 border_col = IM_COL32(60, 60, 60, 255);
  dl->AddRectFilled(pos, ImVec2(pos.x + frame_w, pos.y + frame_h), bg_col, 8.0f);

  // While streaming, blit the most recent decoded BGRA frame from the
  // OBS side-channel decoder. Anything else is a placeholder.
  bool drawn_image = false;
    ID3D11Device* dev = cr::app::d3d_device();

  // Preview is tied to video substitution (camera or photo). In
  // sound-only mode the host preview decoder is bypassed entirely
  // by ObsRtmpSource (no NALU feed, no decode), and we hide the
  // preview UI to match — no point staring at an OBS frame that
  // isn't being substituted onto the phone.
  const bool video_pipeline_up = state().streaming || state().photo_replacing;
  const bool sound_only = state().sound_replacing && !video_pipeline_up;

  if (video_pipeline_up && state().preview_on)
  {
    void* live_srv = cr::render::LivePreview::instance().srv(dev);
    const int live_w = cr::render::LivePreview::instance().width();
    const int live_h = cr::render::LivePreview::instance().height();
    if (live_srv && live_w > 0 && live_h > 0)
    {
      const float tw = (float) live_w;
      const float th = (float) live_h;
      const float src_aspect = tw / th;
      const float dst_aspect = frame_w / frame_h;
      float iw = frame_w, ih = frame_h;
      float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
      const bool fill_preview = false;
      const bool mirror_preview = false;
      if (fill_preview)
      {
        iw = frame_w;
        ih = frame_h;
        if (src_aspect > dst_aspect)
        {
          const float visible_u = dst_aspect / src_aspect;
          const float crop = (1.0f - visible_u) * 0.5f;
          u0 = crop;
          u1 = 1.0f - crop;
        }
        else
        {
          const float visible_v = src_aspect / dst_aspect;
          const float crop = (1.0f - visible_v) * 0.5f;
          v0 = crop;
          v1 = 1.0f - crop;
        }
      }
      else
      {
        if (src_aspect > dst_aspect)
          ih = frame_w / src_aspect;
        else
          iw = frame_h * src_aspect;
      }
      if (mirror_preview)
        std::swap(u0, u1);
      const ImVec2 img_tl(pos.x + (frame_w - iw) * 0.5f, pos.y + (frame_h - ih) * 0.5f);
      const ImVec2 img_br(img_tl.x + iw, img_tl.y + ih);
      dl->AddImageRounded((ImTextureID) live_srv, img_tl, img_br, ImVec2(u0, v0), ImVec2(u1, v1), IM_COL32_WHITE, 8.0f);
      drawn_image = true;
    }
  }

  if (!drawn_image)
  {
    std::string txt;
    if (sound_only)
    {
      txt = "Sound-only replace - preview hidden";
    }
    else if (video_pipeline_up && !state().preview_on)
    {
      txt = "Preview is off";
    }
    else if (video_pipeline_up)
    {
      if (cr::render::H264PreviewDecoder::start_failed())
      {
        txt = "Preview decoder failed - no usable Windows H.264 decoder";
      }
      else if (!obs.obs_connected)
      {
        txt = "Waiting for OBS publisher to connect...";
      }
      else if (obs.video_tags == 0)
      {
        txt = "OBS connected - waiting for video tags...";
      }
      else if (obs.last_video_codec_id >= 0 && obs.last_video_codec_id != 7)
      {
        txt = "OBS video codec is not H.264 - change OBS encoder";
      }
      else if (obs.avc_sequence_headers == 0 && obs.frames == 0)
      {
        txt = "OBS connected - waiting for AVC sequence header...";
      }
      else if (obs.avc_media_packets == 0)
      {
        txt = "OBS H.264 header received - waiting for media frames...";
      }
      else if (obs.avc_keyframes == 0)
      {
        txt = "OBS H.264 media received - waiting for first keyframe...";
      }
      else if (obs.frames == 0)
      {
        txt = "OBS H.264 tags received - waiting for NALUs...";
      }
      else
      {
        txt = "OBS H.264 received - waiting for decoder first frame...";
      }
    }
    else
    {
      txt = "No signal - press Start replace camera / photo / sound";
    }
    ImVec2 ts = ImGui::CalcTextSize(txt.c_str());
    dl->AddText(ImVec2(pos.x + (frame_w - ts.x) * 0.5f, pos.y + (frame_h - ts.y) * 0.5f), IM_COL32(170, 170, 170, 255), txt.c_str());
  }

  // Frame border on top of whatever we drew.
  dl->AddRect(pos, ImVec2(pos.x + frame_w, pos.y + frame_h), border_col, 8.0f, 0, 1.5f);

  // Preview on/off toggle in the top-left corner — only shown when a
  // video pipeline is up. Sound-only mode hides preview entirely.
  if (video_pipeline_up)
  {
    const ImVec2 prev_cursor = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 8, pos.y + 8));
    const bool on = state().preview_on;
    const std::string label = on ? "Hide preview" : "Show preview";
    if (animated_button(label.c_str(), ImVec2(110, 22), on ? ButtonVariant::Live : ButtonVariant::Default))
    {
      state().preview_on = !on;
      cr::render::H264PreviewDecoder::set_enabled(state().preview_on);
      // Drop the last decoded frame so the panel snaps to the
      // "Preview is off" placeholder immediately rather than
      // freezing on the final BGRA upload.
      if (!state().preview_on)
        cr::render::LivePreview::instance().reset();
    }
    ImGui::SetCursorScreenPos(prev_cursor);
  }

  ImGui::EndChild();
}

void open_camera_replace_link() noexcept
{
  cr::winapi::ShellExecuteW(nullptr, L"open", L"https://t.me/CameraReplace", nullptr, nullptr, SW_SHOWNORMAL);
}

void draw_device_corner_link()
{
  UiState& ui = state();
  const char* label = "@CameraReplace";
  const ImVec2 text_sz = ImGui::CalcTextSize(label);
  const ImVec2 item_sz(text_sz.x + 12.0f, text_sz.y + 8.0f);
  const ImVec2 win_pos = ImGui::GetWindowPos();
  const ImVec2 win_size = ImGui::GetWindowSize();
  const float pad = std::max(10.0f, ImGui::GetStyle().WindowPadding.x);
  const ImVec2 screen_pos(
      win_pos.x + std::max(pad, win_size.x - pad - item_sz.x),
      win_pos.y + std::max(pad, win_size.y - pad - item_sz.y));

  ImGui::SetCursorScreenPos(screen_pos);
  ImGui::InvisibleButton("##device_camera_replace_link", item_sz);
  const bool hovered = ImGui::IsItemHovered();
  if (hovered)
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  if (ImGui::IsItemClicked())
    open_camera_replace_link();

  const float hover_target = hovered ? 1.0f : 0.0f;
  ui.motion.device_link_hover = approach(ui.motion.device_link_hover, hover_target, hovered ? 9.0f : 6.0f);
  if (needs_motion_frames(ui.motion.device_link_hover, hover_target, 0.01f))
    ui.motion.any_button_animating = true;
  const float h = ui.motion.device_link_hover;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 text_pos(screen_pos.x + 6.0f, screen_pos.y + 4.0f);
  if (h > 0.01f)
  {
    const ImU32 glow = IM_COL32(255, 255, 255, static_cast<int>(65 * h));
    dl->AddText(ImVec2(text_pos.x - 1, text_pos.y), glow, label);
    dl->AddText(ImVec2(text_pos.x + 1, text_pos.y), glow, label);
    dl->AddText(ImVec2(text_pos.x, text_pos.y - 1), glow, label);
    dl->AddText(ImVec2(text_pos.x, text_pos.y + 1), glow, label);
    const float underline_y = text_pos.y + text_sz.y + 2.0f;
    dl->AddLine(
        ImVec2(text_pos.x, underline_y),
        ImVec2(text_pos.x + text_sz.x, underline_y),
        IM_COL32(255, 255, 255, static_cast<int>(90 * h)),
        1.0f);
  }
  const int base = static_cast<int>(185 + 70 * h);
  dl->AddText(text_pos, IM_COL32(base, base, base, 255), label);
}

void draw_device_panel(float width, float alpha)
{
  UiState& ui = state();
  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * std::max(0.0f, std::min(1.0f, alpha)));
  ImGui::BeginChild("device", ImVec2(width, 0), true);
  ImGui::TextUnformatted("DEVICE");
  ImGui::Separator();

  auto devices = cr::device::DeviceScanner::instance().snapshot();

  // Dropdown of serials when more than one.
  if (devices.size() > 1)
  {
    const cr::device::DeviceInfo* cur = resolve_selected(devices);
    if (ImGui::BeginCombo("##serial", cur ? cur->serial.c_str() : "(none)"))
    {
      for (auto& d : devices)
      {
        bool sel = (cur && cur->serial == d.serial);
        if (ImGui::Selectable(d.serial.c_str(), sel))
        {
          if (ui.selected_serial != d.serial)
          {
            ui.selected_serial = d.serial;
            reset_replace_ui_state(true);
          }
        }
      }
      ImGui::EndCombo();
    }
  }

  const cr::device::DeviceInfo* d = resolve_selected(devices);

  if (!d)
  {
    reset_replace_ui_state(true);
    status_dot("USB device", false);
    status_dot("Authorised", false);
    status_dot("Root access", false);
    status_dot("arm64-v8a", false);
    ImGui::Spacing();
    ImGui::TextDisabled("%s", "Plug in a rooted phone via USB.");
  }
  else
  {
    bool authed = d->auth == cr::device::AuthState::Authorized;
    status_dot("USB device", true);
    status_dot("Authorised", authed, d->auth == cr::device::AuthState::Unauthorized);
    status_dot("Root access", d->rooted, !d->probed);
    status_dot("arm64-v8a", d->arm64, !d->probed);

    ImGui::Spacing();
    ImGui::Text("Serial: %s", d->serial.c_str());
    if (authed && d->probed)
    {
      ImGui::Text("Model:   %s", d->model.c_str());
      ImGui::Text("Device:  %s", d->device.c_str());
      ImGui::Text("Android: %s  (SDK %d)", d->android_ver.c_str(), d->sdk_level);
      ImGui::Text("ABI:     %s", d->abi.c_str());
      if (!d->usb_speed.empty())
      {
        // Tint USB 2.0 amber: 480 Mbps caps raw-NV21 1080p30 at
        // about 720p, so the user knows what to expect when they
        // pick the Raw transport in the OBS panel above.
        const bool is_hs = d->usb_speed.find("USB 2.0") == 0;
        ImU32 col = is_hs ? accents().warn_amber : accents().dim_text;
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::Text("USB:     %s", d->usb_speed.c_str());
        ImGui::PopStyleColor();
      }
      ImU32 se_col = d->selinux_enforcing ? accents().warn_amber : accents().dim_text;
      ImGui::PushStyleColor(ImGuiCol_Text, se_col);
      ImGui::Text("SELinux: %s", d->selinux_mode.c_str());
      ImGui::PopStyleColor();
    }
    else if (d->auth == cr::device::AuthState::Unauthorized)
    {
      ImGui::TextWrapped("%s", "Tap \"Allow USB debugging\" on the phone.");
    }
  }

  ImGui::Spacing();
  {
    bool busy = cr::device::DeviceScanner::instance().reauthorising();
    ImGui::BeginDisabled(busy);
    const std::string label = busy ? "Requesting access..." : "Refresh";
    if (device_menu_button(label.c_str(), ImVec2(-1, 0)))
    {
      // Deep refresh: cycles adb server + pokes unauthorised devices so
      // the RSA "Allow USB debugging" prompt reappears on the phone.
      cr::device::DeviceScanner::instance().refresh_now(true);
    }
    ImGui::EndDisabled();
    if (busy)
    {
      ImGui::TextDisabled("%s", "Check the phone for the RSA prompt.");
    }
  }

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextUnformatted("Software");

  bool can_deploy = d && d->auth == cr::device::AuthState::Authorized && d->rooted && d->arm64;
  ImGui::BeginDisabled(!can_deploy);
  if (device_menu_button("Install software", ImVec2(-1, 0)))
  {
    cr::source::FeedController::install_software(d->serial, [](cr::device::DeployStatus s) { deploy_log_push(std::move(s)); });
  }
  // Inverse of Install — wipe artifacts + drop hooks. Sits directly
  // under Install so the lifecycle pair is visually obvious. Tinted
  // red because it's destructive (kills cameraserver + audioserver
  // briefly; harmless but visible).
  {
    if (device_menu_button("Delete software", ImVec2(-1, 0), ButtonVariant::Destructive))
    {
      cr::source::FeedController::delete_software(d->serial, [](cr::device::DeployStatus s) { deploy_log_push(std::move(s)); });
      // Mirror UI flags — once we wipe the device side, the host
      // session is over too. Drops the live preview image and
      // re-enables the Start replace * buttons.
      reset_replace_ui_state(true);
    }
  }
  {
    const bool software_running = state().software_running;
    const char* label = software_running ? "Stop software" : "Start software";
    const ButtonVariant variant = software_running ? ButtonVariant::Destructive : ButtonVariant::Primary;
    if (device_menu_button(label, ImVec2(-1, 0), variant))
    {
      if (!software_running)
      {
        // Start tailing logcat so we can see the injector's output in UI.
        start_log_tail_if_enabled(d->serial);
        state().software_running = true;
        cr::source::FeedController::start_software(d->serial,
                                                   [](cr::device::DeployStatus s)
                                                   {
                                                     const auto kind = s.kind;
                                                     deploy_log_push(std::move(s));
                                                     if (kind == cr::device::DeployStatus::Kind::Err)
                                                     {
                                                       state().software_running = false;
                                                     }
                                                   });
      }
      else
      {
    cr::source::FeedController::stop_software(d->serial, [](cr::device::DeployStatus s) { deploy_log_push(std::move(s)); });
    // Mirror all replace UI flags — stop_software kills PC sources too.
    reset_replace_ui_state(true);
  }
    }
  }
  ImGui::EndDisabled();

  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();

  // Coexistence rules: camera and sound can run together OR
  // independently; photo is exclusive (cannot coexist with either).
  // Each Start/Stop button manages its own mode flag; ObsRtmpSource
  // is brought up by whichever Start runs first and torn down by
  // whichever Stop leaves no mode active.
  const bool camera_active = state().streaming;
  const bool photo_active = state().photo_replacing;
  const bool sound_active = state().sound_replacing;

  if (!camera_active)
  {
    // Camera disabled only when photo is exclusive-active.
    ImGui::BeginDisabled(!can_deploy || photo_active);
    if (device_menu_button("Start replace camera", ImVec2(-1, 0), ButtonVariant::Primary))
    {
      cr::log::section("Start replace camera", "serial=" + d->serial);
      start_log_tail_if_enabled(d->serial);
      apply_video_start_settings();
      // Optimistic flip (same as sound): Start→Stop immediately so
      // a multi-second arm_video does not look like a dead click.
      state().software_running = true;
      state().streaming = true;
      cr::source::FeedController::start_replace_camera(d->serial,
                                                       [](cr::device::DeployStatus s)
                                                       {
                                                         deploy_log_push(s);
                                                         if (s.kind == cr::device::DeployStatus::Kind::Err)
                                                         {
                                                           state().streaming = false;
                                                         }
                                                       });
    }
    ImGui::EndDisabled();
  }
  else
  {
    if (device_menu_button("Stop replace camera", ImVec2(-1, 0), ButtonVariant::Destructive))
    {
      cr::log::section("Stop replace camera", "serial=" + d->serial);
      cr::source::FeedController::stop_replace_camera(d->serial, [](cr::device::DeployStatus s) { deploy_log_push(std::move(s)); });
      state().streaming = false;
      cr::render::LivePreview::instance().reset();
    }
  }

  // Sound replacement.
  // Coexists with camera. When started solo the host preview decoder
  // is bypassed entirely (ObsRtmpSource::set_video_forward(false))
  // so no OBS frames are decoded for display — handled inside the
  // obs source. Photo is the only blocker.
  if (!sound_active)
  {
    ImGui::BeginDisabled(!can_deploy || photo_active);
    if (device_menu_button("Start replace sound", ImVec2(-1, 0), ButtonVariant::Primary))
    {
      cr::log::section("Start replace sound", "serial=" + d->serial);
      start_log_tail_if_enabled(d->serial);
      state().software_running = true;
      state().sound_replacing = true;
      cr::source::FeedController::start_replace_sound(d->serial,
                                                      [](cr::device::DeployStatus s)
                                                      {
                                                        deploy_log_push(s);
                                                        if (s.kind == cr::device::DeployStatus::Kind::Err)
                                                        {
                                                          state().sound_replacing = false;
                                                        }
                                                      });
    }
    ImGui::EndDisabled();
  }
  else
  {
    if (device_menu_button("Stop replace sound", ImVec2(-1, 0), ButtonVariant::Destructive))
    {
      cr::log::section("Stop replace sound", "serial=" + d->serial);
      cr::source::FeedController::stop_replace_sound(d->serial, [](cr::device::DeployStatus s) { deploy_log_push(std::move(s)); });
      state().sound_replacing = false;
    }
  }

  // ---- Photo (still-capture JPEG, OBS-sourced) --------------------------
  // Coexists with camera + sound — all three share the same OBS RTMP
  // pipeline. When photo mode is on, the camhook intercepts BLOB
  // (still-capture) buffers and fills them with a freshly-encoded
  // JPEG of the latest NV21 frame from OBS. Source is the same shm
  // the camera-replace path uses, so OBS has to be streaming for the
  // saved photo to actually be your OBS content (otherwise the BLOB
  // gets a JPEG of a stale/empty frame).
  if (!photo_active)
  {
    ImGui::BeginDisabled(!can_deploy || camera_active || sound_active);
    if (device_menu_button("Start replace photo", ImVec2(-1, 0), ButtonVariant::Primary))
    {
      cr::log::section("Start replace photo", "serial=" + d->serial);
      start_log_tail_if_enabled(d->serial);
      apply_video_start_settings();
      state().software_running = true;
      state().photo_replacing = true;
      cr::source::FeedController::start_replace_photo(d->serial,
                                                      [](cr::device::DeployStatus s)
                                                      {
                                                        deploy_log_push(s);
                                                        if (s.kind == cr::device::DeployStatus::Kind::Err)
                                                        {
                                                          state().photo_replacing = false;
                                                        }
                                                      });
    }
    ImGui::EndDisabled();
  }
  else
  {
    if (device_menu_button("Stop replace photo", ImVec2(-1, 0), ButtonVariant::Destructive))
    {
      cr::log::section("Stop replace photo", "serial=" + d->serial);
      cr::source::FeedController::stop_replace_photo(d->serial, [](cr::device::DeployStatus s) { deploy_log_push(std::move(s)); });
      state().photo_replacing = false;
    }
  }


  draw_device_corner_link();

  ImGui::EndChild();
  ImGui::PopStyleVar();
}



// Unified log panel — draws every SessionLog entry (host steps, cr::log
// messages, and phone-side cr_* logcat lines) in one scrollable view.
// Sits directly below the preview and exposes local diagnostic messages.
void draw_log_panel()
{
  const bool expanded = state().log_expanded;

  ImGui::BeginChild("unified_log", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollbar);

  // Chevron toggle: points up when collapsed (click to expand upward),
  // down when expanded (click to collapse). Sits to the left of the
  // LOG label so the button position never moves.
  const std::string toggle = expanded ? "v##log_toggle" : "^##log_toggle";
  if (animated_button(toggle.c_str(), ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight())))
  {
    state().log_expanded = !expanded;
  }
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("LOG");
  if (expanded)
  {
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", "  (logs at %%LOCALAPPDATA%%\\CameraReplace)");
  }
  ImGui::SameLine(ImGui::GetContentRegionAvail().x - 50);
  if (animated_button("Clear", ImVec2(54, 0)))
  {
    cr::util::SessionLog::instance().clear();
  }

  if (!expanded)
  {
    ImGui::EndChild();
    return;
  }

  ImGui::Separator();

  ImGui::BeginChild("log_scroll", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
  const auto entries = cr::util::SessionLog::instance().snapshot();
  for (const auto& e : entries)
  {
    ImU32 col = accents().dim_text;
    using K = cr::util::LogKind;
    switch (e.kind)
    {
    case K::Ok:
      col = accents().live_green;
      break;
    case K::Error:
      col = accents().error_red;
      break;
    case K::Warn:
      col = accents().warn_amber;
      break;
    case K::Device:
      col = IM_COL32(170, 170, 210, 255);
      break;
    case K::Info:
      col = IM_COL32(210, 210, 210, 255);
      break;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(e.text.c_str());
    ImGui::PopStyleColor();
  }
  // Auto-scroll when the user is already near the bottom.
  if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4)
    ImGui::SetScrollHereY(1.0f);
  ImGui::EndChild();
  ImGui::EndChild();
}

} // namespace

void draw_main_window()
{
  UiState& ui = state();
  update_main_entrance(ui);
  ui.motion.any_button_animating = false;
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ui.motion.root_alpha = cr::app::App::window_motion_alpha();
  ui.motion.root_scale = cr::app::App::window_motion_scale();
  const float entrance_t = ui.motion.entrance_t;
  const float obs_reveal = panel_reveal(entrance_t, 0.00f, 0.35f);
  const float preview_reveal = panel_reveal(entrance_t, 0.15f, 0.50f);
  const float device_reveal = panel_reveal(entrance_t, 0.34f, 0.45f);
  const float settings_reveal = panel_reveal(entrance_t, 0.55f, 0.45f);
  const float root_scale = std::max(0.90f, std::min(1.0f, ui.motion.root_scale));
  const ImVec2 root_size(vp->WorkSize.x * root_scale, vp->WorkSize.y * root_scale);
  const ImVec2 root_pos(vp->WorkPos.x + (vp->WorkSize.x - root_size.x) * 0.5f, vp->WorkPos.y + (vp->WorkSize.y - root_size.y) * 0.5f);
  ImGui::SetNextWindowPos(root_pos);
  ImGui::SetNextWindowSize(root_size);

  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

  ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * std::max(0.0f, std::min(1.0f, ui.motion.root_alpha)));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::Begin("##camera_replace_root", nullptr, flags);
  ImGui::PopStyleVar(3);

  // Custom title bar replaces the OS chrome we disabled in App::wnd_proc.
  cr::ui::draw_title_bar();

  // Manually inset the body by the style padding (we forced WindowPadding
  // to 0 so the title bar can run edge-to-edge).
  const ImVec2 pad = ImGui::GetStyle().WindowPadding;
  ImGui::SetCursorPos(ImVec2(pad.x, static_cast<float>(cr::app::kTitleBarHeight) + pad.y));

  // The Start/Stop strip moved into the device panel alongside the new
  // Software buttons, so the body fills the rest of the window without
  // a reserved bottom row.
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const ImVec2 body{avail.x - pad.x, avail.y - pad.y};

  ImGui::BeginChild("body", body, false);

  // Source-only layout: OBS runs on PC and hands us a ready H.264 stream,
  // so there's nothing for us to pick on the left anymore. The old
  // source panel is gone; preview/log fill the freed width.
  const float spacing_x = ImGui::GetStyle().ItemSpacing.x;
  const float device_target_w = body.x < 980.0f ? 300.0f : 340.0f;
  ui.motion.device_panel_t = approach(ui.motion.device_panel_t, 1.0f, 4.5f);
  ui.motion.device_panel_w = approach(ui.motion.device_panel_w, device_target_w, 1200.0f);
  const float device_w = std::max(1.0f, ui.motion.device_panel_w * ui.motion.device_panel_t);
  const float center_w = std::max(280.0f, body.x - device_w - spacing_x);

  ImGui::BeginChild("center", ImVec2(center_w, 0), false);
  {
    draw_revealed_panel(obs_reveal, [] { draw_obs_info_panel(); }); // small OBS card at the top

    // Dev build: show the unified log panel under the preview so a
    // maintainer can see the full session.log + phone-side logcat
    // stream live. In production (login enabled) this panel is
    // hidden — the user has no reason to look at the raw log, and
    // they can submit any troubles via "Report a bug" instead.
    const float center_h = ImGui::GetContentRegionAvail().y;
    const float spacing_y = ImGui::GetStyle().ItemSpacing.y;

    // Collapsed log = header row + the BeginChild's own border padding,
    // just enough that the chevron / Clear button sit on a single line.
    const float collapsed_log_h = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2.0f + 4.0f;

    ui.motion.log_t = approach(ui.motion.log_t, state().log_expanded ? 1.0f : 0.0f, 4.8f);
    const float log_h = mix(collapsed_log_h, center_h * 0.45f, ui.motion.log_t);
    const float preview_h = center_h - log_h - spacing_y;

    draw_revealed_panel(preview_reveal, [preview_h] {
      ImGui::BeginChild("preview_area", ImVec2(0, preview_h), false);
      draw_preview_panel();
      ImGui::EndChild();
    });
    draw_revealed_panel(settings_reveal, [log_h] {
      ImGui::BeginChild("log_area", ImVec2(0, log_h), false);
      draw_log_panel();
      ImGui::EndChild();
    });
  }
  ImGui::EndChild();

  if (device_w > 8.0f)
  {
    ImGui::SameLine();
    draw_revealed_panel(device_reveal, [device_w, device_reveal] {
      draw_device_panel(device_w, state().motion.device_panel_t * device_reveal);
    });
  }
  ImGui::EndChild();


  ImGui::End();
  ImGui::PopStyleVar();
}

void shutdown_main_window() noexcept
{
}

bool ui_animation_active() noexcept
{
  const UiState& ui = state();
  if (needs_motion_frames(ui.motion.device_panel_t, 1.0f))
    return true;
  if (needs_motion_frames(ui.motion.device_panel_w, ui.motion.device_panel_w < 320.0f ? 300.0f : 340.0f, 0.75f))
    return true;
  if (needs_motion_frames(ui.motion.log_t, ui.log_expanded ? 1.0f : 0.0f))
    return true;
  if (needs_motion_frames(ui.motion.transport_t, ui.motion.transport_open ? 1.0f : 0.0f))
    return true;
  if (needs_motion_frames(ui.motion.entrance_t, 1.0f))
    return true;
  if (ui.motion.any_button_animating)
    return true;
  return false;
}

} // namespace cr::ui
