@echo off
setlocal EnableExtensions
cd /d "%~dp0"

where cmake >nul 2>nul || goto :missing_cmake
where ninja >nul 2>nul || goto :missing_ninja

if not defined ANDROID_SDK_ROOT set "ANDROID_SDK_ROOT=%LOCALAPPDATA%\Android\Sdk"
if not exist "%ANDROID_SDK_ROOT%\platform-tools\adb.exe" goto :missing_adb
set "ADB_SRC_DIR=%ANDROID_SDK_ROOT%\platform-tools"

if not defined ANDROID_NDK_ROOT set "ANDROID_NDK_ROOT=%ANDROID_SDK_ROOT%\ndk\29.0.14033849"
if not defined NDK_ROOT set "NDK_ROOT=%ANDROID_NDK_ROOT%"
if not exist "%NDK_ROOT%\build\cmake\android.toolchain.cmake" goto :missing_ndk

if not defined LLVM_MINGW_ROOT set "LLVM_MINGW_ROOT=C:\llvm-mingw-20240606-msvcrt-x86_64"
if not exist "%LLVM_MINGW_ROOT%\bin\x86_64-w64-mingw32-clang++.exe" goto :missing_llvm

echo Building CameraReplace public development edition...
call "%~dp0scripts\build_host_dev_dev.cmd"
if errorlevel 1 goto :build_failed

echo.
echo Build complete:
if defined BUILD_DIR (
    echo   %BUILD_DIR%\bin\CameraReplace.exe
) else (
    echo   %~dp0build\host-dev-dev\bin\CameraReplace.exe
)
exit /b 0

:missing_cmake
echo ERROR: CMake was not found in PATH.
echo Install CMake 3.24 or newer and reopen the terminal.
exit /b 1

:missing_ninja
echo ERROR: Ninja was not found in PATH.
echo Install Ninja and reopen the terminal.
exit /b 1

:missing_adb
echo ERROR: Android SDK Platform Tools were not found at:
echo   %ANDROID_SDK_ROOT%\platform-tools
echo Set ANDROID_SDK_ROOT to your Android SDK directory.
exit /b 1

:missing_ndk
echo ERROR: Android NDK r29 was not found at:
echo   %NDK_ROOT%
echo Install NDK 29.0.14033849 or set ANDROID_NDK_ROOT.
exit /b 1

:missing_llvm
echo ERROR: LLVM-MinGW was not found at:
echo   %LLVM_MINGW_ROOT%
echo Set LLVM_MINGW_ROOT to an LLVM-MinGW x86_64 MSVCRT distribution.
exit /b 1

:build_failed
echo.
echo ERROR: CameraReplace build failed.
exit /b 1
