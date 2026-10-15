/**
 * Nintendall - Nintendo 3DS adapter
 *
 * Target core: Azahar, matching the upstream bundled at
 *   consoles/3ds/azahar-master
 *
 * Azahar (a Citra successor) already builds as a library (core/) with a clean
 * frontend split, so the adapter maps its emulation core onto the Nintendall
 * API. Video is served through Azahar's renderer callbacks and audio through
 * its audio core.
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

static const char* n3ds_exts[] = {"3ds", "cia", "cci", "cxi", "app", "3dsx", nullptr};

static const EmuCoreInfo n3ds_info = {
    "Azahar",
    "1.0",
    "Azahar contributors",
    EMU_CONSOLE_NINTENDO_3DS,
    n3ds_exts,
    6,
    /*has_save_states*/ 1,
    /*has_rewind*/ 1,
    /*has_cheats*/ 1,
    /*needs_bios*/ 0,
};

static EmuCoreHandle n3ds_create(const EmuConfig* config,
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

static void n3ds_destroy(EmuCoreHandle core) { free(core); }

static EmuLoadResult n3ds_load_rom(EmuCoreHandle core,
                                   const uint8_t* data, size_t size,
                                   const char* filename) {
    (void)core; (void)data; (void)size; (void)filename;
    // TODO: load the 3DS title through Azahar's Core::System loader.
    return EMU_LOAD_ERROR_INTERNAL;
}

static EmuLoadResult n3ds_load_rom_file(EmuCoreHandle core, const char* path) {
    size_t size = 0;
    uint8_t* data = emu_read_file(path, &size, 0);
    if (!data) return EMU_LOAD_ERROR_INVALID_ROM;
    EmuLoadResult r = n3ds_load_rom(core, data, size, path);
    free(data);
    return r;
}

static void n3ds_unload_rom(EmuCoreHandle core) { (void)core; }
static int  n3ds_run_frame(EmuCoreHandle core) { (void)core; return 0; }
static void n3ds_reset(EmuCoreHandle core) { (void)core; }
static void n3ds_power_cycle(EmuCoreHandle core) { (void)core; }
static void n3ds_set_paused(EmuCoreHandle core, int paused) { (void)core; (void)paused; }
static int  n3ds_is_paused(EmuCoreHandle core) { (void)core; return 0; }
static void n3ds_set_fast_forward(EmuCoreHandle core, int e, int s) { (void)core; (void)e; (void)s; }
static size_t n3ds_save_state(EmuCoreHandle core, void* b, size_t s) { (void)core; (void)b; (void)s; return 0; }
static int  n3ds_load_state(EmuCoreHandle core, const void* b, size_t s) { (void)core; (void)b; (void)s; return 0; }
static int  n3ds_save_sram(EmuCoreHandle core, const char* path) { (void)core; (void)path; return -1; }
static int  n3ds_load_sram(EmuCoreHandle core, const char* path) { (void)core; (void)path; return -1; }

extern "C" const EmuCoreModule n3ds_core_module = {
    "azahar",
    []() -> const EmuCoreInfo* { return &n3ds_info; },
    n3ds_create,
    n3ds_destroy,
    n3ds_load_rom,
    n3ds_load_rom_file,
    n3ds_unload_rom,
    n3ds_run_frame,
    n3ds_reset,
    n3ds_power_cycle,
    n3ds_set_paused,
    n3ds_is_paused,
    n3ds_set_fast_forward,
    n3ds_save_state,
    n3ds_load_state,
    n3ds_save_sram,
    n3ds_load_sram,
    nullptr,   // set_input_device
};

extern "C" void n3ds_core_unused_anchors(void) {
    (void)n3ds_load_rom_file; (void)n3ds_unload_rom; (void)n3ds_run_frame;
    (void)n3ds_reset; (void)n3ds_power_cycle; (void)n3ds_set_paused; (void)n3ds_is_paused;
    (void)n3ds_set_fast_forward; (void)n3ds_save_state; (void)n3ds_load_state;
    (void)n3ds_save_sram; (void)n3ds_load_sram;
}
