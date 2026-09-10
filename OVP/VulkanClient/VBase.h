// ==============================================================
// VBase.h
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2007-2026 Martin Schweiger
// ==============================================================
//
// CONVERTED FROM OVP/D3D9Client/VBase.h, read end to end (102 lines).
// Types only: LPDIRECT3DDEVICE9 -> VulkanDevice*, D3DXVECTOR3 -> FVECTOR3,
// D3DXMATRIX -> FMATRIX4, D3D9Mesh -> VulkanMesh. The one commented-out
// declaration keeps its comment, with D3D9Light renamed with the rest.
// ==============================================================

#ifndef __VBASE_H
#define __VBASE_H

#include "VObject.h"
#include "Mesh.h"

class RunwayLights;
class TaxiLights;
// vBase says `friend class vPlanet;` and then uses vPlanet in its constructor
// and in GetPlanet(), sixty lines before the elaborated `class vPlanet *vP;`
// declares it. A friend declaration introduces the name into the enclosing
// namespace but does NOT make it findable by ordinary lookup until a real
// declaration appears ([namespace.memdef]/3). MSVC finds it anyway; GCC does
// not. Same as TileMgr.h's TileBuffer.
class vPlanet;


// ==============================================================
// class vBase (interface)
// ==============================================================

/**
 * \brief Visual representation of a surface base.
 *
 * A vBase is the visual representation of a surface base object (a "spaceport"
 * on the surface of a planet or moon, usually with runways or landing pads
 * where vessels can land and take off.
 */
class vBase: public vObject {
	friend class vPlanet;

public:
	vBase (OBJHANDLE _hObj, const Scene *scene, vPlanet *vP=NULL);
	~vBase();

	virtual bool GetMinMaxDistance(float *zmin, float *zmax, float *dmin);
	virtual void UpdateBoundingBox();
	virtual DWORD GetMeshCount();

	bool Update (bool bMainScene);

	double	GetElevation() const;
	vPlanet *GetPlanet() const { return vP; }

	// Convert from a base centric system to time invariant geocentric system 
	// (i.e. vector does not change in time and points to the same fixed surface location) 
	//
	VECTOR3 ToLocal(VECTOR3 pos, double *lng=NULL, double *lat=NULL) const;


	// Convert from time invariant geocentric frame to base centric system (Inverse of the ToLocal)
	//
	VECTOR3 FromLocal(VECTOR3 pos) const;
	void	FromLocal(VECTOR3 pos, FVECTOR3 *pTgt) const;

	void RenderRunwayLights (VulkanDevice *dev);
	bool RenderSurface (VulkanDevice *dev);
	bool RenderStructures (VulkanDevice *dev);
	void RenderGroundShadow (VulkanDevice *dev, float alpha);

	const SurftileSpec *GetTileDesc() const { return tspec; }

private:

	void CreateRunwayLights();
	void CreateTaxiLights();

	/**
	 * \brief Modify local lighting due to planet shadow or
	 *   atmospheric dispersion.
	 * \param light pointer to D3DLIGHT7 structure receiving modified parameters
	 * \param nextcheck time interval until next lighting check [s]
	 * \return \e true if lighting modifications should be applied, \e false
	 *   if global lighting conditions apply.
	 */
	//bool ModLighting (VulkanLight *light, double &nextcheck);

	double Tchk;               // next update
	double Tlghtchk;           // next lighting update
	double csun_lights;
	DWORD ntile;               // number of surface tiles
	const SurftileSpec *tspec; // list of tile specs
	VulkanMesh *tilemesh;
	VulkanMesh **structure_bs;
	VulkanMesh **structure_as;
	DWORD nstructure_bs, nstructure_as;
	bool lights;               // use nighttextures for base objects
	//bool bLocalLight;          // true if lighting is modified
	class vPlanet *vP;
	VECTOR3 vLocalPos;
	MATRIX3 mGlobalRot;
	// Was mGlobalRotDX -- the "DX" distinguished the D3DXMATRIX form from the
	// MATRIX3 above it. Kept as a name because the pairing is what makes the
	// two members readable; the type behind it is an FMATRIX4.
	FMATRIX4 mGlobalRotDX;

	int numRunwayLights;
	RunwayLights** runwayLights;

	int numTaxiLights;
	TaxiLights** taxiLights;
};

#endif // !__VBASE_H
