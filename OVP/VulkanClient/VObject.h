// ==============================================================
// VObject.h
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2006-2026 Martin Schweiger
//				 2012-2016 Jarmo Nikkanen
// ==============================================================
//
// CONVERTED FROM OVP/D3D9Client/VObject.h, read end to end (277 lines).
//
// No Direct3D behaviour here at all -- vObject is bookkeeping (position,
// rotation, camera distance, sun direction, bounding box) plus a set of
// virtuals that take a render device. So the conversion is the type mapping
// and nothing else:
//
//   D3DXMATRIX mWorld        -> FMATRIX4        (MWorld() returns FMATRIX4*)
//   D3DXVECTOR3 / D3DXCOLOR  -> FVECTOR3 / FVECTOR4
//   LPDIRECT3DDEVICE9 dev    -> VulkanDevice *dev
//   D3D9Client/Pad/Mesh/Sun  -> Vulkan...
//
// Two things worth naming:
//
//  1. GetBoundingSpherePosDX() BECAME GetBoundingSpherePosF(). The "DX"
//     distinguished the D3DXVECTOR3 form from the VECTOR3 form beside it;
//     with D3DX gone the suffix names something that does not exist, and F
//     says what it now returns (FVECTOR3) against the double-precision
//     VECTOR3 of GetBoundingSpherePos().
//
//  2. "Scene.h" BECAME A FORWARD DECLARATION. The Windows header includes it,
//     but every use of Scene in this file is through a pointer -- the
//     constructor parameter, the scn member and GetScene(). Scene.h includes
//     this file back, so the include is a cycle MSVC tolerates through
//     include guards; a forward declaration is enough and lets this header
//     stand on its own.
// ==============================================================

#ifndef __VOBJECT_H
#define __VOBJECT_H

#include "OrbiterAPI.h"
#include "GraphicsAPI.h"
#include "VulkanClient.h"
#include "AABBUtil.h"
#include <vulkan/vulkan.h>
#include "VulkanTypes.h"
#include <vector>
#include <string>

extern class VulkanConfig *Config;

class VulkanPad;
class Scene;
class SurfNative;
class VulkanMesh;


// ==============================================================
// class vObject (interface)
// ==============================================================

/**
 * \brief Visual object base class.
 *
 * A vObject is a render object representing a 'logical'
 * Orbiter object (identified by its OBJHANDLE) in a scene.
 *
 * A "vObject" is the visual representation of an Orbiter object (vessel,
 * planet/moon/sun, surface base). vObjects usually have one or more meshes
 * associated with them that define their visual appearance, but they can be
 * arbitrarily complex (e.g. planets with cloud layers, atmospheric haze,
 * etc.)
 * Visual objects don't persist like their "logical" counterparts, but are
 * created and deleted as they pass in and out of the visual range of a
 * camera.
 * vObjects are therefore associated with a particular scene. In multi-scene
 * environments, a single logical object may have multiple vObjects
 * associated with it.
 */
class vObject: public oapi::VisObject {
public:

	/**
	 * \brief Constructs a new visual object for a scene
	 * \param _hObj object handle
	 * \param scene scene to which the visual is added
	 */
	vObject (OBJHANDLE _hObj, const Scene *scene);

	/**
	 * \brief Destroys the visual object
	 */
	virtual ~vObject () {}

	/**
	 * \brief Set up global parameters shared by all instances
	 * \param gclient client instance pointer
	 */
	static void GlobalInit (oapi::VulkanClient *gclient);

	/**
	 * \brief Release global parameters
	 */
	static void GlobalExit ();

	/**
	 * \brief Creates a specific object from its handle
	 * \param _hObj object handle
	 * \param scene scene to which the visual is added
	 * \note Depending on the object type to which _hObj refers, this method
	 *   creates and returns an instance of the following classes:
	 *   - vVessel (vessel object)
	 *   - vPlanet (planet or moon object)
	 *   - vBase (surface base object)
	 */
	static vObject *Create (OBJHANDLE _hObj, const Scene *scene);

	/**
	 * \brief Activate or deactivate the object
	 * \param isactive \e true to activate, \e false to deactivate
	 * \note Deactivated objects should skip their update and render methods
	 *   to improve performance.
	 * \sa IsActive
	 */
	virtual void Activate (bool isactive);

	/**
	 * \brief Returns activation state
	 * \return \e true for active, \e false for inactive objects.
	 * \sa Activate
	 */
	inline bool IsActive () const { return active; }

	inline const FMATRIX4 * MWorld() const { return &mWorld; }

	inline Scene * GetScene() const { return scn; }
	inline oapi::VulkanClient * GetClient() const { return gc; }
	inline VulkanDevice * GetDevice() const { return gc->GetDevice(); }

	/**
	 * \brief Returns the handle of the associated logical object
	 * \return object handle
	 */
	// `const OBJHANDLE` / `const int` in the Windows original. A top-level
	// const on a BY-VALUE return does nothing -- the caller gets a copy --
	// so the compiler discards it and GCC reports it (-Wignored-qualifiers).
	// Dropped rather than suppressed: no call site changes, and the
	// declaration stops promising a protection it never gave. The trailing
	// const, which is the one that matters, stays.
	inline OBJHANDLE Object() const { return hObj; }
	inline int Type() const { return objtp; }

	/**
	 * \brief Returns one of the visual's meshes, given by its index.
	 * \param idx mesh index (>= 0)
	 * \return Mesh handle
	 * \note Currently only vessel visuals return anything here.
	 */
	virtual MESHHANDLE GetMesh (UINT idx) { return NULL; }

	virtual void PreInitObject() { }

	virtual bool GetMinMaxDistance(float *zmin, float *zmax, float *dmin) { return false; }

	virtual void UpdateBoundingBox();
	virtual bool IsVisible();
	virtual DWORD GetMeshCount();

	// Was GetBoundingSpherePosDX(). See the file header.
	FVECTOR3 GetBoundingSpherePosF();
	VECTOR3 GetBoundingSpherePos();
	float GetBoundingSphereRadius();
	const char *GetName() const;

	/**
	 * \brief Returns distance from camera
	 * \return camera distance [m]
	 * \sa PosFromCamera
	 */
	inline double CamDist() const { return cdist; }

	/**
	 * \brief Returns object size
	 * \return Object size [m]
	 * \sa CamDist
	 */
	inline double GetSize() const { return size; }

	/**
	 * \brief Returns the apparent radius of the Sun
	 * \return Apparent radius of the Sun [sun_rad/distance]
	 */
	inline double SunApparentRad() const { return sunapprad; }

	/**
	 * \brief Returns object position relative to camera
	 * \return relative position vector [<b>m</b>]
	 * \note The returned distance vector is expressed in the ecliptic frame.
	 * \sa CamDist
	 */
	inline double CameraTgtDist() const { return ctgtdst; }
	inline const VECTOR3 &PosFromCamera() const { return cpos; }
	inline const VECTOR3 &GlobalPos() const { return gpos; }

	/**
	 * \brief Returns a unit vector pointing towards the sun
	 * \return A unit vector pointing towards the sun [<b>m</b>]
	 * \note The returned vector is expressed in the ecliptic frame.
	 */
	inline const VECTOR3 &SunDirection() const { return sundir; }
	inline double SunDistance() const { return sundst; }
	inline bool Is(const std::string x) const { return std::string(name) == x; }

	/**
	 * \brief Per-frame object parameter updates
	 * \return \e true if update was performed, \e false if skipped.
	 * \default Copies global and camera-relative position and rotation
	 *   parameters from the logical object. Updates the world matrix.
	 *   Calls CheckResolution.
	 * \note Inactive objects skip this method.
	 * \sa Render, CheckResolution
	 */
	virtual bool Update (bool bMainScene);
	virtual void ReOrigin(VECTOR3 global_pos);

	/**
	 * \brief Level-of-detail check
	 * \default None.
	 * \note Called by Update.
	 */
	virtual void CheckResolution () {}

	/**
	 * \brief Object render call
	 * \param dev Render device
	 * \return \e true if render operation was performed, \e false if skipped.
	 * \default None, returns \e false.
	 */
	virtual bool Render(VulkanDevice *dev) { return false; }

	/**
	 * \brief Render the vessel's active light beacons
	 * \param dev render device
	 * \default None.
	 */
	virtual void RenderBeacons (VulkanDevice *dev) {}

	 /**
     * \brief Render the vessel's grapple points when switched on (see oapiGetShowGrapplePoints)
     * \param dev render device
     * \default None.
     */
    virtual void RenderGrapplePoints (VulkanDevice *dev) {}

	/**
	 * \brief Render the objects coordinate axes
	 * \param dev render device
	 * \param pSkp The 2-D drawing context
	 */
	virtual void RenderVectors (VulkanDevice *dev, VulkanPad* pSkp);


	void RenderDot(VulkanDevice *dev);



	bool bStencilShadow;	// Use Stencil shadow for this object
	bool bOmit;				// Omit this object from scene rendering
	D9BBox			BBox;

protected:

	void RenderSpot(VulkanDevice *dev, const VECTOR3 *ofs, float size, const VECTOR3 &col, bool lighting, int shape);
	void RenderAxisVector(VulkanPad *pSkp, const FVECTOR4 *pColor, VECTOR3 vector, float lscale, float size, bool bLog=false);
	void RenderAxisLabel(VulkanPad *pSkp, const FVECTOR4 *clr, VECTOR3 vector, float lscale, float size, const char *label, bool bLog=false);


	static oapi::VulkanClient *gc;	// graphics client instance pointer
	static SurfNative * blobtex[3];	// beacon textures
	static VulkanMesh * hStockMesh[16];

	VulkanSun		sunLight;	// Local copy of sun light. (Can be freely edited)
	bool			bBSRecompute;

	bool active;		// visual is active (within camera range)
	int objtp;
	Scene *scn;			// The scene to which the object belongs
	VECTOR3	axis;		// Rotation Axis, i.e. _V(0,1,0) in global frame
	VECTOR3 cpos;		// camera-relative object position
	VECTOR3 sundir;		// Sun direction (unit vector)
	VECTOR3 albedo;
	VECTOR3 gpos;		// Global position
	MATRIX3 grot;		// Global rotation
	MATRIX4 dmWorld;    // world matrix in double precision
	FMATRIX4 mWorld;	// world matrix in single precision
	double size;        // object radius [m]
	double cdist;		// current camera distance
	double sunapprad;	// Apparent size of the sun
	double sundst;		// Distance to the sun [m]
	double ctgtdst;		// Distance form a camera target 
	double lng, lat;	// Surface (lng, lat) in some special cases only (e.g. vBuilding)
	OBJHANDLE hPlanet;	// Planet handle in some special cases (e.g. vBase, vBuilding)
	char name[64];
};

#endif // !__VOBJECT_H
