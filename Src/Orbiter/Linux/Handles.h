// The shim's HANDLE object, and everything CloseHandle has to know about.
//
// WHY THIS IS A FILE OF ITS OWN, rather than staying where it grew inside
// Platform.cpp.
//
// HANDLE is an opaque pointer in this shim, and every one of them points at an
// OrbHandle carrying a kind tag. That is what lets CloseHandle do the right
// thing for a thread, a mutex, an event, a directory watch or an open file
// without the caller having to say which it holds -- exactly the property the
// Win32 call sites assume -- and it means CloseHandle has to be compiled
// against the object's definition.
//
// Platform.cpp is where the object grew, and Platform.cpp is not a translation
// unit a standalone utility can link: it also carries dynamic module loading,
// the registry, the console and the file choosers, and it deliberately leaves
// InitLib and ExitLib undefined for the executable to supply. Measured with
// nm -u on a standalone compile: those two are its ONLY non-libc undefined
// symbols, and they are enough to make it unlinkable anywhere else.
//
// Utils/texpack is the case that forced the split. It is pure zlib and file
// I/O -- no graphics API anywhere in it -- and from the shim it needs the
// directory walk plus four entry points: CreateFile, ReadFile, GetFileSizeEx
// and CloseHandle. Only the last of those was implemented, and only inside
// Platform.cpp, so the tool could not be built at all.
//
// So the handle object, CloseHandle, the thread-local last-error store and
// the Win32 file and directory-enumeration API live here, in a translation
// unit whose only dependencies are libc and pthreads. Platform.cpp includes
// this header and creates OrbHandles exactly as it did before.

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

    // PSEUDO-HANDLES THE CALLER DOES NOT OWN: the three standard streams, the
    // current process, and the module that stands for the executable itself.
    // CloseHandle must leave them alone.
    //
    // It used to recognise them by comparing their addresses, which is only
    // possible while they and CloseHandle sit in one file. They stay in
    // Platform.cpp -- they are about processes, modules and the console -- so
    // the fact travels on the object instead.
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

    // Datafile module: LOAD_LIBRARY_AS_DATAFILE maps the image WITHOUT
    // running any of its code, so there is no dl handle at all -- just the
    // file mapped read-only and its symbol table read directly.
    void           *mapBase = nullptr;
    size_t          mapSize = 0;

    // Module enumerated through EnumProcessModules: its load address and the
    // span of its PT_LOAD segments, recorded at scan time because such a
    // handle has no dl handle to interrogate later. See collectModule.
    void           *modBase = nullptr;
    size_t          modSize = 0;

    // Directory search opened by FindFirstFile. `dirp` is the DIR*, kept as
    // void* so this header does not have to pull in <dirent.h> for the sake
    // of one member; `pattern` is the wildcard the caller gave, split off
    // from the directory part of its argument.
    void           *dirp = nullptr;
    std::string     pattern;
};

extern "C" {

// The thread-local last-error store behind GetLastError. Declared here
// because Platform.cpp sets it from thirty-odd call sites and the store now
// lives in Handles.cpp with GetLastError itself.
void  orb_SetLastError(DWORD e);

// Maps errno onto the Win32 codes the tree checks for. Anything unmapped is
// passed through, which keeps the value diagnosable even when it is not one
// of the recognised constants.
DWORD orb_Win32ErrorFromErrno(int e);

}

#endif // ORBITER_LINUX_HANDLES_H
