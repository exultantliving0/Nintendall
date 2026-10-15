/**
 * Nintendall - minimal SDL2 timer shim
 *
 * mupen64plus-core includes <SDL_timer.h> for SDL_GetTicks/SDL_Delay. Nintendall
 * has no SDL dependency, so this header (and sdl_shim.c) provide just enough of
 * the SDL2 surface for the core to build and run on plain Win32.
 */

#ifndef NINTENDALL_SDL_TIMER_H
#define NINTENDALL_SDL_TIMER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t Uint32;

/* Milliseconds since the process started. */
Uint32 SDL_GetTicks(void);

/* Sleep for the given number of milliseconds. */
void SDL_Delay(Uint32 ms);

#ifdef __cplusplus
}
#endif

#endif /* NINTENDALL_SDL_TIMER_H */
