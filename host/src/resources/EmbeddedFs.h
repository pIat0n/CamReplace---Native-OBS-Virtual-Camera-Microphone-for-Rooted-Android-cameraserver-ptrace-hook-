#pragma once

// EmbeddedFs — on-demand extraction of embedded resources to a private
// directory so external processes (adb, the injector) can spawn from there.
// Layout:
//   %LOCALAPPDATA%\CameraReplace\bin\adb.exe
//   %LOCALAPPDATA%\CameraReplace\bin\cr_injector
//   %LOCALAPPDATA%\CameraReplace\bin\libcr_hooks.so
// Extraction is a no-op if the on-disk file already matches the embedded
// size — cheap to call every launch.

#include <filesystem>
#include <string>

namespace cr::resources
{

// Extract everything that isn't already on disk with the right size. Returns
// the directory that holds the extracted files. Throws std::runtime_error on
// failure (disk full, permission denied, …).
std::filesystem::path extract_all();

// Smaller lifecycle-oriented extraction steps. The ADB suite is enough to
// scan/register devices; Android artifacts are only needed while pushing
// software to a phone.
std::filesystem::path extract_adb_suite();
std::filesystem::path extract_android_artifacts();

// Best-effort local cleanup. `cleanup_android_artifacts()` leaves adb.exe and
// its DLLs for active scanner/adb use; `cleanup_bin_dir()` removes the whole
// private extraction directory after callers have stopped adb.
void cleanup_android_artifacts() noexcept;
void cleanup_bin_dir() noexcept;

// Path to adb.exe (after extract_all has run at least once).
std::filesystem::path adb_exe_path();

// Paths to Android artifacts ready for `adb push`.
std::filesystem::path cr_injector_path();
std::filesystem::path cr_feed_proc_path();
std::filesystem::path libcr_hooks_path();

} // namespace cr::resources
