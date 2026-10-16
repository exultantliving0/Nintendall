/**
 * Nintendall - unified Win32 frontend
 *
 * One window, one executable. It:
 *   - lets the user pick a ROM (File > Load ROM...)
 *   - auto-detects the console from the ROM contents
 *   - finds a compiled-in core for that console and runs it
 *   - presents video through an offscreen back buffer and audio through waveOut
 *   - exposes quick save/load state (slots 1-9) via the unified container
 *
 * It deliberately depends on nothing but Win32 + the common core API, so the
 * whole project builds with just a C/C++ toolchain.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <timeapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <string>
#include <vector>

#include "emu_api.h"
#include "emu_util.h"
#include "rom_detect.h"
#include "save_state.h"
#include "core_registry.h"
#include "zip.h"

// =============================================================================
// Menu / command ids
// =============================================================================

#define IDM_FILE_LOAD          1001
#define IDM_FILE_EXIT          1002

#define IDM_EMU_PAUSE          1101
#define IDM_EMU_RESET          1102
#define IDM_EMU_POWER_CYCLE    1103

#define IDM_STATE_SAVE         1201
#define IDM_STATE_LOAD         1202
#define IDM_STATE_SAVE_SLOT    1210   // + slot (1..9)
#define IDM_STATE_LOAD_SLOT    1220   // + slot (1..9)

#define IDM_HELP_ABOUT         1301

#define IDM_ACCEL_LOAD         2001
#define IDM_ACCEL_SAVE         2002
#define IDM_ACCEL_LOADSTATE    2003
#define IDM_ACCEL_RESET        2004
#define IDM_ACCEL_PAUSE        2005

#define APP_TITLE L"Nintendall"

// =============================================================================
// Audio
// =============================================================================

#define AUDIO_SAMPLE_RATE   48000
#define AUDIO_BUFFERS       8
#define AUDIO_FRAMES_PER_BUF 1024        // stereo frames per WAVEHDR

static HWAVEOUT  g_waveout = nullptr;
static WAVEHDR   g_wavehdr[AUDIO_BUFFERS];
static int16_t   g_wavebuf[AUDIO_BUFFERS][AUDIO_FRAMES_PER_BUF * 2];
static bool      g_wave_inuse[AUDIO_BUFFERS];
static CRITICAL_SECTION g_audio_cs;
static std::vector<int16_t> g_audio_ring;
static double                   g_spf = 0.0;   // smoothed stereo frames the core emits per emulated frame
static long long                g_frame_audio = 0;  // stereo frames emitted so far this eumulated frame

// =============================================================================
// Emulation state
// =============================================================================

static HWND                     g_hwnd = nullptr;
static const EmuCoreModule*     g_module = nullptr;
static EmuCoreHandle            g_core = nullptr;
static EmuStateManager*         g_state_mgr = nullptr;
static CRITICAL_SECTION         g_core_cs;

static char                     g_save_dir[MAX_PATH] = ".";
static char                     g_rom_path[MAX_PATH] = "";
static char                     g_rom_base[256] = "";
static char                     g_rom_title[64] = "";
static uint32_t                 g_rom_crc = 0;
static EmuConsoleType           g_console = EMU_CONSOLE_UNKNOWN;

static volatile LONG            g_paused = 0;
static volatile LONG            g_quit = 0;
static uint64_t                 g_frame_count = 0;
static HANDLE                   g_emu_thread = nullptr;

// =============================================================================
// Video
// =============================================================================

static CRITICAL_SECTION         g_video_cs;
static std::vector<uint32_t>    g_frame;
static int                      g_frame_w = 0;
static int                      g_frame_h = 0;

// Offscreen back buffer: the frame is composed into a memory DC and then
// blitted to the window in one operation, which removes tearing/flicker.
static HDC                      g_back_dc = nullptr;
static HBITMAP                  g_back_bmp = nullptr;
static HBITMAP                  g_back_old = nullptr;
static int                      g_back_w = 0;
static int                      g_back_h = 0;

// =============================================================================
// Helpers
// =============================================================================

static void SetStatus(const wchar_t* fmt, ...) {
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 511, fmt, ap);
    va_end(ap);
    buf[511] = 0;
    SetWindowTextW(g_hwnd, buf);
}

static std::wstring Utf8ToWide(const char* s) {
    if (!s) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    w.resize(n - 1);
    return w;
}

// =============================================================================
// Callbacks (invoked from the emulation thread)
// =============================================================================

static void VideoCallback(void* userdata, const uint32_t* pixels,
                          int width, int height, int pitch) {
    (void)userdata;
    if (!pixels || width <= 0 || height <= 0) return;

    EnterCriticalSection(&g_video_cs);
    if (g_frame_w != width || g_frame_h != height || g_frame.size() < (size_t)width * height)
        g_frame.resize((size_t)width * height);
    g_frame_w = width;
    g_frame_h = height;

    const int src_pitch = pitch > 0 ? pitch : width * 4;
    const uint8_t* src = (const uint8_t*)pixels;
    for (int y = 0; y < height; y++) {
        memcpy(g_frame.data() + (size_t)y * width,
               src + (size_t)y * src_pitch,
               (size_t)width * 4);
    }
    LeaveCriticalSection(&g_video_cs);

    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
}

static void AudioCallback(void* userdata, const int16_t* samples, int count) {
    (void)userdata;
    if (!samples || count <= 0) return;

    // Accumulate the stereo frames the core produced during the emulated frame
    // that is currently running. A single frame may deliver audio in several
    // callbacks (the SNES core flushes every 512 frames), so we total them here
    // and let the frame loop turn the total into a samples-per-frame rate. This
    // runs on the emulation thread, so no locking is needed.
    g_frame_audio += count;

    EnterCriticalSection(&g_audio_cs);
    // Cap the ring at ~2 seconds so a stalled device cannot grow without bound.
    const size_t cap = (size_t)AUDIO_SAMPLE_RATE * 2 * 2;
    if (g_audio_ring.size() < cap) {
        size_t n = (size_t)count * 2;
        size_t room = cap - g_audio_ring.size();
        if (n > room) n = room;
        g_audio_ring.insert(g_audio_ring.end(), samples, samples + n);
    }
    LeaveCriticalSection(&g_audio_cs);
}

static int KeyDown(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) ? 1 : 0;
}

static int InputStateCallback(void* userdata, unsigned port, unsigned device,
                              unsigned index, unsigned id) {
    (void)userdata; (void)index;
    if (port != 0) return 0;
    if (device != EMU_DEVICE_JOYPAD && device != EMU_DEVICE_NONE) return 0;

    switch (id) {
        case EMU_ID_UP:     return KeyDown(VK_UP);
        case EMU_ID_DOWN:   return KeyDown(VK_DOWN);
        case EMU_ID_LEFT:   return KeyDown(VK_LEFT);
        case EMU_ID_RIGHT:  return KeyDown(VK_RIGHT);
        case EMU_ID_A:      return KeyDown('X');
        case EMU_ID_B:      return KeyDown('Z');
        case EMU_ID_X:      return KeyDown('S');
        case EMU_ID_Y:      return KeyDown('A');
        case EMU_ID_L:      return KeyDown('Q');
        case EMU_ID_R:      return KeyDown('W');
        case EMU_ID_START:  return KeyDown(VK_RETURN);
        case EMU_ID_SELECT: return KeyDown(VK_RSHIFT) || KeyDown(VK_SHIFT);
        default:            return 0;
    }
}

// =============================================================================
// Audio pump (called from the emulation thread each frame)
// =============================================================================

static void AudioInit(HWND hwnd) {
    (void)hwnd;
    InitializeCriticalSection(&g_audio_cs);

    WAVEFORMATEX wf = {};
    wf.wFormatTag      = WAVE_FORMAT_PCM;
    wf.nChannels       = 2;
    wf.nSamplesPerSec  = AUDIO_SAMPLE_RATE;
    wf.wBitsPerSample  = 16;
    wf.nBlockAlign     = 4;
    wf.nAvgBytesPerSec = AUDIO_SAMPLE_RATE * 4;
    wf.cbSize          = 0;

    if (waveOutOpen(&g_waveout, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        g_waveout = nullptr;
        return;
    }

    for (int i = 0; i < AUDIO_BUFFERS; i++) {
        ZeroMemory(&g_wavehdr[i], sizeof(WAVEHDR));
        g_wavehdr[i].lpData         = (LPSTR)g_wavebuf[i];
        g_wavehdr[i].dwBufferLength = AUDIO_FRAMES_PER_BUF * 4;
        waveOutPrepareHeader(g_waveout, &g_wavehdr[i], sizeof(WAVEHDR));
        g_wave_inuse[i] = false;
    }
}

static void AudioShutdown() {
    if (g_waveout) {
        waveOutReset(g_waveout);
        for (int i = 0; i < AUDIO_BUFFERS; i++)
            waveOutUnprepareHeader(g_waveout, &g_wavehdr[i], sizeof(WAVEHDR));
        waveOutClose(g_waveout);
        g_waveout = nullptr;
    }
    DeleteCriticalSection(&g_audio_cs);
}

static void AudioPump() {
    if (!g_waveout) return;

    const size_t want = AUDIO_FRAMES_PER_BUF * 2;   // int16 values per buffer

    for (int i = 0; i < AUDIO_BUFFERS; i++) {
        if (g_wave_inuse[i]) {
            if (g_wavehdr[i].dwFlags & WHDR_DONE) g_wave_inuse[i] = false;
            else continue;
        }

        size_t got = 0;
        EnterCriticalSection(&g_audio_cs);
        size_t have = g_audio_ring.size();
        got = have < want ? have : want;
        if (got) {
            memcpy(g_wavebuf[i], g_audio_ring.data(), got * sizeof(int16_t));
            g_audio_ring.erase(g_audio_ring.begin(), g_audio_ring.begin() + got);
        }
        LeaveCriticalSection(&g_audio_cs);

        // Nothing buffered: leave the remaining buffers free rather than
        // padding with silence (padding is what causes the clicks).
        if (got == 0) break;

        // Submit exactly what we have (a short buffer is fine; dwBufferLength
        // bounds playback), so the stream never contains injected silence.
        g_wavehdr[i].dwBufferLength = (DWORD)(got * sizeof(int16_t));
        g_wavehdr[i].dwFlags &= ~WHDR_DONE;
        if (waveOutWrite(g_waveout, &g_wavehdr[i], sizeof(WAVEHDR)) == MMSYSERR_NOERROR)
            g_wave_inuse[i] = true;
    }
}

// =============================================================================
// Emulation thread
// =============================================================================

static DWORD WINAPI EmuThreadProc(LPVOID param) {
    (void)param;

    // Raise the timer resolution to 1 ms. Without this, Sleep() has ~15.6 ms
    // granularity and the ~16.7 ms frame loop is far too jittery, which shows up
    // as uneven video and audio buffer underruns.
    timeBeginPeriod(1);

    LARGE_INTEGER freq, next, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&next);

    const double video_seconds = 1.0 / 60.0988;   // NTSC fallback when no audio

    while (!g_quit) {
        EmuCoreHandle core = nullptr;
        const EmuCoreModule* mod = nullptr;

        EnterCriticalSection(&g_core_cs);
        core = g_core;
        mod = g_module;
        LeaveCriticalSection(&g_core_cs);

        if (!core || !mod) {
            Sleep(10);
            QueryPerformanceCounter(&next);
            continue;
        }

        if (!g_paused) {
            // A frame can emit audio over several callbacks (the SNES core
            // flushes every 512 frames), so total this frame's samples and only
            // then update the smoothed samples-per-frame used for pacing below.
            g_frame_audio = 0;

            EnterCriticalSection(&g_core_cs);
            if (g_core && g_module) {
                mod->run_frame(core);
                g_frame_count++;
            }
            LeaveCriticalSection(&g_core_cs);

            if (g_frame_audio > 0) {
                double cur = (double)g_frame_audio;
                g_spf = (g_spf == 0.0) ? cur : (g_spf * 0.9 + cur * 0.1);
            }

            AudioPump();

            // Pace to the audio device when we have one: the core emits a fixed
            // number of samples per emulated frame, so one frame should last
            // exactly samples_per_frame / sample_rate seconds. Driving timing
            // from the audio clock keeps the ring at a steady level (no silence
            // padding, no drift-induced drops) and stays correct per console,
            // since the count is measured from the core itself.
            //
            // Exception: a self-paced core (mupen64plus) already blocks inside
            // run_frame() until the next console refresh, so it *is* the clock.
            // Pacing it again would double the frame period and corrupt the
            // audio-per-frame measurement, so we simply follow it.
            if (mod->flags & EMU_CORE_FLAG_SELF_PACED) {
                QueryPerformanceCounter(&next);
            } else {
                double frame_seconds = video_seconds;
                if (g_waveout && g_spf > 0.0) {
                    frame_seconds = g_spf / (double)AUDIO_SAMPLE_RATE;

                    // Safety valve: if the ring ever backs up past a few buffers,
                    // ease off slightly so it cannot grow into the 2 s drop cap.
                    size_t level;
                    EnterCriticalSection(&g_audio_cs);
                    level = g_audio_ring.size();
                    LeaveCriticalSection(&g_audio_cs);
                    if (level > (size_t)AUDIO_FRAMES_PER_BUF * 2 * 4)
                        frame_seconds *= 1.05;
                }

                next.QuadPart += (LONGLONG)(frame_seconds * (double)freq.QuadPart);
                QueryPerformanceCounter(&now);
                if (next.QuadPart > now.QuadPart) {
                    LONGLONG delta_us = (next.QuadPart - now.QuadPart) * 1000000 / freq.QuadPart;
                    if (delta_us > 0) Sleep((DWORD)(delta_us / 1000));
                } else {
                    next = now;   // fell behind; catch up immediately
                }
            }
        } else {
            Sleep(10);
            QueryPerformanceCounter(&next);
        }
    }

    timeEndPeriod(1);
    return 0;
}

// =============================================================================
// ROM loading
// =============================================================================

// Battery-backed save (SRAM / EEPROM / flash) belonging to the loaded game.
// One file per game, named after the ROM inside the archive, so that it stays
// the same wherever the game was loaded from.
static void CurrentRomSavePath(char* out, size_t out_size) {
    const char* sub = emu_console_folder_name(g_console);
    _snprintf(out, out_size - 1, "%s\\%s\\%s.sav", g_save_dir, sub, g_rom_base);
    out[out_size - 1] = 0;
}

static void UnloadCurrentRom() {
    EnterCriticalSection(&g_core_cs);
    if (g_core && g_module) {
        if (g_module->save_sram) {
            char sram[MAX_PATH * 2];
            CurrentRomSavePath(sram, sizeof(sram));
            g_module->save_sram(g_core, sram);
        }
        if (g_module->unload_rom) g_module->unload_rom(g_core);
        if (g_module->destroy) g_module->destroy(g_core);
    }
    g_core = nullptr;
    g_module = nullptr;
    LeaveCriticalSection(&g_core_cs);

    EnterCriticalSection(&g_audio_cs);
    g_audio_ring.clear();
    LeaveCriticalSection(&g_audio_cs);

    // Forget the previous core's measured audio rate; the next core re-measures.
    g_spf = 0.0;
    g_frame_audio = 0;
}

static void LoadRom(const char* path) {
    if (!path || !path[0]) return;

    // ROMs are frequently distributed inside .zip archives. This turns the
    // user's selection into a directly loadable file path (unpacking to a cache
    // directory when needed) and reports the real game file name.
    char cache_dir[MAX_PATH * 2];
    _snprintf(cache_dir, sizeof(cache_dir) - 1, "%s\\rom-cache", g_save_dir);
    cache_dir[sizeof(cache_dir) - 1] = 0;

    char display[520] = "";
    char* load_path = emu_prepare_rom(path, cache_dir, display, sizeof(display));
    if (!load_path) return;

    EmuDetectResult det;
    memset(&det, 0, sizeof(det));
    EmuConsoleType console = emu_detect_console_from_file(load_path, &det);

    if (console == EMU_CONSOLE_UNKNOWN) {
        free(load_path);
        MessageBoxW(g_hwnd,
            L"Could not determine the console for this ROM.\n"
            L"The file may be unsupported or corrupted.",
            APP_TITLE, MB_OK | MB_ICONWARNING);
        return;
    }

    const EmuCoreModule* mod = emu_registry_find_for_console(console);
    if (!mod) {
        free(load_path);
        wchar_t msg[512];
        _snwprintf(msg, 511,
            L"No core is compiled in for %s.\n\n"
            L"Rebuild with the matching BUILD_*_CORE option enabled.",
            Utf8ToWide(emu_console_name(console)).c_str());
        msg[511] = 0;
        MessageBoxW(g_hwnd, msg, APP_TITLE, MB_OK | MB_ICONWARNING);
        return;
    }

    // Identify the game by the ROM inside the archive (not the .zip) so that
    // save states / battery saves get a meaningful, stable name.  The .sav
    // that is read back below is named after it, so it is computed here --
    // but not copied into g_rom_base until UnloadCurrentRom() has had the
    // chance to flush the *previous* game's save.
    char rom_base[256];
    const char* name_src = display[0] ? display : path;
    emu_get_basename(name_src, rom_base, sizeof(rom_base));
    emu_strip_extension(rom_base, rom_base, sizeof(rom_base));

    UnloadCurrentRom();

    strncpy(g_rom_base, rom_base, sizeof(g_rom_base) - 1);
    g_rom_base[sizeof(g_rom_base) - 1] = 0;
    strncpy(g_rom_path, path, sizeof(g_rom_path) - 1);
    g_rom_path[sizeof(g_rom_path) - 1] = 0;

    EmuConfig cfg;
    emu_config_default(&cfg);
    cfg.save_dir = g_save_dir;
    cfg.audio_sample_rate = AUDIO_SAMPLE_RATE;

    EnterCriticalSection(&g_core_cs);
    g_core = mod->create(&cfg, VideoCallback, AudioCallback, nullptr,
                         InputStateCallback, nullptr);
    if (!g_core) {
        LeaveCriticalSection(&g_core_cs);
        free(load_path);
        MessageBoxW(g_hwnd, L"Failed to create the emulator core.", APP_TITLE,
                    MB_OK | MB_ICONERROR);
        return;
    }

    EmuLoadResult res = mod->load_rom_file(g_core, load_path);
    if (res != EMU_LOAD_OK) {
        mod->destroy(g_core);
        g_core = nullptr;
        LeaveCriticalSection(&g_core_cs);

        wchar_t msg[512];
        _snwprintf(msg, 511, L"Failed to load ROM:\n%hs\n\n%s",
                   path, emu_load_result_string(res));
        msg[511] = 0;
        MessageBoxW(g_hwnd, msg, APP_TITLE, MB_OK | MB_ICONERROR);
        free(load_path);
        return;
    }

    g_module = mod;
    g_console = console;
    g_rom_crc = det.crc32;
    g_frame_count = 0;

    // Bring back the game's battery-backed save (SRAM/EEPROM/flash).  This has
    // to run after the ROM is loaded -- cores that only discover the save
    // layout by watching the running game (GBA) need the ROM in place first --
    // and before the emulation thread is allowed to run a frame, which the
    // critical section guarantees.
    if (mod->load_sram) {
        char sram[MAX_PATH * 2];
        CurrentRomSavePath(sram, sizeof(sram));
        mod->load_sram(g_core, sram);
    }

    LeaveCriticalSection(&g_core_cs);

    // Title: prefer the core's detection, else the file name.
    const EmuCoreInfo* info = mod->get_info ? mod->get_info() : nullptr;
    if (det.details[0])
        strncpy(g_rom_title, det.details, sizeof(g_rom_title) - 1);
    else
        strncpy(g_rom_title, g_rom_base, sizeof(g_rom_title) - 1);
    g_rom_title[sizeof(g_rom_title) - 1] = 0;

    emu_state_mgr_set_game(g_state_mgr, name_src, det.crc32, console);

    free(load_path);

    SetStatus(L"%s  -  %s", Utf8ToWide(g_rom_base).c_str(),
              Utf8ToWide(emu_console_name(console)).c_str());
    (void)info;
}

static void ShowOpenRomDialog() {
    char file[MAX_PATH * 4] = "";

    static const char filter[] =
        "All Supported ROMs\0"
        "*.nes;*.fds;*.unf;*.nsf;*.sfc;*.smc;*.fig;*.swc;"
        "*.gb;*.gbc;*.gba;*.nds;*.z64;*.n64;*.v64;*.iso;*.gcm;*.3ds;*.cia;*.zip\0"
        "ROM Archives (.zip)\0*.zip\0"
        "NES\0*.nes;*.fds;*.unf;*.nsf\0"
        "Super Nintendo\0*.sfc;*.smc;*.fig;*.swc\0"
        "Game Boy / Color\0*.gb;*.gbc\0"
        "Game Boy Advance\0*.gba\0"
        "Nintendo DS\0*.nds\0"
        "Nintendo 64\0*.z64;*.n64;*.v64\0"
        "GameCube / Wii\0*.iso;*.gcm\0"
        "Nintendo 3DS\0*.3ds;*.cia\0"
        "All Files\0*.*\0"
        "\0";

    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g_hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = sizeof(file);
    ofn.lpstrTitle  = "Load ROM";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn))
        LoadRom(file);
}

// =============================================================================
// Save states
// =============================================================================

static void SaveState(int slot) {
    if (!g_core || !g_module) { SetStatus(L"No game loaded"); return; }

    size_t need = g_module->save_state(g_core, nullptr, 0);
    if (need == 0) { SetStatus(L"Save states are not supported by this core"); return; }

    std::vector<uint8_t> buf(need);
    size_t written = g_module->save_state(g_core, buf.data(), buf.size());
    if (written == 0) { SetStatus(L"Failed to capture save state"); return; }

    int rc = emu_state_mgr_save(g_state_mgr, slot, buf.data(), written,
                                g_module->id, g_rom_title, g_frame_count,
                                EMU_STATE_FLAG_QUICKSAVE);
    if (rc == 0) SetStatus(L"State saved to slot %d", slot);
    else         SetStatus(L"Could not write save state (slot %d)", slot);
}

static void LoadState(int slot) {
    if (!g_core || !g_module) { SetStatus(L"No game loaded"); return; }

    EmuStateHeader hdr;
    size_t payload_size = 0;
    uint8_t* payload = emu_state_mgr_load(g_state_mgr, slot, &hdr, &payload_size);
    if (!payload) { SetStatus(L"No save state in slot %d", slot); return; }

    int ok = g_module->load_state(g_core, payload, payload_size);
    free(payload);
    SetStatus(ok ? L"State loaded from slot %d" : L"Failed to load state (slot %d)", slot);
}

// =============================================================================
// Menus
// =============================================================================

static HMENU BuildMenu() {
    HMENU menu = CreateMenu();

    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_FILE_LOAD, L"&Load ROM...\tCtrl+O");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_FILE_EXIT, L"E&xit");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)file, L"&File");

    HMENU emu = CreatePopupMenu();
    AppendMenuW(emu, MF_STRING, IDM_EMU_PAUSE, L"&Pause\tSpace");
    AppendMenuW(emu, MF_STRING, IDM_EMU_RESET, L"&Reset\tF2");
    AppendMenuW(emu, MF_STRING, IDM_EMU_POWER_CYCLE, L"&Power Cycle");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)emu, L"&Emulation");

    HMENU states = CreatePopupMenu();
    HMENU save = CreatePopupMenu();
    HMENU load = CreatePopupMenu();
    for (int i = 1; i <= 9; i++) {
        wchar_t label[8];
        _snwprintf(label, 7, L"Slot &%d", i);
        AppendMenuW(save, MF_STRING, IDM_STATE_SAVE_SLOT + i, label);
        AppendMenuW(load, MF_STRING, IDM_STATE_LOAD_SLOT + i, label);
    }
    AppendMenuW(states, MF_POPUP, (UINT_PTR)save, L"&Save State");
    AppendMenuW(states, MF_POPUP, (UINT_PTR)load, L"&Load State");
    AppendMenuW(states, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(states, MF_STRING, IDM_STATE_SAVE, L"&Quick Save\tF5");
    AppendMenuW(states, MF_STRING, IDM_STATE_LOAD, L"Quick &Load\tF8");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)states, L"&States");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, IDM_HELP_ABOUT, L"&About");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)help, L"&Help");

    return menu;
}

static void ShowAbout() {
    char cores[512] = "";
    int n = emu_registry_count();
    for (int i = 0; i < n; i++) {
        const EmuCoreModule* m = emu_registry_get(i);
        if (!m || !m->get_info) continue;
        const EmuCoreInfo* info = m->get_info();
        if (!info) continue;
        if (cores[0]) strncat(cores, "\n", sizeof(cores) - strlen(cores) - 1);
        strncat(cores, info->name, sizeof(cores) - strlen(cores) - 1);
        strncat(cores, " - ", sizeof(cores) - strlen(cores) - 1);
        strncat(cores, emu_console_name(info->console),
                sizeof(cores) - strlen(cores) - 1);
    }
    if (!cores[0]) strcpy(cores, "(no cores compiled in)");

    wchar_t msg[768];
    _snwprintf(msg, 767,
        L"Nintendall\n\n"
        L"One executable, many Nintendo consoles.\n\n"
        L"Compiled-in cores:\n%hs",
        cores);
    msg[767] = 0;
    MessageBoxW(g_hwnd, msg, APP_TITLE, MB_OK | MB_ICONINFORMATION);
}

// =============================================================================
// Window procedure
// =============================================================================

// (Re)create the offscreen buffer used to compose a frame before it is shown.
static void EnsureBackBuffer(HDC ref_dc, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (g_back_dc && g_back_w == w && g_back_h == h) return;

    if (!g_back_dc) {
        g_back_dc = CreateCompatibleDC(ref_dc);
        if (!g_back_dc) return;
    }
    if (g_back_bmp) {
        SelectObject(g_back_dc, g_back_old);
        DeleteObject(g_back_bmp);
        g_back_bmp = nullptr;
    }
    g_back_bmp = CreateCompatibleBitmap(ref_dc, w, h);
    if (!g_back_bmp) return;

    g_back_old = (HBITMAP)SelectObject(g_back_dc, g_back_bmp);
    g_back_w = w;
    g_back_h = h;
}

static void DestroyBackBuffer() {
    if (g_back_dc) {
        if (g_back_bmp) SelectObject(g_back_dc, g_back_old);
        if (g_back_bmp) DeleteObject(g_back_bmp);
        DeleteDC(g_back_dc);
    }
    g_back_dc  = nullptr;
    g_back_bmp = nullptr;
    g_back_old = nullptr;
    g_back_w = g_back_h = 0;
}

static void TogglePause() {
    g_paused = !g_paused;
    EnterCriticalSection(&g_core_cs);
    if (g_core && g_module && g_module->set_paused)
        g_module->set_paused(g_core, (int)g_paused);
    LeaveCriticalSection(&g_core_cs);
    SetStatus(g_paused ? L"Paused" : L"Running");
}

// =============================================================================
// Borderless fullscreen
// =============================================================================
//
// F11 swaps the window between its normal framed state and a borderless,
// menu-less window that covers the whole monitor.  Two things have to happen
// in the right order for the decorations to actually disappear:
//
//   1. The menu has to be detached with SetMenu(hwnd, NULL).  A window keeps
//      drawing its menu bar as part of the non-client area for as long as it
//      owns an HMENU, so clearing WS_CAPTION alone still leaves a bar across
//      the top of the screen.
//   2. The window style has to be replaced wholesale with WS_POPUP instead of
//      having bits masked off.  Masking leaves the window an overlapped
//      window that Windows is free to keep giving a resize border.
//
// The saved style/ex-style/menu/placement are restored verbatim on the way
// back out, so toggling repeatedly cannot corrupt the windowed state.

static BOOL   g_fullscreen = FALSE;
static LONG   g_savedStyle = 0;
static LONG   g_savedExStyle = 0;
static HMENU  g_savedMenu = nullptr;
static RECT   g_savedRect = { 0, 0, 0, 0 };

static void SetFullscreen(HWND hwnd, BOOL on) {
    if (on == g_fullscreen) return;

    if (on) {
        // Remember exactly how the windowed window looked.
        g_savedStyle   = GetWindowLongW(hwnd, GWL_STYLE);
        g_savedExStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
        g_savedMenu    = GetMenu(hwnd);
        GetWindowRect(hwnd, &g_savedRect);

        // Detach the menu first, or its bar survives into fullscreen.
        SetMenu(hwnd, nullptr);

        // Strip every decoration and leave a plain popup window.
        SetWindowLongW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        // WS_EX_APPWINDOW would otherwise keep a taskbar button/shell frame.
        SetWindowLongW(hwnd, GWL_EXSTYLE, g_savedExStyle & ~WS_EX_APPWINDOW);

        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi)) {
            SetWindowPos(hwnd, HWND_TOPMOST,
                         mi.rcMonitor.left,  mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
        }
        g_fullscreen = TRUE;
    } else {
        // Put the decorations, the menu and the original geometry back.
        SetWindowLongW(hwnd, GWL_STYLE,   g_savedStyle);
        SetWindowLongW(hwnd, GWL_EXSTYLE, g_savedExStyle);
        if (g_savedMenu) SetMenu(hwnd, g_savedMenu);

        SetWindowPos(hwnd, HWND_NOTOPMOST,
                     g_savedRect.left, g_savedRect.top,
                     g_savedRect.right - g_savedRect.left,
                     g_savedRect.bottom - g_savedRect.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_NOACTIVATE);

        g_fullscreen = FALSE;
        g_savedMenu  = nullptr;
    }

    // The client area just changed size; drop the cached back buffer so the
    // next WM_PAINT rebuilds it at the new resolution instead of stretching.
    DestroyBackBuffer();
    RedrawWindow(hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN | RDW_ERASE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            g_hwnd = hwnd;
            SetMenu(hwnd, BuildMenu());
            return 0;

        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            if (id == IDM_FILE_LOAD) { ShowOpenRomDialog(); return 0; }
            if (id == IDM_FILE_EXIT) { PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0; }

            if (id == IDM_EMU_PAUSE) { TogglePause(); return 0; }
            if (id == IDM_EMU_RESET) {
                EnterCriticalSection(&g_core_cs);
                if (g_core && g_module && g_module->reset) g_module->reset(g_core);
                LeaveCriticalSection(&g_core_cs);
                return 0;
            }
            if (id == IDM_EMU_POWER_CYCLE) {
                EnterCriticalSection(&g_core_cs);
                if (g_core && g_module && g_module->power_cycle) g_module->power_cycle(g_core);
                LeaveCriticalSection(&g_core_cs);
                return 0;
            }

            if (id == IDM_STATE_SAVE) { SaveState(1); return 0; }
            if (id == IDM_STATE_LOAD) { LoadState(1); return 0; }
            if (id >= IDM_STATE_SAVE_SLOT + 1 && id <= IDM_STATE_SAVE_SLOT + 9) {
                SaveState(id - IDM_STATE_SAVE_SLOT); return 0;
            }
            if (id >= IDM_STATE_LOAD_SLOT + 1 && id <= IDM_STATE_LOAD_SLOT + 9) {
                LoadState(id - IDM_STATE_LOAD_SLOT); return 0;
            }

            if (id == IDM_HELP_ABOUT) { ShowAbout(); return 0; }
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;   // handled in WM_PAINT to avoid flicker

        case WM_KEYDOWN:
            if (wParam == VK_F11) {
                SetFullscreen(hwnd, !g_fullscreen);
                return 0;
            }
            if (wParam == VK_ESCAPE && g_fullscreen) {
                SetFullscreen(hwnd, FALSE);
                return 0;
            }
            break;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            RECT rc;
            GetClientRect(hwnd, &rc);
            const int cw = rc.right - rc.left;
            const int ch = rc.bottom - rc.top;

            // Compose the whole frame offscreen, then present it with one
            // BitBlt. Drawing straight to the window DC (black fill, then a
            // stretch blit) is what made the display flash black.
            EnsureBackBuffer(hdc, cw, ch);
            HDC dc = g_back_dc ? g_back_dc : hdc;

            HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
            FillRect(dc, &rc, black);

            EnterCriticalSection(&g_video_cs);
            if (g_frame_w > 0 && g_frame_h > 0 && !g_frame.empty()) {
                // The target display ratio for this console, falling back to the
                // framebuffer's own ratio for anything not listed.
                double want = emu_console_display_aspect(g_console);
                if (want <= 0.0) want = (double)g_frame_w / (double)g_frame_h;

                // Some cores hand back a buffer that includes blanking rows - the
                // N64 VI emits 640x625 for a 640x480 picture.  Stretching the
                // whole buffer squashes the image vertically, so crop away any
                // rows beyond the target ratio first.
                int sx = 0, sy = 0, sw = g_frame_w, sh = g_frame_h;
                const double buf_ratio = (double)sw / (double)sh;
                if (buf_ratio < want) {
                    // Buffer is too tall: keep the middle of the picture.
                    sh = (int)((double)sw / want + 0.5);
                    if (sh > g_frame_h) sh = g_frame_h;
                    sy = (g_frame_h - sh) / 2;
                } else if (buf_ratio > want) {
                    // Buffer is too wide: keep the middle horizontally.
                    sw = (int)((double)sh * want + 0.5);
                    if (sw > g_frame_w) sw = g_frame_w;
                    sx = (g_frame_w - sw) / 2;
                }

                // Fit the cropped picture into the window at the target ratio.
                int dw = cw;
                int dh = (int)(dw / want + 0.5);
                if (dh > ch) {
                    dh = ch;
                    dw = (int)(dh * want + 0.5);
                }
                int dx = (cw - dw) / 2;
                int dy = (ch - dh) / 2;

                BITMAPINFO bmi;
                ZeroMemory(&bmi, sizeof(bmi));
                bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
                bmi.bmiHeader.biWidth       = g_frame_w;
                bmi.bmiHeader.biHeight      = -g_frame_h;   // top-down
                bmi.bmiHeader.biPlanes      = 1;
                bmi.bmiHeader.biBitCount    = 32;
                bmi.bmiHeader.biCompression = BI_RGB;

                SetStretchBltMode(dc, COLORONCOLOR);
                StretchDIBits(dc, dx, dy, dw, dh, sx, sy, sw, sh,
                              g_frame.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
            }
            LeaveCriticalSection(&g_video_cs);

            if (g_back_dc)
                BitBlt(hdc, 0, 0, cw, ch, g_back_dc, 0, 0, SRCCOPY);

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_CLOSE:
            g_quit = 1;
            if (g_emu_thread) {
                WaitForSingleObject(g_emu_thread, 2000);
                CloseHandle(g_emu_thread);
                g_emu_thread = nullptr;
            }
            UnloadCurrentRom();
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            DestroyBackBuffer();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// =============================================================================
// Entry point
// =============================================================================

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, PWSTR cmdLine, int show) {
    (void)hPrev;

    // Save directory next to the executable.
    {
        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        char* slash = strrchr(exe, '\\');
        if (slash) *slash = 0;
        _snprintf(g_save_dir, sizeof(g_save_dir) - 1, "%s\\saves", exe);
        g_save_dir[sizeof(g_save_dir) - 1] = 0;
    }
    emu_mkdir_p(g_save_dir);

    InitializeCriticalSection(&g_video_cs);
    InitializeCriticalSection(&g_core_cs);

    g_state_mgr = emu_state_mgr_create(g_save_dir);

    AudioInit(nullptr);

    // Register window class.
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"NintendallWindow";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0, L"NintendallWindow", APP_TITLE,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 960, 720,
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) return 1;

    g_hwnd = hwnd;
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    // Accelerators.
    ACCEL accels[] = {
        { FVIRTKEY | FCONTROL, 'O', IDM_FILE_LOAD },
        { FVIRTKEY, VK_F5, IDM_STATE_SAVE },
        { FVIRTKEY, VK_F8, IDM_STATE_LOAD },
        { FVIRTKEY, VK_F2, IDM_EMU_RESET },
        { FVIRTKEY, VK_SPACE, IDM_EMU_PAUSE },
    };
    HACCEL haccel = CreateAcceleratorTableW(accels, sizeof(accels) / sizeof(accels[0]));

    g_emu_thread = CreateThread(nullptr, 0, EmuThreadProc, nullptr, 0, nullptr);

    // Optionally accept a ROM path on the command line.
    if (cmdLine && cmdLine[0]) {
        std::wstring w(cmdLine);
        if (w.size() >= 2 && w.front() == L'"' && w.back() == L'"')
            w = w.substr(1, w.size() - 2);
        char narrow[MAX_PATH * 4];
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, narrow, sizeof(narrow), nullptr, nullptr);
        LoadRom(narrow);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (haccel && TranslateAcceleratorW(hwnd, haccel, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_quit = 1;
    if (g_emu_thread) {
        WaitForSingleObject(g_emu_thread, 2000);
        CloseHandle(g_emu_thread);
    }
    UnloadCurrentRom();

    if (haccel) DestroyAcceleratorTable(haccel);
    AudioShutdown();
    emu_state_mgr_destroy(g_state_mgr);
    DeleteCriticalSection(&g_video_cs);
    DeleteCriticalSection(&g_core_cs);

    return (int)msg.wParam;
}
