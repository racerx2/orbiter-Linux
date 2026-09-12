// ==============================================================
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2006-2026 Martin Schweiger
//				 2012-2016 Jarmo Nikkanen
// ==============================================================

// ==============================================================
// Class Scene (interface)
//
// A "Scene" represents the 3-D world as seen from a specific
// viewpoint ("camera"). Each scene therefore has a camera object
// associated with it. The Orbiter core supports a single
// camera, but in principle a graphics client could define
// multiple scenes and render them simultaneously into separate
// windows (or into MFD display surfaces, etc.)
// ==============================================================
//
// CelSphere.h is forward-declared rather than included: the Windows header
// includes it for one member, m_celSphere, which is a pointer.
// ==============================================================

#ifndef __SCENE_H
#define __SCENE_H

#include "VulkanClient.h"
#include "VulkanEffect.h"
#include "VObject.h"
#include <stack>
#include <list>
#include <set>
#include <map>

class vObject;
class vPlanet;
class vVessel;
class VulkanParticleStream;
class VulkanText;
class VulkanPad;
class VulkanCelestialSphere;
class SurfNative;

#define GBUF_COLOR				0
#define GBUF_BLUR				1
#define GBUF_TEMP				2
#define GBUF_DEPTH				3
#define GBUF_GDI				4
#define GBUF_COUNT				5	// Buffer count

#define SHM_LOD_COUNT			5

#define TEX_NOISE				0
#define TEX_CLUT				1
#define TEX_COUNT				2

#define RENDERPASS_UNKNOWN		0x0000
#define RENDERPASS_MAINSCENE	0x0001
#define RENDERPASS_ENVCAM		0x0002
#define RENDERPASS_CUSTOMCAM	0x0003
#define RENDERPASS_SHADOWMAP	0x0004
#define RENDERPASS_PICKSCENE	0x0005
#define RENDERPASS_SKETCHPAD	0x0006
#define RENDERPASS_MAINOVERLAY	0x0007
#define RENDERPASS_NORMAL_DEPTH	0x0008

// Two impossible pointer values passed where a render target is expected, to
// mean "put the previous one back" and "leave it alone". Only the cast's type
// changed.
#define RESTORE ((VulkanTexture*)(-1))
#define CURRENT ((VulkanTexture*)(-2))

#define RENDERTURN_ENVCAM		0
#define RENDERTURN_CUSTOMCAM	1
#define RENDERTURN_IRRADIANCE   2
#define RENDERTURN_LAST			2

#define SMAP_MODE_FOCUS			1
#define SMAP_MODE_SCENE			2

#define OBJTP_BUILDING			1000

#define CAMERA(x) ((Scene::CAMREC*)x)

class Scene {

	friend class VulkanCelestialSphere;

	// Visual record ===================================================================
	//
	struct VOBJREC {           // linked list of object visuals
		vObject *vobj;         // visual instance
		int	type;
		float apprad;
		VOBJREC *prev, *next;  // previous and next list entry
	} *vobjFirst, *vobjLast;   // first and last list entry


public:

	FVECTOR3 vPickRay;

	struct FRUSTUM {
		float znear;
		float zfar;
	};

	// Custom camera parameters ========================================================
	//
	struct CAMREC {
		MATRIX3		mRotation;
		VECTOR3		vPosition;
		double		dAperture;
		SURFHANDLE	hSurface;
		OBJHANDLE	hVessel;
		DWORD		dwFlags;
		int			iError;
		bool		bActive;
		__gcRenderProc pRenderProc;
		void*		pUser;
		float       fSurfLabelScale;
	};

	std::set<CAMREC*> CustomCams;
	std::set<CAMREC*>::const_iterator camCurrent{};

	// Camera frustum parameters ========================================================
	//
	struct CAMERA {
		float		aperture;   // aperture [rad]
		float		aspect;     // aspect ratio
		float		nearplane;  // frustum nearplane distance
		float		farplane;   // frustum farplane distance
		float		apsq;
		float		vh, vw, vhf, vwf;

		VECTOR3		pos;		// Global camera position
		VECTOR3		relpos;		// Relative camera position (Used by Mesh Debugger)
		VECTOR3		dir;		// Camera direction

		FVECTOR3	x;			// Camera axis vector
		FVECTOR3	y;			// Camera axis vector
		FVECTOR3	z;			// Camera axis vector
		FVECTOR3	upos;		// Camera position unit vector

		FMATRIX4	mView;		// view matrix for current camera state
		FMATRIX4	mProj;		// projection matrix for current camera state
		FMATRIX4	mProjView;	// combined projection view matrix
		FMATRIX4	mProjViewInf; // combined projection view matrix, far plane at infinity
		OBJHANDLE	hTarget;	// Current camera target, Mesh Debugger Related

		OBJHANDLE	hObj_proxy;	// closest celestial body
		vPlanet *	vProxy;		// closest celestial body (visual)
		double		alt_proxy;	// camera distance to surface of hObj_proxy

		OBJHANDLE	hNear;		// closest celestial body
		vPlanet *	vNear;		// closest celestial body (visual)

		OBJHANDLE	hGravRef;	// closest celestial body
		vObject*	vGravRef;	// closest celestial body (visual)

		double		alt_near;
		double		lng, lat, elev;

		// Pixel viewport associated with the current camera.
		// Uses main scene viewport when zero
		DWORD viewportW;
		DWORD viewportH;
		// Scale for 2D Labels
		float labelScale;
	};

	// Screen space sun visual parameters ==================================================
	//
	struct SUNVISPARAMS {
		float		brightness;
		bool		visible;
		FVECTOR2	position;
		FVECTOR4	color;
	};

	struct SHADOWMAPPARAM {
		VulkanTexture *pShadowMap;
		FMATRIX4	mProj, mView, mViewProj;
		FVECTOR3	pos;
		FVECTOR3	ld;
		float		rad;
		float		dist;
		float		depth;
		int			lod;
		int			size;
	} smap;

	static void VulkanTechInit(VulkanDevice *pDev, const char *folder);

	/**
	 * \brief Release global parameters
	 */
	static void GlobalExit();

	Scene (oapi::VulkanClient *_gc, DWORD w, DWORD h);
	~Scene ();

	/**
	 * \brief Get a pointer to the client
	 */
	//inline const oapi::VulkanClient *GetClient() const { return gc; }
	inline oapi::VulkanClient *GetClient() const { return gc; }

	void OnOptionChanged(int cat, int item);

	const VulkanSun *GetSun() const { return &sunLight; }
	const VulkanLight *GetLight(int index) const;
	const VulkanLight *GetLights() const { return Lights; }
	DWORD GetLightCount() const { return nLights; }
	VulkanPad* GetPooledSketchpad(int id);
	void RecallDefaultState();
	float GetDisplayScale() const { return fDisplayScale; }
	void CreateSunGlare();


	DWORD GetRenderPass() const;
	DWORD GetRenderFlags() const { return RenderFlags; }
	void BeginPass(DWORD dwPass);
	void PopPass();

	inline DWORD GetStencilDepth() const { return stencilDepth; }
	inline const SHADOWMAPPARAM * GetSMapData() const { return &smap; }
	/**
	 * \brief Get the ambient background colour
	 */
	inline DWORD GetBgColour() const { return bg_rgba; }

	/**
	 * \brief Get the viewport dimension (width)
	 */
	inline DWORD ViewW() const { return viewW; }

	/**
	 * \brief Get the viewport dimension (height)
	 */
	inline DWORD ViewH() const { return viewH; }
	// The `const DWORD` return on those two is dropped: a top-level const on a
	// by-value return does nothing and GCC reports it.

	bool UpdateCamVis();
	void Initialise ();

	/**
	 * \brief Update camera position, visuals, etc.
	 */
	void Update();

	/**
	 * \brief Render the whole main scene
	 */
	void RenderMainScene();

	/**
	 * \brief Returns screen space sun visual parameters for Lens Flare rendering.
	*/
	SUNVISPARAMS GetSunScreenVisualState();

	/**
	 * \brief Gets sun diffuse colour (accounting for atmospheric shift)
	 */
	FVECTOR4 GetSunDiffColor();

	/**
	 * \brief Render a secondary scene. (Env Maps, Shadow Maps, MFD Camera Views)
	 */
	void RenderSecondaryScene(std::set<class vVessel*> &RndList, std::set<class vVessel*> &AdditionalLightsList, DWORD flags = 0xFF);
	int RenderShadowMap(FVECTOR3 &pos, FVECTOR3 &ld, float rad, bool bInternal = false, bool bListExists = false);

	// pSrc was LPDIRECT3DCUBETEXTURE9 and pOut LPDIRECT3DTEXTURE9. A cube map
	// is an image with six array layers and a CUBE view, not a distinct
	// interface, so both are VulkanTexture*.
	bool IntegrateIrradiance(vVessel *vV, VulkanTexture *pSrc, VulkanTexture *pOut);
	bool RenderBlurredMap(VulkanDevice *pDev, VulkanTexture *pSrc);
	void RenderMesh(DEVMESHHANDLE hMesh, const oapi::FMATRIX4 *pWorld);

	VulkanTexture *GetIrradianceDepthStencil() const { return pIrradDS; }
	VulkanTexture *GetEnvDepthStencil() const { return pEnvDS; }
	VulkanTexture *GetBuffer(int id) const { return psgBuffer[id]; }
	VulkanTexture *GetSunTexture() const { return pSunTex; }
	VulkanTexture *GetSunGlareAtm() const { return pSunGlareAtm; }

	/**
	 * \brief Render any shadows cast by vessels on planet surfaces
	 * \param hPlanet handle of planet to cast shadows on
	 * \param depth shadow darkness parameter (0=none, 1=black)
	 * \note Uses stencil buffering if available and requested. Otherwise shadows
	 *   are pure black.
	 * \note Requests for any planet other than that closest to the camera
	 *   are ignored.
	 */
	void RenderVesselShadows(OBJHANDLE hPlanet, float depth) const;

	/**
	 * \brief Create a visual for a new vessel if within visual range.
	 * \param hVessel vessel object handle
	 */
	void NewVessel (OBJHANDLE hVessel);

	/**
	 * \brief Delete a vessel visual prior to destruction of the logical vessel.
	 * \param hVessel vessel object handle
	 */
	void DeleteVessel (OBJHANDLE hVessel);

	void AddParticleStream (class VulkanParticleStream *_pstream);
	void DelParticleStream (DWORD idx);

	void AddLocalLight(const LightEmitter *le, const vObject *vo);
	void ClearLocalLights();

	/**
	 * \brief Get object radius in pixels using oapiCameraGlobalPos()
	 * \param hObj object handle
	 */
	double GetObjectAppRad(OBJHANDLE hObj) const;

	/**
	 * \brief Get object radius in pixels using a custom camera location.
	 * \param hObj object handle
	 */
	double GetObjectAppRad2(OBJHANDLE hObj) const;

	// Picking Functions ============================================================================================================
	//
	FVECTOR3		GetPickingRay(short x, short y);
	VulkanPick		PickScene(short xpos, short ypos);
	TILEPICK		PickSurface(short xpos, short ypos);
	VulkanPick		PickMesh(DEVMESHHANDLE hMesh, const FMATRIX4 *pW, short xpos, short ypos);

	void			ClearOmitFlags();
	bool			IsRendering() const { return bRendering; }


	// Custom Camera Interface ======================================================================================================
	//
	CAMERAHANDLE	SetupCustomCamera(CAMERAHANDLE hCamera, OBJHANDLE hVessel, MATRIX3 &mRot, VECTOR3 &pos, double fov, SURFHANDLE hSurf, DWORD flags);
	int				DeleteCustomCamera(CAMERAHANDLE hCamera);
	void			DeleteAllCustomCameras();
	void			CustomCameraOnOff(CAMERAHANDLE hCamera, bool bOn);
	void 			SetCustomCameraSurfaceLabelScale(CAMERAHANDLE hCamera, float scale);
	void			RenderCustomCameraView(CAMREC *cCur);


	// Camera Matrix Access =========================================================================================================
	//
	// The three getters returned `const LPD3DXMATRIX` -- a const pointer to a
	// non-const matrix -- and each cast the const away from its own member to
	// produce it. `const FMATRIX4 *` removes those three casts.
	void				GetAdjProjViewMatrix(FMATRIX4 *mP, float znear, float zfar);
	const FMATRIX4 *	GetProjectionViewMatrix() const { return &Camera.mProjView; }
	const FMATRIX4 *	GetProjectionMatrix() const { return &Camera.mProj; }
	const FMATRIX4 *	GetViewMatrix() const { return &Camera.mView; }


	// Main Camera Interface =========================================================================================================
	//
	void			SetCameraAperture(float _ap, float _as);
	void			SetCameraFrustumLimits(double nearlimit, double farlimit);
	float			GetDepthResolution(float dist) const;
	float			CameraInSpace() const;

					// Acquire camera information from the Orbiter and initialize internal camera setup
	bool			UpdateCameraFromOrbiter(DWORD dwPass);

					// Manually initialize client's internal camera setup
	bool			SetupInternalCamera(FMATRIX4 *mView, VECTOR3 *pos, double apr, double asp);

					// Pan Camera in a mesh debugger
	bool			CameraPan(VECTOR3 pan, double speed);

					// Check if a sphere located in pCnt (relative to cam) with a specified radius is visible in a camera
	bool			IsVisibleInCamera(const FVECTOR3 *pCnt, float radius);
	bool			IsProxyMesh();
	bool            CameraDirection2Viewport(const VECTOR3 &dir, int &x, int &y);
	double			GetTanAp() const { return tan(Camera.aperture); }
	float			GetCameraAspect() const { return (float)Camera.aspect; }
	float			GetCameraFarPlane() const { return Camera.farplane; }
	float			GetCameraNearPlane() const { return Camera.nearplane; }
	float			GetCameraAperture() const { return (float)Camera.aperture; }
	VECTOR3			GetCameraGPos() const { return Camera.pos; }
	VECTOR3			GetCameraGDir() const { return Camera.dir; }
	OBJHANDLE		GetCameraProxyBody() const { return Camera.hObj_proxy; }
	vPlanet *		GetCameraProxyVisual() const { return Camera.vProxy; }
	double			GetCameraAltitude() const { return Camera.alt_proxy; }
	OBJHANDLE		GetCameraNearBody() const { return Camera.hNear; }
	vPlanet *		GetCameraNearVisual() const { return Camera.vNear; }
	double			GetCameraNearAltitude() const { return Camera.alt_near; }
	double			GetCameraElevation() const { return Camera.elev; }
	void			GetCameraLngLat(double *lng, double *lat) const;
	bool			WorldToScreenSpace(const VECTOR3& rdir, oapi::IVECTOR2* pt, FMATRIX4* pVP = NULL, float clip = 1.0f);
	bool			WorldToScreenSpace2(const VECTOR3& rdir, oapi::FVECTOR2* pt, FMATRIX4* pVP = NULL, float clip = 1.0f);

	DWORD			GetFrameId() const { return dwFrameId; }

	const FVECTOR3 *GetCameraX() const { return &Camera.x; }
	const FVECTOR3 *GetCameraY() const { return &Camera.y; }
	const FVECTOR3 *GetCameraZ() const { return &Camera.z; }

	const CAMERA *	GetCamera() const { return &Camera; }

	void			PushCamera();	// Push current camera onto a stack
	void			PopCamera();	// Restore a camera from a stack
	FMATRIX4		PushCameraFrustumLimits(float nearlimit, float farlimit);
	FMATRIX4		PopCameraFrustumLimits();



	// Visual Management =========================================================================================================
	//
	void			GetLVLH(vVessel *vV, FVECTOR3 *up, FVECTOR3 *nr, FVECTOR3 *cp);
	class vObject *	GetVisObject(OBJHANDLE hObj) const;
	class vVessel *	GetFocusVisual() const { return vFocus; }
	void			CheckVisual(OBJHANDLE hObj);
	double			GetFocusGroundAltitude() const;
	double			GetTargetGroundAltitude() const;
	double			GetTargetElevation() const;
	std::set<vVessel *> GetVessels(double max_dst, bool bActive = true);

	// Locate the visual for hObj in the list if present, or return
	// NULL if not found

protected:

	/**
	 * \brief Render a single marker at a given global position
	 * \param pSkp sketchpad
	 * \param gpos global position (ecliptic frame)
	 * \param label1 label above marker
	 * \param label2 label below marker
	 * \param mode marker shape
	 * \param scale marker size
	 */
	void RenderObjectMarker(oapi::Sketchpad *pSkp, const VECTOR3 &gpos, const std::string& label1, const std::string& label2, int mode, int scale);

	void RenderGlares();

private:
	void		ComputeLocalLightsVisibility();
	DWORD		GetActiveParticleEffectCount();
	float		ComputeNearClipPlane();
	void		VisualizeCubeMap(VulkanTexture *pCube, int mip);
	VOBJREC *	FindVisual (OBJHANDLE hObj) const;
	void		RenderVesselMarker(vVessel *vV, VulkanPad *pSketch);

	// Locate the visual for hObj in the list if present, or return
	// NULL if not found

	void DelVisualRec (VOBJREC *pv);
	void DeleteAllVisuals();
	// Delete entry pv from the list of visuals

	VOBJREC *AddVisualRec (OBJHANDLE hObj);
	// Add an entry for object hObj in the list of visuals

	VECTOR3 SkyColour ();
	// Sky background colour based on atmospheric parameters of closest planet

	void InitGDIResources();
	void ExitGDIResources();

	void FreePooledSketchpads();      ///< Release pooled Sketchpad instances

	void RenderLabelsForCustomCamera();
	Font* GetOrCreateLabelFont(int size);



	// Scene variables ================================================================
	//
	oapi::VulkanClient* gc;
	VulkanDevice *pDevice;     // render device
	DWORD viewW, viewH;        // render viewport size
	DWORD stencilDepth;        // stencil buffer bit depth
	VulkanCelestialSphere* m_celSphere; // celestial sphere background
	DWORD iVCheck;             // index of last object checked for visibility
	bool  bLocalLight;         // enable local light sources
	bool  surfLabelsActive;    // v.2 surface labels activated?

	OBJHANDLE hSun;

	VulkanParticleStream **pstream; // list of particle streams
	DWORD                  nstream; // number of streams


	DWORD bg_rgba;             // ambient background colour

	// GDI resources ====================================================================
	//
	oapi::Font *label_font[4];
	std::map<int, oapi::Font*> labelFontCache;

	std::list<vVessel *> RenderList;
	std::list<vVessel *> SmapRenderList;
	std::list<vVessel *> Casters;
	std::stack<CAMERA>	CameraStack;
	std::stack<DWORD>	PassStack;
	std::stack<FRUSTUM> FrustumStack;


	CAMERA		Camera;
	VulkanLight* Lights;
	VulkanSun	sunLight;

	VECTOR3		sky_color;
	double      bglvl;

	float		fDisplayScale;
	float		lmaxdst2;
	DWORD		nLights;
	DWORD		nplanets;		// Number of distance sorted planets to render
	DWORD		dwTurn;
	DWORD		dwFrameId;
	DWORD		camIndex;
	DWORD		RenderFlags;
	bool		bRendering;

	oapi::Font *pAxisFont;
	oapi::Font *pLabelFont;
	oapi::Font *pDebugFont;

	SurfNative *pLblSrf;

	class ImageProcessing *pLightBlur, *pBlur, *pGDIOverlay, *pIrradiance, *pVisDepth, *pCreateGlare;
	class ShaderClass *pLocalCompute, *pRenderGlares;

	class vVessel *vFocus;
	VOBJREC *vobjEnv, *vobjIrd;
	double dVisualAppRad;

	FVECTOR2 DepthSampleKernel[57];

	VulkanTexture *pSunTex, *pLightGlare, *pSunGlare, *pSunGlareAtm;
	VulkanTexture *pLocalResults;
	VulkanTexture *pLocalResultsSL;

	// Blur Sampling Kernel ==============================================================
	//
	// pBlrTemp and pIrradTemp were cube textures; one Vulkan type covers both,
	// so they now differ from the 2D maps by how they were created rather than
	// by declared type.
	VulkanTexture *pBlrTemp[5];
	VulkanTexture *pIrradTemp;
	VulkanTexture *pIrradTemp2, *pIrradTemp3;

	// Deferred Experiment ===============================================================
	//
	// psgBuffer/ptgBuffer were a SURFACE and a TEXTURE for each G-buffer: the
	// same storage reached through two interfaces, because a D3D9 texture
	// could not be bound as a render target without GetSurfaceLevel(0). One
	// VkImage is both, so the two arrays are now the same five images. Both
	// names are kept because Scene.cpp spells one or the other at ~80 sites.
	VulkanTexture *psgBuffer[GBUF_COUNT];
	VulkanTexture *ptgBuffer[GBUF_COUNT];
	VulkanTexture *pOffscreenTarget;
	VulkanTexture *pTextures[TEX_COUNT];

	VulkanTexture *pEnvDS, *pIrradDS, *pDepthNormalDS;
	VulkanTexture *psShmDS[SHM_LOD_COUNT];
	VulkanTexture *psShmRT[SHM_LOD_COUNT];
	VulkanTexture *ptShmRT[SHM_LOD_COUNT];

	LocalLightsCompute LLCBuf[MAX_SCENE_LIGHTS + 1];

	// Rendering Technique related parameters ============================================
	//
	// FX is this class's own effect, separate from VulkanEffect::FX: it loads
	// SceneTech.fx, which holds LineTech, StarTech and LabelTech. eLine and
	// eStar name techniques and are TECHHANDLE; the other three name
	// parameters and stay HANDLE.
	static VulkanEffectFile	*FX;
	static TECHHANDLE	eLine;
	static TECHHANDLE	eStar;
	static HANDLE		eWVP;
	static HANDLE		eColor;
	static HANDLE		eTex0;

};

#endif // !__SCENE_H
