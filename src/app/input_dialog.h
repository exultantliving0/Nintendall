// =============================================================================
//  Nintendall - the "Controller Configuration" window
// =============================================================================

#ifndef NINTENDALL_INPUT_DIALOG_H
#define NINTENDALL_INPUT_DIALOG_H

#include <windows.h>
#include "emu_api.h"

// Shows the mapping window for the given console.  Returns true when the user
// accepted, in which case the caller should save the configuration.
bool ShowInputDialog(HWND parent, EmuConsoleType console);

#endif  // NINTENDALL_INPUT_DIALOG_H
