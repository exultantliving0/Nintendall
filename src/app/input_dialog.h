// =============================================================================
//  Nintendall - the "Controller Configuration" window
//
//  This follows Mesen's controller configuration screen closely: an info banner
//  across the top, "Key Set" tabs beneath it (each tab is one of four possible
//  bindings for a button), the pad drawn in the middle on a white panel, and a
//  footer with preset/clear buttons, the turbo slider and OK/Cancel.
//
//  The tab strip and the slider are drawn by hand rather than using a native
//  TABCONTROL and TRACKBAR32.  Both are comctl32 classes that are not reliably
//  registered on a plain MinGW build without a v6 side-by-side manifest, and
//  drawing them also matches Mesen better - its tabs are flat text with an
//  underline, not chunky native tabs.
//
//  Clicking a binding box arms it; the next key or gamepad button pressed is
//  written into it and the box highlights while it waits.  Holding a mapped key
//  turns its box green, exactly as Mesen does, so it is obvious at a glance
//  which controls are live.
// =============================================================================

#ifndef NINTENDALL_INPUT_DIALOG_H
#define NINTENDALL_INPUT_DIALOG_H

#include <windows.h>
#include "emu_api.h"

// Shows the mapping window for the given console.  Returns true when the user
// accepted, in which case the caller should save the configuration.
bool ShowInputDialog(HWND parent, EmuConsoleType console);

#endif  // NINTENDALL_INPUT_DIALOG_H