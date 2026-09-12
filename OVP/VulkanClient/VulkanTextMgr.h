
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
// The Windows file uses GDI as a glyph rasteriser: it creates a SYSTEMMEM
// texture, takes a real DC on its surface with GetDC, calls TextOutA, and
// reads the drawn pixels back out. Src/Orbiter/Linux/Gdi.cpp cannot do that
// and is not meant to -- it is a display-list recorder, so TextOutA appends a
// DrawCmd and there are never any glyph pixels to read back. The client
// therefore rasterises the face itself with stb_truetype, the same rasteriser
// ImGui uses and already vendored in the tree.
//
// The HFONT carries only a LOGFONT, and "Courier New" is a name, not a path,
// so fontconfig resolves the name to a file: it does weight and slant
// matching and knows the metric-compatible substitutions (Liberation Mono for
// Courier New, Liberation Sans for Arial) that keep text the same size as on
// Windows. A built-in path list backs it up so a machine without fontconfig
// still gets text. Reading the LOGFONT back needed a one-function addition to
// Gdi.cpp, because the shim's GetObject answered only for bitmaps.
//
// The atlas format changes from D3DFMT_R5G6B5 to VK_FORMAT_R8_UNORM. The
// Windows shader reconstructs a coverage value from the 16-bit colour with
// max(u.r*0.7f, u.g), weighting green because it has six bits to red's five;
// the GLSL reads the single 8-bit channel directly.
//
// ID3DXFont *wfont and the PrintSkp(LPCWSTR) overload were a second,
// independent text system (D3DXCreateFont + DrawTextW) for wide strings only,
// rasterising through GDI at draw time and needing the queue flushed twice
// around every call. The wide path now converts the string to the atlas's
// code page and goes through the same glyph atlas; the declaration stays so
// its one caller, Pad::TextW, still has something to call.
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

	// The face file the atlas was rasterised from, so the log line and any
	// failure name a file the user can act on. GDI never exposed this.
	char	facePath[512];

	// Rendering pipeline configuration
	//
	static char *		Buffer;
};

#endif // !__VULKANTEXTMGR_H
