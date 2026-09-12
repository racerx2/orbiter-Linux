// ==============================================================
// RingMgr.h
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2007-2026 Martin Schweiger
// ==============================================================

#ifndef __RINGMGR_H
#define __RINGMGR_H

#include "VulkanClient.h"
#include "VPlanet.h"
#include "Mesh.h"

#define MAXRINGRES 3

// ==============================================================
// class RingManager (interface)
// ==============================================================
/**
 * \brief Rendering of planet rings at different resolutions
 */
class RingManager {
public:
	/**
	 * \brief Constructs a new ring manager object
	 * \param vPlanet planet instance pointer
	 * \param inner_rad inner radius of the ring (>=1) [unit planet radius]
	 * \param outer_rad outer radius of the ring (>inner_rad) [unit planet radius]
	 */
	RingManager (const vPlanet *vplanet, double inner_rad, double outer_rad);

	/**
	 * \brief Destroys the ring manager object
	 */
	~RingManager ();

	/**
	 * \brief Set up global parameters shared by all instances
	 * \param gclient client instance pointer
	 */
	static void GlobalInit (oapi::VulkanClient *gclient);

	void SetMeshRes (DWORD res);

	inline double InnerRad() const { return irad; }
	inline double OuterRad() const { return orad; }

	bool Render (VulkanDevice *dev, FMATRIX4 &mWorld, bool zenable);

protected:
	VulkanMesh *CreateRing (double irad, double orad, int nsect);
	DWORD LoadTextures ();

private:
	static oapi::VulkanClient *gc;
	const vPlanet *vp;
	VulkanMesh *mesh[MAXRINGRES];
	VulkanTexture *tex[MAXRINGRES];
	VulkanTexture *pTex;
	DWORD rres, tres, ntex;
	double irad, orad;
};

#endif // !__RINGMGR_H
