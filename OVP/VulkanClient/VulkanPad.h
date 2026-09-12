// ===================================================
// Copyright (C) 2012-2026 Jarmo Nikkanen
// licensed under LGPL v2
// ===================================================
//
// Two things worth naming here:
//
//   LPDIRECT3DSURFACE9 and LPDIRECT3DTEXTURE9 both become VulkanTexture*,
//   because a VkImage is both; LPDIRECT3DVERTEXBUFFER9 and
//   LPDIRECT3DINDEXBUFFER9 both become VulkanBuffer*, for the same reason.
//
//   `RenderState *pRState` is gone. It was saved and restored around
//   BeginDrawing/EndDrawing so the pad could leave the device as it found it.
//   Vulkan has no device render state to save -- what D3D9 set with
//   SetRenderState is immutable pipeline state -- and the class was already
//   inert on Windows, because D3D9Frame.cpp creates the device with
//   D3DCREATE_PUREDEVICE and a pure device fails GetRenderState.
// ===================================================

#ifndef __VULKANPAD_H
#define __VULKANPAD_H

#include "OrbiterAPI.h"
#include "VulkanClient.h"
#include <vulkan/vulkan.h>
#include "VulkanTypes.h"
#include "VulkanEffect.h"
#include <memory>
#include <stack>
#include <map>
#include "DrawAPI.h"

using namespace oapi;

class VulkanText;
class SketchMesh;

extern oapi::Font *deffont;
extern oapi::Pen  *defpen;

#define SKPCHG_ALL			0xFFFF
#define SKPCHG_PEN			0x0001
#define SKPCHG_FONT			0x0004
#define SKPCHG_TEXTURE		0x0008
#define SKPCHG_CLIPCONE		0x0010
#define SKPCHG_CLIPRECT		0x0020
#define SKPCHG_TRANSFORM	0x0040
#define SKPCHG_EFFECTS		0x0080
#define SKPCHG_DEPTH		0x0100
#define SKPCHG_TOPOLOGY		0x0200
#define SKPCHG_PATTERN		0x0400


#define SKETCHPAD_NONE		0x0000
#define SKETCHPAD_GDI		0x0001
#define SKETCHPAD_VULKAN	0x0002		// was SKETCHPAD_DIRECTX, same value

// ===============================================================================================
// Feature Switches			//AARRGGBB
//
// Color source:
#define SKPSW_FRAGMENT		0x00000000	// Index 2
#define SKPSW_PENCOLOR		0x00000080	// Index 2
#define SKPSW_TEXTURE		0x000000FF	// Index 2

// Specials:
#define SKPSW_FONT			0x0000FF00	// Index 1
#define SKPSW_COLORKEY		0x00008000	// Index 1
#define SKPSW_LENGTH		0x00FF0000	// Index 0

// Vertex alignment:
#define SKPSW_CENTER		0x80000000	// Index 3
#define SKPSW_WIDEPEN_L		0x00000000	// Index 3
#define SKPSW_WIDEPEN_R		0xFF000000	// Index 3



// ===============================================================================================
// Special properties
//
#define SKP3E_GAMMA			0x00000001
#define SKP3E_NOISE			0x00000002
#define SKP3E_CMATR			0x00000004


// ===============================================================================================
//
#define nLow 17
#define nHigh 65
#define nQueueMax 2048
#define nIndexMax (nQueueMax * 3)

typedef std::shared_ptr<VulkanText> VulkanTextPtr;

struct SkpColor {

	SkpColor() {
		dclr = 0;
		fclr = FVECTOR4(0, 0, 0, 0);
	}

	// Was D3DXCOLOR(c), which unpacks 0xAARRGGBB. Written out because FVECTOR4
	// also has a DWORD constructor and it reads the DWORD as 0xAABBGGRR --
	// ABGR, not ARGB. `fclr = FVECTOR4(c)` would compile, look like a
	// simplification, and silently exchange red and blue.
	explicit SkpColor (DWORD c) {
		dclr = c;
		fclr.a = float((c >> 24) & 0xFF) / 255.0f;
		fclr.r = float((c >> 16) & 0xFF) / 255.0f;
		fclr.g = float((c >>  8) & 0xFF) / 255.0f;
		fclr.b = float( c        & 0xFF) / 255.0f;
		COLORSWAP(&fclr);
	}

	explicit SkpColor(const FVECTOR4 &c) {
		dclr = c.dword_argb();
		fclr.r = c.r; fclr.g = c.g;	fclr.b = c.b; fclr.a = c.a;
		COLORSWAP(&fclr);
	}

	DWORD dclr;
	FVECTOR4 fclr;
};



// The sketchpad vertex. 36 bytes is the stride VulkanUtil.cpp's
// VDECL(SketchpadDecl, 36) declares.
struct SkpVtx {

	SkpVtx() {
		memset(this, 0, sizeof(SkpVtx));
	};

	float x, y, l;			// vertex x, y, length
	float nx, ny;			// next point
	float px, py;			// previous point
	DWORD clr, fnc;
};

static_assert(sizeof(SkpVtx) == 36, "SkpVtx must match VDECL(SketchpadDecl, 36)");

// GradientFillRect [only]
inline void SkpVtxGF(SkpVtx &v, int _x, int _y, DWORD c)
{
	v.x = float(_x) - 0.5f;
	v.y = float(_y) - 0.5f;
	v.clr = c;
	v.fnc = SKPSW_CENTER | SKPSW_FRAGMENT;
	v.l = 0.0f;
}

inline void SkpVtxFC(SkpVtx& v, float _x, float _y, DWORD c)
{
	v.x = _x - 0.5f;
	v.y = _y - 0.5f;
	v.clr = c;
	v.fnc = SKPSW_CENTER | SKPSW_FRAGMENT;
	v.l = 0.0f;
}

// Fill Rect, Ellipse, Polygon [only]
inline void SkpVtxIC(SkpVtx &v, int _x, int _y, const SkpColor &c)
{
	v.x = float(_x) - 0.5f;
	v.y = float(_y) - 0.5f;
	v.clr = c.dclr;
	v.fnc = SKPSW_CENTER | SKPSW_FRAGMENT;
	v.l = 0.0f;
}

// Copy, Stretch, Colorkey Rect [only]
inline void SkpVtxII(SkpVtx &v, int _tx, int _ty, int _sx, int _sy)
{
	v.x = float(_tx) - 0.5f;
	v.y = float(_ty) - 0.5f;
	v.nx = float(_sx);
	v.ny = float(_sy);
	v.l = 0.0f;
};

// Pattern Fill [only]
inline void SkpVtxPF(SkpVtx &v, int _x, int _y, DWORD c)
{
	v.x = float(_x) - 0.5f;
	v.y = float(_y) - 0.5f;
	v.l = 0.0f;
	v.clr = c;
	v.fnc = SKPSW_FRAGMENT | SKPSW_CENTER;
};


// Rotate Rect [only]
inline void SkpVtxFI(SkpVtx &v, float _x, float _y, int _tx, int _ty)
{
	v.x = _x - 0.5f;
	v.y = _y - 0.5f;
	v.nx = float(_tx);
	v.ny = float(_ty);
	v.l = 0.0f;
};

// TextManager Print Font [only]
inline void SkpVtxFF(SkpVtx &v, float _x, float _y, float _tx, float _ty)
{
	v.x = _x - 0.5f;
	v.y = _y - 0.5f;
	v.nx = _tx;
	v.ny = _ty;
	v.l = 0.0f;
};

template <typename Type> int CheckTriangle(short x, const Type *pt, const WORD *Idx, float hd, short npt, bool bSharp);
template <typename Type> int CreatePolyIndexList(const Type *pt, short npt, WORD *Out);


// ===============================================================================================
// The three vector helpers the wide-line code uses. _FV2 was _DXV2; the other
// two name operations the Windows code spells with operators on D3DXVECTOR2 --
// `pt[0] * 2.0 - pt[1]` and `D3DXVec2Length(ptr(np - pp))` -- which FVECTOR2
// cannot spell the same way, its operator* against a double literal being
// ambiguous under ISO rules where D3DXVECTOR2's was not. Same arithmetic.
//
// In the header because both VulkanPad.cpp's AppendLineVertexList and
// VulkanPad2.cpp's VulkanPolyLine::Update need them, and duplicating an
// extrapolation is how two line renderers end up disagreeing about end caps.
// ===============================================================================================

inline FVECTOR2 _FV2(const IVECTOR2 &pt)
{
	return FVECTOR2(float(pt.x), float(pt.y));
}

inline FVECTOR2 _FV2(const FVECTOR2 &pt)
{
	return FVECTOR2(pt.x, pt.y);
}

/// \brief One step backwards past `a`, away from `b`. Gives the first segment
///        of an open polyline a direction it would otherwise not have.
inline FVECTOR2 _FV2Extrapolate(const FVECTOR2 &a, const FVECTOR2 &b)
{
	return FVECTOR2(a.x * 2.0f - b.x, a.y * 2.0f - b.y);
}

inline float _FV2Length(const FVECTOR2 &a, const FVECTOR2 &b)
{
	const float dx = a.x - b.x, dy = a.y - b.y;
	return sqrt(dx*dx + dy*dy);
}


/**
 * \brief The VulkanPad class defines the context for 2-D drawing using
 *  Vulkan calls.
 */
class VulkanPad : public Sketchpad
{
	friend class VulkanText;

	mutable struct {
		DWORD style;
		DWORD color;
		float width;
		bool  bEnabled;
	} QPen;

	mutable struct {
		bool  bEnabled;
	} QBrush;

	struct {
		FVECTOR3 uDir;
		float ca, dst;
		bool bEnable;
	} ClipData[2];

	enum Topo { NONE, TRIANGLE, LINE };

public:
	/**
	 * \brief Constructs a drawing object for a given surface.
	 * \param s surface handle
	 */
	explicit VulkanPad(SURFHANDLE s, const char *name = NULL);
	explicit VulkanPad(const char *name = NULL);

	/**
	 * \brief Destructor. Destroys a drawing object.
	 */
	~VulkanPad();

	/**
	 * \brief Set up global parameters shared by all instances
	 * \param gc client instance pointer
	 * \param pDev Vulkan device instance pointer
	 */
	static void VulkanTechInit(VulkanClient *gc, VulkanDevice *pDev);
	static void SinCos(int n, int i);
	/**
	 * \brief Release global parameters
	 */
	static void GlobalExit();

	/**
	 * \brief Selects a new font to use.
	 * \param font pointer to font resource
	 * \return Previously selected font.
	 * \default None, returns NULL.
	 * \sa oapi::Font, oapi::GraphicsClient::clbkCreateFont
	 */
	oapi::Font  *SetFont (oapi::Font *font);

	/**
	 * \brief Selects a new pen to use.
	 * \param pen pointer to pen resource, or NULL to disable outlines
	 * \return Previously selected pen.
	 * \default None, returns NULL.
	 * \sa oapi::Pen, oapi::GraphicsClient::clbkCreatePen
	 */
	oapi::Pen   *SetPen (oapi::Pen *pen);

	/**
	 * \brief Selects a new brush to use.
	 * \param brush pointer to brush resource, or NULL to disable fill mode
	 * \return Previously selected brush.
	 * \default None, returns NULL.
	 * \sa oapi::Brush, oapi::GraphicsClient::clbkCreateBrush
	 */
	oapi::Brush *SetBrush (oapi::Brush *brush);

	/**
	 * \brief Set horizontal and vertical text alignment.
	 * \param tah horizontal alignment
	 * \param tav vertical alignment
	 * \default None.
	 */
	void SetTextAlign(TAlign_horizontal tah=LEFT, TAlign_vertical tav=TOP);

	/**
	 * \brief Set the foreground colour for text output.
	 * \param col colour description (format: 0xBBGGRR)
	 * \return Previous colour setting.
	 * \default None, returns 0.
	 */
	DWORD SetTextColor (DWORD col);

	/**
	 * \brief Set the background colour for text output.
	 * \param col background colour description (format: 0xBBGGRR)
	 * \return Previous colour setting
	 * \default None, returns 0.
	 * \note The background colour is only used if the background mode
	 *   is set to BK_OPAQUE.
	 * \sa SetBackgroundMode
	 */
	DWORD SetBackgroundColor (DWORD col);

	/**
	 * \brief Set the background mode for text output.
	 * \param mode background mode (see \ref BkgMode)
	 * \default None.
	 * \note In opaque background mode, the text background is drawn
	 *   in the current background colour (see SetBackgroundColor).
	 * \note The default background mode (before the first call of
	 *   SetBackgroundMode) should be transparent.
	 * \sa SetBackgroundColor, SetTextColor
	 */
	void  SetBackgroundMode (BkgMode mode);

	/**
	 * \brief Return height and (average) width of a character in the currently
	 *   selected font.
	 * \return Height of character cell [pixel] in the lower 16 bit of the return value,
	 *   and (average) width of character cell [pixel] in the upper 16 bit.
	 * \default None, returns 0.
	 * \note The height value should describe the height of the character cell (i.e.
	 *   the smallest box circumscribing all characters in the font), but without any
	 *   "internal leading", i.e. the gap between characters in two consecutive lines.
	 * \note For proportional fonts, the width value should be an approximate average
	 *   character width.
	 */
	DWORD GetCharSize ();

	/**
	 * \brief Return the width of a text string in the currently selected font.
	 * \param str text string
	 * \param len string length, or 0 for auto (0-terminated string)
	 * \return width of the string, drawn in the currently selected font [pixel]
	 * \default None, returns 0.
	 * \sa SetFont
	 */
	DWORD GetTextWidth (const char *str, int len = 0);

	/**
	 * \brief Move the drawing reference to a new point.
	 * \param x x-coordinate of new reference point [pixel]
	 * \param y y-coordinate of new reference point [pixel]
	 * \note Some methods use the drawing reference point for
	 *   drawing operations, e.g. \ref LineTo.
	 * \default None.
	 * \sa LineTo
	 */
	void MoveTo (int x, int y);

	/**
	 * \brief Draw a line to a specified point.
	 * \param x x-coordinate of line end point [pixel]
	 * \param y y-coordinate of line end point [pixel]
	 * \default None.
	 * \note The line starts at the current drawing reference
	 *   point.
	 * \sa MoveTo
	 */
	void LineTo (int x, int y);

	/**
	 * \brief Set the position in the surface bitmap which is mapped to the
	 *   origin of the coordinate system for all drawing functions.
	 * \param x horizontal position of the origin [pixel]
	 * \param y vertical position of the origin [pixel]
	 * \default None.
	 * \note By default, the reference point for drawing function coordinates is
	 *   the top left corner of the bitmap, with positive x-axis to the right,
	 *   and positive y-axis down.
	 * \note SetOrigin can be used to shift the logical reference point to a
	 *   different position in the surface bitmap (but not to change the
	 *   orientation of the axes).
	 * \sa GetOrigin
	 */
	void SetOrigin (int x, int y);

	/**
	 * \brief Returns the position in the surface bitmap which is mapped to
	 *   the origin of the coordinate system for all drawing functions.
	 * \param [out] x pointer to integer receiving horizontal position of the origin [pixel]
	 * \param [out] y pointer to integer receiving vertical position of the origin [pixel]
	 * \default Returns (0,0)
	 * \sa SetOrigin
	 */
	void GetOrigin (int *x, int *y) const;

	/**
	 * \brief Draw a text string.
	 * \param x reference x position [pixel]
	 * \param y reference y position [pixel]
	 * \param str text string
	 * \param len string length for output
	 * \return \e true on success, \e false on failure.
	 * \default None, returns false.
	 */
	bool Text (int x, int y, const char *str, int len);

	/**
	 * \brief Draw a text string into a rectangle.
	 * \param x1 left edge [pixel]
	 * \param y1 top edge [pixel]
	 * \param x2 right edge [pixel]
	 * \param y2 bottom edge [pixel]
	 * \param str text string
	 * \param len string length for output
	 * \return \e true on success, \e false on failure.
	 */
	bool TextBox (int x1, int y1, int x2, int y2, const char *str, int len);

	/**
	 * \brief Draw a single pixel in a specified colour.
	 * \param x x-coordinate of point [pixel]
	 * \param y y-coordinate of point [pixel]
	 * \param col pixel colour (format: 0xBBGGRR)
	 */
	void Pixel (int x, int y, DWORD col);

	/**
	 * \brief Draw a line between two points.
	 * \param x0 x-coordinate of first point [pixel]
	 * \param y0 y-coordinate of first point [pixel]
	 * \param x1 x-coordinate of second point [pixel]
	 * \param y1 y-coordinate of second point [pixel]
	 * \default None.
	 * \note The line is drawn with the currently selected pen.
	 * \sa SetPen
	 */
	void Line (int x0, int y0, int x1, int y1);

	/**
	 * \brief Draw a rectangle (filled or outline).
	 * \param x0 left edge of rectangle [pixel]
	 * \param y0 top edge of rectangle [pixel]
	 * \param x1 right edge of rectangle [pixel]
	 * \param y1 bottom edge of rectangle [pixel]
	 * \default Draws the rectangle from 4 line segments and
	 *   fills the rectangle with the currently selected brush resource.
	 * \sa MoveTo, LineTo, Ellipse, Polygon
	 */
	void Rectangle (int x0, int y0, int x1, int y1);

	/**
	 * \brief Draw an ellipse from its bounding box.
	 * \param x0 left edge of bounding box [pixel]
	 * \param y0 top edge of bounding box [pixel]
	 * \param x1 right edge of bounding box [pixel]
	 * \param y1 bottom edge of bounding box [pixel]
	 * \default None.
	 * \note The ellipse is filled with the currently selected
	 *   brush resource.
	 * \sa Rectangle, Polygon
	 */
	void Ellipse (int x0, int y0, int x1, int y1);

	/**
	 * \brief Draw a closed polygon given by vertex points.
	 * \param pt list of vertex points
	 * \param npt number of points in the list
	 * \default None.
	 * \note The polygon should be closed, i.e. the last point
	 *   joined with the first one.
	 * \note The outline of the polygon is drawn with the
	 *   current pen, and filled with the current brush.
	 * \note Filled polygon has a maximum of 64 points.
	 * \sa Polyline, PolyPolygon, Rectangle, Ellipse
	 */
	void Polygon (const oapi::IVECTOR2 *pt, int npt);

	/**
	 * \brief Draw a line of piecewise straight segments.
	 * \param pt list of vertex points
	 * \param npt number of points in the list
	 * \default None
	 * \note The line is drawn with the currently selected pen.
	 * \note Polylines are open figures: the end points are
	 *   not connected, and no fill operation is performed.
	 * \sa Polygon, PolyPolyline, Rectangle, Ellipse
	 */
	void Polyline (const oapi::IVECTOR2 *pt, int npt);

	/**
	 * \brief Obsolete. Will return NULL
	 * \return null
	 */
	HDC GetDC();

	bool TextW(int x, int y, const LPWSTR str, int len = -1);

	// ===============================================================================
	// Sketchpad2 Additions
	// ===============================================================================

	void GetRenderSurfaceSize(LPSIZE size);
	void QuickPen(DWORD color, float width = 1.0f, DWORD style = 0);
	void QuickBrush(DWORD color);
	void SetGlobalLineScale(float width = 1.0f, float pattern = 1.0f);
	void SetWorldTransform(const FMATRIX4 *pWT = NULL);
	void SetWorldTransform2D(float scale=1.0f, float rot=0.0f, const IVECTOR2 *c=NULL, const IVECTOR2 *t=NULL);
	int  DrawMeshGroup(const MESHHANDLE hMesh, DWORD grp, Sketchpad::MeshFlags flags, const SURFHANDLE hTex = NULL);
	void CopyRect(const SURFHANDLE hSrc, const LPRECT src, int tx, int ty);
	void StretchRect(const SURFHANDLE hSrc, const LPRECT src, const LPRECT tgt);
	void RotateRect(const SURFHANDLE hSrc, const LPRECT src, int cx, int cy, float angle, float sw = 1.0f, float sh = 1.0f);
	void ColorKey(const SURFHANDLE hSrc, const LPRECT src, int tx, int ty);
	void TextEx(float x, float y, const char *str, float scale = 100.0f, float angle = 0.0f);
	void ClipRect(const LPRECT clip = NULL);
	void Clipper(int idx, const VECTOR3 *pPos = NULL, double cos_angle = 0.0, double dist = 0.0);
	void DrawPoly(const HPOLY hPoly, DWORD flags = 0);
	void Lines(const FVECTOR2 *pt1, int nlines);
	void DepthEnable(bool bEnable);

	void SetViewMode(SkpView mode = ORTHO);
	//-----------------------------------------
	const FMATRIX4 *ViewMatrix() const;
	const FMATRIX4 *ProjectionMatrix() const;
	const FMATRIX4 *GetViewProjectionMatrix() const;
	void SetViewMatrix(const FMATRIX4 *pV = NULL);
	void SetProjectionMatrix(const FMATRIX4 *pP = NULL);




	// ===============================================================================
	// Sketchpad3 Additions
	// ===============================================================================

	const FMATRIX4 *GetColorMatrix();
	void SetColorMatrix(const FMATRIX4 *pMatrix = NULL);
	void SetBrightness(const FVECTOR4 *pBrightness = NULL);
	FVECTOR4 GetRenderParam(RenderParam param);
	void SetRenderParam(RenderParam param, const FVECTOR4 *data = NULL);
	void SetBlendState(BlendState dwState);
	FMATRIX4 GetWorldTransform() const;
	void PushWorldTransform();
	void PopWorldTransform();
	void SetWorldScaleTransform2D(const FVECTOR2 *scl = NULL, const IVECTOR2 *trl = NULL);
	void GradientFillRect(const LPRECT rect, DWORD c1, DWORD c2, bool bVertical = false);
	void ColorFill(DWORD color, const LPRECT tgt);
	void StretchRegion(const skpRegion *rgn, const SURFHANDLE hSrc, const LPRECT out);
	void CopyTetragon(SURFHANDLE pSrc, const LPRECT _s, const FVECTOR2 pt[4]);
	void ColorCompatibility(bool bEnable);
	void FillTetragon(DWORD c, const FVECTOR2 pt[4]);
	void Clear(DWORD color = 0, bool bColor = true, bool bDepth = true);
	void SetClipDistance(float _near, float _far);
	void ColorKeyStretch(const SURFHANDLE hSrc, const LPRECT _s = NULL, const LPRECT t = NULL);
	void SetWorldBillboard(const FVECTOR3& wpos, float utp = 1.0f, bool bFixed = true, const FVECTOR3* index = NULL);


	// ===============================================================================
	// VulkanClient Privates
	// ===============================================================================

	/**
	*  BeginDrawing(), EndDrawing().
	*  - All the other VulkanPad functions can be only called/used between the BeginDrawing(), EndDrawing() pairs.
	*  - All rendering that is NOT a part of VulkanPad is prohibited between the Begin/End pairs.
	*  - Calling Begin/End doesn't alter the VulkanPad state such as Pens, Brushes, etc...
	*  - EndDrawing() will flush all pending instructions from the draw queue.
	*/
	void EndDrawing();
	void BeginDrawing(VulkanTexture *pRenderTgt, VulkanTexture *pDepthStensil = NULL);
	void BeginDrawing();

	inline void FlushAll() { Flush(NULL); }

	void SetViewProj(const FMATRIX4* pV, const FMATRIX4* pP);

	FMATRIX4 *WorldMatrix();
	DWORD GetLineHeight(); ///< Return height of a character in the currently selected font with "internal leading"
	const char *GetName() const { return name; }
	VulkanTexture *GetRenderTarget() const { return pTgt; }
	bool IsStillDrawing() const { return bBeginDraw; }
	void LoadDefaults();

	// The Rect / RectNative pair existed because of the D3D9 texture/surface
	// split. That split is gone, but the SURFHANDLE / raw-resource one is not,
	// and that is what the names now mark.
	void CopyRectNative(VulkanTexture *pSrc, const LPRECT s, int tx, int ty);
	void StretchRectNative(VulkanTexture *pSrc, const RECT *s, const RECT *t);


private:

	bool Topology(Topo tRequest);
	void SetEnable(DWORD config);
	void ClearEnable(DWORD config);

	bool HasPen() const;
	bool HasBrush() const;
	bool IsDashed() const;
	bool IsAlphaTarget() const;

	float GetPenWidth() const;

	void Reset();
	bool Flush(HPOLY hPoly = NULL);
	void AddRectIdx(WORD aV);
	void FillRect(int l, int t, int r, int b, const SkpColor &c);
	void TexChange(SURFHANDLE hNew);
	bool TexChangeNative(VulkanTexture *hNew);
	RECT GetFullRectNative(VulkanTexture *hSrc);
	void SetFontTextureNative(VulkanTexture *hNew);
	void SetupDevice(Topo tNew);
	RECT GetFullRect(SURFHANDLE hSrc);
	void IsLineTopologyAllowed();
	DWORD ColorComp(DWORD c) const;
	SkpColor ColorComp(const SkpColor &c) const;

	template <typename Type> void AppendLineVertexList(const Type *pt, int npt, bool bLoop);
	template <typename Type> void AppendLineVertexList(const Type *pt);

	oapi::Font  *cfont;  ///< currently selected font (NULL if none)
	oapi::Pen   *cpen;   ///< currently selected pen (NULL if none)
	oapi::Brush *cbrush; ///< currently selected brush (NULL if none)

	SkpColor pencolor;
	SkpColor brushcolor;
	DWORD	 Change;
	bool	 bLine;

	SkpColor		 textcolor;
	SkpColor		 bkcolor;

	bool			 bColorComp;
	bool			 bColorKey;
	bool			 bEnableScissor;
	bool			 bDepthEnable;
	bool			 bMustEndScene;
	bool			 bBeginDraw;
	RECT			 ScissorRect;
	FVECTOR4		 cColorKey;
	SkpView			 vmode;
	Topo			 tCurrent;

	// RenderState *pRState stood here; see the file header.

	WORD vI = 0, iI = 0;
public:
	/// \brief Vertices this pad has actually flushed since BeginDrawing.
	///        Public so clbkReleaseSketchpad can report it: it separates "the
	///        core drew nothing into this surface" from "it drew and the
	///        result was lost".
	UINT nFlushedVtx = 0;
private:
	mutable FMATRIX4 mVP;
	FMATRIX4 mV, mP, mW, mO;
	FMATRIX4 mVOrig, mPOrig;
	FVECTOR4 vTarget;
	DWORD bkmode;
	BlendState dwBlendState;
	TAlign_horizontal tah;
	TAlign_vertical tav;
	float linescale, pattern;
	float zfar;
	int cx, cy;
	RECT tgt;

	VulkanImageDesc	 tgt_desc;
	VulkanTexture   *pTgt;
	VulkanTexture   *pDep;
	VulkanTexture   *hTexture;
	VulkanTexture   *hFontTex;



	// Sketchpad3 --------------------------------------------------------------
	DWORD RenderConfig;
	DWORD Enable;
	FMATRIX4 ColorMatrix;
	FVECTOR4 Gamma, Noise;

	std::stack<FMATRIX4> mWStack;



	// -------------------------------------------------------------------------
	bool  _isSaveBuffer;   ///< Flag indicating that the 'save buffer' can be used
	char* _saveBuffer;     ///< 'Save' string buffer  (null-terminated @ len)
	int   _saveBufferSize; ///< Current size of the 'save' string buffer

	void        ToSaveBuffer (const char *str, int len);        ///< Store len sized string into internal 'save' buffer
	inline void ReleaseSaveBuffer ();                           ///< Mark the buffer as "not save"
	void        WrapOneLine (char* str, int len, int maxWidth); ///< Wraps one text line at maxLenght pixels
	// -------------------------------------------------------------------------
	char name[32];

	void Log(const char *format, ...) const;
	static FILE *log;
	static CRITICAL_SECTION LogCrit;
	static std::map< MESHHANDLE, class SketchMesh*> MeshMap;
	static WORD *Idx;				// List of indices
	static SkpVtx *Vtx;		// List of vertices
	static VulkanClient *gc;
	static VulkanDevice *pDev;
	static FVECTOR2 *pSinCos[5];
	static VulkanTexture *pNoise;
	// -------------------------------------------


	// Rendering pipeline configuration. Applies to every instance of this class
	//
	static VulkanEffectFile	*FX;

	// TECHHANDLE, not HANDLE: these two select a pipeline where every handle
	// below them names a parameter. Two types so the one cannot be passed
	// where the other is wanted.
	static TECHHANDLE	eDrawMesh;
	static TECHHANDLE	eSketch;

	static HANDLE	eWVP;			// Transformation matrix
	static HANDLE	eTex0;
	static HANDLE	eFnt0;
	static HANDLE	eNoiseTex;
	static HANDLE   eNoiseColor;
	static HANDLE   eColorMatrix;
	static HANDLE   eGamma;
	static HANDLE   eW;
	static HANDLE   ePen;
	static HANDLE   eVP;
	static HANDLE   eFov;
	static HANDLE   eRandom;
	static HANDLE   eTarget;
	static HANDLE   eKey;
	static HANDLE   eDashEn;
	static HANDLE   eTexEn;
	static HANDLE   eKeyEn;
	static HANDLE   eFntEn;
	static HANDLE   eWidth;
	static HANDLE   eWide;
	static HANDLE   eSize;
	static HANDLE   eMtrl;
	static HANDLE   eShade;
	static HANDLE   ePos;
	static HANDLE   ePos2;
	static HANDLE   eCov;
	static HANDLE   eCovEn;
	static HANDLE   eClearEn;
	static HANDLE   eEffectsEn;
};





#define SKP_FONT_CRISP		0x1
#define SKP_FONT_ANTIALIAS	0x2
#define SKP_FONT_CLEARTYPE	0x3
#define SKP_FONT_CP_GREEK	0x10



class VulkanPadFont: public oapi::Font {

	friend class VulkanPad;
	friend class GDIPad;

public:

	static void VulkanTechInit(VulkanDevice *pDev);

	/**
	 * \brief Font constructor.
	 * \param height cell or character height [pixel]
	 * \param prop proportional/fixed width flag
	 * \param face font face name
	 * \param style font decoration
	 * \param orientation text orientation [1/10 deg]
	 * \note if \e height > 0, it represents the font cell height. if height < 0,
	 *   its absolute value represents the character height.
	 * \note The \e style parameter can be any combination of the \ref Style
	 *   enumeration items.
	 * \note The following face names are currently recognised: 'Courier New',
	 *   'Arial' and 'Times New Roman'. The generic names 'fixed', 'sans' and
	 *   'serif' are mapped to those specific type names, respectively.
	 * \note if the specified face name is not recognised, then 'sans' is
	 *   selected for \e prop==true, and 'fixed' is selected for \e prop==false.
	 */
	VulkanPadFont (int height, bool prop, const char *face, FontStyle style = FontStyle::FONT_NORMAL, int orientation=0, DWORD flags=0);
	VulkanPadFont (int height, char *face, int width = 0, int weight = 400, FontStyle style = FontStyle::FONT_NORMAL, float spacing = 0.0f);
	/**
	 * \brief Font destructor.
	 */
	~VulkanPadFont ();

	// HFONT here is the shim's own handle type, not a GDI one: its CreateFontA
	// records a face, a height and a weight. VulkanTextMgr rasterises the
	// glyphs into a texture atlas rather than asking USER32 for pixels.
	HFONT	GetGDIFont () const;
	DWORD	GetQuality() const { return Quality; }
	int		GetTextLength(const char *pText, int len) const;
	int		GetIndexByPosition(const char *pText, int pos, int len) const;

private:
	VulkanTextPtr pFont;
	HFONT hFont;
	DWORD Quality;
	float rotation;
	static VulkanDevice *pDev;
};




class VulkanPadPen: public oapi::Pen {

	friend class VulkanPad;
	friend class GDIPad;

public:
	static void VulkanTechInit(VulkanDevice *pDev);

	/**
	 * \brief Pen constructor.
	 * \param style line style (0=invisible, 1=solid, 2=dashed)
	 * \param width line width [pixel]
	 * \param col line colour (format: 0xAABBGGRR)
	 * \note if \e width=0, the pen is drawn with a width of 1 pixel.
	 * \note Dashed line styles are only valid if the width parameter is <= 1.
	 */
	VulkanPadPen (int style, int width, DWORD col);

	/**
	 * \brief Pen destructor.
	 */
	~VulkanPadPen ();

private:
	int style;
	int width;
	SkpColor clr;
	HPEN hPen;
};





class VulkanPadBrush: public oapi::Brush {
	friend class VulkanPad;
	friend class GDIPad;

public:
	static void VulkanTechInit(VulkanDevice *pDev);

	/**
	 * \brief Brush constructor.
	 * \param col line colour (format: 0xAABBGGRR)
	 * \Only solid GDI brushes are supported.
	 */
	explicit VulkanPadBrush (DWORD col);

	/**
	 * \brief Brush destructor.
	 */
	~VulkanPadBrush ();

private:
	SkpColor clr;
	HBRUSH hBrush;
};



class VulkanPolyBase
{
	DWORD alloc_id;
public:
	// Was alloc_id('POLY'). A multi-character literal is
	// implementation-defined, which is what -Wmultichar warns about; the number
	// keeps the tag byte-for-byte identical to the Windows build's.
	static const DWORD ALLOC_ID_POLY = 0x504F4C59;	// 'P','O','L','Y'

					VulkanPolyBase(int _type) : alloc_id(ALLOC_ID_POLY), pOwnerDev(NULL) { type = _type; version = 1; }
	virtual			~VulkanPolyBase() { }
	virtual	void	Draw(VulkanPad*, VulkanDevice *pDev) = 0;
	virtual void	Release() = 0;

	/// \brief The primitive topology this object draws with.
	///
	///        On Windows each subclass passed its own D3DPRIMITIVETYPE to
	///        DrawPrimitive at draw time. Vulkan bakes the topology into the
	///        pipeline, which VulkanPad::Flush binds before Draw() is reached,
	///        so Flush has to be able to ask. Without it every poly object
	///        would be drawn as a triangle list and a fan would come out as
	///        garbage.
	virtual VkPrimitiveTopology Topology() const { return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; }

	int version;
	int type;

protected:
	/// \brief The device the subclass's buffers were allocated from.
	///
	///        Windows Release() is SAFE_RELEASE(pVB), a COM reference count,
	///        and never needs to know which device made the buffer. Vulkan has
	///        no reference counting, and vkDestroyBuffer must be given the
	///        VkDevice vkCreateBuffer was called on. Both constructors are
	///        already handed that device, so they keep it. VulkanPad's static
	///        pDev would be a lie about ownership -- a poly object outlives any
	///        one Sketchpad.
	VulkanDevice *pOwnerDev;
};




class VulkanPolyLine : public VulkanPolyBase
{

public:
	VulkanPolyLine(VulkanDevice *pDev, const FVECTOR2 *pt, int npt, bool bConnect);
	~VulkanPolyLine();

	void Update(const FVECTOR2 *pt, int npt, bool bConnect);
	void Draw(VulkanPad*, VulkanDevice *pDev);
	void Release();

private:
	bool bLoop;
	WORD nVtx, nPt, nIdx, iI, vI;
	VulkanBuffer *pVB; ///< (Local) Vertex buffer pointer
	VulkanBuffer *pIB;
};



class VulkanTriangle : public VulkanPolyBase
{

public:
	VulkanTriangle(VulkanDevice *pDev, const gcCore::clrVtx *pt, int npt, int style);
	~VulkanTriangle();

	void Update(const gcCore::clrVtx *pt, int npt);
	void Draw(VulkanPad*, VulkanDevice *pDev);
	void Release();

	/// \brief PF_TRIANGLES, PF_FAN or PF_STRIP -- the three D3DPRIMITIVETYPEs
	///        this class draws with. See VulkanPolyBase::Topology.
	VkPrimitiveTopology Topology() const;

private:
	int style;
	WORD nPt;
	VulkanBuffer *pVB;
};

#endif
