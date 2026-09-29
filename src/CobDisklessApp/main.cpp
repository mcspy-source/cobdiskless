// SPDX-License-Identifier: GPL-3.0-or-later

#include "admin_app.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

/**
 * @brief Disable Windows console QuickEdit so a stray click cannot stall us.
 *
 * A single left-click anywhere in the console window flips the input handle's
 * ENABLE_QUICK_EDIT_MODE flag on, and that flag makes ReadConsoleInput() *block*
 * the whole console input buffer. Two threads then stall: any logger doing a
 * stdout flush parks inside that same handle, and -- because the console host
 * routes window messages through the process attached to the buffer -- the Win32
 * message pump stops draining, so Task Manager starts reporting
 * "(Not Responding)". The fix is one flag; the symptom is a server that looks
 * hung because someone clicked a terminal.
 *
 * ENABLE_EXTENDED_FLAGS must be set whenever ENABLE_QUICK_EDIT_MODE is cleared,
 * otherwise the console host ignores the change.
 */
void disable_console_quick_edit() {
#ifdef _WIN32
    const HANDLE hInput = GetStdHandle(STD_INPUT_HANDLE);
    // No console at all (redirected stdio, service, or a GUI subsystem child):
    // GetStdHandle returns NULL/INVALID_HANDLE_VALUE and there is nothing to fix.
    if (hInput == INVALID_HANDLE_VALUE || hInput == nullptr) {
        return;
    }
    DWORD mode = 0;
    if (!GetConsoleMode(hInput, &mode)) {
        return; // not a console handle -- already immune
    }
    const DWORD desired = (mode & ~ENABLE_QUICK_EDIT_MODE) | ENABLE_EXTENDED_FLAGS;
    (void)SetConsoleMode(hInput, desired);
#endif
}

} // namespace

int main(int argc, char* argv[]) {
    disable_console_quick_edit();
    cobdiskless::AdminApp app;
    return app.run(argc, argv);
}
