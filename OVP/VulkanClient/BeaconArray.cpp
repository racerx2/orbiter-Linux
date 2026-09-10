// ===========================================================================================
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2012-2026 Jarmo Nikkanen
// ===========================================================================================
//
// CONVERTED FROM OVP/D3D9Client/BeaconArray.cpp, read end to end (148 lines).
//
// Six functions. Four of them are arithmetic and bookkeeping that convert
// unchanged; the two that talk to Direct3D are the vertex buffer and the
// draw, and they are the whole of the conversion:
//
//  1. CreateVertexBuffer -> VulkanDevice::CreateBuffer. D3DUSAGE_DYNAMIC is
//     the hostVisible flag -- memory the CPU maps and rewrites every frame,
//     which is exactly what Update() does. D3DUSAGE_POINTS HAS NO
//     COUNTERPART and needs none: it was a hint that the buffer would feed
//     point sprites, and Vulkan has no per-buffer usage bit for that.
//     D3DPOOL_DEFAULT likewise -- there are no pools.
//
//  2. Lock/Unlock -> Map/Unmap. Same protocol, Vulkan spelling. The names
//     LockVertexBuffer / UnLockVertexBuffer are kept because they are this
//     class's own interface and RunwayLights.cpp is written against them.
//
//  3. The draw. SetVertexDeclaration and the D3DPT_POINTLIST argument both
//     become pipeline state declared before Begin(); SetStreamSource +
//     DrawPrimitive become vkCmdBindVertexBuffers + vkCmdDraw. For a point
//     list the primitive count IS the vertex count, so nVert crosses
//     unchanged -- which is not true of the strip and list draws elsewhere
//     in the client.
//
//  4. THE THREE SetRenderState CALLS HAVE NO COUNTERPART, and it is worth
//     being precise about which of them mattered. D3DRS_ZENABLE=1 after the
//     draw restores device state for whoever draws next; there is nothing to
//     restore, because the next pipeline bind carries its own depth state.
//     D3DRS_POINTSPRITEENABLE=0 is the same, and it also names the one thing
//     in this file that the GLSL has to take over: point sprites are not a
//     render state in Vulkan, so BeaconArray's vertex shader must write
//     gl_PointSize itself. That is recorded in the ledger and in
//     BeaconArray.h.
//
// SAFE_RELEASE(pVB) is gone with COM. vkDestroyBuffer takes the VkDevice
// that vkCreateBuffer was called on, so the release is a call on the device
// -- the same device the constructor asked for the buffer.
//
// The constructor's initialiser list is reordered into declaration order.
// The Windows order (nVert, vB, pVB, hBase, bidx, base_elev) is not the
// order the members are declared in, so members are initialised in an order
// the list does not show -- eleventh instance of the class in this client,
// and GCC's -Wreorder is on under -Wall.
// ===========================================================================================

#include "BeaconArray.h"
#include "Log.h"
#include "Scene.h"
#include "VulkanSurface.h"
#include "VulkanConfig.h"
// vPlanet.h / vBase.h on Windows. The files on disk are VPlanet.h and
// VBase.h; NTFS does not care and ext4 does.
#include "VPlanet.h"
#include "VBase.h"

using namespace oapi;

// ===========================================================================================
//
BeaconArray::BeaconArray(BeaconArrayEntry *pEnt, DWORD nEntry, vBase *_vB)
	: VulkanEffect()
	, nVert(nEntry)
	, bidx(0)
	, pVB(NULL)
	, hBase(NULL)
	, vB(_vB)
	, base_elev()
{
	_TRACE;

	if (vB) hBase = vB->GetObject();

	pBeaconPos = new BeaconPos[nEntry];

	// Was CreateVertexBuffer(bytes, D3DUSAGE_DYNAMIC|D3DUSAGE_POINTS, 0,
	//                        D3DPOOL_DEFAULT, &pVB, NULL) wrapped in HR().
	// HR() checks a VkResult and this returns a pointer, so the check is the
	// NULL test the Map below already performs.
	pVB = gc->GetDevice()->CreateBuffer(nEntry * sizeof(BAVERTEX),
									   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);

	BAVERTEX *pVrt = LockVertexBuffer();

	if (pVrt) {

		for (DWORD i=0;i<nEntry;i++) {

			if (vB) pBeaconPos[i].vLoc = unit(vB->ToLocal(pEnt[i].pos, &pBeaconPos[i].lng, &pBeaconPos[i].lat));

			pVrt[i].pos = FVEC(pEnt[i].pos);
			pVrt[i].dir = FVEC(pEnt[i].dir);

			pVrt[i].color = pEnt[i].color;
			pVrt[i].size  = pEnt[i].size;
			pVrt[i].angle = cos(pEnt[i].angle * 0.0174532925f * 0.5f);

			pVrt[i].on  = pEnt[i].lon;
			pVrt[i].off = pEnt[i].loff;

			pVrt[i].bright  = pEnt[i].bright;
			pVrt[i].falloff = pEnt[i].fall;
		}
		UnLockVertexBuffer();
	}
	else {
		LogErr("Failed to lock a vertex buffer in BeaconArray()");
	}

	pBright = gc->clbkLoadTexture("D3D9RwyLight.dds");

	if (pBright==NULL) LogErr("D3D9RwyLight.dds is Missing");
}


// ===========================================================================================
//
BeaconArray::~BeaconArray()
{
	SAFE_DELETEA(pBeaconPos);
	// Was SAFE_RELEASE(pVB) -- a COM reference count. vkDestroyBuffer and
	// vkFreeMemory both take the VkDevice as their first argument, so the
	// device that made the buffer is the only thing that can free it.
	if (pVB) { gc->GetDevice()->DestroyBuffer(pVB); pVB = NULL; }
	gc->clbkReleaseTexture(pBright);
}


// ===========================================================================================
//
void BeaconArray::Update(DWORD nCount, vPlanet *vP)
{
	if (nCount>nVert) nCount = nVert;
	double meanelev = vP->GetSize();
	BAVERTEX *pVrt = LockVertexBuffer();
	if (!pVrt) return;
	for (DWORD i=0;i<nCount;i++) {
		double elv = 0;
		if (vP->GetElevation(pBeaconPos[bidx].lng, pBeaconPos[bidx].lat, &elv)==1) {
			VECTOR3 vLoc = pBeaconPos[bidx].vLoc * (meanelev+elv);
			vB->FromLocal(vLoc, &pVrt[bidx].pos);
		}
		bidx++;	if (bidx>=nVert) bidx=0;
	}
	UnLockVertexBuffer();
}


// ===========================================================================================
//
BAVERTEX * BeaconArray::LockVertexBuffer()
{
	if (!pVB) return NULL;
	// Was pVB->Lock(0, bytes, (LPVOID*)&pVert, 0) tested against S_OK.
	// VulkanBuffer::Map is the same operation and reports failure by
	// returning NULL, which is what the caller already tests for.
	return (BAVERTEX *)pVB->Map(0, nVert * sizeof(BAVERTEX));
}


// ===========================================================================================
//
void BeaconArray::UnLockVertexBuffer()
{
	if (!pVB) return;
	// Was HR(pVB->Unlock()). vkUnmapMemory cannot fail and Unmap returns
	// void, so there is nothing for HR() to check.
	pVB->Unmap();
}


// ===========================================================================================
//
void BeaconArray::Render(VulkanDevice *dev, const FMATRIX4 *pW, float time)
{
	if (!pVB) return;

	UINT numPasses = 0;
	FX->SetTechnique(eBeaconArrayTech);
	FX->SetMatrix(eW, pW);
	FX->SetTexture(eTex0, SURFACE(pBright)->GetTexture());
	FX->SetFloat(eTime, time);
	FX->SetFloat(eMix, float(Config->RwyBrightness));

	// Was dev->SetVertexDeclaration(pBAVertexDecl) between BeginPass and the
	// draw, and D3DPT_POINTLIST as DrawPrimitive's first argument. Both are
	// baked into the VkPipeline that BeginPass binds, so both have to be
	// declared before Begin().
	FX->SetVertexDecl(pBAVertexDecl);
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_POINT_LIST);

	FX->Begin(&numPasses, 0);		// was D3DXFX_DONOTSAVESTATE
	FX->BeginPass(0);

	//dev->SetRenderState(D3DRS_ZENABLE, 0);

	// SetStreamSource(0, pVB, 0, sizeof(BAVERTEX)) + DrawPrimitive(POINTLIST,
	// 0, nVert). The stride moved into the vertex declaration, which is where
	// a VkVertexInputBindingDescription keeps it; and a point list has one
	// vertex per primitive, so vkCmdDraw's vertex count is nVert unchanged.
	if (dev->IsRecording()) {
		VkCommandBuffer cmd = dev->GetCommandBuffer();
		VkBuffer vb = pVB->Buffer();
		VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
		vkCmdDraw(cmd, nVert, 1, 0, 0);
	}

	// dev->SetRenderState(D3DRS_ZENABLE, 1) stood here, restoring what the
	// commented-out line above would have changed. Nothing to restore: the
	// depth test is pipeline state and the next bind replaces it.

	FX->EndPass();
	FX->End();

	// dev->SetRenderState(D3DRS_POINTSPRITEENABLE, 0) stood here. There is no
	// such render state in Vulkan at all -- point size is written by the
	// vertex shader through gl_PointSize -- so the enable, and therefore the
	// disable, have no counterpart. See the note in BeaconArray.h.
}
