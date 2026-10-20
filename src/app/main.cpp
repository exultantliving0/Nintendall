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
#include "input_config.h"
#include "input_dialog.h"

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

#define IDM_EMU_CONTROLS       1104

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
#define AUDIO_FRAMES_PER_BUF 256         // stereo frames per WAVEHDR (~5.3 ms)
#define AUDIO_RING_FRAMES   (AUDIO_SAMPLE_RATE * 2)   // ~2 s of slack
#define AUDIO_TARGET_FRAMES (AUDIO_SAMPLE_RATE / 4)   // aim to keep ~250 ms queued

// Enough audio buffered before playback starts, and the level below which we
// stop feeding the device so the cushion can rebuild.  Paced cores produce
// roughly what the device consumes, so without a deliberate cushion the ring
// sits near empty and playback stutters.  Measured on N64 the ring
// oscillated between 0 and 530 frames - about 10 ms - without this.
#define AUDIO_PRIME_FRAMES  (AUDIO_SAMPLE_RATE / 10)   // ~100 ms
#define AUDIO_LOW_WATER     (AUDIO_SAMPLE_RATE / 20)   // ~50 ms

static HWAVEOUT  g_waveout = nullptr;
static WAVEHDR   g_wavehdr[AUDIO_BUFFERS];
static int16_t   g_wavebuf[AUDIO_BUFFERS][AUDIO_FRAMES_PER_BUF * 2];
static bool      g_wave_inuse[AUDIO_BUFFERS];
static CRITICAL_SECTION g_audio_cs;

// A plain ring with a head index.  This used to be a std::vector drained with
// erase(begin, begin+n), which memmoved the whole remainder of the buffer every
// single frame - up to 384 KB a frame, i.e. ~23 MB/s of pointless copying on
// the emulation thread.  Advancing an index is O(1).
static int16_t   g_ring[AUDIO_RING_FRAMES * 2];
static size_t    g_ring_head = 0;    // next write
static size_t    g_ring_tail = 0;    // next read
static size_t    g_ring_count = 0;   // int16 values waiting

static double                   g_spf = 0.0;   // smoothed stereo frames the core emits per emulated frame
static long long                g_frame_audio = 0;  // stereo frames emitted so far this eumulated frame
static int                      g_underruns = 0;   // times the ring ran dry

// Wall-clock cost of run_frame(), accumulated since the last audit and read by
// tools\bench-cores.ps1.  Paired with the CPU the script samples externally,
// this separates an emulator-bound core from a frontend-bound one.
static double g_frame_wall_us = 0, g_frame_wall_max = 0;
static long long g_frame_samples = 0;
static bool                     g_audio_primed = false;  // device has a cushion queued

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

// Video-mode diagnostic: records each distinct framebuffer size so the real
// output of a core can be inspected from a log rather than guessed at.
static char                     g_video_log_path[MAX_PATH] = "";
static int                      g_logged_w = 0;
static int                      g_logged_h = 0;

// True once a saved mapping has been read, so defaults are only applied for
// consoles the user has not configured yet.
static bool                     g_input_loaded = false;

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
    // Keep the title bar fixed at "Nintendall" - status messages stay in the
    // status area only and never change the window title.
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

    // One line per distinct framebuffer size, so the video mode a core is
    // actually producing can be inspected without a debugger.
    if (width != g_logged_w || height != g_logged_h) {
        g_logged_w = width;
        g_logged_h = height;
        char line[256];
        _snprintf(line, sizeof(line) - 1,
                  "console=%d framebuffer=%dx%d ratio=%.4f\r\n",
                  (int)g_console, width, height, (double)width / (double)height);
        line[sizeof(line) - 1] = 0;
        FILE* f = fopen(g_video_log_path, "ab");
        if (f) { fputs(line, f); fclose(f); }
    }
    LeaveCriticalSection(&g_video_cs);

    // One line per second of emulated time, recording how the audio ring is
    // behaving.  Underruns - the ring running dry - are what "the audio is not
    // working" looks like from the inside, and drift shows up as the level
    // steadily walking up or down.
    {
        static int countdown = 0;
        if (--countdown <= 0 && g_video_log_path[0]) {
            countdown = 60;
            size_t level;
            EnterCriticalSection(&g_audio_cs);
            level = g_ring_count / 2;
            LeaveCriticalSection(&g_audio_cs);

            char line[224];
            static int prev_frames = 0;
            int queued = 0;
            for (int i = 0; i < AUDIO_BUFFERS; i++)
                if (g_wave_inuse[i]) queued++;
            int fps = (int)(g_frame_count - (long long)prev_frames);
            prev_frames = (int)g_frame_count;

            _snprintf(line, sizeof(line) - 1,
                      "audio level=%zu (%.0f ms) spf=%.1f underruns=%d fps=%d queued=%d primed=%d"
                      " | frame wall=%.2f ms wallmax=%.1f\r\n",
                      level, (double)level * 1000.0 / AUDIO_SAMPLE_RATE,
                      g_spf, g_underruns, fps, queued, g_audio_primed ? 1 : 0,
                      g_frame_samples ? g_frame_wall_us / g_frame_samples / 1000.0 : 0.0,
                      g_frame_wall_max / 1000.0);

            g_frame_wall_us = g_frame_wall_max = 0;
            g_frame_samples = 0;
            line[sizeof(line) - 1] = 0;
            FILE* f = fopen(g_video_log_path, "ab");
            if (f) { fputs(line, f); fclose(f); }
        }
    }

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

    // Cap the ring at ~2 s so a stalled device cannot grow without bound. When
    // it is full, the oldest audio is dropped rather than the newest: keeping
    // up with real time matters more than playing the backlog.
    EnterCriticalSection(&g_audio_cs);
    size_t n = (size_t)count * 2;                 // int16 values
    if (g_ring_count + n > (size_t)AUDIO_RING_FRAMES * 2) {
        size_t drop = g_ring_count + n - (size_t)AUDIO_RING_FRAMES * 2;
        g_ring_tail = (g_ring_tail + drop) % ((size_t)AUDIO_RING_FRAMES * 2);
        g_ring_count -= drop;
    }
    for (size_t i = 0; i < n; i++) {
        g_ring[g_ring_head] = samples[i];
        g_ring_head = (g_ring_head + 1) % ((size_t)AUDIO_RING_FRAMES * 2);
    }
    g_ring_count += n;
    LeaveCriticalSection(&g_audio_cs);
}

static int InputStateCallback(void* userdata, unsigned port, unsigned device,
                              unsigned index, unsigned id) {
    (void)userdata; (void)index;
    if (device != EMU_DEVICE_JOYPAD && device != EMU_DEVICE_NONE) return 0;

    // Player 1 answers to the configured mapping; the NES light gun is driven by
    // the mouse rather than a button.
    if (id == EMU_ID_POINTER_PRESSED) {
        return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0;
    }
    if (port > INPUT_MAX_PORTS - 1) return 0;
    return InputSystem::get().is_pressed(port, (EmuInputId)id);
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
    bool any_queued = false;

    // Cushion management.
    //
    // The core produces roughly the same number of samples per frame as the
    // device consumes, so the ring settles wherever it starts - measured at
    // only 0..530 frames on N64, far too little to ride out anything.  So
    // playback is held off until there is a real cushion buffered, and if the
    // ring later falls below the low-water mark we stop feeding the device for
    // a frame or two to let it rebuild.  The buffers already queued with the
    // device cover that gap, so it is not audible.
    {
        size_t level;
        EnterCriticalSection(&g_audio_cs);
        level = g_ring_count / 2;
        LeaveCriticalSection(&g_audio_cs);

        if (!g_audio_primed) {
            if (level < AUDIO_PRIME_FRAMES)
                return;
            g_audio_primed = true;
        } else if (level < AUDIO_LOW_WATER) {
            return;
        }
    }

    for (int i = 0; i < AUDIO_BUFFERS; i++) {
        if (g_wave_inuse[i]) {
            if (g_wavehdr[i].dwFlags & WHDR_DONE) g_wave_inuse[i] = false;
            else { any_queued = true; continue; }
        }

        // Never inject silence or a held sample into real audio: both are
        // audible as a click or a warble.  Whatever the core actually produced
        // is what gets played.
        size_t got;
        EnterCriticalSection(&g_audio_cs);
        got = g_ring_count < want ? g_ring_count : want;
        if (got) {
            size_t p = g_ring_tail;
            for (size_t k = 0; k < got; k++) {
                g_wavebuf[i][k] = g_ring[p];
                if (++p == (size_t)AUDIO_RING_FRAMES * 2) p = 0;
            }
            g_ring_tail = p;
            g_ring_count -= got;
        }
        LeaveCriticalSection(&g_audio_cs);

        if (got == 0) {
            if (!any_queued) g_underruns++;
            break;      // nothing produced this frame; leave the rest idle
        }

        g_wavehdr[i].dwBufferLength = (DWORD)(got * sizeof(int16_t));
        g_wavehdr[i].dwFlags &= ~WHDR_DONE;
        if (waveOutWrite(g_waveout, &g_wavehdr[i], sizeof(WAVEHDR)) == MMSYSERR_NOERROR) {
            g_wave_inuse[i] = true;
            any_queued = true;
        }
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

        // Sample the pads once per frame, before the core asks about buttons.
        InputSystem::get().poll();

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
                /* Time run_frame(), so an emulator-bound core can be told from
                 * a frontend-bound one.  Per-frame CPU is measured from outside
                 * by tools\bench-cores.ps1, which reads the process counters
                 * directly; doing it in here reported zero, and a number that is
                 * always zero is worse than no number at all. */
                LARGE_INTEGER w0, w1;

                QueryPerformanceCounter(&w0);

                mod->run_frame(core);
                g_frame_count++;

                QueryPerformanceCounter(&w1);

                const double wall_us =
                    (double)(w1.QuadPart - w0.QuadPart) * 1000000.0 / (double)freq.QuadPart;

                g_frame_wall_us += wall_us;
                if (wall_us > g_frame_wall_max) g_frame_wall_max = wall_us;
                g_frame_samples++;
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
                    level = g_ring_count / 2;   // stereo frames waiting
                    LeaveCriticalSection(&g_audio_cs);
                    if (level > (size_t)AUDIO_TARGET_FRAMES * 2)
                        frame_seconds *= 1.05;
                }

                next.QuadPart += (LONGLONG)(frame_seconds * (double)freq.QuadPart);
                QueryPerformanceCounter(&now);
                if (next.QuadPart > now.QuadPart) {
                    LONGLONG delta_ticks = next.QuadPart - now.QuadPart;
                    double delta_us = (double)delta_ticks * 1000000.0 / (double)freq.QuadPart;
                    // Sleep for all but the last millisecond, then spin.  Sleep
                    // is only accurate to about a millisecond even with
                    // timeBeginPeriod(1), and at 60 Hz a whole millisecond of
                    // error every frame is enough to starve the audio ring.
                    if (delta_us > 1500.0) {
                        Sleep((DWORD)((delta_us - 1000.0) / 1000.0));
                        QueryPerformanceCounter(&now);
                    }
                    while (next.QuadPart > now.QuadPart) {
                        QueryPerformanceCounter(&now);
                    }
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
    g_ring_head = g_ring_tail = g_ring_count = 0;
    LeaveCriticalSection(&g_audio_cs);
    // The next game has to fill a fresh cushion before it is heard.
    g_audio_primed = false;

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

    // Each console has its own pad, so start from its defaults unless the user
    // already has a saved mapping.
    if (!g_input_loaded)
        InputSystem::get().apply_all_defaults(console);
    InputSystem::get().set_active_console(console);

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
    AppendMenuW(emu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(emu, MF_STRING, IDM_EMU_CONTROLS, L"&Configure Controls...");
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
            if (id == IDM_EMU_CONTROLS) {
                if (ShowInputDialog(hwnd, g_console)) {
                    char path[MAX_PATH];
                    _snprintf(path, sizeof(path) - 1, "%s\\input.cfg", g_save_dir);
                    InputSystem::get().save(path);
                    SetStatus(L"Controls saved");
                }
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
                    // Taller than the picture should look: the buffer carries
                    // blanking rows.  Crop them away, vertically only, from the
                    // offset the console's VI actually puts the picture at.
                    // Nothing is known about horizontal blanking, so the other
                    // axis is never trimmed - cutting it eats real picture.
                    sh = (int)((double)sw / want + 0.5);
                    if (sh > g_frame_h) sh = g_frame_h;
                    sy = emu_console_active_top(g_console, g_frame_h, sh);
                }
                // Wider than the target ratio needs no crop: the whole buffer is
                // scaled into a box of the target shape.  This is the normal case
                // for the N64's 240p mode, which arrives as 640x240 (8:3) and is
                // shown on a 4:3 screen.

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

    // Video-mode log, next to the saves folder.
    {
        char* slash = strrchr(g_save_dir, '\\');
        if (slash) *slash = 0;
        _snprintf(g_video_log_path, sizeof(g_video_log_path) - 1,
                  "%s\\video-modes.log", g_save_dir);
        g_video_log_path[sizeof(g_video_log_path) - 1] = 0;
        if (slash) *slash = '\\';
    }

    // Controller mapping.  Load the saved table when there is one; otherwise
    // fall back to sensible defaults for whatever console is loaded later.
    {
        char input_cfg[MAX_PATH];
        _snprintf(input_cfg, sizeof(input_cfg) - 1, "%s\\input.cfg", g_save_dir);
        input_cfg[sizeof(input_cfg) - 1] = 0;
        g_input_loaded = InputSystem::get().load(input_cfg);
    }

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
