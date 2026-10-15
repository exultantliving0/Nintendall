/**
 * Nintendall - GameCube / Wii adapter
 *
 * Target core: Dolphin, matching the upstream bundled at
 *   consoles/gamecube and wii/dolphin-master
 *
 * Dolphin already emulates both GameCube and Wii, so one module covers both
 * console types (the registry groups them). Dolphin's own build is CMake-based
 * and produces a large set of libraries (Core, VideoCommon, ...); the adapter
 * below is the Nintendall API shell that those libraries plug into.
 */

#include "emu_api.h"
#include "emu_util.h"
#include "core_registry.h"

#include <cstring>
#include <cstdlib>

struct EmuCore {
    EmuConfig config;
    EmuVideoCallback video_cb = nullptr;
    EmuAudioCallback audio_cb = nullptr;
    EmuInputPollCallback input_poll_cb = nullptr;
    EmuInputStateCallback input_state_cb = nullptr;
    void* userdata = nullptr;
};

static const char* gcwii_exts[] = {"iso", "gcm", "gcz", "rvz", "wia", "wbfs", "dol", "elf", nullptr};

static const EmuCoreInfo gcwii_info = {
    "Dolphin",
    "5.0",
    "Dolphin team",
    EMU_CONSOLE_GAMECUBE,   // registry matches GameCube *and* Wii cores
    gcwii_exts,
    8,
    /*has_save_states*/ 1,
    /*has_rewind*/ 1,
    /*has_cheats*/ 1,
    /*needs_bios*/ 0,
};

static EmuCoreHandle gcwii_create(const EmuConfig* config,
                                  EmuVideoCallback video_cb,
                                  EmuAudioCallback audio_cb,
                                  EmuInputPollCallback input_poll_cb,
                                  EmuInputStateCallback input_state_cb,
                                  void* userdata) {
    EmuCore* c = (EmuCore*)calloc(1, sizeof(EmuCore));
    if (!c) return nullptr;
    if (config) c->config = *config; else emu_config_default(&c->config);
    c->video_cb = video_cb;
    c->audio_cb = audio_cb;
    c->input_poll_cb = input_poll_cb;
    c->input_state_cb = input_state_cb;
    c->userdata = userdata;
    return c;
}

static void gcwii_destroy(EmuCoreHandle core) { free(core); }

static EmuLoadResult gcwii_load_rom(EmuCoreHandle core,
                                    const uint8_t* data, size_t size,
                                    const char* filename) {
    (void)core; (void)data; (void)size; (void)filename;
    // TODO: hand the disc image to Dolphin's Core::Run / BootManager.
    return EMU_LOAD_ERROR_INTERNAL;
}

static EmuLoadResult gcwii_load_rom_file(EmuCoreHandle core, const char* path) {
    size_t size = 0;
    uint8_t* data = emu_read_file(path, &size, 0);
    if (!data) return EMU_LOAD_ERROR_INVALID_ROM;
    EmuLoadResult r = gcwii_load_rom(core, data, size, path);
    free(data);
    return r;
}

static void gcwii_unload_rom(EmuCoreHandle core) { (void)core; }
static int  gcwii_run_frame(EmuCoreHandle core) { (void)core; return 0; }
static void gcwii_reset(EmuCoreHandle core) { (void)core; }
static void gcwii_power_cycle(EmuCoreHandle core) { (void)core; }
static void gcwii_set_paused(EmuCoreHandle core, int paused) { (void)core; (void)paused; }
static int  gcwii_is_paused(EmuCoreHandle core) { (void)core; return 0; }
static void gcwii_set_fast_forward(EmuCoreHandle core, int e, int s) { (void)core; (void)e; (void)s; }
static size_t gcwii_save_state(EmuCoreHandle core, void* b, size_t s) { (void)core; (void)b; (void)s; return 0; }
static int  gcwii_load_state(EmuCoreHandle core, const void* b, size_t s) { (void)core; (void)b; (void)s; return 0; }
static int  gcwii_save_sram(EmuCoreHandle core, const char* path) { (void)core; (void)path; return -1; }
static int  gcwii_load_sram(EmuCoreHandle core, const char* path) { (void)core; (void)path; return -1; }

extern "C" const EmuCoreModule gcwii_core_module = {
    "dolphin",
    []() -> const EmuCoreInfo* { return &gcwii_info; },
    gcwii_create,
    gcwii_destroy,
    gcwii_load_rom,
    gcwii_load_rom_file,
    gcwii_unload_rom,
    gcwii_run_frame,
    gcwii_reset,
    gcwii_power_cycle,
    gcwii_set_paused,
    gcwii_is_paused,
    gcwii_set_fast_forward,
    gcwii_save_state,
    gcwii_load_state,
    gcwii_save_sram,
    gcwii_load_sram,
    nullptr,   // set_input_device
};

extern "C" void gcwii_core_unused_anchors(void) {
    (void)gcwii_load_rom_file; (void)gcwii_unload_rom; (void)gcwii_run_frame;
    (void)gcwii_reset; (void)gcwii_power_cycle; (void)gcwii_set_paused; (void)gcwii_is_paused;
    (void)gcwii_set_fast_forward; (void)gcwii_save_state; (void)gcwii_load_state;
    (void)gcwii_save_sram; (void)gcwii_load_sram;
}
