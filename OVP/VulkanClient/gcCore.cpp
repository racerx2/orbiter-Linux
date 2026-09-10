// ===================================================
// Copyright (C) 2021-2026 Jarmo Nikkanen
// licensed under LGPL v2
// ===================================================
//
// CONVERTED FROM OVP/D3D9Client/gcCore.cpp, read end to end (799 lines).
//
// THE ADD-ON-FACING IMPLEMENTATION. gcCore.h -- which needed no changes at
// all -- declares the interface an external module writes against, and this
// is the client side of it. Most of the file forwards one call and converts
// nothing. Six things do change, and one whole feature could not be carried
// over at all.
//
//  1. THE CUSTOM SWAP CHAIN HAS NO COUNTERPART, and RegisterSwap now takes
//     its own already-defined failure path. See the long note above it: it is
//     the only capability in this conversion that is genuinely lost rather
//     than respelled, and the API already had a way to say so.
//
//  2. StretchRect BECAME VulkanDevice::BlitTexture's RECTANGLE FORM, which
//     was added for this one call site. vkCmdBlitImage takes two rectangles
//     and always has; see VulkanFrame.h. NOTE THE ARGUMENT ORDER -- the
//     converted call names the destination first where StretchRect named the
//     source first.
//
//  3. ColorFill BECAME VulkanDevice::ClearImage, which already existed and
//     already took the rectangle.
//
//  4. LockRect/UnlockRect BECAME VulkanTexture::Map/Unmap, and the two
//     branches on D3DRESOURCETYPE collapse into one -- the same collapse
//     TileBuffer::ReadDDSSurface made, and for the same reason. FIXING A BUG
//     ON THE WAY: see finding 41 at LockSurface.
//
//  5. D3DCAPS9::MaxTextureRepeat HAS NO VULKAN COUNTERPART. GetSystemSpecs
//     reports the value that means "no limit", exactly as gcConst.cpp's copy
//     of the same function does -- the two must agree.
//
//  6. The D3DX casts in RenderLines DISAPPEAR rather than being renamed.
//     VulkanEffect::RenderLines takes const FVECTOR3* and const FMATRIX4*,
//     which is what the caller already had; the (const D3DXVECTOR3*) and
//     (const D3DXMATRIX*) casts existed only to reinterpret one three-float
//     type as another.
//
// NOT CONVERTED, because none of it needed converting: the camera interface,
// the matrix interface, the tile-data interface and every gcCore2 function
// below GetPlanetManager. They are calls into Scene, vVessel, vPlanet and
// TileManager2<SurfTile>, all of which are already converted, and their
// signatures did not change.
// ===================================================


#include <vulkan/vulkan.h>
#include "VulkanTypes.h"
#include "gcCore.h"
#include "VulkanSurface.h"
#include "VulkanClient.h"
// NOT IN THE WINDOWS INCLUDE LIST. CreatePoly, DeletePoly, GetTextLength and
// SketchpadVersion all name Sketchpad types, which reached this file through
// D3D9Surface.h -> D3D9Pad.h on Windows. The converted VulkanSurface.h
// forward-declares VulkanPad instead of including VulkanPad.h, because
// D3D9Pad.h includes D3D9Surface.h back and only MSVC's include guards make
// that cycle survive. Same finding as VulkanControlPanel.cpp's.
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
// and the child is a rectangle the UI host draws. See the note above
// RegisterSwap, and g_controlImages in UIHost.cpp.
extern "C" void orbiter_SetControlImage(HWND h, void *imageView, int w, int hgt);


// ===============================================================================================
// gcSwap -- the custom swap chain's data block.
//
// Was { LPDIRECT3DSWAPCHAIN9 pSwap; LPDIRECT3DSURFACE9 pBack; SURFHANDLE hSurf; }.
// Both D3D9 handles are gone with the swap chain itself (see RegisterSwap
// below), so what is left is the SURFHANDLE the three surviving accessors
// hand back. The class is kept rather than deleted because FlipSwap,
// GetRenderTarget and ReleaseSwap all cast an HSWAP to it, and because a
// future core that publishes a per-window VkSurfaceKHR would refill it.
// ===============================================================================================

class gcSwap
{
public:
	gcSwap() : hSurf(NULL), hWnd(NULL) { }
	~gcSwap() { Release(); }
	void Release() {
		// The control stops showing this image BEFORE the surface -- and with
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
// THE SWAP CHAIN ITSELF DOES NOT CROSS; WHAT IT IS FOR DOES.
//
// RegisterSwap asks D3D9 for an ADDITIONAL SWAP CHAIN on an arbitrary HWND --
// CreateAdditionalSwapChain(&pp, &pSwap) -- so that an add-on can present its
// own rendering into its own child window. A literal translation needs three
// things the client has none of:
//
//   * A VkSurfaceKHR FOR THAT WINDOW. It comes from a platform call --
//     vkCreateXlibSurfaceKHR, vkCreateWaylandSurfaceKHR,
//     glfwCreateWindowSurface -- each needing the real window-system handle.
//     THERE ISN'T ONE. An HWND here is Src/Orbiter/Linux's own handle, and a
//     child control is not an OS window at all: it is a rectangle
//     UIHost::drawControl paints into the single GLFW window's ImGui draw
//     list. There is no X11 Window or wl_surface underneath it to create a
//     surface on -- not "hard to find", but non-existent.
//
//   * A VkSwapchainKHR on it, with its images, views and render pass.
//
//   * A PRESENT. UIHost.cpp owns the only vkQueuePresentKHR in the process
//     and the acquire/submit/present semaphores with it; a client presenting
//     on its own would race the frame pump for the queue.
//
// SO THE MECHANISM CHANGES AND THE CONTRACT DOES NOT. Read what the caller
// actually does with the handle -- DX9ExtMFD's MFDWindow::clbkRefreshDisplay
// is the consumer in this tree, and it is the whole use:
//
//     if (!hSwap) hSwap = pCore->RegisterSwap(hDsp, hSwap, 0);
//     SURFHANDLE tgt = pCore->GetRenderTarget(hSwap);
//     if (surf) pCore->StretchRectInScene(tgt, surf);
//     else      pCore->ClearSurfaceInScene(tgt, 0);
//     pCore->FlipSwap(hSwap);
//
// It never touches a swap chain. It asks for A RENDER TARGET THE SIZE OF THAT
// CHILD WINDOW, draws into it, and says "show it there". Both halves have an
// exact counterpart here: the render target is an ordinary sampled surface,
// and "show it there" is UIHost drawing that image over the control's
// rectangle -- which is precisely what presenting to that child window did.
//
// THE ADD-ON IS UNTOUCHED. MFDWindow.cpp and ExtMFD.cpp remain byte-identical
// to OVP/D3D9Client/samples/DX9ExtMFD (diff clean), which is the point: the
// difference lives where the platform difference is.
//
// Line-by-line against the Windows body (gcCore.cpp:104-150):
//
//   pp.BackBufferWidth/Height = 0   in windowed mode D3D9 reads this as "the
//                                   window's client area". Asked for
//                                   explicitly here -- GetClientRect(hWnd).
//   pp.BackBufferFormat  X8R8G8B8   OAPISURFACE_PF_XRGB. Vulkan has no X8
//                                   format; NatCreateSurface answers
//                                   B8G8R8A8_UNORM plus SetAlphaOne, which is
//                                   what X8 means. See its note.
//   pp.BackBufferCount = 1          one image is what a SURFHANDLE is.
//   MultiSample NONE, quality 0     no OAPISURFACE_ANTIALIAS. Note the AA
//                                   parameter was ignored by the reference
//                                   too, and stays ignored.
//   SwapEffect FLIP, Windowed       properties of a swap chain; nothing to
//   PresentationInterval IMMEDIATE  carry -- the core's own present paces the
//   FullScreen_RefreshRate DEFAULT  frame, once per frame, as it always did.
//   EnableAutoDepthStencil = false  no depth attached to the back buffer...
//   OAPISURFACE_RENDER3D            ...but the reference's SurfNative flags
//                                   ask for one anyway (D3D9 attaches it to
//                                   the SURFACE, not the chain), so it is
//                                   kept: a consumer that renders 3D into its
//                                   swap target needs it.
//   OAPISURFACE_BACKBUFFER          not passed on. It marks a surface as THE
//                                   back buffer for the client's own
//                                   render-target bookkeeping, and this one
//                                   is not; it is a texture that gets drawn.
//   + OAPISURFACE_TEXTURE           THE ONE ADDITION. The reference's back
//                                   buffer was consumed by Present; this one
//                                   is consumed by a sampler, so it needs
//                                   SAMPLED usage. It also makes the settled
//                                   layout SHADER_READ_ONLY_OPTIMAL, which is
//                                   exactly where BlitTexture leaves it -- so
//                                   StretchRectInScene needs no change and no
//                                   extra barrier.
//   if (pData) pData->Release()     kept verbatim: MFDWindow::Resize calls
//   else pData = new gcSwap()       this again with the old handle on every
//                                   resize, and reuses the object.
//   failure -> LogErr + NULL        kept: every caller already handles NULL,
//                                   because on Windows this fails in
//                                   true-fullscreen mode.
// ===============================================================================================
//
HSWAP gcCore::RegisterSwap(HWND hWnd, HSWAP hData, int AA) 
{ 
	gcSwap *pData = (gcSwap*)hData;

	if (!hWnd) {
		LogErr("gcCore::RegisterSwap: no window");
		return NULL;
	}

	// pp.BackBufferWidth/Height = 0. D3D9 read the size off the window; here
	// it has to be read off the window explicitly.
	RECT rc = { 0, 0, 0, 0 };
	GetClientRect(hWnd, &rc);
	const DWORD w = (DWORD)(rc.right - rc.left);
	const DWORD h = (DWORD)(rc.bottom - rc.top);

	if (w == 0 || h == 0) {
		// Windows had no equivalent failure -- a zero-sized window still made
		// a swap chain -- but clbkCreateSurfaceEx returns NULL for a zero
		// surface, so it is refused here rather than left to assert. Resize()
		// calls this again the moment the control has a size.
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
	// have put something on screen. Release() above has already cleared any
	// image from the swap this one replaces.
	return HSWAP(pData);
}


// ===============================================================================================
//
// Was HR(pSwap->Present(0, 0, 0, 0, 0)).
//
// The present becomes the publication: from this call on, UIHost draws this
// image over the control's rectangle, once per frame, until the next
// RegisterSwap or ReleaseSwap. The reference presented one image per call and
// the driver held it on screen until the next; this holds it the same way, and
// the core's own frame pump does the actual presenting exactly as before.
//
// Idempotent, and deliberately re-published every call rather than only on the
// first: the surface can be re-created underneath a live gcSwap (Resize), and
// a caller that flips without having drawn is simply showing the same image
// again -- which is what Present did.
void gcCore::FlipSwap(HSWAP hSwap) 
{ 
	// Guarded rather than left to dereference a NULL the Windows version
	// never had to consider -- there, a failed RegisterSwap meant the caller
	// never got this far.
	if (!hSwap) return;

	gcSwap *pData = (gcSwap*)hSwap;
	if (!pData->hSurf || !pData->hWnd) return;

	// The SAMPLING view, which SetAlphaOne has swizzled alpha=1 on because the
	// surface was asked for as PF_XRGB. That is what makes the picture opaque
	// -- a D3D9 back buffer has no alpha channel and nothing behind the child
	// window is ever visible through it. Measured: the MFD's background reads
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
	// A DEVMESHHANDLE is an opaque handle to the client's own mesh type, so
	// the cast names whatever that type now is. Same line as gcConst.cpp's.
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
	// The (const LPD3DXMATRIX) cast is gone rather than renamed:
	// Scene::PickMesh takes a const FMATRIX4*, which is what the caller
	// already holds. The cast only ever reinterpreted one four-by-four of
	// floats as another.
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
	// Both D3DX casts disappear. VulkanEffect::RenderLines is declared
	// (const FVECTOR3*, const WORD*, int, int, const FMATRIX4*, DWORD), which
	// is exactly what this function was handed.
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
		// StretchRect(src, srcRect, dst, dstRect, filter) -- SOURCE FIRST.
		// BlitTexture names the DESTINATION first, matching its own
		// whole-image form and memcpy, so the two pairs swap here. See
		// VulkanFrame.h. D3DTEXF_LINEAR is the default filter there.
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
		// ColorFill(surface, rect, D3DCOLOR) -> ClearImage(image, rect, DWORD),
		// which already existed and already took the rectangle. The
		// (D3DCOLOR) cast goes with the type: a D3DCOLOR was a DWORD
		// 0xAARRGGBB and the client's DWORD colour is those same bytes in
		// that same order.
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
	// constructor, so a raw memset over it is -Wclass-memaccess. The cast
	// says the clear is deliberate. Same line as gcConst.h's.
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
		// D3DCAPS9::MaxTextureRepeat HAS NO VULKAN COUNTERPART -- it was the
		// largest texture coordinate the fixed-function sampler could still
		// wrap correctly, and Vulkan names no such ceiling. Add-ons read this,
		// so it cannot simply be dropped; it reports the value that means "no
		// limit". gcConst.cpp's copy of this function says the same number,
		// and the two have to agree.
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
	// Already disabled on Windows -- the body is commented out there too and
	// the function returns NULL. The commented lines follow the type renames
	// so that whoever re-enables them is not reading D3D9 in a Linux file.
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
// FINDING 41: THE WINDOWS VERSION OF THIS FUNCTION, AND OF ReleaseLock BELOW,
// LOCKS THE HANDLE ITSELF RATHER THAN THE RESOURCE BEHIND IT.
//
// Both ask SURFACE(hSrf)->GetResource() for the resource TYPE and then call
// LockRect on `hSrf` cast to the matching D3D9 interface:
//
//     LPDIRECT3DRESOURCE9 pResource = SURFACE(hSrf)->GetResource();
//     ...
//     LPDIRECT3DSURFACE9 pSurf = static_cast<LPDIRECT3DSURFACE9>(hSrf);
//     if (HROK(pSurf->LockRect(&lock, NULL, flags))) ...
//
// A SURFHANDLE is `void*`, and every SURFHANDLE in this client is a
// SurfNative* -- VulkanSurface.h says so in as many words. SurfNative is a
// plain C++ class with no virtual functions and therefore no vtable, and its
// first member is `char name[128]`. So that call reads a vtable pointer out of
// the first eight characters of the surface's NAME and jumps through it. It is
// not a wrong value; it is a wild indirect call. The correct expression is the
// one the function has already computed two lines above.
//
// The conversion cannot reproduce the mistake even by accident, because there
// is no second pointer to confuse: Map() is called on the VulkanTexture that
// GetResource() returns.
//
// D3DLOCK_DONOTWAIT has no counterpart and needs none. It asked the D3D9
// runtime not to block while the GPU still held the resource; vkMapMemory does
// no such waiting -- host-visible memory is mappable at any time and the
// caller owns the synchronisation -- so `bWait` has nothing left to select. It
// stays in the signature because gcCore.h is the public interface.
//
// And the two branches on D3DRESOURCETYPE collapse into one, as they do
// everywhere else in this conversion: a VkImage is both a surface and a
// texture. Same collapse as TileBuffer::ReadDDSSurface's.
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
