#include "resources/EmbeddedFs.h"
#include "resources/Resources.h"
#include "util/Log.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h> // SHGetKnownFolderPath
#include "util/WinApiDyn.h"

namespace fs = std::filesystem;

namespace cr::resources
{

namespace
{

constexpr uint64_t kSplitMixIncrement = 0x9E3779B97F4A7C15ull;

uint64_t splitmix64_next(uint64_t& state) noexcept
{
  state += kSplitMixIncrement;
  uint64_t z = state;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

void secure_zero(std::vector<unsigned char>& bytes) noexcept
{
  volatile unsigned char* p = bytes.data();
  for (size_t i = 0; i < bytes.size(); ++i)
  {
    p[i] = 0;
  }
}

struct SecureBytes
{
  std::vector<unsigned char> v;

  SecureBytes() = default;
  explicit SecureBytes(size_t n) : v(n) {}
  SecureBytes(const SecureBytes&) = delete;
  SecureBytes& operator=(const SecureBytes&) = delete;
  SecureBytes(SecureBytes&&) noexcept = default;
  SecureBytes& operator=(SecureBytes&&) noexcept = default;

  ~SecureBytes() noexcept
  {
    secure_zero(v);
  }
};

void xor_splitmix64_stream(unsigned char* data, size_t size, uint64_t seed) noexcept
{
  uint64_t state = seed;
  uint64_t word = 0;
  for (size_t i = 0; i < size; ++i)
  {
    if ((i & 7u) == 0)
    {
      word = splitmix64_next(state);
    }
    data[i] ^= static_cast<unsigned char>((word >> ((i & 7u) * 8u)) & 0xffu);
  }
}

SecureBytes decode_blob(const Blob& b)
{
  if (b.encoded_size < b.size)
  {
    throw std::runtime_error("embedded blob truncated");
  }
  if (b.encrypted && b.seed == 0)
  {
    throw std::runtime_error("embedded blob key missing");
  }

  SecureBytes out(b.size);
  if (b.size != 0)
  {
    std::memcpy(out.v.data(), b.data, b.size);
    if (b.encrypted)
    {
      xor_splitmix64_stream(out.v.data(), out.v.size(), static_cast<uint64_t>(b.seed));
    }
  }
  return out;
}

fs::path local_appdata()
{
  PWSTR wp = nullptr;
  if (cr::winapi::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &wp) != S_OK)
  {
    // Fallback to %USERPROFILE%\AppData\Local — should never hit.
    const char* up = std::getenv("USERPROFILE");
    if (!up)
      throw std::runtime_error("can't locate LocalAppData");
    return fs::path(up) / "AppData" / "Local";
  }
  fs::path p(wp);
  CoTaskMemFree(wp);
  return p;
}

fs::path bin_dir_cached;

const fs::path& bin_dir()
{
  if (bin_dir_cached.empty())
  {
    bin_dir_cached = local_appdata() / "CameraReplace" / "bin";
  }
  return bin_dir_cached;
}

// Write blob to path. Skip the write only when the on-disk file matches the
// embedded blob byte-for-byte — anything cheaper (size + N-byte sniff) has
// burned us when only the middle of a binary changed between builds.
// Reading 200 KB – 1 MB from %LOCALAPPDATA% on startup is cheap; the
// assurance that the extracted file actually matches what's baked into the
// running .exe is worth far more.
void write_blob_if_changed(const Blob& b, const fs::path& dest)
{
  if (b.size == 0)
  {
    cr::log::warn("embed", std::string("embedded blob empty: ") + dest.string());
    return;
  }

  SecureBytes expected = decode_blob(b);

  if (fs::exists(dest))
  {
    std::error_code ec;
    auto have = fs::file_size(dest, ec);
    if (!ec && have == b.size)
    {
      std::ifstream in(dest, std::ios::binary);
      SecureBytes existing(b.size);
      in.read(reinterpret_cast<char*>(existing.v.data()), static_cast<std::streamsize>(b.size));
      if (in && (size_t) in.gcount() == b.size && std::memcmp(existing.v.data(), expected.v.data(), b.size) == 0)
      {
        return; // already up-to-date
      }
    }
  }

  // Write to a temp sibling and rename, so a crashed extract doesn't leave
  // a half-written exe in place.
  fs::path tmp = dest;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out)
      throw std::runtime_error("cannot open " + tmp.string());
    out.write(reinterpret_cast<const char*>(expected.v.data()), static_cast<std::streamsize>(b.size));
    if (!out)
      throw std::runtime_error("write failed: " + tmp.string());
  }
  std::error_code ec;
  fs::rename(tmp, dest, ec);
  if (ec)
  {
    // Rename can fail if the destination is locked (adb running). Retry
    // as copy + delete.
    fs::remove(dest, ec);
    fs::rename(tmp, dest, ec);
    if (ec)
      throw std::runtime_error("rename failed: " + dest.string());
  }
  cr::log::info("embed", std::string("extracted ") + dest.filename().string() + " (" + std::to_string(b.size) + " bytes)");
}

void remove_file_best_effort(const fs::path& path) noexcept
{
  std::error_code ec;
  fs::remove(path, ec);
  if (ec)
  {
    cr::log::warn("embed", "cleanup failed for " + path.string() + ": " + ec.message());
  }
}

} // namespace

// The output filenames are explicit so resource extraction remains
// independent from generated embed metadata.
fs::path extract_adb_suite()
{
  fs::create_directories(bin_dir());

  // ADB suite must be extracted together — adb.exe will fail to start if
  // AdbWinApi.dll is missing alongside it.
  write_blob_if_changed(adb_exe(), bin_dir() / "adb.exe");
  write_blob_if_changed(adb_winapi_dll(), bin_dir() / "AdbWinApi.dll");
  write_blob_if_changed(adb_winusbapi_dll(), bin_dir() / "AdbWinUsbApi.dll");
  write_blob_if_changed(libwinpthread_dll(), bin_dir() / "libwinpthread-1.dll");

  return bin_dir();
}

fs::path extract_android_artifacts()
{
  fs::create_directories(bin_dir());

  // Android artifacts (pushed to /data/local/tmp on phone).
  write_blob_if_changed(cr_injector(), bin_dir() / "cr_injector");
  write_blob_if_changed(cr_feed_proc(), bin_dir() / "cr_feed_proc");
  write_blob_if_changed(libcr_hooks(), bin_dir() / "libcr_hooks.so");

  return bin_dir();
}

fs::path extract_all()
{
  extract_adb_suite();
  extract_android_artifacts();
  return bin_dir();
}

void cleanup_android_artifacts() noexcept
{
  try
  {
    remove_file_best_effort(bin_dir() / "cr_injector");
    remove_file_best_effort(bin_dir() / "cr_feed_proc");
    remove_file_best_effort(bin_dir() / "libcr_hooks.so");
  }
  catch (const std::exception& e)
  {
    cr::log::warn("embed", std::string("android artifact cleanup failed: ") + e.what());
  }
  catch (...)
  {
    cr::log::warn("embed", "android artifact cleanup failed");
  }
}

void cleanup_bin_dir() noexcept
{
  try
  {
    cleanup_android_artifacts();
    remove_file_best_effort(bin_dir() / "adb.exe");
    remove_file_best_effort(bin_dir() / "AdbWinApi.dll");
    remove_file_best_effort(bin_dir() / "AdbWinUsbApi.dll");
    remove_file_best_effort(bin_dir() / "libwinpthread-1.dll");

    std::error_code ec;
    fs::remove(bin_dir(), ec);
    if (ec)
    {
      cr::log::warn("embed", "cleanup failed for " + bin_dir().string() + ": " + ec.message());
    }
  }
  catch (const std::exception& e)
  {
    cr::log::warn("embed", std::string("bin cleanup failed: ") + e.what());
  }
  catch (...)
  {
    cr::log::warn("embed", "bin cleanup failed");
  }
}

fs::path adb_exe_path()
{
  return bin_dir() / "adb.exe";
}
fs::path cr_injector_path()
{
  return bin_dir() / "cr_injector";
}
fs::path cr_feed_proc_path()
{
  return bin_dir() / "cr_feed_proc";
}
fs::path libcr_hooks_path()
{
  return bin_dir() / "libcr_hooks.so";
}
} // namespace cr::resources
