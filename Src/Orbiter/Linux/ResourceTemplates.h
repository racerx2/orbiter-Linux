// Dialog templates converted from Orbiter.rc at build time.
//
// On Windows, rc.exe compiles Orbiter.rc into the executable's resource
// section and CreateDialogParam loads a template from there by id. There is no
// resource section in an ELF binary, so rc2cpp.py performs the same conversion
// at build time and emits a C++ table which is compiled in. The result is the
// same in the way that matters: the templates travel inside the binary, with
// no runtime file to locate or keep in sync.
//
// Orbiter.rc itself is never modified. It stays the single source of truth for
// both platforms, and the converter runs it through the C preprocessor first,
// so every #define in resource.h -- the IDC_* control ids, LAUNCHPAD_WIN_WIDTH
// and the arithmetic built on it -- resolves exactly as the resource compiler
// would resolve it.
//
// COORDINATES
//   Dialog units, not pixels, exactly as stored in the .rc. The conversion to
//   pixels depends on the dialog font and is done at layout time, because that
//   is when the font is known. Storing raw dialog units keeps this table a
//   faithful copy of the template rather than a pre-baked layout.

#ifndef ORBITER_LINUX_RESOURCETEMPLATES_H
#define ORBITER_LINUX_RESOURCETEMPLATES_H

#ifdef _WIN32
#error "This header is for non-Windows builds only."
#endif

#include <windows.h>

// The decoders below hand back pixels in a std::vector.
#include <vector>

namespace orbiter_res {

// One control within a dialog template.
struct ControlTemplate {
    const char *className;   // "Button", "Static", "SysTreeView32", ...
    const char *text;        // caption; empty string when the control has none
    int         id;          // control id, as IDC_* resolved to a number
    int         x, y, cx, cy;// position and size, in dialog units
    DWORD       style;       // WS_* | class-specific style bits
    DWORD       exStyle;
    int         bitmapId;    // for SS_BITMAP controls whose text slot is an id;
                             // 0 when unused
};

// One dialog template.
struct DialogTemplate {
    int                    id;        // IDD_* resolved to a number
    const char            *caption;
    int                    x, y, cx, cy;
    DWORD                  style;
    DWORD                  exStyle;
    const char            *fontName;
    int                    fontSize;
    const ControlTemplate *controls;
    int                    controlCount;
};

// One BITMAP resource: the raw .bmp file bytes, exactly as they appear on
// disk. rc.exe embeds them in the resource section on Windows and LoadBitmap
// decodes from there; the same bytes travel in the executable here, so the
// decode path sees identical input.
struct BitmapResource {
    int                  id;
    const char          *name;    // source filename, for diagnostics
    const char          *type;    // "BITMAP", "ICON", "IMAGE", "TEXT", ...
    const unsigned char *data;
    int                  size;
};

// A MODULE'S STRINGTABLE travels as `orbiterModuleStringBlob`, a flat
// NUL-separated char array:
//
//     "1\0Selected dock is already in use.\0"
//     "5\0Maintain fixed state vectors\0"
//     ...                                  and an empty id ends it.
//
// TWO CONSTRAINTS FORCED THIS SHAPE, and both are worth stating because the
// obvious `struct { int id; const char *text; }` array fails the second.
//
// 1. PER MODULE, NOT GLOBAL. Windows keeps these in the module's own resource
//    section and LoadString(hInst, id, ...) reads that module, so ids are
//    per-module and small ones collide immediately -- ScnEditor's IDS_ERR1 is
//    1. The ordered global registry the dialogs and bitmaps use
//    (ResourceRegistry.cpp) therefore cannot serve them: the table has to be
//    reached THROUGH the module. GetProcAddress on the HINSTANCE does that,
//    the HINSTANCE being a dlopen handle.
//
// 2. NO POINTERS INSIDE IT. A module is not always dlopen'd:
//    ModuleTab::RefreshLists opens every candidate with
//    LOAD_LIBRARY_AS_DATAFILE, and Platform.cpp's GetProcAddress serves those
//    from a READ-ONLY MAPPING through orb_DatafileSymbol -- the image is
//    mapped, never relocated, and no code in it has run. A `const char *`
//    stored inside such an image still holds its LINK-TIME address, so
//    dereferencing it reads whatever happens to be at that address in this
//    process. The struct-of-pointers version segfaulted in strlen on the
//    Launchpad's Modules tab, before the window even appeared. A flat blob
//    has nothing to relocate and reads correctly from both a dlopen handle
//    and a datafile mapping.
//
// orbiterModuleDescription and orbiterModuleCategory (ids 1000 and 1001) are
// unaffected and still work: they are plain char arrays, which is exactly why
// they always did. LoadStringA falls back to them, so a module that declares
// only those two -- every graphics client -- needs no blob.

// The generated table. Defined in the build-time generated DialogTemplates.cpp.
extern const DialogTemplate *const g_dialogTemplates;
extern const int                   g_dialogTemplateCount;

extern const BitmapResource *const g_bitmaps;
extern const int                   g_bitmapCount;

// Looks a template up by its IDD_* id. Returns null when the id is not a
// dialog, which is what CreateDialogParam reports as failure.
const DialogTemplate *FindDialogTemplate(int id);

// The same lookup, but asking the NAMED MODULE FIRST -- which is what Windows
// does and nothing else: CreateDialogParam's first argument is the module the
// template is read from, so ids are per-module there and cannot collide.
//
// This is not a refinement, it is a correctness fix, and the Delta-glider
// proved it. DeltaGlider.rc numbers its Scenario Editor pages
// IDD_EDITOR_PG1..PG3 = 174..176, and Orbiter.rc already uses 174 for a
// dialog of its own. The id-only search finds the executable's table first,
// so the Delta-glider's "Animations" page opened with the CORE's dialog
// layout -- a wrong dialog, not an empty one, and silent.
//
// `moduleName` is matched against the registered module's name without regard
// to case; a null or unknown name falls back to the ordered search, so a
// caller whose HINSTANCE the shim cannot resolve behaves exactly as before.
const DialogTemplate *FindDialogTemplateIn(const char *moduleName, int id);

// Looks a bitmap up by its IDB_* id; null when there is no such resource.
//
// IDS ARE NOT UNIQUE ACROSS TYPES, and Orbiter's own resource.h proves it:
// IDI_FINGER2 and IDR_IMAGE1 are both 292. Windows keys a resource by
// (type, id), so those are two different objects there. Use this only where
// the id is known to be unambiguous; anything that knows the type it wants --
// LoadBitmap, LoadIcon, FindResource -- must use FindResourceOfType.
const BitmapResource *FindBitmapResource(int id);

// The (type, id) lookup Windows actually does. `type` is matched
// case-insensitively; a null type falls back to the id-only search.
const BitmapResource *FindResourceOfType(int id, const char *type);

// ---------------------------------------------------------------------------
// Decoders. Both produce RGBA8, top-down.
//
// An ICON resource is the whole .ico file -- a directory of images at several
// sizes, in a mixture of encodings. The caller picks one by index; see the
// note above DecodeICO for why its DIB entries cannot go through DecodeBMP.
// ---------------------------------------------------------------------------
bool DecodeBMP(const unsigned char *data, int size,
               std::vector<unsigned char> &rgba, int &width, int &height);

int  CountICOImages(const unsigned char *data, int size);
bool GetICOImageSize(const unsigned char *data, int size, int index,
                     int &width, int &height);
bool DecodeICO(const unsigned char *data, int size, int index,
               std::vector<unsigned char> &rgba, int &width, int &height);

} // namespace orbiter_res

#endif // ORBITER_LINUX_RESOURCETEMPLATES_H
