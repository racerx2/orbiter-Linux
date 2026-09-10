// Linux <mmsystem.h> — the multimedia timer subset Orbiter uses.
//
// Included by Src/Orbiter/Pane.h. Only three symbols are referenced across the
// whole tree: timeGetTime, timeBeginPeriod and timeEndPeriod.
//
// timeGetTime returns milliseconds since system start as a 32-bit value, which
// wraps after ~49.7 days. CLOCK_MONOTONIC is the right source: it is unaffected
// by wall-clock adjustments, so a user changing the system time mid-flight
// cannot make the simulator see time run backwards. The value is deliberately
// truncated to DWORD to preserve the documented wrap behaviour rather than
// silently widening it, since callers compute unsigned differences that stay
// correct across a wrap.
//
// timeBeginPeriod/timeEndPeriod request a finer Windows scheduler tick. Linux
// has no equivalent and needs none -- clock_gettime is already nanosecond
// resolution -- so they are accepted and ignored.

#ifndef ORBITER_LINUX_MMSYSTEM_H
#define ORBITER_LINUX_MMSYSTEM_H

#ifdef _WIN32
#error "This header is for non-Windows builds only."
#endif

#include <windows.h>
#include <time.h>

typedef UINT MMRESULT;

#define TIMERR_NOERROR  0
#define TIMERR_NOCANDO  97

inline DWORD timeGetTime(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (DWORD)((uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull);
}

inline MMRESULT timeBeginPeriod(UINT) { return TIMERR_NOERROR; }
inline MMRESULT timeEndPeriod(UINT)   { return TIMERR_NOERROR; }

#endif // ORBITER_LINUX_MMSYSTEM_H
