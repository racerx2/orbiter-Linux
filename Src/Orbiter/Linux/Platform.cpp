// Linux implementation of the Win32 platform services the shim declares.
//
// Covers threads and synchronisation, dynamic module loading, process and
// module inspection, error reporting, the console, the registry and a handful
// of shell and system queries. The window, dialog and GDI surface lives
// separately in Win32Dlg.cpp, because that half is backed by ImGui and this
// half is backed by plain POSIX.
//
// HANDLE REPRESENTATION
//   HANDLE is an opaque pointer in the shim, so every handle here is a
//   pointer to a Handle object carrying a kind tag. That keeps CloseHandle
//   able to do the right thing for a thread, a mutex, an event or a directory
//   watch without the caller having to say which it has -- exactly the
//   property the Win32 callers assume.
//
// MODULE NAMES
//   Orbiter asks for modules by their Windows names ("Modules/ScnEditor.dll").
//   LoadLibrary translates: it tries the name as given, then with .dll swapped
//   for .so, then with a lib prefix. That way the config files and the module
//   directory listing keep working untouched.

#include <windows.h>
#include <commctrl.h>
#include <psapi.h>
#include <process.h>
#include <shlobj.h>

// The HANDLE object, CloseHandle and the last-error store used to live in this
// file. They were moved out so a standalone utility could link them without
// dragging in module loading, the registry and the console -- see Handles.h.
#include "Handles.h"

#include <pthread.h>
#include <dlfcn.h>
#include <link.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/inotify.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>   // TIOCGWINSZ, for the console buffer size
#include <sys/mman.h>    // mmap, for LOAD_LIBRARY_AS_DATAFILE
#include <link.h>        // ElfW
#include <poll.h>        // WaitForSingleObject on a directory-watch handle
#include <pty.h>         // openpty, for the session console
#include <signal.h>      // kill, for closing the console

#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <fstream>
#include <sstream>
#include <filesystem>


extern "C" DWORD orbiter_ConsoleProcessCount(void);
extern "C" void  orbiter_ShowConsoleWindow(int show);

// Sentinel returned by GetConsoleWindow. Not a real window: ShowWindow tests
// for this address and routes to the console rather than the dialog layer.
// External linkage so Win32Dlg.cpp can compare against it.
char g_consoleWindowToken = 0;

namespace {

// ---------------------------------------------------------------------------
// Handle objects
//
// The object is defined in Handles.h and destroyed in Handles.cpp, next to
// CloseHandle. The alias keeps every `new Handle{ Handle::Kind }` in this file
// reading exactly as it did.
// ---------------------------------------------------------------------------

using Handle = OrbHandle;

// The thread-local last-error store moved to Handles.cpp along with
// GetLastError. These forward to it, so the thirty-odd call sites below are
// unchanged.
inline void  setLastError(DWORD e)      { orb_SetLastError(e); }
inline DWORD win32ErrorFromErrno(int e) { return orb_Win32ErrorFromErrno(e); }

// The three standard streams are handed out as fixed pseudo-handles rather
// than allocated, so GetStdHandle can be called repeatedly and compared.
//
// The second initialiser is `pinned`: these five objects are not owned by the
// caller and CloseHandle must leave them alone. It used to recognise them by
// comparing addresses, which stopped being possible when it moved to
// Handles.cpp -- see the note on OrbHandle::pinned.
Handle g_stdIn  { Handle::StdStream, true };
Handle g_stdOut { Handle::StdStream, true };
Handle g_stdErr { Handle::StdStream, true };

// Pseudo-handle for the current process, likewise fixed.
Handle g_currentProcess { Handle::Process, true };

// Sentinel module standing for "the executable itself". LoadLibrary returns
// this for the Windows system libraries Orbiter probes for (Psapi.dll), whose
// entry points are provided by the shim and therefore live in the executable.
Handle g_selfModule { Handle::Module, true };


// Trampoline: Win32 thread routines return DWORD, pthreads return void*.
struct ThreadStart {
    DWORD (*fn)(void *);
    unsigned (*fnEx)(void *);   // _beginthreadex flavour
    void *param;
};

// Minimum stack for a thread created through CreateThread/_beginthreadex.
//
// The stack size a caller passes is sized for ITS OWN work, against Windows'
// behaviour of treating the value as a reserve that grows on demand. A
// pthread stack does not grow: the size given is all there is, and overrunning
// it faults.
//
// That difference is not theoretical here. LuaInline runs each vessel's script
// on its own thread, and the first thing a script does is resolve libc symbols
// through the PLT. glibc's lazy resolver saves the full extended CPU state on
// the stack -- _dl_runtime_resolve_xsavec -- which on AVX-512 hardware is
// several kilobytes on its own, before Lua has used any. The Delta-glider's
// ascent autopilot crashed inside _dl_lookup_symbol_x, not in Lua, for exactly
// this reason.
//
// 1 MB is well under the 8 MB default and comfortably above anything the
// resolver plus a Lua interpreter needs.
constexpr size_t kMinThreadStack = 1u << 20;

size_t threadStackSize(size_t requested)
{
    size_t want = requested ? requested : kMinThreadStack;
    if (want < kMinThreadStack)          want = kMinThreadStack;
    if (want < (size_t)PTHREAD_STACK_MIN) want = (size_t)PTHREAD_STACK_MIN;
    return want;
}

void *threadTrampoline(void *raw)
{
    ThreadStart *s = (ThreadStart *)raw;
    uintptr_t rc = 0;
    if (s->fn)        rc = s->fn(s->param);
    else if (s->fnEx) rc = s->fnEx(s->param);
    delete s;
    return (void *)rc;
}

// ---------------------------------------------------------------------------
// Registry backing store
//
// Orbiter reads and writes a couple of values under HKEY_CURRENT_USER. There
// is no registry here, so the same keys are persisted to a small ini file
// under the XDG config directory. Settings therefore survive restarts exactly
// as they do on Windows, without the call sites changing.
// ---------------------------------------------------------------------------

std::filesystem::path registryPath()
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    std::filesystem::path base;
    if (xdg && *xdg) base = xdg;
    else {
        const char *home = getenv("HOME");
        base = std::filesystem::path(home ? home : ".") / ".config";
    }
    return base / "orbiter" / "registry.ini";
}

std::map<std::string, std::string> &registryStore()
{
    static std::map<std::string, std::string> store;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        std::ifstream f(registryPath());
        std::string line;
        while (std::getline(f, line)) {
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            store[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
    return store;
}

void registryFlush()
{
    std::error_code ec;
    std::filesystem::create_directories(registryPath().parent_path(), ec);
    std::ofstream f(registryPath(), std::ios::trunc);
    for (const auto &kv : registryStore())
        f << kv.first << '=' << kv.second << '\n';
}

std::mutex g_registryLock;

// Open registry keys are represented by a Handle-like object holding the
// subkey prefix; values are stored flat with "subkey\\name" keys.
struct RegKey { std::string prefix; };
std::vector<RegKey *> g_regKeys;

} // namespace

// ===========================================================================
// Threads and synchronisation
// ===========================================================================

extern "C" {

HANDLE CreateThread(void *, SIZE_T stack, DWORD (*start)(void *), void *param,
                    DWORD, DWORD *tid)
{
    Handle *h = new Handle{ Handle::Thread };

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    // Always set the stack: see threadStackSize for why a caller-supplied
    // value cannot be trusted as-is on pthreads.
    pthread_attr_setstacksize(&attr, threadStackSize((size_t)stack));

    ThreadStart *s = new ThreadStart{ start, nullptr, param };
    int rc = pthread_create(&h->thread, &attr, threadTrampoline, s);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        delete s;
        delete h;
        setLastError(win32ErrorFromErrno(rc));
        return nullptr;
    }
    if (tid) *tid = (DWORD)(uintptr_t)h->thread;
    return (HANDLE)h;
}

BOOL TerminateThread(HANDLE thread, DWORD)
{
    Handle *h = (Handle *)thread;
    if (!h || h->kind != Handle::Thread) { setLastError(6); return FALSE; }
    // pthread_cancel is the closest equivalent. It is as unsafe as
    // TerminateThread is on Windows -- neither unwinds cleanly -- so callers
    // that rely on it are already accepting that risk.
    return pthread_cancel(h->thread) == 0 ? TRUE : FALSE;
}

uintptr_t _beginthreadex(void *, unsigned stack, unsigned (*start)(void *),
                         void *param, unsigned, unsigned *tid)
{
    Handle *h = new Handle{ Handle::Thread };

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, threadStackSize((size_t)stack));

    ThreadStart *s = new ThreadStart{ nullptr, start, param };
    int rc = pthread_create(&h->thread, &attr, threadTrampoline, s);
    pthread_attr_destroy(&attr);

    if (rc != 0) { delete s; delete h; return 0; }
    if (tid) *tid = (unsigned)(uintptr_t)h->thread;
    return (uintptr_t)h;
}

void _endthreadex(unsigned retval)
{
    pthread_exit((void *)(uintptr_t)retval);
}

HANDLE CreateMutexA(void *, BOOL initialOwner, LPCSTR)
{
    Handle *h = new Handle{ Handle::Mutex };
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    // Win32 mutexes are recursive for the owning thread; the default pthread
    // mutex is not, so this must be set or a re-entrant lock would deadlock.
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&h->mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    if (initialOwner) pthread_mutex_lock(&h->mutex);
    return (HANDLE)h;
}

BOOL ReleaseMutex(HANDLE mutex)
{
    Handle *h = (Handle *)mutex;
    if (!h || h->kind != Handle::Mutex) { setLastError(6); return FALSE; }
    return pthread_mutex_unlock(&h->mutex) == 0 ? TRUE : FALSE;
}

HANDLE CreateEventA(void *, BOOL manualReset, BOOL initialState, LPCSTR)
{
    Handle *h = new Handle{ Handle::Event };
    h->manualReset = manualReset != 0;
    h->signalled   = initialState != 0;
    pthread_mutex_init(&h->evLock, nullptr);
    pthread_cond_init(&h->cond, nullptr);
    return (HANDLE)h;
}

BOOL SetEvent(HANDLE ev)
{
    Handle *h = (Handle *)ev;
    if (!h || h->kind != Handle::Event) { setLastError(6); return FALSE; }
    pthread_mutex_lock(&h->evLock);
    h->signalled = true;
    // A manual-reset event releases everyone; an auto-reset event releases one
    // waiter, which then consumes the signal.
    if (h->manualReset) pthread_cond_broadcast(&h->cond);
    else                pthread_cond_signal(&h->cond);
    pthread_mutex_unlock(&h->evLock);
    return TRUE;
}

BOOL ResetEvent(HANDLE ev)
{
    Handle *h = (Handle *)ev;
    if (!h || h->kind != Handle::Event) { setLastError(6); return FALSE; }
    pthread_mutex_lock(&h->evLock);
    h->signalled = false;
    pthread_mutex_unlock(&h->evLock);
    return TRUE;
}

DWORD WaitForSingleObject(HANDLE obj, DWORD ms)
{
    Handle *h = (Handle *)obj;
    if (!h) { setLastError(6); return WAIT_FAILED; }

    switch (h->kind) {
    case Handle::Thread: {
        if (h->joined) return WAIT_OBJECT_0;
        if (ms == INFINITE) {
            pthread_join(h->thread, nullptr);
            h->joined = true;
            return WAIT_OBJECT_0;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += ms / 1000;
        ts.tv_nsec += (long)(ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        int rc = pthread_timedjoin_np(h->thread, nullptr, &ts);
        if (rc == 0) { h->joined = true; return WAIT_OBJECT_0; }
        return WAIT_TIMEOUT;
    }

    case Handle::Mutex: {
        // THREE CASES, and collapsing the third into the second is a data race.
        //
        // This used to be `INFINITE ? lock : trylock`, which gives a caller
        // that asked to wait one second a wait of ZERO. It then returns
        // WAIT_TIMEOUT while the caller -- which ignores the return value,
        // because on Windows the wait would have succeeded -- goes on to touch
        // the shared data anyway and to ReleaseMutex a mutex it does not own.
        //
        // Two live victims, both reading and writing one buffer:
        //
        //   console_ng.cpp:286  the console thread takes hMutex for 1000 ms,
        //                       writes cConsoleCmd, releases
        //   console_ng.cpp:76   ParseCmd takes hMutex for 1000 ms, copies
        //                       cConsoleCmd out and clears it, releases
        //
        // Whenever those two collide the loser proceeds regardless, so a
        // console command can be torn between the writer's strncpy and the
        // reader's strcpy. It works by accident exactly as often as the two
        // threads happen not to overlap.
        //
        // Interpreter::WaitExec(timeout) is the same shape and is currently
        // safe only because every call site takes the INFINITE default -- its
        // own comment says "Orbiter waits for the script for 1 second", so the
        // finite path is meant to be used and would have silently broken the
        // Lua ping-pong handoff the moment it was.
        //
        // ms == 0 really is a trylock on Windows, so that case is kept.
        if (ms == INFINITE)
            return pthread_mutex_lock(&h->mutex) == 0 ? WAIT_OBJECT_0 : WAIT_FAILED;
        if (ms == 0)
            return pthread_mutex_trylock(&h->mutex) == 0 ? WAIT_OBJECT_0 : WAIT_TIMEOUT;

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += ms / 1000;
        ts.tv_nsec += (long)(ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }

        // pthread_mutex_timedlock measures against CLOCK_REALTIME, which is
        // what clock_gettime was asked for above.
        const int rc = pthread_mutex_timedlock(&h->mutex, &ts);
        if (rc == 0)         return WAIT_OBJECT_0;
        if (rc == ETIMEDOUT) return WAIT_TIMEOUT;
        return WAIT_FAILED;
    }

    case Handle::Event: {
        pthread_mutex_lock(&h->evLock);
        DWORD result = WAIT_OBJECT_0;
        if (!h->signalled) {
            if (ms == INFINITE) {
                while (!h->signalled)
                    pthread_cond_wait(&h->cond, &h->evLock);
            } else {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec  += ms / 1000;
                ts.tv_nsec += (long)(ms % 1000) * 1000000L;
                if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
                while (!h->signalled) {
                    if (pthread_cond_timedwait(&h->cond, &h->evLock, &ts) != 0) {
                        result = WAIT_TIMEOUT;
                        break;
                    }
                }
            }
        }
        if (result == WAIT_OBJECT_0 && !h->manualReset)
            h->signalled = false;   // auto-reset consumes the signal
        pthread_mutex_unlock(&h->evLock);
        return result;
    }

    case Handle::Watch: {
        // A directory-change handle. ScenarioTab::threadWatchScnList blocks
        // here with INFINITE and refreshes the scenario list on WAIT_OBJECT_0:
        //
        //     dwWaitStatus = WaitForSingleObject (dwChangeHandle, INFINITE);
        //     switch (dwWaitStatus) { case WAIT_OBJECT_0: RefreshList(true); ... }
        //
        // Falling through to WAIT_FAILED, as this did before, makes that loop
        // spin at full speed forever -- a whole core burnt for nothing -- and
        // the list never refreshes when a scenario is added or removed.
        //
        // The inotify fd is non-blocking, so poll() supplies the wait.
        struct pollfd pfd;
        pfd.fd     = h->fd;
        pfd.events = POLLIN;

        const int timeout = (ms == INFINITE) ? -1 : (int)ms;
        const int rc = poll(&pfd, 1, timeout);

        if (rc < 0)  return WAIT_FAILED;
        if (rc == 0) return WAIT_TIMEOUT;
        return WAIT_OBJECT_0;
    }

    default:
        return WAIT_FAILED;
    }
}

// CloseHandle is in Handles.cpp, with the object it destroys.

// ===========================================================================
// Process
// ===========================================================================

HANDLE GetCurrentProcess(void)   { return (HANDLE)&g_currentProcess; }
DWORD  GetCurrentProcessId(void) { return (DWORD)getpid(); }
DWORD  GetCurrentThreadId(void)  { return (DWORD)(uintptr_t)pthread_self(); }

// The Win32 current-thread pseudo-handle, value for value. See the long note
// at the declaration in windows.h: this is deliberately NOT a per-thread
// identity, because the two guards in the graphics client compare its result
// and both comparisons must keep succeeding here exactly as they do on
// Windows. -2 is the documented constant.
HANDLE GetCurrentThread(void)    { return (HANDLE)(intptr_t)-2; }

HANDLE OpenProcess(DWORD, BOOL, DWORD pid)
{
    // Only the current process is ever opened by this tree.
    if ((pid_t)pid == getpid()) return (HANDLE)&g_currentProcess;
    Handle *h = new Handle{ Handle::Process };
    h->pid = (pid_t)pid;
    return (HANDLE)h;
}

DWORD GetProcessId(HANDLE process)
{
    Handle *h = (Handle *)process;
    if (!h) return 0;
    if (h == &g_currentProcess) return (DWORD)getpid();
    return (DWORD)h->pid;
}

// GetLastError is in Handles.cpp, with the thread-local store behind it.

DWORD GetVersion(void)
{
    // Windows encodes major in the low byte, minor in the next, and clears the
    // top bit for NT. Nothing in this tree branches on the value; it only ever
    // reaches the log header, so a stable synthetic version is reported.
    return 0x0A00;   // "10.0"
}

// ===========================================================================
// Dynamic module loading
// ===========================================================================

namespace {

// THE LOADER'S INDEX OF WHAT IS ALREADY LOADED, KEYED BY BASE NAME.
//
// Windows' LoadLibrary does not begin with a file search. It resolves the name
// to a base name -- appending the default ".dll" when there is no extension --
// and if a module with that base name is ALREADY IN THE PROCESS it returns
// that module's handle with its reference count incremented, having touched no
// directory at all. Only a name that matches nothing loaded is searched for on
// disk.
//
// dlopen has no such rule: a name with no slash is looked for along the
// system search path, and "DeltaGlider" is not there under any name, so the
// call simply failed.
//
// THAT IS WHAT KILLED THE SCENARIO EDITOR'S VESSEL PAGES. A vessel names its
// editor extension in its class config as a bare module name --
//
//     Src/Vessel/DeltaGlider/Config/Deltaglider.cfg
//         Module       = DeltaGlider
//         EditorModule = DeltaGlider
//
// -- and ScnEditor::LoadVesselLibrary (Editor.cpp:319) passes it straight to
// LoadLibrary. On Windows that is a hit every time, because the SAME module
// is already loaded: Vessel::RegisterModule loaded "Modules/DeltaGlider.dll"
// to create the vessel in the first place. Here it returned null, so
// EditorTab_Edit::InitTab never found secInit, and the Delta-glider's
// Animations, Passengers and Damage pages -- and the Shuttle-A's -- never
// appeared on the editor's Edit tab.
//
// The index is keyed on the stem, lower-cased, because Windows matches base
// names without regard to case and because this shim maps ".dll" to ".so":
// "Modules/DeltaGlider.so" and "DeltaGlider" have to answer to each other.
std::map<std::string, std::string> &loadedModulePaths()
{
    static std::map<std::string, std::string> m;
    return m;
}

std::string moduleStem(const std::string &path)
{
    std::string s = std::filesystem::path(path).stem().string();
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

} // namespace

HMODULE orb_LoadLibrary(LPCSTR name, int bindMode)
{
    if (!name) { setLastError(87); return nullptr; }

    const std::string given(name);

    // The Windows system libraries Orbiter probes for are provided by the shim
    // itself, so they resolve to the executable rather than to a real object.
    if (strcasecmp(given.c_str(), "Psapi.dll") == 0 ||
        strcasecmp(given.c_str(), "user32.dll") == 0 ||
        strcasecmp(given.c_str(), "kernel32.dll") == 0)
        return (HMODULE)&g_selfModule;

    // ALREADY LOADED WINS, BEFORE ANY SEARCH -- see loadedModulePaths above.
    // The probe is RTLD_NOLOAD, which never loads and reports only whether the
    // object is resident, and it takes a real reference so the Handle returned
    // here can be released by FreeLibrary exactly as any other.
    {
        auto &idx = loadedModulePaths();
        const auto it = idx.find(moduleStem(given));
        if (it != idx.end()) {
            void *dl = dlopen(it->second.c_str(),
                              bindMode | RTLD_LOCAL | RTLD_NOLOAD);
            if (dl) {
                Handle *h = new Handle{ Handle::Module };
                h->dlHandle = dl;
                h->path     = it->second;
                if (getenv("ORBITER_TRACE_MODULES"))
                    fprintf(stderr, "[mod] LoadLibrary(%s) -> already loaded %s\n",
                            name, it->second.c_str());
                return (HMODULE)h;
            }
            // Gone since it was recorded: forget it and search properly.
            idx.erase(it);
        }
    }

    // Try the name as given, then the same path with .dll swapped for .so,
    // then with a lib prefix. Orbiter's module directory and config files name
    // modules the Windows way, and this is what keeps them working unedited.
    std::vector<std::string> candidates{ given };

    std::filesystem::path p(given);
    if (p.extension() == ".dll" || p.extension() == ".DLL") {
        std::filesystem::path so = p;
        so.replace_extension(".so");
        candidates.push_back(so.string());
        candidates.push_back((so.parent_path() /
                              ("lib" + so.filename().string())).string());
    } else if (p.extension().empty()) {
        // The default extension. LoadLibrary appends ".dll" to a name that has
        // none -- that is why "DeltaGlider" and "DeltaGlider.dll" are the same
        // request on Windows -- so the same name gets ".so" here, with the lib
        // prefix form for a module built as a shared library.
        candidates.push_back(given + ".so");
        candidates.push_back((p.parent_path() /
                              ("lib" + p.filename().string() + ".so")).string());
    }

    // A bare relative path is resolved against the working directory by
    // dlopen only when it contains a slash; "Modules/Plugin/X.so" does, so it
    // works, but the absolute form is tried as well so a caller that has
    // changed directory still finds it.
    std::string firstErr;
    for (const std::string &c : candidates) {
        void *dl = dlopen(c.c_str(), bindMode | RTLD_LOCAL);
        if (!dl && firstErr.empty()) {
            const char *e = dlerror();
            if (e) firstErr = e;
        }
        if (dl) {
            Handle *h = new Handle{ Handle::Module };
            h->dlHandle = dl;
            h->path     = c;
            // Record it under its base name so a later bare-name request finds
            // it, which is the loader index Windows keeps. The first load of a
            // given base name wins, as it does there.
            loadedModulePaths().emplace(moduleStem(c), c);
            return (HMODULE)h;
        }
    }

    if (getenv("ORBITER_TRACE_MODULES"))
        fprintf(stderr, "[mod] LoadLibrary(%s) FAILED: %s\n",
                name, firstErr.empty() ? "?" : firstErr.c_str());
    setLastError(2);   // ERROR_FILE_NOT_FOUND
    return nullptr;
}

// Defined in Src/Orbiter/OrbiterAPI.cpp, in this same executable. See the
// block comment at FreeLibrary for why the loader calls them directly.
//
// extern "C++" IS NOT DECORATION. This file is a Win32 shim and much of it
// sits inside extern "C" for the API it emulates; a bare declaration here
// inherits that and asks the linker for the unmangled `InitLib`, which is
// exactly the error it produced:
//
//     Platform.cpp:665: undefined reference to `InitLib'
//
// while the executable exports the C++-mangled _Z7InitLibP11HINSTANCE__.
// Orbitersdk.cpp carries the same warning at its own declaration of these two.
extern "C++" {
	void InitLib(HINSTANCE hModule);
	void ExitLib(HINSTANCE hModule);
}

namespace {

// Paths this process dlclosed while the object stayed mapped.
std::set<std::string> &staleModules()
{
    static std::set<std::string> s;
    return s;
}

// Is the object at this path still mapped into the process?
//
// RTLD_NOLOAD never loads; it only reports. It DOES take a reference on a
// resident object, which is dropped again immediately -- the same ownership
// rule stated at GetModuleHandleA.
bool moduleResident(const std::string &path)
{
    if (path.empty()) return false;
    void *probe = dlopen(path.c_str(), RTLD_LAZY | RTLD_NOLOAD);
    if (!probe) return false;
    dlclose(probe);
    return true;
}

} // namespace

// Run InitLib by hand when the ELF constructor could not.
//
// See the block comment at FreeLibrary. A path is in staleModules() only if
// this process dlclosed it and the object refused to leave, which is exactly
// the case where a fresh dlopen runs no constructor.
static void orb_ReinitIfStale(HMODULE mod)
{
    Handle *h = (Handle *)mod;
    if (!h || h == &g_selfModule || h->mapBase || h->path.empty()) return;

    auto &stale = staleModules();
    auto it = stale.find(h->path);
    if (it == stale.end()) return;
    stale.erase(it);

    if (getenv("ORBITER_TRACE_MODULES"))
        fprintf(stderr, "[mod] %s was resident; calling InitLib explicitly\n",
                h->path.c_str());
    InitLib(mod);
}

HMODULE LoadLibraryA(LPCSTR name)
{
    // A plain LoadLibrary is an executable load: initialisers run and imports
    // must bind, which is RTLD_NOW.
    HMODULE m = orb_LoadLibrary(name, RTLD_NOW);
    if (m) orb_ReinitIfStale(m);
    return m;
}

HMODULE LoadLibraryExA(LPCSTR name, HANDLE, DWORD flags)
{
    // LOAD_LIBRARY_AS_DATAFILE means "map this for its resources only": do not
    // run initialisers, do not resolve imports. The dlopen equivalent is
    // RTLD_LAZY, not RTLD_NOW.
    //
    // This matters, and getting it wrong is what cross-assigned every module
    // category in the Launchpad. ModuleTab::RefreshLists reads each plugin's
    // category with
    //
    //     HMODULE hMod = LoadLibraryEx(path, 0, LOAD_LIBRARY_AS_DATAFILE);
    //     if (hMod) { ... LoadString(hMod, 1001, buf, 1024) ... }
    //
    // and `catstr` is declared ONCE outside the loop, so when hMod comes back
    // null the module silently inherits the previous directory entry's
    // category. That is an upstream bug, invisible on Windows because a
    // datafile load there does not resolve imports and so cannot fail this
    // way.
    //
    // With RTLD_NOW it fails here constantly: a plugin leaves the Orbiter core
    // symbols undefined by design and expects them bound from the executable,
    // and any that cannot be satisfied at load time aborts the whole dlopen.
    // RTLD_LAZY defers function binding to first call -- which never comes,
    // because all that is read is a data symbol.
    const bool asDataFile = (flags & LOAD_LIBRARY_AS_DATAFILE) != 0;

    if (asDataFile) {
        // Map the file and read its symbols; run nothing. See
        // orb_DatafileSymbol above for why dlopen is wrong here.
        std::vector<std::string> cands{ std::string(name) };
        std::filesystem::path p(name);
        if (p.extension() == ".dll" || p.extension() == ".DLL") {
            std::filesystem::path so = p;
            so.replace_extension(".so");
            cands.push_back(so.string());
        }
        for (const std::string &c : cands) {
            const int fd = ::open(c.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            struct stat st;
            if (fstat(fd, &st) != 0 || st.st_size <= 0) { ::close(fd); continue; }
            void *m = mmap(nullptr, (size_t)st.st_size, PROT_READ,
                           MAP_PRIVATE, fd, 0);
            ::close(fd);
            if (m == MAP_FAILED) continue;

            Handle *h = new Handle{ Handle::Module };
            h->path    = c;
            h->mapBase = m;
            h->mapSize = (size_t)st.st_size;
            if (getenv("ORBITER_TRACE_MODULES"))
                fprintf(stderr, "[mod] datafile map %s -> %p\n",
                        c.c_str(), m);
            return (HMODULE)h;
        }
        setLastError(2);   // ERROR_FILE_NOT_FOUND
        return nullptr;
    }

    HMODULE m = orb_LoadLibrary(name, RTLD_NOW);
    if (getenv("ORBITER_TRACE_MODULES"))
        fprintf(stderr, "[mod] LoadLibraryEx(%s) -> %s%s\n",
                name ? name : "(null)", m ? "ok" : "FAILED: ",
                m ? "" : (dlerror() ? dlerror() : "?"));
    return m;
}

// Wrap an existing dl handle as an HMODULE.
//
// Orbitersdk's ELF constructor needs a module handle for InitLib, but it runs
// while its own library is still initialising, so it cannot call LoadLibrary
// on itself -- the loader refuses and returns null. It resolves its own path
// and dl handle with dladdr and RTLD_NOLOAD instead, neither of which loads
// anything, and hands them here to be wrapped in the Handle the rest of the
// shim expects.
//
// Ownership stays with the loader: this Handle is not freed by FreeLibrary,
// because the module did not take a reference it needs to drop.
extern "C" HMODULE orbiter_WrapDlHandle(void *dl, const char *path)
{
    if (!dl) return nullptr;
    Handle *h = new Handle{ Handle::Module };
    h->dlHandle = dl;
    h->path     = path ? path : "";
    return (HMODULE)h;
}

// ===========================================================================
// MODULES THAT dlclose CANNOT UNLOAD, AND WHAT THAT BREAKS
// ===========================================================================
//
// A shared object with a TLS segment is pinned by glibc: once a thread has
// touched its thread-local storage the loader will not unmap it, and dlclose
// becomes a reference-count decrement and nothing more. It returns 0 either
// way, so the caller cannot tell. VulkanClient.so is such an object -- readelf
// shows a TLS program header of 0x90 bytes and ten TLS symbols, most of them
// arriving through VSG and libstdc++ rather than from any code here -- and so
// are many plugins.
//
// Two things hang off the ELF constructor and destructor in
// Src/Orbitersdk/Orbitersdk.cpp, and a module that never leaves memory gets
// neither:
//
//   * the CONSTRUCTOR calls InitLib, which logs the module's build line and
//     calls its InitModule. Orbiter::LoadModule watches `register_module`,
//     which InitModule is what sets. A second dlopen of a resident object
//     returns the old handle and runs no constructor, so register_module
//     stayed null and Orbiter logged "Loading module X (legacy interface)" --
//     a module with no clbkSimulationStart, no clbkPreStep, and for a
//     graphics client, no registration at all.
//
//   * the DESTRUCTOR calls the module's ExitModule, which is where a client
//     unregisters itself and frees what it owns. Never running it means the
//     old instance stays registered with the core after the module has
//     supposedly been unloaded.
//
// Measured on the Launchpad's Video tab: choosing "Console mode" and then the
// graphics engine again left VulkanClient loaded but demoted to legacy and
// dropped from ACTIVE_MODULES, so the NEXT launch came up with no graphics
// client and nothing anywhere said why.
//
// THE FIX IS SYMMETRIC AND LIVES HERE, because this is the layer that knows
// whether the object actually went away:
//
//   FreeLibrary  dlclose, then probe. Still resident means the destructor did
//                not run, so ExitLib is called explicitly and the path is
//                remembered as stale.
//   LoadLibrary  dlopen, then check that memory. A stale path means the
//                constructor did not run, so InitLib is called explicitly.
//
// The bookkeeping is what keeps this from double-initialising: InitLib is
// called ONLY for a path this process previously unloaded, never for one that
// is resident because something else legitimately holds it.
//
// Windows needs none of this because FreeLibrary there really does unload, so
// the DllMain pair fires once per logical load. The observable behaviour is
// now the same on both.
// ===========================================================================

BOOL FreeLibrary(HMODULE mod)
{
    Handle *h = (Handle *)mod;
    if (!h) { setLastError(6); return FALSE; }
    if (h == &g_selfModule) return TRUE;

    if (h->mapBase) {
        munmap(h->mapBase, h->mapSize);               // datafile module
        if (getenv("ORBITER_TRACE_MODULES"))
            fprintf(stderr, "[mod] unmap datafile %s\n", h->path.c_str());
    }
    else if (h->dlHandle) {
        dlclose(h->dlHandle);

        if (moduleResident(h->path)) {
            // The ELF destructor did not run and will not run while the object
            // stays mapped. Do its job before the handle is destroyed -- it is
            // still a valid dlsym target, because the object is still there.
            staleModules().insert(h->path);
            ExitLib((HINSTANCE)h);
            if (getenv("ORBITER_TRACE_MODULES"))
                fprintf(stderr, "[mod] dlclose %s -> STILL RESIDENT, "
                                "ExitLib called explicitly\n", h->path.c_str());
        }
        else if (getenv("ORBITER_TRACE_MODULES")) {
            fprintf(stderr, "[mod] dlclose %s -> unloaded\n", h->path.c_str());
        }
    }
    delete h;
    return TRUE;
}


// ---------------------------------------------------------------------------
// Datafile modules
//
// LOAD_LIBRARY_AS_DATAFILE maps an image so its resources can be read WITHOUT
// running any of its code: no DllMain, no imports bound. ModuleTab::RefreshLists
// relies on that -- it opens every plugin in turn purely to read the two
// strings that give its description and category, and expects the module to be
// untouched afterwards.
//
// Mapping the flag onto dlopen breaks that promise, because dlopen runs ELF
// constructors. The visible symptom was XRSound: its constructor ran during
// the scan and called oapiRegisterModule, Orbiter::LoadModule then cleared
// register_module and dlopened a library that was ALREADY loaded -- so no
// constructor ran the second time, register_module stayed null, and the module
// was logged and driven as "(legacy interface)". It therefore never received
// clbkSimulationStart, which is where XRSound initialises irrKlang, so it
// played nothing.
//
// The honest implementation reads the ELF directly. Both strings are ordinary
// exported data symbols, so finding them needs only the dynamic symbol table
// and a vaddr-to-file-offset conversion through the PT_LOAD headers.
// ---------------------------------------------------------------------------

namespace {

void *orb_DatafileSymbol(Handle *h, const char *name)
{
    const unsigned char *base = (const unsigned char *)h->mapBase;
    const ElfW(Ehdr) *eh = (const ElfW(Ehdr) *)base;

    if (h->mapSize < sizeof(ElfW(Ehdr)) || memcmp(eh->e_ident, ELFMAG, SELFMAG))
        return nullptr;

    const ElfW(Shdr) *sh = (const ElfW(Shdr) *)(base + eh->e_shoff);
    if (eh->e_shoff == 0 || eh->e_shnum == 0) return nullptr;

    // Locate .dynsym and its string table.
    const ElfW(Sym) *syms = nullptr;
    const char *strs = nullptr;
    size_t nsym = 0;
    for (unsigned i = 0; i < eh->e_shnum; ++i) {
        if (sh[i].sh_type != SHT_DYNSYM) continue;
        syms = (const ElfW(Sym) *)(base + sh[i].sh_offset);
        nsym = sh[i].sh_entsize ? sh[i].sh_size / sh[i].sh_entsize : 0;
        if (sh[i].sh_link < eh->e_shnum)
            strs = (const char *)(base + sh[sh[i].sh_link].sh_offset);
        break;
    }
    if (!syms || !strs) return nullptr;

    for (size_t i = 0; i < nsym; ++i) {
        if (!syms[i].st_value || syms[i].st_shndx == SHN_UNDEF) continue;
        const char *sn = strs + syms[i].st_name;
        if (strcmp(sn, name) != 0) continue;

        // st_value is a virtual address. Translate it to a file offset using
        // the PT_LOAD segment that contains it -- the mapping here is the
        // plain file, not the loader's layout, so the two differ.
        const ElfW(Phdr) *ph = (const ElfW(Phdr) *)(base + eh->e_phoff);
        for (unsigned k = 0; k < eh->e_phnum; ++k) {
            if (ph[k].p_type != PT_LOAD) continue;
            if (syms[i].st_value <  ph[k].p_vaddr) continue;
            if (syms[i].st_value >= ph[k].p_vaddr + ph[k].p_filesz) continue;
            const size_t off = ph[k].p_offset +
                               (syms[i].st_value - ph[k].p_vaddr);
            if (off >= h->mapSize) return nullptr;
            return (void *)(base + off);
        }
        return nullptr;
    }
    return nullptr;
}

} // namespace

void *GetProcAddress(HMODULE mod, LPCSTR name)
{
    if (!name) { setLastError(87); return nullptr; }

    Handle *h = (Handle *)mod;

    // For the self module, and for a null module, look the symbol up in the
    // executable's own dynamic symbol table. This is why the Orbiter target is
    // linked with -rdynamic: without it these lookups return null.
    if (!h || h == &g_selfModule)
        return dlsym(RTLD_DEFAULT, name);

    // A datafile module has no dl handle: the image is mapped read-only and
    // its symbols are read straight out of the ELF, because
    // LOAD_LIBRARY_AS_DATAFILE promises that none of its code runs.
    if (h->mapBase)
        return orb_DatafileSymbol(h, name);

    return dlsym(h->dlHandle, name);
}

HMODULE GetModuleHandleA(LPCSTR name)
{
    // A null name means "the executable", which is exactly the self module.
    if (!name) return (HMODULE)&g_selfModule;

    // NOT LOADED MEANS NULL. Windows returns NULL when the named module is
    // not already in the process, and that is the whole point of the call --
    // it is an existence test, and the only way to make one without loading
    // anything. Answering with the self module says "yes, present" for every
    // name ever asked about.
    //
    // THE LOADER INDEX IS CONSULTED FIRST, exactly as orb_LoadLibrary does.
    //
    // A raw dlopen(name, RTLD_NOLOAD) cannot answer this question for an
    // Orbiter module. Modules are loaded through orb_LoadLibrary as
    // "Modules/Plugin/<Name>.so" and with RTLD_LOCAL, and dlopen matches on
    // the name a library was opened with (or its soname) -- so a bare
    // "VulkanClient", or the Windows spelling "VulkanClient.dll", matches
    // nothing at all even though the object is resident.
    //
    // That is not hypothetical: gcCore.h's gcGetCoreInterface() asks for the
    // graphics client by name to fetch gcBindCoreMethod from it, and every
    // add-on that draws through gcCore depends on it. Before this it always
    // returned NULL and the log said "gcGetCoreInterface() FAILED".
    //
    // loadedModulePaths() is keyed on the lower-cased stem, which is what
    // makes "VulkanClient", "VulkanClient.dll" and "Modules/Plugin/
    // VulkanClient.so" one another's answers -- the same base-name matching
    // Windows' loader does.
    std::string path(name);
    {
        auto &idx = loadedModulePaths();
        const auto it = idx.find(moduleStem(path));
        if (it != idx.end()) path = it->second;
    }

    void *dl = dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
    if (!dl) { setLastError(126); return nullptr; }   // ERROR_MOD_NOT_FOUND

    // RTLD_NOLOAD still takes a reference on a module that IS loaded, and
    // GetModuleHandle does not transfer ownership -- the caller never calls
    // FreeLibrary on the result -- so the reference is dropped here.
    dlclose(dl);

    Handle *h = new Handle{ Handle::Module };
    h->dlHandle = dl;
    h->path     = path;
    return (HMODULE)h;
}

DWORD GetModuleFileNameA(HMODULE mod, LPSTR filename, DWORD size)
{
    if (!filename || size == 0) { setLastError(87); return 0; }

    Handle *h = (Handle *)mod;

    if (!h || h == &g_selfModule) {
        ssize_t n = readlink("/proc/self/exe", filename, size - 1);
        if (n < 0) { setLastError(win32ErrorFromErrno(errno)); return 0; }
        filename[n] = '\0';
        return (DWORD)n;
    }

    // For a loaded object, dladdr on any of its symbols gives the path; the
    // recorded path from LoadLibrary is simpler and always correct here.
    snprintf(filename, size, "%s", h->path.c_str());
    return (DWORD)strnlen(filename, size);
}

// ===========================================================================
// Process and module inspection (psapi)
// ===========================================================================

BOOL GetProcessMemoryInfo(HANDLE, PPROCESS_MEMORY_COUNTERS counters, DWORD cb)
{
    if (!counters || cb < sizeof(PROCESS_MEMORY_COUNTERS)) {
        setLastError(87);
        return FALSE;
    }

    // Windows fills whichever of the two structures it is handed and tells
    // them apart by 'cb' alone -- the caller casts the EX form down to
    // PPROCESS_MEMORY_COUNTERS and passes its own sizeof. Same test here, so
    // the call site needs no edit and neither does any existing caller of the
    // base form. See psapi.h.
    const bool bEx = (cb >= sizeof(PROCESS_MEMORY_COUNTERS_EX));

    memset(counters, 0, bEx ? sizeof(PROCESS_MEMORY_COUNTERS_EX)
                            : sizeof(PROCESS_MEMORY_COUNTERS));
    counters->cb = bEx ? (DWORD)sizeof(PROCESS_MEMORY_COUNTERS_EX)
                       : (DWORD)sizeof(PROCESS_MEMORY_COUNTERS);

    // /proc/self/statm reports sizes in pages: total, resident, shared, text,
    // lib, data, dirty. Resident is the working set.
    std::ifstream f("/proc/self/statm");
    unsigned long total = 0, resident = 0;
    if (f >> total >> resident) {
        const long pageSize = sysconf(_SC_PAGESIZE);
        counters->WorkingSetSize     = (SIZE_T)resident * (SIZE_T)pageSize;
        counters->PagefileUsage      = (SIZE_T)total    * (SIZE_T)pageSize;
        counters->PeakWorkingSetSize = counters->WorkingSetSize;

        if (bEx) {
            // Fields three to six: shared, text, lib, data. 'data' is
            // VmData + VmStk -- the private writable address space, which is
            // what PrivateUsage names. Not the resident set; WorkingSetSize
            // above is already that.
            unsigned long shared = 0, text = 0, lib = 0, data = 0;
            if (f >> shared >> text >> lib >> data)
                ((PPROCESS_MEMORY_COUNTERS_EX)counters)->PrivateUsage =
                    (SIZE_T)data * (SIZE_T)pageSize;
        }
    }
    return TRUE;
}

namespace {

struct ModuleScan {
    HMODULE *out;
    size_t   capacity;
    size_t   found;
};

// Handles handed out by EnumProcessModules, CACHED BY LOAD ADDRESS.
//
// Two reasons this is not a fresh allocation per entry, which is what it used
// to be:
//
//   * Nothing frees them. Windows' EnumProcessModules hands back handles the
//     caller does not own and must not close, so PrintModules -- the only
//     caller -- correctly closes none of them. Allocating per call therefore
//     leaked one Handle per loaded object per call: 84 of them per session
//     today, and unbounded for anything that ever enumerated per frame.
//
//   * Windows guarantees the same module yields the same HMODULE, and code
//     compares them. A new pointer each time silently breaks that.
std::map<uintptr_t, Handle *> g_enumModuleCache;

int collectModule(struct dl_phdr_info *info, size_t, void *data)
{
    ModuleScan *scan = (ModuleScan *)data;

    if (scan->found < scan->capacity && scan->out) {
        const uintptr_t base = (uintptr_t)info->dlpi_addr;

        Handle *&h = g_enumModuleCache[base];
        if (!h) {
            h = new Handle{ Handle::Module };
            h->dlHandle = nullptr;

            // AN EMPTY dlpi_name IS THE MAIN EXECUTABLE, not a nameless
            // object. dl_iterate_phdr reports the program itself first with
            // an empty name, and GetModuleFileNameA then returned 0 for it --
            // which PrintModules reads as failure and skips, so Orbiter's own
            // binary was missing from the module list it prints to diagnose
            // bad add-ons. Windows lists the .exe first.
            if (info->dlpi_name && *info->dlpi_name) {
                h->path = info->dlpi_name;
            } else {
                char exe[PATH_MAX];
                const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
                if (n > 0) { exe[n] = '\0'; h->path = exe; }
            }

            // The load address and image size, for GetModuleInformation.
            // Neither was ever filled: the handle carried no dl handle, so
            // lpBaseOfDll stayed null and SizeOfImage stayed zero, and every
            // one of the 84 lines PrintModules writes said "Size=0".
            //
            // The image spans the PT_LOAD segments: base + the highest
            // p_vaddr+p_memsz, which is what Windows means by SizeOfImage.
            h->modBase = (void *)info->dlpi_addr;
            uintptr_t end = 0;
            for (int k = 0; k < info->dlpi_phnum; ++k) {
                const ElfW(Phdr) &ph = info->dlpi_phdr[k];
                if (ph.p_type != PT_LOAD) continue;
                const uintptr_t segEnd = (uintptr_t)ph.p_vaddr + ph.p_memsz;
                if (segEnd > end) end = segEnd;
            }
            h->modSize = (size_t)end;
        }

        scan->out[scan->found] = (HMODULE)h;
    }
    scan->found++;
    return 0;
}

} // namespace

BOOL EnumProcessModules(HANDLE, HMODULE *modules, DWORD cb, LPDWORD needed)
{
    ModuleScan scan{ modules, cb / sizeof(HMODULE), 0 };
    dl_iterate_phdr(collectModule, &scan);
    if (needed) *needed = (DWORD)(scan.found * sizeof(HMODULE));
    return TRUE;
}

BOOL GetModuleInformation(HANDLE, HMODULE mod, LPMODULEINFO info, DWORD cb)
{
    if (!info || cb < sizeof(MODULEINFO)) { setLastError(87); return FALSE; }
    memset(info, 0, sizeof(*info));

    Handle *h = (Handle *)mod;
    if (!h) return TRUE;

    // A handle from EnumProcessModules carries its own measurements, taken
    // from the program headers at scan time. This is the path PrintModules
    // takes, and before it existed every module it logged read "Size=0".
    if (h->modBase || h->modSize) {
        info->lpBaseOfDll = (LPVOID)h->modBase;
        info->SizeOfImage = (DWORD)h->modSize;
        return TRUE;
    }

    if (h->dlHandle) {
        struct link_map *lm = nullptr;
        if (dlinfo(h->dlHandle, RTLD_DI_LINKMAP, &lm) == 0 && lm)
            info->lpBaseOfDll = (LPVOID)lm->l_addr;
    }
    return TRUE;
}

DWORD GetModuleFileNameExA(HANDLE, HMODULE mod, LPSTR name, DWORD size)
{
    return GetModuleFileNameA(mod, name, size);
}

// ===========================================================================
// Error reporting
// ===========================================================================

DWORD FormatMessageA(DWORD flags, LPCVOID, DWORD msgId, DWORD,
                     LPSTR buf, DWORD size, va_list *)
{
    // Only FORMAT_MESSAGE_FROM_SYSTEM is used, to turn a GetLastError value
    // into readable text. The Win32 codes mapped in win32ErrorFromErrno are
    // translated back; anything else is reported numerically rather than
    // guessed at.
    const char *text;
    switch (msgId) {
    case 2:   text = "The system cannot find the file specified."; break;
    case 3:   text = "The system cannot find the path specified."; break;
    case 5:   text = "Access is denied."; break;
    case 6:   text = "The handle is invalid."; break;
    case 8:   text = "Not enough memory is available."; break;
    case 87:  text = "The parameter is incorrect."; break;
    case 183: text = "The file already exists."; break;
    default:  text = nullptr; break;
    }

    std::string msg;
    if (text) msg = text;
    else {
        // Fall back to strerror for a value that came straight from errno.
        const char *se = strerror((int)msgId);
        msg = se ? se : "Unknown error";
        msg += " (" + std::to_string(msgId) + ")";
    }

    if (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) {
        // The caller receives ownership through the LPSTR* it passed and frees
        // it with LocalFree, so this allocates with malloc to match.
        char **out = (char **)buf;
        if (!out) return 0;
        *out = strdup(msg.c_str());
        return (DWORD)msg.size();
    }

    if (!buf || size == 0) return 0;
    snprintf(buf, size, "%s", msg.c_str());
    return (DWORD)strnlen(buf, size);
}

HLOCAL LocalFree(LPVOID mem)
{
    free(mem);
    return nullptr;
}

void OutputDebugStringA(LPCSTR str)
{
    if (str) fputs(str, stderr);
}

// ===========================================================================
// Console
// ===========================================================================

HANDLE GetStdHandle(DWORD which)
{
    switch (which) {
    case STD_INPUT_HANDLE:  g_stdIn.fd  = STDIN_FILENO;  return (HANDLE)&g_stdIn;
    case STD_OUTPUT_HANDLE: g_stdOut.fd = STDOUT_FILENO; return (HANDLE)&g_stdOut;
    case STD_ERROR_HANDLE:  g_stdErr.fd = STDERR_FILENO; return (HANDLE)&g_stdErr;
    default: setLastError(87); return nullptr;
    }
}

BOOL SetConsoleTitleA(LPCSTR title)
{
    // xterm-compatible window title sequence. Terminals that do not understand
    // it ignore it, which is the same practical outcome as the Windows call on
    // a process with no console.
    if (title && isatty(STDOUT_FILENO))
        printf("\033]0;%s\007", title);
    return TRUE;
}

HWND GetConsoleWindow(void)
{
    // ConsoleManager::ShowConsole does
    //     HWND wnd = GetConsoleWindow();
    //     if (wnd) ShowWindow(wnd, show ? SW_SHOW : SW_HIDE);
    // so returning null made it a no-op and the console never appeared.
    //
    // A non-null sentinel is returned instead: it is not a real window, and
    // ShowWindow below recognises it and routes to the console implementation
    // rather than treating it as a dialog. DialogManager also takes this as
    // its parent in NG mode -- Orbiter.cpp:740 -- where it means "no parent".
    return (HWND)&g_consoleWindowToken;
}

BOOL SetConsoleMode(HANDLE h, DWORD mode)
{
    Handle *hh = (Handle *)h;
    if (!hh || hh->fd < 0 || !isatty(hh->fd)) { setLastError(6); return FALSE; }

    struct termios t;
    if (tcgetattr(hh->fd, &t) != 0) { setLastError(win32ErrorFromErrno(errno)); return FALSE; }

    // ENABLE_LINE_INPUT is canonical mode; ENABLE_ECHO_INPUT is echo. Clearing
    // them is how Orbiter puts the console into raw single-key mode.
    if (mode & ENABLE_LINE_INPUT) t.c_lflag |= ICANON; else t.c_lflag &= ~ICANON;
    if (mode & ENABLE_ECHO_INPUT) t.c_lflag |= ECHO;   else t.c_lflag &= ~ECHO;

    return tcsetattr(hh->fd, TCSANOW, &t) == 0 ? TRUE : FALSE;
}

BOOL GetConsoleMode(HANDLE h, DWORD *mode)
{
    Handle *hh = (Handle *)h;
    if (!hh || hh->fd < 0 || !mode || !isatty(hh->fd)) { setLastError(6); return FALSE; }

    struct termios t;
    if (tcgetattr(hh->fd, &t) != 0) { setLastError(win32ErrorFromErrno(errno)); return FALSE; }

    *mode = ENABLE_PROCESSED_INPUT;
    if (t.c_lflag & ICANON) *mode |= ENABLE_LINE_INPUT;
    if (t.c_lflag & ECHO)   *mode |= ENABLE_ECHO_INPUT;
    return TRUE;
}

BOOL SetConsoleTextAttribute(HANDLE h, WORD attr)
{
    Handle *hh = (Handle *)h;
    if (!hh || hh->fd < 0 || !isatty(hh->fd)) return FALSE;

    // Win32 colour bits are blue=1, green=2, red=4; ANSI orders them
    // red=1, green=2, blue=4, so the red and blue bits swap.
    int fg = 0;
    if (attr & FOREGROUND_RED)   fg |= 1;
    if (attr & FOREGROUND_GREEN) fg |= 2;
    if (attr & FOREGROUND_BLUE)  fg |= 4;
    const int bright = (attr & FOREGROUND_INTENSITY) ? 1 : 0;

    dprintf(hh->fd, "\033[%d;%dm", bright, 30 + fg);
    return TRUE;
}

BOOL SetConsoleCursorPosition(HANDLE h, COORD pos)
{
    Handle *hh = (Handle *)h;
    if (!hh || hh->fd < 0 || !isatty(hh->fd)) return FALSE;

    // Column zero of the CURRENT row is a carriage return, not absolute
    // addressing.
    //
    // ConsoleNG::ConsoleOut is the only caller that matters, and it does
    //     GetConsoleScreenBufferInfo(...);   // read the cursor
    //     csbi.dwCursorPosition.X = 0;       // keep the row, go to column 0
    //     SetConsoleCursorPosition(...);
    // to overwrite the "> " prompt the previous line left behind.
    //
    // Emitting ESC[row;colH sent every line to the top-left corner, because
    // GetConsoleScreenBufferInfo reports a zeroed cursor here -- there is no
    // cheap way to read a tty's cursor, and the obvious one (the DSR query
    // ESC[6n) races with InputProc, which is blocked reading the same tty and
    // would consume the reply.
    //
    // A bare CR expresses the intent exactly and needs no query at all. Only
    // the absolute form is used when a row is genuinely being addressed.
    if (pos.X == 0 && pos.Y == 0) {
        (void)!write(hh->fd, "\r", 1);
        return TRUE;
    }
    // ANSI cursor addressing is 1-based; the Win32 coordinates are 0-based.
    dprintf(hh->fd, "\033[%d;%dH", pos.Y + 1, pos.X + 1);
    return TRUE;
}

BOOL GetConsoleScreenBufferInfo(HANDLE h, PCONSOLE_SCREEN_BUFFER_INFO info)
{
    Handle *hh = (Handle *)h;
    if (!hh || !info) { setLastError(87); return FALSE; }

    memset(info, 0, sizeof(*info));

    struct winsize ws;
    if (hh->fd >= 0 && ioctl(hh->fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col) {
        info->dwSize.X = (SHORT)ws.ws_col;
        info->dwSize.Y = (SHORT)ws.ws_row;
    } else {
        info->dwSize.X = 80;
        info->dwSize.Y = 25;
    }

    info->srWindow.Right  = info->dwSize.X - 1;
    info->srWindow.Bottom = info->dwSize.Y - 1;
    info->dwMaximumWindowSize = info->dwSize;
    info->wAttributes = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    return TRUE;
}

BOOL ReadConsoleA(HANDLE h, LPVOID buf, DWORD toRead, LPDWORD read, LPVOID)
{
    Handle *hh = (Handle *)h;
    if (!hh || hh->fd < 0 || !buf) { setLastError(6); return FALSE; }
    ssize_t n = ::read(hh->fd, buf, toRead);
    if (n < 0) { setLastError(win32ErrorFromErrno(errno)); return FALSE; }
    // End of input must be reported as failure, not as a successful read of
    // zero bytes. console_ng.cpp's InputProc is written for that contract --
    //     if (!ReadConsole(hStdI, cbuf, 1024, &count, NULL))
    //         break; // Console not available, exiting
    // -- and immediately below does cConsoleCmd[count - 1] = '\0', which with
    // count == 0 underflows and writes out of bounds. On Windows a console
    // read never returns zero this way; on Linux stdin can be /dev/null or a
    // closed pipe, so it happens on the first read.
    if (n == 0) { if (read) *read = 0; return FALSE; }

    // A Windows console returns CRLF line endings, and InputProc depends on it
    // in two places:
    //
    //     strncpy(cConsoleCmd + 1, cbuf, count);
    //     cConsoleCmd[count - 1] = '\0';   // "eliminates CR"
    //     ...
    //     if (!strncmp(cbuf, "exit\r\n", 6)) break;
    //
    // With "help\r\n" count is 6, so index 5 overwrites the '\r' and leaves
    // "help". A bare "help\n" gives count 5 and index 4 overwrites the 'p',
    // so every command arrives one character short -- "hel", "exi" -- and
    // matches nothing. The literal "exit\r\n" comparison can never match at
    // all, so the console could not even be closed from the prompt.
    //
    // The terminal hands over a lone '\n', so the CR is restored here rather
    // than editing the two call sites. Only when there is room for it: a
    // truncated line would be worse than an unconverted one.
    {
        char *cb = (char *)buf;
        if (n > 0 && cb[n - 1] == '\n' && (DWORD)(n + 1) <= toRead &&
            (n < 2 || cb[n - 2] != '\r')) {
            cb[n - 1] = '\r';
            cb[n]     = '\n';
            ++n;
        }
    }

    if (read) *read = (DWORD)n;
    return TRUE;
}

BOOL WriteConsoleA(HANDLE h, LPCVOID buf, DWORD toWrite, LPDWORD written, LPVOID)
{
    Handle *hh = (Handle *)h;
    if (!hh || hh->fd < 0 || !buf) { setLastError(6); return FALSE; }

    // WRITE UNTIL IT IS ALL WRITTEN. A single ::write may transfer fewer bytes
    // than asked and report success; WriteConsole does not do that, and its
    // callers do not check the count -- ConsoleNG::ConsoleOut passes a whole
    // log line and moves on. A short write would silently truncate it.
    //
    // EINTR is retried rather than reported: a signal arriving mid-write is
    // not a console error, and Windows has no equivalent to surface.
    const char *p = (const char *)buf;
    DWORD done = 0;
    while (done < toWrite) {
        const ssize_t n = ::write(hh->fd, p + done, toWrite - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            setLastError(win32ErrorFromErrno(errno));
            if (written) *written = done;
            return FALSE;
        }
        if (n == 0) break;
        done += (DWORD)n;
    }
    if (written) *written = done;
    return TRUE;
}

DWORD GetConsoleProcessList(LPDWORD list, DWORD count)
{
    // ConsoleManager::IsConsoleExclusive compares this against 1 to decide
    // whether the console window is Orbiter's own to hide. Reporting 1
    // unconditionally claimed every terminal, including one the user started
    // Orbiter from. orbiter_ConsoleProcessCount answers properly: 1 when
    // Orbiter owns the console, 2 when it inherited a shell's terminal.
    if (list && count >= 1) list[0] = (DWORD)getpid();
    return orbiter_ConsoleProcessCount();
}

// ===========================================================================
// Registry
// ===========================================================================

LSTATUS RegOpenKeyExA(HKEY root, LPCSTR subKey, DWORD, DWORD access, PHKEY out)
{
    if (!out) return ERROR_INVALID_PARAMETER;
    std::lock_guard<std::mutex> lock(g_registryLock);

    const std::string prefix = subKey ? std::string(subKey) + "\\" : std::string();

    // Opening a key for reading must fail when the key does not exist, as it
    // does on Windows. Orbiter::Create relies on exactly that:
    //
    //     ret = RegOpenKeyEx (HKEY_CURRENT_USER, "Software\\Wine", 0,
    //                         KEY_QUERY_VALUE, &key);
    //     bWINEenv = (ret == ERROR_SUCCESS);
    //
    // Returning success unconditionally reports every key as present, so that
    // probe always said "running under Wine". It gave the right answer for
    // this port by accident -- there is no IE control here either -- but it
    // would mislead any other existence check.
    //
    // A key with write access is allowed to be created on open, which is what
    // keeps settings writable before they have ever been stored.
    const bool wantWrite = (access & (KEY_SET_VALUE | KEY_WRITE)) != 0;
    if (!wantWrite && !prefix.empty()) {
        bool found = false;
        for (const auto &kv : registryStore()) {
            if (kv.first.compare(0, prefix.size(), prefix) == 0) { found = true; break; }
        }
        if (!found) return ERROR_FILE_NOT_FOUND;
    }

    RegKey *k = new RegKey;
    k->prefix = prefix;
    g_regKeys.push_back(k);
    *out = (HKEY)k;
    return ERROR_SUCCESS;
}

LSTATUS RegCloseKey(HKEY key)
{
    std::lock_guard<std::mutex> lock(g_registryLock);
    RegKey *k = (RegKey *)key;
    for (auto it = g_regKeys.begin(); it != g_regKeys.end(); ++it) {
        if (*it == k) { g_regKeys.erase(it); delete k; break; }
    }
    return ERROR_SUCCESS;
}

LSTATUS RegQueryValueExA(HKEY key, LPCSTR name, LPDWORD, LPDWORD type,
                         LPBYTE data, LPDWORD size)
{
    std::lock_guard<std::mutex> lock(g_registryLock);
    RegKey *k = (RegKey *)key;
    if (!k || !name || !size) return ERROR_INVALID_PARAMETER;

    auto &store = registryStore();
    auto it = store.find(k->prefix + name);
    if (it == store.end()) return ERROR_FILE_NOT_FOUND;

    const std::string &v = it->second;
    if (type) *type = 1;   // REG_SZ

    if (!data) { *size = (DWORD)v.size() + 1; return ERROR_SUCCESS; }
    if (*size < v.size() + 1) { *size = (DWORD)v.size() + 1; return 234; }

    memcpy(data, v.c_str(), v.size() + 1);
    *size = (DWORD)v.size() + 1;
    return ERROR_SUCCESS;
}

LSTATUS RegSetValueExA(HKEY key, LPCSTR name, DWORD, DWORD,
                       const BYTE *data, DWORD size)
{
    std::lock_guard<std::mutex> lock(g_registryLock);
    RegKey *k = (RegKey *)key;
    if (!k || !name || !data) return ERROR_INVALID_PARAMETER;

    registryStore()[k->prefix + name] =
        std::string((const char *)data, size ? size - 1 : 0);
    registryFlush();
    return ERROR_SUCCESS;
}

// ===========================================================================
// Version info
//
// Orbiter reads its own build version from the executable's resources. There
// are no resources in an ELF binary, so the values come from the build-time
// version macros instead; the callers only ever format them into the log
// header and the About tab.
// ===========================================================================

namespace {
// A synthesised VS_FIXEDFILEINFO, returned for the "\\" sub-block.
VS_FIXEDFILEINFO g_versionInfo = {
    0xFEEF04BD,   // dwSignature, the value the SDK stamps
    0x00010000,   // dwStrucVersion
    0, 0, 0, 0,   // file/product version, filled in below
    0, 0, 0, 0, 0, 0, 0
};
bool g_versionReady = false;
} // namespace

DWORD GetFileVersionInfoSizeA(LPCSTR, LPDWORD handle)
{
    if (handle) *handle = 0;
    return (DWORD)sizeof(VS_FIXEDFILEINFO);
}

BOOL GetFileVersionInfoA(LPCSTR, DWORD, DWORD len, LPVOID data)
{
    if (!data || len < sizeof(VS_FIXEDFILEINFO)) { setLastError(87); return FALSE; }
    if (!g_versionReady) {
        g_versionReady = true;
        // Encoded as Windows does: high word major, low word minor.
        g_versionInfo.dwFileVersionMS    = (1 << 16) | 0;
        g_versionInfo.dwFileVersionLS    = 0;
        g_versionInfo.dwProductVersionMS = g_versionInfo.dwFileVersionMS;
        g_versionInfo.dwProductVersionLS = 0;
    }
    memcpy(data, &g_versionInfo, sizeof(g_versionInfo));
    return TRUE;
}

BOOL VerQueryValueA(LPCVOID block, LPCSTR subBlock, LPVOID *buf, UINT *len)
{
    if (!block || !buf || !len) { setLastError(87); return FALSE; }
    // Only the fixed-info root block is ever requested.
    if (subBlock && strcmp(subBlock, "\\") != 0) return FALSE;
    *buf = (LPVOID)block;
    *len = (UINT)sizeof(VS_FIXEDFILEINFO);
    return TRUE;
}

// ===========================================================================
// Directories, shell and system queries
// ===========================================================================

DWORD GetCurrentDirectoryA(DWORD size, LPSTR buf)
{
    if (!buf || size == 0) { setLastError(87); return 0; }
    if (!getcwd(buf, size)) { setLastError(win32ErrorFromErrno(errno)); return 0; }
    return (DWORD)strnlen(buf, size);
}

BOOL SetCurrentDirectoryA(LPCSTR path)
{
    if (!path) { setLastError(87); return FALSE; }
    if (chdir(path) != 0) { setLastError(win32ErrorFromErrno(errno)); return FALSE; }
    return TRUE;
}

BOOL SHCreateDirectoryExA(HWND, LPCSTR path, void *)
{
    if (!path) { setLastError(87); return FALSE; }
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) { setLastError((DWORD)ec.value()); return FALSE; }
    return TRUE;
}

// File attributes. Only the bits callers in this tree test are reported: the
// directory flag, read-only, and the INVALID_FILE_ATTRIBUTES sentinel that
// means the path does not exist. TerrainToolKit uses exactly that pattern to
// decide whether to create its tile cache directory.
DWORD GetFileAttributesA(LPCSTR path)
{
    if (!path) { setLastError(87); return INVALID_FILE_ATTRIBUTES; }

    struct stat st;
    if (stat(path, &st) != 0) {
        setLastError(win32ErrorFromErrno(errno));
        return INVALID_FILE_ATTRIBUTES;
    }

    DWORD attr = 0;
    if (S_ISDIR(st.st_mode)) attr |= FILE_ATTRIBUTE_DIRECTORY;
    if (access(path, W_OK) != 0) attr |= FILE_ATTRIBUTE_READONLY;
    if (attr == 0) attr = FILE_ATTRIBUTE_NORMAL;
    return attr;
}

// A single directory level, unlike SHCreateDirectoryEx above which creates the
// whole chain. The security-attributes argument has no equivalent and is
// ignored; the mode is the usual 0755 before umask.
BOOL CreateDirectoryA(LPCSTR path, void *)
{
    if (!path) { setLastError(87); return FALSE; }
    if (mkdir(path, 0755) != 0) {
        setLastError(win32ErrorFromErrno(errno));
        return FALSE;
    }
    return TRUE;
}

BOOL DeleteFileA(LPCSTR path)
{
    if (!path) { setLastError(87); return FALSE; }
    if (unlink(path) != 0) {
        setLastError(win32ErrorFromErrno(errno));
        return FALSE;
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// Global memory and clipboard
//
// GlobalAlloc's moveable-handle model has no counterpart here and nothing in
// this tree depends on it: every caller allocates, locks once, fills, unlocks
// and hands the handle to SetClipboardData. So the handle IS the pointer, and
// Lock/Unlock are identity operations.
//
// The clipboard holds one text item. Publishing to the desktop selection needs
// a live X or Wayland connection owned by the UI thread, which this layer does
// not have -- so the contents are kept process-local. A copy inside the
// application therefore pastes back correctly; pasting into another
// application does not, and that is a known limit rather than an oversight.
// ---------------------------------------------------------------------------

HGLOBAL GlobalAlloc(UINT flags, SIZE_T bytes)
{
    void *p = (flags & GMEM_ZEROINIT) ? calloc(1, bytes ? bytes : 1)
                                      : malloc(bytes ? bytes : 1);
    if (!p) setLastError(8);   // ERROR_NOT_ENOUGH_MEMORY
    return (HGLOBAL)p;
}

LPVOID  GlobalLock  (HGLOBAL mem) { return (LPVOID)mem; }
BOOL    GlobalUnlock(HGLOBAL)     { return TRUE; }

HGLOBAL GlobalFree(HGLOBAL mem)
{
    free((void *)mem);
    return nullptr;
}

SIZE_T GlobalSize(HGLOBAL mem)
{
    // malloc_usable_size would answer, but callers only ever size their own
    // allocation, so the exact figure is not needed.
    return mem ? 1 : 0;
}

namespace {
std::mutex       g_clipboardLock;
std::string      g_clipboardText;
bool             g_clipboardOpen = false;
}

BOOL OpenClipboard(HWND)
{
    g_clipboardLock.lock();
    g_clipboardOpen = true;
    return TRUE;
}

BOOL CloseClipboard(void)
{
    if (!g_clipboardOpen) return FALSE;
    g_clipboardOpen = false;
    g_clipboardLock.unlock();
    return TRUE;
}

BOOL EmptyClipboard(void)
{
    g_clipboardText.clear();
    return TRUE;
}

HANDLE SetClipboardData(UINT format, HANDLE mem)
{
    if (format != CF_TEXT || !mem) return nullptr;
    g_clipboardText = (const char *)mem;
    // Windows takes ownership of the handle here, so the caller must not free
    // it -- the text has been copied, so the block is released now.
    free((void *)mem);
    return mem;
}

HANDLE GetClipboardData(UINT format)
{
    if (format != CF_TEXT || g_clipboardText.empty()) return nullptr;
    return (HANDLE)g_clipboardText.c_str();
}

HRESULT SHGetFolderPathA(HWND, int csidl, HANDLE, DWORD, LPSTR path)
{
    if (!path) return E_INVALIDARG;

    const char *home = getenv("HOME");
    std::filesystem::path base = home ? home : ".";

    // XDG names the user directories; fall back to the conventional English
    // names when the environment does not define them.
    auto xdgOr = [&](const char *var, const char *fallback) {
        const char *v = getenv(var);
        return (v && *v) ? std::filesystem::path(v) : base / fallback;
    };

    std::filesystem::path result;
    switch (csidl & ~CSIDL_FLAG_CREATE) {
    case CSIDL_PERSONAL:      result = xdgOr("XDG_DOCUMENTS_DIR", "Documents"); break;
    case CSIDL_MYPICTURES:    result = xdgOr("XDG_PICTURES_DIR",  "Pictures");  break;
    case CSIDL_APPDATA:
    case CSIDL_LOCAL_APPDATA: result = xdgOr("XDG_CONFIG_HOME",   ".config");   break;
    case CSIDL_PROFILE:       result = base; break;
    default:                  result = base; break;
    }

    if (csidl & CSIDL_FLAG_CREATE) {
        std::error_code ec;
        std::filesystem::create_directories(result, ec);
    }

    snprintf(path, MAX_PATH, "%s", result.c_str());
    return S_OK;
}

HINSTANCE ShellExecuteA(HWND, LPCSTR, LPCSTR file, LPCSTR, LPCSTR, int)
{
    // Opening a document or URL with the desktop's default handler.
    if (!file) { setLastError(87); return nullptr; }
    std::string cmd = "xdg-open '";
    cmd += file;
    cmd += "' >/dev/null 2>&1 &";
    int rc = system(cmd.c_str());
    // Windows returns a value above 32 on success; the callers only compare
    // against that threshold.
    return (HINSTANCE)(uintptr_t)(rc == 0 ? 33 : 0);
}

// CP_UTF8 IS HONOURED; every other code page keeps the widening copy.
//
// The original version ignored the code page entirely, because the only caller
// at the time passed ASCII paths and a widening copy is exact for those. It is
// not exact for CP_UTF8, and the difference is visible: D3D9Pad's
// UTF8ToCP1252 decodes with this function, and Orbiter's own MFD code writes a
// degree sign into a narrow literal (MfdHsi.cpp:216, "CRS %03.0f°" passed to
// Text with a length of 9 for an eight-character string) because the source
// file is UTF-8. Widening those two bytes separately puts a stray glyph in
// the readout.
//
// MB_ERR_INVALID_CHARS is honoured too, and callers depend on it: the
// reference's fallback for a legacy plugin that hands over Windows-1252 rather
// than UTF-8 is precisely "the conversion returned 0, so use the bytes as they
// are".
//
// wchar_t is 32 bits on Linux, so a codepoint above the BMP is stored whole
// rather than as a surrogate pair. That differs from Win32's UTF-16 output and
// is the correct answer for this platform's wchar_t; the one consumer in the
// tree narrows anything above 0xFF to '?' either way.
int MultiByteToWideChar(UINT CodePage, DWORD dwFlags, LPCSTR src, int srcLen,
                        LPWSTR dst, int dstLen)
{
    if (!src) return 0;
    const size_t n = (srcLen < 0) ? strlen(src) + 1 : (size_t)srcLen;

    if (CodePage == CP_UTF8) {
        const unsigned char *s = (const unsigned char *)src;
        const bool strict = (dwFlags & MB_ERR_INVALID_CHARS) != 0;

        size_t i = 0, out = 0;
        while (i < n) {
            const unsigned char c = s[i];
            unsigned int cp;
            size_t len;

            if      (c < 0x80)           { cp = c;        len = 1; }
            else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
            else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
            else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
            else {
                if (strict) { setLastError(1113); return 0; }  // ERROR_NO_UNICODE_TRANSLATION
                cp = c; len = 1;                               // lenient: pass the byte
            }

            if (i + len > n) {
                if (strict) { setLastError(1113); return 0; }
                cp = c; len = 1;
            }
            else {
                bool bad = false;
                unsigned int v = cp;
                for (size_t k = 1; k < len; k++) {
                    if ((s[i + k] & 0xC0) != 0x80) { bad = true; break; }
                    v = (v << 6) | (unsigned int)(s[i + k] & 0x3F);
                }
                // Overlong encodings are invalid and are the usual signature
                // of a string that was never UTF-8 to begin with.
                if (!bad && ((len == 2 && v < 0x80) ||
                             (len == 3 && v < 0x800) ||
                             (len == 4 && v < 0x10000))) bad = true;
                if (bad) {
                    if (strict) { setLastError(1113); return 0; }
                    cp = c; len = 1;
                }
                else cp = v;
            }

            if (dst && dstLen > 0) {
                if (out >= (size_t)dstLen) break;
                dst[out] = (wchar_t)cp;
            }
            out++;
            i += len;
        }
        return (int)out;
    }

    if (!dst || dstLen == 0) return (int)n;

    // Every other code page: the tree passes ASCII paths through here, so a
    // widening copy is exact.
    size_t out = 0;
    for (; out < n && out < (size_t)dstLen; ++out)
        dst[out] = (wchar_t)(unsigned char)src[out];
    return (int)out;
}

int WideCharToMultiByte(UINT, DWORD, LPCWSTR src, int srcLen,
                        LPSTR dst, int dstLen, LPCSTR, BOOL *)
{
    if (!src) return 0;
    const size_t n = (srcLen < 0) ? wcslen(src) + 1 : (size_t)srcLen;
    if (!dst || dstLen == 0) return (int)n;

    size_t out = 0;
    for (; out < n && out < (size_t)dstLen; ++out)
        dst[out] = (char)(src[out] < 256 ? src[out] : '?');
    return (int)out;
}

// ===========================================================================
// Directory change notification
//
// Orbiter watches the scenario folder so the launchpad tree refreshes when
// files are added or removed. inotify is the direct equivalent.
// ===========================================================================

HANDLE FindFirstChangeNotificationA(LPCSTR path, BOOL, DWORD filter)
{
    if (!path) { setLastError(87); return nullptr; }

    int fd = inotify_init1(IN_NONBLOCK);
    if (fd < 0) { setLastError(win32ErrorFromErrno(errno)); return nullptr; }

    uint32_t mask = 0;
    if (filter & FILE_NOTIFY_CHANGE_FILE_NAME)
        mask |= IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO;
    if (filter & FILE_NOTIFY_CHANGE_DIR_NAME)
        mask |= IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO;
    if (filter & FILE_NOTIFY_CHANGE_LAST_WRITE)
        mask |= IN_MODIFY | IN_CLOSE_WRITE;
    if (!mask) mask = IN_CREATE | IN_DELETE | IN_MODIFY;

    int wd = inotify_add_watch(fd, path, mask);
    if (wd < 0) { close(fd); setLastError(win32ErrorFromErrno(errno)); return nullptr; }

    Handle *h = new Handle{ Handle::Watch };
    h->fd = fd;
    h->watchDesc = wd;
    return (HANDLE)h;
}

BOOL FindNextChangeNotification(HANDLE h)
{
    Handle *hh = (Handle *)h;
    if (!hh || hh->kind != Handle::Watch) { setLastError(6); return FALSE; }

    // Drain the queued events. The caller only wants to know that something
    // changed, which is also all the Windows API reports.
    char buf[4096];
    while (::read(hh->fd, buf, sizeof(buf)) > 0) { }
    return TRUE;
}

BOOL FindCloseChangeNotification(HANDLE h)
{
    return CloseHandle(h);
}

} // extern "C"


// ===========================================================================
// The console window
//
// Orbiter's console behaviour, from the three places that drive it:
//
//   WinMain          if (IsConsoleExclusive()) ShowConsole(false);
//   ConsoleNG ctor   ShowConsole(true);
//   CloseSession     if (IsConsoleExclusive()) ShowConsole(false);
//
// A console-subsystem process on Windows is GIVEN a console window before
// main runs, whether it wants one or not. Orbiter hides it at startup so the
// Launchpad appears alone, shows it when a session begins with no graphics
// client -- ConsoleNG is constructed only in that branch of
// CreateRenderWindow -- and hides it again when the session closes.
//
// IsConsoleExclusive is the guard on both hides. GetConsoleProcessList
// returning one means Orbiter is the only process attached, so the window is
// its own to hide; launched from a shell the count is two or more and Orbiter
// leaves the terminal alone, because it belongs to the user.
//
// The same rule applies here. If stdout is a terminal, Orbiter was started
// from a shell: that terminal is not ours, IsConsoleExclusive is false, and
// nothing is hidden or shown -- exactly as on Windows. Otherwise Orbiter owns
// its console, and this creates and destroys one on demand.
//
// Creating on show rather than hiding a pre-made window is deliberate. The
// observable behaviour is identical -- Windows discards the console's contents
// between sessions too, since CloseSession hides it and the next ConsoleNG
// shows it fresh -- and it avoids driving a window manager to hide a window
// that need not exist yet.
// ===========================================================================

namespace {

pid_t g_consolePid    = -1;
int   g_consoleMaster = -1;
int   g_savedStdIn = -1, g_savedStdOut = -1, g_savedStdErr = -1;
bool  g_ownConsole = false;   // false when inherited from a shell
bool  g_consoleChecked = false;

// Terminal emulators, in preference order.
const char *const kTerminals[] = {
    "konsole", "gnome-terminal", "xfce4-terminal", "xterm",
    "x-terminal-emulator", "alacritty", "kitty"
};

const char *findTerminal()
{
    for (const char *t : kTerminals) {
        std::string probe = "command -v ";
        probe += t;
        probe += " >/dev/null 2>&1";
        if (system(probe.c_str()) == 0) return t;
    }
    return nullptr;
}

// True when Orbiter owns its console, i.e. it was NOT started from a terminal.
bool ownsConsole()
{
    if (!g_consoleChecked) {
        g_consoleChecked = true;
        g_ownConsole = !isatty(STDOUT_FILENO);
    }
    return g_ownConsole;
}

} // namespace

extern "C" DWORD orbiter_ConsoleProcessCount(void)
{
    // One when the console is Orbiter's alone, two when it is a terminal the
    // user started it from. This is what IsConsoleExclusive compares against.
    return ownsConsole() ? 1 : 2;
}

extern "C" void orbiter_ShowConsoleWindow(int show)
{
    if (!ownsConsole()) return;    // not ours to touch

    if (!show) {
        if (g_consolePid <= 0) return;
        kill(g_consolePid, SIGTERM);
        g_consolePid = -1;
        if (g_consoleMaster >= 0) { close(g_consoleMaster); g_consoleMaster = -1; }
        if (g_savedStdIn  >= 0) { dup2(g_savedStdIn,  STDIN_FILENO);  close(g_savedStdIn);  g_savedStdIn  = -1; }
        if (g_savedStdOut >= 0) { dup2(g_savedStdOut, STDOUT_FILENO); close(g_savedStdOut); g_savedStdOut = -1; }
        if (g_savedStdErr >= 0) { dup2(g_savedStdErr, STDERR_FILENO); close(g_savedStdErr); g_savedStdErr = -1; }
        return;
    }

    if (g_consolePid > 0) return;  // already showing

    const char *term = findTerminal();
    if (!term) return;

    // Let the TERMINAL own the pty and attach Orbiter to it.
    //
    // The obvious approach -- openpty here and have the terminal bridge to the
    // master through /proc/<pid>/fd/<n> -- cannot work, and fails silently.
    // /dev/ptmx is a CLONING device: opening that path via /proc allocates a
    // brand new pty pair rather than reopening this one. The bridge sat
    // reading a pty nobody wrote to, which is why the window stayed blank with
    // no error anywhere.
    //
    // A terminal emulator already creates a pty for whatever it runs, and its
    // slave is a real /dev/pts/N that can be opened by name. So the helper
    // reports its tty and then does nothing at all, leaving Orbiter as the one
    // process reading and writing it -- which is precisely the relationship a
    // console-subsystem process has with its console on Windows.
    char ttyfile[128], script[128];
    snprintf(ttyfile, sizeof(ttyfile), "/tmp/orbiter-console-%d.tty", (int)getpid());
    snprintf(script,  sizeof(script),  "/tmp/orbiter-console-%d.sh",  (int)getpid());
    unlink(ttyfile);

    if (FILE *f = fopen(script, "w")) {
        // The helper must not read stdin: Orbiter is the reader, and two
        // readers on one tty would race for every keystroke. It parks on a
        // fifo that is never written instead of sleeping, so closing the
        // window ends it immediately.
        fprintf(f,
                "#!/bin/bash\n"
                "tty > %s\n"
                "exec 0<&-\n"
                "while :; do sleep 3600; done\n",
                ttyfile);
        fclose(f);
        chmod(script, 0700);
    }

    const pid_t pid = fork();
    if (pid == 0) {
        execlp(term, term, "-e", script, (char *)nullptr);
        _exit(127);
    }
    if (pid < 0) return;
    g_consolePid = pid;

    // Wait for the terminal to report its tty, then attach.
    int tty = -1;
    for (int i = 0; i < 300 && tty < 0; ++i) {
        FILE *f = fopen(ttyfile, "r");
        if (f) {
            char path[256] = {0};
            if (fgets(path, sizeof(path), f)) {
                char *nl = strchr(path, '\n');
                if (nl) *nl = '\0';
                if (path[0] == '/') tty = open(path, O_RDWR);
            }
            fclose(f);
        }
        if (tty < 0) {
            struct timespec ts { 0, 10 * 1000 * 1000 };
            nanosleep(&ts, nullptr);
        }
    }
    if (tty < 0) { kill(pid, SIGTERM); g_consolePid = -1; return; }

    g_savedStdIn  = dup(STDIN_FILENO);
    g_savedStdOut = dup(STDOUT_FILENO);
    g_savedStdErr = dup(STDERR_FILENO);

    dup2(tty, STDIN_FILENO);
    dup2(tty, STDOUT_FILENO);
    dup2(tty, STDERR_FILENO);
    if (tty > STDERR_FILENO) close(tty);

    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    // Put the tty into the line discipline a Windows console has.
    //
    // Orbiter writes bare "\n" line endings. On a Windows console that returns
    // the cursor to column zero as well as moving it down; a Unix tty only
    // does so when ONLCR is set, and without it every line starts wherever the
    // previous one ended and overwrites it -- which is why the banner appeared
    // as "biter NG (no graphics)" with the first characters of one line buried
    // under the tail of another.
    //
    // OPOST enables output processing at all; ONLCR is the newline mapping.
    // ICANON and ECHO give line-at-a-time input with local echo, which is what
    // ReadConsole expects and what lets the user see what they type at the
    // prompt.
    {
        struct termios t;
        if (tcgetattr(STDOUT_FILENO, &t) == 0) {
            t.c_oflag |= OPOST | ONLCR;
            t.c_lflag |= ICANON | ECHO | ECHOE | ECHOK;
            t.c_iflag |= ICRNL;
            tcsetattr(STDOUT_FILENO, TCSANOW, &t);
        }
    }

}

// ===========================================================================
// Common file dialogs
//
// GetOpenFileName / GetSaveFileName. TerrainToolKit is the only module that
// uses them -- to pick a terrain tile to load, and to choose where to write
// one -- and it was the only module failing to load at all, with
//     undefined symbol: GetSaveFileNameA
// because the shim declared both and implemented neither.
//
// These run the desktop's own file chooser rather than drawing one: kdialog
// under KDE, zenity elsewhere, both of which are the dialog the user already
// knows. Nothing is faked -- if no chooser is installed the call reports
// cancellation, which is exactly what a user pressing Cancel produces and
// which every caller must already handle.
//
// The Win32 contract that matters to callers:
//   - lpstrFile is an in/out buffer holding the chosen path, nMaxFile long,
//     and may arrive holding a default name.
//   - TRUE means a file was chosen; FALSE means cancelled or failed, and
//     CommDlgExtendedError distinguishes the two. Callers overwhelmingly
//     just test the return value.
//   - nFileOffset and nFileExtension index into lpstrFile at the start of the
//     file name and of the extension.
// ===========================================================================

namespace {

// Turn a Win32 filter into the chooser's own syntax.
//
// lpstrFilter is a double-null-terminated list of PAIRS: a human-readable
// description then a pattern, repeating, e.g.
//     "Elevation files\0*.elv\0All files\0*.*\0\0"
// Only the patterns are wanted; the descriptions are re-derived by the
// chooser itself.
std::vector<std::string> filterPatterns(LPCSTR filter)
{
    std::vector<std::string> pats;
    if (!filter) return pats;

    const char *p = filter;
    while (*p) {
        const char *desc = p;             // description
        p += strlen(desc) + 1;
        if (!*p) break;
        const char *pat = p;              // pattern
        p += strlen(pat) + 1;

        // A pattern field may itself hold several, semicolon separated.
        std::string s(pat);
        size_t start = 0;
        while (start < s.size()) {
            size_t end = s.find(';', start);
            if (end == std::string::npos) end = s.size();
            std::string one = s.substr(start, end - start);
            if (!one.empty()) pats.push_back(one);
            start = end + 1;
        }
    }
    return pats;
}

// Single-quote a string for the shell.
std::string shq(const std::string &in)
{
    std::string out = "'";
    for (char c : in) {
        if (c == '\'') out += "'\\''";
        else           out += c;
    }
    out += "'";
    return out;
}

bool runChooser(LPOPENFILENAMEA ofn, bool saving)
{
    if (!ofn || !ofn->lpstrFile || ofn->nMaxFile == 0) {
        setLastError(87);
        return false;
    }

    const bool haveKdialog = (system("command -v kdialog >/dev/null 2>&1") == 0);
    const bool haveZenity  = (system("command -v zenity  >/dev/null 2>&1") == 0);
    if (!haveKdialog && !haveZenity) {
        // No chooser installed. Reported as cancellation rather than as an
        // error, because that is the outcome callers already handle and it
        // leaves the caller's buffer untouched.
        return false;
    }

    // Starting directory and default file name, from whatever the caller set.
    std::string startDir = ofn->lpstrInitialDir ? ofn->lpstrInitialDir : "";
    std::string startName = ofn->lpstrFile[0] ? ofn->lpstrFile : "";
    std::string start;
    if (!startDir.empty()) {
        start = startDir;
        if (start.back() != '/') start += '/';
    }
    start += startName;

    const std::string title = ofn->lpstrTitle ? ofn->lpstrTitle
                                              : (saving ? "Save File" : "Open File");
    const std::vector<std::string> pats = filterPatterns(ofn->lpstrFilter);

    std::string cmd;
    if (haveKdialog) {
        cmd = "kdialog --title " + shq(title);
        cmd += saving ? " --getsavefilename " : " --getopenfilename ";
        cmd += shq(start.empty() ? "." : start);
        if (!pats.empty()) {
            // kdialog wants "desc (*.a *.b)"; the patterns alone read fine.
            std::string f;
            for (const auto &p : pats) { if (!f.empty()) f += " "; f += p; }
            cmd += " " + shq(f);
        }
    } else {
        cmd = "zenity --file-selection --title=" + shq(title);
        if (saving) cmd += " --save --confirm-overwrite";
        if (!start.empty()) cmd += " --filename=" + shq(start);
        for (const auto &p : pats)
            cmd += " --file-filter=" + shq(p);
    }
    cmd += " 2>/dev/null";

    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe) return false;

    char buf[4096] = {0};
    const bool got = (fgets(buf, sizeof(buf), pipe) != nullptr);
    const int rc = pclose(pipe);

    if (!got || rc != 0) return false;    // cancelled

    // Strip the trailing newline the chooser prints.
    size_t len = strlen(buf);
    while (len && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
    if (!len) return false;

    // Append the default extension if the user gave none, which is what the
    // Windows dialog does when lpstrDefExt is set.
    std::string chosen(buf);
    if (saving && ofn->lpstrDefExt && *ofn->lpstrDefExt) {
        const size_t slash = chosen.find_last_of('/');
        const size_t dot   = chosen.find_last_of('.');
        if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
            chosen += '.';
            chosen += ofn->lpstrDefExt;
        }
    }

    if (chosen.size() + 1 > ofn->nMaxFile) {
        setLastError(122);                // ERROR_INSUFFICIENT_BUFFER
        return false;
    }
    strcpy(ofn->lpstrFile, chosen.c_str());

    // nFileOffset and nFileExtension index into lpstrFile, per the contract.
    const size_t slash = chosen.find_last_of('/');
    ofn->nFileOffset = (WORD)(slash == std::string::npos ? 0 : slash + 1);
    const size_t dot = chosen.find_last_of('.');
    ofn->nFileExtension = (WORD)((dot == std::string::npos || dot < ofn->nFileOffset)
                                 ? 0 : dot + 1);

    if (ofn->lpstrFileTitle && ofn->nMaxFileTitle) {
        snprintf(ofn->lpstrFileTitle, ofn->nMaxFileTitle, "%s",
                 chosen.c_str() + ofn->nFileOffset);
    }
    return true;
}

} // namespace

BOOL GetOpenFileNameA(LPOPENFILENAMEA ofn) { return runChooser(ofn, false) ? TRUE : FALSE; }
BOOL GetSaveFileNameA(LPOPENFILENAMEA ofn) { return runChooser(ofn, true)  ? TRUE : FALSE; }
