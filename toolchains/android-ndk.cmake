# Toolchain file that wraps Android NDK's official toolchain for our layout.
#
# The NDK already ships a CMake toolchain at
#   <ndk>/build/cmake/android.toolchain.cmake
# — we just forward to it with our pinned parameters so scripts don't need to
# repeat them.
#
# Requirements fixed here:
#   - NDK r29 at the known path on this machine
#   - API level 21 (Android 5.0+) — lowest we care about
#   - arm64-v8a only (modern devices)
#   - c++_static STL (fully self-contained .so, no libc++_shared.so alongside)

if(NOT DEFINED ANDROID_NDK_ROOT)
    if(NOT "$ENV{ANDROID_NDK_ROOT}" STREQUAL "")
        set(ANDROID_NDK_ROOT "$ENV{ANDROID_NDK_ROOT}" CACHE PATH "Android NDK root")
    else()
        set(ANDROID_NDK_ROOT "$ENV{LOCALAPPDATA}/Android/Sdk/ndk/29.0.14033849" CACHE PATH "Android NDK root")
    endif()
endif()

if(NOT EXISTS "${ANDROID_NDK_ROOT}/build/cmake/android.toolchain.cmake")
    message(FATAL_ERROR
        "NDK not found at ${ANDROID_NDK_ROOT}. "
        "Set ANDROID_NDK_ROOT cache var to the r29 install path.")
endif()

set(ANDROID_ABI          "arm64-v8a"      CACHE STRING "")
set(ANDROID_PLATFORM     "android-21"     CACHE STRING "")
set(ANDROID_STL          "c++_static"     CACHE STRING "")
set(ANDROID_PIE          TRUE             CACHE BOOL   "")

# Forward to the NDK's own toolchain.
include("${ANDROID_NDK_ROOT}/build/cmake/android.toolchain.cmake")
