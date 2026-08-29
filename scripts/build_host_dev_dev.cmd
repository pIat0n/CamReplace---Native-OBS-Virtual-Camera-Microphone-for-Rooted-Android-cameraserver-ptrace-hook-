@echo off
REM Public local build with no backend/server binding.
REM Embeds Android public artifacts from build\android-public\out.

setlocal EnableExtensions EnableDelayedExpansion
set SCRIPT_DIR=%~dp0
set PROJECT_DIR=%SCRIPT_DIR%..
if not defined BUILD_DIR set "BUILD_DIR=%PROJECT_DIR%\build\host-dev-dev"

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if not defined LLVM_MINGW_ROOT set "LLVM_MINGW_ROOT=C:\llvm-mingw-20240606-msvcrt-x86_64"
set "PATH=%LLVM_MINGW_ROOT%\bin;%PATH%"

if not defined ANDROID_OUT_DIR (
    if defined ANDROID_BUILD_DIR (
        set "ANDROID_OUT_DIR=%ANDROID_BUILD_DIR%\out"
    ) else (
        set "ANDROID_OUT_DIR=%PROJECT_DIR%\build\android-public\out"
    )
)
echo Refreshing public Android artifacts: !ANDROID_OUT_DIR!
call "%SCRIPT_DIR%build_android.cmd"
if errorlevel 1 exit /b 1

for %%F in (cr_injector cr_feed_proc libcr_hooks.so) do (
    if not exist "!ANDROID_OUT_DIR!\%%F" (
        echo *** Missing public Android artifact %%F in !ANDROID_OUT_DIR!
        exit /b 1
    )
)

echo Host compiler: %LLVM_MINGW_ROOT%\bin\x86_64-w64-mingw32-clang++.exe
echo Backend/server: OFF
echo Android outs: !ANDROID_OUT_DIR!

cmake -S "%PROJECT_DIR%\host" -B "%BUILD_DIR%" -G Ninja ^
    -DCMAKE_TOOLCHAIN_FILE="%PROJECT_DIR%\toolchains\llvm-mingw.cmake" ^
    -DLLVM_MINGW_ROOT="%LLVM_MINGW_ROOT%" ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DANDROID_OUT_DIR="!ANDROID_OUT_DIR!"
set RC=%ERRORLEVEL%
if not "%RC%"=="0" goto :err

set COMPILER_OK=
findstr /I /C:"x86_64-w64-mingw32-clang++" "%BUILD_DIR%\CMakeCache.txt" >nul && set COMPILER_OK=1
if not defined COMPILER_OK (
    for /f "delims=" %%F in ('dir /b /s "%BUILD_DIR%\CMakeFiles\CMakeCXXCompiler.cmake" 2^>nul') do (
        findstr /I /C:"x86_64-w64-mingw32-clang++" "%%F" >nul && set COMPILER_OK=1
    )
)
if not defined COMPILER_OK goto :compiler_error

cmake --build "%BUILD_DIR%" --target CameraReplace --parallel
set RC=%ERRORLEVEL%
if not "%RC%"=="0" goto :err

echo.
echo ====================================================
echo  Built host-dev-dev public edition: %BUILD_DIR%\bin\CameraReplace.exe
echo  Public no-server build
echo  Embedded public Android outs: !ANDROID_OUT_DIR!
echo ====================================================
exit /b 0

:err
echo.
echo *** Host-dev-dev public build failed ***
exit /b 1

:compiler_error
echo *** The configured compiler is not the expected stock LLVM-MinGW compiler. ***
exit /b 1
