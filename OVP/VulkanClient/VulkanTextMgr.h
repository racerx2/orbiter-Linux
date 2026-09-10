
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
// CONVERTED FROM OVP/D3D9Client/D3D9TextMgr.h, read end to end (128 lines),
// against D3D9TextMgr.cpp read end to end (619 lines).
//
// THIS IS THE ONE FILE IN THE CLIENT WHERE THE WINDOWS APPROACH CANNOT BE
// CARRIED OVER, AND IT IS WORTH BEING PRECISE ABOUT WHY.
//
// The class is a glyph-atlas builder. Everything it does with the atlas
// afterwards -- the per-character D3D9FontData with its UV rectangle and
// advance, Length2's accumulation, PrintSkp feeding four vertices and six
// indices per character straight into the Sketchpad's queue -- is arithmetic,
// and converts unchanged. What does not convert is HOW THE ATLAS IS FILLED.
//
// D3D9TextMgr.cpp:151-249 does this:
//
//     pDev->CreateTexture(2048, tex_h, ..., D3DPOOL_SYSTEMMEM, &pSrcTex);
//     pSrcTex->GetSurfaceLevel(0, &pSurf);
//     pSurf->GetDC(&hDC);                  // a REAL GDI device context
//     SelectObject(hDC, hFont);
//     ... TextOutA(hDC, x, y, text, 1);    // GDI rasterises the glyph
//     pSurf->ReleaseDC(hDC);               // the pixels are in the texture
//
// -- it borrows GDI as a glyph rasteriser and reads the pixels back out of
// the surface it drew into. Src/Orbiter/Linux/Gdi.cpp CANNOT DO THAT AND IS
// NOT MEANT TO: it is a display-list RECORDER. TextOutA appends a DrawCmd to
// a list and orbiter_ReplayDC later turns that list into ImGui draw commands.
// There are no glyph pixels anywhere, at any point, to read back. GetDC()
// here returns a recording DC that is not attached to an image at all.
//
// So the client rasterises the face itself, with stb_truetype -- the same
// rasteriser ImGui uses for the very same job on the other side of this
// boundary, already vendored in the tree. Two pieces have to be found first:
//
//   WHICH FACE. The HFONT carries a LOGFONT and nothing else. GetObject
//   returns it, which is what the Windows file already does -- and needed a
//   one-function addition to Gdi.cpp, because the shim's GetObject answered
//   only for bitmaps. See the LOGFONT note in Src/Orbiter/Linux/windows.h.
//
//   WHICH FILE. "Courier New" is a name, not a path. fontconfig is the Linux
//   answer to exactly that question -- it is what every toolkit uses, it
//   understands weight and slant matching, and it knows the metric-compatible
//   substitutions (Liberation Mono for Courier New, Liberation Sans for
//   Arial) that keep the text the same size as it is on Windows. A built-in
//   path list backs it up, using the same files UIHost.cpp:3119 already
//   falls back to, so a machine without fontconfig still gets text.
//
// THE ATLAS FORMAT CHANGES, AND IT IS AN IMPROVEMENT RATHER THAN A
// COMPROMISE. The Windows atlas is D3DFMT_R5G6B5 -- white antialiased text on
// black -- and Sketchpad.fx recovers a coverage value from it with
//
//     float f = max(u.r*0.7f, u.g);        // Sketchpad.fx:236
//
// which weights green over red because in R5G6B5 green has six bits and red
// has five. That expression exists only because a 16-bit COLOUR format was
// the cheapest thing GDI would draw into; the quantity it is reconstructing
// is a single coverage number. Here the atlas is VK_FORMAT_R8_UNORM, which
// stores that number directly at eight bits, and the GLSL translation reads
//
//     float f = texture(FntS, frg.tex.zw).r;
//
// -- the same value, one channel, no reconstruction, and finer than either of
// the two channels it replaces.
//
// THE WCHAR FONT IS GONE. `ID3DXFont *wfont` and the whole PrintSkp(LPCWSTR)
// overload were D3DXCreateFont + ID3DXFont::DrawTextW: a SECOND, independent
// text system, used for wide strings only, that rasterises through GDI at
// draw time and needs the queue flushed twice around every call. Its only
// caller is D3D9Pad::TextW. Rather than build a second rasteriser for it, the
// wide path converts the string to the atlas's code page and goes through the
// same glyph atlas as everything else -- which is what PrintSkp(const char*)
// already is. The declaration stays, so TextW still has something to call.
// =================================================================================================================================

#ifndef __VULKANTEXTMGR_H
#define __VULKANTEXTMGR_H

#include <windows.h>
#include <windowsx.h>

#include <stdio.h>
#include <math.h>
#include <vulkan/vulkan.h>

#include "VulkanClient.h"
#include "VulkanTypes.h"
#include "AABBUtil.h"


class SurfNative;

// ----------------------------------------------------------------------------------------
//
// Was D3D9FontData. Six floats: the glyph's cell size and advance in pixels,
// and its rectangle in the atlas as normalised texture coordinates. Nothing
// in it is D3D-specific and nothing changes.
struct VulkanFontData {
	float w, h;		// X,Y position of the charter baseline
	float sp;
	float tx0, ty0;
	float tx1, ty1;
};


// ----------------------------------------------------------------------------------------
//
class VulkanText {

public:
	/**
	 * \brief Constructs a new text object
	 * \param pDevice Vulkan device instance pointer
	 */
	explicit VulkanText(VulkanDevice *pDevice);

	/**
	 * \brief Destroys the text object
	 */
	~VulkanText();

	static void VulkanTechInit(oapi::VulkanClient *gc, VulkanDevice *pDev);

	/**
	 * \brief Release global parameters
	 */
	static void GlobalExit();

	void		SetCharSet(int charset=ANSI_CHARSET);	// Must be set before Init

				// Init Will Create Charters from "first" (32:space) to "last" (255 ???)
	bool        Init(HFONT hFont);

	VulkanTexture *GetTexture() const { return pTex; }

	void        SetLineSpace(int percent=10);
	void		SetTextSpace(float space = 0.0f);
	void		SetTextShare(int percent=0);	// Percent of average width (default=0)

	void		SetColor(DWORD c);				// 0xAARRGGBB
	void		SetColor(float red, float green, float blue, float alpha);
	void		SetRotation(float deg);
	void		SetScaling(float factor);

	void		Reset();
	float		Width();
	int			GetLineSpace();

	float		Length2(const char *str, int len = -1);
	float		Length(BYTE c);
	int			GetIndex(const char *pText, float pos, int len = -1);

	void		SetTextHAlign(int x); // 0-left, 1=center, 2=right
	void		SetTextVAlign(int x); // 0-top, 1=base, 2=bottom

	float		PrintSkp (class VulkanPad *pSkp, float x, float y, const char *str, int len = -1, bool bBox = false);
	float		PrintSkp (class VulkanPad *pSkp, float x, float y, LPCWSTR str, int len = -1, bool bBox = false);

	void		GetVulkanTextMetrics(TEXTMETRIC *t) { memcpy(t, &tm, sizeof(TEXTMETRIC)); }

private:

	float	red, green, blue, alpha;

	int     tex_w;
	int     tex_h;
	int		sharing;
	float	spacing;
	int		linespacing;
	float	max_len;		  // If several strings are printed. This is the wide of the widest one
	float   rotation;
	float	scaling;
	int		charset;
	int     first;            ///< ANSI code of the first charter (FontData[0])
	int		halign,valign;

	VulkanFontData *Data (int c); ///< Returns FontData reference of a character

	VulkanDevice	*pDev;
	VulkanTexture	*pTex;
	VulkanFontData	*FontData;  ///< Array of font data information ( [c - first] )
	TEXTMETRIC		 tm;        ///< Font attributes
	LOGFONT          lf;        ///< Font attributes

	// ID3DXFont *wfont stood here. See the file header: it was a second,
	// independent text system for wide strings, built on D3DXCreateFont.

	// The face file the atlas was rasterised from, kept for the log line and
	// so a failure names something a user can act on. There was nothing to
	// keep on Windows -- GDI never told the caller which file it had used.
	char	facePath[512];

	// Rendering pipeline configuration
	//
	static char *		Buffer;
};

#endif // !__VULKANTEXTMGR_H
