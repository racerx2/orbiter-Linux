// Linux <winuser.h> — forwards to windows.h.
//
// Src/Orbiter/TabExtra.cpp includes this directly. In the Windows SDK
// winuser.h is the window-management portion that windows.h itself pulls in,
// so everything it declares is already provided here.

#ifndef ORBITER_LINUX_WINUSER_H
#define ORBITER_LINUX_WINUSER_H

#ifdef _WIN32
#error "This header is for non-Windows builds only."
#endif

#include <windows.h>

#endif // ORBITER_LINUX_WINUSER_H
