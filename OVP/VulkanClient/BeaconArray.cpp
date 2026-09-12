// ===========================================================================================
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2012-2026 Jarmo Nikkanen
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
// Initialiser list reordered to declaration order (-Wreorder). Every value is
// a parameter or a constant, so nothing observable changes.
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

	// Was CreateVertexBuffer(..., D3DUSAGE_DYNAMIC|D3DUSAGE_POINTS, 0,
	// D3DPOOL_DEFAULT) in HR(). DYNAMIC is the hostVisible flag; D3DUSAGE_POINTS
	// was a point-sprite hint with no Vulkan counterpart, and there are no
	// pools. Failure arrives as a NULL pointer, which the Map below tests.
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
	// Was SAFE_RELEASE(pVB). vkDestroyBuffer and vkFreeMemory both take the
	// VkDevice, so only the device that made the buffer can free it.
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
	// Was pVB->Lock(...) tested against S_OK; Map is the same operation and
	// reports failure by returning NULL, which callers already test for.
	return (BAVERTEX *)pVB->Map(0, nVert * sizeof(BAVERTEX));
}


// ===========================================================================================
//
void BeaconArray::UnLockVertexBuffer()
{
	if (!pVB) return;
	// Was HR(pVB->Unlock()); vkUnmapMemory cannot fail, so nothing to check.
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

	// The vertex declaration was set between BeginPass and the draw, and
	// D3DPT_POINTLIST was an argument to DrawPrimitive. Both are baked into
	// the VkPipeline that BeginPass binds, so both must be declared first.
	FX->SetVertexDecl(pBAVertexDecl);
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_POINT_LIST);

	FX->Begin(&numPasses, 0);		// was D3DXFX_DONOTSAVESTATE
	FX->BeginPass(0);

	//dev->SetRenderState(D3DRS_ZENABLE, 0);

	// Was SetStreamSource + DrawPrimitive. The stride moves into the vertex
	// declaration, where a VkVertexInputBindingDescription keeps it, and a
	// point list has one vertex per primitive, so nVert crosses unchanged.
	if (dev->IsRecording()) {
		VkCommandBuffer cmd = dev->GetCommandBuffer();
		VkBuffer vb = pVB->Buffer();
		VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
		vkCmdDraw(cmd, nVert, 1, 0, 0);
	}

	// SetRenderState(D3DRS_ZENABLE, 1) stood here. Nothing to restore: the
	// depth test is pipeline state and the next bind replaces it.

	FX->EndPass();
	FX->End();

	// SetRenderState(D3DRS_POINTSPRITEENABLE, 0) stood here. Vulkan has no
	// such render state: point size is written by the vertex shader through
	// gl_PointSize, which BeaconArray's GLSL has to do itself.
}
