# =============================================================================
# GameCube / Wii core (Dolphin) - adapter scaffold
#
# The Nintendall adapter is in place, but the upstream Dolphin sources are not
# yet wired into this build. Dolphin is itself a large CMake project; the
# recommended integration is to add its "Core" and "VideoCommon" libraries here
# and drive them from the Nintendall adapter, rather than pulling in the Qt GUI.
# =============================================================================

set(DOLPHIN_DEFAULT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/upstream")

if(NOT EXISTS "${DOLPHIN_DEFAULT_DIR}/Source/Core/Core/Core.h")
    if(EXISTS "${CMAKE_SOURCE_DIR}/../consoles/gamecube and wii/dolphin-master/Source/Core/Core/Core.h")
        set(DOLPHIN_DEFAULT_DIR "${CMAKE_SOURCE_DIR}/../consoles/gamecube and wii/dolphin-master")
    endif()
endif()

set(DOLPHIN_ROOT "${DOLPHIN_DEFAULT_DIR}" CACHE PATH "Path to the Dolphin source tree")

add_library(gcwii_core STATIC gcwii_core.cpp)
target_link_libraries(gcwii_core PRIVATE emu_framework)

if(EXISTS "${DOLPHIN_ROOT}/Source/Core/Core/Core.h")
    message(STATUS "GC/Wii core: Dolphin found at ${DOLPHIN_ROOT}")
    message(WARNING
        "Dolphin's Core/VideoCommon libraries still need to be added to this "
        "CMakeLists and driven from the Nintendall adapter. "
        "Building the placeholder adapter only for now.")
else()
    message(STATUS "GC/Wii core: Dolphin not found; building placeholder adapter.")
endif()

target_compile_definitions(emu_framework PUBLIC EMU_ENABLE_GCWII)

set(EMU_CORE_TARGETS ${EMU_CORE_TARGETS} gcwii_core PARENT_SCOPE)
