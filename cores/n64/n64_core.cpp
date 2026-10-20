/**
 * Nintendall - Nintendo 64 adapter
 *
 * Wraps the vendored mupen64plus-core (classic x86_64 dynarec) +
 * mupen64plus-rsp-hle + angrylion (software RDP) behind the Nintendall
 * EmuCore C API, using the n64_glue layer (n64_glue.c / n64_rsp.c) to stand
 * mupen64plus up without its SDL frontend.
 *
 * Integration points
 *   - n64_glue_startup()      : CoreStartup + config + directories
 *   - n64_glue_load_rom()     : M64CMD_ROM_OPEN + attach_plugins()
 *   - n64_glue_start()        : spawns the emulation thread (M64CMD_EXECUTE)
 *   - n64_glue_frame()        : waits for one rendered frame, delivers video +
 *                               audio on the frontend thread (run_frame body)
 *   - n64_glue_set_input()    : N64 pad state, polled each frame from the
 *                               frontend's InputStateCallback
 *   - n64_glue_save/load_state: savestate job queued to the emulation thread
 *
 * Keyboard mapping (frontend InputStateCallback):
 *   arrows  = d-pad (+ analog stick), X = A, Z = B, S = X, A = Y,
 *   Q = L, W = R, Enter = Start, Shift = Select.
 *   While Shift is held: arrows = C-buttons, Q = L_C, W = R_C.
 *
 * struct EmuCore is only forward-declared in emu_api.h (the handle is an
 * opaque pointer); we complete it here, exactly as cores/mgba/gb_core.cpp
 * does.  The frame/video/audio callbacks have different signatures between
 * the EmuCore API and n64_glue, so small wrapper functions adapt them.
 */

#include "emu_api.h"
#include "emu_util.h"
#include "core_registry.h"

#include "n64_glue.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <cstdint>
#include <vector>

// EmuCore is forward-declared (opaque) in emu_api.h; complete it here.
struct EmuCore {
    EmuConfig config = {};

    EmuVideoCallback   video_cb        = nullptr;
    EmuAudioCallback   audio_cb        = nullptr;
    EmuInputStateCallback input_state_cb = nullptr;
    void*              userdata        = nullptr;

    bool started = false;   // n64_glue_start() has run
    bool loaded  = false;
    bool paused  = false;

    std::vector<uint8_t> rom_data;
};

// -----------------------------------------------------------------------------
// Callback adapters: EmuCore API <-> n64_glue API
// -----------------------------------------------------------------------------

static void n64_video_adapter(void* user, const void* pixels,
                              unsigned width, unsigned height, unsigned pitch)
{
    EmuCore* c = static_cast<EmuCore*>(user);
    if (c && c->video_cb)
        c->video_cb(c->userdata, static_cast<const uint32_t*>(pixels),
                    (int)width, (int)height, (int)pitch);
}

static void n64_audio_adapter(void* user, const int16_t* samples, unsigned frames)
{
    EmuCore* c = static_cast<EmuCore*>(user);
    if (c && c->audio_cb)
        c->audio_cb(c->userdata, samples, (int)frames);
}

// =============================================================================
// Lifecycle
// =============================================================================

static EmuCoreHandle n64_create(const EmuConfig* config,
                                EmuVideoCallback video_cb,
                                EmuAudioCallback audio_cb,
                                EmuInputPollCallback /*input_poll_cb*/,
                                EmuInputStateCallback input_state_cb,
                                void* userdata) {
    EmuCore* c = new (std::nothrow) EmuCore();
    if (!c) return nullptr;

    if (config) c->config = *config;
    else emu_config_default(&c->config);

    c->video_cb = video_cb;
    c->audio_cb = audio_cb;
    c->input_state_cb = input_state_cb;
    c->userdata = userdata;
    return c;
}

static void n64_destroy(EmuCoreHandle core) {
    if (!core) return;
    n64_glue_shutdown();
    delete static_cast<EmuCore*>(core);
}

// =============================================================================
// ROM loading
// =============================================================================

static EmuLoadResult n64_load_rom(EmuCoreHandle core,
                                  const uint8_t* data, size_t size,
                                  const char* /*filename*/) {
    EmuCore* c = static_cast<EmuCore*>(core);
    if (!c || !data || size < 4096)
        return EMU_LOAD_ERROR_INVALID_ROM;

    c->rom_data.assign(data, data + size);

    /* Config + battery saves live next to the frontend's other save data. */
    const char* data_path = c->config.save_dir && c->config.save_dir[0]
        ? c->config.save_dir : ".";
    if (n64_glue_startup(data_path) != 0)
        return EMU_LOAD_ERROR_INTERNAL;

    /* The glue calls the adapters with `user` == c, so they can recover c. */
    n64_glue_set_callbacks(c, n64_video_adapter, n64_audio_adapter);

    if (n64_glue_load_rom(c->rom_data.data(), c->rom_data.size()) != 0)
        return EMU_LOAD_ERROR_INTERNAL;

    if (n64_glue_start() != 0)
        return EMU_LOAD_ERROR_INTERNAL;

    c->started = true;
    c->loaded = true;
    return EMU_LOAD_OK;
}

static EmuLoadResult n64_load_rom_file(EmuCoreHandle core, const char* path) {
    if (!core || !path) return EMU_LOAD_ERROR_INVALID_ROM;
    EmuCore* c = static_cast<EmuCore*>(core);

    size_t n = 0;
    uint8_t* data = emu_read_file(path, &n, 0);
    if (!data) return EMU_LOAD_ERROR_INVALID_ROM;

    EmuLoadResult res = n64_load_rom(core, data, n, path);
    free(data);
    return res;
}

static void n64_unload_rom(EmuCoreHandle core) {
    EmuCore* c = static_cast<EmuCore*>(core);
    if (!c) return;
    n64_glue_shutdown();
    c->started = false;
    c->loaded = false;
    c->paused = false;
    c->rom_data.clear();
}

// =============================================================================
// Execution
// =============================================================================

static int n64_run_frame(EmuCoreHandle core) {
    EmuCore* c = static_cast<EmuCore*>(core);
    if (!c || !c->loaded) return 0;
    if (c->paused) return 1;

    /* Poll the frontend's keyboard state into an N64 pad. */
    uint32_t buttons = 0;
    int8_t stick_x = 0;
    int8_t stick_y = 0;

    if (c->input_state_cb) {
        auto down = [&](unsigned id) -> bool {
            return c->input_state_cb(c->userdata, 0, EMU_DEVICE_JOYPAD, 0, id) != 0;
        };

        bool shift = down(EMU_ID_SELECT);

        if (down(EMU_ID_A))      buttons |= 0x0080u;  // A_BUTTON
        if (down(EMU_ID_B))      buttons |= 0x0040u;  // B_BUTTON
        if (down(EMU_ID_Y))      buttons |= 0x0020u;  // Z_TRIG
        if (down(EMU_ID_C_UP) || down(EMU_ID_X)) buttons |= 0x0800u;  // U_CBUTTON
        if (down(EMU_ID_C_DOWN)) buttons |= 0x0400u;  // D_CBUTTON
        if (down(EMU_ID_C_LEFT)) buttons |= 0x0200u;  // L_CBUTTON
        if (down(EMU_ID_C_RIGHT)) buttons |= 0x0100u;  // R_CBUTTON
        if (down(EMU_ID_START))  buttons |= 0x0010u;  // START_BUTTON
        if (down(EMU_ID_L))      buttons |= 0x2000u;  // L_TRIG
        if (down(EMU_ID_R))      buttons |= 0x1000u;  // R_TRIG

        // D-pad (also sets analog stick)
        if (down(EMU_ID_UP))    { buttons |= 0x0008u; stick_y =  80; }
        if (down(EMU_ID_DOWN))  { buttons |= 0x0004u; stick_y = -80; }
        if (down(EMU_ID_LEFT))  { buttons |= 0x0002u; stick_x = -80; }
        if (down(EMU_ID_RIGHT)) { buttons |= 0x0001u; stick_x =  80; }

        // Joystick (overrides D-pad stick values when active)
        if (down(EMU_ID_JOYSTICK_UP))    stick_y =  80;
        if (down(EMU_ID_JOYSTICK_DOWN))  stick_y = -80;
        if (down(EMU_ID_JOYSTICK_LEFT))  stick_x = -80;
        if (down(EMU_ID_JOYSTICK_RIGHT)) stick_x =  80;
    }

    n64_glue_set_input(buttons, stick_x, stick_y);
    return n64_glue_frame();
}

static void n64_reset(EmuCoreHandle core) {
    if (core) n64_glue_reset();
}

static void n64_power_cycle(EmuCoreHandle core) {
    if (core) n64_glue_power_cycle();
}

static void n64_set_paused(EmuCoreHandle core, int paused) {
    if (core) static_cast<EmuCore*>(core)->paused = paused != 0;
}

static int n64_is_paused(EmuCoreHandle core) {
    return core ? static_cast<EmuCore*>(core)->paused : 0;
}

static void n64_set_fast_forward(EmuCoreHandle /*core*/,
                                 int /*enabled*/, int /*speed*/) {
    // The emulation thread is paced by the frontend via the frame handshake;
    // there is no separate fast-forward knob for now.
}

// =============================================================================
// Save states
// =============================================================================

static size_t n64_save_state(EmuCoreHandle core, void* buffer,
                             size_t buffer_size) {
    EmuCore* c = static_cast<EmuCore*>(core);
    if (!c || !c->loaded) return 0;
    return n64_glue_save_state(static_cast<uint8_t*>(buffer), buffer_size);
}

static int n64_load_state(EmuCoreHandle core, const void* buffer,
                          size_t size) {
    EmuCore* c = static_cast<EmuCore*>(core);
    if (!c || !c->loaded || !buffer || size == 0) return 0;
    return n64_glue_load_state(static_cast<const uint8_t*>(buffer), size) ? 1 : 0;
}

// =============================================================================
// Battery / SRAM
//
// mupen64plus persists battery-backed storage (SRAM/EEPROM/Flash) internally
// under <data>/save/<name>.sav, so the adapter has nothing to do here; report
// whether the cartridge has any via get_has_battery().
// =============================================================================

static int n64_save_sram(EmuCoreHandle /*core*/, const char* /*path*/) {
    return 0;
}

static int n64_load_sram(EmuCoreHandle /*core*/, const char* /*path*/) {
    return 0;
}

static void n64_set_input_device(EmuCoreHandle /*core*/, unsigned /*port*/,
                                 EmuDeviceType /*device*/) {
    // N64 uses a single standard joypad; no device switching.
}

// =============================================================================
// ROM info
// =============================================================================

static const char* n64_exts[] = { "z64", "n64", "v64", "zip", nullptr };

static const EmuCoreInfo n64_info = {
    "mupen64plus (angrylion)",
    "2.0",
    "mupen64plus team",
    EMU_CONSOLE_NINTENDO_64,
    n64_exts,
    4,
    /*has_save_states*/ 1,
    /*has_rewind*/      0,
    /*has_cheats*/      0,
    /*needs_bios*/      0,
};

static const EmuCoreInfo* n64_get_info(void) { return &n64_info; }

static int n64_get_rom_name(EmuCoreHandle core, char* out, size_t cap) {
    (void)core;
    if (!out || cap == 0) return 0;
    n64_glue_get_rom_name(out, cap);
    return (int)strlen(out);
}

static uint32_t n64_get_crc32(EmuCoreHandle core) {
    (void)core;
    return n64_glue_get_crc32();
}

static int n64_get_rom_size(EmuCoreHandle core) {
    (void)core;
    return (int)n64_glue_get_rom_size();
}

static int n64_get_has_battery(EmuCoreHandle core) {
    (void)core;
    return n64_glue_get_has_battery();
}

static EmuRegion n64_get_region(EmuCoreHandle core) {
    (void)core;
    int region = n64_glue_get_region();
    if (region == 1)      return EMU_REGION_PAL;
    if (region == 2)      return EMU_REGION_MPAL;
    return EMU_REGION_NTSC;
}

// =============================================================================
// Module definition
// =============================================================================

extern "C" const EmuCoreModule n64_core_module = {
    "n64",
    n64_get_info,
    n64_create,
    n64_destroy,
    n64_load_rom,
    n64_load_rom_file,
    n64_unload_rom,
    n64_run_frame,
    n64_reset,
    n64_power_cycle,
    n64_set_paused,
    n64_is_paused,
    n64_set_fast_forward,
    n64_save_state,
    n64_load_state,
    n64_save_sram,
    n64_load_sram,
    n64_set_input_device,

    // mupen64plus runs its own emulation thread and its speed limiter already
    // sleeps to the console's real refresh rate, so n64_run_frame() returns one
    // freshly emulated frame per call at the right wall-clock pace.  The frontend
    // must not add a second, audio-derived delay (that halves the frame rate and
    // makes the sample-per-frame measurement drift).
    EMU_CORE_FLAG_SELF_PACED,
};
