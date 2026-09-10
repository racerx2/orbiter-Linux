// ==============================================================
// TileMgr.h
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2006-2026 Martin Schweiger
// ==============================================================

// ==============================================================
// class TileManager (interface)
//
// Planetary surface rendering management, including a simple
// LOD (level-of-detail) algorithm for surface patch resolution.
// ==============================================================
//
// CONVERTED FROM OVP/D3D9Client/TileMgr.h, read end to end (281 lines).
//
// This is the LEGACY (v1) tile manager -- the one CSphereManager still uses
// for the celestial-sphere background, and the one TileManager2 replaced for
// planet surfaces. It is a declaration file and the structure survives
// whole; the changes are all type substitutions already fixed elsewhere in
// this port:
//
//   LPDIRECT3DTEXTURE9 -> VulkanTexture*
//   LPDIRECT3DDEVICE9  -> VulkanDevice*
//   D3DXMATRIX         -> FMATRIX4          (the same 16 floats)
//   LPD3DXMATRIX       -> FMATRIX4*
//   D3DCOLOR           -> DWORD             (it always was one: 0xAARRGGBB)
//   D3DCOLORVALUE      -> COLOUR4           (field for field; see VulkanUtil.h)
//   oapi::D3D9Client   -> oapi::VulkanClient
//   D3D9Effect         -> VulkanEffect
//   D3D9Config         -> VulkanConfig
//
// TWO THINGS DELIBERATELY DO NOT CHANGE:
//
//   ReadDDSSurface still returns HRESULT. HR() now checks a VkResult, but
//   this function's HRESULT is its OWN return value -- S_OK or E_FAIL from
//   its own file reading, not a device's -- and the shim supplies those
//   names. Changing it would be renaming, not converting.
//
//   The loader threading. HANDLE, CreateThread, the queue mutex and
//   `DWORD WINAPI LoadTile_ThreadProc(void*)` are Win32, not Direct3D, and
//   the shim implements all of them over pthreads (windows.h:1220-1226,
//   with WINAPI defined empty). See the same note in Tilemgr2.h.
//
// `static DWORD vbMemCaps` is left in place and left a DWORD. On Windows it
// held D3DUSAGE/D3DPOOL bits chosen from the device caps; what it becomes in
// Vulkan is a question for TileMgr.cpp, where it is read, not for the
// declaration.
// ==============================================================

#ifndef __TILEMGR_H
#define __TILEMGR_H

#include "VulkanEffect.h"
#include "VulkanUtil.h"
#include "Mesh.h"
#include "Spherepatch.h"

#define MAXQUEUE 10

#pragma pack(push,1)
	struct TILEFILESPEC {
		LONG_PTR sidx;    // index for surface texture (-1: not present)
		LONG_PTR midx;    // index for land-water mask texture (-1: not present)
		DWORD eidx;       // index for elevation data blocks (not used yet; always -1)
		DWORD flags;      // tile flags: bit 0: has diffuse component; bit 1: has specular component; bit 2: has city lights
		DWORD subidx[4];  // subtile indices
	};

struct LMASKFILEHEADER { // file header for contents file at level 1-8
	char id[8];          //    ID+version string
	DWORD hsize;         //    header size
	DWORD flag;          //    bitflag content information
	DWORD npatch;        //    number of patches
	BYTE minres;         //    min. resolution level
	BYTE maxres;         //    max. resolution level
};
#pragma pack(pop)

struct TILEDESC {
	VulkanTexture *tex;            // diffuse surface texture
	VulkanTexture *ltex;           // landmask texture, if applicable
	DWORD flag;
	struct TILEDESC *subtile[4];   // sub-tiles for the next resolution level
	DWORD ofs;                     // refers back to the master list entry for the tile
};

typedef struct {
	float tumin, tumax;
	float tvmin, tvmax;
} TEXCRDRANGE;

class VulkanConfig;
class vPlanet;
// FORWARD DECLARATION ADDED. TileManager below declares `friend class
// TileBuffer;` and then a `static TileBuffer *tilebuf;` member. A friend
// declaration introduces the name into the enclosing namespace but does NOT
// make it findable by ordinary name lookup until a real declaration appears
// -- [namespace.memdef]/3. MSVC finds it anyway; GCC follows the standard and
// reports "'TileBuffer' does not name a type". The class is defined at the
// bottom of this same header, so this only moves its declaration earlier.
class TileBuffer;


class TileManager : public VulkanEffect {

	friend class TileBuffer;
	friend class CSphereManager;

public:
	/**
	 * \brief Constructs a new tile manager object
	 * \param gclient client instance pointer
	 * \param vPlanet planet instance pointer
	 */
	TileManager (oapi::VulkanClient *gclient, const vPlanet *vplanet);

	/**
	 * \brief Destroys the tile manager object
	 */
	virtual ~TileManager ();

	/**
	 * \brief Set up global parameters shared by all instances
	 * \param gclient client instance pointer
	 */
	static void GlobalInit(oapi::VulkanClient *gclient);

	/**
	 * \brief Release global parameters
	 */
	static void GlobalExit();

	// One-time global initialisation/exit methods

	inline int GetMaxLevel () const { return maxlvl; }

	virtual void SetMicrotexture (const char *fname);
	virtual void SetMicrolevel (double lvl);

	// THE PRAGMA IS FOR CloudManager, not for this declaration.
	//
	// CloudManager::Render takes FIVE parameters -- it has no bfog -- so it
	// HIDES this one rather than overriding it, and GCC reports the hiding
	// against THIS line (-Woverloaded-virtual), which is why the suppression
	// has to live here rather than at the derived declaration. MSVC has the
	// same warning (C4266) and leaves it off by default, which is why the
	// Windows build is silent about it.
	//
	// The hiding is deliberate and pre-existing: CloudMgr.cpp calls this
	// version explicitly as TileManager::Render(...), and nothing calls the
	// six-argument form through a CloudManager. "Fixing" it -- giving
	// CloudManager the same signature, or a `using` declaration -- would
	// change which function an existing call resolves to. That is a
	// behaviour change, not a conversion.
	//
	// SurfaceManager::Render has the identical six-parameter signature and
	// overrides normally, so nothing is being hidden from it.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverloaded-virtual"
	virtual void Render(VulkanDevice *dev, FMATRIX4 &wmat, double scale, int level, double viewap = 0.0, bool bfog = false);
#pragma GCC diagnostic pop

	void SetAmbientColor(DWORD cAmbient);

	/// \brief D3DRS_CULLMODE for the next Render(), as a
	///        ShaderClass::CullMode. Defaults to CULL_PASS -- the
	///        technique's own, which is what every caller but one gets.
	///
	///        NEW, AND IT CARRIES A VALUE THAT USED TO TRAVEL AS DEVICE
	///        STATE. vPlanet::RenderCloudLayer sets D3DRS_CULLMODE before
	///        calling the cloud manager -- D3DCULL_NONE for the layer seen
	///        from below and D3DCULL_CCW for the layer seen from above --
	///        and restores it afterwards. Cull is baked into the pipeline
	///        here, so the value has to reach the BeginPass that builds
	///        it; CloudManager applies it through a PassOverride. The
	///        surface manager leaves it alone and keeps the .tech's own.
	///        Same reasoning, and the same field, as
	///        TileManager2Base::cullMode for the v2 engine.
	int cullMode;

protected:

	void ProcessTile (int lvl, int hemisp, int ilat, int nlat, int ilng, int nlng, TILEDESC *tile,
	const TEXCRDRANGE &range, VulkanTexture *tex, VulkanTexture *ltex, DWORD flag,
	const TEXCRDRANGE &bkp_range, VulkanTexture *bkp_tex, VulkanTexture *bkp_ltex, DWORD bkp_flag);

	virtual void InitRenderTile() = 0;
	virtual void EndRenderTile() = 0;
	virtual void RenderSimple(int level, int npatch, TILEDESC *tile, FMATRIX4 *mWorld) = 0;

	virtual void RenderTile(int lvl, int hemisp, int ilat, int nlat, int ilng, int nlng, double sdist, TILEDESC *tile,
		const TEXCRDRANGE &range, VulkanTexture *tex, VulkanTexture *ltex, DWORD flag) = 0;

	bool LoadPatchData ();
	// load binary definition file for LOD levels 1-8

	bool LoadTileData ();
	// load binary definition file for LOD levels > 8

	bool AddSubtileData (TILEDESC &td, TILEFILESPEC *tfs, DWORD idx, DWORD sub, DWORD lvl);
	// add a high-resolution subtile specification to the tree

	void LoadTextures (char *modstr = 0);
	// load patch textures for all LOD levels

	void PreloadTileTextures (TILEDESC *tile8, DWORD ntex, DWORD nmask);
	// Pre-load high-resolution tile textures for the planet (level >= 9)

	void AddSubtileTextures (TILEDESC *td, VulkanTexture **tbuf, DWORD nt, VulkanTexture **mbuf, DWORD nm);
	// add a high-resolution subtile texture to the tree

	void LoadSpecularMasks ();
	// load specular and night light textures

	VECTOR3 TileCentre (int hemisp, int ilat, int nlat, int ilng, int nlng);
	// direction to tile centre from planet centre in planet frame

	void TileExtents (int hemisp, int ilat, int nlat, int ilg, int nlng, double &lat1, double &lat2, double &lng1, double &lng2) const;

	int IsTileInView(int lvl, int ilat, float scale);
	// checks if a given tile is observable from camera position

	void SetWorldMatrix (int ilng, int nlng, int ilat, int nlat);
	// set the world transformation for a particular tile

	bool SpecularColour (COLOUR4 *col);
	// adjust specular reflection through atmosphere


	FMATRIX4 mWorld;
	DWORD cAmbient;                  // was D3DCOLOR
	const vPlanet *vp;               // the planet visual
	OBJHANDLE obj;                   // the planet object
	char *objname;                   // the name of the planet (for identifying texture files)
	DWORD tilever;                   // file version for tile textures
	int maxlvl;                      // max LOD level
	int maxbaselvl;                  // max LOD level, capped at 8
	DWORD ntex;                      // total number of loaded textures for levels <= 8
	DWORD nhitex;                    // number of textures for levels > 8
	DWORD nhispec;                   // number of specular reflection masks (level > 8)
	double hipatchrad;               // angular aperture fraction at which to downgrade patch resolution
	double lightfac;                 // city light intensity factor
	double microlvl;                 // intensity of microtexture
	DWORD nmask;                     // number of specular reflection masks/light maps (level <= 8)
	VECTOR3 pcdir;                   // previous camera direction
	static FMATRIX4 Rsouth;          // rotation matrix for mapping tiles to southern hemisphere
	float spec_base;                 // base intensity for specular reflections
	const ATMCONST *atmc;            // atmospheric parameters (used for specular colour modification)
	bool bPreloadTile;               // pre-load surface tile textures
	bool bNoTextures;				 // Textures failed to load
	TILEDESC *tiledesc;              // tile descriptors for levels 1-8
	static TileBuffer *tilebuf;      // subtile manager

	VulkanTexture **texbuf;			// texture buffer for surface textures (level <= 8)
	VulkanTexture **specbuf;		// texture buffer for specular masks (level <= 8);
	SURFHANDLE microtex;			// microtexture overlay

	// object-independent configuration data
	static bool bGlobalSpecular;     // user wants specular reflections
	static bool bGlobalRipple;       // user wants specular microtextures
	static bool bGlobalLights;       // user wants planet city lights

	// tile patch templates
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
	static int patchidx[9];          // texture offsets for different LOD levels
	static int NLAT[9];
	static int NLNG5[1], NLNG6[2], NLNG7[4], NLNG8[8], *NLNG[9];
	static DWORD vpX0, vpX1, vpY0, vpY1; // viewport boundaries

	static DWORD vbMemCaps;          // video/system memory flag for vertex buffers

	struct RENDERPARAM {
		VulkanDevice *dev;           // render device
		FMATRIX4 wmat;               // world matrix
		FMATRIX4 wmat_tmp;           // copy of world matrix used as work buffer
		int tgtlvl;                  // target resolution level
		MATRIX3 grot;                // planet rotation matrix
		VECTOR3 cpos;                // planet offset vector (in global frame)
		VECTOR3 sdir;                // sun direction from planet centre (in planet frame)
		VECTOR3 cdir;                // camera direction from planet centre (in planet frame)
		double cdist;                // camera distance from planet centre (in units of planet radii)
		double viewap;               // aperture of surface cap visible from camera pos
		double objsize;              // planet radius
		double horzdist;			 // distance to the horizon in meters
		bool bfog;                   // distance fog flag
		bool bCockpit;
	} RenderParam;
};


// =======================================================================
// Class TileBuffer: Global resource; holds a collection of
// tile specifications across all planets

class TileBuffer {
public:
	explicit TileBuffer (const oapi::VulkanClient *gclient);
	~TileBuffer ();
	TILEDESC *AddTile ();
	void DeleteSubTiles (TILEDESC *tile);

	friend void ClearVertexBuffers (TILEDESC *td);
	// Recursively remove subrange vertex buffers from a tile tree with
	// root td. This is necessary when a new tile has been loaded, because
	// this can change the subrange extents for child tiles.

	bool LoadTileAsync (const char *name, TILEDESC *tile);
	// load the textures for a tile for planet 'name', given by descriptor
	// 'tile', using a separate thread.
	// Returns false if request can't be entered (queue full, or request
	// already present)

	static bool ShutDown();
	static void HoldThread(bool bHold);

	static HANDLE hQueueMutex; // Tile loading queue access mutex

private:
	static HANDLE hLoadThread; // LoadTile ThreadProc handle
	static HANDLE hStopThread; // Thread kill signal handle

	static void TerminateLoadThread(); // Terminates the LoadTile thread

	bool DeleteTile (TILEDESC *tile);

	// Still an HRESULT: this is the function's OWN status, not a device's.
	// See the file header.
	static HRESULT ReadDDSSurface (VulkanDevice *pDev, const char *fname, LONG_PTR ofs, VulkanTexture **pTex, bool bManaged);
	static DWORD WINAPI LoadTile_ThreadProc (void*);
	// the thread function loading tile textures on demand

	const oapi::VulkanClient *gc;    // the client
	bool bLoadMip;  // load mipmaps for tiles if available

	static bool bHoldThread;
	static int nqueue, queue_in, queue_out;
	DWORD nbuf;     // buffer size;
	DWORD nused;    // number of active entries
	DWORD last;     // index of last activated entry
	TILEDESC **buf; // tile buffer

	static struct QUEUEDESC {
		const char *name;
		TILEDESC *td;
	} loadqueue[MAXQUEUE];
};

#endif // !__TILEMGR_H
