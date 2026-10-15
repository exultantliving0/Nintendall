/**
 * Nintendall - Core Registry implementation
 *
 * Core module symbols are conditionally referenced based on the EMU_ENABLE_*
 * compile definitions set by CMake. This keeps the registry in sync with the
 * cores that were actually built and linked.
 */

#include "core_registry.h"
#include <stddef.h>

// -----------------------------------------------------------------------------
// Core module symbols (defined by each core wrapper)
// -----------------------------------------------------------------------------

#ifdef EMU_ENABLE_NES
extern const EmuCoreModule nes_core_module;
#endif
#ifdef EMU_ENABLE_SNES
extern const EmuCoreModule snes_core_module;
#endif
#ifdef EMU_ENABLE_GB
extern const EmuCoreModule gb_core_module;
#endif
#ifdef EMU_ENABLE_NDS
extern const EmuCoreModule nds_core_module;
#endif
#ifdef EMU_ENABLE_N64
extern const EmuCoreModule n64_core_module;
#endif
#ifdef EMU_ENABLE_GCWII
extern const EmuCoreModule gcwii_core_module;
#endif
#ifdef EMU_ENABLE_3DS
extern const EmuCoreModule n3ds_core_module;
#endif

// -----------------------------------------------------------------------------
// Registry table
// -----------------------------------------------------------------------------

static const EmuCoreModule* const g_modules[] = {
#ifdef EMU_ENABLE_NES
    &nes_core_module,
#endif
#ifdef EMU_ENABLE_SNES
    &snes_core_module,
#endif
#ifdef EMU_ENABLE_GB
    &gb_core_module,
#endif
#ifdef EMU_ENABLE_NDS
    &nds_core_module,
#endif
#ifdef EMU_ENABLE_N64
    &n64_core_module,
#endif
#ifdef EMU_ENABLE_GCWII
    &gcwii_core_module,
#endif
#ifdef EMU_ENABLE_3DS
    &n3ds_core_module,
#endif
    NULL
};

static const int g_module_count =
    (int)(sizeof(g_modules) / sizeof(g_modules[0])) - 1;  // exclude NULL sentinel

// -----------------------------------------------------------------------------
// API
// -----------------------------------------------------------------------------

int emu_registry_count(void) {
    return g_module_count;
}

const EmuCoreModule* emu_registry_get(int index) {
    if (index < 0 || index >= g_module_count) return NULL;
    return g_modules[index];
}

EmuConsoleType emu_registry_module_console(const EmuCoreModule* module) {
    if (!module || !module->get_info) return EMU_CONSOLE_UNKNOWN;
    const EmuCoreInfo* info = module->get_info();
    return info ? info->console : EMU_CONSOLE_UNKNOWN;
}

const EmuCoreModule* emu_registry_find_for_console(EmuConsoleType console) {
    for (int i = 0; i < g_module_count; i++) {
        const EmuCoreModule* m = g_modules[i];
        if (!m) continue;
        const EmuCoreInfo* info = m->get_info ? m->get_info() : NULL;
        if (!info) continue;

        // Exact match
        if (info->console == console) return m;

        // GB/GBC/GBA share the mGBA-style multi-system core
        if ((console == EMU_CONSOLE_GAMEBOY ||
             console == EMU_CONSOLE_GAMEBOY_COLOR ||
             console == EMU_CONSOLE_GAMEBOY_ADVANCE) &&
            (info->console == EMU_CONSOLE_GAMEBOY ||
             info->console == EMU_CONSOLE_GAMEBOY_COLOR ||
             info->console == EMU_CONSOLE_GAMEBOY_ADVANCE)) {
            return m;
        }

        // GameCube and Wii share the Dolphin core
        if ((console == EMU_CONSOLE_GAMECUBE || console == EMU_CONSOLE_WII) &&
            (info->console == EMU_CONSOLE_GAMECUBE || info->console == EMU_CONSOLE_WII)) {
            return m;
        }
    }
    return NULL;
}

int emu_registry_available_consoles(EmuConsoleType* out, int max) {
    int n = 0;
    for (int i = 0; i < g_module_count && n < max; i++) {
        const EmuCoreModule* m = g_modules[i];
        if (!m) continue;
        const EmuCoreInfo* info = m->get_info ? m->get_info() : NULL;
        if (!info) continue;
        out[n++] = info->console;

        // A single GB-family core reports one console; expose the family too.
        if (info->console == EMU_CONSOLE_GAMEBOY_ADVANCE && n + 2 < max) {
            out[n++] = EMU_CONSOLE_GAMEBOY_COLOR;
            out[n++] = EMU_CONSOLE_GAMEBOY;
        }
    }
    return n;
}