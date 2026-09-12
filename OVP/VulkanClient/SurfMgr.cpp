// ==============================================================
// SurfMgr.cpp
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2007-2026 Martin Schweiger
//				 2011 - 2016 Jarmo Nikkanen (D3D9Client modification)  
// ==============================================================

// ==============================================================
// class SurfaceManager (implementation)
//
// Planetary surface rendering management, including a simple
// LOD (level-of-detail) algorithm for surface patch resolution.
// ==============================================================
//
// FX->CommitChanges() has no counterpart. D3DX buffered parameter writes and
// CommitChanges pushed them to the device mid-pass, so one BeginPass served many draws
// with different textures and matrices. Here the uniform block is uploaded and
// the descriptor set written at BeginPass, so a parameter set after it does not
// reach the draw -- silently. Each draw therefore closes and reopens the pass.
//
// DrawIndexedPrimitive's last argument was a primitive count (mesh.nf
// triangles); vkCmdDrawIndexed takes an index count, 3*mesh.nf.
//
// D3DXCOLOR(cAmbient) becomes FCOLOR_ARGB(cAmbient), not FVECTOR4(cAmbient):
// FVECTOR4's DWORD constructor reads ABGR and a D3DCOLOR is ARGB, so the
// obvious spelling would exchange red and blue.
//
// HR() is dropped where it wrapped an FX->Set*, which returns bool rather than
// a VkResult.
// ==============================================================

#include "SurfMgr.h"
#include "VPlanet.h"
#include "VulkanSurface.h"
#include "Scene.h"
#include "DebugControls.h"

using namespace oapi;

// Was D3DMATERIAL9. MATERIAL has the same layout under the SDK's names, so the
// braced initialisers carry over as written.
MATERIAL watermat = {{1,1,1,1},{1,1,1,1},{1,1,1,1},{0,0,0,0},20.0f};
MATERIAL def_mat = {{1,1,1,1},{1,1,1,1},{1,1,1,1},{0,0,0,1},0};

int nrender[15]; // temporary

// =======================================================================

SurfaceManager::SurfaceManager (VulkanClient *gclient, const vPlanet *vplanet)
: TileManager(gclient, vplanet)
{

	maxlvl = min (*(int*)gc->GetConfigParam (CFGPRM_SURFACEMAXLEVEL),				// global setting
	              *(int*)oapiGetObjectParam (obj, OBJPRM_PLANET_SURFACEMAXLEVEL));	// planet-specific setting

	maxbaselvl = min(8, maxlvl);

	pcdir = _V(1,0,0);
	lightfac = *(double*)gc->GetConfigParam (CFGPRM_SURFACELIGHTBRT);
	spec_base = 0.95f;
	atmc = oapiGetPlanetAtmConstants (obj);

	int maxidx = patchidx[maxbaselvl];
	tiledesc = new TILEDESC[maxidx];
	memset (tiledesc, 0, maxidx*sizeof(TILEDESC));
}

// =======================================================================

void SurfaceManager::LoadData()
{
	if (ntex!=0) return;
	LoadPatchData ();
	LoadTileData ();
	LoadTextures ();
	LoadSpecularMasks ();
}


// =======================================================================

void SurfaceManager::SetMicrotexture (const char *fname)
{
	TileManager::SetMicrotexture (fname);
	spec_base = (microtex ? 1.05f : 0.95f); // increase specular intensity to compensate for "ripple" losses
}

// =======================================================================

void SurfaceManager::Render(VulkanDevice *dev, FMATRIX4 &wmat, double scale, int level, double viewap, bool bfog)
{
	if (ntex==0) LoadData();

	// modify colour of specular reflection component
	if (bGlobalSpecular) {
		extern MATERIAL watermat;
		SpecularColour (&watermat.specular);
		watermat.power = (microtex ? 40.0f : 35.0f);
	}

	TileManager::Render (dev, wmat, scale, level, viewap, bfog);
}

// ==============================================================

void SurfaceManager::RenderSimple(int level, int npatch, TILEDESC *tile, FMATRIX4 *mWrld)
{
	// render complete sphere (used at low LOD levels)
	FX->SetTechnique(ePlanetTile);
	FX->SetValue(eSun, gc->GetScene()->GetSun(), sizeof(VulkanSun));
	FX->SetMatrix(eW, mWrld);
	FX->SetValue(eWater, &watermat, sizeof(MATERIAL));
	FX->SetValue(eMat, &def_mat, sizeof(MATERIAL));
	FX->SetValue(eColor, ptr(FCOLOR_ARGB(cAmbient)), sizeof(FVECTOR4));
	FX->SetFloat(eTime, float(fmod(oapiGetSimTime(),60.0)));
	FX->SetVector(eTexOff, ptr(FVECTOR4(1.0f, 0.0f, 1.0f, 0.0f)));
	FX->SetFloat(eMix, 0.0f);

	VulkanDevice *pDev = gc->GetDevice();
	if (!pDev->IsRecording()) return;
	VkCommandBuffer cmd = pDev->GetCommandBuffer();

	// Vertex layout and topology are pipeline state now, so both must be set
	// before BeginPass builds the pipeline.
	FX->SetVertexDecl(pPatchVertexDecl);
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);

	UINT numPasses = 0;
	FX->Begin(&numPasses, 0);		// was D3DXFX_DONOTSAVESTATE; nothing is saved

	for (int idx = 0; idx < npatch; idx++) {

		VBMESH &mesh = PATCH_TPL[level][idx]; // patch template
		if (!mesh.pVB || !mesh.pIB) continue;

		bool purespec = ((tile[idx].flag & 3) == 2);
		bool mixedspec = ((tile[idx].flag & 3) == 3);

		// step 1: render full patch, either completely diffuse or completely specular
		if (purespec) { // completely specular
			FX->SetInt(eSpecularMode, 1);
		}
		else if (mixedspec) {
			FX->SetInt(eSpecularMode, 2);
		}
		else {
			FX->SetInt(eSpecularMode, 0);
		}

		VulkanTexture *ltex = tile[idx].ltex;
		if (ltex==NULL) ltex = gc->GetDefaultTexture()->GetTexture();

		FX->SetTexture(eTex0, tile[idx].tex);
		FX->SetTexture(eTex1, ltex);

		// FX->CommitChanges() stood here; the pass now opens inside the loop,
		// after the per-patch state above.
		if (!FX->BeginPass(0)) continue;

		VkBuffer vb = mesh.pVB->Buffer();
		VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
		vkCmdBindIndexBuffer(cmd, mesh.pIB->Buffer(), 0, VK_INDEX_TYPE_UINT16);
		vkCmdDrawIndexed(cmd, mesh.nf * 3, 1, 0, 0, 0);

		FX->EndPass();
	}

	FX->End();
}


void SurfaceManager::InitRenderTile()
{
	FX->SetTechnique(ePlanetTile);
	FX->SetValue(eSun, gc->GetScene()->GetSun(), sizeof(VulkanSun));
	FX->SetValue(eMat, &def_mat, sizeof(MATERIAL));
	FX->SetValue(eWater, &watermat, sizeof(MATERIAL));
	FX->SetValue(eColor, ptr(FCOLOR_ARGB(cAmbient)), sizeof(FVECTOR4));
	FX->SetFloat(eTime, float(fmod(oapiGetSimTime(),60.0)));

	FX->SetVertexDecl(pPatchVertexDecl);
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);

	UINT numPasses = 0;
	FX->Begin(&numPasses, 0);
	// The BeginPass(0) that stood here has moved into RenderTile: each tile
	// sets its own textures, matrix and texture offsets, and those must be
	// written before the pass opens. The Begin()/End() pair still brackets the
	// whole hemisphere, as it did.
}

void SurfaceManager::EndRenderTile()
{
	FX->End();
}


// =======================================================================

void SurfaceManager::RenderTile (int lvl, int hemisp, int ilat, int nlat, int ilng, int nlng, double sdist,
	TILEDESC *tile, const TEXCRDRANGE &range, VulkanTexture *tex, VulkanTexture *ltex, DWORD flag)
{
	VulkanDevice *pDev = gc->GetDevice();

	VBMESH &mesh = PATCH_TPL[lvl][ilat]; // patch template
	
	if (range.tumin == 0 && range.tumax == 1) {
		FX->SetVector(eTexOff, ptr(FVECTOR4(1.0f, 0.0f, 1.0f, 0.0f)));
	}
	else {
		float tuscale = range.tumax-range.tumin, tuofs = range.tumin;
		float tvscale = range.tvmax-range.tvmin, tvofs = range.tvmin;
		FX->SetVector(eTexOff, ptr(FVECTOR4(tuscale,tuofs,tvscale,tvofs)));
	}

	DWORD flags = *(DWORD*)gc->GetConfigParam(CFGPRM_GETDEBUGFLAGS);

	if (DebugControls::IsActive()) {
		if (flags&DBG_FLAGS_TILES) {
			// The zeros are written 0.0f. D3DXVECTOR4 had one four-float
			// constructor and converted the int literals silently; FVECTOR4
			// has all-float, all-int and all-double overloads, so a mixed
			// call is ambiguous. Same values, spelled to pick an overload.
			float x = 0.6f;
			switch(lvl) {
				case 14: FX->SetVector(eColor, ptr(FVECTOR4(x, 0.0f, 0.0f, 0.0f))); break;
				case 13: FX->SetVector(eColor, ptr(FVECTOR4(0.0f, x, 0.0f, 0.0f))); break;
				case 12: FX->SetVector(eColor, ptr(FVECTOR4(0.0f, 0.0f, x, 0.0f))); break;
				case 11: FX->SetVector(eColor, ptr(FVECTOR4(x, x, 0.0f, 0.0f))); break;
				case 10: FX->SetVector(eColor, ptr(FVECTOR4(x, 0.0f, x, 0.0f))); break;
				case 9:  FX->SetVector(eColor, ptr(FVECTOR4(0.0f, x, x, 0.0f))); break;
				default: FX->SetVector(eColor, ptr(FVECTOR4(0.0f, 0.0f, 0.0f, 0.0f))); break;
			}
		}
	}

	bool purespec = ((flag & 3) == 2);
	bool mixedspec = ((flag & 3) == 3);
	
	if (ltex==NULL) ltex = gc->GetDefaultTexture()->GetTexture();

	FX->SetMatrix(eW, &mWorld);
	FX->SetTexture(eTex0, tex);	  // Base Texture
	FX->SetTexture(eTex1, ltex);  // Specular Mask and Night Lights
	
	if (microtex) {
		FX->SetTexture(eTex3, SURFACE(microtex)->GetTexture()); 
		FX->SetFloat(eMix, 1.0f);
	}
	else FX->SetFloat(eMix, 0.0f);

	if (mixedspec)     FX->SetInt(eSpecularMode, 2);
	else if (purespec) FX->SetInt(eSpecularMode, 1);
	else			   FX->SetInt(eSpecularMode, 0);

	// FX->CommitChanges() stood here; the pass opens instead.
	if (!pDev->IsRecording() || !mesh.pVB || !mesh.pIB) return;
	if (!FX->BeginPass(0)) return;

	VkCommandBuffer cmd = pDev->GetCommandBuffer();
	VkBuffer vb = mesh.pVB->Buffer();
	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
	vkCmdBindIndexBuffer(cmd, mesh.pIB->Buffer(), 0, VK_INDEX_TYPE_UINT16);
	vkCmdDrawIndexed(cmd, mesh.nf * 3, 1, 0, 0, 0);

	FX->EndPass();
}
