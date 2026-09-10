// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "Memstat.h"

#ifndef _WIN32
#include <malloc.h>   // mallinfo2; see HeapUsage
#endif

bool MemStat::bLib = false;
HMODULE MemStat::hLib = 0;

MemStat::MemStat ()
{
	if (!bLib) {
		hLib = LoadLibrary ("Psapi.dll");
		bLib = true;
	}
    hProc = GetCurrentProcess();
    active = (hLib != NULL && hProc != NULL);
	if (active) {
		pGetProcessMemoryInfo = (Proc_GetProcessMemoryInfo)GetProcAddress (hLib, "GetProcessMemoryInfo");
	} else {
		pGetProcessMemoryInfo = 0;
	}
}

MemStat::~MemStat ()
{
    if (hProc) CloseHandle (hProc);
}

long MemStat::HeapUsage ()
{
#ifndef _WIN32
	// ===================================================================
	// WORKING SET IS THE WRONG SIGNAL ON GLIBC, AND THE NAME SAYS SO
	// ===================================================================
	//
	// Both callers want a number that GOES DOWN when memory is released:
	//
	//   Orbiter::Launch          simheapsize = HeapUsage() - m0, i.e. how much
	//                            the world cost to build
	//   UpdateWaitProgress       (mem0 - HeapUsage()) / mem_wait, the wait
	//                            page's progress bar as the world is destroyed
	//
	// On Windows `WorkingSetSize` supplies that, because the Windows heap
	// decommits pages back to the OS as blocks are freed, so the resident set
	// follows the application's allocations down.
	//
	// GLIBC DOES NOT. free() returns a block to the allocator's arena, not to
	// the kernel; RSS only falls for chunks large enough to have been mmap'd.
	// So the literal translation of "working set" -- /proc/self/statm's
	// resident field, which is what this shim's GetProcessMemoryInfo honestly
	// reports -- stays flat or drifts UP while the world is torn down.
	//
	// MEASURED, with the RSS version, over a whole DestroyWorld: forty
	// PBM_SETPOS messages reached the bar and every one of them carried
	//
	//     [msg] progress id=1291 msg=0x0402 wp=-6 ...
	//
	// -- (mem0 - mem) came out NEGATIVE, so the bar clamped at 0 and never
	// moved. Every layer beneath it was correct; the quantity was wrong.
	//
	// mallinfo2().uordblks is the direct answer to the question the method's
	// NAME asks: bytes currently allocated to the application. It falls the
	// moment a block is freed, whatever the kernel does with the pages, which
	// is precisely the Windows behaviour being ported. hblkhd is added because
	// large allocations are served by mmap and counted separately -- Orbiter's
	// meshes and textures are exactly that size class, so leaving it out would
	// miss most of what a scenario allocates.
	//
	// GetProcessMemoryInfo is deliberately NOT changed to return this. It is a
	// documented API whose contract is the resident set, it reports
	// PagefileUsage and PeakWorkingSetSize from the same source, and a future
	// caller asking for a working set should get one. The divergence belongs
	// in the function whose name promises heap usage.
#if defined(__GLIBC__) && \
    (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
	struct mallinfo2 mi = mallinfo2();
	return (long)(mi.uordblks + mi.hblkhd);
#else
	// mallinfo2 arrived in glibc 2.33. The older mallinfo returns int fields
	// that wrap past 2 GB, which a loaded scenario can exceed -- so on an
	// older library fall through to the working set and accept that the wait
	// bar will not move, rather than report a wrapped number that would make
	// simheapsize meaningless too.
	if (pGetProcessMemoryInfo) {
		PROCESS_MEMORY_COUNTERS pmc;
		pGetProcessMemoryInfo (hProc, &pmc, sizeof(pmc));
		return (long)pmc.WorkingSetSize;
	}
	return 0;
#endif
#else
	if (pGetProcessMemoryInfo) {
	    PROCESS_MEMORY_COUNTERS pmc;
		pGetProcessMemoryInfo (hProc, &pmc, sizeof(pmc));
		return (long)pmc.WorkingSetSize;
	} else return 0;
#endif
}