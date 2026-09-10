// ==============================================================
// SurfMgr.h
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2007-2026 Martin Schweiger
// ==============================================================
//
// CONVERTED FROM OVP/D3D9Client/SurfMgr.h, read end to end (37 lines).
//
// The v1 surface manager: TileManager's concrete subclass. Six declarations,
// and every change is a type already fixed in TileMgr.h --
// oapi::D3D9Client -> oapi::VulkanClient, LPDIRECT3DDEVICE9 -> VulkanDevice*,
// D3DXMATRIX -> FMATRIX4, LPD3DXMATRIX -> FMATRIX4*, and
// LPDIRECT3DTEXTURE9 -> VulkanTexture*. The signatures must match the pure
// virtuals in TileMgr.h exactly, which is what makes this file mechanical.
// ==============================================================

#ifndef __SURFMGR_H
#define __SURFMGR_H

#include "TileMgr.h"

/**
 * \brief Planetary surface rendering management.
 *
 * Planetary surface rendering management, including a simple
 * LOD (level-of-detail) algorithm for surface patch resolution.
 */
class SurfaceManager: public TileManager {
public:
	SurfaceManager(oapi::VulkanClient *gclient, const vPlanet *vplanet);
	void SetMicrotexture(const char *fname);
	void Render(VulkanDevice *dev, FMATRIX4 &wmat, double scale, int level, double viewap = 0.0, bool bfog = false);
	void LoadData();

protected:

	void InitRenderTile();
	void EndRenderTile();
	void RenderSimple(int level, int npatch, TILEDESC *tile, FMATRIX4 *mWorld);

	void RenderTile(int lvl, int hemisp, int ilat, int nlat, int ilng, int nlng, double sdist,
		TILEDESC *tile, const TEXCRDRANGE &range, VulkanTexture *tex, VulkanTexture *ltex, DWORD flag);

};

#endif // !__SURFMGR_H
