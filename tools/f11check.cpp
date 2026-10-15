// =============================================================================
//  f11check - developer tool that verifies the F11 fullscreen toggle.
//
//  Launches nintendall.exe, posts a real F11 keypress, then reports the
//  window's style bits and geometry before/after, and grabs a screenshot so
//  the result can be eyeballed.  Not shipped with the installer.
//
//    usage: f11check <path-to-nintendall.exe> <out.bmp>
// =============================================================================

#include <windows.h>
#include <windowsx.h>
#include <stdio.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

static void Describe(const char* label, HWND hwnd) {
    RECT wr, cr;
    GetWindowRect(hwnd, &wr);
    GetClientRect(hwnd, &cr);

    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    LONG ex    = GetWindowLongW(hwnd, GWL_EXSTYLE);
    HMENU menu = GetMenu(hwnd);
    bool  vis   = IsWindowVisible(hwnd);

    printf("[%s]\n", label);
    printf("  window rect : %d,%d  %dx%d\n",
           wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top);
    printf("  client rect : 0,0  %dx%d\n", cr.right - cr.left, cr.bottom - cr.top);
    printf("  non-client  : left=%d top=%d right=%d bottom=%d\n",
           cr.left, cr.top, cr.right, cr.bottom);
    printf("  style       : 0x%08lX  ex: 0x%08lX  visible=%d\n",
           (unsigned long)style, (unsigned long)ex, vis ? 1 : 0);
    printf("  menu        : %p\n", (void*)menu);
    printf("  WS_CAPTION=%d WS_THICKFRAME=%d WS_SYSMENU=%d WS_BORDER=%d WS_POPUP=%d\n",
           (style & WS_CAPTION)     ? 1 : 0,
           (style & WS_THICKFRAME)  ? 1 : 0,
           (style & WS_SYSMENU)     ? 1 : 0,
           (style & WS_BORDER)      ? 1 : 0,
           ((style & WS_POPUP) == WS_POPUP) ? 1 : 0);
    printf("  WS_EX_APPWINDOW=%d WS_EX_TOPMOST=%d\n",
           (ex & WS_EX_APPWINDOW) ? 1 : 0,
           (ex & WS_EX_TOPMOST)   ? 1 : 0);

    // The decisive check: any leftover non-client area at the top is what
    // shows up as a white bar in fullscreen.
    int top_gap = cr.top;
    int left_gap = cr.left;
    printf("  VERDICT     : %s\n",
           (top_gap == 0 && left_gap == 0)
               ? "client fills window (no frame)"
               : "NON-CLIENT AREA PRESENT -> visible bar");
    printf("\n");
}

static bool SaveBmp(HWND hwnd, const char* path) {
    RECT r;
    GetWindowRect(hwnd, &r);
    int w = r.right - r.left;
    int h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return false;

    HDC screen = GetDC(nullptr);
    HDC mem    = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY);

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;   // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount     = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HANDLE fh = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) return false;

    BITMAPFILEHEADER fhdr;
    ZeroMemory(&fhdr, sizeof(fhdr));
    DWORD img_size = (DWORD)(w * h * 4);
    fhdr.bfType      = 0x4D42;          // 'BM'
    fhdr.bfOffBits   = sizeof(fhdr) + sizeof(bi);
    fhdr.bfSize      = fhdr.bfOffBits + img_size;

    DWORD written = 0;
    WriteFile(fh, &fhdr, sizeof(fhdr), &written, nullptr);
    WriteFile(fh, &bi, sizeof(bi), &written, nullptr);

    // Lock the bits and copy them straight out, row by row.
    BITMAPINFO bi2 = bi;
    void* bits = nullptr;
    if (GetDIBits(mem, bmp, 0, (UINT)h, nullptr, &bi2, DIB_RGB_COLORS)) {
        bits = malloc(img_size);
        GetDIBits(mem, bmp, 0, (UINT)h, bits, &bi2, DIB_RGB_COLORS);
        WriteFile(fh, bits, img_size, &written, nullptr);
        free(bits);
    }

    CloseHandle(fh);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    printf("  screenshot  : %s (%dx%d)\n", path, w, h);
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: f11check <nintendall.exe> [out.bmp]\n"); return 2; }
    const char* exe = argv[1];
    const char* bmp = (argc > 2) ? argv[2] : "f11check.bmp";

    printf("launching %s\n\n", exe);
    STARTUPINFOA si;  ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessA(exe, nullptr, nullptr, nullptr, FALSE, 0, nullptr,
                        nullptr, &si, &pi)) {
        printf("CreateProcess failed: %lu\n", GetLastError());
        return 1;
    }
    printf("pid %lu\n\n", (unsigned long)pi.dwProcessId);

    // Wait for the main window to appear.
    HWND hwnd = nullptr;
    for (int i = 0; i < 60 && !hwnd; ++i) {
        Sleep(250);
        hwnd = FindWindowExA(nullptr, nullptr, "NintendallWindow", nullptr);
    }
    if (!hwnd) {
        printf("could not find the Nintendall window\n");
        TerminateProcess(pi.hProcess, 0);
        return 1;
    }

    Sleep(800);
    printf("hwnd %p (pid %lu)\n\n", (void*)hwnd, (unsigned long)pi.dwProcessId);
    Describe("WINDOWED", hwnd);

    // Post a genuine F11 so the app runs its own handler.
    PostMessageW(hwnd, WM_KEYDOWN, VK_F11, 0);
    PostMessageW(hwnd, WM_KEYUP,   VK_F11, 0);
    Sleep(1200);

    Describe("FULLSCREEN (after F11)", hwnd);
    SaveBmp(hwnd, bmp);

    // And back out again, to prove the toggle is reversible.
    PostMessageW(hwnd, WM_KEYDOWN, VK_F11, 0);
    PostMessageW(hwnd, WM_KEYUP,   VK_F11, 0);
    Sleep(1000);

    Describe("WINDOWED AGAIN (after 2nd F11)", hwnd);

    PostMessageW(hwnd, WM_CLOSE, 0, 0);
    Sleep(600);
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}