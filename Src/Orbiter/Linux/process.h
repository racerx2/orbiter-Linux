// Linux <process.h> — MSVC's process and thread-creation header.
//
// Included by Src/Orbiter/Orbiter.cpp, Src/Module/LuaScript/LuaInline and
// Src/Plugin/LuaMFD. Only _beginthreadex, _endthreadex and _execl are used.
//
// _beginthreadex is the CRT-safe thread starter; its signature differs from
// CreateThread in that the routine is __stdcall and returns unsigned. It is
// declared here and implemented in Src/Orbiter/Linux/Process.cpp over
// pthreads, so the Lua interpreter threads keep the same lifecycle.
//
// The returned value is a uintptr_t handle on Windows, which callers pass to
// WaitForSingleObject and CloseHandle. The implementation returns the same
// HANDLE type those functions take here, so the call sites are unchanged.

#ifndef ORBITER_LINUX_PROCESS_H
#define ORBITER_LINUX_PROCESS_H

#ifdef _WIN32
#error "This header is for non-Windows builds only."
#endif

#include <windows.h>
#include <unistd.h>

extern "C" {

// Returns a handle usable with WaitForSingleObject/CloseHandle, or 0 on
// failure, matching the Windows contract.
uintptr_t _beginthreadex (void *security,
                          unsigned stack_size,
                          unsigned (*start_address)(void *),
                          void *arglist,
                          unsigned initflag,
                          unsigned *thrdaddr);

void _endthreadex (unsigned retval);

} // extern "C"

// _execl replaces the current process image, identical in contract to POSIX
// execl, so it forwards directly.
#define _execl execl

#endif // ORBITER_LINUX_PROCESS_H
