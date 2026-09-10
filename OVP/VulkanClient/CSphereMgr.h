// =======================================================================
// CSphereMgr: Rendering of the celestial sphere background at variable
// resolutions.
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2006-2026 Martin Schweiger
//				 2011-2016 Jarmo Nikkanen (D3D9Client modification) 
// =======================================================================
//
// CONVERTED FROM OVP/D3D9Client/CSphereMgr.h, read end to end (156 lines).
//
// The background image manager. It reuses the LEGACY tile machinery --
// TILEDESC, TEXCRDRANGE, VBMESH and TileBuffer all come from TileMgr.h --
// and everything structural about it survives. The substitutions are the
// ones already fixed for TileMgr.h:
//
//   LPDIRECT3DTEXTURE9 -> VulkanTexture*
//   LPDIRECT3DDEVICE9  -> VulkanDevice*
//   D3DXMATRIX         -> FMATRIX4
//   oapi::D3D9Client   -> oapi::VulkanClient
//   D3D9Config         -> VulkanConfig
//
// CreateDeviceObjects LOSES ITS FIRST PARAMETER. It was
// `(LPDIRECT3D9 d3d, LPDIRECT3DDEVICE9 dev)`: the D3D9 OBJECT and the
// device. There is no counterpart to LPDIRECT3D9 -- it was the factory that
// enumerated adapters and created the device, and in this port the core owns
// the VkInstance and hands the client a finished device (see
// VulkanClient.h's note 4 on g_pD3DObject). Dropping a parameter that names
// a thing that no longer exists is the conversion; keeping a NULL placeholder
// would not be.
//
// `float4x4` in CelDataStruct is VulkanUtil.h's alias for FMATRIX4, which is
// what it aliased on Windows too -- it is the HLSL spelling, used here
// because these two structs are the CPU-side mirror of a shader constant
// buffer. #pragma pack(push,4) stays for the same reason: the GLSL block
// that mirrors it must agree, and `layout(scalar)` is what makes it do so.
// =======================================================================

#ifndef __CSPHEREMGR_H
#define __CSPHEREMGR_H

#define STRICT 1
#include "TileMgr.h"

class VulkanConfig;

// =======================================================================
// Class CSphereManager

class CSphereManager
{
public:

// The pack is DELIBERATE -- these two structs are the CPU-side mirror of a
// shader constant buffer and their layout has to be the shader's, not the C++
// ABI's -- and the members are FMATRIX4P rather than FMATRIX4 (float4x4)
// because pack(4) lowers a member's alignment WITHOUT changing the type: an
// ORB_ALIGN16 FMATRIX4 here would still be assumed 16-byte aligned by the
// compiler and its constructor coalesced into aligned SSE stores, which
// SEGFAULTS the moment the struct lands on a 4-aligned address. See the note
// at FVECTOR4P in VulkanUtil.h -- that crash is what this avoids.
//
// -Wpacked-not-aligned therefore stays ON: with no over-aligned member left
// there is nothing to report, and if one is put back the compiler will say so.
#pragma pack(push, 4)
	struct CelDataStruct
	{
		FMATRIX4P mWorld;
		FMATRIX4P mViewProj;
		float	 fAlpha;
		float	 fBeta;
	} CelData;

	struct CelDataFlow
	{
		BOOL	 bAlpha;
		BOOL	 bBeta;
	} CelFlow;
#pragma pack(pop)

	// Measured, not assumed: two 64-byte matrices then two floats, packed to
	// 4. The GLSL block mirroring CelDataStruct must be layout(scalar) and
	// must agree with these offsets.
	static_assert(sizeof(CelDataStruct) == 136, "CelDataStruct must be 136 bytes under pack(4)");
	static_assert(offsetof(CelDataStruct, mViewProj) == 64, "CelDataStruct::mViewProj at 64");
	static_assert(offsetof(CelDataStruct, fAlpha) == 128, "CelDataStruct::fAlpha at 128");
	static_assert(sizeof(CelDataFlow) == 8, "CelDataFlow must be two BOOLs");

	/**
	 * \brief Constructs a new sphere manager object
	 * \param gclient client instance pointer
	 * \param scene scene to which the visual is added
	 */
	CSphereManager (oapi::VulkanClient *gclient, const Scene *scene);

	/**
	 * \brief Destroys the sphere manager object
	 */
	~CSphereManager ();

	/**
	 * \brief Set up global parameters shared by all instances
	 * \param gclient client instance pointer
	 */
	static void GlobalInit (oapi::VulkanClient *gclient);

	/**
	 * \brief Release global parameters
	 */
	static void GlobalExit ();

	// Was CreateDeviceObjects(LPDIRECT3D9 d3d, LPDIRECT3DDEVICE9 dev).
	//
	// BOTH parameters go, and one is replaced. LPDIRECT3D9 was the D3D9
	// factory object, which has no counterpart here (the core owns the
	// VkInstance -- see VulkanClient.h note 4). The device was passed only to
	// call dev->GetViewport(), and a VkDevice answers no such question; the
	// render-target size lives on CVulkanFramework, which is reached through
	// the client. So this takes the client, exactly as GlobalInit does and
	// for exactly the same two lines of work.
	//
	// WORTH KNOWING: this function has NO CALLER anywhere in the Windows
	// tree. It is dead code, converted rather than dropped because deleting
	// it is a decision and this is a conversion.
	static void CreateDeviceObjects(oapi::VulkanClient *gclient);
	static void DestroyDeviceObjects();

	/**
	 * \brief Set the visual brightness of the background image.
	 * \param val brightness value (0-1)
	 */
	void SetBgBrightness(double val);

	void Render (VulkanDevice *dev, int level, double bglvl);

protected:
	bool LoadPatchData ();
	bool LoadTileData ();
	void LoadTextures ();

	void ProcessTile (int lvl, int hemisp, int ilat, int nlat, int ilng, int nlng, TILEDESC *tile,
		const TEXCRDRANGE &range, VulkanTexture *tex, VulkanTexture *ltex, DWORD flag,
		const TEXCRDRANGE &bkp_range, VulkanTexture *bkp_tex, VulkanTexture *bkp_ltex, DWORD bkp_flag);

	void RenderTile (int lvl, int hemisp, int ilat, int nlat, int ilng, int nlng,
		TILEDESC *tile, const TEXCRDRANGE &range, VulkanTexture *tex, VulkanTexture *ltex, DWORD flag);

	void SetWorldMatrix (int ilng, int nlng, int ilat, int nlat);

	VECTOR3 TileCentre (int hemisp, int ilat, int nlat, int ilng, int nlng);
	// returns the direction of the tile centre from the planet centre in local
	// planet coordinates

	void TileExtents (int hemisp, int ilat, int nlat, int ilg, int nlng, double &lat1, double &lat2, double &lng1, double &lng2) const;

	bool TileInView (int lvl, int ilat);
	// Check if specified tile intersects viewport

	static const VulkanConfig *cfg;  // configuration parameters
	const Scene *scn;
	static int *patchidx;            // texture offsets for different LOD levels
	static VBMESH PATCH_TPL_1;
	static VBMESH PATCH_TPL_2;
	static VBMESH PATCH_TPL_3;
	static VBMESH PATCH_TPL_4[2];
	static VBMESH PATCH_TPL_5;
	static VBMESH PATCH_TPL_6[2];
	static VBMESH PATCH_TPL_7[4];
	static VBMESH PATCH_TPL_8[8];
	static VBMESH PATCH_TPL_9[16];
	static VBMESH PATCH_TPL_10[32];
	static VBMESH PATCH_TPL_11[64];
	static VBMESH PATCH_TPL_12[128];
	static VBMESH PATCH_TPL_13[256];
	static VBMESH PATCH_TPL_14[512];
	static VBMESH *PATCH_TPL[15];
	static int **NLNG;
	static int *NLAT;

private:
	HANDLE hTexA, hTexB, hVSConst;
	ShaderClass* pShader;
	VulkanClient* gc;
	char texname[128];
	char starfieldname[128];
	float intensity;                 // opacity of background image
	bool m_bBkgImg;                  ///< background image available?
	bool m_bStarImg;                 ///< starfield image available?
	bool m_bDisabled;                ///< background disabled?
	DWORD maxlvl;                    // max. patch resolution level
	DWORD maxbaselvl;                // max. resolution level, capped at 8
	DWORD ntex;                      // total number of loaded textures for levels <= 8
	DWORD nhitex;                    // number of textures for levels > 8
	DWORD nhispec;                   // number of specular reflection masks (level > 8)
	TILEDESC *tiledesc;              // tile descriptors for levels 1-8
	std::vector<VulkanTexture *> m_texbuf;  // texture buffer for surface textures (level <= 8)
	std::vector<VulkanTexture *> m_starbuf; // texture buffer for starfield textures (level <= 8)
	bool bPreloadTile;               // preload high-resolution tile textures
	MATRIX3 ecl2gal;                 // rotates from ecliptic to galactic frame
	FMATRIX4 trans;                  // transformation from ecliptic to galactic frame
	FMATRIX4 mWorld;

	TileBuffer *tilebuf;
	struct RENDERPARAM {
		VulkanDevice *dev;           // render device
		int tgtlvl;                  // target resolution level
		FMATRIX4 wmat;               // world matrix
		VECTOR3 camdir;              // camera direction in galactic frame
		double viewap;               // viewport aperture (semi-diagonal)
	} RenderParam;

	static DWORD vpX0, vpX1, vpY0, vpY1; // viewport boundaries
	static double diagscale;
};

#endif // !__CSPHEREMGR_H
