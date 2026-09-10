// Linux <psapi.h> — process memory statistics.
//
// Included by Src/Orbiter/Memstat.h, which uses exactly one entry point:
// GetProcessMemoryInfo, to report heap usage in the debug overlay.
//
// Memstat.cpp resolves it dynamically:
//
//     hLib = LoadLibrary("Psapi.dll");
//     pGetProcessMemoryInfo = GetProcAddress(hLib, "GetProcessMemoryInfo");
//
// That path is preserved rather than bypassed. The Linux LoadLibrary returns a
// sentinel handle for "Psapi.dll", and GetProcAddress resolves through dlsym
// against the executable's own dynamic symbol table -- which works because the
// Orbiter target is linked with -rdynamic. So the function below is found by
// name at runtime exactly as the Windows one is, and Memstat.cpp needs no edit.
//
// WorkingSetSize is the field Orbiter reads. The Linux equivalent is the
// resident set size from /proc/self/statm, whose second field is RSS in pages.

#ifndef ORBITER_LINUX_PSAPI_H
#define ORBITER_LINUX_PSAPI_H

#ifdef _WIN32
#error "This header is for non-Windows builds only."
#endif

#include <windows.h>

typedef struct _PROCESS_MEMORY_COUNTERS {
    DWORD  cb;
    DWORD  PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
} PROCESS_MEMORY_COUNTERS, *PPROCESS_MEMORY_COUNTERS;

// The extended form, added for OVP/VulkanClient's control panel, which reports
// PrivateUsage -- a field the base structure does not carry.
//
// ON WINDOWS THIS IS NOT A SECOND FUNCTION. GetProcessMemoryInfo fills
// whichever of the two structures it is handed and tells them apart by the
// 'cb' byte count alone, which is why the call site casts the EX form down to
// PPROCESS_MEMORY_COUNTERS and passes its own sizeof:
//
//     PROCESS_MEMORY_COUNTERS_EX memstats;
//     memstats.cb = sizeof(PROCESS_MEMORY_COUNTERS_EX);
//     GetProcessMemoryInfo(GetCurrentProcess(),
//                          (PPROCESS_MEMORY_COUNTERS)&memstats, sizeof(memstats));
//
// Platform.cpp makes the same test for the same reason, so that call needs no
// edit and neither does any existing caller of the base form.
//
// PrivateUsage on Windows is the process's private COMMIT charge: address
// space that cannot be shared with another process. Its Linux counterpart is
// /proc/self/statm's sixth field, "data" -- VmData + VmStk in pages, the
// private writable address space. It is deliberately NOT the resident set:
// WorkingSetSize above is already that, and the two answer different
// questions.
typedef struct _PROCESS_MEMORY_COUNTERS_EX {
    DWORD  cb;
    DWORD  PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivateUsage;
} PROCESS_MEMORY_COUNTERS_EX, *PPROCESS_MEMORY_COUNTERS_EX;

// Src/Orbiter/Log.cpp walks the loaded modules to record which plugins were
// active when a crash was logged. On Linux the module list comes from
// dl_iterate_phdr, which reports the same thing: base address, span and path
// of every object mapped into the process.
typedef struct _MODULEINFO {
    LPVOID lpBaseOfDll;
    DWORD  SizeOfImage;
    LPVOID EntryPoint;
} MODULEINFO, *LPMODULEINFO;

extern "C" {

// Declared with default visibility so it lands in the executable's dynamic
// symbol table and stays reachable through GetProcAddress/dlsym.
__attribute__((visibility("default")))
BOOL GetProcessMemoryInfo (HANDLE process,
                           PPROCESS_MEMORY_COUNTERS counters,
                           DWORD cb);

BOOL  EnumProcessModules   (HANDLE process, HMODULE *modules, DWORD cb,
                            LPDWORD needed);
BOOL  GetModuleInformation (HANDLE process, HMODULE mod,
                            LPMODULEINFO info, DWORD cb);
DWORD GetModuleFileNameExA (HANDLE process, HMODULE mod, LPSTR name,
                            DWORD size);

} // extern "C"

#define GetModuleFileNameEx GetModuleFileNameExA

#endif // ORBITER_LINUX_PSAPI_H
