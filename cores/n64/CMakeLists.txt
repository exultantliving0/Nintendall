# =============================================================================
# Nintendo 64 core (mupen64plus-core + mupen64plus-rsp-cxd4 + angrylion)
#
# The upstream trees are vendored at cores/n64/upstream/.  The Nintendall
# adapter lives in this folder: n64_core.cpp, n64_glue.{h,c}, n64_rsp.c and
# shim/.
#
# Build notes
#   - mupen64plus-core is built WITH the x86-64 dynarec (see "x86-64 dynarec"
#     below).  Without it the R4300 runs through the C interpreter, which is
#     several times slower and makes most games unplayable.
#   - angrylion (software RDP) runs single-threaded (parallel = false) so no
#     pthread dependency is required.  It renders at native N64 resolution
#     (max 768x640) and the frontend scales up, so the pixel count stays low.
#   - The SDL subset (sdl_shim.c) and the headless frontend stubs
#     (upstream_stubs.c) are compiled straight into m64p_core.
# =============================================================================

set(M64P_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/upstream/mupen64plus-core")
set(M64P_SRC  "${M64P_ROOT}/src")
set(M64P_API  "${M64P_SRC}/api")
set(M64P_MAIN "${M64P_SRC}/main")
set(RSP_CXD4 "${CMAKE_CURRENT_SOURCE_DIR}/upstream/mupen64plus-rsp-cxd4")
set(ANG       "${CMAKE_CURRENT_SOURCE_DIR}/upstream/angrylion")
set(ZLIB      "${CMAKE_CURRENT_SOURCE_DIR}/upstream/zlib")
set(SHIM      "${CMAKE_CURRENT_SOURCE_DIR}/shim")

# -----------------------------------------------------------------------------
# angrylion (software RDP)
# -----------------------------------------------------------------------------
option(N64_PARALLEL_VIDEO
       "Rasterise N64 frames on multiple threads (angrylion software RDP)" ON)

add_library(angrylion STATIC
    "${ANG}/n64video.c"
)
target_include_directories(angrylion PUBLIC "${ANG}")
set_target_properties(angrylion PROPERTIES POSITION_INDEPENDENT_CODE ON
    C_STANDARD 99 C_STANDARD_REQUIRED ON)

# Angrylion is a software rasteriser: it is the expensive half of an N64 frame.
# parallel_al.cpp is its thread pool and splits each frame across cores, which
# is what makes the picture smooth on a multi-core machine.  It is C++ (std::thread,
# which MinGW satisfies with winpthreads and links statically), so the target has
# to be allowed C++17 as well.
if(N64_PARALLEL_VIDEO)
    target_sources(angrylion PRIVATE "${ANG}/parallel_al.cpp")
    set_target_properties(angrylion PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
    target_compile_definitions(angrylion PUBLIC N64_PARALLEL_VIDEO)
    find_package(Threads REQUIRED)
    target_link_libraries(angrylion PUBLIC Threads::Threads)
    message(STATUS "N64: multi-threaded angrylion rasteriser enabled")
else()
    message(STATUS "N64: angrylion rasteriser is single-threaded")
endif()

# -----------------------------------------------------------------------------
# mupen64plus-rsp-cxd4 (LLE RSP interpreter)
#
# angrylion is a software RDP only: it consumes a raw RDP command list.  That
# list has to be produced by an LLE RSP running the F3DEX microcode, which then
# writes DPC_START/DPC_END and thereby triggers the graphics plugin's
# processRdpList().  mupen64plus-rsp-hle is HLE-only and never does this, so
# cxd4 (a full RSP interpreter) is used instead.
#
# module.c is a plugin: normally it resolves the core's Config*/CoreDoCommand
# entry points through a dynamic-library handle.  We never call PluginStartup(),
# so those pointers stay unset (see n64_rsp.c, which installs minimal stubs
# before InitiateRSP()).  The identifiers are renamed at compile time purely so
# that they do not clash with mupen64plus-core's own public API of the same
# name when everything is linked into one executable.
# -----------------------------------------------------------------------------
set(RSP_CXD4_SRCS
    "${RSP_CXD4}/su.c"
    "${RSP_CXD4}/vu/add.c"
    "${RSP_CXD4}/vu/divide.c"
    "${RSP_CXD4}/vu/logical.c"
    "${RSP_CXD4}/vu/multiply.c"
    "${RSP_CXD4}/vu/select.c"
    "${RSP_CXD4}/vu/vu.c"
    "${RSP_CXD4}/module.c"
)

# Keep cxd4's config-plugin shims out of mupen64plus-core's namespace.
set(RSP_CXD4_RENAMES
    -DConfigOpenSection=cxd4_ConfigOpenSection
    -DConfigDeleteSection=cxd4_ConfigDeleteSection
    -DConfigSetParameter=cxd4_ConfigSetParameter
    -DConfigGetParameter=cxd4_ConfigGetParameter
    -DConfigSetDefaultFloat=cxd4_ConfigSetDefaultFloat
    -DConfigSetDefaultBool=cxd4_ConfigSetDefaultBool
    -DConfigGetParamBool=cxd4_ConfigGetParamBool
    -DCoreDoCommand=cxd4_CoreDoCommand
    -DDebugMessage=cxd4_DebugMessage
    -DPluginGetVersion=cxd4_PluginGetVersion
)

add_library(m64p_rsp_cxd4 STATIC ${RSP_CXD4_SRCS})
target_include_directories(m64p_rsp_cxd4 PRIVATE "${RSP_CXD4}" "${M64P_API}")
target_compile_definitions(m64p_rsp_cxd4 PRIVATE
    -DM64P_PLUGIN_API -DWIN32 -DARCH_MIN_SSE2 ${RSP_CXD4_RENAMES})
target_compile_options(m64p_rsp_cxd4 PRIVATE -msse2 -mstackrealign -w)
set_target_properties(m64p_rsp_cxd4 PROPERTIES POSITION_INDEPENDENT_CODE ON
    C_STANDARD 99 C_STANDARD_REQUIRED ON)

# -----------------------------------------------------------------------------
# mupen64plus-core
#
# Interpreter build: cached_interp.c / pure_interp.c are always compiled;
# the dynarec sources (recomp.c, x86_64/{assemble,dynarec,regcache}.c,
# dyna_start.asm) are omitted.
#
# Excluded: api/{debugger,vidext}.c, main/{eventloop,screenshot,
# sdl_key_converter,lirc,netplay,profile}.c, osd/ (M64P_OSD undefined ->
# inline no-ops).  The SDL subset and the headless frontend stubs (shim/) are
# compiled straight into m64p_core.
# -----------------------------------------------------------------------------
set(M64P_CORE_SRCS
    "${M64P_SRC}/api/callbacks.c"
    "${M64P_SRC}/api/common.c"
    "${M64P_SRC}/api/config.c"
    "${M64P_SRC}/api/frontend.c"
    "${M64P_SRC}/backends/api/video_capture_backend.c"
    "${M64P_SRC}/backends/dummy_video_capture.c"
    "${M64P_SRC}/backends/file_storage.c"
    "${M64P_SRC}/backends/clock_ctime_plus_delta.c"
    "${M64P_SRC}/backends/plugins_compat/audio_plugin_compat.c"
    "${M64P_SRC}/backends/plugins_compat/input_plugin_compat.c"
    "${M64P_SRC}/device/cart/cart.c"
    "${M64P_SRC}/device/cart/af_rtc.c"
    "${M64P_SRC}/device/cart/cart_rom.c"
    "${M64P_SRC}/device/cart/eeprom.c"
    "${M64P_SRC}/device/cart/flashram.c"
    "${M64P_SRC}/device/cart/is_viewer.c"
    "${M64P_SRC}/device/cart/sram.c"
    "${M64P_SRC}/device/controllers/game_controller.c"
    "${M64P_SRC}/device/controllers/vru_controller.c"
    "${M64P_SRC}/device/controllers/paks/biopak.c"
    "${M64P_SRC}/device/controllers/paks/mempak.c"
    "${M64P_SRC}/device/controllers/paks/rumblepak.c"
    "${M64P_SRC}/device/controllers/paks/transferpak.c"
    "${M64P_SRC}/device/dd/dd_controller.c"
    "${M64P_SRC}/device/dd/disk.c"
    "${M64P_SRC}/device/device.c"
    "${M64P_SRC}/device/gb/gb_cart.c"
    "${M64P_SRC}/device/gb/mbc3_rtc.c"
    "${M64P_SRC}/device/gb/m64282fp.c"
    "${M64P_SRC}/device/memory/memory.c"
    "${M64P_SRC}/device/pif/bootrom_hle.c"
    "${M64P_SRC}/device/pif/cic.c"
    "${M64P_SRC}/device/pif/n64_cic_nus_6105.c"
    "${M64P_SRC}/device/pif/pif.c"
    "${M64P_SRC}/device/r4300/cached_interp.c"
    "${M64P_SRC}/device/r4300/cp0.c"
    "${M64P_SRC}/device/r4300/cp1.c"
    "${M64P_SRC}/device/r4300/cp2.c"
    "${M64P_SRC}/device/r4300/idec.c"
    "${M64P_SRC}/device/r4300/interrupt.c"
    "${M64P_SRC}/device/r4300/pure_interp.c"
    "${M64P_SRC}/device/r4300/r4300_core.c"
    "${M64P_SRC}/device/r4300/tlb.c"
    "${M64P_SRC}/device/rcp/ai/ai_controller.c"
    "${M64P_SRC}/device/rcp/mi/mi_controller.c"
    "${M64P_SRC}/device/rcp/pi/pi_controller.c"
    "${M64P_SRC}/device/rcp/rdp/fb.c"
    "${M64P_SRC}/device/rcp/rdp/rdp_core.c"
    "${M64P_SRC}/device/rcp/ri/ri_controller.c"
    "${M64P_SRC}/device/rcp/rsp/rsp_core.c"
    "${M64P_SRC}/device/rcp/si/si_controller.c"
    "${M64P_SRC}/device/rcp/vi/vi_controller.c"
    "${M64P_SRC}/device/rdram/rdram.c"
    "${M64P_SRC}/main/cheat.c"
    "${M64P_SRC}/main/main.c"
    "${M64P_SRC}/main/rom.c"
    "${M64P_SRC}/main/savestates.c"
    "${M64P_SRC}/main/util.c"
    "${M64P_SRC}/main/workqueue.c"
    "${M64P_SRC}/plugin/plugin.c"
    "${M64P_SRC}/plugin/dummy_video.c"
    "${M64P_SRC}/plugin/dummy_audio.c"
    "${M64P_SRC}/plugin/dummy_input.c"
    "${M64P_SRC}/plugin/dummy_rsp.c"
    "${M64P_ROOT}/subprojects/md5/md5.c"
    "${M64P_ROOT}/subprojects/minizip/ioapi.c"
    "${M64P_ROOT}/subprojects/minizip/zip.c"
    "${M64P_ROOT}/subprojects/minizip/unzip.c"
    "${M64P_SRC}/osal/dynamiclib_win32.c"
    "${M64P_SRC}/osal/files_win32.c"
    "${SHIM}/sdl_shim.c"
    "${SHIM}/upstream_stubs.c"
)

# zlib is compiled straight into m64p_core (same archive) so that minizip /
# savestates can reference its symbols, but with hidden visibility so zlib's
# public symbols (notably crc32, which mGBA also exports) do not leak into the
# final link and collide with another core.
file(GLOB N64_ZLIB_SRCS "${ZLIB}/*.c")
list(APPEND M64P_CORE_SRCS ${N64_ZLIB_SRCS})
set_source_files_properties(${N64_ZLIB_SRCS} PROPERTIES COMPILE_OPTIONS -fvisibility=hidden)
add_library(m64p_core STATIC ${M64P_CORE_SRCS})
target_include_directories(m64p_core PUBLIC
    "${M64P_SRC}"
    "${M64P_API}"
    "${M64P_MAIN}"
    "${SHIM}"
    "${ZLIB}"
    "${M64P_ROOT}/subprojects"
    "${M64P_ROOT}/subprojects/md5"
    "${M64P_ROOT}/subprojects/xxhash"
)
target_compile_definitions(m64p_core PRIVATE -DM64P_PARALLEL -DWIN32)
set_target_properties(m64p_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# -----------------------------------------------------------------------------
# x86-64 dynarec
#
# Without this the R4300 CPU runs through the C interpreter (cached_interp.c),
# which is several times slower than a JIT and is the main reason N64 games
# stutter.  Upstream calls this DYNAREC = x86_64; NEW_DYNAREC is ARM-only and
# does not apply here.
#
# Two prerequisites:
#   * gen_asm_defines produces asm_defines_nasm.h.  Upstream scrapes those
#     offsets out of a compiled object with awk; gen_asm_defines.c derives
#     them directly with offsetof(), so no awk is needed.
#   * nasm assembles dyna_start.asm, the entry stub generated code runs from.
#     It #includes the header produced above.
# -----------------------------------------------------------------------------
find_program(NASM_EXE nasm)
if(NOT NASM_EXE)
    find_program(NASM_EXE nasm
        PATHS "C:/Program Files/NASM" "C:/mingw64/bin" "C:/msys64/mingw64/bin"
        NO_DEFAULT_PATH)
endif()

# Toggleable so the dynarec can be A/B tested against the C interpreter.
option(N64_DYNAREC "Build the x86-64 R4300 dynarec (much faster, needs nasm)" ON)

if(N64_DYNAREC AND CMAKE_SIZEOF_VOID_P EQUAL 8
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64" AND NASM_EXE)

    set(ASM_DEFINES_HDR "${CMAKE_CURRENT_BINARY_DIR}/asm_defines_nasm.h")
    set(DYNA_START_OBJ "${CMAKE_CURRENT_BINARY_DIR}/dyna_start_x86_64.o")

    add_executable(gen_asm_defines_build "${CMAKE_CURRENT_SOURCE_DIR}/gen_asm_defines.c")
    target_include_directories(gen_asm_defines_build PRIVATE "${M64P_SRC}")
    set_target_properties(gen_asm_defines_build PROPERTIES OUTPUT_NAME gen_asm_defines)

    add_custom_command(
        OUTPUT "${ASM_DEFINES_HDR}"
        COMMAND gen_asm_defines_build "${ASM_DEFINES_HDR}"
        DEPENDS gen_asm_defines_build
        COMMENT "Generating asm_defines_nasm.h for the x86-64 dynarec"
        VERBATIM)

    add_custom_command(
        OUTPUT "${DYNA_START_OBJ}"
        COMMAND "${NASM_EXE}" -f win64
                -I "${CMAKE_CURRENT_BINARY_DIR}/"
                -I "${M64P_SRC}/device/r4300/x86_64/"
                "${M64P_SRC}/device/r4300/x86_64/dyna_start.asm"
                -o "${DYNA_START_OBJ}"
        DEPENDS "${ASM_DEFINES_HDR}"
                "${M64P_SRC}/device/r4300/x86_64/dyna_start.asm"
        COMMENT "Assembling dyna_start.asm with nasm"
        VERBATIM)

    target_sources(m64p_core PRIVATE
        "${M64P_SRC}/device/r4300/recomp.c"
        "${M64P_SRC}/device/r4300/x86_64/assemble.c"
        "${M64P_SRC}/device/r4300/x86_64/dynarec.c"
        "${M64P_SRC}/device/r4300/x86_64/regcache.c"
        "${DYNA_START_OBJ}")
    target_include_directories(m64p_core PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}"
        "${M64P_SRC}/device/r4300/x86_64")
    target_compile_definitions(m64p_core PRIVATE -DDYNAREC)
    message(STATUS "N64: x86-64 dynarec enabled (nasm: ${NASM_EXE})")
else()
    message(WARNING
        "N64: x86-64 dynarec DISABLED (N64_DYNAREC=${N64_DYNAREC}, nasm found="
        "${NASM_EXE}, arch=${CMAKE_SYSTEM_PROCESSOR}). The R4300 falls back to "
        "the C interpreter, which is several times slower and causes stutter.")
endif()

# -----------------------------------------------------------------------------
# Nintendall adapter (n64_core)
# -----------------------------------------------------------------------------
add_library(n64_core STATIC
    n64_core.cpp
    n64_glue.c
    n64_rsp.c
)
target_link_libraries(n64_core PRIVATE
    emu_framework
    m64p_core
    m64p_rsp_cxd4
    angrylion
)
target_include_directories(n64_core PRIVATE
    "${M64P_SRC}"
    "${M64P_API}"
    "${RSP_CXD4}"
    "${ANG}"
    "${ZLIB}"
)
target_compile_definitions(n64_core PRIVATE -DWIN32)
set_target_properties(n64_core PROPERTIES POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

target_compile_definitions(emu_framework PUBLIC EMU_ENABLE_N64)

set(EMU_CORE_TARGETS ${EMU_CORE_TARGETS} n64_core PARENT_SCOPE)
