// =================================================================================================================================
// The MIT Lisence:
//
// Copyright (C) 2012-2026 Jarmo Nikkanen
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy,
// modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software
// is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
// IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// =================================================================================================================================
//
// CONVERTED FROM OVP/D3D9Client/D3D9TextMgr.cpp, read end to end (619 lines).
// See VulkanTextMgr.h for why Init() is the one function here that could not
// be converted line for line.
// =================================================================================================================================

#include <windows.h>
#include <stdio.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include "VulkanTextMgr.h"
#include "Log.h"
#include "VulkanClient.h"
#include "VulkanSurface.h"
#include "VulkanUtil.h"
#include "VulkanConfig.h"
#include "VulkanPad.h"

#include <string>
#include <vector>

// The rasteriser. ImGui's own vendored copy of stb_truetype, already in the
// tree at build/_deps/stb-src -- the same rasteriser the UI host uses for the
// Launchpad's text, which is why the two agree about what a face looks like.
//
// It replaces nothing that Vulkan provides and nothing that D3D9 provided:
// GDI was the rasteriser on Windows and there is no GDI here. See the header.
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

// fontconfig answers "which FILE is 'Courier New'", which is a question Win32
// never had to ask -- a LOGFONT face name IS the font on Windows. It is the
// standard Linux answer and every toolkit uses it.
#include <fontconfig/fontconfig.h>

// The #if for Visual Studio 2012's missing round() stood here. glibc has had
// round() since C99.


// ---------------------------------------------------------------------------
// CP1252 -> Unicode for the 32 code points where they differ.
//
// The Windows loop walks raw bytes 0..255 and hands each to TextOutA, which
// interprets them in the ANSI code page -- CP1252 for ANSI_CHARSET. A
// rasteriser wants Unicode code points, and CP1252 is identity everywhere
// except 0x80-0x9F, where Latin-1 has unused control codes and CP1252 has the
// smart quotes, dashes and the euro sign. Those 32 entries are the whole
// difference and they are written out rather than pulled from iconv, which
// would be a dependency for one table.
// ---------------------------------------------------------------------------
static const unsigned short kCp1252High[32] = {
	0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
	0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
	0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
	0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
};

static int AnsiToUnicode(int c)
{
	if (c >= 0x80 && c <= 0x9F) return kCp1252High[c - 0x80];
	return c;
}


// ---------------------------------------------------------------------------
// Find the file for a LOGFONT.
//
// NO WINDOWS COUNTERPART, because Win32 has none to have: CreateFont takes a
// face name and GDI owns the font database behind it. Here the client has to
// name a file before it can read a glyph out of it.
//
// fontconfig first, because it is the system's own answer and it knows two
// things a hard-coded list cannot: the user's installed fonts, and the
// METRIC-COMPATIBLE substitutions -- Liberation Mono for Courier New,
// Liberation Sans for Arial, Liberation Serif for Times New Roman -- which
// matter here beyond looks. Orbiter's MFDs lay themselves out from
// GetTextWidth, so a substitute of the wrong width moves every column.
//
// The generic names D3D9PadFont documents ('fixed', 'sans', 'serif') are
// passed through unchanged: fontconfig understands them as aliases already.
// ---------------------------------------------------------------------------
static bool ResolveFontFile(const LOGFONT &lf, char *out, size_t outLen)
{
	if (!out || outLen == 0) return false;
	out[0] = 0;

	const char *face = (lf.lfFaceName[0] != 0) ? lf.lfFaceName : "sans";

	if (FcInit()) {
		FcPattern *pat = FcNameParse((const FcChar8 *)face);
		if (pat) {
			// FC_WEIGHT is fontconfig's own scale, not GDI's 0..1000 one.
			// FW_BOLD is 700 and everything at or above it is bold; below it
			// is regular. Mapping the whole range would claim a precision
			// neither side has.
			FcPatternAddInteger(pat, FC_WEIGHT,
								(lf.lfWeight >= FW_BOLD) ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
			FcPatternAddInteger(pat, FC_SLANT,
								lf.lfItalic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);

			FcConfigSubstitute(NULL, pat, FcMatchPattern);
			FcDefaultSubstitute(pat);

			FcResult res = FcResultNoMatch;
			FcPattern *match = FcFontMatch(NULL, pat, &res);
			if (match) {
				FcChar8 *file = NULL;
				if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file) {
					strncpy(out, (const char *)file, outLen - 1);
					out[outLen - 1] = 0;
				}
				FcPatternDestroy(match);
			}
			FcPatternDestroy(pat);
		}
	}

	if (out[0]) return true;

	// The fallback, using the same files UIHost.cpp:3119 already falls back
	// to, so a machine without a fontconfig database still gets text rather
	// than an empty atlas.
	static const char *const kFallback[] = {
		"/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
		"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
		"/usr/share/fonts/truetype/wine/tahoma.ttf",
		"/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf"
	};
	for (size_t i = 0; i < ARRAYSIZE(kFallback); i++) {
		FILE *f = fopen(kFallback[i], "rb");
		if (f) {
			fclose(f);
			strncpy(out, kFallback[i], outLen - 1);
			out[outLen - 1] = 0;
			return true;
		}
	}
	return false;
}


// ----------------------------------------------------------------------------------------
//
VulkanText::VulkanText(VulkanDevice *pDevice) :
	red        (1.0),
	green      (1.0),
	blue       (1.0),
	alpha      (1.0),
	tex_w      (),
	tex_h      (),
	sharing    (),
	spacing    (0.0f),
	linespacing(),
	max_len    (),
	rotation   (),
	scaling    (1.0f),
	charset    (ANSI_CHARSET),
	first      (0),
	halign     (),
	valign     (),
	pDev       (pDevice),
	pTex       (NULL),
	FontData   (NULL)
{
	// wfont(NULL) stood in this list. See the header.
	memset(&tm, 0, sizeof(TEXTMETRIC));
	memset(&lf, 0, sizeof(LOGFONT));
	facePath[0] = 0;
}


// ----------------------------------------------------------------------------------------
//
VulkanText::~VulkanText()
{
	SAFE_DELETEA(FontData);

	// Was SAFE_RELEASE(pTex) and SAFE_RELEASE(wfont). Vulkan objects are not
	// reference counted -- the device that made the image destroys it.
	if (pTex && pDev) { pDev->DestroyTexture(pTex); pTex = NULL; }
}


// ----------------------------------------------------------------------------------------
//
void VulkanText::SetCharSet(int set)
{
	charset=set;
}


// ----------------------------------------------------------------------------------------
//
void VulkanText::SetTextSpace(float space)
{
	spacing = space;
}

// ----------------------------------------------------------------------------------------
//
void VulkanText::SetTextHAlign(int x)
{
	halign=x;
}

// ----------------------------------------------------------------------------------------
//
void VulkanText::SetTextVAlign(int x)
{
	valign=x;
}

// ----------------------------------------------------------------------------------------
//
void VulkanText::SetTextShare(int share)
{
	sharing = (tm.tmHeight*share)/100;
}


// ----------------------------------------------------------------------------------------
//
void VulkanText::SetLineSpace(int line)
{
	linespacing = tm.tmHeight + (tm.tmHeight*line)/100;
}


// ----------------------------------------------------------------------------------------
//
int VulkanText::GetLineSpace()
{
	return linespacing;
}


// ----------------------------------------------------------------------------------------
// Init -- build the glyph atlas.
//
// THE STRUCTURE IS THE WINDOWS FUNCTION'S, LINE FOR LINE: the same 2048-wide
// atlas, the same starting height of 32 doubled on overflow, the same `goto
// restart`, the same x = 5 / y = 5 + h origin, the same x += cx + 4 advance
// and the same wrap and grow tests. Those numbers decide where every glyph
// lands and therefore what every UV in FontData is, so changing any of them
// would change how text looks for no reason.
//
// WHAT IS DIFFERENT IS THE FOUR LINES IN THE MIDDLE. Where the Windows file
// had
//
//     pSurf->GetDC(&hDC); SelectObject(hDC, hFont);
//     TextOutA(hDC, x, y, text, 1);
//     GetTextExtentPoint32(hDC, text, 1, &fnts);
//
// -- borrowing GDI as a rasteriser and reading the pixels back out of the
// surface -- this rasterises the glyph itself and asks the face for the
// advance. See the header for why: Gdi.cpp is a display-list recorder and
// there are no pixels in it to read.
//
// The atlas is built in a plain CPU buffer and uploaded once at the end,
// which is what CreateTexture(D3DPOOL_SYSTEMMEM) + UpdateTexture did. It has
// to be: a device-local VkImage cannot be written by the CPU at all.
// ----------------------------------------------------------------------------------------
bool VulkanText::Init(HFONT hFont)
{
	if (hFont==NULL) {
		LogErr("NULL Font in VulkanText::Init()");
		return false;
	}

	// Receive font attributes
	if (GetObject(hFont, sizeof(LOGFONT), &lf) != sizeof(LOGFONT)) {
		LogErr("GetObject(hFont, LOGFONT) FAIL -- the font carries no description");
		return false;
	}

	tex_w = 2048;	// Texture Width
	tex_h = 32;

	// Allocate space for data
	//
	FontData = new VulkanFontData[256]();	// zero-initialized

	LogAlw("[NEW FONT] (%31s), Size=%d, Weight=%d Pitch&Family=%x", lf.lfFaceName, lf.lfHeight, lf.lfWeight, lf.lfPitchAndFamily);

	// ---------------------------------------------------------------------
	// Find and open the face. No Windows counterpart: GDI owned the font
	// database and CreateFont's face name WAS the font.
	// ---------------------------------------------------------------------
	if (!ResolveFontFile(lf, facePath, sizeof(facePath))) {
		LogErr("No font file found for face \"%s\"", lf.lfFaceName);
		return false;
	}

	std::vector<unsigned char> ttf;
	{
		FILE *f = fopen(facePath, "rb");
		if (!f) { LogErr("Cannot open font file %s", facePath); return false; }
		fseek(f, 0, SEEK_END);
		const long n = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (n <= 0) { fclose(f); LogErr("Empty font file %s", facePath); return false; }
		ttf.resize((size_t)n);
		const size_t got = fread(ttf.data(), 1, (size_t)n, f);
		fclose(f);
		if (got != (size_t)n) { LogErr("Short read on font file %s", facePath); return false; }
	}

	stbtt_fontinfo info;
	const int offset = stbtt_GetFontOffsetForIndex(ttf.data(), 0);
	if (offset < 0 || !stbtt_InitFont(&info, ttf.data(), offset)) {
		LogErr("stbtt_InitFont failed on %s", facePath);
		return false;
	}

	LogAlw("Face file .............. : %s", facePath);

	// THE TWO MEANINGS OF lfHeight, which GDI documents and which have to be
	// kept apart here because stb_truetype offers a separate call for each:
	//   height > 0  is the CELL height -- ascent + descent, the line box.
	//               stbtt_ScaleForPixelHeight scales to exactly that.
	//   height < 0  is the CHARACTER height -- the em square.
	//               stbtt_ScaleForMappingEmToPixels scales to that.
	// Getting this backwards is a systematic size error of the face's
	// line-box-to-em ratio, which for Tahoma is 20% -- the same trap
	// UIHost.cpp's kLineBoxEm note measures from the other direction.
	const float scale = (lf.lfHeight < 0)
		? stbtt_ScaleForMappingEmToPixels(&info, float(-lf.lfHeight))
		: stbtt_ScaleForPixelHeight(&info, float(lf.lfHeight ? lf.lfHeight : 12));

	// Get Text Metrics information
	//
	// Was GetTextMetrics(hDC, &tm) once, guarded by bFirst so the restart
	// loop did not repeat it. Here the metrics come from the face and not
	// from a DC, so they are computed once before the loop and the bFirst
	// flag has nothing left to guard.
	memset((void *)&tm, 0, sizeof(TEXTMETRIC));
	{
		int fa = 0, fd = 0, fg = 0;
		stbtt_GetFontVMetrics(&info, &fa, &fd, &fg);

		tm.tmAscent  = (LONG)ceilf(float(fa) * scale);
		tm.tmDescent = (LONG)ceilf(float(-fd) * scale);
		tm.tmHeight  = tm.tmAscent + tm.tmDescent;
		tm.tmInternalLeading = 0;
		tm.tmExternalLeading = (LONG)ceilf(float(fg) * scale);
		tm.tmWeight  = lf.lfWeight ? lf.lfWeight : FW_NORMAL;

		// tmItalic, tmCharSet, tmFirstChar and tmLastChar would be set here
		// on Windows, and are not, because Src/Orbiter/Linux/windows.h's
		// TEXTMETRIC does not declare them: it carries the fields the tree
		// reads -- height, ascent, descent, leading, weight and the two
		// widths -- and nothing in the client has ever read the other four.
		// Adding them to the shim to write them here and never read them
		// would be inventing API surface, so they are left out rather than
		// faked.

		// tmAveCharWidth and tmMaxCharWidth measured over the printable
		// range, which is what GDI reports and what SetTextShare and the
		// atlas's wrap test below both use.
		long total = 0, count = 0, widest = 0;
		for (int ch = 32; ch < 127; ch++) {
			int adv = 0, lsb = 0;
			stbtt_GetCodepointHMetrics(&info, ch, &adv, &lsb);
			const long w = (long)ceilf(float(adv) * scale);
			total += w; count++;
			if (w > widest) widest = w;
		}
		tm.tmAveCharWidth = count ? (LONG)(total / count) : (LONG)(tm.tmHeight / 2);
		tm.tmMaxCharWidth = (LONG)widest;
	}

	// The CPU-side atlas. One byte per pixel: a coverage value, which is what
	// the shader wants and all it ever wanted. See the header for why this is
	// R8 where Windows used R5G6B5.
	std::vector<unsigned char> atlas;

	// Draw Charters
	//
	char text[] = "c";
	(void)text;		// the Windows loop's one-character TextOut buffer

	int s = tm.tmMaxCharWidth;
	int a = tm.tmAscent + 1;
	int d = tm.tmDescent + 1;
	int h = a+d;
	int x = 5;
	int y = 5 + h;
	int c = first; // ANSI code of the First Charter
	VulkanFontData *pData;

	SIZE fnts;

restart:

	if (tex_h>=2048) {
		LogErr("^^ Font is too large for pre-rendering");
		return false;
	}

	// Was CreateTexture(SYSMEM) + GetSurfaceLevel + GetDC. One allocation,
	// cleared: a glyph is composited onto black, exactly as SetBkColor(0) and
	// SetBkMode(TRANSPARENT) arranged on Windows.
	atlas.assign((size_t)tex_w * tex_h, 0);

	// SetTextAlign(TA_BASELINE | TA_LEFT) and SetTextColor(0xFFFFFF) stood
	// here. Both are properties of the rasterisation below rather than of a
	// DC: y IS the baseline, and stb_truetype produces coverage, which is
	// white text by construction.

	x = 5;
	y = 5 + h;
	c = first;

	{
		float tw = 1.0f / float(tex_w);
		float th = 1.0f / float(tex_h);

		while ( c < 256 ) {
			pData = Data(c);

			const int cp = AnsiToUnicode(c);

			int adv = 0, lsb = 0;
			stbtt_GetCodepointHMetrics(&info, cp, &adv, &lsb);
			fnts.cx = (LONG)ceilf(float(adv) * scale);
			fnts.cy = tm.tmHeight;

			// Rasterise the glyph at the pen position, with y as the
			// baseline -- which is what TA_BASELINE | TA_LEFT meant.
			int gx0 = 0, gy0 = 0, gx1 = 0, gy1 = 0;
			stbtt_GetCodepointBitmapBox(&info, cp, scale, scale, &gx0, &gy0, &gx1, &gy1);

			const int gw = gx1 - gx0;
			const int gh = gy1 - gy0;

			if (gw > 0 && gh > 0) {
				const int px = x + gx0;
				const int py = y + gy0;		// gy0 is negative above the baseline

				// Clipped rather than assumed to fit: a face with an
				// overhanging glyph -- an italic 'f', a script capital --
				// legitimately draws outside its advance, and GDI clipped it
				// to the surface too.
				if (px >= 0 && py >= 0 && px + gw <= tex_w && py + gh <= tex_h) {
					stbtt_MakeCodepointBitmap(&info,
											  &atlas[(size_t)py * tex_w + px],
											  gw, gh, tex_w, scale, scale, cp);
				}
			}

			pData->sp  = float(fnts.cx);		// Char spacing
			pData->w   = float(fnts.cx+3);		// Char Width
			pData->h   = float(h);				// Char Height

			pData->tx0 = float(x-1);
			pData->tx1 = float(x-1 + fnts.cx+3);

			pData->ty0 = float(y - a);
			pData->ty1 = float(y + d);

			pData->tx0 *= tw;
			pData->tx1 *= tw;
			pData->ty0 *= th;
			pData->ty1 *= th;

			c++;	// Next Charter

			x += (fnts.cx + 4);		// --!!-- In order to increase spacing between charters increase this --!!--

			if ((x+s) >= tex_w) {	// Start a New Line
				x = 5;
				y+= (h+5);
			}

			if ((y+h) >= tex_h) {
				// Was ReleaseDC + two Release calls before the retry. There
				// is one buffer here and assign() below reallocates it.
				tex_h *= 2;
				goto restart;
			}
		}
	}

	// DeleteObject(hFont) stood here -- the Windows file consumed the caller's
	// font handle. That is kept: VulkanPadFont's destructor does not delete
	// hFont either, for exactly this reason.
	DeleteObject(hFont);

	// Was CreateTexture(D3DUSAGE_AUTOGENMIPMAP, D3DFMT_R5G6B5, D3DPOOL_DEFAULT)
	// followed by UpdateTexture and GenerateMipSubLevels.
	//
	// The mip chain is asked for by level count rather than by a usage flag,
	// because Vulkan generates nothing -- GenerateMipmaps below blits down the
	// chain, which is what GenerateMipSubLevels did.
	uint32_t mips = 1;
	{
		uint32_t dim = (uint32_t)((tex_w > tex_h) ? tex_w : tex_h);
		while (dim > 1) { dim >>= 1; mips++; }
	}

	pTex = pDev->CreateTexture((uint32_t)tex_w, (uint32_t)tex_h, mips,
							   VK_FORMAT_R8_UNORM,
							   VK_IMAGE_USAGE_SAMPLED_BIT |
							   VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
							   VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	if (!pTex) {
		LogErr("VulkanText: font atlas image creation failed [%d x %d]", tex_w, tex_h);
		return false;
	}

	LogAlw("Font Video Memory Usage = %u kb", tex_w*tex_h/1024);

	if (!pDev->UploadTexture(pTex, 0, 0, atlas.data(), atlas.size())) {
		LogErr("VulkanTextMgr: Surface Update Failed");
		return false;
	}

	pDev->GenerateMipmaps(pTex);

#ifdef FNTDBG
	{
		char texname[256];
		sprintf_s(texname, 256, "_%s_%d_0x%p.png", lf.lfFaceName, (int)lf.lfHeight, (void*)this);
		NatSaveSurface(texname, pTex);
	}
#endif

	// The D3DXCreateFont block stood here, building the second, WCHAR-only
	// text system. See the header: the wide path goes through this same atlas
	// now, so there is nothing to create.

	SetLineSpace(0);
	SetTextShare(0);
	SetTextSpace(0);

	LogAlw("Font and Charter set creation succesfull");

	return true;
}

// ----------------------------------------------------------------------------------------
//
VulkanFontData *VulkanText::Data (int c) {
#ifdef _DEBUG
	if (c < first) { c = first; } // <= did *never* happen, but better save than sorry
#endif
	return &FontData[c - first];
}


// ----------------------------------------------------------------------------------------
//
void VulkanText::SetColor(DWORD c)
{
	alpha = ((float)((c>>24)&0xFF)) / 255.0f;
	red   = ((float)((c>>16)&0xFF)) / 255.0f;
	green = ((float)((c>>8)&0xFF)) / 255.0f;
	blue  = ((float)(c&0xFF)) / 255.0f;
}


// ----------------------------------------------------------------------------------------
//
// The Windows definition gave `a` a default argument here, in the .cpp, where
// the header already declares the parameter without one. MSVC accepts a
// default added at the definition; the standard permits it only if no earlier
// declaration in the same scope gave one, and GCC applies that strictly to a
// member function -- the default must be on the declaration. It is dropped
// here rather than moved, because nothing in the tree calls the three-argument
// form: both call sites pass all four.
void VulkanText::SetColor(float r, float g, float b, float a)
{
	red=r; green=g; blue=b; alpha=a;
}

// ----------------------------------------------------------------------------------------
//
void VulkanText::Reset()
{
	max_len = 0;
}

// ----------------------------------------------------------------------------------------
//
float VulkanText::Width()
{
	return max_len;
}

// ----------------------------------------------------------------------------------------
//
void VulkanText::SetRotation(float deg)
{
	rotation = deg;
}

// ----------------------------------------------------------------------------------------
//
void VulkanText::SetScaling(float factor)
{
	scaling = factor;
}

// ----------------------------------------------------------------------------------------
//
int	VulkanText::GetIndex(const char *pText, float pos, int x)
{
	float del = 1e6;
	float len = 0.0f;
	int i = 0;
	int idx = 0;

	const BYTE *str = (const BYTE *)pText;

	while (i < x || x < 0) {
		if (fabs(pos - len) < del) {
			del = fabs(pos - len);
			idx = i;
		} else break;
		if (str[i] == 0) break;
		len += (Data(str[i])->sp + spacing);
		i++;
	}

	return idx;
}

// ----------------------------------------------------------------------------------------
//
float VulkanText::Length2(const char *_str, int l)
{
	float len = 0;
	int i = 0;

	const BYTE *str = (const BYTE *)_str; // Negative index may occur without this

	while ((i<l || l<=0) && str[i]) {
		// Was `if (str[i] <= 255)`. The cast on the line above is what
		// actually made this loop safe -- with a plain `const char*` a byte
		// above 0x7F is NEGATIVE and Data() would index before the array --
		// and once str is a BYTE* the test can no longer fail for any input.
		// GCC says so (-Wtype-limits: "comparison is always true due to
		// limited range of data type"). Dropping a branch that provably never
		// takes changes no behaviour; keeping it and silencing the warning
		// would only hide the next one.
		len += (Data(str[i])->sp + spacing);
		i++;
	}

	len -= spacing;
	if (len<0) len=0;
	return len * scaling;
}


// ----------------------------------------------------------------------------------------
//
float VulkanText::Length(BYTE c)
{
	return (Data(c)->sp + spacing) * scaling;
}

// ----------------------------------------------------------------------------------------
//
// Unchanged apart from the matrix type and the two D3DX calls it made.
// Everything this function does is feed four vertices and six indices per
// character straight into the pad's queue, which is arithmetic on the client's
// own arrays -- there is no device call in it at all, on either platform.
//
float VulkanText::PrintSkp(VulkanPad *pSkp, float xpos, float ypos, const char *_str, int len, bool bBox)
{

	pSkp->SetFontTextureNative(pTex);

	if (halign == 1) xpos -= Length2(_str, len) * 0.5f;
	if (halign == 2) xpos -= Length2(_str, len);
	if (valign == 1) ypos -= tm.tmAscent;
	if (valign == 2) ypos -= tm.tmHeight;

	const BYTE *str = (const BYTE *)_str;

	xpos = ceil(xpos);
	ypos = ceil(ypos);

	float x_orig = xpos;

	float h = FontData[0].h;

	float bbox_l = xpos - 2;
	float bbox_t = ypos + 1;
	float bbox_b = ypos + h - 1;
	float bbox_r = xpos + 2;

	unsigned char c = str[0];
	int idx = 1;

	while (c && (idx<=len || len<=0)) {
		bbox_r += ceil(Data(c)->sp + spacing);
		c = str[idx++];
	}

	FMATRIX4 rot, mBak;
	bool bRestore = false;

	if (fabs(rotation)>1e-3 || fabs(scaling - 1.0f)>0.001f) {
		FVECTOR2 center = FVECTOR2((bbox_l + bbox_r)*0.5f, bbox_t);
		FVECTOR2 scale = FVECTOR2(scaling, scaling);
		center.x = ceil(center.x);
		center.y = ceil(center.y);
		// Was D3DXMatrixTransformation2D. See VulkanUtil.cpp: D3DX was a
		// Direct3D utility library and Vulkan ships no counterpart, so the
		// term order is written out there once for the whole client.
		VMAT_Transformation2D(&rot, &center, 0.0f, &scale, &center, -rotation*0.01745329f, NULL);

		memcpy(&mBak, pSkp->WorldMatrix(), sizeof(FMATRIX4));
		VMAT_MatrixMultiply(pSkp->WorldMatrix(), &rot, &mBak);
		bRestore = true;
	}

	if (bBox) {
		pSkp->FillRect(int(bbox_l), int(bbox_t+2), int(bbox_r), int(bbox_b), pSkp->bkcolor);
	}

	idx = 1;
	c = str[0];

	// Feed data directly into a drawing queue
	//
	if (pSkp->Topology(VulkanPad::Topo::TRIANGLE)) {

		SkpVtx *pVtx = pSkp->Vtx;
		WORD *pIdx = pSkp->Idx;
		WORD iI = pSkp->iI;
		WORD vI = pSkp->vI;

		DWORD flags = SKPSW_FONT | SKPSW_CENTER | SKPSW_FRAGMENT;
		DWORD color = pSkp->textcolor.dclr;

		while (c && (idx <= len || len <= 0)) {

			VulkanFontData *pData = Data(c);

			pIdx[iI++] = vI;
			pIdx[iI++] = vI + 1;
			pIdx[iI++] = vI + 2;
			pIdx[iI++] = vI;
			pIdx[iI++] = vI + 2;
			pIdx[iI++] = vI + 3;

			float w = pData->w;
			float xp = ceil(xpos);
			SkpVtxFF(pVtx[vI++], xp, ypos, pData->tx0, pData->ty0);
			SkpVtxFF(pVtx[vI++], xp, ypos + h, pData->tx0, pData->ty1);
			SkpVtxFF(pVtx[vI++], xp + w, ypos + h, pData->tx1, pData->ty1);
			SkpVtxFF(pVtx[vI++], xp + w, ypos, pData->tx1, pData->ty0);

			pVtx[vI - 1].fnc = flags;
			pVtx[vI - 1].clr = color;
			pVtx[vI - 2].fnc = flags;
			pVtx[vI - 2].clr = color;
			pVtx[vI - 3].fnc = flags;
			pVtx[vI - 3].clr = color;
			pVtx[vI - 4].fnc = flags;
			pVtx[vI - 4].clr = color;

			xpos += (pData->sp + spacing);

			c = str[idx++];
		}

		pSkp->vI = vI;
		pSkp->iI = iI;
	}


	if (bRestore) {
		memcpy(pSkp->WorldMatrix(), &mBak, sizeof(FMATRIX4));
	}

	float l = xpos - x_orig;
	if (l>max_len) max_len = l;
	return l;
}

// ----------------------------------------------------------------------------------------
//
// THE WIDE-STRING PATH, and it is the one function in this file that is not a
// transcription of its Windows counterpart.
//
// The Windows version is thirty lines of ID3DXFont: DrawTextW with DT_CALCRECT
// to measure, the h/v alignment applied to the returned rectangle, a colour
// channel swap because "pSkp->textcolor.dclr is in the wrong format", and
// DrawTextW again to draw -- with a pSkp->Flush() before each call, because
// ID3DXFont draws through the device behind the pad's back and the queue must
// be empty first.
//
// ALL OF IT IS THE COST OF THE SECOND TEXT SYSTEM, and none of it survives the
// system going away. What is left is what the caller actually asked for: draw
// this string. So the wide characters are folded to the atlas's code page and
// handed to PrintSkp(const char*), which is the same rasterised face, the same
// queue, the same alignment and the same colour -- and no flush, because
// nothing is drawing behind the pad any more.
//
// WHAT IS LOST, said plainly: a code point with no CP1252 spelling. It is
// drawn as '?' rather than in the face's own glyph, where ID3DXFont would have
// drawn it. Widening the atlas beyond 256 entries is the fix and is a change
// to Init's FontData array, not to this function -- the whole class is built
// on a 256-entry table indexed by byte, including Data(), Length() and
// GetIndex().
//
float VulkanText::PrintSkp (VulkanPad *pSkp, float xpos, float ypos, LPCWSTR str, int len, bool bBox)
{
	if (!str) return 0.0f;
	if (len == -1) len = int(wcslen(str));
	if (len <= 0) return 0.0f;

	std::string narrow;
	narrow.reserve((size_t)len);

	for (int i = 0; i < len; i++) {
		const unsigned int u = (unsigned int)str[i];
		char out = '?';

		if (u < 0x80) {
			out = (char)u;
		}
		else if (u <= 0xFF && !(u >= 0x80 && u <= 0x9F)) {
			// Latin-1 and CP1252 agree from 0xA0 up.
			out = (char)u;
		}
		else {
			// The 32 code points where they differ, searched rather than
			// tabled backwards: it is 32 entries and this is not a hot path.
			for (int k = 0; k < 32; k++) {
				if (kCp1252High[k] == u) { out = (char)(0x80 + k); break; }
			}
		}
		narrow.push_back(out);
	}

	return PrintSkp(pSkp, xpos, ypos, narrow.c_str(), (int)narrow.size(), bBox);
}

// -----------------------------------------------------------------------------------------------
//
void VulkanText::VulkanTechInit(VulkanClient *_gc, VulkanDevice *pDev)
{
	Buffer = new char[512];
}

void VulkanText::GlobalExit()
{
	SAFE_DELETEA(Buffer);
}

char *		 VulkanText::Buffer = 0;
