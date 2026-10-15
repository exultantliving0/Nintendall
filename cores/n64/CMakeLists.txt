# =============================================================================
# Nintendo 64 core (mupen64plus-core + mupen64plus-rsp-cxd4 + angrylion)
#
# The upstream trees are vendored at cores/n64/upstream/.  The Nintendall
# adapter lives in this folder: n64_core.cpp, n64_glue.{h,c}, n64_rsp.c and
# shim/.
#
# Build notes
#   - mupen64plus-core is built with the *interpreter* (no dynarec, no nasm)
#     to keep the first working build simple.  Re-add -DDYNAREC + the
#     asm_defines/nasm steps later for speed.
#   - angrylion (software RDP) runs single-threaded (parallel = false) so no
#     pthread dependency is required.
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
add_library(angrylion STATIC
    "${ANG}/n64video.c"
)
target_include_directories(angrylion PUBLIC "${ANG}")
set_target_properties(angrylion PROPERTIES POSITION_INDEPENDENT_CODE ON
    C_STANDARD 99 C_STANDARD_REQUIRED ON)

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
