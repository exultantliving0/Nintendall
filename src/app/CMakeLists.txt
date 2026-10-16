# =============================================================================
# Nintendall - the Win32 GUI application (the one executable users run)
# =============================================================================

add_executable(nintendall WIN32
    main.cpp
    input_config.cpp
    input_dialog.cpp
    controller_layouts.cpp
)

target_link_libraries(nintendall PRIVATE
    emu_framework
    ${EMU_CORE_TARGETS}
)

if(WIN32)
    target_link_libraries(nintendall PRIVATE comdlg32 winmm user32 gdi32)
endif()

if(MINGW)
    # The app uses the Unicode entry point (wWinMain), which MinGW only wires
    # up when built with -municode.
    target_compile_options(nintendall PRIVATE -municode)
    target_link_options(nintendall PRIVATE -municode)
    # Bundling multiple cores means multiple zlib derivations can export the
    # same symbols (notably crc32).  All implementations are the standard
    # CRC-32, so allow the linker to pick the first definition.
    target_link_options(nintendall PRIVATE -Wl,--allow-multiple-definition)
endif()

set_target_properties(nintendall PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME nintendall
)

if(EMU_CORE_TARGETS STREQUAL "")
    message(WARNING
        "No emulator cores are enabled, so the frontend will build but cannot "
        "run anything. Enable at least a console core, e.g. NES or SNES.")
endif()
