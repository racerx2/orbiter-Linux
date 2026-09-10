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
// CONVERTED FROM OVP/D3D9Client/CloudMgr.cpp, read end to end (191 lines).
//
// The same file as SurfMgr.cpp with a different technique, and it takes the
// same four changes -- see that file's header for each in full:
//
//   FX->CommitChanges() has no counterpart, so every draw closes and reopens
//   the pass; SetStreamSource/SetIndices/DrawIndexedPrimitive become the
//   three vkCmd calls with a primitive count turned into an index count;
//   SetVertexDeclaration and the topology move to before Begin(); and
//   D3DMATERIAL9/D3D9Sun become MATERIAL/VulkanSun field for field, with
//   D3DMATERIAL9's capitalised members (.Diffuse, .Ambient, .Power) spelled
//   as MATERIAL's (.diffuse, .ambient, .power).
//
// One thing to notice while reading: RenderTile uses `pDev` without declaring
// it, where the sibling functions declare a local. That is D3D9Effect's
// STATIC pDev, inherited through TileManager. It is VulkanEffect::pDev here
// and resolves the same way; the local is added anyway so the two paths read
// alike and the recording check has something to test.
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
	
	// Was D3DMATERIAL9: four D3DCOLORVALUE and a float, which MATERIAL is
	// field for field, so the braced initialiser carries over as written.
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
	// D3DMATERIAL9's members are capitalised; MATERIAL's are not. Same two
	// fields.
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
	// pipeline state and must be declared before BeginPass builds it.
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
		// texture is set, because BeginPass is what writes the descriptor.
		//
		// The cull comes through the override because vPlanet sets
		// D3DRS_CULLMODE on the device around this call -- see
		// TileManager::cullMode. CULL_PASS leaves the technique's own.
		VulkanEffectFile::PassOverride ovr;
		ovr.cullMode = cullMode;
		if (!FX->BeginPassEx(0, &ovr)) continue;

		VkBuffer vb = mesh.pVB->Buffer();
		VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
		vkCmdBindIndexBuffer(cmd, mesh.pIB->Buffer(), 0, VK_INDEX_TYPE_UINT16);
		// mesh.nf TRIANGLES -> 3*mesh.nf indices.
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
	// The BeginPass(0) that stood here has moved into RenderTile; see
	// SurfMgr.cpp's InitRenderTile for the reason.
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
	// inherited static `pDev` for the three device calls below; this names it
	// locally so the recording check has something to test.
	VulkanDevice *pDevice = VulkanEffect::pDev;
	if (!pDevice || !pDevice->IsRecording() || !mesh.pVB || !mesh.pIB) return;
	// See RenderSimple above: the cull travels on the manager because
	// vPlanet set it as device state around the call.
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
