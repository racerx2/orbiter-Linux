// Linux <windows.h> — the subset Orbiter actually calls.
//
// This exists so the launchpad sources (Launchpad.cpp, LpadTab.cpp, Tab*.cpp,
// OptionsPages.cpp, CustomControls.cpp) compile unmodified on Linux. It is
// placed first on the include path for non-Windows builds only; the Windows
// build never sees it and continues to use the real SDK header.
//
// Scope is deliberately closed: only declarations the tree references. It is
// not a general Win32 emulation and is not intended to grow into one. The
// implementation lives in Win32Dlg.cpp and is backed by ImGui, so a dialog
// created here is drawn by the same renderer as the in-sim dialogs.
//
// Handle types are opaque pointers rather than integers: the implementation
// stores real C++ objects behind them, and pointer-sized handles keep
// GetWindowLongPtr/SetWindowLongPtr (40 and 8 call sites) honest on LP64.

#ifndef ORBITER_LINUX_WINDOWS_H
#define ORBITER_LINUX_WINDOWS_H

#ifdef _WIN32
#error "This header is for non-Windows builds only."
#endif

// This header must be valid C as well as C++: Src/Orbiter/htmlctrl.c is
// compiled by the C driver and includes it. So the C standard headers are
// used rather than their <c...> C++ spellings, helpers are `static inline`
// (which means the same thing in both languages), and everything C++-only is
// guarded by __cplusplus.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <limits.h>
#include <unistd.h>
#include <stdarg.h>
#include <time.h>

#ifdef __cplusplus
// Config.cpp calls std::find on a std::list and MenuInfoBar.h uses
// std::unique_ptr. On Windows both arrive transitively through the SDK
// headers; libstdc++ makes no such guarantee, so they are pulled in here to
// keep those sources unmodified.
#include <algorithm>
#include <memory>
#define ORB_EXTERN_C extern "C"
#define ORB_EXTERN_C_BEGIN extern "C" {
#define ORB_EXTERN_C_END   }
#else
#define ORB_EXTERN_C extern
#define ORB_EXTERN_C_BEGIN
#define ORB_EXTERN_C_END
#endif

// ---------------------------------------------------------------------------
// MSVC C runtime extensions
//
// These are Microsoft's underscore-prefixed CRT names, not Win32 API. Sources
// in this tree call them directly, so they are provided here rather than
// edited out of the call sites.
// ---------------------------------------------------------------------------

static inline int _strnicmp(const char *a, const char *b, size_t n) {
    return strncasecmp(a, b, n);
}
static inline int _stricmp(const char *a, const char *b) {
    return strcasecmp(a, b);
}

// The HTML Help SDK header uses the older un-prefixed spellings.
static inline int strnicmp(const char *a, const char *b, size_t n) {
    return strncasecmp(a, b, n);
}
static inline int stricmp(const char *a, const char *b) {
    return strcasecmp(a, b);
}

// _fullpath resolves a relative path to an absolute one. Callers in this tree
// pass Windows-style paths (meshc passes ".\\"), so separators are translated
// before resolving; this keeps those call sites unmodified. Unlike realpath,
// _fullpath does not require the path to exist, so a failed resolve falls back
// to joining against the working directory rather than returning NULL.
static inline char *_fullpath(char *absPath, const char *relPath, size_t maxLength) {
    if (!relPath || !absPath || maxLength == 0) return NULL;

    char tmp[PATH_MAX];
    size_t i = 0;
    for (; relPath[i] && i < sizeof(tmp) - 1; ++i)
        tmp[i] = (relPath[i] == '\\') ? '/' : relPath[i];
    tmp[i] = '\0';

    char resolved[PATH_MAX];
    if (realpath(tmp, resolved)) {
        snprintf(absPath, maxLength, "%s", resolved);
        return absPath;
    }
    if (tmp[0] == '/') {
        snprintf(absPath, maxLength, "%s", tmp);
        return absPath;
    }
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) return NULL;
    snprintf(absPath, maxLength, "%s/%s", cwd, tmp);
    return absPath;
}

// ---------------------------------------------------------------------------
// Scalar types
//
// DWORD must be exactly 32 bits. On LP64 Linux `unsigned long` is 64 bits,
// which silently breaks every struct Orbiter serialises to file.
// ---------------------------------------------------------------------------

typedef uint32_t            DWORD;
typedef int32_t             LONG;
typedef uint32_t            ULONG;
typedef uint16_t            WORD;
typedef uint8_t             BYTE;
typedef int                 BOOL;
typedef unsigned int        UINT;
typedef char                CHAR;
// PTSTR and PCTSTR belong beside LPTSTR and LPCTSTR and were missing.
//
// Win32 spells the same pointer four ways -- LPSTR/PSTR for the ANSI form and
// LPTSTR/PTSTR for the TCHAR form, which in an ANSI build are the same type --
// and the shim had three of the four. The graphics client's atmosphere dialog
// declares `void SetToolTip(int vid, PTSTR pszText)`, verbatim from the
// reference, and without the typedef that parameter parses as an int: every
// call site then fails with "invalid conversion from 'char*' to 'int'",
// pointing at the call rather than at the missing name.
typedef const char         *LPCSTR, *PCSTR, *LPCTSTR, *PCTSTR;
typedef char               *LPSTR,  *PSTR,  *LPTSTR,  *PTSTR;
typedef void               *LPVOID, *PVOID;
typedef const void         *LPCVOID;

typedef intptr_t            LPARAM;
typedef uintptr_t           WPARAM;
typedef intptr_t            LRESULT;

typedef void                VOID;
typedef float               FLOAT;
typedef double              DOUBLE;
typedef int32_t             HRESULT;

// Windows fixed-width aliases (basetsd.h). Distinct from <cstdint>'s names,
// and used across the tree for on-disk and mesh data.
typedef int8_t              INT8,  *PINT8;
typedef int16_t             INT16, *PINT16;
typedef int32_t             INT32, *PINT32;
typedef int64_t             INT64, *PINT64;
typedef uint8_t             UINT8,  *PUINT8;
typedef uint16_t            UINT16, *PUINT16;
typedef uint32_t            UINT32, *PUINT32;
typedef uint64_t            UINT64, *PUINT64;
typedef int64_t             LONGLONG;
typedef uint64_t            ULONGLONG, DWORDLONG;
typedef int16_t             SHORT;
typedef uint16_t            USHORT;
typedef uint8_t             UCHAR;
typedef int                 INT;
typedef float              *PFLOAT;

// MSVC's __int64 family. These are compiler builtins there, not typedefs, so
// they are provided as macros to cover every spelling the tree uses.
#define __int8  char
#define __int16 short
#define __int32 int
#define __int64 long long


// Wide-character types. Nothing in this tree does wide-char work, but the
// HTML Help SDK header declares wide entry points alongside the ANSI ones and
// needs the types to exist.
typedef wchar_t             WCHAR, *PWCHAR;
typedef wchar_t            *LPWSTR, *PWSTR;
typedef const wchar_t      *LPCWSTR, *PCWSTR;

// NMHDR lives in commctrl.h on Windows, but htmlhelp.h uses it while only
// including windows.h. Defining it here, guarded, lets both headers work
// without either source being edited; commctrl.h honours the same guard.
#ifndef ORB_NMHDR_DEFINED
#define ORB_NMHDR_DEFINED
typedef struct tagNMHDR {
    struct HWND__ *hwndFrom;
    uintptr_t      idFrom;
    unsigned int   code;
} NMHDR, *LPNMHDR;
#endif

typedef int32_t            *LPLONG;
typedef DWORD              *LPDWORD;
typedef WORD               *LPWORD;
typedef BYTE               *LPBYTE;
typedef int                *LPINT;

#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr)    (((HRESULT)(hr)) <  0)
#define S_OK          ((HRESULT)0)
#define S_FALSE       ((HRESULT)1)
#define E_FAIL        ((HRESULT)0x80004005L)
#define E_INVALIDARG  ((HRESULT)0x80070057L)
#define E_NOINTERFACE ((HRESULT)0x80004002L)
#define E_NOTIMPL     ((HRESULT)0x80004001L)
#define E_POINTER     ((HRESULT)0x80004003L)
#define E_HANDLE      ((HRESULT)0x80070006L)
#define E_ABORT       ((HRESULT)0x80004004L)
#define E_ACCESSDENIED ((HRESULT)0x80070005L)
#define E_PENDING     ((HRESULT)0x8000000AL)
#define E_UNEXPECTED  ((HRESULT)0x8000FFFFL)

#define ZeroMemory(dst, len)      memset((dst), 0, (len))
#define CopyMemory(dst, src, len) memcpy((dst), (src), (len))
#define FillMemory(dst, len, val) memset((dst), (val), (len))
typedef intptr_t            INT_PTR;
typedef uintptr_t           UINT_PTR;
typedef intptr_t            LONG_PTR;
typedef uintptr_t           ULONG_PTR;
typedef ULONG_PTR           DWORD_PTR;

#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif

#define CALLBACK
#define WINAPI
#define APIENTRY
#define FAR
#define PASCAL
#define __stdcall
#define __cdecl
#define WINUSERAPI
#define IMGUI_IMPL_API

#define MAX_PATH 260

// ---------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------

#define ORB_DECLARE_HANDLE(name) struct name##__; typedef struct name##__ *name

ORB_DECLARE_HANDLE(HWND);
ORB_DECLARE_HANDLE(HINSTANCE);
ORB_DECLARE_HANDLE(HBITMAP);
ORB_DECLARE_HANDLE(HBRUSH);
ORB_DECLARE_HANDLE(HPEN);
ORB_DECLARE_HANDLE(HFONT);
ORB_DECLARE_HANDLE(HICON);
ORB_DECLARE_HANDLE(HCURSOR);
ORB_DECLARE_HANDLE(HMENU);
ORB_DECLARE_HANDLE(HTREEITEM);
ORB_DECLARE_HANDLE(HIMAGELIST);
ORB_DECLARE_HANDLE(HANDLE);

// HGDIOBJ is deliberately not an opaque handle. In the real SDK it is void*,
// which is what lets SelectObject/DeleteObject accept an HPEN, HBRUSH, HFONT
// or HBITMAP without a cast — a property 26 call sites in DlgCtrl rely on.
typedef void *HGDIOBJ;

// HDC matches GraphicsAPI.h, which already carries a `#ifndef _WIN32` branch
// declaring it as void*. Upstream made that choice for a non-Windows build, so
// this header follows it rather than introducing a conflicting handle type;
// the two declarations must agree or every translation unit including both
// fails with a conflicting typedef.
#ifndef ORB_HDC_DEFINED
#define ORB_HDC_DEFINED
typedef void *HDC;
#endif

typedef HINSTANCE HMODULE;

#define NULL_HANDLE ((HWND)0)

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

typedef struct tagRECT {
    LONG left, top, right, bottom;
} RECT, *LPRECT, *PRECT;

typedef struct tagPOINT {
    LONG x, y;
} POINT, *LPPOINT, *PPOINT;

typedef struct tagSIZE {
    LONG cx, cy;
} SIZE, *LPSIZE;

typedef struct tagMSG {
    HWND   hwnd;
    UINT   message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD  time;
    POINT  pt;
} MSG, *LPMSG, *PMSG;

// COLORREF is 0x00BBGGRR, matching Windows. Orbiter's RGB() call sites and
// its stored colour values both assume this byte order.
typedef DWORD COLORREF;
#define RGB(r, g, b) ((COLORREF)(((BYTE)(r)) | ((WORD)((BYTE)(g)) << 8) | (((DWORD)(BYTE)(b)) << 16)))
#define GetRValue(c) ((BYTE)((c) & 0xFF))
#define GetGValue(c) ((BYTE)(((c) >> 8) & 0xFF))
#define GetBValue(c) ((BYTE)(((c) >> 16) & 0xFF))

// ---------------------------------------------------------------------------
// Window messages — only those the tree dispatches on.
// ---------------------------------------------------------------------------

#define WM_NULL             0x0000
#define WM_CREATE           0x0001
#define WM_DESTROY          0x0002
#define WM_MOVE             0x0003
#define WM_SIZE             0x0005
#define WM_ACTIVATE         0x0006
#define WM_SETFOCUS         0x0007
#define WM_KILLFOCUS        0x0008
#define WM_PAINT            0x000F
#define WM_CLOSE            0x0010
#define WM_QUIT             0x0012
#define WM_ERASEBKGND       0x0014
#define WM_SHOWWINDOW       0x0018
#define WM_SETCURSOR        0x0020
#define WM_GETMINMAXINFO    0x0024
#define WM_DRAWITEM         0x002B
#define WM_SETFONT          0x0030
#define WM_GETFONT          0x0031
#define WM_GETTEXT          0x000D
#define WM_SETTEXT          0x000C
#define WM_GETTEXTLENGTH    0x000E
#define WM_NOTIFY           0x004E
#define WM_CONTEXTMENU      0x007B
#define WM_NCDESTROY        0x0082
#define WM_KEYDOWN          0x0100
#define WM_KEYUP            0x0101
#define WM_CHAR             0x0102
#define WM_SYSKEYDOWN       0x0104
#define WM_SYSKEYUP         0x0105
#define WM_COMMAND          0x0111
#define WM_TIMER            0x0113
#define WM_HSCROLL          0x0114
#define WM_VSCROLL          0x0115
#define WM_CTLCOLOREDIT     0x0133
#define WM_CTLCOLORLISTBOX  0x0134
#define WM_CTLCOLORBTN      0x0135
#define WM_CTLCOLORDLG      0x0136
#define WM_CTLCOLORSTATIC   0x0138
#define WM_MOUSEMOVE        0x0200
#define WM_LBUTTONDOWN      0x0201
#define WM_LBUTTONUP        0x0202
#define WM_LBUTTONDBLCLK    0x0203
#define WM_RBUTTONDOWN      0x0204
#define WM_RBUTTONUP        0x0205
#define WM_MBUTTONDOWN      0x0207
#define WM_MBUTTONUP        0x0208
#define WM_MOUSEWHEEL       0x020A
#define WM_USER             0x0400

// The virtual-key state word carried in a mouse message's wParam.
//
// Orbiter's own handlers read only the wheel delta out of it, but the word is
// passed verbatim to every plugin's clbkProcessMouse, so it has to mean what
// a plugin written against Win32 expects. Real Win32 values.
#define MK_LBUTTON          0x0001
#define MK_RBUTTON          0x0002
#define MK_SHIFT            0x0004
#define MK_CONTROL          0x0008
#define MK_MBUTTON          0x0010

// One notch of a wheel. WM_MOUSEWHEEL reports movement in multiples of this,
// and Camera::ProcessMouse divides by it (-2.0/120.0*RAD per notch of FOV).
#define WHEEL_DELTA         120
#define WM_APP              0x8000

#define WM_INITDIALOG       0x0110

// SetWindowPos flags
#define SWP_NOSIZE          0x0001
#define SWP_NOMOVE          0x0002
#define SWP_NOZORDER        0x0004
#define SWP_NOREDRAW        0x0008
#define SWP_NOACTIVATE      0x0010
#define SWP_SHOWWINDOW      0x0040
#define SWP_HIDEWINDOW      0x0080
#define SWP_NOCOPYBITS      0x0100
#define SWP_NOOWNERZORDER   0x0200

// ShowWindow commands
#define SW_HIDE             0
#define SW_SHOWNORMAL       1
#define SW_SHOW             5
#define SW_MINIMIZE         6
#define SW_RESTORE          9

#define SIZE_RESTORED       0
#define SIZE_MINIMIZED      1
#define SIZE_MAXIMIZED      2

// Standard control ids
#define IDOK                1
#define IDCANCEL            2
#define IDABORT             3
#define IDRETRY             4
#define IDIGNORE            5
#define IDYES               6
#define IDNO                7
#define IDHELP              9

// MessageBox types
#define MB_OK               0x0000
#define MB_OKCANCEL         0x0001
#define MB_YESNOCANCEL      0x0003
#define MB_YESNO            0x0004
#define MB_ICONERROR        0x0010
#define MB_ICONQUESTION     0x0020
#define MB_ICONWARNING      0x0030
#define MB_ICONINFORMATION  0x0040
#define MB_TASKMODAL        0x2000

// GetWindowLongPtr / SetWindowLongPtr indices
#define GWL_STYLE           (-16)
#define GWL_EXSTYLE         (-20)
#define GWLP_USERDATA       (-21)
#define GWLP_WNDPROC        (-4)
#define DWLP_MSGRESULT      0
#define DWLP_DLGPROC        (DWLP_MSGRESULT + sizeof(LRESULT))
#define DWLP_USER           (DWLP_DLGPROC + sizeof(void *))

// Window styles referenced by the dialog templates
#define WS_OVERLAPPED       0x00000000L
#define WS_POPUP            0x80000000L
#define WS_CHILD            0x40000000L
#define WS_MINIMIZE         0x20000000L
#define WS_VISIBLE          0x10000000L
#define WS_DISABLED         0x08000000L
#define WS_CLIPSIBLINGS     0x04000000L
#define WS_CLIPCHILDREN     0x02000000L
#define WS_MAXIMIZE         0x01000000L
#define WS_CAPTION          0x00C00000L
#define WS_BORDER           0x00800000L
#define WS_DLGFRAME         0x00400000L
#define WS_VSCROLL          0x00200000L
#define WS_HSCROLL          0x00100000L
#define WS_SYSMENU          0x00080000L
#define WS_THICKFRAME       0x00040000L
#define WS_GROUP            0x00020000L
#define WS_TABSTOP          0x00010000L
#define WS_MINIMIZEBOX      0x00020000L
#define WS_MAXIMIZEBOX      0x00010000L
#define WS_EX_APPWINDOW     0x00040000L
#define WS_EX_CLIENTEDGE    0x00000200L
#define WS_EX_RIGHT         0x00001000L

// ---------------------------------------------------------------------------
// Control styles
//
// These appear in Orbiter.rc's dialog templates. The resource compiler
// resolves them on Windows; here the build-time converter that reads the .rc
// resolves them through this header, so the values must be the SDK's.
// ---------------------------------------------------------------------------

// Button styles
#define BS_PUSHBUTTON       0x00000000L
#define BS_DEFPUSHBUTTON    0x00000001L
#define BS_CHECKBOX         0x00000002L
#define BS_AUTOCHECKBOX     0x00000003L
#define BS_RADIOBUTTON      0x00000004L
#define BS_3STATE           0x00000005L
#define BS_AUTO3STATE       0x00000006L
#define BS_GROUPBOX         0x00000007L
#define BS_AUTORADIOBUTTON  0x00000009L
#define BS_OWNERDRAW        0x0000000BL
#define BS_LEFTTEXT         0x00000020L
#define BS_TEXT             0x00000000L
#define BS_ICON             0x00000040L
#define BS_BITMAP           0x00000080L
#define BS_LEFT             0x00000100L
#define BS_RIGHT            0x00000200L
#define BS_CENTER           0x00000300L
#define BS_TOP              0x00000400L
#define BS_BOTTOM           0x00000800L
#define BS_VCENTER          0x00000C00L
#define BS_PUSHLIKE         0x00001000L
#define BS_MULTILINE        0x00002000L
#define BS_FLAT             0x00008000L

// Static styles
#define SS_LEFT             0x00000000L
#define SS_CENTER           0x00000001L
#define SS_RIGHT            0x00000002L
#define SS_ICON             0x00000003L
#define SS_BLACKRECT        0x00000004L
#define SS_GRAYRECT         0x00000005L
#define SS_WHITERECT        0x00000006L
#define SS_BLACKFRAME       0x00000007L
#define SS_OWNERDRAW        0x0000000DL
#define SS_BITMAP           0x0000000EL
#define SS_ETCHEDHORZ       0x00000010L
#define SS_ETCHEDVERT       0x00000011L
#define SS_ETCHEDFRAME      0x00000012L
#define SS_NOPREFIX         0x00000080L
#define SS_NOTIFY           0x00000100L
#define SS_CENTERIMAGE      0x00000200L
// The three static styles between SS_CENTERIMAGE and SS_SUNKEN, added when
// ScnEditor.rc became the first .rc on this platform other than Orbiter.rc to
// be converted. rc2cpp.py resolves every style through the C preprocessor
// against these headers, so a missing #define is not a silently wrong style --
// it is UnresolvedSymbol and a failed build, which is the right way round.
// Values from winuser.h; only SS_REALSIZEIMAGE is currently used (IDD_EDITOR's
// preview control), the neighbours are here because leaving a hole in a
// bit-flag run is how the next one gets guessed instead of looked up.
#define SS_RIGHTJUST        0x00000400L
#define SS_REALSIZEIMAGE    0x00000800L
#define SS_SUNKEN           0x00001000L

// Edit styles
#define ES_LEFT             0x00000000L
#define ES_CENTER           0x00000001L
#define ES_RIGHT            0x00000002L
#define ES_MULTILINE        0x00000004L
#define ES_UPPERCASE        0x00000008L
#define ES_LOWERCASE        0x00000010L
#define ES_PASSWORD         0x00000020L
#define ES_AUTOVSCROLL      0x00000040L
#define ES_AUTOHSCROLL      0x00000080L
#define ES_NOHIDESEL        0x00000100L
#define ES_READONLY         0x00000800L
#define ES_WANTRETURN       0x00001000L
#define ES_NUMBER           0x00002000L

// Combo box styles
#define CBS_SIMPLE          0x0001L
#define CBS_DROPDOWN        0x0002L
#define CBS_DROPDOWNLIST    0x0003L
#define CBS_AUTOHSCROLL     0x0040L
#define CBS_SORT            0x0100L
#define CBS_HASSTRINGS      0x0200L
#define CBS_NOINTEGRALHEIGHT 0x0400L

// List box styles
#define LBS_NOTIFY          0x0001L
#define LBS_SORT            0x0002L
#define LBS_NOREDRAW        0x0004L
#define LBS_MULTIPLESEL     0x0008L
#define LBS_OWNERDRAWFIXED  0x0010L
#define LBS_OWNERDRAWVARIABLE 0x0020L
#define LBS_HASSTRINGS      0x0040L
#define LBS_USETABSTOPS     0x0080L
#define LBS_NOINTEGRALHEIGHT 0x0100L
#define LBS_MULTICOLUMN     0x0200L
#define LBS_WANTKEYBOARDINPUT 0x0400L
#define LBS_EXTENDEDSEL     0x0800L
// The rest of the run, completed for the same reason the SS_ block above was:
// rc2cpp.py resolves .rc styles through the C preprocessor against these
// headers, so a gap is a build failure rather than a wrong style -- but only
// once something uses it, and Orbiter.rc used none of these. ScnEditor.rc's
// IDD_EDITOR uses LBS_NOSEL. Values from winuser.h.
#define LBS_DISABLENOSCROLL 0x1000L
#define LBS_NODATA          0x2000L
#define LBS_NOSEL           0x4000L
#define LBS_COMBOBOX        0x8000L

// Dialog styles
#define DS_ABSALIGN         0x0001L
#define DS_SYSMODAL         0x0002L
#define DS_LOCALEDIT        0x0020L
#define DS_SETFONT          0x0040L
#define DS_MODALFRAME       0x0080L
#define DS_NOIDLEMSG        0x0100L
#define DS_SETFOREGROUND    0x0200L
#define DS_3DLOOK           0x0004L
#define DS_FIXEDSYS         0x0008L
#define DS_CENTER           0x0800L
#define DS_CONTROL          0x0400L
#define DS_NOFAILCREATE     0x0010L
#define DS_CENTERMOUSE      0x1000L
#define DS_CONTEXTHELP      0x2000L

// DS_SHELLFONT is the pair, not a bit of its own -- the SDK spells it exactly
// this way. It is what the dialog editor writes for "8, MS Shell Dlg", so
// almost every modern template carries it: ToolKit.rc opens
//
//     IDD_TOOLKIT DIALOGEX 0, 0, 200, 320
//     STYLE DS_SHELLFONT | WS_POPUP | WS_CAPTION | ...
//
// and without the definition rc2cpp.py raised UnresolvedSymbol on the first
// STYLE line and converted none of the module's dialogs.
#define DS_SHELLFONT        (DS_SETFONT | DS_FIXEDSYS)

// Up-down, progress and scrollbar styles
#define UDS_WRAP            0x0001
#define UDS_SETBUDDYINT     0x0002
#define UDS_ALIGNRIGHT      0x0004
#define UDS_ALIGNLEFT       0x0008
#define UDS_AUTOBUDDY       0x0010
#define UDS_ARROWKEYS       0x0020
#define UDS_HORZ            0x0040
#define UDS_NOTHOUSANDS     0x0080

#define PBS_SMOOTH          0x01
#define PBS_VERTICAL        0x04

#define SBS_HORZ            0x0000L
#define SBS_VERT            0x0001L

#define WS_EX_TRANSPARENT   0x00000020L
#define WS_EX_CONTROLPARENT 0x00010000L
#define WS_EX_NOACTIVATE    0x08000000L

// Callback signatures
typedef LRESULT (CALLBACK *WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef INT_PTR (CALLBACK *DLGPROC)(HWND, UINT, WPARAM, LPARAM);
typedef void (CALLBACK *TIMERPROC)(HWND, UINT, UINT_PTR, DWORD);

// MAKEINTRESOURCE encodes an integer id in a char* slot; the implementation
// recognises pointers below 0x10000 as ids rather than strings, which is what
// the real Win32 loader does.
#define MAKEINTRESOURCE(i) ((LPCSTR)(uintptr_t)((WORD)(i)))
// The explicitly-ANSI spelling. On Windows MAKEINTRESOURCE is the neuter form
// that resolves to MAKEINTRESOURCEA or MAKEINTRESOURCEW by UNICODE, and code
// that pairs it with FindResourceA writes the A form out -- D3D9Client's
// SplashScreen() does, and it is not the only one. This shim is ANSI-only, so
// the two are the same macro; the alias exists so that source does not have to
// be edited to say so.
#define MAKEINTRESOURCEA(i) MAKEINTRESOURCE(i)
#define IS_INTRESOURCE(r)  (((uintptr_t)(r)) <= 0xFFFF)

#define LOWORD(l) ((WORD)(((uintptr_t)(l)) & 0xFFFF))
#define HIWORD(l) ((WORD)((((uintptr_t)(l)) >> 16) & 0xFFFF))
#define LOBYTE(w) ((BYTE)(((uintptr_t)(w)) & 0xFF))
#define HIBYTE(w) ((BYTE)((((uintptr_t)(w)) >> 8) & 0xFF))
#define MAKELPARAM(l, h) ((LPARAM)((((WORD)(l)) | ((DWORD)((WORD)(h))) << 16)))
#define MAKEWPARAM(l, h) ((WPARAM)((((WORD)(l)) | ((DWORD)((WORD)(h))) << 16)))

// ---------------------------------------------------------------------------
// Window and dialog API
// ---------------------------------------------------------------------------

ORB_EXTERN_C_BEGIN

HWND    CreateDialogParam   (HINSTANCE, LPCSTR, HWND, DLGPROC, LPARAM);
INT_PTR DialogBoxParam      (HINSTANCE, LPCSTR, HWND, DLGPROC, LPARAM);
HWND    CreateDialog        (HINSTANCE, LPCSTR, HWND, DLGPROC);
BOOL    EndDialog           (HWND, INT_PTR);
BOOL    DestroyWindow       (HWND);

HWND    GetDlgItem          (HWND, int);
int     GetDlgCtrlID        (HWND);
LRESULT SendDlgItemMessageA (HWND, int, UINT, WPARAM, LPARAM);
LRESULT SendMessageA        (HWND, UINT, WPARAM, LPARAM);
BOOL    PostMessageA        (HWND, UINT, WPARAM, LPARAM);

BOOL    SetWindowTextA      (HWND, LPCSTR);
int     GetWindowTextA      (HWND, LPSTR, int);
int     GetWindowTextLengthA(HWND);
BOOL    SetDlgItemTextA     (HWND, int, LPCSTR);
UINT    GetDlgItemTextA     (HWND, int, LPSTR, int);

BOOL    ShowWindow          (HWND, int);
BOOL    EnableWindow        (HWND, BOOL);
BOOL    IsWindowVisible     (HWND);
BOOL    IsWindowEnabled     (HWND);
BOOL    SetWindowPos        (HWND, HWND, int, int, int, int, UINT);
BOOL    GetClientRect       (HWND, LPRECT);
BOOL    GetWindowRect       (HWND, LPRECT);
BOOL    ClientToScreen      (HWND, LPPOINT);
BOOL    ScreenToClient      (HWND, LPPOINT);
BOOL    InvalidateRect      (HWND, const RECT *, BOOL);
// ValidateRect, added for OVP/VulkanClient's clbkCreateRenderWindow, which
// calls it once after filling the client area black -- "avoids white flash
// after splash screen", in the author's words. On Win32 it removes a
// rectangle from the window's update region so the next WM_PAINT does not
// redraw it. There is no update region here: Gdi.cpp is a display-list
// recorder and the core repaints the whole frame every time, so the
// counterpart is a no-op that returns TRUE. Declared rather than #defined
// away so the call site reads as it did.
BOOL    ValidateRect        (HWND, const RECT *);
BOOL    UpdateWindow        (HWND);

LONG_PTR GetWindowLongPtrA  (HWND, int);
LONG_PTR SetWindowLongPtrA  (HWND, int, LONG_PTR);

UINT_PTR SetTimer           (HWND, UINT_PTR, UINT, TIMERPROC);
BOOL     KillTimer          (HWND, UINT_PTR);

int     MessageBoxA         (HWND, LPCSTR, LPCSTR, UINT);
LRESULT DefWindowProcA      (HWND, UINT, WPARAM, LPARAM);

HWND    GetParent           (HWND);

// GetAncestor and EnumChildWindows, added for OVP/VulkanClient's VideoTab,
// which walks up from its own tab page to the Launchpad and then back down
// looking for the scenario tree control. Both are pure walks over the window
// tree Win32Dlg.cpp already maintains -- every Window there has a parent and
// a children vector -- so neither invents any state.
//
// GA_PARENT is the same answer GetParent gives for a child window; GA_ROOT
// walks parents to the top; GA_ROOTOWNER continues through owners, which is
// how Win32 distinguishes an owned popup's root from its owner's.
#define GA_PARENT     1
#define GA_ROOT       2
#define GA_ROOTOWNER  3
HWND    GetAncestor         (HWND, UINT);

typedef BOOL (*WNDENUMPROC)(HWND, LPARAM);
// Depth-first over every descendant, stopping as soon as the callback returns
// FALSE -- which is what Win32 documents and what the VideoTab's EnumChildProc
// relies on to keep the first match.
BOOL    EnumChildWindows    (HWND, WNDENUMPROC, LPARAM);

HWND    SetFocus            (HWND);
HWND    GetFocus            (void);
// The high bit is set while the key is held, which is the bit dialog
// keyboard handling tests to detect a Shift-Tab.
SHORT   GetKeyState         (int vkey);
// GetAsyncKeyState, added for OVP/VulkanClient's RenderWndProc, which uses it
// to read Shift and Ctrl while handling a mouse or key message. On Win32 the
// difference from GetKeyState is WHEN the state is sampled: GetKeyState
// answers for the message being processed, GetAsyncKeyState for right now.
// The distinction does not survive here -- the UI host samples the live
// keyboard once per frame and there is no per-message snapshot -- so both
// answer from that sample, with the same high-bit-means-held convention.
SHORT   GetAsyncKeyState    (int vkey);
BOOL    GetCursorPos        (LPPOINT);
BOOL    SetCursorPos        (int, int);

// Monitor geometry, added for OVP/VulkanClient's FixOutOfScreenPositions,
// which asks which monitor a popup landed on and how big it is so it can pull
// an off-screen dialog back into view. GLFW answers both questions
// (glfwGetMonitors / glfwGetMonitorWorkarea), so the Win32 calls map onto it.
// HMONITOR is an opaque handle here as it is there -- the implementation
// stores a monitor index in it.
ORB_DECLARE_HANDLE(HMONITOR);

#define MONITOR_DEFAULTTONULL     0x00000000
#define MONITOR_DEFAULTTOPRIMARY  0x00000001
#define MONITOR_DEFAULTTONEAREST  0x00000002

#define MONITORINFOF_PRIMARY      0x00000001

typedef struct tagMONITORINFO {
	DWORD cbSize;
	RECT  rcMonitor;
	RECT  rcWork;
	DWORD dwFlags;
} MONITORINFO, *LPMONITORINFO;

HMONITOR MonitorFromWindow  (HWND, DWORD dwFlags);
BOOL     GetMonitorInfoA    (HMONITOR, LPMONITORINFO);

int     LoadStringA         (HINSTANCE, UINT, LPSTR, int);
HBITMAP LoadBitmapA         (HINSTANCE, LPCSTR);

ORB_EXTERN_C_END

// Orbiter is compiled without UNICODE, so the unsuffixed names are the ANSI
// ones. These aliases keep both spellings working, as the real SDK does.
#define SendDlgItemMessage  SendDlgItemMessageA
#define SendMessage         SendMessageA
#define PostMessage         PostMessageA
// The explicit-ANSI spellings. Orbiter's own sources use the unsuffixed names,
// but TerrainToolKit calls CreateDialogParamA directly, and in the real SDK
// both resolve to the same entry point.
#define CreateDialogParamA  CreateDialogParam
#define CreateDialogA       CreateDialog
#define GetMonitorInfo      GetMonitorInfoA
#define DialogBoxParamA     DialogBoxParam
// Windows defines DialogBox as a macro over DialogBoxParam with a zero
// creation parameter, and there is no separate entry point. It was missing
// here, so the only caller in the tree -- the LaunchpadParamTemplate SDK
// sample -- failed to COMPILE, which is why nothing had noticed: a module that
// does not build is absent in exactly the way an unported one is.
#define DialogBox(hInst, tmpl, parent, proc) \
        DialogBoxParam((hInst), (tmpl), (parent), (proc), 0L)
#define DialogBoxA          DialogBox
#define SetWindowText       SetWindowTextA
#define GetWindowText       GetWindowTextA
#define GetWindowTextLength GetWindowTextLengthA
#define SetDlgItemText      SetDlgItemTextA
#define GetDlgItemText      GetDlgItemTextA
#define GetWindowLongPtr    GetWindowLongPtrA
#define SetWindowLongPtr    SetWindowLongPtrA
// The 32-bit forms. On Win64 the SDK defines these as the Ptr versions for the
// indices that hold pointers, and callers that only store an int are unaffected
// either way. TerrainToolKit's gcTableView uses the short spelling.
#define GetWindowLongA      GetWindowLongPtrA
#define SetWindowLongA      SetWindowLongPtrA
#define GetWindowLong       GetWindowLongPtrA
#define SetWindowLong       SetWindowLongPtrA
#define MessageBox          MessageBoxA
#define DefWindowProc       DefWindowProcA
#define LoadString          LoadStringA
#define LoadBitmap          LoadBitmapA

// ---------------------------------------------------------------------------
// GDI
//
// DlgCtrl's gauge and switch controls are owner-drawn: they paint themselves
// with pens, brushes and BitBlt in response to WM_PAINT. These are declared so
// those sources compile unchanged; Win32Dlg.cpp implements them against an
// ImGui draw list, so a control painted here lands in the same frame as the
// in-sim dialogs.
// ---------------------------------------------------------------------------

typedef struct tagPAINTSTRUCT {
    HDC   hdc;
    BOOL  fErase;
    RECT  rcPaint;
    BOOL  fRestore;
    BOOL  fIncUpdate;
    BYTE  rgbReserved[32];
} PAINTSTRUCT, *LPPAINTSTRUCT;

typedef struct tagLOGBRUSH {
    UINT      lbStyle;
    COLORREF  lbColor;
    ULONG_PTR lbHatch;
} LOGBRUSH, *LPLOGBRUSH;

// GetObject fills this for an HBITMAP. Panel.cpp reads bmWidth/bmHeight to
// size the panel background, so the layout matches the SDK.
typedef struct tagBITMAP {
    LONG   bmType;
    LONG   bmWidth;
    LONG   bmHeight;
    LONG   bmWidthBytes;
    WORD   bmPlanes;
    WORD   bmBitsPixel;
    LPVOID bmBits;
} BITMAP, *PBITMAP, *LPBITMAP;

// Device-independent bitmap headers. GraphicsAPI.cpp builds one of these to
// create a DIB section for image loading, so the layout matches the SDK
// exactly -- biSize is compared against sizeof() and the field order is part
// of the BMP file format.
typedef struct tagBITMAPINFOHEADER {
    DWORD biSize;
    LONG  biWidth;
    LONG  biHeight;
    WORD  biPlanes;
    WORD  biBitCount;
    DWORD biCompression;
    DWORD biSizeImage;
    LONG  biXPelsPerMeter;
    LONG  biYPelsPerMeter;
    DWORD biClrUsed;
    DWORD biClrImportant;
} BITMAPINFOHEADER, *PBITMAPINFOHEADER, *LPBITMAPINFOHEADER;

typedef struct tagRGBQUAD {
    BYTE rgbBlue, rgbGreen, rgbRed, rgbReserved;
} RGBQUAD;

// The BMP file header and the 24-bit pixel triple. Dragonfly's panel loader
// reads .bmp files itself rather than going through LoadImage, so it needs
// both. The layout is the on-disk BMP format, so the field order and the
// 2-byte packing of BITMAPFILEHEADER are part of the file format, not a
// choice -- bfType/bfSize straddle a 4-byte boundary and the struct must not
// be padded or every offset read from it is wrong.
#pragma pack(push, 2)
typedef struct tagBITMAPFILEHEADER {
    WORD  bfType;
    DWORD bfSize;
    WORD  bfReserved1;
    WORD  bfReserved2;
    DWORD bfOffBits;
} BITMAPFILEHEADER, *PBITMAPFILEHEADER, *LPBITMAPFILEHEADER;
#pragma pack(pop)

typedef struct tagRGBTRIPLE {
    BYTE rgbtBlue, rgbtGreen, rgbtRed;
} RGBTRIPLE, *PRGBTRIPLE, *LPRGBTRIPLE;

typedef struct tagBITMAPINFO {
    BITMAPINFOHEADER bmiHeader;
    RGBQUAD          bmiColors[1];
} BITMAPINFO, *PBITMAPINFO, *LPBITMAPINFO;

#define BI_RGB          0
#define BI_RLE8         1
#define BI_BITFIELDS    3
#define DIB_RGB_COLORS  0
#define DIB_PAL_COLORS  1

typedef struct tagSCROLLINFO {
    UINT cbSize;
    UINT fMask;
    int  nMin, nMax;
    UINT nPage;
    int  nPos, nTrackPos;
} SCROLLINFO, *LPSCROLLINFO;

typedef struct tagWNDCLASSA {
    UINT      style;
    WNDPROC   lpfnWndProc;
    int       cbClsExtra, cbWndExtra;
    HINSTANCE hInstance;
    HICON     hIcon;
    HCURSOR   hCursor;
    HBRUSH    hbrBackground;
    LPCSTR    lpszMenuName;
    LPCSTR    lpszClassName;
} WNDCLASSA, *LPWNDCLASSA;
typedef WNDCLASSA WNDCLASS;

// Stock objects
#define WHITE_BRUSH    0
#define LTGRAY_BRUSH   1
#define GRAY_BRUSH     2
#define DKGRAY_BRUSH   3
#define BLACK_BRUSH    4
#define NULL_BRUSH     5
#define WHITE_PEN      6
#define BLACK_PEN      7
#define NULL_PEN       8
#define DEFAULT_GUI_FONT 17

// Font metrics, as reported by GetTextMetrics. Only the fields Orbiter's
// instrument code reads are meaningful here.
typedef struct tagTEXTMETRICA {
    LONG tmHeight;
    LONG tmAscent;
    LONG tmDescent;
    LONG tmInternalLeading;
    LONG tmExternalLeading;
    LONG tmAveCharWidth;
    LONG tmMaxCharWidth;
    LONG tmWeight;
    LONG tmOverhang;
    LONG tmDigitizedAspectX;
    LONG tmDigitizedAspectY;
} TEXTMETRICA, *PTEXTMETRICA, *LPTEXTMETRICA;
typedef TEXTMETRICA TEXTMETRIC;
typedef LPTEXTMETRICA LPTEXTMETRIC;

// Pen styles
// The pen styles, with Win32's own values. PS_DOT was missing: the graphics
// client's Sketchpad pen maps its style 2 to PS_DOT and IsDashed() tests for
// it, so a dashed pen could not be spelled at all.
#define PS_SOLID       0
#define PS_DASH        1
#define PS_DOT         2
#define PS_DASHDOT     3
#define PS_DASHDOTDOT  4
#define PS_NULL        5
#define PS_INSIDEFRAME 6

// Brush styles — LOGBRUSH.lbStyle
#define BS_SOLID       0
#define BS_NULL        1
#define BS_HOLLOW      BS_NULL
#define BS_HATCHED     2
#define BS_PATTERN     3

// Hatch styles, the lbHatch value when lbStyle is BS_HATCHED. Dragonfly's
// panel uses HS_BDIAGONAL for its shaded regions.
#define HS_HORIZONTAL  0
#define HS_VERTICAL    1
#define HS_FDIAGONAL   2
#define HS_BDIAGONAL   3
#define HS_CROSS       4
#define HS_DIAGCROSS   5

// MSVC's unprefixed `byte`. It predates std::byte and is an unsigned char, not
// the C++17 enum class -- Dragonfly casts pixel data through it, which the
// scoped std::byte would not permit. Only defined when the tree has not
// already got it from a system header.
#ifndef ORB_BYTE_DEFINED
#define ORB_BYTE_DEFINED
typedef unsigned char byte;
#endif

// Font parameters
#define FW_NORMAL             400
#define FW_BOLD               700
#define ANSI_CHARSET          0
#define DEFAULT_CHARSET       1
#define SYMBOL_CHARSET        2
#define OUT_DEFAULT_PRECIS    0
#define CLIP_DEFAULT_PRECIS   0
#define DEFAULT_QUALITY       0
#define DRAFT_QUALITY         1
#define PROOF_QUALITY         2
#define NONANTIALIASED_QUALITY 3
#define ANTIALIASED_QUALITY   4
// CLEARTYPE_QUALITY completes the set. The Sketchpad font selects it from
// Config->SketchpadFont == 2 and from SKP_FONT_CLEARTYPE. Nothing here
// rasterises ClearType -- stb_truetype antialiases greyscale -- but the value
// round-trips through the LOGFONT and GetQuality() hands it back, so it has to
// exist and has to be Win32's own number.
#define CLEARTYPE_QUALITY     5
#define CLEARTYPE_NATURAL_QUALITY 6
#define DEFAULT_PITCH         0
#define FIXED_PITCH           1
#define VARIABLE_PITCH        2
#define FF_DONTCARE           (0 << 4)
#define FF_ROMAN              (1 << 4)
#define FF_SWISS              (2 << 4)
#define FF_MODERN             (3 << 4)

// System colours
#define COLOR_3DFACE   15
#define COLOR_3DSHADOW 16
#define COLOR_WINDOW   5
#define COLOR_BTNTEXT  18

// BitBlt raster op
#define SRCCOPY        0x00CC0020

// Window class styles
#define CS_VREDRAW     0x0001
#define CS_HREDRAW     0x0002
#define CS_DBLCLKS     0x0008
#define CS_OWNDC       0x0020
#define CS_CLASSDC     0x0040
#define CS_PARENTDC    0x0080
#define CS_NOCLOSE     0x0200
#define CS_SAVEBITS    0x0800
#define CS_BYTEALIGNCLIENT 0x1000
#define CS_BYTEALIGNWINDOW 0x2000
#define CS_GLOBALCLASS 0x4000
// Added for OVP/VulkanClient's WindowMgr, which registers its floating panel
// class with it. It asks the window manager to draw a drop shadow behind the
// window; there is nothing to switch on here, so it is accepted and ignored,
// which is also what Windows does on a theme that draws no shadows.
#define CS_DROPSHADOW  0x00020000

#define IDC_ARROW      MAKEINTRESOURCE(32512)

// Scrollbar
#define SB_HORZ        0
#define SB_VERT        1
#define SB_CTL         2
#define SB_LINEUP      0
#define SB_LINELEFT    0
#define SB_LINEDOWN    1
#define SB_LINERIGHT   1
#define SB_PAGEUP      2
#define SB_PAGELEFT    2
#define SB_PAGEDOWN    3
#define SB_PAGERIGHT   3
#define SB_THUMBPOSITION 4
#define SB_THUMBTRACK  5
#define SB_TOP         6
#define SB_BOTTOM      7
#define SIF_RANGE      0x0001
#define SIF_PAGE       0x0002
#define SIF_POS        0x0004
#define SIF_TRACKPOS   0x0010
#define SIF_ALL        (SIF_RANGE | SIF_PAGE | SIF_POS | SIF_TRACKPOS)

// Button messages
#define BM_GETSTATE    0x00F2
#define BM_SETCHECK    0x00F1
#define BM_GETCHECK    0x00F0
#define BN_CLICKED     0
#define WM_ENABLE      0x000A

// DllMain reasons — Orbitersdk.cpp switches on these.
#define DLL_PROCESS_ATTACH 1
#define DLL_THREAD_ATTACH  2
#define DLL_THREAD_DETACH  3
#define DLL_PROCESS_DETACH 0

// ---------------------------------------------------------------------------
// LOGFONT -- the description CreateFont was given, readable back with
// GetObject(hFont, sizeof(LOGFONT), &lf).
//
// Added because the graphics client's font manager is built on that read-back:
// D3D9TextMgr.cpp's Init() takes an HFONT, asks GetObject for its LOGFONT, and
// uses the face name, height, weight and italic flag to rasterise a glyph
// atlas. On Windows the atlas came from GDI itself -- SelectObject the font
// into a DC and TextOut every character -- but Gdi.cpp here is a display-list
// RECORDER, so there are no glyph pixels to read back and the client has to
// rasterise the face itself. To do that it must first be told which face, and
// this struct is how Win32 says so.
//
// Field for field as Win32 declares it, so that client code copied from the
// reference compiles unchanged; Gdi.cpp fills the five fields CreateFontA is
// actually given and leaves the rest zero, which is what it knows.
// ---------------------------------------------------------------------------
#define LF_FACESIZE 32

typedef struct tagLOGFONTA {
    LONG lfHeight;
    LONG lfWidth;
    LONG lfEscapement;
    LONG lfOrientation;
    LONG lfWeight;
    BYTE lfItalic;
    BYTE lfUnderline;
    BYTE lfStrikeOut;
    BYTE lfCharSet;
    BYTE lfOutPrecision;
    BYTE lfClipPrecision;
    BYTE lfQuality;
    BYTE lfPitchAndFamily;
    CHAR lfFaceName[LF_FACESIZE];
} LOGFONTA, *PLOGFONTA, *LPLOGFONTA;

typedef LOGFONTA LOGFONT;
typedef PLOGFONTA PLOGFONT;
typedef LPLOGFONTA LPLOGFONT;

ORB_EXTERN_C_BEGIN

// Device contexts and painting
HDC     GetDC               (HWND);
int     ReleaseDC           (HWND, HDC);
HDC     BeginPaint          (HWND, LPPAINTSTRUCT);
BOOL    EndPaint            (HWND, const PAINTSTRUCT *);
BOOL    GetUpdateRect       (HWND, LPRECT, BOOL);
HDC     CreateCompatibleDC  (HDC);
BOOL    DeleteDC            (HDC);
BOOL    BitBlt              (HDC, int, int, int, int, HDC, int, int, DWORD);

// Object selection and lifetime
HGDIOBJ SelectObject        (HDC, HGDIOBJ);
BOOL    DeleteObject        (HGDIOBJ);
HGDIOBJ GetStockObject      (int);
DWORD   GetSysColor         (int);
int     GetObjectA          (HGDIOBJ obj, int cb, LPVOID buf);

// Pens, brushes, fonts
HPEN    CreatePen           (int, int, COLORREF);
HBRUSH  CreateSolidBrush    (COLORREF);
HBRUSH  CreateBrushIndirect (const LOGBRUSH *);
HFONT   CreateFontA         (int, int, int, int, int, DWORD, DWORD, DWORD,
                             DWORD, DWORD, DWORD, DWORD, DWORD, LPCSTR);
// CreateFontIndirectA, added for OVP/VulkanClient's SplashScreen, which fills
// a LOGFONTA and passes it rather than spelling fourteen arguments out. Win32
// has both forms and they make the same font; this one unpacks the struct and
// calls the other, which is what the real GDI does as well.
HFONT   CreateFontIndirectA (const LOGFONTA *);

// Drawing primitives
BOOL    MoveToEx            (HDC, int, int, LPPOINT);
BOOL    LineTo              (HDC, int, int);
BOOL    Rectangle           (HDC, int, int, int, int);
BOOL    Ellipse             (HDC, int, int, int, int);
BOOL    Polygon             (HDC, const POINT *, int);
BOOL    Polyline            (HDC, const POINT *, int);
BOOL    Arc                 (HDC, int, int, int, int, int, int, int, int);
// A filled pie wedge: the same bounding box and radials as Arc, but closed
// through the centre and filled with the current brush.
BOOL    Pie                 (HDC, int, int, int, int, int, int, int, int);
BOOL    TextOutA            (HDC, int, int, LPCSTR, int);
COLORREF SetTextColor       (HDC, COLORREF);
COLORREF SetBkColor         (HDC, COLORREF);
int     SetBkMode           (HDC, int);
UINT    SetTextAlign        (HDC, UINT);
BOOL    GetTextExtentPoint32A (HDC, LPCSTR, int, LPSIZE);

// Window classes
short   RegisterClassA      (const WNDCLASSA *);
BOOL    UnregisterClassA    (LPCSTR, HINSTANCE);
HCURSOR LoadCursorA         (HINSTANCE, LPCSTR);
BOOL    MoveWindow          (HWND, int, int, int, int, BOOL);
HWND    SetCapture          (HWND);
BOOL    ReleaseCapture      (void);

// Scrollbars
int     GetScrollPos        (HWND, int);
BOOL    GetScrollRange      (HWND, int, int *, int *);
int     SetScrollPos        (HWND, int, int, BOOL);
BOOL    SetScrollRange      (HWND, int, int, int, BOOL);
int     SetScrollInfo       (HWND, int, const SCROLLINFO *, BOOL);
BOOL    GetScrollInfo       (HWND, int, LPSCROLLINFO);
BOOL    ScrollWindow        (HWND, int, int, const RECT *, const RECT *);

// Module handles — dlopen/dlsym under the hood.
HMODULE GetModuleHandleA    (LPCSTR);
void   *GetProcAddress      (HMODULE, LPCSTR);

ORB_EXTERN_C_END

#define TextOut               TextOutA
#define CreateFont            CreateFontA
#define CreateFontIndirect    CreateFontIndirectA
#define GetTextExtentPoint32  GetTextExtentPoint32A
#define RegisterClass         RegisterClassA
#define UnregisterClass       UnregisterClassA
#define LoadCursor            LoadCursorA
#define GetModuleHandle       GetModuleHandleA
#define GetObject             GetObjectA

// ---------------------------------------------------------------------------
// Threading, console and menus
//
// Declared here and implemented in Src/Orbiter/Linux/*.cpp: threads and
// mutexes map onto pthreads, the console functions onto termios and ANSI
// escape sequences. Trivial ones are inlined directly.
// ---------------------------------------------------------------------------

typedef size_t SIZE_T;
typedef ptrdiff_t SSIZE_T;

typedef struct tagPOINTS {
    SHORT x, y;
} POINTS;

typedef struct _COORD {
    SHORT X, Y;
} COORD;

typedef struct _SMALL_RECT {
    SHORT Left, Top, Right, Bottom;
} SMALL_RECT;

typedef struct _CONSOLE_SCREEN_BUFFER_INFO {
    COORD      dwSize;
    COORD      dwCursorPosition;
    WORD       wAttributes;
    SMALL_RECT srWindow;
    COORD      dwMaximumWindowSize;
} CONSOLE_SCREEN_BUFFER_INFO, *PCONSOLE_SCREEN_BUFFER_INFO;

#define E_OUTOFMEMORY ((HRESULT)0x8007000EL)

// Standard handles
#define STD_INPUT_HANDLE  ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE  ((DWORD)-12)

// Console mode flags
#define ENABLE_PROCESSED_INPUT 0x0001
#define ENABLE_LINE_INPUT      0x0002
#define ENABLE_ECHO_INPUT      0x0004

// Console text attributes
#define FOREGROUND_BLUE      0x0001
#define FOREGROUND_GREEN     0x0002
#define FOREGROUND_RED       0x0004
#define FOREGROUND_INTENSITY 0x0008
#define BACKGROUND_BLUE      0x0010
#define BACKGROUND_GREEN     0x0020
#define BACKGROUND_RED       0x0040
#define BACKGROUND_INTENSITY 0x0080

// Wait results
#define WAIT_OBJECT_0  0x00000000L
#define WAIT_TIMEOUT   0x00000102L
#define WAIT_FAILED    0xFFFFFFFF
#define INFINITE       0xFFFFFFFF

// Menu flags
#define MF_BYCOMMAND   0x00000000
#define MF_BYPOSITION  0x00000400
#define SC_CLOSE       0xF060

// Console attachment. On Windows a GUI process has no console until it borrows
// the parent's or allocates its own; on Linux stdout is already connected to
// the terminal, so there is nothing to attach.
//
// Returning FALSE from both is deliberate and is the behaviour the call sites
// want. cmdline.cpp writes:
//
//     if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole())
//         freopen("CONOUT$", "w", stdout);
//
// With both false, the freopen of the Windows-only "CONOUT$" device is skipped
// and std::cout keeps writing to the real stdout, which is what is wanted.
#define ATTACH_PARENT_PROCESS ((DWORD)-1)

// Console attachment.
//
// Both stay FALSE, deliberately. cmdline.cpp runs
//
//     if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole())
//         freopen("CONOUT$", "w", stdout);
//
// at STARTUP, so making AllocConsole succeed here opens a console window the
// moment Orbiter launches -- before the Launchpad is even shown, which is not
// what the Windows build does and not what is wanted. With both false the
// freopen of the Windows-only CONOUT$ device is skipped and std::cout keeps
// writing to the real stdout.
static inline BOOL AttachConsole(DWORD) { return FALSE; }
static inline BOOL AllocConsole(void)   { return FALSE; }
static inline BOOL FreeConsole(void)    { return TRUE;  }


// MessageBeep plays a system alert sound. There is no portable equivalent, and
// a wrong sound is worse than none, so this emits the terminal bell -- which
// is what a console application would do -- and succeeds.
#define MB_ICONHAND_SOUND  0x00000010
static inline BOOL MessageBeep(UINT) { fputc('\a', stderr); return TRUE; }

ORB_EXTERN_C_BEGIN

// Threads and synchronisation
HANDLE  CreateThread        (void *sa, SIZE_T stack,
                             DWORD (*start)(void *), void *param,
                             DWORD flags, DWORD *tid);
BOOL    TerminateThread     (HANDLE thread, DWORD exitCode);
HANDLE  CreateMutexA        (void *sa, BOOL initialOwner, LPCSTR name);
BOOL    ReleaseMutex        (HANDLE mutex);
DWORD   WaitForSingleObject (HANDLE obj, DWORD ms);
BOOL    CloseHandle         (HANDLE obj);
HANDLE  GetCurrentProcess   (void);
DWORD   GetCurrentProcessId (void);
DWORD   GetCurrentThreadId  (void);

// GetCurrentThread RETURNS A PSEUDO-HANDLE, AND THAT IS THE WHOLE POINT.
//
// On Windows this does not return a handle to the calling thread: it returns
// the constant (HANDLE)-2, a "pseudo-handle" the kernel reinterprets per call
// as whichever thread is asking. So the SAME value comes back on every
// thread, and comparing two of them can never distinguish one thread from
// another. (The real per-thread identity is GetCurrentThreadId, above, which
// is why that one is implemented over pthread_self.)
//
// It is reproduced here rather than made to work, because code in this tree
// COMPARES the results and the comparison must keep giving the answer it
// gives on Windows. D3D9Client.cpp stores GetCurrentThread() in hMainThread
// and then guards clbkGetSketchpad_const with
//
//     if (GetCurrentThread() != hMainThread) { LogErr(...); HALT(); }
//
// while D3D9Surface.cpp asserts GetCurrentThread() == GetMainThread(). Both
// tests are ALWAYS TRUE on Windows -- the guards have never fired and never
// can. Returning a real thread identity here would make them start firing on
// Linux only, turning a dormant Windows check into a Linux-only halt in the
// Sketchpad path and a Linux-only failed assert on every surface built by a
// loader thread. That would be a change to the client rather than a
// conversion of it, so the pseudo-handle is carried across verbatim.
HANDLE  GetCurrentThread    (void);

// LoadLibrary/FreeLibrary map onto dlopen/dlclose. Names ending in .dll are
// translated, and the handful of Windows system libraries Orbiter probes for
// (Psapi.dll) resolve to a sentinel whose symbols are looked up in the
// executable itself -- see psapi.h.
HMODULE LoadLibraryA        (LPCSTR name);
BOOL    FreeLibrary         (HMODULE mod);

// Console
HANDLE  GetStdHandle                 (DWORD which);
BOOL    SetConsoleTitleA             (LPCSTR title);
HWND    GetConsoleWindow             (void);
BOOL    SetConsoleMode               (HANDLE h, DWORD mode);
BOOL    GetConsoleMode               (HANDLE h, DWORD *mode);
BOOL    SetConsoleTextAttribute      (HANDLE h, WORD attr);
BOOL    SetConsoleCursorPosition     (HANDLE h, COORD pos);
BOOL    GetConsoleScreenBufferInfo   (HANDLE h, PCONSOLE_SCREEN_BUFFER_INFO info);
BOOL    ReadConsoleA                 (HANDLE h, LPVOID buf, DWORD toRead,
                                      LPDWORD read, LPVOID reserved);
BOOL    WriteConsoleA                (HANDLE h, LPCVOID buf, DWORD toWrite,
                                      LPDWORD written, LPVOID reserved);

// Menus
HMENU   GetSystemMenu   (HWND hwnd, BOOL revert);
BOOL    DeleteMenu      (HMENU menu, UINT pos, UINT flags);
HWND    GetDesktopWindow(void);

ORB_EXTERN_C_END

#define CreateMutex     CreateMutexA
#define SetConsoleTitle SetConsoleTitleA
#define ReadConsole     ReadConsoleA
#define WriteConsole    WriteConsoleA
#define LoadLibrary     LoadLibraryA

// Sleep takes milliseconds; usleep takes microseconds and rejects values
// >= 1e6, so this uses nanosleep to stay correct for long waits.
static inline void Sleep(DWORD ms) {
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000);
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static inline int lstrlenA(LPCSTR s) { return s ? (int)strlen(s) : 0; }
#define lstrlen lstrlenA

// MSVC's secure and underscore-prefixed CRT variants. The _s forms return
// errno_t and take the destination size; the behaviour that matters to callers
// here is truncation rather than overflow, which snprintf already provides.
static inline int _snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}
static inline int sprintf_s(char *buf, size_t n, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}
static inline int strcpy_s(char *dst, size_t n, const char *src) {
    if (!dst || !src || n == 0) return 22;   // EINVAL
    size_t len = strlen(src);
    if (len >= n) { dst[0] = '\0'; return 34; }  // ERANGE
    memcpy(dst, src, len + 1);
    return 0;
}
static inline int strcat_s(char *dst, size_t n, const char *src) {
    if (!dst || !src) return 22;
    size_t dl = strlen(dst), sl = strlen(src);
    if (dl + sl >= n) return 34;
    memcpy(dst + dl, src, sl + 1);
    return 0;
}
static inline int memcpy_s(void *dst, size_t dstSize, const void *src, size_t n) {
    if (!dst || !src) return 22;
    if (n > dstSize) return 34;
    memcpy(dst, src, n);
    return 0;
}

static inline char *_strdup(const char *s) { return strdup(s); }

// fopen_s returns errno_t and writes the handle through a pointer, rather
// than returning it. A null result maps to EINVAL, matching MSVC.
static inline int fopen_s(FILE **f, const char *name, const char *mode) {
    if (!f) return 22;
    *f = fopen(name, mode);
    return *f ? 0 : 22;
}

// ---------------------------------------------------------------------------
// Application, message loop, registry and system metrics
//
// Src/Orbiter/Orbiter.cpp drives a classic Win32 message loop and queries a
// handful of system settings. The loop is preserved rather than replaced: the
// implementation pumps GLFW and translates its events into the same WM_
// messages, so Orbiter.cpp's WndProc keeps working unmodified.
// ---------------------------------------------------------------------------

typedef char TCHAR;
#define TEXT(s) s
#define _T(s)   s

// PeekMessage flags
#define PM_NOREMOVE 0x0000
#define PM_REMOVE   0x0001
#define PM_NOYIELD  0x0002

// Additional messages Orbiter's WndProc handles
#define WM_NCHITTEST      0x0084
#define WM_POWERBROADCAST 0x0218
#define WM_SYSCOMMAND     0x0112
// The WM_SYSCOMMAND wParam values OVP/VulkanClient's RenderWndProc traps: the
// Alt system-menu key, and the four commands it refuses while fullscreen.
// They are plain message parameters -- numbers Windows sends -- so the values
// are the documented ones and nothing here has to implement them; the client
// simply recognises them if they arrive.
#define SC_SIZE           0xF000
#define SC_MOVE           0xF010
#define SC_MINIMIZE       0xF020
#define SC_MAXIMIZE       0xF030
#define SC_CLOSE          0xF060
#define SC_KEYMENU        0xF100
// SC_MONITORPOWER is already defined a few lines below, beside the power
// broadcast messages that go with it.
#define HTCLIENT          1
#define WA_INACTIVE       0
#define SW_MAXIMIZE       3

#define PBT_APMQUERYSUSPEND   0x0000
#define PBT_APMRESUMESUSPEND  0x0007
#define SC_MONITORPOWER       0xF170

// GetSystemMetrics indices
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1

typedef struct tagMINMAXINFO {
    POINT ptReserved;
    POINT ptMaxSize;
    POINT ptMaxPosition;
    POINT ptMinTrackSize;
    POINT ptMaxTrackSize;
} MINMAXINFO, *LPMINMAXINFO;

typedef struct tagWNDCLASSEXA {
    UINT      cbSize;
    UINT      style;
    WNDPROC   lpfnWndProc;
    int       cbClsExtra, cbWndExtra;
    HINSTANCE hInstance;
    HICON     hIcon;
    HCURSOR   hCursor;
    HBRUSH    hbrBackground;
    LPCSTR    lpszMenuName;
    LPCSTR    lpszClassName;
    HICON     hIconSm;
} WNDCLASSEXA, *LPWNDCLASSEXA;
typedef WNDCLASSEXA WNDCLASSEX;

#define IDC_WAIT      MAKEINTRESOURCE(32514)
#define IDI_APPLICATION MAKEINTRESOURCE(32512)

// SystemParametersInfo actions. Orbiter saves and restores font smoothing
// around fullscreen transitions.
#define SPI_GETFONTSMOOTHING 0x004A
#define SPI_SETFONTSMOOTHING 0x004B
// The desktop minus the compositor's reserved panels. Added for
// OVP/VulkanClient's WindowMgr, which clamps a dragged floating panel to it;
// answered from glfwGetMonitorWorkarea. See Win32Dlg.cpp.
#define SPI_GETWORKAREA      0x0030
#define SPIF_UPDATEINIFILE   0x0001
#define SPIF_SENDCHANGE      0x0002
#define SPIF_SENDWININICHANGE SPIF_SENDCHANGE

// Registry. Orbiter reads a couple of values under HKEY_CURRENT_USER; on Linux
// the implementation is backed by a small ini file under the user config dir,
// so the call sites keep working and settings still persist.
typedef struct HKEY__ *HKEY;
typedef HKEY *PHKEY;
typedef LONG LSTATUS;

#define HKEY_CLASSES_ROOT   ((HKEY)(ULONG_PTR)0x80000000)
#define HKEY_CURRENT_USER   ((HKEY)(ULONG_PTR)0x80000001)
#define HKEY_LOCAL_MACHINE  ((HKEY)(ULONG_PTR)0x80000002)

#define KEY_QUERY_VALUE 0x0001
#define KEY_SET_VALUE   0x0002
#define KEY_READ        0x20019
#define ERROR_SUCCESS   0L
#define ERROR_FILE_NOT_FOUND 2L
#define ERROR_INVALID_PARAMETER 87L
#define ERROR_INVALID_HANDLE 6L
#define ERROR_MORE_DATA 234L

// LoadLibraryEx search flags. Ignored on Linux: dlopen's search order is
// governed by rpath and LD_LIBRARY_PATH, which the build already sets.
#define LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR    0x00000100
#define LOAD_LIBRARY_SEARCH_DEFAULT_DIRS    0x00001000

// Code pages
#define CP_ACP  0
#define CP_UTF8 65001

// MultiByteToWideChar flags. MB_ERR_INVALID_CHARS makes a malformed sequence
// fail the whole call rather than being replaced, which is how a caller tells
// UTF-8 apart from a legacy single-byte string. See the note on the function
// in Platform.cpp.
#define MB_PRECOMPOSED       0x00000001
#define MB_COMPOSITE         0x00000002
#define MB_USEGLYPHCHARS     0x00000004
#define MB_ERR_INVALID_CHARS 0x00000008

ORB_EXTERN_C_BEGIN

// Message loop
BOOL    GetMessageA         (LPMSG, HWND, UINT, UINT);
BOOL    PeekMessageA        (LPMSG, HWND, UINT, UINT, UINT);
BOOL    TranslateMessage    (const MSG *);
LRESULT DispatchMessageA    (const MSG *);

// Window class and cursor
BOOL    GetClassInfoA       (HINSTANCE, LPCSTR, void *);
HICON   LoadIconA           (HINSTANCE, LPCSTR);
HCURSOR SetCursor           (HCURSOR);
int     ShowCursor          (BOOL);
BOOL    ClipCursor          (const RECT *);
int     GetSystemMetrics    (int);

// Added for OVP/VulkanClient's WindowMgr: it measures how far a dragged panel
// overlaps a dock, and reparents a dialog into a sidebar when it is docked.
// Both are implemented in Win32Dlg.cpp over the window tree it already keeps.
BOOL    IntersectRect       (LPRECT dst, const RECT *a, const RECT *b);
HWND    SetParent           (HWND child, HWND newParent);

// System
DWORD   GetLastError        (void);
DWORD   GetVersion          (void);
DWORD   GetCurrentDirectoryA(DWORD, LPSTR);
BOOL    SetCurrentDirectoryA(LPCSTR);
DWORD   GetWindowThreadProcessId(HWND, LPDWORD);
BOOL    SystemParametersInfoA(UINT, UINT, void *, UINT);
DWORD   GetConsoleProcessList(LPDWORD, DWORD);
HMODULE LoadLibraryExA      (LPCSTR, HANDLE, DWORD);

// Registry
LSTATUS RegOpenKeyExA       (HKEY, LPCSTR, DWORD, DWORD, PHKEY);
LSTATUS RegCloseKey         (HKEY);
LSTATUS RegQueryValueExA    (HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
LSTATUS RegSetValueExA      (HKEY, LPCSTR, DWORD, DWORD, const BYTE *, DWORD);

int     MultiByteToWideChar (UINT, DWORD, LPCSTR, int, LPWSTR, int);
int     WideCharToMultiByte (UINT, DWORD, LPCWSTR, int, LPSTR, int,
                             LPCSTR, BOOL *);

ORB_EXTERN_C_END

#define GetMessage           GetMessageA
#define PeekMessage          PeekMessageA
#define DispatchMessage      DispatchMessageA
#define GetClassInfo         GetClassInfoA
#define LoadIcon             LoadIconA
#define GetCurrentDirectory  GetCurrentDirectoryA
#define SetCurrentDirectory  SetCurrentDirectoryA
#define SystemParametersInfo SystemParametersInfoA
#define LoadLibraryEx        LoadLibraryExA
#define RegOpenKeyEx         RegOpenKeyExA
#define RegQueryValueEx      RegQueryValueExA
#define RegSetValueEx        RegSetValueExA

static inline int _putenv(const char *s) { return putenv((char *)s); }

// ---------------------------------------------------------------------------
// Standard control messages
//
// Combo boxes, list boxes and buttons are driven entirely through these
// messages by the launchpad tabs; the dialog implementation dispatches them to
// the corresponding ImGui widget state.
// ---------------------------------------------------------------------------

#define CB_GETEDITSEL     0x0140
#define CB_SETEDITSEL     0x0142
#define CB_ADDSTRING      0x0143
#define CB_DELETESTRING   0x0144
#define CB_GETCOUNT       0x0146
#define CB_GETCURSEL      0x0147
#define CB_GETLBTEXT      0x0148
#define CB_GETLBTEXTLEN   0x0149
#define CB_INSERTSTRING   0x014A
#define CB_RESETCONTENT   0x014B
#define CB_FINDSTRING     0x014C
#define CB_SETCURSEL      0x014E
#define CB_FINDSTRINGEXACT 0x0158
#define CB_SELECTSTRING   0x014D
// The per-item application value. The D3D9 VideoTab packs a display mode's
// width and height into it and reads them back on selection instead of
// parsing the visible label.
#define CB_GETITEMDATA    0x0150
#define CB_SETITEMDATA    0x0151
#define CB_ERR            (-1)

#define CBN_SELCHANGE     1
#define CBN_DBLCLK        2
#define CBN_EDITCHANGE    5
#define CBN_KILLFOCUS     4

#define LB_ADDSTRING      0x0180
#define LB_INSERTSTRING   0x0181
#define LB_DELETESTRING   0x0182
#define LB_RESETCONTENT   0x0184
#define LB_SETCURSEL      0x0186
#define LB_GETCURSEL      0x0188
#define LB_GETTEXT        0x0189
#define LB_GETTEXTLEN     0x018A
#define LB_GETCOUNT       0x018B
#define LB_FINDSTRING     0x018F
#define LB_SETTABSTOPS    0x0192
#define LB_SELECTSTRING   0x018C
#define LB_GETITEMDATA    0x0199
#define LB_SETITEMDATA    0x019A
#define LB_FINDSTRINGEXACT 0x01A2
#define LB_ERR            (-1)

#define LBN_SELCHANGE     1
#define LBN_DBLCLK        2

// Multi-selection list boxes
#define LB_SETSEL         0x0185
#define LB_GETSEL         0x0187
#define LB_GETSELCOUNT    0x0190
#define LB_GETSELITEMS    0x0191

// Edit control notifications
#define EN_SETFOCUS       0x0100
#define EN_KILLFOCUS      0x0200
#define EN_CHANGE         0x0300
#define EN_UPDATE         0x0400

// Button check states
#define BST_UNCHECKED     0x0000
#define BST_CHECKED       0x0001
#define BST_INDETERMINATE 0x0002

// Edit control
#define EM_GETSEL         0x00B0
#define EM_SETSEL         0x00B1
#define EM_REPLACESEL     0x00C2
#define EM_LIMITTEXT      0x00C5

// Additional message-box icons
#define MB_ICONEXCLAMATION MB_ICONWARNING
#define MB_ICONASTERISK    MB_ICONINFORMATION
#define MB_ICONHAND        MB_ICONERROR

// Additional SetWindowPos flags and z-order handles
#define SWP_FRAMECHANGED  0x0020
#define SWP_DRAWFRAME     SWP_FRAMECHANGED
#define HWND_TOP          ((HWND)0)
#define HWND_BOTTOM       ((HWND)1)
#define HWND_TOPMOST      ((HWND)-1)
#define HWND_NOTOPMOST    ((HWND)-2)

#define GWLP_HWNDPARENT   (-8)
#define GWLP_HINSTANCE    (-6)
#define GWLP_ID           (-12)

#define IDC_SIZEWE        MAKEINTRESOURCE(32644)
#define IDC_SIZENS        MAKEINTRESOURCE(32645)
#define IDC_HAND          MAKEINTRESOURCE(32649)

// Window creation
#define CW_USEDEFAULT     ((int)0x80000000)
#define WS_EX_TOPMOST     0x00000008L
#define WS_EX_TOOLWINDOW  0x00000080L

// Resource and global-memory handles. Orbiter reads embedded resources
// through FindResource/LoadResource/LockResource.
typedef HANDLE HGLOBAL;
typedef HANDLE HRSRC;
typedef HANDLE HLOCAL;

#define ERROR_PATH_NOT_FOUND 3L
#define ERROR_ACCESS_DENIED  5L

ORB_EXTERN_C_BEGIN

HWND    CreateWindowExA   (DWORD exStyle, LPCSTR className, LPCSTR windowName,
                           DWORD style, int x, int y, int w, int h,
                           HWND parent, HMENU menu, HINSTANCE inst,
                           LPVOID param);
DWORD   GetModuleFileNameA(HMODULE mod, LPSTR filename, DWORD size);
void    OutputDebugStringA(LPCSTR str);
HBITMAP CreateDIBSection  (HDC hdc, const BITMAPINFO *bmi, UINT usage,
                           void **bits, HANDLE section, DWORD offset);

ORB_EXTERN_C_END

// CreateWindow is CreateWindowEx with no extended style, as in the SDK.
#define CreateWindowA(cls, name, style, x, y, w, h, parent, menu, inst, param) \
    CreateWindowExA(0, cls, name, style, x, y, w, h, parent, menu, inst, param)

#define CreateWindow       CreateWindowA
#define CreateWindowEx     CreateWindowExA
#define GetModuleFileName  GetModuleFileNameA
#define OutputDebugString  OutputDebugStringA

// LoadLibraryEx: load without executing initialisers, for resource-only use.
#define LOAD_LIBRARY_AS_DATAFILE 0x00000002

// Directory change notification. Orbiter watches the scenario folder so the
// launchpad tree refreshes when files appear; the implementation is backed by
// inotify.
#define FILE_NOTIFY_CHANGE_FILE_NAME  0x00000001
#define FILE_NOTIFY_CHANGE_DIR_NAME   0x00000002
#define FILE_NOTIFY_CHANGE_LAST_WRITE 0x00000010

#ifndef ARRAYSIZE
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define MAKELONG(a, b) ((LONG)(((WORD)(a)) | (((DWORD)((WORD)(b))) << 16)))

// Owner-draw. The launchpad paints its tab strip itself and receives this
// block with WM_DRAWITEM.
typedef struct tagDRAWITEMSTRUCT {
    UINT      CtlType;
    UINT      CtlID;
    UINT      itemID;
    UINT      itemAction;
    UINT      itemState;
    HWND      hwndItem;
    HDC       hDC;
    RECT      rcItem;
    ULONG_PTR itemData;
} DRAWITEMSTRUCT, *LPDRAWITEMSTRUCT, *PDRAWITEMSTRUCT;

#define ODT_MENU     1
#define ODT_LISTBOX  2
#define ODT_COMBOBOX 3
#define ODT_BUTTON   4
#define ODT_STATIC   5

#define ODA_DRAWENTIRE 0x0001
#define ODA_SELECT     0x0002
#define ODA_FOCUS      0x0004

#define ODS_SELECTED   0x0001
#define ODS_DISABLED   0x0004
#define ODS_FOCUS      0x0010

// Virtual key codes. These are the values dialog keyboard handling compares
// against, and they are the ones a WM_KEYDOWN carries in wParam.
#define VK_BACK     0x08
#define VK_TAB      0x09
#define VK_RETURN   0x0D
#define VK_SHIFT    0x10
#define VK_CONTROL  0x11
#define VK_MENU     0x12
#define VK_ESCAPE   0x1B
#define VK_SPACE    0x20
#define VK_PRIOR    0x21
#define VK_NEXT     0x22
#define VK_END      0x23
#define VK_HOME     0x24
#define VK_LEFT     0x25
#define VK_UP       0x26
#define VK_RIGHT    0x27
#define VK_DOWN     0x28
#define VK_DELETE   0x2E
#define VK_F1       0x70
#define VK_F2       0x71
#define VK_F3       0x72
#define VK_F4       0x73

// Registry access rights.
//
// Only the read/write distinction is meaningful here: RegOpenKeyEx reports a
// missing key as an error when opened for reading, but may create one when
// opened for writing.
#define KEY_CREATE_SUB_KEY     0x0004
#define KEY_ENUMERATE_SUB_KEYS 0x0008
#define KEY_NOTIFY             0x0010
#define KEY_WRITE              0x20006
#define KEY_ALL_ACCESS         0xF003F

// Text alignment for SetTextAlign
#define TA_LEFT       0
#define TA_RIGHT      2
#define TA_CENTER     6
#define TA_TOP        0
#define TA_BOTTOM     8
#define TA_BASELINE   24

// LoadImage: return a DIB section rather than a device-dependent bitmap.
#define LR_CREATEDIBSECTION 0x2000
#define LR_MONOCHROME       0x0001

// Background mix modes for SetBkMode
#define TRANSPARENT 1
#define OPAQUE      2

// Progress bar
#define PBM_SETRANGE  (WM_USER + 1)
#define PBM_SETPOS    (WM_USER + 2)
#define PBM_DELTAPOS  (WM_USER + 3)
#define PBM_SETSTEP   (WM_USER + 4)
#define PBM_STEPIT    (WM_USER + 5)
#define PBM_SETRANGE32 (WM_USER + 6)
#define PBM_GETRANGE  (WM_USER + 7)
#define PBM_GETPOS    (WM_USER + 8)
#define PROGRESS_CLASSA "msctls_progress32"
#define PROGRESS_CLASS  PROGRESS_CLASSA

// LoadImage types
#define IMAGE_BITMAP 0
#define IMAGE_ICON   1
#define IMAGE_CURSOR 2
#define LR_LOADFROMFILE 0x0010
#define LR_DEFAULTSIZE  0x0040
#define LR_SHARED       0x8000

// Additional window styles and show commands
#define WS_SIZEBOX        WS_THICKFRAME
#define WS_TILEDWINDOW    (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | \
                           WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)

// The SDK's other spellings of the same bits. They are aliases, not extra
// styles, and the dialog editor emits them freely -- ToolKit.rc writes
// WS_CHILDWINDOW where every other script in the tree writes WS_CHILD, and
// rc2cpp.py stopped on it and converted none of that module's dialogs.
#define WS_TILED          WS_OVERLAPPED
#define WS_ICONIC         WS_MINIMIZE
#define WS_CHILDWINDOW    WS_CHILD
#define WS_OVERLAPPEDWINDOW (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | \
                             WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)
#define WS_POPUPWINDOW    (WS_POPUP | WS_BORDER | WS_SYSMENU)

// THE EXTENDED STYLES, COMPLETE.
//
// The handful this shim happened to need were added one at a time as they came
// up, scattered through this header. That worked while one .rc was converted;
// with every module's script going through rc2cpp.py it stops working, because
// the dialog editor writes the full set and ANY unresolved name aborts that
// module's whole conversion -- one missing WS_EX_ and the module has no
// dialogs at all. WS_EX_WINDOWEDGE was the one ToolKit.rc stopped on.
//
// So the SDK's list is given in full and in one place, each guarded, so the
// earlier scattered definitions still stand and nothing is defined twice.
// Values are the SDK's.
#ifndef WS_EX_DLGMODALFRAME
#define WS_EX_DLGMODALFRAME     0x00000001L
#endif
#ifndef WS_EX_NOPARENTNOTIFY
#define WS_EX_NOPARENTNOTIFY    0x00000004L
#endif
#ifndef WS_EX_ACCEPTFILES
#define WS_EX_ACCEPTFILES       0x00000010L
#endif
#ifndef WS_EX_MDICHILD
#define WS_EX_MDICHILD          0x00000040L
#endif
#ifndef WS_EX_WINDOWEDGE
#define WS_EX_WINDOWEDGE        0x00000100L
#endif
#ifndef WS_EX_CONTEXTHELP
#define WS_EX_CONTEXTHELP       0x00000400L
#endif
#ifndef WS_EX_LEFT
#define WS_EX_LEFT              0x00000000L
#endif
#ifndef WS_EX_LTRREADING
#define WS_EX_LTRREADING        0x00000000L
#endif
#ifndef WS_EX_RIGHTSCROLLBAR
#define WS_EX_RIGHTSCROLLBAR    0x00000000L
#endif
#ifndef WS_EX_RTLREADING
#define WS_EX_RTLREADING        0x00002000L
#endif
#ifndef WS_EX_LEFTSCROLLBAR
#define WS_EX_LEFTSCROLLBAR     0x00004000L
#endif
#ifndef WS_EX_LAYERED
#define WS_EX_LAYERED           0x00080000L
#endif
#ifndef WS_EX_NOINHERITLAYOUT
#define WS_EX_NOINHERITLAYOUT   0x00100000L
#endif
#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif
#ifndef WS_EX_LAYOUTRTL
#define WS_EX_LAYOUTRTL         0x00400000L
#endif
#ifndef WS_EX_COMPOSITED
#define WS_EX_COMPOSITED        0x02000000L
#endif
#ifndef WS_EX_OVERLAPPEDWINDOW
#define WS_EX_OVERLAPPEDWINDOW  (WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE)
#endif
#ifndef WS_EX_PALETTEWINDOW
#define WS_EX_PALETTEWINDOW     (WS_EX_WINDOWEDGE | WS_EX_TOOLWINDOW | \
                                 WS_EX_TOPMOST)
#endif
#define SW_SHOWNOACTIVATE 4
#define SW_SHOWMINIMIZED  2

// RedrawWindow flags
#define RDW_INVALIDATE   0x0001
#define RDW_ERASE        0x0004
#define RDW_UPDATENOW    0x0100
#define RDW_ALLCHILDREN  0x0080

// Additional GetSystemMetrics indices
#define SM_CYMIN         29
#define SM_CXSIZEFRAME   32
#define SM_CYSIZEFRAME   33
#define SM_CXFIXEDFRAME  7
#define SM_CYFIXEDFRAME  8
#define SM_CYCAPTION     4

ORB_EXTERN_C_BEGIN

HBRUSH  GetSysColorBrush (int index);
HDC     GetWindowDC      (HWND hwnd);
BOOL    IsIconic         (HWND hwnd);
BOOL    IsDialogMessageA (HWND hDlg, LPMSG msg);
void    PostQuitMessage  (int exitCode);
BOOL    RedrawWindow     (HWND hwnd, const RECT *update, void *rgnUpdate,
                          UINT flags);
BOOL    StretchBlt       (HDC dst, int x, int y, int w, int h,
                          HDC src, int sx, int sy, int sw, int sh, DWORD rop);
HANDLE  LoadImageA       (HINSTANCE inst, LPCSTR name, UINT type,
                          int cx, int cy, UINT load);

ORB_EXTERN_C_END

#define IsDialogMessage IsDialogMessageA
#define LoadImage       LoadImageA

// Sizing and non-client messages the render window's WndProc handles.
#define WM_ENTERSIZEMOVE    0x0231
#define WM_EXITSIZEMOVE     0x0232
#define WM_NCLBUTTONDOWN    0x00A1
#define WM_NCLBUTTONDBLCLK  0x00A3
#define WM_NCMOUSEMOVE      0x00A0

// WM_SIZING edge codes. wParam names which edge the user is dragging, and
// DX9ExtMFD's MFDWindow clamps the rectangle differently for each so the MFD
// stays square.
#define WMSZ_LEFT        1
#define WMSZ_RIGHT       2
#define WMSZ_TOP         3
#define WMSZ_TOPLEFT     4
#define WMSZ_TOPRIGHT    5
#define WMSZ_BOTTOM      6
#define WMSZ_BOTTOMLEFT  7
#define WMSZ_BOTTOMRIGHT 8
#define WM_SIZING        0x0214

// _countof yields the element count of an array. MSVC provides it; the SDK
// spells the same thing ARRAYSIZE, which is already defined above.
#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif

// MAKEFOURCC packs four characters into the little-endian DWORD that file
// formats use as a type tag. It lives in <mmsystem.h>, which <windows.h>
// includes unless WIN32_LEAN_AND_MEAN is set -- so on Windows any file that
// includes <windows.h> alone has it, and that is how ZTreeMgr.cpp gets it for
// its tile-archive magic number, MAKEFOURCC('T','X',1,0). ZTreeMgr.h includes
// only <iostream> and <windows.h>, so there is nowhere else for it to come
// from here.
//
// The graphics client's own d3d9.h defines the identical macro for DDS format
// codes and guards it with #ifndef, so whichever is read first wins and the
// packing is the same either way.
#ifndef MAKEFOURCC
#define MAKEFOURCC(a, b, c, d) \
    ((DWORD)(BYTE)(a) | ((DWORD)(BYTE)(b) << 8) | \
     ((DWORD)(BYTE)(c) << 16) | ((DWORD)(BYTE)(d) << 24))
#endif

// MSVC's sscanf_s. Its %s conversion takes an extra buffer-size argument after
// the pointer, which plain sscanf does not:
//
//     sscanf_s(line, "ATCH_MASK %s", mask, (int)_countof(mask));
//
// Forwarding to sscanf leaves that size as an unused trailing vararg, which is
// harmless -- sscanf stops consuming arguments when the format runs out.
//
// CAUTION: that holds only when every %s is the LAST conversion in the format,
// because only then is the size argument genuinely trailing. It is NOT a
// property of having just one %s. Any conversion AFTER a %s consumes the size
// in place of its own pointer:
//
//     sscanf_s(s, "%s %u %u", buf, 8, &day, &year)
//
// forwards as sscanf(s, "%s %u %u", buf, 8, &day, &year), where the first %u
// writes an unsigned int through the ADDRESS 8 -- a wild store, not a no-op.
// D3D9Client's BuildDate() is exactly this shape, and it is why the graphics
// client's port of it uses plain sscanf instead.
//
// So a call site is safe here only if every %s is trailing. Anything else --
// a %s followed by further conversions, or two or more %s -- must be rewritten
// to plain sscanf without the sizes. -Wformat catches these; do not silence it.
#define sscanf_s sscanf

// MSVC's underscore-prefixed floating-point classifiers. The C99 names are
// macros in <math.h>, so these forward to them rather than being declared.
#ifdef __cplusplus
#include <cmath>
static inline int _isnan(double x)   { return std::isnan(x)   ? 1 : 0; }
static inline int _finite(double x)  { return std::isfinite(x) ? 1 : 0; }
static inline int _isinf(double x)   { return std::isinf(x)   ? 1 : 0; }
#else
#include <math.h>
static inline int _isnan(double x)   { return isnan(x)   ? 1 : 0; }
static inline int _finite(double x)  { return isfinite(x) ? 1 : 0; }
static inline int _isinf(double x)   { return isinf(x)   ? 1 : 0; }
#endif

ORB_EXTERN_C_BEGIN

BOOL   IsChild            (HWND parent, HWND child);
BOOL   PostThreadMessageA (DWORD threadId, UINT msg, WPARAM wp, LPARAM lp);
HANDLE CreateEventA       (void *sa, BOOL manualReset, BOOL initialState,
                           LPCSTR name);
BOOL   SetEvent           (HANDLE ev);
BOOL   ResetEvent         (HANDLE ev);

ORB_EXTERN_C_END

#define PostThreadMessage PostThreadMessageA
#define CreateEvent       CreateEventA

ORB_EXTERN_C_BEGIN

HANDLE FindFirstChangeNotificationA (LPCSTR path, BOOL subtree, DWORD filter);
BOOL   FindNextChangeNotification   (HANDLE h);
BOOL   FindCloseChangeNotification  (HANDLE h);

// Resource loading. Orbiter reads version strings and dialog templates from
// module resources; on Linux these are served from the parsed .rc data.
HANDLE FindResourceA  (HMODULE mod, LPCSTR name, LPCSTR type);
HANDLE LoadResource   (HMODULE mod, HANDLE res);
LPVOID LockResource   (HANDLE resData);
DWORD  SizeofResource (HMODULE mod, HANDLE res);

short  RegisterClassExA (const WNDCLASSEXA *wc);

// ShellExecute opens a document or URL with the desktop's default handler,
// which is xdg-open on Linux.
HINSTANCE ShellExecuteA (HWND hwnd, LPCSTR op, LPCSTR file, LPCSTR params,
                         LPCSTR dir, int showCmd);

ORB_EXTERN_C_END

#define FindFirstChangeNotification FindFirstChangeNotificationA
#define FindResource   FindResourceA
#define RegisterClassEx RegisterClassExA
#define ShellExecute   ShellExecuteA

// _splitpath decomposes a path into drive, directory, filename and extension.
// There are no drive letters on Linux, so the drive buffer is always empty.
static inline void _splitpath(const char *path, char *drive, char *dir,
                       char *fname, char *ext) {
    if (drive) drive[0] = '\0';
    if (dir)   dir[0]   = '\0';
    if (fname) fname[0] = '\0';
    if (ext)   ext[0]   = '\0';
    if (!path) return;

    const char *slash = strrchr(path, '/');
    const char *base  = slash ? slash + 1 : path;
    if (dir && slash) {
        size_t n = (size_t)(slash - path) + 1;
        memcpy(dir, path, n);
        dir[n] = '\0';
    }
    const char *dot = strrchr(base, '.');
    if (fname) {
        size_t n = dot ? (size_t)(dot - base) : strlen(base);
        memcpy(fname, base, n);
        fname[n] = '\0';
    }
    if (ext && dot) strcpy(ext, dot);
}

// MSVC's floating-point error hook. Orbiter defines _matherr() to trap domain
// and range errors during flight; glibc's equivalent SVID struct is named
// `exception` rather than `_exception`, so the MSVC spelling is provided here
// and Orbiter.cpp's definition compiles unchanged.
struct _exception {
    int    type;
    char  *name;
    double arg1;
    double arg2;
    double retval;
};

// ---------------------------------------------------------------------------
// Version info, error formatting and process handles
//
// Src/Orbiter/Log.cpp writes a diagnostic header at startup: the build version
// from the executable's resources, the system error text for a failed call,
// and the list of loaded modules.
// ---------------------------------------------------------------------------

// VS_FIXEDFILEINFO is the binary block VerQueryValue returns for "\\". Layout
// matches the SDK because the field order is part of the resource format.
typedef struct tagVS_FIXEDFILEINFO {
    DWORD dwSignature;
    DWORD dwStrucVersion;
    DWORD dwFileVersionMS;
    DWORD dwFileVersionLS;
    DWORD dwProductVersionMS;
    DWORD dwProductVersionLS;
    DWORD dwFileFlagsMask;
    DWORD dwFileFlags;
    DWORD dwFileOS;
    DWORD dwFileType;
    DWORD dwFileSubtype;
    DWORD dwFileDateMS;
    DWORD dwFileDateLS;
} VS_FIXEDFILEINFO;

#define FORMAT_MESSAGE_ALLOCATE_BUFFER 0x00000100
#define FORMAT_MESSAGE_IGNORE_INSERTS  0x00000200
#define FORMAT_MESSAGE_FROM_STRING     0x00000400
#define FORMAT_MESSAGE_FROM_SYSTEM     0x00001000

#define LANG_NEUTRAL     0x00
#define SUBLANG_DEFAULT  0x01
#define MAKELANGID(p, s) ((((WORD)(s)) << 10) | (WORD)(p))

#define PROCESS_QUERY_INFORMATION 0x0400
#define PROCESS_VM_READ           0x0010

ORB_EXTERN_C_BEGIN

DWORD  GetFileVersionInfoSizeA (LPCSTR filename, LPDWORD handle);
BOOL   GetFileVersionInfoA     (LPCSTR filename, DWORD handle, DWORD len,
                                LPVOID data);
BOOL   VerQueryValueA          (LPCVOID block, LPCSTR subBlock,
                                LPVOID *buf, UINT *len);

DWORD  FormatMessageA          (DWORD flags, LPCVOID source, DWORD msgId,
                                DWORD langId, LPSTR buf, DWORD size,
                                va_list *args);
// Takes void* rather than HLOCAL: FORMAT_MESSAGE_ALLOCATE_BUFFER hands back a
// buffer through an LPSTR, and callers pass that straight to LocalFree without
// a cast, which the SDK's HLOCAL-as-void* typedef permits.
HLOCAL LocalFree               (LPVOID mem);

HANDLE OpenProcess             (DWORD access, BOOL inherit, DWORD pid);
DWORD  GetProcessId            (HANDLE process);
BOOL   SHCreateDirectoryExA    (HWND hwnd, LPCSTR path, void *sa);

ORB_EXTERN_C_END

#define SHCreateDirectoryEx SHCreateDirectoryExA

// MSVC's bounded string copy. Truncation is an error in the _s contract, so
// this reports ERANGE rather than silently cutting the string short.
static inline int strncpy_s(char *dst, size_t dstSize, const char *src, size_t n) {
    if (!dst || !src || dstSize == 0) return 22;
    size_t len = strnlen(src, n);
    if (len >= dstSize) { dst[0] = '\0'; return 34; }
    memcpy(dst, src, len);
    dst[len] = '\0';
    return 0;
}

#ifdef __cplusplus
// MSVC's sprintf_s comes in two forms, and both are used in this tree:
//
//     sprintf_s(buf, size, fmt, ...)      the explicit-size form, defined
//                                         further up this header
//     sprintf_s(buf, fmt, ...)            a template that deduces the size
//                                         of a char array destination
//
// TransX uses the second -- sprintf_s(tbuffer, "%.12g", value) -- which the
// explicit-size form alone cannot express: the format string binds to the
// size parameter and the first vararg to the format, giving
// "cannot convert 'double' to 'const char*'". Only the template is added
// here; it overloads against the existing function rather than replacing it.
template <size_t N>
inline int sprintf_s(char (&buf)[N], const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int r = vsnprintf(buf, N, fmt, ap);
    va_end(ap);
    return r;
}

// strcpy_s and strcat_s have the same two forms, for the same reason. MSVC
// calls these the "secure template overloads": in C++ they are on by default
// (_CRT_SECURE_CPP_OVERLOAD_STANDARD_NAMES), and any call whose destination
// is a fixed-size char array may leave the size out, because the array's
// extent carries it.
//
// The graphics client uses the short form in both:
//
//     strcat_s(buf, "AUTOGENMIPMAP ")   D3D9Surface.cpp / VulkanSurface.cpp
//     strcpy_s(fnt.lfFaceName, "Courier New")   D3D9Client.cpp / VulkanClient.cpp
//
// Against the explicit-size functions above those read as "too few arguments".
// As with sprintf_s these are added as overloads and change nothing about the
// three-argument calls already in the tree; each forwards straight to the
// explicit-size version with N as the bound, which is what MSVC's template
// does too.
template <size_t N>
inline int strcpy_s(char (&dst)[N], const char *src) {
    return strcpy_s(dst, N, src);
}

template <size_t N>
inline int strcat_s(char (&dst)[N], const char *src) {
    return strcat_s(dst, N, src);
}
#endif // __cplusplus

#define GetFileVersionInfoSize GetFileVersionInfoSizeA
#define GetFileVersionInfo     GetFileVersionInfoA
#define VerQueryValue          VerQueryValueA
#define FormatMessage          FormatMessageA

// MSVC's 64-bit file positioning. The POSIX equivalents already take 64-bit
// offsets on a modern glibc, so these forward directly.
static inline int _fseeki64(FILE *f, int64_t off, int origin) {
    return fseeko(f, (off_t)off, origin);
}
static inline int64_t _ftelli64(FILE *f) {
    return (int64_t)ftello(f);
}

// File attributes and directory creation.
//
// TerrainToolKit checks for a tile cache directory and creates it:
//     DWORD a = GetFileAttributes(path);
//     if (a == INVALID_FILE_ATTRIBUTES) CreateDirectory(path, NULL);
//     else if (a & FILE_ATTRIBUTE_DIRECTORY) ...
// Only the directory bit and the "does not exist" sentinel are meaningful
// here, so the attribute word carries just those.
#define INVALID_FILE_ATTRIBUTES  ((DWORD)-1)
#define FILE_ATTRIBUTE_READONLY  0x00000001
#define FILE_ATTRIBUTE_HIDDEN    0x00000002
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define FILE_ATTRIBUTE_ARCHIVE   0x00000020
#define FILE_ATTRIBUTE_NORMAL    0x00000080

ORB_EXTERN_C_BEGIN
DWORD GetFileAttributesA (LPCSTR path);
BOOL  CreateDirectoryA   (LPCSTR path, void *sa);
BOOL  DeleteFileA        (LPCSTR path);
ORB_EXTERN_C_END

#define GetFileAttributes GetFileAttributesA
#define CreateDirectory   CreateDirectoryA
#define DeleteFile        DeleteFileA

// Extended window style for a sunken 1px border.
#define WS_EX_STATICEDGE  0x00020000L

// Mouse tracking. WM_MOUSELEAVE is posted after TrackMouseEvent is armed, so a
// control can un-highlight when the pointer leaves it.
#define WM_MOUSELEAVE     0x02A3
#define WM_MOUSEHOVER     0x02A1
#define TME_LEAVE         0x00000002
#define TME_HOVER         0x00000001

typedef struct tagTRACKMOUSEEVENT {
    DWORD cbSize;
    DWORD dwFlags;
    HWND  hwndTrack;
    DWORD dwHoverTime;
} TRACKMOUSEEVENT, *LPTRACKMOUSEEVENT;

// PtInRect: inclusive of left/top, exclusive of right/bottom, as Win32 does.
static inline BOOL PtInRect(const RECT *r, POINT p) {
    if (!r) return FALSE;
    return (p.x >= r->left && p.x < r->right &&
            p.y >= r->top  && p.y < r->bottom) ? TRUE : FALSE;
}

ORB_EXTERN_C_BEGIN
BOOL TrackMouseEvent (LPTRACKMOUSEEVENT tme);
// A masked blit: the colour given is treated as transparent. Implemented in
// Gdi.cpp alongside BitBlt/StretchBlt.
BOOL TransparentBlt  (HDC dst, int x, int y, int w, int h,
                      HDC src, int sx, int sy, int sw, int sh,
                      UINT transparentColour);
ORB_EXTERN_C_END

// ---------------------------------------------------------------------------
// GDI: off-screen bitmaps, rectangle fill, extent measurement and clipping.
//
// All of these are called by TerrainToolKit's gcTableView.cpp, which paints
// its property tree into an off-screen bitmap and blits it:
//
//   hBuf = CreateCompatibleBitmap(_hDC, w, h);   // the back buffer
//   FillRect(hBM, &rect, hBr1);                  // row backgrounds
//   GetTextExtentExPointA(...)                   // label column width
//   HRGN hRgn = CreateRectRgn(0, 0, w, h);
//   SelectClipRgn(_hDC, hRgn);                   // clip to the control
//   ExcludeClipRect(_hDC, l, t, r, b);           // hole per child control
//   TextOutW(hBM, ..., ws.c_str(), ...)          // UTF-16 values
//
// GetTextExtentExPointA is NOT GetTextExtentPoint32A: it additionally takes a
// maximum extent and optional per-character width array, and reports how many
// characters fit. gcTableView passes 100000 and two nulls, wanting only the
// total size, but the signature has to match or the call does not compile.
// ---------------------------------------------------------------------------

ORB_DECLARE_HANDLE(HRGN);

// WGL: the Win32 binding for OpenGL. Dragonfly's instrument panel renders
// through it and declares an HGLRC member, so the handle type must exist for
// its header to compile. Only the type is provided -- the module is built for
// its physics and systems model, and its GL rendering path is inert without a
// graphics client to give it a surface.
ORB_DECLARE_HANDLE(HGLRC);

// WGL pixel format description. Dragonfly's instrument panel asks for a GL
// pixel format before creating its context. The layout matches the SDK
// because the struct is passed to ChoosePixelFormat/SetPixelFormat by size.
typedef struct tagPIXELFORMATDESCRIPTOR {
    WORD  nSize;
    WORD  nVersion;
    DWORD dwFlags;
    BYTE  iPixelType;
    BYTE  cColorBits;
    BYTE  cRedBits, cRedShift;
    BYTE  cGreenBits, cGreenShift;
    BYTE  cBlueBits, cBlueShift;
    BYTE  cAlphaBits, cAlphaShift;
    BYTE  cAccumBits;
    BYTE  cAccumRedBits, cAccumGreenBits, cAccumBlueBits, cAccumAlphaBits;
    BYTE  cDepthBits, cStencilBits, cAuxBuffers;
    BYTE  iLayerType, bReserved;
    DWORD dwLayerMask, dwVisibleMask, dwDamageMask;
} PIXELFORMATDESCRIPTOR, *PPIXELFORMATDESCRIPTOR, *LPPIXELFORMATDESCRIPTOR;

#define PFD_TYPE_RGBA          0
#define PFD_TYPE_COLORINDEX    1
#define PFD_MAIN_PLANE         0
#define PFD_DOUBLEBUFFER       0x00000001
#define PFD_STEREO             0x00000002
#define PFD_DRAW_TO_WINDOW     0x00000004
#define PFD_DRAW_TO_BITMAP     0x00000008
#define PFD_SUPPORT_GDI        0x00000010
#define PFD_SUPPORT_OPENGL     0x00000020

// Additional font output precision constants.
#define OUT_STRING_PRECIS      1
#define OUT_CHARACTER_PRECIS   2
#define OUT_STROKE_PRECIS      3
#define OUT_TT_PRECIS          4
#define OUT_DEVICE_PRECIS      5
#define OUT_RASTER_PRECIS      6
#define OUT_TT_ONLY_PRECIS     7
#define OUT_OUTLINE_PRECIS     8

ORB_EXTERN_C_BEGIN

HBITMAP CreateCompatibleBitmap (HDC hdc, int w, int h);
int     FillRect               (HDC hdc, const RECT *r, HBRUSH brush);
BOOL    GetTextExtentExPointA  (HDC hdc, LPCSTR str, int len, int maxExtent,
                                LPINT lpnFit, LPINT alpDx, LPSIZE size);
BOOL    TextOutW               (HDC hdc, int x, int y, LPCWSTR str, int len);

HRGN    CreateRectRgn          (int left, int top, int right, int bottom);
// A hatched brush. The pattern is drawn as a real hatch by Gdi.cpp rather
// than approximated with a solid fill, since Dragonfly uses it to mark
// disabled panel regions and a flat colour would not read as disabled.
HBRUSH  CreateHatchBrush       (int style, COLORREF colour);

// Used by the Sketchpad the graphics client draws its 2D output with -- the
// HUD, every MFD, panel instruments. All four are ordinary GDI operations
// this layer can record and replay; they simply had no caller until a client
// existed.
BOOL    GetTextMetricsA        (HDC hdc, LPTEXTMETRIC tm);
COLORREF SetPixel              (HDC hdc, int x, int y, COLORREF colour);

// GetPixel, and the reason it is here.
//
// OVP/VulkanClient's WindowMgr recolours its title-bar graphic pixel by pixel:
// it selects the loaded bitmap into one memory DC and a compatible bitmap into
// another, reads every source pixel with GetPixel, computes a colour from the
// green and blue channels, and writes it with SetPixel. Without this the
// sidebar has no title bars at all.
//
// It reads THE BITMAP SELECTED INTO THE DC, not the screen -- which is what
// Win32 does for a memory DC and the only thing that can be answered here,
// because a screen DC in this shim is a display-list recorder with no pixels
// behind it. A DC with no selected bitmap therefore returns CLR_INVALID, which
// is what Win32 returns for a point outside the clipping region.
#define CLR_INVALID ((COLORREF)0xFFFFFFFF)
COLORREF GetPixel              (HDC hdc, int x, int y);
BOOL    SetViewportOrgEx       (HDC hdc, int x, int y, LPPOINT prev);

// The read half of SetViewportOrgEx, and three more GDI entry points the
// Sketchpad's GDI implementation (OVP/VulkanClient/GDIPad.cpp) calls and
// nothing else in the tree did. All four are Win32 GDI, not Direct3D; see
// Gdi.cpp for what each records.
//
// Note the two count-array types: PolyPolygon takes `const int *` and
// PolyPolyline takes `const DWORD *`. That asymmetry is Win32's own and is
// reproduced so the call sites need no cast.
BOOL    GetViewportOrgEx       (HDC hdc, LPPOINT pt);
BOOL    PolyPolygon            (HDC hdc, const POINT *pts, const int *counts, int nfig);
BOOL    PolyPolyline           (HDC hdc, const POINT *pts, const DWORD *counts, DWORD nfig);
int     DrawTextA              (HDC hdc, LPCSTR str, int len, LPRECT rc, UINT format);

// DrawText's format flags. Only the three GDIPad::TextBox asks for are
// honoured (see DrawTextA); the rest are defined so call sites compile and
// are accepted and ignored rather than pretended.
#define DT_TOP              0x00000000
#define DT_LEFT             0x00000000
#define DT_CENTER           0x00000001
#define DT_RIGHT            0x00000002
#define DT_VCENTER          0x00000004
#define DT_BOTTOM           0x00000008
#define DT_WORDBREAK        0x00000010
#define DT_SINGLELINE       0x00000020
#define DT_EXPANDTABS       0x00000040
#define DT_NOCLIP           0x00000100
#define DT_CALCRECT         0x00000400
#define DT_NOPREFIX         0x00000800

// WGL: pixel format selection and context management.
//
// These have no meaning without a GL surface, and the Launchpad-phase renderer
// is Vulkan. They are declared so Dragonfly's instrument panel compiles and
// links; the implementations report failure rather than pretending to
// succeed, so the panel's GL path disables itself instead of drawing into a
// context that does not exist. When a graphics client provides a real GL or
// interop surface, these are the four functions to implement against it.
int   ChoosePixelFormat   (HDC hdc, const PIXELFORMATDESCRIPTOR *pfd);
BOOL  SetPixelFormat      (HDC hdc, int fmt, const PIXELFORMATDESCRIPTOR *pfd);
int   DescribePixelFormat (HDC hdc, int fmt, UINT bytes,
                           LPPIXELFORMATDESCRIPTOR pfd);
HGLRC wglCreateContext    (HDC hdc);
BOOL  wglMakeCurrent      (HDC hdc, HGLRC ctx);
BOOL  wglDeleteContext    (HGLRC ctx);
HGLRC wglGetCurrentContext(void);
int     SelectClipRgn          (HDC hdc, HRGN rgn);
int     ExcludeClipRect        (HDC hdc, int left, int top,
                                int right, int bottom);

ORB_EXTERN_C_END

#define GetTextExtentExPoint GetTextExtentExPointA
#define GetTextMetrics       GetTextMetricsA
#define DrawText             DrawTextA

// SelectClipRgn / ExcludeClipRect return one of these.
#define ERRORRGN       0
#define NULLREGION     1
#define SIMPLEREGION   2
#define COMPLEXREGION  3

// Clipboard and global memory.
//
// TerrainToolKit's table view copies a selection as text:
//     OpenClipboard(hWnd); EmptyClipboard();
//     HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, len);
//     memcpy(GlobalLock(h), text, len); GlobalUnlock(h);
//     SetClipboardData(CF_TEXT, h); CloseClipboard();
//
// GlobalAlloc/Lock/Unlock are a plain heap allocation here -- the moveable
// handle model has no equivalent and nothing in this tree relies on it. The
// clipboard itself is backed by the desktop selection.
#define GMEM_FIXED     0x0000
#define GMEM_MOVEABLE  0x0002
#define GMEM_ZEROINIT  0x0040
#define GHND           (GMEM_MOVEABLE | GMEM_ZEROINIT)
#define GPTR           (GMEM_FIXED | GMEM_ZEROINIT)

#define CF_TEXT        1
#define CF_BITMAP      2
#define CF_UNICODETEXT 13

ORB_EXTERN_C_BEGIN
HGLOBAL GlobalAlloc      (UINT flags, SIZE_T bytes);
LPVOID  GlobalLock       (HGLOBAL mem);
BOOL    GlobalUnlock     (HGLOBAL mem);
HGLOBAL GlobalFree       (HGLOBAL mem);
SIZE_T  GlobalSize       (HGLOBAL mem);

BOOL    OpenClipboard    (HWND owner);
BOOL    CloseClipboard   (void);
BOOL    EmptyClipboard   (void);
HANDLE  SetClipboardData (UINT format, HANDLE mem);
HANDLE  GetClipboardData (UINT format);
ORB_EXTERN_C_END

// The real <windows.h> pulls in <mmsystem.h> and <commdlg.h> unless
// WIN32_LEAN_AND_MEAN is defined, which is why Log.cpp can call timeGetTime()
// and TerrainToolKit/ToolKit.h can name OPENFILENAMEA while including only
// <windows.h>. Included last so their own include of this header is a no-op
// against the guard above.
#include <mmsystem.h>
#include <commdlg.h>

#define _DOMAIN    1   // argument domain error
#define _SING      2   // singularity
#define _OVERFLOW  3   // overflow range error
#define _PLOSS     4   // partial loss of significance
#define _TLOSS     5   // total loss of significance
#define _UNDERFLOW 6   // underflow range error

// ---------------------------------------------------------------------------
// MSVC CRT and SDK odds and ends used by XRSound.
// ---------------------------------------------------------------------------

// MessageBox flag: bring the box to the foreground. Nothing to do here, but
// XRSound's config parser passes it when reporting a parse failure.
#define MB_SETFOREGROUND   0x00010000

// Integer limits under their SDK spellings.
#define MAXINT8    0x7f
#define MAXUINT8   0xff
#define MAXINT16   0x7fff
#define MAXUINT16  0xffff
#define MAXINT32   0x7fffffff
#define MAXUINT32  0xffffffff
#define MAXINT64   0x7fffffffffffffffLL
#define MAXUINT64  0xffffffffffffffffULL
#define MININT32   (-0x7fffffff - 1)
#define MAXINT     MAXINT32
#define MAXLONG    MAXINT32
#define MAXSHORT   MAXINT16
#define MAXCHAR    MAXINT8

// GetTickCount64: milliseconds since boot, 64-bit so it does not wrap after
// 49 days as the 32-bit form does. XRSound uses it for its realtime update
// pacing. CLOCK_MONOTONIC is the direct equivalent and is unaffected by clock
// adjustments, which matters for an interval timer.
static inline ULONGLONG GetTickCount64(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ULONGLONG)ts.tv_sec * 1000ULL + (ULONGLONG)(ts.tv_nsec / 1000000);
}
static inline DWORD GetTickCount(void) { return (DWORD)GetTickCount64(); }

// Structured exception handling.
//
// __try/__except is an MSVC extension with no standard counterpart: it catches
// hardware faults such as an access violation, which are signals here, not C++
// exceptions. There is no honest way to resume from a SIGSEGV in a portable
// program, so this maps the construct onto a plain try/catch.
//
// That is a REAL DIFFERENCE and worth stating. XRSoundDLL::clbkPreStep uses
// both of its __try blocks the same way -- guard a call, log an access
// violation, keep going -- so a C++ exception thrown by the guarded code is
// still caught and logged exactly as intended. A genuine segfault inside
// irrKlang would have been swallowed on Windows and will terminate here.
// Given the guards exist for a stale vessel pointer that UpdateAllVesselsMap
// is supposed to have already removed, that case should not arise, and
// crashing loudly on it is arguably better than hiding it.
#ifndef _WIN32
#define __try           try
#define __except(x)     catch (...)
#define __finally
#define EXCEPTION_EXECUTE_HANDLER       1
#define EXCEPTION_CONTINUE_SEARCH       0
#define EXCEPTION_CONTINUE_EXECUTION  (-1)
#endif

// localtime_s: MSVC's argument order is (tm*, time_t*) and it returns errno_t,
// the reverse of POSIX localtime_r which returns the tm*.
static inline int localtime_s(struct tm *result, const time_t *t) {
    if (!result || !t) return 22;              // EINVAL
    return localtime_r(t, result) ? 0 : 22;
}
static inline int gmtime_s(struct tm *result, const time_t *t) {
    if (!result || !t) return 22;
    return gmtime_r(t, result) ? 0 : 22;
}

// _ASSERTE is the debug-CRT assertion. assert() is the direct equivalent and,
// like _ASSERTE, compiles away when NDEBUG is set.
#ifndef _ASSERTE
#ifdef __cplusplus
#include <cassert>
#else
#include <assert.h>
#endif
#define _ASSERTE(expr) assert(expr)
#endif
#ifndef _ASSERT
#define _ASSERT(expr) _ASSERTE(expr)
#endif

// ---------------------------------------------------------------------------
// Graphics client support
//
// Added for OVP/VulkanClient, the Vulkan port of OVP/D3D9Client. The D3D9
// sources call these throughout -- Log.cpp alone uses five of the six -- and
// they are ordinary Win32/CRT with exact POSIX equivalents, so they belong
// here rather than being edited out of ~100 call sites.
//
// Nothing here emulates Direct3D. The D3D9 API surface itself is converted
// file by file into Vulkan; only the platform layer is bridged.
// ---------------------------------------------------------------------------

#include <pthread.h>
#include <signal.h>

// --- Critical sections -----------------------------------------------------
//
// A Win32 critical section is RECURSIVE: the owning thread may re-enter one it
// already holds, and releases it only after a matching number of Leaves. A
// default pthread mutex is not, and re-locking it deadlocks.
//
// That distinction is not academic here. D3D9Client's logging nests -- a
// LogErr inside a section already entered by LogAlw -- and with a default
// mutex the process would hang on the first nested log line rather than fail
// to compile. PTHREAD_MUTEX_RECURSIVE is the only correct choice.

typedef struct _RTL_CRITICAL_SECTION {
    pthread_mutex_t mutex;
    int             initialised;
} CRITICAL_SECTION, *LPCRITICAL_SECTION, *PCRITICAL_SECTION;

static inline void InitializeCriticalSection(LPCRITICAL_SECTION cs) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&cs->mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    cs->initialised = 1;
}

// The spin count is a scheduling hint on Windows -- how long to spin before
// sleeping -- and has no effect on correctness. Linux adaptive mutexes are a
// glibc extension that cannot also be recursive, so the hint is ignored.
static inline BOOL InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION cs, DWORD spin) {
    (void)spin;
    InitializeCriticalSection(cs);
    return TRUE;
}

static inline void EnterCriticalSection(LPCRITICAL_SECTION cs) {
    pthread_mutex_lock(&cs->mutex);
}
static inline void LeaveCriticalSection(LPCRITICAL_SECTION cs) {
    pthread_mutex_unlock(&cs->mutex);
}
static inline BOOL TryEnterCriticalSection(LPCRITICAL_SECTION cs) {
    return pthread_mutex_trylock(&cs->mutex) == 0 ? TRUE : FALSE;
}
static inline void DeleteCriticalSection(LPCRITICAL_SECTION cs) {
    if (cs->initialised) {
        pthread_mutex_destroy(&cs->mutex);
        cs->initialised = 0;
    }
}

// --- High-resolution timing ------------------------------------------------
//
// LARGE_INTEGER is a union in the SDK with QuadPart overlaying the Low/High
// pair. The layout is load-bearing, not cosmetic: Log.cpp declares its
// counters as __int64 and passes their addresses as (LARGE_INTEGER*), so
// QuadPart must sit at offset 0 in an 8-byte object or every timestamp is
// read from the wrong half.

// The SDK declares the halves TWICE: once anonymously, so that `li.LowPart`
// works, and once as a named member `u`, so that `li.u.LowPart` does too.
// Only the named form was here, and the anonymous one is the spelling most
// callers use -- Utils/texpack reads `sz.LowPart` straight from a
// GetFileSizeEx result, which without this does not compile at all.
//
// Both overlay QuadPart at offset 0, so nothing about the layout changes; the
// anonymous struct is a C11 feature and a GCC/Clang extension in C++, which
// is how the real SDK header spells it too.
typedef union _LARGE_INTEGER {
    struct { DWORD LowPart; LONG  HighPart; };
    struct { DWORD LowPart; LONG  HighPart; } u;
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef union _ULARGE_INTEGER {
    struct { DWORD LowPart; DWORD HighPart; };
    struct { DWORD LowPart; DWORD HighPart; } u;
    ULONGLONG QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;

// CLOCK_MONOTONIC, reported as a nanosecond counter with a fixed 1e9 tick
// frequency. It is unaffected by wall-clock adjustments, so a user changing
// the system time mid-flight cannot make a frame appear to take negative time.
//
// The 1e9 frequency is what makes D3D9Client's own arithmetic come out right
// unmodified: D3D9GetTime computes count * 1e6 / frequency, which for a
// nanosecond counter is exactly microseconds, the unit its callers expect.
static inline BOOL QueryPerformanceFrequency(LARGE_INTEGER *freq) {
    if (!freq) return FALSE;
    freq->QuadPart = 1000000000LL;
    return TRUE;
}
static inline BOOL QueryPerformanceCounter(LARGE_INTEGER *count) {
    struct timespec ts;
    if (!count) return FALSE;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    count->QuadPart = (LONGLONG)ts.tv_sec * 1000000000LL + (LONGLONG)ts.tv_nsec;
    return TRUE;
}

// --- Debug break -----------------------------------------------------------
//
// SIGTRAP, not __builtin_trap(). Both stop the process, but SIGTRAP is what a
// debugger installs a handler for and can be continued past, while
// __builtin_trap raises SIGILL and is unrecoverable. That matches Windows,
// where DebugBreak() breaks into an attached debugger and terminates the
// process when there is none.
static inline void DebugBreak(void) { raise(SIGTRAP); }

// --- Remaining secure-CRT forms --------------------------------------------
//
// The rest of the _s family is defined further up this header; these two are
// the forms the client's logging uses.
//
// _vsnprintf_s takes BOTH a buffer size and a character count, unlike
// vsnprintf. MSVC writes at most `count` characters plus a terminator, and
// truncates rather than overflowing when count >= sizeOfBuffer, so the
// effective limit is the smaller of the two.
static inline int _vsnprintf_s(char *buf, size_t bufSize, size_t count,
                               const char *fmt, va_list ap) {
    size_t lim;
    if (!buf || bufSize == 0) return -1;
    lim = (count + 1 < bufSize) ? count + 1 : bufSize;
    return vsnprintf(buf, lim, fmt, ap);
}

static inline int fprintf_s(FILE *f, const char *fmt, ...) {
    va_list ap; int r;
    va_start(ap, fmt);
    r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

// ---------------------------------------------------------------------------
// Files and directory enumeration
//
// Added for Utils/texpack, the planet texture-tree packer. It is the only
// thing in the tree that reads a file through the Win32 handle API rather
// than stdio, and the only thing that walks a directory with a wildcard --
// which is why none of this existed until it was ported.
//
// This block sits at the end of the header rather than beside GetFileAttributes
// where it belongs, because GetFileSizeEx takes a PLARGE_INTEGER and that union
// is not declared until the graphics-client section above.
//
// The implementation is in Handles.cpp, next to CloseHandle, because a file
// handle is closed with CloseHandle and must therefore be the same tagged
// object every other HANDLE in this shim is.
// ---------------------------------------------------------------------------

// CreateFile: desired access, and the creation disposition.
#define GENERIC_READ        0x80000000
#define GENERIC_WRITE       0x40000000
#define FILE_SHARE_READ     0x00000001
#define FILE_SHARE_WRITE    0x00000002
#define FILE_SHARE_DELETE   0x00000004
#define CREATE_NEW          1
#define CREATE_ALWAYS       2
#define OPEN_EXISTING       3
#define OPEN_ALWAYS         4
#define TRUNCATE_EXISTING   5

// The failure value CreateFile and FindFirstFile return. NOT null: Win32 uses
// -1 here and 0 for most other APIs, and callers test for this exact value.
#define INVALID_HANDLE_VALUE ((HANDLE)(LONG_PTR)-1)

// 100-nanosecond intervals since 1601. Nothing in this tree reads the times a
// directory search reports; the type exists because WIN32_FIND_DATA carries
// three of them and the layout is the SDK's.
#ifndef ORB_FILETIME_DEFINED
#define ORB_FILETIME_DEFINED
typedef struct _FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME, *PFILETIME, *LPFILETIME;
#endif

typedef struct _WIN32_FIND_DATAA {
    DWORD    dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD    nFileSizeHigh;
    DWORD    nFileSizeLow;
    DWORD    dwReserved0;
    DWORD    dwReserved1;
    CHAR     cFileName[MAX_PATH];
    CHAR     cAlternateFileName[14];
} WIN32_FIND_DATAA, *PWIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;

typedef WIN32_FIND_DATAA WIN32_FIND_DATA;
typedef LPWIN32_FIND_DATAA LPWIN32_FIND_DATA;

ORB_EXTERN_C_BEGIN

HANDLE CreateFileA    (LPCSTR name, DWORD access, DWORD share, void *sa,
                       DWORD disposition, DWORD flags, HANDLE templateFile);
BOOL   ReadFile       (HANDLE h, LPVOID buf, DWORD toRead, LPDWORD read,
                       void *overlapped);
BOOL   GetFileSizeEx  (HANDLE h, PLARGE_INTEGER size);

// The search argument is a path WITH the wildcard on the end, not a
// directory: "Surf/04/000001/*.dds". See Handles.cpp for the two Win32
// behaviours reproduced there -- case-insensitive matching, and "." and ".."
// being listed.
HANDLE FindFirstFileA (LPCSTR spec, LPWIN32_FIND_DATAA data);
BOOL   FindNextFileA  (HANDLE h, LPWIN32_FIND_DATAA data);
BOOL   FindClose      (HANDLE h);

ORB_EXTERN_C_END

#define CreateFile     CreateFileA
#define FindFirstFile  FindFirstFileA
#define FindNextFile   FindNextFileA

// NOTE: min/max are deliberately NOT defined here.
//
// The real <windows.h> defines them as macros unless NOMINMAX is set, and
// D3D9Client's sources do use them. They are not added because a macro named
// `max` breaks std::max, std::numeric_limits<>::max() and any header that
// spells those -- for the ENTIRE tree, not just the client, since this header
// is included almost everywhere. The client's own uses are converted to
// std::max/std::min at the call site instead, which is a handful of lines
// against a tree-wide hazard.

#endif // ORBITER_LINUX_WINDOWS_H
