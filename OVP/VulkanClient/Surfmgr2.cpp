// ==============================================================
//   ORBITER VISUALISATION PROJECT (OVP)
//   Copyright (C) 2006-2026 Martin Schweiger
//   Dual licensed under GPL v3 and LGPL v3
// ==============================================================

// ==============================================================
// surfmgr2.cpp
// class SurfaceManager2 (implementation)
//
// Planetary surface rendering engine v2, including a simple
// LOD (level-of-detail) algorithm for surface patch resolution.
// ==============================================================
//
// CONVERTED FROM OVP/D3D9Client/Surfmgr2.cpp, read end to end (1883 lines).
//
// The surface tile itself: elevation files, edge matching between tiles of
// different levels, and the one render function that feeds the terrain
// shader. The elevation code -- more than half the file -- is INT16 and float
// array arithmetic and converts line for line. What changed:
//
//  1. TWO SetRenderState CALLS AFTER Setup(), AND THEY ARE NOT THE SAME CASE.
//
//     `SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW)` DOES SOMETHING: it turns
//     back-face culling on for the terrain, so the far side of the planet is
//     not drawn. ShaderClass built every pipeline with VK_CULL_MODE_NONE, so
//     dropping the call would draw it. Cull is immutable pipeline state in
//     Vulkan, so ShaderClass gained `SetCullMode`, which joins its pipeline
//     cache key -- exactly the addition, and exactly the reasoning, that
//     `SetTopology` was.
//
//     `SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, 0/1)` around the tree walk,
//     for Config->NoPlanetAA, HAS NO COUNTERPART AND NEEDS NONE. Multisample
//     state is baked into a VkPipeline (and, more to the point, into the
//     render pass), and every client pipeline is built against the core's
//     render pass, which UIHost.cpp creates with one sample. There is nothing
//     to toggle: planet AA is already off. Core Vulkan has no dynamic
//     equivalent -- VK_EXT_extended_dynamic_state3's rasterizationSamples is
//     an extension this client does not require.
//
//  2. `D3DCAPS9::MaxTextureRepeat` HAS NO COUNTERPART. It was the largest
//     texture-coordinate repeat range a card supported; Vulkan imposes no
//     such limit, because wrapping is done on a float coordinate with no
//     documented ceiling. `MaxRep` is written here and read nowhere in the
//     tree, so nothing observes the difference; it is set to the value that
//     means "no limit" and says so.
//
//  3. THE SIX D3DX TEXTURE LOADS in SeekTileTexture become the shared
//     `LoadDDSFile` (declared in Tilemgr2.h) and
//     `NatCreateTextureFromDDSInMemory`. Same split as everywhere else: read
//     the bytes, then decode. `D3DPOOL_DEFAULT` and the two D3DX filter
//     arguments have no counterpart -- memory placement follows the usage
//     flags and no filtering happens on load.
//
//  4. `overlay->Release()` in DeleteOverlay becomes
//     `VulkanDevice::DestroyTexture`. It is guarded by `ownoverlay`, so the
//     tile really is the owner, and vkDestroyImage takes the VkDevice that
//     made the image.
//
//  5. NINE TILE PATHS were Windows paths built for fopen or FileExists --
//     Surf, Mask, Elev, Elev_mod and Label, in five functions. Same class as
//     finding 24: a backslash is a legal Linux filename character, so each
//     asks for one literally-named file, misses, and reports the same "no
//     data" an absent layer gives. The symptom would be a planet with no
//     terrain, no night lights and no elevation at all.
//
//  6. `(mask - ilat & mask)` IS `((mask - ilat) & mask)`, twice, in
//     ElevationData. The parentheses are added rather than the warning
//     suppressed, because that precedence is exactly the thing worth being
//     explicit about; the value is unchanged.
//
// The types are the usual list: D3D9Client -> VulkanClient, D3D9Pad ->
// VulkanPad, D3D9Light -> VulkanLight, LPDIRECT3DDEVICE9 -> VulkanDevice*,
// LPDIRECT3DTEXTURE9 -> VulkanTexture*, D3DXVECTOR2/3 -> FVECTOR2/3, and
// D3DXVec3TransformCoord / D3DXVec3Dot -> oapi::TransformCoord / oapi::dot.
// ==============================================================

#include "Surfmgr2.h"
#include "Tilemgr2.h"
#include "Cloudmgr2.h"
#include "VulkanCatalog.h"
#include "VulkanConfig.h"
#include "VVessel.h"
#include "VectorHelpers.h"
#include "DebugControls.h"
#include "gcCore.h"
#include "VulkanSurface.h"

// =======================================================================
extern void FilterElevationGraphics(OBJHANDLE hPlanet, int lvl, int ilat, int ilng, float *elev);

// The GLSL block that reads this must be layout(scalar), which is what the
// pack(4) mirrors. `param` is FVECTOR4P rather than float4 (FVECTOR4) for the
// reason spelled out at FVECTOR4P in VulkanUtil.h: pack(4) lowers a member's
// alignment but not its TYPE's, so an ORB_ALIGN16 FVECTOR4 at offset 144 --
// 0 mod 16 here, but 4 mod 16 in the second element of any array of these --
// gets aligned SSE stores from its constructor and faults. FVECTOR3 is not
// over-aligned, so the other four members are unaffected.
#pragma pack(push, 4)
struct LightF
{
	float3     position[4];         /* position in world space */
	float3     direction[4];        /* direction in world space */
	float3     diffuse[4];          /* diffuse color of light */
	float3     attenuation[4];      /* Attenuation */
	FVECTOR4P  param[4];            /* range, falloff, theta, phi */
};
#pragma pack(pop)


// =======================================================================
// Utility functions

static void VtxInterpolate (VERTEX_2TEX &res, const VERTEX_2TEX &a, const VERTEX_2TEX &b, double w)
{
	float w1 = (float)w;
	float w0 = 1.0f-w1;
	res.x = a.x*w0 + b.x*w1;
	res.y = a.y*w0 + b.y*w1;
	res.z = a.z*w0 + b.z*w1;
	res.nx = a.nx*w0 + b.nx*w1;
	res.ny = a.ny*w0 + b.ny*w1;
	res.nz = a.nz*w0 + b.nz*w1;
}


int compare_lights(const void * a, const void * b);


// =======================================================================
// =======================================================================

SurfTile::SurfTile (TileManager2Base *_mgr, int _lvl, int _ilat, int _ilng)
: Tile (_mgr, _lvl, _ilat, _ilng)
{
	smgr = static_cast<TileManager2<SurfTile>* > (_mgr);
	node = 0;
	elev = NULL;
	ggelev = NULL;
	elev_file = NULL;
	ltex = NULL;
	has_elevfile = false;
	label = NULL;
	imicrolvl = 14;	// Water resolution level
	// Was GetCaps()->MaxTextureRepeat. See point 2 in the file header: there
	// is no such limit in Vulkan, and nothing reads this.
	MaxRep = 0xFFFFFFFF;
	if (Config->TileMipmaps == 1) bMipmaps = true;

	memset(&ehdr, 0, sizeof(ELEVFILEHEADER));
}

// -----------------------------------------------------------------------

SurfTile::~SurfTile ()
{
	smgr->GetPlanet()->TileDeleted(this);

	if (elev) g_pMemgr_f->Free(elev);
	if (elev_file) g_pMemgr_i->Free(elev_file);	
	if (tex && owntex) g_pTexmgr_tt->Free(tex);
	if (ltex && owntex) g_pTexmgr_tt->Free(ltex);
		
	DeleteLabels();
}

// -----------------------------------------------------------------------

void SurfTile::PreLoad()
{
	// `char fname[128]` stood beside path and nothing writes or reads it.
	char path[MAX_PATH];
	
	owntex = true;

	assert(tex == nullptr);
	assert(ltex == nullptr);

	VulkanDevice  *pDev = mgr->Dev();
	VulkanTexture *pSysSrf = nullptr;
	
	// Configure microtexture range for "Water texture" and "Cloud microtexture".
	GetParentMicroTexRange(&microrange);
	GetParentOverlayRange(&overlayrange);

	// Load surface texture

	if (smgr->DoLoadIndividualFiles(0)) { // try loading from individual tile file
		// "%s\\Surf\\..." on Windows. See point 5 in the file header.
		sprintf_s(path, MAX_PATH, "%s/Surf/%02d/%06d/%06d.dds", mgr->DataRootDir().c_str(), lvl + 4, ilat, ilng);
		LoadTextureFile(path, &pSysSrf);
	}
	if (!pSysSrf && smgr->ZTreeManager(0)) { // try loading from compressed archive
		BYTE *buf;
		DWORD ndata = smgr->ZTreeManager(0)->ReadData(lvl+4, ilat, ilng, &buf);
		if (ndata) {
			LoadTextureFromMemory(buf, ndata, &pSysSrf);
			smgr->ZTreeManager(0)->ReleaseData(buf);
		}
	}
	
	if (CreateTexture(pDev, pSysSrf, &tex) != true) {
		if (GetParentSubTexRange(&texrange)) {
			tex = getSurfParent()->Tex();
			owntex = false;
		}
	}
	
	// SAFE_RELEASE(pSysSrf) -- the staging copy CreateTexture has just
	// uploaded from. Nothing is reference counted; the device that made the
	// image destroys it.
	if (pSysSrf) { pDev->DestroyTexture(pSysSrf); pSysSrf = nullptr; }


	// Load mask texture

	if (tex && (mgr->Cprm().bSpecular || mgr->Cprm().bLights))
	{
		if (smgr->DoLoadIndividualFiles(1)) { // try loading from individual tile file
			sprintf_s(path, MAX_PATH, "%s/Mask/%02d/%06d/%06d.dds", mgr->DataRootDir().c_str(), lvl + 4, ilat, ilng);
			LoadTextureFile(path, &pSysSrf);
		}
		if (!pSysSrf && smgr->ZTreeManager(1)) { // try loading from compressed archive
			BYTE* buf;
			DWORD ndata = smgr->ZTreeManager(1)->ReadData(lvl + 4, ilat, ilng, &buf);
			if (ndata) {
				LoadTextureFromMemory(buf, ndata, &pSysSrf);
				smgr->ZTreeManager(1)->ReleaseData(buf);
			}
		}
		
		if (owntex) {
			CreateTexture(pDev, pSysSrf, &ltex);			
		}
		else if (node && node->Parent()) {
			ltex = getSurfParent()->ltex;
		}		
		
		if (pSysSrf) { pDev->DestroyTexture(pSysSrf); pSysSrf = nullptr; }
	}
}

// -----------------------------------------------------------------------

void SurfTile::Load ()
{
	// Load elevation data
	float *elev = ElevationData ();

	bool shift_origin = (lvl >= 4);
	int res = mgr->GridRes();

	if (lvl <= 0)
	{
		if (!lvl) { // create hemisphere mesh for western or eastern hemispheres
			mesh = CreateMesh_hemisphere(res, elev, 0.0);
	//  } else {    // create full sphere mesh
	//	  // TODO
		}
	} else {
		// create rectangular patch
		mesh = CreateMesh_quadpatch (res, res, elev, 1.0, 0.0, &texrange, shift_origin, &vtxshift, mgr->GetPlanet()->prm.tilebb_excess);
	}

	CreateLabels();
}

// -----------------------------------------------------------------------

INT16 *SurfTile::ReadElevationFile (const char *name, int lvl, int ilat, int ilng)
{
	const int ndat = TILE_ELEVSTRIDE*TILE_ELEVSTRIDE;
	INT16 *e = NULL;

	// Elevation resolution used for "rounding" due to INT16 elevation. 
	// Technically, should not apply to float based elevation but required due to rounding in physics.
	double tgt_res = mgr->ElevRes();

	char path[MAX_PATH];
	// `char fname[128]` stood here and nothing writes or reads it -- the log
	// lines below use the `name` parameter.
	FILE *f;
	int i;

	// Elevation data
	if (smgr->DoLoadIndividualFiles(2)) { // try loading from individual tile file
		sprintf_s(path, MAX_PATH, "%s/Elev/%02d/%06d/%06d.elv", mgr->DataRootDir().c_str(), lvl, ilat, ilng);
		if (!fopen_s(&f, path, "rb")) {
			e = g_pMemgr_i->New(ndat);
			elev = g_pMemgr_f->New(ndat);
			// read the elevation file header
			fread (&ehdr, sizeof(ELEVFILEHEADER), 1, f);
			if (ehdr.hdrsize != sizeof(ELEVFILEHEADER)) fseek (f, ehdr.hdrsize, SEEK_SET);
			LogClr("Teal", "NewTile[%s]: Lvl=%d, Scale=%g, Offset=%g", name, lvl-4, ehdr.scale, ehdr.offset);

#ifdef ORBITER2016
			ehdr.scale = 1.0;
#endif
			switch (ehdr.dtype) {
			case 0: // flat tile, defined by offset
				for (i = 0; i < ndat; i++) e[i] = 0;
				break;
			case 8: {
				UINT8 *tmp = g_pMemgr_u->New(ndat);
				fread (tmp, sizeof(UINT8), ndat, f);
				for (i = 0; i < ndat; i++)
					e[i] = (INT16)tmp[i];
				g_pMemgr_u->Free(tmp);
				tmp = NULL;
				}
				break;
			case -16:
				fread (e, sizeof(INT16), ndat, f);
				break;
			}
			fclose (f);
		}
	}
	if (!e && smgr->ZTreeManager(2)) { // try loading from compressed archive
		BYTE *buf;
		DWORD ndata = smgr->ZTreeManager(2)->ReadData(lvl, ilat, ilng, &buf);
		if (ndata) {
			BYTE *p = buf;
			e = g_pMemgr_i->New(ndat);
			elev = g_pMemgr_f->New(ndat);
			memcpy(&ehdr, p, sizeof(ELEVFILEHEADER));
			LogClr("Teal", "NewTileA[%s]: Lvl=%d, Scale=%g, Offset=%g", name, lvl-4, ehdr.scale, ehdr.offset);

#ifdef ORBITER2016
			ehdr.scale = 1.0;
#endif
			p += ehdr.hdrsize;
			switch (ehdr.dtype) {
			case 0:
				for (i = 0; i < ndat; i++) e[i] = 0;
				break;
			case 8:
				for (i = 0; i < ndat; i++)
					e[i] = (INT16)(*p++);
				break;
			case -16:
				memcpy(e, p, ndat*sizeof(INT16));
				p += ndat*sizeof(INT16);
				break;
			}
			smgr->ZTreeManager(2)->ReleaseData(buf);
		}
	}

	if (e) {
	
		if (ehdr.scale != tgt_res) { // rescale the data
			double rescale = ehdr.scale / tgt_res;
			for (i = 0; i < ndat; i++)
				e[i] = (INT16)(e[i] * rescale);
		}
		if (ehdr.offset) {
			INT16 sofs = (INT16)(ehdr.offset / tgt_res);
			for (i = 0; i < ndat; i++)
				e[i] += sofs;
		}

		// Convert to float
		for (i = 0; i < ndat; i++) elev[i] = float(e[i]) * float(tgt_res);	
	}

	// Elevation mod data
	if (e) {
		bool ok = false;
		double rescale;
		INT16 offset;
		bool do_rescale, do_shift;
		ELEVFILEHEADER hdr;
		if (smgr->DoLoadIndividualFiles(3)) { // try loading from individual tile file
			sprintf_s (path, MAX_PATH, "%s/Elev_mod/%02d/%06d/%06d.elv", mgr->DataRootDir().c_str(), lvl, ilat, ilng);
			if (!fopen_s(&f, path, "rb")) {
				fread (&hdr, sizeof(ELEVFILEHEADER), 1, f);
				if (hdr.hdrsize != sizeof(ELEVFILEHEADER)) fseek (f, hdr.hdrsize, SEEK_SET);
				LogClr("Teal", "NewElevMod[%s]: Lvl=%d, Scale=%g, Offset=%g", name, lvl - 4, hdr.scale, hdr.offset);

#ifdef ORBITER2016
				hdr.scale = 1.0;
#endif
				rescale = (do_rescale = (hdr.scale != tgt_res)) ? hdr.scale/tgt_res : 1.0;
				offset = (do_shift = (hdr.offset != 0.0)) ? INT16(hdr.offset/tgt_res) : 0;

				switch (hdr.dtype) {
				case 0: // overwrite the entire tile with a flat offset
					for (i = 0; i < ndat; i++) e[i] = offset, elev[i] = float(hdr.offset);
					break;
				case 8: {
					const UINT8 mask = UCHAR_MAX;
					UINT8 *tmp = g_pMemgr_u->New(ndat);
					fread (tmp, sizeof(UINT8), ndat, f);
					for (i = 0; i < ndat; i++) {
						if (tmp[i] != mask) {
							e[i] = (INT16)(do_rescale ? (INT16)(tmp[i] * rescale) : (INT16)tmp[i]);
							if (do_shift) e[i] += offset;
							elev[i] = float(e[i]) * float(tgt_res);
						}
					}
					g_pMemgr_u->Free(tmp);
					tmp = NULL;
					}
					break;
				case -16: {
					const INT16 mask = SHRT_MAX;
					INT16* tmp = g_pMemgr_i->New(ndat);
					fread (tmp, sizeof(INT16), ndat, f);
					for (i = 0; i < ndat; i++) {
						if (tmp[i] != mask) {
							e[i] = (do_rescale ? (INT16)(tmp[i] * rescale) : tmp[i]);
							if (do_shift) e[i] += offset;
							elev[i] = float(e[i]) * float(tgt_res);
						}
					}
					g_pMemgr_i->Free(tmp);
					tmp = NULL;
					}
					break;
				}
				fclose(f);
				ok = true;
			}
		}
		if (!ok && smgr->ZTreeManager(3)) { // try loading from compressed archive
			BYTE *buf;
			DWORD ndata = smgr->ZTreeManager(3)->ReadData(lvl, ilat, ilng, &buf);
			if (ndata) {
				BYTE *p = buf;
				ELEVFILEHEADER *phdr = (ELEVFILEHEADER*)p;
				LogClr("Teal", "NewElevModA[%s]: Lvl=%d, Scale=%g, Offset=%g", name, lvl - 4, phdr->scale, phdr->offset);

#ifdef ORBITER2016
				phdr->scale = 1.0;
#endif
				p += phdr->hdrsize;
				rescale = (do_rescale = (phdr->scale != tgt_res)) ? phdr->scale/tgt_res : 1.0;
				offset = (do_shift = (phdr->offset != 0.0)) ? INT16(phdr->offset/tgt_res) : 0;

				switch(phdr->dtype) {
				case 0:
					for (i = 0; i < ndat; i++) e[i] = offset, elev[i] = float(phdr->offset);
					break;
				case 8: {
					const UINT8 mask = UCHAR_MAX;
					for (i = 0; i < ndat; i++) {
						if (p[i] != mask) {
							e[i] = (INT16)(do_rescale ? p[i] * rescale : p[i]);
							if (do_shift) e[i] += offset;
							elev[i] = float(e[i]) * float(tgt_res);
						}
					}
					} break;
				case -16: {
					const INT16 mask = SHRT_MAX;
					INT16 *buf16 = (INT16*)p;
					for (i = 0; i < ndat; i++) {
						if (buf16[i] != mask) {
							e[i] = (do_rescale ? (INT16)(buf16[i] * rescale) : buf16[i]);
							if (do_shift) e[i] += offset;
							elev[i] = float(e[i]) * float(tgt_res);
						}
					}
					} break;
				}
				smgr->ZTreeManager(3)->ReleaseData(buf);
			}
		}
		if (Config->bFlats) FilterElevationGraphics(mgr->GetPlanet()->Object(), lvl - 4, ilat, ilng, elev);
	}
	return e;
}

// -----------------------------------------------------------------------

VulkanTexture * SurfTile::SetOverlay(VulkanTexture * pOverlay, bool bOwn)
{
	VulkanTexture * pRet = NULL;
	if (bOwn && ownoverlay && overlay) pRet = overlay;

	overlay = pOverlay;
	ownoverlay = bOwn;

	for (int i = 0; i < 4; i++) {
		auto x = node->Child(i);
		if (x) if (x->Entry()) {
			VulkanTexture * pOld = x->Entry()->overlay;
			if (pOld == NULL || pOld == pRet) {
				x->Entry()->GetParentOverlayRange(&overlayrange);
				x->Entry()->SetOverlay(pOverlay, false);
			}
		}
	}
	return pRet;
}

// -----------------------------------------------------------------------

bool SurfTile::DeleteOverlay(VulkanTexture * pOverlay)
{
	bool bReturn = false;
	if (pOverlay == NULL) pOverlay = overlay;

	if (overlay) {
		if (pOverlay == overlay) {
			if (ownoverlay) {
				// overlay->Release(). See point 4 in the file header.
				mgr->Dev()->DestroyTexture(overlay);
				bReturn = true;
			}
			for (int i = 0; i < 4; i++) {
				auto x = node->Child(i);
				if (x) if (x->Entry()) x->Entry()->DeleteOverlay(pOverlay);
			}
			overlay = NULL;
			ownoverlay = false;
		}
	}
	return bReturn;
}


// -----------------------------------------------------------------------

float SurfTile::Interpolate(FMATRIX4 &in, float t, float u)
{
	return 0.0f;
}

// -----------------------------------------------------------------------

bool SurfTile::InterpolateElevationGrid(const float *in, float *out)
{
	int q0 = 0, c = 129;

	if (!(ilat & 1)) q0 = TILE_ELEVSTRIDE * 128;
	if ((ilng & 1)) q0 += 128;

	for (int i = 0; i <= c; i++)
	{
		int q1 = q0 + 1;
		int q2 = q0 + TILE_ELEVSTRIDE;
		int q3 = q1 + TILE_ELEVSTRIDE;
		int x = (TILE_ELEVSTRIDE << 1) * i;

		for (int k = 0; k <= c; k++)
		{
			float f0 = in[k + q0];	float f1 = in[k + q1];
			float f2 = in[k + q2];	float f3 = in[k + q3];

			out[x + 0] = (f0 + f1 + f2 + f3) * 0.25f;
			if (k != c) out[x + 1] = (f1 + f3) * 0.5f;

			if (i != c) {
				out[x + TILE_ELEVSTRIDE] = (f2 + f3) * 0.5f;
				if (k != c) out[x + TILE_ELEVSTRIDE + 1] = f3;
			}
			x += 2;
		}
		q0 += TILE_ELEVSTRIDE;
	}
	return true;
}


// -----------------------------------------------------------------------

bool SurfTile::LoadElevationData ()
{
	// Note: a tile's elevation data are retrieved from its great-grandparent tile. Each tile stores the elevation
	// data for its area at 8x higher resolution than required by itself, so they can be used by the grandchildren.
	// This reduces the number of elevation files, by storing more information in each. It also has a caching effect:
	// Once a grandparent has loaded its elevation data on request of a grandchild, any other grandchildren's requests
	// can be served directly without further disk I/O.

	if (elev) return true; // already present

	has_elevfile = false;
	int mode = mgr->Cprm().elevMode;
	if (!mode) return false;

	DWORD phy_lvl = mgr->GetPlanet()->GetPhysicsPatchRes();
	int ndat = TILE_ELEVSTRIDE*TILE_ELEVSTRIDE;

	elev_file = ReadElevationFile (mgr->CbodyName(), lvl + 4, ilat, ilng);
	double tgt_res = mgr->ElevRes();

	// phy_lvl is fetched and never read, on both platforms.
	(void)phy_lvl;

	if (elev_file) has_elevfile = true;
	else if (lvl > 0) {

		// Acquire elev header data from a parent
		QuadTreeNode<SurfTile> *parent = node->Parent();
		if (parent &&  parent->Entry()) memcpy(&ehdr, &parent->Entry()->ehdr, sizeof(ELEVFILEHEADER));

		// construct elevation grid by interpolating ancestor data
		ELEVHANDLE hElev = mgr->ElevMgr();
		if (!hElev) return false;

		// Cubic Interpolation
		if (mode == 2) {

			int plvl = lvl-1;
			int pilat = ilat >> 1;
			int pilng = ilng >> 1;
			INT16 *pelev_file = 0;
			QuadTreeNode<SurfTile> *parent = node->Parent();
			for (; plvl >= 0; plvl--) { // find ancestor with elevation data
				if (parent && parent->Entry()->has_elevfile) {
					pelev_file = parent->Entry()->elev_file;
					break;
				}
				if (parent) parent = parent->Parent();
				pilat >>= 1;
				pilng >>= 1;
			}

			if (!pelev_file) return false;

			elev = g_pMemgr_f->New(ndat);

			// submit ancestor data to elevation manager for interpolation
			mgr->GetClient()->ElevationGrid(hElev, ilat, ilng, lvl, pilat, pilng, plvl, pelev_file, elev);

			// Convert to float
			for (int i = 0; i < ndat; i++) elev[i] *= float(tgt_res);
		}

		// Experimental Linear Interpolation
		else {
			QuadTreeNode<SurfTile> *parent = node->Parent();
			if (parent &&  parent->Entry()->elev) {
				elev = g_pMemgr_f->New(ndat);
				InterpolateElevationGrid(parent->Entry()->elev, elev);
			}
		}
	}

	if (has_elevfile) LogClr("Teal", "TileCreatedFromFile: Level=%d, ilat=%d, ilng=%d", lvl, ilat, ilng);
	else LogClr("Teal", "TileInterpolatedFromParent: Level=%d, ilat=%d, ilng=%d", lvl, ilat, ilng);

	return (elev != 0);
}

// -----------------------------------------------------------------------

float *SurfTile::ElevationData () const
{
	if (!ggelev) {
		int ancestor_dlvl = 3;
		while (1 << (8-ancestor_dlvl) < mgr->GridRes()) ancestor_dlvl--;
		if (lvl >= ancestor_dlvl) { // traverse quadtree back to great-grandparent
			QuadTreeNode<SurfTile> *ancestor = node; // start at my own node
			int blockRes = TILE_FILERES;
			while (blockRes > mgr->GridRes() && ancestor) {
				ancestor = ancestor->Parent();
				blockRes >>= 1;
			}
			if (ancestor && ancestor->Entry()->LoadElevationData()) {
				// compute pixel offset into great-grandparent tile set
				int nblock = TILE_FILERES/blockRes;
				int mask = nblock-1;
				// `mask - ilat & mask` -- '-' binds tighter than '&', so this
				// is (mask - ilat) & mask. See point 6 in the file header.
				int ofs = (((mask - ilat) & mask) * TILE_ELEVSTRIDE + (ilng & mask)) * blockRes;
				ggelev = ancestor->Entry()->elev + ofs;
			}
		} else {
			SurfTile *ggp = smgr->GlobalTile(lvl - ancestor_dlvl);// +3);
			if (ggp && ggp->LoadElevationData ()) {
				int blockRes = mgr->GridRes();
				int nblock = TILE_FILERES/blockRes;
				int mask = nblock-1;
				int ofs = (((mask - ilat) & mask) * TILE_ELEVSTRIDE + (ilng & mask)) * blockRes;
				ggelev = ggp->elev + ofs;
			}
		}
		if (ggelev) ComputeElevationData(ggelev);
	}
	return ggelev;
}

// -----------------------------------------------------------------------

void SurfTile::ComputeElevationData(const float *elev) const
{
	if (has_elevfile) return;
	int i, j;
	int res = mgr->GridRes();
	ehdr.emax = -1e30;
	ehdr.emin = 1e30;
	ehdr.emean = 0.0;
	for (j = 0; j <= res; j++) {
		for (i = 0; i <= res; i++) {
			ehdr.emean += elev[i];
			ehdr.emax = max(ehdr.emax, (double)elev[i]);
			ehdr.emin = min(ehdr.emin, (double)elev[i]);
		}
		elev += TILE_ELEVSTRIDE;
	}
	ehdr.emean /= double((res + 1)*(res + 1));
}

// ------------------------------------------------------------------------------
// bGet(true) = Get the data specifically from this tile recardless of it's state
//
int SurfTile::GetElevation(double lng, double lat, double *elev, FVECTOR3 *nrm, SurfTile **cache, bool bFilter, bool bGet) const
{
	static int ndat = TILE_ELEVSTRIDE*TILE_ELEVSTRIDE;
	if (cache) *cache = (SurfTile *)this;

	if (lat<bnd.minlat || lat>bnd.maxlat) return -1;
	if (lng<bnd.minlng || lng>bnd.maxlng) return -1;

	if (state == ForRender || bGet)
	{
		if (!ggelev) { *elev = 0.0; return 2; }
		else {
			double fRes = double(mgr->GridRes());

			if (!bFilter) {
				int i = int((lat - bnd.minlat) * fRes / (bnd.maxlat - bnd.minlat)) + 1;
				int j = int((lng - bnd.minlng) * fRes / (bnd.maxlng - bnd.minlng)) + 1;
				*elev = double(ggelev[j+i*TILE_ELEVSTRIDE]);
			}
			else {

				float x = float((lat - bnd.minlat) * fRes / (bnd.maxlat - bnd.minlat)) + 1.0f; // 0.5f
				float y = float((lng - bnd.minlng) * fRes / (bnd.maxlng - bnd.minlng)) + 1.0f; // 0.5f
				float fx = (x - floor(x));
				float fy = (y - floor(y));

				int i0 = int(x) * TILE_ELEVSTRIDE;
				int i1 = i0 + TILE_ELEVSTRIDE;
				int j0 = int(y);

				assert(i0 > 0);
				assert(j0 > 0);
				assert((j0 + i1) < ndat);

				float q = lerp(ggelev[j0+i0], ggelev[j0+i1], fx); j0++;
				float w = lerp(ggelev[j0+i0], ggelev[j0+i1], fx);

				*elev = double(lerp(q,w,fy));
			}

			return 1;
		}
	}

	if (state == Invisible) return 0;

	if (state == Active) {
		int i = 0;
		if (lng > (bnd.minlng + bnd.maxlng)*0.5) i++;
		if (lat < (bnd.minlat + bnd.maxlat)*0.5) i += 2;
		if (node->Child(i))
			return node->Child(i)->Entry()->GetElevation(lng, lat, elev, nrm, cache);
	}

	if (state == Inactive) {
		// Calling elevation without tree being fully initialized.
		// Force data acquision using current level due to lack of render level. 
		return GetElevation(lng, lat, elev, nrm, cache, bFilter, true);
	}

	return -3;
}

// -----------------------------------------------------------------------

SurfTile *SurfTile::getTextureOwner()
{
	if (owntex) return this;
	SurfTile *parent = getSurfParent();
	while (parent) {
		if (parent->owntex) return parent;
		else parent = parent->getSurfParent();
	}
	return NULL;
}

// -----------------------------------------------------------------------

float SurfTile::fixinput(double a, int x)
{
	switch(x) {
		case 0: return float((1.0+floor(a*0.0625))*16.0);
		case 1: return float((1.0+floor(a*0.125))*8.0);
		case 2: return float((1.0+floor(a*0.5))*2.0);
	}
	return 0.0f;
}

// -----------------------------------------------------------------------

double SurfTile::GetCameraDistance()
{
	VECTOR3 cnt = Centre() * (mgr->CbodySize() + GetMeanElev());
	cnt = mgr->prm.cpos + mul(mgr->prm.grot, cnt);
	return length(cnt);
}

// -----------------------------------------------------------------------

FVECTOR4 SurfTile::MicroTexRange(SurfTile *pT, int ml) const
{
	float rs = 1.0f / float( 1 << (lvl-pT->Level()) );	// Range subdivision
	float xo = pT->MicroRep[ml].x * texrange.tumin;
	float yo = pT->MicroRep[ml].y * texrange.tvmin;
	xo -= floor(xo); // Micro texture offset for current tile
	yo -= floor(yo); // Micro texture offset for current tile
	return FVECTOR4(xo, yo, pT->MicroRep[ml].x * rs, pT->MicroRep[ml].y * rs);
}

// -----------------------------------------------------------------------
// Called during rendering even if this tile is not rendered but children are
// i.e. called for every RENDERED and ACTIVE tile
//
void SurfTile::StepIn ()
{
	// `LPDIRECT3DDEVICE9 pDev = mgr->Dev()` stood here and nothing reads it.
	const vPlanet *vPlanet = mgr->GetPlanet();

	if (vPlanet != mgr->GetScene()->GetCameraProxyVisual()) return;

	// Compute micro texture repeat counts -------------------------------------
	//
	if (owntex && vPlanet->MicroCfg.bEnabled) {
		double s = vPlanet->GetSize();
		for (int i = 0; i < int(ARRAYSIZE(vPlanet->MicroCfg.Level)); ++i) {
			double f = s / vPlanet->MicroCfg.Level[i].size;
			MicroRep[i] = FVECTOR2(fixinput(width*f, i), fixinput(height*f, i));
		}
	}
}


// -----------------------------------------------------------------------

void SurfTile::Render ()
{
	Tile::Render();

	if (!mesh) return; // DEBUG : TEMPORARY

	VulkanDevice *pDev = mgr->Dev();
	vPlanet *vPlanet = mgr->GetPlanet();
	const Scene *scene = mgr->GetScene();
	// `const D3D9Client *pClient = mgr->GetClient()` stood here and nothing
	// reads it -- the two uses below go through mgr->GetClient() directly.

	static const double rad0 = sqrt(2.0)*PI05;
	double sdist, rad;
	bool has_specular = false;
	bool has_shadows = false;
	bool has_lights = false;
	bool has_microtex = false;
	bool has_atmosphere = vPlanet->HasAtmosphere();
	bool has_ripples = vPlanet->HasRipples();
	// `bool bUseZBuf = mgr->IsUsingZBuf()` stood here and nothing reads it.

	if (vPlanet->CameraAltitude()>20e3) has_ripples = false;

	double ca = 1.0 + saturate(vPlanet->CameraAltitude() / 150e3) * Config->OrbitalShadowMult;

	PlanetShader* pShader = mgr->GetShader();
	ShaderParams* sp = vPlanet->GetTerrainParams();
	FlowControlPS* fc = vPlanet->GetFlowControl();
	FlowControlVS* fcv = vPlanet->GetFlowControlVS();

	bool render_lights = pShader->bNightlights;
	bool render_shadows = (mgr->GetPlanet()->CloudMgr2() != NULL) && mgr->GetClient()->GetConfigParam(CFGPRM_CLOUDSHADOWS) && pShader->bCloudShd;

	if (ltex) {
		sdist = acos(dotp(mgr->prm.sdir, cnt));
		rad = rad0 / (double)(2 << lvl); // tile radius
		has_specular = (ltex != NULL) && sdist < (1.75 + rad);
		has_lights = (render_lights && ltex && sdist > 1.35);
		has_shadows = (render_shadows && sdist < (PI05 + rad));
	}
	
	has_specular &= pShader->bWater;
	has_ripples &= pShader->bRipples & has_specular;
	has_lights &= pShader->bNightlights;
	has_atmosphere &= pShader->bAtmosphere;

	sp->vCloudOff = FVECTOR4(0, 0, 1, 1);

	// D3DXVec3TransformCoord(&bs_pos, &mesh->bsCnt, &mWorld)
	FVECTOR3 bs_pos = oapi::TransformCoord(mesh->bsCnt, mWorld);

	// ----------------------------------------------------------------------
	// Assign micro texture range information to shaders 
	// ----------------------------------------------------------------------

	SurfTile *pT = getTextureOwner();

	if (pT && vPlanet->MicroCfg.bEnabled && pShader->bMicrotex)
	{
		sp->vMSc[0] = MicroTexRange(pT, 0);
		sp->vMSc[1] = MicroTexRange(pT, 1);
		sp->vMSc[2] = MicroTexRange(pT, 2);
		has_microtex = true;
	}



	// ----------------------------------------------------------------------
	// Setup cloud shadows 
	// ----------------------------------------------------------------------

	has_shadows = false;

	if (render_shadows)
	{
		VulkanTexture * pCloud = NULL, * pCloud2 = NULL;

		const TileManager2<CloudTile> *cmgr = vPlanet->CloudMgr2();
		int maxlvl = min(lvl,9);
		double rot = mgr->prm.rprm->cloudrot;
		double edglat = (bnd.minlat + bnd.maxlat)*0.5;	// latitude of tile center
		double edglng = wrap(rot + bnd.minlng);		// surface tile minlng-edge position on cloud-layer

		for (int attempt=0;attempt<2;attempt++) {

			CloudTile *ctile = (CloudTile *)cmgr->SearchTile(edglng, edglat, maxlvl, true);

			if (ctile) {

				double icsize = double( 1 << ctile->Level() ) / PI;	// inverse of cloud tile size in radians

				// Compute surface tile uv origin on a selected claud tile
				// Note: edglng exists always within tile i.e. no wrap from PI to -PI
				double u0 = (edglng - ctile->bnd.minlng) * icsize;
				double v0 = (ctile->bnd.maxlat - bnd.maxlat) * icsize;		// Note: Tile corner is lower-left, texture corner is upper-left
				double u1 = u0 + (bnd.maxlng - bnd.minlng) * icsize;
				double v1 = v0 + (bnd.maxlat - bnd.minlat) * icsize;

				// Feed uv-offset and uv-range to the shaders
				sp->vCloudOff = FVECTOR4(float(u0), float(v0), float(u1-u0), float(v1-v0));
				sp->fAlpha = float(ca);
				pCloud = ctile->Tex();
			

				// Texture uv range extends to another tile
				if (u1 > 1.0) {

					double csize = PI / double(1<<ctile->Level());			// cloud tile size in radians
					double ctr = (ctile->bnd.minlng + ctile->bnd.maxlng) * 0.5;		// cloud tile center
					double lng  = wrap(ctr + csize*sign(rot));				// center of the next tile

					// Request an other tile from the same level as the first one
					CloudTile *ctile2 = (CloudTile *)cmgr->SearchTile(lng, edglat, ctile->Level(), true);

					if (ctile2) {
						if (ctile2->Level() == ctile->Level()) {
							// Rendering with dual texture
							pCloud2 = ctile2->Tex();
							has_shadows = true;
							break;
						}
						else {
							// Failed to match a dual texture render requirements (due to no-valid tile available)
							// Try again and request a lower level texture data for the first texture
							maxlvl = ctile2->Level();
							if (attempt==1) LogErr("CloudShadows mapping failed");
						}
					}
				}
				else {
					// Just one texture in use
					has_shadows = true;
					break;
				}
			}
		}

		pShader->SetTexture(pShader->tCloud, pCloud, IPF_CLAMP | IPF_ANISOTROPIC, Config->Anisotrophy);
		pShader->SetTexture(pShader->tCloud2, pCloud2, IPF_CLAMP | IPF_ANISOTROPIC, Config->Anisotrophy);
	}

	
	fc->bCloudShd = has_shadows;
	fc->bMicroTex = has_microtex;
	fc->bLocals = false;
	fc->bOverlay = false;
	fc->bMask = false;
	fc->bShadows = false;
	fc->bInSpace = !vPlanet->CameraInAtmosphere();
	fc->bPlanetShadow = vPlanet->SphericalShadow();
	fcv->bElevOvrl = false;


	// ----------------------------------------------------------------------
	// DevTools: Render with overlay image
	// ----------------------------------------------------------------------

	if (pShader->bDevtools && scene->GetRenderPass() == RENDERPASS_MAINSCENE)
	{
		FVECTOR4 texcoord;
		const vPlanet::sOverlay* oLay = vPlanet->IntersectOverlay(bnd.vec, &texcoord);

		if (oLay)
		{
			bool bOlayEnable = false; for (auto x : oLay->pSurf) if (x) bOlayEnable = true;

			if (bOlayEnable)
			{
				// Global large-scale overlay
				pShader->SetTexture("tOverlay", oLay->pSurf[0]);
				pShader->SetTexture("tMskOverlay", oLay->pSurf[1]);
				pShader->SetTexture("tElvOverlay", oLay->pSurf[2]);

				memcpy(&sp->vOverlayCtrl, &oLay->Blend, sizeof(FVECTOR4) * 4);
				sp->vOverlayOff = texcoord;

				if (oLay->pSurf[0] || oLay->pSurf[1]) fc->bOverlay = true;
				if (oLay->pSurf[2]) fcv->bElevOvrl = true;
			}
		}
		else if (overlay) {
			// Local tile specific overlay
			pShader->SetTexture("tOverlay", overlay);
			sp->vOverlayOff = GetTexRangeDX(&overlayrange);
			fc->bOverlay = true;
		}
	}


	// ----------------------------------------------------------------------
	// Setup Main Texture
	// ----------------------------------------------------------------------

	pShader->SetTexture(pShader->tDiff, tex, IPF_CLAMP | IPF_ANISOTROPIC, Config->Anisotrophy);
	fc->bTexture = (tex != nullptr);

	// ----------------------------------------------------------------------
	// Night Lights and Water Specular
	// ----------------------------------------------------------------------
	
	if (pShader->bNightlights || pShader->bWater)
	{
		if ((has_specular || has_lights) && ltex) {
			pShader->SetTexture(pShader->tMask, ltex, IPF_CLAMP | IPF_ANISOTROPIC, Config->Anisotrophy);
			fc->bMask = true;
		}
		else pShader->SetTexture(pShader->tMask, NULL, IPF_CLAMP | IPF_ANISOTROPIC, Config->Anisotrophy);
	}

	if (tex == nullptr || !vPlanet->HasTextures())
	{
		fc->bTexture = false;
		fc->bMask = false;
		fc->bCloudShd = false;
		fc->bMicroTex = false;
	}

	sp->vTexOff = GetTexRangeDX(&texrange);
	sp->vMicroOff = GetTexRangeDX(&microrange);
	sp->mWorld = mWorld;
	sp->fTgtScale = tgtscale;

	if (has_lights) sp->fBeta = float(mgr->Cprm().lightfac);
	else sp->fBeta = 0.0f;

	// has_atmosphere is computed and, on both platforms, never read.
	(void)has_atmosphere;
	(void)has_ripples;



	// ---------------------------------------------------------------------
	// Setup shadow maps
	// ---------------------------------------------------------------------

	if (pShader->bShdMap)
	{
		const Scene::SHADOWMAPPARAM* shd = scene->GetSMapData();

		FVECTOR3 bc = bs_pos - shd->pos;

		double alt = scene->GetCameraAltitude() - scene->GetTargetElevation();

		if ((alt < 10e3) && (scene->GetCameraProxyVisual() == mgr->GetPlanet())) {

			if (shd->pShadowMap && (Config->ShadowMapMode != 0) && (Config->TerrainShadowing == 2)) {

				float x = dot(bc, shd->ld);

				if (sqrt(dot(bc, bc) - x * x) < (shd->rad + mesh->bsRad)) {
					float s = float(shd->size);
					float sr = 2.0f * shd->rad / s;
					sp->mLVP = shd->mViewProj;
					sp->vSHD = FVECTOR4(sr, 1.0f / s, 0.0f, 1.0f / shd->depth);
					fc->bShadows = true;
				}
			}
		}

		pShader->SetTexture(pShader->tShadowMap, shd->pShadowMap);
	}

	// ---------------------------------------------------------------------
	// Setup local light sources
	//---------------------------------------------------------------------

	int cfg = vPlanet->GetShaderID();

	LightF Locals;
	BOOL Spots[4];

	if (cfg != PLT_GIANT)
	{
		for (int i = 0; i < 4; i++)
		{
			Locals.attenuation[i] = FVECTOR3(1.0f, 1.0f, 1.0f);
			Locals.diffuse[i] = FVECTOR3(0.0f, 0.0f, 0.0f);
			Locals.direction[i] = FVECTOR3(1.0f, 0.0f, 0.0f);
			Locals.param[i] = FVECTOR4(0.0f, 0.0f, 0.0f, 0.0f);
			Locals.position[i] = FVECTOR3(0.0f, 0.0f, 0.0f);
			Spots[i] = false;
		}

		if (scene->GetRenderPass() == RENDERPASS_MAINSCENE)
		{
			const VulkanLight* pLights = scene->GetLights();
			int nSceneLights = min(scene->GetLightCount(), MAX_SCENE_LIGHTS);

			if (pLights && nSceneLights > 0 && pShader->bLocals)
			{
				int nMeshLights = 0;

				_LightList LightList[MAX_SCENE_LIGHTS];

				// Find all local lights effecting this mesh ------------------------------------------
				//
				for (int i = 0; i < nSceneLights; i++) {
					float il = pLights[i].GetIlluminance(bs_pos, mesh->bsRad);
					if (il > 0.005f) {
						LightList[nMeshLights].illuminace = il;
						LightList[nMeshLights++].idx = i;
					}
				}

				if (nMeshLights > 0) {

					// If any, Sort the list based on illuminance -------------------------------------------
					qsort(LightList, nMeshLights, sizeof(_LightList), compare_lights);

					nMeshLights = min(nMeshLights, 4);

					// Create a list of N most effective lights ---------------------------------------------
					for (int i = 0; i < nMeshLights; i++)
					{
						auto pL = pLights[LightList[i].idx];
						Locals.attenuation[i] = pL.Attenuation;
						Locals.diffuse[i] = FVECTOR4(pL.Diffuse).rgb;
						Locals.direction[i] = pL.Direction;
						Locals.param[i] = pL.Param;
						Locals.position[i] = pL.Position;
						Spots[i] = (pL.Type == 1);
					}

					// Enable local lights and feed data to shader
					fc->bLocals = true;
				}
			}
		}
	}



	// -------------------------------------------------------------------
	// render surface mesh
	//
	pShader->SetVSConstants(pShader->PrmVS, sp, sizeof(ShaderParams));
	pShader->SetPSConstants(pShader->Prm, sp, sizeof(ShaderParams));


	pShader->SetVSConstants(pShader->FlowVS, fcv, sizeof(FlowControlVS));
	pShader->SetPSConstants(pShader->Flow, fc, sizeof(FlowControlPS));

	if (fc->bLocals && (cfg != PLT_GIANT))
	{
		pShader->SetPSConstants(pShader->Lights, &Locals, sizeof(Locals));
		pShader->SetPSConstants(pShader->Spotlight, Spots, sizeof(Spots));
	}

	pShader->UpdateTextures();

	// SetStreamSource + SetIndices + DrawIndexedPrimitive(TRIANGLELIST, 0, 0,
	// mesh->nv, 0, mesh->nf). The last argument is a FACE count and
	// vkCmdDrawIndexed's first is an INDEX count; the stride moved into the
	// vertex declaration and VK_INDEX_TYPE_UINT16 into the bind.
	if (pDev->IsRecording() && mesh->pVB && mesh->pIB) {
		VkCommandBuffer cmd = pDev->GetCommandBuffer();
		VkBuffer vb = mesh->pVB->Buffer();
		VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
		vkCmdBindIndexBuffer(cmd, mesh->pIB->Buffer(), 0, VK_INDEX_TYPE_UINT16);
		vkCmdDrawIndexed(cmd, mesh->nf * 3, 1, 0, 0, 0);
		// DIAGNOSTIC: paint the whole attachment magenta right after a tile
		// draw. This uses no pipeline, no shader and no transform -- if the
		// window does not turn magenta, this command buffer is not the one
		// that reaches the screen, whatever its extent says.
		// EVERY FRAME, not the first few: a clear issued only in frame 1 is
		// invisible by the time anyone looks at the window.
		if (getenv("ORBITER_VK_TILE_CLEAR"))
			pDev->ClearFrame(true, false, false, 0x00FF00FF, 1.0f, 0);
		// ONLY THE MAIN SCENE. The env-camera pass draws the same tiles into a
		// cube face first, with its own 90-degree square camera, and those
		// draws are not what reaches the window. Tracing the first draws of a
		// frame without this filter measures the cube face and calls it the
		// screen -- which is exactly the mistake that produced the earlier
		// "every tile is behind the camera" reading.
		if (getenv("ORBITER_VK_TRACE_TILES") && scene->GetRenderPass() == RENDERPASS_MAINSCENE) {
			static int nDrawn = 0;
			if (++nDrawn <= 24) {
				// WHICH TARGET is this draw going into? A tile drawn into an
				// offscreen pass (shadow map, env map) never reaches the
				// screen, and looks identical from the CPU side to one that
				// does. The frame extent is the tell: the core's frame is the
				// window, an offscreen target is its own size.
				LogErr("TILETRACE surf draw #%d nf=%u nv=%u cmd=%p frame=%ux%u",
					   nDrawn, (unsigned)mesh->nf, (unsigned)mesh->nv,
					   (void *)pDev->GetCommandBuffer(),
					   pDev->GetFrameWidth(), pDev->GetFrameHeight());
				// The world matrix AS UPLOADED for this tile -- measured at
				// the draw, not at the manager, because sp is a shared struct
				// filled per tile.
				// THE VERTEX DATA ITSELF -- the last input never checked. If
				// the buffer was never filled, every position is garbage and
				// every triangle clips away, which looks exactly like "the
				// draw produces no fragments".
				if (mesh->pVB->IsHostVisible()) {
					const VERTEX_2TEX *v = (const VERTEX_2TEX *)mesh->pVB->Map();
					if (v) {
						for (int k = 0; k < 3; k++)
							LogErr("TILETRACE   vtx[%d] pos=(% .3f % .3f % .3f) nrm=(% .3f % .3f % .3f) uv=(%.3f %.3f) e=%.3f",
								   k, v[k].x, v[k].y, v[k].z, v[k].nx, v[k].ny, v[k].nz,
								   v[k].tu0, v[k].tv0, v[k].e);
						mesh->pVB->Unmap();
					}
					else LogErr("TILETRACE   vertex buffer Map() failed");
				}
				else LogErr("TILETRACE   vertex buffer is device-local, cannot read back");

				// WHERE DOES THE WHOLE MESH LAND? Vertex 0 alone proves
				// nothing: a top-level tile spans a quadrant of the globe, so
				// its first corner is routinely on the far side and behind the
				// camera even when the tile is perfectly visible. Sweep every
				// vertex through the same arithmetic the shader does --
				// row-vector v*M twice -- and count how many survive the
				// clip test. Zero over every tile means the geometry never
				// reaches the rasteriser; a nonzero count means it does and
				// the fault is downstream.
				if (mesh->pVB->IsHostVisible()) {
					const VERTEX_2TEX *v = (const VERTEX_2TEX *)mesh->pVB->Map();
					if (v) {
						const FMATRIX4P &W = sp->mWorld;
						const FMATRIX4 &P = vPlanet->GetScatterConst()->mVP;
						int nIn = 0, nBehind = 0;
						float sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0;
						bool bAny = false;
						for (DWORD k = 0; k < mesh->nv; k++) {
							float wx = v[k].x*W.m11 + v[k].y*W.m21 + v[k].z*W.m31 + W.m41;
							float wy = v[k].x*W.m12 + v[k].y*W.m22 + v[k].z*W.m32 + W.m42;
							float wz = v[k].x*W.m13 + v[k].y*W.m23 + v[k].z*W.m33 + W.m43;
							float cx = wx*P.m11 + wy*P.m21 + wz*P.m31 + P.m41;
							float cy = wx*P.m12 + wy*P.m22 + wz*P.m32 + P.m42;
							float cz = wx*P.m13 + wy*P.m23 + wz*P.m33 + P.m43;
							float cw = wx*P.m14 + wy*P.m24 + wz*P.m34 + P.m44;
							if (cw <= 0.0f) { nBehind++; continue; }
							float nx = cx / cw, ny = cy / cw, nz = cz / cw;
							if (nx >= -1.0f && nx <= 1.0f && ny >= -1.0f && ny <= 1.0f &&
								nz >= 0.0f && nz <= 1.0f) {
								nIn++;
								if (!bAny) { sx0 = sx1 = nx; sy0 = sy1 = ny; bAny = true; }
								else {
									sx0 = min(sx0, nx); sx1 = max(sx1, nx);
									sy0 = min(sy0, ny); sy1 = max(sy1, ny);
								}
							}
						}
						LogErr("TILETRACE   clip sweep: %d/%u inside, %d behind camera%s",
							   nIn, (unsigned)mesh->nv, nBehind,
							   bAny ? "" : "  <-- NOTHING VISIBLE FROM THIS TILE");
						if (bAny)
							LogErr("TILETRACE   ndc box x[% .3f % .3f] y[% .3f % .3f]  -> px x[%.0f %.0f] y[%.0f %.0f]",
								   sx0, sx1, sy0, sy1,
								   (sx0*0.5f+0.5f)*pDev->GetFrameWidth(),
								   (sx1*0.5f+0.5f)*pDev->GetFrameWidth(),
								   (sy0*0.5f+0.5f)*pDev->GetFrameHeight(),
								   (sy1*0.5f+0.5f)*pDev->GetFrameHeight());
						mesh->pVB->Unmap();
					}
				}

				const FMATRIX4P &w = sp->mWorld;
				LogErr("TILETRACE   mWorld r0 % .5f % .5f % .5f % .5f", w.m11, w.m12, w.m13, w.m14);
				LogErr("TILETRACE   mWorld r1 % .5f % .5f % .5f % .5f", w.m21, w.m22, w.m23, w.m24);
				LogErr("TILETRACE   mWorld r2 % .5f % .5f % .5f % .5f", w.m31, w.m32, w.m33, w.m34);
				LogErr("TILETRACE   mWorld r3 % .1f % .1f % .1f % .5f", w.m41, w.m42, w.m43, w.m44);
			}
		}
	}
	else {
		// THE GUARD IS NEW AND IT MUST NOT BE SILENT. D3D9 drew whatever was
		// bound; here a tile whose buffers were never created simply does not
		// appear, with nothing said. Reported once so "the planet is missing"
		// cannot be mistaken for a shading problem again.
		static int nSkipped = 0;
		if (++nSkipped <= 3)
			LogErr("SurfTile::Render: no draw -- recording=%d pVB=%p pIB=%p nf=%u",
				   int(pDev->IsRecording()), (void *)mesh->pVB, (void *)mesh->pIB,
				   (unsigned)mesh->nf);
	}
	
	// Render tile bounding box
	//
	/*
	if (DebugControls::IsActive()) {
		DWORD flags  = *(DWORD*)mgr->GetClient()->GetConfigParam(CFGPRM_GETDEBUGFLAGS);
		if (flags&DBG_FLAGS_TILEBOXES) {
			VulkanEffect::RenderTileBoundingBox(&mWorld, mesh->Box, ptr(FVECTOR4(1.0f,0.0f,0.0f,1.0f)));
		}
	}*/
}

// -----------------------------------------------------------------------

void SurfTile::MatchEdges ()
{
	if (edgeok) return; // done already
	edgeok = true;
	if (!mesh) return;  // sanity check

	QuadTreeNode<SurfTile>* lngnbr = smgr->FindNode(lvl, ilng + (ilng & 1 ? 1 : -1), ilat);
	QuadTreeNode<SurfTile>* latnbr = smgr->FindNode(lvl, ilng, ilat + (ilat & 1 ? 1 : -1));
	QuadTreeNode<SurfTile>* dianbr = smgr->FindNode(lvl, ilng + (ilng & 1 ? 1 : -1), ilat + (ilat & 1 ? 1 : -1));


	if (lngnbr && !(lngnbr->Entry()->state & TILE_VALID)) lngnbr = 0;
	if (latnbr && !(latnbr->Entry()->state & TILE_VALID)) latnbr = 0;
	if (dianbr && !(dianbr->Entry()->state & TILE_VALID)) dianbr = 0;

	int new_lngnbr_lvl = (lngnbr ? lngnbr->Entry()->lvl : lvl);
	int new_latnbr_lvl = (latnbr ? latnbr->Entry()->lvl : lvl);
	int new_dianbr_lvl = (dianbr ? dianbr->Entry()->lvl : lvl);

	// fix lower-level neighbours first
	bool nbr_updated = false;
	if (new_lngnbr_lvl < lvl) {
		lngnbr->Entry()->MatchEdges ();
		nbr_updated = true;
	}
	if (new_latnbr_lvl < lvl) {
		latnbr->Entry()->MatchEdges ();
		nbr_updated = true;
	}
	if (!nbr_updated && new_dianbr_lvl < lvl) {
		// now we need to check the diagonal neighbour, since this could be lower resolution
		// and thus responsible for the corner node elevation
		dianbr->Entry()->MatchEdges ();
	}

	bool lngedge_changed = (new_lngnbr_lvl != lngnbr_lvl);
	bool latedge_changed = (new_latnbr_lvl != latnbr_lvl);
	bool diaedge_changed = (new_dianbr_lvl != dianbr_lvl);

	if (lngedge_changed || latedge_changed || diaedge_changed) {
		if (new_dianbr_lvl < new_lngnbr_lvl && new_dianbr_lvl < new_latnbr_lvl) {
			FixCorner (dianbr ? dianbr->Entry() : 0);
			FixLatitudeBoundary (latnbr ? latnbr->Entry() : 0, true);
			FixLongitudeBoundary (lngnbr ? lngnbr->Entry() : 0, true);
		} else if (new_latnbr_lvl < new_lngnbr_lvl) {
			FixLatitudeBoundary (latnbr ? latnbr->Entry() : 0);
			FixLongitudeBoundary (lngnbr ? lngnbr->Entry() : 0, true);
		} else {
			FixLongitudeBoundary (lngnbr ? lngnbr->Entry() : 0);
			FixLatitudeBoundary (latnbr ? latnbr->Entry() : 0, true);
		}
		mesh->MapVertices(mgr->Dev()); // copy the updated vertices to the vertex buffer
		lngnbr_lvl = new_lngnbr_lvl;
		latnbr_lvl = new_latnbr_lvl;
		dianbr_lvl = new_dianbr_lvl;
	}
}

// -----------------------------------------------------------------------

void SurfTile::FixCorner (const SurfTile *nbr)
{
	if (!mesh) return; // sanity check

	int res = mgr->GridRes();
	int vtx_idx = (ilat & 1 ? 0 : (res+1)*res) + (ilng & 1 ? res : 0);
	VERTEX_2TEX &vtx_store = mesh->vtx[mesh->nv + (ilat & 1 ? 0 : res)];

	float *elev = ElevationData();
	if (!elev) return;

	if (nbr) {
		float *nbr_elev = nbr->ElevationData();
		if (!nbr_elev) return;

		float corner_elev = elev[TILE_ELEVSTRIDE+1 + (ilat & 1 ? 0 : TILE_ELEVSTRIDE*res) + (ilng & 1 ? res : 0)];
		float nbr_corner_elev = nbr_elev[TILE_ELEVSTRIDE+1 + (ilat & 1 ? TILE_ELEVSTRIDE*res : 0) + (ilng & 1 ? 0 : res)];

		double rad = mgr->CbodySize();
		double radfac = (rad+nbr_corner_elev)/(rad+corner_elev);
		mesh->vtx[vtx_idx].x = (float)(vtx_store.x*radfac + vtxshift.x*(radfac-1.0));
		mesh->vtx[vtx_idx].y = (float)(vtx_store.y*radfac + vtxshift.y*(radfac-1.0));
		mesh->vtx[vtx_idx].z = (float)(vtx_store.z*radfac + vtxshift.z*(radfac-1.0));
	}
}

// -----------------------------------------------------------------------

void SurfTile::FixLongitudeBoundary (const SurfTile *nbr, bool keep_corner)
{
	// Fix the left or right edge
	int i, i0, i1, j, vtx_ofs, nbrlvl, dlvl;
	int res = mgr->GridRes();
	VERTEX_2TEX *vtx_store;

	vtx_ofs = (ilng & 1 ? res : 0); // check if neighbour is at left or right edge
	if (nbr && nbr->mesh && nbr->mesh->vtx) {
		nbrlvl = min(nbr->lvl, lvl); // we don't need to worry about neigbour levels higher than ours
		vtx_store = mesh->vtx + mesh->nv;
		if (nbrlvl == lvl) { // put my own edge back
			i0 = 0;
			i1 = res;
			// Braced: `if (keep_corner) if (...) ...; else ...;` is a
			// -Wdangling-else. The else binds to the INNER if, which is
			// what is meant; the braces say so. MSVC does not report it.
			if (keep_corner) {
				if (ilat & 1) i0++; else i1--;
			}
			for (i = i0; i <= i1; i++)
				mesh->vtx[vtx_ofs+i*(res+1)] = vtx_store[i];
		} else {  // interpolate to neighbour's left edge
			dlvl = lvl-nbrlvl;
			int nsub = 1 << dlvl; // number of tiles fitting alongside the lowres neighbour
			if (nsub <= res) { // for larger tile level differences the interleaved sampling method doesn't work
				float *elev = ElevationData();
				float *nbr_elev = nbr->ElevationData();
				if (elev && nbr_elev) {
					elev += TILE_ELEVSTRIDE+1 + vtx_ofs; // add offset
					int nsub = 1 << dlvl;    // number of tiles fitting alongside the lowres neighbour
					int vtx_skip = nsub*(res+1);  // interleave factor for nodes attaching to neigbour nodes
					int nbr_range = res/nsub; // number of neighbour vertices attaching to us - 1
					int subidxmask = (1<<dlvl)-1;
					int subidx = ilat & subidxmask;
					int nbr_ofs = (subidxmask-subidx) * nbr_range * TILE_ELEVSTRIDE;
					nbr_elev += TILE_ELEVSTRIDE+1 + nbr_ofs + (res-vtx_ofs);
					double rad = mgr->CbodySize();
					// match nodes to neighbour elevations
					i0 = 0;
					i1 = nbr_range;
					if (keep_corner) {
						if (ilat & 1) i0++; else i1--;
					}
					for (i = i0; i <= i1; i++) {
						double radfac = (rad+nbr_elev[i*TILE_ELEVSTRIDE])/(rad+elev[i*TILE_ELEVSTRIDE*nsub]);
						mesh->vtx[vtx_ofs+i*vtx_skip].x = (float)(vtx_store[i*nsub].x*radfac + vtxshift.x*(radfac-1.0));
						mesh->vtx[vtx_ofs+i*vtx_skip].y = (float)(vtx_store[i*nsub].y*radfac + vtxshift.y*(radfac-1.0));
						mesh->vtx[vtx_ofs+i*vtx_skip].z = (float)(vtx_store[i*nsub].z*radfac + vtxshift.z*(radfac-1.0));
					}
					// interpolate the nodes that fall between neighbour nodes
					for (i = 0; i < nbr_range; i++)
						for (j = 1; j < nsub; j++)
							VtxInterpolate (mesh->vtx[vtx_ofs+j*(res+1)+i*vtx_skip], mesh->vtx[vtx_ofs+i*vtx_skip], mesh->vtx[vtx_ofs+(i+1)*vtx_skip], (double)j/double(nsub));
				}
			} else {
				// problems
			}
		}
	}
}

// -----------------------------------------------------------------------

void SurfTile::FixLatitudeBoundary (const SurfTile *nbr, bool keep_corner)
{
	// Fix the top or bottom edge
	int i, i0, i1, j, nbrlvl, dlvl;
	int res = mgr->GridRes();
	VERTEX_2TEX *vtx_store;

	int line = (ilat & 1 ? 0 : res);
	int vtx_ofs = (ilat & 1 ? 0 : line*(res+1));

	if (nbr && nbr->mesh && nbr->mesh->vtx) {
		nbrlvl = min(nbr->lvl, lvl); // we don't need to worry about neigbour levels higher than ours
		vtx_store = mesh->vtx + mesh->nv + res + 1;
		if (nbrlvl == lvl) { // put my own edge back
			i0 = 0;
			i1 = res;
			// Braced: `if (keep_corner) if (...) ...; else ...;` is a
			// -Wdangling-else. The else binds to the INNER if, which is
			// what is meant; the braces say so. MSVC does not report it.
			if (keep_corner) {
				if (ilng & 1) i1--; else i0++;
			}
			for (i = i0; i <= i1; i++)
				mesh->vtx[vtx_ofs+i] = vtx_store[i];
		} else {
			dlvl = lvl-nbrlvl;
			int nsub = 1 << dlvl; // number of tiles fitting alongside the lowres neighbour
			if (nsub <= res) { // for larger tile level differences the interleaved sampling method doesn't work
				float *elev = ElevationData();
				float *nbr_elev = nbr->ElevationData();
				if (elev && nbr_elev) {
					elev += (line+1)*TILE_ELEVSTRIDE + 1; // add offset
					int nsub = 1 << dlvl;    // number of tiles fitting alongside the lowres neighbour
					int vtx_skip = nsub;     // interleave factor for nodes attaching to neigbour nodes
					int nbr_range = res/nsub; // number of neighbour vertices attaching to us - 1
					int subidxmask = (1<<dlvl)-1;
					int subidx = ilng & subidxmask;
					int nbr_ofs = subidx * nbr_range;
					nbr_elev += TILE_ELEVSTRIDE+1 + nbr_ofs + (res-line)*TILE_ELEVSTRIDE;
					double rad = mgr->CbodySize();
					// match nodes to neighbour elevations
					i0 = 0;
					i1 = nbr_range;
					if (keep_corner) {
						if (ilng & 1) i1--; else i0++;
					}
					for (i = i0; i <= i1; i++) {
						double radfac = (rad+nbr_elev[i])/(rad+elev[i*nsub]);
						mesh->vtx[vtx_ofs+i*vtx_skip].x = (float)(vtx_store[i*nsub].x*radfac + vtxshift.x*(radfac-1.0));
						mesh->vtx[vtx_ofs+i*vtx_skip].y = (float)(vtx_store[i*nsub].y*radfac + vtxshift.y*(radfac-1.0));
						mesh->vtx[vtx_ofs+i*vtx_skip].z = (float)(vtx_store[i*nsub].z*radfac + vtxshift.z*(radfac-1.0));
					}
					// interpolate the nodes that fall between neighbour nodes
					for (i = 0; i < nbr_range; i++)
						for (j = 1; j < nsub; j++)
							VtxInterpolate (mesh->vtx[vtx_ofs+i*vtx_skip+j], mesh->vtx[vtx_ofs+i*vtx_skip], mesh->vtx[vtx_ofs+(i+1)*vtx_skip], (double)j/double(nsub));
				}
			} else {
				// problems
			}
		}
	}
}

// -----------------------------------------------------------------------

void SurfTile::CreateLabels()
{
	if (!label) {
		label = TileLabel::Create(this);
	}
}

// -----------------------------------------------------------------------

inline void SurfTile::DeleteLabels()
{
	SAFE_DELETE(label);
}

// -----------------------------------------------------------------------

void SurfTile::RenderLabels(VulkanPad *skp, oapi::Font **labelfont, int *fontidx)
{
	if (!label) return;
	label->Render(skp, labelfont, fontidx);
}


// =======================================================================
// =======================================================================

template<>
void TileManager2<SurfTile>::Render (MATRIX4 &dwmat, bool use_zbuf, const vPlanet::RenderPrm &rprm)
{
	bUseZ = use_zbuf;
	ElevModeLvl = 0;

	// set generic parameters
	SetRenderPrm (dwmat, 0, use_zbuf, rprm);

	int i;
	class Scene *scene = GetClient()->GetScene();

	// adjust scaling parameters (can only be done if no z-buffering is in use)
	if (!use_zbuf) {
		double R = obj_size;
		double D = prm.cdist*R;
		double zmax = (D - R*R/D) * 1.5;
		double zmin = max (2.0, min (zmax*1e-4, (D-R) * 0.8));

		vp->GetScatterConst()->mVP = scene->PushCameraFrustumLimits(zmin, zmax);
	}

	// build a transformation matrix for frustum testing
	MATRIX4 Mproj = _MATRIX4(scene->GetProjectionMatrix());
	Mproj.m33 = 1.0; Mproj.m43 = -1.0;  // adjust near plane to 1, far plane to infinity
	MATRIX4 Mview = _MATRIX4(scene->GetViewMatrix());
	prm.dviewproj = mul(Mview, Mproj);

	// ---------------------------------------------------------------------

	// `static float spec_base = 0.7f;` stood here and nothing reads it.

	bool has_ripples = vp->HasRipples();

	// Choose a proper shader for a body
	//
	pShader = vp->GetShader();
	int cfg = vp->GetShaderID();

	pShader->ClearTextures();
	// SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW) followed Setup() on
	// Windows. Cull is pipeline state here, so it has to be declared BEFORE
	// the pipeline is built -- see point 1 in the file header and
	// ShaderClass::SetCullMode.
	pShader->SetCullMode(ShaderClass::CULL_CCW);
	pShader->Setup(pPatchVertexDecl, bUseZ, 0);

	ShaderParams* sp = vp->GetTerrainParams();
	FlowControlPS* fc = vp->GetFlowControl();
	FlowControlVS* fcv = vp->GetFlowControlVS();
	ConstParams* cp = vp->GetScatterConst();


	// The view-projection the tiles are transformed by, printed once. If this
	// is zeroes or nonsense the geometry collapses and nothing appears, which
	// is indistinguishable from "not drawn" on screen. Env-gated.
	if (getenv("ORBITER_VK_TRACE_MTX")) {
		// WHICH PASS, WHICH FRAME, WHICH BODY. cp->mVP is written once per
		// frame by vPlanet::UpdateScatter, and only for RENDERPASS_MAINSCENE.
		// If a tile render runs in a pass that never refreshed it, or in a
		// frame where UpdateScatter bailed out, the tiles are transformed by
		// a stale matrix and every triangle lands behind the camera. One line
		// per manager call so the pattern over frames is visible.
		static int nPass = 0;
		if (nPass++ < 60) {
			const FMATRIX4 &q = cp->mVP;
			LogErr("MTXPASS #%d frame=%u pass=%u zbuf=%d body=%s  mVP r0=(% .4f % .4f % .4f % .4f) r3=(% .1f % .1f % .1f % .4f)",
				   nPass, (unsigned)scene->GetFrameId(), (unsigned)scene->GetRenderPass(),
				   int(use_zbuf), CbodyName(),
				   q.m11, q.m12, q.m13, q.m14, q.m41, q.m42, q.m43, q.m44);
		}
		static int nDump = 0;
		if (nDump++ < 2) {
			const FMATRIX4 &m = cp->mVP;
			LogErr("MTXDUMP Const.mVP row0 % .4f % .4f % .4f % .4f", m.m11, m.m12, m.m13, m.m14);
			LogErr("MTXDUMP Const.mVP row1 % .4f % .4f % .4f % .4f", m.m21, m.m22, m.m23, m.m24);
			LogErr("MTXDUMP Const.mVP row2 % .4f % .4f % .4f % .4f", m.m31, m.m32, m.m33, m.m34);
			LogErr("MTXDUMP Const.mVP row3 % .4f % .4f % .4f % .4f", m.m41, m.m42, m.m43, m.m44);
			LogErr("MTXDUMP sizeof(ConstParams)=%u sizeof(ShaderParams)=%u",
				   (unsigned)sizeof(ConstParams), (unsigned)sizeof(ShaderParams));
			const FMATRIX4P &w = sp->mWorld;
			LogErr("MTXDUMP Prm.mWorld row0 % .4f % .4f % .4f % .4f", w.m11, w.m12, w.m13, w.m14);
			LogErr("MTXDUMP Prm.mWorld row1 % .4f % .4f % .4f % .4f", w.m21, w.m22, w.m23, w.m24);
			LogErr("MTXDUMP Prm.mWorld row2 % .4f % .4f % .4f % .4f", w.m31, w.m32, w.m33, w.m34);
			LogErr("MTXDUMP Prm.mWorld row3 % .1f % .1f % .1f % .4f", w.m41, w.m42, w.m43, w.m44);
		}
	}

	pShader->SetVSConstants("Const", cp, sizeof(ConstParams));
	pShader->SetPSConstants("Const", cp, sizeof(ConstParams));

	if (pShader->bAtmosphere && cfg != PLT_GIANT)
	{
		pShader->SetTexture("tLndRay", vp->GetScatterTable(RAY_LAND), IPF_LINEAR | IPF_CLAMP);
		pShader->SetTexture("tLndMie", vp->GetScatterTable(MIE_LAND), IPF_LINEAR | IPF_CLAMP);
		pShader->SetTexture("tLndAtn", vp->GetScatterTable(ATN_LAND), IPF_LINEAR | IPF_CLAMP);
		pShader->SetTexture("tSun", vp->GetScatterTable(SUN_COLOR), IPF_LINEAR | IPF_CLAMP);
		pShader->SetTexture("tNoise", GetClient()->GetNoiseTex(), IPF_LINEAR | IPF_WRAP);

		if (pShader->bWater) {
			pShader->SetTexture("tAmbient", vp->GetScatterTable(SKY_AMBIENT), IPF_LINEAR | IPF_CLAMP);
		}
	}

	if (ElevMode == eElevMode::Spherical) {
		// Force spherical rendering at shader level
		fcv->bSpherical = true;
	}	

	if (!use_zbuf) fcv->bSpherical = true;

	// -------------------------------------------------------------------
	vVessel *vFocus = scene->GetFocusVisual();
	vPlanet *vProxy = scene->GetCameraProxyVisual();
	// Both are fetched and, on both platforms, never read.
	(void)vFocus; (void)vProxy;
	// sp is fetched here and read only by SurfTile::Render, which fetches it
	// again for each tile.
	(void)sp;


	// Setup micro textures ---------------------------------------------
	//
	if (vp->MicroCfg.bEnabled && pShader->bMicrotex)
	{
		UINT Filter = IPF_ANISOTROPIC;
		if (Config->MicroFilter == 0) Filter = IPF_POINT;
		if (Config->MicroFilter == 1) Filter = IPF_LINEAR;
		UINT micro_aniso = (1 << (max(1, Config->MicroFilter) - 1));
		pShader->SetTexture("tMicroA", vp->MicroCfg.Level[0].pTex, IPF_WRAP | Filter, micro_aniso);
		pShader->SetTexture("tMicroB", vp->MicroCfg.Level[1].pTex, IPF_WRAP | Filter, micro_aniso);
		pShader->SetTexture("tMicroC", vp->MicroCfg.Level[2].pTex, IPF_WRAP | Filter, micro_aniso);

		fc->bMicroNormals = vp->MicroCfg.bNormals;
	}

	if (has_ripples && pShader->bRipples) {
		fc->bRipples = true;
		pShader->SetTexture("tOcean", hOcean, IPF_WRAP | IPF_ANISOTROPIC, 4);
	}

	vp->InitEclipse(pShader);

	// SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, 0) stood here and its
	// restore stood after the tree walk, both under Config->NoPlanetAA.
	// Multisample state is baked into the pipeline and into the render pass,
	// and the core's render pass has one sample -- so there is nothing to
	// turn off. See point 1 in the file header.

	// ------------------------------------------------------------------
	// TODO: render full sphere for levels < 4

	loader->WaitForMutex();

	// update the tree
	for (i = 0; i < 2; i++)
		ProcessNode (tiletree+i);

	vp->tile_cache = NULL;

	// render the tree
	for (i = 0; i < 2; i++)
		RenderNode (tiletree+i);

	loader->ReleaseMutex();

	// Backup the stats and clear counters
	if (scene->GetRenderPass() == RENDERPASS_MAINSCENE) prevstat = elvstat;
	elvstat.Elev = elvstat.Sphe = 0;

	/*if (scene->GetRenderPass() == RENDERPASS_MAINSCENE) {
		if (GetHandle() == scene->GetCameraNearBody()) {
			VulkanDebugLog("Body=%s, Elv=%d, Sph=%d", CbodyName(), prevstat.Elev, prevstat.Sphe);
			if (ElevMode == eElevMode::Elevated) VulkanDebugLog("Elevated");
			if (ElevMode == eElevMode::Spherical) VulkanDebugLog("Spherical");
			if (ElevMode == eElevMode::ForcedElevated) VulkanDebugLog("ForcedElevated");
		}
	}*/

	// Pop previous frustum configuration, must initialize mVP
	if (!use_zbuf)	vp->GetScatterConst()->mVP = scene->PopCameraFrustumLimits();
}

// -----------------------------------------------------------------------

template<>
void TileManager2<SurfTile>::RenderLabels(VulkanPad *skp, oapi::Font **labelfont, int *fontidx)
{
	for (int i = 0; i < 2; ++i) {
		RenderNodeLabels(tiletree + i, skp, labelfont, fontidx);
	}
}

// -----------------------------------------------------------------------

// GCC requires an explicit specialisation to be DECLARED before the
// first use that would otherwise instantiate the primary template.
// CreateLabels and DeleteLabels below both call SetSubtreeLabels, and
// its specialisation is defined after them -- which MSVC accepts and
// GCC reports as "specialization after instantiation". The definitions
// keep the reference's order; only this declaration is new.
template<>
void TileManager2<SurfTile>::SetSubtreeLabels(QuadTreeNode<SurfTile> *node, bool activate);

template<>
void TileManager2<SurfTile>::CreateLabels()
{
	loader->WaitForMutex();
	for (int i = 0; i < 2; ++i) {
		SetSubtreeLabels(tiletree + i, true);
	}
	loader->ReleaseMutex();
}

// -----------------------------------------------------------------------

template<>
void TileManager2<SurfTile>::DeleteLabels()
{
	loader->WaitForMutex();
	for (int i = 0; i < 2; ++i) {
		SetSubtreeLabels(tiletree + i, false);
	}
	loader->ReleaseMutex();
}

// -----------------------------------------------------------------------

template<>
void TileManager2<SurfTile>::SetSubtreeLabels(QuadTreeNode<SurfTile> *node, bool activate)
{
	if (node->Entry()) {
		if (activate) node->Entry()->CreateLabels();
		else          node->Entry()->DeleteLabels();
	}
	for (int i = 0; i < 4; ++i) {
		if (node->Child(i)) {
			SetSubtreeLabels(node->Child(i), activate);
		}
	}
}

// -----------------------------------------------------------------------

template<>
void TileManager2<SurfTile>::LoadZTrees()
{
	treeMgr = new ZTreeMgr*[ntreeMgr = 5]();
	if (cprm.tileLoadFlags & 0x0002) {
		treeMgr[0] = ZTreeMgr::CreateFromFile(m_dataRootDir.c_str(), ZTreeMgr::LAYER_SURF);
		treeMgr[1] = ZTreeMgr::CreateFromFile(m_dataRootDir.c_str(), ZTreeMgr::LAYER_MASK);
		treeMgr[2] = ZTreeMgr::CreateFromFile(m_dataRootDir.c_str(), ZTreeMgr::LAYER_ELEV);
		treeMgr[3] = ZTreeMgr::CreateFromFile(m_dataRootDir.c_str(), ZTreeMgr::LAYER_ELEVMOD);
		treeMgr[4] = ZTreeMgr::CreateFromFile(m_dataRootDir.c_str(), ZTreeMgr::LAYER_LABEL);
	}
	else {
		for (int i = 0; i < ntreeMgr; i++)
			treeMgr[i] = 0;
	}
}

// -----------------------------------------------------------------------

template<>
void TileManager2<SurfTile>::InitHasIndividualFiles()
{
	hasIndividualFiles = new bool[ntreeMgr]();
	if (cprm.tileLoadFlags & 0x0001) {
		const char *name[] = { "Surf", "Mask", "Elev", "Elev_mod", "Label" };
		// `char dummy[MAX_PATH]` stood beside path and nothing reads it.
		char path[MAX_PATH];
		for (int i = 0; i < int(ARRAYSIZE(name)); ++i) {
			// "%s\\%s" on Windows. See point 5 in the file header.
			sprintf_s(path, MAX_PATH, "%s/%s", m_dataRootDir.c_str(), name[i]);
			hasIndividualFiles[i] = FileExists(path);
		}
	}
}

// -----------------------------------------------------------------------

template<>
Tile * TileManager2<SurfTile>::SearchTile (double lng, double lat, int maxlvl, bool bOwntex) const
{
	if (lng<0) return SearchTileSub(&tiletree[0], lng, lat, maxlvl, bOwntex);
	else	   return SearchTileSub(&tiletree[1], lng, lat, maxlvl, bOwntex);
}
// -----------------------------------------------------------------------

template<>
int TileManager2<SurfTile>::GetElevation(double lng, double lat, double *elev, FVECTOR3 *nrm, SurfTile **cache)
{
	int rv = 0;
	loader->WaitForMutex();
	if (lng<0) rv = tiletree[0].Entry()->GetElevation(lng, lat, elev, nrm, cache);
	else	   rv = tiletree[1].Entry()->GetElevation(lng, lat, elev, nrm, cache);
	loader->ReleaseMutex();
	return rv;
}

// -----------------------------------------------------------------------

template<>
void TileManager2<SurfTile>::Unload(int lvl)
{
	tiletree[0].DelAbove(lvl);
	tiletree[1].DelAbove(lvl);	
}

// -----------------------------------------------------------------------

template<>
void TileManager2<SurfTile>::Pick(FVECTOR3 &vRay, TILEPICK *pPick)
{
	std::list<Tile *> tiles;

	pPick->d = 1e12f;

	// Give me a list of rendered tiles ----------------------------------------------
	//
	QueryTiles(&tiletree[0], tiles);
	QueryTiles(&tiletree[1], tiles);

	for (auto tile : tiles)	tile->Pick(&(tile->mWorld), &vRay, *pPick);
}

// -----------------------------------------------------------------------

template<>
SURFHANDLE TileManager2<SurfTile>::SeekTileTexture(int iLng, int iLat, int level, int flags)
{
	bool bOk = false;
	VulkanTexture * pTex = NULL;
		
	if (flags & gcTileFlags::TEXTURE)
	{
		if (flags & gcTileFlags::CACHE)
		{
			char path[MAX_PATH];
			// "%s\\Surf\\..." on Windows. See point 5 in the file header.
			sprintf_s(path, MAX_PATH, "%s/Surf/%02d/%06d/%06d.dds", m_dataRootDir.c_str(), level + 4, iLat, iLng);
			// D3DXCreateTextureFromFileEx: open, read the DDS header, choose
			// a format, allocate, upload. See point 3 in the file header.
			pTex = LoadDDSFile(path);
			bOk = (pTex != NULL);
		}

		if (flags & gcTileFlags::TREE)
		{
			if (!bOk) {
				if (ZTreeManager(0)) {
					BYTE *buf;
					DWORD ndata = ZTreeManager(0)->ReadData(level + 4, iLat, iLng, &buf);
					if (ndata) {
						pTex = NatCreateTextureFromDDSInMemory(buf, size_t(ndata));
						if (pTex) bOk = true;
						ZTreeManager(0)->ReleaseData(buf);
					}
				}
			}
		}
	}

	if (flags & gcTileFlags::MASK)
	{
		if (flags & gcTileFlags::CACHE)
		{
			char path[MAX_PATH];
			sprintf_s(path, MAX_PATH, "%s/Mask/%02d/%06d/%06d.dds", m_dataRootDir.c_str(), level + 4, iLat, iLng);
			pTex = LoadDDSFile(path);
			bOk = (pTex != NULL);
		}

		if (flags & gcTileFlags::TREE)
		{
			if (!bOk) {
				if (ZTreeManager(1)) {
					BYTE* buf;
					DWORD ndata = ZTreeManager(1)->ReadData(level + 4, iLat, iLng, &buf);
					if (ndata) {
						pTex = NatCreateTextureFromDDSInMemory(buf, size_t(ndata));
						if (pTex) bOk = true;
						ZTreeManager(1)->ReleaseData(buf);
					}
				}
			}
		}
	}

	if (bOk && pTex) return new SurfNative(pTex, OAPISURFACE_TEXTURE);
	return NULL;
}

// -----------------------------------------------------------------------

template<>
bool TileManager2<SurfTile>::HasTileData(int iLng, int iLat, int level, int flags)
{
	bool bOk = false;
	
	if (flags & gcTileFlags::TEXTURE) {
		if (flags & gcTileFlags::CACHE) {
			char path[MAX_PATH];
			sprintf_s(path, MAX_PATH, "%s/Surf/%02d/%06d/%06d.dds", m_dataRootDir.c_str(), level + 4, iLat, iLng);
			bOk = FileExists(path);

		}
		if (flags & gcTileFlags::TREE) if (!bOk && ZTreeManager(0)) if (ZTreeManager(0)->Idx(level + 4, iLat, iLng) != ((DWORD)-1)) bOk = true;
	}

	if (flags & gcTileFlags::MASK) {
		if (flags & gcTileFlags::CACHE) {
			char path[MAX_PATH];
			sprintf_s(path, MAX_PATH, "%s/Mask/%02d/%06d/%06d.dds", m_dataRootDir.c_str(), level + 4, iLat, iLng);
			bOk = FileExists(path);
		}
		if (flags & gcTileFlags::TREE) if (!bOk && ZTreeManager(1)) if (ZTreeManager(1)->Idx(level + 4, iLat, iLng) != ((DWORD)-1)) bOk = true;
	}

	if (flags & gcTileFlags::ELEVATION) {
		if (flags & gcTileFlags::CACHE) {
			char path[MAX_PATH];
			sprintf_s(path, MAX_PATH, "%s/Elev/%02d/%06d/%06d.elv", m_dataRootDir.c_str(), level + 4, iLat, iLng);
			bOk = FileExists(path);
		}
		if (flags & gcTileFlags::TREE) if (!bOk && ZTreeManager(2)) if (ZTreeManager(2)->Idx(level + 4, iLat, iLng) != ((DWORD)-1)) bOk = true;
	}

	return bOk;
}

// -----------------------------------------------------------------------

template<>
float* TileManager2<SurfTile>::BrowseElevationData(int lvl, int ilat, int ilng, int flags, ELEVFILEHEADER* _hdr)
{
	ELEVFILEHEADER ehdr;
	const int ndat = TILE_ELEVSTRIDE * TILE_ELEVSTRIDE;
	float* elev = NULL;
	char path[MAX_PATH];
	char fname[128];
	FILE* f;
	int i;

	// Elevation data
	if (flags & gcTileFlags::CACHE) { // try loading from individual tile file
		sprintf_s(path, MAX_PATH, "%s/Elev/%02d/%06d/%06d.elv", m_dataRootDir.c_str(), lvl + 4, ilat, ilng);
		if (!fopen_s(&f, path, "rb")) {
			elev = new float[ndat];
			fread(&ehdr, sizeof(ELEVFILEHEADER), 1, f);
			if (ehdr.hdrsize != sizeof(ELEVFILEHEADER)) fseek(f, ehdr.hdrsize, SEEK_SET);

			ehdr.scale = 1.0;

			switch (ehdr.dtype) {
			case 0: // flat tile, defined by offset
				for (i = 0; i < ndat; i++) elev[i] = 0.0f;
				break;
			case 8: {
				UINT8* tmp = new UINT8[ndat];
				fread(tmp, sizeof(UINT8), ndat, f);
				for (i = 0; i < ndat; i++) elev[i] = float(tmp[i]);
				delete[]tmp;
				break;
			}
			case -16: {
				INT16* tmp = new INT16[ndat];
				fread(tmp, sizeof(INT16), ndat, f);
				for (i = 0; i < ndat; i++) elev[i] = float(tmp[i]);
				delete[]tmp;
				break;
			}
			}
			fclose(f);
		}
	}
	if (!elev && (flags & gcTileFlags::TREE) && ZTreeManager(2)) { // try loading from compressed archive
		BYTE* buf;
		DWORD ndata = ZTreeManager(2)->ReadData(lvl + 4, ilat, ilng, &buf);
		if (ndata) {
			BYTE* p = buf;
			elev = new float[ndat];
			memcpy(&ehdr, p, sizeof(ELEVFILEHEADER));
			p += ehdr.hdrsize;
			INT16* pi = (INT16*)p;
			switch (ehdr.dtype) {
			case 0:
				{ for (i = 0; i < ndat; i++) elev[i] = 0.0f; } break;
			case 8:
				{ for (i = 0; i < ndat; i++) elev[i] = float(*p++); } break;
			case -16:
				{ for (i = 0; i < ndat; i++) elev[i] = float(*pi++); } break;
			}
			ZTreeManager(2)->ReleaseData(buf);
		}
	}

	if (elev) {
		if (ehdr.scale != 1.0) for (i = 0; i < ndat; i++) elev[i] = trunc(elev[i] * float(ehdr.scale));
		if (ehdr.offset) for (i = 0; i < ndat; i++)	elev[i] += trunc(float(ehdr.offset));
	}


	// Elevation mod data
	if (elev && (flags & gcTileFlags::MOD)) {
		bool ok = false;
		ELEVFILEHEADER hdr;
		if (flags & gcTileFlags::CACHE) { // try loading from individual tile file
			// "%s\\Elev_mod\\..." on Windows. See point 5 in the file header.
			sprintf_s(fname, ARRAYSIZE(fname), "%s/Elev_mod/%02d/%06d/%06d.elv", CbodyName(), lvl + 4, ilat, ilng);
			bool found = GetClient()->TexturePath(fname, path);
			if (found && !fopen_s(&f, path, "rb")) {

				fread(&hdr, sizeof(ELEVFILEHEADER), 1, f);
				if (hdr.hdrsize != sizeof(ELEVFILEHEADER)) fseek(f, hdr.hdrsize, SEEK_SET);

				switch (hdr.dtype)
				{
				case 0: // overwrite the entire tile with a flat offset
					for (i = 0; i < ndat; i++) elev[i] = float(hdr.offset);
					break;

				case 8: {
					const UINT8 mask = UCHAR_MAX;
					UINT8* tmp = new UINT8[ndat];
					fread(tmp, sizeof(UINT8), ndat, f);
					for (i = 0; i < ndat; i++)
						if (tmp[i] != mask)
							elev[i] = float(trunc(float(tmp[i]) * hdr.scale) + trunc(hdr.offset));
					delete[]tmp;
					break;
				}
				case -16: {
					const INT16 mask = SHRT_MAX;
					INT16* tmp = new INT16[ndat];
					fread(tmp, sizeof(INT16), ndat, f);
					for (i = 0; i < ndat; i++)
						if (tmp[i] != mask)
							elev[i] = float(trunc(float(tmp[i]) * hdr.scale) + trunc(hdr.offset));
					delete[]tmp;
					break;
				}
				}
				fclose(f);
				ok = true;
			}
		}

		if (!ok && (flags & gcTileFlags::TREE) && ZTreeManager(3)) { // try loading from compressed archive
			BYTE* buf;
			DWORD ndata = ZTreeManager(3)->ReadData(lvl + 4, ilat, ilng, &buf);
			if (ndata) {
				BYTE* p = buf;
				ELEVFILEHEADER* phdr = (ELEVFILEHEADER*)p;
				p += phdr->hdrsize;

				switch (phdr->dtype)
				{
				case 0:
					for (i = 0; i < ndat; i++) elev[i] = float(phdr->offset);
					break;
				case 8: {
					const UINT8 mask = UCHAR_MAX;
					for (i = 0; i < ndat; i++)
						if (p[i] != mask)
							elev[i] = float(trunc(float(p[i]) * phdr->scale) + trunc(phdr->offset));
					break;
				}
				case -16: {
					const INT16 mask = SHRT_MAX;
					INT16* buf16 = (INT16*)p;
					for (i = 0; i < ndat; i++)
						if (buf16[i] != mask)
							elev[i] = float(trunc(float(buf16[i]) * phdr->scale) + trunc(phdr->offset));
					break;
				}
				}
				ZTreeManager(3)->ReleaseData(buf);
			}
		}
	}

	if (_hdr) memcpy(_hdr, &ehdr, sizeof(ELEVFILEHEADER));

	return elev;
}
