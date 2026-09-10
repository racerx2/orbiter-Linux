
// ===================================================
// Copyright (C) 2012-2026 Jarmo Nikkanen
// licensed under LGPL v2
// ===================================================
//
// CONVERTED FROM OVP/D3D9Client/D3D9Pad2.cpp, read end to end (1031 lines).
//
// THE SKETCHPAD2 ADDITIONS. Two thirds of this file is vertex-writing --
// CopyRect, StretchRect, RotateRect, ColorKey, CopyTetragon, FillTetragon and
// their Native variants all fill four (or thirty-six) SkpVtx and hand them to
// the queue -- and every line of that converts unchanged. What changes:
//
//   The five D3DX matrix calls: D3DXMatrixAffineTransformation2D,
//   D3DXMatrixMultiply, D3DXMatrixIdentity and two D3DXVECTOR2 constructions.
//   D3DX is a Direct3D UTILITY library with no Vulkan counterpart, so these
//   go through VMAT_ -- see VulkanUtil.cpp, where the term order of
//   D3DXMatrixTransformation2D is transcribed from its documentation once.
//
//   The three casts through (const FMATRIX4*) in ViewMatrix,
//   ProjectionMatrix and GetViewProjectionMatrix. Those existed because the
//   members were D3DXMATRIX and the Sketchpad interface returns FMATRIX4; the
//   members ARE FMATRIX4 now, so the casts are gone rather than converted.
//   WorldMatrix's (LPD3DXMATRIX) cast goes the same way.
//
//   DrawMeshGroup's device calls: SetRenderState(D3DRS_CULLMODE) becomes a
//   PassOverride, and FX->CommitChanges() -- which existed because D3DX
//   buffered parameter writes -- becomes closing and reopening the pass, for
//   the same reason RenderReEntry does in VulkanEffect.cpp.
//
//   VulkanPolyLine and VulkanTriangle: their vertex and index buffers, and
//   their draws. THE TOPOLOGY IS THE ONE STRUCTURAL CHANGE -- see
//   VulkanPolyBase::Topology in VulkanPad.h. VulkanTriangle picks a
//   D3DPRIMITIVETYPE at draw time on Windows; Vulkan bakes it into the
//   pipeline, which VulkanPad::Flush binds before Draw() is reached.
// ===================================================

#include "VulkanPad.h"
#include "VulkanTextMgr.h"
#include "VulkanSurface.h"
#include "VulkanUtil.h"
#include "Scene.h"
#include "Mesh.h"
#include <sstream>
#include <cstdlib>




// ===============================================================================================
// Sketchpad2 Interface
// ===============================================================================================

// ===============================================================================================
//
void VulkanPad::GetRenderSurfaceSize(LPSIZE size)
{
	size->cx = tgt_desc.Width;
	size->cy = tgt_desc.Height;
}


// ===============================================================================================
//
void VulkanPad::QuickPen(DWORD color, float width, DWORD style)
{
#ifdef SKPDBG
	Log("QuickPen(0x%X, %f, %u)", color, width, style);
#endif

	if (QPen.bEnabled) {
		if ((QPen.style == style) && (QPen.width == width) && (QPen.color == color)) return;
	}

	Change |= SKPCHG_PEN;

	cpen = NULL;
	if (color == 0) QPen.bEnabled = false;
	else QPen.bEnabled = true;
	QPen.style = style;
	QPen.width = width;
	QPen.color = color;
	pencolor = SkpColor(ColorComp(color));

	IsLineTopologyAllowed();
}


// ===============================================================================================
//
void VulkanPad::QuickBrush(DWORD color)
{
#ifdef SKPDBG
	Log("QuickBrush(0x%X)", color);
#endif

	// No Change flags here
	cbrush = NULL;
	if (color == 0) QBrush.bEnabled = false;
	else QBrush.bEnabled = true;
	brushcolor = SkpColor(ColorComp(color));

	IsLineTopologyAllowed();
}


// ===============================================================================================
//
void VulkanPad::AddRectIdx(WORD aV)
{
	Idx[iI++] = aV;
	Idx[iI++] = WORD(aV + 1);
	Idx[iI++] = WORD(aV + 2);
	Idx[iI++] = aV;
	Idx[iI++] = WORD(aV + 2);
	Idx[iI++] = WORD(aV + 3);
}


// ===============================================================================================
//
RECT VulkanPad::GetFullRect(SURFHANDLE hSrc)
{
	// LONG, not long. 'long' is 64 bits here and 32 on Win64, which is the
	// same narrowing _RECT() in VulkanUtil.h exists to avoid.
	return {0, 0, static_cast<LONG>(SURFACE(hSrc)->GetWidth()), static_cast<LONG>(SURFACE(hSrc)->GetHeight())};
}


// ===============================================================================================
//
void VulkanPad::CopyRect(const SURFHANDLE hSrc, const LPRECT _s, int tx, int ty)
{
#ifdef SKPDBG
	Log("CopyRect(%s)", _PTR(hSrc));
#endif

	TexChange(hSrc);

	if (Topology(TRIANGLE)) {

		auto s = _s ? *_s : GetFullRect(hSrc);

		int h = std::abs(int(s.bottom - s.top));
		int w = std::abs(int(s.right - s.left));

		AddRectIdx(vI);

		SkpVtxII(Vtx[vI++], tx    , ty    , s.left , s.top   );
		SkpVtxII(Vtx[vI++], tx    , ty + h, s.left , s.bottom);
		SkpVtxII(Vtx[vI++], tx + w, ty + h, s.right, s.bottom);
		SkpVtxII(Vtx[vI++], tx + w, ty    , s.right, s.top   );

		DWORD x = SKPSW_TEXTURE | SKPSW_CENTER;

		Vtx[vI - 1].fnc = x;
		Vtx[vI - 2].fnc = x;
		Vtx[vI - 3].fnc = x;
		Vtx[vI - 4].fnc = x;
	}
}


// ===============================================================================================
//
void VulkanPad::StretchRect(const SURFHANDLE hSrc, const LPRECT _s, const LPRECT _t)
{
#ifdef SKPDBG
	Log("StretchRect(%s)", _PTR(hSrc));
#endif

	TexChange(hSrc);

	if (Topology(TRIANGLE)) {

		auto s = _s ? *_s : GetFullRect(hSrc);
		auto t = _t ? *_t : tgt;

		AddRectIdx(vI);

		SkpVtxII(Vtx[vI++], t.left , t.top   , s.left , s.top   );
		SkpVtxII(Vtx[vI++], t.left , t.bottom, s.left , s.bottom);
		SkpVtxII(Vtx[vI++], t.right, t.bottom, s.right, s.bottom);
		SkpVtxII(Vtx[vI++], t.right, t.top   , s.right, s.top   );

		DWORD x = SKPSW_TEXTURE | SKPSW_CENTER;

		Vtx[vI - 1].fnc = x;
		Vtx[vI - 2].fnc = x;
		Vtx[vI - 3].fnc = x;
		Vtx[vI - 4].fnc = x;
	}
}


// ===============================================================================================
//
void VulkanPad::RotateRect(const SURFHANDLE hSrc, const LPRECT _s, int tcx, int tcy, float angle, float sw, float sh)
{
#ifdef SKPDBG
	Log("RotateRect(%s)", _PTR(hSrc));
#endif

	TexChange(hSrc);

	if (Topology(TRIANGLE)) {

		auto s = _s ? *_s : GetFullRect(hSrc);

		float w = float(s.right - s.left) * sw;
		float h = float(s.bottom - s.top) * sh;

		float san = sin(angle) * 0.5f;
		float can = cos(angle) * 0.5f;

		float ax = float(tcx) + (-w * can + h * san);
		float ay = float(tcy) + (-w * san - h * can);

		float bx = float(tcx) + (-w * can - h * san);
		float by = float(tcy) + (-w * san + h * can);

		float cx = float(tcx) + (+w * can - h * san);
		float cy = float(tcy) + (+w * san + h * can);

		float dx = float(tcx) + (+w * can + h * san);
		float dy = float(tcy) + (+w * san - h * can);

		AddRectIdx(vI);

		SkpVtxFI(Vtx[vI++], ax, ay, s.left , s.top   );
		SkpVtxFI(Vtx[vI++], bx, by, s.left , s.bottom);
		SkpVtxFI(Vtx[vI++], cx, cy, s.right, s.bottom);
		SkpVtxFI(Vtx[vI++], dx, dy, s.right, s.top   );

		DWORD x = SKPSW_TEXTURE | SKPSW_CENTER;

		Vtx[vI - 1].fnc = x;
		Vtx[vI - 2].fnc = x;
		Vtx[vI - 3].fnc = x;
		Vtx[vI - 4].fnc = x;
	}
}


// ===============================================================================================
//
void VulkanPad::ColorKey(const SURFHANDLE hSrc, const LPRECT _s, int tx, int ty)
{
#ifdef SKPDBG
	Log("ColorKey(%s)", _PTR(hSrc));
#endif

	TexChange(hSrc);

	if (Topology(TRIANGLE)) {

		auto s = _s ? *_s : GetFullRect(hSrc);

		int h = std::abs(int(s.bottom - s.top));
		int w = std::abs(int(s.right - s.left));

		AddRectIdx(vI);

		SkpVtxII(Vtx[vI++], tx    , ty    , s.left , s.top   );
		SkpVtxII(Vtx[vI++], tx    , ty + h, s.left , s.bottom);
		SkpVtxII(Vtx[vI++], tx + w, ty + h, s.right, s.bottom);
		SkpVtxII(Vtx[vI++], tx + w, ty    , s.right, s.top   );

		DWORD f = SKPSW_TEXTURE | SKPSW_COLORKEY | SKPSW_CENTER;

		Vtx[vI - 1].fnc = f;
		Vtx[vI - 2].fnc = f;
		Vtx[vI - 3].fnc = f;
		Vtx[vI - 4].fnc = f;
	}
}


// ===============================================================================================
//
void VulkanPad::ColorKeyStretch(const SURFHANDLE hSrc, const LPRECT _s, const LPRECT _t)
{
#ifdef SKPDBG
	Log("ColorKeyStretch(%s)", _PTR(hSrc));
#endif

	TexChange(hSrc);

	DWORD dwBak = dwBlendState;

	SetBlendState(BlendState((dwBak & 0xF) | BlendState::FILTER_POINT));

	if (Topology(TRIANGLE)) {

		auto s = _s ? *_s : GetFullRect(hSrc);
		auto t = _t ? *_t : tgt;

		AddRectIdx(vI);

		SkpVtxII(Vtx[vI++], t.left , t.top   , s.left , s.top   );
		SkpVtxII(Vtx[vI++], t.left , t.bottom, s.left , s.bottom);
		SkpVtxII(Vtx[vI++], t.right, t.bottom, s.right, s.bottom);
		SkpVtxII(Vtx[vI++], t.right, t.top   , s.right, s.top   );

		DWORD x = SKPSW_TEXTURE | SKPSW_COLORKEY | SKPSW_CENTER;

		Vtx[vI - 1].fnc = x;
		Vtx[vI - 2].fnc = x;
		Vtx[vI - 3].fnc = x;
		Vtx[vI - 4].fnc = x;
	}

	SetBlendState(BlendState(dwBak));
}


// ===============================================================================================
//
void VulkanPad::CopyRectNative(VulkanTexture *pSrc, const LPRECT _s, int tx, int ty)
{
#ifdef SKPDBG
	Log("CopyRectNative(%s)", _PTR(pSrc));
#endif

	TexChangeNative(pSrc);

	if (Topology(TRIANGLE)) {

		auto s = _s ? *_s : GetFullRectNative(pSrc);

		int h = std::abs(int(s.bottom - s.top));
		int w = std::abs(int(s.right - s.left));

		AddRectIdx(vI);

		SkpVtxII(Vtx[vI++], tx    , ty    , s.left     , s.top       );
		SkpVtxII(Vtx[vI++], tx    , ty + h, s.left     , s.bottom - 1);
		SkpVtxII(Vtx[vI++], tx + w, ty + h, s.right - 1, s.bottom - 1);
		SkpVtxII(Vtx[vI++], tx + w, ty    , s.right - 1, s.top       );

		DWORD x = SKPSW_TEXTURE | SKPSW_CENTER;

		Vtx[vI - 1].fnc = x;
		Vtx[vI - 2].fnc = x;
		Vtx[vI - 3].fnc = x;
		Vtx[vI - 4].fnc = x;
	}
}


// ===============================================================================================
//
void VulkanPad::StretchRectNative(VulkanTexture *pSrc, const RECT *_s, const RECT *_t)
{
#ifdef SKPDBG
	Log("StretchRectNative(%s)", _PTR(pSrc));
#endif

	TexChangeNative(pSrc);

	if (Topology(TRIANGLE)) {

		auto s = _s ? *_s : GetFullRectNative(pSrc);
		auto t = *_t;

		AddRectIdx(vI);

		SkpVtxII(Vtx[vI++], t.left , t.top   , s.left     , s.top       );
		SkpVtxII(Vtx[vI++], t.left , t.bottom, s.left     , s.bottom - 1);
		SkpVtxII(Vtx[vI++], t.right, t.bottom, s.right - 1, s.bottom - 1);
		SkpVtxII(Vtx[vI++], t.right, t.top   , s.right - 1, s.top       );

		DWORD x = SKPSW_TEXTURE | SKPSW_CENTER;

		Vtx[vI - 1].fnc = x;
		Vtx[vI - 2].fnc = x;
		Vtx[vI - 3].fnc = x;
		Vtx[vI - 4].fnc = x;
	}
}


// ===============================================================================================
//
void VulkanPad::CopyTetragon(const SURFHANDLE hSrc, const LPRECT _s, const FVECTOR2 tp[4])
{
#ifdef SKPDBG
	Log("CopyTetragon(%s)", _PTR(hSrc));
#endif
	FVECTOR2 sp[4];
	FVECTOR2 a, b, c, d;
	static const int n = 6;
	static const float step = 1.0f / float(n - 1);

	DWORD fn = SKPSW_TEXTURE | SKPSW_CENTER;

	TexChange(hSrc);

	if (Topology(TRIANGLE))
	{
		auto s = _s ? *_s : GetFullRect(hSrc);

		// Was FVECTOR2{s.left, s.top} -- brace-initialising a float pair from
		// two LONGs, which is a narrowing conversion in a braced initialiser
		// list and ill-formed by the standard. MSVC accepts it; GCC rejects
		// it. The conversion is spelled out.
		sp[0] = FVECTOR2(float(s.left) , float(s.top)   );
		sp[1] = FVECTOR2(float(s.left) , float(s.bottom));
		sp[2] = FVECTOR2(float(s.right), float(s.bottom));
		sp[3] = FVECTOR2(float(s.right), float(s.top)   );

		// Create indices
		for (int j = 0; j < (n-1); j++)
		{
			for (int i = 0; i < (n-1); i++)
			{
				WORD q = WORD(vI + i + j * n);
				Idx[iI++] = WORD(q + 0); Idx[iI++] = WORD(q + 1); Idx[iI++] = WORD(q + (n + 1));
				Idx[iI++] = WORD(q + 0); Idx[iI++] = WORD(q + (n + 1)); Idx[iI++] = WORD(q + n);
			}
		}

		// Create grid points
		for (int i = 0; i < n; i++)
		{
			float x = float(i) * step;

			a = lerp(tp[0], tp[3], x);
			b = lerp(tp[1], tp[2], x);
			c = lerp(sp[0], sp[3], x);
			d = lerp(sp[1], sp[2], x);

			for (int k = 0; k < n; k++) {
				FVECTOR2 tv = lerp(a, b, float(k) * step);
				FVECTOR2 sv = lerp(c, d, float(k) * step);
				SkpVtxFF(Vtx[vI], tv.x, tv.y, sv.x, sv.y);
				Vtx[vI].fnc = fn;
				vI++;
			}
		}

		// `int j = 0;` stood above the grid loop and was never used -- the
		// loop declares its own `i` and `k`. Dropped; GCC reports it
		// (-Wunused-variable) and there is no behaviour to preserve.
	}
}


// ===============================================================================================
//
void VulkanPad::FillTetragon(DWORD c, const FVECTOR2 pt[4])
{
#ifdef SKPDBG
	Log("FillTetragon(0x%X)", c);
#endif

	// `DWORD fn = SKPSW_TEXTURE | SKPSW_CENTER;` stood here and is never
	// used: SkpVtxFC sets fnc itself, to SKPSW_CENTER | SKPSW_FRAGMENT.
	// Dropped rather than kept, for the same reason as above -- and note that
	// had it been used it would have been WRONG, asking for a texture on a
	// draw that has none.

	if (Topology(TRIANGLE)) {
		AddRectIdx(vI);
		SkpVtxFC(Vtx[vI++], pt[0].x, pt[0].y, c);
		SkpVtxFC(Vtx[vI++], pt[1].x, pt[1].y, c);
		SkpVtxFC(Vtx[vI++], pt[2].x, pt[2].y, c);
		SkpVtxFC(Vtx[vI++], pt[3].x, pt[3].y, c);
	}
}


// ===============================================================================================
//
bool VulkanPad::TextW (int x, int y, const LPWSTR str, int len)
{
#ifdef SKPDBG
	Log("TextW()");
#endif

	// No "Setup" here, done in PrintSkp()

	if (!cfont) return false;
	if (len == -1) len = int(wcslen(str));
	if (!len) return true;

	VulkanTextPtr pText = static_cast<const VulkanPadFont *>(cfont)->pFont;
	switch (tah) {
		default:
		case LEFT:   pText->SetTextHAlign(0); break;
		case CENTER: pText->SetTextHAlign(1); break;
		case RIGHT:  pText->SetTextHAlign(2); break;
	}

	switch (tav) {
		default:
		case TOP:      pText->SetTextVAlign(0); break;
		case BASELINE: pText->SetTextVAlign(1); break;
		case BOTTOM:   pText->SetTextVAlign(2); break;
	}

	int lineSpace = pText->GetLineSpace();

	std::wstring _str(str, str + size_t(len));

	std::wistringstream f(_str);
	std::wstring s;
	int _y = y;
	while (getline(f, s, L'\n')) {
		pText->PrintSkp(this, x - 1.0f, _y - 1.0f, s.c_str(), -1);
		_y += lineSpace;
	}

	return true;
}

// ===============================================================================================
//
void VulkanPad::TextEx(float x, float y, const char *str, float scale, float angle)
{
#ifdef SKPDBG
	Log("TextEx()");
#endif

	// No "Setup" here, done in PrintSkp()

	if (cfont == NULL) return;

	VulkanTextPtr pText = static_cast<const VulkanPadFont *>(cfont)->pFont;

	switch (tah) {
		default:
		case LEFT:   pText->SetTextHAlign(0); break;
		case CENTER: pText->SetTextHAlign(1); break;
		case RIGHT:  pText->SetTextHAlign(2); break;
	}

	switch (tav) {
		default:
		case TOP:      pText->SetTextVAlign(0); break;
		case BASELINE: pText->SetTextVAlign(1); break;
		case BOTTOM:   pText->SetTextVAlign(2); break;
	}

	pText->SetRotation(angle);
	pText->SetScaling(scale);
	pText->PrintSkp(this, x - 1.0f, y - 1.0f, str, -1, (bkmode == OPAQUE));
}


// ===============================================================================================
//
void VulkanPad::ClipRect(const LPRECT clip)
{
#ifdef SKPDBG
	Log("ClipRect(%s)", _PTR(clip));
#endif

	Change |= SKPCHG_CLIPRECT;

	bEnableScissor = (clip != NULL);
	if (clip) ScissorRect = (*clip);
	else ScissorRect = { 0,0,0,0 };
}


// ===============================================================================================
//
void VulkanPad::Clipper(int idx, const VECTOR3 *uDir, double cos_angle, double dist)
{
#ifdef SKPDBG
	Log("Clipper()");
#endif

	Change |= SKPCHG_CLIPCONE;

	if (idx < 0) idx = 0;
	if (idx > 1) idx = 1;

	if (uDir) {
		// Was D3DXVEC(*uDir), which built a D3DXVECTOR3 from a VECTOR3's
		// three doubles. FVECTOR3 is the same three floats.
		ClipData[idx].uDir = FVECTOR3(float(uDir->x), float(uDir->y), float(uDir->z));
		ClipData[idx].ca = float(cos_angle);
		ClipData[idx].dst = float(dist);
		ClipData[idx].bEnable = true;
	}
	else {
		ClipData[idx].uDir = FVECTOR3(0,0,1);
		ClipData[idx].ca = 2.0f;
		ClipData[idx].dst = 0.0f;
		ClipData[idx].bEnable = false;
	}
}


// ===============================================================================================
//
void VulkanPad::DepthEnable(bool bEnable)
{
#ifdef SKPDBG
	Log("DepthEnable(%u)", DWORD(bEnable));
#endif

	Flush(); // Must Flush() here before a mode change

	if (pDep) {
		Change |= SKPCHG_DEPTH;
		bDepthEnable = bEnable;
	}
	else bDepthEnable = false;
}


// ===============================================================================================
//
// The three casts through (const FMATRIX4*) are gone: mV, mP and mVP were
// D3DXMATRIX and the Sketchpad interface returns FMATRIX4, so every one of
// these had to launder the type. They ARE FMATRIX4 now.
//
const FMATRIX4 *VulkanPad::ViewMatrix() const
{
	return &mV;
}


// ===============================================================================================
//
const FMATRIX4 *VulkanPad::ProjectionMatrix() const
{
	return &mP;
}


// ===============================================================================================
//
const FMATRIX4 *VulkanPad::GetViewProjectionMatrix() const
{
	// mVP is `mutable` for exactly this: a const getter that recomputes.
	VMAT_MatrixMultiply(&mVP, &mV, &mP);
	return &mVP;
}


// ===============================================================================================
//
void VulkanPad::SetViewMatrix(const FMATRIX4 *pV)
{
#ifdef SKPDBG
	Log("SetViewMatrix(%s)", _PTR(pV));
#endif
	Change |= SKPCHG_TRANSFORM;
	if (pV) memcpy(&mV, pV, sizeof(FMATRIX4));
	else mV = mVOrig;
}


// ===============================================================================================
//
void VulkanPad::SetProjectionMatrix(const FMATRIX4 *pP)
{
#ifdef SKPDBG
	Log("SetProjectionMatrix(%s)", _PTR(pP));
#endif
	Change |= SKPCHG_TRANSFORM;
	if (pP) memcpy(&mP, pP, sizeof(FMATRIX4));
	else mP = mPOrig;
}


// ===============================================================================================
//
void VulkanPad::SetViewMode(SkpView mode)
{
#ifdef SKPDBG
	Log("SetViewMode(0x%X)", DWORD(mode));
#endif
	Flush();	// Must Flush() here before a mode change
	Change |= SKPCHG_TRANSFORM;
	vmode = mode;
}


// ===============================================================================================
// !! For a private use in VulkanClient !!
//
FMATRIX4 *VulkanPad::WorldMatrix()
{
#ifdef SKPDBG
	Log("WorldMatrix() ! - ! - !");
#endif
	Change |= SKPCHG_TRANSFORM;
	return &mW;
}


// ===============================================================================================
//
void VulkanPad::SetWorldTransform2D(float scale, float rot, const IVECTOR2 *c, const IVECTOR2 *t)
{
#ifdef SKPDBG
	Log("SetWorldTransform2D(%f, %f, %s, %s)", scale, rot, _PTR(c), _PTR(t));
#endif
	Change |= SKPCHG_TRANSFORM;

	FVECTOR2 ctr = FVECTOR2(0, 0);
	FVECTOR2 trl = FVECTOR2(0, 0);

	if (c) ctr = FVECTOR2(float(c->x), float(c->y));
	if (t) trl = FVECTOR2(float(t->x), float(t->y));

	// Was D3DXMatrixAffineTransformation2D(&mW, scale, &ctr, rot, &trl).
	// D3DX documents that call as D3DXMatrixTransformation2D with no scaling
	// centre, no scaling rotation, a UNIFORM scale, and the same point used
	// as the rotation centre -- which is exactly this. The term order lives
	// in VMAT_Transformation2D (VulkanUtil.cpp), transcribed there once from
	// the documentation rather than re-derived per call site.
	const FVECTOR2 scl(scale, scale);
	VMAT_Transformation2D(&mW, NULL, 0.0f, &scl, &ctr, rot, &trl);
}


// ===============================================================================================
//
void VulkanPad::SetWorldTransform(const FMATRIX4 *pWT)
{
#ifdef SKPDBG
	Log("SetWorldTransform(%s)", _PTR(pWT));
#endif
	Change |= SKPCHG_TRANSFORM;

	if (pWT) memcpy(&mW, pWT, sizeof(FMATRIX4));
	else VMAT_Identity(&mW);
}


// ===============================================================================================
//
void VulkanPad::SetWorldBillboard(const FVECTOR3& wpos, float scale, bool bFixed, const FVECTOR3* index)
{
#ifdef SKPDBG
	Log("SetWorldBillboard()");
#endif
	// mP._11 / mV._11 became mP.m11 / mV.m11 -- the same elements, D3DXMATRIX's
	// leading-underscore naming being the only difference. This function
	// already worked on FMATRIX4 for its output (mWorld._x.xyz etc.), which
	// is why only the two reads change.
	scale *= (mP.m11 + mP.m22) * 0.5f;
	Change |= SKPCHG_TRANSFORM;
	FVECTOR3 up = unit(wpos);
	FVECTOR3 y  = unit(cross((index ? *index : FVECTOR3(mV.m11, mV.m21, mV.m31)), up));
	FVECTOR3 x  = cross(up, y);
	float d = (bFixed ? dot(up, wpos) / float(tgt_desc.Width) : 1.0f) * scale;
	FMATRIX4 mWorld;
	// The _x/_y/_z/_p view of FMATRIX4 is an anonymous struct of FVECTOR4, and
	// GCC rejects a member with a user-declared constructor inside an anonymous
	// aggregate -- so DrawAPI.h guards that view with #ifdef _WIN32 and tells
	// callers elsewhere to write the m** fields, which name the same storage.
	// _x.xyz is m11/m12/m13, _y.xyz is m21/m22/m23, and so on; the .w of each
	// row (m14, m24, m34) is not written here on either build.
	mWorld.m11 = x.x * d;    mWorld.m12 = x.y * d;    mWorld.m13 = x.z * d;
	mWorld.m21 = y.x * d;    mWorld.m22 = y.y * d;    mWorld.m23 = y.z * d;
	mWorld.m31 = up.x * d;   mWorld.m32 = up.y * d;   mWorld.m33 = up.z * d;
	mWorld.m41 = wpos.x;     mWorld.m42 = wpos.y;     mWorld.m43 = wpos.z;
	mWorld.m44 = 1.0f;
	memcpy(&mW, &mWorld, sizeof(FMATRIX4));
}


// ===============================================================================================
//
void VulkanPad::SetGlobalLineScale(float width, float pat)
{
#ifdef SKPDBG
	Log("SetGlobalLineScale(%f, %f)", width, pat);
#endif
	Change |= SKPCHG_PEN;
	linescale = width;
	pattern = pat;
}


// ===============================================================================================
//
void VulkanPad::SetFontTextureNative(VulkanTexture *hNew)
{
	if (hNew == hFontTex) return;
	Change |= SKPCHG_FONT;
	hFontTex = hNew;
}


// ===============================================================================================
//
bool VulkanPad::TexChangeNative(VulkanTexture *hNew)
{
	if (hNew == hTexture) return false;
	Change |= SKPCHG_TEXTURE;
	hTexture = hNew;
	return true;
}


// ===============================================================================================
//
void VulkanPad::TexChange(SURFHANDLE hNew)
{
	if (!SURFACE(hNew)->IsTexture()) {
		LogErr("Sketchpad2: Source is not a texture");
		HALT();
		return;
	}

	TexChangeNative(SURFACE(hNew)->GetTexture());

	if (SURFACE(hNew)->IsColorKeyEnabled()) {
		bColorKey = true;
		// Was D3DXCOLOR(SURFACE(hNew)->ColorKey), which unpacks a DWORD
		// 0xAARRGGBB into four floats. FVECTOR4 has no DWORD constructor, so
		// the unpack is written out -- the same four bytes in the same order
		// as SkpColor's, which is the other place this appears.
		const DWORD ck = SURFACE(hNew)->GetColorKey();
		cColorKey = FVECTOR4(float((ck >> 16) & 0xFF) / 255.0f,
							 float((ck >>  8) & 0xFF) / 255.0f,
							 float( ck        & 0xFF) / 255.0f,
							 float((ck >> 24) & 0xFF) / 255.0f);
	}
	else {
		bColorKey = false;
		cColorKey = FVECTOR4(0, 0, 0, 0);
	}
}


// ===============================================================================================
//
int VulkanPad::DrawMeshGroup(const MESHHANDLE hMesh, DWORD grp, Sketchpad::MeshFlags flags, const SURFHANDLE hTex)
{
#ifdef SKPDBG
	Log("DrawMeshGroup(%s, gpr=%u, flags=0x%X, hTex=%s)", _PTR(hMesh), grp, flags, _PTR(hTex));
#endif
	UINT num;
	SketchMesh* pMesh = GetSketchMesh(hMesh);

	if (!pMesh) return -1;

	DWORD nGrp = pMesh->GroupCount();

	if (grp >= nGrp) return -1;

	if (!FX) return -1;

	// Flush Pending graphics ------------------------------------
	//
	SetupDevice(tCurrent);

	// Initialize device for drawing a mesh ----------------------
	//
	pMesh->Init();

	// Was pDev->SetRenderState(D3DRS_CULLMODE, ...) between BeginPass and the
	// draw. Cull mode is pipeline state here, so it goes in the override and
	// arrives before the bind. See VulkanEffectFile::PassOverride.
	VulkanEffectFile::PassOverride ovr;
	// The Windows call is
	// SetRenderState(D3DRS_CULLMODE, (flags & CULL_NONE) ? D3DCULL_NONE
	//                                                    : D3DCULL_CCW),
	// so the else branch is an explicit CCW rather than "leave it alone", and
	// CULL_CCW says that rather than CULL_PASS.
	ovr.cullMode = (flags & Sketchpad::MeshFlags::CULL_NONE)
		? VulkanEffectFile::PassOverride::CULL_NONE
		: VulkanEffectFile::PassOverride::CULL_CCW;

	// The mesh supplies its own vertex layout: SketchMesh renders NTVERTEX,
	// which is what SketchMeshVS declares. The pad's own SketchpadDecl
	// describes SkpVtx and would be the wrong stride here.
	FX->SetVertexDecl(pNTVertexDecl);
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);

	FX->SetTechnique(eDrawMesh);
	FX->SetBool(eShade, (flags & Sketchpad::MeshFlags::SMOOTH_SHADE) != 0);

	if (!FX->Begin(&num, 0)) return -1;

	if (flags & Sketchpad::MeshFlags::RENDER_ALL) grp = 0;

	// Draw a mesh group(s) ----------------------------------------
	//
	// ONE PASS PER GROUP, where Windows opens the pass once and calls
	// FX->CommitChanges() between groups. CommitChanges existed because D3DX
	// buffered parameter writes and a draw after BeginPass would otherwise
	// not see them; here the parameter block is uploaded AT BeginPass, so a
	// value changed afterwards has nowhere to go until the next one. Closing
	// and reopening the pass is the same sequence of draws with the same
	// values -- one more uniform slice out of the frame arena per group --
	// and it is what VulkanEffect.cpp's RenderReEntry does for the same
	// reason.
	while (grp < nGrp)
	{
		SURFHANDLE pTex = hTex ? hTex : pMesh->GetTexture(grp);
		FVECTOR4    Mat = pMesh->GetMaterial(grp);

		if (pTex)
		{
			FX->SetTexture(eTex0, SURFACE(pTex)->GetTexture());
			FX->SetBool(eTexEn, true);
		}
		else
		{
			FX->SetBool(eTexEn, false);
		}

		FX->SetValue(eMtrl, &Mat, sizeof(FVECTOR4));

		if (FX->BeginPassEx(0, &ovr)) {
			pMesh->RenderGroup(grp);
			FX->EndPass();
		}

		grp++;

		if (!(flags & Sketchpad::MeshFlags::RENDER_ALL)) break;
	}

	FX->End();

	return nGrp;
}


// ===============================================================================================
//
RECT VulkanPad::GetFullRectNative(VulkanTexture *hSrc)
{
	// Was hSrc->GetLevelDesc(0, &desc). A VkImage answers no questions about
	// itself; VulkanTexture records what it was asked for. See VulkanTypes.h.
	const VulkanImageDesc &desc = hSrc->Desc();
	return {0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height)};
}



// ======================================================================================
// Polyline Interface
// ======================================================================================


VulkanPolyLine::VulkanPolyLine(VulkanDevice *pDev, const FVECTOR2 *pt, int npt, bool bConnect) : VulkanPolyBase(0)
{
	nPt = WORD(npt + 2);
	nVtx = WORD(2 * nPt);
	nIdx = WORD(6 * nPt);

	// Were CreateVertexBuffer and CreateIndexBuffer with D3DUSAGE_DYNAMIC |
	// D3DUSAGE_WRITEONLY in D3DPOOL_DEFAULT -- "the CPU writes this every
	// frame and never reads it". Vulkan has one buffer type and says the same
	// thing with a usage flag and host-visible memory; D3DFMT_INDEX16 is not
	// a property of the buffer either, it is given to vkCmdBindIndexBuffer.
	pOwnerDev = pDev;
	pVB = pDev->CreateBuffer(nVtx * sizeof(SkpVtx), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
	pIB = pDev->CreateBuffer(nIdx * sizeof(WORD), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);

	if (!pVB || !pIB) {
		LogErr("VulkanPolyLine: buffer allocation failed (%u vertices, %u indices)", nVtx, nIdx);
		return;
	}

	WORD *Ix = (WORD *)pIB->Map();

	if (Ix) {
		iI = 0;
		for (WORD i=0,p=0;p<nPt;p++) {
			Ix[iI++] = WORD(i+0);
			Ix[iI++] = WORD(i+1);
			Ix[iI++] = WORD(i+2);
			Ix[iI++] = WORD(i+1);
			Ix[iI++] = WORD(i+3);
			Ix[iI++] = WORD(i+2);
			i += 2;
		}
		pIB->Unmap();
	}

	vI = 0;
	bLoop = bConnect;

	if (pt) Update(pt, npt, bConnect);
}


// ===============================================================================================
//
VulkanPolyLine::~VulkanPolyLine()
{

}


// ===============================================================================================
//
void VulkanPolyLine::Release()
{
	// Was SAFE_RELEASE on both. Vulkan objects are not reference counted --
	// the device that made the buffer destroys it, which is pOwnerDev: the
	// device this object's constructor was handed. See the note on
	// VulkanDevice::DestroyBuffer for why this is a call rather than `delete`.
	if (pOwnerDev) {
		if (pVB) { pOwnerDev->DestroyBuffer(pVB); pVB = NULL; }
		if (pIB) { pOwnerDev->DestroyBuffer(pIB); pIB = NULL; }
	}
}


// ===============================================================================================
//
void VulkanPolyLine::Draw(VulkanPad *pSkp, VulkanDevice *pDev)
{
	// Was SetStreamSource + SetIndices + SetRenderState(CULLMODE, NONE) +
	// DrawIndexedPrimitive. The cull mode is pipeline state and VulkanPad's
	// Flush already sets CULL_NONE for every Sketchpad draw, so it is not
	// repeated here; the rest is the same three operations spelled in Vulkan.
	if (!pVB || !pIB || !pDev->IsRecording()) return;

	VkCommandBuffer cmd = pDev->GetCommandBuffer();

	VkBuffer vb = pVB->Buffer();
	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
	vkCmdBindIndexBuffer(cmd, pIB->Buffer(), 0, VK_INDEX_TYPE_UINT16);

	// DrawIndexedPrimitive(TRIANGLELIST, 0, 0, vI, 0, vI-2) -- vI-2 is the
	// PRIMITIVE count, so the index count is three times it. vkCmdDrawIndexed
	// takes indices, which is why the multiplication appears here and the
	// division disappears from VulkanPad::Flush.
	if (vI >= 2) vkCmdDrawIndexed(cmd, UINT(vI - 2) * 3, 1, 0, 0, 0);

	(void)pSkp;
}


// ===============================================================================================
//
void VulkanPolyLine::Update(const FVECTOR2 *_pt, int _npt, bool bConnect)
{
	if (!pVB) return;

	SkpVtx *Vx = (SkpVtx *)pVB->Map();
	if (!Vx) return;

	// Was `D3DXVECTOR2 *pt = (D3DXVECTOR2 *)_pt;` -- a reinterpret cast from
	// FVECTOR2*, legal only because the two are the same two floats. With one
	// vector type there is nothing to cast.
	const FVECTOR2 *pt = _pt;

	WORD npt = WORD(_npt);
	WORD li = WORD(npt - 1);

	vI = 0;
	float length = 0.0f;

	FVECTOR2 pp; // Prev point
	FVECTOR2 np;	// Next point

	bLoop = bConnect;

	// Line Init ------------------------------------------------------------
	//
	if (bLoop) pp = pt[li];
	else	   pp = _FV2Extrapolate(pt[0], pt[1]);

	// Create line segments -------------------------------------------------
	//
	for (WORD i = 0; i<npt; i++) {

		if (i != li)	np = pt[i + 1];
		else {
			if (bLoop)	np = pt[0];
			else		np = _FV2Extrapolate(pt[i], pt[i - 1]);
		}

		WORD vII = WORD(vI + 1);

		// --------------------------------------
		Vx[vI].x = Vx[vII].x = float(pt[i].x);
		Vx[vI].y = Vx[vII].y = float(pt[i].y);
		Vx[vI].nx = Vx[vII].nx = np.x;
		Vx[vI].ny = Vx[vII].ny = np.y;
		Vx[vI].px = Vx[vII].px = pp.x;
		Vx[vI].py = Vx[vII].py = pp.y;
		Vx[vI].l = Vx[vII].l = length;
		// --------------------------------------
		Vx[vI].fnc = SKPSW_WIDEPEN_L | SKPSW_PENCOLOR;
		Vx[vII].fnc = SKPSW_WIDEPEN_R | SKPSW_PENCOLOR;
		vI+=2;
		// --------------------------------------
		pp = pt[i];
		length += _FV2Length(np, pp);
	}

	if (bLoop) {
		Vx[vI++] = Vx[0];
		Vx[vI++] = Vx[1];
	}

	pVB->Unmap();
}


// ======================================================================================
// Triangle Interface
// ======================================================================================


VulkanTriangle::VulkanTriangle(VulkanDevice *pDev, const gcCore::clrVtx *pt, int npt, int _style) : VulkanPolyBase(1)
{
	nPt = WORD(npt);
	style = _style;
	pOwnerDev = pDev;
	pVB = pDev->CreateBuffer(nPt * sizeof(SkpVtx), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
	if (!pVB) {
		LogErr("VulkanTriangle: buffer allocation failed (%u vertices)", nPt);
		return;
	}
	if (pt) Update(pt, npt);
}


// ===============================================================================================
//
VulkanTriangle::~VulkanTriangle()
{

}


// ===============================================================================================
//
void VulkanTriangle::Release()
{
	// Was SAFE_RELEASE(pVB). See VulkanPolyLine::Release.
	if (pOwnerDev && pVB) { pOwnerDev->DestroyBuffer(pVB); pVB = NULL; }
}


// ===============================================================================================
// The three D3DPRIMITIVETYPEs this class draws with, moved from the draw to
// the pipeline. See VulkanPolyBase::Topology.
//
// TRIANGLE_FAN IS THE ONE TO WATCH. It is core Vulkan 1.0 and works on every
// desktop driver, but it is the single topology that
// VK_KHR_portability_subset can withhold -- MoltenVK on macOS does, because
// Metal has no fan. Nothing in this port runs there; if anything ever does,
// the fix is to expand a fan into a list in Update() rather than to change
// this function, because the vertex data is already ours to reshape.
//
VkPrimitiveTopology VulkanTriangle::Topology() const
{
	if (style == PF_FAN)   return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
	if (style == PF_STRIP) return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
	return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;		// PF_TRIANGLES
}


// ===============================================================================================
//
void VulkanTriangle::Draw(VulkanPad* pSkp, VulkanDevice *pDev)
{
	if (!pVB || !pDev->IsRecording()) return;

	VkCommandBuffer cmd = pDev->GetCommandBuffer();

	VkBuffer vb = pVB->Buffer();
	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);

	// The three DrawPrimitive calls took a PRIMITIVE count -- nPt/3 triangles,
	// or nPt-2 for a fan or a strip. vkCmdDraw takes a VERTEX count, which is
	// nPt in all three cases: the topology already says how the vertices are
	// grouped, and it is now baked into the pipeline (see Topology above).
	vkCmdDraw(cmd, nPt, 1, 0, 0);

	(void)pSkp;
}


// ===============================================================================================
//
void VulkanTriangle::Update(const gcCore::clrVtx *pt, int npt)
{
	if (!pVB) return;

	SkpVtx *Vx = (SkpVtx *)pVB->Map();
	if (!Vx) return;

	// memset(Vtx, 0, sizeof(SkpVtx)*npt) verbatim, through a void*. SkpVtx has
	// a user-provided default constructor, which makes it non-trivial and
	// makes GCC warn (-Wclass-memaccess) about memset on it -- correctly, in
	// general. Not here: Vx points at freshly mapped device memory that no
	// constructor has ever run over, and SkpVtx is trivially copyable, so
	// clearing it is exactly what the Windows code meant. The cast is GCC's
	// own documented way to say so.
	memset(static_cast<void *>(Vx), 0, sizeof(SkpVtx)*npt);

	for (int i = 0; i < npt; i++) {
		Vx[i].x = pt[i].pos.x;
		Vx[i].y = pt[i].pos.y;
		Vx[i].clr = pt[i].color;
		Vx[i].fnc = SKPSW_CENTER | SKPSW_FRAGMENT;
	}

	pVB->Unmap();
}
