// =============================================================================
//  Nintendall - input: gamepad polling, the button-mapping store and turbo
//  See input_config.h for the physical-input code scheme.
//  Now uses SDL3 GameController API for unified cross-platform input.
// =============================================================================

#include "input_config.h"

#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_gamepad.h>

// =============================================================================
// PortMapping implementation
// =============================================================================

int PortMapping::count() const {
    int n = 0;
    for (int i = 0; i < INPUT_MAX_BUTTONS; i++) if (used[i]) n++;
    return n;
}

void PortMapping::clear() {
    memset(buttons, 0, sizeof(buttons));
    memset(codes, 0, sizeof(codes));
    memset(turbo, 0, sizeof(turbo));
    memset(turbo_on, 0, sizeof(turbo_on));
    memset(used, 0, sizeof(used));
}

int PortMapping::find_by_id(EmuInputId id) const {
    for (int i = 0; i < INPUT_MAX_BUTTONS; i++) {
        if (used[i] && buttons[i] == id) return i;
    }
    return -1;
}

void PortMapping::set_by_id(EmuInputId id, uint32_t code, int key_set) {
    int slot = find_by_id(id);
    if (slot < 0) {
        for (int i = 0; i < INPUT_MAX_BUTTONS; i++) {
            if (!used[i]) { slot = i; break; }
        }
        if (slot < 0) return;
        buttons[slot] = id;
        used[slot] = 1;
    }
    if (key_set >= 0 && key_set < INPUT_KEY_SETS) {
        codes[slot][key_set] = code;
    }
}

void PortMapping::set_turbo(EmuInputId id, uint32_t code) {
    int slot = find_by_id(id);
    if (slot >= 0) {
        turbo[slot] = code;
        turbo_on[slot] = (code != INPUT_NONE);
    }
}

bool PortMapping::has_binding(EmuInputId id) const {
    return find_by_id(id) >= 0;
}

// =============================================================================
// SDL3 GameController mapping
// =============================================================================

// Maps SDL GameController buttons to our EmuInputId
static const struct { SDL_GamepadButton sdl; EmuInputId emu; const char* name; } sdl_button_map[] = {
    { SDL_GAMEPAD_BUTTON_SOUTH, EMU_ID_A,          "A" },          // A / Cross
    { SDL_GAMEPAD_BUTTON_EAST,  EMU_ID_B,          "B" },          // B / Circle
    { SDL_GAMEPAD_BUTTON_WEST,  EMU_ID_Y,          "Y" },          // Y / Square
    { SDL_GAMEPAD_BUTTON_NORTH, EMU_ID_X,          "X" },          // X / Triangle
    { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  EMU_ID_L,  "LB" },       // LB / L1
    { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, EMU_ID_R,  "RB" },       // RB / R1
    { SDL_GAMEPAD_BUTTON_BACK,  EMU_ID_SELECT,     "Select" },    // Back / Select / Share
    { SDL_GAMEPAD_BUTTON_START, EMU_ID_START,      "Start" },     // Start / Options
    { SDL_GAMEPAD_BUTTON_LEFT_STICK, EMU_ID_L3,    "L3" },        // L3
    { SDL_GAMEPAD_BUTTON_RIGHT_STICK, EMU_ID_R3,   "R3" },        // R3
    { SDL_GAMEPAD_BUTTON_DPAD_UP,    EMU_ID_UP,    "D-Up" },
    { SDL_GAMEPAD_BUTTON_DPAD_DOWN,  EMU_ID_DOWN,  "D-Down" },
    { SDL_GAMEPAD_BUTTON_DPAD_LEFT,  EMU_ID_LEFT,  "D-Left" },
    { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, EMU_ID_RIGHT, "D-Right" },
    { SDL_GAMEPAD_BUTTON_GUIDE, EMU_ID_HOME,       "Guide" },
};

static const struct { SDL_GamepadAxis sdl; EmuInputId emu; } sdl_axis_map[] = {
    { SDL_GAMEPAD_AXIS_LEFTX,  EMU_ID_ANALOG_LEFT_X },
    { SDL_GAMEPAD_AXIS_LEFTY,  EMU_ID_ANALOG_LEFT_Y },
    { SDL_GAMEPAD_AXIS_RIGHTX, EMU_ID_ANALOG_RIGHT_X },
    { SDL_GAMEPAD_AXIS_RIGHTY, EMU_ID_ANALOG_RIGHT_Y },
    { SDL_GAMEPAD_AXIS_LEFT_TRIGGER,  EMU_ID_TRIGGER_LEFT },
    { SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, EMU_ID_TRIGGER_RIGHT },
};

static const int SDL_BUTTON_COUNT = sizeof(sdl_button_map) / sizeof(sdl_button_map[0]);
static const int SDL_AXIS_COUNT = sizeof(sdl_axis_map) / sizeof(sdl_axis_map[0]);

// =============================================================================
// Internal state
// =============================================================================

static bool s_sdl_initialized = false;
static SDL_Gamepad* s_gamepads[INPUT_MAX_PORTS] = { nullptr };
static int s_gamepad_instance_ids[INPUT_MAX_PORTS] = { -1, -1, -1, -1 };
static bool s_sdl_initialized_once = false;

static int s_pad_pressed[INPUT_MAX_PORTS] = { 0 };
static int s_pad_prev[INPUT_MAX_PORTS] = { 0 };

// =============================================================================
// Helpers
// =============================================================================

// NSO (Nintendo Switch Online) controller mappings - not in SDL3's built-in DB yet
// VID: 0x057e (Nintendo), PIDs: 0x2006/2007=NES, 0x2008=SNES, 0x2009=N64, 0x200a=Genesis
static const char* nso_mappings[] = {
    // NSO NES Controller (and Famicom) - D-pad, A, B, Start, Select
    "050000007e0500000620000000000000,Nintendo Switch Online NES Controller,a:b0,b:b1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,start:b9,back:b8,",
    "050000007e0500000720000000000000,Nintendo Switch Online Famicom Controller,a:b0,b:b1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,start:b9,back:b8,",
    
    // NSO SNES Controller - D-pad, A, B, X, Y, L, R, Start, Select
    "050000007e0500000820000000000000,Nintendo Switch Online SNES Controller,a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,start:b9,back:b8,",
    
    // NSO N64 Controller - A, B, C-buttons (X,Y), L, R, Z (leftshoulder), Start, D-pad
    // N64 uses: A=b0, B=b1, C-Up/C-Down/C-Left/C-Right mapped to right stick or dpad
    "050000007e0500000920000000000000,Nintendo Switch Online N64 Controller,a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,start:b9,guide:b8,",
    
    // NSO Sega Genesis Controller - 6-button: A, B, C, X, Y, Z, Start, Mode
    "050000007e0500000a20000000000000,Nintendo Switch Online Genesis Controller,a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,start:b9,guide:b8,",
};

// Add NSO controller mappings to SDL's gamepad database at runtime
static void AddNSOMappings() {
    for (int i = 0; i < (int)(sizeof(nso_mappings)/sizeof(nso_mappings[0])); i++) {
        if (SDL_AddGamepadMapping(nso_mappings[i]) < 0) {
            // Mapping might already exist or be invalid - ignore
        }
    }
}

static void EnsureSDL() {
    if (s_sdl_initialized_once) return;
    s_sdl_initialized_once = true;
    if (SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC) < 0) {
        // SDL initialization failed - continue without gamepad support
        return;
    }
    s_sdl_initialized = true;
    AddNSOMappings();
}

static void UpdateGamepadList() {
    if (!s_sdl_initialized) return;

    int count = 0;
    const SDL_JoystickID* joysticks = SDL_GetJoysticks(&count);
    if (!joysticks) return;

    // Track which gamepads we've seen
    bool seen[INPUT_MAX_PORTS] = { false };

    for (int i = 0; i < count && i < INPUT_MAX_PORTS; i++) {
        SDL_JoystickID instance_id = joysticks[i];
        if (SDL_IsGamepad(instance_id)) {
            // Find a free slot
            int slot = -1;
            for (int s = 0; s < INPUT_MAX_PORTS; s++) {
                if (s_gamepad_instance_ids[s] == instance_id) {
                    slot = s;
                    break;
                }
                if (s_gamepad_instance_ids[s] == -1 && slot == -1) {
                    slot = s;
                }
            }
            if (slot >= 0) {
                if (s_gamepads[slot] && s_gamepad_instance_ids[slot] != instance_id) {
                    SDL_CloseGamepad(s_gamepads[slot]);
                    s_gamepads[slot] = nullptr;
                }
                if (!s_gamepads[slot]) {
                    s_gamepads[slot] = SDL_OpenGamepad(instance_id);
                }
                s_gamepad_instance_ids[slot] = instance_id;
                seen[slot] = true;
            }
        }
    }

    // Close gamepads that are no longer connected
    for (int i = 0; i < INPUT_MAX_PORTS; i++) {
        if (!seen[i] && s_gamepads[i]) {
            SDL_CloseGamepad(s_gamepads[i]);
            s_gamepads[i] = nullptr;
            s_gamepad_instance_ids[i] = -1;
        }
    }
}

// Helper: get button name for display
static const char* sdl_button_name(int sdl_btn) {
    for (int i = 0; i < SDL_BUTTON_COUNT; i++) {
        if (sdl_button_map[i].sdl == sdl_btn) return sdl_button_map[i].name;
    }
    return "Btn";
}

// =============================================================================
// InputSystem implementation
// =============================================================================

InputSystem::InputSystem() {
    memset(g_pad_pressed, 0, sizeof(g_pad_pressed));
    memset(g_pad_prev, 0, sizeof(g_pad_prev));
    memset(g_map, 0, sizeof(g_map));
    memset(g_analog, 0, sizeof(g_analog));
    for (int i = 0; i < INPUT_MAX_PORTS; i++) {
        s_gamepad_instance_ids[i] = -1;
        s_gamepads[i] = nullptr;
    }
}

InputSystem& InputSystem::get() {
    static InputSystem inst;
    return inst;
}

void InputSystem::poll() {
    EnsureSDL();
    UpdateGamepadList();

    g_frame++;

    memcpy(g_pad_prev, g_pad_pressed, sizeof(g_pad_prev));
    memset(g_pad_pressed, 0, sizeof(g_pad_pressed));
    memset(g_analog, 0, sizeof(g_analog));

    for (int port = 0; port < INPUT_MAX_PORTS; port++) {
        SDL_Gamepad* gp = s_gamepads[port];
        if (!gp) continue;

        // Buttons
        int bits = 0;
        for (int i = 0; i < SDL_BUTTON_COUNT; i++) {
            if (SDL_GetGamepadButton(gp, sdl_button_map[i].sdl)) {
                bits |= (1 << sdl_button_map[i].emu);
            }
        }
        g_pad_pressed[port] = bits;

        // Analog axes
        for (int i = 0; i < SDL_AXIS_COUNT; i++) {
            Sint16 val = SDL_GetGamepadAxis(gp, sdl_axis_map[i].sdl);
            // SDL axis is -32768..32767, normalize to -128..127
            int16_t norm = (int16_t)(val * 128 / 32768);
            // Apply deadzone (~25%)
            const int deadzone = 32;
            if (abs(norm) < deadzone) norm = 0;
            this->g_analog[port][i] = (int8_t)norm;
        }
    }

    // Handle capture mode for remapping dialog
    process_capture();
}

// =============================================================================
// Input state query
// =============================================================================

int InputSystem::is_pressed(unsigned port, EmuInputId id) const {
    if (port >= INPUT_MAX_PORTS) return 0;

    // Check analog axes
    if (id >= EMU_ID_ANALOG_LEFT_X && id <= EMU_ID_TRIGGER_RIGHT) {
        int axis = id - EMU_ID_ANALOG_LEFT_X;
        if (axis >= 0 && axis < 6) return this->g_analog[port][axis] != 0 ? 1 : 0;
    }

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
            if (p >= 0 && p < INPUT_MAX_PORTS && b >= 0 && b < 16) {
                SDL_Gamepad* gp = s_gamepads[p];
                if (gp && SDL_GetGamepadButton(gp, (SDL_GamepadButton)b)) return 1;
            }
        }
    }
    return turbo_edge(port, slot);
}

int InputSystem::turbo_firing(unsigned port, EmuInputId id) const {
    if (port >= INPUT_MAX_PORTS) return 0;
    return turbo_edge(port, g_map[port].find_by_id(id));
}

// =============================================================================
// Rumble / Haptics
// =============================================================================

void InputSystem::set_rumble(unsigned port, uint16_t low_freq, uint16_t high_freq) {
    if (port >= INPUT_MAX_PORTS) return;
    SDL_Gamepad* gp = s_gamepads[port];
    if (!gp) return;
    SDL_RumbleGamepad(gp, low_freq, high_freq, 0); // duration 0 = until stopped
}

void InputSystem::stop_rumble(unsigned port) {
    if (port >= INPUT_MAX_PORTS) return;
    SDL_Gamepad* gp = s_gamepads[port];
    if (!gp) return;
    SDL_RumbleGamepad(gp, 0, 0, 0);
}

// =============================================================================
// Mapping / Remapping
// =============================================================================

void InputSystem::begin_capture(unsigned port, int slot) {
    g_capture_port = (int)(port & 3);
    g_capture_slot = slot;
    // Swallow whatever is already held
    memcpy(g_pad_prev, g_pad_pressed, sizeof(g_pad_prev));
    while (GetAsyncKeyState(VK_LBUTTON) & 1) { }
    for (int v = 1; v <= 0x1FF; v++) (void)GetAsyncKeyState(v);
    // Clear gamepad state for capture
    memcpy(s_pad_prev, g_pad_pressed, sizeof(g_pad_prev));
    memset(s_pad_prev, 0, sizeof(s_pad_prev));
}

void InputSystem::cancel_capture() {
    g_capture_port = -1;
    g_capture_slot = -1;
}

// Capture handling - called from input_dialog_list.cpp
void InputSystem::process_capture() {
    if (!capturing()) return;

    // Check keyboard if allowed
    if (g_capture_type == 0 || g_capture_type == 1) {
        for (int v = 1; v <= 0x1FF; v++) {
            if (GetAsyncKeyState(v) & 1) {
                PortMapping& m = g_map[g_capture_port];
                if (g_capture_turbo) {
                    m.turbo[g_capture_slot] = (uint32_t)v;
                    m.turbo_on[g_capture_slot] = 1;
                } else {
                    m.codes[g_capture_slot][g_capture_key_set] = (uint32_t)v;
                }
                cancel_capture();
                return;
            }
        }
    }

    // Check gamepad if allowed
    if (g_capture_type == 0 || g_capture_type == 2) {
        EnsureSDL();
        for (int p = 0; p < INPUT_MAX_PORTS; p++) {
            if (!s_gamepads[p]) continue;
            for (int b = 0; b < 16; b++) {
                if (SDL_GetGamepadButton(s_gamepads[p], (SDL_GamepadButton)b)) {
                    PortMapping& m = g_map[g_capture_port];
                    uint32_t code = INPUT_XINPUT_BASE + (uint32_t)(p * 256 + b);
                    if (g_capture_turbo) {
                        m.turbo[g_capture_slot] = code;
                        m.turbo_on[g_capture_slot] = 1;
                    } else {
                        m.codes[g_capture_slot][g_capture_key_set] = code;
                    }
                    cancel_capture();
                    return;
                }
            }
        }
    }
}

// =============================================================================
// Turbo
// =============================================================================

int InputSystem::turbo_edge(unsigned port, int slot) const {
    if (slot < 0 || slot >= INPUT_MAX_BUTTONS) return 0;
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
        if (p >= 0 && p < INPUT_MAX_PORTS && b >= 0 && b < 16) {
            SDL_Gamepad* gp = s_gamepads[p];
            if (gp && SDL_GetGamepadButton(gp, (SDL_GamepadButton)b)) held = 1;
        }
    }
    if (!held) return 0;

    int period = turbo_period();
    if (period < INPUT_TURBO_MIN) period = INPUT_TURBO_MIN;
    return (g_frame % (unsigned)period) == 0;
}

// =============================================================================
// Description / Naming
// =============================================================================

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
        if (b >= 0 && b < 16) {
            const char* name = sdl_button_name(b);
            snprintf(buf, bufSize, "Joy%d %s", p + 1, name);
            return buf;
        }
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

// =============================================================================
// Persistence
// =============================================================================

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

// =============================================================================
// Defaults
// =============================================================================

void InputSystem::apply_defaults(EmuConsoleType console, unsigned port) {
    // Default keyboard mappings
    struct { EmuInputId id; uint32_t code; } defaults[] = {
        { EMU_ID_UP,    VK_UP }, { EMU_ID_DOWN,  VK_DOWN },
        { EMU_ID_LEFT,  VK_LEFT }, { EMU_ID_RIGHT, VK_RIGHT },
        { EMU_ID_A,     'X' }, { EMU_ID_B, 'Z' },
        { EMU_ID_X,     'S' }, { EMU_ID_Y, 'A' },
        { EMU_ID_L,     'Q' }, { EMU_ID_R, 'W' },
        { EMU_ID_START, VK_RETURN }, { EMU_ID_SELECT, VK_SHIFT },
    };

    PortMapping& m = g_map[port & 3];
    m.clear();
    for (auto& d : defaults) m.set_by_id(d.id, d.code, 0);

    // Apply SDL gamepad defaults for key set 1 - use actual SDL button enum values
    // Note: Triggers are axes in SDL3, not buttons. L2/R2 map to analog triggers.
    EnsureSDL();
    int count = 0;
    const SDL_JoystickID* joysticks = SDL_GetJoysticks(&count);
    if (joysticks && count > 0) {
        for (int i = 0; i < count && i < INPUT_MAX_PORTS; i++) {
            if (SDL_IsGamepad(joysticks[i])) {
                // Map SDL gamepad to our default layout using actual SDL enum values
                static const struct { EmuInputId id; SDL_GamepadButton sdl_btn; } pad_map[] = {
                    { EMU_ID_A,      SDL_GAMEPAD_BUTTON_SOUTH },
                    { EMU_ID_B,      SDL_GAMEPAD_BUTTON_EAST },
                    { EMU_ID_X,      SDL_GAMEPAD_BUTTON_NORTH },
                    { EMU_ID_Y,      SDL_GAMEPAD_BUTTON_WEST },
                    { EMU_ID_L,      SDL_GAMEPAD_BUTTON_LEFT_SHOULDER },
                    { EMU_ID_R,      SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER },
                    { EMU_ID_START,  SDL_GAMEPAD_BUTTON_START },
                    { EMU_ID_SELECT, SDL_GAMEPAD_BUTTON_BACK },
                    { EMU_ID_UP,     SDL_GAMEPAD_BUTTON_DPAD_UP },
                    { EMU_ID_DOWN,   SDL_GAMEPAD_BUTTON_DPAD_DOWN },
                    { EMU_ID_LEFT,   SDL_GAMEPAD_BUTTON_DPAD_LEFT },
                    { EMU_ID_RIGHT,  SDL_GAMEPAD_BUTTON_DPAD_RIGHT },
                };
                for (auto& entry : pad_map) {
                    // Store as port*256 + sdl_button_enum
                    uint32_t code = INPUT_XINPUT_BASE + (uint32_t)((port & 3) * 256 + entry.sdl_btn);
                    m.set_by_id(entry.id, code, 1);
                }
                break; // Only first controller for defaults
            }
        }
    }
}

void InputSystem::apply_all_defaults(EmuConsoleType console) {
    for (int p = 0; p < INPUT_MAX_PORTS; p++) apply_defaults(console, (unsigned)p);
}

// =============================================================================
// Turbo
// =============================================================================

bool InputSystem::console_has_turbo(EmuConsoleType c) {
    return c == EMU_CONSOLE_NES || c == EMU_CONSOLE_SNES;
}

void InputSystem::set_turbo_speed(int s) {
    if (s < 1) s = 1;
    if (s > INPUT_TURBO_MAX - 1) s = INPUT_TURBO_MAX - 1;
    g_turbo_speed = s;
}

// =============================================================================
// Connected pads
// =============================================================================

int InputSystem::connected_pads() const {
    EnsureSDL();
    int n = 0;
    int count = 0;
    const SDL_JoystickID* joysticks = SDL_GetJoysticks(&count);
    if (joysticks) {
        for (int i = 0; i < count && i < INPUT_MAX_PORTS; i++) {
            if (SDL_IsGamepad(joysticks[i])) n++;
        }
    }
    return n;
}

// =============================================================================
// Active console (for turbo gating)
// =============================================================================