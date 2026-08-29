# Camera Replace — Windows host application.
#
# Single statically-linked .exe. Everything that needs to land on the phone
# (adb.exe + .so binaries + the cr_* native helpers) is embedded as a
# binary resource and extracted into %LOCALAPPDATA%\CameraReplace on first
# run.

cmake_minimum_required(VERSION 3.24)
project(camera_replace_host CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(Embed)

# --- warnings / build hygiene ------------------------------------------------
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter       # ImGui backends trigger these
    -Wno-missing-field-initializers
)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# --- ImGui (sources live in ../imgui) ----------------------------------------
set(IMGUI_DIR "${CMAKE_SOURCE_DIR}/../imgui")

add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)
target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)
target_compile_definitions(imgui PUBLIC
    IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
    IMGUI_DISABLE_DEBUG_TOOLS
)

add_library(cr_secure_channel STATIC
    ${CMAKE_SOURCE_DIR}/../common/secure_channel/SecureSession.cpp
    ${CMAKE_SOURCE_DIR}/../common/secure_channel/vendor/monocypher/monocypher.c
)
target_include_directories(cr_secure_channel PUBLIC
    ${CMAKE_SOURCE_DIR}/../common/secure_channel
    ${CMAKE_SOURCE_DIR}/../common
)
# --- main executable ---------------------------------------------------------
add_executable(CameraReplace WIN32
    src/main.cpp
    src/app/App.cpp
    src/device/AdbClient.cpp
    src/device/DeviceScanner.cpp
    src/device/DeviceStack.cpp
    src/device/LogTail.cpp
    src/device/PhoneOps.cpp
    src/render/D3D11Host.cpp
    src/render/H264PreviewDecoder.cpp
    src/render/LivePreview.cpp
    src/resources/EmbeddedFs.cpp
    src/source/AudioPump.cpp
    src/source/FeedController.cpp
    src/source/HostStack.cpp
    src/source/ObsRtmpSource.cpp
    src/source/RtmpServer.cpp
    src/transport/TcpFeedClient.cpp
    src/ui/MainWindow.cpp
    src/ui/StartupIntro.cpp
    src/ui/Theme.cpp
    src/ui/TitleBar.cpp
    src/util/Log.cpp
    src/util/SessionLog.cpp
    src/util/WinApiDyn.cpp
    # Embeds CameraReplace.manifest (requireAdministrator + DPI-aware) so
    # Windows shows the UAC prompt at process create. Refusing the prompt
    # aborts the launch — there is no unprivileged code path.
    resources/CameraReplace.rc
)

target_include_directories(CameraReplace PRIVATE
    src
    "${CMAKE_SOURCE_DIR}/../common"
    "${CMAKE_SOURCE_DIR}/../third_party/stb")

target_link_libraries(CameraReplace PRIVATE
    imgui
    cr_secure_channel
    d3d11 dxgi d3dcompiler   # DX11 runtime
    mf mfplat mfreadwrite mfuuid   # Media Foundation: H.264 preview decoder
    ws2_32                          # Winsock: TcpFeedClient + RtmpServer
    user32 gdi32 ole32 uuid winmm
    imm32 dwmapi
)

# Where the built exe lands.
set_target_properties(CameraReplace PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Strip symbols + subsystem:Windows (no console).
target_link_options(CameraReplace PRIVATE
    $<$<CONFIG:Release>:-Wl,-s>
    -mwindows
)

target_compile_definitions(CameraReplace PRIVATE
    CR_STARTUP_ASCII_INTRO=1)

# --- embedded resources ------------------------------------------------------
# Paths are resolved lazily — missing sources still configure (with
# warnings) and generate empty placeholders. Rebuild the android tree
# first to fill them in.

if(NOT DEFINED ADB_SRC_DIR OR "${ADB_SRC_DIR}" STREQUAL "")
    if(NOT "$ENV{ADB_SRC_DIR}" STREQUAL "")
        set(ADB_SRC_DIR "$ENV{ADB_SRC_DIR}")
    elseif(NOT "$ENV{ANDROID_SDK_ROOT}" STREQUAL "")
        set(ADB_SRC_DIR "$ENV{ANDROID_SDK_ROOT}/platform-tools")
    else()
        set(ADB_SRC_DIR "$ENV{LOCALAPPDATA}/Android/Sdk/platform-tools")
    endif()
endif()
if(NOT DEFINED ANDROID_OUT_DIR)
    set(ANDROID_OUT_DIR "${CMAKE_SOURCE_DIR}/../build/android/out")
endif()

cr_embed_resource(TARGET CameraReplace NAME adb_exe
    FILE "${ADB_SRC_DIR}/adb.exe"               OUT_NAME "adb.exe")
cr_embed_resource(TARGET CameraReplace NAME adb_winapi_dll
    FILE "${ADB_SRC_DIR}/AdbWinApi.dll"         OUT_NAME "AdbWinApi.dll")
cr_embed_resource(TARGET CameraReplace NAME adb_winusbapi_dll
    FILE "${ADB_SRC_DIR}/AdbWinUsbApi.dll"      OUT_NAME "AdbWinUsbApi.dll")
cr_embed_resource(TARGET CameraReplace NAME libwinpthread_dll
    FILE "${ADB_SRC_DIR}/libwinpthread-1.dll"   OUT_NAME "libwinpthread-1.dll")
cr_embed_resource(TARGET CameraReplace NAME cr_injector
    FILE "${ANDROID_OUT_DIR}/cr_injector"       OUT_NAME "cr_injector")
cr_embed_resource(TARGET CameraReplace NAME cr_feed_proc
    FILE "${ANDROID_OUT_DIR}/cr_feed_proc"      OUT_NAME "cr_feed_proc")
cr_embed_resource(TARGET CameraReplace NAME libcr_hooks
    FILE "${ANDROID_OUT_DIR}/libcr_hooks.so"    OUT_NAME "libcr_hooks.so")
cr_embed_resource(TARGET CameraReplace NAME boot_ascii
    FILE "${CMAKE_SOURCE_DIR}/assets/intro/ascii.txt" OUT_NAME "ascii.txt")
cr_embed_resource(TARGET CameraReplace NAME boot_stdout_wav
    FILE "${CMAKE_SOURCE_DIR}/assets/intro/stdoutown.wav" OUT_NAME "stdoutown.wav")
cr_embed_resource(TARGET CameraReplace NAME titlebar_brand_png
    FILE "${CMAKE_SOURCE_DIR}/assets/titlebar/brand_icon.png" OUT_NAME "brand_icon.png")
cr_embed_resource(TARGET CameraReplace NAME titlebar_brand_click_mp3
    FILE "${CMAKE_SOURCE_DIR}/assets/titlebar/brand_click.mp3" OUT_NAME "brand_click.mp3")
cr_embed_resource(TARGET CameraReplace NAME device_granted_wav
    FILE "${CMAKE_SOURCE_DIR}/assets/device/granted.wav" OUT_NAME "granted.wav")
cr_embed_resource(TARGET CameraReplace NAME device_error_wav
    FILE "${CMAKE_SOURCE_DIR}/assets/device/error.wav" OUT_NAME "error.wav")
