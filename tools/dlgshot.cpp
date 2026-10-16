// Developer tool: opens the controller mapping dialog, screenshots it, and
// closes it.  Lets the dialog be checked without anyone clicking through it.
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "input_dialog.h"
#include "input_config.h"

static HBRUSH g_bg = nullptr;
static HDC     g_mem = nullptr;
static HBITMAP g_bmp = nullptr;
static HWND    g_target = nullptr;

static void SaveBMP(const char* path) {
    RECT rc;
    GetWindowRect(g_target, &rc);
    int W = rc.right - rc.left, H = rc.bottom - rc.top;
    if (W <= 0 || H <= 0) return;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, W, H);
    HGDIOBJ old = SelectObject(mem, bmp);

    // PW_RENDERFULLCONTENT so the whole window lands in the bitmap.
    PrintWindow(g_target, mem, 2);

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = W;
    bi.bmiHeader.biHeight      = -H;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HANDLE hf = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf != INVALID_HANDLE_VALUE) {
        BITMAPFILEHEADER fhdr;
        ZeroMemory(&fhdr, sizeof(fhdr));
        fhdr.bfType    = 0x4D42;
        fhdr.bfOffBits = sizeof(fhdr) + sizeof(bi);
        fhdr.bfSize    = fhdr.bfOffBits + (DWORD)(W * H * 4);
        DWORD wr;
        WriteFile(hf, &fhdr, sizeof(fhdr), &wr, nullptr);
        WriteFile(hf, &bi, sizeof(bi), &wr, nullptr);
        void* bits = malloc((size_t)W * H * 4);
        GetDIBits(mem, bmp, 0, (UINT)H, bits, &bi, DIB_RGB_COLORS);
        WriteFile(hf, bits, (DWORD)((size_t)W * H * 4), &wr, nullptr);
        CloseHandle(hf);
        free(bits);
        printf("wrote %s (%dx%d)\n", path, W, H);
    }

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

static BOOL CALLBACK EnumChild(HWND child, LPARAM lp) {
    (void)lp;
    char cls[64] = { 0 };
    GetClassNameA(child, cls, (int)sizeof(cls));
    RECT r;
    GetWindowRect(child, &r);
    MapWindowPoints(HWND_DESKTOP, GetParent(child), (POINT*)&r, 2);
    printf("   child %-16s id=%-5d rect=(%d,%d)-(%d,%d) vis=%d\n",
           cls, (int)GetDlgCtrlID(child), r.left, r.top, r.right, r.bottom,
           IsWindowVisible(child));
    return TRUE;
}

static DWORD WINAPI Shoot(LPVOID param) {
    const char* path = (const char*)param;
    Sleep(1500);
    g_target = FindWindowA("NintendallControllerConfig", NULL);
    if (g_target) {
        SetForegroundWindow(g_target);
        Sleep(400);
        RECT cr;
        GetClientRect(g_target, &cr);
        printf("client %dx%d\n", cr.right, cr.bottom);
        EnumChildWindows(g_target, EnumChild, 0);
        SaveBMP(path);
        PostMessageA(g_target, WM_CLOSE, 0, 0);
    } else {
        printf("dialog window not found\n");
    }
    return 0;
}

// Saves a mapping, wipes memory, loads it back and compares.  A format change
// that loses key sets or turbo bindings would otherwise silently drop every
// user's configuration on upgrade.
static int SelfTest() {
    const char* path = "input-selftest.cfg";
    InputSystem& in = InputSystem::get();

    in.apply_all_defaults(EMU_CONSOLE_SNES);
    PortMapping& m = in.mapping(0);
    // Give every key set something distinct, plus a turbo binding.
    for (int k = 0; k < INPUT_KEY_SETS; k++)
        m.set_by_id(EMU_ID_A, (uint32_t)(0x30 + k), k);
    m.set_turbo(EMU_ID_B, (uint32_t)'P');
    in.set_turbo_speed(7);

    if (!in.save(path)) { printf("FAIL: could not save\n"); return 1; }

    // Wipe, then read back.
    for (int p = 0; p < INPUT_MAX_PORTS; p++) in.mapping(p).clear();
    in.set_turbo_speed(1);
    if (!in.load(path)) { printf("FAIL: could not load\n"); return 1; }

    const PortMapping& r = in.mapping(0);
    int fails = 0;
    for (int k = 0; k < INPUT_KEY_SETS; k++) {
        int slot = r.find_by_id(EMU_ID_A);
        uint32_t want = (uint32_t)(0x30 + k);
        uint32_t got = (slot >= 0) ? r.codes[slot][k] : 0;
        if (got != want) { printf("FAIL: A keyset %d want %u got %u\n", k, want, got); fails++; }
    }
    int bs = r.find_by_id(EMU_ID_B);
    if (bs < 0 || r.turbo[bs] != (uint32_t)'P' || !r.turbo_on[bs]) {
        printf("FAIL: turbo binding lost\n"); fails++;
    }
    if (in.turbo_speed() != 7) { printf("FAIL: turbo speed %d\n", in.turbo_speed()); fails++; }

    // The SNES defaults are 12 buttons; the rest of the table is unused.
    for (int p = 0; p < INPUT_MAX_PORTS; p++) {
        int n = in.mapping(p).count();
        if (n != 12) { printf("FAIL: port %d has %d buttons, expected 12\n", p, n); fails++; }
    }

    remove(path);
    printf(fails ? "SELFTEST FAILED (%d)\n" : "selftest ok (%d failures)\n", fails);
    return fails ? 1 : 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "--test") == 0) return SelfTest();

    const char* out = (argc > 1) ? argv[1] : "dialog.bmp";
    // Console index: 0 NES, 1 SNES, 2 GB, 3 GBA, 4 N64
    EmuConsoleType con = EMU_CONSOLE_NES;
    int which = (argc > 2) ? atoi(argv[2]) : 0;
    switch (which) {
        case 1:  con = EMU_CONSOLE_SNES; break;
        case 2:  con = EMU_CONSOLE_GAMEBOY; break;
        case 3:  con = EMU_CONSOLE_GAMEBOY_ADVANCE; break;
        case 4:  con = EMU_CONSOLE_NINTENDO_64; break;
    }

    // Give the boxes something to show.
    InputSystem::get().apply_defaults(con, 0);

    CreateThread(NULL, 0, Shoot, (LPVOID)out, 0, NULL);
    ShowInputDialog(NULL, con);
    return 0;
}