/**
 * Nintendall - SNES core adapter (bsnes)
 *
 * Wraps the bsnes emulator core behind the Nintendall EmuCore C API.
 *
 * bsnes is built as a multi-system core with a single "platform" object that
 * serves files and receives video/audio/input. This file provides that platform
 * (Emulator::Platform) and drives SuperFamicom::Interface.
 *
 * The layout mirrors bsnes' own libretro target (target-libretro/program.cpp),
 * reduced to the Super Famicom system and re-pointed at the Nintendall API.
 */

#include "emu_api.h"
#include "emu_util.h"
#include "core_registry.h"

#include <emulator/emulator.hpp>
#include <sfc/interface/interface.hpp>
#include <nall/directory.hpp>
#include <nall/decode/zip.hpp>
using namespace nall;

#include <heuristics/heuristics.hpp>
#include <heuristics/heuristics.cpp>
#include <heuristics/super-famicom.cpp>
#include <heuristics/game-boy.cpp>
#include <heuristics/bs-memory.cpp>
#include <heuristics/sufami-turbo.cpp>

// Embedded board database + BS-X IPL ROM (generated C arrays).
#include "resources.hpp"

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

// =============================================================================
// Helpers
// =============================================================================

static void strip_copier_header(vector<uint8_t>& rom) {
    if ((rom.size() & 0x7fff) == 512 && rom.size() > 512) {
        memmove(rom.data(), rom.data() + 512, rom.size() - 512);
        rom.resize(rom.size() - 512);
    }
}

static void hack_patch_memory(vector<uint8_t>& data, const std::string& title) {
    if (title == "Satellaview BS-X" && data.size() >= 0x100000) {
        if (data[0x4a9b] == 0x10) data[0x4a9b] = 0x80;
        if (data[0x4d6d] == 0x10) data[0x4d6d] = 0x80;
        if (data[0x4ded] == 0x10) data[0x4ded] = 0x80;
        if (data[0x4e9a] == 0x10) data[0x4e9a] = 0x80;
    }
}

static double d2i16(double v) {
    v *= 0x8000;
    if (v > 0x7fff) v = 0x7fff;
    else if (v < -0x8000) v = -0x8000;
    return (int16_t)std::floor(v + 0.5);
}

// =============================================================================
// Core
// =============================================================================

struct EmuCore : public Emulator::Platform {
    EmuConfig config;

    EmuVideoCallback video_cb = nullptr;
    EmuAudioCallback audio_cb = nullptr;
    EmuInputPollCallback input_poll_cb = nullptr;
    EmuInputStateCallback input_state_cb = nullptr;
    void* userdata = nullptr;

    SuperFamicom::Interface* emulator = nullptr;

    bool loaded = false;
    bool paused = false;

    // video
    std::vector<uint32_t> framebuffer;   // 0xAARRGGBB
    uint width = 256;
    uint height = 240;
    uint32_t palette[32768];

    // audio
    std::vector<int16_t> audiobuf;

    // rom data
    std::string rom_path;
    std::string game_base;      // basename without extension (for .srm)
    std::string save_dir;
    std::string sfc_manifest;
    std::string sfc_region;
    std::string sfc_title;
    std::vector<uint8_t> sfc_program;
    std::vector<uint8_t> sfc_data;
    std::vector<uint8_t> sfc_expansion;
    std::vector<uint8_t> sfc_firmware;

    // -------------------------------------------------------------------------
    // Palette (24-bit, matches bsnes' default luminance/saturation/gamma)
    // -------------------------------------------------------------------------
    void build_palette() {
        const double luminance = 1.0;
        const double saturation = 1.0;
        const double gamma = 1.5;
        (void)luminance; (void)saturation;

        for (uint color = 0; color < 32768; color++) {
            uint16_t r = (color >> 10) & 31;
            uint16_t g = (color >>  5) & 31;
            uint16_t b = (color >>  0) & 31;

            r = (r << 3) | (r >> 2); r = (r << 8) | r;
            g = (g << 3) | (g >> 2); g = (g << 8) | g;
            b = (b << 3) | (b >> 2); b = (b << 8) | b;

            if (gamma != 1.0) {
                const double reciprocal = 1.0 / 32767.0;
                r = r > 32767 ? r : (uint16_t)(32767 * std::pow(r * reciprocal, gamma));
                g = g > 32767 ? g : (uint16_t)(32767 * std::pow(g * reciprocal, gamma));
                b = b > 32767 ? b : (uint16_t)(32767 * std::pow(b * reciprocal, gamma));
            }

            uint32_t rr = (r >> 8) << 16;
            uint32_t gg = (g >> 8) << 8;
            uint32_t bb = (b >> 8);
            palette[color] = 0xFF000000u | rr | gg | bb;
        }
    }

    // -------------------------------------------------------------------------
    // Emulator::Platform - file IO
    // -------------------------------------------------------------------------
    auto path(uint id) -> string override {
        (void)id;
        return save_dir.empty() ? string{} : string{save_dir.c_str()};
    }

    auto open(uint id, string name, vfs::file::mode mode, bool required = false)
        -> shared_pointer<vfs::file> override {
        (void)required;

        if (name == "ipl.rom" && mode == vfs::file::mode::read)
            return vfs::memory::file::open(iplrom, sizeof(iplrom));

        if (name == "boards.bml" && mode == vfs::file::mode::read)
            return vfs::memory::file::open(Boards, sizeof(Boards));

        if (id == 1) {  // Super Famicom
            if (name == "manifest.bml" && mode == vfs::file::mode::read)
                return vfs::memory::file::open((const uint8_t*)sfc_manifest.data(), (uint)sfc_manifest.size());
            if (name == "program.rom" && mode == vfs::file::mode::read)
                return vfs::memory::file::open(sfc_program.data(), (uint)sfc_program.size());
            if (name == "data.rom" && mode == vfs::file::mode::read)
                return vfs::memory::file::open(sfc_data.data(), (uint)sfc_data.size());
            if (name == "expansion.rom" && mode == vfs::file::mode::read)
                return vfs::memory::file::open(sfc_expansion.data(), (uint)sfc_expansion.size());
            if (name == "save.ram")
                return vfs::fs::file::open(srm_path(), mode);
        }

        return {};
    }

    auto srm_path() -> string {
        std::string p = game_base + ".srm";
        if (!save_dir.empty()) p = save_dir + "/" + p;
        return string{p.c_str()};
    }

    // -------------------------------------------------------------------------
    // Emulator::Platform - ROM load request
    // -------------------------------------------------------------------------
    auto load(uint id, string name, string type, vector<string> options = {})
        -> Load override {
        (void)name; (void)type; (void)options;
        if (id == 1) {
            if (load_super_famicom())
                return {id, string{sfc_region.c_str()}};
        }
        return {};
    }

    auto load_super_famicom() -> bool {
        vector<uint8_t> rom;
        {
            size_t sz = 0;
            uint8_t* raw = emu_read_file(rom_path.c_str(), &sz, 0);
            if (!raw) return false;
            rom.resize(sz);
            if (sz) memcpy(rom.data(), raw, sz);
            free(raw);
        }
        if (rom.size() < 0x8000) return false;

        strip_copier_header(rom);

        auto heuristics = Heuristics::SuperFamicom(rom, string{rom_path.c_str()});
        sfc_title  = heuristics.title().data();
        sfc_region = heuristics.videoRegion().data();
        sfc_manifest = heuristics.manifest().data();

        hack_patch_memory(rom, sfc_title);

        uint offset = 0;
        auto copy_block = [&](std::vector<uint8_t>& dst, uint size) {
            if (!size) return;
            dst.resize(size);
            memcpy(dst.data(), rom.data() + offset, size);
            offset += size;
        };
        copy_block(sfc_program,   heuristics.programRomSize());
        copy_block(sfc_data,      heuristics.dataRomSize());
        copy_block(sfc_expansion, heuristics.expansionRomSize());
        copy_block(sfc_firmware,  heuristics.firmwareRomSize());
        return true;
    }

    // -------------------------------------------------------------------------
    // Emulator::Platform - video / audio
    // -------------------------------------------------------------------------
    auto videoFrame(const uint16* data, uint pitch, uint widthIn, uint heightIn, uint scale)
        -> void override {
        (void)scale;
        width  = widthIn;
        height = heightIn;

        if (framebuffer.size() < (size_t)width * height)
            framebuffer.resize((size_t)width * height);

        for (uint y = 0; y < height; y++) {
            const uint8_t* row = (const uint8_t*)data + (size_t)y * pitch;
            const uint16_t* src = (const uint16_t*)row;
            uint32_t* dst = framebuffer.data() + (size_t)y * width;
            for (uint x = 0; x < width; x++)
                dst[x] = palette[src[x] & 0x7fff];
        }

        if (video_cb)
            video_cb(userdata, framebuffer.data(), (int)width, (int)height, (int)width * 4);
    }

    auto audioFrame(const double* samples, uint channels) -> void override {
        if (channels >= 2) {
            int16_t l = (int16_t)d2i16(samples[0]);
            int16_t r = (int16_t)d2i16(samples[1]);
            audiobuf.push_back(l);
            audiobuf.push_back(r);
        } else {
            int16_t s = (int16_t)d2i16(samples[0]);
            audiobuf.push_back(s);
            audiobuf.push_back(s);
        }

        // Flush once a reasonable chunk has accumulated.
        if (audiobuf.size() >= 1024) flush_audio();
    }

    void flush_audio() {
        if (audio_cb && !audiobuf.empty()) {
            audio_cb(userdata, audiobuf.data(), (int)(audiobuf.size() / 2));
        }
        audiobuf.clear();
    }

    // -------------------------------------------------------------------------
    // Emulator::Platform - input
    // -------------------------------------------------------------------------
    auto inputPoll(uint port, uint device, uint input) -> int16 override {
        if (!input_state_cb) return 0;
        if (device != SuperFamicom::ID::Device::Gamepad) return 0;

        unsigned our_port = (port == SuperFamicom::ID::Port::Controller2) ? 1u : 0u;

        // bsnes gamepad input order: Up Down Left Right B A Y X L R Select Start
        static const unsigned map[] = {
            EMU_ID_UP, EMU_ID_DOWN, EMU_ID_LEFT, EMU_ID_RIGHT,
            EMU_ID_B, EMU_ID_A, EMU_ID_Y, EMU_ID_X,
            EMU_ID_L, EMU_ID_R, EMU_ID_SELECT, EMU_ID_START
        };
        if (input >= sizeof(map) / sizeof(map[0])) return 0;

        return input_state_cb(userdata, our_port, EMU_DEVICE_JOYPAD, 0, map[input]) ? 1 : 0;
    }

    auto inputRumble(uint port, uint device, uint input, bool enable) -> void override {
        (void)port; (void)device; (void)input; (void)enable;
    }
};

// =============================================================================
// Core info
// =============================================================================

static const char* snes_exts[] = {"sfc", "smc", "fig", "swc", "bs", "st", "zip", nullptr};

static const EmuCoreInfo snes_info = {
    "bsnes",
    "115",
    "bsnes team",
    EMU_CONSOLE_SNES,
    snes_exts,
    7,
    /*has_save_states*/ 1,
    /*has_rewind*/ 1,
    /*has_cheats*/ 1,
    /*needs_bios*/ 0,
};

// =============================================================================
// Lifecycle
// =============================================================================

static EmuCoreHandle snes_create(const EmuConfig* config,
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
    c->save_dir = c->config.save_dir ? c->config.save_dir : "./saves";

    c->build_palette();
    c->audiobuf.reserve(4096);
    c->framebuffer.assign(256 * 240, 0xFF000000u);

    Emulator::platform = c;
    c->emulator = new SuperFamicom::Interface;
    return c;
}

static void snes_destroy(EmuCoreHandle core) {
    if (!core) return;
    if (core->emulator) {
        if (core->loaded) core->emulator->unload();
        delete core->emulator;
    }
    delete core;
}

// =============================================================================
// ROM loading
// =============================================================================

static EmuLoadResult snes_load_rom(EmuCoreHandle core,
                                   const uint8_t* data, size_t size,
                                   const char* filename) {
    (void)data; (void)size;
    if (!core) return EMU_LOAD_ERROR_INVALID_ROM;

    if (core->loaded) {
        core->emulator->unload();
        core->loaded = false;
    }

    core->rom_path = filename ? filename : "";

    char base[256];
    emu_get_basename(filename ? filename : "game", base, sizeof(base));
    emu_strip_extension(base, base, sizeof(base));
    core->game_base = base;

    if (!core->emulator->load())
        return EMU_LOAD_ERROR_INVALID_ROM;

    core->emulator->power();

    auto d = core->emulator->display();
    core->width  = d.width ? d.width : 256;
    core->height = d.height ? d.height : 240;

    core->loaded = true;
    return EMU_LOAD_OK;
}

static EmuLoadResult snes_load_rom_file(EmuCoreHandle core, const char* path) {
    return snes_load_rom(core, nullptr, 0, path);
}

static void snes_unload_rom(EmuCoreHandle core) {
    if (!core || !core->emulator) return;
    if (core->loaded) core->emulator->unload();
    core->loaded = false;
}

// =============================================================================
// Execution
// =============================================================================

static int snes_run_frame(EmuCoreHandle core) {
    if (!core || !core->loaded) return 0;
    if (core->paused) return 1;

    if (core->input_poll_cb) core->input_poll_cb(core->userdata);
    core->emulator->run();
    core->flush_audio();
    return 1;
}

static void snes_reset(EmuCoreHandle core) {
    if (core && core->emulator) core->emulator->reset();
}
static void snes_power_cycle(EmuCoreHandle core) {
    if (core && core->emulator) core->emulator->power();
}
static void snes_set_paused(EmuCoreHandle core, int paused) {
    if (core) core->paused = paused != 0;
}
static int snes_is_paused(EmuCoreHandle core) { return core ? core->paused : 0; }
static void snes_set_fast_forward(EmuCoreHandle core, int enabled, int speed) {
    (void)core; (void)enabled; (void)speed;
}

// =============================================================================
// Save states
// =============================================================================

static size_t snes_save_state(EmuCoreHandle core, void* buffer, size_t buffer_size) {
    if (!core || !core->loaded) return 0;
    auto s = core->emulator->serialize();
    if (!buffer) return s.size();
    if (s.size() > buffer_size) return 0;
    memcpy(buffer, s.data(), s.size());
    return s.size();
}

static int snes_load_state(EmuCoreHandle core, const void* buffer, size_t size) {
    if (!core || !core->loaded || !buffer || !size) return 0;
    serializer s((const uint8_t*)buffer, (uint)size);
    return core->emulator->unserialize(s) ? 1 : 0;
}

// =============================================================================
// Battery save RAM
// =============================================================================

static int snes_save_sram(EmuCoreHandle core, const char* path) {
    if (!core || !core->loaded) return -1;
    (void)path;  // bsnes writes through platform->open("save.ram") using its own path
    core->emulator->save();
    return 0;
}

static int snes_load_sram(EmuCoreHandle core, const char* path) {
    (void)core; (void)path;  // handled via platform->open("save.ram") on load
    return 0;
}

// =============================================================================
// Module definition
// =============================================================================

extern "C" const EmuCoreModule snes_core_module = {
    "bsnes",
    []() -> const EmuCoreInfo* { return &snes_info; },
    snes_create,
    snes_destroy,
    snes_load_rom,
    snes_load_rom_file,
    snes_unload_rom,
    snes_run_frame,
    snes_reset,
    snes_power_cycle,
    snes_set_paused,
    snes_is_paused,
    snes_set_fast_forward,
    snes_save_state,
    snes_load_state,
    snes_save_sram,
    snes_load_sram,
    nullptr,   // set_input_device (gamepad + mouse only for now)
};
