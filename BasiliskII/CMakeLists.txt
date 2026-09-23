# Cockatrice III - the emulator itself.
#
# One target, three hosts. Everything host-specific is either a file under
# platform/<host>/ or a short conditional in this file; there are no per-port
# build trees any more.

# The Windows icon and version resource are compiled by windres/rc.
if(WIN32)
    enable_language(RC)
endif()

# ---------------------------------------------------------------------------
# Shared include paths and defines
#
# platform/<host> comes first so its config.h is found ahead of anything else,
# then platform/ for the shared sysdeps.h, then include/ for the emulator's own
# headers. SDL/ is on the list because sysdeps.h pulls in user_strings_sdl.h,
# which now lives there as a single copy shared by all three hosts (the old
# per-port copies were byte-identical).
#
# Exported to the subdirectories below via normal variable scope.
# ---------------------------------------------------------------------------
set(COCKATRICE_PLATFORM_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/${COCKATRICE_PLATFORM}
    ${CMAKE_CURRENT_SOURCE_DIR}/platform
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/SDL
)

set(COCKATRICE_COMMON_DEFS HAVE_CONFIG_H _REENTRANT)
if(COCKATRICE_DEBUG_LOGGING)
    list(APPEND COCKATRICE_COMMON_DEFS
        COCKATRICE_DEBUG=1 SCC_DEBUG=1 EMULOP_DEBUG=1 HEARTBEAT_DEBUG=1)
endif()

# ---------------------------------------------------------------------------
# SDL 1.2 (sdl12-compat satisfies this and is what CI installs)
#
# Sources say #include <SDL/SDL.h>, but FindSDL reports the directory that
# *contains* SDL.h, so its parent has to go on the path as well.
# ---------------------------------------------------------------------------
find_package(SDL REQUIRED)
get_filename_component(SDL_INCLUDE_PARENT "${SDL_INCLUDE_DIR}" DIRECTORY)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
add_subdirectory(slirp)
add_subdirectory(vendor/musashi)

if(COCKATRICE_ENABLE_UAE)
    # Both this core and Musashi descend from separate lineages that happen to
    # share symbol names, so the core is linked symbol-isolated: it exports only
    # its uae_cpu_* API, and its Musashi-compatible m68k_* surface is switched
    # off so it cannot collide with vendor/musashi.
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(UAE_CPU_ISOLATE_SYMBOLS ON CACHE BOOL "" FORCE)
    set(UAE_CPU_MUSASHI_API OFF CACHE BOOL "" FORCE)
    set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(vendor/uae-portable-cpu EXCLUDE_FROM_ALL)
endif()

# ---------------------------------------------------------------------------
# m68k-rs (Rust staticlib)
#
# Built through cargo with an explicit --target so that a cross-built slice
# (notably macOS x86_64 from an Apple Silicon host) links against a matching
# archive; cargo without --target always builds the host triple.
# ---------------------------------------------------------------------------
if(COCKATRICE_ENABLE_M68K_RS)
    if(COCKATRICE_PLATFORM STREQUAL "darwin")
        set(_rs_triple "$<IF:$<BOOL:0>,,>")
        if(COCKATRICE_ARCH STREQUAL "AMD64")
            set(_rs_triple "x86_64-apple-darwin")
        else()
            set(_rs_triple "aarch64-apple-darwin")
        endif()
    elseif(COCKATRICE_PLATFORM STREQUAL "windows")
        if(COCKATRICE_ARCH STREQUAL "AMD64")
            set(_rs_triple "x86_64-pc-windows-gnu")
        else()
            set(_rs_triple "aarch64-pc-windows-gnullvm")
        endif()
    else()
        if(COCKATRICE_ARCH STREQUAL "AMD64")
            set(_rs_triple "x86_64-unknown-linux-gnu")
        else()
            set(_rs_triple "aarch64-unknown-linux-gnu")
        endif()
    endif()

    set(_rs_target_dir ${CMAKE_CURRENT_BINARY_DIR}/m68k_rs_target)
    set(_rs_lib ${_rs_target_dir}/${_rs_triple}/release/libcockatrice_m68k_rs.a)

    add_custom_command(
        OUTPUT ${_rs_lib}
        COMMAND cargo build --release
                --target ${_rs_triple}
                --manifest-path ${CMAKE_CURRENT_SOURCE_DIR}/cpu/m68k_rs/Cargo.toml
                --target-dir ${_rs_target_dir}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cpu/m68k_rs/Cargo.toml
                ${CMAKE_CURRENT_SOURCE_DIR}/cpu/m68k_rs/src/lib.rs
        COMMENT "Building m68k-rs (${_rs_triple})"
        VERBATIM
    )
    add_custom_target(m68k_rs_cargo DEPENDS ${_rs_lib})

    add_library(m68k_rs STATIC IMPORTED GLOBAL)
    set_target_properties(m68k_rs PROPERTIES IMPORTED_LOCATION ${_rs_lib})
    add_dependencies(m68k_rs m68k_rs_cargo)
endif()

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
set(COCKATRICE_CORE_SOURCES
    main.cpp
    prefs.cpp
    prefs_parse.cpp
    rom_patches.cpp
    slot_rom.cpp
    rsrc_patches.cpp
    emul_op.cpp
    toolbox_traps.cpp
    toolbox_menu.cpp
    toolbox_window.cpp
    macos_util.cpp
    xpram.cpp
    timer.cpp
    adb.cpp
    serial.cpp
    scc.cpp
    ether.cpp
    sony.cpp
    disk.cpp
    cdrom.cpp
    scsi.cpp
    video.cpp
    audio.cpp
    extfs.cpp
    user_strings.cpp
    memory.cpp
    cpu_engine.cpp
    cpu_glue.cpp
)

# The SDL layer is genuinely shared: sys_unix_sdl.cpp and sdl_pcap.cpp carry
# their own __linux__/WIN32 branches rather than having per-port twins.
set(COCKATRICE_SDL_SOURCES
    SDL/main_sdl.cpp
    SDL/sys_unix_sdl.cpp
    SDL/video_sdl.cpp
    SDL/video_blit.cpp
    SDL/audio_sdl.cpp
    SDL/sdl_pcap.cpp
    SDL/menu_bar.cpp
    SDL/menu_model.cpp
    SDL/host_paths.cpp
    SDL/timer_common.cpp
    SDL/user_strings_sdl.cpp
)

set(COCKATRICE_DUMMY_SOURCES
    dummy/prefs_dummy.cpp
    dummy/clip_dummy.cpp
    dummy/serial_dummy.cpp
    dummy/scsi_dummy.cpp
    dummy/prefs_editor_dummy.cpp
)

# The engine adapters live in cpu/; each translates one 680x0 core onto the
# CPUEngine interface in include/cpu_engine.h.
set(COCKATRICE_CPU_SOURCES
    cpu/musashi_glue.cpp
    cpu/memory_musashi.cpp
)
if(COCKATRICE_ENABLE_M68K_RS)
    list(APPEND COCKATRICE_CPU_SOURCES cpu/m68k_rs_glue.cpp)
endif()
if(COCKATRICE_ENABLE_UAE)
    list(APPEND COCKATRICE_CPU_SOURCES cpu/uae_cpu_glue.cpp)
endif()

# NOTE: there is no jit_support/ here any more. It was a shared W^X/icache
# module proposed in docs/jit-memory-architecture.md and compiled by the old
# Makefiles, but nothing ever called it: the vendored CPU core keeps its own
# equivalents (src/cpu/vm.c and the ARM64 compemu backends) and the Rust engine
# never referenced it, so it only ever produced unused objects. It has been
# deleted; recover it from git history if that design is ever taken up.

# Each host supplies exactly three symbols -- MenuBar_Init, MenuBar_UpdateAll
# and MenuBar_ShowOpenFileDialog. The actions behind the menus live in
# SDL/menu_bar.cpp and the menu bar itself -- which menus exist, their labels,
# ordering, shortcuts and enable rules -- is described once in
# SDL/menu_model.cpp, so a host only ever translates that description into
# NSMenu / HMENU / GtkMenu calls.
if(COCKATRICE_PLATFORM STREQUAL "darwin")
    set(COCKATRICE_PLATFORM_SOURCES
        platform/darwin/sdlmain.m
        bridge/darwin/macos_menu_bridge.mm
        bridge/darwin/macos_window_bridge.mm
        platform/posix/xpram_unix.cpp
    )
elseif(COCKATRICE_PLATFORM STREQUAL "windows")
    set(COCKATRICE_PLATFORM_SOURCES
        platform/windows/menu_bar_win32.cpp
        platform/windows/cockatrice.rc
        dummy/xpram_dummy.cpp
    )
else()
    # GTK is optional and off: the dialog falls back to zenity then kdialog,
    # so a Linux build needs no desktop toolkit at all.
    set(COCKATRICE_PLATFORM_SOURCES
        platform/linux/menu_bar_linux.cpp
        platform/posix/xpram_unix.cpp
    )
endif()

add_executable(CockatriceIII
    ${COCKATRICE_CORE_SOURCES}
    ${COCKATRICE_SDL_SOURCES}
    ${COCKATRICE_DUMMY_SOURCES}
    ${COCKATRICE_CPU_SOURCES}
    ${COCKATRICE_PLATFORM_SOURCES}
)

target_include_directories(CockatriceIII PRIVATE
    ${COCKATRICE_PLATFORM_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/bridge/darwin
    ${CMAKE_CURRENT_SOURCE_DIR}/cpu/m68k_rs/include
    ${SDL_INCLUDE_DIR}
    ${SDL_INCLUDE_PARENT}
)
if(WIN32)
    # Vendored libpcap headers; the library itself is dlopen'd at runtime.
    target_include_directories(CockatriceIII PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/platform/windows)
endif()

target_compile_definitions(CockatriceIII PRIVATE
    ${COCKATRICE_COMMON_DEFS}
    ENABLE_M68K_RS_CPU=$<BOOL:${COCKATRICE_ENABLE_M68K_RS}>
    ENABLE_UAE_PORTABLE_CPU=$<BOOL:${COCKATRICE_ENABLE_UAE}>
)

if(MSVC)
    target_compile_options(CockatriceIII PRIVATE /W3)
else()
    target_compile_options(CockatriceIII PRIVATE
        -Wall -Wno-unused -Wno-multichar -Wno-deprecated-declarations
        -Wno-format -Wno-write-strings)
endif()

target_link_libraries(CockatriceIII PRIVATE slirp musashi ${SDL_LIBRARY})
if(COCKATRICE_ENABLE_UAE)
    target_link_libraries(CockatriceIII PRIVATE uaecpu_isolated)
endif()
if(COCKATRICE_ENABLE_M68K_RS)
    target_link_libraries(CockatriceIII PRIVATE m68k_rs)
endif()

if(APPLE)
    target_link_libraries(CockatriceIII PRIVATE
        "-framework Cocoa" "-framework IOKit" pthread ${CMAKE_DL_LIBS})
elseif(WIN32)
    target_link_libraries(CockatriceIII PRIVATE
        ws2_32 iphlpapi comdlg32 user32 ntdll userenv bcrypt
        -static-libgcc -static-libstdc++)
else()
    target_link_libraries(CockatriceIII PRIVATE pthread m ${CMAKE_DL_LIBS})
endif()

# ---------------------------------------------------------------------------
# macOS: ad-hoc signing and the .app bundle
#
# The binary is signed in place rather than being built as MACOSX_BUNDLE,
# because the emulator is also run directly from the build directory and the
# packaging step wants a plain executable alongside the bundle.
#
# The JIT entitlement is required: without com.apple.security.cs.allow-jit the
# translated-code mapping cannot be made executable under Hardened Runtime.
# ---------------------------------------------------------------------------
if(APPLE)
    set(_entitlements ${CMAKE_CURRENT_SOURCE_DIR}/platform/darwin/entitlements.plist)
    # Signing goes through codesign-jit.sh, which serialises it. Concurrent
    # codesign invocations intermittently fail to write their result back (see
    # the script's header for the full symptom); serialising is the fix, and it
    # still surfaces a genuine signing failure.
    add_custom_command(TARGET CockatriceIII POST_BUILD
        COMMAND ${CMAKE_SOURCE_DIR}/scripts/codesign-jit.sh
                ${_entitlements} $<TARGET_FILE:CockatriceIII>
        COMMENT "Ad-hoc signing with JIT entitlement"
        VERBATIM)

    set(COCKATRICE_BUNDLE_VERSION "${PROJECT_VERSION}" CACHE STRING
        "CFBundleVersion / CFBundleShortVersionString")
    set(BUNDLE_VERSION "${COCKATRICE_BUNDLE_VERSION}")
    configure_file(platform/darwin/Info.plist.in
                   ${CMAKE_CURRENT_BINARY_DIR}/Info.plist @ONLY)

    set(_app ${CMAKE_CURRENT_BINARY_DIR}/CockatriceIII.app)
    add_custom_target(bundle
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${_app}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_app}/Contents/MacOS
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_app}/Contents/Resources
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:CockatriceIII>
                ${_app}/Contents/MacOS/CockatriceIII
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/Info.plist
                ${_app}/Contents/Info.plist
        COMMAND ${CMAKE_COMMAND} -E copy
                ${CMAKE_CURRENT_SOURCE_DIR}/platform/darwin/CockatriceIII.icns
                ${_app}/Contents/Resources/CockatriceIII.icns
        COMMAND codesign -s - --force --deep --entitlements ${_entitlements} ${_app}
        DEPENDS CockatriceIII
        COMMENT "Assembling CockatriceIII.app"
        VERBATIM)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
