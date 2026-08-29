#include "ui/StartupIntro.h"

#include "resources/Resources.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

namespace cr::ui
{

namespace
{

constexpr double kLineIntervalSeconds = 0.045;
constexpr double kHoldSeconds = 0.35;

struct StartupIntroState
{
  bool active = false;
  bool started = false;
  double started_at = 0.0;
  int visible_lines = 0;
  int sounded_lines = 0;
};

StartupIntroState& intro_state()
{
  static StartupIntroState s;
  return s;
}

const std::vector<std::string>& intro_lines()
{
  static const std::vector<std::string> lines = [] {
    std::vector<std::string> out;
    const auto blob = cr::resources::boot_ascii();
    if (!blob.data || blob.size == 0)
      return out;
    const std::string text(reinterpret_cast<const char*>(blob.data), blob.size);
    std::string line;
    for (char c : text)
    {
      if (c == '\r')
        continue;
      if (c == '\n')
      {
        out.push_back(line);
        line.clear();
      }
      else
      {
        line.push_back(c);
      }
    }
    if (!line.empty())
      out.push_back(line);
    return out;
  }();
  return lines;
}

int max_line_chars()
{
  static const int n = [] {
    int out = 0;
    for (const auto& line : intro_lines())
      out = std::max(out, static_cast<int>(line.size()));
    return out;
  }();
  return n;
}

struct WaveSample
{
  WAVEFORMATEX format{};
  const unsigned char* pcm = nullptr;
  DWORD pcm_size = 0;
  bool ready = false;
};

struct SoundQueue
{
  std::mutex mu;
  std::condition_variable cv;
  std::thread worker;
  int pending = 0;
  bool stop = false;
  bool started = false;
};

SoundQueue& sound_queue()
{
  static SoundQueue q;
  return q;
}

bool chunk_is(const unsigned char* p, const char* id) noexcept
{
  return std::memcmp(p, id, 4) == 0;
}

std::uint16_t le16(const unsigned char* p) noexcept
{
  return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::uint32_t le32(const unsigned char* p) noexcept
{
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}

const WaveSample& wave_sample() noexcept
{
  static const WaveSample sample = [] {
    WaveSample out{};
    const auto blob = cr::resources::boot_stdout_wav();
    if (!blob.data || blob.size < 12)
      return out;
    const auto* data = blob.data;
    const std::size_t size = blob.size;
    if (!chunk_is(data, "RIFF") || !chunk_is(data + 8, "WAVE"))
      return out;

    bool have_fmt = false;
    bool have_data = false;
    for (std::size_t off = 12; off + 8 <= size;)
    {
      const unsigned char* chunk = data + off;
      const std::uint32_t chunk_size = le32(chunk + 4);
      const std::size_t payload = off + 8;
      if (payload > size || chunk_size > size - payload)
        break;

      if (chunk_is(chunk, "fmt ") && chunk_size >= 16)
      {
        const unsigned char* fmt = data + payload;
        const std::uint16_t tag = le16(fmt);
        const std::uint16_t channels = le16(fmt + 2);
        const std::uint32_t samples_per_sec = le32(fmt + 4);
        const std::uint32_t avg_bytes_per_sec = le32(fmt + 8);
        const std::uint16_t block_align = le16(fmt + 12);
        const std::uint16_t bits_per_sample = le16(fmt + 14);
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
      else if (chunk_is(chunk, "data") && chunk_size > 0)
      {
        out.pcm = data + payload;
        out.pcm_size = static_cast<DWORD>(chunk_size);
        have_data = true;
      }

      off = payload + chunk_size + (chunk_size & 1u);
    }
    out.ready = have_fmt && have_data && out.pcm && out.pcm_size > 0;
    return out;
  }();
  return sample;
}

bool play_line_sound(HWAVEOUT out, const WaveSample& sample) noexcept
{
  if (!out || !sample.ready)
    return false;

  WAVEHDR header{};
  header.lpData = const_cast<LPSTR>(reinterpret_cast<const char*>(sample.pcm));
  header.dwBufferLength = sample.pcm_size;
  MMRESULT rc = ::waveOutPrepareHeader(out, &header, sizeof(header));
  if (rc != MMSYSERR_NOERROR)
    return false;

  rc = ::waveOutWrite(out, &header, sizeof(header));
  if (rc != MMSYSERR_NOERROR)
  {
    ::waveOutUnprepareHeader(out, &header, sizeof(header));
    return false;
  }

  const DWORD started = ::GetTickCount();
  for (;;)
  {
    rc = ::waveOutUnprepareHeader(out, &header, sizeof(header));
    if (rc == MMSYSERR_NOERROR)
      return true;
    if (rc != WAVERR_STILLPLAYING)
      return false;
    if (::GetTickCount() - started > 1000)
    {
      ::waveOutReset(out);
      ::waveOutUnprepareHeader(out, &header, sizeof(header));
      return false;
    }
    ::Sleep(1);
  }
}

void sound_worker() noexcept
{
  auto& q = sound_queue();
  const auto& sample = wave_sample();
  HWAVEOUT out = nullptr;
  if (sample.ready)
    (void) ::waveOutOpen(&out, WAVE_MAPPER, &sample.format, 0, 0, CALLBACK_NULL);

  for (;;)
  {
    int count = 0;
    {
      std::unique_lock<std::mutex> lk(q.mu);
      q.cv.wait(lk, [&q] { return q.stop || q.pending > 0; });
      if (q.stop && q.pending <= 0)
        break;
      count = q.pending;
      q.pending = 0;
    }

    if (!out && sample.ready)
      (void) ::waveOutOpen(&out, WAVE_MAPPER, &sample.format, 0, 0, CALLBACK_NULL);
    for (int i = 0; i < count; ++i)
      (void) play_line_sound(out, sample);
  }

  if (out)
  {
    ::waveOutReset(out);
    ::waveOutClose(out);
  }
}

void start_sound_worker() noexcept
{
  auto& q = sound_queue();
  std::lock_guard<std::mutex> lk(q.mu);
  if (q.started)
    return;
  q.stop = false;
  q.pending = 0;
  try
  {
    q.worker = std::thread(sound_worker);
    q.started = true;
  }
  catch (...)
  {
    q.started = false;
  }
}

void enqueue_line_sound() noexcept
{
  start_sound_worker();
  auto& q = sound_queue();
  {
    std::lock_guard<std::mutex> lk(q.mu);
    if (!q.started || q.stop)
      return;
    q.pending = std::min(q.pending + 1, 64);
  }
  q.cv.notify_one();
}

} // namespace

void bootstrap_startup_intro() noexcept
{
#if defined(CR_STARTUP_ASCII_INTRO) && CR_STARTUP_ASCII_INTRO
  auto& s = intro_state();
  s.active = !intro_lines().empty();
  s.started = false;
  s.started_at = 0.0;
  s.visible_lines = 0;
  s.sounded_lines = 0;
  if (s.active)
    start_sound_worker();
#endif
}

bool startup_intro_active() noexcept
{
#if defined(CR_STARTUP_ASCII_INTRO) && CR_STARTUP_ASCII_INTRO
  return intro_state().active;
#else
  return false;
#endif
}

void draw_startup_intro()
{
#if defined(CR_STARTUP_ASCII_INTRO) && CR_STARTUP_ASCII_INTRO
  auto& s = intro_state();
  const auto& lines = intro_lines();
  if (!s.active || lines.empty())
    return;

  const double now = ImGui::GetTime();
  if (!s.started)
  {
    s.started = true;
    s.started_at = now;
  }

  const double elapsed = now - s.started_at;
  s.visible_lines = std::min(static_cast<int>(lines.size()), static_cast<int>(elapsed / kLineIntervalSeconds) + 1);
  while (s.sounded_lines < s.visible_lines)
  {
    ++s.sounded_lines;
    enqueue_line_sound();
  }

  const double done_at = static_cast<double>(lines.size()) * kLineIntervalSeconds + kHoldSeconds;
  if (s.visible_lines >= static_cast<int>(lines.size()) && elapsed >= done_at)
  {
    s.active = false;
    return;
  }

  // Keep the standalone startup intro aligned with the main UI.
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::Begin("startup_ascii_intro",
               nullptr,
               ImGuiWindowFlags_NoDecoration |
                   ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus |
                   ImGuiWindowFlags_NoNav |
                   ImGuiWindowFlags_NoScrollbar);

  const int max_chars = max_line_chars();
  if (s.visible_lines > 0 && max_chars > 0)
  {
    const std::string probe(static_cast<std::size_t>(max_chars), '0');
    const float line_h = ImGui::GetTextLineHeight();
    const ImVec2 block_size(
        ImGui::CalcTextSize(probe.c_str()).x,
        static_cast<float>(lines.size()) * line_h);
    const float left = std::max(0.0f, (vp->WorkSize.x - block_size.x) * 0.5f);
    const float top = std::max(0.0f, (vp->WorkSize.y - block_size.y) * 0.5f);

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    for (int i = 0; i < s.visible_lines; ++i)
    {
      ImGui::SetCursorPos(ImVec2(left, top + static_cast<float>(i) * line_h));
      ImGui::TextUnformatted(lines[static_cast<std::size_t>(i)].c_str());
    }
    ImGui::PopStyleColor();
  }

  ImGui::End();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
#endif
}

void shutdown_startup_intro() noexcept
{
  auto& q = sound_queue();
  {
    std::lock_guard<std::mutex> lk(q.mu);
    if (!q.started)
      return;
    q.stop = true;
    q.pending = 0;
  }
  q.cv.notify_all();
  if (q.worker.joinable())
    q.worker.join();
  {
    std::lock_guard<std::mutex> lk(q.mu);
    q.started = false;
    q.stop = false;
    q.pending = 0;
  }
}

} // namespace cr::ui
