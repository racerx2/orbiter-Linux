// ==============================================================
// MeshMgr.h
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2006-2026 Martin Schweiger
// ==============================================================
//
// CONVERTED FROM OVP/D3D9Client/MeshMgr.h, read end to end (38 lines).
// Two renames -- oapi::D3D9Client -> oapi::VulkanClient and
// D3D9Mesh -> VulkanMesh -- and nothing else. No Direct3D type appears.
//
// Worth knowing while reading: the nested `struct MeshBuffer` here is NOT
// Mesh.h's class of the same name. It is a two-field pairing of a
// MESHHANDLE with a mesh, private to this class, and it shadows the other
// one inside it. That collision is the Windows source's and is left alone.
// ==============================================================

#ifndef __MESHMGR_H
#define __MESHMGR_H

#include "VulkanClient.h"
#include "Mesh.h"

// ==============================================================
// class MeshManager (interface)
// ==============================================================
/**
 * \brief Simple management of persistent mesh templates
 */

class MeshManager {
public:
	explicit MeshManager (oapi::VulkanClient *gclient);
	~MeshManager();
	void DeleteAll();
	int StoreMesh (MESHHANDLE hMesh, const char *name);
	const VulkanMesh *GetMesh (MESHHANDLE hMesh);

private:
	oapi::VulkanClient *gc;
	struct MeshBuffer {
		MESHHANDLE hMesh;
		VulkanMesh *mesh;
	} *mlist;
	int nmlist, nmlistbuf;
};

#endif // !__MESHMGR_H
