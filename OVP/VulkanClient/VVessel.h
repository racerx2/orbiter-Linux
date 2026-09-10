// ==============================================================
// VVessel.h
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2006-2026 Martin Schweiger
//				 2012-2016 Jarmo Nikkanen
// ==============================================================
//
// CONVERTED FROM OVP/D3D9Client/VVessel.h, read end to end (229 lines).
//
// The vessel visual. A declaration file; the types change and the structure
// does not:
//
//   oapi::D3D9Client        -> oapi::VulkanClient
//   LPDIRECT3DDEVICE9       -> VulkanDevice*
//   LPD3DXMATRIX / D3DXMATRIX -> FMATRIX4* / FMATRIX4
//   D3DXVECTOR3             -> FVECTOR3
//   D3D9Pad                 -> VulkanPad
//   D3D9Mesh                -> VulkanMesh
//   D3D9Pick                -> VulkanPick
//   LPDIRECT3DTEXTURE9      -> VulkanTexture*
//
// **LPDIRECT3DCUBETEXTURE9 ALSO BECOMES VulkanTexture\*, and that is the one
// substitution worth pausing on.** D3D9 had a separate INTERFACE for a cube
// map. Vulkan does not: a cube map is a VkImage with six array layers and a
// VkImageView created with viewType VK_IMAGE_VIEW_TYPE_CUBE. So the type is
// the same type, and "is it a cube" is a property of how it was created --
// which is why the three environment-map members below lose their distinct
// type and keep their distinct names.
//
// `bool const Playback() const` loses the top-level const on its returned
// prvalue (-Wignored-qualifiers); the value is unaffected. Same finding as
// OapiExtension.h's and Tilemgr2.h's.
// ==============================================================

#ifndef __VVESSEL_H
#define __VVESSEL_H

#include "VObject.h"
#include "Mesh.h"
#include "gcCore.h"
#include <unordered_set>
#include <vector>

typedef struct {
	float fdata;
	VECTOR3 ref, vdata;
	std::vector<VECTOR3> vtx;
} _defstate;



// ==============================================================
// class vVessel (interface)
// ==============================================================

/**
 * \brief Visual representation of a vessel object.
 *
 * The vVessel instance is not persistent: It is created when the
 * object moves into visual range of the camera, and is destroyed
 * when it moves out of visual range.
 *
 * The vVessel contains a set of meshes and animations. At each
 * time step, it synchronises the visual with the logical animation
 * state, and renders the resulting meshes.
 */

class vVessel: public vObject {
public:
	friend class VulkanClient;
	/**
	 * \brief Creates a new vessel visual for a scene
	 * \param _hObj vessel object handle
	 * \param scene scene to which the visual is added
	 */
	vVessel (OBJHANDLE _hObj, const Scene *scene);

	~vVessel ();

	/**
	 * \brief Set up global parameters shared by all instances
	 * \param gclient client instance pointer
	 */
	static void GlobalInit (oapi::VulkanClient *gc);

	/**
	 * \brief Release global parameters
	 */
	static void GlobalExit ();

	void clbkEvent(DWORD evnt, DWORD_PTR context);

	MESHHANDLE GetMesh (UINT idx);
	bool GetMinMaxDistance(float *zmin, float *zmax, float *dmin);
	void GetMinMaxLightDist(float *mind, float *maxd);
	int	 GetMatrixTransform(gcCore::MatrixId matrix_id, DWORD mesh, DWORD group, FMATRIX4 *pMat);
	int  SetMatrixTransform(gcCore::MatrixId matrix_id, DWORD mesh, DWORD group, const FMATRIX4 *pMat);
	void UpdateBoundingBox();
	bool IsInsideShadows();
	bool IntersectShadowVolume();
	bool IntersectShadowTarget();
	void ReloadTextures();
	
	inline DWORD GetMeshCount();

	void PreInitObject();

	VESSEL *GetInterface() { return vessel; }

	/**
	 * \brief Per-frame object parameter updates
	 * \return \e true if update was performed, \e false if skipped.
	 * \action
	 *   - Calls vObject::Update
	 *   - Calls \ref UpdateAnimations
	 */
	bool Update (bool bMainScene);

	/**
	 * \brief Object render call
	 * \param dev render device
	 * \return \e true if render operation was performed (object active),
	 *   \e false if skipped (object inactive)
	 * \action Calls Render(dev,false), i.e. performs the external render pass.
	 * \sa Render(VulkanDevice*,bool)
	 */
	bool Render (VulkanDevice *dev);

	/**
	 * \brief Object render call
	 * \param dev render device
	 * \param internalpass flag for internal render pass
	 * \note This method renders either the external vessel meshes
	 *   (internalpass=false) or internal meshes (internalpass=true), e.g.
	 *   the virtual cockpit.
	 * \note The internal pass is only performed on the focus object, and only
	 *   in cockpit camera mode.
	 * \sa Render(VulkanDevice*)
	 */
	bool Render (VulkanDevice *dev, bool bInternalPass);

	bool RenderExhaust();

	/**
	 * \brief Render the vessel's active light beacons
	 * \param dev render device
	 */
	void RenderLightCone (FMATRIX4 *pWT);
	void RenderBeacons (VulkanDevice *dev);
	void RenderReentry (VulkanDevice *dev);
	void RenderGrapplePoints (VulkanDevice *dev);
	void RenderGroundShadow (VulkanDevice *dev, OBJHANDLE hPlanet, float depth);
	void RenderVectors (VulkanDevice *dev, VulkanPad *pSkp);
	bool RenderENVMap (VulkanDevice *pDev, DWORD cnt=2, DWORD flags=0xFF);
	bool ProbeIrradiance(VulkanDevice *pDev, DWORD cnt = 2, DWORD flags = 0xFF);

	// Were LPDIRECT3DCUBETEXTURE9 / LPDIRECT3DTEXTURE9. See the file header:
	// Vulkan has no separate cube-map type.
	VulkanTexture *GetEnvMap(int idx);
	VulkanTexture *GetIrradEnv() { return pIrdEnv; }
	VulkanTexture *GetIrradianceMap() { return pIrrad; }

	float GetExhaustLength() const { return ExhaustLength; }

	VulkanPick Pick(const FVECTOR3 *vDir);

	bool HasExtPass();
	bool HasShadow();
	// Was `bool const Playback() const`; see the file header.
	bool Playback() const { return vessel->Playback(); }
	class MatMgr * GetMaterialManager() const { return pMatMgr; }

	/**
	* \brief Update animations of the visual
	*
	* Synchronises the visual animation states with the logical
	* animation states of the vessel object.
	* \param mshidx mesh index
	* \note If mshidx == (UINT)-1 (default), all meshes are updated.
	*/
	void UpdateAnimations(int mshidx = -1);

protected:

	void LoadMeshes();
	void InsertMesh(UINT idx);
	void DisposeMeshes();
	void DelMesh(UINT idx);
	void ResetMesh(UINT idx);
	void InitNewAnimation(UINT idx);
	void DisposeAnimations();
	void DelAnimation(UINT idx);
	void ResetAnimations(UINT reset = 1);

	/**
	 * \brief Grow \ref animstate buffer (if needed)
	 *
	 * Increases the size of \ref animstate buffer (if needed).
	 * This method also updates the \ref nanim member.
	 * \param newSize new size of buffer (return value of VESSEL::GetAnimPtr())
	 * \return previous value of \ref nanim
	 */
	void GrowAnimstateBuffer (UINT newSize);

	/**
	 * \brief Modify local lighting due to planet shadow or
	 *   atmospheric dispersion.
	 * \param light pointer to VulkanLight structure receiving modified parameters
	 * \return \e true if lighting modifications should be applied, \e false
	 *   if global lighting conditions apply.
	 */
	bool ModLighting();

	void Animate (UINT an, UINT mshidx);
	void AnimateComponent (ANIMATIONCOMP *comp, const FMATRIX4 &T);
	void RestoreDefaultState(ANIMATIONCOMP *AC);
	void StoreDefaultState(ANIMATIONCOMP *AC);
	void DeleteDefaultState(ANIMATIONCOMP *AC);


private:

	// Animation database containing 'default' states.
	//
	std::map<MGROUP_TRANSFORM *, _defstate> defstate;
	std::unordered_set<UINT> applyanim;
	std::map<int, double> currentstate;


	VESSEL *vessel;			// access instance for the vessel
	class MatMgr *pMatMgr;

	VulkanTexture *pEnv[4], *pIrdEnv;
	VulkanTexture *pIrrad;

	int nEnv;				// Number of environmental maps
	int iFace;				// EnvMap Face index that is to be rendered next
	int eFace;

	struct MESHREC {
		VulkanMesh *mesh;	// mesh representation
		FMATRIX4 *trans;	// mesh transformation matrix (rel. to vessel frame)
		WORD vismode;
	} *meshlist;			// list of associated meshes

	UINT nmesh;				// number of meshes
	UINT vClass;
	ANIMATION *anim;		// list of animations (defined in the vessel object)
	double tCheckLight;		// time for next lighting check
	float ExhaustLength;

	static class SurfNative *defreentrytex, *defexhausttex;
};

#endif // !__VVESSEL_H
