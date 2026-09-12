// ==============================================================
//   ORBITER VISUALISATION PROJECT (OVP)
//   Copyright (C) 2006-2026 Martin Schweiger
//   Dual licensed under GPL v3 and LGPL v3
// ==============================================================

// ==============================================================
// spherepatch.cpp
// Create meshes for spheres and sphere patches
// ==============================================================
//
// The Windows file spells its include "TileMgr2.h"; the file on disk has
// always been Tilemgr2.h. NTFS did not care and ext4 does.
// ==============================================================

#include "Spherepatch.h"
#include "AABBUtil.h"
#include "Tilemgr2.h"

static float TEX2_MULTIPLIER = 4.0f; // microtexture multiplier

// ==============================================================
// struct VBMESH

// Both initialiser lists reordered to declaration order (-Wreorder). Every
// value is a constant, so nothing observable changes.
VBMESH::VBMESH (class TileManager2Base *pmgr)
	: pVB(NULL)
	, pIB(NULL)
	, vtx(NULL)
	, idx(NULL)
	, nv(0)
	, nf(0)
	, nv_cur(0)
	, nf_cur(0)
	, bsRad(0.0)
	, bBox(false)
{
}

VBMESH::VBMESH ()
	: pVB(NULL)
	, pIB(NULL)
	, vtx(NULL)
	, idx(NULL)
	, nv(0)
	, nf(0)
	, nv_cur(0)
	, nf_cur(0)
	, bsRad(0.0)
	, bBox(false)
{
}

VBMESH::~VBMESH ()
{
	if (pVB) g_pVtxmgr_vb->Free(pVB);
	if (pIB) g_pIdxmgr_ib->Free(pIB);
	if (vtx) g_pMemgr_vtx->Free(vtx);
	if (idx) g_pMemgr_w->Free(idx);
	nv_cur = 0;
	nf_cur = 0;
}

void VBMESH::ComputeSphere()
{
	// D3DXVec3Length is a D3DX utility entry point with no counterpart, so the
	// length is written out.
	bsCnt = FVECTOR3(float(Box[0].x + Box[7].x), float(Box[0].y + Box[7].y), float(Box[0].z + Box[7].z)) * 0.5f;
	const FVECTOR3 half = FVECTOR3(float(Box[0].x - Box[7].x), float(Box[0].y - Box[7].y), float(Box[0].z - Box[7].z)) * 0.5f;
	bsRad = sqrt(half.x*half.x + half.y*half.y + half.z*half.z);
}


// ==============================================================
// D3DXComputeBoundingSphere, written out: a D3DX CPU utility with no Vulkan
// counterpart. Its centre is the midpoint of the axis-aligned extents,
// (min + max) * 0.5 per axis, not the average of the points, and the radius is
// the greatest distance from that centre to any point. The two differ whenever
// the vertices are not evenly spread, and a sphere patch's are not; using the
// centroid gives a plausible, slightly too small sphere that culls tiles which
// are actually visible.
//
static void ComputeBoundingSphere (const VERTEX_2TEX *pVtx, DWORD nVtx, FVECTOR3 *pCentre, float *pRadius)
{
	if (!pVtx || !nVtx) {
		*pCentre = FVECTOR3(0.0f, 0.0f, 0.0f);
		*pRadius = 0.0f;
		return;
	}

	float minx = pVtx[0].x, maxx = pVtx[0].x;
	float miny = pVtx[0].y, maxy = pVtx[0].y;
	float minz = pVtx[0].z, maxz = pVtx[0].z;

	for (DWORD i = 1; i < nVtx; i++) {
		if      (pVtx[i].x < minx) minx = pVtx[i].x;
		else if (pVtx[i].x > maxx) maxx = pVtx[i].x;
		if      (pVtx[i].y < miny) miny = pVtx[i].y;
		else if (pVtx[i].y > maxy) maxy = pVtx[i].y;
		if      (pVtx[i].z < minz) minz = pVtx[i].z;
		else if (pVtx[i].z > maxz) maxz = pVtx[i].z;
	}

	const FVECTOR3 c((minx + maxx) * 0.5f, (miny + maxy) * 0.5f, (minz + maxz) * 0.5f);

	float r2 = 0.0f;
	for (DWORD i = 0; i < nVtx; i++) {
		const float dx = pVtx[i].x - c.x;
		const float dy = pVtx[i].y - c.y;
		const float dz = pVtx[i].z - c.z;
		const float d2 = dx*dx + dy*dy + dz*dz;
		if (d2 > r2) r2 = d2;
	}

	*pCentre = c;
	*pRadius = sqrt(r2);
}


void VBMESH::MapVertices(VulkanDevice *pDev, DWORD MemFlag)
{
	if (nv!=nv_cur && vtx) {
		pVB = g_pVtxmgr_vb->New(nv); nv_cur = nv;
	}

	if (nf!=nf_cur && idx) {
		pIB = g_pIdxmgr_ib->New(nf); nf_cur = nf;
	}

	VERTEX_2TEX *pVBuffer;
	WORD *pIBuffer;

	if (vtx) {

		ComputeBoundingSphere(vtx, nv, &bsCnt, &bsRad);

		if (pVB) {
			// D3DLOCK_DISCARD has no equivalent and needs none: these buffers
			// are host-visible, so Map() returns the memory itself and there
			// is no previous content to stall on.
			if ((pVBuffer = (VERTEX_2TEX *)pVB->Map()) != NULL) {
				memcpy(pVBuffer, vtx, nv*sizeof(VERTEX_2TEX));
				pVB->Unmap();
			}
		} else LogErr("Failed to create vertex buffer");
	}

	if (idx) {
		if (pIB) {
			if ((pIBuffer = (WORD *)pIB->Map()) != NULL) {
				memcpy(pIBuffer, idx, nf*sizeof(WORD)*3);
				pIB->Unmap();
			}
		} else LogErr("Failed to create index buffer");
	}
}


// ==============================================================
// CreateSphere()
// Create a spherical mesh of radius 1 and resolution defined by nrings
// Below is a list of #vertices and #indices against nrings:
//
// nrings  nvtx   nidx   (nidx = 12 nrings^2)
//   4       38    192
//   6       80    432
//   8      138    768
//  12      302   1728
//  16      530   3072
//  20      822   4800
//  24     1178   6912

void CreateSphere (VulkanDevice *pDev, VBMESH &mesh, DWORD nrings, bool hemisphere, int which_half, int texres)
{
	// Allocate memory for the vertices and indices
	DWORD       nVtx = hemisphere ? nrings*(nrings+1)+2 : nrings*(2*nrings+1)+2;
	DWORD       nIdx = hemisphere ? 6*nrings*nrings : 12*nrings*nrings;
	VERTEX_2TEX* Vtx = g_pMemgr_vtx->New(nVtx);
	WORD*        Idx = g_pMemgr_w->New(nIdx);

	// Counters
    WORD x, y, nvtx = 0, nidx = 0;
	VERTEX_2TEX *vtx = Vtx;
	WORD *idx = Idx;

	// Angle deltas for constructing the sphere's vertices
    FLOAT fDAng   = (FLOAT)PI / nrings;
    FLOAT fDAngY0 = fDAng;
	DWORD x1 = (hemisphere ? nrings : nrings*2);
	DWORD x2 = x1+1;
	FLOAT du = 0.5f/(FLOAT)texres;
	FLOAT a  = (1.0f-2.0f*du)/(FLOAT)x1;

    // Make the middle of the sphere
    for (y = 0; y < nrings; y++) {
        FLOAT y0 = (FLOAT)cos(fDAngY0);
        FLOAT r0 = (FLOAT)sin(fDAngY0);
		FLOAT tv = fDAngY0/(FLOAT)PI;

        for (x = 0; x < x2; x++) {
            FLOAT fDAngX0 = x*fDAng - (FLOAT)PI;  // subtract Pi to wrap at +-180 deg
			if (hemisphere && which_half) fDAngX0 += (FLOAT)PI;

			FVECTOR3 v = {r0*(FLOAT)cos(fDAngX0), y0, r0*(FLOAT)sin(fDAngX0)};
			FLOAT tu = a*(FLOAT)x + du;
			//FLOAT tu = x/(FLOAT)x1;

            *vtx++ = VERTEX_2TEX (v, v, tu, tv, tu, tv);
			nvtx++;
        }
        fDAngY0 += fDAng;
    }

    for (y = 0; y < nrings-1; y++) {
        for (x = 0; x < x1; x++) {
            *idx++ = (WORD)( (y+0)*x2 + (x+0) );
            *idx++ = (WORD)( (y+0)*x2 + (x+1) );
            *idx++ = (WORD)( (y+1)*x2 + (x+0) );
            *idx++ = (WORD)( (y+0)*x2 + (x+1) );
            *idx++ = (WORD)( (y+1)*x2 + (x+1) );
            *idx++ = (WORD)( (y+1)*x2 + (x+0) ); 
			nidx += 6;
        }
    }
    // Make top and bottom
	FVECTOR3 pvy = {0, 1, 0}, nvy = {0,-1,0};
	WORD wNorthVtx = nvtx;
    *vtx++ = VERTEX_2TEX (pvy, pvy, 0.5f, 0.0f, 0.5f, 0.0f);
    nvtx++;
	WORD wSouthVtx = nvtx;
    *vtx++ = VERTEX_2TEX (nvy, nvy, 0.5f, 1.0f, 0.5f, 1.0f);
    nvtx++;

    for (x = 0; x < x1; x++) {
		WORD p1 = wSouthVtx;
		WORD p2 = (WORD)( (y)*x2 + (x+0) );
		WORD p3 = (WORD)( (y)*x2 + (x+1) );

        *idx++ = p1;
        *idx++ = p3;
        *idx++ = p2;
		nidx += 3;
    }

    for (x = 0; x < x1; x++) {
		WORD p1 = wNorthVtx;
		WORD p2 = (WORD)( (0)*x2 + (x+0) );
		WORD p3 = (WORD)( (0)*x2 + (x+1) );

        *idx++ = p1;
        *idx++ = p3;
        *idx++ = p2;
		nidx += 3;
    }

	mesh.nv  = nVtx;
	mesh.nf  = nIdx/3;
	mesh.vtx = Vtx;
	mesh.idx = Idx;
	mesh.MapVertices(pDev);
	
	if (Vtx) g_pMemgr_vtx->Free(Vtx);
	if (Idx) g_pMemgr_w->Free(Idx);

	Vtx = NULL;
	Idx = NULL;
	mesh.vtx = NULL;
	mesh.idx = NULL;
}

// ==============================================================

void CreateSpherePatch (VulkanDevice *pDev, VBMESH &mesh, int nlng, int nlat, int ilat, int res, int bseg,
	bool reduce, bool outside, bool store_vtx, bool shift_origin)
{

	const float c1 = 1.0f, c2 = 0.0f;
	int i, j, nVtx, nIdx, nseg, n, nofs0, nofs1;
	double minlat, maxlat, lat, minlng, maxlng, lng;
	double slat, clat, slng, clng;
	WORD tmp;
	VECTOR3 pos, tpos;

	minlat = PI*0.5 * (double)ilat/(double)nlat;
	maxlat = PI*0.5 * (double)(ilat+1)/(double)nlat;
	minlng = 0;
	maxlng = PI*2.0/(double)nlng;
	if (bseg < 0 || ilat == nlat-1) bseg = (nlat-ilat)*res;

	// generate nodes
	nVtx = (bseg+1)*(res+1);
	if (reduce) nVtx -= ((res+1)*res)/2;
	VERTEX_2TEX *Vtx = g_pMemgr_vtx->New(nVtx);

	// create transformation for bounding box
	// we define the local coordinates for the patch so that the x-axis points
	// from (minlng,minlat) corner to (maxlng,minlat) corner (origin is halfway between)
	// y-axis points from local origin to middle between (minlng,maxlat) and (maxlng,maxlat)
	// bounding box is created in this system and then transformed back to planet coords.
	double clat0 = cos(minlat), slat0 = sin(minlat);
	double clng0 = cos(minlng), slng0 = sin(minlng);
	double clat1 = cos(maxlat), slat1 = sin(maxlat);
	double clng1 = cos(maxlng), slng1 = sin(maxlng);
	VECTOR3 ex = {clat0*clng1 - clat0*clng0, 0, clat0*slng1 - clat0*slng0}; normalise(ex);
	VECTOR3 ey = {0.5*(clng0+clng1)*(clat1-clat0), slat1-slat0, 0.5*(slng0+slng1)*(clat1-clat0)}; normalise(ey);
	VECTOR3 ez = crossp (ey, ex);
	MATRIX3 R = {ex.x, ex.y, ex.z,  ey.x, ey.y, ey.z,  ez.x, ez.y, ez.z};
	VECTOR3 pref = {0.5*(clat0*clng1 + clat0*clng0), slat0, 0.5*(clat0*slng1 + clat0*slng0)}; // origin
	VECTOR3 tpmin, tpmax; 

	// Read below only when shift_origin is set, which is the condition that
	// writes them; GCC cannot see that and warns. The Windows build left them
	// indeterminate.
	float dx = 0.0f, dy = 0.0f;
	if (shift_origin) {
		dx = (float)clat0;
		dy = (float)slat0;
	}

	for (i = n = 0; i <= res; i++) {  // loop over longitudinal strips
		lat = minlat + (maxlat-minlat) * (double)i/(double)res;
		slat = sin(lat), clat = cos(lat);
		nseg = (reduce ? bseg-i : bseg);
		for (j = 0; j <= nseg; j++) {
			lng = (nseg ? minlng + (maxlng-minlng) * (double)j/(double)nseg : 0.0);
			slng = sin(lng), clng = cos(lng);
			pos = _V(clat*clng, slat, clat*slng);
			tpos = mul (R, pos-pref);
			if (!n) {
				tpmin = tpos;
				tpmax = tpos;
			} else {
				if      (tpos.x < tpmin.x) tpmin.x = tpos.x;
			    else if (tpos.x > tpmax.x) tpmax.x = tpos.x;
				if      (tpos.y < tpmin.y) tpmin.y = tpos.y;
				else if (tpos.y > tpmax.y) tpmax.y = tpos.y;
				if      (tpos.z < tpmin.z) tpmin.z = tpos.z;
				else if (tpos.z > tpmax.z) tpmax.z = tpos.z;
			}

			Vtx[n].x = Vtx[n].nx = FVAL(pos.x);
			Vtx[n].y = Vtx[n].ny = FVAL(pos.y);
			Vtx[n].z = Vtx[n].nz = FVAL(pos.z);
			if (shift_origin)
				Vtx[n].x -= dx, Vtx[n].y -= dy;

			Vtx[n].tu0 = FVAL(nseg ? (c1*j)/nseg+c2 : 0.5f); // overlap to avoid seams
			Vtx[n].tv0 = FVAL((c1*(res-i))/res+c2);
			//Vtx[n].tu1 = (nseg ? Vtx[n].tu0 * TEX2_MULTIPLIER : 0.5f);
			//Vtx[n].tv1 = Vtx[n].tv0 * TEX2_MULTIPLIER;
			if (!outside) {
				Vtx[n].nx = -Vtx[n].nx;
				Vtx[n].ny = -Vtx[n].ny;
				Vtx[n].nz = -Vtx[n].nz;
			}
			n++;
		}
	}

	// generate faces
	nIdx = (reduce ? res * (2*bseg-res) : 2*res*bseg) * 3;
	WORD *Idx = g_pMemgr_w->New(nIdx);

	for (i = n = nofs0 = 0; i < res; i++) {
		nseg = (reduce ? bseg-i : bseg);
		nofs1 = nofs0+nseg+1;
		for (j = 0; j < nseg; j++) {
			Idx[n++] = WORD(nofs0+j);
			Idx[n++] = WORD(nofs1+j);
			Idx[n++] = WORD(nofs0+j+1);
			if (reduce && j == nseg-1) break;
			Idx[n++] = WORD(nofs0+j+1);
			Idx[n++] = WORD(nofs1+j);
			Idx[n++] = WORD(nofs1+j+1);
		}
		nofs0 = nofs1;
	}
	if (!outside)
		for (i = 0; i < nIdx/3; i += 3)
			tmp = Idx[i+1], Idx[i+1] = Idx[i+2], Idx[i+2] = tmp;

	mesh.nv  = nVtx;
	mesh.nf  = nIdx/3;
	mesh.vtx = Vtx;
	mesh.idx = Idx;
	mesh.MapVertices(pDev);

	if (Vtx) g_pMemgr_vtx->Free(Vtx);
	if (Idx) g_pMemgr_w->Free(Idx);

	Vtx = NULL;
	Idx = NULL;
	mesh.vtx = NULL;
	mesh.idx = NULL;	

	if (shift_origin) {
		pref.x -= dx;
		pref.y -= dy;
	}

	// transform bounding box back to patch coordinates
	mesh.Box[0] = _V(tmul (R, _V(tpmin.x, tpmin.y, tpmin.z)) + pref);
	mesh.Box[1] = _V(tmul (R, _V(tpmax.x, tpmin.y, tpmin.z)) + pref);
	mesh.Box[2] = _V(tmul (R, _V(tpmin.x, tpmax.y, tpmin.z)) + pref);
	mesh.Box[3] = _V(tmul (R, _V(tpmax.x, tpmax.y, tpmin.z)) + pref);
	mesh.Box[4] = _V(tmul (R, _V(tpmin.x, tpmin.y, tpmax.z)) + pref);
	mesh.Box[5] = _V(tmul (R, _V(tpmax.x, tpmin.y, tpmax.z)) + pref);
	mesh.Box[6] = _V(tmul (R, _V(tpmin.x, tpmax.y, tpmax.z)) + pref);
	mesh.Box[7] = _V(tmul (R, _V(tpmax.x, tpmax.y, tpmax.z)) + pref);
}



// ====================================================================
// NOTE: This is used to delete a vertex buffers from a static VBMESH
//
void ClearVBMesh (VBMESH &mesh)
{
	if (mesh.pVB) g_pVtxmgr_vb->Free(mesh.pVB);
	if (mesh.pIB) g_pIdxmgr_ib->Free(mesh.pIB);
	if (mesh.vtx) g_pMemgr_vtx->Free(mesh.vtx);
	if (mesh.idx) g_pMemgr_w->Free(mesh.idx);
	mesh.nv = 0;
	mesh.nf = 0;
	mesh.nv_cur = 0;
	mesh.nf_cur = 0;
	mesh.pVB = nullptr;
	mesh.pIB = nullptr;
	mesh.vtx = nullptr;
	mesh.idx = nullptr;
}

