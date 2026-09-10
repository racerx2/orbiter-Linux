
// ===================================================
// Copyright (C) 2012-2026 Jarmo Nikkanen
// licensed under LGPL v2
// ===================================================
//
// CONVERTED FROM OVP/D3D9Client/D3D9Pad3.cpp, read end to end (305 lines).
//
// THE SKETCHPAD3 ADDITIONS: the colour matrix, the gamma and noise render
// parameters, the world-transform stack, GradientFillRect, ColorFill,
// StretchRegion, Clear and SetClipDistance.
//
// Most of it is arithmetic on FMATRIX4 and FVECTOR4 members and does not
// touch the graphics API at all -- SetColorMatrix, SetBrightness,
// GetRenderParam, SetRenderParam, SetEnable, ClearEnable, PushWorldTransform
// and PopWorldTransform convert line for line. What changes:
//
//   memcpy_s becomes memcpy. The _s form is a Microsoft bounds-checked
//   variant (Annex K, which no other implementation ships); its destination
//   size is the second argument. Same three-argument copy underneath.
//
//   D3DXMatrixScaling in SetWorldScaleTransform2D. D3DX is a Direct3D
//   UTILITY library with no Vulkan counterpart, and a scaling matrix is four
//   assignments, so it is written out here rather than given a VMAT_ name of
//   its own -- the same treatment D3DXMatrixOrthoOffCenterLH gets in
//   VulkanPad.cpp's BeginDrawing and again in SetClipDistance below.
//
//   Clear(). THE ONE CALL IN THIS FILE THAT REACHES THE DEVICE. On Windows
//   it is pDev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, ...),
//   which a D3D9 device accepts at any point inside a scene. Vulkan has two
//   different clears and the choice is not free: vkCmdClearColorImage works
//   only OUTSIDE a render pass, and this is called from inside one, so the
//   call is vkCmdClearAttachments -- the only Vulkan clear that is legal
//   there, and the only one that takes a rectangle. See the function.
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
		// memcpy_s(&ColorMatrix, sizeof(FMATRIX4), pMatrix, sizeof(FMATRIX4)).
		// The bounds-checked _s forms are Microsoft's; the destination size
		// they take is the second argument, and with both sizes equal the
		// check can never fire. Plain memcpy is the same copy.
		memcpy(&ColorMatrix, pMatrix, sizeof(FMATRIX4));
		SetEnable(SKP3E_CMATR);
	}
	else {
		// Was memset(&ColorMatrix, 0, sizeof(FMATRIX4)). FMATRIX4 has
		// user-declared constructors, which makes it non-trivial and makes GCC
		// warn about memset on it. Zero() is the type's OWN spelling of the
		// same sixteen zeroes -- see DrawAPI.h -- so this uses it rather than
		// casting the warning away.
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
	// Was memcpy_s(&fm, sizeof(FMATRIX4), &mW, sizeof(D3DXMATRIX)) -- mW was a
	// D3DXMATRIX and the two types are the same sixteen floats, which is why
	// the source size was spelled with the other type's name. mW IS an
	// FMATRIX4 now, so both sizes are the one size.
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

	// Was D3DXVECTOR3. FVECTOR3 is the same three floats and is what
	// VMAT_SetTranslation takes.
	FVECTOR3 t;

	t.x = 0;
	t.y = 0;
	t.z = 0;

	if (trl) t.x = float(trl->x), t.y = float(trl->y);

	// Was D3DXMatrixScaling(&mW, sx, sy, 1.0f) followed by
	// D3DMAT_SetTranslation. D3DXMatrixScaling writes the identity and then
	// the three diagonal terms; D3DX is a Direct3D utility library with no
	// Vulkan counterpart, and four assignments do not need a name of their
	// own. D3DMAT_ IS the client's own, and became VMAT_.
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
// Was:
//     DWORD flags = 0;
//     if (bColor) flags |= D3DCLEAR_TARGET;
//     if (bDepth) flags |= D3DCLEAR_ZBUFFER;
//     pDev->Clear(0, NULL, flags, color, 1.0f, 0);
//
// THE TWO CLEARS ARE NOT INTERCHANGEABLE IN VULKAN, and picking the wrong one
// is a validation error rather than a wrong picture:
//
//   vkCmdClearColorImage / vkCmdClearDepthStencilImage clear a whole image
//   (VkImageSubresourceRange -- mip levels and array layers, no rectangle)
//   and are legal ONLY OUTSIDE a render pass.
//
//   vkCmdClearAttachments clears the attachments of the render pass that is
//   currently bound, takes a VkClearRect, and is legal ONLY INSIDE one.
//
// Every VulkanPad drawing call runs inside the core's render pass -- Flush
// binds a pipeline, which cannot happen outside one -- so this is the second,
// and it is also the one that matches what the D3D9 call did: clear the
// CURRENT render target and the CURRENT depth buffer, whatever they are, not
// a named image. The attachment indices are the core's: colour is 0, and the
// depth attachment carries no index at all (aspect bits identify it).
//
// The zero-order argument of pDev->Clear was the D3DRECT count and NULL the
// rectangle list, meaning "the whole target"; VkClearRect has no NULL, so the
// whole target is spelled out as tgt -- the rectangle BeginDrawing already
// computed from the target's own dimensions.
//
// WORTH KNOWING, AND FAITHFULLY REPRODUCED: this does NOT Flush() first. Any
// Sketchpad geometry still sitting in the vertex queue is drawn AFTER the
// clear, so `DrawLine(); Clear();` leaves the line visible on Windows. That
// is the Windows behaviour and callers may depend on it; changing it here
// would be a silent behaviour change, not a conversion.
//
void VulkanPad::Clear(DWORD color, bool bColor, bool bDepth)
{
	if (!pDev || !pDev->IsRecording()) return;
	if (!bColor && !bDepth) return;

	VkClearAttachment att[2] = {};
	uint32_t nAtt = 0;

	if (bColor) {
		// The D3DCOLOR argument is 0xAARRGGBB, the same packing SkpColor's
		// DWORD constructor unpacks. Written out here because a VkClearValue
		// takes four floats and nothing else.
		att[nAtt].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		att[nAtt].colorAttachment = 0;
		att[nAtt].clearValue.color.float32[0] = float((color >> 16) & 0xFF) / 255.0f;	// R
		att[nAtt].clearValue.color.float32[1] = float((color >>  8) & 0xFF) / 255.0f;	// G
		att[nAtt].clearValue.color.float32[2] = float( color        & 0xFF) / 255.0f;	// B
		att[nAtt].clearValue.color.float32[3] = float((color >> 24) & 0xFF) / 255.0f;	// A
		nAtt++;
	}

	if (bDepth) {
		// 1.0f and 0 were the Z and stencil arguments of pDev->Clear. The
		// stencil bit goes in only when the format has one -- clearing an
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
	// Was D3DXMatrixOrthoOffCenterLH(&mO, 0, W, H, 0, nr, fr). Written out for
	// the same reason as in VulkanPad.cpp's BeginDrawing, and identical to it
	// except that the near and far planes are the caller's rather than 0 and
	// zfar. D3D9 clip space is 0 <= z <= w and so is Vulkan's, so the
	// left-handed orthographic matrix carries over unchanged.
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

	// mP._33 / mP._43 became mP.m33 / mP.m43 -- the same two elements,
	// D3DXMATRIX's leading-underscore naming being the only difference.
	mP.m33 = fr / (fr - nr);
	mP.m43 = -nr * mP.m33;
}
