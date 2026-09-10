// Linux <windowsx.h> — the message-cracker helpers Orbiter uses.
//
// In the Windows SDK this is a convenience header of macros over windows.h,
// most commonly GET_X_LPARAM / GET_Y_LPARAM for unpacking mouse coordinates
// out of an LPARAM. It declares no new API of its own.
//
// The coordinates are SIGNED: a mouse position can be negative when the
// pointer is captured and dragged outside the window, so the low and high
// words must be sign-extended rather than taken as unsigned. Using LOWORD
// directly here is the classic Win32 bug.

#ifndef ORBITER_LINUX_WINDOWSX_H
#define ORBITER_LINUX_WINDOWSX_H

#ifdef _WIN32
#error "This header is for non-Windows builds only."
#endif

#include <windows.h>

#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#define GET_WHEEL_DELTA_WPARAM(wp) ((short)HIWORD(wp))
#define GET_KEYSTATE_WPARAM(wp)    ((int)LOWORD(wp))

#define HANDLE_WM_COMMAND(hwnd, wParam, lParam, fn) \
    ((fn)((hwnd), (int)(LOWORD(wParam)), (HWND)(lParam), (UINT)HIWORD(wParam)), 0L)

#endif // ORBITER_LINUX_WINDOWSX_H
