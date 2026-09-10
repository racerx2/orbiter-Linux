// A scripted driver for the Launchpad and its dialogs.
//
// ===========================================================================
// WHY THIS EXISTS
// ===========================================================================
//
// Several of the shim's defects can only be confirmed by DOING something to
// the UI -- opening a modal and pressing Tab, clicking a tab button, deleting
// a row from a list. The five-scenario sweep cannot: it launches straight into
// a scenario and never touches the Launchpad.
//
// The obvious tool does not work here. vkrun.sh's own header records why:
// synthetic pointer motion from xdotool never lands inside the GLFW window on
// a KDE Wayland desktop -- windowmove and windowsize report success, the
// geometry really does change, and the click still goes nowhere. So driving
// the UI from outside the process is not available.
//
// This drives it from INSIDE, through exactly the paths a real click and a
// real keystroke take:
//
//   a key press  -> PostMessage(WM_KEYDOWN) to orbiter_ActiveDialog(), which
//                   is what UIHost::postKeyboardMessages does for real keys
//   a click      -> PostMessage(WM_COMMAND, MAKEWPARAM(id, BN_CLICKED)), which
//                   is what Win32Dlg::notifyParent posts when the UI host
//                   detects a real click
//
// Nothing is simulated at a lower level and nothing bypasses the message
// queue, so a test that passes here exercises the same code a user does.
//
// ===========================================================================
// IT IS INERT UNLESS ASKED FOR
// ===========================================================================
//
// The whole file does nothing at all unless ORBITER_UI_SCRIPT names a
// readable file. One getenv at startup, then a null check per frame.
//
// Usage:
//     ORBITER_UI_SCRIPT=/tmp/lp.txt ./Orbiter
//
// Script syntax -- one command per line, '#' comments, blank lines ignored:
//
//     wait <frames>        idle, letting the UI settle
//     key <NAME>           TAB, ESC, RETURN, SPACE, UP, DOWN, LEFT, RIGHT,
//                          HOME, END
//     syskey <letter>      Alt+letter, the mnemonic an '&' declares
//     click <ctrlid>       press the control with that id in the active dialog
//                          (posts WM_COMMAND to its owner, as a push button does)
//     press <ctrlid>       WM_LBUTTONDOWN + WM_LBUTTONUP ON the control -- the
//                          only way to reach a control with its own wndproc
//     childdump [ctrlid]   log the active dialog's children (or one control's),
//                          with class, geometry, visibility and wndproc
//     cancel               close the active dialog as its caption X does
//                          (posts WM_COMMAND(IDCANCEL), which is what
//                          DefDlgProc makes of WM_CLOSE for a dialog)
//     check <id> [0|1]     toggle (or set) a check box, then notify
//     radio <id>           check an auto-radio, clearing its group siblings
//     select <id> <index>  pick an entry in a combo or list box, then notify
//     tree <id> <row> [expand|dbl]   click a visible row of a tree control
//     treedump <id>        log every visible row of a tree, with its index
//     mouse <x> <y>        move the pointer over the RENDER window
//     mousedown <l|r|m>    press a button and hold it
//     mouseup <l|r|m>      release it
//     wheel <notches>      one WM_MOUSEWHEEL, positive is away from the user
//     customdump           log every registered custom function, with its index
//     customcmd <text>     run the custom function whose label contains <text>
//     customcmd @<index>   run the one at that index ('@', not '#' -- a '#'
//                          starts a comment and would strip the argument)
//     dump <label>         log the active dialog, the focused control and the
//                          modal depth, tagged with the label
//     log <text>           write a marker into the log
//     quit                 post WM_QUIT
//
// Every line is echoed to the log as it runs, so the log alone reconstructs
// what the driver did and what the UI did in response.
// ===========================================================================

#include <windows.h>

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>

// The dialog layer. orbiter_ActiveDialog is the same accessor
// UIHost::postKeyboardMessages targets, so the driver aims where real keys go.
extern "C" HWND orbiter_ActiveDialog(void);
extern "C" int  orbiter_ModalDepth(void);

// The pointer, for the mouse commands. Sets the raw position and button bits
// that UIHost::postMouseMessages reads; everything downstream of that -- the
// press/release edges, the drag capture, the wParam packing, PostMessage, the
// dispatch into RenderWndProc and Orbiter::MsgProc -- is the same code a hand
// on a mouse goes through. See the note by g_injectMouse in UIHost.cpp.
extern "C" void orbiter_InjectMouse(int x, int y, int buttons, float wheel);

// The state-changing half of a real click.
//
// A `click` posts the WM_COMMAND a button sends and nothing else, which is
// right for a push button -- pressing one changes no state of its own. It is
// WRONG for a check box, a radio button or a combo: for those, UIHost's
// renderer changes the control's state FIRST and notifies second, so a
// handler that reads the control back (BM_GETCHECK, CB_GETCURSEL -- which is
// what every one of them does) sees the new value.
//
// Driving those three through `click` alone therefore tests nothing: the
// handler runs, reads the OLD state, and concludes nothing changed. These are
// the same four entry points drawControl uses, so `check`, `radio` and
// `select` below take the identical path a user's click does.
extern "C" void orbiter_NotifyCommand(HWND ctrl, unsigned short notifyCode);
extern "C" int  orbiter_GetCheckState(HWND h);
extern "C" void orbiter_SetCheckState(HWND h, int state);
extern "C" void orbiter_CheckRadioButton(HWND h);
extern "C" void orbiter_SetCurSel(HWND h, int sel);

// Tree controls. The Extra tab is a tree and an Edit button, and the scenario
// list is a tree too, so without these two neither page can be driven at all.
// orbiter_TreeClickRow is the same entry point UIHost's renderer calls when a
// row is clicked; the row index is into the VISIBLE rows, which is why the
// count comes from orbiter_TreeVisibleCount rather than from the item tree.
extern "C" int  orbiter_TreeVisibleCount(HWND h);
extern "C" void orbiter_TreeClickRow(HWND h, int index, int onExpander,
                                     int doubleClick);
extern "C" int  orbiter_TreeGetRow(HWND h, int index, const char **text,
                                   int *depth, int *image, int *selected,
                                   int *hasChildren, int *expanded,
                                   int *checkState);

// The Custom Functions dialog. Its entries are ImGui buttons drawn from
// Orbiter's customcmd table, not controls in the HWND tree, so `click` cannot
// reach one -- see the note above these three in DlgFunction.cpp. Running the
// command from here enters it inside orbiter_PumpFrame, which is where a real
// click enters it too.
// Orbiter.cpp: the menu bar's registered buttons, for menudump / menucmd.
extern "C" int         orbiter_MenuCmdCount(void);
extern "C" const char *orbiter_MenuCmdLabel(int i);
extern "C" int         orbiter_MenuCmdRun(int i);

extern "C" int         orbiter_CustomCmdCount(void);
extern "C" const char *orbiter_CustomCmdLabel(int i);
extern "C" int         orbiter_CustomCmdRun(int i);

// The control tree, for `childdump`, and the custom-class test that decides
// which of drawControl's branches a control takes. Same three the UI host
// itself uses to lay a dialog out, so a dump reports what the renderer sees
// rather than a second opinion.
extern "C" int  orbiter_EnumChildren(HWND h, HWND *out, int max);
extern "C" void orbiter_GetControlInfo(HWND h, const char **cls, const char **txt,
                                       int *id, int *x, int *y, int *cx, int *cy,
                                       unsigned *style, int *visible, int *enabled);
extern "C" int  orbiter_HasWndProc(HWND h);

void oapiWriteLog(char *line);

namespace {

struct Step {
    std::string op;
    std::string arg;
};

std::vector<Step> g_script;
size_t            g_next     = 0;
int               g_waitLeft = 0;
bool              g_loaded   = false;
bool              g_active   = false;

void drvlog(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    oapiWriteLog(buf);
}

int vkeyFromName(const std::string &n)
{
    if (n == "TAB")    return VK_TAB;
    if (n == "ESC")    return VK_ESCAPE;
    if (n == "RETURN") return VK_RETURN;
    if (n == "SPACE")  return VK_SPACE;
    if (n == "UP")     return VK_UP;
    if (n == "DOWN")   return VK_DOWN;
    if (n == "LEFT")   return VK_LEFT;
    if (n == "RIGHT")  return VK_RIGHT;
    if (n == "HOME")   return VK_HOME;
    if (n == "END")    return VK_END;
    return 0;
}

void loadScript()
{
    g_loaded = true;
    const char *path = getenv("ORBITER_UI_SCRIPT");
    if (!path || !*path) return;

    std::ifstream f(path);
    if (!f) {
        drvlog("UiDriver: cannot open script '%s'", path);
        return;
    }

    std::string line;
    while (std::getline(f, line)) {
        // Strip a comment and surrounding space.
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        size_t b = line.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        size_t e = line.find_last_not_of(" \t\r\n");
        line = line.substr(b, e - b + 1);
        if (line.empty()) continue;

        Step s;
        const size_t sp = line.find_first_of(" \t");
        if (sp == std::string::npos) s.op = line;
        else {
            s.op  = line.substr(0, sp);
            const size_t a = line.find_first_not_of(" \t", sp);
            if (a != std::string::npos) s.arg = line.substr(a);
        }
        g_script.push_back(s);
    }

    g_active = !g_script.empty();
    drvlog("UiDriver: loaded %zu steps from '%s'", g_script.size(), path);
}

// The dialog and control the UI is currently on. This is the observation side
// of the driver: a test asserts on these, so they are logged in a fixed,
// easily matched shape.
void dumpState(const char *label)
{
    HWND dlg = orbiter_ActiveDialog();
    HWND foc = GetFocus();
    const int dlgId = dlg ? GetDlgCtrlID(dlg) : -1;
    const int focId = foc ? GetDlgCtrlID(foc) : -1;
    drvlog("UIDRV %s: dialog=%d focus=%d modal=%d",
           label ? label : "state", dlgId, focId, orbiter_ModalDepth());
}

// Resolve a control id, optionally scoped to a parent: "1010" or "105/1010".
//
// WHY SCOPING EXISTS, and it is not a convenience. GetDlgItem here DESCENDS
// into nested child dialogs -- that is deliberate, and it is what makes a
// control on a Launchpad tab page reachable from the top-level handle. But
// ScnEditor keeps all twelve of its tab pages alive as siblings and merely
// HIDES eleven of them, and control ids repeat across those templates.
//
// So `click 1010`, meant for IDC_ELEMENTS on the Edit page, found a control
// with that id on the earlier, hidden Vessel page instead. The notification
// went to that page's procedure, which ran SwitchTab(11) -- and because
// ScnEditorTab::SwitchTab hides ITS OWN page, the Edit page stayed visible
// and the Date page was shown over it. The screenshot showed two dialogs
// composited on top of each other and looked exactly like a renderer defect.
// It was the test pointing at the wrong control.
//
// "<parent>/<id>" resolves the parent first and searches only inside it.
static HWND resolveCtrl(HWND dlg, const std::string &arg, int *idOut)
{
    const size_t slash = arg.find('/');
    if (slash == std::string::npos) {
        const int id = atoi(arg.c_str());
        if (idOut) *idOut = id;
        return GetDlgItem(dlg, id);
    }
    const int pid = atoi(arg.substr(0, slash).c_str());
    const int id  = atoi(arg.c_str() + slash + 1);
    if (idOut) *idOut = id;
    HWND parent = GetDlgItem(dlg, pid);
    if (!parent) {
        drvlog("UIDRV: parent %d not in the active dialog", pid);
        return nullptr;
    }
    return GetDlgItem(parent, id);
}

} // namespace

// Called once per frame from the UI host's pump, after the widget pass and
// after real key events have been posted -- so a scripted key lands in the
// same queue position a real one would.
extern "C" void orbiter_UiDriverStep(void)
{
    if (!g_loaded) loadScript();
    if (!g_active) return;

    if (g_waitLeft > 0) { --g_waitLeft; return; }

    if (g_next >= g_script.size()) {
        drvlog("UIDRV: script complete (%zu steps)", g_script.size());
        g_active = false;
        return;
    }

    const Step &s = g_script[g_next++];

    if (s.op == "wait") {
        g_waitLeft = atoi(s.arg.c_str());
        return;
    }

    drvlog("UIDRV step %zu: %s %s", g_next - 1, s.op.c_str(), s.arg.c_str());

    if (s.op == "log") {
        // Already echoed above; the marker is the point.
        return;
    }

    if (s.op == "dump") {
        dumpState(s.arg.empty() ? "state" : s.arg.c_str());
        return;
    }

    // --- the pointer ----------------------------------------------------
    //
    // Position and buttons are held here rather than re-derived each step, so
    // a drag reads naturally in a script:
    //
    //     mouse 800 400
    //     mousedown r
    //     mouse 830 400
    //     mouse 860 400
    //     mouseup r
    //
    // and each intervening `wait` lets a frame carry the movement into
    // Camera::UpdateMouse, which is where a drag becomes rotation.
    static int msX = 0, msY = 0, msButtons = 0;

    if (s.op == "mouse") {
        int x = msX, y = msY;
        if (sscanf(s.arg.c_str(), "%d %d", &x, &y) == 2) {
            msX = x; msY = y;
            orbiter_InjectMouse(msX, msY, msButtons, 0.0f);
        } else {
            drvlog("UIDRV: mouse needs '<x> <y>', got '%s'", s.arg.c_str());
        }
        return;
    }

    if (s.op == "mousedown" || s.op == "mouseup") {
        const char c = s.arg.empty() ? 'l' : (char)tolower((unsigned char)s.arg[0]);
        const int bit = (c == 'r') ? 2 : (c == 'm') ? 4 : 1;
        if (s.op == "mousedown") msButtons |= bit;
        else                     msButtons &= ~bit;
        orbiter_InjectMouse(msX, msY, msButtons, 0.0f);
        drvlog("UIDRV mouse buttons now %d at (%d,%d)", msButtons, msX, msY);
        return;
    }

    if (s.op == "wheel") {
        const float notches = (float)atof(s.arg.c_str());
        orbiter_InjectMouse(msX, msY, msButtons, notches);
        return;
    }

    if (s.op == "key") {
        HWND target = orbiter_ActiveDialog();
        if (!target) { drvlog("UIDRV: no active dialog for key"); return; }
        const int vk = vkeyFromName(s.arg);
        if (!vk) { drvlog("UIDRV: unknown key '%s'", s.arg.c_str()); return; }
        PostMessageA(target, WM_KEYDOWN, (WPARAM)vk, 0);
        PostMessageA(target, WM_KEYUP,   (WPARAM)vk, 0);
        return;
    }

    if (s.op == "syskey") {
        HWND target = orbiter_ActiveDialog();
        if (!target || s.arg.empty()) return;
        PostMessageA(target, WM_SYSKEYDOWN,
                     (WPARAM)toupper((unsigned char)s.arg[0]), 0);
        return;
    }

    // (see resolveCtrl above the dispatch for the "<parent>/<id>" form)
    if (s.op == "click") {
        HWND dlg = orbiter_ActiveDialog();
        if (!dlg) { drvlog("UIDRV: no active dialog for click"); return; }
        int id = 0;
        HWND ctrl = resolveCtrl(dlg, s.arg, &id);
        if (!ctrl) { drvlog("UIDRV: control %s not in the active dialog",
                            s.arg.c_str()); return; }

        // TO ITS PARENT, not to the active dialog. A control notification
        // goes to the window that owns the control -- for a control on a tab
        // page that is the PAGE's dialog procedure, not the Launchpad's, and
        // posting to the wrong one delivers WM_COMMAND to a handler that has
        // no case for that id. This is what Win32Dlg::notifyParent does.
        HWND owner = GetParent(ctrl);
        if (!owner) owner = dlg;
        PostMessageA(owner, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED), (LPARAM)ctrl);
        return;
    }

    // check <id>          -- toggle a check box
    // check <id> <0|1>    -- set it
    //
    // Toggles the state and then notifies, in that order, exactly as
    // drawControl's BS_AUTOCHECKBOX branch does. NOTE the notification is
    // SENT, not posted: orbiter_NotifyCommand goes straight to the parent's
    // dialog procedure the same way a real click's does, so the handler has
    // run by the time the next script step is read.
    if (s.op == "check") {
        HWND dlg = orbiter_ActiveDialog();
        if (!dlg) { drvlog("UIDRV: no active dialog for check"); return; }

        int id = 0, want = -1;
        const size_t sp = s.arg.find_first_of(" \t");
        if (sp == std::string::npos) id = atoi(s.arg.c_str());
        else {
            id   = atoi(s.arg.substr(0, sp).c_str());
            want = atoi(s.arg.substr(sp + 1).c_str());
        }

        HWND ctrl = GetDlgItem(dlg, id);
        if (!ctrl) { drvlog("UIDRV: control %d not in the active dialog", id); return; }

        const int before = orbiter_GetCheckState(ctrl);
        const int after  = (want < 0)
                         ? (before == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED)
                         : (want ? BST_CHECKED : BST_UNCHECKED);
        orbiter_SetCheckState(ctrl, after);
        orbiter_NotifyCommand(ctrl, BN_CLICKED);
        drvlog("UIDRV check %d: %s -> %s", id,
               before == BST_CHECKED ? "checked" : "unchecked",
               after  == BST_CHECKED ? "checked" : "unchecked");
        return;
    }

    // radio <id> -- check an auto-radio and clear its WS_GROUP siblings
    if (s.op == "radio") {
        HWND dlg = orbiter_ActiveDialog();
        if (!dlg) { drvlog("UIDRV: no active dialog for radio"); return; }
        const int id = atoi(s.arg.c_str());
        HWND ctrl = GetDlgItem(dlg, id);
        if (!ctrl) { drvlog("UIDRV: control %d not in the active dialog", id); return; }
        orbiter_CheckRadioButton(ctrl);
        orbiter_NotifyCommand(ctrl, BN_CLICKED);
        drvlog("UIDRV radio %d checked", id);
        return;
    }

    // select <id> <index> -- pick an entry in a combo or list box
    if (s.op == "select") {
        HWND dlg = orbiter_ActiveDialog();
        if (!dlg) { drvlog("UIDRV: no active dialog for select"); return; }

        const size_t sp = s.arg.find_first_of(" \t");
        if (sp == std::string::npos) {
            drvlog("UIDRV: select needs an id and an index");
            return;
        }
        const std::string idArg = s.arg.substr(0, sp);
        const int idx = atoi(s.arg.substr(sp + 1).c_str());

        int id = 0;
        HWND ctrl = resolveCtrl(dlg, idArg, &id);
        if (!ctrl) { drvlog("UIDRV: control %s not in the active dialog",
                            idArg.c_str()); return; }

        orbiter_SetCurSel(ctrl, idx);
        orbiter_NotifyCommand(ctrl, CBN_SELCHANGE);
        drvlog("UIDRV select %s -> index %d", idArg.c_str(), idx);
        return;
    }

    // tree <id> <row> [expand|dbl]   -- click a visible row of a tree control
    // treedump <id>                  -- log every visible row, with its index
    //
    // A tree row is not a control and has no id, so `click` cannot reach one.
    // These go through orbiter_TreeClickRow, which is what UIHost's renderer
    // calls for a real click, so selection notifications reach the dialog the
    // same way. treedump exists because the row index is into the VISIBLE
    // rows: it shifts as branches expand, and guessing it is how a test ends
    // up asserting against the wrong item.
    if (s.op == "tree" || s.op == "treedump") {
        HWND dlg = orbiter_ActiveDialog();
        if (!dlg) { drvlog("UIDRV: no active dialog for %s", s.op.c_str()); return; }

        int id = 0, row = 0;
        std::string mode;
        {
            const size_t sp = s.arg.find_first_of(" \t");
            if (sp == std::string::npos) id = atoi(s.arg.c_str());
            else {
                id = atoi(s.arg.substr(0, sp).c_str());
                std::string rest = s.arg.substr(sp + 1);
                const size_t sp2 = rest.find_first_of(" \t");
                if (sp2 == std::string::npos) row = atoi(rest.c_str());
                else {
                    row  = atoi(rest.substr(0, sp2).c_str());
                    mode = rest.substr(sp2 + 1);
                }
            }
        }

        HWND ctrl = GetDlgItem(dlg, id);
        if (!ctrl) { drvlog("UIDRV: control %d not in the active dialog", id); return; }

        const int n = orbiter_TreeVisibleCount(ctrl);

        if (s.op == "treedump") {
            drvlog("UIDRV treedump %d: %d visible row(s)", id, n);
            for (int i = 0; i < n; i++) {
                const char *text = nullptr;
                int depth = 0, image = 0, sel = 0, kids = 0, exp = 0, chk = 0;
                if (!orbiter_TreeGetRow(ctrl, i, &text, &depth, &image, &sel,
                                        &kids, &exp, &chk)) continue;
                drvlog("UIDRV   [%02d] depth=%d %s%s%s '%s'", i, depth,
                       sel ? "SEL " : "", kids ? (exp ? "- " : "+ ") : "",
                       chk ? "[x] " : "", text ? text : "");
            }
            return;
        }

        if (row < 0 || row >= n) {
            drvlog("UIDRV: tree row %d out of range (%d visible)", row, n);
            return;
        }
        const int onExpander  = (mode == "expand") ? 1 : 0;
        const int doubleClick = (mode == "dbl")    ? 1 : 0;
        orbiter_TreeClickRow(ctrl, row, onExpander, doubleClick);
        drvlog("UIDRV tree %d row %d%s%s", id, row,
               onExpander ? " (expander)" : "", doubleClick ? " (double)" : "");
        return;
    }

    // childdump [id]  -- log every child of the active dialog (or of control
    //                    <id> within it), with class, geometry and whether it
    //                    has a window procedure of its own.
    //
    // The last column is the one that matters. drawControl branches on
    // orbiter_HasWndProc: a control that HAS one is painted by sending it
    // WM_PAINT and replaying the GDI it recorded, and a control that does not
    // is drawn by the host imitating a standard Win32 class. A custom class
    // that fails the test is drawn as a blank rectangle and is invisible for
    // exactly that reason -- which is not something a screenshot can tell you
    // apart from "the control drew nothing".
    if (s.op == "childdump") {
        HWND dlg = orbiter_ActiveDialog();
        if (!dlg) { drvlog("UIDRV: no active dialog for childdump"); return; }
        if (!s.arg.empty()) {
            HWND sub = GetDlgItem(dlg, atoi(s.arg.c_str()));
            if (!sub) { drvlog("UIDRV: control %s not in the active dialog", s.arg.c_str()); return; }
            dlg = sub;
        }

        HWND kids[256];
        const int n = orbiter_EnumChildren(dlg, kids, 256);
        drvlog("UIDRV childdump: %d child(ren)", n);
        for (int i = 0; i < n; i++) {
            const char *cls = nullptr, *txt = nullptr;
            int id = 0, x = 0, y = 0, cx = 0, cy = 0, vis = 0, en = 0;
            unsigned st = 0;
            orbiter_GetControlInfo(kids[i], &cls, &txt, &id, &x, &y, &cx, &cy,
                                   &st, &vis, &en);
            drvlog("UIDRV   [%02d] id=%-5d cls='%s' at (%d,%d) %dx%d "
                   "vis=%d en=%d style=0x%08X wndproc=%d txt='%s'",
                   i, id, cls ? cls : "", x, y, cx, cy, vis, en, st,
                   orbiter_HasWndProc(kids[i]), txt ? txt : "");
        }
        return;
    }

    // press <id>  -- a real press and release ON the control, not a WM_COMMAND
    //
    // `click` posts WM_COMMAND(BN_CLICKED) to the control's OWNER, which is
    // what a standard push button sends. A control with a window procedure of
    // its own never sends that: DX9ExtMFD's MFD_BtnProc handles WM_LBUTTONDOWN
    // and WM_LBUTTONUP and calls ProcessButton itself, and its PWR button (
    // index 12) is what powers the MFD on. `click` cannot press it at all.
    //
    // Down and up are separate messages rather than one event because that is
    // what the host delivers for these controls and what DlgCtrl's gauges and
    // sliders need -- see the orbiter_HasWndProc branch in drawControl.
    if (s.op == "press") {
        HWND dlg = orbiter_ActiveDialog();
        if (!dlg) { drvlog("UIDRV: no active dialog for press"); return; }
        const int id = atoi(s.arg.c_str());
        HWND ctrl = GetDlgItem(dlg, id);
        if (!ctrl) { drvlog("UIDRV: control %d not in the active dialog", id); return; }
        PostMessageA(ctrl, WM_LBUTTONDOWN, MK_LBUTTON, 0);
        PostMessageA(ctrl, WM_LBUTTONUP,   0,          0);
        drvlog("UIDRV press %d (wndproc=%d)", id, orbiter_HasWndProc(ctrl));
        return;
    }

    // cancel  -- close the active dialog the way its caption's X does
    //
    // drawDialog posts WM_COMMAND(IDCANCEL) when the close box is clicked,
    // because that is what DefDlgProc synthesises from WM_CLOSE for a dialog.
    // The box itself is an ImGui widget and an injected press does not
    // activate one on this desk, so this posts the same message the box does
    // and exercises the half that matters: the module's own IDCANCEL handler,
    // its CloseDlg, and oapiCloseDialog.
    if (s.op == "cancel") {
        HWND dlg = orbiter_ActiveDialog();
        if (!dlg) { drvlog("UIDRV: no active dialog to cancel"); return; }
        drvlog("UIDRV cancel: posting IDCANCEL to dialog id=%d", GetDlgCtrlID(dlg));
        PostMessageA(dlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
        return;
    }

    // customdump                -- log every registered custom function
    // customcmd <text>          -- run the one whose label contains <text>
    // customcmd @<index>        -- run the one at that index
    //
    // Matched on a substring of the label, case-insensitively, because the
    // labels come from add-on modules and are not stable enough to type
    // exactly ("Terrain Toolkit", "Terrain ToolKit"). An ambiguous match runs
    // nothing and lists the candidates: silently taking the first would make
    // a test assert against whichever module happened to load first.
    //
    // '@' AND NOT '#' FOR THE INDEX. loadScript strips everything from the
    // first '#' as a comment, so `customcmd #3` arrives here as `customcmd`
    // with an empty argument -- which matches every label, is reported as
    // ambiguous, and runs NOTHING. A sweep written that way reports a clean
    // pass for thirteen entries none of which were ever entered. Measured:
    // the first two runs of funcsweep.sh did exactly that.
    if (s.op == "customdump") {
        const int n = orbiter_CustomCmdCount();
        drvlog("UIDRV customdump: %d custom function(s)", n);
        for (int i = 0; i < n; i++) {
            const char *lbl = orbiter_CustomCmdLabel(i);
            drvlog("UIDRV   [%02d] '%s'", i, lbl ? lbl : "");
        }
        return;
    }

    if (s.op == "customcmd") {
        const int n = orbiter_CustomCmdCount();
        if (n <= 0) { drvlog("UIDRV: no custom functions registered"); return; }

        int hit = -1;

        if (!s.arg.empty() && s.arg[0] == '@') {
            hit = atoi(s.arg.c_str() + 1);
            if (hit < 0 || hit >= n) {
                drvlog("UIDRV: custom function index %d out of range (%d registered)",
                       hit, n);
                return;
            }
        } else {
            std::string want = s.arg;
            for (char &c : want) c = (char)tolower((unsigned char)c);

            int matches = 0;
            for (int i = 0; i < n; i++) {
                const char *lbl = orbiter_CustomCmdLabel(i);
                if (!lbl) continue;
                std::string have = lbl;
                for (char &c : have) c = (char)tolower((unsigned char)c);
                if (have.find(want) == std::string::npos) continue;
                matches++;
                if (hit < 0) hit = i;
            }

            if (hit < 0) {
                drvlog("UIDRV: no custom function matching '%s' (%d registered)",
                       s.arg.c_str(), n);
                return;
            }
            if (matches > 1) {
                drvlog("UIDRV: '%s' matches %d custom functions; use customdump "
                       "and customcmd @<index>", s.arg.c_str(), matches);
                return;
            }
        }

        const char *lbl = orbiter_CustomCmdLabel(hit);
        // Logged BEFORE the call and flushed by oapiWriteLog, so a command
        // that hangs or faults still leaves its name in the log. That is how
        // the TerrainToolKit freeze was pinned down, and a line written after
        // the call would have said nothing.
        drvlog("UIDRV customcmd [%02d] '%s' -- entering", hit, lbl ? lbl : "");
        orbiter_CustomCmdRun(hit);
        drvlog("UIDRV customcmd [%02d] '%s' -- returned", hit, lbl ? lbl : "");
        return;
    }

    // menudump              -- log the menu bar's buttons, with indices
    // menucmd <label|@index> -- press one
    //
    // The bar is how the core dialogs are opened -- Ship, Camera, Function,
    // Info, Options, Map, Record -- and it is an ImGui window, so an injected
    // pointer reaches it as HOVER and never as a press (see the note in
    // the porting notes, where this cost a quicksave test). That
    // left Options and Custom functions unopenable by any test at all, which
    // is why comparisons against the Windows build had to be done by hand.
    //
    // This calls the registered callback, which is what MenuInfoBar's own
    // click handler does, from inside the frame pump.
    if (s.op == "menudump") {
        const int n = orbiter_MenuCmdCount();
        drvlog("UIDRV menudump: %d menu item(s)", n);
        for (int i = 0; i < n; i++) {
            const char *lbl = orbiter_MenuCmdLabel(i);
            drvlog("UIDRV   [%02d] '%s'", i, lbl ? lbl : "");
        }
        return;
    }

    if (s.op == "menucmd") {
        const int n = orbiter_MenuCmdCount();
        if (n <= 0) { drvlog("UIDRV: no menu items registered"); return; }

        int hit = -1;
        if (!s.arg.empty() && s.arg[0] == '@') {
            hit = atoi(s.arg.c_str() + 1);
            if (hit < 0 || hit >= n) {
                drvlog("UIDRV: menu index %d out of range (%d registered)", hit, n);
                return;
            }
        } else {
            std::string want = s.arg;
            for (char &c : want) c = (char)tolower((unsigned char)c);
            int matches = 0;
            for (int i = 0; i < n; i++) {
                const char *lbl = orbiter_MenuCmdLabel(i);
                if (!lbl) continue;
                std::string have = lbl;
                for (char &c : have) c = (char)tolower((unsigned char)c);
                if (have.find(want) == std::string::npos) continue;
                matches++;
                if (hit < 0) hit = i;
            }
            if (hit < 0) {
                drvlog("UIDRV: no menu item matching '%s' (%d registered)",
                       s.arg.c_str(), n);
                return;
            }
            if (matches > 1) {
                drvlog("UIDRV: '%s' matches %d menu items; use menudump and "
                       "menucmd @<index>", s.arg.c_str(), matches);
                return;
            }
        }

        const char *lbl = orbiter_MenuCmdLabel(hit);
        // Logged BEFORE the call, so an item that hangs still names itself.
        drvlog("UIDRV menucmd [%02d] '%s' -- entering", hit, lbl ? lbl : "");
        orbiter_MenuCmdRun(hit);
        drvlog("UIDRV menucmd [%02d] '%s' -- returned", hit, lbl ? lbl : "");
        return;
    }

    if (s.op == "quit") {
        PostQuitMessage(0);
        g_active = false;
        return;
    }

    drvlog("UIDRV: unknown command '%s'", s.op.c_str());
}
