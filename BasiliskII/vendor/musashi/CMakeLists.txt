# Musashi 680x0 interpreter - the golden-path CPU engine.
#
# Vendored with local fixes (see docs/MUSASHI_FIXES.md at the repository root).

# m68kmake expands m68k_in.c into the 65,536-entry opcode jump table and the
# handler bodies. It takes an output *prefix* as argv[1] -- concatenated
# directly with "m68kops.c"/"m68kops.h", so it must end in a separator -- and
# the input file as argv[2]. Passing both keeps the generated sources in the
# build tree; the previous Makefile instead chdir'd into this directory and let
# them land next to the checked-in sources.
#
# This is a build-time tool, so it must run on the host. That holds across the
# supported matrix: every target is either built natively, or is the macOS
# Intel slice cross-built from Apple Silicon, where the tool still runs.
add_executable(m68kmake m68kmake.c)

set(MUSASHI_GENERATED
    ${CMAKE_CURRENT_BINARY_DIR}/m68kops.h
    ${CMAKE_CURRENT_BINARY_DIR}/m68kops.c
)

add_custom_command(
    OUTPUT ${MUSASHI_GENERATED}
    COMMAND m68kmake ${CMAKE_CURRENT_BINARY_DIR}/ ${CMAKE_CURRENT_SOURCE_DIR}/m68k_in.c
    DEPENDS m68kmake ${CMAKE_CURRENT_SOURCE_DIR}/m68k_in.c
    COMMENT "Generating Musashi 680x0 opcode tables"
    VERBATIM
)

# m68kcpu.c #includes m68kfpu.c and m68kmmu.h, so they are dependencies of the
# build rather than separate translation units.
add_library(musashi STATIC
    m68kcpu.c
    m68kdasm.c
    softfloat/softfloat.c
    ${MUSASHI_GENERATED}
)

target_include_directories(musashi PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/softfloat
    ${CMAKE_CURRENT_BINARY_DIR}
)

# Musashi's configuration header reaches into the emulator's own types.
target_include_directories(musashi PRIVATE ${COCKATRICE_PLATFORM_INCLUDE_DIRS})
target_compile_definitions(musashi PRIVATE ${COCKATRICE_COMMON_DEFS})

if(MSVC)
    target_compile_options(musashi PRIVATE /w)
else()
    target_compile_options(musashi PRIVATE -w)
endif()
