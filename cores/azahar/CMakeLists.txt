# =============================================================================
# Nintendo 3DS core (Azahar) - adapter scaffold
#
# The Nintendall adapter is in place, but the upstream Azahar sources are not
# yet wired into this build. Azahar builds its emulation core into a library;
# add that here and drive it from the Nintendall adapter.
# =============================================================================

set(AZAHAR_DEFAULT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/upstream")

if(NOT EXISTS "${AZAHAR_DEFAULT_DIR}/src/core/core.h")
    if(EXISTS "${CMAKE_SOURCE_DIR}/../consoles/3ds/azahar-master/src/core/core.h")
        set(AZAHAR_DEFAULT_DIR "${CMAKE_SOURCE_DIR}/../consoles/3ds/azahar-master")
    endif()
endif()

set(AZAHAR_ROOT "${AZAHAR_DEFAULT_DIR}" CACHE PATH "Path to the Azahar source tree")

add_library(n3ds_core STATIC n3ds_core.cpp)
target_link_libraries(n3ds_core PRIVATE emu_framework)

if(EXISTS "${AZAHAR_ROOT}/src/core/core.h")
    message(STATUS "3DS core: Azahar found at ${AZAHAR_ROOT}")
    message(WARNING
        "Azahar's core library still needs to be added to this CMakeLists and "
        "driven from the Nintendall adapter. "
        "Building the placeholder adapter only for now.")
else()
    message(STATUS "3DS core: Azahar not found; building placeholder adapter.")
endif()

target_compile_definitions(emu_framework PUBLIC EMU_ENABLE_3DS)

set(EMU_CORE_TARGETS ${EMU_CORE_TARGETS} n3ds_core PARENT_SCOPE)
