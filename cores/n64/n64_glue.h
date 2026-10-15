/**
 * Nintendall - mupen64plus glue layer
 *
 * This header is the bridge between the Nintendall N64 adapter
 * (n64_core.cpp) and the vendored mupen64plus-core.
 *
 * The glue owns everything that is specific to standing mupen64plus up without
 * its own frontend:
 *
 *   - CoreStartup / CoreShutdown
 *   - ROM open/close
 *   - the video (angrylion, software RDP), audio and input plugin tables
 *   - the RSP entry layer (mupen64plus-rsp-cxd4 LLE interpreter, see n64_rsp.c)
 *   - the emulation thread that runs M64CMD_EXECUTE
 *   - the frame boundary handshake between that thread and the frontend
 *   - save states
 *
 * The emulation thread is started by n64_glue_start() and runs until
 * n64_glue_shutdown().  It publishes each finished frame from
 * gfx.updateScreen() without blocking (so save/load-state still works while the
 * frontend is paused); n64_glue_frame() waits for the next fresh frame, so a
 * call to it always returns a finished frame.
 */

#ifndef NINTENDALL_N64_GLUE_H
#define NINTENDALL_N64_GLUE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Video frame callback.  Pixels are 0xAARRGGBB, top-down, pitch in bytes. */
typedef void (*n64_video_cb)(void* user, const void* pixels,
                             unsigned width, unsigned height, unsigned pitch);

/* Audio callback.  Interleaved stereo signed 16-bit frames at 48 kHz. */
typedef void (*n64_audio_cb)(void* user, const int16_t* samples, unsigned frames);

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

/* Register the callbacks used while the core runs.  Must be called before
 * n64_glue_start(). */
void n64_glue_set_callbacks(void* user, n64_video_cb video, n64_audio_cb audio);

/* Bring up the mupen64plus core and prepare the working directories.
 * data_path is the directory used for the core config, the ROM database and
 * battery saves.  Returns 0 on success. */
int n64_glue_startup(const char* data_path);

/* Load a ROM image (must be called after n64_glue_startup).  The buffer may be
 * released once this returns.  Returns 0 on success. */
int n64_glue_load_rom(const uint8_t* rom, size_t rom_size);

/* Reset the console (soft reset) / power cycle it. */
void n64_glue_reset(void);
void n64_glue_power_cycle(void);

/* Spawn the emulation thread; it runs M64CMD_EXECUTE until shutdown.
 * Returns 0 on success. */
int n64_glue_start(void);

/* Deliver the next finished frame to the video/audio callbacks.  This is the
 * body of the frontend's run_frame.  Returns 0 once the emulation thread has
 * stopped, 1 otherwise. */
int n64_glue_frame(void);

/* Current N64 pad state, applied on the next input poll.  buttons uses the
 * mupen64plus BUTTONS bit layout, stick_x/stick_y are the analog axes in the
 * range -128..127. */
void n64_glue_set_input(uint32_t buttons, int8_t stick_x, int8_t stick_y);

/* ROM title reported by the core (falls back to the raw ROM header). */
void n64_glue_get_rom_name(char* out, size_t cap);

/* Region detected from the ROM header: 0 NTSC, 1 PAL, 2 MPAL. */
int n64_glue_get_region(void);

/* CRC32 of the loaded ROM (the frontend's console detection value). */
uint32_t n64_glue_get_crc32(void);

/* ROM size in bytes. */
unsigned n64_glue_get_rom_size(void);

/* Battery save type: non-zero when the cartridge has battery-backed storage. */
int n64_glue_get_has_battery(void);

/* ------------------------------------------------------------------------- */
/* Save states                                                               */
/* ------------------------------------------------------------------------- */

/* Save the current state.  Pass buffer == NULL to query the required size.
 * Returns the number of bytes required (probe) or written, or 0 on failure. */
size_t n64_glue_save_state(uint8_t* buffer, size_t buffer_size);

/* Load a state previously produced by n64_glue_save_state.  Returns 0 on
 * success. */
int n64_glue_load_state(const uint8_t* buffer, size_t size);

/* ------------------------------------------------------------------------- */
/* Teardown                                                                  */
/* ------------------------------------------------------------------------- */

/* Stop the emulation thread, close the ROM and shut the core down. */
void n64_glue_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* NINTENDALL_N64_GLUE_H */
