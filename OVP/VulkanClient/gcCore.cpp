// ===================================================
// Copyright (C) 2021-2026 Jarmo Nikkanen
// licensed under LGPL v2
// ===================================================
//
// The add-on-facing implementation of gcCore.h. Most of it forwards one call
// and converts nothing; the custom swap chain is the one capability that is
// genuinely lost rather than respelled -- see the note above RegisterSwap.
// ===================================================


#include <vulkan/vulkan.h>
#include "VulkanTypes.h"
#include "gcCore.h"
#include "VulkanSurface.h"
#include "VulkanClient.h"
// Not in the Windows include list. The Sketchpad types reached this file
// through D3D9Surface.h -> D3D9Pad.h; VulkanSurface.h forward-declares
// VulkanPad instead, because that include cycle only survives on MSVC's guards.
#include "VulkanPad.h"
#include "Scene.h"
#include "VVessel.h"
#include "VPlanet.h"
#include "Surfmgr2.h"
#include "IProcess.h"

extern oapi::VulkanClient *g_client;
extern std::set<Font *> g_fonts;

// Src/Orbiter/Linux/UIHost.cpp. The destination half of RegisterSwap: it is
// what "present into that child window" means when the process has one window
// and the child is a rectangle the UI host draws.
extern "C" void orbiter_SetControlImage(HWND h, void *imageView, int w, int hgt);


// ===============================================================================================
// Was { LPDIRECT3DSWAPCHAIN9 pSwap; LPDIRECT3DSURFACE9 pBack; SURFHANDLE hSurf; }.
// Both D3D9 handles go with the swap chain itself (see RegisterSwap below),
// leaving the SURFHANDLE the three surviving accessors hand back.
// ===============================================================================================

class gcSwap
{
public:
	gcSwap() : hSurf(NULL), hWnd(NULL) { }
	~gcSwap() { Release(); }
	void Release() {
		// The control stops showing this image before the surface -- and with
		// it the VkImageView the UI host holds -- is destroyed. The other
		// order leaves a dangling view in g_controlImages for as long as the
		// next frame takes to notice.
		if (hWnd) orbiter_SetControlImage(hWnd, NULL, 0, 0);
		DELETE_SURFACE(hSurf);
	}
	SURFHANDLE hSurf;
	HWND       hWnd;	// which control this swap presents into; see RegisterSwap
};



DLLCLBK void gcBindCoreMethod(void** ppFnc, const char* name)
{
	*ppFnc = NULL;
#define binder_start
	if (strcmp(name,"RegisterSwap")==0) *ppFnc = (void*)&gcCore2::RegisterSwap;
	if (strcmp(name,"FlipSwap")==0) *ppFnc = (void*)&gcCore2::FlipSwap;
	if (strcmp(name,"GetRenderTarget")==0) *ppFnc = (void*)&gcCore2::GetRenderTarget;
	if (strcmp(name,"ReleaseSwap")==0) *ppFnc = (void*)&gcCore2::ReleaseSwap;
	if (strcmp(name,"DeleteCustomCamera")==0) *ppFnc = (void*)&gcCore2::DeleteCustomCamera;
	if (strcmp(name,"CustomCameraOnOff")==0) *ppFnc = (void*)&gcCore2::CustomCameraOnOff;
	if (strcmp(name,"CustomCameraOverlay")==0) *ppFnc = (void*)&gcCore2::CustomCameraOverlay;
	if (strcmp(name,"SetCustomCameraSurfaceLabelScale")==0) *ppFnc = (void*)&gcCore2::SetCustomCameraSurfaceLabelScale;
	if (strcmp(name,"SetupCustomCamera")==0) *ppFnc = (void*)&gcCore2::SetupCustomCamera;
	if (strcmp(name,"SketchpadVersion")==0) *ppFnc = (void*)&gcCore2::SketchpadVersion;
	if (strcmp(name,"CreatePoly")==0) *ppFnc = (void*)&gcCore2::CreatePoly;
	if (strcmp(name,"CreateTriangles")==0) *ppFnc = (void*)&gcCore2::CreateTriangles;
	if (strcmp(name,"DeletePoly")==0) *ppFnc = (void*)&gcCore2::DeletePoly;
	if (strcmp(name,"GetTextLength")==0) *ppFnc = (void*)&gcCore2::GetTextLength;
	if (strcmp(name,"GetCharIndexByPosition")==0) *ppFnc = (void*)&gcCore2::GetCharIndexByPosition;
	if (strcmp(name,"RegisterRenderProc")==0) *ppFnc = (void*)&gcCore2::RegisterRenderProc;
	if (strcmp(name,"CreateSketchpadFont")==0) *ppFnc = (void*)&gcCore2::CreateSketchpadFont;
	if (strcmp(name,"GetMeshMaterial")==0) *ppFnc = (void*)&gcCore2::GetMeshMaterial;
	if (strcmp(name,"SetMeshMaterial")==0) *ppFnc = (void*)&gcCore2::SetMeshMaterial;
	if (strcmp(name,"GetMatrix")==0) *ppFnc = (void*)&gcCore2::GetMatrix;
	if (strcmp(name,"SetMatrix")==0) *ppFnc = (void*)&gcCore2::SetMatrix;
	if (strcmp(name,"GetDevMesh")==0) *ppFnc = (void*)&gcCore2::GetDevMesh;
	if (strcmp(name,"LoadDevMeshGlobal")==0) *ppFnc = (void*)&gcCore2::LoadDevMeshGlobal;
	if (strcmp(name,"ReleaseDevMesh")==0) *ppFnc = (void*)&gcCore2::ReleaseDevMesh;
	if (strcmp(name,"RenderMesh")==0) *ppFnc = (void*)&gcCore2::RenderMesh;
	if (strcmp(name,"PickMesh")==0) *ppFnc = (void*)&gcCore2::PickMesh;
	if (strcmp(name,"RenderLines")==0) *ppFnc = (void*)&gcCore2::RenderLines;
	if (strcmp(name,"GetSystemSpecs")==0) *ppFnc = (void*)&gcCore2::GetSystemSpecs;
	if (strcmp(name,"GetSurfaceSpecs")==0) *ppFnc = (void*)&gcCore2::GetSurfaceSpecs;
	if (strcmp(name,"LoadSurface")==0) *ppFnc = (void*)&gcCore2::LoadSurface;
	if (strcmp(name,"SaveSurface")==0) *ppFnc = (void*)&gcCore2::SaveSurface;
	if (strcmp(name,"GetMipSublevel")==0) *ppFnc = (void*)&gcCore2::GetMipSublevel;
	if (strcmp(name,"GenerateMipmaps")==0) *ppFnc = (void*)&gcCore2::GenerateMipmaps;
	if (strcmp(name,"CompressSurface")==0) *ppFnc = (void*)&gcCore2::CompressSurface;
	if (strcmp(name,"LoadBitmapFromFile")==0) *ppFnc = (void*)&gcCore2::LoadBitmapFromFile;
	if (strcmp(name,"GetRenderWindow")==0) *ppFnc = (void*)&gcCore2::GetRenderWindow;
	if (strcmp(name,"RegisterGenericProc")==0) *ppFnc = (void*)&gcCore2::RegisterGenericProc;
	if (strcmp(name,"StretchRectInScene")==0) *ppFnc = (void*)&gcCore2::StretchRectInScene;
	if (strcmp(name,"ClearSurfaceInScene")==0) *ppFnc = (void*)&gcCore2::ClearSurfaceInScene;
	if (strcmp(name,"ScanScreen")==0) *ppFnc = (void*)&gcCore2::ScanScreen;
	if (strcmp(name,"LockSurface")==0) *ppFnc = (void*)&gcCore2::LockSurface;
	if (strcmp(name,"ReleaseLock")==0) *ppFnc = (void*)&gcCore2::ReleaseLock;
	if (strcmp(name,"GetPlanetManager")==0) *ppFnc = (void*)&gcCore2::GetPlanetManager;
	if (strcmp(name,"SetTileOverlay")==0) *ppFnc = (void*)&gcCore2::SetTileOverlay;
	if (strcmp(name,"AddGlobalOverlay")==0) *ppFnc = (void*)&gcCore2::AddGlobalOverlay;
	if (strcmp(name,"GetTileData")==0) *ppFnc = (void*)&gcCore2::GetTileData;
	if (strcmp(name,"GetTile")==0) *ppFnc = (void*)&gcCore2::GetTile;
	if (strcmp(name,"HasTileData")==0) *ppFnc = (void*)&gcCore2::HasTileData;
	if (strcmp(name,"SeekTileTexture")==0) *ppFnc = (void*)&gcCore2::SeekTileTexture;
	if (strcmp(name,"SeekTileElevation")==0) *ppFnc = (void*)&gcCore2::SeekTileElevation;
	if (strcmp(name,"GetElevation")==0) *ppFnc = (void*)&gcCore2::GetElevation;
	if (strcmp(name,"CreateIPInterface")==0) *ppFnc = (void*)&gcCore2::CreateIPInterface;
	if (strcmp(name,"ReleaseIPInterface")==0) *ppFnc = (void*)&gcCore2::ReleaseIPInterface;
#define binder_end
	if (*ppFnc == NULL) oapiWriteLogV("ERROR:gcCoreAPI: Function [%s] failed to bind", name);
}


// ===============================================================================================
// Custom SwapChain Interface
// ===============================================================================================
//
// The swap chain itself does not cross; what it is for does.
//
// RegisterSwap asks D3D9 for an additional swap chain on an arbitrary HWND, so
// that an add-on can present its own rendering into its own child window. A
// literal translation needs three things that do not exist here:
//
//   * A VkSurfaceKHR for that window. It comes from a platform call needing a
//     real window-system handle, and there isn't one: an HWND here is
//     Src/Orbiter/Linux's own handle, and a child control is not an OS window
//     at all -- it is a rectangle UIHost::drawControl paints into the single
//     GLFW window's ImGui draw list. No X11 Window or wl_surface underneath it.
//
//   * A VkSwapchainKHR on it, with its images, views and render pass.
//
//   * A present. UIHost.cpp owns the only vkQueuePresentKHR in the process and
//     the acquire/submit/present semaphores with it; a client presenting on its
//     own would race the frame pump for the queue.
//
// So the mechanism changes and the contract does not. What the caller does with
// the handle -- DX9ExtMFD's MFDWindow::clbkRefreshDisplay is the consumer in
// this tree -- never touches a swap chain: it asks for a render target the size
// of that child window, draws into it, and says "show it there". Here the
// render target is an ordinary sampled surface, and "show it there" is UIHost
// drawing that image over the control's rectangle, which is precisely what
// presenting to that child window did. The add-on itself is unchanged.
//
// The surface flags, against the D3DPRESENT_PARAMETERS the reference filled in.
// BackBufferWidth/Height = 0 meant "the window's client area", which has to be
// read off the window explicitly here. X8R8G8B8 becomes OAPISURFACE_PF_XRGB:
// Vulkan has no X8 format, and NatCreateSurface answers B8G8R8A8_UNORM plus
// SetAlphaOne, which is what X8 meant. RENDER3D is kept because the reference's
// SurfNative flags ask for a depth buffer even though the chain does not (D3D9
// attaches it to the surface). OAPISURFACE_TEXTURE is the one addition: this
// image is consumed by a sampler rather than by Present, so it needs SAMPLED
// usage, and it settles the layout at SHADER_READ_ONLY_OPTIMAL -- where
// BlitTexture leaves it, so StretchRectInScene needs no extra barrier.
// BACKBUFFER is not passed on: it marks a surface as *the* back buffer for the
// client's own render-target bookkeeping, and this is not that. Swap effect,
// presentation interval, refresh rate and multisample are all swap-chain
// properties with nothing to carry; the AA parameter was ignored by the
// reference too and stays ignored.
// ===============================================================================================
//
HSWAP gcCore::RegisterSwap(HWND hWnd, HSWAP hData, int AA) 
{ 
	gcSwap *pData = (gcSwap*)hData;

	if (!hWnd) {
		LogErr("gcCore::RegisterSwap: no window");
		return NULL;
	}

	RECT rc = { 0, 0, 0, 0 };
	GetClientRect(hWnd, &rc);
	const DWORD w = (DWORD)(rc.right - rc.left);
	const DWORD h = (DWORD)(rc.bottom - rc.top);

	if (w == 0 || h == 0) {
		// A zero-sized window still made a swap chain on Windows, but
		// clbkCreateSurfaceEx returns NULL for a zero surface, so it is
		// refused here rather than left to assert. Resize() calls this again
		// the moment the control has a size.
		LogErr("gcCore::RegisterSwap: window has a zero client area (%ux%u)", w, h);
		return NULL;
	}

	SURFHANDLE hSurf = g_client->clbkCreateSurfaceEx(w, h,
		OAPISURFACE_RENDERTARGET | OAPISURFACE_TEXTURE |
		OAPISURFACE_RENDER3D     | OAPISURFACE_PF_XRGB);

	if (!hSurf) {
		LogErr("gcCore::RegisterSwap: failed to create a %ux%u render target", w, h);
		return NULL;
	}

	if (!pData) pData = new gcSwap();
	else pData->Release();

	SURFACE(hSurf)->SetName("SwapChainBackBuffer");

	pData->hSurf = hSurf;
	pData->hWnd  = hWnd;

	// Nothing has been drawn into it yet, so it is not published until the
	// first FlipSwap -- the same moment the reference's first Present would
	// have put something on screen.
	return HSWAP(pData);
}


// ===============================================================================================
//
// Was HR(pSwap->Present(0, 0, 0, 0, 0)). The present becomes the publication:
// from this call on, UIHost draws this image over the control's rectangle, once
// per frame, until the next RegisterSwap or ReleaseSwap -- the same way the
// driver held the reference's presented image until the next Present.
//
// Deliberately re-published on every call rather than only the first: the
// surface can be re-created underneath a live gcSwap (Resize).
void gcCore::FlipSwap(HSWAP hSwap) 
{ 
	if (!hSwap) return;

	gcSwap *pData = (gcSwap*)hSwap;
	if (!pData->hSurf || !pData->hWnd) return;

	// The sampling view, which SetAlphaOne has swizzled alpha=1 on because the
	// surface was asked for as PF_XRGB. That is what makes the picture opaque:
	// a D3D9 back buffer has no alpha channel, and nothing behind the child
	// window was ever visible through it. Measured -- the MFD background reads
	// srgb(0,0,0), not the dialog face.
	VulkanTexture *pTex = SURFACE(pData->hSurf)->GetTexture();
	if (!pTex) return;

	orbiter_SetControlImage(pData->hWnd, (void*)pTex->View(),
							(int)pTex->Width(), (int)pTex->Height());
}


// ===============================================================================================
//
SURFHANDLE gcCore::GetRenderTarget(HSWAP hSwap) 
{ 
	return hSwap ? ((gcSwap*)hSwap)->hSurf : NULL;
}

// ===============================================================================================
//
void gcCore::ReleaseSwap(HSWAP hSwap) 
{ 
	if (hSwap) delete ((gcSwap*)hSwap);
}






// ===============================================================================================
// Custom Camera Interface
// ===============================================================================================
//
CAMERAHANDLE gcCore::SetupCustomCamera(CAMERAHANDLE hCam, OBJHANDLE hVessel, VECTOR3 &pos, VECTOR3 &dir, VECTOR3 &up, double fov, SURFHANDLE hSurf, DWORD flags)
{
	VECTOR3 x = crossp(up, dir);
	MATRIX3 mTake;
	mTake.m11 = x.x;	mTake.m21 = x.y;	mTake.m31 = x.z;
	mTake.m12 = up.x;	mTake.m22 = up.y;	mTake.m32 = up.z;
	mTake.m13 = dir.x;	mTake.m23 = dir.y;	mTake.m33 = dir.z;
	Scene *pScene = g_client->GetScene();
	return pScene ? pScene->SetupCustomCamera(hCam, hVessel, mTake, pos, fov, hSurf, flags) : NULL;
}


// ===============================================================================================
//
void gcCore::CustomCameraOnOff(CAMERAHANDLE hCam, bool bOn)
{
	Scene *pScene = g_client->GetScene();
	if (pScene) {
		pScene->CustomCameraOnOff(hCam, bOn);
	}
}


// ===============================================================================================
//
void gcCore::CustomCameraOverlay(CAMERAHANDLE hCam, __gcRenderProc clbk, void *pUser)
{
	CAMERA(hCam)->pRenderProc = clbk;
	CAMERA(hCam)->pUser = pUser;
}


// ===============================================================================================
//
int gcCore::DeleteCustomCamera(CAMERAHANDLE hCam)
{
	Scene *pScene = g_client->GetScene();
	return pScene ? pScene->DeleteCustomCamera(hCam) : 0;
}

// ===============================================================================================
//
void gcCore::SetCustomCameraSurfaceLabelScale(CAMERAHANDLE hCam, float scale)
{
	Scene* pScene = g_client->GetScene();

	if(pScene) {
		pScene->SetCustomCameraSurfaceLabelScale(hCam, scale);
	}
}






// ===============================================================================================
// SketchPad Interface
// ===============================================================================================


// ===============================================================================================
//
int gcCore::SketchpadVersion(Sketchpad* pSkp)
{
	return ((VulkanPad*)pSkp)->GetVersion();
}


// ===============================================================================================
//
oapi::Font* gcCore::CreateSketchpadFont(int height, char* face, int width, int weight, FontStyle Style, float spacing)
{
	return g_client->clbkCreateFontEx(height, face, width, weight, Style, spacing);
}


// ===============================================================================================
//
HPOLY gcCore::CreatePoly(HPOLY hPoly, const FVECTOR2 *pt, int npt, DWORD flags)
{
	VulkanDevice *pDev = g_client->GetDevice();
	if (!hPoly) return new VulkanPolyLine(pDev, pt, npt, (flags&PF_CONNECT) != 0);
	((VulkanPolyLine *)hPoly)->Update(pt, npt, (flags&PF_CONNECT) != 0);
	return hPoly;
}


// ===============================================================================================
//
HPOLY gcCore::CreateTriangles(HPOLY hPoly, const gcCore::clrVtx *pt, int npt, DWORD flags)
{
	VulkanDevice *pDev = g_client->GetDevice();
	if (!hPoly) return new VulkanTriangle(pDev, pt, npt, flags);
	((VulkanTriangle *)hPoly)->Update(pt, npt);
	return hPoly;
}


// ===============================================================================================
//
void gcCore::DeletePoly(HPOLY hPoly)
{
	if (hPoly) {
		((VulkanPolyBase *)hPoly)->Release();
		delete ((VulkanPolyBase *)hPoly);
	}
}


// ===============================================================================================
//
DWORD gcCore::GetTextLength(oapi::Font *hFont, const char *pText, int len)
{
	return DWORD((static_cast<VulkanPadFont *>(hFont))->GetTextLength(pText, len));
}


// ===============================================================================================
//
DWORD gcCore::GetCharIndexByPosition(oapi::Font *hFont, const char *pText, int pos, int len)
{
	return DWORD((static_cast<VulkanPadFont *>(hFont))->GetIndexByPosition(pText, pos, len));
}


// ===============================================================================================
//
bool gcCore::RegisterRenderProc(__gcRenderProc proc, DWORD flags, void *pParam)
{
	return g_client->RegisterRenderProc(proc, flags, pParam);
}




// ===============================================================================================
// Mesh interface functions
// ===============================================================================================
//
int gcCore::GetMatrix(MatrixId matrix_id, OBJHANDLE hVessel, DWORD mesh, DWORD group, FMATRIX4 *pMat)
{
	if (oapiGetObjectType(hVessel) != OBJTP_VESSEL) return -10;
	Scene *pScn = g_client->GetScene();
	vVessel *pVes = (vVessel *)pScn->GetVisObject(hVessel);
	if (pVes) return pVes->GetMatrixTransform(matrix_id, mesh, group, pMat);
	return -11;
}


// ===============================================================================================
//
int gcCore::SetMatrix(MatrixId matrix_id, OBJHANDLE hVessel, DWORD mesh, DWORD group, const FMATRIX4 *pMat)
{
	if (oapiGetObjectType(hVessel) != OBJTP_VESSEL) return -10;
	Scene *pScn = g_client->GetScene();
	vVessel *pVes = (vVessel *)pScn->GetVisObject(hVessel);
	if (pVes) return pVes->SetMatrixTransform(matrix_id, mesh, group, pMat);
	return -11;
}


// ===============================================================================================
//
int gcCore::GetMeshMaterial(DEVMESHHANDLE hMesh, DWORD idx, MatProp prop, FVECTOR4* value)
{
	return g_client->clbkMeshMaterialEx(hMesh, idx, prop, value);
}


// ===============================================================================================
//
int gcCore::SetMeshMaterial(DEVMESHHANDLE hMesh, DWORD idx, MatProp prop, const FVECTOR4* value)
{
	return g_client->clbkSetMeshMaterialEx(hMesh, idx, prop, value);
}

// ===============================================================================================
//
DEVMESHHANDLE gcCore::GetDevMesh(MESHHANDLE hMesh)
{
	return g_client->GetDevMesh(hMesh);
}


// ===============================================================================================
//
DEVMESHHANDLE gcCore::LoadDevMeshGlobal(const char* file_name, bool bUseCache)
{
	MESHHANDLE hMesh = oapiLoadMeshGlobal(file_name);
	return g_client->GetDevMesh(hMesh);
}


// ===============================================================================================
//
void gcCore::ReleaseDevMesh(DEVMESHHANDLE hMesh)
{
	delete (VulkanMesh*)(hMesh);
}


// ===============================================================================================
//
void gcCore::RenderMesh(DEVMESHHANDLE hMesh, const oapi::FMATRIX4* pWorld)
{
	Scene* pScene = g_client->GetScene();
	pScene->RenderMesh(hMesh, pWorld);
}


// ===============================================================================================
//
bool gcCore::PickMesh(PickMeshStruct* pm, DEVMESHHANDLE hMesh, const FMATRIX4* pWorld, short x, short y)
{
	Scene* pScene = g_client->GetScene();
	// The (const LPD3DXMATRIX) cast is gone rather than renamed: Scene::PickMesh
	// takes a const FMATRIX4*, which is what the caller already holds.
	VulkanPick pk = pScene->PickMesh(hMesh, pWorld, x, y);

	if (pk.group >= 0) {
		if (pk.dist < pm->dist) {
			pm->pos = _FV(pk.pos);
			pm->normal = _FV(pk.normal);
			pm->grp_inst = pk.group;
			pm->dist = pk.dist;
			return true;
		}
	}
	return false;
}







// ===============================================================================================
// Custom Render Interface
// ===============================================================================================
//
// ===============================================================================================
//
SURFHANDLE gcCore::LoadSurface(const char* fname, DWORD flags)
{
	return g_client->clbkLoadSurface(fname, flags);
}

// ===============================================================================================
//
bool gcCore::SaveSurface(const char* file, SURFHANDLE hSrf)
{
	return NatSaveSurface(file, SURFACE(hSrf)->GetResource());
}

// ===============================================================================================
//
SURFHANDLE gcCore::GetMipSublevel(SURFHANDLE hSrf, int level)
{
	return NatGetMipSublevel(hSrf, level);
}

// ===============================================================================================
//
bool gcCore::GenerateMipmaps(SURFHANDLE hSurface)
{
	return NatGenerateMipmaps(hSurface);
}

// ===============================================================================================
//
SURFHANDLE gcCore::CompressSurface(SURFHANDLE hSurface, DWORD flags)
{
	return NatCompressSurface(hSurface, flags);
}


// ===============================================================================================
//
void gcCore::RenderLines(const FVECTOR3* pVtx, const WORD* pIdx, int nVtx, int nIdx, const FMATRIX4* pWorld, DWORD color)
{
	// Both D3DX casts disappear: VulkanEffect::RenderLines takes exactly the
	// types this function was handed.
	VulkanEffect::RenderLines(pVtx, pIdx, nVtx, nIdx, pWorld, color);
}


// ===============================================================================================
//
bool gcCore::StretchRectInScene(SURFHANDLE tgt, SURFHANDLE src, LPRECT tr, LPRECT sr)
{
	if (S_OK == g_client->BeginScene())
	{
		VulkanTexture *pss = SURFACE(src)->GetSurface();
		VulkanTexture *pts = SURFACE(tgt)->GetSurface();
		// StretchRect(src, srcRect, dst, dstRect, filter) names the source
		// first; BlitTexture names the destination first, matching its own
		// whole-image form and memcpy, so the two pairs swap here.
		// D3DTEXF_LINEAR is BlitTexture's default filter.
		bool bOK = g_client->GetDevice()->BlitTexture(pts, tr, pss, sr);
		g_client->EndScene();
		return bOK;
	}
	return false;
}

// ===============================================================================================
//
bool gcCore::ClearSurfaceInScene(SURFHANDLE tgt, DWORD color, LPRECT tr)
{
	if (S_OK == g_client->BeginScene())
	{
		VulkanTexture *pts = SURFACE(tgt)->GetSurface();
		// ColorFill(surface, rect, D3DCOLOR) -> ClearImage(image, rect, DWORD).
		// The (D3DCOLOR) cast goes with the type: a D3DCOLOR was a DWORD
		// 0xAARRGGBB, the same bytes in the same order.
		bool bOK = g_client->GetDevice()->ClearImage(pts, tr, color);
		g_client->EndScene();
		return bOK;
	}
	return false;
}





// ===============================================================================================
// Some Helper Functions
// ===============================================================================================
//
// ===============================================================================================
//
gcCore::PickGround gcCore::ScanScreen(int scr_x, int scr_y)
{
	// PickGround holds an oapi::DRECT, which has a user-declared copy
	// constructor, so a raw memset over it is -Wclass-memaccess. The cast says
	// the clear is deliberate.
	PickGround pg; memset(static_cast<void*>(&pg), 0, sizeof(PickGround));

	Scene* pScene = g_client->GetScene();
	TILEPICK tp = pScene->PickSurface(scr_x, scr_y);
	SurfTile* pTile = static_cast<SurfTile*>(tp.pTile);

	if (pTile) {

		pTile->GetIndex(&pg.iLng, &pg.iLat);

		pg.Bounds.left = pTile->bnd.minlng;
		pg.Bounds.right = pTile->bnd.maxlng;
		pg.Bounds.top = pTile->bnd.maxlat;
		pg.Bounds.bottom = pTile->bnd.minlat;

		pg.lat = tp.lat;
		pg.lng = tp.lng;

		pg.emax = float(pTile->GetMaxElev());
		pg.emin = float(pTile->GetMinElev());

		pg.msg = 0;
		pg.dist = tp.d;
		pg.elev = tp.elev;
		pg.level = pTile->Level();
		pg.hTile = HTILE(pTile);
		pg.normal = _FV(tp._n);
		pg.pos = _FV(tp._p);
	}
	return pg;
}


// ===============================================================================================
//
void gcCore::GetSystemSpecs(SystemSpecs* sp, int size)
{
	if (size == sizeof(SystemSpecs)) {
		sp->DisplayMode = g_client->GetFramework()->GetDisplayMode();
		// D3DCAPS9::MaxTextureWidth -> VkPhysicalDeviceLimits::maxImageDimension2D.
		sp->MaxTexSize = g_client->GetHardwareCaps()->limits.maxImageDimension2D;
		// D3DCAPS9::MaxTextureRepeat has no Vulkan counterpart -- it was the
		// largest texture coordinate the fixed-function sampler could still
		// wrap correctly, and Vulkan names no such ceiling. Add-ons read this,
		// so it reports "no limit" rather than being dropped. gcConst.cpp's
		// copy of this function has to say the same number.
		sp->MaxTexRep = 0xFFFFFFFF;
		sp->gcAPIVer = BuildDate();
	}
}

// ===============================================================================================
//
bool gcCore::GetSurfaceSpecs(SURFHANDLE hSrf, SurfaceSpecs* sp, int size)
{
	return SURFACE(hSrf)->GetSpecs(sp, size);
}


// ===============================================================================================
//
bool gcCore::RegisterGenericProc(__gcGenericProc proc, DWORD id, void* pParam)
{
	return g_client->RegisterGenericProc(proc, id, pParam);
}


// ===============================================================================================
//
HBITMAP	gcCore::LoadBitmapFromFile(const char* fname)
{
	return g_client->gcReadImageFromFile(fname);
}


// ===============================================================================================
//
HWND gcCore::GetRenderWindow()
{
	return g_client->GetRenderWindow();
}


// ===============================================================================================
// gcCore2 Interface --- Tile access interface functions
// ===============================================================================================
//

HPLANETMGR gcCore2::GetPlanetManager(OBJHANDLE hPlanet)
{
	Scene *pScene = g_client->GetScene();
	vPlanet *vPl = (vPlanet *)pScene->GetVisObject(hPlanet);
	return HPLANETMGR(vPl);
}


// ===============================================================================================
//
HTILE gcCore2::GetTile(HPLANETMGR vPl, double lng, double lat, int maxlevel)
{
	vPlanet *vP = static_cast<vPlanet *>(vPl);
	return HTILE(vP->FindTile(lng, lat, maxlevel));
}


// ===============================================================================================
//
gcCore::PickGround gcCore2::GetTileData(HPLANETMGR vPl, double lng, double lat, int maxlevel)
{
	// See ScanScreen above: DRECT is not trivially copyable.
	PickGround pg; memset(static_cast<void*>(&pg), 0, sizeof(PickGround));
	if (!vPl) return pg;

	vPlanet *vP = static_cast<vPlanet *>(vPl);
	SurfTile *pTile = static_cast<SurfTile *>(vP->FindTile(lng, lat, maxlevel));

	if (!pTile) {
		oapiWriteLogV("gcCore::FindTile() Failed");
		return pg;
	}

	pTile->GetIndex(&pg.iLng, &pg.iLat);
	pTile->GetElevation(lng, lat, &pg.elev, &pg.normal, NULL, true, false);

	VECTOR3 pos = vP->GetUnitSurfacePos(lng, lat) * (vP->GetSize() + pg.elev);
	MATRIX3 mRot; oapiGetRotationMatrix(vP->Object(), &mRot);

	pos = mul(mRot, pos) + vP->PosFromCamera();

	pg.Bounds.left = pTile->bnd.minlng;
	pg.Bounds.right = pTile->bnd.maxlng;
	pg.Bounds.top = pTile->bnd.maxlat;
	pg.Bounds.bottom = pTile->bnd.minlat;

	pg.lat = lat;
	pg.lng = lng;

	pg.emax = float(pTile->GetMaxElev());
	pg.emin = float(pTile->GetMinElev());

	pg.msg = 0;
	pg.dist = length(pos);
	pg.level = pTile->Level();
	pg.hTile = HTILE(pTile);
	pg.pos = FVECTOR3(pos);

	return pg;
}

// ===============================================================================================
//
bool gcCore2::SeekTileElevation(HPLANETMGR hMgr, int iLng, int iLat, int level, int flags, ElevInfo *pInfo)
{
	ELEVFILEHEADER hdr;
	if (!hMgr) return false;
	if (((vPlanet*)(hMgr))->SurfMgr2()) {
		float* pData = ((vPlanet*)(hMgr))->SurfMgr2()->BrowseElevationData(level, iLat, iLng, flags, &hdr);
		if (!pData) return false;
		pInfo->MaxElev = hdr.emax;
		pInfo->MinElev = hdr.emin;
		pInfo->MeanElev = hdr.emean;
		pInfo->Resolution = hdr.scale;
		pInfo->Offset = hdr.offset;
		pInfo->pElevData = pData;
		return true;
	}
	return false;
}


// ===============================================================================================
//
SURFHANDLE gcCore2::SeekTileTexture(HPLANETMGR hMgr, int iLng, int iLat, int level, int flags, void *reserved)
{
	if (!hMgr) return NULL;
	if (((vPlanet *)(hMgr))->SurfMgr2()) {
		return ((vPlanet *)(hMgr))->SurfMgr2()->SeekTileTexture(iLng, iLat, level, flags);
	}
	return NULL;
}


// ===============================================================================================
//
bool gcCore2::HasTileData(HPLANETMGR hMgr, int iLng, int iLat, int level, int flags)
{
	if (!hMgr) return false;
	if (((vPlanet *)(hMgr))->SurfMgr2()) {
		return ((vPlanet *)(hMgr))->SurfMgr2()->HasTileData(iLng, iLat, level, flags);
	}
	return false;
}


// ===============================================================================================
//
int gcCore2::GetElevation(HTILE hTile, double lng, double lat, double *out_elev)
{
	SurfTile *pTile = static_cast<SurfTile *>(hTile);
	return pTile->GetElevation(lng, lat, out_elev, NULL, NULL, true, true);
}


// ===============================================================================================
//
SURFHANDLE gcCore2::SetTileOverlay(HTILE hTile, const SURFHANDLE hOverlay)
{
	// Already disabled on Windows: the body is commented out there too. The
	// commented lines follow the type renames so that whoever re-enables them
	// is not reading D3D9 in a Linux file.
	//SurfTile *pTile = static_cast<SurfTile *>(hTile);
	//VulkanTexture *pTex = SURFACE(hOverlay)->GetTexture();
	//return HSURFNATIVE(pTile->SetOverlay(pTex, true));
	return NULL;
}


// ===============================================================================================
//
HOVERLAY gcCore2::AddGlobalOverlay(HPLANETMGR hMgr, VECTOR4 mmll, OlayType type, const SURFHANDLE hOverlay, HOVERLAY hOld, const FVECTOR4* pBlend)
{
	if (!hMgr) return NULL;
	vPlanet *vP = static_cast<vPlanet *>(hMgr);
	vPlanet::sOverlay* oLay = static_cast<vPlanet::sOverlay*>(hOld);
	if (hOverlay) {
		VulkanTexture *pTex = SURFACE(hOverlay)->GetTexture();
		return vP->AddOverlaySurface(mmll, type, pTex, oLay, pBlend);
	}
	return vP->AddOverlaySurface(mmll, type, NULL, oLay, pBlend);
}

// ===============================================================================================
//
// The Windows version of this function, and of ReleaseLock below, locks the
// handle itself rather than the resource behind it: it asks
// SURFACE(hSrf)->GetResource() for the resource type, then calls LockRect on
// `hSrf` cast to the matching D3D9 interface. A SURFHANDLE is void*, and every
// SURFHANDLE in this client is a SurfNative* -- a plain class with no virtual
// functions, whose first member is `char name[128]`. So that call reads a
// vtable pointer out of the first eight characters of the surface's name and
// jumps through it. The correct expression is the one the function has already
// computed two lines above, and the conversion cannot reproduce the mistake:
// Map() is called on the VulkanTexture GetResource() returns.
//
// D3DLOCK_DONOTWAIT has no counterpart and needs none. It asked the D3D9
// runtime not to block while the GPU still held the resource; host-visible
// memory is mappable at any time and the caller owns the synchronisation, so
// `bWait` has nothing left to select. It stays in the signature because
// gcCore.h is the public interface.
//
bool gcCore::LockSurface(SURFHANDLE hSrf, Lock* pOut, bool bWait)
{
	VulkanTexture *pResource = SURFACE(hSrf)->GetResource();
	if (!pResource) return false;

	void *pData = pResource->Map();
	if (!pData) return false;

	pOut->pData = pData;
	pOut->Pitch = DWORD(pResource->RowPitch());
	return true;
}


// ===============================================================================================
//
void gcCore::ReleaseLock(SURFHANDLE hSrf)
{
	VulkanTexture *pResource = SURFACE(hSrf)->GetResource();
	if (pResource) pResource->Unmap();
}

// ===============================================================================================
//
gcIPInterface* gcCore2::CreateIPInterface(const char* file, const char* PSEntry, const char* VSEntry, const char* ppf)
{
	ImageProcessing* pIPI = new ImageProcessing(g_client->GetDevice(), file, PSEntry, ppf);

	if (pIPI->IsOK() == false) {
		oapiWriteLogV("gcCore::CreateIPInterface() Failed !  File = [%s]", file);
		return NULL;
	}
	return new gcIPInterface(pIPI);
}

// ===============================================================================================
//
void gcCore2::ReleaseIPInterface(gcIPInterface* pIPI)
{
	if (!pIPI) return;
	if (pIPI->pIPI) delete pIPI->pIPI;
	delete pIPI;
}
