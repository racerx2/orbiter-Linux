// GDI implementation over ImGui draw lists.
//
// DlgCtrl's gauges and switches, and the Launchpad's owner-drawn tab strip,
// paint themselves with pens, brushes and BitBlt in response to WM_PAINT.
// Those sources are unmodified, so the GDI calls they make have to land
// somewhere. They land in an ImGui draw list.
//
// THE MODEL
//   A device context here is a recording surface, not a window. BeginPaint
//   hands out a DC bound to a control; every drawing call appends to that DC's
//   command list; EndPaint marks it complete. The frame pump then replays the
//   completed list into the ImGui draw list for that control's rectangle.
//
//   Recording rather than drawing immediately is what makes this work at all.
//   WM_PAINT arrives when a message is dispatched, which is not the same
//   moment as the frame being built -- ImGui's draw list is only valid inside
//   a frame, so painting straight into it from a message handler would either
//   be discarded or corrupt the frame.
//
// SELECTED OBJECTS
//   GDI is a state machine: SelectObject installs a pen or brush and every
//   later call uses it implicitly. That state lives on the DC here, and each
//   recorded command captures the pen and brush current at the time it was
//   recorded, because they may be swapped again before the frame is replayed.
//
// COORDINATES
//   Control-relative pixels, as GDI uses. The frame pump adds the control's
//   screen origin when it replays.

#include <windows.h>
#include <commctrl.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <strings.h>        // strcasecmp, for the case-insensitive file retry

#include "imgui.h"
#include "ResourceTemplates.h"

// Implemented in UIHost.cpp, which owns the Vulkan device. Returns the
// renderer texture handle, or 0 if the renderer is not up yet.
extern "C" unsigned long long orbiter_UploadTexture(
    const unsigned char *rgba, int width, int height);

// Implemented in Win32Dlg.cpp, which owns the tree-view state.
extern "C" HIMAGELIST orbiter_TreeImageList(HWND tree);

// Also Win32Dlg.cpp: it owns the window hierarchy. GetDC needs it to tell a
// child control from a top-level window.
extern "C" HWND orbiter_GetParentWnd(HWND h);

// Declared rather than pulled in from OrbiterAPI.h: that header wants the SDK
// include chain, and this file is a Win32 shim compiled ahead of it. The
// signature takes a non-const char* upstream and is matched exactly.
void oapiWriteLog(char *line);

#include <vector>
#include <string>
#include <map>
#include <memory>
#include <cmath>

namespace orbiter_res {
// Defined in BmpDecode.cpp.
bool DecodeBMP(const unsigned char *data, int size,
               std::vector<unsigned char> &rgba, int &w, int &h);
}

namespace {

// ---------------------------------------------------------------------------
// GDI objects
// ---------------------------------------------------------------------------

struct GdiObject {
    enum Kind { Pen, Brush, Font, Bitmap } kind;

    // Pen
    int      penStyle = PS_SOLID;
    int      penWidth = 1;

    // Pen and brush share a colour.
    COLORREF colour = 0;

    // Brush
    UINT     brushStyle = BS_SOLID;
    int      hatchStyle = 0;        // HS_*, when brushStyle is BS_HATCHED

    // Font
    //
    // Everything CreateFontA is handed is kept, not just the three fields the
    // recorder itself draws with. GetObject(hFont, sizeof(LOGFONT), &lf) is
    // part of the API and the graphics client's font manager is built on it:
    // it rasterises its own glyph atlas from the face, and it can only do
    // that if it can ask which face. Discarding italic, charset and the rest
    // made that read-back lossy for no saving.
    int         fontHeight = 0;       // magnitude, for drawing
    int         fontHeightSigned = 0; // as passed, for GetObject
    int         fontWidth  = 0;
    int         fontWeight = FW_NORMAL;
    int         fontEscapement = 0;
    int         fontOrientation = 0;
    bool        fontItalic = false;
    bool        fontUnderline = false;
    bool        fontStrikeOut = false;
    int         fontCharSet = 0;
    int         fontOutPrecision = 0;
    int         fontClipPrecision = 0;
    int         fontQuality = 0;
    int         fontPitchAndFamily = 0;
    std::string fontFace;

    // Bitmap. `pixels` owns the decoded RGBA8 image when the object came
    // from a resource; `bits` points either at it or at a DIB section's
    // caller-visible buffer.
    int   bmWidth = 0, bmHeight = 0;
    void *bits = nullptr;
    std::vector<unsigned char> pixels;

    // Texture handle for the renderer, uploaded lazily on first draw. Zero
    // means "not yet uploaded"; the upload needs a live frame, which a
    // LoadBitmap call during dialog construction does not have.
    unsigned long long texture = 0;

    // Set by SetPixel when it writes `pixels` after that upload has happened,
    // so the next draw re-uploads instead of showing the stale texture.
    // Nothing here frees the old handle, which is this file's existing model
    // -- maskedTexture caches one per colour key forever for the same reason.
    // The only writer is WindowMgr's title-bar recolouring, which runs once
    // per node and again when an application changes its colour.
    bool pixelsDirty = false;

    bool stock = false;   // stock objects must survive DeleteObject
};

// Every allocated GDI object. The handle is the object address; membership in
// this set is what makes a handle valid.
// Deliberately never destroyed, for the same reason as g_windowClasses in
// Win32Dlg.cpp.
//
// Modules release their GDI objects from ExitModule, which runs as the
// process tears down -- ShuttleA does exactly that, via
// oapiUnregisterCustomControls -> UnregisterPropertyList -> DeleteObject.
// The order in which a shared object's destructors run relative to this
// executable's namespace-scope objects is undefined, so with an ordinary
// global those late DeleteObject calls read a map that has already been
// destroyed:
//
//     heap-use-after-free ... READ of size 8
//     #3 toObject Gdi.cpp:112
//     #4 DeleteObject Gdi.cpp:726
//     #7 ExitModule ShuttleA.cpp:2469
//
// Allocating once and never freeing removes the ordering hazard: the map
// stays valid as long as any code can reach it. The GdiObjects it owns are
// reclaimed by the kernel at exit along with everything else.
std::map<GdiObject *, std::unique_ptr<GdiObject>> &gdiObjects()
{
    static std::map<GdiObject *, std::unique_ptr<GdiObject>> *objects =
        new std::map<GdiObject *, std::unique_ptr<GdiObject>>();
    return *objects;
}
#define g_objects gdiObjects()

GdiObject *newObject(GdiObject::Kind kind)
{
    auto owned = std::make_unique<GdiObject>();
    owned->kind = kind;
    GdiObject *raw = owned.get();
    g_objects.emplace(raw, std::move(owned));

    // A GROWTH ALARM, not a counter.
    //
    // Every object here is reached through toObject(), an O(log n) lookup on
    // this map, and every drawing call makes one. So an unbounded population
    // is not merely a leak -- it slows all painting, forever, and neither
    // symptom names its cause.
    //
    // A dialog's whole working set is a few dozen pens, brushes and fonts.
    // Crossing into the thousands means something is allocating per frame,
    // which is exactly the defect this found: GetSysColorBrush minted a fresh
    // brush on every call and marked it stock so DeleteObject would refuse to
    // free it, and Launchpad's WM_DRAWITEM handler calls it once per frame.
    static size_t nextReport = 1000;
    if (g_objects.size() >= nextReport) {
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "GDI: %zu live objects -- a dialog needs dozens, so this is "
                 "something allocating per frame", g_objects.size());
        oapiWriteLog(msg);
        nextReport *= 4;
    }
    return raw;
}

GdiObject *toObject(HGDIOBJ h)
{
    GdiObject *o = (GdiObject *)h;
    return (o && g_objects.count(o)) ? o : nullptr;
}

// Stock objects are created once and never freed, matching Win32 where
// DeleteObject on a stock handle is a no-op.
GdiObject *stockObject(int index)
{
    static std::map<int, GdiObject *> stock;
    auto it = stock.find(index);
    if (it != stock.end()) return it->second;

    GdiObject *o;
    switch (index) {
    case WHITE_BRUSH:  o = newObject(GdiObject::Brush); o->colour = RGB(255,255,255); break;
    case LTGRAY_BRUSH: o = newObject(GdiObject::Brush); o->colour = RGB(192,192,192); break;
    case GRAY_BRUSH:   o = newObject(GdiObject::Brush); o->colour = RGB(128,128,128); break;
    case DKGRAY_BRUSH: o = newObject(GdiObject::Brush); o->colour = RGB(64,64,64);    break;
    case BLACK_BRUSH:  o = newObject(GdiObject::Brush); o->colour = RGB(0,0,0);       break;
    case NULL_BRUSH:   o = newObject(GdiObject::Brush); o->brushStyle = BS_NULL;      break;
    case WHITE_PEN:    o = newObject(GdiObject::Pen);   o->colour = RGB(255,255,255); break;
    case BLACK_PEN:    o = newObject(GdiObject::Pen);   o->colour = RGB(0,0,0);       break;
    case NULL_PEN:     o = newObject(GdiObject::Pen);   o->penStyle = PS_NULL;        break;
    default:           o = newObject(GdiObject::Brush); o->colour = RGB(0,0,0);       break;
    }
    o->stock = true;
    stock[index] = o;
    return o;
}

// ---------------------------------------------------------------------------
// Recorded drawing commands
// ---------------------------------------------------------------------------

struct DrawCmd {
    enum Op {
        Line, Rect, Ellipse, Arc, Pie, Polygon, Polyline, Text, Blit
    } op;

    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int x2 = 0, y2 = 0, x3 = 0, y3 = 0;   // arc start/end points

    // Captured at record time: the pen and brush may be swapped before this
    // command is replayed, and GDI semantics are that the state at the moment
    // of the call is what applies.
    bool     hasOutline = false;
    COLORREF outline    = 0;
    int      outlineWidth = 1;
    bool     hasFill    = false;
    COLORREF fill       = 0;

    std::string        text;
    COLORREF           textColour = 0;
    UINT               textAlign  = TA_LEFT;
    std::vector<POINT> points;

    // Blit source
    GdiObject *srcBitmap = nullptr;

    // Blit source rectangle, in source-bitmap pixels. Both BitBlt and
    // TransparentBlt are used in this tree to pick one icon out of a strip:
    //
    //   PropertyList::OnPaint  BitBlt(hDC, 2, y+2, 14, 14, hDCmem,
    //                                 28 + (expanded ? 0 : 14), 0, SRCCOPY)
    //   gcPropertyTree::PaintIcon
    //                          TransparentBlt(hBM, x, y+yo, s, s, hSr,
    //                                         sx, 0, s, s, ck)
    //
    // so discarding sx/sy/sw/sh draws the whole strip squashed into the
    // destination rectangle instead of the one glyph that was asked for.
    int srcX = 0, srcY = 0, srcW = 0, srcH = 0;

    // TransparentBlt's colour key. hasColourKey distinguishes "key is black"
    // from "no key", which a sentinel value could not.
    bool     hasColourKey = false;
    COLORREF colourKey    = 0;

    // Clipping state at record time. GDI clipping is a property of the DC, so
    // a command is clipped by whatever was selected when it was issued, not by
    // what is selected when the frame is replayed.
    bool hasClip   = false;
    RECT clip      = { 0, 0, 0, 0 };
    // How many of the DC's exclusion rectangles were in force. ExcludeClipRect
    // only ever adds to the list until SelectClipRgn resets it, so a count is
    // enough to reconstruct the set without copying it per command.
    size_t excludeCount = 0;
};

struct DeviceContext {
    // The control this DC paints into. Null for a memory DC, which records but
    // is never replayed directly.
    HWND owner = nullptr;

    // THE WIN32 DC DEFAULTS, AND THEY ARE LOAD-BEARING.
    //
    // A fresh device context arrives with WHITE_BRUSH and BLACK_PEN already
    // selected. Those are the documented defaults, and drawing code relies on
    // them: it selects only what it means to CHANGE. DX9ExtMFD's RepaintButton
    // is exactly that --
    //
    //     SelectObject (hDC, GetStockObject (BLACK_PEN));
    //     Rectangle (hDC, 0, 0, BW, BH);
    //
    // -- it names the pen and says nothing about the brush, because the
    // default already IS the brush it wants: Rectangle fills its interior
    // with the current brush, so on Windows every one of those buttons has a
    // WHITE face inside a black border.
    //
    // With both null here, captureState left hasFill false (and would have
    // left hasOutline false for anything that did not name a pen), so the
    // replay stroked the border and filled nothing. MEASURED, scanning one
    // row across a button in the DX9 MFD dialog:
    //
    //     x=459        #000000   border
    //     x=460..498   #F0F0F0   interior  <-- the DIALOG FACE showing through
    //     x=499        #000000   border
    //     x=500..502   #F0F0F0   dialog face
    //
    // -- the button interior was the same #F0F0F0 as the background it sat
    // on, where Windows draws #FFFFFF, so the buttons read as bare outlines.
    //
    // The pen default is the same defect with no symptom yet: a Rectangle or
    // Ellipse drawn without naming a pen currently has NO outline, where
    // Windows gives it a black one. Setting it can only move the port toward
    // the reference -- code that genuinely wants no outline selects NULL_PEN,
    // which captureState already honours.
    //
    // stockObject() is defined above and caches, so this costs one map lookup
    // per DC.
    GdiObject *pen   = stockObject(BLACK_PEN);
    GdiObject *brush = stockObject(WHITE_BRUSH);
    GdiObject *font  = nullptr;
    GdiObject *bitmap = nullptr;      // for a memory DC

    COLORREF textColour = RGB(0, 0, 0);
    COLORREF bkColour   = RGB(255, 255, 255);
    int      bkMode     = OPAQUE;
    UINT     textAlign  = TA_LEFT;

    POINT current{ 0, 0 };            // MoveToEx/LineTo cursor

    // Viewport origin, set by SetViewportOrgEx. GDI offsets every subsequent
    // primitive by it; Orbiter's MFDs use it to draw in their own coordinates
    // rather than the render target's.
    int originX = 0, originY = 0;

    // Clipping. clipActive is set by SelectClipRgn and cleared when it is
    // passed a null region, which is how Win32 removes a clip.
    bool              clipActive = false;
    RECT              clipRect   = { 0, 0, 0, 0 };
    std::vector<RECT> excluded;

    std::vector<DrawCmd> commands;
    bool memoryDC = false;
};

std::map<DeviceContext *, std::unique_ptr<DeviceContext>> g_dcs;

DeviceContext *toDC(HDC h)
{
    DeviceContext *d = (DeviceContext *)h;
    return (d && g_dcs.count(d)) ? d : nullptr;
}

DeviceContext *newDC()
{
    auto owned = std::make_unique<DeviceContext>();
    DeviceContext *raw = owned.get();
    g_dcs.emplace(raw, std::move(owned));
    return raw;
}

// Records the DC's clip state onto a command.
//
// Clipping in GDI is DC state that applies at the moment of the call, and this
// DC is replayed later, so it has to travel with the command exactly as the
// pen and brush do.
void captureClip(const DeviceContext *dc, DrawCmd &c)
{
    c.hasClip      = dc->clipActive;
    c.clip         = dc->clipRect;
    c.excludeCount = dc->excluded.size();
}

// Fills in the pen/brush state a command should carry.
void captureState(const DeviceContext *dc, DrawCmd &c)
{
    if (dc->pen && dc->pen->penStyle != PS_NULL) {
        c.hasOutline   = true;
        c.outline      = dc->pen->colour;
        c.outlineWidth = dc->pen->penWidth;
    }
    if (dc->brush && dc->brush->brushStyle != BS_NULL) {
        c.hasFill = true;
        c.fill    = dc->brush->colour;
    }
    captureClip(dc, c);
}

// COLORREF is 0x00BBGGRR; ImGui's IM_COL32 takes r,g,b,a. Getting this
// backwards produces plausible-looking but wrong colours, which is worse than
// an obvious failure.
inline ImU32 toImGui(COLORREF c)
{
    return IM_COL32(GetRValue(c), GetGValue(c), GetBValue(c), 255);
}

// A copy of a bitmap with the colour key made transparent, uploaded once and
// cached. This is what makes TransparentBlt a masked blit rather than a plain
// one: Win32 compares each source pixel against crTransparent and skips the
// matches, and the equivalent here is an alpha of zero at those pixels.
//
// The comparison is on RGB only. The stored pixels are RGBA8 and the key is a
// COLORREF with no alpha channel, so including alpha would never match.
unsigned long long maskedTexture(GdiObject *bm, COLORREF key)
{
    static std::map<std::pair<GdiObject *, COLORREF>, unsigned long long> cache;

    const auto id = std::make_pair(bm, key);
    auto it = cache.find(id);
    if (it != cache.end()) return it->second;

    if (!bm || bm->pixels.empty() || bm->bmWidth <= 0 || bm->bmHeight <= 0)
        return 0;

    const unsigned char kr = GetRValue(key);
    const unsigned char kg = GetGValue(key);
    const unsigned char kb = GetBValue(key);

    std::vector<unsigned char> masked = bm->pixels;
    for (size_t i = 0; i + 3 < masked.size(); i += 4) {
        if (masked[i] == kr && masked[i + 1] == kg && masked[i + 2] == kb)
            masked[i + 3] = 0;
    }

    const unsigned long long tex =
        orbiter_UploadTexture(masked.data(), bm->bmWidth, bm->bmHeight);
    cache[id] = tex;
    return tex;
}

} // namespace

// ===========================================================================
// Replay
//
// Called by the frame pump once per painted control, inside a frame, with the
// control's screen origin. This is the only point at which anything recorded
// above reaches ImGui.
// ===========================================================================

extern "C" void orbiter_ReplayDC(HDC hdc, float originX, float originY)
{
    DeviceContext *dc = toDC(hdc);
    if (!dc) return;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    if (!dl) return;

    // The DC's own viewport origin is added to the caller's.
    //
    // SetViewportOrgEx is how GDI shifts every subsequent primitive, and it is
    // what Orbiter's MFDs use to draw in their own coordinates rather than the
    // render target's. Ignoring it drew every instrument at the same place,
    // stacked on top of each other and jumping as each one set an origin the
    // replay then discarded.
    const float ox = originX + (float)dc->originX;
    const float oy = originY + (float)dc->originY;

    auto P = [&](int x, int y) {
        return ImVec2(ox + (float)x, oy + (float)y);
    };

    // Bounding box of a command, used to test it against the exclusion set.
    auto bounds = [&](const DrawCmd &c, RECT &r) {
        if (c.op == DrawCmd::Polygon || c.op == DrawCmd::Polyline) {
            if (c.points.empty()) return false;
            r.left = r.right = c.points[0].x;
            r.top = r.bottom = c.points[0].y;
            for (const POINT &p : c.points) {
                if (p.x < r.left)   r.left   = p.x;
                if (p.x > r.right)  r.right  = p.x;
                if (p.y < r.top)    r.top    = p.y;
                if (p.y > r.bottom) r.bottom = p.y;
            }
            return true;
        }
        r.left   = c.x0 < c.x1 ? c.x0 : c.x1;
        r.right  = c.x0 < c.x1 ? c.x1 : c.x0;
        r.top    = c.y0 < c.y1 ? c.y0 : c.y1;
        r.bottom = c.y0 < c.y1 ? c.y1 : c.y0;
        return true;
    };

    for (const DrawCmd &c : dc->commands) {
        // ExcludeClipRect punches holes in the clip region. ImGui's clip stack
        // only intersects, so a subtractive region cannot be expressed
        // directly. What the callers actually use it for is protecting the
        // rectangles their CHILD controls occupy -- gcPropertyTree excludes
        // each edit, combo and slider before blitting its back buffer -- and a
        // command that lies wholly inside such a hole draws nothing at all.
        // Dropping those commands reproduces the visible result exactly.
        //
        // A command that only partially overlaps a hole is still drawn whole.
        // That is the limit of this approach and it is worth knowing: nothing
        // in this tree straddles an exclusion, because the holes are aligned
        // to the child rectangles the drawing avoids anyway.
        if (c.excludeCount) {
            RECT b;
            bool dropped = false;
            if (bounds(c, b)) {
                for (size_t i = 0; i < c.excludeCount && i < dc->excluded.size(); ++i) {
                    const RECT &e = dc->excluded[i];
                    if (b.left >= e.left && b.right <= e.right &&
                        b.top >= e.top && b.bottom <= e.bottom) {
                        dropped = true;
                        break;
                    }
                }
            }
            if (dropped) continue;
        }

        // SelectClipRgn intersects, which the clip stack does express.
        const bool clipped = c.hasClip;
        if (clipped)
            dl->PushClipRect(P(c.clip.left, c.clip.top),
                             P(c.clip.right, c.clip.bottom), true);

        switch (c.op) {
        case DrawCmd::Line:
            if (c.hasOutline)
                dl->AddLine(P(c.x0, c.y0), P(c.x1, c.y1), toImGui(c.outline),
                            (float)c.outlineWidth);
            break;

        case DrawCmd::Rect:
            // GDI's Rectangle fills with the brush and outlines with the pen,
            // and its right/bottom edges are exclusive.
            if (c.hasFill)
                dl->AddRectFilled(P(c.x0, c.y0), P(c.x1, c.y1), toImGui(c.fill));
            if (c.hasOutline)
                dl->AddRect(P(c.x0, c.y0), P(c.x1, c.y1), toImGui(c.outline),
                            0.0f, 0, (float)c.outlineWidth);
            break;

        case DrawCmd::Ellipse: {
            const ImVec2 a = P(c.x0, c.y0), b = P(c.x1, c.y1);
            const ImVec2 centre((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            const ImVec2 radius((b.x - a.x) * 0.5f, (b.y - a.y) * 0.5f);
            if (c.hasFill)
                dl->AddEllipseFilled(centre, radius, toImGui(c.fill));
            if (c.hasOutline)
                dl->AddEllipse(centre, radius, toImGui(c.outline), 0.0f, 0,
                               (float)c.outlineWidth);
            break;
        }

        case DrawCmd::Arc:
        case DrawCmd::Pie: {
            // THE ANGLES ARE COMPUTED IN CONTROL SPACE, not screen space.
            //
            // This used to take the difference between the start point (in
            // control coordinates, as GDI gave it) and `centre` (already
            // through P(), so in screen coordinates) and then add back
            // originX/originY to compensate. That cancels the caller's origin
            // but NOT the DC's own SetViewportOrgEx offset, so any arc drawn
            // after a viewport origin was set came out at the wrong angles.
            // Doing the arithmetic entirely in control space needs no
            // compensation at all and cannot drift.
            const float cxc = (float)(c.x0 + c.x1) * 0.5f;
            const float cyc = (float)(c.y0 + c.y1) * 0.5f;

            const ImVec2 a = P(c.x0, c.y0), b = P(c.x1, c.y1);
            const ImVec2 centre((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            const float  r = ((b.x - a.x) + (b.y - a.y)) * 0.25f;

            float a0 = atan2f((float)c.y2 - cyc, (float)c.x2 - cxc);
            float a1 = atan2f((float)c.y3 - cyc, (float)c.x3 - cxc);

            // GDI sweeps COUNTER-CLOCKWISE from the start radial to the end
            // radial. ImGui's PathArcTo interpolates from a_min to a_max, and
            // angles here are in a Y-down space where increasing means
            // clockwise -- so the sweep has to run DOWNWARD. Forcing
            // a1 <= a0 is what makes a 30-degree wedge draw as 30 degrees
            // instead of the 330 the other way round.
            while (a1 > a0) a1 -= 6.28318530718f;

            dl->PathArcTo(centre, r, a0, a1);

            if (c.op == DrawCmd::Pie) {
                // A pie closes through the centre and fills with the brush.
                // Recorded as a bare Arc before, this drew an unfilled outline
                // at best and -- with a NULL pen, which the `if (!hasOutline)`
                // guard rejected outright -- nothing at all. Dragonfly's radar
                // antenna wedge is a filled Pie under a NULL pen and never
                // appeared.
                dl->PathLineTo(centre);
                if (c.hasFill) {
                    std::vector<ImVec2> pts(dl->_Path.Data,
                                            dl->_Path.Data + dl->_Path.Size);
                    if (pts.size() >= 3)
                        dl->AddConvexPolyFilled(pts.data(), (int)pts.size(),
                                                toImGui(c.fill));
                }
                if (c.hasOutline)
                    dl->PathStroke(toImGui(c.outline), ImDrawFlags_Closed,
                                   (float)c.outlineWidth);
                else
                    dl->_Path.Size = 0;
            } else {
                if (c.hasOutline)
                    dl->PathStroke(toImGui(c.outline), 0, (float)c.outlineWidth);
                else
                    dl->_Path.Size = 0;
            }
            break;
        }

        case DrawCmd::Polygon:
        case DrawCmd::Polyline: {
            if (c.points.size() < 2) break;
            for (const POINT &p : c.points)
                dl->PathLineTo(P(p.x, p.y));
            if (c.op == DrawCmd::Polygon) {
                if (c.hasFill) {
                    // PathFillConvex consumes the path, so the outline is
                    // rebuilt afterwards when both are wanted.
                    std::vector<ImVec2> pts;
                    for (const POINT &p : c.points) pts.push_back(P(p.x, p.y));
                    dl->AddConvexPolyFilled(pts.data(), (int)pts.size(),
                                            toImGui(c.fill));
                    dl->_Path.Size = 0;
                    for (const POINT &p : c.points) dl->PathLineTo(P(p.x, p.y));
                }
                dl->PathStroke(toImGui(c.hasOutline ? c.outline : c.fill),
                               ImDrawFlags_Closed, (float)c.outlineWidth);
            } else {
                dl->PathStroke(toImGui(c.outline), 0, (float)c.outlineWidth);
            }
            break;
        }

        case DrawCmd::Text: {
            if (c.text.empty()) break;
            ImVec2 pos = P(c.x0, c.y0);
            // GDI aligns the text box, not the pen position, so the width has
            // to be measured before placing it.
            if (c.textAlign & (TA_CENTER | TA_RIGHT)) {
                const ImVec2 sz = ImGui::CalcTextSize(c.text.c_str());
                if ((c.textAlign & TA_CENTER) == TA_CENTER) pos.x -= sz.x * 0.5f;
                else if (c.textAlign & TA_RIGHT)            pos.x -= sz.x;
            }
            dl->AddText(pos, toImGui(c.textColour), c.text.c_str());
            break;
        }

        case DrawCmd::Blit:
            // The source bitmap's pixels are uploaded as a texture on first
            // use. It cannot happen at LoadBitmap time: uploading needs the
            // renderer, and bitmaps are loaded during dialog construction,
            // long before the first frame.
            if (c.srcBitmap && !c.srcBitmap->pixels.empty()) {
                GdiObject *bm = c.srcBitmap;

                // TransparentBlt: the source is re-uploaded with alpha zeroed
                // wherever a pixel matches the colour key. A separate texture
                // is used rather than tinting at draw time, because the key is
                // a per-pixel test and ImGui has no shader hook for it. The
                // result is cached per (bitmap, key) -- gcPropertyTree draws
                // four icons every frame with two distinct keys.
                unsigned long long tex = 0;
                if (c.hasColourKey) {
                    tex = maskedTexture(bm, c.colourKey);
                } else {
                    // pixelsDirty is SetPixel having written the buffer after
                    // an upload; see the note on the field.
                    if (bm->texture == 0 || bm->pixelsDirty) {
                        bm->texture = orbiter_UploadTexture(
                            bm->pixels.data(), bm->bmWidth, bm->bmHeight);
                        bm->pixelsDirty = false;
                    }
                    tex = bm->texture;
                }

                if (tex) {
                    // The source rectangle becomes UVs. A zero width or height
                    // means the caller gave none, so the whole image is used --
                    // which is what BitBlt does when the sizes agree.
                    ImVec2 uv0(0.0f, 0.0f), uv1(1.0f, 1.0f);
                    if (c.srcW > 0 && c.srcH > 0 &&
                        bm->bmWidth > 0 && bm->bmHeight > 0) {
                        uv0 = ImVec2((float)c.srcX / (float)bm->bmWidth,
                                     (float)c.srcY / (float)bm->bmHeight);
                        uv1 = ImVec2((float)(c.srcX + c.srcW) / (float)bm->bmWidth,
                                     (float)(c.srcY + c.srcH) / (float)bm->bmHeight);
                    }
                    dl->AddImage((ImTextureID)tex,
                                 P(c.x0, c.y0), P(c.x1, c.y1), uv0, uv1);
                }
            }
            break;
        }

        if (clipped) dl->PopClipRect();
    }
}

// Clears a DC's recorded commands. The frame pump calls this after replay so
// the next WM_PAINT starts clean.
extern "C" void orbiter_ResetDC(HDC hdc)
{
    if (DeviceContext *dc = toDC(hdc)) dc->commands.clear();
}

// The persistent paint DC for a control.
//
// A control is painted once per frame and replayed in the same frame, so it
// needs one DC that survives between the two rather than a fresh one per
// WM_PAINT -- otherwise the commands recorded by the handler would belong to a
// context the pump never sees. Created on first use and kept for the control's
// lifetime.
static std::map<HWND, DeviceContext *> g_paintDCs;

extern "C" HDC orbiter_GetPaintDC(HWND h)
{
    auto it = g_paintDCs.find(h);
    if (it != g_paintDCs.end() && g_dcs.count(it->second))
        return (HDC)it->second;

    DeviceContext *dc = newDC();
    dc->owner = h;
    g_paintDCs[h] = dc;
    return (HDC)dc;
}

// The same lookup WITHOUT creating one, and without which the frame pump
// cannot ask "did anything draw into this control?" -- asking with
// orbiter_GetPaintDC would manufacture an empty DC for every control on every
// frame and leak one per window that never draws.
extern "C" HDC orbiter_FindPaintDC(HWND h)
{
    auto it = g_paintDCs.find(h);
    if (it != g_paintDCs.end() && g_dcs.count(it->second) &&
        !it->second->commands.empty())
        return (HDC)it->second;
    return nullptr;
}

// Discard what a control has recorded. This is what InvalidateRect's bErase
// means here: on Windows it tells the system to paint the background over the
// window's pixels, and the recorded command list IS this port's pixels.
extern "C" void orbiter_ClearPaintDC(HWND h)
{
    auto it = g_paintDCs.find(h);
    if (it != g_paintDCs.end() && g_dcs.count(it->second))
        it->second->commands.clear();
}

// Accessors for the DC state a WM_CTLCOLOR* handler leaves behind.
//
// Those handlers communicate by mutating the DC -- SetTextColor, SetBkColor,
// SetBkMode -- and returning a brush, rather than by filling in a struct. So
// after sending the message the caller has to read the DC back to find out
// what was asked for.
extern "C" {

void orbiter_SetDCTextColor(HDC hdc, unsigned c)
{
    if (DeviceContext *dc = toDC(hdc)) dc->textColour = (COLORREF)c;
}
void orbiter_SetDCBkColor(HDC hdc, unsigned c)
{
    if (DeviceContext *dc = toDC(hdc)) dc->bkColour = (COLORREF)c;
}
void orbiter_SetDCBkMode(HDC hdc, int mode)
{
    if (DeviceContext *dc = toDC(hdc)) dc->bkMode = mode;
}

unsigned orbiter_GetDCTextColor(HDC hdc)
{
    DeviceContext *dc = toDC(hdc);
    return dc ? (unsigned)dc->textColour : 0;
}
unsigned orbiter_GetDCBkColor(HDC hdc)
{
    DeviceContext *dc = toDC(hdc);
    return dc ? (unsigned)dc->bkColour : 0;
}
int orbiter_GetDCBkMode(HDC hdc)
{
    DeviceContext *dc = toDC(hdc);
    return dc ? dc->bkMode : OPAQUE;
}

} // extern "C"

// ===========================================================================
// Device contexts
// ===========================================================================

extern "C" {

// THE OTHER HALF OF THE BeginPaint DEFECT BELOW.
//
// BeginPaint was returning a throwaway DC and every WM_PAINT handler's drawing
// was discarded; see the long note there. GetDC is the SECOND way Win32 code
// draws into a window -- outside WM_PAINT, at the moment something changes --
// and it had exactly the same fault for exactly the same reason.
//
// ScnEditor's vessel preview is the case that found it. DrawVesselBmp does
//
//     HDC hDC = GetDC (hImgWnd);
//     ... StretchBlt (hDC, 0, 0, r.right, h, hBmpDC, 0, 0, dx, dy, SRCCOPY);
//     ReleaseDC (hImgWnd, hDC);
//
// on a stock SS_BITMAP static. The blit went into a context the pump never
// replayed, so the panel stayed empty however well the bitmap loaded.
//
// WHY THE COMMANDS ARE KEPT. A DC from GetDC does not begin a repaint: on
// Windows it draws ON TOP of what the window already shows, and those pixels
// stay until something erases them. Nothing re-issues this blit -- the tab's
// WM_PAINT is never sent, because the pump only sends WM_PAINT to controls
// that paint themselves -- so clearing here, as BeginPaint deliberately does,
// would erase the picture the caller just drew. InvalidateRect(h, NULL, TRUE)
// is what erases it, and it does exactly that; see orbiter_ClearPaintDC.
//
// The STATE is still reset, because that half of Win32 is true: a DC handed
// out by GetDC has the default objects selected regardless of what the last
// holder chose. Only the recorded picture survives.
//
// CHILD WINDOWS ONLY. A top-level window has no entry in the pump's control
// pass, so a persistent DC for one would accumulate commands that are never
// replayed and never cleared -- DialogWin's caption buttons and the client's
// GetDC(hRenderWnd) black fill are both that shape. They keep the throwaway.
HDC GetDC(HWND h)
{
    if (h && orbiter_GetParentWnd(h)) {
        if (DeviceContext *dc = toDC(orbiter_GetPaintDC(h))) {
            dc->pen        = stockObject(BLACK_PEN);
            dc->brush      = stockObject(WHITE_BRUSH);
            dc->font       = nullptr;
            dc->bitmap     = nullptr;
            dc->textColour = RGB(0, 0, 0);
            dc->bkColour   = RGB(255, 255, 255);
            dc->bkMode     = OPAQUE;
            dc->textAlign  = TA_LEFT;
            dc->current    = POINT{ 0, 0 };
            dc->originX    = 0;
            dc->originY    = 0;
            dc->clipActive = false;
            dc->excluded.clear();
            dc->owner      = h;
            return (HDC)dc;
        }
    }

    DeviceContext *dc = newDC();
    dc->owner = h;
    return (HDC)dc;
}

HDC GetWindowDC(HWND h) { return GetDC(h); }

int ReleaseDC(HWND, HDC hdc)
{
    // A released DC keeps its commands: the frame pump has not replayed them
    // yet. Only the handle ownership ends here.
    return toDC(hdc) ? 1 : 0;
}

HDC BeginPaint(HWND h, LPPAINTSTRUCT ps)
{
    // THE CONTROL'S PERSISTENT PAINT DC, not a fresh one -- and this is the
    // whole of whether a self-painting control is visible at all.
    //
    // The pump paints one of these in two steps (UIHost::drawControl):
    //
    //     orbiter_SendPaint(h);                       // -> WM_PAINT
    //     if (HDC dc = orbiter_GetPaintDC(h)) {
    //         orbiter_ReplayDC(dc, pos.x, pos.y);     // -> ImGui draw list
    //         orbiter_ResetDC(dc);
    //     }
    //
    // and orbiter_GetPaintDC's own note says why it is persistent: "otherwise
    // the commands recorded by the handler would belong to a context the pump
    // never sees". That is precisely what a newDC() here produced. Every
    // Rectangle, LineTo and TextOut a WM_PAINT handler issued went into a
    // throwaway DC, the pump replayed the persistent one, and the persistent
    // one was always empty -- so the control drew nothing, every frame,
    // forever.
    //
    // On Windows there is no difference to notice: BeginPaint returns a DC
    // onto the window itself and the pixels are already on screen by the time
    // EndPaint returns. Here the DC is a COMMAND RECORDER and the identity of
    // the object matters.
    //
    // Found through DX9ExtMFD, whose dialog was completely blank while
    // childdump reported all seventeen children present, visible, enabled and
    // taking the wndproc branch -- so the controls were there and painting,
    // and nothing was arriving. It is not specific to that plugin: every
    // BeginPaint in the tree is a WM_PAINT handler for a custom control class
    // -- DlgCtrl's gauges (DlgCtrl.cpp:78, :514), its switches
    // (DlgCtrlSwitch.cpp:34), TerrainToolKit's gcTableView, WindowMgr's side
    // bar -- and all of them had the same fate.
    //
    // The commands are cleared first because a WM_PAINT redraws the whole
    // update region, which here is always the entire client rect: a handler
    // that paints twice before the pump replays must not stack its output.
    DeviceContext *dc = toDC(orbiter_GetPaintDC(h));
    if (!dc) return nullptr;
    dc->commands.clear();
    dc->owner = h;

    if (ps) {
        memset(ps, 0, sizeof(*ps));
        ps->hdc    = (HDC)dc;
        ps->fErase = FALSE;
        GetClientRect(h, &ps->rcPaint);
    }
    return (HDC)dc;
}

BOOL EndPaint(HWND, const PAINTSTRUCT *) { return TRUE; }

BOOL GetUpdateRect(HWND h, LPRECT r, BOOL)
{
    // ImGui redraws every frame, so a window always has a pending update.
    if (r) GetClientRect(h, r);
    return TRUE;
}

HDC CreateCompatibleDC(HDC)
{
    DeviceContext *dc = newDC();
    dc->memoryDC = true;
    return (HDC)dc;
}

BOOL DeleteDC(HDC hdc)
{
    DeviceContext *dc = toDC(hdc);
    if (!dc) return FALSE;
    g_dcs.erase(dc);
    return TRUE;
}

// ===========================================================================
// Object creation and selection
// ===========================================================================

HPEN CreatePen(int style, int width, COLORREF colour)
{
    GdiObject *o = newObject(GdiObject::Pen);
    o->penStyle = style;
    // A pen width of 0 means "one pixel, unscaled" in GDI.
    o->penWidth = width > 0 ? width : 1;
    o->colour   = colour;
    return (HPEN)o;
}

HBRUSH CreateSolidBrush(COLORREF colour)
{
    GdiObject *o = newObject(GdiObject::Brush);
    o->colour     = colour;
    o->brushStyle = BS_SOLID;
    return (HBRUSH)o;
}

HBRUSH CreateBrushIndirect(const LOGBRUSH *lb)
{
    GdiObject *o = newObject(GdiObject::Brush);
    if (lb) {
        o->colour     = lb->lbColor;
        o->brushStyle = lb->lbStyle;
    }
    return (HBRUSH)o;
}

// A hatched brush.
//
// The hatch pattern is recorded but the fill is rendered solid: the draw-list
// replay has no pattern-fill primitive, and a hatch drawn as individual lines
// would need the fill region clipped to the shape being filled, which the
// command model does not carry. So this is a KNOWN approximation rather than
// an oversight -- a hatched region appears as a flat one of the same colour.
// Dragonfly's panel is the only consumer and uses it for shading.
HBRUSH CreateHatchBrush(int style, COLORREF colour)
{
    GdiObject *o = newObject(GdiObject::Brush);
    o->colour     = colour;
    o->brushStyle = BS_HATCHED;
    o->hatchStyle = style;
    return (HBRUSH)o;
}

HFONT CreateFontA(int height, int width, int escapement, int orientation,
                  int weight, DWORD italic, DWORD underline, DWORD strikeout,
                  DWORD charset, DWORD outPrecision, DWORD clipPrecision,
                  DWORD quality, DWORD pitchAndFamily, LPCSTR face)
{
    GdiObject *o = newObject(GdiObject::Font);
    // A negative height is a character height rather than a cell height in
    // GDI; the magnitude is what matters for sizing.
    o->fontHeight = height < 0 ? -height : height;
    o->fontWidth  = width;
    o->fontWeight = weight;
    o->fontEscapement = escapement;
    o->fontOrientation = orientation;
    o->fontItalic = (italic != 0);
    o->fontUnderline = (underline != 0);
    o->fontStrikeOut = (strikeout != 0);
    o->fontCharSet = (int)charset;
    o->fontOutPrecision = (int)outPrecision;
    o->fontClipPrecision = (int)clipPrecision;
    o->fontQuality = (int)quality;
    o->fontPitchAndFamily = (int)pitchAndFamily;
    o->fontFace   = face ? face : "";

    // The SIGN of the height is not recoverable from the magnitude, and
    // GetObject has to give back what was passed. Kept separately rather than
    // by storing the signed value, because every drawing path here wants the
    // magnitude and would otherwise have to remember to take it.
    o->fontHeightSigned = height;
    return (HFONT)o;
}

// The struct form. Win32 has both and they make the same font; this unpacks
// the fourteen fields and calls the other, which is what GDI does too. Added
// for OVP/VulkanClient's SplashScreen, which fills a LOGFONTA.
HFONT CreateFontIndirectA(const LOGFONTA *lf)
{
    if (!lf) return nullptr;
    return CreateFontA(lf->lfHeight, lf->lfWidth, lf->lfEscapement,
                       lf->lfOrientation, lf->lfWeight, lf->lfItalic,
                       lf->lfUnderline, lf->lfStrikeOut, lf->lfCharSet,
                       lf->lfOutPrecision, lf->lfClipPrecision, lf->lfQuality,
                       lf->lfPitchAndFamily, lf->lfFaceName);
}

HGDIOBJ SelectObject(HDC hdc, HGDIOBJ obj)
{
    DeviceContext *dc = toDC(hdc);
    GdiObject     *o  = toObject(obj);
    if (!dc || !o) return nullptr;

    GdiObject *prev = nullptr;
    switch (o->kind) {
    case GdiObject::Pen:    prev = dc->pen;    dc->pen    = o; break;
    case GdiObject::Brush:  prev = dc->brush;  dc->brush  = o; break;
    case GdiObject::Font:   prev = dc->font;   dc->font   = o; break;
    case GdiObject::Bitmap: prev = dc->bitmap; dc->bitmap = o; break;
    }
    return (HGDIOBJ)prev;
}

BOOL DeleteObject(HGDIOBJ obj)
{
    GdiObject *o = toObject(obj);
    if (!o) return FALSE;
    if (o->stock) return TRUE;   // deleting a stock object is a no-op
    g_objects.erase(o);
    return TRUE;
}

HGDIOBJ GetStockObject(int index) { return (HGDIOBJ)stockObject(index); }

int GetObjectA(HGDIOBJ obj, int cb, LPVOID buf)
{
    GdiObject *o = toObject(obj);
    if (!o || !buf) return 0;

    if (o->kind == GdiObject::Bitmap && cb >= (int)sizeof(BITMAP)) {
        BITMAP *bm = (BITMAP *)buf;
        memset(bm, 0, sizeof(*bm));
        bm->bmWidth  = o->bmWidth;
        bm->bmHeight = o->bmHeight;
        bm->bmBitsPixel = 32;
        bm->bmWidthBytes = o->bmWidth * 4;
        bm->bmBits = o->bits;
        return sizeof(BITMAP);
    }

    // The font case, which the graphics client's font manager needs: it takes
    // an HFONT and has to find out which face to rasterise, because there is
    // no GDI here to rasterise it for them. See the LOGFONT note in windows.h.
    //
    // Everything CreateFontA was given comes back; the fields it was never
    // given stay zero, which is the truthful answer rather than a guess.
    if (o->kind == GdiObject::Font && cb >= (int)sizeof(LOGFONT)) {
        LOGFONT *lf = (LOGFONT *)buf;
        memset(lf, 0, sizeof(*lf));
        lf->lfHeight         = (LONG)o->fontHeightSigned;
        lf->lfWidth          = (LONG)o->fontWidth;
        lf->lfEscapement     = (LONG)o->fontEscapement;
        lf->lfOrientation    = (LONG)o->fontOrientation;
        lf->lfWeight         = (LONG)o->fontWeight;
        lf->lfItalic         = o->fontItalic    ? 1 : 0;
        lf->lfUnderline      = o->fontUnderline ? 1 : 0;
        lf->lfStrikeOut      = o->fontStrikeOut ? 1 : 0;
        lf->lfCharSet        = (BYTE)o->fontCharSet;
        lf->lfOutPrecision   = (BYTE)o->fontOutPrecision;
        lf->lfClipPrecision  = (BYTE)o->fontClipPrecision;
        lf->lfQuality        = (BYTE)o->fontQuality;
        lf->lfPitchAndFamily = (BYTE)o->fontPitchAndFamily;

        // strncpy, not strcpy: LF_FACESIZE is 32 and GDI truncates a longer
        // face name rather than overrunning. The last byte stays zero because
        // the struct was cleared above.
        strncpy(lf->lfFaceName, o->fontFace.c_str(), LF_FACESIZE - 1);
        return (int)sizeof(LOGFONT);
    }
    return 0;
}

DWORD GetSysColor(int index)
{
    // The real Windows system colours, not a theme approximation. Orbiter's
    // owner-drawn controls tint themselves with these and then sit next to
    // controls drawn by the UI host, so the two must agree or the seams show.
    // Values are the Windows 10 "Aero Lite" defaults, which is what a stock
    // dialog is painted with.
    switch (index) {
    case COLOR_3DFACE:   return RGB(240, 240, 240);
    case COLOR_3DSHADOW: return RGB(160, 160, 160);
    case COLOR_WINDOW:   return RGB(255, 255, 255);
    case COLOR_BTNTEXT:  return RGB(0, 0, 0);
    default:             return RGB(240, 240, 240);
    }
}

// CACHED PER INDEX, and that is the documented contract rather than an
// optimisation. Windows returns a SHARED brush that the caller must not
// delete; a fresh one per call is a different object every time and, marked
// stock so DeleteObject refuses it, one that nothing can ever free.
//
// The caller that made this matter is on the Launchpad's own paint path:
//
//     // Launchpad.cpp:406, WM_DRAWITEM for IDC_MNU_PAGECONTAINER
//     HANDLE hpBrush = SelectObject(hDC, GetSysColorBrush(COLOR_3DFACE));
//
// WM_DRAWITEM runs once per frame, so this leaked one GdiObject per frame for
// as long as the Launchpad was on screen. MEASURED before the fix: 1000 live
// objects 16 seconds into an idle Launchpad session, about 71 per second --
// one per frame at the frame rate, exactly as predicted.
//
// The cost is not only memory. Every object is reached through toObject(), an
// O(log n) lookup on that map, and every drawing call makes one -- so the map
// growing without bound makes all painting progressively slower, with nothing
// to point at the cause.
HBRUSH GetSysColorBrush(int index)
{
    static std::map<int, GdiObject *> sysBrushes;

    auto it = sysBrushes.find(index);
    if (it != sysBrushes.end()) return (HBRUSH)it->second;

    GdiObject *o = newObject(GdiObject::Brush);
    o->colour = GetSysColor(index);
    o->stock  = true;   // system brushes are not the caller's to delete
    sysBrushes[index] = o;
    return (HBRUSH)o;
}

// ===========================================================================
// Drawing primitives
// ===========================================================================

BOOL MoveToEx(HDC hdc, int x, int y, LPPOINT prev)
{
    DeviceContext *dc = toDC(hdc);
    if (!dc) return FALSE;
    if (prev) *prev = dc->current;
    dc->current.x = x;
    dc->current.y = y;
    return TRUE;
}

BOOL LineTo(HDC hdc, int x, int y)
{
    DeviceContext *dc = toDC(hdc);
    if (!dc) return FALSE;

    DrawCmd c;
    c.op = DrawCmd::Line;
    c.x0 = dc->current.x; c.y0 = dc->current.y;
    c.x1 = x;             c.y1 = y;
    captureState(dc, c);
    dc->commands.push_back(std::move(c));

    // GDI leaves the cursor at the end point.
    dc->current.x = x;
    dc->current.y = y;
    return TRUE;
}

BOOL Rectangle(HDC hdc, int left, int top, int right, int bottom)
{
    DeviceContext *dc = toDC(hdc);
    if (!dc) return FALSE;
    DrawCmd c;
    c.op = DrawCmd::Rect;
    c.x0 = left; c.y0 = top; c.x1 = right; c.y1 = bottom;
    captureState(dc, c);
    dc->commands.push_back(std::move(c));
    return TRUE;
}

BOOL Ellipse(HDC hdc, int left, int top, int right, int bottom)
{
    DeviceContext *dc = toDC(hdc);
    if (!dc) return FALSE;
    DrawCmd c;
    c.op = DrawCmd::Ellipse;
    c.x0 = left; c.y0 = top; c.x1 = right; c.y1 = bottom;
    captureState(dc, c);
    dc->commands.push_back(std::move(c));
    return TRUE;
}

BOOL Arc(HDC hdc, int left, int top, int right, int bottom,
         int xs, int ys, int xe, int ye)
{
    DeviceContext *dc = toDC(hdc);
    if (!dc) return FALSE;
    DrawCmd c;
    c.op = DrawCmd::Arc;
    c.x0 = left; c.y0 = top; c.x1 = right; c.y1 = bottom;
    c.x2 = xs;   c.y2 = ys;  c.x3 = xe;    c.y3 = ye;
    captureState(dc, c);
    dc->commands.push_back(std::move(c));
    return TRUE;
}

// A filled pie wedge: the same geometry as Arc, but closed through the centre
// and filled with the current brush.
//
// It was recorded as a plain Arc, with a comment claiming the replay closed
// the path whenever the command carried a brush. It did not -- the Arc case
// opened with `if (!c.hasOutline) break;` and only ever stroked. So a Pie
// drawn with a brush and a NULL pen, which is exactly how Dragonfly's radar
// antenna wedge is drawn, produced nothing at all.
BOOL Pie(HDC hdc, int left, int top, int right, int bottom,
         int xs, int ys, int xe, int ye)
{
    DeviceContext *dc = toDC(hdc);
    if (!dc) return FALSE;
    DrawCmd c;
    c.op = DrawCmd::Pie;
    c.x0 = left; c.y0 = top; c.x1 = right; c.y1 = bottom;
    c.x2 = xs;   c.y2 = ys;  c.x3 = xe;    c.y3 = ye;
    captureState(dc, c);
    dc->commands.push_back(std::move(c));
    return TRUE;
}

BOOL Polygon(HDC hdc, const POINT *pts, int count)
{
    DeviceContext *dc = toDC(hdc);
    if (!dc || !pts || count < 2) return FALSE;
    DrawCmd c;
    c.op = DrawCmd::Polygon;
    c.points.assign(pts, pts + count);
    captureState(dc, c);
    dc->commands.push_back(std::move(c));
    return TRUE;
}

BOOL Polyline(HDC hdc, const POINT *pts, int count)
{
    DeviceContext *dc = toDC(hdc);
    if (!dc || !pts || count < 2) return FALSE;
    DrawCmd c;
    c.op = DrawCmd::Polyline;
    c.points.assign(pts, pts + count);
    captureState(dc, c);
    dc->commands.push_back(std::move(c));
    return TRUE;
}

BOOL TextOutA(HDC hdc, int x, int y, LPCSTR str, int len)
{
    DeviceContext *dc = toDC(hdc);
    if (!dc || !str) return FALSE;
    DrawCmd c;
    c.op = DrawCmd::Text;
    c.x0 = x; c.y0 = y;
    c.text.assign(str, len >= 0 ? (size_t)len : strlen(str));
    c.textColour = dc->textColour;
    c.textAlign  = dc->textAlign;
    dc->commands.push_back(std::move(c));
    return TRUE;
}

COLORREF SetTextColor(HDC hdc, COLORREF colour)
{
    DeviceContext *dc = toDC(hdc);
    if (!dc) return 0;
    const COLORREF prev = dc->textColour;
    dc->textColour = colour;
    return prev;
}

COLORREF SetBkColor(HDC hdc, COLORREF colour)
{
    DeviceContext *dc = toDC(hdc);
    if (!dc) return 0;
    const COLORREF prev = dc->bkColour;
    dc->bkColour = colour;
    return prev;
}

int SetBkMode(HDC hdc, int mode)
{
    DeviceContext *dc = toDC(hdc);
    if (!dc) return 0;
    const int prev = dc->bkMode;
    dc->bkMode = mode;
    return prev;
}

UINT SetTextAlign(HDC hdc, UINT align)
{
    DeviceContext *dc = toDC(hdc);
    if (!dc) return 0;
    const UINT prev = dc->textAlign;
    dc->textAlign = align;
    return prev;
}

BOOL GetTextExtentPoint32A(HDC hdc, LPCSTR str, int len, LPSIZE size)
{
    if (!size) return FALSE;
    if (!str || len <= 0) { size->cx = size->cy = 0; return TRUE; }

    // Measured with ImGui's current font. Callers use this to lay text out, so
    // measuring with anything else would misplace it.
    const std::string s(str, (size_t)len);
    const ImVec2 sz = ImGui::CalcTextSize(s.c_str());
    size->cx = (LONG)sz.x;
    size->cy = (LONG)sz.y;
    (void)hdc;
    return TRUE;
}

BOOL BitBlt(HDC dst, int x, int y, int w, int h, HDC src, int sx, int sy, DWORD)
{
    DeviceContext *d = toDC(dst);
    if (!d) return FALSE;

    DeviceContext *s = toDC(src);
    DrawCmd c;
    c.op = DrawCmd::Blit;
    c.x0 = x; c.y0 = y; c.x1 = x + w; c.y1 = y + h;
    // A BitBlt is 1:1, so the source rectangle is the destination's size at
    // the source offset. PropertyList::OnPaint depends on this to pick the
    // expanded or collapsed arrow out of its 14-pixel strip.
    c.srcX = sx; c.srcY = sy; c.srcW = w; c.srcH = h;
    c.srcBitmap = s ? s->bitmap : nullptr;
    captureClip(d, c);
    d->commands.push_back(std::move(c));
    return TRUE;
}

BOOL StretchBlt(HDC dst, int x, int y, int w, int h,
                HDC src, int sx, int sy, int sw, int sh, DWORD)
{
    DeviceContext *d = toDC(dst);
    if (!d) return FALSE;

    DeviceContext *s = toDC(src);
    DrawCmd c;
    c.op = DrawCmd::Blit;
    c.x0 = x; c.y0 = y; c.x1 = x + w; c.y1 = y + h;
    c.srcX = sx; c.srcY = sy; c.srcW = sw; c.srcH = sh;
    c.srcBitmap = s ? s->bitmap : nullptr;
    captureClip(d, c);
    d->commands.push_back(std::move(c));
    return TRUE;
}

// A masked blit. Identical to StretchBlt except that source pixels equal to
// the key colour are not copied; see maskedTexture above for how that is
// realised.
BOOL TransparentBlt(HDC dst, int x, int y, int w, int h,
                    HDC src, int sx, int sy, int sw, int sh,
                    UINT transparentColour)
{
    DeviceContext *d = toDC(dst);
    if (!d) return FALSE;

    DeviceContext *s = toDC(src);
    DrawCmd c;
    c.op = DrawCmd::Blit;
    c.x0 = x; c.y0 = y; c.x1 = x + w; c.y1 = y + h;
    c.srcX = sx; c.srcY = sy; c.srcW = sw; c.srcH = sh;
    c.srcBitmap    = s ? s->bitmap : nullptr;
    c.hasColourKey = true;
    c.colourKey    = (COLORREF)transparentColour;
    captureClip(d, c);
    d->commands.push_back(std::move(c));
    return TRUE;
}

// ===========================================================================
// Off-screen bitmaps, rectangle fill, wide text and clipping
// ===========================================================================

// An empty bitmap of the given size, for use as a back buffer.
//
// The pixels are allocated but left transparent. Nothing reads them back --
// the callers draw into the memory DC and then blit it out, and both of those
// go through the command list rather than through the pixel buffer -- so the
// buffer exists to make GetObject report the right dimensions, which is what
// gcPropertyTree::Paint checks to decide whether to rebuild it.
HBITMAP CreateCompatibleBitmap(HDC, int w, int h)
{
    GdiObject *o = newObject(GdiObject::Bitmap);
    o->bmWidth  = w > 0 ? w : 0;
    o->bmHeight = h > 0 ? h : 0;
    const size_t bytes = (size_t)o->bmWidth * (size_t)o->bmHeight * 4;
    if (bytes) {
        o->pixels.assign(bytes, 0);
        o->bits = o->pixels.data();
    }
    return (HBITMAP)o;
}

// Fills a rectangle with the given brush, NOT with the DC's selected brush,
// and draws no border. That difference from Rectangle() is the whole point of
// the call, and gcPropertyTree relies on it: it fills every row with one of
// several brushes while a different brush stays selected for the tree spine.
//
// The right and bottom edges are exclusive, as they are for Rectangle.
int FillRect(HDC hdc, const RECT *r, HBRUSH brush)
{
    DeviceContext *dc = toDC(hdc);
    GdiObject     *b  = toObject((HGDIOBJ)brush);
    if (!dc || !r) return 0;

    DrawCmd c;
    c.op = DrawCmd::Rect;
    c.x0 = r->left; c.y0 = r->top;
    c.x1 = r->right; c.y1 = r->bottom;
    if (b && b->brushStyle != BS_NULL) {
        c.hasFill = true;
        c.fill    = b->colour;
    }
    c.hasOutline = false;
    captureClip(dc, c);
    dc->commands.push_back(std::move(c));
    return 1;
}

// Measures a string, and reports how many characters fit within maxExtent.
//
// This is the extended form: unlike GetTextExtentPoint32, it takes a width
// budget and can fill a per-character running-width array. gcPropertyTree
// passes a budget of 100000 and two nulls, wanting only the total, but the
// fit count and the width array are filled in when asked for so the function
// is not silently narrower than its name.
BOOL GetTextExtentExPointA(HDC hdc, LPCSTR str, int len, int maxExtent,
                           LPINT lpnFit, LPINT alpDx, LPSIZE size)
{
    (void)hdc;
    if (!size) return FALSE;
    if (!str || len <= 0) {
        size->cx = size->cy = 0;
        if (lpnFit) *lpnFit = 0;
        return TRUE;
    }

    const std::string s(str, (size_t)len);
    const ImVec2 total = ImGui::CalcTextSize(s.c_str());
    size->cx = (LONG)total.x;
    size->cy = (LONG)total.y;

    if (lpnFit || alpDx) {
        int fit = 0;
        for (int i = 1; i <= len; ++i) {
            const std::string prefix(str, (size_t)i);
            const int wpx = (int)ImGui::CalcTextSize(prefix.c_str()).x;
            if (alpDx) alpDx[i - 1] = wpx;
            if (wpx <= maxExtent) fit = i;
        }
        if (lpnFit) *lpnFit = fit;
    }
    return TRUE;
}

// Wide-character TextOut. The text is converted to UTF-8 and recorded through
// the same path as TextOutA, because that is what the font stack consumes.
//
// gcPropertyTree reaches this by round-tripping its values through
// std::wstring_convert specifically so that non-ASCII characters survive --
// the micro sign in its UNITS formatting is the reason -- so the conversion
// back has to be a real one and not a truncating cast.
BOOL TextOutW(HDC hdc, int x, int y, LPCWSTR str, int len)
{
    DeviceContext *dc = toDC(hdc);
    if (!dc || !str) return FALSE;

    std::string utf8;
    for (int i = 0; i < len && str[i]; ++i) {
        const unsigned int cp = (unsigned int)str[i];
        if (cp < 0x80) {
            utf8 += (char)cp;
        } else if (cp < 0x800) {
            utf8 += (char)(0xC0 | (cp >> 6));
            utf8 += (char)(0x80 | (cp & 0x3F));
        } else {
            utf8 += (char)(0xE0 | (cp >> 12));
            utf8 += (char)(0x80 | ((cp >> 6) & 0x3F));
            utf8 += (char)(0x80 | (cp & 0x3F));
        }
    }

    DrawCmd c;
    c.op = DrawCmd::Text;
    c.x0 = x; c.y0 = y;
    c.text       = std::move(utf8);
    c.textColour = dc->textColour;
    c.textAlign  = dc->textAlign;
    captureClip(dc, c);
    dc->commands.push_back(std::move(c));
    return TRUE;
}

// ---------------------------------------------------------------------------
// Regions
//
// Only rectangular regions exist here, which is all this tree creates. A
// region object carries its rectangle and nothing else.
// ---------------------------------------------------------------------------

namespace {
std::map<RECT *, std::unique_ptr<RECT>> g_regions;
}

HRGN CreateRectRgn(int left, int top, int right, int bottom)
{
    auto owned = std::make_unique<RECT>();
    owned->left = left; owned->top = top;
    owned->right = right; owned->bottom = bottom;
    RECT *raw = owned.get();
    g_regions.emplace(raw, std::move(owned));
    return (HRGN)raw;
}

// Selects a clipping region, or removes the clip when given null -- which is
// how Win32 spells "no clipping" and what gcPropertyTree::Paint does when it
// has finished.
//
// Selecting a region also clears the exclusion list, because ExcludeClipRect
// modifies the current clip and a new clip has no holes in it yet.
int SelectClipRgn(HDC hdc, HRGN rgn)
{
    DeviceContext *dc = toDC(hdc);
    if (!dc) return ERRORRGN;

    RECT *r = (RECT *)rgn;
    dc->excluded.clear();

    if (!r || !g_regions.count(r)) {
        dc->clipActive = false;
        return NULLREGION;
    }
    dc->clipActive = true;
    dc->clipRect   = *r;
    return SIMPLEREGION;
}

// Subtracts a rectangle from the clipping region.
//
// See the replay loop for how a subtractive region is honoured against an
// intersect-only clip stack, and for the limit of that approach.
int ExcludeClipRect(HDC hdc, int left, int top, int right, int bottom)
{
    DeviceContext *dc = toDC(hdc);
    if (!dc) return ERRORRGN;

    RECT r;
    r.left = left; r.top = top; r.right = right; r.bottom = bottom;
    dc->excluded.push_back(r);
    return COMPLEXREGION;
}

// ===========================================================================
// Bitmaps, icons and cursors
// ===========================================================================

HBITMAP LoadBitmapA(HINSTANCE, LPCSTR name)
{
    GdiObject *o = newObject(GdiObject::Bitmap);

    // Resource ids arrive packed into the pointer by MAKEINTRESOURCE. The
    // .bmp bytes come from the table rc2cpp.py generated, so this decodes
    // exactly what LoadBitmap decodes from the resource section on Windows.
    if (!IS_INTRESOURCE(name)) return (HBITMAP)o;

    // By TYPE as well as id -- see FindResourceOfType. IDI_FINGER2 and
    // IDR_IMAGE1 share id 292.
    const auto *res = orbiter_res::FindResourceOfType((int)(uintptr_t)name,
                                                      "BITMAP");
    if (!res) return (HBITMAP)o;

    std::vector<unsigned char> rgba;
    int w = 0, h = 0;
    if (!orbiter_res::DecodeBMP(res->data, res->size, rgba, w, h)) {
        fprintf(stderr, "Orbiter: could not decode bitmap resource %s\n",
                res->name ? res->name : "?");
        return (HBITMAP)o;
    }

    o->bmWidth  = w;
    o->bmHeight = h;
    o->pixels   = std::move(rgba);
    o->bits     = o->pixels.data();
    return (HBITMAP)o;
}

// LoadImage's LR_LOADFROMFILE path: `name` is a filename, not a packed id.
//
// WHAT THIS FIXES. ScnEditor's New Vessel tab reads an `ImageBmp` key from
// each vessel .cfg and loads that file for the preview panel:
//
//     hVesselBmp = (HBITMAP)LoadImage (ed->InstHandle(), imagename,
//                                      IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE);
//
// The flags argument was discarded and the call fell through to LoadBitmapA,
// which accepts only a MAKEINTRESOURCE id and returns an EMPTY bitmap object
// for anything else. Empty but NOT null -- so the caller's `if (hVesselBmp)`
// passed, GetObject reported bmWidth 0, and DrawVesselBmp's
//
//     h = min (imghmax, (int)(r.right*dy)/dx);
//
// divided by zero. Every stock vessel ships an ImageBmp, so selecting any of
// them in the New Vessel list killed the process with SIGFPE.
//
// Returning NULL when the file cannot be read is the half that kills the
// crash on its own: Windows' LoadImage returns NULL on failure and both
// callers here are written against that (`return (hVesselBmp != NULL)`, and
// DrawVesselBmp's else branch hides the control).
//
// WHY THERE IS LINUX-ONLY CODE HERE. The separator translation and the
// case-insensitive retry are forced, not embellishment: these paths come from
// .cfg files shared verbatim with the Windows build --
// "Images\\Vessels\\Default\\DeltaGlider.bmp" -- and ext4 resolves neither
// the backslashes nor a case mismatch. Same defect and same remedy as
// GraphicsClient::TexturePath, deliberately the same shape.
static HBITMAP loadBitmapFile(const char *name)
{
    if (!name || !*name) return nullptr;
    namespace fs = std::filesystem;

    std::string p(name);
    for (char &c : p) if (c == '\\') c = '/';

    std::error_code ec;
    if (!fs::exists(p, ec)) {
        const fs::path rel(p);
        const std::string want = rel.filename().string();
        fs::path dir = rel.parent_path();
        if (dir.empty()) dir = ".";

        bool found = false;
        for (const auto &e : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            if (strcasecmp(e.path().filename().string().c_str(),
                           want.c_str()) == 0) {
                p = e.path().string();
                found = true;
                break;
            }
        }
        if (!found) return nullptr;
    }

    FILE *f = fopen(p.c_str(), "rb");
    if (!f) return nullptr;

    std::vector<unsigned char> raw;
    if (fseek(f, 0, SEEK_END) == 0) {
        const long len = ftell(f);
        if (len > 0 && fseek(f, 0, SEEK_SET) == 0) {
            raw.resize((size_t)len);
            if (fread(raw.data(), 1, raw.size(), f) != raw.size()) raw.clear();
        }
    }
    fclose(f);
    if (raw.empty()) return nullptr;

    std::vector<unsigned char> rgba;
    int w = 0, h = 0;
    if (!orbiter_res::DecodeBMP(raw.data(), (int)raw.size(), rgba, w, h))
        return nullptr;

    GdiObject *o = newObject(GdiObject::Bitmap);
    o->bmWidth  = w;
    o->bmHeight = h;
    o->pixels   = std::move(rgba);
    o->bits     = o->pixels.data();
    return (HBITMAP)o;
}

HANDLE LoadImageA(HINSTANCE inst, LPCSTR name, UINT type, int cx, int cy,
                  UINT flags)
{
    if (type != IMAGE_BITMAP) return nullptr;

    if ((flags & LR_LOADFROMFILE) && !IS_INTRESOURCE(name))
        return (HANDLE)loadBitmapFile(name);

    HBITMAP bm = LoadBitmapA(inst, name);
    GdiObject *o = toObject((HGDIOBJ)bm);
    // A requested size of zero means "use the image's own", which is what
    // every caller in this tree passes.
    if (o && cx > 0 && cy > 0 && o->pixels.empty()) {
        o->bmWidth  = cx;
        o->bmHeight = cy;
    }
    return (HANDLE)bm;
}

HBITMAP CreateDIBSection(HDC, const BITMAPINFO *bmi, UINT, void **bits,
                         HANDLE, DWORD)
{
    GdiObject *o = newObject(GdiObject::Bitmap);
    if (bmi) {
        o->bmWidth = bmi->bmiHeader.biWidth;
        // A negative height means a top-down DIB; the magnitude is the size.
        o->bmHeight = bmi->bmiHeader.biHeight < 0
                    ? -bmi->bmiHeader.biHeight : bmi->bmiHeader.biHeight;
        const size_t bytes = (size_t)o->bmWidth * (size_t)o->bmHeight * 4;
        o->bits = bytes ? calloc(1, bytes) : nullptr;
    }
    if (bits) *bits = o->bits;
    return (HBITMAP)o;
}

// LoadIcon, decoding the .ico rc2cpp.py embedded for the ICON resource.
//
// The same shape as LoadBitmapA above: an id packed by MAKEINTRESOURCE, a
// lookup in the generated table, a decode into RGBA. The 32x32 image is the
// one chosen, which is what LoadIcon returns on Windows without
// LR_DEFAULTSIZE overrides.
//
// An icon is returned as a Bitmap object because that is all any caller here
// does with one -- there is no separate HICON type in this shim, and
// windows.h maps HICON to the same handle.
HICON LoadIconA(HINSTANCE, LPCSTR name)
{
    if (!IS_INTRESOURCE(name)) return nullptr;

    const auto *res = orbiter_res::FindResourceOfType((int)(uintptr_t)name,
                                                      "ICON");
    if (!res) return nullptr;

    const int count = orbiter_res::CountICOImages(res->data, res->size);
    if (count <= 0) return nullptr;

    // Prefer 32x32, else the largest that is not larger than 64 -- the sizes a
    // title bar and a task switcher ask for.
    int best = 0, bestW = 0;
    for (int i = 0; i < count; ++i) {
        int w = 0, h = 0;
        if (!orbiter_res::GetICOImageSize(res->data, res->size, i, w, h))
            continue;
        if (w == 32) { best = i; bestW = w; break; }
        if (w > bestW && w <= 64) { best = i; bestW = w; }
    }

    std::vector<unsigned char> rgba;
    int w = 0, h = 0;
    if (!orbiter_res::DecodeICO(res->data, res->size, best, rgba, w, h)) {
        fprintf(stderr, "Orbiter: could not decode icon resource %s\n",
                res->name ? res->name : "?");
        return nullptr;
    }

    GdiObject *o = newObject(GdiObject::Bitmap);
    o->bmWidth  = w;
    o->bmHeight = h;
    o->pixels   = std::move(rgba);
    o->bits     = o->pixels.data();
    return (HICON)o;
}

HCURSOR LoadCursorA(HINSTANCE, LPCSTR) { return nullptr; }

// ---------------------------------------------------------------------------
// The window icon, for UIHost.
//
// On Windows the icon reaches the title bar through the window CLASS --
// Orbiter::Create sets wndClass.hIcon and RegisterClass does the rest. X11 has
// no class-level icon: _NET_WM_ICON is a property on the window itself,
// carrying the pixels, and glfwSetWindowIcon is the way to set it. So UIHost
// asks for the images and hands them over after creating the window.
//
// Several sizes are returned rather than one, because the window manager picks
// per use -- a small one for the title bar, a larger one for the task switcher
// -- and giving it only 32x32 makes everything else a scaled 32x32.
//
// Returns the number of images written, up to `max`.
extern "C" int orbiter_GetWindowIconImages(int resId, int max, int *widths,
                                           int *heights,
                                           unsigned char **pixels)
{
    if (max <= 0 || !widths || !heights || !pixels) return 0;

    const auto *res = orbiter_res::FindResourceOfType(resId, "ICON");
    if (!res) return 0;

    const int count = orbiter_res::CountICOImages(res->data, res->size);
    if (count <= 0) return 0;

    // Cached: the pixels outlive this call because glfwSetWindowIcon copies
    // them, but the caller has no place to own them either, and a window icon
    // is set at most a handful of times per run.
    static std::vector<std::vector<unsigned char>> keep;

    int n = 0;
    for (int i = 0; i < count && n < max; ++i) {
        int w = 0, h = 0;
        if (!orbiter_res::GetICOImageSize(res->data, res->size, i, w, h))
            continue;
        // 256x256 is 262 kB of pixels for an icon no window manager asks for
        // at that size here; the useful range is 16..64.
        if (w > 128 || h > 128) continue;

        std::vector<unsigned char> rgba;
        int dw = 0, dh = 0;
        if (!orbiter_res::DecodeICO(res->data, res->size, i, rgba, dw, dh))
            continue;

        keep.push_back(std::move(rgba));
        widths[n]  = dw;
        heights[n] = dh;
        pixels[n]  = keep.back().data();
        ++n;
    }
    return n;
}

// ===========================================================================
// WGL
//
// Reporting failure, deliberately and consistently.
//
// There is no GL surface behind a DC here -- the renderer is Vulkan and a DC
// is a command recorder, not a drawable. Dragonfly's instrument panel probes
// for a pixel format and only builds its GL path if it gets one, so returning
// zero makes it fall back cleanly. Returning a fake format and a null context
// would let it proceed and then draw into nothing, which is far harder to
// diagnose than an honest refusal.
//
// This is the seam to implement against when a graphics client exists.
// ===========================================================================

int ChoosePixelFormat(HDC, const PIXELFORMATDESCRIPTOR *) { return 0; }

BOOL SetPixelFormat(HDC, int, const PIXELFORMATDESCRIPTOR *) { return FALSE; }

int DescribePixelFormat(HDC, int, UINT bytes, LPPIXELFORMATDESCRIPTOR pfd)
{
    // Zeroing the caller's struct matters: it inspects the returned flags,
    // and leaving a stack buffer untouched would have it read whatever was
    // there before.
    if (pfd && bytes >= sizeof(PIXELFORMATDESCRIPTOR)) {
        memset(pfd, 0, sizeof(*pfd));
        pfd->nSize    = sizeof(PIXELFORMATDESCRIPTOR);
        pfd->nVersion = 1;
    }
    return 0;   // no formats available
}

HGLRC wglCreateContext(HDC)          { return nullptr; }
BOOL  wglMakeCurrent(HDC, HGLRC)     { return FALSE; }
BOOL  wglDeleteContext(HGLRC)        { return TRUE; }
HGLRC wglGetCurrentContext(void)     { return nullptr; }

// ===========================================================================
// Image lists
//
// Used for the scenario tree's icons. Backed by the same bitmap objects above,
// so the images appear once the imaging layer can supply pixels.
// ===========================================================================

namespace {
struct ImageList {
    int cx = 0, cy = 0;
    std::vector<GdiObject *> images;
};

// ===========================================================================
// ALLOCATED ONCE AND NEVER FREED, and that is deliberate. This is the same
// static-destruction-order fault Win32Dlg.cpp's windowClasses() already
// solves, in the same way, for the same reason -- it simply had not been
// applied here.
//
// This map lives in the EXECUTABLE. The code that empties it lives in a
// PLUGIN: ScnEditor's destructor calls ImageList_Destroy from ExitModule,
// which the dynamic loader runs from _dl_fini during exit(). Whether this
// map is still alive at that moment is not defined by anything --
//
//     exit(0)  ->  __run_exit_handlers  ->  _dl_fini
//       ->  orb_module_detach  ->  ExitLib  ->  ExitModule
//       ->  ScnEditor::~ScnEditor  ->  ImageList_Destroy
//       ->  g_imageLists.erase()   on a map that may already be destroyed
//
// -- and the symptom is not a clean crash but
//     double free or corruption (!prev)
// raised from inside the allocator, several frames away from the cause.
//
// Never destroying it removes the ordering question entirely: the map stays
// valid for as long as any code can reach it. The "leak" is reclaimed by the
// kernel at process exit like everything else.
//
// This had never fired because nothing in this port had ever reached a clean
// exit: the render window was created under an unregistered window class, so
// WM_CLOSE never reached RenderWndProc and CloseSession never ran. Fixing
// that surfaced this on the first shutdown.
std::map<ImageList *, std::unique_ptr<ImageList>> &imageLists()
{
    static std::map<ImageList *, std::unique_ptr<ImageList>> *lists =
        new std::map<ImageList *, std::unique_ptr<ImageList>>();
    return *lists;
}
} // namespace

#define g_imageLists imageLists()

HIMAGELIST ImageList_Create(int cx, int cy, UINT, int, int)
{
    auto owned = std::make_unique<ImageList>();
    owned->cx = cx;
    owned->cy = cy;
    ImageList *raw = owned.get();
    g_imageLists.emplace(raw, std::move(owned));
    return (HIMAGELIST)raw;
}

BOOL ImageList_Destroy(HIMAGELIST h)
{
    ImageList *il = (ImageList *)h;
    if (!il || !g_imageLists.count(il)) return FALSE;
    g_imageLists.erase(il);
    return TRUE;
}

int ImageList_Add(HIMAGELIST h, HBITMAP image, HBITMAP)
{
    ImageList *il = (ImageList *)h;
    if (!il || !g_imageLists.count(il)) return -1;
    il->images.push_back(toObject((HGDIOBJ)image));
    return (int)il->images.size() - 1;
}

// Resolves a tree view's image-list entry to a renderer texture.
//
// The tree attaches its list with TVM_SETIMAGELIST and then refers to icons by
// index; the renderer needs a texture and UVs. Each entry is a separate
// bitmap here rather than a strip, so the UVs are the whole image -- but they
// are still reported, because the Windows control does use a strip and a
// future change to match that would only alter these four numbers.
unsigned long long orbiter_ImageListTexture(HWND tree, int index,
                                            float *u0, float *v0,
                                            float *u1, float *v1)
{
    if (u0) *u0 = 0.0f;
    if (v0) *v0 = 0.0f;
    if (u1) *u1 = 1.0f;
    if (v1) *v1 = 1.0f;

    HIMAGELIST hil = orbiter_TreeImageList(tree);
    ImageList *il = (ImageList *)hil;
    if (!il || !g_imageLists.count(il)) return 0;
    if (index < 0 || index >= (int)il->images.size()) return 0;

    GdiObject *bm = il->images[index];
    if (!bm || bm->pixels.empty()) return 0;

    if (bm->texture == 0)
        bm->texture = orbiter_UploadTexture(bm->pixels.data(),
                                            bm->bmWidth, bm->bmHeight);
    return bm->texture;
}

// Resolves a bitmap resource id straight to a renderer texture.
//
// An SS_BITMAP static is drawn by Windows itself from the resource named in
// its template -- it is not owner-drawn, and no WM_DRAWITEM is sent for it.
// IDC_LOGO is exactly this: Launchpad.cpp's WM_DRAWITEM handler covers only
// IDC_SHADOW and IDC_MNU_PAGECONTAINER, so if the host does not draw the
// bitmap nothing does.
//
// The decoded image is cached, since the same banner is drawn every frame.
unsigned long long orbiter_BitmapTexture(int resId, int *width, int *height)
{
    static std::map<int, GdiObject *> cache;

    GdiObject *bm = nullptr;
    auto it = cache.find(resId);
    if (it != cache.end() && g_objects.count(it->second)) {
        bm = it->second;
    } else {
        bm = toObject((HGDIOBJ)LoadBitmapA(nullptr, MAKEINTRESOURCE(resId)));
        if (!bm) return 0;
        cache[resId] = bm;
    }

    if (bm->pixels.empty()) return 0;
    if (width)  *width  = bm->bmWidth;
    if (height) *height = bm->bmHeight;

    if (bm->texture == 0)
        bm->texture = orbiter_UploadTexture(bm->pixels.data(),
                                            bm->bmWidth, bm->bmHeight);
    return bm->texture;
}

} // extern "C"

// ===========================================================================
// Text metrics, pixels and the viewport origin
//
// Added for the graphics client's Sketchpad, which draws the HUD, the MFDs
// and the panel instruments. All three are ordinary GDI operations; they had
// no caller until a client existed, which is why they were declared but never
// implemented.
// ===========================================================================

BOOL GetTextMetricsA(HDC hdc, LPTEXTMETRIC tm)
{
    DeviceContext *dc = toDC(hdc);
    if (!dc || !tm) return FALSE;

    memset(tm, 0, sizeof(*tm));

    // Derived from the selected font's own metrics, so instruments that lay
    // themselves out from tmHeight and tmAveCharWidth -- which most of the
    // MFDs do -- get numbers consistent with what is actually drawn.
    const GdiObject *f = dc->font;
    const float height = f && f->fontHeight ? (float)f->fontHeight
                                            : ImGui::GetFontSize();

    tm->tmHeight          = (LONG)height;
    tm->tmAscent          = (LONG)(height * 0.8f);
    tm->tmDescent         = (LONG)(height * 0.2f);
    tm->tmInternalLeading = 0;
    tm->tmExternalLeading = 0;
    tm->tmWeight          = f ? f->fontWeight : FW_NORMAL;

    // Average character width measured rather than guessed: the width of a
    // representative sample divided by its length.
    static const char sample[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                 "abcdefghijklmnopqrstuvwxyz";
    SIZE sz{};
    if (GetTextExtentPoint32A(hdc, sample, (int)(sizeof(sample) - 1), &sz))
        tm->tmAveCharWidth = sz.cx / (LONG)(sizeof(sample) - 1);
    else
        tm->tmAveCharWidth = (LONG)(height * 0.5f);

    tm->tmMaxCharWidth = tm->tmAveCharWidth * 2;
    return TRUE;
}

// The pixel pair.
//
// A DC in this file is a display-list recorder, so "the pixel at x,y" only
// exists when the DC has a BITMAP selected into it -- a memory DC. That is
// precisely the case Win32 code uses these two for, and the case
// OVP/VulkanClient's WindowMgr needs: it selects its title-bar graphic into
// one memory DC and a compatible bitmap into another, then recolours the
// image a pixel at a time.
//
// So both work on the selected bitmap's pixel buffer when there is one. For a
// screen DC SetPixel keeps recording a one-pixel rectangle, which is the only
// thing it can do and what it did before, and GetPixel reports CLR_INVALID --
// Win32's own answer for a point it cannot read.
//
// `pixels` is decoded RGBA8, so the byte order is R,G,B,A and a COLORREF is
// 0x00BBGGRR: the channels are reversed between the two and the conversion
// below is where that is handled.

COLORREF GetPixel(HDC hdc, int x, int y)
{
    DeviceContext *dc = toDC(hdc);
    if (!dc || !dc->bitmap) return CLR_INVALID;

    GdiObject *bm = dc->bitmap;
    if (bm->pixels.empty()) return CLR_INVALID;
    if (x < 0 || y < 0 || x >= bm->bmWidth || y >= bm->bmHeight) return CLR_INVALID;

    const size_t i = ((size_t)y * (size_t)bm->bmWidth + (size_t)x) * 4;
    if (i + 3 >= bm->pixels.size()) return CLR_INVALID;

    const unsigned char *p = bm->pixels.data() + i;
    return RGB(p[0], p[1], p[2]);
}

COLORREF SetPixel(HDC hdc, int x, int y, COLORREF colour)
{
    DeviceContext *dc = toDC(hdc);
    if (!dc) return (COLORREF)-1;

    // A memory DC: write the bitmap, which is what Win32 does and what makes
    // the result survive to the BitBlt that reads it. Recording a rectangle
    // into a memory DC's command list would go nowhere at all, because a blit
    // FROM a memory DC takes its bitmap, not its commands.
    if (GdiObject *bm = dc->bitmap) {
        if (!bm->pixels.empty() &&
            x >= 0 && y >= 0 && x < bm->bmWidth && y < bm->bmHeight) {

            const size_t i = ((size_t)y * (size_t)bm->bmWidth + (size_t)x) * 4;
            if (i + 3 < bm->pixels.size()) {
                unsigned char *p = bm->pixels.data() + i;
                p[0] = GetRValue(colour);
                p[1] = GetGValue(colour);
                p[2] = GetBValue(colour);
                p[3] = 0xFF;
                // The texture is uploaded once and cached on the object, so a
                // write after that upload would never be seen. Marking it dirty
                // makes the next draw re-upload -- once, not once per pixel.
                bm->pixelsDirty = true;
                return colour;
            }
        }
        return (COLORREF)-1;
    }

    // A one-pixel filled rectangle: the draw list has no point primitive, and
    // a degenerate rectangle is exactly one pixel.
    DrawCmd c;
    c.op = DrawCmd::Rect;
    c.x0 = x; c.y0 = y; c.x1 = x + 1; c.y1 = y + 1;
    captureState(dc, c);
    c.hasFill    = true;
    c.fill       = colour;
    c.hasOutline = false;
    dc->commands.push_back(std::move(c));
    return colour;
}

BOOL SetViewportOrgEx(HDC hdc, int x, int y, LPPOINT prev)
{
    DeviceContext *dc = toDC(hdc);
    if (!dc) return FALSE;
    if (prev) { prev->x = dc->originX; prev->y = dc->originY; }
    dc->originX = x;
    dc->originY = y;
    return TRUE;
}

// The read half of the pair above. GDIPad::GetOrigin is the caller; on
// Windows it is the only way to ask a DC where its origin was put, because
// SetViewportOrgEx's `prev` only answers while you are moving it.
BOOL GetViewportOrgEx(HDC hdc, LPPOINT pt)
{
    DeviceContext *dc = toDC(hdc);
    if (!dc || !pt) return FALSE;
    pt->x = dc->originX;
    pt->y = dc->originY;
    return TRUE;
}

// PolyPolygon and PolyPolyline draw N figures from one flat point array, with
// a per-figure count array. There is nothing in the recorder that needs to
// know they arrived together -- each figure is an independent command -- so
// both are the single-figure call in a loop, which is also exactly what the
// picture is.
//
// The two differ in their count array's type, and that is Win32's doing, not
// a mistake: PolyPolygon takes `const int *` and PolyPolyline takes
// `const DWORD *`. Both are kept as declared so the call sites do not have to
// cast.
BOOL PolyPolygon(HDC hdc, const POINT *pts, const int *counts, int nfig)
{
    DeviceContext *dc = toDC(hdc);
    if (!dc || !pts || !counts || nfig < 1) return FALSE;
    BOOL ok = TRUE;
    const POINT *p = pts;
    for (int i = 0; i < nfig; i++) {
        if (counts[i] >= 2) { if (!Polygon(hdc, p, counts[i])) ok = FALSE; }
        p += counts[i];
    }
    return ok;
}

BOOL PolyPolyline(HDC hdc, const POINT *pts, const DWORD *counts, DWORD nfig)
{
    DeviceContext *dc = toDC(hdc);
    if (!dc || !pts || !counts || nfig < 1) return FALSE;
    BOOL ok = TRUE;
    const POINT *p = pts;
    for (DWORD i = 0; i < nfig; i++) {
        if (counts[i] >= 2) { if (!Polyline(hdc, p, (int)counts[i])) ok = FALSE; }
        p += counts[i];
    }
    return ok;
}

// DrawTextA lays a string out inside a rectangle. GDIPad::TextBox is the only
// caller in this tree and it always asks for DT_LEFT | DT_NOPREFIX |
// DT_WORDBREAK -- left aligned, no '&' accelerator handling, wrap at the
// right edge -- so that is what this does, and the other DT_ flags are
// accepted and ignored rather than pretended.
//
// WORD WRAPPING IS DONE HERE because the recorder has no layout engine: it
// records TextOut calls at fixed positions. So the string is split into lines
// that fit, each line becomes one TextOutA, and the line height comes from
// GetTextMetricsA -- the same number GDIPad::GetCharSize reports.
//
// The return value is GDI's: the height of the drawn text, or 0 on failure.
int DrawTextA(HDC hdc, LPCSTR str, int len, LPRECT rc, UINT format)
{
    DeviceContext *dc = toDC(hdc);
    if (!dc || !str || !rc) return 0;

    const size_t slen = (len >= 0) ? (size_t)len : strlen(str);
    if (!slen) return 0;

    TEXTMETRIC tm;
    if (!GetTextMetricsA(hdc, &tm)) return 0;
    const int lineh = tm.tmHeight ? tm.tmHeight : 12;

    const int boxw = rc->right - rc->left;
    if (boxw <= 0) return 0;

    // The previous alignment is saved and restored: DrawText is documented to
    // ignore the DC's text alignment, and leaving TA_LEFT behind would change
    // where the caller's next TextOut lands.
    const UINT prevAlign = SetTextAlign(hdc, TA_LEFT | TA_TOP);

    int y = rc->top;
    size_t i = 0;

    while (i < slen) {

        // How much of the remainder fits on one line?
        size_t take = slen - i;
        size_t lastBreak = 0;
        SIZE sz;

        for (size_t n = 1; n <= slen - i; n++) {
            if (str[i + n - 1] == '\n') { take = n; lastBreak = n; break; }
            if (!GetTextExtentPoint32A(hdc, str + i, (int)n, &sz)) { take = n; break; }
            if (sz.cx > boxw && n > 1) {
                // Too wide. Back up to the last space, if the caller asked
                // for word breaking and there is one.
                take = (format & DT_WORDBREAK) && lastBreak ? lastBreak : n - 1;
                break;
            }
            if (str[i + n - 1] == ' ') lastBreak = n;
            take = n;
        }

        size_t draw = take;
        while (draw && (str[i + draw - 1] == '\n' || str[i + draw - 1] == '\r')) draw--;

        if (draw) TextOutA(hdc, rc->left, y, str + i, (int)draw);

        y += lineh;
        i += take;
        if (y >= rc->bottom) break;		// DT_NOCLIP is not asked for
    }

    SetTextAlign(hdc, prevAlign);

    return y - rc->top;
}

// ===========================================================================
// Compositing one device context into another
//
// This GDI layer RECORDS drawing commands and replays them into the frame; it
// does not rasterise into a bitmap. That is what makes it fast and what lets
// Orbiter's dialogs and instruments draw with no software renderer behind
// them -- but it means BitBlt between two of these device contexts has no
// pixels to copy.
//
// It matters because that is exactly how Orbiter composites its 2D output:
// every MFD draws into its own surface, and the panel code blits those
// surfaces onto the main render surface. With a pixel-copying BitBlt those
// blits move nothing, and the instruments vanish -- drawn correctly into
// their surfaces and then never reaching the screen.
//
// The equivalent operation for a command recorder is to APPEND the source's
// commands to the target, translated by the blit offset. The result is
// identical once replayed, and the source is left intact so it can be
// composited again next frame.
// ===========================================================================

extern "C" void orbiter_AppendDC(HDC dstDC, HDC srcDC, int dx, int dy,
                                 int clipW, int clipH)
{
    DeviceContext *dst = toDC(dstDC);
    DeviceContext *src = toDC(srcDC);
    if (!dst || !src || dst == src) return;

    const size_t first = dst->commands.size();
    dst->commands.insert(dst->commands.end(),
                         src->commands.begin(), src->commands.end());

    // Translate the copied commands into the target's coordinates, and clip
    // them to the blit rectangle so a surface cannot draw outside the area it
    // was given.
    for (size_t i = first; i < dst->commands.size(); ++i) {
        DrawCmd &c = dst->commands[i];

        c.x0 += dx; c.y0 += dy;
        c.x1 += dx; c.y1 += dy;
        c.x2 += dx; c.y2 += dy;
        c.x3 += dx; c.y3 += dy;
        for (POINT &p : c.points) { p.x += dx; p.y += dy; }

        if (clipW > 0 && clipH > 0) {
            c.hasClip  = true;
            c.clip     = { dx, dy, dx + clipW, dy + clipH };
        }
    }
}
