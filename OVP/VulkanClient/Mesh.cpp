// ==============================================================
// Mesh.cpp
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2006-2026 Martin Schweiger
//				 2010 - 2022 Jarmo Nikkanen (VulkanClient implementation)
// ==============================================================
//
// CONVERTED FROM OVP/D3D9Client/Mesh.cpp, read end to end (3431 lines).
//
// The mesh: vertex buffers, materials, textures, the light list, the shadow
// passes and eleven render entry points. Most of it is Orbiter's own geometry
// and material logic and converts by renaming types. What actually changed:
//
//  1. MeshBuffer HOLDS FOUR HOST-VISIBLE BUFFERS AND FOUR SHADOW COPIES, and
//     the shadow copies are NOT a D3D9 artefact -- see the note on Map()
//     below, and Mesh.h's note 1. D3DUSAGE_DYNAMIC and D3DLOCK_DISCARD are
//     the two things that genuinely disappear: both are hints to a runtime
//     that renames buffers behind the client, and Vulkan has no such runtime.
//
//  2. SAFE_RELEASE ON A BUFFER IS DestroyBuffer. Vulkan objects are not
//     reference counted; the wrapper owns its VkBuffer and VkDeviceMemory.
//     SafeDestroy() below is that, written once because MeshBuffer releases
//     four buffers in three places.
//
//  3. <xnamath.h> IS GONE, and it was never Direct3D. It is DirectXMath in
//     its older XNA spelling -- a Windows SSE wrapper over the same _mm_*
//     intrinsics GCC has always had. Same finding as VBase.cpp's; the uses
//     are written out where they occur.
//
//  4. THE D3DX MATRIX AND VECTOR CALLS BECOME VMAT_ AND oapi::. D3DXMatrixIdentity
//     -> VMAT_Identity, D3DXMatrixMultiply -> VMAT_MatrixMultiply (which
//     returns void, so a call site using the return value needs a local),
//     D3DXVec3TransformCoord/Normal -> oapi::TransformCoord/TransformNormal,
//     D3DXVec3Normalize/Cross/Dot/Length -> the SDK's own free functions.
//
//  5. THE RENDER STATES SET BETWEEN Begin() AND THE DRAW BECOME PassOverrides,
//     and the ones that no longer exist disappear. Same rule as Surfmgr2.cpp:
//     a post-BeginPass SetRenderState is an override only when the state it
//     names still exists.
//
// Type mapping, once: D3D9Mesh -> VulkanMesh, D3D9MatExt -> VulkanMatExt,
// D3D9Tune -> VulkanTune, D3D9Sun -> VulkanSun, D3D9Pick -> VulkanPick,
// D3D9Effect -> VulkanEffect, D3D9Client -> VulkanClient,
// LPDIRECT3DDEVICE9 -> VulkanDevice*, LPDIRECT3DTEXTURE9 /
// LPDIRECT3DCUBETEXTURE9 -> VulkanTexture*, LPDIRECT3DVERTEXBUFFER9 /
// LPDIRECT3DINDEXBUFFER9 -> VulkanBuffer*, D3DXMATRIX -> FMATRIX4,
// D3DXVECTOR3/4 -> FVECTOR3/4, D3DXCOLOR -> FVECTOR4, D3DCOLOR -> DWORD,
// D3DMATERIAL9 -> MATERIAL, D3DXVec* -> the SDK's.
// ==============================================================

#define VISIBILITY_TOL 0.0015f

#include "Mesh.h"
#include "Log.h"
#include "Scene.h"
#include "VulkanSurface.h"
#include "VulkanCatalog.h"
#include "VulkanConfig.h"
#include "DebugControls.h"
#include "VectorHelpers.h"

// <xnamath.h> and the two #pragma warning lines around it stood here. See
// note 3 in the file header: it is DirectXMath, not Direct3D, and the handful
// of places that used it are written out where they occur.

using namespace oapi;


MeshShader* VulkanMesh::s_pShader[16] = {};
MeshShader::VSConst MeshShader::vs_const = {};
MeshShader::PSConst MeshShader::ps_const = {};
MeshShader::PSBools MeshShader::ps_bools = {};


// -------------------------------------------------------------------------------------------
// The counterpart of SAFE_RELEASE for a VulkanBuffer. See note 2 in the file
// header. VulkanDevice::DestroyBuffer is the delete, and VulkanEffect::pDev is
// the client's one device -- the same static every VulkanMesh method reaches
// through its private VulkanEffect base.
// -------------------------------------------------------------------------------------------
static inline void SafeDestroy(VulkanBuffer *&p)
{
	if (p) { VulkanEffect::pDev->DestroyBuffer(p); p = NULL; }
}


int compare_lights(const void * a, const void * b)
{
	float fa = static_cast<const _LightList*>(a)->illuminace;
	float fb = static_cast<const _LightList*>(b)->illuminace;
	if (fa < fb * 0.9995f) return  1;
	if (fa > fb * 1.0005f) return -1;
	return 0;
}



// ======================================================================================
// Buffer Object Implementation
// ======================================================================================
//

MeshBuffer::MeshBuffer(DWORD _nVtx, DWORD _nFace, const class VulkanMesh *_pRoot)
{
	nVtx = _nVtx;
	nIdx = _nFace * 3;

	pVB = NULL;
	pIB = NULL;
	pGB = NULL;
	pSB = NULL;

	pVBSys = new NMVERTEX[nVtx];
	pIBSys = new WORD[nIdx];
	// D3DXVECTOR4 was not over-aligned; FVECTOR4 is alignas(16). An ARRAY of
	// them is packed identically (16 bytes either way), and new[] gives the
	// alignment automatically for an over-aligned type since C++17, which this
	// builds as. See Mesh.h's note 2.
	pGBSys = new FVECTOR4[nVtx];
	pSBSys = new SMVERTEX[nVtx];

	pRoot = _pRoot;
	mapMode = MAPMODE_STATIC;
	bMustRemap = true;
}


MeshBuffer::MeshBuffer(MeshBuffer *pSrc, const class VulkanMesh *_pRoot)
{
	nVtx = pSrc->nVtx;
	nIdx = pSrc->nIdx;

	pVB = NULL;
	pIB = NULL;
	pGB = NULL;
	pSB = NULL;

	pVBSys = new NMVERTEX[nVtx];
	pIBSys = new WORD[nIdx];
	pGBSys = new FVECTOR4[nVtx];
	pSBSys = new SMVERTEX[nVtx];

	memcpy(pSBSys, pSrc->pSBSys, sizeof(SMVERTEX) * nVtx);
	memcpy(pVBSys, pSrc->pVBSys, sizeof(NMVERTEX) * nVtx);
	memcpy(pGBSys, pSrc->pGBSys, sizeof(FVECTOR4) * nVtx);
	memcpy(pIBSys, pSrc->pIBSys, sizeof(WORD) * nIdx);

	pRoot = _pRoot;
	mapMode = MAPMODE_STATIC;
	bMustRemap = true;
}


MeshBuffer::~MeshBuffer()
{
	SAFE_DELETEA(pGBSys);
	SAFE_DELETEA(pIBSys);
	SAFE_DELETEA(pVBSys);
	SAFE_DELETEA(pSBSys);
	SafeDestroy(pIB);
	SafeDestroy(pVB);
	SafeDestroy(pGB);
	SafeDestroy(pSB);
}

void MeshBuffer::MustRemap(DWORD mode)
{
	if (mode == MAPMODE_CURRENT) mode = mapMode;

	if (mode != mapMode) {
		SafeDestroy(pIB);
		SafeDestroy(pVB);
		SafeDestroy(pGB);
		SafeDestroy(pSB);
		mapMode = mode;
	}

	bMustRemap = true;
}

// -------------------------------------------------------------------------------------------
// Was: CreateVertexBuffer / CreateIndexBuffer into D3DPOOL_DEFAULT, then
// Lock / memcpy / Unlock from the four system-memory shadow copies.
//
// THREE THINGS CHANGE AND ONE DELIBERATELY DOES NOT.
//
//   D3DUSAGE_DYNAMIC AND D3DLOCK_DISCARD HAVE NO COUNTERPART, and need none.
//   Both are hints to a runtime that renames a buffer behind the client so a
//   full overwrite does not stall on the GPU still reading the old contents.
//   Vulkan has no such runtime -- there is nothing between vkMapMemory and
//   the memory -- so the two flags disappear rather than being translated
//   into something weaker. What they bought is a driver optimisation, not a
//   capability, and every write below is a full overwrite either way.
//
//   THE BUFFERS ARE HOST-VISIBLE IN BOTH MAP MODES. D3DPOOL_DEFAULT plus a
//   Lock is a combination Vulkan does not have: device-local memory cannot be
//   mapped at all, so a device-local vertex buffer would need a staging
//   buffer and a vkCmdCopyBuffer for every remap. Host-visible is correct for
//   both modes and is what VulkanCatalog.h's Vtxmgr and Idxmgr already do for
//   the tile buffers. MAPMODE_STATIC and MAPMODE_DYNAMIC therefore no longer
//   select different memory -- they still select different remap behaviour,
//   which is what the callers actually use them for.
//
//   D3DFMT_INDEX16 IS NOT A BUFFER PROPERTY HERE. The index type is given to
//   vkCmdBindIndexBuffer at bind time, so it leaves creation and reappears at
//   the draw. Same note as Idxmgr's.
//
//   THE FOUR SHADOW COPIES STAY. They are not a D3DPOOL_DEFAULT workaround:
//   the mesh code reads and edits vertices between frames (EditGroup,
//   TransformGroup, UpdateTangentSpace), and reading back from a mapped
//   write-combined pointer is as bad an idea here as reading back from a
//   default-pool buffer was there.
// -------------------------------------------------------------------------------------------
void MeshBuffer::Map(VulkanDevice *pDev)
{

	if (!bMustRemap) return;

	bMustRemap = false;

	if (!pSB) {
		pSB = pDev->CreateBuffer(nVtx * sizeof(SMVERTEX), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
		if (!pSB) { LogErr("MeshBuffer::Map() failed to create the shadow vertex buffer"); return; }
	}
	if (!pVB) {
		pVB = pDev->CreateBuffer(nVtx * sizeof(NMVERTEX), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
		if (!pVB) { LogErr("MeshBuffer::Map() failed to create the vertex buffer"); return; }
	}
	if (!pGB) {
		pGB = pDev->CreateBuffer(nVtx * sizeof(FVECTOR4), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
		if (!pGB) { LogErr("MeshBuffer::Map() failed to create the geometry buffer"); return; }
	}
	if (!pIB) {
		pIB = pDev->CreateBuffer(nIdx * sizeof(WORD), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
		if (!pIB) { LogErr("MeshBuffer::Map() failed to create the index buffer"); return; }
	}

	void *pTgt;

	pTgt = pSB->Map();
	if (pTgt) { memcpy(pTgt, pSBSys, nVtx * sizeof(SMVERTEX)); pSB->Unmap(); }

	pTgt = pVB->Map();
	if (pTgt) { memcpy(pTgt, pVBSys, nVtx * sizeof(NMVERTEX)); pVB->Unmap(); }

	pTgt = pGB->Map();
	if (pTgt) { memcpy(pTgt, pGBSys, nVtx * sizeof(FVECTOR4)); pGB->Unmap(); }

	pTgt = pIB->Map();
	if (pTgt) { memcpy(pTgt, pIBSys, nIdx * sizeof(WORD)); pIB->Unmap(); }
}








// ======================================================================================
// Mesh Implementation
// ======================================================================================
//
void VulkanMesh::Null(const char *meshName /* = NULL */)
{
	nGrp = 0;
	Grp = NULL;
	nTex = 0;
	Tex	= NULL;
	pTune = NULL;
	nMtrl = 0;
	pBuf = NULL;
	Mtrl = NULL;
	pGrpTF = NULL;
	cAmbient = 0;
	MaxFace  = 0;
	MaxVert  = 0;
	vClass = 0;
	DefShader = SHADER_NULL;

	bIsTemplate = false;
	bGlobalTF = false;
	bBSRecompute = true;
	bBSRecomputeAll = true;
	bModulateMatAlpha = false;
	bIsReflective = false;
	bCanRenderFast = false;
	bMtrlModidied = false;

	Locals = new LightStruct[Config->MaxLights()];

	// THE (void*) CASTS ON THE memset/memcpy CALLS IN THIS FILE ARE NOT
	// COSMETIC, and they are not a change of behaviour either. GCC's
	// -Wclass-memaccess (on under -Wall) reports a memset or a cross-type
	// memcpy on a class type that is not trivially default-constructible or
	// whose source type differs; MSVC has no equivalent. Nine sites here:
	// LightStruct and GROUPREC both hold FVECTOR4/FMATRIX4/D9BBox members
	// with user-provided constructors, and the light copies read out of a
	// VulkanLight, which derives from LightStruct.
	//
	// EVERY ONE OF THEM MUST STAY A memset OR memcpy. The obvious "fix" --
	// using the type's own constructor -- would be WRONG here:
	// LightStruct's default constructor sets Direction to (1,0,0) and
	// Attenuation to (1,1,1), not to zero, so `Locals[i] = LightStruct()`
	// is a different array from the one this line makes. The cast says "I
	// mean the bytes" and nothing else changes.
	memset((void*)Locals, 0, sizeof(LightStruct) * Config->MaxLights());
	memset(LightList, 0, sizeof(LightList));
	strcpy_s(this->name, ARRAYSIZE(this->name), meshName ? meshName : "???");
}

// ===========================================================================================
//
VulkanMesh::VulkanMesh(const char *fname) : VulkanEffect()
{
	Null();
	MESHHANDLE hMesh = oapiLoadMesh(fname);

	if (hMesh) {
		LoadMeshFromHandle(hMesh);
		oapiDeleteMesh(hMesh);
	}

	MeshCatalog.insert(this);
	if (pBuf) pBuf->Map(pDev);
}

// ===========================================================================================
//
VulkanMesh::VulkanMesh(MESHHANDLE hMesh, bool asTemplate, FVECTOR3 *reorig, float *scale) : VulkanEffect()
{
	Null();
	LoadMeshFromHandle(hMesh, reorig, scale);
	bIsTemplate = asTemplate;
	MeshCatalog.insert(this);
	if (pBuf) pBuf->Map(pDev);
}


// ===========================================================================================
//
VulkanMesh::VulkanMesh(DWORD groups, const MESHGROUPEX **hGroup, const SURFHANDLE *hSurf) : VulkanEffect()
{
	Null();
	nGrp = groups;
	Grp = new GROUPREC[nGrp]; memset((void*)Grp, 0, sizeof(GROUPREC) * nGrp);

	for (DWORD i=0;i<nGrp;i++) {
		SetGroupRec(i, hGroup[i]);
		Grp[i].TexIdxEx[0] = SPEC_DEFAULT;
		Grp[i].TexMixEx[0] = 0.0f;
		Grp[i].TexIdx  = i;
		Grp[i].MtrlIdx = SPEC_DEFAULT;
	}

	nMtrl = 0;
	nTex = nGrp+1;
	Tex = new lpSurfNative[nTex];
	Tex[0] = 0; // 'no texture'
	for (DWORD i=1;i<nTex;i++) Tex[i] = SURFACE(hSurf[i-1]);

	ProcessInherit();

	pBuf = new MeshBuffer(MaxVert, MaxFace, this);

	for (DWORD i=0;i<nGrp;i++) CopyVertices(&Grp[i], hGroup[i]);

	pGrpTF = new FMATRIX4[nGrp];

	VMAT_Identity(&mTransform);
	VMAT_Identity(&mTransformInv);
	MeshCatalog.insert(this);

	UpdateBoundingBox();
	CheckMeshStatus();

	pBuf->Map(pDev);
}


// ===========================================================================================
//
VulkanMesh::VulkanMesh(const MESHGROUPEX *pGroup, const MATERIAL *pMat, SurfNative *pTex) : VulkanEffect()
{
	Null();

	// template meshes are stored in system memory
	nGrp   = 1;
	Grp    = new GROUPREC[nGrp]; memset((void*)Grp, 0, sizeof(GROUPREC) * nGrp);
	nTex   = 2;
	Tex	   = new lpSurfNative[nTex];
	Tex[0] = 0; // 'no texture'
	Tex[1] = pTex;
	nMtrl  = 1;
	Mtrl   = new VulkanMatExt[nMtrl];
	pGrpTF = new FMATRIX4[nGrp];

	SetGroupRec(0, pGroup);

	pBuf = new MeshBuffer(MaxVert, MaxFace, this);

	// `(const D3DMATERIAL9*)pMat` on Windows -- a cast between two structures
	// that are field for field the same. D3DMATERIAL9 has no counterpart and
	// needs none: the overload takes the SDK's own MATERIAL, which is what
	// the caller already has, so the cast disappears rather than changing type.
	SetMaterial(pMat, 0, false);
	CopyVertices(&Grp[0], pGroup);

	VMAT_Identity(&mTransform);
	VMAT_Identity(&mTransformInv);
	MeshCatalog.insert(this);

	UpdateBoundingBox();
	CheckMeshStatus();

	pBuf->Map(pDev);
}


// ===========================================================================================
// Create new Instance from a template mesh
//
VulkanMesh::VulkanMesh(MESHHANDLE hMesh, const VulkanMesh &hTemp)
{
	Null(oapiGetMeshFilename(hMesh));

	// Confirm the source is global template
	assert(hTemp.bIsTemplate == true);

	nGrp = oapiMeshGroupCount(hMesh); assert(nGrp == hTemp.nGrp);

	if (nGrp == 0) return;

	strcpy_s(name, ARRAYSIZE(name), hTemp.name);

	// Use Template's Vertex Data directly, no need for a local copy unless locally modified. 
	pBuf = hTemp.pBuf;

	BBox = hTemp.BBox;
	MaxVert = hTemp.MaxVert;
	MaxFace = hTemp.MaxFace;
	// Clone group records from a tremplate
	Grp = new GROUPREC[nGrp];
	memcpy(Grp, hTemp.Grp, sizeof(GROUPREC)*nGrp);

	if (MaxVert == 0 || MaxFace == 0) return;

	// -----------------------------------------------------------------------
	nTex = oapiMeshTextureCount(hMesh) + 1;	assert(nTex == hTemp.nTex);
	Tex = new lpSurfNative[nTex];
	Tex[0] = 0; // 'no texture'
	for (DWORD i = 1; i<nTex; i++) Tex[i] = SURFACE(oapiGetTextureHandle(hMesh, i));

	// -----------------------------------------------------------------------
	nMtrl = oapiMeshMaterialCount(hMesh); assert(nMtrl == hTemp.nMtrl);
	if (nMtrl) Mtrl = new VulkanMatExt[nMtrl];
	for (DWORD i = 0; i<nMtrl; i++)	SetMaterial(oapiMeshMaterial(hMesh, i), i, false);

	pGrpTF = new FMATRIX4[nGrp];

	VMAT_Identity(&mTransform);
	VMAT_Identity(&mTransformInv);

	MeshCatalog.insert(this);

	UpdateBoundingBox();
	CheckMeshStatus();

	// No need to "pBuf->Map(pDev)" here, source template is already mapped 
}


// ===========================================================================================
//
VulkanMesh::~VulkanMesh()
{
	_TRACE;

	if (MeshCatalog.erase(this)) LogAlw("Mesh %s Removed from catalog", _PTR(this));
	else 						 LogErr("Mesh %s wasn't in meshcatalog", _PTR(this));

	Release();

	LogOk("Mesh %s Deleted successfully -------------------------------", _PTR(this));
}


// ===========================================================================================
//
void VulkanMesh::Release()
{
	SAFE_DELETEA(Locals);
	SAFE_DELETEA(Grp);
	SAFE_DELETEA(Tex);
	SAFE_DELETEA(Mtrl);
	SAFE_DELETEA(pGrpTF);
	SAFE_DELETEA(pTune);

	if (pBuf) if (pBuf->IsLocalTo(this)) delete pBuf;
}


// ===========================================================================================
// Reload a mesh file in response to VESSEL::MeshModified() call
//

void VulkanMesh::ReLoadMeshFromHandle(MESHHANDLE hMesh)
{
	const char* meshn = oapiGetMeshFilename(hMesh);
	strcpy_s(name, 128, meshn ? meshn : "???");

	// Relese buffers, Tex, Mtrl and Grp counts may have changed.
	SAFE_DELETEA(Tex);
	SAFE_DELETEA(Mtrl);
	SAFE_DELETEA(pGrpTF);
	SAFE_DELETEA(Grp);

	nGrp = oapiMeshGroupCount(hMesh);

	if (nGrp == 0) return;

	Grp = new GROUPREC[nGrp];
	memset((void*)Grp, 0, sizeof(GROUPREC) * nGrp);

	MaxFace = 0; // Incremented in SetGroupRec()
	MaxVert = 0;

	for (DWORD i = 0; i<nGrp; i++) SetGroupRec(i, oapiMeshGroupEx(hMesh, i));

	if (MaxVert == 0 || MaxFace == 0) {
		if (pBuf) if (pBuf->IsLocalTo(this)) {
			delete pBuf; pBuf = NULL;
		}
		return;
	}

	// If this is an instance, Create a local vertex buffers... 
	if (pBuf->IsLocalTo(this) == false) pBuf = new MeshBuffer(MaxVert, MaxFace, this);

	// -----------------------------------------------------------------------
	nTex = oapiMeshTextureCount(hMesh) + 1;
	Tex = new lpSurfNative[nTex];
	Tex[0] = 0; // 'no texture'
	for (DWORD i = 1; i<nTex; i++) Tex[i] = SURFACE(oapiGetTextureHandle(hMesh, i));
	// -----------------------------------------------------------------------
	nMtrl = oapiMeshMaterialCount(hMesh);
	if (nMtrl) Mtrl = new VulkanMatExt[nMtrl];
	for (DWORD i = 0; i<nMtrl; i++)	SetMaterial(oapiMeshMaterial(hMesh, i), i, false);
	// -----------------------------------------------------------------------

	ProcessInherit();

	for (DWORD i = 0; i<nGrp; i++) CopyVertices(&Grp[i], oapiMeshGroupEx(hMesh, i));

	pGrpTF = new FMATRIX4[nGrp];

	VMAT_Identity(&mTransform);
	VMAT_Identity(&mTransformInv);

	UpdateBoundingBox();
	CheckMeshStatus();

	pBuf->Map(pDev);
}



// ===========================================================================================
//
void VulkanMesh::LoadMeshFromHandle(MESHHANDLE hMesh, FVECTOR3 *reorig, float *scale)
{
	const char* meshn = oapiGetMeshFilename(hMesh);
	strcpy_s(name, 128, meshn ? meshn : "???");

	nGrp = oapiMeshGroupCount(hMesh);

	if (nGrp == 0) return;

	Grp = new GROUPREC[nGrp]; memset((void*)Grp, 0, sizeof(GROUPREC) * nGrp);
	
	for (DWORD i = 0; i<nGrp; i++) SetGroupRec(i, oapiMeshGroupEx(hMesh, i));

	if (MaxVert == 0 || MaxFace == 0) return;

	pBuf = new MeshBuffer(MaxVert, MaxFace, this);

	// -----------------------------------------------------------------------
	nTex = oapiMeshTextureCount(hMesh) + 1;
	Tex = new lpSurfNative[nTex];
	Tex[0] = 0; // 'no texture'
	for (DWORD i = 1; i<nTex; i++) Tex[i] = SURFACE(oapiGetTextureHandle(hMesh, i));

	// -----------------------------------------------------------------------
	nMtrl = oapiMeshMaterialCount(hMesh);
	if (nMtrl) Mtrl = new VulkanMatExt[nMtrl];
	for (DWORD i = 0; i<nMtrl; i++)	SetMaterial(oapiMeshMaterial(hMesh, i), i, false);

	ProcessInherit();

	for (DWORD i = 0; i<nGrp; i++) CopyVertices(&Grp[i], oapiMeshGroupEx(hMesh, i), reorig, scale);

	pGrpTF = new FMATRIX4[nGrp];

	VMAT_Identity(&mTransform);
	VMAT_Identity(&mTransformInv);

	UpdateBoundingBox();
	CheckMeshStatus();
}

// ===========================================================================================
//
void VulkanMesh::ReloadTextures()
{
	for (UINT i = 0; i < nTex; i++) if (Tex[i]) SURFACE(Tex[i])->Reload();
}

// ===========================================================================================
//
void VulkanMesh::SetName(const char *name_)
{
	if (name_) strcpy_s(this->name, ARRAYSIZE(this->name), name_);
}

// ===========================================================================================
//
void VulkanMesh::SetName(UINT idx)
{
	if ((strncmp(name, "???", 3) == 0) || (name[0] == 0)) sprintf_s(name, ARRAYSIZE(name), "MeshIdx-%u", idx);
}

// ===========================================================================================
//
bool VulkanMesh::HasShadow() const
{
	if (!IsOK()) return false;
	for (DWORD g=0; g<nGrp; g++) {
		if ((Grp[g].UsrFlag & 0x3) != 0) continue;
		//if (Grp[g].IntFlag & 3) continue;
		return true;
	}
	return false;
}


// ===========================================================================================
//
void VulkanMesh::ProcessInherit()
{
	_TRACE;
	if (!IsOK()) return;
	if (Grp[0].MtrlIdx == SPEC_INHERIT) Grp[0].MtrlIdx = SPEC_DEFAULT;
	if (Grp[0].TexIdx == SPEC_INHERIT) Grp[0].TexIdx = SPEC_DEFAULT;
	if (Grp[0].TexIdxEx[0] == SPEC_INHERIT) Grp[0].TexIdxEx[0] = SPEC_DEFAULT;

	bool bPopUp = false;

	for (DWORD i=0;i<nGrp;i++) {

		if (Grp[i].UsrFlag & 0x8) LogErr("MeshGroupFlag 0x8 in use (OPERATION NOT IMPLEMENTED)");

		// Inherit Material
		if (Grp[i].MtrlIdx == SPEC_INHERIT) Grp[i].MtrlIdx = Grp[i-1].MtrlIdx;

		// Inherit Texture
		if (Grp[i].TexIdx == SPEC_DEFAULT) Grp[i].TexIdx = 0;
		else if (Grp[i].TexIdx == SPEC_INHERIT) Grp[i].TexIdx = Grp[i-1].TexIdx;
		else Grp[i].TexIdx++;

		// Inherit Night Texture
		if (Grp[i].TexIdxEx[0] == SPEC_DEFAULT) Grp[i].TexIdxEx[0] = 0;
		else if (Grp[i].TexIdxEx[0] == SPEC_INHERIT) Grp[i].TexIdxEx[0] = Grp[i-1].TexIdxEx[0];
		else Grp[i].TexIdxEx[0]++;

		// Do some safety checks
		if (Grp[i].TexIdx>=nTex) {
			LogErr("Mesh(%s) has a texture index %u in group %u out of range.", _PTR(this), Grp[i].TexIdx, i);
			Grp[i].TexIdx = 0;
			bPopUp = true;
		}
		if (Grp[i].TexIdxEx[0]>=nTex) {
			LogErr("Mesh(%s) has a night texture index %u in group %u out of range.", _PTR(this), Grp[i].TexIdxEx[0], i);
			Grp[i].TexIdxEx[0] = 0;
			bPopUp = true;
		}

		if (Grp[i].MtrlIdx!=SPEC_DEFAULT) {
			if (Grp[i].MtrlIdx>=nMtrl) {
				LogErr("Mesh(%s) has a material index %u in group %u out of range.", _PTR(this), Grp[i].MtrlIdx, i);
				Grp[i].MtrlIdx = SPEC_DEFAULT;
				bPopUp = true;
			}
		}
	}
	if (bPopUp) MessageBoxA(NULL, "Invalid Mesh Detected", "VulkanClient Error:",MB_OK);
}


// ===========================================================================================
//
FVECTOR3 VulkanMesh::GetGroupSize(DWORD idx) const
{
	if (!IsOK()) return FVECTOR3(0,0,0);
	if (idx>=nGrp) return FVECTOR3(0,0,0);
	if (Grp[idx].nVert<2) return FVECTOR3(0,0,0);
	return FVECTOR3f4(Grp[idx].BBox.max - Grp[idx].BBox.min);
}


// ===========================================================================================
//
void VulkanMesh::ResetTransformations()
{
	_TRACE;
	if (!IsOK()) return;
	VMAT_Identity(&mTransform);
	VMAT_Identity(&mTransformInv);
	bGlobalTF = false;
	bBSRecompute = true;
	bBSRecomputeAll = true;
	for (DWORD i=0;i<nGrp;i++) {
		VMAT_Identity(&Grp[i].Transform);
		VMAT_Identity(&pGrpTF[i]);
		Grp[i].bTransform = false;
	}
}


// ===========================================================================================
// THE ONE FUNCTION IN THIS FILE THAT USED <xnamath.h>, and it is not Direct3D.
//
// XMVECTOR is DirectXMath's four-float SSE register type; XMLoadFloat3,
// XMVectorSet, XMVector3Normalize, XMVector3Dot and XMStoreFloat3 are the
// wrapper around the same _mm_* intrinsics GCC has always had. The arithmetic
// underneath is three-component add, subtract, scale, dot and normalise --
// every one of which FVECTOR3 already has -- so it is written out rather than
// replaced by a second wrapper. Same finding, and the same treatment, as
// VBase.cpp's CheckMeshStats.
//
// TWO SMALLER CONSEQUENCES:
//
//   _aligned_malloc / _aligned_free are MSVC CRT names with no counterpart
//   here (the C11 spelling is aligned_alloc and the C++17 one is a new
//   overload). They existed because XMVECTOR must be 16-byte aligned;
//   FVECTOR3 is three floats and needs no alignment request, so the
//   allocation is a plain new[]/delete[].
//
//   THE REFERENCE ALLOCATES nVtx+1 AND USES nVtx. The extra element is never
//   written or read -- both loops stop at nVtx -- so it is slack rather than
//   a guard, and the conversion allocates what is used.
//
// The one place accuracy could drift is XMVector3Dot, which returns a
// splatted vector where dot() returns a scalar; the expression multiplies it
// by n immediately, so the two are the same three products.
// ===========================================================================================
void VulkanMesh::UpdateTangentSpace(NMVERTEX *pVrt, WORD *pIdx, DWORD nVtx, DWORD nFace, bool bTextured)
{
	if (!IsOK()) return;

	if (bTextured) {

		FVECTOR3 *ta = new FVECTOR3[nVtx];
		FVECTOR3 zero = FVECTOR3(0, 0, 0);
		for (DWORD i = 0; i < nVtx; i++) ta[i] = zero;

		for (DWORD i=0;i<nFace;i++) {

			DWORD i0 = pIdx[i*3];
			DWORD i1 = pIdx[i*3+1];
			DWORD i2 = pIdx[i*3+2];

			FVECTOR3 r0 = FVECTOR3(pVrt[i0].x, pVrt[i0].y, pVrt[i0].z);
			FVECTOR3 r1 = FVECTOR3(pVrt[i1].x, pVrt[i1].y, pVrt[i1].z);
			FVECTOR3 r2 = FVECTOR3(pVrt[i2].x, pVrt[i2].y, pVrt[i2].z);
			FVECTOR2 t0 = FVECTOR2(pVrt[i0].u, pVrt[i0].v);
			FVECTOR2 t1 = FVECTOR2(pVrt[i1].u, pVrt[i1].v);
			FVECTOR2 t2 = FVECTOR2(pVrt[i2].u, pVrt[i2].v);

			float u0 = t1.x - t0.x;
			float v0 = t1.y - t0.y;
			float u1 = t2.x - t0.x;
			float v1 = t2.y - t0.y;

			FVECTOR3 k0 = r1 - r0;
			FVECTOR3 k1 = r2 - r0;

			float q = (u0*v1-u1*v0);
			if (q==0) q = 1.0f;
			else q = 1.0f / q;

			FVECTOR3 t = ((k0*v1 - k1*v0) * q);
			ta[i0]+=t; ta[i1]+=t; ta[i2]+=t;
			pVrt[i0].w = pVrt[i1].w = pVrt[i2].w = (q<0.0f ? 1.0f : -1.0f);
		}

		for (DWORD i=0;i<nVtx; i++) {
			FVECTOR3 n = unit(FVECTOR3(pVrt[i].nx, pVrt[i].ny, pVrt[i].nz));
			FVECTOR3 t = unit(ta[i] - n * dot(ta[i], n));
			pVrt[i].tx = t.x;
			pVrt[i].ty = t.y;
			pVrt[i].tz = t.z;
		}

		delete[] ta;
	}
	else {
		for (DWORD i=0;i<nVtx; i++) {
			FVECTOR3 n = FVECTOR3(pVrt[i].nx,  pVrt[i].ny,  pVrt[i].nz);
			FVECTOR3 t = Perpendicular(&n);
			t = unit(t);
			pVrt[i].tx = t.x;
			pVrt[i].ty = t.y;
			pVrt[i].tz = t.z;
		}
	}
}



// ===========================================================================================
//
void VulkanMesh::SetGroupRec(DWORD i, const MESHGROUPEX *mg)
{
	if (i>=nGrp) return;
	memcpy(Grp[i].TexIdxEx, mg->TexIdxEx, MAXTEX*sizeof(DWORD));
	memcpy(Grp[i].TexMixEx, mg->TexMixEx, MAXTEX*sizeof(float));
	Grp[i].TexIdx  = mg->TexIdx;
	Grp[i].MtrlIdx = mg->MtrlIdx;
	Grp[i].IdexOff = MaxFace*3;
	Grp[i].VertOff = MaxVert;
	Grp[i].nFace   = mg->nIdx/3;
	Grp[i].nVert   = mg->nVtx;
	Grp[i].UsrFlag = mg->UsrFlag;
	Grp[i].IntFlag = mg->Flags;
	Grp[i].zBias   = mg->zBias;

	VMAT_Identity(&Grp[i].Transform);

	MaxFace += Grp[i].nFace;
	MaxVert += Grp[i].nVert;
}


// ===========================================================================================
//
bool VulkanMesh::CopyVertices(GROUPREC *grp, const MESHGROUPEX *mg, FVECTOR3 *reorig, float *scale)
{
	if (!pBuf) return false;
	NTVERTEX *pNT = mg->Vtx;
	SMVERTEX *pShad = pBuf->pSBSys + grp->VertOff;
	NMVERTEX *pVert = pBuf->pVBSys + grp->VertOff;
	FVECTOR4 *pGeo = pBuf->pGBSys + grp->VertOff;
	WORD *pIndex = pBuf->pIBSys + grp->IdexOff;

	for (DWORD i=0;i<mg->nIdx;i++) pIndex[i] = mg->Idx[i];

	for (DWORD i=0;i<mg->nVtx; i++) {
		float x = pNT[i].nx; float y = pNT[i].ny; float z = pNT[i].nz;
		float b = 1.0f/sqrt(y*y+z*z+x*x);
		pVert[i].nx = (x*b);
		pVert[i].ny = (y*b);
		pVert[i].nz = (z*b);

		if (scale) {
			pVert[i].x = pNT[i].x * (*scale);
			pVert[i].y = pNT[i].y * (*scale);
			pVert[i].z = pNT[i].z * (*scale);
		}
		else {
			pVert[i].x = pNT[i].x;
			pVert[i].y = pNT[i].y;
			pVert[i].z = pNT[i].z;
		}

		pVert[i].u  = pNT[i].tu;
		pVert[i].v  = pNT[i].tv;
		pVert[i].w  = 1.0f;
		pVert[i].tx = 1.0f;
		pVert[i].ty = 0.0f;
		pVert[i].tz = 0.0f;

		if (reorig) {
			pVert[i].x += reorig->x;
			pVert[i].y += reorig->y;
			pVert[i].z += reorig->z;
		}

		pGeo[i] = FVECTOR4(pVert[i].x, pVert[i].y, pVert[i].z, 0.0f);

		pShad[i].x = pVert[i].x; pShad[i].y = pVert[i].y; pShad[i].z = pVert[i].z;
		pShad[i].tu = pVert[i].u; pShad[i].tv = pVert[i].v;
	}

	// Check vertex index errors (This is important)
	//
	for (DWORD i=0;i<(mg->nIdx/3);i++) {
		DWORD v0 = i*3;	DWORD v1 = v0+1; DWORD v2 = v0+2;
		if (pIndex[v0]>=mg->nVtx || pIndex[v1]>=mg->nVtx || pIndex[v2]>=mg->nVtx) {
			pIndex[v0] = pIndex[v1] = pIndex[v2] = 0;
		}
	}

	// For un-instanced mesh the base-offset is zero
	if (Config->UseNormalMap) UpdateTangentSpace(pVert, pIndex, mg->nVtx, mg->nIdx/3, grp->TexIdx!=0);

	if (mg->nVtx>0) BoundingBox(pVert, mg->nVtx, &grp->BBox);
	else D9ZeroAABB(&grp->BBox);

	return true;
}


// ===========================================================================================
// This is required by Client implementation see clbkEditMeshGroup
//
int VulkanMesh::EditGroup(DWORD grp, GROUPEDITSPEC *ges)
{
	_TRACE;
	if (!IsOK()) return 1;
	if (grp >= nGrp) return 1;
	if (!pBuf) return 1;

	bBSRecompute = true;

	GROUPREC *g = &Grp[grp];
	DWORD flag = ges->flags;
	// `DWORD old = g->UsrFlag;` stood here and is never read -- finding 35's
	// family, reported by GCC and not by MSVC.

	if (flag & GRPEDIT_SETUSERFLAG)	     g->UsrFlag  = ges->UsrFlag;
	else if (flag & GRPEDIT_ADDUSERFLAG) g->UsrFlag |= ges->UsrFlag;
	else if (flag & GRPEDIT_DELUSERFLAG) g->UsrFlag &= ~ges->UsrFlag;

	if (flag & GRPEDIT_VTX) {

		if (pBuf->IsLocalTo(this) == false) 
		{
			// Can't make modifications to a global template
			// Create a local copy of the Mesh
			// TODO: Create local copy of the group only
			pBuf = new MeshBuffer(pBuf, this);
		}

		pBuf->MustRemap(MAPMODE_CURRENT);

		SMVERTEX *pShad = pBuf->pSBSys + g->VertOff;
		FVECTOR4 *pGeo = pBuf->pGBSys + g->VertOff;
		NMVERTEX *vtx = pBuf->pVBSys + g->VertOff;
		WORD *idx = pBuf->pIBSys + g->IdexOff;

		DWORD i, vi;
		if (vtx) {
			for (i = 0; i < ges->nVtx; i++) {
				vi = (ges->vIdx ? ges->vIdx[i] : i);
				if (vi < g->nVert) {

					if      (flag & GRPEDIT_VTXCRDX)    vtx[vi].x   = ges->Vtx[i].x;
					else if (flag & GRPEDIT_VTXCRDADDX) vtx[vi].x  += ges->Vtx[i].x;
					if      (flag & GRPEDIT_VTXCRDY)    vtx[vi].y   = ges->Vtx[i].y;
					else if (flag & GRPEDIT_VTXCRDADDY) vtx[vi].y  += ges->Vtx[i].y;
					if      (flag & GRPEDIT_VTXCRDZ)    vtx[vi].z   = ges->Vtx[i].z;
					else if (flag & GRPEDIT_VTXCRDADDZ) vtx[vi].z  += ges->Vtx[i].z;
					if      (flag & GRPEDIT_VTXNMLX)    vtx[vi].nx  = ges->Vtx[i].nx;
					else if (flag & GRPEDIT_VTXNMLADDX) vtx[vi].nx += ges->Vtx[i].nx;
					if      (flag & GRPEDIT_VTXNMLY)    vtx[vi].ny  = ges->Vtx[i].ny;
					else if (flag & GRPEDIT_VTXNMLADDY) vtx[vi].ny += ges->Vtx[i].ny;
					if      (flag & GRPEDIT_VTXNMLZ)    vtx[vi].nz  = ges->Vtx[i].nz;
					else if (flag & GRPEDIT_VTXNMLADDZ) vtx[vi].nz += ges->Vtx[i].nz;
					if      (flag & GRPEDIT_VTXTEXU)    vtx[vi].u  = ges->Vtx[i].tu;
					else if (flag & GRPEDIT_VTXTEXADDU) vtx[vi].u += ges->Vtx[i].tu;
					if      (flag & GRPEDIT_VTXTEXV)    vtx[vi].v  = ges->Vtx[i].tv;
					else if (flag & GRPEDIT_VTXTEXADDV) vtx[vi].v += ges->Vtx[i].tv;

					if ((flag & GRPEDIT_VTXCRD)!=0 || (flag & GRPEDIT_VTXCRDADD)!=0) {
						pGeo[vi] = FVECTOR4(vtx[vi].x, vtx[vi].y, vtx[vi].z, 0.0f);

						pShad[vi].x = vtx[vi].x; pShad[vi].y = vtx[vi].y; pShad[vi].z = vtx[vi].z;
						pShad[vi].tu = vtx[vi].u; pShad[vi].tv = vtx[vi].v;
					}
				}
			}

			if (Config->UseNormalMap) UpdateTangentSpace(vtx, idx, g->nVert, g->nFace, g->TexIdx!=0);

			if (g->nVert>0) BoundingBox(vtx, g->nVert, &g->BBox);
			else D9ZeroAABB(&g->BBox);
		}
	}

	UpdateFlags();
	return 0;
}


NTVERTEX Convert(NMVERTEX &v)
{
	NTVERTEX n;
	n.x = v.x; n.y = v.y; n.z = v.z;
	n.nx = v.nx; n.ny = v.ny; n.nz = v.nz;
	n.tu = v.u;	n.tv = v.v;
	return n;
}


int VulkanMesh::GetGroup (DWORD grp, GROUPREQUESTSPEC *grs)
{
	if (!pBuf) return 1;

	static NTVERTEX zero = {0,0,0, 0,0,0, 0,0};
	if (grp >= nGrp) return 1;
	DWORD nv = Grp[grp].nVert;
	DWORD ni = Grp[grp].nFace*3;
	DWORD i, vi;
	int ret = 0;

	if (grs->nVtx && grs->Vtx) { // vertex data requested
		NMVERTEX *vtx = pBuf->pVBSys + Grp[grp].VertOff;
		if (vtx) {
			if (grs->VtxPerm) { // random access data request
				for (i = 0; i < grs->nVtx; i++) {
					vi = grs->VtxPerm[i];
					if (vi < nv) {
						grs->Vtx[i] = Convert(vtx[vi]);
					} else {
						grs->Vtx[i] = zero;
						ret = 1;
					}
				}
			} else {
				if (grs->nVtx > nv) grs->nVtx = nv;
				for (i=0;i<grs->nVtx;i++) grs->Vtx[i] = Convert(vtx[i]);
			}
		}
		else return 1;
	}

	if (grs->nIdx && grs->Idx) { // index data requested
		WORD *idx = pBuf->pIBSys + Grp[grp].IdexOff;
		if (idx) {
			if (grs->IdxPerm) { // random access data request
				for (i = 0; i < grs->nIdx; i++) {
					vi = grs->IdxPerm[i];
					if (vi < ni) {
						grs->Idx[i] = idx[vi];
					} else {
						grs->Idx[i] = 0;
						ret = 1;
					}
				}
			} else {
				if (grs->nIdx > ni) grs->nIdx = ni;
				for (i=0;i<grs->nIdx;i++) grs->Idx[i] = idx[i];
			}
		}
		else return 1;
	}

	grs->MtrlIdx = Grp[grp].MtrlIdx;
	grs->TexIdx = Grp[grp].TexIdx;
	return ret;
}

// ===========================================================================================
//
void VulkanMesh::UpdateFlags()
{
	Flags = 0;
	for (DWORD i = 0; i < nGrp; i++)
	{
		if (Grp[i].UsrFlag & 0x20) Flags |= 0x20;
	}
}

// ===========================================================================================
//
void VulkanMesh::SetMFDScreenId(DWORD idx, WORD id)
{
	if (idx<nGrp) Grp[idx].MFDScreenId = id;
}

// ===========================================================================================
//
bool VulkanMesh::SetTexture(DWORD texidx, SURFHANDLE tex)
{
	_TRACE;
	if (!IsOK()) return false;
	if (texidx >= nTex) {
		LogErr("VulkanMesh::SetTexture(%u, %s) index out of range",texidx, _PTR(tex));
		return false;
	}
	Tex[texidx] = (lpSurfNative)tex;
	LogBlu("VulkanMesh(%s)::SetTexture(%u, %s) (%s)", _PTR(this), texidx, _PTR(tex), SURFACE(tex)->GetName());
	CheckMeshStatus();
	return true;
}

// ===========================================================================================
//
DWORD VulkanMesh::GetMeshGroupMaterialIdx(DWORD idx) const
{
	if (!IsOK()) return 0;
	if (idx>=nGrp) return 0;
	return Grp[idx].MtrlIdx;
}

// ===========================================================================================
//
DWORD VulkanMesh::GetMeshGroupTextureIdx(DWORD idx) const
{
	if (!IsOK()) return 0;
	if (idx>=nGrp) return 0;
	return Grp[idx].TexIdx;
}

// ===========================================================================================
//
bool VulkanMesh::HasTexture(SURFHANDLE hSurf) const
{
	if (!IsOK()) return false;
	for (DWORD i=0;i<nTex;i++) if (Tex[i]==hSurf) return true;
	return false;
}

// ===========================================================================================
//
void VulkanMesh::SetTexMixture(DWORD ntex, float mix)
{
	_TRACE;
	if (!IsOK()) return;
	ntex--;
	for (DWORD g = 0; g < nGrp; g++) if (Grp[g].TexIdxEx[ntex] != SPEC_DEFAULT) Grp[g].TexMixEx[ntex] = mix;
}

// ===========================================================================================
//
void VulkanMesh::SetSunLight(const VulkanSun *light)
{
	memcpy(&sunLight, light, sizeof(VulkanSun));
}

// ===========================================================================================
//
DWORD VulkanMesh::GetVertexCount(int grp) const
{
	if (grp<0) return MaxVert;
	else return Grp[grp].nVert;
}

// ===========================================================================================
//
DWORD VulkanMesh::GetIndexCount(int grp) const
{
	if (grp<0) return MaxFace*3;
	else return Grp[grp].nFace*3;
}

// ===========================================================================================
//
DWORD VulkanMesh::GetGroupTransformCount() const
{
	DWORD cnt = 0;
	for (DWORD i=0;i<nGrp;i++) if (Grp[i].bTransform) cnt++;
	return cnt;
}

// ===========================================================================================
//
const VulkanMesh::GROUPREC *VulkanMesh::GetGroup(DWORD idx) const
{
	if (!IsOK()) return NULL;
	if (idx<nGrp) return &Grp[idx];
	return NULL;
}

// ===========================================================================================
//
const VulkanMatExt * VulkanMesh::GetMaterial(DWORD idx) const
{
	if (idx >= nMtrl) return NULL;
	return &Mtrl[idx];
}

// ===========================================================================================
//
bool VulkanMesh::GetMaterial(VulkanMatExt *pMat, DWORD idx) const
{
	if (pMat && idx<nMtrl) {
		memcpy(pMat, &Mtrl[idx], sizeof(VulkanMatExt));
		return true;
	}
	return false;
}

// ===========================================================================================
// D3DMATERIAL9 -> MATERIAL. The two are five COLOUR4-shaped fields and a
// float, in the same order; CreateMatExt already took the SDK's own type on
// Windows through a cast at every call site. See VulkanUtil.h's note 2.
//
void VulkanMesh::SetMaterial(const MATERIAL *pMat, DWORD idx, bool bStat)
{
	VulkanMatExt Mat;
	// Faulty output coming from oapiMeshMaterial, why ????
	if (pMat <= ((void*)0x100)) { 
		CreateDefaultMat(&Mat);
	} else CreateMatExt(pMat, &Mat);
	SetMaterial(&Mat, idx, bStat);
}

// ===========================================================================================
//
void VulkanMesh::SetMaterial(const VulkanMatExt *pMat, DWORD idx, bool bStat)
{
	if (idx < nMtrl) {
		memcpy(&Mtrl[idx], pMat, sizeof(VulkanMatExt));

		if (Mtrl[idx].Specular.w < 0.1f) {
			Mtrl[idx].Specular.x = 0.0f;
			Mtrl[idx].Specular.y = 0.0f;
			Mtrl[idx].Specular.z = 0.0f;
		}
	}
	if (bStat) CheckMeshStatus();
}


// ===========================================================================================
//0 = success, 1=no graphics engine attached,
//2 = graphics engine does not support operation, 3 = invalid mesh handle,
//4 = material index out of range, 5 = material property not supported by shader used by the mesh.
//
// EVERY `*((D3DXVECTOR4*)value)` AND `*((D3DXVECTOR3*)value)` CAST DISAPPEARS.
// The parameter is already an FVECTOR4 and the members are already FVECTOR3/4,
// so what was a reinterpret_cast between two layout-compatible types is now an
// assignment between the types themselves -- and for the FVECTOR3 members it
// is `value->xyz`, the union view DrawAPI.h already provides, rather than a
// cast that would read four floats out of a three-float member.
//
int VulkanMesh::SetMaterialEx(DWORD idx, MatProp mid, const FVECTOR4* value)
{

	if (idx >= nMtrl) return 4;

	// SET ---------------------------------------------------------------
	if (value) {
		switch (mid) {
		case MatProp::Diffuse:
			Mtrl[idx].Diffuse = *value;
			Mtrl[idx].ModFlags |= VULKANMATEX_DIFFUSE;
			bMtrlModidied = true;
			return 0;
		case MatProp::Ambient:
			Mtrl[idx].Ambient = value->xyz;
			Mtrl[idx].ModFlags |= VULKANMATEX_AMBIENT;
			bMtrlModidied = true;
			return 0;
		case MatProp::Specular:
			Mtrl[idx].Specular = *value;
			Mtrl[idx].ModFlags |= VULKANMATEX_SPECULAR;
			bMtrlModidied = true;
			return 0;
		case MatProp::Light:
			Mtrl[idx].Emissive = value->xyz;
			Mtrl[idx].ModFlags |= VULKANMATEX_EMISSIVE;
			bMtrlModidied = true;
			return 0;
		case MatProp::Emission:
			Mtrl[idx].Emission2 = value->xyz;
			Mtrl[idx].ModFlags |= VULKANMATEX_EMISSION2;
			bMtrlModidied = true;
			return 0;
		case MatProp::Reflect:
			Mtrl[idx].Reflect = value->xyz;
			Mtrl[idx].ModFlags |= VULKANMATEX_REFLECT;
			bMtrlModidied = true;
			return 0;
		case MatProp::Smooth:
			Mtrl[idx].Roughness = FVECTOR2(value->g, value->r);
			Mtrl[idx].ModFlags |= VULKANMATEX_ROUGHNESS;
			bMtrlModidied = true;
			return 0;
		case MatProp::Fresnel:
			Mtrl[idx].Fresnel = value->xyz;
			Mtrl[idx].ModFlags |= VULKANMATEX_FRESNEL;
			bMtrlModidied = true;
			return 0;
		case MatProp::Metal:
			Mtrl[idx].Metalness = value->r;
			Mtrl[idx].ModFlags |= VULKANMATEX_METALNESS;
			bMtrlModidied = true;
			return 0;
		case MatProp::SpecialFX:
			Mtrl[idx].SpecialFX = *value;
			Mtrl[idx].ModFlags |= VULKANMATEX_SPECIALFX;
			bMtrlModidied = true;
			return 0;
		}
		return 5;
	}

	// CLEAR -------------------------------------------------------------

	if (value == NULL)
	{
		switch (mid) {
		case MatProp::Diffuse:
			Mtrl[idx].Diffuse = FVECTOR4(1.0f, 1.0f, 1.0f, 1.0f);
			Mtrl[idx].ModFlags &= (~VULKANMATEX_DIFFUSE);
			bMtrlModidied = true;
			return 0;
		case MatProp::Ambient:
			Mtrl[idx].Ambient = FVECTOR3(1.0f, 1.0f, 1.0f);
			Mtrl[idx].ModFlags &= (~VULKANMATEX_AMBIENT);
			bMtrlModidied = true;
			return 0;
		case MatProp::Specular:
			Mtrl[idx].Specular = FVECTOR4(1.0f, 1.0f, 1.0f, 20.0f);
			Mtrl[idx].ModFlags &= (~VULKANMATEX_SPECULAR);
			bMtrlModidied = true;
			return 0;
		case MatProp::Light:
			Mtrl[idx].Emissive = FVECTOR3(0.0f, 0.0f, 0.0f);
			Mtrl[idx].ModFlags &= (~VULKANMATEX_EMISSIVE);
			bMtrlModidied = true;
			return 0;
		case MatProp::Emission:
			Mtrl[idx].Emission2 = FVECTOR3(1.0f, 1.0f, 1.0f);
			Mtrl[idx].ModFlags &= (~VULKANMATEX_EMISSION2);
			bMtrlModidied = true;
			return 0;
		case MatProp::Reflect:
			Mtrl[idx].Reflect = FVECTOR3(0.0f, 0.0f, 0.0f);
			Mtrl[idx].ModFlags &= (~VULKANMATEX_REFLECT);
			bMtrlModidied = true;
			return 0;
		case MatProp::Smooth:
			Mtrl[idx].Roughness = FVECTOR2(0.0f, 1.0f);
			Mtrl[idx].ModFlags &= (~VULKANMATEX_ROUGHNESS);
			bMtrlModidied = true;
			return 0;
		case MatProp::Fresnel:
			Mtrl[idx].Fresnel = FVECTOR3(1.0f, 0.0f, 1024.0f);
			Mtrl[idx].ModFlags &= (~VULKANMATEX_FRESNEL);
			bMtrlModidied = true;
			return 0;
		case MatProp::Metal:
			Mtrl[idx].Metalness = 0.0f;
			Mtrl[idx].ModFlags &= (~VULKANMATEX_METALNESS);
			bMtrlModidied = true;
			return 0;
		case MatProp::SpecialFX:
			Mtrl[idx].SpecialFX = FVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
			Mtrl[idx].ModFlags &= (~VULKANMATEX_SPECIALFX);
			bMtrlModidied = true;
			return 0;
		}
		return 5;
	}
	return 5;
}


// ===========================================================================================
//0 = success, 1=no graphics engine attached,
//2 = graphics engine does not support operation, 3 = invalid mesh handle,
//4 = material index out of range, 5 = material property not supported by shader used by the mesh.
//
int VulkanMesh::GetMaterialEx(DWORD idx, MatProp mid, FVECTOR4* value)
{
	if (idx >= nMtrl) return 4;

	if (value)
	{
		switch (mid)
		{
		case MatProp::Diffuse:
			*value = Mtrl[idx].Diffuse;
			return 0;
		case MatProp::Ambient:
			value->xyz = Mtrl[idx].Ambient;
			return 0;
		case MatProp::Specular:
			*value = Mtrl[idx].Specular;
			return 0;
		case MatProp::Light:
			value->xyz = Mtrl[idx].Emissive;
			return 0;
		case  MatProp::Emission:
			if ((Mtrl[idx].ModFlags&VULKANMATEX_EMISSION2) == 0) return -2;
			value->xyz = Mtrl[idx].Emission2;
			return 0;
		case MatProp::Reflect:
			if ((Mtrl[idx].ModFlags&VULKANMATEX_REFLECT) == 0) return -2;
			value->xyz = Mtrl[idx].Reflect;
			return 0;
		case MatProp::Smooth:
			if ((Mtrl[idx].ModFlags&VULKANMATEX_ROUGHNESS) == 0) return -2;
			value->g = Mtrl[idx].Roughness.x;
			value->r = Mtrl[idx].Roughness.y;
			return 0;
		case MatProp::Fresnel:
			if ((Mtrl[idx].ModFlags&VULKANMATEX_FRESNEL) == 0) return -2;
			value->xyz = Mtrl[idx].Fresnel;
			return 0;
		case MatProp::Metal:
			if ((Mtrl[idx].ModFlags& VULKANMATEX_METALNESS) == 0) return -2;
			value->r = Mtrl[idx].Metalness;
			return 0;
		case MatProp::SpecialFX:
			if ((Mtrl[idx].ModFlags& VULKANMATEX_SPECIALFX) == 0) return -2;
			*value = Mtrl[idx].SpecialFX;
			return 0;
		}
		return 5;
	}
	return 5;
}

// ===========================================================================================
//
bool VulkanMesh::GetTexTune(VulkanTune *pT, DWORD idx) const
{
	if (idx<nTex && idx!=0) {
		if (!pTune) VulkanTuneInit(pT);
		else memcpy(pT, &pTune[idx], sizeof(VulkanTune));
		return true;
	}
	return false;
}

// ===========================================================================================
//
void VulkanMesh::SetTexTune(const VulkanTune *pT, DWORD idx)
{
	if (idx < nTex && idx != 0) {
		if (!pTune) {
			pTune = new VulkanTune[nTex];
			for (DWORD i = 0; i < nTex; i++) VulkanTuneInit(&pTune[i]);
		}
		memcpy(&pTune[idx], pT, sizeof(VulkanTune));
	}
}

// ===========================================================================================
// D3DCOLOR -> DWORD, the same 0xAARRGGBB packing under a name that does not
// claim to be a Direct3D type.
//
void VulkanMesh::SetAmbientColor(DWORD c)
{
	_TRACE;
	if (!IsOK()) return;
	cAmbient = c;
}

// ===========================================================================================
//
void VulkanMesh::SetupFog(const FMATRIX4 *pW)
{
	_TRACE;
	if (!IsOK()) return;
	FX->SetVector(eAttennuate, ptr(FVECTOR4(1.0f,1.0f,1.0f,1.0f)));
	FX->SetVector(eInScatter,  ptr(FVECTOR4(0.0f,0.0f,0.0f,0.0f)));
}

// ===========================================================================================
//
void VulkanMesh::RenderGroup(int idx)
{
	RenderGroup(GetGroup(idx));
}

// ===========================================================================================
// Was SetVertexDeclaration + SetStreamSource + SetIndices + DrawIndexedPrimitive.
//
//   SetStreamSource / SetIndices -> vkCmdBindVertexBuffers /
//   vkCmdBindIndexBuffer, which are COMMANDS recorded into the frame's command
//   buffer rather than state set on a device. D3DFMT_INDEX16 reappears here as
//   VK_INDEX_TYPE_UINT16: the index type is a bind-time argument in Vulkan and
//   a creation-time one in D3D9.
//
//   DrawIndexedPrimitive(TRIANGLELIST, BaseVertexIndex, MinIndex, NumVertices,
//   StartIndex, PrimitiveCount) -> vkCmdDrawIndexed(indexCount, 1, firstIndex,
//   vertexOffset, 0). THE COUNT CHANGES MEANING: D3D9 takes a PRIMITIVE count
//   and Vulkan an INDEX count, so nFace becomes nFace*3. MinIndex and
//   NumVertices have no counterpart -- they were a hint to the software vertex
//   processing path. Same conversion as SketchMesh::RenderGroup's.
//
//   SetVertexDeclaration IS PIPELINE STATE HERE, and that changes WHEN it
//   takes effect. On Windows it is device state read at the draw, so setting
//   it one line before DrawIndexedPrimitive works. A VkPipeline bakes the
//   vertex layout in and is bound at BeginPass, so this call reaches the NEXT
//   pipeline build, not the draw below. It is kept in the reference's place
//   because it is still the declaration this mesh needs; a caller drawing
//   inside an already-open pass must have declared it before Begin().
//
void VulkanMesh::RenderGroup(const GROUPREC *grp)
{
	_TRACE;
	if (!IsOK()) return;
	if (!grp) return;

	pBuf->Map(pDev);

	VkCommandBuffer cmd = pDev->GetCommandBuffer();
	if (!cmd || !pBuf->pVB || !pBuf->pIB) return;

	// AND THE TOPOLOGY WITH IT, AND LEAVING IT OUT COST THE WHOLE COCKPIT.
	//
	// `D3DPT_TRIANGLELIST` is an ARGUMENT to DrawIndexedPrimitive on Windows,
	// named afresh at every draw. Here it is pipeline state held on the shared
	// VulkanEffectFile and it STICKS: BeaconArray.cpp sets POINT_LIST for the
	// beacon sprites and nothing puts it back, so every mesh drawn after a
	// vessel with beacons -- the virtual cockpit included -- was built into a
	// POINT_LIST pipeline and rasterised as 110 groups of one-pixel dots.
	//
	// It presented as "the VC renders in about one run in five", because
	// whether a beacon draw had happened first depends on load order. The
	// validation layer named it and was not believed at first, because it
	// names the SHADER rather than the draw:
	//
	//     VUID-VkGraphicsPipelineCreateInfo-topology-08773
	//     ... topology is POINT_LIST, but PointSize is not written ...
	//     POINTTRACE fx-pass: technique=VesselTech vs=PBR_VS ps=PBR_PS
	//     POINTTRACE fx-pass: technique=ShadowTech vs=ShadowMeshTechExVS
	//
	// -- VesselTech is the vessel mesh shader; it has no business in a point
	// pipeline at all.
	//
	// So every draw site names its own topology, exactly as every
	// DrawPrimitive call on Windows names its own D3DPRIMITIVETYPE. Same
	// reasoning as the one on SetVertexDecl above: it must be set before
	// Begin(), because it reaches the pipeline build and not the draw.
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	FX->SetVertexDecl(pMeshVertexDecl);

	VkBuffer vb = pBuf->pVB->Buffer();
	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
	vkCmdBindIndexBuffer(cmd, pBuf->pIB->Buffer(), 0, VK_INDEX_TYPE_UINT16);
	vkCmdDrawIndexed(cmd, grp->nFace*3, 1, grp->IdexOff, (int32_t)grp->VertOff, 0);

	VulkanStats.Mesh.Vertices += grp->nVert;
	VulkanStats.Mesh.MeshGrps++;
}


// reset stucts template
//
// `_p = { 0 }` on Windows, chosen by a __cplusplus test between two spellings
// that differ only in where the zero comes from. `{}` is the same
// value-initialisation in every standard from C++11 on, so the test has
// nothing left to select, and it is what -Wmissing-field-initializers does not
// report. Finding 36's family.
template <typename T> void reset (T& _p)
	{ _p = {}; }


// ===========================================================================================
//
void VulkanMesh::ResetRenderStatus()
{
	for (DWORD g = 0; g < nGrp; g++) Grp[g].bRendered = false;
}


// ===========================================================================================
//
bool VulkanMesh::IsGroupRendered(DWORD idx) const
{
	return Grp[idx].bRendered;
}


// ================================================================================================
// Analyze mesh. Must be called when ever there is a change in textures or materials
//
void VulkanMesh::CheckMeshStatus()
{
	UpdateFlags();

	bCanRenderFast = true;
	bIsReflective = false;
	VulkanMatExt *mat = NULL;

	for (DWORD g = 0; g < nGrp; g++) {

		Grp[g].Shader = SHADER_PBR;
		Grp[g].PBRStatus = 0;

		DWORD ti = Grp[g].TexIdx;

		if (Tex[ti] != NULL) {
			if (Tex[ti]->IsAdvanced()) {
				bCanRenderFast = false;
				if (Tex[ti]->GetMap(MAP_METALNESS) && (DefShader == SHADER_NULL)) DefShader = SHADER_METALNESS;
				if (Tex[ti]->GetMap(MAP_SPECULAR)) Grp[g].PBRStatus |= 0x2;
				if (Tex[ti]->GetMap(MAP_ROUGHNESS)) Grp[g].PBRStatus |= 0x4;
				if (Tex[ti]->GetMap(MAP_REFLECTION)) Grp[g].PBRStatus |= 0x8;
				if (Tex[ti]->GetMap(MAP_TRANSLUCENCE)) Grp[g].Shader = SHADER_ADV;
				if (Tex[ti]->GetMap(MAP_TRANSMITTANCE)) Grp[g].Shader = SHADER_ADV;
			}
		}

		if (Grp[g].MtrlIdx == SPEC_DEFAULT) mat = &defmat;
		else mat = &Mtrl[Grp[g].MtrlIdx];

		if (mat->ModFlags&VULKANMATEX_SPECULAR) Grp[g].PBRStatus |= 0x1;
		if (mat->ModFlags&VULKANMATEX_REFLECT) Grp[g].PBRStatus |= 0x8;
		if (mat->ModFlags&VULKANMATEX_ROUGHNESS) Grp[g].PBRStatus |= 0x4;
		if (mat->ModFlags&VULKANMATEX_FRESNEL) Grp[g].PBRStatus |= 0x10;
	}


	for (DWORD g = 0; g < nGrp; g++) {
		if (Grp[g].PBRStatus >= (0x8|0x2)) bIsReflective = true;
		if (Grp[g].PBRStatus >= 0x2) bCanRenderFast = false;
	}

	if (DefShader == SHADER_METALNESS) {
		bIsReflective = true;
		for (DWORD g = 0; g < nGrp; g++) Grp[g].Shader = SHADER_METALNESS;
	}

	// Which of the two draw paths this mesh will take, and why. Render()
	// hands the mesh to RenderFast() -- the FAST_VS/FAST_PS pass P2 -- when
	// bCanRenderFast is set, and keeps it on the PBR pass P0 otherwise, and
	// the two do not agree about a translucent group: FAST_PS writes
	// `cDiff.a = gMtrlAlpha`, PBR_PS writes `saturate(gMtrlAlpha + fTot)` and
	// so turns a 0.15-alpha glass opaque wherever it reflects. Only a texture
	// carrying a specular or roughness map (PBRStatus >= 0x2) clears
	// bCanRenderFast, so name the group and the texture that did it.
	// Diagnostic only; env-gated.
	{
		static const bool bTraceGrp = (getenv("ORBITER_VK_TRACE_GRP") != NULL);
		if (bTraceGrp && nGrp > 64) {
			LogErr("MESHSTAT '%s': nGrp=%u bCanRenderFast=%d bIsReflective=%d DefShader=%d",
				   name ? name : "(unnamed)", nGrp, int(bCanRenderFast),
				   int(bIsReflective), int(DefShader));
			std::map<DWORD, int> shown;
			for (DWORD g = 0; g < nGrp; g++) {
				const DWORD ti = Grp[g].TexIdx;
				const bool bAdv = (ti && Tex[ti] && Tex[ti]->IsAdvanced());
				if (Grp[g].PBRStatus < 0x2 && !bAdv) continue;
				if (shown.find(ti) != shown.end() && Grp[g].PBRStatus < 0x2) continue;
				shown[ti] = 1;
				LogErr("MESHSTAT   grp %3u PBRStatus=0x%X tex=%u[%s] advanced=%d "
					   "norm=%d spec=%d rghn=%d refl=%d emis=%d heat=%d metl=%d "
					   "transl=%d transm=%d",
					   g, (unsigned)Grp[g].PBRStatus, ti,
					   (ti && Tex[ti]) ? Tex[ti]->GetName() : "(none)",
					   (ti && Tex[ti]) ? int(Tex[ti]->IsAdvanced()) : -1,
					   (ti && Tex[ti]) ? (Tex[ti]->GetMap(MAP_NORMAL) != NULL) : -1,
					   (ti && Tex[ti]) ? (Tex[ti]->GetMap(MAP_SPECULAR) != NULL) : -1,
					   (ti && Tex[ti]) ? (Tex[ti]->GetMap(MAP_ROUGHNESS) != NULL) : -1,
					   (ti && Tex[ti]) ? (Tex[ti]->GetMap(MAP_REFLECTION) != NULL) : -1,
					   (ti && Tex[ti]) ? (Tex[ti]->GetMap(MAP_EMISSION) != NULL) : -1,
					   (ti && Tex[ti]) ? (Tex[ti]->GetMap(MAP_HEAT) != NULL) : -1,
					   (ti && Tex[ti]) ? (Tex[ti]->GetMap(MAP_METALNESS) != NULL) : -1,
					   (ti && Tex[ti]) ? (Tex[ti]->GetMap(MAP_TRANSLUCENCE) != NULL) : -1,
					   (ti && Tex[ti]) ? (Tex[ti]->GetMap(MAP_TRANSMITTANCE) != NULL) : -1);
			}
		}
	}
}


// ===========================================================================================
//
void VulkanMesh::ConfigureAtmo()
{
	//LogSunLight(sunLight);
	float x = 1.0f - saturate(max(sunLight.Color.r, sunLight.Color.b) * 2.0f);
	FX->SetFloat(eNight, x);
	FX->SetValue(eSun, &sunLight, sizeof(VulkanSun));
}


// ================================================================================================
// This is a rendering routine for a Exterior Mesh, non-spherical moons/asteroids
//
// THE PASS STRUCTURE CHANGES, and it is the one substantial change in this
// function. On Windows the loop opens a pass when the group's SHADER changes,
// writes the group's parameters, calls FX->CommitChanges() and draws --
// because D3DX buffered parameter writes and CommitChanges pushed them to the
// device inside the open pass.
//
// There is nothing to commit here. VulkanEffectFile uploads the uniform block
// and writes the descriptor set AT BeginPass, so anything set after it does
// not reach the draw. So the pass opens ONCE PER DRAW, after every per-group
// value has been written -- which is exactly the conversion SurfMgr, CloudMgr
// and Particle.cpp already took, and CommitChanges disappears rather than
// becoming a no-op.
//
// THE FOUR RENDER STATES SET AROUND THE DRAW SPLIT THREE WAYS:
//
//   D3DRS_CULLMODE (DBG_FLAGS_DUALSIDED, and Grp[].bDualSided's CW pass),
//   D3DRS_ZENABLE (RENDER_BASEBS and the HUD), D3DRS_ZWRITEENABLE (the
//   dual-sided pass) and D3DRS_DESTBLEND (the HUD) are all pipeline state
//   here, so they become a PassOverride handed to BeginPassEx BEFORE the
//   bind, not device state set after it. The reference's own comment on the
//   BASEBS line -- "Must be here because BeginPass() sets it enabled" -- is
//   the D3DX behaviour it was working around, and it inverts: the override
//   must now come first.
//
//   D3DRS_MULTISAMPLEANTIALIAS (the bOIT save/restore) HAS NO COUNTERPART AND
//   NEEDS NONE. The core's render pass is single-sampled (UIHost.cpp creates
//   it with VK_SAMPLE_COUNT_1_BIT), so there is no resolve to disable, and
//   Vulkan has no core dynamic state for it in any case. Both the GetRenderState
//   and the SetRenderState disappear, together with dwMSAA. Same finding, and
//   the same reasoning, as Surfmgr2.cpp's.
//
//   The restore lines -- ZENABLE 1, ZWRITEENABLE 1, DESTBLEND INVSRCALPHA,
//   CULLMODE CCW -- have nothing to restore. A pipeline is not layered over;
//   the next BeginPassEx builds from the pass's own state plus its override.
//
void VulkanMesh::Render(const FMATRIX4 *pW, int iTech, VulkanTexture **pEnv, int nEnv)
{

	_TRACE;
	
	if (!IsOK()) return;

	pBuf->Map(pDev);

	// Check material status
	//
	if (bMtrlModidied) {
		CheckMeshStatus();
		bMtrlModidied = false;
	}

	if (DebugControls::IsActive() == false) {
		if (bCanRenderFast && vClass != VCLASS_XR2) {
			RenderFast(pW, iTech);
			return;
		}
	}

	DWORD flags=0, selmsh=0, selgrp=0, displ=0; // Debug Variables
	bool bActiveVisual = false;

	const VCHUDSPEC *hudspec = NULL;

	if (DebugControls::IsActive()) {
		flags  = *(DWORD*)gc->GetConfigParam(CFGPRM_GETDEBUGFLAGS);
		selmsh = *(DWORD*)gc->GetConfigParam(CFGPRM_GETSELECTEDMESH);
		selgrp = *(DWORD*)gc->GetConfigParam(CFGPRM_GETSELECTEDGROUP);
		displ  = *(DWORD*)gc->GetConfigParam(CFGPRM_GETDISPLAYMODE);
		bActiveVisual = (pCurrentVisual==DebugControls::GetVisual());
		if (displ>0 && !bActiveVisual) return;
		if ((displ==2 || displ==3) && uCurrentMesh!=selmsh) return;
	}

	Scene *scn = gc->GetScene();

	bool bWorldMesh = false;
	bool bMeshCull = true;
	bool bTextured = true;
	bool bGroupCull = true;
	bool bUpdateFlow = true;
	bool bShadowProjection = false;



	switch (iTech) {
		case RENDER_VC:
			EnablePlanetGlow(false);
			break;
		case RENDER_BASE:
			EnablePlanetGlow(false);
			bMeshCull = false;
			bShadowProjection = true;
			break;
		case RENDER_BASEBS:
			EnablePlanetGlow(false);
			bMeshCull = false;
			bShadowProjection = true;
			break;
		case RENDER_ASTEROID:
			EnablePlanetGlow(false);
			bMeshCull = false;
			bGroupCull = false;
			bShadowProjection = true;
			break;
		case RENDER_VESSEL:
			EnablePlanetGlow(true);
			break;
	}

	VulkanEffect::FX->SetBool(VulkanEffect::eBaseBuilding, bShadowProjection);

	FVECTOR4 Field;
	FMATRIX4 mWorldView, q;

	VMAT_MatrixMultiply(&mWorldView, pW, scn->GetViewMatrix());

	if (bMeshCull || bGroupCull) Field = D9LinearFieldOfView(scn->GetProjectionMatrix());

	if (bMeshCull) if (!D9IsAABBVisible(&BBox, &mWorldView, &Field)) {
		if (flags&(DBG_FLAGS_BOXES|DBG_FLAGS_SPHERES)) RenderBoundingBox(pW);
		return;
	}

	FMATRIX4 mWorldMesh;

	if (bGlobalTF) VMAT_MatrixMultiply(&mWorldMesh, &mTransform, pW);
	else mWorldMesh = *pW;

	VulkanStats.Mesh.Meshes++;

	VulkanMatExt *mat, *old_mat = NULL;
	SURFHANDLE old_tex = NULL;

	VkCommandBuffer cmd = pDev->GetCommandBuffer();
	if (!cmd || !pBuf->pVB || !pBuf->pIB) return;

	// The D3DPT_TRIANGLELIST of every DrawIndexedPrimitive below, named here
	// because topology is pipeline state and STICKS across draws. See the long
	// note in RenderGroup: without it a beacon draw's POINT_LIST reached this
	// mesh and the vessel came out as a scatter of one-pixel dots.
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	FX->SetVertexDecl(pMeshVertexDecl);
	{
		VkBuffer vb = pBuf->pVB->Buffer();
		VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
		vkCmdBindIndexBuffer(cmd, pBuf->pIB->Buffer(), 0, VK_INDEX_TYPE_UINT16);
	}

	// The base override every pass in this call inherits. See the note above.
	VulkanEffectFile::PassOverride ovrBase;
	if (flags&DBG_FLAGS_DUALSIDED) ovrBase.cullMode = VulkanEffectFile::PassOverride::CULL_NONE;
	if (iTech == RENDER_BASEBS) ovrBase.depthTest = 0;

	FX->SetTechnique(eVesselTech);
	FX->SetBool(eFresnel, false);
	FX->SetBool(eEnvMapEnable, false);
	FX->SetBool(eTuneEnabled, false);
	FX->SetBool(eLightsEnabled, false);
	FX->SetBool(eOITEnable, false);
	FX->SetVector(eColor, ptr(FVECTOR4(0.0f, 0.0f, 0.0f, 0.0f)));

	ConfigureAtmo();

	if (DebugControls::IsActive()) if (pTune) FX->SetBool(eTuneEnabled, true);


	TexFlow FC;	reset(FC);

	const VulkanLight *pLights = gc->GetScene()->GetLights();
	int nSceneLights = gc->GetScene()->GetLightCount();

	for (int i = 0; i < Config->MaxLights(); i++) memcpy(&Locals[i], &null_light, sizeof(LightStruct));

	int nMeshLights = 0;

	if (pLights && nSceneLights>0) {

		// D3DXVec3TransformCoord(&pos, ptr(D3DXVECTOR3f4(BBox.bs)), pW) --
		// the SDK's own TransformCoord, which returns rather than writing
		// through an out-parameter.
		FVECTOR3 pos = oapi::TransformCoord(FVECTOR3f4(BBox.bs), *pW);

		// Find all local lights effecting this mesh ------------------------------------------
		//
		for (int i = 0; i < nSceneLights; i++) {
			float il = pLights[i].GetIlluminance(pos, BBox.bs.w);
			if (il > 0.005f) {
				LightList[nMeshLights].illuminace = il;
				LightList[nMeshLights++].idx = i;
			}
		}

		if (nMeshLights > 0) {

			FX->SetBool(eLightsEnabled, true);

			// If any, Sort the list based on illuminance -------------------------------------------
			qsort(LightList, nMeshLights, sizeof(_LightList), compare_lights);

			nMeshLights = min(nMeshLights, Config->MaxLights());

			// Create a list of N most effective lights ---------------------------------------------
			for (int i = 0; i < nMeshLights; i++) {
				memcpy((void*)&Locals[i], (const void*)&pLights[LightList[i].idx], sizeof(LightStruct));

				// Override application configuration to prevent oversaturation of lights at point plank range. 
				if (scn->GetRenderPass() == RENDERPASS_MAINSCENE)
					Locals[i].Attenuation.x = max(Locals[i].Attenuation.x, float(Config->GFXLocalMax));
			}
		}
	}

	FX->SetValue(eLights, Locals, sizeof(LightStruct) * Config->MaxLights());


	if (nEnv >= 1 && pEnv[0]) FX->SetTexture(eEnvMapA, pEnv[0]);


	UINT numPasses = 0;
	FX->Begin(&numPasses, 0);	// was D3DXFX_DONOTSAVESTATE; nothing is saved or restored

	WORD CurrentShader = SHADER_NULL;

	bool bRefl = true;

	for (DWORD g=0; g<nGrp; g++) {


		bool bHUD = (Grp[g].MFDScreenId == 0x100);

		// See RenderFast's copy of this: ORBITER_VK_NOVCHUD=1 skips the VC HUD
		// glass. Diagnostic only.
		{
			static const bool bNoVCHud = (getenv("ORBITER_VK_NOVCHUD") != NULL);
			if (bHUD && bNoVCHud) {
				static int nRep = 0;
				if (nRep < 40) {
					nRep++;
					LogErr("NOVCHUD Render: skipped grp %u of %u (MFDScreenId=%u UsrFlag=%X)",
						   g, nGrp, (unsigned)Grp[g].MFDScreenId, (unsigned)Grp[g].UsrFlag);
				}
				continue;
			}
		}

		// Group census: how many of this mesh's groups the two skip rules
		// above actually let through. Reported once per group count so a
		// per-frame walk does not flood the log. Diagnostic only.
		static const bool bTraceGrp = (getenv("ORBITER_VK_TRACE_GRP") != NULL);
		if (bTraceGrp) {
			static std::map<DWORD, int> seen;
			if (seen.find(nGrp) == seen.end()) {
				seen[nGrp] = 1;
				DWORD nSkip = 0, nHud = 0;
				for (DWORD k = 0; k < nGrp; k++) {
					if (Grp[k].MFDScreenId == 0x100) nHud++;
					else if (Grp[k].UsrFlag & 0x2)   nSkip++;
				}
				LogErr("GRPTRACE Render: mesh with %u groups: %u skipped by UsrFlag&2, "
					   "%u HUD, %u drawn", nGrp, nSkip, nHud, nGrp - nSkip - nHud);

				// And, for a mesh big enough to be a cockpit, one line per
				// group: which texture and which material it draws with, and
				// what alpha that material carries. Enough to find the one
				// translucent group a picture is asking about.
				if (nGrp > 64) {
					for (DWORD k = 0; k < nGrp; k++) {
						const DWORD ti = Grp[k].TexIdx;
						const VulkanMatExt *pm = (Grp[k].MtrlIdx == SPEC_DEFAULT)
											   ? &defmat : &Mtrl[Grp[k].MtrlIdx];
						LogErr("GRPTRACE   grp %3u tex=%u[%s] mtrl=%d diffA=%.3f "
							   "specA=%.3f UsrFlag=%X shader=%u nVert=%u",
							   k, ti,
							   (ti && Tex[ti]) ? Tex[ti]->GetName() : "(none)",
							   (Grp[k].MtrlIdx == SPEC_DEFAULT) ? -1 : int(Grp[k].MtrlIdx),
							   pm->Diffuse.a, pm->Specular.a,
							   (unsigned)Grp[k].UsrFlag, (unsigned)Grp[k].Shader,
							   Grp[k].nVert);
					}
				}
			}
		}

		// Inline engine renders HUD/MFDs in a separate rendering pass and flag 0x2 is used to disable rendering during the main rendering pass
		if ((Grp[g].UsrFlag & 0x2) && (!bHUD)) continue;

		// Bisection aid: ORBITER_VK_SKIPGRP=<n>[,<n>...] leaves those group
		// indices of the mesh with more than 64 groups undrawn, so "which
		// group IS that thing on the screen" is answered by looking rather
		// than by inferring it from its size. Diagnostic only.
		{
			static const char *pSkip = getenv("ORBITER_VK_SKIPGRP");
			if (pSkip && nGrp > 64) {
				char buf[128]; snprintf(buf, sizeof(buf), ",%u,", g);
				char list[256]; snprintf(list, sizeof(list), ",%s,", pSkip);
				if (strstr(list, buf)) continue;
			}
		}

		// Check skip conditions ==========================================
		//
		DWORD ti = Grp[g].TexIdx;
		DWORD tni = Grp[g].TexIdxEx[0];

		if (ti == 0 && tni != 0) continue;


		// Cull unvisible geometry ----------------------------------------
		//
		if (bGroupCull) if (!D9IsBSVisible(&Grp[g].BBox, &mWorldView, &Field)) continue;



		// Enforce special shader for XR2 HUD to bypass faulty materials
		if ((vClass == VCLASS_XR2) && (Grp[g].MFDScreenId == 0x100)) Grp[g].Shader = SHADER_XR2HUD;



		// Select the pass -------------------------------------------------
		//
		// Was: EndPass the previous shader's pass and BeginPass this one's,
		// here at the top of the loop. Only the bookkeeping stays here -- the
		// rest of the loop reads CurrentShader -- and the BeginPass moves down
		// to the draw. See the note on this function.
		//
		CurrentShader = Grp[g].Shader;



		// Mesh Debugger -------------------------------------------------------------------------------------------
		//
		if (DebugControls::IsActive()) {

			if (bActiveVisual) {

				if (displ==3 && g!=selgrp) continue;

				FX->SetVector(eColor, ptr(FVECTOR4(0.0f, 0.0f, 0.0f, 0.0f)));

				if (flags&DBG_FLAGS_HLMESH) {
					if (uCurrentMesh==selmsh) {
						FX->SetVector(eColor, ptr(FVECTOR4(0.0f, 0.0f, 0.5f, 0.5f)));
					}
				}
				if (flags&DBG_FLAGS_HLGROUP) {
					if (g==selgrp && uCurrentMesh==selmsh) {
						FX->SetVector(eColor, ptr(FVECTOR4(0.0f, 0.5f, 0.0f, 0.5f)));
					}
				}
			}
		}


		// ---------------------------------------------------------------------------------------------------------
		//
		if (Tex[ti] != old_tex) {
			if (Tex[ti] == NULL) {
				reset(FC);
				bUpdateFlow = true;
			}
		}

		if (Tex[ti]==NULL) bTextured = false, old_tex = NULL;
		else bTextured = true;



		// Setup Textures and Normal Maps ==========================================================================
		//
		if (bTextured) {

			if (Tex[ti]!=old_tex) {

				VulkanStats.Mesh.TexChanges++;

				old_tex = Tex[ti];

				FX->SetTexture(eTex0, Tex[ti]->GetTexture());

				bUpdateFlow = true;	// Fix this later

				if (CurrentShader == SHADER_LEGACY) {
					if (tni && Grp[g].TexMixEx[0] < 0.5f) tni = 0;
					if (tni && Tex[tni]) FX->SetTexture(eEmisMap, Tex[tni]->GetTexture());
				}
				else {

					if (DebugControls::IsActive()) if (pTune) {
						FX->SetValue(eTune, &pTune[ti], sizeof(VulkanTune));
					}

					VulkanTexture *pTransl = NULL;
					VulkanTexture *pTransm = NULL;
					VulkanTexture *pMetl = NULL;
					VulkanTexture *pRefl = Tex[ti]->GetMap(MAP_REFLECTION);
					VulkanTexture *pNorm = Tex[ti]->GetMap(MAP_NORMAL);
					VulkanTexture *pRghn = Tex[ti]->GetMap(MAP_ROUGHNESS);
					VulkanTexture *pEmis = Tex[ti]->GetMap(MAP_EMISSION);
					VulkanTexture *pHeat = Tex[ti]->GetMap(MAP_HEAT);
					VulkanTexture *pSpec = Tex[ti]->GetMap(MAP_SPECULAR);

					if (tni && Grp[g].TexMixEx[0] < 0.5f) tni = 0;
					if (!pEmis && tni && Tex[tni]) pEmis = Tex[tni]->GetTexture();

					if (pNorm) FX->SetTexture(eTex3, pNorm);
					if (pRghn) FX->SetTexture(eRghnMap, pRghn);
					if (pMetl) FX->SetTexture(eMetlMap, pMetl);
					if (pEmis) FX->SetTexture(eEmisMap, pEmis);
					if (pHeat) FX->SetTexture(eHeatMap, pHeat);
					if (pSpec) FX->SetTexture(eSpecMap, pSpec);
					if (pRefl) FX->SetTexture(eReflMap, pRefl);

					if (CurrentShader == SHADER_ADV || CurrentShader == SHADER_METALNESS)
					{
						pTransl = Tex[ti]->GetMap(MAP_TRANSLUCENCE);
						pTransm = Tex[ti]->GetMap(MAP_TRANSMITTANCE);
						pMetl = Tex[ti]->GetMap(MAP_METALNESS);

						if (pTransl) FX->SetTexture(eTranslMap, pTransl);
						if (pTransm) FX->SetTexture(eTransmMap, pTransm);
						if (pMetl) FX->SetTexture(eMetlMap, pMetl);

						FC.Transl = (pTransl != NULL);
						FC.Transm = (pTransm != NULL);	
						FC.Metl = (pMetl != NULL);
					}
					else {
						FC.Transl = false;
						FC.Transm = false;
						FC.Metl = false;
					}

					FC.Emis = (pEmis != NULL);
					FC.Norm = (pNorm != NULL);
					FC.Rghn = (pRghn != NULL);
					FC.Heat = (pHeat != NULL);
					FC.Spec = (pSpec != NULL);
					FC.Refl = (pRefl != NULL);
				}
			}
		}

		// Apply MFD Screen Override ================================================================================
		//
		if (Grp[g].MFDScreenId) {

			bTextured = true;
			old_tex = NULL;
			old_mat = NULL;
			reset(FC);
			bUpdateFlow = true;

			SURFHANDLE hMFD;
			if (bHUD) hMFD = gc->GetVCHUDSurface(&hudspec);
			else hMFD = gc->GetMFDSurface(Grp[g].MFDScreenId - 1);

			// Which surface a VC screen group is actually textured with, and
			// how big it is. Diagnostic only; env-gated.
			{
				static const bool bTraceVC = (getenv("ORBITER_VK_TRACE_VC") != NULL);
				if (bTraceVC) {
					static std::map<DWORD, int> seen;
					const DWORD key = Grp[g].MFDScreenId;
					if (seen.find(key) == seen.end()) {
						seen[key] = 1;
						VulkanTexture *pT = hMFD ? SURFACE(hMFD)->GetSurface() : NULL;
						LogErr("VCTRACE screen group %u: MFDScreenId=%u hMFD=%s size=%ux%u "
							   "mtrlIdx=%u modAlpha=%d",
							   g, (unsigned)Grp[g].MFDScreenId, _PTR(hMFD),
							   pT ? pT->Width() : 0, pT ? pT->Height() : 0,
							   (unsigned)Grp[g].MtrlIdx, int(bModulateMatAlpha));
					}
				}
			}

			if (hMFD) FX->SetTexture(eTex0, SURFACE(hMFD)->GetTexture());
			else	  FX->SetTexture(eTex0, gc->GetDefaultTexture()->GetTexture());

			if (Grp[g].MtrlIdx==SPEC_DEFAULT) mat = &mfdmat;
			else							  mat = &Mtrl[Grp[g].MtrlIdx];

			if (bModulateMatAlpha || bTextured==false)  FX->SetFloat(eMtrlAlpha, mat->Diffuse.w);
			else										FX->SetFloat(eMtrlAlpha, 1.0f);

			FX->SetValue(eMtrl, mat, sizeof(VulkanMatExt)-4);
		}


		// Setup Mesh group material  ==========================================================================
		//
		else {

			if (Grp[g].MtrlIdx==SPEC_DEFAULT) mat = &defmat;
			else							  mat = &Mtrl[Grp[g].MtrlIdx];

			if (mat!=old_mat) {

				VulkanStats.Mesh.MtrlChanges++;

				old_mat = mat;

				FX->SetValue(eMtrl, mat, sizeof(VulkanMatExt)-4);

				if (bModulateMatAlpha || bTextured==false)  FX->SetFloat(eMtrlAlpha, mat->Diffuse.w);
				else										FX->SetFloat(eMtrlAlpha, 1.0f);
			}
		}


		// Apply Animations =========================================================================================
		//
		if (Grp[g].bTransform) {
			bWorldMesh = false;
			// D3DXMatrixMultiply returned its output, so the call could be
			// nested inside SetMatrix. VMAT_MatrixMultiply returns void, so
			// the local it already writes into is named at the call.
			VMAT_MatrixMultiply(&q, &pGrpTF[g], pW);
			FX->SetMatrix(eW, &q);
		}
		else if (!bWorldMesh) {
			FX->SetMatrix(eW, &mWorldMesh);
			bWorldMesh = true;
		}

		// The world matrix and the first group's geometry, once per mesh, so a
		// run in which this mesh does not appear can be compared against one in
		// which it does. Diagnostic only; env-gated with ORBITER_VK_TRACE_VC.
		{
			static const bool bTraceW = (getenv("ORBITER_VK_TRACE_VC") != NULL);
			if (bTraceW) {
				static std::map<DWORD, int> seen;
				if (seen.find(nGrp) == seen.end()) {
					seen[nGrp] = 1;
					const FMATRIX4 &m = Grp[g].bTransform ? q : mWorldMesh;
					LogErr("WTRACE mesh nGrp=%u grp=%u bTF=%d nVert=%u nFace=%u "
						   "VertOff=%u IdexOff=%u  W=[%.4f %.4f %.4f | %.4f %.4f %.4f | "
						   "%.4f %.4f %.4f | %.3f %.3f %.3f %.3f]  bs=(%.3f %.3f %.3f r=%.3f)",
						   nGrp, g, int(Grp[g].bTransform), Grp[g].nVert, Grp[g].nFace,
						   Grp[g].VertOff, Grp[g].IdexOff,
						   m.m11, m.m12, m.m13, m.m21, m.m22, m.m23,
						   m.m31, m.m32, m.m33, m.m41, m.m42, m.m43, m.m44,
						   Grp[g].BBox.bs.x, Grp[g].BBox.bs.y, Grp[g].BBox.bs.z,
						   Grp[g].BBox.bs.w);
				}
			}
		}

		if (bUpdateFlow) {
			bUpdateFlow = false;
			FX->SetValue(eFlow, &FC, sizeof(TexFlow));
		}

		bool bPBR = (Grp[g].PBRStatus & 0xF) == (0x8 + 0x4);
		bool bRGH = (Grp[g].PBRStatus & 0xE) == (0x8 + 0x2);
		bool bNoL = (Grp[g].UsrFlag & 0x04) != 0;
		bool bNoC = (Grp[g].UsrFlag & 0x10) != 0;
		bool bOIT = (Grp[g].UsrFlag & 0x20) != 0;
		bool bENV = false;
		bool bFRS = false;

		// Setup Mesh drawing options =================================================================================
		//
		FX->SetBool(eOITEnable, bOIT);
		FX->SetBool(eTextured, bTextured);
		FX->SetBool(eFullyLit, bNoL);
		FX->SetBool(eNoColor,  bNoC);
		FX->SetBool(eSwitch, bPBR);
		FX->SetBool(eRghnSw, bRGH);

		// Update envmap and fresnel status as required
		if (bRefl) {
			bFRS = (Grp[g].PBRStatus & 0x1E) >= 0x10;
			FX->SetBool(eFresnel, bFRS);
			if (IsReflective()) {			
				bENV = ((Grp[g].PBRStatus & 0x1E) >= 0xA) | (Grp[g].Shader == SHADER_METALNESS);
				FX->SetBool(eEnvMapEnable, bENV);
			}
		}

		

		// FX->CommitChanges() stood here. It has no counterpart: D3DX buffered
		// the parameter writes above and this pushed them to the device inside
		// the open pass, where BeginPassEx below is the point at which they
		// are uploaded. See the note on this function.



		// Mesh Debugger -------------------------------------------------------------------------------------------
		//
		if (DebugControls::IsActive()) {

			if ((bActiveVisual) && (g == selgrp) && (uCurrentMesh == selmsh)) {

				bool bAdd = (Grp[g].UsrFlag & 0x08) != 0;
				bool bNoS = (Grp[g].UsrFlag & 0x01) != 0;

				static const char *YesNo[2] = { "No", "Yes" };
				static const char *LPW[2] = { "Legacy", "PBR" };
				static const char *RGH[2] = { "Disabled", "Enabled" };
				static const char *Shaders[7] = { "PBR", "PBR-ADV", "FAST", "XR2", "METALNESS", "SPECULAR", "???"};

				DebugControls::Append("MeshIdx = %d, GrpIdx = %d\n", uCurrentMesh, g);
				DebugControls::Append("MtrlIdx = %d, TexIdx = %d\n", Grp[g].MtrlIdx, Grp[g].TexIdx);
				DebugControls::Append("FaceCnt = %d, VtxCnt = %d\n", Grp[g].nFace, Grp[g].nVert);

				DebugControls::Append("GroupFlags.. = 0x%X\n", Grp[g].UsrFlag);
				DebugControls::Append("Shader...... = %s\n", Shaders[CurrentShader]);
				DebugControls::Append("Textured.... = %s\n", YesNo[bTextured]);
				DebugControls::Append("ModMatAlpha. = %s\n", YesNo[bModulateMatAlpha]);
				DebugControls::Append("NoColor..... = %s\n", YesNo[bNoC]);
				DebugControls::Append("NoLighting.. = %s\n", YesNo[bNoL]);
				DebugControls::Append("NoShadow.... = %s\n", YesNo[bNoS]);
				DebugControls::Append("Additive.... = %s\n", YesNo[bAdd]);

				if (CurrentShader == SHADER_PBR || CurrentShader == SHADER_ADV || CurrentShader == SHADER_METALNESS)
				{
					if (CurrentShader != SHADER_METALNESS) {
						DebugControls::Append("\nPBR-Shader State:\n");
						DebugControls::Append("PBR-Switch.. = %s\n", LPW[bPBR]);
						DebugControls::Append("Rghn-Conver. = %s\n", RGH[bRGH]);
						DebugControls::Append("Fresnel Mode = %s\n", RGH[bFRS]);
					}
					
					DebugControls::Append("Env Mapping. = %s\n", RGH[bENV]);

					DebugControls::Append("TextureMaps = [ ");
					if (FC.Emis) DebugControls::Append("emis ");
					if (FC.Metl) DebugControls::Append("metl ");
					if (FC.Norm) DebugControls::Append("norm ");
					if (FC.Rghn) DebugControls::Append("rghn ");
					if (FC.Spec) DebugControls::Append("spec ");
					if (FC.Refl) DebugControls::Append("refl ");
					if (FC.Transl) DebugControls::Append("transl ");
					if (FC.Transm) DebugControls::Append("transm ");
					DebugControls::Append("]\n");
				}

				DebugControls::Append("Local Lights = %d\n", nMeshLights);

				DebugControls::Refresh();
			}
		}


		// Start rendering -------------------------------------------------------------------------------------------
		//
		VulkanEffectFile::PassOverride ovr = ovrBase;

		// Was SetRenderState(ZENABLE, 0) + SetRenderState(DESTBLEND, ONE)
		// around the draw, with the two restores below it.
		//
		// depthWrite GOES WITH depthTest, and that is not an addition:
		// D3DRS_ZENABLE = D3DZB_FALSE turns depth buffering off ENTIRELY,
		// writes included -- D3DRS_ZWRITEENABLE only selects among the writes
		// that a live depth buffer would make. Vulkan splits the two, so both
		// have to be said. Leaving the write on stamps the HUD glass quad
		// into the depth buffer and hides whatever the cockpit draws behind it
		// afterwards.
		if (bHUD) {
			ovr.depthTest = 0;
			ovr.depthWrite = 0;
			ovr.dstBlend = VK_BLEND_FACTOR_ONE;
		}

		// Bisection aid: ORBITER_VK_MESHNODEPTH=1 takes the depth test out of
		// every mesh draw, to tell "not drawn" apart from "drawn and hidden by
		// the depth buffer". Diagnostic only.
		{
			static const bool bNoZ = (getenv("ORBITER_VK_MESHNODEPTH") != NULL);
			if (bNoZ) { ovr.depthTest = 0; ovr.depthWrite = 0; }
			// And the same for the cull, to tell "culled" apart from "not
			// transformed onto the screen". Diagnostic only.
			static const bool bNoCull = (getenv("ORBITER_VK_MESHNOCULL") != NULL);
			if (bNoCull) ovr.cullMode = VulkanEffectFile::PassOverride::CULL_NONE;
		}

		if (Grp[g].bDualSided) {
			// The reverse-facing pass: CULLMODE CW with ZWRITEENABLE off. It
			// is its own pipeline here, so it is its own BeginPassEx rather
			// than two SetRenderStates around an extra draw.
			VulkanEffectFile::PassOverride dsl = ovr;
			dsl.cullMode = VulkanEffectFile::PassOverride::CULL_CW;
			dsl.depthWrite = 0;
			FX->BeginPassEx(CurrentShader, &dsl);
			vkCmdDrawIndexed(cmd, Grp[g].nFace*3, 1, Grp[g].IdexOff, (int32_t)Grp[g].VertOff, 0);
			FX->EndPass();
		}

		// The bOIT GetRenderState/SetRenderState pair on
		// D3DRS_MULTISAMPLEANTIALIAS, and dwMSAA with them, stood here and
		// around the draw below. See the note on this function: the core's
		// render pass is single-sampled, so there is nothing to turn off.

		// A FAILED BeginPass MUST NOT BE SILENT, and this is the second time
		// that has cost a picture. D3DX's BeginPass either applied the pass's
		// state or returned an error the caller saw; here a false means no
		// pipeline was bound, and the vkCmdDrawIndexed below then draws this
		// mesh's geometry with WHATEVER pipeline the previous draw left --
		// another shader, another vertex layout, another blend. Reported once
		// per technique so a bad frame does not flood the log.
		if (!FX->BeginPassEx(CurrentShader, &ovr)) {
			static std::map<int, int> seen;
			if (seen.find(int(CurrentShader)) == seen.end()) {
				seen[int(CurrentShader)] = 1;
				LogErr("VulkanMesh::Render: BeginPassEx(%d) failed -- mesh %u group %u "
					   "of %u drawn with no pipeline of its own",
					   int(CurrentShader), uCurrentMesh, g, nGrp);
			}
		}
		vkCmdDrawIndexed(cmd, Grp[g].nFace*3, 1, Grp[g].IdexOff, (int32_t)Grp[g].VertOff, 0);
		FX->EndPass();

		Grp[g].bRendered = true;

		VulkanStats.Mesh.Vertices += Grp[g].nVert;
		VulkanStats.Mesh.MeshGrps++;

	}

	// `if (CurrentShader != 0xFFFF) FX->EndPass();` stood here, closing the
	// one pass the loop left open. Every pass is now closed where it was
	// opened, so there is none left.

	FX->End();

	if (flags&(DBG_FLAGS_BOXES|DBG_FLAGS_SPHERES)) RenderBoundingBox(pW);
	FX->SetVector(eColor, ptr(FVECTOR4(0.0f, 0.0f, 0.0f, 0.0f)));
	// The trailing SetRenderState(CULLMODE, CCW) that undid DBG_FLAGS_DUALSIDED
	// has nothing to undo: the cull mode was ovrBase's, and ovrBase does not
	// outlive this call.
}



// ================================================================================================
// Render without animations 
//
// Same three changes as Render() above -- the pass opens once per draw rather
// than once per shader change, the buffer binds are commands, and the
// D3DRS_MULTISAMPLEANTIALIAS pair around the bOIT draw has no counterpart.
//
void VulkanMesh::RenderSimplified(const FMATRIX4 *pW, VulkanTexture **pEnv, int nEnv, bool bSP)
{
	if (!IsOK()) return;

	pBuf->Map(pDev);

	// Check material status
	//
	if (bMtrlModidied) {
		CheckMeshStatus();
		bMtrlModidied = false;
	}

	// `Scene *scn = gc->GetScene();` stood here and is never read in this
	// function -- finding 35's family.

	bool bTextured = true;
	bool bUpdateFlow = true;
	
	EnablePlanetGlow(true);
	
	VulkanEffect::FX->SetBool(VulkanEffect::eBaseBuilding, bSP);

	VulkanMatExt *mat, *old_mat = NULL;
	SURFHANDLE old_tex = NULL;
	TexFlow FC;	reset(FC);

	VkCommandBuffer cmd = pDev->GetCommandBuffer();
	if (!cmd || !pBuf->pVB || !pBuf->pIB) return;

	// The D3DPT_TRIANGLELIST of the draws below; see the note in RenderGroup.
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	FX->SetVertexDecl(pMeshVertexDecl);
	{
		VkBuffer vb = pBuf->pVB->Buffer();
		VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
		vkCmdBindIndexBuffer(cmd, pBuf->pIB->Buffer(), 0, VK_INDEX_TYPE_UINT16);
	}

	FX->SetTechnique(eVesselTech);
	FX->SetBool(eFresnel, false);
	FX->SetBool(eEnvMapEnable, false);
	FX->SetBool(eTuneEnabled, false);
	FX->SetBool(eLightsEnabled, false);
	FX->SetVector(eColor, ptr(FVECTOR4(0.0f, 0.0f, 0.0f, 0.0f)));
	FX->SetMatrix(eW, pW);

	ConfigureAtmo();

	// Process Local Light Sources ------------------------------------------------------------
	//
	const VulkanLight *pLights = gc->GetScene()->GetLights();
	int nSceneLights = gc->GetScene()->GetLightCount();

	for (int i = 0; i < Config->MaxLights(); i++) memcpy(&Locals[i], &null_light, sizeof(LightStruct));

	//VulkanDebugLog("Mesh=[%s], nLights=%d", GetName(), nSceneLights);

	if (pLights && nSceneLights>0) {

		int nMeshLights = 0;
		FVECTOR3 pos = oapi::TransformCoord(FVECTOR3f4(BBox.bs), *pW);

		// Find all local lights effecting this mesh ------------------------------------------
		//
		for (int i = 0; i < nSceneLights; i++) {
			float il = pLights[i].GetIlluminance(pos, BBox.bs.w);
			if (il > 0.005f) {
				LightList[nMeshLights].illuminace = il;
				LightList[nMeshLights++].idx = i;
			}
		}

		if (nMeshLights > 0) {

			FX->SetBool(eLightsEnabled, true);

			// If any, Sort the list based on illuminance -------------------------------------------
			qsort(LightList, nMeshLights, sizeof(_LightList), compare_lights);

			nMeshLights = min(nMeshLights, Config->MaxLights());

			// Create a list of N most effective lights ---------------------------------------------
			for (int i = 0; i < nMeshLights; i++) memcpy((void*)&Locals[i], (const void*)&pLights[LightList[i].idx], sizeof(LightStruct));
		}
	}

	FX->SetValue(eLights, Locals, sizeof(LightStruct) * Config->MaxLights());

	if (nEnv >= 1 && pEnv[0]) FX->SetTexture(eEnvMapA, pEnv[0]);

	bool bRefl = true;
	WORD CurrentShader = 0xFFFF;
	UINT numPasses = 0;

	FX->Begin(&numPasses, 0);


	// Render MeshGroups ----------------------------------------------------
	//
	for (DWORD g = 0; g<nGrp; g++) {

		// Check skip conditions --------------------------------------------
		//
		DWORD ti = Grp[g].TexIdx;
		if (Grp[g].UsrFlag & 0x2) continue;


		// Select the pass. See Render(): the BeginPass moves to the draw.
		//
		CurrentShader = Grp[g].Shader;

		// -------------------------------------------------------------------
		//
		if (Tex[ti] != old_tex) {
			if (Tex[ti] == NULL) {
				reset(FC);
				bUpdateFlow = true;
			}
		}

		if (Tex[ti] == NULL) bTextured = false, old_tex = NULL;
		else bTextured = true;


		// Setup Textures and Normal Maps =====================================
		//
		if (bTextured) {

			if (Tex[ti] != old_tex) {

				old_tex = Tex[ti];
				FX->SetTexture(eTex0, Tex[ti]->GetTexture());
				bUpdateFlow = true;	// Fix this later

				VulkanTexture *pTransl = NULL;
				VulkanTexture *pTransm = NULL;
				VulkanTexture *pSpec = Tex[ti]->GetMap(MAP_SPECULAR);
				VulkanTexture *pNorm = Tex[ti]->GetMap(MAP_NORMAL);
				VulkanTexture *pRefl = Tex[ti]->GetMap(MAP_REFLECTION);
				VulkanTexture *pRghn = Tex[ti]->GetMap(MAP_ROUGHNESS);
				VulkanTexture *pMetl = Tex[ti]->GetMap(MAP_METALNESS);
				VulkanTexture *pEmis = Tex[ti]->GetMap(MAP_EMISSION);

				if (pNorm) FX->SetTexture(eTex3, pNorm);
				if (pRghn) FX->SetTexture(eRghnMap, pRghn);
				if (pRefl) FX->SetTexture(eReflMap, pRefl);
				if (pMetl) FX->SetTexture(eMetlMap, pMetl);
				if (pSpec) FX->SetTexture(eSpecMap, pSpec);
				if (pEmis) FX->SetTexture(eEmisMap, pEmis);

				if (CurrentShader == SHADER_ADV) {

					pTransl = Tex[ti]->GetMap(MAP_TRANSLUCENCE);
					pTransm = Tex[ti]->GetMap(MAP_TRANSMITTANCE);

					if (pTransl) FX->SetTexture(eTranslMap, pTransl);
					if (pTransm) FX->SetTexture(eTransmMap, pTransm);

					FC.Transl = (pTransl != NULL);
					FC.Transm = (pTransm != NULL);
				}
				else {
					FC.Transl = false;
					FC.Transm = false;
				}

				FC.Emis = (pEmis != NULL);
				FC.Metl = (pMetl != NULL);
				FC.Norm = (pNorm != NULL);
				FC.Rghn = (pRghn != NULL);
				FC.Spec = (pSpec != NULL);
				FC.Refl = (pRefl != NULL);
			}	
		}


		// Setup Mesh group material  ==========================================
		//
		else {
			if (Grp[g].MtrlIdx == SPEC_DEFAULT) mat = &defmat;
			else mat = &Mtrl[Grp[g].MtrlIdx];
			if (mat != old_mat) {
				old_mat = mat;
				FX->SetValue(eMtrl, mat, sizeof(VulkanMatExt) - 4);
				if (bModulateMatAlpha || bTextured == false) FX->SetFloat(eMtrlAlpha, mat->Diffuse.w);
				else FX->SetFloat(eMtrlAlpha, 1.0f);
			}
		}

		// Must update FlowControl ?
		//
		if (bUpdateFlow) {
			bUpdateFlow = false;
			FX->SetValue(eFlow, &FC, sizeof(TexFlow));
		}

		bool bPBR = (Grp[g].PBRStatus & 0xF) == (0x8 + 0x4);
		bool bRGH = (Grp[g].PBRStatus & 0xE) == (0x8 + 0x2);
		bool bNoL = (Grp[g].UsrFlag & 0x04) != 0;
		bool bNoC = (Grp[g].UsrFlag & 0x10) != 0;
		bool bOIT = (Grp[g].UsrFlag & 0x20) != 0;
		bool bENV = false;
		bool bFRS = false;


		// Setup Mesh drawing options =================================================================================
		//
		FX->SetBool(eOITEnable, bOIT);
		FX->SetBool(eTextured, bTextured);
		FX->SetBool(eFullyLit, bNoL);
		FX->SetBool(eNoColor, bNoC);
		FX->SetBool(eSwitch, bPBR);
		FX->SetBool(eRghnSw, bRGH);


		// Update envmap and fresnel status as required
		//
		if (bRefl) {
			bFRS = (Grp[g].PBRStatus & 0x1E) >= 0x10;
			FX->SetBool(eFresnel, bFRS);
			if (IsReflective()) {
				bENV = (Grp[g].PBRStatus & 0x1E) >= 0xA;
				FX->SetBool(eEnvMapEnable, bENV);
			}
		}

		// Start rendering -------------------------------------------------------------------------------------------
		//
		// FX->CommitChanges() and the D3DRS_MULTISAMPLEANTIALIAS save/restore
		// around the draw stood here. See Render(): neither has a counterpart.

		FX->BeginPass(CurrentShader);
		vkCmdDrawIndexed(cmd, Grp[g].nFace*3, 1, Grp[g].IdexOff, (int32_t)Grp[g].VertOff, 0);
		FX->EndPass();

		Grp[g].bRendered = true;
	}

	FX->End();
}


// ================================================================================================
// Render a legacy orbiter mesh without any additional textures
//
// The pass structure differs from Render()'s in the reference -- ONE pass,
// index 2, opened before the loop -- and converges on the same answer here,
// because FX->CommitChanges() between the groups is what forced the change.
// See Render() for the reasoning; the pass is opened per draw and pass 2 is
// simply the constant it now takes.
//
void VulkanMesh::RenderFast(const FMATRIX4 *pW, int iTech)
{

	_TRACE;
	DWORD flags = 0, selmsh = 0, selgrp = 0, displ = 0; // Debug Variables
	bool bActiveVisual = false;

	const VCHUDSPEC *hudspec = NULL;

	if (!IsOK()) return;

	pBuf->Map(pDev);

	if (DebugControls::IsActive()) {
		flags = *(DWORD*)gc->GetConfigParam(CFGPRM_GETDEBUGFLAGS);
		selmsh = *(DWORD*)gc->GetConfigParam(CFGPRM_GETSELECTEDMESH);
		selgrp = *(DWORD*)gc->GetConfigParam(CFGPRM_GETSELECTEDGROUP);
		displ = *(DWORD*)gc->GetConfigParam(CFGPRM_GETDISPLAYMODE);
		bActiveVisual = (pCurrentVisual == DebugControls::GetVisual());
		if (displ>0 && !bActiveVisual) return;
		if ((displ == 2 || displ == 3) && uCurrentMesh != selmsh) return;
	}

	Scene *scn = gc->GetScene();

	bool bWorldMesh = false;
	bool bMeshCull = true;
	bool bTextured = true;
	bool bGroupCull = true;
	bool bUpdateFlow = true;
	bool bShadowProjection = false;

	switch (iTech) {
		case RENDER_VC:
			EnablePlanetGlow(false);
			break;
		case RENDER_BASE:
			EnablePlanetGlow(false);
			bMeshCull = false;
			bShadowProjection = true;
			break;
		case RENDER_BASEBS:
			EnablePlanetGlow(false);
			bMeshCull = false;
			bShadowProjection = true;
			break;
		case RENDER_ASTEROID:
			EnablePlanetGlow(false);
			bMeshCull = false;
			bGroupCull = false;
			bShadowProjection = true;
			break;
		case RENDER_VESSEL:
			EnablePlanetGlow(true);
			break;
	}

	VulkanEffect::FX->SetBool(VulkanEffect::eBaseBuilding, bShadowProjection);

	FVECTOR4 Field;
	FMATRIX4 mWorldView, q;

	VMAT_MatrixMultiply(&mWorldView, pW, scn->GetViewMatrix());

	if (bMeshCull || bGroupCull) Field = D9LinearFieldOfView(scn->GetProjectionMatrix());

	if (bMeshCull) if (!D9IsAABBVisible(&BBox, &mWorldView, &Field)) {
		if (flags&(DBG_FLAGS_BOXES | DBG_FLAGS_SPHERES)) RenderBoundingBox(pW);
		return;
	}

	FMATRIX4 mWorldMesh;

	if (bGlobalTF) VMAT_MatrixMultiply(&mWorldMesh, &mTransform, pW);
	else mWorldMesh = *pW;

	VulkanStats.Mesh.Meshes++;

	VulkanMatExt *mat, *old_mat = NULL;
	SURFHANDLE old_tex = NULL;
	VulkanTexture *pEmis_old = NULL;

	VkCommandBuffer cmd = pDev->GetCommandBuffer();
	if (!cmd || !pBuf->pVB || !pBuf->pIB) return;

	// The D3DPT_TRIANGLELIST of the draws below; see the note in RenderGroup.
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	FX->SetVertexDecl(pMeshVertexDecl);
	{
		VkBuffer vb = pBuf->pVB->Buffer();
		VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
		vkCmdBindIndexBuffer(cmd, pBuf->pIB->Buffer(), 0, VK_INDEX_TYPE_UINT16);
	}

	VulkanEffectFile::PassOverride ovrBase;
	if (flags&DBG_FLAGS_DUALSIDED) ovrBase.cullMode = VulkanEffectFile::PassOverride::CULL_NONE;
	// Was SetRenderState(ZENABLE, 0) immediately AFTER BeginPass, with the
	// reference's own comment "Must be here because BeginPass() sets it
	// enabled". It is pipeline state here, so it must be declared BEFORE the
	// bind instead -- the requirement inverts.
	if (iTech == RENDER_BASEBS) ovrBase.depthTest = 0;


	FX->SetTechnique(eVesselTech);
	FX->SetBool(eTuneEnabled, false);
	FX->SetBool(eLightsEnabled, false);
	FX->SetVector(eColor, ptr(FVECTOR4(0.0f, 0.0f, 0.0f, 0.0f)));

	ConfigureAtmo();

	if (DebugControls::IsActive()) if (pTune) FX->SetBool(eTuneEnabled, true);

	TexFlow FC;	reset(FC);

	const VulkanLight *pLights = gc->GetScene()->GetLights();
	int nSceneLights = gc->GetScene()->GetLightCount();

	for (int i = 0; i < Config->MaxLights(); i++) memcpy(&Locals[i], &null_light, sizeof(LightStruct));

	//VulkanDebugLog("Mesh=[%s], nLights=%d", GetName(), nSceneLights);

	if (pLights && nSceneLights>0) {

		int nMeshLights = 0;
		FVECTOR3 pos = oapi::TransformCoord(FVECTOR3f4(BBox.bs), *pW);

		// Find all local lights effecting this mesh ------------------------------------------
		//
		for (int i = 0; i < nSceneLights; i++) {
			float il = pLights[i].GetIlluminance(pos, BBox.bs.w);
			if (il > 0.0) {
				LightList[nMeshLights].illuminace = il;
				LightList[nMeshLights++].idx = i;
			}
		}

		if (nMeshLights > 0) {

			FX->SetBool(eLightsEnabled, true);

			// If any, Sort the list based on illuminance -------------------------------------------
			qsort(LightList, nMeshLights, sizeof(_LightList), compare_lights);

			nMeshLights = min(nMeshLights, Config->MaxLights());

			// Create a list of N most effective lights ---------------------------------------------
			int i;
			for (i = 0; i < nMeshLights; i++) memcpy((void*)&Locals[i], (const void*)&pLights[LightList[i].idx], sizeof(LightStruct));
		}
	}

	FX->SetValue(eLights, Locals, sizeof(LightStruct) * Config->MaxLights());

	UINT numPasses = 0;
	FX->Begin(&numPasses, 0);

	// The single `FX->BeginPass(2)` stood here, before the loop, with the
	// BASEBS depth override on the line after it. Both moved -- see above.

	for (DWORD g = 0; g<nGrp; g++) {

		// This mesh group must be skipped by user orders
		if ((Grp[g].UsrFlag & 0x2) && (Grp[g].MFDScreenId != 0x100)) continue;

		bool bHUD = Grp[g].MFDScreenId == 0x100;

		// Bisection aid: ORBITER_VK_NOVCHUD=1 leaves the virtual-cockpit HUD
		// glass undrawn, so "is that pane the HUD group or some other
		// translucent group of the cockpit mesh?" can be answered by looking.
		// Diagnostic only.
		{
			static const bool bNoVCHud = (getenv("ORBITER_VK_NOVCHUD") != NULL);
			if (bHUD && bNoVCHud) {
				static int nRep = 0;
				if (nRep < 40) {
					nRep++;
					LogErr("NOVCHUD RenderFast: skipped grp %u of %u (MFDScreenId=%u UsrFlag=%X)",
						   g, nGrp, (unsigned)Grp[g].MFDScreenId, (unsigned)Grp[g].UsrFlag);
				}
				continue;
			}
		}

		// Mesh Debugger -------------------------------------------------------------------------------------------
		//
		if (DebugControls::IsActive()) {

			if (bActiveVisual) {

				if (displ == 3 && g != selgrp) continue;

				FX->SetVector(eColor, ptr(FVECTOR4(0.0f, 0.0f, 0.0f, 0.0f)));

				if (flags&DBG_FLAGS_HLMESH) {
					if (uCurrentMesh == selmsh) {
						FX->SetVector(eColor, ptr(FVECTOR4(0.0f, 0.0f, 0.5f, 0.5f)));
					}
				}
				if (flags&DBG_FLAGS_HLGROUP) {
					if (g == selgrp && uCurrentMesh == selmsh) {
						FX->SetVector(eColor, ptr(FVECTOR4(0.0f, 0.5f, 0.0f, 0.5f)));
					}
				}
			}
		}


		// ---------------------------------------------------------------------------------------------------------
		//
		DWORD ti = Grp[g].TexIdx;
		DWORD tni = Grp[g].TexIdxEx[0];

		if (ti == 0 && tni != 0) continue;

		if (Tex[ti] == NULL || ti == 0) bTextured = false, old_tex = NULL;
		else						    bTextured = true;


		// Cull unvisible geometry =================================================================================
		//
		if (bGroupCull) if (!D9IsBSVisible(&Grp[g].BBox, &mWorldView, &Field)) continue;


		// Setup Textures and Normal Maps ==========================================================================
		//
		if (bTextured) {

			if (Tex[ti] != old_tex) {

				VulkanStats.Mesh.TexChanges++;

				if (DebugControls::IsActive()) if (pTune) FX->SetValue(eTune, &pTune[ti], sizeof(VulkanTune));

				old_tex = Tex[ti];
				FX->SetTexture(eTex0, Tex[ti]->GetTexture());

				VulkanTexture *pEmis = Tex[ti]->GetMap(MAP_EMISSION);

				if (tni && Grp[g].TexMixEx[0]<0.5f) tni = 0;
				if (!pEmis && tni && Tex[tni]) pEmis = Tex[tni]->GetTexture();

				if (pEmis != pEmis_old) {
					FX->SetTexture(eEmisMap, pEmis);
					pEmis_old = pEmis;
					FC.Emis = (pEmis != NULL);
					bUpdateFlow = true;
				}
			}
		}

		// Apply MFD Screen Override ================================================================================
		//
		if (Grp[g].MFDScreenId) {
			bTextured = true;
			old_tex = NULL;
			old_mat = NULL;
			reset(FC);
			bUpdateFlow = true;

			SURFHANDLE hMFD;
			if (bHUD) hMFD = gc->GetVCHUDSurface(&hudspec);
			else hMFD = gc->GetMFDSurface(Grp[g].MFDScreenId - 1);

			if (hMFD) FX->SetTexture(eTex0, SURFACE(hMFD)->GetTexture());
			else	  FX->SetTexture(eTex0, gc->GetDefaultTexture()->GetTexture());

			if (Grp[g].MtrlIdx == SPEC_DEFAULT) mat = &mfdmat;
			else							    mat = &Mtrl[Grp[g].MtrlIdx];

			if (bModulateMatAlpha || bTextured == false)  FX->SetFloat(eMtrlAlpha, mat->Diffuse.w);
			else										  FX->SetFloat(eMtrlAlpha, 1.0f);

			FX->SetValue(eMtrl, mat, sizeof(VulkanMatExt)-4);
			FX->SetBool(eEnvMapEnable, false);
			FX->SetBool(eFresnel, false);
		}

		// Setup Mesh group material  ==========================================================================
		//
		else {

			if (Grp[g].MtrlIdx == SPEC_DEFAULT) mat = &defmat;
			else							    mat = &Mtrl[Grp[g].MtrlIdx];

			if (mat != old_mat) {

				VulkanStats.Mesh.MtrlChanges++;

				old_mat = mat;

				FX->SetValue(eMtrl, mat, sizeof(VulkanMatExt)-4);

				if (bModulateMatAlpha || bTextured == false)  FX->SetFloat(eMtrlAlpha, mat->Diffuse.w);
				else										  FX->SetFloat(eMtrlAlpha, 1.0f);
			}
		}


		// Apply Animations =========================================================================================
		//
		if (Grp[g].bTransform) {
			bWorldMesh = false;
			VMAT_MatrixMultiply(&q, &pGrpTF[g], pW);
			FX->SetMatrix(eW, &q);
		}
		else if (!bWorldMesh) {
			FX->SetMatrix(eW, &mWorldMesh);
			bWorldMesh = true;
		}

		if (bUpdateFlow) {
			bUpdateFlow = false;
			FX->SetValue(eFlow, &FC, sizeof(TexFlow));
		}

		// Setup Mesh drawing options =================================================================================
		//
		FX->SetBool(eTextured, bTextured);
		FX->SetBool(eFullyLit, (Grp[g].UsrFlag & 0x4) != 0);
		FX->SetBool(eNoColor, (Grp[g].UsrFlag & 0x10) != 0);
		FX->SetBool(eOITEnable, (Grp[g].UsrFlag & 0x20) != 0);

		// FX->CommitChanges() stood here. See Render().

		VulkanEffectFile::PassOverride ovr = ovrBase;

		// depthWrite with depthTest; see the note in Render().
		if (bHUD) {
			ovr.depthTest = 0;
			ovr.depthWrite = 0;
			ovr.dstBlend = VK_BLEND_FACTOR_ONE;
		}

		if (Grp[g].bDualSided) {
			VulkanEffectFile::PassOverride dsl = ovr;
			dsl.cullMode = VulkanEffectFile::PassOverride::CULL_CW;
			dsl.depthWrite = 0;
			FX->BeginPassEx(2, &dsl);
			vkCmdDrawIndexed(cmd, Grp[g].nFace*3, 1, Grp[g].IdexOff, (int32_t)Grp[g].VertOff, 0);
			FX->EndPass();
		}

		FX->BeginPassEx(2, &ovr);
		vkCmdDrawIndexed(cmd, Grp[g].nFace*3, 1, Grp[g].IdexOff, (int32_t)Grp[g].VertOff, 0);
		FX->EndPass();

		Grp[g].bRendered = true;

		VulkanStats.Mesh.Vertices += Grp[g].nVert;
		VulkanStats.Mesh.MeshGrps++;
	}

	FX->End();

	if (flags&(DBG_FLAGS_BOXES | DBG_FLAGS_SPHERES)) RenderBoundingBox(pW);
	FX->SetVector(eColor, ptr(FVECTOR4(0.0f, 0.0f, 0.0f, 0.0f)));
}


// ===========================================================================================
//
FMATRIX4 VulkanMesh::GetTransform(int g, bool bCombined)
{
	if (g < 0) return mTransform;

	if (Grp[g].bTransform) {
		if (bCombined) return pGrpTF[g];
		else return Grp[g].Transform;
	}

	FMATRIX4 Ident; VMAT_Identity(&Ident);
	return Ident;
}


// ===========================================================================================
// D3DXMatrixInverse(&out, NULL, &in) -> MatrixInverse(out, in). See
// VectorHelpers.h: D3DX supplied the inverse and Orbiter's SDK does not, so it
// is written out there once rather than per call site. The determinant
// out-parameter D3DX took is the optional third argument.
//
bool VulkanMesh::SetTransform(int g, const FMATRIX4 *pMat)
{
	if (g >= int(nGrp)) return false;

	// Set Mesh Transform if g < 0
	if (g < 0) {
		mTransform = *pMat;
		bGlobalTF = true;
		bBSRecompute = true;
		bBSRecomputeAll = true;
		MatrixInverse(mTransformInv, mTransform);
		for (DWORD i = 0; i<nGrp; i++) {
			if (Grp[i].bTransform) VMAT_MatrixMultiply(&pGrpTF[i], &mTransform, &Grp[i].Transform);
			else pGrpTF[i] = mTransform;
		}
		return true;
	}

	Grp[g].bUpdate = true;
	Grp[g].bTransform = true;
	Grp[g].Transform = *pMat;
	VMAT_MatrixMultiply(&pGrpTF[g], &mTransform, &Grp[g].Transform);

	return true;
}





// ===========================================================================================
//
void VulkanMesh::RenderBaseTile(const FMATRIX4 *pW)
{
	if (!IsOK()) return;

	Scene *scn = gc->GetScene();

	bool bTextured = true;
	bool bGroupCull = true;
	bool bUseNormalMap = (Config->UseNormalMap==1);

	FMATRIX4 mWorldView;
	VMAT_MatrixMultiply(&mWorldView, pW, scn->GetViewMatrix());

	FVECTOR4 Field = D9LinearFieldOfView(scn->GetProjectionMatrix());

	VulkanMatExt *mat, *old_mat = NULL;
	SURFHANDLE old_tex = NULL;
	VulkanTexture *pNorm = NULL;

	VkCommandBuffer cmd = pDev->GetCommandBuffer();
	if (!cmd || !pBuf->pVB || !pBuf->pIB) return;

	// The D3DPT_TRIANGLELIST of the draws below; see the note in RenderGroup.
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	FX->SetVertexDecl(pMeshVertexDecl);
	{
		VkBuffer vb = pBuf->pVB->Buffer();
		VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
		vkCmdBindIndexBuffer(cmd, pBuf->pIB->Buffer(), 0, VK_INDEX_TYPE_UINT16);
	}


	FX->SetTechnique(eBaseTile);
	FX->SetVector(eColor, ptr(FVECTOR4(0.0f, 0.0f, 0.0f, 0.0f)));
	FX->SetMatrix(eGT, gc->GetIdentity());
	FX->SetMatrix(eW, pW);

	ConfigureAtmo();

	UINT numPasses = 0;
	FX->Begin(&numPasses, 0);

	for (DWORD pass=0;pass<numPasses;pass++) {

		if (bUseNormalMap==false && pass==0) continue; // Skip normal mapped rendering pass

		for (DWORD g=0; g<nGrp; g++) {

			if (Grp[g].UsrFlag & 0x2) continue;

			// Render group ----------------------------------------------
			//

			DWORD ti=Grp[g].TexIdx;
			DWORD tni=Grp[g].TexIdxEx[0];

			if (ti==0 && tni!=0) continue;

			if (Tex[ti]==NULL || ti==0) bTextured = false;
			else						bTextured = true;

			if (bTextured) {
				pNorm = Tex[ti]->GetMap(MAP_NORMAL);
				if (pNorm==NULL && pass==0) continue;
				if (pNorm!=NULL && pass==1) continue;
			}
			else {
				if (pass==0) continue;
				pNorm=NULL;
				old_tex=NULL;
			}

			// Cull unvisible geometry ------------------------------------------------------
			//
			if (bGroupCull) if (!D9IsBSVisible(&Grp[g].BBox, &mWorldView, &Field)) continue;


			// Setup Textures and Normal Maps ==========================================================================
			//
			if (bTextured) {

				if (Tex[ti]!=old_tex) {

					if (tni && Grp[g].TexMixEx[0]<0.5f) tni=0;

					old_tex = Tex[ti];
					FX->SetTexture(eTex0, Tex[ti]->GetTexture());

					if (tni && Tex[tni]) {
						FX->SetTexture(eEmisMap, Tex[tni]->GetTexture());
						//FX->SetBool(eUseEmis, true);
					} //else FX->SetBool(eUseEmis, false);

					if (bUseNormalMap) if (pNorm) FX->SetTexture(eTex3, pNorm);
				}
			}

			// Setup Mesh group material ==============================================================================
			//
			if (Grp[g].MtrlIdx==SPEC_DEFAULT) mat = &defmat;
			else							  mat = &Mtrl[Grp[g].MtrlIdx];

			if (mat!=old_mat) {
				old_mat = mat;
				FX->SetValue(eMtrl, mat, sizeof(VulkanMatExt)-4);
				if (bModulateMatAlpha || bTextured==false) FX->SetFloat(eMtrlAlpha, mat->Diffuse.w);
				else FX->SetFloat(eMtrlAlpha, 1.0f);
			}

			// Setup Mesh drawing options =================================================================================
			//
			FX->SetBool(eTextured, bTextured);
			FX->SetBool(eFullyLit, (Grp[g].UsrFlag&0x4)!=0);

			// FX->CommitChanges() stood here, inside a pass opened before this
			// loop. The pass opens per draw instead -- see Render().
			FX->BeginPass(pass);
			vkCmdDrawIndexed(cmd, Grp[g].nFace*3, 1, Grp[g].IdexOff, (int32_t)Grp[g].VertOff, 0);
			FX->EndPass();

			VulkanStats.Mesh.Vertices += Grp[g].nVert;
			VulkanStats.Mesh.MeshGrps++;
		}
	}
	FX->End();
}


// ================================================================================================
// TWO CALLS IN HERE PASS A MATRIX WHERE A POINTER IS WANTED.
//
// `D3DXMatrixIdentity(MeshShader::vs_const.mW)` and
// `D3DXMatrixMultiply(MeshShader::vs_const.mW, ...)` are written without the
// address-of operator, although `mW` is a float4x4 VALUE -- three lines below,
// `MeshShader::vs_const.mW = mWorldMesh;` assigns to it as one. The address is
// what both functions need, and it is what they are given here.
//
void VulkanMesh::RenderShadowMap(const FMATRIX4 *pW, const FMATRIX4 *pVP, int opt)
{
	if (!IsOK()) return;

	pBuf->Map(pDev);

	FMATRIX4 mWorldMesh;

	MeshShader* pShader = nullptr;
	
	MeshShader::vs_const.mVP = *pVP;

	VMAT_Identity(&MeshShader::vs_const.mW);
	
	if (bGlobalTF) VMAT_MatrixMultiply(&mWorldMesh, &mTransform, pW);
	else mWorldMesh = *pW;

	VkCommandBuffer cmd = pDev->GetCommandBuffer();
	if (!cmd || !pBuf->pIB) return;

	// Was SetStreamSource with a different buffer and stride per branch. The
	// binds are commands here; the STRIDE moves into the vertex declaration
	// each Setup() names, which is where a VkPipeline keeps it.
	VulkanBuffer *pStream = NULL;

	if (opt == 1) {
		// Screenspace Depth and Normal buffer rendering
		pStream = pBuf->pVB;
		pShader = s_pShader[SHADER_NORMAL_DEPTH];
		pShader->Setup(pMeshVertexDecl, true, 0);
	}
	else {
		if (Flags & 0x20) {
			// Regular shadowmap with OIT support
			pStream = pBuf->pSB;
			pShader = s_pShader[SHADER_SHADOWMAP_OIT];
			pShader->Setup(pPosTexDecl, true, 0);
		}
		else {
			// Regular shadowmap for self shadowing
			pStream = pBuf->pGB;
			pShader = s_pShader[SHADER_SHADOWMAP];
			pShader->Setup(pVector4Decl, true, 0);
		}
	}

	if (!pStream) return;

	{
		VkBuffer vb = pStream->Buffer();
		VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
		vkCmdBindIndexBuffer(cmd, pBuf->pIB->Buffer(), 0, VK_INDEX_TYPE_UINT16);
	}

	pShader->ClearTextures();

	bool bInit = true;

	for (DWORD g = 0; g < nGrp; g++)
	{
		if (Grp[g].UsrFlag & 0x2) continue;
		if (Grp[g].UsrFlag & 0x1) continue;
		
		MeshShader::ps_bools.bOIT = (Grp[g].UsrFlag & 0x20) != 0;

		if (MeshShader::ps_bools.bOIT) {
			DWORD ti = Grp[g].TexIdx;
			if (ti) {
				auto hTex = Tex[ti]->GetTexture();
				if (hTex) pShader->SetTexture(pShader->hPST[0], hTex, IPF_WRAP | IPF_POINT);
				else MeshShader::ps_bools.bOIT = false;
			}
			else MeshShader::ps_bools.bOIT = false;
		}

		if (Grp[g].bTransform) {
			VMAT_MatrixMultiply(&MeshShader::vs_const.mW, &pGrpTF[g], pW);		// Apply Animations to instance matrices
			bInit = true;
		}
		else {
			if (bInit) MeshShader::vs_const.mW = mWorldMesh;
			bInit = false;
		}

		if (pShader->hVSC) pShader->SetVSConstants(pShader->hVSC, &MeshShader::vs_const, sizeof(MeshShader::vs_const));
		if (pShader->hPSC) pShader->SetPSConstants(pShader->hPSC, &MeshShader::ps_const, sizeof(MeshShader::ps_const));
		if (pShader->hPSB) pShader->SetPSConstants(pShader->hPSB, &MeshShader::ps_bools, sizeof(MeshShader::ps_bools));
		pShader->UpdateTextures();

		vkCmdDrawIndexed(cmd, Grp[g].nFace*3, 1, Grp[g].IdexOff, (int32_t)Grp[g].VertOff, 0);
	}
}


// ================================================================================================
//
void VulkanMesh::RenderStencilShadows(float alpha, const FMATRIX4 *pP, const FMATRIX4 *pW, bool bShadowMap, const FVECTOR4 *elev)
{
	if (!IsOK()) return;

	DWORD Pass = 0;
	FMATRIX4 GroupMatrix, mWorldMesh; UINT numPasses = 0;

	if (bGlobalTF) VMAT_MatrixMultiply(&mWorldMesh, &mTransform, pW);
	else mWorldMesh = *pW;

	VkCommandBuffer cmd = pDev->GetCommandBuffer();
	if (!cmd || !pBuf->pSB || !pBuf->pIB) return;

	// The D3DPT_TRIANGLELIST of the draws below; see the note in RenderGroup.
	// ShadowTech was one of the two techniques the layer caught in a
	// POINT_LIST pipeline it had no business being in.
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	FX->SetVertexDecl(pPosTexDecl);
	{
		VkBuffer vb = pBuf->pSB->Buffer();
		VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
		vkCmdBindIndexBuffer(cmd, pBuf->pIB->Buffer(), 0, VK_INDEX_TYPE_UINT16);
	}

	FX->SetTechnique(eShadowTech);
	
	if (elev) FX->SetVector(eInScatter, elev);
	else FX->SetVector(eInScatter, ptr(FVECTOR4(0.0f,1.0f,0.0f,0.0f)));

	FX->SetFloat(eMix, alpha);
	FX->Begin(&numPasses, 0);
	
	if (pP) FX->SetValue(eGT, pP, sizeof(FMATRIX4));	// Shadow Projection

	bool bInit = true;

	for (DWORD g = 0; g < nGrp; g++) {

		if (Grp[g].UsrFlag & 0x2) continue;
		if ((Grp[g].UsrFlag & 0x1) && (bShadowMap == false)) continue;

		bool bOIT = (Grp[g].UsrFlag & 0x20) != 0;
		
		if (bOIT) {
			DWORD ti = Grp[g].TexIdx;
			if (ti) {
				auto hTex = Tex[ti]->GetTexture();
				if (hTex) {
					FX->SetTexture(eTex0, hTex);
				} else bOIT = false;
			} else bOIT = false;
		}

		FX->SetBool(eOITEnable, bOIT);

		if (Grp[g].bTransform) {
			VMAT_MatrixMultiply(&GroupMatrix, &pGrpTF[g], pW);		// Apply Animations to instance matrices
			FX->SetValue(eW, &GroupMatrix, sizeof(FMATRIX4));
			bInit = true;
		}
		else {
			if (bInit) {
				FX->SetValue(eW, &mWorldMesh, sizeof(FMATRIX4));
			}
			bInit = false;
		}

		// FX->CommitChanges() stood here, and the single BeginPass(Pass) that
		// bracketed this loop stood above it. One pass per draw -- see Render().
		FX->BeginPass(Pass);
		vkCmdDrawIndexed(cmd, Grp[g].nFace*3, 1, Grp[g].IdexOff, (int32_t)Grp[g].VertOff, 0);
		FX->EndPass();
	}

	FX->End();
}


// ================================================================================================
//
void VulkanMesh::RenderShadowsEx(float alpha, const FMATRIX4 *pP, const FMATRIX4 *pW, const FVECTOR4 *light, const FVECTOR4 *param)
{
	if (!IsOK()) return;

	VulkanStats.Mesh.Meshes++;

	VkCommandBuffer cmd = pDev->GetCommandBuffer();
	if (!cmd || !pBuf->pSB || !pBuf->pIB) return;

	// The D3DPT_TRIANGLELIST of the draws below; see the note in RenderGroup.
	// ShadowTech was one of the two techniques the layer caught in a
	// POINT_LIST pipeline it had no business being in.
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	FX->SetVertexDecl(pPosTexDecl);
	{
		VkBuffer vb = pBuf->pSB->Buffer();
		VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
		vkCmdBindIndexBuffer(cmd, pBuf->pIB->Buffer(), 0, VK_INDEX_TYPE_UINT16);
	}

	FX->SetTechnique(eShadowTech);
	FX->SetMatrix(eW, pW);
	FX->SetMatrix(eGT, pP);
	FX->SetFloat(eMix, alpha);
	if (light) FX->SetVector(eColor, light);
	else FX->SetVector(eColor, ptr(FVECTOR4(0.0f,1.0f,0.0f,0.0f)));
	FX->SetVector(eTexOff, param);


	UINT numPasses = 0;
	FX->Begin(&numPasses, 0);

	for (DWORD g = 0; g<nGrp; g++) {

		if (Grp[g].UsrFlag & 0x3) continue;

		bool bOIT = (Grp[g].UsrFlag & 0x20) != 0;

		if (bOIT) {
			DWORD ti = Grp[g].TexIdx;
			if (ti) {
				auto hTex = (Tex[ti] ? Tex[ti]->GetTexture() : NULL);
				if (hTex) {
					FX->SetTexture(eTex0, hTex);
				}
				else bOIT = false;
			}
			else bOIT = false;
		}

		FX->SetBool(eOITEnable, bOIT);

		FX->BeginPass(1);
		vkCmdDrawIndexed(cmd, Grp[g].nFace*3, 1, Grp[g].IdexOff, (int32_t)Grp[g].VertOff, 0);
		FX->EndPass();

		VulkanStats.Mesh.Vertices += Grp[g].nVert;
		VulkanStats.Mesh.MeshGrps++;
	}

	FX->End();
}



// ================================================================================================
// This is a rendering routine for a Exterior Mesh, non-spherical moons/asteroids
//
// TWO DrawPrimitiveUP CALLS WITH TWO TOPOLOGIES, and that is what shapes the
// pass structure here. D3DPT_LINESTRIP and D3DPT_LINELIST were arguments to
// the draw on Windows; a VkPipeline bakes the topology in, so two topologies
// are two pipelines and therefore two passes. SetTopology declares each one
// before its BeginPass, which is the point at which the pipeline is fetched.
//
// THE COUNTS CHANGE MEANING. DrawPrimitiveUP takes a PRIMITIVE count -- 9
// line-strip segments and 3 line-list lines -- and DrawUP takes a VERTEX
// count, which is 10 and 6. Same finding as DrawUP's own note in
// VulkanEffect.h, and getting it wrong draws part of the box.
//
// D3DVECTOR becomes FVECTOR3: twelve bytes either way, which is what
// pPositionDecl's stride says. It is spelled with constructors rather than
// braces because FVECTOR3 is a union with user-declared constructors and so
// not an aggregate.
//
void VulkanMesh::RenderBoundingBox(const FMATRIX4 *pW)
{
	_TRACE;

	if (!IsOK()) return;
	if (DebugControls::IsActive()==false) return;

	FMATRIX4 q;

	static const FVECTOR3 poly[10] = {
		FVECTOR3(0.0f, 0.0f, 0.0f),
		FVECTOR3(1.0f, 0.0f, 0.0f),
		FVECTOR3(1.0f, 1.0f, 0.0f),
		FVECTOR3(0.0f, 1.0f, 0.0f),
		FVECTOR3(0.0f, 0.0f, 0.0f),
		FVECTOR3(0.0f, 0.0f, 1.0f),
		FVECTOR3(1.0f, 0.0f, 1.0f),
		FVECTOR3(1.0f, 1.0f, 1.0f),
		FVECTOR3(0.0f, 1.0f, 1.0f),
		FVECTOR3(0.0f, 0.0f, 1.0f)
	};

	static const FVECTOR3 list[6] = {
		FVECTOR3(1.0f, 0.0f, 0.0f),
		FVECTOR3(1.0f, 0.0f, 1.0f),
		FVECTOR3(1.0f, 1.0f, 0.0f),
		FVECTOR3(1.0f, 1.0f, 1.0f),
		FVECTOR3(0.0f, 1.0f, 0.0f),
		FVECTOR3(0.0f, 1.0f, 1.0f)
	};



	DWORD flags  = *(DWORD*)gc->GetConfigParam(CFGPRM_GETDEBUGFLAGS);
	DWORD selmsh = *(DWORD*)gc->GetConfigParam(CFGPRM_GETSELECTEDMESH);
	DWORD selgrp = *(DWORD*)gc->GetConfigParam(CFGPRM_GETSELECTEDGROUP);
	bool  bSel   =  (uCurrentMesh==selmsh);


	if (flags&(DBG_FLAGS_SELVISONLY|DBG_FLAGS_SELMSHONLY|DBG_FLAGS_SELGRPONLY) && DebugControls::GetVisual()!=pCurrentVisual) return;
	if (flags&DBG_FLAGS_SELMSHONLY && !bSel) return;
	if (flags&DBG_FLAGS_SELGRPONLY && !bSel) return;

	if (flags&DBG_FLAGS_BOXES) {

		FX->SetVertexDecl(pPositionDecl);

		// ----------------------------------------------------------------
		FX->SetMatrix(eW, pW);
		FX->SetVector(eColor, ptr(FVECTOR4(0.0f, 1.0f, 0.0f, 0.5f)));
		FX->SetTechnique(eBBTech);
		// ----------------------------------------------------------------

		UINT numPasses = 0;
		FX->Begin(&numPasses, 0);

		for (DWORD g=0; g<nGrp; g++) {

			if (flags&DBG_FLAGS_SELGRPONLY && g!=selgrp) continue;
			if (Grp[g].UsrFlag & 0x2) continue;

			FX->SetVector(eAttennuate, &Grp[g].BBox.min);
			FX->SetVector(eInScatter, &Grp[g].BBox.max);

			// Apply Animations =========================================================================================
			//
			if (Grp[g].bTransform) {
				if (bGlobalTF) {
					VMAT_MatrixMultiply(&q, &mTransform, &Grp[g].Transform);
					FX->SetMatrix(eGT, &q);
				}
				else FX->SetMatrix(eGT, &Grp[g].Transform);
			}
			else FX->SetMatrix(eGT, &mTransform);


			// Setup Mesh drawing options =================================================================================
			//
			// FX->CommitChanges() stood here; see Render(). The two draws are
			// two topologies and therefore two passes -- see the note above.

			FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_LINE_STRIP);
			FX->BeginPass(0);
			FX->DrawUP(poly, 10, sizeof(FVECTOR3), NULL, 0);
			FX->EndPass();

			FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
			FX->BeginPass(0);
			FX->DrawUP(list, 6, sizeof(FVECTOR3), NULL, 0);
			FX->EndPass();
		}

		// Topology is REMEMBERED between calls (see VulkanEffect.h), so it is
		// put back to the triangle list every other caller assumes.
		FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);

		FX->End();
	}

	if (flags&DBG_FLAGS_SPHERES) {
		for (DWORD g=0; g<nGrp; g++) {
			if (flags&DBG_FLAGS_SELGRPONLY && g!=selgrp) continue;
			if (Grp[g].UsrFlag & 0x2) continue;
			VulkanEffect::RenderBoundingSphere(pW, NULL, &Grp[g].BBox.bs, ptr(FVECTOR4(0.0f,1.0f,0.0f,0.75f)));
		}
	}
	if (flags&DBG_FLAGS_BOXES) VulkanEffect::RenderBoundingBox(pW, &mTransform, &BBox.min, &BBox.max, ptr(FVECTOR4(0.0f,0.0f,1.0f,0.75f)));
	if (flags&DBG_FLAGS_SPHERES) VulkanEffect::RenderBoundingSphere(pW, &mTransform, &BBox.bs, ptr(FVECTOR4(0.0f,0.0f,1.0f,0.75f)));
}


// ===========================================================================================
// The second of the two <xnamath.h> users, and the same treatment: XMVectorMin
// and XMVectorMax over a vertex list are a componentwise min and max. See
// UpdateTangentSpace above, and VBase.cpp's CheckMeshStats.
//
// XMVectorSetW(mi, 0) survives as the explicit `.w = 0` on each corner: the
// bounding box's min and max are FVECTOR4s whose w is not a coordinate.
//
void VulkanMesh::BoundingBox(const NMVERTEX *vtx, DWORD n, D9BBox *box)
{
	FVECTOR3 mi, mx;
	mi = mx = FVECTOR3(vtx[0].x, vtx[0].y, vtx[0].z);
	for (DWORD i = 1; i < n; i++) {
		FVECTOR3 x = FVECTOR3(vtx[i].x, vtx[i].y, vtx[i].z);
		mi = vmin(mi, x);
		mx = vmax(mx, x);
	}
	box->min = FVECTOR4(mi, 0.0f);
	box->max = FVECTOR4(mx, 0.0f);
}

// ===========================================================================================
//
void VulkanMesh::TransformGroup(DWORD n, const FMATRIX4 *m)
{
	if (!IsOK()) return;
	if(n>=nGrp) {
		char buf[256];
		snprintf(buf, 256, "Group %d for mesh %s does not exist", n, name);
		oapiAddNotification(OAPINOTIF_ERROR, "Invalid mesh group", buf);
		return;
	}

	bBSRecompute = true;

	// `Grp[n].Transform = Grp[n].Transform * (*m);` -- D3DXMATRIX has an
	// operator*, FMATRIX4 does not. VMAT_MatrixMultiply(out, a, b) applies a
	// first, which is what `a * b` meant under D3DX's row-vector convention,
	// and it builds into a local before assigning, so out may alias an input.
	VMAT_MatrixMultiply(&Grp[n].Transform, &Grp[n].Transform, m);
	Grp[n].bTransform = true;
	Grp[n].bUpdate = true;

	VMAT_MatrixMultiply(&pGrpTF[n], &mTransform, &Grp[n].Transform);
}

// ===========================================================================================
//
void VulkanMesh::Transform(const FMATRIX4 *m)
{
	if (!IsOK()) return;

	bBSRecompute = true;
	bBSRecomputeAll = true;
	bGlobalTF = true;
	VMAT_MatrixMultiply(&mTransform, &mTransform, m);

	MatrixInverse(mTransformInv, mTransform);

	for (DWORD i=0;i<nGrp;i++) {
		if (Grp[i].bTransform) VMAT_MatrixMultiply(&pGrpTF[i], &mTransform, &Grp[i].Transform);
		else pGrpTF[i] = mTransform;
	}
}

// ===========================================================================================
//
void VulkanMesh::SetPosition(VECTOR3 &pos)
{
	bGlobalTF = true;

	// _41.._43 -> m41..m43. DrawAPI.h declares the underscore-style views only
	// under #ifdef _WIN32; the m** names are the same storage and are always
	// available. Same finding as VBase.cpp's.
	mTransform.m41 = float(pos.x);
	mTransform.m42 = float(pos.y);
	mTransform.m43 = float(pos.z);

	MatrixInverse(mTransformInv, mTransform);

	for (DWORD i = 0; i<nGrp; i++) {
		if (Grp[i].bTransform) VMAT_MatrixMultiply(&pGrpTF[i], &mTransform, &Grp[i].Transform);
		else pGrpTF[i] = mTransform;
	}
}

// ===========================================================================================
//
void VulkanMesh::SetRotation(FMATRIX4 &rot)
{
	bGlobalTF = true;

	// TODO: BUG: Position will be acquired from rot matrix
	//
	memcpy((void*)&mTransform, (const void*)&rot, 48);

	MatrixInverse(mTransformInv, mTransform);

	for (DWORD i = 0; i<nGrp; i++) {
		if (Grp[i].bTransform) VMAT_MatrixMultiply(&pGrpTF[i], &mTransform, &Grp[i].Transform);
		else pGrpTF[i] = mTransform;
	}
}

// ===========================================================================================
//
void VulkanMesh::UpdateBoundingBox()
{
	if (!IsOK()) return;
	if (bBSRecompute==false) return;

	for (DWORD i=0;i<nGrp;i++) {
		if (Grp[i].bUpdate || bBSRecomputeAll) {
			Grp[i].bUpdate = false;
			if (bGlobalTF) {
				if (Grp[i].bTransform)	D9UpdateAABB(&Grp[i].BBox, &mTransform, &Grp[i].Transform);
				else					D9UpdateAABB(&Grp[i].BBox, &mTransform);
			}
			else {
				if (Grp[i].bTransform)	D9UpdateAABB(&Grp[i].BBox, &Grp[i].Transform);
				else					D9UpdateAABB(&Grp[i].BBox);
			}
		}
	}

	bBSRecomputeAll = false;
	bBSRecompute = false;

	if (nGrp==0) {
		BBox.min = FVECTOR4(0.0f,0.0f,0.0f,0.0f);
		BBox.max = FVECTOR4(0.0f,0.0f,0.0f,0.0f);
	}
	else {
		for (DWORD i=0;i<nGrp;i++) {
			if (Grp[i].bTransform) {
				if (bGlobalTF) {
					FMATRIX4 q;
					// The nested D3DXMatrixMultiply, unnested: VMAT_ returns
					// void. The output aliases an input on the second call,
					// which is safe -- VMAT_MatrixMultiply builds into a local
					// and assigns at the end.
					VMAT_MatrixMultiply(&q, &mTransform, &Grp[i].Transform);
					VMAT_MatrixMultiply(&q, &q, &mTransformInv);
					D9AddAABB(&Grp[i].BBox, &q, &BBox, i==0);
				}
				else D9AddAABB(&Grp[i].BBox, &Grp[i].Transform, &BBox, i==0);
			}
			else {
				D9AddAABB(&Grp[i].BBox, NULL, &BBox, i==0);
			}
		}
	}

	D9UpdateAABB(&BBox, &mTransform);
}


// ===========================================================================================
//
D9BBox * VulkanMesh::GetAABB()
{
	if (!IsOK()) return nullptr;
	UpdateBoundingBox();
	return &BBox;
}

// ===========================================================================================
//
FVECTOR3 VulkanMesh::GetBoundingSpherePos()
{
	if (!IsOK()) return FVECTOR3(0,0,0);
	UpdateBoundingBox();
	return FVECTOR3f4(BBox.bs);
}

// ===========================================================================================
//
float VulkanMesh::GetBoundingSphereRadius()
{
	if (!IsOK()) return 0.0f;
	UpdateBoundingBox();
	return BBox.bs.w;
}

// ===========================================================================================
// D3DXIntersectTri is IntersectTri in VectorHelpers.h -- the second caller in
// the client, after Tile::Pick, which is why it lives in the shared header
// rather than twice. Its output convention and the reason the two-sided
// variant is safe are recorded there.
//
// D3DXVec3Cross / Dot / Normalize / TransformCoord / TransformNormal become
// the SDK's cross / dot / unit / oapi::TransformCoord / oapi::TransformNormal,
// which return their result instead of writing through an out-parameter.
// D3DXMatrixInverse's determinant out-parameter is MatrixInverse's optional
// third argument, and `det` is never read here.
//
VulkanPick VulkanMesh::Pick(const FMATRIX4 *pW, const FMATRIX4 *pT, const FVECTOR3 *vDir)
{
	VulkanPick result;
	result.dist  = 1e30f;
	result.pMesh = NULL;
	result.vObj  = NULL;
	result.group = -1;
	result.idx = -1;

	if (!pBuf->pGBSys || !pBuf->pIBSys) {
		LogErr("VulkanMesh::Pick() Failed: No Geometry Available");
		return result;
	}

	UpdateBoundingBox();

	FMATRIX4 mW, mWT, mWorldMesh;

	if (pT) VMAT_MatrixMultiply(&mWT, pT, pW);
	else mWT = *pW;

	if (bGlobalTF) VMAT_MatrixMultiply(&mWorldMesh, &mTransform, &mWT);
	else mWorldMesh = mWT;

	for (DWORD g=0;g<nGrp;g++) {

		if ((Grp[g].UsrFlag & 0x2) && (Grp[g].MFDScreenId != 0x100)) continue;

		FVECTOR3 bs = FVECTOR3f4(Grp[g].BBox.bs);
		float rad = Grp[g].BBox.bs.w * VMAT_BSScaleFactor(&mWT);

		bs = oapi::TransformCoord(bs, mWT);

		float dst = dot(bs, *vDir);
		float len2 = dot(bs, bs);

		if (dst < -rad) continue;
		if (sqrt(len2 - dst*dst) > rad) continue;

		if (Grp[g].bTransform) VMAT_MatrixMultiply(&mW, &pGrpTF[g], &mWT);
		else mW = mWorldMesh;

		FVECTOR3 _a, _b, _c, cp;

		WORD *pIdc = pBuf->pIBSys + Grp[g].IdexOff;
		FVECTOR4 *pVrt = pBuf->pGBSys + Grp[g].VertOff;

		FMATRIX4 mWI;
		MatrixInverse(mWI, mW);

		FVECTOR3 pos = oapi::TransformCoord(FVECTOR3(0.0f, 0.0f, 0.0f), mWI);
		FVECTOR3 dir = oapi::TransformNormal(*vDir, mWI);

		for (DWORD i=0;i<Grp[g].nFace;i++) {

			WORD a = pIdc[i*3+0];
			WORD b = pIdc[i*3+1];
			WORD c = pIdc[i*3+2];

			_a = FVECTOR3f4(pVrt[a]);
			_b = FVECTOR3f4(pVrt[b]);
			_c = FVECTOR3f4(pVrt[c]);

			float u, v, dsq;

			cp = cross(_c - _b, _a - _b);

			if (dot(cp, dir)<0) {
				if (IntersectTri(_c, _b, _a, pos, dir, &u, &v, &dsq)) {
					if (dsq > 0.1f) {
						if (dsq < result.dist) {
							result.dist = dsq;
							result.group = int(g);
							result.pMesh = this;
							result.idx = int(i);
							result.u = u;
							result.v = v;
						}
					}
				}
			}
		}
	}


	if (result.idx >= 0 && result.group >= 0) {

		int i = result.idx;
		int g = result.group;

		if (Grp[g].bTransform) mW = pGrpTF[g];
		else {
			if (bGlobalTF) mW = mTransform;
			else VMAT_Identity(&mW);
		}

		if (pT) VMAT_MatrixMultiply(&mW, &mW, pT);

		FVECTOR3 cp;

		WORD *pIdc = &pBuf->pIBSys[Grp[g].IdexOff];
		FVECTOR4 *pVrt = &pBuf->pGBSys[Grp[g].VertOff];

		WORD a = pIdc[i * 3 + 0];
		WORD b = pIdc[i * 3 + 1];
		WORD c = pIdc[i * 3 + 2];

		FVECTOR3 _a = FVECTOR3f4(pVrt[a]);
		FVECTOR3 _b = FVECTOR3f4(pVrt[b]);
		FVECTOR3 _c = FVECTOR3f4(pVrt[c]);

		float u = result.u;
		float v = result.v;

		cp = cross(_c - _b, _a - _b);

		cp = oapi::TransformNormal(cp, mW);
		result.normal = unit(cp);

		FVECTOR3 p = (_b * u) + (_a * v) + (_c * (1.0f - u - v));
		result.pos = oapi::TransformCoord(p, mW);
	}

	return result;
}



// ===========================================================================================
// SPECIAL RENDER FUNCTIONS SECTION
// ===========================================================================================
//

// This is a special rendering routine used to render 3D arrow --------------------------------
//
// The CULLMODE pair around RenderGroup(0) is a PassOverride: it was device
// state set after BeginPass, and it is pipeline state here. The restore has
// nothing to restore -- see Render().
//
void VulkanMesh::RenderAxisVector(FMATRIX4 *pW, const FVECTOR4 *pColor, float len)
{
	UINT numPasses = 0;
	FX->SetTechnique(eAxisTech);
	FX->SetFloat(eMix, len);
	FX->SetValue(eColor, pColor, sizeof(FVECTOR4));
	FX->SetMatrix(eW, pW);
	// BEFORE Begin(), not inside RenderGroup. RenderGroup sets the topology
	// too, but it runs inside the already-open pass below, so its call reaches
	// the NEXT pipeline build rather than this one -- which is the same trap
	// the note on SetVertexDecl there describes. See RenderGroup for what a
	// leaked POINT_LIST costs.
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	FX->Begin(&numPasses, 0);

	VulkanEffectFile::PassOverride ovr;
	ovr.cullMode = VulkanEffectFile::PassOverride::CULL_CW;

	FX->BeginPassEx(0, &ovr);
	RenderGroup(0);
	FX->EndPass();
	FX->End();
}

// Used only by ring manager --------------------------------------------------------------------
//
void VulkanMesh::RenderRings(const FMATRIX4 *pW, VulkanTexture *pTex)
{
	_TRACE;
	if (!IsOK()) return;
	if (!pTex) return;

	VulkanStats.Mesh.Vertices += Grp[0].nVert;
	VulkanStats.Mesh.MeshGrps++;

	UINT numPasses = 0;
	FX->SetTechnique(eRingTech);
	FX->SetMatrix(eW, pW);
	FX->SetTexture(eTex0, pTex);
	FX->SetValue(eSun, &sunLight, sizeof(VulkanSun));
	FX->SetValue(eMtrl, &defmat, sizeof(VulkanMatExt)-4);
	// Before Begin(); see RenderAxisVector.
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	FX->Begin(&numPasses, 0);
	FX->BeginPass(0);
	RenderGroup(0);
	FX->EndPass();
	FX->End();
}

// Used only by ring manager --------------------------------------------------------------------
//
void VulkanMesh::RenderRings2(const FMATRIX4 *pW, VulkanTexture *pTex, float irad, float orad)
{
	_TRACE;
	if (!IsOK()) return;
	if (!pTex) return;

	VulkanStats.Mesh.Vertices += Grp[0].nVert;
	VulkanStats.Mesh.MeshGrps++;

	UINT numPasses = 0;
	FX->SetTechnique(eRingTech2);
	FX->SetMatrix(eW, pW);
	FX->SetTexture(eTex0, pTex);
	FX->SetValue(eSun, &sunLight, sizeof(VulkanSun));
	FX->SetValue(eMtrl, &defmat, sizeof(VulkanMatExt)-4);
	FX->SetVector(eTexOff, ptr(FVECTOR4(irad, orad, 0.0f, 0.0f)));
	// Before Begin(); see RenderAxisVector.
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	FX->Begin(&numPasses, 0);
	FX->BeginPass(0);
	RenderGroup(0);
	FX->EndPass();
	FX->End();
}


// ===========================================================================================
// Modules/D3D9Client/NewMesh.hlsl -> Modules/VulkanClient/NewMesh.glsl. The
// module directory and the shading language both follow the module; the three
// ENTRY POINT NAMES do not, because they are content the GLSL must match.
//
void VulkanMesh::GlobalInit(VulkanDevice *pDev)
{
	memset(s_pShader, 0, sizeof(s_pShader));

	s_pShader[SHADER_SHADOWMAP] = new MeshShader(pDev, "Modules/VulkanClient/NewMesh.glsl", "ShdMapVS", "ShdMapPS");
	s_pShader[SHADER_SHADOWMAP_OIT] = new MeshShader(pDev, "Modules/VulkanClient/NewMesh.glsl", "ShdMapOIT_VS", "ShdMapOIT_PS");
	s_pShader[SHADER_NORMAL_DEPTH] = new MeshShader(pDev, "Modules/VulkanClient/NewMesh.glsl", "NormalDepth_VS", "NormalDepth_PS");
}


// ===========================================================================================
//
void VulkanMesh::GlobalExit()
{
	for (auto x : s_pShader) if (x) delete x;
}
