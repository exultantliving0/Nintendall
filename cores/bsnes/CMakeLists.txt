# =============================================================================
# SNES core (bsnes)
#
# Expects the bsnes repository root (the folder containing "bsnes/", "nall/"
# and "libco/") either vendored at cores/bsnes/upstream/ or provided
# through the BSNES_ROOT cache variable.
#
# Only the library portion of bsnes is built here: the GUI targets
# (target-bsnes) and the existing libretro target are excluded. The Nintendall
# adapter supplies its own platform + frontend instead.
# =============================================================================

set(BSNES_DEFAULT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/upstream")

if(NOT EXISTS "${BSNES_DEFAULT_DIR}/bsnes/emulator/emulator.cpp")
    if(EXISTS "${CMAKE_SOURCE_DIR}/../consoles/snes/bsnes-master/bsnes/emulator/emulator.cpp")
        set(BSNES_DEFAULT_DIR "${CMAKE_SOURCE_DIR}/../consoles/snes/bsnes-master")
    endif()
endif()

set(BSNES_ROOT "${BSNES_DEFAULT_DIR}" CACHE PATH "Path to the bsnes repository root")

if(NOT EXISTS "${BSNES_ROOT}/bsnes/emulator/emulator.cpp")
    message(WARNING
        "bsnes sources not found (looked in ${BSNES_ROOT}).\n"
        "Run tools/vendor-emulators.ps1 or set -DBSNES_ROOT=<path>.\n"
        "Skipping the SNES core.")
    return()
endif()

message(STATUS "SNES core: using bsnes at ${BSNES_ROOT}")

set(B    "${BSNES_ROOT}/bsnes")
set(NALL "${BSNES_ROOT}/nall")
set(LIBCO "${BSNES_ROOT}/libco")

# -----------------------------------------------------------------------------
# bsnes library sources (mirrors bsnes/GNUmakefile)
# -----------------------------------------------------------------------------

set(BSNES_SOURCES
    "${LIBCO}/libco.c"

    "${B}/emulator/emulator.cpp"

    "${B}/sfc/interface/interface.cpp"
    "${B}/sfc/system/system.cpp"
    "${B}/sfc/controller/controller.cpp"
    "${B}/sfc/cartridge/cartridge.cpp"
    "${B}/sfc/memory/memory.cpp"
    "${B}/sfc/cpu/cpu.cpp"
    "${B}/sfc/smp/smp.cpp"
    "${B}/sfc/dsp/dsp.cpp"
    "${B}/sfc/ppu/ppu.cpp"
    "${B}/sfc/ppu-fast/ppu.cpp"
    "${B}/sfc/expansion/expansion.cpp"
    "${B}/sfc/coprocessor/coprocessor.cpp"
    "${B}/sfc/slot/slot.cpp"

    "${B}/processor/arm7tdmi/arm7tdmi.cpp"
    "${B}/processor/gsu/gsu.cpp"
    "${B}/processor/hg51b/hg51b.cpp"
    "${B}/processor/spc700/spc700.cpp"
    "${B}/processor/upd96050/upd96050.cpp"
    "${B}/processor/wdc65816/wdc65816.cpp"

    "${B}/gb/Core/apu.c"
    "${B}/gb/Core/camera.c"
    "${B}/gb/Core/display.c"
    "${B}/gb/Core/gb.c"
    "${B}/gb/Core/joypad.c"
    "${B}/gb/Core/mbc.c"
    "${B}/gb/Core/memory.c"
    "${B}/gb/Core/printer.c"
    "${B}/gb/Core/random.c"
    "${B}/gb/Core/rewind.c"
    "${B}/gb/Core/rumble.c"
    "${B}/gb/Core/save_state.c"
    "${B}/gb/Core/sgb.c"
    "${B}/gb/Core/sm83_cpu.c"
    "${B}/gb/Core/timing.c"
)

add_library(bsnes STATIC ${BSNES_SOURCES})

target_include_directories(bsnes PUBLIC
    "${BSNES_ROOT}"          # <nall/...>, <libco/...>
    "${B}"                   # <emulator/...>, <sfc/...>, <processor/...>, <heuristics/...>
    "${B}/gb"                # SameBoy internals
    "${NALL}"
)

target_compile_definitions(bsnes PUBLIC
    GB_INTERNAL
    GB_DISABLE_DEBUGGER
    GB_DISABLE_CHEATS
    _GNU_SOURCE
    GB_VERSION="115"
)

set_target_properties(bsnes PROPERTIES
    C_STANDARD 11
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

if(MSVC)
    target_compile_options(bsnes PRIVATE /W0 /wd4996)
    # bsnes/SameBoy upstream targets GCC/Clang; relax a few MSVC-only issues.
    target_compile_options(bsnes PRIVATE /wd4100 /wd4189 /wd4244 /wd4267 /wd4996)
else()
    target_compile_options(bsnes PRIVATE
        -w -Wno-narrowing -Wno-multichar -fno-strict-aliasing)
endif()

# -----------------------------------------------------------------------------
# Adapter
# -----------------------------------------------------------------------------

add_library(snes_core STATIC snes_core.cpp)

target_link_libraries(snes_core
    PRIVATE emu_framework
    PUBLIC  bsnes
)

# resources.hpp (embedded Boards/iplrom) lives in the libretro target dir.
target_include_directories(snes_core PRIVATE
    "${B}/target-libretro"
    "${B}/heuristics"
)

set_target_properties(snes_core PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

target_compile_definitions(emu_framework PUBLIC EMU_ENABLE_SNES)

set(EMU_CORE_TARGETS ${EMU_CORE_TARGETS} snes_core PARENT_SCOPE)
