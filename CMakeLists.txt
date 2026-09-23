# Cockatrice III - top-level build
#
# Supported matrix, and nothing else: {macOS, Windows, Linux} x {AMD64, ARM64},
# all 64-bit, all on SDL 1.2 (sdl12-compat is fine and is what CI installs).
# The 32-bit and PowerPC ports are gone; the guest stays a flat 32-bit 68k
# machine, which is exactly why the host wants a full 4 GB window to itself
# (see BasiliskII/memory.cpp).

cmake_minimum_required(VERSION 3.16)

project(CockatriceIII
    VERSION 0.6.0
    DESCRIPTION "Macintosh Quadra emulator"
    LANGUAGES C CXX)

# sdlmain.m and the Cocoa menu/window bridges are Objective-C(++).
if(APPLE)
    enable_language(OBJC)
    enable_language(OBJCXX)
endif()

# A 32-bit host cannot reserve the 4 GB guest window the memory system is built
# on, and supporting the fallback layout was the single largest source of
# per-platform divergence. Refuse early and clearly rather than failing deep in
# memory_init().
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "Cockatrice III is 64-bit only (got a ${CMAKE_SIZEOF_VOID_P}-byte pointer). "
        "Select a 64-bit toolchain.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Normalise the host architecture to the two names used throughout. On Apple,
# CMAKE_OSX_ARCHITECTURES wins when set, so `-DCMAKE_OSX_ARCHITECTURES=x86_64`
# on an Apple Silicon machine cross-builds the Intel slice correctly.
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    list(LENGTH CMAKE_OSX_ARCHITECTURES _ck_arch_count)
    if(_ck_arch_count GREATER 1)
        # Universal binaries are produced by building each slice separately and
        # running lipo, because the vendored CPU core's symbol isolation admits
        # only one architecture per build.
        message(FATAL_ERROR
            "Build one architecture at a time; combine slices with lipo "
            "(CMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}).")
    endif()
    set(_ck_raw_arch "${CMAKE_OSX_ARCHITECTURES}")
else()
    set(_ck_raw_arch "${CMAKE_SYSTEM_PROCESSOR}")
endif()
string(TOLOWER "${_ck_raw_arch}" _ck_raw_arch)

if(_ck_raw_arch MATCHES "^(x86_64|amd64)$")
    set(COCKATRICE_ARCH "AMD64")
elseif(_ck_raw_arch MATCHES "^(arm64|aarch64)$")
    set(COCKATRICE_ARCH "ARM64")
else()
    message(FATAL_ERROR
        "Unsupported architecture '${_ck_raw_arch}'; want AMD64 or ARM64.")
endif()

if(APPLE)
    set(COCKATRICE_PLATFORM "darwin")
elseif(WIN32)
    set(COCKATRICE_PLATFORM "windows")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(COCKATRICE_PLATFORM "linux")
else()
    message(FATAL_ERROR
        "Unsupported system '${CMAKE_SYSTEM_NAME}'; want Darwin, Windows or Linux.")
endif()

message(STATUS "Cockatrice III: ${COCKATRICE_PLATFORM}/${COCKATRICE_ARCH}")

option(COCKATRICE_ENABLE_M68K_RS "Build the Rust m68k-rs CPU engine (needs cargo)" ON)
option(COCKATRICE_ENABLE_UAE     "Build the vendored uae-portable-cpu engine + JIT" ON)
option(COCKATRICE_DEBUG_LOGGING  "Enable SCC/EmulOp/heartbeat debug logging" OFF)

include(CTest)

add_subdirectory(BasiliskII)
