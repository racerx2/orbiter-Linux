// ==============================================================
// VulkanClient.cpp
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2006-2026 Martin Schweiger
//				 2012-2016 Jarmo Nikkanen
// ==============================================================
//
// CONVERTED FROM OVP/D3D9Client/D3D9Client.cpp, read end to end (3303 lines).
//
// This is the module entry point and the GraphicsClient override set: the
// functions Orbiter's core calls to start the client, hand it a window, run a
// frame, and shut it down. Most of it is core API and converts unchanged. The
// changes are structural and worth stating once, here, rather than at each of
// the several dozen places they show up.
//
//  1. THE CORE OWNS THE DEVICE AND THE FRAME. On Windows the client created
//     the Direct3D device, the swap chain and the back buffer, called
//     BeginScene/EndScene around its own rendering, and Present()ed. Here
//     Src/Orbiter/Linux/UIHost.cpp stands up the VkInstance, the
//     VkPhysicalDevice, the VkDevice, the queue, the descriptor pool, the
//     swapchain and the colour+depth render pass BEFORE any client loads, and
//     it owns the only vkQueuePresentKHR in the process. So this file does
//     not create a device -- VulkanDevice::Adopt takes the core's context --
//     and clbkDisplayFrame does not present.
//
//  2. LOST DEVICES DO NOT EXIST. D3D9's TestCooperativeLevel /
//     D3DERR_DEVICELOST / Reset() cycle -- which this file spends a good deal
//     of code on -- has no Vulkan counterpart. The nearest thing is
//     VK_ERROR_DEVICE_LOST, which is unrecoverable, and swapchain
//     recreation on resize, which is the CORE's job. See
//     the porting notes for the longer argument.
//
//  3. IMGUI IS THE CORE'S. imgui_impl_dx9 and imgui_impl_win32 were the
//     client's backends on Windows; UIHost.cpp initialises ImGui with the
//     Vulkan and SDL backends and runs its frame. The client draws into
//     ImGui through the ImGuiDialog interface in OrbiterAPI.h and never
//     touches a backend.
//
//  4. d3d9on12.h AND Direct3DCreate9On12 HAVE NO COUNTERPART. They let a
//     D3D9 program run on the D3D12 runtime, which is a Windows migration
//     path and nothing more.
//
//  5. NvOptimusEnablement HAS NO COUNTERPART. It is a DWORD exported from a
//     Windows DLL that the NVIDIA driver reads out of the PE export table to
//     pick the discrete GPU on a laptop. Linux has no such protocol; the
//     equivalent is an environment variable the user sets
//     (DRI_PRIME/__NV_PRIME_RENDER_OFFLOAD), which is not the module's to
//     set.
//
//  6. NVAPI HAS NO COUNTERPART. The stereo blocks sit behind #ifdef _NVAPI_H,
//     never defined in this tree, and NVAPI is a Windows-only Direct3D
//     library.
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
// <d3d9on12.h> and the two ImGui backend headers are gone; see notes 3 and 4
// in the file header. "imgui.h" stays -- the client builds ImGui windows
// through ImGuiDialog, it simply does not own a backend.
#include "imgui.h"

// The _MSC_VER round() shim for Visual Studio 2012 and earlier is gone. GCC
// has had C99's round() since long before C++17, which this builds as.

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
// g_pD3DObject is gone. It was the IDirect3D9 factory object, created by the
// VideoTab so it could enumerate adapters and modes. The VkInstance comes
// from the core's context instead -- see VulkanFrame.h -- and the client
// never creates one.
Memgr<float>* g_pMemgr_f = nullptr;
Memgr<INT16>* g_pMemgr_i = nullptr;
Memgr<UINT8>* g_pMemgr_u = nullptr;
Memgr<WORD>* g_pMemgr_w = nullptr;
Memgr<VERTEX_2TEX>* g_pMemgr_vtx = nullptr;
Texmgr<VulkanTexture*>* g_pTexmgr_tt = nullptr;
Vtxmgr<VulkanBuffer*>* g_pVtxmgr_vb = nullptr;
Idxmgr<VulkanBuffer*>* g_pIdxmgr_ib = nullptr;

// The __Direct3DCreate9On12 function-pointer typedef is gone with the header
// that named its argument type; see note 4.

set<VulkanMesh*> MeshCatalog;
set<SurfNative*> SurfaceCatalog;
unordered_map<string, SURFHANDLE> SharedTextures;
unordered_map<string, SURFHANDLE> ClonedTextures;
unordered_map<MESHHANDLE, class SketchMesh*> MeshMap;
unordered_map<std::string, VulkanTexture*> MicroTextures;

DWORD uCurrentMesh = 0;
vObject *pCurrentVisual = 0;
_VulkanStats VulkanStats;

// The #ifdef _NVAPI_H StereoHandle is gone; see note 6.

bool bFreeze = false;
bool bFreezeEnable = false;
bool bFreezeRenderAll = false;

// Debuging Brush-, Pen- and Font-accounting
std::set<Font *> g_fonts;
std::set<Pen *> g_pens;
std::set<Brush *> g_brushes;

extern list<gcGUIApp *> g_gcGUIAppList;

// The extern "C" NvOptimusEnablement export is gone; see note 5.

// ==============================================================
// API interface
// ==============================================================

// ==============================================================
// Initialise module

DLLCLBK void InitModule(HINSTANCE hDLL)
{

#ifdef _DEBUG
	// _CrtSetDbgFlag / _CrtSetBreakAlloc are the MSVC debug heap and have no
	// counterpart; the equivalent here is running under valgrind or building
	// with -fsanitize=address, neither of which is a call the module makes.
	// THE ASSERTIONS BELOW ARE KEPT IN FULL. They check the layout of
	// oapi::FVECTOR4 -- that its union views agree and that .a/.w sit behind
	// .rgb/.xyz -- which is exactly the kind of thing that could differ
	// between MSVC and GCC and would be silently wrong if it did.

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

	// D3DXCheckVersion(D3D_SDK_VERSION, D3DX_SDK_VERSION) HAS NO COUNTERPART.
	// It asked whether the installed D3DX redistributable matched the headers
	// the module was built against -- a Windows redistributable-versioning
	// problem. The Vulkan loader answers the equivalent question at
	// vkCreateInstance time, and the core has already done that by the time
	// this runs; a client that gets here has a working device.
	//
	// The #ifdef _NVAPI_H NvAPI_Initialize block that followed is gone; see
	// note 6 in the file header.

	Config = new VulkanConfig();

	// The cache directory names follow the module: "Cache/VulkanClient/Shaders".
	// GetFileAttributesA and CreateDirectoryA come from the Linux shim
	// (Src/Orbiter/Linux/windows.h), which implements both over stat() and
	// mkdir(), so the three-step create converts verbatim.
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

	// The #ifdef _NVAPI_H NvAPI_Unload block is gone; see note 6.

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

// THE ORDER OF THIS INITIALISER LIST IS THE ORDER OF THE DECLARATIONS.
// The Windows list is in a different order, which MSVC accepts silently;
// GCC warns (-Wreorder), and since members are initialised in declaration
// order regardless, a list that reads differently from what runs is a trap.
// Same members, same values, only the sequence moves.
//
// pLoadLabel("") and pLoadItem("") become pLoadLabel{} and pLoadItem{}. Both
// are char[128], and a character ARRAY cannot be initialised from a string
// literal in parentheses -- that is an MSVC extension, and GCC rejects it.
// The braces value-initialise the buffer to all zeros, which is exactly what
// an empty string in a char buffer is.
//
// hMainThread(NULL) becomes MainThreadId(0); see GetMainThreadId() in the
// header for why the pseudo-handle became a thread id.
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
	// SAFE_RELEASE(g_pD3DObject) has no counterpart: there is no factory
	// object to release. The VkInstance belongs to the core.
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


// ==============================================================
// This is called only once when the launchpad will appear
// This callback will initialize the Video tab only
//
// THE ENTIRE D3D9-CREATION BLOCK IS GONE, and it is the clearest single
// example of note 1 in the file header. On Windows this function:
//
//   - built a D3D9ON12_ARGS from Config->Enable9On12,
//   - looked up Direct3DCreate9On12 in an already-loaded D3D9.dll,
//   - called it, or Direct3DCreate9, to make the IDirect3D9 factory,
//   - failed the client if that returned NULL.
//
// There is nothing here to create. The VkInstance, VkPhysicalDevice, VkDevice
// and queue all belong to Src/Orbiter/Linux/UIHost.cpp and exist before this
// module is loaded; VulkanDevice::Adopt takes them in
// clbkCreateRenderWindow. Config->Enable9On12 selected between the native
// D3D9 runtime and the D3D12 emulation layer, a Windows-only choice with no
// counterpart at all.
//
// OapiExtension::RunsUnderWINE() goes with it. It was asked here only to skip
// the 9-on-12 path, since WINE's D3D9.dll has no such export.
//
bool VulkanClient::clbkInitialise()
{
	_TRACE;
	LogAlw("================ clbkInitialise ===============");
	LogAlw("Orbiter Version = %d",oapiGetOrbiterVersion());

	// Perform default setup
	if (GraphicsClient::clbkInitialise()==false) return false;

	//Create the Launchpad video tab interface
	// (char*) on the literal: oapiWriteLog takes char*, not const char*, and
	// GCC rejects the implicit conversion from a string literal
	// (-Wwrite-strings) where MSVC permits it. The cast is the spelling the
	// rest of this file already uses at its other oapiWriteLog call sites.
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

	// `if (!g_pD3DObject) return NULL;` has no counterpart -- there is no
	// factory object. What this function used to reach through it,
	// CD3DFramework9::Initialize now gets from the core's context.

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
	// surfBltTgt is gone. The Windows line set it and said so itself -- "This
	// variable is not used, set it to NULL anyway". It is GraphicsClient's
	// own member for the default Blt target and nothing in the client reads
	// it; VulkanClient.h does not redeclare it.
	//
	// Unchanged. GetCurrentThread() is the shim's, and returns the same
	// pseudo-handle (HANDLE)-2 Windows returns, so the two guards fed by this
	// value stay dead exactly as they are on Windows. See the note at
	// VulkanClient::GetMainThread() for why translating it to a real thread
	// id -- which an earlier version of this port did -- turns a dormant
	// Windows check into a Linux-only abort.
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

	// The GDI black-fill of the client area converts as written: the Linux
	// shim supplies GetClientRect, GetDC, CreateSolidBrush, FillRect,
	// DeleteObject, ReleaseDC and ValidateRect, and Gdi.cpp records the fill
	// into the display list the core replays. The comment on ValidateRect is
	// the author's and still applies.
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

	// D3DXCreateTextureFromFileA becomes NatLoadTexture. D3DX read the file,
	// chose a format and uploaded in one call; NatLoadTexture is the client's
	// own loader (VulkanSurface.cpp) doing the same three things over stb_image
	// and DDS. THE FILE NAME IS UNCHANGED -- Textures/D3D9Noise.dds is a
	// shipped asset, not an API name, and renaming it would break every
	// installation. Same decision as Scene.cpp's D3D9CLUT.dds.
	pNoiseTex = NatLoadTexture("Textures/D3D9Noise.dds");
	if (!pNoiseTex) LogErr("Failed to load Textures/D3D9Noise.dds");

	// GetRenderTarget(0) and GetDepthStencilSurface() have no counterpart:
	// the swapchain images and the depth buffer belong to UIHost.cpp and the
	// client is never handed one. What it gets instead are the attachment
	// PROXIES the framework built -- extent and format, no image -- which is
	// all the back-buffer SURFHANDLE is ever asked for. See
	// VulkanDevice::CreateAttachmentProxy.
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

	// The #ifdef _NVAPI_H stereo block stood here; see note 6 in the file
	// header. NvAPI_Stereo_CreateHandleFromIUnknown takes an IUnknown*, which
	// is the clearest statement of why it cannot be carried: it is a COM
	// interface on a Direct3D device.

	// Create status queries -----------------------------------------
	//
	// FOUR CreateQuery CALLS, ALL WITH A NULL OUT-POINTER. That is D3D9's
	// documented idiom for "is this query type supported" -- it validates and
	// creates nothing -- and the four answers were only logged.
	//
	// The Vulkan counterparts are not four questions but two, and neither is
	// a create:
	//
	//   D3DQUERYTYPE_OCCLUSION       VK_QUERY_TYPE_OCCLUSION, which every
	//                                conforming implementation supports; the
	//                                only variable is whether PRECISE counts
	//                                are available (occlusionQueryPrecise).
	//   D3DQUERYTYPE_PIPELINETIMINGS
	//   D3DQUERYTYPE_BANDWIDTHTIMINGS
	//   D3DQUERYTYPE_PIXELTIMINGS    no counterpart at all. These were
	//                                D3D9 driver-specific profiling counters,
	//                                unimplemented on most hardware even
	//                                then. The Vulkan equivalent is
	//                                VK_QUERY_TYPE_PIPELINE_STATISTICS, which
	//                                is one query type behind one feature bit
	//                                (pipelineStatisticsQuery) rather than
	//                                three.
	//
	// So the four log lines become two, reporting the two feature bits that
	// actually answer the question. Nothing in the client reads either.
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

	// bRunning's other half, on the host's side of the boundary.
	//
	// The reference needs one flag here because it owns its own device and
	// swap chain: setting bRunning is enough for the next Present to show a
	// scene. Here the window, the swapchain and the only present belong to
	// Src/Orbiter/Linux/UIHost.cpp, and it gates BOTH on g_sessionActive --
	// whether to call this client's scene callback at all, and whether the
	// host window has any reason to be on screen. Without this call the
	// window was HIDDEN the moment the Launchpad closed and the scene
	// callback was never invoked: a scenario launched to a blank desktop with
	// nothing in the log to say why.
	// The other half of the handshake: WHAT to draw when the host pumps a
	// frame. Registering it and raising the session flag are both required --
	// UIHost.cpp's renderFrame does
	//     if (g_sceneRenderFn && g_sessionActive) g_sceneRenderFn(...)
	// -- and this client previously did neither, so the test failed twice
	// over and the scene was never recorded.
	orbiter_SetSceneRenderCallback(&VulkanClient::SceneRenderThunk, this);

	orbiter_BeginSession();

	// The splash is the window's only content while a scenario loads, and
	// loading is now over. The reference's counterpart is releasing
	// pSplashScreen; here the pixels belong to the host, so it is told.
	orbiter_ClearSplash();

	LogAlw("=============== Loading Completed and Visuals Created ================");

#ifdef _DEBUG
	SketchPadTest();
#endif

	WriteLog("[Scene Initialized]");
}

// ==============================================================
// Perform some routine tests with sketchpad
//
// EVERY CALL IN THIS FUNCTION IS CORE API -- oapiGetSketchpad, the Sketchpad
// methods, gcCore2's polygon builders -- so it converts unchanged except for
// the asset path. "D3D9/SketchpadTest.dds" becomes "Vulkan/SketchpadTest.dds"
// because it is a directory THIS MODULE ships under its own name, unlike
// D3D9Noise.dds which lives in the shared Textures folder.
//
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

	// ptr(FVECTOR2(...)) becomes a named local. ptr() existed to take the
	// address of a temporary, which C++ forbids and MSVC allowed; the value
	// is unchanged. Same finding as Scene.cpp's, and the same one scale is
	// reused for the four calls that share it.
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

	// IMAGE_DDS becomes IMAGE_PNG. NatSaveSurface has no DDS writer -- there
	// is no Vulkan call that produces BC blocks and the client does not carry
	// a compressor -- which is the decision already recorded in
	// NatCompressSurface and in DebugControls::SaveEnvMap. The first save
	// above already asked for PNG, so the two outputs now match.
	oapiSaveSurface("SketchpadOutput2", hTgt, ImageFileFormat::IMAGE_PNG);

	oapiReleaseTexture(hTgt);
}




// ==============================================================
// Called when simulation session is about to be closed
//
void VulkanClient::clbkCloseSession(bool fastclose)
{

	LogAlw("================ clbkCloseSession ===============");

	// The partner of orbiter_BeginSession() in clbkPostCreation, and it has to
	// come FIRST: it clears the host's g_sessionActive, so the frame pump
	// stops calling this client's scene callback before anything the callback
	// touches is torn down below. Ending the session after the teardown would
	// leave the host free to render one more frame from freed objects.
	orbiter_EndSession();

	// AND THEN WAIT FOR THE FRAMES THAT WERE ALREADY SUBMITTED.
	//
	// orbiter_EndSession stops the host STARTING another frame; it does not
	// wait for the ones in flight, and everything below this line deletes
	// objects those frames are still reading. On Windows nothing had to wait:
	// every D3D9 resource was reference-counted and the runtime held a
	// reference for as long as its own queued commands needed one, so a
	// Release during teardown decremented a count and freed nothing early.
	// Vulkan has no such count, and the layer named four kinds of casualty at
	// once:
	//
	//     VUID-vkDestroySampler-sampler-01082
	//     VUID-vkDestroyPipeline-pipeline-00765
	//     VUID-vkDestroyBuffer-buffer-00922
	//     VUID-vkDestroyImageView-imageView-01026
	//
	// ten of each -- the report cap -- ending in the segfault that closed
	// every session.
	//
	// DestroyObjects already does exactly this pair, but at the END of the
	// teardown, by which time the scene, every visual, every mesh and every
	// shader have been deleted. The wait belongs where the deleting starts.
	// vkDeviceWaitIdle alone is not enough: the frames' command buffers still
	// NAME the descriptor sets and images, which is why the command pools are
	// reset too. See orbiter_ResetFrameCommands.
	//
	// AND UNDER THE DEVICE LOCK, because the tile loaders are still running
	// at this point -- TileBuffer::ShutDown and TileManager2Base::ShutDown
	// are forty lines below, deliberately, since that is where the reference
	// stops them. vkDeviceWaitIdle is defined as vkQueueWaitIdle on every
	// queue, so it is host access to the queue those threads are submitting
	// on through EndOneShot, and the layer reported exactly that:
	//
	//     UNASSIGNED-Threading-MultipleThreads-Write
	//     vkDeviceWaitIdle(): THREADING ERROR : object of type VkQueue is
	//     simultaneously used in current thread A and thread B
	//
	// The reference needs no counterpart: D3DCREATE_MULTITHREADED made the
	// D3D9 runtime hold this lock for every device call. See orbiter_LockDevice.
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


// ==============================================================
// SAFE_RELEASE BECOMES DestroyTexture FOR EVERY IMAGE IN THIS FUNCTION, and
// the difference is worth stating once because it recurs: Release()
// DECREMENTED a reference count and destroyed only at zero, where
// DestroyTexture destroys unconditionally. That is correct for each of these
// -- pSplashScreen, pTextScreen, pNoiseTex and the microtextures are all
// images this client created and nothing else holds -- but it is the reason
// SAFE_RELEASE must never be translated mechanically. See finding 45 in
// DebugControls.cpp for the case where it must not be translated at all.
//
void VulkanClient::clbkDestroyRenderWindow (bool fastclose)
{
	_TRACE;
	oapiWriteLog((char*)"Vulkan: [Destroy Render Window Called]");
	LogAlw("============= clbkDestroyRenderWindow ===========");

	// The #ifdef _NVAPI_H stereo-handle teardown stood here; see note 6.

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
// THE RENDER-TARGET STACK IS THE SINGLE BIGGEST STRUCTURAL CONVERSION IN
// THIS FILE, so the argument is written out once here and the three
// functions below follow it.
//
// WHAT THE WINDOWS VERSION DID. A render target is DEVICE STATE in D3D9:
// SetRenderTarget(0, surf) and SetDepthStencilSurface(surf) point the device
// at a pair of surfaces, SetViewport sizes the transform, and every draw that
// follows lands there until someone points it somewhere else. The stack is
// the client's own bookkeeping on top of that -- "remember what was bound so
// I can put it back" -- and the three device calls in each of these functions
// are what actually did the pointing.
//
// WHAT VULKAN HAS INSTEAD. The set of attachments is baked into a
// VkFramebuffer inside a VkRenderPass. A pass cannot be begun inside another
// pass, the pipelines drawn in it must have been built against a COMPATIBLE
// pass, and the viewport is dynamic state on a command buffer rather than on
// a device. So "change the render target" is not three calls -- it is
// VulkanDevice::BeginOffscreen: pick or build a pass for these attachment
// formats, pick or build a framebuffer for these exact images, transition
// them, take a command buffer of its own, and begin. EndOffscreen ends the
// pass, transitions the colour attachments to SHADER_READ_ONLY_OPTIMAL,
// submits and waits.
//
// SO THE STACK SURVIVES AND THE DEVICE CALLS BECOME ONE PAIR. The list, the
// order, the codes and the log lines are unchanged; the three SetXxx calls
// become BeginOffscreen, and popping ends the current pass and re-begins the
// one underneath. Only one offscreen pass can be open at a time, which is why
// each of these ends the current one first -- SetRenderTarget did the same
// thing implicitly by simply overwriting the binding.
//
// THE BACK BUFFER IS THE EXCEPTION AND MUST BE. pBackBuffer is an ATTACHMENT
// PROXY: it carries the extent and format of the core's colour attachment and
// holds no VkImage, because the swapchain images belong to UIHost.cpp. There
// is nothing to begin a pass on -- and nothing needs to be, because the
// core's render pass is ALREADY OPEN on the frame's command buffer when the
// scene callback runs. So a push of the back buffer records the stack entry
// and begins nothing, and popping back to it ends the offscreen pass and
// leaves the core's, which is exactly the state the frame started in.
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

	// SetViewport is gone as a separate call: BeginOffscreen sets the
	// viewport and the scissor from the extent of the attachments it is
	// given, which is the same rectangle from the same place.
	if (pColor && !pColor->IsProxy()) {
		if (!pDevice->BeginOffscreen(pColor, pDepthStencil)) {
			LogErr("PushRenderTarget: BeginOffscreen failed for %s", _PTR(pColor));
		}
	}

	RenderStack.push_front(data);
	LogDbg("Plum", "PUSH:RenderStack[%lu]={%s, %s} %s", (unsigned long)RenderStack.size(), _PTR(data.pColor), _PTR(data.pDepthStencil), labels[data.code]);
}

// ==============================================================
// The Windows version changes the DEVICE binding and deliberately leaves the
// stack alone -- the entry on top still names the surfaces that were pushed.
// That behaviour is preserved: the pass is re-begun on the new pair and the
// stack is untouched, so a following Pop restores whatever is underneath,
// not what this call installed. Same asymmetry, same consequences.
//
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

// ==============================================================
// HackFriendlyHack HAS NOTHING LEFT TO DO, and the function's own comment
// says why: it existed to put the D3D DEVICE "in 'more' expected state" for
// someone attaching a debugger -- viewport, render target and depth-stencil
// surface set back to the back buffer. All three were device state. Vulkan
// has none of them: there is no current render target to reset, the viewport
// lives on a command buffer, and the core's render pass is already the one
// bound. Kept as an empty body because the SDK declares it and callers may
// exist outside this tree.
//
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

	// The counterpart of pDevice->BeginScene(). It does not open a command
	// buffer -- UIHost.cpp's frame pump owns the one that reaches the
	// swapchain -- it declares that a scene frame is being built, which is
	// what makes the host call this client's scene callback when it pumps.
	// Its EndSceneFrame partner is in PresentScene, where Present() was.
	orbiter_BeginSceneFrame();
}


// ==============================================================
// THE SCENE IS RECORDED HERE, NOT IN clbkRenderScene, and this is the largest
// structural difference in the whole client.
//
// The reference draws the moment Orbiter asks: clbkRenderScene calls
// BeginScene(), scene->RenderMainScene(), the overlays, EndScene(). It can,
// because a D3D9 device is always ready to record -- there is no such thing as
// "outside a frame".
//
// A Vulkan draw has to go into a command buffer, and the ONLY command buffer
// that reaches the swapchain belongs to UIHost.cpp's frame pump. It exists
// solely for the duration of the callback the pump makes, inside the render
// pass it has already opened. So the work cannot happen when Orbiter asks; it
// happens when the host calls back.
//
// clbkRenderScene therefore only OPENS the frame, and everything it used to do
// lives here. The order Orbiter drives is unchanged --
//
//     clbkRenderScene()  -> orbiter_BeginSceneFrame()
//     clbkDisplayFrame() -> PresentScene() -> orbiter_EndSceneFrame()
//                                          -> pump -> THIS
//
// -- so the scene is still built once per frame, in the same sequence, just
// recorded at the point where a command buffer exists.
//
// WITHOUT THIS the client rendered into no command buffer at all. The log said
// so, several hundred times a frame:
//
//     VulkanERROR: ShaderClass::Setup outside a frame -- no command buffer
//     VulkanERROR: VulkanEffectFile::BeginPass(0) outside the scene callback
//
// and the window showed the menu bar and HUD (which the host draws) over a
// black scene, with a "Critical Error / BltError" box on top.
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

	// The frame's buffer is the host's and is valid only until this returns,
	// so it is published for the duration and withdrawn at the end. Every
	// ShaderClass::Setup and VulkanEffectFile::BeginPass below reads it
	// through VulkanDevice::GetCommandBuffer().
	pDevice->SetFrameCommandBuffer(cmd, width, height);

	// Which buffer the CORE handed us, so a draw recorded elsewhere can be
	// told apart from one recorded into the frame that is actually presented.
	if (getenv("ORBITER_VK_TRACE_TILES")) {
		static int n = 0;
		if (n++ < 3)
			LogErr("FRAMETRACE core scene callback cmd=%p %ux%u", (void *)cmd, width, height);
	}

	if (pWM) pWM->Animate();

	// THE FRAME BOUNDARY FOR EVERY DESCRIPTOR POOL IN THE CLIENT, and a call
	// with no counterpart in the reference.
	//
	// A D3D9 draw consumed its state immediately: SetTexture recorded a
	// binding and the call was over. A Vulkan draw only records a REFERENCE
	// to a descriptor set, so each draw needs its own and every one of them
	// has to stay alive until the frame that named it has been submitted.
	// Both pipeline builders allocate from a pool for that reason, and both
	// pools are recycled here -- once, at the top of the frame, which is the
	// only point at which the previous frame's sets are certainly done with.
	//
	// VulkanEffectFile::ResetFrame also rewinds the per-frame uniform arena;
	// without this call it fills after a few thousand passes and says so.
	//
	// EVERY effect file, not `VulkanEffect::FX` alone. There are four --
	// VulkanEffect::FX, Scene::FX, VulkanPad::FX and
	// VulkanCelestialSphere::s_FX -- and this line used to reset one, so the
	// other three filled once and then refused every pass for the rest of the
	// session:
	//
	//     2451 x VulkanEffectFile: the frame's uniform arena is full
	//            (786432 bytes, 4096 passes). ResetFrame is not being called.
	//
	// BeginPass returns false there, so those draws were dropped in silence.
	//
	// Switching this from `VulkanEffect::FX->ResetFrame()` to all four also
	// exposed a second, older defect, in the reset ITSELF -- it was resetting
	// pools the GPU was still reading:
	//
	//     VUID-vkResetDescriptorPool-descriptorPool-00313
	//     vkResetDescriptorPool(): descriptorPool can't be called on
	//     VkDescriptorPool 0x14c000000014c that is currently in use by
	//     VkCommandBuffer 0x63498dac1400.
	//
	// which the validation layer reported with the old single-FX line in
	// place too, and which the driver answered with intermittent
	// VK_ERROR_DEVICE_LOST. "The only point at which the previous frame's
	// sets are certainly done with" assumes ONE frame in flight; the core
	// keeps orbiter_GetFramesInFlight() of them. Both ResetFrame bodies now
	// ROTATE their pools and arenas on that lag instead of resetting them in
	// place -- see VulkanEffectFile's FrameSet note.
	VulkanEffectFile::ResetFrameAll();
	ShaderClass::ResetFrame();

	// `if (Config->PresentLocation == 1) PresentScene();` is gone with the
	// setting. PresentLocation chose whether the present happened here or in
	// clbkDisplayFrame, and there is no present to place: UIHost.cpp owns the
	// only vkQueuePresentKHR. See the note at the top of VulkanConfig.h, and
	// PresentScene() below for what is left of that function. It is called
	// once, from clbkDisplayFrame, which is the callback whose name says so.

	scene_time = VulkanGetTime();

	// TestCooperativeLevel() HAS NO COUNTERPART, and this is the one place
	// the absence is load-bearing rather than incidental.
	//
	// A D3D9 device could be LOST -- another application taking exclusive
	// fullscreen, a display-mode change, a driver reset -- after which every
	// call failed with D3DERR_DEVICELOST until Reset() succeeded. That whole
	// state machine is a Direct3D 9 concept. Vulkan has one failure that
	// resembles it, VK_ERROR_DEVICE_LOST, and it is UNRECOVERABLE: the
	// VkDevice and every object made from it are dead and the program must
	// rebuild from the instance. It is reported by the call that hit it, not
	// polled, and the calls in question are the queue submits and present --
	// all of which belong to UIHost.cpp here, not to the client.
	//
	// The other half of what this guarded is a resize, which on Windows meant
	// a device Reset. Here it means the CORE recreates its swapchain and
	// render pass; the client is told through clbkOptionChanged and
	// GetRenderPass(), and its pipelines key on the pass so they rebuild
	// themselves. See the porting notes.
	//
	// So the MessageBoxA that stood here -- and its advice about Alt-Tab and
	// multi-sampling in true fullscreen, both Direct3D-fullscreen concepts --
	// has nothing to report.

	// GetAvailableTextureMem() becomes GetLocalMemorySize(), with the caveat
	// its own declaration carries: D3D9 reported what was FREE, and core
	// Vulkan reports only what EXISTS, so this is the total device-local heap
	// rather than the same number. The test below is "are we nearly out",
	// which an upper bound cannot answer -- it will simply never fire on a
	// card with more than 32MB of VRAM, which is every card that can run
	// this. Recorded rather than silently dropped, because a client that
	// wants the true figure needs VK_EXT_memory_budget.
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
			// BeginScene/EndScene have no counterpart -- the core's render
			// pass is already open on the frame's command buffer -- and
			// D3DXFont::DrawTextA becomes the client's own Sketchpad text.
			// See pOverlayFont in the header for why the D3DX font could not
			// be carried. DT_CENTER|DT_TOP becomes SetTextAlign(CENTER, TOP)
			// with the x at the centre of the same rectangle, which is what
			// DrawTextA did with it.
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
			// D3DCOLOR_XRGB(0,255,255) is 0x00FFFF; SetTextColor takes the
			// same 0x00RRGGBB packing.
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

	// The host's buffer goes out of scope the moment this returns. Anything
	// that tries to draw after it -- a Sketchpad the panel code forgot to
	// release, a stray blit -- must fail loudly rather than record into a
	// dangling handle, which is what withdrawing it here guarantees.
	pDevice->SetFrameCommandBuffer(VK_NULL_HANDLE, 0, 0);
}

// ==============================================================

void VulkanClient::clbkTimeJump(double simt, double simdt, double mjd)
{
	_TRACE;
	GraphicsClient::clbkTimeJump (simt, simdt, mjd);
}


// ==============================================================
// PresentScene HAS NOTHING TO PRESENT, and this is note 1 in the file header
// at its sharpest.
//
// IDirect3DDevice9::Present hands the back buffer to the display and flips.
// The Vulkan counterpart is vkQueuePresentKHR on a swapchain image that was
// acquired with vkAcquireNextImageKHR, and BOTH belong to
// Src/Orbiter/Linux/UIHost.cpp: it acquires, begins the render pass, calls
// the client's scene callback inside it, ends, submits and presents, once per
// frame. There is exactly one present in the process and it is not this one.
// A second would not be a second picture -- it would be a validation error
// and a hang.
//
// What is left is the timing, which is real, and RenderWithPopupWindows,
// which is Win32 window management rather than presentation. Both are kept.
//
void VulkanClient::PresentScene()
{
	double time = VulkanGetTime();

	// The fullscreen/windowed branch collapses because the two arms differed
	// only in whether Present was skipped when popup windows had already
	// repainted the screen -- and there is no Present here. What the branch
	// still has to do, it does in both arms.
	RenderWithPopupWindows();

	// THIS IS WHERE Present() WENT.
	//
	// The note above is right that this client has no present of its own, and
	// wrong to conclude that nothing takes Present's place. orbiter_EndSceneFrame
	// closes the scene frame BeginSceneFrame opened and calls the host's frame
	// pump, which acquires a swapchain image, opens the render pass, calls
	// this client's registered scene callback inside it, composites the
	// dialogs on top and presents. One call, one picture on screen -- exactly
	// what Present did, performed by the file that owns the swapchain.
	//
	// Omitting it is what left the window blank: the client rendered its
	// scene into a callback the host never invoked, because the host only
	// invokes it while a session is active and only pumps when asked.
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
		// StretchRect(src, NULL, dst, rect, POINT) becomes
		// BlitTexture(dst, rect, src, NULL, false) -- DESTINATION FIRST, and
		// D3DTEXF_POINT is the trailing false. See the note in Scene.cpp's
		// RenderBlurredMap.
		//
		// AND THE DESTINATION CANNOT BE BLITTED TO. pBackBuffer is an
		// attachment proxy with no VkImage (the swapchain images are
		// UIHost.cpp's), and vkCmdBlitImage is illegal inside a render pass
		// in any case. The splash screen therefore goes through the client's
		// own Sketchpad, which draws into the pass that is already open --
		// the same route the "Frozen" overlay above takes, and the same
		// finding as Scene::VisualizeCubeMap's.
		if (Sketchpad *pSkp = clbkGetSketchpad(GetBackBufferHandle())) {
			if (pSplashScreen) {
				RECT full = _RECT(0, 0, viewW, viewH);
				pSkp->StretchRect(SURFHANDLE(pSplashScreen), NULL, &full);
			}
			if (pTextScreen) pSkp->StretchRect(SURFHANDLE(pTextScreen), NULL, &txt);
			clbkReleaseSketchpad(pSkp);
		}
	}

	// Was `if (Config->PresentLocation == 0) PresentScene();`. Unconditional
	// now, because PresentLocation is gone with the present it placed; see
	// clbkRenderScene.
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


// ==============================================================
// SetDialogBoxMode(true) HAS NO COUNTERPART, here or in
// RenderWithPopupWindows below.
//
// It told a D3D9 device in EXCLUSIVE FULLSCREEN to allow GDI to draw over the
// front buffer, so that Win32 dialogs could appear on top of the rendered
// scene. It required D3DSWAPEFFECT_DISCARD, a lockable back buffer and a
// non-multisampled swap chain -- which is why the message box in
// clbkRenderScene warned about dialogs and multi-sampling in true fullscreen.
//
// There is no exclusive fullscreen here to escape from: the core runs an SDL
// window (borderless fullscreen at most), and dialogs are ImGui windows drawn
// inside the same frame rather than HWNDs the desktop compositor puts on top.
// So the function has nothing left to do; it is kept because the SDK declares
// it and the callback must exist.
//
void VulkanClient::clbkPreOpenPopup ()
{
	_TRACE;
}

// =======================================================================
// This function is pure Win32 window arithmetic -- GetWindowRect,
// MonitorFromWindow, GetMonitorInfo, MoveWindow -- and converts unchanged
// against the Linux shim, which implements all four.
//
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

// #pragma region / #pragma endregion ARE GONE THROUGHOUT THIS FILE. They are
// an MSVC editor feature -- collapsible source folds -- that GCC does not
// know, and -Wall reports every one of them as an unknown pragma. The
// comment banners they wrapped are kept, so the file still reads in the same
// sections.

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

	// The Windows line is `int idx = meshmgr->StoreMesh(hMesh, fname);` and
	// idx is never read. The call is kept -- it is what stores the mesh --
	// and the variable dropped, because GCC warns on it where MSVC does not.
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
	// SURFACE(surf) is dropped: VulkanMesh::SetTexture takes a SURFHANDLE,
	// where D3D9Mesh::SetTexture took a SurfNative*. Same object either way.
	if (hMesh && surf) return ((VulkanMesh*)hMesh)->SetTexture(texidx, surf);
	return false;
}


// ==============================================================
// The (const D3DMATERIAL9*) and (D3DMATERIAL9*) casts in the two functions
// below are gone. They existed because D3D9Util's CreateMatExt/GetMatExt took
// a D3DMATERIAL9, and the SDK's MATERIAL has the same four colour members in
// the same order -- so the cast was a reinterpretation between two structs
// that happened to agree. The converted pair takes the SDK's MATERIAL
// directly (VulkanUtil.h), which is what the caller already has, so there is
// nothing to reinterpret. Dropping a cast that was covering a type pun is
// not a simplification; it is the removal of the D3D9 type that made it
// necessary.
//
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

// ==============================================================
// Returns a specific render parameter
//
// RP_REQUIRETEXPOW2 stays 0 and is now a statement of fact rather than a
// measurement: Vulkan requires full non-power-of-two texture support of every
// conforming implementation, so the answer cannot be anything else. Same
// finding as IsLimited() in the header.

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


// ==============================================================
// Message handler for render window
//
// THIS FUNCTION IS ENTIRELY WIN32 AND CONVERTS ALMOST UNCHANGED. Every
// message, every GET_X_LPARAM, TrackMouseEvent and GetAsyncKeyState is
// supplied by Src/Orbiter/Linux/windows.h and driven by the SDL event pump in
// UIHost.cpp -- the client asks the same questions and gets the same answers.
// The two substitutions are D3D9Pick -> VulkanPick and, at one site,
// GetObjectA() -> Object() (finding 34: GetObjectA was a Windows name
// collision workaround for the ANSI/Unicode GetObject macro, which the shim
// does not define).
//
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

// =======================================================================
// D3DXMatrixOrthoOffCenterRH becomes VMAT_OrthoOffCenterRH and D3D9Effect
// becomes VulkanEffect; the matrix, including its depth range, is unchanged
// -- D3D9 clip space is 0 <= z <= w and so is Vulkan's.

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


// =======================================================================
// Surface functions
// =======================================================================
//
// THE FIVE-CALL READBACK DANCE COLLAPSES INTO ONE CALL, and the reason is
// that both APIs have the same restriction and Vulkan gives it one name.
//
// A D3D9 default-pool surface cannot be locked, so the Windows body creates a
// render target, creates a system-memory surface, StretchRects into the first,
// GetRenderTargetDatas into the second, and locks that. Vulkan device-local
// memory cannot be mapped either, and the answer is vkCmdCopyImageToBuffer
// into a host-visible buffer -- which is what VulkanDevice::ReadTexture is.
// So five device calls and two temporary surfaces become ReadTexture into a
// vector.
//
// THE POOL BRANCH GOES WITH THEM. `desc->Pool != D3DPOOL_SYSTEMMEM` chose
// between that dance and locking the surface directly. There is one pool
// here; a host-visible image can be mapped and a device-local one cannot, and
// ReadTexture already takes both cases. So there is one path.
//
// AND ONE BUG DOES NOT SURVIVE THE COLLAPSE. The Windows sysmem branch ends
// `pSystem->UnlockRect()` -- pSystem, which on that path was never created
// and is still NULL. It is a null-pointer call on every save from a
// system-memory surface. There is nothing to translate it to, because there
// is no second surface at all here, so it simply is not written.
//
// The pixel format is what ReadTexture gives: 32-bit BGRA, four bytes per
// pixel, width*4 to the row. SaveSurfaceToFile is told that as a pitch rather
// than reading it out of a D3DLOCKED_RECT.

bool VulkanClient::clbkSaveSurfaceToImage(SURFHANDLE surf, const char *fname, ImageFileFormat fmt, float quality)
{
	_TRACE;
	if (ChkDev(__FUNCTION__)) return false;

	// A NULL SURFACE MEANS THE BACK BUFFER, AND THAT ONE CANNOT BE READ THE
	// SAME WAY.
	//
	// The reference resolves it to pDevice->GetRenderTarget(0) -- a surface
	// the D3D9 client owns, because it created the device and with it the
	// swap chain. This client owns neither. GetBackBufferHandle() returns a
	// SurfNative wrapping an attachment PROXY: VulkanDevice::CreateAttachmentProxy
	// builds it with vkImage == VK_NULL_HANDLE deliberately, because there is
	// no image here to name. ReadTexture on it therefore could only fail, and
	// did -- once per session, which is Orbiter::PreCloseSession's
	// Images/CurrentState.jpg (the thumbnail the Launchpad shows for the
	// current state) never being written.
	//
	// So the back buffer is read by the side that owns it. See
	// orbiter_CaptureBackBuffer, and UIHost.cpp's recordFrameCapture for why
	// the copy has to be recorded inside the frame rather than taken
	// afterwards.
	const bool bBackBuffer = (surf == NULL);
	if (bBackBuffer) surf = pFramework->GetBackBufferHandle();

	VulkanTexture *pSurf = SURFACE(surf)->GetSurface();

	if (pSurf==NULL) return false;

	bool bRet = false;

	if (fmt == ImageFileFormat::IMAGE_DDS) {
		char path[MAX_PATH];
		sprintf_s(path, "%s.dds", fname);
		// NatSaveSurface refuses DDS by name and logs it; see the note there.
		// The call is kept so the refusal is reported where the request was
		// made, exactly as the reference reported D3DX's failure.
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


// ==============================================================
// The 32-bit-to-24-bit repack, unchanged. desc->Height and desc->Width stay
// parameters, as they were on Windows -- see the note on the declaration for
// why the back-buffer path needs them to; pRect.pBits and pRect.Pitch become
// the two parameters that follow, which is the whole of the D3DLOCKED_RECT
// that was ever read.
//
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

// ==============================================================
// PUTTING AN IMAGE ON THE CLIPBOARD HAS NO COUNTERPART HERE, and the refusal
// is by name rather than a silent false.
//
// The Windows body opens the clipboard, makes a memory DC and a compatible
// bitmap, BitBlts into it and calls SetClipboardData(CF_BITMAP, hBm).
// Src/Orbiter/Linux/Platform.cpp's clipboard is the desktop text selection --
// SetClipboardData accepts CF_TEXT and returns NULL for anything else -- and
// GLFW, which is what backs it, offers only glfwSetClipboardString. There is
// no image channel to write to.
//
// WORTH RECORDING BECAUSE THE WINDOWS VERSION DOES NOT DO WHAT ITS NAME SAYS
// EITHER. It BitBlts from GetDC(hRenderWnd) -- the render WINDOW's device
// context, i.e. whatever is on screen -- not from the surface it was asked to
// save. Saving an off-screen surface to the clipboard put a picture of the
// simulator window on the clipboard instead. So the parameters this function
// now receives, the surface's own pixels, are what a working version would
// need; what is missing is somewhere to put them.
//
bool oapi::VulkanClient::SaveSurfaceToClipboard (const VulkanTexture* pTex, const void* pBits, size_t pitch)
{
	LogErr("SaveSurfaceToClipboard: putting an image on the clipboard is not "
		   "supported -- the clipboard here carries text only. Save to a file "
		   "instead.");
	return false;
}


// ==============================================================
// Nothing here touches a Direct3D type: NatLoadSurface is the client's own
// loader and the rest is map lookups. The commented-out cloning block is kept
// exactly as the author left it, D3D9Mesh -> VulkanMesh aside, because it is
// a record of an approach and not dead generated code.

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

		// `static const DWORD exclude = ~(OAPISURFACE_SHARED | OAPISURFACE_ORIGIN);`
		// stood above and is read only by the commented-out cloning block
		// below. It is moved into that comment with the code it serves,
		// because an unused file-scope constant is a warning here where it
		// was not on Windows, and leaving it live would be keeping a variable
		// alive for code that does not run.
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

// ==============================================================
// The '\\' path separator becomes '/'. It is a path this function BUILDS
// rather than one it receives, so the correct separator is the platform's;
// Windows accepted both, Linux accepts only the forward slash.
//
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

// =======================================================================
// SurfNative KEEPS ITS REFERENCE COUNT, and that is not a leftover.
//
// IncRef/DecRef here are not COM: they count how many times ORBITER has asked
// for a surface, which is a question about the SDK's ownership model rather
// than about Direct3D. The Vulkan image inside is destroyed unconditionally
// when the SurfNative goes, which is the part that changed.

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

// =======================================================================
// THE DECISION TREE IS KEPT AND ITS TESTS ARE RE-EXPRESSED. The Windows body
// picks between four device calls by asking about formats, pools and usage
// flags; three of those four calls collapse into one here, but the questions
// still matter, so the shape stays and each test is translated:
//
//   sd->Format == td->Format          unchanged; VulkanImageDesc::Format
//   td->Usage & D3DUSAGE_RENDERTARGET  Usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
//   sd->Pool == D3DPOOL_DEFAULT        !sd->HostVisible
//   sd->Pool == D3DPOOL_SYSTEMMEM      sd->HostVisible
//   SURFACE(x)->GetType()==D3DRTYPE_TEXTURE  SURFACE(x)->IsTexture()
//
// D3DPOOL AND HostVisible ARE THE SAME QUESTION ASKED TWICE. D3DPOOL_SYSTEMMEM
// meant "the CPU can reach this and the GPU cannot render to it"; Vulkan calls
// that host-visible memory, and it is a property of the allocation rather than
// a pool the resource was created in. See VulkanTypes.h's note on
// VulkanImageDesc.
//
// AND THREE DEVICE CALLS BECOME ONE. StretchRect, UpdateSurface and
// GetRenderTargetData were three entry points because a D3D9 copy was
// constrained by the pools at both ends: StretchRect for default-to-default,
// UpdateSurface for sysmem-to-default, GetRenderTargetData for
// default-to-sysmem. vkCmdBlitImage has no such rule -- it copies between
// images, and where the memory lives is not part of the question -- so all
// three become VulkanDevice::BlitTexture. THE ARGUMENT ORDER IS DESTINATION
// FIRST, so every converted call swaps its first two pairs.
//
// The branches are kept anyway rather than merged, because they differ in
// more than the call: the UpdateSurface branch copies to a POINT rather than
// a rectangle, and the GetRenderTargetData branch sets OAPISURFACE_CAPTURE on
// the target. Merging them would lose both.

// -----------------------------------------------------------------------------------------------
// D3DUSAGE_AUTOGENMIPMAP'S COUNTERPART, AND WHY IT HAS TO BE A CALL.
//
// NatCreateSurface turns OAPISURFACE_MIPMAPS into `Usage |=
// D3DUSAGE_AUTOGENMIPMAP` on Windows (D3D9Surface.cpp:313). That flag is a
// standing instruction to the driver: whenever level 0 of the texture is
// dirtied -- by a StretchRect into it, by rendering to it, by an unlock --
// D3D9 regenerates the whole chain by itself. Nothing in the client ever asks
// for it, which is why `SurfNative::GenerateMipMaps()` exists in the reference
// and is called from nowhere in it.
//
// Vulkan has no such flag and no driver-side mip generation. The levels are
// declared at creation and stay exactly as they were last written. So the
// regeneration D3D9 did implicitly has to be issued explicitly, at the same
// moments -- and this helper is that instruction, called on every path below
// that writes the target.
//
// WHAT IT COST TO NOT HAVE IT: the virtual-cockpit MFD screens were black.
// Instrument::AllocSurface (Src/Orbiter/Mfd.cpp:715) gives an MFD two
// surfaces -- `surf`, which the instrument draws into, and `tex`, which the
// mesh samples -- and adds OAPISURFACE_MIPMAPS to `tex` in VC mode.
// Instrument::Update ends with `gc->clbkBlt(tex, 0, 0, surf)`, which wrote
// level 0 and left levels 1..9 of the 512x512 chain at their undefined
// contents forever. A VC MFD panel is ~180 px on screen, so the sampler
// (WrapS: MipFilter = LINEAR) reads about level 1-2 -- never the level that
// had the picture in it.
//
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

	// POINT tp = { tgtx, tgty } becomes the destination RECTANGLE of the
	// source's size at that point, because BlitTexture takes rectangles at
	// both ends where UpdateSurface took a point at one. Same copy: the
	// UpdateSurface branch below is guarded by !bCL, so the two are the same
	// size by construction.
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


		// Screen Capture: Target is in system memory and source is a render taeget
		// 
		// NOTE THE ARGUMENT ORDER OF THE WINDOWS LINE. GetRenderTargetData's
		// signature is (pRenderTarget, pDestSurface), i.e. SOURCE first --
		// and the call reads `GetRenderTargetData(pss, pts)`, which is
		// correct: pss is the render target being read, pts the system-memory
		// destination. The converted call names the destination first, as
		// every BlitTexture does, so the two pointers swap places on the line
		// while the copy stays the same direction.
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


	// Scaling.. Format mismatch.. ColorKey.. Compressed Source..
	// Go for SketchPad
	//
	// THE RARE ROUTE, AND IT REPORTS ITSELF ONCE. Everything above copies with
	// a queue operation; this is the only branch that has to RENDER, and it is
	// the one a COMPRESSED source is forced onto -- bSC gates both BlitTexture
	// paths. It is also the branch with no failure reporting of its own: it
	// ends in `return AutoGenMips(tgt)`, which is `return true` whatever
	// happened, so a caller can never learn that nothing was drawn.
	//
	// Being able to see whether it was ENTERED is the difference between
	// "the copy rendered nothing" and "the copy was never issued", and those
	// two have completely different causes. The Delta-glider's registration
	// panel drew black with no complaint anywhere in the log, and separating
	// those two is exactly what was missing.
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

				// GROUND TRUTH FOR THE BLACK REGISTRATION PANEL.
				//
				// Every layer on this route reports success and the result is
				// black, so the only thing left is to look at the pixels.
				// ORBITER_VK_DUMPBLT=1 writes the source and the target to
				// /tmp once, right after the copy: if the source is black the
				// fault is in loading idpanel1.dds, and if the source is right
				// and the target is black the fault is in this draw.
				// Diagnostic only, env-gated, first fallback copy only.
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


// =======================================================================
// THE GDI-CACHE BRANCH HAS NOTHING LEFT TO WORK AROUND.
//
// The Windows body has two paths. If the surface is a GDI surface it gets a
// DC and StretchBlts into it. Otherwise -- and this is the whole of the else
// branch -- it borrows a cache texture, gets a DC on THAT, blits into it, and
// then copies the cache back over the real surface, because
// IDirect3DSurface9::GetDC() does not work on a render target.
//
// There is no such restriction here. SurfNative::GetDC() returns a recording
// DC from Src/Orbiter/Linux/Gdi.cpp for any surface, render target or not --
// the finding is already recorded at the top of VulkanSurface.h, which is
// where pDX7/CreateDX7/DX7Sync went for the same reason. So the two paths
// become one, and GetGDICache, the StretchRect-back and the UpdateSurface-back
// go with the restriction that made them necessary.
//
// WHAT THAT COSTS, STATED PLAINLY: a recording DC records draw commands and
// does not rasterise into the surface's pixels, so a StretchBlt through it
// puts nothing into the image. The bitmap copy is recorded on the DC for
// whoever replays it. This is the same limitation Scene.cpp's GDI-overlay
// clear ran into, and it is a property of the display-list GDI, not of this
// function.
//
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
	// A DIAGNOSTIC THAT SEGFAULTS IS WORSE THAN NO DIAGNOSTIC, AND THIS ONE DID.
	//
	// LogSpecs() reads name, Flags and pResource straight off the SurfNative
	// and hands pResource to NatDumpResource, which dereferences it. Given a
	// DANGLING handle that is a wild read, and the crash lands inside the
	// error reporter -- so the failure it was called to explain is never
	// written, the log ends mid-sentence, and the user gets a bare SIGSEGV.
	//
	// MEASURED, 2026-09-10. The DG's virtual-cockpit coolant readout blitted
	// an 8x11 glyph and clbkScaleBlt fell through to here. The core shows the
	// TARGET healthy (name "DG\blittgt1.dds") and the SOURCE not a surface at
	// all: its `name` reads
	//
	//     08 00 0A 00  0A 00 0C 00  0C 00 0E 00 ... 01 00 00 00 02 00 ...
	//
	// -- little-endian 16-bit 8,10,10,12,12,14 then 0,1,2, 1,3,2, 2,3,4:
	// an INDEX BUFFER. The surface had been freed and its memory reused for
	// mesh indices, so every field LogSpecs read was index data, and
	// pResource came out as 0x22001400220020.
	//
	// SurfaceCatalog is exactly the question to ask, and asking it here is
	// the reference's own idiom, not an invention: D3D9Client.cpp:2312 gates
	// `delete SURFACE(surf)` on `SurfaceCatalog.count(...)` for the same
	// reason, and VulkanClient.cpp:2900 carries that across. Every live
	// SurfNative inserts itself at construction and erases at destruction.
	//
	// The dangling handle is a REAL DEFECT and is not fixed by this; what is
	// fixed is that it now names itself instead of killing the process on the
	// way to being reported.
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



// =======================================================================
// GDI functions
// =======================================================================
//
// THE NULL-SURFACE BRANCH LOSES ITS IMAGE. On Windows a NULL surface with
// GDIOverlay enabled meant "give me a DC on the scene's GDI overlay buffer",
// and the first caller of the frame cleared that buffer to the colour key
// through the same DC.
//
// Neither half survives as written. GetDC on a VulkanTexture does not exist:
// Src/Orbiter/Linux/Gdi.cpp is a display-list recorder, so a DC is not tied to
// an image at all -- the finding is recorded at the top of VulkanSurface.h.
// And the clear has already moved: Scene's constructor clears GBUF_GDI with
// ClearImage, which is what four GDI calls were doing the long way round.
//
// So bGDIClear has nothing to guard and the buffer has no DC to hand out.
// What is left is a recording DC, which is what every other GetDC in this
// client returns, so a module that draws an overlay gets a valid DC and its
// commands go onto the display list rather than into GBUF_GDI. That is the
// honest state of the GDI overlay under a recording GDI, and it is stated
// here rather than hidden behind a NULL return.

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
		// The counterpart of pGDI->ReleaseDC(hDC): the DC above was created
		// here and is deleted here.
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

// =======================================================================
// THE THREE ImGui CALLBACKS BELONG TO THE CORE NOW, and this is note 3 in the
// file header.
//
// On Windows the CLIENT owned the ImGui backends: it called
// ImGui_ImplDX9_Init in clbkImGuiInit, ImGui_ImplDX9_NewFrame each frame, and
// ImGui_ImplDX9_RenderDrawData inside its own BeginScene/EndScene.
//
// Src/Orbiter/Linux/UIHost.cpp does all three, with imgui_impl_glfw and
// imgui_impl_vulkan, and it does them around the same frame that calls this
// client's scene callback -- ImGui::NewFrame before, ImGui::Render and
// ImGui_ImplVulkan_RenderDrawData after. A second NewFrame or Render from
// here would not be a second UI; it would be an assertion inside ImGui.
//
// What the client still owns is the LAST part of clbkImGuiRenderDrawData:
// releasing the surfaces it protected for ImGui's use during the frame. That
// is the client's own bookkeeping, nothing to do with a backend, and it is
// kept.
//
// The multi-viewport block goes with the backend. UpdatePlatformWindows and
// RenderPlatformWindowsDefault drive per-viewport backends that only the
// backend owner can provide.

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
	// ImGui_ImplDX9_Init(pDevice) has no counterpart: UIHost.cpp initialises
	// imgui_impl_glfw and imgui_impl_vulkan before this module loads. See the
	// note above clbkImGuiNewFrame.
}

void VulkanClient::clbkImGuiShutdown()
{
	_TRACE;
	// Clean up also here just in case
	for(auto &surf: ImTextures) {
		clbkReleaseSurface(surf);
	}
	ImTextures.clear();
	// ImGui_ImplDX9_Shutdown() goes with the Init above -- UIHost.cpp shuts
	// its own backends down.
}

// =======================================================================
// AN ImGui TEXTURE ID IS A DESCRIPTOR SET HERE, NOT A TEXTURE POINTER.
//
// The Windows body returns the IDirect3DTexture9* itself, because
// imgui_impl_dx9 takes exactly that as an ImTextureID. imgui_impl_vulkan
// takes a VkDescriptorSet, which has to be ALLOCATED from the backend's own
// pool -- so the client cannot mint one, and must not, since it does not own
// the backend.
//
// Src/Orbiter/Linux/UIHost.cpp exports orbiter_ImGuiTextureFromView for this,
// and caches by image view because ImGui_ImplVulkan_AddTexture allocates a
// set per call and this is asked once per icon per frame. Returning a
// texture pointer allocated nothing, which is why the reference needs no such
// cache and this does.
//
// The per-frame reference hold is kept as the reference has it: the surface
// is pushed onto ImTextures and its count raised, and clbkImGuiRenderDrawData
// lets go at the end of the frame. It guards the same thing it always did --
// a surface released mid-frame outliving the draw that names it.

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


// =======================================================================
// THE SPLASH SCREEN IS THE PLACE THE CONVERSION HURTS MOST, so what survives
// and what does not is stated here rather than three times below.
//
// The Windows pair works like this: two D3D9 offscreen plain surfaces are
// created, the splash image is decoded into the larger one, GDI TextOut writes
// the build strings and the loading status ONTO those surfaces through
// GetDC/ReleaseDC, and both are StretchRect'd onto the back buffer and
// Present()ed -- outside the normal frame loop, because the simulation has not
// started and there is no frame loop yet.
//
// Three of those five steps have no counterpart:
//
//   THE GDI TEXT DOES NOT LAND IN THE IMAGE. Src/Orbiter/Linux/Gdi.cpp is a
//   display-list RECORDER: GetDC returns a DC that collects draw commands,
//   and nothing rasterises them into a VkImage. The TextOut/MoveToEx/LineTo
//   calls are kept -- they are correct, and they reach the core's own overlay
//   when it replays the list -- but they do not write pixels here.
//
//   THE BLIT TO THE BACK BUFFER CANNOT HAPPEN. pBackBuffer is an attachment
//   proxy with no VkImage; the swapchain images belong to UIHost.cpp.
//
//   THE PRESENT CANNOT HAPPEN. GetSwapChain(0)->Present is the one call this
//   process has exactly one of, in UIHost.cpp's presentFrame.
//
// AND THE CORE ALREADY DRAWS A SPLASH SCREEN. UIHost.cpp has drawSplash() and
// orbiter_ClearSplash(), which is what the user actually sees while loading.
// So these two functions keep the state they are asked to keep -- the two
// surfaces, the strings, the layout rectangle -- and stop where the display
// belongs to someone else, rather than pretending to present.

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

		// The two StretchRects onto the back buffer and the swap-chain
		// Present go with the back buffer that has no image and the present
		// this client does not own; see the note above.

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
	// device, and there is no frame open to clear -- this runs before the
	// simulation's first frame, and the core's render pass has not begun.

	// CreateOffscreenPlainSurface(w, h, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT)
	// becomes NatCreateSurface with the flags that say the same thing: a
	// plain surface the CPU can reach (SYSMEM) and take a DC on (GDI), in
	// the client's 32-bit XRGB format. The "offscreen plain" resource TYPE
	// has no counterpart -- there is one image type here -- so what is left
	// of it is the usage.
	const DWORD splashFlags = OAPISURFACE_PF_XRGB | OAPISURFACE_SYSMEM | OAPISURFACE_GDI;

	pTextScreen   = SURFACE(NatCreateSurface(loadd_w, loadd_h, splashFlags));
	pSplashScreen = SURFACE(NatCreateSurface(viewW, viewH, splashFlags));

	if (!pTextScreen || !pSplashScreen) {
		LogErr("SplashScreen: could not create the splash surfaces");
		return;
	}
	pTextScreen->SetName("SplashText");
	pSplashScreen->SetName("SplashScreen");

	// D3DXGetImageInfoFromFile followed by D3DXLoadSurfaceFromFile becomes one
	// decode plus one blit. D3DX could scale into a destination rectangle as
	// part of loading; here the image is decoded to its own size and
	// BlitTexture scales it into the same rectangle, which is the same two
	// operations with the seam in a different place. THE FIT ARITHMETIC IS
	// UNCHANGED, including the two branches' different reference sizes.
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

		// D3DXLoadSurfaceFromFileInMemory becomes NatCreateTextureFromMemory;
		// see VulkanSurface.cpp. The comment below is the author's and is
		// still the assumption the arithmetic makes.
		//
		// Splash screen image is 1920 x 1200 pixel
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

	// ColorFill(pSplashScreen, NULL, black) -- SurfNative::Fill is its
	// counterpart; see VulkanFrame.h on why a whole-image fill and a
	// sub-rectangle fill are two different Vulkan calls.
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
		// "D3D9Client" becomes "VulkanClient", and the "via D3D9on12 emulator"
		// suffix goes with Config->Enable9On12; see note 4 in the file header.
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

		// DWORD VPOS = viewH - 50; and DWORD LSPACE = 20; stood here, both
		// assigned and never read. Kept as a comment rather than as two
		// variables GCC would warn about; they mark where a third block of
		// text used to go.

		SelectObject(hDC, hO);
		DeleteObject(hF);

		pSplashScreen->ReleaseDC(hDC);
	}

	// The two StretchRects and the Present that ended the Windows function are
	// gone with the back buffer that has no image and the present this client
	// does not own. The core draws the loading screen; see the note above
	// OutputLoadStatus. What this function has produced -- the two surfaces,
	// with the image in the larger one -- is what clbkDisplayFrame draws
	// through the Sketchpad while !bRunning.
}


// =======================================================================
// BeginScene/EndScene HAVE NO DEVICE CALL BEHIND THEM ANY MORE.
//
// IDirect3DDevice9::BeginScene told the runtime that draw calls were coming
// and EndScene that they had stopped; a draw outside the pair was an error.
// Vulkan's equivalent is being inside a render pass on a recording command
// buffer, and the core has already begun both by the time the client's scene
// callback runs -- see note 1 in the file header.
//
// bRendering IS THE PART THAT MATTERS AND IT STAYS. IsInScene() is read by
// the Sketchpad and by the surface code to decide whether a draw is legal
// right now, which is exactly the question the flag was always answering.
// What changes is that it is set unconditionally rather than from an HRESULT,
// because there is no call left to fail. S_OK is returned for the same
// reason.

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

// =======================================================================
// 2D Drawing Interface
//
// The thread guard is the reference's, unchanged. GetCurrentThread() returns
// the pseudo-handle (HANDLE)-2 on every thread, here as on Windows, so this
// comparison is a constant against itself and the guard has never fired and
// cannot. It is carried rather than repaired -- see the note at
// VulkanClient::GetMainThread().
//
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

		// The pad has just dirtied level 0, which is the other moment
		// D3DUSAGE_AUTOGENMIPMAP fires on Windows -- see the note on
		// AutoGenMips above clbkScaleBlt. It is a no-op for every surface the
		// core currently draws on with a pad (the VC HUD is SKETCHPAD|TEXTURE
		// and an MFD's `surf` carries no mipmaps either; only an MFD's `tex`
		// does, and that one is written by clbkBlt), but the flag does not
		// distinguish how level 0 got dirty and neither should this.
		AutoGenMips(hSrf);

		// What the pad actually left in the VC HUD surface, identified by
		// asking for it rather than by guessing at its size -- the VC MFDs are
		// the same 512x512.
		//
		// AFTER PopRenderTargets, AND THE FIRST ATTEMPT AT THIS WAS BEFORE IT
		// AND MEASURED NOTHING. EndDrawing only flushes the pad's geometry
		// into the offscreen command buffer; it is PopRenderTargets that ends
		// the pass and submits it. Reading the image between the two reads it
		// before anything has executed -- which comes back all zeroes and
		// looks exactly like "the sketchpad drew nothing".
		{
			static const bool bDumpSkp = (getenv("ORBITER_VK_DUMP_SKP") != NULL);
			if (bDumpSkp) {
				const VCHUDSPEC *spec = NULL;
				SURFHANDLE hHud = GetVCHUDSurface(&spec);
				// The VC MFDs go through exactly the same offscreen-sketchpad
				// path, so dumping one alongside the HUD says whether the
				// path is broken or only the HUD's content is missing.
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
						// BOTH FORMATS, because the PNG is ambiguous: an
						// all-zero surface is RGBA(0,0,0,0), which a viewer
						// shows as transparent and reads as white. The JPEG
						// drops alpha, so it shows the colour that is
						// actually there.
						if (pT) {
							NatSaveSurface("vchud.png", pT);
							NatSaveSurface("vchud.jpg", pT);

							// And the numbers, so neither picture has to be
							// interpreted: how much of the surface is not
							// black, and what the extremes are.
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
