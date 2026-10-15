# =============================================================================
# NES core (Nestopia)
#
# Expects the upstream Nestopia source tree (the folder containing
# "source/core") either vendored at cores/nestopia/upstream/ or provided
# through the NST_SOURCE_DIR cache variable.
# =============================================================================

set(NST_DEFAULT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/upstream")

# Fall back to the bundled consoles/ tree during local development.
if(NOT EXISTS "${NST_DEFAULT_DIR}/source/core/NstCore.hpp")
    if(EXISTS "${CMAKE_SOURCE_DIR}/../consoles/nes/nestopia-master/source/core/NstCore.hpp")
        set(NST_DEFAULT_DIR "${CMAKE_SOURCE_DIR}/../consoles/nes/nestopia-master")
    endif()
endif()

set(NST_SOURCE_DIR "${NST_DEFAULT_DIR}" CACHE PATH "Path to the Nestopia source tree")

if(NOT EXISTS "${NST_SOURCE_DIR}/source/core/NstCore.hpp")
    message(WARNING
        "Nestopia sources not found (looked in ${NST_SOURCE_DIR}).\n"
        "Run tools/vendor-emulators.ps1 or set -DNST_SOURCE_DIR=<path>.\n"
        "Skipping the NES core.")
    return()
endif()

message(STATUS "NES core: using Nestopia at ${NST_SOURCE_DIR}")

set(NST_CORE "${NST_SOURCE_DIR}/source/core")

file(GLOB_RECURSE NES_CORE_SOURCES
    "${NST_CORE}/*.cpp"
)

# Only the scalers that are compiled out by the NST_NO_* defines below may be
# excluded: NstVideoRenderer.cpp still references the ScaleX and NTSC filters,
# so those translation units must stay in the build.
list(FILTER NES_CORE_SOURCES EXCLUDE REGEX
    "NstVideoFilter(2xSaI|HqX|xBR)\\.cpp$")

add_library(nestopia STATIC ${NES_CORE_SOURCES})

target_include_directories(nestopia PUBLIC
    "${NST_SOURCE_DIR}/source"
    "${NST_CORE}"
    "${NST_CORE}/api"
)

# No external zlib required; states are stored uncompressed by the adapter.
# Optional scaler filters are disabled to keep the build lean.
target_compile_definitions(nestopia PRIVATE
    NST_NO_ZLIB
    NST_NO_HQ2X
    NST_NO_XBR
    NST_NO_2XSAI
)

if(MSVC)
    target_compile_options(nestopia PRIVATE /W0)
else()
    target_compile_options(nestopia PRIVATE -w)
endif()

# -----------------------------------------------------------------------------
# Adapter
# -----------------------------------------------------------------------------

add_library(nes_core STATIC nes_core.cpp)

target_link_libraries(nes_core
    PRIVATE emu_framework
    PUBLIC  nestopia
)

target_compile_definitions(emu_framework PUBLIC EMU_ENABLE_NES)

set(EMU_CORE_TARGETS ${EMU_CORE_TARGETS} nes_core PARENT_SCOPE)