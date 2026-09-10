// Window and dialog implementation for Linux.
//
// This is the half of the shim backed by ImGui; Platform.cpp is the half
// backed by POSIX. Together they let Launchpad.cpp, LpadTab.cpp, Tab*.cpp,
// OptionsPages.cpp, DlgCtrl and the ScnEditor compile and run without a single
// line of those sources being edited.
//
// HOW THIS WORKS
//   Win32 dialogs are a message-driven model: the application supplies a
//   DlgProc, the system creates controls from a template, and everything the
//   application does to a control it does by sending it a message. That model
//   is kept intact rather than replaced. A Window here is a plain C++ object
//   holding the state a Win32 control would hold -- text, enabled, visible,
//   selection, scroll range -- and SendMessage mutates that state and notifies
//   the parent's DlgProc exactly as Windows does.
//
//   ImGui then draws that state once per frame and feeds interaction back in
//   as the corresponding WM_COMMAND / WM_NOTIFY. So Orbiter's dialog code sees
//   the message traffic it was written against, and the fact that the pixels
//   come from ImGui rather than USER32 never reaches it.
//
// WHY NOT DRAW DIRECTLY FROM THE DLGPROC
//   Because Win32 is retained-mode and ImGui is immediate-mode. The dialog
//   procedures run when a message arrives, not once per frame, so their state
//   has to live somewhere between frames. That somewhere is the Window object.
//
// COORDINATES
//   Templates store dialog units. One horizontal unit is 1/4 of the average
//   character width and one vertical unit is 1/8 of the character height, so
//   the conversion depends on the dialog font and is done at layout time.

#include <windows.h>
#include <commctrl.h>
#include "ResourceTemplates.h"

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <algorithm>
#include <filesystem>   // the module stem in moduleNameOf, for dialog scoping
#include <dlfcn.h>
#include <strings.h>    // strcasecmp, for the case-insensitive class compare
#include <psapi.h>      // the chain MemStat::HeapUsage resolves; see the
                        // progress-bar self-test

// Implemented in UIHost.cpp. Called from the message loop below; see the
// comment at PeekMessageA for why the frame is pumped from there.
extern "C" void orbiter_PumpFrame(void);
// Nonzero while a graphics client is driving frames; the message loop must not
// pump then. See the call site in PeekMessageA and the note in UIHost.cpp.
extern "C" int  orbiter_SessionOwnsFrames(void);
// Modal message box, drawn by the UI host. MessageBoxA below spins the loop
// until a button is pressed, which is what makes it modal -- except when it
// cannot, in which case it raises the box deferred and returns; see the note
// there.
extern "C" void orbiter_ShowMessageBox(const char *text, const char *caption,
                                       unsigned type, int deferred);
extern "C" int  orbiter_MessageBoxResult(void);
extern "C" void orbiter_ClearMessageBox(void);
// Nonzero while orbiter_PumpFrame is building a frame on this thread, so a
// shim entry point reached from a dialog's draw can tell that it is inside
// the pump rather than above it.
extern "C" int  orbiter_InFramePump(void);
// Defined below with the other UI-host accessors; SendMessageA calls it when
// the tree-view selection changes.
extern "C" void orbiter_NotifyTree(HWND ctrl, UINT code, void *newItem,
                                   void *oldItem);
extern "C" HDC  orbiter_GetPaintDC(HWND h);
// Gdi.cpp: discard a control's recorded drawing. InvalidateRect's bErase.
extern "C" void orbiter_ClearPaintDC(HWND h);
// The host window's caption. SetWindowTextA calls it for the render window,
// which on Windows is a window of its own with its own title bar and here is
// the one GLFW window shared with the Launchpad.
extern "C" void orbiter_SetHostWindowTitle(const char *title);
// Implemented in Platform.cpp: the session console.
extern "C" void orbiter_ShowConsoleWindow(int show);

// Declared rather than pulled in from OrbiterAPI.h: that header wants the SDK
// include chain, and this file is a Win32 shim compiled ahead of it. The
// signature takes a non-const char* upstream and is matched exactly.
void oapiWriteLog(char *line);

// Implemented in Gdi.cpp. A WM_CTLCOLOR* handler reports its choices by
// mutating the DC rather than through a return struct, so the DC has to be
// read back after the message is sent.
extern "C" {
void     orbiter_SetDCTextColor(HDC hdc, unsigned c);
void     orbiter_SetDCBkColor  (HDC hdc, unsigned c);
void     orbiter_SetDCBkMode   (HDC hdc, int mode);
unsigned orbiter_GetDCTextColor(HDC hdc);
unsigned orbiter_GetDCBkColor  (HDC hdc);
int      orbiter_GetDCBkMode   (HDC hdc);
}

namespace {

// Message tracing, enabled with ORBITER_TRACE_MSG=1.
//
// The dialog layer is message-driven and mostly invisible from outside, so
// when a click or key appears to do nothing there is no way to tell which link
// in the chain broke without watching the messages go past.
const bool g_traceMsg = getenv("ORBITER_TRACE_MSG") != nullptr;

#define TRACEMSG(...) do { if (g_traceMsg) { \
    fprintf(stderr, "[msg] " __VA_ARGS__); fputc('\n', stderr); } } while (0)

// ---------------------------------------------------------------------------
// Tree view
//
// The scenario list is a SysTreeView32, and TabScenario.cpp drives it entirely
// through messages: TVM_INSERTITEM to build it, TVM_GETNEXTITEM to walk it,
// TVM_GETITEM to read an item's text, TVM_SELECTITEM to move the caret. It
// then reads the selection back by walking parents to compose a path.
//
// So the item tree has to be real -- ordering, parent links and all -- rather
// than a flat list with labels. ScanDirectory inserts with TVI_SORT for
// folders and computes an explicit predecessor for files, and RefreshList
// re-selects the previous scenario by matching item text level by level.
// ---------------------------------------------------------------------------

struct TreeItem {
    std::string text;
    int         image = 0;
    int         selectedImage = 0;
    int         cChildren = 0;
    LPARAM      lParam = 0;
    UINT        state = 0;
    bool        expanded = false;

    TreeItem              *parent = nullptr;
    std::vector<TreeItem *> children;
};

struct TreeData {
    // Owns every item; the vectors above are non-owning links into this.
    std::vector<std::unique_ptr<TreeItem>> storage;
    std::vector<TreeItem *> roots;
    TreeItem   *selected = nullptr;
    HIMAGELIST  images = nullptr;

    TreeItem *create() {
        storage.push_back(std::make_unique<TreeItem>());
        return storage.back().get();
    }

    void clear() {
        storage.clear();
        roots.clear();
        selected = nullptr;
    }

    // Depth-first successor, which is what TVGN_NEXT means for a tree view
    // being walked linearly.
    static TreeItem *nextSibling(TreeItem *it) {
        if (!it) return nullptr;
        return siblingAt(it, +1);
    }
    static TreeItem *prevSibling(TreeItem *it) {
        if (!it) return nullptr;
        return siblingAt(it, -1);
    }

    static std::vector<TreeItem *> *siblingList(TreeItem *it, TreeData *td) {
        return it->parent ? &it->parent->children : &td->roots;
    }

private:
    static TreeItem *siblingAt(TreeItem *it, int delta);
};

// Sibling lookup needs the owning list, which for a root item lives on the
// TreeData rather than on a parent. The tree pointer is threaded through by
// the message handler rather than stored on every item.
TreeData *g_activeTree = nullptr;

TreeItem *TreeData::siblingAt(TreeItem *it, int delta)
{
    std::vector<TreeItem *> *list =
        it->parent ? &it->parent->children
                   : (g_activeTree ? &g_activeTree->roots : nullptr);
    if (!list) return nullptr;
    for (size_t i = 0; i < list->size(); ++i) {
        if ((*list)[i] != it) continue;
        const long j = (long)i + delta;
        if (j < 0 || j >= (long)list->size()) return nullptr;
        return (*list)[j];
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Window objects
// ---------------------------------------------------------------------------

struct Window;

// Every live window, keyed by the HWND handed out to Orbiter. Handles are the
// object address, so lookup is a cast plus a validity check against this set.
std::map<Window *, std::unique_ptr<Window>> g_windows;
// Top-level dialogs in creation order.
//
// g_windows is a std::map keyed by pointer, so iterating it yields address
// order, which is arbitrary. Anything that needs "the active dialog" -- above
// all, where to deliver keyboard input -- must not depend on that: the answer
// is the most recently created visible one, which is the modal when a modal is
// up and the Launchpad otherwise.
std::vector<Window *> g_topLevelOrder;

struct Window {
    // Identity
    int          id      = 0;         // control id within its parent
    std::string  className;           // "Button", "Static", "SysTreeView32"...
    Window      *parent  = nullptr;   // layout parent; null for a top-level window
    Window      *owner   = nullptr;   // owning window, for a popup dialog
    std::vector<Window *> children;

    // Template-derived geometry, in dialog units.
    int x = 0, y = 0, cx = 0, cy = 0;
    DWORD style   = 0;
    DWORD exStyle = 0;

    // State a Win32 control carries between messages.
    std::string text;
    bool        enabled = true;
    bool        visible = true;

    // Selection state for list-like controls. -1 is Win32's CB_ERR/LB_ERR.
    std::vector<std::string> items;
    int                      curSel = -1;

    // Multi-selection, for LBS_MULTIPLESEL / LBS_EXTENDEDSEL list boxes.
    // The Planetarium marker list and the Labels feature list are these:
    // they are populated with LB_SETSEL and read back item by item with
    // LB_GETSEL, which a single curSel cannot represent.
    std::vector<char>        itemSel;

    // The application-defined value CB_SETITEMDATA / LB_SETITEMDATA attaches
    // to an item. It is how a caller maps a visible string back to the record
    // it came from: the D3D9 VideoTab packs a mode's width and height into it
    // and reads them back on selection, rather than parsing the label.
    std::vector<LONG_PTR>    itemData;

    // Button check state (BST_CHECKED / BST_UNCHECKED).
    int checkState = BST_UNCHECKED;

    // Scrollbar state, one entry per bar.
    struct ScrollState { int minPos = 0, maxPos = 0, pos = 0, page = 0; };
    ScrollState scrollH, scrollV;

    // Trackbar tick spacing, TBM_SETTICFREQ. One is the Win32 default: a
    // TBS_AUTOTICKS bar with no explicit frequency ticks every unit.
    int ticFreq = 1;

    // Dialogs only.
    DLGPROC dlgProc  = nullptr;
    WNDPROC wndProc  = nullptr;
    LPARAM  initParam = 0;
    bool    isDialog = false;
    bool    modal    = false;
    INT_PTR modalResult = 0;
    bool    endDialogCalled = false;

    // GWLP_USERDATA and the other window words the tree stores pointers in.
    LONG_PTR userData = 0;
    LONG_PTR dwlUser  = 0;
    LRESULT  msgResult = 0;

    // Per-window extra bytes, sized by the class's cbWndExtra.
    //
    // A window class may request extra storage, addressed by *byte offset*
    // through GetWindowLongPtr/SetWindowLongPtr. DlgCtrl relies on this
    // heavily: a gauge asks for 32 bytes and keeps its position, range minimum,
    // range maximum and flags at offsets 0, 8, 16 and 24. Folding those into a
    // single slot would make all four alias, so every gauge would paint from
    // whichever value was written last.
    std::vector<LONG_PTR> extra;

    // Bitmap id for SS_BITMAP controls, carried through from the template.
    int bitmapId = 0;

    // TVS_CHECKBOXES as the TEMPLATE declared it.
    //
    // A tree created with that style gets a state image list, and every item
    // inserted afterwards starts with state image 1 -- an unchecked box. That
    // default is what ModuleTab depends on: InitActivation ticks the active
    // modules to image 2 and then *removes* the box from the category rows
    // with SetItemState(0, TVIS_STATEIMAGEMASK), which only makes sense if
    // the rows had one to begin with.
    //
    // The live style cannot be consulted, because ModuleTab::OnInitDialog
    // rewrites it without TVS_CHECKBOXES before a single item is inserted:
    //     SetWindowLongPtr(hTree, GWL_STYLE, TVS_DISABLEDRAGDROP |
    //         TVS_SHOWSELALWAYS | TVS_NOTOOLTIPS | WS_BORDER | WS_TABSTOP);
    // On Windows the image list already exists by then and survives; here the
    // equivalent is to remember what the template asked for.
    bool treeCheckboxes = false;

    // Tree view contents, allocated on first use so an ordinary control does
    // not carry the cost.
    std::unique_ptr<TreeData> tree;

    TreeData *treeData() {
        if (!tree) tree = std::make_unique<TreeData>();
        return tree.get();
    }
};

inline Window *toWindow(HWND h)
{
    Window *w = (Window *)h;
    if (!w) return nullptr;
    return g_windows.count(w) ? w : nullptr;
}

inline HWND toHwnd(Window *w) { return (HWND)w; }

Window *createWindow()
{
    auto owned = std::make_unique<Window>();
    Window *raw = owned.get();
    g_windows.emplace(raw, std::move(owned));
    return raw;
}

// Focus is tracked but not acted on for drawing: ImGui owns keyboard routing.
// It matters for tab navigation, which needs to know where the cycle is, and
// for destroyWindowRecursive just below, which has to clear it -- which is why
// it is defined up here rather than beside SetFocus/GetFocus.
Window *g_focus = nullptr;

void destroyWindowRecursive(Window *w)
{
    if (!w) return;

    // FOCUS MUST NOT OUTLIVE THE WINDOW IT NAMES.
    //
    // g_focus is a raw Window*, and closing a modal destroys the dialog and
    // every control in it. Leaving the pointer behind leaves it dangling into
    // freed memory. Nothing crashed today only because every reader happens to
    // launder it through toWindow(), which validates against g_windows and
    // returns null for a dead pointer -- so the symptom was cosmetic and
    // diagnostic rather than fatal:
    //
    //     UIDRV after_escape: dialog=171 focus=0 modal=0
    //
    // focus=0 there is GetDlgCtrlID failing the validity check on a destroyed
    // control, not a control with id 0. Windows returns focus to the owner
    // when a modal closes, which is what this does.
    //
    // THE TEST IS "IS FOCUS ANYWHERE IN THIS SUBTREE", not "is focus this
    // window". Focus sits on a CONTROL, and it is the control's dialog that
    // gets destroyed -- so a plain `g_focus == w` never fires for the case
    // that matters. It also cannot be left to the recursion to catch on the
    // way down: each child would hand focus to its parent, which this call is
    // about to destroy, putting the pointer right back where it started.
    // Measured: with the equality test the driver still reported focus=0.
    if (g_focus) {
        for (Window *a = g_focus; a; a = a->parent) {
            if (a != w) continue;
            Window *heir = w->owner ? w->owner : w->parent;
            g_focus = (heir && g_windows.count(heir)) ? heir : nullptr;
            break;
        }
    }

    // Copy the child list: destroying a child unlinks it from this vector.
    std::vector<Window *> kids = w->children;
    for (Window *c : kids) destroyWindowRecursive(c);

    if (w->parent) {
        auto &sib = w->parent->children;
        sib.erase(std::remove(sib.begin(), sib.end(), w), sib.end());
    }
    g_topLevelOrder.erase(
        std::remove(g_topLevelOrder.begin(), g_topLevelOrder.end(), w),
        g_topLevelOrder.end());
    g_windows.erase(w);
}

// Registered window classes. Orbiter registers its own for the custom
// controls (OrbiterCtrl_Gauge and friends) and for the render window.
//
// cbWndExtra is recorded because a window created from the class gets that
// many bytes of private storage, addressed by offset through
// Get/SetWindowLongPtr. DlgCtrl's controls keep all their state there.
struct WindowClass {
    WNDPROC proc  = nullptr;
    int     extra = 0;      // cbWndExtra, in bytes
};
// Deliberately never destroyed.
//
// Modules unregister their window classes from ExitModule, which the loader
// runs as the process tears down -- ExtMFD does exactly that with
// "ExtMFD_Display". The order in which a shared object's destructors run
// relative to this executable's namespace-scope objects is not defined, so
// with an ordinary global the erase could land on a map that had already been
// destroyed:
//
//     double free or corruption (!prev)
//     ... std::map<..., WindowClass>::erase (__x="ExtMFD_Display")
//     ... UnregisterClassA
//
// which aborted every session on exit, after a clean run.
//
// Allocating it once and never freeing it removes the ordering hazard
// entirely: the map stays valid for as long as any code can reach it. The
// "leak" is reclaimed by the kernel at process exit like everything else, and
// is the standard remedy for a global that outlives its own static lifetime.
std::map<std::string, WindowClass> &windowClasses()
{
    static std::map<std::string, WindowClass> *classes =
        new std::map<std::string, WindowClass>();
    return *classes;
}
#define g_windowClasses windowClasses()

// Timers, keyed by (window, id) as Win32 does.
struct Timer {
    Window   *owner;
    UINT_PTR  id;
    UINT      intervalMs;
    TIMERPROC proc;
    DWORD     nextDue;
};
std::vector<Timer> g_timers;

// The message queue. GetMessage/PeekMessage drain this; the GLFW event pump
// fills it. Keeping a real queue rather than dispatching inline preserves the
// ordering guarantees Orbiter's message loop assumes.
// A queued message, optionally owning the struct its lParam points at.
//
// WM_NOTIFY carries a pointer to an NM_* struct. Sending it synchronously is
// fine because the caller's stack copy outlives the call, but posting it is
// not -- so the payload is copied here and kept alive until the message has
// been dispatched.
struct QueuedMsg {
    MSG               msg{};
    std::vector<char> payload;   // empty when lParam is not a pointer
};

std::vector<QueuedMsg> g_messageQueue;

// Keeps the most recently returned message's payload alive across the
// PeekMessage -> DispatchMessage pair, which is when the receiver reads it.
std::vector<char> g_currentPayload;

bool g_quitPosted = false;
int  g_quitCode   = 0;

// Modal dialog nesting. DialogBoxParam runs a nested loop, so the stack
// records which dialog each level is waiting on.
std::vector<Window *> g_modalStack;


// Dispatches a message to whichever procedure owns the window, matching the
// Win32 rule that a dialog uses its DLGPROC and everything else its WNDPROC.
LRESULT dispatchToProc(Window *w, UINT msg, WPARAM wp, LPARAM lp)
{
    if (!w) return 0;

    if (w->isDialog && w->dlgProc) {
        const INT_PTR ret = w->dlgProc(toHwnd(w), msg, wp, lp);

        // A DLGPROC normally returns TRUE/FALSE for "handled" and leaves the
        // real result in DWLP_MSGRESULT. A handful of messages are exceptions
        // where the returned value IS the result, and WM_CTLCOLOR* is the one
        // that matters here: Launchpad.cpp returns
        // (INT_PTR)GetStockObject(BLACK_BRUSH) from WM_CTLCOLORSTATIC to make
        // IDC_BLACKBOX a black panel. Treating that as a mere "handled" flag
        // and returning msgResult instead discards the brush, and the panel
        // silently renders with default colours.
        switch (msg) {
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORLISTBOX:
            return (LRESULT)ret;
        default:
            break;
        }
        return ret ? w->msgResult : 0;
    }
    if (w->wndProc)
        return w->wndProc(toHwnd(w), msg, wp, lp);

    // No procedure: fall through to the default handling.
    return DefWindowProcA(toHwnd(w), msg, wp, lp);
}

// A combo box's edit field follows its selection, which is what makes
// GetWindowText on a combo return the selected item. See the long note at the
// combo-box message cases for why this exists and what its absence cost.
//
// LIST BOXES DELIBERATELY DO NOT DO THIS. A list box has no edit field, and
// its window text is its (unused) caption, unrelated to LB_SETCURSEL --
// syncing it there would invent behaviour Windows does not have. Hence the
// class test rather than one rule for both.
void comboSyncText(Window *w)
{
    if (!w || w->className != "ComboBox") return;
    if (w->curSel >= 0 && (size_t)w->curSel < w->items.size())
        w->text = w->items[w->curSel];
    else
        w->text.clear();
}

// Sends WM_COMMAND to a control's parent, the way a control notifies of user
// interaction. notifyCode goes in the high word of wParam, the control id in
// the low word, and the control handle in lParam.
void notifyParent(Window *ctrl, WORD notifyCode)
{
    if (!ctrl || !ctrl->parent) return;

    // POSTED, not sent. A control notification must reach the dialog
    // procedure from the message loop, not from inside the paint pass -- which
    // is where the UI host detects the click.
    //
    // Sending it directly runs the handler mid-frame, and a handler is allowed
    // to open a modal: ExtraTab's Open button calls clbkOpen ->
    // DialogBoxParam, whose nested message loop pumps another frame and calls
    // ImGui::NewFrame while the outer frame is still open. ImGui asserts on
    // exactly that. Windows has the same separation for the same reason: a
    // click queues WM_COMMAND, it is not delivered during WM_PAINT.
    TRACEMSG("notifyParent: post WM_COMMAND id=%d code=%u", ctrl->id,
             (unsigned)notifyCode);
    PostMessageA(toHwnd(ctrl->parent), WM_COMMAND,
                 MAKEWPARAM(ctrl->id, notifyCode), (LPARAM)toHwnd(ctrl));
}

// ---------------------------------------------------------------------------
// Dialog-unit conversion
//
// A dialog unit is defined against the dialog's font: 4 horizontal units per
// average character width, 8 vertical units per character height. The stock
// templates use 8pt MS Shell Dlg, whose base unit pair is (6, 13) at the
// default DPI -- the values Windows itself computes for that font. Using
// those keeps the converted layouts proportioned as designed.
// ---------------------------------------------------------------------------

constexpr int kBaseUnitX = 6;
constexpr int kBaseUnitY = 13;

inline int duToPxX(int du) { return (du * kBaseUnitX) / 4; }
inline int duToPxY(int du) { return (du * kBaseUnitY) / 8; }

// ---------------------------------------------------------------------------
// Template instantiation
// ---------------------------------------------------------------------------

// Resolves the id a template was requested by. MAKEINTRESOURCE packs an
// integer into a pointer below 0x10000, which is how Orbiter passes IDD_*
// values; anything above that is a genuine string name.
bool resourceId(LPCSTR name, int *out)
{
    if (!name) return false;
    if (IS_INTRESOURCE(name)) { *out = (int)(uintptr_t)name; return true; }
    // Named resources are not used by this tree's dialogs.
    return false;
}

Window *instantiateTemplate(const orbiter_res::DialogTemplate *tmpl,
                            Window *parent, DLGPROC proc, LPARAM param)
{
    Window *dlg = createWindow();
    dlg->isDialog  = true;
    dlg->dlgProc   = proc;
    dlg->initParam = param;
    dlg->id        = tmpl->id;
    dlg->className = "#32770";           // the Win32 dialog class name
    dlg->text      = tmpl->caption ? tmpl->caption : "";
    // Dialog units become pixels here and stay pixels. See the note above
    // duToPxX: everything downstream -- GetClientRect, Resize, SetWindowPos --
    // works in pixels, as it does on Windows.
    dlg->x  = duToPxX(tmpl->x);  dlg->y  = duToPxY(tmpl->y);
    dlg->cx = duToPxX(tmpl->cx); dlg->cy = duToPxY(tmpl->cy);
    dlg->style   = tmpl->style;
    dlg->exStyle = tmpl->exStyle;
    dlg->visible = (tmpl->style & WS_VISIBLE) != 0;

    // WS_CHILD decides what the hWndParent argument means, and the two cases
    // are completely different windows.
    //
    // A tab page template carries WS_CHILD: it is a child window, positioned
    // and clipped inside its parent -- LpadTab.cpp parents every page to
    // IDC_MNU_PAGECONTAINER this way.
    //
    // Every modal template carries WS_POPUP instead. There the argument is the
    // *owner*, not the parent: the dialog is still a top-level window with its
    // own position on screen. LaunchpadItem::OpenDialog passes the Launchpad
    // as owner exactly so the modal stays on top of it.
    //
    // Treating a popup as a child nests it inside the owner's coordinate
    // space, where it is drawn at the wrong place and clipped by the owner --
    // which is why pressing Edit on the Extra tab appeared to do nothing.
    if (tmpl->style & WS_CHILD) {
        dlg->parent = parent;
        if (parent) parent->children.push_back(dlg);
        else        g_topLevelOrder.push_back(dlg);
    } else {
        dlg->owner  = parent;
        dlg->parent = nullptr;
        g_topLevelOrder.push_back(dlg);
    }

    for (int i = 0; i < tmpl->controlCount; ++i) {
        const orbiter_res::ControlTemplate &ct = tmpl->controls[i];

        Window *c = createWindow();
        c->id        = ct.id;
        c->className = ct.className ? ct.className : "";
        c->text      = ct.text ? ct.text : "";
        c->x  = duToPxX(ct.x);  c->y  = duToPxY(ct.y);
        c->cx = duToPxX(ct.cx); c->cy = duToPxY(ct.cy);
        c->style     = ct.style;
        c->exStyle   = ct.exStyle;
        c->bitmapId  = ct.bitmapId;
        c->parent    = dlg;
        c->enabled   = (ct.style & WS_DISABLED) == 0;
        c->visible   = true;

        // Recorded from the template, before any code rewrites the style.
        // See the note on Window::treeCheckboxes.
        c->treeCheckboxes = (c->className == "SysTreeView32") &&
                            (ct.style & TVS_CHECKBOXES) != 0;

        // Checkbox and radio styles start unchecked, as Windows does.
        c->checkState = BST_UNCHECKED;

        // A control whose class is one Orbiter registered -- the DlgCtrl
        // gauges, switches and property list -- gets that class's window
        // procedure and private storage, exactly as CreateWindowEx would.
        // Without this the control would never receive WM_PAINT and would
        // have nowhere to keep its state.
        auto cit = g_windowClasses.find(c->className);
        if (cit != g_windowClasses.end()) {
            c->wndProc = cit->second.proc;
            if (cit->second.extra > 0) {
                const size_t slots =
                    ((size_t)cit->second.extra + sizeof(LONG_PTR) - 1) / sizeof(LONG_PTR);
                c->extra.assign(slots, 0);
            }
        }

        dlg->children.push_back(c);
    }

    return dlg;
}

// Constrains a bounded control's position, TOLERATING A REVERSED RANGE.
//
// std::clamp REQUIRES lo <= hi, and that precondition is CHECKED: libstdc++
// built with _GLIBCXX_ASSERTIONS aborts the process on
//
//     stl_algo.h:3638: std::clamp(...) [with _Tp = int]:
//     Assertion '!(__hi < __lo)' failed.
//
// Win32 has no such precondition. An up-down whose upper limit is BELOW its
// lower limit is legal and documented -- UDM_SETRANGE says the control simply
// reverses direction -- and an empty range is what any control with nothing to
// count over naturally gets.
//
// THIS IS NOT HYPOTHETICAL, AND IT ABORTED THE WHOLE PROGRAM. Meshdebug's
// RefreshDialog is pristine upstream (Meshdebug.cpp:207) and does
//
//     SendMessage (GetDlgItem (hDlg, IDC_MESHSPIN), UDM_SETRANGE, 0,
//                  MAKELONG (g_nmesh-1, 0));
//
// where g_nmesh is an unsigned DWORD. A visual whose meshes are not counted
// yet makes that MAKELONG(0xFFFFFFFF, 0) -- low word 0xFFFF, which the 16-bit
// form reads as MAXIMUM -1, against a minimum of 0. Pressing "Mesh debugger"
// in the F4 Function menu then killed the process inside std::clamp.
//
// It presented as INTERMITTENT, and the reason is worth writing down: the two
// UDM_SETRANGE calls sit inside `if (vis)`, so they only run when a visual is
// current, while the "(0 to -1)" label beside them is printed unconditionally.
// A run with no visual showed the tell-tale label and did not crash.
//
// Windows clamps into the interval the two bounds span, whichever way round
// they are, and never faults. So does this. Every scroll-state clamp in this
// file goes through it -- including the two or three that are provably safe
// today -- because a mixture of the two spellings is an invitation to copy the
// wrong one into the next control.
inline int boundedPos(int v, int a, int b)
{
    const int lo = std::min(a, b);
    const int hi = std::max(a, b);
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

} // namespace

// ===========================================================================
// Dialog creation and lifetime
// ===========================================================================

extern "C" {

void orbiter_ListBoxSelfTest(void);       // defined below
void orbiter_ProgressBarSelfTest(void);   // defined below
void orbiter_RadioGroupSelfTest(void);    // defined below

// Config.cpp. Not a dialog check -- it is here because this is the earliest
// point in the run where the log is open, which is the whole reason the three
// above moved here from main(). See the note in CreateDialogParam.
void orbiter_ConfigPathSelfTest(void);

// The module a template is read from, as a bare name.
//
// CreateDialogParam's first argument names the module Windows reads the
// template out of, and this shim ignored it entirely -- so ids were global and
// two modules using the same IDD_* number gave one of them the other's layout.
// GetModuleFileName turns the HINSTANCE back into the path LoadLibrary
// recorded, and its stem is what rc2cpp.py registered the tables under (the
// CMake target name: "Modules/DeltaGlider.so" -> "DeltaGlider"). The
// executable's own hInst resolves to /proc/self/exe, whose stem matches no
// registered module, so core dialogs fall through to the ordered search and
// behave exactly as they always did.
static std::string moduleNameOf(HINSTANCE inst)
{
    if (!inst) return std::string();
    char path[1024] = { 0 };
    if (!GetModuleFileNameA((HMODULE)inst, path, sizeof(path)))
        return std::string();
    return std::filesystem::path(path).stem().string();
}

HWND CreateDialogParam(HINSTANCE inst, LPCSTR templateName, HWND parent,
                       DLGPROC proc, LPARAM param)
{
    // The dialog layer's own contract checks, run once before the first
    // dialog exists.
    //
    // NOT from main(): the log is not open that early and the result went
    // nowhere -- the first attempt printed nothing at all, which is exactly
    // the "a check that stops running looks like one that passes" failure it
    // is meant to guard against. Here, Orbiter::Create has already run.
    static bool bSelfTested = false;
    if (!bSelfTested) {
        bSelfTested = true;
        orbiter_ListBoxSelfTest();
        orbiter_ProgressBarSelfTest();
        orbiter_RadioGroupSelfTest();
        orbiter_ConfigPathSelfTest();
    }

    int id = 0;
    if (!resourceId(templateName, &id)) return nullptr;

    const std::string mod = moduleNameOf(inst);
    const orbiter_res::DialogTemplate *tmpl =
        orbiter_res::FindDialogTemplateIn(mod.c_str(), id);
    if (!tmpl) return nullptr;

    Window *dlg = instantiateTemplate(tmpl, toWindow(parent), proc, param);

    // WM_INITDIALOG carries the creation parameter in lParam. This is where
    // every Orbiter dialog populates its controls, so it must be sent before
    // the handle is returned.
    dispatchToProc(dlg, WM_INITDIALOG, 0, param);

    return toHwnd(dlg);
}

HWND CreateDialog(HINSTANCE inst, LPCSTR templateName, HWND parent,
                  DLGPROC proc)
{
    return CreateDialogParam(inst, templateName, parent, proc, 0);
}

INT_PTR DialogBoxParam(HINSTANCE inst, LPCSTR templateName, HWND parent,
                       DLGPROC proc, LPARAM param)
{
    HWND h = CreateDialogParam(inst, templateName, parent, proc, param);
    Window *dlg = toWindow(h);
    if (!dlg) return -1;

    dlg->modal   = true;
    dlg->visible = true;
    g_modalStack.push_back(dlg);

    // Win32 runs a nested message loop here and does not return until
    // EndDialog is called. The same shape is kept: the frame pump drives the
    // dialog, and this spins until the procedure ends it.
    //
    // IsDialogMessage IS THE MODAL LOOP'S JOB, and leaving it out is what
    // separates a dialog loop from a plain message loop. Windows' own
    // DialogBoxParam calls it internally; a DLGPROC does not implement Tab
    // order, Enter for the default button, Escape for cancel, arrow-key
    // navigation or Alt+mnemonics -- IsDialogMessage does, and it is the only
    // thing that does.
    //
    // The keys were arriving and being thrown away. UIHost::postKeyboardMessages
    // posts them to orbiter_ActiveDialog(), which IS the modal while one is up
    // (the most recently created visible top-level window), so they landed in
    // the queue correctly, were pulled out here, and went straight to
    // DispatchMessage -- which hands them to a procedure that has no handling
    // for them. Every modal in the tree was keyboard-dead: the Extra tab's
    // Edit dialogs, Save-scenario-as, the module configuration dialogs.
    //
    // The modeless Launchpad never had this problem because Launchpad.cpp:149
    // routes its own messages through IsDialogMessage explicitly, from
    // LaunchpadDialog::ConsumeMessage. Only the nested loop was missing it.
    //
    // MEASURED, both ways, with the scripted UI driver (Linux/UiDriver.cpp)
    // opening the About tab's Disclaimer box -- a real DialogBoxParam modal
    // whose procedure ends on IDOK or IDCANCEL:
    //
    //   without this call   modal_open       dialog=199 focus=-1 modal=1
    //                       after TAB        dialog=199 focus=-1 modal=1
    //                       after TAB        dialog=199 focus=-1 modal=1
    //                       after ESC        dialog=199 focus=-1 modal=1
    //
    //   with it             modal_open       dialog=199 focus=-1 modal=1
    //                       after TAB        dialog=199 focus=1    modal=1
    //                       after TAB        dialog=199 focus=1050 modal=1
    //                       after ESC        dialog=171 focus=0    modal=0
    //
    // So the pre-fix state was worse than "no tab order": ESCAPE COULD NOT
    // CLOSE THE DIALOG. A user without a mouse had no way out of any modal in
    // the program.
    while (!dlg->endDialogCalled && !g_quitPosted) {
        MSG msg;
        if (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (!IsDialogMessageA(toHwnd(dlg), &msg)) {
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
        } else {
            Sleep(1);   // idle rather than spin
        }
    }

    const INT_PTR result = dlg->modalResult;

    if (!g_modalStack.empty()) g_modalStack.pop_back();
    destroyWindowRecursive(dlg);
    return result;
}

BOOL EndDialog(HWND h, INT_PTR result)
{
    Window *w = toWindow(h);
    if (!w) return FALSE;
    w->modalResult     = result;
    w->endDialogCalled = true;
    w->visible         = false;
    return TRUE;
}

BOOL DestroyWindow(HWND h)
{
    Window *w = toWindow(h);
    if (!w) return FALSE;
    dispatchToProc(w, WM_DESTROY, 0, 0);
    destroyWindowRecursive(w);
    return TRUE;
}

HWND CreateWindowExA(DWORD exStyle, LPCSTR className, LPCSTR windowName,
                     DWORD style, int x, int y, int w, int h,
                     HWND parent, HMENU menu, HINSTANCE, LPVOID param)
{
    // CW_USEDEFAULT IS A SENTINEL, NOT A COORDINATE, and storing it as one is
    // an arithmetic bomb.
    //
    // It is (int)0x80000000 -- INT_MIN -- and Windows RESOLVES it at creation:
    // the system picks a position and from then on the window has a real one,
    // which is what GetWindowRect, ClientToScreen and ScreenToClient go on to
    // use. This kept the sentinel, so every one of those did arithmetic on
    // INT_MIN.
    //
    // The window it happens to is the RENDER window. GraphicsAPI.cpp:347 and
    // :351 -- both branches of GraphicsClient::clbkCreateRenderWindow, which
    // VulkanClient::clbkCreateRenderWindow calls -- pass CW_USEDEFAULT for x
    // and y. So Camera::UpdateMouse's
    //
    //     ScreenToClient (g_pOrbiter->GetRenderWnd(), &pt);
    //     dx = pt.x - mx;
    //
    // subtracted INT_MIN from a cursor position of 446, which wraps to
    // INT_MIN + 446, and dx came out as exactly INT_MIN. ShiftPhi/ShiftTheta
    // were then handed -INT_MIN * 0.005 radians. MEASURED: one right-click
    // with the hand held still moved the camera from phi 120 to phi -159.75
    // and theta 0 to theta 80.25 -- the view "flips and does weird shit" on
    // the press, before any drag, and again on every press after it.
    //
    // Resolved to zero, which is both halves of the Win32 rule at once: for a
    // pop-up or child window Windows itself substitutes zero, and for the
    // overlapped render window this port has no separate window to place --
    // UIHost.cpp's postMouseMessages already states the invariant the rest of
    // the port relies on, that "the render window's origin is (0,0) and it has
    // no parent, so Win32Dlg's ScreenToClient subtracts nothing". It was true
    // of every window except the one that mattered.
    //
    // The size sentinel is resolved too. Nothing in this tree passes it -- the
    // render window is created at VideoData.winw x winh -- but a zero-sized
    // window is a visible failure and a 2.1-billion-pixel one is a baffling
    // one, so it does not get to stay either.
    if (x == CW_USEDEFAULT) x = 0;
    if (y == CW_USEDEFAULT) y = 0;
    if (w == CW_USEDEFAULT) w = 0;
    if (h == CW_USEDEFAULT) h = 0;

    Window *win = createWindow();
    win->className = className ? className : "";
    win->text      = windowName ? windowName : "";
    win->x = x; win->y = y; win->cx = w; win->cy = h;
    win->style   = style;
    win->exStyle = exStyle;
    win->parent  = toWindow(parent);
    win->visible = (style & WS_VISIBLE) != 0;
    win->enabled = (style & WS_DISABLED) == 0;
    // For a child window the menu slot is the control id, as in Win32.
    win->id = (style & WS_CHILD) ? (int)(intptr_t)menu : 0;

    if (win->parent) win->parent->children.push_back(win);

    auto it = g_windowClasses.find(win->className);
    if (it != g_windowClasses.end()) {
        win->wndProc = it->second.proc;
        // Allocate the class's requested private storage, rounded up to whole
        // pointer-sized slots since that is how it is addressed.
        if (it->second.extra > 0) {
            const size_t slots =
                ((size_t)it->second.extra + sizeof(LONG_PTR) - 1) / sizeof(LONG_PTR);
            win->extra.assign(slots, 0);
        }
    }

    dispatchToProc(win, WM_CREATE, 0, (LPARAM)param);
    return toHwnd(win);
}

// ===========================================================================
// Control lookup and window words
// ===========================================================================

HWND GetDlgItem(HWND dlg, int id)
{
    Window *w = toWindow(dlg);
    if (!w) return nullptr;
    // Direct children first, then descend: Orbiter nests tab pages inside a
    // container control and still asks the top-level dialog for their ids.
    for (Window *c : w->children)
        if (c->id == id) return toHwnd(c);
    for (Window *c : w->children) {
        HWND found = GetDlgItem(toHwnd(c), id);
        if (found) return found;
    }
    return nullptr;
}

int GetDlgCtrlID(HWND h)
{
    Window *w = toWindow(h);
    return w ? w->id : 0;
}

HWND GetParent(HWND h)
{
    Window *w = toWindow(h);
    if (!w) return nullptr;
    // For an owned popup, GetParent reports the owner -- which is what
    // Win32 does and what callers use to centre a dialog on its opener.
    if (w->parent) return toHwnd(w->parent);
    if (w->owner)  return toHwnd(w->owner);
    return nullptr;
}

BOOL IsChild(HWND parent, HWND child)
{
    Window *p = toWindow(parent);
    Window *c = toWindow(child);
    if (!p || !c) return FALSE;
    for (Window *w = c->parent; w; w = w->parent)
        if (w == p) return TRUE;
    return FALSE;
}

// Added for OVP/VulkanClient's VideoTab, which walks up from its own tab page
// to the Launchpad window and then back down looking for the scenario tree.
// Both of these are walks over the parent/children links this file already
// maintains; neither adds any state.

HWND GetAncestor(HWND h, UINT flags)
{
    Window *w = toWindow(h);
    if (!w) return nullptr;

    if (flags == GA_PARENT) {
        // Win32's GA_PARENT is NOT GetParent: for a top-level window GetParent
        // reports the OWNER and GA_PARENT reports the desktop, i.e. nothing
        // here. So this answers the parent link only.
        return w->parent ? toHwnd(w->parent) : nullptr;
    }

    Window *root = w;
    while (root->parent) root = root->parent;

    // GA_ROOTOWNER keeps going through the owner chain, which is what tells an
    // owned popup's root apart from its owner's.
    if (flags == GA_ROOTOWNER) {
        while (root->owner) {
            root = root->owner;
            while (root->parent) root = root->parent;
        }
    }
    return toHwnd(root);
}

namespace {

// Depth first, stopping the moment the callback returns FALSE. Returns false
// up the recursion so the whole walk unwinds, which is what Win32 does --
// EnumChildWindows itself then returns the callback's last value.
bool enumChildren(Window *w, WNDENUMPROC fn, LPARAM param)
{
    // A copy, because the callback is allowed to create or destroy windows.
    std::vector<Window *> kids = w->children;
    for (Window *c : kids) {
        if (!fn(toHwnd(c), param)) return false;
        if (!enumChildren(c, fn, param)) return false;
    }
    return true;
}

} // namespace

BOOL EnumChildWindows(HWND parent, WNDENUMPROC fn, LPARAM param)
{
    Window *w = toWindow(parent);
    if (!w || !fn) return FALSE;
    return enumChildren(w, fn, param) ? TRUE : FALSE;
}

// Window words.
//
// Non-negative indices address two DIFFERENT things depending on the window,
// and conflating them corrupts both:
//
//   * On a dialog, 0/8/16 are DWLP_MSGRESULT/DWLP_DLGPROC/DWLP_USER.
//     LpadTab.cpp stores its LaunchpadTab* at DWLP_USER.
//   * On a window of a registered class, index N is a byte offset into the
//     cbWndExtra the class asked for. DlgCtrl keeps a gauge's position at
//     offset 0, and CustomCtrl::SetHwnd stores `this` at offset 0 too.
//
// So offset 0 means "message result" on a dialog and "the control's own
// pointer" on a custom control. The window's kind is what disambiguates.
LONG_PTR GetWindowLongPtrA(HWND h, int index)
{
    Window *w = toWindow(h);
    if (!w) return 0;

    switch (index) {
    case GWLP_USERDATA:   return w->userData;
    case GWL_STYLE:       return (LONG_PTR)w->style;
    case GWL_EXSTYLE:     return (LONG_PTR)w->exStyle;
    case GWLP_WNDPROC:    return (LONG_PTR)w->wndProc;
    case GWLP_HWNDPARENT: return (LONG_PTR)(w->parent ? toHwnd(w->parent)
                                                      : nullptr);
    default: break;
    }

    if (index < 0) return w->dwlUser;

    if (w->isDialog) {
        switch (index) {
        case 0:  return (LONG_PTR)w->msgResult;          // DWLP_MSGRESULT
        case 8:  return (LONG_PTR)w->dlgProc;            // DWLP_DLGPROC
        default: break;                                  // DWLP_USER and up
        }
    }

    const size_t slot = (size_t)index / sizeof(LONG_PTR);
    return slot < w->extra.size() ? w->extra[slot] : 0;
}

LONG_PTR SetWindowLongPtrA(HWND h, int index, LONG_PTR value)
{
    Window *w = toWindow(h);
    if (!w) return 0;
    const LONG_PTR prev = GetWindowLongPtrA(h, index);

    switch (index) {
    case GWLP_USERDATA:   w->userData = value; return prev;
    case GWL_STYLE:       w->style    = (DWORD)value; return prev;
    case GWL_EXSTYLE:     w->exStyle  = (DWORD)value; return prev;
    case GWLP_WNDPROC:    w->wndProc  = (WNDPROC)value; return prev;
    case GWLP_HWNDPARENT: return prev;   // reparenting is not used here
    default: break;
    }

    if (index < 0) { w->dwlUser = value; return prev; }

    if (w->isDialog) {
        switch (index) {
        case 0: w->msgResult = (LRESULT)value; return prev;   // DWLP_MSGRESULT
        case 8: w->dlgProc   = (DLGPROC)value; return prev;   // DWLP_DLGPROC
        default: break;
        }
    }

    const size_t slot = (size_t)index / sizeof(LONG_PTR);
    // Grow rather than reject: a class that under-declared its cbWndExtra
    // would otherwise silently drop writes, and losing a gauge's range is far
    // harder to spot than a slightly larger buffer.
    if (slot >= w->extra.size()) w->extra.resize(slot + 1, 0);
    w->extra[slot] = value;
    return prev;
}

// ===========================================================================
// THE WM_USER COLLISION -- why the progress bar is dispatched by class
// ===========================================================================
//
// Common-control messages are numbered from WM_USER *per class*, and the
// trackbar's range and the progress bar's range overlap exactly:
//
//     value        trackbar            progress bar
//     WM_USER+1    TBM_GETRANGEMIN     PBM_SETRANGE
//     WM_USER+2    TBM_GETRANGEMAX     PBM_SETPOS
//     WM_USER+3    TBM_GETTIC          PBM_DELTAPOS
//     WM_USER+4    TBM_SETTIC          PBM_SETSTEP
//     WM_USER+5    TBM_SETPOS          PBM_STEPIT
//     WM_USER+6    TBM_SETRANGE        PBM_SETRANGE32
//     WM_USER+7    TBM_SETRANGEMIN     PBM_GETRANGE
//     WM_USER+8    TBM_SETRANGEMAX     PBM_GETPOS
//
// On Windows this is not ambiguous, because SendMessage delivers to the
// TARGET WINDOW'S procedure and each class's procedure reads the number its
// own way. This shim has ONE switch serving every class, so the number alone
// cannot decide -- and the compiler proved it, rejecting the file with six
// "duplicate case value" errors the moment the PBM_ cases were written beside
// the TBM_ ones. Had the trackbar cases not already been there, the same
// numbers would have been answered with the wrong semantics in silence:
// Launchpad.cpp's PBM_SETPOS is 0x0402, which the switch above reads as
// TBM_GETRANGEMAX -- a read, so the bar would simply never have moved.
//
// So the overlapping band is dispatched by CLASS first, which is what Win32
// itself does. ONLY WM_USER+0..+8 overlaps: TBM_SETTICFREQ/PAGESIZE/LINESIZE
// sit at +20, +21 and +23 and every UDM_ at +101 and above, so those stay in
// the shared switch where they are unambiguous. TVM_ (0x1100), LVM_ (0x1000)
// and TCM_ (0x1300) are numbered from their own bases and never collide.
//
// Class names are compared case-insensitively because Win32's are. The
// templates spell it "msctls_progress32", but RegisterClass and CreateWindow
// match without regard to case, so a caller spelling it otherwise must not
// silently fall through to the trackbar's reading of the same number.

static bool classIs(const Window *w, const char *name)
{
    return w && strcasecmp(w->className.c_str(), name) == 0;
}

// Where a sorted list box or combo puts a new string.
//
// CBS_SORT / LBS_SORT make ADDSTRING insert in order rather than append, and
// return the index it used. The comparison is CASE-INSENSITIVE: a stock
// sorted list box orders through the locale's string collation, not a byte
// compare, so "Earth" and "earth" sort together rather than the whole of
// upper case sorting before the whole of lower case.
static size_t sortedInsertPos(const Window *w, const char *text)
{
    const auto before = [](const std::string &a, const char *b) {
        return strcasecmp(a.c_str(), b) < 0;
    };
    return (size_t)(std::lower_bound(w->items.begin(), w->items.end(),
                                     text, before) - w->items.begin());
}

// Keep a parallel per-item vector aligned across a sorted insert.
//
// itemData is grown LAZILY -- LB_SETITEMDATA/CB_SETITEMDATA resize it to
// items.size() on first use -- so it is routinely empty or short. Appending
// to a short vector is harmless, but INSERTING into one at an index past its
// end would silently shift every later item's data onto the wrong string.
// So: leave it alone while it is empty (nobody has attached data), and
// otherwise pad it to the pre-insert length before inserting at `at`.
static void insertParallel(std::vector<LONG_PTR> &v, size_t at, size_t newCount)
{
    if (v.empty()) return;                 // no data attached to this control
    if (v.size() < newCount - 1) v.resize(newCount - 1, 0);
    v.insert(v.begin() + std::min(at, v.size()), 0);
}

// The progress bar's reading of the WM_USER band.
//
// It is another bounded integer, so it reuses the same scroll state as the
// trackbar and the up-down. LIVE ON THE LAUNCHPAD: LaunchpadDialog::WaitProc
// sets the range at WM_INITDIALOG and UpdateWaitProgress drives the position
// while a scenario loads --
//
//     Launchpad.cpp:445  PBM_SETRANGE, 0, MAKELPARAM(0, 1000)
//     Launchpad.cpp:490  PBM_SETPOS,   0, 0
//     Launchpad.cpp:509  PBM_SETPOS,   (mem0 - mem) / mem_wait, 0
//
// Returns false when the message is not one of the progress bar's, so the
// caller falls through to the shared switch.
static bool progressBarMessage(Window *w, UINT msg, WPARAM wp, LPARAM lp,
                               LRESULT &out)
{
    // The wait page is transient -- it exists only between CloseSession's
    // "destroy the world" and "hide it again" -- so the bar cannot be caught
    // reliably in a screenshot. This reports what it was actually told, and
    // whether it was visible to be told it.
    if (g_traceMsg && msg >= WM_USER && msg <= WM_USER + 8)
        TRACEMSG("progress id=%d msg=0x%04X wp=%ld lp=%ld "
                 "(range %d..%d pos %d, visible=%d)",
                 w->id, msg, (long)wp, (long)lp,
                 w->scrollV.minPos, w->scrollV.maxPos, w->scrollV.pos,
                 (int)w->visible);

    switch (msg) {
    case PBM_SETRANGE: {
        const int lo = (int)(short)LOWORD(lp);
        const int hi = (int)(short)HIWORD(lp);
        out = MAKELONG(w->scrollV.minPos, w->scrollV.maxPos);
        w->scrollV.minPos = lo;
        w->scrollV.maxPos = hi;
        w->scrollV.pos = boundedPos(w->scrollV.pos, lo, hi);
        return true;
    }

    case PBM_SETRANGE32:
        out = MAKELONG(w->scrollV.minPos, w->scrollV.maxPos);
        w->scrollV.minPos = (int)wp;
        w->scrollV.maxPos = (int)lp;
        w->scrollV.pos = boundedPos(w->scrollV.pos,
                                    w->scrollV.minPos, w->scrollV.maxPos);
        return true;

    case PBM_SETPOS:
        out = w->scrollV.pos;
        w->scrollV.pos = boundedPos((int)wp,
                                    w->scrollV.minPos, w->scrollV.maxPos);
        return true;

    case PBM_DELTAPOS:
        out = w->scrollV.pos;
        w->scrollV.pos = boundedPos(w->scrollV.pos + (int)wp,
                                    w->scrollV.minPos, w->scrollV.maxPos);
        return true;

    case PBM_GETPOS:
        out = w->scrollV.pos;
        return true;

    case PBM_GETRANGE:
        // wParam selects which limit to return; lParam may point at a
        // PBRANGE, which nothing in this tree passes.
        out = wp ? w->scrollV.minPos : w->scrollV.maxPos;
        return true;

    case PBM_SETSTEP:
        // The step defaults to 10 on Windows, and 0 in the ScrollState, so
        // the zero has to be read as "unset" rather than "step by nothing".
        out = w->scrollV.page ? w->scrollV.page : 10;
        w->scrollV.page = (int)wp;
        return true;

    case PBM_STEPIT: {
        out = w->scrollV.pos;
        const int step = w->scrollV.page ? w->scrollV.page : 10;
        int next = w->scrollV.pos + step;
        // StepIt WRAPS at the top rather than clamping, which is the one
        // place a progress bar differs from a trackbar.
        if (next > w->scrollV.maxPos) next = w->scrollV.minPos;
        w->scrollV.pos = next;
        return true;
    }

    default:
        return false;
    }
}

// ===========================================================================
// Message delivery
//
// SendMessage is synchronous, as on Windows: control messages mutate state
// and return immediately, and anything else reaches the window procedure.
// ===========================================================================

LRESULT SendMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Window *w = toWindow(h);
    if (!w) return 0;

    // Class-private message numbers, before the shared switch can misread
    // them. See THE WM_USER COLLISION above.
    if (classIs(w, "msctls_progress32")) {
        LRESULT out = 0;
        if (progressBarMessage(w, msg, wp, lp, out)) return out;
    }

    // THE OTHER HALF OF THE WM_USER COLLISION -- THE SYNCHRONOUS HALF.
    //
    // WM_USER is where an application's OWN private messages begin. A window
    // procedure may number its messages from WM_USER upwards and nothing else
    // may read them; that is the whole contract, and it is why the same number
    // means TBM_GETPOS to a trackbar and something else entirely to a dialog.
    // Windows keeps them apart because SendMessage delivers to the TARGET
    // window's procedure. This shim's shared switch decides on the NUMBER, so
    // a private message SENT to a dialog was answered by whichever control
    // case sat at that number and never reached the dialog: WM_USER+0 is
    // TBM_GETPOS, which returns scrollV.pos -- a plausible-looking zero.
    //
    // ONLY THE SYNCHRONOUS PATH WAS AFFECTED, and that is worth stating
    // precisely because it is easy to get backwards. DispatchMessageA calls
    // dispatchToProc directly, so every POSTED WM_USER already reached its
    // window procedure. The Scenario Editor's Landed tab (Editor.cpp:2129,
    // 2134, 2318) and the Launchpad's Modules tab (TabModule.cpp:384) post,
    // and both were working; ModuleTab::InitActivation is reached exactly once
    // per visit either way, measured.
    //
    // WHAT WAS ACTUALLY BROKEN is the Scenario Editor's module API. A vessel
    // adds its own editor page by SENDING, and WM_SCNEDITOR is WM_USER+0:
    //
    //     DeltaGlider.cpp:1960  SendMessage (hEditor, WM_SCNEDITOR,
    //                                        SE_ADDPAGEBUTTON, &eps1)
    //     DeltaGlider.cpp:1962  ... eps2      1965  ... eps3
    //     ShuttleA.cpp:2611     SendMessage (hEditor, WM_SCNEDITOR,
    //                                        SE_ADDPAGEBUTTON, &eps1)
    //
    // Each answered with a trackbar position instead of reaching
    // EditorTab_Edit::TabProc's WM_SCNEDITOR case (Editor.cpp:1407), so
    // AddPageButton never ran and the vessel's own pages never appeared on the
    // editor's Edit tab. The page, once open, asks the editor which vessel it
    // is editing the same way --
    //
    //     DeltaGlider.cpp:1780  SendMessage (hDlg, WM_SCNEDITOR,
    //                                        SE_GETVESSEL, &hVessel)
    //     ShuttleA.cpp:2496     the same
    //
    // -- and EditorTab_Custom::TabProc (Editor.cpp:3087) is what writes the
    // handle back, so hVessel would have been left uninitialised.
    //
    // The rule restored is Windows': the band is class-private, so only the
    // classes that own those numbers may have them read as control messages.
    // The trackbar's TBM_ cases are in the shared switch below and the
    // progress bar's PBM_ cases are handled just above; every other window
    // gets its private message delivered to its own procedure, which is what
    // WM_USER means.
    if (msg >= WM_USER && msg <= WM_USER + 8 &&
        !classIs(w, TRACKBAR_CLASS) && !classIs(w, PROGRESS_CLASS))
        return dispatchToProc(w, msg, wp, lp);

    switch (msg) {

    // -- generic text ------------------------------------------------------
    case WM_SETTEXT:
        w->text = lp ? (const char *)lp : "";
        return TRUE;

    case WM_GETTEXT: {
        if (!lp || wp == 0) return 0;
        char *buf = (char *)lp;
        size_t n = std::min((size_t)wp - 1, w->text.size());
        memcpy(buf, w->text.c_str(), n);
        buf[n] = '\0';
        return (LRESULT)n;
    }

    case WM_GETTEXTLENGTH:
        return (LRESULT)w->text.size();

    case WM_ENABLE:
        w->enabled = wp != 0;
        return 0;

    // -- buttons -----------------------------------------------------------
    case BM_SETCHECK:
        w->checkState = (int)wp;
        return 0;

    case BM_GETCHECK:
        return w->checkState;

    case BM_GETSTATE:
        return w->checkState;

    // -- combo box ---------------------------------------------------------
    //
    // A COMBO BOX'S SELECTION AND ITS WINDOW TEXT ARE THE SAME THING ON
    // WINDOWS, and this shim kept only the first of them.
    //
    // A combo is a list plus an edit field, and GetWindowText reads the EDIT
    // FIELD. Selecting an item -- by CB_SETCURSEL, by CB_SELECTSTRING, or by
    // the user picking from the drop-down -- replaces the edit field's
    // contents with that item's string, so on Windows
    //
    //     SendDlgItemMessage(hDlg, IDC_REF, CB_SELECTSTRING, -1, "Earth");
    //     GetWindowText(GetDlgItem(hDlg, IDC_REF), cbuf, 256);   // "Earth"
    //
    // Here CB_SETCURSEL wrote curSel and nothing else, GetWindowTextA returned
    // w->text, and w->text was empty -- so the second line gave "".
    //
    // WHAT THAT COST, and it is not subtle. Every Scenario Editor tab that
    // works from a reference body reads it exactly that way:
    //
    //     EditorTab_Elements::Refresh   Editor.cpp:1546
    //         GetWindowText (GetDlgItem (hTab, IDC_REF), cbuf, 256);
    //         OBJHANDLE hRef = oapiGetGbodyByName (cbuf);
    //         if (!hRef) return;              <-- always taken
    //
    // so Refresh returned before writing a single field: the six orbital
    // element boxes stayed empty and the nine secondary parameters stayed at
    // the template's "XXX" placeholders, on a tab that otherwise drew
    // perfectly. Apply() has the same three lines and silently did nothing.
    // The same shape is in EditorTab_Statevec, EditorTab_Landed,
    // ScnEditor::SetBasePosition and ScnEditor::SelectBase.
    //
    // Fixed where Windows keeps it rather than by special-casing
    // GetWindowText: the selection writes the text. That way every reader --
    // GetWindowText, GetDlgItemText, WM_GETTEXT and the renderer's own
    // preview -- sees one value, and an EDITABLE combo (CBS_DROPDOWN, which
    // IDC_REF is) can still hold typed text that is not in the list, exactly
    // as it does on Windows.
    // CBS_SORT: ADDSTRING INSERTS, IT DOES NOT APPEND.
    //
    // Both callers fill these in index order and rely on the control to sort:
    //
    //     ScnEditor::ScanCBodyList          Editor.cpp:103
    //         for (n = 0; n < oapiGetGbodyCount(); n++) {
    //             oapiGetObjectName (oapiGetGbodyByIndex (n), cbuf, 256);
    //             SendDlgItemMessage (hDlg, hList, CB_ADDSTRING, 0, cbuf);
    //         }
    //
    // -- so IDC_REF, the gravity-reference combo on the Elements, State
    // vector and Landed tabs, listed the celestial bodies in solar-system
    // definition order here and alphabetically on Windows.
    case CB_ADDSTRING: {
        const char *text = lp ? (const char *)lp : "";
        if (!(w->style & CBS_SORT)) {
            w->items.push_back(text);
            return (LRESULT)(w->items.size() - 1);
        }
        const size_t at = sortedInsertPos(w, text);
        w->items.insert(w->items.begin() + at, text);
        insertParallel(w->itemData, at, w->items.size());
        if (w->curSel >= (int)at) ++w->curSel;
        return (LRESULT)at;
    }

    case CB_INSERTSTRING: {
        const size_t at = std::min((size_t)wp, w->items.size());
        w->items.insert(w->items.begin() + at, lp ? (const char *)lp : "");
        if (at < w->itemData.size()) w->itemData.insert(w->itemData.begin() + at, 0);
        return (LRESULT)at;
    }

    case CB_DELETESTRING:
        if (wp >= w->items.size()) return CB_ERR;
        w->items.erase(w->items.begin() + wp);
        if (wp < w->itemData.size()) w->itemData.erase(w->itemData.begin() + wp);
        if (w->curSel >= (int)w->items.size()) w->curSel = -1;
        comboSyncText(w);
        return (LRESULT)w->items.size();

    case CB_RESETCONTENT:
        w->items.clear();
        w->itemData.clear();
        w->curSel = -1;
        comboSyncText(w);
        return 0;

    case CB_GETCOUNT:
        return (LRESULT)w->items.size();

    case CB_SETCURSEL:
        // A negative index clears the selection rather than being an error.
        if ((int)wp < 0) { w->curSel = -1; comboSyncText(w); return CB_ERR; }
        if (wp >= w->items.size()) return CB_ERR;
        w->curSel = (int)wp;
        comboSyncText(w);
        return w->curSel;

    case CB_GETCURSEL:
        return w->curSel < 0 ? CB_ERR : w->curSel;

    case CB_GETLBTEXT: {
        if (wp >= w->items.size() || !lp) return CB_ERR;
        const std::string &s = w->items[wp];
        memcpy((char *)lp, s.c_str(), s.size() + 1);
        return (LRESULT)s.size();
    }

    case CB_GETLBTEXTLEN:
        if (wp >= w->items.size()) return CB_ERR;
        return (LRESULT)w->items[wp].size();

    case CB_FINDSTRINGEXACT:
    case CB_FINDSTRING: {
        if (!lp) return CB_ERR;
        const std::string needle((const char *)lp);
        // The search starts *after* the given index and wraps, which is the
        // documented behaviour and what callers passing -1 rely on.
        const int count = (int)w->items.size();
        if (count == 0) return CB_ERR;
        const int start = ((int)wp + 1) % count;
        for (int n = 0; n < count; ++n) {
            const int i = (start + n) % count;
            const std::string &s = w->items[i];
            // CASE-INSENSITIVE, both forms. This was a byte-wise comparison,
            // which is not what either message does on Windows and which
            // fails exactly when a caller's spelling differs in case from the
            // string it added -- a lookup that silently finds nothing.
            const bool hit = (msg == CB_FINDSTRINGEXACT)
                ? (s.size() == needle.size() &&
                   strcasecmp(s.c_str(), needle.c_str()) == 0)
                : (s.size() >= needle.size() &&
                   strncasecmp(s.c_str(), needle.c_str(), needle.size()) == 0);
            if (hit) return i;
        }
        return CB_ERR;
    }

    // -- list box ----------------------------------------------------------
    // LBS_SORT, the same as CBS_SORT above.
    //
    //     ScnEditorTab::ScanVesselList      Editor.cpp:473
    //         for (i = 0; i < oapiGetVesselCount(); i++) { ...
    //             SendDlgItemMessage (hTab, ResId, LB_ADDSTRING, 0, cbuf);
    //
    // IDC_LIST1 on the Vessel tab is the editor's main vessel list, and it
    // was in scenario order rather than alphabetical. It also carries
    // LBS_USETABSTOPS and its items are "name\t(class)\tref"; sorting the
    // whole string is what Windows does, and since the name comes first and
    // the tab terminates it, that orders by name.
    case LB_ADDSTRING: {
        const char *text = lp ? (const char *)lp : "";
        if (!(w->style & LBS_SORT)) {
            w->items.push_back(text);
            w->itemSel.push_back(0);
            return (LRESULT)(w->items.size() - 1);
        }
        const size_t at = sortedInsertPos(w, text);
        w->items.insert(w->items.begin() + at, text);
        w->itemSel.insert(w->itemSel.begin() + std::min(at, w->itemSel.size()), 0);
        insertParallel(w->itemData, at, w->items.size());
        if (w->curSel >= (int)at) ++w->curSel;
        return (LRESULT)at;
    }

    case LB_RESETCONTENT:
        w->items.clear();
        w->itemSel.clear();
        w->itemData.clear();
        w->curSel = -1;
        return 0;

    case LB_INSERTSTRING: {
        // A negative index appends, as on Windows.
        const size_t at = ((int)wp < 0) ? w->items.size()
                                        : std::min((size_t)wp, w->items.size());
        w->items.insert(w->items.begin() + at, lp ? (const char *)lp : "");
        w->itemSel.insert(w->itemSel.begin() + std::min(at, w->itemSel.size()), 0);
        if (at < w->itemData.size()) w->itemData.insert(w->itemData.begin() + at, 0);
        if (w->curSel >= (int)at) ++w->curSel;
        return (LRESULT)at;
    }

    // LB_DELETESTRING AND LB_FINDSTRING WERE BOTH ABSENT, and the pair of
    // them broke the Scenario Editor in two different ways. Neither reported
    // anything: an unhandled control message falls through to
    // DefWindowProc, which returns 0 -- a perfectly good index.
    //
    //     EditorTab_Vessel::SelectVessel
    //         idx = SendDlgItemMessage(.., LB_FINDSTRING, -1, name);
    //         if (idx == LB_ERR) { ...fall back to the focus object... }
    //         if (idx != LB_ERR) { LB_SETCURSEL idx; ed->hVessel = hV; }
    //
    // idx came back 0 and never LB_ERR, so the list always highlighted the
    // FIRST vessel while ed->hVessel held the right one -- the editor showing
    // one vessel and editing another.
    //
    //     EditorTab_Vessel::VesselDeleted
    //         idx = ... LB_FINDSTRING ...;  if (idx == LB_ERR) return;
    //         SendDlgItemMessage(.., LB_DELETESTRING, idx, 0);
    //
    // and the delete did nothing at all, so a destroyed vessel stayed in the
    // list. Selecting it afterwards hands GetVesselFromList a stale
    // OBJHANDLE.
    case LB_DELETESTRING: {
        if (wp >= w->items.size()) return LB_ERR;
        w->items.erase(w->items.begin() + wp);
        if (wp < w->itemSel.size())  w->itemSel.erase(w->itemSel.begin() + wp);
        if (wp < w->itemData.size()) w->itemData.erase(w->itemData.begin() + wp);

        // The selection is LOST when the selected item goes, and merely
        // shifts when an earlier one does. VesselDeleted depends on exactly
        // that distinction -- it tests LB_GETCURSEL for LB_ERR straight
        // afterwards to decide whether to re-select.
        if (w->curSel == (int)wp)      w->curSel = -1;
        else if (w->curSel > (int)wp)  --w->curSel;

        return (LRESULT)w->items.size();   // Win32 returns the remaining count
    }

    case LB_FINDSTRING:
    case LB_FINDSTRINGEXACT: {
        if (!lp) return LB_ERR;
        const char *needle = (const char *)lp;
        const int count = (int)w->items.size();
        if (count == 0) return LB_ERR;

        // Searches start AFTER the given index and wrap, so -1 means "from
        // the beginning". LB_FINDSTRING is a PREFIX match and
        // LB_FINDSTRINGEXACT a whole-string one; both are CASE-INSENSITIVE
        // on Windows, which matters here because oapiGetObjectName returns a
        // vessel's name in its configured case and the list was filled from
        // the same source but is not guaranteed to match byte for byte.
        const size_t nlen = strlen(needle);
        const int start = (((int)wp + 1) % count + count) % count;
        for (int n = 0; n < count; ++n) {
            const int i = (start + n) % count;
            const std::string &s = w->items[i];
            const bool hit = (msg == LB_FINDSTRINGEXACT)
                ? (s.size() == nlen && strcasecmp(s.c_str(), needle) == 0)
                : (s.size() >= nlen && strncasecmp(s.c_str(), needle, nlen) == 0);
            if (hit) return i;
        }
        return LB_ERR;
    }

    case LB_SETTABSTOPS:
        // Tab stops are presentation only. Accepted so the caller's success
        // check passes; the column positions have no meaning in an ImGui
        // list, which lays its own text out.
        return TRUE;

    case LB_GETITEMDATA:
    case CB_GETITEMDATA:
        return (wp < w->itemData.size()) ? w->itemData[wp] : (LRESULT)CB_ERR;

    case LB_SETITEMDATA:
    case CB_SETITEMDATA:
        if (wp >= w->items.size()) return CB_ERR;
        if (w->itemData.size() < w->items.size()) w->itemData.resize(w->items.size(), 0);
        w->itemData[wp] = (LONG_PTR)lp;
        return TRUE;

    case CB_SELECTSTRING: {
        // Find by prefix and select, returning the index. Same search rules
        // as CB_FINDSTRING; the ScnEditor uses it to restore a combo to a
        // named entry.
        if (!lp) return CB_ERR;
        const char *needle = (const char *)lp;
        const size_t nlen = strlen(needle);
        const int count = (int)w->items.size();
        if (count == 0) return CB_ERR;
        const int start = (((int)wp + 1) % count + count) % count;
        for (int n = 0; n < count; ++n) {
            const int i = (start + n) % count;
            const std::string &s = w->items[i];
            if (s.size() >= nlen && strncasecmp(s.c_str(), needle, nlen) == 0) {
                w->curSel = i;
                comboSyncText(w);
                return i;
            }
        }
        return CB_ERR;
    }

    // -- multi-selection list boxes ----------------------------------------
    case LB_SETSEL: {
        // wParam selects or deselects; lParam is the index, or -1 for all.
        const bool sel = wp != 0;
        if ((int)lp < 0) {
            for (auto &f : w->itemSel) f = sel ? 1 : 0;
            return 0;
        }
        if ((size_t)lp >= w->itemSel.size()) return LB_ERR;
        w->itemSel[lp] = sel ? 1 : 0;
        return 0;
    }

    case LB_GETSEL:
        if (wp >= w->itemSel.size()) return LB_ERR;
        return w->itemSel[wp] ? 1 : 0;

    case LB_GETSELCOUNT: {
        int n = 0;
        for (char f : w->itemSel) if (f) ++n;
        return n;
    }

    case LB_GETSELITEMS: {
        if (!lp) return LB_ERR;
        int *out = (int *)lp;
        int n = 0;
        for (size_t i = 0; i < w->itemSel.size() && n < (int)wp; ++i)
            if (w->itemSel[i]) out[n++] = (int)i;
        return n;
    }

    case LB_GETCOUNT:
        return (LRESULT)w->items.size();

    case LB_SETCURSEL:
        if ((int)wp < 0) { w->curSel = -1; return LB_ERR; }
        if (wp >= w->items.size()) return LB_ERR;
        w->curSel = (int)wp;
        return w->curSel;

    case LB_GETCURSEL:
        return w->curSel < 0 ? LB_ERR : w->curSel;

    case LB_GETTEXT: {
        if (wp >= w->items.size() || !lp) return LB_ERR;
        const std::string &s = w->items[wp];
        memcpy((char *)lp, s.c_str(), s.size() + 1);
        return (LRESULT)s.size();
    }

    case LB_GETTEXTLEN:
        if (wp >= w->items.size()) return LB_ERR;
        return (LRESULT)w->items[wp].size();

    // -- trackbar and up-down ----------------------------------------------
    //
    // Both are a bounded integer. The scrollbar state already models exactly
    // that, so it is reused rather than duplicated; neither control is ever a
    // scrolling window, so there is no conflict.
    case TBM_SETRANGE:
        w->scrollV.minPos = (int)(short)LOWORD(lp);
        w->scrollV.maxPos = (int)(short)HIWORD(lp);
        w->scrollV.pos = boundedPos(w->scrollV.pos,
                                    w->scrollV.minPos, w->scrollV.maxPos);
        return 0;

    case TBM_SETRANGEMIN:
        w->scrollV.minPos = (int)lp;
        w->scrollV.pos = std::max(w->scrollV.pos, w->scrollV.minPos);
        return 0;

    case TBM_SETRANGEMAX:
        w->scrollV.maxPos = (int)lp;
        w->scrollV.pos = std::min(w->scrollV.pos, w->scrollV.maxPos);
        return 0;

    case TBM_GETRANGEMIN: return w->scrollV.minPos;
    case TBM_GETRANGEMAX: return w->scrollV.maxPos;

    case TBM_SETPOS:
        // wParam is the redraw flag, lParam the position.
        w->scrollV.pos = boundedPos((int)lp,
                                    w->scrollV.minPos, w->scrollV.maxPos);
        return 0;

    case TBM_GETPOS:
        return w->scrollV.pos;

    case TBM_SETTICFREQ:
        // wParam is the frequency; lParam is unused. Kept rather than
        // discarded because a TBS_AUTOTICKS bar draws one tick per frequency
        // unit, and without it every such bar would tick every UNIT: 200 of
        // them on IDC_DBG_SPEED, which is a grey smear rather than a scale.
        w->ticFreq = ((int)wp > 0) ? (int)wp : 1;
        return 0;

    case TBM_SETPAGESIZE:
    case TBM_SETLINESIZE:
        // Keyboard step: the pages read the position rather than the
        // increment, and nothing here moves a trackbar by keyboard.
        return 0;

    case UDM_SETRANGE:
        // Note the order: for the 16-bit form the MAXIMUM is in the low word.
        w->scrollV.maxPos = (int)(short)LOWORD(lp);
        w->scrollV.minPos = (int)(short)HIWORD(lp);
        return 0;

    case UDM_SETRANGE32:
        w->scrollV.minPos = (int)wp;
        w->scrollV.maxPos = (int)lp;
        return 0;

    case UDM_GETRANGE:
        return MAKELONG(w->scrollV.maxPos, w->scrollV.minPos);

    case UDM_SETPOS:
        w->scrollV.pos = boundedPos((int)(short)LOWORD(lp),
                                    w->scrollV.minPos, w->scrollV.maxPos);
        return 0;

    case UDM_SETPOS32:
        w->scrollV.pos = boundedPos((int)lp,
                                    w->scrollV.minPos, w->scrollV.maxPos);
        return 0;

    case UDM_GETPOS:
    case UDM_GETPOS32:
        return w->scrollV.pos;

    // The PROGRESS BAR's messages are not here. They occupy the same numbers
    // as the trackbar's, so they are dispatched by class before this switch
    // is reached -- see progressBarMessage above.

    // -- tree view ---------------------------------------------------------
    case TVM_INSERTITEM: {
        auto *tvis = (LPTVINSERTSTRUCT)lp;
        if (!tvis) return 0;
        TreeData *td = w->treeData();
        g_activeTree = td;

        TreeItem *item = td->create();
        const TVITEM &src = tvis->item;

        // A tree declared with TVS_CHECKBOXES gives every new item state
        // image 1 -- an unchecked box. See Window::treeCheckboxes; without
        // this no module row has a check box and the Modules tab cannot be
        // used at all.
        if (w->treeCheckboxes)
            item->state = (item->state & ~TVIS_STATEIMAGEMASK) | (1u << 12);

        if (src.mask & TVIF_TEXT)  item->text = src.pszText ? src.pszText : "";
        if (src.mask & TVIF_IMAGE) item->image = src.iImage;
        if (src.mask & TVIF_SELECTEDIMAGE) item->selectedImage = src.iSelectedImage;
        if (src.mask & TVIF_CHILDREN) item->cChildren = src.cChildren;
        if (src.mask & TVIF_PARAM) item->lParam = src.lParam;
        if (src.mask & TVIF_STATE)
            item->state = (item->state & ~src.stateMask) |
                          (src.state & src.stateMask);

        // TVI_ROOT and a null parent both mean top level.
        TreeItem *parent = nullptr;
        if (tvis->hParent && tvis->hParent != TVI_ROOT)
            parent = (TreeItem *)tvis->hParent;
        item->parent = parent;

        std::vector<TreeItem *> &list = parent ? parent->children : td->roots;
        const HTREEITEM after = tvis->hInsertAfter;

        if (after == TVI_SORT) {
            // Alphabetical, CASE-INSENSITIVELY, which is what the Windows
            // tree view does and what ScanDirectory asks for on folders.
            //
            // A plain std::string comparison is byte-wise, so every uppercase
            // letter sorts before every lowercase one. That is invisible until
            // two siblings differ first at a letter of differing case, and the
            // Modules tab has exactly that pair:
            //
            //     "SDK sample plugins"        'D' = 0x44
            //     "Script tools and drivers"  'c' = 0x63
            //
            // Byte-wise puts SDK first; Windows puts Script first, because
            // "sc" < "sd". Same for the scenario folder list, where a name
            // beginning with a capital would otherwise jump the whole
            // lowercase block.
            auto at = std::lower_bound(
                list.begin(), list.end(), item,
                [](const TreeItem *a, const TreeItem *b) {
                    return strcasecmp(a->text.c_str(), b->text.c_str()) < 0;
                });
            list.insert(at, item);
        } else if (after == TVI_FIRST) {
            list.insert(list.begin(), item);
        } else if (after == TVI_LAST || !after) {
            list.push_back(item);
        } else {
            // Insert directly after a named sibling; ScanDirectory computes
            // one explicitly to keep files ordered ahead of subfolders.
            auto at = std::find(list.begin(), list.end(), (TreeItem *)after);
            if (at == list.end()) list.push_back(item);
            else                  list.insert(at + 1, item);
        }
        return (LRESULT)item;
    }

    case TVM_DELETEITEM: {
        TreeData *td = w->treeData();
        g_activeTree = td;

        // TVI_ROOT clears the whole control, which is how RefreshList starts.
        if (!lp || (HTREEITEM)lp == TVI_ROOT) { td->clear(); return TRUE; }

        // A specific handle removes that item and its descendants.
        // ExtraTab::UnregisterExtraParam uses this form when a plugin drops
        // its entry, so treating it as a no-op leaves a dead row behind
        // pointing at a deleted object.
        TreeItem *victim = (TreeItem *)lp;

        // Collect the subtree first; erasing while walking would invalidate
        // the iteration.
        std::vector<TreeItem *> doomed;
        std::vector<TreeItem *> stack{ victim };
        while (!stack.empty()) {
            TreeItem *cur = stack.back();
            stack.pop_back();
            doomed.push_back(cur);
            for (TreeItem *c : cur->children) stack.push_back(c);
        }

        // Unlink from the parent's child list, or from the root list.
        std::vector<TreeItem *> &siblings =
            victim->parent ? victim->parent->children : td->roots;
        siblings.erase(std::remove(siblings.begin(), siblings.end(), victim),
                       siblings.end());

        // The selection must not survive its item.
        for (TreeItem *d : doomed)
            if (td->selected == d) td->selected = nullptr;

        for (TreeItem *d : doomed) {
            for (auto it = td->storage.begin(); it != td->storage.end(); ++it) {
                if (it->get() == d) { td->storage.erase(it); break; }
            }
        }
        return TRUE;
    }

    case TVM_GETNEXTITEM: {
        TreeData *td = w->treeData();
        g_activeTree = td;
        TreeItem *it = (TreeItem *)lp;
        switch (wp) {
        case TVGN_ROOT:     return (LRESULT)(td->roots.empty() ? nullptr
                                                               : td->roots[0]);
        case TVGN_CARET:    return (LRESULT)td->selected;
        case TVGN_CHILD:
            if (!it) return (LRESULT)(td->roots.empty() ? nullptr : td->roots[0]);
            return (LRESULT)(it->children.empty() ? nullptr : it->children[0]);
        case TVGN_NEXT:     return (LRESULT)TreeData::nextSibling(it);
        case TVGN_PREVIOUS: return (LRESULT)TreeData::prevSibling(it);
        case TVGN_PARENT:   return (LRESULT)(it ? it->parent : nullptr);
        default:            return 0;
        }
    }

    case TVM_GETITEM: {
        auto *tvi = (LPTVITEM)lp;
        if (!tvi) return FALSE;
        TreeItem *it = (TreeItem *)tvi->hItem;
        if (!it) return FALSE;
        if (tvi->mask & TVIF_TEXT) {
            if (tvi->pszText && tvi->cchTextMax > 0) {
                snprintf(tvi->pszText, (size_t)tvi->cchTextMax, "%s",
                         it->text.c_str());
            }
        }
        if (tvi->mask & TVIF_IMAGE)    tvi->iImage = it->image;
        if (tvi->mask & TVIF_PARAM)    tvi->lParam = it->lParam;
        if (tvi->mask & TVIF_STATE)    tvi->state  = it->state;
        if (tvi->mask & TVIF_CHILDREN) tvi->cChildren = it->cChildren;
        return TRUE;
    }

    case TVM_SETITEM: {
        auto *tvi = (LPTVITEM)lp;
        if (!tvi) return FALSE;
        TreeItem *it = (TreeItem *)tvi->hItem;
        if (!it) return FALSE;
        if (tvi->mask & TVIF_TEXT && tvi->pszText) it->text = tvi->pszText;
        if (tvi->mask & TVIF_IMAGE) it->image = tvi->iImage;
        if (tvi->mask & TVIF_STATE)
            it->state = (it->state & ~tvi->stateMask) | (tvi->state & tvi->stateMask);
        return TRUE;
    }

    case TVM_SELECTITEM: {
        TreeData *td = w->treeData();
        g_activeTree = td;
        TreeItem *prev = td->selected;
        td->selected = (TreeItem *)lp;

        // Expand the ANCESTORS of the new selection so it is reachable, which
        // is what a tree view does when the caret is set programmatically.
        //
        // Starting the walk at the selection itself is wrong: selecting a
        // folder would then also expand it. A click selects before a
        // double-click toggles, so a collapse could never stick -- the click
        // that preceded it had already re-expanded the folder. The item's own
        // expanded state belongs to the user.
        if (td->selected)
            for (TreeItem *a = td->selected->parent; a; a = a->parent)
                a->expanded = true;

        if (wp == TVGN_CARET && td->selected != prev)
            orbiter_NotifyTree(toHwnd(w), TVN_SELCHANGED, td->selected, prev);
        return TRUE;
    }

    case TVM_SETIMAGELIST: {
        TreeData *td = w->treeData();
        HIMAGELIST prev = td->images;
        td->images = (HIMAGELIST)lp;
        return (LRESULT)prev;
    }

    case TVM_EXPAND: {
        TreeItem *it = (TreeItem *)lp;
        if (!it) return FALSE;
        if (wp == TVE_COLLAPSE)    it->expanded = false;
        else if (wp == TVE_EXPAND) it->expanded = true;
        else if (wp == TVE_TOGGLE) it->expanded = !it->expanded;
        return TRUE;
    }

    case TVM_GETCOUNT: {
        TreeData *td = w->treeData();
        return (LRESULT)td->storage.size();
    }

    default:
        break;
    }

    return dispatchToProc(w, msg, wp, lp);
}

LRESULT SendDlgItemMessageA(HWND dlg, int id, UINT msg, WPARAM wp, LPARAM lp)
{
    HWND ctrl = GetDlgItem(dlg, id);
    return ctrl ? SendMessageA(ctrl, msg, wp, lp) : 0;
}

BOOL PostMessageA(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    // Posted messages are queued rather than delivered, which is the whole
    // difference from SendMessage and matters where Orbiter posts to itself
    // from inside a handler.
    QueuedMsg q;
    q.msg.hwnd    = h;
    q.msg.message = msg;
    q.msg.wParam  = wp;
    q.msg.lParam  = lp;
    g_messageQueue.push_back(std::move(q));
    return TRUE;
}

BOOL PostThreadMessageA(DWORD, UINT msg, WPARAM wp, LPARAM lp)
{
    return PostMessageA(nullptr, msg, wp, lp);
}

void PostQuitMessage(int code)
{
    g_quitPosted = true;
    g_quitCode   = code;
}

LRESULT DefWindowProcA(HWND h, UINT msg, WPARAM, LPARAM)
{
    Window *w = toWindow(h);
    if (!w) return 0;
    // The only default behaviour this tree depends on is closing a window.
    if (msg == WM_CLOSE) { DestroyWindow(h); return 0; }
    return 0;
}

// ===========================================================================
// Text accessors
// ===========================================================================

BOOL SetWindowTextA(HWND h, LPCSTR text)
{
    Window *w = toWindow(h);
    if (!w) return FALSE;
    w->text = text ? text : "";

    // THE RENDER WINDOW'S CAPTION IS A REAL TITLE BAR, and on this platform it
    // belongs to the host window rather than to a shim Window.
    //
    // On Windows the Launchpad and the render window are two separate windows
    // with two separate captions; here they are one GLFW window used for both,
    // so a caption set on the render window has to reach it. Without this the
    // title stayed "OpenOrbiter Launchpad" for the whole of a running session
    // -- GraphicsClient::clbkCreateRenderWindow creates the render window with
    // an empty caption and the client names it immediately afterwards
    // (D3D9Client.cpp:430 sets "[D3D9Client]"), and neither of those calls
    // went anywhere.
    //
    // The class is the test, and it is exact: strWndClass
    // ("Orbiter Render Window", GraphicsAPI.cpp:43) is registered by
    // GraphicsClient::clbkInitialise for this one window and nothing else in
    // the tree uses it. Comparing against orbiter_GetRenderWindow() instead
    // would be wrong: that function CREATES the render window on first call,
    // so asking it here would conjure one the moment anything set any window's
    // text -- see the note at its definition.
    if (w->className == "Orbiter Render Window")
        orbiter_SetHostWindowTitle(w->text.c_str());

    return TRUE;
}

int GetWindowTextA(HWND h, LPSTR buf, int size)
{
    Window *w = toWindow(h);
    if (!w || !buf || size <= 0) return 0;
    const size_t n = std::min((size_t)size - 1, w->text.size());
    memcpy(buf, w->text.c_str(), n);
    buf[n] = '\0';
    return (int)n;
}

int GetWindowTextLengthA(HWND h)
{
    Window *w = toWindow(h);
    return w ? (int)w->text.size() : 0;
}

BOOL SetDlgItemTextA(HWND dlg, int id, LPCSTR text)
{
    HWND c = GetDlgItem(dlg, id);
    return c ? SetWindowTextA(c, text) : FALSE;
}

UINT GetDlgItemTextA(HWND dlg, int id, LPSTR buf, int size)
{
    HWND c = GetDlgItem(dlg, id);
    return c ? (UINT)GetWindowTextA(c, buf, size) : 0;
}

// ===========================================================================
// Visibility, geometry and focus
// ===========================================================================

BOOL ShowWindow(HWND h, int cmd)
{
    // The console window is not a dialog.
    //
    // ConsoleManager::ShowConsole reaches here with the handle
    // GetConsoleWindow returned, which is a sentinel rather than a Window
    // object. Falling through to toWindow() would find nothing and the call
    // would be silently dropped, which is why the session console never
    // appeared. Route it to the console implementation instead.
    extern char g_consoleWindowToken;
    if (h == (HWND)&g_consoleWindowToken) {
        orbiter_ShowConsoleWindow(cmd != SW_HIDE);
        return TRUE;
    }

    Window *w = toWindow(h);
    if (!w) return FALSE;
    const bool was = w->visible;
    w->visible = (cmd != SW_HIDE);
    TRACEMSG("ShowWindow id=%d class=%s %s -> %s", w->id,
             w->className.empty() ? "(dialog)" : w->className.c_str(),
             was ? "shown" : "hidden", w->visible ? "shown" : "hidden");
    return was ? TRUE : FALSE;
}

BOOL IsWindowVisible(HWND h)
{
    Window *w = toWindow(h);
    return (w && w->visible) ? TRUE : FALSE;
}

BOOL EnableWindow(HWND h, BOOL enable)
{
    Window *w = toWindow(h);
    if (!w) return FALSE;
    const bool was = w->enabled;
    w->enabled = enable != 0;

    // The WS_DISABLED style bit has to move with it. DlgCtrl's gauge decides
    // how to paint itself by reading
    //     GetWindowLongPtr(hWnd, GWL_STYLE) & WS_DISABLED
    // rather than by asking IsWindowEnabled, so leaving the style untouched
    // would have every gauge draw as enabled regardless.
    if (enable) w->style &= ~WS_DISABLED;
    else        w->style |=  WS_DISABLED;

    TRACEMSG("EnableWindow id=%d %s (was %s)", w->id,
             enable ? "enable" : "DISABLE", was ? "enabled" : "disabled");

    // A control that has just been disabled is told, so it can repaint.
    if (was != (enable != 0))
        SendMessageA(h, WM_ENABLE, (WPARAM)(enable != 0), 0);

    // Win32 returns non-zero if the window was *previously disabled*.
    return was ? FALSE : TRUE;
}

BOOL IsWindowEnabled(HWND h)
{
    Window *w = toWindow(h);
    return (w && w->enabled) ? TRUE : FALSE;
}

BOOL SetWindowPos(HWND h, HWND, int x, int y, int cx, int cy, UINT flags)
{
    Window *w = toWindow(h);
    if (!w) return FALSE;

    const int oldCx = w->cx, oldCy = w->cy;

    if (!(flags & SWP_NOMOVE)) { w->x = x; w->y = y; }
    if (!(flags & SWP_NOSIZE)) { w->cx = cx; w->cy = cy; }
    if (flags & SWP_SHOWWINDOW) w->visible = true;
    if (flags & SWP_HIDEWINDOW) w->visible = false;

    // A size change through SetWindowPos sends WM_SIZE, exactly as Win32 does.
    //
    // Leaving this out is why a Launchpad tab page never filled its frame.
    // LaunchpadDialog::Resize resizes the page container and then calls
    // LaunchpadTab::TabAreaResized, which does
    //
    //     SetWindowPos(hTab, NULL, 0, 0, w, h, ...)
    //
    // and nothing else -- it relies on the resulting WM_SIZE to reach
    // TabProc -> OnSize, which is where ModuleTab restretches its splitter,
    // tree and button row and ScenarioTab its panes. With no message the page
    // kept the geometry its template gave it, so the window grew and the
    // content did not, leaving a band of bare dialog face below the tab.
    //
    // Sent only on an actual change, and only when the caller did not ask for
    // NOSIZE, so the ordinary move-only calls in Resize stay quiet.
    if (!(flags & SWP_NOSIZE) && (w->cx != oldCx || w->cy != oldCy))
        SendMessageA(h, WM_SIZE, SIZE_RESTORED, MAKELPARAM(w->cx, w->cy));

    return TRUE;
}

BOOL MoveWindow(HWND h, int x, int y, int cx, int cy, BOOL)
{
    Window *w = toWindow(h);
    if (!w) return FALSE;
    w->x = x; w->y = y; w->cx = cx; w->cy = cy;
    return TRUE;
}

BOOL GetClientRect(HWND h, LPRECT r)
{
    Window *w = toWindow(h);
    if (!w || !r) return FALSE;
    // Origin-relative, in pixels -- the geometry is already stored that way.
    r->left = r->top = 0;
    r->right  = w->cx;
    r->bottom = w->cy;
    return TRUE;
}

BOOL GetWindowRect(HWND h, LPRECT r)
{
    Window *w = toWindow(h);
    if (!w || !r) return FALSE;
    // Screen coordinates: walk up the parent chain accumulating offsets,
    // since a control's stored position is relative to its parent.
    int ox = 0, oy = 0;
    for (Window *a = w; a; a = a->parent) { ox += a->x; oy += a->y; }
    r->left   = ox;
    r->top    = oy;
    r->right  = ox + w->cx;
    r->bottom = oy + w->cy;
    return TRUE;
}

BOOL ClientToScreen(HWND h, LPPOINT p)
{
    Window *w = toWindow(h);
    if (!w || !p) return FALSE;
    for (Window *a = w; a; a = a->parent) { p->x += a->x; p->y += a->y; }
    return TRUE;
}

BOOL ScreenToClient(HWND h, LPPOINT p)
{
    Window *w = toWindow(h);
    if (!w || !p) return FALSE;
    for (Window *a = w; a; a = a->parent) { p->x -= a->x; p->y -= a->y; }
    return TRUE;
}

// g_focus is defined near the top of the file, beside destroyWindowRecursive,
// which has to clear it when the focused window is destroyed.

// Shift state, refreshed once per frame by the UI host. GetKeyState reports
// it so dialog keyboard handling can reverse the tab direction.
bool g_shiftDown = false;
// Ctrl too, for OVP/VulkanClient's RenderWndProc: its picking and debug
// shortcuts test Shift AND Ctrl through GetAsyncKeyState. The UI host samples
// both in the same place, once per frame.
bool g_ctrlDown = false;

extern "C" void orbiter_SetShiftState(int down) { g_shiftDown = down != 0; }
extern "C" void orbiter_SetCtrlState (int down) { g_ctrlDown  = down != 0; }

SHORT GetKeyState(int vkey)
{
    // Shift and Ctrl are what this tree's dialog handling and the graphics
    // client consult. The high bit means "currently down", which is the bit
    // the callers test.
    if (vkey == VK_SHIFT)   return g_shiftDown ? (SHORT)0x8000 : (SHORT)0;
    if (vkey == VK_CONTROL) return g_ctrlDown  ? (SHORT)0x8000 : (SHORT)0;
    return 0;
}

// GetAsyncKeyState answers from the same sample; see windows.h on why the
// "now" versus "at this message" distinction does not survive here.
SHORT GetAsyncKeyState(int vkey)
{
    return GetKeyState(vkey);
}

HWND SetFocus(HWND h)
{
    Window *prev = g_focus;
    g_focus = toWindow(h);
    return prev ? toHwnd(prev) : nullptr;
}

HWND GetFocus(void) { return g_focus ? toHwnd(g_focus) : nullptr; }

BOOL IsIconic(HWND) { return FALSE; }

// ERASING DISCARDS WHAT THE CONTROL HAS RECORDED, and that is not a no-op any
// more now that GetDC hands out the control's persistent DC (see the note
// there). bErase means "paint the background over this window's pixels", and
// in this port a control's recorded command list IS its pixels: leaving them
// would draw the old picture under the new one for ever.
//
// ScnEditor's DrawVesselBmp is written to depend on it --
//
//     InvalidateRect (hImgWnd, NULL, TRUE);
//     UpdateWindow (hImgWnd);
//     if (hVesselBmp) { ... StretchBlt ... } else ShowWindow (hImgWnd, SW_HIDE);
//
// -- so each vessel selection replaces the previous preview instead of
// stacking on it, and selecting a type with no ImageBmp clears the panel.
//
// Only the erase clears. bErase FALSE means "keep the pixels, I am going to
// repaint over them", which is what leaving the commands alone does.
BOOL InvalidateRect(HWND h, const RECT *, BOOL erase)
{
    if (h && erase) orbiter_ClearPaintDC(h);
    return TRUE;
}
// See windows.h: there is no update region to validate, because the core
// repaints every frame from the recorded display list.
BOOL ValidateRect(HWND, const RECT *) { return TRUE; }
BOOL UpdateWindow(HWND)                       { return TRUE; }
BOOL RedrawWindow(HWND, const RECT *, void *, UINT) { return TRUE; }

// ===========================================================================
// Scrollbars
// ===========================================================================

namespace {
Window::ScrollState *scrollBar(HWND h, int bar)
{
    Window *w = toWindow(h);
    if (!w) return nullptr;
    // SB_CTL addresses a standalone scrollbar control, which stores its state
    // in the vertical slot; SB_HORZ/SB_VERT address a window's own bars.
    return (bar == SB_HORZ) ? &w->scrollH : &w->scrollV;
}
} // namespace

int SetScrollPos(HWND h, int bar, int pos, BOOL)
{
    auto *s = scrollBar(h, bar);
    if (!s) return 0;
    const int prev = s->pos;
    s->pos = boundedPos(pos, s->minPos, s->maxPos);
    return prev;
}

int GetScrollPos(HWND h, int bar)
{
    auto *s = scrollBar(h, bar);
    return s ? s->pos : 0;
}

BOOL SetScrollRange(HWND h, int bar, int minPos, int maxPos, BOOL)
{
    auto *s = scrollBar(h, bar);
    if (!s) return FALSE;
    s->minPos = minPos;
    s->maxPos = maxPos;
    s->pos    = boundedPos(s->pos, minPos, maxPos);
    return TRUE;
}

BOOL GetScrollRange(HWND h, int bar, int *minPos, int *maxPos)
{
    auto *s = scrollBar(h, bar);
    if (!s) return FALSE;
    if (minPos) *minPos = s->minPos;
    if (maxPos) *maxPos = s->maxPos;
    return TRUE;
}

int SetScrollInfo(HWND h, int bar, const SCROLLINFO *si, BOOL)
{
    auto *s = scrollBar(h, bar);
    if (!s || !si) return 0;
    if (si->fMask & SIF_RANGE) { s->minPos = si->nMin; s->maxPos = si->nMax; }
    if (si->fMask & SIF_PAGE)  { s->page = (int)si->nPage; }
    if (si->fMask & SIF_POS)   { s->pos = si->nPos; }
    // The thumb cannot travel into the last page's worth of range.
    const int travel = std::max(s->minPos, s->maxPos - std::max(0, s->page - 1));
    s->pos = boundedPos(s->pos, s->minPos, travel);
    return s->pos;
}

BOOL GetScrollInfo(HWND h, int bar, LPSCROLLINFO si)
{
    auto *s = scrollBar(h, bar);
    if (!s || !si) return FALSE;
    if (si->fMask & SIF_RANGE) { si->nMin = s->minPos; si->nMax = s->maxPos; }
    if (si->fMask & SIF_PAGE)  { si->nPage = (UINT)s->page; }
    if (si->fMask & SIF_POS)   { si->nPos = s->pos; }
    if (si->fMask & SIF_TRACKPOS) { si->nTrackPos = s->pos; }
    return TRUE;
}

BOOL ScrollWindow(HWND h, int dx, int dy, const RECT *, const RECT *)
{
    // Scrolls a window's client area, moving its child windows with it.
    //
    // OptionsPageContainer scrolls a whole options page this way: SetPageSize
    // and VScroll compute a delta and call ScrollWindow(hPage, 0, dy), then
    // read nothing back -- the child positions *are* the scroll state. So
    // accepting this and doing nothing, as it did before, silently pins every
    // over-long options page to its top with a scrollbar that moves nothing.
    Window *w = toWindow(h);
    if (!w) return FALSE;
    if (!dx && !dy) return TRUE;

    for (Window *c : w->children) {
        c->x += dx;
        c->y += dy;
    }
    return TRUE;
}

// ===========================================================================
// Timers
// ===========================================================================

UINT_PTR SetTimer(HWND h, UINT_PTR id, UINT intervalMs, TIMERPROC proc)
{
    Window *w = toWindow(h);
    for (Timer &t : g_timers) {
        if (t.owner == w && t.id == id) {   // resetting an existing timer
            t.intervalMs = intervalMs;
            t.proc       = proc;
            t.nextDue    = timeGetTime() + intervalMs;
            return id;
        }
    }
    g_timers.push_back(Timer{ w, id, intervalMs, proc,
                              timeGetTime() + intervalMs });
    return id;
}

BOOL KillTimer(HWND h, UINT_PTR id)
{
    Window *w = toWindow(h);
    const size_t before = g_timers.size();
    g_timers.erase(std::remove_if(g_timers.begin(), g_timers.end(),
                                  [&](const Timer &t) {
                                      return t.owner == w && t.id == id;
                                  }),
                   g_timers.end());
    return g_timers.size() != before ? TRUE : FALSE;
}

// ===========================================================================
// Message loop
// ===========================================================================

BOOL PeekMessageA(LPMSG msg, HWND, UINT, UINT, UINT flags)
{
    if (!msg) return FALSE;

    // Pump a frame. This is the hook that makes the UI run: Orbiter::Run
    // blocks here waiting for the user, and rendering from inside the message
    // loop is what Win32 effectively does anyway -- which is why Orbiter.cpp
    // needs no modification at all. Interaction during the frame posts
    // WM_COMMAND, so anything the user did is already in the queue by the
    // time this returns.
    //
    // The elapsed-time condition matters as much as the empty-queue one. A
    // held DlgCtrl gauge arrow sets a timer of 3000/(rmax-rmin) ms, which can
    // be about one message per frame; with only the empty-queue test the
    // queue would never drain to nothing and the display would freeze for as
    // long as the button was held.
    // NOT WHILE A SESSION IS RUNNING. Once a graphics client is driving,
    // clbkDisplayFrame -> PresentScene -> orbiter_EndSceneFrame pumps exactly
    // one frame per simulation step, and a second pump from here acquires a
    // second swapchain image without presenting the first -- which on a
    // two-image swapchain leaves nothing to acquire and makes presentFrame
    // present an index it does not own. The driver faults there.
    //
    // This is the Windows arrangement, not a departure from it: there, Present
    // is called once per frame from clbkDisplayFrame and the message loop only
    // dispatches. See orbiter_SessionOwnsFrames in UIHost.cpp.
    static DWORD lastPump = 0;
    const DWORD nowPump = timeGetTime();
    if (!g_quitPosted && !orbiter_SessionOwnsFrames() &&
        (g_messageQueue.empty() || (DWORD)(nowPump - lastPump) >= 8)) {
        lastPump = nowPump;
        orbiter_PumpFrame();
    }

    // Expired timers generate WM_TIMER, as they do on Windows.
    const DWORD now = timeGetTime();
    for (Timer &t : g_timers) {
        if ((int)(now - t.nextDue) >= 0) {
            t.nextDue = now + t.intervalMs;
            QueuedMsg q;
            q.msg.hwnd    = t.owner ? toHwnd(t.owner) : nullptr;
            q.msg.message = WM_TIMER;
            q.msg.wParam  = t.id;
            q.msg.lParam  = (LPARAM)t.proc;
            g_messageQueue.push_back(std::move(q));
        }
    }

    if (g_quitPosted && g_messageQueue.empty()) {
        memset(msg, 0, sizeof(*msg));
        msg->message = WM_QUIT;
        msg->wParam  = (WPARAM)g_quitCode;
        return TRUE;
    }

    if (g_messageQueue.empty()) return FALSE;

    QueuedMsg &q = g_messageQueue.front();
    *msg = q.msg;

    // Hand back a pointer into a buffer that outlives this call: the receiver
    // reads it during the DispatchMessage that follows.
    if (!q.payload.empty()) {
        g_currentPayload = q.payload;
        msg->lParam = (LPARAM)g_currentPayload.data();
    }

    if (flags & PM_REMOVE) g_messageQueue.erase(g_messageQueue.begin());
    return TRUE;
}

BOOL GetMessageA(LPMSG msg, HWND filterWnd, UINT filterMin, UINT filterMax)
{
    // GetMessage blocks until a message arrives, and returns FALSE only for
    // WM_QUIT -- which is what terminates Orbiter's main loop.
    for (;;) {
        if (PeekMessageA(msg, filterWnd, filterMin, filterMax, PM_REMOVE)) {
            return msg->message == WM_QUIT ? FALSE : TRUE;
        }
        Sleep(1);
    }
}

BOOL TranslateMessage(const MSG *) { return FALSE; }

LRESULT DispatchMessageA(const MSG *msg)
{
    if (!msg) return 0;
    if (msg->message != WM_TIMER)
        TRACEMSG("dispatch msg=0x%04X wp=%lu hwnd=%p", msg->message,
                 (unsigned long)msg->wParam, (void *)msg->hwnd);

    // A WM_TIMER carrying a TIMERPROC invokes it directly rather than going to
    // the window procedure, matching Win32.
    if (msg->message == WM_TIMER && msg->lParam) {
        ((TIMERPROC)msg->lParam)(msg->hwnd, WM_TIMER,
                                 (UINT_PTR)msg->wParam, timeGetTime());
        return 0;
    }

    Window *w = toWindow(msg->hwnd);
    if (!w) return 0;
    return dispatchToProc(w, msg->message, msg->wParam, msg->lParam);
}

BOOL IsDialogMessageA(HWND dlg, LPMSG msg)
{
    // Standard dialog keyboard handling. Launchpad::ConsumeMessage routes
    // every message through this before Orbiter's own loop sees it, and
    // returning FALSE unconditionally -- as this did -- removes Tab
    // navigation, Enter for the default button, Escape for cancel, and the
    // Alt+mnemonic accelerators the '&' in a caption declares.
    Window *w = toWindow(dlg);
    if (!w || !msg) return FALSE;
    if (msg->message != WM_KEYDOWN && msg->message != WM_CHAR &&
        msg->message != WM_SYSKEYDOWN)
        return FALSE;

    // Alt+letter: activate the control whose caption marks that letter with
    // '&'. "&Launch Orbiter" binds Alt+L, "E&xit" binds Alt+X.
    if (msg->message == WM_SYSKEYDOWN) {
        const char want = (char)toupper((int)msg->wParam);
        TRACEMSG("IsDialogMessage: WM_SYSKEYDOWN mnemonic '%c'", want);

        std::vector<Window *> all{ w };
        for (size_t i = 0; i < all.size(); ++i)
            for (Window *c : all[i]->children) all.push_back(c);

        for (Window *c : all) {
            if (!c->visible || !c->enabled) continue;
            const std::string &t = c->text;
            for (size_t i = 0; i + 1 < t.size(); ++i) {
                if (t[i] != '&') continue;
                if (t[i + 1] == '&') { ++i; continue; }   // literal ampersand
                if (toupper((unsigned char)t[i + 1]) == want) {
                    TRACEMSG("  mnemonic matched control id=%d \"%s\"",
                             c->id, t.c_str());
                    notifyParent(c, BN_CLICKED);
                    return TRUE;
                }
                break;   // only the first marker in a caption counts
            }
        }
        return FALSE;
    }

    // Collect the focusable controls in template order, which is the tab
    // order: WS_TABSTOP marks a stop, and a disabled or hidden control is
    // skipped, exactly as Windows does.
    std::vector<Window *> stops;
    // A PRE-ORDER WALK IN TEMPLATE ORDER, and the placement of the record is
    // the whole difficulty.
    //
    // The reverse iteration below is correct and necessary: children are
    // pushed back-to-front so that they POP front-to-first, which is what
    // makes an explicit stack produce a forward depth-first order. The bug
    // was recording INSIDE that reversed loop -- `stops.push_back(c)` next to
    // the `stack.push_back(c)` -- which appended in reverse and made the
    // whole tab order run backwards.
    //
    // MEASURED on the Launchpad before the fix, with the scripted driver:
    // three Tabs from a fresh dialog gave focus 1264 -> 1263 -> 1262, i.e.
    // About -> Extra -> Video, the tab strip bottom to top. The template
    // order is Scenarios 1259, Options 1260, Modules 1261, Video 1262,
    // Extra 1263, About 1264, so the first Tab landed on the LAST control.
    //
    // Recording at POP time instead puts each control in the list at the
    // moment it is visited, which is forward template order.
    std::vector<Window *> stack{ w };
    while (!stack.empty()) {
        Window *cur = stack.back();
        stack.pop_back();

        // The dialog itself is the walk's root, not a tab stop.
        if (cur != w && (cur->style & WS_TABSTOP) && cur->enabled)
            stops.push_back(cur);

        for (auto it = cur->children.rbegin(); it != cur->children.rend(); ++it) {
            Window *c = *it;
            if (!c->visible) continue;
            stack.push_back(c);
        }
    }

    switch (msg->wParam) {
    case VK_TAB: {
        if (stops.empty()) return TRUE;
        // Shift reverses direction, as it does on Windows.
        const bool back = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        int cur = -1;
        for (size_t i = 0; i < stops.size(); ++i)
            if (stops[i] == g_focus) { cur = (int)i; break; }
        const int n = (int)stops.size();
        const int next = (cur < 0) ? 0
                       : ((cur + (back ? -1 : 1)) % n + n) % n;
        g_focus = stops[next];
        return TRUE;
    }

    case VK_RETURN: {
        // Enter presses the default push button, which the template marks
        // with BS_DEFPUSHBUTTON.
        std::vector<Window *> all{ w };
        for (size_t i = 0; i < all.size(); ++i)
            for (Window *c : all[i]->children) all.push_back(c);
        for (Window *c : all) {
            if (c->visible && c->enabled &&
                (c->style & BS_DEFPUSHBUTTON) == BS_DEFPUSHBUTTON) {
                notifyParent(c, BN_CLICKED);
                return TRUE;
            }
        }
        return FALSE;
    }

    case VK_ESCAPE:
        // Escape is IDCANCEL. The Launchpad has no cancel button, so its
        // handler ignores this and the message falls through unconsumed.
        SendMessageA(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
        return TRUE;

    default:
        break;
    }
    return FALSE;
}

// ===========================================================================
// Window classes
// ===========================================================================

short RegisterClassA(const WNDCLASSA *wc)
{
    if (!wc || !wc->lpszClassName) return 0;
    g_windowClasses[wc->lpszClassName] = WindowClass{ wc->lpfnWndProc,
                                                      wc->cbWndExtra };
    return 1;
}

short RegisterClassExA(const WNDCLASSEXA *wc)
{
    if (!wc || !wc->lpszClassName) return 0;
    g_windowClasses[wc->lpszClassName] = WindowClass{ wc->lpfnWndProc,
                                                      wc->cbWndExtra };
    return 1;
}

BOOL UnregisterClassA(LPCSTR name, HINSTANCE)
{
    if (!name) return FALSE;
    return g_windowClasses.erase(name) ? TRUE : FALSE;
}

BOOL GetClassInfoA(HINSTANCE, LPCSTR name, void *out)
{
    // Orbiter.cpp calls this to fetch the stock dialog class ("#32770"), sets
    // an icon on the returned struct and re-registers it. Reporting failure
    // would leave that struct uninitialised and the re-registration would
    // install a null window procedure, so a minimal class is filled in.
    if (!name) return FALSE;

    if (out) {
        WNDCLASSA *wc = (WNDCLASSA *)out;
        memset(wc, 0, sizeof(*wc));
        wc->lpszClassName = name;
        auto it = g_windowClasses.find(name);
        if (it != g_windowClasses.end()) {
            wc->lpfnWndProc = it->second.proc;
            wc->cbWndExtra  = it->second.extra;
        }
    }
    return TRUE;
}

// ===========================================================================
// Cursor, menus and miscellany
// ===========================================================================

// Defined in UIHost.cpp, which owns the GLFW window.
extern "C" void orbiter_GetHostCursorPos(int *x, int *y);
extern "C" void orbiter_SetHostCursorPos(int x, int y);
extern "C" void orbiter_SetCursorVisible(int visible);
extern "C" void orbiter_SetCursorConfined(int confined);

// ===========================================================================
// THE ROTATION-MODE TRIO, which used to be three no-op stubs.
// ===========================================================================
//
// Orbiter::InitRotationMode calls all three when the right button goes down,
// and ExitRotationMode undoes them:
//
//     ShowCursor(FALSE);       hide it
//     SetCapture(hRenderWnd);  keep the messages coming
//     ClipCursor(&rScreen);    // "so we don't miss the button up event"
//
// Returning success and doing nothing meant that on this platform the pointer
// stayed visible during a camera drag and was free to leave the window, which
// is precisely the lost button-up the reference's own comment names -- and
// leaves the camera stuck in rotation mode when it happens.
// ===========================================================================

// Win32 keeps a DISPLAY COUNTER, not a boolean: the cursor is visible while it
// is >= 0, ShowCursor(TRUE) increments and ShowCursor(FALSE) decrements, and
// the new value is returned. Orbiter relies on exactly that -- it stores the
// result in g_iCursorShowCount and tests `== 0` / `< 0` to avoid hiding twice.
// A boolean here would make nested hides unbalanced.
int ShowCursor(BOOL show)
{
    static int count = 0;
    count += show ? 1 : -1;
    orbiter_SetCursorVisible(count >= 0);
    return count;
}

// Confinement. GLFW has no "confine but keep visible" mode, and nothing in
// this tree wants one: ClipCursor is only ever called next to ShowCursor
// (FALSE), and UIHost combines the two into GLFW_CURSOR_DISABLED.
BOOL ClipCursor(const RECT *r)
{
    orbiter_SetCursorConfined(r != nullptr);
    return TRUE;
}

// SetCapture's contract is "keep sending me mouse messages even when the
// pointer is outside my window", and it is met by two things that are already
// in place rather than by anything here:
//
//   * UIHost::postMouseMessages holds its own drag capture -- a button that
//     went down over the render window keeps routing there until it comes up,
//     whatever the pointer crosses on the way;
//   * the GLFW_CURSOR_DISABLED that ClipCursor above turns on means the
//     pointer cannot leave the window in the first place.
//
// There is no separate handle to track, so these report success honestly.
HWND    SetCapture(HWND)      { return nullptr; }
BOOL    ReleaseCapture(void)  { return TRUE; }
HWND    GetDesktopWindow(void)   { return nullptr; }

// Input from a graphics client that owns its own window.
//
// The client REGISTERS these rather than being discovered with dlsym: plugins
// are loaded with RTLD_LOCAL (see orb_LoadLibrary in Platform.cpp), so their
// symbols never enter the global scope and dlsym(RTLD_DEFAULT, ...) cannot
// find them -- it returns null silently, which is exactly how the cursor came
// back as (0,0) while the client had the right position all along.
extern "C" {
    int  (*g_clientCursorPos)(int *, int *) = nullptr;
    void (*g_clientSetCursorPos)(int, int)  = nullptr;
    const unsigned char *(*g_clientKeyState)() = nullptr;
    void (*g_clientCursorDelta)(int *, int *) = nullptr;

    void orbiter_RegisterClientInput(int (*cursorPos)(int *, int *),
                                     void (*setCursorPos)(int, int),
                                     const unsigned char *(*keyState)(),
                                     void (*cursorDelta)(int *, int *))
    {
        g_clientCursorPos    = cursorPos;
        g_clientSetCursorPos = setCursorPos;
        g_clientKeyState     = keyState;
        g_clientCursorDelta  = cursorDelta;
    }
}

BOOL GetCursorPos(LPPOINT p)
{
    if (!p) return FALSE;

    // A graphics client with its own window reports the cursor itself.
    //
    // This used to return (0,0) unconditionally, with a comment claiming the
    // frame pump filled it -- which never happened. Camera::UpdateMouse reads
    // this every frame and works entirely from the DELTA between successive
    // reads, so a constant zero means the mouse can never rotate the camera.
    //
    // Resolved weakly: the client is a plugin that may not be loaded, and the
    // Launchpad must work without it.
    if (g_clientCursorPos) {
        int x = 0, y = 0;
        if (g_clientCursorPos(&x, &y)) { p->x = x; p->y = y; return TRUE; }
    }

    // Otherwise the Launchpad's own window, whose cursor position the UI host
    // already tracks for ImGui. Declared there rather than pulling GLFW into
    // this file, which deals only in the dialog layer.
    {
        int x = 0, y = 0;
        orbiter_GetHostCursorPos(&x, &y);
        p->x = x;
        p->y = y;
    }
    return TRUE;
}

BOOL SetCursorPos(int x, int y)
{
    // Camera::UpdateMouse recentres the pointer while dragging so it cannot
    // leave the window. Forwarded to the client, which records it so the next
    // delta is measured from the warped position.
    if (g_clientSetCursorPos) { g_clientSetCursorPos(x, y); return TRUE; }

    // AND OTHERWISE THE HOST'S WINDOW, which is the case that was missing.
    //
    // GetCursorPos above already falls back to orbiter_GetHostCursorPos when
    // no client hook is registered; this had no such fallback, so with the
    // Vulkan client -- which shares the host's window and registers nothing --
    // the warp silently did nothing. Camera::UpdateMouse then measured dx from
    // the point the drag began rather than from the previous frame, and the
    // camera spun faster the further the pointer moved. The asymmetry was the
    // whole defect: one half of a get/set pair had a fallback and the other
    // did not.
    orbiter_SetHostCursorPos(x, y);
    return TRUE;
}
HCURSOR SetCursor(HCURSOR)  { return nullptr; }
// ShowCursor is implemented above, next to ClipCursor -- the two are one
// mechanism and reading them apart is what let both stay stubs.

HMENU GetSystemMenu(HWND, BOOL) { return nullptr; }
BOOL  DeleteMenu(HMENU, UINT, UINT) { return TRUE; }

int MessageBoxA(HWND, LPCSTR text, LPCSTR caption, UINT type)
{
    // A real modal. TabScenario uses this for save errors and for the
    // "File exists. Overwrite?" confirmation, and reads the answer -- writing
    // to stderr and returning IDOK would silently confirm a destructive
    // overwrite the user never saw.
    //
    // Like DialogBoxParam, this spins the message loop until the user
    // answers, which is what makes it modal without a second thread.
    //
    // WHICH ONLY WORKS IF SOMETHING ELSE IS STILL DRAWING FRAMES, and that is
    // the whole of the difference from Windows.
    //
    // On Windows the box is a window of its own with its own message loop
    // inside user32; it draws and answers itself no matter who called it or
    // from where. Here it is an ImGui window drawn by drawMessageBox() at the
    // tail of orbiter_PumpFrame, so it appears on screen only when a pump
    // runs, and the spin below is waiting for a button in a box that a pump
    // has to draw first. There are two situations where no pump can run:
    //
    //   A SESSION OWNS THE FRAMES. PeekMessageA above declines to pump while
    //   orbiter_SessionOwnsFrames() is set -- it must, or two frame sources
    //   would fight over the swapchain -- and the client's own pump is
    //   reached through clbkDisplayFrame on this very thread. Blocking here
    //   blocks it.
    //
    //   THE CALL CAME FROM INSIDE THE PUMP. A dialog's OnDraw or a window
    //   procedure runs inside orbiter_PumpFrame, so the thread that would
    //   draw the box is already here, one frame from the end of building it.
    //
    // Both were live. TerrainToolKit is reached from DlgFunction::OnDraw and
    // raises MB_OK from ToolKit::Initialize when gcGUI is disabled, which is
    // both of them at once, and the whole program stopped dead:
    //
    //   #4  Sleep                        windows.h:1371
    //   #5  MessageBoxA                  Win32Dlg.cpp
    //   #6  ToolKit::Initialize          ToolKit.cpp:342
    //   #7  DlgFunction::OnDraw          DlgFunction.cpp:23
    //   #10 orbiter_DrawImGuiDialogs     DlgMgr.cpp:798
    //   #11 orbiter_PumpFrame            UIHost.cpp
    //   #12 VulkanClient::PresentScene   VulkanClient.cpp:1697
    //
    // -- the box waiting for the frame, inside the frame.
    //
    // So when the loop cannot be spun the box is raised DEFERRED: it is drawn
    // by the next pump, dismissed by the user, and takes itself down. What
    // changes is only WHEN this call returns, and for MB_OK -- which has one
    // button and one possible answer, and whose return value ToolKit and
    // every other notification ignore -- that is the entire difference. A box
    // that asks a real question cannot be answered honestly without a frame,
    // so it answers IDCANCEL, the same reading a closed window gets below,
    // and says so in the log rather than inventing a yes.
    const bool canBlock = !orbiter_SessionOwnsFrames() && !orbiter_InFramePump();

    orbiter_ShowMessageBox(text ? text : "", caption ? caption : "Orbiter",
                           type, canBlock ? 0 : 1);

    if (!canBlock) {
        // The same partition drawMessageBox() uses to lay the buttons out:
        // anything that is not one of these three kinds is drawn with a lone
        // OK button, so IDOK is the only answer the box can ever give and
        // returning it now says nothing that waiting would not.
        const unsigned kind = type & 0x0F;
        const bool asksAQuestion = (kind == MB_YESNO || kind == MB_YESNOCANCEL ||
                                    kind == MB_OKCANCEL);
        if (!asksAQuestion) return IDOK;

        char msg[512];
        snprintf(msg, sizeof(msg),
                 "MessageBoxA: \"%s\" asks for an answer that cannot be waited "
                 "for here (session=%d, inPump=%d); returning IDCANCEL. The "
                 "box is still shown.",
                 text ? text : "", orbiter_SessionOwnsFrames(),
                 orbiter_InFramePump());
        oapiWriteLog(msg);
        return IDCANCEL;
    }

    while (orbiter_MessageBoxResult() == 0 && !g_quitPosted) {
        MSG msg;
        if (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        } else {
            Sleep(1);
        }
    }

    const int result = orbiter_MessageBoxResult();
    orbiter_ClearMessageBox();
    // A closed window while a box is up answers as cancel, which is the
    // safe reading for a confirmation prompt.
    return result ? result : IDCANCEL;
}

int GetSystemMetrics(int index)
{
    // Only the screen size is meaningful before the window exists; the frame
    // metrics are decoration sizes that ImGui does not use.
    switch (index) {
    case SM_CXSCREEN: return 1920;
    case SM_CYSCREEN: return 1080;
    case SM_CYCAPTION: return 24;
    default: return 0;
    }
}

BOOL TrackMouseEvent(LPTRACKMOUSEEVENT tme)
{
    // DECLARED SINCE THE SHIM WAS WRITTEN (windows.h:2135), NEVER DEFINED,
    // because nothing in the core arms mouse tracking. The graphics client is
    // the first caller -- D3D9Client.cpp:1794 for the render window and
    // WindowMgr.cpp:1597 for the gcGUI title bars, both with TME_LEAVE -- and
    // it linked as an undefined symbol until now.
    //
    // What the call means on Windows: "post me one WM_MOUSELEAVE when the
    // pointer next leaves hwndTrack". It is a one-shot subscription, and both
    // call sites re-arm it from their WM_MOUSEMOVE handler.
    //
    // This layer has no mouse-leave event to hang it on: Win32Dlg's message
    // pump synthesises WM_MOUSEMOVE, WM_?BUTTON* and WM_MOUSEWHEEL from what
    // UIHost reports, and there is no crossing notification behind it, so
    // nothing can post WM_MOUSELEAVE and no handler for it exists anywhere in
    // this tree (checked: no WM_MOUSELEAVE case in any Src/Orbiter/Linux .cpp).
    //
    // So the arming succeeds and the leave message never arrives, which is the
    // documented failure mode of not tracking: a control that highlights on
    // hover stays highlighted until the pointer moves over something else that
    // repaints it. Returning FALSE would be worse -- it means "could not arm",
    // and MSDN has callers treat that as an error path -- and there is nothing
    // to log about, because the client re-arms on every WM_MOUSEMOVE and this
    // would say the same thing hundreds of times a second.
    //
    // The place to make this real is UIHost's pointer reporting: when it
    // learns the pointer has left the window (or moved to a different HWND),
    // post WM_MOUSELEAVE to whichever hwndTrack is armed here. That needs a
    // per-window armed flag and nothing else.
    (void)tme;
    return TRUE;
}

// Declared here rather than included: UIHost.cpp has no header of its own and
// publishes its exports as extern "C" by design. See orbiter_GetWorkArea there.
extern "C" int orbiter_GetWorkArea(int *x, int *y, int *w, int *h);

BOOL SystemParametersInfoA(UINT action, UINT, void *param, UINT)
{
    // Font smoothing is saved and restored around fullscreen transitions on
    // Windows. There is nothing to change here, so the getter reports enabled
    // and the setter succeeds without doing anything.
    if (action == SPI_GETFONTSMOOTHING && param) {
        *(BOOL *)param = TRUE;
        return TRUE;
    }

    // SPI_GETWORKAREA, added for OVP/VulkanClient's WindowMgr, which clamps a
    // dragged floating panel to the desktop. glfwGetMonitorWorkarea is the
    // exact counterpart and the compositor is what decides that rectangle, so
    // nothing here has to guess where the panels are. It lives in UIHost.cpp
    // because that is the only translation unit that links GLFW.
    if (action == SPI_GETWORKAREA && param) {
        int x = 0, y = 0, w = 0, h = 0;
        if (!orbiter_GetWorkArea(&x, &y, &w, &h)) return FALSE;
        RECT *r = (RECT *)param;
        r->left   = (LONG)x;
        r->top    = (LONG)y;
        r->right  = (LONG)(x + w);
        r->bottom = (LONG)(y + h);
        return TRUE;
    }
    return TRUE;
}

// IntersectRect and SetParent, added with the above for WindowMgr: the first
// measures how far a dragged panel overlaps a dock, the second reparents a
// dialog into a sidebar when it is docked.

BOOL IntersectRect(LPRECT dst, const RECT *a, const RECT *b)
{
    if (!dst || !a || !b) return FALSE;

    // Win32 normalises nothing and neither does this: the intersection is the
    // greater of the two lefts to the lesser of the two rights, and an empty
    // result is zeroed and reported FALSE -- which is what callers test.
    dst->left   = a->left   > b->left   ? a->left   : b->left;
    dst->top    = a->top    > b->top    ? a->top    : b->top;
    dst->right  = a->right  < b->right  ? a->right  : b->right;
    dst->bottom = a->bottom < b->bottom ? a->bottom : b->bottom;

    if (dst->right <= dst->left || dst->bottom <= dst->top) {
        dst->left = dst->top = dst->right = dst->bottom = 0;
        return FALSE;
    }
    return TRUE;
}

HWND SetParent(HWND child, HWND newParent)
{
    Window *c = toWindow(child);
    if (!c) return nullptr;

    Window *old = c->parent;
    Window *p   = toWindow(newParent);

    if (old) {
        auto &sib = old->children;
        sib.erase(std::remove(sib.begin(), sib.end(), c), sib.end());
    }

    c->parent = p;
    if (p) p->children.push_back(c);

    // Win32 returns the PREVIOUS parent, and a NULL return means the window
    // had none -- not that the call failed.
    return old ? toHwnd(old) : nullptr;
}

int LoadStringA(HINSTANCE hModule, UINT id, LPSTR buf, int size)
{
    // Module string resources.
    //
    // TabVideo::ScanDir loads every candidate module and reads string 1001 to
    // decide whether it is a graphics engine; OnGraphicsClientLoaded reads
    // string 1000 for the description shown on the tab. ELF has no string
    // table, so the equivalent is an exported symbol, looked up by the same
    // id-to-name mapping the resource script uses:
    //
    //     1000 -> orbiterModuleDescription
    //     1001 -> orbiterModuleCategory
    //
    // A module declares them as
    //     extern "C" const char orbiterModuleCategory[] = "Graphics engines";
    // which is the direct counterpart of a STRINGTABLE entry.
    if (!buf || size <= 0) return 0;
    buf[0] = '\0';

    // THE MODULE'S WHOLE STRING TABLE, tried first.
    //
    // The two named symbols below were all this ever carried, so every other
    // id -- every id a module's own code asks for -- returned an empty string.
    // The Scenario Editor's Date tab builds both "Vessel state propagation"
    // combo boxes out of LoadString(hInst, IDS_PROP1+i) (Editor.cpp:1187 and
    // :1194), so both came up with four blank entries; its Docking tab
    // reports failures with IDS_ERR1..4 and reported nothing at all.
    //
    // rc2cpp.py --module now emits the .rc's STRINGTABLE as an exported
    // `orbiterModuleStringBlob`, and GetProcAddress on the HINSTANCE finds it.
    // That keeps the lookup PER MODULE, which it must be: these ids start at 1
    // and collide across modules immediately, so the ordered global registry
    // used for dialogs and bitmaps could not serve them.
    //
    // A FLAT BLOB, "id\0text\0id\0text\0...\0", and the reason is in
    // ResourceTemplates.h: ModuleTab::RefreshLists opens every candidate
    // module LOAD_LIBRARY_AS_DATAFILE, and Platform.cpp serves those symbols
    // out of a read-only mapping that was never relocated -- so a `const
    // char *` stored inside the image still holds its link-time address. The
    // first version of this was an array of {int, const char*} and it
    // segfaulted in strlen on the Launchpad's Modules tab, id 1000, before
    // the window appeared. There is nothing to relocate in a blob.
    if (hModule) {
        const char *p = (const char *)
            GetProcAddress((HMODULE)hModule, "orbiterModuleStringBlob");
        if (p) {
            while (*p) {
                const char *idStr = p;  p += strlen(p) + 1;
                const char *text  = p;  p += strlen(p) + 1;
                if (atoi(idStr) != (int)id) continue;
                const size_t n = std::min((size_t)size - 1, strlen(text));
                memcpy(buf, text, n);
                buf[n] = '\0';
                return (int)n;
            }
        }
    }

    const char *symbol = nullptr;
    switch (id) {
    case 1000: symbol = "orbiterModuleDescription"; break;
    case 1001: symbol = "orbiterModuleCategory";    break;
    default:   return 0;
    }

    const char *value = (const char *)GetProcAddress((HMODULE)hModule, symbol);
    if (getenv("ORBITER_TRACE_MODULES"))
        fprintf(stderr, "[str] LoadString(mod=%p, %u/%s) -> %s\n",
                (void *)hModule, id, symbol, value ? value : "(null)");
    if (!value) return 0;

    snprintf(buf, (size_t)size, "%s", value);
    return (int)strnlen(buf, (size_t)size);
}

// ===========================================================================
// Accessors for the UI host
//
// UIHost.cpp draws the dialog tree but must not see the Window type, which is
// private to this file. These give it exactly what it needs to lay out and
// interact with a control, and nothing more.
// ===========================================================================

extern "C" {

int orbiter_EnumTopLevelDialogs(HWND *out, int max)
{
    // Creation order, oldest first: the Launchpad is created before any modal,
    // so drawing in this order puts later dialogs on top.
    int n = 0;
    for (Window *w : g_topLevelOrder) {
        if (!w->isDialog) continue;
        if (out && n < max) out[n] = toHwnd(w);
        ++n;
        if (n >= max) break;
    }
    return n;
}

// The dialog that should receive keyboard input: the most recently created
// visible top-level one. That is the modal while a modal is up, and the
// Launchpad otherwise.
HWND orbiter_GetParentWnd(HWND h)
{
    Window *w = toWindow(h);
    return (w && w->parent) ? toHwnd(w->parent) : nullptr;
}

HWND orbiter_GetOwner(HWND h)
{
    Window *w = toWindow(h);
    return (w && w->owner) ? toHwnd(w->owner) : nullptr;
}

HWND orbiter_ActiveDialog(void)
{
    for (auto it = g_topLevelOrder.rbegin(); it != g_topLevelOrder.rend(); ++it) {
        Window *w = *it;
        if (w->isDialog && w->visible) return toHwnd(w);
    }
    return g_topLevelOrder.empty() ? nullptr : toHwnd(g_topLevelOrder.back());
}

int orbiter_EnumChildren(HWND parent, HWND *out, int max)
{
    Window *w = toWindow(parent);
    if (!w) return 0;
    int n = 0;
    for (Window *c : w->children) {
        if (out && n < max) out[n] = toHwnd(c);
        ++n;
        if (n >= max) break;
    }
    return n;
}

void orbiter_GetControlInfo(HWND h, const char **cls, const char **text,
                            int *id, int *x, int *y, int *cx, int *cy,
                            unsigned *style, int *visible, int *enabled)
{
    Window *w = toWindow(h);
    if (!w) {
        if (cls) *cls = nullptr;
        if (text) *text = nullptr;
        if (visible) *visible = 0;
        return;
    }
    if (cls)     *cls     = w->className.c_str();
    if (text)    *text    = w->text.c_str();
    if (id)      *id      = w->id;
    if (x)       *x       = w->x;
    if (y)       *y       = w->y;
    if (cx)      *cx      = w->cx;
    if (cy)      *cy      = w->cy;
    if (style)   *style   = (unsigned)w->style;
    if (visible) *visible = w->visible ? 1 : 0;
    if (enabled) *enabled = w->enabled ? 1 : 0;
}

// How many modal dialogs are stacked. Zero means the Launchpad is on top.
// Used by the scripted UI driver to assert that a modal actually opened and
// that Escape actually closed it -- see Linux/UiDriver.cpp.
int orbiter_ModalDepth(void)
{
    return (int)g_modalStack.size();
}

// ===========================================================================
// THE LIST-BOX SELF-TEST
// ===========================================================================
//
// LB_FINDSTRING and LB_DELETESTRING were both ABSENT from SendMessageA, and
// nothing said so: an unhandled control message falls through to
// DefWindowProc, which returns 0 -- a perfectly plausible index. The Scenario
// Editor is the live caller and the failure was invisible in two different
// ways at once (the wrong row highlighted; a deleted vessel left in the list).
//
// Reaching the Scenario Editor from the scripted UI driver means opening a
// session, then a custom-command dialog, then a tab -- a long path whose own
// failure modes would obscure this one. So the CONTRACT is asserted directly
// instead, in the exact sequence EditorTab_Vessel performs:
//
//     SelectVessel:  idx = LB_FINDSTRING(-1, name); if (idx == LB_ERR) ...
//     VesselDeleted: idx = LB_FINDSTRING(-1, name); if (idx == LB_ERR) return;
//                    LB_DELETESTRING(idx);
//                    if (LB_GETCURSEL() == LB_ERR) -> re-select
//
// That last test is the subtle one: the caller distinguishes "the selection
// was destroyed" from "the selection merely shifted", so the two cases cannot
// be collapsed.
//
// Runs once at startup and says PASSED out loud -- a check that stops running
// looks exactly like a check that passes, and this port has been bitten by
// that before.

// The failing line names its own test, because two self-tests share this
// helper: a "ListBox self-test:" prefix on a progress-bar failure sends the
// next reader to the wrong function.
static const char *g_selfTestName = "ListBox";

static bool lbCheck(const char *what, long got, long want, int &bad)
{
    if (got == want) return true;
    ++bad;
    char msg[224];
    snprintf(msg, sizeof(msg), "%s self-test: %s returned %ld, expected %ld",
             g_selfTestName, what, got, want);
    oapiWriteLog(msg);
    return false;
}

void orbiter_ListBoxSelfTest(void)
{
    int bad = 0;

    Window *lb = createWindow();
    lb->className = "ListBox";
    HWND h = toHwnd(lb);

    lbCheck("LB_ADDSTRING #0", (long)SendMessageA(h, LB_ADDSTRING, 0, (LPARAM)"Atlantis"),   0, bad);
    lbCheck("LB_ADDSTRING #1", (long)SendMessageA(h, LB_ADDSTRING, 0, (LPARAM)"GL-01"),      1, bad);
    lbCheck("LB_ADDSTRING #2", (long)SendMessageA(h, LB_ADDSTRING, 0, (LPARAM)"ISS"),        2, bad);
    lbCheck("LB_GETCOUNT",     (long)SendMessageA(h, LB_GETCOUNT, 0, 0),                     3, bad);

    // A search from -1 starts at the beginning. Prefix match, case-insensitive
    // -- both are Windows behaviour and the second matters, because the caller
    // gets its needle from oapiGetObjectName rather than from the list.
    lbCheck("LB_FINDSTRING exact-case",  (long)SendMessageA(h, LB_FINDSTRING, (WPARAM)-1, (LPARAM)"GL-01"), 1, bad);
    lbCheck("LB_FINDSTRING other-case",  (long)SendMessageA(h, LB_FINDSTRING, (WPARAM)-1, (LPARAM)"gl-01"), 1, bad);
    lbCheck("LB_FINDSTRING prefix",      (long)SendMessageA(h, LB_FINDSTRING, (WPARAM)-1, (LPARAM)"GL"),    1, bad);
    lbCheck("LB_FINDSTRING absent",      (long)SendMessageA(h, LB_FINDSTRING, (WPARAM)-1, (LPARAM)"Zzz"),   LB_ERR, bad);

    // THE DEFECT IN ONE LINE: before the fix this fell through to
    // DefWindowProc and returned 0, which the caller reads as "found at
    // index 0" and never as LB_ERR.
    lbCheck("LB_FINDSTRINGEXACT prefix-only",
            (long)SendMessageA(h, LB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)"GL"), LB_ERR, bad);

    // Delete the SELECTED item: the selection must be lost, not shifted.
    SendMessageA(h, LB_SETCURSEL, 1, 0);
    lbCheck("LB_GETCURSEL before delete", (long)SendMessageA(h, LB_GETCURSEL, 0, 0), 1, bad);
    lbCheck("LB_DELETESTRING count",      (long)SendMessageA(h, LB_DELETESTRING, 1, 0), 2, bad);
    lbCheck("LB_GETCURSEL after deleting the selection",
            (long)SendMessageA(h, LB_GETCURSEL, 0, 0), LB_ERR, bad);
    lbCheck("the deleted item is gone",
            (long)SendMessageA(h, LB_FINDSTRING, (WPARAM)-1, (LPARAM)"GL-01"), LB_ERR, bad);
    lbCheck("the survivor moved up",
            (long)SendMessageA(h, LB_FINDSTRING, (WPARAM)-1, (LPARAM)"ISS"), 1, bad);

    // Delete an EARLIER item: the selection shifts down rather than clearing.
    SendMessageA(h, LB_SETCURSEL, 1, 0);
    SendMessageA(h, LB_DELETESTRING, 0, 0);
    lbCheck("LB_GETCURSEL after deleting an earlier item",
            (long)SendMessageA(h, LB_GETCURSEL, 0, 0), 0, bad);

    // Per-item data, which CB_/LB_SETITEMDATA attach and the D3D9 VideoTab
    // uses to carry a display mode's dimensions.
    SendMessageA(h, LB_SETITEMDATA, 0, (LPARAM)0xABCD);
    lbCheck("LB_GETITEMDATA", (long)SendMessageA(h, LB_GETITEMDATA, 0, 0), 0xABCD, bad);

    destroyWindowRecursive(lb);

    if (!bad)
        oapiWriteLog((char *)"ListBox self-test PASSED: FINDSTRING prefix and "
                             "case rules, FINDSTRINGEXACT, DELETESTRING's "
                             "selection-lost vs selection-shifted rule, and "
                             "per-item data");
    else {
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "ListBox self-test FAILED with %d bad values -- the Scenario "
                 "Editor's vessel list depends on every one of them", bad);
        oapiWriteLog(msg);
    }
}

// ===========================================================================
// THE WM_USER COLLISION SELF-TEST
// ===========================================================================
//
// The compiler caught the collision once, as six "duplicate case value"
// errors, and that was luck: it only fired because the trackbar cases already
// existed. Nothing stops a future control class from being added with its own
// WM_USER numbers and quietly winning the argument -- and the failure mode is
// not a crash but a bar that never moves.
//
// So the property is asserted directly: the SAME NUMBER sent to two different
// classes must produce two different answers. Sending PBM_SETPOS (0x0402) to
// a progress bar must move it; sending the identical 0x0402 to a trackbar
// must be read as TBM_GETRANGEMAX and move nothing.
//
// Both directions matter. A dispatch that routed everything to the progress
// bar would pass a progress-only test and break every slider on the Video
// tab.

void orbiter_ProgressBarSelfTest(void)
{
    int bad = 0;
    g_selfTestName = "ProgressBar";

    Window *pb = createWindow();
    pb->className = "msctls_progress32";
    HWND hp = toHwnd(pb);

    Window *tb = createWindow();
    tb->className = "msctls_trackbar32";
    HWND ht = toHwnd(tb);

    // --- the progress bar reads the band its own way -----------------------
    SendMessageA(hp, PBM_SETRANGE, 0, MAKELPARAM(0, 1000));   // Launchpad.cpp:445
    lbCheck("PBM_GETRANGE max", (long)SendMessageA(hp, PBM_GETRANGE, 0, 0), 1000, bad);
    lbCheck("PBM_GETRANGE min", (long)SendMessageA(hp, PBM_GETRANGE, 1, 0),    0, bad);

    SendMessageA(hp, PBM_SETPOS, 250, 0);                     // Launchpad.cpp:509
    lbCheck("PBM_SETPOS then PBM_GETPOS",
            (long)SendMessageA(hp, PBM_GETPOS, 0, 0), 250, bad);

    // SETPOS returns the PREVIOUS position, which is how a caller can tell
    // whether anything changed without reading back.
    lbCheck("PBM_SETPOS returns the previous position",
            (long)SendMessageA(hp, PBM_SETPOS, 600, 0), 250, bad);

    lbCheck("PBM_DELTAPOS",
            (long)SendMessageA(hp, PBM_DELTAPOS, 100, 0), 600, bad);
    lbCheck("PBM_DELTAPOS moved the bar",
            (long)SendMessageA(hp, PBM_GETPOS, 0, 0), 700, bad);

    // Out of range clamps rather than wrapping.
    SendMessageA(hp, PBM_SETPOS, 99999, 0);
    lbCheck("PBM_SETPOS clamps at the maximum",
            (long)SendMessageA(hp, PBM_GETPOS, 0, 0), 1000, bad);

    // ...but STEPIT wraps, which is the one place the two differ.
    SendMessageA(hp, PBM_SETSTEP, 10, 0);
    SendMessageA(hp, PBM_SETPOS, 995, 0);
    SendMessageA(hp, PBM_STEPIT, 0, 0);
    lbCheck("PBM_STEPIT wraps at the top rather than clamping",
            (long)SendMessageA(hp, PBM_GETPOS, 0, 0), 0, bad);

    // --- THE COLLISION -----------------------------------------------------
    //
    // Give the trackbar a range and a position through ITS messages, then
    // send it the byte-identical numbers the progress bar just used.
    SendMessageA(ht, TBM_SETRANGE, 1, MAKELONG(0, 100));
    SendMessageA(ht, TBM_SETPOS,   1, 42);
    lbCheck("the trackbar's own TBM_GETPOS",
            (long)SendMessageA(ht, TBM_GETPOS, 0, 0), 42, bad);

    // 0x0402 is PBM_SETPOS *and* TBM_GETRANGEMAX. On a trackbar it must be
    // the read: it returns the maximum and leaves the position alone. If the
    // dispatch ever routes by number instead of class, this line moves the
    // trackbar to 900 and the next one catches it.
    lbCheck("0x0402 on a trackbar is TBM_GETRANGEMAX, not PBM_SETPOS",
            (long)SendMessageA(ht, TBM_GETRANGEMAX, 900, 0), 100, bad);
    lbCheck("...and it did NOT move the trackbar",
            (long)SendMessageA(ht, TBM_GETPOS, 0, 0), 42, bad);

    // The mirror image: 0x0406 is TBM_SETRANGE and PBM_SETRANGE32. On the
    // progress bar it must be the 32-bit range set, taking wParam/lParam as
    // the two limits rather than a packed LONG.
    SendMessageA(hp, PBM_SETRANGE32, 5, 55);
    lbCheck("0x0406 on a progress bar is PBM_SETRANGE32 (min)",
            (long)SendMessageA(hp, PBM_GETRANGE, 1, 0), 5, bad);
    lbCheck("0x0406 on a progress bar is PBM_SETRANGE32 (max)",
            (long)SendMessageA(hp, PBM_GETRANGE, 0, 0), 55, bad);

    // The trackbar is untouched by all of it -- the two controls share the
    // numbers but not the state.
    lbCheck("the trackbar's range survived the progress bar's traffic",
            (long)SendMessageA(ht, TBM_GETRANGEMAX, 0, 0), 100, bad);

    // --- WHAT ACTUALLY DRIVES THE BAR --------------------------------------
    //
    // The message layer above is only half of it. The Launchpad's wait-page
    // bar is driven by LaunchpadDialog::UpdateWaitProgress, whose position is
    //
    //     (mem0 - memstat->HeapUsage()) / mem_wait
    //
    // and whose visibility is `mem_wait ? SW_SHOW : SW_HIDE`, where mem_wait
    // is Orbiter::simheapsize/1000. Every one of those numbers comes from
    // MemStat::HeapUsage, which does NOT call GetProcessMemoryInfo directly:
    // it resolves it at runtime,
    //
    //     hLib = LoadLibrary("Psapi.dll");
    //     pGetProcessMemoryInfo = GetProcAddress(hLib, "GetProcessMemoryInfo");
    //
    // and returns a flat 0 if either step fails. A zero there is silent and
    // total: simheapsize becomes 0, mem_wait becomes 0, the bar is hidden and
    // never updated -- which looks exactly like a renderer that does not work.
    //
    // So the resolution chain is asserted here, by the same three calls
    // Memstat.cpp makes, rather than reasoned about.
    {
        HMODULE hLib = LoadLibraryA("Psapi.dll");
        lbCheck("LoadLibrary(\"Psapi.dll\") is non-null", hLib ? 1 : 0, 1, bad);

        typedef BOOL (*PFN)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
        PFN pfn = (PFN)GetProcAddress(hLib, "GetProcessMemoryInfo");
        lbCheck("GetProcAddress(\"GetProcessMemoryInfo\") resolves",
                pfn ? 1 : 0, 1, bad);

        if (pfn) {
            PROCESS_MEMORY_COUNTERS pmc;
            memset(&pmc, 0, sizeof(pmc));
            const BOOL ok = pfn(GetCurrentProcess(), &pmc, sizeof(pmc));
            lbCheck("GetProcessMemoryInfo succeeds", ok ? 1 : 0, 1, bad);
            // Any live process has a resident set of megabytes. The test is
            // "not zero", because zero is the failure the whole chain
            // collapses to, and it is the only value that matters.
            lbCheck("WorkingSetSize is non-zero",
                    pmc.WorkingSetSize > 0 ? 1 : 0, 1, bad);
        }
    }

    // Class matching is case-insensitive, as Win32's is.
    Window *pb2 = createWindow();
    pb2->className = "MSCTLS_Progress32";
    HWND hp2 = toHwnd(pb2);
    SendMessageA(hp2, PBM_SETRANGE, 0, MAKELPARAM(0, 10));
    SendMessageA(hp2, PBM_SETPOS, 7, 0);
    lbCheck("the class compare is case-insensitive",
            (long)SendMessageA(hp2, PBM_GETPOS, 0, 0), 7, bad);

    destroyWindowRecursive(pb);
    destroyWindowRecursive(tb);
    destroyWindowRecursive(pb2);

    if (!bad)
        oapiWriteLog((char *)"ProgressBar self-test PASSED: the WM_USER band is "
                             "dispatched by class -- 0x0402 sets a progress "
                             "bar's position and reads a trackbar's maximum, "
                             "and neither control's state leaks into the other");
    else {
        char msg[224];
        snprintf(msg, sizeof(msg),
                 "ProgressBar self-test FAILED with %d bad values -- the "
                 "Launchpad's load progress and every Video-tab slider read "
                 "the same message numbers", bad);
        oapiWriteLog(msg);
    }
}

int orbiter_IsDialog(HWND h)
{
    Window *w = toWindow(h);
    return (w && w->isDialog) ? 1 : 0;
}

void orbiter_NotifyCommand(HWND ctrl, unsigned short notifyCode)
{
    notifyParent(toWindow(ctrl), notifyCode);
}

int orbiter_GetCheckState(HWND h)
{
    Window *w = toWindow(h);
    return w ? w->checkState : BST_UNCHECKED;
}

void orbiter_SetCheckState(HWND h, int state)
{
    if (Window *w = toWindow(h)) w->checkState = state;
}

// ===========================================================================
// THE AUTO-RADIO GROUP
// ===========================================================================
//
// Checking an auto-radio button is only HALF of what BS_AUTORADIOBUTTON means.
// MSDN: "the system automatically sets the button's check state to checked and
// automatically sets the check state for all other buttons in the same group
// to cleared." The port did the first half and not the second, so clicking
// "exponential" checked it and left "linear" checked too -- BOTH radio buttons
// lit at once, on every one of the 47 auto-radio buttons in the templates.
//
// AND THE CORE DEPENDS ON THE OTHER HALF ENTIRELY. OptionsPages.cpp's handler
// for the pair is
//
//     case IDC_OPT_CSP_STARMAPLIN:
//     case IDC_OPT_CSP_STARMAPEXP:
//         if (code == BN_CLICKED) {
//             Cfg()->CfgVisualPrm.StarPrm.map_log = (id == IDC_OPT_CSP_STARMAPEXP);
//
// -- it records the value from the id and never touches either button's check
// state. Nothing anywhere would have cleared the old one.
//
// THE GROUP IS A SPAN, NOT A PARENT. Windows delimits it by WS_GROUP in
// TEMPLATE ORDER: a group begins at a control carrying WS_GROUP and runs until
// the next one that does. The celestial-sphere page is exactly that shape --
//
//     IDC_OPT_CSP_ENABLESTARPIX   BS_AUTOCHECKBOX | WS_GROUP   <- group starts
//     ... statics, edits, up-downs ...
//     IDC_OPT_CSP_STARMAPLIN      BS_AUTORADIOBUTTON
//     IDC_OPT_CSP_STARMAPEXP      BS_AUTORADIOBUTTON
//     IDC_OPT_CSP_ENABLESTARMAP   BS_AUTOCHECKBOX | WS_GROUP   <- and ends
//
// -- so neither radio carries WS_GROUP itself and the span has to be found by
// scanning outwards. Note also that rc.exe does NOT add WS_GROUP the way it
// adds WS_CHILD | WS_VISIBLE, so the bit in the template is the whole truth.
//
// ONLY AUTO-RADIO SIBLINGS ARE CLEARED. The checkbox at the head of that group
// must not be touched, and Windows does not touch it: USER32 clears a sibling
// only when its type is BS_AUTORADIOBUTTON. Clearing "every button in the
// span" would untick "Background stars: show as pixels" whenever the user
// picked a magnitude mapping.
void orbiter_CheckRadioButton(HWND h)
{
    Window *w = toWindow(h);
    if (!w) return;

    w->checkState = BST_CHECKED;

    Window *parent = w->parent;
    if (!parent) return;

    const std::vector<Window *> &sib = parent->children;

    // Where this control sits in template order.
    size_t self = sib.size();
    for (size_t i = 0; i < sib.size(); ++i)
        if (sib[i] == w) { self = i; break; }
    if (self == sib.size()) return;

    // Backwards to the start of the group: the nearest control at or before
    // this one carrying WS_GROUP. If none does, the group is the whole dialog,
    // which is what Windows does too.
    size_t first = 0;
    for (size_t i = self + 1; i-- > 0; )
        if (sib[i]->style & WS_GROUP) { first = i; break; }

    // Forwards to just before the next WS_GROUP.
    size_t last = sib.size() - 1;
    for (size_t i = self + 1; i < sib.size(); ++i)
        if (sib[i]->style & WS_GROUP) { last = i - 1; break; }

    for (size_t i = first; i <= last; ++i) {
        if (sib[i] == w) continue;
        if ((sib[i]->style & 0x0F) != BS_AUTORADIOBUTTON) continue;
        sib[i]->checkState = BST_UNCHECKED;
    }
}

// ===========================================================================
// THE AUTO-RADIO GROUP SELF-TEST
// ===========================================================================
//
// Built as a copy of the page the defect was found on -- the celestial-sphere
// options page -- because its shape is the whole difficulty: the group is
// delimited by WS_GROUP on controls that are NOT the radios, it contains a
// checkbox that must survive, and there is a SECOND group after it whose radio
// must also survive.
//
// The third assertion is the one that discriminates. "Clear every other radio
// under the same parent" is the obvious wrong fix, it passes the first two
// checks, and on the real page it would reach across the WS_GROUP boundary and
// silently untick the next group's selection.
void orbiter_RadioGroupSelfTest(void)
{
    int bad = 0;
    g_selfTestName = "RadioGroup";

    Window *dlg = createWindow();
    dlg->isDialog = true;

    auto add = [&](int id, DWORD style, int check) {
        Window *c = createWindow();
        c->id         = id;
        c->className  = "Button";
        c->style      = style;
        c->checkState = check;
        c->parent     = dlg;
        dlg->children.push_back(c);
        return c;
    };

    //  IDC_OPT_CSP_ENABLESTARPIX   BS_AUTOCHECKBOX | WS_GROUP   <- group 1
    //  (a static, standing for the labels and edits between them)
    //  IDC_OPT_CSP_STARMAPLIN      BS_AUTORADIOBUTTON
    //  IDC_OPT_CSP_STARMAPEXP      BS_AUTORADIOBUTTON
    //  IDC_OPT_CSP_ENABLESTARMAP   BS_AUTOCHECKBOX | WS_GROUP   <- group 2
    //  a radio in group 2
    Window *chk1 = add(100, BS_AUTOCHECKBOX | WS_GROUP, BST_CHECKED);
    Window *stat = add(101, 0,                          BST_UNCHECKED);
    Window *radA = add(102, BS_AUTORADIOBUTTON,         BST_CHECKED);
    Window *radB = add(103, BS_AUTORADIOBUTTON,         BST_UNCHECKED);
    Window *chk2 = add(104, BS_AUTOCHECKBOX | WS_GROUP, BST_CHECKED);
    Window *radC = add(105, BS_AUTORADIOBUTTON,         BST_CHECKED);
    (void)stat;

    // Click "exponential".
    orbiter_CheckRadioButton(toHwnd(radB));

    lbCheck("the clicked radio is checked",     radB->checkState, BST_CHECKED,   bad);
    lbCheck("THE SIBLING RADIO IS CLEARED",     radA->checkState, BST_UNCHECKED, bad);
    lbCheck("the checkbox at the group head survives",
            chk1->checkState, BST_CHECKED, bad);
    lbCheck("the NEXT group's checkbox survives",
            chk2->checkState, BST_CHECKED, bad);
    lbCheck("THE NEXT GROUP'S RADIO SURVIVES",
            radC->checkState, BST_CHECKED, bad);

    // And back again, so the test does not pass on a one-way implementation.
    orbiter_CheckRadioButton(toHwnd(radA));
    lbCheck("clicking back checks the first",   radA->checkState, BST_CHECKED,   bad);
    lbCheck("...and clears the second",         radB->checkState, BST_UNCHECKED, bad);

    // A radio in the SECOND group clears only within its own group, and that
    // group runs to the end of the dialog because no later control carries
    // WS_GROUP.
    orbiter_CheckRadioButton(toHwnd(radC));
    lbCheck("group 2 does not reach back into group 1",
            radA->checkState, BST_CHECKED, bad);

    destroyWindowRecursive(dlg);

    if (!bad)
        oapiWriteLog((char *)"RadioGroup self-test PASSED: an auto-radio checks "
                             "itself and clears the auto-radio siblings in its "
                             "WS_GROUP span only -- checkboxes in the span and "
                             "radios past the boundary are untouched");
    else {
        char msg[224];
        snprintf(msg, sizeof(msg),
                 "RadioGroup self-test FAILED with %d bad values -- 47 "
                 "auto-radio buttons in the templates depend on this", bad);
        oapiWriteLog(msg);
    }
}

int orbiter_GetItemCount(HWND h)
{
    Window *w = toWindow(h);
    return w ? (int)w->items.size() : 0;
}

const char *orbiter_GetItemText(HWND h, int index)
{
    Window *w = toWindow(h);
    if (!w || index < 0 || index >= (int)w->items.size()) return nullptr;
    return w->items[index].c_str();
}

// Multi-selection accessors for the renderer. A list box with
// LBS_MULTIPLESEL / LBS_EXTENDEDSEL tracks a flag per item rather than one
// current index.
int orbiter_IsMultiSel(HWND h)
{
    Window *w = toWindow(h);
    if (!w) return 0;
    return (w->style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)) ? 1 : 0;
}

int orbiter_GetItemSel(HWND h, int index)
{
    Window *w = toWindow(h);
    if (!w || index < 0 || index >= (int)w->itemSel.size()) return 0;
    return w->itemSel[index] ? 1 : 0;
}

void orbiter_ToggleItemSel(HWND h, int index)
{
    Window *w = toWindow(h);
    if (!w || index < 0 || index >= (int)w->itemSel.size()) return;
    w->itemSel[index] = w->itemSel[index] ? 0 : 1;
}

int orbiter_GetCurSel(HWND h)
{
    Window *w = toWindow(h);
    return w ? w->curSel : -1;
}

void orbiter_SetCurSel(HWND h, int sel)
{
    if (Window *w = toWindow(h)) {
        w->curSel = sel;
        // The user picking from the drop-down updates the edit field, exactly
        // as CB_SETCURSEL does. This is the path the renderer takes
        // (UIHost.cpp's combo popup), and without it a combo the user changed
        // by hand would report the OLD text to GetWindowText while showing
        // the new item -- worse than the original defect, because the two
        // would disagree. comboSyncText ignores list boxes, which share this
        // entry point.
        comboSyncText(w);
    }
}

void orbiter_SetText(HWND h, const char *text)
{
    if (Window *w = toWindow(h)) w->text = text ? text : "";
}

// The user TYPING in an editable combo's edit field, which is a different
// thing from the selection changing and must not go through orbiter_SetText.
//
// A CBS_DROPDOWN / CBS_SIMPLE combo is two controls: an edit field and a list,
// and they stay coupled in BOTH directions. Picking from the list writes the
// item into the field (comboSyncText, above). Typing into the field drives the
// list the other way: USER32 runs a prefix search for what has been typed and
// selects the item it finds, or clears the selection when nothing matches --
// which is why CB_GETCURSEL answers CB_ERR after the user types a string that
// is in no item.
//
// Both halves matter here. Without the search, typing "de" into a "deg"/"rad"
// combo would leave CB_GETCURSEL at CB_ERR while the field plainly reads a
// list item, and EditorTab_Elements::Refresh indexes anglescale[] with exactly
// that value (Editor.cpp:1568-1577). Without the clear, the next comboSyncText
// -- a CB_DELETESTRING, or a CB_SETCURSEL elsewhere -- would resurrect the old
// item's text over what the user typed.
//
// CB_FINDSTRING is the search USER32 uses (a case-insensitive prefix match
// that wraps), and the shim already implements it; an empty field selects
// nothing, as it does on Windows.
//
// The one caller is the renderer's editable-combo field, which follows this
// with CBN_EDITCHANGE. ScnEditor listens for that on IDC_REF in three tabs
// (Editor.cpp:1622, 1832, 2128) and answers by re-reading the field with
// GetWindowText and resolving it with oapiGetGbodyByName -- so typing "Moon"
// re-targets the reference body without touching the list, exactly as on
// Windows.
void orbiter_SetComboEditText(HWND h, const char *text)
{
    Window *w = toWindow(h);
    if (!w) return;
    w->text = text ? text : "";

    if (w->text.empty()) {
        w->curSel = -1;
        return;
    }
    const LRESULT found = SendMessageA(h, CB_FINDSTRING, (WPARAM)-1,
                                       (LPARAM)w->text.c_str());
    w->curSel = (found == CB_ERR) ? -1 : (int)found;
}

int orbiter_HasWndProc(HWND h)
{
    // True when the control belongs to a class Orbiter registered -- the
    // DlgCtrl gauges, switches and property list, and CustomControls. Those
    // paint themselves in response to WM_PAINT, so the UI host must send it
    // rather than draw a substitute.
    Window *w = toWindow(h);
    return (w && w->wndProc) ? 1 : 0;
}

int orbiter_QueryCtlColor(HWND h, unsigned *fg, unsigned *bk, int *opaque)
{
    // WM_CTLCOLORSTATIC asks the parent how a static should be coloured. The
    // handler receives the control's DC, calls SetTextColor/SetBkColor or
    // SetBkMode on it, and returns the background brush.
    //
    // Launchpad.cpp uses this to make IDC_BLACKBOX a black panel with pale
    // text -- the template gives that control no special style, so this
    // message is the only thing that distinguishes it. A handler that does
    // not recognise the control returns FALSE and the defaults stand.
    Window *w = toWindow(h);
    if (!w || !w->parent) return 0;

    HDC dc = orbiter_GetPaintDC(h);
    if (!dc) return 0;

    // Defaults, so a handler that sets only one of the two still yields a
    // sensible pair.
    orbiter_SetDCTextColor(dc, 0x000000);
    orbiter_SetDCBkColor(dc, 0xF0F4F8);
    orbiter_SetDCBkMode(dc, OPAQUE);

    const LRESULT brush = SendMessageA(toHwnd(w->parent), WM_CTLCOLORSTATIC,
                                       (WPARAM)dc, (LPARAM)toHwnd(w));
    if (!brush) return 0;   // not handled

    if (fg)     *fg     = orbiter_GetDCTextColor(dc);
    if (bk)     *bk     = orbiter_GetDCBkColor(dc);
    // SetBkMode(TRANSPARENT) means "do not fill", which WaitProc uses for
    // IDC_WAITTEXT so the dialog brush shows through.
    if (opaque) *opaque = (orbiter_GetDCBkMode(dc) == OPAQUE) ? 1 : 0;
    return 1;
}

// Sends a tree-view notification to the control's parent.
//
// TabScenario::OnNotify receives an NMHDR* and casts it to NM_TREEVIEW, then
// switches on hdr.code. The struct is filled the way the common control fills
// it: hwndFrom/idFrom identify the control, and itemNew/itemOld carry the
// handles for a selection change.
void orbiter_NotifyTree(HWND ctrl, UINT code, void *newItem, void *oldItem)
{
    Window *w = toWindow(ctrl);
    if (!w || !w->parent) return;

    TreeItem *ni = (TreeItem *)newItem;
    TreeItem *oi = (TreeItem *)oldItem;

    NM_TREEVIEW nm;
    memset(&nm, 0, sizeof(nm));
    nm.hdr.hwndFrom = ctrl;
    nm.hdr.idFrom   = (UINT_PTR)w->id;
    nm.hdr.code     = code;

    // The item structures must carry lParam, not just the handle. Both
    // OptionsPageContainer::OnNotifyPagelist and ExtraTab::OnNotify read
    //     (SomeType*)pnmtv->itemNew.lParam
    // straight out of this notification and call through it, so a handle-only
    // struct hands them a null pointer to dereference.
    nm.itemNew.mask   = TVIF_HANDLE | TVIF_PARAM | TVIF_STATE;
    nm.itemNew.hItem  = (HTREEITEM)ni;
    nm.itemNew.lParam = ni ? ni->lParam : 0;
    nm.itemNew.state  = ni ? ni->state  : 0;

    nm.itemOld.mask   = TVIF_HANDLE | TVIF_PARAM;
    nm.itemOld.hItem  = (HTREEITEM)oi;
    nm.itemOld.lParam = oi ? oi->lParam : 0;

    // Posted with an owned copy of the struct, for the reason given at
    // notifyParent: the handler may open a modal, and it must not run inside
    // the paint pass. lParam here points at a stack object, so the bytes are
    // copied into the queue entry and the pointer is rebound on delivery.
    QueuedMsg q;
    q.msg.hwnd    = toHwnd(w->parent);
    q.msg.message = WM_NOTIFY;
    q.msg.wParam  = (WPARAM)w->id;
    q.payload.assign((const char *)&nm, (const char *)&nm + sizeof(nm));
    g_messageQueue.push_back(std::move(q));
}

// Flattens the visible part of a tree into display order, so the renderer can
// draw rows without knowing the item structure. A collapsed item contributes
// itself but not its descendants, which is what makes expansion work.
static void flattenTree(const std::vector<TreeItem *> &list, int depth,
                        std::vector<TreeItem *> &out,
                        std::vector<int> &depths)
{
    for (TreeItem *it : list) {
        out.push_back(it);
        depths.push_back(depth);
        if (it->expanded && !it->children.empty())
            flattenTree(it->children, depth + 1, out, depths);
    }
}

int orbiter_GetBitmapId(HWND h)
{
    // Carried through from the template: for an SS_BITMAP control the .rc
    // puts a resource id in the text slot rather than a caption, and
    // rc2cpp.py records it separately.
    Window *w = toWindow(h);
    return w ? w->bitmapId : 0;
}

void orbiter_GetScrollState(HWND h, int *lo, int *hi, int *pos, int *page)
{
    Window *w = toWindow(h);
    if (!w) return;
    if (lo)   *lo   = w->scrollV.minPos;
    if (hi)   *hi   = w->scrollV.maxPos;
    if (pos)  *pos  = w->scrollV.pos;
    if (page) *page = w->scrollV.page;
}

void orbiter_GetRange(HWND h, int *lo, int *hi, int *pos)
{
    Window *w = toWindow(h);
    if (!w) return;
    if (lo)  *lo  = w->scrollV.minPos;
    if (hi)  *hi  = w->scrollV.maxPos;
    if (pos) *pos = w->scrollV.pos;
}

// The trackbar's tick spacing, for the TBS_AUTOTICKS renderer.
int orbiter_GetTicFreq(HWND h)
{
    Window *w = toWindow(h);
    return (w && w->ticFreq > 0) ? w->ticFreq : 1;
}

// Moves a trackbar and reports it the way the real control does: a WM_HSCROLL
// to the parent with SB_THUMBTRACK, which OptionsPage::DlgProc routes to
// OnHScroll. The page then reads the position back with TBM_GETPOS.
void orbiter_SetTrackPos(HWND h, int pos)
{
    Window *w = toWindow(h);
    if (!w) return;
    const int clamped = boundedPos(pos, w->scrollV.minPos, w->scrollV.maxPos);
    if (clamped == w->scrollV.pos) return;
    w->scrollV.pos = clamped;

    if (w->parent)
        SendMessageA(toHwnd(w->parent), WM_HSCROLL,
                     MAKEWPARAM(SB_THUMBTRACK, clamped), (LPARAM)toHwnd(w));
}

// A scrollbar control drag or click. The scroll position lives on the control
// and the parent is told through WM_VSCROLL, which is exactly what
// OptionsPageContainer::VScroll expects: it reads the position back with
// GetScrollInfo(SB_CTL) and then scrolls the page itself.
void orbiter_ScrollBarNotify(HWND h, int request, int pos)
{
    Window *w = toWindow(h);
    if (!w || !w->parent) return;

    // SB_CTL means the message came from a standalone scrollbar control, and
    // lParam carries its handle -- which is how VScroll knows which control to
    // query.
    SendMessageA(toHwnd(w->parent), WM_VSCROLL,
                 MAKEWPARAM(request, pos), (LPARAM)toHwnd(w));
}

// An up-down click. UDN_DELTAPOS is sent before the value changes so the
// parent may veto it by returning non-zero, which is the documented contract;
// nothing in this tree vetoes, but honouring it costs one branch.
void orbiter_SetSpinPos(HWND h, int pos, int delta)
{
    Window *w = toWindow(h);
    if (!w) return;

    if (w->parent) {
        NMUPDOWN nm;
        memset(&nm, 0, sizeof(nm));
        nm.hdr.hwndFrom = h;
        nm.hdr.idFrom   = (UINT_PTR)w->id;
        nm.hdr.code     = UDN_DELTAPOS;
        nm.iPos   = w->scrollV.pos;
        nm.iDelta = delta;
        if (SendMessageA(toHwnd(w->parent), WM_NOTIFY,
                         (WPARAM)w->id, (LPARAM)&nm))
            return;   // vetoed
    }
    w->scrollV.pos = boundedPos(pos, w->scrollV.minPos, w->scrollV.maxPos);
}

HIMAGELIST orbiter_TreeImageList(HWND h)
{
    Window *w = toWindow(h);
    return (w && w->tree) ? w->tree->images : nullptr;
}

int orbiter_TreeVisibleCount(HWND h)
{
    Window *w = toWindow(h);
    if (!w || !w->tree) return 0;
    g_activeTree = w->tree.get();
    std::vector<TreeItem *> items; std::vector<int> depths;
    flattenTree(w->tree->roots, 0, items, depths);
    return (int)items.size();
}

int orbiter_TreeGetRow(HWND h, int index, const char **text, int *depth,
                       int *image, int *selected, int *hasChildren,
                       int *expanded, int *checkState)
{
    Window *w = toWindow(h);
    if (!w || !w->tree) return 0;
    g_activeTree = w->tree.get();
    std::vector<TreeItem *> items; std::vector<int> depths;
    flattenTree(w->tree->roots, 0, items, depths);
    if (index < 0 || index >= (int)items.size()) return 0;

    TreeItem *it = items[index];
    const bool sel = (it == w->tree->selected);
    if (text)        *text        = it->text.c_str();
    if (depth)       *depth       = depths[index];
    // A selected item shows its selected image, which for a scenario is the
    // highlighted document icon.
    if (image)       *image       = sel ? it->selectedImage : it->image;
    if (selected)    *selected    = sel ? 1 : 0;
    if (hasChildren) *hasChildren = (!it->children.empty() || it->cChildren) ? 1 : 0;
    if (expanded)    *expanded    = it->expanded ? 1 : 0;
    // State-image index, in the top four bits of `state`. TabModule uses this
    // directly: modules are ticked with TreeView_SetCheckState, and category
    // rows then have their check box removed with SetItemState(0,
    // TVIS_STATEIMAGEMASK). So 0 means no box at all, 1 unchecked, 2 checked.
    if (checkState)  *checkState  = (int)((it->state & TVIS_STATEIMAGEMASK) >> 12);
    return 1;
}

// Toggles a row's check box, going through the same state bits the tree uses.
void orbiter_TreeToggleCheck(HWND h, int index)
{
    Window *w = toWindow(h);
    if (!w || !w->tree) return;
    g_activeTree = w->tree.get();
    std::vector<TreeItem *> items; std::vector<int> depths;
    flattenTree(w->tree->roots, 0, items, depths);
    if (index < 0 || index >= (int)items.size()) return;

    TreeItem *it = items[index];
    const UINT cur = (it->state & TVIS_STATEIMAGEMASK) >> 12;
    if (cur == 0) return;   // no check box on this row

    const UINT next = (cur == 2) ? 1 : 2;
    it->state = (it->state & ~TVIS_STATEIMAGEMASK) | (next << 12);
}

// A click on a row: selects it, and on the expander toggles instead.
void orbiter_TreeClickRow(HWND h, int index, int onExpander, int doubleClick)
{
    Window *w = toWindow(h);
    if (!w || !w->tree) return;
    g_activeTree = w->tree.get();
    std::vector<TreeItem *> items; std::vector<int> depths;
    flattenTree(w->tree->roots, 0, items, depths);
    if (index < 0 || index >= (int)items.size()) return;

    TreeItem *it = items[index];

    if (onExpander && (!it->children.empty() || it->cChildren)) {
        it->expanded = !it->expanded;
        return;
    }

    if (doubleClick) {
        // A folder toggles; a leaf launches, which is what NM_DBLCLK means to
        // TabScenario -- it posts IDLAUNCH.
        if (!it->children.empty()) { it->expanded = !it->expanded; return; }
        orbiter_NotifyTree(h, NM_DBLCLK, it, nullptr);
        return;
    }

    // Route through the message so the selection path and notification are
    // identical to a programmatic TVM_SELECTITEM.
    SendMessageA(h, TVM_SELECTITEM, TVGN_CARET, (LPARAM)it);
}

// A tree view sends NM_CUSTOMDRAW to its parent on every paint, and
// ModuleTab::OnNotify counts those to drive its whole activation sequence:
//
//     case NM_CUSTOMDRAW:
//         if (counter >= 0 && counter < 4) {
//             if (counter == 2) PostMessage(hDlg, WM_USER, 0, 0);
//             counter++;
//         } else if (counter == 4) ActivateFromList();
//
// WM_USER runs InitActivation(), which ticks the modules marked active in the
// config, removes the check boxes from the category rows and expands the tree;
// ActivateFromList() is what actually loads and unloads a module when its box
// is ticked. Without this notification none of that ever runs -- the tree sits
// collapsed, nothing is ticked, and checking a box does nothing at all.
//
// The comment in TabModule.cpp calls the counting a "terrible hack"; it is
// still the mechanism, so the notification has to arrive.
void orbiter_NotifyTreeCustomDraw(HWND ctrl)
{
    Window *w = toWindow(ctrl);
    if (!w || !w->parent) return;

    // The handler casts the payload to NM_TREEVIEW* and reads only hdr, but a
    // full-sized zeroed struct is sent so any field it might touch is valid.
    NM_TREEVIEW nm;
    memset(&nm, 0, sizeof(nm));
    nm.hdr.hwndFrom = ctrl;
    nm.hdr.idFrom   = (UINT_PTR)w->id;
    nm.hdr.code     = NM_CUSTOMDRAW;

    QueuedMsg q;
    q.msg.hwnd    = toHwnd(w->parent);
    q.msg.message = WM_NOTIFY;
    q.msg.wParam  = (WPARAM)w->id;
    q.payload.assign((const char *)&nm, (const char *)&nm + sizeof(nm));
    g_messageQueue.push_back(std::move(q));
}

void orbiter_SendPaint(HWND h)
{
    Window *w = toWindow(h);
    if (!w) return;

    // An owner-drawn control is painted by exactly one of two routes, not
    // both: the parent handles WM_DRAWITEM, or the control handles WM_PAINT.
    // Launchpad.cpp uses the first for IDC_SHADOW and IDC_MNU_PAGECONTAINER,
    // and its handler returns TRUE to say it drew. Sending WM_PAINT as well
    // would paint the control twice into the same DC.
    if (w->parent) {
        DRAWITEMSTRUCT dis;
        memset(&dis, 0, sizeof(dis));
        dis.CtlType    = ODT_STATIC;
        dis.CtlID      = (UINT)w->id;
        dis.itemAction = ODA_DRAWENTIRE;
        dis.hwndItem   = toHwnd(w);
        dis.hDC        = orbiter_GetPaintDC(h);
        GetClientRect(toHwnd(w), &dis.rcItem);

        // wParam is the control id: Launchpad.cpp's handler dispatches on it.
        if (SendMessageA(toHwnd(w->parent), WM_DRAWITEM,
                         (WPARAM)w->id, (LPARAM)&dis))
            return;   // the parent drew it
    }
    SendMessageA(h, WM_PAINT, 0, 0);
}

} // extern "C"

} // extern "C"
