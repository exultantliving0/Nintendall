// =============================================================================
//  Nintendall - controller artwork for the mapping dialog
//  See controller_layouts.h for the design.
// =============================================================================

#include "controller_layouts.h"

#include <algorithm>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

namespace {

// Box sizes in design units.  Mesen fixes these per control rather than sizing
// them to the text, which is what keeps a layout collision-free; only boxes
// left at zero are measured from their contents.
const int kBoxH    = 26;
const int kBoxDPad = 100;    // the four directional boxes are wider
const int kBox     = 76;     // everything else
const int kBoxSh   = 70;     // shoulder buttons

#define BTN(id, cx, cy, w, name, lx, ly, lpos, tdy) \
    { id, cx, cy, w, kBoxH, tdy, name, lx, ly, lpos }
#define DPAD(id, cx, cy) BTN(id, cx, cy, kBoxDPad, "", 0, 0, LBL_NONE, 0)
#define NOTURBO 0

// -----------------------------------------------------------------------------
// NES - the real controller
//
// Laid out from the controller's own artwork (the Wikimedia Commons
// "Nes_controller.svg"), so the binding boxes sit on the buttons where they
// actually are: the d-pad cross on the left, SELECT and START on the middle
// bar, and B and A as the two round buttons on the right.  Like the real pad,
// it is a pale shell around a dark face.
//
// The d-pad keeps all four directions.  The core reads UP, DOWN, LEFT and
// RIGHT separately and NES games need them, so dropping either axis would
// leave half of the d-pad dead.
// -----------------------------------------------------------------------------

const PadHousing kHousingsNes[] = {
    { 0,   8,   2, 596, 240 },   // the pale shell
    { 2,  16,  42, 574, 195 },   // the dark face
};

const PadButton kButtonsNes[] = {
    // The cross: one box on each arm, overlapping so the four read as one
    // plus shape the way the real d-pad does.
    BTN(EMU_ID_UP,    112, 124, 62, "", 0, 0, LBL_NONE, NOTURBO),
    BTN(EMU_ID_LEFT,   74, 149, 62, "", 0, 0, LBL_NONE, NOTURBO),
    BTN(EMU_ID_RIGHT, 150, 149, 62, "", 0, 0, LBL_NONE, NOTURBO),
    BTN(EMU_ID_DOWN,  112, 174, 62, "", 0, 0, LBL_NONE, NOTURBO),

    // The two round buttons, B left of A as on the real controller.  Names go
    // below the shell, since black text would vanish into the dark face.
    BTN(EMU_ID_B, 458, 178, kBox, "B", 458, 260, LBL_BELOW, NOTURBO),
    BTN(EMU_ID_A, 537, 178, kBox, "A", 537, 260, LBL_BELOW, NOTURBO),

    // SELECT and START, on the middle bar.
    BTN(EMU_ID_SELECT, 240, 188, kBox, "Select", 240, 260, LBL_BELOW, NOTURBO),
    BTN(EMU_ID_START,  324, 188, kBox, "Start",  324, 260, LBL_BELOW, NOTURBO),
};
const PadLayout kPadNes = {
    "NES", 612, 272,
    kHousingsNes, (int)(sizeof(kHousingsNes) / sizeof(kHousingsNes[0])),
    kButtonsNes,  (int)(sizeof(kButtonsNes)  / sizeof(kButtonsNes[0])),
    0   // no turbo
};

// -----------------------------------------------------------------------------
// SNES - L and R on the shoulders, a four-way face cluster to the right
// -----------------------------------------------------------------------------

const PadHousing kHousingsSnes[] = {
    { 1,  14,  40, 244, 170 },   // d-pad ring
    { 1, 300,  40, 290, 230 },   // face-button ring
};

const PadButton kButtonsSnes[] = {
    BTN(EMU_ID_UP,   131,  66, kBoxDPad, "",   0,   0, LBL_NONE,  NOTURBO),
    BTN(EMU_ID_LEFT,  56, 125, kBoxDPad, "",   0,   0, LBL_NONE,  NOTURBO),
    BTN(EMU_ID_RIGHT,200, 125, kBoxDPad, "",   0,   0, LBL_NONE,  NOTURBO),
    BTN(EMU_ID_DOWN, 131, 184, kBoxDPad, "",   0,   0, LBL_NONE,  NOTURBO),

    BTN(EMU_ID_L,  86, 16, kBox, "L",  86,  42, LBL_BELOW, NOTURBO),
    BTN(EMU_ID_R, 490, 16, kBox, "R", 490,  42, LBL_BELOW, NOTURBO),

    // A four-way cluster: X on top, Y left, A right, B below, each with its
    // turbo box tucked underneath.
    BTN(EMU_ID_X, 445,  78, kBox, "X", 445,  52, LBL_ABOVE, 36),
    BTN(EMU_ID_Y, 366, 110, kBox, "Y", 366,  84, LBL_ABOVE, 36),
    BTN(EMU_ID_A, 524, 110, kBox, "A", 524,  84, LBL_ABOVE, 36),
    BTN(EMU_ID_B, 445, 218, kBox, "B", 445, 192, LBL_ABOVE, 36),

    BTN(EMU_ID_SELECT, 244, 240, kBox, "Select", 244, 266, LBL_BELOW, NOTURBO),
    BTN(EMU_ID_START,  318, 240, kBox, "Start",  318, 266, LBL_BELOW, NOTURBO),
};
const PadLayout kPadSnes = {
    "SNES", 585, 300,
    kHousingsSnes, (int)(sizeof(kHousingsSnes) / sizeof(kHousingsSnes[0])),
    kButtonsSnes,  (int)(sizeof(kButtonsSnes)  / sizeof(kButtonsSnes[0])),
    1   // turbo
};

// -----------------------------------------------------------------------------
// Game Boy / Game Boy Color - A and B sit on a diagonal, as on the real pad
// -----------------------------------------------------------------------------

const PadHousing kHousingsGb[] = {
    { 1,  12,  16, 216, 152 },
    { 0, 372,  30, 200, 130 },
};

const PadButton kButtonsGb[] = {
    DPAD(EMU_ID_UP,    120,  38),
    DPAD(EMU_ID_LEFT,   44,  92),
    DPAD(EMU_ID_RIGHT, 196,  92),
    DPAD(EMU_ID_DOWN,  120, 146),

    BTN(EMU_ID_B, 416,  72, kBox, "B", 416,  46, LBL_ABOVE, NOTURBO),
    BTN(EMU_ID_A, 506, 122, kBox, "A", 506, 148, LBL_BELOW, NOTURBO),

    BTN(EMU_ID_SELECT, 250, 182, kBox, "Select", 250, 206, LBL_BELOW, NOTURBO),
    BTN(EMU_ID_START,  330, 182, kBox, "Start",  330, 206, LBL_BELOW, NOTURBO),
};
const PadLayout kPadGb = {
    "Game Boy", 585, 220,
    kHousingsGb, (int)(sizeof(kHousingsGb) / sizeof(kHousingsGb[0])),
    kButtonsGb,  (int)(sizeof(kButtonsGb)  / sizeof(kButtonsGb[0])),
    0   // no turbo
};

// -----------------------------------------------------------------------------
// Game Boy Advance - wide and flat, with L and R across the top
// -----------------------------------------------------------------------------

const PadHousing kHousingsGba[] = {
    { 1,  12,  30, 200, 160 },
    { 1, 372,  30, 202, 160 },
};

const PadButton kButtonsGba[] = {
    DPAD(EMU_ID_UP,   107,  60),
    BTN(EMU_ID_LEFT,   62, 110, 96, "", 0, 0, LBL_NONE, NOTURBO),
    BTN(EMU_ID_RIGHT, 162, 110, 96, "", 0, 0, LBL_NONE, NOTURBO),
    DPAD(EMU_ID_DOWN, 107, 160),

    BTN(EMU_ID_L, 240, 14, kBoxSh, "L", 240, 38, LBL_BELOW, NOTURBO),
    BTN(EMU_ID_R, 470, 14, kBoxSh, "R", 470, 38, LBL_BELOW, NOTURBO),

    BTN(EMU_ID_A, 437,  80, kBox, "A", 437,  54, LBL_ABOVE, NOTURBO),
    BTN(EMU_ID_B, 519, 140, kBox, "B", 519, 166, LBL_BELOW, NOTURBO),

    BTN(EMU_ID_SELECT, 252, 184, kBox, "Select", 252, 208, LBL_BELOW, NOTURBO),
    BTN(EMU_ID_START,  330, 184, kBox, "Start",  330, 208, LBL_BELOW, NOTURBO),
};
const PadLayout kPadGba = {
    "Game Boy Advance", 585, 220,
    kHousingsGba, (int)(sizeof(kHousingsGba) / sizeof(kHousingsGba[0])),
    kButtonsGba,  (int)(sizeof(kButtonsGba)  / sizeof(kButtonsGba[0])),
    0   // no turbo
};

// -----------------------------------------------------------------------------
// Nintendo 64
//
// Mesen has no N64 pad, so this follows its visual language rather than
// copying anything: the same pale housings and fixed-width binding boxes,
// arranged the way the real controller is - L on the left shoulder, R on the
// right, the C buttons beside A and B, Start and Select down the middle.
//
// Only the twelve inputs the core reads are offered.  X and Y are its two C
// buttons, so they are labelled C-Up and C-Down.
// -----------------------------------------------------------------------------

const PadHousing kHousingsN64[] = {
    { 1,  14,  44, 200, 150 },   // d-pad ring
    { 1, 330,  46, 242, 148 },   // face-button ring
};

const PadButton kButtonsN64[] = {
    BTN(EMU_ID_UP,   114,  70, 96, "", 0, 0, LBL_NONE, NOTURBO),
    BTN(EMU_ID_LEFT,  64, 119, 96, "", 0, 0, LBL_NONE, NOTURBO),
    BTN(EMU_ID_RIGHT,164, 119, 96, "", 0, 0, LBL_NONE, NOTURBO),
    BTN(EMU_ID_DOWN, 114, 168, 96, "", 0, 0, LBL_NONE, NOTURBO),

    BTN(EMU_ID_L, 160, 18, kBoxSh, "L", 160,  44, LBL_BELOW, NOTURBO),
    BTN(EMU_ID_R, 520, 18, kBoxSh, "R", 520,  44, LBL_BELOW, NOTURBO),

    // The two C buttons sit to the left of A and B, as on the real pad.
    BTN(EMU_ID_X, 380,  92, kBox, "C-Up",   380,  66, LBL_ABOVE, NOTURBO),
    BTN(EMU_ID_Y, 380, 150, kBox, "C-Down", 380, 176, LBL_BELOW, NOTURBO),

    BTN(EMU_ID_A, 486,  92, kBox, "A", 486,  66, LBL_ABOVE, NOTURBO),
    BTN(EMU_ID_B, 486, 150, kBox, "B", 486, 176, LBL_BELOW, NOTURBO),

    BTN(EMU_ID_START,  272, 102, kBox, "Start",  272, 128, LBL_BELOW, NOTURBO),
    BTN(EMU_ID_SELECT, 272, 152, kBox, "Select", 272, 178, LBL_BELOW, NOTURBO),
};
const PadLayout kPadN64 = {
    "Nintendo 64", 585, 220,
    kHousingsN64, (int)(sizeof(kHousingsN64) / sizeof(kHousingsN64[0])),
    kButtonsN64,  (int)(sizeof(kButtonsN64)  / sizeof(kButtonsN64[0])),
    0   // no turbo
};

#undef BTN
#undef DPAD
#undef NOTURBO

// --- Mesen's screen colours --------------------------------------------------

const PadColors kColors = {
    /*panel*/         RGB(255, 255, 255),
    /*housing_fill*/  RGB(240, 240, 240),
    /*housing_line*/  RGB(192, 192, 192),
    /*box_fill*/      RGB(240, 240, 240),
    /*box_line*/      RGB(160, 160, 160),
    /*box_text*/      RGB( 48,  48,  48),
    /*label_text*/    RGB(  0,   0,   0),
    /*down_fill*/     RGB(183, 225, 161),
    /*down_line*/     RGB( 90, 150,  70),
    /*listen_fill*/   RGB(255, 241, 189),
    /*listen_line*/   RGB(214, 154,  42),
    /*dark_fill*/     RGB( 40,  40,  48),   // a real NES pad's face
    /*dark_line*/     RGB( 24,  24,  30),
};

void DrawRect(HDC dc, const RECT& r, COLORREF fill, COLORREF line, int lw) {
    HPEN  pen  = CreatePen(PS_SOLID, lw, line);
    HBRUSH br  = CreateSolidBrush(fill);
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, br);
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

}  // namespace

const PadColors* pad_colors() { return &kColors; }

const PadLayout* pad_layout_for(EmuConsoleType console) {
    switch (console) {
        case EMU_CONSOLE_NES:             return &kPadNes;
        case EMU_CONSOLE_SNES:            return &kPadSnes;
        case EMU_CONSOLE_GAMEBOY:         return &kPadGb;
        case EMU_CONSOLE_GAMEBOY_COLOR:   return &kPadGb;
        case EMU_CONSOLE_GAMEBOY_ADVANCE: return &kPadGba;
        case EMU_CONSOLE_NINTENDO_64:     return &kPadN64;
        default:                          return &kPadNes;
    }
}

int pad_box_width(HDC dc, HFONT font, const char* text, int min_w) {
    if (!text || !*text) return min_w;
    HGDIOBJ of = SelectObject(dc, font);
    SIZE sz;
    sz.cx = sz.cy = 0;
    GetTextExtentPoint32A(dc, text, (int)strlen(text), &sz);
    SelectObject(dc, of);
    int w = sz.cx + 20;            // breathing room either side
    return w < min_w ? min_w : w;
}

// A button's width: its declared width, or measured when it has none.
static int WidthOf(const PadButton& b, HDC dc, HFONT font, const char* text) {
    if (b.w > 0) return b.w;
    return pad_box_width(dc, font, text, kBox);
}

void pad_box_rect(const PadLayout* pad, int index, int width_design,
                  int ox, int oy, float scale, int dy, RECT* out) {
    const PadButton& b = pad->buttons[index];
    int w = (int)(width_design * scale);
    int h = (int)(b.h * scale);
    int cx = ox + (int)(b.cx * scale);
    int cy = oy + (int)((b.cy + dy) * scale);
    out->left   = cx - w / 2;
    out->top    = cy - h / 2;
    out->right  = out->left + w;
    out->bottom = out->top + h;
}

const PadButton* pad_hit_test(const PadLayout* pad, const int* widths, const int* turbo_widths,
                              float px, float py, int ox, int oy, float scale) {
    if (!pad || scale <= 0) return nullptr;
    float dx = (px - ox) / scale;
    float dy = (py - oy) / scale;

    for (int i = 0; i < pad->button_count; i++) {
        const PadButton& b = pad->buttons[i];
        int w = widths ? widths[i] : b.w;
        float hw = w / 2.0f;
        if (dx >= b.cx - hw && dx <= b.cx + hw &&
            dy >= b.cy - b.h / 2.0f && dy <= b.cy + b.h / 2.0f)
            return &pad->buttons[i];

        if (b.turbo_dy && turbo_widths) {
            int tw = turbo_widths[i];
            float thw = tw / 2.0f;
            float tcy = b.cy + b.turbo_dy;
            if (dx >= b.cx - thw && dx <= b.cx + thw &&
                dy >= tcy - b.h / 2.0f && dy <= tcy + b.h / 2.0f)
                return &pad->buttons[i];
        }
    }
    return nullptr;
}

void pad_paint_state(HDC dc, HFONT box_font, const PadLayout* pad,
                     int ox, int oy, float scale,
                     PadPressedFn pressed, void* ctx,
                     const char* const* texts, const char* const* turbo_texts,
                     int listening_index, int turbo_listening_index) {
    if (!pad) return;
    const PadColors& c = kColors;
    SetBkMode(dc, TRANSPARENT);

    // --- housings ------------------------------------------------------------
    // Kinds 0/1 are Mesen's pale grey; 2/3 are the dark grey of a real NES pad.
    {
        for (int i = 0; i < pad->housing_count; i++) {
            const PadHousing& hs = pad->housings[i];
            const bool dark = (hs.kind >= 2);
            HPEN  pen = CreatePen(PS_SOLID, 1, dark ? c.dark_line : c.housing_line);
            HBRUSH br = CreateSolidBrush(dark ? c.dark_fill : c.housing_fill);
            HGDIOBJ op = SelectObject(dc, pen);
            HGDIOBJ ob = SelectObject(dc, br);
            int x = ox + (int)(hs.x * scale);
            int y = oy + (int)(hs.y * scale);
            int w = (int)(hs.w * scale);
            int h = (int)(hs.h * scale);
            if (hs.kind == 1 || hs.kind == 3) Ellipse(dc, x, y, x + w, y + h);
            else                            RoundRect(dc, x, y, x + w, y + h, 12 * scale, 12 * scale);
            SelectObject(dc, ob);
            SelectObject(dc, op);
            DeleteObject(br);
            DeleteObject(pen);
        }
    }

    // --- binding boxes -------------------------------------------------------
    for (int i = 0; i < pad->button_count; i++) {
        const PadButton& b = pad->buttons[i];
        const char* txt  = (texts && texts[i]) ? texts[i] : "";
        const char* ttxt = (turbo_texts && turbo_texts[i]) ? turbo_texts[i] : nullptr;

        bool held      = pressed && pressed(ctx, b.id);
        bool listening = (i == listening_index);
        bool tlisten   = (i == turbo_listening_index);

        int w = WidthOf(b, dc, box_font, txt);
        RECT r;
        pad_box_rect(pad, i, w, ox, oy, scale, 0, &r);

        COLORREF fill = c.box_fill, line = c.box_line;
        if (listening) { fill = c.listen_fill; line = c.listen_line; }
        else if (held) { fill = c.down_fill;   line = c.down_line;   }
        DrawRect(dc, r, fill, line, listening ? 2 : 1);

        HGDIOBJ of = SelectObject(dc, box_font);
        SetTextColor(dc, c.box_text);
        DrawTextA(dc, txt, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, of);

        if (b.turbo_dy && ttxt) {
            int tw = WidthOf(b, dc, box_font, ttxt);
            RECT tr;
            pad_box_rect(pad, i, tw, ox, oy, scale, b.turbo_dy, &tr);
            COLORREF tf = tlisten ? c.listen_fill : c.box_fill;
            COLORREF tl = tlisten ? c.listen_line : c.box_line;
            DrawRect(dc, tr, tf, tl, tlisten ? 2 : 1);
            of = SelectObject(dc, box_font);
            SetTextColor(dc, c.box_text);
            DrawTextA(dc, ttxt, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dc, of);
        }
    }

    // --- names ---------------------------------------------------------------
    {
        HFONT lf = CreateFontA(-(int)(15 * scale), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        HGDIOBJ of = SelectObject(dc, lf);
        SetTextColor(dc, c.label_text);
        for (int i = 0; i < pad->button_count; i++) {
            const PadButton& b = pad->buttons[i];
            if (!b.name || !*b.name) continue;
            int pad_ = (int)(56 * scale);

            if (b.lpos != LBL_NONE) {
                RECT t;
                int cx = ox + (int)(b.lx * scale);
                int cy = oy + (int)(b.ly * scale);
                t.left = cx - pad_; t.right = cx + pad_;
                t.top = cy - (int)(12 * scale); t.bottom = cy + (int)(12 * scale);
                DrawTextA(dc, b.name, -1, &t,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }

            // The caption under a turbo box.  Only for pads that have turbo at
            // all - otherwise NES and N64 keep stray "Turbo A" text.
            if (pad->has_turbo && b.turbo_dy) {
                char cap[48];
                snprintf(cap, sizeof(cap), "Turbo %s", b.name);
                RECT t;
                int cx = ox + (int)(b.cx * scale);
                int cy = oy + (int)((b.cy + b.turbo_dy + b.h / 2 + 4) * scale);
                t.left = cx - pad_; t.right = cx + pad_;
                t.top = cy - (int)(11 * scale); t.bottom = cy + (int)(11 * scale);
                DrawTextA(dc, cap, -1, &t,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
        }
        SelectObject(dc, of);
        DeleteObject(lf);
    }
}