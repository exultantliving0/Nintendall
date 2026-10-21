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

// Simple file debug log, enabled with the NINTENDALL_DEBUG environment variable.
void DebugLog(const char* fmt, ...) {
    static int enabled = -1;
    if (enabled < 0) {
        enabled = GetEnvironmentVariableA("NINTENDALL_DEBUG", nullptr, 0) > 0 ? 1 : 0;
    }
    if (!enabled) return;

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

static const int SDL_BUTTON_COUNT = sizeof(sdl_button_map) / sizeof(sdl_button_map[0]);

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

// Controller-specific naming/defaults.  SDL already knows how to talk to the
// NSO N64 pad; this only lets us show its real button names and give it a
// sensible default mapping.
enum ControllerProfile {
    PROFILE_GENERIC = 0,
    PROFILE_NSO_N64,
};
static ControllerProfile s_profile[INPUT_MAX_PORTS] = { PROFILE_GENERIC, PROFILE_GENERIC, PROFILE_GENERIC, PROFILE_GENERIC };

// Capture edge detection: the first poll after begin_capture() records the
// current state so a button that was already held (or a bad mapping that
// reports a permanent press) cannot bind itself instantly.
static bool s_cap_has_snapshot = false;
static bool s_cap_prev_gp[INPUT_MAX_PORTS][16];
static Uint8 s_cap_prev_btn[INPUT_MAX_PORTS][32];
static Uint8 s_cap_prev_hat[INPUT_MAX_PORTS][4];
static Sint16 s_cap_prev_axis[INPUT_MAX_PORTS][INPUT_AXIS_COUNT];
static Sint16 s_cap_prev_raw_axis[INPUT_MAX_PORTS][INPUT_AXIS_COUNT];

// The joystick behind a port, whether SDL classified it as a gamepad or as a
// plain joystick.  Raw-button bindings need this for both kinds of device.
static SDL_Joystick* PortJoystick(int port) {
    if (s_gamepads[port]) return SDL_GetGamepadJoystick(s_gamepads[port]);
    return s_joysticks[port];
}

// SDL's HIDAPI driver already identifies the NSO N64 pad, both through its
// vendor/product and through the controller-type byte at the end of the GUID.
static ControllerProfile DetectControllerProfile(SDL_Joystick* joy) {
    if (!joy) return PROFILE_GENERIC;
    if (SDL_GetJoystickVendor(joy) == 0x057e && SDL_GetJoystickProduct(joy) == 0x2019)
        return PROFILE_NSO_N64;
    SDL_GUID guid = SDL_GetJoystickGUID(joy);
    if (guid.data[15] == 0x0c)  // k_eSwitchDeviceInfoControllerType_N64
        return PROFILE_NSO_N64;
    return PROFILE_GENERIC;
}

// Convert an SDL axis to the signed 8-bit range the cores use.  Trigger axes
// rest at 0 and rise to 127; stick axes are centred on 0.
static int8_t NormalizeAxisValue(int axis, Sint16 val) {
    if (axis >= 4) {
        if (val < 0) val = 0;  // some HID devices report triggers as -32768..32767
        int n = (int)val * 128 / 32768;
        if (n < 0) n = 0;
        if (n > 127) n = 127;
        return (int8_t)n;
    }

    int n = (int)val * 128 / 32768;
    if (n < -128) n = -128;
    if (n > 127) n = 127;
    const int deadzone = 10;  // ~8% - enough to hide worn-stick drift
    if (n > -deadzone && n < deadzone) n = 0;
    return (int8_t)n;
}

static int8_t ReadAxisNormalized(int port, int axis) {
    if (axis < 0 || axis >= INPUT_AXIS_COUNT) return 0;
    if (s_gamepads[port])
        return NormalizeAxisValue(axis, SDL_GetGamepadAxis(s_gamepads[port], (SDL_GamepadAxis)axis));
    if (s_joysticks[port] && axis < s_joystick_num_axes[port])
        return NormalizeAxisValue(axis, SDL_GetJoystickAxis(s_joysticks[port], axis));
    return 0;
}

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
                    if (s_gamepads[slot])
                        s_profile[slot] = DetectControllerProfile(SDL_GetGamepadJoystick(s_gamepads[slot]));
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
                        s_profile[slot] = DetectControllerProfile(s_joysticks[slot]);
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
            s_profile[i] = PROFILE_GENERIC;
        }
        if (!seen_joy[i] && s_joysticks[i]) {
            char dbg[64];
            snprintf(dbg, sizeof(dbg), "[Input] Raw joystick disconnected: slot=%d\n", i);
            DebugLog("%s", dbg);
            SDL_CloseJoystick(s_joysticks[i]);
            s_joysticks[i] = nullptr;
            s_joystick_instance_ids[i] = -1;
            s_profile[i] = PROFILE_GENERIC;
            s_joystick_num_buttons[i] = 0;
            s_joystick_num_axes[i] = 0;
            s_joystick_num_hats[i] = 0;
        }
    }
}

// Record the current physical state when capture starts.
static void SnapshotCaptureState() {
    memset(s_cap_prev_gp, 0, sizeof(s_cap_prev_gp));
    memset(s_cap_prev_btn, 0, sizeof(s_cap_prev_btn));
    memset(s_cap_prev_hat, 0, sizeof(s_cap_prev_hat));
    memset(s_cap_prev_axis, 0, sizeof(s_cap_prev_axis));
    memset(s_cap_prev_raw_axis, 0, sizeof(s_cap_prev_raw_axis));

    for (int p = 0; p < INPUT_MAX_PORTS; p++) {
        SDL_Gamepad* gp = s_gamepads[p];
        if (gp) {
            for (int b = 0; b < 16; b++)
                s_cap_prev_gp[p][b] = SDL_GetGamepadButton(gp, (SDL_GamepadButton)b) ? true : false;
            for (int a = 0; a < INPUT_AXIS_COUNT; a++)
                s_cap_prev_axis[p][a] = NormalizeAxisValue(a, SDL_GetGamepadAxis(gp, (SDL_GamepadAxis)a));
        }
        SDL_Joystick* joy = PortJoystick(p);
        if (joy) {
            int nbtn = SDL_GetNumJoystickButtons(joy);
            for (int b = 0; b < nbtn && b < 32; b++)
                s_cap_prev_btn[p][b] = SDL_GetJoystickButton(joy, b);

            int nhats = SDL_GetNumJoystickHats(joy);
            for (int h = 0; h < nhats && h < 4; h++)
                s_cap_prev_hat[p][h] = SDL_GetJoystickHat(joy, h);

            int naxes = SDL_GetNumJoystickAxes(joy);
            for (int a = 0; a < naxes && a < INPUT_AXIS_COUNT; a++)
                s_cap_prev_raw_axis[p][a] = NormalizeAxisValue(a, SDL_GetJoystickAxis(joy, a));
        }
    }
}

// Return the code of a newly pressed raw button/hat on this port, or 0.
static uint32_t PollRawJoystickCapture(int p, SDL_Joystick* joy) {
    int nbtn = SDL_GetNumJoystickButtons(joy);
    for (int b = 0; b < nbtn && b < 32; b++) {
        Uint8 pressed = SDL_GetJoystickButton(joy, b) ? 1 : 0;
        if (pressed && !s_cap_prev_btn[p][b]) {
            s_cap_prev_btn[p][b] = pressed;
            return INPUT_XINPUT_BASE + (uint32_t)(p * 256 + 128 + b);
        }
        s_cap_prev_btn[p][b] = pressed;
    }

    int nhats = SDL_GetNumJoystickHats(joy);
    for (int h = 0; h < nhats && h < 4; h++) {
        Uint8 hat = SDL_GetJoystickHat(joy, h);
        Uint8 prev = s_cap_prev_hat[p][h];
        for (int d = 0; d < 4; d++) {
            Uint8 mask = (d == 0) ? SDL_HAT_UP : (d == 1) ? SDL_HAT_DOWN
                       : (d == 2) ? SDL_HAT_LEFT : SDL_HAT_RIGHT;
            if ((hat & mask) && !(prev & mask)) {
                s_cap_prev_hat[p][h] = hat;
                return INPUT_XINPUT_BASE + (uint32_t)(p * 256 + 192 + h * 4 + d);
            }
        }
        s_cap_prev_hat[p][h] = hat;
    }
    return 0;
}

// Return the code of a newly moved gamepad axis, or 0.
static uint32_t PollGamepadAxisCapture(int p, SDL_Gamepad* gp) {
    const int threshold = 60;  // ~47% of full travel
    for (int a = 0; a < INPUT_AXIS_COUNT; a++) {
        Sint16 cur = NormalizeAxisValue(a, SDL_GetGamepadAxis(gp, (SDL_GamepadAxis)a));
        Sint16 prev = s_cap_prev_axis[p][a];
        bool now = abs((int)cur) > threshold;
        bool was = abs((int)prev) > threshold;
        s_cap_prev_axis[p][a] = cur;
        if (now && !was)
            return INPUT_XINPUT_BASE + (uint32_t)(p * 256 + (int)INPUT_AXIS_BASE + a);
    }
    return 0;
}

// Return the code of a newly moved raw-joystick axis, or 0.
static uint32_t PollRawAxisCapture(int p, SDL_Joystick* joy) {
    const int threshold = 60;
    int naxes = SDL_GetNumJoystickAxes(joy);
    for (int a = 0; a < naxes && a < INPUT_AXIS_COUNT; a++) {
        Sint16 cur = NormalizeAxisValue(a, SDL_GetJoystickAxis(joy, a));
        Sint16 prev = s_cap_prev_raw_axis[p][a];
        bool now = abs((int)cur) > threshold;
        bool was = abs((int)prev) > threshold;
        s_cap_prev_raw_axis[p][a] = cur;
        if (now && !was)
            return INPUT_XINPUT_BASE + (uint32_t)(p * 256 + (int)INPUT_AXIS_BASE + a);
    }
    return 0;
}

// Generic SDL gamepad button name.
static const char* generic_button_name(int sdl_btn) {
    for (int i = 0; i < SDL_BUTTON_COUNT; i++) {
        if (sdl_button_map[i].sdl == sdl_btn) return sdl_button_map[i].name;
    }
    if (sdl_btn == SDL_GAMEPAD_BUTTON_MISC1) return "Capture";
    return "Btn";
}

// The NSO N64 pad is exposed by SDL as a standard gamepad, but its physical
// buttons land in non-obvious standard slots.  These are the slots the Linux
// hid-nintendo driver documents for the same hardware:
//   0 A, 1 B, 2 C-Left, 3 C-Up, 4 C-Right, 9 L, 10 R, 6 Start,
//   left trigger = Z, right trigger = C-Down.
static const char* n64_button_name(int sdl_btn) {
    switch (sdl_btn) {
        case SDL_GAMEPAD_BUTTON_SOUTH:          return "A";
        case SDL_GAMEPAD_BUTTON_EAST:           return "B";
        case SDL_GAMEPAD_BUTTON_WEST:           return "C-Left";
        case SDL_GAMEPAD_BUTTON_NORTH:          return "C-Up";
        case SDL_GAMEPAD_BUTTON_BACK:           return "C-Right";
        case SDL_GAMEPAD_BUTTON_GUIDE:          return "Home";
        case SDL_GAMEPAD_BUTTON_START:          return "Start";
        case SDL_GAMEPAD_BUTTON_LEFT_STICK:     return "ZR";
        case SDL_GAMEPAD_BUTTON_RIGHT_STICK:    return "R3";
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:  return "L";
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "R";
        case SDL_GAMEPAD_BUTTON_DPAD_UP:        return "D-Up";
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:      return "D-Down";
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:      return "D-Left";
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:     return "D-Right";
        case SDL_GAMEPAD_BUTTON_MISC1:          return "Capture";
        default:                                return "Btn";
    }
}

static const char* axis_name(int axis, ControllerProfile profile) {
    if (profile == PROFILE_NSO_N64) {
        switch (axis) {
            case 0: return "Stick X";
            case 1: return "Stick Y";
            case 2: return "Right X";
            case 3: return "Right Y";
            case 4: return "Z";
            case 5: return "C-Down";
        }
    } else {
        switch (axis) {
            case 0: return "Left X";
            case 1: return "Left Y";
            case 2: return "Right X";
            case 3: return "Right Y";
            case 4: return "LT";
            case 5: return "RT";
        }
    }
    return "Axis";
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
        s_profile[i] = PROFILE_GENERIC;
    }
}

InputSystem& InputSystem::get() {
    static InputSystem inst;
    return inst;
}

void InputSystem::poll() {
    EnsureSDL();
    // SDL only refreshes joystick/gamepad state when its events are pumped.
    // This app uses a plain Win32 message loop, so nothing else calls this for
    // us - without it every SDL_Get*Button() query returns stale (all-zero)
    // state and controller capture appears dead.
    SDL_UpdateJoysticks();
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
        }

        // Read all six standard axes, whether this is a GameController or a
        // raw joystick.  Triggers are included: on the NSO N64 pad SDL exposes
        // Z and C-Down as the trigger axes, so they must still reach the core.
        for (int a = 0; a < INPUT_AXIS_COUNT; a++)
            this->g_analog[port][a] = ReadAxisNormalized(port, a);

        // The mapping may re-point the N64 stick at different physical axes.
        apply_analog_bindings((unsigned)port);
    }

    process_capture();
}

// Find the physical axis a slot is bound to, or -1 if it is not an axis.
static int FindBoundAxis(const PortMapping& m, EmuInputId id) {
    int slot = m.find_by_id(id);
    if (slot < 0) return -1;
    for (int k = 0; k < INPUT_KEY_SETS; k++) {
        uint32_t code = m.codes[slot][k];
        if (!INPUT_IS_GAMEPAD(code)) continue;
        uint32_t v = code - INPUT_XINPUT_BASE;
        int b = (int)(v % 256);
        if (b >= (int)INPUT_AXIS_BASE && b < (int)(INPUT_AXIS_BASE + INPUT_AXIS_COUNT))
            return b - (int)INPUT_AXIS_BASE;
    }
    return -1;
}

void InputSystem::apply_analog_bindings(unsigned port) {
    if (port >= INPUT_MAX_PORTS) return;
    int ax = FindBoundAxis(g_map[port], EMU_ID_ANALOG_LEFT_X);
    int ay = FindBoundAxis(g_map[port], EMU_ID_ANALOG_LEFT_Y);
    if (ax >= 0) g_analog[port][0] = ReadAxisNormalized((int)port, ax);
    if (ay >= 0) g_analog[port][1] = ReadAxisNormalized((int)port, ay);
}

// =============================================================================
// Input state query
// =============================================================================

int InputSystem::is_pressed(unsigned port, EmuInputId id) const {
    if (port >= INPUT_MAX_PORTS) return 0;

    // Check analog axes
    if (id >= EMU_ID_ANALOG_LEFT_X && id <= EMU_ID_TRIGGER_RIGHT) {
        int axis = id - EMU_ID_ANALOG_LEFT_X;
        if (axis >= 0 && axis < INPUT_AXIS_COUNT) return this->g_analog[port][axis] != 0 ? 1 : 0;
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
            // Trigger/stick axes bound as buttons.  The NSO N64's Z and
            // C-Down arrive here as the two trigger axes.
            if (p >= 0 && p < INPUT_MAX_PORTS &&
                b >= (int)INPUT_AXIS_BASE && b < (int)(INPUT_AXIS_BASE + INPUT_AXIS_COUNT)) {
                int axis = b - (int)INPUT_AXIS_BASE;
                if (abs((int)g_analog[p][axis]) > 20) return 1;
            }
            // Also support raw joystick codes (port*256 + 128 + button/hat)
            if (b >= 128 && b < 192) {
                int raw_b = b - 128;
                SDL_Joystick* joy = PortJoystick(p);
                if (joy && raw_b < SDL_GetNumJoystickButtons(joy)) {
                    return SDL_GetJoystickButton(joy, raw_b) ? 1 : 0;
                }
            }
            if (b >= 192 && b < 256) {
                int hat_raw = b - 192;
                int hat_idx = hat_raw / 4;
                int dir = hat_raw % 4;
                SDL_Joystick* joy = PortJoystick(p);
                if (joy && hat_idx < SDL_GetNumJoystickHats(joy)) {
                    Uint8 hat = SDL_GetJoystickHat(joy, hat_idx);
                    Uint8 mask = (dir == 0) ? SDL_HAT_UP : (dir == 1) ? SDL_HAT_DOWN : (dir == 2) ? SDL_HAT_LEFT : SDL_HAT_RIGHT;
                    return (hat & mask) ? 1 : 0;
                }
            }
        }
    }
    return turbo_edge(port, slot);
}

int InputSystem::analog_value(unsigned port, EmuInputId id) const {
    if (port >= INPUT_MAX_PORTS) return 0;
    if (id >= EMU_ID_ANALOG_LEFT_X && id <= EMU_ID_TRIGGER_RIGHT) {
        int axis = id - EMU_ID_ANALOG_LEFT_X;
        if (axis >= 0 && axis < INPUT_AXIS_COUNT) return g_analog[port][axis];
    }
    return is_pressed(port, id) ? 1 : 0;
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
    s_cap_has_snapshot = false;
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

    // Controllers: SDL gamepads first, then raw joystick data for buttons the
    // SDL mapping doesn't cover (and for plain joysticks).
    if (g_capture_type == 0 || g_capture_type == 2 || g_capture_type == 3) {
        EnsureSDL();

        // Ignore whatever is already held when capture begins.
        if (!s_cap_has_snapshot) {
            SnapshotCaptureState();
            s_cap_has_snapshot = true;
            return;
        }

        for (int p = 0; p < INPUT_MAX_PORTS; p++) {
            uint32_t code = 0;

            SDL_Gamepad* gp = s_gamepads[p];
            if (gp && g_capture_type != 3) {
                for (int b = 0; b < 16; b++) {
                    bool pressed = SDL_GetGamepadButton(gp, (SDL_GamepadButton)b);
                    if (pressed && !s_cap_prev_gp[p][b]) {
                        code = INPUT_XINPUT_BASE + (uint32_t)(p * 256 + b);
                        break;
                    }
                    s_cap_prev_gp[p][b] = pressed;
                }
            }

            if (!code && gp) code = PollGamepadAxisCapture(p, gp);

            if (!code) {
                SDL_Joystick* joy = PortJoystick(p);
                if (joy) {
                    if (!gp) code = PollRawAxisCapture(p, joy);
                    if (!code && g_capture_type != 3) code = PollRawJoystickCapture(p, joy);
                }
            }

            if (code) {
                DebugLog("[Capture] bound port=%d slot=%d code=0x%x\n",
                         g_capture_port, g_capture_slot, code);
                PortMapping& m = g_map[g_capture_port];
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
            } else if (b >= (int)INPUT_AXIS_BASE && b < (int)(INPUT_AXIS_BASE + INPUT_AXIS_COUNT)) {
                int axis = b - (int)INPUT_AXIS_BASE;
                if (abs((int)g_analog[p][axis]) > 20) held = 1;
            } else if (b >= 128 && b < 160) {
                int raw_b = b - 128;
                SDL_Joystick* joy = PortJoystick(p);
                if (joy && raw_b < SDL_GetNumJoystickButtons(joy)) {
                    held = SDL_GetJoystickButton(joy, raw_b) ? 1 : 0;
                }
            } else if (b >= 192 && b < 256) {
                int hat_raw = b - 192;
                int hat_idx = hat_raw / 4;
                int dir = hat_raw % 4;
                SDL_Joystick* joy = PortJoystick(p);
                if (joy && hat_idx < SDL_GetNumJoystickHats(joy)) {
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

const char* InputSystem::describe(unsigned port, uint32_t code, char* buf, size_t bufSize) {
    (void)port;
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
        ControllerProfile prof = (p >= 0 && p < INPUT_MAX_PORTS) ? s_profile[p] : PROFILE_GENERIC;

        if (b >= 0 && b < 16) {
            const char* name = (prof == PROFILE_NSO_N64) ? n64_button_name(b)
                                                         : generic_button_name(b);
            snprintf(buf, bufSize, "Joy%d %s", p + 1, name);
            return buf;
        } else if (b >= (int)INPUT_AXIS_BASE && b < (int)(INPUT_AXIS_BASE + INPUT_AXIS_COUNT)) {
            // Trigger/stick axis bound as a button
            snprintf(buf, bufSize, "Joy%d %s", p + 1,
                     axis_name(b - (int)INPUT_AXIS_BASE, prof));
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
    if (port >= INPUT_MAX_PORTS) return;
    EnsureSDL();
    SDL_UpdateJoysticks();
    UpdateGamepadList();

    PortMapping& m = g_map[port];
    m.clear();

    auto btn_code = [&](SDL_GamepadButton b) -> uint32_t {
        return INPUT_XINPUT_BASE + (uint32_t)(port * 256 + (int)b);
    };
    auto axis_code = [&](int a) -> uint32_t {
        return INPUT_XINPUT_BASE + (uint32_t)(port * 256 + (int)INPUT_AXIS_BASE + a);
    };

    if (console == EMU_CONSOLE_NINTENDO_64) {
        // Keyboard defaults chosen around the N64's own button names.
        struct { EmuInputId id; uint32_t code; } kb[] = {
            { EMU_ID_UP,      VK_UP },   { EMU_ID_DOWN,  VK_DOWN },
            { EMU_ID_LEFT,    VK_LEFT }, { EMU_ID_RIGHT, VK_RIGHT },
            { EMU_ID_A,       'X' },     { EMU_ID_B,     'Z' },
            { EMU_ID_C_UP,    'S' },     { EMU_ID_C_DOWN,'C' },
            { EMU_ID_C_LEFT,  'A' },     { EMU_ID_C_RIGHT,'D' },
            { EMU_ID_L,       'Q' },     { EMU_ID_R,     'W' },
            { EMU_ID_Y,       'E' },     // Z trigger
            { EMU_ID_START,   VK_RETURN },
        };
        for (auto& d : kb) m.set_by_id(d.id, d.code, 0);

        if (s_profile[port] == PROFILE_NSO_N64) {
            // SDL's native N64 layout.  Its C-Up/C-Left/C-Right are ordinary
            // buttons; Z and C-Down are the left/right trigger axes; the stick
            // is the left stick.  This mirrors hid-nintendo's mapping.
            struct { EmuInputId id; uint32_t code; } pad[] = {
                { EMU_ID_A,             btn_code(SDL_GAMEPAD_BUTTON_SOUTH) },
                { EMU_ID_B,             btn_code(SDL_GAMEPAD_BUTTON_EAST) },
                { EMU_ID_C_UP,          btn_code(SDL_GAMEPAD_BUTTON_NORTH) },
                { EMU_ID_C_LEFT,        btn_code(SDL_GAMEPAD_BUTTON_WEST) },
                { EMU_ID_C_RIGHT,       btn_code(SDL_GAMEPAD_BUTTON_BACK) },
                { EMU_ID_C_DOWN,        axis_code(5) },  // right trigger
                { EMU_ID_Y,             axis_code(4) },  // Z = left trigger
                { EMU_ID_L,             btn_code(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) },
                { EMU_ID_R,             btn_code(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) },
                { EMU_ID_START,         btn_code(SDL_GAMEPAD_BUTTON_START) },
                { EMU_ID_UP,            btn_code(SDL_GAMEPAD_BUTTON_DPAD_UP) },
                { EMU_ID_DOWN,          btn_code(SDL_GAMEPAD_BUTTON_DPAD_DOWN) },
                { EMU_ID_LEFT,          btn_code(SDL_GAMEPAD_BUTTON_DPAD_LEFT) },
                { EMU_ID_RIGHT,         btn_code(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) },
                { EMU_ID_ANALOG_LEFT_X, axis_code(0) },
                { EMU_ID_ANALOG_LEFT_Y, axis_code(1) },
            };
            for (auto& d : pad) m.set_by_id(d.id, d.code, 1);
        } else {
            // Generic pad: keep the familiar face-button layout.  The dialog
            // can bind the remaining C buttons and Z to whatever is free.
            struct { EmuInputId id; SDL_GamepadButton b; } pad[] = {
                { EMU_ID_A,      SDL_GAMEPAD_BUTTON_SOUTH },
                { EMU_ID_B,      SDL_GAMEPAD_BUTTON_EAST },
                { EMU_ID_C_UP,   SDL_GAMEPAD_BUTTON_NORTH },
                { EMU_ID_Y,      SDL_GAMEPAD_BUTTON_WEST },   // Z
                { EMU_ID_L,      SDL_GAMEPAD_BUTTON_LEFT_SHOULDER },
                { EMU_ID_R,      SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER },
                { EMU_ID_START,  SDL_GAMEPAD_BUTTON_START },
                { EMU_ID_UP,     SDL_GAMEPAD_BUTTON_DPAD_UP },
                { EMU_ID_DOWN,   SDL_GAMEPAD_BUTTON_DPAD_DOWN },
                { EMU_ID_LEFT,   SDL_GAMEPAD_BUTTON_DPAD_LEFT },
                { EMU_ID_RIGHT,  SDL_GAMEPAD_BUTTON_DPAD_RIGHT },
            };
            for (auto& d : pad) {
                m.set_by_id(d.id, btn_code(d.b), 1);
            }
            m.set_by_id(EMU_ID_ANALOG_LEFT_X, axis_code(0), 1);
            m.set_by_id(EMU_ID_ANALOG_LEFT_Y, axis_code(1), 1);
        }
        return;
    }

    struct { EmuInputId id; uint32_t code; } defaults[] = {
        { EMU_ID_UP,    VK_UP }, { EMU_ID_DOWN,  VK_DOWN },
        { EMU_ID_LEFT,  VK_LEFT }, { EMU_ID_RIGHT, VK_RIGHT },
        { EMU_ID_A,     'X' }, { EMU_ID_B, 'Z' },
        { EMU_ID_X,     'S' }, { EMU_ID_Y, 'A' },
        { EMU_ID_L,     'Q' }, { EMU_ID_R, 'W' },
        { EMU_ID_START, VK_RETURN }, { EMU_ID_SELECT, VK_SHIFT },
    };
    for (auto& d : defaults) m.set_by_id(d.id, d.code, 0);

    // GameController defaults for key set 1
    if (s_gamepads[port]) {
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
        for (auto& entry : pad_map)
            m.set_by_id(entry.id, btn_code(entry.sdl_btn), 1);
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
