// ==============================================================
// File: VulkanFrame.cpp
// Desc: Class functions to implement the Vulkan app framework.
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2007-2026 Martin Schweiger
//				 2011 - 2016 Jarmo Nikkanen
// ==============================================================
//
// CONVERTED FROM OVP/D3D9Client/D3D9Frame.cpp, read end to end (641 lines),
// against D3D9Frame.h read end to end (146 lines).
//
// This file holds two things: the conversion of CD3DFramework9, and the
// method bodies of VulkanDevice, VulkanTexture and VulkanBuffer.
//
// THE SECOND HALF HAS NO WINDOWS ORIGINAL AND THAT IS THE POINT. Every one of
// those bodies implements something IDirect3DDevice9 already did:
// CreateTexture, CreateVertexBuffer, LockRect, GetRenderTargetData,
// StretchRect, ColorFill, GetSurfaceLevel, Release. D3D9 supplied them because
// its device owned an allocator, a resource pool and an upload path. Vulkan
// supplies none of that -- vkCreateImage does not allocate, mapping
// device-local memory is not allowed, and nothing changes an image's layout
// for you -- so the code D3D9 hid has to exist somewhere, and this is where.
// It is not an emulation layer: nothing here has a D3D9 name, a D3D9 signature
// or a D3D9 enum in it, and no call goes through anything but Vulkan.
//
// WHAT LEFT THE WINDOWS FILE, item by item:
//
//   The eleven IDirect3DVertexDeclaration9 globals at the top, and the eleven
//   CreateVertexDeclaration calls that filled them, ARE GONE FROM THIS FILE.
//   A Vulkan vertex layout is not a device object -- it is immutable data
//   baked into a VkPipeline -- so the declarations are constants and live
//   with their tables in VulkanUtil.cpp, where the VDECL macro defines the
//   same eleven names.
//
//   CreateFullscreenMode() and CreateWindowedMode() BECOME ONE FUNCTION,
//   AdoptDevice(). They differed only in the D3DPRESENT_PARAMETERS they filled
//   in before CreateDevice; the swapchain here is the core's, so there are no
//   present parameters to fill and no device to create, and what is left --
//   take the extent, describe the attachments, wrap them in the back-buffer
//   SURFHANDLE -- was already identical in both.
//
//   The 40-odd CAPS lines become the Vulkan queries that answer the same
//   questions. Where a question has no counterpart, the log line says so
//   rather than printing a fabricated number: MaxTextureBlendStages,
//   MaxSimultaneousTextures and MaxVertexBlendMatrices are fixed-function
//   limits, and Vulkan has no fixed-function pipeline to limit.
//
//   D3DXCreateFontIndirect x2 is gone with LPD3DXFONT. See the note in
//   VulkanFrame.h: it wraps GDI glyph rasterisation, and Gdi.cpp here is a
//   display-list recorder with no glyphs to rasterise.
// ==============================================================

#define STRICT

#define _CRT_SECURE_NO_DEPRECATE

#include <windows.h>
#include "GraphicsAPI.h"
#include "VulkanFrame.h"
#include "VulkanUtil.h"
#include "AABBUtil.h"
#include "VulkanSurface.h"
#include "Log.h"
#include "VulkanConfig.h"
#include "OapiExtension.h"

#include <string.h>
#include <vector>

using namespace oapi;

// The eleven vertex declarations stood here as
//     IDirect3DVertexDeclaration9 *pMeshVertexDecl = NULL;   (and ten more)
// and were created in Initialize() and released in DestroyObjects(). They are
// now `const VertexDecl *` constants defined by the VDECL macro at the head of
// VulkanUtil.cpp, next to the attribute tables they describe. Nothing is
// created and nothing is released, because a vertex layout is no longer an
// object -- see the file header.

static const char *vkmessage = { "A Vulkan-capable graphics driver is required and was not found\0" };


//-----------------------------------------------------------------------------
// The two things every Vulkan resource call needs and D3D9 never mentioned.
//-----------------------------------------------------------------------------

// Which aspect of an image a barrier or a copy is talking about.
//
// D3D9 had no such notion: a surface was colour or it was depth, and the API
// knew which from the format without ever saying so. Vulkan makes it explicit
// in every VkImageSubresourceRange, so the format has to be turned back into
// an aspect mask in one place rather than at forty call sites.
static VkImageAspectFlags AspectOf(VkFormat fmt)
{
	switch (fmt) {
	case VK_FORMAT_D16_UNORM:
	case VK_FORMAT_X8_D24_UNORM_PACK32:
	case VK_FORMAT_D32_SFLOAT:
		return VK_IMAGE_ASPECT_DEPTH_BIT;
	case VK_FORMAT_D16_UNORM_S8_UINT:
	case VK_FORMAT_D24_UNORM_S8_UINT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
		return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	case VK_FORMAT_S8_UINT:
		return VK_IMAGE_ASPECT_STENCIL_BIT;
	default:
		return VK_IMAGE_ASPECT_COLOR_BIT;
	}
}

// One image-layout transition, with the pipeline stages and access masks the
// synchronisation rules demand for it.
//
// THIS IS THE PIECE OF D3D9 THAT WAS ENTIRELY INVISIBLE. A D3D9 resource is
// always usable: you could sample a texture, then render into it, then lock
// it, and the runtime inserted whatever the hardware needed. A VkImage is
// valid only in the right VkImageLayout for the operation, and moving between
// layouts is an explicit barrier that the application must issue. Every
// Upload, Read, Blit, Clear and mip generation below is barrier, operation,
// barrier -- and this function is that barrier.
//
// THE LAYER DEFAULT IS "ALL OF THEM", TO MATCH THE MIP DEFAULT, and that is
// what makes the layout invariant stated on UploadTexture true of a CUBE as
// well as of a 2D image.
//
// It read layerCount = 1. For every 2D image that is the whole image and the
// two spellings are identical -- arrayLayers is 1 -- so nothing about the 2D
// path changes. For a six-layer cube it transitioned FACE +X ONLY, while
// SetLayout() went on to record the settled layout for the whole texture. The
// other five faces stayed in VK_IMAGE_LAYOUT_UNDEFINED with nothing tracking
// that they had, and sampling the cube then read them:
//
//     VUID-vkCmdDraw-None-09600  ... expects VK_IMAGE_LAYOUT_SHADER_READ_ONLY_
//     OPTIMAL ... is in VK_IMAGE_LAYOUT_UNDEFINED
//
// No call site passes either layer argument, so this is the only behaviour it
// can change. D3D9 has nothing to convert here: a cube face was a surface you
// could use at any time, with no layout to get wrong.
static void Barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
					VkImageLayout from, VkImageLayout to,
					uint32_t baseMip = 0, uint32_t mipCount = VK_REMAINING_MIP_LEVELS,
					uint32_t baseLayer = 0, uint32_t layerCount = VK_REMAINING_ARRAY_LAYERS)
{
	if (image == VK_NULL_HANDLE || from == to) return;

	VkImageMemoryBarrier b = {};
	b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange.aspectMask = aspect;
	b.subresourceRange.baseMipLevel = baseMip;
	b.subresourceRange.levelCount = mipCount;
	b.subresourceRange.baseArrayLayer = baseLayer;
	b.subresourceRange.layerCount = layerCount;

	VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
	VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

	switch (from) {
	case VK_IMAGE_LAYOUT_UNDEFINED:
		b.srcAccessMask = 0;
		srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		break;
	case VK_IMAGE_LAYOUT_PREINITIALIZED:
		b.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
		srcStage = VK_PIPELINE_STAGE_HOST_BIT;
		break;
	case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
		b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		break;
	case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
		b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		break;
	case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
		b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
		srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		break;
	case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
		b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		break;
	case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
		b.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		srcStage = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		break;
	default:
		b.srcAccessMask = 0;
		srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		break;
	}

	switch (to) {
	case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
		b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		break;
	case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
		b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		break;
	case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
		b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		dstStage = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
				   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		break;
	case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
		b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
						  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		break;
	case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
		b.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
						  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		dstStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		break;
	case VK_IMAGE_LAYOUT_GENERAL:
		b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
						  VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT;
		// VK_PIPELINE_STAGE_HOST_BIT IS NOT PART OF ALL_COMMANDS, and the two
		// HOST access bits above are legal only in a barrier that names it.
		// ALL_COMMANDS expands to the queue's pipeline stages; the host is not
		// one of them, so this was:
		//
		//     VUID-vkCmdPipelineBarrier-pImageMemoryBarriers-02820
		//     dstAccessMask (VK_ACCESS_2_HOST_READ_BIT) is not supported by
		//     stage mask (VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT)
		//
		// twenty times per frame. GENERAL is the layout this client uses for
		// an image it is about to map, so the host bits are the point of the
		// transition and the stage has to be added rather than the access
		// bits dropped.
		dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT;
		break;
	default:
		b.dstAccessMask = 0;
		dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		break;
	}

	vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, NULL, 0, NULL, 1, &b);
}


// ===========================================================================
// VulkanTexture
//
// Counterpart of IDirect3DTexture9 / IDirect3DSurface9 / IDirect3DVolumeTexture9.
// ===========================================================================

VulkanTexture::VulkanTexture() :
	vkOwner(VK_NULL_HANDLE),
	vkImage(VK_NULL_HANDLE),
	vkMemory(VK_NULL_HANDLE),
	vkView(VK_NULL_HANDLE),
	vkAttachView(VK_NULL_HANDLE),
	vkLayout(VK_IMAGE_LAYOUT_UNDEFINED),
	mapped(NULL),
	pitch(0),
	bOwnsImage(true),
	bAlphaOne(false)		// see SetAlphaOne
{
	desc = VulkanImageDesc();
	desc.Format = VK_FORMAT_UNDEFINED;
	desc.Depth = 1;
	desc.Mips = 1;
	desc.Layers = 1;	// a cube map says 6; see CreateTextureCube
	desc.Samples = VK_SAMPLE_COUNT_1_BIT;
}

// ---------------------------------------------------------------------------
// Name: VulkanTexture::AttachmentView
// Desc: The view to bind when this image is a framebuffer attachment.
//
//       A framebuffer attachment must name EXACTLY ONE mip level. vkView is
//       the sampling view and spans the whole chain, so handing it to
//       vkCreateFramebuffer is:
//
//           VUID-VkFramebufferCreateInfo-pAttachments-00883
//           pAttachments[0] has mip levelCount of 10 but only a single mip
//           level is allowed when creating a Framebuffer
//
//       D3D9 never had to distinguish the two: a texture was sampled through
//       IDirect3DTexture9 and rendered into through the IDirect3DSurface9 that
//       GetSurfaceLevel(0) returned, so the per-level view was already there.
//       Here it is built on demand and kept for the life of the texture.
//
//       An unmipped image needs nothing: its own view already names one level.
// ---------------------------------------------------------------------------
VkImageView VulkanTexture::AttachmentView()
{
	// bAlphaOne JOINS THE MIP COUNT AS A REASON TO BUILD A SECOND VIEW.
	// SetAlphaOne swizzles the SAMPLING view's alpha to 1, which is what
	// D3DFMT_X8R8G8B8 did -- and a framebuffer attachment may not be swizzled
	// at all (VUID-VkFramebufferCreateInfo-pAttachments-00884 requires the
	// identity mapping). So a surface that is both sampled and rendered into,
	// which is every MFD and HUD surface, needs the plain view here even when
	// it has only one mip.
	if (desc.Mips <= 1 && !bAlphaOne) return vkView;
	if (vkAttachView != VK_NULL_HANDLE) return vkAttachView;
	if (vkOwner == VK_NULL_HANDLE || vkImage == VK_NULL_HANDLE) return vkView;

	VkImageViewCreateInfo vci = {};
	vci.sType	 = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vci.image	 = vkImage;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format	 = desc.Format;
	vci.subresourceRange.aspectMask = AspectOf(desc.Format);
	vci.subresourceRange.baseMipLevel	= 0;
	vci.subresourceRange.levelCount		= 1;
	vci.subresourceRange.baseArrayLayer	= 0;
	vci.subresourceRange.layerCount		= 1;

	if (vkCreateImageView(vkOwner, &vci, NULL, &vkAttachView) != VK_SUCCESS) {
		LogErr("VulkanTexture::AttachmentView: vkCreateImageView failed");
		vkAttachView = VK_NULL_HANDLE;
		return vkView;			// wrong, but no worse than before
	}
	return vkAttachView;
}


// ---------------------------------------------------------------------------
// Name: VulkanTexture::SetAlphaOne
// Desc: THE COUNTERPART OF D3DFMT_X8R8G8B8, WHICH VULKAN HAS NO FORMAT FOR.
//
//       D3D9ClientSurface::NatCreateSurface picks the surface format like
//       this (D3D9Surface.cpp:322):
//
//           if (flags & OAPISURFACE_ALPHA) Format = D3DFMT_A8R8G8B8;
//           else                           Format = D3DFMT_X8R8G8B8;
//
//       X8R8G8B8 is four bytes wide and HAS NO ALPHA CHANNEL: the eight bits
//       are there but they are not part of the format, and a sampler reading
//       that texture gets alpha = 1.0 no matter what is in memory.
//
//       VULKAN HAS NO X8 FORMAT. VK_FORMAT_B8G8R8A8_UNORM is the only 32-bit
//       BGRA there is, and it means what it says -- the alpha byte is read
//       back. So a surface the reference would have made X8R8G8B8 samples
//       with whatever alpha happens to be in it, and for every surface the
//       Sketchpad draws into that is ZERO: D3D9Pad::Flush sets
//       D3DRS_COLORWRITEENABLE to 0x7 for its ALPHABLEND state, so the pad
//       writes colour and never touches alpha, and VirtualCockpit::ClearHUD
//       clears the surface to 0x00000000 first.
//
//       WHAT THAT COST: the virtual-cockpit HUD. The surface held a perfectly
//       drawn green HUD -- SRFCE, the heading tape, the airspeed and altitude
//       boxes, the pitch ladder -- with alpha 0 on every one of its 262144
//       pixels, and the mesh pass that draws the HUD glass multiplies by that
//       alpha. Measured, not deduced: `4753 of 262144 pixels have colour, 0
//       have alpha; max BGRA = 0 255 0 0`.
//
//       The exact equivalent of "this format has no alpha channel" is a
//       component swizzle on the VIEW -- VK_COMPONENT_SWIZZLE_ONE for A --
//       which is where Vulkan puts the question. The storage is unchanged, so
//       every blit, readback and clear still sees the real bytes; only what a
//       SAMPLER reads changes, which is precisely the difference between
//       X8R8G8B8 and A8R8G8B8.
//
//       Not a default: it is applied only where the reference would have
//       chosen X8R8G8B8. See NatCreateSurface.
// ---------------------------------------------------------------------------
bool VulkanTexture::SetAlphaOne()
{
	if (bAlphaOne) return true;
	if (vkOwner == VK_NULL_HANDLE || vkImage == VK_NULL_HANDLE) return false;
	if (vkView == VK_NULL_HANDLE) return false;		// nothing samples it

	VkImageViewCreateInfo vci = {};
	vci.sType	 = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	vci.image	 = vkImage;
	vci.viewType = (desc.Depth > 1) ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
	vci.format	 = desc.Format;
	vci.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
	vci.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
	vci.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
	vci.components.a = VK_COMPONENT_SWIZZLE_ONE;
	vci.subresourceRange.aspectMask		= AspectOf(desc.Format);
	vci.subresourceRange.baseMipLevel	= 0;
	vci.subresourceRange.levelCount		= desc.Mips;
	vci.subresourceRange.baseArrayLayer	= 0;
	vci.subresourceRange.layerCount		= 1;

	VkImageView view = VK_NULL_HANDLE;
	if (vkCreateImageView(vkOwner, &vci, NULL, &view) != VK_SUCCESS) {
		LogErr("VulkanTexture::SetAlphaOne: vkCreateImageView failed");
		return false;
	}

	// Called at creation, before anything can have written a descriptor
	// naming the old view, so destroying it here is safe.
	vkDestroyImageView(vkOwner, vkView, NULL);
	vkView = view;
	bAlphaOne = true;
	return true;
}


// THE FREEING IS HERE, NOT IN VulkanDevice::DestroyTexture, and the reason is
// worth stating because the header's note is easy to read the other way round.
// vkDestroyImage needs a VkDevice, and vkOwner is exactly that -- recorded
// when the image was made. DestroyTexture exists for the CALL SITE: the pools
// in VulkanCatalog.h see this class only through the forward declaration in
// VulkanTypes.h, and `delete x` on an incomplete type runs no destructor at
// all. Routing the call through the device is what makes the type complete at
// the point of deletion.
VulkanTexture::~VulkanTexture()
{
	if (vkOwner == VK_NULL_HANDLE) return;

	if (mapped) {
		vkUnmapMemory(vkOwner, vkMemory);
		mapped = NULL;
	}

	// The view is this object's even when the image is not: a mip view owns
	// its VkImageView and shares its parent's VkImage, exactly as
	// GetSurfaceLevel(n) returned a new interface onto the same storage.
	if (vkView != VK_NULL_HANDLE) {
		vkDestroyImageView(vkOwner, vkView, NULL);
		vkView = VK_NULL_HANDLE;
	}

	// The lazy attachment view, when one was built. It aliases the same image
	// and is owned here exactly as vkView is.
	if (vkAttachView != VK_NULL_HANDLE) {
		vkDestroyImageView(vkOwner, vkAttachView, NULL);
		vkAttachView = VK_NULL_HANDLE;
	}

	if (bOwnsImage) {
		if (vkImage != VK_NULL_HANDLE) {
			vkDestroyImage(vkOwner, vkImage, NULL);
			vkImage = VK_NULL_HANDLE;
		}
		if (vkMemory != VK_NULL_HANDLE) {
			vkFreeMemory(vkOwner, vkMemory, NULL);
			vkMemory = VK_NULL_HANDLE;
		}
	}
}

// Counterpart of IDirect3DTexture9::LockRect.
//
// D3D9 would lock any resource in D3DPOOL_SYSTEMMEM or D3DPOOL_MANAGED and, on
// a default-pool resource, fail. Vulkan draws the same line but by memory
// rather than by pool: memory without HOST_VISIBLE cannot be mapped at all.
// So this returns NULL for a device-local image and the caller stages, which
// is what the D3D9 code already did whenever the lock failed.
void *VulkanTexture::Map()
{
	if (!desc.HostVisible || vkMemory == VK_NULL_HANDLE) return NULL;
	if (mapped) return mapped;
	if (vkMapMemory(vkOwner, vkMemory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
		mapped = NULL;
		return NULL;
	}
	return mapped;
}

void VulkanTexture::Unmap()
{
	if (!mapped) return;

	// The memory a host-visible image is bound to is allocated HOST_COHERENT
	// below, so there is no vkFlushMappedMemoryRanges here. That is a property
	// of the allocation, not an omission: see CreateTexture.
	vkUnmapMemory(vkOwner, vkMemory);
	mapped = NULL;
}


// ===========================================================================
// VulkanBuffer
//
// Counterpart of IDirect3DVertexBuffer9 and IDirect3DIndexBuffer9 -- two
// interfaces in D3D9, one type here, because Vulkan tells them apart by usage
// flags rather than by type.
// ===========================================================================

VulkanBuffer::VulkanBuffer() :
	vkOwner(VK_NULL_HANDLE),
	vkBuffer(VK_NULL_HANDLE),
	vkMemory(VK_NULL_HANDLE),
	bytes(0),
	usage(0),
	hostVisible(false),
	mapped(NULL)
{
}

VulkanBuffer::~VulkanBuffer()
{
	if (vkOwner == VK_NULL_HANDLE) return;

	if (mapped) {
		vkUnmapMemory(vkOwner, vkMemory);
		mapped = NULL;
	}
	if (vkBuffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(vkOwner, vkBuffer, NULL);
		vkBuffer = VK_NULL_HANDLE;
	}
	if (vkMemory != VK_NULL_HANDLE) {
		vkFreeMemory(vkOwner, vkMemory, NULL);
		vkMemory = VK_NULL_HANDLE;
	}
}

// Counterpart of Lock(offset, size, &ppbData, flags).
//
// The flags have no counterpart and need none. D3DLOCK_DISCARD and
// D3DLOCK_NOOVERWRITE told the driver whether it could rename the buffer
// behind you or had to wait; Vulkan never renames anything and never waits, so
// the promise those flags made -- "this will not stall" -- is the only
// behaviour available, and keeping the buffer's memory mapped for its whole
// life is how it is kept.
void *VulkanBuffer::Map(VkDeviceSize offset, VkDeviceSize length)
{
	if (!hostVisible || vkMemory == VK_NULL_HANDLE) return NULL;

	if (mapped) {
		// Already mapped from a previous Lock. D3D9 permitted nested locks on
		// the same buffer and returned the same pointer; so does this, offset
		// by the caller's own offset.
		return (char *)mapped + offset;
	}
	if (vkMapMemory(vkOwner, vkMemory, offset, length, 0, &mapped) != VK_SUCCESS) {
		mapped = NULL;
		return NULL;
	}
	return mapped;
}

void VulkanBuffer::Unmap()
{
	if (!mapped) return;
	vkUnmapMemory(vkOwner, vkMemory);
	mapped = NULL;
}


// ===========================================================================
// VulkanDevice
//
// Counterpart of LPDIRECT3DDEVICE9. See the class note in VulkanFrame.h for
// why it has no SetRenderState, no SetTexture and no DrawPrimitive.
// ===========================================================================

VulkanDevice::VulkanDevice() :
	vkInstance(VK_NULL_HANDLE),
	vkPhysical(VK_NULL_HANDLE),
	vkDevice(VK_NULL_HANDLE),
	vkQueue(VK_NULL_HANDLE),
	queueFamily(0),
	vkDescriptorPool(VK_NULL_HANDLE),
	vkRenderPass(VK_NULL_HANDLE),
	vkCommandPool(VK_NULL_HANDLE),
	vkPipelineCache(VK_NULL_HANDLE),
	vkCmd(VK_NULL_HANDLE),
	frameWidth(0),
	frameHeight(0),
	vkActivePass(VK_NULL_HANDLE),
	vkSavedCmd(VK_NULL_HANDLE),
	savedWidth(0),
	savedHeight(0),
	offColourCount(0),
	offDepth(NULL),
	polygonMode(VK_POLYGON_MODE_FILL)		// D3DFILL_SOLID
{
	memset(&props, 0, sizeof(props));
	memset(&memProps, 0, sizeof(memProps));
	memset(&features, 0, sizeof(features));
	memset(offColour, 0, sizeof(offColour));
}

VulkanDevice::~VulkanDevice()
{
	Destroy();
}

// Counterpart of IDirect3D9::CreateDevice, and the one place the difference in
// ownership is decided. Nothing here is created except the two objects the
// core does not have: a command pool for uploads (the core's pools belong to
// its frames and are reset under it) and a pipeline cache.
bool VulkanDevice::Adopt(const OrbiterVulkanContext &ctx)
{
	vkInstance		 = (VkInstance)ctx.instance;
	vkPhysical		 = (VkPhysicalDevice)ctx.physicalDevice;
	vkDevice		 = (VkDevice)ctx.device;
	vkQueue			 = (VkQueue)ctx.queue;
	queueFamily		 = ctx.queueFamily;
	vkDescriptorPool = (VkDescriptorPool)ctx.descriptorPool;
	vkRenderPass	 = (VkRenderPass)ctx.renderPass;

	if (vkDevice == VK_NULL_HANDLE || vkPhysical == VK_NULL_HANDLE ||
		vkQueue == VK_NULL_HANDLE || vkRenderPass == VK_NULL_HANDLE) {
		LogErr("VulkanDevice::Adopt: the core published an incomplete context");
		return false;
	}

	// Was GetDeviceCaps + GetAdapterIdentifier. One D3DCAPS9 struct became
	// three separate queries, because Vulkan keeps limits, memory layout and
	// optional features in three different places.
	vkGetPhysicalDeviceProperties(vkPhysical, &props);
	vkGetPhysicalDeviceMemoryProperties(vkPhysical, &memProps);
	vkGetPhysicalDeviceFeatures(vkPhysical, &features);

	VkCommandPoolCreateInfo cpci = {};
	cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT |
				 VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
	cpci.queueFamilyIndex = queueFamily;
	if (vkCreateCommandPool(vkDevice, &cpci, NULL, &vkCommandPool) != VK_SUCCESS) {
		LogErr("VulkanDevice::Adopt: vkCreateCommandPool failed");
		vkCommandPool = VK_NULL_HANDLE;
		return false;
	}

	// No D3D9 counterpart. D3D9 had no pipelines to cache -- render state was
	// set piecemeal and the driver assembled what it needed behind the API.
	// Every state combination is a VkPipeline here, so the cache is what keeps
	// the first frame after a technique change from stalling.
	VkPipelineCacheCreateInfo pcci = {};
	pcci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
	if (vkCreatePipelineCache(vkDevice, &pcci, NULL, &vkPipelineCache) != VK_SUCCESS)
		vkPipelineCache = VK_NULL_HANDLE;	// not fatal; pipelines still build

	return true;
}

// Was SAFE_RELEASE(pDevice). It is not the same thing and must not be: the
// device, queue, descriptor pool and render pass are the core's, and
// destroying them here would take the Launchpad down with the session.
void VulkanDevice::Destroy()
{
	if (vkDevice == VK_NULL_HANDLE) return;

	// The offscreen caches, which are this class's own objects and not the
	// core's. Destroyed before the device handle is dropped, because
	// vkDestroyFramebuffer and vkDestroyRenderPass both take it.
	for (auto &x : offFbCache) if (x.second) vkDestroyFramebuffer(vkDevice, x.second, NULL);
	for (auto &x : offPassCache) if (x.second) vkDestroyRenderPass(vkDevice, x.second, NULL);
	offFbCache.clear();
	offPassCache.clear();

	if (vkPipelineCache != VK_NULL_HANDLE) {
		vkDestroyPipelineCache(vkDevice, vkPipelineCache, NULL);
		vkPipelineCache = VK_NULL_HANDLE;
	}
	if (vkCommandPool != VK_NULL_HANDLE) {
		vkDestroyCommandPool(vkDevice, vkCommandPool, NULL);
		vkCommandPool = VK_NULL_HANDLE;
	}

	vkInstance		 = VK_NULL_HANDLE;
	vkPhysical		 = VK_NULL_HANDLE;
	vkDevice		 = VK_NULL_HANDLE;
	vkQueue			 = VK_NULL_HANDLE;
	vkDescriptorPool = VK_NULL_HANDLE;
	vkRenderPass	 = VK_NULL_HANDLE;
	vkCmd			 = VK_NULL_HANDLE;
}

void VulkanDevice::SetFrameCommandBuffer(VkCommandBuffer cmd, uint32_t w, uint32_t h)
{
	vkCmd = cmd;
	if (cmd != VK_NULL_HANDLE) {
		frameWidth = w;
		frameHeight = h;
	}
}

// The bookkeeping D3D9 did for you. D3DPOOL said where a resource lived and
// the runtime picked the heap; Vulkan reports the heaps and the application
// chooses, every time.
uint32_t VulkanDevice::FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags want) const
{
	for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
		if ((typeBits & (1u << i)) == 0) continue;
		if ((memProps.memoryTypes[i].propertyFlags & want) == want) return i;
	}
	return UINT32_MAX;
}

// A command buffer for work that happens outside the frame -- uploads, layout
// transitions, mip generation.
//
// D3D9 needed nothing like it: UpdateTexture and a D3DPOOL_MANAGED resource
// put the runtime in charge of when the copy happened. Here the copy is a
// command and commands need a buffer, and the frame's buffer belongs to the
// core and only exists during the scene callback.
//
// THE SUBMIT IS SYNCHRONOUS. EndOneShot waits for the queue rather than
// pipelining, which is the same guarantee LockRect/UnlockRect gave: when the
// call returns, the bytes are on the GPU.
//
// IT IS NOT CALLED ONLY ON THE RENDER THREAD. This comment used to say it was
// -- "where every clbkLoad* in this client already runs" -- and that was the
// assumption that crashed the client. Orbiter's tile loaders are threads:
// TileLoader::Load_ThreadProc (Tilemgr2.cpp) and TileBuffer::LoadTile_ThreadProc
// (TileMgr.cpp), both deliberately left unconverted because they are pure
// Win32 threading with a shim behind it. A cloud tile's LoadDDSFile ->
// NatCreateTextureFromDDSInMemory -> UploadTexture lands here on that thread
// while the render thread is doing the same for an ImageProcessing constant
// buffer, from inside the scene callback:
//
//   Thread 1   EndOneShot:vkQueueSubmit   -> SIGSEGV inside libnvidia-glcore
//   Thread 885 EndOneShot:vkQueueWaitIdle -> stuck in the driver's rwlock
//
// A VkCommandPool may not be allocated from, recorded into or freed from by
// two threads at once, and neither may a VkQueue be submitted to. D3D9 needed
// none of this stated because D3D9Client.cpp asks for D3DCREATE_MULTITHREADED
// and the runtime holds the lock for you; Vulkan has no such flag, so the
// whole span -- allocate, record, submit, wait, free -- is one critical
// section under the core's device lock. See orbiter_LockDevice.
//
// The lock is taken here and released in EndOneShot. Every early return below
// releases it; a caller that gets VK_NULL_HANDLE must not call EndOneShot,
// and none does -- they all test the handle first.
VkCommandBuffer VulkanDevice::BeginOneShot()
{
	if (vkCommandPool == VK_NULL_HANDLE) return VK_NULL_HANDLE;

	orbiter_LockDevice();

	VkCommandBufferAllocateInfo ai = {};
	ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	ai.commandPool = vkCommandPool;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 1;

	VkCommandBuffer cmd = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(vkDevice, &ai, &cmd) != VK_SUCCESS) {
		orbiter_UnlockDevice();
		return VK_NULL_HANDLE;
	}

	VkCommandBufferBeginInfo bi = {};
	bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
		vkFreeCommandBuffers(vkDevice, vkCommandPool, 1, &cmd);
		orbiter_UnlockDevice();
		return VK_NULL_HANDLE;
	}
	return cmd;
}

void VulkanDevice::EndOneShot(VkCommandBuffer cmd)
{
	if (cmd == VK_NULL_HANDLE) return;

	vkEndCommandBuffer(cmd);

	VkSubmitInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;

	vkQueueSubmit(vkQueue, 1, &si, VK_NULL_HANDLE);
	vkQueueWaitIdle(vkQueue);
	vkFreeCommandBuffers(vkDevice, vkCommandPool, 1, &cmd);

	// Releases what BeginOneShot took. See the note there.
	orbiter_UnlockDevice();
}


// ===========================================================================
// OFFSCREEN RENDERING -- SetRenderTarget / SetDepthStencilSurface / BeginScene
//
// See VulkanFrame.h for why this is four operations rather than one call. The
// two caches below are the objects D3D9 did not have, and each is keyed on
// exactly what the specification says makes two of them interchangeable.
// ===========================================================================

// The load/store behaviour is LOAD + STORE on every attachment, and that is
// the faithful choice rather than the fast one. SetRenderTarget did NOT clear
// -- Orbiter's code calls Clear() explicitly wherever it wants a clean target,
// and D3D9Pad::Clear and Scene's own clears are converted to
// vkCmdClearAttachments, which runs inside the pass. Using CLEAR here would
// silently wipe targets that the reference draws into twice.
//
// initialLayout and finalLayout are both the attachment layout because
// BeginOffscreen transitions the images before beginning the pass and leaves
// them there; the next Blit, Read or bind transitions them onward, which is
// what every other operation in this file already does.
VkRenderPass VulkanDevice::GetOffscreenPass(const VkFormat *pColFmt, uint32_t nCol, VkFormat depthFmt)
{
	std::vector<uint32_t> key;
	key.reserve(nCol + 1);
	for (uint32_t i = 0; i < nCol; i++) key.push_back((uint32_t)pColFmt[i]);
	key.push_back((uint32_t)depthFmt);		// VK_FORMAT_UNDEFINED = no depth

	auto it = offPassCache.find(key);
	if (it != offPassCache.end()) return it->second;

	VkAttachmentDescription att[9] = {};
	VkAttachmentReference colRef[8] = {};
	VkAttachmentReference depRef = {};

	for (uint32_t i = 0; i < nCol; i++) {
		att[i].format = pColFmt[i];
		att[i].samples = VK_SAMPLE_COUNT_1_BIT;
		att[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		att[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		att[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		att[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		att[i].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		att[i].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		colRef[i].attachment = i;
		colRef[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	}

	uint32_t nAtt = nCol;

	if (depthFmt != VK_FORMAT_UNDEFINED) {
		att[nCol].format = depthFmt;
		att[nCol].samples = VK_SAMPLE_COUNT_1_BIT;
		att[nCol].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		att[nCol].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		att[nCol].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
		att[nCol].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
		att[nCol].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		att[nCol].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		depRef.attachment = nCol;
		depRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		nAtt++;
	}

	VkSubpassDescription sub = {};
	sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	sub.colorAttachmentCount = nCol;
	sub.pColorAttachments = nCol ? colRef : NULL;
	sub.pDepthStencilAttachment = (depthFmt != VK_FORMAT_UNDEFINED) ? &depRef : NULL;

	// The two dependencies say what D3D9 arranged invisibly: whatever was
	// reading these images before the pass must finish before it writes them,
	// and whatever samples them afterwards must wait for the writes.
	VkSubpassDependency dep[2] = {};
	dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
	dep[0].dstSubpass = 0;
	dep[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
	dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
						  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
	dep[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
	dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
						   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dep[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

	dep[1].srcSubpass = 0;
	dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
	dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
						  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
	dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
						   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
	dep[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

	VkRenderPassCreateInfo rpci = {};
	rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	rpci.attachmentCount = nAtt;
	rpci.pAttachments = att;
	rpci.subpassCount = 1;
	rpci.pSubpasses = &sub;
	rpci.dependencyCount = 2;
	rpci.pDependencies = dep;

	VkRenderPass pass = VK_NULL_HANDLE;
	if (vkCreateRenderPass(vkDevice, &rpci, NULL, &pass) != VK_SUCCESS) {
		LogErr("VulkanDevice::GetOffscreenPass: vkCreateRenderPass failed");
		return VK_NULL_HANDLE;
	}

	offPassCache[key] = pass;
	return pass;
}


// A framebuffer is tied to the exact image views, so those and the extent are
// the key. The render pass is in it too: two passes that are compatible could
// share a framebuffer, but nothing here needs that and a handle in the key is
// cheaper than deciding compatibility.
VkFramebuffer VulkanDevice::GetOffscreenFramebuffer(VkRenderPass pass, const VkImageView *pView,
													uint32_t nView, uint32_t w, uint32_t h)
{
	std::vector<uint64_t> key;
	key.reserve(nView + 3);
	key.push_back((uint64_t)pass);
	for (uint32_t i = 0; i < nView; i++) key.push_back((uint64_t)pView[i]);
	key.push_back(w);
	key.push_back(h);

	auto it = offFbCache.find(key);
	if (it != offFbCache.end()) return it->second;

	VkFramebufferCreateInfo fci = {};
	fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	fci.renderPass = pass;
	fci.attachmentCount = nView;
	fci.pAttachments = pView;
	fci.width = w;
	fci.height = h;
	fci.layers = 1;

	VkFramebuffer fb = VK_NULL_HANDLE;
	if (vkCreateFramebuffer(vkDevice, &fci, NULL, &fb) != VK_SUCCESS) {
		LogErr("VulkanDevice::GetOffscreenFramebuffer: vkCreateFramebuffer failed");
		return VK_NULL_HANDLE;
	}

	offFbCache[key] = fb;
	return fb;
}


bool VulkanDevice::BeginOffscreen(VulkanTexture *pColour, VulkanTexture *pDepth)
{
	return BeginOffscreen(&pColour, pColour ? 1 : 0, pDepth);
}


bool VulkanDevice::BeginOffscreen(VulkanTexture * const *ppColour, uint32_t nColour,
								  VulkanTexture *pDepth)
{
	if (vkActivePass != VK_NULL_HANDLE) {
		// A D3D9 SetRenderTarget inside a scene was legal; a Vulkan render
		// pass inside another is not, and the validation layer says so. This
		// is a caller error rather than a conversion gap, so it is reported by
		// name instead of being papered over with a nested pass.
		LogErr("VulkanDevice::BeginOffscreen: already inside an offscreen pass");
		return false;
	}

	if (nColour > ARRAYSIZE(offColour)) {
		LogErr("VulkanDevice::BeginOffscreen: %u colour attachments, %u is the limit",
			   nColour, (uint32_t)ARRAYSIZE(offColour));
		return false;
	}

	VkFormat colFmt[8] = {};
	VkImageView views[9] = {};
	uint32_t nView = 0;
	uint32_t w = 0, h = 0;

	for (uint32_t i = 0; i < nColour; i++) {
		VulkanTexture *p = ppColour[i];
		if (!p || p->Image() == VK_NULL_HANDLE || p->View() == VK_NULL_HANDLE) {
			// An attachment proxy has no image; see CreateAttachmentProxy.
			LogErr("VulkanDevice::BeginOffscreen: colour attachment %u is not a real image", i);
			return false;
		}
		colFmt[i] = p->Format();
		// AttachmentView(), not View(): a framebuffer attachment must name a
		// single mip level. See VUID-VkFramebufferCreateInfo-pAttachments-00883
		// and the note at VulkanTexture::AttachmentView.
		views[nView++] = p->AttachmentView();
		if (i == 0) { w = p->Width(); h = p->Height(); }
		else if (p->Width() != w || p->Height() != h) {
			// D3D9 required this too -- multiple render targets had to share
			// dimensions -- and simply failed the SetRenderTarget.
			LogErr("VulkanDevice::BeginOffscreen: colour attachments differ in size");
			return false;
		}
	}

	VkFormat depFmt = VK_FORMAT_UNDEFINED;
	if (pDepth && pDepth->Image() != VK_NULL_HANDLE) {
		depFmt = pDepth->Format();
		views[nView++] = pDepth->AttachmentView();
		if (nColour == 0) { w = pDepth->Width(); h = pDepth->Height(); }
	}

	if (w == 0 || h == 0) {
		LogErr("VulkanDevice::BeginOffscreen: no attachments");
		return false;
	}

	VkRenderPass pass = GetOffscreenPass(colFmt, nColour, depFmt);
	if (pass == VK_NULL_HANDLE) return false;

	VkFramebuffer fb = GetOffscreenFramebuffer(pass, views, nView, w, h);
	if (fb == VK_NULL_HANDLE) return false;

	// The frame's command buffer is already inside the core's render pass, so
	// the offscreen work is recorded into one of its own. See VulkanFrame.h.
	VkCommandBuffer cmd = BeginOneShot();
	if (cmd == VK_NULL_HANDLE) {
		LogErr("VulkanDevice::BeginOffscreen: no command buffer");
		return false;
	}

	// Into the attachment layouts the pass declares. This is the part D3D9 did
	// not have at all: SetRenderTarget on a texture that had just been sampled
	// needed no transition because a D3D9 resource has no layout.
	for (uint32_t i = 0; i < nColour; i++) {
		VulkanTexture *p = ppColour[i];
		Barrier(cmd, p->Image(), VK_IMAGE_ASPECT_COLOR_BIT,
				p->Layout(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		p->SetLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		offColour[i] = p;
	}
	offColourCount = nColour;

	offDepth = NULL;
	if (depFmt != VK_FORMAT_UNDEFINED) {
		Barrier(cmd, pDepth->Image(), AspectOf(depFmt),
				pDepth->Layout(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
		pDepth->SetLayout(VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
		offDepth = pDepth;
	}

	VkRenderPassBeginInfo rbi = {};
	rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rbi.renderPass = pass;
	rbi.framebuffer = fb;
	rbi.renderArea.offset.x = 0;
	rbi.renderArea.offset.y = 0;
	rbi.renderArea.extent.width = w;
	rbi.renderArea.extent.height = h;
	rbi.clearValueCount = 0;		// LOAD, not CLEAR -- see GetOffscreenPass
	rbi.pClearValues = NULL;

	vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);

	// The viewport and scissor. In D3D9 SetRenderTarget reset the viewport to
	// the whole target, which is what the client relied on; here they are
	// dynamic pipeline state and have to be issued.
	//
	// THE HEIGHT IS POSITIVE HERE, DELIBERATELY, and the Y direction is left
	// to the caller.
	//
	// D3D9 clip space has +1 at the TOP; Vulkan's has +1 at the BOTTOM, so a
	// converted D3D9 projection needs a negative-height viewport to land the
	// same way up. ShaderClass::Setup and VulkanEffectFile::BeginPassEx both
	// issue exactly that, and both re-issue it at bind time -- so they
	// override whatever this pass sets and are unaffected either way.
	//
	// Flipping it HERE looks like it would make the client consistent, and it
	// does not: the callers that draw into an offscreen surface without a
	// projection matrix of their own -- the sketchpad writing an MFD or a HUD
	// into a texture -- are already correct against this rectangle, and
	// flipping it turns them upside down. ImageProcessing is the one caller
	// whose matrix genuinely assumes D3D's axis, so it sets its own viewport
	// (see ImageProcessing::Execute); the flip belongs there, next to the
	// matrix that needs it, and not in a shared entry point.
	VkViewport vp = {};
	vp.x = 0.0f; vp.y = 0.0f;
	vp.width = float(w); vp.height = float(h);
	vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
	vkCmdSetViewport(cmd, 0, 1, &vp);

	VkRect2D sc = {};
	sc.offset.x = 0; sc.offset.y = 0;
	sc.extent.width = w; sc.extent.height = h;
	vkCmdSetScissor(cmd, 0, 1, &sc);

	vkSavedCmd = vkCmd;
	savedWidth = frameWidth;
	savedHeight = frameHeight;

	vkActivePass = pass;
	vkCmd = cmd;
	frameWidth = w;
	frameHeight = h;

	return true;
}


// Counterpart of EndScene() plus putting the previous render target back.
//
// THE SUBMIT IS SYNCHRONOUS, exactly as EndOneShot's is, and for the same
// reason: the caller's next line usually samples what was just drawn --
// Scene::RenderShadowMap hands its map straight to the mesh pass, and
// ImageProcessing::Execute's output is the next stage's input. D3D9 gave that
// guarantee implicitly by serialising on the render-target change.
void VulkanDevice::EndOffscreen()
{
	if (vkActivePass == VK_NULL_HANDLE) return;

	VkCommandBuffer cmd = vkCmd;

	vkCmdEndRenderPass(cmd);

	// Leave every attachment where the rest of this file expects to find an
	// image it did not just write: readable by a shader. SettledLayout is the
	// same rule Upload/Read/Blit use.
	//
	// ONLY IF THE IMAGE CAN LEGALLY HOLD THAT LAYOUT. SHADER_READ_ONLY_OPTIMAL
	// requires SAMPLED or INPUT_ATTACHMENT usage; an offscreen target created
	// as a pure render target (COLOR_ATTACHMENT plus the transfer bits, which
	// is what a target only ever blitted out of asks for) has neither, and the
	// transition is then illegal:
	//
	//     VUID-VkImageMemoryBarrier-oldLayout-01211
	//     newLayout (VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) is not
	//     compatible with VkImage usage flags TRANSFER_SRC|TRANSFER_DST|
	//     COLOR_ATTACHMENT
	//
	// Unguarded it was sixteen errors a frame and a segfault inside the
	// driver, on vVessel::RenderENVMap's PopRenderTargets. D3D9 had no such
	// rule -- a render target was readable by anything the moment the device
	// stopped drawing to it -- so no call site in the reference distinguishes
	// the two, and the distinction has to be made here.
	//
	// An image that cannot be sampled stays in COLOR_ATTACHMENT_OPTIMAL, which
	// is what every transfer path already transitions out of on demand.
	for (uint32_t i = 0; i < offColourCount; i++) {
		VulkanTexture *p = offColour[i];
		if (!p) continue;
		const VkImageUsageFlags u = p->Desc().Usage;
		const VkImageLayout settled =
			(u & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT))
				? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
				: VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		Barrier(cmd, p->Image(), VK_IMAGE_ASPECT_COLOR_BIT, p->Layout(), settled);
		p->SetLayout(settled);
		offColour[i] = NULL;
	}
	offColourCount = 0;

	// The depth buffer stays in its attachment layout: nothing samples it, and
	// the next pass that uses it declares the same initialLayout.
	offDepth = NULL;

	vkActivePass = VK_NULL_HANDLE;
	vkCmd = vkSavedCmd;
	frameWidth = savedWidth;
	frameHeight = savedHeight;
	vkSavedCmd = VK_NULL_HANDLE;

	EndOneShot(cmd);
}


// ===========================================================================
// IDirect3DDevice9::Clear with a NULL rectangle list. See VulkanFrame.h for
// why this is vkCmdClearAttachments and NOT ClearImage.
//
// The colour is a D3DCOLOR -- 0xAARRGGBB -- which is what every caller in the
// client passes and what D3D9's Clear took.
// ===========================================================================
void VulkanDevice::ClearFrame(bool bColour, bool bDepth, bool bStencil,
							  DWORD colour, float z, DWORD stencil)
{
	if (!IsRecording()) return;
	if (!bColour && !bDepth && !bStencil) return;

	VkClearAttachment att[9] = {};
	uint32_t n = 0;

	if (bColour) {
		const float q = 1.0f / 255.0f;
		VkClearColorValue c = {};
		c.float32[0] = float((colour >> 16) & 0xFF) * q;	// r
		c.float32[1] = float((colour >>  8) & 0xFF) * q;	// g
		c.float32[2] = float( colour        & 0xFF) * q;	// b
		c.float32[3] = float((colour >> 24) & 0xFF) * q;	// a

		// Every colour attachment of the pass, which is what D3DCLEAR_TARGET
		// meant: it cleared all bound render targets, not just the first.
		uint32_t nCol = GetRenderPassColourCount();
		for (uint32_t i = 0; i < nCol && n < ARRAYSIZE(att); i++) {
			att[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			att[n].colorAttachment = i;
			att[n].clearValue.color = c;
			n++;
		}
	}

	if ((bDepth || bStencil) && n < ARRAYSIZE(att)) {
		att[n].aspectMask = (bDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
							(bStencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
		att[n].clearValue.depthStencil.depth = z;
		att[n].clearValue.depthStencil.stencil = stencil;
		n++;
	}

	if (n == 0) return;

	VkClearRect rect = {};
	rect.rect.offset.x = 0;
	rect.rect.offset.y = 0;
	rect.rect.extent.width = frameWidth;
	rect.rect.extent.height = frameHeight;
	rect.baseArrayLayer = 0;
	rect.layerCount = 1;

	vkCmdClearAttachments(vkCmd, n, att, 1, &rect);
}


// ---------------------------------------------------------------------------
// Format queries.
//
// These three are IDirect3D9::CheckDeviceFormat with a different D3DUSAGE_ in
// each. D3D9 asked the OBJECT about an adapter/format/usage triple; Vulkan
// asks the physical device about a format and answers with a bitfield of
// everything it can do, so the usage argument becomes the bit that is tested.
// ---------------------------------------------------------------------------

bool VulkanDevice::SupportsVertexTexture(VkFormat fmt) const
{
	// Was CheckDeviceFormat(..., D3DUSAGE_QUERY_VERTEXTEXTURE, ...).
	//
	// Vulkan has no separate vertex-texture capability: SAMPLED_IMAGE_BIT
	// means a shader can sample the format, and which stage does the sampling
	// is not a property of the format. What D3D9's query really asked --
	// "can the vertex stage sample at all" -- is
	// limits.maxPerStageDescriptorSampledImages for the vertex stage, and
	// Vulkan's minimum guarantee for it is 16. So the honest answer is the
	// sampling bit plus that floor.
	if (fmt == VK_FORMAT_UNDEFINED) return false;
	VkFormatProperties p;
	vkGetPhysicalDeviceFormatProperties(vkPhysical, fmt, &p);
	return (p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0 &&
		   props.limits.maxPerStageDescriptorSampledImages > 0;
}

bool VulkanDevice::SupportsRenderTarget(VkFormat fmt) const
{
	if (fmt == VK_FORMAT_UNDEFINED) return false;
	VkFormatProperties p;
	vkGetPhysicalDeviceFormatProperties(vkPhysical, fmt, &p);
	return (p.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
}

bool VulkanDevice::SupportsDepthStencil(VkFormat fmt) const
{
	// Was CheckDepthStencilMatch(adapter, type, adapterFormat, renderTargetFormat,
	// depthStencilFormat) -- a question about a PAIR, because D3D9 allowed a
	// driver to refuse a particular colour/depth combination. Vulkan has no
	// such pairing rule: a depth format usable as a depth-stencil attachment
	// may be used with any colour attachment, so the pair collapses to the
	// single format the caller cares about.
	if (fmt == VK_FORMAT_UNDEFINED) return false;
	VkFormatProperties p;
	vkGetPhysicalDeviceFormatProperties(vkPhysical, fmt, &p);
	return (p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
}

// The same candidate list, in the same order, as UIHost.cpp's
// selectDepthFormat -- which names this function as what it is matching. Both
// run against the same VkPhysicalDevice, so both reach the same answer, which
// is what makes the client's pipelines compatible with the core's render pass.
VkFormat VulkanDevice::SelectDepthFormat() const
{
	static const VkFormat candidates[] = {
		VK_FORMAT_D24_UNORM_S8_UINT,
		VK_FORMAT_D32_SFLOAT_S8_UINT,
		VK_FORMAT_D16_UNORM_S8_UINT
	};
	for (size_t i = 0; i < ARRAYSIZE(candidates); i++)
		if (SupportsDepthStencil(candidates[i])) return candidates[i];
	return VK_FORMAT_UNDEFINED;
}

VkFormat VulkanDevice::SelectColourFormat() const
{
	// UIHost.cpp:1406's list, in its order.
	static const VkFormat candidates[] = {
		VK_FORMAT_B8G8R8A8_UNORM,
		VK_FORMAT_R8G8B8A8_UNORM,
		VK_FORMAT_B8G8R8_UNORM,
		VK_FORMAT_R8G8B8_UNORM
	};
	for (size_t i = 0; i < ARRAYSIZE(candidates); i++)
		if (SupportsRenderTarget(candidates[i])) return candidates[i];
	return VK_FORMAT_B8G8R8A8_UNORM;
}


// ---------------------------------------------------------------------------
// The layout an image rests in between operations.
//
// D3D9 had no such notion, so there is nothing to convert; it exists because
// every barrier below needs a "put it back how you found it" target and
// UNDEFINED is not a legal destination layout.
// ---------------------------------------------------------------------------
static VkImageLayout SettledLayout(const VulkanTexture *pTex)
{
	if (!pTex) return VK_IMAGE_LAYOUT_GENERAL;

	// A linear, host-visible image is one the CPU writes and a copy reads.
	// GENERAL is the only layout valid for both.
	if (pTex->IsHostVisible()) return VK_IMAGE_LAYOUT_GENERAL;

	const VkImageUsageFlags u = pTex->Desc().Usage;
	if (u & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
		return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	if (u & VK_IMAGE_USAGE_SAMPLED_BIT)
		return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	if (u & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
		return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
}

// The extent of one mip level. D3D9 answered this with GetLevelDesc; a VkImage
// answers nothing, so the halving is done here from what was recorded.
static void MipExtent(const VulkanImageDesc &d, uint32_t mip,
					  uint32_t *w, uint32_t *h, uint32_t *z)
{
	*w = d.Width  >> mip; if (*w == 0) *w = 1;
	*h = d.Height >> mip; if (*h == 0) *h = 1;
	*z = d.Depth  >> mip; if (*z == 0) *z = 1;
}


// ---------------------------------------------------------------------------
// Name: CreateTexture3D
// Desc: The one image-creation path. CreateTexture forwards to it with
//       depth 1, because a 2D image and a 3D image differ in Vulkan only by
//       imageType and extent.depth -- the split into CreateTexture and
//       D3DXCreateVolumeTexture existed because IDirect3DTexture9 and
//       IDirect3DVolumeTexture9 were different interfaces.
//
//       WHERE THE MEMORY GOES IS DECIDED FROM THE USAGE, and that is what
//       NatCreateSurface means by "CreateTexture honours the memory choice
//       through the usage it was given". D3D9 said it with D3DPOOL: SYSTEMMEM
//       for something the CPU touches, DEFAULT for something the GPU renders
//       with. Here an image asked for with nothing but the two transfer bits
//       is one the CPU is going to write, so it is linear and host-visible;
//       an image asked for with SAMPLED, COLOR_ATTACHMENT or
//       DEPTH_STENCIL_ATTACHMENT is one the GPU renders with, so it is
//       tiled and device-local and the CPU reaches it through a staging copy.
// ---------------------------------------------------------------------------
VulkanTexture *VulkanDevice::CreateTexture3D(uint32_t w, uint32_t h, uint32_t depth,
											 uint32_t mips, VkFormat fmt,
											 VkImageUsageFlags usage)
{
	if (vkDevice == VK_NULL_HANDLE || w == 0 || h == 0 || depth == 0) return NULL;
	if (fmt == VK_FORMAT_UNDEFINED) return NULL;
	if (mips == 0) mips = 1;

	// EVERY IMAGE IS A TRANSFER SOURCE AND DESTINATION, because in D3D9 every
	// surface was.
	//
	// D3D9 had no per-resource usage flags for copying: StretchRect,
	// UpdateSurface, GetRenderTargetData and D3DXFilterTexture would take any
	// surface as their source, so no call site in the reference ever declared
	// "this one can be copied from". Vulkan requires the intent up front, and
	// the callers converted from those D3D9 calls do not have it to give.
	//
	// Leaving it to the callers produced 48 validation errors per frame, all
	// the same shape:
	//
	//     VUID-vkCmdCopyImage-aspect-06662     ... was created with
	//     VUID-vkCmdBlitImage-srcImage-00219   TRANSFER_DST|SAMPLED but
	//     VUID-VkImageMemoryBarrier-oldLayout-01212   requires TRANSFER_SRC
	//
	// -- a texture loaded from a DDS with SAMPLED|TRANSFER_DST, then used as
	// the source of GenerateMipmaps' blit down the chain, of BlitTexture, or
	// of a readback. Adding the two bits here restores the D3D9 property the
	// call sites were written against, in the one place that knows it applies
	// to all of them. Both are cheap: they constrain image layout on some
	// hardware but allocate nothing extra.
	usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

	const VkImageUsageFlags gpuBits =
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
		VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
		VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
		VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;

	bool hostVisible = ((usage & gpuBits) == 0);

	// A linear image has restrictions an optimally-tiled one does not:
	// 2D only, one mip level, one layer, one sample. Vulkan requires it and
	// D3D9's D3DPOOL_SYSTEMMEM had the same practical shape.
	if (hostVisible && (depth > 1 || mips > 1)) hostVisible = false;

	// ...and the format must support linear tiling for what it is being asked
	// to do. Compressed formats generally do not, which is why a DDS always
	// lands in an optimally-tiled image and is staged into.
	if (hostVisible) {
		VkFormatProperties fp;
		vkGetPhysicalDeviceFormatProperties(vkPhysical, fmt, &fp);
		const VkFormatFeatureFlags need =
			VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
		if ((fp.linearTilingFeatures & need) != need) hostVisible = false;
	}

	VkImageCreateInfo ici = {};
	ici.sType		  = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.imageType	  = (depth > 1) ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
	ici.format		  = fmt;
	ici.extent.width  = w;
	ici.extent.height = h;
	ici.extent.depth  = depth;
	ici.mipLevels	  = mips;
	ici.arrayLayers	  = 1;
	ici.samples		  = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling		  = hostVisible ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
	ici.usage		  = usage;
	ici.sharingMode	  = VK_SHARING_MODE_EXCLUSIVE;
	ici.initialLayout = hostVisible ? VK_IMAGE_LAYOUT_PREINITIALIZED
									: VK_IMAGE_LAYOUT_UNDEFINED;

	VkImage image = VK_NULL_HANDLE;
	if (vkCreateImage(vkDevice, &ici, NULL, &image) != VK_SUCCESS) {
		LogErr("VulkanDevice::CreateTexture: vkCreateImage failed [%u x %u x %u, fmt %d]",
			   w, h, depth, int(fmt));
		return NULL;
	}

	VkMemoryRequirements mr = {};
	vkGetImageMemoryRequirements(vkDevice, image, &mr);

	// HOST_COHERENT is asked for alongside HOST_VISIBLE so that Unmap() does
	// not have to flush. Every desktop implementation offers the pair.
	const VkMemoryPropertyFlags want = hostVisible
		? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
		: VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	uint32_t type = FindMemoryType(mr.memoryTypeBits, want);
	if (type == UINT32_MAX && !hostVisible) {
		// A device with no DEVICE_LOCAL heap the image can live in is an
		// integrated part where every heap is host-visible. Take any heap.
		type = FindMemoryType(mr.memoryTypeBits, 0);
	}
	if (type == UINT32_MAX) {
		LogErr("VulkanDevice::CreateTexture: no memory type for the image");
		vkDestroyImage(vkDevice, image, NULL);
		return NULL;
	}

	VkMemoryAllocateInfo mai = {};
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = type;

	VkDeviceMemory memory = VK_NULL_HANDLE;
	if (vkAllocateMemory(vkDevice, &mai, NULL, &memory) != VK_SUCCESS) {
		LogErr("VulkanDevice::CreateTexture: vkAllocateMemory failed [%u bytes]",
			   (unsigned)mr.size);
		vkDestroyImage(vkDevice, image, NULL);
		return NULL;
	}
	if (vkBindImageMemory(vkDevice, image, memory, 0) != VK_SUCCESS) {
		vkFreeMemory(vkDevice, memory, NULL);
		vkDestroyImage(vkDevice, image, NULL);
		return NULL;
	}

	VulkanTexture *pTex = new VulkanTexture();
	pTex->vkOwner	 = vkDevice;
	pTex->vkImage	 = image;
	pTex->vkMemory	 = memory;
	pTex->vkLayout	 = ici.initialLayout;
	pTex->bOwnsImage = true;

	if (getenv("ORBITER_VK_TRACE_TEX"))
		LogErr("TEXTRACE 2d/3d image=%p %ux%ux%u mips=%u fmt=%d usage=0x%X hv=%d",
			   (void *)image, w, h, depth, mips, int(fmt), (unsigned)usage, int(hostVisible));

	pTex->desc.Width	   = w;
	pTex->desc.Height	   = h;
	pTex->desc.Depth	   = depth;
	pTex->desc.Format	   = fmt;
	pTex->desc.Usage	   = usage;
	pTex->desc.Mips		   = mips;
	pTex->desc.Layers	   = 1;		// see CreateTextureCube for the other value
	pTex->desc.HostVisible = hostVisible;
	pTex->desc.Samples	   = VK_SAMPLE_COUNT_1_BIT;

	// Was D3DLOCKED_RECT::Pitch, which LockRect filled in. A linear image can
	// be asked for its real row stride; a tiled one has none to give, so the
	// tightly-packed stride is recorded, which is what the staging buffers
	// below use.
	if (hostVisible) {
		VkImageSubresource sub = {};
		sub.aspectMask = AspectOf(fmt);
		VkSubresourceLayout sl = {};
		vkGetImageSubresourceLayout(vkDevice, image, &sub, &sl);
		pTex->pitch = (size_t)sl.rowPitch;
	}
	else {
		pTex->pitch = SurfNative::StaticFormatSizeInBytes(fmt, w);
	}

	// The view. Only images something samples or renders into need one; a
	// transfer-only image is never bound to anything.
	if (usage & gpuBits) {
		VkImageViewCreateInfo ivci = {};
		ivci.sType	  = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		ivci.image	  = image;
		ivci.viewType = (depth > 1) ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
		ivci.format	  = fmt;
		ivci.subresourceRange.aspectMask	 = AspectOf(fmt);
		ivci.subresourceRange.baseMipLevel	 = 0;
		ivci.subresourceRange.levelCount	 = mips;
		ivci.subresourceRange.baseArrayLayer = 0;
		ivci.subresourceRange.layerCount	 = 1;

		if (vkCreateImageView(vkDevice, &ivci, NULL, &pTex->vkView) != VK_SUCCESS) {
			LogErr("VulkanDevice::CreateTexture: vkCreateImageView failed");
			pTex->vkView = VK_NULL_HANDLE;
			delete pTex;			// the destructor frees image and memory
			return NULL;
		}
	}

	// A host-visible image is written by the CPU straight away, and
	// PREINITIALIZED is only valid until something else touches it. Moving it
	// to GENERAL now is what makes both the CPU write and the later copy legal.
	if (hostVisible) {
		VkCommandBuffer cmd = BeginOneShot();
		if (cmd != VK_NULL_HANDLE) {
			Barrier(cmd, image, AspectOf(fmt),
					VK_IMAGE_LAYOUT_PREINITIALIZED, VK_IMAGE_LAYOUT_GENERAL);
			EndOneShot(cmd);
			pTex->vkLayout = VK_IMAGE_LAYOUT_GENERAL;
		}
	}
	else {
		// AND A DEVICE-LOCAL IMAGE IS SETTLED THE MOMENT IT EXISTS, because
		// THAT IS THE D3D9 PROPERTY THIS WHOLE FILE IS CONVERTING.
		//
		// A D3D9 resource is usable as soon as CreateTexture returns: there is
		// no layout, so sampling a texture nothing has written yet reads
		// undefined CONTENT and is otherwise legal. A VkImage begins in
		// VK_IMAGE_LAYOUT_UNDEFINED and only leaves it when some operation
		// issues a barrier -- Upload, Read, Blit, Clear, GenerateMipmaps or
		// BeginOffscreen. Every one of those transitions from pTex->Layout(),
		// so an image whose first use is one of them was always correct.
		//
		// An image whose FIRST use is being SAMPLED was not, and there is no
		// opportunity to fix it later: a descriptor is written and bound from
		// inside the core's render pass, where a barrier cannot be issued.
		// Measured, not assumed -- two images reached a draw this way:
		//
		//   2048x2048 R32_SFLOAT COLOR_ATTACHMENT   the shadow map, sampled
		//                                           on a frame with no shadow pass
		//   512x1    R32_SFLOAT                     a lookup table
		//
		//   VUID-vkCmdDraw-None-09600 ... expects VkImage ... to be in layout
		//   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL -- instead, current
		//   layout is VK_IMAGE_LAYOUT_UNDEFINED
		//
		// Settling it here restores the D3D9 guarantee in the one place that
		// applies to every image, and keeps the invariant this file states on
		// UploadTexture true from the moment of creation rather than from the
		// first operation. The content is still undefined until something
		// writes it, exactly as it was on D3D9.
		const VkImageLayout settled = SettledLayout(pTex);
		if (settled != VK_IMAGE_LAYOUT_UNDEFINED) {
			VkCommandBuffer cmd = BeginOneShot();
			if (cmd != VK_NULL_HANDLE) {
				Barrier(cmd, image, AspectOf(fmt), VK_IMAGE_LAYOUT_UNDEFINED, settled);
				EndOneShot(cmd);
				pTex->vkLayout = settled;
			}
		}
	}

	return pTex;
}

VulkanTexture *VulkanDevice::CreateTexture(uint32_t w, uint32_t h, uint32_t mips,
										   VkFormat fmt, VkImageUsageFlags usage)
{
	return CreateTexture3D(w, h, 1, mips, fmt, usage);
}


// ---------------------------------------------------------------------------
// Name: CreateBuffer
// Desc: Counterpart of CreateVertexBuffer and CreateIndexBuffer at once.
//
//       D3DUSAGE_WRITEONLY and D3DUSAGE_DYNAMIC become the one question
//       'hostVisible' asks. D3DPOOL_MANAGED has no counterpart and needs
//       none: it meant "keep a system-memory copy and re-upload it after a
//       device loss", and a Vulkan device that is lost takes every object
//       with it, so there is nothing a shadow copy could be restored into.
// ---------------------------------------------------------------------------
VulkanBuffer *VulkanDevice::CreateBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage,
										 bool hostVisible)
{
	if (vkDevice == VK_NULL_HANDLE || bytes == 0) return NULL;

	// A device-local buffer still has to be filled somehow, and the only way
	// in is a transfer. D3D9 never needed this because every pool was
	// writable one way or another.
	if (!hostVisible) usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;

	VkBufferCreateInfo bci = {};
	bci.sType		= VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size		= bytes;
	bci.usage		= usage;
	bci.sharingMode	= VK_SHARING_MODE_EXCLUSIVE;

	VkBuffer buffer = VK_NULL_HANDLE;
	if (vkCreateBuffer(vkDevice, &bci, NULL, &buffer) != VK_SUCCESS) {
		LogErr("VulkanDevice::CreateBuffer: vkCreateBuffer failed [%u bytes]", (unsigned)bytes);
		return NULL;
	}

	VkMemoryRequirements mr = {};
	vkGetBufferMemoryRequirements(vkDevice, buffer, &mr);

	const VkMemoryPropertyFlags want = hostVisible
		? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
		: VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

	uint32_t type = FindMemoryType(mr.memoryTypeBits, want);
	if (type == UINT32_MAX) type = FindMemoryType(mr.memoryTypeBits, 0);
	if (type == UINT32_MAX) {
		LogErr("VulkanDevice::CreateBuffer: no memory type for the buffer");
		vkDestroyBuffer(vkDevice, buffer, NULL);
		return NULL;
	}

	VkMemoryAllocateInfo mai = {};
	mai.sType			= VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.allocationSize	= mr.size;
	mai.memoryTypeIndex	= type;

	VkDeviceMemory memory = VK_NULL_HANDLE;
	if (vkAllocateMemory(vkDevice, &mai, NULL, &memory) != VK_SUCCESS) {
		LogErr("VulkanDevice::CreateBuffer: vkAllocateMemory failed [%u bytes]", (unsigned)mr.size);
		vkDestroyBuffer(vkDevice, buffer, NULL);
		return NULL;
	}
	if (vkBindBufferMemory(vkDevice, buffer, memory, 0) != VK_SUCCESS) {
		vkFreeMemory(vkDevice, memory, NULL);
		vkDestroyBuffer(vkDevice, buffer, NULL);
		return NULL;
	}

	VulkanBuffer *pBuf = new VulkanBuffer();
	pBuf->vkOwner	  = vkDevice;
	pBuf->vkBuffer	  = buffer;
	pBuf->vkMemory	  = memory;
	pBuf->bytes		  = bytes;
	pBuf->usage		  = usage;
	pBuf->hostVisible = (memProps.memoryTypes[type].propertyFlags &
						 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
	return pBuf;
}


// ---------------------------------------------------------------------------
// Name: CreateMipView
// Desc: Counterpart of IDirect3DTexture9::GetSurfaceLevel(level).
//
//       D3D9 returned a new IDirect3DSurface9 that shared the texture's
//       storage and held a reference to it. Vulkan's counterpart of "a
//       different way of looking at the same image" is a VkImageView with
//       baseMipLevel set -- so the view is new and owned, the image is the
//       parent's and is not.
// ---------------------------------------------------------------------------
VulkanTexture *VulkanDevice::CreateMipView(VulkanTexture *pTex, uint32_t level)
{
	if (!pTex || pTex->Image() == VK_NULL_HANDLE) return NULL;
	if (level >= pTex->Mips()) return NULL;

	uint32_t w, h, z;
	MipExtent(pTex->Desc(), level, &w, &h, &z);

	VkImageViewCreateInfo ivci = {};
	ivci.sType	  = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	ivci.image	  = pTex->Image();
	ivci.viewType = (pTex->Desc().Depth > 1) ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
	ivci.format	  = pTex->Format();
	ivci.subresourceRange.aspectMask	 = AspectOf(pTex->Format());
	ivci.subresourceRange.baseMipLevel	 = level;
	ivci.subresourceRange.levelCount	 = 1;
	ivci.subresourceRange.baseArrayLayer = 0;
	ivci.subresourceRange.layerCount	 = 1;

	VkImageView view = VK_NULL_HANDLE;
	if (vkCreateImageView(vkDevice, &ivci, NULL, &view) != VK_SUCCESS) {
		LogErr("VulkanDevice::CreateMipView: vkCreateImageView failed [level %u]", level);
		return NULL;
	}

	VulkanTexture *pView = new VulkanTexture();
	pView->vkOwner	  = vkDevice;
	pView->vkImage	  = pTex->Image();
	pView->vkMemory	  = VK_NULL_HANDLE;	// not this object's to map or free
	pView->vkView	  = view;
	pView->vkLayout	  = pTex->Layout();
	pView->bOwnsImage = false;

	pView->desc			 = pTex->Desc();
	pView->desc.Width	 = w;
	pView->desc.Height	 = h;
	pView->desc.Depth	 = z;
	pView->desc.Mips	 = 1;
	pView->pitch		 = SurfNative::StaticFormatSizeInBytes(pTex->Format(), w);
	return pView;
}


// ---------------------------------------------------------------------------
// Name: CreateAttachmentProxy
// Desc: See the note by the declaration. The counterpart of GetRenderTarget(0)
//       and GetDepthStencilSurface() for attachments the client does not own
//       and is never handed.
// ---------------------------------------------------------------------------
VulkanTexture *VulkanDevice::CreateAttachmentProxy(uint32_t w, uint32_t h, VkFormat fmt,
												   VkImageUsageFlags usage)
{
	VulkanTexture *pTex = new VulkanTexture();
	pTex->vkOwner	 = vkDevice;
	pTex->vkImage	 = VK_NULL_HANDLE;
	pTex->vkMemory	 = VK_NULL_HANDLE;
	pTex->vkView	 = VK_NULL_HANDLE;
	pTex->vkLayout	 = VK_IMAGE_LAYOUT_UNDEFINED;
	pTex->bOwnsImage = false;

	pTex->desc.Width	   = w;
	pTex->desc.Height	   = h;
	pTex->desc.Depth	   = 1;
	pTex->desc.Format	   = fmt;
	pTex->desc.Usage	   = usage;
	pTex->desc.Mips		   = 1;
	pTex->desc.Layers	   = 1;
	pTex->desc.HostVisible = false;
	pTex->desc.Samples	   = VK_SAMPLE_COUNT_1_BIT;
	pTex->pitch			   = SurfNative::StaticFormatSizeInBytes(fmt, w);
	return pTex;
}


// ---------------------------------------------------------------------------
// Name: CreateTextureCube
// Desc: Counterpart of D3DXCreateCubeTexture, added for vVessel's environment
//       and irradiance maps.
//
//       D3D9 had a third texture INTERFACE for this. Vulkan has none, and the
//       thing it has instead is spelled in three places at once, all of which
//       have to agree or the image is not a cube:
//
//         VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT on the image,
//         arrayLayers 6 (imageType stays 2D -- a cube is NOT a 3D image),
//         VK_IMAGE_VIEW_TYPE_CUBE on the view that samples it.
//
//       The FACE ORDER carries over unchanged: D3DCUBEMAP_FACE_POSITIVE_X..
//       NEGATIVE_Z are 0..5 and Vulkan's array layers for a cube are +X, -X,
//       +Y, -Y, +Z, -Z in that same order. So EnvMapDirection's switch needs
//       no renumbering.
//
//       No host-visible path, deliberately: D3DXCreateCubeTexture's callers
//       here both pass D3DUSAGE_RENDERTARGET, and a linear-tiled image may
//       not have six layers in any case.
// ---------------------------------------------------------------------------
VulkanTexture *VulkanDevice::CreateTextureCube(uint32_t size, uint32_t mips,
											   VkFormat fmt, VkImageUsageFlags usage)
{
	if (vkDevice == VK_NULL_HANDLE || size == 0) return NULL;
	if (fmt == VK_FORMAT_UNDEFINED) return NULL;
	if (mips == 0) mips = 1;

	VkImageCreateInfo ici = {};
	ici.sType		  = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.flags		  = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
	ici.imageType	  = VK_IMAGE_TYPE_2D;
	ici.format		  = fmt;
	ici.extent.width  = size;
	ici.extent.height = size;
	ici.extent.depth  = 1;
	ici.mipLevels	  = mips;
	ici.arrayLayers	  = 6;
	ici.samples		  = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling		  = VK_IMAGE_TILING_OPTIMAL;
	ici.usage		  = usage;
	ici.sharingMode	  = VK_SHARING_MODE_EXCLUSIVE;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VkImage image = VK_NULL_HANDLE;
	if (vkCreateImage(vkDevice, &ici, NULL, &image) != VK_SUCCESS) {
		LogErr("VulkanDevice::CreateTextureCube: vkCreateImage failed [%u, fmt %d]",
			   size, int(fmt));
		return NULL;
	}

	VkMemoryRequirements mr = {};
	vkGetImageMemoryRequirements(vkDevice, image, &mr);

	uint32_t type = FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (type == UINT32_MAX) type = FindMemoryType(mr.memoryTypeBits, 0);
	if (type == UINT32_MAX) {
		LogErr("VulkanDevice::CreateTextureCube: no memory type for the image");
		vkDestroyImage(vkDevice, image, NULL);
		return NULL;
	}

	VkMemoryAllocateInfo mai = {};
	mai.sType			= VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.allocationSize	= mr.size;
	mai.memoryTypeIndex	= type;

	VkDeviceMemory memory = VK_NULL_HANDLE;
	if (vkAllocateMemory(vkDevice, &mai, NULL, &memory) != VK_SUCCESS) {
		LogErr("VulkanDevice::CreateTextureCube: vkAllocateMemory failed [%u bytes]",
			   (unsigned)mr.size);
		vkDestroyImage(vkDevice, image, NULL);
		return NULL;
	}
	if (vkBindImageMemory(vkDevice, image, memory, 0) != VK_SUCCESS) {
		vkFreeMemory(vkDevice, memory, NULL);
		vkDestroyImage(vkDevice, image, NULL);
		return NULL;
	}

	VulkanTexture *pTex = new VulkanTexture();
	pTex->vkOwner	 = vkDevice;
	pTex->vkImage	 = image;
	pTex->vkMemory	 = memory;
	pTex->vkLayout	 = VK_IMAGE_LAYOUT_UNDEFINED;
	pTex->bOwnsImage = true;

	if (getenv("ORBITER_VK_TRACE_TEX"))
		LogErr("TEXTRACE cube image=%p size=%u fmt=%d usage=0x%X",
			   (void *)image, size, int(fmt), (unsigned)usage);

	pTex->desc.Width	   = size;
	pTex->desc.Height	   = size;
	pTex->desc.Depth	   = 1;
	pTex->desc.Format	   = fmt;
	pTex->desc.Usage	   = usage;
	pTex->desc.Mips		   = mips;
	pTex->desc.Layers	   = 6;
	pTex->desc.HostVisible = false;
	pTex->desc.Samples	   = VK_SAMPLE_COUNT_1_BIT;
	pTex->pitch			   = SurfNative::StaticFormatSizeInBytes(fmt, size);

	// The whole-cube view -- what a shader samples with a direction. The
	// per-face views a framebuffer needs come from CreateFaceView.
	VkImageViewCreateInfo ivci = {};
	ivci.sType	  = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	ivci.image	  = image;
	ivci.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
	ivci.format	  = fmt;
	ivci.subresourceRange.aspectMask	 = AspectOf(fmt);
	ivci.subresourceRange.baseMipLevel	 = 0;
	ivci.subresourceRange.levelCount	 = mips;
	ivci.subresourceRange.baseArrayLayer = 0;
	ivci.subresourceRange.layerCount	 = 6;

	if (vkCreateImageView(vkDevice, &ivci, NULL, &pTex->vkView) != VK_SUCCESS) {
		LogErr("VulkanDevice::CreateTextureCube: vkCreateImageView failed");
		pTex->vkView = VK_NULL_HANDLE;
		delete pTex;			// the destructor frees image and memory
		return NULL;
	}

	// Settled the moment it exists, all six faces, for the reason set out in
	// CreateTexture3D: a D3D9 resource is usable as soon as it is created, and
	// an image whose first use is being sampled never gets a barrier anywhere
	// else. The layer default on Barrier() covers every face.
	const VkImageLayout settled = SettledLayout(pTex);
	if (settled != VK_IMAGE_LAYOUT_UNDEFINED) {
		VkCommandBuffer cmd = BeginOneShot();
		if (cmd != VK_NULL_HANDLE) {
			Barrier(cmd, image, AspectOf(fmt), VK_IMAGE_LAYOUT_UNDEFINED, settled);
			EndOneShot(cmd);
			pTex->vkLayout = settled;
		}
	}

	return pTex;
}


// ---------------------------------------------------------------------------
// Name: CreateFaceView
// Desc: Counterpart of IDirect3DCubeTexture9::GetCubeMapSurface(face, level).
//
//       Same shape as CreateMipView above and for the same reason: D3D9
//       handed back a new surface interface sharing the texture's storage,
//       and Vulkan's "another way of looking at the same image" is a
//       VkImageView. This one sets baseArrayLayer rather than baseMipLevel,
//       and its viewType is 2D rather than CUBE, because a framebuffer
//       attachment is a single 2D image and not a cube.
// ---------------------------------------------------------------------------
VulkanTexture *VulkanDevice::CreateFaceView(VulkanTexture *pTex, uint32_t face, uint32_t level)
{
	if (!pTex || pTex->Image() == VK_NULL_HANDLE) return NULL;
	if (face >= pTex->Desc().Layers) return NULL;
	if (level >= pTex->Mips()) return NULL;

	uint32_t w, h, z;
	MipExtent(pTex->Desc(), level, &w, &h, &z);

	VkImageViewCreateInfo ivci = {};
	ivci.sType	  = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	ivci.image	  = pTex->Image();
	ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	ivci.format	  = pTex->Format();
	ivci.subresourceRange.aspectMask	 = AspectOf(pTex->Format());
	ivci.subresourceRange.baseMipLevel	 = level;
	ivci.subresourceRange.levelCount	 = 1;
	ivci.subresourceRange.baseArrayLayer = face;
	ivci.subresourceRange.layerCount	 = 1;

	VkImageView view = VK_NULL_HANDLE;
	if (vkCreateImageView(vkDevice, &ivci, NULL, &view) != VK_SUCCESS) {
		LogErr("VulkanDevice::CreateFaceView: vkCreateImageView failed [face %u, level %u]",
			   face, level);
		return NULL;
	}

	VulkanTexture *pView = new VulkanTexture();
	pView->vkOwner	  = vkDevice;
	pView->vkImage	  = pTex->Image();
	pView->vkMemory	  = VK_NULL_HANDLE;	// not this object's to map or free
	pView->vkView	  = view;
	pView->vkLayout	  = pTex->Layout();
	pView->bOwnsImage = false;

	pView->desc			 = pTex->Desc();
	pView->desc.Width	 = w;
	pView->desc.Height	 = h;
	pView->desc.Depth	 = 1;
	pView->desc.Mips	 = 1;
	pView->desc.Layers	 = 1;	// this view is one face, not a cube
	pView->pitch		 = SurfNative::StaticFormatSizeInBytes(pTex->Format(), w);
	return pView;
}


// ---------------------------------------------------------------------------
// Destruction. See the note by the declarations: the freeing itself is in the
// destructors, and these exist so the call site does not `delete` a type it
// only knows by forward declaration.
// ---------------------------------------------------------------------------
void VulkanDevice::DestroyTexture(VulkanTexture *pTex)
{
	delete pTex;
}

void VulkanDevice::DestroyBuffer(VulkanBuffer *pBuf)
{
	delete pBuf;
}


// ---------------------------------------------------------------------------
// Name: UploadTexture
// Desc: Put CPU bytes into one mip level (and, for a 3D image, one slice).
//
//       This is LockRect / memcpy / UnlockRect, and it is longer because
//       D3D9's version was doing all of this out of sight. A D3DPOOL_MANAGED
//       texture had a system-memory copy that the runtime uploaded on your
//       behalf at the next draw; there is no such copy and no such runtime
//       here, so the bytes go into a host-visible staging buffer and are
//       copied across with vkCmdCopyBufferToImage.
//
//       THE LAYOUT INVARIANT, stated once because everything below depends on
//       it: A VulkanTexture is always in ONE layout across ALL of its mip
//       levels AND ALL of its array layers -- SetLayout records one layout for
//       the whole texture, so a barrier that moves less than all of it leaves
//       the record lying. See the layer-default note on Barrier().
//       Every operation here transitions the whole image, works, and
//       transitions the whole image back to SettledLayout. That costs an
//       extra barrier or two on a multi-level upload and buys the thing D3D9
//       gave for free -- that the caller never has to know what state a
//       resource is in.
// ---------------------------------------------------------------------------
bool VulkanDevice::UploadTexture(VulkanTexture *pTex, uint32_t mip, uint32_t slice,
								 const void *pData, size_t bytes)
{
	if (!pTex || !pData || bytes == 0) return false;
	if (pTex->Image() == VK_NULL_HANDLE) return false;	// an attachment proxy
	if (mip >= pTex->Mips()) return false;
	// A layered image's slice is a face; see the note at the copy region.
	if (pTex->Desc().Layers > 1 && slice >= pTex->Desc().Layers) return false;

	uint32_t mw, mh, mz;
	MipExtent(pTex->Desc(), mip, &mw, &mh, &mz);

	// The direct path, for an image whose memory the CPU can already reach.
	// This is what LockRect did on a D3DPOOL_SYSTEMMEM surface, and the row
	// loop is there for the same reason D3DLOCKED_RECT had a Pitch: the
	// driver's row stride is its own business and need not be width*bpp.
	if (pTex->IsHostVisible() && mip == 0 && slice == 0) {
		char *dst = (char *)pTex->Map();
		if (dst) {
			const size_t dstPitch = pTex->RowPitch();
			const size_t srcPitch = (mh > 0) ? (bytes / mh) : bytes;
			const size_t run = (srcPitch < dstPitch) ? srcPitch : dstPitch;
			const char *src = (const char *)pData;
			for (uint32_t y = 0; y < mh; y++)
				memcpy(dst + (size_t)y * dstPitch, src + (size_t)y * srcPitch, run);
			pTex->Unmap();
			return true;
		}
		// fall through and stage, exactly as the D3D9 code fell through to
		// UpdateSurface whenever a lock was refused
	}

	VulkanBuffer *pStage = CreateBuffer((VkDeviceSize)bytes,
										VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
	if (!pStage) return false;

	void *p = pStage->Map();
	if (!p) { DestroyBuffer(pStage); return false; }
	memcpy(p, pData, bytes);
	pStage->Unmap();

	VkCommandBuffer cmd = BeginOneShot();
	if (cmd == VK_NULL_HANDLE) { DestroyBuffer(pStage); return false; }

	const VkImageAspectFlags aspect = AspectOf(pTex->Format());
	const VkImageLayout settled = SettledLayout(pTex);

	Barrier(cmd, pTex->Image(), aspect, pTex->Layout(),
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	VkBufferImageCopy region = {};
	region.bufferOffset		 = 0;
	region.bufferRowLength	 = 0;		// tightly packed; see the pitch note
	region.bufferImageHeight = 0;
	// WHAT `slice` SELECTS DEPENDS ON WHICH KIND OF IMAGE THIS IS, and the two
	// are not interchangeable.
	//
	// A 3D image stacks its slices along z, so the slice is an imageOffset.z
	// -- which is what this did, and what CreateVolumeTexture's
	// UploadTexture(pVol, m, i, ...) means.
	//
	// A CUBE stacks its six faces as ARRAY LAYERS: arrayLayers is 6 and
	// extent.depth is 1 (see CreateTextureCube). Addressing a face with
	// imageOffset.z asks for z = 1..5 in an image one deep -- out of bounds --
	// so five of the six faces were never written at all. The face is
	// baseArrayLayer.
	//
	// D3D9 kept the two apart in the type system: GetCubeMapSurface(face)
	// against LockBox(slice) on IDirect3DVolumeTexture9. One Vulkan entry
	// point serves both, so it has to ask which it is holding.
	const bool bLayered = (pTex->Desc().Layers > 1);

	region.imageSubresource.aspectMask	  = aspect;
	region.imageSubresource.mipLevel	  = mip;
	region.imageSubresource.baseArrayLayer = bLayered ? slice : 0;
	region.imageSubresource.layerCount	  = 1;
	region.imageOffset = { 0, 0, bLayered ? 0 : (int32_t)slice };
	region.imageExtent = { mw, mh, 1 };

	vkCmdCopyBufferToImage(cmd, pStage->Buffer(), pTex->Image(),
						   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	Barrier(cmd, pTex->Image(), aspect,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, settled);

	EndOneShot(cmd);
	pTex->SetLayout(settled);

	DestroyBuffer(pStage);
	return true;
}


// ---------------------------------------------------------------------------
// Name: ReadTexture
// Desc: Read one mip level back to CPU memory.
//
//       Counterpart of GetRenderTargetData into a D3DPOOL_SYSTEMMEM surface
//       followed by LockRect. The restriction is the same on both platforms --
//       you cannot map what the GPU renders with -- and so is the answer:
//       copy it somewhere you can.
// ---------------------------------------------------------------------------
bool VulkanDevice::ReadTexture(VulkanTexture *pTex, uint32_t mip, void *pData, size_t bytes)
{
	if (!pTex || !pData || bytes == 0) return false;
	if (pTex->Image() == VK_NULL_HANDLE) return false;
	if (mip >= pTex->Mips()) return false;

	uint32_t mw, mh, mz;
	MipExtent(pTex->Desc(), mip, &mw, &mh, &mz);

	if (pTex->IsHostVisible() && mip == 0) {
		const char *src = (const char *)pTex->Map();
		if (src) {
			const size_t srcPitch = pTex->RowPitch();
			const size_t dstPitch = (mh > 0) ? (bytes / mh) : bytes;
			const size_t run = (srcPitch < dstPitch) ? srcPitch : dstPitch;
			char *dst = (char *)pData;
			for (uint32_t y = 0; y < mh; y++)
				memcpy(dst + (size_t)y * dstPitch, src + (size_t)y * srcPitch, run);
			pTex->Unmap();
			return true;
		}
	}

	VulkanBuffer *pStage = CreateBuffer((VkDeviceSize)bytes,
										VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
	if (!pStage) return false;

	VkCommandBuffer cmd = BeginOneShot();
	if (cmd == VK_NULL_HANDLE) { DestroyBuffer(pStage); return false; }

	const VkImageAspectFlags aspect = AspectOf(pTex->Format());
	const VkImageLayout settled = SettledLayout(pTex);

	Barrier(cmd, pTex->Image(), aspect, pTex->Layout(),
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

	VkBufferImageCopy region = {};
	region.bufferOffset		 = 0;
	region.bufferRowLength	 = 0;
	region.bufferImageHeight = 0;
	region.imageSubresource.aspectMask	   = aspect;
	region.imageSubresource.mipLevel	   = mip;
	region.imageSubresource.baseArrayLayer = 0;
	region.imageSubresource.layerCount	   = 1;
	region.imageOffset = { 0, 0, 0 };
	region.imageExtent = { mw, mh, mz };

	vkCmdCopyImageToBuffer(cmd, pTex->Image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
						   pStage->Buffer(), 1, &region);

	Barrier(cmd, pTex->Image(), aspect,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, settled);

	EndOneShot(cmd);
	pTex->SetLayout(settled);

	const void *p = pStage->Map();
	if (!p) { DestroyBuffer(pStage); return false; }
	memcpy(pData, p, bytes);
	pStage->Unmap();

	DestroyBuffer(pStage);
	return true;
}


// Whether a format can be the source or the destination of vkCmdBlitImage.
//
// D3D9 had no such question for StretchRect -- it worked, or the driver did
// it in software. Vulkan states it as two optional format features and
// declines the call outright if they are missing, which is why every blit
// below is guarded and falls back to an exact copy where it can.
static bool CanBlit(VkPhysicalDevice phys, VkFormat fmt, VkImageTiling tiling, bool asSource)
{
	VkFormatProperties p;
	vkGetPhysicalDeviceFormatProperties(phys, fmt, &p);
	const VkFormatFeatureFlags f = (tiling == VK_IMAGE_TILING_LINEAR)
		? p.linearTilingFeatures : p.optimalTilingFeatures;
	return (f & (asSource ? VK_FORMAT_FEATURE_BLIT_SRC_BIT
						  : VK_FORMAT_FEATURE_BLIT_DST_BIT)) != 0;
}


// ---------------------------------------------------------------------------
// Name: BlitTexture
// Desc: Counterpart of StretchRect, and of the copying half of
//       D3DXLoadSurfaceFromSurface.
//
//       vkCmdBlitImage rescales and converts format in one call, which is
//       what StretchRect did. What it cannot do is produce block-compressed
//       output -- no Vulkan call can -- so a blit INTO a BC image is refused
//       here rather than silently producing nothing. NatCompressSurface says
//       the same thing at the level above.
//
//       Every level of the shorter mip chain is blitted, not just level 0.
//       SurfNative::Decompress and SurfNative::DeClone both create the
//       destination with the source's full level count and expect all of them,
//       which is what D3DXLoadSurfaceFromSurface per level gave them.
// ---------------------------------------------------------------------------
bool VulkanDevice::BlitTexture(VulkanTexture *pDst, VulkanTexture *pSrc)
{
	if (!pDst || !pSrc) return false;
	if (pDst->Image() == VK_NULL_HANDLE || pSrc->Image() == VK_NULL_HANDLE) return false;
	if (pDst == pSrc) return false;		// see SurfNative::GetTempSurface

	const VkImageTiling srcTiling = pSrc->IsHostVisible() ? VK_IMAGE_TILING_LINEAR
														  : VK_IMAGE_TILING_OPTIMAL;
	const VkImageTiling dstTiling = pDst->IsHostVisible() ? VK_IMAGE_TILING_LINEAR
														  : VK_IMAGE_TILING_OPTIMAL;

	const bool blittable = CanBlit(vkPhysical, pSrc->Format(), srcTiling, true) &&
						   CanBlit(vkPhysical, pDst->Format(), dstTiling, false);

	const bool copyable = (pSrc->Format() == pDst->Format()) &&
						  (pSrc->Width() == pDst->Width()) &&
						  (pSrc->Height() == pDst->Height());

	if (!blittable && !copyable) {
		LogErr("VulkanDevice::BlitTexture: no path from format %d to format %d "
			   "[%u x %u -> %u x %u]. A block-compressed destination has none.",
			   int(pSrc->Format()), int(pDst->Format()),
			   pSrc->Width(), pSrc->Height(), pDst->Width(), pDst->Height());
		return false;
	}

	VkCommandBuffer cmd = BeginOneShot();
	if (cmd == VK_NULL_HANDLE) return false;

	const VkImageAspectFlags sAspect = AspectOf(pSrc->Format());
	const VkImageAspectFlags dAspect = AspectOf(pDst->Format());
	const VkImageLayout sSettled = SettledLayout(pSrc);
	const VkImageLayout dSettled = SettledLayout(pDst);

	Barrier(cmd, pSrc->Image(), sAspect, pSrc->Layout(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	Barrier(cmd, pDst->Image(), dAspect, pDst->Layout(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	const uint32_t levels = (pSrc->Mips() < pDst->Mips()) ? pSrc->Mips() : pDst->Mips();

	for (uint32_t m = 0; m < levels; m++) {

		uint32_t sw, sh, sz, dw, dh, dz;
		MipExtent(pSrc->Desc(), m, &sw, &sh, &sz);
		MipExtent(pDst->Desc(), m, &dw, &dh, &dz);

		if (blittable) {
			VkImageBlit b = {};
			b.srcSubresource.aspectMask		= sAspect;
			b.srcSubresource.mipLevel		= m;
			b.srcSubresource.baseArrayLayer	= 0;
			b.srcSubresource.layerCount		= 1;
			b.srcOffsets[0] = { 0, 0, 0 };
			b.srcOffsets[1] = { (int32_t)sw, (int32_t)sh, (int32_t)sz };
			b.dstSubresource.aspectMask		= dAspect;
			b.dstSubresource.mipLevel		= m;
			b.dstSubresource.baseArrayLayer	= 0;
			b.dstSubresource.layerCount		= 1;
			b.dstOffsets[0] = { 0, 0, 0 };
			b.dstOffsets[1] = { (int32_t)dw, (int32_t)dh, (int32_t)dz };

			vkCmdBlitImage(cmd,
						   pSrc->Image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
						   pDst->Image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
						   1, &b, VK_FILTER_LINEAR);
		}
		else {
			VkImageCopy c = {};
			c.srcSubresource.aspectMask		= sAspect;
			c.srcSubresource.mipLevel		= m;
			c.srcSubresource.baseArrayLayer	= 0;
			c.srcSubresource.layerCount		= 1;
			c.dstSubresource.aspectMask		= dAspect;
			c.dstSubresource.mipLevel		= m;
			c.dstSubresource.baseArrayLayer	= 0;
			c.dstSubresource.layerCount		= 1;
			c.extent = { sw, sh, sz };

			vkCmdCopyImage(cmd,
						   pSrc->Image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
						   pDst->Image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
						   1, &c);
		}
	}

	Barrier(cmd, pSrc->Image(), sAspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sSettled);
	Barrier(cmd, pDst->Image(), dAspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dSettled);

	EndOneShot(cmd);
	pSrc->SetLayout(sSettled);
	pDst->SetLayout(dSettled);
	return true;
}


// ---------------------------------------------------------------------------
// Name: BlitTexture (rectangle form)
// Desc: The exact counterpart of StretchRect(pSrc, sr, pDst, tr, filter),
//       added for gcCore::StretchRectInScene.
//
//       This is not a new capability and not an emulation: vkCmdBlitImage's
//       srcOffsets and dstOffsets ARE two rectangles, and StretchRect's two
//       rectangles are what goes in them. The whole-image overload above is
//       the case where both are the full extent.
//
//       Mip 0 only, because that is what StretchRect addressed -- a D3D9
//       surface IS one level. And no vkCmdCopyImage fallback, because the two
//       rectangles are allowed to differ in size and a copy cannot rescale;
//       a block-compressed destination is refused here exactly as StretchRect
//       refused one.
// ---------------------------------------------------------------------------
bool VulkanDevice::BlitTexture(VulkanTexture *pDst, const RECT *tr,
							   VulkanTexture *pSrc, const RECT *sr, bool bLinear)
{
	if (!pDst || !pSrc) return false;
	if (pDst->Image() == VK_NULL_HANDLE || pSrc->Image() == VK_NULL_HANDLE) return false;
	if (pDst == pSrc) return false;		// see SurfNative::GetTempSurface

	const VkImageTiling srcTiling = pSrc->IsHostVisible() ? VK_IMAGE_TILING_LINEAR
														  : VK_IMAGE_TILING_OPTIMAL;
	const VkImageTiling dstTiling = pDst->IsHostVisible() ? VK_IMAGE_TILING_LINEAR
														  : VK_IMAGE_TILING_OPTIMAL;

	if (!CanBlit(vkPhysical, pSrc->Format(), srcTiling, true) ||
		!CanBlit(vkPhysical, pDst->Format(), dstTiling, false)) {
		LogErr("VulkanDevice::BlitTexture(rect): no blit path from format %d to "
			   "format %d. A block-compressed destination has none.",
			   int(pSrc->Format()), int(pDst->Format()));
		return false;
	}

	VkCommandBuffer cmd = BeginOneShot();
	if (cmd == VK_NULL_HANDLE) return false;

	const VkImageAspectFlags sAspect = AspectOf(pSrc->Format());
	const VkImageAspectFlags dAspect = AspectOf(pDst->Format());
	const VkImageLayout sSettled = SettledLayout(pSrc);
	const VkImageLayout dSettled = SettledLayout(pDst);

	Barrier(cmd, pSrc->Image(), sAspect, pSrc->Layout(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	Barrier(cmd, pDst->Image(), dAspect, pDst->Layout(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	VkImageBlit b = {};
	b.srcSubresource.aspectMask		= sAspect;
	b.srcSubresource.mipLevel		= 0;
	b.srcSubresource.baseArrayLayer	= 0;
	b.srcSubresource.layerCount		= 1;
	b.dstSubresource.aspectMask		= dAspect;
	b.dstSubresource.mipLevel		= 0;
	b.dstSubresource.baseArrayLayer	= 0;
	b.dstSubresource.layerCount		= 1;

	// A NULL rectangle means the whole image. That is StretchRect's rule, not
	// an invention: "If NULL, the entire surface is used."
	if (sr) {
		b.srcOffsets[0] = { (int32_t)sr->left,  (int32_t)sr->top,    0 };
		b.srcOffsets[1] = { (int32_t)sr->right, (int32_t)sr->bottom, 1 };
	}
	else {
		b.srcOffsets[0] = { 0, 0, 0 };
		b.srcOffsets[1] = { (int32_t)pSrc->Width(), (int32_t)pSrc->Height(), 1 };
	}

	if (tr) {
		b.dstOffsets[0] = { (int32_t)tr->left,  (int32_t)tr->top,    0 };
		b.dstOffsets[1] = { (int32_t)tr->right, (int32_t)tr->bottom, 1 };
	}
	else {
		b.dstOffsets[0] = { 0, 0, 0 };
		b.dstOffsets[1] = { (int32_t)pDst->Width(), (int32_t)pDst->Height(), 1 };
	}

	// D3DTEXF_LINEAR / D3DTEXF_POINT were the only two StretchRect accepted.
	vkCmdBlitImage(cmd,
				   pSrc->Image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				   pDst->Image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				   1, &b, bLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);

	Barrier(cmd, pSrc->Image(), sAspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sSettled);
	Barrier(cmd, pDst->Image(), dAspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dSettled);

	EndOneShot(cmd);
	pSrc->SetLayout(sSettled);
	pDst->SetLayout(dSettled);
	return true;
}


// ---------------------------------------------------------------------------
// Name: GenerateMipmaps
// Desc: Build the mip chain by blitting down it.
//
//       D3DUSAGE_AUTOGENMIPMAP has NO COUNTERPART. D3D9 let the driver
//       regenerate the chain whenever level 0 changed; Vulkan generates
//       nothing at all, so the chain is built here, one halving blit per
//       level, with the barrier in between that makes the level just written
//       readable as the next level's source.
//
//       The loop's shape is the standard one and the reason for its shape is
//       the layout invariant: each level goes TRANSFER_DST -> TRANSFER_SRC as
//       soon as it has been written, because the next iteration reads it.
// ---------------------------------------------------------------------------
bool VulkanDevice::GenerateMipmaps(VulkanTexture *pTex)
{
	if (!pTex || pTex->Image() == VK_NULL_HANDLE) return false;
	if (pTex->Mips() <= 1) return true;		// nothing to build, not a failure

	const VkImageTiling tiling = pTex->IsHostVisible() ? VK_IMAGE_TILING_LINEAR
													   : VK_IMAGE_TILING_OPTIMAL;
	if (!CanBlit(vkPhysical, pTex->Format(), tiling, true) ||
		!CanBlit(vkPhysical, pTex->Format(), tiling, false)) {
		// A compressed format lands here, and it is the right answer: the
		// levels of a BC texture come from the file, not from the GPU.
		LogWrn("VulkanDevice::GenerateMipmaps: format %d cannot be blitted; "
			   "levels left as loaded", int(pTex->Format()));
		return false;
	}

	VkCommandBuffer cmd = BeginOneShot();
	if (cmd == VK_NULL_HANDLE) return false;

	const VkImageAspectFlags aspect = AspectOf(pTex->Format());
	const VkImageLayout settled = SettledLayout(pTex);

	// Level 0 becomes the first source; every other level becomes a
	// destination waiting to be written.
	Barrier(cmd, pTex->Image(), aspect, pTex->Layout(),
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	Barrier(cmd, pTex->Image(), aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 1);

	for (uint32_t m = 1; m < pTex->Mips(); m++) {

		uint32_t sw, sh, sz, dw, dh, dz;
		MipExtent(pTex->Desc(), m - 1, &sw, &sh, &sz);
		MipExtent(pTex->Desc(), m,     &dw, &dh, &dz);

		VkImageBlit b = {};
		b.srcSubresource.aspectMask		= aspect;
		b.srcSubresource.mipLevel		= m - 1;
		b.srcSubresource.baseArrayLayer	= 0;
		b.srcSubresource.layerCount		= 1;
		b.srcOffsets[0] = { 0, 0, 0 };
		b.srcOffsets[1] = { (int32_t)sw, (int32_t)sh, (int32_t)sz };
		b.dstSubresource.aspectMask		= aspect;
		b.dstSubresource.mipLevel		= m;
		b.dstSubresource.baseArrayLayer	= 0;
		b.dstSubresource.layerCount		= 1;
		b.dstOffsets[0] = { 0, 0, 0 };
		b.dstOffsets[1] = { (int32_t)dw, (int32_t)dh, (int32_t)dz };

		vkCmdBlitImage(cmd,
					   pTex->Image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					   pTex->Image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					   1, &b, VK_FILTER_LINEAR);

		// What was just written is the next iteration's source.
		Barrier(cmd, pTex->Image(), aspect,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m, 1);
	}

	Barrier(cmd, pTex->Image(), aspect,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, settled);

	EndOneShot(cmd);
	pTex->SetLayout(settled);
	return true;
}


// ---------------------------------------------------------------------------
// Name: ClearImage
// Desc: Counterpart of IDirect3DDevice9::ColorFill, which SurfNative::Fill
//       calls with a rectangle or with NULL for the whole surface.
//
//       ColorFill took a rectangle and Vulkan has no single call that does.
//       vkCmdClearColorImage takes VkImageSubresourceRanges -- whole levels --
//       and vkCmdClearAttachments, which does take rectangles, is valid only
//       inside a render pass, which Fill is not called from. So the whole-
//       image case is the clear and the rectangle case is a copy from a
//       staging buffer filled with the colour: exact, and slow in proportion
//       to the rectangle rather than to the surface.
// ---------------------------------------------------------------------------
bool VulkanDevice::ClearImage(VulkanTexture *pTex, const RECT *r, DWORD colour)
{
	if (!pTex || pTex->Image() == VK_NULL_HANDLE) return false;
	if (AspectOf(pTex->Format()) != VK_IMAGE_ASPECT_COLOR_BIT) return false;

	const uint32_t iw = pTex->Width();
	const uint32_t ih = pTex->Height();

	uint32_t x = 0, y = 0, w = iw, h = ih;
	if (r) {
		if (r->right <= r->left || r->bottom <= r->top) return true;	// empty
		x = (uint32_t)((r->left   < 0) ? 0 : r->left);
		y = (uint32_t)((r->top    < 0) ? 0 : r->top);
		w = (uint32_t)((r->right  < 0) ? 0 : r->right)  - x;
		h = (uint32_t)((r->bottom < 0) ? 0 : r->bottom) - y;
		if (x >= iw || y >= ih) return true;
		if (x + w > iw) w = iw - x;
		if (y + h > ih) h = ih - y;
	}
	if (w == 0 || h == 0) return true;

	const bool whole = (x == 0 && y == 0 && w == iw && h == ih);

	const VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	const VkImageLayout settled = SettledLayout(pTex);

	// 0xAARRGGBB, the layout every colour in this client is written in.
	const float a = float((colour >> 24) & 0xFF) / 255.0f;
	const float cr = float((colour >> 16) & 0xFF) / 255.0f;
	const float cg = float((colour >> 8) & 0xFF) / 255.0f;
	const float cb = float(colour & 0xFF) / 255.0f;

	if (whole) {
		VkCommandBuffer cmd = BeginOneShot();
		if (cmd == VK_NULL_HANDLE) return false;

		Barrier(cmd, pTex->Image(), aspect, pTex->Layout(),
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

		VkClearColorValue cv = {};
		cv.float32[0] = cr; cv.float32[1] = cg; cv.float32[2] = cb; cv.float32[3] = a;

		VkImageSubresourceRange range = {};
		range.aspectMask	 = aspect;
		range.baseMipLevel	 = 0;
		range.levelCount	 = VK_REMAINING_MIP_LEVELS;
		range.baseArrayLayer = 0;
		range.layerCount	 = 1;

		vkCmdClearColorImage(cmd, pTex->Image(),
							 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &range);

		Barrier(cmd, pTex->Image(), aspect,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, settled);

		EndOneShot(cmd);
		pTex->SetLayout(settled);
		return true;
	}

	// The rectangle case. The staging buffer holds the colour in the image's
	// own byte order, which is the one thing vkCmdClearColorImage would have
	// handled for us and a raw copy will not.
	DWORD packed;
	switch (pTex->Format()) {
	case VK_FORMAT_B8G8R8A8_UNORM:
	case VK_FORMAT_B8G8R8A8_SRGB:
		// byte 0 = B, 1 = G, 2 = R, 3 = A -- which is 0xAARRGGBB as stored.
		packed = colour;
		break;
	case VK_FORMAT_R8G8B8A8_UNORM:
	case VK_FORMAT_R8G8B8A8_SRGB:
		packed = (colour & 0xFF00FF00u) |
				 ((colour & 0x00FF0000u) >> 16) |
				 ((colour & 0x000000FFu) << 16);
		break;
	default:
		LogErr("VulkanDevice::ClearImage: no rectangle fill for format %d",
			   int(pTex->Format()));
		return false;
	}

	const size_t bytes = (size_t)w * h * 4;
	VulkanBuffer *pStage = CreateBuffer((VkDeviceSize)bytes,
										VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
	if (!pStage) return false;

	DWORD *p = (DWORD *)pStage->Map();
	if (!p) { DestroyBuffer(pStage); return false; }
	for (size_t i = 0; i < (size_t)w * h; i++) p[i] = packed;
	pStage->Unmap();

	VkCommandBuffer cmd = BeginOneShot();
	if (cmd == VK_NULL_HANDLE) { DestroyBuffer(pStage); return false; }

	Barrier(cmd, pTex->Image(), aspect, pTex->Layout(),
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	VkBufferImageCopy region = {};
	region.bufferOffset		 = 0;
	region.bufferRowLength	 = 0;
	region.bufferImageHeight = 0;
	region.imageSubresource.aspectMask	   = aspect;
	region.imageSubresource.mipLevel	   = 0;
	region.imageSubresource.baseArrayLayer = 0;
	region.imageSubresource.layerCount	   = 1;
	region.imageOffset = { (int32_t)x, (int32_t)y, 0 };
	region.imageExtent = { w, h, 1 };

	vkCmdCopyBufferToImage(cmd, pStage->Buffer(), pTex->Image(),
						   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

	Barrier(cmd, pTex->Image(), aspect,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, settled);

	EndOneShot(cmd);
	pTex->SetLayout(settled);

	DestroyBuffer(pStage);
	return true;
}


// ###########################################################################
//
//   CVulkanFramework -- the conversion of CD3DFramework9
//
// ###########################################################################

//-----------------------------------------------------------------------------
// Name: CVulkanFramework()
// Desc: The constructor. Clears static variables.
//
//       The Windows constructor tested `g_pD3DObject == NULL` and put up a
//       message box naming the DirectX version. There is no counterpart
//       object to test here -- the instance belongs to the core and exists
//       before any client is loaded -- so the equivalent test is whether the
//       core will hand over a context at all, which is what Initialize() asks
//       and reports. The constructor only clears.
//-----------------------------------------------------------------------------
CVulkanFramework::CVulkanFramework()
{
	Clear();
}

//-----------------------------------------------------------------------------
// Name: ~CVulkanFramework()
// Desc: The destructor. Deletes all objects
//-----------------------------------------------------------------------------
CVulkanFramework::~CVulkanFramework()
{
	LogAlw("Deleting Framework");
}

void CVulkanFramework::Clear()
{
	hWnd			  = NULL;
	bIsFullscreen	  = false;
	bVertexTexture    = false;
	bAAEnabled		  = false;
	bNoVSync		  = false;
	Alpha			  = false;
	dwRenderWidth	  = 0;
	dwRenderHeight	  = 0;
	dwFSMode		  = 0;
	pDevice			  = NULL;
	dwZBufferBitDepth = 0;
	dwStencilBitDepth = 0;
	Adapter			  = 0;
	Mode			  = 0;
	MultiSample		  = 0;
	dwDisplayMode	  = 0;
	pRenderTarget	  = NULL;
	pDepthStencil	  = NULL;
	pBackBuffer		  = NULL;

	// SWVert, Pure, DDM, nvPerfHud, pLargeFont and pSmallFont were cleared
	// here. See VulkanFrame.h for why each is gone.

	memset((void *)&rcScreenRect, 0, sizeof(RECT));

	// memset of d3dPP and caps stood here. D3DPRESENT_PARAMETERS has no
	// counterpart -- every field in it describes the swapchain, which is the
	// core's -- and D3DCAPS9 became three separate Vulkan queries that live
	// on VulkanDevice and are filled by Adopt().
}

//-----------------------------------------------------------------------------
// Name: DestroyObjects()
// Desc: Objects created in Initialize() section are destroyed in here
//-----------------------------------------------------------------------------
HRESULT CVulkanFramework::DestroyObjects()
{
	_TRACE;
	LogAlw("========== Destroying framework objects ==========");

	// The eleven SAFE_RELEASE calls on the vertex declarations stood here.
	// The declarations are constants now and there is nothing to release --
	// see the file header.

	// SAFE_RELEASE(pLargeFont) and SAFE_RELEASE(pSmallFont) stood here too.

	// THE BACK BUFFER SURFACE IS NOT DELETED HERE, and the reference is why.
	//
	// CD3DFramework9::DestroyObjects releases pRenderTarget and pDepthStencil
	// -- the two D3D9 interfaces -- and says NOTHING about pBackBuffer. That
	// is not an omission: pBackBuffer is a SurfNative, every SurfNative
	// registers itself in SurfaceCatalog, and clbkDestroyRenderWindow deletes
	// whatever is left in that catalog immediately before calling this
	// function. It is one of the surfaces the reference's own
	//
	//     LogErr("UnDeleted Surfaces(s) Detected %u... Releasing...")
	//
	// line reports and frees. Deleting it again here is a double delete, and
	// it is the one that ended every session:
	//
	//     Thread 1 "Orbiter" received signal SIGSEGV
	//     #0  0x0000000000000491 in ?? ()
	//     #1  CVulkanFramework::DestroyObjects  VulkanFrame.cpp:2555
	//     #2  VulkanClient::clbkDestroyRenderWindow  VulkanClient.cpp:1200
	//
	// -- a call through the vtable of an object freed a few lines earlier.
	//
	// So the pointer is only cleared. The two attachment proxies below ARE
	// this class's to free: SurfNative's destructor skips them because the
	// surface carries OAPISURFACE_BACKBUFFER, exactly as the D3D9 one skipped
	// releasing interfaces it did not own.
	pBackBuffer = NULL;

	// The proxies GetRenderTarget(0) and GetDepthStencilSurface() stood in
	// for. They hold no image, so this frees two small objects and nothing
	// of the core's.
	if (pDevice) {
		if (pRenderTarget) { pDevice->DestroyTexture(pRenderTarget); pRenderTarget = NULL; }
		if (pDepthStencil) { pDevice->DestroyTexture(pDepthStencil); pDepthStencil = NULL; }
	}

	// Was:
	//     HR(pDevice->EvictManagedResources());
	//     Sleep(200);
	//     if (pDevice->Reset(&d3dPP)==S_OK) ...
	//
	// EvictManagedResources emptied the D3DPOOL_MANAGED shadow copies, and
	// there is no managed pool here -- the client owns every allocation
	// outright. The Reset was how a D3D9 device was made to let go of
	// resources still referenced by the runtime, and the Sleep was a guess at
	// how long that took. Both become the two calls that actually give the
	// guarantee: wait until nothing is executing, then reset the frames'
	// command buffers so none of them still names a descriptor set or an
	// image about to be freed. UIHost.cpp's own note on
	// orbiter_ResetFrameCommands measured what happens without the second --
	// 465 validation errors at teardown.
	// Under the device lock, for the same reason clbkCloseSession's copy of
	// this pair is: vkDeviceWaitIdle is host access to every queue, and the
	// queue belongs to the core and is shared. See orbiter_LockDevice.
	if (pDevice && pDevice->GetDevice() != VK_NULL_HANDLE) {
		orbiter_LockDevice();
		vkDeviceWaitIdle(pDevice->GetDevice());
		orbiter_ResetFrameCommands();
		orbiter_UnlockDevice();
	}

	// Was SAFE_RELEASE(pDevice), which on Windows destroyed the device
	// because the client had created it. Here Destroy() releases only what
	// the client made -- see VulkanDevice::Destroy.
	if (pDevice) {
		pDevice->Destroy();
		delete pDevice;
		pDevice = NULL;
	}

	return S_OK;
}


// Was GetAvailableTextureMem(). See the note by the declaration for why the
// number means something weaker than the one it replaces.
VkDeviceSize VulkanDevice::GetLocalMemorySize() const
{
	VkDeviceSize total = 0;
	for (uint32_t i = 0; i < memProps.memoryHeapCount; i++)
		if (memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
			total += memProps.memoryHeaps[i].size;
	return total;
}


// Whether a format may be used as a VERTEX ATTRIBUTE.
//
// Was caps.DeclTypes tested against D3DDTCAPS_DEC3N / _FLOAT16_2 / _FLOAT16_4:
// one bitfield naming the handful of packed vertex types a driver might not
// support. Vulkan asks per format, through bufferFeatures rather than the
// image features every other query here uses.
static bool SupportsVertexFormat(VkPhysicalDevice phys, VkFormat fmt)
{
	VkFormatProperties p;
	vkGetPhysicalDeviceFormatProperties(phys, fmt, &p);
	return (p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
}


//-----------------------------------------------------------------------------
// Name: Initialize()
// Desc: Creates the internal objects for the framework
//-----------------------------------------------------------------------------
HRESULT CVulkanFramework::Initialize(HWND _hWnd, GraphicsClient::VIDEODATA *vData)
{
	_TRACE;

	Clear();

	// DDM and nvPerfHud were read from Config here. Both settings are gone --
	// see VulkanConfig.h, which lists what each of them set and why the flag
	// has nowhere to go.

	bool bFail = false;

	if (_hWnd == NULL || vData == NULL) {
		LogErr("ERROR: Invalid input parameter in CVulkanFramework::Initialize()");
		return E_INVALIDARG;
	}

	// Setup state for windowed/fullscreen mode
	//
	hWnd          = _hWnd;
	bAAEnabled    = (Config->SceneAntialias != 0);
	bIsFullscreen = vData->fullscreen;
	bNoVSync      = vData->novsync;
	dwFSMode	  = vData->style;
	Adapter		  = vData->deviceidx;
	Mode		  = vData->modeidx;

	LogAlw("[VideoConfiguration] Adapter=%u, ModeIndex=%u", Adapter, Mode);

	// ------------------------------------------------------------------
	// WHERE CreateDevice USED TO BE, AND IT IS HERE INSTEAD OF FURTHER DOWN.
	//
	// The Windows file asked g_pD3DObject for the caps first and created the
	// device last, because IDirect3D9 exists before any device does. The
	// counterpart of g_pD3DObject is the core's context, and the physical
	// device only becomes reachable once that has been fetched -- so the
	// adopt stands exactly where g_pD3DObject stood, and everything below it
	// is in the reference's own order.
	// ------------------------------------------------------------------
	OrbiterVulkanContext ctx;
	memset(&ctx, 0, sizeof(ctx));

	if (!orbiter_GetVulkanContext(&ctx)) {
		LogErr("ERROR: [Vulkan context unavailable]");
		LogErr(vkmessage);
		MessageBoxA(NULL, vkmessage, "VulkanClient Initialization Failed", MB_OK);
		return E_FAIL;
	}

	pDevice = new VulkanDevice();
	if (!pDevice->Adopt(ctx)) {
		LogErr("ERROR: [Vulkan device adoption failed]");
		delete pDevice;
		pDevice = NULL;
		return E_FAIL;
	}

	const VkPhysicalDeviceProperties *caps = pDevice->GetProperties();

	// Was GetAdapterIdentifier(Adapter, 0, &info) and info.Description.
	LogOapi("3D-Adapter.............. : %s", caps->deviceName);
	LogAlw("dwFSMode................ : %u", dwFSMode);

	// The GPU the user chose on the Video tab. On Windows this was the
	// Adapter argument to every g_pD3DObject call below; here the device is
	// already made, so the index can only be recorded for the next run --
	// which is what orbiter_SetPreferredGpuIndex does and says.
	orbiter_SetPreferredGpuIndex((int)Adapter);

	// VIDEODATA::outputidx, which the reference never read because a D3D9
	// adapter IS an output and `Adapter` already carried it. Vulkan
	// enumerates GPUs, not adapter/output pairs, so the monitor has to be
	// named separately. It must be set before any mode query: the mode list
	// is that monitor's. See pickPrimaryMonitor in UIHost.cpp.
	orbiter_SetOutputIndex(vData->outputidx);

	// Get DisplayMode Resolution ---------------------------------------
	//
	// The three fullscreen styles and the windowed case below decide the
	// SIZE; applying it is one call to orbiter_SetDisplayMode afterwards,
	// because every Win32 call the reference used here -- SetWindowLongA,
	// SetWindowPos, GetSystemMetrics, SystemParametersInfo -- acts on a
	// window this client does not own.
	const int  fsRequested = vData->fullscreen ? 1 : 0;
	int mw = 0, mh = 0, mhz = 0;

	if (bIsFullscreen) {

		switch (dwFSMode) {

			// True Fullscreen
			case 0:
			{
				dwDisplayMode = 0;

				// Was:
				//   if (Adapter < GetAdapterCount())
				//     if (Mode < GetAdapterModeCount(Adapter, D3DFMT_X8R8G8B8))
				//       EnumAdapterModes(Adapter, D3DFMT_X8R8G8B8, Mode, &mode);
				//
				// -- three guarded steps that between them left dwRenderWidth
				// at zero whenever any one of them failed. The mode list here
				// is the host's, the bounds check is inside the call, and the
				// monitor's current mode is a better answer to a bad index
				// than nothing at all.
				if (!orbiter_GetVideoMode((int)Mode, &mw, &mh, &mhz)) {
					LogWrn("Video mode index %u is not in the host's list; "
						   "using the monitor's current mode", Mode);
					if (!orbiter_GetCurrentVideoMode(&mw, &mh, &mhz)) { mw = mh = mhz = 0; }
				}
				dwRenderWidth  = (DWORD)mw;
				dwRenderHeight = (DWORD)mh;
			}
			break;

			// Fullscreen Window
			case 1:
			{
				dwDisplayMode = 1;
				// GetSystemMetrics(SM_CXSCREEN / SM_CYSCREEN), widened to
				// SM_CXVIRTUALSCREEN when pageflip is set. The host works the
				// extent out from the monitor it is putting the window on, so
				// nothing is computed here and the size is read back below.
				bIsFullscreen = false;
			}
			break;

			// Fullscreen Window with Taskbar
			case 2:
			{
				dwDisplayMode = 1;
				// SystemParametersInfo(SPI_GETWORKAREA) -- the screen minus
				// the taskbar. The host's counterpart is the monitor work
				// area, which is what leaves a panel or a dock visible.
				bIsFullscreen = false;
			}
			break;
		}
	}
	else {
		dwDisplayMode = 2;
		// Was GetWindowLongA / SetWindowLongA to add WS_CLIPCHILDREN |
		// WS_VISIBLE, then SetWindowPos to vData->winw x winh when
		// trystencil ("Force window size") was set. The style bits have no
		// counterpart -- there are no child windows to clip and the window is
		// already mapped -- and the size is passed through.
		mw = vData->winw;
		mh = vData->winh;
		dwRenderWidth  = (DWORD)mw;
		dwRenderHeight = (DWORD)mh;
	}

	// Vertical sync. Was d3dPP.PresentationInterval, set in both
	// Create*Mode() functions; the present mode belongs to the swapchain, so
	// it is asked for rather than set.
	orbiter_SetVSync(bNoVSync ? 0 : 1);

	orbiter_SetDisplayMode(fsRequested, (int)dwFSMode,
						   vData->pageflip ? 1 : 0,
						   vData->trystencil ? 1 : 0,
						   mw, mh, mhz);

	// Hardware CAPS Checks --------------------------------------------------
	//
	// Was GetDeviceCaps(Adapter, D3DDEVTYPE_HAL, &caps), one struct of
	// everything. Adopt() already filled the three Vulkan structs that
	// replace it, so there is no call to make and no HRESULT to test -- a
	// physical device always answers.

	// AA CAPS Checks --------------------------------------------------
	//
	// Was three CheckDeviceMultiSampleType calls, one per sample count, each
	// of which the driver could refuse. Vulkan states the answer as a bitmask
	// of the counts the framebuffer supports, so the three questions are one
	// field and the loop that reads it.
	DWORD aamax = 0;
	{
		const VkSampleCountFlags ok = caps->limits.framebufferColorSampleCounts &
									  caps->limits.framebufferDepthSampleCounts;
		if (ok & VK_SAMPLE_COUNT_2_BIT) aamax = 2;
		if (ok & VK_SAMPLE_COUNT_4_BIT) aamax = 4;
		if (ok & VK_SAMPLE_COUNT_8_BIT) aamax = 8;
	}

	MultiSample = (aamax < DWORD(Config->SceneAntialias)) ? aamax : DWORD(Config->SceneAntialias);

	if (MultiSample == 1) MultiSample = 0;

	// The caps log, line for line. Where a line has no counterpart it says so
	// rather than printing a number that means something else -- the whole
	// point of this block is that somebody reads it off a user's log.

	// MaxTextureBlendStages, MaxSimultaneousTextures and MaxVertexBlendMatrices
	// were limits of the FIXED-FUNCTION pipeline: how many texture stages
	// could be combined, how many textures a stage could see, how many
	// matrices vertex blending could weight. Vulkan has no fixed-function
	// pipeline, so there is nothing to limit and no number to print.
	LogAlw("MaxTextureBlendStages... : n/a (no fixed-function pipeline)");
	LogOapi("MaxTextureWidth......... : %u", caps->limits.maxImageDimension2D);
	LogOapi("MaxTextureHeight........ : %u", caps->limits.maxImageDimension2D);
	// MaxTextureRepeat bounded how far a texture coordinate could go before
	// wrapping stopped working. Vulkan states no such limit.
	LogOapi("MaxTextureRepeat........ : n/a (unbounded)");
	// VolumeTextureAddressCaps named the address modes legal on a volume
	// texture. Vulkan's sampler address modes are not per-image-type.
	LogOapi("VolTexAddressCaps....... : n/a (address modes are sampler state)");
	LogAlw("MaxVolumeExtent......... : %u", caps->limits.maxImageDimension3D);
	// MaxPrimitiveCount and MaxVertexIndex were two numbers for one thing:
	// how large an index may be in a single draw.
	LogAlw("MaxDrawIndexedIndexValue : %u", caps->limits.maxDrawIndexedIndexValue);
	LogAlw("MaxVertexIndex.......... : %u", caps->limits.maxDrawIndexedIndexValue);
	LogAlw("MaxAnisotropy........... : %f", caps->limits.maxSamplerAnisotropy);
	LogAlw("MaxSampledImagesPerStage : %u", caps->limits.maxPerStageDescriptorSampledImages);
	LogAlw("MaxStreams.............. : %u", caps->limits.maxVertexInputBindings);
	LogAlw("MaxStreamStride......... : %u", caps->limits.maxVertexInputBindingStride);
	LogAlw("MaxVertexBlendMatrices.. : n/a (no fixed-function pipeline)");
	// MaxVShaderInstructionsExecuted capped a shader's dynamic instruction
	// count. SPIR-V has no such cap.
	LogAlw("MaxVShaderInstrExecuted. : n/a (unbounded)");
	LogAlw("MaxPointSize............ : %f", caps->limits.pointSizeRange[1]);
	// VertexShaderVersion and PixelShaderVersion said which shader model the
	// driver spoke. The counterpart is which Vulkan version it speaks, and
	// there is one number for both stages because SPIR-V is one language.
	LogAlw("VulkanApiVersion........ : %u.%u.%u",
		   VK_VERSION_MAJOR(caps->apiVersion), VK_VERSION_MINOR(caps->apiVersion),
		   VK_VERSION_PATCH(caps->apiVersion));
	LogAlw("DriverVersion........... : 0x%X", caps->driverVersion);
	LogOapi("NumSimultaneousRTs...... : %u", caps->limits.maxColorAttachments);
	// D3DPTEXTURECAPS_POW2 and NONPOW2CONDITIONAL asked whether the hardware
	// needed power-of-two textures. Vulkan has no such restriction at all --
	// maxImageDimension2D is the only bound on an image's size -- so the two
	// checks below that could fail on Windows cannot fail here.
	LogAlw("Non-power-of-2 textures. : Yes (unconditional)");
	// VertexDeclCaps named the packed vertex types a driver might refuse;
	// Vulkan answers per format, which is what the three lines further down do.
	LogOapi("MiscCaps................ : n/a (pipeline state, not device caps)");
	LogAlw("DevCaps................. : n/a (see VkPhysicalDeviceFeatures)");

	// The D3DRS_COLORWRITEENABLE check stood here and could fail. Its
	// counterpart is VkPipelineColorBlendAttachmentState::colorWriteMask,
	// which is core and mandatory, so there is nothing to test.

	// The two non-power-of-two checks stood here and could set bFail. See
	// the log line above: neither can fail.

	// Was:
	//   if ((caps.PixelShaderVersion&0xFFFF)<0x0300 ||
	//       (caps.VertexShaderVersion&0xFFFF)<0x0300) bFail = true;
	//
	// -- "Shader Model 3.0 or this client will not run". The counterpart is
	// the Vulkan version, and 1.1 is the floor because the client's uniform
	// blocks are declared layout(scalar): VK_EXT_scalar_block_layout is
	// promoted to core in 1.2 and available as an extension from 1.1, and
	// std140 would repad every struct VulkanUtil.h static_asserts the size of.
	if (caps->apiVersion < VK_API_VERSION_1_1) {
		LogErr("[Vulkan 1.1 or newer is required]");
		bFail = true;
	}
	else if (caps->apiVersion < VK_API_VERSION_1_2) {
		LogWrn("[Vulkan 1.1: scalar block layout is an extension on this driver]");
	}

	// Do Some Additional Hardware Checks ===================================================================

	// D3DPMISCCAPS_SEPARATEALPHABLEND -- whether colour and alpha could be
	// blended with different factors. Vulkan's blend state carries separate
	// src/dst factors and a separate op for alpha in every case, so the
	// answer is not conditional.
	LogOapi("Separate AlphaBlend..... : Yes (always)");

	// Check shadow mapping support
	//
	bool bShadowMap = true;
	if (!pDevice->SupportsRenderTarget(VK_FORMAT_R32_SFLOAT)) bShadowMap = false;
	if (pDevice->SelectDepthFormat() == VK_FORMAT_UNDEFINED) bShadowMap = false;

	if (bShadowMap) LogOapi("Shadow Mapping.......... : Yes");
	else			LogOapi("Shadow Mapping.......... : No");

	// Was D3DFMT_A16B16G16R16F as a render target, plus a depth-stencil match.
	bool bFloat16BB = true;
	if (!pDevice->SupportsRenderTarget(VK_FORMAT_R16G16B16A16_SFLOAT)) bFloat16BB = false;
	if (pDevice->SelectDepthFormat() == VK_FORMAT_UNDEFINED) bFloat16BB = false;

	if (bFloat16BB) LogOapi("R16G16B16A16_SFLOAT..... : Yes");
	else		    LogOapi("R16G16B16A16_SFLOAT..... : No");

	// The four D3DUSAGE_QUERY_VERTEXTEXTURE checks. All four had to pass or
	// the client refused to run, and that is kept: the terrain and cloud
	// shaders sample elevation from the vertex stage.
	const bool VT16BB = pDevice->SupportsVertexTexture(VK_FORMAT_R16G16B16A16_SFLOAT);
	const bool VT32BB = pDevice->SupportsVertexTexture(VK_FORMAT_R32G32B32A32_SFLOAT);
	const bool VT16BC = pDevice->SupportsVertexTexture(VK_FORMAT_R16_SFLOAT);
	const bool VT32BC = pDevice->SupportsVertexTexture(VK_FORMAT_R32_SFLOAT);

	LogOapi("Vertex_R16G16B16A16F.... : %s", VT16BB ? "Yes" : "No");
	LogOapi("Vertex_R32G32B32A32F.... : %s", VT32BB ? "Yes" : "No");
	LogOapi("Vertex_R16F............. : %s", VT16BC ? "Yes" : "No");
	LogOapi("Vertex_R32F............. : %s", VT32BC ? "Yes" : "No");

	if (!VT16BB || !VT32BB || !VT16BC || !VT32BC) bFail = true;

	// bVertexTexture was declared and never assigned in the Windows file --
	// HasVertexTextureSup() therefore always returned FALSE. The four checks
	// immediately above are the answer it was meant to report, so it is
	// assigned here.
	bVertexTexture = (VT16BB && VT32BB && VT16BC && VT32BC);

	bool bFloat32BB = pDevice->SupportsRenderTarget(VK_FORMAT_R32G32B32A32_SFLOAT);

	if (bFloat32BB) LogOapi("R32G32B32A32_SFLOAT..... : Yes");
	else		    LogOapi("R32G32B32A32_SFLOAT..... : No");

	// Was D3DFMT_D32F_LOCKABLE -- a depth buffer the CPU could read. Vulkan
	// has no lockable depth format; the question that survives is whether
	// D32_SFLOAT works as a depth attachment, and reading it back is
	// ReadTexture's job.
	LogOapi("D32_SFLOAT.............. : %s",
			pDevice->SupportsDepthStencil(VK_FORMAT_D32_SFLOAT) ? "Yes" : "No");

	LogOapi("A2R10G10B10............. : %s",
			pDevice->SupportsRenderTarget(VK_FORMAT_A2R10G10B10_UNORM_PACK32) ? "Yes" : "No");

	// Was D3DFMT_L8, the single-channel 8-bit format, as a render target.
	LogOapi("R8_UNORM................ : %s",
			pDevice->SupportsRenderTarget(VK_FORMAT_R8_UNORM) ? "Yes" : "No");

	// D3DDTCAPS_DEC3N / _FLOAT16_2 / _FLOAT16_4 as vertex attribute formats.
	LogOapi("A2B10G10R10_SNORM....... : %s",
			SupportsVertexFormat(pDevice->GetPhysicalDevice(),
								 VK_FORMAT_A2B10G10R10_SNORM_PACK32) ? "Yes" : "No");
	LogOapi("R16G16_SFLOAT........... : %s",
			SupportsVertexFormat(pDevice->GetPhysicalDevice(),
								 VK_FORMAT_R16G16_SFLOAT) ? "Yes" : "No");
	LogOapi("R16G16B16A16_SFLOAT vtx. : %s",
			SupportsVertexFormat(pDevice->GetPhysicalDevice(),
								 VK_FORMAT_R16G16B16A16_SFLOAT) ? "Yes" : "No");

	// Check (Log) whether orbiter runs on WINE
	//
	LogOapi("Runs under WINE......... : %s", OapiExtension::RunsUnderWINE() ? "Yes" : "No");
	LogOapi("VulkanClient Build Date. : %u", BuildDate());

	// The commented-out GetLocaleInfoEx block stood here in the Windows file
	// and is left out rather than converted: it was already dead, and its
	// counterpart would be localeconv(), which answers a different question.

	// Check MipMap autogeneration
	//
	// D3DCAPS2_CANAUTOGENMIPMAP, and a warning when it was absent. It is
	// always absent here, because Vulkan generates nothing -- so this is a
	// statement rather than a warning: VulkanDevice::GenerateMipmaps blits
	// down the chain itself, which is what SurfNative::GenerateMipMaps calls.
	LogOapi("MipMap generation....... : by the client (Vulkan auto-generates none)");

	if (bFail) {
		oapiWriteLog((char*)"Vulkan: FAIL: !! Graphics card doesn't meet the minimum requirements to run !!");
		MessageBoxA(NULL, "Graphics card doesn't meet the minimum requirements to run VulkanClient.",
					"VulkanClient Error", MB_OK);
		return -1;
	}

	// Was: if (bIsFullscreen) CreateFullscreenMode(); else CreateWindowedMode();
	// The two differed only in the D3DPRESENT_PARAMETERS they filled before
	// CreateDevice, and there are none to fill.
	HRESULT hr = AdoptDevice();

	if (FAILED(hr)) {
		LogErr("[Device Initialization Failed]");
		return hr;
	}

	LogOapi("Device-local memory..... : %u MB (total, not free)",
			(unsigned)(pDevice->GetLocalMemorySize() >> 20));

	// The D3DVIEWPORT9 block and SetViewport stood here. A Vulkan viewport is
	// not device state: it is either baked into a pipeline or set on a
	// command buffer with vkCmdSetViewport, and either way it belongs to the
	// draw rather than to start-up. The extent it would have been given is
	// dwRenderWidth x dwRenderHeight, which the scene callback receives as
	// its width and height arguments every frame.

	// The eleven CreateVertexDeclaration calls stood here. See the file
	// header: a vertex layout is not a device object in Vulkan.

	// The two D3DXCreateFontIndirect calls stood here. See VulkanFrame.h.

	LogAlw("=== [3DDevice Initialized] ===");
	return S_OK;
}


//-----------------------------------------------------------------------------
// Name: AdoptDevice()
// Desc: Was CreateFullscreenMode() and CreateWindowedMode().
//
//       THE TWO WERE ONE FUNCTION WITH TWO PREAMBLES. Both filled in
//       D3DPRESENT_PARAMETERS, both called CreateDevice, and both ended with
//       the identical six lines:
//
//           pDevice->GetRenderTarget(0, &pRenderTarget);
//           pDevice->GetDepthStencilSurface(&pDepthStencil);
//           pBackBuffer = new SurfNative(pRenderTarget,
//                             OAPISURFACE_BACKBUFFER | OAPISURFACE_RENDER3D |
//                             OAPISURFACE_RENDERTARGET, pDepthStencil);
//           SURFACE(pBackBuffer)->SetName("BackBuffer");
//
//       The preambles are gone with D3DPRESENT_PARAMETERS -- back buffer size
//       and format, swap effect, depth-stencil format, presentation interval
//       and multisampling are all properties of the swapchain, and the
//       swapchain is the core's. So what is left is those six lines, once.
//
//       THE EXTENT IS READ, NOT CHOSEN. On Windows the back buffer was
//       exactly the size asked for, because CreateDevice was told. Here the
//       window manager has the last word -- UIHost.cpp's note on
//       orbiter_GetSurfaceExtent measures a 2560x1440 request becoming a
//       2560x1372 client area once the title bar is accounted for -- so the
//       size that matters is the one the surface reports after
//       orbiter_SetDisplayMode has settled, and that is what is recorded.
//-----------------------------------------------------------------------------
HRESULT CVulkanFramework::AdoptDevice()
{
	_TRACE;

	if (!pDevice) return E_FAIL;

	// Was GetClientRect(hWnd, &rcScreenRect) in the windowed path and
	// SetRect(&rcScreenRect, 0, 0, dwRenderWidth, dwRenderHeight) in the
	// fullscreen one. One question, one answer: how big is the thing being
	// rendered into.
	int sw = 0, sh = 0;
	orbiter_GetSurfaceExtent(&sw, &sh);
	if (sw <= 0 || sh <= 0) orbiter_GetFramebufferSize(&sw, &sh);

	if (sw <= 0 || sh <= 0) {
		LogErr("CVulkanFramework::AdoptDevice: the host reports a zero-sized surface");
		return E_FAIL;
	}

	dwRenderWidth  = (DWORD)sw;
	dwRenderHeight = (DWORD)sh;

	// Was SetRect(&rcScreenRect, 0, 0, dwRenderWidth, dwRenderHeight). SetRect
	// is a USER32 entry point that assigns four fields, and it is not in
	// Src/Orbiter/Linux/windows.h -- so the four fields are assigned. LONG,
	// not long: 'long' is 64 bits here and 32 on Win64, which is the same
	// narrowing _RECT() in VulkanUtil.h exists to avoid.
	rcScreenRect.left   = 0;
	rcScreenRect.top    = 0;
	rcScreenRect.right  = LONG(dwRenderWidth);
	rcScreenRect.bottom = LONG(dwRenderHeight);

	// Was dwZBufferBitDepth = 24; dwStencilBitDepth = 8; hard-coded in both
	// functions because D3DFMT_D24S8 was hard-coded too. The core chooses the
	// depth format, so the two numbers are read off the format it chose --
	// which is the same one, on any driver that offers it.
	const VkFormat depthFormat = pDevice->SelectDepthFormat();

	switch (depthFormat) {
	case VK_FORMAT_D24_UNORM_S8_UINT:	dwZBufferBitDepth = 24; dwStencilBitDepth = 8; break;
	case VK_FORMAT_D32_SFLOAT_S8_UINT:	dwZBufferBitDepth = 32; dwStencilBitDepth = 8; break;
	case VK_FORMAT_D16_UNORM_S8_UINT:	dwZBufferBitDepth = 16; dwStencilBitDepth = 8; break;
	default:
		LogErr("CVulkanFramework::AdoptDevice: no depth-stencil format available");
		return E_FAIL;
	}

	if (dwDisplayMode == 2)
		LogAlw("[WINDOWED MODE] %u x %u,  hWindow=%s",
			   dwRenderWidth, dwRenderHeight, _PTR(hWnd));
	else
		LogAlw("[FULLSCREEN MODE] %u x %u,  hWindow=%s",
			   dwRenderWidth, dwRenderHeight, _PTR(hWnd));

	LogAlw("Render Size = [%u, %u]", dwRenderWidth, dwRenderHeight);
	LogAlw("Depth/Stencil = [%u, %u] bits (format %d)",
		   dwZBufferBitDepth, dwStencilBitDepth, int(depthFormat));

	if (MultiSample)
		LogAlw("Multisampling requested at %ux; the swapchain is the core's "
			   "and is single-sampled, so this applies to the client's own "
			   "render targets only", MultiSample);

	// GetRenderTarget(0) and GetDepthStencilSurface(). See
	// CreateAttachmentProxy: the client is never handed either image, so what
	// it gets is their extent and format.
	pRenderTarget = pDevice->CreateAttachmentProxy(dwRenderWidth, dwRenderHeight,
												   pDevice->SelectColourFormat(),
												   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
	pDepthStencil = pDevice->CreateAttachmentProxy(dwRenderWidth, dwRenderHeight,
												   depthFormat,
												   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);

	if (!pRenderTarget || !pDepthStencil) {
		LogErr("CVulkanFramework::AdoptDevice: could not describe the core's attachments");
		return E_FAIL;
	}

	pBackBuffer = (SURFHANDLE) new SurfNative(pRenderTarget,
					OAPISURFACE_BACKBUFFER | OAPISURFACE_RENDER3D | OAPISURFACE_RENDERTARGET,
					pDepthStencil);
	SURFACE(pBackBuffer)->SetName("BackBuffer");

	return S_OK;
}
