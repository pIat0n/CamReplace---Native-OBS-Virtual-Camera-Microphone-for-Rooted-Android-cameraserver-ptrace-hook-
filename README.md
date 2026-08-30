# CameraReplace

**English** | [Русский](README_RU.md)

<img width="1284" height="762" alt="image" src="https://github.com/user-attachments/assets/bc1f329d-c67a-43de-8c9b-c9b75c097fff" />


CameraReplace is a Windows host application and a set of native Android components for feeding OBS video and audio into a rooted Android test device over USB. The Windows application receives an RTMP stream, forwards media through private ADB tunnels, and controls the phone-side feed and hook lifecycle.

This repository contains the public development edition. It has no licensing backend, Telegram authentication, bug-report uploader, production credentials, VMProtect project, launch-ticket gate, or release build pipeline. The encrypted host-to-device media channel is negotiated locally and does not require a server.

## Download

Prebuilt Windows executables are published on the [GitHub Releases page](https://github.com/pIat0n/CamReplace---Native-OBS-Virtual-Camera-Microphone-for-Rooted-Android-cameraserver-ptrace-hook-/releases). The source code remains fully buildable with `build.bat` as described below.

## Features

- OBS/RTMP H.264 and AAC ingestion on Windows.
- Compressed H.264 and raw NV21 video transports.
- PCM microphone feed.
- Embedded ADB and Android arm64 payloads in one Windows executable.
- Camera, microphone, and still-image replacement controls.
- Live preview, device discovery, USB-speed diagnostics, and session logs.

## Requirements

- Windows 10 or Windows 11 x64.
- CMake 3.24 or newer and Ninja available in `PATH`.
- Android SDK Platform Tools.
- Android NDK `29.0.14033849`.
- LLVM-MinGW x86_64 MSVCRT toolchain.
- A rooted arm64-v8a Android test device running API 21 or newer.
- OBS Studio for the media source.

The build script automatically checks the common SDK locations. For custom installations, set these environment variables before running it:

```bat
set ANDROID_SDK_ROOT=D:\Android\Sdk
set ANDROID_NDK_ROOT=D:\Android\Sdk\ndk\29.0.14033849
set LLVM_MINGW_ROOT=D:\Toolchains\llvm-mingw
```

## Build

Clone the repository and run the single root script:

```bat
build.bat
```

The script verifies the required tools, builds the Android arm64 components, embeds them together with ADB into the Windows host, and writes the final executable to:

```text
build\host-dev-dev\bin\CameraReplace.exe
```

No backend, private key, environment file, VMProtect installation, or prebuilt project artifact is required.

## Project layout

- `host/` — Windows GUI, RTMP ingest, preview, transports, and ADB orchestration.
- `android/` — injector, feed process, camera hook, and audio hook.
- `common/` — local secure-channel protocol and bundled cryptography.
- `imgui/` — Dear ImGui sources used by the Windows UI.
- `third_party/` — source dependencies required by the build.
- `scripts/` and `toolchains/` — implementation used by `build.bat`.

## Support and updates

Follow the [CameraReplace Telegram channel](https://t.me/CameraReplaceService) for project news, support, and future compatibility updates for different smartphone models.

You can support continued development with USDT. Send funds only over the network shown for each address:

- **USDT (TON):** `UQA2RCTTi6fyQE8LLgGR7NcBhY4GkDAIVnF6O0c4jGlABWjs`
- **USDT (TRC20):** `TU3TeGPfMJDwqwd3RefFVft1Xn9JNyTiUL`
- **USDT (SPL):** `HxQHjW64McYiJMjUZPtisb9uEijWRk2G9q3Rogv1cHGg`
- **USDT (ERC20):** `0xC168ACfD9cae5ac2416b61a55BB0A8c82C48a79A`
- **USDT (BEP20):** `0xC168ACfD9cae5ac2416b61a55BB0A8c82C48a79A`

## Responsible use

Use CameraReplace only on devices you own or are explicitly authorized to test. Do not use it to misrepresent camera or microphone input to people, identity checks, financial applications, access-control systems, or other services.

## License

A project license has not yet been selected. Third-party components retain their original licenses; see `THIRD_PARTY_NOTICES.txt` and the license files stored beside those components.
