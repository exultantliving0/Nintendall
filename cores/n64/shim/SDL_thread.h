/**
 * Nintendall - minimal SDL2 threading shim
 *
 * Supplies the mutex/condition/thread subset used by mupen64plus-core
 * (main/workqueue.c, main/savestates.c, main/cheat.c) on top of the Win32
 * synchronisation primitives. Argument order follows SDL2, not SDL1:
 *
 *   SDL_CreateThread(fn, name, data)
 *   SDL_CondWait(cond, mutex)
 *
 * Only the symbols the core actually references are declared here; anything
 * else belongs in sdl_shim.c behind the same names.
 */

#ifndef NINTENDALL_SDL_THREAD_H
#define NINTENDALL_SDL_THREAD_H

#ifdef __cplusplus
extern "C" {
#endif

#define SDLCALL

typedef struct SDL_mutex SDL_mutex;
typedef struct SDL_cond SDL_cond;

/* Opaque thread handle (a Win32 HANDLE underneath). */
typedef void SDL_Thread;

typedef int (SDLCALL *SDL_ThreadFunction)(void* data);

SDL_mutex* SDL_CreateMutex(void);
void       SDL_DestroyMutex(SDL_mutex* mutex);
/* SDL2 returns 0 on success and -1 on failure; the core checks the result. */
int        SDL_LockMutex(SDL_mutex* mutex);
int        SDL_UnlockMutex(SDL_mutex* mutex);

SDL_cond* SDL_CreateCond(void);
void      SDL_DestroyCond(SDL_cond* cond);
int       SDL_CondWait(SDL_cond* cond, SDL_mutex* mutex);
int       SDL_CondSignal(SDL_cond* cond);

SDL_Thread* SDL_CreateThread(SDL_ThreadFunction fn, const char* name, void* data);
void        SDL_WaitThread(SDL_Thread* thread, int* status);

/* Needed by the core's timer/thread bookkeeping. */
void* SDL_ThreadID(void);

#ifdef __cplusplus
}
#endif

#endif /* NINTENDALL_SDL_THREAD_H */
