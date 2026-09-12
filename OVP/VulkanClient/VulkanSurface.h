// ==============================================================
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2012-2026 Jarmo Nikkanen
// ==============================================================
//
// D3D9 has two resource types where Vulkan has one. IDirect3DSurface9 and
// IDirect3DTexture9 are distinct interfaces, and SurfNative stored a
// D3DRESOURCETYPE to branch on; a VkImage is a texture if it was created with
// VK_IMAGE_USAGE_SAMPLED_BIT and a render target if it was created with
// VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, and can be both at once. So 'type' and
// the pTexSurf mip-0 cache are gone, and every branch on them collapses to one
// path.
//
// D3DSURFACE_DESC became VulkanImageDesc (VulkanTypes.h), which the client
// fills in rather than reads back -- a VkImage remembers nothing you can query.
//
// pDX7, CreateDX7() and DX7Sync() are gone. They existed because
// IDirect3DSurface9::GetDC() does not work on a render target, so the code made
// a lockable X8R8G8B8 copy, StretchRect'd the render target into it, took a GDI
// DC on that, and blitted back on release. GetDC() here returns a recording DC
// from Src/Orbiter/Linux/Gdi.cpp, a display-list recorder not tied to any
// image, so there is nothing left to work around.
// ==============================================================

#ifndef __VULKANSURFACE_H
#define __VULKANSURFACE_H

#include "OrbiterAPI.h"
#include "VulkanClient.h"
#include <vulkan/vulkan.h>
#include "VulkanTypes.h"

class VulkanPad;
class GDIPad;

#define	MAP_NORMAL			0
#define	MAP_SPECULAR		1
#define	MAP_EMISSION		2
#define	MAP_REFLECTION		3
#define	MAP_TRANSLUCENCE	4
#define	MAP_TRANSMITTANCE	5
#define	MAP_ROUGHNESS		6
#define	MAP_METALNESS		7
#define	MAP_HEAT			8
#define MAP_MAX_COUNT		9

#define OAPISURFACE_MAPS		0x80000000		// Additional Texture Maps
#define OAPISURFACE_BACKBUFFER	0x40000000		// It's a backbuffer
#define OAPISURFACE_ORIGIN		0x20000000		// The origin from where the clones are being made, can't change (immutable)
#define OAPISURFACE_CAPTURE		0x10000000		// The origin from where the clones are being made, can't change (immutable)

#define OAPISURF_SKP_GDI_WARN	0x00000001

VulkanTexture *		NatLoadSpecialTexture(const char* fname, const char* ext);
// Counterpart of D3DXCreateTextureFromFileA: decode a file into a texture,
// DDS or not, with no SurfNative around it.
VulkanTexture *		NatLoadTexture(const char* path);
SURFHANDLE			NatLoadSurface(const char* file, DWORD flags, bool bPath = false);
bool				NatSaveSurface(const char* file, VulkanTexture *pResource);
SURFHANDLE			NatCreateSurface(int width, int height, DWORD flags);
SURFHANDLE			NatGetMipSublevel(SURFHANDLE hSrf, int level);
bool				NatGenerateMipmaps(SURFHANDLE hSrf);
SURFHANDLE			NatCompressSurface(SURFHANDLE hSurface, DWORD flags);
bool				NatCreateName(char* out, int mlen, const char* fname, const char* id);

// ------------------------------------------------------------------------------------
// DDS decoding. D3DXGetImageInfoFromFileA, D3DXCreateTextureFromFileExA and
// D3DXCreateTextureFromFileInMemoryEx between them read the header, chose a
// format and uploaded every mip level. There is no D3DX here, so the client
// reads DDS itself. NatDDSImageBytes only measures the image starting at
// `data`; it does not decode. VulkanUtil.cpp's LoadPlanetTextures needs that to
// walk a file of several DDS images laid end to end.
// ------------------------------------------------------------------------------------
long				NatDDSImageBytes(const void* data, long bytesAvailable);
// bFullMipChain is D3DXCreateTextureFromFileInMemoryEx's `MipLevels = 0,
// Filter = D3DX_FILTER_BOX` pair: generate the levels the file does not carry.
VulkanTexture *		NatCreateTextureFromDDSInMemory(const void* data, size_t bytes,
													bool bFullMipChain = false);
// Counterpart of D3DXLoadSurfaceFromFileInMemory for the non-DDS formats.
VulkanTexture *		NatCreateTextureFromMemory(const void* data, size_t bytes,
											   uint32_t *pW = NULL, uint32_t *pH = NULL);

// The OAPI -> VK direction is exact; VK -> OAPI cannot be. D3DFMT_X8R8G8B8 and
// D3DFMT_A8R8G8B8 are one Vulkan format, VK_FORMAT_B8G8R8A8_UNORM, because "X8"
// only meant "alpha present, ignored" and Vulkan does not record that. The
// reverse mapping reports the ARGB form; whether alpha is meaningful comes from
// the surface's own OAPISURFACE_ALPHA / OAPISURFACE_NOALPHA flag.
DWORD				NatConvertFormat_VK_to_OAPI(VkFormat Format);
VkFormat			NatConvertFormat_OAPI_to_VK(DWORD Format);

const char*			NatUsage(VkImageUsageFlags Usage);
const char*			NatMemory(bool bHostVisible);		// was NatPool(D3DPOOL)
const char*			NatOAPIFlags(DWORD AF);
const char*			NatOAPIFormat(DWORD PF);
void				NatDumpResource(VulkanTexture *pResource);


#define ERR_DC_NOT_AVAILABLE		0x1
#define ERR_USED_NOT_DEFINED		0x2


// Every SURFHANDLE in the client is a pointer into the SurfNative class

class SurfNative
{
	friend class oapi::VulkanClient;
	friend class VulkanPad;
	friend class GDIPad;

	struct _HDC_LOCAL {
		HDC hDC;
		VulkanTexture *pSrf;
	};

public:

							SurfNative(VulkanTexture *pSrf, DWORD Flags, VulkanTexture *pDep = NULL);
							SurfNative(SurfNative* hOrigin);
							~SurfNative();

	void					AddMap(DWORD id, VulkanTexture *pMap);
	const VulkanImageDesc*	GetDesc() const { return &desc; }
	bool					GenerateMipMaps();
	bool					Decompress();
	VulkanTexture *			GetGDICache(DWORD Flags);
	void					IncRef() { RefCount++; }
	bool					DecRef() { RefCount--; return RefCount <= 0; }
	bool					DeClone();
	bool					GetSpecs(gcCore::SurfaceSpecs* sp, int size);

	void					Reload();

	DWORD					GetMipMaps() const { return Mipmaps; }
	DWORD					GetWidth() const { return desc.Width; }
	DWORD					GetHeight() const { return desc.Height; }
	DWORD					GetOAPIFlags() const { return Flags; }
	DWORD					GetSizeInBytes();
	DWORD*					GetClientFlags();

	const char*				GetName() const { return name; }
	void					SetName(const char*);
	HDC						GetDC();
	void					ReleaseDC(HDC);

	// Was desc.Pool == D3DPOOL_SYSTEMMEM || desc.Usage & D3DUSAGE_DYNAMIC.
	// Both meant "the CPU can reach this", which is one property in Vulkan.
	bool					IsGDISurface() const { return desc.HostVisible; }
	bool					IsCompressed() const;
	bool					IsBackBuffer() const { return (Flags & OAPISURFACE_BACKBUFFER) != 0; }

	// Was type == D3DRTYPE_TEXTURE. The question was always "can this be
	// sampled by a shader", which in Vulkan is a usage bit rather than a type.
	bool					IsTexture() const { return (desc.Usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0; }
	bool					IsRenderTarget() const;
	bool					Is3DRenderTarget() const { return (pDepth != NULL) && ((Flags & OAPISURFACE_RENDER3D) != 0); }
	bool					IsPowerOfTwo() const;
	bool					IsSystemMem() const { return desc.HostVisible; }
	bool					IsAdvanced() const { return (Flags & OAPISURFACE_MAPS); }
	bool					IsColorKeyEnabled() const { return (ColorKey != SURF_NO_CK); }
	bool					IsClone() const { return hOrigin != this; }

	VulkanTexture *			GetTempSurface();
	VulkanTexture *			GetResource() const { return pResource; }
	VulkanTexture *			GetDepthStencil() const { return pDepth; }

	// Different things in D3D9: GetSurface() had to fetch mip level 0 to get a
	// bindable render target out of a texture. A VkImage is both, so both
	// return the resource; the two names survive because call sites spell one
	// or the other.
	VulkanTexture *			GetSurface() { return pResource; }
	VulkanTexture *			GetTexture() const { return IsTexture() ? pResource : NULL; }

	VulkanTexture *			GetMap(int type) const { return pMap[type]; }
	VulkanTexture *			GetMap(int type, int type2) const { return (pMap[type] ? pMap[type] : pMap[type2]); }
	VulkanPad*				GetPooledSketchPad();
	void					SetColorKey(DWORD ck);			// Enable and set color key
	DWORD					GetColorKey() const { return ColorKey; }

	bool					Fill(LPRECT r, DWORD color);

	DWORD					GetTextureSizeInBytes(VulkanTexture *pT);
	DWORD					GetFormatSizeInBytes(VkFormat Format, DWORD pixels);

	// The same table, reachable without a SurfNative: GetFormatSizeInBytes is a
	// non-static member on Windows though it uses no member state, and callers
	// holding only a VulkanTexture need the answer. The member forwards here.
	static DWORD			StaticFormatSizeInBytes(VkFormat Format, DWORD pixels);

	void					LogSpecs() const;

	// -------------------------------------------------------------------------------

	char					name[128];				// Surface name
	SURFHANDLE				hOrigin;
	VulkanImageDesc			desc;					// Surface size and format description
	VulkanTexture *			pGDICache;				// Low level GDI cache for surface syncing
	VulkanTexture *			pTemp;					// Cache for in-surface blitting
	VulkanTexture *			pDepth;					// DepthStencil image for 3D rendering
	VulkanTexture *			pResource;				// Main resource
	VulkanTexture *			pMap[MAP_MAX_COUNT];	// Additional texture maps _norm, _rghn, _spec, etc...
	VulkanDevice *			pDevice;
	DWORD					ColorKey;
	DWORD					Flags;					// Surface Flags/Attribs
	DWORD					Mipmaps;				// Mipmap count. 1 = no mipmaps
	DWORD					ClientFlags;
	int						RefCount;
	VulkanPad*				pSkp;					// Pooled sketchpad interface cache
	_HDC_LOCAL				DC;
};


#endif
