/**
 * Nintendall - NES core adapter (Nestopia)
 *
 * Wraps the Nestopia emulator core (source/core) behind the Nintendall
 * EmuCore C API. Build alongside the upstream Nestopia "source/core"
 * tree; see CMakeLists.txt in this directory.
 */

#include "emu_api.h"
#include "emu_util.h"
#include "core_registry.h"

// Nestopia core API
#include <NstApiEmulator.hpp>
#include <NstApiMachine.hpp>
#include <NstApiVideo.hpp>
#include <NstApiSound.hpp>
#include <NstApiInput.hpp>
#include <NstApiCartridge.hpp>
#include <NstApiUser.hpp>
#include <NstApiMemory.hpp>

#include <cstring>
#include <cstdio>
#include <string>
#include <sstream>
#include <vector>
#include <new>

using namespace Nes;
using namespace Nes::Api;

// =============================================================================
// Memory-backed stream
// =============================================================================

namespace {

class MemoryBuffer : public std::streambuf {
public:
    MemoryBuffer(const uint8_t* data, size_t size) {
        char* p = const_cast<char*>(reinterpret_cast<const char*>(data));
        setg(p, p, p + size);
    }
protected:
    pos_type seekoff(off_type off, std::ios_base::seekdir dir,
                     std::ios_base::openmode which) override {
        if (!(which & std::ios_base::in)) return pos_type(off_type(-1));
        char* newg;
        switch (dir) {
            case std::ios_base::beg: newg = eback() + off; break;
            case std::ios_base::cur: newg = gptr() + off; break;
            case std::ios_base::end: newg = egptr() + off; break;
            default: return pos_type(off_type(-1));
        }
        if (newg < eback() || newg > egptr()) return pos_type(off_type(-1));
        setg(eback(), newg, egptr());
        return pos_type(newg - eback());
    }
    pos_type seekpos(pos_type sp, std::ios_base::openmode which) override {
        return seekoff(off_type(sp), std::ios_base::beg, which);
    }
};

// Nestopia requires a user "File" handler for battery-backed storage.
class BatteryFile : public User::File {
public:
    BatteryFile(std::vector<uint8_t>* battery) : m_battery(battery) {}

    Action GetAction() const throw() override { return User::File::LOAD_BATTERY; }

    Result SetContent(const void* mem, ulong size) throw() override {
        if (!m_battery) return RESULT_ERR_GENERIC;
        m_battery->assign((const uint8_t*)mem, (const uint8_t*)mem + size);
        return RESULT_OK;
    }

    void GetRawStorage(void*& data, ulong& size) const throw() override {
        if (m_battery && !m_battery->empty()) {
            data = m_battery->data();
            size = (ulong)m_battery->size();
        } else {
            data = nullptr;
            size = 0;
        }
    }

private:
    std::vector<uint8_t>* m_battery;
};

} // namespace

// =============================================================================
// Core instance
// =============================================================================

struct EmuCore {
    EmuConfig config;

    EmuVideoCallback video_cb = nullptr;
    EmuAudioCallback audio_cb = nullptr;
    EmuInputPollCallback input_poll_cb = nullptr;
    EmuInputStateCallback input_state_cb = nullptr;
    void* userdata = nullptr;

    Emulator* emulator = nullptr;
    Machine* machine = nullptr;
    Video* video = nullptr;
    Sound* sound = nullptr;
    Input* input = nullptr;

    bool loaded = false;
    bool paused = false;
    bool fast_forward = false;

    // frame buffers
    std::vector<uint32_t> framebuffer;   // 0xAARRGGBB
    std::vector<int16_t> audiobuf;       // interleaved stereo
    int width = 256;
    int height = 240;

    // current input state for both ports
    uint pad_buttons[2] = {0, 0};
    uint zapper_x = 0, zapper_y = 0, zapper_fire = 0;
    bool zapper_enabled = false;

    std::vector<uint8_t> battery;

    // ---- input handling ----
    void poll_inputs() {
        if (input_poll_cb) input_poll_cb(userdata);
        pad_buttons[0] = 0;
        pad_buttons[1] = 0;
        if (!input_state_cb) return;

        auto pressed = [&](unsigned id) -> bool {
            return input_state_cb(userdata, 0, EMU_DEVICE_JOYPAD, 0, id) != 0;
        };

        for (unsigned port = 0; port < 2; port++) {
            uint b = 0;
            if (input_state_cb(userdata, port, EMU_DEVICE_JOYPAD, 0, EMU_ID_A))      b |= Core::Input::Controllers::Pad::A;
            if (input_state_cb(userdata, port, EMU_DEVICE_JOYPAD, 0, EMU_ID_B))      b |= Core::Input::Controllers::Pad::B;
            if (input_state_cb(userdata, port, EMU_DEVICE_JOYPAD, 0, EMU_ID_SELECT)) b |= Core::Input::Controllers::Pad::SELECT;
            if (input_state_cb(userdata, port, EMU_DEVICE_JOYPAD, 0, EMU_ID_START))  b |= Core::Input::Controllers::Pad::START;
            if (input_state_cb(userdata, port, EMU_DEVICE_JOYPAD, 0, EMU_ID_UP))     b |= Core::Input::Controllers::Pad::UP;
            if (input_state_cb(userdata, port, EMU_DEVICE_JOYPAD, 0, EMU_ID_DOWN))   b |= Core::Input::Controllers::Pad::DOWN;
            if (input_state_cb(userdata, port, EMU_DEVICE_JOYPAD, 0, EMU_ID_LEFT))   b |= Core::Input::Controllers::Pad::LEFT;
            if (input_state_cb(userdata, port, EMU_DEVICE_JOYPAD, 0, EMU_ID_RIGHT))  b |= Core::Input::Controllers::Pad::RIGHT;
            pad_buttons[port] = b;
        }
        (void)pressed;

        // Light gun (Zapper) on port 2 when a pointer device is selected.
        if (input_state_cb(userdata, 1, EMU_DEVICE_LIGHTGUN, 0, EMU_ID_POINTER_X))
            zapper_enabled = true;
        if (zapper_enabled) {
            zapper_x = (uint)input_state_cb(userdata, 1, EMU_DEVICE_LIGHTGUN, 0, EMU_ID_POINTER_X);
            zapper_y = (uint)input_state_cb(userdata, 1, EMU_DEVICE_LIGHTGUN, 0, EMU_ID_POINTER_Y);
            zapper_fire = input_state_cb(userdata, 1, EMU_DEVICE_LIGHTGUN, 0, EMU_ID_POINTER_PRESSED) ? 1 : 0;
        }
    }
};

// =============================================================================
// Core info
// =============================================================================

static const char* nes_exts[] = {"nes", "nsf", "fds", "unf", "unif", nullptr};

static const EmuCoreInfo nes_info = {
    "Nestopia",
    "1.52",
    "Martin Freij and contributors",
    EMU_CONSOLE_NES,
    nes_exts,
    5,
    /*has_save_states*/ 1,
    /*has_rewind*/ 1,
    /*has_cheats*/ 1,
    /*needs_bios*/ 0,
};

// =============================================================================
// Lifecycle
// =============================================================================

static EmuCoreHandle nes_create(const EmuConfig* config,
                                EmuVideoCallback video_cb,
                                EmuAudioCallback audio_cb,
                                EmuInputPollCallback input_poll_cb,
                                EmuInputStateCallback input_state_cb,
                                void* userdata) {
    EmuCore* c = new (std::nothrow) EmuCore();
    if (!c) return nullptr;

    if (config) c->config = *config;
    else emu_config_default(&c->config);

    c->video_cb = video_cb;
    c->audio_cb = audio_cb;
    c->input_poll_cb = input_poll_cb;
    c->input_state_cb = input_state_cb;
    c->userdata = userdata;

    try {
        c->emulator = new Emulator();
        c->machine  = new Machine(*c->emulator);
        c->video    = new Video(*c->emulator);
        c->sound    = new Sound(*c->emulator);
        c->input    = new Input(*c->emulator);
    } catch (...) {
        delete c->input; delete c->sound; delete c->video;
        delete c->machine; delete c->emulator;
        delete c;
        return nullptr;
    }

    c->framebuffer.assign(256 * 240 * 4, 0xFF000000u);

    // Configure 32-bit BGRA/RGB output with no filtering.
    Video::RenderState state;
    state.bits.count = 32;
    state.bits.mask.r = 0x00FF0000u;
    state.bits.mask.g = 0x0000FF00u;
    state.bits.mask.b = 0x000000FFu;
    state.width = 256;
    state.height = 240;
    state.filter = Video::RenderState::FILTER_NONE;
    c->video->SetRenderState(state);

    return c;
}

static void nes_destroy(EmuCoreHandle core) {
    if (!core) return;
    delete core->input;
    delete core->sound;
    delete core->video;
    delete core->machine;
    delete core->emulator;
    delete core;
}

// =============================================================================
// ROM loading
// =============================================================================

static EmuLoadResult nes_load_rom(EmuCoreHandle core,
                                  const uint8_t* data, size_t size,
                                  const char* filename) {
    if (!core || !data || size < 16) return EMU_LOAD_ERROR_INVALID_ROM;

    core->machine->Unload();

    MemoryBuffer mb(data, size);
    std::istream stream(&mb);

    Machine::FavoredSystem system =
        (core->config.region == EMU_REGION_PAL) ? Machine::FAVORED_NES_PAL
                                                : Machine::FAVORED_NES_NTSC;

    Result result = core->machine->Load(stream, system,
                                        Machine::DONT_ASK_PROFILE);
    if (result != RESULT_OK) {
        // NSF / FDS fallbacks
        stream.clear();
        stream.seekg(0, std::ios::beg);
        if (data[0] == 'N' && data[1] == 'E' && data[2] == 'S' && data[3] == 'M')
            result = core->machine->LoadSound(stream, system);
        else if (data[0] == 'F' && data[1] == 'D' && data[2] == 'S')
            result = core->machine->LoadDisk(stream, system);
    }

    if (result != RESULT_OK) return EMU_LOAD_ERROR_INVALID_ROM;

    core->machine->Power(true);

    // The APU boots at Nestopia's built-in default of 44100 Hz. The frontend
    // plays every core back at config.audio_sample_rate (48 kHz), so if we leave
    // the APU alone it renders 44100 Hz audio that is then played at 48 kHz:
    // every frame comes up short and Nestopia holds the last sample to fill the
    // rest. That sounds sharp (about 1.5 semitones high) and warbly, most
    // obvious on sustained music such as Castlevania or Zelda. Set the rate
    // after power-on so the APU's mix table/filter are built for the right rate.
    {
        const ulong rate = (ulong)(core->config.audio_sample_rate > 0
                                       ? core->config.audio_sample_rate : 48000);
        core->sound->SetSampleRate(rate);
    }

    Video::RenderState state;
    if (core->video->GetRenderState(state) == RESULT_OK) {
        core->width = state.width ? state.width : 256;
        core->height = state.height ? state.height : 240;
    }
    core->framebuffer.assign((size_t)core->width * core->height * 4, 0xFF000000u);

    core->loaded = true;
    return EMU_LOAD_OK;
}

static EmuLoadResult nes_load_rom_file(EmuCoreHandle core, const char* path) {
    size_t size = 0;
    uint8_t* data = emu_read_file(path, &size, 0);
    if (!data) return EMU_LOAD_ERROR_INVALID_ROM;
    EmuLoadResult r = nes_load_rom(core, data, size, path);
    free(data);
    return r;
}

static void nes_unload_rom(EmuCoreHandle core) {
    if (!core || !core->machine) return;
    core->machine->Unload();
    core->loaded = false;
}

// =============================================================================
// Execution
// =============================================================================

static int nes_run_frame(EmuCoreHandle core) {
    if (!core || !core->loaded) return 0;
    if (core->paused) return 1;

    core->poll_inputs();

    Core::Input::Controllers controllers;
    controllers.pad[0].buttons = core->pad_buttons[0];
    controllers.pad[1].buttons = core->pad_buttons[1];
    if (core->zapper_enabled) {
        controllers.zapper.x = core->zapper_x;
        controllers.zapper.y = core->zapper_y;
        controllers.zapper.fire = core->zapper_fire;
    }

    Core::Video::Output vout(core->framebuffer.data(), (long)core->width * 4);

    // Ask for exactly one console frame of audio.  The APU renders at
    // config.audio_sample_rate (set in nes_load_rom), so the count has to follow
    // the console's real refresh rate - NTSC NES is 60.0988 Hz, PAL 50.0070 Hz.
    // Asking for too many makes Nestopia hold the last sample to fill the
    // remainder (audible as a sharp, warbling pitch); asking for too few lets
    // the internal ring back up and drop audio.
    const uint rate = (uint)(core->config.audio_sample_rate > 0
                                 ? core->config.audio_sample_rate : 48000);
    const double fps = (core->machine->GetMode() == Machine::PAL) ? 50.0070 : 60.0988;
    uint frames = (uint)((double)rate / fps + 0.5);
    if (frames == 0) frames = 1;
    if (core->audiobuf.size() < (size_t)frames * 2)
        core->audiobuf.assign((size_t)frames * 2, 0);
    Core::Sound::Output sout(core->audiobuf.data(), frames, nullptr, 0);

    core->emulator->Execute(&vout, &sout, &controllers);

    // Convert 0x00RRGGBB -> 0xFFRRGGBB (add opaque alpha) already what video_cb wants.
    if (core->video_cb) {
        core->video_cb(core->userdata, core->framebuffer.data(),
                       core->width, core->height, core->width * 4);
    }
    if (core->audio_cb && frames) {
        // Nestopia wrote `frames` mono int16 samples into audiobuf[0..frames).
        // Expand in place to interleaved stereo.
        for (int i = (int)frames - 1; i >= 0; i--) {
            int16_t s = core->audiobuf[i];
            core->audiobuf[i * 2 + 0] = s;
            core->audiobuf[i * 2 + 1] = s;
        }
        core->audio_cb(core->userdata, core->audiobuf.data(), (int)frames);
    }
    return 1;
}

static void nes_reset(EmuCoreHandle core) {
    if (core && core->machine) core->machine->Reset(false);
}
static void nes_power_cycle(EmuCoreHandle core) {
    if (core && core->machine) core->machine->Reset(true);
}
static void nes_set_paused(EmuCoreHandle core, int paused) {
    if (core) core->paused = paused != 0;
}
static int nes_is_paused(EmuCoreHandle core) { return core ? core->paused : 0; }
static void nes_set_fast_forward(EmuCoreHandle core, int enabled, int speed) {
    if (core) core->fast_forward = enabled != 0;
    (void)speed;
}

// =============================================================================
// Save states
// =============================================================================

static size_t nes_save_state(EmuCoreHandle core, void* buffer, size_t buffer_size) {
    if (!core || !core->machine || !core->loaded) return 0;

    std::ostringstream oss(std::ios::binary);
    if (core->machine->SaveState(oss, Machine::NO_COMPRESSION) != RESULT_OK)
        return 0;

    std::string s = oss.str();
    if (!buffer) return s.size();
    if (s.size() > buffer_size) return 0;
    memcpy(buffer, s.data(), s.size());
    return s.size();
}

static int nes_load_state(EmuCoreHandle core, const void* buffer, size_t size) {
    if (!core || !core->machine || !buffer || !size) return 0;
    MemoryBuffer mb((const uint8_t*)buffer, size);
    std::istream stream(&mb);
    return core->machine->LoadState(stream) == RESULT_OK ? 1 : 0;
}

// =============================================================================
// Battery / SRAM
// =============================================================================

static int nes_save_sram(EmuCoreHandle core, const char* path) {
    if (!core || !core->loaded) return -1;
    // Snapshot battery-backed regions through the Memory API.
    Memory mem(*core->emulator);
    ulong n = mem.NumRegions();
    std::vector<uint8_t> out;
    for (ulong i = 0; i < n; i++) {
        Memory::Region r = mem.GetRegion(i);
        if (!r.battery || !r.data || !r.size) continue;
        size_t off = out.size();
        out.resize(off + r.size);
        memcpy(out.data() + off, r.data, r.size);
    }
    if (out.empty()) return 0;
    return emu_write_file(path, out.data(), out.size());
}

static int nes_load_sram(EmuCoreHandle core, const char* path) {
    if (!core || !core->loaded) return -1;
    size_t size = 0;
    uint8_t* data = emu_read_file(path, &size, 0);
    if (!data) return -1;

    Memory mem(*core->emulator);
    ulong n = mem.NumRegions();
    size_t off = 0;
    for (ulong i = 0; i < n; i++) {
        Memory::Region r = mem.GetRegion(i);
        if (!r.battery || !r.data || !r.size) continue;
        size_t take = (off + r.size <= size) ? r.size : 0;
        if (!take) break;
        memcpy(r.data, data + off, take);
        off += take;
    }
    free(data);
    return 0;
}

// =============================================================================
// Input device selection (no-op; handled at poll time)
// =============================================================================

static void nes_set_input_device(EmuCoreHandle core, unsigned port, EmuDeviceType device) {
    if (!core) return;
    if (port == 1 && device == EMU_DEVICE_LIGHTGUN) core->zapper_enabled = true;
    if (port == 1 && device == EMU_DEVICE_JOYPAD)    core->zapper_enabled = false;
}

// =============================================================================
// Module definition
// =============================================================================

extern "C" const EmuCoreModule nes_core_module = {
    "nestopia",
    []() -> const EmuCoreInfo* { return &nes_info; },
    nes_create,
    nes_destroy,
    nes_load_rom,
    nes_load_rom_file,
    nes_unload_rom,
    nes_run_frame,
    nes_reset,
    nes_power_cycle,
    nes_set_paused,
    nes_is_paused,
    nes_set_fast_forward,
    nes_save_state,
    nes_load_state,
    nes_save_sram,
    nes_load_sram,
    nes_set_input_device,
};