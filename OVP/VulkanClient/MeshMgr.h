// ==============================================================
// MeshMgr.h
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2006-2026 Martin Schweiger
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
	// This nested MeshBuffer is not Mesh.h's class of the same name: it is a
	// private pairing of a MESHHANDLE with a mesh, and inside this class it
	// shadows the other one. The collision comes from the Windows source.
	struct MeshBuffer {
		MESHHANDLE hMesh;
		VulkanMesh *mesh;
	} *mlist;
	int nmlist, nmlistbuf;
};

#endif // !__MESHMGR_H
