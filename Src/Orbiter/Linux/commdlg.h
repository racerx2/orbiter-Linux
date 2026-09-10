// Linux <commdlg.h> — the Win32 common dialogs.
//
// TerrainToolKit uses GetOpenFileName/GetSaveFileName to pick a tile or an
// image to import. The structure layout matters because callers fill it field
// by field and pass it by pointer; the entry points are implemented in
// Platform.cpp against a portal file chooser.

#ifndef ORBITER_LINUX_COMMDLG_H
#define ORBITER_LINUX_COMMDLG_H

#ifdef _WIN32
#error "This header is for non-Windows builds only."
#endif

#include <windows.h>

typedef struct tagOFNA {
    DWORD         lStructSize;
    HWND          hwndOwner;
    HINSTANCE     hInstance;
    LPCSTR        lpstrFilter;
    LPSTR         lpstrCustomFilter;
    DWORD         nMaxCustFilter;
    DWORD         nFilterIndex;
    LPSTR         lpstrFile;
    DWORD         nMaxFile;
    LPSTR         lpstrFileTitle;
    DWORD         nMaxFileTitle;
    LPCSTR        lpstrInitialDir;
    LPCSTR        lpstrTitle;
    DWORD         Flags;
    WORD          nFileOffset;
    WORD          nFileExtension;
    LPCSTR        lpstrDefExt;
    LPARAM        lCustData;
    void         *lpfnHook;
    LPCSTR        lpTemplateName;
    void         *pvReserved;
    DWORD         dwReserved;
    DWORD         FlagsEx;
} OPENFILENAMEA, *LPOPENFILENAMEA;

typedef OPENFILENAMEA OPENFILENAME;
typedef LPOPENFILENAMEA LPOPENFILENAME;

// OPENFILENAME.Flags
#define OFN_READONLY             0x00000001
#define OFN_OVERWRITEPROMPT      0x00000002
#define OFN_HIDEREADONLY         0x00000004
#define OFN_NOCHANGEDIR          0x00000008
#define OFN_SHOWHELP             0x00000010
#define OFN_ALLOWMULTISELECT     0x00000200
#define OFN_PATHMUSTEXIST        0x00000800
#define OFN_FILEMUSTEXIST        0x00001000
#define OFN_CREATEPROMPT         0x00002000
#define OFN_NOTESTFILECREATE     0x00010000
#define OFN_NONETWORKBUTTON      0x00020000
#define OFN_EXPLORER             0x00080000
#define OFN_NODEREFERENCELINKS   0x00100000
#define OFN_ENABLESIZING         0x00800000

ORB_EXTERN_C_BEGIN

BOOL  GetOpenFileNameA (LPOPENFILENAMEA ofn);
BOOL  GetSaveFileNameA (LPOPENFILENAMEA ofn);
DWORD CommDlgExtendedError (void);

ORB_EXTERN_C_END

#define GetOpenFileName GetOpenFileNameA
#define GetSaveFileName GetSaveFileNameA

#endif // ORBITER_LINUX_COMMDLG_H
