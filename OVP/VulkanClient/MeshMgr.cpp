// ==============================================================
// MeshMgr.cpp
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2007-2026 Martin Schweiger
// ==============================================================

// ==============================================================
// class MeshManager (implementation)
// Simple management of persistent mesh templates
// ==============================================================
//
// CONVERTED FROM OVP/D3D9Client/MeshMgr.cpp, read end to end (80 lines).
//
// A growable array of mesh templates. No Direct3D call anywhere in it. Three
// changes:
//
//   D3D9Mesh -> VulkanMesh, D3D9Client -> VulkanClient.
//
//   D3DXVECTOR3 -> FVECTOR3, for GetGroupSize's return.
//
//   THE INCLUDE'S CASE. The Windows file writes `#include "Meshmgr.h"` and
//   the file on disk is MeshMgr.h. NTFS does not care and ext4 does, so this
//   is a hard compile error here and was invisible there. Same class as the
//   TileMgr2.h / Tilemgr2.h correction in Spherepatch.cpp; see
//   the porting notes.
// ==============================================================

#include "MeshMgr.h"

using namespace oapi;

MeshManager::MeshManager(VulkanClient *gclient)
{
	gc = gclient;
	mlist = NULL;
	nmlist = nmlistbuf = 0;
}

MeshManager::~MeshManager()
{
	DeleteAll();
}

void MeshManager::DeleteAll()
{	
	int i;
	for (i=0;i<nmlist;i++) delete mlist[i].mesh;
	if (nmlistbuf) {
		delete []mlist;
		mlist = NULL;
		nmlist = nmlistbuf = 0;
	}
}

int MeshManager::StoreMesh(MESHHANDLE hMesh, const char *name)
{
	if (hMesh==NULL) {
		LogErr("NULL Mesh in MeshManager::StoreMesh()");
		return -1;
	}

	if (GetMesh(hMesh)) return -1; // mesh already stored

	if (nmlist==nmlistbuf) { // need to allocate buffer
		MeshBuffer *tmp = new MeshBuffer[nmlistbuf += 32];
		if (nmlist) {
			memcpy (tmp, mlist, nmlist*sizeof(MeshBuffer));
			delete []mlist;
		}
		mlist = tmp;
	}
	mlist[nmlist].hMesh = hMesh;
	mlist[nmlist].mesh = new VulkanMesh(hMesh, true);
	mlist[nmlist].mesh->SetName(name);
	nmlist++;

	float lim = 1e3;
	DWORD count = mlist[nmlist-1].mesh->GetGroupCount();

	for (DWORD i=0;i<count;i++) {
		// Was D3DXVECTOR3; the same three floats.
		FVECTOR3 s = mlist[nmlist-1].mesh->GetGroupSize(i);
		if (fabs(s.x)>lim || fabs(s.y)>lim || fabs(s.z)>lim) return (int)i;
	}

	return -1;
}

const VulkanMesh *MeshManager::GetMesh (MESHHANDLE hMesh)
{
	int i;
	for (i=0;i<nmlist;i++) if (mlist[i].hMesh==hMesh) return mlist[i].mesh;
	// Should we store the mesh here ??
	return NULL;
}
