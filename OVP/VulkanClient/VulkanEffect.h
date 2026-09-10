// ===========================================================================================
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2011-2026 Jarmo Nikkanen
// ===========================================================================================
//
// CONVERTED FROM OVP/D3D9Client/D3D9Effect.h, read end to end (193 lines).
//
// THE ONE THING THAT MATTERS HERE: ID3DXEffect HAS NO COUNTERPART, AND
// D3DXHANDLE WAS TWO DIFFERENT THINGS.
//
// A D3DX effect is a compiled .fx file holding TECHNIQUES (each a list of
// passes, each pass setting a shader pair and a block of render state) and
// PARAMETERS (matrices, colours, flags, textures) shared across them. The
// client drives it as
//
//     FX->SetTechnique(eVesselTech);      // pick a technique
//     FX->SetMatrix(eW, pW);              // set a parameter
//     FX->SetTexture(eTex0, pTex);        // set a texture parameter
//     FX->Begin(&passes, ...); FX->BeginPass(0); ... FX->EndPass(); FX->End();
//
// and BOTH eVesselTech and eW are spelled D3DXHANDLE, although they name
// completely different kinds of thing. D3DX could conflate them because both
// were opaque tokens into one object.
//
// Vulkan cannot. A technique is a VkPipeline -- shaders plus depth, blend,
// cull and vertex layout, all frozen at creation. A parameter is either bytes
// at an offset in a uniform buffer or a descriptor. Selecting a technique is
// vkCmdBindPipeline; setting a parameter is a memcpy or a descriptor write.
// They have nothing in common, so this header gives them separate types:
//
//     D3DXHANDLE eVesselTech  ->  TECHHANDLE   (a technique: a pipeline set)
//     D3DXHANDLE eW           ->  HANDLE       (a parameter, as ShaderClass
//                                               already uses: a
//                                               ShaderReflection::Var*)
//
// Getting that wrong is the same class of mistake as
// ShaderClass::SetPSConstants(HANDLE) writing through the vertex table --
// which the Windows source actually does. Separate types make it a compile
// error rather than a silent one.
//
// FX itself becomes VulkanEffectFile, declared below: the client's own
// replacement for the D3DX effect framework, built on VkPipeline and uniform
// buffers. It is NOT a D3D9 emulation -- there is no IDirect3DDevice9 behind
// it and no render state to set. It exists because the .fx file's structure
// (named techniques, named parameters) is real content that the shaders and
// ~700 call sites are written against, and that structure has to live
// somewhere once D3DX is gone.
//
// The rest of the file is the type mapping:
//   LPDIRECT3DDEVICE9        -> VulkanDevice *
//   LPDIRECT3DVERTEXBUFFER9  -> VulkanBuffer *
//   LPDIRECT3DTEXTURE9       -> VulkanTexture *
//   LPDIRECT3DCUBETEXTURE9   -> VulkanTexture *   (a cube map is an image with
//                               six array layers and a CUBE view, not a
//                               separate interface)
//   LPD3DXMATRIX / D3DXMATRIX-> FMATRIX4 *
//   D3DXVECTOR3/4, D3DXCOLOR -> FVECTOR3 / FVECTOR4
//   D3D9MatExt, D3D9Client   -> VulkanMatExt, VulkanClient
//   D3D9SM_*                 -> VULKANSM_*
// ===========================================================================================

#ifndef __VULKANEFFECT_H
#define __VULKANEFFECT_H

#define VULKANSM_SPHERE	0x01
#define VULKANSM_ARROW	0x02

#include "VulkanClient.h"
#include "VulkanFrame.h"
#include <vulkan/vulkan.h>
#include "VulkanTypes.h"
#include <deque>	// the per-frame resource rotation; see VulkanEffectFile::ResetFrame()
#include <map>
#include <set>		// the VulkanEffectFile instance registry; see ResetFrameAll()
#include <string>
#include <vector>

// NOTE: a "bool" in HLSL is 32bits (i.e. int)
// Must match with counterpart in the GLSL translation of D3D9Client.fx
//
// This carries over unchanged, and for the same reason: a bool inside a GLSL
// uniform block is also 32 bits, so the struct is laid out identically on
// both sides. It is uploaded raw, so -- like LightStruct and VulkanMatExt --
// the GLSL block that reads it must be layout(scalar).
struct TexFlow {
	BOOL Emis;		// Enable Emission Maps
	BOOL Spec;		// Enable Specular Maps
	BOOL Refl;		// Enable Reflection Maps
	BOOL Transl;	// Enable translucent effect
	BOOL Transm;	// Enable transmissive effect
	BOOL Rghn;		// Enable roughness map
	BOOL Norm;		// Enable normal map
	BOOL Metl;		// Enable metalness map
	BOOL Heat;		// Enable heat map
};

using namespace oapi;


// -------------------------------------------------------------------------------------------
// A technique handle. Counterpart of the D3DXHANDLE values that name
// techniques rather than parameters -- see the file header for why they are
// no longer the same type.
//
// A technique is a NAMED SET OF PIPELINES rather than one pipeline, because
// the pipeline also depends on the vertex layout and the blend/depth mode the
// caller wants, exactly as in ShaderClass::Setup. So the handle names the
// shader pair and the pass state from the .fx; the rest of the key arrives at
// bind time.
// -------------------------------------------------------------------------------------------
class VulkanTechnique;
typedef VulkanTechnique *TECHHANDLE;


// -------------------------------------------------------------------------------------------
// VulkanEffectFile -- what FX points at.
//
// Replaces ID3DXEffect. The call shape is kept because ~700 call sites across
// the client are written in it and every one of them is doing something that
// still has to happen; what is underneath is entirely different.
//
//   SetTechnique   selects which pipeline set the next draws use.
//   SetMatrix /
//   SetValue /
//   SetVector /
//   SetFloat / SetBool  write into the uniform block staged for this frame.
//   SetTexture     records a descriptor binding.
//   Begin / BeginPass / EndPass / End  bound the recording. D3DX used them to
//                  apply the pass's render state; here BeginPass is where the
//                  pipeline is actually bound and the descriptor set written,
//                  because that is the point at which every input is known.
//
// CommitChanges has no counterpart: D3DX needed it because parameters set
// after BeginPass would otherwise not reach the device. Here the uniform
// buffer and descriptor set are written at BeginPass, so there is nothing
// deferred to flush.
// -------------------------------------------------------------------------------------------
class VulkanEffectFile
{
public:
			VulkanEffectFile(VulkanDevice *pDev);
			~VulkanEffectFile();

	/// \brief Load an effect: its techniques and their shaders. Counterpart of
	///        D3DXCreateEffectFromFileA.
	bool			Load(const char *file);

	/// \brief Counterpart of ID3DXEffect::GetTechniqueByName.
	TECHHANDLE		GetTechniqueByName(const char *name);
	/// \brief Counterpart of ID3DXEffect::GetParameterByName.
	HANDLE			GetParameterByName(const char *name);

	void			SetTechnique(TECHHANDLE hTech);

	// ------------------------------------------------------------------
	// These two have no ID3DXEffect counterpart, and they are not additions
	// to the effect model -- they are two pieces of D3D9 DEVICE state that
	// Vulkan makes pipeline state, so they have to be known before the
	// pipeline is built rather than at the draw.
	//
	//   SetVertexDecl was `pDev->SetVertexDeclaration(pNTVertexDecl)`, which
	//   in every one of D3D9Effect.cpp's render helpers sits immediately
	//   before FX->Begin(). It sits in the same place here.
	//
	//   SetTopology was the D3DPRIMITIVETYPE argument to
	//   DrawIndexedPrimitiveUP -- given at the DRAW on Windows, where the
	//   pipeline did not exist. VkGraphicsPipelineCreateInfo takes the
	//   topology, so it has to be declared before BeginPass. It moves up to
	//   sit beside SetVertexDecl, which is the honest place for it.
	//
	// Together with the technique's own pass state they are the whole
	// pipeline key -- the same role Setup()'s three arguments play in
	// ShaderClass.
	// ------------------------------------------------------------------
	void			SetVertexDecl(const VertexDecl *pDecl);
	void			SetTopology(VkPrimitiveTopology topo);

	/// \brief Counterpart of DrawIndexedPrimitiveUP and DrawPrimitiveUP --
	///        "draw from this pointer in my memory".
	///
	///        Vulkan has no such call and cannot: a draw sources its vertices
	///        from a VkBuffer bound to the command buffer, so the caller's
	///        array has to reach one. This copies it into a per-frame scratch
	///        buffer and draws from that, which is what the D3D9 runtime did
	///        behind DrawPrimitiveUP anyway. pIdx may be NULL for the
	///        unindexed form.
	void			DrawUP(const void *pVtx, UINT nVtx, UINT stride,
						   const WORD *pIdx, UINT nIdx);

	bool			SetMatrix(HANDLE h, const FMATRIX4 *pM);
	bool			SetVector(HANDLE h, const FVECTOR4 *pV);
	bool			SetFloat(HANDLE h, float f);
	bool			SetBool(HANDLE h, bool b);
	bool			SetInt(HANDLE h, int i);
	bool			SetValue(HANDLE h, const void *pData, UINT bytes);
	bool			SetTexture(HANDLE h, VulkanTexture *pTex);

	/// \brief Counterparts of ID3DXEffect::GetValue / GetMatrix: read back a
	///        parameter that was written earlier.
	///
	///        D3DX held an effect's parameters in its own memory and would
	///        hand any of them back on request. So does this -- `Params` is a
	///        plain byte vector staged for the frame -- so the read is a
	///        memcpy out of it and NOT a query to the device, which could not
	///        answer one. Added because CelSphere.cpp's RenderGridLabels
	///        needs exactly this: it saves gWVP, draws the elevation labels
	///        with a rotated one, and puts the original back.
	bool			GetValue(HANDLE h, void *pData, UINT bytes) const;
	bool			GetMatrix(HANDLE h, FMATRIX4 *pM) const;
	bool			GetFloat(HANDLE h, float *pF) const;

	/// \brief Bound a technique. 'passes' returns the pass count, as D3DX did.
	bool			Begin(UINT *passes, DWORD flags = 0);
	bool			BeginPass(UINT pass);

	// -------------------------------------------------------------------
	// THE RENDER STATES CALL SITES SET *AFTER* BeginPass ON WINDOWS.
	//
	// D3DX applied the pass's state block at BeginPass, and a caller was then
	// free to override any of it with SetRenderState before the draw --
	// which two of them do:
	//
	//   D3D9Effect::Render2DPanel   D3DRS_DESTBLEND = additive ? ONE : INVSRCALPHA
	//   D3D9Pad::Flush              D3DRS_COLORWRITEENABLE, D3DRS_ALPHABLENDENABLE,
	//                               D3DRS_ZENABLE, D3DRS_ZWRITEENABLE,
	//                               D3DRS_CULLMODE, D3DRS_SCISSORTESTENABLE,
	//                               D3DSAMP_MIN/MAGFILTER, D3DSAMP_MAXANISOTROPY
	//
	// There is no "after" here: a VkPipeline is immutable once bound. So the
	// overrides have to arrive BEFORE the bind, and every one that is baked
	// into the pipeline joins its cache key. The two that Vulkan keeps
	// dynamic -- viewport and scissor -- do not, and are set on the command
	// buffer instead.
	//
	// -1 means "use the pass's own value" for every field, so a zeroed-out
	// struct is not accidentally a request to disable everything.
	// -------------------------------------------------------------------
	struct PassOverride {
		/// \brief The three values D3DRS_CULLMODE can take, plus "the pass's
		///        own".
		///
		///        THIS REPLACED A BOOLEAN `cullNone`, and the reason is
		///        HazeMgr.cpp: it draws the inner haze ring with
		///        D3DCULL_**CW** and puts D3DCULL_CCW back afterwards, which
		///        a two-state field cannot say. Note that CW and CCW do NOT
		///        differ in Vulkan's cullMode -- both are
		///        VK_CULL_MODE_BACK_BIT -- they differ in frontFace, because
		///        "cull the clockwise ones" and "cull the counter-clockwise
		///        ones" are the same cull with opposite ideas of which
		///        winding faces front. GetPipeline does that translation with
		///        the same table the .tech parser's ParseCullMode uses, so
		///        the two cannot drift.
		///
		///        Zero is a REAL VALUE here (D3DCULL_NONE), which is why the
		///        "unset" sentinel is CULL_PASS and not 0.
		enum CullMode { CULL_PASS = -1, CULL_NONE = 0, CULL_CW = 1, CULL_CCW = 2 };

		int  dstBlend;			///< a VkBlendFactor, or -1
		int  blendEnable;		///< D3DRS_ALPHABLENDENABLE, or -1
		int  colorWriteMask;	///< D3DRS_COLORWRITEENABLE (RGBA bits), or -1
		int  depthTest;			///< D3DRS_ZENABLE, or -1
		int  depthWrite;		///< D3DRS_ZWRITEENABLE, or -1
		int  cullMode;			///< D3DRS_CULLMODE, as a CullMode above
		int  filter;			///< 0 point, 1 linear, 2 anisotropic, or -1
		const RECT *scissor;	///< NULL for the whole frame. Dynamic, not in the key.

		PassOverride() : dstBlend(-1), blendEnable(-1), colorWriteMask(-1),
						 depthTest(-1), depthWrite(-1), cullMode(CULL_PASS),
						 filter(-1), scissor(NULL) {}
	};

	bool			BeginPassEx(UINT pass, const PassOverride *ovr);

	bool			EndPass();
	bool			End();

	VulkanDevice *	GetDevice() const { return pDev; }

	/// \brief Release the frame's descriptor sets and uniform slices.
	///
	///        NO D3DX COUNTERPART, and it is not bookkeeping this conversion
	///        invented -- it is the bookkeeping D3D9 did for you. Every
	///        FX->SetTexture and FX->SetMatrix on Windows was a call into a
	///        runtime that owned the constant registers and the texture
	///        stages and reused them freely, because a D3D9 draw consumed its
	///        state immediately. A Vulkan draw only RECORDS a reference: the
	///        descriptor set and the uniform bytes a command buffer names must
	///        stay untouched until that command buffer has finished
	///        executing. So each pass gets its own slice out of a per-frame
	///        arena, and the arena is recycled once -- here -- at the frame
	///        boundary, where the previous frame is known to be done.
	void			ResetFrame();

	/// \brief ResetFrame for EVERY live effect file, not just one.
	///
	///        THERE IS NOT ONE EFFECT FILE, THERE ARE FOUR, and each has its
	///        own arena and its own descriptor pool:
	///
	///            VulkanEffect::FX             VulkanEffect.cpp
	///            Scene::FX                    Scene.cpp
	///            VulkanPad::FX                VulkanPad.cpp
	///            VulkanCelestialSphere::s_FX  CelSphere.cpp
	///
	///        matching the three D3DXCreateEffectFromFile calls in the
	///        reference plus the celestial sphere's own. The per-frame reset
	///        was driven as `VulkanEffect::FX->ResetFrame()`, so the other
	///        three arenas only ever filled: each refused every pass after
	///        its first ~4096 with
	///
	///            VulkanEffectFile: the frame's uniform arena is full
	///            (786432 bytes, 4096 passes). ResetFrame is not being called.
	///
	///        and BeginPass returns false there, so those draws were silently
	///        dropped for the rest of the session.
	///
	///        D3DX had no such failure mode -- it owned the constant registers
	///        and recycled them itself -- so this reset is a Vulkan-only
	///        obligation, and an obligation that is per instance has to be
	///        discharged per instance. Same registry shape ShaderClass::
	///        ResetFrame already walks, for the same reason.
	static void		ResetFrameAll();

private:
	/// \brief Every live effect file, so the static ResetFrameAll can walk
	///        them. Maintained by the constructor and the destructor.
	static std::set<VulkanEffectFile*>	Instances;

	VulkanDevice *						pDev;
	std::map<std::string, VulkanTechnique*>	Techniques;
	VulkanTechnique *					pCurrent;
	std::string							fname;

	// The effect's parameter block, and it is ONE block for the whole effect
	// because that is what an .fx file already is: `uniform extern float4x4
	// gVP;` and its ~90 siblings are declared at file scope and shared by
	// every technique. D3DX kept one set of parameter values across
	// SetTechnique for exactly that reason, and the client depends on it --
	// UpdateEffectCamera sets eEast/eNorth/eCameraPos once per frame and
	// every technique drawn afterwards reads them.
	//
	// So the values live here as a CPU-side image of the block, written by
	// the Set* calls and uploaded at BeginPass. That is also what makes
	// GetParameterByName effect-wide rather than per-technique.
	std::vector<unsigned char>			Params;
	std::map<std::string, ShaderReflection::Var> ParamMap;

	// Texture parameters are descriptors, not bytes, so they are held apart
	// from the block. Indexed by the Var's samplerIndex.
	VulkanTexture *						Textures[32];

	// Set before Begin(); see SetVertexDecl / SetTopology.
	const VertexDecl *					pDecl;
	VkPrimitiveTopology					topology;

	// The scratch DrawUP copies into. Grown on demand, never shrunk: the
	// largest user-memory draw in the client is Render2DPanel's mesh group.
	VulkanBuffer *						pScratchVB;
	VulkanBuffer *						pScratchIB;
	VkDeviceSize						scratchVBSize, scratchIBSize;

	// How much of each scratch buffer this frame has handed out. DrawUP
	// sub-allocates from the front and binds at the offset, so two DrawUP
	// calls in one frame get their own bytes -- which is what D3D9's runtime
	// gave every DrawPrimitiveUP. Reset by ResetFrame().
	VkDeviceSize						scratchVBUsed, scratchIBUsed;

	// Scratch buffers replaced by a larger one DURING a frame. They cannot be
	// destroyed at that moment: draws already recorded in this frame's command
	// buffer still name them, and freeing a bound buffer is what took the
	// device down (VK_ERROR_DEVICE_LOST at vkQueueSubmit).
	//
	// NOR AT THE NEXT FRAME BOUNDARY, which is what this used to do and what
	// the validation layer reported as VUID-vkDestroyBuffer-buffer-00922 --
	// the frame that recorded the bind is still in flight one frame later.
	// Each is tagged with the frame that retired it and freed once
	// orbiter_GetFramesInFlight() frames have gone by, the same lag the pool
	// rotation uses.
	std::deque<std::pair<VulkanBuffer *, unsigned long long> > retiredScratch;

	UINT								curPass;

	// ONE descriptor set layout and ONE pipeline layout for the whole effect,
	// not one per technique. That follows from the parameter block being
	// effect-wide: every technique reads the same uniform block at the same
	// offsets and the same texture bindings, so every pipeline built here has
	// the same interface. D3DX expressed the same fact by letting any
	// parameter be set at any time regardless of the selected technique.
	VkDescriptorSetLayout				vkSetLayout;
	VkPipelineLayout					vkPipeLayout;
	uint32_t							blockSize;		///< bytes of the parameter block
	uint32_t							nSamplers;		///< highest sampler binding + 1
	// Three samplers, not one: PassOverride::filter selects between them.
	// D3DSAMP_MIN/MAGFILTER was device state a call site could change per
	// draw; a VkSampler is an object, so "change the filter" means "use a
	// different one". [0] point, [1] linear, [2] anisotropic.
	VkSampler							vkSampler;		///< the default -- same as vkSamplers[1]
	VkSampler							vkSamplers[3];

	// ------------------------------------------------------------------
	// THE .fx SAMPLER_STATE BLOCKS.
	//
	// D3D9Client.fx declares twenty-four of them --
	//
	//     sampler ClampS = sampler_state {
	//         Texture = <gTex0>;
	//         MinFilter = ANISOTROPIC; MagFilter = LINEAR; MipFilter = LINEAR;
	//         MaxAnisotropy = ANISOTROPY_MACRO;
	//         AddressU = CLAMP; AddressV = CLAMP;
	//     };
	//
	// -- and eight of them read gTex0 with DIFFERENT state. That is the whole
	// reason they exist: WrapS and ClampS are the same texture sampled two
	// ways, and a planet tile drawn with WRAP instead of CLAMP wraps its edge
	// pixels around, which is visible as a seam on every tile.
	//
	// THE BLOCKS ARE NOT HLSL. `sampler_state` is D3DX EFFECT FRAMEWORK
	// syntax, exactly like `technique` and `pass`, and glslang understands
	// none of it -- so it goes where the techniques went, into the .tech
	// file, transcribed verbatim. That is the same split this port already
	// makes and for the same reason.
	//
	// In GLSL a sampler IS the combination, so the shader declares one
	// `sampler2D ClampS` per sampler_state and the binding carries the state.
	// The client still sets textures by TEXTURE name (FX->SetTexture(eTex0,
	// ...)), so the `Texture = <gTex0>` line is what joins the two: it says
	// which bindings a texture parameter feeds, and SetTexture writes all of
	// them. On Windows the D3DX runtime did that join.
	// ------------------------------------------------------------------
	struct SamplerState {
		std::string				texture;	///< the texture parameter it reads
		VkFilter				minFilter;
		VkFilter				magFilter;
		VkSamplerMipmapMode		mipMode;
		bool					bMip;		///< MipFilter = NONE clamps LOD to 0
		bool					bAniso;		///< MinFilter = ANISOTROPIC
		float					maxAniso;
		VkSamplerAddressMode	addrU, addrV, addrW;
		float					lodBias;
		SamplerState() : minFilter(VK_FILTER_LINEAR), magFilter(VK_FILTER_LINEAR),
						 mipMode(VK_SAMPLER_MIPMAP_MODE_LINEAR), bMip(true),
						 bAniso(false), maxAniso(1.0f),
						 addrU(VK_SAMPLER_ADDRESS_MODE_REPEAT),
						 addrV(VK_SAMPLER_ADDRESS_MODE_REPEAT),
						 addrW(VK_SAMPLER_ADDRESS_MODE_REPEAT),
						 lodBias(0.0f) {}
	};

	/// \brief By sampler name, as the .tech file and the GLSL both spell it.
	std::map<std::string, SamplerState>	SamplerStates;

	/// \brief Texture parameter name -> every binding whose sampler_state
	///        names it. What FX->SetTexture(eTex0, ...) has to write.
	std::map<std::string, std::vector<uint32_t> > TexBindings;

	/// \brief One VkSampler per binding, built from that binding's
	///        sampler_state. NULL where the .tech declares none, in which
	///        case vkSampler is used -- the behaviour before the blocks
	///        existed.
	VkSampler							vkBindSampler[32];

	// A 1x1 opaque white image, for a sampler binding the current technique
	// does not use.
	//
	// NO D3D9 COUNTERPART BECAUSE D3D9 NEEDED NONE: an unset texture stage
	// sampled white and that was the end of it. Vulkan has no such default --
	// a descriptor left unwritten and then read is undefined behaviour, and
	// the validation layer reports it -- so the default has to be a real
	// image. Sampling it gives white, which is what the reference produced.
	VulkanTexture *						pWhite;
	/// The cube-shaped counterpart of pWhite, for an unset samplerCube slot.
	/// A cube declaration sampled through a 2D view is a GPU fault, not a
	/// wrong colour, so the fallback has to match the declaration's dim.
	/// Built only when a technique actually declares one. See bCubeBind.
	VulkanTexture *						pWhiteCube;
	/// Which sampler bindings are cube-dimensioned, from reflection.
	bool								bCubeBind[32];

	// The per-frame arena. See ResetFrame().
	VkDescriptorPool					vkPool;
	VulkanBuffer *						pUniformArena;
	VkDeviceSize						arenaSize, arenaUsed;
	VkDescriptorSet						curSet;
	VkDeviceSize						curUniformOffset;

	// ------------------------------------------------------------------
	// THE ROTATION, and why the four members above are not simply reset.
	//
	// ResetFrame used to call vkResetDescriptorPool on vkPool and rewind
	// arenaUsed at the top of every frame, on the stated assumption that
	// "the previous frame's sets are certainly done with" by then. They are
	// not. The core keeps orbiter_GetFramesInFlight() frames in flight and
	// its vkWaitForFences is on the fence of the SLOT it is about to record
	// into -- which says nothing about the other slots, still executing and
	// still naming these sets and these bytes. The validation layer says so
	// plainly:
	//
	//     VUID-vkResetDescriptorPool-descriptorPool-00313
	//     vkResetDescriptorPool(): descriptorPool can't be called on
	//     VkDescriptorPool 0x14c... that is currently in use by
	//     VkCommandBuffer 0x6349...
	//     VUID-vkDestroyBuffer-buffer-00922
	//     vkDestroyBuffer(): can't be called on VkBuffer 0x334c... that is
	//     currently in use by VkDescriptorSet 0xf1e4...
	//
	// and the driver answered it with intermittent VK_ERROR_DEVICE_LOST.
	//
	// So the resources are ROTATED rather than reset: what a frame used is
	// pushed onto `retired` tagged with that frame's number, and a set is
	// taken back only once orbiter_GetFramesInFlight() frames have passed,
	// at which point the core has fenced the slot that was using it. Every
	// call site above keeps using vkPool / pUniformArena / pScratchVB
	// unchanged -- only what they point at changes, once per frame.
	//
	// This is the Vulkan shape of a guarantee D3D9 gave for free; there is
	// no line in the reference to convert here, only an obligation to meet.
	struct FrameSet {
		VkDescriptorPool	pool;
		VulkanBuffer *		arena;
		VkDeviceSize		arenaSize;
		VulkanBuffer *		scratchVB;
		VkDeviceSize		scratchVBSize;
		VulkanBuffer *		scratchIB;
		VkDeviceSize		scratchIBSize;
		unsigned long long	retiredAt;
		FrameSet() : pool(VK_NULL_HANDLE), arena(NULL), arenaSize(0),
					 scratchVB(NULL), scratchVBSize(0),
					 scratchIB(NULL), scratchIBSize(0), retiredAt(0) {}
	};
	std::deque<FrameSet>				retired;	///< oldest first
	unsigned long long					frameNo;

	/// \brief Build a fresh pool and arena into vkPool/pUniformArena. Used
	///        once by CreateResources and again by ResetFrame when the
	///        rotation has not yet come round.
	bool			CreateFrameSet();

	bool			EnsureScratch(VkDeviceSize vbytes, VkDeviceSize ibytes);

	/// \brief Merge every pass's reflection into the one effect-wide
	///        parameter table, and build the layouts from it. Called at the
	///        end of Load(), when every shader has been compiled.
	bool			BuildParameterTable();

	/// \brief Fetch or build the pipeline for (pass, declaration, topology,
	///        overrides).
	VkPipeline		GetPipeline(struct VulkanPass *pPass, const PassOverride *ovr);
};


class VulkanEffect {

	DWORD vulkanid;

public:
	static void VulkanTechInit(VulkanClient *gc, VulkanDevice *pDev, const char *folder);

	/**
	 * \brief Release global parameters
	 */
	static void GlobalExit();

	static void ShutDown();

	VulkanEffect();
	~VulkanEffect();

	static void EnablePlanetGlow(bool bEnabled);
	static void UpdateEffectCamera(OBJHANDLE hPlanet);
	static void InitLegacyAtmosphere(OBJHANDLE hPlanet, float GlobalAmbient);
	static void SetViewProjMatrix(FMATRIX4 *pVP);

	static void RenderLines(const FVECTOR3 *pVtx, const WORD *pIdx, int nVtx, int nIdx, const FMATRIX4 *pW, DWORD color);
	static void RenderTileBoundingBox(const FMATRIX4 *pW, VECTOR4 *pVtx, const FVECTOR4 *color);
	static void RenderBoundingBox(const FMATRIX4 *pW, const FMATRIX4 *pGT, const FVECTOR4 *bmin, const FVECTOR4 *bmax, const FVECTOR4 *color);
	static void RenderBoundingSphere(const FMATRIX4 *pW, const FMATRIX4 *pGT, const FVECTOR4 *bs, const FVECTOR4 *color);
	static void RenderBillboard(const FMATRIX4 *pW, VulkanTexture *pTex, float alpha = 1.0f);
	static void RenderExhaust(const FMATRIX4 *pW, VECTOR3 &cdir, EXHAUSTSPEC *es, SURFHANDLE def);
	static void RenderSpot(float intens, const FVECTOR4 *color, const FMATRIX4 *pW, SURFHANDLE pTex);
	static void Render2DPanel(const MESHGROUP *mg, const SURFHANDLE pTex, const FMATRIX4 *pW, float alpha, float scale, bool additive);
	static void RenderReEntry(const SURFHANDLE pTex, const FVECTOR3 *vPosA, const FVECTOR3 *vPosB, const FVECTOR3 *vDir, float alpha_a, float alpha_b, float size);
	static void RenderArrow(OBJHANDLE hObj, const VECTOR3 *ofs, const VECTOR3 *dir, const VECTOR3 *rot, float size, const FVECTOR4 *pColor);

	static VulkanDevice *pDev;			///< Static (global) render device
	static VulkanBuffer *VB;			///< Static (global) Vertex buffer pointer

	static FVECTOR4 atm_color;			///< Earth glow color

	// Rendering Technique related parameters
	static VulkanEffectFile	*FX;
	static VulkanClient		*gc;		///< The graphics client instance

	static VulkanMatExt	mfdmat;
	static VulkanMatExt	defmat;
	static VulkanMatExt	night_mat;
	static VulkanMatExt	emissive_mat;
	
	// Techniques ----------------------------------------------------
	// TECHHANDLE, not HANDLE: these select a pipeline. See the file header.
	static TECHHANDLE	eVesselTech;     ///< Vessel exterior, surface bases
	static TECHHANDLE	eSimple;
	static TECHHANDLE	eBBTech;         ///< Bounding Box Tech
	static TECHHANDLE	eTBBTech;        ///< Bounding Box Tech
	static TECHHANDLE	eBSTech;         ///< Bounding Sphere Tech
	static TECHHANDLE   eExhaust;        ///< Render engine exhaust texture
	static TECHHANDLE   eSpotTech;       ///< Vessel beacons
	static TECHHANDLE   ePanelTech;      ///< Used to draw a new style 2D panel
	static TECHHANDLE   ePanelTechB;     ///< Used to draw a new style 2D panel
	static TECHHANDLE	eBaseTile;
	static TECHHANDLE	eRingTech;       ///< Planet rings technique
	static TECHHANDLE	eRingTech2;      ///< Planet rings technique
	static TECHHANDLE	eShadowTech;     ///< Vessel ground shadows
	static TECHHANDLE	eGeometry;
	static TECHHANDLE	eBaseShadowTech; ///< Used to draw transparent surface without texture
	static TECHHANDLE	eBeaconArrayTech;
	static TECHHANDLE	eArrowTech;      ///< (Grapple point) arrows
	static TECHHANDLE	eAxisTech;
	static TECHHANDLE	ePlanetTile;
	static TECHHANDLE	eCloudTech;
	static TECHHANDLE	eCloudShadow;
	static TECHHANDLE	eSkyDomeTech;
	static TECHHANDLE	eDiffuseTech;
	static TECHHANDLE	eEmissiveTech;
	static TECHHANDLE	eHazeTech;
	static TECHHANDLE	eSimpMesh;

	// Transformation Matrices ----------------------------------------
	static HANDLE	eVP;         ///< Combined View & Projection Matrix
	static HANDLE	eW;          ///< World Matrix
	static HANDLE	eLVP;        ///< Light view projection
	static HANDLE	eGT;         ///< MeshGroup transformation matrix

	// Lighting related parameters ------------------------------------
	static HANDLE   eMtrl;
	static HANDLE   eTune;
	static HANDLE	eMat;        ///< Material
	static HANDLE	eWater;      ///< Water
	static HANDLE	eSun;        ///< Sun
	static HANDLE	eLights;     ///< Additional light sources
	static HANDLE	eKernel;
	static HANDLE	eAtmoParams;

	// Auxiliary params ----------------------------------------------
	static HANDLE   eModAlpha;     ///< BOOL multiply material alpha with texture alpha
	static HANDLE	eFullyLit;     ///< BOOL
	static HANDLE	eFlow;		   ///< BOOL
	static HANDLE	eShadowToggle; ///< BOOL
	static HANDLE	eEnvMapEnable; ///< BOOL
	static HANDLE	eInSpace;      ///< BOOL
	static HANDLE	eNoColor;      ///< BOOL
	static HANDLE	eLightsEnabled;///< BOOL
	static HANDLE	eBaseBuilding; ///< BOOL
	static HANDLE	eTuneEnabled;  ///< BOOL
	static HANDLE	eFresnel;	   ///< BOOL
	static HANDLE   eSwitch;	   ///< BOOL
	static HANDLE   eRghnSw;	   ///< BOOL
	static HANDLE	eTextured;	   ///< BOOL
	static HANDLE	eOITEnable;	   ///< BOOL
	static HANDLE	eInvProxySize;
	static HANDLE	eMix;          ///< FLOAT Auxiliary factor/multiplier
	static HANDLE   eColor;        ///< Auxiliary color input
	static HANDLE   eFogColor;     ///< Fog color input
	static HANDLE   eTexOff;       ///< Surface tile texture offsets
	static HANDLE	eSpecularMode;
	static HANDLE	eHazeMode;
	static HANDLE   eTime;         ///< FLOAT Simulation elapsed time
	static HANDLE	eExposure;
	static HANDLE	eCameraPos;	
	static HANDLE   eNorth;
	static HANDLE	eEast;
	static HANDLE   eDistScale;
	static HANDLE   eGlowConst;
	static HANDLE   eRadius;
	static HANDLE	eFogDensity;
	static HANDLE	ePointScale;
	static HANDLE	eAtmColor;
	static HANDLE	eProxySize;
	static HANDLE	eMtrlAlpha;
	static HANDLE	eAttennuate;
	static HANDLE	eInScatter;
	static HANDLE	eSHD;
	static HANDLE	eNight;

	// Textures --------------------------------------------------------
	static HANDLE	eTex0;    ///< Primary texture
	static HANDLE	eTex1;    ///< Secondary texture
	static HANDLE	eTex3;    ///< Tertiary texture
	static HANDLE	eSpecMap;
	static HANDLE	eEmisMap;
	static HANDLE	eEnvMapA;
	static HANDLE	eEnvMapB;
	static HANDLE	eReflMap;
	static HANDLE	eMetlMap;
	static HANDLE	eHeatMap;
	static HANDLE	eRghnMap;
	static HANDLE	eTranslMap;
	static HANDLE	eTransmMap;
	static HANDLE	eShadowMap;
	static HANDLE	eIrradMap;

	// Legacy Atmosphere -----------------------------------------------
	static HANDLE	eGlobalAmb;	 
	static HANDLE	eSunAppRad;	 
	static HANDLE	eAmbient0;	 
	static HANDLE	eDispersion;	  
};

#endif // !__VULKANEFFECT_H
