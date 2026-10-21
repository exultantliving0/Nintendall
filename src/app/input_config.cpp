// =============================================================================
//  Nintendall - input: gamepad polling, the button-mapping store and turbo
//  See input_config.h for the physical-input code scheme.
//  Now uses SDL3 GameController API + raw Joystick fallback for universal input.
// =============================================================================

#include "input_config.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>

// Simple file debug log
void DebugLog(const char* fmt, ...) {
    FILE* f = fopen("C:\\temp\\nintendall_input.log", "a");
    if (f) {
        va_list args;
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);
        fclose(f);
    }
}

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
// SDL3 GameController mapping (standard layout)
// =============================================================================

static const struct { SDL_GamepadButton sdl; EmuInputId emu; const char* name; } sdl_button_map[] = {
    { SDL_GAMEPAD_BUTTON_SOUTH, EMU_ID_A,          "A" },
    { SDL_GAMEPAD_BUTTON_EAST,  EMU_ID_B,          "B" },
    { SDL_GAMEPAD_BUTTON_WEST,  EMU_ID_Y,          "Y" },
    { SDL_GAMEPAD_BUTTON_NORTH, EMU_ID_X,          "X" },
    { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  EMU_ID_L,  "LB" },
    { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, EMU_ID_R,  "RB" },
    { SDL_GAMEPAD_BUTTON_BACK,  EMU_ID_SELECT,     "Select" },
    { SDL_GAMEPAD_BUTTON_START, EMU_ID_START,      "Start" },
    { SDL_GAMEPAD_BUTTON_LEFT_STICK, EMU_ID_L3,    "L3" },
    { SDL_GAMEPAD_BUTTON_RIGHT_STICK, EMU_ID_R3,   "R3" },
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
// NSO (Nintendo Switch Online) controller mappings - placeholder until we get real GUIDs
// =============================================================================

static const char* nso_mappings[] = {
    "050000007e0500000620000000000000,Nintendo Switch Online NES Controller,a:b0,b:b1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,start:b9,back:b8,",
    "050000007e0500000720000000000000,Nintendo Switch Online Famicom Controller,a:b0,b:b1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,start:b9,back:b8,",
    "050000007e0500000820000000000000,Nintendo Switch Online SNES Controller,a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,start:b9,back:b8,",
    "050000007e0500000920000000000000,Nintendo Switch Online N64 Controller,a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,start:b9,guide:b8,",
    "050000007e0500000a20000000000000,Nintendo Switch Online Genesis Controller,a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,start:b9,guide:b8,",
};

static void AddNSOMappings() {
    for (int i = 0; i < (int)(sizeof(nso_mappings)/sizeof(nso_mappings[0])); i++) {
        if (SDL_AddGamepadMapping(nso_mappings[i]) < 0) { /* ignore */ }
    }
}

// =============================================================================
// Internal state
// =============================================================================

static bool s_sdl_initialized = false;
static bool s_sdl_initialized_once = false;

// GameController devices (standard layout)
static SDL_Gamepad* s_gamepads[INPUT_MAX_PORTS] = { nullptr };
static int s_gamepad_instance_ids[INPUT_MAX_PORTS] = { -1, -1, -1, -1 };

// Raw Joystick devices (fallback for non-GameController devices)
static SDL_Joystick* s_joysticks[INPUT_MAX_PORTS] = { nullptr };
static int s_joystick_instance_ids[INPUT_MAX_PORTS] = { -1, -1, -1, -1 };
static int s_joystick_num_buttons[INPUT_MAX_PORTS] = { 0 };
static int s_joystick_num_axes[INPUT_MAX_PORTS] = { 0 };
static int s_joystick_num_hats[INPUT_MAX_PORTS] = { 0 };

static int s_pad_pressed[INPUT_MAX_PORTS] = { 0 };
static int s_pad_prev[INPUT_MAX_PORTS] = { 0 };

// =============================================================================
// Helpers
// =============================================================================

static void EnsureSDL() {
    if (s_sdl_initialized_once) return;
    s_sdl_initialized_once = true;
    if (SDL_InitSubSystem(SDL_INIT_GAMEPAD | SDL_INIT_HAPTIC | SDL_INIT_JOYSTICK) < 0) {
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

    bool seen_gp[INPUT_MAX_PORTS] = { false };
    bool seen_joy[INPUT_MAX_PORTS] = { false };

    for (int i = 0; i < count && i < INPUT_MAX_PORTS; i++) {
        SDL_JoystickID instance_id = joysticks[i];

        // Check if this is a new device
        bool is_new_gp = false;
        bool is_new_joy = false;
        int existing_gp_slot = -1;
        int existing_joy_slot = -1;

        for (int s = 0; s < INPUT_MAX_PORTS; s++) {
            if (s_gamepad_instance_ids[s] == instance_id) existing_gp_slot = s;
            if (s_joystick_instance_ids[s] == instance_id) existing_joy_slot = s;
        }

        if (SDL_IsGamepad(instance_id)) {
            if (existing_gp_slot == -1) is_new_gp = true;
        } else {
            if (existing_joy_slot == -1) is_new_joy = true;
        }

        if (SDL_IsGamepad(instance_id)) {
            // GameController path
            int slot = -1;
            for (int s = 0; s < INPUT_MAX_PORTS; s++) {
                if (s_gamepad_instance_ids[s] == instance_id) { slot = s; break; }
                if (s_gamepad_instance_ids[s] == -1 && slot == -1) { slot = s; }
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
                seen_gp[slot] = true;
                if (is_new_gp) {
                    // Log new GameController device
                    SDL_Joystick* joy = SDL_OpenJoystick(instance_id);
                    if (joy) {
                        SDL_GUID guid = SDL_GetJoystickGUID(joy);
                        char guid_str[33];
                        SDL_GUIDToString(guid, guid_str, sizeof(guid_str));
                        const char* name = SDL_GetJoystickName(joy);
                        Uint16 vid = SDL_GetJoystickVendor(joy);
                        Uint16 pid = SDL_GetJoystickProduct(joy);
                        char dbg[256];
                        snprintf(dbg, sizeof(dbg), "[Input] GameController connected: VID=%04x PID=%04x Name='%s' GUID=%s\n",
                               vid, pid, name ? name : "unknown", guid_str);
                        DebugLog("%s", dbg);
                        SDL_CloseJoystick(joy);
                    }
                }
            }
        } else {
            // Raw Joystick fallback
            int slot = -1;
            for (int s = 0; s < INPUT_MAX_PORTS; s++) {
                if (s_joystick_instance_ids[s] == instance_id) { slot = s; break; }
                if (s_joystick_instance_ids[s] == -1 && slot == -1) { slot = s; }
            }
            if (slot >= 0) {
                if (s_joysticks[slot] && s_joystick_instance_ids[slot] != instance_id) {
                    SDL_CloseJoystick(s_joysticks[slot]);
                    s_joysticks[slot] = nullptr;
                }
                if (!s_joysticks[slot]) {
                    s_joysticks[slot] = SDL_OpenJoystick(instance_id);
                    if (s_joysticks[slot]) {
                        s_joystick_num_buttons[slot] = SDL_GetNumJoystickButtons(s_joysticks[slot]);
                        s_joystick_num_axes[slot] = SDL_GetNumJoystickAxes(s_joysticks[slot]);
                        s_joystick_num_hats[slot] = SDL_GetNumJoystickHats(s_joysticks[slot]);
                        if (is_new_joy) {
                            char dbg[128];
                            snprintf(dbg, sizeof(dbg), "[Input] Raw joystick connected: slot=%d buttons=%d axes=%d hats=%d\n",
                                   slot, s_joystick_num_buttons[slot], s_joystick_num_axes[slot], s_joystick_num_hats[slot]);
                            DebugLog("%s", dbg);
                        }
                    }
                }
                s_joystick_instance_ids[slot] = instance_id;
                seen_joy[slot] = true;
            }
        }
    }

    // Close disconnected
    for (int i = 0; i < INPUT_MAX_PORTS; i++) {
        if (!seen_gp[i] && s_gamepads[i]) {
            char dbg[64];
            snprintf(dbg, sizeof(dbg), "[Input] GameController disconnected: slot=%d\n", i);
            DebugLog("%s", dbg);
            SDL_CloseGamepad(s_gamepads[i]);
            s_gamepads[i] = nullptr;
            s_gamepad_instance_ids[i] = -1;
        }
        if (!seen_joy[i] && s_joysticks[i]) {
            char dbg[64];
            snprintf(dbg, sizeof(dbg), "[Input] Raw joystick disconnected: slot=%d\n", i);
            DebugLog("%s", dbg);
            SDL_CloseJoystick(s_joysticks[i]);
            s_joysticks[i] = nullptr;
            s_joystick_instance_ids[i] = -1;
            s_joystick_num_buttons[i] = 0;
            s_joystick_num_axes[i] = 0;
            s_joystick_num_hats[i] = 0;
        }
    }
}

// Helper: button name for GameController
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
        s_joystick_instance_ids[i] = -1;
        s_joysticks[i] = nullptr;
        s_joystick_num_buttons[i] = 0;
        s_joystick_num_axes[i] = 0;
        s_joystick_num_hats[i] = 0;
    }
}

InputSystem& InputSystem::get() {
    static InputSystem inst;
    return inst;
}

void InputSystem::poll() {
    static int poll_counter = 0;
    if (++poll_counter % 100 == 0) {
        DebugLog("[Poll] poll() called, frame=%u\n", g_frame);
    }
    EnsureSDL();
    UpdateGamepadList();

    g_frame++;

    memcpy(g_pad_prev, g_pad_pressed, sizeof(g_pad_prev));
    memset(g_pad_pressed, 0, sizeof(g_pad_pressed));
    memset(g_analog, 0, sizeof(g_analog));

    for (int port = 0; port < INPUT_MAX_PORTS; port++) {
        // GameController path
        SDL_Gamepad* gp = s_gamepads[port];
        if (gp) {
            int bits = 0;
            for (int i = 0; i < SDL_BUTTON_COUNT; i++) {
                if (SDL_GetGamepadButton(gp, sdl_button_map[i].sdl)) {
                    bits |= (1 << sdl_button_map[i].emu);
                }
            }
            g_pad_pressed[port] = bits;

            for (int i = 0; i < SDL_AXIS_COUNT; i++) {
                Sint16 val = SDL_GetGamepadAxis(gp, sdl_axis_map[i].sdl);
                int16_t norm = (int16_t)(val * 128 / 32768);
                const int deadzone = 32;
                if (abs(norm) < deadzone) norm = 0;
                this->g_analog[port][i] = (int8_t)norm;
            }
        }

        // Raw Joystick path
        SDL_Joystick* joy = s_joysticks[port];
        if (joy) {
            // Buttons
            int nbtn = s_joystick_num_buttons[port];
            for (int b = 0; b < nbtn && b < 32; b++) {
                if (SDL_GetJoystickButton(joy, b)) {
                    // Map raw button to a virtual EmuInputId starting at EMU_ID_JOYSTICK_UP (300)
                    // We'll use EMU_ID_JOYSTICK_UP + b for raw buttons
                    if (EMU_ID_JOYSTICK_UP + b < 332) { // safety
                        g_pad_pressed[port] |= (1 << (EMU_ID_JOYSTICK_UP + b));
                    }
                }
            }

            // Axes
            int naxis = s_joystick_num_axes[port];
            for (int a = 0; a < naxis && a < 6; a++) {
                Sint16 val = SDL_GetJoystickAxis(joy, a);
                int16_t norm = (int16_t)(val * 128 / 32768);
                const int deadzone = 32;
                if (abs(norm) < deadzone) norm = 0;
                // Store in g_analog starting at axis 6 (after standard 6 axes)
                if (a < 6) this->g_analog[port][a] = (int8_t)norm; // overlap for now
            }

            // Hats (D-pad on many raw joysticks)
            int nhats = s_joystick_num_hats[port];
            for (int h = 0; h < nhats; h++) {
                Uint8 hat = SDL_GetJoystickHat(joy, h);
                if (hat & SDL_HAT_UP)    g_pad_pressed[port] |= (1 << (EMU_ID_JOYSTICK_UP + 32 + h*4 + 0));
                if (hat & SDL_HAT_DOWN)  g_pad_pressed[port] |= (1 << (EMU_ID_JOYSTICK_UP + 32 + h*4 + 1));
                if (hat & SDL_HAT_LEFT)  g_pad_pressed[port] |= (1 << (EMU_ID_JOYSTICK_UP + 32 + h*4 + 2));
                if (hat & SDL_HAT_RIGHT) g_pad_pressed[port] |= (1 << (EMU_ID_JOYSTICK_UP + 32 + h*4 + 3));
            }
        }
    }

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

    // Check raw joystick buttons/axes/hats
    if (id >= EMU_ID_JOYSTICK_UP && id < EMU_ID_JOYSTICK_UP + 64) {
        int raw_idx = id - EMU_ID_JOYSTICK_UP;
        if (raw_idx < 32) {
            // Buttons 0-31
            SDL_Joystick* joy = s_joysticks[port];
            if (joy && raw_idx < s_joystick_num_buttons[port]) {
                return SDL_GetJoystickButton(joy, raw_idx) ? 1 : 0;
            }
        } else if (raw_idx < 64) {
            // Hats 0-3 (4 directions each)
            int hat_idx = (raw_idx - 32) / 4;
            int dir = (raw_idx - 32) % 4;
            SDL_Joystick* joy = s_joysticks[port];
            if (joy && hat_idx < s_joystick_num_hats[port]) {
                Uint8 hat = SDL_GetJoystickHat(joy, hat_idx);
                Uint8 mask = (dir == 0) ? SDL_HAT_UP : (dir == 1) ? SDL_HAT_DOWN : (dir == 2) ? SDL_HAT_LEFT : SDL_HAT_RIGHT;
                return (hat & mask) ? 1 : 0;
            }
        }
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
            // Also support raw joystick codes (port*256 + 128 + button/hat)
            if (b >= 128 && b < 192) {
                int raw_b = b - 128;
                SDL_Joystick* joy = s_joysticks[p];
                if (joy && raw_b < s_joystick_num_buttons[p]) {
                    return SDL_GetJoystickButton(joy, raw_b) ? 1 : 0;
                }
            }
            if (b >= 192 && b < 256) {
                int hat_raw = b - 192;
                int hat_idx = hat_raw / 4;
                int dir = hat_raw % 4;
                SDL_Joystick* joy = s_joysticks[p];
                if (joy && hat_idx < s_joystick_num_hats[p]) {
                    Uint8 hat = SDL_GetJoystickHat(joy, hat_idx);
                    Uint8 mask = (dir == 0) ? SDL_HAT_UP : (dir == 1) ? SDL_HAT_DOWN : (dir == 2) ? SDL_HAT_LEFT : SDL_HAT_RIGHT;
                    return (hat & mask) ? 1 : 0;
                }
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
    SDL_RumbleGamepad(gp, low_freq, high_freq, 0);
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
    memcpy(g_pad_prev, g_pad_pressed, sizeof(g_pad_prev));
    while (GetAsyncKeyState(VK_LBUTTON) & 1) { }
    for (int v = 1; v <= 0x1FF; v++) (void)GetAsyncKeyState(v);
}

void InputSystem::cancel_capture() {
    g_capture_port = -1;
    g_capture_slot = -1;
}

// Capture handling - called from input_dialog_list.cpp
void InputSystem::process_capture() {
    if (!capturing()) return;

    // DEBUG
    static int capture_debug_counter = 0;
    if (++capture_debug_counter % 100 == 0) {
        char dbg[128];
        snprintf(dbg, sizeof(dbg), "[Capture] Active: port=%d slot=%d type=%d keyset=%d\n",
               g_capture_port, g_capture_slot, g_capture_type, g_capture_key_set);
        DebugLog("%s", dbg);
    }

    // Keyboard
    if (g_capture_type == 0 || g_capture_type == 1) {
        for (int v = 1; v <= 0x1FF; v++) {
            if (GetAsyncKeyState(v) & 1) {
                char dbg[64];
                snprintf(dbg, sizeof(dbg), "[Capture] Keyboard: VK=%d\n", v);
                DebugLog("%s", dbg);
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

    // GameController
    if (g_capture_type == 0 || g_capture_type == 2) {
        EnsureSDL();
        for (int p = 0; p < INPUT_MAX_PORTS; p++) {
            if (!s_gamepads[p]) continue;
            for (int b = 0; b < 16; b++) {
                if (SDL_GetGamepadButton(s_gamepads[p], (SDL_GamepadButton)b)) {
                    char dbg[64];
                    snprintf(dbg, sizeof(dbg), "[Capture] GameController: port=%d btn=%d\n", p, b);
                    DebugLog("%s", dbg);
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

    // Raw Joystick fallback
    if (g_capture_type == 0 || g_capture_type == 2) {
        EnsureSDL();
        for (int p = 0; p < INPUT_MAX_PORTS; p++) {
            if (!s_joysticks[p]) continue;
            // Buttons
            for (int b = 0; b < s_joystick_num_buttons[p] && b < 32; b++) {
                if (SDL_GetJoystickButton(s_joysticks[p], b)) {
                    char dbg[64];
                    snprintf(dbg, sizeof(dbg), "[Capture] Raw Joystick: port=%d btn=%d\n", p, b);
                    DebugLog("%s", dbg);
                    PortMapping& m = g_map[g_capture_port];
                    uint32_t code = INPUT_XINPUT_BASE + (uint32_t)(p * 256 + 128 + b);
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
            // Hats
            for (int h = 0; h < s_joystick_num_hats[p]; h++) {
                Uint8 hat = SDL_GetJoystickHat(s_joysticks[p], h);
                if (hat) {
                    for (int d = 0; d < 4; d++) {
                        Uint8 mask = (d == 0) ? SDL_HAT_UP : (d == 1) ? SDL_HAT_DOWN : (d == 2) ? SDL_HAT_LEFT : SDL_HAT_RIGHT;
                        if (hat & mask) {
                            char dbg[64];
                            snprintf(dbg, sizeof(dbg), "[Capture] Raw Joystick Hat: port=%d hat=%d dir=%d\n", p, h, d);
                            DebugLog("%s", dbg);
                            PortMapping& m = g_map[g_capture_port];
                            uint32_t code = INPUT_XINPUT_BASE + (uint32_t)(p * 256 + 192 + h * 4 + d);
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
        if (p >= 0 && p < INPUT_MAX_PORTS) {
            if (b >= 0 && b < 16) {
                SDL_Gamepad* gp = s_gamepads[p];
                if (gp && SDL_GetGamepadButton(gp, (SDL_GamepadButton)b)) held = 1;
            } else if (b >= 128 && b < 160) {
                int raw_b = b - 128;
                SDL_Joystick* joy = s_joysticks[p];
                if (joy && raw_b < s_joystick_num_buttons[p]) {
                    held = SDL_GetJoystickButton(joy, raw_b) ? 1 : 0;
                }
            } else if (b >= 192 && b < 256) {
                int hat_raw = b - 192;
                int hat_idx = hat_raw / 4;
                int dir = hat_raw % 4;
                SDL_Joystick* joy = s_joysticks[p];
                if (joy && hat_idx < s_joystick_num_hats[p]) {
                    Uint8 hat = SDL_GetJoystickHat(joy, hat_idx);
                    Uint8 mask = (dir == 0) ? SDL_HAT_UP : (dir == 1) ? SDL_HAT_DOWN : (dir == 2) ? SDL_HAT_LEFT : SDL_HAT_RIGHT;
                    held = (hat & mask) ? 1 : 0;
                }
            }
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
        } else if (b >= 128 && b < 160) {
            // Raw joystick button
            snprintf(buf, bufSize, "Joy%d Btn%d", p + 1, b - 128);
            return buf;
        } else if (b >= 192 && b < 256) {
            // Raw joystick hat
            int hat_raw = b - 192;
            int hat_idx = hat_raw / 4;
            int dir = hat_raw % 4;
            const char* dirs[4] = { "Hat-Up", "Hat-Down", "Hat-Left", "Hat-Right" };
            snprintf(buf, bufSize, "Joy%d %s%d", p + 1, dirs[dir], hat_idx + 1);
            return buf;
        }
        snprintf(buf, bufSize, "Joy%d Code%d", p + 1, b);
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

    // GameController defaults for key set 1
    EnsureSDL();
    int count = 0;
    const SDL_JoystickID* joysticks = SDL_GetJoysticks(&count);
    if (joysticks && count > 0) {
        for (int i = 0; i < count && i < INPUT_MAX_PORTS; i++) {
            if (SDL_IsGamepad(joysticks[i])) {
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
                    uint32_t code = INPUT_XINPUT_BASE + (uint32_t)((port & 3) * 256 + entry.sdl_btn);
                    m.set_by_id(entry.id, code, 1);
                }
                break;
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
            if (SDL_IsGamepad(joysticks[i]) || (i < INPUT_MAX_PORTS)) n++; // count all joysticks
        }
    }
    return n;
}

// =============================================================================
// Active console (for turbo gating)
// =============================================================================