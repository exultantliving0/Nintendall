// =============================================================================
//  Nintendall - the "Controller Configuration" window
//  See input_dialog.h.
// =============================================================================

#include "input_dialog.h"

#include <windowsx.h>
#include <stdio.h>
#include <string.h>

#include "controller_layouts.h"
#include "input_config.h"

namespace {

// The accent Mesen uses for its slider and its "listening" highlight.
const COLORREF kAccent     = RGB(0, 120, 212);
const COLORREF kBanner     = RGB(232, 240, 250);
const COLORREF kPage       = RGB(250, 250, 252);
const COLORREF kTabActive  = RGB(32, 32, 32);
const COLORREF kTabIdle    = RGB(150, 150, 155);

enum {
    IDC_PLAYER    = 101,
    IDC_PRESET    = 102,
    IDC_CLEAR     = 103,
    IDC_OK        = 104,
    IDC_CANCEL    = 105,
    IDC_LBL_PORT  = 107,
    IDC_TIMER     = 1,
};

// Vertical bands of the window, in client coordinates.
const int kBannerH   = 32;
const int kTabGap    = 6;
const int kTabStripH = 26;
const int kFoot1H    = 48;    // port / preset / turbo slider
const int kFoot2H    = 40;    // clear bindings / OK / Cancel
const int kPadInset  = 10;
const int kPad       = 12;

// One entry of the hand-drawn tab strip.
struct TabRect { RECT r; };

struct DialogState {
    HWND             hwnd;
    EmuConsoleType   console;
    const PadLayout* pad;
    unsigned         port;
    int              key_set;
    int              listening;        // button index, -1 when idle
    bool             listening_turbo;
    bool             accepted;

    RECT  panel;
    float scale;
    int   pad_x, pad_y;

    // Key Set tabs
    TabRect tabs[INPUT_KEY_SETS];

    // Turbo slider, in client coordinates
    RECT   slider;
    RECT   slider_label;
    bool   show_slider;

    int          widths[INPUT_MAX_BUTTONS];
    int          turbo_widths[INPUT_MAX_BUTTONS];
    const char*  texts[INPUT_MAX_BUTTONS];
    const char*  turbo_texts[INPUT_MAX_BUTTONS];
    char         text_buf[INPUT_MAX_BUTTONS][32];
    char         turbo_buf[INPUT_MAX_BUTTONS][32];
};

DialogState g_dlg;
HFONT g_box_font = nullptr;
HFONT g_ui_font  = nullptr;
HFONT g_big_font = nullptr;

void LayoutDialog();
void Refresh();

int PressedThunk(void* ctx, EmuInputId id) {
    (void)ctx;
    return InputSystem::get().is_pressed(g_dlg.port, id);
}

// Fills texts[] / turbo_texts[] for the current port, key set and pad.
void BuildTexts() {
    DialogState& d = g_dlg;
    const PortMapping& m = InputSystem::get().mapping(d.port);
    const int want_turbo = d.pad && d.pad->has_turbo;

    for (int i = 0; i < INPUT_MAX_BUTTONS; i++) {
        d.texts[i] = nullptr;
        d.turbo_texts[i] = nullptr;
        if (!d.pad || i >= d.pad->button_count) continue;

        const PadButton& b = d.pad->buttons[i];
        int slot = m.find_by_id(b.id);
        if (slot >= 0 && m.codes[slot][d.key_set] != INPUT_NONE) {
            InputSystem::describe(m.codes[slot][d.key_set], d.text_buf[i], sizeof(d.text_buf[i]));
            d.texts[i] = d.text_buf[i];
        }
        if (want_turbo && b.turbo_dy) {
            if (slot >= 0 && m.turbo_on[slot] && m.turbo[slot] != INPUT_NONE) {
                InputSystem::describe(m.turbo[slot], d.turbo_buf[i], sizeof(d.turbo_buf[i]));
                d.turbo_texts[i] = d.turbo_buf[i];
            } else {
                d.turbo_texts[i] = "";      // show the empty slot Mesen shows
            }
        }
    }
}

void MeasureBoxes(HDC dc) {
    DialogState& d = g_dlg;
    if (!d.pad) return;
    for (int i = 0; i < d.pad->button_count; i++) {
        const PadButton& b = d.pad->buttons[i];
        // Boxes have a fixed width in the layout; only measure the rare one
        // that does not, so hit testing agrees with what was painted.
        d.widths[i] = b.w;
        if (!d.widths[i])
            d.widths[i] = pad_box_width(dc, g_box_font, d.texts[i] ? d.texts[i] : "", 76);
        d.turbo_widths[i] = (b.turbo_dy && d.turbo_texts[i]) ? d.widths[i] : 0;
    }
}

void DrawFrame(HDC dc, const RECT& r, COLORREF line) {
    HPEN pen = CreatePen(PS_SOLID, 1, line);
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
}

// --- the info banner ---------------------------------------------------------
void PaintBanner(HDC dc, const RECT& rc) {
    HBRUSH br = CreateSolidBrush(kBanner);
    FillRect(dc, &rc, br);
    DeleteObject(br);

    const int d_ = 18;
    int cy = (rc.top + rc.bottom) / 2;
    HBRUSH ab = CreateSolidBrush(kAccent);
    HGDIOBJ ob = SelectObject(dc, ab);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, rc.left + 9, cy - d_ / 2, rc.left + 9 + d_, cy + d_ / 2);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(ab);

    HGDIOBJ of = SelectObject(dc, g_big_font);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    RECT qr = { rc.left + 9, cy - 11, rc.left + 9 + d_, cy + 11 };
    DrawTextA(dc, "?", -1, &qr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, of);

    of = SelectObject(dc, g_ui_font);
    SetTextColor(dc, RGB(32, 32, 32));
    RECT tr = { rc.left + 34, rc.top, rc.right - 8, rc.bottom };
    DrawTextA(dc,
              "Each button can be mapped to up to 4 different keyboard keys or gamepad buttons.",
              -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, of);
}

// --- the "Key Set #n" strip --------------------------------------------------
// Mesen draws these as plain text with an underline on the active one, so they
// are painted here rather than using a native tab control.
void PaintTabs(HDC dc) {
    DialogState& d = g_dlg;
    HGDIOBJ of = SelectObject(dc, g_ui_font);
    SetBkMode(dc, TRANSPARENT);

    const int top = kBannerH + kTabGap;
    int x = kPad + 4;

    for (int i = 0; i < INPUT_KEY_SETS; i++) {
        char lbl[32];
        snprintf(lbl, sizeof(lbl), "Key Set #%d", i + 1);

        SIZE sz;
        sz.cx = sz.cy = 0;
        GetTextExtentPoint32A(dc, lbl, (int)strlen(lbl), &sz);

        RECT t = { x, top, x + sz.cx, top + sz.cy };
        d.tabs[i].r = t;
        bool active = (i == d.key_set);
        SetTextColor(dc, active ? kTabActive : kTabIdle);
        DrawTextA(dc, lbl, -1, &t, DT_LEFT | DT_TOP | DT_SINGLELINE);

        if (active) {
            HPEN pen = CreatePen(PS_SOLID, 2, kAccent);
            HGDIOBJ op = SelectObject(dc, pen);
            MoveToEx(dc, t.left, t.bottom + 3, nullptr);
            LineTo(dc, t.right, t.bottom + 3);
            SelectObject(dc, op);
            DeleteObject(pen);
        }
        x = t.right + 26;
    }
    SelectObject(dc, of);
}

// --- the turbo slider, drawn to match Mesen's -------------------------------
void PaintSlider(HDC dc) {
    DialogState& d = g_dlg;
    if (!d.show_slider) return;

    HGDIOBJ of = SelectObject(dc, g_ui_font);
    SetBkMode(dc, TRANSPARENT);

    // "Turbo Speed:", right-aligned just left of the track
    SetTextColor(dc, RGB(32, 32, 32));
    DrawTextA(dc, "Turbo Speed:", -1, &d.slider_label,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    const int track_y = (d.slider.top + d.slider.bottom) / 2;
    const int x0 = d.slider.left + 9;
    const int x1 = d.slider.right - 9;

    int speed = InputSystem::get().turbo_speed();
    if (speed < 1) speed = 1;
    if (speed > 9) speed = 9;
    int thumb = x0 + (int)((long)(x1 - x0) * (speed - 1) / 8);

    HPEN lp = CreatePen(PS_SOLID, 2, RGB(200, 200, 205));
    HGDIOBJ op = SelectObject(dc, lp);
    MoveToEx(dc, x0, track_y, nullptr);
    LineTo(dc, x1, track_y);
    SelectObject(dc, op);
    DeleteObject(lp);

    HPEN ap = CreatePen(PS_SOLID, 2, kAccent);
    op = SelectObject(dc, ap);
    MoveToEx(dc, x0, track_y, nullptr);
    LineTo(dc, thumb, track_y);
    SelectObject(dc, op);
    DeleteObject(ap);

    HBRUSH ab = CreateSolidBrush(kAccent);
    HGDIOBJ ob = SelectObject(dc, ab);
    HGDIOBJ on = SelectObject(dc, GetStockObject(NULL_PEN));
    const int r = 7;
    Ellipse(dc, thumb - r, track_y - r, thumb + r, track_y + r);
    SelectObject(dc, on);
    SelectObject(dc, ob);
    DeleteObject(ab);

    // Slow / Fast captions under the ends of the track
    RECT slow = { d.slider.left, track_y + 10, d.slider.left + 46, track_y + 26 };
    RECT fast = { d.slider.right - 46, track_y + 10, d.slider.right, track_y + 26 };
    SetTextColor(dc, RGB(90, 90, 95));
    DrawTextA(dc, "Slow", -1, &slow, DT_LEFT | DT_TOP | DT_SINGLELINE);
    DrawTextA(dc, "Fast", -1, &fast, DT_RIGHT | DT_TOP | DT_SINGLELINE);

    SelectObject(dc, of);
}

void PaintEverything(HDC dc) {
    DialogState& d = g_dlg;
    RECT rc;
    GetClientRect(d.hwnd, &rc);

    HBRUSH pg = CreateSolidBrush(kPage);
    FillRect(dc, &rc, pg);
    DeleteObject(pg);

    RECT banner = { 0, 0, rc.right, kBannerH };
    PaintBanner(dc, banner);
    PaintTabs(dc);

    // The white pad panel, framed like Mesen's.
    HBRUSH wb = CreateSolidBrush(pad_colors()->panel);
    FillRect(dc, &d.panel, wb);
    DeleteObject(wb);
    DrawFrame(dc, d.panel, RGB(190, 190, 196));

    if (d.pad) {
        BuildTexts();
        MeasureBoxes(dc);
        pad_paint_state(dc, g_box_font, d.pad, d.pad_x, d.pad_y, d.scale,
                        PressedThunk, nullptr, d.texts, d.turbo_texts,
                        d.listening, d.listening_turbo ? d.listening : -1);
    }

    PaintSlider(dc);
}

void LayoutDialog() {
    DialogState& d = g_dlg;
    RECT rc;
    GetClientRect(d.hwnd, &rc);
    const int W = rc.right, H = rc.bottom;

    d.panel.left   = kPad;
    d.panel.right  = W - kPad;
    d.panel.top    = kBannerH + kTabGap + kTabStripH + 8;
    d.panel.bottom = H - kFoot1H - kFoot2H - 8;

    // Fit the pad inside the panel, keeping its proportions.
    const int avail_w = (d.panel.right - d.panel.left) - kPadInset * 2;
    const int avail_h = (d.panel.bottom - d.panel.top) - kPadInset * 2;
    float sx = d.pad ? (float)avail_w / d.pad->design_w : 1.0f;
    float sy = d.pad ? (float)avail_h / d.pad->design_h : 1.0f;
    d.scale = sx < sy ? sx : sy;
    if (d.scale < 0.2f) d.scale = 0.2f;

    int pw = (int)(d.pad ? d.pad->design_w * d.scale : 0);
    int ph = (int)(d.pad ? d.pad->design_h * d.scale : 0);
    d.pad_x = d.panel.left + kPadInset + (avail_w - pw) / 2;
    d.pad_y = d.panel.top  + kPadInset + (avail_h - ph) / 2;

    // Footer, first row: port selector and preset on the left, turbo slider
    // on the right - laid out the way Mesen does it.
    const int y1 = H - kFoot1H - kFoot2H + 6;
    int x = kPad;
    MoveWindow(GetDlgItem(d.hwnd, IDC_LBL_PORT), x, y1 + 6, 42, 20, TRUE);
    x += 46;
    MoveWindow(GetDlgItem(d.hwnd, IDC_PLAYER), x, y1 + 3, 100, 24, TRUE);
    x += 110;
    MoveWindow(GetDlgItem(d.hwnd, IDC_PRESET), x, y1 + 3, 116, 24, TRUE);

    d.show_slider = d.pad && d.pad->has_turbo;
    if (d.show_slider) {
        d.slider.left   = W - kPad - 190;
        d.slider.right  = W - kPad;
        d.slider.top    = y1 + 2;
        d.slider.bottom = y1 + 26;
        d.slider_label.left   = d.slider.left - 124;
        d.slider_label.right  = d.slider.left - 10;
        d.slider_label.top    = y1 + 3;
        d.slider_label.bottom = y1 + 27;
    }

    // Footer, second row.
    const int y2 = H - kFoot2H + 6;
    MoveWindow(GetDlgItem(d.hwnd, IDC_CLEAR), kPad, y2, 140, 26, TRUE);

    const int bw = 84, gap = 8;
    int bx = W - kPad - bw * 2 - gap;
    MoveWindow(GetDlgItem(d.hwnd, IDC_CANCEL), bx, y2, bw, 26, TRUE);
    bx += bw + gap;
    MoveWindow(GetDlgItem(d.hwnd, IDC_OK), bx, y2, bw, 26, TRUE);
}

void Refresh() {
    BuildTexts();
    InvalidateRect(g_dlg.hwnd, nullptr, FALSE);
}

void CancelListening() {
    g_dlg.listening = -1;
    g_dlg.listening_turbo = false;
    InputSystem::get().cancel_capture();
}

// Maps a click on the slider track to a turbo speed of 1..9.
void SliderClicked(int mx) {
    DialogState& d = g_dlg;
    if (!d.show_slider) return;
    const int x0 = d.slider.left + 9;
    const int x1 = d.slider.right - 9;
    if (x1 <= x0) return;
    int speed = 1 + (int)((long)(mx - x0) * 8 / (x1 - x0));
    if (speed < 1) speed = 1;
    if (speed > 9) speed = 9;
    InputSystem::get().set_turbo_speed(speed);
    InvalidateRect(d.hwnd, nullptr, FALSE);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    DialogState& d = g_dlg;

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        d.hwnd = hwnd;

        g_box_font = CreateFontA(-11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        g_ui_font = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        g_big_font = CreateFontA(-13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");

        CreateWindowExW(0, L"STATIC", L"Port", WS_CHILD | WS_VISIBLE | SS_LEFT,
                        0, 0, 0, 0, hwnd, (HMENU)IDC_LBL_PORT, cs->hInstance, nullptr);

        HWND combo = CreateWindowExW(0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
            0, 0, 0, 0, hwnd, (HMENU)IDC_PLAYER, cs->hInstance, nullptr);
        for (int i = 0; i < INPUT_MAX_PORTS; i++) {
            char lbl[32];
            snprintf(lbl, sizeof(lbl), "Player %d", i + 1);
            SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)lbl);
        }
        SendMessageW(combo, CB_SETCURSEL, 0, 0);

        HWND bPreset = CreateWindowExW(0, L"BUTTON", L"Select Preset...",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0,
            hwnd, (HMENU)IDC_PRESET, cs->hInstance, nullptr);
        HWND bClear = CreateWindowExW(0, L"BUTTON", L"Clear Key Bindings",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0,
            hwnd, (HMENU)IDC_CLEAR, cs->hInstance, nullptr);
        HWND bOk = CreateWindowExW(0, L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 0, 0,
            hwnd, (HMENU)IDC_OK, cs->hInstance, nullptr);
        HWND bCancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0,
            hwnd, (HMENU)IDC_CANCEL, cs->hInstance, nullptr);

        HWND ui[] = { bPreset, bClear, bOk, bCancel, combo };
        for (HWND w : ui) SendMessageW(w, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
        SendMessageW(GetDlgItem(hwnd, IDC_LBL_PORT), WM_SETFONT, (WPARAM)g_big_font, TRUE);

        SetTimer(hwnd, IDC_TIMER, 60, nullptr);
        LayoutDialog();
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        PaintEverything(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_TIMER:
        // Keeps the "held" highlight live while a key is down.
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);

        // Key Set tabs
        for (int i = 0; i < INPUT_KEY_SETS; i++) {
            RECT t = d.tabs[i].r;
            t.bottom += 6;
            if (PtInRect(&t, POINT{ x, y })) {
                d.key_set = i;
                CancelListening();
                Refresh();
                return 0;
            }
        }

        // Turbo slider: click to set, then drag.
        if (d.show_slider && x >= d.slider.left && x <= d.slider.right &&
            y >= d.slider.top - 4 && y <= d.slider.bottom + 4) {
            SliderClicked(x);
            SetCapture(hwnd);
            return 0;
        }

        if (!d.pad) return 0;
        // Only clicks inside the panel count.
        if (x < d.panel.left || x >= d.panel.right ||
            y < d.panel.top  || y >= d.panel.bottom) return 0;

        const PadButton* b = pad_hit_test(d.pad, d.widths, d.turbo_widths,
                                          (float)x, (float)y, d.pad_x, d.pad_y, d.scale);
        if (!b) { CancelListening(); Refresh(); return 0; }

        int idx = (int)(b - d.pad->buttons);
        bool want_turbo = false;
        if (b->turbo_dy) {
            float dy = ((float)y - d.pad_y) / d.scale;
            want_turbo = (dy >= b->cy + b->turbo_dy - b->h / 2.0f);
        }

        InputSystem& in = InputSystem::get();
        PortMapping& m = in.mapping(d.port);
        int slot = m.find_by_id(b->id);
        if (slot < 0) {
            m.set_by_id(b->id, INPUT_NONE, d.key_set);
            slot = m.find_by_id(b->id);
            if (slot < 0) return 0;
        }
        d.listening = idx;
        d.listening_turbo = want_turbo;
        in.set_capture_key_set(d.key_set);
        in.set_capture_turbo(want_turbo);
        in.begin_capture(d.port, slot);
        Refresh();
        return 0;
    }

    case WM_MOUSEMOVE:
        if (GetCapture() == hwnd && d.show_slider)
            SliderClicked(GET_X_LPARAM(lp));
        return 0;

    case WM_LBUTTONUP:
        if (GetCapture() == hwnd) { ReleaseCapture(); InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;

    case WM_COMMAND: {
        switch (LOWORD(wp)) {
        case IDC_PLAYER:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                d.port = (unsigned)SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0);
                CancelListening();
                Refresh();
            }
            return 0;
        case IDC_PRESET:
            InputSystem::get().apply_defaults(d.console, d.port);
            CancelListening();
            LayoutDialog();
            Refresh();
            return 0;
        case IDC_CLEAR:
            InputSystem::get().mapping(d.port).clear();
            CancelListening();
            Refresh();
            return 0;
        case IDC_OK:     d.accepted = true;  DestroyWindow(hwnd); return 0;
        case IDC_CANCEL: d.accepted = false; DestroyWindow(hwnd); return 0;
        }
        return 0;
    }

    case WM_SIZE:
        LayoutDialog();
        Refresh();
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            if (d.listening >= 0) { CancelListening(); Refresh(); return 0; }
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;

    case WM_CLOSE:
        d.accepted = false;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, IDC_TIMER);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

bool ShowInputDialog(HWND parent, EmuConsoleType console) {
    ZeroMemory(&g_dlg, sizeof(g_dlg));
    g_dlg.console   = console;
    g_dlg.pad       = pad_layout_for(console);
    g_dlg.port      = 0;
    g_dlg.key_set   = 0;
    g_dlg.listening = -1;
    g_dlg.scale     = 1.0f;

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"NintendallControllerConfig";
    RegisterClassExW(&wc);

    RECT r = { 0, 0, 836, 660 };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_APPWINDOW,
        L"NintendallControllerConfig", L"Controller Configuration",
        WS_OVERLAPPEDWINDOW | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
        parent, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return false;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Polling here rather than only on input keeps held buttons highlighted
        // and lets a pending capture notice the gamepad.
        InputSystem::get().poll();
        if (g_dlg.listening >= 0 && !InputSystem::get().capturing()) {
            g_dlg.listening = -1;
            g_dlg.listening_turbo = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    InputSystem::get().cancel_capture();
    if (g_box_font) { DeleteObject(g_box_font); g_box_font = nullptr; }
    if (g_ui_font)  { DeleteObject(g_ui_font);  g_ui_font  = nullptr; }
    if (g_big_font) { DeleteObject(g_big_font); g_big_font = nullptr; }

    bool accepted = g_dlg.accepted;
    g_dlg.accepted = false;
    return accepted;
}