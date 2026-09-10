// Linux <Uxtheme.h> — Windows visual styles.
//
// Included by Src/Orbiter/Launchpad.cpp and Src/Orbiter/OptionsPages.cpp. The
// whole tree uses exactly one entry point, EnableThemeDialogTexture, at two
// call sites.
//
// On Windows that call paints a property-sheet page's background with the tab
// control's texture so a child dialog blends into the tab it sits on. It is
// purely cosmetic and has no analogue outside the Windows theming engine, so
// it succeeds and does nothing. The launchpad's appearance on Linux comes from
// the ImGui styling instead.
//
// Note the include spelling: the two sources write "Uxtheme.h" with a capital
// U while the Windows SDK ships uxtheme.h lowercase. Windows resolves either;
// this file uses the capitalised name and a lowercase symlink covers the rest.

#ifndef ORBITER_LINUX_UXTHEME_H
#define ORBITER_LINUX_UXTHEME_H

#ifdef _WIN32
#error "This header is for non-Windows builds only."
#endif

#include <windows.h>

typedef struct HTHEME__ *HTHEME;

// EnableThemeDialogTexture flags
#define ETDT_DISABLE       0x00000001
#define ETDT_ENABLE        0x00000002
#define ETDT_USETABTEXTURE 0x00000004
#define ETDT_ENABLETAB     (ETDT_ENABLE | ETDT_USETABTEXTURE)

inline HRESULT EnableThemeDialogTexture(HWND, DWORD) { return S_OK; }

inline BOOL IsAppThemed(void)    { return FALSE; }
inline BOOL IsThemeActive(void)  { return FALSE; }

#endif // ORBITER_LINUX_UXTHEME_H
