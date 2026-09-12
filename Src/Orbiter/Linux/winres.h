// Linux <winres.h> — the header a resource script includes.
//
// On Windows this pulls in the Win32 constants a .rc needs (window and control
// styles, dialog styles, the standard control ids). Without it the
// preprocessor pass in rcstrings.py and rc2cpp.py aborts on the first line and
// the whole script silently yields nothing. windows.h already carries every
// style and message constant the scripts reference and is guarded against
// RC_INVOKED use, so this simply forwards to it.

#ifndef ORBITER_LINUX_WINRES_H
#define ORBITER_LINUX_WINRES_H

#ifdef _WIN32
#error "This header is for non-Windows builds only."
#endif

#include <windows.h>
#include <commctrl.h>

// IDC_STATIC is the id every unnamed static in a dialog template carries; the
// Windows SDK defines it as (-1) in winresrc.h. Every label, frame and group
// box the dialog editor writes references it. Without the definition
// rc2cpp.py's preprocessor pass raises UnresolvedSymbol on the first one and
// converts nothing -- the same silent no-templates state as not running the
// converter at all.
//
// The guard is the SDK's own: a resource.h that defines IDC_STATIC itself
// still wins, because the .rc includes it first.
#ifndef IDC_STATIC
#define IDC_STATIC (-1)
#endif

#endif // ORBITER_LINUX_WINRES_H
