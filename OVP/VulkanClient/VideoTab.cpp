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
// Direct3DCreate9() gave the Windows client a g_pD3DObject it could enumerate
// hardware with before any device existed -- exactly what a Launchpad tab
// needs. Its three uses split three ways here: adapters come from the core's
// VkInstance, display modes from the window system (Vulkan does not enumerate
// them at all), and caps from VkPhysicalDeviceLimits.
//
// The credits box will be empty. EM_SETTEXTEX exists in the shim's richedit.h
// so this file compiles as written, but Win32Dlg.cpp implements no rich edit
// control, so the message is never answered. Substituting WM_SETTEXT would put
// raw RTF markup on screen instead.
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
// For ScanAtmoCfgs, which replaces FindFirstFile's case-insensitive wildcard.
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
// Replaces GetAdapterCount() + GetAdapterIdentifier(). The VkInstance is the
// core's -- the client creates none, and orbiter_GetVulkanContext is callable
// before the Launchpad has drawn a frame, which is precisely this case.
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
			// This case has no break, on Windows either. Any IDC_VID_BPP
			// notification that is not CBN_SELCHANGE falls into IDC_VID_FULL
			// below, and BN_CLICKED is 0 -- so a combo notification with a
			// zero high word switches the dialog to full screen and sets
			// data->fullscreen = true. Left as written, because adding the
			// break changes what the dialog does; the marker only tells GCC
			// the fall-through was seen (-Wextra reports it, MSVC does not).
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
	// Replaces D3DDISPLAYMODE and D3DADAPTER_IDENTIFIER9. The mode's format
	// went with the query that took it; of the identifier only Description was
	// ever read.
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
		// deviceName is what D3DADAPTER_IDENTIFIER9::Description was.
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties(devices[i], &props);
		LogAlw("Adapter %d: %s", i, props.deviceName);
		SendDlgItemMessageA(hTab, IDC_VID_DEVICE, CB_ADDSTRING, 0, (LPARAM)props.deviceName);
	}

	SendDlgItemMessage(hTab, IDC_VID_DEVICE, CB_SETCURSEL, data->deviceidx, 0);


	// Was GetAdapterDisplayMode(). Vulkan cannot say what the desktop is
	// showing; the window system can, and UIHost.cpp publishes it.
	orbiter_GetCurrentVideoMode(&curW, &curH, &curHz);

	LogAlw("Current Mode W=%u, H=%u", curW, curH);

	// The D3DFMT_X8R8G8B8 argument to GetAdapterModeCount has no counterpart:
	// it asked which modes the adapter offers in one back-buffer format, and
	// the swapchain format here is the core's choice.
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
// The mode list comes back the same whichever adapter is picked: D3D9
// enumerated modes per adapter, glfwGetVideoModes enumerates them per monitor,
// and the resolutions a monitor accepts do not depend on which GPU drives it.
// The rebuild is kept so the combo is still reset and reselected, which is
// what the rest of the dialog expects after a device change.
//
bool VideoTab::SelectAdapter(DWORD index)
{

	SelectedAdapterIdx = index; 

	GraphicsClient::VIDEODATA *data = gclient->GetVideoData();

	std::vector<VkPhysicalDevice> devices;

	if (!GetPhysicalDevices(devices)) {
		LogErr("VideoTab::SelectAdapter(%u) No Vulkan instance available", index);
		return false;
	}
	else {

		char cbuf[32];

		if (devices.size()<=index) {
			LogErr("Adapter Index out of range");
			return false;
		}

		// A GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &curMode) stood here
		// whose result was never read; it went with the variable.

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
// The scenario block below needed two shim additions, GetAncestor and
// EnumChildWindows -- both pure walks over the window tree Win32Dlg.cpp already
// maintains. It looks for Orbiter's Launchpad scenario tree by control id,
// which on Linux is an ImGui tree in UIHost.cpp and not a window at all, so
// EnumChildWindows finds nothing and the function takes its own "FAILED to get
// a handle of a scenario dialog" path -- one the Windows version also handles.
//
// Nothing reads the result either way: SetScenarioName writes
// VulkanClient::scenarioName, which is never read on either platform.

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
		// Was find_last_not_of('\\'), and path += "\\" below. This string is
		// opened as a file, so the separator has to be '/'.
		path.erase( path.find_last_not_of( '/' )+1 ); // trim trailing path-delimiter

		char buf[MAX_PATH];
		// Was = {0}; -Wextra reports that as leaving members uninitialised even
		// though the language value-initialises all of them. = {} is identical.
		TVITEMA tvItem = {};
		tvItem.mask = TVIF_TEXT | TVIF_HANDLE;
		tvItem.pszText = buf;
		tvItem.cchTextMax = ARRAYSIZE(buf);

		for (auto it = hNodes.crbegin(); it != hNodes.crend(); ++it) {
			tvItem.hItem = *it;
			// (void) discards the macro's BOOL, which this call always
			// discarded; GCC reports the unused computed value under -Wall.
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

	// Was D3DCAPS9 caps + GetDeviceCaps(). D3DCAPS9 covered limits, format
	// support and feature bits in one struct; Vulkan splits the three, and the
	// only field this file reads, MaxAnisotropy, is among the limits.
	float maxAniso = 1.0f;

	std::vector<VkPhysicalDevice> devices;

	if (!GetPhysicalDevices(devices) || devices.size() <= SelectedAdapterIdx) {
		LogErr("VideoTab::SelectAdapter(%u) No Vulkan device available", SelectedAdapterIdx);
		return;
	}

	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(devices[SelectedAdapterIdx], &props);
	maxAniso = props.limits.maxSamplerAnisotropy;

	// CheckDeviceMultiSampleType, asked once per sample count per format,
	// becomes one bitmask of the counts a colour attachment may have.
	//
	// The core's render pass is created with VK_SAMPLE_COUNT_1_BIT
	// (UIHost.cpp), so this combo offers what the hardware can do -- as the
	// D3D9 query also did -- not what the client can currently switch on.
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
	// int() at the comparison settles the signed/unsigned warning; the count
	// never exceeds INT_MAX.
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
	// Vulkan's sampler takes a float ratio. The cast truncates towards zero,
	// which is what the switch below expects.
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
// Was CreateFile / GetFileSize / ReadFile / CloseHandle over a HANDLE. The shim
// implements no kernel file handles and defines no INVALID_HANDLE_VALUE, so
// this is stdio, as the shader cache also is.
//
// EM_SETTEXTEX exists in the shim's richedit.h but Win32Dlg.cpp implements no
// rich edit control, so the box stays empty until it grows one.
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
	// Was "GC\\". oapiOpenFile resolves this against the config directory and
	// opens it, so the separator is a filesystem separator.
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
// FindClose, now std::filesystem::directory_iterator. FindFirstFile's
// "*_atm.cfg" match is case-insensitive on NTFS, so Mars_ATM.cfg is found on
// Windows and would be skipped by a literal suffix compare -- hence the
// lowercasing below. The leading-dot test existed to skip "." and "..", which
// directory_iterator does not produce; it is kept only for genuinely
// dot-prefixed names.
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

