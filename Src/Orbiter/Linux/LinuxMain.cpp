// Linux entry point and platform-backend bridge.
//
// Three things live here that do not belong with either the POSIX services in
// Platform.cpp or the dialog layer in Win32Dlg.cpp:
//
//   1. The ELF entry point. Orbiter defines WinMain; ELF starts at main.
//   2. The ImGui platform-backend bridge. Orbiter calls ImGui_ImplWin32_*
//      by name; the Linux build compiles imgui_impl_glfw.cpp instead.
//   3. HtmlHelp, which has no Linux equivalent and is redirected.

#include <windows.h>
#include <commctrl.h>

#include <string>
#include <vector>

// The ImGui backends are C++, not extern "C". Their real headers are included
// rather than hand-declared, so the signatures and mangled names match the
// definitions the build compiles from imgui_impl_glfw.cpp -- declaring them by
// hand inside extern "C" produces unmangled symbols that nothing resolves.
#include "imgui.h"
#include "imgui_impl_glfw.h"

// ===========================================================================
// The GLFW window
//
// The graphics client owns the render window. Until a Vulkan client exists
// there is nothing to own it, so the pointer lives here and the client sets it
// once created. Keeping it behind a setter rather than a bare global means the
// ImGui backend cannot be initialised against a window that does not exist.
// ===========================================================================

namespace {
GLFWwindow *g_glfwWindow = nullptr;
}

extern "C" void orbiter_SetGLFWWindow(GLFWwindow *win) { g_glfwWindow = win; }
extern "C" GLFWwindow *orbiter_GetGLFWWindow(void)     { return g_glfwWindow; }

// Win32Dlg.cpp. Asserts the list-box message contract the Scenario Editor
// depends on.
//
// IT DOES NOT RUN FROM HERE, AND THE COMMENT THAT SAID SO WAS LEFT BEHIND BY
// THE FIX. It was called from main() once; the log is not open that early, so
// the result went nowhere and the check printed nothing at all -- which is
// indistinguishable from passing. It now runs, with three others, from
// Win32Dlg.cpp's CreateDialogParam, where Orbiter::Create has already run.
// The declaration is kept only because removing it would leave nothing here
// pointing at where they went.
extern "C" void orbiter_ListBoxSelfTest(void);

// ===========================================================================
// ImGui platform backend bridge
//
// Src/Orbiter/DlgMgr.cpp and Orbiter.cpp call the Win32 backend by name at
// four sites. Rather than edit those sources, the names are provided here and
// forwarded to the GLFW backend, which the build compiles in place of the
// Win32 one.
//
// These carry C++ linkage, matching the declarations in imgui_impl_win32.h
// that the callers compile against.
// ===========================================================================

// True while the Orbiter core object exists.
//
// Module destructors run from _dl_fini, which is AFTER main returns and the
// core has been destroyed. A module's ExitModule then calls back into the
// core -- Meshdebug calls oapiUnregisterCustomCmd -- and dereferences a dead
// g_pOrbiter. On Windows the equivalent never happens because Orbiter unloads
// its modules explicitly while it is still alive, and the DLL detach that
// follows has nothing left to do.
//
// Cleared once WinMain returns, so any destructor running after that does
// nothing rather than reaching into freed memory.
//
// AND CLEARED ON ANY exit() TOO, WHICH IS NOT THE SAME THING.
//
// Clearing it after WinMain covers only the orderly path. --fastexit does not
// take it: Orbiter::CloseSession calls exit() directly (Orbiter.cpp:1012),
// which runs the exit handlers and then _dl_fini while WinMain is still on the
// stack and this flag is still true. Every module destructor then fires
// against a core that is halfway through closing a session, and the first one
// to call back into it dies. Measured, from the core dump:
//
//   Orbiter::CloseSession -> exit -> __run_exit_handlers -> _dl_fini
//     -> orb_module_detach -> ExitModule (TrackIR.cpp:298)
//       -> 0x0000000000000111        <- a vtable read out of dead memory
//
// TrackIR's ExitModule calls oapiUnregisterLaunchpadItem and then deletes an
// object with a virtual destructor; neither survives the core being gone.
//
// An atexit handler is the right hook because it catches every route to
// exit(), not just the one route known today. glibc registers _dl_fini's
// closure first and runs the list in reverse, so a handler installed from main
// runs BEFORE the module destructors -- which is exactly the ordering needed.
static bool g_coreAlive = true;
extern "C" int orbiter_CoreAlive(void) { return g_coreAlive ? 1 : 0; }

extern "C" void orbiter_MarkCoreDead(void) { g_coreAlive = false; }

bool ImGui_ImplWin32_Init(void *)
{
    GLFWwindow *win = orbiter_GetGLFWWindow();
    if (!win) return false;

    // Only once per process.
    //
    // UIHost initialises this backend when it brings the Launchpad up, and
    // DialogManager::InitImGui calls here again as soon as a graphics client
    // attaches -- on Windows those are two different backends, Win32 for the
    // dialogs and the client's own for the scene, so the second call is
    // harmless there. Here both are the same GLFW backend, and ImGui asserts:
    //     "Already initialized a platform backend!"
    // which aborted the session the instant the Vulkan client loaded.
    //
    // The already-initialised case is a success, not a failure: the backend
    // the caller wanted is present and working, which is exactly what it is
    // asking for.
    if (ImGui::GetIO().BackendPlatformUserData != nullptr)
        return true;

    // install_callbacks = true lets the backend chain GLFW's input callbacks,
    // which is how keyboard and mouse reach ImGui. Orbiter installs no GLFW
    // callbacks of its own, so there is nothing to conflict with.
    return ImGui_ImplGlfw_InitForVulkan(win, true);
}

void ImGui_ImplWin32_Shutdown(void)
{
    // Not shut down here for the same reason: UIHost owns this backend and
    // needs it after the session ends, to draw the Launchpad again. Tearing
    // it down when a session closes would leave the Launchpad with no input.
}

void ImGui_ImplWin32_NewFrame(void)
{
    ImGui_ImplGlfw_NewFrame();
}

LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM)
{
    // On Windows this lets ImGui consume input messages before the
    // application sees them. The GLFW backend receives input through GLFW's
    // own callbacks instead, so by the time a message reaches Orbiter's
    // WndProc ImGui has already had its chance. Reporting "not consumed" is
    // therefore correct: claiming otherwise would swallow Orbiter's input.
    return 0;
}

extern "C" {

void InitCommonControls(void)
{
    // Registers the common control classes on Windows. The control types are
    // implemented directly by Win32Dlg.cpp here, so there is nothing to
    // register.
}

DWORD GetWindowThreadProcessId(HWND, LPDWORD processId)
{
    // Every window in this build belongs to the process that created it and
    // to the single UI thread.
    if (processId) *processId = (DWORD)getpid();
    return (DWORD)(uintptr_t)pthread_self();
}

// ===========================================================================
// HTML Help
//
// HtmlHelp opens a .chm through the Windows help viewer. There is no .chm
// reader to rely on here, and Orbiter's documentation ships as PDF alongside
// the compiled help, so the request is handed to the desktop's default
// handler. Callers check the return value, so a failed open reports failure
// rather than pretending the help appeared.
// ===========================================================================

HWND HtmlHelpA(HWND, LPCSTR file, UINT, DWORD_PTR)
{
    if (!file || !*file) return nullptr;

    std::string path(file);

    // A .chm topic reference is written "file.chm::/topic.htm". Only the
    // container is openable here, so the topic suffix is dropped.
    const size_t sep = path.find("::");
    if (sep != std::string::npos) path.erase(sep);

    // Prefer a PDF of the same name when one exists: Orbiter ships its manuals
    // in both forms, and the PDF is the one a Linux desktop can display.
    const size_t dot = path.rfind('.');
    if (dot != std::string::npos) {
        std::string pdf = path.substr(0, dot) + ".pdf";
        if (access(pdf.c_str(), R_OK) == 0) path = pdf;
    }

    if (access(path.c_str(), R_OK) != 0) return nullptr;

    std::string cmd = "xdg-open '" + path + "' >/dev/null 2>&1 &";
    return system(cmd.c_str()) == 0 ? (HWND)1 : nullptr;
}

} // extern "C"

// ===========================================================================
// Entry point
//
// ELF has no WinMain. This converts argc/argv into the single command-line
// string WinMain expects and calls it.
// ===========================================================================

// WinMain carries C++ linkage: Orbiter.cpp defines it without extern "C", so
// declaring it as C here would name a different, undefined symbol.
INT WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, INT);

int main(int argc, char **argv)
{
    // WinMain receives the command line *without* the program name, matching
    // GetCommandLine's behaviour after the executable path is stripped. Orbiter
    // parses it with its own tokenizer, which expects that form.
    std::string cmdline;
    for (int i = 1; i < argc; ++i) {
        if (i > 1) cmdline += ' ';
        // Arguments containing spaces are re-quoted so the tokenizer splits
        // them back into the same pieces the shell produced.
        if (strchr(argv[i], ' ')) {
            cmdline += '"';
            cmdline += argv[i];
            cmdline += '"';
        } else {
            cmdline += argv[i];
        }
    }

    // Every route out of the process must mark the core dead before the module
    // destructors run, not just the one that returns from WinMain. --fastexit
    // calls exit() from inside Orbiter::CloseSession and never comes back
    // here; see the note by g_coreAlive for what that crashed.
    atexit(orbiter_MarkCoreDead);

    // hInstance identifies the executable's own module. GetModuleHandle(NULL)
    // returns exactly that, and is what the shim resolves symbols against.
    HINSTANCE self = (HINSTANCE)GetModuleHandleA(nullptr);

    const int rc = (int)WinMain(self, nullptr, (LPSTR)cmdline.c_str(),
                               SW_SHOWNORMAL);

    // From here the core is gone. Module destructors run later still, from
    // _dl_fini, and must not call back into it -- see orbiter_CoreAlive.
    g_coreAlive = false;
    return rc;
}
