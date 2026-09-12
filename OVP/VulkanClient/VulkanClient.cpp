// ==============================================================
// VulkanClient.cpp
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2006-2026 Martin Schweiger
//				 2012-2016 Jarmo Nikkanen
// ==============================================================
//
// This is the module entry point and the GraphicsClient override set. Most of
// it is core API and converts unchanged; the structural changes are stated
// once here rather than at each of the several dozen places they show up.
//
//  1. The core owns the device and the frame. On Windows the client created
//     the Direct3D device, the swap chain and the back buffer, wrapped its
//     rendering in BeginScene/EndScene and Present()ed. Here
//     Src/Orbiter/Linux/UIHost.cpp stands all of that up before any client
//     loads, and owns the only vkQueuePresentKHR in the process.
//
//  2. Lost devices do not exist. D3D9's TestCooperativeLevel / Reset() cycle,
//     which this file spends a good deal of code on, has no counterpart:
//     VK_ERROR_DEVICE_LOST is unrecoverable, and swapchain recreation on
//     resize is the core's job.
//
//  3. ImGui is the core's. The client draws through ImGuiDialog and never
//     touches a backend.
//
//  4. d3d9on12, NvOptimusEnablement and NVAPI have no counterpart: a Windows
//     D3D9-on-D3D12 migration path, a DWORD the NVIDIA driver reads out of a
//     PE export table to pick the discrete GPU, and a Windows-only Direct3D
//     library behind an #ifdef never defined in this tree.
// ==============================================================


#define STRICT 1
#define ORBITER_MODULE

#include <set> // ...for Brush-, Pen- and Font-accounting
#include "Orbitersdk.h"
#include "VulkanClient.h"
#include "VulkanConfig.h"
#include "VulkanUtil.h"
#include "VulkanCatalog.h"
#include "VulkanSurface.h"
#include "VulkanTextMgr.h"
#include "VulkanFrame.h"
#include "VulkanPad.h"
#include "CSphereMgr.h"
#include "Scene.h"
#include "Mesh.h"
#include "VVessel.h"
#include "VStar.h"
#include "MeshMgr.h"
#include "Particle.h"
#include "TileMgr.h"
#include "RingMgr.h"
#include "HazeMgr.h"
#include "Log.h"
#include "VideoTab.h"
#include "GDIPad.h"
#include "OapiExtension.h"
#include "DebugControls.h"
#include "Surfmgr2.h"
#include "gcCore.h"
#include "gcConst.h"
#include <unordered_map>
// <d3d9on12.h> and the two ImGui backend headers are gone; see notes 3 and 4.
#include "imgui.h"


// ==============================================================
// Structure definitions

struct VulkanClient::RenderProcData {
	__gcRenderProc proc;
	void *pParam;
	DWORD id;
};

struct VulkanClient::GenericProcData {
	__gcGenericProc proc;
	void *pParam;
	DWORD id;
};

using namespace oapi;

HINSTANCE g_hInst = 0;
VulkanClient *g_client = 0;
class gcConst* g_pConst = 0;
// g_pD3DObject is gone: the IDirect3D9 factory the VideoTab used to enumerate
// adapters. The VkInstance comes from the core's context.
Memgr<float>* g_pMemgr_f = nullptr;
Memgr<INT16>* g_pMemgr_i = nullptr;
Memgr<UINT8>* g_pMemgr_u = nullptr;
Memgr<WORD>* g_pMemgr_w = nullptr;
Memgr<VERTEX_2TEX>* g_pMemgr_vtx = nullptr;
Texmgr<VulkanTexture*>* g_pTexmgr_tt = nullptr;
Vtxmgr<VulkanBuffer*>* g_pVtxmgr_vb = nullptr;
Idxmgr<VulkanBuffer*>* g_pIdxmgr_ib = nullptr;


set<VulkanMesh*> MeshCatalog;
set<SurfNative*> SurfaceCatalog;
unordered_map<string, SURFHANDLE> SharedTextures;
unordered_map<string, SURFHANDLE> ClonedTextures;
unordered_map<MESHHANDLE, class SketchMesh*> MeshMap;
unordered_map<std::string, VulkanTexture*> MicroTextures;

DWORD uCurrentMesh = 0;
vObject *pCurrentVisual = 0;
_VulkanStats VulkanStats;

// The #ifdef _NVAPI_H StereoHandle is gone -- NVAPI is Windows-only.

bool bFreeze = false;
bool bFreezeEnable = false;
bool bFreezeRenderAll = false;

// Debuging Brush-, Pen- and Font-accounting
std::set<Font *> g_fonts;
std::set<Pen *> g_pens;
std::set<Brush *> g_brushes;

extern list<gcGUIApp *> g_gcGUIAppList;

// The extern "C" NvOptimusEnablement export is gone -- it is a DWORD the
// NVIDIA driver reads out of a PE export table to pick the discrete GPU.

// ==============================================================
// API interface
// ==============================================================

// ==============================================================
// Initialise module

DLLCLBK void InitModule(HINSTANCE hDLL)
{

#ifdef _DEBUG
	// _CrtSetDbgFlag / _CrtSetBreakAlloc are the MSVC debug heap and have no
	// counterpart. The assertions below check that oapi::FVECTOR4's union
	// views agree, which could differ between MSVC and GCC.

	assert(sizeof(FVECTOR4) == 16);
	assert(sizeof(FVECTOR4::data) == 16);

	assert(sizeof(FVECTOR4::rgb) == 12);
	assert(sizeof(FVECTOR4::xyz) == 12);

	auto dut = FVECTOR4(1.2, 3.4, 5.6, 7.8);
	assert(dut.data[0] == dut.r);
	assert(dut.data[1] == dut.g);
	assert(dut.data[2] == dut.b);
	assert(dut.data[3] == dut.a);

	assert(dut.data[0] == dut.x);
	assert(dut.data[1] == dut.y);
	assert(dut.data[2] == dut.z);
	assert(dut.data[3] == dut.w);

	assert(dut.rgb.x == dut.xyz.x);
	assert(dut.rgb.y == dut.xyz.y);
	assert(dut.rgb.z == dut.xyz.z);

	assert(dut.a == dut.w);

	// Check that 'a' and 'w' are placed behind 'rgb' rsp. 'xyz' and unaffected
	dut.rgb = 0;
	assert(dut.a == dut.w);
#endif

	VulkanInitLog("Modules/VulkanClient/VulkanClientLog.html");

	g_pMemgr_f = new Memgr<float>("float");
	g_pMemgr_i = new Memgr<INT16>("UINT16");
	g_pMemgr_u = new Memgr<UINT8>("UINT8");
	g_pMemgr_w = new Memgr<WORD>("WORD");
	g_pMemgr_vtx = new Memgr<VERTEX_2TEX>("VERTEX_2TEX");

	// D3DXCheckVersion asked whether the installed D3DX redistributable matched
	// the headers; the Vulkan loader answers the equivalent at vkCreateInstance
	// time, and the core has already done that.

	Config = new VulkanConfig();

	// The cache directories follow the module name. GetFileAttributesA and
	// CreateDirectoryA come from the Linux shim, over stat() and mkdir().
	if (Config->ShaderCacheUse) {
		DWORD fa = GetFileAttributesA("Cache");
		if (fa == INVALID_FILE_ATTRIBUTES) CreateDirectoryA("Cache", NULL);
		fa = GetFileAttributesA("Cache/VulkanClient");
		if (fa == INVALID_FILE_ATTRIBUTES) CreateDirectoryA("Cache/VulkanClient", NULL);
		fa = GetFileAttributesA("Cache/VulkanClient/Shaders");
		if (fa == INVALID_FILE_ATTRIBUTES) CreateDirectoryA("Cache/VulkanClient/Shaders", NULL);
	}

	g_pConst = new gcConst();

	DebugControls::Create();
	AtmoControls::Create();
	vPlanet::ParseMicroTexturesFile();

	VulkanStats.TilesRendered[RENDERPASS_MAINSCENE] = 0;
	VulkanStats.TilesRendered[RENDERPASS_CUSTOMCAM] = 0;
	VulkanStats.TilesRendered[RENDERPASS_ENVCAM] = 0;

	g_hInst = hDLL;
	g_client = new VulkanClient(hDLL);


	if (oapiRegisterGraphicsClient(g_client)==false) {
		delete g_client;
		g_client = 0;
	}
}

// ==============================================================
// Clean up module

DLLCLBK void ExitModule(HINSTANCE hDLL)
{
	LogAlw("--------------ExitModule------------");

	delete Config;
	delete g_pConst;

	if (g_client) {
		oapiUnregisterGraphicsClient(g_client);
		delete g_client;
		g_client = 0;
	}

	DebugControls::Release();
	AtmoControls::Release();

	// The #ifdef _NVAPI_H NvAPI_Unload block is gone -- NVAPI is Windows-only.

	LogAlw("Log Closed");
	VulkanCloseLog();

	SAFE_DELETE(g_pMemgr_f);
	SAFE_DELETE(g_pMemgr_i);
	SAFE_DELETE(g_pMemgr_u);
	SAFE_DELETE(g_pMemgr_w);
	SAFE_DELETE(g_pMemgr_vtx);
}

DLLCLBK gcGUIBase * gcGetGUICore()
{
	return dynamic_cast<gcGUIBase *>(g_pWM);
}


DLLCLBK gcConst * gcGetCoreAPI()
{
	return dynamic_cast<gcConst *>(g_pConst);
}



// ==============================================================
// VulkanClient class implementation
// ==============================================================

// Initialiser list reordered to declaration order (-Wreorder). pLoadLabel("")
// becomes pLoadLabel{}: a char array cannot be initialised from a parenthesised
// string literal outside MSVC.
VulkanClient::VulkanClient (HINSTANCE hInstance) :
	GraphicsClient(hInstance),
	pBltGrpTgt	(NULL),
	pBltSkp		(NULL),
	pDevice     (NULL),
	pDefaultTex (NULL),
	pScatterTest(NULL),
	pFramework  (NULL),
	pCaps       (NULL),
	scenarioName("(none selected)"),
	hMainThread (NULL),
	pWM			(NULL),
	pCustomSplashScreen(NULL),
	pSplashTextColor(0xE0A0A0),
	hRenderWnd(),
	bControlPanel (false),
	bScatterUpdate(false),
	bFullscreen   (false),
	bAAEnabled    (false),
	bFailed       (false),
	bRunning      (false),
	bVertexTex    (false),
	bVSync        (false),
	bRendering	  (false),
	bGDIClear	  (true),
	viewW         (0),
	viewH         (0),
	viewBPP       (0),
	frame_timer   (0),
	vtab(NULL),
	scene     (),
	meshmgr   (),
	hLblFont1   (NULL),
	hLblFont2   (NULL),
	pOverlayFont(NULL),
	pLoadLabel{},
	pLoadItem{},
	pItemsSkp   (NULL),
	loadd_x       (0),
	loadd_y       (0),
	loadd_w       (0),
	loadd_h       (0),
	LabelPos      (0)

{
}

// ==============================================================

VulkanClient::~VulkanClient()
{
	LogAlw("VulkanClient destructor called");
	SAFE_DELETE(vtab);
	// SAFE_RELEASE(g_pD3DObject) has no counterpart: the VkInstance is the
	// core's.
}


// ==============================================================
// 
bool VulkanClient::ChkDev(const char *fnc) const
{
	if (pDevice) return false;
	LogErr("Call [%s] Failed. Vulkan Graphics services off-line", fnc);
	return true;
}


// ==============================================================
// Overridden
//
const void *VulkanClient::GetConfigParam (DWORD paramtype) const
{
	return (paramtype >= CFGPRM_TILELOADTHREAD)
		 ? (paramtype >= CFGPRM_GETSELECTEDMESH)
		 ? DebugControls::GetConfigParam(paramtype)
		 : OapiExtension::GetConfigParam(paramtype)
		 : GraphicsClient::GetConfigParam(paramtype);
}


// The entire D3D9-creation block is gone -- the clearest example of note 1. It
// made the IDirect3D9 factory, optionally through Direct3DCreate9On12, and
// there is nothing here to create. OapiExtension::RunsUnderWINE() goes with
// it: it was asked here only to skip the 9-on-12 path.
bool VulkanClient::clbkInitialise()
{
	_TRACE;
	LogAlw("================ clbkInitialise ===============");
	LogAlw("Orbiter Version = %d",oapiGetOrbiterVersion());

	// Perform default setup
	if (GraphicsClient::clbkInitialise()==false) return false;

	//Create the Launchpad video tab interface
	// (char*) on the literal: oapiWriteLog takes char*, and GCC rejects the
	// implicit conversion from a string literal where MSVC permits it.
	oapiWriteLog((char*)"[Vulkan] Initialize VideoTab...");
	vtab = new VideoTab(this, ModuleInstance(), OrbiterInstance(), LaunchpadVideoTab());
	return vtab->Initialise();
}


// ==============================================================
// This is called when a simulation session will begin
//
HWND VulkanClient::clbkCreateRenderWindow()
{
	_TRACE;

	LogAlw("================ clbkCreateRenderWindow ===============");

	// `if (!g_pD3DObject) return NULL;` has no counterpart; what it reached
	// through, CVulkanFramework::Initialize gets from the core.

	Config->WriteParams();
	
	uEnableLog		 = Config->DebugLvl;
	pSplashScreen    = NULL;
	pBackBuffer      = NULL;
	pTextScreen      = NULL;
	hRenderWnd       = NULL;
	pDefaultTex		 = NULL;
	hLblFont1		 = NULL;
	hLblFont2		 = NULL;
	bControlPanel    = false;
	bFullscreen      = false;
	bFailed			 = false;
	bRunning		 = false;
	bVertexTex		 = false;
	viewW = viewH    = 0;
	viewBPP          = 0;
	frame_timer		 = 0;
	scene            = NULL;
	meshmgr          = NULL;
	pFramework       = NULL;
	pDevice			 = NULL;
	pBltGrpTgt		 = NULL;	// Let's set this NULL here, constructor is called only once. Not when exiting and restarting a simulation.
	pNoiseTex		 = NULL;
	// surfBltTgt is gone: GraphicsClient's own member, which nothing reads.
	// GetCurrentThread() is the shim's and returns the same pseudo-handle
	// Windows returns, so the two guards fed by hMainThread stay dead exactly
	// as they are there. See GetMainThread().
	hMainThread		 = GetCurrentThread();

	VMAT_Identity(&ident);

	oapiDebugString()[0] = '\0';

	MeshCatalog.clear();
	SurfaceCatalog.clear();

	hRenderWnd = GraphicsClient::clbkCreateRenderWindow();

	LogAlw("Window Handle = %s",_PTR(hRenderWnd));
	SetWindowText(hRenderWnd, "[VulkanClient]");

	LogOk("Starting to initialize device and 3D environment...");

	pFramework = new CVulkanFramework();

	WriteLog("[Vulkan Initialized]");

	HRESULT hr = pFramework->Initialize(hRenderWnd, GetVideoData());

	if (hr!=S_OK) {
		LogErr("ERROR: Failed to initialize 3D Framework");
		return NULL;
	}

	RECT rect;
	GetClientRect(hRenderWnd, &rect);
	HDC hWnd = GetDC(hRenderWnd);
	HBRUSH hBr = CreateSolidBrush(RGB(0,0,0));
	FillRect(hWnd, &rect, hBr);
	DeleteObject(hBr);
	ReleaseDC(hRenderWnd, hWnd);
	ValidateRect(hRenderWnd, NULL);	// avoids white flash after splash screen

	pCaps = pFramework->GetCaps();

	WriteLog("[3DDevice Initialized]");

	// GetD3DDevice() became GetVulkanDevice(); see VulkanFrame.h.
	pDevice		= pFramework->GetVulkanDevice();
	viewW		= pFramework->GetWidth();
	viewH		= pFramework->GetHeight();
	bFullscreen = (pFramework->IsFullscreen() == TRUE);
	bAAEnabled  = (pFramework->IsAAEnabled() == TRUE);
	viewBPP		= 32;
	bVertexTex  = (pFramework->HasVertexTextureSup() == TRUE);
	bVSync		= (pFramework->GetVSync() == TRUE);

	char fld[] = "VulkanClient";

	g_pTexmgr_tt = new Texmgr<VulkanTexture*>(pDevice, "TileTextures");
	g_pVtxmgr_vb = new Vtxmgr<VulkanBuffer*>(pDevice, "TileVertex");
	g_pIdxmgr_ib = new Idxmgr<VulkanBuffer*>(pDevice, "TileIndices");

	// D3DXCreateTextureFromFileA becomes NatLoadTexture, the client's own
	// loader. The file name is unchanged: Textures/D3D9Noise.dds is a shipped
	// asset, not an API name, and renaming it would break every installation.
	pNoiseTex = NatLoadTexture("Textures/D3D9Noise.dds");
	if (!pNoiseTex) LogErr("Failed to load Textures/D3D9Noise.dds");

	// GetRenderTarget(0) and GetDepthStencilSurface() have no counterpart: the
	// swapchain images belong to the core, so what the client gets are the
	// attachment proxies -- extent and format, no image.
	pBackBuffer   = pFramework->GetRenderTargetProxy();
	pDepthStencil = pFramework->GetDepthStencilProxy();

	LogAlw("Render Target = %s", _PTR(pBackBuffer));
	LogAlw("DepthStencil = %s", _PTR(pDepthStencil));

	meshmgr		= new MeshManager(this);

	// Bring Sketchpad Online
	VulkanPadFont::VulkanTechInit(pDevice);
	VulkanPadPen::VulkanTechInit(pDevice);
	VulkanPadBrush::VulkanTechInit(pDevice);
	VulkanText::VulkanTechInit(this, pDevice);
	VulkanPad::VulkanTechInit(this, pDevice);

	deffont = (oapi::Font*) new VulkanPadFont(20, true, "fixed");
	defpen  = (oapi::Pen*)  new VulkanPadPen(1, 1, 0x00FF00);

	pDefaultTex = SURFACE(clbkLoadTexture("Null.dds"));
	if (pDefaultTex==NULL) LogErr("Null.dds not found");

	int x=0;
	if (viewW>1282) x=4;

	hLblFont1 = CreateFont(24+x, 0, 0, 0, 700, false, false, 0, 0, 3, 2, 1, 49, "Courier New");
	hLblFont2 = CreateFont(18+x, 0, 0, 0, 700, false, false, 0, 0, 3, 2, 1, 49, "Courier New");

	// The debug overlay font, with the parameters CD3DFramework9 gave D3DX
	// for pLargeFont: 30px, bold, Arial. See pOverlayFont in the header.
	pOverlayFont = clbkCreateFont(30, true, "Arial", FONT_BOLD);

	SplashScreen();  // Warning SurfNative is not yet fully initialized here

	ShowWindow(hRenderWnd, SW_SHOW);

	OutputLoadStatus("Building Shader Programs...",0);

	VulkanEffect::VulkanTechInit(this, pDevice, fld);

	// Device-specific initialisations

	TileManager::GlobalInit(this);
	TileManager2Base::GlobalInit(this);
	RingManager::GlobalInit(this);
	HazeManager::GlobalInit(this);
	HazeManager2::GlobalInit(this);
	VulkanParticleStream::GlobalInit(this);
	CSphereManager::GlobalInit(this);
	vStar::GlobalInit(this);
	vObject::GlobalInit(this);
	vVessel::GlobalInit(this);
	vPlanet::GlobalInit(this);
	OapiExtension::GlobalInit(*Config);

	OutputLoadStatus("SceneTech.glsl",1);
	Scene::VulkanTechInit(pDevice, fld);
	VulkanMesh::GlobalInit(pDevice);

	// Create scene instance
	scene = new Scene(this, viewW, viewH);

	WriteLog("[VulkanClient Initialized]");
	LogOk("...3D environment initialised");

	// The #ifdef _NVAPI_H stereo block stood here -- NVAPI is Windows-only.

	// Create status queries -----------------------------------------
	//
	// Four CreateQuery calls with a NULL out-pointer -- D3D9's idiom for "is
	// this query type supported" -- whose answers were only logged. Occlusion
	// queries are a core Vulkan requirement, and the three timing types were
	// driver-specific counters whose nearest equivalent is one feature bit.
	LogAlw("VK_QUERY_TYPE_OCCLUSION is supported by device (core requirement); "
		   "precise counts %s", pDevice->GetFeatures()->occlusionQueryPrecise
		   ? "supported" : "not supported");
	LogAlw("VK_QUERY_TYPE_PIPELINE_STATISTICS %s by device",
		   pDevice->GetFeatures()->pipelineStatisticsQuery
		   ? "is supported" : "not supported");

	return hRenderWnd;
}



// ==============================================================
// This is called when the simulation is ready to go but the clock
// is not yet ticking
//
void VulkanClient::clbkPostCreation()
{
	_TRACE;
	LogAlw("================ clbkPostCreation ===============");

	if (scene) scene->Initialise();

	// Create Window Manager -----------------------------------------
	//
	if (Config->gcGUIMode != 0) {
		pWM = new WindowManager(hRenderWnd, ModuleInstance(), !(GetVideoData()->fullscreen));
		if (pWM->IsOK() == false) SAFE_DELETE(pWM);
	}

	bRunning = true;

	// bRunning's other half, on the host's side of the boundary. The window,
	// the swapchain and the only present belong to UIHost.cpp, and it gates
	// both on a session flag -- whether to call this client's scene callback
	// at all, and whether the host window has any reason to be on screen.
	// Registering the callback and raising the flag are both required, and
	// this client did neither: a scenario launched to a blank desktop with
	// nothing in the log to say why.
	orbiter_SetSceneRenderCallback(&VulkanClient::SceneRenderThunk, this);

	orbiter_BeginSession();

	// The reference releases pSplashScreen when loading is over; here the
	// pixels belong to the host, so it is told.
	orbiter_ClearSplash();

	LogAlw("=============== Loading Completed and Visuals Created ================");

#ifdef _DEBUG
	SketchPadTest();
#endif

	WriteLog("[Scene Initialized]");
}

// Core API throughout. "D3D9/SketchpadTest.dds" becomes
// "Vulkan/SketchpadTest.dds", a directory this module ships under its own name.
void VulkanClient::SketchPadTest()
{
	SURFHANDLE hSrc = clbkLoadSurface("Vulkan/SketchpadTest.dds", OAPISURFACE_TEXTURE);
	SURFHANDLE hTgt = clbkLoadSurface("Vulkan/SketchpadTest.dds", OAPISURFACE_RENDERTARGET);

	if (!hSrc || !hTgt) return;

	oapiSetSurfaceColourKey(hSrc, 0xFFFFFFFF);

	Sketchpad *pSkp = oapiGetSketchpad(hTgt);

	RECT r = { 17, 1, 31, 15 };
	RECT t = { 1, 33, 31, 63 };
	RECT q = { 0, 16, 16, 32 };
	RECT w = { 49, 1, 63, 15 };

	pSkp->CopyRect(hSrc, &r, 1, 1);
	pSkp->ColorKey(hSrc, &r, 33, 1);
	pSkp->SetBlendState((Sketchpad::BlendState)(Sketchpad::BlendState::ALPHABLEND | Sketchpad::BlendState::FILTER_POINT));
	pSkp->StretchRect(hSrc, &r, &t);
	pSkp->SetBlendState();
	pSkp->StretchRect(hSrc, &r, &w);
	pSkp->RotateRect(hSrc, &q, 24, 24, float(PI05));
	pSkp->RotateRect(hSrc, &q, 40, 24, float(PI));
	pSkp->RotateRect(hSrc, &q, 56, 24, float(-PI05));

	RECT tr = { 33, 49, 47, 63 };
	pSkp->ColorFill(0xFFFF00FF, &tr);

	pSkp->QuickPen(0xFF000000);
	pSkp->QuickBrush(0x80FFFF00);
	pSkp->Rectangle(49, 33, 63, 47); // 1px margin
	pSkp->Ellipse(49, 49, 63, 63);	// 1px margin

									// No Pen, Brush Only
	pSkp->SetPen(NULL);
	pSkp->QuickBrush(0xFF00FF00);
	pSkp->Rectangle(65, 33, 79, 47); // 1px tlm 1px brm
	pSkp->Ellipse(65, 49, 79, 63);	// 1px tlm 1px brm

	pSkp->QuickPen(0xFFFFFFFF);
	pSkp->MoveTo(65, 1);
	pSkp->LineTo(65, 14);
	pSkp->LineTo(78, 14);
	pSkp->LineTo(78, 1);
	pSkp->LineTo(65, 1);

	RECT cr = { 33, 33, 47, 47 };
	pSkp->ClipRect(&cr);
	pSkp->ColorFill(0xFFFFFF00, NULL);
	pSkp->ClipRect();

	oapiReleaseSketchpad(pSkp);

	oapiSaveSurface("SketchpadOutput", hTgt, ImageFileFormat::IMAGE_PNG);

	oapiReleaseTexture(hSrc);
	oapiReleaseTexture(hTgt);


	 
	// Run Different Kind of Tests
	// 

	hTgt = oapiCreateSurfaceEx(768, 512, OAPISURFACE_RENDERTARGET);

	if (!hTgt) return;

	gcCore2* pCore = gcGetCoreInterface();

	if (!pCore) return;

	float a = 0.0f;
	float s = float(PI2 / 6.0);

	gcCore::clrVtx Vtx[8];
	oapi::FVECTOR2 Pol[6];

	for (int i = 0; i < 6; i++) {
		Pol[i].x = cos(a);
		Pol[i].y = sin(a);
		Vtx[i + 1].pos.x = Pol[i].x;
		Vtx[i + 1].pos.y = Pol[i].y;
		a += s;
	}

	//				 AABBGGRR
	Vtx[1].color = 0xFFFF0000;
	Vtx[2].color = 0xFFFFFF00;
	Vtx[3].color = 0xFF00FF00;
	Vtx[4].color = 0xFF00FFFF;
	Vtx[5].color = 0xFF0000FF;
	Vtx[6].color = 0xFFFF00FF;

	// Center vertex
	Vtx[0].pos.x = 0.0f;
	Vtx[0].pos.y = 0.0f;
	Vtx[0].color = 0xFFFFFFFF;    // White
	Vtx[7] = Vtx[1];

	HPOLY hColors = pCore->CreateTriangles(NULL, Vtx, 8, PF_FAN);
	HPOLY hOutline = pCore->CreatePoly(NULL, Pol, 6, PF_CONNECT);
	HPOLY hOutline2 = pCore->CreatePoly(NULL, Pol, 6);

	Vtx[0].color = 0xFFFF0000;    
	Vtx[1].color = 0xFFFFFF00;	  
	Vtx[2].color = 0xFFF0FF00;   
	Vtx[3].color = 0xFF00FFFF;	
	Vtx[4].color = 0xFF0000FF;    
	Vtx[5].color = 0xFFFF00FF;

	Vtx[0].pos = FVECTOR2(-1, 0);
	Vtx[1].pos = FVECTOR2(-1, 1);
	Vtx[2].pos = FVECTOR2(0, 0);
	Vtx[3].pos = FVECTOR2(0, 1);
	Vtx[4].pos = FVECTOR2(1, 0);
	Vtx[5].pos = FVECTOR2(1, 1);

	HPOLY hStrip = pCore->CreateTriangles(NULL, Vtx, 6, PF_STRIP);


	Vtx[0].color = 0xFF00FF00;    // Green
	Vtx[1].color = 0xFF00FF00;	  // Green	
	Vtx[2].color = 0xFFFF00FF;    // Mangenta
	Vtx[3].color = 0xFFFF00FF;	  // Mangenta
	Vtx[4].color = 0xFF0000FF;    // Blue
	Vtx[5].color = 0xFF0000FF;	  // Blue

	Vtx[0].pos = FVECTOR2(-1, 1);
	Vtx[1].pos = FVECTOR2(-1, 0);
	Vtx[2].pos = FVECTOR2(0, 1);
	Vtx[3].pos = FVECTOR2(0, 0);
	Vtx[4].pos = FVECTOR2(1, 1);
	Vtx[5].pos = FVECTOR2(1, 0);

	HPOLY hStrip2 = pCore->CreateTriangles(NULL, Vtx, 6, PF_STRIP);


	IVECTOR2 pos0 = { 128, 128 };
	IVECTOR2 pos1 = { 128, 384 };
	IVECTOR2 pos2 = { 384, 128 };
	IVECTOR2 pos3 = { 640, 128 };
	IVECTOR2 pos4 = { 384, 384 };
	IVECTOR2 pos5 = { 640, 384 };

	pSkp = oapiGetSketchpad(hTgt);

	pSkp->ColorFill(0xFFFFFFFF, NULL);

	pSkp->QuickBrush(0xA0000088);
	pSkp->QuickPen(0xA0000000, 3.0f);
	pSkp->PushWorldTransform();

	// ptr(FVECTOR2(...)) becomes a named local: ptr() existed to take the
	// address of a temporary, which C++ forbids and MSVC allowed.
	FVECTOR2 sc100(100.0f, 100.0f);

	pSkp->SetWorldScaleTransform2D(&sc100, &pos0);
	pSkp->DrawPoly(hColors);
	pSkp->DrawPoly(hOutline);

	pSkp->SetWorldScaleTransform2D(&sc100, &pos1);
	pSkp->DrawPoly(hOutline2);

	pSkp->SetWorldScaleTransform2D(&sc100, &pos2);
	pSkp->DrawPoly(hStrip);

	pSkp->SetWorldScaleTransform2D(&sc100, &pos3);
	pSkp->DrawPoly(hStrip2);

	pSkp->SetWorldScaleTransform2D(&sc100, &pos4);
	pSkp->QuickPen(0xFF000000, 25.0f);
	pSkp->DrawPoly(hOutline);

	hSrc = clbkLoadSurface("generic/noisep.dds", OAPISURFACE_TEXTURE);

	FVECTOR2 sc1(1.0f, 1.0f);
	pSkp->SetWorldScaleTransform2D(&sc1, &pos5);

	FVECTOR2 pt[4];
	pt[0] = FVECTOR2(-100.0f, -100.0f);
	pt[1] = FVECTOR2(-100.0f, 50.0f);
	pt[2] = FVECTOR2(100.0f, 100.0f);
	pt[3] = FVECTOR2(100.0f, -50.0f);


	pSkp->CopyTetragon(hSrc, NULL, pt);
	pSkp->PopWorldTransform();


	oapiReleaseTexture(hSrc);
	oapiReleaseSketchpad(pSkp);


	pCore->DeletePoly(hColors); // Must release Sketchpad before releasing any sketchpad resources
	pCore->DeletePoly(hOutline);
	pCore->DeletePoly(hOutline2);
	pCore->DeletePoly(hStrip);
	pCore->DeletePoly(hStrip2);

	// IMAGE_DDS becomes IMAGE_PNG: NatSaveSurface has no DDS writer, no Vulkan
	// call produces BC blocks and the client carries no compressor.
	oapiSaveSurface("SketchpadOutput2", hTgt, ImageFileFormat::IMAGE_PNG);

	oapiReleaseTexture(hTgt);
}




// ==============================================================
// Called when simulation session is about to be closed
//
void VulkanClient::clbkCloseSession(bool fastclose)
{

	LogAlw("================ clbkCloseSession ===============");

	// The partner of orbiter_BeginSession(), and it has to come first: it clears
	// the host's session flag, so the frame pump stops calling this client's
	// scene callback before anything the callback touches is torn down.
	orbiter_EndSession();

	// And then wait for the frames that were already submitted.
	// orbiter_EndSession stops the host starting another; it does not wait for
	// the ones in flight, and everything below deletes objects those frames
	// are still reading. On Windows nothing had to wait -- the runtime held a
	// reference for as long as its queued commands needed one -- and here the
	// layer named four kinds of casualty at once (sampler, pipeline, buffer,
	// image view, ten of each, the report cap) before the segfault that closed
	// every session. DestroyObjects does this same pair, but at the END of the
	// teardown, by which time the scene is gone.
	//
	// vkDeviceWaitIdle alone is not enough: the frames' command buffers still
	// NAME the descriptor sets and images, so the pools are reset too. And
	// under the device lock, because the tile loaders are still running and
	// vkDeviceWaitIdle is host access to the queue they submit on.
	if (pFramework && pFramework->GetVulkanDevice() &&
		pFramework->GetVulkanDevice()->GetDevice() != VK_NULL_HANDLE) {
		orbiter_LockDevice();
		vkDeviceWaitIdle(pFramework->GetVulkanDevice()->GetDevice());
		orbiter_ResetFrameCommands();
		orbiter_UnlockDevice();
	}

	//	Post shutdown signals for gcGUI applications
	//
	for (auto pApp : g_gcGUIAppList) pApp->clbkShutdown();

	//	Post shutdown signals for user applications
	//
	if (IsGenericProcEnabled(GENERICPROC_SHUTDOWN)) MakeGenericProcCall(GENERICPROC_SHUTDOWN, 0, NULL);


	// Check the status of RenderTarget Stack ------------------------------------------------
	//
	if (RenderStack.empty() == false) {
		LogErr("RenderStack contains %d items:", (int)RenderStack.size());
		while (!RenderStack.empty()) {
			LogErr("RenderTarget=%s, DepthStencil=%s", _PTR(RenderStack.front().pColor), _PTR(RenderStack.front().pDepthStencil));
			RenderStack.pop_front();
		}
	}

	// Disable rendering and some other systems
	//
	bRunning = false;


	// At first, shutdown tile loaders -------------------------------------------------------
	//
	if (TileBuffer::ShutDown()==false) LogErr("Failed to Shutdown TileBuffer()");
	if (TileManager2Base::ShutDown()==false) LogErr("Failed to Shutdown TileManager2Base()");

	// Close dialog if Open and disconnect a visual form debug controls
	DebugControls::Close();

	// Disconnect textures from pipeline (Unlikely nesseccary)
	VulkanEffect::ShutDown();

	// DEBUG: List all textures connected to meshes
	/* DWORD cnt = MeshCatalog->CountEntries();
	for (DWORD i=0;i<cnt;i++) {
		VulkanMesh *x = (VulkanMesh*)MeshCatalog->Get(i);
		if (x) x->DumpTextures();
	} */
	//GraphicsClient::clbkCloseSession(fastclose);
	pCustomSplashScreen = NULL;
	pSplashTextColor = 0xE0A0A0;

	SAFE_DELETE(pWM);
	LogAlw("================= Deleting Scene ================");
	Scene::GlobalExit();
	SAFE_DELETE(scene);
	LogAlw("============== Deleting Mesh Manager ============");
	SAFE_DELETE(meshmgr);
	WriteLog("[Session Closed. Scene deleted.]");

}


// SAFE_RELEASE becomes DestroyTexture for every image here. Release()
// decremented a count and destroyed only at zero, where DestroyTexture
// destroys unconditionally -- correct for each of these, but the reason
// SAFE_RELEASE must never be translated mechanically.
void VulkanClient::clbkDestroyRenderWindow (bool fastclose)
{
	_TRACE;
	oapiWriteLog((char*)"Vulkan: [Destroy Render Window Called]");
	LogAlw("============= clbkDestroyRenderWindow ===========");

	// The #ifdef _NVAPI_H stereo-handle teardown stood here.

	LogAlw("===== Calling GlobalExit() for sub-systems ======");
	HazeManager::GlobalExit();
	HazeManager2::GlobalExit();
	TileManager::GlobalExit();
	TileManager2Base::GlobalExit();
	VulkanParticleStream::GlobalExit();
	CSphereManager::GlobalExit();
	vStar::GlobalExit();
	vVessel::GlobalExit();
	vPlanet::GlobalExit();
	vObject::GlobalExit();
	VulkanMesh::GlobalExit();

	SAFE_DELETE(defpen);
	SAFE_DELETE(deffont);

	DeleteObject(hLblFont1);
	DeleteObject(hLblFont2);

	// The counterpart of CD3DFramework9's SAFE_RELEASE(pLargeFont), which
	// stood in DestroyObjects. See pOverlayFont in the header.
	if (pOverlayFont) { clbkReleaseFont(pOverlayFont); pOverlayFont = NULL; }

	VulkanPad::GlobalExit();
	VulkanText::GlobalExit();
	VulkanEffect::GlobalExit();

	DELETE_SURFACE(pSplashScreen);	// Splash screen related
	DELETE_SURFACE(pTextScreen);	// Splash screen related
	DELETE_SURFACE(pDefaultTex);
	if (pNoiseTex)     { pDevice->DestroyTexture(pNoiseTex);     pNoiseTex = NULL; }

	SURFHANDLE hBackBuffer = GetBackBufferHandle();

	DELETE_SURFACE(hBackBuffer);

	LogAlw("============ Checking Object Catalogs ===========");

	// Clear microtextures --------------------------------------------------------------------------------------
	//
	for (auto& it : MicroTextures) if (it.second) pDevice->DestroyTexture(it.second);
	MicroTextures.clear();


	// Check surface catalog --------------------------------------------------------------------------------------
	//
	if (SharedTextures.size() > 0)
	{
		LogWrn("Texture Repository has %u entries... Releasing...", (DWORD)SharedTextures.size());
		auto Undeleted(SharedTextures);
		for (auto srf : Undeleted) {
			LogWrn("Texture [%s]", SURFACE(srf.second)->GetName());
			delete lpSurfNative(srf.second);
		}
	}

	// Check surface catalog --------------------------------------------------------------------------------------
	//
	if (SurfaceCatalog.size() > 0)
	{
		LogErr("UnDeleted Surfaces(s) Detected %u... Releasing...", (DWORD)SurfaceCatalog.size());
		auto Undeleted(SurfaceCatalog);
		for (auto srf : Undeleted) {
			LogErr("Surface [%s] (%u, %u)", srf->GetName(), srf->GetWidth(), srf->GetHeight());
			delete srf;
		}
	}

	// Check mesh catalog --------------------------------------------------------------------------------------
	//
	if (MeshCatalog.size() > 0)
	{
		LogErr("UnDeleted Meshe(s) Detected %u", (DWORD)MeshCatalog.size());
		auto Undeleted(MeshCatalog);
		for (auto msh : Undeleted)
		{
			LogErr("Mesh[%s] Handle = %s ", msh->GetName(), _PTR(msh));
			delete msh;
		}
	}

	// Check Fonts catalog --------------------------------------------------------------------------------------
	//
	if (g_fonts.size()) {
		LogWrn("%u un-released fonts!", (DWORD)g_fonts.size());
		for (auto it = g_fonts.begin(); it != g_fonts.end(); ) {
			clbkReleaseFont(*it++);
		}
		g_fonts.clear();
	}

	// --- Brushes
	if (g_brushes.size()) {
		LogWrn("%u un-released brushes!", (DWORD)g_brushes.size());
		for (auto it = g_brushes.begin(); it != g_brushes.end(); ) {
			clbkReleaseBrush(*it++);
		}
		g_brushes.clear();
	}

	// --- Pens
	if (g_pens.size()) {
		LogWrn("%u un-released pens!", (DWORD)g_pens.size());
		for (auto it = g_pens.begin(); it != g_pens.end(); ) {
			clbkReleasePen(*it++);
		}
		g_pens.clear();
	}

	// Check tile catalog --------------------------------------------------------------------------------------
	//

	for (auto it : MeshMap)	SAFE_DELETE(it.second);

	MeshMap.clear();
	SharedTextures.clear();
	SurfaceCatalog.clear();
	MeshCatalog.clear();

	g_pTexmgr_tt->CleanUp();
	g_pVtxmgr_vb->CleanUp();
	g_pIdxmgr_ib->CleanUp();
	SAFE_DELETE(g_pTexmgr_tt);
	SAFE_DELETE(g_pVtxmgr_vb);
	SAFE_DELETE(g_pIdxmgr_ib);

	pFramework->DestroyObjects();

	SAFE_DELETE(pFramework);

	// Close Render Window -----------------------------------------
	GraphicsClient::clbkDestroyRenderWindow(fastclose);

	hRenderWnd		 = NULL;
	pDevice			 = NULL;
	bFailed			 = false;
	viewW = viewH    = 0;
	viewBPP          = 0;

}

// ==============================================================

void VulkanClient::clbkDebugString(const char* str)
{
	VulkanDebugLog("%s", str);
}


// ==============================================================

void VulkanClient::PushSketchpad(SURFHANDLE surf, VulkanPad *pSkp) const
{
	if (surf) {
		VulkanTexture *pTgt = SURFACE(surf)->GetSurface();
		VulkanTexture *pDep = SURFACE(surf)->GetDepthStencil();
		PushRenderTarget(pTgt, pDep, RENDERPASS_SKETCHPAD);
		RenderStack.front().pSkp = pSkp;
	}
}



// ==============================================================
// The render-target stack is the biggest structural conversion in this file,
// so the argument is written out once and the three functions below follow it.
//
// A render target is DEVICE STATE in D3D9: SetRenderTarget,
// SetDepthStencilSurface and SetViewport point the device at a pair of
// surfaces. In Vulkan the attachment set is baked into a VkFramebuffer inside
// a VkRenderPass, a pass cannot begin inside another, and the viewport is
// dynamic state on a command buffer -- so the three device calls become one
// BeginOffscreen, and only one pass can be open at a time, which is why each
// of these ends the current one first.
//
// The back buffer is the exception and must be: pBackBuffer is an attachment
// proxy holding no VkImage, and the core's render pass is already open when
// the scene callback runs, so pushing it begins nothing.
// ==============================================================

void VulkanClient::PushRenderTarget(VulkanTexture *pColor, VulkanTexture *pDepthStencil, int code) const
{
	static const char *labels[] = { "NULL", "MAIN", "ENV", "CUSTOMCAM", "SHADOWMAP", "PICK", "SKETCHPAD", "OVERLAY" };

	RenderTgtData data;
	data.pColor = pColor;
	data.pDepthStencil = pDepthStencil;
	data.pSkp = NULL;
	data.code = code;

	// Only one pass may be open; end the one underneath, exactly as
	// SetRenderTarget replaced the previous binding.
	if (pDevice->IsOffscreen()) pDevice->EndOffscreen();

	// SetViewport is gone as a separate call: BeginOffscreen takes the viewport
	// and scissor from the extent of the attachments it is given.
	if (pColor && !pColor->IsProxy()) {
		if (!pDevice->BeginOffscreen(pColor, pDepthStencil)) {
			LogErr("PushRenderTarget: BeginOffscreen failed for %s", _PTR(pColor));
		}
	}

	RenderStack.push_front(data);
	LogDbg("Plum", "PUSH:RenderStack[%lu]={%s, %s} %s", (unsigned long)RenderStack.size(), _PTR(data.pColor), _PTR(data.pDepthStencil), labels[data.code]);
}

// The Windows version changes the DEVICE binding and leaves the stack alone,
// so a following Pop restores what is underneath rather than what this call
// installed. Same asymmetry, same consequences.
void VulkanClient::AlterRenderTarget(VulkanTexture *pColor, VulkanTexture *pDepthStencil)
{
	if (pDevice->IsOffscreen()) pDevice->EndOffscreen();

	if (pColor && !pColor->IsProxy()) {
		if (!pDevice->BeginOffscreen(pColor, pDepthStencil)) {
			LogErr("AlterRenderTarget: BeginOffscreen failed for %s", _PTR(pColor));
		}
	}
}

// ==============================================================

void VulkanClient::PopRenderTargets() const
{
	static const char *labels[] = { "NULL", "MAIN", "ENV", "CUSTOMCAM", "SHADOWMAP", "PICK", "SKETCHPAD", "OVERLAY" };

	assert(RenderStack.empty() == false);

	if (pDevice->IsOffscreen()) pDevice->EndOffscreen();

	RenderStack.pop_front();

	if (RenderStack.empty()) {
		LogDbg("Orange", "POP: Last one out ------------------------------------");
		return;
	}

	RenderTgtData data = RenderStack.front();

	if (data.pColor && !data.pColor->IsProxy()) {
		if (!pDevice->BeginOffscreen(data.pColor, data.pDepthStencil)) {
			LogErr("PopRenderTargets: BeginOffscreen failed for %s", _PTR(data.pColor));
		}
	}

	LogDbg("Plum", "POP:RenderStack[%lu]={%s, %s, %s} %s", (unsigned long)RenderStack.size(), _PTR(data.pColor), _PTR(data.pDepthStencil), _PTR(data.pSkp), labels[data.code]);
}

// HackFriendlyHack has nothing left to do. It put the D3D device "in 'more'
// expected state" for a debugger -- viewport, render target and depth-stencil
// surface -- and all three were device state. Kept as an empty body.
void VulkanClient::HackFriendlyHack()
{
}

// ==============================================================

VulkanTexture *VulkanClient::GetTopDepthStencil()
{
	if (RenderStack.empty()) return NULL;
	return RenderStack.front().pDepthStencil;
}

// ==============================================================

VulkanTexture *VulkanClient::GetTopRenderTarget()
{
	if (RenderStack.empty()) return NULL;
	return RenderStack.front().pColor;
}

// ==============================================================

VulkanPad *VulkanClient::GetTopInterface() const
{
	if (RenderStack.empty()) return NULL;
	return RenderStack.front().pSkp;
}


// ==============================================================

void VulkanClient::clbkUpdate(bool running)
{
	_TRACE;
	double tot_update = VulkanGetTime();
	if (bFailed==false && bRunning) scene->Update();
	VulkanSetTime(VulkanStats.Timer.Update, tot_update);
}


// ==============================================================

double frame_time = 0.0;
double scene_time = 0.0;

void VulkanClient::clbkRenderScene()
{
	_TRACE;

	if (pDevice==NULL || scene==NULL) return;
	if (bFailed) return;
	if (!bRunning) return;

	// The counterpart of pDevice->BeginScene(). It opens no command buffer -- it
	// declares that a scene frame is being built, which is what makes the host
	// call this client's scene callback. EndSceneFrame is in PresentScene.
	orbiter_BeginSceneFrame();
}


// ==============================================================
// The scene is recorded here, not in clbkRenderScene, and this is the largest
// structural difference in the whole client. The reference draws the moment
// Orbiter asks, because a D3D9 device is always ready to record; the only
// Vulkan command buffer that reaches the swapchain belongs to UIHost.cpp's
// frame pump and exists solely for the duration of the callback it makes. So
// clbkRenderScene only opens the frame and everything it used to do lives
// here, in the same order and once per frame.
//
// Without this the client rendered into no command buffer at all -- several
// hundred "outside a frame" errors per frame, and a black scene under the
// host's own menu bar and HUD.
// ==============================================================
void VulkanClient::SceneRenderThunk(void *cmdBuf, void *renderPass,
									unsigned width, unsigned height, void *user)
{
	VulkanClient *pThis = (VulkanClient *)user;
	if (pThis) pThis->RenderSceneWork((VkCommandBuffer)cmdBuf, width, height);
}

void VulkanClient::RenderSceneWork(VkCommandBuffer cmd, unsigned width, unsigned height)
{
	if (pDevice == NULL || scene == NULL) return;
	if (bFailed || !bRunning) return;

	// The frame's buffer is the host's and is valid only until this returns, so
	// it is published for the duration and withdrawn at the end.
	pDevice->SetFrameCommandBuffer(cmd, width, height);

	if (getenv("ORBITER_VK_TRACE_TILES")) {
		static int n = 0;
		if (n++ < 3)
			LogErr("FRAMETRACE core scene callback cmd=%p %ux%u", (void *)cmd, width, height);
	}

	if (pWM) pWM->Animate();

	// The frame boundary for every descriptor pool in the client, with no
	// counterpart in the reference: a D3D9 draw consumed its state
	// immediately, where a Vulkan draw only records a REFERENCE to a
	// descriptor set that has to outlive the frame that named it.
	//
	// EVERY effect file, not VulkanEffect::FX alone. There are four, and this
	// line used to reset one, so the other three filled their per-frame
	// uniform arenas once and then refused every pass for the rest of the
	// session -- 2451 x "the frame's uniform arena is full" -- with BeginPass
	// returning false in silence. Both ResetFrame bodies also ROTATE their
	// pools rather than resetting in place, because the core keeps
	// orbiter_GetFramesInFlight() frames and resetting a pool the GPU was
	// still reading gave intermittent VK_ERROR_DEVICE_LOST.
	VulkanEffectFile::ResetFrameAll();
	ShaderClass::ResetFrame();

	// `if (Config->PresentLocation == 1) PresentScene();` is gone with the
	// setting: it chose where the present happened, and there is none to place.

	scene_time = VulkanGetTime();

	// TestCooperativeLevel() has no counterpart, and here the absence is
	// load-bearing. A D3D9 device could be LOST -- another application taking
	// exclusive fullscreen, a mode change, a driver reset -- after which every
	// call failed until Reset() succeeded. VK_ERROR_DEVICE_LOST is
	// unrecoverable and is reported by the call that hit it, and those calls
	// belong to UIHost.cpp. The resize half is the core's too; the client's
	// pipelines key on the render pass and rebuild themselves.

	// GetAvailableTextureMem() becomes GetLocalMemorySize(), which reports what
	// EXISTS rather than what is FREE, so the "nearly out" test below can never
	// fire on a card with more than 32MB. The true figure needs
	// VK_EXT_memory_budget.
	UINT mem = UINT(pDevice->GetLocalMemorySize()>>20);
	if (mem<32) TileBuffer::HoldThread(true);

	scene->RenderMainScene();		// Render the main scene

	VESSEL *hVes = oapiGetFocusInterface();

	if (hVes && Config->LabelDisplayFlags)
	{
		char Label[7] = "";
		if (Config->LabelDisplayFlags & VulkanConfig::LABEL_DISPLAY_RECORD && hVes->Recording()) strcpy_s(Label, 7, "Record");
		if (Config->LabelDisplayFlags & VulkanConfig::LABEL_DISPLAY_REPLAY && hVes->Playback()) strcpy_s(Label, 7, "Replay");

		if (Label[0]!=0) {
			// D3DXFont::DrawTextA becomes the client's own Sketchpad text; see
			// pOverlayFont in the header. DT_CENTER|DT_TOP becomes
			// SetTextAlign with the x at the centre of the same rectangle.
			RECT rect2 = _RECT(0, viewH - 60, viewW, viewH - 20);
			if (Sketchpad *pSkp = clbkGetSketchpad(GetBackBufferHandle())) {
				if (pOverlayFont) pSkp->SetFont(pOverlayFont);
				pSkp->SetTextAlign(Sketchpad::CENTER, Sketchpad::TOP);
				pSkp->SetTextColor(0x000000);
				pSkp->Text((rect2.left + rect2.right) / 2, rect2.top, Label, 6);
				rect2.left-=4; rect2.top-=4;
				pSkp->SetTextColor(0xFFFFFF);
				pSkp->Text((rect2.left + rect2.right) / 2, rect2.top, Label, 6);
				clbkReleaseSketchpad(pSkp);
			}
		}
	}

	if (bFreeze) {
		RECT rect2 = _RECT(0, viewH - 60, viewW, viewH - 20);
		if (Sketchpad *pSkp = clbkGetSketchpad(GetBackBufferHandle())) {
			if (pOverlayFont) pSkp->SetFont(pOverlayFont);
			pSkp->SetTextAlign(Sketchpad::CENTER, Sketchpad::TOP);
			// D3DCOLOR_XRGB(0,255,255) is 0x00FFFF, the same packing.
			pSkp->SetTextColor(0x00FFFF);
			pSkp->Text((rect2.left + rect2.right) / 2, rect2.top, "Frozen", 6);
			clbkReleaseSketchpad(pSkp);
		}
	}

	VulkanSetTime(VulkanStats.Timer.Scene, scene_time);


	if (bControlPanel) RenderControlPanel();

	// Compute total frame time
	VulkanSetTime(VulkanStats.Timer.FrameTotal, frame_time);
	frame_time = VulkanGetTime();

	// The host's buffer goes out of scope the moment this returns, so a draw
	// issued after it fails loudly rather than recording into a dangling
	// handle.
	pDevice->SetFrameCommandBuffer(VK_NULL_HANDLE, 0, 0);
}

// ==============================================================

void VulkanClient::clbkTimeJump(double simt, double simdt, double mjd)
{
	_TRACE;
	GraphicsClient::clbkTimeJump (simt, simdt, mjd);
}


// PresentScene has nothing to present. The counterpart of Present is
// vkQueuePresentKHR on an acquired swapchain image, and both belong to
// UIHost.cpp -- a second present would be a validation error and a hang. What
// is left is the timing and RenderWithPopupWindows, which is window
// management.
void VulkanClient::PresentScene()
{
	double time = VulkanGetTime();

	// The fullscreen/windowed branch collapses: the arms differed only in
	// whether Present was skipped, and there is no Present here.
	RenderWithPopupWindows();

	// This is where Present() went. orbiter_EndSceneFrame closes the scene frame
	// and calls the host's frame pump, which acquires an image, opens the pass,
	// calls this client's scene callback and presents. Omitting it left the
	// window blank.
	orbiter_EndSceneFrame();

	VulkanSetTime(VulkanStats.Timer.Display, time);
}

// ==============================================================

double framer_rater_limit = 0.0;

bool VulkanClient::clbkDisplayFrame()
{
	_TRACE;
//	static int iRefrState = 0;
	double time = VulkanGetTime();

	if (!bRunning && pDevice) {
		RECT txt = _RECT( loadd_x, loadd_y, loadd_x+loadd_w, loadd_y+loadd_h );
		// The destination cannot be blitted to: pBackBuffer is an attachment
		// proxy with no VkImage, and vkCmdBlitImage is illegal inside a render
		// pass anyway. So the splash goes through the client's own Sketchpad.
		if (Sketchpad *pSkp = clbkGetSketchpad(GetBackBufferHandle())) {
			if (pSplashScreen) {
				RECT full = _RECT(0, 0, viewW, viewH);
				pSkp->StretchRect(SURFHANDLE(pSplashScreen), NULL, &full);
			}
			if (pTextScreen) pSkp->StretchRect(SURFHANDLE(pTextScreen), NULL, &txt);
			clbkReleaseSketchpad(pSkp);
		}
	}

	// Was `if (Config->PresentLocation == 0) PresentScene();`.
	PresentScene();

	double frmt = (1000000.0/Config->FrameRate) - (time - framer_rater_limit);

	framer_rater_limit = time;

	if (Config->EnableLimiter && Config->FrameRate>0 && bVSync==false) {
		if (frmt>0) frame_timer++;
		else        frame_timer--;
		if (frame_timer>40) frame_timer=40;
		Sleep(frame_timer);
	}

	return true;
}


// SetDialogBoxMode(true) has no counterpart, here or in
// RenderWithPopupWindows. It let GDI draw over the front buffer of a D3D9
// device in EXCLUSIVE FULLSCREEN so Win32 dialogs could appear over the scene.
// There is no exclusive fullscreen to escape from, and dialogs are ImGui
// windows drawn inside the same frame.
void VulkanClient::clbkPreOpenPopup ()
{
	_TRACE;
}

static DWORD g_lastPopupWindowCount = 0;
static void FixOutOfScreenPositions (const HWND *hWnd, DWORD count)
{
	// Only check if a popup window is *added*
	if (count > g_lastPopupWindowCount)
	{
		for (DWORD i=0; i<count; ++i)
		{
			RECT rect;
			GetWindowRect(hWnd[i], &rect);

			int x = -1, y, w, h; // x != -1 indicates "position change needed"
			if (rect.left < 0) {
				x = 0;
				y = rect.top;
			}
			if (rect.top  < 0) {
				x = rect.left;
				y = 0;
			}

			// For the rest we need monitor information...
			HMONITOR monitor = MonitorFromWindow(hWnd[i], MONITOR_DEFAULTTONEAREST);
			MONITORINFO info;
			info.cbSize = sizeof(MONITORINFO);
			GetMonitorInfo(monitor, &info);

			int monitorWidth = info.rcMonitor.right - info.rcMonitor.left; // info.rcWork....
			int monitorHeight = info.rcMonitor.bottom - info.rcMonitor.top;

			if (rect.right > monitorWidth) {
				x = monitorWidth - (rect.right - rect.left);
				y = rect.top;
			}
			if (rect.bottom > monitorHeight) {
				x = rect.left;
				y = monitorHeight - (rect.bottom - rect.top);
			}

			if (x != -1) {
				w = rect.right - rect.left,
				h = rect.bottom - rect.top;
				MoveWindow(hWnd[i], x, y, w, h, FALSE);
			}
		}

	}
	g_lastPopupWindowCount = count;
}

// =======================================================================

bool VulkanClient::RenderWithPopupWindows()
{
	_TRACE;

	const HWND *hPopupWnd;
	DWORD count = GetPopupList(&hPopupWnd);

	// The fullscreen SetDialogBoxMode branch is gone; see clbkPreOpenPopup.

	FixOutOfScreenPositions(hPopupWnd, count);

	if (!bFullscreen) {
		for (DWORD i=0;i<count;i++) {
			DWORD val = GetWindowLongA(hPopupWnd[i], GWL_STYLE);
			if ((val&WS_SYSMENU)==0) {
				SetWindowLongA(hPopupWnd[i], GWL_STYLE, val|WS_SYSMENU);
			}
		}
	}

	return false;
}

// #pragma region / #pragma endregion are gone throughout: an MSVC editor
// feature GCC reports as an unknown pragma under -Wall.

// =======================================================================
// Particle stream functions
// =======================================================================

ParticleStream *VulkanClient::clbkCreateParticleStream(PARTICLESTREAMSPEC *pss)
{
	LogErr("UnImplemented Feature Used clbkCreateParticleStream");
	return NULL;
}

// =======================================================================

ParticleStream *VulkanClient::clbkCreateExhaustStream(PARTICLESTREAMSPEC *pss,
	OBJHANDLE hVessel, const double *lvl, const VECTOR3 *ref, const VECTOR3 *dir)
{
	_TRACE;
	ExhaustStream *es = new ExhaustStream (this, hVessel, lvl, ref, dir, pss);
	scene->AddParticleStream (es);
	return es;
}

// =======================================================================

ParticleStream *VulkanClient::clbkCreateExhaustStream(PARTICLESTREAMSPEC *pss,
	OBJHANDLE hVessel, const double *lvl, const VECTOR3 &ref, const VECTOR3 &dir)
{
	_TRACE;
	ExhaustStream *es = new ExhaustStream (this, hVessel, lvl, ref, dir, pss);
	scene->AddParticleStream (es);
	return es;
}

// ======================================================================

ParticleStream *VulkanClient::clbkCreateReentryStream (PARTICLESTREAMSPEC *pss,
	OBJHANDLE hVessel)
{
	_TRACE;
	ReentryStream *rs = new ReentryStream (this, hVessel, pss);
	scene->AddParticleStream (rs);
	return rs;
}

// ==============================================================

ScreenAnnotation* VulkanClient::clbkCreateAnnotation()
{
	_TRACE;
	return GraphicsClient::clbkCreateAnnotation();
}

// =======================================================================
// Mesh functions
// =======================================================================

void VulkanClient::clbkStoreMeshPersistent(MESHHANDLE hMesh, const char *fname)
{
	_TRACE;
	if (ChkDev(__FUNCTION__)) return;

	if (fname) {
		LogAlw("Storing a mesh %s (%s)", _PTR(hMesh), fname);
		if (hMesh==NULL) LogErr("VulkanClient::clbkStoreMeshPersistent(%s) hMesh is NULL",fname);
	}
	else {
		LogAlw("Storing a mesh %s", _PTR(hMesh));
		if (hMesh==NULL) LogErr("VulkanClient::clbkStoreMeshPersistent() hMesh is NULL");
	}

	if (hMesh==NULL) return;

	// The Windows line assigns the return to an `idx` that is never read.
	meshmgr->StoreMesh(hMesh, fname);
}

// ==============================================================

DEVMESHHANDLE VulkanClient::GetDevMesh(MESHHANDLE hMesh)
{
	const VulkanMesh *pDevMesh = meshmgr->GetMesh(hMesh);
	if (!pDevMesh) {
		meshmgr->StoreMesh(hMesh, "GetDevMesh()");
		pDevMesh = meshmgr->GetMesh(hMesh);
	}

	// Create a new Instance from a template
	return DEVMESHHANDLE(new VulkanMesh(hMesh, *pDevMesh));
}

// ==============================================================

bool VulkanClient::clbkSetMeshTexture(DEVMESHHANDLE hMesh, DWORD texidx, SURFHANDLE surf)
{
	_TRACE;
	// SURFACE(surf) is dropped: VulkanMesh::SetTexture takes a SURFHANDLE.
	if (hMesh && surf) return ((VulkanMesh*)hMesh)->SetTexture(texidx, surf);
	return false;
}


// The (const D3DMATERIAL9*) casts in the two functions below are gone: they
// reinterpreted the SDK's MATERIAL as a D3DMATERIAL9, two structs that
// happened to agree. CreateMatExt/GetMatExt now take MATERIAL directly.
int VulkanClient::clbkSetMeshMaterial(DEVMESHHANDLE hMesh, DWORD matidx, const MATERIAL *mat)
{
	_TRACE;
	if (!hMesh) return 3;
	VulkanMesh *mesh = (VulkanMesh*)hMesh;
	DWORD nmat = mesh->GetMaterialCount();
	if (matidx >= nmat) return 4; // "index out of range"
	VulkanMatExt meshmat;
	//mesh->GetMaterial(&meshmat, matidx);
	CreateMatExt(mat, &meshmat);
	mesh->SetMaterial(&meshmat, matidx);
	return 0;
}

// ==============================================================

int VulkanClient::clbkMeshMaterial (DEVMESHHANDLE hMesh, DWORD matidx, MATERIAL *mat)
{
	_TRACE;
	if (!hMesh) return 3;
	VulkanMesh *mesh = (VulkanMesh*)hMesh;
	DWORD nmat = mesh->GetMaterialCount();
	if (matidx >= nmat) return 4; // "index out of range"
	const VulkanMatExt *meshmat = mesh->GetMaterial(matidx);
	if (meshmat) GetMatExt(meshmat, mat);
	return 0;
}

// ==============================================================

int VulkanClient::clbkSetMeshMaterialEx(DEVMESHHANDLE hMesh, DWORD matidx, MatProp mat, const oapi::FVECTOR4* in)
{
	if (!hMesh) return 3;
	VulkanMesh* mesh = (VulkanMesh*)hMesh;
	return mesh->SetMaterialEx(matidx, mat, in);
}

// ==============================================================

int VulkanClient::clbkMeshMaterialEx(DEVMESHHANDLE hMesh, DWORD matidx, MatProp mat, oapi::FVECTOR4* out)
{
	if (!hMesh) return 3;
	VulkanMesh* mesh = (VulkanMesh*)hMesh;
	return mesh->GetMaterialEx(matidx, mat, out);
}

// ==============================================================

bool VulkanClient::clbkSetMeshProperty(DEVMESHHANDLE hMesh, DWORD prop, DWORD value)
{
	_TRACE;
	VulkanMesh *mesh = (VulkanMesh*)hMesh;
	switch (prop) {
		case MESHPROPERTY_MODULATEMATALPHA:
			mesh->EnableMatAlpha(value!=0);
			return true;
	}
	return false;
}

// ==============================================================
// Returns a dev-mesh for a visual

MESHHANDLE VulkanClient::clbkGetMesh(VISHANDLE vis, UINT idx)
{
	_TRACE;
	if (vis==NULL) {
		LogErr("NULL visual in clbkGetMesh(NULL,%u)",idx);
		return NULL;
	}
	MESHHANDLE hMesh = ((vObject*)vis)->GetMesh(idx);
	if (hMesh==NULL) LogWrn("clbkGetMesh() returns NULL");
	return hMesh;
}

// =======================================================================

int VulkanClient::clbkEditMeshGroup(DEVMESHHANDLE hMesh, DWORD grpidx, GROUPEDITSPEC *ges)
{
	_TRACE;
	return ((VulkanMesh*)hMesh)->EditGroup(grpidx, ges);
}

// =======================================================================


int VulkanClient::clbkGetMeshGroup (DEVMESHHANDLE hMesh, DWORD grpidx, GROUPREQUESTSPEC *grs)
{
	_TRACE;
	return ((VulkanMesh*)hMesh)->GetGroup (grpidx, grs);
}


// ==============================================================

void VulkanClient::clbkNewVessel(OBJHANDLE hVessel)
{
	_TRACE;
	if (scene) scene->NewVessel(hVessel);
}

// ==============================================================

void VulkanClient::clbkDeleteVessel(OBJHANDLE hVessel)
{
	if (scene) scene->DeleteVessel(hVessel);
}


// ==============================================================
// copy video options from the video tab

void VulkanClient::clbkRefreshVideoData()
{
	_TRACE;
	if (vtab) vtab->UpdateConfigData();
}

// ==============================================================

void VulkanClient::clbkOptionChanged(DWORD cat, DWORD item)
{
	switch (cat) {
	case OPTCAT_CELSPHERE:
		if (scene) scene->OnOptionChanged(cat, item);
		return;
	}
}

// ==============================================================

bool VulkanClient::clbkUseLaunchpadVideoTab() const
{
	_TRACE;
	return true;
}

// ==============================================================
// Fullscreen mode flag

bool VulkanClient::clbkFullscreenMode() const
{
	_TRACE;
	return bFullscreen;
}

// ==============================================================
// return the dimensions of the render viewport

void VulkanClient::clbkGetViewportSize(DWORD *width, DWORD *height) const
{
	_TRACE;
	*width = viewW, *height = viewH;
}

// RP_REQUIRETEXPOW2 stays 0, now a statement of fact: Vulkan requires full
// non-power-of-two support of every conforming implementation.

bool VulkanClient::clbkGetRenderParam(DWORD prm, DWORD *value) const
{
	_TRACE;
	switch (prm) {
		case RP_COLOURDEPTH:
			*value = viewBPP;
			return true;

		case RP_ZBUFFERDEPTH:
			*value = GetFramework()->GetZBufferBitDepth();
			return true;

		case RP_STENCILDEPTH:
			*value = GetFramework()->GetStencilBitDepth();
			return true;

		case RP_MAXLIGHTS:
			*value = MAX_SCENE_LIGHTS;
			return true;

		case RP_REQUIRETEXPOW2:
			*value = 0;
			return true;
	}
	return false;
}

// ==============================================================
// Responds to visual events

int VulkanClient::clbkVisEvent(OBJHANDLE hObj, VISHANDLE vis, DWORD msg, DWORD_PTR context)
{
	_TRACE;
	VisObject *vo = (VisObject*)vis;
	vo->clbkEvent(msg, context);
	if (DebugControls::IsActive()) {
		if (msg==EVENT_VESSEL_INSMESH || msg==EVENT_VESSEL_DELMESH) {
			if (DebugControls::GetVisual()==vo) DebugControls::UpdateVisual();
		}
	}
	return 1;
}


// ==============================================================
//
void VulkanClient::PickTerrain(DWORD uMsg, int xpos, int ypos)
{
	bool bUD = (uMsg == WM_LBUTTONUP || uMsg == WM_RBUTTONUP || uMsg == WM_LBUTTONDOWN || uMsg == WM_RBUTTONDOWN);
	bool bPrs = IsGenericProcEnabled(GENERICPROC_PICK_TERRAIN) && bUD;
	bool bHov = IsGenericProcEnabled(GENERICPROC_HOVER_TERRAIN) && (uMsg == WM_MOUSEMOVE || uMsg == WM_MOUSEWHEEL);

	if (bPrs || bHov) {
		gcCore::PickGround pg = gcCore2::ScanScreen(xpos, ypos);
		pg.msg = uMsg;
		if (bPrs) MakeGenericProcCall(GENERICPROC_PICK_TERRAIN, sizeof(gcCore::PickGround), &pg);
		if (bHov) MakeGenericProcCall(GENERICPROC_HOVER_TERRAIN, sizeof(gcCore::PickGround), &pg);
	}
}


// Entirely Win32. The two substitutions are D3D9Pick -> VulkanPick and, at one
// site, GetObjectA() -> Object(), GetObjectA having been a workaround for the
// ANSI/Unicode GetObject macro that the shim does not define.
LRESULT VulkanClient::RenderWndProc (HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	static bool bTrackMouse = false;
	static short xpos=0, ypos=0;

	VulkanPick pick;

	if (hRenderWnd!=hWnd && uMsg!= WM_NCDESTROY) {
		LogErr("Invalid Window !! RenderWndProc() called after calling clbkDestroyRenderWindow() uMsg=0x%X", uMsg);
		return 0;
	}

	if (bRunning && DebugControls::IsActive()) {
		// Must update camera to correspond MAIN_SCENE due to Pick() function,
		// because env-maps have altered camera settings
		// GetScene()->UpdateCameraFromOrbiter(RENDERPASS_PICKSCENE);
		// Obsolete: since moving env/cam stuff in pre-scene
	}

	if (pWM) if (pWM->MainWindowProc(hWnd, uMsg, wParam, lParam)) return 0;


	switch (uMsg)
	{
		case WM_MOUSELEAVE:
		{
			if (bTrackMouse && bRunning) GraphicsClient::RenderWndProc (hWnd, WM_LBUTTONUP, 0, 0);
			return 0;
		}

		case WM_MBUTTONDOWN:
		{
			break;
		}

		case WM_RBUTTONUP:
		case WM_RBUTTONDOWN:
		{
			int xp = GET_X_LPARAM(lParam);
			int yp = GET_Y_LPARAM(lParam);
			PickTerrain(uMsg, xp, yp);
			break;
		}


		case WM_LBUTTONDOWN:
		{
			bTrackMouse = true;
			xpos = GET_X_LPARAM(lParam);
			ypos = GET_Y_LPARAM(lParam);

			GetScene()->vPickRay = GetScene()->GetPickingRay(xpos, ypos);

			TRACKMOUSEEVENT te; te.cbSize = sizeof(TRACKMOUSEEVENT); te.dwFlags = TME_LEAVE; te.hwndTrack = hRenderWnd;
			TrackMouseEvent(&te);

			bool bShift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
			bool bCtrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
			bool bPckVsl = IsGenericProcEnabled(GENERICPROC_PICK_VESSEL);

			if (DebugControls::IsActive() || bPckVsl || (bShift && bCtrl)) {
				pick = GetScene()->PickScene(xpos, ypos);
				if (bPckVsl) {
					gcCore::PickData out;
					out.hVessel = pick.vObj->Object();
					out.mesh = MESHHANDLE(pick.pMesh);
					out.group = pick.group;
					out.pos = _FV(pick.pos);
					out.normal = _FV(pick.normal);
					out.dist = pick.dist;
					MakeGenericProcCall(GENERICPROC_PICK_VESSEL, sizeof(gcCore::PickData), &out);
				}
			}

			PickTerrain(uMsg, xpos, ypos);

			// No Debug Controls
			if (bShift && bCtrl && !DebugControls::IsActive() && !oapiCameraInternal()) {

				if (!pick.pMesh) break;

				OBJHANDLE hObj = pick.vObj->Object();
				if (oapiGetObjectType(hObj) == OBJTP_VESSEL) {
					oapiSetFocusObject(hObj);
				}

				break;
			}

			// With Debug Controls
			if (DebugControls::IsActive()) {

				DWORD flags = *(DWORD*)GetConfigParam(CFGPRM_GETDEBUGFLAGS);

				if (flags&DBG_FLAGS_PICK) {

					if (!pick.pMesh) break;

					if (bShift && bCtrl) {
						OBJHANDLE hObj = pick.vObj->Object();
						if (oapiGetObjectType(hObj)==OBJTP_VESSEL) {
							oapiSetFocusObject(hObj);
							break;
						}
					}
					else if (pick.group>=0) {
						DebugControls::SetVisual(pick.vObj);
						DebugControls::SelectMesh(pick.pMesh);
						DebugControls::SelectGroup(pick.group);
						DebugControls::SetGroupHighlight(true);
						DebugControls::SetPickPos(pick.pos);
					}
				}
			}

			break;
		}

		case WM_LBUTTONUP:
		{
			int xp = GET_X_LPARAM(lParam);
			int yp = GET_Y_LPARAM(lParam);

			PickTerrain(uMsg, xp, yp);

			if (DebugControls::IsActive()) {
				DWORD flags = *(DWORD*)GetConfigParam(CFGPRM_GETDEBUGFLAGS);
				if (flags&DBG_FLAGS_PICK) {
					DebugControls::SetGroupHighlight(false);
				}
			}
			bTrackMouse = false;
			break;
		}

		case WM_KEYDOWN:
		{
			bool bShift = (GetAsyncKeyState(VK_SHIFT) & 0x8000)!=0;
			bool bCtrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000)!=0;
			if (wParam == 'C' && bShift && bCtrl) bControlPanel = !bControlPanel;
			if (wParam == 'N' && bShift && bCtrl) Config->bCloudNormals = !Config->bCloudNormals;
			if (wParam == 'F' && bShift && bCtrl) {
				if (bFreeze) bFreezeEnable = bFreeze = false;
				else bFreezeEnable = true;
			}
			if (wParam == 'A' && bFreeze) bFreezeRenderAll = !bFreezeRenderAll;

			break;
		}

		case WM_MOUSEWHEEL:
		{
			if (DebugControls::IsActive()) {
				short d = GET_WHEEL_DELTA_WPARAM(wParam);
				if (d<-1) d=-1;
				if (d>1) d=1;
				double speed = *(double *)GetConfigParam(CFGPRM_GETCAMERASPEED);
				speed *= (DebugControls::GetVisualSize()/100.0);
				if (scene->CameraPan(_V(0,0,double(d))*2.0, speed)) return 0;
			}

			PickTerrain(uMsg, xpos, ypos);
			break;
		}

		case WM_MOUSEMOVE:

			if (DebugControls::IsActive())
			{

				double x = double(GET_X_LPARAM(lParam) - xpos);
				double y = double(GET_Y_LPARAM(lParam) - ypos);
				xpos = GET_X_LPARAM(lParam);
				ypos = GET_Y_LPARAM(lParam);

				if (bTrackMouse) {
					double speed = *(double *)GetConfigParam(CFGPRM_GETCAMERASPEED);
					speed *= (DebugControls::GetVisualSize() / 100.0);
					if (scene->CameraPan(_V(-x, y, 0)*0.05, speed)) return 0;
				}
			}

			xpos = GET_X_LPARAM(lParam);
			ypos = GET_Y_LPARAM(lParam);

			PickTerrain(uMsg, xpos, ypos);

			break;

		case WM_MOVE:
			// If in windowed mode, move the Framework's window
			break;

		case WM_SYSCOMMAND:
			switch (wParam) {
				case SC_KEYMENU:
					// trap Alt system keys
					return 1;
				case SC_MOVE:
				case SC_SIZE:
				case SC_MAXIMIZE:
				case SC_MONITORPOWER:
					// Prevent moving/sizing and power loss in fullscreen mode
					if (bFullscreen) return 1;
					break;
			}
			break;

		case WM_SYSKEYUP:
			if (bFullscreen) return 0;  // trap Alt-key
			break;
	}

	if (!bRunning && uMsg>=0x0200 && uMsg<=0x020E) return 0;
	return GraphicsClient::RenderWndProc (hWnd, uMsg, wParam, lParam);
}


// ==============================================================
// Message handler for Launchpad "video" tab

INT_PTR VulkanClient::LaunchpadVideoWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	_TRACE;
	if (vtab) return vtab->WndProc(hWnd, uMsg, wParam, lParam);
	else return false;
}

// D3DXMatrixOrthoOffCenterRH becomes VMAT_OrthoOffCenterRH; the matrix and its
// depth range are unchanged -- D3D9 clip space is 0 <= z <= w and so is
// Vulkan's.

void VulkanClient::clbkRender2DPanel (SURFHANDLE *hSurf, MESHHANDLE hMesh, MATRIX3 *T, float alpha, bool additive)
{
	_TRACE;

	SURFHANDLE surf = NULL;
	DWORD ngrp = oapiMeshGroupCount(hMesh);

	if (ngrp==0) return;

	float sx = 1.0f/(float)(T->m11),  dx = (float)(T->m13);
	float sy = 1.0f/(float)(T->m22),  dy = (float)(T->m23);
	float vw = (float)viewW;
	float vh = (float)viewH;

	FMATRIX4 mVP;
	VMAT_OrthoOffCenterRH(&mVP, (0.0f-dx)*sx, (vw-dx)*sx, (vh-dy)*sy, (0.0f-dy)*sy, -100.0f, 100.0f);
	VulkanEffect::SetViewProjMatrix(&mVP);

	for (DWORD i=0;i<ngrp;i++) {

		float scale = 1.0f;

		MESHGROUP *gr = oapiMeshGroup(hMesh, i);

		if (gr->UsrFlag & 2) continue; // skip this group

		DWORD TexIdx = gr->TexIdx;

		if (TexIdx >= TEXIDX_MFD0) {
			int mfdidx = TexIdx - TEXIDX_MFD0;
			surf = GetMFDSurface(mfdidx);
			if (!surf) surf = (SURFHANDLE)pDefaultTex;
		} else if (hSurf) {
			surf = hSurf[TexIdx];
		}
		else surf = oapiGetTextureHandle (hMesh, gr->TexIdx+1);

		for (unsigned int k=0;k<gr->nVtx;k++) gr->Vtx[k].z = 0.0f;

		VulkanEffect::Render2DPanel(gr, SURFACE(surf), &ident, alpha, scale, additive);
	}
}


// =======================================================================

void VulkanClient::clbkRender2DPanel (SURFHANDLE *hSurf, MESHHANDLE hMesh, MATRIX3 *T, bool additive)
{
	_TRACE;
	clbkRender2DPanel (hSurf, hMesh, T, 1.0f, additive);
}

// =======================================================================

DWORD VulkanClient::clbkGetDeviceColour (BYTE r, BYTE g, BYTE b)
{
	_TRACE;
	return ((DWORD)r << 16) + ((DWORD)g << 8) + (DWORD)b;
}


// =======================================================================
// Surface, Blitting and Filling Functions
// =======================================================================


// The five-call readback dance collapses into one call. A D3D9 default-pool
// surface cannot be locked, so the Windows body creates two temporaries,
// StretchRects, GetRenderTargetDatas and locks. Vulkan device-local memory
// cannot be mapped either, and the answer is ReadTexture -- which takes the
// host-visible case too, so the pool branch goes as well.
//
// One bug does not survive the collapse: the Windows sysmem branch ends
// `pSystem->UnlockRect()` on a pSystem never created on that path, a
// null-pointer call on every save from a system-memory surface.

bool VulkanClient::clbkSaveSurfaceToImage(SURFHANDLE surf, const char *fname, ImageFileFormat fmt, float quality)
{
	_TRACE;
	if (ChkDev(__FUNCTION__)) return false;

	// A NULL surface means the back buffer, and that one cannot be read the
	// same way: the reference resolves it to GetRenderTarget(0), a surface the
	// D3D9 client owns, while here it is an attachment proxy with no VkImage.
	// ReadTexture on it could only fail -- and did, once per session, which is
	// Images/CurrentState.jpg never being written. See
	// orbiter_CaptureBackBuffer.
	const bool bBackBuffer = (surf == NULL);
	if (bBackBuffer) surf = pFramework->GetBackBufferHandle();

	VulkanTexture *pSurf = SURFACE(surf)->GetSurface();

	if (pSurf==NULL) return false;

	bool bRet = false;

	if (fmt == ImageFileFormat::IMAGE_DDS) {
		char path[MAX_PATH];
		sprintf_s(path, "%s.dds", fname);
		// NatSaveSurface refuses DDS by name, where the request was made.
		return NatSaveSurface(path, pSurf);
	}

	uint32_t w = pSurf->Width();
	uint32_t h = pSurf->Height();

	std::vector<unsigned char> pix;

	if (bBackBuffer) {
		// The capture reports the size it actually took, which is the
		// swapchain's and not the proxy's -- they differ after a resize.
		int cw = 0, ch = 0;
		orbiter_GetSurfaceExtent(&cw, &ch);
		if (cw <= 0 || ch <= 0) { cw = int(w); ch = int(h); }
		pix.resize(size_t(cw) * size_t(ch) * 4);

		if (!orbiter_CaptureBackBuffer(pix.data(), (unsigned)pix.size(), &cw, &ch)) {
			LogErr("clbkSaveSurfaceToImage: back buffer capture failed");
			return false;
		}
		w = uint32_t(cw);
		h = uint32_t(ch);
		pix.resize(size_t(w) * size_t(h) * 4);
	}
	else {
		pix.resize(size_t(w) * 4 * h);
		if (!pDevice->ReadTexture(pSurf, 0, pix.data(), pix.size())) {
			LogErr("clbkSaveSurfaceToImage: readback failed");
			return false;
		}
	}

	const size_t pitch = size_t(w) * 4;

	if (fname == NULL) {
		// copy device-dependent bitmap to clipboard
		bRet = SaveSurfaceToClipboard(pSurf, pix.data(), pitch);
	} else {
		// save as file
		bRet = SaveSurfaceToFile(pSurf, pix.data(), pitch, w, h, fname, fmt, quality);
	}

	return bRet;
}


// The 32-bit-to-24-bit repack, unchanged. Width and height stay parameters as
// they were on Windows -- see the declaration for why the back-buffer path
// needs them to.
bool oapi::VulkanClient::SaveSurfaceToFile (const VulkanTexture* pTex, const void* pBits, size_t pitch,
                                            DWORD width, DWORD height,
                                            const char* fname, oapi::ImageFileFormat fmt, float quality)
{
	bool bRet = false;
	ImageData ID;

	ID.bpp = 24;
	ID.height = height;
	ID.width = width;
	ID.stride = ((ID.width * ID.bpp + 31) & ~31) >> 3;
	ID.bufsize = ID.stride * ID.height;

	BYTE* tgt = ID.data = new BYTE[ID.bufsize];
	const BYTE* src = (const BYTE*)pBits;

	for (DWORD k = 0; k<ID.height; k++) {
		for (DWORD i = 0; i<ID.width; i++) {
			tgt[0 + i * 3] = src[0 + i * 4];
			tgt[1 + i * 3] = src[1 + i * 4];
			tgt[2 + i * 3] = src[2 + i * 4];
		}
		tgt += ID.stride;
		src += pitch;
	}

	bRet = WriteImageDataToFile(ID, fname, fmt, quality);

	delete[]ID.data;
	ID.data = NULL;

	return bRet;
}

// Putting an image on the clipboard has no counterpart, and the refusal is by
// name rather than a silent false: the clipboard here is the desktop text
// selection. Worth recording that the Windows version does not do what its
// name says either -- it BitBlts from GetDC(hRenderWnd), the render window,
// not from the surface it was given.
bool oapi::VulkanClient::SaveSurfaceToClipboard (const VulkanTexture* pTex, const void* pBits, size_t pitch)
{
	LogErr("SaveSurfaceToClipboard: putting an image on the clipboard is not "
		   "supported -- the clipboard here carries text only. Save to a file "
		   "instead.");
	return false;
}



SURFHANDLE VulkanClient::clbkLoadTexture(const char *fname, DWORD flags)
{
	_TRACE;
	if (ChkDev(__FUNCTION__)) return NULL;

	DWORD attrib = OAPISURFACE_TEXTURE;
	if (flags & 0x1) attrib |= OAPISURFACE_SYSMEM;
	if (flags & 0x2) attrib |= OAPISURFACE_UNCOMPRESS | OAPISURFACE_RENDERTARGET;
	if (flags & 0x4) attrib |= OAPISURFACE_NOMIPMAPS;
	if (flags & 0x8) attrib |= OAPISURFACE_SHARED;

	return clbkLoadSurface(fname, attrib);
}

// ==============================================================

SURFHANDLE VulkanClient::clbkLoadSurface (const char *fname, DWORD attrib, bool bPath)
{
	_TRACE;
	if (ChkDev(__FUNCTION__)) return NULL;

	static const DWORD val = OAPISURFACE_RENDERTARGET | OAPISURFACE_GDI | OAPISURFACE_SYSMEM;

	if (!(attrib & val))
	{
		// It's a regular texture, let's manage it
		//
		string name(fname);

		if (attrib & OAPISURFACE_SHARED)
		{
			auto ent = SharedTextures.find(name);

			if (ent == SharedTextures.end())
			{
				SURFHANDLE hSrf = NatLoadSurface(fname, attrib, bPath);
				if (hSrf) SharedTextures[name] = hSrf;
				return hSrf;
			}
			else return ent->second;
		}

		// `static const DWORD exclude = ...` stood above, read only by the
		// block below, so it moves into the comment with the code it serves.
		/*
		auto ent = ClonedTextures.find(name);

		if (ent == ClonedTextures.end())
		{
			SURFHANDLE hSrf = NatLoadSurface(fname, attrib);
			if (hSrf) SharedTextures[name] = hSrf;
			return hSrf;
		}
		else
		{
			static const DWORD exclude = ~(OAPISURFACE_SHARED | OAPISURFACE_ORIGIN);
			DWORD original = SURFACE(ent->second)->GetOAPIFlags();

			if (original & OAPISURFACE_ORIGIN)
			{
				if ((attrib & exclude) == (original & exclude))
				{
					return SURFHANDLE(new SurfNative(SURFACE(ent->second))); // Clone it
				}
			}
			else
			{
				// Create "origin" for cloning
				SURFHANDLE hSrf = NatLoadSurface(fname, attrib | OAPISURFACE_ORIGIN);
				if (hSrf) {
					ent->second = hSrf;
					return SURFHANDLE(new SurfNative(SURFACE(hSrf))); // Clone it
				}
				else return NULL;
			}
		}
		*/
	}
	
	return NatLoadSurface(fname, attrib, bPath);
}

// The '\\' path separator becomes '/'. It is a path this function BUILDS rather
// than one it receives, so the separator is the platform's.
HBITMAP VulkanClient::gcReadImageFromFile(const char *_path)
{
	char path[MAX_PATH];
	sprintf_s(path, sizeof(path), "%s/%s", OapiExtension::GetTextureDir(), _path);
	return ReadImageFromFile(path);
}

// ==============================================================

void VulkanClient::clbkReleaseTexture(SURFHANDLE hTex)
{
	clbkReleaseSurface(hTex);
}


// ==============================================================

SURFHANDLE VulkanClient::clbkCreateSurfaceEx(DWORD w, DWORD h, DWORD attrib)
{
	_TRACE;
	if (ChkDev(__FUNCTION__)) return NULL;

#ifdef _DEBUG
	LogAttribs(attrib, w, h, "CreateSrfEx");
#endif // _DEBUG

	if (w == 0 || h == 0) return NULL;	// Inline engine returns NULL for a zero surface

	SURFHANDLE hNew = NatCreateSurface(w, h, attrib);
	SURFACE(hNew)->SetName("clbkCreateSurfaceEx");
	return hNew;
}


// =======================================================================

SURFHANDLE VulkanClient::clbkCreateSurface(DWORD w, DWORD h, SURFHANDLE hTemplate)
{
	_TRACE;
	if (ChkDev(__FUNCTION__)) return NULL;
	if (w == 0 || h == 0) return NULL;	// Inline engine returns NULL for a zero surface

	DWORD attrib = OAPISURFACE_PF_XRGB | OAPISURFACE_RENDERTARGET | OAPISURFACE_TEXTURE;

	if (hTemplate) attrib = SURFACE(hTemplate)->GetOAPIFlags();
	
	SURFHANDLE hNew = NatCreateSurface(w, h, attrib);
	SURFACE(hNew)->SetName("clbkCreateSurface");
	return hNew;
}

// =======================================================================

SURFHANDLE VulkanClient::clbkCreateSurface(HBITMAP hBmp)
{
	_TRACE;
	if (ChkDev(__FUNCTION__)) return NULL;

	SURFHANDLE hSurf = GraphicsClient::clbkCreateSurface(hBmp);
	SURFACE(hSurf)->SetName("clbkCreateSurface_HBITMAP");
	return hSurf;
}

// =======================================================================

SURFHANDLE VulkanClient::clbkCreateTexture(DWORD w, DWORD h)
{
	_TRACE;
	if (ChkDev(__FUNCTION__)) return NULL;
	if (w == 0 || h == 0) return NULL;	// Inline engine returns NULL for a zero surface

	SURFHANDLE hNew = NatCreateSurface(w, h, OAPISURFACE_PF_XRGB | OAPISURFACE_RENDERTARGET | OAPISURFACE_TEXTURE);
	SURFACE(hNew)->SetName("clbkCreateTexture");
	return hNew;
}

// =======================================================================

void VulkanClient::clbkIncrSurfaceRef(SURFHANDLE surf)
{
	_TRACE;
	if (surf) SURFACE(surf)->IncRef();
}

// SurfNative keeps its reference count, and that is not a leftover:
// IncRef/DecRef count how many times ORBITER has asked for a surface, which is
// the SDK's ownership model rather than Direct3D's. The image inside is
// counted too, and for a while it was not -- ~SurfNative destroyed it
// outright, so a texture an effect still had bound was freed under it.

bool VulkanClient::clbkReleaseSurface(SURFHANDLE surf)
{
	_TRACE;
	if (ChkDev(__FUNCTION__)) return false;

	// Do not release 'origin' (i.e. reference) for cloned surfaces.
	if (SURFACE(surf)->GetOAPIFlags() & OAPISURFACE_ORIGIN) return false;

	// Do not release surfaces stored in repository
	for (auto ent : SharedTextures) if (ent.second == surf) return false;

	// Don't release surfaces used by meshes
	for (auto mesh : MeshCatalog) if (mesh && mesh->HasTexture(surf)) return false;

	// If the surface exists, delete it.
	if (SURFACE(surf)->DecRef())
	{
		if (SurfaceCatalog.count(SURFACE(surf)))
		{
			delete SURFACE(surf);
			return true;
		}
	}
	return false;
}

// =======================================================================

bool VulkanClient::clbkGetSurfaceSize(SURFHANDLE surf, DWORD *w, DWORD *h)
{
	_TRACE;
	if (!w || !h) return false;
	if (surf==NULL) surf = pFramework->GetBackBufferHandle();
	*w = SURFACE(surf)->GetWidth();
	*h = SURFACE(surf)->GetHeight();
	return true;
}

// =======================================================================

bool VulkanClient::clbkSetSurfaceColourKey(SURFHANDLE surf, DWORD ckey)
{
	_TRACE;
	if (surf==NULL) { LogErr("Surface is NULL"); return false; }
	SURFACE(surf)->SetColorKey(ckey);
	return true;
}



// =======================================================================
// Blitting functions
// =======================================================================

int VulkanClient::clbkBeginBltGroup(SURFHANDLE tgt)
{
	_TRACE;
	if (pBltGrpTgt) return -1;

	if (tgt == RENDERTGT_NONE) {
		pBltGrpTgt = NULL;
		return -2;
	}

	if (tgt == RENDERTGT_MAINWINDOW) pBltGrpTgt = pFramework->GetBackBufferHandle();
	else pBltGrpTgt = tgt;

	if (!SURFACE(tgt)->IsRenderTarget()) {
		pBltGrpTgt = NULL;
		return -3;
	}

	//pBltSkp = SURFACE(tgt)->GetPooledSketchPad();
	//pBltSkp->BeginDrawing();
	return 0;
}

// =======================================================================

int VulkanClient::clbkEndBltGroup()
{
	_TRACE;
	if (pBltGrpTgt==NULL) return -2;
	//pBltSkp->EndDrawing();
	pBltSkp = NULL;
	pBltGrpTgt = NULL;
	return 0;
}

// =======================================================================

bool VulkanClient::clbkBlt(SURFHANDLE tgt, DWORD tgtx, DWORD tgty, SURFHANDLE src, DWORD flag) const
{
	_TRACE;
	const VulkanImageDesc* sd = SURFACE(src)->GetDesc();
	return clbkScaleBlt(tgt, tgtx, tgty, sd->Width, sd->Height, src, 0, 0, sd->Width, sd->Height, flag);
}

// =======================================================================

bool VulkanClient::clbkBlt(SURFHANDLE tgt, DWORD tgtx, DWORD tgty, SURFHANDLE src, DWORD srcx, DWORD srcy, DWORD w, DWORD h, DWORD flag) const
{
	_TRACE;
	return clbkScaleBlt(tgt, tgtx, tgty, w, h, src, srcx, srcy, w, h, flag);
}

// The decision tree is kept and its tests are re-expressed.
// D3DUSAGE_RENDERTARGET becomes VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, and
// D3DPOOL_SYSTEMMEM / D3DPOOL_DEFAULT become HostVisible -- the same question
// asked of the allocation rather than of a pool. StretchRect, UpdateSurface
// and GetRenderTargetData were three entry points only because a D3D9 copy was
// constrained by the pools at both ends; all three become BlitTexture, with
// the DESTINATION named first. The branches stay separate because one copies
// to a point and one sets OAPISURFACE_CAPTURE on the target.

// -----------------------------------------------------------------------------
// D3DUSAGE_AUTOGENMIPMAP's counterpart, and why it has to be a call. On
// Windows the flag is a standing instruction to the driver to regenerate the
// whole chain whenever level 0 is dirtied, which is why
// SurfNative::GenerateMipMaps() exists in the reference and is called from
// nowhere in it. Vulkan has no such flag and no driver-side generation.
//
// What it cost: the virtual-cockpit MFD screens were black. An MFD's `tex`
// carries OAPISURFACE_MIPMAPS in VC mode and Instrument::Update ends with
// clbkBlt into it, which wrote level 0 and left levels 1..9 of the 512x512
// chain undefined. A VC MFD panel is ~180 px on screen, so the sampler reads
// about level 1-2 -- never the level that had the picture in it.
static bool AutoGenMips(SURFHANDLE tgt)
{
	if (tgt && SURFACE(tgt)->GetMipMaps() > 1) SURFACE(tgt)->GenerateMipMaps();
	return true;
}

bool VulkanClient::clbkScaleBlt (SURFHANDLE tgt, DWORD tgtx, DWORD tgty, DWORD tgtw, DWORD tgth,
                                 SURFHANDLE src, DWORD srcx, DWORD srcy, DWORD srcw, DWORD srch, DWORD flag) const
{
	
	if (src==NULL) { oapiWriteLog((char*)"ERROR: oapiBlt() Source surface is NULL"); return false; }

	if (tgt==NULL) tgt = pFramework->GetBackBufferHandle();


	RECT rs = _RECT(srcx, srcy, srcx + srcw, srcy + srch);
	RECT rt = _RECT(tgtx, tgty, tgtx + tgtw, tgty + tgth);


	// Can't blit in a clone, declone..
	//
	if (SURFACE(tgt)->IsClone()) SURFACE(tgt)->DeClone();

	// Can't blit in a compressed surface, decompress..
	//
	if (!SURFACE(tgt)->Decompress())
	{
		HALT();
	}

	const VulkanImageDesc* td = SURFACE(tgt)->GetDesc();
	const VulkanImageDesc* sd = SURFACE(src)->GetDesc();

	// POINT tp becomes a destination RECTANGLE of the source's size at that
	// point, because BlitTexture takes rectangles at both ends. The branch
	// below is guarded by !bCL, so the two are the same size by construction.
	RECT tp = _RECT(tgtx, tgty, tgtx + srcw, tgty + srch);


	// Check failure and abort conditions, Match with know DX7 behavior ---------------------
	//
	if (rt.right > (long)td->Width || rt.bottom > (long)td->Height) return true;
	if (rt.left < 0 || rt.top < 0)  return true;

	if (rs.right > (long)sd->Width || rs.bottom > (long)sd->Height)  return true;
	if (rs.left < 0 || rs.top < 0) return true;

	if (rs.left > rs.right) return true;
	if (rt.left > rt.right) return true;
	if (rs.top > rs.bottom) return true;
	if (rt.top > rt.bottom) return true;

	if (srcw == 0 || srch == 0 || tgtw == 0 || tgth == 0) return true;


	// Check Blt conditions
	//
	bool bCK = (SURFACE(src)->GetColorKey() != SURF_NO_CK) && (SURFACE(src)->GetColorKey() != 0);	// ColorKey In Use
	bool bCL = (srcw != tgtw) || (srch != tgth);		// Scaling In Use
	bool bSC = SURFACE(src)->IsCompressed();			// Compressed source

	VulkanTexture *pss = SURFACE(src)->GetSurface();
	VulkanTexture *pts = SURFACE(tgt)->GetSurface();



	if ((sd->Format == td->Format) && !bCK && !bSC)
	{

		// Most common case: Target is a render-target and source is in a video memory
		//
		if ((td->Usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && (sd->HostVisible == false))
		{
			if (src != tgt)
			{
				if (pDevice->BlitTexture(pts, &rt, pss, &rs, false)) return AutoGenMips(tgt);

				LogErr("oapiBlt() BlitTexture() Failed 1");
				BltError(src, tgt, &rs, &rt);
				return false;
			}
			else
			{
				// Source and Target are the same surface, reroute through temp.
				//
				VulkanTexture *tmp = SURFACE(src)->GetTempSurface();

				if (pDevice->BlitTexture(tmp, &rs, pss, &rs, false))
				{
					if (pDevice->BlitTexture(pts, &rt, tmp, &rs, false)) return AutoGenMips(tgt);
				}

				LogErr("oapiBlt() BlitTexture() Failed 2");
				BltError(src, tgt, &rs, &rt);
				return false;
			}
		}
	}

	if ((sd->Format == td->Format) && !bCK && !bSC && !bCL)
	{

		// Texture Update: Source is in system memory and target is a texture
		// 
		if (sd->HostVisible)
		{
			if (pDevice->BlitTexture(pts, &tp, pss, &rs, false))	return AutoGenMips(tgt);

			LogErr("oapiBlt() BlitTexture() (was UpdateSurface) Failed");
			BltError(src, tgt, &rs, &rt);
			return false;
		}


		// GetRenderTargetData's signature is (pRenderTarget, pDestSurface) --
		// SOURCE first. BlitTexture names the destination first, so the two
		// pointers swap places while the copy keeps its direction.
		if ((td->HostVisible) && (sd->Usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
		{
			if (pDevice->BlitTexture(pts, &rt, pss, &rs, false)) {
				SURFACE(tgt)->Flags |= OAPISURFACE_CAPTURE;
				return true;
			}
		
			LogErr("oapiBlt() BlitTexture() (was GetRenderTargetData) Failed");
			BltError(src, tgt, &rs, &rt);
			return false;
		}
	}


	// The rare route, and it reports itself once. This is the only branch that
	// has to RENDER, the one a compressed source is forced onto, and the only
	// one with no failure reporting of its own -- it ends in AutoGenMips,
	// which returns true whatever happened. Seeing whether it was ENTERED
	// separates "rendered nothing" from "never issued".
	{
		static bool bSaid = false;
		if (!bSaid && (bSC || bCK || bCL || sd->Format != td->Format)) {
			bSaid = true;
			LogErr("oapiBlt(): first Sketchpad-fallback copy -- src %ux%u "
				   "(compressed=%d colorkey=%d) -> tgt %ux%u, scaling=%d, "
				   "format match=%d",
				   sd->Width, sd->Height, int(bSC), int(bCK),
				   td->Width, td->Height, int(bCL),
				   int(sd->Format == td->Format));
		}
	}

	if (src != tgt)
	{
		if ((td->Usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && SURFACE(src)->IsTexture() && (sd->HostVisible == false))
		{
			Sketchpad* pSkp = clbkGetSketchpad_const(tgt);

			if (bCK)
			{
				if (bCL) ((VulkanPad*)pSkp)->ColorKeyStretch(src, &rs, &rt);
				else pSkp->ColorKey(src, &rs, tgtx, tgty);
				clbkReleaseSketchpad_const(pSkp);
				return AutoGenMips(tgt);
			}
			else
			{		
				pSkp->StretchRect(src, &rs, &rt);
				clbkReleaseSketchpad_const(pSkp);
				AutoGenMips(tgt);

				// Ground truth for the black registration panel: every layer
				// here reports success and the result is black, so
				// ORBITER_VK_DUMPBLT=1 writes source and target to /tmp once.
				{
					static const bool bDump = (getenv("ORBITER_VK_DUMPBLT") != NULL);
					static bool bDone = false;
					if (bDump && !bDone) {
						bDone = true;
						NatSaveSurface("/tmp/blt_src.png", SURFACE(src)->GetTexture());
						NatSaveSurface("/tmp/blt_tgt.png", SURFACE(tgt)->GetTexture());
						LogErr("DUMPBLT: wrote /tmp/blt_src.png and /tmp/blt_tgt.png");
					}
				}
				return true;
			}
		}
	}

	LogErr("oapiBlt() Failed (End)");
	BltError(src, tgt, &rs, &rt);
	return false;
}


// The GDI-cache branch has nothing left to work around: it borrowed a cache
// texture because IDirect3DSurface9::GetDC() does not work on a render target,
// and SurfNative::GetDC() returns a recording DC for any surface. What that
// costs, stated plainly: a recording DC does not rasterise into the surface's
// pixels, so a StretchBlt through it puts nothing into the image.
bool VulkanClient::clbkCopyBitmap(SURFHANDLE pdds, HBITMAP hbm, int x, int y, int dx, int dy)
{
	HDC                     hdcImage;
	HDC                     hdc;
	BITMAP                  bm;

	if (hbm == NULL || pdds == NULL) return false;

	// Select bitmap into a memoryDC so we can use it.
	//
	hdcImage = CreateCompatibleDC(NULL);

	if (!hdcImage) OutputDebugString("createcompatible dc failed\n");

	SelectObject(hdcImage, hbm);

	// Get size of the bitmap
	//
	GetObject(hbm, sizeof(bm), &bm);
	dx = dx == 0 ? bm.bmWidth : dx;     // Use the passed size, unless zero
	dy = dy == 0 ? bm.bmHeight : dy;


	// Get size of surface.
	//
	DWORD surfW = SURFACE(pdds)->GetWidth();
	DWORD surfH = SURFACE(pdds)->GetHeight();

	if ((hdc = clbkGetSurfaceDC(pdds)) != NULL) {
		StretchBlt(hdc, 0, 0, surfW, surfH, hdcImage, x, y,	dx, dy, SRCCOPY);
		clbkReleaseSurfaceDC(pdds, hdc);
		DeleteDC(hdcImage);
		SURFACE(pdds)->SetName("clbkCopyBitmap");
		return true;
	}

	DeleteDC(hdcImage);
	return false;
}

// =======================================================================

bool VulkanClient::clbkFillSurface(SURFHANDLE tgt, DWORD col) const
{
	_TRACE;
	if (tgt==NULL) tgt = pFramework->GetBackBufferHandle();
	bool ret = SURFACE(tgt)->Fill(NULL, col);
	return ret;
}

// =======================================================================

bool VulkanClient::clbkFillSurface(SURFHANDLE tgt, DWORD tgtx, DWORD tgty, DWORD w, DWORD h, DWORD col) const
{
	_TRACE;
	if (tgt==NULL) tgt = pFramework->GetBackBufferHandle();
	RECT r = _RECT(tgtx, tgty, tgtx+w, tgty+h);
	bool ret = SURFACE(tgt)->Fill(&r, col);
	return ret;
}

// =======================================================================
// (long) on the four abs() arguments: RECT's fields are LONG, which is 64-bit
// on Linux, so abs() picks the long overload and %u would print half of it.
// The subtraction is cast to what the format string says.
//
void VulkanClient::BltError(SURFHANDLE src, SURFHANDLE tgt, const LPRECT s, const LPRECT t, bool bHalt) const
{
	LogErr("Source Rect (%d,%d,%d,%d) (w=%u,h=%u)", (int)s->left, (int)s->top, (int)s->right, (int)s->bottom,
		(unsigned)abs(int(s->left - s->right)), (unsigned)abs(int(s->top - s->bottom)));
	LogErr("Target Rect (%d,%d,%d,%d) (w=%u,h=%u)", (int)t->left, (int)t->top, (int)t->right, (int)t->bottom,
		(unsigned)abs(int(t->left - t->right)), (unsigned)abs(int(t->top - t->bottom)));
	// A diagnostic that segfaults is worse than no diagnostic, and this one
	// did: LogSpecs() hands pResource to NatDumpResource, which dereferences
	// it, so given a dangling handle the crash lands inside the error reporter
	// and the failure it was called to explain is never written.
	//
	// Measured: the DG's virtual-cockpit coolant readout blitted an 8x11 glyph
	// and fell through to here. The target was healthy; the source's `name`
	// read back as little-endian 16-bit 8,10,10,12,12,14 then 0,1,2, 1,3,2,
	// 2,3,4 -- an INDEX BUFFER. The surface had been freed and its memory
	// reused for mesh indices.
	//
	// SurfaceCatalog is the reference's own idiom for this question. The
	// dangling handle is a real defect and is not fixed by this; what is fixed
	// is that it names itself instead of killing the process.
	LogErr("Source Data Below: ----------------------------------");
	if (src && SurfaceCatalog.count(SURFACE(src)))
		SURFACE(src)->LogSpecs();
	else
		LogErr("SOURCE IS NOT A LIVE SURFACE. Handle=%s is not in the surface "
			   "catalog -- it was released while a caller still held it, or it "
			   "was never a surface. Nothing below it can be trusted and it is "
			   "not being dereferenced.", _PTR(src));

	LogErr("Target Data Below: ----------------------------------");
	if (tgt && SurfaceCatalog.count(SURFACE(tgt)))
		SURFACE(tgt)->LogSpecs();
	else
		LogErr("TARGET IS NOT A LIVE SURFACE. Handle=%s is not in the surface "
			   "catalog.", _PTR(tgt));

	if (bHalt) HALT();
}



// The NULL-surface branch loses its image. On Windows a NULL surface with
// GDIOverlay enabled meant "give me a DC on the scene's GDI overlay buffer",
// and the first caller of the frame cleared it through the same DC. Neither
// half survives: a DC is not tied to an image here, and the clear has moved
// into Scene's constructor. A module that draws an overlay still gets a valid
// DC, but its commands go onto the display list rather than into GBUF_GDI.

HDC VulkanClient::clbkGetSurfaceDC(SURFHANDLE surf)
{
	_TRACE;
	if (ChkDev(__FUNCTION__)) return NULL;

	if (surf == NULL) {
		if (Config->GDIOverlay) return CreateCompatibleDC(NULL);
		return NULL;
	}
	HDC hDC = SURFACE(surf)->GetDC();
	return hDC;
}

// =======================================================================

void VulkanClient::clbkReleaseSurfaceDC(SURFHANDLE surf, HDC hDC)
{
	_TRACE;
	if (ChkDev(__FUNCTION__)) return;

	if (hDC == NULL) { LogErr("VulkanClient::clbkReleaseSurfaceDC() Input hDC is NULL"); return; }
	if (surf == NULL) {
		if (Config->GDIOverlay) DeleteDC(hDC);
		return;
	}
	SURFACE(surf)->ReleaseDC(hDC);
}

// =======================================================================

bool VulkanClient::clbkFilterElevation(OBJHANDLE hPlanet, int ilat, int ilng, int lvl, double elev_res, INT16* elev)
{
	_TRACE;
	return FilterElevationPhysics(hPlanet, lvl, ilat, ilng, elev_res, elev);
}

// The three ImGui callbacks belong to the core now; this is note 3. UIHost.cpp
// runs its own backends around the same frame that calls this client's scene
// callback, so a second NewFrame or Render from here would be an assertion
// inside ImGui. What the client still owns is releasing the surfaces it
// protected for ImGui's use during the frame.

void VulkanClient::clbkImGuiNewFrame()
{
	_TRACE;
}

void VulkanClient::clbkImGuiRenderDrawData()
{
	_TRACE;

	// Release textures that were protected for ImGui usage during the frame
	for(auto &surf: ImTextures) {
		clbkReleaseSurface(surf);
	}
	ImTextures.clear();
}


void VulkanClient::clbkImGuiInit()
{
	_TRACE;
	// ImGui_ImplDX9_Init(pDevice) has no counterpart; see clbkImGuiNewFrame.
}

void VulkanClient::clbkImGuiShutdown()
{
	_TRACE;
	// Clean up also here just in case
	for(auto &surf: ImTextures) {
		clbkReleaseSurface(surf);
	}
	ImTextures.clear();
	// ImGui_ImplDX9_Shutdown() goes with the Init above.
}

// An ImGui texture id is a descriptor set here, not a texture pointer.
// imgui_impl_vulkan takes a VkDescriptorSet, which has to be ALLOCATED from
// the backend's own pool, so the client cannot mint one. UIHost.cpp exports
// orbiter_ImGuiTextureFromView and caches by image view, because
// ImGui_ImplVulkan_AddTexture allocates a set per call.

extern "C" unsigned long long orbiter_ImGuiTextureFromView(void *imageView);

uint64_t VulkanClient::clbkImGuiSurfaceTexture(SURFHANDLE surf)
{
	ImTextures.push_back(surf);
	clbkIncrSurfaceRef(surf);
	VulkanTexture *pTxt = SURFACE(surf)->GetTexture();
	if (!pTxt) return 0;
	return (uint64_t)orbiter_ImGuiTextureFromView((void*)pTxt->View());
}

// =======================================================================

bool VulkanClient::clbkSplashLoadMsg (const char *msg, int line)
{
	_TRACE;
	return OutputLoadStatus (msg, line);
}

// =======================================================================

lpSurfNative VulkanClient::GetDefaultTexture() const
{
	return pDefaultTex;
}

// =======================================================================

HWND VulkanClient::GetWindow()
{
	return pFramework->GetRenderWindow();
}

// =======================================================================

SURFHANDLE VulkanClient::GetBackBufferHandle() const
{
	_TRACE;
	return pFramework->GetBackBufferHandle();
}

// =======================================================================
// LPD3DXMATRIX becomes const FMATRIX4*; see the note on the declaration in
// VulkanClient.h for why the constness changes with it.

void VulkanClient::MakeRenderProcCall(Sketchpad *pSkp, DWORD id, const FMATRIX4 *pV, const FMATRIX4 *pP)
{
	for (auto it = RenderProcs.cbegin(); it != RenderProcs.cend(); ++it) {
		if (it->id == id) {
			VulkanPad *pSkp2 = (VulkanPad *)pSkp;
			pSkp2->LoadDefaults();
			if (id == RENDERPROC_EXTERIOR || id == RENDERPROC_PLANETARIUM) {
				pSkp2->SetViewMode(Sketchpad::USER);
			}
			pSkp2->SetViewProj(pV, pP);
			it->proc(pSkp, it->pParam);
			pSkp2->FlushAll(); // Flush render queue
		}
	}
}

// =======================================================================

void VulkanClient::MakeGenericProcCall(DWORD id, int iUser, void *pUser) const
{
	for (auto it = GenericProcs.cbegin(); it != GenericProcs.cend(); ++it) {
		if (it->id == id) it->proc(iUser, pUser, it->pParam);
	}
}


// =======================================================================

bool VulkanClient::RegisterRenderProc(__gcRenderProc proc, DWORD id, void *pParam)
{
	if (id)	{ // register (add)
		RenderProcData data = { proc, pParam, id };
		RenderProcs.push_back(data);
		return true;
	}
	else { // unregister, mark as unused (remove later)
		for (auto it = RenderProcs.begin(); it != RenderProcs.end(); ++it) {
			if (it->proc == proc) {
				it->id = 0;
				it->pParam = NULL;
				it->proc = NULL;
				return true;
			}
		}
	}
	return false;
}

// =======================================================================

bool VulkanClient::RegisterGenericProc(__gcGenericProc proc, DWORD id, void *pParam)
{
	if (id) { // register (add)
		GenericProcData data = { proc, pParam, id };
		GenericProcs.push_back(data);
		return true;
	}
	else { // unregister, mark as unused (remove later)
		for (auto it = GenericProcs.begin(); it != GenericProcs.end(); ++it) {
			if (it->proc == proc) {
				it->id = 0;
				it->pParam = NULL;
				it->proc = NULL;
				return true;
			}
		}
	}
	return false;
}

// =======================================================================

bool VulkanClient::IsGenericProcEnabled(DWORD id) const
{
	for (const auto &val : GenericProcs) if (val.id == id) return true;
	return false;
}

// =======================================================================

void VulkanClient::WriteLog(const char *msg) const
{
	_TRACE;
	char cbuf[256];
	sprintf_s(cbuf, 256, "Vulkan: %s", msg);
	oapiWriteLog(cbuf);
}


// The splash screen is where the conversion hurts most, so what survives and
// what does not is stated here rather than three times below.
//
// The Windows pair creates two offscreen plain surfaces, decodes the splash
// image into the larger, writes text onto them with GDI TextOut, and
// StretchRects both onto the back buffer and Present()s. Three of those steps
// have no counterpart: the GDI text does not land in the image (Gdi.cpp is a
// display-list recorder, so the TextOut calls are kept and reach the core's
// overlay when it replays the list, but they write no pixels); the blit cannot
// happen, pBackBuffer being an attachment proxy; and the present cannot
// happen, UIHost.cpp having the only one. The core already draws a splash
// screen, so these two functions keep the state they are asked to keep and
// stop where the display belongs to someone else.

bool VulkanClient::OutputLoadStatus(const char *txt, int line)
{

	if (bRunning) return false;

	if (line == 1) strcpy_s(pLoadItem, 127, txt); else
	if (line == 0) strcpy_s(pLoadLabel, 127, txt), pLoadItem[0] = '\0'; // New top line => clear 2nd line

	if (pTextScreen) {

		// TestCooperativeLevel() has no counterpart; see clbkRenderScene.

		RECT txtr = _RECT( loadd_x, loadd_y, loadd_x+loadd_w, loadd_y+loadd_h );

		// StretchRect(pSplashScreen, &txt, pTextScreen, NULL, POINT) --
		// destination first here, and the whole of pTextScreen is the target.
		pDevice->BlitTexture(pTextScreen->GetSurface(), NULL,
							 pSplashScreen->GetSurface(), &txtr, false);

		HDC hDC = pTextScreen->GetDC();
		if (hDC) {
			HFONT hO = (HFONT)SelectObject(hDC, hLblFont1);
			SetTextColor(hDC, pSplashTextColor);
			SetBkMode(hDC,TRANSPARENT);
			SetTextAlign(hDC, TA_LEFT|TA_TOP);

			TextOut(hDC, 2, 2, pLoadLabel, lstrlen(pLoadLabel));

			SelectObject(hDC, hLblFont2);
			TextOut(hDC, 2, 36, pLoadItem, lstrlen(pLoadItem));

			HPEN pen = CreatePen(PS_SOLID,1,pSplashTextColor);
			HPEN po = (HPEN)SelectObject(hDC, pen);

			MoveToEx(hDC, 0, 32, NULL);
			LineTo(hDC, loadd_w, 32);

			SelectObject(hDC, po);
			SelectObject(hDC, hO);
			DeleteObject(pen);

			pTextScreen->ReleaseDC(hDC);
		}

		// The two StretchRects and the Present go with the back buffer that has
		// no image; see the note above.

		// Prevent "Not Responding" during loading
		MSG msg;
		while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessage(&msg);
	}
	return false;
}

// =======================================================================
void VulkanClient::clbkSetSplashScreen(const char *filename, DWORD textCol)
{
	pCustomSplashScreen = filename;
	pSplashTextColor = textCol;
}


void VulkanClient::SplashScreen()
{

	loadd_x = 279*viewW/1280;
	loadd_y = 545*viewH/800;
	loadd_w = viewW/3;
	loadd_h = 80;

	RECT rS;

	GetWindowRect(hRenderWnd, &rS);

	LogAlw("Splash Window Size = [%u, %u]", rS.right - rS.left, rS.bottom - rS.top);
	LogAlw("Splash Window LeftTop = [%d, %d]", (int)rS.left, (int)rS.top);

	// TestCooperativeLevel() and Clear() have no counterpart: there is no lost
	// device, and no frame open to clear -- this runs before the first one.

	// CreateOffscreenPlainSurface becomes NatCreateSurface with the flags that
	// say the same thing. The "offscreen plain" TYPE has no counterpart.
	const DWORD splashFlags = OAPISURFACE_PF_XRGB | OAPISURFACE_SYSMEM | OAPISURFACE_GDI;

	pTextScreen   = SURFACE(NatCreateSurface(loadd_w, loadd_h, splashFlags));
	pSplashScreen = SURFACE(NatCreateSurface(viewW, viewH, splashFlags));

	if (!pTextScreen || !pSplashScreen) {
		LogErr("SplashScreen: could not create the splash surfaces");
		return;
	}
	pTextScreen->SetName("SplashText");
	pSplashScreen->SetName("SplashScreen");

	// D3DXGetImageInfoFromFile plus D3DXLoadSurfaceFromFile becomes one decode
	// plus one blit; the fit arithmetic is unchanged.
	VulkanTexture *pImg = NULL;
	double imageW = 0.0, imageH = 0.0;

	if(pCustomSplashScreen != NULL) {
		uint32_t iw = 0, ih = 0;
		pImg = NatLoadTexture(pCustomSplashScreen);
		if (pImg) { iw = pImg->Width(); ih = pImg->Height(); }
		imageW = double(iw ? iw : 1);
		imageH = double(ih ? ih : 1);
	}
	else {
		HMODULE hOrbiter =  GetModuleHandleA("orbiter.exe");
		HRSRC hRes = FindResourceA(hOrbiter, MAKEINTRESOURCEA(292), "IMAGE");
		HGLOBAL hImage = LoadResource(hOrbiter, hRes);
		LPVOID pData = LockResource(hImage);
		DWORD size = SizeofResource(hOrbiter, hRes);

		// D3DXLoadSurfaceFromFileInMemory becomes NatCreateTextureFromMemory.
		pImg = NatCreateTextureFromMemory(pData, size);
		imageW = 1920.0;
		imageH = 1200.0;
	}

	double scale = min(viewW / imageW, viewH / imageH);
	double _w = (imageW * scale);
	double _h = (imageH * scale);
	double _l = abs(viewW - _w)/2.0;
	double _t = abs(viewH - _h)/2.0;
	RECT imgRect = {
		static_cast<LONG>( round(_l) ),
		static_cast<LONG>( round(_t) ),
		static_cast<LONG>( round(_w + _l) ),
		static_cast<LONG>( round(_h + _t) )
	};

	// ColorFill(pSplashScreen, NULL, black) becomes SurfNative::Fill; a
	// whole-image fill and a sub-rectangle fill are two different calls.
	pSplashScreen->Fill(NULL, 0);

	if (pImg) {
		pDevice->BlitTexture(pSplashScreen->GetSurface(), &imgRect, pImg, NULL, true);
		pDevice->DestroyTexture(pImg);
		pImg = NULL;
	}
	else LogErr("SplashScreen: could not decode the splash image");


	HDC hDC = pSplashScreen->GetDC();
	if (hDC) {

		LOGFONTA fnt; memset((void *)&fnt, 0, sizeof(LOGFONT));

		fnt.lfHeight		 = 18;
		fnt.lfWeight		 = 700;
		fnt.lfCharSet		 = ANSI_CHARSET;
		fnt.lfOutPrecision	 = OUT_DEFAULT_PRECIS;
		fnt.lfClipPrecision	 = CLIP_DEFAULT_PRECIS;
		fnt.lfQuality		 = ANTIALIASED_QUALITY;
		fnt.lfPitchAndFamily = DEFAULT_PITCH;
		strcpy_s(fnt.lfFaceName, "Courier New");

		HFONT hF = CreateFontIndirect(&fnt);

		HFONT hO = (HFONT)SelectObject(hDC, hF);
		SetTextColor(hDC, pSplashTextColor);
		SetBkMode(hDC,TRANSPARENT);

		const char *months[]={"???","Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec","???"};

		DWORD d = oapiGetOrbiterVersion();
		DWORD y = d/10000; d-=y*10000;
		DWORD m = d/100; d-=m*100;
		if (m>12) m=0;

		char dataA[256];
		// "D3D9Client" becomes "VulkanClient"; the "via D3D9on12 emulator"
		// suffix goes with the Enable9On12 setting, which has no counterpart.
		strcpy(dataA, "VulkanClient");

#ifdef _DEBUG
		strcat_s(dataA, 256, " (Debug Build)");
#else
		strcat_s(dataA, 256, " (Release Build)");
#endif

		char dataB[128]; sprintf_s(dataB,128,"Build %s %lu 20%lu [%u]", months[m], (unsigned long)d, (unsigned long)y, oapiGetOrbiterVersion());
		//char dataE[] = { "Note: Cubic Interpolation is use... Consider using linear for better elevation matching" };
		//char dataF[] = { "Note: Terrain flattening offline due to cubic interpolation" };

		int xc = viewW*750/1280;
		int yc = viewH*545/800;

		TextOut(hDC, xc, yc + 0*20, "ORBITER Space Flight Simulator",30);
		TextOut(hDC, xc, yc + 1*20, dataB, lstrlen(dataB));
		TextOut(hDC, xc, yc + 2*20, dataA, lstrlen(dataA));


		SelectObject(hDC, hO);
		DeleteObject(hF);

		pSplashScreen->ReleaseDC(hDC);
	}

	// The two StretchRects and the Present that ended the Windows function are
	// gone with the back buffer that has no image. What is produced here is
	// what clbkDisplayFrame draws through the Sketchpad while !bRunning.
}


// BeginScene/EndScene have no device call behind them any more: the Vulkan
// equivalent is being inside a render pass on a recording command buffer, and
// the core has already begun both. bRendering is the part that matters and it
// stays -- IsInScene() is read by the Sketchpad and the surface code to decide
// whether a draw is legal right now.

HRESULT VulkanClient::BeginScene()
{
	bRendering = true;
	return S_OK;
}

// =======================================================================

void VulkanClient::EndScene()
{
	bRendering = false;
}

// =======================================================================
// Drawing (Sketchpad) Interface
// =======================================================================


double sketching_time;

// The thread guard is the reference's, unchanged: GetCurrentThread() returns
// the same pseudo-handle on every thread, so it cannot fire. Carried rather
// than repaired -- see VulkanClient::GetMainThread().
oapi::Sketchpad *VulkanClient::clbkGetSketchpad_const(SURFHANDLE surf) const
{
	if (ChkDev(__FUNCTION__)) return NULL;

	if (GetCurrentThread() != hMainThread) {
		LogErr("Sketchpad called from a worker thread !");
		HALT();
	}

	if (surf == RENDERTGT_MAINWINDOW) surf = GetBackBufferHandle();

	if (SURFACE(surf)->IsRenderTarget())
	{
		// Get Pooled Sketchpad
		VulkanPad *pPad = SURFACE(surf)->GetPooledSketchPad();

		// Get Current interface if any
		VulkanPad *pCur = GetTopInterface();

		// Do we have an existing SketchPad interface in use
		if (pCur) {
			if (pCur == pPad) {
				LogErr("Sketchpad already exists for this surface");
				HALT();
			}
			pCur->EndDrawing();	// Put the current one in hold
			LogDbg("Red", "Switching to another sketchpad in a middle");
		}

		// Push a new Sketchpad onto a stack
		PushSketchpad(surf, pPad);

		pPad->BeginDrawing();
		pPad->LoadDefaults();

		return pPad;
	}
	else {
		HDC hDC = SURFACE(surf)->GetDC();
		if (hDC) return new GDIPad(surf, hDC);
	}

	return NULL;
}

// =======================================================================
// 2D Drawing Interface
//
oapi::Sketchpad* VulkanClient::clbkGetSketchpad(SURFHANDLE surf)
{
	return clbkGetSketchpad_const(surf);
}

// =======================================================================

void VulkanClient::clbkReleaseSketchpad_const(oapi::Sketchpad* sp) const
{
	_TRACE;
	if (ChkDev(__FUNCTION__)) return;

	if (!sp) return;

	SURFHANDLE hSrf = sp->GetSurface();

	if (SURFACE(hSrf)->IsRenderTarget()) {

		VulkanPad* pPad = ((VulkanPad*)sp);

		if (GetTopInterface() != pPad) {
			LogErr("Sketchpad release failed. Not a top one.");
			HALT();
		}

		pPad->EndDrawing();

		PopRenderTargets();

		// The pad has just dirtied level 0, the other moment
		// D3DUSAGE_AUTOGENMIPMAP fires on Windows. A no-op for every surface
		// the core currently draws on with a pad, but the flag did not
		// distinguish how level 0 got dirty and neither should this.
		AutoGenMips(hSrf);

		// What the pad actually left in the VC HUD surface. After
		// PopRenderTargets: EndDrawing only flushes the pad's geometry into
		// the offscreen buffer, and it is PopRenderTargets that submits it.
		{
			static const bool bDumpSkp = (getenv("ORBITER_VK_DUMP_SKP") != NULL);
			if (bDumpSkp) {
				const VCHUDSPEC *spec = NULL;
				SURFHANDLE hHud = GetVCHUDSurface(&spec);
				// The VC MFDs take the same path, so dumping one alongside
				// says whether the path is broken or only the HUD's content.
				SURFHANDLE hMfd = GetMFDSurface(0);
				if (hMfd == hSrf) {
					static int m = 0;
					if (++m == 40) {
						VulkanTexture *pM = SURFACE(hSrf)->GetSurface();
						std::vector<unsigned char> px(size_t(pM->Width()) * pM->Height() * 4);
						size_t nLit = 0;
						if (pDevice->ReadTexture(pM, 0, px.data(), px.size()))
							for (size_t i = 0; i < px.size(); i += 4)
								if (px[i] | px[i+1] | px[i+2]) nLit++;
						LogErr("SKPDUMP VC MFD0 %ux%u flushedVtx=%u: %zu of %zu pixels have colour",
							   pM->Width(), pM->Height(), pPad->nFlushedVtx,
							   nLit, px.size() / 4);
						NatSaveSurface("vcmfd0.jpg", pM);
					}
				}
				if (hHud == hSrf) {
					static int n = 0;
					if (++n == 40) {
						VulkanTexture *pT = SURFACE(hSrf)->GetSurface();
						LogErr("SKPDUMP VC HUD surface %ux%u flushedVtx=%u -> vchud.png "
							   "(spec size %f, centre %f %f %f)",
							   pT ? pT->Width() : 0, pT ? pT->Height() : 0,
							   pPad->nFlushedVtx,
							   spec ? spec->size : 0.0f,
							   spec ? spec->hudcnt.x : 0.0f,
							   spec ? spec->hudcnt.y : 0.0f,
							   spec ? spec->hudcnt.z : 0.0f);
						// Both formats: an all-zero surface reads as
						// transparent in PNG, and the JPEG drops alpha.
						if (pT) {
							NatSaveSurface("vchud.png", pT);
							NatSaveSurface("vchud.jpg", pT);

							std::vector<unsigned char> px(size_t(pT->Width()) * pT->Height() * 4);
							if (pDevice->ReadTexture(pT, 0, px.data(), px.size())) {
								size_t nLit = 0, nAlpha = 0;
								unsigned mx[4] = { 0, 0, 0, 0 };
								for (size_t i = 0; i < px.size(); i += 4) {
									if (px[i] | px[i+1] | px[i+2]) nLit++;
									if (px[i+3]) nAlpha++;
									for (int c = 0; c < 4; c++)
										if (px[i+c] > mx[c]) mx[c] = px[i+c];
								}
								LogErr("SKPDUMP VC HUD: %zu of %zu pixels have colour, "
									   "%zu have alpha; max BGRA = %u %u %u %u",
									   nLit, px.size() / 4, nAlpha,
									   mx[0], mx[1], mx[2], mx[3]);
							}
						}
					}
				}
			}
		}

		// Do we have an old interface ?
		VulkanPad* pOld = GetTopInterface();
		if (pOld) {
			pOld->BeginDrawing();	// Continue with the old one
			LogDbg("Red", "Continue Previous Sketchpad");
		}
	}
	else {
		GDIPad* pGDI = (GDIPad*)sp;
		SURFACE(hSrf)->ReleaseDC(pGDI->GetDC());
		delete pGDI;
	}
}

// =======================================================================

void VulkanClient::clbkReleaseSketchpad(oapi::Sketchpad *sp)
{
	clbkReleaseSketchpad_const(sp);
}


// =======================================================================
// D3D9PadFont/Pen/Brush become VulkanPadFont/Pen/Brush. Nothing else here
// touches a graphics API at all: these are set insertions and deletes.

Font *VulkanClient::clbkCreateFont(int height, bool prop, const char *face, FontStyle style, int orientation) const
{
	_TRACE;
	if (ChkDev(__FUNCTION__)) return NULL;
	return *g_fonts.insert(new VulkanPadFont(height, prop, face, style, orientation)).first;
}

Font* VulkanClient::clbkCreateFontEx(int height, char* face, int width, int weight, FontStyle style, float spacing) const
{
	_TRACE;
	if (ChkDev(__FUNCTION__)) return NULL;
	return *g_fonts.insert(new VulkanPadFont(height, face, width, weight, style, spacing)).first;
}


// =======================================================================

void VulkanClient::clbkReleaseFont(Font *font) const
{
	_TRACE;
	if (!g_fonts.count(font)) return;
	g_fonts.erase(font);
	delete ((VulkanPadFont*)font);
}

// =======================================================================

Pen *VulkanClient::clbkCreatePen(int style, int width, DWORD col) const
{
	_TRACE;
	return *g_pens.insert(new VulkanPadPen(style, width, col)).first;
}

// =======================================================================

void VulkanClient::clbkReleasePen(Pen *pen) const
{
	_TRACE;
	if (!g_pens.count(pen)) return;
	g_pens.erase(pen);
	delete ((VulkanPadPen*)pen);
}

// =======================================================================

Brush *VulkanClient::clbkCreateBrush(DWORD col) const
{
	_TRACE;
	return *g_brushes.insert(new VulkanPadBrush(col)).first;
}

// =======================================================================

void VulkanClient::clbkReleaseBrush(Brush *brush) const
{
	_TRACE;
	if (!g_brushes.count(brush)) return;
	g_brushes.erase(brush);
	delete ((VulkanPadBrush*)brush);
}


// ======================================================================
// class VisObject

VisObject::VisObject(OBJHANDLE hObj) : hObj(hObj)
{
	_TRACE;
}

// =======================================================================

VisObject::~VisObject ()
{
}
