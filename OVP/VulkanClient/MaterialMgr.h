// ==============================================================
// MaterialMgr.h
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2012-2026 Jarmo Nikkanen
// ==============================================================
//
// CONVERTED FROM OVP/D3D9Client/MaterialMgr.h, read end to end (90 lines).
//
// Per-vessel material and environment-camera overrides, read from and written
// to a text file. Nothing here touches a graphics API; the types change:
//
//   D3D9Client   -> VulkanClient
//   D3D9Mesh     -> VulkanMesh
//   D3D9MatExt   -> VulkanMatExt   (field for field; see VulkanUtil.h)
//   D3DXVECTOR3  -> FVECTOR3
//
// `#include <d3d9.h>` and `<d3dx9.h>` become `VulkanTypes.h`, which is this
// port's counterpart of the first and has no counterpart to the second --
// D3DX is a utility library, and what this file used from it (the vector
// types) now comes from the SDK.
//
// `#include "vObject.h"` becomes `"VObject.h"`. The file on disk has always
// been VObject.h; NTFS did not care and ext4 does. Same class as
// `MeshMgr.cpp`'s `Meshmgr.h`.
// ==============================================================

#ifndef __MATERIALMGR_H
#define __MATERIALMGR_H

#include "VulkanTypes.h"

#include "Mesh.h"
#include "VulkanClient.h"
#include "VulkanUtil.h"
#include "VObject.h"

#define ENVCAM_OMIT_ATTC		0x0001
#define ENVCAM_OMIT_DOCKS		0x0002
#define ENVCAM_FOCUS			0x0004


/**
 * \brief Storage structure to keep environmental camera information.
 */
struct ENVCAMREC {
	FVECTOR3		lPos;			///< Camera local position
	float			near_clip;		///< Near clip-plane distance
	DWORD			flags;			///< Camera flags
	WORD			nAttc;			///< Number of attachments points in a list
	WORD			nDock;			///< Number of docking ports in a list
	BYTE *			pOmitAttc;		///< Omit attachments
	BYTE *			pOmitDock;		///< Omit vessels in docking ports
};

/**
 * \brief Management of custom configurations for vessel materials
 */
class MatMgr {

public:
	// Disable copy construct & copy assign
					MatMgr    (MatMgr const&) = delete;
	MatMgr &		operator= (MatMgr const&) = delete;

					MatMgr(class vObject *vObj, class VulkanClient *_gc);
					~MatMgr();

	//DWORD			NewRecord(const char *name, DWORD midx);
	//void			ClearRecord(DWORD iRec);
	void			RegisterMaterialChange(VulkanMesh *pMesh, DWORD midx, const VulkanMatExt *pM);
	void			RegisterShaderChange(VulkanMesh *pMesh, WORD id);
	void			ApplyConfiguration(VulkanMesh *pMesh);
	bool			SaveConfiguration();
	bool			LoadConfiguration(bool bAppend=false);
	bool			LoadCameraConfig();
	bool			HasMesh(const char *name);
	void			ResetCamera(DWORD idx);

	ENVCAMREC *		GetCamera(DWORD idx);
	DWORD			CameraCount();

private:

	vObject			*vObj;
	VulkanClient	*gc;
	//DWORD			nRec;	///< Number of records
	//DWORD			mRec;	///< Allocated records


	struct SHADER {
		SHADER(std::string x, WORD i) { name = x; id = i; }
		std::string name;
		WORD id;
	};

	struct MESHREC {
		WORD shader;
		std::map<int, VulkanMatExt> material;
	};

	std::map<std::string, MESHREC> MeshConfig;
	std::list<SHADER> Shaders;

	ENVCAMREC *pCamera;
};

#endif
