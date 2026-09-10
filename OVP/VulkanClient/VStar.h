// ==============================================================
// VStar.h
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2006-2026 Martin Schweiger
// ==============================================================
//
// CONVERTED FROM OVP/D3D9Client/VStar.h, read end to end (58 lines).
// Two types: oapi::D3D9Client -> oapi::VulkanClient and
// LPDIRECT3DDEVICE9 -> VulkanDevice*. Nothing else in the file names
// Direct3D.
// ==============================================================

#ifndef __VSTAR_H
#define __VSTAR_H

#include "VObject.h"

//class VulkanMesh;


// ==============================================================
// class vStar (interface)
// ==============================================================

/**
 * \brief Visual representation of the (one) central star.
 *
 * Renders the central star as a billboard mesh.
 */
class vStar: public vObject {
public:
	/**
	 * \brief Constructs a new central star object for a scene
	 * \param _hObj object handle
	 * \param scene scene to which the visual is added
	 */
	vStar (OBJHANDLE _hObj, const Scene *scene);

	/**
	 * \brief Destroys the central star object
	 */
	~vStar ();

	/**
	 * \brief Set up global parameters shared by all instances
	 * \param gclient client instance pointer
	 */
	static void GlobalInit (oapi::VulkanClient *gc);

	/**
	 * \brief Release global parameters
	 */
	static void GlobalExit ();

	bool Update (bool bMainScene);
	bool Render (VulkanDevice *dev);

private:
	double maxdist;                    ///< max render distance
	static SURFHANDLE deftex; ///< default texture
};

#endif // !__VSTAR_H
