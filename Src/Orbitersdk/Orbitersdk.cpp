// Copyright (c) Martin Schweiger
// Licensed under the MIT License

// ========================================================================
// To be linked into all Orbiter addon modules.
// Contains standard module entry point and version information.
// ========================================================================

#include <windows.h>
#include <fstream>
#include <stdio.h>

// On ELF, symbol export is controlled by visibility rather than by an
// import/export keyword: the executable exports with default visibility and
// modules bind at load time. The macros therefore carry no decoration there,
// leaving the Windows dllexport/dllimport path unchanged.
#ifdef _WIN32
#define DLLCLBK extern "C" __declspec(dllexport)
#define OAPIFUNC __declspec(dllimport)
#else
#define DLLCLBK extern "C" __attribute__((visibility("default")))
#define OAPIFUNC
#endif

// InitLib and ExitLib are defined in the Orbiter executable
// (Src/Orbiter/OrbiterAPI.cpp) and bound at load time.
//
// DECLARED HERE, AT FILE SCOPE, AND DELIBERATELY OUTSIDE ANY extern "C".
//
// They used to be declared at BLOCK scope, inside the functions that call
// them, and that was a trap with a very long fuse. A block-scope function
// declaration still declares a namespace-scope entity -- and one written
// inside the body of an `extern "C"` function inherits C language linkage.
// orbiter_module_attach is exactly that, so its local
//
//     OAPIFUNC void InitLib (HINSTANCE hModule);
//
// asked for the UNMANGLED symbol `InitLib`, while the executable exports the
// C++-mangled `_Z7InitLibP11HINSTANCE__`.
//
// It linked anyway, for a reason that is pure accident: DllMain -- ordinary
// C++ linkage -- contained the same declaration and appeared EARLIER in the
// file, so it declared ::InitLib with C++ linkage first and the later one
// merely matched it. Guarding DllMain to Windows removed that first
// declaration and the reference silently became unmangled:
//
//     libOrbitersdk.a(Orbitersdk.cpp.o): in function `orbiter_module_attach':
//     undefined reference to `InitLib'
//
// which is a link failure of the Orbiter executable itself, caused by deleting
// a function that was never called. One file-scope declaration removes the
// order dependence entirely.
OAPIFUNC void InitLib (HINSTANCE hModule);
OAPIFUNC void ExitLib (HINSTANCE hModule);

// WINDOWS ONLY, and the guard is a link fix as much as a tidy-up.
//
// On ELF nothing ever calls this. Windows invokes DllMain from LoadLibrary;
// there is no such hook here, which is the whole reason the constructor path
// below exists. So on Linux this function was dead code -- but it was dead
// code that still DEFINED THE SYMBOL, and that is not free:
//
// -Wl,-u,orbiter_module_attach forces this object file out of
// libOrbitersdk.a into every module. Any module that legitimately defines its
// own DllMain then has two definitions in one link, and ELF -- unlike a
// Windows import library, where an archive member is only pulled when it
// resolves something -- reports
//
//     libOrbitersdk.a(Orbitersdk.cpp.o): multiple definition of
//     `DllMain(HINSTANCE__*, unsigned int, void*)';
//     Dragonfly.cpp:1007: first defined here
//
// Src/Vessel/Dragonfly is the module that hits it. Its own DllMain is left
// exactly as it is: on Windows it does its work as always, and on ELF it is
// harmlessly dead, because everything it does is already true at load time --
// PANEL_DLLAtach() only clears Panel_Resources_Loaded, a zero-initialised
// global, and the hDLL it stores is only ever passed to
// PANEL_InitGDIResources, which ignores the parameter.
//
// The ELF replacement for all of this is orbiter_module_attach below, run by
// the dynamic linker as a constructor.
#ifdef _WIN32
BOOL WINAPI DllMain (HINSTANCE hModule,
					 DWORD ul_reason_for_call,
					 LPVOID lpReserved)
{
	// Left exactly as upstream has it. The file-scope declaration above
	// already covers this call; redeclaring here is redundant but it keeps
	// this function byte-identical to the Win32 reference, which is worth
	// more than the saved line.
	OAPIFUNC void InitLib (HINSTANCE hModule);
	typedef void (*DLLEXIT)(HINSTANCE);
	static DLLEXIT DLLExit;

	switch (ul_reason_for_call) {
	case DLL_PROCESS_ATTACH:
		InitLib (hModule);
		DLLExit = (DLLEXIT)GetProcAddress (hModule, "ExitModule");
		if (!DLLExit) DLLExit = (DLLEXIT)GetProcAddress (hModule, "opcDLLExit");
		break;
	case DLL_PROCESS_DETACH:
		if (DLLExit) (*DLLExit)(hModule);
		break;
	}
	return TRUE;
}
#endif

#ifndef _WIN32
// ELF equivalent of DllMain.
//
// Windows calls DllMain automatically on LoadLibrary; ELF has no such hook, so
// on Linux DllMain above was dead code and InitLib -- and through it every
// module's InitModule -- NEVER RAN. That is not a cosmetic gap:
//
//   Satsat's InitModule is what reads Config/Saturn/Data/tass17.dat into the
//   shared TASS 1.7 tables. Without it every Saturnian moon constructed
//   itself against empty tables and logged "SATSAT <moon>: Terms 0", where
//   Windows reports 113, 33, 101, 59, 68, 100 and 605. The moons had no
//   ephemeris at all and nothing said so.
//
// __attribute__((constructor)) / ((destructor)) are run by the dynamic linker
// at dlopen and dlclose, which is exactly when DLL_PROCESS_ATTACH and
// DLL_PROCESS_DETACH fire on Windows.
//
// hModule must be a handle GetProcAddress can resolve against, because DllMain
// uses it to find ExitModule. dladdr gives this object's path and
// RTLD_NOLOAD reopens it without loading a second copy, yielding a handle to
// ourselves.

#include <dlfcn.h>

// Implemented in Src/Orbiter/Linux/Platform.cpp and bound from the
// executable at dlopen time, like every other shim entry point.
extern "C" HMODULE orbiter_WrapDlHandle(void *dl, const char *path);
// Non-zero while the Orbiter core still exists; see LinuxMain.cpp.
extern "C" int orbiter_CoreAlive(void);

static HINSTANCE orb_self_module()
{
	// A handle to THIS shared object, without dlopen.
	//
	// The obvious spelling -- dladdr for the path, then LoadLibraryA on it --
	// returns null, because it asks the loader to dlopen a library that is
	// still running its own constructors. The trace showed
	//     [init] attach self=(nil) InitModule=(nil)
	// for the first module loaded, so InitLib had no handle, GetProcAddress
	// found nothing, InitModule was never called, oapiRegisterModule never
	// ran, and Orbiter logged "(legacy interface)". XRSound then never
	// received clbkSimulationStart -- which is where it initialises irrKlang
	// and announces itself -- so it produced no sound at all.
	//
	// dl_iterate_phdr and dladdr both work mid-construction because neither
	// loads anything; they only read what the loader already has. The handle
	// is built directly from the path, which is all GetModuleFileName and
	// GetProcAddress need.
	Dl_info info;
	if (!dladdr((void *)&orb_self_module, &info) || !info.dli_fname)
		return nullptr;

	// RTLD_NOLOAD returns a handle for an already-loaded object without
	// initialising it again, and unlike a plain dlopen it is safe here: it
	// never triggers a load, so there is no recursion into our own init.
	if (void *h = dlopen(info.dli_fname, RTLD_LAZY | RTLD_NOLOAD)) {
		// ==============================================================
		// THIS REFERENCE IS KEPT ON PURPOSE. IT PINS THE MODULE IN MEMORY.
		// ==============================================================
		//
		// RTLD_NOLOAD does not load, but it DOES take a reference on an
		// object that is already loaded, and this one is never dropped. So
		// every module sits at refcount 2 -- one from Orbiter's
		// LoadLibrary, one from here -- and Orbiter::UnloadModule's single
		// FreeLibrary takes it to 1, leaving the library mapped.
		//
		// That reads like a leak and it was briefly "fixed" by dlclosing
		// here. It is not a leak; it is what keeps the process alive.
		//
		// MEASURED, from the core dump of the very next session exit:
		//
		//   Orbiter::CloseApp
		//     -> Orbiter::UnloadModule -> FreeLibrary -> dlclose
		//        -> orb_module_detach -> ExitModule (LuaInline)
		//           -> ~InterpreterList -> ~Environment -> WaitForSingleObject
		//   ... while another thread sat in
		//   InterpreterList::Environment::InterpreterThreadProc, and a third
		//   had already faulted at an address with no symbol behind it.
		//
		// LuaInline runs one interpreter THREAD per environment and tears
		// them down one at a time; XRSound and LuaMFD are the same shape.
		// Unmapping the library takes the code those threads are executing
		// out from under them, and the crash lands in ?? () where nothing
		// can be read from it. On Windows FreeLibrary really does unload and
		// this is survivable because TerminateThread genuinely kills a
		// thread; pthread_cancel is not that, so the same sequence here is
		// a use-after-unmap.
		//
		// The two things unloading was wanted for do not actually need it,
		// and Src/Orbiter/Linux/Platform.cpp does both without it: it calls
		// ExitLib itself when it sees the object is still resident after
		// dlclose, and InitLib itself when a resident object is loaded
		// again. The entry points therefore run at exactly the moments
		// Windows runs them, and nothing is ever pulled out from under a
		// running thread.
		return orbiter_WrapDlHandle(h, info.dli_fname);
	}

	return nullptr;
}

static HINSTANCE g_orb_self = nullptr;

// The module's own entry points, declared WEAK.
//
// InitLib finds these with GetProcAddress(hModule, "InitModule"), and hModule
// comes from dlopen-ing this library from inside its own ELF constructor --
// while it is still mid-initialisation, which is precisely when a dlsym
// against it cannot be relied on. When that lookup failed, InitLib simply did
// nothing: InitModule was never called, oapiRegisterModule never ran, and
// Orbiter::LoadModule saw a null register_module and logged
//     Loading module XRSound (legacy interface)
// where Windows logs "Loading module XRSound". A module loaded that way gets
// no clbkSimulationStart and no clbkPreStep, so XRSound registered nothing and
// played nothing.
//
// A weak declaration is resolved by the LINKER within this shared object, so
// no runtime lookup is involved at all. It is null when the module defines no
// such function, which is what the legacy path is for.
extern "C" __attribute__((weak)) void InitModule(HINSTANCE);
extern "C" __attribute__((weak)) void ExitModule(HINSTANCE);
extern "C" __attribute__((weak)) void opcDLLInit(HINSTANCE);
extern "C" __attribute__((weak)) void opcDLLExit(HINSTANCE);

// Not static, so -Wl,-u,orbiter_module_attach can name it and force this
// object file out of libOrbitersdk.a. HIDDEN visibility is essential: with
// default visibility Galsat.so and Satsat.so export the symbol, and a module
// that links against them satisfies the -u reference from the shared library
// instead of pulling the archive member -- which is exactly why Jupiter, Io
// and the Saturnian moons still linked without DllMain. Hidden keeps it out
// of every dynamic symbol table, so each module must supply its own.
extern "C" __attribute__((visibility("hidden")))
void orbiter_module_attach(void)
{
	g_orb_self = orb_self_module();

	if (getenv("ORBITER_TRACE_MODULES"))
		fprintf(stderr, "[mod] attach self=%p InitModule=%p opcDLLInit=%p\n",
		        (void *)g_orb_self, (void *)InitModule, (void *)opcDLLInit);

	// The log line InitLib writes needs a handle; the entry points do not,
	// and are called directly below.
	// InitLib does the logging AND calls InitModule/opcDLLInit itself, via
	// GetProcAddress. Calling them again here initialised every module twice
	// -- XRSound parsed its config file twice over.
	//
	// InitLib is declared at FILE scope, not here: a declaration inside this
	// function would inherit its extern "C" linkage and ask the linker for an
	// unmangled symbol. See the note by the declaration.
	InitLib(g_orb_self);
}

__attribute__((constructor))
static void orb_module_attach_ctor(void)
{
	orbiter_module_attach();
}

__attribute__((destructor))
static void orb_module_detach(void)
{
	// Only while the core is alive.
	//
	// This runs from _dl_fini for any module still loaded at process exit --
	// after main has returned and Orbiter has been destroyed. ExitModule then
	// calls back into the core: Meshdebug's calls oapiUnregisterCustomCmd,
	// which dereferenced a dead g_pOrbiter and segfaulted on every exit.
	//
	// A module unloaded normally, through FreeLibrary, still gets its
	// ExitModule at the right moment, because dlclose runs this destructor
	// while the core is very much alive.
	// ORBITER_TRACE_MODULES shows whether this ran at all, which is the only
	// way to tell a module that was never unloaded from one whose ExitModule
	// did nothing. Both look identical from outside.
	if (getenv("ORBITER_TRACE_MODULES"))
		fprintf(stderr, "[mod] detach self=%p coreAlive=%d ExitModule=%p "
		                "opcDLLExit=%p\n",
		        (void *)g_orb_self, orbiter_CoreAlive(),
		        (void *)ExitModule, (void *)opcDLLExit);

	if (!orbiter_CoreAlive()) return;

	// THE WEAK SYMBOLS, NOT ExitLib.
	//
	// ExitLib dispatches through GetProcAddress(hModule, "ExitModule"), which
	// is dlsym on this object's own handle -- and this runs from _dl_fini for
	// this object, i.e. while it is being torn down. That lookup happening to
	// work is not something to depend on when the alternative costs nothing:
	// the weak declarations above are resolved by the LINKER inside this
	// shared object, so there is no runtime lookup at all, and a module that
	// defines neither leaves both null.
	//
	// This became reachable only when orb_self_module stopped holding a
	// reference; before that the destructor never ran with the core alive.
	if (ExitModule)      ExitModule(g_orb_self);
	else if (opcDLLExit) opcDLLExit(g_orb_self);
}
#endif

int oapiGetModuleVersion ()
{
	static int v = 0;
	if (!v) {
		OAPIFUNC int Date2Int (char *date);
		v = Date2Int ((char*)__DATE__);
	}
	return v;
}

DLLCLBK int GetModuleVersion (void)
{
	return oapiGetModuleVersion();
}

void dummy () {}

