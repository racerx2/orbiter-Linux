// Linux <winres.h> — the header a resource script includes.
//
// On Windows this pulls in the Win32 constants a .rc needs (window and control
// styles, dialog styles, the standard control ids) plus afxres.h in MFC
// projects. Several .rc files in this tree include it:
//
//     OVP/D3D9Client/samples/DrawOrbits/Orbits.rc
//     OVP/D3D9Client/samples/DX9ExtMFD/ExtMFD.rc
//
// Without it the preprocessor pass in rcstrings.py and rc2cpp.py aborts on the
// first line and the whole script yields nothing -- which is why those two
// modules had no category and fell to "Miscellaneous".
//
// windows.h already carries every style and message constant the scripts
// reference, and it is guarded against RC_INVOKED use, so this simply forwards
// to it.

#ifndef ORBITER_LINUX_WINRES_H
#define ORBITER_LINUX_WINRES_H

#ifdef _WIN32
#error "This header is for non-Windows builds only."
#endif

#include <windows.h>
#include <commctrl.h>

// IDC_STATIC is the id every UNNAMED static in a template carries, and the
// Windows SDK defines it here -- winres.h includes winresrc.h, which carries
//
//     #ifndef IDC_STATIC
//     #define IDC_STATIC              (-1)
//     #endif
//
// A .rc that has any label, frame or group box written by the dialog editor
// references it, and DeltaGlider.rc does so 27 times. Without the definition
// rc2cpp.py's preprocessor pass raises UnresolvedSymbol on the first one and
// converts nothing, which is the same silent no-templates state as not
// running the converter at all.
//
// The guard is the SDK's own: a resource.h that defines IDC_STATIC itself --
// which is what Visual Studio writes into a fresh project -- still wins,
// because the .rc includes it first.
#ifndef IDC_STATIC
#define IDC_STATIC (-1)
#endif

#endif // ORBITER_LINUX_WINRES_H
