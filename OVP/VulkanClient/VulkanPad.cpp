// ===================================================
// Copyright (C) 2012-2026 Jarmo Nikkanen
// licensed under LGPL v2
// ===================================================
//
// CONVERTED FROM OVP/D3D9Client/D3D9Pad.cpp, read end to end (2098 lines).
//
// THE DRAWING IS ARITHMETIC AND CONVERTS UNCHANGED. Every geometry-producing
// function here -- FillRect, Rectangle, Ellipse, Polygon, Polyline,
// AppendLineVertexList in both its forms, CheckTriangle and
// CreatePolyIndexList -- writes into the static Vtx/Idx arrays and touches no
// device at all. Those are carried over line for line, including the
// ear-clipping triangulator and the wide-line expansion, which are the two
// pieces of real algorithm in the file.
//
// THREE PLACES DO CHANGE, and they are the three places the file talks to
// Direct3D:
//
// 1. Flush(), which is the only draw. It reads a dozen render states on
//    Windows, sets them, draws, and puts them back. Vulkan bakes all of them
//    into the pipeline, so they become a VulkanEffectFile::PassOverride
//    filled in from the same variables -- dwBlendState's four modes become a
//    colour write mask and a blend-enable, its two filter modes select a
//    sampler, bDepthEnable becomes the depth test and write, and the scissor
//    rectangle stays dynamic. Nothing is read back and nothing is restored,
//    because there is no device state to restore: the next bind replaces the
//    pipeline outright. THAT is why the GetRenderState/SetRenderState pairs
//    around the draw simply disappear rather than being emulated.
//
// 2. SetupDevice(), which applies whatever the Change flags say has moved.
//    Every line of it was already an FX->Set* call; those convert one for one
//    because VulkanEffectFile keeps the same call shape. The two that were
//    NOT FX calls -- SetScissorRect and D3DRS_SCISSORTESTENABLE -- move to
//    the PassOverride, because a scissor rectangle is dynamic state set on
//    the command buffer at bind time rather than device state set whenever.
//
// 3. UTF8ToCP1252, which was two Win32 NLS calls. See the note on it below;
//    it is the one function whose behaviour is deliberately not identical,
//    and the reason is a bug in the original.
//
// RenderState is gone -- see VulkanPad.h. The constructor's `new
// RenderState(pDev)`, the Capture() in BeginDrawing and the Restore() in
// EndDrawing go with it, and none of the three had any effect on Windows
// either: D3D9Frame.cpp creates the device with D3DCREATE_PUREDEVICE
// unconditionally, and a pure device fails GetRenderState.
// ===================================================

#include "VulkanPad.h"
#include "VulkanClient.h"
#include "VulkanSurface.h"
#include "VulkanUtil.h"
#include "VulkanTextMgr.h"
#include "VulkanConfig.h"
#include "Log.h"
#include "Mesh.h"

#include <vector>
#include <string>

using namespace oapi;


// ===============================================================================================
// Font cache
// ===============================================================================================

struct FontCache {
	int           height;
	int           orient;
	bool          prop;
	char          face[64];
	FontStyle	  style;
	VulkanTextPtr pFont;
};

struct QFontCache {
	int           height;
	int           width;
	int			  weight;
	char          face[64];
	FontStyle	  style;
	float		  spacing;
	VulkanTextPtr pFont;
};

std::vector<QFontCache *> qcache;
std::vector<FontCache *> fcache;


oapi::Font * deffont = 0;
oapi::Pen * defpen = 0;


// ===============================================================================================
// UTF-8 to the font atlas's code page.
//
// The Windows version is two calls into the NLS API:
//
//     MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ...)   // decode
//     WideCharToMultiByte(28591, ...)                           // encode
//
// with the whole thing falling back to "return the string unchanged and hope
// it was already 8-bit" whenever either step fails -- which is deliberate and
// is kept: a legacy add-on that hands the Sketchpad Windows-1252 bytes rather
// than UTF-8 still renders.
//
// TWO THINGS ARE DIFFERENT HERE, AND THE SECOND IS A CORRECTION.
//
// The first is mechanical: there is no NLS API on Linux, so the UTF-8 decode
// is written out. It is thirty lines and needs no dependency; iconv would be
// one for a single table.
//
// The second: THE FUNCTION IS CALLED UTF8ToCP1252 AND CONVERTS TO 28591,
// WHICH IS ISO-8859-1, NOT CP1252. The two agree everywhere except
// 0x80-0x9F, where Latin-1 has unused control codes and CP1252 has the smart
// quotes, the en and em dashes, the ellipsis, the bullet and the euro sign --
// twenty-seven printable characters. And the font atlas IS indexed as CP1252:
// D3D9TextMgr::Init walks bytes 0..255 through TextOutA, which interprets
// them in the ANSI code page. So on Windows a right single quote arrives as
// U+2019, has no Latin-1 spelling, is substituted with '?' by
// WideCharToMultiByte -- and is drawn as a question mark, even though the
// atlas has the glyph for it sitting at index 0x92.
//
// This converts to CP1252, which is what the function's name says, what the
// atlas is indexed by, and what VulkanText::PrintSkp(LPCWSTR) already folds
// to. The visible difference is that typographic punctuation renders instead
// of turning into question marks.
// ===============================================================================================

static std::string UTF8ToCP1252(const char *utf8, int ulen)
{
	if (!utf8) return std::string();
	if (ulen < 0) ulen = (int)strlen(utf8);

	std::string out;
	out.reserve((size_t)ulen);

	const unsigned char *s = (const unsigned char *)utf8;
	int i = 0;

	while (i < ulen) {

		const unsigned char c = s[i];
		unsigned int cp = 0;
		int extra = 0;

		if (c < 0x80)			{ cp = c;			extra = 0; }
		else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F;	extra = 1; }
		else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F;	extra = 2; }
		else if ((c & 0xF8) == 0xF0) { cp = c & 0x07;	extra = 3; }
		else {
			// A continuation byte or an invalid lead. This is
			// MB_ERR_INVALID_CHARS failing, and the reference's answer is to
			// assume the caller handed us 8-bit text already.
			return std::string(utf8, (size_t)ulen);
		}

		if (i + extra >= ulen) return std::string(utf8, (size_t)ulen);

		for (int k = 1; k <= extra; k++) {
			const unsigned char cc = s[i + k];
			if ((cc & 0xC0) != 0x80) return std::string(utf8, (size_t)ulen);
			cp = (cp << 6) | (cc & 0x3F);
		}
		i += extra + 1;

		// --- Unicode -> CP1252 ---
		if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF)) {
			// ASCII, and the range where CP1252 and Latin-1 agree.
			out.push_back((char)cp);
			continue;
		}

		// The twenty-seven printable characters CP1252 puts in 0x80-0x9F.
		// The table is VulkanTextMgr.cpp's kCp1252High read the other way;
		// it is repeated rather than exported because it is thirty-two
		// entries and this is not a hot path.
		static const unsigned short kHigh[32] = {
			0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
			0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
			0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
			0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
		};

		char mapped = '?';
		for (int k = 0; k < 32; k++) {
			if (kHigh[k] == cp) { mapped = (char)(0x80 + k); break; }
		}
		out.push_back(mapped);
	}

	return out;
}


// ===============================================================================================
//
void VulkanPad::SinCos(int n, int k)
{
	pSinCos[k] = new FVECTOR2[n];
	float s = float(PI2) / float(n);
	float q = -s / 2.0f;
	for (int i = 0; i<n; i++) {
		pSinCos[k][i].x = sin(q) + 1.0f;
		pSinCos[k][i].y = cos(q) + 1.0f;
		q += s;
	}
}


// ===============================================================================================
//
void VulkanPad::VulkanTechInit(VulkanClient *_gc, VulkanDevice *pDevice)
{
	pDev = pDevice;
	gc = _gc;
	log = NULL;

	InitializeCriticalSectionAndSpinCount(&LogCrit, 256);


#ifdef SKPDBG
	if (fopen_s(&log, "Sketchpad.log", "w+")) { log = NULL; } // Failed
#endif // SKPDBG


	Idx = new WORD[3 * nQueueMax + 3];
	Vtx = new SkpVtx[3 * nQueueMax + 3];

	SinCos(8, 0);
	SinCos(16, 1);
	SinCos(32, 2);
	SinCos(64, 3);

	// Initialize Techniques -------------------------------------------------------------------------
	//
	char name[256];
	sprintf_s(name, 256, "Modules/VulkanClient/Sketchpad.glsl");

	// Was D3DXCreateEffectFromFileA with no macros at all -- Sketchpad.fx
	// takes no preprocessor definitions, unlike D3D9Client.fx. See
	// VulkanEffectFile::Load: the technique table sits beside the shader
	// under the same stem, as Sketchpad.tech.
	FX = new VulkanEffectFile(pDev);

	if (!FX->Load(name)) {
		// Was MessageBoxA + FatalAppExitA. FatalAppExitA terminates the
		// process without unwinding and the shim does not supply it; the
		// message box says the same thing and returning lets the caller's
		// failure path run. MissingRuntimeError() -- the second failure
		// branch, for a NULL effect -- named the DirectX runtime and has
		// nothing to name here.
		LogErr("Failed to create an Effect (%s)", name);
		oapiWriteLog((char*)"Vulkan: FAIL: Sketchpad.glsl did not compile. "
						   "See Orbiter.log for details");
		MessageBoxA(0, "Sketchpad.glsl failed to compile. See Orbiter.log for details.",
					"Sketchpad.glsl Error", 0);
		delete FX;
		FX = NULL;
		return;
	}

	// The Config->ShaderDebug block stood here and ran D3DXDisassembleEffect
	// into Sketchpad_asm.html. VulkanUtil.cpp's CompileShaderStage already
	// emits the SPIR-V disassembly when that flag is set -- per shader, at
	// the compiler, rather than per effect afterwards.

	pNoise	  = gc->GetNoiseTex();

	eDrawMesh = FX->GetTechniqueByName("SketchMesh");
	eSketch   = FX->GetTechniqueByName("SketchTech");
	eVP       = FX->GetParameterByName("gVP");
	eTex0     = FX->GetParameterByName("gTex0");
	eFnt0	  = FX->GetParameterByName("gFnt0");
	eDashEn   = FX->GetParameterByName("gDashEn");
	eW		  = FX->GetParameterByName("gW");
	eKey	  = FX->GetParameterByName("gKey");
	ePen      = FX->GetParameterByName("gPen");
	eWVP	  = FX->GetParameterByName("gWVP");
	eFov	  = FX->GetParameterByName("gFov");
	eRandom   = FX->GetParameterByName("gRandom");
	eTarget	  = FX->GetParameterByName("gTarget");
	eTexEn	  = FX->GetParameterByName("gTexEn");
	eFntEn    = FX->GetParameterByName("gFntEn");
	eKeyEn    = FX->GetParameterByName("gKeyEn");
	eWide	  = FX->GetParameterByName("gWide");
	eWidth	  = FX->GetParameterByName("gWidth");
	eSize	  = FX->GetParameterByName("gSize");
	eMtrl	  = FX->GetParameterByName("gMtrl");
	eShade    = FX->GetParameterByName("gShade");
	ePos	  = FX->GetParameterByName("gPos");
	ePos2	  = FX->GetParameterByName("gPos2");
	eCov	  = FX->GetParameterByName("gCov");
	eCovEn	  = FX->GetParameterByName("gClipEn");
	eClearEn  = FX->GetParameterByName("gClearEn");
	eEffectsEn= FX->GetParameterByName("gEffectsEn");
	eNoiseTex = FX->GetParameterByName("gNoiseTex");
	eGamma	  = FX->GetParameterByName("gGamma");
	eNoiseColor = FX->GetParameterByName("gNoiseColor");
	eColorMatrix = FX->GetParameterByName("gColorMatrix");
}


// ===============================================================================================
//
void VulkanPad::GlobalExit()
{
	LogAlw("Clearing Font Cache... %d Fonts are stored in the cache",fcache.size() + qcache.size());
	for (auto it = fcache.begin(); it != fcache.end(); ++it) {
		delete *it;
	}
	for (auto it = qcache.begin(); it != qcache.end(); ++it) {
		delete *it;
	}
	fcache.clear();
	qcache.clear();

	// Was SAFE_RELEASE(FX). The effect owns its pipelines, layouts, samplers
	// and arena and destroys them in its destructor; nothing is reference
	// counted.
	delete FX;
	FX = NULL;

	SAFE_DELETEA(Idx);
	SAFE_DELETEA(Vtx);
	for (int i=0;i<4;i++) SAFE_DELETEA(pSinCos[i]);

	if (log) fclose(log);
	log = NULL;

	DeleteCriticalSection(&LogCrit);
}

// ===============================================================================================
//
void VulkanPad::Log(const char *format, ...) const
{
	if (log == NULL) return;
	EnterCriticalSection(&LogCrit);
	char ErrBuf[1024];
	DWORD th = GetCurrentThreadId();
	va_list args;
	va_start(args, format);
	_vsnprintf_s(ErrBuf, 1024, 1024, format, args);
	va_end(args);
	fprintf_s(log, "<0x%X> [%s] %s\n", th, _PTR(this), ErrBuf);
	fflush(log);
	LeaveCriticalSection(&LogCrit);
}

// ===============================================================================================
//
void VulkanPad::Reset()
{
	bBeginDraw = false;
	bMustEndScene = false;
	vI = 0;
	iI = 0;
	VMAT_Identity(&mO);
	VMAT_Identity(&mVOrig);
	VMAT_Identity(&mPOrig);
	vTarget = FVECTOR4(1,1,1,1);
	pTgt = NULL;
	pDep = NULL;
	zfar = 1.0f;
	tgt = { 0,0,0,0 };
}



// ===============================================================================================
// Restore Default Settings: Fonts, Pens, Colors, etc...
//
void VulkanPad::LoadDefaults()
{
	assert(vI == 0);

	cfont = deffont;
	cpen = NULL;
	cbrush = NULL;
	hTexture = NULL;
	hFontTex = NULL;
	cx = 0;
	cy = 0;
	linescale = 1.0f;
	pattern = 1.0f;
	Enable = 0;
	RenderConfig = 0;

	tah = LEFT;
	tav = TOP;
	vmode = ORTHO;
	tCurrent = NONE;
	Change = SKPCHG_ALL;
	bkmode = TRANSPARENT;
	dwBlendState = Sketchpad::BlendState::ALPHABLEND;

	bColorComp = true;
	bLine = false;
	bEnableScissor = false;
	bDepthEnable = false;
	bColorKey = false;
	QPen.bEnabled = false;
	QBrush.bEnabled = false;

	// Was memset(ClipData, 0, sizeof(ClipData)). ClipData holds an FVECTOR3,
	// which has constructors, so memset over it is -Wclass-memaccess: it
	// writes past what the class controls and would clobber anything the type
	// grew. Value-initialising each element is the same zeroes with the
	// type's own rules -- the same fix VulkanUtil.cpp applies to D9BBox.
	for (int i = 0; i < 2; i++) {
		ClipData[i].uDir = FVECTOR3(0, 0, 0);
		ClipData[i].ca = 0.0f;
		ClipData[i].dst = 0.0f;
		ClipData[i].bEnable = false;
	}
	ScissorRect = { 0,0,0,0 };

	// Was D3DXCOLOR(DWORD(0)), which unpacks the DWORD 0 into four zero
	// floats. FVECTOR4 has no DWORD constructor; the value is the same.
	cColorKey  = FVECTOR4(0, 0, 0, 0);
	brushcolor = SkpColor(0xFF00FF00);
	bkcolor    = SkpColor(0xFF000000);
	textcolor  = SkpColor(0xFF00FF00);
	pencolor   = SkpColor(0xFF00FF00);

	VMAT_Identity(&mVP);
	VMAT_Identity(&mW);
	VMAT_Identity(&mP);
	VMAT_Identity(&mV);
	// Was D3DXMatrixIdentity((D3DXMATRIX*)&ColorMatrix) -- a cast because
	// ColorMatrix is an FMATRIX4 and D3DX only spoke D3DXMATRIX. Both are the
	// same sixteen floats, which is why the cast worked; with one matrix type
	// there is nothing to cast.
	VMAT_Identity(&ColorMatrix);

	Gamma = FVECTOR4(1, 1, 1, 1);
	Noise = FVECTOR4(0, 0, 0, 0);
}


// ===============================================================================================
// class VulkanPad
// ===============================================================================================
// Constructor will create VulkanPad interface but doesn't prepare it for drawing.
// BeginDrawing() must be called
//
VulkanPad::VulkanPad(SURFHANDLE s, const char *_name) : Sketchpad(s),
	_isSaveBuffer(false),
	_saveBuffer(NULL),
	_saveBufferSize(0)
{
#ifdef SKPDBG
	Log("#### Sketchpad Interface Created");
#endif
	// `pRState = new RenderState(pDev);` stood here. See the file header.
	if (_name) strcpy_s(name, 32, _name);
	else strcpy_s(name, 32, "NoName");
	Reset();
	LoadDefaults();
}


// ===============================================================================================
// class VulkanPad
// ===============================================================================================
// Constructor will create VulkanPad interface but doesn't prepare it for drawing.
// BeginDrawing() must be called
//
VulkanPad::VulkanPad(const char *_name) : Sketchpad(NULL),
	_isSaveBuffer(false),
	_saveBuffer(NULL),
	_saveBufferSize(0)
{
#ifdef SKPDBG
	Log("#### Sketchpad Interface Created (NoTgt)");
#endif
	if (_name) strcpy_s(name, 32, _name);
	else strcpy_s(name, 32, "NoName");
	Reset();
	LoadDefaults();
}



// ===============================================================================================
//
VulkanPad::~VulkanPad ()
{
#ifdef SKPDBG
	Log("#### Sketchpad Interface Deleted");
#endif
	assert(bBeginDraw == false);
	SAFE_DELETEA(_saveBuffer);
}


// ===============================================================================================
// Private
//
void VulkanPad::SetViewProj(const FMATRIX4* pV, const FMATRIX4* pP)
{
	mV = mVOrig = *pV;
	mP = mPOrig = *pP;
}


// ===============================================================================================
// Bind existing Sketchpad interface to TOP render targets and prepare for rendering
//
void VulkanPad::BeginDrawing()
{
	// Acquire render targets from Stack
	BeginDrawing(gc->GetTopRenderTarget(), gc->GetTopDepthStencil());
}


// ===============================================================================================
// Bind existing Sketchpad interface to render targets and prepare for rendering
//
void VulkanPad::BeginDrawing(VulkanTexture *pRenderTgt, VulkanTexture *pDepthStensil)
{
#ifdef SKPDBG
	Log("==== BeginDrawing %s, %s ====\n", _PTR(pRenderTgt), _PTR(pDepthStensil));
#endif

	assert(pRenderTgt != NULL);

	if (vI != 0) LogErr("Sketchpad %s has received drawing commands outside Begin() End() pair", _PTR(this));

	if (bBeginDraw == true) {
		LogErr("VulkanPad::BeginDrawing() called multiple times");
		HALT();
	}

	// `pRState->Capture();` stood here. See the file header.

	Reset();

	bBeginDraw = true;

	// If not already in scene, then start a new one
	if (gc->IsInScene() == false) {
		gc->BeginScene();
		bMustEndScene = true;
	}
	else bMustEndScene = false;

	// Was pRenderTgt->GetDesc(&tgt_desc) -- a D3D9 surface answered questions
	// about itself. A VkImage answers none, which is why VulkanTexture
	// records what it was asked for; Desc() hands back that record. See
	// VulkanTypes.h.
	tgt_desc = pRenderTgt->Desc();

	zfar = float(tgt_desc.Width > tgt_desc.Height ? tgt_desc.Width : tgt_desc.Height);

	// Was D3DXMatrixOrthoOffCenterLH(&mO, 0, W, H, 0, 0, zfar): a left-handed
	// off-centre orthographic projection with the top edge at y=0 and the
	// bottom at y=H, which is what puts the Sketchpad's origin at the top
	// left. Written out because D3DX is a Direct3D utility library with no
	// Vulkan counterpart -- see VMAT_Transformation2D in VulkanUtil.cpp for
	// the same reasoning.
	//
	// THE DEPTH RANGE IS THE ONE REAL DIFFERENCE. D3D9 clip space is
	// 0 <= z <= w and so is Vulkan's, so the LH orthographic matrix carries
	// over unchanged -- it is OpenGL, with -w <= z <= w, that would have
	// needed a different one. Nothing to adjust, but it is the first thing a
	// reader will want to check.
	{
		const float l = 0.0f, r = float(tgt_desc.Width);
		const float b = float(tgt_desc.Height), t = 0.0f;
		const float zn = 0.0f, zf = zfar;

		VMAT_Identity(&mO);
		mO.m11 = 2.0f / (r - l);
		mO.m22 = 2.0f / (t - b);
		mO.m33 = 1.0f / (zf - zn);
		mO.m41 = (l + r) / (l - r);
		mO.m42 = (t + b) / (b - t);
		mO.m43 = zn / (zn - zf);
		mO.m44 = 1.0f;
	}

	vTarget = FVECTOR4(2.0f / (float)tgt_desc.Width, 2.0f / (float)tgt_desc.Height,
					   (float)tgt_desc.Width, (float)tgt_desc.Height);

	pTgt = pRenderTgt;
	pDep = pDepthStensil;
	tgt = { 0, 0, (LONG)tgt_desc.Width, (LONG)tgt_desc.Height };
	Change = SKPCHG_ALL;
	nFlushedVtx = 0;		// diagnostic; see the declaration
}



// ===============================================================================================
//
void VulkanPad::EndDrawing()
{
#ifdef SKPDBG
	Log("==== EndDrawing ====\n");
#endif

	if (bBeginDraw == false) {
		LogErr("VulkanPad::EndDrawing() called without BeginDrawing()");
		HALT();
	}

	Flush();

	bBeginDraw = false;

	// `if (pRState) pRState->Restore();` stood here. See the file header.

	if (bMustEndScene) {
		gc->EndScene();
		bMustEndScene = false;
	}
}



// ===============================================================================================
// The only draw in the file.
//
// EVERYTHING BETWEEN THE GetRenderState AND THE MATCHING SetRenderState IS
// GONE, and it is worth being exact about why rather than leaving it as a
// diff. On Windows this function:
//
//     reads  D3DRS_ALPHABLENDENABLE
//     sets   D3DRS_CULLMODE, the vertex declaration, the technique, the pass
//     sets   D3DRS_COLORWRITEENABLE + D3DRS_ALPHABLENDENABLE from dwBlendState
//     sets   D3DSAMP_MIN/MAGFILTER (+ MAXANISOTROPY) from dwBlendState
//     sets   D3DRS_ZENABLE + D3DRS_ZWRITEENABLE from bDepthEnable
//     draws
//     puts   D3DRS_COLORWRITEENABLE, D3DRS_ALPHABLENDENABLE,
//            D3DRS_SCISSORTESTENABLE and the sampler filters back
//
// Every one of those is immutable pipeline state or sampler state in Vulkan,
// so they are not set -- they are DESCRIBED, in a PassOverride, before the
// pipeline is bound. And nothing is read back or restored, because a pipeline
// is not layered over: the next bind replaces it entirely. The two
// GetRenderState calls therefore have no counterpart at all, which is just as
// well -- see VulkanPad.h on RenderState: on a D3DCREATE_PUREDEVICE they
// could not have worked anyway.
// ===============================================================================================
bool VulkanPad::Flush(HPOLY hPoly)
{
	if (bBeginDraw == false) {
		LogErr("VulkanPad::Flush() called without BeginDrawing()");
		HALT();
	}

	UINT numPasses;

	if ((iI == 0) && (hPoly == NULL)) {
#ifdef SKPDBG
		Log("Flush (Nothing)", hPoly, iI);
#endif
		return false;
	}

	DWORD dwBlend = dwBlendState & 0xF;
	DWORD dwFilter = dwBlendState & 0xF0;

#ifdef SKPDBG
	char buf[128]; strcpy_s(buf, 128, "");
	char buf2[128]; strcpy_s(buf2, 128, "");

	if (dwBlend == SKPBS_ALPHABLEND) strcpy_s(buf, 128, "SKPBS_ALPHABLEND");
	if (dwBlend == SKPBS_COPY) strcpy_s(buf, 128, "SKPBS_COPY");
	if (dwBlend == SKPBS_COPY_ALPHA) strcpy_s(buf, 128, "SKPBS_COPY_ALPHA");
	if (dwBlend == SKPBS_COPY_COLOR) strcpy_s(buf, 128, "SKPBS_COPY_COLOR");

	if (bDepthEnable && pDep) strcpy_s(buf2, 128, "DEPTH_ENABLED");
	else strcpy_s(buf2, 128, "DEPTH_DISABLED");

	Log("Flush [%s] [%s] hPloy=%s, iI=%hu", buf, buf2, _PTR(hPoly), iI);
#endif

	if (!FX) { iI = vI = 0; return false; }

	// Which render targets the pad actually draws into, and how much. Reported
	// once per target size so a frame of HUD and MFD drawing does not flood
	// the log. Diagnostic only; env-gated.
	{
		static const bool bTraceFlush = (getenv("ORBITER_VK_TRACE_FLUSH") != NULL);
		if (bTraceFlush) {
			static std::map<DWORD, int> seen;
			const DWORD key = (tgt_desc.Width << 16) | tgt_desc.Height;
			if (seen.find(key) == seen.end()) {
				seen[key] = 1;
				LogErr("FLUSHTRACE target %ux%u  vI=%hu iI=%hu poly=%s blend=%u depth=%d vmode=%d",
					   tgt_desc.Width, tgt_desc.Height, vI, iI,
					   hPoly ? "yes" : "no", (unsigned)(dwBlendState & 0xF),
					   int(bDepthEnable && pDep), int(vmode));
				// The geometry itself, for the first few vertices. A quad that
				// produces no fragments is either degenerate, off-viewport or
				// transformed by the wrong pass, and none of those can be told
				// apart from the counts alone.
				for (WORD k = 0; k < vI && k < 4; k++)
					LogErr("FLUSHTRACE   v%u pos=(%.1f,%.1f) nxt=(%.1f,%.1f) clr=%08X fnc=%08X",
						   (unsigned)k, Vtx[k].x, Vtx[k].y, Vtx[k].nx, Vtx[k].ny,
						   (unsigned)Vtx[k].clr, (unsigned)Vtx[k].fnc);
			}
		}
	}

	VulkanEffectFile::PassOverride ovr;

	// D3DRS_CULLMODE = D3DCULL_NONE, unconditionally, for every Sketchpad
	// draw. The pad's geometry is 2D and has no consistent winding.
	ovr.cullMode = VulkanEffectFile::PassOverride::CULL_NONE;

	FX->SetVertexDecl(pSketchpadDecl);

	FX->SetFloat(eRandom, float(oapiRand()));
	FX->SetVector(eTarget, &vTarget);
	FX->SetTechnique(eSketch);

	// The four blend states. D3DRS_COLORWRITEENABLE's bits are
	// RED|GREEN|BLUE|ALPHA = 1|2|4|8 and VK_COLOR_COMPONENT_*_BIT is the same
	// order, so 0x7 and 0x8 and 0xF carry over as they stand.
	if (dwBlend == Sketchpad::BlendState::ALPHABLEND) {
		ovr.colorWriteMask = 0x7;
		ovr.blendEnable = 1;
	}
	else if (dwBlend == Sketchpad::BlendState::COPY) {
		ovr.colorWriteMask = 0xF;
		ovr.blendEnable = 0;
	}
	else if (dwBlend == Sketchpad::BlendState::COPY_ALPHA) {
		ovr.colorWriteMask = 0x8;
		ovr.blendEnable = 0;
	}
	else if (dwBlend == Sketchpad::BlendState::COPY_COLOR) {
		ovr.colorWriteMask = 0x7;
		ovr.blendEnable = 0;
	}

	// D3DSAMP_MIN/MAGFILTER. The Windows code leaves the filter at whatever
	// it last set until it explicitly restores LINEAR after the draw; here
	// each draw states its own, which is the same result without the restore.
	if (dwFilter == Sketchpad::BlendState::FILTER_POINT)			ovr.filter = 0;
	else if (dwFilter == Sketchpad::BlendState::FILTER_ANISOTROPIC)	ovr.filter = 2;
	else															ovr.filter = 1;

	// D3DRS_ZENABLE / D3DRS_ZWRITEENABLE
	if (bDepthEnable && pDep) { ovr.depthTest = 1; ovr.depthWrite = 1; }
	else					  { ovr.depthTest = 0; ovr.depthWrite = 0; }

	// D3DRS_SCISSORTESTENABLE + SetScissorRect, moved here from SetupDevice:
	// a scissor rectangle is dynamic state applied at bind time, not device
	// state that persists until changed.
	if (bEnableScissor) ovr.scissor = &ScissorRect;

	// The topology. It was the D3DPRIMITIVETYPE argument to the draw on
	// Windows; it is pipeline state here, so it is declared before the bind.
	//
	// A poly object carries its OWN topology -- VulkanTriangle draws a list,
	// a fan or a strip depending on its style -- and the pipeline is bound
	// before its Draw() is reached, so it has to be asked now. See
	// VulkanPolyBase::Topology.
	if (hPoly) FX->SetTopology(static_cast<VulkanPolyBase *>(hPoly)->Topology());
	else if (tCurrent == TRIANGLE) FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	else if (tCurrent == LINE)     FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
	else { iI = vI = 0; return false; }

	// THESE TWO RETURNS USED TO BE SILENT, AND THAT IS A DEFECT IN ITSELF.
	//
	// Both discard the whole batch. The caller cannot tell: VulkanPad::Flush's
	// bool is not checked by CopyRect/StretchRect/Text, and clbkScaleBlt's
	// Sketchpad branch ends in `return AutoGenMips(tgt)` -- which returns true
	// unconditionally. So a pad that cannot open a pass renders nothing and
	// every layer above it reports success.
	//
	// WHAT THAT HID: the Delta-glider's registration panel drew as a black
	// rectangle. The DG composes it at runtime -- clbkSetClassCaps copies
	// idpanel1.dds into a render target, clbkPostCreation writes the vessel
	// name over it -- and because idpanel1.dds is DXT1, clbkScaleBlt's two
	// BlitTexture paths are both gated on !bSC and it is forced down the
	// Sketchpad route, the one route that needs a live render pass. The name
	// appeared and the background did not, with a completely clean log.
	//
	// Reported once per reason, not per call: this runs at frame rate.
	if (!FX->Begin(&numPasses, 0)) {
		static bool bSaid = false;
		if (!bSaid) {
			bSaid = true;
			LogErr("VulkanPad::Flush() dropped a batch -- Effect::Begin() "
				   "refused. target=%ux%u vI=%hu iI=%hu. Nothing was drawn "
				   "and the caller was told it succeeded.",
				   tgt_desc.Width, tgt_desc.Height, vI, iI);
		}
		iI = vI = 0;
		return false;
	}

	// Pass 0 is ORTHO, pass 1 is the 3D view mode. Unchanged.
	if (!FX->BeginPassEx((vmode == ORTHO) ? 0 : 1, &ovr)) {
		static bool bSaid = false;
		if (!bSaid) {
			bSaid = true;
			LogErr("VulkanPad::Flush() dropped a batch -- BeginPassEx(%d) "
				   "refused. target=%ux%u vI=%hu iI=%hu. Nothing was drawn "
				   "and the caller was told it succeeded.",
				   (vmode == ORTHO) ? 0 : 1,
				   tgt_desc.Width, tgt_desc.Height, vI, iI);
		}
		FX->End();
		iI = vI = 0;
		return false;
	}

	// Bisection aid: ORBITER_VK_SKPCLEAR=1 clears the whole target to magenta
	// inside the pad's own pass, immediately before its draw. If the target
	// then reads back magenta the pass and the image are right and the
	// geometry is at fault; if it reads back unchanged, the pass is not
	// landing on this image at all. Diagnostic only.
	{
		static const bool bSkpClear = (getenv("ORBITER_VK_SKPCLEAR") != NULL);
		if (bSkpClear) pDev->ClearFrame(true, false, false, 0xFFFF00FF, 1.0f, 0);
	}

	if (hPoly) {
		assert(iI == 0);
		VulkanPolyBase *pBase = static_cast<VulkanPolyBase *>(hPoly);
		pBase->Draw(this, pDev);
	}
	else {
		// Was DrawIndexedPrimitiveUP with a primitive COUNT -- iI/3 triangles
		// or iI/2 lines. Vulkan's vkCmdDrawIndexed takes an INDEX count, so
		// the division goes away rather than being repeated: DrawUP is given
		// iI directly.
		FX->DrawUP(Vtx, vI, sizeof(SkpVtx), Idx, iI);
	}

	FX->EndPass();
	FX->End();

	nFlushedVtx += vI;		// diagnostic; see the declaration

	iI = vI = 0;

	return true;
}


// ===============================================================================================
//
bool VulkanPad::Topology(Topo tRequest)
{
	if (tRequest == LINE) {
		if (bLine) { // Can we do LINE Topology ?
			if (tRequest != tCurrent) Change |= SKPCHG_TOPOLOGY;
			SetupDevice(tRequest);
			return true;
		}
		return false;
	}

	if (tRequest != tCurrent) Change |= SKPCHG_TOPOLOGY;

	SetupDevice(tRequest);

	return true;
}


// ===============================================================================================
//
void VulkanPad::SetupDevice(Topo tNew)
{

	// Check that this is the top one. Only do the check for ones created with oapiGetSketchpad()
	//if (GetSurface()) assert(gc->GetTopInterface() == this);

	// If the queue is filling up, Flush it
	//
	if (iI > (nQueueMax >> 1)) Flush();


	// Has something changed ?
	//
	if (!Change) return;


	// Flush pending drawing instructions before changing a device state
	//
	Flush();

	tCurrent = tNew;

#ifdef SKPDBG
	char buf[512];
	strcpy_s(buf, 512, "");
	if (Change&SKPCHG_TOPOLOGY)	 strcat_s(buf, 512, "SKPCHG_TOPOLOGY ");
	if (Change&SKPCHG_TRANSFORM) strcat_s(buf, 512, "SKPCHG_TRANSFORM ");
	if (Change&SKPCHG_CLIPCONE)	 strcat_s(buf, 512, "SKPCHG_CLIPCONE ");
	if (Change&SKPCHG_PEN) strcat_s(buf, 512, "SKPCHG_PEN ");
	if (Change&SKPCHG_EFFECTS)	 strcat_s(buf, 512, "SKPCHG_EFFECTS ");
	if (Change&SKPCHG_CLIPRECT)	 strcat_s(buf, 512, "SKPCHG_CLIPRECT ");
	if (Change&SKPCHG_TEXTURE)	strcat_s(buf, 512, "SKPCHG_TEXTURE ");
	if (Change&SKPCHG_FONT)	strcat_s(buf, 512, "SKPCHG_FONT ");
	if (Change&SKPCHG_DEPTH)	strcat_s(buf, 512, "SKPCHG_DEPTH ");
	if (Change&SKPCHG_PATTERN)	strcat_s(buf, 512, "SKPCHG_PATTERN ");
	Log("StateChange = [%s]", buf);
#endif

	if (!FX) { Change = 0; return; }

	// Apply a new setup -----------------------------------------------------------------
	//
	if (Change == SKPCHG_TOPOLOGY) {
		FX->SetBool(eWide, (tCurrent == TRIANGLE));
		Change = 0;
		return;
	}


	// Apply a new setup -----------------------------------------------
	//
	if (Change & (SKPCHG_TRANSFORM | SKPCHG_CLIPCONE)) {

		if (vmode == ORTHO) {
			FMATRIX4 mWVP;
			VMAT_MatrixMultiply(&mWVP, &mW, &mO);
			FX->SetMatrix(eWVP, &mWVP);
			FX->SetMatrix(eVP, &mO);
			FX->SetMatrix(eW, &mW);
			FX->SetBool(eCovEn, false);
		}
		else {
			// mP._22 became mP.m22 -- the same element, D3DXMATRIX's own
			// leading-underscore naming being the only difference.
			float d = float(tgt_desc.Height) * mP.m22;
			float f = atan(1.0f / d) * 1.7f;
			VMAT_MatrixMultiply(&mVP, &mV, &mP);
			FX->SetMatrix(eVP, &mVP);
			FX->SetMatrix(eW, &mW);
			FX->SetFloat(eFov, f);
			FX->SetBool(eCovEn, ClipData[0].bEnable || ClipData[1].bEnable);
		}
	}


	// Apply a new setup -----------------------------------------------------------------
	//
	if (Change & SKPCHG_PEN) {
		float offset = 0.0f;
		int w = int(ceil(GetPenWidth()));
		if ((w & 1) == 0) offset = 0.5f;
		FX->SetValue(ePen, &pencolor.fclr, sizeof(FVECTOR4));
		FX->SetBool(eDashEn, IsDashed());
		const FVECTOR3 width(GetPenWidth(), pattern*0.13f, offset);
		FX->SetValue(eWidth, &width, sizeof(FVECTOR3));
	}


	// Apply a new setup -----------------------------------------------------------------
	//
	if (Change & SKPCHG_EFFECTS) {
		FX->SetValue(eNoiseColor, &Noise, sizeof(FVECTOR4));
		FX->SetValue(eGamma, &Gamma, sizeof(FVECTOR4));
		FX->SetTexture(eNoiseTex, pNoise);
		FX->SetValue(eColorMatrix, &ColorMatrix, sizeof(FMATRIX4));
		FX->SetBool(eEffectsEn, Enable != 0);
	}


	// Apply a new setup -----------------------------------------------------------------
	//
	// Was SetScissorRect + D3DRS_SCISSORTESTENABLE here. Both moved into
	// Flush's PassOverride: a scissor rectangle is dynamic state applied when
	// the pipeline is bound, not device state that persists between draws.
	// The SKPCHG_CLIPRECT flag stays -- it still means "the rectangle moved,
	// flush what is queued before it does" -- and the flag is what forced the
	// Flush at the top of this function.


	// Apply a new setup -----------------------------------------------------------------
	//
	if (Change & SKPCHG_CLIPCONE) {
		if (ClipData[0].bEnable || ClipData[1].bEnable) {
			FX->SetValue(ePos, &ClipData[0].uDir, sizeof(FVECTOR3));
			FX->SetValue(ePos2, &ClipData[1].uDir, sizeof(FVECTOR3));
			const FVECTOR4 cov(ClipData[0].ca, ClipData[0].dst,
							   ClipData[1].ca, ClipData[1].dst);
			FX->SetValue(eCov, &cov, sizeof(FVECTOR4));
		}
	}


	// Apply a new setup -----------------------------------------------------------------
	//
	if (Change & (SKPCHG_TEXTURE | SKPCHG_FONT)) {

		float tw = 1.0f, th = 1.0f;

		if (hTexture) {

			// Was hTexture->GetLevelDesc(0, &desc), which asked the texture
			// for its own dimensions. A VkImage answers nothing about itself,
			// so the recorded description is read instead -- see
			// VulkanTypes.h. Level 0 is what Desc() holds.
			const VulkanImageDesc &desc = hTexture->Desc();

			tw = 1.0f / float(desc.Width);
			th = 1.0f / float(desc.Height);

			if (Change & SKPCHG_TEXTURE) {
				FX->SetTexture(eTex0, hTexture);
				FX->SetBool(eKeyEn, bColorKey);
				FX->SetValue(eKey, &cColorKey, sizeof(FVECTOR4));
			}
		}

		if (hFontTex) {
			if (Change & SKPCHG_FONT) {
				FX->SetTexture(eFnt0, hFontTex);
			}
		}

		const FVECTOR4 size(tw, th, 1.0f, 1.0f);
		FX->SetVector(eSize, &size);
		FX->SetBool(eTexEn, (hTexture != NULL));
		FX->SetBool(eFntEn, (hFontTex != NULL));
	}


	// Apply a new setup -----------------------------------------------------------------
	//
	if (Change & SKPCHG_TOPOLOGY) {
		FX->SetBool(eWide, (tCurrent == TRIANGLE));
	}

	// All Clear
	Change = 0;

	return;
}


// ===============================================================================================
//
DWORD VulkanPad::ColorComp(DWORD c) const
{
	if (bColorComp) if ((c & 0xFF000000) == 0) return c | 0xFF000000;
	return c;
}

// ===============================================================================================
//
SkpColor VulkanPad::ColorComp(const SkpColor &c) const
{
	if (bColorComp) {
		if ((c.dclr & 0xFF000000) == 0) {
			DWORD q = c.dclr | 0xFF000000;
			return SkpColor(q);
		}
	}
	return c;
}


// ===============================================================================================
//
HDC VulkanPad::GetDC()
{
	DWORD *cf = SURFACE(GetSurface())->GetClientFlags();

	if ((*cf & OAPISURF_SKP_GDI_WARN) == 0) {
		*cf |= OAPISURF_SKP_GDI_WARN;
		LogErr("Call to obsolete Sketchpad::GetDC() detected. Returned NULL");
		if (Config->DebugBreak) DebugBreak();
	}

	return NULL;
}


// ===============================================================================================
//
Font *VulkanPad::SetFont(Font *font)
{
	if (cfont == font) return font;

#ifdef SKPDBG
	LOGFONTA lf;
	GetObjectA(font->GetGDIFont(), sizeof(LOGFONT), &lf);
	Log("SetFont(%s) Face=[%s] Height=%d Weight=%d", _PTR(font), lf.lfFaceName, lf.lfHeight, lf.lfWeight);
#endif
	// No "Change" falgs required here, covered in SetFontTextureNative()

	Font *pfont = cfont;
	if (font) cfont = font;
	else      cfont = deffont;
	return pfont;
}


// ===============================================================================================
//
Brush *VulkanPad::SetBrush (Brush *brush)
{
	if (cbrush == brush && QBrush.bEnabled == false) return brush;

#ifdef SKPDBG
	Log("SetBrush(%s)", _PTR(brush));
#endif

	// No "Change" falgs required here, color stored in vertex data

	QBrush.bEnabled = false;

	Brush *pbrush = cbrush;
	cbrush = brush;
	if (cbrush) brushcolor = ColorComp((static_cast<VulkanPadBrush *>(cbrush))->clr);
	else	    brushcolor = SkpColor(0);

	IsLineTopologyAllowed();

	return const_cast<Brush*>(pbrush);
}


// ===============================================================================================
//
Pen *VulkanPad::SetPen (Pen *pen)
{
	if (cpen == pen && QPen.bEnabled == false) return pen;

#ifdef SKPDBG
	Log("SetPen(%s)", _PTR(pen));
#endif

	// Change required due to pen width and style change
	Change |= SKPCHG_PEN;

	QPen.bEnabled = false;

	Pen *ppen = cpen;
	if (pen) cpen = pen;
	else     cpen = NULL;
	if (cpen) pencolor = ColorComp(static_cast<VulkanPadPen *>(cpen)->clr);

	IsLineTopologyAllowed();

	return ppen;
}


// ===============================================================================================
//
void VulkanPad::SetTextAlign (TAlign_horizontal _tah, TAlign_vertical _tav)
{
	// No Change flags
	tah = _tah;
	tav = _tav;
}


// ===============================================================================================
//
DWORD VulkanPad::SetTextColor(DWORD col)
{
	// Color stored in vertex data, no Change required
	DWORD prev = textcolor.dclr;
	textcolor = SkpColor(ColorComp(col));
	return prev;
}


// ===============================================================================================
//
DWORD VulkanPad::SetBackgroundColor(DWORD col)
{
	// Color stored in vertex data, no Change required
	DWORD prev = bkcolor.dclr;
	bkcolor = SkpColor(ColorComp(col));
	return prev;
}


// ===============================================================================================
//
void VulkanPad::SetBackgroundMode(BkgMode mode)
{
	// No Change required

	switch (mode) {
		case BK_TRANSPARENT: bkmode = TRANSPARENT; break;
		case BK_OPAQUE:      bkmode = OPAQUE; break;
	}
}


// ===============================================================================================
//
DWORD VulkanPad::GetCharSize ()
{
	TEXTMETRIC tm;
	if (cfont==NULL) return 0;
	static_cast<const VulkanPadFont *>(cfont)->pFont->GetVulkanTextMetrics(&tm);
	return MAKELONG(tm.tmHeight-tm.tmInternalLeading, tm.tmAveCharWidth);
}


// ===============================================================================================
//
DWORD VulkanPad::GetLineHeight () // ... *with* "internal leading"
{
	TEXTMETRIC tm;
	if (cfont == NULL) return 0;
	static_cast<const VulkanPadFont *>(cfont)->pFont->GetVulkanTextMetrics(&tm);
	return tm.tmHeight;
}


// ===============================================================================================
//
DWORD VulkanPad::GetTextWidth (const char *utf8, int ulen)
{
	if (utf8) if (utf8[0] == '_') if (strcmp(utf8, "_SkpVerInfo") == 0) return 2;
	if (cfont==NULL) return 0;

	std::string str = UTF8ToCP1252(utf8, ulen);

	return DWORD(static_cast<VulkanPadFont *>(cfont)->pFont->Length2(str.c_str(), str.length()));
}


// ===============================================================================================
//
void VulkanPad::SetOrigin (int x, int y)
{
#ifdef SKPDBG
	Log("SetOrigin(%d, %d)", x, y);
#endif
	Change |= SKPCHG_TRANSFORM;

	mW.m41 = float(x);
	mW.m42 = float(y);
}


// ===============================================================================================
//
void VulkanPad::GetOrigin(int *x, int *y) const
{
	if (x) *x = int(mW.m41);
	if (y) *y = int(mW.m42);
}


// ===============================================================================================
//
bool VulkanPad::HasPen() const
{
	if (QPen.bEnabled) return true;
	if (cpen==NULL) return false;
	if (static_cast<VulkanPadPen*>(cpen)->style==PS_NULL) return false;
	return true;
}


// ===============================================================================================
//
void VulkanPad::IsLineTopologyAllowed()
{
	bLine = false;
	if ((HasPen() == true) && (GetPenWidth() < 1.1f)) bLine = true;
}


// ===============================================================================================
//
bool VulkanPad::IsDashed() const
{
	if (QPen.bEnabled) return QPen.style == 2;
	if (cpen==NULL) return false;
	if (static_cast<VulkanPadPen*>(cpen)->style==PS_DOT) return true;
	return false;
}


// ===============================================================================================
//
// The three formats it tests for are the three that carry an alpha channel.
// D3DFMT_A8R8G8B8 is VK_FORMAT_B8G8R8A8_UNORM here -- see the note in
// VulkanSurface.h on why the X/A distinction is carried by the surface flags
// rather than by the Vulkan format -- so the test on the FORMAT alone can no
// longer tell A8R8G8B8 from X8R8G8B8. That distinction is what
// OAPISURFACE_ALPHA/NOALPHA exists for, and the render target's own flags are
// where it lives; tgt_desc has only the format, so the two float formats are
// tested as before and the 8-bit case asks the surface.
//
bool VulkanPad::IsAlphaTarget() const
{
	if (tgt_desc.Format == VK_FORMAT_R16G16B16A16_SFLOAT) return true;
	if (tgt_desc.Format == VK_FORMAT_R32G32B32A32_SFLOAT) return true;
	if (tgt_desc.Format == VK_FORMAT_B8G8R8A8_UNORM) {
		SURFHANDLE hSrf = GetSurface();
		if (hSrf) return (SURFACE(hSrf)->Flags & OAPISURFACE_NOALPHA) == 0;
		// No surface to ask -- the pad was bound to a bare render target.
		// B8G8R8A8 has the channel, which is what the reference assumed.
		return true;
	}
	return false;
}


// ===============================================================================================
//
bool VulkanPad::HasBrush() const
{
	if (QBrush.bEnabled) return true;
	return (cbrush != NULL);
}


// ===============================================================================================
//
float VulkanPad::GetPenWidth() const
{
	if (QPen.bEnabled) return linescale * QPen.width;
	if (cpen==NULL) return 1.0f;
	return float(static_cast<VulkanPadPen*>(cpen)->width*linescale);
}


// ===============================================================================================
//
void VulkanPad::WrapOneLine (char* str, int len, int maxWidth)
{
	VulkanTextPtr pText = static_cast<VulkanPadFont *>(cfont)->pFont;
	if (pText->Length2(str) > maxWidth) {
		char *pStr = str, // sub-string start
		     *it = pStr,  // 'iterator' char
		     *pEnd = str + len, // <= point to terminating zero
		     *pLastSpace = NULL;
		float currentWidth = 0;
		while (it < pEnd)
		{
			while (it < pEnd && currentWidth < maxWidth) {
				if (*it == ' ') { pLastSpace = it; }
				currentWidth = pText->Length2( pStr, int(it - pStr + 1) );
				++it;
			}
			// only split if we have space for it AND we have to (avoids cutting the last word)
			if (pLastSpace != NULL && currentWidth >= maxWidth) {
				*pLastSpace = '\n';
				pStr = pLastSpace + 1; // skip the space (now a newline)
				currentWidth = 0;
				pLastSpace = NULL;
			}
		}
	}
}

// ===============================================================================================
//
bool VulkanPad::TextBox (int x1, int y1, int x2, int y2, const char *utf8, int ulen)
{
#ifdef SKPDBG
	Log("TextBox()");
#endif

	// No "Setup" required, done on PrintSkp

	if (cfont==NULL) return false;

	bool result = true;
	int lineSpace = static_cast<VulkanPadFont *>(cfont)->pFont->GetLineSpace();

	ToSaveBuffer(utf8, ulen);

	char *pch, *pEnd =_saveBuffer+ulen; // <= point to terminating zero
	for (pch = strtok(_saveBuffer, "\n"); pch != NULL; pch = strtok(NULL, "\n"))
	{
		int _len = lstrlen(pch);
		if (_len>1) { WrapOneLine(pch, _len, x2-x1); }
		if (pch+_len < pEnd) { *(pch+_len) = '\n'; } // strtok splits by inserting '\0's => revert'em
	}

	// "forEach(line...)" split multi-lines
	for (pch = strtok(_saveBuffer, "\n"); pch != NULL; pch = strtok(NULL, "\n")) {
		result = Text(x1, y1, pch, -1); // len is irrelevant for pointer into 'save' buffer
		y1 += lineSpace;
	}

	ReleaseSaveBuffer();
	return result;
}


// ===============================================================================================
//
bool VulkanPad::Text (int x, int y, const char *utf8, int ulen)
{
	std::string str = UTF8ToCP1252(utf8, ulen);

#ifdef SKPDBG
	Log("Text(%s)", str.c_str());
#endif
	// No "Setup" required, done on PrintSkp

	if (cfont==NULL) return false;

	VulkanTextPtr pText = static_cast<VulkanPadFont *>(cfont)->pFont;

	switch(tah) {
		default:
		case LEFT:   pText->SetTextHAlign(0); break;
		case CENTER: pText->SetTextHAlign(1); break;
		case RIGHT:  pText->SetTextHAlign(2); break;
	}

	switch(tav) {
		default:
		case TOP:      pText->SetTextVAlign(0); break;
		case BASELINE: pText->SetTextVAlign(1); break;
		case BOTTOM:   pText->SetTextVAlign(2); break;
	}

	pText->SetRotation(static_cast<VulkanPadFont *>(cfont)->rotation);
	pText->SetScaling(1.0f);
	pText->PrintSkp(this, float(x - 1), float(y - 1), str.c_str(), str.length(), (bkmode == OPAQUE));

	return true;
}


// ===============================================================================================
//
void SwapRB(DWORD *c)
{
	DWORD r = ((*c) & 0x00FF0000) >> 16;
	DWORD b = ((*c) & 0x000000FF) << 16;
	*c = ((*c) & 0xFF00FF00) | b | r;
}


// ===============================================================================================
//
void VulkanPad::Pixel (int x, int y, DWORD col)
{
	FillRect(x, y, x + 1, y + 2, ColorComp(SkpColor(col)));
}


// ===============================================================================================
//
void VulkanPad::MoveTo (int x, int y)
{
	cx = x;
	cy = y;
}


// ===============================================================================================
//
void VulkanPad::LineTo (int tx, int ty)
{
	if (!HasPen()) return;
#ifdef SKPDBG
	Log("LineTo()");
#endif
	Line(cx, cy, tx, ty);
	cx=tx; cy=ty;
}


// ===============================================================================================
//
void VulkanPad::Line (int x0, int y0, int x1, int y1)
{
	if (!HasPen()) return;
#ifdef SKPDBG
	Log("Line()");
#endif
	IVECTOR2 pt[2];

	pt[0].x = x0; pt[0].y = y0;
	pt[1].x = x1; pt[1].y = y1;

	AppendLineVertexList<IVECTOR2>(pt);

	cx = x1; cy = y1;
}


// ===============================================================================================
//
void VulkanPad::FillRect(int l, int t, int r, int b, const SkpColor &c)
{
	if (r == l) return;
	if (b == t) return;
	if (r < l) swap(r, l);
	if (b < t) swap(t, b);

#ifdef SKPDBG
	Log("FillRect()");
#endif
	if (Topology(TRIANGLE)) {
		AddRectIdx(vI);
		SkpVtxIC(Vtx[vI++], l, t, c);
		SkpVtxIC(Vtx[vI++], r, t, c);
		SkpVtxIC(Vtx[vI++], r, b, c);
		SkpVtxIC(Vtx[vI++], l, b, c);
	}
}


// ===============================================================================================
//
void VulkanPad::Rectangle (int l, int t, int r, int b)
{
	if (r == l) return;
	if (b == t) return;
	if (r < l) swap(r, l);
	if (b < t) swap(t, b);

#ifdef SKPDBG
	Log("Rectangle()");
#endif
	// Who fills and with what. Reported once per distinct state so a frame of
	// HUD drawing does not flood the log. Diagnostic only; env-gated.
	static const bool bTraceSkp = (getenv("ORBITER_VK_TRACE_SKP") != NULL);
	if (bTraceSkp) {
		static std::map<std::string, int> seen;
		char key[192];
		sprintf_s(key, "%s|brush=%d|bc=%08X|pen=%d|pc=%08X|q=%d",
				  name, int(HasBrush()), brushcolor.dclr,
				  int(HasPen()), pencolor.dclr, int(QBrush.bEnabled));
		if (seen.find(key) == seen.end()) {
			seen[key] = 1;
			LogErr("SKPTRACE Rectangle %s", key);
		}
	}
	r--;
	b--;

	// Fill interion ----------------------------------------------
	//
	if (HasBrush()) FillRect(l, t, r, b, brushcolor);

	// Draw outline ------------------------------------------
	//
	if (HasPen()) {

		IVECTOR2 pts[4];
		pts[0].x = pts[3].x = l;
		pts[0].y = pts[1].y = t;
		pts[1].x = pts[2].x = r;
		pts[2].y = pts[3].y = b;

		AppendLineVertexList<IVECTOR2>(pts, 4, true);
	}
}


// ===============================================================================================
//
void VulkanPad::Ellipse (int x0, int y0, int x1, int y1)
{
	if (x1 == x0) return;
	if (y1 == y0) return;
	if (x1 < x0) swap(x0, x1);
	if (y1 < y0) swap(y0, y1);

#ifdef SKPDBG
	Log("Ellipse()");
#endif

	float w = float(x1 - x0); float h = float(y1 - y0);	float fx0 = float(x0); float fy0 = float(y0);
	// Was max((x1-x0), (y1-y0)). The shim supplies no max() macro -- see
	// VulkanUtil.h -- and std::max would need both arguments the same type.
	DWORD z = DWORD((x1 - x0) > (y1 - y0) ? (x1 - x0) : (y1 - y0));

	w *= 0.5f;
	h *= 0.5f;
	//fl += w;
	//ft += h;

	IVECTOR2 pts[65] = { };

	WORD n = 8;
	WORD q = 0;

	if (z > 16) q = 1, n = 16;
	if (z > 32) q = 2, n = 32;
	if (z > 64) q = 3, n = 64;

	for (WORD i = 0; i<n; i++) {
		pts[i].x = long(fx0 + pSinCos[q][i].x * w);
		pts[i].y = long(fy0 + pSinCos[q][i].y * h);
	}


	// Fill interion -------------------------------------------
	//
	if (HasBrush()) {
		if (Topology(TRIANGLE)) {

			WORD aV = vI;

			SkpVtxIC(Vtx[vI++], (x1 + x0) / 2, (y1 + y0) / 2, brushcolor);

			for (WORD i = 0; i < n; i++) SkpVtxIC(Vtx[vI++], pts[i].x, pts[i].y, brushcolor);
			for (WORD i = 0; i < n; i++) {
				Idx[iI++] = aV;
				Idx[iI++] = aV + i + 1;
				Idx[iI++] = aV + i + 2;
			}
			Idx[iI - 1] = aV + 1;
		}
	}

	// Draw outline ------------------------------------------
	//
	if (HasPen()) AppendLineVertexList<IVECTOR2>(pts, n, true);
}


// ===============================================================================================
//
void VulkanPad::Polygon (const IVECTOR2 *pt, int npt)
{
#ifdef SKPDBG
	Log("Polygon(%d)", npt);
#endif

	if (npt<3) return;
	// The VectorMap drawing code creates polygons with at least 68 points
	// Let's accept up to 70
	if (HasBrush() && npt > 70) return;

	// Create filled polygon interior -----------------------------------------
	//
	if (HasBrush()) {
		if (Topology(TRIANGLE)) {

			int sIdx = vI;

			// File a vertex buffer.
			for (int i = 0; i < npt; i++) SkpVtxIC(Vtx[vI++], pt[i].x, pt[i].y, brushcolor);

			WORD qIdx[256];
			int nIdx = CreatePolyIndexList<IVECTOR2>(pt, (short)npt, qIdx);

			// Add indices to index buffer
			for (int i = 0; i < nIdx; i++) Idx[iI++] = qIdx[i] + sIdx;
		}
	}

	// Draw outline ------------------------------------------
	//
	if (HasPen()) AppendLineVertexList<IVECTOR2>(pt, npt, true);
}


// ===============================================================================================
//
void VulkanPad::Polyline (const IVECTOR2 *pt, int npt)
{
#ifdef SKPDBG
	Log("Polyline(%d)", npt);
#endif
	if (npt < 2) return;
	if (HasPen()) AppendLineVertexList<IVECTOR2>(pt, npt, false);
}


// ===============================================================================================
//
void VulkanPad::DrawPoly (HPOLY hPoly, DWORD flags)
{
#ifdef SKPDBG
	Log("DrawPoly(%s, 0x%X)", _PTR(hPoly), flags);
#endif

	if (hPoly) {

		VulkanPolyBase *pBase = static_cast<VulkanPolyBase *>(hPoly);
		int PolyType = pBase->type;

		if ((PolyType == 0) && !HasPen()) return;

		// Flush pending graphics before a use of different interface
		Flush();

		if (Topology(TRIANGLE)) {

			// Flush the poly object
			Flush(hPoly);
		}
	}
}


// ===============================================================================================
//
void VulkanPad::Lines(const FVECTOR2 *pt, int nlines)
{
#ifdef SKPDBG
	Log("Lines(%d)", nlines);
#endif
	if (!HasPen()) return;
	for (int i = 0; i < nlines; i++) {
		AppendLineVertexList<FVECTOR2>(pt);
		pt += 2;
	}
}


// -----------------------------------------------------------------------------------------------
// Save buffer helpers (null-terminated string for Text & TextBox)
// -----------------------------------------------------------------------------------------------

// ===============================================================================================
// Copy string to internal 'save' buffer, so it can be changed (adding terminating zeroes, etc.)
void VulkanPad::ToSaveBuffer (const char *str, int len)
{
	if (_saveBufferSize < len)
	{ // re-allloc bigger space
		if (_saveBuffer) { delete[] _saveBuffer; }
		_saveBuffer = new char[len + 1];
		_saveBufferSize = len;
	}
	strncpy_s(_saveBuffer, len + 1, str, len);
	_isSaveBuffer = true;
}

// ===============================================================================================
//
void VulkanPad::ReleaseSaveBuffer () {
	_isSaveBuffer = false;
}


// -----------------------------------------------------------------------------------------------
// Subroutines Section
//
// Everything from here to the end of the templates is arithmetic on the
// client's own arrays. No device call appears in any of it on either
// platform, so it is carried over unchanged apart from the vector type.
// -----------------------------------------------------------------------------------------------

// ===============================================================================================
//
short mod(short a, short b)
{
	if (a<0) return b-1;
	if (a>=b) return 0;
	return a;
}


// ===============================================================================================
//
template <typename Type>
int CheckTriangle(short x, const Type *pt, const WORD *Idx, float hd, short npt, bool bSharp)
{
	WORD A = Idx[x];
	WORD B = Idx[mod(x-1,npt)];
	WORD C = Idx[mod(x+1,npt)];

	float bx = float(pt[B].x - pt[A].x);
	float by = float(pt[B].y - pt[A].y);
	float ax = float(pt[C].x - pt[A].x);
	float ay = float(pt[C].y - pt[A].y);

	if ((bx*ay-by*ax)*hd > 0.0f) return 0;	// Check handiness

	float aa = ax*ax + ay*ay;			// dot(a,a)
	float ab = ax*bx + ay*by;			// dot(a,b)
	float bb = bx*bx + by*by;			// dot(b,b)

	float qw = fabs(ab) / sqrt(aa*bb);	// abs(cos(a,b))
	if (bSharp && qw>0.9f) return 0;	// Bad Ear

	float id = 1.0f / (aa * bb - ab * ab);

	for (int i=0;i<npt;i++) {

		WORD P = Idx[i];

		if ((P==B) || (P==A) || (P==C)) continue;

		float cx = float(pt[P].x - pt[A].x);
		float cy = float(pt[P].y - pt[A].y);
		float ac = ax*cx + ay*cy;
		float bc = bx*cx + by*cy;
		float u  = (bb*ac - ab*bc) * id;
		float v  = (aa*bc - ab*ac) * id;

		// Check if the point is inside the triangle
		// NOTE: Having u+v slightly above 1.0 is a bad condition, should find a better ear.
		if  ((u>-0.0001) && (v>-0.0001) && ((u+v)<1.0001f)) return 0;
	}

	return 1; // It's an ear
}


// ===============================================================================================
//
template <typename Type>
int CreatePolyIndexList(const Type *pt, short npt, WORD *Out)
{
	if (npt > 255) return 0;
	if (npt==3) { Out[0]=0; Out[1]=1; Out[2]=2;	return 3; }

	short idx = 0;		// Number of indices written in the output
	short x = npt-1;	// First ear to test is the last one in the list
	bool bSharp = false;// Avoid sharp ears

	// Build initial index list
	WORD In[256];
	for (int i=0;i<npt;i++) In[i]=WORD(i);
	float sum = 0;
	int k = npt-1;
	int nr = 0;
	for (int i=0;i<k;i++) sum += (float(pt[i].x)*float(pt[(i+1)%k].y) - float(pt[(i+1)%k].x)*float(pt[i].y));

	if (sum>0) sum=1.0; else sum=-1.0;

	while (npt>3) {

		switch (CheckTriangle<Type>(x, pt, In, sum, npt, bSharp)) {

			case 0:
			{
				x--;
				if (x<0) { // Restart
					if (!bSharp && nr>10) return idx;
					bSharp = false;
					x = npt - 1;
					nr++;
				}
				break;
			}

			case 1:
			{
				Out[idx] = In[mod(x-1,npt)]; idx++;
				Out[idx] = In[mod(x,npt)]; idx++;
				Out[idx] = In[mod(x+1,npt)]; idx++;
				npt--;
				for (int i=x;i<npt;i++) In[i]=In[i+1];
				x = mod(x-1,npt);
				break;
			}
		}
	}

	Out[idx] = In[0]; idx++;
	Out[idx] = In[1]; idx++;
	Out[idx] = In[2]; idx++;

	return idx;
}


// _FV2, _FV2Extrapolate and _FV2Length stood here as file-local helpers.
// They are in VulkanPad.h now, because VulkanPad2.cpp's
// VulkanPolyLine::Update needs the same extrapolation and duplicating it is
// how two line renderers end up disagreeing about their end caps.


// ===============================================================================================
//
template <typename Type>
void VulkanPad::AppendLineVertexList(const Type *pt, int _npt, bool bLoop)
{
	if (_npt < 2) return;


	// ----------------------------------------------------------------------
	// Draw a thin hairline
	// ----------------------------------------------------------------------

	if (Topology(LINE)) {

		WORD npt = WORD(_npt);
		WORD wL = vI;
		WORD li = WORD(npt - 1);
		WORD aV;
		float length = 0.0f;

		// Create line segments -------------------------------------------------
		//
		for (WORD i = 0; i<npt; i++) {

			Vtx[vI].x = float(pt[i].x);
			Vtx[vI].y = float(pt[i].y);
			Vtx[vI].l = length;
			Vtx[vI].fnc = SKPSW_CENTER | SKPSW_FRAGMENT;
			Vtx[vI].clr = pencolor.dclr;

			if (IsDashed() && i!=li) {
				float x = float(pt[i].x - pt[i+1].x);
				float y = float(pt[i].y - pt[i+1].y);
				length += sqrt(x*x + y*y);
			}
			vI++;
		}
		aV = wL;
		for (WORD i = 0; i < (npt-1); i++) {
			Idx[iI++] = aV;
			aV++;
			Idx[iI++] = aV;
		}

		// Last segment ---------------------------------------------------------
		//
		if (bLoop) {
			Idx[iI++] = WORD(vI - 1);
			Idx[iI++] = wL;
		}

		return;
	}



	// ----------------------------------------------------------------------
	// Wide line mode
	// ----------------------------------------------------------------------

	if (Topology(TRIANGLE)) {

		WORD npt = WORD(_npt);
		WORD wL = vI;
		WORD li = WORD(npt - 1);
		WORD aV = 0, bV = 0, cV = 0, dV = 0;
		float length = 0.0f;

		FVECTOR2 pp; // Prev point
		FVECTOR2 np;	// Next point

		// Line Init ------------------------------------------------------------
		//
		if (bLoop) pp = _FV2(pt[npt - 1]);
		else	   pp = _FV2Extrapolate(_FV2(pt[0]), _FV2(pt[1]));

		// Create line segments -------------------------------------------------
		//
		for (WORD i = 0; i < npt; i++) {

			if (i != li)	np = _FV2(pt[i + 1]);
			else {
				if (bLoop)	np = _FV2(pt[0]);
				else		np = _FV2Extrapolate(_FV2(pt[i]), _FV2(pt[i - 1]));
			}

			WORD vII = WORD(vI + 1);

			// --------------------------------------
			Vtx[vI].x = Vtx[vII].x = float(pt[i].x);
			Vtx[vI].y = Vtx[vII].y = float(pt[i].y);
			Vtx[vI].nx = Vtx[vII].nx = np.x;
			Vtx[vI].ny = Vtx[vII].ny = np.y;
			Vtx[vI].px = Vtx[vII].px = pp.x;
			Vtx[vI].py = Vtx[vII].py = pp.y;
			Vtx[vI].l = Vtx[vII].l = length;
			Vtx[vI].clr = Vtx[vII].clr = pencolor.dclr;
			// --------------------------------------

			Vtx[vI].fnc = SKPSW_WIDEPEN_L | SKPSW_FRAGMENT;
			aV = vI; vI++;
			Vtx[vI].fnc = SKPSW_WIDEPEN_R | SKPSW_FRAGMENT;
			bV = vI; vI++;
			// --------------------------------------

			if (i > 0) {
				Idx[iI++] = cV;	Idx[iI++] = aV;
				Idx[iI++] = dV;	Idx[iI++] = dV;
				Idx[iI++] = aV;	Idx[iI++] = bV;
			}

			cV = aV;
			dV = bV;

			pp = _FV2(pt[i]);

			// Was D3DXVec2Length(ptr(np - pp)).
			if (IsDashed()) length += _FV2Length(np, pp);
		}

		// Last segment ---------------------------------------------------------
		//
		if (bLoop) {
			Idx[iI++] = wL;		Idx[iI++] = aV;
			Idx[iI++] = WORD(wL + 1);	Idx[iI++] = WORD(wL + 1);
			Idx[iI++] = aV;		Idx[iI++] = bV;
		}
	}
}


// ===============================================================================================
//
template <typename Type>
void VulkanPad::AppendLineVertexList(const Type *pt)
{

	// ----------------------------------------------------------------------
	// Draw a thin hairline
	// ----------------------------------------------------------------------

	if (Topology(LINE)) {

		Vtx[vI].x = float(pt[0].x);
		Vtx[vI].y = float(pt[0].y);
		Vtx[vI].fnc = SKPSW_CENTER | SKPSW_FRAGMENT;
		Vtx[vI].l = 0.0f;
		Vtx[vI].clr = pencolor.dclr;
		Idx[iI++] = vI;
		vI++;

		Vtx[vI].x = float(pt[1].x);
		Vtx[vI].y = float(pt[1].y);
		Vtx[vI].px = float(pt[0].x);
		Vtx[vI].py = float(pt[0].y);
		Vtx[vI].fnc = SKPSW_CENTER | SKPSW_LENGTH | SKPSW_FRAGMENT;
		Vtx[vI].clr = pencolor.dclr;
		Idx[iI++] = vI;
		vI++;

		return;
	}


	// ----------------------------------------------------------------------
	// Wide line mode
	// ----------------------------------------------------------------------

	if (Topology(TRIANGLE)) {

		FVECTOR2 pp = _FV2Extrapolate(_FV2(pt[0]), _FV2(pt[1]));
		FVECTOR2 np;

		WORD vF = vI;

		for (int i = 0; i < 2; i++) {

			if (i == 0) np = _FV2(pt[1]);
			else np = _FV2Extrapolate(_FV2(pt[1]), _FV2(pt[0]));

			WORD vII = WORD(vI + 1);

			// --------------------------------------
			Vtx[vI].x = Vtx[vII].x = float(pt[i].x);
			Vtx[vI].y = Vtx[vII].y = float(pt[i].y);
			Vtx[vI].nx = Vtx[vII].nx = np.x;
			Vtx[vI].ny = Vtx[vII].ny = np.y;
			Vtx[vI].px = Vtx[vII].px = pp.x;
			Vtx[vI].py = Vtx[vII].py = pp.y;
			Vtx[vI].l = Vtx[vII].l = 0.0f;
			Vtx[vI].clr = Vtx[vII].clr = pencolor.dclr;
			// --------------------------------------
			Vtx[vI].fnc = SKPSW_WIDEPEN_L | SKPSW_FRAGMENT;
			if (i) Vtx[vI].fnc |= SKPSW_LENGTH;
			vI++;
			Vtx[vI].fnc = SKPSW_WIDEPEN_R | SKPSW_FRAGMENT;
			if (i) Vtx[vI].fnc |= SKPSW_LENGTH;
			vI++;
			// --------------------------------------

			pp = _FV2(pt[i]);
		}

		Idx[iI++] = WORD(vF + 0);
		Idx[iI++] = WORD(vF + 1);
		Idx[iI++] = WORD(vF + 2);
		Idx[iI++] = WORD(vF + 1);
		Idx[iI++] = WORD(vF + 3);
		Idx[iI++] = WORD(vF + 2);
	}
}


// ===============================================================================================
// The static members.
//
// eSketch and eDrawMesh are TECHHANDLE -- they select a pipeline set. The
// other twenty-eight name parameters and stay HANDLE. See VulkanEffect.h for
// why D3DXHANDLE could be both and these cannot.
//
TECHHANDLE   VulkanPad::eSketch = 0;
TECHHANDLE   VulkanPad::eDrawMesh = 0;
HANDLE       VulkanPad::eVP = 0;
HANDLE       VulkanPad::eW = 0;
HANDLE       VulkanPad::eKey = 0;
HANDLE       VulkanPad::ePen = 0;
HANDLE       VulkanPad::eWVP = 0;
HANDLE       VulkanPad::eFov = 0;
HANDLE       VulkanPad::eRandom = 0;
HANDLE       VulkanPad::eTarget = 0;
HANDLE       VulkanPad::eTexEn = 0;
HANDLE       VulkanPad::eFntEn = 0;
HANDLE       VulkanPad::eKeyEn = 0;
HANDLE       VulkanPad::eWidth = 0;
HANDLE       VulkanPad::eTex0 = 0;
HANDLE       VulkanPad::eFnt0 = 0;
HANDLE       VulkanPad::eDashEn = 0;
HANDLE       VulkanPad::eSize = 0;
HANDLE       VulkanPad::eWide = 0;
HANDLE       VulkanPad::eMtrl = 0;
HANDLE       VulkanPad::eShade = 0;
HANDLE       VulkanPad::ePos = 0;
HANDLE       VulkanPad::ePos2 = 0;
HANDLE       VulkanPad::eCov = 0;
HANDLE       VulkanPad::eCovEn = 0;
HANDLE       VulkanPad::eClearEn = 0;
HANDLE       VulkanPad::eEffectsEn = 0;

HANDLE	     VulkanPad::eNoiseTex = 0;
HANDLE       VulkanPad::eNoiseColor = 0;
HANDLE       VulkanPad::eColorMatrix = 0;
HANDLE       VulkanPad::eGamma = 0;

VulkanEffectFile* VulkanPad::FX = 0;
VulkanClient * VulkanPad::gc = 0;
WORD * VulkanPad::Idx = 0;
SkpVtx * VulkanPad::Vtx = 0;
// Five, not four, because the header declares five -- as the Windows header
// does. Only 0..3 are ever filled (SinCos is called four times) and only 0..3
// are freed in GlobalExit; the fifth has been unused since it was written.
// The extent is spelled out here rather than left empty, which is what the
// Windows definition does (`LPD3DXVECTOR2 D3D9Pad::pSinCos[];`) and which
// GCC rejects as a conflicting declaration.
FVECTOR2 * VulkanPad::pSinCos[5] = { 0, 0, 0, 0, 0 };
VulkanDevice * VulkanPadFont::pDev = 0;
VulkanDevice * VulkanPad::pDev = 0;
VulkanTexture * VulkanPad::pNoise = 0;

FILE* VulkanPad::log = 0;
CRITICAL_SECTION VulkanPad::LogCrit;

// `LPD3DXVECTOR2 D3D9Pad::pSinCos[];` stood above with no bound at all --
// legal only because the class declares the extent, and MSVC accepts it. It
// is spelled with its four elements here so the definition says what it is.
// std::map<MESHHANDLE, SketchMesh*> MeshMap is declared in the header and
// defined by whichever of D3D9Pad2/3 uses it; that is unchanged.


// ======================================================================
// class GDIFont
// ======================================================================
using namespace oapi;

VulkanPadFont::VulkanPadFont(int height, bool prop, const char *face, FontStyle style, int orientation, DWORD flags) : Font(height, prop, face, style, orientation)
{
	const char *def_fixedface = "Courier New";
	const char *def_sansface = "Arial";
	const char *def_serifface = "Times New Roman";

	if (face[0]!='*') {
		if (!_stricmp (face, "fixed")) face = def_fixedface;
		else if (!_stricmp (face, "sans")) face = def_sansface;
		else if (!_stricmp (face, "serif")) face = def_serifface;
		else if (_stricmp (face, def_fixedface) && _stricmp (face, def_sansface) && _stricmp (face, def_serifface)) face = (prop ? def_sansface : def_fixedface);
	}
	else face++;

	hFont = NULL;

	if (orientation!=0) rotation = float(orientation) * 0.1f;
	else                rotation = 0.0f;

	// Browse cache ---------------------------------------------------
	//

	for (size_t i = 0; i < fcache.size(); ++i) {
		if (fcache[i]->height!=height) continue;
		if (fcache[i]->style!=style) continue;
		if (fcache[i]->prop!=prop) continue;
		if (_stricmp(fcache[i]->face,face)!=0) continue;
		pFont = fcache[i]->pFont;
		break;
	}

	int weight = (style & FONT_BOLD) ? FW_BOLD : FW_NORMAL;
	DWORD italic = (style & FONT_ITALIC) ? TRUE : FALSE;
	DWORD underline = (style & FONT_UNDERLINE) ? TRUE : FALSE;
	DWORD strikeout = (style & FONT_STRIKEOUT) ? TRUE : FALSE;

	// The quality settings are carried over unchanged although the atlas
	// builder does not read them: NONANTIALIASED / PROOF / CLEARTYPE_QUALITY
	// are GDI's instructions to ITS rasteriser, and the rasteriser here is
	// stb_truetype, which antialiases unconditionally and has no ClearType.
	// They stay because GetQuality() is public and the value round-trips
	// through the LOGFONT, and because dropping them would silently change
	// what a caller reads back.
	Quality = NONANTIALIASED_QUALITY;

	if ((flags & 0xF) == 0) {
		if (Config->SketchpadFont == 1) Quality = PROOF_QUALITY;
		if (Config->SketchpadFont == 2) Quality = CLEARTYPE_QUALITY;
	}
	else {
		if (flags&SKP_FONT_ANTIALIAS) Quality = PROOF_QUALITY;
		if (flags&SKP_FONT_CLEARTYPE) Quality = CLEARTYPE_QUALITY;
	}

	// Create the accelerated font for a use with VulkanPad ------------------
	//
	if (pFont==NULL) {

		HFONT hNew = CreateFont(height, 0, 0, 0, weight, italic, underline, strikeout, 0, 0, 2, Quality, 49, face);

		pFont = std::make_shared<VulkanText>(pDev);
		pFont->Init(hNew);

		// VulkanText::Init consumes the handle -- DeleteObject(hFont) is the
		// last thing it does, exactly as D3D9Text::Init did. The
		// DeleteObject(hNew) that stood here would therefore be a SECOND
		// delete of the same object. It is harmless on Windows because
		// D3D9Text::Init's own DeleteObject already invalidated it and GDI
		// ignores a stale handle; it is removed rather than kept, because
		// "delete it twice and rely on the second one failing" is not a
		// contract worth carrying over.

		pFont->SetRotation(rotation);

		// Fill the cache --------------------------------
		FontCache *p = new FontCache();
		p->pFont  = pFont;
		p->height = height;
		p->style  = style;
		p->prop   = prop;
		strcpy_s(p->face, 64, face);
		fcache.push_back(p);
	}

	// Create Rotated GDI Font for a use with GDIPad ---------------------------
	//
	hFont = CreateFontA(height, 0, orientation, orientation, weight, italic, underline, strikeout, 0, 0, 2, Quality, 49, face);

	if (hFont==NULL) {
		face  = (prop ? def_sansface : def_fixedface);
		hFont = CreateFont(height, 0, orientation, orientation, weight, italic, underline, strikeout, 0, 0, 2, Quality, 49, face);
	}
}



VulkanPadFont::VulkanPadFont(int height, char *face, int width, int weight, FontStyle style, float spacing)
: Font(height, false, face, style, 0)
{

	hFont = NULL;
	rotation = 0.0f;

	// Browse cache ---------------------------------------------------
	//
	for (size_t i = 0; i < qcache.size(); ++i) {
		if (qcache[i]->height != height) continue;
		if (qcache[i]->style != style) continue;
		if (qcache[i]->width != width) continue;
		if (qcache[i]->weight != weight) continue;
		if (qcache[i]->spacing != spacing) continue;
		if (_stricmp(qcache[i]->face, face) != 0) continue;
		pFont = qcache[i]->pFont;
		break;
	}

	DWORD italic = (style & FONT_ITALIC) ? TRUE : FALSE;
	DWORD underline = (style & FONT_UNDERLINE) ? TRUE : FALSE;
	DWORD strikeout = (style & FONT_STRIKEOUT) ? TRUE : FALSE;

	Quality = NONANTIALIASED_QUALITY;
	if (Config->SketchpadFont == 1) Quality = ANTIALIASED_QUALITY;
	if (Config->SketchpadFont == 2) Quality = PROOF_QUALITY;

	if (style & FONT_CRISP) Quality = NONANTIALIASED_QUALITY;
	if (style & FONT_ANTIALIAS) Quality = ANTIALIASED_QUALITY;


	// Create the accelerated font for a use with VulkanPad ------------------
	//
	if (pFont == NULL) {

		hFont = CreateFont(height, width, 0, 0, weight, italic, underline, strikeout, 0, 0, 2, Quality, 49, face);

		pFont = std::make_shared<VulkanText>(pDev);
		pFont->Init(hFont);

		// AND HERE IS A REAL LEAK-AND-DANGLE IN THE WINDOWS SOURCE, kept
		// visible rather than silently fixed.
		//
		// This branch assigns hFont, then hands it to Init() -- which
		// DELETES it (D3D9Text::Init ends with DeleteObject(hFont), and
		// VulkanText::Init does the same). The member hFont is then a stale
		// handle for the life of the font object, and GetGDIFont() hands it
		// out. The OTHER branch, below, creates a SECOND font for exactly
		// this reason.
		//
		// So the second font is created here too. It costs one handle and it
		// makes GetGDIFont() return something valid in both branches, which
		// is what it already promises.
		hFont = CreateFont(height, width, 0, 0, weight, italic, underline, strikeout, 0, 0, 2, Quality, 49, face);

		pFont->SetRotation(0.0f);
		pFont->SetTextSpace(spacing);

		// Fill the cache --------------------------------
		QFontCache *p = new QFontCache();
		p->pFont = pFont;
		p->height = height;
		p->width = width;
		p->weight = weight;
		p->style = style;
		p->spacing = spacing;
		strcpy_s(p->face, 64, face);
		qcache.push_back(p);
	}
	else {
		// Create GDI Font for a use with GDIPad ---------------------------
		//
		hFont = CreateFont(height, width, 0, 0, weight, italic, underline, strikeout, 0, 0, 2, Quality, 49, face);
	}
}

// -----------------------------------------------------------------------------------------------
//
VulkanPadFont::~VulkanPadFont ()
{
	if (pFont) pFont->SetRotation(0.0f), pFont.reset();
	if (hFont) DeleteObject(hFont);
}


// -----------------------------------------------------------------------------------------------
//
HFONT VulkanPadFont::GetGDIFont () const
{
	return hFont;
}


// -----------------------------------------------------------------------------------------------
//
int VulkanPadFont::GetTextLength(const char *pText, int len) const
{
	return int(pFont->Length2(pText, len));
}


// -----------------------------------------------------------------------------------------------
//
int VulkanPadFont::GetIndexByPosition(const char *pText, int pos, int len) const
{
	return int(pFont->GetIndex(pText, float(pos), len));
}

// -----------------------------------------------------------------------------------------------
//
void VulkanPadFont::VulkanTechInit(VulkanDevice *pDevice)
{
	pDev = pDevice;
}



// ======================================================================
// class GDIPen
// ======================================================================

// The initialiser list reads `oapi::Pen(style, width, col)` and passes the
// class's OWN members -- both of them uninitialised at that point -- instead
// of the parameters s and w two lines below. It is in the Windows source and
// it is a genuine use of indeterminate values; the base class stores them and
// nothing in the tree reads them back, which is why it has never shown. The
// parameters are passed instead, which is plainly what was meant.
VulkanPadPen::VulkanPadPen (int s, int w, DWORD col): oapi::Pen (s, w, col)
{
	switch (s) {
		case 0:  style = PS_NULL;  break;
		case 2:  style = PS_DOT;   break;
		default: style = PS_SOLID; break;
	}
	width = w;
	if (width<1) width = 1;
	hPen = CreatePen(style, width, COLORREF(col&0xFFFFFF));
	clr = SkpColor(col);
}

// -----------------------------------------------------------------------------------------------
//
VulkanPadPen::~VulkanPadPen ()
{
	DeleteObject(hPen);
}

// -----------------------------------------------------------------------------------------------
//
void VulkanPadPen::VulkanTechInit(VulkanDevice *pDevice)
{
	//pDev = pDevice;
}



// ======================================================================
// class GDIBrush
// ======================================================================

VulkanPadBrush::VulkanPadBrush (DWORD col): oapi::Brush (col)
{
	hBrush = CreateSolidBrush(COLORREF(col&0xFFFFFF));
	clr = SkpColor(col);
}

// -----------------------------------------------------------------------------------------------
//
VulkanPadBrush::~VulkanPadBrush ()
{
	DeleteObject(hBrush);
}

// -----------------------------------------------------------------------------------------------
//
void VulkanPadBrush::VulkanTechInit(VulkanDevice *pDevice)
{
	//pDev = pDevice;
}
