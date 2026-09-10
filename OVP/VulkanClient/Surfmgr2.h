// ==============================================================
//   ORBITER VISUALISATION PROJECT (OVP)
//   Copyright (C) 2006-2026 Martin Schweiger
//   Dual licensed under GPL v3 and LGPL v3
// ==============================================================

// CONVERTED FROM OVP/D3D9Client/Surfmgr2.h, read end to end (99 lines).
//
// SurfTile: Tile's main subclass, and the elevation engine's data. The
// declarations that change are the four that name a Direct3D type:
//
//   the D3D9 texture pointer  -> VulkanTexture*  (SetOverlay, DeleteOverlay,
//                                                 and the ltex member)
//   the D3DX 2-vector         -> FVECTOR2        (MicroRep[3])
//   the D3D9 Sketchpad        -> VulkanPad       (RenderLabels)
//
// Everything else -- the elevation grid, the edge-matching, the interpolation
// -- is arithmetic on INT16 and float arrays and is untouched.

#ifndef __SURFMGR2_H
#define __SURFMGR2_H

#include "Tilemgr2_imp.hpp"
#include "TileLabel.h"
#include "VulkanPad.h"

// FORWARD DECLARATION ADDED, for a CIRCULAR INCLUDE that exists on both
// platforms: TileLabel.h includes this file and this file includes
// TileLabel.h. Whichever is included first, the other's guard is already set
// when it is reached -- so if TileLabel.h comes first, `TileLabel *label`
// below names a type that has not been declared yet. It happens to work on
// Windows only because every translation unit there reaches Surfmgr2.h
// first. One line makes the order stop mattering.
class TileLabel;

/**
 * \brief Planetary surface rendering engine.
 *
 * Planetary surface rendering engine v2, including a simple
 * LOD (level-of-detail) algorithm for surface patch resolution.
 */
class SurfTile: public Tile {
	friend class TileManager2Base;
	template<class SurfTile> friend class TileManager2;
	friend class TileLabel;

	void MatchEdges ();

public:
	SurfTile (TileManager2Base *_mgr, int _lvl, int _ilat, int _ilng);
	~SurfTile ();

	inline QuadTreeNode<SurfTile> *GetNode() const { return node; }
	inline void SetNode (QuadTreeNode<SurfTile> *_node) { node = _node; }
	// Register the tile to a quad tree node

	int GetElevation(double lng, double lat, double *elev, FVECTOR3 *nrm=NULL, SurfTile **cache=NULL, bool bFilter=true, bool bGet=false) const;

	double GetCameraDistance();
	SurfTile *getTextureOwner();

	VulkanTexture * SetOverlay(VulkanTexture * pOverlay, bool bOwn = true);
	bool DeleteOverlay(VulkanTexture * pOverlay = NULL);

	double GetMinElev() const { return ehdr.emin; }		// virtual from Tile::
	double GetMaxElev() const { return ehdr.emax; }		// virtual from Tile::
	double GetMeanElev() const { return ehdr.emean; }	// virtual from Tile::

protected:
	virtual Tile *getParent() const { return node && node->Parent() ? node->Parent()->Entry() : NULL; }
	inline SurfTile *getSurfParent() const { return node && node->Parent() ? node->Parent()->Entry() : NULL; }

	// Return pointer to parent tile, if exists

	void Load ();
	void PreLoad ();
	INT16 *ReadElevationFile (const char *name, int lvl, int ilat, int ilng);
	bool LoadElevationData ();
	void Render ();
	void StepIn ();
	bool IsElevated() { return (ggelev!=NULL); }

	TileManager2<SurfTile> *smgr;	// surface tile manager interface
	QuadTreeNode<SurfTile> *node;	// my node in the quad tree, if I'm part of a tree

	void FixCorner (const SurfTile *nbr);
	// Match corner elevation to neighbour

	void FixLongitudeBoundary (const SurfTile *nbr, bool keep_corner=false);
	// Match longitude edge elevation to neighbour. If keep_corner==true, skip corner node

	void FixLatitudeBoundary (const SurfTile *nbr, bool keep_corner=false);
	// Match latitude edge elevation to neighbour. If keep_corner==true, skip corner node

	// v2 Labels interface -----------------------------------------------
	void CreateLabels();    ///< create the label object from the label tile file, if available
	void DeleteLabels();    ///< delete the TileLabel object if it exists
	void RenderLabels(VulkanPad *skp, oapi::Font **labelfont, int *fontidx);

private:
	bool InterpolateElevationGrid(const float *pelev, float *elev);
	float Interpolate(FMATRIX4 &in, float x, float y);
	float *ElevationData () const;
	void ComputeElevationData(const float *elev) const;
	float fixinput(double, int);
	FVECTOR4 MicroTexRange(SurfTile *pT, int lvl) const;

	PlanetShader* pShader;
	mutable ELEVFILEHEADER ehdr;///< Let's store the complete header for later use
	FVECTOR2 MicroRep[3];
	DWORD MaxRep;
	VulkanTexture * ltex;	///< landmask/nightlight texture, if applicable
	INT16 *elev_file;			///< elevation data [m]
	float *elev;				///< elevation data [m] (8x subsampled)
	mutable float *ggelev;		///< pointer to my elevation data in the great-grandparent

	TileLabel *label;			///< surface labels associated with this tile
};

#endif // !__SURFMGR2_H

