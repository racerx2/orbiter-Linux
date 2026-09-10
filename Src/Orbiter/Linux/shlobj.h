// Linux <shlobj.h> — shell folder locations.
//
// Orbiter uses this to find the user's documents folder for saved flights and
// screenshots. The Linux equivalent is the XDG base directory specification:
// $XDG_DOCUMENTS_DIR when set, otherwise $HOME. SHGetFolderPath is implemented
// in Src/Orbiter/Linux/Shell.cpp over that.

#ifndef ORBITER_LINUX_SHLOBJ_H
#define ORBITER_LINUX_SHLOBJ_H

#ifdef _WIN32
#error "This header is for non-Windows builds only."
#endif

#include <windows.h>

// CSIDL values for the folders this tree asks for.
#define CSIDL_PERSONAL      0x0005   // My Documents
#define CSIDL_APPDATA       0x001a   // Roaming application data
#define CSIDL_LOCAL_APPDATA 0x001c   // Local application data
#define CSIDL_MYPICTURES    0x0027   // My Pictures
#define CSIDL_PROFILE       0x0028   // User profile root
#define CSIDL_FLAG_CREATE   0x8000   // create the folder if absent

#define SHGFP_TYPE_CURRENT  0
#define SHGFP_TYPE_DEFAULT  1

ORB_EXTERN_C_BEGIN

HRESULT SHGetFolderPathA (HWND hwnd, int csidl, HANDLE token, DWORD flags,
                          LPSTR path);

ORB_EXTERN_C_END

#define SHGetFolderPath SHGetFolderPathA

#endif // ORBITER_LINUX_SHLOBJ_H
