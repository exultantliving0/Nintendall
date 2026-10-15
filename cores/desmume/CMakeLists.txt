# =============================================================================
# Nintendo DS core (DeSmuME) - adapter scaffold
#
# The Nintendall adapter is in place, but the upstream DeSmuME sources are not
# yet wired into this build. See cores/desmume/upstream/src/frontend/libretro for
# the load / run / state calls that need to be mapped onto the Nintendall API.
# =============================================================================

set(DESMUME_DEFAULT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/upstream")

if(NOT EXISTS "${DESMUME_DEFAULT_DIR}/src/NDSSystem.cpp")
    if(EXISTS "${CMAKE_SOURCE_DIR}/../consoles/Ds/desmume-master/desmume/src/NDSSystem.cpp")
        set(DESMUME_DEFAULT_DIR "${CMAKE_SOURCE_DIR}/../consoles/Ds/desmume-master/desmume")
    endif()
endif()

set(DESMUME_ROOT "${DESMUME_DEFAULT_DIR}" CACHE PATH "Path to the DeSmuME source tree")

add_library(nds_core STATIC nds_core.cpp)
target_link_libraries(nds_core PRIVATE emu_framework)

if(EXISTS "${DESMUME_ROOT}/src/NDSSystem.cpp")
    message(STATUS "NDS core: DeSmuME found at ${DESMUME_ROOT}")
    # TODO: add the DeSmuME sources here, e.g.
    #   file(GLOB_RECURSE DESMUME_SOURCES "${DESMUME_ROOT}/src/*.cpp"
    #        "${DESMUME_ROOT}/src/*.c")
    #   add_library(desmume_core STATIC ${DESMUME_SOURCES})
    #   target_include_directories(desmume_core PUBLIC "${DESMUME_ROOT}/src")
    #   target_link_libraries(nds_core PRIVATE desmume_core)
    message(WARNING
        "DeSmuME sources were found, but wiring them up is still a TODO in "
        "cores/desmume/CMakeLists.txt. Building the placeholder adapter only.")
else()
    message(STATUS "NDS core: DeSmuME not found; building placeholder adapter.")
endif()

target_compile_definitions(emu_framework PUBLIC EMU_ENABLE_NDS)

set(EMU_CORE_TARGETS ${EMU_CORE_TARGETS} nds_core PARENT_SCOPE)
