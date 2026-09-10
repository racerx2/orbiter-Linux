// Linux <zmouse.h> — mouse-wheel constants.
//
// Included by Src/Orbiter/Panel2D.cpp and Src/Orbiter/Camera.cpp. On Windows
// this header predates the wheel being part of the core SDK; it only supplies
// the wheel notch size and a few scroll constants.
//
// WHEEL_DELTA is the quantum of one detent. Orbiter divides the wheel value in
// WM_MOUSEWHEEL by it, so the value must stay 120 for the input layer to
// report the same number of notches as it does on Windows.

#ifndef ORBITER_LINUX_ZMOUSE_H
#define ORBITER_LINUX_ZMOUSE_H

#ifdef _WIN32
#error "This header is for non-Windows builds only."
#endif

#include <windows.h>

#define WHEEL_DELTA       120
#define WHEEL_PAGESCROLL  ((UINT)-1)

#define MSH_MOUSEWHEEL    "MSWHEEL_ROLLMSG"
#define MOUSEZ_CLASSNAME  "MouseZ"
#define MOUSEZ_TITLE      "Magellan MSWHEEL"

#define MSH_WHEELMODULE_CLASS MOUSEZ_CLASSNAME
#define MSH_WHEELMODULE_TITLE MOUSEZ_TITLE

#define MSH_SCROLL_LINES  "MSH_SCROLL_LINES_MSG"

#endif // ORBITER_LINUX_ZMOUSE_H
