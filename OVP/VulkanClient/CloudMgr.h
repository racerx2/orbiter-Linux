// ==============================================================
// CloudMgr.h
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2006-2026 Martin Schweiger
// ==============================================================
//
// CONVERTED FROM OVP/D3D9Client/CloudMgr.h, read end to end (39 lines).
//
// TileManager's other concrete subclass, and the same mechanical set of type
// changes as SurfMgr.h: oapi::D3D9Client -> oapi::VulkanClient,
// LPDIRECT3DDEVICE9 -> VulkanDevice*, D3DXMATRIX -> FMATRIX4,
// LPD3DXMATRIX -> FMATRIX4*, LPDIRECT3DTEXTURE9 -> VulkanTexture*. The
// signatures have to match TileMgr.h's pure virtuals exactly.
// ==============================================================

#ifndef __CLOUDMGR_H
#define __CLOUDMGR_H

#include "TileMgr.h"

/**
 * \brief Planetary rendering management for clouds.
 *
 * Planetary rendering management for cloud layers, including a simple
 * LOD (level-of-detail) algorithm for patch resolution.
 */
class CloudManager: public TileManager {
public:
	CloudManager (oapi::VulkanClient *gclient, const vPlanet *vplanet);

	// FIVE parameters, not TileManager::Render's six -- there is no bfog for
	// a cloud layer -- so this HIDES the base version rather than overriding
	// it, deliberately and as it always did. CloudMgr.cpp calls the base
	// explicitly as TileManager::Render(...). GCC reports the hiding, but
	// against the BASE declaration, so the suppression and the full note live
	// in TileMgr.h beside it.
	void Render(VulkanDevice *dev, FMATRIX4 &wmat, double scale, int level, double viewap = 0.0);
	void RenderShadow(VulkanDevice *dev, FMATRIX4 &wmat, double scale, int level, double viewap, float shadowalpha);
	void LoadData();

protected:

	void InitRenderTile();
	void EndRenderTile();
	void RenderSimple(int level, int npatch, TILEDESC *tile, FMATRIX4 *mWorld);
	void RenderTile(int lvl, int hemisp, int ilat, int nlat, int ilng, int nlng, double sdist,
		TILEDESC *tile, const TEXCRDRANGE &range, VulkanTexture *tex, VulkanTexture *ltex, DWORD flag);

//private:
//	int cloudtexidx;
};

#endif // !__CLOUDMGR_H
