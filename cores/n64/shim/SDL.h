/**
 * Nintendall - minimal SDL2 shim
 *
 * The mupen64plus-core tree expects <SDL.h> to pull in the timer and thread
 * APIs. Nintendall does not link SDL, so this header re-exports the two shim
 * headers plus the handful of video-adjacent entry points the core touches
 * (SDL_WasInit/SDL_Quit/SDL_PumpEvents). Everything is implemented in
 * sdl_shim.c using plain Win32.
 */

#ifndef NINTENDALL_SDL_H
#define NINTENDALL_SDL_H

#include "SDL_timer.h"
#include "SDL_thread.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int SDL_bool;
#define SDL_TRUE  1
#define SDL_FALSE 0

/* No SDL subsystems are ever initialised, so SDL_WasInit(0) reports 0 and the
 * core correctly concludes the frontend is not an SDL application. */
uint32_t SDL_WasInit(uint32_t flags);
void     SDL_Quit(void);
void     SDL_PumpEvents(void);

#ifdef __cplusplus
}
#endif

#endif /* NINTENDALL_SDL_H */
