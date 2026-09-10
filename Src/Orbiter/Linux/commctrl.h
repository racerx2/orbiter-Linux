// Linux <commctrl.h> — the common-control subset Orbiter uses.
//
// Included by Launchpad.cpp, LpadTab.cpp, TabAbout.cpp, TabExtra.cpp,
// Orbiter.h and the ScnEditor/Meshdebug/TrackIR plugins. Those sources are not
// modified; this header supplies what they reference.
//
// The TreeView_* names are macros in the real SDK, not functions: each one
// expands to a SendMessage with a TVM_* code. They are kept as macros here for
// the same reason, so the message flow through the dialog implementation is
// identical to Windows and there is one code path to implement rather than
// two. Structure layouts and message numbers match the SDK because the
// scenario tree passes TVITEM/TVINSERTSTRUCT by pointer through SendMessage.

#ifndef ORBITER_LINUX_COMMCTRL_H
#define ORBITER_LINUX_COMMCTRL_H

#ifdef _WIN32
#error "This header is for non-Windows builds only."
#endif

#include <windows.h>

// ---------------------------------------------------------------------------
// WM_NOTIFY payloads
// ---------------------------------------------------------------------------

// NMHDR is defined in windows.h under this guard, because htmlhelp.h needs it
// without including commctrl.h. Honouring the same guard here keeps a single
// definition however the two headers get combined.
#ifndef ORB_NMHDR_DEFINED
#define ORB_NMHDR_DEFINED
typedef struct tagNMHDR {
    HWND     hwndFrom;
    UINT_PTR idFrom;
    UINT     code;
} NMHDR, *LPNMHDR;
#endif

// Notification codes are unsigned wraparound values in the SDK; the
// (0U - n) spelling is preserved so the numbers match exactly.
#define NM_FIRST        (0U -  0U)
#define NM_OUTOFMEMORY  (NM_FIRST - 1)
#define NM_CLICK        (NM_FIRST - 2)
#define NM_DBLCLK       (NM_FIRST - 3)
#define NM_RETURN       (NM_FIRST - 4)
#define NM_RCLICK       (NM_FIRST - 5)
#define NM_RDBLCLK      (NM_FIRST - 6)
#define NM_SETFOCUS     (NM_FIRST - 7)
#define NM_KILLFOCUS    (NM_FIRST - 8)
#define NM_CUSTOMDRAW   (NM_FIRST - 12)
#define NM_HOVER        (NM_FIRST - 13)

// ---------------------------------------------------------------------------
// Tab control
//
// Used by the TrackIR plugin's configuration dialog. Like the TreeView_* set
// above, TabCtrl_* are macros over messages in the real SDK, and are kept as
// macros here so the traffic through the dialog implementation is identical.
// ---------------------------------------------------------------------------

#define TCM_FIRST 0x1300

#define TCM_GETITEMCOUNT   (TCM_FIRST + 4)
#define TCM_GETITEMA       (TCM_FIRST + 5)
#define TCM_SETITEMA       (TCM_FIRST + 6)
#define TCM_INSERTITEMA    (TCM_FIRST + 7)
#define TCM_DELETEITEM     (TCM_FIRST + 8)
#define TCM_DELETEALLITEMS (TCM_FIRST + 9)
#define TCM_GETITEMRECT    (TCM_FIRST + 10)
#define TCM_GETCURSEL      (TCM_FIRST + 11)
#define TCM_SETCURSEL      (TCM_FIRST + 12)
#define TCM_ADJUSTRECT     (TCM_FIRST + 40)
#define TCM_SETCURFOCUS    (TCM_FIRST + 48)
#define TCM_GETCURFOCUS    (TCM_FIRST + 47)

#define TCM_GETITEM        TCM_GETITEMA
#define TCM_SETITEM        TCM_SETITEMA
#define TCM_INSERTITEM     TCM_INSERTITEMA

// TCITEM.mask bits
#define TCIF_TEXT          0x0001
#define TCIF_IMAGE         0x0002
#define TCIF_RTLREADING    0x0004
#define TCIF_PARAM         0x0008
#define TCIF_STATE         0x0010

typedef struct tagTCITEMA {
    UINT   mask;
    DWORD  dwState;
    DWORD  dwStateMask;
    LPSTR  pszText;
    int    cchTextMax;
    int    iImage;
    LPARAM lParam;
} TCITEMA, *LPTCITEMA;

typedef TCITEMA TCITEM;
typedef LPTCITEMA LPTCITEM;

// Legacy spellings. The SDK carries both, and TrackIRconfig.cpp uses the
// underscored one -- as the tree view sources use TV_ITEM for TVITEM.
typedef TCITEMA TC_ITEMA;
typedef TCITEMA TC_ITEM;

// Notification codes
#define TCN_FIRST       (0U - 550U)
#define TCN_KEYDOWN     (TCN_FIRST - 0)
#define TCN_SELCHANGE   (TCN_FIRST - 1)
#define TCN_SELCHANGING (TCN_FIRST - 2)

#define TabCtrl_GetItemCount(hwnd) \
    ((int)SendMessageA((hwnd), TCM_GETITEMCOUNT, 0, 0))
#define TabCtrl_GetItem(hwnd, i, pitem) \
    ((BOOL)SendMessageA((hwnd), TCM_GETITEM, (WPARAM)(int)(i), (LPARAM)(TCITEM *)(pitem)))
#define TabCtrl_SetItem(hwnd, i, pitem) \
    ((BOOL)SendMessageA((hwnd), TCM_SETITEM, (WPARAM)(int)(i), (LPARAM)(TCITEM *)(pitem)))
#define TabCtrl_InsertItem(hwnd, i, pitem) \
    ((int)SendMessageA((hwnd), TCM_INSERTITEM, (WPARAM)(int)(i), (LPARAM)(const TCITEM *)(pitem)))
#define TabCtrl_DeleteItem(hwnd, i) \
    ((BOOL)SendMessageA((hwnd), TCM_DELETEITEM, (WPARAM)(int)(i), 0))
#define TabCtrl_DeleteAllItems(hwnd) \
    ((BOOL)SendMessageA((hwnd), TCM_DELETEALLITEMS, 0, 0))
#define TabCtrl_GetItemRect(hwnd, i, prc) \
    ((BOOL)SendMessageA((hwnd), TCM_GETITEMRECT, (WPARAM)(int)(i), (LPARAM)(RECT *)(prc)))
#define TabCtrl_GetCurSel(hwnd) \
    ((int)SendMessageA((hwnd), TCM_GETCURSEL, 0, 0))
#define TabCtrl_SetCurSel(hwnd, i) \
    ((int)SendMessageA((hwnd), TCM_SETCURSEL, (WPARAM)(int)(i), 0))
#define TabCtrl_AdjustRect(hwnd, bLarger, prc) \
    ((void)SendMessageA((hwnd), TCM_ADJUSTRECT, (WPARAM)(BOOL)(bLarger), (LPARAM)(RECT *)(prc)))

#define WC_TABCONTROLA "SysTabControl32"
#define WC_TABCONTROL  WC_TABCONTROLA

// Tab control window styles and notification codes above; the trackbar's own
// notification codes follow.
//
// A trackbar reports through WM_HSCROLL/WM_VSCROLL like a scrollbar, but with
// its own code set in the low word of wParam. The values coincide with the
// SB_* ones by design, and gcTableView switches on the TB_ spellings.
#define TB_LINEUP         0
#define TB_LINEDOWN       1
#define TB_PAGEUP         2
#define TB_PAGEDOWN       3
#define TB_THUMBPOSITION  4
#define TB_THUMBTRACK     5
#define TB_TOP            6
#define TB_BOTTOM         7
#define TB_ENDTRACK       8

// ---------------------------------------------------------------------------
// Tree view
// ---------------------------------------------------------------------------

// Tree view window styles.
//
// These decide what the control draws, and the Launchpad relies on their
// absence as much as their presence: no tree here sets TVS_HASBUTTONS, so
// none shows expander buttons and folders are opened by double-clicking.
// Values are the SDK's.
#define TVS_HASBUTTONS      0x0001
#define TVS_HASLINES        0x0002
#define TVS_LINESATROOT     0x0004
#define TVS_EDITLABELS      0x0008
#define TVS_DISABLEDRAGDROP 0x0010
#define TVS_SHOWSELALWAYS   0x0020
#define TVS_RTLREADING      0x0040
#define TVS_NOTOOLTIPS      0x0080
#define TVS_CHECKBOXES      0x0100
#define TVS_TRACKSELECT     0x0200
#define TVS_SINGLEEXPAND    0x0400
#define TVS_INFOTIP         0x0800
#define TVS_FULLROWSELECT   0x1000
#define TVS_NOSCROLL        0x2000
#define TVS_NONEVENHEIGHT   0x4000
#define TVS_NOHSCROLL       0x8000

#define TV_FIRST 0x1100

#define TVM_INSERTITEMA     (TV_FIRST + 0)
#define TVM_DELETEITEM      (TV_FIRST + 1)
#define TVM_EXPAND          (TV_FIRST + 2)
#define TVM_GETITEMRECT     (TV_FIRST + 4)
#define TVM_GETCOUNT        (TV_FIRST + 5)
#define TVM_GETINDENT       (TV_FIRST + 6)
#define TVM_SETINDENT       (TV_FIRST + 7)
#define TVM_GETIMAGELIST    (TV_FIRST + 8)
#define TVM_SETIMAGELIST    (TV_FIRST + 9)
#define TVM_GETNEXTITEM     (TV_FIRST + 10)
#define TVM_SELECTITEM      (TV_FIRST + 11)
#define TVM_GETITEMA        (TV_FIRST + 12)
#define TVM_SETITEMA        (TV_FIRST + 13)
#define TVM_EDITLABELA      (TV_FIRST + 14)
#define TVM_HITTEST         (TV_FIRST + 17)
#define TVM_ENSUREVISIBLE   (TV_FIRST + 20)
#define TVM_SORTCHILDREN    (TV_FIRST + 19)

#define TVM_INSERTITEM      TVM_INSERTITEMA
#define TVM_GETITEM         TVM_GETITEMA
#define TVM_SETITEM         TVM_SETITEMA
#define TVM_EDITLABEL       TVM_EDITLABELA

// TVITEM.mask bits
#define TVIF_TEXT           0x0001
#define TVIF_IMAGE          0x0002
#define TVIF_PARAM          0x0004
#define TVIF_STATE          0x0008
#define TVIF_HANDLE         0x0010
#define TVIF_SELECTEDIMAGE  0x0020
#define TVIF_CHILDREN       0x0040

// TVITEM.state bits
#define TVIS_SELECTED       0x0002
#define TVIS_EXPANDED       0x0020
#define TVIS_BOLD           0x0010
#define TVIS_STATEIMAGEMASK 0xF000

// TreeView_GetNextItem relationship codes
#define TVGN_ROOT           0x0000
#define TVGN_NEXT           0x0001
#define TVGN_PREVIOUS       0x0002
#define TVGN_PARENT         0x0003
#define TVGN_CHILD          0x0004
#define TVGN_FIRSTVISIBLE   0x0005
#define TVGN_CARET          0x0009

// Expand actions
#define TVE_COLLAPSE        0x0001
#define TVE_EXPAND          0x0002
#define TVE_TOGGLE          0x0003

// Insertion anchors. These are sentinel handle values, not real items.
#define TVI_ROOT            ((HTREEITEM)(ULONG_PTR)-0x10000)
#define TVI_FIRST           ((HTREEITEM)(ULONG_PTR)-0x0FFFF)
#define TVI_LAST            ((HTREEITEM)(ULONG_PTR)-0x0FFFE)
#define TVI_SORT            ((HTREEITEM)(ULONG_PTR)-0x0FFFD)

// Tree view notifications
#define TVN_FIRST           (0U - 400U)
#define TVN_SELCHANGINGA    (TVN_FIRST - 1)
#define TVN_SELCHANGEDA     (TVN_FIRST - 2)
#define TVN_ITEMEXPANDINGA  (TVN_FIRST - 5)
#define TVN_ITEMEXPANDEDA   (TVN_FIRST - 6)
#define TVN_KEYDOWN         (TVN_FIRST - 12)

#define TVN_SELCHANGING     TVN_SELCHANGINGA
#define TVN_SELCHANGED      TVN_SELCHANGEDA
#define TVN_ITEMEXPANDING   TVN_ITEMEXPANDINGA
#define TVN_ITEMEXPANDED    TVN_ITEMEXPANDEDA

typedef struct tagTVITEMA {
    UINT      mask;
    HTREEITEM hItem;
    UINT      state;
    UINT      stateMask;
    LPSTR     pszText;
    int       cchTextMax;
    int       iImage;
    int       iSelectedImage;
    int       cChildren;
    LPARAM    lParam;
} TVITEMA, *LPTVITEMA;

typedef TVITEMA TVITEM,  *LPTVITEM;
typedef TVITEMA TV_ITEM, *LP_TV_ITEM;

// The SDK union carries both item and itemex; it is anonymous so that the
// `.item` spelling used by the scenario tree resolves unchanged.
typedef struct tagTVINSERTSTRUCTA {
    HTREEITEM hParent;
    HTREEITEM hInsertAfter;
    union {
        TVITEMA itemex;
        TVITEMA item;
    };
} TVINSERTSTRUCTA, *LPTVINSERTSTRUCTA;

typedef TVINSERTSTRUCTA TVINSERTSTRUCT,  *LPTVINSERTSTRUCT;
typedef TVINSERTSTRUCTA TV_INSERTSTRUCT, *LP_TV_INSERTSTRUCT;

typedef struct tagNMTREEVIEWA {
    NMHDR   hdr;
    UINT    action;
    TVITEMA itemOld;
    TVITEMA itemNew;
    POINT   ptDrag;
} NMTREEVIEWA, *LPNMTREEVIEWA;

typedef NMTREEVIEWA NMTREEVIEW,  *LPNMTREEVIEW;
typedef NMTREEVIEWA NM_TREEVIEW, *LPNM_TREEVIEW;

// TreeView_* are macros in the SDK. Keeping them as macros means every tree
// operation arrives at the dialog implementation as a TVM_* SendMessage,
// exactly as on Windows.
#define TreeView_InsertItem(hwnd, lpis) \
    ((HTREEITEM)SendMessage((hwnd), TVM_INSERTITEM, 0, (LPARAM)(LPTVINSERTSTRUCT)(lpis)))
#define TreeView_DeleteItem(hwnd, hitem) \
    ((BOOL)SendMessage((hwnd), TVM_DELETEITEM, 0, (LPARAM)(HTREEITEM)(hitem)))
#define TreeView_DeleteAllItems(hwnd) \
    ((BOOL)SendMessage((hwnd), TVM_DELETEITEM, 0, (LPARAM)TVI_ROOT))
#define TreeView_Expand(hwnd, hitem, code) \
    ((BOOL)SendMessage((hwnd), TVM_EXPAND, (WPARAM)(code), (LPARAM)(HTREEITEM)(hitem)))
#define TreeView_GetNextItem(hwnd, hitem, code) \
    ((HTREEITEM)SendMessage((hwnd), TVM_GETNEXTITEM, (WPARAM)(code), (LPARAM)(HTREEITEM)(hitem)))
#define TreeView_GetChild(hwnd, hitem)        TreeView_GetNextItem(hwnd, hitem, TVGN_CHILD)
#define TreeView_GetNextSibling(hwnd, hitem)  TreeView_GetNextItem(hwnd, hitem, TVGN_NEXT)
#define TreeView_GetPrevSibling(hwnd, hitem)  TreeView_GetNextItem(hwnd, hitem, TVGN_PREVIOUS)
#define TreeView_GetParent(hwnd, hitem)       TreeView_GetNextItem(hwnd, hitem, TVGN_PARENT)
#define TreeView_GetRoot(hwnd)                TreeView_GetNextItem(hwnd, NULL, TVGN_ROOT)
#define TreeView_GetSelection(hwnd)           TreeView_GetNextItem(hwnd, NULL, TVGN_CARET)
#define TreeView_Select(hwnd, hitem, code) \
    ((BOOL)SendMessage((hwnd), TVM_SELECTITEM, (WPARAM)(code), (LPARAM)(HTREEITEM)(hitem)))
#define TreeView_SelectItem(hwnd, hitem)      TreeView_Select(hwnd, hitem, TVGN_CARET)
#define TreeView_EnsureVisible(hwnd, hitem) \
    ((BOOL)SendMessage((hwnd), TVM_ENSUREVISIBLE, 0, (LPARAM)(HTREEITEM)(hitem)))
#define TreeView_GetItem(hwnd, pitem) \
    ((BOOL)SendMessage((hwnd), TVM_GETITEM, 0, (LPARAM)(LPTVITEM)(pitem)))
#define TreeView_SetItem(hwnd, pitem) \
    ((BOOL)SendMessage((hwnd), TVM_SETITEM, 0, (LPARAM)(const LPTVITEM)(pitem)))
#define TreeView_GetCount(hwnd) \
    ((UINT)SendMessage((hwnd), TVM_GETCOUNT, 0, 0))
#define TreeView_SortChildren(hwnd, hitem, recurse) \
    ((BOOL)SendMessage((hwnd), TVM_SORTCHILDREN, (WPARAM)(recurse), (LPARAM)(HTREEITEM)(hitem)))

// The parameter names are underscore-prefixed and the local is _ms_TVi, the
// same convention the Windows SDK uses, and for the same reason: a parameter
// named `mask` or `data` would be substituted into `_tvi.mask`, expanding it
// to `_tvi.TVIS_STATEIMAGEMASK`. Macro parameters must not collide with the
// member names the macro body dereferences.
#define TreeView_SetItemState(hwndTV, hti, _data, _mask) \
    do { TVITEM _ms_TVi; \
         _ms_TVi.mask      = TVIF_STATE; \
         _ms_TVi.hItem     = (HTREEITEM)(hti); \
         _ms_TVi.stateMask = (_mask); \
         _ms_TVi.state     = (_data); \
         SendMessage((hwndTV), TVM_SETITEM, 0, (LPARAM)(LPTVITEM)&_ms_TVi); \
    } while (0)

// Check state lives in the top four bits of `state` as a 1-based state-image
// index, so checked is index 2 and unchecked index 1.
#define INDEXTOSTATEIMAGEMASK(i) ((i) << 12)
#define TreeView_SetCheckState(hwnd, hitem, check) \
    TreeView_SetItemState(hwnd, hitem, INDEXTOSTATEIMAGEMASK((check) ? 2 : 1), TVIS_STATEIMAGEMASK)

#define TreeView_GetCheckStateImpl(hwnd, hitem) \
    ((((UINT)(SendMessage((hwnd), TVM_GETITEM, 0, 0))) >> 12) - 1)
#define TreeView_GetCheckState(hwnd, hitem) \
    orb_TreeView_GetCheckState((hwnd), (HTREEITEM)(hitem))

inline UINT orb_TreeView_GetCheckState(HWND hwnd, HTREEITEM hitem) {
    TVITEM tvi;
    tvi.mask      = TVIF_STATE | TVIF_HANDLE;
    tvi.hItem     = hitem;
    tvi.stateMask = TVIS_STATEIMAGEMASK;
    tvi.state     = 0;
    if (!SendMessage(hwnd, TVM_GETITEM, 0, (LPARAM)&tvi)) return (UINT)-1;
    return (tvi.state >> 12) - 1;
}

// ---------------------------------------------------------------------------
// Trackbar (slider)
//
// The options pages use these for continuous settings and read them back
// through WM_HSCROLL, which OptionsPage::DlgProc dispatches to OnHScroll.
// ---------------------------------------------------------------------------

#define TBM_GETPOS        (WM_USER)
#define TBM_GETRANGEMIN   (WM_USER + 1)
#define TBM_GETRANGEMAX   (WM_USER + 2)
#define TBM_SETPOS        (WM_USER + 5)
#define TBM_SETRANGE      (WM_USER + 6)
#define TBM_SETRANGEMIN   (WM_USER + 7)
#define TBM_SETRANGEMAX   (WM_USER + 8)
#define TBM_SETTICFREQ    (WM_USER + 20)
#define TBM_SETPAGESIZE   (WM_USER + 21)
#define TBM_SETLINESIZE   (WM_USER + 23)

#define TRACKBAR_CLASSA   "msctls_trackbar32"
#define TRACKBAR_CLASS    TRACKBAR_CLASSA

// Trackbar window styles, passed at creation.
//
// gcTableView's CreateSlider builds its sliders with
//     WS_CHILD | WS_VISIBLE | TBS_NOTICKS | TBS_TRANSPARENTBKGND | TBS_BOTH
// so all three must exist for it to compile. Values are the SDK's.
//
// TBS_BOTH means "ticks on both sides", which with TBS_NOTICKS set draws
// none -- the combination is contradictory but harmless, and it is what the
// source asks for, so it is reproduced rather than corrected.
#define TBS_AUTOTICKS        0x0001
#define TBS_VERT             0x0002
#define TBS_HORZ             0x0000
#define TBS_TOP              0x0004
#define TBS_BOTTOM           0x0000
#define TBS_LEFT             0x0004
#define TBS_RIGHT            0x0000
#define TBS_BOTH             0x0008
#define TBS_NOTICKS          0x0010
#define TBS_ENABLESELRANGE   0x0020
#define TBS_FIXEDLENGTH      0x0040
#define TBS_NOTHUMB          0x0080
#define TBS_TOOLTIPS         0x0100
#define TBS_REVERSED         0x0200
#define TBS_DOWNISLEFT       0x0400
#define TBS_NOTIFYBEFOREMOVE 0x0800
#define TBS_TRANSPARENTBKGND 0x1000

// ---------------------------------------------------------------------------
// Up-down (spin) control
// ---------------------------------------------------------------------------

typedef struct _NM_UPDOWN {
    NMHDR hdr;
    int   iPos;
    int   iDelta;
} NMUPDOWN, *LPNMUPDOWN;

typedef NMUPDOWN NM_UPDOWN;

#define UDN_FIRST      (0U - 721U)
#define UDN_DELTAPOS   (UDN_FIRST - 1)

#define UDM_SETRANGE   (WM_USER + 101)
#define UDM_GETRANGE   (WM_USER + 102)
#define UDM_SETPOS     (WM_USER + 103)
#define UDM_GETPOS     (WM_USER + 104)
#define UDM_SETRANGE32 (WM_USER + 111)
#define UDM_GETRANGE32 (WM_USER + 112)
#define UDM_SETPOS32   (WM_USER + 113)
#define UDM_GETPOS32   (WM_USER + 114)

// ---------------------------------------------------------------------------
// Image lists
// ---------------------------------------------------------------------------

#define ILC_COLOR    0x0000
#define ILC_COLOR24  0x0018
#define ILC_COLOR32  0x0020
#define ILC_MASK     0x0001

extern "C" {

HIMAGELIST ImageList_Create   (int cx, int cy, UINT flags, int cInitial, int cGrow);
BOOL       ImageList_Destroy  (HIMAGELIST himl);
int        ImageList_Add      (HIMAGELIST himl, HBITMAP hbmImage, HBITMAP hbmMask);
int        ImageList_AddMasked(HIMAGELIST himl, HBITMAP hbmImage, COLORREF crMask);
BOOL       ImageList_Draw     (HIMAGELIST himl, int i, HDC hdc, int x, int y, UINT style);

void       InitCommonControls (void);

} // extern "C"

// The Ex form declares which control classes a module needs before using
// them. Every class this shim provides is always available, so the request is
// accepted; ScnEditor calls it at startup and checks the result.
typedef struct tagINITCOMMONCONTROLSEX {
    DWORD dwSize;
    DWORD dwICC;
} INITCOMMONCONTROLSEX, *LPINITCOMMONCONTROLSEX;

#define ICC_LISTVIEW_CLASSES   0x00000001
#define ICC_TREEVIEW_CLASSES   0x00000002
#define ICC_BAR_CLASSES        0x00000004
#define ICC_TAB_CLASSES        0x00000008
#define ICC_UPDOWN_CLASS       0x00000010
#define ICC_PROGRESS_CLASS     0x00000020
#define ICC_HOTKEY_CLASS       0x00000040
#define ICC_ANIMATE_CLASS      0x00000080
#define ICC_WIN95_CLASSES      0x000000FF
#define ICC_DATE_CLASSES       0x00000100
#define ICC_USEREX_CLASSES     0x00000200
#define ICC_STANDARD_CLASSES   0x00004000

static inline BOOL InitCommonControlsEx(const INITCOMMONCONTROLSEX *) { return TRUE; }

// Common control window classes
#define WC_TREEVIEWA   "SysTreeView32"
#define WC_TREEVIEW    WC_TREEVIEWA
#define UPDOWN_CLASSA  "msctls_updown32"
#define UPDOWN_CLASS   UPDOWN_CLASSA

// Tree view window styles, passed at creation.
#define TVS_HASBUTTONS       0x0001
#define TVS_HASLINES         0x0002
#define TVS_LINESATROOT      0x0004
#define TVS_EDITLABELS       0x0008
#define TVS_DISABLEDRAGDROP  0x0010
#define TVS_SHOWSELALWAYS    0x0020
#define TVS_CHECKBOXES       0x0100
#define TVS_TRACKSELECT      0x0200
#define TVS_FULLROWSELECT    0x1000
#define TVS_NOTOOLTIPS       0x0080

// Image list types for TVM_SETIMAGELIST
#define TVSIL_NORMAL   0
#define TVSIL_STATE    2

#define TreeView_SetImageList(hwnd, himl, which) \
    ((HIMAGELIST)SendMessage((hwnd), TVM_SETIMAGELIST, (WPARAM)(which), (LPARAM)(HIMAGELIST)(himl)))

#define ILC_COLOR8   0x0008
#define ILC_COLOR16  0x0010


// ---------------------------------------------------------------------------
// Tooltips
// ---------------------------------------------------------------------------
//
// Referenced by OVP/VulkanClient/AtmoControls.cpp, which gives every slider a
// tooltip. DECLARATIONS ONLY -- there is no tooltip control in this shim, and
// the client needs none for its code to be correct:
//
//   * CreateWindowEx on an unregistered class still returns a real window
//     (Win32Dlg.cpp's CreateWindowExA always creates one), so the client's
//     `if (!s.hWnd || !s.hwndTip) return;` guard does NOT fire -- the TTM_*
//     sends below really are issued.
//   * They are harmless. TTM_ACTIVATE is WM_USER+1 and TTM_ADDTOOLA is
//     WM_USER+4, which is the collision THE WM_USER COLLISION in Win32Dlg.cpp
//     is about -- and that fix already covers this: WM_USER+0..+8 goes to the
//     window's OWN procedure for every class except the trackbar and the
//     progress bar. A tooltip window is neither, has no registered class and
//     therefore no procedure, so the message is swallowed rather than being
//     read as TBM_GETRANGEMIN.
//   * The window is created WS_POPUP with no WS_VISIBLE, so `visible` is
//     false and nothing draws a stray box in the dialog.
//
// The consequence is exact and worth stating: tooltip TEXT is stored (the
// client keeps it in sValue::tooltip either way) and tooltip DISPLAY does not
// happen yet. Showing it is this shim's job, not the client's -- the renderer
// already has the string it would need.

#define TOOLTIPS_CLASSA   "tooltips_class32"
#define TOOLTIPS_CLASS    TOOLTIPS_CLASSA

#define TTS_ALWAYSTIP     0x01
#define TTS_NOPREFIX      0x02
#define TTS_BALLOON       0x40

#define TTF_IDISHWND      0x0001
#define TTF_CENTERTIP     0x0002
#define TTF_RTLREADING    0x0004
#define TTF_SUBCLASS      0x0010
#define TTF_TRACK         0x0020
#define TTF_ABSOLUTE      0x0080
#define TTF_TRANSPARENT   0x0100

#define TTM_ACTIVATE        (WM_USER + 1)
#define TTM_SETDELAYTIME    (WM_USER + 3)
#define TTM_ADDTOOLA        (WM_USER + 4)
#define TTM_DELTOOLA        (WM_USER + 5)
#define TTM_RELAYEVENT      (WM_USER + 7)
#define TTM_GETTOOLINFOA    (WM_USER + 8)
#define TTM_SETTOOLINFOA    (WM_USER + 9)
#define TTM_UPDATETIPTEXTA  (WM_USER + 12)

#define TTM_ADDTOOL         TTM_ADDTOOLA
#define TTM_DELTOOL         TTM_DELTOOLA
#define TTM_GETTOOLINFO     TTM_GETTOOLINFOA
#define TTM_SETTOOLINFO     TTM_SETTOOLINFOA
#define TTM_UPDATETIPTEXT   TTM_UPDATETIPTEXTA

// Field order and types match the SDK's TTTOOLINFOA, because the client fills
// one in and passes it by pointer through SendMessage.
typedef struct tagTOOLINFOA {
    UINT      cbSize;
    UINT      uFlags;
    HWND      hwnd;
    UINT_PTR  uId;
    RECT      rect;
    HINSTANCE hinst;
    LPSTR     lpszText;
    LPARAM    lParam;
    void     *lpReserved;
} TTTOOLINFOA, *LPTTTOOLINFOA, TOOLINFOA, *LPTOOLINFOA;

typedef TTTOOLINFOA TOOLINFO;
typedef LPTTTOOLINFOA LPTOOLINFO;

#endif // ORBITER_LINUX_COMMCTRL_H
