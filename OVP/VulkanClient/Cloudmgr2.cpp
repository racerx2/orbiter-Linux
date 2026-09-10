// ==============================================================
//   ORBITER VISUALISATION PROJECT (OVP)
//   Copyright (C) 2006-2026 Martin Schweiger
//   Dual licensed under GPL v3 and LGPL v3
// ==============================================================

// ==============================================================
// cloudmgr2.cpp
// Rendering of planetary cloud layers, engine v2, including a simple
// LOD (level-of-detail) algorithm for cloud patch resolution.
// ==============================================================
//
// CONVERTED FROM OVP/D3D9Client/Cloudmgr2.cpp, read end to end (301 lines).
//
// Eight functions: three CloudTile members and five explicit specialisations
// of TileManager2<CloudTile>. Almost all of it is quadtree walking, LOD
// arithmetic and shader parameter marshalling, and converts line for line.
//
// THREE THINGS CHANGE.
//
//  1. THE DRAW. SetStreamSource + SetIndices + DrawIndexedPrimitive become
//     vkCmdBindVertexBuffers + vkCmdBindIndexBuffer + vkCmdDrawIndexed. The
//     one number that is NOT carried over unchanged is the count:
//     DrawIndexedPrimitive's last argument is a PRIMITIVE count (mesh->nf,
//     faces) and vkCmdDrawIndexed's first is an INDEX count, so it is
//     mesh->nf*3. The stride moves into the vertex declaration, where a
//     VkVertexInputBindingDescription keeps it, and VK_INDEX_TYPE_UINT16 is
//     given at the bind rather than being a property of the buffer as
//     D3DFMT_INDEX16 was.
//
//     The topology is NOT set here even though the Windows call names
//     D3DPT_TRIANGLELIST: ShaderClass builds TRIANGLE_LIST pipelines unless
//     a caller says otherwise, and the pipeline was already bound by
//     TileManager2<CloudTile>::Render's Setup() before any tile is drawn.
//
//  2. THE TWO TILE PATHS. Both are literal Windows paths -- "%s\\Cloud\\..."
//     -- built for an fopen. A backslash is a legal filename character on
//     Linux, so these do not fail as bad paths: they ask for one file
//     literally named `Earth\Cloud\05\000012\000034.dds`, miss, and report
//     the same "no tile" a body with no cloud data gives. The symptom would
//     be a planet whose clouds never sharpen past the base texture. Fifth
//     and sixth instances of the class recorded in the porting notes
//     (finding 24).
//
//  3. TWO DEAD LOCALS HAD TO GO. `int cfg` in CloudTile::Render and `char
//     dummy[MAX_PATH]` in InitHasIndividualFiles are written and never read.
//     MSVC's C4189 is off by default; GCC's -Wunused-variable is inside
//     -Wall. Both are commented out rather than deleted, so the reference's
//     shape is still visible at the point where it differed.
//
// The types are the usual list: LPDIRECT3DDEVICE9 -> VulkanDevice*,
// LPDIRECT3DTEXTURE9 -> VulkanTexture*, and SAFE_RELEASE of the preloaded
// texture becomes VulkanDevice::DestroyTexture -- see the note at that line.
// ==============================================================

#include "Cloudmgr2.h"
#include "VulkanCatalog.h"
#include "VulkanConfig.h"

// =======================================================================
// =======================================================================

CloudTile::CloudTile (TileManager2Base *_mgr, int _lvl, int _ilat, int _ilng)
: Tile (_mgr, _lvl, _ilat, _ilng)
{
	cmgr = static_cast<TileManager2<CloudTile>* > (_mgr);
	node = 0;
	imicrolvl = 6;	// Cloud micro resolution level
	cloudalt = mgr->GetPlanet()->prm.cloudalt;
	if (Config->TileMipmaps == 2) bMipmaps = true;
	if (Config->TileMipmaps == 1 && _lvl < 10) bMipmaps = true;
}

// -----------------------------------------------------------------------

CloudTile::~CloudTile ()
{
	if (tex && owntex) g_pTexmgr_tt->Free(tex);
}

// -----------------------------------------------------------------------

void CloudTile::PreLoad()
{
	assert(tex == nullptr);

	// Configure microtexture range for "Water texture" and "Cloud microtexture".
	GetParentMicroTexRange(&microrange);

	VulkanDevice  *pDev = mgr->Dev();
	VulkanTexture *pSysSrf = nullptr;

	if (cmgr->DoLoadIndividualFiles(0)) { // try loading from individual tile file
		char path[MAX_PATH];
		// "%s\\Cloud\\%02d\\%06d\\%06d.dds" on Windows. See point 2 in the
		// file header: this is a filesystem path, not a surface name, and
		// the separator is '/'.
		sprintf_s (path, MAX_PATH, "%s/Cloud/%02d/%06d/%06d.dds", mgr->DataRootDir().c_str(), lvl+4, ilat, ilng);
		LoadTextureFile(path, &pSysSrf);
	}
	if (!pSysSrf && cmgr->ZTreeManager(0)) { // try loading from compressed archive
		BYTE *buf;
		DWORD ndata = cmgr->ZTreeManager(0)->ReadData(lvl+4, ilat, ilng, &buf);
		if (ndata) {
			LoadTextureFromMemory(buf, ndata, &pSysSrf);
			cmgr->ZTreeManager(0)->ReleaseData(buf);
		}
	}

	owntex = true;

	if (CreateTexture(pDev, pSysSrf, &tex) != true) {
		if (GetParentSubTexRange(&texrange)) {
			tex = getParent()->Tex();
			owntex = false;
		}
		else tex = nullptr;
	}

	// Was SAFE_RELEASE(pSysSrf), dropping the last COM reference to the
	// D3DPOOL_SYSTEMMEM staging copy that CreateTexture has just uploaded
	// from. The staging texture is a VulkanTexture here and holds a VkImage
	// and a VkDeviceMemory, neither of which is reference counted -- so the
	// release is an explicit destroy against the device that made it.
	if (pSysSrf) pDev->DestroyTexture(pSysSrf);
}


// -----------------------------------------------------------------------

void CloudTile::Load ()
{
	
	bool shift_origin = (lvl >= 4);
	int res = mgr->GridRes();

	if (!lvl) {
		// create hemisphere mesh for western or eastern hemispheres
		mesh = CreateMesh_hemisphere (res, 0, cloudalt);
	//} else if (ilat == 0 || ilat == (1<<lvl)-1) {
		// create triangular patch for north/south pole region
	//	mesh = CreateMesh_tripatch (TILE_PATCHRES, elev, shift_origin, &vtxshift);
	} else {
		// create rectangular patch
		mesh = CreateMesh_quadpatch (res, res, 0, 1.0, cloudalt, &texrange, shift_origin, &vtxshift);
	}
}

// -----------------------------------------------------------------------

void CloudTile::Render()
{
	Tile::Render();

	VulkanDevice *pDev = mgr->Dev();
	vPlanet* vPlanet = mgr->GetPlanet();
	PlanetShader* pShader = mgr->GetShader();
	ShaderParams* sp = vPlanet->GetTerrainParams();

	// `int cfg = vPlanet->GetShaderID();` stood here and nothing in this
	// function reads it. See point 3 in the file header.
	//int cfg = vPlanet->GetShaderID();

	// ---------------------------------------------------------------------
	// Feed tile specific data to shaders
	//
	// ----------------------------------------------------------------------

	bool bTexture = (tex && vPlanet->HasTextures());

	if (bTexture)
	{
		pShader->SetTexture(pShader->tDiff, tex, IPF_ANISOTROPIC | IPF_CLAMP, Config->Anisotrophy);

		sp->vCloudOff = GetTexRangeDX(&texrange);
		sp->vMicroOff = GetTexRangeDX(&microrange);
		sp->fAlpha = 1.0f;
		sp->fBeta = 1.0f;
		sp->mWorld = mWorld;

		// -------------------------------------------------------------------
		// render surface mesh

		pShader->SetPSConstants(pShader->Prm, sp, sizeof(ShaderParams));
		pShader->SetVSConstants(pShader->PrmVS, sp, sizeof(ShaderParams));

		pShader->UpdateTextures();

		// SetStreamSource(0, mesh->pVB, 0, sizeof(VERTEX_2TEX)) +
		// SetIndices(mesh->pIB) + DrawIndexedPrimitive(TRIANGLELIST, 0, 0,
		// mesh->nv, 0, mesh->nf). See point 1 in the file header: the last
		// argument is a face count and vkCmdDrawIndexed wants indices.
		if (pDev->IsRecording() && mesh && mesh->pVB && mesh->pIB) {
			VkCommandBuffer cmd = pDev->GetCommandBuffer();
			VkBuffer vb = mesh->pVB->Buffer();
			VkDeviceSize offset = 0;
			vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
			vkCmdBindIndexBuffer(cmd, mesh->pIB->Buffer(), 0, VK_INDEX_TYPE_UINT16);
			vkCmdDrawIndexed(cmd, mesh->nf * 3, 1, 0, 0, 0);
		}
	}
}


// =======================================================================
// =======================================================================

template<>
void TileManager2<CloudTile>::Render (MATRIX4 &dwmat, bool use_zbuf, const vPlanet::RenderPrm &rprm)
{
	// set generic parameters
	SetRenderPrm (dwmat, rprm.cloudrot, use_zbuf, rprm);

	int i;
	class Scene *scene = GetClient()->GetScene();

	// adjust scaling parameters (can only be done if no z-buffering is in use)
	if (!use_zbuf) {
		double R = obj_size;
		double Rc = R+rprm.cloudalt;
		double D = prm.cdist*R;
		double zmin, zmax;
		if (D > Rc) {
			zmax = sqrt(D*D-Rc*Rc);
			zmin = D-Rc;
		} else {
			zmax = sqrt(D*D-R*R) + sqrt(Rc*Rc-R*R);
			zmin = Rc-D;
		}
		zmin = max (2.0, min (zmax*1e-4, zmin));

		vp->GetScatterConst()->mVP = scene->PushCameraFrustumLimits(zmin, zmax);
	}

	// build a transformation matrix for frustum testing
	MATRIX4 Mproj = _MATRIX4(scene->GetProjectionMatrix());
	Mproj.m33 = 1.0; Mproj.m43 = -1.0;  // adjust near plane to 1, far plane to infinity
	MATRIX4 Mview = _MATRIX4(scene->GetViewMatrix());
	prm.dviewproj = mul(Mview,Mproj);

	// ---------------------------------------------------------------------
	// Initialize shading technique and feed planet specific data to shaders
	//

	ElevMode = eElevMode::Spherical;
	ElevModeLvl = 0;

	ShaderParams* sp = vp->GetTerrainParams();
	FlowControlPS* fc = vp->GetFlowControl();
	fc->bBelowClouds = vp->CameraAltitude() < rprm.cloudalt;

	int cfg = vp->GetShaderID();

	// Select cloud layer shader
	pShader = (cfg == PLT_GIANT ? vp->GetShader(PLT_G_CLOUDS) : vp->GetShader(PLT_CLOUDS));

	// SetRenderState(D3DRS_CULLMODE, ...) was issued by
	// vPlanet::RenderCloudLayer before this call -- D3DCULL_NONE for the
	// layer seen from below, D3DCULL_CCW for the layer seen from above.
	// The cull is pipeline state here, so the value travels on the
	// manager and is applied where the pipeline is built. See
	// TileManager2Base::cullMode and ShaderClass::SetCullMode.
	pShader->SetCullMode(cullMode);
	pShader->Setup(pPatchVertexDecl, false, 1);
	pShader->ClearTextures();


	// Check Eclipse conditions -------------------------------------------
	//

	vp->InitEclipse(pShader);

	pShader->SetPSConstants("Const", vp->GetScatterConst(), sizeof(ConstParams));
	pShader->SetVSConstants("Const", vp->GetScatterConst(), sizeof(ConstParams));
	pShader->SetPSConstants("Flow", fc, sizeof(FlowControlPS));

	if (cfg != PLT_GIANT)
	{
		if (Config->CloudMicro) {
			pShader->SetTexture("tCloudMicro", hCloudMicro, IPF_ANISOTROPIC | IPF_WRAP, Config->Anisotrophy);
			if (Config->bCloudNormals)
				pShader->SetTexture("tCloudMicroNorm", hCloudMicroNorm, IPF_ANISOTROPIC | IPF_WRAP, Config->Anisotrophy);
		}
		pShader->SetTexture("tSun", vp->GetScatterTable(SUN_COLOR), IPF_LINEAR | IPF_CLAMP);
		pShader->SetTexture("tLndRay", vp->GetScatterTable(RAY_LAND), IPF_LINEAR | IPF_CLAMP);
		pShader->SetTexture("tLndAtn", vp->GetScatterTable(ATN_LAND), IPF_LINEAR | IPF_CLAMP);
		//pShader->SetTexture("tSkyRayColor", vp->GetScatterTable(RAY_COLOR), IPF_LINEAR | IPF_CLAMP);
		//pShader->SetTexture("tSkyMieColor", vp->GetScatterTable(MIE_COLOR), IPF_LINEAR | IPF_CLAMP);
	}

	// TODO: render full sphere for levels < 4

	loader->WaitForMutex();

	// update the tree
	for (i = 0; i < 2; i++)
		ProcessNode (tiletree+i);

	// render the tree
	for (i = 0; i < 2; i++)
		RenderNode (tiletree+i);
	
	loader->ReleaseMutex ();

	// Pop previous frustum configuration, must initialize mVP
	if (!use_zbuf)	vp->GetScatterConst()->mVP = scene->PopCameraFrustumLimits();

	// `sp` is fetched above and, on Windows as here, never read in this
	// function -- CloudTile::Render fetches it again for each tile. Kept,
	// and referenced here so that -Wunused-variable does not fire on a
	// declaration the reference makes.
	(void)sp;
}

// -----------------------------------------------------------------------

template<>
int TileManager2<CloudTile>::Coverage (double latmin, double latmax, double lngmin, double lngmax, int maxlvl, const Tile **tbuf, int nt) const
{
	double crot = GetPlanet()->prm.cloudrot;
	//double crot = prm.rprm->cloudrot;
	lngmin = lngmin + crot;
	lngmax = lngmax + crot;
	if (lngmin > PI) {
		lngmin -= PI2;
		lngmax -= PI2;
	}
	int nfound = 0;
	for (int i = 0; i < 2; i++) {
		CheckCoverage (tiletree+i, latmin, latmax, lngmin, lngmax, maxlvl, tbuf, nt, &nfound);
	}
	return nfound;
}

// -----------------------------------------------------------------------

template<>
void TileManager2<CloudTile>::LoadZTrees()
{
	treeMgr = new ZTreeMgr*[ntreeMgr = 1]();
	if (cprm.tileLoadFlags & 0x0002) {
		treeMgr[0] = ZTreeMgr::CreateFromFile(m_dataRootDir.c_str(), ZTreeMgr::LAYER_CLOUD);
	}
}

// -----------------------------------------------------------------------

template<>
void TileManager2<CloudTile>::InitHasIndividualFiles()
{
	hasIndividualFiles = new bool[ntreeMgr]();
	if (cprm.tileLoadFlags & 0x0001) {
		// `char dummy[MAX_PATH]` stood beside path and nothing writes or
		// reads it. See point 3 in the file header.
		char path[MAX_PATH];
		// "%s\\Cloud" on Windows -- a directory handed to FileExists. See
		// point 2 in the file header.
		sprintf_s(path, MAX_PATH, "%s/Cloud", m_dataRootDir.c_str());
		hasIndividualFiles[0] = FileExists(path);
	}
}

// -----------------------------------------------------------------------

template<>
Tile * TileManager2<CloudTile>::SearchTile (double lng, double lat, int maxlvl, bool bOwntex) const
{
	if (lng<0) return SearchTileSub(&tiletree[0], lng, lat, maxlvl, bOwntex);
	else	   return SearchTileSub(&tiletree[1], lng, lat, maxlvl, bOwntex);
}

// -----------------------------------------------------------------------

template<>
void TileManager2<CloudTile>::Unload(int lvl)
{
	tiletree[0].DelAbove(lvl);
	tiletree[1].DelAbove(lvl);
}
