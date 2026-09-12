// Linux entry point and platform-backend bridge.
//
// What lives here belongs with neither the POSIX services in Platform.cpp nor
// the dialog layer in Win32Dlg.cpp: the ELF entry point (Orbiter defines
// WinMain), the ImGui platform-backend bridge (Orbiter calls ImGui_ImplWin32_*
// by name, and this build compiles imgui_impl_glfw.cpp instead), and HtmlHelp,
// which has no Linux equivalent and is redirected.

#include <windows.h>
#include <commctrl.h>

#include <string>
#include <vector>

// The ImGui backends are C++, not extern "C", so their real headers are
// included rather than hand-declared: declaring them by hand inside extern "C"
// produces unmangled symbols that nothing resolves.
#include "imgui.h"
#include "imgui_impl_glfw.h"

// The graphics client owns the render window. Until a Vulkan client exists
// there is nothing to own it, so the pointer lives here and the client sets it
// once created.
namespace {
GLFWwindow *g_glfwWindow = nullptr;
}

extern "C" void orbiter_SetGLFWWindow(GLFWwindow *win) { g_glfwWindow = win; }
extern "C" GLFWwindow *orbiter_GetGLFWWindow(void)     { return g_glfwWindow; }

// Defined in Win32Dlg.cpp; asserts the list-box message contract the Scenario
// Editor depends on. It does not run from here: the log is not open this early,
// so a failure would print nothing and be indistinguishable from a pass. It
// runs, with three others, from Win32Dlg.cpp's CreateDialogParam, after
// Orbiter::Create.
extern "C" void orbiter_ListBoxSelfTest(void);

// DlgMgr.cpp and Orbiter.cpp call the Win32 ImGui backend by name, so rather
// than edit those sources the names are defined here and forwarded to the GLFW
// backend the build compiles in its place. They carry C++ linkage, to match the
// declarations in imgui_impl_win32.h the callers compile against.

// True while the Orbiter core object exists.
//
// Module destructors run from _dl_fini, after main returns and the core has been
// destroyed, and a module's ExitModule then calls back into it -- Meshdebug
// calls oapiUnregisterCustomCmd -- dereferencing a dead g_pOrbiter. Windows
// never has the problem, because Orbiter unloads its modules explicitly while it
// is still alive.
//
// Clearing the flag after WinMain returns covers only the orderly path:
// --fastexit calls exit() from inside Orbiter::CloseSession, which runs the exit
// handlers and then _dl_fini while WinMain is still on the stack, so every
// module destructor fires against a core halfway through closing a session:
//
//   Orbiter::CloseSession -> exit -> __run_exit_handlers -> _dl_fini
//     -> orb_module_detach -> ExitModule (TrackIR)
//       -> 0x0000000000000111        <- a vtable read out of dead memory
//
// An atexit handler catches every route to exit(), not just the one known today,
// and glibc registers _dl_fini's closure first and runs the list in reverse, so
// a handler installed from main runs before the module destructors.
static bool g_coreAlive = true;
extern "C" int orbiter_CoreAlive(void) { return g_coreAlive ? 1 : 0; }

extern "C" void orbiter_MarkCoreDead(void) { g_coreAlive = false; }

bool ImGui_ImplWin32_Init(void *)
{
    GLFWwindow *win = orbiter_GetGLFWWindow();
    if (!win) return false;

    // Only once per process. UIHost initialises this backend for the Launchpad
    // and DialogManager::InitImGui calls here again when a graphics client
    // attaches; on Windows those are two different backends, so the second call
    // is harmless there, while here both are the same GLFW backend and ImGui
    // asserts "Already initialized a platform backend!". Already-initialised is
    // reported as success: the backend the caller wanted is present.
    if (ImGui::GetIO().BackendPlatformUserData != nullptr)
        return true;

    // install_callbacks = true lets the backend chain GLFW's input callbacks,
    // which is how keyboard and mouse reach ImGui. Orbiter installs no GLFW
    // callbacks of its own, so there is nothing to conflict with.
    return ImGui_ImplGlfw_InitForVulkan(win, true);
}

void ImGui_ImplWin32_Shutdown(void)
{
    // Deliberately empty: UIHost owns this backend and needs it after the
    // session ends, to draw the Launchpad again. Tearing it down when a
    // session closes would leave the Launchpad with no input.
}

void ImGui_ImplWin32_NewFrame(void)
{
    ImGui_ImplGlfw_NewFrame();
}

LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM)
{
    // On Windows this lets ImGui consume input messages before the application
    // sees them. The GLFW backend receives input through GLFW's own callbacks,
    // so by the time a message reaches Orbiter's WndProc ImGui has already had
    // its chance: claiming to consume anything here would swallow Orbiter's
    // input.
    return 0;
}

extern "C" {

void InitCommonControls(void)
{
    // Registers the common control classes on Windows. Win32Dlg.cpp implements
    // the control types directly, so there is nothing to register.
}

DWORD GetWindowThreadProcessId(HWND, LPDWORD processId)
{
    // Every window in this build belongs to the process that created it and
    // to the single UI thread.
    if (processId) *processId = (DWORD)getpid();
    return (DWORD)(uintptr_t)pthread_self();
}

// HtmlHelp opens a .chm through the Windows help viewer. There is no .chm reader
// to rely on here, and Orbiter's documentation ships as PDF alongside the
// compiled help, so the request goes to the desktop's default handler. Callers
// check the return value, so a failed open must report failure rather than
// pretend the help appeared.
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

// WinMain carries C++ linkage: Orbiter.cpp defines it without extern "C", so
// declaring it as C here would name a different, undefined symbol.
INT WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, INT);

int main(int argc, char **argv)
{
    // WinMain receives the command line *without* the program name, matching
    // GetCommandLine's behaviour after the executable path is stripped;
    // Orbiter's own tokenizer expects that form, and the re-quoting below is
    // what makes it split the arguments back into the shell's pieces.
    std::string cmdline;
    for (int i = 1; i < argc; ++i) {
        if (i > 1) cmdline += ' ';
        if (strchr(argv[i], ' ')) {
            cmdline += '"';
            cmdline += argv[i];
            cmdline += '"';
        } else {
            cmdline += argv[i];
        }
    }

    // Every route out of the process must mark the core dead before the module
    // destructors run, not just the one that returns from WinMain: --fastexit
    // calls exit() from inside Orbiter::CloseSession and never comes back here.
    // See g_coreAlive.
    atexit(orbiter_MarkCoreDead);

    // hInstance identifies the executable's own module, which is what
    // GetModuleHandle(NULL) returns and what the shim resolves symbols against.
    HINSTANCE self = (HINSTANCE)GetModuleHandleA(nullptr);

    const int rc = (int)WinMain(self, nullptr, (LPSTR)cmdline.c_str(),
                               SW_SHOWNORMAL);

    // From here the core is gone. Module destructors run later still, from
    // _dl_fini, and must not call back into it -- see orbiter_CoreAlive.
    g_coreAlive = false;
    return rc;
}
