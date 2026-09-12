// Linux <richedit.h> — the rich edit declarations a .rc may name.
//
// TerrainToolKit's ToolKit.rc includes it, and rcstrings.py preprocesses a .rc
// with `cpp -P -DRC_INVOKED` before parsing it. A missing include aborts the
// preprocessor, and the parser then reports "no STRINGTABLE" rather than an
// error -- so the module's IDS_INFO/IDS_TYPE strings are silently dropped and
// it falls back to ModuleTab's default category of "Miscellaneous" instead of
// the name Windows shows.
//
// Nothing in this tree instantiates a rich edit control; the include is
// boilerplate left behind by ResEdit. Only the names and style bits a
// resource script could plausibly reference are provided.

#ifndef ORBITER_LINUX_RICHEDIT_H
#define ORBITER_LINUX_RICHEDIT_H

#ifdef _WIN32
#error "This header is for non-Windows builds only."
#endif

// Window class names.
#define RICHEDIT_CLASSA    "RICHEDIT"
#define RICHEDIT_CLASS10A  "RICHEDIT"
#define MSFTEDIT_CLASSA    "RICHEDIT50W"
#define RICHEDIT_CLASS     RICHEDIT_CLASSA
#define MSFTEDIT_CLASS     MSFTEDIT_CLASSA

// Rich-edit-specific window styles; the plain ES_* set lives in windows.h.
#define ES_SAVESEL          0x00008000
#define ES_SUNKEN           0x00004000
#define ES_DISABLENOSCROLL  0x00002000
#define ES_SELECTIONBAR     0x01000000
#define ES_NOOLEDRAGDROP    0x00000008
#define ES_VERTICAL         0x00400000
#define ES_NOIME            0x00080000
#define ES_SELFIME          0x00040000

// Event mask bits, set with EM_SETEVENTMASK.
#define ENM_NONE            0x00000000
#define ENM_CHANGE          0x00000001
#define ENM_UPDATE          0x00000002
#define ENM_SCROLL          0x00000004
#define ENM_KEYEVENTS       0x00010000
#define ENM_MOUSEEVENTS     0x00020000
#define ENM_REQUESTRESIZE   0x00040000
#define ENM_SELCHANGE       0x00080000
#define ENM_LINK            0x04000000

// SETTEXTEX and EM_SETTEXTEX, named by OVP/VulkanClient's credits dialog,
// which reads Credits.rtf and hands the bytes to a rich edit control in one
// call. The names are here; the control is not. Win32Dlg.cpp implements no
// RICHEDIT class, so EM_SETTEXTEX goes unanswered and the credits box stays
// empty until it grows one. Substituting WM_SETTEXT would put raw RTF markup
// on screen instead.
#define ST_DEFAULT          0x00000000
#define ST_KEEPUNDO         0x00000001
#define ST_SELECTION        0x00000002
#define ST_NEWCHARS         0x00000004
#define ST_UNICODE          0x00000008

typedef struct _settextex {
    DWORD flags;     ///< ST_* above
    UINT  codepage;  ///< 1200 for Unicode, or a code page such as CP_ACP
} SETTEXTEX;

// Messages, offset from WM_USER.
#define EM_SETTEXTEX        (WM_USER + 97)
#define EM_GETLIMITTEXT     (WM_USER + 37)
#define EM_SETBKGNDCOLOR    (WM_USER + 67)
#define EM_SETCHARFORMAT    (WM_USER + 68)
#define EM_SETEVENTMASK     (WM_USER + 69)
#define EM_SETPARAFORMAT    (WM_USER + 71)
#define EM_STREAMIN         (WM_USER + 73)
#define EM_STREAMOUT        (WM_USER + 74)
#define EM_AUTOURLDETECT    (WM_USER + 91)

// Notification codes.
#define EN_MSGFILTER        0x0700
#define EN_REQUESTRESIZE    0x0701
#define EN_SELCHANGE        0x0702
#define EN_LINK             0x070B

#endif // ORBITER_LINUX_RICHEDIT_H
