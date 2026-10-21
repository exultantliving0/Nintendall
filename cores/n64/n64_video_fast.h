/*
 * Nintendall - fast N64 video bridge (GLideN64)
 *
 * GLideN64 is linked as a static plugin, so it cannot load the mupen64plus
 * core API through a DLL handle.  n64_video_fast.cpp plays the part of both
 * the frontend (child HWND + WGL context + VidExt_* entry points) and the
 * dynamic loader (NintendallCoreSymbol()).
 *
 * The glue layer selects this path only when the user chose the fast video
 * backend.  The accurate angrylion/-cxd4 path stays compiled in and untouched.
 */

#ifndef NINTENDALL_N64_VIDEO_FAST_H
#define NINTENDALL_N64_VIDEO_FAST_H

#include <stdint.h>

#include "m64p_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Remember the parent window and the client-area size used for the next ROM.
 * Safe to call before n64_glue_startup(). */
void n64_fast_video_configure(void* parent_hwnd, int width, int height);

/* Create/destroy the child rendering HWND and its parent subclass. */
int  n64_fast_video_attach(void);
void n64_fast_video_detach(void);

/* Bring GLideN64's plugin layer up/down.  PluginStartup() reads the core
 * config API and registers the Video-GLideN64 defaults. */
int  n64_fast_video_plugin_startup(void);
void n64_fast_video_plugin_shutdown(void);

/* Plugin table entry points used by n64_glue.c. */
int  n64_fast_video_initiate(const GFX_INFO* info);
int  n64_fast_video_rom_open(void);
void n64_fast_video_rom_closed(void);
void n64_fast_video_process_dlist(void);
void n64_fast_video_process_rdp_list(void);
void n64_fast_video_update_screen(void);
void n64_fast_video_show_cfb(void);
void n64_fast_video_vi_status_changed(void);
void n64_fast_video_vi_width_changed(void);
void n64_fast_video_change_window(void);
void n64_fast_video_move_screen(int x, int y);
void n64_fast_video_set_rendering_callback(void (*callback)(int));
void n64_fast_video_resize_output(int width, int height);
void n64_fast_video_fb_read(unsigned int addr);
void n64_fast_video_fb_write(unsigned int addr, unsigned int size);
void n64_fast_video_fb_get_info(void* info);
void n64_fast_video_read_screen(void* dest, int* width, int* height, int front);

/* Called on the emulation thread as it exits, before the frontend joins it. */
void n64_fast_video_release_thread_context(void);

#ifdef __cplusplus
}
#endif

#endif /* NINTENDALL_N64_VIDEO_FAST_H */
