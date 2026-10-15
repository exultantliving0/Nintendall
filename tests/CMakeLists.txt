# =============================================================================
# Headless core smoke test (opt-in with -DBUILD_SMOKE_TEST=ON)
#
# Console app that loads a ROM through the compiled-in cores and reports
# whether video/audio/save-state all worked. No window, no platform libs.
# =============================================================================

add_executable(nintendall_test smoke_test.cpp)

target_link_libraries(nintendall_test PRIVATE
    emu_framework
    ${EMU_CORE_TARGETS}
)

set_target_properties(nintendall_test PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

if(MINGW)
    target_link_options(nintendall_test PRIVATE -Wl,--allow-multiple-definition)
endif()
