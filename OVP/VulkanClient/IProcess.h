// ===================================================
// Copyright (C) 2021-2026 Jarmo Nikkanen
// licensed under LGPL v2
//
// LINUX/VULKAN CONVERSION OF OVP/D3D9Client/IProcess.h
//
// WHAT THIS CLASS IS, AND WHY IT SURVIVES ALMOST INTACT.
//
// ImageProcessing is the client's "run a pixel shader over a rectangle"
// helper: give it a shader file and an entry point, set some constants and
// textures, point it at a render target, and Execute() draws two triangles
// covering the target so the shader runs once per pixel. Nothing about that
// idea is Direct3D-specific, so the class keeps its name, its members, its
// method list and the order of everything in it. What changes is what each
// member IS.
//
// THE SIX SUBSTITUTIONS, once, so the rest reads as the same file:
//
//   LPDIRECT3DDEVICE9        -> VulkanDevice *
//   LPDIRECT3DPIXELSHADER9   -> VkShaderModule   (and the vertex shader too;
//                               Vulkan has one module type, not one per stage)
//   LPD3DXCONSTANTTABLE      -> ShaderReflection *
//   D3DXHANDLE               -> const ShaderReflection::Var *
//   LPDIRECT3DSURFACE9       -> VulkanTexture *
//   LPDIRECT3DBASETEXTURE9   -> VulkanTexture *   (D3D9 needed a base
//                               interface because a 2D texture and a cube map
//                               were different types; a VkImage is one type
//                               whose view says which it is)
//
// THREE MEMBERS DISAPPEAR ENTIRELY, and each is worth naming:
//
//   pRtgBak[4] and pDepthBak. On Windows Execute() read the four current
//   render targets and the depth surface back out of the device, set its own,
//   drew, and put the old ones back -- because a render target IS device
//   state in D3D9 and there was nowhere else to keep it. Vulkan has no such
//   state to save: the attachments are baked into a VkFramebuffer inside a
//   VkRenderPass, and VulkanDevice::BeginOffscreen/EndOffscreen bracket the
//   draw with a pass of their own and restore the frame's command buffer
//   afterwards. So the save/restore is not translated -- it is performed by
//   the two calls that replace SetRenderTarget.
//
//   D3DVIEWPORT9 iVP is kept, as a VkViewport, because SetupViewPort() still
//   computes it and the projection matrix from the target's size -- but it is
//   no longer pushed with SetViewport(): BeginOffscreen sets the viewport and
//   scissor from the attachment extent, which is the same number arrived at
//   from the same place.
//
// SEVERAL MEMBERS ARE NEW, and none has a D3D9 counterpart at all: a
// descriptor set layout, a pipeline layout, one descriptor set, the two
// uniform buffers the constant writes land in, a pipeline cache keyed on what
// Execute() varies, and the scratch vertex/index buffers that replace
// DrawPrimitiveUP's hidden staging. Every one of them is the same addition,
// for the same reason, as the ones ShaderClass grew in VulkanUtil.h -- see
// the notes there rather than repeating them.
// ===================================================

#ifndef __IPROCESS_H
#define __IPROCESS_H

// Was <d3d9.h> and <d3dx9.h>. VulkanUtil.h brings in <vulkan/vulkan.h>, the
// vertex declarations, SMVERTEX, SketchMesh and the two shader compilers --
// which is the same set of things D3D9Util.h brought in beside the two
// Direct3D headers.
// VulkanFrame.h is named explicitly rather than left to VulkanUtil.h's own
// include chain: this header stores a ShaderReflection::Var* and a
// VulkanImageDesc BY VALUE, and VulkanTypes.h only forward-declares the class
// that holds the first. D3D9Util.h had the same relationship with d3dx9.h and
// got away with it because D3DXHANDLE is a typedef for a pointer to an opaque
// struct -- no definition needed anywhere.
#include <list>
#include <map>
#include <string>
#include <vector>
#include "OrbiterAPI.h"
#include "VulkanFrame.h"
#include "VulkanUtil.h"
#include "gcCore.h"

// Address mode WRAP is assumed by default
// Filter POINT is assumed by default
/*
#define IPF_WRAP		0x0000
#define IPF_WRAP_U		0x0000
#define IPF_WRAP_V		0x0000
#define IPF_WRAP_W		0x0000
#define IPF_CLAMP		0x0007
#define IPF_CLAMP_U		0x0001
#define IPF_CLAMP_V		0x0002
#define IPF_CLAMP_W		0x0004
#define IPF_MIRROR		0x0038
#define IPF_MIRROR_U	0x0008
#define IPF_MIRROR_V	0x0010
#define IPF_MIRROR_W	0x0020
#define IPF_POINT		0x0000
#define IPF_LINEAR		0x0040
#define IPF_PYRAMIDAL	0x0080
#define IPF_GAUSSIAN	0x0100*/


class ImageProcessing {

public:


	// ----------------------------------------------------------------------------------
	// Create a IPI (Image processing interface) which allows to process and create data via GPU
	// _file is the filename where user shader code exists
	// _entry is the function entry point "myFunc". e.g. float4 myFunc(float x : TEXCOORD0, float y : TEXCOORD1) : COLOR 
	// which contains the executed code with two input variables x, y
	// ppf is a list of preprocessor directives e.g. "_MYSECTION;_DEBUG" used like #if defined(_MYSECTION) ..code.. #endif
	// ----------------------------------------------------------------------------------
			ImageProcessing(VulkanDevice *pDev, const char *_file, const char *_entry, const char *ppf = NULL, const char *_vsentry = NULL);
			~ImageProcessing();

	bool	CompileShader(const char *Entry);
	bool	Activate(const char *Shader = NULL);

	// ----------------------------------------------------------------------------------
	// Use the 'Set' functions to assign a value into a shader constants ( e.g. uniform extern float4 myVector; )
	// If the variable "var" is defined but NOT used by the shader code, the variable "var" doesn't exists in
	// a constant table and an error is printed when trying to assign a value to it.
	// ----------------------------------------------------------------------------------
	void	SetFloat(const char *var, float val);
	void	SetInt(const char *var, int val);
	void	SetBool(const char *var, bool val);
	// ----------------------------------------------------------------------------------
	void	SetFloat(const char *var, const void *val, int bytes);
	void	SetInt(const char *var, const int *val, int bytes);
	void	SetBool(const char *var, const bool *val, int bytes);
	void	SetStruct(const char *var, const void *val, int bytes);

	// ----------------------------------------------------------------------------------
	// Use the 'Set' functions to assign a value into a shader constants ( e.g. uniform extern float4 myVector; )
	// If the variable "var" is defined but NOT used by the shader code, the variable "var" doesn't exists in
	// a constant table and an error is printed when trying to assign a value to it.
	// ----------------------------------------------------------------------------------
	void	SetVSFloat(const char *var, float val);
	void	SetVSInt(const char *var, int val);
	void	SetVSBool(const char *var, bool val);
	// ----------------------------------------------------------------------------------
	void	SetVSFloat(const char *var, const void *val, int bytes);
	void	SetVSInt(const char *var, const int *val, int bytes);
	void	SetVSBool(const char *var, const bool *val, int bytes);
	void	SetVSStruct(const char *var, const void *val, int bytes);

	// ----------------------------------------------------------------------------------
	// SetTexture can be used to assign a texture and a sampler state flags to a sampler
	// In a shader code sampler is defined as (e.g. sampler mySamp; ) where "mySamp" is
	// the variable passed to SetTexture function. It's then used in a shader code like
	// tex2D(mySamp, float2(x,y))
	// ----------------------------------------------------------------------------------
	void	SetTexture(const char *var, SURFHANDLE hTex, DWORD flags);
	
	// ----------------------------------------------------------------------------------
	// SetOutput assigns a render target to the IP interface. "id" is an index of the render
	// target with a maximum value of 3. It is possible to render in four different targets
	// at the same time. Multisample AA is only supported with one render target. Unbound
	// a render target by setting it to NULL. After a NULL render target all later targets
	// are ignored.
	// ----------------------------------------------------------------------------------
	void	SetOutput(int id, SURFHANDLE hTex);

	// ----------------------------------------------------------------------------------
	bool	IsOK();
	void	SetTemplate(float w = 1.0f, float h = 1.0f, float x = 0.0f, float y = 0.0f);
	void	SetMesh(const MESHHANDLE hMesh, const char *tex = NULL, gcIPInterface::ipicull = gcIPInterface::ipicull::None);

	bool	Execute(bool bInScene = false);
	bool	Execute(const char *shader, bool bInScene, DWORD blendop);
	bool    Execute(DWORD blendop, bool bInScene = false, gcIPInterface::ipitemplate tmp = gcIPInterface::ipitemplate::Rect, int gpr = -1);

	// ----------------------------------------------------------------------------------
	int		FindDefine(const char *key);

	// Native Vulkan calls --------------------------------------------------------------
	//
	// Was "Native DirectX calls". Same three functions, same purpose -- take
	// the API's own object rather than a SURFHANDLE -- and the object is a
	// VulkanTexture now. The two surface types D3D9 distinguished here,
	// LPDIRECT3DSURFACE9 for a render target and LPDIRECT3DBASETEXTURE9 for a
	// sampled texture, are one type in Vulkan: an image is a render target or
	// a texture by its usage flags, not by its interface.
	void	SetDepthStencil(VulkanTexture *hSrf = NULL);
	void	SetOutputNative(int id, VulkanTexture *hSrf);
	void	SetTextureNative(const char *var, VulkanTexture *hTex, DWORD flags);

private:

	bool	SetupViewPort();	

	typedef struct {
		VkShaderModule		pPixel;
		ShaderReflection   *pPSConst;
	} SHADER;

	struct {
		VulkanTexture  *hTex;
		DWORD			flags;
		// NEW, AND THE PRICE OF HAVING NO SAMPLER STATE ON THE DEVICE. D3D9
		// set eight D3DSAMP_ values on a numbered slot and the runtime kept
		// them; a VkSampler is an immutable object built from those same
		// eight numbers, so the slot has to own one. Rebuilt only when the
		// flags change, which is what the eight SetSamplerState calls cost
		// when they changed nothing. Same member, same reasoning, as
		// ShaderClass::TexParams::pSampler.
		VkSampler		pSampler;
		DWORD			smpFlags;	///< the flags pSampler was built from
		// The GLSL binding number this slot's sampler is declared at. D3D9
		// needed no such thing: the sampler INDEX was the device slot, so one
		// number answered both questions. Here the index picks the entry in
		// this array -- which is what GetSamplerIndex answered on Windows --
		// and the binding says where in the descriptor set it goes, and the
		// two are equal only by accident.
		uint32_t		binding;
	} pTextures[8];

	gcIPInterface::ipicull mesh_cull;

	SketchMesh *pMesh;
	std::map<std::string, SHADER> Shaders;
	VulkanDevice *pDevice;
	// pRtgBak[4] and pDepthBak are gone; see the file header.
	VulkanTexture *pRtg[4];
	VulkanTexture *pDepth;
	ShaderReflection *pVSConst;
	ShaderReflection *pPSConst;
	VkShaderModule pVertex;
	VkShaderModule pPixel;
	VulkanImageDesc desc;
	FMATRIX4     mVP;
	FVECTOR4     vTemplate;
	VkViewport   iVP;
	const ShaderReflection::Var *hVP;
	const ShaderReflection::Var *hPos;
	const ShaderReflection::Var *hSiz;
	SMVERTEX	*pOcta;

	int		mesh_tex_idx;
	char	file[256];
	char	ppf[256];
	char	entry[32];
	// NEW, AND ONLY BECAUSE A PIPELINE NAMES ITS ENTRY POINTS. D3D9 compiled
	// an entry point into a shader object and SetVertexShader took the object;
	// the name was not needed again. VkPipelineShaderStageCreateInfo::pName
	// names the SPIR-V entry point at pipeline-build time, so the vertex
	// entry has to be kept -- 'entry' already keeps the pixel one, which is
	// why there is no second addition for that stage.
	char	vsentry[32];

	std::list<std::string> def;

	// ------------------------------------------------------------------
	// Everything below this line is new, and none of it has a D3D9
	// counterpart -- it is the machinery that replaces device state.
	// ------------------------------------------------------------------

	/// \brief Build the descriptor set layout, the pipeline layout, the two
	///        uniform buffers and the single descriptor set, once, from the
	///        reflection of the shaders this object compiled.
	bool	CreateResources();

	/// \brief Fetch or build the pipeline for one Execute(). Its arguments
	///        are exactly what Execute() varies: the blend mode, the
	///        primitive topology the template implies, whether the mesh
	///        path's D3DCULL_CCW is wanted, whether there is a depth
	///        attachment, and the vertex declaration. Everything else that
	///        Execute() used to set with SetRenderState is constant and is
	///        written into the create-info directly.
	VkPipeline GetPipeline(DWORD blendop, VkPrimitiveTopology topo, bool bCull,
						   bool bDepth, const VertexDecl *pDecl);

	/// \brief Counterpart of ID3DXConstantTable::SetValue -- put 'bytes'
	///        bytes at the variable's offset in its stage's uniform block.
	///        The same function, for the same reason, as
	///        ShaderClass::WriteConstants.
	bool	WriteConstants(ShaderReflection *pCB, const ShaderReflection::Var *v,
						   const void *data, int bytes);

	/// \brief Make sure slot 'idx' owns a VkSampler matching its IPF_ flags.
	void	UpdateSampler(int idx);

	/// \brief Write the one descriptor set and bind it. Replaces the eight
	///        SetSamplerState calls plus SetTexture, per slot, that the
	///        Windows Execute() issued.
	bool	BindResources(VkCommandBuffer cmd);

	/// \brief The scratch buffers the two template draws copy into.
	///        DrawIndexedPrimitiveUP and DrawPrimitiveUP had the runtime do
	///        this behind them. Same pair, same reason, as ShaderClass's.
	bool	EnsureScratch(VkDeviceSize vbytes, VkDeviceSize ibytes);

	VkDescriptorSetLayout	vkSetLayout;
	VkPipelineLayout		vkPipeLayout;
	// A POOL AND NOT A SINGLE SET, and the reason is the mesh template. That
	// path draws one group at a time and changes the texture between groups
	// -- SetTexture(mesh_tex_idx, ...) on Windows, which was device state and
	// took effect at the next DrawPrimitive. A descriptor set is not device
	// state: it is memory the recorded draw still refers to, so rewriting the
	// same set between two draws in one command buffer changes what the FIRST
	// draw samples as well. Hence a fresh set per draw, out of a pool this
	// object owns and resets at the top of every Execute().
	VkDescriptorPool		vkPool;
	VkDescriptorSet			vkSet;		///< the set BindResources last wrote
	VulkanBuffer		   *pVSBuf;		///< the vertex stage's uniform block
	VulkanBuffer		   *pPSBuf;		///< the pixel stage's uniform block
	uint32_t				vsBinding;	///< where the VS block is declared
	uint32_t				psBinding;	///< where the PS block is declared
	VulkanBuffer		   *pScratchVB, *pScratchIB;
	VkDeviceSize			scratchVBSize, scratchIBSize;
	VulkanTexture		   *pWhite;		///< see BindResources
	/// The cube-shaped counterpart of pWhite, for an unset samplerCube
	/// binding. Built only when one of these shaders declares a cube -- see
	/// CreateResources. A cube declaration filled with a 2D view faults.
	VulkanTexture		   *pWhiteCube;

	/// \brief The pipeline cache. Keyed on what GetPipeline() takes, for the
	///        same reason ShaderClass::PipeKey is keyed on what Setup() takes
	///        -- plus the render pass, because a VkPipeline is valid only in a
	///        pass COMPATIBLE with the one it was built against and this class
	///        draws into a different set of attachments on nearly every call,
	///        and plus the pixel shader module, because Activate() swaps that
	///        between calls and D3D9 could change it with one SetPixelShader.
	struct PipeKey {
		DWORD				blend;
		VkPrimitiveTopology	topo;
		bool				cull;
		bool				depth;
		const VertexDecl   *pDecl;
		VkRenderPass		pass;
		VkShaderModule		ps;
		bool operator<(const PipeKey &o) const {
			if (blend != o.blend) return blend < o.blend;
			if (topo != o.topo) return topo < o.topo;
			if (cull != o.cull) return cull < o.cull;
			if (depth != o.depth) return depth < o.depth;
			if (pDecl != o.pDecl) return pDecl < o.pDecl;
			if (pass != o.pass) return pass < o.pass;
			return ps < o.ps;
		}
	};
	std::map<PipeKey, VkPipeline> Pipelines;
};

#endif
