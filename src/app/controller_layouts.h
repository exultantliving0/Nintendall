// =============================================================================
//  Nintendall - controller artwork for the mapping dialog
//
//  This follows the look of Mesen's controller configuration screen: rather
//  than a picture of a pad, it draws pale grey housings (an ellipse around the
//  d-pad, another around the face buttons) with a small binding box per button
//  and the console's own name beside it.  The binding boxes are sized to their
//  text, which is why they read as "Joy1 DPad Up" on a wide pad and "X" on a
//  narrow one.
//
//  All coordinates are in a fixed design space that the dialog scales to fit,
//  so the layout is resolution independent and needs no bitmap assets.
//
//  Mesen only draws NES, SNES and Vs pads.  The N64 layout here is built to
//  match the same visual language - same housings, same box style - but shows
//  the twelve inputs the N64 core actually reads, which is A, B, X, Y (its two
//  C buttons), L, R, Start, Select and the d-pad.
// =============================================================================

#ifndef NINTENDALL_CONTROLLER_LAYOUTS_H
#define NINTENDALL_CONTROLLER_LAYOUTS_H

#include <windows.h>
#include "emu_api.h"

// Where a button's name sits relative to its binding box.
enum LabelPos { LBL_NONE = 0, LBL_ABOVE, LBL_BELOW, LBL_LEFT, LBL_RIGHT };

// A pale grey shape behind the buttons - the d-pad ring, the face-button ring.
// `kind` is 0 for a rounded rectangle and 1 for an ellipse, drawn in the pale
// grey Mesen uses.  Kinds 2 and 3 are the same shapes in dark grey, which is
// how a real NES pad is coloured: a pale shell around a dark face.
struct PadHousing {
    int kind;            // 0 = rounded rect, 1 = ellipse, 2/3 = dark versions
    int x, y, w, h;      // design-space bounds
};

// One bindable button: a box, optionally a turbo box below it, and a name.
// Mesen gives each box a fixed width chosen in its designer rather than sizing
// it to the text, which is what lets the layout guarantee nothing collides;
// `w` is that width, and text that does not fit is clipped.
struct PadButton {
    EmuInputId  id;
    int         cx, cy;         // centre of the binding box
    int         w, h;           // box size, in design units
    int         turbo_dy;       // offset to the turbo box; 0 when unused
    const char* name;            // the console's name for this button
    int         lx, ly;         // where that name is drawn
    LabelPos    lpos;
};

struct PadLayout {
    const char*     name;
    int             design_w, design_h;
    const PadHousing* housings;
    int             housing_count;
    const PadButton* buttons;
    int             button_count;
    int             has_turbo;   // show Turbo boxes and the Turbo Speed slider
};

const PadLayout* pad_layout_for(EmuConsoleType console);

// The colour scheme, matching Mesen's screen.
struct PadColors {
    COLORREF panel, housing_fill, housing_line;
    COLORREF box_fill,   box_line,   box_text;
    COLORREF label_text;
    COLORREF down_fill,  down_line;  // while the button is held
    COLORREF listen_fill, listen_line;
    COLORREF dark_fill,  dark_line;  // a real NES pad's dark face
};

const PadColors* pad_colors();

// Width a binding box needs for the given text, measured with `font`.  The
// result is in the same units as the font, i.e. design units when the unscaled
// font is passed.
int pad_box_width(HDC dc, HFONT font, const char* text, int min_w);

// On-screen rectangle of a button's box.  `width_design` comes from
// pad_box_width; `dy` shifts it (used for the turbo box).
void pad_box_rect(const PadLayout* pad, int index, int width_design,
                  int ox, int oy, float scale, int dy, RECT* out);

// True when the given button is currently held, for the highlight.
typedef int (*PadPressedFn)(void* ctx, EmuInputId id);

// Paints the controller.  `texts` and `turbo_texts` hold one entry per button
// (either may be null); the two listening indices are -1 when nothing is armed.
void pad_paint_state(HDC dc, HFONT box_font, const PadLayout* pad,
                     int ox, int oy, float scale,
                     PadPressedFn pressed, void* ctx,
                     const char* const* texts, const char* const* turbo_texts,
                     int listening_index, int turbo_listening_index);

// The button under a client-space point, or nullptr.  `widths`/`turbo_widths`
// must be the same arrays handed to pad_paint_state.
const PadButton* pad_hit_test(const PadLayout* pad, const int* widths,
                              const int* turbo_widths,
                              float px, float py, int ox, int oy, float scale);

#endif  // NINTENDALL_CONTROLLER_LAYOUTS_H