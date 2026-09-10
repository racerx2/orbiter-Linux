// ==============================================================
// VBase.cpp
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2007-2026 Martin Schweiger
//				 2011 - 2016 Jarmo Nikkanen (D3D9Client modification)  
// ==============================================================

// ==============================================================
// class vBase (implementation)
//
// A vBase is the visual representation of a surface base
// object (a "spaceport" on the surface of a planet or moon,
// usually with runways or landing pads where vessels can
// land and take off.
// ==============================================================
//
// CONVERTED FROM OVP/D3D9Client/VBase.cpp, read end to end (557 lines).
//
// THE ONE HEADER THAT HAD TO GO IS <xnamath.h>, and it is worth being clear
// about what it was. It is DirectXMath (in its older Xbox/XNA spelling): a
// Windows SSE wrapper library, not part of Direct3D and not part of D3DX.
// There is no Linux counterpart because there is nothing to port TO -- it
// wraps the same _mm_* intrinsics GCC has had all along. It is used in
// exactly one function, CheckMeshStats, and for exactly one thing: a
// componentwise min and max over a vertex list. That is four lines of scalar
// code, so it is written out.
//
//   XMVECTOR / XMFLOAT3           -> FVECTOR3
//   XMLoadFloat3 / XMStoreFloat3  -> the load and store the casts already were
//   XMVectorMin / XMVectorMax     -> componentwise min / max
//
// The #pragma warning(push/disable:4838/pop) around the include goes with
// it: 4838 is MSVC's narrowing-conversion warning and it was suppressed for
// xnamath.h's own headers, not for any code here.
//
// CheckMeshStats and MeshStats HAVE NO CALLER ANYWHERE IN THE TREE, and
// cannot have one outside this file -- MeshStats is declared here, so no
// other translation unit can even spell the second argument. Converted
// rather than deleted, on the same principle as
// CSphereManager::CreateDeviceObjects: removing dead code is a separate
// decision from converting it. Its `rad` is computed from max+min rather
// than max-min, which makes it the distance from the origin to the box
// centre rather than a radius -- carried over as written, because it is not
// a platform difference and nothing reads it.
//
// The rest of the file is the usual list, and every one of them is a type:
//
//   D3D9Mesh            -> VulkanMesh
//   D3D9Sun             -> VulkanSun
//   LPDIRECT3DDEVICE9   -> VulkanDevice *
//   D3DXVECTOR3/4       -> FVECTOR3 / FVECTOR4
//   D3DXMATRIX          -> FMATRIX4
//   D3DXMatrixIdentity  -> VMAT_Identity        (returns void, not a pointer,
//                                                so the call moves out of the
//                                                argument list it was nested in)
//   D3DMAT_SetRotation  -> VMAT_SetRotation
//   D3DXMatrixMultiply  -> VMAT_MatrixMultiply
//   D3DXVec3TransformNormal -> oapi::TransformNormal
//   D3DXVec3Dot / Length-> oapi::dot / oapi::length
//   D3DXVEC             -> FVEC
//   mProj._11.._44      -> mProj.m11..m44       (DrawAPI.h guards the _11 view
//                                                with #ifdef _WIN32; the m**
//                                                names are the same storage)
//
// D9ComputeMinMaxDistance LOSES ITS DEVICE ARGUMENT at all three call sites.
// That is not this file's decision: AABBUtil.cpp's own body never mentions
// the device, and there is no IDirect3DDevice9 to pass. See AABBUtil.h.
//
// Two `if (assignment)` conditions get their parentheses, for the same
// reason they did in CSphereMgr.cpp and TileMgr.cpp: -Wparentheses is inside
// -Wall and MSVC's C4706 is off by default.
// ==============================================================

#include "VBase.h"
#include "TileMgr.h"
#include "VulkanClient.h"
#include "VulkanSurface.h"
#include "BeaconArray.h"
#include "RunwayLights.h"
#include "AABBUtil.h"
#include "OrbiterAPI.h"
#include "Scene.h"
#include "DebugControls.h"
#include "VulkanConfig.h"
#include "VPlanet.h"

typedef struct {
	float rad;
	float width, length, height;
	FVECTOR3 pos, min, max;
} MeshStats;


void CheckMeshStats(MESHHANDLE hMesh, MeshStats *stats)
{
	int nGrp = oapiMeshGroupCount(hMesh);
	if (nGrp == 0) return;

	// XMLoadFloat3(ptr(XMFLOAT3(1e12f,1e12f,1e12f))) and its negation. See
	// the file header: this is the whole of what xnamath.h was here for.
	FVECTOR3 mi(1e12f, 1e12f, 1e12f);
	FVECTOR3 mx = -mi;

	for (int i = 0; i < nGrp; i++) {

		MESHGROUPEX *grp = oapiMeshGroupEx(hMesh, i);
		
		for (DWORD v = 0; v < grp->nVtx; v++) {
			// XMLoadFloat3((XMFLOAT3*)&grp->Vtx[v].x) read the three floats
			// at the head of an NTVERTEX -- the position -- and nothing else.
			FVECTOR3 x(grp->Vtx[v].x, grp->Vtx[v].y, grp->Vtx[v].z);
			// XMVectorMin / XMVectorMax, componentwise.
			if (x.x < mi.x) mi.x = x.x;
			if (x.y < mi.y) mi.y = x.y;
			if (x.z < mi.z) mi.z = x.z;
			if (x.x > mx.x) mx.x = x.x;
			if (x.y > mx.y) mx.y = x.y;
			if (x.z > mx.z) mx.z = x.z;
		}
	}

	stats->min = mi;
	stats->max = mx;

	stats->width = stats->max.x - stats->min.x;
	stats->height = stats->max.y - stats->min.y;
	stats->length = stats->max.z - stats->min.z;
	stats->pos = (stats->max + stats->min) * 0.5f;
	stats->rad = length(stats->max + stats->min) * 0.5f;
}



vBase::vBase (OBJHANDLE _hObj, const Scene *scene, vPlanet *_vP): vObject (_hObj, scene)
{
	_TRACE;
	DWORD i,j;

	vP = _vP;
	hPlanet = oapiGetBasePlanet(hObj);

	if (!vP) vP = static_cast<vPlanet*>( scene->GetVisObject(hPlanet) );
	
	structure_bs	= NULL;
	structure_as	= NULL;
	nstructure_bs	= 0;
	nstructure_as	= 0;
	tspec			= NULL;
	tilemesh		= NULL;
	numRunwayLights = 0;
	numTaxiLights   = 0;
	runwayLights    = NULL;
	taxiLights		= NULL;
	csun_lights     = RAD * Config->SunAngle;

	// ----------------------------------------------------------------------
	// Compute transformations from local base frame to planet frame and back
	//
	MATRIX3 plrot; VECTOR3 relpos;
	oapiGetRotationMatrix(hPlanet, &plrot);
	oapiGetRotationMatrix(hObj, &mGlobalRot);
	oapiGetRelativePos(hObj, hPlanet, &relpos);
	vLocalPos = tmul(plrot, relpos);

	swap(plrot.m12, plrot.m21);
	swap(plrot.m13, plrot.m31);
	swap(plrot.m23, plrot.m32);

	mGlobalRot = mul(plrot, mGlobalRot);

	VMAT_Identity(&mGlobalRotDX);
	VMAT_SetRotation(&mGlobalRotDX, &mGlobalRot);
	//------------------------------------------------------------------------

	// load surface tiles
	DWORD _ntile = gc->GetBaseTileList (_hObj, &tspec);
	ntile = 0;
	for (i=0; i<_ntile; ++i) {
		// Only count (render) tiles where bit0 is set!
		if (tspec[i].texflag & 0x01) {
			++ntile;
		}
	}

	// Do not render tiles for planets having a new tile format
	if (vP->tilever >= 2) ntile = 0;

	if (ntile) {

		MESHGROUPEX **grps = new MESHGROUPEX*[ntile];
		SURFHANDLE *texs = new SURFHANDLE[ntile];

		for (i = 0, j = 0; i < _ntile; ++i) {
			// Only render tiles where bit0 is set!
			if (tspec[i].texflag & 0x01) {
				DWORD ng = oapiMeshGroupCount(tspec[i].mesh);
				if (ng!=1) LogErr("MeshGroup Count = %u",ng);
				else {
					texs[j] = tspec[i].tex;
					grps[j] = oapiMeshGroupEx(tspec[i].mesh, 0);
				}
				++j;
			}
		}
		tilemesh = new VulkanMesh(ntile, (const MESHGROUPEX**)grps, texs);
		delete []grps;
		grps = NULL;
        delete []texs;
		texs = NULL;
	}

	// load meshes for generic structures
	MESHHANDLE *sbs, *sas;
	DWORD nsbs, nsas;
	gc->GetBaseStructures (_hObj, &sbs, &nsbs, &sas, &nsas);

	if ((nstructure_bs = nsbs) != 0) {
		structure_bs = new VulkanMesh*[nsbs];
		for (i = 0; i < nsbs; i++) structure_bs[i] = new VulkanMesh(sbs[i]);
	}

	if ((nstructure_as = nsas) != 0) {
		structure_as = new VulkanMesh*[nsas];
		for (i = 0; i < nsas; i++) structure_as[i] = new VulkanMesh(sas[i]);
	}

	lights = false;
	Tchk = Tlghtchk = oapiGetSimTime()-1.0;

	UpdateBoundingBox();
	
	char name[64];
	oapiGetObjectName(_hObj, name, 64);
	LogAlw("New Base Visual(%s) %s hBase=%s, nsbs=%u, nsas=%u", _PTR(this), name, _PTR(_hObj), nsbs, nsas);

	CreateRunwayLights();
	CreateTaxiLights();
}


// ===========================================================================================
//
VECTOR3 vBase::ToLocal(VECTOR3 pos, double *lng, double *lat) const
{
	double rad;
	VECTOR3 vLoc = mul(mGlobalRot, pos) + vLocalPos;
	if (lng && lat) oapiLocalToEqu(hPlanet, vLoc, lng, lat, &rad);
	return vLoc;
}


// ===========================================================================================
//
VECTOR3 vBase::FromLocal(VECTOR3 pos) const
{
	return tmul(mGlobalRot, pos-vLocalPos);
}

// ===========================================================================================
//
void vBase::FromLocal(VECTOR3 pos, FVECTOR3 *pTgt) const
{
	FVECTOR3 pv(float(pos.x-vLocalPos.x), float(pos.y-vLocalPos.y), float(pos.z-vLocalPos.z));
	// D3DXVec3TransformNormal(out, in, mat) -- the 3x3 part of the matrix
	// applied to a direction, with no translation. Same rotation, same
	// row-vector convention.
	*pTgt = oapi::TransformNormal(pv, mGlobalRotDX);
}

// ===========================================================================================
//
double vBase::GetElevation() const
{
	VECTOR3 bp;
	oapiGetRelativePos(hObj, hPlanet, &bp);
	return length(bp) - oapiGetSize(hPlanet);
}


// ===========================================================================================
//
void vBase::CreateRunwayLights()
{
	const char *file = oapiGetObjectFileName(hObj);
	if (file) numRunwayLights = RunwayLights::CreateRunwayLights(this, scn, file, runwayLights);
	else LogErr("Configuration file not found for object %s", _PTR(hObj));
}

// ===========================================================================================
//
void vBase::CreateTaxiLights()
{
	const char *file = oapiGetObjectFileName(hObj);
	if (file) numTaxiLights = TaxiLights::CreateTaxiLights(hObj, scn, file, taxiLights);
	else LogErr("Configuration file not found for object %s", _PTR(hObj));
}

// ===========================================================================================
//
vBase::~vBase ()
{
	DWORD i;

	if (tilemesh) delete tilemesh;

	if (nstructure_bs) {
		for (i = 0; i < nstructure_bs; i++)	delete structure_bs[i];
		delete []structure_bs;
		structure_bs = NULL;
	}
	if (nstructure_as) {
		for (i = 0; i < nstructure_as; i++)	delete structure_as[i];
		delete []structure_as;
		structure_as = NULL;
	}

	if (runwayLights) {
		for(i=0; i<(DWORD)numRunwayLights; i++)
		{
			SAFE_DELETE(runwayLights[i]);
		}
		delete[] runwayLights;
		runwayLights = NULL;
	}

	if (taxiLights) {
		for(i=0; i<(DWORD)numTaxiLights; i++)
		{
			SAFE_DELETE(taxiLights[i]);
		}
		delete[] taxiLights;
	}

	if (DebugControls::IsActive()) {
		DebugControls::RemoveVisual(this);
	}
}

// ===========================================================================================
//
DWORD vBase::GetMeshCount()
{
	if (tilemesh) return nstructure_bs + nstructure_as + 1;
	else          return nstructure_bs + nstructure_as;
}

// ===========================================================================================
//
bool vBase::GetMinMaxDistance(float *zmin, float *zmax, float *dmin)
{
	if (bBSRecompute) UpdateBoundingBox();
	
	FMATRIX4 mWorldView;

	Scene *scn = gc->GetScene();

	FVECTOR4 Field = D9LinearFieldOfView(scn->GetProjectionMatrix());

	VMAT_MatrixMultiply(&mWorldView, &mWorld, scn->GetViewMatrix());

	// The device argument is gone from all three calls -- see the note in the
	// file header and in AABBUtil.h.
	if (tilemesh) {
		D9ComputeMinMaxDistance(tilemesh->GetAABB(), &mWorldView, &Field, zmin, zmax, dmin);
	}

	if (nstructure_bs) {
		for (DWORD i = 0; i < nstructure_bs; i++) {
			D9ComputeMinMaxDistance(structure_bs[i]->GetAABB(), &mWorldView, &Field, zmin, zmax, dmin);
		}
	}

	if (nstructure_as) {
		for (DWORD i = 0; i < nstructure_as; i++) {
			D9ComputeMinMaxDistance(structure_as[i]->GetAABB(), &mWorldView, &Field, zmin, zmax, dmin);
		}
	}

	return true;
}


// ===========================================================================================
//
void vBase::UpdateBoundingBox()
{
	bBSRecompute = false;
	
	if (tilemesh || nstructure_bs || nstructure_as) D9InitAABB(&BBox);
	else D9ZeroAABB(&BBox);

	
	if (tilemesh) D9AddAABB(tilemesh->GetAABB(), NULL, &BBox);
		
	if (nstructure_bs) {
		for (DWORD i = 0; i < nstructure_bs; i++) {
			D9AddAABB(structure_bs[i]->GetAABB(), NULL, &BBox);
		}
	}

	if (nstructure_as) {
		for (DWORD i = 0; i < nstructure_as; i++) {
			D9AddAABB(structure_as[i]->GetAABB(), NULL, &BBox);
		}
	}

	D9UpdateAABB(&BBox);
}


// ===========================================================================================
//
bool vBase::Update (bool bMainScene)
{
	_TRACE;
	if (!active) return false;
	if (!vObject::Update(bMainScene)) return false;

	double simt = oapiGetSimTime();

	if (fabs(simt-Tlghtchk)>0.1 || oapiGetPause()) {
		VECTOR3 rpos = gpos - vP->GlobalPos();
		sunLight = vP->GetObjectAtmoParams(rpos);
		Tlghtchk = simt;
	}

	if (fabs(simt-Tchk)>1.0) {
		VECTOR3 pos, sdir;
		MATRIX3 rot;
		oapiGetGlobalPos (hObj, &pos); normalise(pos);
		oapiGetRotationMatrix (hObj, &rot);
		sdir = tmul (rot, -pos);
		double csun = sdir.y;
		bool night = csun < csun_lights;
		if (lights != night) {
			DWORD i;
			for (i = 0; i < nstructure_bs; i++)	structure_bs[i]->SetTexMixture (1, night ? 1.0f:0.0f);
			for (i = 0; i < nstructure_as; i++)	structure_as[i]->SetTexMixture (1, night ? 1.0f:0.0f);
			lights = night;
		}
		Tchk = simt;
	}
	return true;
}


// ===========================================================================================
//
bool vBase::RenderSurface(VulkanDevice *dev)
{
	// note: assumes z-buffer disabled
	if (!active) return false;
	if (!IsVisible()) return false;

	pCurrentVisual = this;

	// render tiles
	if (tilemesh) {
		uCurrentMesh = 0; // Used for debugging
		tilemesh->SetSunLight(&sunLight);
		tilemesh->RenderBaseTile(&mWorld);
		++uCurrentMesh;
	}

	// render generic objects under shadows
	if (nstructure_bs) {
		for (DWORD i = 0; i < nstructure_bs; ++i) {
			structure_bs[i]->SetSunLight(&sunLight);
			structure_bs[i]->Render(&mWorld, RENDER_BASEBS);
			++uCurrentMesh;
		}
	}

	return true;
}


// ===========================================================================================
//
bool vBase::RenderStructures(VulkanDevice *dev)
{
	if (!active) return false;
	if (!IsVisible()) return false;

	pCurrentVisual = this;
	uCurrentMesh = 0; // Used for debugging

	if (tilemesh) uCurrentMesh++;
	uCurrentMesh += nstructure_bs;

	// render generic objects above shadows
	for (DWORD i=0; i<nstructure_as; i++) {
		FVECTOR3 bs = structure_as[i]->GetBoundingSpherePos();
		FVECTOR3 qw = TransformCoord(bs, mWorld);
		VulkanSun sp = vP->GetObjectAtmoParams(qw._V() + vP->CameraPos());
		structure_as[i]->SetSunLight(&sp);
		structure_as[i]->Render(&mWorld, RENDER_BASE);
		++uCurrentMesh;
	}
	return true;
}


// ===========================================================================================
//
void vBase::RenderRunwayLights(VulkanDevice *dev)
{
	if (!active) return;
	if (!IsVisible()) return;

	pCurrentVisual = this;

	for(int i=0; i<numRunwayLights; i++)
	{
		if (scn->GetRenderPass() == RENDERPASS_MAINSCENE) runwayLights[i]->Update(vP);
		runwayLights[i]->Render(dev, &mWorld, lights);
	}

	for(int i=0; i<numTaxiLights; i++)
	{
		taxiLights[i]->Render(dev, &mWorld, lights);
	}
	
	if (DebugControls::IsActive()) {
		DWORD flags = *(DWORD*)gc->GetConfigParam(CFGPRM_GETDEBUGFLAGS);
		if (flags&DBG_FLAGS_SELVISONLY && this!=DebugControls::GetVisual()) return; // Used for debugging
		if (flags&DBG_FLAGS_BOXES) {
			// D3DXMatrixIdentity returned its argument, so the Windows call
			// nested it inside the argument list. VMAT_Identity returns void
			// -- the identity is built first and then passed. The colour
			// literals say `1.0f` rather than `1`: FVECTOR4 has all-float,
			// all-int and all-double constructors where D3DXVECTOR4 had one,
			// so a mixed call is ambiguous rather than silently converted.
			FMATRIX4 id;
			VMAT_Identity(&id);
			VulkanEffect::RenderBoundingBox(&mWorld, &id, &BBox.min, &BBox.max, ptr(FVECTOR4(1.0f, 0.0f, 1.0f, 0.75f)));
		}
	}
}


// ===========================================================================================
//
void vBase::RenderGroundShadow(VulkanDevice *dev, float alpha)
{
	if (!nstructure_as) return; // nothing to do
	if (!active) return;
	if (!IsVisible()) return;
	if (Config->TerrainShadowing == 0) return;

	pCurrentVisual = this;

	VECTOR3 sd;
	oapiGetGlobalPos(hObj, &sd); normalise(sd);
	
	MATRIX3 mRot;
	oapiGetRotationMatrix(hObj, &mRot);
	FVECTOR3 lsun = FVEC(tmul(mRot, sd));

	if (lsun.y > -0.07f) return;

	float scale = (-lsun.y - 0.07f) * 25.0f;
	scale = (1.0f - alpha) * saturate(scale);
	

	// build shadow projection matrix
	FMATRIX4 mProj;
	
	OBJHANDLE hPlanet = oapiGetBasePlanet(hObj); 
	double prad = oapiGetSize(hPlanet);
	FVECTOR4 param = D9OffsetRange(prad, 30e3);

	for (DWORD i=0; i<nstructure_as; i++) {
		
		if (structure_as[i]->HasShadow()) {

			double a, b, el0, el1, el2;
			double d = atan(1.0 / prad);
			VECTOR3 va, vb, vc;
			VECTOR3 q = _V(structure_as[i]->BBox.bs);
			float rad = structure_as[i]->BBox.bs.w;

			if (rad<250.0f) ToLocal(q, &a, &b);	
			else ToLocal(_V(0,0,0), &a, &b);

			if (vP->GetElevation(a, b, &el0) <= 0) el0 = oapiSurfaceElevation(hPlanet, a, b);
			if (vP->GetElevation(a + d, b, &el1) <= 0) el1 = oapiSurfaceElevation(hPlanet, a + d, b);
			if (vP->GetElevation(a, b + d, &el2) <= 0) el2 = oapiSurfaceElevation(hPlanet, a, b + d);

			oapiEquToLocal(hPlanet, a, b, el0 + prad, &va);
			oapiEquToLocal(hPlanet, a + d, b, el1 + prad, &vb);
			oapiEquToLocal(hPlanet, a, b + d, el2 + prad, &vc);

			va = FromLocal(va);
			vb = FromLocal(vb);
			vc = FromLocal(vc);
				
			VECTOR3 n = -unit(crossp(vb - va, vc - va));

			FVECTOR3 hn = FVEC(n);
				
			float zo = float(-dotp(va, n));
			float nd = dot(hn, lsun);
			hn /= nd;
			float ofs = zo / nd;

			// _11.._44 is the D3DXMATRIX field naming. DrawAPI.h declares
			// that view only under #ifdef _WIN32, because GCC rejects a
			// member with a user-declared constructor inside an anonymous
			// union member -- so the m** names, which are the same storage
			// and are always available, are used instead.
			mProj.m11 = 1.0f - (float)(lsun.x*hn.x);
			mProj.m12 = -(float)(lsun.y*hn.x);
			mProj.m13 = -(float)(lsun.z*hn.x);
			mProj.m14 = 0;
			mProj.m21 = -(float)(lsun.x*hn.y);
			mProj.m22 = 1.0f - (float)(lsun.y*hn.y);
			mProj.m23 = -(float)(lsun.z*hn.y);
			mProj.m24 = 0;
			mProj.m31 = -(float)(lsun.x*hn.z);
			mProj.m32 = -(float)(lsun.y*hn.z);
			mProj.m33 = 1.0f - (float)(lsun.z*hn.z);
			mProj.m34 = 0;
			mProj.m41 = -(float)(lsun.x*ofs);
			mProj.m42 = -(float)(lsun.y*ofs);
			mProj.m43 = -(float)(lsun.z*ofs);
			mProj.m44 = 1;
				
			FVECTOR4 nrml = FVECTOR4(float(n.x), float(n.y), float(n.z), zo);

			structure_as[i]->RenderShadowsEx(scale, &mProj, &mWorld, &nrml, &param);
		}
	}
}
