// ==============================================================
// Scene.cpp
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2007-2026 Martin Schweiger
//				 2012 - 2016 Jarmo Nikkanen
// ==============================================================
//
// CONVERTED FROM OVP/D3D9Client/Scene.cpp, read end to end (3929 lines).
//
// The scene: the camera, the visual list, the render passes, the shadow maps,
// the environment maps, the glare pipeline and the post-processing chain. It
// is the largest file in the client and the one that touches the device most,
// so the notes below are the ones that recur; the rest are beside the code.
//
//  1. THE THREE ARRAY PAIRS ARE ONE SET OF IMAGES EACH, and this is the first
//     thing that can go wrong. Scene.h explains why psgBuffer/ptgBuffer,
//     psShmRT/ptShmRT and pLocalResultsSL/pLocalResults each became the SAME
//     VulkanTexture reached through two names: a D3D9 texture could not be
//     bound as a render target without GetSurfaceLevel(0), so the client kept
//     a SURFACE and a TEXTURE for every G-buffer. A VkImage is both.
//
//     SO THE DESTRUCTOR MUST NOT RELEASE BOTH. The Windows destructor calls
//     SAFE_RELEASE on each array in turn, which is correct there -- two
//     reference counts -- and would be a double free here. Each pair is
//     destroyed once and both names are cleared.
//
//  2. GetSurfaceLevel(0) DISAPPEARS EVERYWHERE, for the same reason, and with
//     it every SAFE_RELEASE of the surface it produced. Where the reference
//     fetches a surface to render into, the texture IS the render target.
//
//  3. CreateDepthStencilSurface BECOMES CreateTexture WITH
//     DEPTH_STENCIL_ATTACHMENT USAGE. D3DFMT_D24S8 is
//     VK_FORMAT_D24_UNORM_S8_UINT, which is OPTIONAL in Vulkan --
//     D32_SFLOAT_S8_UINT is the fallback every desktop driver supports, and
//     SelectDepthFormat/SupportsDepthStencil make that choice. Same pattern
//     as NatCreateSurface's.
//
//  4. D3DUSAGE_AUTOGENMIPMAP HAS NO COUNTERPART. Vulkan generates nothing;
//     the level count is declared at creation and VulkanDevice::GenerateMipmaps
//     fills the chain by blitting down it. Same finding as
//     SurfNative::GenerateMipMaps'.
//
//  5. SetRenderTarget IS NOT DEVICE STATE. In D3D9 a render target is set on
//     the device and changed between draws; in Vulkan it is baked into a
//     VkFramebuffer inside a VkRenderPass, and a pass cannot be nested inside
//     another. Everything in this file that renders into its own target --
//     the shadow maps, the environment maps, the irradiance chain, the custom
//     cameras, the post-processing buffers -- therefore goes through
//     VulkanDevice::BeginOffscreen / EndOffscreen, which is the render-pass
//     and framebuffer cache described in VulkanFrame.h. That is the piece
//     IProcess.cpp was deferred waiting for.
//
//  6. THE HLSL PATHS BECOME GLSL PATHS AND THE MODULE DIRECTORY FOLLOWS THE
//     MODULE, but the ENTRY POINT NAMES and the TEXTURE FILE NAMES do not:
//     the first are content the GLSL must match, and the second are files
//     shipped in the installation. "Modules/D3D9Client/Glare.hlsl" becomes
//     "Modules/VulkanClient/Glare.glsl"; "D3D9Noise.dds" stays what it is on
//     disk.
//
// Type mapping is the file-wide one: D3D9Client -> VulkanClient,
// D3D9CelestialSphere -> VulkanCelestialSphere, D3D9Light/Sun/Pick/MatExt ->
// Vulkan..., D3D9ParticleStream -> VulkanParticleStream, D3D9Text ->
// VulkanText, D3D9Pad -> VulkanPad, LPDIRECT3DDEVICE9 -> VulkanDevice*,
// LPDIRECT3DSURFACE9 / TEXTURE9 / CUBETEXTURE9 -> VulkanTexture*,
// ID3DXEffect -> VulkanEffectFile, D3DXHANDLE -> TECHHANDLE or HANDLE,
// D3DXMATRIX -> FMATRIX4, D3DXVECTOR2/3/4 -> FVECTOR2/3/4,
// D3DXCOLOR -> FVECTOR4, D3DCOLOR -> DWORD.
// ==============================================================

#include "Scene.h"
#include "VPlanet.h"
#include "VVessel.h"
#include "VBase.h"
#include "Particle.h"
#include "CSphereMgr.h"
#include "VulkanUtil.h"
#include "VulkanConfig.h"
#include "VulkanSurface.h"
// VulkanPad.h is named here and is NOT in the Windows include list, because
// on Windows it did not have to be: D3D9Surface.h included D3D9Pad.h, and
// that is where Scene.cpp got the full definition of the Sketchpad class it
// uses on nearly every page. VulkanSurface.h includes VulkanTypes.h instead --
// a deliberate narrowing recorded in that file, since the surface no longer
// needs the pad's definition to declare itself. So the dependency Scene.cpp
// always had is now spelled out rather than inherited.
#include "VulkanPad.h"
#include "VulkanTextMgr.h"
#include "VulkanCatalog.h"
#include "AABBUtil.h"
#include "OapiExtension.h"
#include "DebugControls.h"
#include "IProcess.h"
#include "VectorHelpers.h"
#include "CelSphere.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

#define IKernelSize 150

using namespace oapi;
using namespace std;

static FMATRIX4 ident;

const double LABEL_DISTLIMIT = 0.6;

struct PList { // auxiliary structure for object distance sorting
	vObject *vo;
	double dist;
};

const int MAXPLANET = 512; // hard limit; should be fixed
static PList plist[MAXPLANET];

VulkanEffectFile * Scene::FX = 0;
TECHHANDLE Scene::eLine = 0;
TECHHANDLE Scene::eStar = 0;
HANDLE Scene::eWVP = 0;
HANDLE Scene::eColor = 0;
HANDLE Scene::eTex0 = 0;


FVECTOR4 IKernel[IKernelSize];

static const int FONT_SIZES[4] = { 12, 16, 20, 26 };


// -------------------------------------------------------------------------------------------
// The counterparts of SAFE_RELEASE for the two client resource types. Vulkan
// objects are not reference counted; VulkanDevice::DestroyTexture is the
// delete. Written once here because this file releases some sixty images.
//
// SEE NOTE 1 IN THE FILE HEADER BEFORE ADDING A CALL: psgBuffer/ptgBuffer,
// psShmRT/ptShmRT and pLocalResultsSL/pLocalResults are each ONE image under
// two names, and destroying both names is a double free.
// -------------------------------------------------------------------------------------------
static inline void SafeDestroy(VulkanTexture *&p)
{
	if (p) { VulkanEffect::pDev->DestroyTexture(p); p = NULL; }
}


bool sort_vessels(const vVessel *a, const vVessel *b)
{
	return a->CameraTgtDist() < b->CameraTgtDist();
}

float Rand()
{
	return float(rand()) / 32768.0f;
}

// ===========================================================================================
//
Scene::Scene(VulkanClient *_gc, DWORD w, DWORD h)
{
	_TRACE;

	gc = _gc;
	vobjEnv = NULL;
	vobjIrd = NULL;
	m_celSphere = NULL;
	Lights = NULL;
	hSun = NULL;
	pAxisFont  = NULL;
	pLabelFont = NULL;
	pDebugFont = NULL;
	pBlur = NULL;
	pOffscreenTarget = NULL;
	pLocalCompute = NULL;
	pRenderGlares = NULL;
	pCreateGlare = NULL;
	viewH = h;
	viewW = w;
	nLights = 0;
	dwTurn = 0;
	dwFrameId = 0;
	surfLabelsActive = false;

	pSunTex = NULL;
	pLightGlare = NULL;
	pSunGlare = NULL;
	pSunGlareAtm = NULL;
	pEnvDS = NULL;
	pIrradDS = NULL;
	pIrradiance = NULL;
	pIrradTemp = NULL;
	pIrradTemp2 = NULL;
	pIrradTemp3 = NULL;
	pDepthNormalDS = NULL;
	pVisDepth = NULL;
	pLocalResults = NULL;
	pLocalResultsSL = NULL;

	fDisplayScale = float(viewH) / 1080.0f;

	for (auto& a : DepthSampleKernel) a = FVECTOR2(0, 0);

	memset(&psShmDS, 0, sizeof(psShmDS));
	memset(&ptShmRT, 0, sizeof(ptShmRT));
	memset(&psShmRT, 0, sizeof(psShmRT));


	pDevice = _gc->GetDevice();

	// (void*) ON THE THREE memsets BELOW. Scene::CAMERA holds VECTOR3s and
	// FVECTOR3s, VulkanSun holds FVECTOR3s and SHADOWMAPPARAM holds FMATRIX4s,
	// so none of the three is trivially copyable to GCC's eye and it warns
	// (-Wclass-memaccess) where MSVC does not. THE ZERO IS STILL WHAT IS
	// WANTED and a constructor is not a substitute: FVECTOR3's default
	// constructor leaves its fields untouched, so value-initialising these
	// would not zero them. Same treatment, same reasoning, as the nine sites
	// in Mesh.cpp.
	memset((void*)&Camera, 0, sizeof(Camera));

	VMAT_Identity(&ident);

	SetCameraAperture(float(RAD*50.0), float(viewH)/float(viewW));
	SetCameraFrustumLimits(2.5f, 5e6f); // initial limits
	Camera.labelScale = 1.0f;

	m_celSphere = new VulkanCelestialSphere(gc, this);
	Lights = new VulkanLight[MAX_SCENE_LIGHTS];

	bLocalLight = *(bool*)gc->GetConfigParam(CFGPRM_LOCALLIGHT);
	
	memset((void*)&sunLight, 0, sizeof(VulkanSun));
	memset((void*)&smap, 0, sizeof(smap));

	CLEARARRAY(pBlrTemp);
	CLEARARRAY(pTextures);
	CLEARARRAY(ptgBuffer);
	CLEARARRAY(psgBuffer);

	vobjFirst = vobjLast = NULL;
	nstream = 0;
	iVCheck = 0;

	InitGDIResources();

	while (true) {
		float dx = 0;
		float dy = 0;
		for (int i = 0; i < IKernelSize; i++) {
			double r = oapiRand();
			double a = oapiRand() * PI2;
			IKernel[i].x = float(cos(a) * r);
			IKernel[i].y = float(sin(a) * r);
			dx += IKernel[i].x;
			dy += IKernel[i].y;
		}
		if ((fabs(dx) < 1.0f) && (fabs(dy) < 1.0f)) break;
	}

	for (int i = 0; i < IKernelSize; i++) {
		float d = IKernel[i].x*IKernel[i].x + IKernel[i].y*IKernel[i].y;
		IKernel[i].z = sqrt(1.0f - saturate(d));
		IKernel[i].w = sqrt(IKernel[i].z);
	}



	// ------------------------------------------------------------------------------
	// Read Sun glare sampling kernel file
	//
	// GKernel.txt is DATA that ships beside the module, so only the module
	// directory changes -- the file keeps its name.

	ifstream fs("Modules/VulkanClient/GKernel.txt");
	if (fs.good()) {
		string line; vector<FVECTOR2> data;
		while (getline(fs, line)) {
			std::istringstream iss(line);
			char c;	float a, b;	iss >> a >> c >> b;
			data.push_back(FVECTOR2(a, b));
		}
		if (data.size() != ARRAYSIZE(DepthSampleKernel)) LogErr("Modules/VulkanClient/GKernel.txt Size missmatch. Expecting 57 entries");
		else for (int i = 0; i < int(ARRAYSIZE(DepthSampleKernel)); i++) DepthSampleKernel[i] = data[i];
		data.clear();
	} else LogErr("Failed to read: Modules/VulkanClient/GKernel.txt");
	fs.close();

	CreateSunGlare();


	// ------------------------------------------------------------------------------
	// Initialize a shaders for local lights visibility checks and rendering 

	if (Config->bGlares || Config->bLocalGlares)
	{
		pRenderGlares = new ShaderClass(pDevice, "Modules/VulkanClient/Glare.glsl", "GlareVS", "GlarePS", "RenderGlares", "");
		pLocalCompute = new ShaderClass(pDevice, "Modules/VulkanClient/Glare.glsl", "VisibilityVS", "VisibilityPS", "LocalVisCheck", "");
		// D3DXCreateTexture + GetSurfaceLevel(0). One image, two names -- see
		// note 1: pLocalResultsSL is pLocalResults and is not destroyed twice.
		pLocalResults = pDevice->CreateTexture(32, 1, 1, VK_FORMAT_R16_SFLOAT,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
			VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
		pLocalResultsSL = pLocalResults;
	}


	// Render screen depth and screen space normals
	//

	if (Config->bGlares || Config->bLocalGlares) {
		pVisDepth = new ImageProcessing(pDevice, "Modules/VulkanClient/LightBlur.glsl", "PSDepth", NULL);
		pVisDepth->CompileShader("PSNormal");
	}

	// Initialize envmapping and shadow maps -----------------------------------------------------------------------------------------------
	//
	DWORD EnvMapSize = Config->EnvMapSize;
	DWORD ShmMapSize = Config->ShadowMapSize;

	// D3DFMT_D24S8 / D3DFMT_D24X8 -> the one depth-stencil format the device
	// actually supports. D24_UNORM_S8_UINT is optional in Vulkan;
	// D32_SFLOAT_S8_UINT is the fallback every desktop driver has. The X8 and
	// S8 spellings collapse: Vulkan has no "stencil bits present but unused"
	// format, and an unused stencil is the same image with the stencil left
	// alone -- which is what D24X8 already meant.
	VkFormat dsFmt = VK_FORMAT_D24_UNORM_S8_UINT;
	if (!pDevice->SupportsDepthStencil(dsFmt)) dsFmt = VK_FORMAT_D32_SFLOAT_S8_UINT;

	if (Config->EnvMapMode) {
		pEnvDS = pDevice->CreateTexture(EnvMapSize, EnvMapSize, 1, dsFmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
	}

	if (Config->bIrradiance) {
		pIrradDS = pDevice->CreateTexture(128, 128, 1, dsFmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
	}


	if (Config->ShadowMapMode) {
		UINT size = ShmMapSize;
		for (int i = 0; i < SHM_LOD_COUNT; i++) {
			psShmDS[i] = pDevice->CreateTexture(size, size, 1, dsFmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
			ptShmRT[i] = pDevice->CreateTexture(size, size, 1, VK_FORMAT_R32_SFLOAT,
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
				VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
			psShmRT[i] = ptShmRT[i];		// GetSurfaceLevel(0); see note 2
			size >>= 1;
		}

		smap.pShadowMap = ptShmRT[0];
	}



	// Create auxiliary color buffer for on screen GDI
	//
	if (Config->GDIOverlay) {
		ptgBuffer[GBUF_GDI] = pDevice->CreateTexture(viewW, viewH, 1, VK_FORMAT_B8G8R8A8_UNORM,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
			VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
		pGDIOverlay = new ImageProcessing(pDevice, "Modules/VulkanClient/GDIOverlay.glsl", "PSMain");
	}
	else pGDIOverlay = NULL;


	// Create an auxiliary screen space normal and depth buffer (i.e. Shader readable depth buffer)
	//
	if (Config->bGlares || Config->bLocalGlares) {
		pDepthNormalDS = pDevice->CreateTexture(viewW, viewH, 1, dsFmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
		ptgBuffer[GBUF_DEPTH] = pDevice->CreateTexture(viewW, viewH, 1, VK_FORMAT_R16G16B16A16_SFLOAT,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
			VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
	}


	// Initialize post processing effects --------------------------------------------------------------------------------------------------
	//
	pLightBlur = NULL;

	if (Config->PostProcess) {

		int BufSize = 1;
		int BufFmt = 0;

		// Get the actual back buffer description.
		//
		// `D3DSURFACE_DESC desc; gc->GetBackBuffer()->GetDesc(&desc);` -- a
		// query to the runtime. A VkImage answers no such question, so the
		// client's own record is read instead: VulkanTexture carries the
		// VulkanImageDesc it was created with. See VulkanTypes.h.
		const VulkanImageDesc &desc = gc->GetBackBuffer()->Desc();

		char flags[32] = { 0 };
		if (Config->ShaderDebug) strcpy_s(flags, 32, "DISASM");

		// Load postprocessing effects
		if (Config->PostProcess == PP_DEFAULT)
			pLightBlur = new ImageProcessing(pDevice, "Modules/VulkanClient/LightBlur.glsl", "PSMain", flags);

		if (pLightBlur) {
			BufSize = pLightBlur->FindDefine("BufferDivider");
			BufFmt = pLightBlur->FindDefine("BufferFormat");
		}

		// D3DFMT_A16B16G16R16F -> R16G16B16A16_SFLOAT (the two names reverse
		// the component order and mean the same bytes); D3DFMT_A2R10G10B10 ->
		// A2R10G10B10_UNORM_PACK32, which is a PACKED format and therefore
		// keeps D3D's DWORD-order naming rather than taking byte order.
		VkFormat BackBuffer = desc.Format;
		if (BufFmt == 1) BackBuffer = VK_FORMAT_R16G16B16A16_SFLOAT;
		if (BufFmt == 2) BackBuffer = VK_FORMAT_A2R10G10B10_UNORM_PACK32;

		const VkImageUsageFlags rtUsage =
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
			VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

		// Create auxiliary color buffer for color operations
		ptgBuffer[GBUF_COLOR] = pDevice->CreateTexture(viewW, viewH, 1, BackBuffer, rtUsage);

		// Load some textures.
		//
		// D3D9Noise.dds and D3D9CLUT.dds are FILES IN THE INSTALLATION, not
		// code, so they keep their names -- renaming them here would ask for
		// files that do not exist. NatLoadTexture is
		// D3DXCreateTextureFromFileA's counterpart (VulkanSurface.h).
		char buff[MAX_PATH];
		if (gc->TexturePath("D3D9Noise.dds", buff)) pTextures[TEX_NOISE] = NatLoadTexture(buff);
		if (gc->TexturePath("D3D9CLUT.dds", buff)) pTextures[TEX_CLUT] = NatLoadTexture(buff);

		if (pLightBlur) {
			ptgBuffer[GBUF_BLUR] = pDevice->CreateTexture(viewW / BufSize, viewH / BufSize, 1, BackBuffer, rtUsage);
			ptgBuffer[GBUF_TEMP] = pDevice->CreateTexture(viewW / BufSize, viewH / BufSize, 1, BackBuffer, rtUsage);
		}

		if (pLightBlur) {
			// Construct an offscreen backbuffer with custom pixel format.
			//
			// CreateRenderTarget's multisample arguments have no counterpart
			// worth carrying: the core's render pass is single-sampled
			// (UIHost.cpp, VK_SAMPLE_COUNT_1_BIT), so the back buffer this
			// copies its sample type from is single-sampled too and the
			// arguments would both be "one sample". Same finding as
			// Surfmgr2.cpp's D3DRS_MULTISAMPLEANTIALIAS.
			pOffscreenTarget = pDevice->CreateTexture(viewW, viewH, 1, BackBuffer, rtUsage);
			if (!pOffscreenTarget) {
				LogErr("Creation of Offscreen render target failed");
				SAFE_DELETE(pLightBlur);
			}
		}
	}

	// `for (...) if (ptgBuffer[i]) ptgBuffer[i]->GetSurfaceLevel(0, &psgBuffer[i]);`
	// -- the assignment is the whole of it now. See note 2.
	for (int i = 0; i < int(ARRAYSIZE(ptgBuffer)); i++) psgBuffer[i] = ptgBuffer[i];


	if (Config->GDIOverlay) {
		// Clear the GDI Overlay with transparency.
		//
		// Was GetDC on the surface, CreateSolidBrush, FillRect, ReleaseDC --
		// four GDI calls to fill an image with one colour, which is what a
		// D3D9 surface's DC was good for. Src/Orbiter/Linux/Gdi.cpp is a
		// display-list RECORDER and would record a FillRect that never
		// reaches any pixels, so this says what it means: clear the image to
		// the colour key. ClearImage is ColorFill's counterpart.
		DWORD color = 0xF08040; // BGR "Color Key" value for transparency
		pDevice->ClearImage(psgBuffer[GBUF_GDI], NULL, color);
	}

	LogAlw("================ Scene Created ===============");
}

// ===========================================================================================
// SEE NOTE 1 IN THE FILE HEADER. The Windows destructor releases psgBuffer and
// ptgBuffer, psShmRT and ptShmRT, and pLocalResultsSL and pLocalResults --
// six arrays holding three sets of objects, each with two COM references. Here
// each pair is one image, so each is destroyed once and the second name is
// simply cleared.
//
// `pDevice->SetRenderTarget(0..3, NULL)` at the top has no counterpart and
// needs none: it existed to make the runtime let go of the render targets
// before they were released, and nothing in Vulkan holds a reference to an
// image because it was once an attachment. The one thing that does -- a
// command buffer still naming a descriptor -- is what orbiter_ResetFrameCommands
// answers, and CVulkanFramework::DestroyObjects calls it.
//
Scene::~Scene ()
{
	_TRACE;

	for (int i = 0; i < int(ARRAYSIZE(ptgBuffer)); i++) { SafeDestroy(ptgBuffer[i]); psgBuffer[i] = NULL; }
	for (int i = 0; i < int(ARRAYSIZE(pTextures)); i++) SafeDestroy(pTextures[i]);

	SAFE_DELETE(pGDIOverlay);
	SAFE_DELETE(pBlur);
	SAFE_DELETE(pVisDepth);
	SAFE_DELETE(pLightBlur);
	SAFE_DELETE(pIrradiance);
	SAFE_DELETE(m_celSphere);
	SAFE_DELETE(pLocalCompute);
	SAFE_DELETE(pRenderGlares);
	SAFE_DELETE(pCreateGlare);

	SafeDestroy(pOffscreenTarget);
	SafeDestroy(pEnvDS);
	SafeDestroy(pIrradDS);
	SafeDestroy(pIrradTemp);
	SafeDestroy(pIrradTemp2);
	SafeDestroy(pIrradTemp3);
	SafeDestroy(pDepthNormalDS);
	SafeDestroy(pLocalResults);
	pLocalResultsSL = NULL;			// the same image as pLocalResults
	SafeDestroy(pSunTex);
	SafeDestroy(pLightGlare);
	SafeDestroy(pSunGlare);
	SafeDestroy(pSunGlareAtm);

	for (int i = 0; i < int(ARRAYSIZE(psShmDS)); i++) SafeDestroy(psShmDS[i]);
	for (int i = 0; i < int(ARRAYSIZE(ptShmRT)); i++) { SafeDestroy(ptShmRT[i]); psShmRT[i] = NULL; }
	for (int i = 0; i < int(ARRAYSIZE(pBlrTemp)); i++) SafeDestroy(pBlrTemp[i]);

	if (Lights) {
		delete []Lights;
		Lights = NULL;
	}

	// Particle Streams
	if (nstream) {
		for (DWORD j=0;j<nstream;j++) delete pstream[j];
		delete []pstream;
		pstream = NULL;
	}

	DeleteAllCustomCameras();
	DeleteAllVisuals();
	ExitGDIResources();

	FreePooledSketchpads();
}



// ===========================================================================================
// Every `pXxx->GetSurfaceLevel(0, &pTgt); ... SetOutputNative(0, pTgt); SAFE_RELEASE(pTgt);`
// collapses to one line: the texture IS the render target. See note 2 in the
// file header. pTgt goes with them.
//
// D3DUSAGE_AUTOGENMIPMAP with a level count of 0 asked D3DX for a complete
// chain and the driver to keep it current. Vulkan has neither, so the level
// count is computed here and VulkanDevice::GenerateMipmaps fills the chain
// after each glare is drawn -- which is where the driver would have done it.
//
void Scene::CreateSunGlare()
{
	// ------------------------------------------------------------------------------
	// Create sun texture and glares

	if (pCreateGlare) SAFE_DELETE(pCreateGlare);

	pCreateGlare = new ImageProcessing(pDevice, "Modules/VulkanClient/Glare.glsl", "CreateSunGlarePS");
	pCreateGlare->CompileShader("CreateLocalGlarePS");
	pCreateGlare->CompileShader("CreateSunGlareAtmPS");
	pCreateGlare->CompileShader("CreateSunTexPS");
	

	if (!pSunTex) {
		UINT ts = (viewH >> 4) & 0xFFFC; // "ts" will be 64 for a Full HD display;  
		if (ts < 4) ts = 4;

		const VkImageUsageFlags glareUsage =
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
			VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

		// The complete chain D3DX's MipLevels=0 asked for.
		auto mips = [](uint32_t d) { uint32_t m = 1; while (d > 1) { d >>= 1; m++; } return m; };

		pSunTex      = pDevice->CreateTexture(ts * 5,  ts * 5,  mips(ts * 5),  VK_FORMAT_B8G8R8A8_UNORM, glareUsage);
		pLightGlare  = pDevice->CreateTexture(ts * 4,  ts * 4,  mips(ts * 4),  VK_FORMAT_R16_SFLOAT, glareUsage);
		pSunGlare    = pDevice->CreateTexture(ts * 12, ts * 12, mips(ts * 12), VK_FORMAT_R16_SFLOAT, glareUsage);
		pSunGlareAtm = pDevice->CreateTexture(ts * 12, ts * 12, mips(ts * 12), VK_FORMAT_R16_SFLOAT, glareUsage);
	}

	pCreateGlare->Activate("CreateSunGlarePS");
	pCreateGlare->SetOutputNative(0, pSunGlare);
	if (!pCreateGlare->Execute(false)) LogErr("pCreateGlare Execute Failed (CreateSunGlarePS)");
	pDevice->GenerateMipmaps(pSunGlare);

	pCreateGlare->Activate("CreateSunGlareAtmPS");
	pCreateGlare->SetOutputNative(0, pSunGlareAtm);
	if (!pCreateGlare->Execute(false)) LogErr("pCreateGlare Execute Failed (CreateSunGlareAtmPS)");
	pDevice->GenerateMipmaps(pSunGlareAtm);

	pCreateGlare->Activate("CreateLocalGlarePS");
	pCreateGlare->SetOutputNative(0, pLightGlare);
	if (!pCreateGlare->Execute(false)) LogErr("pCreateGlare Execute Failed (CreateLocalGlarePS)");
	pDevice->GenerateMipmaps(pLightGlare);

	pCreateGlare->Activate("CreateSunTexPS");
	pCreateGlare->SetOutputNative(0, pSunTex);
	if (!pCreateGlare->Execute(false)) LogErr("pCreateGlare Execute Failed (CreateSunTexPS)");
	pDevice->GenerateMipmaps(pSunTex);
}


// ===========================================================================================
//
void Scene::Initialise()
{
	_TRACE;

	hSun = oapiGetGbodyByIndex(0); // generalise later

	DWORD ambient = *(DWORD*)gc->GetConfigParam(CFGPRM_AMBIENTLEVEL);

	// Setup sunlight -------------------------------
	//
	sunLight.Color = 1.0f;
	sunLight.Ambient = float(ambient)*0.0039f;
	sunLight.Transmission = 1.0f;
	sunLight.Incatter = 0.0f;

	// Update Sunlight direction -------------------------------------
	//
	VECTOR3 rpos, cpos;
	oapiGetGlobalPos(hSun, &rpos);
	oapiCameraGlobalPos(&cpos); rpos-=cpos;
	sunLight.Dir = -unit(rpos);

	// Do not "pre-create" visuals here. Will cause changed call order for vessel callbacks
}


// ===========================================================================================
// Pooled Sketchpad API
// ===========================================================================================

static VulkanPad *_pad = NULL;

#define SKETCHPAD_LABELS        0  ///< Sketchpad for planetarium mode labels and markers
#define SKETCHPAD_2D_OVERLAY    1  ///< Sketchpad for HUD Overlay render to backbuffer directly
#define SKETCHPAD_DEBUG_TEXT    2  ///< Sketchpad to draw Debug String on a bottom of the screen
#define SKETCHPAD_PLANETARIUM   3  ///< Sketchpad to draw user defined planetarium

// ===========================================================================================

void Scene::OnOptionChanged(int cat, int item)
{
	if (cat == OPTCAT_CELSPHERE)
		m_celSphere->OnOptionChanged(cat, item);
}

// ===========================================================================================
// Get pooled Sketchpad instance
//
VulkanPad *Scene::GetPooledSketchpad (int id) // one of SKETCHPAD_xxx
{
	assert(id <= SKETCHPAD_PLANETARIUM);

	if (!_pad) _pad = new VulkanPad("POOLED_SKETCHPAD");

	// Automatically binds a Sketchpad to a top render target
	_pad->BeginDrawing();
	_pad->LoadDefaults();

	switch (id)
	{
	case SKETCHPAD_LABELS:
		_pad->SetFont(pLabelFont);
		_pad->SetTextAlign(Sketchpad::CENTER, Sketchpad::BOTTOM);
		break;

	case SKETCHPAD_2D_OVERLAY:
		break;

	case SKETCHPAD_DEBUG_TEXT:
		_pad->SetFont(pDebugFont);
		_pad->SetTextColor(0xFFFFFF);
		_pad->SetTextAlign(Sketchpad::LEFT, Sketchpad::BOTTOM);
		_pad->QuickPen(0xFF000000);
		_pad->QuickBrush(0xB0000000);
		break;

	case SKETCHPAD_PLANETARIUM:
		break;
	}

	return _pad;
}

// ===========================================================================================
// Release pooled Sketchpad instances
void Scene::FreePooledSketchpads()
{
	SAFE_DELETE(_pad);
}

// ===========================================================================================
//
double Scene::GetObjectAppRad(OBJHANDLE hObj) const
{
	VECTOR3 pos,cam;
	oapiGetGlobalPos (hObj, &pos);
	oapiCameraGlobalPos(&cam); // must use oapiCam.. here. called before camera setup
	double rad = oapiGetSize (hObj);
	double dst = dist (pos, cam);
	return (rad*double(viewH))/(dst*tan(oapiCameraAperture()));
}

// ===========================================================================================
//
double Scene::GetObjectAppRad2(OBJHANDLE hObj) const
{
	VECTOR3 pos;
	oapiGetGlobalPos (hObj, &pos);
	VECTOR3 cam = GetCameraGPos();
	double rad = oapiGetSize (hObj);
	double dst = dist (pos, cam);
	return (rad*double(viewH))/(dst*tan(oapiCameraAperture()));
}

// ===========================================================================================
//
void Scene::CheckVisual(OBJHANDLE hObj)
{
	_TRACE;

	if (hObj==NULL) return;

	VOBJREC *pv = FindVisual(hObj);
	if (!pv) pv = AddVisualRec(hObj);

	pv->apprad = float(GetObjectAppRad(hObj));

	if (pv->type == OBJTP_STAR) {
		pv->vobj->Activate(true);
		return;
	}

	if (pv->vobj->IsActive()) {
		if (pv->apprad < 1.0) pv->vobj->Activate(false);
	} else {
		if (pv->apprad > 2.0) pv->vobj->Activate(true);
	}
	// the range check has a small hysteresis to avoid continuous
	// creation/deletion for objects at the edge of visibility
}

// ===========================================================================================
//
const VulkanLight *Scene::GetLight(int index) const
{
	if ((DWORD)index<MAX_SCENE_LIGHTS || index>=0) return &Lights[index];
	return NULL;
}

// ===========================================================================================
//
Scene::VOBJREC *Scene::FindVisual(OBJHANDLE hObj) const
{
	if (hObj==NULL) return NULL;
	VOBJREC *pv;
	for (pv=vobjFirst; pv; pv=pv->next) if (pv->vobj->Object()==hObj) return pv;
	return NULL;
}

// ===========================================================================================
//
class vObject *Scene::GetVisObject(OBJHANDLE hObj) const
{
	Scene::VOBJREC *v = FindVisual(hObj);
	if (v) return v->vobj;
	return NULL;
}

// ===========================================================================================
//
std::set<vVessel *> Scene::GetVessels(double max_dst, bool bAct)
{
	std::set<vVessel *> List;
	VOBJREC *pv;
	for (pv = vobjFirst; pv; pv = pv->next) {
		if (pv->type != OBJTP_VESSEL) continue;
		if (bAct && pv->vobj->IsActive() == false) continue;
		if (pv->vobj->CamDist() < max_dst) List.insert((vVessel *)pv->vobj);
	}
	return List;
}

// ===========================================================================================
// `pv->vobj->GetObject()` here and in DeleteAllVisuals is finding 34 again:
// <windows.h> rewrites GetObject as GetObjectA, so the Windows source names
// the method that survives the macro. vObject declares Object() and nothing
// else. Three sites in this file.
//
void Scene::DelVisualRec (VOBJREC *pv)
{
	_TRACE;
	// unlink the entry
	if (pv->prev) pv->prev->next = pv->next;
	else          vobjFirst = pv->next;

	if (pv->next) pv->next->prev = pv->prev;
	else          vobjLast = pv->prev;

	DebugControls::RemoveVisual(pv->vobj);

	vobjEnv = NULL;
	vobjIrd = NULL;

	// delete the visual, its children and the entry itself
	gc->UnregisterVisObject(pv->vobj->Object());

	delete pv->vobj;
	delete pv;
}

// ===========================================================================================
//
void Scene::DeleteAllVisuals()
{
	_TRACE;
	VOBJREC *pv = vobjFirst;
	while (pv) {
		VOBJREC *pvn = pv->next;

		DebugControls::RemoveVisual(pv->vobj);

		gc->UnregisterVisObject(pv->vobj->Object());
		
		LogAlw("Deleting Visual %s", _PTR(pv->vobj));
		delete pv->vobj;
		delete pv;
		pv = pvn;
	}
	vobjFirst = vobjLast = NULL;
	vobjEnv = NULL;
	vobjIrd = NULL;
}

// ===========================================================================================
//
Scene::VOBJREC *Scene::AddVisualRec(OBJHANDLE hObj)
{
	_TRACE;

	char buf[256];

	// create the visual and entry
	VOBJREC *pv = new VOBJREC;

	memset(pv, 0, sizeof(VOBJREC));

	pv->vobj = vObject::Create(hObj, this);
	pv->type = oapiGetObjectType(hObj);

	oapiGetObjectName(hObj, buf, 255);

	VESSEL *hVes=NULL;
	if (pv->type==OBJTP_VESSEL) hVes = oapiGetVesselInterface(hObj);

	// link entry to end of list
	pv->prev = vobjLast;
	pv->next = NULL;
	if (vobjLast) vobjLast->next = pv;
	else          vobjFirst = pv;
	vobjLast = pv;

	LogAlw("RegisteringVisual (%s) hVessel=%s, hObj=%s, Vis=%s, Rec=%s, Type=%d", buf, _PTR(hVes), _PTR(hObj), _PTR(pv->vobj), _PTR(pv), pv->type);

	gc->RegisterVisObject(hObj, (VISHANDLE)pv->vobj);
	
	// Initialize Meshes
	pv->vobj->PreInitObject();

	return pv;
}

// ===========================================================================================
//
DWORD Scene::GetActiveParticleEffectCount()
{
	// render exhaust particle system
	DWORD count = 0;
	for (DWORD n = 0; n < nstream; n++) if (pstream[n]->IsActive()) count++;
	return count;
}

// ===========================================================================================
//
VECTOR3 Scene::SkyColour ()
{
	VECTOR3 col = {0,0,0};
	OBJHANDLE hProxy = oapiCameraProxyGbody();
	if (hProxy && oapiPlanetHasAtmosphere (hProxy)) {
		const ATMCONST *atmp = oapiGetPlanetAtmConstants (hProxy);
		VECTOR3 rc, rp, pc;
		rc = GetCameraGPos();
		oapiGetGlobalPos (hProxy, &rp);
		pc = rc-rp;
		double cdist = length (pc);
		if (cdist < atmp->radlimit) {
			ATMPARAM prm;
			oapiGetPlanetAtmParams (hProxy, cdist, &prm);
			normalise (rp);
			double coss = dotp (pc, rp) / -cdist;
			double intens = std::min (1.0,(1.0839*coss+0.4581)) * sqrt (prm.rho/atmp->rho0);
			// => intensity=0 at sun zenith distance 115?
			//    intensity=1 at sun zenith distance 60?
			if (intens > 0.0)
				col += _V(atmp->color0.x*intens, atmp->color0.y*intens, atmp->color0.z*intens);
		}
		for (int i=0;i<3;i++) if (col.data[i] > 1.0) col.data[i] = 1.0;
	}
	return col;
}

// ===========================================================================================
//
void Scene::Update ()
{
	_TRACE;

	// update particle streams - should be skipped when paused
	if (!oapiGetPause()) {
		for (DWORD i=0;i<nstream;) {
			if (pstream[i]->Expired()) DelParticleStream(i);
			else pstream[i++]->Update();
		}
	}

	static bool bFirstUpdate = true;

	// check object visibility (one object per frame in the interest
	// of scalability)
	DWORD nobj = oapiGetObjectCount();

	if (bFirstUpdate) {
		bFirstUpdate = false;
		for (DWORD i=0;i<nobj;i++) {
			OBJHANDLE hObj = oapiGetObjectByIndex(i);
			CheckVisual(hObj);
		}
	}
	else {

		if (iVCheck >= nobj) iVCheck = 0;

		// This function will browse through vessels and planets. (not bases)
		// Base visuals don't exist in the visual record.
		OBJHANDLE hObj = oapiGetObjectByIndex(iVCheck++);
		CheckVisual(hObj);
	}


	// If Camera target has changed, setup mesh debugger
	//
	OBJHANDLE hTgt = oapiCameraTarget();

	if (hTgt!=Camera.hTarget && hTgt!=NULL) {

		Camera.hTarget = hTgt;

		if (DebugControls::IsActive()) {
			if (oapiGetObjectType(hTgt) == OBJTP_SURFBASE) {
				OBJHANDLE hPlanet = oapiGetBasePlanet(hTgt);
				vPlanet *vp = static_cast<vPlanet *>(GetVisObject(hPlanet));
				if (vp) {
					vBase *vb = vp->GetBaseByHandle(hTgt);
					if (vb) {
						DebugControls::SetVisual(vb);
					}
				}
				return; // why?
			}
		}

		vObject *vo = GetVisObject(hTgt);

		if (vo) {

			if (DebugControls::IsActive()) {
				DebugControls::SetVisual(vo);
			}

			// Why is this here ?
			//
			// kuddel: OrbiterSound 4.0 did not play the sounds of the 'focused'
			//         Vessel when focus changed during playback. Therfore the
			//         VulkanClient does a oapiSetFocusObject call when playback
			//         is running. Is a OrbiterSound error, but we can work-around
			//         this, so we do! To reproduce, just disable the following
			//         code and run the 'Welcome.scn'.
			//         See also: http://www.orbiter-forum.com/showthread.php?p=392689&postcount=18
			//         and following...

			// OrbiterSound 4.0 'playback helper'
			if (OapiExtension::RunsOrbiter2010() &&
			    OapiExtension::RunsOrbiterSound40() &&
				oapiIsVessel(hTgt) && // oapiGetObjectType(vo->Object()) == OBJTP_VESSEL &&
				dynamic_cast<vVessel*>(vo)->Playback()
				)
			{
				// Orbiter doesn't do this when (only) camera focus changes
				// during playback, therfore we do it ;)
				oapiSetFocusObject(hTgt);
			}
		}
	}
}

// ===========================================================================================
//
double Scene::GetTargetElevation() const
{
	VESSEL *hVes = oapiGetVesselInterface(Camera.hTarget);
	if (hVes) return hVes->GetSurfaceElevation();
	return 0.0;
}


// ===========================================================================================
//
double Scene::GetFocusGroundAltitude() const
{
	VESSEL *hVes = oapiGetFocusInterface();
	if (hVes) return hVes->GetAltitude() - hVes->GetSurfaceElevation();
	return 0.0;
}



// ===========================================================================================
//
double Scene::GetTargetGroundAltitude() const
{
	VESSEL *hVes = oapiGetVesselInterface(Camera.hTarget);
	if (hVes) return hVes->GetAltitude() - hVes->GetSurfaceElevation();
	return 0.0;
}



// ============================================================================================
// Up, North, Forward in Ecliptic frame
//
// D3DXVEC -> FVEC, and the two D3DX vector calls become the SDK's own, which
// RETURN their result instead of writing through an out-parameter.
//
void Scene::GetLVLH(vVessel *vV, FVECTOR3 *up, FVECTOR3 *nr, FVECTOR3 *fw)
{
	if (!vV || !up || !nr || !fw) return;

	MATRIX3 grot; VECTOR3 rpos;
	VESSEL *hV = vV->GetInterface(); assert(hV);
	OBJHANDLE hRef = hV->GetGravityRef();
	oapiGetRotationMatrix(hRef, &grot);
	hV->GetRelativePos(hRef, rpos);
	VECTOR3 axis = mul(grot, _V(0, 1, 0));
	normalise(rpos);
	*up = FVEC(rpos);
	*fw = FVEC(unit(crossp(axis, rpos)));
	*nr = cross(*up, *fw);
	*nr = unit(*nr);
}



// ===========================================================================================
// Compute a distance to a near/far plane
// ===========================================================================================

float Scene::ComputeNearClipPlane()
{
	float zsurf = 1000.0f;
	VOBJREC *pv = NULL;

	OBJHANDLE hObj = Camera.hObj_proxy;
	OBJHANDLE hTgt = Camera.hTarget;
	VESSEL *hVes = oapiGetVesselInterface(hTgt);

	if (hObj && hVes) {
		VECTOR3 pos;
		oapiGetGlobalPos(hObj,&pos);
		double g = atan(Camera.apsq);
		double t = dotp(unit(Camera.pos-pos), unit(Camera.dir));
		// Two statements on one line in the Windows original. Split onto two
		// lines because GCC reads the second `if` as an else-branch that is
		// not indented like one (-Wmisleading-indentation) where MSVC says
		// nothing. FORMATTING ONLY -- both clamps are kept exactly as
		// written, including the first one's 1.0 (not -1.0), which is the
		// author's and is not corrected here.
		if (t<-1.0) t=1.0;
		if (t>1.0) t=1.0f;
		double a = PI - acos(t);
		double R = oapiGetSize(hObj) + hVes->GetSurfaceElevation();
		double r = length(Camera.pos-pos);
		double h = r - R;
		if (h<10e3) {
			double d = a - g; if (d<0) d=0;
			zsurf = float(h*cos(g)/cos(d));
			if (zsurf>1000.0f || zsurf<0.0f) zsurf=1000.0f;
		}
	}

	float zmin = 1.0f;
	if (GetCameraAltitude()>10e3) zmin = 0.1f;

	int count = 0;
	int actbase = 0;
	vPlanet *pl = NULL;

	float farpoint = 0.0f;
	float nearpoint = 10e3f;
	float neardist = 10e3f;

	for (pv = vobjFirst; pv; pv = pv->next) {

		float nr = 10e3f;
		float fr = 0.0f;
		float dn = 10e3f;

		bool bCockpit = false;

		if (pv->type==OBJTP_VESSEL) {
			if (pv->vobj==vFocus) {
				bCockpit = oapiCameraInternal();
			}
		}

		if (pv->apprad>0.01 && pv->vobj->IsActive()) {

			vObject *obj = pv->vobj;

			if (pv->type==OBJTP_PLANET) {
				if (obj->Object()==hObj) pl = (vPlanet*)obj;
				obj->GetMinMaxDistance(&nr, &fr, &dn);
				if (dn<neardist) neardist = dn;
				if (nr<nearpoint) nearpoint = nr;
				if (fr>farpoint) farpoint = fr;
				continue;
			}

			if (pv->type==OBJTP_VESSEL) {

				if (obj->IsVisible()) {

					if (bCockpit) if (vFocus->HasExtPass()==false) continue; // Ignore MinMax

					obj->GetMinMaxDistance(&nr, &fr, &dn);

					if (dn<neardist) neardist = dn;
					if (nr<nearpoint) nearpoint = nr;
					if (fr>farpoint) farpoint = fr;
					count++;
				}
			}
		}
	}

	if (pl) {

		float nr = 10e3;
		float fr = 0.0f;
		float dn = 10e3;

		DWORD bc = pl->GetBaseCount();
		for (DWORD i=0;i<bc;i++) {
			vBase *vb = pl->GetBaseByIndex(i);
			if (vb) {
				if (vb->IsActive() && vb->IsVisible()) {
					vb->GetMinMaxDistance(&nr, &fr, &dn);
					if (dn<neardist) neardist = dn;
					if (nr<nearpoint) nearpoint = nr;
					if (fr>farpoint) farpoint = fr;
					actbase++;
				}
			}
		}
	}

	DWORD prteff = GetActiveParticleEffectCount();

	if (farpoint==0.0) farpoint = 20e4;

	// The device argument is gone. D9NearPlane took one only to ask
	// GetViewport() for the frame's height; the converted version reads it
	// from VulkanEffect::pDev instead, because the viewport is not device
	// state here. See the note at the top of AABBUtil.h.
	float znear = D9NearPlane(nearpoint, farpoint, neardist, GetProjectionMatrix(), (prteff!=0));

	if (oapiCameraInternal()) {
		if (Config->NearClipPlane==0) zmin = 1.0f;
		else						  zmin = 0.1f;
	}

	znear = min(znear, zsurf);
	znear = max(znear, zmin);

	return znear;
}





// ===========================================================================================
// Prepare scene for rendering
//
// - Update camera for rendering of the main scene
// - Update all visuals
// - Distance sort planets
// - Setup sky color
// - Setup local light sources
// ===========================================================================================

bool Scene::UpdateCamVis()
{

	// Update camera parameters --------------------------------------
	// and call vObject::Update() for all visuals
	//
	bool bRet = UpdateCameraFromOrbiter(RENDERPASS_MAINSCENE);

	if (Camera.hObj_proxy) VulkanEffect::UpdateEffectCamera(Camera.hObj_proxy);

	// Update Sunlight direction -------------------------------------
	//
	VECTOR3 rpos;
	oapiGetGlobalPos(hSun, &rpos);
	rpos -= Camera.pos;
	sunLight.Dir = -unit(rpos);

	// Get focus visual -----------------------------------------------
	//
	OBJHANDLE hFocus = oapiGetFocusObject();
	vFocus = NULL;
	for (VOBJREC *pv=vobjFirst; pv; pv=pv->next) {
		if (pv->type==OBJTP_VESSEL) if (pv->vobj->Object()==hFocus) {
			vFocus = (vVessel *)pv->vobj;
			break;
		}
	}

	// Compute SkyColor -----------------------------------------------
	//
	sky_color = SkyColour();
	bglvl = (sky_color.x + sky_color.y + sky_color.z) / 3.0;
	// D3DCOLOR_RGBA(r,g,b,a) is a MACRO OVER FOUR INTEGERS -- (a<<24)|(r<<16)|
	// (g<<8)|b -- and never called into Direct3D. bg_rgba is a DWORD in the
	// same 0xAARRGGBB packing, so the macro is written out rather than
	// replaced; there is nothing here to convert.
	bg_rgba = (DWORD(255) << 24) |
			  (DWORD(int(sky_color.x*255) & 0xFF) << 16) |
			  (DWORD(int(sky_color.y*255) & 0xFF) << 8) |
			   DWORD(int(sky_color.z*255) & 0xFF);

	// DIAGNOSTIC: the daytime sky reads black and the stars are not being
	// dimmed, both of which are driven from here. Env-gated, once a second.
	if (getenv("ORBITER_VK_TRACE_SKY")) {
		static double tLast = -1e9;
		const double t = oapiGetSysTime();
		if (t - tLast > 1.0) {
			tLast = t;
			LogErr("SKY sky_color=(%.4f %.4f %.4f) bglvl=%.4f bg_rgba=%08X",
				   sky_color.x, sky_color.y, sky_color.z, bglvl, (unsigned)bg_rgba);
		}
	}


	// Process Local Light Sources -------------------------------------
	//
	if (bLocalLight) {

		ClearLocalLights();

		VOBJREC *pv = NULL;
		for (pv = vobjFirst; pv; pv = pv->next) {
			if (!pv->vobj->IsActive()) continue;
			OBJHANDLE hObj = pv->vobj->Object();
			if (oapiGetObjectType (hObj) == OBJTP_VESSEL) {
				VESSEL *vessel = oapiGetVesselInterface (hObj);
				DWORD nemitter = vessel->LightEmitterCount();
				for (DWORD j = 0; j < nemitter; j++) {
					const LightEmitter *em = vessel->GetLightEmitter(j);
					if ((em->GetVisibility() == LightEmitter::VIS_EXTERNAL) || (em->GetVisibility() == LightEmitter::VIS_ALWAYS))
						AddLocalLight(em, pv->vobj);
				}
			}
		}
	}


	// ----------------------------------------------------------------
	// render solar system celestial objects (planets and moons)
	// we render without z-buffer, so need to distance-sort the objects
	// ----------------------------------------------------------------

	VOBJREC *pv = NULL;
	nplanets = 0;

	// DWORD(MAXPLANET): nplanets is a DWORD and MAXPLANET an int literal, so
	// GCC warns on the mixed-signedness comparison (-Wsign-compare) where
	// MSVC does not. The cast changes nothing -- MAXPLANET is positive and
	// far below 2^31 -- and is the same one-word fix applied wherever this
	// pairing appears in the converted client.
	for (pv = vobjFirst; pv && nplanets < DWORD(MAXPLANET); pv = pv->next) {
		if (pv->apprad < 0.01 && pv->type != OBJTP_STAR) continue;
		if (pv->type == OBJTP_PLANET || pv->type == OBJTP_STAR) {
			plist[nplanets].vo = pv->vobj;
			plist[nplanets].dist = pv->vobj->CamDist();
			nplanets++;
		}
	}

	int distcomp(const void *arg1, const void *arg2);

	qsort((void*)plist, nplanets, sizeof(PList), distcomp);

	return bRet && (vFocus != nullptr);
}

// ===========================================================================================
//
void Scene::ClearLocalLights()
{
	nLights  = 0;
	lmaxdst2 = 0.0f;

	// Clear active local lisghts list -------------------------------
	// int(MAX_SCENE_LIGHTS): the constant is a DWORD and i an int, so GCC
	// warns on the mixed-signedness comparison where MSVC does not. The cast
	// changes nothing.
	for (int i = 0; i < int(MAX_SCENE_LIGHTS); i++) Lights[i].Reset();
}

// ===========================================================================================
//
void Scene::AddLocalLight(const LightEmitter *le, const vObject *vo)
{
	if (Lights==NULL) return;
	if (le->IsActive()==false || le->GetIntensity()==0.0) return;

	assert(vo != NULL);

	VulkanLight lght(le, vo);

	// -----------------------------------------------------------------------------
	// Replace or Add
	//
	if (nLights == MAX_SCENE_LIGHTS) {
		if (lght.Dst2 > lmaxdst2) return;
		DWORD imax = 0;
		for (DWORD i = 0; i < MAX_SCENE_LIGHTS; i++) if (Lights[i].Dst2 > lmaxdst2) imax = i;
		Lights[imax] = lght;
		lmaxdst2 = lght.Dst2;
	}
	else {
		Lights[nLights] = lght;
		if (lght.Dst2>lmaxdst2) lmaxdst2 = lght.Dst2;
		nLights++;
	}
}

// ===========================================================================================
// FOUR THINGS CHANGE HERE, and three of them recur throughout the rest of this
// file, so they are spelled out once:
//
//   GetDesc() ON A SURFACE becomes VulkanTexture::Desc(), the record the
//   client kept when it created the image. A VkImage answers no questions
//   about itself; see VulkanTypes.h.
//
//   D3DXMatrixOrthoOffCenterLH becomes VMAT_OrthoOffCenterLH -- written out in
//   VulkanUtil.cpp, because D3DX was a utility library and Vulkan ships no
//   counterpart. Its note records the one handedness difference that is left
//   uncorrected and why.
//
//   PushRenderTarget/PopRenderTargets keep their names and their places. They
//   are the client's own render-target stack (VulkanClient.h), and what
//   changes is underneath them: the push now begins a render pass through
//   VulkanDevice::BeginOffscreen rather than calling SetRenderTarget. The
//   reference's own comment -- "Must setup render target before calling
//   Setup()" -- becomes a hard requirement rather than good practice, because
//   Setup() builds a pipeline and a pipeline is tied to a render pass.
//
//   pDevice->DrawPrimitiveUP becomes ShaderClass::DrawUP. Vulkan has no
//   draw-from-memory call; see VulkanUtil.h. THE COUNT CHANGES MEANING: D3D9
//   took a PRIMITIVE count and, for a point list, one point is one primitive,
//   so nGlares happens to be both. It is spelled as a vertex count here.
//
void Scene::ComputeLocalLightsVisibility()
{

	if (!ptgBuffer[GBUF_DEPTH] || !pLocalCompute) {
		Config->bGlares = false;
		Config->bLocalGlares = false;
		return;
	}

	VECTOR3 gsun;
	oapiGetGlobalPos(oapiGetObjectByIndex(0), &gsun);

	// Put the Sun on a top of the list
	LLCBuf[0].index = 0.0f;
	LLCBuf[0].pos = FVECTOR3(unit(gsun - Camera.pos)) * 10e4;
	LLCBuf[0].cone = 1.0f;

	int nGlares = 1;

	for (int i = 0; i < int(nLights); i++)
	{
		if (Lights[i].cone > 0.0f) {
			LLCBuf[nGlares].index = float(nGlares);
			LLCBuf[nGlares].pos = Lights[i].Position;
			LLCBuf[nGlares].cone = Lights[i].cone;
			Lights[i].GPUId = nGlares;
			nGlares++;
		}
	}

	struct {
		FMATRIX4 mVP;
		FMATRIX4 mSVP;
		FVECTOR4 vSrc;
		FVECTOR3 vDir;
	} ComputeData;

	const VulkanImageDesc &rdesc = pLocalResultsSL->Desc();

	VMAT_OrthoOffCenterLH(&ComputeData.mVP, 0.0f, (float)rdesc.Width, (float)rdesc.Height, 0.0f, 0.0f, 1.0f);

	const VulkanImageDesc &ddesc = psgBuffer[GBUF_DEPTH]->Desc();

	ComputeData.vSrc = FVECTOR4((float)ddesc.Width, (float)ddesc.Height, 1.0f / (float)ddesc.Width, 1.0f / (float)ddesc.Height);
	ComputeData.vDir = Camera.z;
	ComputeData.mSVP = Camera.mProjView;

	// Must setup render target before calling Setup()
	gc->PushRenderTarget(pLocalResultsSL, NULL, RENDERPASS_UNKNOWN);

	pLocalCompute->ClearTextures();
	pLocalCompute->SetPSConstants("cbPS", &ComputeData, sizeof(ComputeData));
	pLocalCompute->SetPSConstants("cbKernel", DepthSampleKernel, sizeof(DepthSampleKernel));
	pLocalCompute->SetVSConstants("cbPS", &ComputeData, sizeof(ComputeData));
	pLocalCompute->SetTexture("tDepth", ptgBuffer[GBUF_DEPTH], IPF_CLAMP | IPF_POINT);
	// D3DPT_POINTLIST was an argument to the draw; it is pipeline state here,
	// so it has to be declared before Setup() builds the pipeline.
	pLocalCompute->SetTopology(VK_PRIMITIVE_TOPOLOGY_POINT_LIST);
	pLocalCompute->Setup(pLocalLightsDecl, false, 0);
	pLocalCompute->UpdateTextures();

	// Compute local lights visibility
	pLocalCompute->DrawUP(LLCBuf, nGlares, sizeof(LocalLightsCompute));

	pLocalCompute->DetachTextures();
	gc->PopRenderTargets();
}


// ===========================================================================================
// THIS FUNCTION HAS NO COUNTERPART AND CANNOT HAVE ONE.
//
// Every line of it is a SetRenderState: fill mode, stencil, colour write mask,
// depth test and write, alpha test and blend, blend op and factors, cull mode.
// In D3D9 those are DEVICE STATE -- global, sticky, and settable at any time --
// so a routine that puts them all back to a known configuration is meaningful,
// and the client calls it after anything that might have left the device
// somewhere unexpected.
//
// IN VULKAN NONE OF THEM IS DEVICE STATE. Every one is baked into a
// VkPipeline at creation time and applies only while that pipeline is bound;
// the next bind replaces the lot. There is no global configuration to recall,
// nothing persists between draws, and a pipeline built with the wrong blend
// cannot be corrected afterwards -- which is exactly why the render states the
// reference sets between BeginPass and a draw became PassOverride instead.
//
// So the function stays, because ~20 call sites in this file and in
// VulkanClient.cpp call it and each of those calls is a statement about where
// the reference thought the device might be dirty, which is worth keeping
// visible. It does nothing, and that is correct rather than unimplemented.
//
void Scene::RecallDefaultState()
{
}


// ===========================================================================================
// THE MAIN SCENE, and the four recurring conversions in it:
//
//   pDevice->Clear(0, NULL, D3DCLEAR_TARGET|_ZBUFFER|_STENCIL, c, z, s)
//   becomes VulkanDevice::ClearFrame(colour, depth, stencil, c, z, s) --
//   vkCmdClearAttachments, which is the only Vulkan call that clears what is
//   BOUND rather than a whole image, and the only one legal inside a render
//   pass. See VulkanFrame.h; the distinction is the same one VulkanPad3.cpp's
//   Clear() records.
//
//   pDevice->SetRenderState(D3DRS_CULLMODE, ...) around the celestial sphere
//   HAS NO COUNTERPART AND NEEDS NONE. It was device state, sticky across
//   draws; the cull mode is baked into every pipeline here, and CelSphere's
//   own passes carry theirs. Both the set and the restore disappear -- the
//   same finding as Surfmgr2.cpp's, and Mesh.cpp's.
//
//   pDevice->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME) is the ONE
//   piece of global fill state that still has to act globally, because it is
//   a debug switch applied to the whole frame. It becomes
//   VulkanDevice::SetPolygonMode, which both pipeline builders read and both
//   pipeline caches key on.
//
//   vFocus->GetObjectA() is Object(), and GetBoundingSpherePosDX() is
//   GetBoundingSpherePosF(). Findings 34 and VObject.h's note 1.
//
void Scene::RenderMainScene()
{
	_TRACE;

	dwFrameId++; // Advance to a next frame

	double scene_time = VulkanGetTime();
	VulkanSetTime(VulkanStats.Timer.CamVis, scene_time);

	if (!UpdateCamVis()) {
		if (SUCCEEDED(gc->BeginScene())) {
			pDevice->ClearFrame(true, true, true, 0, 1.0f, 0);
			gc->EndScene();
		}
		return; // Scene not yet properly inilialized, return
	}


	// Update Vessel Animations
	//
	for (VOBJREC *pv = vobjFirst; pv; pv = pv->next) {
		if (pv->type == OBJTP_VESSEL) {
			vVessel *vv = (vVessel *)pv->vobj;
			vv->UpdateAnimations();
		}
	}


	if (vFocus == NULL) return;

	// Was LPDIRECT3DSURFACE9. gc->GetBackBuffer() already returns the client's
	// own image rather than a surface interface -- see VulkanClient.h's note 5
	// -- so the declaration is the only change.
	VulkanTexture *pBackBuffer;

	if (pOffscreenTarget) pBackBuffer = pOffscreenTarget;
	else				  pBackBuffer = gc->GetBackBuffer();


	// Begin a Scene ------------------------------------------------------------------------------------
	//
	if (FAILED (gc->BeginScene())) return;



	// -------------------------------------------------------------------------------------------------------
	// Render Custom Camera and Environment Views
	// -------------------------------------------------------------------------------------------------------
	bool bIrrad = Config->EnvMapMode && Config->bIrradiance;

	if (Config->CustomCamMode == 0 && dwTurn == RENDERTURN_CUSTOMCAM) dwTurn++;
	if (Config->EnvMapMode == 0 && dwTurn == RENDERTURN_ENVCAM) dwTurn++;
	if (!bIrrad && dwTurn == RENDERTURN_IRRADIANCE) dwTurn++;

	if (dwTurn>RENDERTURN_LAST) dwTurn = 0;

	int RenderCount = max(1, Config->EnvMapFaces);


	// --------------------------------------------------------------------------------------------------------
	// Render Custom Camera view for a focus vessel
	// --------------------------------------------------------------------------------------------------------

	if (dwTurn == RENDERTURN_CUSTOMCAM)
	{
		if (Config->CustomCamMode && (CustomCams.size() > 0))
		{
			if (camCurrent == CustomCams.cend()) camCurrent = CustomCams.cbegin();

			// `OBJHANDLE hVessel = vFocus->GetObjectA();` stood here and is
			// never read -- finding 35's family. The call has no side effect
			// (it returns a member), so it goes with the variable.

			vObject *vO = GetVisObject((*camCurrent)->hVessel);
			double maxd = min(500e3, GetCameraAltitude() + 15e3);

			if (vO->CamDist() < maxd && (*camCurrent)->bActive)
			{
				RenderCustomCameraView((*camCurrent));

				if ((*camCurrent)->pRenderProc) {
					VulkanPad *pSkp = (VulkanPad *)gc->clbkGetSketchpad((*camCurrent)->hSurface);
					pSkp->LoadDefaults();
					(*camCurrent)->pRenderProc(pSkp, (*camCurrent)->pUser);
					gc->clbkReleaseSketchpad(pSkp);
				}
			}
			camCurrent++;
		}
	}


	// -------------------------------------------------------------------------------------------------------
	// Render Environmental Map For the Vessels
	// -------------------------------------------------------------------------------------------------------

	if (dwTurn == RENDERTURN_ENVCAM) {

		if (Config->EnvMapMode) {
			DWORD flags = 0;
			if (Config->EnvMapMode == 1) flags |= 0x01;
			if (Config->EnvMapMode == 2) flags |= (0x03 | 0x20);

			if (vobjEnv == NULL) vobjEnv = vobjFirst;

			while (vobjEnv) {
				if (vobjEnv->type == OBJTP_VESSEL && vobjEnv->apprad>8.0f) {
					if (vobjEnv->vobj) {
						vVessel *vVes = (vVessel *)vobjEnv->vobj;
						if (vVes->RenderENVMap(pDevice, RenderCount, flags) == false) break; // Not yet done with this vessel
					}
				}
				vobjEnv = vobjEnv->next; // Move to the next one
			}
		}
	}



	// -------------------------------------------------------------------------------------------------------
	// Render Irradiance Map For Vessels
	// -------------------------------------------------------------------------------------------------------

	if (dwTurn == RENDERTURN_IRRADIANCE) {

		if (Config->EnvMapMode && Config->bIrradiance) {
			DWORD flags = 0;
			if (Config->EnvMapMode == 1) flags |= 0x01;
			if (Config->EnvMapMode == 2) flags |= (0x03 | 0x20);

			if (vobjIrd == NULL) vobjIrd = vobjFirst;

			while (vobjIrd) {
				if (vobjIrd->type == OBJTP_VESSEL && vobjIrd->apprad>8.0f) {
					if (vobjIrd->vobj) {
						vVessel *vVes = (vVessel *)vobjIrd->vobj;
						if (vVes->ProbeIrradiance(pDevice, RenderCount, flags) == false) break; // Not yet done with this vessel
					}
				}
				vobjIrd = vobjIrd->next; // Move to the next one
			}
		}
	}


	// ---------------------------------------------------------------------------------------------
	// Init. camera setup and create a render list
	// ---------------------------------------------------------------------------------------------

	VOBJREC* pv = NULL;
	VulkanTexture *pShdMap = NULL;

	UpdateCameraFromOrbiter(RENDERPASS_MAINSCENE);
	UpdateCamVis();

	RenderList.clear();

	for (pv = vobjFirst; pv; pv = pv->next) {
		if (!pv->vobj->IsActive()) continue;
		if (!pv->vobj->IsVisible()) continue;
		if (pv->type == OBJTP_VESSEL) {
			vVessel* vV = (vVessel*)pv->vobj;
			RenderList.push_back(vV);
			vV->bStencilShadow = true;
		}
	}

	float znear_for_vessels = ComputeNearClipPlane();




	// ---------------------------------------------------------------------------------------------
	// Start Rendering of Normal and Depth Buffer for SSAO and (point in scene) visibility checks
	// ---------------------------------------------------------------------------------------------

	if (psgBuffer[GBUF_DEPTH] && pDepthNormalDS)
	{
		SetCameraFrustumLimits(0.1f, 1e6f);
		BeginPass(RENDERPASS_NORMAL_DEPTH);

		gc->PushRenderTarget(psgBuffer[GBUF_DEPTH], pDepthNormalDS, RENDERPASS_NORMAL_DEPTH);

		RecallDefaultState();

		// Clear buffers
		pDevice->ClearFrame(true, true, true, 0, 1.0f, 0);

		// Render vessels
		for (auto* vVes : RenderList) vVes->Render(pDevice, false);

		// Render Cockpit
		if (oapiCameraInternal() && vFocus) vFocus->Render(pDevice, true);

		gc->PopRenderTargets();
		PopPass();
	}

	// ---------------------------------------------------------------------------------------------
	// Compute visibility of the Sun and Local light sources. After field depth render ! ! !
	// ---------------------------------------------------------------------------------------------

	ComputeLocalLightsVisibility();


	// -------------------------------------------------------------------------------------------------------
	// Start Main Scene Rendering
	// -------------------------------------------------------------------------------------------------------

	RenderFlags = 0xFFFFFFFF; // Not used for main scene, set to 0xFFFFFFFF 

	// Push main render target and depth surfaces
	//
	gc->PushRenderTarget(pBackBuffer, gc->GetDepthStencil(), RENDERPASS_MAINSCENE);	// Main Scene


	if (DebugControls::IsActive()) {
		pDevice->ClearFrame(true, true, true, 0, 1.0f, 0);
		DWORD flags = *(DWORD*)gc->GetConfigParam(CFGPRM_GETDEBUGFLAGS);
		// D3DFILL_WIREFRAME / D3DFILL_SOLID. See the note on this function:
		// this is the one D3DRS_ in the client that still acts on every draw
		// that follows, so it becomes a device flag both pipeline builders
		// read rather than a PassOverride.
		if (flags&DBG_FLAGS_WIREFRAME) pDevice->SetPolygonMode(VK_POLYGON_MODE_LINE);
		else						   pDevice->SetPolygonMode(VK_POLYGON_MODE_FILL);
	}
	else {
		// Clear the viewport
		pDevice->ClearFrame(true, true, true, 0, 1.0f, 0);
	}



	// Do we use z-clear render mode or not ?
	bool bClearZBuffer = false;
	if ( (GetTargetGroundAltitude() > 2e3) && (oapiCameraInternal() == false)) bClearZBuffer = true;
	if (IsProxyMesh()) bClearZBuffer = false;


	if (DebugControls::IsActive()) {
		DWORD camMode = *(DWORD*)gc->GetConfigParam(CFGPRM_GETCAMERAMODE);
		if (camMode!=0) znear_for_vessels = 0.1f;
	}

	// -------------------------------------------------------------------------------------------------------
	// render celestial sphere background
	// -------------------------------------------------------------------------------------------------------

	// SetRenderState(D3DRS_CULLMODE, D3DCULL_CW) stood here and the matching
	// D3DCULL_CCW below. Neither has a counterpart -- see the note on this
	// function.

	vPlanet *vPl = GetCameraProxyVisual();
	// vPl is assigned and never read, here and in the Windows original. Kept
	// rather than deleted -- deleting it would be a change to the author's
	// code rather than a conversion -- and silenced because GCC warns where
	// MSVC does not. Same treatment as bQuality in RenderBlurredMap.
	(void)vPl;

	// -------------------------------------------------------------------------------------------------------
	// Render the celestial sphere (background image, stars, planetarium features)
	// -------------------------------------------------------------------------------------------------------

	// Set generic clip plane distances for celestial sphere
	SetCameraFrustumLimits(0.1, 10);

	m_celSphere->Render(pDevice, sky_color);

	// Set Initial Near clip plane distance
	if (bClearZBuffer) SetCameraFrustumLimits(1e3, 3e8f);
	else			   SetCameraFrustumLimits(znear_for_vessels, 3e8f);

	// ---------------------------------------------------------------------------------------------
	// Create a caster list for shadow mapping
	// ---------------------------------------------------------------------------------------------

	Casters.clear();

	for (pv = vobjFirst; pv; pv = pv->next) {
		if (!pv->vobj->IsActive()) continue;
		if (pv->type == OBJTP_VESSEL) Casters.push_back((vVessel *)pv->vobj);
	}

	Casters.sort(sort_vessels);



	// ---------------------------------------------------------------------------------------------
	// Render shadow map for vFocus early for surface base and planet rendering
	// ---------------------------------------------------------------------------------------------

	int shadow_lod = -1;
	// shadow_lod is assigned below and never read, here and in the Windows
	// original -- the value that is used comes from a SECOND, inner
	// shadow_lod declared later in this function. Kept rather than deleted,
	// and silenced because GCC warns where MSVC does not. Same treatment as
	// bQuality in RenderBlurredMap.
	(void)shadow_lod;
	float bouble_rad = 10.0f;		// Terrain shadow mapping coverage

	if (Config->ShadowMapMode >= 1 && Config->TerrainShadowing == 2) {

		SmapRenderList.clear();
		SmapRenderList.push_back(vFocus);

		FVECTOR3 ld = sunLight.Dir;
		FVECTOR3 pos = vFocus->GetBoundingSpherePosF();
		float rad = vFocus->GetBoundingSphereRadius();
		float frad = rad;

		vFocus->bStencilShadow = false;


		// What else should be included besides vFocus ?

		for (auto v : Casters)
		{
			if (v == vFocus) continue;
			if (v->HasShadow() == false) continue;

			FVECTOR3 bs_pos = v->GetBoundingSpherePosF();
			float bs_rad = v->GetBoundingSphereRadius();

			if (bs_rad > 80.0) continue;

			FVECTOR3 bc = bs_pos - pos;
			float z = dot(ld, bc);
			if (fabs(z) > 1e3) continue;
			FVECTOR3 fbc = bc - ld * z;
			float dst = length(fbc);
			if (dst > 1e3) continue;

			float nrd = (rad + dst + bs_rad) * 0.5f;

			bool bInclude = false;

			if (dst < (bs_rad + frad)) bInclude = true;
			if (nrd < bouble_rad) bInclude = true;

			if (bInclude) {

				v->bStencilShadow = false;
				SmapRenderList.push_back(v);

				if (nrd < rad) continue;

				if (nrd < bs_rad) {
					pos = bs_pos;
					rad = bs_rad;
				}
				else {
					if (dst > 0.001f) pos = pos + fbc * ((nrd - rad) / dst);
					rad = nrd;
				}
			}
		}

		shadow_lod = RenderShadowMap(pos, ld, rad, false, true);

		pShdMap = smap.pShadowMap;
	}



	// ---------------------------------------------------------------------------------------------
	// Render Planets
	// ---------------------------------------------------------------------------------------------

	DWORD plnmode = *(DWORD*)gc->GetConfigParam(CFGPRM_PLANETARIUMFLAG);
	DWORD mkrmode = *(DWORD*)gc->GetConfigParam(CFGPRM_SURFMARKERFLAG);

	for (DWORD i=0;i<nplanets;i++) {

		// double nplane, fplane;
		// plist[i].vo->RenderZRange (&nplane, &fplane);
		// cam->SetFrustumLimits (nplane, fplane);
		// since we are not using z-buffers here, we can adjust the projection
		// matrix at will to make sure the object is within the viewing frustum

		OBJHANDLE hObj = plist[i].vo->Object();
		bool isActive = plist[i].vo->IsActive();

		if (isActive) plist[i].vo->Render(pDevice);
		else		  plist[i].vo->RenderDot(pDevice);


		VulkanPad *pSketch = GetPooledSketchpad(SKETCHPAD_LABELS);

		if (pSketch) {

			if (isActive) plist[i].vo->RenderVectors(pDevice, pSketch);

			if (mkrmode & MKR_ENABLE) {

				if (mkrmode & MKR_CMARK) {
					VECTOR3 pp;
					char name[256];
					oapiGetObjectName(hObj, name, 256);
					oapiGetGlobalPos(hObj, &pp);

					m_celSphere->EnsureMarkerDrawingContext((oapi::Sketchpad**)&pSketch, 0, m_celSphere->MarkerColor(0), m_celSphere->MarkerPen(0));
					RenderObjectMarker(pSketch, pp, std::string(name), std::string(), 0, viewH / 80);
				}

				if (isActive && (mkrmode & MKR_SURFMARK) && (oapiGetObjectType(hObj) == OBJTP_PLANET))
				{
					int label_format = *(int*)oapiGetObjectParam(hObj, OBJPRM_PLANET_LABELENGINE);
					if (label_format < 2 && (mkrmode & MKR_LMARK)) // user-defined planetary surface labels
					{
						double rad = oapiGetSize(hObj);
						double apprad = rad / (plist[i].dist * tan(GetCameraAperture()));
						const GraphicsClient::LABELLIST *list;
						DWORD n, nlist;
						MATRIX3 prot;
						VECTOR3 ppos, cpos;

						nlist = gc->GetSurfaceMarkers(hObj, &list);

						oapiGetRotationMatrix(hObj, &prot);
						oapiGetGlobalPos(hObj, &ppos);
						VECTOR3 cp = GetCameraGPos();
						cpos = tmul(prot, cp - ppos); // camera in local planet coords

						for (n = 0; n < nlist; n++) {

							if (list[n].active && apprad*list[n].distfac > LABEL_DISTLIMIT) {

								int size = (int)(viewH / 80.0*list[n].size + 0.5);
								int col = list[n].colour;

								m_celSphere->EnsureMarkerDrawingContext((oapi::Sketchpad**)&pSketch, 0, m_celSphere->MarkerColor(col), m_celSphere->MarkerPen(col));
								const std::vector<oapi::GraphicsClient::LABELSPEC>& ls = list[n].marker;
								VECTOR3 sp;
								for (int j = 0; j < int(ls.size()); j++) {
									if (dotp(ls[j].pos, cpos - ls[j].pos) >= 0.0) { // surface point visible?
										sp = mul(prot, ls[j].pos) + ppos;
										RenderObjectMarker(pSketch, sp, ls[j].label[0], ls[j].label[1], list[n].shape, size);
									}
								}
							}
						}
					}

					if (mkrmode & MKR_BMARK) {

						DWORD n = oapiGetBaseCount(hObj);
						MATRIX3 prot;
						oapiGetRotationMatrix(hObj, &prot);
						int size = (int)(viewH / 80.0);

						m_celSphere->EnsureMarkerDrawingContext((oapi::Sketchpad**)&pSketch, 0, m_celSphere->MarkerColor(0), m_celSphere->MarkerPen(0));

						for (DWORD j = 0; j < n; j++) {

							OBJHANDLE hBase = oapiGetBaseByIndex(hObj, j);

							VECTOR3 ppos, cpos, bpos;

							oapiGetGlobalPos(hObj, &ppos);
							oapiGetGlobalPos(hBase, &bpos);
							VECTOR3 cp = GetCameraGPos();
							cpos = tmul(prot, cp - ppos); // camera in local planet coords
							bpos = tmul(prot, bpos - ppos);

							double apprad = 8000e3 / (length(cpos - bpos) * tan(GetCameraAperture()));

							if (dotp(bpos, cpos - bpos) >= 0.0 && apprad > LABEL_DISTLIMIT) { // surface point visible?
								char name[64]; oapiGetObjectName(hBase, name, 63);
								VECTOR3 sp = mul(prot, bpos) + ppos;
								RenderObjectMarker(pSketch, sp, std::string(name), std::string(), 0, size);
							}
						}
					}
				}
			}

			pSketch->EndDrawing();	// SKETCHPAD_LABELS
		}
	}


	// -------------------------------------------------------------------------------------------------------
	// render a user defined planetarium art
	// -------------------------------------------------------------------------------------------------------

	if (plnmode & PLN_ENABLE) {
		VulkanPad *pSketch = GetPooledSketchpad(SKETCHPAD_PLANETARIUM);
		gc->MakeRenderProcCall(pSketch, RENDERPROC_PLANETARIUM, GetViewMatrix(), GetProjectionMatrix());
		pSketch->EndDrawing(); // SKETCHPAD_PLANETARIUM
	}


	// -------------------------------------------------------------------------------------------------------
	// render a user defined exterior art
	// -------------------------------------------------------------------------------------------------------

	if (oapiCameraInternal() == false) {
		VulkanPad *pSketch = GetPooledSketchpad(SKETCHPAD_PLANETARIUM);
		gc->MakeRenderProcCall(pSketch, RENDERPROC_EXTERIOR, GetViewMatrix(), GetProjectionMatrix());
		pSketch->EndDrawing(); // SKETCHPAD_PLANETARIUM
	}

	/*for (DWORD i = 0; i < nplanets; ++i)
	{
		OBJHANDLE hObj = plist[i].vo->Object();
		if (oapiGetObjectType(hObj) != OBJTP_PLANET) continue;
		VulkanPad* pSketch = GetPooledSketchpad(SKETCHPAD_PLANETARIUM);
		pSketch->LoadDefaults();
		pSketch->SetViewMode(Sketchpad::USER);
		pSketch->SetViewProj(GetViewMatrix(), GetProjectionMatrix());
		static_cast<vPlanet*>(plist[i].vo)->TestComputations(pSketch);
		pSketch->EndDrawing(); // SKETCHPAD_PLANETARIUM
	}*/

	// -------------------------------------------------------------------------------------------------------
	// render new-style surface markers
	// -------------------------------------------------------------------------------------------------------

	if ((mkrmode & MKR_ENABLE) && (mkrmode & MKR_LMARK))
	{
		VulkanPad* pSketch = GetPooledSketchpad(SKETCHPAD_LABELS);
		m_celSphere->EnsureMarkerDrawingContext((oapi::Sketchpad**)&pSketch, 0, 0, m_celSphere->MarkerPen(6));

		int fontidx = -1;
		for (DWORD i = 0; i < nplanets; ++i)
		{
			OBJHANDLE hObj = plist[i].vo->Object();
			if (oapiGetObjectType(hObj) != OBJTP_PLANET) { continue; }
			if (!surfLabelsActive) {
				static_cast<vPlanet*>( plist[i].vo )->ActivateLabels(true);
			}

			int label_format = *(int*)oapiGetObjectParam(hObj, OBJPRM_PLANET_LABELENGINE);

			if (label_format == 2)
			{
				static_cast<vPlanet*>(plist[i].vo)->RenderLabels(pDevice, pSketch, label_font, &fontidx);
			}
		}

		pSketch->EndDrawing();	// SKETCHPAD_LABELS

		surfLabelsActive = true;
	}
	else {
		surfLabelsActive = false;
	}


	// -------------------------------------------------------------------------------------------------------
	// render the vessel objects
	// -------------------------------------------------------------------------------------------------------

	// Set near clip plane for vessel exterior rendering
	if (bClearZBuffer) {
		pDevice->ClearFrame(false, true, false, 0, 1.0f, 0); // clear z-buffer
		SetCameraFrustumLimits(znear_for_vessels, 1e8f);
	}


	VulkanEffect::UpdateEffectCamera(Camera.hObj_proxy);


	auto RenderMarkers = RenderList;

	// Render the vessels inside the shadows
	//
	if (Config->ShadowMapMode >= 1) {

		FVECTOR3 ld = sunLight.Dir;
		FVECTOR3 pos = vFocus->GetBoundingSpherePosF();
		float rad = vFocus->GetBoundingSphereRadius();

		int lod = RenderShadowMap(pos, ld, rad);

		if (lod >= 0) {

			pShdMap = ptShmRT[lod];

			auto it = RenderList.begin();

			while (it != RenderList.end()) {
				if ((*it)->IsInsideShadows()) {
					(*it)->Render(pDevice);
					it = RenderList.erase(it);
				}
				else ++it;
			}
		}
	}



	if ((Config->ShadowMapMode >= 2) && (DebugControls::IsActive()==false)) {
		// Don't render more shadows if debug controls are open

		std::list<vVessel *> Intersect;

		// Select the objects to shadow map
		//
		if (Config->ShadowMapMode >= 3) {
			for (auto it = RenderList.begin(); it != RenderList.end(); ++it) {
				if ((*it)->CamDist() < 1e3) Intersect.push_back((*it));
			}
		}
		else {
			for (auto it = RenderList.begin(); it != RenderList.end(); ++it) {
				if ((*it)->IntersectShadowTarget()) Intersect.push_back((*it));
			}
		}


		while (!Intersect.empty()) {

			FVECTOR3 ld = sunLight.Dir;
			FVECTOR3 pos = Intersect.front()->GetBoundingSpherePosF();
			float rad = Intersect.front()->GetBoundingSphereRadius();

			Intersect.pop_front();

			int lod = RenderShadowMap(pos, ld, rad);

			if (lod >= 0) {

				// Render objects in shadow
				auto it = RenderList.begin();

				while (it != RenderList.end()) {
					if ((*it)->IsInsideShadows()) {
						(*it)->Render(pDevice);
						Intersect.remove((*it));
						it = RenderList.erase(it);
					}
					else ++it;
				}
			}
		}
	}


	// Render the remaining vessels those are not yet renderred
	//
	smap.pShadowMap = NULL;

	while (RenderList.empty()==false) {
		RenderList.front()->Render(pDevice);
		RenderList.pop_front();
	}

	VulkanPad* pSketch = GetPooledSketchpad(SKETCHPAD_LABELS);
	if (pSketch) {
		m_celSphere->EnsureMarkerDrawingContext((oapi::Sketchpad**)&pSketch, 0, m_celSphere->MarkerColor(0), m_celSphere->MarkerPen(0));
		for (auto x : RenderMarkers) RenderVesselMarker(x, pSketch);
		pSketch->EndDrawing();	// SKETCHPAD_LABELS
	}



	// -------------------------------------------------------------------------------------------------------
	// render custom user objects
	// -------------------------------------------------------------------------------------------------------

	if (oapiCameraInternal() == false) {
		if (gc->IsGenericProcEnabled(GENERICPROC_RENDER_EXTERIOR)) {
			gc->MakeGenericProcCall(GENERICPROC_RENDER_EXTERIOR, 0, NULL);
		}
	}


	// -------------------------------------------------------------------------------------------------------
	// render the vessel sub-systems
	// -------------------------------------------------------------------------------------------------------

	// render exhausts
	//
	for (pv=vobjFirst; pv; pv=pv->next) {
		if (!pv->vobj->IsActive() || !pv->vobj->IsVisible() || pv->vobj->GetMeshCount() < 1) continue;
		OBJHANDLE hObj = pv->vobj->Object();
		if (oapiGetObjectType(hObj) == OBJTP_VESSEL) {
			((vVessel*)pv->vobj)->RenderExhaust();
		}
	}

	// render beacons
	//
	for (pv=vobjFirst; pv; pv=pv->next) {
		if (!pv->vobj->IsActive()) continue;
		pv->vobj->RenderBeacons(pDevice);
	}

	// render grapple points
    //
    for (pv=vobjFirst; pv; pv=pv->next) {
        if (!pv->vobj->IsActive()) continue;
        pv->vobj->RenderGrapplePoints(pDevice);
    }

	// render exhaust particle system
	//
	for (DWORD n = 0; n < nstream; n++) pstream[n]->Render(pDevice);


	// -------------------------------------------------------------------------------------------------------
	// Render vessel axis vectors
	// -------------------------------------------------------------------------------------------------------

	DWORD bfvmode = *(DWORD*)gc->GetConfigParam(CFGPRM_FORCEVECTORFLAG);
	DWORD favmode = *(DWORD*)gc->GetConfigParam(CFGPRM_FRAMEAXISFLAG);

	if (bfvmode & BFV_ENABLE || favmode & FAV_ENABLE)
	{

		pDevice->ClearFrame(false, true, false, 0, 1.0f, 0); // clear z-buffer

		pSketch = GetPooledSketchpad(SKETCHPAD_LABELS);
		pSketch->SetFont(pAxisFont);
		pSketch->SetTextAlign(Sketchpad::LEFT, Sketchpad::TOP);

		for (pv=vobjFirst; pv; pv=pv->next) {
			if (!pv->vobj->IsActive()) continue;
			if (!pv->vobj->IsVisible()) continue;
			if (oapiCameraInternal() && vFocus==pv->vobj) continue;

			pv->vobj->RenderVectors(pDevice, pSketch);
		}

		pSketch->EndDrawing();	// SKETCHPAD_LABELS
	}





	// -------------------------------------------------------------------------------------------------------
	// render the internal parts of the focus object in a separate render pass
	// -------------------------------------------------------------------------------------------------------

	// Whether the internal pass runs at all, and why not when it does not.
	// Reported for the first few frames only. Diagnostic only; env-gated.
	static const bool bTraceVC = (getenv("ORBITER_VK_TRACE_VC") != NULL);
	if (bTraceVC) {
		static int nRep = 0;
		if (nRep < 6) {
			nRep++;
			LogErr("VCTRACE Scene internal pass: oapiCameraInternal=%d vFocus=%s",
				   int(oapiCameraInternal() ? 1 : 0), _PTR(vFocus));
		}
	}

	if (oapiCameraInternal() && vFocus) {

		// switch cockpit lights on, external-only lights off
		//
		if (bLocalLight) {
			ClearLocalLights();
			VESSEL *vessel = oapiGetFocusInterface();
			DWORD nemitter = vessel->LightEmitterCount();
			for (DWORD j = 0; j < nemitter; j++) {
				const LightEmitter *em = vessel->GetLightEmitter(j);
				if ((em->GetVisibility() == LightEmitter::VIS_COCKPIT) || (em->GetVisibility() == LightEmitter::VIS_ALWAYS))
					AddLocalLight(em, vFocus);
			}
		}

		pDevice->ClearFrame(false, true, false, 0, 1.0f, 0); // clear z-buffer
		double znear = Config->VCNearPlane;
		if (znear<0.01) znear=0.01;
		if (znear>1.0)  znear=1.0;
		OBJHANDLE hFocus = oapiGetFocusObject();
		SetCameraFrustumLimits(znear, oapiGetSize(hFocus)*2.0);

		// The frustum the virtual cockpit is actually drawn with, and the
		// depth row of the matrix it produced. Diagnostic only; env-gated.
		if (bTraceVC) {
			static int nRep = 0;
			if (nRep < 4) {
				nRep++;
				LogErr("VCTRACE frustum: znear=%.4f zfar=%.2f ap=%.4f asp=%.4f "
					   "VP.m33=%.6f m34=%.6f m43=%.6f m44=%.6f",
					   Camera.nearplane, Camera.farplane, Camera.aperture, Camera.aspect,
					   Camera.mProjView.m33, Camera.mProjView.m34,
					   Camera.mProjView.m43, Camera.mProjView.m44);
			}
		}

		vFocus->Render(pDevice, true);
	}

	// D3DFILL_SOLID, putting back what the wireframe debug flag above may have
	// set. It is the one D3DRS_ in this file that still has a counterpart to
	// restore, because it is genuinely global; see the note on this function.
	pDevice->SetPolygonMode(VK_POLYGON_MODE_FILL);


	// End Of Main Scene Rendering ---------------------------------------------
	//








	// -------------------------------------------------------------------------------------------------------
	// Copy Offscreen render target to backbuffer
	// -------------------------------------------------------------------------------------------------------


	if (pOffscreenTarget && pLightBlur) {

		int iGensPerFrame = pLightBlur->FindDefine("PassCount");

		// GetDesc() on two surfaces; the client's own record instead.
		const VulkanImageDesc &blur = psgBuffer[GBUF_BLUR]->Desc();

		// `D3DXVECTOR2 scr` was computed from the colour buffer's size and is
		// never read -- finding 35's family. Its query goes with it; only sbf
		// reaches a shader.
		FVECTOR2 sbf = FVECTOR2(1.0f / float(blur.Width), 1.0f / float(blur.Height));


		if (pLightBlur->IsOK())
		{
			float fInt = float(Config->GFXIntensity);
			float fDst = float(Config->GFXDistance);
			float fThr = float(Config->GFXThreshold);
			float fGam = float(Config->GFXGamma);

			// Grap a copy of a backbuffer.
			//
			// StretchRect(src, NULL, dst, NULL, D3DTEXF_POINT) ->
			// BlitTexture(dst, NULL, src, NULL, false). NOTE THE ARGUMENT
			// ORDER: StretchRect named the SOURCE first and this names the
			// DESTINATION first, matching memcpy and the two-argument form.
			// D3DTEXF_POINT is bLinear = false. See VulkanFrame.h.
			pDevice->BlitTexture(psgBuffer[GBUF_COLOR], NULL, pOffscreenTarget, NULL, false);

			pLightBlur->SetFloat("vSB", &sbf, sizeof(FVECTOR2));
			pLightBlur->SetBool("bBlendIn", false);
			pLightBlur->SetBool("bBlur", false);

			pLightBlur->SetFloat("fIntensity", &fInt, sizeof(float));
			pLightBlur->SetFloat("fDistance", &fDst, sizeof(float));
			pLightBlur->SetFloat("fThreshold", &fThr, sizeof(float));
			pLightBlur->SetFloat("fGamma", &fGam, sizeof(float));	

			// -----------------------------------------------------
			pLightBlur->SetBool("bSample", true);
			pLightBlur->SetTextureNative("tBack", ptgBuffer[GBUF_COLOR], IPF_POINT | IPF_CLAMP);
			pLightBlur->SetOutputNative(0, psgBuffer[GBUF_BLUR]);

			if (!pLightBlur->Execute(true)) LogErr("pLightBlur Execute Failed");

			// -----------------------------------------------------
			pLightBlur->SetBool("bSample", false);
			pLightBlur->SetBool("bBlur", true);

			for (int i = 0; i < iGensPerFrame; i++) {

				pLightBlur->SetBool("bDir", false);
				pLightBlur->SetTextureNative("tBlur", ptgBuffer[GBUF_BLUR], IPF_POINT | IPF_CLAMP);
				pLightBlur->SetOutputNative(0, psgBuffer[GBUF_TEMP]);

				if (!pLightBlur->Execute(true)) LogErr("pLightBlur Execute Failed");

				pLightBlur->SetBool("bDir", true);
				pLightBlur->SetTextureNative("tBlur", ptgBuffer[GBUF_TEMP], IPF_POINT | IPF_CLAMP);
				pLightBlur->SetOutputNative(0, psgBuffer[GBUF_BLUR]);

				if (!pLightBlur->Execute(true)) LogErr("pLightBlur Execute Failed");
			}

			pLightBlur->SetBool("bBlendIn", true);
			pLightBlur->SetBool("bBlur", false);
			pLightBlur->SetTextureNative("tBack", ptgBuffer[GBUF_COLOR], IPF_LINEAR | IPF_CLAMP);
			pLightBlur->SetTextureNative("tBlur", ptgBuffer[GBUF_BLUR], IPF_LINEAR | IPF_CLAMP);
			pLightBlur->SetOutputNative(0, gc->GetBackBuffer());

			if (!pLightBlur->Execute(true)) LogErr("pLightBlur Execute Failed");
		}
		else {
			LogErr("pLightBlur is not o.k.");
		}
	}


	// -------------------------------------------------------------------------------------------------------
	// Render glares for the Sun and local lights
	// -------------------------------------------------------------------------------------------------------

	gc->PushRenderTarget(gc->GetBackBuffer(), gc->GetDepthStencil(), RENDERPASS_MAINSCENE);	
	RenderGlares();
	gc->PopRenderTargets();


	// -------------------------------------------------------------------------------------------------------
	// Render GDI Overlay to backbuffer directly
	// -------------------------------------------------------------------------------------------------------

	if (pGDIOverlay)
	{
		if (pGDIOverlay->IsOK())
		{
			gc->bGDIClear = true; // Must clear background before continuing drawing into overlay
			// D3DXCOLOR's DWORD constructor reads 0xAARRGGBB; FVECTOR4's reads
			// 0xAABBGGRR. FCOLOR_ARGB is the one that means what this line
			// means -- see VulkanUtil.h. Getting it backwards would make the
			// colour key a different colour and the overlay opaque.
			FVECTOR4 clr = FCOLOR_ARGB(0x4080F0); // RGB ColorKey
			pGDIOverlay->SetTextureNative("tSrc", ptgBuffer[GBUF_GDI], IPF_POINT | IPF_CLAMP);
			pGDIOverlay->SetFloat("vColorKey", &clr, sizeof(clr));
			pGDIOverlay->SetOutputNative(0, gc->GetBackBuffer());
			if (!pGDIOverlay->Execute(true)) LogErr("pGDIOverlay Execute Failed");
		}
		else
		{
			LogErr("pGDIOverlay is not OK.");
		}
	}

	/*if (Camera.vNear) {
		VulkanDebugLog("vNear = %s", Camera.vNear->GetName());
		VulkanDebugLog("vProxy = %s", Camera.vProxy->GetName());
		VulkanDebugLog("vGravRef = %s", Camera.vGravRef->GetName());
	}*/

	// -------------------------------------------------------------------------------------------------------
	// Render HUD Overlay to backbuffer directly
	// -------------------------------------------------------------------------------------------------------


	gc->PushRenderTarget(gc->GetBackBuffer(), gc->GetDepthStencil(), RENDERPASS_MAINOVERLAY);	// Overlay

	pSketch = GetPooledSketchpad(SKETCHPAD_2D_OVERLAY);

	if (pSketch) {
		gc->MakeRenderProcCall(pSketch, RENDERPROC_HUD_1ST, NULL, NULL);
		pSketch->EndDrawing(); // SKETCHPAD_2D_OVERLAY
	}
	gc->Render2DOverlay();
	pSketch = GetPooledSketchpad(SKETCHPAD_2D_OVERLAY);
	if (pSketch) {
		gc->MakeRenderProcCall(pSketch, RENDERPROC_HUD_2ND, NULL, NULL);
		pSketch->EndDrawing(); // SKETCHPAD_2D_OVERLAY
	}


	// Enable Freeze mode after the main scene is complete
	//
	if (bFreezeEnable) bFreeze = true;

	
	// -------------------------------------------------------------------------------------------------------
	// EnvMap Debugger  TODO: Should be allowed to visualize other maps as well, not just index 0
	// -------------------------------------------------------------------------------------------------------

	if (DebugControls::IsActive()) {
		
		int sel = DebugControls::GetSelectedEnvMap();

		switch (sel) {
		case 1:		case 2:		case 3:		case 4:
		case 5:
			VisualizeCubeMap(vFocus->GetEnvMap(ENVMAP_MAIN), sel - 1);
			break;
		case 6:
			VisualizeCubeMap(vFocus->GetIrradEnv(), 0);
			break;
		case 7:
			VisualizeCubeMap(pIrradTemp, 0);
			break;
		case 8:
			if (pShdMap) {
				pSketch = GetPooledSketchpad(SKETCHPAD_2D_OVERLAY);
				pSketch->CopyRectNative(pShdMap, NULL, 0, 0);
				pSketch->EndDrawing();
			}
			break;
		case 9:
			if (vFocus->GetIrradianceMap()) {
				pSketch = GetPooledSketchpad(SKETCHPAD_2D_OVERLAY);
				pSketch->CopyRectNative(vFocus->GetIrradianceMap(), NULL, 0, 0);
				pSketch->EndDrawing();
			}
			break;
		case 10:
			if (ptgBuffer[GBUF_BLUR]) {
				pSketch = GetPooledSketchpad(SKETCHPAD_2D_OVERLAY);
				pSketch->CopyRectNative(ptgBuffer[GBUF_BLUR], NULL, 0, 0);
				pSketch->EndDrawing();
			}
			break;
		case 11:
			if (ptgBuffer[GBUF_DEPTH]) {
				if (pVisDepth) {
					if (pVisDepth->IsOK()) {
						pVisDepth->Activate("PSDepth");
						pVisDepth->SetTextureNative("tBack", ptgBuffer[GBUF_DEPTH], IPF_POINT | IPF_CLAMP);
						pVisDepth->SetOutputNative(0, gc->GetBackBuffer());
						pVisDepth->Execute(true);
					}
				}
			}
			break;
		case 12:
			if (ptgBuffer[GBUF_DEPTH]) {
				if (pVisDepth) {
					if (pVisDepth->IsOK()) {
						pVisDepth->Activate("PSNormal");
						pVisDepth->SetTextureNative("tBack", ptgBuffer[GBUF_DEPTH], IPF_POINT | IPF_CLAMP);
						pVisDepth->SetOutputNative(0, gc->GetBackBuffer());
						pVisDepth->Execute(true);
					}
				}
			}
			break;
		case 13:
			if (pLocalResults) {
				pSketch = GetPooledSketchpad(SKETCHPAD_2D_OVERLAY);
				pSketch->SetBlendState(Sketchpad::BlendState::FILTER_POINT);
				pSketch->StretchRectNative(pLocalResults, NULL, ptr(_RECT(0, 0, viewW, 10)));
				pSketch->SetBlendState(Sketchpad::BlendState::FILTER_LINEAR);
				pSketch->EndDrawing();
			}
			break;
		case 14:
			if (Camera.vNear) {
				auto ptE = Camera.vNear->GetEclipse();
				if (ptE) {
					pSketch = GetPooledSketchpad(SKETCHPAD_2D_OVERLAY);
					pSketch->SetBlendState(Sketchpad::BlendState::FILTER_POINT);
					pSketch->StretchRectNative(ptE, NULL, ptr(_RECT(0, 0, viewW, 10)));
					pSketch->SetBlendState(Sketchpad::BlendState::FILTER_LINEAR);
					pSketch->EndDrawing();
				}
			}
			break;
		default:
			break;
		}
	}


	if (AtmoControls::Visualize())
	{
		vPlanet* vP = GetCameraProxyVisual();
		pSketch = GetPooledSketchpad(SKETCHPAD_2D_OVERLAY);
		pSketch->SetBlendState(Sketchpad::COPY);
		int x = 0, y = ViewH();

		// `pTab->GetLevelDesc(0, &desc)` asked the runtime for level 0's size,
		// which IS the texture's own size. Width()/Height() are that record.
		VulkanTexture *pTab = vP->GetScatterTable(RAY_LAND);
		if (pTab) {
			pSketch->StretchRectNative(pTab, NULL, ptr(_R(0, y - int(pTab->Height()), int(pTab->Width()), y)));
			y -= int(pTab->Height() + 5);
		}
		pTab = vP->GetScatterTable(MIE_LAND);
		if (pTab) {
			pSketch->StretchRectNative(pTab, NULL, ptr(_R(0, y - int(pTab->Height()), int(pTab->Width()), y)));
			y -= int(pTab->Height() + 5);
		}
		pTab = vP->GetScatterTable(ATN_LAND);
		if (pTab) {
			pSketch->StretchRectNative(pTab, NULL, ptr(_R(0, y - int(pTab->Height()), int(pTab->Width()), y)));
			y -= int(pTab->Height() + 5);
		}
		for (int i=0;i<9;i++)
		{
			if (i == RAY_LAND || i == MIE_LAND || i == ATN_LAND) continue;
			pTab = vP->GetScatterTable(i);
			if (!pTab) continue;
			pSketch->CopyRectNative(pTab, NULL, x, y - int(pTab->Height()));
			x += int(pTab->Width() + 5);
		}
		pSketch->SetBlendState(Sketchpad::ALPHABLEND);
		pSketch->EndDrawing();
	}


	// -------------------------------------------------------------------------------------------------------
	// Draw Debug String on a bottom of the screen
	// -------------------------------------------------------------------------------------------------------

	const char* dbgString = oapiDebugString();
	int len = lstrlen(dbgString);

	if (len>0 || !VulkanDebugQueue.empty()) {

		pSketch = GetPooledSketchpad(SKETCHPAD_DEBUG_TEXT);

		DWORD height = Config->DebugFontSize;

		// Display Orbiter's debug string
		if (len > 0) {
			DWORD width = pSketch->GetTextWidth(dbgString, len);
			pSketch->Rectangle(-1, viewH - height - 1, width + 4, viewH);
			pSketch->Text(2, viewH - 2, dbgString, len);
		}

		DWORD pos = viewH;

		// Display additional debug string queue
		//
		while (!VulkanDebugQueue.empty()) {
			pos -= (height * 3) / 2;
			std::string str = VulkanDebugQueue.front();
			len = lstrlen(str.c_str());
			DWORD width = pSketch->GetTextWidth(str.c_str(), len);
			pSketch->Rectangle(-1, pos - height - 1, width + 4, pos);
			pSketch->Text(2, pos - 2, str.c_str(), len);
			VulkanDebugQueue.pop();
		}

		pSketch->EndDrawing(); // SKETCHPAD_DEBUG_TEXT
	}


	gc->PopRenderTargets();	// Overlay
	gc->PopRenderTargets();	// Main Scene

	gc->HackFriendlyHack();
	gc->EndScene();

	dwTurn++;
}



// ===========================================================================================
//
void Scene::RenderVesselMarker(vVessel *vV, VulkanPad *pSketch)
{
	DWORD mkrmode = *(DWORD*)gc->GetConfigParam(CFGPRM_SURFMARKERFLAG);
	if ((mkrmode & (MKR_ENABLE | MKR_VMARK)) == (MKR_ENABLE | MKR_VMARK)) {
		RenderObjectMarker(pSketch, vV->GlobalPos(), std::string(vV->GetName()), std::string(), 0, viewH / 80);
	}
}


// ===========================================================================================
// Lens flare code (SolarLiner)
//
// D3DXVec4Transform(&out, &v, pM) is oapi::mul(v, M) -- DrawAPI.h's own
// row-vector multiply, which returns its result. Same convention, no
// transpose: see VulkanUtil.h's note on VMAT_MatrixMultiply.
//
Scene::SUNVISPARAMS Scene::GetSunScreenVisualState()
{
	SUNVISPARAMS result = SUNVISPARAMS();

	VECTOR3 cam = GetCameraGPos();
	VECTOR3 sunGPos;
	oapiGetGlobalPos(oapiGetGbodyByIndex(0), &sunGPos);
	sunGPos -= cam;

	DWORD w, h;
	oapiGetViewportSize(&w, &h);

	// `const LPD3DXMATRIX pVP` was a const POINTER to a non-const matrix and
	// the getter cast the const away to produce it; see Scene.h.
	const FMATRIX4 *pVP = GetProjectionViewMatrix();
	FVECTOR4 sun = FVECTOR4(float(sunGPos.x), float(sunGPos.y), float(sunGPos.z), 1.0f);
	FVECTOR4 pos = mul(sun, *pVP);
	result.brightness = saturate(pos.z);

	FVECTOR2 scrPos = FVECTOR2(pos.x, pos.y);
	// FVECTOR2's operator/= and operator*= take a NON-CONST FVECTOR2&, so
	// `scrPos /= pos.w` cannot compile: the float has to become an FVECTOR2
	// first, and a temporary will not bind to a non-const reference under
	// GCC. Named intermediates, therefore, and the arithmetic is unchanged --
	// FVECTOR2(q) sets both components to q, so dividing by it is dividing
	// both by pos.w. Same finding as VectorHelpers.h's abs()/pow(), recorded
	// there.
	FVECTOR2 vW(pos.w);
	FVECTOR2 vHalf(0.5f);
	scrPos /= vW;
	scrPos *= vHalf;
	scrPos.x *= float(w) / float(h);

	result.position = scrPos;
	result.position.x *= 1.8f;

	short xpos = short((scrPos.x + 0.5f) * w);
	short ypos = short((1.0f - (scrPos.y + 0.5f)) * h);

	VulkanPick pick = PickScene(xpos, ypos);

	if (pick.pMesh != NULL)
	{
		DWORD matIndex = pick.pMesh->GetMeshGroupMaterialIdx(pick.group);
		VulkanMatExt material;
		pick.pMesh->GetMaterial(&material, matIndex);
		FVECTOR4 surfCol(material.Diffuse.x, material.Diffuse.y, material.Diffuse.z, material.Diffuse.w);

		result.visible = (surfCol.a != 1.0f);
		if (result.visible)
		{
			FVECTOR4 color = FVECTOR4(surfCol.r*surfCol.a, surfCol.g*surfCol.a, surfCol.b*surfCol.a, 1.0f);
			// `color += GetSunDiffColor() * (1 - surfCol.a);` -- FVECTOR4 has
			// += against a FLOAT only, so the vector form is spelled out.
			// Same finding as Mesh.cpp's ProcessColor.
			color = color + GetSunDiffColor() * (1.0f - surfCol.a);

			result.color = color;
		}
		return result;
	}
	result.visible = true;
	result.color = GetSunDiffColor();

	return result;
}


// ===========================================================================================
// Lens flare code (SolarLiner)
//
// Was D3DXCOLOR. The one line that needed more than a rename is the early
// return: `return GetSun()->Color;` relied on D3DXVECTOR3 converting
// implicitly to a D3DXCOLOR with alpha 1. FVECTOR3 has no such conversion to
// FVECTOR4, so the alpha is written.
//
FVECTOR4 Scene::GetSunDiffColor()
{
	vPlanet *vP = Camera.vProxy;

	FVECTOR3 _one(1.0f, 1.0f, 1.0f);
	VECTOR3 GS, GP, GO;
	oapiCameraGlobalPos(&GO);

	OBJHANDLE hS = oapiGetGbodyByIndex(0);	// the central star
	OBJHANDLE hP = vP->Object();			// the planet object
	oapiGetGlobalPos(hS, &GS);				// sun position
	oapiGetGlobalPos(hP, &GP);				// planet position

	VECTOR3 S = GS - GO;						// sun's position from object
	VECTOR3 P = GO - GP;

	double s = length(S);

	if (hP == hS) return FVECTOR4(GetSun()->Color, 1.0f);

	double r = length(P);
	double pres = 1000.0;
	double size = oapiGetSize(hP) + vP->GetMinElevation();
	double grav = oapiGetMass(hP) * 6.67259e-11 / (size*size);

	float aalt = 1.0f;
	//float amb0 = 0.0f;
	float disp = 0.0f;
	float al = 0.0f;
	float k = float(sqrt(r*r - size*size));		// Horizon distance
	float alt = float(r - size);
	float rs = float(oapiGetSize(hS) / s);
	float ac = float(-dotp(S, P) / (r*s));					// sun elevation

															// Avoid some fault conditions
	if (alt<0) alt = 0, k = 1e3, size = r;

	// Two statements on one line in the Windows original; split for
	// -Wmisleading-indentation, exactly as in ComputeNearClipPlane. Formatting
	// only -- both clamps are unchanged.
	if (ac>1.0f) ac = 1.0f;
	if (ac<-1.0f) ac = -1.0f;

	ac = acos(ac) - asin(float(size / r));

	if (ac>1.39f)  ac = 1.39f;
	if (ac<-1.39f) ac = -1.39f;

	float h = tan(ac);

	const ATMCONST *atm = (oapiGetObjectType(hP) == OBJTP_PLANET ? oapiGetPlanetAtmConstants(hP) : NULL);

	if (atm) {
		aalt = float(atm->p0 * log(atm->p0 / pres) / (atm->rho0*grav));
		//amb0 = float(min(0.7, log(atm->rho0 + 1.0f)*0.4));
		disp = float(std::max(0.02, std::min(0.9, log(atm->rho0 + 1.0))));
	}

	if (alt>10e3f) al = aalt / k;
	else           al = 0.173f;

	FVECTOR3 lcol(1.0f, 1.0f, 1.0f);
	//FVECTOR3 r0 = _one - FVECTOR3(0.65f, 0.75f, 1.0f) * disp;
	FVECTOR3 r0 = _one - FVECTOR3(1.15f, 1.65f, 2.35f) * disp;

	if (atm) {
		float x = sqrt(saturate(h / al));
		float y = sqrt(saturate((h + rs) / (2.0f*rs)));
		lcol = (r0 + (_one - r0) * x) * y;
	}
	else {
		lcol = r0 * saturate((h + rs) / (2.0f*rs));
	}

	return FVECTOR4(lcol.x, lcol.y, lcol.z, 1.0f);
}



// ===========================================================================================
// D3DXVECTOR3 becomes FVECTOR3 in the signature, which is the same three
// floats; D3DXVec3Length becomes length(), which VectorHelpers.h already has.
//
// D3DXMatrixOrthoOffCenterRH and D3DXMatrixLookAtRH become VMAT_OrthoOffCenterRH
// and VMAT_LookAtRH, written out in VulkanUtil.cpp from the D3DX
// documentation's own definitions -- D3DX is a Direct3D utility library with
// no Vulkan counterpart, and this is the same treatment every other D3DXMatrix
// call in the client gets. D3DXMatrixMultiply becomes VMAT_Multiply.
//
// ptr(D3DXVECTOR3(0,1,0)) loses its ptr(): the helper existed to take the
// address of a temporary, which C++ forbids, and VMAT_LookAtRH takes const
// pointers so a named local is needed instead. Same value, one line longer.
//
// THE CLEAR IS THE ONE REAL SUBSTITUTION. pDevice->Clear(0, NULL, TARGET |
// ZBUFFER, 0, 1.0f, 0) becomes ClearFrame(true, true, false, 0, 1.0f, 0) --
// vkCmdClearAttachments, which clears the attachments of the pass currently
// open. See VulkanFrame.h on why that is not the same call as ClearImage.
// ===========================================================================================
int Scene::RenderShadowMap(FVECTOR3 &pos, FVECTOR3 &ld, float rad, bool bInternal, bool bListExists)
{
	rad *= 1.02f;

	smap.pos = pos;
	smap.ld = ld;
	smap.rad = rad;

	float mnd =  1e16f;
	float mxd = -1e16f;
	float rsmax = 0.0f;
	float tanap = float(GetTanAp());
	float viewh = float(ViewH());

	if (!bListExists) {

		// If the list doesn't exists then create it...
		SmapRenderList.clear();

		// browse through vessels to find shadowers --------------------------
		//
		for (VOBJREC *pv = vobjFirst; pv; pv = pv->next) {
			if (pv->type != OBJTP_VESSEL) continue;
			vVessel *vV = (vVessel *)pv->vobj;
			if (!vV->IsActive()) continue;
			if (vV->IntersectShadowVolume()) {
				SmapRenderList.push_back(vV);
				vV->GetMinMaxLightDist(&mnd, &mxd);
			}
		}

		// Compute shadow lod
		rsmax = viewh * rad / (tanap * length(pos));
	}


	if (SmapRenderList.size() == 0) return -1;	// The list is empty, Nothing to render


	if (bListExists) {

		for (auto vV : SmapRenderList)
		{
			// Get shadow min-max distances
			vV->GetMinMaxLightDist(&mnd, &mxd);

			// Compute shadow lod
			// GetBoundingSpherePosDX() became GetBoundingSpherePosF(); see
			// the note at the top of VObject.h.
			FVECTOR3 bspos = vV->GetBoundingSpherePosF();
			float rs = viewh * rad / (tanap * length(bspos));
			if (rs > rsmax) rsmax = rs;
		}
	}

	smap.depth = (mxd - mnd) + 10.0f;

	VMAT_OrthoOffCenterRH(&smap.mProj, -rad, rad, rad, -rad, 50.0f, 50.0f + smap.depth);

	smap.dist = mnd - 55.0f;

	FVECTOR3 lp = pos + ld * smap.dist;

	FVECTOR3 up(0, 1, 0);
	VMAT_LookAtRH(&smap.mView, &lp, &pos, &up);
	VMAT_MatrixMultiply(&smap.mViewProj, &smap.mView, &smap.mProj);

	float lod = log2f(float(Config->ShadowMapSize) / (rsmax*1.5f));

	smap.lod = min(int(round(lod)), SHM_LOD_COUNT - 1);
	smap.lod = max(smap.lod, 0);
	smap.size = Config->ShadowMapSize >> smap.lod;

	gc->PushRenderTarget(psShmRT[smap.lod], psShmDS[smap.lod], RENDERPASS_SHADOWMAP);

	// Clear the viewport
	pDevice->ClearFrame(true, true, false, 0, 1.0f, 0);


	// render the vessel objects --------------------------------
	//
	BeginPass(RENDERPASS_SHADOWMAP);

	while(SmapRenderList.size()>0) {
		SmapRenderList.front()->Render(pDevice, bInternal);
		SmapRenderList.pop_front();
	}

	PopPass();

	gc->PopRenderTargets();

	smap.pShadowMap = ptShmRT[smap.lod];

	return smap.lod;
}



// ===========================================================================================
// D3D9Effect::UpdateEffectCamera becomes VulkanEffect::UpdateEffectCamera --
// the class was renamed, nothing else. The Clear is the same substitution as
// in RenderShadowMap above; note that this one clears the STENCIL as well and
// to 0xFF000000 rather than 0.
// ===========================================================================================
void Scene::RenderSecondaryScene(std::set<vVessel*> &RndList, std::set<vVessel*> &LightsList, DWORD flags)
{
	_TRACE;
	RenderFlags = flags;

	// Process Local Light Sources -------------------------------------
	// And toggle external lights on
	//
	if (bLocalLight) {

		ClearLocalLights();

		for (auto vVes : RndList) {
			if (!vVes->IsActive()) continue;
			VESSEL *vessel = vVes->GetInterface();
			DWORD nemitter = vessel->LightEmitterCount();
			for (DWORD j = 0; j < nemitter; j++) {
				const LightEmitter *em = vessel->GetLightEmitter(j);
				if ((em->GetVisibility() == LightEmitter::VIS_EXTERNAL) || (em->GetVisibility() == LightEmitter::VIS_ALWAYS)) AddLocalLight(em, vVes);
			}		
		}

		for (auto vVes : LightsList) {
			if (!vVes->IsActive()) continue;
			if (RndList.count(vVes)) continue; // Already included skip it
			VESSEL *vessel = vVes->GetInterface();
			DWORD nemitter = vessel->LightEmitterCount();
			for (DWORD j = 0; j < nemitter; j++) {
				const LightEmitter *em = vessel->GetLightEmitter(j);
				if ((em->GetVisibility() == LightEmitter::VIS_EXTERNAL) || (em->GetVisibility() == LightEmitter::VIS_ALWAYS)) AddLocalLight(em, vVes);
			}
		}
	}

	VulkanEffect::UpdateEffectCamera(GetCameraProxyBody());

	// Clear the viewport
	pDevice->ClearFrame(true, true, true, 0xFF000000, 1.0f, 0);

	
	// render planets -------------------------------------------
	//
	if (flags & 0x01) {
		for (DWORD i = 0; i<nplanets; i++) {
			bool isActive = plist[i].vo->IsActive();
			if (isActive) plist[i].vo->Render(pDevice);
			else		  plist[i].vo->RenderDot(pDevice);
		}
	}

	// render the vessel objects --------------------------------
	//
	if (flags & 0x02) {
		for (auto vVes : RndList) {
			if (!vVes->IsActive()) continue;
			if (!vVes->IsVisible()) continue;
			vVes->Render(pDevice);
		}
	}

	// render exhausts -------------------------------------------
	//
	if (flags & 0x04) {
		for (auto vVes : RndList) {
			if (!vVes->IsActive()) continue;
			if (!vVes->IsVisible()) continue;
			vVes->RenderExhaust();
		}
	}

	// render beacons -------------------------------------------
	//
	if (flags & 0x08) {
		for (auto vVes : RndList) {
			if (!vVes->IsActive()) continue;
			vVes->RenderBeacons(pDevice);
		}
	}

	// render exhaust particle system ----------------------------
	if (flags & 0x10) {
		for (DWORD n = 0; n < nstream; n++) pstream[n]->Render(pDevice);
	}

	// Flags 0x20 = BaseStructures
}



// ===========================================================================================
// The two cube-map idioms in this function and the next are the same pair
// everywhere they appear, so they are argued once here.
//
//   D3DXCreateCubeTexture(pDev, size, mips, D3DUSAGE_RENDERTARGET, fmt,
//                         D3DPOOL_DEFAULT, &pCube)
// becomes
//   pDev->CreateTextureCube(size, mips, fmt, usage)
//
// A cube map is not a separate interface here: it is a VK_IMAGE_TYPE_2D image
// with six array layers and CUBE_COMPATIBLE, viewed as VIEW_TYPE_CUBE. See
// VulkanFrame.h. D3DPOOL_DEFAULT has no counterpart -- there is one pool --
// and D3DUSAGE_RENDERTARGET becomes COLOR_ATTACHMENT_BIT in the usage flags,
// where SAMPLED and the two TRANSFER bits are added because these images are
// also read by a shader and blitted between.
//
//   pCube->GetCubeMapSurface(D3DCUBEMAP_FACES(i), mip, &pSrf) ... SAFE_RELEASE
// becomes
//   pSrf = pDevice->CreateFaceView(pCube, i, mip) ... DestroyTexture(pSrf)
//
// The face ORDER carries over unchanged: D3D9 numbers +X, -X, +Y, -Y, +Z, -Z
// from zero and so does Vulkan's array-layer order for a cube. What does not
// carry over is the lifetime: GetCubeMapSurface handed back a reference on an
// existing object, where CreateFaceView makes a NEW VkImageView that shares
// the parent's image and has to be destroyed. SAFE_RELEASE therefore becomes
// DestroyTexture, which is a real destruction rather than a decrement -- and
// it is correct here precisely because nothing else holds this view.
//
// D3DFMT_X8R8G8B8 becomes VK_FORMAT_B8G8R8A8_UNORM. There is no X8 form in
// Vulkan: "the alpha byte is present but ignored" was a D3D9 distinction the
// runtime made and Vulkan does not, so the same 32 bits are declared with
// their alpha meaningful and the shaders simply do not read it.
//
// StretchRect(src, NULL, dst, NULL, D3DTEXF_POINT) becomes
// BlitTexture(dst, NULL, src, NULL, false). NOTE THE ARGUMENT ORDER: it names
// the DESTINATION first, matching memcpy; every converted StretchRect swaps
// its first two pairs. D3DTEXF_POINT is the false in the last argument.
//
// ONE CORRECTION, TWICE. The Windows error paths inside the two face loops --
// `if (!pBlur->Execute(true)) { LogErr(...); return false; }` -- leave pSrf
// held. On Windows that leaks one COM reference on a surface whose texture is
// still alive, which the next Release eventually reclaims. Here pSrf is a
// VkImageView this function CREATED and nothing else holds, so the same code
// leaks the view outright, every frame the blur fails. The destroy is added on
// those two paths; nothing else about them changes.
// ===========================================================================================
bool Scene::RenderBlurredMap(VulkanDevice *pDev, VulkanTexture *pSrc)
{
	bool bQuality = true;
	// bQuality is set and never read, here and in the Windows original. It is
	// kept rather than deleted because deleting it would be a change to the
	// author's code rather than a conversion, and silenced because GCC warns
	// where MSVC does not.
	(void)bQuality;

	if (!pSrc) return false;

	if (!pBlur) {
		pBlur = new ImageProcessing(pDev, "Modules/VulkanClient/EnvMapBlur.glsl", "PSBlur");
	}

	if (!pBlur->IsOK()) {
		LogErr("pBlur is not OK");
		return false;
	}

	if (!pEnvDS) {
		LogErr("EnvDepthStencil doesn't exists");
		return false;
	}

	VulkanImageDesc desc = pEnvDS->Desc();
	DWORD width = min((UINT)512, desc.Width);

	const VkImageUsageFlags cubeUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
									  | VK_IMAGE_USAGE_SAMPLED_BIT
									  | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
									  | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

	if (!pBlrTemp[0]) {
		for (int i = 0; i < 5; i++) {
			pBlrTemp[i] = pDev->CreateTextureCube(width >> i, 1, VK_FORMAT_B8G8R8A8_UNORM, cubeUsage);
			if (!pBlrTemp[i]) return false;
		}
	}


	FVECTOR3 dir, up, cp;
	VulkanTexture *pSrf = NULL;
	VulkanTexture *pTmp = NULL;

	// Create clurred mip sub-levels
	//
	for (DWORD i = 0; i < 6; i++) {
		pSrf = pDevice->CreateFaceView(pSrc, i, 0);
		pTmp = pDevice->CreateFaceView(pBlrTemp[0], i, 0);
		pDevice->BlitTexture(pTmp, NULL, pSrf, NULL, false);
		if (pSrf) { pDevice->DestroyTexture(pSrf); pSrf = NULL; }
		if (pTmp) { pDevice->DestroyTexture(pTmp); pTmp = NULL; }
	}


	// Create clurred mip sub-levels
	//
	for (int mip = 1; mip < 5; mip++) {

		pBlur->SetFloat("fD", (4.0f / float(256 >> (mip - 1))));
		pBlur->SetBool("bDir", false);
		pBlur->SetTextureNative("tCube", pBlrTemp[mip-1], IPF_LINEAR);

		for (DWORD i = 0; i < 6; i++) {

			EnvMapDirection(i, &dir, &up);
			// D3DXVec3Cross followed by D3DXVec3Normalize. VectorHelpers.h has
			// both as expressions, so the two out-parameter calls become one
			// assignment each.
			cp = cross(up, dir);
			cp = unit(cp);

			pSrf = pDevice->CreateFaceView(pSrc, i, mip);

			pBlur->SetOutputNative(0, pSrf);
			pBlur->SetFloat("vDir", &dir, sizeof(FVECTOR3));
			pBlur->SetFloat("vUp", &up, sizeof(FVECTOR3));
			pBlur->SetFloat("vCp", &cp, sizeof(FVECTOR3));

			if (!pBlur->Execute(true)) {
				LogErr("pBlur Execute Failed");
				if (pSrf) pDevice->DestroyTexture(pSrf);
				return false;
			}

			pTmp = pDevice->CreateFaceView(pBlrTemp[mip-1], i, 0);
			pDevice->BlitTexture(pTmp, NULL, pSrf, NULL, false);
			if (pSrf) { pDevice->DestroyTexture(pSrf); pSrf = NULL; }
			if (pTmp) { pDevice->DestroyTexture(pTmp); pTmp = NULL; }
		}

		pBlur->SetBool("bDir", true);

		for (DWORD i = 0; i < 6; i++) {

			EnvMapDirection(i, &dir, &up);
			cp = cross(up, dir);
			cp = unit(cp);

			pSrf = pDevice->CreateFaceView(pSrc, i, mip);

			pBlur->SetOutputNative(0, pSrf);
			pBlur->SetFloat("vDir", &dir, sizeof(FVECTOR3));
			pBlur->SetFloat("vUp", &up, sizeof(FVECTOR3));
			pBlur->SetFloat("vCp", &cp, sizeof(FVECTOR3));

			if (!pBlur->Execute(true)) {
				LogErr("pBlur Execute Failed");
				if (pSrf) pDevice->DestroyTexture(pSrf);
				return false;
			}

			pTmp = pDevice->CreateFaceView(pBlrTemp[mip], i, 0);
			pDevice->BlitTexture(pTmp, NULL, pSrf, NULL, false);
			if (pSrf) { pDevice->DestroyTexture(pSrf); pSrf = NULL; }
			if (pTmp) { pDevice->DestroyTexture(pTmp); pTmp = NULL; }
		}
	}

	return true;
}



// ===========================================================================================
// Same two cube-map idioms as RenderBlurredMap above, plus three more:
//
//   D3DXCreateTexture(pDev, w, h, mips, D3DUSAGE_RENDERTARGET, fmt,
//                     D3DPOOL_DEFAULT, &pTex)
// becomes pDev->CreateTexture(w, h, mips, fmt, usage) -- the same collapse of
// pool and usage as the cube form.
//
//   D3DFMT_A16B16G16R16F becomes VK_FORMAT_R16G16B16A16_SFLOAT. THE CHANNEL
//   ORDER IS REVERSED IN THE NAME AND IDENTICAL IN MEMORY: D3D9 named a
//   format from the most significant byte down, Vulkan names it from the
//   first byte up. A16B16G16R16F is R first in memory, which is what
//   R16G16B16A16_SFLOAT says.
//
//   pOut->GetSurfaceLevel(0, &pOuts) becomes pOuts = pOut. A VulkanTexture
//   already IS the thing a render target attachment is built from, and mip 0
//   is what CreateMipView(pOut, 0) would hand back -- a second object
//   describing the same image. See VulkanSurface.h on why GetSurface() and
//   GetTexture() are the same image here.
//
// D3DXVECTOR2 becomes FVECTOR2, and ptr(D3DXVECTOR2(...)) loses its ptr() for
// the same reason it did in RenderShadowMap: a named local instead of the
// address of a temporary.
// ===========================================================================================
bool Scene::IntegrateIrradiance(vVessel *vV, VulkanTexture *pSrc, VulkanTexture *pOut)
{
	if (!pSrc) return false;

	if (!pIrradiance) {
		pIrradiance = new ImageProcessing(pDevice, "Modules/VulkanClient/IrradianceInteg.glsl", "PSPreInteg");
		pIrradiance->CompileShader("PSInteg");
		pIrradiance->CompileShader("PSPostBlur");
	}

	if (!pIrradiance->IsOK()) {
		LogErr("pIrradiance is not OK");
		return false;
	}

	if (!pIrradDS) {
		LogErr("pIrradDS doesn't exists");
		return false;
	}

	VulkanTexture *pOuts = pOut;

	VulkanImageDesc desc = pIrradDS->Desc();
	VulkanImageDesc desc_out = pOuts->Desc();
	
	const VkImageUsageFlags rtUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
									| VK_IMAGE_USAGE_SAMPLED_BIT
									| VK_IMAGE_USAGE_TRANSFER_SRC_BIT
									| VK_IMAGE_USAGE_TRANSFER_DST_BIT;

	if (!pIrradTemp) {
		pIrradTemp = pDevice->CreateTextureCube(16, 1, VK_FORMAT_R16G16B16A16_SFLOAT, rtUsage);
		if (!pIrradTemp) {
			LogErr("Failed to create irradiance temp");
			return false;
		}
		pIrradTemp2 = pDevice->CreateTexture(128, 128, 1, VK_FORMAT_R16G16B16A16_SFLOAT, rtUsage);
		if (!pIrradTemp2) {
			LogErr("Failed to create irradiance temp");
			return false;
		}
		pIrradTemp3 = pDevice->CreateTexture(desc_out.Width, desc_out.Height, 1, VK_FORMAT_R16G16B16A16_SFLOAT, rtUsage);
		if (!pIrradTemp3) {
			LogErr("Failed to create irradiance temp");
			return false;
		}
	}

	FVECTOR3 nr, up, cp;
	VulkanTexture *pSrf = NULL;
	VulkanTexture *pTgt = NULL;
	// pTmp2 and pTmp3 were GetSurfaceLevel(0) of pIrradTemp2 and pIrradTemp3.
	// Mip 0 of an image IS the image here, so the two extra objects and the
	// two releases at the end of the function go away with them.
	VulkanTexture *pTmp2 = pIrradTemp2;
	VulkanTexture *pTmp3 = pIrradTemp3;


	// ---------------------------------------------------------------------
	// Pre-Integrate Irradiance Cube
	//
	pIrradiance->Activate("PSPreInteg");
	FVECTOR2 fD(1.0f / float(desc.Width), 1.0f / float(desc.Height));
	pIrradiance->SetFloat("fD", &fD, sizeof(FVECTOR2));

	for (DWORD i = 0; i < 6; i++)
	{
		pSrf = pDevice->CreateFaceView(pSrc, i, 0);
		pTgt = pDevice->CreateFaceView(pIrradTemp, i, 0);
		
		pDevice->BlitTexture(pTmp2, NULL, pSrf, NULL, false);

		pIrradiance->SetOutputNative(0, pTgt);
		pIrradiance->SetTextureNative("tSrc", pIrradTemp2, IPF_POINT | IPF_CLAMP);

		if (!pIrradiance->Execute(true)) {
			LogErr("pIrradiance Execute Failed");
			// Same correction as RenderBlurredMap's: the Windows path leaves
			// both faces held, which here leaks two VkImageViews outright.
			if (pTgt) pDevice->DestroyTexture(pTgt);
			if (pSrf) pDevice->DestroyTexture(pSrf);
			return false;
		}

		if (pTgt) { pDevice->DestroyTexture(pTgt); pTgt = NULL; }
		if (pSrf) { pDevice->DestroyTexture(pSrf); pSrf = NULL; }
	}

	


	// ---------------------------------------------------------------------
	// Main Integration
	//
	GetLVLH(vV, &up, &nr, &cp);

	float Glow = float(Config->PlanetGlow);

	pIrradiance->Activate("PSInteg");
	pIrradiance->SetOutputNative(0, pTmp3);
	pIrradiance->SetTextureNative("tCube", pIrradTemp, IPF_LINEAR);
	pIrradiance->SetFloat("Kernel", IKernel, sizeof(IKernel));
	pIrradiance->SetFloat("vNr", &nr, sizeof(FVECTOR3));
	pIrradiance->SetFloat("vUp", &up, sizeof(FVECTOR3));
	pIrradiance->SetFloat("vCp", &cp, sizeof(FVECTOR3));
	pIrradiance->SetFloat("fIntensity", &Glow, sizeof(float));
	pIrradiance->SetBool("bUp", false);
	pIrradiance->SetTemplate(0.5f, 1.0f, 0.0f, 0.0f);

	if (!pIrradiance->Execute(true)) {
		LogErr("pIrradiance Execute Failed");
		return false;
	}

	pIrradiance->SetBool("bUp", true);
	pIrradiance->SetTemplate(0.5f, 1.0f, 0.5f, 0.0f);

	if (!pIrradiance->Execute(true)) {
		LogErr("pIrradiance Execute Failed");
		return false;
	}

	// ---------------------------------------------------------------------
	// Post Blur
	//
	pIrradiance->Activate("PSPostBlur");
	FVECTOR2 fDo(1.0f / float(desc_out.Width), 1.0f / float(desc_out.Height));
	pIrradiance->SetFloat("fD", &fDo, sizeof(FVECTOR2));
	pIrradiance->SetOutputNative(0, pOuts);
	pIrradiance->SetTextureNative("tSrc", pIrradTemp3, IPF_POINT | IPF_WRAP);

	if (!pIrradiance->Execute(true)) {
		LogErr("pIrradiance Execute Failed");
		return false;
	}

	// The three SAFE_RELEASE calls that stood here released pTmp2, pTmp3 and
	// pOuts -- three GetSurfaceLevel(0) references. None of the three is a
	// separate object any more, so there is nothing to release: pTmp2 is
	// pIrradTemp2, pTmp3 is pIrradTemp3 and pOuts is the caller's pOut.
	// Destroying any of them here would destroy the image itself.

	return true;
}



// ===========================================================================================
//
void Scene::ClearOmitFlags()
{
	VOBJREC *pv = NULL;
	for (pv=vobjFirst; pv; pv=pv->next) pv->vobj->bOmit = false;
}


// ===========================================================================================
// gc->GetBackBuffer() returns a VulkanTexture directly -- it is the ATTACHMENT
// PROXY the client keeps for the core's colour attachment, which is why its
// GetDesc() becomes Desc() and there is no surface object in between. See
// VulkanDevice::CreateAttachmentProxy.
//
// AND THAT PROXY IS WHY THIS FUNCTION CANNOT DO WHAT IT DID. The proxy holds
// no VkImage: the swapchain images belong to UIHost.cpp and the client is
// never handed one. StretchRect into the back buffer therefore has no
// counterpart -- there is nothing to blit into. The Windows function drew the
// six cube faces straight onto the frame buffer as a debugging aid, outside
// any render pass, which is a thing D3D9 allowed and Vulkan does not: the
// swapchain image is inside the core's render pass for the whole frame, and
// vkCmdBlitImage is illegal there.
//
// The body is kept, computing the same six destination rectangles from the
// back buffer's own extent, so that the geometry is not lost; the blit that
// each iteration ended with is refused by name rather than silently dropped.
// A working version of this belongs in the Sketchpad, which can draw into the
// open pass -- see VulkanPad2.cpp's CopyRect.
//
// The uninitialised-x/y warning the Windows switch produces (case 6+ leaves
// both indeterminate) is answered by initialising them, which is a correction
// GCC insists on and MSVC did not.
// ===========================================================================================
void Scene::VisualizeCubeMap(VulkanTexture *pCube, int mip)
{
	if (!pCube) return;

	VulkanTexture *pSrf = NULL;
	VulkanTexture *pBack = gc->GetBackBuffer();

	if (!pBack) return;

	VulkanImageDesc bdesc = pBack->Desc();

	DWORD x = 0, y = 0, h = bdesc.Height / 3;

	for (DWORD i=0;i<6;i++) {

		pSrf = pDevice->CreateFaceView(pCube, i, mip);

		switch (i) {
			case 0:	x = 2*h; y=h; break;
			case 1:	x = 0;   y=h; break;
			case 2:	x = 1*h; y=0; break;
			case 3:	x = 1*h; y=2*h; break;
			case 4:	x = 1*h; y=h; break;
			case 5:	x = 3*h; y=h; break;
		}

		RECT dr;
		dr.left = x;
		dr.top = y;
		dr.bottom = y+h;
		dr.right = x+h;

		// StretchRect(pSrf, NULL, pBack, &dr, D3DTEXF_POINT) HAS NO
		// COUNTERPART. pBack is an attachment proxy with no VkImage; see the
		// note above.
		(void)dr;

		if (pSrf) { pDevice->DestroyTexture(pSrf); pSrf = NULL; }
	}
}



// ===========================================================================================
// SetRenderState(D3DRS_STENCILENABLE, FALSE) HAS NO COUNTERPART. The stencil
// test is pipeline state in Vulkan, chosen when the VkPipeline is built, so
// there is no device flag to put back after RenderGroundShadow -- the shadow
// pipelines enable it and every other pipeline in the client leaves it off.
// This is the same finding as Scene::RecallDefaultState's; see the note there.
// ===========================================================================================
void Scene::RenderVesselShadows (OBJHANDLE hPlanet, float depth) const
{
	// If this planet is not a proxy body skip the rest
	if (hPlanet != oapiCameraProxyGbody()) return;

	// render vessel shadows
	VOBJREC *pv;
	for (pv = vobjFirst; pv; pv = pv->next) {
		if (!pv->vobj->IsActive()) continue;
		if (oapiGetObjectType(pv->vobj->Object()) == OBJTP_VESSEL)
			((vVessel*)(pv->vobj))->RenderGroundShadow(pDevice, hPlanet, depth);
	}

	// reset device parameters -- see the note above; nothing to reset.

	// render particle shadows
	VulkanTexture *tex = 0;
	for (DWORD j=0;j<nstream;j++) pstream[j]->RenderGroundShadow(pDevice, tex);
}


// ===========================================================================================
// ptr(D3DXVECTOR4(...)) becomes a named FVECTOR4, for the same reason it did
// in RenderShadowMap; LPD3DXMATRIX(pWorld) becomes pWorld, because
// VulkanMesh::RenderSimplified already takes an FMATRIX4 and the cast existed
// only to reinterpret one as the other.
//
// HR() is dropped from the four FX calls. VulkanEffectFile's setters return
// bool rather than HRESULT -- there is no HRESULT here -- and they log their
// own failures, which is what HR() did.
// ===========================================================================================
void Scene::RenderMesh(DEVMESHHANDLE hMesh, const oapi::FMATRIX4 *pWorld)
{
	VulkanMesh *pMesh = (VulkanMesh *)hMesh;

	const Scene::SHADOWMAPPARAM *shd = GetSMapData();

	float s = float(shd->size);
	float sr = 2.0f * shd->rad / s;

	VulkanEffect::FX->SetMatrix(VulkanEffect::eLVP, &shd->mViewProj);

	if (shd->pShadowMap) {
		VulkanEffect::FX->SetTexture(VulkanEffect::eShadowMap, shd->pShadowMap);
		FVECTOR4 vSHD(sr, 1.0f / s, float(oapiRand()), 1.0f / shd->depth);
		VulkanEffect::FX->SetVector(VulkanEffect::eSHD, &vSHD);
		VulkanEffect::FX->SetBool(VulkanEffect::eShadowToggle, true);
	}
	else {
		VulkanEffect::FX->SetBool(VulkanEffect::eShadowToggle, false);
	}

	pMesh->SetSunLight(&sunLight);
	pMesh->RenderSimplified(pWorld);
}



// ===========================================================================================
// D3DXVec3Transform(&homog, &pos, pVP) becomes homog = mul(FVECTOR4(pos, 1),
// *pVP). The two are the same arithmetic: D3DXVec3Transform extends the
// vector to (x,y,z,1), multiplies by the matrix as a ROW vector and keeps all
// four components -- which is exactly oapi::mul in DrawAPI.h, and the reason
// it is mul rather than TransformCoord is that TransformCoord divides by w
// and throws it away, where both functions below need w themselves.
// ===========================================================================================
bool Scene::WorldToScreenSpace(const VECTOR3 &wpos, oapi::IVECTOR2 *pt, FMATRIX4 *pVP, float clip)
{
	FVECTOR4 homog;
	FVECTOR3 pos(float(wpos.x), float(wpos.y), float(wpos.z));

	if (pVP) homog = mul(FVECTOR4(pos.x, pos.y, pos.z, 1.0f), *pVP);
	else homog = mul(FVECTOR4(pos.x, pos.y, pos.z, 1.0f), *GetProjectionViewMatrix());

	if (homog.w < 0.0f) return false;

	homog.x /= homog.w;
	homog.y /= homog.w;

	bool bClip = false;
	if (homog.x < -clip || homog.x > clip || homog.y < -clip || homog.y > clip) bClip = true;

	if (std::hypot(homog.x, homog.y) < 1e-6) {
		pt->x = viewW / 2;
		pt->y = viewH / 2;
	}
	else {
		pt->x = (long)((float(viewW) * 0.5f * (1.0f + homog.x)) + 0.5f);
		pt->y = (long)((float(viewH) * 0.5f * (1.0f - homog.y)) + 0.5f);
	}

	return !bClip;
}


// ===========================================================================================
//
bool Scene::WorldToScreenSpace2(const VECTOR3& wpos, oapi::FVECTOR2* pt, FMATRIX4* pVP, float clip)
{
	FVECTOR4 homog;
	FVECTOR3 pos(float(wpos.x), float(wpos.y), float(wpos.z));

	if (pVP) homog = mul(FVECTOR4(pos.x, pos.y, pos.z, 1.0f), *pVP);
	else homog = mul(FVECTOR4(pos.x, pos.y, pos.z, 1.0f), *GetProjectionViewMatrix());

	homog.x /= homog.w;
	homog.y /= homog.w;

	bool bClip = false;
	if (homog.w < 0.0f) bClip = true;
	if (homog.x < -clip || homog.x > clip || homog.y < -clip || homog.y > clip) bClip = true;

	if (std::hypot(homog.x, homog.y) < 1e-6) {
		pt->x = viewW / 2;
		pt->y = viewH / 2;
	}
	else {
		pt->x = (float(viewW) * 0.5f * (1.0f + homog.x)) + 0.5f;
		pt->y = (float(viewH) * 0.5f * (1.0f - homog.y)) + 0.5f;
	}

	return !bClip;
}

// ===========================================================================================
//
void Scene::RenderObjectMarker(oapi::Sketchpad *pSkp, const VECTOR3 &gpos, const std::string& label1, const std::string& label2, int mode, int scale)
{
	VECTOR3 dp (gpos - GetCameraGPos());
	normalise (dp);
	m_celSphere->RenderMarker(pSkp, dp, label1, label2, mode, scale);
}

// ===========================================================================================
//
void Scene::NewVessel(OBJHANDLE hVessel)
{
	CheckVisual(hVessel);
}

// ===========================================================================================
//
void Scene::DeleteVessel(OBJHANDLE hVessel)
{
	VOBJREC *pv = FindVisual(hVessel);
	if (pv) DelVisualRec(pv);
}


// ===========================================================================================
// D3D9ParticleStream became VulkanParticleStream. Nothing else here touches
// the API: this is array bookkeeping.
// ===========================================================================================
void Scene::AddParticleStream (class VulkanParticleStream *_pstream)
{

	VulkanParticleStream **tmp = new VulkanParticleStream*[nstream+1];
	if (nstream) {
		memcpy (tmp, pstream, nstream*sizeof(VulkanParticleStream*));
		delete []pstream;
	}
	pstream = tmp;
	pstream[nstream++] = _pstream;

}

// ===========================================================================================
//
void Scene::DelParticleStream (DWORD idx)
{

	VulkanParticleStream **tmp;
	if (nstream > 1) {
		DWORD i, j;
		tmp = new VulkanParticleStream*[nstream-1];
		for (i = j = 0; i < nstream; i++)
			if (i != idx) tmp[j++] = pstream[i];
	} else tmp = 0;
	delete pstream[idx];
	delete []pstream;
	pstream = tmp;
	nstream--;

}

// ===========================================================================================
// oapiCreateFont/oapiReleaseFont are core calls, not Direct3D ones, so this
// converts unchanged. "Arial" stays: the Linux font lookup in
// VulkanTextMgr.cpp resolves family names through fontconfig, which answers
// "Arial" with the installed metric-compatible substitute exactly as it does
// for every other Windows family name the client asks for.
// ===========================================================================================
void Scene::InitGDIResources ()
{
	char dbgfnt[64]; sprintf_s(dbgfnt,64,"*%s",Config->DebugFont);
	pAxisFont  = oapiCreateFont(24, false, "Arial", FONT_NORMAL, 0);
	pLabelFont = oapiCreateFont(15, false, "Arial", FONT_NORMAL, 0);
	pDebugFont = oapiCreateFont(Config->DebugFontSize, true, dbgfnt, FONT_NORMAL, 0);

	for (int i = 0; i < 4; ++i) {
		label_font[i] = GetOrCreateLabelFont(FONT_SIZES[i]);
	}
	//@todo: different pens for different fonts?
}

// ===========================================================================================
//
void Scene::ExitGDIResources ()
{
	oapiReleaseFont(pAxisFont);
	oapiReleaseFont(pLabelFont);
	oapiReleaseFont(pDebugFont);
	
	// Only delete the cache. The label_font are just weak refs!
	for(auto &[size, font] : labelFontCache) {
		gc->clbkReleaseFont(font);
	}
}


// ===========================================================================================
//
float Scene::GetDepthResolution(float dist) const
{
	return fabs( (Camera.nearplane-Camera.farplane)*(dist*dist) / (Camera.farplane * Camera.nearplane * 16777215.0f) );
}

// ===========================================================================================
//
float Scene::CameraInSpace() const
{
	if (Camera.vProxy) {
		if (Camera.vProxy->HasAtmosphere()) {
			ConstParams* cp = Camera.vProxy->GetScatterConst();
			if (cp)	return 1.0f - exp(-cp->CamAlt * cp->iH.x);
		}
	}
	return 1.0f;
}

// ===========================================================================================
//
void Scene::GetCameraLngLat(double *lng, double *lat) const
{
	if (lng) *lng = Camera.lng;
	if (lat) *lat = Camera.lat;
}

// ===========================================================================================
//
void Scene::PushCamera()
{
	CameraStack.push(Camera);
}

// ===========================================================================================
//
void Scene::PopCamera()
{
	Camera = CameraStack.top();
	CameraStack.pop();
}

// ===========================================================================================
// FMATRIX4(GetProjectionViewMatrix()) becomes *GetProjectionViewMatrix(). The
// Windows expression relied on FMATRIX4's constructor from a D3DXMATRIX*,
// which exists only under _WIN32 in DrawAPI.h; the matrix is already an
// FMATRIX4 here, so the conversion is a dereference.
// ===========================================================================================
FMATRIX4 Scene::PushCameraFrustumLimits(float nearlimit, float farlimit)
{
	FRUSTUM fr = { Camera.nearplane, Camera.farplane };
	FrustumStack.push(fr);
	SetCameraFrustumLimits(nearlimit, farlimit);
	return *GetProjectionViewMatrix();
}

// ===========================================================================================
//
FMATRIX4 Scene::PopCameraFrustumLimits()
{
	SetCameraFrustumLimits(FrustumStack.top().znear, FrustumStack.top().zfar);
	FrustumStack.pop();
	return *GetProjectionViewMatrix();
}

// ===========================================================================================
//
void Scene::BeginPass(DWORD dwPass)
{
	PassStack.push(dwPass);
}

// ===========================================================================================
//
void Scene::PopPass()
{
	PassStack.pop();
}

// ===========================================================================================
//
DWORD Scene::GetRenderPass() const
{
	if (PassStack.empty()) return RENDERPASS_MAINSCENE;
	return PassStack.top();
}


// ===========================================================================================
// Camera.mProj._11 becomes Camera.mProj.m11 throughout the rest of this file.
// The two names are the SAME FIELD -- FMATRIX4 is a union whose _11 view
// exists only under _WIN32 (DrawAPI.h), so the m-form is the one that
// compiles everywhere. Same storage, same value, no reordering.
//
// D3DXVec3Normalize(&v, &v) becomes v = unit(v), which VectorHelpers.h has as
// an expression.
// ===========================================================================================
FVECTOR3 Scene::GetPickingRay(short xpos, short ypos)
{
	float x = 2.0f*float(xpos) / float(ViewW()) - 1.0f;
	float y = 2.0f*float(ypos) / float(ViewH()) - 1.0f;
	FVECTOR3 vPick = Camera.x * (x / Camera.mProj.m11) + Camera.y * (-y / Camera.mProj.m22) + Camera.z;
	vPick = unit(vPick);
	return vPick;
}

// ===========================================================================================
//
TILEPICK Scene::PickSurface(short xpos, short ypos)
{
	// (void*): TILEPICK holds VECTOR3s, so GCC will not memset it without the
	// cast (-Wclass-memaccess) where MSVC does. The zero is what is wanted.
	TILEPICK tp; memset((void*)&tp, 0, sizeof(TILEPICK));
	vPlanet *vp = GetCameraProxyVisual();
	if (!vp) return tp;
	FVECTOR3 vRay = GetPickingRay(xpos, ypos);
	vp->PickSurface(vRay, &tp);
	return tp;
}

// ===========================================================================================
//
VulkanPick Scene::PickScene(short xpos, short ypos)
{
	FVECTOR3 vPick = GetPickingRay(xpos, ypos);

	VulkanPick result;
	result.dist  = 1e30f;
	result.pMesh = NULL;
	result.vObj  = NULL;
	result.group = -1;
	result.idx = -1;

	for (VOBJREC *pv=vobjFirst; pv; pv=pv->next) {

		if (pv->type!=OBJTP_VESSEL) continue;
		if (!pv->vobj->IsActive()) continue;
		if (!pv->vobj->IsVisible()) continue;

		vVessel *vVes = (vVessel *)pv->vobj;
		double cd = vVes->CamDist();

		if (cd<5e3 && cd>1e-3) {
			VulkanPick pick = vVes->Pick(&vPick);
			if (pick.pMesh) if (pick.dist<result.dist) result = pick;
		}
	}
	return result;
}

// ===========================================================================================
// ptr(GetPickingRay(...)) becomes a named local: ptr() existed to take the
// address of a temporary, which C++ forbids, and VulkanMesh::Pick takes a
// const FVECTOR3*.
// ===========================================================================================
VulkanPick Scene::PickMesh(DEVMESHHANDLE hMesh, const FMATRIX4 *pW, short xpos, short ypos)
{
	VulkanMesh *pMesh = (VulkanMesh *)hMesh;
	FVECTOR3 vRay = GetPickingRay(xpos, ypos);
	return pMesh->Pick(pW, NULL, &vRay);
}

// ===========================================================================================
// ZeroMemory becomes memset. The Linux windows.h shim defines ZeroMemory, so
// the name would compile, but the client spells the same thing memset
// everywhere else it is not carrying a Win32 idiom.
// ===========================================================================================
void Scene::GetAdjProjViewMatrix(FMATRIX4 *pMP, float znear, float zfar)
{
	float tanap = tan(Camera.aperture);
	memset((void*)pMP, 0, sizeof(FMATRIX4));
	pMP->m11 = (Camera.aspect / tanap);
	pMP->m22 = (1.0f / tanap);
	pMP->m43 = (pMP->m33 = zfar / (zfar - znear)) * (-znear);
	pMP->m34 = 1.0f;
}


// ===========================================================================================
// D3DXMatrixMultiply becomes VMAT_MatrixMultiply and D3D9Effect becomes
// VulkanEffect; the projection matrix itself is unchanged, including its
// left-handed 0..1 depth range, because Vulkan clip space uses the same range.
// ===========================================================================================
void Scene::SetCameraAperture(float ap, float as)
{
	Camera.aperture = ap;
	Camera.aspect = as;

	float tanap = tan(ap);

	memset((void*)&Camera.mProj, 0, sizeof(FMATRIX4));

	Camera.mProj.m11 = (as / tanap);
	Camera.mProj.m22 = (1.0f / tanap);
	Camera.mProj.m43 = (Camera.mProj.m33 = Camera.farplane / (Camera.farplane-Camera.nearplane)) * (-Camera.nearplane);
	Camera.mProj.m34 = 1.0f;

	float x = tanap / as;
	float y = tanap;
	float z = as / tanap;

	Camera.apsq = sqrt(x*x*x*z + y*y);

	Camera.vh   = tan(ap);
	Camera.vw   = Camera.vh/as;
	Camera.vhf  = 1.0f / cos(ap);
	Camera.vwf  = Camera.vhf/as;

	VMAT_MatrixMultiply(&Camera.mProjView, &Camera.mView, &Camera.mProj);
	VulkanEffect::SetViewProjMatrix(&Camera.mProjView);
}

// ===========================================================================================
//
void Scene::SetCameraFrustumLimits (double nearlimit, double farlimit)
{
	Camera.nearplane = (float)nearlimit;
	Camera.farplane  = (float)farlimit;
	SetCameraAperture(Camera.aperture, Camera.aspect);
}

// ===========================================================================================
//
bool Scene::IsProxyMesh()
{
	return Camera.vProxy ? Camera.vProxy->IsMesh() : false;
}



// ===========================================================================================
// _VD3DX(v) becomes _V(v). Both spell "this three-float vector as a VECTOR3";
// the Windows name said which library the source type came from, and it does
// not come from there any more. See the conversion helpers in VulkanUtil.h,
// which keep the same shape for the same reason.
// ===========================================================================================
bool Scene::CameraPan(VECTOR3 pan, double speed)
{
	DWORD camMode = *(DWORD *)gc->GetConfigParam(CFGPRM_GETCAMERAMODE);
	OBJHANDLE hTgt = oapiCameraTarget();

	if (DebugControls::IsActive()==true && hTgt) {
		if (camMode==1) {
			VECTOR3 pos;
			oapiGetGlobalPos(hTgt, &pos);
			Camera.pos = pos + Camera.relpos;
			Camera.pos += Camera.dir * (pan.z*speed) + _V(Camera.x) * (pan.x*speed) + _V(Camera.y) * (pan.y*speed);
			Camera.relpos = Camera.pos - pos;
			return true;
		}
	}
	return false;
}


// ===========================================================================================
// D3DXMatrixIdentity becomes VMAT_Identity and D3DMAT_SetRotation becomes
// VMAT_SetRotation -- the same two functions under the client's own prefix,
// which is what every D3DMAT_ helper in D3D9Util.cpp became.
//
// The comment about "D3D single-precision format" is left as the author wrote
// it. It describes why render space is camera-centred, which is a property of
// 32-bit floats rather than of Direct3D, and is just as true here.
// ===========================================================================================
bool Scene::UpdateCameraFromOrbiter(DWORD dwPass)
{
	MATRIX3 grot;
	VECTOR3 pos;

	DWORD camMode = *(DWORD *)gc->GetConfigParam(CFGPRM_GETCAMERAMODE);

	OBJHANDLE hTgt = oapiCameraTarget();

	if (hTgt) {
		if (DebugControls::IsActive()==false || camMode==0) {
			// Acquire camera information from Orbiter
			oapiGetGlobalPos(hTgt, &pos);
			oapiCameraGlobalPos(&Camera.pos);
			Camera.relpos = Camera.pos - pos;	// camera_relpos is a mesh debugger paramater
		}
		else {
			// Mesh debugger camera mode active
			oapiGetGlobalPos(hTgt, &pos);
			Camera.pos = pos + Camera.relpos; // Compute from target pos and offset
		}
	}
	else {
		// Camera target doesn't exist. (Should not happen)
		oapiCameraGlobalPos(&Camera.pos);
		Camera.relpos = _V(0,0,0);
	}

	oapiCameraGlobalDir(&Camera.dir);
	oapiCameraRotationMatrix(&grot);
	VMAT_Identity(&Camera.mView);
	VMAT_SetRotation(&Camera.mView, &grot);

	// note: in render space, the camera is always placed at the origin,
	// so that render coordinates are precise in the vicinity of the
	// observer (before they are translated into D3D single-precision
	// format). However, the orientation of the render space is the same
	// as orbiter's global coordinate system. Therefore there is a
	// translational transformation between orbiter global coordinates
	// and render coordinates.

	for (VOBJREC *pv = vobjFirst; pv; pv = pv->next) pv->vobj->Update(true);

	return SetupInternalCamera(&Camera.mView, NULL, oapiCameraAperture(), double(viewH)/double(viewW));
}



// ===========================================================================================
// D3DXVEC(v) becomes FVEC(v) -- "this VECTOR3 as three floats" -- for the same
// reason _VD3DX became _V above.
// ===========================================================================================
bool Scene::SetupInternalCamera(FMATRIX4 *mNew, VECTOR3 *gpos, double apr, double asp)
{

	// Update camera orientation if a new matrix is provided
	if (mNew) {
		Camera.mView	  = *mNew;
		Camera.x   = FVECTOR3(Camera.mView.m11, Camera.mView.m21, Camera.mView.m31);
		Camera.y   = FVECTOR3(Camera.mView.m12, Camera.mView.m22, Camera.mView.m32);
		Camera.z   = FVECTOR3(Camera.mView.m13, Camera.mView.m23, Camera.mView.m33);
		Camera.dir = _V(Camera.z);
	}

	if (gpos) Camera.pos = *gpos;

	Camera.upos = FVEC(unit(Camera.pos));

	// find a logical reference body
	Camera.hObj_proxy = oapiCameraProxyGbody();
	Camera.hNear = NULL;
	Camera.hGravRef = NULL;
	Camera.vGravRef = NULL;

	// find the planet closest to the current camera position
	double closest = 0;
	int n = oapiGetGbodyCount();
	for (int i = 1; i < n; i++) {
		VECTOR3 gp; OBJHANDLE hB = oapiGetGbodyByIndex(i);
		oapiGetGlobalPos(hB, &gp);
		VECTOR3 dst = gp - Camera.pos;
		double l = pow(oapiGetMass(hB), 0.33) / dotp(dst, dst);
		if (l > closest) {
			closest = l;
			Camera.hNear = hB;
		}	
	}

	// find the planet closest to the current camera position
	closest = 0;
	for (int i = 0; i < n; i++) {
		VECTOR3 gp; OBJHANDLE hB = oapiGetGbodyByIndex(i);
		oapiGetGlobalPos(hB, &gp);
		VECTOR3 dst = gp - Camera.pos;
		double l = oapiGetMass(hB) / dotp(dst, dst);
		if (l > closest) {
			closest = l;
			Camera.hGravRef = hB;
		}
	}

	/*if (Camera.hNear) {
		// If the near body is not visible enough, switch to proxy.
		double apr = oapiGetSize(Camera.hNear) / closest;
		if (apr < 4e-3) Camera.hNear = Camera.hObj_proxy;
	}*/

	// find the visual
	if (oapiGetObjectType(Camera.hObj_proxy) == OBJTP_PLANET)
		Camera.vProxy = (vPlanet*)GetVisObject(Camera.hObj_proxy);
	else Camera.vProxy = nullptr;
	
	if (oapiGetObjectType(Camera.hNear) == OBJTP_PLANET)
		Camera.vNear = (vPlanet*)GetVisObject(Camera.hNear);
	else Camera.vNear = nullptr;

	Camera.vGravRef = GetVisObject(Camera.hGravRef);

	
	// Something is very wrong... abort...
	if (Camera.hGravRef == NULL || Camera.hObj_proxy == NULL || Camera.hNear == NULL) {
		assert(false); return false;
	}
	if (Camera.vGravRef == NULL || Camera.vProxy == NULL || Camera.vNear == NULL) {
		return false;
	}

	// Camera altitude over the proxy
	VECTOR3 pos; MATRIX3 grot; double rad;
	oapiGetGlobalPos(Camera.hObj_proxy, &pos);
	oapiGetRotationMatrix(Camera.hObj_proxy, &grot);

	oapiLocalToEqu(Camera.hObj_proxy, tmul(grot, Camera.pos - pos), &Camera.lng, &Camera.lat, &rad);

	Camera.alt_proxy = dist(Camera.pos, pos) - oapiGetSize(Camera.hObj_proxy);

	if (Camera.vProxy->Type() == OBJTP_PLANET) 
		rad = oapiSurfaceElevation(Camera.hObj_proxy, Camera.lng, Camera.lat);
	
	Camera.elev = Camera.alt_proxy - rad;

	// Camera altitude over the proxy
	oapiGetGlobalPos(Camera.hNear, &pos);
	Camera.alt_near = dist(Camera.pos, pos) - oapiGetSize(Camera.hNear);

	// Call SetCameraAparture to update ViewProj Matrix
	SetCameraAperture(float(apr), float(asp));

	// Finally update world matrices from all visuals
	//
	if (gpos) for (VOBJREC *pv = vobjFirst; pv; pv = pv->next) pv->vobj->ReOrigin(Camera.pos);

	return true;
}





// ===========================================================================================
// CUSTOM CAMERA INTERFACE
// ===========================================================================================

int Scene::DeleteCustomCamera(CAMERAHANDLE hCam)
{
	if (!hCam) return 0;
	int iError = CAMERA(hCam)->iError;
	CustomCams.erase(CAMERA(hCam));
	delete CAMERA(hCam);
	camCurrent = CustomCams.cbegin();
	return iError;
}

// ===========================================================================================
//
void Scene::DeleteAllCustomCameras()
{
	for (auto x : CustomCams) delete CAMERA(x);
	CustomCams.clear();
}

// ===========================================================================================
// memset(pv, 0, sizeof(CAMREC)) gets a (void*) cast: CAMREC holds a MATRIX3
// and a VECTOR3 and is therefore not trivially copyable to GCC's eye, which
// warns (-Wclass-memaccess) where MSVC does not. THE ZERO IS STILL WHAT IS
// WANTED and a constructor would not be a substitute -- the fields are
// assigned immediately below and the memset exists to clear the ones that are
// not. Same treatment, same reasoning, as the nine sites in Mesh.cpp.
// ===========================================================================================
CAMERAHANDLE Scene::SetupCustomCamera(CAMERAHANDLE hCamera, OBJHANDLE hVessel, MATRIX3 &mRot, VECTOR3 &pos, double fov, SURFHANDLE hSurf, DWORD flags)
{
	CAMREC *pv = NULL;

	if (!hSurf) return NULL;
	if (Config->CustomCamMode==0) return NULL;
	if (SURFACE(hSurf)->Is3DRenderTarget()==false) return NULL;

	if (hCamera==NULL) {
		pv = new CAMREC; memset((void*)pv, 0, sizeof(CAMREC));
		CustomCams.insert(pv);
		camCurrent = CustomCams.cbegin();
	}
	else {
		pv = (CAMREC *)hCamera;
	}

	if (!pv) return NULL;

	pv->bActive = true;
	pv->dAperture = fov;
	pv->dwFlags = flags;
	pv->hSurface = hSurf;
	pv->mRotation = mRot;
	pv->vPosition = pos;
	pv->hVessel = hVessel;
	pv->iError = 0;
	pv->fSurfLabelScale = 1.0f;

	return (CAMERAHANDLE)pv;
}

// ===========================================================================================
//
void Scene::CustomCameraOnOff(CAMERAHANDLE hCamera, bool bOn)
{
	if (!hCamera) return;
	CAMERA(hCamera)->bActive = bOn;
}

// ===========================================================================================
//
Font* Scene::GetOrCreateLabelFont(int size)
{
	auto it = labelFontCache.find(size);
	if(it == labelFontCache.end())
	{
		Font* newFont = gc->clbkCreateFont(size, true, "Arial", FONT_BOLD);
		it = labelFontCache.emplace(size, newFont).first;
	}

	return it->second;
}

// ===========================================================================================
//
void Scene::RenderLabelsForCustomCamera()
{
	if(!surfLabelsActive)
		return;
	
	vPlanet *planet = GetCameraProxyVisual();

	if(!planet)
		return;

	VulkanPad *skp = GetPooledSketchpad(SKETCHPAD_LABELS);

	if(!skp)
		return;

	const float labelScale = Camera.labelScale;

	// Set-up pen before calling Label rendering logic. Otherwise the marks were not visible
	skp->QuickPen(RGB(255, 255, 255), labelScale);

	Font* fonts[4];
	for (int i = 0; i < 4; i++) {
		// Retrieve cached fonts or create them new. This prevents recreating fonts when having different custom cameras at the cost of O(log n) access
		fonts[i] = GetOrCreateLabelFont((int)(FONT_SIZES[i] * labelScale));
	}

	int fontidx = -1;
	planet->RenderLabels(pDevice, skp, fonts, &fontidx);

	skp->EndDrawing();
}


// ===========================================================================================
// GetSurface() and GetDepthStencil() both hand back a VulkanTexture now; see
// VulkanSurface.h on why the two D3D9 surface kinds are one image here.
//
// The three D3DXMatrix calls become their VMAT_ counterparts, as everywhere
// else in this file.
// ===========================================================================================
void Scene::RenderCustomCameraView(CAMREC *cCur)
{
	VESSEL *pVes = oapiGetVesselInterface(cCur->hVessel);

	DWORD w = SURFACE(cCur->hSurface)->GetWidth();
	DWORD h = SURFACE(cCur->hSurface)->GetHeight();

	VulkanTexture *pSrf = SURFACE(cCur->hSurface)->GetSurface();
	VulkanTexture *pDSs = SURFACE(cCur->hSurface)->GetDepthStencil();

	if (!pSrf) cCur->iError = -1;
	if (!pDSs) cCur->iError = -2;

	if (cCur->iError!=0) return;

	MATRIX3 grot;
	VECTOR3 gpos;

	pVes->GetRotationMatrix(grot);
	pVes->Local2Global(cCur->vPosition, gpos);

	FMATRIX4 mEnv, mGlo;

	VMAT_Identity(&mGlo);
	VMAT_SetRotation(&mGlo, &grot);
	VMAT_Identity(&mEnv);
	VMAT_SetRotation(&mEnv, &cCur->mRotation);
	VMAT_MatrixMultiply(&mEnv, &mGlo, &mEnv);

	PushCamera();

	SetCameraFrustumLimits(0.1, 2e7);
	SetupInternalCamera(&mEnv, &gpos, cCur->dAperture, double(h)/double(w));

	// Copy target surface dimensions. This is needed so the surface label render path can correctly compute the projection
	Camera.viewportW = w;
	Camera.viewportH = h;
	Camera.labelScale = cCur->fSurfLabelScale;
	
	VOBJREC *pv = NULL;
	std::set<vVessel*> List;
	std::set<vVessel*> Lights;

	for (pv = vobjFirst; pv; pv = pv->next) if (pv->type == OBJTP_VESSEL) List.insert((vVessel *)pv->vobj);
	
	BeginPass(RENDERPASS_CUSTOMCAM);

	gc->PushRenderTarget(pSrf, pDSs, RENDERPASS_CUSTOMCAM);

	RenderSecondaryScene(List, Lights, 0xFF);

	// Render surface labels
	if(cCur->dwFlags & CUSTOMCAM_SURFACE_LABELS)
	{
		RenderLabelsForCustomCamera();
	}

	gc->PopRenderTargets();

	PopPass();
	PopCamera();
}

void Scene::SetCustomCameraSurfaceLabelScale(CAMERAHANDLE hCamera, float scale)
{
	if(!hCamera) {
		return;
	}

	if(scale <= 0.0f || !std::isfinite(scale)) {
		return;
	}

	CAMREC* camera = CAMERA(hCamera);

	camera->fSurfLabelScale = std::clamp(scale, 0.25f, 8.0f);

	return;
}



// ===========================================================================================
//
void Scene::RenderGlares()
{
	// -------------------------------------------------------------------------------------------------------
	// Render glares for the Sun and local lights
	// -------------------------------------------------------------------------------------------------------

	if (pRenderGlares && pLocalResultsSL)
	{
		static SMVERTEX Vertex[4] = { {-1, -1, 0, 0, 0}, {-1, 1, 0, 0, 1}, {1, 1, 0, 1, 1}, {1, -1, 0, 1, 0} };
		static WORD cIndex[6] = { 0, 2, 1, 0, 3, 2 };
		// D3DXMATRIX in the constant struct becomes FMATRIX4; the layout is
		// the same sixteen floats. Note that FMATRIX4 is alignas(16) where
		// D3DXMATRIX was not -- it is first in the struct, so nothing shifts,
		// but the struct's own alignment rises to 16. That matters only to
		// the GLSL block it is copied into, which declares the same fields in
		// the same order.
		VulkanImageDesc desc; FVECTOR2 pt;
		struct { FMATRIX4 mVP; float4 Pos, Color; float GPUId, Alpha, Blend; } Const;

		Const.Color = FVECTOR4(1, 1, 1, 1);
		VMAT_OrthoOffCenterLH(&Const.mVP, 0.0f, (float)viewW, (float)viewH, 0.0f, 0.0f, 1.0f);
		// pLocalResultsSL->GetDesc(&desc) becomes desc = ...->Desc(). And note
		// that pLocalResultsSL and pLocalResults are ONE IMAGE here, not a
		// surface and its texture -- see the note in the destructor.
		desc = pLocalResultsSL->Desc();

		pRenderGlares->ClearTextures();
		pRenderGlares->Setup(pPosTexDecl, false, 1);
		pRenderGlares->SetTextureVS("tVis", pLocalResults, IPF_CLAMP | IPF_POINT); // Set texture containing pre-cumputed visibility factors

		if (Config->bGlares && pSunGlare)
		{
			pRenderGlares->SetTexture("tTex0", pSunGlare, IPF_CLAMP | IPF_LINEAR);
			pRenderGlares->UpdateTextures();

			// Render Sun glare
			VECTOR3 gsun; oapiGetGlobalPos(oapiGetObjectByIndex(0), &gsun);
			double sdst = length(gsun - Camera.pos);
			VECTOR3 usun = (gsun - Camera.pos) / sdst;
			VECTOR3 pos = usun * 10e4;

			if (WorldToScreenSpace2(pos, &pt))
			{
				float cis = 1.0f, glare = float(Config->GFXGlare) * saturate(8.0 * AU / sdst);
				FVECTOR4 clr = FVECTOR4(1, 1, 1, 1);

				vPlanet* vp = GetCameraNearVisual();
			
				if (vp && vp->IsActive())
				{
					VECTOR3 crp = vp->CameraPos();
					clr = vp->SunLightColor(crp, 2.0);
					cis = CameraInSpace();
					glare *= pow(clr.MaxRGB(), 0.33f) * cis;				
				}
							
				float cd = length(pt - FVECTOR2(viewW, viewH) * 0.5f) / float(viewW); // Glare distance from a screen center
				float alpha = 2.0f * glare * max(0.5f, 1.0f - cd);
				float size = 300.0f * GetDisplayScale() * pow(alpha, 0.25f);

				Const.GPUId = 0.5f / float(desc.Width);
				Const.Pos = FVECTOR4(pt.x, pt.y, size, size);
				Const.Color.rgb = clr.rgb / (clr.MaxRGB() + 0.0001f);
				Const.Alpha = alpha * 2.0f;
				Const.Blend = sqrt(cis);

				pRenderGlares->SetVSConstants("Const", &Const, sizeof(Const));
				pRenderGlares->SetPSConstants("Const", &Const, sizeof(Const));
				// DrawIndexedPrimitiveUP becomes ShaderClass::DrawUP, which
				// copies through a scratch buffer -- there is no "draw from
				// this pointer in my memory" in Vulkan. THE LAST COUNT IS
				// INDICES, not primitives: 2 triangles is 6.
				pRenderGlares->DrawUP(Vertex, 4, sizeof(SMVERTEX), cIndex, 6);
			}
		}

		if (Config->bLocalGlares && pLightGlare)
		{
			pRenderGlares->SetTexture("tTex0", pLightGlare, IPF_CLAMP | IPF_LINEAR);
			pRenderGlares->UpdateTextures();

			// Render glares for local lights
			// int(nLights): nLights is a DWORD and i an int; the cast is the
			// same one-word answer to -Wsign-compare as elsewhere in this file.
			for (int i = 0; i < int(nLights); ++i) {
				int GPUId = Lights[i].GPUId;
				if (GPUId >= 0) {
					FVECTOR3 lpos = Lights[i].Position;
					if (WorldToScreenSpace2(_V(lpos), &pt)) {
						float size = 40.0f;
						Const.GPUId = (float(GPUId) + 0.5f) / desc.Width;
						Const.Pos = FVECTOR4(pt.x, pt.y, size, size);
						Const.Alpha = Lights[i].cone;
						Const.Color = Lights[i].Diffuse;
						Const.Blend = 1.0f;
						pRenderGlares->SetVSConstants("Const", &Const, sizeof(Const));
						pRenderGlares->SetPSConstants("Const", &Const, sizeof(Const));
						pRenderGlares->DrawUP(Vertex, 4, sizeof(SMVERTEX), cIndex, 6);
					}
				}
			}
		}
		pRenderGlares->DetachTextures();
	}
}


// ===========================================================================================
//
bool Scene::IsVisibleInCamera(const FVECTOR3 *pCnt, float radius)
{
	float z = Camera.z.x*pCnt->x + Camera.z.y*pCnt->y + Camera.z.z*pCnt->z;
	if (z<(-radius)) return false;
	if (z<0) z=-z;
	float y = Camera.y.x*pCnt->x + Camera.y.y*pCnt->y + Camera.y.z*pCnt->z;
	if (y<0) y=-y;
	if (y-(radius*Camera.vhf) > (Camera.vh*z)) return false;
	float x = Camera.x.x*pCnt->x + Camera.x.y*pCnt->y + Camera.x.z*pCnt->z;
	if (x<0) x=-x;
	if (x-(radius*Camera.vwf) > (Camera.vw*z)) return false;
	return true;
}

// ===========================================================================================
// D3DMAT_VectorMatrixMultiply becomes VMAT_VectorMatrixMultiply -- the same
// helper under the client's own prefix, and the same one that divides by w.
// ===========================================================================================
bool Scene::CameraDirection2Viewport(const VECTOR3 &dir, int &x, int &y)
{
	FVECTOR3 homog;
	FVECTOR3 idir = FVECTOR3( -float(dir.x), -float(dir.y), -float(dir.z) );
	VMAT_VectorMatrixMultiply(&homog, &idir, &Camera.mProjView);
	if (homog.x >= -1.0f && homog.y <= 1.0f && homog.z >= 0.0) {
		const DWORD width = Camera.viewportW != 0 ? Camera.viewportW : viewW;
		const DWORD height = Camera.viewportH != 0 ? Camera.viewportH : viewH;

		if(width == 0 || height == 0)
		{
			return false;
		}

		if (std::hypot(homog.x, homog.y) < 1e-6) {
			x = width / 2, y = height / 2;
		} else {
			x = (int)(width*0.5f*(1.0f + homog.x));
			y = (int)(height*0.5f*(1.0f - homog.y));
		}
		return true;
	}
	return false;
}


// ===========================================================================================
// SAFE_RELEASE(FX) becomes SAFE_DELETE(FX). An ID3DXEffect was reference
// counted; VulkanEffectFile is a plain heap object this class owns and made,
// so the release becomes a delete. Nothing in Vulkan is reference counted --
// see VulkanFrame.h on why SAFE_RELEASE has no counterpart anywhere in the
// converted client.
// ===========================================================================================
void Scene::GlobalExit()
{
	SAFE_DELETE(FX);
}

// ===========================================================================================
// D3D9TechInit becomes VulkanTechInit, and the effect it loads is
// SceneTech.glsl beside SceneTech.tech rather than SceneTech.fx.
//
// D3DXCreateEffectFromFile HAS NO COUNTERPART, and neither does the
// ID3DXBuffer of errors it filled. An .fx file is a Direct3D concept: it
// carried the shaders AND the technique/pass declarations AND the render
// states in one file, and D3DX compiled the lot. Vulkan has no such object,
// so VulkanEffectFile::Load reads the GLSL and a .tech table beside it -- see
// VulkanEffect.h. The two-branch error/warning inspection of the D3DX error
// buffer goes with it: glslang's log is written to Orbiter.log by
// CompileShaderStage itself, at the point of failure, with the file and entry
// point named, which is strictly more than the buffer said.
//
// The MessageBoxA on the error branch is kept -- the Linux shim provides it,
// and this failure is one the user has to see.
//
// THE SIGNATURE CHANGES FROM LPDIRECT3DDEVICE9 TO VulkanDevice*, which is
// what every TechInit in the client takes now, and the 'folder' argument
// still names the module directory: callers pass "VulkanClient".
// ===========================================================================================
void Scene::VulkanTechInit(VulkanDevice *pDev, const char *folder)
{
	char name[256];
	sprintf_s(name,256,"Modules/%s/SceneTech.glsl", folder);

	// Create the Effect from a .glsl file plus its .tech table.
	FX = new VulkanEffectFile(pDev);

	if (!FX->Load(name)) {
		LogErr("Failed to create an Effect (%s)",name);
		MessageBoxA(0, "SceneTech.glsl failed to compile. See Orbiter.log for details.",
					"SceneTech.glsl Error", 0);
		delete FX;
		FX = NULL;
		return;
	}

	eLine  = FX->GetTechniqueByName("LineTech");
	eStar  = FX->GetTechniqueByName("StarTech");
	// GetParameterByName drops D3DX's first argument. It was the parent
	// parameter to search inside; 0 meant the top level, which is the only
	// value the client ever passed.
	eWVP   = FX->GetParameterByName("gWVP");
	eTex0  = FX->GetParameterByName("gTex0");
	eColor = FX->GetParameterByName("gColor");

	VulkanCelestialSphere::VulkanTechInit(FX);
}

// ===========================================================================================
//
int distcomp (const void *arg1, const void *arg2)
{
	double d1 = ((PList*)arg1)->dist;
	double d2 = ((PList*)arg2)->dist;
	return (d1 > d2 ? -1 : d1 < d2 ? 1 : 0);
}
