/**
 * Nintendall - Core Registry
 *
 * Each emulator core exposes a single `EmuCoreModule` symbol. The registry
 * collects the cores that were compiled in and lets the frontend look up the
 * right core for a detected console type.
 *
 * Adding a new console means:
 *   1. write a wrapper that defines an EmuCoreModule
 *   2. add its symbol to the table in core_registry.cpp
 *   3. enable the corresponding CMake option
 */

#ifndef CORE_REGISTRY_H
#define CORE_REGISTRY_H

#include "emu_api.h"

#ifdef __cplusplus
extern "C" {
#endif

// -----------------------------------------------------------------------------
// Per-core operation signatures (the module "vtable")
// -----------------------------------------------------------------------------

typedef const EmuCoreInfo* (*EmuCoreGetInfoFn)(void);

typedef EmuCoreHandle (*EmuCoreCreateFn)(const EmuConfig* config,
                                         EmuVideoCallback video_cb,
                                         EmuAudioCallback audio_cb,
                                         EmuInputPollCallback input_poll_cb,
                                         EmuInputStateCallback input_state_cb,
                                         void* userdata);

typedef void            (*EmuCoreDestroyFn)(EmuCoreHandle core);
typedef EmuLoadResult   (*EmuCoreLoadRomFn)(EmuCoreHandle core, const uint8_t* data,
                                            size_t size, const char* filename);
typedef EmuLoadResult   (*EmuCoreLoadRomFileFn)(EmuCoreHandle core, const char* path);
typedef void            (*EmuCoreUnloadRomFn)(EmuCoreHandle core);
typedef int             (*EmuCoreRunFrameFn)(EmuCoreHandle core);
typedef void            (*EmuCoreResetFn)(EmuCoreHandle core);
typedef void            (*EmuCorePowerCycleFn)(EmuCoreHandle core);
typedef void            (*EmuCoreSetPausedFn)(EmuCoreHandle core, int paused);
typedef int             (*EmuCoreIsPausedFn)(EmuCoreHandle core);
typedef void            (*EmuCoreSetFastForwardFn)(EmuCoreHandle core, int enabled, int speed);
typedef size_t          (*EmuCoreSaveStateFn)(EmuCoreHandle core, void* buffer, size_t buffer_size);
typedef int             (*EmuCoreLoadStateFn)(EmuCoreHandle core, const void* buffer, size_t size);
typedef int             (*EmuCoreSaveSramFn)(EmuCoreHandle core, const char* path);
typedef int             (*EmuCoreLoadSramFn)(EmuCoreHandle core, const char* path);
typedef void            (*EmuCoreSetInputDeviceFn)(EmuCoreHandle core, unsigned port,
                                                   EmuDeviceType device);

typedef struct EmuCoreModule {
    const char* id;                 // stable id, e.g. "nestopia"

    EmuCoreGetInfoFn          get_info;
    EmuCoreCreateFn           create;
    EmuCoreDestroyFn          destroy;
    EmuCoreLoadRomFn          load_rom;
    EmuCoreLoadRomFileFn      load_rom_file;
    EmuCoreUnloadRomFn        unload_rom;
    EmuCoreRunFrameFn         run_frame;
    EmuCoreResetFn            reset;
    EmuCorePowerCycleFn       power_cycle;
    EmuCoreSetPausedFn        set_paused;
    EmuCoreIsPausedFn         is_paused;
    EmuCoreSetFastForwardFn   set_fast_forward;
    EmuCoreSaveStateFn        save_state;
    EmuCoreLoadStateFn        load_state;
    EmuCoreSaveSramFn         save_sram;
    EmuCoreLoadSramFn         load_sram;
    EmuCoreSetInputDeviceFn   set_input_device;

    // Optional capability bits; see EMU_CORE_FLAG_*.  Omitted/zero for cores
    // that follow the frontend's default "run one frame, frontend paces it"
    // contract.
    uint32_t                  flags;
} EmuCoreModule;

// The core owns real-time pacing: run_frame() already blocks internally for one
// console refresh period (e.g. mupen64plus's speed limiter).  The frontend must
// not add its own frame-rate sleep on top of it, and must not derive pacing from
// the number of audio samples it happens to drain.
#define EMU_CORE_FLAG_SELF_PACED  0x1u

// =============================================================================
// Registry API
// =============================================================================

// Returns number of registered (compiled-in) cores.
int emu_registry_count(void);

// Returns module at index, or NULL.
const EmuCoreModule* emu_registry_get(int index);

// Find the best core for a console type, or NULL if none compiled in.
const EmuCoreModule* emu_registry_find_for_console(EmuConsoleType console);

// Convenience: console emulated by a module.
EmuConsoleType emu_registry_module_console(const EmuCoreModule* module);

// Fill an array (size max) with all consoles that have a core available.
// Returns the number written.
int emu_registry_available_consoles(EmuConsoleType* out, int max);

#ifdef __cplusplus
}
#endif

#endif // CORE_REGISTRY_H
