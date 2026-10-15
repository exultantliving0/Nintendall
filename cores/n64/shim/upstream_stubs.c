/*
 * Nintendall - mupen64plus-core build stubs
 *
 * The mupen64plus core keeps a handful of frontend-ish modules that we do not
 * compile into the static core library:
 *
 *   api/vidext.c     - SDL + OpenGL window creation / GL context handling
 *   main/eventloop.c - SDL keyboard event pump and hotkey handling
 *   main/screenshot.c- screenshot capture and PNG/GIF writing
 *   main/lirc.c      - LIRC remote control  (guarded by WITH_LIRC)
 *   main/netplay.c   - netplay              (guarded by M64P_NETPLAY)
 *   api/debugger.c   - debugger             (guarded by DBG)
 *
 * main/main.c and api/frontend.c still reference a small number of symbols
 * from those modules, so this file provides minimal, headless replacements.
 *
 * We never call any of the SDL keyboard/eventloop or screenshot entry points
 * ourselves - the frontend supplies input and video - but the linker still
 * needs them to resolve.
 *
 * Note: this core does *not* create a rendering window of its own.  The
 * software RDP (angrylion) hands finished frames to our glue layer, which
 * passes them to the Win32 frontend.  Hence every VidExt_* function below is
 * a harmless no-op.
 */

#include <stddef.h>

#include "m64p_types.h"
#include "m64p_vidext.h"
#include "m64p_frontend.h"

/* api/vidext.h (private to the core library) */
#include "vidext.h"

/* main/eventloop.h and main/screenshot.h live under main/ on the include path */
#include "eventloop.h"
#include "screenshot.h"

/* ------------------------------------------------------------------------- */
/* api/m64p_vidext.h                                                         */
/* ------------------------------------------------------------------------- */

m64p_error CALL VidExt_Init(void)
{
    return M64ERR_SUCCESS;
}

m64p_error CALL VidExt_InitWithRenderMode(m64p_render_mode RenderMode)
{
    (void)RenderMode;
    return M64ERR_SUCCESS;
}

m64p_error CALL VidExt_Quit(void)
{
    return M64ERR_SUCCESS;
}

m64p_error CALL VidExt_ListFullscreenModes(m64p_2d_size *SizeArray, int *NumSizes)
{
    (void)SizeArray;
    if (NumSizes != NULL)
        *NumSizes = 0;
    return M64ERR_SUCCESS;
}

m64p_error CALL VidExt_ListFullscreenRates(m64p_2d_size Size, int *NumRates, int *Rates)
{
    (void)Size;
    (void)Rates;
    if (NumRates != NULL)
        *NumRates = 0;
    return M64ERR_SUCCESS;
}

m64p_error CALL VidExt_SetVideoMode(int Width, int Height, int BitsPerPixel,
                                    m64p_video_mode ScreenMode, m64p_video_flags Flags)
{
    (void)Width; (void)Height; (void)BitsPerPixel; (void)ScreenMode; (void)Flags;
    return M64ERR_SUCCESS;
}

m64p_error CALL VidExt_SetVideoModeWithRate(int Width, int Height, int RefreshRate,
                                            int BitsPerPixel, m64p_video_mode ScreenMode,
                                            m64p_video_flags Flags)
{
    (void)Width; (void)Height; (void)RefreshRate; (void)BitsPerPixel;
    (void)ScreenMode; (void)Flags;
    return M64ERR_SUCCESS;
}

m64p_error CALL VidExt_ResizeWindow(int Width, int Height)
{
    (void)Width; (void)Height;
    return M64ERR_SUCCESS;
}

m64p_error CALL VidExt_SetCaption(const char *Title)
{
    (void)Title;
    return M64ERR_SUCCESS;
}

m64p_error CALL VidExt_ToggleFullScreen(void)
{
    return M64ERR_SUCCESS;
}

m64p_function CALL VidExt_GL_GetProcAddress(const char *Proc)
{
    (void)Proc;
    return NULL;
}

m64p_error CALL VidExt_GL_SetAttribute(m64p_GLattr Attr, int Value)
{
    (void)Attr; (void)Value;
    return M64ERR_SUCCESS;
}

m64p_error CALL VidExt_GL_GetAttribute(m64p_GLattr Attr, int *pValue)
{
    (void)Attr;
    if (pValue != NULL)
        *pValue = 0;
    return M64ERR_SUCCESS;
}

m64p_error CALL VidExt_GL_SwapBuffers(void)
{
    return M64ERR_SUCCESS;
}

uint32_t CALL VidExt_GL_GetDefaultFramebuffer(void)
{
    return 0;
}

m64p_error CALL VidExt_VK_GetSurface(void **pSurface, void *pInstance)
{
    (void)pSurface; (void)pInstance;
    return M64ERR_UNSUPPORTED;
}

m64p_error CALL VidExt_VK_GetInstanceExtensions(const char **ppExtensions, uint32_t *pNumExtensions)
{
    (void)ppExtensions;
    if (pNumExtensions != NULL)
        *pNumExtensions = 0;
    return M64ERR_UNSUPPORTED;
}

/* ------------------------------------------------------------------------- */
/* api/vidext.h (core-private entry points)                                   */
/* ------------------------------------------------------------------------- */

m64p_error OverrideVideoFunctions(m64p_video_extension_functions *VideoFunctionStruct)
{
    /* The frontend never installs a video extension, so there is nothing to
     * remember.  Just report that we do not accept one. */
    (void)VideoFunctionStruct;
    return M64ERR_UNSUPPORTED;
}

int VidExt_InFullscreenMode(void)
{
    return 0;
}

int VidExt_VideoRunning(void)
{
    /* Report "no video subsystem".  main.c only uses this to answer
     * M64CORE_VIDEO_MODE queries, which the frontend never issues. */
    return 0;
}

/* ------------------------------------------------------------------------- */
/* main/eventloop.c                                                           */
/* ------------------------------------------------------------------------- */

int event_set_core_defaults(void)
{
    /* main_set_core_defaults() only proceeds when this returns non-zero.
     * Returning non-zero means "defaults are installed successfully". */
    return 1;
}

void event_initialize(void)
{
    /* Nothing to initialise - no SDL event pump, no hotkey table. */
}

void event_sdl_keydown(int keysym, int keymod)
{
    (void)keysym; (void)keymod;
}

void event_sdl_keyup(int keysym, int keymod)
{
    (void)keysym; (void)keymod;
}

int event_gameshark_active(void)
{
    return 0;
}

void event_set_gameshark(int active)
{
    (void)active;
}

/* ------------------------------------------------------------------------- */
/* main/screenshot.c                                                          */
/* ------------------------------------------------------------------------- */

void ScreenshotRomOpen(void)
{
    /* Called from CoreDoCommand(M64CMD_ROM_OPEN).  No screenshot filename was
     * supplied, so there is nothing to arm. */
}

void TakeScreenshot(int iFrameNumber)
{
    (void)iFrameNumber;
}
