// The shim's HANDLE object, and everything CloseHandle has to know about.
//
// HANDLE is an opaque pointer here, and every one of them points at an
// OrbHandle carrying a kind tag. That is what lets CloseHandle do the right
// thing for a thread, a mutex, an event, a directory watch or an open file
// without the caller having to say which it holds -- exactly the property the
// Win32 call sites assume -- so CloseHandle must be compiled against this
// definition.
//
// The object lives here rather than in Platform.cpp so that Handles.cpp
// depends on nothing but libc and pthreads. Platform.cpp cannot be linked on
// its own: it leaves InitLib and ExitLib undefined for the executable to
// supply, and those two are its only non-libc undefined symbols. Utils/texpack
// is the case that forced the split -- pure zlib and file I/O, needing
// CreateFile, ReadFile, GetFileSizeEx, CloseHandle and the directory walk
// without dragging in module loading, the registry, the console and the file
// choosers.

#ifndef ORBITER_LINUX_HANDLES_H
#define ORBITER_LINUX_HANDLES_H

#ifdef _WIN32
#error "This header is for non-Windows builds only."
#endif

#include <windows.h>
#include <pthread.h>
#include <string>

struct OrbHandle {
    enum Kind {
        Thread, Mutex, Event, Process, Watch, StdStream, Module, File, Find
    } kind;

    // Pseudo-handles the caller does not own: the three standard streams, the
    // current process, and the module that stands for the executable itself.
    // CloseHandle must leave these alone.
    bool            pinned = false;

    // Thread
    pthread_t       thread{};
    bool            joined = false;

    // Mutex
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

    // Event
    pthread_cond_t  cond  = PTHREAD_COND_INITIALIZER;
    pthread_mutex_t evLock = PTHREAD_MUTEX_INITIALIZER;
    bool            signalled = false;
    bool            manualReset = false;

    // Process / stream / watch / open file
    pid_t           pid = 0;
    int             fd  = -1;
    int             watchDesc = -1;

    // Module (dlopen handle, or null for the pseudo-module representing the
    // executable itself)
    void           *dlHandle = nullptr;
    std::string     path;

    // Datafile module: LOAD_LIBRARY_AS_DATAFILE maps the image without running
    // any of its code, so there is no dl handle at all -- just the file mapped
    // read-only and its symbol table read directly.
    void           *mapBase = nullptr;
    size_t          mapSize = 0;

    // Module enumerated through EnumProcessModules: load address and the span
    // of its PT_LOAD segments, recorded at scan time because such a handle has
    // no dl handle to interrogate later. See collectModule.
    void           *modBase = nullptr;
    size_t          modSize = 0;

    // Directory search opened by FindFirstFile. `dirp` is the DIR*, held as
    // void* to keep <dirent.h> out of this header; `pattern` is the wildcard
    // the caller gave, split off from the directory part of its argument.
    void           *dirp = nullptr;
    std::string     pattern;
};

extern "C" {

// The thread-local last-error store behind GetLastError, which lives in
// Handles.cpp alongside GetLastError itself.
void  orb_SetLastError(DWORD e);

// Maps errno onto the Win32 codes the tree checks for. Anything unmapped is
// passed through, which keeps the value diagnosable even when it is not one
// of the recognised constants.
DWORD orb_Win32ErrorFromErrno(int e);

}

#endif // ORBITER_LINUX_HANDLES_H
