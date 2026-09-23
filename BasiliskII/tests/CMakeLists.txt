# Cockatrice III test suites.
#
# Two families: the Basilisk subsystem tests (the gate) and the multi-engine
# 680x0 CPU suites. The emulator objects they share are compiled once into an
# OBJECT library, which also sidesteps the --whole-archive/-force_load dance
# the old Makefile needed to pull test-only symbols out of a static archive.

set(COCKATRICE_TEST_CORE_SOURCES
    include/test_env.cpp
    include/test_stubs.cpp
    include/test_sys.cpp
    ../memory.cpp
    ../prefs_parse.cpp
    ../SDL/video_blit.cpp
    ../cpu_glue.cpp
    ../cpu_engine.cpp
    ../emul_op.cpp
    ../toolbox_traps.cpp
    ../toolbox_menu.cpp
    ../toolbox_window.cpp
    ../scc.cpp
    ../scsi.cpp
    ../dummy/scsi_dummy.cpp
    ../disk.cpp
    ../macos_util.cpp
    ../rom_patches.cpp
    ../rsrc_patches.cpp
    ../cpu/musashi_glue.cpp
    ../cpu/memory_musashi.cpp
)
if(COCKATRICE_ENABLE_M68K_RS)
    list(APPEND COCKATRICE_TEST_CORE_SOURCES ../cpu/m68k_rs_glue.cpp)
endif()
if(COCKATRICE_ENABLE_UAE)
    list(APPEND COCKATRICE_TEST_CORE_SOURCES ../cpu/uae_cpu_glue.cpp)
endif()

add_library(cockatrice_test_core OBJECT ${COCKATRICE_TEST_CORE_SOURCES})

target_include_directories(cockatrice_test_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${COCKATRICE_PLATFORM_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../dummy
    ${CMAKE_CURRENT_SOURCE_DIR}/../cpu/m68k_rs/include
    ${SDL_INCLUDE_DIR}
    ${SDL_INCLUDE_PARENT}
)

target_compile_definitions(cockatrice_test_core PUBLIC
    ${COCKATRICE_COMMON_DEFS}
    ENABLE_M68K_RS_CPU=$<BOOL:${COCKATRICE_ENABLE_M68K_RS}>
    ENABLE_UAE_PORTABLE_CPU=$<BOOL:${COCKATRICE_ENABLE_UAE}>
    TEST_REPO_ROOT="${CMAKE_SOURCE_DIR}"
    MUSASHI_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../vendor/musashi/test"
)

# PUBLIC throughout: these propagate include directories as well as archives,
# and this object library itself compiles uae_cpu_glue.cpp, which needs the
# uae_cpu.h that uaecpu_isolated exports as a usage requirement.
target_link_libraries(cockatrice_test_core PUBLIC musashi)
if(COCKATRICE_ENABLE_UAE)
    target_link_libraries(cockatrice_test_core PUBLIC uaecpu_isolated)
endif()
if(COCKATRICE_ENABLE_M68K_RS)
    target_link_libraries(cockatrice_test_core PUBLIC m68k_rs)
endif()

if(NOT MSVC)
    target_compile_options(cockatrice_test_core PUBLIC -Wall -Wextra -Wno-unused -g)
endif()

# ASAN=ON builds the suites with AddressSanitizer and UBSan. The resource and
# ROM patch code writes through raw pointers into buffers sized at runtime, so
# out-of-bounds writes and unsigned-underflow scans are exactly the failure
# mode these tests exist to catch, and a plain build can miss them entirely.
option(COCKATRICE_TEST_ASAN "Build the test suites with ASan + UBSan" OFF)
if(COCKATRICE_TEST_ASAN AND NOT MSVC)
    target_compile_options(cockatrice_test_core PUBLIC
        -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(cockatrice_test_core PUBLIC -fsanitize=address,undefined)
endif()

# Shared link set for every test binary.
function(cockatrice_add_test name)
    # Linking the OBJECT library contributes its objects and its PUBLIC usage
    # requirements. Naming $<TARGET_OBJECTS:...> here as well would link every
    # object a second time and fail on duplicate symbols.
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE cockatrice_test_core)
    if(APPLE)
        target_link_libraries(${name} PRIVATE pthread ${CMAKE_DL_LIBS})
        # The UAE JIT needs the same entitlement here as in the app, or the
        # JIT-enabled engine configurations cannot map executable pages. As
        # with the app, signing is serialised through codesign-jit.sh: these
        # POST_BUILD steps are exactly what used to run concurrently.
        add_custom_command(TARGET ${name} POST_BUILD
            COMMAND ${CMAKE_SOURCE_DIR}/scripts/codesign-jit.sh
                    ${CMAKE_CURRENT_SOURCE_DIR}/../platform/darwin/entitlements.plist
                    $<TARGET_FILE:${name}>
            VERBATIM)
    elseif(WIN32)
        # The same system libraries as the app: Winsock for the LToUDP and
        # slirp code, and what the m68k-rs Rust standard library needs.
        target_link_libraries(${name} PRIVATE
            ws2_32 iphlpapi comdlg32 user32 ntdll userenv bcrypt pthread
            -static-libgcc -static-libstdc++)
    elseif(UNIX)
        target_link_libraries(${name} PRIVATE pthread m ${CMAKE_DL_LIBS})
    endif()
    add_test(NAME ${name} COMMAND ${name})
    # These drive a full 68k machine; a hung engine should fail, not hang CI.
    set_tests_properties(${name} PROPERTIES TIMEOUT 120)
endfunction()

foreach(t
    basilisk_memory_test
    basilisk_engine_test
    basilisk_emulop_test
    basilisk_patches_test
    basilisk_rsrcpatch_test
    basilisk_patchguard_test
    basilisk_stubabi_test
    basilisk_toolbox_test
    basilisk_scsi_test
    basilisk_scc_test
    basilisk_disk_test
    basilisk_prefs_test
    basilisk_blit_test)
    cockatrice_add_test(${t} basilisk/${t}.cpp)
    # The Basilisk suites are the gate: these are required to pass.
    set_tests_properties(${t} PROPERTIES LABELS gate)
endforeach()

# The CPU suites are one binary covering every registered engine.
cockatrice_add_test(cpu_tests
    cpu/cpu_test_main.cpp
    cpu/cpu_emulop.cpp
    cpu/cpu_instructions.cpp
    cpu/cpu_jsr.cpp
    cpu/cpu_exceptions.cpp
    cpu/cpu_fpu.cpp
    cpu/cpu_opcode_battery.cpp
    cpu/cpu_regressions.cpp
    cpu/rom_snippets.cpp
    cpu/cpu_interrupt_stress.cpp
)

# The CPU suite is reported rather than gated, matching the make-era
# run_all.sh, which failed the build on it only under STRICT_CPU=1.
#
# It has 20 known failures: four opcode fixtures (abcd, sbcd, chk2, cmp2)
# across each of the five UAE engine configurations. Those are real accuracy
# gaps in the UAE core rather than test-harness problems -- Musashi and m68k-rs
# pass the same fixtures -- and are documented in tests/README.md.
#
#   ctest -L gate   # must stay green
#   ctest -L cpu    # engine accuracy, informational
set_tests_properties(cpu_tests PROPERTIES LABELS cpu)
