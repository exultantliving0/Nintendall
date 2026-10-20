// =============================================================================
//  Nintendall - input: gamepad polling, the button-mapping store and turbo
//  See input_config.h for the physical-input code scheme.
// =============================================================================

#include "input_config.h"

#include <stdio.h>
#include <string.h>

// -----------------------------------------------------------------------------
// XInput, loaded by hand so a missing DLL degrades instead of breaking startup
// -----------------------------------------------------------------------------

typedef struct _XINPUT_STATE {
    DWORD PacketNumber;
    struct {
        WORD  buttons;
        BYTE  bLeftTrigger;
        BYTE  bRightTrigger;
        SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY;
    } Gamepad;
} XINPUT_STATE, *PXINPUT_STATE;

typedef DWORD (WINAPI *XInputGetStateFn)(DWORD, XINPUT_STATE*);

static HMODULE          g_xinput = nullptr;
static XInputGetStateFn g_xinput_get_state = nullptr;
static bool             g_xinput_tried = false;

static void EnsureXInput() {
    if (g_xinput_tried) return;
    g_xinput_tried = true;
    g_xinput = LoadLibraryA("xinput1_4.dll");
    if (!g_xinput) g_xinput = LoadLibraryA("xinput9_1_0.dll");
    if (!g_xinput) return;
    g_xinput_get_state = (XInputGetStateFn)GetProcAddress(g_xinput, "XInputGetState");
    if (!g_xinput_get_state) {
        FreeLibrary(g_xinput);
        g_xinput = nullptr;
    }
}

const XInputButtonDef XINPUT_BUTTONS[] = {
    { XBTN_DPAD_UP,        "DPad Up"    },
    { XBTN_DPAD_DOWN,      "DPad Down"  },
    { XBTN_DPAD_LEFT,      "DPad Left"  },
    { XBTN_DPAD_RIGHT,     "DPad Right" },
    { XBTN_START,          "Start"      },
    { XBTN_BACK,           "Back"       },
    { XBTN_LEFT_THUMB,     "L Stick"    },
    { XBTN_RIGHT_THUMB,    "R Stick"    },
    { XBTN_LEFT_SHOULDER,  "LB"         },
    { XBTN_RIGHT_SHOULDER, "RB"         },
    { XBTN_GUIDE,          "Guide"      },
    { XBTN_MISC,           "Menu"       },
    { XBTN_LEFT_TRIGGER,   "LT"         },
    { XBTN_RIGHT_TRIGGER,  "RT"         },
};
const int XINPUT_BUTTON_COUNT = (int)(sizeof(XINPUT_BUTTONS) / sizeof(XINPUT_BUTTONS[0]));

// -----------------------------------------------------------------------------
// PortMapping
// -----------------------------------------------------------------------------

int PortMapping::count() const {
    int n = 0;
    for (int i = 0; i < INPUT_MAX_BUTTONS; i++) if (used[i]) n++;
    return n;
}

void PortMapping::clear() { memset(this, 0, sizeof(*this)); }

int PortMapping::find_by_id(EmuInputId id) const {
    for (int i = 0; i < INPUT_MAX_BUTTONS; i++)
        if (used[i] && buttons[i] == id) return i;
    return -1;
}

// Returns the slot for `id`, creating it if the table has room.  -1 when full.
static int EnsureSlot(PortMapping* m, EmuInputId id) {
    int slot = m->find_by_id(id);
    if (slot >= 0) return slot;
    for (int i = 0; i < INPUT_MAX_BUTTONS; i++) {
        if (!m->used[i]) {
            m->used[i]    = 1;
            m->buttons[i] = id;
            return i;
        }
    }
    return -1;
}

void PortMapping::set_by_id(EmuInputId id, uint32_t code, int key_set) {
    if (key_set < 0 || key_set >= INPUT_KEY_SETS) return;
    int slot = EnsureSlot(this, id);
    if (slot < 0) return;
    codes[slot][key_set] = code;
}

void PortMapping::set_turbo(EmuInputId id, uint32_t code) {
    int slot = EnsureSlot(this, id);
    if (slot < 0) return;
    turbo[slot]    = code;
    turbo_on[slot] = 1;
}

bool PortMapping::has_binding(EmuInputId id) const {
    int slot = find_by_id(id);
    if (slot < 0) return false;
    if (turbo_on[slot] && turbo[slot] != INPUT_NONE) return true;
    for (int k = 0; k < INPUT_KEY_SETS; k++)
        if (codes[slot][k] != INPUT_NONE) return true;
    return false;
}

// -----------------------------------------------------------------------------
// Default layouts - what each machine's pad actually had
// -----------------------------------------------------------------------------

namespace {

struct DefaultEntry { EmuInputId id; uint32_t code; };

const DefaultEntry kNES[] = {
    { EMU_ID_UP,     (uint32_t)VK_UP     }, { EMU_ID_DOWN,   (uint32_t)VK_DOWN  },
    { EMU_ID_LEFT,   (uint32_t)VK_LEFT   }, { EMU_ID_RIGHT,  (uint32_t)VK_RIGHT },
    { EMU_ID_A,      (uint32_t)'X'       }, { EMU_ID_B,      (uint32_t)'Z'      },
    { EMU_ID_SELECT, (uint32_t)VK_RSHIFT }, { EMU_ID_START,  (uint32_t)VK_RETURN},
};

const DefaultEntry kSNES[] = {
    { EMU_ID_UP,     (uint32_t)VK_UP     }, { EMU_ID_DOWN,   (uint32_t)VK_DOWN  },
    { EMU_ID_LEFT,   (uint32_t)VK_LEFT   }, { EMU_ID_RIGHT,  (uint32_t)VK_RIGHT },
    { EMU_ID_A,      (uint32_t)'X'       }, { EMU_ID_B,      (uint32_t)'Z'      },
    { EMU_ID_X,      (uint32_t)'S'       }, { EMU_ID_Y,      (uint32_t)'A'      },
    { EMU_ID_L,      (uint32_t)'Q'       }, { EMU_ID_R,      (uint32_t)'W'      },
    { EMU_ID_SELECT, (uint32_t)VK_RSHIFT }, { EMU_ID_START,  (uint32_t)VK_RETURN},
};

const DefaultEntry kGB[] = {
    { EMU_ID_UP,     (uint32_t)VK_UP     }, { EMU_ID_DOWN,   (uint32_t)VK_DOWN  },
    { EMU_ID_LEFT,   (uint32_t)VK_LEFT   }, { EMU_ID_RIGHT,  (uint32_t)VK_RIGHT },
    { EMU_ID_A,      (uint32_t)'X'       }, { EMU_ID_B,      (uint32_t)'Z'      },
    { EMU_ID_START,  (uint32_t)VK_RETURN }, { EMU_ID_SELECT, (uint32_t)VK_SHIFT },
};

const DefaultEntry kGBA[] = {
    { EMU_ID_UP,     (uint32_t)VK_UP     }, { EMU_ID_DOWN,   (uint32_t)VK_DOWN  },
    { EMU_ID_LEFT,   (uint32_t)VK_LEFT   }, { EMU_ID_RIGHT,  (uint32_t)VK_RIGHT },
    { EMU_ID_A,      (uint32_t)'X'       }, { EMU_ID_B,      (uint32_t)'Z'      },
    { EMU_ID_L,      (uint32_t)'Q'       }, { EMU_ID_R,      (uint32_t)'W'      },
    { EMU_ID_START,  (uint32_t)VK_RETURN }, { EMU_ID_SELECT, (uint32_t)VK_SHIFT },
};

   // The N64 core reads twelve inputs plus an analog joystick.
   const DefaultEntry kN64[] = {
       { EMU_ID_UP,     (uint32_t)VK_UP     }, { EMU_ID_DOWN,   (uint32_t)VK_DOWN  },
       { EMU_ID_LEFT,   (uint32_t)VK_LEFT   }, { EMU_ID_RIGHT,  (uint32_t)VK_RIGHT },
       { EMU_ID_A,      (uint32_t)'X'       }, { EMU_ID_B,      (uint32_t)'Z'      },
       { EMU_ID_C_UP,   (uint32_t)'C'       }, { EMU_ID_C_DOWN, (uint32_t)'V'      },
       { EMU_ID_C_LEFT, (uint32_t)'N'       }, { EMU_ID_C_RIGHT,(uint32_t)'M'      },
       { EMU_ID_L,      (uint32_t)'Q'       }, { EMU_ID_R,      (uint32_t)'W'      },
       { EMU_ID_Y,      (uint32_t)'T'       },  // Z trigger
       { EMU_ID_START,  (uint32_t)VK_RETURN },
   };
// apply_defaults adds INPUT_XINPUT_BASE + 1 to turn an index into a stored code.
const DefaultEntry kXInputPad[] = {
    { EMU_ID_A,      9  },   // RB
    { EMU_ID_B,      10 },   // LB
    { EMU_ID_X,      3  },   // Y
    { EMU_ID_Y,      2  },   // X
    { EMU_ID_L,      13 },   // LT
    { EMU_ID_R,      14 },   // RT
    { EMU_ID_START,  4  },   // Start
    { EMU_ID_SELECT, 5  },   // Back
    { EMU_ID_UP,     0  },
    { EMU_ID_DOWN,   1  },
    { EMU_ID_LEFT,   2  },
    { EMU_ID_RIGHT,  3  },
};

}  // namespace

// -----------------------------------------------------------------------------
// InputSystem
// -----------------------------------------------------------------------------

InputSystem::InputSystem() {
    memset(g_pad_pressed, 0, sizeof(g_pad_pressed));
    memset(g_pad_prev,    0, sizeof(g_pad_prev));
    memset(g_map,         0, sizeof(g_map));
}

InputSystem& InputSystem::get() {
    static InputSystem inst;
    return inst;
}

bool InputSystem::console_has_turbo(EmuConsoleType c) {
    // Turbo/autofire only means something on the machines that had it.
    return c == EMU_CONSOLE_NES || c == EMU_CONSOLE_SNES;
}

void InputSystem::set_turbo_speed(int s) {
    if (s < 1) s = 1;
    if (s > INPUT_TURBO_MAX - 1) s = INPUT_TURBO_MAX - 1;
    g_turbo_speed = s;
}

void InputSystem::apply_defaults(EmuConsoleType console, unsigned port) {
    const DefaultEntry* table = nullptr;
    int n = 0;
    switch (console) {
        case EMU_CONSOLE_NES:             table = kNES;  n = (int)(sizeof(kNES)  / sizeof(kNES[0]));  break;
        case EMU_CONSOLE_SNES:            table = kSNES; n = (int)(sizeof(kSNES) / sizeof(kSNES[0])); break;
        case EMU_CONSOLE_GAMEBOY:         table = kGB;   n = (int)(sizeof(kGB)   / sizeof(kGB[0]));   break;
        case EMU_CONSOLE_GAMEBOY_COLOR:   table = kGB;   n = (int)(sizeof(kGB)   / sizeof(kGB[0]));   break;
        case EMU_CONSOLE_GAMEBOY_ADVANCE: table = kGBA;  n = (int)(sizeof(kGBA)  / sizeof(kGBA[0]));  break;
        case EMU_CONSOLE_NINTENDO_64:     table = kN64;  n = (int)(sizeof(kN64)  / sizeof(kN64[0]));  break;
        default: break;
    }

    PortMapping& m = g_map[port & 3];
    m.clear();
    if (!table) return;

    // Key set #1 is the keyboard, and keeps the mapping Nintall has always had.
    for (int i = 0; i < n; i++) m.set_by_id(table[i].id, table[i].code, 0);

    // Key set #2 is an XInput pad, so plugging a controller in just works
    // instead of needing a trip through the dialog first.
    if (connected_pads() > 0) {
        for (int i = 0; i < (int)(sizeof(kXInputPad) / sizeof(kXInputPad[0])); i++)
            m.set_by_id(kXInputPad[i].id, INPUT_XINPUT_BASE + 1 + (uint32_t)kXInputPad[i].code, 1);
    }
}

void InputSystem::apply_all_defaults(EmuConsoleType console) {
    for (int p = 0; p < INPUT_MAX_PORTS; p++) apply_defaults(console, (unsigned)p);
}

int InputSystem::connected_pads() const {
    EnsureXInput();
    if (!g_xinput_get_state) return 0;
    int n = 0;
    XINPUT_STATE st;
    for (DWORD i = 0; i < INPUT_MAX_PORTS; i++)
        if (g_xinput_get_state(i, &st) == ERROR_SUCCESS) n++;
    return n;
}

void InputSystem::poll() {
    EnsureXInput();
    g_frame++;

    memcpy(g_pad_prev, g_pad_pressed, sizeof(g_pad_prev));
    memset(g_pad_pressed, 0, sizeof(g_pad_pressed));

    if (g_xinput_get_state) {
        XINPUT_STATE st;
        for (DWORD i = 0; i < INPUT_MAX_PORTS; i++) {
            if (g_xinput_get_state(i, &st) != ERROR_SUCCESS) continue;
            int bits = (int)st.Gamepad.buttons;
            // Treat the triggers as buttons so LT/RT can be mapped like any other.
            if (st.Gamepad.bLeftTrigger  > 30) bits |= XBTN_LEFT_TRIGGER;
            if (st.Gamepad.bRightTrigger > 30) bits |= XBTN_RIGHT_TRIGGER;
            g_pad_pressed[i] = bits;
        }
    }

    if (!capturing()) return;

    // Anything that went down this frame is offered to the listening slot.  The
    // keyboard is checked with GetAsyncKeyState's edge bit so a held key does
    // not re-bind itself every frame.
    PortMapping& m = g_map[g_capture_port];

    // Check keyboard if allowed
    if (g_capture_type == 0 || g_capture_type == 1) {
        for (int v = 1; v <= 0x1FF; v++) {
            if (!(GetAsyncKeyState(v) & 1)) continue;       // 1 == pressed since last call
            if (g_capture_turbo) {
                m.turbo[g_capture_slot] = (uint32_t)v;
                m.turbo_on[g_capture_slot] = 1;
            } else {
                m.codes[g_capture_slot][g_capture_key_set] = (uint32_t)v;
            }
            g_capture_port = -1;
            g_capture_slot = -1;
            return;
        }
    }

    // Check gamepad if allowed
    if (g_capture_type == 0 || g_capture_type == 2) {
        for (int p = 0; p < INPUT_MAX_PORTS; p++) {
            int fresh = g_pad_pressed[p] & ~g_pad_prev[p];
            if (!fresh) continue;
            for (int b = 0; b < XINPUT_BUTTON_COUNT; b++) {
                if (!(fresh & XINPUT_BUTTONS[b].mask)) continue;
                uint32_t code = INPUT_XINPUT_BASE + (uint32_t)(p * 256 + b);
                if (g_capture_turbo) {
                    m.turbo[g_capture_slot] = code;
                    m.turbo_on[g_capture_slot] = 1;
                } else {
                    m.codes[g_capture_slot][g_capture_key_set] = code;
                }
                g_capture_port = -1;
                g_capture_slot = -1;
                return;
            }
        }
    }
}

void InputSystem::begin_capture(unsigned port, int slot) {
    g_capture_port = (int)(port & 3);
    g_capture_slot = slot;
    // Swallow whatever is already held, so holding a key does not instantly bind.
    memcpy(g_pad_prev, g_pad_pressed, sizeof(g_pad_prev));
    while (GetAsyncKeyState(VK_LBUTTON) & 1) { }
    for (int v = 1; v <= 0x1FF; v++) (void)GetAsyncKeyState(v);   // clear edge bits
}

void InputSystem::cancel_capture() {
    g_capture_port = -1;
    g_capture_slot = -1;
}

// Turbo fires while its key is held, pulsing once every turbo_period() frames.
// Holding the turbo key acts as the trigger, which is how Mesen behaves.
int InputSystem::turbo_edge(unsigned port, int slot) const {
    if (slot < 0 || slot >= INPUT_MAX_BUTTONS) return 0;
    // Turbo is only meaningful on the consoles that had it.
    if (!console_has_turbo(g_console)) return 0;
    const PortMapping& m = g_map[port];
    if (!m.turbo_on[slot] || m.turbo[slot] == INPUT_NONE) return 0;

    uint32_t code = m.turbo[slot];
    int held = 0;
    if (INPUT_IS_KEYBOARD(code))
        held = (GetAsyncKeyState((int)code) & 0x8000) ? 1 : 0;
    else if (INPUT_IS_GAMEPAD(code)) {
        uint32_t v = code - INPUT_XINPUT_BASE;
        int p = (int)(v / 256), b = (int)(v % 256);
        if (p >= 0 && p < INPUT_MAX_PORTS && b >= 0 && b < XINPUT_BUTTON_COUNT)
            held = (g_pad_pressed[p] & XINPUT_BUTTONS[b].mask) ? 1 : 0;
    }
    if (!held) return 0;

    int period = turbo_period();
    if (period < INPUT_TURBO_MIN) period = INPUT_TURBO_MIN;
    // Hold for one frame of every period, so a short tap still registers.
    return (g_frame % (unsigned)period) == 0;
}

int InputSystem::turbo_firing(unsigned port, EmuInputId id) const {
    if (port >= INPUT_MAX_PORTS) return 0;
    return turbo_edge(port, g_map[port].find_by_id(id));
}

int InputSystem::is_pressed(unsigned port, EmuInputId id) const {
    if (port >= INPUT_MAX_PORTS) return 0;
    const PortMapping& m = g_map[port];
    int slot = m.find_by_id(id);
    if (slot < 0) return 0;

    for (int k = 0; k < INPUT_KEY_SETS; k++) {
        uint32_t code = m.codes[slot][k];
        if (code == INPUT_NONE) continue;
        if (INPUT_IS_KEYBOARD(code)) {
            if (GetAsyncKeyState((int)code) & 0x8000) return 1;
        } else if (INPUT_IS_GAMEPAD(code)) {
            uint32_t v = code - INPUT_XINPUT_BASE;
            int p = (int)(v / 256), b = (int)(v % 256);
            if (p < 0 || p >= INPUT_MAX_PORTS || b < 0 || b >= XINPUT_BUTTON_COUNT) continue;
            if (g_pad_pressed[p] & XINPUT_BUTTONS[b].mask) return 1;
        }
    }
    return turbo_edge(port, slot);
}

const char* InputSystem::describe(uint32_t code, char* buf, size_t bufSize) {
    if (bufSize == 0) return buf;
    if (code == INPUT_NONE) { snprintf(buf, bufSize, "--"); return buf; }

    if (INPUT_IS_KEYBOARD(code)) {
        UINT vk = (UINT)code;
        if (vk >= VK_F1 && vk <= VK_F24) {
            snprintf(buf, bufSize, "F%d", (int)(vk - VK_F1 + 1));
            return buf;
        }
        wchar_t name[64];
        if (GetKeyNameTextW((LONG)vk, name, (int)(sizeof(name) / sizeof(name[0])))) {
            if (WideCharToMultiByte(CP_UTF8, 0, name, -1, buf, (int)bufSize, nullptr, nullptr) > 0)
                return buf;
        }
        snprintf(buf, bufSize, "Key %u", code);
        return buf;
    }

    if (INPUT_IS_GAMEPAD(code)) {
        uint32_t v = code - INPUT_XINPUT_BASE;
        int p = (int)(v / 256), b = (int)(v % 256);
        if (b >= 0 && b < XINPUT_BUTTON_COUNT)
            snprintf(buf, bufSize, "Joy%d %s", p + 1, XINPUT_BUTTONS[b].name);
        else
            snprintf(buf, bufSize, "Joy%d Btn%d", p + 1, b);
        return buf;
    }

    if (INPUT_IS_MOUSE(code)) {
        snprintf(buf, bufSize, "Mouse %d", (int)(code - INPUT_MOUSE_BASE));
        return buf;
    }

    snprintf(buf, bufSize, "--");
    return buf;
}

// -----------------------------------------------------------------------------
// Persistence - a plain text file next to the executable
// -----------------------------------------------------------------------------

bool InputSystem::save(const char* path) const {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "# Nintendall input mapping\n");
    fprintf(f, "# keyset <1..4> per button, then one line per key set\n");
    fprintf(f, "# 0=unbound 1..0x1FF=key 0x1FF..=mouse 0xFFFF..=xinput\n");
    fprintf(f, "turbo_speed %d\n", g_turbo_speed);
    for (int p = 0; p < INPUT_MAX_PORTS; p++) {
        const PortMapping& m = g_map[p];
        fprintf(f, "port%d %d\n", p + 1, m.count());
        for (int i = 0; i < INPUT_MAX_BUTTONS; i++) {
            if (!m.used[i]) continue;
            fprintf(f, "  %d", (int)m.buttons[i]);
            for (int k = 0; k < INPUT_KEY_SETS; k++) fprintf(f, " %u", m.codes[i][k]);
            fprintf(f, "\n");
            if (m.turbo_on[i] && m.turbo[i] != INPUT_NONE)
                fprintf(f, "    turbo %d %u\n", (int)m.buttons[i], m.turbo[i]);
        }
    }
    fclose(f);
    return true;
}

bool InputSystem::load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    memset(g_map, 0, sizeof(g_map));

    int p = 0;
    char line[256];
    while (fgets(line, (int)sizeof(line), f)) {
        // Entries are written indented, so skip leading blanks before matching
        // the keyword - otherwise the turbo lines never match and turbo
        // bindings are silently dropped on every restart.
        char* s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\n' || *s == '\r' || *s == 0) continue;

        if (strncmp(s, "turbo_speed", 11) == 0) {
            int v = 6;
            if (sscanf(s + 11, "%d", &v) == 1) set_turbo_speed(v);
            continue;
        }
        if (strncmp(s, "port", 4) == 0) {
            int port = 0, n = 0;
            if (sscanf(s + 4, "%d %d", &port, &n) == 2 && port >= 1 && port <= INPUT_MAX_PORTS) {
                g_map[port - 1].clear();
                p = port;
            }
            continue;
        }
        if (strncmp(s, "turbo", 5) == 0) {
            int id = 0; unsigned code = 0;
            if (sscanf(s + 5, "%d %u", &id, &code) == 2 && p >= 1 && p <= INPUT_MAX_PORTS)
                g_map[p - 1].set_turbo((EmuInputId)id, code);
            continue;
        }
        {
            // "<id> <code> <code> <code> <code>" - older files carry one code.
            int id = 0;
            unsigned code[INPUT_KEY_SETS] = { 0, 0, 0, 0 };
            int got = sscanf(s, "%d %u %u %u %u", &id, &code[0], &code[1], &code[2], &code[3]);
            if (got >= 2 && p >= 1 && p <= INPUT_MAX_PORTS) {
                for (int k = 0; k < INPUT_KEY_SETS && k < got - 1; k++)
                    g_map[p - 1].set_by_id((EmuInputId)id, code[k], k);
            }
        }
    }
    fclose(f);
    return true;
}

