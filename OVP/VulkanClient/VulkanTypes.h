// ==============================================================
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2006-2026 Martin Schweiger
//				 2012-2016 Jarmo Nikkanen
// ==============================================================
//
// THIS FILE IS THE LINUX COUNTERPART OF <d3d9.h> / <d3dx9.h>.
//
// It is the one file in this conversion that has no Windows original, and
// the reason is worth stating precisely so it is not mistaken for an
// emulation layer.
//
// Every D3D9Client source file opens with
//
//     #include <d3d9.h>
//     #include <d3dx9.h>
//
// and those are SYSTEM headers. They do not exist on Linux, and the thing
// that replaces them -- <vulkan/vulkan.h> -- does not carry the same set of
// names, because Direct3D 9 and Vulkan draw the line between "resource" and
// "description of a resource" in different places:
//
//   * IDirect3DTexture9 is ONE object holding image storage, a format, a
//     mip chain and the state used to sample it. Vulkan splits that into
//     VkImage + VkDeviceMemory + VkImageView + VkSampler, four handles with
//     no owner. Somebody has to hold them together; on Linux that somebody
//     is the client.
//
//   * IDirect3DVertexBuffer9 is storage plus a Lock/Unlock protocol.
//     Vulkan has VkBuffer + VkDeviceMemory + vkMapMemory, again unowned.
//
//   * IDirect3DVertexDeclaration9 is a DEVICE OBJECT in D3D9 -- you create
//     it through the device and bind it at draw time. In Vulkan the vertex
//     layout is not an object at all: it is immutable data baked into a
//     VkPipeline at pipeline-creation time. So its counterpart here is a
//     plain constant table, not a handle. That is why VertexDecl below is
//     the only type this header defines completely: the declarations
//     themselves live in VulkanUtil.h and must be complete there.
//
// So this header holds exactly what <d3d9.h> held for the Windows client and
// nothing more: the NAMES of the resource types the client passes around,
// and the vertex-layout description. It defines no device interface, no
// IUnknown, no QueryInterface/AddRef/Release, no SetRenderState and no
// DrawPrimitive. The classes named here are forward declarations whose
// definitions belong to the files that own them, exactly as the D3D9 client
// let <d3d9.h> name IDirect3DDevice9 while the device itself came from
// elsewhere.
// ==============================================================

#ifndef __VULKANTYPES_H
#define __VULKANTYPES_H

#include <vulkan/vulkan.h>
#include <stdint.h>


// ------------------------------------------------------------------------------------
// The client-side resource types.
//
// These are the names <d3d9.h> supplied. Their definitions live with the
// files that own them:
//
//   VulkanDevice   -- the logical device, queues and the recording command
//                     buffer. Counterpart of LPDIRECT3DDEVICE9. Defined by
//                     the conversion of D3D9Frame.h, which is the file that
//                     created and owned the D3D9 device.
//   VulkanTexture  -- VkImage + memory + view + extent + format + mips.
//                     Counterpart of LPDIRECT3DTEXTURE9.
//   VulkanBuffer   -- VkBuffer + memory + size, and the map/unmap that
//                     replaces Lock/Unlock. Counterpart of both
//                     LPDIRECT3DVERTEXBUFFER9 and LPDIRECT3DINDEXBUFFER9;
//                     Vulkan does not distinguish them by type, only by the
//                     usage flags given at creation.
//   ShaderReflection -- the interface a compiled SPIR-V module declares:
//                     its uniform blocks, their members and offsets, and its
//                     sampler bindings. Counterpart of LPD3DXCONSTANTTABLE,
//                     which D3DX produced as a side product of compiling
//                     HLSL. One per stage, exactly as D3D9Util.h kept one
//                     for the vertex shader and one for the pixel shader.
//                     The shader objects themselves need no wrapper: a
//                     VkShaderModule stands in directly for both
//                     LPDIRECT3DVERTEXSHADER9 and LPDIRECT3DPIXELSHADER9.
// ------------------------------------------------------------------------------------

class VulkanDevice;
class VulkanTexture;
class VulkanBuffer;
class ShaderReflection;


// ------------------------------------------------------------------------------------
// Image description. Counterpart of D3DSURFACE_DESC.
//
// D3DSURFACE_DESC is Format, Type, Usage, Pool, MultiSampleType, Width and
// Height, and it is FILLED IN BY THE DEVICE -- GetDesc() asks the resource
// what it is. Vulkan images answer no such question: once created, a VkImage
// remembers nothing you can query. So the client records what it asked for,
// which is why this is a struct the client fills rather than one it reads.
//
// Field by field:
//   Type            gone. It said "surface" or "texture"; Vulkan has one
//                   image type, and the distinction survives only as the
//                   SAMPLED / COLOR_ATTACHMENT usage bits below.
//   Pool +
//   D3DUSAGE_DYNAMIC  became HostVisible. D3DPOOL_SYSTEMMEM and
//                   D3DUSAGE_DYNAMIC both meant "the CPU can reach this";
//                   in Vulkan that is one property of the memory the image
//                   is bound to, not two properties of the image.
//   Usage           became VkImageUsageFlags. D3DUSAGE_RENDERTARGET is
//                   COLOR_ATTACHMENT_BIT; being a texture is SAMPLED_BIT.
//                   D3DUSAGE_AUTOGENMIPMAP HAS NO COUNTERPART -- Vulkan does
//                   not generate mipmaps for you, so the client blits down
//                   the chain itself. See SurfNative::GenerateMipMaps.
// ------------------------------------------------------------------------------------

typedef struct {
	uint32_t				Width;
	uint32_t				Height;
	uint32_t				Depth;			///< was D3DVOLUME_DESC::Depth; 1 for a 2D image
	VkFormat				Format;
	VkImageUsageFlags		Usage;
	uint32_t				Mips;			///< 1 = no mipmaps
	uint32_t				Layers;			///< 1 for a plain image, 6 for a cube map
	bool					HostVisible;	///< was D3DPOOL_SYSTEMMEM / D3DUSAGE_DYNAMIC
	VkSampleCountFlagBits	Samples;		///< was D3DMULTISAMPLE_TYPE
} VulkanImageDesc;

// Layers is the CUBE MAP, and it is here rather than reusing Depth because the
// two are different things and confusing them produces an image of the wrong
// TYPE, not merely the wrong size.
//
// D3D9 had a third texture interface, IDirect3DCubeTexture9, alongside
// IDirect3DTexture9 and IDirect3DVolumeTexture9. Vulkan has neither a cube
// type nor a cube flag on the view alone: a cube map is a VK_IMAGE_TYPE_2D
// image with arrayLayers 6 and VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, sampled
// through a view whose viewType is VK_IMAGE_VIEW_TYPE_CUBE. A 3D image has
// arrayLayers 1 and extent.depth 6, which is a different image, sampled with
// three coordinates instead of a direction.
//
// So Depth stays what D3DVOLUME_DESC::Depth was and Layers is new, and
// GetCubeMapSurface(face, level) becomes a view with baseArrayLayer = face --
// see VulkanDevice::CreateFaceView.

// Depth is here and not in a second struct because Vulkan has one image type.
// D3D9 needed D3DSURFACE_DESC and D3DVOLUME_DESC because IDirect3DTexture9 and
// IDirect3DVolumeTexture9 were different interfaces; a VkImage is 2D or 3D by
// its imageType alone, so one description covers both and CreateTexture3D
// fills the field CreateTexture leaves at 1.


// ------------------------------------------------------------------------------------
// Vertex layout description.
//
// Counterpart of D3DVERTEXELEMENT9[] and IDirect3DVertexDeclaration9.
//
// A D3DVERTEXELEMENT9 is
//     { Stream, Offset, Type, Method, Usage, UsageIndex }
// and it binds a byte range of the vertex to an HLSL SEMANTIC NAME, e.g.
// POSITION0 or TEXCOORD1. Vulkan has no semantic names -- GLSL inputs are
// matched by an explicit location number -- so the semantic has to become a
// number here, and the rule used throughout this client is the only one that
// can be applied mechanically and checked by eye:
//
//     LOCATION = THE ELEMENT'S INDEX IN THE DECLARATION.
//
// The first element is location 0, the second location 1, and so on, in the
// order the Windows declaration listed them. The GLSL translations of the
// shaders are written to match, which is why the location is spelled out in
// each entry below rather than being implied by array position -- so that a
// reordered array is a visible change, not a silent one.
//
// Method is gone: D3DDECLMETHOD_DEFAULT was the only value used anywhere in
// the D3D9 client, and it means "no tessellator", which is Vulkan's only
// behaviour for a vertex input.
//
// Stream is gone from the element and moved to Binding(): every declaration
// in this client uses stream 0.
// ------------------------------------------------------------------------------------

typedef struct {
	uint32_t	location;		///< GLSL layout(location=) of the target input
	VkFormat	format;			///< Counterpart of D3DDECLTYPE
	uint32_t	offset;			///< Byte offset within the vertex, unchanged
} VertexAttrib;


class VertexDecl
{
public:
	VertexDecl(const char *name, uint32_t stride, const VertexAttrib *attribs, uint32_t count)
		: name(name), stride(stride), attribs(attribs), count(count) {}

	/// \brief The one vertex binding. Every declaration in this client is stream 0.
	VkVertexInputBindingDescription Binding(uint32_t binding = 0) const
	{
		VkVertexInputBindingDescription b = {};
		b.binding = binding;
		b.stride = stride;
		b.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
		return b;
	}

	/// \brief Fill a caller-supplied array with the attribute descriptions.
	/// \return number written, or 0 if 'max' is too small to hold them all.
	uint32_t Attributes(VkVertexInputAttributeDescription *out, uint32_t max, uint32_t binding = 0) const
	{
		if (!out || max < count) return 0;
		for (uint32_t i = 0; i < count; i++) {
			out[i].location = attribs[i].location;
			out[i].binding = binding;
			out[i].format = attribs[i].format;
			out[i].offset = attribs[i].offset;
		}
		return count;
	}

	const char *		Name() const	{ return name; }
	uint32_t			Stride() const	{ return stride; }
	uint32_t			Count() const	{ return count; }
	const VertexAttrib *Attribs() const	{ return attribs; }

private:
	const char *		name;		///< For logging. D3D9 declarations were anonymous.
	uint32_t			stride;		///< Byte size of one vertex
	const VertexAttrib *attribs;
	uint32_t			count;
};

#endif // !__VULKANTYPES_H
