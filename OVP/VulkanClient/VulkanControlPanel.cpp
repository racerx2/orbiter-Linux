
// ===========================================================================================
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2012-2026 Jarmo Nikkanen
// ===========================================================================================
//
// The debug statistics overlay: a full-screen Sketchpad page of surface, mesh
// and tile counters, and a stacked bar showing where the frame's time went.
// ===========================================================================================


#include "Scene.h"
#include "VulkanFrame.h"
#include "VulkanUtil.h"
#include "VulkanConfig.h"
#include "VulkanClient.h"
#include "VulkanSurface.h"
// Not in the Windows include list. DrawTimeBar builds a VulkanPadBrush, which
// on Windows arrived transitively through D3D9Surface.h -> D3D9Pad.h; the
// converted VulkanSurface.h forward-declares VulkanPad instead, because that
// include cycle only survives on MSVC's guards.
#include "VulkanPad.h"
#include "VulkanCatalog.h"
#include "Mesh.h"
#include "psapi.h"
#include "DebugControls.h"
#include <sstream>
#include <string>

using namespace oapi;


// -------------------------------------------------------------------------------------------
// GetLevelDesc(0, &desc) asked the runtime what the texture is; a VkImage
// answers no such question, so the client's own record is read instead.
//
// The two ARGB lines become one. D3DFMT_A8R8G8B8 and D3DFMT_X8R8G8B8 are
// different D3D formats but one Vulkan format, VK_FORMAT_B8G8R8A8_UNORM,
// because "X8" only meant "alpha present, ignored". Renaming both lines
// mechanically would leave two identical tests, shifting a 32-bit surface
// left twice: sixteen bytes per pixel instead of four.
//
// D3DFMT_A4R4G4B4 is dropped rather than mapped -- nothing in this client
// produces it. The remaining spellings match SurfNative::StaticFormatSizeInBytes
// on purpose; two size tables that disagree is how this file would rot.
// -------------------------------------------------------------------------------------------

DWORD TextureSizeInBytes(VulkanTexture *pTex)
{
	const VulkanImageDesc &desc = pTex->Desc();
	DWORD lev = pTex->Mips();
	DWORD size = desc.Height*desc.Width;
	if (desc.Format==VK_FORMAT_BC1_RGBA_UNORM_BLOCK) size=size>>1;
	if (desc.Format==VK_FORMAT_B8G8R8A8_UNORM) size=size<<2;		// A8R8G8B8 and X8R8G8B8
	if (desc.Format==VK_FORMAT_R5G6B5_UNORM_PACK16) size=size<<1;
	if (desc.Format==VK_FORMAT_R8G8B8_UNORM) size=size*3;

	if (lev) size += (size>>2) + (size>>4) + (size>>8);
	return size;
}


void VulkanClient::Label(const char *format, ...)
{
	char buffer[256];
	va_list args;
	va_start(args, format);
			
	_vsnprintf_s(buffer, 255, 255, format, args);

	va_end(args);

	int len = lstrlen(buffer);
	pItemsSkp->Text(20, LabelPos, buffer, len);
	LabelPos += 22;
}

double Get(VulkanTime &t)
{
	return t.time / max(1.0, min(1000.0, t.count));
}

void Reset(VulkanTime &t)
{
	t.count = t.time = t.peak = 0.0;
}

void VulkanClient::DrawTimeBar(double t, double s, double f, DWORD color, const char *label)
{
	static double x = 8;
	static int z = 100;
	char legend[256];

	if (color == 0) {
		x = 8;
		z = 100;
		return;
	}

	sprintf_s(legend, 256, "%.64s, %0.2fms (%.2f%%)", label, (t/f)*0.001, t*0.0001);

	VulkanPadBrush brush(color);
	int y = viewH - 20;
	int q = viewW - 640;
	pItemsSkp->SetBrush(&brush);
	pItemsSkp->Rectangle(int(x), y, int(x + t*s), y + 16);
	pItemsSkp->Rectangle(q, z, q + 32, z + 16);
	pItemsSkp->Text(q+40, z, legend, -1);
	pItemsSkp->SetBrush(NULL);
	x += t*s;
	z += 32;
}

void VulkanClient::RenderControlPanel()
{
	static std::map<DWORD, DWORD> TileBuf;
	// Two label tables nothing in the function reads. GCC reports them under
	// -Wunused-variable; MSVC does not. Commented out in place rather than
	// deleted, so the reference's own lines stay visible.
	// static const char *OnOff[]={"Off","On"};
	// static const char *SkpU[]={"Auto","GDI"};

	VulkanDevice *dev = pDevice;

	// Src/Orbiter/Linux/psapi.h already answered GetProcessMemoryInfo, but
	// carried only the plain counters structure. PrivateUsage is an _EX field,
	// so the extended form was added there for this call.
	PROCESS_MEMORY_COUNTERS_EX memstats;
	memstats.cb = sizeof(PROCESS_MEMORY_COUNTERS_EX);
	GetProcessMemoryInfo(GetCurrentProcess(), (PPROCESS_MEMORY_COUNTERS)&memstats, sizeof(memstats));


	
	pItemsSkp = oapiGetSketchpad(GetBackBufferHandle());

	oapi::Pen * pen    = oapiCreatePen(1, 10, 0xFFFF00);
	oapi::Pen * nullp  = oapiCreatePen(0, 1,  0xFFFFFF);
	oapi::Brush *brush = oapiCreateBrush(0xA0000000);
	oapi::Font *largef = oapiCreateFont(38,false,(char*)"Arial", FONT_BOLD);
	oapi::Font *smallf = oapiCreateFont(22,false,(char*)"Fixed");

	pItemsSkp->SetPen(nullp);

	pItemsSkp->SetBrush(brush);
	pItemsSkp->Rectangle(-1, -1, viewW+1, viewH+1);
	
	
	pItemsSkp->SetTextColor(0x00FF00);
	pItemsSkp->SetFont(largef);
	// Sketchpad::Text takes an explicit character count: 21 for "D3D9Client
	// Statistics", 23 for the new name. Leaving the 21 would silently print
	// "VulkanClient Statisti".
	pItemsSkp->Text(20,70,"VulkanClient Statistics",23);
	pItemsSkp->SetFont(smallf);
	LabelPos = 130;
	
	// Zeroed and never touched again -- the loop below counts plain textures
	// into textr_*, not these. Commented out for -Wunused-variable.
	// DWORD plain_count = 0, plain_size = 0;
	DWORD textr_count = 0, textr_size = 0;
	DWORD rendt_count = 0, rendt_size = 0;
	DWORD rttex_count = 0, rttex_size = 0;
	DWORD dyntx_count = 0, dyntx_size = 0;
	DWORD sysme_count = 0, sysme_size = 0;
	DWORD duall_count = 0, duall_size = 0;

	size_t nSurf = SurfaceCatalog.size();

	for (auto pSurf : SurfaceCatalog)
	{
		// Was desc.Pool == D3DPOOL_DEFAULT, "does this live in device memory".
		// DEFAULT and SYSTEMMEM are the only pools the client ever asks for,
		// so the enumeration is one boolean here and the test is its negation.
		if (!pSurf->desc.HostVisible) {
			if (pSurf->IsRenderTarget()) {	
				if (pSurf->IsTexture()) {
					rttex_count++;
					rttex_size += pSurf->GetSizeInBytes();
				}
				else {
					rendt_count++;
					rendt_size += pSurf->GetSizeInBytes();
				}
			}
			else {
				textr_count++;
				textr_size += pSurf->GetSizeInBytes();
			}
		}
		else {
			sysme_count++;
			sysme_size += pSurf->GetSizeInBytes();
		}
	}

	Label("Application Size.....: %lu MB", memstats.PrivateUsage >> 20);
	// Was dev->GetAvailableTextureMem(), which reported *free* video memory.
	// Core Vulkan has no such query, so this is the total device-local heap
	// size -- a different number, and the label now says so.
	Label("Video memory (total).: %u MB", DWORD(dev->GetLocalMemorySize()>>20));
	Label("Surface Handles......: %lu", nSurf);
	Label("SystemMem Surfaces...: %u (%u MB)", sysme_count, sysme_size>>20);
	Label("Dynamic Textures.....: %u (%u MB)", dyntx_count, dyntx_size>>20);
	Label("Render Targets.......: %u (%u MB)", rendt_count, rendt_size>>20);
	Label("Render Textures......: %u (%u MB)", rttex_count, rttex_size>>20);
	Label("Dual Layer Surfaces..: %u (%u MB)", duall_count, duall_size>>20);
	Label("Plain Textures.......: %u (%u MB)", textr_count, textr_size>>20);
	
	LabelPos += 22;

	size_t tt_c = g_pTexmgr_tt->UsedSize() + g_pTexmgr_tt->FreeSize();
	size_t tv_c = g_pVtxmgr_vb->UsedSize() + g_pVtxmgr_vb->FreeSize();

	std::stringstream tiles{};

	for (auto& x : VulkanStats.TilesRendered) {
		if (x.second != 0) TileBuf[x.first] = x.second;
		x.second = 0;
	}

	if (VulkanStats.TilesRendered.size()) {
		if (TileBuf.count(RENDERPASS_MAINSCENE)) tiles << "Main[" << TileBuf[RENDERPASS_MAINSCENE] << "] ";
		if (TileBuf.count(RENDERPASS_CUSTOMCAM)) tiles << "Cams[" << TileBuf[RENDERPASS_CUSTOMCAM] << "] ";
		if (TileBuf.count(RENDERPASS_ENVCAM)) tiles << "Env[" << TileBuf[RENDERPASS_ENVCAM] << "] ";
	}

	Label("Tile Texture Cache...: Used[%u] Free[%u] Capacity (%u MB)", g_pTexmgr_tt->UsedCount(), g_pTexmgr_tt->FreeCount(), tt_c >> 20);
	Label("Tile Vertex Cache....: Used[%u] Free[%u] Capacity (%u MB)", g_pVtxmgr_vb->UsedCount(), g_pVtxmgr_vb->FreeCount(), tv_c >> 20);
	Label("Tiles Allocated......: %u", VulkanStats.TilesAllocated);
	Label("Tiles Renderred......: %s", tiles.str().c_str());
	
	DWORD tot_verts = 0;
	DWORD tot_trans = 0;
	DWORD tot_group = 0;

	for (auto pMesh : MeshCatalog)
	{
		if (pMesh) {
			tot_verts += pMesh->GetVertexCount();
			tot_trans += pMesh->GetGroupTransformCount();
			tot_group += pMesh->GetGroupCount();
		}
	}

	static DWORD matchg = 0, texchg = 0;
	static DWORD verts = 0, grps = 0, meshes = 0;
	static double DCPeak = 0.0;
	static double LockPeak = 0.0;

	LabelPos += 22;
	Label("Mesh Vtx Allocated...: %u (%u MB)", tot_verts, (tot_verts*sizeof(NMVERTEX))>>20); 
	Label("Groups Allocated.....: %u", tot_group);
	Label("Group Tarnsforms.....: %u", tot_trans); 
	Label("Mesh vertices render.: %u", verts);
	Label("Mesh groups rendered.: %u", grps);
	Label("Meshes rendered......: %u", meshes);
	Label("Texture changes......: %u", texchg);
	Label("Material changes.....: %u", matchg);
	Label("GetDC peak time......: %0.2fms", DCPeak*0.001);
	Label("Lock wait peak time..: %0.2fms", LockPeak*0.001);

	if (DebugControls::IsActive()) {

		if (DebugControls::IsSelectedGroupRendered()) Label("Group is rendered....: Yes");
		else Label("Group is rendered....: No");

		vObject *vObj = DebugControls::GetVisual();
		if (vObj) {
			DWORD nMesh = vObj->GetMeshCount();
			for (DWORD i = 0; i < nMesh; i++) {
				VulkanMesh *hMesh = static_cast<VulkanMesh *>(vObj->GetMesh(i));
				hMesh->ResetRenderStatus();
			}
		}
	}
	
	// DRAW TIME LINE ------------------------------------------------------------------

	static double systime = 0.0;
	static double frames = 1.0;
	static double lock;
	static double blit;
	static double getdc;
	static double scene;
	static double scale;
	static double outside;
	static double update;
	static double display;
	// -------------------------------------
//	static double pln_srf;
//	static double pln_cld;
	// -------------------------------------
//	static double scn_pst;
//	static double scn_ves;
//	static double scn_vc;
//	static double scn_hud;
//	static double scn_cam;
	// -------------------------------------
//	static double prt_cam;
//	static double prt_env;
//	static double prt_blr;
	
	if (oapiGetSysMJD() > systime) {
		
		systime = oapiGetSysMJD() + 0.8 / 86400.0;

		frames = VulkanStats.Timer.Scene.count;
		double iframes = 1.0 / frames;

		matchg = DWORD(double(VulkanStats.Mesh.MtrlChanges) * iframes);
		texchg = DWORD(double(VulkanStats.Mesh.TexChanges) * iframes);
		verts = DWORD(double(VulkanStats.Mesh.Vertices) * iframes);
		grps = DWORD(double(VulkanStats.Mesh.MeshGrps) * iframes);
		meshes = DWORD(double(VulkanStats.Mesh.Meshes) * iframes);
		DCPeak = VulkanStats.Timer.GetDC.peak;
		LockPeak = VulkanStats.Timer.LockWait.peak;

		double total = VulkanStats.Timer.FrameTotal.time;

		scene   = VulkanStats.Timer.Scene.time;
		update  = VulkanStats.Timer.Update.time;
		display = VulkanStats.Timer.Display.time;
		lock    = VulkanStats.Timer.LockWait.time;
		blit    = VulkanStats.Timer.BlitTime.time;
		getdc	= VulkanStats.Timer.GetDC.time;

		// Time spend outside of client
		outside = total - (scene + update + display + lock + blit + getdc);

		scale   = double(viewW - 16) / total;

		// -------------------------------------
		Reset(VulkanStats.Timer.CamVis);
		Reset(VulkanStats.Timer.Scene);
		Reset(VulkanStats.Timer.Update);
		Reset(VulkanStats.Timer.Display);
		Reset(VulkanStats.Timer.LockWait);
		Reset(VulkanStats.Timer.BlitTime);
		Reset(VulkanStats.Timer.GetDC);
		Reset(VulkanStats.Timer.FrameTotal);
		// -------------------------------------
		Reset(VulkanStats.Timer.HUDOverlay);
		// -------------------------------------
		memset(&VulkanStats.Mesh, 0, sizeof(VulkanStats.Mesh));
	}
	

	DrawTimeBar(0, 0, 0, 0);
	DrawTimeBar(outside, scale, frames, 0x774411, "Non-client specific tasks");
	DrawTimeBar(blit,    scale, frames, 0xFF5555, "Time used in Bliting");
	DrawTimeBar(getdc,	 scale, frames, 0x33FFFF, "Time used in GetDC");
	DrawTimeBar(lock,    scale, frames, 0x0066FF, "Waiting Locks (CPU-IDLE)");
	DrawTimeBar(display, scale, frames, 0x000099, "Waiting Present (CPU-IDLE)");
	DrawTimeBar(scene,   scale, frames, 0x00CC00, "Drawing Scene");

	//------------------------------
	/*DrawTimeBar(scn_cam, scale, frames, 0x000055, "Visual updates");
	DrawTimeBar(pln_srf, scale, frames, 0x005500, "Planet's surface");
	DrawTimeBar(pln_cld, scale, frames, 0x55FF55, "Planet's cloud layer");
	DrawTimeBar(scn_ves, scale, frames, 0xFF0000, "Vessel objects");
	DrawTimeBar(scn_vc,  scale, frames, 0xFFFF00, "Virtual cockpit");
	DrawTimeBar(scn_hud, scale, frames, 0x777700, "HUD and 2D panels");
	DrawTimeBar(scn_pst, scale, frames, 0xFF00FF, "Post-processing effects");
	//------------------------------
	DrawTimeBar(prt_cam, scale, frames, 0x00FFFF, "Custom cameras");
	DrawTimeBar(prt_env, scale, frames, 0x007777, "Environment map");
	DrawTimeBar(prt_blr, scale, frames, 0x004444, "EnvMap blur");
	//-------------------------------
	DrawTimeBar(scene,   scale, frames, 0x777777, "Other rendering tasks");*/
	
	pItemsSkp->SetPen(NULL);
	pItemsSkp->SetBrush(NULL);

	oapiReleaseFont(smallf);
	oapiReleaseFont(largef);
	oapiReleasePen(pen);
	oapiReleasePen(nullp);
	oapiReleaseBrush(brush);

	oapiReleaseSketchpad(pItemsSkp);
}



bool VulkanClient::ControlPanelMsg(WPARAM wParam)
{
	return false;
}
