// ==============================================================
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2006-2026 Martin Schweiger
//				 2012-2016 Jarmo Nikkanen
// ==============================================================

// CONVERTED FROM OVP/D3D9Client/VideoTab.h, read end to end (68 lines).
//
// The Launchpad video tab. It is a WIN32 DIALOG and nothing else -- HWND,
// HINSTANCE, WPARAM, LPARAM, INT_PTR and CALLBACK are all Win32, not
// Direct3D, and the shim supplies every one of them. So one type changes:
//
//   oapi::VulkanClient -> oapi::VulkanClient
//
// Two more things are made explicit rather than changed: `string` is spelled
// `std::string` (the Windows file relies on a `using namespace std` reaching
// it from an includer, which is not guaranteed here), and VulkanClient.h is
// included by name because the class is used, not merely pointed at through
// a declaration the includer happened to have.

#ifndef __VIDEOTAB_H
#define __VIDEOTAB_H
#include "VulkanClient.h"
#include <string>
#include <vector>
#include <map>

// ==============================================================

class VideoTab {

	struct _AtmoCfg { std::string cfg, file; };
public:
	VideoTab(oapi::VulkanClient *gc, HINSTANCE _hInst, HINSTANCE _hOrbiterInst, HWND hVideoTab);
	~VideoTab();

	BOOL WndProc (HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
	// Video tab message handler

	void UpdateConfigData();
	// copy dialog state back to parameter structure

	bool Initialise();
	// Initialise dialog elements

protected:
	void SelectFullscreen(bool);
	void SelectMode(DWORD index);
	bool SelectAdapter(DWORD index);
	// Update dialog after user device selection

	void SelectWidth();
	// Update dialog after window width selection

	void SelectHeight();
	// Update dialog after window height selection

private:
	static INT_PTR CALLBACK SetupDlgProcWrp(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
	static INT_PTR CALLBACK CreditsDlgProcWrp(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
	INT_PTR CALLBACK SetupDlgProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
	INT_PTR CALLBACK CreditsDlgProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
	void InitCreditsDialog(HWND hWnd);
	void CreateSymbolicLinks();
	void InitSetupDialog(HWND hWnd);
	void SaveSetupState(HWND hWnd);
	void ScanAtmoCfgs();
	bool GetConfigName(const char* file, std::string& cfg, std::string& planet);
	
	oapi::VulkanClient *gclient;
	HINSTANCE hOrbiterInst; // orbiter instance handle
	HINSTANCE hInst;        // module instance handle
	HWND hTab;              // window handle of the video tab
	int aspect_idx;
	DWORD SelectedAdapterIdx;
	bool bHasMultiSample;
	std::map<std::string, std::vector<_AtmoCfg>> AtmoCfgs;
};

//};

#endif // !__VIDEOTAB_H

