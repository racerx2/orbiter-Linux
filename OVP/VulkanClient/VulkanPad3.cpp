
// ===================================================
// Copyright (C) 2012-2026 Jarmo Nikkanen
// licensed under LGPL v2
// ===================================================
//
// memcpy_s becomes memcpy throughout: the _s form is Microsoft's
// bounds-checked variant (Annex K, which no other implementation ships), and
// its destination size is the second argument. The D3DX matrix helpers have no
// Vulkan counterpart and are written out as the assignments they perform.
// ===================================================

#include "VulkanPad.h"
#include "VulkanSurface.h"
#include "VulkanUtil.h"
#include <sstream>



// ===============================================================================================
// Sketchpad3 Interface
// ===============================================================================================

void VulkanPad::ColorCompatibility(bool bEnable)
{
#ifdef SKPDBG
	Log("ColorCompatibility(%u)", DWORD(bEnable));
#endif
	bColorComp = bEnable;
}

// ===============================================================================================
//
const FMATRIX4 *VulkanPad::GetColorMatrix()
{
	return &ColorMatrix;
}


// ===============================================================================================
//
void VulkanPad::SetColorMatrix(const FMATRIX4 *pMatrix)
{
#ifdef SKPDBG
	Log("SetColorMatrix(0x%X)", DWORD(pMatrix));
#endif
	if (pMatrix) {
		memcpy(&ColorMatrix, pMatrix, sizeof(FMATRIX4));
		SetEnable(SKP3E_CMATR);
	}
	else {
		// Was memset(&ColorMatrix, 0, sizeof(FMATRIX4)); FMATRIX4's
		// user-declared constructors make that a -Wclass-memaccess warning.
		// Zero() is the type's own spelling of the same sixteen zeroes.
		ColorMatrix.Zero();
		ColorMatrix.m11 = 1.0f;
		ColorMatrix.m22 = 1.0f;
		ColorMatrix.m33 = 1.0f;
		ColorMatrix.m44 = 1.0f;
		ClearEnable(SKP3E_CMATR);
	}
}


// ===============================================================================================
//
void VulkanPad::SetBrightness(const FVECTOR4 *pBrightness)
{
#ifdef SKPDBG
	Log("SetBrightness(0x%X)", DWORD(pBrightness));
#endif
	if (pBrightness == NULL) SetColorMatrix(NULL);
	else {
		ColorMatrix.Zero();		// was memset; see SetColorMatrix
		ColorMatrix.m11 = pBrightness->r;
		ColorMatrix.m22 = pBrightness->g;
		ColorMatrix.m33 = pBrightness->b;
		ColorMatrix.m44 = pBrightness->a;
		SetEnable(SKP3E_CMATR);
	}
}


// ===============================================================================================
//
FVECTOR4 VulkanPad::GetRenderParam(RenderParam param)
{
	switch (param) {
	case Sketchpad::RenderParam::PRM_GAMMA: return Gamma;
	case Sketchpad::RenderParam::PRM_NOISE: return Noise;
	}
	return FVECTOR4(0, 0, 0, 0);
}


// ===============================================================================================
//
void VulkanPad::SetRenderParam(RenderParam param, const FVECTOR4 *d)
{
#ifdef SKPDBG
	Log("SetRenderParam(%u, 0x%X)", param, DWORD(d));
#endif
	if (d == NULL) {
		switch (param) {
		case Sketchpad::RenderParam::PRM_GAMMA: Gamma = FVECTOR4(1, 1, 1, 1); ClearEnable(SKP3E_GAMMA); break;
		case Sketchpad::RenderParam::PRM_NOISE: Noise = FVECTOR4(0, 0, 0, 0); ClearEnable(SKP3E_NOISE); break;
		}
		return;
	}

	switch (param) {
	case Sketchpad::RenderParam::PRM_GAMMA: Gamma = FVECTOR4(d->r, d->g, d->b, d->a); SetEnable(SKP3E_GAMMA);  break;
	case Sketchpad::RenderParam::PRM_NOISE: Noise = *d; SetEnable(SKP3E_NOISE);  break;
	}
}

// ===============================================================================================
//
void VulkanPad::SetEnable(DWORD config)
{
	Change |= SKPCHG_EFFECTS;
	Enable |= config;
}


// ===============================================================================================
//
void VulkanPad::ClearEnable(DWORD config)
{
	Change |= SKPCHG_EFFECTS;
	Enable &= (~config);
}


// ===============================================================================================
//
void VulkanPad::SetBlendState(BlendState dwState)
{
#ifdef SKPDBG
	Log("SetBlendState(%u)", dwState);
#endif
	// Must Flush() here before a mode change
	Flush();
	dwBlendState = dwState;
}


// ===============================================================================================
//
FMATRIX4 VulkanPad::GetWorldTransform() const
{
	FMATRIX4 fm;
	memcpy(&fm, &mW, sizeof(FMATRIX4));
	return fm;
}


// ===============================================================================================
//
void VulkanPad::PushWorldTransform()
{
#ifdef SKPDBG
	Log("PushWorldTransform()");
#endif
	mWStack.push(mW);
}


// ===============================================================================================
//
void VulkanPad::PopWorldTransform()
{
#ifdef SKPDBG
	Log("PopWorldTransform()");
#endif
	if (mWStack.empty() == false) {
		mW = mWStack.top();
		mWStack.pop();
		Change |= SKPCHG_TRANSFORM;
	}
}


// ===============================================================================================
//
void VulkanPad::SetWorldScaleTransform2D(const FVECTOR2 *scl, const IVECTOR2 *trl)
{
#ifdef SKPDBG
	Log("SetWorldScaleTransform2D(0x%X, 0x%X)", DWORD(scl), DWORD(trl));
#endif
	Change |= SKPCHG_TRANSFORM;

	float sx = 1.0f, sy = 1.0f;

	if (scl) sx = scl->x, sy = scl->y;

	FVECTOR3 t;

	t.x = 0;
	t.y = 0;
	t.z = 0;

	if (trl) t.x = float(trl->x), t.y = float(trl->y);

	// Was D3DXMatrixScaling(&mW, sx, sy, 1.0f): the identity, then the three
	// diagonal terms.
	VMAT_Identity(&mW);
	mW.m11 = sx;
	mW.m22 = sy;
	mW.m33 = 1.0f;

	VMAT_SetTranslation(&mW, &t);
}


// ===============================================================================================
//
void VulkanPad::GradientFillRect(const LPRECT R, DWORD c1, DWORD c2, bool bVertical)
{
#ifdef SKPDBG
	Log("GradientFillRect()");
#endif

	DWORD a, b, c, d;

	a = d = c1; b = c = c2;

	if (bVertical) { a = b = c1; c = d = c2; }

	int l = R->left;	int r = R->right;
	int t = R->top;		int m = R->bottom;

	r--; m--;

	if (Topology(TRIANGLE)) {
		AddRectIdx(vI);
		SkpVtxGF(Vtx[vI++], l, t, a);
		SkpVtxGF(Vtx[vI++], r, t, b);
		SkpVtxGF(Vtx[vI++], r, m, c);
		SkpVtxGF(Vtx[vI++], l, m, d);
	}
}


// ===============================================================================================
//
void VulkanPad::ColorFill(DWORD color, const LPRECT tgt)
{
#ifdef SKPDBG
	Log("ColorFill()");
#endif
	if (tgt) FillRect(tgt->left, tgt->top, tgt->right, tgt->bottom, SkpColor(color));
	else FillRect(0, 0, tgt_desc.Width, tgt_desc.Height, SkpColor(color));
}

// ===============================================================================================
//
void VulkanPad::StretchRegion(const skpRegion *rgn, const SURFHANDLE hSrc, const LPRECT out)
{
#ifdef SKPDBG
	Log("StretchRegion()");
#endif
	const RECT *ext = &(rgn->outr);
	const RECT *itr = &(rgn->intr);

	int x0 = ext->left;
	int x1 = itr->left;
	int x2 = itr->right;
	int x3 = ext->right;

	int y0 = ext->top;
	int y1 = itr->top;
	int y2 = itr->bottom;
	int y3 = ext->bottom;

	int tx0 = out->left;
	int tx3 = out->right;
	int ty0 = out->top;
	int ty3 = out->bottom;

	int tx1 = tx0 + (x1 - x0);
	int tx2 = tx3 - (x3 - x2);
	int ty1 = ty0 + (y1 - y0);
	int ty2 = ty3 - (y3 - y2);

	// Corners
	if (x0 != x1 && y0 != y1) CopyRect(hSrc, ptr(_R(x0, y0, x1, y1)), tx0, ty0);	// TOP-LEFT
	if (x2 != x3 && y0 != y1) CopyRect(hSrc, ptr(_R(x2, y0, x3, y1)), tx2, ty0);	// TOP-RIGHT
	if (x0 != x1 && y2 != y3) CopyRect(hSrc, ptr(_R(x0, y2, x1, y3)), tx0, ty2);	// BTM-LEFT
	if (x2 != x3 && y2 != y3) CopyRect(hSrc, ptr(_R(x2, y2, x3, y3)), tx2, ty2);	// BTM-RIGHT

																					// Sides
	if (x1 != x2 && y0 != y1) StretchRect(hSrc, ptr(_R(x1, y0, x2, y1)), ptr(_R(tx1, ty0, tx2, ty1)));	// TOP
	if (x1 != x2 && y2 != y3) StretchRect(hSrc, ptr(_R(x1, y2, x2, y3)), ptr(_R(tx1, ty2, tx2, ty3)));	// BOTTOM
	if (x0 != x1 && y1 != y2) StretchRect(hSrc, ptr(_R(x0, y1, x1, y2)), ptr(_R(tx0, ty1, tx1, ty2)));	// LEFT
	if (x2 != x3 && y1 != y2) StretchRect(hSrc, ptr(_R(x2, y1, x3, y2)), ptr(_R(tx2, ty1, tx3, ty2)));	// RIGHT

	// Center
	StretchRect(hSrc, ptr(_R(x1, y1, x2, y2)), ptr(_R(tx1, ty1, tx2, ty2)));
}

// ===============================================================================================
// Was pDev->Clear(0, NULL, D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER, color, 1.0f, 0),
// which a D3D9 device accepts anywhere inside a scene. Vulkan's two clears are
// not interchangeable: vkCmdClearColorImage / vkCmdClearDepthStencilImage
// clear a whole image and are legal only OUTSIDE a render pass, while
// vkCmdClearAttachments clears the currently bound pass's attachments, takes a
// rectangle, and is legal only INSIDE one. Every VulkanPad drawing call runs
// inside the core's render pass, so this is the second -- which is also the
// one that matches D3D9's "the current target, whatever it is".
//
// The NULL rectangle list meant "the whole target"; VkClearRect has no NULL,
// so it is spelled out as tgt.
//
// Faithfully reproduced: this does NOT Flush() first, so geometry still in the
// vertex queue is drawn AFTER the clear. `DrawLine(); Clear();` leaves the line
// visible on Windows too, and callers may depend on it.
//
void VulkanPad::Clear(DWORD color, bool bColor, bool bDepth)
{
	if (!pDev || !pDev->IsRecording()) return;
	if (!bColor && !bDepth) return;

	VkClearAttachment att[2] = {};
	uint32_t nAtt = 0;

	if (bColor) {
		// The D3DCOLOR argument is 0xAARRGGBB; a VkClearValue takes four
		// floats and nothing else.
		att[nAtt].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		att[nAtt].colorAttachment = 0;
		att[nAtt].clearValue.color.float32[0] = float((color >> 16) & 0xFF) / 255.0f;	// R
		att[nAtt].clearValue.color.float32[1] = float((color >>  8) & 0xFF) / 255.0f;	// G
		att[nAtt].clearValue.color.float32[2] = float( color        & 0xFF) / 255.0f;	// B
		att[nAtt].clearValue.color.float32[3] = float((color >> 24) & 0xFF) / 255.0f;	// A
		nAtt++;
	}

	if (bDepth) {
		// The stencil bit goes in only when the format has one; clearing an
		// aspect the attachment does not have is invalid.
		att[nAtt].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		VkFormat dsFmt = pDev->SelectDepthFormat();
		if (dsFmt == VK_FORMAT_D32_SFLOAT_S8_UINT ||
			dsFmt == VK_FORMAT_D24_UNORM_S8_UINT ||
			dsFmt == VK_FORMAT_D16_UNORM_S8_UINT) att[nAtt].aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
		att[nAtt].clearValue.depthStencil.depth = 1.0f;
		att[nAtt].clearValue.depthStencil.stencil = 0;
		nAtt++;
	}

	VkClearRect rect = {};
	rect.rect.offset.x = int32_t(tgt.left);
	rect.rect.offset.y = int32_t(tgt.top);
	rect.rect.extent.width = uint32_t(tgt.right - tgt.left);
	rect.rect.extent.height = uint32_t(tgt.bottom - tgt.top);
	rect.baseArrayLayer = 0;
	rect.layerCount = 1;

	vkCmdClearAttachments(pDev->GetCommandBuffer(), nAtt, att, 1, &rect);
}

// ===============================================================================================
//
void VulkanPad::SetClipDistance(float nr, float fr)
{
	// Was D3DXMatrixOrthoOffCenterLH(&mO, 0, W, H, 0, nr, fr), written out.
	// D3D9 clip space is 0 <= z <= w and so is Vulkan's, so the left-handed
	// orthographic matrix carries over unchanged.
	{
		const float l = 0.0f, r = float(tgt_desc.Width);
		const float b = float(tgt_desc.Height), t = 0.0f;
		const float zn = nr, zf = fr;

		VMAT_Identity(&mO);
		mO.m11 = 2.0f / (r - l);
		mO.m22 = 2.0f / (t - b);
		mO.m33 = 1.0f / (zf - zn);
		mO.m41 = (l + r) / (l - r);
		mO.m42 = (t + b) / (b - t);
		mO.m43 = zn / (zn - zf);
		mO.m44 = 1.0f;
	}

	mP.m33 = fr / (fr - nr);
	mP.m43 = -nr * mP.m33;
}
