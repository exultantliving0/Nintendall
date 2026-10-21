/**
 * Nintendall - mupen64plus glue layer
 *
 * Bridges the vendored mupen64plus-core to the Nintendall N64 adapter.  See
 * n64_glue.h for the contract.
 *
 * Layout of the bridge:
 *
 *   CoreStartup()
 *        |
 *   M64CMD_ROM_OPEN
 *        |
 *   n64_glue_attach()  -> assign gfx/audio/input/rsp to our tables and call
 *                         plugin_start() so plugin.c fills its *_info structs
 *                         and calls our initiate*() functions
 *        |
 *   emulation thread: M64CMD_EXECUTE -> main_run() -> run_device()
 *        |
 *   every VI: gfx.updateScreen() -> n64video_update_screen() (software RDP)
 *                                 -> screen_write() into the capture buffer
 *                                 -> present_frame() (blocks until consumed)
 *
 * Pixel format
 *   The angrylion VI filter leaves 0x00RRGGBB words in its prescale buffer, so
 *   screen_write() simply ORs in an opaque alpha to produce the 0xAARRGGBB the
 *   frontend expects.
 *
 * Save states
 *   mupen64plus only snapshots the machine from a safe point (a VI or COMPARE
 *   interrupt) on the emulation thread.  The plugin-facing entry points only
 *   queue a job, so we queue the job and wait for the core's
 *   M64CORE_STATE_SAVECOMPLETE / LOADCOMPLETE callback, then hand the bytes
 *   back through the frontend's own state container.
 */

#include "n64_glue.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* mupen64plus public API.
 * M64P_CORE_PROTOTYPES must be defined so m64p_frontend.h / m64p_config.h
 * expose CoreStartup/CoreShutdown/CoreDoCommand/Config*.  */
#define M64P_CORE_PROTOTYPES 1
#include "m64p_types.h"
#include "m64p_config.h"
#include "m64p_frontend.h"
#include "m64p_plugin.h"

/* mupen64plus core internals */
#include "plugin/plugin.h"          /* gfx/audio/input/rsp, plugin_start, Controls */
#include "main/rom.h"               /* ROM_HEADER / ROM_PARAMS / ROM_SETTINGS / g_rom_size */
#include "main/version.h"           /* FRONTEND_API_VERSION */
#include "osal/files.h"             /* osal_get_user_datapath */
#include "device/r4300/recomp.h"    /* struct r4300_core + dynarec interface decls */
#include "device/rcp/vi/vi_controller.h"  /* vi_clock_from_tv_standard */

/* angrylion (software RDP) */
#include "n64video.h"
#include "screen.h"
#include "parallel_al.h"

/* Angrylion calls the parallel_* entry points unconditionally; config.parallel
 * decides at runtime whether the work is actually split.  With
 * N64_PARALLEL_VIDEO the real thread pool (parallel_al.cpp) is linked in.
 * Without it, no-op stubs satisfy the linker and every call runs inline. */
#ifndef N64_PARALLEL_VIDEO
void parallel_alinit(uint32_t num) { (void)num; }
void parallel_run(void task(uint32_t)) { if (task) task(0); }
uint32_t parallel_num_workers(void) { return 1; }
void parallel_close(void) {}
#endif

/* RSP entry layer (mupen64plus-rsp-cxd4 LLE interpreter - see n64_rsp.c) */
void n64_rsp_init(RSP_INFO* info);
void n64_rsp_execute(void);
void n64_rsp_rom_closed(void);

/* ------------------------------------------------------------------------- */
/* Constants                                                                 */
/* ------------------------------------------------------------------------- */

/* The VI prescale buffer is H_RES_NTSC x V_SYNC_PAL = 640 x 625, but PAL
 * interlaced modes can reach 768 x 640 across the filtered paths, so the
 * capture/deliver buffers are sized generously. */
#define N64_MAX_WIDTH   768u
#define N64_MAX_HEIGHT  640u
#define N64_MAX_PIXELS  (N64_MAX_WIDTH * N64_MAX_HEIGHT)

#define N64_DP_INTERRUPT 0x20u

/* mupen64plus SAVETYPE_* : EEPROM_4K is 0, so "no battery" is NONE (5). */
#define N64_SAVETYPE_NONE 5

#define N64_AUDIO_RATE        48000u
#define N64_AUDIO_RING_FRAMES 65536u   /* ~1.36 s at 48 kHz */
#define N64_AUDIO_CHUNK       4096u    /* frames handed over per callback */
#define N64_AUDIO_SRC_FRAMES  8192u    /* most native frames we consume at once */
#define N64_AUDIO_OUT_FRAMES  16384u   /* resampled output scratch size */

/* Where the drift correction aims.  The ring is drained once per emulated
 * frame, so this is a few frames of slack - not a fraction of the whole 1.36 s
 * buffer, which would leave the correction pinned at its limit and permanently
 * skew the resampling ratio. */
#define N64_DRIFT_TARGET_FRAMES 4096u

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

static n64_video_cb s_video_cb;
static n64_audio_cb s_audio_cb;
static void*        s_user;

static char s_data_path[MAX_PATH];
static char s_state_path[MAX_PATH];

static int  s_core_up;
static int  s_rom_open;
static HANDLE s_emu_thread;

/* Plugin info structs handed to us by plugin.c.  They are passed by value and
 * contain pointers into g_dev, which is a process global and therefore stable.
 * We hold our own copies. */
static GFX_INFO   s_gfx_info;
static AUDIO_INFO s_audio_info;
static int        s_gfx_up;
static int        s_audio_up;
static int        s_input_up;
static int        s_rsp_up;

static struct n64video_config s_vi_config;

static gfx_plugin_functions   s_gfx_table;
static audio_plugin_functions s_audio_table;
static input_plugin_functions s_input_table;
static rsp_plugin_functions   s_rsp_table;

/* ---- video handshake ---------------------------------------------------- */

static CRITICAL_SECTION s_lock;
static CONDITION_VARIABLE s_cond_frame;  /* core -> frontend: frame ready */
static CONDITION_VARIABLE s_cond_done;   /* frontend -> core: frame consumed */
static uint32_t s_presented;
static uint32_t s_consumed;
static int      s_exit;

/* Reset requests arrive on the frontend/UI thread, but main_reset() mutates
 * g_dev directly, so it must run on the emulation thread.  n64_glue_reset()
 * only records the request; gfx_UpdateScreen() (emulation thread) applies it at
 * the frame boundary. */
static volatile LONG s_pending_reset;   /* 0 none, 1 soft, 2 hard */

static uint32_t s_capture[N64_MAX_PIXELS];  /* written by screen_write (core) */
static uint32_t s_deliver[N64_MAX_PIXELS];  /* handed to the frontend */
static unsigned s_cap_w, s_cap_h;           /* capture dimensions */
static unsigned s_out_w, s_out_h;           /* last delivered dimensions */

/* ---- audio -------------------------------------------------------------- */

static int16_t s_ring[N64_AUDIO_RING_FRAMES * 2];
static unsigned s_ring_head;    /* write index (frames) */
static unsigned s_ring_tail;    /* read index (frames) */
static unsigned s_ring_count;

static int16_t s_src[N64_AUDIO_SRC_FRAMES * 2];
static int16_t s_dst[N64_AUDIO_OUT_FRAMES * 2];

static unsigned s_ai_clock = 48681812u;  /* NTSC */
static double   s_ai_rate  = 44100.0;

/* Smoothed drift correction applied to the resampling ratio, kept near 1.0.
 * See audio_convert_and_queue(). */
static double   s_rate_adj  = 1.0;
static double   s_phase;
static int16_t  s_prev_l;
static int16_t  s_prev_r;

/* ---- input -------------------------------------------------------------- */

static volatile LONG s_input_buttons;
static volatile LONG s_input_x;
static volatile LONG s_input_y;

/* ---- save state --------------------------------------------------------- */

static CRITICAL_SECTION s_state_lock;
static HANDLE           s_state_event;
static volatile LONG    s_state_param;
static volatile LONG    s_state_value;

/* ---- logging ------------------------------------------------------------ */

static void glue_log(int level, const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    (void)level;
    fprintf(stderr, "[n64] %s\n", buf);
    fflush(stderr);
}

/* angrylion's msg.h (implemented by libretro's interface.cpp, which we do not
 * compile).  Provide no-op stubs so n64video.c links. */
void msg_error(const char* err, ...)
{
    va_list ap;
    va_start(ap, err);
    vfprintf(stderr, err, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

void msg_warning(const char* err, ...)
{
    va_list ap;
    va_start(ap, err);
    vfprintf(stderr, err, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}

void msg_debug(const char* err, ...)
{
    (void)err;
}

/* The classic dynarec interface (declared in device/r4300/recomp.h) is
 * referenced from cp0.c / interrupt.c / r4300_core.c unconditionally (the
 * calls sit behind a runtime emumode == DYNAREC check), so interpreter-only
 * builds need linking stubs.  When the dynarec is compiled in
 * (N64_HAS_DYNAREC, set by CMake when it is) the real implementations come
 * from device/r4300/x86_64/dynarec.c and recomp.c.
 *
 * Defining the stubs in that case would be worse than a link error:
 * nintendall links with --allow-multiple-definition (needed because zlib's
 * crc32 is shared with another core) and n64_core comes first on the link
 * line, so the empty stubs would silently override the real functions.
 * dyna_jump/dyna_stop are how generated code learns where to resume; without
 * them the JIT jumps to a stale address and the process dies - which is
 * exactly the access violation that kept this core on the interpreter. */
#ifndef N64_HAS_DYNAREC
void dyna_jump(void) {}
void dyna_stop(struct r4300_core* r4300) { (void)r4300; }
void dynarec_jump_to(struct r4300_core* r4300, uint32_t address)
{
    (void)r4300;
    (void)address;
}
#endif

/* ------------------------------------------------------------------------- */
/* mupen64plus core callbacks                                                */
/* ------------------------------------------------------------------------- */

static void CALL debug_callback(void* context, int level, const char* message)
{
    (void)context;
    glue_log(level, "%s", message);
}

static void CALL state_callback(void* context, m64p_core_param param, int value)
{
    (void)context;
    EnterCriticalSection(&s_state_lock);
    s_state_param = (LONG)param;
    s_state_value = (LONG)value;
    LeaveCriticalSection(&s_state_lock);
    SetEvent(s_state_event);
}

/* ------------------------------------------------------------------------- */
/* angrylion glue                                                            */
/* ------------------------------------------------------------------------- */

void plugin_init(void) {}

void plugin_close(void) {}

void plugin_sync_dp(void)
{
    if (s_gfx_info.MI_INTR_REG != NULL)
        *s_gfx_info.MI_INTR_REG |= N64_DP_INTERRUPT;
    if (s_gfx_info.CheckInterrupts != NULL)
        s_gfx_info.CheckInterrupts();
}

uint32_t** plugin_get_dp_registers(void)
{
    /* Works because GFX_INFO orders the DPC registers exactly like dp_register. */
    return (uint32_t**)&s_gfx_info.DPC_START_REG;
}

uint32_t** plugin_get_vi_registers(void)
{
    /* Works because GFX_INFO orders the VI registers exactly like vi_register. */
    return (uint32_t**)&s_gfx_info.VI_STATUS_REG;
}

uint8_t* plugin_get_rdram(void)
{
    return s_gfx_info.RDRAM;
}

uint32_t plugin_get_rdram_size(void)
{
    return 0x800000u;
}

uint8_t* plugin_get_dmem(void)
{
    return s_gfx_info.DMEM;
}

uint8_t* plugin_get_rom_header(void)
{
    return s_gfx_info.HEADER;
}

uint32_t plugin_get_rom_name(char* name, uint32_t name_size)
{
    /* The ROM database / header name is already parsed by mupen64plus, so
     * there is no need to walk the Shift_JIS header here. */
    size_t n;
    if (name == NULL || name_size == 0)
        return 0;
    n = strlen(ROM_PARAMS.headername);
    if (n >= name_size)
        n = name_size - 1;
    memcpy(name, ROM_PARAMS.headername, n);
    name[n] = '\0';
    return (uint32_t)n;
}

/* angrylion screen backend: there is no window, so every windowing entry point
 * is a no-op.  screen_write() is the one that matters: it captures the frame
 * the VI filter just produced. */
void screen_init(struct n64video_config* config) { (void)config; }
void screen_read(struct frame_buffer* fb, bool rgb) { (void)fb; (void)rgb; }
void screen_set_fullscreen(bool fullscreen) { (void)fullscreen; }
bool screen_get_fullscreen(void) { return false; }
void screen_toggle_fullscreen(void) {}
void screen_close(void) {}

void screen_swap(bool blank)
{
    /* Frames reach the frontend through screen_write(); nothing to swap. */
    (void)blank;
}

void screen_write(struct frame_buffer* fb, int32_t output_height)
{
    unsigned w, h, y;
    const uint32_t* src;
    uint32_t* dst;

    (void)output_height;   /* the frontend scales; natural size is enough */

    if (fb == NULL || fb->pixels == NULL)
        return;

    w = fb->width;
    h = fb->height;
    if (w > N64_MAX_WIDTH)  w = N64_MAX_WIDTH;
    if (h > N64_MAX_HEIGHT) h = N64_MAX_HEIGHT;
    if (w == 0 || h == 0)
        return;

    src = fb->pixels;
    dst = s_capture;

    /* fb->pitch is a stride in pixels (PRESCALE_WIDTH); the capture buffer is
     * tightly packed.  The VI filter produces 0x00RRGGBB, so an opaque alpha
     * is all that is missing. */
    for (y = 0; y < h; y++) {
        const uint32_t* srow = src + (size_t)y * fb->pitch;
        uint32_t* drow = dst + (size_t)y * w;
        unsigned x;
        for (x = 0; x < w; x++)
            drow[x] = 0xFF000000u | (srow[x] & 0x00FFFFFFu);
    }

    s_cap_w = w;
    s_cap_h = h;
}

/* ------------------------------------------------------------------------- */
/* Audio: native -> 48 kHz, into a ring buffer                               */
/* ------------------------------------------------------------------------- */

static void audio_ring_push_frame(int16_t l, int16_t r)
{
    if (s_ring_count >= N64_AUDIO_RING_FRAMES) {
        /* Full: drop the oldest frame so we stay close to real time. */
        s_ring_tail = (s_ring_tail + 1u) % N64_AUDIO_RING_FRAMES;
        s_ring_count--;
    }
    s_ring[s_ring_head * 2 + 0] = l;
    s_ring[s_ring_head * 2 + 1] = r;
    s_ring_head = (s_ring_head + 1u) % N64_AUDIO_RING_FRAMES;
    s_ring_count++;
}

static void audio_ring_reset(void)
{
    s_ring_head = 0;
    s_ring_tail = 0;
    s_ring_count = 0;
    s_phase = 0.0;
    s_prev_l = 0;
    s_prev_r = 0;
    s_rate_adj = 1.0;    /* start unbiased after a reset or ROM change */
}

/* Resample `frames` native frames (already in s_src) to 48 kHz and queue them.
 * Caller must not hold s_lock (this runs on the emulation thread). */
static void audio_convert_and_queue(unsigned frames)
{
    unsigned i;
    unsigned produced = 0;
    double frac = s_phase;
    int16_t pl = s_prev_l, pr = s_prev_r;
    double step;
    const int16_t* in = s_src;

    /* Drift correction.
     *
     * The AI's sample rate comes out of the emulated registers, and the N64's
     * audio and video clocks are not an exact multiple of 48 kHz.  Resampling
     * with a fixed ratio therefore loses or gains a few samples a second, and
     * the ring slowly drifts: run long enough and it either fills and starts
     * dropping the oldest audio (pitch jumps) or empties and leaves gaps.
     *
     * The correction is a nudge towards the middle of the ring.  The target has
     * to be small and realistic, though: this ring is drained once per emulated
     * frame, so it only ever holds a frame or two.  Aiming at a fraction of the
     * whole buffer leaves the correction pinned at its limit constantly, which
     * skews the ratio permanently instead of correcting anything.
     */
    {
        unsigned count;
        const unsigned target = N64_DRIFT_TARGET_FRAMES;
        double want = 1.0;

        EnterCriticalSection(&s_lock);
        count = s_ring_count;
        LeaveCriticalSection(&s_lock);

        /* A larger step emits fewer output frames per input frame, so the ring
         * fills; a smaller one drains it.  A ring that is too full therefore
         * wants a smaller step and a ring that is too empty wants a larger one. */
        if      (count > target * 2u)   want = 0.9970;
        else if (count > target)        want = 0.9990;
        else if (count < target / 4u)   want = 1.0030;
        else if (count < target / 2u)   want = 1.0010;

        /* Ease towards the target rather than stepping, so the correction is
         * not itself audible as a pitch wobble. */
        s_rate_adj += (want - s_rate_adj) * 0.05;
        if (s_rate_adj < 0.99)  s_rate_adj = 0.99;
        if (s_rate_adj > 1.01)  s_rate_adj = 1.01;
    }

    step = (s_ai_rate / (double)N64_AUDIO_RATE) * s_rate_adj;
    if (step <= 0.0)
        step = 1.0;

    for (i = 0; i < frames; i++) {
        int16_t cl = in[i * 2 + 0];
        int16_t cr = in[i * 2 + 1];
        while (frac < 1.0 && produced < N64_AUDIO_OUT_FRAMES) {
            s_dst[produced * 2 + 0] = (int16_t)(pl + (int32_t)((cl - pl) * frac));
            s_dst[produced * 2 + 1] = (int16_t)(pr + (int32_t)((cr - pr) * frac));
            produced++;
            frac += step;
        }
        frac -= 1.0;
        pl = cl;
        pr = cr;
    }

    s_phase = frac;
    s_prev_l = pl;
    s_prev_r = pr;

    if (produced == 0)
        return;

    EnterCriticalSection(&s_lock);
    for (i = 0; i < produced; i++)
        audio_ring_push_frame(s_dst[i * 2 + 0], s_dst[i * 2 + 1]);
    LeaveCriticalSection(&s_lock);
}

/* ------------------------------------------------------------------------- */
/* Plugin tables - video                                                     */
/* ------------------------------------------------------------------------- */

static m64p_error CALL gfx_PluginGetVersion(m64p_plugin_type* type, int* version,
                                            int* api, const char** name, int* caps)
{
    if (type)    *type = M64PLUGIN_GFX;
    if (version) *version = 0x020200;
    if (api)     *api = GFX_API_VERSION;
    if (name)    *name = "Nintendall angrylion software RDP";
    if (caps)    *caps = 0;
    return M64ERR_SUCCESS;
}

static int CALL gfx_InitiateGFX(GFX_INFO info)
{
    s_gfx_info = info;   /* by-value handoff from plugin.c */
    s_gfx_up = 1;
    return 1;
}

static void CALL gfx_ProcessDList(void)
{
    /* angrylion is an RDP-only renderer: it consumes a raw RDP command list,
     * not an F3DEX display list.  Display lists must be turned into RDP
     * commands by an LLE RSP (which also writes the DPC registers); this entry
     * point therefore has nothing to do, exactly like the reference
     * mupen64plus-video-angrylion plugin. */
}

static void CALL gfx_ProcessRDPList(void)
{
    /* The RDP command list is ready (DPC_START/DPC_END were written by the
     * LLE RSP): render it. */
    n64video_process_list();
}

static void CALL gfx_UpdateScreen(void)
{
    unsigned w, h;
    LONG reset;

    /* Apply a pending reset before touching this frame's output.  Running it
     * here keeps every g_dev mutation on the emulation thread. */
    reset = InterlockedExchange(&s_pending_reset, 0);
    if (reset != 0)
        CoreDoCommand(M64CMD_RESET, (reset == 2) ? 1 : 0, NULL);

    n64video_update_screen();   /* -> screen_write() -> s_capture */

    EnterCriticalSection(&s_lock);
    if (s_exit) {
        LeaveCriticalSection(&s_lock);
        return;
    }

    w = s_cap_w;
    h = s_cap_h;
    if (w == 0 || h == 0) {
        /* screen_write() hasn't been called yet (the VI hasn't been
         * configured yet), so synthesize a test pattern so the frontend
         * always receives a frame. */
        w = N64_MAX_WIDTH;
        h = N64_MAX_HEIGHT;
        for (unsigned y = 0; y < h; y++) {
            for (unsigned x = 0; x < w; x++) {
                uint8_t r = (uint8_t)(x * 255 / w);
                uint8_t g = (uint8_t)(y * 255 / h);
                uint8_t b = 128;
                s_capture[y * w + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        }
        s_cap_w = w;
        s_cap_h = h;
    }

    unsigned y;
    for (y = 0; y < h; y++)
        memcpy(s_deliver + (size_t)y * w, s_capture + (size_t)y * w,
               (size_t)w * sizeof(uint32_t));
    s_out_w = w;
    s_out_h = h;
    s_presented++;

    /* Signal a frame is ready.  We deliberately do NOT block here: the
     * frontend waits for the frame on its own thread, and keeping the core
     * running (rather than parked in this callback) is what lets a save/load
     * state request be serviced even while the frontend is paused. */
    WakeAllConditionVariable(&s_cond_frame);

    LeaveCriticalSection(&s_lock);
}

static void CALL gfx_RomClosed(void)
{
    n64video_close();
}

static int CALL gfx_RomOpen(void)
{
    /* main_run() calls this right after init_device(), which is the point at
     * which g_dev (and therefore our GFX_INFO pointers) is fully populated. */
    n64video_init(&s_vi_config);
    return 1;
}
static void CALL gfx_ShowCFB(void) {}
static void CALL gfx_ViStatusChanged(void) {}
static void CALL gfx_ViWidthChanged(void) {}
static void CALL gfx_ChangeWindow(void) {}
static void CALL gfx_MoveScreen(int x, int y) { (void)x; (void)y; }
static void CALL gfx_SetRenderingCallback(void (*cb)(int)) { (void)cb; }
static void CALL gfx_ResizeVideoOutput(int w, int h) { (void)w; (void)h; }
static void CALL gfx_FBRead(unsigned int addr) { (void)addr; }
static void CALL gfx_FBWrite(unsigned int addr, unsigned int size) { (void)addr; (void)size; }
static void CALL gfx_FBGetFrameBufferInfo(void* p)
{
    /* Report "no framebuffer" so fb.c leaves RDRAM alone.  angrylion already
     * renders into RDRAM, so there is nothing to protect. */
    if (p) memset(p, 0, sizeof(unsigned int) * 4 * 6);
}

static void CALL gfx_ReadScreen(void* dest, int* width, int* height, int front)
{
    (void)front;
    if (width)  *width  = (int)s_out_w;
    if (height) *height = (int)s_out_h;
    if (dest != NULL && s_out_w > 0 && s_out_h > 0) {
        EnterCriticalSection(&s_lock);
        memcpy(dest, s_deliver, (size_t)s_out_w * s_out_h * sizeof(uint32_t));
        LeaveCriticalSection(&s_lock);
    }
}

/* ------------------------------------------------------------------------- */
/* Plugin tables - audio                                                     */
/* ------------------------------------------------------------------------- */

static m64p_error CALL audio_PluginGetVersion(m64p_plugin_type* type, int* version,
                                              int* api, const char** name, int* caps)
{
    if (type)    *type = M64PLUGIN_AUDIO;
    if (version) *version = 0x020000;
    if (api)     *api = AUDIO_API_VERSION;
    if (name)    *name = "Nintendall audio bridge";
    if (caps)    *caps = 0;
    return M64ERR_SUCCESS;
}

static int CALL audio_InitiateAudio(AUDIO_INFO info)
{
    s_audio_info = info;   /* by-value handoff from plugin.c */
    s_audio_up = 1;
    return 1;
}

static void CALL audio_AiDacrateChanged(int system_type)
{
    uint32_t dacrate = 0;

    s_ai_clock = vi_clock_from_tv_standard((m64p_system_type)system_type);
    if (s_audio_info.AI_DACRATE_REG != NULL)
        dacrate = *s_audio_info.AI_DACRATE_REG;

    /* plugins_compat encodes the desired rate back into the DACRATE register
     * before calling this, so it can be recovered exactly. */
    s_ai_rate = (dacrate == 0) ? 44100.0
                               : (double)s_ai_clock / (1.0 + (double)dacrate);
}

static void CALL audio_AiLenChanged(void)
{
    uint32_t addr, len, bytes, frames, off;

    if (!s_audio_up || s_audio_info.RDRAM == NULL ||
        s_audio_info.AI_DRAM_ADDR_REG == NULL || s_audio_info.AI_LEN_REG == NULL)
        return;

    addr = *s_audio_info.AI_DRAM_ADDR_REG;
    len  = *s_audio_info.AI_LEN_REG;

    /* RDRAM is 8 MiB; AI requests are 16-bit aligned and at most a few KiB. */
    addr &= 0x7FFFFFu;
    bytes = len & 0x3FFFFu;
    if (bytes == 0)
        return;

    if (addr + bytes > 0x800000u) {
        if (addr >= 0x800000u)
            return;
        bytes = 0x800000u - addr;
    }

    frames = bytes / 4u;   /* 16-bit stereo */
    if (frames == 0)
        return;

    for (off = 0; off < frames; off += N64_AUDIO_SRC_FRAMES) {
        unsigned n = frames - off;
        if (n > N64_AUDIO_SRC_FRAMES)
            n = N64_AUDIO_SRC_FRAMES;
        memcpy(s_src, s_audio_info.RDRAM + addr + (size_t)off * 4u, (size_t)n * 4u);
        audio_convert_and_queue(n);
    }
}

static void CALL audio_ProcessAList(void) {}
static void CALL audio_RomClosed(void) {}
static int  CALL audio_RomOpen(void) { return 1; }
static void CALL audio_SetSpeedFactor(int percent) { (void)percent; }
static void CALL audio_VolumeUp(void) {}
static void CALL audio_VolumeDown(void) {}
static int  CALL audio_VolumeGetLevel(void) { return 100; }
static void CALL audio_VolumeSetLevel(int level) { (void)level; }
static void CALL audio_VolumeMute(void) {}
static const char* CALL audio_VolumeGetString(void) { return "100%"; }

/* ------------------------------------------------------------------------- */
/* Plugin tables - input                                                     */
/* ------------------------------------------------------------------------- */

static m64p_error CALL input_PluginGetVersion(m64p_plugin_type* type, int* version,
                                              int* api, const char** name, int* caps)
{
    if (type)    *type = M64PLUGIN_INPUT;
    if (version) *version = 0x020101;
    if (api)     *api = INPUT_API_VERSION;
    if (name)    *name = "Nintendall input bridge";
    if (caps)    *caps = 0;
    return M64ERR_SUCCESS;
}

static void CALL input_InitiateControllers(CONTROL_INFO info)
{
    int i;
    if (info.Controls == NULL)
        return;

    for (i = 0; i < 4; i++) {
        info.Controls[i].Present = (i == 0) ? 1 : 0;
        info.Controls[i].RawData = 0;
        info.Controls[i].Plugin  = (i == 0) ? PLUGIN_NONE : PLUGIN_NONE;
        info.Controls[i].Type    = CONT_TYPE_STANDARD;
    }
    s_input_up = 1;
}

static void CALL input_GetKeys(int control, BUTTONS* keys)
{
    if (keys == NULL)
        return;

    keys->Value = 0;
    if (control != 0)
        return;

    keys->Value = (unsigned int)s_input_buttons;
    keys->X_AXIS = (signed char)s_input_x;
    keys->Y_AXIS = (signed char)s_input_y;
}

static void CALL input_ControllerCommand(int control, unsigned char* command)
{
    (void)control; (void)command;
}
static void CALL input_ReadController(int control, unsigned char* command)
{
    (void)control; (void)command;
}
static void CALL input_RomClosed(void) {}
static int  CALL input_RomOpen(void) { return 1; }
static void CALL input_SDL_KeyDown(int keymod, int keysym) { (void)keymod; (void)keysym; }
static void CALL input_SDL_KeyUp(int keymod, int keysym) { (void)keymod; (void)keysym; }
static void CALL input_RenderCallback(void) {}
static void CALL input_SendVRUWord(uint16_t length, uint16_t* word, uint8_t lang)
{
    (void)length; (void)word; (void)lang;
}
static void CALL input_SetMicState(int state) { (void)state; }
static void CALL input_ReadVRUResults(uint16_t* error_flags, uint16_t* num_results,
                                      uint16_t* mic_level, uint16_t* voice_level,
                                      uint16_t* voice_length, uint16_t* matches)
{
    (void)error_flags; (void)num_results; (void)mic_level;
    (void)voice_level; (void)voice_length; (void)matches;
}
static void CALL input_ClearVRUWords(uint8_t length) { (void)length; }
static void CALL input_SetVRUWordMask(uint8_t length, uint8_t* mask) { (void)length; (void)mask; }

/* ------------------------------------------------------------------------- */
/* Plugin tables - RSP                                                       */
/* ------------------------------------------------------------------------- */

static m64p_error CALL rsp_PluginGetVersion(m64p_plugin_type* type, int* version,
                                            int* api, const char** name, int* caps)
{
    if (type)    *type = M64PLUGIN_RSP;
    if (version) *version = 0x020600;
    if (api)     *api = RSP_API_VERSION;
    if (name)    *name = "Nintendall mupen64plus-rsp-cxd4 bridge";
    if (caps)    *caps = 0;
    return M64ERR_SUCCESS;
}

static unsigned int CALL rsp_DoRspCycles(unsigned int cycles)
{
    n64_rsp_execute();
    return cycles;
}

static void CALL rsp_InitiateRSP(RSP_INFO info, unsigned int* cycle_count)
{
    (void)cycle_count;
    n64_rsp_init(&info);
    s_rsp_up = 1;
}

static void CALL rsp_RomClosed(void)
{
    n64_rsp_rom_closed();
}

/* ------------------------------------------------------------------------- */
/* Plugin attach                                                             */
/* ------------------------------------------------------------------------- */

static void build_plugin_tables(void)
{
    memset(&s_gfx_table, 0, sizeof(s_gfx_table));
    s_gfx_table.getVersion         = gfx_PluginGetVersion;
    s_gfx_table.changeWindow       = gfx_ChangeWindow;
    s_gfx_table.initiateGFX        = gfx_InitiateGFX;
    s_gfx_table.moveScreen         = gfx_MoveScreen;
    s_gfx_table.processDList       = gfx_ProcessDList;
    s_gfx_table.processRDPList     = gfx_ProcessRDPList;
    s_gfx_table.romClosed          = gfx_RomClosed;
    s_gfx_table.romOpen            = gfx_RomOpen;
    s_gfx_table.showCFB            = gfx_ShowCFB;
    s_gfx_table.updateScreen       = gfx_UpdateScreen;
    s_gfx_table.viStatusChanged    = gfx_ViStatusChanged;
    s_gfx_table.viWidthChanged     = gfx_ViWidthChanged;
    s_gfx_table.readScreen         = gfx_ReadScreen;
    s_gfx_table.setRenderingCallback = gfx_SetRenderingCallback;
    s_gfx_table.resizeVideoOutput  = gfx_ResizeVideoOutput;
    s_gfx_table.fBRead             = gfx_FBRead;
    s_gfx_table.fBWrite            = gfx_FBWrite;
    s_gfx_table.fBGetFrameBufferInfo = gfx_FBGetFrameBufferInfo;

    memset(&s_audio_table, 0, sizeof(s_audio_table));
    s_audio_table.getVersion       = audio_PluginGetVersion;
    s_audio_table.aiDacrateChanged = audio_AiDacrateChanged;
    s_audio_table.aiLenChanged     = audio_AiLenChanged;
    s_audio_table.initiateAudio    = audio_InitiateAudio;
    s_audio_table.processAList     = audio_ProcessAList;
    s_audio_table.romClosed        = audio_RomClosed;
    s_audio_table.romOpen          = audio_RomOpen;
    s_audio_table.setSpeedFactor   = audio_SetSpeedFactor;
    s_audio_table.volumeUp         = audio_VolumeUp;
    s_audio_table.volumeDown       = audio_VolumeDown;
    s_audio_table.volumeGetLevel   = audio_VolumeGetLevel;
    s_audio_table.volumeSetLevel   = audio_VolumeSetLevel;
    s_audio_table.volumeMute       = audio_VolumeMute;
    s_audio_table.volumeGetString  = audio_VolumeGetString;

    memset(&s_input_table, 0, sizeof(s_input_table));
    s_input_table.getVersion            = input_PluginGetVersion;
    s_input_table.controllerCommand     = input_ControllerCommand;
    s_input_table.getKeys               = input_GetKeys;
    s_input_table.initiateControllers   = input_InitiateControllers;
    s_input_table.readController        = input_ReadController;
    s_input_table.romClosed             = input_RomClosed;
    s_input_table.romOpen               = input_RomOpen;
    s_input_table.keyDown               = input_SDL_KeyDown;
    s_input_table.keyUp                 = input_SDL_KeyUp;
    s_input_table.renderCallback        = input_RenderCallback;
    s_input_table.sendVRUWord           = input_SendVRUWord;
    s_input_table.setMicState           = input_SetMicState;
    s_input_table.readVRUResults        = input_ReadVRUResults;
    s_input_table.clearVRUWords         = input_ClearVRUWords;
    s_input_table.setVRUWordMask        = input_SetVRUWordMask;

    memset(&s_rsp_table, 0, sizeof(s_rsp_table));
    s_rsp_table.getVersion   = rsp_PluginGetVersion;
    s_rsp_table.doRspCycles  = rsp_DoRspCycles;
    s_rsp_table.initiateRSP  = rsp_InitiateRSP;
    s_rsp_table.romClosed    = rsp_RomClosed;
}

/* Assign our tables to the core's plugin globals and let plugin.c fill in its
 * info structs (and call our initiate*() functions).  Must run after
 * CoreStartup (which resets the tables to the dummy plugins) and after
 * M64CMD_ROM_OPEN (plugin_start_gfx needs g_rom_size). */
static int attach_plugins(void)
{
    build_plugin_tables();
    gfx   = s_gfx_table;
    audio = s_audio_table;
    input = s_input_table;
    rsp   = s_rsp_table;

    if (plugin_start(M64PLUGIN_GFX) != M64ERR_SUCCESS) {
        glue_log(M64MSG_ERROR, "failed to start the video plugin");
        return -1;
    }
    if (plugin_start(M64PLUGIN_AUDIO) != M64ERR_SUCCESS) {
        glue_log(M64MSG_ERROR, "failed to start the audio plugin");
        return -1;
    }
    if (plugin_start(M64PLUGIN_INPUT) != M64ERR_SUCCESS) {
        glue_log(M64MSG_ERROR, "failed to start the input plugin");
        return -1;
    }
    if (plugin_start(M64PLUGIN_RSP) != M64ERR_SUCCESS) {
        glue_log(M64MSG_ERROR, "failed to start the RSP plugin");
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------------- */
/* Emulation thread                                                          */
/* ------------------------------------------------------------------------- */

static DWORD WINAPI emu_thread_proc(LPVOID param)
{
    (void)param;
    /* Blocks until the game stops (M64CMD_STOP / main_stop). */
    CoreDoCommand(M64CMD_EXECUTE, 0, NULL);

    /* Wake the frontend in case it is blocked waiting for a frame. */
    EnterCriticalSection(&s_lock);
    s_exit = 1;
    WakeAllConditionVariable(&s_cond_frame);
    WakeAllConditionVariable(&s_cond_done);
    LeaveCriticalSection(&s_lock);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

void n64_glue_set_callbacks(void* user, n64_video_cb video, n64_audio_cb audio)
{
    s_user = user;
    s_video_cb = video;
    s_audio_cb = audio;
}

/* Older builds let mupen64plus keep its battery saves in the global per-user
 * data directory (%APPDATA%\Mupen64Plus\save).  Now that they live next to the
 * rest of our save data, pull across anything that was left behind so existing
 * progress is not silently orphaned.  An existing file always wins, so this
 * only ever acts once per save. */
static void migrate_legacy_saves(void)
{
    const char* old_root = osal_get_user_datapath();
    char old_path[1024];
    char new_dir[1024];
    char new_path[1024];
    WIN32_FIND_DATAA find_data;
    HANDLE find;

    if (old_root == NULL || old_root[0] == '\0')
        return;

    if (snprintf(old_path, sizeof(old_path), "%ssave\\*", old_root) >= (int)sizeof(old_path))
        return;

    find = FindFirstFileA(old_path, &find_data);
    if (find == INVALID_HANDLE_VALUE)
        return;

    snprintf(new_dir, sizeof(new_dir), "%s\\save", s_data_path);
    CreateDirectoryA(new_dir, NULL);

    do {
        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        snprintf(old_path, sizeof(old_path), "%ssave\\%s", old_root, find_data.cFileName);
        snprintf(new_path, sizeof(new_path), "%s\\%s", new_dir, find_data.cFileName);
        CopyFileA(old_path, new_path, TRUE);
    } while (FindNextFileA(find, &find_data));

    FindClose(find);
}

int n64_glue_startup(const char* data_path)
{
    if (s_core_up)
        return 0;

    InitializeCriticalSection(&s_lock);
    InitializeCriticalSection(&s_state_lock);
    s_state_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (s_state_event == NULL)
        return -1;

    if (data_path != NULL && data_path[0] != '\0') {
        snprintf(s_data_path, sizeof(s_data_path), "%s", data_path);
    } else {
        GetCurrentDirectoryA(sizeof(s_data_path), s_data_path);
    }

    /* Config + battery saves live next to the frontend's other save data. */
    CreateDirectoryA(s_data_path, NULL);

    snprintf(s_state_path, sizeof(s_state_path), "%s\\n64_slot.state", s_data_path);

    /* angrylion configuration: solid filtered NTSC/PAL output, whole prescale
     * buffer (no overscan cropping), multi-threaded rasteriser. */
    n64video_config_defaults(&s_vi_config);
    s_vi_config.vi.mode = VI_MODE_NORMAL;
    s_vi_config.vi.interp = VI_INTERP_LINEAR;
    s_vi_config.vi.widescreen = false;
    s_vi_config.vi.hide_overscan = false;

#ifdef N64_PARALLEL_VIDEO
    /* The software rasteriser is the expensive half of a frame, and angrylion
     * splits it across cores.  Leave one core for the emulation thread so audio
     * and input keep their deadlines. */
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        unsigned workers = si.dwNumberOfProcessors;
        if (workers > 1) {
            s_vi_config.parallel  = true;
            s_vi_config.num_workers = workers - 1;
        } else {
            s_vi_config.parallel  = false;
            s_vi_config.num_workers = 1;
        }
    }
#else
    s_vi_config.parallel = false;   /* single-threaded rasteriser */
    s_vi_config.num_workers = 1;
#endif

    /* SpeedLimiter must be off: the frontend paces the emulation from its own
     * thread, and the core blocks in gfx.updateScreen() every frame. */
    if (CoreStartup(FRONTEND_API_VERSION, s_data_path, s_data_path,
                    NULL, debug_callback, NULL, state_callback) != M64ERR_SUCCESS) {
        glue_log(M64MSG_ERROR, "CoreStartup failed");
        return -1;
    }

    /* Battery saves (EEPROM/SRAM/FlashRAM) and controller paks are managed by
     * mupen64plus itself: it opens them when the ROM starts and writes straight
     * through on every in-game save.  Point its per-user data directory at our
     * own save folder so those files land in <save_dir>\save\ instead of the
     * global per-user directory, and therefore travel with the app. */
    ConfigOverrideUserPaths(s_data_path, NULL);
    migrate_legacy_saves();

    /* OnScreenDisplay defaults to on; without a window that only costs time
     * (and the OSD module is not compiled in).
     *
     * R4300Emulator picks the R4300 engine: 2 is the classic x86-64 dynarec,
     * 1 the cached interpreter.  The dynarec is what makes N64 games playable
     * on modest hardware - the interpreter burns roughly an order of magnitude
     * more CPU per frame - so it is used whenever it is compiled in
     * (N64_HAS_DYNAREC; default on x86-64 when nasm is available), with the
     * interpreter kept as the fallback for builds without nasm.
     *
     * It used to die with an access violation a couple of seconds into a game.
     * That was integration, not the dynarec itself: this file's interpreter-only
     * stubs above silently overrode the real dyna_jump/dyna_stop (see the note
     * there), and dyna_start.asm was assembled without -d WIN64 so it called
     * through %rdi instead of %rcx.  Both are fixed; the stubs are now compiled
     * only when the dynarec is absent. */
    {
        m64p_handle core_config = NULL;
        if (ConfigOpenSection("Core", &core_config) == M64ERR_SUCCESS && core_config)
        {
            ConfigSetParameter(core_config, "OnScreenDisplay", M64TYPE_BOOL, &(int){0});
#ifdef N64_HAS_DYNAREC
            int emumode = 2;   /* x86-64 dynarec */
#else
            int emumode = 1;   /* cached interpreter (no dynarec built) */
#endif
            ConfigSetParameter(core_config, "R4300Emulator", M64TYPE_INT, &emumode);
        }
    }

    s_core_up = 1;
    return 0;
}

int n64_glue_load_rom(const uint8_t* rom, size_t rom_size)
{
    if (!s_core_up || rom == NULL || rom_size < 4096)
        return -1;

    /* A previous ROM may still be open. */
    if (s_rom_open) {
        CoreDoCommand(M64CMD_ROM_CLOSE, 0, NULL);
        s_rom_open = 0;
        s_gfx_up = s_audio_up = s_input_up = s_rsp_up = 0;
    }

    audio_ring_reset();
    s_cap_w = s_cap_h = 0;
    s_out_w = s_out_h = 0;

    if (CoreDoCommand(M64CMD_ROM_OPEN, (int)rom_size, (void*)rom) != M64ERR_SUCCESS) {
        glue_log(M64MSG_ERROR, "failed to open the ROM");
        return -1;
    }
    s_rom_open = 1;

    if (attach_plugins() != 0)
        return -1;

    return 0;
}

int n64_glue_start(void)
{
    if (!s_core_up || !s_rom_open)
        return -1;
    if (s_emu_thread != NULL)
        return 0;

    s_exit = 0;
    s_presented = 0;
    s_consumed = 0;

    s_emu_thread = CreateThread(NULL, 8u * 1024u * 1024u, emu_thread_proc, NULL, 0, NULL);
    if (s_emu_thread == NULL) {
        glue_log(M64MSG_ERROR, "failed to create the emulation thread");
        return -1;
    }
    return 0;
}

int n64_glue_frame(void)
{
    unsigned w, h, i;

    /* Wait for the core to present a fresh frame.  Bounded so a stalled core
     * can never hang the caller; the core produces frames at ~real time, so
     * this normally returns within a frame interval. */
    EnterCriticalSection(&s_lock);
    while (!s_exit && s_presented == s_consumed) {
        if (!SleepConditionVariableCS(&s_cond_frame, &s_lock, 1000))
            break;
    }

    /* Deliver on this thread.  We hold s_lock so the core cannot overwrite
     * s_deliver mid-callback; the handoff is cheap and keeps the frame whole.
     * Only advance s_consumed when a frame was actually available, so a timed-
     * out wait cannot run the counter ahead of s_presented. */
    w = s_out_w;
    h = s_out_h;
    if (s_presented != s_consumed) {
        if (s_video_cb && w > 0 && h > 0)
            s_video_cb(s_user, s_deliver, w, h, w * sizeof(uint32_t));
        s_consumed++;
    }
    LeaveCriticalSection(&s_lock);

    /* Hold the core to real time.
     *
     * n64_glue_frame() blocks until the core presents the next frame, so the
     * frontend marks this core self-paced and skips its audio-clock pacing -
     * adding that sleep on top of the blocking wait would double the frame
     * period.  The upshot is that the core free-runs as fast as the host can
     * emulate it, and the AI then produces samples faster than the 48 kHz
     * device consumes them.  Measured on Super Mario 64 the surplus was about
     * 1.5%, which slowly fills the frontend ring until it starts discarding
     * audio: sound drifting out of step with picture over a couple of minutes.
     *
     * So the wait is done here instead, against the NTSC field rate. */
    {
        static LARGE_INTEGER s_next_vi = { 0 };
        LARGE_INTEGER freq, now;

        if (s_next_vi.QuadPart == 0) {
            QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&now);
            s_next_vi = now;
        } else {
            QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&now);

            /* 1 / 60.0988 s, in performance-counter ticks. */
            const LONGLONG period = (LONGLONG)((double)freq.QuadPart / 60.0988);

            if (s_next_vi.QuadPart <= now.QuadPart) {
                /* Fell behind (a stall, or the host is too slow).  Resume from
                 * now rather than trying to make up every missed frame. */
                s_next_vi = now;
            }
            s_next_vi.QuadPart += period;

            while (now.QuadPart < s_next_vi.QuadPart) {
                LONGLONG left_us = (s_next_vi.QuadPart - now.QuadPart) * 1000000 / freq.QuadPart;
                /* Sleep is only good to about a millisecond even with
                 * timeBeginPeriod(1); spin the last millisecond so the frame
                 * period does not jitter by a whole millisecond every frame. */
                if (left_us > 1500) {
                    Sleep((DWORD)((left_us - 1000) / 1000));
                }
                QueryPerformanceCounter(&now);
            }
        }
    }

    /* Audio is drained here and only here, so the callback always runs on the
     * frontend thread.  We drain the ring in N64_AUDIO_CHUNK-frame pieces.  If
     * the AI has not produced anything yet (boot, a silent scene, right after a
     * reset/state load) we simply deliver nothing: the frontend treats that as
     * silence.  There used to be a synthetic test tone here, but it was a loud
     * square wave -- whenever the ring happened to be empty it produced an
     * audible beep. */
    if (s_audio_cb) {
        for (;;) {
            unsigned n;
            int16_t chunk[N64_AUDIO_CHUNK * 2];

            EnterCriticalSection(&s_lock);
            n = s_ring_count;
            if (n > N64_AUDIO_CHUNK)
                n = N64_AUDIO_CHUNK;
            for (i = 0; i < n; i++) {
                chunk[i * 2 + 0] = s_ring[s_ring_tail * 2 + 0];
                chunk[i * 2 + 1] = s_ring[s_ring_tail * 2 + 1];
                s_ring_tail = (s_ring_tail + 1u) % N64_AUDIO_RING_FRAMES;
            }
            s_ring_count -= n;
            LeaveCriticalSection(&s_lock);

            if (n == 0)
                break;      /* AI produced nothing this frame: no audio */

            s_audio_cb(s_user, chunk, n);
            if (n < N64_AUDIO_CHUNK)
                break;
        }
    }

    return 1;
}
void n64_glue_set_input(uint32_t buttons, int8_t stick_x, int8_t stick_y)
{
    InterlockedExchange(&s_input_buttons, (LONG)buttons);
    InterlockedExchange(&s_input_x, (LONG)stick_x);
    InterlockedExchange(&s_input_y, (LONG)stick_y);
}

void n64_glue_reset(void)
{
    if (s_core_up && s_rom_open)
        InterlockedExchange(&s_pending_reset, 1);
}

void n64_glue_power_cycle(void)
{
    if (s_core_up && s_rom_open)
        InterlockedExchange(&s_pending_reset, 2);
}

void n64_glue_get_rom_name(char* out, size_t cap)
{
    if (out == NULL || cap == 0)
        return;
    snprintf(out, cap, "%s", ROM_PARAMS.headername);
}

int n64_glue_get_region(void)
{
    return (int)ROM_PARAMS.systemtype;
}

uint32_t n64_glue_get_crc32(void)
{
    return ROM_HEADER.CRC1;
}

unsigned n64_glue_get_rom_size(void)
{
    return (unsigned)g_rom_size;
}

int n64_glue_get_has_battery(void)
{
    /* EEPROM_4K is 0; only NONE (5) means "no battery-backed storage". */
    return ROM_SETTINGS.savetype != N64_SAVETYPE_NONE ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/* Save states                                                               */
/* ------------------------------------------------------------------------- */

static int wait_for_state_param(int want_param, DWORD timeout_ms, int* out_value)
{
    DWORD deadline = GetTickCount() + timeout_ms;

    for (;;) {
        DWORD now = GetTickCount();
        DWORD remaining;
        LONG param;

        if ((int)(deadline - now) <= 0)
            return 0;
        remaining = deadline - now;

        if (WaitForSingleObject(s_state_event, remaining) != WAIT_OBJECT_0)
            return 0;

        EnterCriticalSection(&s_state_lock);
        param = s_state_param;
        if (out_value)
            *out_value = (int)s_state_value;
        LeaveCriticalSection(&s_state_lock);

        if ((int)param == want_param)
            return 1;
    }
}

size_t n64_glue_save_state(uint8_t* buffer, size_t buffer_size)
{
    FILE* f;
    long size;
    static uint8_t* cache;
    static size_t cache_size;

    if (!s_core_up || !s_rom_open || !s_emu_thread)
        return 0;

    if (buffer != NULL && cache != NULL && buffer_size >= cache_size) {
        memcpy(buffer, cache, cache_size);
        return cache_size;
    }

    /* Queue the save and let the core snapshot itself at the next safe point
     * (a VI interrupt), from the emulation thread. */
    if (CoreDoCommand(M64CMD_STATE_SAVE, 1, s_state_path) != M64ERR_SUCCESS)
        return 0;

    if (!wait_for_state_param(M64CORE_STATE_SAVECOMPLETE, 10000, NULL)) {
        glue_log(M64MSG_ERROR, "timed out waiting for the state save");
        return 0;
    }

    f = fopen(s_state_path, "rb");
    if (f == NULL)
        return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    size = ftell(f);
    if (size <= 0) { fclose(f); return 0; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }

    free(cache);
    cache = (uint8_t*)malloc((size_t)size);
    cache_size = (size_t)size;
    if (cache == NULL || fread(cache, 1, cache_size, f) != cache_size) {
        fclose(f);
        free(cache);
        cache = NULL;
        cache_size = 0;
        return 0;
    }
    fclose(f);

    if (buffer == NULL)
        return cache_size;
    if (buffer_size < cache_size)
        return 0;
    memcpy(buffer, cache, cache_size);
    return cache_size;
}

int n64_glue_load_state(const uint8_t* buffer, size_t size)
{
    FILE* f;

    if (!s_core_up || !s_rom_open || !s_emu_thread || buffer == NULL || size == 0)
        return 0;

    f = fopen(s_state_path, "wb");
    if (f == NULL)
        return 0;
    if (fwrite(buffer, 1, size, f) != size) {
        fclose(f);
        return 0;
    }
    fclose(f);

    if (CoreDoCommand(M64CMD_STATE_LOAD, 0, s_state_path) != M64ERR_SUCCESS)
        return 0;

    if (!wait_for_state_param(M64CORE_STATE_LOADCOMPLETE, 10000, NULL)) {
        glue_log(M64MSG_ERROR, "timed out waiting for the state load");
        return 0;
    }

    /* The restored VI registers may differ; make sure the frontend is told. */
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Shutdown                                                                  */
/* ------------------------------------------------------------------------- */

void n64_glue_shutdown(void)
{
    if (!s_core_up)
        return;

    if (s_emu_thread != NULL) {
        if (s_rom_open)
            CoreDoCommand(M64CMD_STOP, 0, NULL);

        EnterCriticalSection(&s_lock);
        s_exit = 1;
        WakeAllConditionVariable(&s_cond_frame);
        WakeAllConditionVariable(&s_cond_done);
        LeaveCriticalSection(&s_lock);

        WaitForSingleObject(s_emu_thread, 10000);
        CloseHandle(s_emu_thread);
        s_emu_thread = NULL;
    }

    if (s_rom_open) {
        CoreDoCommand(M64CMD_ROM_CLOSE, 0, NULL);
        s_rom_open = 0;
    }

    CoreShutdown();
    s_core_up = 0;

    if (s_state_event) {
        CloseHandle(s_state_event);
        s_state_event = NULL;
    }
    DeleteCriticalSection(&s_state_lock);
    DeleteCriticalSection(&s_lock);
}
