# =============================================================================
# Game Boy / Game Boy Color / Game Boy Advance core (mGBA)
#
# Expects the mGBA source tree (the folder containing "include/mgba/core/core.h")
# either vendored at cores/mgba/upstream/ or provided through the MGBA_ROOT
# cache variable.
#
# mGBA ships its own CMake build. Adding it with EXCLUDE_FROM_ALL makes it
# detect LIBMGBA_ONLY (via the directory's EXCLUDE_FROM_ALL property) and build
# just the static "mgba" library with the GB + GBA cores, VFS enabled, and all
# optional dependencies (zlib, PNG, FFmpeg, Qt, SDL, ...) disabled.
# =============================================================================

set(MGBA_DEFAULT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/upstream")

# Fall back to the bundled consoles/ tree during local development.
if(NOT EXISTS "${MGBA_DEFAULT_DIR}/include/mgba/core/core.h")
    if(EXISTS "${CMAKE_SOURCE_DIR}/../consoles/gameboyadvanced and gbc/mgba-master/include/mgba/core/core.h")
        set(MGBA_DEFAULT_DIR "${CMAKE_SOURCE_DIR}/../consoles/gameboyadvanced and gbc/mgba-master")
    endif()
endif()

set(MGBA_ROOT "${MGBA_DEFAULT_DIR}" CACHE PATH "Path to the mGBA source tree")

if(NOT EXISTS "${MGBA_ROOT}/include/mgba/core/core.h")
    message(WARNING
        "mGBA sources not found (looked in ${MGBA_ROOT}).\n"
        "Run tools/vendor-emulators.ps1 or set -DMGBA_ROOT=<path>.\n"
        "Skipping the GB/GBC/GBA core.")
    return()
endif()

message(STATUS "GB/GBC/GBA core: using mGBA at ${MGBA_ROOT}")

# Build the upstream library in its own binary dir. EXCLUDE_FROM_ALL keeps it
# out of the default "all" target; the adapter below pulls it in.
add_subdirectory("${MGBA_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/upstream" EXCLUDE_FROM_ALL)

# -----------------------------------------------------------------------------
# Adapter
#
# The adapter #includes mGBA's public headers, so it must be compiled with the
# exact same feature macros as the library itself. Several of them (ENABLE_VFS,
# ENABLE_DIRECTORIES, ...) change the layout of struct mCore, so a mismatch
# would make member/offset accesses read garbage. mGBA stores these in the
# target's COMPILE_DEFINITIONS property, which CMake does not propagate to
# consumers, so mirror them onto the adapter explicitly.
# -----------------------------------------------------------------------------

add_library(gb_core STATIC gb_core.cpp)

get_target_property(MGBA_COMPILE_DEFS mgba COMPILE_DEFINITIONS)
if(MGBA_COMPILE_DEFS)
    target_compile_definitions(gb_core PRIVATE ${MGBA_COMPILE_DEFS})
endif()

# Set at the mGBA directory scope (so they do not reach us automatically) but
# still visible to the public headers.
target_compile_definitions(gb_core PRIVATE M_CORE_GB M_CORE_GBA)

target_include_directories(gb_core PRIVATE "${MGBA_ROOT}/include")

target_link_libraries(gb_core
    PRIVATE emu_framework
    PUBLIC  mgba
)

if(MSVC)
    target_compile_options(gb_core PRIVATE /W3)
else()
    target_compile_options(gb_core PRIVATE -Wall)
endif()

target_compile_definitions(emu_framework PUBLIC EMU_ENABLE_GB)

set(EMU_CORE_TARGETS ${EMU_CORE_TARGETS} gb_core PARENT_SCOPE)
