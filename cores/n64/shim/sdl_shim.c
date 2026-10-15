/**
 * Nintendall - minimal SDL2 shim implementation (Win32)
 *
 * Implements the mutex/condition/thread/timer subset declared in the shim
 * headers. See SDL.h for why this exists at all.
 */

#include "SDL.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdlib.h>

/* ------------------------------------------------------------------------- */
/* Types                                                                     */
/* ------------------------------------------------------------------------- */

struct SDL_mutex {
    CRITICAL_SECTION cs;
};

struct SDL_cond {
    CONDITION_VARIABLE cv;
};

struct n64_sdl_thread {
    HANDLE handle;
    SDL_ThreadFunction fn;
    void* data;
    int result;
};

/* ------------------------------------------------------------------------- */
/* Mutex                                                                     */
/* ------------------------------------------------------------------------- */

SDL_mutex* SDL_CreateMutex(void)
{
    SDL_mutex* m = (SDL_mutex*)malloc(sizeof(*m));
    if (!m) return NULL;
    InitializeCriticalSection(&m->cs);
    return m;
}

void SDL_DestroyMutex(SDL_mutex* mutex)
{
    if (!mutex) return;
    DeleteCriticalSection(&mutex->cs);
    free(mutex);
}

int SDL_LockMutex(SDL_mutex* mutex)
{
    if (!mutex) return -1;
    EnterCriticalSection(&mutex->cs);
    return 0;
}

int SDL_UnlockMutex(SDL_mutex* mutex)
{
    if (!mutex) return -1;
    LeaveCriticalSection(&mutex->cs);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Condition variables                                                       */
/* ------------------------------------------------------------------------- */

SDL_cond* SDL_CreateCond(void)
{
    SDL_cond* c = (SDL_cond*)malloc(sizeof(*c));
    if (!c) return NULL;
    InitializeConditionVariable(&c->cv);
    return c;
}

void SDL_DestroyCond(SDL_cond* cond)
{
    if (!cond) return;
    free(cond);
}

int SDL_CondWait(SDL_cond* cond, SDL_mutex* mutex)
{
    if (!cond || !mutex) return -1;
    return SleepConditionVariableCS(&cond->cv, &mutex->cs, INFINITE) ? 0 : -1;
}

int SDL_CondSignal(SDL_cond* cond)
{
    if (!cond) return -1;
    WakeConditionVariable(&cond->cv);
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Threads                                                                   */
/* ------------------------------------------------------------------------- */

static DWORD WINAPI n64_sdl_thread_trampoline(LPVOID param)
{
    struct n64_sdl_thread* t = (struct n64_sdl_thread*)param;
    t->result = t->fn(t->data);
    return (DWORD)t->result;
}

SDL_Thread* SDL_CreateThread(SDL_ThreadFunction fn, const char* name, void* data)
{
    struct n64_sdl_thread* t;
    (void)name;

    if (!fn) return NULL;

    t = (struct n64_sdl_thread*)malloc(sizeof(*t));
    if (!t) return NULL;

    t->fn = fn;
    t->data = data;
    t->result = 0;
    t->handle = CreateThread(NULL, 0, n64_sdl_thread_trampoline, t, 0, NULL);
    if (!t->handle) {
        free(t);
        return NULL;
    }

    return (SDL_Thread*)t;
}

void SDL_WaitThread(SDL_Thread* thread, int* status)
{
    struct n64_sdl_thread* t = (struct n64_sdl_thread*)thread;
    DWORD rc = 0;

    if (!t) return;

    WaitForSingleObject(t->handle, INFINITE);
    GetExitCodeThread(t->handle, &rc);
    if (status) *status = (int)rc;

    CloseHandle(t->handle);
    free(t);
}

void* SDL_ThreadID(void)
{
    return (void*)(uintptr_t)GetCurrentThreadId();
}

/* ------------------------------------------------------------------------- */
/* Timer                                                                     */
/* ------------------------------------------------------------------------- */

Uint32 SDL_GetTicks(void)
{
    return (Uint32)GetTickCount64();
}

void SDL_Delay(Uint32 ms)
{
    Sleep((DWORD)ms);
}

/* ------------------------------------------------------------------------- */
/* Video-adjacent no-ops                                                     */
/* ------------------------------------------------------------------------- */

uint32_t SDL_WasInit(uint32_t flags)
{
    (void)flags;
    return 0;
}

void SDL_Quit(void)
{
}

void SDL_PumpEvents(void)
{
}
