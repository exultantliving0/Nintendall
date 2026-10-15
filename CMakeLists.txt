cmake_minimum_required(VERSION 3.20)

project(Nintendall
    VERSION 1.0.0
    DESCRIPTION "Many Nintendo systems in one app"
    LANGUAGES C CXX)

# =============================================================================
# Nintendall - one executable, many console cores.
#
# Layout
#   src/framework/   shared C API + framework (emu_api, registry, detect, ...)
#   src/app/         the Win32 GUI executable (main.cpp)
#   cores/<name>/    one adapter per console, plus its vendored upstream tree
#   tests/           optional headless smoke test
#   assets/          runtime data files
#   docs/            design notes
#   tools/           helper scripts
# =============================================================================

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY
        STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# =============================================================================
# Options
# =============================================================================

option(BUILD_NES_CORE    "Build NES (Nestopia) core"            ON)
option(BUILD_SNES_CORE   "Build SNES (bsnes) core"             ON)
option(BUILD_GBA_CORE    "Build GB/GBC/GBA (mGBA) core"        ON)
option(BUILD_DS_CORE     "Build NDS (DeSmuME) core"            OFF)
option(BUILD_N64_CORE    "Build N64 (mupen64plus + angrylion) core" OFF)
option(BUILD_GCWII_CORE  "Build GameCube/Wii (Dolphin) core"   OFF)
option(BUILD_3DS_CORE    "Build 3DS (Azahar) core"             OFF)
option(BUILD_FRONTEND    "Build the Nintendall app"            ON)
option(BUILD_SMOKE_TEST  "Build the headless core smoke test"  OFF)

# =============================================================================
# Platform settings
# =============================================================================

if(WIN32)
    add_compile_definitions(
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _CRT_SECURE_NO_WARNINGS
        _WIN32_WINNT=0x0601
    )
    list(APPEND PLATFORM_LIBS user32 gdi32 opengl32 shell32 ole32 comdlg32 winmm)
    if(MSVC)
        add_compile_options(/W3 /MP /bigobj)
    endif()
elseif(UNIX)
    list(APPEND PLATFORM_LIBS dl pthread m)
    if(APPLE)
        list(APPEND PLATFORM_LIBS "-framework OpenGL" "-framework Cocoa" "-framework IOKit")
    endif()
endif()

# =============================================================================
# Framework library (must exist before any core: every core links against it)
# =============================================================================

add_library(emu_framework STATIC
    src/framework/emu_api.cpp
    src/framework/rom_detect.cpp
    src/framework/save_state.cpp
    src/framework/core_registry.cpp
    src/framework/zip.cpp
)

target_include_directories(emu_framework PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/framework"
)

set_target_properties(emu_framework PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Accumulated list of core adapter targets (each core appends to this).
set(EMU_CORE_TARGETS "")

# =============================================================================
# Emulator cores
# =============================================================================

if(BUILD_NES_CORE)
    add_subdirectory(cores/nestopia)
endif()

if(BUILD_SNES_CORE)
    add_subdirectory(cores/bsnes)
endif()

if(BUILD_GBA_CORE)
    add_subdirectory(cores/mgba)
endif()

if(BUILD_DS_CORE)
    add_subdirectory(cores/desmume)
endif()

if(BUILD_N64_CORE)
    add_subdirectory(cores/n64)
endif()

if(BUILD_GCWII_CORE)
    add_subdirectory(cores/dolphin)
endif()

if(BUILD_3DS_CORE)
    add_subdirectory(cores/azahar)
endif()

# =============================================================================
# Frontend application
# =============================================================================

if(BUILD_FRONTEND)
    add_subdirectory(src/app)
endif()

# =============================================================================
# Smoke test (optional)
# =============================================================================

if(BUILD_SMOKE_TEST)
    add_subdirectory(tests)
endif()

# =============================================================================
# Install / packaging
# =============================================================================

if(BUILD_FRONTEND)
    install(TARGETS nintendall RUNTIME DESTINATION bin)
    install(FILES assets/config.ini DESTINATION bin/assets OPTIONAL)
endif()

include(CPack)
set(CPACK_PACKAGE_NAME "Nintendall")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Nintendall - many Nintendo systems in one app")
set(CPACK_GENERATOR "ZIP")
if(WIN32)
    set(CPACK_GENERATOR "ZIP;NSIS")
endif()
