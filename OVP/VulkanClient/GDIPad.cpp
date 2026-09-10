// ==============================================================
// GDIPad.cpp
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2006-2026 Martin Schweiger
// ==============================================================
//
// CONVERTED FROM OVP/D3D9Client/GDIPad.cpp, read end to end (306 lines).
//
// THE GDI SKETCHPAD, AND IT STAYS A GDI SKETCHPAD. Every method is a Win32
// GDI call -- SelectObject, TextOut, MoveToEx, Rectangle, PolyPolyline -- and
// the shim implements all of them (Src/Orbiter/Linux/Gdi.cpp). Not one
// Direct3D call appears in the file. So the conversion is three renames of
// the pad's own resource classes:
//
//   D3D9PadFont / D3D9PadPen / D3D9PadBrush -> VulkanPadFont / ...Pen /
//   ...Brush. Those are the objects clbkCreateFont and friends return, and
//   this file reaches into them for the raw HFONT / HPEN / HBRUSH.
//
// WORTH KNOWING: on Linux `Gdi.cpp` is a DISPLAY LIST RECORDER, not a
// rasteriser -- TextOutA appends a DrawCmd and orbiter_ReplayDC turns the
// list into ImGui draw commands. These calls therefore still produce a
// picture; what they do NOT do is leave pixels in a bitmap that something
// else can read back. That distinction is why VulkanTextMgr had to be
// rebuilt on stb_truetype and this file did not have to change at all.
//
// UTF8ToCP1252 IS THE ONE FUNCTION WHOSE BEHAVIOUR CHANGES, and for the
// reason recorded as finding 18: the Windows version is named for CP1252 and
// converts to 28591 (ISO-8859-1), so the twenty-seven printable characters
// CP1252 puts in 0x80-0x9F -- the smart quotes, the dashes, the ellipsis, the
// bullet, the euro sign -- are substituted with '?'. This is the same
// conversion VulkanPad.cpp carries, written out because the two Win32 NLS
// calls it was built from (MultiByteToWideChar / WideCharToMultiByte) have no
// counterpart here. The two copies are duplicated exactly as the Windows
// source duplicates them.
// ==============================================================

#include "GDIPad.h"
#include "VulkanPad.h"
#include "VulkanClient.h"
#include "VulkanSurface.h"
#include "VulkanUtil.h"
#include "VulkanConfig.h"
#include "Log.h"

using namespace oapi;


static std::string UTF8ToCP1252(const char *utf8, int ulen)
{
	if (!utf8) return std::string();
	if (ulen < 0) ulen = (int)strlen(utf8);

	std::string out;
	out.reserve((size_t)ulen);

	const unsigned char *s = (const unsigned char *)utf8;
	int i = 0;

	while (i < ulen) {

		const unsigned char c = s[i];
		unsigned int cp = 0;
		int extra = 0;

		if (c < 0x80)			{ cp = c;			extra = 0; }
		else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F;	extra = 1; }
		else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F;	extra = 2; }
		else if ((c & 0xF8) == 0xF0) { cp = c & 0x07;	extra = 3; }
		else {
			// A continuation byte or an invalid lead. This is
			// MB_ERR_INVALID_CHARS failing, and the reference's answer is to
			// assume the caller handed us 8-bit text already.
			return std::string(utf8, (size_t)ulen);
		}

		if (i + extra >= ulen) return std::string(utf8, (size_t)ulen);

		for (int k = 1; k <= extra; k++) {
			const unsigned char cc = s[i + k];
			if ((cc & 0xC0) != 0x80) return std::string(utf8, (size_t)ulen);
			cp = (cp << 6) | (cc & 0x3F);
		}
		i += extra + 1;

		// --- Unicode -> CP1252 ---
		if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF)) {
			// ASCII, and the range where CP1252 and Latin-1 agree.
			out.push_back((char)cp);
			continue;
		}

		// The twenty-seven printable characters CP1252 puts in 0x80-0x9F.
		// The table is VulkanTextMgr.cpp's kCp1252High read the other way;
		// it is repeated rather than exported because it is thirty-two
		// entries and this is not a hot path.
		static const unsigned short kHigh[32] = {
			0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
			0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
			0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
			0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
		};

		char mapped = '?';
		for (int k = 0; k < 32; k++) {
			if (kHigh[k] == cp) { mapped = (char)(0x80 + k); break; }
		}
		out.push_back(mapped);
	}

	return out;
}

// ===============================================================================================
// class GDIPad
// ===============================================================================================

GDIPad::GDIPad (SURFHANDLE s, HDC hdc): Sketchpad (s)
{
	LogOk("Creating GDI SketchPad... for Surface %s", _PTR(s));

	hDC    = hdc;
	cfont  = NULL;
	cpen   = NULL;
	cbrush = NULL;
	hFont0 = NULL;
	hFontA = NULL;

	// Default initial drawing settings
	SetBkMode (hDC, TRANSPARENT); // transparent text background
	SelectObject(hDC, GetStockObject (NULL_PEN));
	SelectObject(hDC, GetStockObject (NULL_BRUSH));
}

// ===============================================================================================
//
GDIPad::~GDIPad ()
{
	// make sure to deselect custom resources before destroying the DC
	if (hFont0) SelectObject (hDC, hFont0);
	SelectObject (hDC, GetStockObject (NULL_PEN));
	SelectObject (hDC, GetStockObject (NULL_BRUSH));
	if (hFontA) DeleteObject(hFontA);
	LogOk("...GDI SketchPad Released for surface %s", _PTR(GetSurface()));
}

// ===============================================================================================
//
HDC GDIPad::GetDC()
{
	return hDC;
}

// ===============================================================================================
//
Font *GDIPad::SetFont (Font *font)
{
	Font *pfont = cfont;
	if (font) {
		HFONT hFont = (HFONT)SelectObject (hDC, static_cast<VulkanPadFont*>(font)->hFont);
		if (!cfont) hFont0 = hFont;
	} else if (hFont0) { // restore original font
		SelectObject (hDC, hFont0);
		hFont0 = 0;
	}
	cfont = font;
	return pfont;
}

// ===============================================================================================
//
Pen *GDIPad::SetPen (Pen *pen)
{
	Pen *ppen = cpen;
	if (pen) cpen = pen;
	else     cpen = NULL;
	if (cpen) SelectObject (hDC, static_cast<VulkanPadPen*>(cpen)->hPen);
	else      SelectObject (hDC, GetStockObject (NULL_PEN));
	return ppen;
}

// ===============================================================================================
//
Brush *GDIPad::SetBrush (Brush *brush)
{
	Brush *pbrush = cbrush;
	cbrush = brush;
	if (brush) SelectObject (hDC, static_cast<VulkanPadBrush*>(cbrush)->hBrush);
	else SelectObject (hDC, GetStockObject (NULL_BRUSH));
	return pbrush;
}

// ===============================================================================================
//
void GDIPad::SetTextAlign (TAlign_horizontal tah, TAlign_vertical tav)
{
	UINT align = 0;
	switch (tah) {
		case LEFT:     align |= TA_LEFT;     break;
		case CENTER:   align |= TA_CENTER;   break;
		case RIGHT:    align |= TA_RIGHT;    break;
	}
	switch (tav) {
		case TOP:      align |= TA_TOP;      break;
		case BASELINE: align |= TA_BASELINE; break;
		case BOTTOM:   align |= TA_BOTTOM;   break;
	}
	::SetTextAlign (hDC, align);
}

// ===============================================================================================
//
DWORD GDIPad::SetTextColor (DWORD col)
{
	return (DWORD)::SetTextColor (hDC, (COLORREF)(col&0xFFFFFF));
}

// ===============================================================================================
//
DWORD GDIPad::SetBackgroundColor (DWORD col)
{
	return (DWORD)SetBkColor (hDC, (COLORREF)(col&0xFFFFFF));
}

// ===============================================================================================
//
void GDIPad::SetBackgroundMode (BkgMode mode)
{
	int bkmode;
	switch (mode) {
		case BK_TRANSPARENT: bkmode = TRANSPARENT; break;
		case BK_OPAQUE:      bkmode = OPAQUE; break;
		default: return;
	}
	SetBkMode (hDC, bkmode);
}

// ===============================================================================================
//
DWORD GDIPad::GetCharSize ()
{
	TEXTMETRIC tm;
	GetTextMetrics (hDC, &tm);
	return MAKELONG(tm.tmHeight-tm.tmInternalLeading, tm.tmAveCharWidth);
}

// ===============================================================================================
//
DWORD GDIPad::GetTextWidth (const char *utf8, int len)
{
	if (utf8) if (utf8[0] == '_') if (strcmp(utf8, "_SkpVerInfo") == 0) return 1;
	SIZE size;
	if (!len) len = lstrlen(utf8);
	std::string str = UTF8ToCP1252(utf8, len);

	GetTextExtentPoint32 (hDC, str.c_str(), str.length(), &size);
	return (DWORD)size.cx;
}

// ===============================================================================================
//
void GDIPad::SetOrigin (int x, int y)
{
	SetViewportOrgEx (hDC, x, y, NULL);
}

// ===============================================================================================
//
void GDIPad::GetOrigin (int *x, int *y) const
{
	POINT point;
	GetViewportOrgEx (hDC, &point);
	if (x) *x = point.x;
	if (y) *y = point.y;
}

// ===============================================================================================
//
bool GDIPad::Text (int x, int y, const char *utf8, int len)
{
	std::string str = UTF8ToCP1252(utf8, len);
	return (TextOut (hDC, x, y, str.c_str(), str.length()) != FALSE);
}

// ===============================================================================================
//
bool GDIPad::TextW (int x, int y, const LPWSTR str, int len)
{
	return (TextOutW(hDC, x, y, str, len) != FALSE);
}

// ===============================================================================================
//
bool GDIPad::TextBox (int x1, int y1, int x2, int y2, const char *utf8, int len)
{
	std::string str = UTF8ToCP1252(utf8, len);
	RECT r;
	r.left =   x1;
	r.top =    y1;
	r.right =  x2;
	r.bottom = y2;
	return (DrawText (hDC, str.c_str(), str.length(), &r, DT_LEFT|DT_NOPREFIX|DT_WORDBREAK) != 0);
}

// ===============================================================================================
//
void GDIPad::Pixel (int x, int y, DWORD col)
{
	SetPixel (hDC, x, y, (COLORREF)col);
}

// ===============================================================================================
//
void GDIPad::MoveTo (int x, int y)
{
	MoveToEx (hDC, x, y, NULL);
}

// ===============================================================================================
//
void GDIPad::LineTo (int x, int y)
{
	::LineTo (hDC, x, y);
}

// ===============================================================================================
//
void GDIPad::Line (int x0, int y0, int x1, int y1)
{
	MoveToEx (hDC, x0, y0, NULL);
	::LineTo (hDC, x1, y1);
}

// ===============================================================================================
//
void GDIPad::Rectangle (int x0, int y0, int x1, int y1)
{
	::Rectangle (hDC, x0, y0, x1, y1);
}

// ===============================================================================================
//
void GDIPad::Ellipse (int x0, int y0, int x1, int y1)
{
	::Ellipse (hDC, x0, y0, x1, y1);
}

// ===============================================================================================
//
void GDIPad::Polygon (const IVECTOR2 *pt, int npt)
{
	::Polygon (hDC, (const POINT*)pt, npt);
}

// ===============================================================================================
//
void GDIPad::Polyline (const IVECTOR2 *pt, int npt)
{
	::Polyline (hDC, (const POINT*)pt, npt);
}

// ===============================================================================================
//
void GDIPad::PolyPolygon (const IVECTOR2 *pt, const int *npt, const int nline)
{
	::PolyPolygon (hDC, (const POINT*)pt, npt, nline);
}

// ===============================================================================================
//
void GDIPad::PolyPolyline (const IVECTOR2 *pt, const int *npt, const int nline)
{
	::PolyPolyline (hDC, (const POINT*)pt, (const DWORD*)npt, nline);
}

