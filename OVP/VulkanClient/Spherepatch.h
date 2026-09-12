// ==============================================================
//   ORBITER VISUALISATION PROJECT (OVP)
//   Copyright (C) 2006-2026 Martin Schweiger
//   Dual licensed under GPL v3 and LGPL v3
// ==============================================================

// ==============================================================
// spherepatch.h
// Create meshes for spheres and sphere patches
// ==============================================================

#ifndef __SPHEREPATCH_H
#define __SPHEREPATCH_H

#include "VulkanClient.h"
#include "VulkanUtil.h"

struct VBMESH {

	explicit VBMESH(class TileManager2Base *pMgr);
	VBMESH();
	~VBMESH();

	void MapVertices (VulkanDevice *dev, DWORD MemFlag=0); // copy vertices from vtx to vb
	void ComputeSphere();

	VulkanBuffer *pVB;				// mesh vertex buffer
	VulkanBuffer *pIB;				// mesh index buffer

	VERTEX_2TEX *vtx;				// separate storage of vertices (NULL if not available)
	WORD *idx;						// list of indices
	DWORD nv;						// number of vertices
	DWORD nf;						// number of faces (number of indices/3)
	DWORD nv_cur;					
	DWORD nf_cur;					
	VECTOR4 Box[8];					// bounding box vertices
	FVECTOR3 bsCnt;					// bounding sphere position
	float  bsRad;					// bounding sphere radius
	bool bBox;						// true if bounding box data is valid
};

// The device parameter on these functions (and on VBMESH::MapVertices) is
// vestigial, as it already was on Windows: none of the bodies reads it. Every
// allocation in Spherepatch.cpp goes through the g_pVtxmgr_vb / g_pIdxmgr_ib /
// g_pMemgr_* managers, which carry their own device. It is kept because the
// call sites pass it.
void CreateSphere(VulkanDevice *pDev, VBMESH &mesh, DWORD nrings, bool hemisphere, int which_half, int texres);
void CreateSpherePatch(VulkanDevice *pDev, VBMESH &mesh, int nlng, int nlat, int ilat, int res, int bseg = -1, bool reduce = true, bool outside = true, bool store_vtx = false, bool shift_origin = false);
void ClearVBMesh (VBMESH &mesh);

#endif // !__SPHEREPATCH_H
