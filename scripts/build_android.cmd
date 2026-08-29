@echo off
REM Android public developer build with no backend dependency.
REM Outputs build\android-public\out for the Windows host build.

setlocal EnableExtensions EnableDelayedExpansion
set SCRIPT_DIR=%~dp0
set PROJECT_DIR=%SCRIPT_DIR%..

if not defined ANDROID_BUILD_DIR set "ANDROID_BUILD_DIR=%PROJECT_DIR%\build\android-public"
set BUILD_DIR=%ANDROID_BUILD_DIR%
set OUT_DIR=%BUILD_DIR%\out

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

if not defined NDK_ROOT (
    if defined ANDROID_NDK_ROOT (
        set "NDK_ROOT=%ANDROID_NDK_ROOT%"
    ) else (
        set "NDK_ROOT=%LOCALAPPDATA%\Android\Sdk\ndk\29.0.14033849"
    )
)
set "PATH=%NDK_ROOT%\prebuilt\windows-x86_64\bin;%PATH%"

echo Android compiler: %NDK_ROOT%\toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android21-clang++.cmd

cmake -S "%PROJECT_DIR%\android" -B "%BUILD_DIR%" -G Ninja ^
    -DCMAKE_TOOLCHAIN_FILE="%PROJECT_DIR%\toolchains\android-ndk.cmake" ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DANDROID_NDK_ROOT="%NDK_ROOT%"
if errorlevel 1 goto :err

set ABI_OK=
set PLATFORM_OK=
set NDK_OK=
findstr /I /C:"ANDROID_ABI:STRING=arm64-v8a" "%BUILD_DIR%\CMakeCache.txt" >nul && set ABI_OK=1
findstr /I /C:"ANDROID_PLATFORM:STRING=android-21" "%BUILD_DIR%\CMakeCache.txt" >nul && set PLATFORM_OK=1
findstr /I /C:"ANDROID_NDK_ROOT:" "%BUILD_DIR%\CMakeCache.txt" | findstr /I /C:"29.0.14033849" >nul && set NDK_OK=1
if not defined ABI_OK goto :compiler_error
if not defined PLATFORM_OK goto :compiler_error
if not defined NDK_OK goto :compiler_error

cmake --build "%BUILD_DIR%" --parallel
if errorlevel 1 goto :err

for %%F in (cr_injector cr_feed_proc libcr_hooks.so) do (
    if not exist "%OUT_DIR%\%%F" (
        echo *** Missing %%F in %OUT_DIR%
        exit /b 1
    )
)

echo.
echo ====================================================
echo  Built public Android artifacts: %OUT_DIR%
echo  No backend or launch tickets
echo ====================================================
exit /b 0

:err
echo.
echo *** Android public build failed ***
exit /b 1

:compiler_error
echo *** The configured compiler is not the expected Android NDK compiler. ***
exit /b 1
