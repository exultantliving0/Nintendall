/*
 * Nintendall - fast N64 video bridge (GLideN64)
 *
 * GLideN64 is compiled as a static mupen64plus video plugin.  It expects the
 * frontend to provide:
 *
 *   - the mupen64plus Config API and PluginGetVersion through a DLL handle
 *     (MupenPlusAPIImpl.cpp uses DLSYM), and
 *   - the VidExt_* video extension that owns the window and the WGL context.
 *
 * This file supplies both.  The plugin renders into a child HWND that covers
 * the frontend's client area; the parent's WndProc is subclassed so the child
 * follows resizes (including borderless fullscreen) without the frontend
 * needing to know about GLideN64.
 *
 * The context is created on the emulation thread, because that is the thread
 * that calls gfx.romOpen() (and later every VI) inside mupen64plus.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <GL/gl.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define M64P_CORE_PROTOTYPES 1
#define M64P_PLUGIN_PROTOTYPES 1
#include "m64p_types.h"
#include "m64p_config.h"
#include "m64p_common.h"
#include "m64p_frontend.h"
#include "m64p_vidext.h"
#include "m64p_plugin.h"

#include "n64_video_fast.h"

/* GLideN64's C entry points.  PluginStartup/PluginShutdown/PluginGetVersion
 * are declared by m64p_common.h; the rest live in CommonPluginAPI.cpp and
 * MupenPlusPluginAPI.cpp and are not part of the public core headers. */
extern "C" {
int  CALL InitiateGFX(GFX_INFO info);
void CALL ProcessDList(void);
void CALL ProcessRDPList(void);
int  CALL RomOpen(void);
void CALL RomClosed(void);
void CALL UpdateScreen(void);
void CALL ShowCFB(void);
void CALL ViStatusChanged(void);
void CALL ViWidthChanged(void);
void CALL ChangeWindow(void);
void CALL MoveScreen(int xpos, int ypos);
void CALL SetRenderingCallback(void (*callback)(int));
void CALL ResizeVideoOutput(int width, int height);
void CALL FBRead(unsigned int addr);
void CALL FBWrite(unsigned int addr, unsigned int size);
void CALL FBGetFrameBufferInfo(void* info);
void CALL ReadScreen2(void* dest, int* width, int* height, int front);
}

#ifndef WGL_CONTEXT_MAJOR_VERSION_ARB
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#endif
#ifndef WGL_CONTEXT_MINOR_VERSION_ARB
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#endif
#ifndef WGL_CONTEXT_PROFILE_MASK_ARB
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#endif
#ifndef WGL_CONTEXT_CORE_PROFILE_BIT_ARB
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
#endif
#ifndef WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB
#define WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 0x00000002
#endif

typedef HGLRC (WINAPI *PFNWGLCREATECONTEXTATTRIBSARBPROC)(HDC, HGLRC, const int*);
typedef BOOL (WINAPI *PFNWGLSWAPINTERVALEXTPROC)(int);

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

static HWND  s_parent;
static HWND  s_child;
static WNDPROC s_parent_old_proc;
static int   s_subclassed;
static int   s_parent_added_clipchildren;
static int   s_child_class_registered;

static volatile LONG s_view_w = 960;
static volatile LONG s_view_h = 720;

static HDC   s_dc;
static HGLRC s_hglrc;

static int   s_attrs_doublebuffer = 1;
static int   s_attrs_depth = 24;
static int   s_attrs_stencil = 8;
static int   s_attrs_color = 32;
static int   s_attrs_alpha = 0;
static int   s_attrs_swap = 0;
static int   s_attrs_ms_buffers = 0;
static int   s_attrs_ms_samples = 0;
static int   s_attrs_major = 3;
static int   s_attrs_minor = 3;
static int   s_attrs_profile = M64P_GL_CONTEXT_PROFILE_CORE;

static int   s_plugin_started;
static volatile LONG s_rom_open;

/* ------------------------------------------------------------------------- */
/* Logging                                                                   */
/* ------------------------------------------------------------------------- */

static void fast_log(const char* fmt, ...)
{
    va_list ap;
    fputs("[n64/GLideN64] ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static void CALL fast_debug_callback(void* context, int level, const char* message)
{
    (void)context;
    if (message == NULL)
        return;
    fprintf(stderr, "[n64/GLideN64] debug(%d): %s\n", level, message);
    fflush(stderr);
}

/* ------------------------------------------------------------------------- */
/* Core API resolver (the static replacement for dlsym)                      */
/* ------------------------------------------------------------------------- */

extern "C" void* NintendallCoreSymbol(const char* name)
{
    if (name == NULL)
        return NULL;

#define CORE_SYM(fn) \
    if (strcmp(name, #fn) == 0) \
        return (void*)(uintptr_t)(&fn)

    CORE_SYM(ConfigGetSharedDataFilepath);
    CORE_SYM(ConfigGetUserConfigPath);
    CORE_SYM(ConfigGetUserDataPath);
    CORE_SYM(ConfigGetUserCachePath);
    CORE_SYM(ConfigOpenSection);
    CORE_SYM(ConfigDeleteSection);
    CORE_SYM(ConfigSaveSection);
    CORE_SYM(ConfigSaveFile);
    CORE_SYM(ConfigSetDefaultInt);
    CORE_SYM(ConfigSetDefaultFloat);
    CORE_SYM(ConfigSetDefaultBool);
    CORE_SYM(ConfigSetDefaultString);
    CORE_SYM(ConfigGetParamInt);
    CORE_SYM(ConfigGetParamFloat);
    CORE_SYM(ConfigGetParamBool);
    CORE_SYM(ConfigGetParamString);
    CORE_SYM(ConfigExternalGetParameter);
    CORE_SYM(ConfigExternalOpen);
    CORE_SYM(ConfigExternalClose);

    CORE_SYM(VidExt_Init);
    CORE_SYM(VidExt_InitWithRenderMode);
    CORE_SYM(VidExt_Quit);
    CORE_SYM(VidExt_ListFullscreenModes);
    CORE_SYM(VidExt_ListFullscreenRates);
    CORE_SYM(VidExt_SetVideoMode);
    CORE_SYM(VidExt_SetVideoModeWithRate);
    CORE_SYM(VidExt_ResizeWindow);
    CORE_SYM(VidExt_SetCaption);
    CORE_SYM(VidExt_ToggleFullScreen);
    CORE_SYM(VidExt_GL_GetProcAddress);
    CORE_SYM(VidExt_GL_SetAttribute);
    CORE_SYM(VidExt_GL_GetAttribute);
    CORE_SYM(VidExt_GL_SwapBuffers);
    CORE_SYM(VidExt_GL_GetDefaultFramebuffer);

    CORE_SYM(PluginGetVersion);

#undef CORE_SYM

    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Child window                                                              */
/* ------------------------------------------------------------------------- */

static LRESULT CALLBACK FastChildWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                         LPARAM lParam)
{
    switch (msg) {
    case WM_MOUSEACTIVATE:
        /* Never steal keyboard focus from the frontend window. */
        return MA_NOACTIVATE;
    case WM_SETFOCUS:
        if (GetParent(hwnd) != NULL)
            SetFocus(GetParent(hwnd));
        return 0;
    case WM_ERASEBKGND:
        return 1;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static int ensure_child_class(void)
{
    if (s_child_class_registered)
        return 0;

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = FastChildWndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"NintendallGLChild";

    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return -1;

    s_child_class_registered = 1;
    return 0;
}

static void update_child_bounds(void)
{
    if (s_parent == NULL)
        return;

    RECT rc;
    GetClientRect(s_parent, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0)
        return;

    InterlockedExchange(&s_view_w, (LONG)w);
    InterlockedExchange(&s_view_h, (LONG)h);

    if (s_child != NULL) {
        SetWindowPos(s_child, HWND_TOP, 0, 0, w, h,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
        if (InterlockedCompareExchange(&s_rom_open, 0, 0) != 0)
            ResizeVideoOutput(w, h);
    }

    /* Keep the parent from painting over the GL child, including after the
     * frontend swaps to its borderless-fullscreen style (which drops the bit). */
    LONG style = GetWindowLongW(s_parent, GWL_STYLE);
    if ((style & WS_CLIPCHILDREN) == 0) {
        SetWindowLongW(s_parent, GWL_STYLE, style | WS_CLIPCHILDREN);
        s_parent_added_clipchildren = 1;
    }
}

static LRESULT CALLBACK ParentSubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
                                           LPARAM lParam)
{
    LRESULT result = CallWindowProcW(s_parent_old_proc, hwnd, msg, wParam, lParam);

    if (msg == WM_SIZE) {
        if (wParam != SIZE_MINIMIZED)
            update_child_bounds();
    } else if (msg == WM_NCDESTROY) {
        /* The frontend is tearing the window down; the child dies with it. */
        s_parent = NULL;
        s_child = NULL;
        s_parent_old_proc = NULL;
        s_subclassed = 0;
    }

    return result;
}

int n64_fast_video_attach(void)
{
    if (s_parent == NULL)
        return -1;
    if (s_child != NULL) {
        update_child_bounds();
        return 0;
    }
    if (ensure_child_class() != 0) {
        fast_log("could not register the GL child window class");
        return -1;
    }

    RECT rc;
    GetClientRect(s_parent, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0)
        w = (int)InterlockedCompareExchange(&s_view_w, 0, 0);
    if (h <= 0)
        h = (int)InterlockedCompareExchange(&s_view_h, 0, 0);
    if (w <= 0) w = 960;
    if (h <= 0) h = 720;

    s_child = CreateWindowExW(
        0, L"NintendallGLChild", L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        0, 0, w, h, s_parent, NULL, GetModuleHandleW(NULL), NULL);
    if (s_child == NULL) {
        fast_log("could not create the GL child window (error %lu)", GetLastError());
        return -1;
    }

    InterlockedExchange(&s_view_w, (LONG)w);
    InterlockedExchange(&s_view_h, (LONG)h);
    SetWindowPos(s_child, HWND_TOP, 0, 0, w, h,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);

    LONG style = GetWindowLongW(s_parent, GWL_STYLE);
    if ((style & WS_CLIPCHILDREN) == 0) {
        SetWindowLongW(s_parent, GWL_STYLE, style | WS_CLIPCHILDREN);
        s_parent_added_clipchildren = 1;
    }

    s_parent_old_proc = (WNDPROC)SetWindowLongPtrW(
        s_parent, GWLP_WNDPROC, (LONG_PTR)ParentSubclassProc);
    if (s_parent_old_proc == NULL) {
        fast_log("could not subclass the frontend window (error %lu)", GetLastError());
        DestroyWindow(s_child);
        s_child = NULL;
        return -1;
    }
    s_subclassed = 1;

    fast_log("attached to parent HWND %p (%dx%d)", (void*)s_parent, w, h);
    return 0;
}

void n64_fast_video_detach(void)
{
    if (s_parent != NULL && s_subclassed) {
        WNDPROC current = (WNDPROC)GetWindowLongPtrW(s_parent, GWLP_WNDPROC);
        if (current == ParentSubclassProc)
            SetWindowLongPtrW(s_parent, GWLP_WNDPROC, (LONG_PTR)s_parent_old_proc);
        if (s_parent_added_clipchildren) {
            LONG style = GetWindowLongW(s_parent, GWL_STYLE);
            SetWindowLongW(s_parent, GWL_STYLE, style & ~WS_CLIPCHILDREN);
        }
    }
    s_subclassed = 0;
    s_parent_added_clipchildren = 0;
    s_parent_old_proc = NULL;

    if (s_child != NULL) {
        if (IsWindow(s_child))
            DestroyWindow(s_child);
        s_child = NULL;
    }
    s_parent = NULL;
}

void n64_fast_video_configure(void* parent_hwnd, int width, int height)
{
    HWND parent = (HWND)parent_hwnd;
    if (s_parent != NULL && s_parent != parent)
        n64_fast_video_detach();

    s_parent = parent;
    if (width > 0)
        InterlockedExchange(&s_view_w, (LONG)width);
    if (height > 0)
        InterlockedExchange(&s_view_h, (LONG)height);

    if (s_child != NULL)
        update_child_bounds();
}

/* ------------------------------------------------------------------------- */
/* WGL context                                                               */
/* ------------------------------------------------------------------------- */

static const char* wgl_profile_name(int profile)
{
    if (profile == M64P_GL_CONTEXT_PROFILE_CORE)
        return "core";
    if (profile == M64P_GL_CONTEXT_PROFILE_COMPATIBILITY)
        return "compatibility";
    return "other";
}

static void destroy_gl_context(void)
{
    if (s_hglrc != NULL) {
        wglMakeCurrent(NULL, NULL);
        wglDeleteContext(s_hglrc);
        s_hglrc = NULL;
    }
    if (s_dc != NULL) {
        if (s_child != NULL)
            ReleaseDC(s_child, s_dc);
        s_dc = NULL;
    }
}

static m64p_error create_gl_context(void)
{
    if (s_child == NULL)
        return M64ERR_INVALID_STATE;

    destroy_gl_context();

    s_dc = GetDC(s_child);
    if (s_dc == NULL) {
        fast_log("GetDC failed");
        return M64ERR_INTERNAL;
    }

    /* A new child window starts with no pixel format, even though the old
     * window (and its DC) had one.  Ask the DC itself rather than remembering
     * this across sessions: switching fast -> accurate -> fast recreates the
     * child, and wglCreateContext fails on a format-less DC. */
    if (GetPixelFormat(s_dc) == 0) {
        PIXELFORMATDESCRIPTOR pfd;
        ZeroMemory(&pfd, sizeof(pfd));
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = (BYTE)(s_attrs_color > 0 ? s_attrs_color : 32);
        pfd.cAlphaBits = (BYTE)(s_attrs_alpha > 0 ? s_attrs_alpha : 0);
        pfd.cDepthBits = (BYTE)(s_attrs_depth > 0 ? s_attrs_depth : 24);
        pfd.cStencilBits = (BYTE)(s_attrs_stencil > 0 ? s_attrs_stencil : 8);
        pfd.iLayerType = PFD_MAIN_PLANE;

        int pf = ChoosePixelFormat(s_dc, &pfd);
        if (pf == 0) {
            fast_log("ChoosePixelFormat failed (error %lu)", GetLastError());
            ReleaseDC(s_child, s_dc);
            s_dc = NULL;
            return M64ERR_UNSUPPORTED;
        }
        if (!SetPixelFormat(s_dc, pf, &pfd)) {
            fast_log("SetPixelFormat failed (error %lu)", GetLastError());
            ReleaseDC(s_child, s_dc);
            s_dc = NULL;
            return M64ERR_UNSUPPORTED;
        }
    }

    HGLRC legacy = wglCreateContext(s_dc);
    if (legacy == NULL) {
        fast_log("wglCreateContext failed (error %lu)", GetLastError());
        ReleaseDC(s_child, s_dc);
        s_dc = NULL;
        return M64ERR_INTERNAL;
    }
    if (!wglMakeCurrent(s_dc, legacy)) {
        fast_log("wglMakeCurrent(legacy) failed (error %lu)", GetLastError());
        wglDeleteContext(legacy);
        ReleaseDC(s_child, s_dc);
        s_dc = NULL;
        return M64ERR_INTERNAL;
    }

    HGLRC core = NULL;
    PFNWGLCREATECONTEXTATTRIBSARBPROC create_attribs =
        (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");
    if (create_attribs != NULL) {
        int profile_bit = (s_attrs_profile == M64P_GL_CONTEXT_PROFILE_COMPATIBILITY)
            ? WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB
            : WGL_CONTEXT_CORE_PROFILE_BIT_ARB;
        int attribs[] = {
            WGL_CONTEXT_MAJOR_VERSION_ARB, s_attrs_major > 0 ? s_attrs_major : 3,
            WGL_CONTEXT_MINOR_VERSION_ARB, s_attrs_minor > 0 ? s_attrs_minor : 3,
            WGL_CONTEXT_PROFILE_MASK_ARB, profile_bit,
            0
        };
        core = create_attribs(s_dc, NULL, attribs);
        if (core == NULL)
            fast_log("wglCreateContextAttribsARB(%d.%d %s) failed; "
                     "falling back to the compatibility context",
                     s_attrs_major, s_attrs_minor, wgl_profile_name(s_attrs_profile));
    } else {
        fast_log("wglCreateContextAttribsARB is unavailable");
    }

    if (core != NULL) {
        if (!wglMakeCurrent(s_dc, core)) {
            fast_log("wglMakeCurrent(core) failed (error %lu)", GetLastError());
            wglDeleteContext(core);
            wglMakeCurrent(NULL, NULL);
            wglDeleteContext(legacy);
            ReleaseDC(s_child, s_dc);
            s_dc = NULL;
            return M64ERR_INTERNAL;
        }
        wglDeleteContext(legacy);
        s_hglrc = core;
    } else {
        /* Keep the legacy context as a last resort.  GLideN64 will report a
         * missing 3.3 context itself if the driver really cannot provide one. */
        s_hglrc = legacy;
    }

    PFNWGLSWAPINTERVALEXTPROC swap_interval =
        (PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT");
    if (swap_interval != NULL)
        swap_interval(s_attrs_swap);

    const GLubyte* version = glGetString(GL_VERSION);
    const GLubyte* renderer = glGetString(GL_RENDERER);
    fast_log("GL context ready: %s (%s), swap interval %d",
             version ? (const char*)version : "unknown",
             renderer ? (const char*)renderer : "unknown",
             s_attrs_swap);

    return M64ERR_SUCCESS;
}

/* ------------------------------------------------------------------------- */
/* Core config used by GLideN64                                              */
/* ------------------------------------------------------------------------- */

static void set_config_int(const char* section, const char* parameter, int value)
{
    m64p_handle handle = NULL;
    if (ConfigOpenSection(section, &handle) == M64ERR_SUCCESS && handle != NULL)
        ConfigSetParameter(handle, parameter, M64TYPE_INT, &value);
}

static void set_config_bool(const char* section, const char* parameter, int value)
{
    m64p_handle handle = NULL;
    if (ConfigOpenSection(section, &handle) == M64ERR_SUCCESS && handle != NULL)
        ConfigSetParameter(handle, parameter, M64TYPE_BOOL, &value);
}

static void prepare_core_config(void)
{
    int w = (int)InterlockedCompareExchange(&s_view_w, 0, 0);
    int h = (int)InterlockedCompareExchange(&s_view_h, 0, 0);
    if (w < 320) w = 960;
    if (h < 240) h = 720;

    /* Video-General drives DisplayWindowMupen64plus::_start(). */
    set_config_bool("Video-General", "Fullscreen", 0);
    set_config_int("Video-General", "ScreenWidth", w);
    set_config_int("Video-General", "ScreenHeight", h);
    set_config_bool("Video-General", "VerticalSync", 0);

    /* The fast profile defaults to native N64 rendering, 4:3, no MSAA/FXAA,
     * no extra GL thread, no texture packs.  These are applied on every ROM
     * open; GLideN64's Config_LoadConfig() runs immediately before it starts
     * the display window. */
    set_config_bool("Video-GLideN64", "ThreadedVideo", 0);
    set_config_bool("Video-GLideN64", "AsyncShaderCompilation", 0);
    set_config_int("Video-GLideN64", "MultiSampling", 0);
    set_config_bool("Video-GLideN64", "FXAA", 0);
    set_config_int("Video-GLideN64", "AspectRatio", 1);       /* force 4:3 */
    set_config_int("Video-GLideN64", "BufferSwapMode", 0);    /* on VI */
    set_config_bool("Video-GLideN64", "EnableFBEmulation", 1);
    set_config_bool("Video-GLideN64", "EnableShadersStorage", 1);
    set_config_bool("Video-GLideN64", "EnableCustomSettings", 0);
    set_config_bool("Video-GLideN64", "txHiresEnable", 0);
    set_config_int("Video-GLideN64", "txEnhancementMode", 0);
    set_config_bool("Video-GLideN64", "ShowFPS", 0);
    set_config_bool("Video-GLideN64", "ShowVIS", 0);
    set_config_bool("Video-GLideN64", "ShowPercent", 0);
    set_config_bool("Video-GLideN64", "ShowInternalResolution", 0);
    set_config_bool("Video-GLideN64", "ShowRenderingResolution", 0);
    set_config_bool("Video-GLideN64", "ShowStatistics", 0);
}

/* ------------------------------------------------------------------------- */
/* Plugin lifecycle                                                          */
/* ------------------------------------------------------------------------- */

int n64_fast_video_plugin_startup(void)
{
    if (s_plugin_started)
        return 0;

    m64p_error result = PluginStartup(NULL, NULL, fast_debug_callback);
    if (result != M64ERR_SUCCESS) {
        fast_log("PluginStartup failed (%d)", (int)result);
        return -1;
    }

    s_plugin_started = 1;
    fast_log("GLideN64 plugin layer started");
    return 0;
}

void n64_fast_video_plugin_shutdown(void)
{
    if (!s_plugin_started)
        return;
    PluginShutdown();
    s_plugin_started = 0;
}

int n64_fast_video_initiate(const GFX_INFO* info)
{
    if (!s_plugin_started || info == NULL)
        return 0;
    return InitiateGFX(*info) ? 1 : 0;
}

int n64_fast_video_rom_open(void)
{
    if (!s_plugin_started || s_child == NULL)
        return 0;

    prepare_core_config();

    int ok = RomOpen();
    if (ok)
        InterlockedExchange(&s_rom_open, 1);
    else
        fast_log("GLideN64 RomOpen failed");
    return ok ? 1 : 0;
}

void n64_fast_video_rom_closed(void)
{
    InterlockedExchange(&s_rom_open, 0);
    if (s_plugin_started)
        RomClosed();
}

void n64_fast_video_process_dlist(void)
{
    if (s_plugin_started)
        ProcessDList();
}

void n64_fast_video_process_rdp_list(void)
{
    if (s_plugin_started)
        ProcessRDPList();
}

void n64_fast_video_update_screen(void)
{
    if (s_plugin_started)
        UpdateScreen();
}

void n64_fast_video_show_cfb(void)
{
    if (s_plugin_started)
        ShowCFB();
}

void n64_fast_video_vi_status_changed(void)
{
    if (s_plugin_started)
        ViStatusChanged();
}

void n64_fast_video_vi_width_changed(void)
{
    if (s_plugin_started)
        ViWidthChanged();
}

void n64_fast_video_change_window(void)
{
    if (s_plugin_started)
        ChangeWindow();
}

void n64_fast_video_move_screen(int x, int y)
{
    if (s_plugin_started)
        MoveScreen(x, y);
}

void n64_fast_video_set_rendering_callback(void (*callback)(int))
{
    if (s_plugin_started)
        SetRenderingCallback(callback);
}

void n64_fast_video_resize_output(int width, int height)
{
    if (!s_plugin_started || width <= 0 || height <= 0)
        return;
    if (InterlockedCompareExchange(&s_rom_open, 0, 0) == 0)
        return;

    /* DisplayWindow::setWindowSize() only records the request; the actual
     * viewport update happens on the emulation thread at the next VI. */
    ResizeVideoOutput(width, height);
}

void n64_fast_video_fb_read(unsigned int addr)
{
    if (s_plugin_started)
        FBRead(addr);
}

void n64_fast_video_fb_write(unsigned int addr, unsigned int size)
{
    if (s_plugin_started)
        FBWrite(addr, size);
}

void n64_fast_video_fb_get_info(void* info)
{
    if (s_plugin_started)
        FBGetFrameBufferInfo(info);
}

void n64_fast_video_read_screen(void* dest, int* width, int* height, int front)
{
    if (s_plugin_started)
        ReadScreen2(dest, width, height, front);
    else {
        if (width) *width = 0;
        if (height) *height = 0;
    }
}

void n64_fast_video_release_thread_context(void)
{
    /* Called on the emulation thread just before it exits.  Normally RomClosed
     * has already released the context; this keeps a stalled/aborted run from
     * leaving it current on a dead thread. */
    if (s_dc != NULL && wglGetCurrentContext() != NULL)
        wglMakeCurrent(NULL, NULL);
}

/* ------------------------------------------------------------------------- */
/* mupen64plus video extension                                               */
/* ------------------------------------------------------------------------- */

extern "C" m64p_error CALL VidExt_Init(void)
{
    return M64ERR_SUCCESS;
}

extern "C" m64p_error CALL VidExt_InitWithRenderMode(m64p_render_mode RenderMode)
{
    return RenderMode == M64P_RENDER_OPENGL ? M64ERR_SUCCESS : M64ERR_UNSUPPORTED;
}

extern "C" m64p_error CALL VidExt_Quit(void)
{
    destroy_gl_context();
    return M64ERR_SUCCESS;
}

extern "C" m64p_error CALL VidExt_ListFullscreenModes(m64p_2d_size* SizeArray,
                                                      int* NumSizes)
{
    (void)SizeArray;
    if (NumSizes != NULL)
        *NumSizes = 0;
    return M64ERR_SUCCESS;
}

extern "C" m64p_error CALL VidExt_ListFullscreenRates(m64p_2d_size Size,
                                                      int* NumRates, int* Rates)
{
    (void)Size; (void)Rates;
    if (NumRates != NULL)
        *NumRates = 0;
    return M64ERR_SUCCESS;
}

extern "C" m64p_error CALL VidExt_SetVideoMode(int Width, int Height,
                                               int BitsPerPixel,
                                               m64p_video_mode ScreenMode,
                                               m64p_video_flags Flags)
{
    (void)BitsPerPixel; (void)ScreenMode; (void)Flags;

    if (s_child == NULL)
        return M64ERR_INVALID_STATE;
    if (Width > 0 && Height > 0 && s_child != NULL)
        SetWindowPos(s_child, HWND_TOP, 0, 0, Width, Height,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);

    m64p_error result = create_gl_context();
    if (result != M64ERR_SUCCESS) {
        MessageBoxW(s_parent, L"Could not create an OpenGL 3.3 context for the "
                    L"fast N64 renderer.\n\nSwitch this game to the Accurate "
                    L"video backend, or update the graphics driver.",
                    L"Nintendall", MB_OK | MB_ICONERROR);
    }
    return result;
}

extern "C" m64p_error CALL VidExt_SetVideoModeWithRate(int Width, int Height,
                                                       int RefreshRate,
                                                       int BitsPerPixel,
                                                       m64p_video_mode ScreenMode,
                                                       m64p_video_flags Flags)
{
    (void)RefreshRate;
    return VidExt_SetVideoMode(Width, Height, BitsPerPixel, ScreenMode, Flags);
}

extern "C" m64p_error CALL VidExt_ResizeWindow(int Width, int Height)
{
    if (s_child == NULL)
        return M64ERR_INVALID_STATE;
    InterlockedExchange(&s_view_w, (LONG)Width);
    InterlockedExchange(&s_view_h, (LONG)Height);
    SetWindowPos(s_child, HWND_TOP, 0, 0, Width, Height,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    return M64ERR_SUCCESS;
}

extern "C" m64p_error CALL VidExt_SetCaption(const char* Title)
{
    (void)Title;   /* the frontend owns the title bar */
    return M64ERR_SUCCESS;
}

extern "C" m64p_error CALL VidExt_ToggleFullScreen(void)
{
    /* Fullscreen is the frontend's F11 feature, not GLideN64's. */
    return M64ERR_SUCCESS;
}

extern "C" m64p_function CALL VidExt_GL_GetProcAddress(const char* Proc)
{
    if (Proc == NULL)
        return NULL;

    PROC address = wglGetProcAddress(Proc);
    if (address == NULL || address == (PROC)1 || address == (PROC)2 ||
        address == (PROC)3 || address == (PROC)-1) {
        static HMODULE opengl32;
        if (opengl32 == NULL)
            opengl32 = LoadLibraryW(L"opengl32.dll");
        if (opengl32 != NULL)
            address = GetProcAddress(opengl32, Proc);
    }
    return (m64p_function)address;
}

extern "C" m64p_error CALL VidExt_GL_SetAttribute(m64p_GLattr Attr, int Value)
{
    switch (Attr) {
    case M64P_GL_DOUBLEBUFFER:        s_attrs_doublebuffer = Value; break;
    case M64P_GL_BUFFER_SIZE:         s_attrs_color = Value; break;
    case M64P_GL_DEPTH_SIZE:          s_attrs_depth = Value; break;
    case M64P_GL_RED_SIZE:
    case M64P_GL_GREEN_SIZE:
    case M64P_GL_BLUE_SIZE:           s_attrs_color = Value * 3; break;
    case M64P_GL_ALPHA_SIZE:          s_attrs_alpha = Value; break;
    case M64P_GL_SWAP_CONTROL:        s_attrs_swap = Value; break;
    case M64P_GL_MULTISAMPLEBUFFERS:  s_attrs_ms_buffers = Value; break;
    case M64P_GL_MULTISAMPLESAMPLES:  s_attrs_ms_samples = Value; break;
    case M64P_GL_CONTEXT_MAJOR_VERSION: s_attrs_major = Value; break;
    case M64P_GL_CONTEXT_MINOR_VERSION: s_attrs_minor = Value; break;
    case M64P_GL_CONTEXT_PROFILE_MASK:  s_attrs_profile = Value; break;
    default: break;
    }
    return M64ERR_SUCCESS;
}

extern "C" m64p_error CALL VidExt_GL_GetAttribute(m64p_GLattr Attr, int* pValue)
{
    if (pValue == NULL)
        return M64ERR_INVALID_STATE;

    switch (Attr) {
    case M64P_GL_DOUBLEBUFFER:        *pValue = s_attrs_doublebuffer; break;
    case M64P_GL_BUFFER_SIZE:         *pValue = s_attrs_color; break;
    case M64P_GL_DEPTH_SIZE:          *pValue = s_attrs_depth; break;
    case M64P_GL_RED_SIZE:            *pValue = 8; break;
    case M64P_GL_GREEN_SIZE:          *pValue = 8; break;
    case M64P_GL_BLUE_SIZE:           *pValue = 8; break;
    case M64P_GL_ALPHA_SIZE:          *pValue = s_attrs_alpha; break;
    case M64P_GL_SWAP_CONTROL:        *pValue = s_attrs_swap; break;
    case M64P_GL_MULTISAMPLEBUFFERS:  *pValue = s_attrs_ms_buffers; break;
    case M64P_GL_MULTISAMPLESAMPLES:  *pValue = s_attrs_ms_samples; break;
    case M64P_GL_CONTEXT_MAJOR_VERSION: *pValue = s_attrs_major; break;
    case M64P_GL_CONTEXT_MINOR_VERSION: *pValue = s_attrs_minor; break;
    case M64P_GL_CONTEXT_PROFILE_MASK:  *pValue = s_attrs_profile; break;
    default: *pValue = 0; break;
    }
    return M64ERR_SUCCESS;
}

extern "C" m64p_error CALL VidExt_GL_SwapBuffers(void)
{
    HDC dc = s_dc != NULL ? s_dc : wglGetCurrentDC();
    if (dc != NULL)
        SwapBuffers(dc);
    return M64ERR_SUCCESS;
}

extern "C" uint32_t CALL VidExt_GL_GetDefaultFramebuffer(void)
{
    return 0;
}
