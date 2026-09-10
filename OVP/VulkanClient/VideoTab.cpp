// ==============================================================
// Class VideoTab (implementation)
// Manages the user selections in the "Video" tab of the Orbiter
// Launchpad dialog.
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2006-2026 Martin Schweiger
//				 2010-2016 Jarmo Nikkanen (D3D9Client implementation)
// ==============================================================
//
// CONVERTED FROM OVP/D3D9Client/VideoTab.cpp, read end to end (1051 lines).
//
// THIS FILE IS A WIN32 DIALOG that happens to ask a graphics API three
// questions. Nine tenths of it -- SendDlgItemMessage, EnableWindow,
// GetWindowText, the sliders, the check boxes, the whole of SaveSetupState --
// is Win32, not Direct3D, and crosses unchanged against
// Src/Orbiter/Linux/windows.h. What changed:
//
//  1. g_pD3DObject IS GONE, and it is the thing this file was built around.
//     Direct3DCreate9() gave the Windows client an object it could enumerate
//     hardware with BEFORE any device existed, which is exactly what a
//     Launchpad tab needs. Its three uses split into two different Linux
//     answers, because Vulkan and the windowing system answer different halves
//     of what D3D9 answered alone:
//
//       ADAPTERS -- GetAdapterCount / GetAdapterIdentifier -> the core's
//       VkInstance (orbiter_GetVulkanContext, which is idempotent and safe to
//       call before the Launchpad has drawn a frame), then
//       vkEnumeratePhysicalDevices and vkGetPhysicalDeviceProperties. The
//       device name is deviceName, which is what Description was.
//
//       DISPLAY MODES -- GetAdapterDisplayMode / GetAdapterModeCount /
//       EnumAdapterModes -> orbiter_GetCurrentVideoMode /
//       orbiter_GetVideoModeCount / orbiter_GetVideoMode. Vulkan does not
//       enumerate display modes at all; that is a window-system question, and
//       UIHost.cpp publishes these three FOR THIS TAB.
//
//       CAPS -- GetDeviceCaps().MaxAnisotropy ->
//       VkPhysicalDeviceLimits::maxSamplerAnisotropy;
//       CheckDeviceMultiSampleType -> limits.framebufferColorSampleCounts.
//
//     The D3DFMT_X8R8G8B8 argument to the mode queries disappears rather than
//     being translated: it asked for the modes available in one back-buffer
//     format, and the swapchain format here is chosen by the core, not by
//     this tab.
//
//  2. THE MODE LIST IS PER-MONITOR, NOT PER-ADAPTER. SelectAdapter re-lists
//     the modes when the user picks a different GPU, and on Linux it gets the
//     same list back, because glfwGetVideoModes asks the MONITOR. That is not
//     a loss: the modes a monitor accepts do not depend on which GPU drives
//     it. The loop is kept as written so the combo is still rebuilt and
//     reselected.
//
//  3. TWO WIN32 FILE APIS BECOME PORTABLE ONES, on precedents this conversion
//     already set. CreateFile/GetFileSize/ReadFile/CloseHandle in
//     InitCreditsDialog become stdio -- the same decision D3D9Util.cpp's
//     shader cache took, and the reason is the same: the shim implements no
//     kernel file handles and defines no INVALID_HANDLE_VALUE.
//     FindFirstFileA/FindNextFileA/FindClose in ScanAtmoCfgs become
//     std::filesystem::directory_iterator with a case-insensitive suffix
//     test, which is exactly what vPlanet::EnumerateDirectory became.
//
//  4. FOUR MORE WINDOWS PATHS (finding 24). "GC\\" twice, the scenario path's
//     trailing-backslash trim and its separator, and
//     "Modules/D3D9Client/Credits.rtf" -- which was already forward-slashed
//     and only needed the client's rename.
//
//  5. TWO ADDITIONS TO THE SHIM, both pure window-tree walks over state
//     Win32Dlg.cpp already keeps: GetAncestor and EnumChildWindows. See
//     UpdateConfigData, and the core section of the ledger.
//
//  6. THE CREDITS BOX WILL BE EMPTY, and that is recorded rather than worked
//     around: EM_SETTEXTEX now exists in the shim's richedit.h so this file
//     compiles as written, but Win32Dlg.cpp implements no rich edit control,
//     so the message is not answered. Substituting WM_SETTEXT would put raw
//     RTF markup on screen, which is worse than blank.
// ==============================================================

#include "VulkanClient.h"
#include "VideoTab.h"
#include "resource.h"
#include "resource_video.h"
#include "VideoTab.h"
#include "AABBUtil.h"
#include "VulkanConfig.h"
#include "Commctrl.h"
#include "OapiExtension.h"
#include <vector>
#include <sstream>
#include <richedit.h>
// <filesystem> for ScanAtmoCfgs, <algorithm> and <cctype> for the
// case-insensitive suffix test that replaces FindFirstFile's wildcard.
#include <filesystem>
#include <algorithm>
#include <cctype>

using namespace oapi;

const UINT IDC_SCENARIO_TREE = (oapiGetOrbiterVersion() >= 111105) ? 1090 : 1088;

BOOL CALLBACK EnumChildProc(HWND hwnd, LPARAM lParam)
{
	if (GetDlgItem(hwnd, IDC_SCENARIO_TREE)) {
		*(HWND*)lParam = hwnd; 
		return false;
	}
	return true;
}


// ==============================================================
// Constructor

VideoTab::VideoTab(VulkanClient *gc, HINSTANCE _hInst, HINSTANCE _hOrbiterInst, HWND hVideoTab)
{
	gclient      = gc;
	hInst        = _hInst;
	hOrbiterInst = _hOrbiterInst;
	hTab         = hVideoTab;
	aspect_idx	 = 0;
	SelectedAdapterIdx = 0;
}

VideoTab::~VideoTab()
{
	
}


// ==============================================================
// The two hardware queries that replace g_pD3DObject.
//
// They are file-static rather than members because they answer questions
// about the machine, not about this dialog, and because the Windows original
// asked them through a global for the same reason.
//
// GetPhysicalDevices() is GetAdapterCount() + GetAdapterIdentifier() in one:
// D3D9 numbered its adapters and handed back a description string per index,
// which is what vkEnumeratePhysicalDevices plus vkGetPhysicalDeviceProperties
// gives. The VkInstance is the CORE's -- the client creates none, and
// orbiter_GetVulkanContext is documented as callable before the Launchpad has
// drawn a frame, which is precisely this case.
// ==============================================================

static bool GetPhysicalDevices(std::vector<VkPhysicalDevice> &out)
{
	out.clear();

	OrbiterVulkanContext ctx;
	memset(&ctx, 0, sizeof(ctx));
	if (!orbiter_GetVulkanContext(&ctx)) return false;
	if (!ctx.instance) return false;

	VkInstance inst = (VkInstance)ctx.instance;

	uint32_t count = 0;
	if (vkEnumeratePhysicalDevices(inst, &count, NULL) != VK_SUCCESS) return false;
	if (count == 0) return false;

	out.resize(count);
	if (vkEnumeratePhysicalDevices(inst, &count, out.data()) != VK_SUCCESS) {
		out.clear();
		return false;
	}
	return true;
}


// ==============================================================
// Dialog message handler

BOOL VideoTab::WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	GraphicsClient::VIDEODATA *data = gclient->GetVideoData();

	switch (uMsg) {

	case WM_INITDIALOG:
	{
		return TRUE;
	}

	case WM_COMMAND:

		switch (LOWORD(wParam)) {

		case IDC_VID_DEVICE:
			if (HIWORD(wParam)==CBN_SELCHANGE) {
				DWORD idx = DWORD(SendDlgItemMessage(hWnd, IDC_VID_DEVICE, CB_GETCURSEL, 0, 0));
				SelectAdapter(idx);
				return TRUE;
			}
			break;

		case IDC_VID_MODE:
			if (HIWORD(wParam) == CBN_SELCHANGE) {
				DWORD idx = DWORD(SendDlgItemMessage (hWnd, IDC_VID_MODE, CB_GETCURSEL, 0, 0));
				SelectMode(idx);
				return TRUE;
			}
			break;

		case IDC_VID_BPP:
			if (HIWORD(wParam) == CBN_SELCHANGE) {
				SelectFullscreen(data->fullscreen);
				return TRUE;
			}
			// FINDING 42: THIS CASE HAS NO break, ON WINDOWS EITHER. Any
			// IDC_VID_BPP notification that is not CBN_SELCHANGE falls into
			// IDC_VID_FULL below, and BN_CLICKED is 0 -- so a combo
			// notification with a zero high word (CBN_ERRSPACE, and
			// CBN_SELENDOK's neighbours) switches the dialog to full screen
			// and sets data->fullscreen = true. Left as written, because
			// adding the break changes what the dialog does; the marker below
			// only tells GCC that the fall-through was seen, since -Wextra
			// reports it and MSVC does not.
			[[fallthrough]];

		case IDC_VID_FULL:
			if (HIWORD(wParam) == BN_CLICKED) {
				SelectFullscreen(true);
				data->fullscreen = true;
				return TRUE;
			}
			break;

		case IDC_VID_WINDOW:
			if (HIWORD(wParam) == BN_CLICKED) {
				SelectFullscreen(false);
				data->fullscreen = false;
				return TRUE;
			}
			break;

		case IDC_VID_WIDTH:
			if (HIWORD(wParam) == EN_CHANGE) {
				SelectWidth ();
				return TRUE;
			}
			break;

		case IDC_VID_HEIGHT:
			if (HIWORD(wParam) == EN_CHANGE) {
				SelectHeight ();
				return TRUE;
			}
			break;

		case IDC_VID_STENCIL:
			return TRUE;
			break;
			
		case IDC_VID_ASPECT:
			if (HIWORD(wParam) == BN_CLICKED) {
				SelectWidth();
				return TRUE;
			}
			break;

		case IDC_VID_4X3:
		case IDC_VID_16X10:
		case IDC_VID_16X9:
			if (HIWORD(wParam) == BN_CLICKED) {
				aspect_idx = LOWORD(wParam) - IDC_VID_4X3;
				SelectWidth();
				return TRUE;
			}
			break;

		case IDC_VID_INFO:
			DialogBoxParamA(hInst, MAKEINTRESOURCE(IDD_D3D9SETUP), hTab, SetupDlgProcWrp, (LPARAM)this);
			return TRUE;
		}
		break;
	}
	return FALSE;
}

// ==============================================================
// Initialise the Launchpad "video" tab

bool VideoTab::Initialise()
{
	// D3DDISPLAYMODE mode, curMode; and D3DADAPTER_IDENTIFIER9 info; stood
	// here. A display mode is three integers and a format, and the format has
	// gone with the query that took it; an adapter identifier was a hundred
	// bytes of driver strings and GUIDs of which one field, Description, was
	// read.
	int curW = 0, curH = 0, curHz = 0;

	GraphicsClient::VIDEODATA *data = gclient->GetVideoData();

	data->forceenum = false;
	data->trystencil = false;

	SendDlgItemMessage(hTab, IDC_VID_DEVICE, CB_RESETCONTENT, 0, 0);
	SendDlgItemMessage(hTab, IDC_VID_MODE, CB_RESETCONTENT, 0, 0);
	SendDlgItemMessage(hTab, IDC_VID_BPP, CB_RESETCONTENT, 0, 0);

	ScanAtmoCfgs();

	char cbuf[32];

	std::vector<VkPhysicalDevice> devices;
	GetPhysicalDevices(devices);
	int nAdapter = int(devices.size());

	if (nAdapter == 0) {
		LogErr("VideoTab::Initialize() No Vulkan devices found");
		FailedDeviceError();
		return false;
	}

	if (data->deviceidx < 0 || (data->deviceidx)>=nAdapter) data->deviceidx = 0;

	for (int i=0;i<nAdapter;i++) {
		// GetAdapterIdentifier(i, 0, &info) -> the device's own properties.
		// deviceName is what Description was: the name the driver reports.
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties(devices[i], &props);
		LogAlw("Adapter %d: %s", i, props.deviceName);
		SendDlgItemMessageA(hTab, IDC_VID_DEVICE, CB_ADDSTRING, 0, (LPARAM)props.deviceName);
	}

	SendDlgItemMessage(hTab, IDC_VID_DEVICE, CB_SETCURSEL, data->deviceidx, 0);


	// GetAdapterDisplayMode(deviceidx, &curMode) -- "what is the desktop
	// showing right now". Vulkan cannot answer that; the window system can,
	// and UIHost.cpp publishes it for exactly this line.
	orbiter_GetCurrentVideoMode(&curW, &curH, &curHz);

	LogAlw("Current Mode W=%u, H=%u", curW, curH);

	// GetAdapterModeCount(deviceidx, D3DFMT_X8R8G8B8). The format argument has
	// no counterpart: it asked which modes the adapter offers in one
	// back-buffer format, and the swapchain format here is the core's choice.
	UINT nModes = UINT(orbiter_GetVideoModeCount());

	if (nModes == 0) {
		LogErr("VideoTab::Initialize() No Display Modes Available");
		FailedDeviceError();
	}

	for (UINT k=0;k<nModes;k++) {
		int mw = 0, mh = 0, mhz = 0;
		orbiter_GetVideoMode(int(k), &mw, &mh, &mhz);
		sprintf_s(cbuf,32,"%u x %u  %uHz", mw, mh, mhz);
		LogAlw("Index:%u %u x %u  %uHz", k, mw, mh, mhz);
		SendDlgItemMessageA(hTab, IDC_VID_MODE, CB_ADDSTRING, 0, (LPARAM)cbuf);
		SendDlgItemMessageA(hTab, IDC_VID_MODE, CB_SETITEMDATA, k, (LPARAM)(mh<<16 | mw));
	}

	SendDlgItemMessageA(hTab, IDC_VID_BPP, CB_ADDSTRING, 0, (LPARAM)"True Full Screen (no alt-tab)");
	SendDlgItemMessageA(hTab, IDC_VID_BPP, CB_ADDSTRING, 0, (LPARAM)"Full Screen Window");
	SendDlgItemMessageA(hTab, IDC_VID_BPP, CB_ADDSTRING, 0, (LPARAM)"Window with Taskbar");
	SendDlgItemMessageA(hTab, IDC_VID_BPP, CB_SETCURSEL, data->style, 0);

	//SetWindowText(GetDlgItem(hTab, IDC_VID_STATIC5), "Resolution");
	SetWindowText(GetDlgItem(hTab, IDC_VID_STATIC6), "Full Screen Mode");


	SendDlgItemMessage(hTab, IDC_VID_MODE, CB_SETCURSEL, data->modeidx, 0);
	SendDlgItemMessage(hTab, IDC_VID_VSYNC, BM_SETCHECK, data->novsync ? BST_CHECKED : BST_UNCHECKED, 0);
		
	SetWindowText(GetDlgItem(hTab, IDC_VID_WIDTH), std::to_string(data->winw).c_str());
	SetWindowText(GetDlgItem(hTab, IDC_VID_HEIGHT), std::to_string(data->winh).c_str());

	aspect_idx = 0;
		
	if (data->winw == (4*data->winh)/3 || data->winh == (3*data->winw)/4)	aspect_idx = 1;
	else if (data->winw == (16*data->winh)/10 || data->winh == (10*data->winw)/16) aspect_idx = 2;
	else if (data->winw == (16*data->winh)/9 || data->winh == (9*data->winw)/16) aspect_idx = 3;
		
	SendDlgItemMessage(hTab, IDC_VID_ASPECT, BM_SETCHECK, aspect_idx ? BST_CHECKED : BST_UNCHECKED, 0);
	if (aspect_idx) aspect_idx--;
	SendDlgItemMessage(hTab, IDC_VID_4X3+aspect_idx, BM_SETCHECK, BST_CHECKED, 0);

	SendDlgItemMessage(hTab, IDC_VID_STENCIL,  BM_SETCHECK, data->trystencil, 0); // GDI Compatibility mode
	SendDlgItemMessage(hTab, IDC_VID_ENUM,     BM_SETCHECK, data->forceenum, 0);  
	SendDlgItemMessage(hTab, IDC_VID_PAGEFLIP, BM_SETCHECK, data->pageflip, 0);	  // Full scrren Window	

	bool bRet = SelectAdapter(data->deviceidx);

	SelectFullscreen(data->fullscreen);

	ShowWindow (GetDlgItem (hTab, IDC_VID_INFO), SW_SHOW);

	SetWindowText(GetDlgItem(hTab, IDC_VID_INFO), "Advanced");

	return bRet;
}


// ==============================================================
// 
void VideoTab::SelectMode(DWORD index)
{
	GraphicsClient::VIDEODATA *data = gclient->GetVideoData();
	SendDlgItemMessage(hTab, IDC_VID_MODE, CB_GETITEMDATA, index, 0);
	data->modeidx = index;
}


// ==============================================================
// Respond to user adapter selection
//
// THE MODE LIST IS THE SAME LIST WHICHEVER ADAPTER IS PICKED, and that is
// correct rather than a shortcut. D3D9 enumerated modes per ADAPTER;
// glfwGetVideoModes enumerates them per MONITOR, and the resolutions a monitor
// accepts do not depend on which GPU is driving it. The rebuild is kept so the
// combo is still reset and reselected, which is what the rest of the dialog
// expects after a device change.
//
bool VideoTab::SelectAdapter(DWORD index)
{

	SelectedAdapterIdx = index; 

	GraphicsClient::VIDEODATA *data = gclient->GetVideoData();

	std::vector<VkPhysicalDevice> devices;

	if (!GetPhysicalDevices(devices)) {
		// Was "Direct3DCreate9 Failed" on a NULL g_pD3DObject. The equivalent
		// failure is the core having no Vulkan instance to hand over.
		LogErr("VideoTab::SelectAdapter(%u) No Vulkan instance available", index);
		return false;
	}
	else {

		char cbuf[32];

		if (devices.size()<=index) {
			LogErr("Adapter Index out of range");
			return false;
		}

		// GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &curMode) stood here, and
		// curMode was never read afterwards -- one more of finding 35. The
		// query goes with the variable rather than being kept to be discarded.

		SendDlgItemMessage(hTab, IDC_VID_MODE, CB_RESETCONTENT, 0, 0);

		DWORD nModes = DWORD(orbiter_GetVideoModeCount());

		if (nModes == 0) {
			LogErr("VideoTab::SelectAdapter() No Display Modes Available");	
		}

		for (DWORD k=0;k<nModes;k++) {
			int mw = 0, mh = 0, mhz = 0;
			orbiter_GetVideoMode(int(k), &mw, &mh, &mhz);
			sprintf_s(cbuf,32,"%u x %u %uHz", mw, mh, mhz);
			SendDlgItemMessageA(hTab, IDC_VID_MODE, CB_ADDSTRING, 0, (LPARAM)cbuf);
			SendDlgItemMessageA(hTab, IDC_VID_MODE, CB_SETITEMDATA, k, (LPARAM)(mh<<16 | mw));
		}

		SendDlgItemMessage(hTab, IDC_VID_MODE, CB_SETCURSEL, data->modeidx, 0);
	}

	return true;
}



void VideoTab::SelectFullscreen(bool bFull)
{

	SetWindowText(GetDlgItem(hTab, IDC_VID_ENUM), "(unused)");
	SetWindowText(GetDlgItem(hTab, IDC_VID_STENCIL), "Force window size");
	SetWindowText(GetDlgItem(hTab, IDC_VID_PAGEFLIP), "Multiple displays");

	SendDlgItemMessage(hTab, IDC_VID_FULL, BM_SETCHECK, bFull ? BST_CHECKED : BST_UNCHECKED, 0);
	SendDlgItemMessage(hTab, IDC_VID_WINDOW, BM_SETCHECK, bFull ? BST_UNCHECKED : BST_CHECKED, 0);

	EnableWindow(GetDlgItem(hTab, IDC_VID_ENUM), false);
	EnableWindow(GetDlgItem(hTab, IDC_VID_STENCIL), true);

	if (bFull) {
		EnableWindow(GetDlgItem(hTab, IDC_VID_ASPECT), false);
		EnableWindow(GetDlgItem(hTab, IDC_VID_WIDTH), false);
		EnableWindow(GetDlgItem(hTab, IDC_VID_HEIGHT), false);
		EnableWindow(GetDlgItem(hTab, IDC_VID_4X3), false);
		EnableWindow(GetDlgItem(hTab, IDC_VID_16X10), false);
		EnableWindow(GetDlgItem(hTab, IDC_VID_16X9), false);
		EnableWindow(GetDlgItem(hTab, IDC_VID_MODE), true);
		EnableWindow(GetDlgItem(hTab, IDC_VID_VSYNC), true);
		EnableWindow(GetDlgItem(hTab, IDC_VID_PAGEFLIP), true);
		EnableWindow(GetDlgItem(hTab, IDC_VID_BPP), true);
	}
	else {
		EnableWindow(GetDlgItem(hTab, IDC_VID_ASPECT), true);
		EnableWindow(GetDlgItem(hTab, IDC_VID_WIDTH), true);
		EnableWindow(GetDlgItem(hTab, IDC_VID_HEIGHT), true);
		EnableWindow(GetDlgItem(hTab, IDC_VID_4X3), true);
		EnableWindow(GetDlgItem(hTab, IDC_VID_16X10), true);
		EnableWindow(GetDlgItem(hTab, IDC_VID_16X9), true);
		EnableWindow(GetDlgItem(hTab, IDC_VID_MODE), false);
		EnableWindow(GetDlgItem(hTab, IDC_VID_VSYNC), true);
		EnableWindow(GetDlgItem(hTab, IDC_VID_PAGEFLIP), false);
		EnableWindow(GetDlgItem(hTab, IDC_VID_BPP), false);
	}
}


static int aspect_wfac[3] = {4,16,16};
static int aspect_hfac[3] = {3,10,9};


void VideoTab::SelectWidth ()
{
	if (SendDlgItemMessage (hTab, IDC_VID_ASPECT, BM_GETCHECK, 0, 0) == BST_CHECKED) {
		char cbuf[32];
		int w, h, wfac = aspect_wfac[aspect_idx], hfac = aspect_hfac[aspect_idx];
		GetWindowText(GetDlgItem(hTab, IDC_VID_WIDTH),  cbuf, 32); w = atoi(cbuf);
		GetWindowText(GetDlgItem(hTab, IDC_VID_HEIGHT), cbuf, 32); h = atoi(cbuf);
		if (w != (wfac*h)/hfac) {
			h = (hfac*w)/wfac;
			SetWindowText (GetDlgItem (hTab, IDC_VID_HEIGHT), std::to_string(h).c_str());
		}
	}
}

// ==============================================================
// Respond to user selection of render window height

void VideoTab::SelectHeight ()
{
	if (SendDlgItemMessage (hTab, IDC_VID_ASPECT, BM_GETCHECK, 0, 0) == BST_CHECKED) {
		char cbuf[32];
		int w, h, wfac = aspect_wfac[aspect_idx], hfac = aspect_hfac[aspect_idx];
		GetWindowText(GetDlgItem(hTab, IDC_VID_WIDTH),  cbuf, 32); w = atoi(cbuf);
		GetWindowText(GetDlgItem(hTab, IDC_VID_HEIGHT), cbuf, 32); h = atoi(cbuf);
		if (h != (hfac*w)/wfac) {
			w = (wfac*h)/hfac;
			SetWindowText (GetDlgItem (hTab, IDC_VID_WIDTH), std::to_string(w).c_str());
		}
	}
}

// ==============================================================
// copy dialog state back to parameter structure
//
// THE SCENARIO BLOCK BELOW IS WIN32 UI, NOT DIRECT3D, and it is converted as
// written. Two things about it are worth recording.
//
// It needed two shim additions, GetAncestor and EnumChildWindows -- both pure
// walks over the window tree Win32Dlg.cpp already maintains. And it looks for
// Orbiter's own Launchpad scenario tree by control id, which on Linux is an
// ImGui tree in UIHost.cpp and not a window at all; so EnumChildWindows finds
// nothing and the function takes its own "FAILED to get a handle of a scenario
// dialog" path, which is a path the Windows version already handles.
//
// FINDING 43: NOTHING READS THE RESULT. gclient->SetScenarioName(path) writes
// VulkanClient::scenarioName, and scenarioName is written by that setter, set
// once more in the constructor, and never read anywhere in the client on
// either platform. So the whole block is a path computation whose product is
// discarded. Converted rather than deleted, on the same principle as
// CSphereManager::CreateDeviceObjects -- deleting dead code is a separate
// decision from porting it.

void VideoTab::UpdateConfigData()
{
	char cbuf[32];
	GraphicsClient::VIDEODATA *data = gclient->GetVideoData();

	// device parameters
	data->deviceidx  = (int)SendDlgItemMessage (hTab, IDC_VID_DEVICE, CB_GETCURSEL, 0, 0);
	data->modeidx	 = (int)SendDlgItemMessage (hTab, IDC_VID_MODE, CB_GETCURSEL, 0, 0);
	data->style		 = SendDlgItemMessage (hTab, IDC_VID_BPP, CB_GETCURSEL, 0, 0);
	data->fullscreen = (SendDlgItemMessage (hTab, IDC_VID_FULL, BM_GETCHECK, 0, 0) == BST_CHECKED);
	data->novsync    = (SendDlgItemMessage (hTab, IDC_VID_VSYNC, BM_GETCHECK, 0, 0) == BST_CHECKED);
	data->pageflip   = (SendDlgItemMessage (hTab, IDC_VID_PAGEFLIP, BM_GETCHECK, 0, 0) == BST_CHECKED);
	data->trystencil = (SendDlgItemMessage (hTab, IDC_VID_STENCIL, BM_GETCHECK, 0, 0) == BST_CHECKED);
	data->forceenum  = (SendDlgItemMessage (hTab, IDC_VID_ENUM, BM_GETCHECK, 0, 0) == BST_CHECKED);

	GetWindowText(GetDlgItem(hTab, IDC_VID_WIDTH),  cbuf, 32); data->winw = atoi(cbuf);
	GetWindowText(GetDlgItem(hTab, IDC_VID_HEIGHT), cbuf, 32); data->winh = atoi(cbuf);	


	HWND hChild = NULL;
	HWND hRoot = GetAncestor(hTab, GA_ROOT);

	EnumChildWindows(hRoot, EnumChildProc, (LPARAM)&hChild);

	if (hChild) {

		HWND hTree = GetDlgItem(hChild, IDC_SCENARIO_TREE);

		if (hTree==NULL) {
			LogErr("FAILED to get a scenario tree control handle");
			return;
		}

		HTREEITEM item = TreeView_GetSelection(hTree);

		if (item == NULL) {
			LogErr("FAILED. Scenario not selected");
			return;
		}

		using std::vector;
		vector<HTREEITEM> hNodes;

		while (item) { // [ego, parent, grandparent, ...]
			hNodes.push_back( item );
			item = TreeView_GetParent(hTree, item);
		}

		using std::string;
		string path = OapiExtension::GetScenarioDir();
		// Was find_last_not_of('\\') and path += "\\". A scenario directory on
		// Linux is separated with '/', and this string is opened as a file --
		// finding 24. Two more instances.
		path.erase( path.find_last_not_of( '/' )+1 ); // trim trailing path-delimiter

		char buf[MAX_PATH];
		// Was = {0}, which -Wextra reports as leaving eight members
		// uninitialised even though the language value-initialises every one
		// of them. = {} says the same thing without the report and produces
		// the identical object. Finding 36's family.
		TVITEMA tvItem = {};
		tvItem.mask = TVIF_TEXT | TVIF_HANDLE;
		tvItem.pszText = buf;
		tvItem.cchTextMax = ARRAYSIZE(buf);

		for (auto it = hNodes.crbegin(); it != hNodes.crend(); ++it) {
			tvItem.hItem = *it;
			// The (void) is the macro's return value, which this call has
			// always discarded: TreeView_GetItem expands to a SendMessage
			// cast to BOOL, and GCC reports an unused computed value under
			// -Wall where MSVC's equivalent is off by default.
			(void)TreeView_GetItem(hTree, &tvItem);
			// Note: The returned text will not necessarily be stored in the
			//       original buffer passed by the application.
			//       It is possible that pszText will point to text in a
			//       new buffer rather than place it in the old buffer. 
			path += "/"; path += tvItem.pszText;
		}
		path += ".scn";

		gclient->SetScenarioName(path);

		LogAlw("Scenario = %s", path.c_str());
	}
	else {
		LogErr("FAILED to get a handle of a scenario dialog");
	}
}





// ***************************************************************************************************
// Advanced setup Dialog
// ***************************************************************************************************


INT_PTR CALLBACK VideoTab::SetupDlgProcWrp(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	static class VideoTab *VTab = NULL;
	switch (uMsg) {
		case WM_INITDIALOG: 
			VTab = (class VideoTab *)lParam;
			VTab->InitSetupDialog(hWnd);
			return true;

		case WM_COMMAND:
		case WM_HSCROLL:
			if (VTab) VTab->SetupDlgProc(hWnd, uMsg, wParam, lParam);
			break;
	}
	return false;
}



INT_PTR CALLBACK VideoTab::SetupDlgProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{

	if (uMsg==WM_HSCROLL) {
		if (LOWORD(wParam)==TB_THUMBTRACK) {
			char lbl[32];
			WORD pos = HIWORD(wParam);
			if (HWND(lParam)==GetDlgItem(hWnd, IDC_CONVERGENCE)) {
				sprintf_s(lbl,32,"%1.2fm",float(pos)*0.01);
				SetWindowTextA(GetDlgItem(hWnd, IDC_CONV_DSP), lbl);
			}
			if (HWND(lParam)==GetDlgItem(hWnd, IDC_SEPARATION)) {
				sprintf_s(lbl,32,"%1.0f%%",float(pos));
				SetWindowTextA(GetDlgItem(hWnd, IDC_SEPA_DSP), lbl);
			}
		}
		return false;
	}

	switch (LOWORD(wParam)) {

		case IDC_MESH_DEBUGGER:
			MessageBoxA(hWnd,"You must restart launchpad for changes to take effect","Notification",MB_OK);
			break;

		case IDC_CREDITS:
			LoadLibrary("riched20.dll");
			DialogBoxParamA(hInst, MAKEINTRESOURCEA(IDD_D3D9CREDITS), hWnd, CreditsDlgProcWrp, (LPARAM)this);
			break;

		case IDC_SRFPRELOAD:
			SendDlgItemMessageA(hWnd, IDC_DEMAND, BM_SETCHECK, BST_UNCHECKED, 0);
			break;

		case IDC_DEMAND:
			SendDlgItemMessageA(hWnd, IDC_SRFPRELOAD, BM_SETCHECK, BST_UNCHECKED, 0);
			break;

		case IDOK:
		case IDCANCEL:
			SaveSetupState(hWnd);
			EndDialog (hWnd, 0);
			break;
	}
	
	return false;
}





void VideoTab::InitSetupDialog(HWND hWnd)
{

	char cbuf[32];
	DWORD aamax = 0;

	// Was D3DCAPS9 caps; and GetDeviceCaps(idx, D3DDEVTYPE_HAL, &caps).
	// D3DCAPS9 was one struct covering limits, format support and feature
	// bits; Vulkan splits those three, and the one field this function reads
	// -- MaxAnisotropy -- lives in the limits. Held as its own variable
	// because that is all of D3DCAPS9 the file uses.
	float maxAniso = 1.0f;

	std::vector<VkPhysicalDevice> devices;

	if (!GetPhysicalDevices(devices) || devices.size() <= SelectedAdapterIdx) {
		LogErr("VideoTab::SelectAdapter(%u) No Vulkan device available", SelectedAdapterIdx);
		return;
	}

	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(devices[SelectedAdapterIdx], &props);
	maxAniso = props.limits.maxSamplerAnisotropy;

	// CheckDeviceMultiSampleType(adapter, HAL, X8R8G8B8, windowed, N_SAMPLES,
	// NULL) == S_OK -> "does this format support N-sample multisampling".
	// Vulkan answers it as a bitmask of the counts a colour attachment may
	// have, which is the same question for every colour format at once.
	//
	// WORTH KNOWING, though it is not this file's business to fix: the core's
	// render pass is created with VK_SAMPLE_COUNT_1_BIT (UIHost.cpp), so what
	// this combo offers is what the HARDWARE can do, exactly as the D3D9 query
	// reported, and not what the client can currently switch on.
	const VkSampleCountFlags aa = props.limits.framebufferColorSampleCounts;

	if (aa & VK_SAMPLE_COUNT_2_BIT) aamax=2;
	if (aa & VK_SAMPLE_COUNT_4_BIT) aamax=4;
	if (aa & VK_SAMPLE_COUNT_8_BIT) aamax=8;
	
	LogAlw("InitSetupDialog() Enum Device AA capability = %u",aamax);


	// AA -----------------------------------------

	SendDlgItemMessage(hWnd, IDC_AA, CB_RESETCONTENT, 0, 0);
	SendDlgItemMessageA(hWnd, IDC_AA, CB_ADDSTRING, 0, (LPARAM)"None");
	if (aamax>=2)  SendDlgItemMessageA(hWnd, IDC_AA, CB_ADDSTRING, 0, (LPARAM)"2x");
	if (aamax>=4)  SendDlgItemMessageA(hWnd, IDC_AA, CB_ADDSTRING, 0, (LPARAM)"4x");
	if (aamax>=8)  SendDlgItemMessageA(hWnd, IDC_AA, CB_ADDSTRING, 0, (LPARAM)"8x");
	

	// AF -----------------------------------------

	SendDlgItemMessage(hWnd, IDC_AF, CB_RESETCONTENT, 0, 0);
	if (maxAniso>=2) SendDlgItemMessageA(hWnd, IDC_AF, CB_ADDSTRING, 0, (LPARAM)"2x");
	if (maxAniso>=4) SendDlgItemMessageA(hWnd, IDC_AF, CB_ADDSTRING, 0, (LPARAM)"4x");
	if (maxAniso>=8) SendDlgItemMessageA(hWnd, IDC_AF, CB_ADDSTRING, 0, (LPARAM)"8x");
	if (maxAniso>=12) SendDlgItemMessageA(hWnd, IDC_AF, CB_ADDSTRING, 0, (LPARAM)"12x");
	if (maxAniso>=16) SendDlgItemMessageA(hWnd, IDC_AF, CB_ADDSTRING, 0, (LPARAM)"16x");
	

	// DEBUG --------------------------------------

	SendDlgItemMessage(hWnd, IDC_DEBUG, CB_RESETCONTENT, 0, 0);
	SendDlgItemMessageA(hWnd, IDC_DEBUG, CB_ADDSTRING, 0, (LPARAM)"0");
	SendDlgItemMessageA(hWnd, IDC_DEBUG, CB_ADDSTRING, 0, (LPARAM)"1");
	SendDlgItemMessageA(hWnd, IDC_DEBUG, CB_ADDSTRING, 0, (LPARAM)"2");
	SendDlgItemMessageA(hWnd, IDC_DEBUG, CB_ADDSTRING, 0, (LPARAM)"3");
	SendDlgItemMessageA(hWnd, IDC_DEBUG, CB_ADDSTRING, 0, (LPARAM)"4");
	
	// SKETCHPAD --------------------------------------

	SendDlgItemMessage(hWnd, IDC_FONT, CB_RESETCONTENT, 0, 0);
	SendDlgItemMessageA(hWnd, IDC_FONT, CB_ADDSTRING, 0, (LPARAM)"Crisp");
	SendDlgItemMessageA(hWnd, IDC_FONT, CB_ADDSTRING, 0, (LPARAM)"Antialiased");
	SendDlgItemMessageA(hWnd, IDC_FONT, CB_ADDSTRING, 0, (LPARAM)"Cleartype");
	
	// ENVMAP MODE --------------------------------------

	SendDlgItemMessage(hWnd, IDC_ENVMODE, CB_RESETCONTENT, 0, 0);
	SendDlgItemMessageA(hWnd, IDC_ENVMODE, CB_ADDSTRING, 0, (LPARAM)"Disable (Debug)");
	SendDlgItemMessageA(hWnd, IDC_ENVMODE, CB_ADDSTRING, 0, (LPARAM)"Planet Only");
	SendDlgItemMessageA(hWnd, IDC_ENVMODE, CB_ADDSTRING, 0, (LPARAM)"Full Scene");
	SendDlgItemMessage(hWnd, IDC_ENVMODE, CB_SETCURSEL, 0, 0);

	// CUSTOM CAMERA MODE --------------------------------------

	SendDlgItemMessage(hWnd, IDC_CAMMODE, CB_RESETCONTENT, 0, 0);
	SendDlgItemMessageA(hWnd, IDC_CAMMODE, CB_ADDSTRING, 0, (LPARAM)"Disable");
	SendDlgItemMessageA(hWnd, IDC_CAMMODE, CB_ADDSTRING, 0, (LPARAM)"Enabled");
	SendDlgItemMessage(hWnd, IDC_ENVMODE, CB_SETCURSEL, 0, 0);

	// ENVMAP FACES --------------------------------------

	SendDlgItemMessage(hWnd, IDC_ENVFACES, CB_RESETCONTENT, 0, 0);
	SendDlgItemMessageA(hWnd, IDC_ENVFACES, CB_ADDSTRING, 0, (LPARAM)"Light");
	SendDlgItemMessageA(hWnd, IDC_ENVFACES, CB_ADDSTRING, 0, (LPARAM)"Medimum");
	SendDlgItemMessageA(hWnd, IDC_ENVFACES, CB_ADDSTRING, 0, (LPARAM)"Heavy");
	SendDlgItemMessage(hWnd, IDC_ENVFACES, CB_SETCURSEL, 0, 0);

	// TEXTURE MIPMAP POLICY --------------------------------------

	SendDlgItemMessage(hWnd, IDC_TEXMIPS, CB_RESETCONTENT, 0, 0);
	SendDlgItemMessageA(hWnd, IDC_TEXMIPS, CB_ADDSTRING, 0, (LPARAM)"Load as defined");
	SendDlgItemMessageA(hWnd, IDC_TEXMIPS, CB_ADDSTRING, 0, (LPARAM)"Autogen missing");
	SendDlgItemMessageA(hWnd, IDC_TEXMIPS, CB_ADDSTRING, 0, (LPARAM)"Autogen all");
	SendDlgItemMessage(hWnd, IDC_TEXMIPS, CB_SETCURSEL, 0, 0);

	// MICROTEX FILTER --------------------------------------------

	SendDlgItemMessage(hWnd,  IDC_MICROFILTER, CB_RESETCONTENT, 0, 0);
	SendDlgItemMessageA(hWnd, IDC_MICROFILTER, CB_ADDSTRING, 0, (LPARAM)"Point (Fast/Good)");
	SendDlgItemMessageA(hWnd, IDC_MICROFILTER, CB_ADDSTRING, 0, (LPARAM)"Linear (Fast/Bad)");
	SendDlgItemMessageA(hWnd, IDC_MICROFILTER, CB_ADDSTRING, 0, (LPARAM)"Anisotropic 2x");
	SendDlgItemMessageA(hWnd, IDC_MICROFILTER, CB_ADDSTRING, 0, (LPARAM)"Anisotropic 4x (Better)");
	SendDlgItemMessageA(hWnd, IDC_MICROFILTER, CB_ADDSTRING, 0, (LPARAM)"Anisotropic 8x");
	SendDlgItemMessageA(hWnd, IDC_MICROFILTER, CB_ADDSTRING, 0, (LPARAM)"Anisotropic 16x (Slow/Best)");
	SendDlgItemMessage(hWnd,  IDC_MICROFILTER, CB_SETCURSEL, 0, 0);
	
	// MICROTEX FILTER --------------------------------------------

	SendDlgItemMessage(hWnd,  IDC_MICROMODE, CB_RESETCONTENT, 0, 0);
	SendDlgItemMessageA(hWnd, IDC_MICROMODE, CB_ADDSTRING, 0, (LPARAM)"Disabled");
	SendDlgItemMessageA(hWnd, IDC_MICROMODE, CB_ADDSTRING, 0, (LPARAM)"Enabled");
	SendDlgItemMessage(hWnd,  IDC_MICROMODE, CB_SETCURSEL, 0, 0);


	// MICROTEX BLEND MODE -----------------------------------------
	
	SendDlgItemMessage(hWnd,  IDC_BLENDMODE, CB_RESETCONTENT, 0, 0);
	SendDlgItemMessageA(hWnd, IDC_BLENDMODE, CB_ADDSTRING, 0, (LPARAM)"Soft light");
	SendDlgItemMessageA(hWnd, IDC_BLENDMODE, CB_ADDSTRING, 0, (LPARAM)"Normal light");
	SendDlgItemMessageA(hWnd, IDC_BLENDMODE, CB_ADDSTRING, 0, (LPARAM)"Hard light");
	SendDlgItemMessage(hWnd,  IDC_BLENDMODE, CB_SETCURSEL, 0, 0);

	// TILE MIPMAP POLICY -----------------------------------------

	SendDlgItemMessage(hWnd, IDC_MIPMAPS, CB_RESETCONTENT, 0, 0);
	SendDlgItemMessageA(hWnd, IDC_MIPMAPS, CB_ADDSTRING, 0, (LPARAM)"Disabled");
	SendDlgItemMessageA(hWnd, IDC_MIPMAPS, CB_ADDSTRING, 0, (LPARAM)"Enabled (slow2load)");
	SendDlgItemMessage(hWnd, IDC_MIPMAPS, CB_SETCURSEL, 0, 0);

	// ARCHIVE METHOD ------------------------------------------

	SendDlgItemMessage(hWnd, IDC_ARCHIVE, CB_RESETCONTENT, 0, 0);
	SendDlgItemMessageA(hWnd, IDC_ARCHIVE, CB_ADDSTRING, 0, (LPARAM)"Cache only");
	SendDlgItemMessageA(hWnd, IDC_ARCHIVE, CB_ADDSTRING, 0, (LPARAM)"Archive only");
	SendDlgItemMessageA(hWnd, IDC_ARCHIVE, CB_ADDSTRING, 0, (LPARAM)"Cache & Archive");
	SendDlgItemMessage(hWnd, IDC_ARCHIVE, CB_SETCURSEL, 0, 0);

	// POSTPROCESSING METHOD ------------------------------------------

	SendDlgItemMessage(hWnd, IDC_POSTPROCESS, CB_RESETCONTENT, 0, 0);
	SendDlgItemMessageA(hWnd, IDC_POSTPROCESS, CB_ADDSTRING, 0, (LPARAM)"None");
	SendDlgItemMessageA(hWnd, IDC_POSTPROCESS, CB_ADDSTRING, 0, (LPARAM)"Light glow");
	SendDlgItemMessage(hWnd, IDC_POSTPROCESS, CB_SETCURSEL, 0, 0);

	// Local Lights -----------------------------------------

	SendDlgItemMessage(hWnd, IDC_LIGHTCONFIG, CB_RESETCONTENT, 0, 0);
	SendDlgItemMessageA(hWnd, IDC_LIGHTCONFIG, CB_ADDSTRING, 0, (LPARAM)"None");
	SendDlgItemMessageA(hWnd, IDC_LIGHTCONFIG, CB_ADDSTRING, 0, (LPARAM)"4x Partial");
	SendDlgItemMessageA(hWnd, IDC_LIGHTCONFIG, CB_ADDSTRING, 0, (LPARAM)"4x Full");
	SendDlgItemMessageA(hWnd, IDC_LIGHTCONFIG, CB_ADDSTRING, 0, (LPARAM)"8x Partial");
	SendDlgItemMessageA(hWnd, IDC_LIGHTCONFIG, CB_ADDSTRING, 0, (LPARAM)"8x Full");
	SendDlgItemMessageA(hWnd, IDC_LIGHTCONFIG, CB_ADDSTRING, 0, (LPARAM)"12x Partial");
	SendDlgItemMessageA(hWnd, IDC_LIGHTCONFIG, CB_ADDSTRING, 0, (LPARAM)"12x Full");
	SendDlgItemMessageA(hWnd, IDC_LIGHTCONFIG, CB_ADDSTRING, 0, (LPARAM)"16x Partial");
	SendDlgItemMessageA(hWnd, IDC_LIGHTCONFIG, CB_ADDSTRING, 0, (LPARAM)"16x Full");
	SendDlgItemMessageA(hWnd, IDC_LIGHTCONFIG, CB_ADDSTRING, 0, (LPARAM)"20x Partial");
	SendDlgItemMessageA(hWnd, IDC_LIGHTCONFIG, CB_ADDSTRING, 0, (LPARAM)"20x Full");

	// Shadows -----------------------------------------

	SendDlgItemMessage(hWnd, IDC_SELFSHADOWS, CB_RESETCONTENT, 0, 0);
	SendDlgItemMessageA(hWnd, IDC_SELFSHADOWS, CB_ADDSTRING, 0, (LPARAM)"None");
	SendDlgItemMessageA(hWnd, IDC_SELFSHADOWS, CB_ADDSTRING, 0, (LPARAM)"Focus + payload");
	SendDlgItemMessageA(hWnd, IDC_SELFSHADOWS, CB_ADDSTRING, 0, (LPARAM)"Near by objects");
	SendDlgItemMessageA(hWnd, IDC_SELFSHADOWS, CB_ADDSTRING, 0, (LPARAM)"All visible objects");

	SendDlgItemMessage(hWnd, IDC_SHADOWFILTER, CB_RESETCONTENT, 0, 0);
	SendDlgItemMessageA(hWnd, IDC_SHADOWFILTER, CB_ADDSTRING, 0, (LPARAM)"9 samples");
	SendDlgItemMessageA(hWnd, IDC_SHADOWFILTER, CB_ADDSTRING, 0, (LPARAM)"27 samples");
	SendDlgItemMessageA(hWnd, IDC_SHADOWFILTER, CB_ADDSTRING, 0, (LPARAM)"27s dither");
	//SendDlgItemMessageA(hWnd, IDC_SHADOWFILTER, CB_ADDSTRING, 0, (LPARAM)"40 samples");
	//SendDlgItemMessageA(hWnd, IDC_SHADOWFILTER, CB_ADDSTRING, 0, (LPARAM)"40s dither");

	SendDlgItemMessage(hWnd, IDC_TERRAIN, CB_RESETCONTENT, 0, 0);
	SendDlgItemMessageA(hWnd, IDC_TERRAIN, CB_ADDSTRING, 0, (LPARAM)"None");
	SendDlgItemMessageA(hWnd, IDC_TERRAIN, CB_ADDSTRING, 0, (LPARAM)"Stencil");
	SendDlgItemMessageA(hWnd, IDC_TERRAIN, CB_ADDSTRING, 0, (LPARAM)"Projected");

	SendDlgItemMessage(hWnd, IDC_MESHRES, CB_RESETCONTENT, 0, 0);
	SendDlgItemMessageA(hWnd, IDC_MESHRES, CB_ADDSTRING, 0, (LPARAM)"16");
	SendDlgItemMessageA(hWnd, IDC_MESHRES, CB_ADDSTRING, 0, (LPARAM)"32");

	SendDlgItemMessage(hWnd, IDC_TILECOUNT, CB_RESETCONTENT, 0, 0);
	SendDlgItemMessageA(hWnd, IDC_TILECOUNT, CB_ADDSTRING, 0, (LPARAM)"600");
	SendDlgItemMessageA(hWnd, IDC_TILECOUNT, CB_ADDSTRING, 0, (LPARAM)"1200");
	SendDlgItemMessageA(hWnd, IDC_TILECOUNT, CB_ADDSTRING, 0, (LPARAM)"2400");

	// gcGUI -----------------------------------------
	if (Config->gcGUIMode == 1) Config->gcGUIMode = 0;
	SendDlgItemMessage(hWnd, IDC_GUIMODE, CB_RESETCONTENT, 0, 0);
	SendDlgItemMessageA(hWnd, IDC_GUIMODE, CB_ADDSTRING, 0, (LPARAM)"Disabled");
	SendDlgItemMessageA(hWnd, IDC_GUIMODE, CB_ADDSTRING, 0, (LPARAM)"(unused)");
	SendDlgItemMessageA(hWnd, IDC_GUIMODE, CB_ADDSTRING, 0, (LPARAM)"Windowed");

	// Earth AtmoConfig -------------------------------
	SendDlgItemMessage(hWnd, IDC_EARTHVISCFG, CB_RESETCONTENT, 0, 0);
	for (auto x : AtmoCfgs["Earth"]) SendDlgItemMessageA(hWnd, IDC_EARTHVISCFG, CB_ADDSTRING, 0, (LPARAM)x.cfg.c_str());
	// int against vector::size() -- one more of finding 26. The index is never
	// negative and the count never exceeds INT_MAX, so int() at the comparison
	// says which of the two the loop means.
	for (int i = 0; i < int(AtmoCfgs["Earth"].size()); i++) {
		if (Config->AtmoCfg["Earth"] == AtmoCfgs["Earth"][i].file) {
			SendDlgItemMessage(hWnd, IDC_EARTHVISCFG, CB_SETCURSEL, i, 0);
			break;
		}
	}
		


	// Write values in controls ----------------

	bool bFS = (SendDlgItemMessage(hTab, IDC_VID_BPP, CB_GETCURSEL, 0, 0)==0 && SendDlgItemMessage(hTab, IDC_VID_FULL, BM_GETCHECK, 0, 0)==BST_CHECKED);
	bool bGB = (SendDlgItemMessage (hTab, IDC_VID_STENCIL, BM_GETCHECK, 0, 0)==BST_CHECKED);

	if (bFS || bGB) {
		Config->SceneAntialias = 0;
		EnableWindow(GetDlgItem(hWnd, IDC_AA), false);
	}
	else {
		EnableWindow(GetDlgItem(hWnd, IDC_AA), true);
	}


	SendDlgItemMessage(hWnd, IDC_CONVERGENCE, TBM_SETRANGEMAX, 1, 100);
	SendDlgItemMessage(hWnd, IDC_CONVERGENCE, TBM_SETRANGEMIN, 1, 5);
	SendDlgItemMessage(hWnd, IDC_CONVERGENCE, TBM_SETTICFREQ, 5, 0);
	
	SendDlgItemMessage(hWnd, IDC_SEPARATION, TBM_SETRANGEMAX,  1, 100);
	SendDlgItemMessage(hWnd, IDC_SEPARATION, TBM_SETRANGEMIN,  1, 10);
	SendDlgItemMessage(hWnd, IDC_SEPARATION, TBM_SETTICFREQ,  5, 0);
	
	SendDlgItemMessage(hWnd, IDC_LODBIAS, TBM_SETRANGEMAX, 1, 10);
	SendDlgItemMessage(hWnd, IDC_LODBIAS, TBM_SETRANGEMIN, 1, -10);
	SendDlgItemMessage(hWnd, IDC_LODBIAS, TBM_SETTICFREQ, 1, 0);

	SendDlgItemMessage(hWnd, IDC_MICROBIAS, TBM_SETRANGEMAX, 1, 10);
	SendDlgItemMessage(hWnd, IDC_MICROBIAS, TBM_SETRANGEMIN, 1, 0);
	SendDlgItemMessage(hWnd, IDC_MICROBIAS, TBM_SETTICFREQ, 1, 0);
	

	sprintf_s(cbuf,32,"%1.1fm",float(Config->Convergence));
	SetWindowTextA(GetDlgItem(hWnd, IDC_CONV_DSP), cbuf);
			
	sprintf_s(cbuf,32,"%1.0f%%",float(Config->Separation));
	SetWindowTextA(GetDlgItem(hWnd, IDC_SEPA_DSP), cbuf);

	SendDlgItemMessage(hWnd, IDC_CONVERGENCE, TBM_SETPOS, 1, int(Config->Convergence*100.0));
	SendDlgItemMessage(hWnd, IDC_SEPARATION,  TBM_SETPOS, 1, int(Config->Separation));
	SendDlgItemMessage(hWnd, IDC_LODBIAS,     TBM_SETPOS, 1, int(Config->LODBias*5.0));
	SendDlgItemMessage(hWnd, IDC_MICROBIAS,   TBM_SETPOS, 1, int(Config->MicroBias));

	SendDlgItemMessage(hWnd, IDC_TILECOUNT, CB_SETCURSEL, Config->MaxTiles, 0);
	SendDlgItemMessage(hWnd, IDC_MESHRES, CB_SETCURSEL, Config->MeshRes, 0);
	SendDlgItemMessage(hWnd, IDC_ARCHIVE, CB_SETCURSEL, Config->PlanetTileLoadFlags-1, 0);
	SendDlgItemMessage(hWnd, IDC_BLENDMODE, CB_SETCURSEL, Config->BlendMode, 0);
	SendDlgItemMessage(hWnd, IDC_MICROMODE, CB_SETCURSEL, Config->MicroMode, 0);
	SendDlgItemMessage(hWnd, IDC_MICROFILTER, CB_SETCURSEL, Config->MicroFilter, 0);
	SendDlgItemMessage(hWnd, IDC_TEXMIPS, CB_SETCURSEL, Config->TextureMips, 0);
	SendDlgItemMessage(hWnd, IDC_ENVMODE, CB_SETCURSEL, Config->EnvMapMode, 0);
	SendDlgItemMessage(hWnd, IDC_CAMMODE, CB_SETCURSEL, Config->CustomCamMode, 0);
	SendDlgItemMessage(hWnd, IDC_ENVFACES, CB_SETCURSEL, Config->EnvMapFaces-1, 0);
	SendDlgItemMessage(hWnd, IDC_FONT, CB_SETCURSEL, Config->SketchpadFont, 0);
	SendDlgItemMessage(hWnd, IDC_DEBUG, CB_SETCURSEL, Config->DebugLvl, 0);
	SendDlgItemMessage(hWnd, IDC_MIPMAPS, CB_SETCURSEL, Config->TileMipmaps, 0);
	SendDlgItemMessage(hWnd, IDC_POSTPROCESS, CB_SETCURSEL, Config->PostProcess, 0);
	SendDlgItemMessage(hWnd, IDC_LIGHTCONFIG, CB_SETCURSEL, Config->LightConfig, 0);
	SendDlgItemMessage(hWnd, IDC_SELFSHADOWS, CB_SETCURSEL, Config->ShadowMapMode, 0);
	SendDlgItemMessage(hWnd, IDC_SHADOWFILTER, CB_SETCURSEL, Config->ShadowFilter, 0);
	SendDlgItemMessage(hWnd, IDC_TERRAIN, CB_SETCURSEL, Config->TerrainShadowing, 0);
	SendDlgItemMessage(hWnd, IDC_GUIMODE, CB_SETCURSEL, Config->gcGUIMode, 0);

	SendDlgItemMessage(hWnd, IDC_DEMAND, BM_SETCHECK, Config->PlanetPreloadMode==0, 0);
	SendDlgItemMessage(hWnd, IDC_SRFPRELOAD, BM_SETCHECK, Config->PlanetPreloadMode==1, 0);
	SendDlgItemMessage(hWnd, IDC_GLASSSHADE, BM_SETCHECK, Config->EnableGlass==1, 0);
	SendDlgItemMessage(hWnd, IDC_MESH_DEBUGGER, BM_SETCHECK, Config->EnableMeshDbg==1, 0);
	SendDlgItemMessage(hWnd, IDC_CLOUDMICRO, BM_SETCHECK, Config->CloudMicro == 1, 0);
	SendDlgItemMessage(hWnd, IDC_GDIOVERLAY, BM_SETCHECK, Config->GDIOverlay == 1, 0);
	SendDlgItemMessage(hWnd, IDC_ABSANIM, BM_SETCHECK, Config->bAbsAnims == 1, 0);
	SendDlgItemMessage(hWnd, IDC_CLOUDNORM, BM_SETCHECK, Config->bCloudNormals == 1, 0);
	SendDlgItemMessage(hWnd, IDC_FLATS, BM_SETCHECK, Config->bFlats == 1, 0);
	SendDlgItemMessage(hWnd, IDC_ESUNGLARE, BM_SETCHECK, Config->bGlares == 1, 0);
	SendDlgItemMessage(hWnd, IDC_ELIGHTSGLARE, BM_SETCHECK, Config->bLocalGlares == 1, 0);
	SendDlgItemMessage(hWnd, IDC_EIRRAD, BM_SETCHECK, Config->bIrradiance == 1, 0);
	SendDlgItemMessage(hWnd, IDC_ESCACHE, BM_SETCHECK, Config->ShaderCacheUse == 1, 0);
	SendDlgItemMessage(hWnd, IDC_EAQUALITY, BM_SETCHECK, Config->bAtmoQuality == 1, 0);


	SendDlgItemMessage(hWnd, IDC_NORMALMAPS, BM_SETCHECK, Config->UseNormalMap==1, 0);
	SendDlgItemMessage(hWnd, IDC_BASEVIS,    BM_SETCHECK, Config->PreLBaseVis==1, 0);
	SendDlgItemMessage(hWnd, IDC_NEARPLANE,  BM_SETCHECK, Config->NearClipPlane==1, 0);
	SendDlgItemMessage(hWnd, IDC_BREAK,		 BM_SETCHECK, Config->DebugBreak == 1, 0);
	
	sprintf_s(cbuf,32,"%d", Config->PlanetLoadFrequency);
	SetWindowText(GetDlgItem(hWnd, IDC_HZ), cbuf);

	sprintf_s(cbuf,32,"%3.3f", Config->PlanetGlow);
	SetWindowText(GetDlgItem(hWnd, IDC_PLANETGLOW), cbuf);

	// caps.MaxAnisotropy was a DWORD; maxSamplerAnisotropy is a float, because
	// Vulkan's sampler takes a float ratio. The DWORD cast is where the two
	// meet, and it truncates towards zero exactly as the switch below expects.
	DWORD af = min(DWORD(maxAniso), DWORD(Config->Anisotrophy));

	switch(af) {
		case 2: SendDlgItemMessage(hWnd, IDC_AF, CB_SETCURSEL, 0, 0); break;
		default:
		case 4: SendDlgItemMessage(hWnd, IDC_AF, CB_SETCURSEL, 1, 0); break;
		case 8: SendDlgItemMessage(hWnd, IDC_AF, CB_SETCURSEL, 2, 0); break;
		case 12: SendDlgItemMessage(hWnd, IDC_AF, CB_SETCURSEL, 3, 0); break;
		case 16: SendDlgItemMessage(hWnd, IDC_AF, CB_SETCURSEL, 4, 0); break;
	}

	DWORD aaSel = min(aamax, DWORD(Config->SceneAntialias));

	switch(aaSel) {
		case 0: SendDlgItemMessage(hWnd, IDC_AA, CB_SETCURSEL, 0, 0); break;
		case 2: SendDlgItemMessage(hWnd, IDC_AA, CB_SETCURSEL, 1, 0); break;
		default:
		case 4: SendDlgItemMessage(hWnd, IDC_AA, CB_SETCURSEL, 2, 0); break;
		case 8: SendDlgItemMessage(hWnd, IDC_AA, CB_SETCURSEL, 3, 0); break;
	}
}




void VideoTab::SaveSetupState(HWND hWnd)
{
	char cbuf[32];
	// Combo boxes
	Config->SketchpadFont = (int)SendDlgItemMessage (hWnd, IDC_FONT, CB_GETCURSEL, 0, 0);
	Config->EnvMapMode	  = (int)SendDlgItemMessage (hWnd, IDC_ENVMODE, CB_GETCURSEL, 0, 0);
	Config->CustomCamMode = (int)SendDlgItemMessage (hWnd, IDC_CAMMODE, CB_GETCURSEL, 0, 0);
	Config->EnvMapFaces	  = (int)SendDlgItemMessage (hWnd, IDC_ENVFACES, CB_GETCURSEL, 0, 0) + 1;
	Config->TextureMips	  = (int)SendDlgItemMessage (hWnd, IDC_TEXMIPS, CB_GETCURSEL, 0, 0);
	Config->MicroMode	  = (int)SendDlgItemMessage (hWnd, IDC_MICROMODE, CB_GETCURSEL, 0, 0);
	Config->MicroFilter	  = (int)SendDlgItemMessage (hWnd, IDC_MICROFILTER, CB_GETCURSEL, 0, 0);
	Config->BlendMode	  = (int)SendDlgItemMessage (hWnd, IDC_BLENDMODE, CB_GETCURSEL, 0, 0);
	Config->TileMipmaps   = (int)SendDlgItemMessage (hWnd, IDC_MIPMAPS, CB_GETCURSEL, 0, 0);
	Config->PostProcess   = (int)SendDlgItemMessage (hWnd, IDC_POSTPROCESS, CB_GETCURSEL, 0, 0);
	Config->PlanetTileLoadFlags = (int)SendDlgItemMessage (hWnd, IDC_ARCHIVE, CB_GETCURSEL, 0, 0) + 1;
	Config->LightConfig   = (int)SendDlgItemMessage(hWnd, IDC_LIGHTCONFIG, CB_GETCURSEL, 0, 0);
	Config->ShadowMapMode = (int)SendDlgItemMessage(hWnd, IDC_SELFSHADOWS, CB_GETCURSEL, 0, 0);
	Config->ShadowFilter  = (int)SendDlgItemMessage(hWnd, IDC_SHADOWFILTER, CB_GETCURSEL, 0, 0);
	Config->TerrainShadowing = (int)SendDlgItemMessage(hWnd, IDC_TERRAIN, CB_GETCURSEL, 0, 0);
	Config->gcGUIMode	  = (int)SendDlgItemMessage(hWnd, IDC_GUIMODE, CB_GETCURSEL, 0, 0);
	Config->MeshRes		  = int(SendDlgItemMessage(hWnd, IDC_MESHRES, CB_GETCURSEL, 0, 0));
	Config->MaxTiles	  = int(SendDlgItemMessage(hWnd, IDC_TILECOUNT, CB_GETCURSEL, 0, 0));

	if (Config->gcGUIMode == 1) Config->gcGUIMode = 0;

	// Check boxes
	Config->UseNormalMap  = (int)SendDlgItemMessage (hWnd, IDC_NORMALMAPS, BM_GETCHECK, 0, 0);
	Config->PreLBaseVis   = (int)SendDlgItemMessage (hWnd, IDC_BASEVIS,    BM_GETCHECK, 0, 0);
	Config->NearClipPlane = (int)SendDlgItemMessage (hWnd, IDC_NEARPLANE,  BM_GETCHECK, 0, 0);
	Config->EnableGlass   = (int)SendDlgItemMessage (hWnd, IDC_GLASSSHADE,  BM_GETCHECK, 0, 0);
	Config->EnableMeshDbg = (int)SendDlgItemMessage (hWnd, IDC_MESH_DEBUGGER,  BM_GETCHECK, 0, 0);
	Config->CloudMicro    = (int)SendDlgItemMessage (hWnd, IDC_CLOUDMICRO, BM_GETCHECK, 0, 0);
	Config->GDIOverlay	  = (int)SendDlgItemMessage (hWnd, IDC_GDIOVERLAY, BM_GETCHECK, 0, 0);
	Config->bAbsAnims	  = (int)SendDlgItemMessage (hWnd, IDC_ABSANIM, BM_GETCHECK, 0, 0);
	Config->bCloudNormals = (int)SendDlgItemMessage(hWnd, IDC_CLOUDNORM, BM_GETCHECK, 0, 0);
	Config->bFlats		  = (int)SendDlgItemMessage(hWnd, IDC_FLATS, BM_GETCHECK, 0, 0);
	Config->DebugBreak	  = (int)SendDlgItemMessage(hWnd, IDC_BREAK, BM_GETCHECK, 0, 0);
	Config->bGlares		  = (int)SendDlgItemMessage(hWnd, IDC_ESUNGLARE, BM_GETCHECK, 0, 0);
	Config->bLocalGlares  = (int)SendDlgItemMessage(hWnd, IDC_ELIGHTSGLARE, BM_GETCHECK, 0, 0);
	Config->bIrradiance   = (int)SendDlgItemMessage(hWnd, IDC_EIRRAD, BM_GETCHECK, 0, 0);
	Config->ShaderCacheUse= (int)SendDlgItemMessage(hWnd, IDC_ESCACHE, BM_GETCHECK, 0, 0);
	Config->bAtmoQuality  = (int)SendDlgItemMessage(hWnd, IDC_EAQUALITY, BM_GETCHECK, 0, 0);

	// Sliders
	Config->Convergence   = double(SendDlgItemMessage(hWnd, IDC_CONVERGENCE, TBM_GETPOS, 0, 0)) * 0.01;
	Config->Separation	  = double(SendDlgItemMessage(hWnd, IDC_SEPARATION,  TBM_GETPOS, 0, 0));
	Config->LODBias       = 0.2 * double(SendDlgItemMessage(hWnd, IDC_LODBIAS,  TBM_GETPOS, 0, 0));
	Config->MicroBias     = int(SendDlgItemMessage(hWnd, IDC_MICROBIAS,  TBM_GETPOS, 0, 0));

	// Other things
	GetWindowText(GetDlgItem(hWnd, IDC_HZ),  cbuf, 32);

	Config->PlanetLoadFrequency = atoi(cbuf);
	Config->PlanetPreloadMode = (int)SendDlgItemMessage (hWnd, IDC_SRFPRELOAD, BM_GETCHECK, 0, 0);

	GetWindowText(GetDlgItem(hWnd, IDC_PLANETGLOW),  cbuf, 32);
	Config->PlanetGlow = atof(cbuf);

	Config->DebugLvl = (int)SendDlgItemMessage (hWnd, IDC_DEBUG, CB_GETCURSEL, 0, 0);

	switch(SendDlgItemMessage (hWnd, IDC_AF, CB_GETCURSEL, 0, 0)) {
		default:
		case 0: Config->Anisotrophy = 2; break;
		case 1: Config->Anisotrophy = 4; break;
		case 2: Config->Anisotrophy = 8; break;
		case 3: Config->Anisotrophy = 12; break;
		case 4: Config->Anisotrophy = 16; break;
	}

	switch(SendDlgItemMessage (hWnd, IDC_AA, CB_GETCURSEL, 0, 0)) {
		default:
		case 0: Config->SceneAntialias = 0; break;
		case 1: Config->SceneAntialias = 2; break;
		case 2: Config->SceneAntialias = 4; break;
		case 3: Config->SceneAntialias = 8; break;
	}

	int EASel = (int)SendDlgItemMessage(hWnd, IDC_EARTHVISCFG, CB_GETCURSEL, 0, 0);
	if (!AtmoCfgs["Earth"][EASel].file.empty()) Config->AtmoCfg["Earth"] = AtmoCfgs["Earth"][EASel].file;
	else Config->AtmoCfg["Earth"] = "Earth.atm.cfg";
}



// ***************************************************************************************************
// Credist Dialog
// ***************************************************************************************************

INT_PTR CALLBACK VideoTab::CreditsDlgProcWrp(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	static class VideoTab *VTab = NULL;
	switch (uMsg) {
		case WM_INITDIALOG: 
			VTab = (class VideoTab *)lParam;
			VTab->InitCreditsDialog(hWnd);
			return true;
		case WM_COMMAND:
			if (VTab) VTab->CreditsDlgProc(hWnd, uMsg, wParam, lParam);
	}
	return false;
}



INT_PTR CALLBACK VideoTab::CreditsDlgProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (LOWORD(wParam)) {
		case IDOK:
		case IDCANCEL:
			EndDialog (hWnd, 0);
			break;
	}
	return false;
}

// ==============================================================
// Was CreateFile / GetFileSize / ReadFile / CloseHandle over a HANDLE, with
// INVALID_HANDLE_VALUE as the failure test.
//
// NONE OF THOSE EXIST HERE, and this is not a Direct3D difference: the shim
// implements no kernel file handles and defines no INVALID_HANDLE_VALUE,
// because reading a file is what stdio is for. Exactly the decision
// D3D9Util.cpp's shader cache took, and taken again here so the two agree.
// The reading is byte-for-byte the same: open, size, read the whole thing into
// a zeroed buffer one larger, hand it over, free it, close.
//
// The path follows the client's rename. It is already forward-slashed on
// Windows, so only "D3D9Client" changes -- but the log line below spelled the
// same path with backslashes, and that one is corrected too so the two agree.
//
// SEE THE FILE HEADER ON EM_SETTEXTEX: the message now exists but no rich edit
// control does, so the box will be empty until Win32Dlg.cpp grows one.
//
void VideoTab::InitCreditsDialog(HWND hWnd)
{
	FILE *pFile = fopen("Modules/VulkanClient/Credits.rtf", "rb");

	if (pFile==NULL) {
		LogErr("Failed to open a file /Modules/VulkanClient/Credits.rtf");
		return;
	}

	fseek(pFile, 0, SEEK_END);
	long fsize = ftell(pFile);
	fseek(pFile, 0, SEEK_SET);

	if (fsize < 0) {
		LogErr("Failed to read a file /Modules/VulkanClient/Credits.rtf");
		fclose(pFile);
		return;
	}

	size_t size = size_t(fsize);
	char *credits = new char[size+1];
	memset(credits,0,size+1);

	if (fread(credits, 1, size, pFile) == size) {
		SETTEXTEX text;
		text.flags = ST_DEFAULT;
		text.codepage = CP_ACP;
		SendDlgItemMessageA(hWnd, IDC_CREDITSTEXT, EM_SETTEXTEX, (WPARAM)&text, (LPARAM)credits);
	}
	else LogErr("Failed to read a file /Modules/VulkanClient/Credits.rtf");

	delete []credits;
	credits = NULL;

	fclose(pFile);
}

bool VideoTab::GetConfigName(const char* file, std::string& cfg, std::string& planet)
{
	// "GC\\" + file. oapiOpenFile resolves this against the config directory
	// and opens it, so the separator is a filesystem separator -- finding 24.
	std::string filename = "GC/" + std::string(file);
	FILEHANDLE hFile = oapiOpenFile(filename.c_str(), FILE_IN_ZEROONFAIL, CONFIG);
	if (hFile) {
		char ConfigName[32] = {}; char PlanetName[32] = {};
		bool bA = oapiReadItem_string(hFile, (char*)"ConfigName", ConfigName);
		bool bB = oapiReadItem_string(hFile, (char*)"Planet", PlanetName);
		oapiCloseFile(hFile, FILE_IN_ZEROONFAIL);
		cfg = std::string(ConfigName);
		planet = std::string(PlanetName);
		return bA && bB;
	}
	return false;
}

// ==============================================================
// Was WIN32_FIND_DATA + FindFirstFileA("<cfg>GC\\*_atm.cfg") + FindNextFileA +
// FindClose: the Win32 directory walk, and the fourth distinct spelling of one
// in this client after <io.h>'s _findfirst, vPlanet's FindFirstFile and the
// shim's own.
//
// std::filesystem::directory_iterator, as in vPlanet::EnumerateDirectory, and
// with the same care about case: FindFirstFile's "*_atm.cfg" match is
// case-INSENSITIVE on NTFS, so a file named Mars_ATM.cfg is found on Windows
// and would be skipped by a literal suffix compare here. The directory test
// replaces the FILE_ATTRIBUTE_DIRECTORY check and the leading-dot test, which
// existed to skip "." and ".." -- entries directory_iterator does not produce
// at all, so that test is kept only for genuinely dot-prefixed names.
//
void VideoTab::ScanAtmoCfgs()
{
	_AtmoCfg cfg = { "Default", "Earth.atm.cfg"};
	AtmoCfgs["Earth"].push_back(cfg);

	namespace fs = std::filesystem;

	std::string dir = std::string(OapiExtension::GetConfigDir()) + "GC/";

	std::error_code ec;
	fs::directory_iterator it(dir, ec);
	if (ec) return;		// FindFirstFileA returning INVALID_HANDLE_VALUE

	for (const fs::directory_entry &e : it) {

		if (e.is_directory(ec)) continue;

		const std::string name = e.path().filename().string();
		if (name.empty() || name[0] == '.') continue;

		// The wildcard "*_atm.cfg", case-insensitively.
		static const std::string suffix = "_atm.cfg";
		if (name.size() <= suffix.size()) continue;
		std::string tail = name.substr(name.size() - suffix.size());
		std::transform(tail.begin(), tail.end(), tail.begin(),
					   [](unsigned char c) { return (char)std::tolower(c); });
		if (tail != suffix) continue;

		std::string cfgname, planet;
		if (GetConfigName(name.c_str(), cfgname, planet)) {
			_AtmoCfg entry = { cfgname, name };
			AtmoCfgs[planet].push_back(entry);
		}
		else oapiWriteLogV("File Not Found [%s]", name.c_str());
	}
}

