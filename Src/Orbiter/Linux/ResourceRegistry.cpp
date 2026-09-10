// ===========================================================================
// The resource lookups, and the registry of MODULE resource tables.
// ===========================================================================
//
// WHAT THIS REPLACES ON WINDOWS. rc.exe compiles every module's .rc into that
// module's own resource section: Orbiter.rc into orbiter.exe, ScnEditor.rc into
// ScnEditor.dll, DeltaGlider.rc into DeltaGlider.dll. CreateDialogParam takes
// the module as its first argument --
//
//     hDlg = oapiOpenDialogEx (hInst, IDD_EDITOR, EditorProc, 0, this);
//
// -- and Windows looks IDD_EDITOR up in THAT module. Every DLL therefore has
// its own id namespace and its own templates, with no possibility of collision
// and no registration step.
//
// ELF has no resource section. Linux/rc2cpp.py performs the same conversion at
// build time and emits a C++ table, and until now it was run for exactly one
// file -- Src/Orbiter/Orbiter.rc -- so the executable had its dialogs and no
// module had any. A module's .rc was listed as a CMake source and silently
// ignored, because nothing on this platform compiles a .rc.
//
// WHAT THAT COST. Win32Dlg.cpp's CreateDialogParam ends in
//
//     const orbiter_res::DialogTemplate *tmpl = orbiter_res::FindDialogTemplate(id);
//     if (!tmpl) return nullptr;
//
// -- a silent null. Clicking ScnEdit on the F4 menu bar called
// ScnEditor::OpenDialog, which called oapiOpenDialogEx with IDD_EDITOR, which
// found nothing and returned NULL, and absolutely nothing happened or was
// logged. The same was true of all 13 Scenario Editor dialogs, TrackIR's 5,
// the DeltaGlider's 5, Meshdebug's, AtmConfig's, ShuttleA's and the two vessel
// configurators.
//
// THE SHAPE OF THE FIX. rc2cpp.py gains a --module mode: it emits the module's
// tables and a static constructor that hands them to
// orbiter_RegisterModuleResources at dlopen -- before InitModule, so before
// anything can ask. The lookups below search the executable's table first and
// then each registered module's, which is why they had to move out of the
// generated file and into this one.
//
// ONE DELIBERATE DIVERGENCE, AND IT IS FORCED. This shim's lookups take an id
// and no module, because CreateDialogParam's HINSTANCE is not usable here --
// Linux/Platform.cpp's GetModuleHandle returns a dlopen handle, not something
// a table can be keyed on, and Orbiter passes plugin hInst values around that
// the shim mints itself. So the search is ordered rather than scoped: the
// executable wins, then modules in registration order. That is only a
// difference where two modules use the SAME id for DIFFERENT dialogs, which
// Windows would resolve and this cannot; a collision is therefore reported
// once, by name, rather than silently resolved. Nothing in the tree currently
// collides -- see the warning below, which has never fired.
// ===========================================================================

#include "ResourceTemplates.h"

#include <strings.h>   // strcasecmp, for the resource type match
#include <stdio.h>
#include <vector>

namespace orbiter_res {

namespace {

struct ModuleResources {
	const char           *name;
	const DialogTemplate *dialogs;
	int                   dialogCount;
	const BitmapResource *bitmaps;
	int                   bitmapCount;
};

// A function-local static rather than a file-scope one: modules register from
// static constructors at dlopen, and a plain file-scope vector in the
// executable is not guaranteed to be constructed before a shared object's
// static constructors run. The first registrar to call constructs it.
std::vector<ModuleResources> &registry()
{
	static std::vector<ModuleResources> v;
	return v;
}

} // namespace

// Appends to the registry. Not in the anonymous namespace above because
// orbiter_RegisterModuleResources, which lives outside orbiter_res, calls it.
void registryAdd(const char *name,
                 const DialogTemplate *dialogs, int dialogCount,
                 const BitmapResource *bitmaps, int bitmapCount)
{
	registry().push_back(ModuleResources{ name, dialogs, dialogCount,
	                                      bitmaps, bitmapCount });
}

const DialogTemplate *FindDialogTemplate(int id)
{
	for (int i = 0; i < g_dialogTemplateCount; ++i)
		if (g_dialogTemplates[i].id == id)
			return &g_dialogTemplates[i];

	for (const ModuleResources &m : registry())
		for (int i = 0; i < m.dialogCount; ++i)
			if (m.dialogs[i].id == id)
				return &m.dialogs[i];

	return nullptr;
}

const DialogTemplate *FindDialogTemplateIn(const char *moduleName, int id)
{
	// The named module first, which is the whole point: on Windows the
	// template comes from the module CreateDialogParam was given and from
	// nowhere else. See the declaration in ResourceTemplates.h for what the
	// id-only search did to the Delta-glider's editor pages.
	if (moduleName && *moduleName) {
		for (const ModuleResources &m : registry()) {
			if (!m.name || strcasecmp(m.name, moduleName) != 0) continue;
			for (int i = 0; i < m.dialogCount; ++i)
				if (m.dialogs[i].id == id)
					return &m.dialogs[i];
			break;      // the module is registered and does not have it
		}
	}

	// Unknown module, or the module has no such id: the ordered search, which
	// is what every caller got before there was any scoping at all.
	return FindDialogTemplate(id);
}

const BitmapResource *FindBitmapResource(int id)
{
	for (int i = 0; i < g_bitmapCount; ++i)
		if (g_bitmaps[i].id == id)
			return &g_bitmaps[i];

	for (const ModuleResources &m : registry())
		for (int i = 0; i < m.bitmapCount; ++i)
			if (m.bitmaps[i].id == id)
				return &m.bitmaps[i];

	return nullptr;
}

// (type, id) is the real key, as it is on Windows. FindBitmapResource above
// matches on id alone and is kept for callers that know their id is
// unambiguous; anything reading a resource of a specific type must use this
// one or it can pick up a different resource that happens to share the id.
// Orbiter's own resource.h proves the hazard: IDI_FINGER2 and IDR_IMAGE1 are
// both 292.
const BitmapResource *FindResourceOfType(int id, const char *type)
{
	if (!type) return FindBitmapResource(id);

	for (int i = 0; i < g_bitmapCount; ++i) {
		if (g_bitmaps[i].id != id) continue;
		if (strcasecmp(g_bitmaps[i].type, type) == 0)
			return &g_bitmaps[i];
	}

	for (const ModuleResources &m : registry()) {
		for (int i = 0; i < m.bitmapCount; ++i) {
			if (m.bitmaps[i].id != id) continue;
			if (strcasecmp(m.bitmaps[i].type, type) == 0)
				return &m.bitmaps[i];
		}
	}

	return nullptr;
}

} // namespace orbiter_res

// The entry point rc2cpp.py's --module output calls from its static
// constructor. extern "C" so it resolves out of the executable's dynamic
// symbol table exactly like every other orbiter_* entry point a module uses.
extern "C" void orbiter_RegisterModuleResources(
	const char *name,
	const orbiter_res::DialogTemplate *dialogs, int dialogCount,
	const orbiter_res::BitmapResource *bitmaps, int bitmapCount)
{
	// A module with no dialogs still registers, because it may have BITMAPs.
	// This tested dialogs alone, from when the only converted module was the
	// Scenario Editor and a dialog-less one could not occur; now every module
	// is converted and most MFDs have an icon and no window at all.
	if (dialogCount <= 0 && bitmapCount <= 0) return;

	// COLLIDING IDS ARE NORMAL AND ARE RESOLVED, NOT REPORTED.
	//
	// This used to warn on every id already claimed, because the lookup was
	// keyed on the id alone and a collision really did mean one module getting
	// another's layout. Ids are per-module on Windows, so overlap is not a
	// mistake -- it is what independent authors numbering from IDD_* upwards
	// inevitably produce, and with every module converted rather than one, the
	// warning fired constantly for a condition that is now handled:
	// FindDialogTemplateIn asks the module CreateDialogParam named, and only
	// falls back to the ordered search when that module has no such id.
	//
	// What remains worth saying is the case the scoping cannot save: two
	// registrations of the SAME id where the caller's HINSTANCE resolves to
	// neither module. That is not detectable here, so nothing is printed; the
	// fallback path is documented at FindDialogTemplateIn instead.

	orbiter_res::registryAdd(name, dialogs, dialogCount, bitmaps, bitmapCount);
}
