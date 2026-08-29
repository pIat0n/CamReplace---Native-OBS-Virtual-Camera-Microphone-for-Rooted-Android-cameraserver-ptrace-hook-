# Toolchain file for LLVM-MinGW (x86_64-w64-mingw32)
# Builds fully static Windows x64 executables with clang++.
#
# Invoked as:
#   cmake -B build/host -S host -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=../../toolchains/llvm-mingw.cmake

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Pin to the known good Windows toolchain by default, but allow Linux/macOS
# cross-build hosts to provide their local unpacked llvm-mingw root.
if(NOT DEFINED LLVM_MINGW_ROOT)
    if(NOT "$ENV{LLVM_MINGW_ROOT}" STREQUAL "")
        set(LLVM_MINGW_ROOT "$ENV{LLVM_MINGW_ROOT}" CACHE PATH "LLVM-MinGW root")
    else()
        set(LLVM_MINGW_ROOT "C:/llvm-mingw-20240606-msvcrt-x86_64" CACHE PATH "LLVM-MinGW root")
    endif()
endif()

# CMake cache files treat backslashes as escape characters. Normalise a root
# supplied through a Windows environment variable before composing tool paths.
file(TO_CMAKE_PATH "${LLVM_MINGW_ROOT}" LLVM_MINGW_ROOT)

if(CMAKE_HOST_WIN32)
    set(_CR_TOOL_EXE ".exe")
else()
    set(_CR_TOOL_EXE "")
endif()

set(CMAKE_C_COMPILER   "${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-clang${_CR_TOOL_EXE}")
set(CMAKE_CXX_COMPILER "${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-clang++${_CR_TOOL_EXE}")
set(CMAKE_RC_COMPILER  "${LLVM_MINGW_ROOT}/bin/x86_64-w64-mingw32-windres${_CR_TOOL_EXE}")
set(CMAKE_AR           "${LLVM_MINGW_ROOT}/bin/llvm-ar${_CR_TOOL_EXE}")
set(CMAKE_RANLIB       "${LLVM_MINGW_ROOT}/bin/llvm-ranlib${_CR_TOOL_EXE}")

# Tell CMake we're cross-ish-building so it doesn't try to use host libs.
set(CMAKE_FIND_ROOT_PATH "${LLVM_MINGW_ROOT}/x86_64-w64-mingw32")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Aggressive static linking — the whole point is one exe with nothing alongside.
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-static -static-libgcc -static-libstdc++")

# MSVCRT-edition of llvm-mingw: link against legacy msvcrt (already present on
# every Windows install) rather than the UCRT — avoids shipping UCRT DLLs.
add_compile_definitions(__USE_MINGW_ANSI_STDIO=1)
