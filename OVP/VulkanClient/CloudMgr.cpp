// ==============================================================
// CloudMgr.cpp
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2007-2026 Martin Schweiger
// ==============================================================

// ==============================================================
// class CloudManager (implementation)
//
// Planetary rendering management for cloud layers, including a simple
// LOD (level-of-detail) algorithm for patch resolution.
// ==============================================================
//
// FX->CommitChanges() has no counterpart -- a descriptor set is written at
// BeginPass, not re-sent mid-pass -- so every per-tile draw closes and reopens
// the pass instead. D3DMATERIAL9 and D3D9Sun become MATERIAL and VulkanSun
// field for field, with D3DMATERIAL9's capitalised members (.Diffuse,
// .Ambient, .Power) spelled as MATERIAL's (.diffuse, .ambient, .power).
// ==============================================================

#include "CloudMgr.h"
#include "VPlanet.h"
#include "VulkanSurface.h"
#include "Scene.h"

using namespace oapi;

// =======================================================================

CloudManager::CloudManager(VulkanClient *gclient, const vPlanet *vplanet)
: TileManager (gclient, vplanet)
{
	size_t len = strlen(objname)+7;
	char *texname = new char[len];
	strcpy_s(texname, len, objname);
	strcat_s(texname, len, "_cloud");
	delete []objname;
	objname = texname;

	maxlvl = min (*(int*)gc->GetConfigParam (CFGPRM_SURFACEMAXLEVEL),        // global setting
	              *(int*)oapiGetObjectParam (obj, OBJPRM_PLANET_SURFACEMAXLEVEL)); // planet-specific setting
	maxbaselvl = min (8, maxlvl);
	pcdir = _V(1,0,0);
	lightfac = *(double*)gc->GetConfigParam (CFGPRM_SURFACELIGHTBRT);
	nmask = 0;
	nhitex = nhispec = 0;

	atmc = oapiGetPlanetAtmConstants (obj);

	int maxidx = patchidx[maxbaselvl];
	tiledesc = new TILEDESC[maxidx];
	memset (tiledesc, 0, maxidx*sizeof(TILEDESC));

	for (int i = 0; i < patchidx[maxbaselvl]; i++)	tiledesc[i].flag = 1;
}

// =======================================================================

void CloudManager::LoadData()
{
	if (ntex!=0 || bNoTextures) return;
	LoadTextures();
}

// =======================================================================

void CloudManager::Render(VulkanDevice *dev, FMATRIX4 &wmat, double scale, int level, double viewap)
{
	LoadData();
	if (bNoTextures) return;
	
	MATERIAL def_mat = {{1,1,1,1},{1,1,1,1},{1,1,1,1},{0,0,0,1},0};

	FX->SetTechnique(eCloudTech);
	FX->SetValue(eMat, &def_mat, sizeof(MATERIAL));
	FX->SetValue(eSun, gc->GetScene()->GetSun(), sizeof(VulkanSun));
	FX->SetInt(eSpecularMode, 0);

	if (microtex) {
		FX->SetTexture(eTex3, SURFACE(microtex)->GetTexture());
	}

	bool do_micro = (microtex && microlvl > 0.01);

	if (do_micro) {	
		FX->SetFloat(eMix, float(microlvl));
	}
	else {
		FX->SetFloat(eMix, 0.0f);
	}
	
	TileManager::Render(dev, wmat, scale, level, viewap);
}


void CloudManager::RenderShadow(VulkanDevice *dev, FMATRIX4 &wmat, double scale, int level, double viewap, float shadowalpha)
{
	LoadData();
	if (bNoTextures) return;

	MATERIAL cloudmat = {{0,0,0,1},{0,0,0,1},{0,0,0,0},{0,0,0,0},0};
	cloudmat.diffuse.a = cloudmat.ambient.a = shadowalpha;

	FX->SetTechnique(eCloudShadow);
	FX->SetValue(eMat, &cloudmat, sizeof(MATERIAL));
	FX->SetValue(eSun, gc->GetScene()->GetSun(), sizeof(VulkanSun));
	FX->SetInt(eSpecularMode, 0);

	if (microtex) {
		FX->SetTexture(eTex3, SURFACE(microtex)->GetTexture());
	}

	bool do_micro = (microtex && microlvl > 0.01);

	if (do_micro) {
		FX->SetFloat(eMix, float(microlvl));
	}
	else {
		FX->SetFloat(eMix, 0.0f);
	}

	TileManager::Render(dev, wmat, scale, level, viewap);
}

// ==============================================================

void CloudManager::RenderSimple(int level, int npatch, TILEDESC *tile, FMATRIX4 *mWrld)
{
	LoadData();
	if (bNoTextures) return;

	// render complete sphere (used at low LOD levels)
	VulkanDevice *pDevice = gc->GetDevice();
	if (!pDevice->IsRecording()) return;
	VkCommandBuffer cmd = pDevice->GetCommandBuffer();

	// Was pDev->SetVertexDeclaration(pPatchVertexDecl). Both of these are
	// pipeline state and have to be declared before BeginPass builds it.
	FX->SetVertexDecl(pPatchVertexDecl);
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);

	FX->SetMatrix(eW, mWrld);
	UINT numPasses = 0;
	FX->Begin(&numPasses, 0);		// was D3DXFX_DONOTSAVESTATE

	for (int idx = 0; idx < npatch; idx++) {
		VBMESH &mesh = PATCH_TPL[level][idx]; // patch template	
		if (!mesh.pVB || !mesh.pIB) continue;
		FX->SetTexture(eTex0, tile[idx].tex);

		// FX->CommitChanges() stood here; the pass opens after the per-patch
		// texture is set, because BeginPass is what writes the descriptor. The
		// cull comes through the override because vPlanet set D3DRS_CULLMODE
		// as device state around this call; CULL_PASS leaves the technique's.
		VulkanEffectFile::PassOverride ovr;
		ovr.cullMode = cullMode;
		if (!FX->BeginPassEx(0, &ovr)) continue;

		VkBuffer vb = mesh.pVB->Buffer();
		VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
		vkCmdBindIndexBuffer(cmd, mesh.pIB->Buffer(), 0, VK_INDEX_TYPE_UINT16);
		// mesh.nf triangles -> 3*mesh.nf indices.
		vkCmdDrawIndexed(cmd, mesh.nf * 3, 1, 0, 0, 0);

		FX->EndPass();
	}

	FX->End();
}


void CloudManager::InitRenderTile()
{
	FX->SetVertexDecl(pPatchVertexDecl);
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);

	FX->SetFloat(eTime, float(fmod(oapiGetSimTime(),60.0)));
	UINT numPasses = 0;
	FX->Begin(&numPasses, 0);
	// The BeginPass(0) that stood here has moved into RenderTile, which now
	// opens and closes a pass per tile.
}

void CloudManager::EndRenderTile()
{
	FX->End();
}


// =======================================================================

void CloudManager::RenderTile (int lvl, int hemisp, int ilat, int nlat, int ilng, int nlng, double sdist,
	TILEDESC *tile, const TEXCRDRANGE &range, VulkanTexture *tex, VulkanTexture *ltex, DWORD flag)
{
	VBMESH &mesh = PATCH_TPL[lvl][ilat]; // patch template

	if (range.tumin == 0 && range.tumax == 1) {
		FX->SetVector(eTexOff, ptr(FVECTOR4(1.0f, 0.0f, 1.0f, 0.0f)));
	}
	else {
		float tuscale = range.tumax-range.tumin, tuofs = range.tumin;
		float tvscale = range.tvmax-range.tvmin, tvofs = range.tvmin;
		FX->SetVector(eTexOff, ptr(FVECTOR4(tuscale,tuofs,tvscale,tvofs)));
	}
	FX->SetMatrix(eW, &mWorld);
	FX->SetTexture(eTex0, tex);	  // Diffuse Texture

	// FX->CommitChanges() stood here. The Windows function reached for the
	// inherited static `pDev`; this names it locally so the recording check
	// has something to test.
	VulkanDevice *pDevice = VulkanEffect::pDev;
	if (!pDevice || !pDevice->IsRecording() || !mesh.pVB || !mesh.pIB) return;
	// See RenderSimple above for the cull override.
	VulkanEffectFile::PassOverride ovr;
	ovr.cullMode = cullMode;
	if (!FX->BeginPassEx(0, &ovr)) return;

	VkCommandBuffer cmd = pDevice->GetCommandBuffer();
	VkBuffer vb = mesh.pVB->Buffer();
	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
	vkCmdBindIndexBuffer(cmd, mesh.pIB->Buffer(), 0, VK_INDEX_TYPE_UINT16);
	vkCmdDrawIndexed(cmd, mesh.nf * 3, 1, 0, 0, 0);

	FX->EndPass();
}
