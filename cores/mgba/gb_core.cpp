/**
 * Nintendall - Game Boy / Game Boy Color / Game Boy Advance adapter
 *
 * Wraps mGBA (the upstream tree vendored at cores/mgba/upstream/) behind the
 * Nintendall EmuCore C API. One core covers all three handhelds: mGBA content-
 * sniffs the ROM and picks its GBA or GB backend at load time.
 *
 * Notes on the mapping:
 *   - mGBA renders native 32-bit mColor = mCOLOR_XBGR8 (R in the low byte),
 *     the frontend wants 0x00RRGGBB, so R/B are swapped and alpha set opaque.
 *   - The GBA backend runs its APU at 32768 Hz and the GB backend at 131072 Hz,
 *     while the frontend's device is fixed at 48 kHz, so the adapter linearly
 *     resamples to the requested rate (persistent phase keeps it continuous).
 *   - Save states use the core's own blob; battery saves use
 *     savedataClone/savedataRestore.
 */

#include "emu_api.h"
#include "emu_util.h"
#include "core_registry.h"

// mGBA public API
#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/core/interface.h>
#include <mgba/core/log.h>
#include <mgba-util/vfs.h>
#include <mgba-util/audio-buffer.h>

// Key enumerations (GBA_KEY_*, GB_KEY_*). These live in mGBA's internal
// headers, which is fine: we link the internal library anyway.
#include <mgba/internal/gba/input.h>
#include <mgba/internal/gb/input.h>

#include <cstring>
#include <cstdlib>
#include <new>
#include <vector>

namespace {

// Video scratch buffer. 256x240 comfortably covers GBA (240x160) and GB/GBC
// (160x144); mGBA writes `width` pixels per row at this stride.
constexpr int kStride = 256;
constexpr int kMaxHeight = 240;

// mGBA's internal audio ring holds 0x4000 stereo frames for both backends, so
// that is the most we can ever drain in one go.
constexpr size_t kSrcFrames = 0x4000;
// Worst case output: GBA (32768 Hz -> 48000 Hz) upsamples by ~1.47x.
constexpr size_t kMaxOutFrames = 0x4000 * 2;

// mGBA falls back to printf when no default logger is installed, which floods
// the console with BIOS/IO stub messages. Install a no-op logger instead.
void quiet_log(struct mLogger*, int, enum mLogLevel, const char*, va_list) {}
struct mLogger g_quiet_logger = {quiet_log, nullptr};

} // namespace

struct EmuCore {
    EmuConfig config;

    EmuVideoCallback video_cb = nullptr;
    EmuAudioCallback audio_cb = nullptr;
    EmuInputPollCallback input_poll_cb = nullptr;
    EmuInputStateCallback input_state_cb = nullptr;
    void* userdata = nullptr;

    mCore* core = nullptr;
    mPlatform platform = mPLATFORM_NONE;
    bool loaded = false;
    bool paused = false;
    bool fast_forward = false;

    // GBA only: the .sav that has been handed to mGBA, so that saving is a
    // no-op for that exact file (it is already mapped and written through).
    char gba_save_path[1024] = {0};
    bool gba_save_attached = false;

    // Owned copy of the ROM, only used for the memory-based load path.
    std::vector<uint8_t> rom_data;

    // Video
    std::vector<mColor> video_native;      // mGBA's XBGR8 output
    std::vector<uint32_t> framebuffer;     // frontend's 0xAARRGGBB
    int width = 240;
    int height = 160;

    // Audio resampling (native rate -> config.audio_sample_rate)
    int target_rate = 48000;
    double step = 1.0;                     // native frames per output frame
    double phase = 0.0;                    // fractional carry between frames
    int16_t prev_l = 0, prev_r = 0;
    std::vector<int16_t> src_pcm;          // drained native-rate frames
    std::vector<int16_t> out_pcm;          // resampled frames handed to audio_cb

    // Battery save size discovered at load time (0 = no battery).
    size_t sram_size = 0;
};

// =============================================================================
// Core info
// =============================================================================

static const char* gb_exts[] = {"gba", "gbc", "gb", "zip", nullptr};

static const EmuCoreInfo gb_info = {
    "mGBA",
    "0.10.x",
    "mGBA contributors",
    EMU_CONSOLE_GAMEBOY_ADVANCE,   // the registry expands this to the GB family
    gb_exts,
    4,
    /*has_save_states*/ 1,
    /*has_rewind*/ 0,
    /*has_cheats*/ 0,
    /*needs_bios*/ 0,
};

// =============================================================================
// Teardown helpers
// =============================================================================

static void gb_teardown_mcore(EmuCore* c) {
    if (!c->core) return;
    // mCoreConfigDeinit must run first: deinit() frees the mCore itself
    // (see _GBACoreDeinit/_GBCoreDeinit, which both end with free(core)), and
    // it also frees core->opts. Never free(c->core) here -- that is a
    // double-free.
    mCoreConfigDeinit(&c->core->config);
    c->core->deinit(c->core);
    c->core = nullptr;
    c->loaded = false;
    c->platform = mPLATFORM_NONE;
    c->gba_save_attached = false;
    c->gba_save_path[0] = 0;
}

// =============================================================================
// Lifecycle
// =============================================================================

static EmuCoreHandle gb_create(const EmuConfig* config,
                               EmuVideoCallback video_cb,
                               EmuAudioCallback audio_cb,
                               EmuInputPollCallback input_poll_cb,
                               EmuInputStateCallback input_state_cb,
                               void* userdata) {
    EmuCore* c = new (std::nothrow) EmuCore();
    if (!c) return nullptr;

    // Keep mGBA quiet (idempotent; harmless to set on every create).
    mLogSetDefaultLogger(&g_quiet_logger);

    if (config) c->config = *config;
    else emu_config_default(&c->config);

    c->target_rate = c->config.audio_sample_rate > 0 ? c->config.audio_sample_rate : 48000;

    c->video_cb = video_cb;
    c->audio_cb = audio_cb;
    c->input_poll_cb = input_poll_cb;
    c->input_state_cb = input_state_cb;
    c->userdata = userdata;

    c->video_native.assign((size_t)kStride * kMaxHeight, 0);
    c->framebuffer.assign((size_t)kStride * kMaxHeight, 0xFF000000u);
    c->src_pcm.assign(kSrcFrames * 2, 0);
    c->out_pcm.assign(kMaxOutFrames * 2, 0);

    return c;
}

static void gb_destroy(EmuCoreHandle core) {
    if (!core) return;
    gb_teardown_mcore(core);
    delete core;
}

// =============================================================================
// ROM loading
// =============================================================================

static EmuLoadResult gb_finish_load(EmuCore* c) {
    mCore* core = c->core;

    // Rendering + audio plumbing.
    core->setVideoBuffer(core, c->video_native.data(), (size_t)kStride);
    core->setAudioBufferSize(core, 2048);

    // Configure options the same way the libretro port does. mCoreInitConfig
    // zeroes core->opts, so without this the master volume would be 0 (silent).
    mCoreOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.useBios = true;
    opts.volume = 0x100;               // full volume (GBA_AUDIO_VOLUME_MAX)
    opts.sampleRate = (unsigned)c->target_rate;
    mCoreConfigLoadDefaults(&core->config, &opts);
    mCoreLoadForeignConfig(core, &core->config);

    core->reset(core);

    // Native audio rate + resampling ratio.
    unsigned native_rate = core->audioSampleRate(core);
    if (!native_rate) native_rate = 32768;
    c->step = (double)native_rate / (double)c->target_rate;
    c->phase = 0.0;
    c->prev_l = c->prev_r = 0;

    // Discover battery-backed storage size (0 = none).
    void* sram = nullptr;
    c->sram_size = core->savedataClone ? core->savedataClone(core, &sram) : 0;
    if (sram) free(sram);

    c->platform = core->platform(core);
    c->loaded = true;
    return EMU_LOAD_OK;
}

static EmuLoadResult gb_load_vfile(EmuCore* c, VFile* rom) {
    if (!rom) return EMU_LOAD_ERROR_INVALID_ROM;

    gb_teardown_mcore(c);

    mCore* core = mCoreFindVF(rom);
    if (!core) {
        rom->close(rom);
        return EMU_LOAD_ERROR_INVALID_ROM;
    }

    mCoreInitConfig(core, nullptr);
    if (!core->init(core)) {
        mCoreConfigDeinit(&core->config);
        free(core);
        rom->close(rom);
        return EMU_LOAD_ERROR_INTERNAL;
    }

    c->core = core;
    if (!core->loadROM(core, rom)) {
        // loadROM takes ownership of rom on success only. mGBA's own deinit
        // frees the core struct, so config teardown has to come first and the
        // core pointer must not be freed by us.
        mCoreConfigDeinit(&core->config);
        core->deinit(core);
        c->core = nullptr;
        return EMU_LOAD_ERROR_INVALID_ROM;
    }

    return gb_finish_load(c);
}

static EmuLoadResult gb_load_rom(EmuCoreHandle core,
                                 const uint8_t* data, size_t size,
                                 const char* filename) {
    (void)filename;
    if (!core || !data || size < 0x100) return EMU_LOAD_ERROR_INVALID_ROM;

    core->rom_data.assign(data, data + size);
    VFile* rom = VFileFromMemory(core->rom_data.data(), core->rom_data.size());
    if (!rom) return EMU_LOAD_ERROR_OUT_OF_MEMORY;
    return gb_load_vfile(core, rom);
}

static EmuLoadResult gb_load_rom_file(EmuCoreHandle core, const char* path) {
    if (!core || !path) return EMU_LOAD_ERROR_INVALID_ROM;
    VFile* rom = VFileOpen(path, O_RDONLY);
    if (!rom) return EMU_LOAD_ERROR_INVALID_ROM;
    return gb_load_vfile(core, rom);
}

static void gb_unload_rom(EmuCoreHandle core) {
    if (!core) return;
    gb_teardown_mcore(core);
    core->rom_data.clear();
}

// =============================================================================
// Execution
// =============================================================================

static uint32_t gb_build_keys(EmuCore* c) {
    if (!c->input_state_cb) return 0;

    auto down = [&](unsigned id) -> bool {
        return c->input_state_cb(c->userdata, 0, EMU_DEVICE_JOYPAD, 0, id) != 0;
    };

    uint32_t k = 0;
    if (c->platform == mPLATFORM_GBA) {
        if (down(EMU_ID_A))      k |= 1u << GBA_KEY_A;
        if (down(EMU_ID_B))      k |= 1u << GBA_KEY_B;
        if (down(EMU_ID_SELECT)) k |= 1u << GBA_KEY_SELECT;
        if (down(EMU_ID_START))  k |= 1u << GBA_KEY_START;
        if (down(EMU_ID_RIGHT))  k |= 1u << GBA_KEY_RIGHT;
        if (down(EMU_ID_LEFT))   k |= 1u << GBA_KEY_LEFT;
        if (down(EMU_ID_UP))     k |= 1u << GBA_KEY_UP;
        if (down(EMU_ID_DOWN))   k |= 1u << GBA_KEY_DOWN;
        if (down(EMU_ID_R))      k |= 1u << GBA_KEY_R;
        if (down(EMU_ID_L))      k |= 1u << GBA_KEY_L;
    } else {
        if (down(EMU_ID_A))      k |= 1u << GB_KEY_A;
        if (down(EMU_ID_B))      k |= 1u << GB_KEY_B;
        if (down(EMU_ID_SELECT)) k |= 1u << GB_KEY_SELECT;
        if (down(EMU_ID_START))  k |= 1u << GB_KEY_START;
        if (down(EMU_ID_RIGHT))  k |= 1u << GB_KEY_RIGHT;
        if (down(EMU_ID_LEFT))   k |= 1u << GB_KEY_LEFT;
        if (down(EMU_ID_UP))     k |= 1u << GB_KEY_UP;
        if (down(EMU_ID_DOWN))   k |= 1u << GB_KEY_DOWN;
    }
    return k;
}

static void gb_deliver_video(EmuCore* c) {
    if (!c->video_cb) return;

    const mColor* src = c->video_native.data();
    uint32_t* dst = c->framebuffer.data();
    const int w = c->width, h = c->height;

    for (int y = 0; y < h; y++) {
        const mColor* srow = src + (size_t)y * kStride;
        uint32_t* drow = dst + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            // mGBA native XBGR8 (0x00BBGGRR) -> frontend 0xAARRGGBB.
            mColor p = srow[x];
            uint32_t r = p & 0x000000FFu;
            uint32_t g = p & 0x0000FF00u;
            uint32_t b = p & 0x00FF0000u;
            drow[x] = 0xFF000000u | (r << 16) | g | (b >> 16);
        }
    }

    c->video_cb(c->userdata, dst, w, h, w * 4);
}

static void gb_deliver_audio(EmuCore* c) {
    if (!c->audio_cb) {
        // Still have to drain, or the ring backs up and mGBA drops samples.
        struct mAudioBuffer* mb = c->core->getAudioBuffer(c->core);
        if (mb && mAudioBufferAvailable(mb) > 0) {
            size_t n = mAudioBufferAvailable(mb);
            if (n > kSrcFrames) n = kSrcFrames;
            mAudioBufferRead(mb, c->src_pcm.data(), n);
        }
        return;
    }

    struct mAudioBuffer* mb = c->core->getAudioBuffer(c->core);
    if (!mb) return;

    size_t avail = mAudioBufferAvailable(mb);
    if (avail == 0) return;
    if (avail > kSrcFrames) avail = kSrcFrames;

    size_t got = mAudioBufferRead(mb, c->src_pcm.data(), avail);
    if (got == 0) return;

    // The native rate is not fixed: writing SOUNDBIAS changes the GBA's
    // sample interval (32768 Hz at reset, up to 131072 Hz later), so the
    // ratio has to be refreshed every frame instead of being cached at load.
    unsigned native_rate = c->core->audioSampleRate(c->core);
    if (!native_rate) native_rate = (c->platform == mPLATFORM_GBA) ? 32768u : 131072u;
    c->step = (double)native_rate / (double)c->target_rate;

    // Linear interpolation native -> target, carrying phase and the previous
    // input frame across calls so the seam between frames is continuous.
    const int16_t* in = c->src_pcm.data();
    int16_t* out = c->out_pcm.data();
    size_t produced = 0;
    double frac = c->phase;
    int16_t pl = c->prev_l, pr = c->prev_r;
    const double step = c->step;

    for (size_t i = 0; i < got; i++) {
        int16_t cl = in[i * 2 + 0];
        int16_t cr = in[i * 2 + 1];
        while (frac < 1.0 && produced < kMaxOutFrames) {
            out[produced * 2 + 0] = (int16_t)(pl + (int32_t)((cl - pl) * frac));
            out[produced * 2 + 1] = (int16_t)(pr + (int32_t)((cr - pr) * frac));
            produced++;
            frac += step;
        }
        frac -= 1.0;
        pl = cl;
        pr = cr;
    }

    c->phase = frac;
    c->prev_l = pl;
    c->prev_r = pr;

    if (produced) c->audio_cb(c->userdata, out, (int)produced);
}

static int gb_run_frame(EmuCoreHandle core) {
    if (!core || !core->core || !core->loaded) return 0;
    if (core->paused) return 1;

    if (core->input_poll_cb) core->input_poll_cb(core->userdata);
    core->core->setKeys(core->core, gb_build_keys(core));

    core->core->runFrame(core->core);

    unsigned w = 0, h = 0;
    core->core->currentVideoSize(core->core, &w, &h);
    if (w > 0 && h > 0) {
        core->width = (int)w;
        core->height = (int)h;
    }

    gb_deliver_video(core);
    gb_deliver_audio(core);
    return 1;
}

static void gb_reset(EmuCoreHandle core) {
    if (core && core->core) core->core->reset(core->core);
}

static void gb_power_cycle(EmuCoreHandle core) {
    if (core && core->core) core->core->reset(core->core);
}

static void gb_set_paused(EmuCoreHandle core, int paused) {
    if (core) core->paused = paused != 0;
}
static int gb_is_paused(EmuCoreHandle core) { return core ? core->paused : 0; }
static void gb_set_fast_forward(EmuCoreHandle core, int enabled, int speed) {
    if (core) core->fast_forward = enabled != 0;
    (void)speed;
}

// =============================================================================
// Save states
// =============================================================================

static size_t gb_save_state(EmuCoreHandle core, void* buffer, size_t buffer_size) {
    if (!core || !core->core || !core->loaded) return 0;
    size_t need = core->core->stateSize(core->core);
    if (!buffer) return need;
    if (buffer_size < need) return 0;
    if (!core->core->saveState(core->core, buffer)) return 0;
    return need;
}

static int gb_load_state(EmuCoreHandle core, const void* buffer, size_t size) {
    if (!core || !core->core || !buffer) return 0;
    if (size < core->core->stateSize(core->core)) return 0;
    return core->core->loadState(core->core, buffer) ? 1 : 0;
}

// =============================================================================
// Battery / SRAM
// =============================================================================

static int gb_save_sram(EmuCoreHandle core, const char* path) {
    if (!core || !core->core || !core->loaded) return -1;

    // Game Boy Advance: gb_load_sram() gave the .sav to mGBA, which keeps it
    // open for as long as the game is loaded and writes every in-game save
    // into it as it happens, so the data is already on disk.  Copying it out
    // again would also mean truncating a file the core still has mapped, which
    // Windows refuses.  Games that never touched their save chip wrote nothing
    // and have nothing to preserve.
    if (core->platform == mPLATFORM_GBA && core->gba_save_attached &&
        _stricmp(core->gba_save_path, path) == 0) {
        return 0;
    }

    void* sram = nullptr;
    size_t n = core->core->savedataClone ? core->core->savedataClone(core->core, &sram) : 0;
    if (!n || !sram) {
        if (sram) free(sram);
        return 0;   // no battery-backed storage
    }

    int rc = emu_write_file(path, sram, n);
    free(sram);
    return rc;
}

static int gb_load_sram(EmuCoreHandle core, const char* path) {
    if (!core || !core->core || !core->loaded) return -1;

    if (core->platform == mPLATFORM_GBA) {
        // A GBA save cannot be restored before the game has touched its save
        // chip: the chip type decides how big the save is and mGBA only learns
        // it by watching the cartridge accesses.  Handing mGBA the file (which
        // is what its own frontends do) solves that -- the core keeps the file
        // mapped and switches between 512K and 1M flash by itself when the
        // game asks for it.
        //
        // Attach the file even when it does not exist yet.  mCoreLoadSaveFile
        // opens with O_CREAT, and this is what makes a new game use the file
        // instead of anonymous memory; otherwise the first in-game save never
        // reaches disk.
        if (!mCoreLoadSaveFile(core->core, path, false)) return -1;
        core->gba_save_attached = true;
        snprintf(core->gba_save_path, sizeof(core->gba_save_path), "%s", path);
        return 0;
    }

    size_t n = 0;
    uint8_t* data = emu_read_file(path, &n, 0);
    if (!data) return -1;

    bool ok = core->core->savedataRestore
        ? core->core->savedataRestore(core->core, data, n, true)
        : false;
    free(data);
    return ok ? 0 : -1;
}

// =============================================================================
// Module definition
// =============================================================================

extern "C" const EmuCoreModule gb_core_module = {
    "mgba",
    []() -> const EmuCoreInfo* { return &gb_info; },
    gb_create,
    gb_destroy,
    gb_load_rom,
    gb_load_rom_file,
    gb_unload_rom,
    gb_run_frame,
    gb_reset,
    gb_power_cycle,
    gb_set_paused,
    gb_is_paused,
    gb_set_fast_forward,
    gb_save_state,
    gb_load_state,
    gb_save_sram,
    gb_load_sram,
    nullptr,   // set_input_device
};
