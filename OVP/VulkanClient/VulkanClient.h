// ==============================================================
// VulkanClient.h
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2006-2026 Martin Schweiger
//				 2012-2016 Jarmo Nikkanen
// ==============================================================
//
// CONVERTED FROM OVP/D3D9Client/D3D9Client.h, read end to end (1485 lines).
//
// The client class itself is an oapi::GraphicsClient override set, and the
// GraphicsClient API is already cross-platform -- so most of this file is
// signatures that do not change and the SDK documentation that goes with
// them. What changed:
//
//  1. THE DEVICE AND RESOURCE TYPES, per the tree-wide mapping:
//     LPDIRECT3DDEVICE9 -> VulkanDevice*, LPDIRECT3DTEXTURE9 ->
//     VulkanTexture*, LPDIRECT3DSURFACE9 -> VulkanTexture* (a Vulkan render
//     target is an image like any other; D3D9's separate surface interface
//     has no counterpart), LPD3DXMATRIX -> FMATRIX4*, D3DCAPS9 ->
//     VkPhysicalDeviceProperties, CD3DFramework9 -> CVulkanFramework.
//
//  2. class RenderState IS GONE, and this is the largest single removal in
//     the file. It captured thirteen pieces of D3D9 device render state --
//     ALPHABLENDENABLE, ZENABLE, ZWRITEENABLE, CULLMODE, COLORWRITEENABLE,
//     SCISSORTESTENABLE, FILLMODE, STENCILENABLE, ALPHATESTENABLE, BLENDOP,
//     SRCBLEND, DESTBLEND and the scissor rect -- so that the Sketchpad could
//     restore them after drawing over a scene. It is used twice, both in
//     D3D9Pad.cpp.
//
//     VULKAN HAS NO DEVICE RENDER STATE. Every one of those thirteen except
//     the scissor rect is immutable pipeline state, fixed when the
//     VkPipeline is created; the scissor and viewport are dynamic state set
//     per command buffer. A draw cannot inherit or corrupt another draw's
//     state, because a draw brings its own pipeline. There is nothing to
//     capture and nothing to restore, so VulkanPad simply binds the pipeline
//     it needs.
//
//     Worth recording, because it says the removal is safe rather than
//     merely convenient: D3D9Frame.cpp sets D3DCREATE_PUREDEVICE
//     unconditionally (Pure = true in Clear(), never cleared anywhere in the
//     file), and a pure device FAILS GetRenderState. So on Windows this class
//     was already capturing nothing and restoring nothing, thirteen logged
//     HR errors at a time.
//
//  3. IsLimited() IS ALWAYS false. It asked whether the hardware has only
//     conditional non-power-of-two texture support
//     (D3DPTEXTURECAPS_POW2 && NONPOW2CONDITIONAL). Vulkan core requires full
//     NPOT support of every conforming implementation, so the condition
//     cannot arise. Kept as a function because callers ask.
//
//  4. g_pD3DObject IS GONE. The VkInstance comes from the core's context; see
//     VulkanFrame.h.
//
//  5. GetBackBuffer()/GetDepthStencil() RETURN CLIENT IMAGES, NOT THE
//     SWAPCHAIN'S. The client does not own the swapchain image and never
//     acquires or presents one -- UIHost.cpp does both. See VulkanFrame.h.
//
//  6. THE _MSC_VER FSTREAM BRANCH IS GONE. It selected <fstream.h> for
//     pre-2003 MSVC. <fstream> is correct everywhere this builds.
//
//  7. THE NVAPI STEREO HANDLE IS GONE. It sat behind #ifdef _NVAPI_H, which
//     is never defined in this tree, and NVAPI is a Windows-only Direct3D
//     library with no Linux counterpart.
//
// WindowMgr.h, which this includes, needed NO CHANGES AT ALL -- it is pure
// Win32 GDI and gcGUI, and compiles against Src/Orbiter/Linux/windows.h as
// written. Checked by compiling it, not by reading it hopefully.
// ==============================================================

#ifndef __VULKANCLIENT_H
#define __VULKANCLIENT_H

#include <fstream>

#include <vulkan/vulkan.h>
#include "VulkanTypes.h"
#include "VulkanCatalog.h"
#include "GraphicsAPI.h"
#include "VulkanUtil.h"
#include <stdio.h>
#include <assert.h>
#include "OrbiterAPI.h"
#include "VulkanFrame.h"
//#include "gcCore.h"
#include <vector>
#include <stack>
#include <list>
#include "WindowMgr.h"

#define PP_DEFAULT			0x1
#define PP_LENSFLARE		0x2

#define MAX_SCENE_LIGHTS	(DWORD)24
#define MAX_MESH_LIGHTS		8	// Must match the setting in the GLSL translation of D3D9Client.fx

class VulkanConfig;
class MeshManager;
class TextureManager;
class Scene;
class VideoTab;
class SurfNative;
class CVulkanFramework;
class VulkanMesh;
class VulkanAnnotation;
class VulkanText;
class CSphereManager;
class FileParser;
class OapiExtension;
class VulkanPad;

typedef char* LPCHAR;
typedef void* CAMERAHANDLE;
typedef class VulkanMesh* HMESH;
typedef class SurfNative* lpSurfNative;

/**
 * \brief Statistical data storage
 */
struct _VulkanStats
{
	_VulkanStats()
	{
		memset(&Mesh, 0, sizeof(Mesh));
		memset(&Timer, 0, sizeof(Timer));
		TilesAllocated = 0;
	}

	struct {
		DWORD Vertices;		///< Number of vertices rendered
		DWORD MeshGrps;		///< Number of mesh groups rendered
		DWORD Meshes;		///< Number of meshes rendered
		DWORD TexChanges;	///< Number of texture changes
		DWORD MtrlChanges;	///< Number of material changes
	} Mesh;					///< Mesh related statistics
			
	struct {
		VulkanTime Update;		///< clbkUpdate
		VulkanTime Scene;		///< clbkRenderScene
		VulkanTime Display;		///< clbkDisplayFrame
		VulkanTime FrameTotal;	///< Total frame time
		//------------------------------------------------------------
		VulkanTime HUDOverlay;	///< Total time spend in HUD, 2D Panel overlay
		VulkanTime CamVis;		///< Object/camera updates
		VulkanTime Surface;		///< Surface
		VulkanTime Clouds;		///< Clouds
		//-------------------------------------------------------------
		VulkanTime LockWait;	///< Time waiting GetDC or vertex buffer lock
		VulkanTime BlitTime;	///<
		VulkanTime GetDC;		///<
	} Timer;					///< Render timing related statistics

	DWORD TilesAllocated;	///< Number of allocated tiles
	std::map<DWORD, DWORD> TilesRendered;	///< Number of rendered tiles
};


// A D3D9 render target was an IDirect3DSurface9, a type distinct from a
// texture. Vulkan has one image type used for both, so both members are
// VulkanTexture* and the distinction survives only as usage flags.
struct RenderTgtData {
	VulkanTexture *pColor;
	VulkanTexture *pDepthStencil;
	class VulkanPad *pSkp;
	int code;
};


extern _VulkanStats VulkanStats;
extern bool bFreeze;
extern bool bFreezeEnable;
extern bool bFreezeRenderAll;
extern DWORD			uCurrentMesh;
extern class vObject* pCurrentVisual;
extern std::set<VulkanMesh*> MeshCatalog;
extern std::set<SurfNative*>	SurfaceCatalog;
extern Memgr<float>* g_pMemgr_f;
extern Memgr<INT16>* g_pMemgr_i;
extern Memgr<UINT8>* g_pMemgr_u;
extern Memgr<WORD>* g_pMemgr_w;
extern Memgr<VERTEX_2TEX>* g_pMemgr_vtx;
extern Texmgr<VulkanTexture*>* g_pTexmgr_tt;
extern Vtxmgr<VulkanBuffer*>* g_pVtxmgr_vb;
extern Idxmgr<VulkanBuffer*>* g_pIdxmgr_ib;


namespace oapi {



// ==============================================================
// VulkanClient class interface
// The Vulkan render client for Orbiter
// ==============================================================

class VulkanClient : public GraphicsClient 
{

	friend class ::Scene;	// <= likes to call Render2DOverlay()

public:

	/**
	 * \brief Create a graphics object.
	 *
	 * The graphics object is typically created during module initialisation
	 * (see \ref InitModule). Once the client is created, it must be registered
	 * with the Orbiter core via the oapiRegisterGraphicsClient function.
	 * \param hInstance module instance handle (as passed to InitModule)
	 */
	explicit VulkanClient (HINSTANCE hInstance);

	/**
	 * \brief Destroy the graphics object.
	 *
	 * Usually, the graphics object is destroyed when the module is unloaded
	 * (see opcDLLExit), after is has been detached from the Orbiter core
	 * via a call to oapiUnregisterGraphicsClient.
	 */
	~VulkanClient ();


	HBITMAP gcReadImageFromFile(const char *path);

	/**
	 * \brief Perform any one-time setup tasks.
	 *
	 * This includes enumerating drivers, graphics modes, etc.
	 * Derived classes should also call the base class method to allow
	 * default setup.
	 * \default Initialises the VideoData structure from the Orbiter.cfg
	 *   file
	 * \par Calling sequence:
	 *   Called during processing of oapiRegisterGraphicsClient, after the
	 *   Launchpad Video tab has been inserted (if clbkUseLaunchpadVideoTab
	 *   returns true).
	 */
	bool clbkInitialise ();

	/**
	 * \brief Request for video configuration data
	 *
	 * Called by Orbiter before the render window is opened or configuration
	 * parameters are written to file. Applications should here either update
	 * the provided VIDEODATA structure from any user selections made in the
	 * Launchpad Video tab and leave it to Orbiter to write these parameters
	 * to Orbiter.cfg, or write the current video settings to their own
	 * configuration file.
	 * \default None.
	 */
	void clbkRefreshVideoData ();

	/**
     * \brief Called when a config setting is changed by the user during
     *    a simulation setting, to give the client opportunity to respond to the change.
     * \param cat option category, see \ref optcat
     * \param item option item, see \ref optitem
     */
	void clbkOptionChanged(DWORD cat, DWORD item);

	void clbkDebugString(const char* str);

	/**
	 * \brief Texture request
	 *
	 * Load a texture from a file into a device-specific texture object, and
	 * return a generic SURFHANDLE for it.
	 * \param fname texture file name with path relative to orbiter
	 *   texture folders; can be used as input for OpenTextureFile.
	 * \param flags request for texture properties
	 * \return Texture handle, cast into generic SURFHANDLE, or NULL if texture
	 *   could not be loaded.
	 * \note The following flags are supported:
	 *   - bit 0 set: force creation in system memory
	 *   - bit 1 set: decompress, even if format is supported by device
	 *   - bit 2 set: don't load mipmaps, even if supported by device
	 *   - bit 3 set: load as global resource (can be managed by graphics client)
	 * \note If bit 3 of flags is set, orbiter will not try to modify or release
	 *   the texture. The client should manage the texture (i.e. keep it in a
	 *   repository and release it at destruction).
	 */
	SURFHANDLE clbkLoadTexture (const char *fname, DWORD flags = 0);

	/**
	 * \brief Load a surface from file into a surface object, and return a SURFHANDLE for it.
	 * \param fname texture file name with path relative to orbiter texture folders
	 * \param attrib \ref surfacecaps (see notes)
	 * \return A SURFHANDLE for the loaded surface, for example a pointer to the surface object.
	 * \note The attrib bitflag can contain one of the following main attributes:
	 *  - OAPISURFACE_RO: Load the surface to be readable by the GPU pipeline
	 *  - OAPISURFACE_RW: Load the surface to be readable and writable by the GPU pipeline
	 *  - OAPISURFACE_GDI: Load the surface to be readable and writable by the CPU, and can be blitted into an uncompressed RO or RW surface without alpha channel
	 *  - OAPISURFACE_STATIC: Load the surface to be readable by the GPU pipeline
     *  In addition, the flag can contain any of the following auxiliary attributes:
	 *  - OAPISURFACE_MIPMAPS: Load the mipmaps for the surface from file, or create them if necessary
	 *  - OAPISURFACE_NOMIPMAPS: Don't load mipmaps, even if they are available in the file
	 *  - OAPISURFACE_NOALPHA: Load the surface without an alpha channel
	 *  - OAPISURFACE_UNCOMPRESS: Uncompress the surface on loading.
	 * \sa oapiCreateSurface(DWORD,DWORD,DWORD)
	 */
	SURFHANDLE clbkLoadSurface (const char *fname, DWORD attrib, bool bPath = false);


	/**
	 * \brief Save the contents of a surface to a formatted image file or to the clipboard
	 * \param surf surface handle (0 for primary render surface)
	 * \param fname image file path relative to orbiter root directory (excluding file extension), or NULL to save to clipboard
	 * \param fmt output file format
	 * \param quality quality request if the format supports it (0-1)
	 * \return Should return true on success
	 */
	bool clbkSaveSurfaceToImage (SURFHANDLE surf, const char *fname, ImageFileFormat fmt, float quality=0.7f);

	/**
	 * \brief Write surface to file (sub-function of \ref clbkSaveSurfaceToImage)
	 *
	 * Was (const D3DSURFACE_DESC*, D3DLOCKED_RECT&, ...). A D3DSURFACE_DESC is
	 * format, type, usage, pool, multisample settings, width and height; a
	 * D3DLOCKED_RECT is a mapped pointer plus a row pitch. Vulkan splits those
	 * across VkImageCreateInfo, the memory allocation and vkGetImageSubresourceLayout,
	 * and the client already carries the assembled facts in VulkanTexture --
	 * so the description is the texture itself, and the mapped bits and pitch
	 * are passed alongside as what they are.
	 *
	 * THE SIZE IS A PARAMETER AGAIN, and that is the reference's shape rather
	 * than a departure from it: the Windows body reads desc->Width and
	 * desc->Height out of the D3DSURFACE_DESC it is handed, never out of a
	 * texture. It matters for the back-buffer case, where the pixels come
	 * from orbiter_CaptureBackBuffer and the SurfNative standing in for the
	 * back buffer is a proxy whose recorded size was fixed at session start
	 * -- a window resize would otherwise write a JPEG of the wrong dimensions
	 * out of a correctly captured frame.
	 *
	 * \param pTex source texture (format description; may be the back-buffer proxy)
	 * \param pBits mapped pixel data
	 * \param pitch bytes per row of pBits
	 * \param width pixel width of pBits
	 * \param height pixel height of pBits
	 * \param fname image file path relative to orbiter root directory (excluding file extension)
	 * \param fmt output file format
	 * \param quality quality request if the format supports it (0-1)
	 * \return Should return true on success
	 */
	bool SaveSurfaceToFile (const VulkanTexture* pTex, const void* pBits, size_t pitch,
	                        DWORD width, DWORD height,
	                        const char* fname, ImageFileFormat fmt, float quality);

	/**
	 * \brief Store an image to the clipboard (sub-function of \ref clbkSaveSurfaceToImage)
	 *
	 * \param pTex source texture
	 * \param pBits mapped pixel data
	 * \param pitch bytes per row of pBits
	 * \return Should return true on success
	 */
	bool SaveSurfaceToClipboard (const VulkanTexture* pTex, const void* pBits, size_t pitch);

	/**
	 * \brief Texture release request
	 *
	 * Called by Orbiter when a previously loaded texture can be released
	 * from memory.
	 * \param hTex texture handle
	 */
	void clbkReleaseTexture (SURFHANDLE hTex);

	/**
	 * \brief Replace a texture in a device-specific mesh.
	 * \param hMesh device mesh handle
	 * \param texidx texture index (>= 0)
	 * \param tex texture handle
	 * \return Should return \e true if operation successful, \e false otherwise.
	 */
	bool clbkSetMeshTexture (DEVMESHHANDLE hMesh, DWORD texidx, SURFHANDLE tex);

	/**
	 * \brief Replace properties of an existing mesh material.
	 * \param hMesh device mesh handle
	 * \param matidx material index (>= 0)
	 * \param mat pointer to material structure
	 * \return Overloaded functions should return an integer error flag, with
	 *   the following codes: 0="success", 3="invalid mesh handle", 4="material index out of range"
	 */
	int clbkSetMeshMaterial(DEVMESHHANDLE hMesh, DWORD matidx, const MATERIAL* mat);
	int clbkSetMeshMaterialEx(DEVMESHHANDLE hMesh, DWORD matidx, MatProp mat, const oapi::FVECTOR4* in);

	/**
	* \brief Retrieve the properties of one of the mesh materials.
	* \param hMesh device mesh handle
	* \param matidx material index (>= 0)
	* \param mat [out] pointer to MATERIAL structure to be filled by the method.
	* \return true if successful, false on error (index out of range)
	*/
	int clbkMeshMaterial(DEVMESHHANDLE hMesh, DWORD matidx, MATERIAL* mat);
	int clbkMeshMaterialEx(DEVMESHHANDLE hMesh, DWORD matidx, MatProp mat, oapi::FVECTOR4* out);

	/**
     * \brief Set custom properties for a device-specific mesh.
	 * \param hMesh device mesh handle
	 * \param property property tag
	 * \param value new mesh property value
	 * \return The method should return \e true if the property tag was recognised
	 *   and the request could be executed, \e false otherwise.
	 * \note - \c MESHPROPERTY_MODULATEMATALPHA \n \n
	 * if value==0 (default) disable material alpha information in textured mesh groups (only use texture alpha channel).\n
	 * if value<>0 modulate (mix) material alpha values with texture alpha maps.
	 */
	bool clbkSetMeshProperty (DEVMESHHANDLE hMesh, DWORD property, DWORD value);

	/**
	 * \brief React to vessel creation
	 * \param hVessel object handle of new vessel
	 * \note Calls Scene::NewVessel() to check for visual
	 */
	void clbkNewVessel (OBJHANDLE hVessel);

	/**
	 * \brief React to vessel destruction
	 * \param hVessel object handle of vessel to be destroyed
	 * \note Calls Scene::DeleteVessel() to remove the visual
	 */
	void clbkDeleteVessel (OBJHANDLE hVessel);


	/**
	 * \brief Message callback for a visual object.
	 * \param hObj handle of the object that created the message
	 * \param vis client-supplied identifier for the visual
	 * \param msg event identifier
	 * \param context message context
	 * \return Function should return 1 if it processes the message, 0 otherwise.
	 * \sa RegisterVisObject, UnregisterVisObject, visevent
	 */
	int clbkVisEvent (OBJHANDLE hObj, VISHANDLE vis, DWORD msg, DWORD_PTR context);

	/**
	 * \brief Return a DEVMESHHANDLE handle for a visual, defined by its index. (! Return type incorrect !)
	 * \param vis visual identifier
	 * \param idx mesh index (>= 0)
	 * \return Mesh handle (client-specific)
	 */
	virtual MESHHANDLE clbkGetMesh (VISHANDLE vis, UINT idx);

	/**
	 * \brief Mesh group data retrieval interface for device-specific meshes.
	 * \param hMesh device mesh handle
	 * \param grpidx mesh group index (>= 0)
	 * \param grs data buffers and buffer size information. See \ref oapiGetMeshGroup
	 * \return Should return 0 on success, or error flags > 0.
	 */
	int clbkGetMeshGroup (DEVMESHHANDLE hMesh, DWORD grpidx, GROUPREQUESTSPEC *grs);

	/**
	 * \brief Mesh group editing interface for device-specific meshes.
	 * \param hMesh device mesh handle
	 * \param grpidx mesh group index (>= 0)
	 * \param ges mesh group modification specs
	 * \return Should return 0 on success, or error flags > 0.
	 */
	int clbkEditMeshGroup (DEVMESHHANDLE hMesh, DWORD grpidx, GROUPEDITSPEC *ges);


	// ==================================================================
	/// \name Dialog interface
	//@{
	/**
	 * \brief Popup window open notification.
	 */
	void clbkPreOpenPopup ();

	//@}

	// ==================================================================
	/// \name Particle stream methods
	// @{

	/**
	 * \brief Create a generic particle stream.
	 * \param pss particle stream parameters
	 * \return Pointer to new particle stream.
	 * \sa ParticleStream
	 */
	ParticleStream *clbkCreateParticleStream (PARTICLESTREAMSPEC *pss);

	/**
	 * \brief Create a particle stream associated with a vessel.
	 * \param pss particle stream parameters
	 * \param hVessel vessel handle
	 * \param lvl pointer to exhaust level control variable
	 * \param ref pointer to stream source position (vessel frame) [<b>m</b>]
	 * \param dir pointer to stream direction (vessel frame)
	 * \return Pointer to new particle stream
	 */
	ParticleStream *clbkCreateExhaustStream (PARTICLESTREAMSPEC *pss, OBJHANDLE hVessel, const double *lvl, const VECTOR3 *ref, const VECTOR3 *dir);

	/**
	 * \brief Create a particle stream associated with a vessel.
	 * \param pss particle stream parameters
	 * \param hVessel vessel handle
	 * \param lvl pointer to exhaust level control variable
	 * \param ref stream source position (vessel frame) [<b>m</b>]
	 * \param dir stream direction (vessel frame)
	 * \return Pointer to new particle stream
	 * \note The ref and dir parameters are fixed in this version of the method.
	 */
	ParticleStream *clbkCreateExhaustStream (PARTICLESTREAMSPEC *pss, OBJHANDLE hVessel, const double *lvl, const VECTOR3 &ref, const VECTOR3 &dir);

	/**
	 * \brief Create a vessel particle stream for reentry heating effect
	 * \param pss particle stream parameters
	 * \param hVessel vessel handle
	 * \return Pointer to new particle stream
	 */
	ParticleStream *clbkCreateReentryStream (PARTICLESTREAMSPEC *pss, OBJHANDLE hVessel);
	// @}

	/**
	 * \brief Create an annotation object for displaying on-screen text.
	 * \return Pointer to new screen annotation object.
	 */
	ScreenAnnotation* clbkCreateAnnotation();

	/**
	 * \brief Render window message handler
	 * \param hWnd render window handle
	 * \param uMsg Windows message identifier
	 * \param wParam WPARAM message parameter
	 * \param lParam LPARAM message parameter
	 * \return The return value depends on the message being processed.
	 * \note This method currently intercepts only the WM_CLOSE and WM_DESTROY
	 *   messages, and passes everything else to the Orbiter core message
	 *   handler.
	 */
	LRESULT RenderWndProc (HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

	/**
	 * \brief Message handler for 'video' tab in Orbiter Launchpad dialog
	 * \param hWnd window handle for video tab
	 * \param uMsg Windows message
	 * \param wParam WPARAM message value
	 * \param lParam LPARAM message value
	 * \return The return value depends on the message type and the action taken.
	 */
	INT_PTR LaunchpadVideoWndProc (HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

	/**
	 * \brief Fullscreen mode flag
	 * \return true if the client is set up for running in fullscreen
	 *   mode, false for windowed mode.
	 */
	bool clbkFullscreenMode () const;

	/**
	 * \brief Returns the dimensions of the render viewport
	 * \param width render viewport width [pixel]
	 * \param height render viewport height [pixel]
	 */
	void clbkGetViewportSize (DWORD *width, DWORD *height) const;

	/**
	 * \brief Returns a specific render parameter
	 * \param[in] prm parameter identifier (see \sa renderprm)
	 * \param[out] value value of the queried parameter
	 * \return true if the specified parameter is supported by the client,
	 *    false if not.
	 */
	bool clbkGetRenderParam (DWORD prm, DWORD *value) const;

	/**
	 * \brief Render an instrument panel in cockpit view as a 2D billboard.
	 * \param hSurf array of texture handles for the panel surface
	 * \param hMesh billboard mesh handle
	 * \param T transformation matrix for panel mesh vertices (2D)
	 * \param transparent If true, panel should be rendered additive (transparent)
	 */
	void clbkRender2DPanel (SURFHANDLE *hSurf, MESHHANDLE hMesh, MATRIX3 *T, bool transparent = false);

	/**
	 * \brief Render an instrument panel in cockpit view as a 2D billboard.
	 * \param hSurf array of texture handles for the panel surface
	 * \param hMesh billboard mesh handle
	 * \param T transformation matrix for panel mesh vertices (2D)
	 * \param alpha opacity value, between 0 (transparent) and 1 (opaque)
	 * \param additive If true, panel should be rendered additive (transparent)
	 */
	void clbkRender2DPanel (SURFHANDLE *hSurf, MESHHANDLE hMesh, MATRIX3 *T, float alpha, bool additive = false);

	// ==================================================================
	/// \name Surface-related methods
	// @{

	/**
	 * \brief Create a surface for texturing, as a blitting source, etc.
	 * \param w surface width [pixels]
	 * \param h surface height [pixels]
	 * \param attrib \ref surfacecaps (bitflags). See notes.
	 * \return Surface handle, or NULL on failure.
	 * \note The attribute flag can contain one of the following main attributes:
	 *  - OAPISURFACE_RO: create a surface that can be read by the GPU pipeline, and that can be updated from system memory.
	 *  - OAPISURFACE_RW: create a surface that can be read and written by the GPU pipeline, and that can be updated from system memory.
	 *  - OAPISURFACE_GDI: create a surface that can be read and written from the CPU, and can be blitted into an uncompressed RO or RW surface without an alpha channel
	 *  In addition, the flag can contain any combination of the following auxiliary attributes:
	 *  - OAPISURFACE_MIPMAPS: create a full chain of mipmaps for the surface if possible
	 *  - OAPISURFACE_NOALPHA: create a surface without an alpha channel
	 */
	SURFHANDLE clbkCreateSurfaceEx (DWORD w, DWORD h, DWORD attrib);

	/**
	 * \brief Create an offscreen surface
	 * \param w surface width [pixels]
	 * \param h surface height [pixels]
	 * \param hTemplate surface format template
	 * \return pointer to surface, cast into a SURFHANDLE, or NULL to
	 *   indicate failure.
	 * \sa clbkCreateTexture, clbkReleaseSurface
	 */
	SURFHANDLE clbkCreateSurface (DWORD w, DWORD h, SURFHANDLE hTemplate = NULL);

	/**
	 * \brief Create a texture for rendering
	 * \param w texture width
	 * \param h texture height
	 * \return pointer to texture, returned as generic SURFHANDLE. NULL
	 *   indicates failure.
	 * \sa clbkCreateSurface, clbkReleaseSurface
	 */
	SURFHANDLE clbkCreateTexture (DWORD w, DWORD h);

	/**
	 * \brief Create an offscreen surface from a bitmap
	 * \param hBmp bitmap handle
	 * \return surface handle, or NULL to indicate failure
	 * \note The reference counter for the new surface is set to 1.
	 * \sa clbkIncrSurfaceRef, clbkReleaseSurface
	 */
	SURFHANDLE clbkCreateSurface (HBITMAP hBmp);

	/**
	 * \brief Increment the reference counter of a surface.
	 * \param surf surface handle
	 * \note This is the client's OWN reference count on SurfNative, which
	 *   Orbiter drives through this API. It is not COM reference counting --
	 *   there is none left in this client.
	 */
	void clbkIncrSurfaceRef (SURFHANDLE surf);

	/**
	 * \brief Decrement surface reference counter, release surface if counter
	 *   reaches 0.
	 * \param surf surface handle
	 * \return true on success
	 * \sa clbkCreateSurface, clbkIncrSurfaceRef
	 */
	bool clbkReleaseSurface (SURFHANDLE surf);

	/**
	 * \brief Return the width and height of a surface
	 * \param[in] surf surface handle
	 * \param[out] w surface width
	 * \param[out] h surface height
	 * \return true if surface dimensions could be obtained.
	 * \sa clbkCreateSurface
	 */
	bool clbkGetSurfaceSize (SURFHANDLE surf, DWORD *w, DWORD *h);

	/**
	 * \brief Set transparency colour key for a surface.
	 * \param surf surface handle
	 * \param ckey transparency colour key value
	 */
	bool clbkSetSurfaceColourKey (SURFHANDLE surf, DWORD ckey);

	/**
	 * \brief Convert an RGB colour triplet into a device-specific colour value.
	 * \param r red component
	 * \param g green component
	 * \param b blue component
	 * \return colour value
	 * \default Packs the RGB values into a DWORD of the form 0x00RRGGBB, with
	 *   8 bits per colour component.
	 * \sa clbkFillSurface
	 */
	DWORD clbkGetDeviceColour (BYTE r, BYTE g, BYTE b);
	// @}

	// ==================================================================
	/// \name Surface blitting methods
	// @{

	/**
	 * \brief Copy one surface into an area of another one.
	 * \param tgt target surface handle
	 * \param tgtx left edge of target rectangle
	 * \param tgty top edge of target rectangle
	 * \param src source surface handle
	 * \param flag blitting parameters (see notes)
	 * \return true on success, false if the blit cannot be performed.
	 * \note By convention, tgt==NULL is valid and refers to the primary render
	 *   surface (e.g. for copying 2-D overlay surfaces).
	 * \note The following bit-flags are defined:
	 *   <table col=2>
	 *   <tr><td>BLT_SRCCOLORKEY</td><td>Use the colour key defined by the source surface for transparency</td></tr>
	 *   <tr><td>BLT_TGTCOLORKEY</td><td>Use the colour key defined by the target surface for transparency</td></tr>
	 *   </table>
	 */
	bool clbkBlt (SURFHANDLE tgt, DWORD tgtx, DWORD tgty, SURFHANDLE src, DWORD flag = 0) const;

	/**
	 * \brief Copy a rectangle from one surface to another.
	 * \param tgt target surfac handle
	 * \param tgtx left edge of target rectangle
	 * \param tgty top edge of target rectangle
	 * \param src source surface handle
	 * \param srcx left edge of source rectangle
	 * \param srcy top edge of source rectangle
	 * \param w width of rectangle
	 * \param h height of rectangle
	 * \param flag blitting parameters (see notes)
	 * \return true on success, false if the blit cannot be performed.
	 */
	bool clbkBlt (SURFHANDLE tgt, DWORD tgtx, DWORD tgty, SURFHANDLE src, DWORD srcx, DWORD srcy, DWORD w, DWORD h, DWORD flag = 0) const;

	/**
	 * \brief Copy a rectangle from one surface to another, stretching or shrinking as required.
	 * \param tgt target surface handle
	 * \param tgtx left edge of target rectangle
	 * \param tgty top edge of target rectangle
	 * \param tgtw width of target rectangle
	 * \param tgth height of target rectangle
	 * \param src source surface handle
	 * \param srcx left edge of source rectangle
	 * \param srcy top edge of source rectangle
	 * \param srcw width of source rectangle
	 * \param srch height of source rectangle
	 * \param flag blitting parameters
	 * \return true on success, false if the blit cannot be performed.
	 */
	bool clbkScaleBlt (SURFHANDLE tgt, DWORD tgtx, DWORD tgty, DWORD tgtw, DWORD tgth,
		                       SURFHANDLE src, DWORD srcx, DWORD srcy, DWORD srcw, DWORD srch, DWORD flag = 0) const;


	/**
	 * \brief Begins a block of blitting operations to the same target surface.
	 * \param tgt Target surface for subsequent blitting calls.
	 * \return Should return an error code (0 on success, the return value from
	 *   the base class call, or a client-specific code)
	 * \note The special target RENDERTGT_MAINWINDOW refers to the main render surface.
	 * \sa clbkEndBltGroup
	 */
	int clbkBeginBltGroup (SURFHANDLE tgt);

	/**
	 * \brief Ends a block of blitting operations to the same target surface.
	 * \return Should return an error code (0 on success, the return value from
	 *   the base class call, or a client-specific code)
	 * \sa clbkBeginBltGroup
	 */
	int clbkEndBltGroup ();

	/**
	 * \brief Fill a surface with a uniform colour
	 * \param surf surface handle
	 * \param col colour value
	 * \return true on success, false if the fill operation cannot be performed.
	 * \sa clbkFillSurface(SURFHANDLE,DWORD,DWORD,DWORD,DWORD,DWORD)
	 */
	bool clbkFillSurface (SURFHANDLE surf, DWORD col) const;

	/**
	 * \brief Fill an area in a surface with a uniform colour
	 * \param surf surface handle
	 * \param tgtx left edge of target rectangle
	 * \param tgty top edge of target rectangle
	 * \param w width of rectangle
	 * \param h height of rectangle
	 * \param col colour value
	 * \return true on success, false if the fill operation cannot be performed.
	 * \sa clbkFillSurface(SURFHANDLE,DWORD)
	 */
	bool clbkFillSurface (SURFHANDLE surf, DWORD tgtx, DWORD tgty, DWORD w, DWORD h, DWORD col) const;


	/**
	 * \brief Copy a bitmap object into a surface
	 * \param pdds surface handle
	 * \param hbm bitmap handle
	 * \param x left edge of source bitmap area to be copied
	 * \param y top edge of source bitmap area to be copied
	 * \param dx width of source bitmap area to be copied
	 * \param dy height of source bitmap area to be copied
	 * \return \e true on success, \e false if surface or bitmap handle are invalid.
	 */
	bool clbkCopyBitmap (SURFHANDLE pdds, HBITMAP hbm, int x, int y, int dx, int dy);
	// @}


	// ==================================================================
	/// \name 2-D drawing interface
	//@{
	/**
	 * \brief Create a 2-D drawing object ("sketchpad") associated with a surface.
	 * \param surf surface handle
	 * \return Pointer to drawing object.
	 * \sa Sketchpad, clbkReleaseSketchpad
	 */
	Sketchpad* clbkGetSketchpad_const(SURFHANDLE surf) const;
	Sketchpad * clbkGetSketchpad (SURFHANDLE surf);

	/**
	 * \brief Release a drawing object.
	 * \param sp pointer to drawing object
	 * \sa Sketchpad, clbkGetSketchpad
	 */
	void clbkReleaseSketchpad_const(Sketchpad* sp) const;
	void clbkReleaseSketchpad (Sketchpad *sp);

	/**
	 * \brief Create a font resource for 2-D drawing.
	 * \param height cell or character height [pixel]
	 * \param prop proportional/fixed width flag
	 * \param face font face name
	 * \param style font decoration style
	 * \param orientation text orientation [1/10 deg]
	 * \return Pointer to font resource
	 * \sa clbkReleaseFont, oapi::Font
	 */
	Font* clbkCreateFont(int height, bool prop, const char* face, FontStyle style = FontStyle::FONT_NORMAL, int orientation = 0) const;
	Font* clbkCreateFontEx(int height, char* face, int width, int weight, FontStyle style, float spacing) const;

	/**
	 * \brief De-allocate a font resource.
	 * \param font pointer to font resource
	 * \sa clbkCreateFont, oapi::Font
	 */
	void clbkReleaseFont (Font *font) const;

	/**
	 * \brief Create a pen resource for 2-D drawing.
	 * \param style line style (0=invisible, 1=solid, 2=dashed)
	 * \param width line width [pixel]
	 * \param col line colour (format: 0xBBGGRR)
	 * \return Pointer to pen resource
	 * \sa clbkReleasePen, oapi::Pen
	 */
	Pen *clbkCreatePen (int style, int width, DWORD col) const;

	/**
	 * \brief De-allocate a pen resource.
	 * \param pen pointer to pen resource
	 * \sa clbkCreatePen, oapi::Pen
	 */
	void clbkReleasePen (Pen *pen) const;

	/**
	 * \brief Create a brush resource for 2-D drawing.
	 * \param col line colour (format: 0xBBGGRR)
	 * \return Pointer to brush resource
	 * \sa clbkReleaseBrush, oapi::Brush
	 */
	Brush *clbkCreateBrush (DWORD col) const;

	/**
	 * \brief De-allocate a brush resource.
	 * \param brush pointer to brush resource
	 * \sa clbkCreateBrush, oapi::Brush
	 */
	void clbkReleaseBrush (Brush *brush) const;
	//@}


	// ==================================================================
	/// \name GDI-related methods
	// @{

	/**
	 * \brief Return a Windows graphics device interface handle for a surface
	 * \param surf surface handle
	 * \return GDI handle, or NULL on failure
	 * \note On Linux this is the display-list recorder in
	 *   Src/Orbiter/Linux/Gdi.cpp: the returned HDC accumulates draw commands
	 *   which orbiter_ReplayDC turns into ImGui draw data. It is a real HDC to
	 *   every caller here; it simply rasterises nothing itself.
	 */
	HDC clbkGetSurfaceDC (SURFHANDLE surf);

	/**
	 * \brief Release a Windows graphics device interface
	 * \param surf surface handle
	 * \param hDC GDI handle
	 */
	void clbkReleaseSurfaceDC (SURFHANDLE surf, HDC hDC);
	// @}

	/**
	 * \brief Filter elevation grid data
	 * \param hPlanet object handle of the planet the data belongs to
	 * \param ilat patch latitude index
	 * \param ilng patch longitude index
	 * \param lvl patch resolution level
	 * \param elev_res elevation level resolution
	 * \param elev pointer to array with elevation grid data
	 */
	bool clbkFilterElevation(OBJHANDLE hPlanet, int ilat, int ilng, int lvl, double elev_res, INT16* elev);
	// @}

	void clbkImGuiNewFrame() override;
	void clbkImGuiRenderDrawData() override;
	void clbkImGuiInit() override;
	void clbkImGuiShutdown() override;
	uint64_t clbkImGuiSurfaceTexture(SURFHANDLE surf) override;

	HWND				GetRenderWindow () const { return hRenderWnd; }
	CVulkanFramework *  GetFramework() const { return pFramework; }
	Scene *             GetScene() const { return scene; }
	MeshManager *       GetMeshMgr() const { return meshmgr; }
	void 				WriteLog (const char *msg) const;
	VulkanDevice *      GetDevice() const { return pDevice; }
	lpSurfNative		GetDefaultTexture() const;
	SURFHANDLE			GetBackBufferHandle() const;
	VulkanTexture *     GetNoiseTex() const { return pNoiseTex; }
	void 				SplashScreen();
	inline bool			IsControlPanelOpen() const { return bControlPanel; }
	inline bool 		IsRunning() const { return bRunning; }

	// Was (pCaps->TextureCaps & D3DPTEXTURECAPS_POW2) &&
	//     (pCaps->TextureCaps & D3DPTEXTURECAPS_NONPOW2CONDITIONAL)
	// -- "this card only does non-power-of-two textures under conditions".
	// Vulkan requires full NPOT support of every conforming implementation,
	// so the condition cannot arise. Kept because callers ask.
	inline bool			IsLimited() const { return false; }

	const FMATRIX4 *	GetIdentity() const { return &ident; }
	HWND 				GetWindow();
	bool 				HasVertexTextureSupport() const { return bVertexTex; }
	const VkPhysicalDeviceProperties *GetHardwareCaps() const { return pCaps; }
	//FileParser *		GetFileParser() const { return parser; }
	VulkanTexture *		GetBackBuffer() const { return pBackBuffer; }
	VulkanTexture *		GetDepthStencil() const { return pDepthStencil; }
	const void *		GetConfigParam (DWORD paramtype) const;
	bool				RegisterRenderProc(__gcRenderProc proc, DWORD id, void *pParam = NULL);
	bool				RegisterGenericProc(__gcGenericProc proc, DWORD id, void *pParam = NULL);
	// const FMATRIX4* rather than LPD3DXMATRIX. The Windows parameters were
	// LPD3DXMATRIX -- non-const -- and every caller passed Scene's
	// GetViewMatrix()/GetProjectionMatrix(), whose `const LPD3DXMATRIX`
	// return type is a const POINTER to a NON-const matrix and which cast the
	// const off their own members to produce it. Scene.h drops those three
	// casts and returns `const FMATRIX4 *`, which is what the callers
	// actually want; this signature follows, since nothing here writes
	// through either pointer and the callback (__gcRenderProc) is handed
	// neither.
	void				MakeRenderProcCall(Sketchpad *pSkp, DWORD id, const FMATRIX4 *pV, const FMATRIX4 *pP);
	void				MakeGenericProcCall(DWORD id, int iUser, void *pUser) const;
	bool				IsGenericProcEnabled(DWORD id) const;
	void				SetScenarioName(const std::string &path) { scenarioName = path; };
	void				HackFriendlyHack();
	void				PickTerrain(DWORD uMsg, int xpos, int ypos);
	DEVMESHHANDLE		GetDevMesh(MESHHANDLE hMesh);

	// hMainThread = GetCurrentThread(), carried across verbatim.
	//
	// GetCurrentThread() returns a PSEUDO-HANDLE -- the constant (HANDLE)-2,
	// meaning "whichever thread is asking" -- not a handle identifying a
	// particular thread. So both users compare that constant against itself:
	//
	//   D3D9Client.cpp:3130   if (GetCurrentThread() != hMainThread)  never true
	//   D3D9Surface.cpp:494   assert(GetCurrentThread() == ...)       never fails
	//
	// Two thread-affinity guards that have never caught anything, and CANNOT.
	//
	// AN EARLIER VERSION OF THIS PORT "FIXED" THEM, and that was a mistake
	// worth recording. It reasoned that both were asking "am I on the main
	// thread", which is a question about thread IDs, and so returned a real
	// one from GetCurrentThreadId(). That makes the guards LIVE on Linux and
	// only on Linux: Src/Orbiter/Linux/Platform.cpp implements
	// GetCurrentThreadId over pthread_self, so the surface assert fails for
	// every surface built off the main thread -- which is exactly what
	// TileLoader::Load_ThreadProc does on every tile it loads -- and the
	// Sketchpad guard can reach HALT() from a worker thread. A dormant
	// Windows check became a Linux-only abort.
	//
	// So the shim grew GetCurrentThread() returning the same pseudo-handle
	// Windows returns (see Src/Orbiter/Linux/windows.h), and this is the
	// reference's accessor unchanged. The guards stay dead here exactly as
	// they are dead there. Repairing them is a change to the client, not a
	// conversion of it, and belongs in a separate decision.
	HANDLE				GetMainThread() const { return hMainThread; }


	// ==================================================================
	//
	HRESULT				BeginScene();
	void				EndScene();
	bool				IsInScene() const { return bRendering; }
	void				PushSketchpad(SURFHANDLE surf, VulkanPad* pSkp) const;
	void				PushRenderTarget(VulkanTexture *pColor, VulkanTexture *pDepthStencil = NULL, int code = 0) const;
	void				AlterRenderTarget(VulkanTexture *pColor, VulkanTexture *pDepthStencil = NULL);
	void				PopRenderTargets() const;
	VulkanTexture *     GetTopDepthStencil();
	VulkanTexture *     GetTopRenderTarget();
	class VulkanPad *	GetTopInterface() const;


protected:

	/** \brief Launchpad video tab indicator
	 * \return true if the module wants to use the video tab in the launchpad
	 *   dialog, false otherwise.
	 */
	bool clbkUseLaunchpadVideoTab () const;

	/**
	 * \brief Simulation session start notification
	 *
	 * Called at the beginning of a simulation session to allow the client
	 * to create the 3-D rendering window (or to switch into fullscreen
	 * mode).
	 * \return Should return window handle of the rendering window.
	 * \note On Linux the window already exists -- UIHost.cpp created it for
	 *   the Launchpad -- so this adopts it through orbiter_GetRenderWindow()
	 *   rather than opening one. See VulkanFrame.h.
	 */
	HWND clbkCreateRenderWindow ();

	/**
	 * \brief Simulation startup finalisation
	 *
	 * Called at the beginning of a simulation session after the scenario has
	 * been parsed and the logical objects have been created.
	 */
	void clbkPostCreation ();

	/**
	 * \brief End of simulation session notification
	 *
	 * Called before the end of a simulation session. At the point of call,
	 * logical objects still exist (OBJHANDLEs valid), and external modules
	 * are still loaded.
	 * \param fastclose Indicates a "fast shutdown" request
	 * \sa clbkDestroyRenderWindow
	 */
	void clbkCloseSession (bool fastclose);

	/**
	 * \brief Render window closure notification
	 *
	 * Called at the end of a simulation session. At the point of call, all
	 * logical simulation objects have been destroyed, and object modules have
	 * been unloaded. This method should not access any OBJHANDLE or VESSEL
	 * objects any more.
	 * \param fastclose Indicates a "fast shutdown" request
	 * \sa clbkCloseSession
	 */
	void clbkDestroyRenderWindow (bool fastclose);

	/**
	 * \brief Per-frame update notification
	 *
	 * Called once per frame, after the logical world state has been updated,
	 * but before clbkRenderScene().
	 * \param running true if simulation is running, false if paused.
	 * \note Unlike clbkPreStep and clbkPostStep, this method is also called
	 *   while the simulation is paused.
	 */
	void clbkUpdate (bool running);

	/**
	 * \brief Per-frame render notification
	 *
	 * Called once per frame, after the logical world state has been updated,
	 * to allow the client to render the current scene.
	 * \note After the 3D scene has been rendered, this function should call
	 *   \ref Render2DOverlay to initiate rendering of 2D elements (2D
	 *   instrument panel, HUD, etc.)
	 */
	void clbkRenderScene ();

	/**
	 * \brief React to a discontinous jump in simulation time.
	 * \param simt new simulation time relative to session start [s]
	 * \param simdt jump interval [s]
	 * \param mjd new absolute simulation time in MJD format [days]
	 * \note Currently, this method does nothing.
	 */
	void clbkTimeJump (double simt, double simdt, double mjd);

	/**
	 * \brief Display a scene on screen after rendering it.
	 * \return Should return true on successful operation, false on failure or
	 *   if no operation was performed.
	 */
	bool clbkDisplayFrame ();

	/**
	 * \brief Display a load status message on the splash screen
	 * \param msg Pointer to load status message string
	 * \param line message line to be displayed (0 or 1)
	 * \return Should return true if it displays the message, false if not.
	 */
	bool clbkSplashLoadMsg (const char *msg, int line);

	void clbkSetSplashScreen(const char *filename, DWORD textCol) override;

	/**
	 * \brief Store a persistent mesh template
	 * \param hMesh mesh handle
	 * \param fname mesh file name
	 * \note the mesh templates loaded with \ref oapiLoadMeshGlobal are shared
	 *   between all vessel instances and should never be edited.
	 */
	void clbkStoreMeshPersistent (MESHHANDLE hMesh, const char *fname);

	/**
	 * \brief Renders the fullscreen viewport in the presence of popup windows.
	 * \return \e true if render window has been updated and no more page flipping
	 *   is required, \e false if page still needs to be flipped.
	 */
	bool RenderWithPopupWindows ();

public:

	/**
	 * \brief Displays a message on the splash screen.
	 * \param msg Message to be displayed.
	 * \param line line number (0 or 1)
	 * \return true if load status was initialised, false if not.
	 * \sa clbkSplashLoadMsg
	 */
	bool OutputLoadStatus (const char *msg, int line);

private:
	// The scene hook the host calls back on. See the long note above
	// RenderSceneWork in VulkanClient.cpp: the only command buffer that
	// reaches the swapchain exists for the duration of this callback, so the
	// whole of what the reference does in clbkRenderScene happens here.
	static void SceneRenderThunk(void *cmdBuf, void *renderPass,
								 unsigned width, unsigned height, void *user);
	void RenderSceneWork(VkCommandBuffer cmd, unsigned width, unsigned height);

	void BltError(SURFHANDLE src, SURFHANDLE tgt, const LPRECT s, const LPRECT t, bool bHalt = true) const;
	void SketchPadTest();
	void PresentScene();
	void Label(const char *format, ...);
	void DrawTimeBar(double t, double scale, double frames, DWORD color, const char *label=NULL);
	bool ChkDev(const char *fnc) const;

	SURFHANDLE				pBltGrpTgt;
	VulkanPad*				pBltSkp;


	VulkanDevice *			pDevice;
	lpSurfNative			pDefaultTex;
	lpSurfNative			pScatterTest;
	VulkanTexture *			pNoiseTex;
	// WERE LPDIRECT3DSURFACE9 FROM CreateOffscreenPlainSurface; THEY ARE
	// CLIENT SURFACES NOW, AND HAVE TO BE.
	//
	// The two splash-screen surfaces are written with GDI -- SplashScreen()
	// and OutputLoadStatus() both do GetDC / TextOut / ReleaseDC on them --
	// and drawn to the screen with StretchRect. A bare VulkanTexture answers
	// neither: GetDC is SurfNative's (see VulkanSurface.h on why it exists at
	// all here), and the blit has to go through the Sketchpad because the
	// back buffer is an attachment proxy with no image. A SurfNative is
	// exactly the object that has both, so these become client surfaces
	// rather than raw images. Everything else about their use is unchanged.
	lpSurfNative			pSplashScreen;
	lpSurfNative			pTextScreen;
	VulkanTexture *			pBackBuffer;
	VulkanTexture *			pDepthStencil;
	CVulkanFramework *		pFramework;
	const VkPhysicalDeviceProperties *pCaps;
	std::string				scenarioName;
	HANDLE					hMainThread;
	WindowManager *			pWM;
	const char *            pCustomSplashScreen;
	DWORD                   pSplashTextColor;

	HWND hRenderWnd;        // render window handle

	bool bControlPanel;
	bool bScatterUpdate;
	bool bFullscreen;       // fullscreen render mode flag
	bool bAAEnabled;
	bool bFailed;
	bool bRunning;
	bool bVertexTex;
	bool bVSync;
	bool bRendering;
	bool bGDIClear;

	DWORD viewW, viewH;     // dimensions of the render viewport
	DWORD viewBPP;          // bit depth of render viewport
	DWORD frame_timer;

	// device enumeration callback function

	VideoTab *vtab;			// video selection user interface
	Scene *scene;           // Scene description

	MeshManager *meshmgr;   // mesh manager
	FMATRIX4 ident;

	struct RenderProcData;
	struct GenericProcData;

	std::vector<RenderProcData> RenderProcs;
	std::vector<GenericProcData> GenericProcs;
	std::vector<SURFHANDLE> ImTextures;
	mutable std::list<RenderTgtData> RenderStack;

	HFONT hLblFont1;
	HFONT hLblFont2;

	// THE COUNTERPART OF CD3DFramework9's pLargeFont, MOVED HERE.
	//
	// The framework created two LPD3DXFONTs and handed them out with
	// GetLargeFont()/GetSmallFont(). D3DXCreateFontIndirect wraps GDI glyph
	// rasterisation, and Src/Orbiter/Linux/Gdi.cpp is a display-list RECORDER
	// rather than a rasteriser -- there are no glyph pixels to blit -- so
	// neither survives as a D3DX font. The small one was never used at all.
	// The large one has exactly three call sites, all in
	// VulkanClient::clbkRenderScene's debug overlay ("Record", "Replay",
	// "Frozen"), so it becomes an ordinary client font drawn through the
	// client's own Sketchpad, with the same parameters the framework asked
	// D3DX for: 30px, bold, Arial.
	oapi::Font *pOverlayFont;

	char pLoadLabel[128];
	char pLoadItem[128];

	// Control Panel
	void RenderControlPanel();
	bool ControlPanelMsg(WPARAM wParam);

	Sketchpad *pItemsSkp;

	DWORD loadd_x, loadd_y, loadd_w, loadd_h;
	int LabelPos;

}; // class VulkanClient


// ======================================================================
// class VisObject
// ======================================================================
/**
 * \brief Visual object representation.
 *
 * A VisObject is the visual representation of an Orbiter object (vessel,
 * planet, etc.). The 'logical' object representation resides in the Orbiter
 * core, while its 'visual' representation is located in the graphics client.
 *
 * Visual representations should be non-permanent: they should be created
 * when the object enters the visual range of the camera, and deleted when
 * they leave it.
 *
 * Only a single VisObject instance should be created per object, even if
 * the visual is present in multiple views.
 */
class VisObject {
public:
	/**
	 * \brief Creates a visual for object hObj.
	 * \param hObj object handle
	 * \sa oapi::GraphicsClient::RegisterVisObject
	 */
	explicit VisObject (OBJHANDLE hObj);

	/**
	 * \brief Destroys the visual.
	 * \sa oapi::GraphicsClient::UnregisterVisObject
	 */
	virtual ~VisObject ();

	/**
	 * \brief Returns the object handle associated with the visual.
	 * \return Object handle
	 */
	OBJHANDLE GetObject () const { return hObj; }

	/**
	 * \brief Message callback.
	 * \param event message identifier
	 * \param context message content (message-specific)
	 * \note For currently supported event types, see \ref visevent.
	 */
	virtual void clbkEvent (DWORD event, DWORD_PTR context) {}

protected:
	OBJHANDLE hObj;	///< Object handle associated with the visual
};

}; // namespace oapi


// ======================================================================
// class RenderState DID NOT CONVERT.
//
// It stood here, capturing thirteen pieces of D3D9 device render state with
// GetRenderState so that the Sketchpad could put them back after drawing over
// a scene, and it is used twice, both in D3D9Pad.cpp.
//
// Vulkan has no device render state to capture. ALPHABLENDENABLE, ZENABLE,
// ZWRITEENABLE, CULLMODE, COLORWRITEENABLE, FILLMODE, STENCILENABLE,
// ALPHATESTENABLE, BLENDOP, SRCBLEND and DESTBLEND are all immutable
// VkPipeline state, fixed at pipeline creation; SCISSORTESTENABLE and the
// scissor rect are dynamic state set per command buffer. A draw cannot
// inherit another draw's state because a draw brings its own pipeline, so
// there is nothing to save and nothing to restore -- VulkanPad binds the
// pipeline it wants.
//
// And the Windows version was already inert: D3D9Frame.cpp sets
// D3DCREATE_PUREDEVICE unconditionally (Pure = true in Clear(), never
// cleared), and a pure device fails GetRenderState. Capture() was logging
// thirteen HR errors and storing nothing; Restore() was putting that nothing
// back.
// ======================================================================

#endif // !__VULKANCLIENT_H
