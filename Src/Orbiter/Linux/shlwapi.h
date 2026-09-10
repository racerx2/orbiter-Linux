// Linux <Shlwapi.h> — the Windows shell path helpers.
//
// One function is used in this tree: PathFileExists, by Utils/texpack, to ask
// whether a tile file or a level directory is present before opening it. The
// Windows build links Shlwapi.lib for it; here it is one call to access().
//
// PathFileExists answers TRUE for a DIRECTORY as well as a file -- texpack
// depends on that, since it tests a level directory ("Surf/04") with the same
// call it uses for a tile -- and access(F_OK) has the same property.
//
// It is static inline rather than declared and implemented elsewhere so that
// a standalone utility including this header needs nothing linked for it.

#ifndef ORBITER_LINUX_SHLWAPI_H
#define ORBITER_LINUX_SHLWAPI_H

#ifdef _WIN32
#error "This header is for non-Windows builds only."
#endif

#include <windows.h>
#include <unistd.h>

static inline BOOL PathFileExistsA(LPCSTR path) {
    return (path && access(path, F_OK) == 0) ? TRUE : FALSE;
}

// Orbiter is compiled without UNICODE, so the unsuffixed name is the ANSI one.
#define PathFileExists PathFileExistsA

#endif // ORBITER_LINUX_SHLWAPI_H
