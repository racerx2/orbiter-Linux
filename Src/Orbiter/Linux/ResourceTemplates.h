// Dialog templates converted from Orbiter.rc at build time.
//
// On Windows, rc.exe compiles Orbiter.rc into the executable's resource section
// and CreateDialogParam loads a template from there by id. An ELF binary has no
// resource section, so rc2cpp.py does the same conversion at build time and
// emits a C++ table, leaving Orbiter.rc itself unmodified as the single source
// of truth for both platforms. The converter runs it through the C preprocessor
// first, so every #define in resource.h resolves as the resource compiler would.
//
// Coordinates are dialog units, not pixels, exactly as stored in the .rc: the
// conversion depends on the dialog font and is done at layout time, when the
// font is known.

#ifndef ORBITER_LINUX_RESOURCETEMPLATES_H
#define ORBITER_LINUX_RESOURCETEMPLATES_H

#ifdef _WIN32
#error "This header is for non-Windows builds only."
#endif

#include <windows.h>

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
// disk, so the decode path sees the same input it would on Windows.
struct BitmapResource {
    int                  id;
    const char          *name;    // source filename, for diagnostics
    const char          *type;    // "BITMAP", "ICON", "IMAGE", "TEXT", ...
    const unsigned char *data;
    int                  size;
};

// A module's STRINGTABLE travels as `orbiterModuleStringBlob`, a flat
// NUL-separated char array:
//
//     "1\0Selected dock is already in use.\0"
//     "5\0Maintain fixed state vectors\0"
//     ...                                  and an empty id ends it.
//
// Two constraints force that shape, and the obvious
// `struct { int id; const char *text; }` array fails the second.
//
// 1. Per module, not global. Windows keeps these in the module's own resource
//    section and LoadString(hInst, id, ...) reads that module, so ids are
//    per-module and small ones collide immediately -- ScnEditor's IDS_ERR1 is
//    1. The ordered global registry the dialogs and bitmaps use cannot serve
//    them: the table has to be reached through the module, which
//    GetProcAddress on the HINSTANCE (a dlopen handle) does.
//
// 2. No pointers inside it. A module is not always dlopen'd:
//    ModuleTab::RefreshLists opens every candidate with
//    LOAD_LIBRARY_AS_DATAFILE, and Platform.cpp's GetProcAddress serves those
//    from a read-only mapping through orb_DatafileSymbol -- mapped, never
//    relocated, with no code in it having run. A `const char *` stored inside
//    such an image still holds its link-time address, so dereferencing it
//    reads whatever happens to be at that address in this process: the
//    struct-of-pointers version segfaulted in strlen on the Launchpad's
//    Modules tab, before the window even appeared.
//
// orbiterModuleDescription and orbiterModuleCategory (ids 1000 and 1001) are
// plain char arrays and unaffected; LoadStringA falls back to them, so a module
// declaring only those two -- every graphics client -- needs no blob.

// The generated table. Defined in the build-time generated DialogTemplates.cpp.
extern const DialogTemplate *const g_dialogTemplates;
extern const int                   g_dialogTemplateCount;

extern const BitmapResource *const g_bitmaps;
extern const int                   g_bitmapCount;

// Looks a template up by its IDD_* id. Returns null when the id is not a
// dialog, which is what CreateDialogParam reports as failure.
const DialogTemplate *FindDialogTemplate(int id);

// The same lookup, but asking the named module first, which is what Windows
// does: CreateDialogParam's first argument is the module the template is read
// from, so ids are per-module there and cannot collide. DeltaGlider.rc numbers
// its Scenario Editor pages IDD_EDITOR_PG1..PG3 = 174..176 and Orbiter.rc
// already uses 174, so an id-only search finds the executable's table first and
// the Delta-glider's "Animations" page opens with the core's dialog layout -- a
// wrong dialog, not an empty one, and silent.
//
// `moduleName` is matched case-insensitively; a null or unknown name falls back
// to the ordered search, so a caller whose HINSTANCE the shim cannot resolve
// still gets a template.
const DialogTemplate *FindDialogTemplateIn(const char *moduleName, int id);

// Looks a bitmap up by its IDB_* id; null when there is no such resource.
//
// Ids are not unique across types -- in Orbiter's own resource.h IDI_FINGER2 and
// IDR_IMAGE1 are both 292, and Windows keys a resource by (type, id), so those
// are two different objects there. Use this only where the id is known to be
// unambiguous; anything that knows the type it wants must use
// FindResourceOfType.
const BitmapResource *FindBitmapResource(int id);

// The (type, id) lookup Windows actually does. `type` is matched
// case-insensitively; a null type falls back to the id-only search.
const BitmapResource *FindResourceOfType(int id, const char *type);

// Decoders, both producing RGBA8, top-down. An ICON resource is the whole .ico
// file -- a directory of images at several sizes, in a mixture of encodings --
// and the caller picks one by index; see DecodeICO for why its DIB entries
// cannot go through DecodeBMP.
bool DecodeBMP(const unsigned char *data, int size,
               std::vector<unsigned char> &rgba, int &width, int &height);

int  CountICOImages(const unsigned char *data, int size);
bool GetICOImageSize(const unsigned char *data, int size, int index,
                     int &width, int &height);
bool DecodeICO(const unsigned char *data, int size, int index,
               std::vector<unsigned char> &rgba, int &width, int &height);

} // namespace orbiter_res

#endif // ORBITER_LINUX_RESOURCETEMPLATES_H
