/**
 * Nintendall - Nintendo DS adapter
 *
 * Target core: DeSmuME, matching the upstream bundled at
 *   consoles/Ds/desmume-master/desmume
 *
 * DeSmuME's libretro frontend (desmume/src/frontend/libretro) shows how to
 * drive the core (NDS_Init / NDS_LoadROM / NDS_Reset / NDS_exec) and how to
 * fetch the two screens and audio. This adapter is the Nintendall API shell for
 * that work; the CMakeLists notes the integration steps.
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

static const char* nds_exts[] = {"nds", "dsi", "ids", "srl", "zip", nullptr};

static const EmuCoreInfo nds_info = {
    "DeSmuME",
    "0.9.x",
    "DeSmuME team",
    EMU_CONSOLE_NINTENDO_DS,
    nds_exts,
    5,
    /*has_save_states*/ 1,
    /*has_rewind*/ 1,
    /*has_cheats*/ 1,
    /*needs_bios*/ 1,
};

static EmuCoreHandle nds_create(const EmuConfig* config,
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

static void nds_destroy(EmuCoreHandle core) { free(core); }

static EmuLoadResult nds_load_rom(EmuCoreHandle core,
                                  const uint8_t* data, size_t size,
                                  const char* filename) {
    (void)core; (void)data; (void)size; (void)filename;
    // TODO: NDS_LoadROM + NDS_Reset via the DeSmuME core API.
    return EMU_LOAD_ERROR_INTERNAL;
}

static EmuLoadResult nds_load_rom_file(EmuCoreHandle core, const char* path) {
    size_t size = 0;
    uint8_t* data = emu_read_file(path, &size, 0);
    if (!data) return EMU_LOAD_ERROR_INVALID_ROM;
    EmuLoadResult r = nds_load_rom(core, data, size, path);
    free(data);
    return r;
}

static void nds_unload_rom(EmuCoreHandle core) { (void)core; }
static int  nds_run_frame(EmuCoreHandle core) { (void)core; return 0; }
static void nds_reset(EmuCoreHandle core) { (void)core; }
static void nds_power_cycle(EmuCoreHandle core) { (void)core; }
static void nds_set_paused(EmuCoreHandle core, int paused) { (void)core; (void)paused; }
static int  nds_is_paused(EmuCoreHandle core) { (void)core; return 0; }
static void nds_set_fast_forward(EmuCoreHandle core, int e, int s) { (void)core; (void)e; (void)s; }
static size_t nds_save_state(EmuCoreHandle core, void* b, size_t s) { (void)core; (void)b; (void)s; return 0; }
static int  nds_load_state(EmuCoreHandle core, const void* b, size_t s) { (void)core; (void)b; (void)s; return 0; }
static int  nds_save_sram(EmuCoreHandle core, const char* path) { (void)core; (void)path; return -1; }
static int  nds_load_sram(EmuCoreHandle core, const char* path) { (void)core; (void)path; return -1; }

extern "C" const EmuCoreModule nds_core_module = {
    "desmume",
    []() -> const EmuCoreInfo* { return &nds_info; },
    nds_create,
    nds_destroy,
    nds_load_rom,
    nds_load_rom_file,
    nds_unload_rom,
    nds_run_frame,
    nds_reset,
    nds_power_cycle,
    nds_set_paused,
    nds_is_paused,
    nds_set_fast_forward,
    nds_save_state,
    nds_load_state,
    nds_save_sram,
    nds_load_sram,
    nullptr,   // set_input_device
};

extern "C" void nds_core_unused_anchors(void) {
    (void)nds_load_rom_file; (void)nds_unload_rom; (void)nds_run_frame;
    (void)nds_reset; (void)nds_power_cycle; (void)nds_set_paused; (void)nds_is_paused;
    (void)nds_set_fast_forward; (void)nds_save_state; (void)nds_load_state;
    (void)nds_save_sram; (void)nds_load_sram;
}
