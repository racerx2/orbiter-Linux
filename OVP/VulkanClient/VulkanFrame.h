// ==============================================================
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2006-2026 Martin Schweiger
//				 2012-2016 Jarmo Nikkanen
// ==============================================================

// ============================================================================
// File: VulkanFrame.h
// Desc: Class to manage the Vulkan environment objects
//
// CONVERTED FROM OVP/D3D9Client/D3D9Frame.h, read end to end (146 lines),
// against D3D9Frame.cpp read end to end (641 lines).
//
// THE ONE FINDING THAT SHAPES THIS WHOLE FILE: THE CLIENT DOES NOT CREATE A
// DEVICE.
//
// CD3DFramework9::Initialize() called g_pD3DObject->CreateDevice() and owned
// the result. That cannot happen here, and not as a matter of taste --
// Src/Orbiter/Linux/UIHost.cpp already stands up a complete Vulkan device for
// the Launchpad before any client is loaded: instance, physical device,
// logical device, graphics queue, descriptor pool, surface, swapchain, the
// colour+depth render pass, and the ImGui backend bound to all of it. Its own
// comment states the constraint: "A graphics client must use THAT device
// rather than create its own -- two devices cannot present to one surface,
// and the client has to render into the same swapchain images that ImGui
// draws its dialogs over."
//
// So Initialize() ADOPTS rather than creates, through the interface the core
// publishes for exactly this purpose:
//
//     orbiter_GetVulkanContext()        the handles, once initialise() has run
//     orbiter_SetSceneRenderCallback()  where the scene gets recorded
//
// Line for line, the Windows Initialize() maps onto that as:
//
//   Direct3DCreate9 / g_pD3DObject       -> context.instance, already made
//   GetAdapterIdentifier                 -> vkGetPhysicalDeviceProperties
//   GetDeviceCaps                        -> VkPhysicalDeviceProperties::limits
//   CheckDeviceFormat                    -> vkGetPhysicalDeviceFormatProperties
//   CheckDeviceMultiSampleType           -> limits.framebufferColorSampleCounts
//   EnumAdapterModes / GetSystemMetrics  -> orbiter_GetVideoMode*
//   SetWindowLongA / SetWindowPos        -> orbiter_SetDisplayMode / SetWindowSize
//   CreateDevice(&d3dPP, &pDevice)       -> orbiter_GetVulkanContext
//   GetRenderTarget / GetDepthStencil    -> the swapchain image is the core's;
//                                           see the note on pBackBuffer below
//   CreateVertexDeclaration x11          -> nothing. A Vulkan vertex layout is
//                                           data baked into a pipeline, not a
//                                           device object. The tables live in
//                                           VulkanUtil.h/.cpp.
//   D3DXCreateFontIndirect x2            -> see the note on the fonts below
//
// D3DPRESENT_PARAMETERS has no counterpart at all. Every field in it --
// back buffer size and format, swap effect, depth-stencil format, vsync
// interval, multisampling -- is a property of the swapchain, and the
// swapchain belongs to the core. What the client used to set through d3dPP it
// now asks for through orbiter_SetVSync / orbiter_SetDisplayMode, and reads
// back through orbiter_GetSurfaceExtent.
//
// D3DFWERR_* are gone with it: they name DirectDraw and D3D setup failures
// that no longer occur here. Initialize() returns S_OK or E_FAIL through the
// shim's HRESULT, which is what its callers already test.
// ============================================================================

#ifndef VULKANFRAME_H
#define VULKANFRAME_H

#include <vulkan/vulkan.h>
#include "Orbitersdk.h"
#include "GraphicsAPI.h"
#include "VulkanTypes.h"
// ShaderReflection holds std::string and std::vector. Both arrive
// transitively through Orbitersdk.h, but this file is the one that uses them.
// <map> is the render-pass and framebuffer caches; see BeginOffscreen.
#include <string>
#include <vector>
#include <map>

// D3D9Frame.h included "D3D9Client.h" to reach oapi::GraphicsClient::VIDEODATA
// in Initialize(). That is a cycle -- D3D9Client.h includes D3D9Frame.h back --
// which MSVC tolerates through the include guards. GraphicsAPI.h is where
// VIDEODATA actually lives and is all this header needs, so it is included
// directly and the cycle does not exist here.

class SurfNative;


// ---------------------------------------------------------------------------
// The interface Src/Orbiter/Linux/UIHost.cpp publishes to a graphics client.
//
// Declared here rather than included, because UIHost.cpp is a core
// translation unit with no header of its own and these are extern "C" by
// design. The struct must stay identical to the one at UIHost.cpp:57 --
// it is passed by address across that boundary.
// ---------------------------------------------------------------------------

extern "C" {

struct OrbiterVulkanContext {
	void     *instance;			// VkInstance
	void     *physicalDevice;	// VkPhysicalDevice
	void     *device;			// VkDevice
	void     *queue;			// VkQueue
	unsigned  queueFamily;
	void     *descriptorPool;	// VkDescriptorPool
	void     *renderPass;		// VkRenderPass of the main swapchain
	void     *window;			// GLFWwindow*
	unsigned  minImageCount;
	unsigned  imageCount;
};

int  orbiter_GetVulkanContext(OrbiterVulkanContext *out);

// The scene hook. renderFrame() owns the only command buffer that reaches the
// swapchain and the only vkQueuePresentKHR, so the client cannot present and
// cannot simply ask for a command buffer -- clbkRenderScene runs at a
// different time from the frame pump. It registers this instead and is called
// INSIDE the open render pass, BEFORE ImGui's draws, so the scene lands under
// the dialogs in the same swapchain image and they present together.
typedef void (*OrbiterSceneRenderFn)(void *cmdBuf, void *renderPass,
									 unsigned width, unsigned height,
									 void *user);
void orbiter_SetSceneRenderCallback(OrbiterSceneRenderFn fn, void *user);

void orbiter_GetSurfaceExtent(int *w, int *h);
void orbiter_GetFramebufferSize(int *w, int *h);
int  orbiter_GetVideoModeCount(void);
int  orbiter_GetVideoMode(int idx, int *w, int *h, int *hz);
int  orbiter_GetCurrentVideoMode(int *w, int *h, int *hz);
void orbiter_SetVSync(int enable);
void orbiter_SetPreferredGpuIndex(int idx);
void orbiter_SetOutputIndex(int idx);
void orbiter_SetWindowSize(int w, int h);

// The counterpart of the whole fullscreen/windowed switch at the head of
// CD3DFramework9::Initialize -- EnumAdapterModes, GetSystemMetrics(SM_CX*),
// SystemParametersInfo(SPI_GETWORKAREA), SetWindowLongA and SetWindowPos.
// Every one of those acts on a window this client does not own, so the whole
// switch becomes one call to the file that does own it. The parameters carry
// the reference's own arguments unchanged:
//
//   fullscreen   vData->fullscreen
//   style        vData->style      (0 true fullscreen, 1 fullscreen window,
//                                   2 fullscreen window with taskbar)
//   spanDisplays vData->pageflip   -- SM_CXVIRTUALSCREEN rather than SM_CXSCREEN
//   forceSize    vData->trystencil -- the SWP_NOSENDCHANGING on SetWindowPos
//   w, h, hz     the chosen mode
void orbiter_SetDisplayMode(int fullscreen, int style,
							int spanDisplays, int forceSize,
							int w, int h, int hz);

// Reset every frame's command buffer, so none still refers to a descriptor
// set or an image the client is about to free. Called from DestroyObjects()
// where the Windows file called Reset(&d3dPP) -- both exist for the same
// reason, to make the runtime let go of resources before they are released.
void orbiter_ResetFrameCommands(void);

// ---------------------------------------------------------------------------
// THE SESSION AND FRAME HOOKS. WITHOUT THESE NOTHING IS EVER DRAWN, which is
// exactly what happened: the Launchpad appeared, a scenario launched, and the
// window vanished with no renderer and no error.
//
// The core draws the scene only while g_sessionActive is true --
//
//     if (g_sceneRenderFn && g_sessionActive) g_sceneRenderFn(...)
//
// -- and hides the host window entirely when nothing wants to be drawn in it,
// on the same flag. Registering the scene callback is therefore only half the
// handshake: the client must also say when a session and a frame begin and
// end. This client registered the callback and said none of the rest, so
// g_sessionActive stayed false for the whole session.
//
// Each one is the counterpart of a line the reference already has:
//
//   orbiter_BeginSession()   D3D9Client::clbkPostCreation's `bRunning = true`
//   orbiter_EndSession()     clbkCloseSession's teardown of the same state
//   orbiter_BeginSceneFrame  BeginScene() at the top of clbkRenderScene
//   orbiter_EndSceneFrame    Present() -- it pumps and presents the frame
//   orbiter_ClearSplash      releasing pSplashScreen once loading is over
//
// EndSceneFrame is the one that actually puts a picture on screen: it calls
// orbiter_PumpFrame, which acquires a swapchain image, opens the render pass,
// calls the scene callback registered above, composites the dialogs and
// presents. It is the only present in the process -- see PresentScene in
// VulkanClient.cpp for why the client has none of its own.
void orbiter_BeginSession(void);
void orbiter_EndSession(void);
void orbiter_BeginSceneFrame(void);
void orbiter_EndSceneFrame(void);
void orbiter_ClearSplash(void);

// HOW MANY FRAMES THE CORE KEEPS IN FLIGHT, and therefore how long any
// per-frame resource this client hands to a draw must be left alone.
//
// NO WINDOWS COUNTERPART. The D3D9 runtime owned the constant registers and
// the texture bindings a draw referred to and kept them alive for its own
// queued frames; the client could overwrite a constant immediately after a
// DrawPrimitive. In Vulkan a draw only RECORDS a reference, so a descriptor
// pool or uniform arena reused too early is a use-after-free -- and the
// answer to that is VK_ERROR_DEVICE_LOST, at a random later moment.
//
// See ShaderClass::ResetFrame and VulkanEffectFile::ResetFrame, which rotate
// their pools on exactly this lag.
unsigned orbiter_GetFramesInFlight(void);

// THE DEVICE LOCK -- what D3DCREATE_MULTITHREADED provided on Windows.
//
// D3D9Client.cpp asks for the flag and the D3D9 runtime then serialises every
// device call, which is why the reference can upload a tile texture from
// TileLoader::Load_ThreadProc while the render thread draws. Vulkan has no
// such flag: VkQueue and VkCommandPool are externally synchronised and the
// application must do it. The queue belongs to the core (VulkanDevice::Adopt
// takes it from the published context) and both sides submit on it, so the
// lock lives there and is reached through these two calls.
//
// BeginOneShot takes it and EndOneShot releases it -- see the note on those.
void orbiter_LockDevice(void);
void orbiter_UnlockDevice(void);

// THE BACK BUFFER, READ BACK -- clbkSaveSurfaceToImage(NULL, ...).
//
// The reference reaches its back buffer with pDevice->GetRenderTarget(0),
// because the D3D9 client created the device and owns the swap chain. This
// client owns neither: the swapchain images are the core's, and
// CVulkanFramework's back-buffer SurfNative is an attachment proxy holding no
// VkImage at all, so ReadTexture on it can only fail -- which is exactly what
// "clbkSaveSurfaceToImage: readback failed" was, once per session, with
// Images/CurrentState.jpg never written.
//
// So the operation is asked of the side that owns the image. It arms a copy,
// pumps one frame -- the client's own scene callback draws it, as in any
// other frame -- and hands back that frame's pixels: BGRA, four bytes per
// pixel, width*4 to the row, which is the layout ReadTexture would have
// produced. Returns 0 if nothing was captured. See UIHost.cpp's
// recordFrameCapture for why the copy has to be inside the frame.
int orbiter_CaptureBackBuffer(void *dst, unsigned bytes, int *w, int *h);

// GPU CRASH DIAGNOSTICS. No D3D9 counterpart: a lost D3D9 device was reported
// by Present with D3DERR_DEVICELOST and the runtime told you nothing about
// where, so the reference has nothing corresponding to this.
//
// VK_ERROR_DEVICE_LOST is reported by whichever call runs after the reset, not
// by the one that caused it, so on its own it names nothing. A checkpoint is a
// marker recorded into the command stream; after a loss the driver hands back
// which markers the GPU reached, and the offender lies between the last one
// that COMPLETED and the first that merely started.
//
// THE TAG MUST OUTLIVE THE SUBMISSION -- the driver stores the pointer, not
// the text, and hands it back verbatim. A string literal, or the c_str() of a
// std::string owned by something that lives at least as long as the frame.
//
// Both are no-ops unless ORBITER_VK_CHECKPOINTS is set, so they cost nothing
// in a normal run. The client records into the command buffer the core owns,
// so its draws and the core's ImGui pass share one stream and one queue.
void orbiter_VkCheckpoint(void *cmdBuf, const char *tag);
void orbiter_DumpGpuCheckpoints(const char *where);

} // extern "C"


//-----------------------------------------------------------------------------
// Name: VulkanTexture
// Desc: Counterpart of IDirect3DTexture9 AND IDirect3DSurface9 AND
//       IDirect3DVolumeTexture9 -- three D3D9 interfaces, one Vulkan type.
//
//       This is the bookkeeping VulkanTypes.h describes: D3D9 handed you one
//       object holding storage, a format, a mip chain and the state used to
//       sample it. Vulkan gives four unowned handles -- VkImage,
//       VkDeviceMemory, VkImageView, VkSampler -- and somebody has to hold
//       them together. It is NOT a D3D9 interface: no QueryInterface, no
//       AddRef/Release, no GetLevelDesc, no GetSurfaceLevel.
//
//       Layout tracking is the one piece with no D3D9 analogue at all. A
//       D3D9 resource is always usable; a VkImage is only valid for an
//       operation in the right VkImageLayout, and moving between layouts is
//       an explicit barrier. Recording the current layout here is what lets
//       a blit or a bind insert the barrier it needs without the caller
//       having to remember what happened to the image last.
//-----------------------------------------------------------------------------
class VulkanTexture
{
	friend class VulkanDevice;
public:
			VulkanTexture();
			~VulkanTexture();

	VkImage			Image() const		{ return vkImage; }
	VkImageView		View() const		{ return vkView; }

	/// \brief The view to use when this image is a FRAMEBUFFER ATTACHMENT.
	///
	/// A framebuffer attachment must name exactly one mip level
	/// (VUID-VkFramebufferCreateInfo-pAttachments-00883); View() covers the
	/// whole chain, which is what a sampler wants and what an attachment may
	/// not have. For a single-level image the two are the same view; for a
	/// mipped one this builds a level-0 view on first use and keeps it for the
	/// life of the texture. D3D9 needed no such distinction because
	/// GetSurfaceLevel(0) already produced a per-level surface.
	VkImageView		AttachmentView();
	VkDeviceMemory	Memory() const		{ return vkMemory; }

	const VulkanImageDesc &Desc() const	{ return desc; }
	uint32_t		Width() const		{ return desc.Width; }
	uint32_t		Height() const		{ return desc.Height; }
	uint32_t		Mips() const		{ return desc.Mips; }
	VkFormat		Format() const		{ return desc.Format; }

	/// \brief The layout the image is in right now. See the class note.
	VkImageLayout	Layout() const		{ return vkLayout; }
	void			SetLayout(VkImageLayout l)	{ vkLayout = l; }

	/// \brief Counterpart of LockRect/UnlockRect. Host-visible images only;
	///        returns NULL for device-local ones, which have to be staged.
	void *			Map();
	void			Unmap();
	size_t			RowPitch() const	{ return pitch; }

	bool			IsHostVisible() const { return desc.HostVisible; }

	/// \brief False for the two kinds of VulkanTexture that hold a VkImage
	///        they did not create: the mip views CreateMipView returns, which
	///        share their parent's image exactly as GetSurfaceLevel(n) shared
	///        its texture's, and the attachment proxies CreateAttachmentProxy
	///        returns, which hold no image at all. Both were reference-counted
	///        interfaces in D3D9 and needed no such flag; Vulkan destruction
	///        is unconditional, so the ownership has to be recorded.
	bool			OwnsImage() const	{ return bOwnsImage; }

	/// \brief True for a VulkanTexture that stands for one of the core's
	///        render-pass attachments rather than for an image of its own.
	///        See VulkanDevice::CreateAttachmentProxy.
	bool			IsProxy() const		{ return vkImage == VK_NULL_HANDLE && desc.Width != 0; }

	// The description records what was ASKED FOR, because a VkImage answers
	// no questions about itself. NatCreateSurface settles host visibility and
	// sample count after the image exists, so those two are settable.
	void			SetHostVisible(bool b)				{ desc.HostVisible = b; }
	void			SetSamples(VkSampleCountFlagBits s)	{ desc.Samples = s; }

	/// \brief Make this image SAMPLE as if it had no alpha channel -- the
	///        counterpart of D3DFMT_X8R8G8B8, which Vulkan has no format for.
	///        Rebuilds the sampling view with VK_COMPONENT_SWIZZLE_ONE on A.
	///        See the long note on the definition; it is what the virtual
	///        cockpit's HUD and MFD screens need to be visible at all.
	bool			SetAlphaOne();
	bool			IsAlphaOne() const	{ return bAlphaOne; }

private:
	VkDevice		vkOwner;		///< the device that made it, needed to destroy it
	VkImage			vkImage;
	VkDeviceMemory	vkMemory;
	VkImageView		vkView;
	VkImageView		vkAttachView;	///< lazy level-0 view; see AttachmentView()
	VkImageLayout	vkLayout;
	VulkanImageDesc	desc;
	void *			mapped;
	size_t			pitch;
	bool			bOwnsImage;		///< see OwnsImage()
	bool			bAlphaOne;		///< see SetAlphaOne()
};


//-----------------------------------------------------------------------------
// Name: VulkanBuffer
// Desc: Counterpart of IDirect3DVertexBuffer9 and IDirect3DIndexBuffer9.
//
//       Vulkan has one buffer type; a vertex buffer and an index buffer
//       differ only in the usage flags given at creation, which is why the
//       two D3D9 interfaces become one class here. D3DFMT_INDEX16 is not a
//       property of the buffer either -- the index type is given to
//       vkCmdBindIndexBuffer at bind time.
//
//       Lock/Unlock become Map/Unmap, which is the same protocol under a
//       Vulkan name.
//-----------------------------------------------------------------------------
class VulkanBuffer
{
	friend class VulkanDevice;
public:
			VulkanBuffer();
			~VulkanBuffer();

	VkBuffer		Buffer() const		{ return vkBuffer; }
	VkDeviceSize	Size() const		{ return bytes; }
	bool			IsHostVisible() const { return hostVisible; }

	/// \brief Counterpart of Lock(). NULL if the memory is device-local.
	void *			Map(VkDeviceSize offset = 0, VkDeviceSize length = VK_WHOLE_SIZE);
	void			Unmap();

private:
	VkDevice			vkOwner;
	VkBuffer			vkBuffer;
	VkDeviceMemory		vkMemory;
	VkDeviceSize		bytes;
	VkBufferUsageFlags	usage;
	bool				hostVisible;
	void *				mapped;
};


//-----------------------------------------------------------------------------
// Name: ShaderReflection
// Desc: Counterpart of LPD3DXCONSTANTTABLE.
//
//       D3DX produced a constant table as a side product of compiling HLSL,
//       and the client used three things from it: look a name up
//       (GetConstantByName), find which sampler slot a texture name landed in
//       (GetSamplerIndex), and write bytes at a name (SetValue). All three
//       come from SPIR-V reflection here.
//
//       The handle type is the difference that fixes a bug. A D3DXHANDLE is
//       an opaque pointer with no idea which table it came from, which is how
//       ShaderClass::SetPSConstants(HANDLE) could hand a pixel-shader handle
//       to the vertex-shader table and get silence. A Var knows its own
//       stage, so the same mistake cannot be spelled.
//-----------------------------------------------------------------------------
class ShaderReflection
{
public:
	struct Var {
		std::string				name;
		VkShaderStageFlagBits	stage;		///< which module this came from
		uint32_t				set;		///< descriptor set
		uint32_t				binding;	///< descriptor binding
		uint32_t				offset;		///< byte offset within its uniform block
		/// \brief What glslang reports, which is NOT a byte count for a leaf:
		///        it is the number of ARRAY ELEMENTS, and 1 for anything that
		///        is not an array -- a mat4 reports 1, a vec3[27] reports 27.
		///        (Checked against glslang, not assumed.)
		///
		///        The synthesized STRUCT AGGREGATES are the exception and
		///        carry a real byte span, because it can be derived exactly
		///        from the leaves either side of them. See BuildReflection.
		///
		///        Nothing computes a block's size from this. The block's own
		///        size is BlockSize(), which glslang does report in bytes.
		uint32_t				size;
		bool					bSampler;
		/// \brief True when the sampler is a samplerCube.
		///
		/// A view bound to a combined image sampler must have the viewType
		/// the shader's OpTypeImage declares -- a CUBE sampler needs a
		/// VK_IMAGE_VIEW_TYPE_CUBE view, and binding a 2D one is
		/// VUID-vkCmdDrawIndexed-viewType-07752 and a GPU fault, not a
		/// mis-sampled pixel. D3D9 needed no such flag: SetTexture took any
		/// IDirect3DBaseTexture9 and the runtime matched it to the sampler
		/// declaration itself. Recorded here so the unset-slot fallback can
		/// pick a cube for a cube. See ShaderClass::BindResources.
		bool					bCube;
		uint32_t				samplerIndex;	///< counterpart of GetSamplerIndex()
	};

			ShaderReflection(VkShaderStageFlagBits stage) : stage(stage), blockSize(0) {}

	/// \brief Counterpart of ID3DXConstantTable::GetConstantByName.
	const Var *				GetConstantByName(const char *name) const;
	/// \brief Counterpart of ID3DXConstantTable::GetSamplerIndex.
	uint32_t				GetSamplerIndex(const Var *v) const { return v ? v->samplerIndex : 0; }

	VkShaderStageFlagBits	Stage() const		{ return stage; }
	uint32_t				BlockSize() const	{ return blockSize; }
	const std::vector<Var> &Vars() const		{ return vars; }

	void					Add(const Var &v);
	void					SetBlockSize(uint32_t n) { blockSize = n; }

private:
	VkShaderStageFlagBits	stage;
	uint32_t				blockSize;		///< bytes of the uniform block
	std::vector<Var>		vars;
};


//-----------------------------------------------------------------------------
// Name: VulkanDevice
// Desc: Counterpart of LPDIRECT3DDEVICE9 -- the object every draw in the
//       client goes through.
//
//       IT IS NOT A D3D9 DEVICE INTERFACE. There is no SetRenderState, no
//       SetTexture, no DrawPrimitive and no SetVertexDeclaration, because
//       Vulkan has no such state: what D3D9 set as independent pieces of
//       device state is immutable pipeline state here, chosen when the
//       pipeline is built. What is left is what Vulkan actually needs one
//       object to hold:
//
//         - the handles, all adopted from the core, none created here
//         - the command buffer currently being recorded, which is the core's
//           and is valid only for the duration of the scene callback
//         - memory type selection and one-shot submits, the two pieces of
//           bookkeeping every Vulkan program writes and D3D9 hid
//         - the render pass every client pipeline must be built against
//
//       Nothing here is reference counted. Vulkan objects are destroyed by an
//       explicit vkDestroy* against the device that made them, which is why
//       SAFE_RELEASE has no counterpart in the converted client.
//-----------------------------------------------------------------------------
class VulkanDevice
{
public:
			VulkanDevice();
			~VulkanDevice();

	/// \brief Adopt the core's context. Counterpart of CreateDevice().
	bool	Adopt(const OrbiterVulkanContext &ctx);
	void	Destroy();

	// The adopted handles.
	VkInstance			GetInstance() const			{ return vkInstance; }
	VkPhysicalDevice	GetPhysicalDevice() const	{ return vkPhysical; }
	VkDevice			GetDevice() const			{ return vkDevice; }
	VkQueue				GetQueue() const			{ return vkQueue; }
	uint32_t			GetQueueFamily() const		{ return queueFamily; }
	VkDescriptorPool	GetDescriptorPool() const	{ return vkDescriptorPool; }

	/// \brief The render pass the NEXT pipeline must be created against, and
	///        the one the next draw is recorded into.
	///
	///        THIS USED TO BE A CONSTANT and it cannot be. Outside an
	///        offscreen pass it is the core's colour+depth pass, owned by
	///        UIHost.cpp and kept across resizes precisely so the handle stays
	///        valid. Inside BeginOffscreen it is the pass built for the
	///        attachments the caller asked for -- because a VkPipeline is
	///        valid only in a render pass COMPATIBLE with the one it was built
	///        against, and compatibility requires the attachment formats to
	///        match. A pipeline built for the swapchain's B8G8R8A8 pass cannot
	///        be used to draw into an R32_SFLOAT shadow map.
	///
	///        That is why the pipeline caches in ShaderClass and
	///        VulkanEffectFile key on this value alongside their own state:
	///        the same technique drawn into two different targets is two
	///        pipelines. D3D9 needed none of this -- SetRenderTarget was
	///        device state and the shaders did not care what it pointed at.
	VkRenderPass		GetRenderPass() const		{ return vkActivePass ? vkActivePass : vkRenderPass; }

	/// \brief The core's own pass, whatever is active. Used by the frame pump
	///        path, which is always the core's.
	VkRenderPass		GetCoreRenderPass() const	{ return vkRenderPass; }

	// ----------------------------------------------------------------------
	// OFFSCREEN RENDERING
	//
	// Counterpart of SetRenderTarget(i, surf) + SetDepthStencilSurface(surf) +
	// BeginScene() ... EndScene(), and the single largest structural
	// difference left between the two APIs.
	//
	// IN D3D9 A RENDER TARGET IS DEVICE STATE. You set it, you draw, you set
	// another one; the runtime tracked what that meant. In Vulkan the set of
	// attachments is baked into a VkFramebuffer inside a VkRenderPass, a pass
	// cannot be begun inside another pass, and the pipelines drawn in it must
	// have been built against a compatible pass. So "change the render target"
	// is not one call here -- it is: pick or build a render pass for these
	// attachment formats, pick or build a framebuffer for these exact images,
	// transition the images into their attachment layouts, and begin the pass.
	//
	// Both caches are keyed on exactly what the API says makes two passes or
	// two framebuffers interchangeable: the formats for the pass, the image
	// views and extent for the framebuffer. Neither has a D3D9 counterpart,
	// because D3D9 had neither object.
	//
	// THE RECORDING TARGET CHANGES TOO. The frame's command buffer is already
	// inside the core's render pass when the scene callback runs, so an
	// offscreen pass cannot be recorded into it. BeginOffscreen takes a
	// one-shot command buffer of its own, swaps it in as the device's current
	// buffer, and EndOffscreen submits and waits before putting the frame's
	// back. The wait is what makes the result usable by the very next draw,
	// which is the guarantee EndScene() gave on a render target the next call
	// sampled.
	//
	// Up to eight colour attachments -- D3D9 allowed four
	// (D3DCAPS9::NumSimultaneousRTs) and this client uses at most four.
	// pDepth may be NULL for a colour-only pass.
	bool			BeginOffscreen(VulkanTexture * const *ppColour, uint32_t nColour,
								   VulkanTexture *pDepth);
	/// \brief The one-target form, which is what nearly every call site wants.
	bool			BeginOffscreen(VulkanTexture *pColour, VulkanTexture *pDepth = NULL);
	void			EndOffscreen();
	/// \brief True between BeginOffscreen and EndOffscreen.
	bool			IsOffscreen() const			{ return vkActivePass != VK_NULL_HANDLE; }

	/// \brief How many COLOUR attachments the render pass a pipeline is about
	///        to be built for has.
	///
	///        NEEDED BECAUSE VkPipelineColorBlendStateCreateInfo MUST CARRY
	///        ONE ATTACHMENT STATE PER COLOUR ATTACHMENT, and both pipeline
	///        builders hard-coded one. D3D9 had no such requirement:
	///        D3DRS_ALPHABLENDENABLE and friends were device state that
	///        applied to whatever was bound, and multiple render targets
	///        shared it. Getting this wrong is not a picture -- it is
	///        vkCreateGraphicsPipelines rejecting the pipeline.
	uint32_t		GetRenderPassColourCount() const { return vkActivePass ? offColourCount : 1; }

	/// \brief Counterpart of IDirect3DDevice9::Clear with a NULL rectangle
	///        list -- "clear what is currently bound".
	///
	///        NOT ClearImage, AND THE TWO ARE NOT INTERCHANGEABLE. ClearImage
	///        is vkCmdClearColorImage, which clears a whole IMAGE and is legal
	///        only OUTSIDE a render pass. This is vkCmdClearAttachments, which
	///        clears the attachments of the pass currently open and is legal
	///        only INSIDE one -- and that is what Scene's and the client's
	///        Clear() calls are: they run between BeginScene and EndScene,
	///        with a render target bound. Same distinction, and the same
	///        reasoning, as VulkanPad3.cpp's Clear().
	///
	///        The flags are D3DCLEAR_TARGET / _ZBUFFER / _STENCIL as three
	///        booleans, because that is all the client ever passes.
	void			ClearFrame(bool bColour, bool bDepth, bool bStencil,
							   DWORD colour = 0, float z = 1.0f, DWORD stencil = 0);

	/// \brief D3DRS_FILLMODE, which is the last piece of D3D9 DEVICE state in
	///        the client that still has to act globally.
	///
	///        Scene::RenderMainScene sets D3DFILL_WIREFRAME for the whole
	///        frame when the mesh debugger's wireframe flag is on, and
	///        D3DFILL_SOLID otherwise -- one call, affecting every draw that
	///        follows. Vulkan bakes polygonMode into the pipeline, so there is
	///        nothing to set globally; what there is instead is this flag,
	///        read by BOTH pipeline builders and part of BOTH their cache
	///        keys, so that turning it on builds wireframe pipelines and
	///        turning it off goes back to the solid ones already cached.
	void			SetPolygonMode(VkPolygonMode m)	{ polygonMode = m; }
	VkPolygonMode	GetPolygonMode() const			{ return polygonMode; }

	// The command buffer of the frame being recorded. Set by the scene
	// callback on entry and cleared on exit: outside that window there is no
	// command buffer, and a draw call issued then has nowhere to go. D3D9 had
	// no equivalent because a D3D9 device is always recordable.
	void			SetFrameCommandBuffer(VkCommandBuffer cmd, uint32_t w, uint32_t h);
	VkCommandBuffer	GetCommandBuffer() const	{ return vkCmd; }
	bool			IsRecording() const			{ return vkCmd != VK_NULL_HANDLE; }
	uint32_t		GetFrameWidth() const		{ return frameWidth; }
	uint32_t		GetFrameHeight() const		{ return frameHeight; }

	// Resource creation. Counterparts of IDirect3DDevice9::CreateTexture,
	// CreateVertexBuffer and CreateIndexBuffer -- the same responsibility on
	// the same object, spelled in Vulkan's terms. Vulkan does not distinguish
	// a vertex buffer from an index buffer by type, only by the usage flags
	// given here, which is why the two D3D9 calls collapse into one.
	//
	// 'hostVisible' is D3DUSAGE_DYNAMIC: memory the CPU can map and write
	// each frame, as against device-local memory that has to be staged into.
	VulkanTexture  *CreateTexture(uint32_t w, uint32_t h, uint32_t mips,
								  VkFormat fmt, VkImageUsageFlags usage);
	VulkanBuffer   *CreateBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage,
								 bool hostVisible);

	/// \brief Counterpart of D3DXCreateVolumeTexture. A 3D image is a VkImage
	///        with imageType VK_IMAGE_TYPE_3D -- not a separate interface as
	///        IDirect3DVolumeTexture9 was.
	VulkanTexture  *CreateTexture3D(uint32_t w, uint32_t h, uint32_t depth,
									uint32_t mips, VkFormat fmt, VkImageUsageFlags usage);

	/// \brief Counterpart of D3DXCreateCubeTexture. A cube map is a
	///        VK_IMAGE_TYPE_2D image with SIX ARRAY LAYERS and
	///        VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, viewed as
	///        VK_IMAGE_VIEW_TYPE_CUBE -- not a separate interface as
	///        IDirect3DCubeTexture9 was, and not a 3D image with depth 6.
	///        See VulkanTypes.h's note on VulkanImageDesc::Layers.
	VulkanTexture  *CreateTextureCube(uint32_t size, uint32_t mips, VkFormat fmt,
									  VkImageUsageFlags usage);

	/// \brief A view of ONE FACE at one mip level, sharing the parent's image.
	///        Counterpart of IDirect3DCubeTexture9::GetCubeMapSurface(face,
	///        level), which returned a separate IDirect3DSurface9 to render
	///        into. Here it is a VkImageView with baseArrayLayer = face and
	///        viewType 2D, which is what a framebuffer attachment for one face
	///        must be. The result does not own its VkImage, exactly as
	///        CreateMipView's does not.
	///
	///        `face` is the D3DCUBEMAP_FACES ordinal, and the two orders
	///        AGREE: D3D9 numbers +X, -X, +Y, -Y, +Z, -Z from 0, and so does
	///        Vulkan's array-layer order for a cube. So the index carries over
	///        unchanged.
	VulkanTexture  *CreateFaceView(VulkanTexture *pTex, uint32_t face, uint32_t level = 0);

	/// \brief Put CPU bytes into one mip level (and one 3D slice) of an image.
	///
	///        D3D9 did this with LockRect/memcpy/UnlockRect, because a
	///        D3DPOOL_MANAGED resource had a system-memory copy the runtime
	///        uploaded for you. Vulkan device-local memory cannot be mapped at
	///        all: the bytes go into a host-visible staging buffer and are
	///        copied across with vkCmdCopyBufferToImage, with the layout
	///        transitions either side. That is what this hides, and it is the
	///        one piece of D3D9 convenience that has no direct spelling here.
	bool			UploadTexture(VulkanTexture *pTex, uint32_t mip, uint32_t slice,
								  const void *pData, size_t bytes);

	/// \brief Read one mip level back to CPU memory.
	///
	///        D3D9 needed GetRenderTargetData into a D3DPOOL_SYSTEMMEM copy,
	///        because a default-pool resource cannot be locked. Vulkan has the
	///        same restriction and the same answer, except the destination is
	///        a host-visible buffer and the copy is vkCmdCopyImageToBuffer.
	bool			ReadTexture(VulkanTexture *pTex, uint32_t mip, void *pData, size_t bytes);

	/// \brief Copy one image into another, rescaling and converting format.
	///        Counterpart of StretchRect and of D3DXLoadSurfaceFromSurface's
	///        copy half -- vkCmdBlitImage does both. It cannot compress:
	///        no Vulkan call produces BC blocks. See NatCompressSurface.
	bool			BlitTexture(VulkanTexture *pDst, VulkanTexture *pSrc);

	/// \brief The rectangle form, and the exact counterpart of
	///        StretchRect(pSrc, srcRect, pDst, dstRect, filter).
	///
	///        NOT A NEW CAPABILITY. vkCmdBlitImage has always taken
	///        rectangles -- srcOffsets and dstOffsets are two corners each --
	///        so this is the same call spelled with the arguments D3D9
	///        already passed it. The whole-image form above is the case where
	///        both rectangles are the full extent, and it stays separate
	///        because it also walks the mip chain and falls back to
	///        vkCmdCopyImage; this one does mip 0 only, which is what
	///        StretchRect did.
	///
	///        NOTE THE ARGUMENT ORDER. StretchRect names the SOURCE first;
	///        this names the DESTINATION first, matching the two-argument
	///        form above and memcpy. Every call site converted from
	///        StretchRect therefore swaps its first two pairs.
	///
	///        Either rectangle may be NULL, meaning the whole image -- which
	///        is StretchRect's own rule. There is no vkCmdCopyImage fallback
	///        as there is above, because a copy cannot rescale and these two
	///        rectangles are allowed to differ in size; a block-compressed
	///        destination is therefore refused, exactly as StretchRect
	///        refused one.
	bool			BlitTexture(VulkanTexture *pDst, const RECT *tr,
								VulkanTexture *pSrc, const RECT *sr,
								bool bLinear = true);

	/// \brief Build the mip chain by blitting down it. There is no
	///        D3DUSAGE_AUTOGENMIPMAP here -- Vulkan generates nothing.
	bool			GenerateMipmaps(VulkanTexture *pTex);

	/// \brief Counterpart of ColorFill. NULL rectangle means the whole image.
	///
	///        The two cases are not one call, and the reason is a correction
	///        to what this comment first claimed: vkCmdClearColorImage does
	///        NOT take a rectangle list. It takes VkImageSubresourceRanges --
	///        whole mip levels and array layers -- so it answers "clear this
	///        image" and cannot answer "clear this rectangle of it". The only
	///        Vulkan call that clears a rectangle is vkCmdClearAttachments,
	///        which is valid only inside a render pass. SurfNative::Fill is
	///        called from outside one, so the sub-rectangle case is a copy
	///        from a staging buffer filled with the colour instead. See the
	///        implementation.
	bool			ClearImage(VulkanTexture *pTex, const RECT *r, DWORD colour);

	/// \brief A view of one mip level, sharing the parent's image.
	///        Counterpart of GetSurfaceLevel(level). The result does not own
	///        its VkImage.
	VulkanTexture  *CreateMipView(VulkanTexture *pTex, uint32_t level);

	/// \brief Counterpart of CheckDepthStencilMatch for a depth format.
	bool			SupportsDepthStencil(VkFormat fmt) const;

	/// \brief The depth-stencil format of the core's render pass.
	///
	///        There is no query for it: UIHost.cpp's attachDepth chooses it
	///        and publishes only the VkRenderPass, not its attachment formats.
	///        So the client runs THE SAME SELECTION over the same candidate
	///        list against the same physical device and gets the same answer.
	///        UIHost.cpp:756 names this function as the thing it is matching.
	///        Stencil is in every candidate because Mesh.fx's ShadowTech rests
	///        on a stencil test.
	VkFormat		SelectDepthFormat() const;

	/// \brief The colour format of the core's swapchain, by the same
	///        reasoning -- with one honest caveat. UIHost.cpp picks it with
	///        ImGui_ImplVulkanH_SelectSurfaceFormat, which asks the SURFACE
	///        what it supports, and the client has no VkSurfaceKHR. This asks
	///        the physical device instead, over the same candidate list and in
	///        the same order, which agrees on every implementation seen but is
	///        not the identical question. It feeds GetSpecs() and the back
	///        buffer's reported format, not any pipeline.
	VkFormat		SelectColourFormat() const;

	/// \brief A VulkanTexture that DESCRIBES an attachment of the core's
	///        render pass rather than owning an image.
	///
	///        Counterpart of GetRenderTarget(0) and GetDepthStencilSurface(),
	///        which returned real handles because the D3D9 device was the
	///        client's own. Here the swapchain images and the depth buffer
	///        belong to UIHost.cpp, there are several colour images rather
	///        than one, and the client is never given any of them -- it
	///        records commands into the frame's command buffer inside the
	///        render pass the core has already begun.
	///
	///        Image() is VK_NULL_HANDLE and that is the sentinel: a surface
	///        whose resource is a proxy cannot be bound, blitted or mapped,
	///        because there is nothing to bind. What it carries is the extent
	///        and the format, which is all the back-buffer SURFHANDLE is ever
	///        asked for.
	VulkanTexture  *CreateAttachmentProxy(uint32_t w, uint32_t h, VkFormat fmt,
										  VkImageUsageFlags usage);

	// Destruction is on the device too, and that is not symmetry for its own
	// sake. Release() needed no device because a COM object holds its own
	// reference to one; vkDestroyImage and vkFreeMemory both take the VkDevice
	// as their first argument, so the device is the only thing that can do it.
	//
	// It also has to be a call rather than `delete x` at the call site. The
	// pools in VulkanCatalog.h see these types only through the forward
	// declarations in VulkanTypes.h, and deleting an incomplete type is
	// undefined behaviour that runs NO destructor -- so every recycled tile
	// would leak its image and its memory, silently, until the GPU ran out.
	// GCC reports it as -Wdelete-incomplete; MSVC's C4150 is off by default.
	void			DestroyTexture(VulkanTexture *pTex);
	void			DestroyBuffer(VulkanBuffer *pBuf);

	// Bookkeeping D3D9 did not expose because it did it for you.
	uint32_t		FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;

	/// \brief Begin a one-shot command buffer for uploads and layout
	///        transitions, outside the frame. Counterpart of the implicit
	///        upload D3DPOOL_MANAGED and UpdateTexture performed.
	VkCommandBuffer	BeginOneShot();
	void			EndOneShot(VkCommandBuffer cmd);

	VkCommandPool	GetCommandPool() const		{ return vkCommandPool; }
	VkPipelineCache	GetPipelineCache() const	{ return vkPipelineCache; }

	const VkPhysicalDeviceProperties *GetProperties() const	{ return &props; }
	const VkPhysicalDeviceFeatures   *GetFeatures() const	{ return &features; }

	/// \brief Counterpart of IDirect3DDevice9::GetAvailableTextureMem, as
	///        closely as Vulkan allows -- which is not very, and the log line
	///        that prints it says so. D3D9 reported what was FREE. Core
	///        Vulkan reports only what EXISTS:
	///        vkGetPhysicalDeviceMemoryProperties gives heap sizes and there
	///        is no free figure without VK_EXT_memory_budget. So this is the
	///        total size of the device-local heaps, which is an upper bound
	///        rather than the same number.
	VkDeviceSize	GetLocalMemorySize() const;

	/// \brief Whether a format can be sampled from a vertex shader.
	///        Counterpart of CheckDeviceFormat(D3DUSAGE_QUERY_VERTEXTEXTURE).
	bool	SupportsVertexTexture(VkFormat fmt) const;
	/// \brief Counterpart of CheckDeviceFormat(D3DUSAGE_RENDERTARGET).
	bool	SupportsRenderTarget(VkFormat fmt) const;

private:

	VkInstance			vkInstance;
	VkPhysicalDevice	vkPhysical;
	VkDevice			vkDevice;
	VkQueue				vkQueue;
	uint32_t			queueFamily;
	VkDescriptorPool	vkDescriptorPool;
	VkRenderPass		vkRenderPass;

	// Created here, not adopted: the core's pool is ImGui's and its buffers
	// are the frame's. Uploads need their own.
	VkCommandPool		vkCommandPool;
	VkPipelineCache		vkPipelineCache;

	VkCommandBuffer		vkCmd;			// borrowed, only during the callback
	uint32_t			frameWidth;
	uint32_t			frameHeight;

	// --- offscreen state; see BeginOffscreen ------------------------------
	//
	// The two caches are the objects D3D9 did not have. A render pass is
	// interchangeable with another when their attachment FORMATS and sample
	// counts match, so the pass key is the format list; a framebuffer is tied
	// to the exact VkImageViews and the extent, so that is its key. Both are
	// std::map rather than unordered_map for the same reason ShaderClass's
	// pipeline cache is: the keys are tiny, the maps hold a handful of
	// entries, and a comparison is cheaper than a hash.
	std::map<std::vector<uint32_t>, VkRenderPass>	offPassCache;
	std::map<std::vector<uint64_t>, VkFramebuffer>	offFbCache;

	/// \brief Fetch or build the render pass for this set of attachment
	///        formats. depthFmt VK_FORMAT_UNDEFINED means colour only.
	VkRenderPass	GetOffscreenPass(const VkFormat *pColFmt, uint32_t nCol, VkFormat depthFmt);
	/// \brief Fetch or build the framebuffer for these exact views.
	VkFramebuffer	GetOffscreenFramebuffer(VkRenderPass pass, const VkImageView *pView,
											uint32_t nView, uint32_t w, uint32_t h);

	VkRenderPass		vkActivePass;	///< non-NULL only inside an offscreen pass
	VkCommandBuffer		vkSavedCmd;		///< the frame's buffer, put back by EndOffscreen
	uint32_t			savedWidth, savedHeight;
	VulkanTexture *		offColour[8];
	uint32_t			offColourCount;
	VulkanTexture *		offDepth;

	/// \brief D3DRS_FILLMODE; see SetPolygonMode. VK_POLYGON_MODE_FILL is
	///        D3DFILL_SOLID, which is what the device starts in.
	VkPolygonMode		polygonMode;

	VkPhysicalDeviceProperties			props;
	VkPhysicalDeviceMemoryProperties	memProps;
	VkPhysicalDeviceFeatures			features;
};


//-----------------------------------------------------------------------------
// Name: CVulkanFramework
// Desc: Was CD3DFramework9. Maintains the device and the render surface used
//       for 3D rendering.
//-----------------------------------------------------------------------------
class CVulkanFramework
{

private:

    // Internal variables for the framework class
    HWND                   hWnd;               // The window object
    BOOL                   bIsFullscreen;      // Fullscreen vs. windowed
    BOOL                   bVertexTexture;
    BOOL                   bAAEnabled;
    BOOL                   bNoVSync;           // don't use vertical sync in fullscreen
    BOOL                   Alpha;
    DWORD                  dwRenderWidth;      // Dimensions of the render target
    DWORD                  dwRenderHeight;     // Dimensions of the render target
    DWORD                  dwFSMode;
    VulkanDevice          *pDevice;            // The Vulkan device
    DWORD                  dwZBufferBitDepth;  // Bit depth of z-buffer
    DWORD                  dwStencilBitDepth;  // Bit depth of stencil buffer (0 if none)
    DWORD                  Adapter;
    DWORD                  Mode;
    DWORD                  MultiSample;
	DWORD				   dwDisplayMode;

	// Were LPDIRECT3DSURFACE9, filled by GetRenderTarget(0) and
	// GetDepthStencilSurface(). They are kept, and kept for the same reason:
	// DestroyObjects has to let go of them. What changed is what they hold --
	// the swapchain colour image and the depth buffer belong to UIHost.cpp and
	// the client is never handed either, so these are the attachment proxies
	// CreateAttachmentProxy returns: the extent and the format of the core's
	// two render-pass attachments, and no image at all.
    VulkanTexture         *pRenderTarget;
    VulkanTexture         *pDepthStencil;

    SURFHANDLE			   pBackBuffer;
    RECT                   rcScreenRect;       // Screen rect for window

	// SWVert, Pure, DDM and nvPerfHud are gone. They set
	// D3DCREATE_SOFTWARE_VERTEXPROCESSING, D3DCREATE_PUREDEVICE,
	// D3DCREATE_DISABLE_DRIVER_MANAGEMENT and selected D3DDEVTYPE_REF -- flags
	// to a CreateDevice call that no longer happens, on a device the core
	// already made. Vulkan has no software vertex processing, no "pure"
	// device (it never reports back state, which is what PUREDEVICE bought),
	// and no driver-managed resource pool to disable.

    // Internal functions for the framework class
    HRESULT AdoptDevice();
    void    Clear();

public:

    // Access functions
    inline HWND                GetRenderWindow() const          { return hWnd; }
    inline VulkanDevice*       GetVulkanDevice() const          { return pDevice; }
    inline DWORD               GetZBufferBitDepth() const       { return dwZBufferBitDepth; }
    inline DWORD               GetStencilBitDepth() const       { return dwStencilBitDepth; }
    inline DWORD               GetWidth() const                 { return dwRenderWidth; }  // Dimensions of the render target
    inline DWORD               GetHeight() const                { return dwRenderHeight; } // Dimensions of the render target
    inline const RECT          GetScreenRect() const            { return rcScreenRect; }
    inline SURFHANDLE          GetBackBufferHandle() const      { return pBackBuffer; }

	// The two attachment proxies, which is what
	// VulkanClient::clbkCreateRenderWindow needs where the Windows version
	// called pDevice->GetRenderTarget(0, ...) and GetDepthStencilSurface().
	// Those two D3D9 calls handed back the device's CURRENT targets, which
	// the client owned; here the swapchain colour image and the depth buffer
	// belong to UIHost.cpp and the client is handed neither, so what it gets
	// is the extent and format of the core's two render-pass attachments --
	// all the back-buffer SURFHANDLE is ever asked for. The framework already
	// built both in AdoptDevice; these expose them.
	inline VulkanTexture      *GetRenderTargetProxy() const      { return pRenderTarget; }
	inline VulkanTexture      *GetDepthStencilProxy() const      { return pDepthStencil; }
    inline BOOL                IsFullscreen() const             { return bIsFullscreen; }
    inline BOOL                IsAAEnabled() const              { return bAAEnabled; }
    inline BOOL                HasVertexTextureSup() const      { return bVertexTexture; }
    inline BOOL                GetVSync() const                 { return (bNoVSync==FALSE); }

	// Was GetCaps() returning const D3DCAPS9*. D3DCAPS9 was one struct
	// covering limits, format support and feature bits; Vulkan splits those
	// three across VkPhysicalDeviceProperties::limits,
	// vkGetPhysicalDeviceFormatProperties and VkPhysicalDeviceFeatures, so
	// there is no single struct to hand back. This returns the one that holds
	// the limits, and the format and feature queries are methods on
	// VulkanDevice.
	inline const VkPhysicalDeviceProperties *GetCaps() const
	{ return pDevice ? pDevice->GetProperties() : nullptr; }

	// GetDisplayMode 0=True Fullscreen, 1=Fullscreen Window, 2=Windowed
	inline DWORD			   GetDisplayMode() const			{ return dwDisplayMode; }

	// GetLargeFont/GetSmallFont are gone with LPD3DXFONT.
	//
	// D3DXCreateFontIndirect wraps GDI glyph rasterisation, and
	// Src/Orbiter/Linux/Gdi.cpp is a display-list RECORDER, not a rasteriser
	// -- TextOut appends a DrawCmd and orbiter_ReplayDC turns it into ImGui
	// draw commands, so there are no glyph pixels to blit. The small font was
	// created and never used at all; the large one has three call sites, all
	// in one debug overlay (D3D9Client.cpp:4090-4099, the frame-rate label and
	// "Frozen"), which are converted to the client's own text path in
	// VulkanTextMgr rather than to a second font system here.

    // Creates the Framework
    HRESULT Initialize(HWND hWnd, struct oapi::GraphicsClient::VIDEODATA *vData);

    HRESULT DestroyObjects();

            CVulkanFramework();
           ~CVulkanFramework();
};


//-----------------------------------------------------------------------------
// Flags used for the Initialize() method of a CVulkanFramework object
//-----------------------------------------------------------------------------
#define VKFW_FULLSCREEN    0x00000001 // Use fullscreen mode
#define VKFW_STEREO        0x00000002 // Use stereo-scopic viewing
#define VKFW_ZBUFFER       0x00000004 // Create and use a zbuffer
#define VKFW_NOVSYNC       0x00000010 // Don't use vertical sync in fullscreen
#define VKFW_PAGEFLIP      0x00000020 // Allow page flipping in fullscreen

// D3DFW_NO_FPUSETUP is not carried over: it suppressed DDSCL_FPUSETUP, a
// DirectDraw cooperative-level flag that set the x87 control word. There is
// no such call to suppress.

// The D3DFWERR_* block is gone. Those fourteen codes name DirectDraw and D3D
// device-setup failures -- NODIRECTDRAW, COULDNTSETCOOPLEVEL, NOPRIMARY,
// NOCLIPPER, NOBACKBUFFER, BADDISPLAYMODE, NONZEROREFCOUNT -- for a setup
// sequence the client no longer performs. Nothing in the tree tested them:
// every caller of Initialize() checks FAILED(hr) only.

#endif // !VULKANFRAME_H
