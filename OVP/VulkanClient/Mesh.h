// ==============================================================
// Mesh.h
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2006-2026 Martin Schweiger
//				 2012-2019 Jarmo Nikkanen
// ==============================================================

// ==============================================================
// class VulkanMesh (interface)
//
// This class represents a mesh in terms of Vulkan resources
// (vertex buffers, index lists, materials, textures) which allow
// it to be rendered through the Vulkan device.
// ==============================================================
//
// CONVERTED FROM OVP/D3D9Client/Mesh.h, read end to end (376 lines).
//
// The class header comment on Windows still says "in terms of DX7 interface
// elements", which stopped being true two ports ago; it says Vulkan here
// because that is what the members are now.
//
// This is a type-mapping conversion -- the mesh logic is Orbiter's, not
// Direct3D's -- with three things worth naming:
//
//  1. MeshBuffer HELD FOUR BUFFERS AND FOUR SHADOW COPIES. pVB/pGB/pIB/pSB
//     were D3DPOOL_DEFAULT resources with pVBSys/pGBSys/pIBSys/pSBSys as the
//     system-memory originals, and Map() pushed the shadow copies into the
//     device buffers. That two-copy dance is what D3DPOOL_DEFAULT forced: a
//     default-pool buffer cannot be read back and is lost on device reset.
//
//     The shadow copies STAY. They are not a D3D9 artefact -- the mesh code
//     reads and edits vertices between frames (EditGroup, TransformGroup,
//     UpdateTangentSpace), and reading back from device-local memory is no
//     more possible in Vulkan than it was in D3D9. What changes is Map(): it
//     copies into host-visible buffers directly instead of Lock/Unlock.
//
//  2. D3DXVECTOR4 *pGBSys BECAME FVECTOR4* -- and this one has a size
//     consequence. FVECTOR4 is alignas(16) where D3DXVECTOR4 was not, so an
//     ARRAY of them is identically packed (16 bytes either way) but the
//     allocation must be 16-byte aligned. new[] gives that automatically for
//     an over-aligned type since C++17, which this builds as.
//
//  3. LPDIRECT3DCUBETEXTURE9 *pEnv BECAME VulkanTexture** . A cube map is an
//     image with six array layers and a CUBE-typed view, not a separate
//     interface.
//
// D3D9Mesh -> VulkanMesh, D3D9MatExt -> VulkanMatExt, D3D9Tune -> VulkanTune,
// D3D9Sun -> VulkanSun, D3D9Pick -> VulkanPick, D3D9Effect -> VulkanEffect,
// D3DXMATRIX -> FMATRIX4, D3DCOLOR -> DWORD, D3DMATERIAL9 -> MATERIAL.
// ==============================================================

#ifndef __MESH_H
#define __MESH_H

#include "VulkanClient.h"
#include "VulkanEffect.h"
#include "AABBUtil.h"
#include <vulkan/vulkan.h>
#include "VulkanTypes.h"
#include <vector>

const DWORD SPEC_DEFAULT = (DWORD)(-1); // "default" material/texture flag
const DWORD SPEC_INHERIT = (DWORD)(-2); // "inherit" material/texture flag

#define RENDER_VESSEL		0
#define RENDER_BASE			1
#define RENDER_ASTEROID		2
#define RENDER_BASETILES	3
#define RENDER_VC			4
#define RENDER_BASEBS		5
#define RENDER_CUSTOM		6

#define ENVMAP_MAIN			0


// Mesh Shaders
#define SHADER_NULL				0xFFFF
#define SHADER_PBR				0
#define SHADER_ADV				1
#define SHADER_LEGACY			2			// Shader most compatible with DX7 Inline
#define SHADER_XR2HUD			3			// XR2 HUD shader
#define SHADER_METALNESS		4
#define SHADER_SHADOWMAP		10
#define SHADER_SHADOWMAP_OIT	11
#define SHADER_NORMAL_DEPTH		12

#define VCLASS_AMSO			1
#define VCLASS_XR2			2
#define VCLASS_ULTRA		3
#define VCLASS_SSU_CENTAUR	4

// Mesh memory mapping mode
#define MAPMODE_UNKNOWN		0
#define MAPMODE_CURRENT		1
#define MAPMODE_STATIC		2
#define MAPMODE_DYNAMIC		3


struct _LightList {
	int		idx;
	float	illuminace;
};


class MeshShader : public ShaderClass
{
public:

	static struct VSConst {
		float4x4 mVP;	// View Projection Matrix
		float4x4 mW;	// World Matrix
	} vs_const;

	static struct PSConst {
		float3 Cam_X;
		float3 Cam_Y;
		float3 Cam_Z;
	} ps_const;

	static struct PSBools {
		BOOL bOIT;		// Enable order independent transparency
	} ps_bools;

	// Measured, not assumed: these three are uploaded whole, by name, into
	// the GLSL blocks of NewMesh.glsl, so their layout has to be the
	// shader's. Two 64-byte matrices; three FVECTOR3s, which are 12 bytes and
	// carry no alignas (unlike FVECTOR4 and FMATRIX4); one 32-bit BOOL. No
	// #pragma pack is needed here for the same reason -- nothing in them
	// declares an alignment above its own size -- and the asserts are what
	// says so rather than leaving it to be rediscovered.
	//
	// The GLSL blocks mirroring them must be layout(scalar), which is what
	// makes std140's vec3-to-16 rounding not apply. Confirmed against
	// glslang: ps_const reflects at 0/12/24 and ps_bools.bOIT at 36.
	static_assert(sizeof(VSConst) == 128, "MeshShader::VSConst must be two 4x4 matrices");
	static_assert(offsetof(VSConst, mW) == 64, "MeshShader::VSConst::mW at 64");
	static_assert(sizeof(PSConst) == 36, "MeshShader::PSConst must be three tightly packed FVECTOR3");
	static_assert(offsetof(PSConst, Cam_Y) == 12, "MeshShader::PSConst::Cam_Y at 12");
	static_assert(offsetof(PSConst, Cam_Z) == 24, "MeshShader::PSConst::Cam_Z at 24");
	static_assert(sizeof(PSBools) == 4, "MeshShader::PSBools must be one 32-bit BOOL");


	// The shader path changes with the module name and the language: the
	// Windows line names Modules/D3D9Client/NewMesh.hlsl. The GLSL
	// translation lives beside the module that loads it.
	MeshShader(VulkanDevice *pDev, const char *file, const char *vs, const char *ps, const char *opt = NULL) :
		ShaderClass(pDev, "Modules/VulkanClient/NewMesh.glsl", vs, ps, "MeshShader", opt)
	{
		memset(hPST, 0, sizeof(hPST));
		hVSC  = GetVSHandle("vs_const");
		hPSC  = GetPSHandle("ps_const");
		hPSB  = GetPSHandle("ps_bools");
		hPST[0] = GetPSHandle("tDiff");
	}

	~MeshShader()
	{

	}

	HANDLE hPSB, hVSC, hPSC, hPST[16];
};



class MeshBuffer
{
public:

	MeshBuffer(MeshBuffer *pSrc, const class VulkanMesh *_pRoot);
	MeshBuffer(DWORD nVtx, DWORD nIdx, const class VulkanMesh *_pRoot);
	~MeshBuffer();

	void Map(VulkanDevice *pDev);
	bool IsLocalTo(const class VulkanMesh *_pRoot) const { return (_pRoot == pRoot); }
	void MustRemap(DWORD mode);

	VulkanBuffer *pVB;
	VulkanBuffer *pGB;
	VulkanBuffer *pIB;
	VulkanBuffer *pSB;

	// The system-memory originals. See note 1 in the file header: these are
	// not a D3D9 artefact and they stay.
	NMVERTEX				*pVBSys;
	FVECTOR4				*pGBSys;
	WORD					*pIBSys;
	SMVERTEX				*pSBSys;

	DWORD nVtx;
	DWORD nIdx;
	DWORD mapMode;
	bool  bMustRemap;

	const class VulkanMesh	*pRoot;
};






/**
 * \brief Mesh object with Vulkan-specific vertex buffer
 *
 * Meshes consist of one or more vertex groups, and a set of materials and
 * textures.
 */

class VulkanMesh : private VulkanEffect
{

public:

	bool bCanRenderFast;		// Mesh doesn't contain any advanced features in any group
	bool bIsReflective;			// Mesh has a reflective material in one or more groups
	bool bMtrlModidied;
	bool bIsTemplate;
	
	D9BBox BBox;
	MeshBuffer *pBuf;

	struct GROUPREC {			// mesh group definition
		DWORD VertOff;			// Main mesh Vertex Offset
		DWORD IdexOff;			// Main mesh Index Offset
		//------------------------------------------------
		DWORD nFace;			// Face/Primitive count
		DWORD nVert;			// Vertex count
		//------------------------------------------------
		DWORD MtrlIdx;			// material index
		DWORD TexIdx;			// texture index 0=None
		DWORD UsrFlag;			// user-defined flag
		WORD  IntFlag;			// internal flags
		WORD  zBias;
		WORD  MFDScreenId;		// MFD screen ID + 1
		WORD  PBRStatus;
		WORD  Shader;
		bool  bTransform;
		bool  bUpdate;			// Bounding box update required
		bool  bDualSided;
		bool  bDeleted;			// This entry is deleted by DelGroup()
		bool  bRendered;
		FMATRIX4  Transform;	// Group specific transformation matrix
		D9BBox BBox;
		DWORD TexIdxEx[MAXTEX];
		float TexMixEx[MAXTEX];
	};


					VulkanMesh(const char *fname);
					VulkanMesh(DWORD nGrp, const MESHGROUPEX **hGroup, const SURFHANDLE *hSurf);
					VulkanMesh(const MESHGROUPEX *pGroup, const MATERIAL *pMat, SurfNative *pTex);
					VulkanMesh(MESHHANDLE hMesh, bool asTemplate = false, FVECTOR3 *reorig = NULL, float *scale = NULL);
					VulkanMesh(MESHHANDLE hMesh, const VulkanMesh &hTemp);
					~VulkanMesh();

	bool			IsOK() const { return pBuf != NULL; }

	void			Release();

	void			LoadMeshFromHandle(MESHHANDLE hMesh, FVECTOR3 *reorig = NULL, float *scale = NULL);
	void			ReLoadMeshFromHandle(MESHHANDLE hMesh);
	void			ReloadTextures();

	void			SetName(const char *name);
	void			SetName(UINT idx);
	const char *	GetName() const { return name; }

	void			SetDefaultShader(WORD shader) { DefShader = shader; bMtrlModidied = true; }
	WORD			GetDefaultShader() const { return DefShader; }
	
	void			SetClass(DWORD cl) { vClass = cl; }


	/**
	 * \brief Check if a mesh is casting shadows
	 * \return Returns true if the mesh is casting shadows.
	 */
	bool			HasShadow() const;

	/**
	 * \brief Returns a pointer to a mesh group.
	 * \param idx group index (>= 0)
	 * \return Pointer to group structure.
	 */
	const GROUPREC * GetGroup(DWORD idx) const;
	void            SetMFDScreenId(DWORD idx, WORD id);
	void			SetDualSided(DWORD idx, bool bState) { Grp[idx].bDualSided = bState; }

	/**
	 * \brief Returns number of material specifications.
	 * \return Number of materials.
	 */
	SURFHANDLE		GetTexture(DWORD idx) const { return (SURFHANDLE)Tex[idx]; }
	bool			HasTexture(SURFHANDLE hSurf) const;
	bool			IsReflective() const { return bIsReflective | (DefShader==SHADER_METALNESS); }

	/**
	 * \brief returns a pointer to a material definition.
	 * \param idx material index (>= 0)
	 * \return Pointer to material object.
	 */
	const VulkanMatExt *	GetMaterial(DWORD idx) const;
	bool			GetMaterial(VulkanMatExt *pMat, DWORD idx) const;
	void			SetMaterial(const VulkanMatExt *pMat, DWORD idx, bool bUpdateStatus = true);
	void			SetMaterial(const MATERIAL *pMat, DWORD idx, bool bUpdateStatus = true);
	int				SetMaterialEx(DWORD idx, MatProp mid, const FVECTOR4* in);
	int				GetMaterialEx(DWORD idx, MatProp mid, FVECTOR4* out);
	bool			GetTexTune(VulkanTune *pT, DWORD idx) const;
	void			SetTexTune(const VulkanTune *pT, DWORD idx);

	DWORD			GetGroupCount() const { return nGrp; }
	DWORD			GetMaterialCount() const { return nMtrl; }
	DWORD			GetTextureCount() const { return nTex; }
	DWORD			GetVertexCount(int grp=-1) const;
	DWORD			GetIndexCount(int grp=-1) const;
	bool			IsGroupRendered(DWORD grp) const;

	DWORD			GetMeshGroupMaterialIdx(DWORD grp) const;
	DWORD			GetMeshGroupTextureIdx(DWORD grp) const;
	DWORD			GetGroupTransformCount() const;
	FVECTOR3		GetBoundingSpherePos();
	float			GetBoundingSphereRadius();
	D9BBox *		GetAABB();
	FVECTOR3		GetGroupSize(DWORD idx) const;
	FMATRIX4 *		GetTransform() { if (bGlobalTF) return &mTransform; else return NULL; }

	FMATRIX4		GetTransform(int grp, bool bCombined);
	bool			SetTransform(int grp, const FMATRIX4 *pMat);

	void			SetPosition(VECTOR3 &pos);
	void			SetRotation(FMATRIX4 &rot);

	/**
	 * \brief Replace a mesh texture.
	 * \param texidx texture index (>= 0)
	 * \param tex texture handle
	 * \return \e true on success, \e false otherwise.
	 */
	bool			SetTexture(DWORD texidx, SURFHANDLE tex);
	void			SetTexMixture (DWORD ntex, float mix);

	void			RenderGroup(const GROUPREC *grp);
	void			RenderGroup(int idx);
	void			RenderBaseTile(const FMATRIX4 *pW);
	void			RenderBoundingBox(const FMATRIX4 *pW);
	void			Render(const FMATRIX4 *pW, int iTech=RENDER_VESSEL, VulkanTexture **pEnv=NULL, int nEnv=0);
	void			RenderFast(const FMATRIX4 *pW, int iTech);
	void			RenderShadowMap(const FMATRIX4 *pW, const FMATRIX4 *pVP, int flags);
	void			RenderStencilShadows(float alpha, const FMATRIX4 *pP, const FMATRIX4 *pW, bool bShadowMap = false, const FVECTOR4 *elev = NULL);
	void			RenderShadowsEx(float alpha, const FMATRIX4 *pP, const FMATRIX4 *pW, const FVECTOR4 *light, const FVECTOR4 *param);
	void			RenderRings(const FMATRIX4 *pW, VulkanTexture *pTex);
	void			RenderRings2(const FMATRIX4 *pW, VulkanTexture *pTex, float irad, float orad);
	void			RenderAxisVector(FMATRIX4 *pW, const FVECTOR4 *pColor, float len);
	void			RenderSimplified(const FMATRIX4 *pW, VulkanTexture **pEnv = NULL, int nEnv = 0, bool bSP = false);
	void			CheckMeshStatus();
	void			ResetTransformations();
	void			TransformGroup(DWORD n, const FMATRIX4 *m);
	void			Transform(const FMATRIX4 *m);
	int				GetGroup (DWORD grp, GROUPREQUESTSPEC *grs);
	int				EditGroup (DWORD grp, GROUPEDITSPEC *ges);

	void			SetSunLight(const VulkanSun *pLight);

	VulkanPick		Pick(const FMATRIX4 *pW, const FMATRIX4 *pT, const FVECTOR3 *vDir);

	void			UpdateBoundingBox();
	void			BoundingBox(const NMVERTEX *vtx, DWORD n, D9BBox *box);

	void			SetAmbientColor(DWORD c);
	void			SetupFog(const FMATRIX4 *pW);
	void			ResetRenderStatus();


	/**
	 * \brief Enable/disable material alpha value for transparency calculation.
	 * \param enable flag for enabling/disabling material alpha calculation.
	 * \note By default, material alpha values are ignored for mesh groups
	 *   with textures, and the texture alpha values are used instead.
	 */
	inline void		EnableMatAlpha (bool enable) { bModulateMatAlpha = enable; }

	static void		GlobalInit(VulkanDevice *pDev);
	static void		GlobalExit();

private:


	void			UpdateTangentSpace(NMVERTEX *pVrt, WORD *pIdx, DWORD nVtx, DWORD nFace, bool bTextured);
	void			ProcessInherit();
	bool			CopyVertices(GROUPREC *grp, const MESHGROUPEX *mg, FVECTOR3 *reorig = NULL, float *scale = NULL);
	void			SetGroupRec(DWORD i, const MESHGROUPEX *mg);
	void			Null(const char *meshName = NULL);
	void			UpdateFlags();
	void			ConfigureAtmo();

	WORD	DefShader;
	DWORD	MaxVert;
	DWORD	MaxFace;
	GROUPREC *Grp;              // list of mesh groups
	DWORD nGrp;                 // number of mesh groups
	DWORD nMtrl;                // number of mesh materials
	DWORD nTex;                 // number of mesh textures
	DWORD vClass;
	DWORD Flags;
	VulkanMatExt *Mtrl;         // list of mesh materials
	SurfNative **Tex;			// list of mesh textures
	VulkanTune *pTune;
	FMATRIX4 mTransform;
	FMATRIX4 mTransformInv;
	FMATRIX4 *pGrpTF;
	VulkanSun sunLight;
	DWORD cAmbient;				// was D3DCOLOR
	LightStruct null_light;

	_LightList LightList[MAX_SCENE_LIGHTS];
	LightStruct *Locals;
	bool bBSRecompute;			// Bounding sphere must be recomputed
	bool bBSRecomputeAll;
	bool bModulateMatAlpha;     // mix material and texture alpha channels
	bool bGlobalTF;				// Mesh has a valid mTransform matrix

	char name[128];

	static MeshShader* s_pShader[16];
};

#endif // !__MESH_H
