// ===========================================================================
// gcCore: the add-on's import library.
//
// gcCoreAPI.h declares the graphics-client core API with
//     #define gc_interface static
// so every entry point is a static member function, and any module that
// mentions one needs its definition at link time. The real definitions live
// in the client (OVP/VulkanClient/gcCore.cpp), whose bodies reach straight
// into it -- g_client, Scene, CAMERA(hCam).
//
// These cannot simply return nothing. Interpreter.cpp guards every call with
// `if (pCore)`, which makes LuaInterpreter safe, but that guard protects no
// add-on: `gcGetCoreInterface()` does not return null when a client is there:
//
//     if (pBindCoreMethod) return (pCoreInterface = new gcCore2(pBindCoreMethod));
//
// and gcCore2's constructor body is `#define fnc_binder` -- empty. The object
// it hands back is a token. Nothing is bound to it, and every `pCore->Foo()`
// on it is an ordinary call to the static member `gcCore::Foo`, resolved by
// the linker, not through the interface at all.
//
// On Windows that resolves into the client: the add-on links D3D9Client.lib
// and its import table points at D3D9Client.dll's copy. A DLL has its own
// symbol namespace and an import library is how you reach into one. Linux has
// no such thing. Measured on DX9ExtMFD:
//
//   DX9ExtMFD.so    U _ZN6gcCore12RegisterSwapEP6HWND__Pvi   (imported)
//   Orbiter          W _ZN6gcCore12RegisterSwapEP6HWND__Pvi   (this file)
//   VulkanClient.so  T _ZN6gcCore12RegisterSwapEP6HWND__Pvi   (the real one)
//
// The executable is searched before any dlopen'd object, so the add-on bound
// to the weak stub here and got `return nullptr` -- silently, with no log
// line, because a stub has nothing to say. DX9ExtMFD's MFD display was blank
// for exactly this reason: RegisterSwap answered null, clbkRefreshDisplay set
// bFailed and never drew again. Weak-vs-strong does not help: ELF resolves by
// search order, and a weak definition found first still wins.
//
// So each body forwards, which is what an import library does: it asks the
// loaded client for its own implementation by name through the export the
// header was already designed around -- `gcBindCoreMethod`, the same one
// gcGetCoreInterface() looks up -- and calls it. The client's binder maps
// every name in this file (gcCore.cpp, binder_start..binder_end).
//
// With no client loaded each returns the "nothing happened" value for its
// type, which is what LuaInterpreter needs. Still weak, so a client linked
// directly rather than dlopen'd keeps precedence and nothing here can clash.
// ===========================================================================

#include "gcCoreAPI.h"

#define GC_STUB __attribute__((weak))

namespace {

// The client's binder, resolved lazily and retried while absent rather than
// cached as a failure: a module's first gcCore call can come before the
// graphics client is loaded, and a one-shot "no client" answer would make that
// module gcCore-less for the whole session. GetModuleHandle is a lookup in the
// loader index orb_LoadLibrary maintains, so retrying costs a string compare.
__gcBindCoreMethod gcBinder()
{
	static __gcBindCoreMethod pBind = nullptr;
	if (pBind) return pBind;

	HMODULE hModule = GetModuleHandle("VulkanClient");
	if (!hModule) return nullptr;

	pBind = (__gcBindCoreMethod)GetProcAddress(hModule, "gcBindCoreMethod");
	return pBind;
}

void *gcBind(const char *name)
{
	__gcBindCoreMethod pBind = gcBinder();
	if (!pBind) return nullptr;
	void *pFnc = nullptr;
	pBind(&pFnc, name);
	return pFnc;
}

} // namespace

// The lookup is done once per call site and remembered, including a negative
// answer -- the client logs "failed to bind" for a name it does not map, and
// re-asking every frame would fill the log. `bound` is only set once a client
// is actually there, so the retry above still works.
//
// decltype(&CLS::NAME) is a plain function-pointer type here: gc_interface is
// `static`, so these are not pointer-to-member.
#define GC_FWD(CLS, NAME, ARGS)                                             \
	{                                                                       \
		using FN = decltype(&CLS::NAME);                                    \
		static FN   fn    = nullptr;                                        \
		static bool bound = false;                                          \
		if (!bound && gcBinder()) { fn = (FN)gcBind(#NAME); bound = true; } \
		if (fn) return fn ARGS;                                             \
		return {};                                                          \
	}

#define GC_FWD_VOID(CLS, NAME, ARGS)                                        \
	{                                                                       \
		using FN = decltype(&CLS::NAME);                                    \
		static FN   fn    = nullptr;                                        \
		static bool bound = false;                                          \
		if (!bound && gcBinder()) { fn = (FN)gcBind(#NAME); bound = true; } \
		if (fn) fn ARGS;                                                    \
	}

GC_STUB HSWAP gcCore::RegisterSwap(HWND hWnd, HSWAP hSwap, int AA)
GC_FWD(gcCore, RegisterSwap, (hWnd, hSwap, AA))

GC_STUB void gcCore::FlipSwap(HSWAP hSwap)
GC_FWD_VOID(gcCore, FlipSwap, (hSwap))

GC_STUB SURFHANDLE gcCore::GetRenderTarget(HSWAP hSwap)
GC_FWD(gcCore, GetRenderTarget, (hSwap))

GC_STUB void gcCore::ReleaseSwap(HSWAP hSwap)
GC_FWD_VOID(gcCore, ReleaseSwap, (hSwap))

GC_STUB int gcCore::DeleteCustomCamera(CAMERAHANDLE hCam)
GC_FWD(gcCore, DeleteCustomCamera, (hCam))

GC_STUB void gcCore::CustomCameraOnOff(CAMERAHANDLE hCam, bool bOn)
GC_FWD_VOID(gcCore, CustomCameraOnOff, (hCam, bOn))

GC_STUB void gcCore::CustomCameraOverlay(CAMERAHANDLE hCam, __gcRenderProc clbk, void* pUser)
GC_FWD_VOID(gcCore, CustomCameraOverlay, (hCam, clbk, pUser))

GC_STUB void gcCore::SetCustomCameraSurfaceLabelScale(CAMERAHANDLE hCam, float scale)
GC_FWD_VOID(gcCore, SetCustomCameraSurfaceLabelScale, (hCam, scale))

GC_STUB CAMERAHANDLE gcCore::SetupCustomCamera(CAMERAHANDLE hCam, OBJHANDLE hVessel, VECTOR3& vPos, VECTOR3& vDir, VECTOR3& vUp, double dFov, SURFHANDLE hSurf, DWORD dwFlags)
GC_FWD(gcCore, SetupCustomCamera, (hCam, hVessel, vPos, vDir, vUp, dFov, hSurf, dwFlags))

GC_STUB int gcCore::SketchpadVersion(Sketchpad *pSkp)
GC_FWD(gcCore, SketchpadVersion, (pSkp))

GC_STUB HPOLY gcCore::CreatePoly(HPOLY hPoly, const oapi::FVECTOR2* pt, int npt, DWORD flags)
GC_FWD(gcCore, CreatePoly, (hPoly, pt, npt, flags))

GC_STUB HPOLY gcCore::CreateTriangles(HPOLY hPoly, const clrVtx* pt, int npt, DWORD flags)
GC_FWD(gcCore, CreateTriangles, (hPoly, pt, npt, flags))

GC_STUB void gcCore::DeletePoly(HPOLY hPoly)
GC_FWD_VOID(gcCore, DeletePoly, (hPoly))

GC_STUB DWORD gcCore::GetTextLength(oapi::Font* hFont, const char* pText, int len)
GC_FWD(gcCore, GetTextLength, (hFont, pText, len))

GC_STUB DWORD gcCore::GetCharIndexByPosition(oapi::Font* hFont, const char* pText, int pos, int len)
GC_FWD(gcCore, GetCharIndexByPosition, (hFont, pText, pos, len))

GC_STUB bool gcCore::RegisterRenderProc(__gcRenderProc proc, DWORD id, void* pParam)
GC_FWD(gcCore, RegisterRenderProc, (proc, id, pParam))

GC_STUB int gcCore::GetMeshMaterial(DEVMESHHANDLE hMesh, DWORD idx, MatProp prop, FVECTOR4 *value)
GC_FWD(gcCore, GetMeshMaterial, (hMesh, idx, prop, value))

GC_STUB int gcCore::SetMeshMaterial(DEVMESHHANDLE hMesh, DWORD idx, MatProp prop, const FVECTOR4* value)
GC_FWD(gcCore, SetMeshMaterial, (hMesh, idx, prop, value))

GC_STUB int gcCore::GetMatrix(MatrixId mat, OBJHANDLE hVessel, DWORD mesh, DWORD group, oapi::FMATRIX4* pMat)
GC_FWD(gcCore, GetMatrix, (mat, hVessel, mesh, group, pMat))

GC_STUB int gcCore::SetMatrix(MatrixId mat, OBJHANDLE hVessel, DWORD mesh, DWORD group, const oapi::FMATRIX4* pMat)
GC_FWD(gcCore, SetMatrix, (mat, hVessel, mesh, group, pMat))

GC_STUB DEVMESHHANDLE gcCore::GetDevMesh(MESHHANDLE hMesh)
GC_FWD(gcCore, GetDevMesh, (hMesh))

GC_STUB DEVMESHHANDLE gcCore::LoadDevMeshGlobal(const char* file_name, bool bUseCache)
GC_FWD(gcCore, LoadDevMeshGlobal, (file_name, bUseCache))

GC_STUB void gcCore::ReleaseDevMesh(DEVMESHHANDLE hMesh)
GC_FWD_VOID(gcCore, ReleaseDevMesh, (hMesh))

GC_STUB void gcCore::RenderMesh(DEVMESHHANDLE hMesh, const FMATRIX4* pWorld)
GC_FWD_VOID(gcCore, RenderMesh, (hMesh, pWorld))

GC_STUB bool gcCore::PickMesh(PickMeshStruct* pm, DEVMESHHANDLE hMesh, const FMATRIX4* pWorld, short x, short y)
GC_FWD(gcCore, PickMesh, (pm, hMesh, pWorld, x, y))

GC_STUB void gcCore::RenderLines(const FVECTOR3* pVtx, const WORD* pIdx, int nVtx, int nIdx, const FMATRIX4* pWorld, DWORD color)
GC_FWD_VOID(gcCore, RenderLines, (pVtx, pIdx, nVtx, nIdx, pWorld, color))

GC_STUB void gcCore::GetSystemSpecs(SystemSpecs* sp, int size)
GC_FWD_VOID(gcCore, GetSystemSpecs, (sp, size))

GC_STUB bool gcCore::GetSurfaceSpecs(SURFHANDLE hSrf, SurfaceSpecs *sp, int size)
GC_FWD(gcCore, GetSurfaceSpecs, (hSrf, sp, size))

GC_STUB SURFHANDLE gcCore::LoadSurface(const char *fname, DWORD flags)
GC_FWD(gcCore, LoadSurface, (fname, flags))

GC_STUB bool gcCore::SaveSurface(const char* file, SURFHANDLE hSrf)
GC_FWD(gcCore, SaveSurface, (file, hSrf))

GC_STUB SURFHANDLE gcCore::GetMipSublevel(SURFHANDLE hSrf, int level)
GC_FWD(gcCore, GetMipSublevel, (hSrf, level))

GC_STUB bool gcCore::GenerateMipmaps(SURFHANDLE hSurface)
GC_FWD(gcCore, GenerateMipmaps, (hSurface))

GC_STUB SURFHANDLE gcCore::CompressSurface(SURFHANDLE hSurface, DWORD flags)
GC_FWD(gcCore, CompressSurface, (hSurface, flags))

GC_STUB HBITMAP gcCore::LoadBitmapFromFile(const char *fname)
GC_FWD(gcCore, LoadBitmapFromFile, (fname))

GC_STUB HWND gcCore::GetRenderWindow()
GC_FWD(gcCore, GetRenderWindow, ())

GC_STUB bool gcCore::RegisterGenericProc(__gcGenericProc proc, DWORD id, void *pParam)
GC_FWD(gcCore, RegisterGenericProc, (proc, id, pParam))

GC_STUB bool gcCore::StretchRectInScene(SURFHANDLE tgt, SURFHANDLE src, LPRECT tr, LPRECT sr)
GC_FWD(gcCore, StretchRectInScene, (tgt, src, tr, sr))

GC_STUB bool gcCore::ClearSurfaceInScene(SURFHANDLE tgt, DWORD color, LPRECT tr)
GC_FWD(gcCore, ClearSurfaceInScene, (tgt, color, tr))

GC_STUB gcCore::PickGround gcCore::ScanScreen(int scr_x, int scr_y)
GC_FWD(gcCore, ScanScreen, (scr_x, scr_y))

GC_STUB bool gcCore::LockSurface(SURFHANDLE hSrf, Lock* pOut, bool bWait)
GC_FWD(gcCore, LockSurface, (hSrf, pOut, bWait))

GC_STUB void gcCore::ReleaseLock(SURFHANDLE hSrf)
GC_FWD_VOID(gcCore, ReleaseLock, (hSrf))

GC_STUB HPLANETMGR gcCore2::GetPlanetManager(OBJHANDLE hPlanet)
GC_FWD(gcCore2, GetPlanetManager, (hPlanet))

GC_STUB SURFHANDLE gcCore2::SetTileOverlay(HTILE hTile, const SURFHANDLE hOverlay)
GC_FWD(gcCore2, SetTileOverlay, (hTile, hOverlay))

GC_STUB HOVERLAY gcCore2::AddGlobalOverlay(HPLANETMGR hMgr, VECTOR4 mmll, OlayType type, const SURFHANDLE hOverlay, HOVERLAY hOld, const FVECTOR4 *pBlend)
GC_FWD(gcCore2, AddGlobalOverlay, (hMgr, mmll, type, hOverlay, hOld, pBlend))

GC_STUB gcCore2::PickGround gcCore2::GetTileData(HPLANETMGR hMgr, double lng, double lat, int maxlevel)
GC_FWD(gcCore2, GetTileData, (hMgr, lng, lat, maxlevel))

GC_STUB HTILE gcCore2::GetTile(HPLANETMGR hMgr, double lng, double lat, int maxlevel)
GC_FWD(gcCore2, GetTile, (hMgr, lng, lat, maxlevel))

GC_STUB bool gcCore2::HasTileData(HPLANETMGR hMgr, int iLng, int iLat, int level, int flags)
GC_FWD(gcCore2, HasTileData, (hMgr, iLng, iLat, level, flags))

GC_STUB SURFHANDLE gcCore2::SeekTileTexture(HPLANETMGR hMgr, int iLng, int iLat, int level, int flags, void *reserved)
GC_FWD(gcCore2, SeekTileTexture, (hMgr, iLng, iLat, level, flags, reserved))

GC_STUB bool gcCore2::SeekTileElevation(HPLANETMGR hMgr, int iLng, int iLat, int level, int flags, ElevInfo *pEI)
GC_FWD(gcCore2, SeekTileElevation, (hMgr, iLng, iLat, level, flags, pEI))

GC_STUB int gcCore2::GetElevation(HTILE hTile, double lng, double lat, double *out_elev)
GC_FWD(gcCore2, GetElevation, (hTile, lng, lat, out_elev))

GC_STUB gcIPInterface* gcCore2::CreateIPInterface(const char* file, const char* PSEntry, const char* VSEntry, const char* ppf)
GC_FWD(gcCore2, CreateIPInterface, (file, PSEntry, VSEntry, ppf))

GC_STUB void gcCore2::ReleaseIPInterface(gcIPInterface* pIPI)
GC_FWD_VOID(gcCore2, ReleaseIPInterface, (pIPI))
