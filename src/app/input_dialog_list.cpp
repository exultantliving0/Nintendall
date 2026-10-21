// =============================================================================
//  Nintendall - the "Controller Configuration" window (list-based)
// =============================================================================

#include "input_dialog.h"

#include <windowsx.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#include "controller_layouts.h"
#include "input_config.h"

namespace {

enum {
    IDC_CONSOLE   = 100,
    IDC_PORT      = 101,
    IDC_PRESET    = 102,
    IDC_CLEAR     = 103,
    IDC_OK        = 104,
    IDC_CANCEL    = 105,
    IDC_LBL_CONSOLE = 106,
    IDC_LBL_PORT  = 107,
    IDC_TIMER     = 1,
};

struct ButtonRow {
    EmuInputId id;
    char name[64];
};

struct DialogState {
    HWND hwnd;
    EmuConsoleType console;
    unsigned port;
    int listening_type; // 0 = none, 1 = keyboard, 2 = gamepad
    int listening_slot; // which button slot
    bool accepted;
    
    std::vector<ButtonRow> buttons;
    std::vector<HWND> edit_kb;    // edit boxes for keyboard
    std::vector<HWND> edit_gp;    // edit boxes for gamepad
    std::vector<HWND> btn_kb;     // buttons to capture keyboard
    std::vector<HWND> btn_gp;     // buttons to capture gamepad
};

DialogState g_dlg;
HFONT g_ui_font = nullptr;
HFONT g_big_font = nullptr;

void BuildButtonList() {
    g_dlg.buttons.clear();
    const PadLayout* pad = pad_layout_for(g_dlg.console);
    if (!pad) return;
    for (int i = 0; i < pad->button_count; i++) {
        ButtonRow row;
        row.id = pad->buttons[i].id;
        // Use button name if available, otherwise generate from ID
        if (pad->buttons[i].name && pad->buttons[i].name[0]) {
            strncpy(row.name, pad->buttons[i].name, sizeof(row.name) - 1);
            row.name[sizeof(row.name) - 1] = 0;
        } else {
            switch (row.id) {
                case EMU_ID_UP: strcpy(row.name, "Up"); break;
                case EMU_ID_DOWN: strcpy(row.name, "Down"); break;
                case EMU_ID_LEFT: strcpy(row.name, "Left"); break;
                case EMU_ID_RIGHT: strcpy(row.name, "Right"); break;
                case EMU_ID_C_UP: strcpy(row.name, "C-Up"); break;
                case EMU_ID_C_DOWN: strcpy(row.name, "C-Down"); break;
                case EMU_ID_C_LEFT: strcpy(row.name, "C-Left"); break;
                case EMU_ID_C_RIGHT: strcpy(row.name, "C-Right"); break;
                case EMU_ID_ANALOG_LEFT_X: strcpy(row.name, "Joystick X Axis"); break;
                case EMU_ID_ANALOG_LEFT_Y: strcpy(row.name, "Joystick Y Axis"); break;
                case EMU_ID_A: strcpy(row.name, "A"); break;
                case EMU_ID_B: strcpy(row.name, "B"); break;
                case EMU_ID_X: strcpy(row.name, "X"); break;
                case EMU_ID_Y: strcpy(row.name, "Y"); break;
                case EMU_ID_L: strcpy(row.name, "L"); break;
                case EMU_ID_R: strcpy(row.name, "R"); break;
                case EMU_ID_L2: strcpy(row.name, "L2"); break;
                case EMU_ID_R2: strcpy(row.name, "R2"); break;
                case EMU_ID_START: strcpy(row.name, "Start"); break;
                case EMU_ID_SELECT: strcpy(row.name, "Select"); break;
                default: sprintf(row.name, "Button %d", (int)row.id); break;
            }
        }
        g_dlg.buttons.push_back(row);
    }
}

void UpdateDisplay() {
    InputSystem& in = InputSystem::get();
    const PortMapping& m = in.mapping(g_dlg.port);
    
    for (size_t i = 0; i < g_dlg.buttons.size(); i++) {
        const auto& btn = g_dlg.buttons[i];
        int slot = m.find_by_id(btn.id);
        bool is_analog = (btn.id == EMU_ID_ANALOG_LEFT_X || btn.id == EMU_ID_ANALOG_LEFT_Y);
        
        // Keyboard (keyset 0).  An analog axis has no meaningful keyboard
        // binding; the keyboard column is disabled for these rows.
        char buf[128];
        uint32_t kb_code = 0;
        if (!is_analog && slot >= 0 && m.codes[slot][0] != INPUT_NONE) {
            kb_code = m.codes[slot][0];
        }
        if (is_analog) {
            SetWindowTextA(g_dlg.edit_kb[i], "Analog");
        } else {
            InputSystem::describe(g_dlg.port, kb_code, buf, sizeof(buf));
            SetWindowTextA(g_dlg.edit_kb[i], buf);
        }
        
        // Gamepad (keyset 1)
        uint32_t gp_code = 0;
        if (slot >= 0 && m.codes[slot][1] != INPUT_NONE) {
            gp_code = m.codes[slot][1];
        }
        if (is_analog && gp_code == 0) {
            // The stick works without an explicit binding (the core defaults to
            // the left-stick axes).  Say so instead of showing a bare "--".
            SetWindowTextA(g_dlg.edit_gp[i],
                           btn.id == EMU_ID_ANALOG_LEFT_X ? "Auto (axis 0)" : "Auto (axis 1)");
        } else {
            InputSystem::describe(g_dlg.port, gp_code, buf, sizeof(buf));
            SetWindowTextA(g_dlg.edit_gp[i], buf);
        }
    }
}

void CancelListening() {
    g_dlg.listening_type = 0;
    g_dlg.listening_slot = -1;
    InputSystem::get().cancel_capture();
    UpdateDisplay();
}

void CleanupChildren() {
    for (auto h : g_dlg.edit_kb) if (h) DestroyWindow(h);
    for (auto h : g_dlg.edit_gp) if (h) DestroyWindow(h);
    for (auto h : g_dlg.btn_kb) if (h) DestroyWindow(h);
    for (auto h : g_dlg.btn_gp) if (h) DestroyWindow(h);
    g_dlg.edit_kb.clear();
    g_dlg.edit_gp.clear();
    g_dlg.btn_kb.clear();
    g_dlg.btn_gp.clear();
}

void LayoutControls() {
    CleanupChildren();
    
    RECT rc;
    GetClientRect(g_dlg.hwnd, &rc);
    
    int y = 80; // Start below header
    int left_margin = 20;
    int button_name_w = 120;
    int edit_w = 160;
    int btn_w = 80;
    int row_h = 28;
    int spacing = 8;
    
    // Create header labels
    HWND lbl_btn = CreateWindowExW(0, L"STATIC", L"Button", WS_CHILD | WS_VISIBLE | SS_LEFT,
        left_margin, y - 20, button_name_w, 16, g_dlg.hwnd, nullptr, GetModuleHandle(nullptr), nullptr);
    HWND lbl_kb = CreateWindowExW(0, L"STATIC", L"Keyboard", WS_CHILD | WS_VISIBLE | SS_LEFT,
        left_margin + button_name_w + spacing, y - 20, edit_w + btn_w + spacing, 16, g_dlg.hwnd, nullptr, GetModuleHandle(nullptr), nullptr);
    HWND lbl_gp = CreateWindowExW(0, L"STATIC", L"Controller", WS_CHILD | WS_VISIBLE | SS_LEFT,
        left_margin + button_name_w + spacing*2 + edit_w + btn_w, y - 20, edit_w + btn_w + spacing, 16, g_dlg.hwnd, nullptr, GetModuleHandle(nullptr), nullptr);
    
    SendMessageW(lbl_btn, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    SendMessageW(lbl_kb, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    SendMessageW(lbl_gp, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    
    BuildButtonList();
    g_dlg.edit_kb.resize(g_dlg.buttons.size());
    g_dlg.edit_gp.resize(g_dlg.buttons.size());
    g_dlg.btn_kb.resize(g_dlg.buttons.size());
    g_dlg.btn_gp.resize(g_dlg.buttons.size());
    
    for (size_t i = 0; i < g_dlg.buttons.size(); i++) {
        int cur_y = y + (int)i * (row_h + spacing);
        
        // Button name label
        HWND lbl = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
            left_margin, cur_y + 4, button_name_w, 20, g_dlg.hwnd, (HMENU)(2000 + (int)i), GetModuleHandle(nullptr), nullptr);
        SetWindowTextA(lbl, g_dlg.buttons[i].name);
        SendMessageW(lbl, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
        
        // Keyboard edit box
        g_dlg.edit_kb[i] = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL,
            left_margin + button_name_w + spacing, cur_y, edit_w, row_h, g_dlg.hwnd, nullptr, GetModuleHandle(nullptr), nullptr);
        SendMessageW(g_dlg.edit_kb[i], WM_SETFONT, (WPARAM)g_ui_font, TRUE);
        
        // Keyboard capture button
        g_dlg.btn_kb[i] = CreateWindowExW(0, L"BUTTON", L"Set", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            left_margin + button_name_w + spacing*2 + edit_w, cur_y, btn_w, row_h, g_dlg.hwnd, (HMENU)(3000 + (int)i), GetModuleHandle(nullptr), nullptr);
        SendMessageW(g_dlg.btn_kb[i], WM_SETFONT, (WPARAM)g_ui_font, TRUE);
        if (g_dlg.buttons[i].id == EMU_ID_ANALOG_LEFT_X ||
            g_dlg.buttons[i].id == EMU_ID_ANALOG_LEFT_Y) {
            EnableWindow(g_dlg.btn_kb[i], FALSE);
        }
        
        // Gamepad edit box
        g_dlg.edit_gp[i] = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL,
            left_margin + button_name_w + spacing*3 + edit_w + btn_w, cur_y, edit_w, row_h, g_dlg.hwnd, nullptr, GetModuleHandle(nullptr), nullptr);
        SendMessageW(g_dlg.edit_gp[i], WM_SETFONT, (WPARAM)g_ui_font, TRUE);
        
        // Gamepad capture button
        g_dlg.btn_gp[i] = CreateWindowExW(0, L"BUTTON", L"Set", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            left_margin + button_name_w + spacing*4 + edit_w*2 + btn_w, cur_y, btn_w, row_h, g_dlg.hwnd, (HMENU)(4000 + (int)i), GetModuleHandle(nullptr), nullptr);
        SendMessageW(g_dlg.btn_gp[i], WM_SETFONT, (WPARAM)g_ui_font, TRUE);
    }
    
    // Footer buttons
    int footer_y = rc.bottom - 40;
    MoveWindow(GetDlgItem(g_dlg.hwnd, IDC_CLEAR), left_margin, footer_y, 140, 26, TRUE);
    
    int bw = 84, gap = 8;
    int bx = rc.right - left_margin - bw * 2 - gap;
    MoveWindow(GetDlgItem(g_dlg.hwnd, IDC_CANCEL), bx, footer_y, bw, 26, TRUE);
    bx += bw + gap;
    MoveWindow(GetDlgItem(g_dlg.hwnd, IDC_OK), bx, footer_y, bw, 26, TRUE);
    
    UpdateDisplay();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        DebugLog("[Dialog] WM_CREATE entered\n");
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        g_dlg.hwnd = hwnd;
        
        g_ui_font = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        g_big_font = CreateFontA(-13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        
        // Console label
        CreateWindowExW(0, L"STATIC", L"Console:", WS_CHILD | WS_VISIBLE | SS_LEFT,
            20, 12, 60, 20, hwnd, (HMENU)IDC_LBL_CONSOLE, cs->hInstance, nullptr);
        
        // Console dropdown
        HWND comboConsole = CreateWindowExW(0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
            90, 10, 160, 24, hwnd, (HMENU)IDC_CONSOLE, cs->hInstance, nullptr);
        SendMessageA(comboConsole, CB_ADDSTRING, 0, (LPARAM)"NES");
        SendMessageA(comboConsole, CB_ADDSTRING, 0, (LPARAM)"SNES");
        SendMessageA(comboConsole, CB_ADDSTRING, 0, (LPARAM)"Game Boy");
        SendMessageA(comboConsole, CB_ADDSTRING, 0, (LPARAM)"Game Boy Color");
        SendMessageA(comboConsole, CB_ADDSTRING, 0, (LPARAM)"Game Boy Advance");
        SendMessageA(comboConsole, CB_ADDSTRING, 0, (LPARAM)"Nintendo 64");
        // Set current
        switch (g_dlg.console) {
            case EMU_CONSOLE_NES: SendMessageW(comboConsole, CB_SETCURSEL, 0, 0); break;
            case EMU_CONSOLE_SNES: SendMessageW(comboConsole, CB_SETCURSEL, 1, 0); break;
            case EMU_CONSOLE_GAMEBOY: SendMessageW(comboConsole, CB_SETCURSEL, 2, 0); break;
            case EMU_CONSOLE_GAMEBOY_COLOR: SendMessageW(comboConsole, CB_SETCURSEL, 3, 0); break;
            case EMU_CONSOLE_GAMEBOY_ADVANCE: SendMessageW(comboConsole, CB_SETCURSEL, 4, 0); break;
            case EMU_CONSOLE_NINTENDO_64: SendMessageW(comboConsole, CB_SETCURSEL, 5, 0); break;
            default: SendMessageW(comboConsole, CB_SETCURSEL, 0, 0); break;
        }
        
        // Port label
        CreateWindowExW(0, L"STATIC", L"Player:", WS_CHILD | WS_VISIBLE | SS_LEFT,
            270, 12, 50, 20, hwnd, (HMENU)IDC_LBL_PORT, cs->hInstance, nullptr);
        
        // Port dropdown
        HWND comboPort = CreateWindowExW(0, L"COMBOBOX", nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS,
            330, 10, 80, 24, hwnd, (HMENU)IDC_PORT, cs->hInstance, nullptr);
        for (int i = 0; i < INPUT_MAX_PORTS; i++) {
            char lbl[32];
            snprintf(lbl, sizeof(lbl), "Player %d", i + 1);
            SendMessageA(comboPort, CB_ADDSTRING, 0, (LPARAM)lbl);
        }
        SendMessageW(comboPort, CB_SETCURSEL, 0, 0);
        
        // Buttons
        HWND bPreset = CreateWindowExW(0, L"BUTTON", L"Select Preset...",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 420, 8, 116, 26,
            hwnd, (HMENU)IDC_PRESET, cs->hInstance, nullptr);
        HWND bClear = CreateWindowExW(0, L"BUTTON", L"Clear All Bindings",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0,
            hwnd, (HMENU)IDC_CLEAR, cs->hInstance, nullptr);
        HWND bOk = CreateWindowExW(0, L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 0, 0,
            hwnd, (HMENU)IDC_OK, cs->hInstance, nullptr);
        HWND bCancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0,
            hwnd, (HMENU)IDC_CANCEL, cs->hInstance, nullptr);
        
        HWND ui[] = { bPreset, bClear, bOk, bCancel, comboConsole, comboPort };
        for (HWND w : ui) if (w) SendMessageW(w, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
        SendMessageW(GetDlgItem(hwnd, IDC_LBL_CONSOLE), WM_SETFONT, (WPARAM)g_big_font, TRUE);
        SendMessageW(GetDlgItem(hwnd, IDC_LBL_PORT), WM_SETFONT, (WPARAM)g_big_font, TRUE);
        UINT_PTR timer_id = SetTimer(hwnd, IDC_TIMER, 60, nullptr);
        DebugLog("[Dialog] SetTimer returned: %p\n", (void*)timer_id);
        LayoutControls();
        return 0;
    }
    
    case WM_TIMER:
        InvalidateRect(hwnd, nullptr, FALSE);
        if (g_dlg.listening_type != 0 && !InputSystem::get().capturing()) {
            g_dlg.listening_type = 0;
            g_dlg.listening_slot = -1;
            UpdateDisplay();
        }
        return 0;
    
    case WM_SIZE:
        LayoutControls();
        return 0;
    
    case WM_COMMAND: {
        if (HIWORD(wp) == BN_CLICKED) {
            // Capture buttons
            if (LOWORD(wp) >= 3000 && LOWORD(wp) < 4000) {
                int idx = LOWORD(wp) - 3000;
                if (idx >= 0 && idx < (int)g_dlg.buttons.size()) {
                    g_dlg.listening_type = 1; // keyboard
                    g_dlg.listening_slot = idx;
                    InputSystem& in = InputSystem::get();
                    in.set_capture_key_set(0);
                    in.set_capture_type(1); // keyboard only
                    in.set_capture_turbo(false);
                    int slot = in.mapping(g_dlg.port).find_by_id(g_dlg.buttons[idx].id);
                    if (slot < 0) {
                        in.mapping(g_dlg.port).set_by_id(g_dlg.buttons[idx].id, INPUT_NONE, 0);
                        slot = in.mapping(g_dlg.port).find_by_id(g_dlg.buttons[idx].id);
                    }
                    if (slot >= 0) in.begin_capture(g_dlg.port, slot);
                }
                return 0;
            }
            if (LOWORD(wp) >= 4000 && LOWORD(wp) < 5000) {
                int idx = LOWORD(wp) - 4000;
                if (idx >= 0 && idx < (int)g_dlg.buttons.size()) {
                    g_dlg.listening_type = 2; // gamepad
                    g_dlg.listening_slot = idx;
                    InputSystem& in = InputSystem::get();
                    in.set_capture_key_set(1);
                    bool analog_row = (g_dlg.buttons[idx].id == EMU_ID_ANALOG_LEFT_X ||
                                       g_dlg.buttons[idx].id == EMU_ID_ANALOG_LEFT_Y);
                    in.set_capture_type(analog_row ? 3 : 2); // axis only / gamepad only
                    in.set_capture_turbo(false);
                    int slot = in.mapping(g_dlg.port).find_by_id(g_dlg.buttons[idx].id);
                    if (slot < 0) {
                        in.mapping(g_dlg.port).set_by_id(g_dlg.buttons[idx].id, INPUT_NONE, 1);
                        slot = in.mapping(g_dlg.port).find_by_id(g_dlg.buttons[idx].id);
                    }
                    if (slot >= 0) in.begin_capture(g_dlg.port, slot);
                }
                return 0;
            }
        }
        
        switch (LOWORD(wp)) {
        case IDC_CONSOLE:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int sel = (int)SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0);
                switch (sel) {
                    case 0: g_dlg.console = EMU_CONSOLE_NES; break;
                    case 1: g_dlg.console = EMU_CONSOLE_SNES; break;
                    case 2: g_dlg.console = EMU_CONSOLE_GAMEBOY; break;
                    case 3: g_dlg.console = EMU_CONSOLE_GAMEBOY_COLOR; break;
                    case 4: g_dlg.console = EMU_CONSOLE_GAMEBOY_ADVANCE; break;
                    case 5: g_dlg.console = EMU_CONSOLE_NINTENDO_64; break;
                }
                CancelListening();
                LayoutControls();
            }
            return 0;
        case IDC_PORT:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                g_dlg.port = (unsigned)SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0);
                CancelListening();
                UpdateDisplay();
            }
            return 0;
        case IDC_PRESET:
            InputSystem::get().apply_defaults(g_dlg.console, g_dlg.port);
            CancelListening();
            UpdateDisplay();
            return 0;
        case IDC_CLEAR:
            InputSystem::get().mapping(g_dlg.port).clear();
            CancelListening();
            UpdateDisplay();
            return 0;
        case IDC_OK:
            g_dlg.accepted = true;
            DestroyWindow(hwnd);
            return 0;
        case IDC_CANCEL:
            g_dlg.accepted = false;
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;
    }
    
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            if (g_dlg.listening_type != 0) {
                CancelListening();
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;
    
    case WM_CLOSE:
        g_dlg.accepted = false;
        DestroyWindow(hwnd);
        return 0;
    
    case WM_DESTROY:
        KillTimer(hwnd, IDC_TIMER);
        // Child windows are destroyed automatically when the dialog window is
        // destroyed - don't double-destroy them here.  Just clear our bookkeeping,
        // then post a quit message so ShowInputDialog's GetMessageW loop exits.
        g_dlg.edit_kb.clear();
        g_dlg.edit_gp.clear();
        g_dlg.btn_kb.clear();
        g_dlg.btn_gp.clear();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

bool ShowInputDialog(HWND parent, EmuConsoleType console) {
    ZeroMemory(&g_dlg, sizeof(g_dlg));
    g_dlg.console = console;
    g_dlg.port = 0;
    g_dlg.listening_type = 0;
    g_dlg.listening_slot = -1;
    g_dlg.accepted = false;
    
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = L"NintendallControllerConfig";
    RegisterClassExW(&wc);
    
    RECT r = { 0, 0, 720, 600 };
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

    while (true) {
        // While listening, keep polling input instead of blocking on messages.
        if (g_dlg.listening_type != 0) {
            if (!PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                InputSystem::get().poll();
                Sleep(1);
                continue;
            }
        } else {
            if (GetMessageW(&msg, nullptr, 0, 0) <= 0) break;  // WM_QUIT or error
        }

        InputSystem::get().poll();

        // IsDialogMessageW intercepts certain keys before WM_KEYDOWN ever
        // reaches our window procedure: Enter becomes "click the default OK
        // button", Escape becomes "click Cancel", etc.  While we are listening
        // for a keyboard binding, grab those keys here so they get bound
        // instead of closing or clicking the dialog.
        if (msg.message == WM_KEYDOWN &&
            g_dlg.listening_type != 0 && g_dlg.listening_slot >= 0) {
            int vk = (int)msg.wParam;
            if (vk == VK_ESCAPE) {
                CancelListening();
                continue;
            }
            // While listening, swallow all keyboard keys so they cannot reach
            // IsDialogMessageW (which would turn Enter into "click OK" or
            // arrow keys into focus navigation).  Only keyboard listening
            // actually binds the key; gamepad listening ignores the key.
            if (g_dlg.listening_type == 1 && vk >= 1 && vk <= 0x1FF) {
                InputSystem& in = InputSystem::get();
                PortMapping& m = in.mapping(g_dlg.port);
                int slot = m.find_by_id(g_dlg.buttons[g_dlg.listening_slot].id);
                if (slot < 0) {
                    m.set_by_id(g_dlg.buttons[g_dlg.listening_slot].id, INPUT_NONE, 0);
                    slot = m.find_by_id(g_dlg.buttons[g_dlg.listening_slot].id);
                }
                if (slot >= 0) {
                    m.codes[slot][0] = (uint32_t)vk;
                    g_dlg.listening_type = 0;
                    g_dlg.listening_slot = -1;
                    UpdateDisplay();
                }
            }
            continue;
        }

        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    
    InputSystem::get().cancel_capture();
    if (g_ui_font) { DeleteObject(g_ui_font); g_ui_font = nullptr; }
    if (g_big_font) { DeleteObject(g_big_font); g_big_font = nullptr; }
    
    bool accepted = g_dlg.accepted;
    g_dlg.accepted = false;
    return accepted;
}



