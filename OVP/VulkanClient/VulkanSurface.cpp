// ===========================================================================================
// VulkanSurface.cpp
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2011-2026 Jarmo Nikkanen
// ===========================================================================================
//
// CONVERTED FROM OVP/D3D9Client/D3D9Surface.cpp, read end to end (1321 lines).
//
// This is the file where D3DX did the most work for the client, so it is the
// file with the most genuinely new code. Five D3DX entry points carried the
// whole image pipeline:
//
//   D3DXGetImageInfoFromFileA          read size/format/mips without decoding
//   D3DXCreateTextureFromFileExA       decode + upload + optional mip build
//   D3DXCreateTextureFromFileInMemoryEx  the same from a memory blob
//   D3DXLoadSurfaceFromFile/Surface    decode or rescale into a surface
//   D3DXSaveTextureToFileA/SurfaceToFileA  encode
//
// None exists here, so the client reads and writes images itself:
//
//   DDS   -- parsed below. It is the format Orbiter ships planet and vessel
//            textures in, and the one D3DX was doing the least for: a DDS
//            file is a header plus already-compressed blocks, so "decoding"
//            is reading the header and handing the blocks to the GPU.
//   PNG /
//   JPG /
//   BMP   -- stb_image, which is exactly what Src/Orbiter/Linux/WinCodec.cpp
//            already uses for the core's WIC replacement. ImGui vendors it,
//            so this introduces no new dependency; see the note on
//            STB_IMAGE_IMPLEMENTATION below.
//
// THE OTHER STRUCTURAL CHANGE is the one VulkanSurface.h describes: D3D9 had
// two resource types and this file branched on D3DRESOURCETYPE in seven
// places (NatSaveSurface, NatCompressSurface, the constructor, GetTexture,
// GetSurface, GenerateMipMaps, GetSizeInBytes, NatDumpResource). Vulkan has
// one image type, so every one of those branches collapses.
//
// pTexSurf, pDX7, CreateDX7() and DX7Sync() are gone -- see VulkanSurface.h.
// ===========================================================================================

#define STRICT

#include "VulkanSurface.h"
#include "VulkanClient.h"
#include "VulkanConfig.h"
#include "VulkanCatalog.h"
#include "VulkanUtil.h"
#include "AABBUtil.h"
#include "Log.h"
#include "VulkanPad.h"   // GetPooledSketchPad constructs one
#include <vector>
#include <algorithm>

// stb_image, as Src/Orbiter/Linux/WinCodec.cpp uses it for the core. The
// implementation is defined in exactly one translation unit per module, and
// this is the client's -- the core has its own copy in its own module, which
// is correct: they are separate shared objects.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#include "stb_image.h"
#include "stb_image_write.h"    // NatSaveSurface; replaces D3DXSaveTextureToFileA

// stb_dxt, from the same collection the CMakeLists already links as stb::stb.
// It is here for one reason, spelled out at NatBuildMipChain below:
// D3DXCreateTextureFromFileInMemoryEx(..., MipLevels = 0, ..., D3DX_FILTER_BOX,
// ...) built the whole mip chain of a BLOCK-COMPRESSED texture, decompressing,
// filtering and recompressing behind that one argument. No Vulkan call can
// write BC blocks -- vkCmdBlitImage refuses a compressed destination -- so the
// recompression is the one part of that argument that has to be done here.
#define STB_DXT_IMPLEMENTATION
#include "stb_dxt.h"

using namespace oapi;

extern VulkanClient* g_client;


// ===========================================================================================
// FORMAT CONVERSION
//
// See VulkanSurface.h for why the VK -> OAPI direction cannot be exact.
// The trap in this table is the byte order: a D3DFMT_A8R8G8B8 pixel is the
// DWORD 0xAARRGGBB, whose bytes little-endian run B,G,R,A -- so it is
// VK_FORMAT_B8G8R8A8_UNORM, not R8G8B8A8. Getting it backwards swaps red and
// blue in every surface in the client, which looks like an art bug.
// ===========================================================================================

VkFormat NatConvertFormat_OAPI_to_VK(DWORD Format)
{
	Format &= OAPISURFACE_PF_MASK;

	if (Format == OAPISURFACE_PF_XRGB) return VK_FORMAT_B8G8R8A8_UNORM;
	if (Format == OAPISURFACE_PF_ARGB) return VK_FORMAT_B8G8R8A8_UNORM;
	if (Format == OAPISURFACE_PF_RGB565) return VK_FORMAT_R5G6B5_UNORM_PACK16;
	if (Format == OAPISURFACE_PF_S16R) return VK_FORMAT_R16_UNORM;
	if (Format == OAPISURFACE_PF_F16R) return VK_FORMAT_R16_SFLOAT;
	if (Format == OAPISURFACE_PF_F16RG) return VK_FORMAT_R16G16_SFLOAT;
	if (Format == OAPISURFACE_PF_F32R) return VK_FORMAT_R32_SFLOAT;
	if (Format == OAPISURFACE_PF_F32RG) return VK_FORMAT_R32G32_SFLOAT;
	if (Format == OAPISURFACE_PF_DXT1) return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
	if (Format == OAPISURFACE_PF_F32RGBA) return VK_FORMAT_R32G32B32A32_SFLOAT;
	if (Format == OAPISURFACE_PF_F16RGBA) return VK_FORMAT_R16G16B16A16_SFLOAT;
	if (Format == OAPISURFACE_PF_DXT3) return VK_FORMAT_BC2_UNORM_BLOCK;
	if (Format == OAPISURFACE_PF_DXT5) return VK_FORMAT_BC3_UNORM_BLOCK;
	// D3DFMT_L8 replicated its single channel across RGB via a fixed swizzle.
	// VK_FORMAT_R8_UNORM reads (r,0,0,1); the GLSL translations that sample a
	// grayscale map take .r, which is what the HLSL got from the swizzle too.
	if (Format == OAPISURFACE_PF_GRAY) return VK_FORMAT_R8_UNORM;
	// D3DFMT_A8 put its channel in alpha rather than red. Same storage, and
	// the shader reads .r here where it read .a there.
	if (Format == OAPISURFACE_PF_ALPHA) return VK_FORMAT_R8_UNORM;
	return VK_FORMAT_UNDEFINED;
}


DWORD NatConvertFormat_VK_to_OAPI(VkFormat Format)
{
	DWORD Out = OAPISURFACE_NOALPHA;
	if (Format == VK_FORMAT_R5G6B5_UNORM_PACK16) return Out | OAPISURFACE_PF_RGB565;
	if (Format == VK_FORMAT_R16_UNORM) return Out | OAPISURFACE_PF_S16R;
	if (Format == VK_FORMAT_R16_SFLOAT) return Out | OAPISURFACE_PF_F16R;
	if (Format == VK_FORMAT_R16G16_SFLOAT) return Out | OAPISURFACE_PF_F16RG;
	if (Format == VK_FORMAT_R32_SFLOAT) return Out | OAPISURFACE_PF_F32R;
	if (Format == VK_FORMAT_R32G32_SFLOAT) return Out | OAPISURFACE_PF_F32RG;
	if (Format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK) return Out | OAPISURFACE_PF_DXT1;
	if (Format == VK_FORMAT_R8_UNORM) return Out | OAPISURFACE_PF_GRAY;

	Out = OAPISURFACE_ALPHA;
	if (Format == VK_FORMAT_R32G32B32A32_SFLOAT) return Out | OAPISURFACE_PF_F32RGBA;
	if (Format == VK_FORMAT_R16G16B16A16_SFLOAT) return Out | OAPISURFACE_PF_F16RGBA;
	// X8R8G8B8 and A8R8G8B8 are the same Vulkan format; this reports the
	// alpha-bearing one, and the caller's OAPISURFACE_NOALPHA flag says
	// whether that alpha means anything. See VulkanSurface.h.
	if (Format == VK_FORMAT_B8G8R8A8_UNORM) return Out | OAPISURFACE_PF_ARGB;
	if (Format == VK_FORMAT_BC2_UNORM_BLOCK) return Out | OAPISURFACE_PF_DXT3;
	if (Format == VK_FORMAT_BC3_UNORM_BLOCK) return Out | OAPISURFACE_PF_DXT5;
	return 0;
}


// ===========================================================================================
// DDS
//
// The reference never parsed DDS itself except in LoadPlanetTextures, where it
// had to walk a file of several images and therefore had to know how long each
// one was -- and it declared its own DDSURFACEDESC2_x64 for exactly that,
// because the SDK struct changes size between 32- and 64-bit builds.
//
// This describes the DDS FILE FORMAT instead, whose field offsets are fixed by
// the format and not by any SDK. The fields below are the ones the reference
// reads: dwFlags, dwHeight, dwWidth, dwPitchOrLinearSize, dwMipMapCount, and
// out of the pixel format the FourCC and dwRGBBitCount.
// ===========================================================================================

#define DDSD_PITCH			0x00000008
#define DDSD_LINEARSIZE		0x00080000
#define DDSD_MIPMAPCOUNT	0x00020000
#define DDPF_FOURCC			0x00000004

#pragma pack(push, 1)
typedef struct {
	DWORD dwSize, dwFlags, dwHeight, dwWidth, dwPitchOrLinearSize;
	DWORD dwDepth, dwMipMapCount, dwReserved1[11];
	struct {
		DWORD dwSize, dwFlags, dwFourCC;
		DWORD dwRGBBitCount, dwRBitMask, dwGBitMask, dwBBitMask, dwABitMask;
	} ddspf;
	DWORD dwCaps, dwCaps2, dwCaps3, dwCaps4, dwReserved2;
} NatDDSHeader;
#pragma pack(pop)

static_assert(sizeof(NatDDSHeader) == 124, "DDS header is 124 bytes by the file format");


// Is this header D3DFMT_A4R4G4B4 or D3DFMT_X4R4G4B4? Declared here because
// NatDDSFormat asks the question and the loader asks it again -- the bytes on
// disk are two per texel where the format NatDDSFormat reports declares four,
// so every length and every copy has to know, exactly as for 24-bit.
//
// X4R4G4B4 is the same layout with no alpha mask; the reference sends it to
// X8R8G8B8, so the widening loop writes 255 when dwABitMask is 0.
static bool NatDDSIs4444(const NatDDSHeader *h)
{
	return !(h->ddspf.dwFlags & DDPF_FOURCC)
		&& h->ddspf.dwRGBBitCount == 16
		&& h->ddspf.dwRBitMask == 0x0F00
		&& h->ddspf.dwGBitMask == 0x00F0
		&& h->ddspf.dwBBitMask == 0x000F
		&& (h->ddspf.dwABitMask == 0xF000 || h->ddspf.dwABitMask == 0);
}


static VkFormat NatDDSFormat(const NatDDSHeader *h)
{
	if (h->ddspf.dwFlags & DDPF_FOURCC) {
		if (h->ddspf.dwFourCC == MAKEFOURCC('D','X','T','1')) return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
		if (h->ddspf.dwFourCC == MAKEFOURCC('D','X','T','3')) return VK_FORMAT_BC2_UNORM_BLOCK;
		if (h->ddspf.dwFourCC == MAKEFOURCC('D','X','T','5')) return VK_FORMAT_BC3_UNORM_BLOCK;
		return VK_FORMAT_UNDEFINED;
	}
	if (h->ddspf.dwRGBBitCount == 32) return VK_FORMAT_B8G8R8A8_UNORM;
	// 24-BIT UNCOMPRESSED, WHICH ORBITER SHIPS AND THIS DID NOT DECODE.
	//
	// D3DFMT_R8G8B8 is a valid DDS pixel format and four of the textures in
	// the stock scenarios use it (512x512 and 1024x1024, flags=DDPF_RGB,
	// bits=24, Rmask=00FF0000 Gmask=0000FF00 Bmask=000000FF). D3DX loaded
	// them by EXPANDING to X8R8G8B8 -- Direct3D 9 could not sample a 24-bit
	// surface either -- and returning that.
	//
	// Vulkan is in the same position: VK_FORMAT_B8G8R8_UNORM exists but is
	// not required to be sampleable and is unsupported on this driver, so the
	// answer is the same one D3DX gave. The format reported here is the
	// 32-bit one and the loader below widens each row, three bytes to four,
	// alpha 255. Reported as "unsupported" it silently became the white
	// fallback instead.
	if (h->ddspf.dwRGBBitCount == 24) return VK_FORMAT_B8G8R8A8_UNORM;

	// 16-BIT UNCOMPRESSED IS NOT ONE FORMAT, AND THIS TREATED IT AS R5G6B5.
	//
	// The line here used to be
	//
	//     if (h->ddspf.dwRGBBitCount == 16) return VK_FORMAT_R5G6B5_UNORM_PACK16;
	//
	// with no look at the channel masks. Every 16-bit DDS this installation
	// ships is D3DFMT_A4R4G4B4 (Rmask=00000F00 Gmask=000000F0 Bmask=0000000F
	// Amask=0000F000) -- fourteen files, counted, and not one R5G6B5 among
	// them: Cockpit\hud*.dds, Cockpit\Glasspit*.dds, Common\adiball_*.dds,
	// font1tex.dds, main_menu*.dds, ShuttleA\panel_el.dds. Handing those
	// bytes to the sampler as R5G6B5 reinterprets the texel: the alpha nibble
	// lands in the top five bits and becomes RED, and the alpha channel is
	// gone. Measured on Cockpit\hud.dds, whose every texel is R=5 G=E B=5
	// with the shape carried entirely in alpha, so the word is 0x?5E5:
	//
	//     alpha nibble 0 -> 0x05E5 -> R5G6B5 (0,190,41)   <- transparent
	//     alpha nibble F -> 0xF5E5 -> R5G6B5 (247,190,41) <- opaque
	//
	// and those are exactly the two colours the 2D-panel and glass-cockpit
	// HUD came out as -- a solid green bar with an orange edge where the
	// reference draws a thin line, because nothing was ever transparent.
	//
	// D3D9Surface.cpp:165 does this under the heading "File Formats Not
	// Supported":
	//
	//     if (info.Format == D3DFMT_A4R4G4B4) Format = D3DFMT_A8R8G8B8;
	//     if (info.Format == D3DFMT_X4R4G4B4) Format = D3DFMT_X8R8G8B8;
	//
	// -- Direct3D 9 could sample A4R4G4B4 and the client asked D3DX to widen
	// it anyway. So the answer is the reference's answer: report the 32-bit
	// format and widen each level on the way in, the same shape as the
	// 24-bit case above. NatDDSIs4444 below is what tells the loader to.
	//
	// A genuine R5G6B5 file still maps to R5G6B5. 1-5-5-5 is deliberately NOT
	// mapped: no file here uses it, the reference left it to Direct3D rather
	// than converting it, and an untested mapping that silently mis-renders
	// is the failure being fixed. It falls through to UNDEFINED, which logs
	// its masks and says so.
	if (h->ddspf.dwRGBBitCount == 16) {
		if (NatDDSIs4444(h)) return VK_FORMAT_B8G8R8A8_UNORM;
		if (h->ddspf.dwRBitMask == 0xF800 && h->ddspf.dwGBitMask == 0x07E0 &&
			h->ddspf.dwBBitMask == 0x001F) return VK_FORMAT_R5G6B5_UNORM_PACK16;
		return VK_FORMAT_UNDEFINED;
	}

	if (h->ddspf.dwRGBBitCount == 8) return VK_FORMAT_R8_UNORM;
	return VK_FORMAT_UNDEFINED;
}


// Is this header a 24-bit uncompressed image? The bytes on disk are three per
// texel where the format above declares four, so every length and every copy
// has to know. See NatDDSFormat.
static bool NatDDSIs24Bit(const NatDDSHeader *h)
{
	return !(h->ddspf.dwFlags & DDPF_FOURCC) && h->ddspf.dwRGBBitCount == 24;
}


// Bytes of one mip level. A block-compressed level is measured in 4x4 blocks
// and never smaller than one block, which is the rule that makes a 1x1 DXT1
// level 8 bytes rather than 0 -- getting it wrong walks the mip chain off the
// end of a small texture.
static size_t NatLevelBytes(VkFormat fmt, uint32_t w, uint32_t h)
{
	switch (fmt) {
	case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
		return size_t(std::max(1u, (w + 3) / 4)) * std::max(1u, (h + 3) / 4) * 8;
	case VK_FORMAT_BC2_UNORM_BLOCK:
	case VK_FORMAT_BC3_UNORM_BLOCK:
		return size_t(std::max(1u, (w + 3) / 4)) * std::max(1u, (h + 3) / 4) * 16;
	default:
		return size_t(w) * h * SurfNative::StaticFormatSizeInBytes(fmt, 1);
	}
}


// ===========================================================================================
// How long is the DDS image that starts here?
//
// This is the walk LoadPlanetTextures performs, lifted out so it is written
// once. The reference's correction is preserved exactly: a header that
// declares NEITHER DDSD_LINEARSIZE NOR DDSD_PITCH gets a linear size computed
// from its dimensions, with DXT1 at half a byte per pixel and DXT3/DXT5 at
// one. That correction exists because such headers are in the wild and the
// walk cannot proceed without a length.
//
// The reference then adds sizeof(Magic) + sizeof(DDSURFACEDESC2_x64) and does
// NOT add the mip levels -- Orbiter's tile archives store one level per
// image. That is kept: adding a mip chain here would step past the next image.
// ===========================================================================================
long NatDDSImageBytes(const void* data, long bytesAvailable)
{
	if (!data || bytesAvailable < long(4 + sizeof(NatDDSHeader))) return 0;

	const char *p = (const char*)data;
	DWORD magic = *(const DWORD*)p;
	if (magic != MAKEFOURCC('D','D','S',' ')) return 0;

	NatDDSHeader h = *(const NatDDSHeader*)(p + 4);

	if ((h.dwFlags & DDSD_LINEARSIZE) == 0 && (h.dwFlags & DDSD_PITCH) == 0) {
		h.dwFlags |= DDSD_LINEARSIZE;
		if (h.ddspf.dwFourCC == MAKEFOURCC('D','X','T','5')) h.dwPitchOrLinearSize = h.dwHeight * h.dwWidth;
		else if (h.ddspf.dwFourCC == MAKEFOURCC('D','X','T','3')) h.dwPitchOrLinearSize = h.dwHeight * h.dwWidth;
		else if (h.ddspf.dwFourCC == MAKEFOURCC('D','X','T','1')) h.dwPitchOrLinearSize = h.dwHeight * h.dwWidth / 2;
		else h.dwPitchOrLinearSize = h.dwHeight * h.dwWidth * h.ddspf.dwRGBBitCount / 8;
	}

	long bytes = (h.dwFlags & DDSD_LINEARSIZE)
		? long(h.dwPitchOrLinearSize)
		: long(h.dwHeight * h.dwWidth * h.ddspf.dwRGBBitCount / 8);

	bytes += long(4 + sizeof(NatDDSHeader));

	if (bytes <= 0 || bytes > bytesAvailable) return 0;
	return bytes;
}


// ===========================================================================================
// BUILD THE MIP CHAIN THAT D3DX BUILT
//
// Tilemgr2.cpp's LoadTextureFile and LoadTextureFromMemory asked D3DX for
//
//     D3DXCreateTextureFromFileInMemoryEx(..., MipLevels, ..., Filter, ...)
//     DWORD Mips = 1, Filter = D3DX_FILTER_NONE;
//     if (bMipmaps) Filter = D3DX_FILTER_BOX, Mips = 0;
//
// and Mips = 0 means "the complete chain", GENERATED with a box filter where
// the file does not carry it. Orbiter's tile files never carry it: every tile
// in Textures/<planet>/Surf, /Mask and /Cloud, and every image inside the .tree
// archives, is a single-level DXT1 or DXT5 image -- which is why NatDDSImageBytes
// above walks from one image to the next without adding a chain.
//
// The destination those levels are copied into is not this texture but the tile
// pool's, and VulkanCatalog.h's Texmgr::Alloc creates that with
//
//     UINT Mips = (Config->TileMipmaps == 1) ? 6 : 1;
//
// -- six levels, the reference's number. So with a one-level source, five of
// the six levels of every tile texture in the cache are never written, and what
// the sampler reads from them is whatever the pool's last tenant left there or
// nothing at all. That is visible as tile-shaped patches that go dark or wrong
// as the camera moves and the hardware picks a different mip: measured on the
// Earth cloud layer, where the selected level read as zero alpha over whole
// tiles and forcing textureLod(..., 0) in CloudPS made the patch vanish.
//
// vkCmdBlitImage, which VulkanDevice::GenerateMipmaps uses, CANNOT write a
// block-compressed image -- no Vulkan call can -- so the chain for a BC texture
// has to be built on the CPU: decompress, box-filter, recompress. That is the
// one thing D3DX did here that has no counterpart, and it is what these three
// helpers are. The recompressor is stb_dxt, from the stb collection this module
// already links; the decompressor is the BC block layout, which is a format
// definition rather than a choice.
// ===========================================================================================

static bool NatIsBC(VkFormat f)
{
	return f == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ||
		   f == VK_FORMAT_BC2_UNORM_BLOCK ||
		   f == VK_FORMAT_BC3_UNORM_BLOCK;
}


// One 4x4 block -> 16 RGBA texels. The colour half is identical in all three
// formats; only the alpha half differs. Note that BC2 and BC3 always use the
// four-colour interpretation, whatever the order of c0 and c1 -- the
// three-colour-plus-transparent form belongs to BC1 alone.
static void NatBCDecodeBlock(VkFormat fmt, const unsigned char *blk, unsigned char *out)
{
	const bool bAlphaBlock = (fmt != VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
	const unsigned char *c = bAlphaBlock ? blk + 8 : blk;

	const unsigned c0 = unsigned(c[0]) | (unsigned(c[1]) << 8);
	const unsigned c1 = unsigned(c[2]) | (unsigned(c[3]) << 8);
	const unsigned bits = unsigned(c[4]) | (unsigned(c[5]) << 8) |
						  (unsigned(c[6]) << 16) | (unsigned(c[7]) << 24);

	unsigned char r[4], g[4], b[4], a[4];
	// 5-6-5 -> 8-8-8. Same rounding rule as the 4-bit expansion above:
	// (n*255 + half) / max, never a shift, or white stops being white.
	const unsigned c65[2] = { c0, c1 };
	for (int i = 0; i < 2; i++) {
		r[i] = (unsigned char)(((( c65[i] >> 11) & 0x1F) * 255 + 15) / 31);
		g[i] = (unsigned char)(((( c65[i] >>  5) & 0x3F) * 255 + 31) / 63);
		b[i] = (unsigned char)(((  c65[i]        & 0x1F) * 255 + 15) / 31);
		a[i] = 255;
	}

	if (bAlphaBlock || c0 > c1) {
		r[2] = (unsigned char)((2 * r[0] + r[1]) / 3);
		g[2] = (unsigned char)((2 * g[0] + g[1]) / 3);
		b[2] = (unsigned char)((2 * b[0] + b[1]) / 3);
		a[2] = 255;
		r[3] = (unsigned char)((r[0] + 2 * r[1]) / 3);
		g[3] = (unsigned char)((g[0] + 2 * g[1]) / 3);
		b[3] = (unsigned char)((b[0] + 2 * b[1]) / 3);
		a[3] = 255;
	}
	else {
		r[2] = (unsigned char)((r[0] + r[1]) / 2);
		g[2] = (unsigned char)((g[0] + g[1]) / 2);
		b[2] = (unsigned char)((b[0] + b[1]) / 2);
		a[2] = 255;
		r[3] = g[3] = b[3] = 0;
		a[3] = 0;					// BC1's one bit of alpha
	}

	for (int i = 0; i < 16; i++) {
		const unsigned k = (bits >> (2 * i)) & 3;
		out[i * 4 + 0] = r[k];
		out[i * 4 + 1] = g[k];
		out[i * 4 + 2] = b[k];
		out[i * 4 + 3] = a[k];
	}

	if (fmt == VK_FORMAT_BC2_UNORM_BLOCK) {
		// Eight bytes of straight 4-bit alpha, two texels per byte, low
		// nibble first.
		for (int i = 0; i < 16; i++) {
			const unsigned n = (i & 1) ? (blk[i >> 1] >> 4) : (blk[i >> 1] & 0xF);
			out[i * 4 + 3] = (unsigned char)((n * 255 + 7) / 15);
		}
	}
	else if (fmt == VK_FORMAT_BC3_UNORM_BLOCK) {
		unsigned char av[8];
		av[0] = blk[0];
		av[1] = blk[1];
		if (av[0] > av[1]) for (int i = 2; i < 8; i++)
			av[i] = (unsigned char)(((8 - i) * av[0] + (i - 1) * av[1]) / 7);
		else {
			for (int i = 2; i < 6; i++)
				av[i] = (unsigned char)(((6 - i) * av[0] + (i - 1) * av[1]) / 5);
			av[6] = 0; av[7] = 255;
		}
		const unsigned lo = unsigned(blk[2]) | (unsigned(blk[3]) << 8) | (unsigned(blk[4]) << 16);
		const unsigned hi = unsigned(blk[5]) | (unsigned(blk[6]) << 8) | (unsigned(blk[7]) << 16);
		for (int i = 0; i < 16; i++) {
			const unsigned k = (i < 8) ? ((lo >> (3 * i)) & 7) : ((hi >> (3 * (i - 8))) & 7);
			out[i * 4 + 3] = av[k];
		}
	}
}


// A whole block-compressed level -> RGBA8. Texels outside the image (a level
// narrower than one block) are simply not written; the caller sized the buffer
// to the image, not to the block grid.
static void NatBCDecodeLevel(VkFormat fmt, const unsigned char *src, uint32_t w, uint32_t h,
							 std::vector<unsigned char> &rgba)
{
	const size_t blockBytes = (fmt == VK_FORMAT_BC1_RGBA_UNORM_BLOCK) ? 8 : 16;
	const uint32_t bw = std::max(1u, (w + 3) / 4);
	const uint32_t bh = std::max(1u, (h + 3) / 4);

	rgba.assign(size_t(w) * h * 4, 0);

	unsigned char texel[16 * 4];
	for (uint32_t by = 0; by < bh; by++) {
		for (uint32_t bx = 0; bx < bw; bx++) {
			NatBCDecodeBlock(fmt, src + (size_t(by) * bw + bx) * blockBytes, texel);
			for (uint32_t y = 0; y < 4; y++) {
				const uint32_t py = by * 4 + y;
				if (py >= h) break;
				for (uint32_t x = 0; x < 4; x++) {
					const uint32_t px = bx * 4 + x;
					if (px >= w) break;
					memcpy(&rgba[(size_t(py) * w + px) * 4], &texel[(y * 4 + x) * 4], 4);
				}
			}
		}
	}
}


// BC1's ONE-BIT ALPHA, which stb_dxt cannot write and the water mask is made
// entirely of.
//
// A BC1 block has two 5-6-5 endpoints and picks its meaning from their order:
// c0 > c1 is four opaque colours, c0 <= c1 is three colours plus TRANSPARENT
// at index 3. stb_compress_dxt_block(..., alpha = 0, ...) always emits the
// first form -- it orders the endpoints max-then-min on purpose -- so a chain
// built with it alone comes out fully opaque.
//
// That is not a corner case here. Textures/Earth/Mask/**.dds is DXT1 and
// MEASURED 100% punch-through blocks, every one, with 43% of the texels
// transparent: the water/land mask IS the one-bit alpha. NewPlanet.glsl reads
// it as
//
//     float fMask = (1.0f - cMsk.a);      // Specular Mask
//
// so alpha 0 is water and alpha 1 is land. Flatten the mips to opaque and
// every tile that is not being drawn at level 0 reports its ocean as land:
// no specular, no fresnel, no blue water brightening, and the land branch of
// the diffuse term. It switches the moment the tile crosses a LOD boundary,
// which is what "the coast just turns on like a light switch" is.
//
// So the punch-through form is written here. The endpoints come from the
// OPAQUE texels only -- a transparent texel's colour is not part of the image
// and letting it drag an endpoint would bleed it into its neighbours.
static unsigned NatPack565(int r, int g, int b)
{
	return ((unsigned(r) >> 3) << 11) | ((unsigned(g) >> 2) << 5) | (unsigned(b) >> 3);
}

static void NatUnpack565(unsigned v, int *r, int *g, int *b)
{
	*r = int(((( v >> 11) & 0x1F) * 255 + 15) / 31);
	*g = int(((( v >>  5) & 0x3F) * 255 + 31) / 63);
	*b = int(((  v        & 0x1F) * 255 + 15) / 31);
}

static void NatBC1EncodePunchThrough(const unsigned char *t, unsigned char *dst)
{
	int lo[3] = { 255, 255, 255 }, hi[3] = { 0, 0, 0 };
	int nOpaque = 0;
	for (int i = 0; i < 16; i++) {
		if (t[i * 4 + 3] < 128) continue;			// D3DX's 50% threshold
		nOpaque++;
		for (int c = 0; c < 3; c++) {
			const int v = t[i * 4 + c];
			if (v < lo[c]) lo[c] = v;
			if (v > hi[c]) hi[c] = v;
		}
	}

	unsigned c0 = 0, c1 = 0;
	if (nOpaque) {
		c0 = NatPack565(lo[0], lo[1], lo[2]);
		c1 = NatPack565(hi[0], hi[1], hi[2]);
		// c0 <= c1 IS the mode selector, so the order is the whole point.
		if (c0 > c1) { const unsigned s = c0; c0 = c1; c1 = s; }
	}

	int pr[3], pg[3], pb[3];
	NatUnpack565(c0, &pr[0], &pg[0], &pb[0]);
	NatUnpack565(c1, &pr[1], &pg[1], &pb[1]);
	pr[2] = (pr[0] + pr[1]) / 2;
	pg[2] = (pg[0] + pg[1]) / 2;
	pb[2] = (pb[0] + pb[1]) / 2;

	unsigned bits = 0;
	for (int i = 0; i < 16; i++) {
		unsigned k = 3;								// transparent
		if (t[i * 4 + 3] >= 128) {
			int best = 0, bd = 1 << 30;
			for (int j = 0; j < 3; j++) {
				const int dr = t[i * 4 + 0] - pr[j];
				const int dg = t[i * 4 + 1] - pg[j];
				const int db = t[i * 4 + 2] - pb[j];
				const int d = dr * dr + dg * dg + db * db;
				if (d < bd) { bd = d; best = j; }
			}
			k = unsigned(best);
		}
		bits |= k << (2 * i);
	}

	dst[0] = (unsigned char)(c0 & 0xFF);   dst[1] = (unsigned char)(c0 >> 8);
	dst[2] = (unsigned char)(c1 & 0xFF);   dst[3] = (unsigned char)(c1 >> 8);
	dst[4] = (unsigned char)( bits        & 0xFF);
	dst[5] = (unsigned char)((bits >>  8) & 0xFF);
	dst[6] = (unsigned char)((bits >> 16) & 0xFF);
	dst[7] = (unsigned char)((bits >> 24) & 0xFF);
}


// RGBA8 -> a whole block-compressed level. A level narrower than one block is
// padded by clamping, which is what a box filter reaching past an edge does
// everywhere else here.
static void NatBCEncodeLevel(VkFormat fmt, const std::vector<unsigned char> &rgba,
							 uint32_t w, uint32_t h, std::vector<unsigned char> &out)
{
	const size_t blockBytes = (fmt == VK_FORMAT_BC1_RGBA_UNORM_BLOCK) ? 8 : 16;
	const uint32_t bw = std::max(1u, (w + 3) / 4);
	const uint32_t bh = std::max(1u, (h + 3) / 4);

	out.assign(size_t(bw) * bh * blockBytes, 0);

	unsigned char texel[16 * 4];
	for (uint32_t by = 0; by < bh; by++) {
		for (uint32_t bx = 0; bx < bw; bx++) {

			for (uint32_t y = 0; y < 4; y++) {
				const uint32_t py = std::min(by * 4 + y, h - 1);
				for (uint32_t x = 0; x < 4; x++) {
					const uint32_t px = std::min(bx * 4 + x, w - 1);
					memcpy(&texel[(y * 4 + x) * 4], &rgba[(size_t(py) * w + px) * 4], 4);
				}
			}

			unsigned char *dst = &out[(size_t(by) * bw + bx) * blockBytes];

			if (fmt == VK_FORMAT_BC2_UNORM_BLOCK) {
				// stb_dxt has no BC2; its alpha half is not interpolated, so
				// it is written straight and the colour half is the same DXT1
				// block the other two formats use.
				for (int i = 0; i < 8; i++) {
					const unsigned n0 = (texel[(i * 2 + 0) * 4 + 3] * 15 + 127) / 255;
					const unsigned n1 = (texel[(i * 2 + 1) * 4 + 3] * 15 + 127) / 255;
					dst[i] = (unsigned char)(n0 | (n1 << 4));
				}
				stb_compress_dxt_block(dst + 8, texel, 0, STB_DXT_NORMAL);
			}
			else if (fmt == VK_FORMAT_BC3_UNORM_BLOCK) {
				// alpha = 1 emits the sixteen-byte DXT5 block, whose alpha
				// half is a real interpolated channel.
				stb_compress_dxt_block(dst, texel, 1, STB_DXT_NORMAL);
			}
			else {
				// BC1. A block with any transparent texel has to be written in
				// the punch-through form, which stb cannot emit -- see
				// NatBC1EncodePunchThrough above.
				bool punch = false;
				for (int i = 0; i < 16 && !punch; i++) punch = (texel[i * 4 + 3] < 128);

				if (punch) NatBC1EncodePunchThrough(texel, dst);
				else {
					stb_compress_dxt_block(dst, texel, 0, STB_DXT_NORMAL);

					// stb orders the endpoints max-then-min, so the block is
					// the four-colour form -- EXCEPT for a flat block, where
					// the two come out EQUAL and c0 <= c1 reads as
					// punch-through. One step apart keeps it opaque, and a
					// step is one unit of the blue channel's five bits.
					unsigned e0 = unsigned(dst[0]) | (unsigned(dst[1]) << 8);
					unsigned e1 = unsigned(dst[2]) | (unsigned(dst[3]) << 8);
					if (e0 <= e1) {
						if (e1 < 0xFFFF) e0 = e1 + 1; else e1 = e0 - 1;
						dst[0] = (unsigned char)(e0 & 0xFF); dst[1] = (unsigned char)(e0 >> 8);
						dst[2] = (unsigned char)(e1 & 0xFF); dst[3] = (unsigned char)(e1 >> 8);
					}
				}
			}
		}
	}
}


// D3DX_FILTER_BOX: one destination texel is the average of the four source
// texels above it. On the power-of-two levels every tile texture has, that is
// the whole of it; the clamp is for the odd sizes a non-square image reaches.
static void NatBoxHalve(const std::vector<unsigned char> &src, uint32_t w, uint32_t h,
						uint32_t bpp, std::vector<unsigned char> &dst,
						uint32_t *nw, uint32_t *nh)
{
	const uint32_t dw = std::max(1u, w >> 1);
	const uint32_t dh = std::max(1u, h >> 1);
	dst.assign(size_t(dw) * dh * bpp, 0);

	for (uint32_t y = 0; y < dh; y++) {
		const uint32_t y0 = std::min(y * 2, h - 1);
		const uint32_t y1 = std::min(y * 2 + 1, h - 1);
		for (uint32_t x = 0; x < dw; x++) {
			const uint32_t x0 = std::min(x * 2, w - 1);
			const uint32_t x1 = std::min(x * 2 + 1, w - 1);
			const unsigned char *a = &src[(size_t(y0) * w + x0) * bpp];
			const unsigned char *b = &src[(size_t(y0) * w + x1) * bpp];
			const unsigned char *c = &src[(size_t(y1) * w + x0) * bpp];
			const unsigned char *d = &src[(size_t(y1) * w + x1) * bpp];
			unsigned char *o = &dst[(size_t(y) * dw + x) * bpp];
			for (uint32_t k = 0; k < bpp; k++)
				o[k] = (unsigned char)((unsigned(a[k]) + b[k] + c[k] + d[k] + 2) / 4);
		}
	}
	*nw = dw; *nh = dh;
}


// How many levels can this format's chain be filled with? Zero means "leave it
// at one" -- the answer for the formats no tile texture uses, where inventing
// an untested conversion is worse than not mipmapping.
static uint32_t NatMipChainLevels(VkFormat fmt, uint32_t w, uint32_t h)
{
	if (!NatIsBC(fmt) && fmt != VK_FORMAT_B8G8R8A8_UNORM && fmt != VK_FORMAT_R8_UNORM)
		return 1;
	uint32_t n = 1, d = std::max(w, h);
	while (d > 1) { d >>= 1; n++; }
	return n;
}


// Fill levels 1..Mips-1 of a texture whose level 0 is already uploaded.
// pLevel0 is the level in the texture's OWN layout -- blocks for a BC format,
// texels otherwise.
static bool NatBuildMipChain(VulkanDevice *pDev, VulkanTexture *pTex, VkFormat fmt,
							 const void *pLevel0, uint32_t w, uint32_t h)
{
	const uint32_t levels = pTex->Mips();
	if (levels <= 1) return true;

	const bool bc = NatIsBC(fmt);
	const uint32_t bpp = bc ? 4 : (fmt == VK_FORMAT_R8_UNORM ? 1 : 4);

	std::vector<unsigned char> cur, next, blocks;

	if (bc) NatBCDecodeLevel(fmt, (const unsigned char *)pLevel0, w, h, cur);
	else    cur.assign((const unsigned char *)pLevel0,
					   (const unsigned char *)pLevel0 + size_t(w) * h * bpp);

	for (uint32_t m = 1; m < levels; m++) {

		uint32_t nw = 0, nh = 0;
		NatBoxHalve(cur, w, h, bpp, next, &nw, &nh);
		cur.swap(next);
		w = nw; h = nh;

		if (bc) {
			NatBCEncodeLevel(fmt, cur, w, h, blocks);
			if (!pDev->UploadTexture(pTex, m, 0, blocks.data(), blocks.size())) return false;
		}
		else {
			if (!pDev->UploadTexture(pTex, m, 0, cur.data(), cur.size())) return false;
		}
	}
	return true;
}


// ===========================================================================================
// Decode one DDS image into a texture. Counterpart of
// D3DXCreateTextureFromFileInMemoryEx.
//
// bFullMipChain is that call's `MipLevels = 0, Filter = D3DX_FILTER_BOX` pair:
// when the file carries a single level, the rest of the chain is generated.
// See NatBuildMipChain above for why it has to be generated here rather than
// by VulkanDevice::GenerateMipmaps.
// ===========================================================================================
VulkanTexture *NatCreateTextureFromDDSInMemory(const void* data, size_t bytes, bool bFullMipChain)
{
	if (!data || bytes < 4 + sizeof(NatDDSHeader)) return NULL;

	const char *p = (const char*)data;
	if (*(const DWORD*)p != MAKEFOURCC('D','D','S',' ')) return NULL;

	const NatDDSHeader *h = (const NatDDSHeader*)(p + 4);
	VkFormat fmt = NatDDSFormat(h);
	if (fmt == VK_FORMAT_UNDEFINED) {
		// SAY WHICH FORMAT, or this is unactionable. D3DX decoded every DDS
		// variant Orbiter ships and never had to report one; here an
		// unsupported pixel format means a texture silently becomes the white
		// fallback, and a glow or mask texture that turns into opaque white
		// changes what the frame looks like.
		const DWORD fourcc = h->ddspf.dwFourCC;
		char cc[5] = { char(fourcc & 0xFF), char((fourcc >> 8) & 0xFF),
					   char((fourcc >> 16) & 0xFF), char((fourcc >> 24) & 0xFF), 0 };
		for (int i = 0; i < 4; i++) if (cc[i] < 32 || cc[i] > 126) cc[i] = '.';
		LogErr("NatCreateTextureFromDDSInMemory: unsupported DDS pixel format "
			   "%ux%u flags=%08X fourCC='%s'(%08X) bits=%u "
			   "Rmask=%08X Gmask=%08X Bmask=%08X Amask=%08X",
			   (unsigned)h->dwWidth, (unsigned)h->dwHeight,
			   (unsigned)h->ddspf.dwFlags, cc, (unsigned)fourcc,
			   (unsigned)h->ddspf.dwRGBBitCount,
			   (unsigned)h->ddspf.dwRBitMask, (unsigned)h->ddspf.dwGBitMask,
			   (unsigned)h->ddspf.dwBBitMask, (unsigned)h->ddspf.dwABitMask);
		return NULL;
	}

	const uint32_t fileMips = (h->dwFlags & DDSD_MIPMAPCOUNT) ? std::max(1u, (uint32_t)h->dwMipMapCount) : 1u;

	// `MipLevels = 0` asked D3DX for the whole chain whatever the file held.
	// The levels the file does not carry are generated below; the count is
	// decided here because a VkImage's level count is fixed at creation.
	uint32_t mips = fileMips;
	if (bFullMipChain && fileMips == 1)
		mips = NatMipChainLevels(fmt, h->dwWidth, h->dwHeight);

	VulkanDevice *pDev = g_client->GetDevice();
	VulkanTexture *pTex = pDev->CreateTexture(h->dwWidth, h->dwHeight, mips, fmt,
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	if (!pTex) return NULL;

	const char *src = p + 4 + sizeof(NatDDSHeader);
	size_t left = bytes - (4 + sizeof(NatDDSHeader));

	// A 24-bit file carries three bytes per texel where the format declares
	// four, so each level is widened into this before upload. D3DX did the
	// same expansion behind D3DXCreateTextureFromFileInMemoryEx; see
	// NatDDSFormat.
	const bool b24 = NatDDSIs24Bit(h);

	// A 4-4-4-4 file carries two bytes per texel where the format declares
	// four, and is widened here for the same reason -- see NatDDSFormat.
	// D3DX did it behind D3DXCreateTextureFromFileInMemoryEx once
	// D3D9Surface.cpp:165 named A8R8G8B8 as the format to create.
	const bool b4444 = NatDDSIs4444(h);
	const bool b4444HasAlpha = b4444 && h->ddspf.dwABitMask == 0xF000;
	std::vector<unsigned char> wide;

	// The bytes of level 0 as the texture holds them, kept for the chain
	// builder below. For a widened file that is `wide`, which the next
	// iteration overwrites, so it is copied; otherwise it is the file itself.
	const char *level0 = src;
	std::vector<unsigned char> level0wide;
	bool bLevel0 = false;

	uint32_t w = h->dwWidth, ht = h->dwHeight;
	for (uint32_t m = 0; m < fileMips; m++)
	{
		if (b4444) {
			const size_t texels = size_t(w) * ht;
			const size_t n2 = texels * 2;
			if (n2 > left) break;
			wide.resize(texels * 4);
			const unsigned char *s = (const unsigned char *)src;
			unsigned char *d = wide.data();
			for (size_t i = 0; i < texels; i++) {
				// Little-endian on disk, as every other field of this header
				// is read.
				const unsigned v = unsigned(s[0]) | (unsigned(s[1]) << 8);
				// Nibble expansion is (n*255 + 7)/15, NEVER n<<4: a shift
				// leaves fully opaque at 240, which composites the HUD as a
				// grey veil instead of leaving it alone. Same rule as the
				// A4R4G4B4 decode in VulkanImageIO.cpp and BC2's 4-bit alpha.
				const unsigned b = (v      ) & 0xF;
				const unsigned g = (v >>  4) & 0xF;
				const unsigned r = (v >>  8) & 0xF;
				const unsigned a = (v >> 12) & 0xF;
				d[0] = (unsigned char)((b * 255 + 7) / 15);		// B
				d[1] = (unsigned char)((g * 255 + 7) / 15);		// G
				d[2] = (unsigned char)((r * 255 + 7) / 15);		// R
				d[3] = b4444HasAlpha ? (unsigned char)((a * 255 + 7) / 15)
									 : (unsigned char)0xFF;		// X4 -> opaque
				s += 2; d += 4;
			}
			if (!pDev->UploadTexture(pTex, m, 0, wide.data(), wide.size())) {
				pDev->DestroyTexture(pTex);
				return NULL;
			}
			if (m == 0) { level0wide = wide; bLevel0 = true; }
			src += n2; left -= n2;
			w = std::max(1u, w >> 1);
			ht = std::max(1u, ht >> 1);
			continue;
		}

		if (b24) {
			const size_t texels = size_t(w) * ht;
			const size_t n3 = texels * 3;
			if (n3 > left) break;
			wide.resize(texels * 4);
			const unsigned char *s = (const unsigned char *)src;
			unsigned char *d = wide.data();
			for (size_t i = 0; i < texels; i++) {
				d[0] = s[0];		// B
				d[1] = s[1];		// G
				d[2] = s[2];		// R
				d[3] = 0xFF;		// X8 -> opaque, as D3DFMT_X8R8G8B8
				s += 3; d += 4;
			}
			if (!pDev->UploadTexture(pTex, m, 0, wide.data(), wide.size())) {
				pDev->DestroyTexture(pTex);
				return NULL;
			}
			if (m == 0) { level0wide = wide; bLevel0 = true; }
			src += n3; left -= n3;
			w = std::max(1u, w >> 1);
			ht = std::max(1u, ht >> 1);
			continue;
		}

		size_t n = NatLevelBytes(fmt, w, ht);
		if (n > left) {
			// The file declares more levels than it carries. Upload what is
			// there and stop, rather than reading past the buffer -- the
			// concatenated tile archives LoadPlanetTextures walks are exactly
			// this shape, one level per image with a mip count of 1.
			break;
		}
		if (!pDev->UploadTexture(pTex, m, 0, src, n)) {
			pDev->DestroyTexture(pTex);
			return NULL;
		}
		if (m == 0) bLevel0 = true;
		src += n; left -= n;
		w = std::max(1u, w >> 1);
		ht = std::max(1u, ht >> 1);
	}

	// The rest of what `MipLevels = 0, D3DX_FILTER_BOX` produced.
	if (bLevel0 && mips > fileMips) {
		const void *pL0 = level0wide.empty() ? (const void*)level0
											 : (const void*)level0wide.data();
		if (!NatBuildMipChain(pDev, pTex, fmt, pL0, h->dwWidth, h->dwHeight))
			LogErr("NatCreateTextureFromDDSInMemory: mip chain build failed for "
				   "%ux%u format %d", (unsigned)h->dwWidth, (unsigned)h->dwHeight, int(fmt));
	}

	return pTex;
}


// ===========================================================================================
// Decode PNG / JPG / BMP into a texture, through stb_image.
//
// D3DX chose the surface format from the file: JPG and BMP became X8R8G8B8 and
// PNG A8R8G8B8, which NatLoadSurface below spells out. stb_image always hands
// back RGBA8, so the format is B8G8R8A8_UNORM in every case and the channel
// order is fixed during the copy -- the same swap Src/Orbiter/Linux/WinCodec.cpp
// makes, and for the same reason: getting it backwards produces a plausible
// image with red and blue exchanged.
// ===========================================================================================
static VulkanTexture *NatCreateTextureFromImageFile(const char *path, uint32_t *pW = NULL, uint32_t *pH = NULL)
{
	int w = 0, h = 0, comp = 0;
	stbi_uc *pix = stbi_load(path, &w, &h, &comp, 4);
	if (!pix) return NULL;

	for (int i = 0; i < w * h; i++) std::swap(pix[i * 4 + 0], pix[i * 4 + 2]);	// RGBA -> BGRA

	VulkanDevice *pDev = g_client->GetDevice();
	VulkanTexture *pTex = pDev->CreateTexture((uint32_t)w, (uint32_t)h, 1, VK_FORMAT_B8G8R8A8_UNORM,
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

	if (pTex) {
		if (!pDev->UploadTexture(pTex, 0, 0, pix, size_t(w) * h * 4)) {
			pDev->DestroyTexture(pTex);
			pTex = NULL;
		}
	}

	stbi_image_free(pix);
	if (pW) *pW = (uint32_t)w;
	if (pH) *pH = (uint32_t)h;
	return pTex;
}


// ===========================================================================================
// The same decode, from a block of memory instead of a file.
//
// Counterpart of D3DXLoadSurfaceFromFileInMemory, which VulkanClient.cpp's
// SplashScreen() uses to decode the splash image out of the executable's
// resources -- there is no file to open, the bytes come from
// FindResource/LoadResource/LockResource. stb_image has the same pair of entry
// points that D3DX did, so this is NatCreateTextureFromImageFile with
// stbi_load_from_memory in place of stbi_load and nothing else changed.
// ===========================================================================================
VulkanTexture *NatCreateTextureFromMemory(const void *data, size_t bytes, uint32_t *pW, uint32_t *pH)
{
	if (!data || !bytes) return NULL;

	int w = 0, h = 0, comp = 0;
	stbi_uc *pix = stbi_load_from_memory((const stbi_uc*)data, (int)bytes, &w, &h, &comp, 4);
	if (!pix) return NULL;

	for (int i = 0; i < w * h; i++) std::swap(pix[i * 4 + 0], pix[i * 4 + 2]);	// RGBA -> BGRA

	VulkanDevice *pDev = g_client->GetDevice();
	VulkanTexture *pTex = pDev->CreateTexture((uint32_t)w, (uint32_t)h, 1, VK_FORMAT_B8G8R8A8_UNORM,
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

	if (pTex) {
		if (!pDev->UploadTexture(pTex, 0, 0, pix, size_t(w) * h * 4)) {
			pDev->DestroyTexture(pTex);
			pTex = NULL;
		}
	}

	stbi_image_free(pix);
	if (pW) *pW = (uint32_t)w;
	if (pH) *pH = (uint32_t)h;
	return pTex;
}


// Counterpart of D3DXGetImageInfoFromFileA: size and format without decoding
// the pixels. stb_image_info does it for PNG/JPG/BMP; DDS is read from its
// header, which is what D3DX did too.
static bool NatImageInfo(const char *path, uint32_t *w, uint32_t *h, VkFormat *fmt, uint32_t *mips)
{
	FILE *f = NULL;
	if (fopen_s(&f, path, "rb") || !f) return false;

	char head[4 + sizeof(NatDDSHeader)];
	size_t got = fread(head, 1, sizeof(head), f);

	if (got == sizeof(head) && *(DWORD*)head == MAKEFOURCC('D','D','S',' ')) {
		const NatDDSHeader *dh = (const NatDDSHeader*)(head + 4);
		if (w) *w = dh->dwWidth;
		if (h) *h = dh->dwHeight;
		if (fmt) *fmt = NatDDSFormat(dh);
		if (mips) *mips = (dh->dwFlags & DDSD_MIPMAPCOUNT) ? std::max(1u, (uint32_t)dh->dwMipMapCount) : 1u;
		fclose(f);
		return true;
	}
	fclose(f);

	int iw = 0, ih = 0, comp = 0;
	if (!stbi_info(path, &iw, &ih, &comp)) return false;
	if (w) *w = (uint32_t)iw;
	if (h) *h = (uint32_t)ih;
	if (fmt) *fmt = VK_FORMAT_B8G8R8A8_UNORM;
	if (mips) *mips = 1;
	return true;
}


// The one entry point the rest of this file loads through: DDS or not.
static VulkanTexture *NatCreateTextureFromFile(const char *path)
{
	FILE *f = NULL;
	if (fopen_s(&f, path, "rb") || !f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	rewind(f);
	if (n < 4) { fclose(f); return NULL; }

	DWORD magic = 0;
	if (fread(&magic, 1, 4, f) != 4) { fclose(f); return NULL; }

	if (magic == MAKEFOURCC('D','D','S',' ')) {
		rewind(f);
		std::vector<char> blob(n);
		size_t got = fread(blob.data(), 1, n, f);
		fclose(f);
		return NatCreateTextureFromDDSInMemory(blob.data(), got);
	}
	fclose(f);
	return NatCreateTextureFromImageFile(path);
}


// ===========================================================================================
//
void NatCheckFlags(DWORD &flags)
{
	// Append dependend flags
	if (flags & OAPISURFACE_RENDER3D) flags |= OAPISURFACE_RENDERTARGET;
	if (flags & OAPISURFACE_SKETCHPAD) flags |= OAPISURFACE_RENDERTARGET;

	if (flags & OAPISURFACE_RENDERTARGET) {
		if (flags & OAPISURFACE_GDI)
		{
			oapiWriteLog((char*)"OAPISURFACE_GDI is incomaptible with OAPISURFACE_RENDERTARGET and OAPISURFACE_SKETCHPAD");
			HALT();
		}
		if (flags & OAPISURFACE_SYSMEM)
		{
			oapiWriteLog((char*)"OAPISURFACE_SYSMEM is incomaptible with OAPISURFACE_RENDERTARGET and OAPISURFACE_SKETCHPAD");
			HALT();
		}
	}

	if (flags & OAPISURFACE_MIPMAPS) {
		if ((flags & OAPISURFACE_TEXTURE) == 0)
		{
			oapiWriteLog((char*)"OAPISURFACE_MIPMAPS can be only assigned to a OAPISURFACE_TEXTURE");
			HALT();
		}
		if (flags & OAPISURFACE_SYSMEM)
		{
			oapiWriteLog((char*)"OAPISURFACE_MIPMAPS is incomaptible with OAPISURFACE_SYSMEM");
			HALT();
		}
	}
}


// ===========================================================================================
// Was D3DXCreateTextureFromFileExA with D3DFMT_FROM_FILE and Config->TextureMips
// deciding whether to auto-generate a mip chain.
//
// "Autogen" has no counterpart: D3DUSAGE_AUTOGENMIPMAP asked the driver to
// build the chain, and Vulkan has no such request -- the client blits down the
// chain itself. So the config decision moves from a creation flag to a call to
// NatGenerateMipmaps after loading, which is the same two outcomes reached the
// only way Vulkan offers.
//
VulkanTexture *NatLoadSpecialTexture(const char* fname, const char* ext)
{
	char path[MAX_PATH];
	char name[MAX_PATH];

	NatCreateName(name, ARRAYSIZE(name), fname, ext);

	if (g_client->TexturePath(name, path)) {
		uint32_t w = 0, h = 0, mips = 1;
		VkFormat fmt = VK_FORMAT_UNDEFINED;
		if (NatImageInfo(path, &w, &h, &fmt, &mips)) {
			return NatCreateTextureFromFile(path);
		}
	}
	return NULL;
}



// ======================================================================================
// Main loading routine
//
VulkanTexture *NatLoadTexture(const char* path) { return NatCreateTextureFromFile(path); }

SURFHANDLE NatLoadSurface(const char* file, DWORD flags, bool bPath)
{
	VulkanTexture *pTex = NULL;
	SurfNative* pNat = NULL;

	NatCheckFlags(flags);

	char path[MAX_PATH];

	if (bPath) strcpy_s(path, MAX_PATH, file);
	else {
		if (!g_client->TexturePath(file, path)) {
			return NULL;
		}
	}
	
	DWORD pass = OAPISURFACE_TEXTURE | OAPISURFACE_SHARED;

	// Load regular texture with additional maps if exists
	//
	if ((flags & ~pass) == 0)
	{
		uint32_t w = 0, h = 0, mips = 1;
		VkFormat fmt = VK_FORMAT_UNDEFINED;

		if (NatImageInfo(path, &w, &h, &fmt, &mips))
		{
			pTex = NatCreateTextureFromFile(path);

			if (pTex)
			{
				pNat = new SurfNative(pTex, flags);
				pNat->SetName(file);

				// Config->TextureMips: 2 = build a chain for everything,
				// 1 = build one where the file has none. Was a creation flag;
				// is an explicit blit now. See the note above.
				if (Config->TextureMips == 2 || (Config->TextureMips == 1 && mips == 1))
					NatGenerateMipmaps(SURFHANDLE(pNat));

				LogBlu("TextureLoaded [%s] PLAIN Mips=%u Format=%d (%u,%u) Flags=0x%X, %s",
					file, pNat->GetMipMaps(), int(fmt), w, h, flags, _PTR(pNat));

				pNat->AddMap(MAP_HEAT, NatLoadSpecialTexture(file, "heat"));
				pNat->AddMap(MAP_NORMAL, NatLoadSpecialTexture(file, "norm"));
				pNat->AddMap(MAP_SPECULAR, NatLoadSpecialTexture(file, "spec"));
				pNat->AddMap(MAP_EMISSION, NatLoadSpecialTexture(file, "emis"));
				pNat->AddMap(MAP_ROUGHNESS, NatLoadSpecialTexture(file, "rghn"));
				pNat->AddMap(MAP_METALNESS, NatLoadSpecialTexture(file, "metal"));
				pNat->AddMap(MAP_REFLECTION, NatLoadSpecialTexture(file, "refl"));
				pNat->AddMap(MAP_TRANSLUCENCE, NatLoadSpecialTexture(file, "transl"));
				pNat->AddMap(MAP_TRANSMITTANCE, NatLoadSpecialTexture(file, "transm"));
			}
			else oapiWriteLogV("FAILED: NatLoadSurface(%s)", path);
		}
		else oapiWriteLogV("FAILED: NatLoadSurface(%s)", path);

		return SURFHANDLE(pNat);
	}


	// Load more complex surface
	//
	uint32_t w = 0, h = 0, mips = 1;
	VkFormat srcFmt = VK_FORMAT_UNDEFINED;

	if (NatImageInfo(path, &w, &h, &srcFmt, &mips))
	{
		if (flags & OAPISURFACE_SKETCHPAD) flags |= OAPISURFACE_RENDERTARGET;
		if (flags & OAPISURFACE_RENDERTARGET) flags |= OAPISURFACE_UNCOMPRESS;

		VkFormat Format = srcFmt;

		// The reference forces X8R8G8B8/A8R8G8B8 for JPG/PNG/BMP and rejects
		// A4R4G4B4/X4R4G4B4. Both collapse: stb_image hands back RGBA8
		// whatever went in, so anything that is not a DDS is already
		// B8G8R8A8_UNORM by the time it gets here.

		if (flags & OAPISURFACE_UNCOMPRESS)
		{
			if (Format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ||
				Format == VK_FORMAT_BC2_UNORM_BLOCK ||
				Format == VK_FORMAT_BC3_UNORM_BLOCK) Format = VK_FORMAT_B8G8R8A8_UNORM;
		}

		// User defined format
		VkFormat Fmt = NatConvertFormat_OAPI_to_VK(flags);
		if (Fmt != VK_FORMAT_UNDEFINED) Format = Fmt;

		// A caller that names OAPISURFACE_PF_XRGB is asking for the format
		// with NO ALPHA CHANNEL, which Vulkan cannot spell -- see
		// VulkanTexture::SetAlphaOne and NatCreateSurface. Unlike the created
		// surfaces, a LOADED one gets its format from the file unless asked
		// otherwise, so only the explicit request counts here.
		const bool bNoAlphaChannel =
			((flags & OAPISURFACE_PF_MASK) == OAPISURFACE_PF_XRGB);

		if (flags & OAPISURFACE_NOMIPMAPS) mips = 1;

		VulkanTexture *pSrc = NatCreateTextureFromFile(path);
		if (!pSrc) return NULL;

		if (flags & OAPISURFACE_TEXTURE)
		{
			// THE REFERENCE PASSES Usage INTO THE TEXTURE CREATION, AND THIS
			// BRANCH WAS DROPPING IT.
			//
			// D3D9Surface.cpp computes
			//
			//     if (flags & OAPISURFACE_RENDERTARGET) Usage = D3DUSAGE_RENDERTARGET;
			//
			// and then hands Usage to D3DXCreateTextureFromFileExA in THIS
			// branch -- so a surface asked for as both a texture and a render
			// target gets a texture that can be rendered into, and the
			// OAPISURFACE_RENDERTARGET branch below is only reached by a
			// surface that is not a texture. The port took the file loader's
			// fixed usage (SAMPLED|TRANSFER_SRC|TRANSFER_DST) and ignored the
			// flag entirely.
			//
			// Cockpit\hud.dds is exactly that surface -- OAPISURFACE_TEXTURE |
			// OAPISURFACE_RENDERTARGET | OAPISURFACE_UNCOMPRESS -- and the
			// consequence was not a missing feature but a hang. HUD::
			// TexBltString blits each glyph from one part of the HUD texture
			// onto another part of the SAME surface, clbkScaleBlt's
			// render-target branch is guarded on
			//
			//     td->Usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
			//
			// which was false, so every path fell through to
			// "oapiBlt() Failed (End)" -> BltError -> RuntimeError ->
			// MessageBoxA, raised from inside the scene callback where it can
			// never be drawn or dismissed. The client spun there forever with
			// no window on screen.
			//
			// The decode already happened, so the render-target usage is asked
			// for here and the decoded image copied in -- the same shape the
			// OAPISURFACE_RENDERTARGET branch below already uses.
			VulkanTexture *pFinal = pSrc;

			if (flags & OAPISURFACE_RENDERTARGET)
			{
				VulkanDevice *pDev = g_client->GetDevice();
				VulkanTexture *pRT = pDev->CreateTexture(w, h, 1, Format,
					VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
					VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

				if (pRT) {
					pDev->BlitTexture(pRT, pSrc);
					pDev->DestroyTexture(pSrc);
					pFinal = pRT;
				}
				else {
					// Keep the sampled-only texture rather than failing the
					// load: it still draws, and the blit paths that need an
					// attachment log their own error.
					LogErr("NatLoadSurface: [%s] asked for a render target but "
						   "the format cannot be one; loaded as a plain texture", file);
				}
			}

			if (bNoAlphaChannel) pFinal->SetAlphaOne();

			SurfNative* pSrf = new SurfNative(pFinal, flags);
			pSrf->SetName(file);
			if (flags & OAPISURFACE_MIPMAPS) NatGenerateMipmaps(SURFHANDLE(pSrf));
			LogBlu("TextureLoaded [%s] Format=%d (%u,%u) Flags=0x%X, %s", file, int(Format), w, h, flags, _PTR(pSrf));
			return SURFHANDLE(pSrf);
		}

		if (flags & OAPISURFACE_RENDERTARGET)
		{
			// Was CreateRenderTarget + D3DXLoadSurfaceFromFile: make an empty
			// render target, then decode the file into it. Here the decode
			// already happened, so the render-target usage is asked for at
			// creation and the decoded image is copied in -- which is what
			// D3DXLoadSurfaceFromFile was doing underneath.
			VulkanDevice *pDev = g_client->GetDevice();
			VulkanTexture *pRT = pDev->CreateTexture(w, h, 1, Format,
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
				VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

			if (pRT) {
				pDev->BlitTexture(pRT, pSrc);
				pDev->DestroyTexture(pSrc);
				if (bNoAlphaChannel) pRT->SetAlphaOne();
				SurfNative* pSrf = new SurfNative(pRT, flags);
				pSrf->SetName(file);
				LogBlu("SurfaceLoaded [%s] RENDERTARGET Format=%d (%u,%u) Flags=0x%X %s", file, int(Format), w, h, flags, _PTR(pSrf));
				return SURFHANDLE(pSrf);
			}
		}

		g_client->GetDevice()->DestroyTexture(pSrc);
	}

	return NULL;
}



// ===============================================================================================
// Was three branches on GetType() plus a render-target special case that
// copied every mip into a D3DPOOL_SYSTEMMEM texture with GetRenderTargetData
// before saving, because a default-pool resource cannot be read back.
//
// Vulkan has the same restriction and answers it the same way, except that the
// readback destination is a host-visible BUFFER rather than a second texture,
// and the copy is vkCmdCopyImageToBuffer. VulkanDevice::ReadTexture hides that,
// so what is left here is: get the pixels, encode them.
//
// D3DXSaveTextureToFileA/D3DXSaveSurfaceToFileA supported DDS, BMP, JPG and
// PNG. stb_image_write covers BMP, JPG and PNG; DDS OUTPUT IS NOT SUPPORTED and
// says so rather than writing a file that is not a DDS. Nothing in the client
// asks for it -- clbkSaveSurfaceToImage is screenshots and gcCore surface
// dumps -- but the reference accepted ".dds" so the refusal is explicit.
//
bool NatSaveSurface(const char* file, VulkanTexture *pResource)
{
	if (!pResource) return false;

	if (contains(file, ".dds")) {
		LogErr("NatSaveSurface: DDS output is not supported (%s)", file);
		NatDumpResource(pResource);
		return false;
	}

	VulkanDevice *pDev = g_client->GetDevice();

	uint32_t w = pResource->Width(), h = pResource->Height();
	std::vector<unsigned char> pix(size_t(w) * h * 4);

	if (!pDev->ReadTexture(pResource, 0, pix.data(), pix.size())) {
		oapiWriteLog((char*)"NatSaveSurface(): readback failed");
		NatDumpResource(pResource);
		return false;
	}

	// BGRA on the GPU, RGBA for the encoder -- the same swap the loader makes.
	for (size_t i = 0; i < size_t(w) * h; i++) std::swap(pix[i * 4 + 0], pix[i * 4 + 2]);

	int ok = 0;
	if (contains(file, ".bmp")) ok = stbi_write_bmp(file, w, h, 4, pix.data());
	else if (contains(file, ".jpg")) ok = stbi_write_jpg(file, w, h, 4, pix.data(), 90);
	else ok = stbi_write_png(file, w, h, 4, pix.data(), w * 4);

	if (ok) return true;

	oapiWriteLog((char*)"NatSaveSurface():");
	NatDumpResource(pResource);
	return false;
}



// ===============================================================================================
// Was two creation paths -- D3DXCreateTexture for textures/sysmem/GDI, and
// CreateRenderTarget for render targets -- because those were different
// interfaces. One VkImage does both; the difference is the usage flags, so the
// paths merge and the flag decoding is what is left.
//
SURFHANDLE NatCreateSurface(int width, int height, DWORD flags)
{
	DWORD Mips = 1;
	VulkanDevice *pDev = g_client->GetDevice();

	NatCheckFlags(flags);

	VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	bool hostVisible = false;

	if (flags & (OAPISURFACE_TEXTURE | OAPISURFACE_SKETCHPAD)) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
	if (flags & OAPISURFACE_RENDERTARGET) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	if (flags & (OAPISURFACE_SYSMEM | OAPISURFACE_GDI)) hostVisible = true;
	if (flags & OAPISURFACE_NOMIPMAPS) Mips = 1;

	// D3DUSAGE_AUTOGENMIPMAP has no counterpart; the level count is declared
	// here and NatGenerateMipmaps fills them.
	if (flags & OAPISURFACE_MIPMAPS)
	{
		Mips = 1;
		uint32_t d = std::max(width, height);
		while (d > 1) { d >>= 1; Mips++; }
	}

	// D3DMULTISAMPLE_8_SAMPLES. Whether the device can do it is a limit here
	// rather than a caps bit, so it is clamped instead of assumed.
	VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
	if (flags & OAPISURFACE_ANTIALIAS) {
		VkSampleCountFlags ok = pDev->GetProperties()->limits.framebufferColorSampleCounts;
		if (ok & VK_SAMPLE_COUNT_8_BIT) samples = VK_SAMPLE_COUNT_8_BIT;
		else if (ok & VK_SAMPLE_COUNT_4_BIT) samples = VK_SAMPLE_COUNT_4_BIT;
		else if (ok & VK_SAMPLE_COUNT_2_BIT) samples = VK_SAMPLE_COUNT_2_BIT;
	}

	// THE FORMAT CHOICE, AND THE ONE PIECE OF IT VULKAN CANNOT SPELL.
	//
	// The reference is D3D9Surface.cpp:322 --
	//
	//     if (flags & OAPISURFACE_ALPHA) Format = D3DFMT_A8R8G8B8;
	//     else                           Format = D3DFMT_X8R8G8B8;
	//
	// -- and X8R8G8B8 is not "A8R8G8B8 with the alpha ignored by convention".
	// It is a format with NO ALPHA CHANNEL: four bytes wide, but a sampler
	// reading it gets alpha = 1.0 whatever is in the fourth byte.
	//
	// VULKAN HAS NO X8 FORMAT AT ALL. VK_FORMAT_B8G8R8A8_UNORM is the only
	// 32-bit BGRA and it means what it says. So the format carries over and
	// the MISSING CHANNEL is expressed where Vulkan puts that question -- a
	// component swizzle on the view. See VulkanTexture::SetAlphaOne for what
	// leaving it out cost (the whole virtual-cockpit HUD) and why the swizzle
	// is the exact equivalent rather than an approximation.
	//
	// The condition is the reference's, unchanged: an explicit PF_ format
	// wins, then OAPISURFACE_ALPHA, and otherwise it is X8R8G8B8. BOTH ways
	// of arriving at X8R8G8B8 count -- OAPISURFACE_PF_XRGB names it outright
	// (NatConvertFormat_OAPI_to_VK has to answer B8G8R8A8 for it, for the same
	// reason), and the default takes it when no alpha was asked for.
	VkFormat Format = NatConvertFormat_OAPI_to_VK(flags);
	const bool bNoAlphaChannel =
		((flags & OAPISURFACE_PF_MASK) == OAPISURFACE_PF_XRGB) ||
		((Format == VK_FORMAT_UNDEFINED) && ((flags & OAPISURFACE_ALPHA) == 0));
	if (Format == VK_FORMAT_UNDEFINED) Format = VK_FORMAT_B8G8R8A8_UNORM;

	VulkanTexture *pTex = pDev->CreateTexture(width, height, Mips, Format, usage);
	if (!pTex) { assert(false); return NULL; }

	if (bNoAlphaChannel) pTex->SetAlphaOne();

	if (hostVisible || samples != VK_SAMPLE_COUNT_1_BIT) {
		// Recorded so GetDesc() reports what was asked for; CreateTexture
		// honours the memory choice through the usage it was given.
		pTex->SetHostVisible(hostVisible);
		pTex->SetSamples(samples);
	}

	VulkanTexture *pDepth = NULL;
	if (flags & OAPISURFACE_RENDER3D)
	{
		// D3DFMT_D24S8 -> the same depth+stencil format Vulkan spells
		// VK_FORMAT_D24_UNORM_S8_UINT. It is optional there; D32_SFLOAT_S8_UINT
		// is the fallback every desktop driver supports.
		VkFormat dfmt = VK_FORMAT_D24_UNORM_S8_UINT;
		if (!pDev->SupportsDepthStencil(dfmt)) dfmt = VK_FORMAT_D32_SFLOAT_S8_UINT;
		pDepth = pDev->CreateTexture(width, height, 1, dfmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
	}

	return SURFHANDLE(new SurfNative(pTex, flags, pDepth));
}


// ===============================================================================================
// Was GetSurfaceLevel(level) -- a view of one mip of a texture, wrapped as a
// separate render-target surface.
//
// Vulkan expresses that as an image VIEW with baseMipLevel set, so the
// sublevel shares the parent's image rather than being a distinct object. The
// SurfNative it returns therefore does NOT own its image, which is what the
// OAPISURFACE_BACKBUFFER path in the destructor already handles: a surface
// that did not create its resource does not destroy it.
//
SURFHANDLE NatGetMipSublevel(SURFHANDLE hSrf, int level)
{
	static const DWORD fl = OAPISURFACE_RENDERTARGET | OAPISURFACE_TEXTURE;

	if ((SURFACE(hSrf)->Flags & fl) == fl)
	{
		VulkanTexture *pTex = SURFACE(hSrf)->GetTexture();
		if (!pTex) return NULL;
		VulkanTexture *pView = g_client->GetDevice()->CreateMipView(pTex, level);
		if (pView) {
			SurfNative* pNat = new SurfNative(pView, OAPISURFACE_RENDERTARGET | OAPISURFACE_BACKBUFFER, NULL);
			return SURFHANDLE(pNat);
		}
	}
	else {
		LogErr("NatGetMipSublevel() Surface is not a rendertarget-texture. Handle = %s", _PTR(hSrf));
	}
	return NULL;
}


// ===============================================================================================
// Was two branches on GetType(), each creating a DXT texture and filling every
// mip with D3DXLoadSurfaceFromSurface -- which decoded, rescaled and
// block-compressed in one call.
//
// THERE IS NO COUNTERPART TO THE COMPRESSION. vkCmdBlitImage rescales and
// converts between uncompressed formats, but no Vulkan call produces BC blocks;
// block compression is an offline step or a shader. So this reports the
// request and returns the surface uncompressed rather than returning a texture
// whose format lies about its contents.
//
// Nothing in the client calls it: NatCompressSurface is reachable only through
// gcCore's CompressSurface, which no shipped module uses. It is converted
// rather than dropped because it is part of the public gcCore surface.
//
SURFHANDLE NatCompressSurface(SURFHANDLE hSurface, DWORD flags)
{
	LogWrn("NatCompressSurface: block compression has no Vulkan counterpart; "
		   "returning the surface uncompressed [%s]", SURFACE(hSurface)->GetName());
	SURFACE(hSurface)->IncRef();
	return hSurface;
}


// ===============================================================================================
// Was GetSurfaceLevel(0) then StretchRect down the chain with D3DTEXF_LINEAR.
// vkCmdBlitImage with VK_FILTER_LINEAR is the same operation; the difference is
// that each level has to be transitioned to TRANSFER_SRC/TRANSFER_DST around
// the blit, which VulkanDevice::GenerateMipmaps does.
//
bool NatGenerateMipmaps(SURFHANDLE hSrf)
{
	VulkanTexture *pTex = SURFACE(hSrf)->GetTexture();
	if (!pTex) return false;
	if (pTex->Mips() <= 1) return false;
	return g_client->GetDevice()->GenerateMipmaps(pTex);
}


// -----------------------------------------------------------------------------------------------
// Was: read D3DRESOURCETYPE, then GetDesc() for a surface or GetLevelDesc(0) +
// GetLevelCount() for a texture, and assert on anything else.
//
// A VkImage answers none of those questions -- it remembers nothing you can
// query -- so the description is copied from the texture, which recorded what
// it was created with. The type branch has nothing to branch on and is gone.
//
SurfNative::SurfNative(VulkanTexture *pRes, DWORD flags, VulkanTexture *_pDepth) :
	hOrigin(this),
	pGDICache(NULL),
	pTemp(NULL),
	pDepth(_pDepth),
	pResource(pRes),
	pDevice(g_client->GetDevice()),
	ColorKey(SURF_NO_CK),
	Flags(flags),
	Mipmaps(1),
	ClientFlags(0),
	RefCount(1),
	pSkp(NULL)
{
	assert(pRes != NULL);
	// Compares a pseudo-handle against itself and so never fails, here as on
	// Windows -- NOT a real thread-identity test. Writing it with thread IDs
	// would make it fire for every surface the tile loader builds. See the
	// note at VulkanClient::GetMainThread().
	assert(GetCurrentThread() == g_client->GetMainThread());

	SurfaceCatalog.insert(this);

	memset(pMap, 0, sizeof(pMap));
	memset(&desc, 0, sizeof(desc));
	memset(&DC, 0, sizeof(DC));

	strcpy_s(name, sizeof(name), "null");

	desc = pResource->Desc();
	Mipmaps = desc.Mips;
}


// -----------------------------------------------------------------------------------------------
//
SurfNative::SurfNative(SurfNative* pOrigin)
{
	pResource = pOrigin->pResource;
	pTemp = NULL;
	pDepth = pOrigin->pDepth;
	hOrigin = pOrigin;
	pGDICache = NULL;
	pDevice = g_client->GetDevice();
	ColorKey = pOrigin->ColorKey;
	Flags = pOrigin->Flags;
	desc = pOrigin->desc;
	Mipmaps = pOrigin->Mipmaps;
	ClientFlags = 0;
	RefCount = 1;
	pSkp = NULL;

	memset(&DC, 0, sizeof(DC));
	for (int i = 0; i < MAP_MAX_COUNT; i++) pMap[i] = pOrigin->pMap[i];

	strcpy_s(name, 128, pOrigin->name);
}


// -----------------------------------------------------------------------------------------------
// Was SAFE_RELEASE throughout. Vulkan objects are not reference counted; the
// device that created them destroys them. The ownership rule is unchanged: a
// clone destroys nothing it shares, and a back buffer destroys nothing at all
// because the swapchain image is the core's.
//
SurfNative::~SurfNative()
{
	if (SurfaceCatalog.erase(this) != 1) assert(false);

	if (hOrigin == this)
	{
		for (int i = 0; i < MAP_MAX_COUNT; i++) if (pMap[i]) { pDevice->DestroyTexture(pMap[i]); pMap[i] = NULL; }

		if (!(Flags & OAPISURFACE_BACKBUFFER))
		{
			if (pResource) { pDevice->DestroyTexture(pResource); pResource = NULL; }
			if (pDepth) { pDevice->DestroyTexture(pDepth); pDepth = NULL; }
		}
	}

	if (pTemp) { pDevice->DestroyTexture(pTemp); pTemp = NULL; }
	if (pGDICache) { pDevice->DestroyTexture(pGDICache); pGDICache = NULL; }
	SAFE_DELETE(pSkp);
}


// -----------------------------------------------------------------------------------------------
//
void SurfNative::AddMap(DWORD id, VulkanTexture *_pMap)
{
	if (id >= MAP_MAX_COUNT) return;
	if (pMap[id]) pDevice->DestroyTexture(pMap[id]);
	pMap[id] = _pMap;
	Flags |= OAPISURFACE_MAPS;
}


// -----------------------------------------------------------------------------------------------
// D3D9 could not StretchRect a surface onto itself, so in-surface blitting went
// through a scratch render target. vkCmdBlitImage has the same restriction --
// source and destination subresources must not overlap -- so the scratch stays.
//
VulkanTexture *SurfNative::GetTempSurface()
{
	if (pTemp) return pTemp;
	pTemp = pDevice->CreateTexture(desc.Width, desc.Height, 1, desc.Format,
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
	return pTemp;
}


// -----------------------------------------------------------------------------------------------
//
void SurfNative::SetColorKey(DWORD ck)
{
	ColorKey = ck;
}


// -----------------------------------------------------------------------------------------------
// Was: OAPISURFACE_BACKBUFFER, or (Pool == DEFAULT && Usage & RENDERTARGET).
// The pool test was asking "is this device-local", which is now the inverse of
// HostVisible; the usage test is the same question in Vulkan's spelling.
//
bool SurfNative::IsRenderTarget() const
{
	if (Flags & OAPISURFACE_BACKBUFFER) return true;
	if (!desc.HostVisible && (desc.Usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) return true;
	return false;
}


// -----------------------------------------------------------------------------------------------
//
bool SurfNative::IsCompressed() const
{
	// DXT2 and DXT4 were in the Windows list. They are DXT3 and DXT5 with
	// premultiplied alpha -- the same block layout, hence the same Vulkan
	// format -- so BC2 and BC3 cover all four.
	if (desc.Format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK) return true;
	if (desc.Format == VK_FORMAT_BC2_UNORM_BLOCK) return true;
	if (desc.Format == VK_FORMAT_BC3_UNORM_BLOCK) return true;
	return false;
}


// -----------------------------------------------------------------------------------------------
//
bool SurfNative::IsPowerOfTwo() const
{
	DWORD w = desc.Width, h = desc.Height;
	for (int i = 0; i < 14; i++) if ((w & 1) == 0) w = w >> 1; else { if (w != 1) return false; else break; }
	for (int i = 0; i < 14; i++) if ((h & 1) == 0) h = h >> 1; else { if (h != 1) return false; else break; }
	return true;
}


// -----------------------------------------------------------------------------------------------
// The trailing loop rewrites '/' as '\'. It is kept: the name is used as a
// TEXTURE PATH by Reload(), Decompress() and DeClone(), all of which hand it to
// g_client->TexturePath(), and the core's Config::ConfigPath translates
// separators on the way through (the porting notes). Changing the
// convention here would leave those three lookups asking for a name the core
// has not seen.
//
void SurfNative::SetName(const char* n)
{
	strcpy_s(name, 128, n);
	int i = -1;
	while (name[++i] != 0) if (name[i] == '/') name[i] = '\\';
}


// -----------------------------------------------------------------------------------------------
//
VulkanTexture *SurfNative::GetGDICache(DWORD Flags)
{
	if (pGDICache) return pGDICache;
	pGDICache = pDevice->CreateTexture(desc.Width, desc.Height, 1, VK_FORMAT_B8G8R8A8_UNORM,
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
	return pGDICache;
}


// -----------------------------------------------------------------------------------------------
//
bool SurfNative::GetSpecs(gcCore::SurfaceSpecs* sp, int size)
{
	if (size == sizeof(gcCore::SurfaceSpecs))
	{
		sp->Flags = Flags;
		sp->Width = desc.Width;
		sp->Height = desc.Height;
		sp->Mips = Mipmaps;
		return true;
	}
	return false;
}


// -----------------------------------------------------------------------------------------------
// The D3DUSAGE_AUTOGENMIPMAP branch is gone -- Vulkan has no driver-side mip
// generation -- so what remains is the explicit chain the else-branch already
// did, with vkCmdBlitImage in place of StretchRect.
//
bool SurfNative::GenerateMipMaps()
{
	if (!IsTexture()) return false;
	if (Mipmaps <= 1) return false;
	return pDevice->GenerateMipmaps(pResource);
}


// -----------------------------------------------------------------------------------------------
// Was ColorFill on a render target, or a GDI FillRect on a lockable surface.
//
// vkCmdClearColorImage is the ColorFill counterpart and takes a rectangle list,
// so it covers both cases -- including the host-visible one, which no longer
// needs a GDI round trip to fill a rectangle with a colour.
//
bool SurfNative::Fill(LPRECT rect, DWORD c)
{
	RECT re;
	if (rect == NULL) { re.left = 0; re.top = 0; re.right = desc.Width; re.bottom = desc.Height; }
	else re = *rect;

	if (pDevice->ClearImage(pResource, &re, c)) return true;

	LogErr("ColorFill Failed");
	LogSpecs();
	HALT();
	return false;
}


// -----------------------------------------------------------------------------------------------
// GetDC / ReleaseDC
//
// IDirect3DSurface9::GetDC HAS NO COUNTERPART, and neither does the problem it
// created. On Windows it worked only on a lockable surface, which is why the
// reference kept a second X8R8G8B8 copy (pDX7) purely so a render target could
// be given a DC, blitting up before and down after.
//
// Src/Orbiter/Linux/Gdi.cpp is a display-list RECORDER: CreateCompatibleDC
// hands back a DC that accumulates draw commands, and orbiter_ReplayDC turns
// them into ImGui draw data. It is not attached to an image at all, so it works
// the same for every surface and there is nothing to blit up or down. The three
// branches of the Windows GetDC -- capture, GDI surface, render target --
// collapse into one, and CreateDX7/DX7Sync go with them.
//
// What a caller gets is therefore a real DC that records; what reaches the
// screen is whatever replays it. For the client's main render surface that is
// the frame pump, via orbiter_SetSceneDC.
//
HDC	SurfNative::GetDC()
{
	if (DC.hDC) {
		LogErr("SurfNative: GetDC() Is Already Open");
		LogSpecs();
		HALT();
		return NULL;
	}

	DC.hDC = CreateCompatibleDC(NULL);
	if (!DC.hDC) {
		LogErr("SurfNative: GetDC() Failed");
		LogSpecs();
		HALT();
		return NULL;
	}
	DC.pSrf = pResource;
	return DC.hDC;
}



// -----------------------------------------------------------------------------------------------
//
void SurfNative::ReleaseDC(HDC _hDC)
{
	if (!_hDC) return;

	assert(_hDC == DC.hDC);

	// Was ReleaseDC on the surface, then DX7Sync(false) to blit the GDI copy
	// back. Nothing was drawn into an image here, so there is nothing to blit
	// back; the display list stays on the DC for whoever replays it.
	DeleteDC(DC.hDC);

	DC.pSrf = NULL;
	DC.hDC = NULL;	
}


// -----------------------------------------------------------------------------------------------
// Decompress / DeClone / Reload
//
// All three do the same thing on Windows: re-read the file from disk into a
// new texture, in an uncompressed format, and swap it in. They are converted
// together because the D3DX call at the heart of each -- one
// D3DXCreateTextureFromFileExA with a forced format -- is now
// NatCreateTextureFromFile plus, where the file is compressed, a blit through
// an uncompressed image. VulkanDevice::BlitTexture does the format conversion,
// which is what D3DX's format override was doing.
//
static VkFormat NatDecompressedFormat(VkFormat f)
{
	// D3DFMT_DXT1 -> X8R8G8B8, DXT3/DXT5 -> A8R8G8B8. Both are one Vulkan
	// format; the alpha distinction is carried by the surface flags.
	if (f == VK_FORMAT_BC1_RGBA_UNORM_BLOCK) return VK_FORMAT_B8G8R8A8_UNORM;
	if (f == VK_FORMAT_BC2_UNORM_BLOCK) return VK_FORMAT_B8G8R8A8_UNORM;
	if (f == VK_FORMAT_BC3_UNORM_BLOCK) return VK_FORMAT_B8G8R8A8_UNORM;
	return f;
}


bool SurfNative::Decompress()
{
	if (!IsCompressed()) return true;

	char path[MAX_PATH];
	if (!g_client->TexturePath(name, path)) {
		oapiWriteLogV("SurfNative::Decompress() File Not Found [%s]", path);
		return false;
	}

	VulkanTexture *pSrc = NatCreateTextureFromFile(path);
	if (!pSrc) { LogSpecs(); return false; }

	VulkanTexture *pDecomp = pDevice->CreateTexture(desc.Width, desc.Height, Mipmaps,
		NatDecompressedFormat(desc.Format),
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
		VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

	if (!pDecomp) { pDevice->DestroyTexture(pSrc); LogSpecs(); return false; }

	pDevice->BlitTexture(pDecomp, pSrc);
	pDevice->DestroyTexture(pSrc);

	if (pResource) pDevice->DestroyTexture(pResource);

	pResource = pDecomp;
	desc = pDecomp->Desc();
	Mipmaps = desc.Mips;
	Flags = OAPISURFACE_RENDERTARGET | OAPISURFACE_TEXTURE;
	return true;
}


// -----------------------------------------------------------------------------------------------
//
bool SurfNative::DeClone()
{
	char path[MAX_PATH];

	if (!IsClone()) return false;

	LogWrn("DeCloning Surface [%s] Handle=%s", name, _PTR(this));

	assert(pGDICache == NULL);
	assert(pTemp == NULL);
	assert(DC.hDC == NULL);

	if (!g_client->TexturePath(name, path)) {
		oapiWriteLogV("SurfNative::DeClone() File Not Found [%s]", path);
		return false;
	}

	VulkanTexture *pSrc = NatCreateTextureFromFile(path);
	if (!pSrc) { LogErr("DeClone Failed"); LogSpecs(); return false; }

	VulkanTexture *pTex = pDevice->CreateTexture(desc.Width, desc.Height, Mipmaps,
		NatDecompressedFormat(desc.Format),
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
		VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

	if (!pTex) { pDevice->DestroyTexture(pSrc); LogErr("DeClone Failed"); LogSpecs(); return false; }

	pDevice->BlitTexture(pTex, pSrc);
	pDevice->DestroyTexture(pSrc);

	// The clone never owned pResource, so it is not destroyed here -- it
	// belongs to hOrigin. This is the point at which this surface starts
	// owning one.
	pResource = pTex;
	hOrigin = this;
	desc = pTex->Desc();
	Mipmaps = desc.Mips;
	Flags = OAPISURFACE_RENDERTARGET | OAPISURFACE_TEXTURE;
	return true;
}

// -----------------------------------------------------------------------------------------------
//
void SurfNative::Reload()
{	
	if (pResource) { pDevice->DestroyTexture(pResource); pResource = NULL; }
	for (size_t i = 0; i < ARRAYSIZE(pMap); i++) if (pMap[i]) { pDevice->DestroyTexture(pMap[i]); pMap[i] = NULL; }

	char path[MAX_PATH];

	if (!g_client->TexturePath(name, path)) {
		oapiWriteLogV("SurfNative::Reload() File Not Found [%s]", path);
		return;
	}

	if (Flags == OAPISURFACE_TEXTURE)
	{
		uint32_t w = 0, h = 0, mips = 1;
		VkFormat fmt = VK_FORMAT_UNDEFINED;

		if (NatImageInfo(path, &w, &h, &fmt, &mips))
		{
			pResource = NatCreateTextureFromFile(path);

			if (pResource)
			{
				desc = pResource->Desc();
				Mipmaps = desc.Mips;

				if (Config->TextureMips == 2 || (Config->TextureMips == 1 && mips == 1))
					pDevice->GenerateMipmaps(pResource);

				AddMap(MAP_HEAT, NatLoadSpecialTexture(name, "heat"));
				AddMap(MAP_NORMAL, NatLoadSpecialTexture(name, "norm"));
				AddMap(MAP_SPECULAR, NatLoadSpecialTexture(name, "spec"));
				AddMap(MAP_EMISSION, NatLoadSpecialTexture(name, "emis"));
				AddMap(MAP_ROUGHNESS, NatLoadSpecialTexture(name, "rghn"));
				AddMap(MAP_METALNESS, NatLoadSpecialTexture(name, "metal"));
				AddMap(MAP_REFLECTION, NatLoadSpecialTexture(name, "refl"));
				AddMap(MAP_TRANSLUCENCE, NatLoadSpecialTexture(name, "transl"));
				AddMap(MAP_TRANSMITTANCE, NatLoadSpecialTexture(name, "transm"));
			}
		}
	}
}



// -----------------------------------------------------------------------------------------------
//
void SurfNative::LogSpecs() const
{
	LogErr("Surface name is [%s] OAPI_Handle=%s", name, _PTR(this));
	if (pTemp) LogErr("Has a In-surface temp layer");
	if (pDepth) LogErr("Has a DepthStencil surface");
	if (DC.hDC) LogErr("Has a HDC [%s]", _PTR(DC.hDC));
	LogErr("OAPI_Attribs: %s", NatOAPIFlags(Flags));
	NatDumpResource(pResource);
}



// -----------------------------------------------------------------------------------------------
//
DWORD SurfNative::GetTextureSizeInBytes(VulkanTexture *pT)
{
	if (!pT) return 0;
	DWORD size = StaticFormatSizeInBytes(pT->Format(), pT->Height() * pT->Width());
	if (pT->Mips() > 1) size += ((size>>2) + (size>>4) + (size>>6));
	return size;
}

// -----------------------------------------------------------------------------------------------
//
DWORD SurfNative::GetFormatSizeInBytes(VkFormat Format, DWORD pixels)
{
	return StaticFormatSizeInBytes(Format, pixels);
}

// -----------------------------------------------------------------------------------------------
// The same table the Windows member carried, reachable without an instance --
// VulkanUtil.cpp's CreateVolumeTexture needs the answer holding only a texture.
//
DWORD SurfNative::StaticFormatSizeInBytes(VkFormat Format, DWORD pixels)
{
	if (Format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK) return pixels >> 1;	// DXT1
	if (Format == VK_FORMAT_BC2_UNORM_BLOCK) return pixels;				// DXT3
	if (Format == VK_FORMAT_BC3_UNORM_BLOCK) return pixels;				// DXT5
	if (Format == VK_FORMAT_B8G8R8A8_UNORM) return pixels * 4;			// A8R8G8B8 / X8R8G8B8
	if (Format == VK_FORMAT_R8G8B8A8_UNORM) return pixels * 4;
	if (Format == VK_FORMAT_R5G6B5_UNORM_PACK16) return pixels * 2;
	if (Format == VK_FORMAT_R8G8B8_UNORM) return pixels * 3;
	if (Format == VK_FORMAT_R16_UNORM) return pixels * 2;				// L16
	if (Format == VK_FORMAT_R16_SFLOAT) return pixels * 2;
	if (Format == VK_FORMAT_R16G16_SFLOAT) return pixels * 4;
	if (Format == VK_FORMAT_R32_SFLOAT) return pixels * 4;
	if (Format == VK_FORMAT_R32G32_SFLOAT) return pixels * 8;
	if (Format == VK_FORMAT_R8_UNORM) return pixels;					// L8 / A8
	if (Format == VK_FORMAT_R32G32B32A32_SFLOAT) return pixels * 16;
	if (Format == VK_FORMAT_R16G16B16A16_SFLOAT) return pixels * 8;
	// D3DFMT_A4R4G4B4 was in the Windows list at 2 bytes. It has no place in
	// this client -- NatConvertFormat_OAPI_to_VK never produces it and the
	// loader rejected it on Windows too -- so it is not carried over.
	return pixels;
}


// -----------------------------------------------------------------------------------------------
// Was two branches on GetType(): a surface measured its one level, a texture
// measured its chain plus every additional map. One image type, one path.
//
DWORD SurfNative::GetSizeInBytes()
{
	DWORD size = GetTextureSizeInBytes(pResource);
	for (int i = 0; i < MAP_MAX_COUNT; i++) size += GetTextureSizeInBytes(pMap[i]);
	return size;
}


// -----------------------------------------------------------------------------------------------
//
DWORD* SurfNative::GetClientFlags()
{
	return &ClientFlags;
}


// -----------------------------------------------------------------------------------------------
//
VulkanPad * SurfNative::GetPooledSketchPad()
{
	if (!IsRenderTarget()) {
		LogErr("Can't optain a Sketchpad to a non-render target surface %s", _PTR(this));
		assert(false);
		return NULL;
	}
	if (!pSkp) pSkp = new VulkanPad(this, "SurfNative.Pooled");
	return pSkp;
}



// -----------------------------------------------------------------------------------------------
//
bool NatCreateName(char* out, int mlen, const char* fname, const char* id)
{
	char buffe[MAX_PATH];
	strcpy_s(buffe, MAX_PATH, fname);
	char* p = strrchr(buffe, '.');
	if (p != NULL) {
		*p = '\0';
		sprintf_s(out, mlen, "%s_%s.%s", buffe, id, ++p);
	}
	return (p != NULL);
}


// -----------------------------------------------------------------------------------------------
// Was NatUsage(DWORD) decoding D3DUSAGE_AUTOGENMIPMAP / RENDERTARGET / DYNAMIC.
// AUTOGENMIPMAP and DYNAMIC have no counterpart -- the first because Vulkan
// generates no mipmaps, the second because host visibility is a property of
// the memory, reported by NatMemory below.
//
const char* NatUsage(VkImageUsageFlags Usage)
{
	static char buf[128];
	strcpy_s(buf, 128, "");
	if (Usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) strcat_s(buf, "COLOR_ATTACHMENT ");
	if (Usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) strcat_s(buf, "DEPTH_STENCIL ");
	if (Usage & VK_IMAGE_USAGE_SAMPLED_BIT) strcat_s(buf, "SAMPLED ");
	if (Usage & VK_IMAGE_USAGE_STORAGE_BIT) strcat_s(buf, "STORAGE ");
	if (Usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) strcat_s(buf, "TRANSFER_SRC ");
	if (Usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) strcat_s(buf, "TRANSFER_DST ");
	if (Usage == 0) strcat_s(buf, "NONE ");
	return buf;
}


// -----------------------------------------------------------------------------------------------
// Was NatPool(D3DPOOL). D3DPOOL_DEFAULT / SYSTEMMEM / MANAGED described where
// a resource lived and who could touch it. Vulkan has memory property flags
// instead, and the only distinction this client makes is whether the CPU can
// reach it -- MANAGED, the pool that kept a shadow copy and uploaded for you,
// has no counterpart at all.
//
const char* NatMemory(bool bHostVisible)
{
	static char buf[64];
	strcpy_s(buf, 64, bHostVisible ? "HOST_VISIBLE" : "DEVICE_LOCAL");
	return buf;
}


// -----------------------------------------------------------------------------------------------
//
const char* NatOAPIFlags(DWORD AF)
{
	static char buf[512]; strcpy_s(buf, 512, "");

	if (AF & OAPISURFACE_TEXTURE)		strcat_s(buf, 512, "OAPISURFACE_TEXTURE, ");
	if (AF & OAPISURFACE_RENDERTARGET)	strcat_s(buf, 512, "OAPISURFACE_RENDERTARGET, ");
	if (AF & OAPISURFACE_GDI)			strcat_s(buf, 512, "OAPISURFACE_GDI, ");
	if (AF & OAPISURFACE_SKETCHPAD)		strcat_s(buf, 512, "OAPISURFACE_SKETCHPAD, ");
	if (AF & OAPISURFACE_MIPMAPS)		strcat_s(buf, 512, "OAPISURFACE_MIPMAPS, ");
	if (AF & OAPISURFACE_NOMIPMAPS)		strcat_s(buf, 512, "OAPISURFACE_NOMIPMAPS, ");
	if (AF & OAPISURFACE_ALPHA)			strcat_s(buf, 512, "OAPISURFACE_ALPHA, ");
	if (AF & OAPISURFACE_NOALPHA)		strcat_s(buf, 512, "OAPISURFACE_NOALPHA, ");
	if (AF & OAPISURFACE_UNCOMPRESS)	strcat_s(buf, 512, "OAPISURFACE_UNCOMPRESS, ");
	if (AF & OAPISURFACE_SYSMEM)		strcat_s(buf, 512, "OAPISURFACE_SYSMEM, ");
	if (AF & OAPISURFACE_ANTIALIAS)		strcat_s(buf, 512, "OAPISURFACE_ANTIALIAS, ");
	if (AF & OAPISURFACE_RENDER3D)		strcat_s(buf, 512, "OAPISURFACE_RENDER3D, ");
	return buf;
}


// -----------------------------------------------------------------------------------------------
//
const char* NatOAPIFormat(DWORD PF)
{
	static char buf[64];
	strcpy_s(buf, 64, "UNKNOWN");
	DWORD AF = PF & OAPISURFACE_PF_MASK;

	if (AF == OAPISURFACE_PF_XRGB)	strcpy_s(buf, 64, "OAPISURFACE_PF_XRGB ");
	if (AF == OAPISURFACE_PF_ARGB)	strcpy_s(buf, 64, "OAPISURFACE_PF_ARGB ");
	if (AF == OAPISURFACE_PF_RGB565)strcpy_s(buf, 64, "OAPISURFACE_PF_RGB565 ");
	if (AF == OAPISURFACE_PF_S16R)	strcpy_s(buf, 64, "OAPISURFACE_PF_S16R ");
	if (AF == OAPISURFACE_PF_F32R)	strcpy_s(buf, 64, "OAPISURFACE_PF_F32R ");
	if (AF == OAPISURFACE_PF_F32RG)	strcpy_s(buf, 64, "OAPISURFACE_PF_F32RG ");
	if (AF == OAPISURFACE_PF_F32RGBA)strcpy_s(buf, 64, "OAPISURFACE_PF_F32RGBA ");
	if (AF == OAPISURFACE_PF_F16R)	strcpy_s(buf, 64, "OAPISURFACE_PF_F16R ");
	if (AF == OAPISURFACE_PF_F16RG)	strcpy_s(buf, 64, "OAPISURFACE_PF_F16RG ");
	if (AF == OAPISURFACE_PF_F16RGBA)strcpy_s(buf, 64, "OAPISURFACE_PF_F16RGBA ");
	if (AF == OAPISURFACE_PF_DXT1)	strcpy_s(buf, 64, "OAPISURFACE_PF_DXT1 ");
	if (AF == OAPISURFACE_PF_DXT3)	strcpy_s(buf, 64, "OAPISURFACE_PF_DXT3 ");
	if (AF == OAPISURFACE_PF_DXT5)	strcpy_s(buf, 64, "OAPISURFACE_PF_DXT5 ");
	if (AF == OAPISURFACE_PF_ALPHA)	strcpy_s(buf, 64, "OAPISURFACE_PF_ALPHA ");
	if (AF == OAPISURFACE_PF_GRAY)	strcpy_s(buf, 64, "OAPISURFACE_PF_GRAY ");
	return buf;
}


// -----------------------------------------------------------------------------------------------
// The sType[] table and the GetType() branch are gone: there is one image type
// and it does not report itself, so the dump prints what the client recorded.
//
void NatDumpResource(VulkanTexture *pResource)
{
	if (!pResource) { oapiWriteLog((char*)"VK_DUMP: (null)"); return; }

	const VulkanImageDesc &d = pResource->Desc();
	DWORD f = NatConvertFormat_VK_to_OAPI(d.Format);

	oapiWriteLogV("VK_DUMP: Mips = %u", d.Mips);
	oapiWriteLogV("VK_DUMP: Usage = %s", NatUsage(d.Usage));
	oapiWriteLogV("VK_DUMP: Memory = %s", NatMemory(d.HostVisible));
	oapiWriteLogV("VK_DUMP: Format = %s (%d)", NatOAPIFormat(f), int(d.Format));
	oapiWriteLogV("VK_DUMP: Samples = %u", (unsigned)d.Samples);
	oapiWriteLogV("VK_DUMP: Size = (%u, %u)", d.Width, d.Height);
}
