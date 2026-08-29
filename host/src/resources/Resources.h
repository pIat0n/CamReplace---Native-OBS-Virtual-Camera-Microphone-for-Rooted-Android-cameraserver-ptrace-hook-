#pragma once

// Embedded-resource registry.
// All binary payloads the Windows app needs (adb.exe, cr_injector, .so files,
// fonts…) are baked into the exe via cr_embed_resource() at build time.
// Consumers get them through the accessors here — no filesystem touching
// required until we decide to extract them in EmbeddedFs.

#include <cstddef>

namespace cr::resources
{

struct Blob
{
  const unsigned char* data; // encoded bytes; never null
  std::size_t size;          // plaintext size; 0 if source absent
  std::size_t encoded_size;  // bytes available at data
  unsigned long long seed;   // splitmix64 stream seed
  bool encrypted;            // true for normal embedded payloads
  const char* out_name;      // suggested filename when extracted
};

// Defined by generated .cpp files from cr_embed_resource().
// ADB suite — all four are required for USB device access on Windows;
// AdbWinApi and AdbWinUsbApi are loaded by adb.exe at startup, and adb
// forkserver uses libwinpthread.
Blob adb_exe() noexcept;
Blob adb_winapi_dll() noexcept;
Blob adb_winusbapi_dll() noexcept;
Blob libwinpthread_dll() noexcept;

// Android-side artifacts we push to the phone.
Blob cr_injector() noexcept;
Blob cr_feed_proc() noexcept;
Blob libcr_hooks() noexcept;

// First-launch login intro.
Blob boot_ascii() noexcept;
Blob boot_stdout_wav() noexcept;

// Title bar brand controls.
Blob titlebar_brand_png() noexcept;
Blob titlebar_brand_click_mp3() noexcept;

// Device menu click sounds.
Blob device_granted_wav() noexcept;
Blob device_error_wav() noexcept;

} // namespace cr::resources
