// Developer tool: renders the controller layouts the way the mapping dialog
// paints them, so the artwork can be eyeballed without opening the dialog.
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "controller_layouts.h"
#include "emu_api.h"

static const EmuConsoleType kOrder[] = {
    EMU_CONSOLE_NES, EMU_CONSOLE_SNES,
    EMU_CONSOLE_GAMEBOY, EMU_CONSOLE_GAMEBOY_ADVANCE,
    EMU_CONSOLE_NINTENDO_64
};

// Plausible bindings, so the boxes are a realistic width.
static const char* TextFor(EmuInputId id) {
    switch (id) {
        case EMU_ID_UP:     return "Up";
        case EMU_ID_DOWN:   return "Down";
        case EMU_ID_LEFT:   return "Left";
        case EMU_ID_RIGHT:  return "Right";
        case EMU_ID_A:      return "X";
        case EMU_ID_B:      return "Z";
        case EMU_ID_X:      return "Joy1 Y";
        case EMU_ID_Y:      return "Joy1 X";
        case EMU_ID_L:      return "Q";
        case EMU_ID_R:      return "W";
        case EMU_ID_SELECT: return "Joy1 Button9";
        case EMU_ID_START:  return "Joy1 Button10";
    }
    return "";
}

static const char* turboText(const char* name) {
    if (!name || !*name) return "";
    if (strncmp(name, "Turbo", 5) == 0) return "";
    char buf[32];
    snprintf(buf, sizeof(buf), "Turbo %s", name);
    return buf;
}

int main(int argc, char** argv) {
    const char* out = (argc > 1) ? argv[1] : "controllers.bmp";
    const int  which = (argc > 2) ? atoi(argv[2]) : -1;   // single pad if >= 0

    HFONT boxFont = CreateFontA(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

    HDC screen = GetDC(nullptr);

    // Lay out the requested pads into a grid.
    const int count = (int)(sizeof(kOrder) / sizeof(kOrder[0]));
    int n = 0;
    const PadLayout* shown[8];
    for (int i = 0; i < count; i++) {
        if (which >= 0 && i != which) continue;
        shown[n++] = pad_layout_for(kOrder[i]);
    }

    const int panel_w = 820, panel_h = 380, cols = 2;
    const int rows = (n + cols - 1) / cols;
    const int W = panel_w * cols, H = panel_h * rows;

    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, W, H);
    HGDIOBJ old = SelectObject(mem, bmp);

    HBRUSH page = CreateSolidBrush(RGB(250, 250, 252));
    RECT full = { 0, 0, W, H };
    FillRect(mem, &full, page);
    DeleteObject(page);

    for (int i = 0; i < n; i++) {
        const PadLayout* pad = shown[i];
        int px = (i % cols) * panel_w + 12;
        int py = (i / cols) * panel_h + 12;
        int avail_w = panel_w - 24, avail_h = panel_h - 24;

        // White panel with the gray frame the dialog draws.
        RECT panel = { px, py, px + avail_w, py + avail_h };
        HBRUSH wb = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(mem, &panel, wb);
        DeleteObject(wb);
        FrameRect(mem, &panel, (HBRUSH)GetStockObject(GRAY_BRUSH));

        float sx = (float)(avail_w - 20) / pad->design_w;
        float sy = (float)(avail_h - 20) / pad->design_h;
        float s = sx < sy ? sx : sy;
        int ox = px + 10 + (int)((avail_w - 20) - pad->design_w * s) / 2;
        int oy = py + 10 + (int)((avail_h - 20) - pad->design_h * s) / 2;

        const int MAXB = 24;
        const char* texts[MAXB] = { 0 };
        const char* ttexts[MAXB] = { 0 };
        for (int b = 0; b < pad->button_count; b++) {
            texts[b] = TextFor(pad->buttons[b].id);
            if (pad->has_turbo && pad->buttons[b].turbo_dy) {
                ttexts[b] = turboText(pad->buttons[b].name);
            }
        }
        pad_paint_state(mem, boxFont, pad, ox, oy, s, nullptr, nullptr,
                        texts, ttexts, -1, -1);
    }

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = W;
    bi.bmiHeader.biHeight      = -H;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HANDLE hf = CreateFileA(out, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
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

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    printf("wrote %s (%dx%d)\n", out, W, H);
    return 0;
}