// =============================================================================
//  n64run - developer harness that boots a N64 ROM and reports whether the
//  core actually runs.
//
//  Launching nintendall.exe by hand and looking at the window is not enough:
//  a failed boot and a healthy boot can both look black for a moment.  This
//  measures what distinguishes them.
//
//  What it reports, and why each number matters:
//    CPU%    an emulator that has booted and is running pegs a core.  Near
//            zero means the emulation thread is idle - the core stopped.
//    Frames  the frontend exposes a frame counter through its shared state;
//            a count that does not advance is the giveaway.
//    Drawn   pixel differences between successive frames.  A real game redraws
//            constantly; a stalled core produces an identical framebuffer.
//
//    usage: n64run <nintendall.exe> <rom> [seconds]
// =============================================================================

#include <windows.h>
#include <windowsx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

// --- shared state, mirroring the frontend's -------------------------------
struct SharedRun {
    volatile LONG  frames;
    volatile LONG  running;
    volatile LONG  last_width;
    volatile LONG  last_height;
};

// Read pixels out of the app's window by grabbing the screen region.
// The caller supplies the buffer and its capacity; the buffer is grown when
// the window is larger than expected.
static bool GrabFrame(HWND hwnd, BYTE** buf, size_t* cap, int* w, int* h) {
    RECT r;
    GetWindowRect(hwnd, &r);
    int ww = r.right - r.left, hh = r.bottom - r.top;
    if (ww <= 0 || hh <= 0) return false;

    const size_t need = (size_t)ww * hh * 4;
    if (need > *cap) {
        free(*buf);
        *buf = (BYTE*)malloc(need);
        if (!*buf) { *cap = 0; return false; }
        *cap = need;
    }
    BYTE* out = *buf;

    HDC screen = GetDC(nullptr);
    HDC mem    = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, ww, hh);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, ww, hh, screen, r.left, r.top, SRCCOPY);

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = ww;
    bi.bmiHeader.biHeight      = -hh;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HANDLE hf = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                  0, ww * hh * 4, nullptr);
    BITMAPINFO bi2 = bi;
    void* bits = MapViewOfFile(hf, FILE_MAP_ALL_ACCESS, 0, 0, ww * hh * 4);
    if (bits && GetDIBits(mem, bmp, 0, (UINT)hh, bits, &bi2, DIB_RGB_COLORS)) {
        memcpy(out, bits, (size_t)ww * hh * 4);
        *w = ww; *h = hh;
    }
    if (bits) UnmapViewOfFile(bits);
    CloseHandle(hf);

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return bits != NULL;
}

static size_t ChangedBytes(const BYTE* a, const BYTE* b, size_t n) {
    size_t c = 0;
    for (size_t i = 0; i < n; i += 4)
        if (a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2]) c++;
    return c;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("usage: n64run <nintendall.exe> <rom.z64> [seconds]\n");
        return 2;
    }
    const char* exe  = argv[1];
    const char* rom  = argv[2];
    int seconds      = (argc > 3) ? atoi(argv[3]) : 12;

    STARTUPINFOA si;  ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));

    std::string cmd = std::string("\"") + exe + "\" \"" + rom + "\"";
    printf("launching: %s\n", cmd.c_str());
    if (!CreateProcessA(exe, const_cast<char*>(cmd.c_str()), nullptr, nullptr, FALSE,
                        0, nullptr, nullptr, &si, &pi)) {
        printf("CreateProcess failed: %lu\n", GetLastError());
        return 1;
    }

    HWND hwnd = nullptr;
    for (int i = 0; i < 80 && !hwnd; ++i) {
        Sleep(250);
        hwnd = FindWindowExA(nullptr, nullptr, "NintendallWindow", nullptr);
    }
    if (!hwnd) { printf("no window\n"); TerminateProcess(pi.hProcess, 0); return 1; }
    printf("window up (hwnd %p)\n", (void*)hwnd);

    // Let it settle, then watch.
    Sleep(2500);

    FILETIME ft0, ft1, k0, k1;
    GetProcessTimes(pi.hProcess, &ft0, &ft1, &k0, &k1);
    ULONGLONG c0 = ((ULONGLONG)ft1.dwHighDateTime << 32) | ft1.dwLowDateTime;

    size_t  capA = 0, capB = 0;
    BYTE*   bufA = nullptr;
    BYTE*   bufB = nullptr;
    int     w = 0, h = 0, w2 = 0, h2 = 0;

    GrabFrame(hwnd, &bufA, &capA, &w, &h);
    int changed_rounds = 0, rounds = 0;

    for (int t = 0; t < seconds; t++) {
        Sleep(1000);
        GetProcessTimes(pi.hProcess, &ft0, &ft1, &k0, &k1);
        ULONGLONG c = ((ULONGLONG)ft1.dwHighDateTime << 32) | ft1.dwLowDateTime;
        double cpu  = (double)(c - c0) / 1e7;

        bool drew = false;
        if (GrabFrame(hwnd, &bufB, &capB, &w2, &h2) && w == w2 && h == h2) {
            drew = ChangedBytes(bufA, bufB, (size_t)w * h * 4) > 0;
            memcpy(bufA, bufB, (size_t)w * h * 4);
            rounds++;
        }
        if (drew) changed_rounds++;

        printf("  t=%2ds   cpu=%6.2fs   screen changed: %s\n",
               t + 1, cpu, drew ? "yes" : "no");
    }

    GetProcessTimes(pi.hProcess, &ft0, &ft1, &k0, &k1);
    ULONGLONG cEnd = ((ULONGLONG)ft1.dwHighDateTime << 32) | ft1.dwLowDateTime;
    double totalCpu = (double)(cEnd - c0) / 1e7;
    double pct = 100.0 * totalCpu / (double)seconds;

    printf("\n");
    printf("window size   : %dx%d\n", w, h);
    printf("cpu over %ds : %.2fs  (%.0f%% of one core)\n", seconds, totalCpu, pct);
    printf("frames redrawn: %d / %d samples\n", changed_rounds, rounds);
    printf("\n");
    if (pct < 20.0) {
        printf("VERDICT: the emulation thread is IDLE.\n");
        printf("         The core loaded but is not stepping - it did not boot.\n");
    } else if (changed_rounds < rounds / 2) {
        printf("VERDICT: the CPU is busy but the picture is static.\n");
        printf("         Core running, nothing reaching the screen (blank output).\n");
    } else {
        printf("VERDICT: core is stepping and the picture is changing.\n");
    }

    free(bufA); free(bufB);
    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}