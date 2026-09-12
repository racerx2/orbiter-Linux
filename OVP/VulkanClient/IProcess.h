// ===================================================
// Copyright (C) 2021-2026 Jarmo Nikkanen
// licensed under LGPL v2
// ===================================================

#ifndef __IPROCESS_H
#define __IPROCESS_H

// Was <d3d9.h> and <d3dx9.h>. VulkanFrame.h is named explicitly rather than
// left to VulkanUtil.h's include chain: this header stores a
// ShaderReflection::Var* and a VulkanImageDesc by value, and VulkanTypes.h
// only forward-declares the class that holds the first. D3D9Util.h got away
// without it because D3DXHANDLE is a pointer to an opaque struct.
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
	// The two surface types D3D9 distinguished here -- LPDIRECT3DSURFACE9 for
	// a render target, LPDIRECT3DBASETEXTURE9 for a sampled texture -- are one
	// type in Vulkan: an image is one or the other by its usage flags, not by
	// its interface.
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
		// New: there is no sampler state on the device. D3D9 set eight
		// D3DSAMP_ values on a numbered slot and the runtime kept them; a
		// VkSampler is an immutable object built from those same numbers, so
		// the slot owns one and rebuilds it only when the flags change.
		VkSampler		pSampler;
		DWORD			smpFlags;	///< the flags pSampler was built from
		// The GLSL binding this slot's sampler is declared at. On Windows one
		// number answered both questions, because the sampler index was the
		// device slot. Here the index picks the entry in this array and the
		// binding says where in the descriptor set it goes; the two are equal
		// only by accident.
		uint32_t		binding;
	} pTextures[8];

	gcIPInterface::ipicull mesh_cull;

	SketchMesh *pMesh;
	std::map<std::string, SHADER> Shaders;
	VulkanDevice *pDevice;
	// pRtgBak[4] and pDepthBak are gone. Execute() saved the device's four
	// render targets and depth surface, set its own, drew, and put the old
	// ones back, because a render target is device state in D3D9. There is no
	// such state here -- the attachments live in a VkFramebuffer inside a
	// VkRenderPass, and BeginOffscreen/EndOffscreen bracket the draw with a
	// pass of their own and restore the frame's command buffer afterwards.
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
	// New, because a pipeline names its entry points.
	// VkPipelineShaderStageCreateInfo::pName needs the SPIR-V entry point at
	// pipeline-build time, where D3D9 only needed it at compile time. 'entry'
	// already keeps the pixel stage's.
	char	vsentry[32];

	std::list<std::string> def;

	// ------------------------------------------------------------------
	// Everything below is new: the machinery that replaces device state.
	// ------------------------------------------------------------------

	/// \brief Build the descriptor set layout, the pipeline layout, the two
	///        uniform buffers and the single descriptor set, once, from the
	///        reflection of the shaders this object compiled.
	bool	CreateResources();

	/// \brief Fetch or build the pipeline for one Execute(). Its arguments are
	///        what Execute() varies; everything else it used to set with
	///        SetRenderState is constant and goes straight into the
	///        create-info.
	VkPipeline GetPipeline(DWORD blendop, VkPrimitiveTopology topo, bool bCull,
						   bool bDepth, const VertexDecl *pDecl);

	/// \brief Counterpart of ID3DXConstantTable::SetValue -- put 'bytes' bytes
	///        at the variable's offset in its stage's uniform block.
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
	///        this behind them.
	bool	EnsureScratch(VkDeviceSize vbytes, VkDeviceSize ibytes);

	VkDescriptorSetLayout	vkSetLayout;
	VkPipelineLayout		vkPipeLayout;
	// A pool rather than a single set, because of the mesh template: that path
	// draws one group at a time and changes the texture between groups, which
	// on Windows was device state taking effect at the next DrawPrimitive. A
	// descriptor set is memory the recorded draw still refers to, so rewriting
	// one set between two draws in a command buffer changes what the first
	// draw samples as well. Hence a fresh set per draw, from a pool this
	// object resets at the top of every Execute().
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
	/// binding. Built only when one of these shaders declares a cube; a cube
	/// declaration filled with a 2D view faults.
	VulkanTexture		   *pWhiteCube;

	/// \brief The pipeline cache, keyed on what GetPipeline() takes, plus two
	///        things: the render pass, because a VkPipeline is valid only in a
	///        pass compatible with the one it was built against and this class
	///        draws into a different set of attachments on nearly every call;
	///        and the pixel shader module, because Activate() swaps that
	///        between calls where D3D9 needed only one SetPixelShader.
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
