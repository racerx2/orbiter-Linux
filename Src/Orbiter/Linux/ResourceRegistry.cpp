// ===========================================================================
// The resource lookups, and the registry of module resource tables.
// ===========================================================================
//
// On Windows rc.exe compiles every module's .rc into that module's own
// resource section, and CreateDialogParam takes the module as its first
// argument --
//
//     hDlg = oapiOpenDialogEx (hInst, IDD_EDITOR, EditorProc, 0, this);
//
// -- so Windows looks IDD_EDITOR up in that module alone. Every DLL has its
// own id namespace and its own templates, with no possibility of collision and
// no registration step.
//
// ELF has no resource section, so Linux/rc2cpp.py does the conversion at build
// time. Its --module mode emits a module's tables plus a static constructor
// that hands them to orbiter_RegisterModuleResources at dlopen -- before
// InitModule, so before anything can ask. A module whose .rc is not converted
// fails silently: Win32Dlg.cpp's CreateDialogParam ends in
//
//     const orbiter_res::DialogTemplate *tmpl = orbiter_res::FindDialogTemplate(id);
//     if (!tmpl) return nullptr;
//
// and oapiOpenDialogEx returns NULL with nothing drawn and nothing logged.
//
// One forced divergence: these lookups take an id and no module, because
// CreateDialogParam's HINSTANCE is not usable here -- Linux/Platform.cpp's
// GetModuleHandle returns a dlopen handle, not something a table can be keyed
// on, and Orbiter passes plugin hInst values around that the shim mints
// itself. So the search is ordered rather than scoped: the executable wins,
// then modules in registration order. That differs from Windows only where two
// modules use the same id for different dialogs and the caller's module cannot
// be identified; FindDialogTemplateIn narrows that as far as it can be
// narrowed.
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

// Function-local rather than file-scope: modules register from static
// constructors at dlopen, and a file-scope vector in the executable is not
// guaranteed to be constructed before a shared object's static constructors
// run. The first registrar to call constructs it.
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
	// The named module first: on Windows the template comes from the module
	// CreateDialogParam was given and from nowhere else. ResourceTemplates.h
	// records what the id-only search did to the Delta-glider's editor pages.
	if (moduleName && *moduleName) {
		for (const ModuleResources &m : registry()) {
			if (!m.name || strcasecmp(m.name, moduleName) != 0) continue;
			for (int i = 0; i < m.dialogCount; ++i)
				if (m.dialogs[i].id == id)
					return &m.dialogs[i];
			break;      // the module is registered and does not have it
		}
	}

	// Unknown module, or the module has no such id: fall back to the ordered
	// search.
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

// (type, id) is the real key, as it is on Windows. Anything reading a resource
// of a specific type must use this rather than FindBitmapResource, or it can
// pick up a different resource that happens to share the id -- in Orbiter's own
// resource.h, IDI_FINGER2 and IDR_IMAGE1 are both 292.
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
	// A module with no dialogs still registers, because it may have BITMAPs:
	// most MFDs have an icon and no window at all.
	if (dialogCount <= 0 && bitmapCount <= 0) return;

	// Ids colliding between modules is normal, not a mistake -- it is what
	// independent authors numbering from IDD_* upwards inevitably produce --
	// so nothing is warned about here. FindDialogTemplateIn resolves it by
	// asking the module CreateDialogParam named. The one case it cannot save
	// is two registrations of the same id where the caller's HINSTANCE
	// resolves to neither module, which is not detectable at this point.

	orbiter_res::registryAdd(name, dialogs, dialogCount, bitmaps, bitmapCount);
}
