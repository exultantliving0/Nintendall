// =============================================================================
//  Nintendall - input: gamepad polling, the button-mapping store and turbo
//
//  Physical inputs are identified by a single 32-bit code:
//
//      0                      unbound
//      1 .. 0x1FF             keyboard virtual-key code
//      0x1FF + n              mouse button n
//      0xFFFF + port*0x100+n  SDL controller port, where n is:
//                               0..15   GameController buttons
//                               64..69  GameController axes
//                               128..159 raw joystick buttons
//                               192..255 raw joystick hats
//
//  Anything at or above 0xFFFF counts as "a gamepad", which is how the mapping
//  dialog decides what to label things.
//
//  Each emulated button carries INPUT_KEY_SETS independent bindings - that is
//  Mesen's "Key Set" tabs, and it is what lets one button respond to, say, both
//  Space and a gamepad's X at the same time.  Every button may additionally have
//  a turbo binding that fires it automatically on a timer.
// =============================================================================

#ifndef NINTENDALL_INPUT_CONFIG_H
#define NINTENDALL_INPUT_CONFIG_H

#include <windows.h>

#include "emu_api.h"

// Debug logging function (defined in input_config.cpp)
void DebugLog(const char* fmt, ...);

#define INPUT_MAX_PORTS   4
#define INPUT_MAX_BUTTONS 20
#define INPUT_KEY_SETS    4       // Mesen's "Key Set #1..#4"
#define INPUT_TURBO_MIN   2       // frames between auto-presses at Slow
#define INPUT_TURBO_MAX   11      // ...and at Fast

// --- physical input codes ---------------------------------------------------
#define INPUT_NONE             0u
#define INPUT_IS_KEYBOARD(c)   ((c) > 0 && (c) <= 0x1FF)
#define INPUT_IS_MOUSE(c)      ((c) > 0x1FF && (c) < 0xFFFF)
#define INPUT_IS_GAMEPAD(c)    ((c) >= 0xFFFF)
#define INPUT_MOUSE_BASE       0x1FFu
#define INPUT_XINPUT_BASE      0xFFFFu
#define INPUT_DINPUT_BASE      0x11000u

// Gamepad axis bindings live in the otherwise-unused 64..127 part of the
// per-port byte.  They let triggers (and sticks) be bound like buttons, which
// matters for controllers such as the NSO N64 pad: SDL reports its Z and
// C-Down as the left/right trigger axes.
#define INPUT_AXIS_BASE        64u
#define INPUT_AXIS_COUNT       6

// One console's controller.  Slots are addressed by EmuInputId, so a given
// button has exactly one row however many keys are bound to it.
struct PortMapping {
    EmuInputId buttons[INPUT_MAX_BUTTONS];          // slot -> emulated button
    uint32_t   codes[INPUT_MAX_BUTTONS][INPUT_KEY_SETS];  // slot, key set -> code
    uint32_t   turbo[INPUT_MAX_BUTTONS];           // slot -> turbo code
    int        turbo_on[INPUT_MAX_BUTTONS];         // slot -> turbo enabled
    int        used[INPUT_MAX_BUTTONS];             // slot -> in use

    int count() const;
    void clear();
    int  find_by_id(EmuInputId id) const;
    void set_by_id(EmuInputId id, uint32_t code, int key_set = 0);
    void set_turbo(EmuInputId id, uint32_t code);
    bool has_binding(EmuInputId id) const;
};

// XInput button masks, so the dialog can name them.
enum {
    XBTN_DPAD_UP = 0x0001, XBTN_DPAD_DOWN = 0x0002,
    XBTN_DPAD_LEFT = 0x0004, XBTN_DPAD_RIGHT = 0x0008,
    XBTN_START = 0x0010, XBTN_BACK = 0x0020,
    XBTN_LEFT_THUMB = 0x0040, XBTN_RIGHT_THUMB = 0x0080,
    XBTN_LEFT_SHOULDER = 0x0100, XBTN_RIGHT_SHOULDER = 0x0200,
    XBTN_GUIDE = 0x0400, XBTN_MISC = 0x0800,
    XBTN_LEFT_TRIGGER = 0x1000, XBTN_RIGHT_TRIGGER = 0x2000,
};

// Order matters: the index is the XInput bit index used to build the code.
struct XInputButtonDef { WORD mask; const char* name; };
extern const XInputButtonDef XINPUT_BUTTONS[];
extern const int XINPUT_BUTTON_COUNT;

class InputSystem {
public:
    static InputSystem& get();

    // Called once per frame, before the core asks about buttons.
    void poll();

    // 1 when the given emulated button is currently held (or firing on turbo).
    int is_pressed(unsigned port, EmuInputId id) const;

    // Signed value (-128..127) for analog axes; digital inputs return 0/1.
    // Cores must use this rather than is_pressed() for EMU_ID_ANALOG_*.
    int analog_value(unsigned port, EmuInputId id) const;

    // True while turbo is firing this frame, so the dialog can flash the button.
    int turbo_firing(unsigned port, EmuInputId id) const;

    // Human-readable name for a physical input code, for the dialog.  The port
    // selects the controller-specific names (e.g. C-Up on an NSO N64 pad).
    static const char* describe(unsigned port, uint32_t code, char* buf, size_t bufSize);

    // 1 when any XInput pad is connected.
    int connected_pads() const;

    PortMapping&       mapping(unsigned port)       { return g_map[port & 3]; }
    const PortMapping& mapping(unsigned port) const { return g_map[port & 3]; }

    // --- the remapping dialog's capture mode --------------------------------
    // While armed, the next physical input seen is offered to the pending slot.
    int  capture_port() const { return g_capture_port; }
    int  capture_slot() const { return g_capture_slot; }
    bool capturing()  const { return g_capture_port >= 0; }

    // Which key set a new binding lands in; the dialog's "Key Set #N" tabs set
    // this before arming a capture.
    void set_capture_key_set(int k) {
        if (k >= 0 && k < INPUT_KEY_SETS) g_capture_key_set = k;
    }
    // Capture type: 0 = any input, 1 = keyboard/mouse only, 2 = gamepad only
    void set_capture_type(int t) { g_capture_type = t; }
    int capture_type() const { return g_capture_type; }
    // Whether the armed slot is binding its turbo key rather than a key set.
    void set_capture_turbo(bool t) { g_capture_turbo = t ? 1 : 0; }
    bool capturing_turbo() const { return g_capture_turbo != 0; }
    void begin_capture(unsigned port, int slot);
    void cancel_capture();
    // Process pending capture (called from poll or dialog)
    void process_capture();

    // --- rumble / haptics ---------------------------------------------------
    void set_rumble(unsigned port, uint16_t low_freq, uint16_t high_freq);
    void stop_rumble(unsigned port);

    // --- turbo --------------------------------------------------------------
    int  turbo_speed() const { return g_turbo_speed; }
    void set_turbo_speed(int s);
    int  turbo_period() const { return INPUT_TURBO_MAX + 1 - g_turbo_speed; }

    // --- persistence --------------------------------------------------------
    bool load(const char* path);
    bool save(const char* path) const;

    // Per-console defaults.
    void apply_defaults(EmuConsoleType console, unsigned port);
    void apply_all_defaults(EmuConsoleType console);

    // The console currently running.  Turbo only fires on the machines that
    // actually had it, so a mapping saved for one console cannot autofire on
    // another.
    void set_active_console(EmuConsoleType c) { g_console = c; }

    // True when the given console can use turbo/autofire at all.
    static bool console_has_turbo(EmuConsoleType console);

private:
    InputSystem();
    PortMapping g_map[INPUT_MAX_PORTS];
    int8_t      g_analog[INPUT_MAX_PORTS][6];     // analog axes: LX, LY, RX, RY, LT, RT
    int         g_capture_port = -1;
    int         g_capture_slot = -1;
    int         g_capture_key_set = 0;
    int         g_capture_type  = 0;  // 0 any, 1 keyboard, 2 gamepad, 3 axis only
    int         g_capture_turbo  = 0;
    int         g_turbo_speed   = 6;
    EmuConsoleType g_console     = EMU_CONSOLE_NES;
    unsigned    g_frame         = 0;

    // True when the slot's turbo should fire on the current frame.
    int turbo_edge(unsigned port, int slot) const;

    // Re-point the left analog axes at whatever physical axes the user bound
    // to EMU_ID_ANALOG_LEFT_X/Y (defaults to axes 0/1).
    void apply_analog_bindings(unsigned port);
};

#endif  // NINTENDALL_INPUT_CONFIG_H