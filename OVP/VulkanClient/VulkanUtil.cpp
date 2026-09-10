// ==============================================================
// Utilities
// Part of the ORBITER VISUALISATION PROJECT (OVP) Vulkan Client
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2006-2026 Martin Schweiger
//				 2012-2016 Jarmo Nikkanen
//				 2012-2016 Emile "Bibi Uncle" Gregoire
// ==============================================================
//
// CONVERTED FROM OVP/D3D9Client/D3D9Util.cpp, read end to end (2102 lines).
//
// About 85% of this file is maths, strings, materials, lights and matrix
// algebra that needed only the type mapping. The changes worth naming:
//
//  1. THE ELEVEN VERTEX DECLARATIONS ARE DEFINED HERE. On Windows they were
//     IDirect3DVertexDeclaration9* globals in D3D9Frame.cpp, built by
//     pDevice->CreateVertexDeclaration(). A Vulkan vertex layout is not a
//     device object -- it is data compiled into a VkPipeline -- so there is
//     nothing to create, nothing to release, and no device to tie it to.
//     They are constants, and they live next to the attribute tables and the
//     vertex structs that give them their strides.
//
//  2. CopyBuffer LOST ITS TYPE SWITCH. It took two LPDIRECT3DRESOURCE9 and
//     branched on GetType() because a vertex buffer and an index buffer were
//     different interfaces. Vulkan has one VkBuffer, told apart only by the
//     usage flags given at creation, so the two identical branches become one.
//
//  3. D3DXVec3* BECAME THE SDK's OWN. D3DXVec3Normalize, D3DXVec3Dot,
//     D3DXVec3Cross, D3DXVec3Length, D3DXVec3TransformCoord and
//     D3DXVec3TransformNormal all have exact counterparts in oapi::
//     (normalize, dot, cross, length, TransformCoord, TransformNormal), so
//     the D3DX dependency goes without a line of new maths.
//
//  4. THE MATRIX HELPERS KEEP THEIR ARITHMETIC EXACTLY. Every _11.._44
//     became m11..m44, which names the same float in the same slot -- the
//     SDK comment on FMATRIX4 says it is layout-compatible with D3DXMATRIX.
//     Nothing was reordered, transposed or "corrected".
//
//  5. ZeroMemory(mat, sizeof) BECAME mat->Zero(). FMATRIX4 has user-declared
//     constructors, so memset over it is formally undefined and GCC reports
//     -Wclass-memaccess; FMATRIX4::Zero() writes the same sixteen zeroes by a
//     defined route. Same for D3DMAT_Copy -> a plain assignment.
//
//  6. startsWith() IS NOW A PREFIX TEST. The Windows body is byte-identical
//     to contains() -- std::search over the whole haystack, returning
//     it != cend(). Its one call site (VPlanet.cpp, startsWith(line,
//     "AlbedoRGB") while parsing config keys) wants what the name promises.
//
//  7. SurfaceLighting() IS DEAD CODE and is converted anyway. It is defined
//     here, declared in no header and called from nowhere in the tree.
//     Carried across for fidelity rather than quietly dropped.
//
// The device-touching half -- CopyBuffer, CreateVolumeTexture,
// LoadPlanetTextures, SketchMesh and ShaderClass -- is where the real
// rewriting is, and each carries its own note at the point of change.
// ==============================================================

#define STRICT

#include "VulkanUtil.h"
#include "AABBUtil.h"
#include "VulkanClient.h"
#include "VectorHelpers.h"
#include "VulkanConfig.h"
#include "VulkanFrame.h"
#include "VulkanSurface.h"
#include "VObject.h"
#include "Mesh.h"        // SPEC_INHERIT / SPEC_DEFAULT, as on Windows
#include <functional>
#include <cctype>
#include <unordered_map>
#include <algorithm>
#include <limits>
#include <sys/stat.h>
#include <inttypes.h>   // PRIXPTR, for _PTR() below

using namespace oapi;

extern VulkanClient* g_client;
extern std::unordered_map<MESHHANDLE, class SketchMesh*> MeshMap;


// ==============================================================================================
// The vertex declarations.
//
// One VertexDecl per attribute table in VulkanUtil.h, each naming the vertex
// struct whose size is its stride, so the offsets in the table and the struct
// they describe can never drift apart silently. On Windows these were device
// objects created in D3D9Frame.cpp; see the file header.
// ==============================================================================================

// The stride is spelled sizeof(struct) wherever VulkanUtil.h can see the
// struct, so the two can never drift. Two cannot: the haze vertex lives in
// HazeMgr.h and the sketchpad vertex in D3D9Pad.h, neither converted yet, so
// those carry the literal byte size their attribute offsets already imply
// (12+4+8 = 24, and 12+16+4+4 = 36). A static_assert against sizeof() goes in
// beside each struct when those two files are converted.
#define VDECL(name, stride) \
	static const VertexDecl s_##name(#name, stride, name, ARRAYSIZE(name)); \
	const VertexDecl *p##name = &s_##name;

VDECL(NTVertexDecl,     sizeof(NTVERTEX))
VDECL(BAVertexDecl,     sizeof(BAVERTEX))
VDECL(PosColorDecl,     sizeof(VERTEX_XYZC))
VDECL(PositionDecl,     sizeof(VERTEX_XYZ))
VDECL(Vector4Decl,      sizeof(FVECTOR4))
VDECL(PosTexDecl,       sizeof(SMVERTEX))
VDECL(HazeVertexDecl,   24)								// HazeMgr.h, unconverted
VDECL(MeshVertexDecl,   sizeof(NMVERTEX))
VDECL(PatchVertexDecl,  sizeof(VERTEX_2TEX))
VDECL(SketchpadDecl,    36)								// D3D9Pad.h, unconverted
VDECL(LocalLightsDecl,  sizeof(LocalLightsCompute))

#undef VDECL


DWORD BuildDate()
{
	const char *months[] = { "???","Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
	char month[8];
	unsigned int day = 0, year = 0;
	// PLAIN sscanf, AND THE SIZE ARGUMENT IS GONE. The Windows line is
	//     sscanf_s(__DATE__, "%s %u %u", month, 8, &day, &year)
	// where the 8 is MSVC's buffer size for %s. Src/Orbiter/Linux/windows.h
	// defines sscanf_s as sscanf, and its own comment at :1829 names THIS
	// FUNCTION as the reason the rule exists: a size argument is only harmless
	// when every %s is the last conversion. Here two %u follow it, so the
	// first would write an unsigned int through the ADDRESS 8 -- a wild store
	// at startup, not a no-op. -Wformat is what caught it.
	assert(sscanf(__DATE__, "%7s %u %u", month, &day, &year) == 3);
	DWORD m = 0;
	for (DWORD i = 1; i <= 12; i++) if (strncmp(month, months[i], 3) == 0) { m = i; break; }
	assert(m != 0);
	return (year % 100) * 10000 + m * 100 + day;
}

WORD crc16(const char *data, int length)
{
	DWORD crc = 0;
	for (int i = 0; i < length; ++i) {
		crc = crc ^ (DWORD(data[i]) << 8);
		for (int j = 0; j < 8; j++) {
			if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
			else crc = (crc << 1);
			crc &= 0xFFFF;
		}
	}
	return WORD(crc & 0xFFFF);	
}

// ===========================================================================================
// Sun occlusion by planet hObj for a given global position gpos
//
float SunOcclusionByPlanet(OBJHANDLE hObj, VECTOR3 gpos)
{
	VECTOR3 gsun, gpln;
	OBJHANDLE hSun = oapiGetObjectByIndex(0);
	
	oapiGetGlobalPos(hSun, &gsun);
	oapiGetGlobalPos(hObj, &gpln);

	VECTOR3 rpos = gpln - gpos;
	VECTOR3 spos = gsun - gpos;	
	double	sd = length(spos);				
	double  sz = oapiGetSize(hObj);
	VECTOR3 usd = spos / sd;					
	VECTOR3 up = unit(rpos);
	double r  = length(rpos);
	double ca = -dot(up, usd);
	double qr = sqrt(saturate(1.0 - ca * ca)) * r;
	double dp = r * r - sz * sz;
	double hd = dp > 1e4 ? sqrt(dp) : 1000.0; // Distance to horizon
	double sr = oapiGetSize(hSun) * fabs(hd) / sd;
	// How much of the sun's "disc" is shadowed by planet (APPROXIMATION)
	double svb = ca > 0.0 ? 1.0 : ilerp(sz - sr * 0.33, sz + sr, qr); 
	return svb;
}

// Check if object 'body' is casting shadows on 'ref' ---------------------
//
bool IsCastingShadows(vObject* body, vObject* ref, double* sunsize_out)
{
	double sz = oapiGetSize(oapiGetGbodyByIndex(0));
	VECTOR3 bc = body->GlobalPos() - ref->GlobalPos();
	double x = dot(bc, ref->SunDirection());			// Distance to projection plane
	double s = fabs(x) * sz / ref->SunDistance();		// Size of the sun at projection plane
	double refrad = body->GetSize() + ref->GetSize() + s;
	if (sunsize_out) *sunsize_out = s;

	if (x < 0) return false; // 'body' is behind 'ref'
	if (sqrt(dotp(bc, bc) - x * x) < refrad) return true;
	return false;
}


double Distance(vObject* a, vObject* b)
{
	return length(a->GlobalPos() - b->GlobalPos());
}


float OcclusionFactor(float x, float sunrad, float plnrad)
{
	bool bReverse = sunrad > plnrad;
	return OcclusionFactor(x, sunrad, plnrad, bReverse);
}


// =================================================================================================================================
// Occlusion area of two circles, 1.0f = zero occlusion, 0.0f = full occlusion of smaller circle by bigger one  
// if bReverse then occlusion of bigger by smaller one
//
float OcclusionFactor(float x, float r1, float r2, bool bReverse)
{
	if (x > (r1 + r2)) return 1.0f;

	float rmax = std::max(r1, r2);
	float rmin = std::min(r1, r2);

	float a2 = rmin * rmin;
	float b2 = rmax * rmax;

	if (x < (rmax - rmin)) {
		if (bReverse) return 1.0f - a2 / b2;
		return 0.0f;
	}

	bool bInv = x < sqrt(b2 - a2);

	float s = (r1 + r2 + x) * 0.5f;
	float A = sqrt(s * (s - r1) * (s - r2) * (s - x)); //Heron's area formula
	float h = 2.0f * A / x;

	float s1 = asin(saturate(h / rmin)); // Sector 1
	float s2 = asin(saturate(h / rmax)); // Sector 2

	if (bInv) s1 = float(PI) - s1;

	s1 *= a2;
	s2 *= b2;

	float h2 = h * h;
	float t1 = h * sqrt(std::max(0.0f, a2 - h2)); // Triangle 1
	float t2 = h * sqrt(std::max(0.0f, b2 - h2)); // Triangle 2

	if (bInv) t1 = -t1;

	float area = (s1 - t1) + (s2 - t2);

	return 1.0f - area / (float(PI) * (bReverse ? b2 : a2));
}


// Was #if _WIN64 / #else, choosing "0x%llX" or "0x%lX" for a LONG_PTR. On
// LP64 Linux a pointer is 64 bits and intptr_t is long, so the 64-bit form is
// the only one; PRIxPTR from <inttypes.h> says that without a preprocessor
// branch at all.
const char *_PTR(const void *p)
{
	static long i = 0; static char buf[8][32];	i++;
	sprintf_s(buf[i & 0x7], 32, "0x%" PRIXPTR, (uintptr_t)p);
	return buf[i & 0x7];
}


// ===============================================================================================
// Was CopyBuffer(LPDIRECT3DRESOURCE9, LPDIRECT3DRESOURCE9) with two identical
// branches, one for D3DRTYPE_VERTEXBUFFER and one for D3DRTYPE_INDEXBUFFER.
// Vulkan has one buffer type -- a vertex buffer and an index buffer differ
// only in the usage flags given at creation -- so the switch has nothing to
// switch on and the two branches collapse into the one below.
//
// The index-buffer branch also compared src_desc.Format against dst_desc.Format
// (D3DFMT_INDEX16 vs INDEX32). That check has no counterpart either: the index
// type is not a property of a Vulkan buffer, it is an argument to
// vkCmdBindIndexBuffer.
//
bool CopyBuffer(VulkanBuffer *pDst, VulkanBuffer *pSrc)
{
	if (!pSrc || !pDst) return false;
	if (pDst->Size() < pSrc->Size()) return false;

	void *pSrcData = pSrc->Map();
	void *pDstData = pDst->Map();

	if (!pSrcData || !pDstData) {
		if (pSrcData) pSrc->Unmap();
		if (pDstData) pDst->Unmap();
		LogErr("CopyBuffer: buffer memory is not host visible");
		return false;
	}

	memcpy(pDstData, pSrcData, size_t(pSrc->Size()));

	pSrc->Unmap();
	pDst->Unmap();

	return true;
}

void LogMatrix(FMATRIX4 *pM, const char *name)
{
	LogAlw("%s", name);
	LogAlw("[%9.9g, %9.9g, %9.9g, %9.9g]", pM->m11, pM->m12, pM->m13, pM->m14);
	LogAlw("[%9.9g, %9.9g, %9.9g, %9.9g]", pM->m21, pM->m22, pM->m23, pM->m24);
	LogAlw("[%9.9g, %9.9g, %9.9g, %9.9g]", pM->m31, pM->m32, pM->m33, pM->m34);
	LogAlw("[%9.9g, %9.9g, %9.9g, %9.9g]", pM->m41, pM->m42, pM->m43, pM->m44);
}

inline FVECTOR4 CV2VEC4(const COLOUR4 &in)
{
	return FVECTOR4(in.r, in.g, in.b, in.a);
}

inline FVECTOR4 CV2VEC4(const COLOUR4 &in, float w)
{
	return FVECTOR4(in.r, in.g, in.b, w);
}

inline FVECTOR3 CV2VEC3(const COLOUR4 &in)
{
	return FVECTOR3(in.r, in.g, in.b);
}

inline COLOUR4 VECtoCV(const FVECTOR3 &in, float w)
{
	COLOUR4 c = { in.x, in.y, in.z, w };
	return c;
}

inline COLOUR4 VECtoCV(const FVECTOR4 &in)
{
	COLOUR4 c = { in.x, in.y, in.z, in.w };
	return c;
}

void UpdateMatExt(const MATERIAL *pIn, VulkanMatExt *pOut)
{
	pOut->Ambient = CV2VEC3(pIn->ambient);
	pOut->Diffuse = CV2VEC4(pIn->diffuse);
	pOut->Emissive = CV2VEC3(pIn->emissive);
	pOut->Specular = CV2VEC4(pIn->specular, pIn->power);
}

void GetMatExt(const VulkanMatExt *pIn, MATERIAL *pOut)
{
	pOut->ambient = VECtoCV(pIn->Ambient, 0);
	pOut->diffuse = VECtoCV(pIn->Diffuse);
	pOut->emissive = VECtoCV(pIn->Emissive, 0);
	pOut->specular = VECtoCV(pIn->Specular);
	pOut->specular.a = 0.0f;
	pOut->power	= pIn->Specular.w;
}

void CreateMatExt(const MATERIAL *pIn, VulkanMatExt *pOut)
{
	pOut->Ambient = CV2VEC3(pIn->ambient);
	pOut->Diffuse = CV2VEC4(pIn->diffuse);
	pOut->Emissive = CV2VEC3(pIn->emissive);
	pOut->Specular = CV2VEC4(pIn->specular, pIn->power);
	pOut->Reflect = FVECTOR3(0, 0, 0);
	pOut->Fresnel = FVECTOR3(1, 0, 1024.0f);
	pOut->Emission2 = FVECTOR3(1, 1, 1);
	pOut->Roughness = FVECTOR2(1.0f, 1.0f);
	pOut->SpecialFX = FVECTOR4(0, 0, 0, 0);
	pOut->Metalness = 0.0f;
	pOut->ModFlags = 0;
}

void CreateDefaultMat(VulkanMatExt *pOut)
{
	pOut->Ambient = FVECTOR3(0, 0, 0);
	pOut->Diffuse = FVECTOR4(1, 1, 1, 1);
	pOut->Emissive = FVECTOR3(0, 0, 0);
	pOut->Specular = FVECTOR4(0.2f, 0.2f, 0.2f, 50.0f);
	pOut->Reflect = FVECTOR3(0, 0, 0);
	pOut->Fresnel = FVECTOR3(1, 0, 1024.0f);
	pOut->Emission2 = FVECTOR3(1, 1, 1);
	pOut->Roughness = FVECTOR2(1.0f, 1.0f);
	pOut->SpecialFX = FVECTOR4(0, 0, 0, 0);
	pOut->Metalness = 0.0f;
	pOut->ModFlags = 0;
}

void VulkanTuneInit(VulkanTune *pTune)
{
	COLOUR4 white = { 1, 1, 1, 1 };
	pTune->Albedo = white;
	pTune->Emis = white;
	pTune->Spec = white;
	pTune->Refl = white;
	pTune->Transl = white;
	pTune->Transm = white;
	pTune->Norm = white;
	pTune->Rghn = white;
}

// ===========================================================================================
// DEAD CODE ON BOTH PLATFORMS. Defined here, declared in no header, called
// from nowhere in the tree. Converted for fidelity; see the file header.
//
// The one behavioural note: the Windows body assigns a D3DXCOLOR (four floats)
// to light->Color and light->Ambient, which are FVECTOR3. The alpha it builds
// was discarded by that assignment, so the FVECTOR3 constructions below are
// what the code already did, spelled so it is visible.
//
void SurfaceLighting(VulkanSun *light, OBJHANDLE hP, OBJHANDLE hO, float ao)
{
	// hP=hPlanet, hS=hSun
	VECTOR3 GO, GS, GP;

	FVECTOR3 _one(1,1,1);

	OBJHANDLE hS = oapiGetGbodyByIndex(0);	// the central star
	oapiGetGlobalPos (hO, &GO);				// object position
	oapiGetGlobalPos (hS, &GS);				// sun position
	oapiGetGlobalPos (hP, &GP);				// planet position

	VECTOR3 S = GS-GO;							// sun's position from base
	VECTOR3 P = unit(GO-GP);

	float s  = float(length(S));				// sun's distance
	float rs = float(oapiGetSize(hS)) / s;
	float h  = float(dotp(S,P)) / s;			// sun elevation
	float d  = 0.173f;							// sun elevation for dispersion
	float ae = 0.242f;							// sun elevation for ambient
	float aq = 0.342f;

	float amb0 = 0.0f;
	float disp = 0.0f;
	float amb  = 0.0f;

	const ATMCONST *atm = (oapiGetObjectType(hP)==OBJTP_PLANET ? oapiGetPlanetAtmConstants (hP) : NULL);

	if (atm) {
		amb0 = float(std::min (0.7, log1p(atm->rho0)*0.4));
		disp = float(std::max (0.02, std::min(0.9, log1p(atm->rho0))));
	}

	FVECTOR3 lcol;
	FVECTOR3 r0 = _one - FVECTOR3(0.65f, 0.75f, 1.0f) * disp;

	if (atm) { // case 1: planet has atmosphere
		lcol = (r0 + (_one-r0) * saturate(h/d)) * saturate((h+rs)/(2.0f*rs));
		amb  = saturate((h+ae)/aq);
		amb  = saturate(std::max(amb0*amb-0.05f,ao));
		lcol *= 1.0f-amb*0.5f; // reduce direct light component to avoid overexposure
	}
	else {   // case 2: planet has no atmosphere
		lcol = r0 * saturate((h+rs)/(2.0f*rs));
		amb  = ao;
		lcol *= 1.0f-amb*0.5f; // reduce direct light component to avoid overexposure
	}

	light->Color = FVECTOR3(lcol.x, lcol.y, lcol.z);
	light->Ambient = FVECTOR3(amb, amb, amb);
	light->Dir = FVEC(S) * (-1.0f/s);
}


void strremchr(char *str, int idx)
{
	while (str[idx]!='\0') {
		str[idx] = str[idx+1];
		idx++;
	}
}

// --------------------------------------------------------------
// Improved version of fgets
// Copyright (C) 2012-2026 Jarmo Nikkanen
// Return:
// -1 = eof
//  0 = invalid string
//  1 = success without '=' in string
//  2 = success with '=' in string
//
// param:
//  0x01 = Don't Remove spaces from both sides of '='
//  0x02 = Don't convert '/' to '\'
//  0x04 = Convert to upper case
//  0x08 = Remove '=' if exists

int fgets2(char *buf, int cmax, FILE *file, DWORD param)
{
	bool bEql = false;
	bool bEquality = (param&0x01)==0;
	bool bSlash = (param&0x02)==0;
	bool bEqlRem = (param&0x08)!=0;
	bool bUpper = (param&0x04)!=0;

	if (fgets(buf, cmax, file)==NULL) return -1;

	int num = lstrlen(buf);

	if (num==(cmax-1)) LogErr("Insufficient buffer size in fgets2() size=%d, string=(%s)",cmax,buf);

	// Replace tabs with spaces and cut a comment parts and unwanted chars
	// Check the existance of equality sign '='
	for (int i=0;i<num;i++) {
		char c = buf[i];
		if (c=='=') bEql=true;
		if (c=='=' && bEqlRem) buf[i]=' ';
		if (c=='\t') buf[i]=' ';
		if (c=='/' && bSlash) buf[i]='\\';
		if (c==';' || c==0xA || c==0xD) {
			buf[i]='\0';
			break;
		}
	}

	num = lstrlen(buf);
	if (num==0) return 0;

	// Remove spaces from the end of the line
	while (num>0) {
		num--;
		if (buf[num]==' ') buf[num]='\0';
		else break;
	}

	// Remove spaces from the front of the line
	while (buf[0]==' ') strremchr(buf,0);

	num = lstrlen(buf);
	if (num==0) return 0;

	// Remove repeatitive spaces if exists. (double trible spaces and so on)
	// At this point a space can not be the last char, therefore [i+1] is not a problem
	for (int i=0;i<num;) {
		if (buf[i]==' ' && buf[i+1]==' ') {
			strremchr(buf, i);
			num--;
		}
		else i++;
	}

	num = lstrlen(buf);
	if (num==0) return 0;

	// Remove spaces from both sides of '=' if exists
	if (bEql && bEquality) {
		if (buf[0]=='=' || buf[num-1]=='=') return 0;
		for (int i=0;i<num;i++) if (buf[i]=='=') {
			if (buf[i+1]==' ') strremchr(buf,i+1);
			if (buf[i-1]==' ') strremchr(buf,i-1);
			break;
		}
	}

	// _strupr_s is an MSVC CRT extension with no POSIX counterpart and none in
	// the shim. The loop is what it does.
	if (bUpper) for (char *p = buf; *p; p++) *p = (char)toupper((unsigned char)*p);

	// Done
	if (bEql) return 2;
	return 1;
}

// -----------------------------------------------------------------------------------
// String helper
// ------------------------------------------------------------------------------------

// trim from start
std::string &ltrim (std::string &s)
{
	s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](auto c) { return !std::isspace(c); }));
	return s;
}

// trim from end
std::string &rtrim (std::string &s)
{
	s.erase(std::find_if(s.rbegin(), s.rend(), [](auto c) { return !std::isspace(c); }).base(), s.end());
	return s;
}

// trim from both ends
std::string &trim (std::string &s) {
	return ltrim(rtrim(s));
}

// uppercase complete string
//
// std::toupper is overloaded -- <cctype>'s int(int) and <locale>'s
// template<charT>(charT, const locale&) -- and std::transform cannot pick
// between them from the name alone. MSVC resolves it; GCC reports "no
// matching function". The cast names the C one, which is what was meant.
void toUpper (std::string &s) {
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::toupper(c); });
}

// string to double (returns quiet_NaN if conversion failed)
double toDoubleOrNaN (const std::string &str)
{
	return (str[0] == 'N' || str[0] == 'n') // "NaN" or "nan"?
		? std::numeric_limits<double>::quiet_NaN()
		: atof(str.c_str());
}

// case insensitive compare
//
// THE WINDOWS BODY IS contains(). It is byte-identical to the function below
// it: std::search across the whole haystack, returning it != cend(), which
// answers "does the needle appear anywhere" and not "does the haystack start
// with it". Its one call site is VPlanet.cpp, startsWith(line, "AlbedoRGB"),
// parsing config keys off the front of a line -- so a comment mentioning
// AlbedoRGB further along the line would have matched.
//
// Fixed to what the name and the call site both mean.
bool startsWith (const std::string &haystack, const std::string &needle)
{
	if (needle.size() > haystack.size()) return false;
	return std::equal(needle.cbegin(), needle.cend(), haystack.cbegin(),
		[](char a, char b) { return std::toupper((unsigned char)a) == std::toupper((unsigned char)b); });
}

// case insensitive conatins
bool contains (const std::string &haystack, const std::string &needle)
{
	auto it = std::search(
		haystack.cbegin(), haystack.cend(), needle.cbegin(), needle.cend(),
		[](char a, char b) { return std::toupper((unsigned char)a) == std::toupper((unsigned char)b); }
	);
	return it != haystack.cend();
}

// case insensitive find
size_t find_ci (const std::string &haystack, const std::string &needle)
{
	auto it = std::search(
		haystack.cbegin(), haystack.cend(), needle.cbegin(), needle.cend(),
		[](char a, char b) { return std::toupper((unsigned char)a) == std::toupper((unsigned char)b); }
	);
	return it != haystack.cend()
		? static_cast<size_t>(it - haystack.cbegin())
		: std::string::npos;
}

// case insensitive rfind
size_t rfind_ci (const std::string &haystack, const std::string &needle)
{
	auto it = std::search(
		haystack.rbegin(), haystack.rend(), needle.rbegin(), needle.rend(),
		[](char a, char b) { return std::toupper((unsigned char)a) == std::toupper((unsigned char)b); }
	);
	return it != haystack.rend()
		? static_cast<size_t>(haystack.rend() - it)
		: std::string::npos;
}

// parse assignments like "foo=bar", "foo = bar" or even "foo= bar ; with comment"
std::pair<std::string, std::string> &splitAssignment (const std::string &line, const char delim /* = '=' */)
{
	static std::pair<std::string, std::string> ret;

	const char comment = ';';
	size_t delPos = line.find(delim),  // delimiter position
		cmtPos = line.find(comment);// comment pos...

									// ...convert to 'comment part length' if comment found
	cmtPos -= cmtPos != std::string::npos ? delPos + 1 : 0;

	ret.first = line.substr(0, delPos);
	trim(ret.first);
	ret.second = line.substr(delPos + 1, cmtPos);
	trim(ret.second);

	return ret;
}

// replace all occurances of 's' in 'subj' by 't'
std::string::size_type replace_all (std::string &subj, const std::string &s, const std::string &t)
{
	std::string::size_type n = 0, c = 0;
	while ((n = subj.find(s, n)) != std::string::npos) {
		subj.replace(n, s.size(), t);
		n += t.size();
		++c;
	}
	return c;
}

// =======================================================================
// Some utility methods for vectors and matrices
//
// D3DXVec3Normalize, D3DXVec3Dot, D3DXVec3Cross and D3DXVec3Length all have
// exact counterparts in namespace oapi, so these lose the D3DX dependency
// without gaining a line of new arithmetic.
// ============================================================================

float Vec3Angle(FVECTOR3 a, FVECTOR3 b)
{
	a = oapi::normalize(a);
	b = oapi::normalize(b);
	float x = oapi::dot(a, b);
	if (x<-1.0f) x=-1.0f;
	if (x> 1.0f) x= 1.0f;
	return acos(x);
}

// ============================================================================
//
FVECTOR3 Perpendicular(FVECTOR3 *a)
{
	float x = fabs(a->x);
	float y = fabs(a->y);
	float z = fabs(a->z);
	float m = std::min(std::min(x, y), z);
	if (m==x) return FVECTOR3(0, a->z,  a->y);
	if (m==y) return FVECTOR3(a->z, 0, -a->x);
	else      return FVECTOR3(a->y, -a->x, 0);
}

// Cleate a billboarding matrix. X-axis of the vertex data will be pointing to the camera
//
void VMAT_CreateX_Billboard(const FVECTOR3 *toCam, const FVECTOR3 *pos, float size, FMATRIX4 *pOut)
{
	float hz  = 1.0f/sqrt(toCam->x*toCam->x + toCam->z*toCam->z);

	pOut->m11 =  toCam->x;
	pOut->m12 =  toCam->y;
	pOut->m13 =  toCam->z;
	pOut->m31 = -toCam->z*hz;
	pOut->m32 =  0.0f;
	pOut->m33 =  toCam->x*hz;
	pOut->m21 = -pOut->m12*pOut->m33;
	pOut->m22 =  pOut->m33*pOut->m11 - pOut->m13*pOut->m31;
	pOut->m23 =  pOut->m31*pOut->m12;
	pOut->m41 =  pos->x;
	pOut->m42 =  pos->y;
	pOut->m43 =  pos->z;
	pOut->m14 = pOut->m24 = pOut->m34 = pOut->m44 = 0.0f;
	pOut->m11 *= size; pOut->m12 *= size; pOut->m13 *= size;
	pOut->m21 *= size; pOut->m22 *= size; pOut->m23 *= size;
	pOut->m31 *= size;					  pOut->m33 *= size;
}


// Cleate a billboarding matrix. X-axis of the vertex data will be pointing to the camera
//
void VMAT_CreateX_Billboard(const FVECTOR3 *toCam, const FVECTOR3 *pos, const FVECTOR3 *dir, float size, float stretch, FMATRIX4 *pOut)
{
	FVECTOR3 q = oapi::normalize(oapi::cross(*dir, *toCam));
	FVECTOR3 w = oapi::normalize(oapi::cross(q, *dir));

	pOut->m11 = w.x * size;
	pOut->m12 = w.y * size;
	pOut->m13 = w.z * size;

	pOut->m21 = q.x * size;
	pOut->m22 = q.y * size;
	pOut->m23 = q.z * size;

	pOut->m31 = dir->x * stretch;
	pOut->m32 = dir->y * stretch;
	pOut->m33 = dir->z * stretch;

	pOut->m41 = pos->x;
	pOut->m42 = pos->y;
	pOut->m43 = pos->z;

	pOut->m14 = pOut->m24 = pOut->m34 = pOut->m44 = 0.0f;
}

// ============================================================================
//
// ZeroMemory(mat, sizeof(D3DXMATRIX)) on Windows. FMATRIX4 has user-declared
// constructors, so memset over it is formally undefined (-Wclass-memaccess);
// FMATRIX4::Zero() writes the same sixteen zeroes by a defined route.
void VMAT_ZeroMatrix(FMATRIX4 *mat)
{
	mat->Zero();
}

// ============================================================================
// Matrix identity

void VMAT_Identity (FMATRIX4 *mat)
{
	mat->Ident();
}

// ============================================================================
// Copy a FMATRIX4  (was memcpy)

void VMAT_Copy (FMATRIX4 *tgt, const FMATRIX4 *src)
{
	 *tgt = *src;
}

// ============================================================================
//
void VMAT_FromAxis(FMATRIX4 *mat, const FVECTOR3 *x, const FVECTOR3 *y, const FVECTOR3 *z)
{
	mat->m11 = x->x;
	mat->m21 = x->y;
	mat->m31 = x->z;

	mat->m12 = y->x;
	mat->m22 = y->y;
	mat->m32 = y->z;

	mat->m13 = z->x;
	mat->m23 = z->y;
	mat->m33 = z->z;
}

// ============================================================================
//
void VMAT_FromAxis(FMATRIX4 *mat, const VECTOR3 *x, const VECTOR3 *y, const VECTOR3 *z)
{
	mat->m11 = float(x->x);
	mat->m21 = float(x->y);
	mat->m31 = float(x->z);

	mat->m12 = float(y->x);
	mat->m22 = float(y->y);
	mat->m32 = float(y->z);

	mat->m13 = float(z->x);
	mat->m23 = float(z->y);
	mat->m33 = float(z->z);
}

// ============================================================================
//
void VMAT_FromAxisT(FMATRIX4 *mat, const FVECTOR3 *x, const FVECTOR3 *y, const FVECTOR3 *z)
{
	mat->m11 = x->x;
	mat->m12 = x->y;
	mat->m13 = x->z;

	mat->m21 = y->x;
	mat->m22 = y->y;
	mat->m23 = y->z;

	mat->m31 = z->x;
	mat->m32 = z->y;
	mat->m33 = z->z;
}

// ============================================================================
// Copy a rotation matrix into a FMATRIX4

void VMAT_SetRotation (FMATRIX4 *mat, const MATRIX3 *rot)
{
	mat->m11 = (float)rot->m11;
	mat->m12 = (float)rot->m12;
	mat->m13 = (float)rot->m13;
	mat->m21 = (float)rot->m21;
	mat->m22 = (float)rot->m22;
	mat->m23 = (float)rot->m23;
	mat->m31 = (float)rot->m31;
	mat->m32 = (float)rot->m32;
	mat->m33 = (float)rot->m33;
}

// ============================================================================
// Copy the transpose of a matrix as rotation of a transformation matrix

void VMAT_SetInvRotation (FMATRIX4 *mat, const MATRIX3 *rot)
{
	mat->m11 = (float)rot->m11;
	mat->m12 = (float)rot->m21;
	mat->m13 = (float)rot->m31;
	mat->m21 = (float)rot->m12;
	mat->m22 = (float)rot->m22;
	mat->m23 = (float)rot->m32;
	mat->m31 = (float)rot->m13;
	mat->m32 = (float)rot->m23;
	mat->m33 = (float)rot->m33;
}

// ============================================================================
// Define a rotation matrix from a rotation axis & rotation angle

void VMAT_RotationFromAxis (const FVECTOR3 &axis, float angle, FMATRIX4 *rot)
{
	// Calculate quaternion
	angle *= 0.5f;
	float w = cosf(angle), sina = sinf(angle);
	float x = sina * axis.x;
	float y = sina * axis.y;
	float z = sina * axis.z;

	// Rotation matrix
	float xx = x*x, yy = y*y, zz = z*z;
	float xy = x*y, xz = x*z, yz = y*z;
	float wx = w*x, wy = w*y, wz = w*z;

	rot->m11 = 1 - 2 * (yy+zz);
	rot->m12 =     2 * (xy+wz);
	rot->m13 =     2 * (xz-wy);
	rot->m21 =     2 * (xy-wz);
	rot->m22 = 1 - 2 * (xx+zz);
	rot->m23 =     2 * (yz+wx);
	rot->m31 =     2 * (xz+wy);
	rot->m32 =     2 * (yz-wx);
	rot->m33 = 1 - 2 * (xx+yy);

	rot->m14 = rot->m24 = rot->m34 = rot->m41 = rot->m42 = rot->m43 = 0.0f;
	rot->m44 = 1.0f;
}

// ============================================================================
// Set up a as matrix for ANTICLOCKWISE rotation r around x/y/z-axis

void VMAT_RotX  (FMATRIX4 *mat, double r)
{
	double sinr = sin(r), cosr = cos(r);
	mat->Zero();
	mat->m22 = mat->m33 = (float)cosr;
	mat->m23 = -(mat->m32 = (float)sinr);
	mat->m11 = mat->m44 = 1.0f;
}

// ============================================================================
//
void VMAT_RotY (FMATRIX4 *mat, double r)
{
	double sinr = sin(r), cosr = cos(r);
	mat->Zero();
	mat->m11 = mat->m33 = (float)cosr;
	mat->m31 = -(mat->m13 = (float)sinr);
	mat->m22 = mat->m44 = 1.0f;
}

// ============================================================================
//
float VMAT_BSScaleFactor(const FMATRIX4 *mat)
{
	float lx = mat->m11*mat->m11 + mat->m12*mat->m12 + mat->m13*mat->m13;
    float ly = mat->m21*mat->m21 + mat->m22*mat->m22 + mat->m23*mat->m23;
    float lz = mat->m31*mat->m31 + mat->m32*mat->m32 + mat->m33*mat->m33;
	return sqrt(std::max(std::max(lx,ly),lz));
}

// ============================================================================
// Apply a translation vector to a transformation matrix

void VMAT_SetTranslation (FMATRIX4 *mat, const VECTOR3 *trans)
{
	mat->m41 = (float)trans->x;
	mat->m42 = (float)trans->y;
	mat->m43 = (float)trans->z;
}

void VMAT_SetTranslation(FMATRIX4 *mat, const FVECTOR3 *trans)
{
	mat->m41 = (float)trans->x;
	mat->m42 = (float)trans->y;
	mat->m43 = (float)trans->z;
}

// ============================================================================
//
bool VMAT_VectorMatrixMultiply (FVECTOR3 *res, const FVECTOR3 *v, const FMATRIX4 *mat)
{
    float x = v->x*mat->m11 + v->y*mat->m21 + v->z* mat->m31 + mat->m41;
    float y = v->x*mat->m12 + v->y*mat->m22 + v->z* mat->m32 + mat->m42;
    float z = v->x*mat->m13 + v->y*mat->m23 + v->z* mat->m33 + mat->m43;
    float w = v->x*mat->m14 + v->y*mat->m24 + v->z* mat->m34 + mat->m44;

    if (fabs (w) < 1e-5f) return false;

    res->x = x/w;
    res->y = y/w;
    res->z = z/w;
    return true;
}

// ============================================================================
// Name: VMAT_MatrixMultiply()
// Desc: out = a * b. Counterpart of D3DXMatrixMultiply.
//
//       Written out because D3DX was a Direct3D UTILITY library and Vulkan
//       ships no counterpart -- there is no vkMatrixMultiply, and there is no
//       reason there would be: Vulkan draws, it does not do linear algebra.
//
//       Accumulated into a local before storing, so `out` may alias either
//       operand. D3DXMatrixMultiply documented the same guarantee and the
//       client relies on it -- VulkanText::PrintSkp does
//       VMAT_MatrixMultiply(pSkp->WorldMatrix(), &rot, &mBak) with the
//       destination being the pad's live world matrix.
// ============================================================================

void VMAT_MatrixMultiply (FMATRIX4 *out, const FMATRIX4 *a, const FMATRIX4 *b)
{
    FMATRIX4 r;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            r.data[i * 4 + j] = a->data[i * 4 + 0] * b->data[0 * 4 + j]
                              + a->data[i * 4 + 1] * b->data[1 * 4 + j]
                              + a->data[i * 4 + 2] * b->data[2 * 4 + j]
                              + a->data[i * 4 + 3] * b->data[3 * 4 + j];
        }
    }
    *out = r;
}


// ============================================================================
// Name: VMAT_Transformation2D()
// Desc: Counterpart of D3DXMatrixTransformation2D.
//
//       D3DX defines the result as the product, in this order,
//
//         M = T(-Csc) * Rsc^-1 * S * Rsc * T(Csc) *
//             T(-Crot) * R * T(Crot) * T(t)
//
//       and that order is the whole content of the function: every term is a
//       trivial matrix, and getting them the wrong way round is the only way
//       to get this wrong. It is transcribed from the definition rather than
//       simplified, so it can be checked against the documentation term by
//       term. A NULL pointer means the identity for that term, as D3DX says.
//
//       Z is untouched: this is a 2D transform embedded in a 4x4, which is
//       what the Sketchpad's world matrix is.
// ============================================================================

void VMAT_Transformation2D (FMATRIX4 *out,
                            const FVECTOR2 *pScalingCenter, float scalingRotation,
                            const FVECTOR2 *pScaling,
                            const FVECTOR2 *pRotationCenter, float rotation,
                            const FVECTOR2 *pTranslation)
{
    FMATRIX4 m;
    VMAT_Identity(&m);

    // --- the scaling half ---------------------------------------------------
    if (pScaling) {

        const float scx = pScalingCenter ? pScalingCenter->x : 0.0f;
        const float scy = pScalingCenter ? pScalingCenter->y : 0.0f;

        FMATRIX4 t;  VMAT_Identity(&t);  t.m41 = -scx; t.m42 = -scy;
        VMAT_MatrixMultiply(&m, &m, &t);

        if (scalingRotation != 0.0f) {
            FMATRIX4 ri; VMAT_Identity(&ri);
            const float c = cosf(-scalingRotation), s = sinf(-scalingRotation);
            ri.m11 = c; ri.m12 = s; ri.m21 = -s; ri.m22 = c;
            VMAT_MatrixMultiply(&m, &m, &ri);
        }

        FMATRIX4 sc; VMAT_Identity(&sc); sc.m11 = pScaling->x; sc.m22 = pScaling->y;
        VMAT_MatrixMultiply(&m, &m, &sc);

        if (scalingRotation != 0.0f) {
            FMATRIX4 rf; VMAT_Identity(&rf);
            const float c = cosf(scalingRotation), s = sinf(scalingRotation);
            rf.m11 = c; rf.m12 = s; rf.m21 = -s; rf.m22 = c;
            VMAT_MatrixMultiply(&m, &m, &rf);
        }

        FMATRIX4 tb; VMAT_Identity(&tb); tb.m41 = scx; tb.m42 = scy;
        VMAT_MatrixMultiply(&m, &m, &tb);
    }

    // --- the rotation half --------------------------------------------------
    if (rotation != 0.0f) {

        const float rcx = pRotationCenter ? pRotationCenter->x : 0.0f;
        const float rcy = pRotationCenter ? pRotationCenter->y : 0.0f;

        FMATRIX4 t; VMAT_Identity(&t); t.m41 = -rcx; t.m42 = -rcy;
        VMAT_MatrixMultiply(&m, &m, &t);

        // D3DX's own 2D rotation, row-vector: positive angle turns the
        // +x axis toward +y.
        FMATRIX4 r; VMAT_Identity(&r);
        const float c = cosf(rotation), s = sinf(rotation);
        r.m11 = c; r.m12 = s; r.m21 = -s; r.m22 = c;
        VMAT_MatrixMultiply(&m, &m, &r);

        FMATRIX4 tb; VMAT_Identity(&tb); tb.m41 = rcx; tb.m42 = rcy;
        VMAT_MatrixMultiply(&m, &m, &tb);
    }

    // --- the translation ----------------------------------------------------
    if (pTranslation) {
        FMATRIX4 t; VMAT_Identity(&t); t.m41 = pTranslation->x; t.m42 = pTranslation->y;
        VMAT_MatrixMultiply(&m, &m, &t);
    }

    *out = m;
}


// =======================================================================
// Name: VMAT_MatrixInvert()
// Desc: Does the matrix operation: [Q] = inv[A]. Note: this function only
//       works for matrices with [0 0 0 1] for the 4th column.
//
// Returns the shim's S_OK / E_INVALIDARG: these are this function's OWN
// return values rather than a device's, so they are not VkResult.
// =======================================================================

HRESULT VMAT_MatrixInvert (FMATRIX4 *res, FMATRIX4 *a)
{
    if( fabs(a->m44 - 1.0f) > .001f)
        return E_INVALIDARG;
    if( fabs(a->m14) > .001f || fabs(a->m24) > .001f || fabs(a->m34) > .001f )
        return E_INVALIDARG;

    float fDetInv = 1.0f / ( a->m11 * ( a->m22 * a->m33 - a->m23 * a->m32 ) -
                             a->m12 * ( a->m21 * a->m33 - a->m23 * a->m31 ) +
                             a->m13 * ( a->m21 * a->m32 - a->m22 * a->m31 ) );

    res->m11 =  fDetInv * ( a->m22 * a->m33 - a->m23 * a->m32 );
    res->m12 = -fDetInv * ( a->m12 * a->m33 - a->m13 * a->m32 );
    res->m13 =  fDetInv * ( a->m12 * a->m23 - a->m13 * a->m22 );
    res->m14 = 0.0f;

    res->m21 = -fDetInv * ( a->m21 * a->m33 - a->m23 * a->m31 );
    res->m22 =  fDetInv * ( a->m11 * a->m33 - a->m13 * a->m31 );
    res->m23 = -fDetInv * ( a->m11 * a->m23 - a->m13 * a->m21 );
    res->m24 = 0.0f;

    res->m31 =  fDetInv * ( a->m21 * a->m32 - a->m22 * a->m31 );
    res->m32 = -fDetInv * ( a->m11 * a->m32 - a->m12 * a->m31 );
    res->m33 =  fDetInv * ( a->m11 * a->m22 - a->m12 * a->m21 );
    res->m34 = 0.0f;

    res->m41 = -( a->m41 * res->m11 + a->m42 * res->m21 + a->m43 * res->m31 );
    res->m42 = -( a->m41 * res->m12 + a->m42 * res->m22 + a->m43 * res->m32 );
    res->m43 = -( a->m41 * res->m13 + a->m42 * res->m23 + a->m43 * res->m33 );
    res->m44 = 1.0f;

    return S_OK;
}


// ===========================================================================================
// D3DXMatrixOrthoOffCenterLH, transcribed from the D3DX documentation's own
// matrix rather than rederived. See the note on the declaration for what the
// two handedness differences mean and why neither is corrected here.
// ===========================================================================================
void VMAT_OrthoOffCenterLH(FMATRIX4 *out, float l, float r, float b, float t,
						   float zn, float zf)
{
	if (!out) return;

	VMAT_Identity(out);

	out->m11 = 2.0f / (r - l);
	out->m22 = 2.0f / (t - b);
	out->m33 = 1.0f / (zf - zn);

	out->m41 = (l + r) / (l - r);
	out->m42 = (t + b) / (b - t);
	out->m43 = zn / (zn - zf);
	out->m44 = 1.0f;
}


// ===========================================================================================
// D3DXMatrixOrthoOffCenterRH. The LH form with m33 negated -- which is the
// whole of the handedness difference for an orthographic projection.
// ===========================================================================================
void VMAT_OrthoOffCenterRH(FMATRIX4 *out, float l, float r, float b, float t,
						   float zn, float zf)
{
	if (!out) return;

	VMAT_Identity(out);

	out->m11 = 2.0f / (r - l);
	out->m22 = 2.0f / (t - b);
	out->m33 = 1.0f / (zn - zf);

	out->m41 = (l + r) / (l - r);
	out->m42 = (t + b) / (b - t);
	out->m43 = zn / (zn - zf);
	out->m44 = 1.0f;
}


// ===========================================================================================
// D3DXMatrixLookAtRH, transcribed from the D3DX documentation's construction.
// ===========================================================================================
void VMAT_LookAtRH(FMATRIX4 *out, const FVECTOR3 *pEye, const FVECTOR3 *pAt,
				   const FVECTOR3 *pUp)
{
	if (!out || !pEye || !pAt || !pUp) return;

	FVECTOR3 zaxis = unit(*pEye - *pAt);
	FVECTOR3 xaxis = unit(cross(*pUp, zaxis));
	FVECTOR3 yaxis = cross(zaxis, xaxis);

	out->m11 = xaxis.x;  out->m12 = yaxis.x;  out->m13 = zaxis.x;  out->m14 = 0.0f;
	out->m21 = xaxis.y;  out->m22 = yaxis.y;  out->m23 = zaxis.y;  out->m24 = 0.0f;
	out->m31 = xaxis.z;  out->m32 = yaxis.z;  out->m33 = zaxis.z;  out->m34 = 0.0f;

	out->m41 = -dot(xaxis, *pEye);
	out->m42 = -dot(yaxis, *pEye);
	out->m43 = -dot(zaxis, *pEye);
	out->m44 = 1.0f;
}

// ============================================================================
//
const char *RemovePath(const char *in)
{
	int len = lstrlen(in);
	const char *cptr = in;
	for (int i=0;i<len;i++) if (in[i]=='\\' || in[i]=='/') cptr = &in[i+1];
	return cptr;
}


// Light Emitter ============================================================================
//
VulkanLight::VulkanLight(const LightEmitter *le, const class vObject *vo) :
	cone(1.0f), GPUId(-1),
	cosp(0), tanp(0), cosu(0),
	range(0), range2(0),
	intensity(-1.0), le(NULL)
{
	UpdateLight(le, vo);
}

// ============================================================================
//
VulkanLight::VulkanLight() :
	cone(1.0f), GPUId(-1),
	cosp(0), tanp(0), cosu(0),
	range(0), range2(0),
	intensity(-1.0),
	le(NULL)
{

}

// ============================================================================
//
VulkanLight::~VulkanLight()
{

}

// ============================================================================
//
void VulkanLight::Reset()
{
	intensity = -1.0f;
	cone = 1.0f;
	GPUId = -1;
}

// ============================================================================
//
float VulkanLight::GetIlluminance(FVECTOR3 &_pos, float r) const
{
	if (intensity < 0) return -1.0f;

	FVECTOR3 pos = _pos - Position;

	float d = oapi::length(pos);
	float d2 = d*d;

	if (d < r) return 1e6;	// Light is inside the sphere
	if (d > (r + range)) return -1.0f; // Light can't reach the sphere

	if ((Type == 1) && (cosp>0.1)) {
		float x = oapi::dot(pos, Direction);
		if (x < -r) return -1.0f;	// The sphere is a way behind the spotlight
		if ((sqrt(d2 - x*x) - x*tanp) * cosp > r) return -1.0f; // Light cone doesn't intersect the sphere
	}

	// The sphere is lit from outside
	return intensity / (Attenuation.x + Attenuation.y*d + Attenuation.z*d2);
}

// ============================================================================
//
const LightEmitter *VulkanLight::GetEmitter() const
{
	return le;
}

// ============================================================================
//
void VulkanLight::UpdateLight(const LightEmitter *_le, const class vObject *vo)
{
	le = _le;

	// -----------------------------------------------------------------------------

	Position = oapi::TransformCoord(FVEC(le->GetPosition()), *vo->MWorld());
	Dst2 = oapi::dot(Position, Position);

	// -----------------------------------------------------------------------------

	const double *att = ((PointLight*)le)->GetAttenuation();
	Attenuation = FVECTOR3((float)att[0], (float)att[1], (float)att[2]);

	// -----------------------------------------------------------------------------

	tanp = 0.0f;
	cosu = 1.0f;
	cosp = 1.0f;
	cone = 1.0f;
	float P = 0.0f;
	float U = 0.0f;

	if (le->GetType() == LightEmitter::LT_SPOT) {
		P = float(((SpotLight*)le)->GetPenumbra());
		U = float(((SpotLight*)le)->GetUmbra());
		if (P > 3.05f) P = 3.05f;
		if (U > 2.96f) U = 2.96f;
	}

	// -----------------------------------------------------------------------------
	switch (le->GetType()) {

		case LightEmitter::LT_POINT: {
			Type = 0;
		} break;

		case LightEmitter::LT_SPOT: {
			Type = 1;
			// Param[i] on Windows. D3DXVECTOR4 has operator[]; oapi::FVECTOR4 does
			// not -- but its union carries float data[4] over the same storage, so
			// this is the identical write under the spelling the type offers.
			cosp = cos(P * 0.5f);
			cosu = cos(U * 0.5f);
			tanp = tan(P * 0.5f);
			Param.data[VulkanLFalloff] = 1.0f;
			Param.data[VulkanLPhi] = cosp;
			Param.data[VulkanLTheta] = 1.0f / (cosu - cosp);
		} break;

		default:
			LogErr("Invalid Light Emitter Type");
			break;
	}

	// -----------------------------------------------------------------------------
	intensity = float(le->GetIntensity());
	const COLOUR4 &col_d = le->GetDiffuseColour();
	Diffuse.r = (col_d.r*intensity);
	Diffuse.g = (col_d.g*intensity);
	Diffuse.b = (col_d.b*intensity);
	Diffuse.a = (col_d.a*intensity);


	float c = float(att[0]);
	float b = float(att[1]);
	float a = float(att[2]);
	float limit = 0.01f; // Intensity limit for max range
	float Q = intensity - c*limit;
	float d = b*b*limit + 4.0f*a*Q;

	range = (sqrt(limit * d) - b*limit) / (2.0f*a*limit);

	range = std::min(range, float(((PointLight*)le)->GetRange()));

	range2 = range*range;
	Param.data[VulkanLRange] = range;


	// -----------------------------------------------------------------------------
	if (Type != 0) {
		Direction = oapi::TransformNormal(FVEC(le->GetDirection()), *vo->MWorld());
		float angle = acos(oapi::dot(oapi::unit(Position), Direction));
		cone = ilerp(U * 0.5f, P * 0.5f, angle);
	}
}


// ==============================================================================================
// SHADER COMPILATION
//
// The Windows pair, CompilePixelShader and CompileVertexShader, are ~110 lines
// each and are the same function twice over -- the only differences are
// "ps_3_0" vs "vs_3_0", CreatePixelShader vs CreateVertexShader, and a
// disassembly branch the vertex one does not have. Converted here as one
// worker with a stage argument and two thin entry points, because on this side
// the two bodies would have been character-for-character identical.
//
// WHAT REPLACES D3DX
//
//   D3DXCompileShaderFromFileA(file, macros, NULL, entry, "ps_3_0", ...)
//     -> glslang: parse GLSL, link, emit SPIR-V. The profile string is gone;
//        a SPIR-V module records its own stage and version.
//
//   D3DXGetShaderConstantTable(bytecode, &pConst)
//     -> reflection over the linked program. D3DX produced the constant table
//        as a side product of compilation; glslang produces the same
//        information from the same parse, so this is still one step.
//
//   pDev->CreatePixelShader(bytecode, &pShader)
//     -> vkCreateShaderModule.
//
//   D3DXMACRO macro[16] / macro[32], filled from a ";, "-separated option
//     string -> #define lines prepended to the source. glslang has no macro
//     array; the preamble is how it takes them. The 16-entry cap the Windows
//     loops enforce is gone with the fixed array.
//
//   D3DXDisassembleShader + an "_asm.html" dump, behind the DISASM option
//     -> spv::Disassemble into a .spvasm file. Same purpose, same trigger.
//
// THE SHADER CACHE CONVERTS DIRECTLY. Its logic is: stat the cache file, stat
// the source, and use the cache only if it is NEWER. That is
// CreateFile/GetFileTime/CompareFileTime/ReadFile on Windows and stat()/fopen/
// fread here -- the same three questions, asked with POSIX calls. The cached
// blob is SPIR-V rather than DXBC, so the file name keeps the crc of the
// option string that the Windows version already used to tell variants apart.
//
// glslang needs process-wide init exactly once. D3DX needed none, so there is
// no counterpart to convert -- it is a requirement of the replacement.
// ==============================================================================================

#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#include <glslang/SPIRV/disassemble.h>
#include <sstream>
#include <fstream>

static bool g_bGlslangReady = false;

static void EnsureGlslang()
{
	if (!g_bGlslangReady) {
		glslang::InitializeProcess();
		g_bGlslangReady = true;
	}
}

// Modification time in seconds, or 0 if the file is not there.
// Counterpart of GetFileTime(); CompareFileTime becomes a comparison.
static time_t FileTime(const char *path)
{
	struct stat st;
	if (stat(path, &st) != 0) return 0;
	return st.st_mtime;
}

static bool ReadWholeFile(const char *path, std::string &out)
{
	FILE *f = NULL;
	if (fopen_s(&f, path, "rb") || !f) return false;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	rewind(f);
	if (n < 0) { fclose(f); return false; }
	out.resize((size_t)n);
	size_t got = n ? fread(&out[0], 1, (size_t)n, f) : 0;
	fclose(f);
	out.resize(got);
	return true;
}

// ----------------------------------------------------------------------------------------------
// Reflection. Counterpart of what D3DXGetShaderConstantTable handed back.
//
// glslang reports uniforms with a type, an offset within their block and a
// binding. A sampler has no offset -- it is a descriptor -- so it is recorded
// with its binding as the "sampler index" GetSamplerIndex() returns, which is
// the same question D3D9 answered with a sampler register number.
//
// IS A REFLECTED DESCRIPTOR CUBE-DIMENSIONED?
//
// TObjectReflection::glDefineType is the one public field that carries the
// type. glslang fills it with the OpenGL enum for that type -- the same value
// glGetActiveUniformsiv(GL_UNIFORM_TYPE) reports -- so the cube-dimensioned
// declarations are exactly this set. getType() would ask directly, but it
// hands back a const TType* and TType is only forward-declared by the
// installed headers; that is the same reason the offset test below is what
// spots a descriptor.
//
// The values are the ones in /usr/include/GL/glcorearb.h. They are named here
// rather than pulled in from a GL header so that a Vulkan client does not
// acquire a GL include dependency for four constants.
//
// D3D9 had no counterpart to convert. SetTexture() took any
// IDirect3DBaseTexture9 and the runtime reconciled it against the sampler
// declaration; in Vulkan a cube declaration sampled through a 2D image view
// is a GPU fault, so the client has to know which it is.
static bool IsCubeGlType(int glDefineType)
{
	switch (glDefineType) {
	case 0x8B60:	// GL_SAMPLER_CUBE
	case 0x8DC5:	// GL_SAMPLER_CUBE_SHADOW
	case 0x8DCC:	// GL_INT_SAMPLER_CUBE
	case 0x8DD4:	// GL_UNSIGNED_INT_SAMPLER_CUBE
	case 0x900C:	// GL_SAMPLER_CUBE_MAP_ARRAY
	case 0x900D:	// GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW
	case 0x900E:	// GL_INT_SAMPLER_CUBE_MAP_ARRAY
	case 0x900F:	// GL_UNSIGNED_INT_SAMPLER_CUBE_MAP_ARRAY
	case 0x9050:	// GL_IMAGE_CUBE
	case 0x9054:	// GL_IMAGE_CUBE_MAP_ARRAY
	case 0x905B:	// GL_INT_IMAGE_CUBE
	case 0x905F:	// GL_INT_IMAGE_CUBE_MAP_ARRAY
	case 0x9066:	// GL_UNSIGNED_INT_IMAGE_CUBE
	case 0x906A:	// GL_UNSIGNED_INT_IMAGE_CUBE_MAP_ARRAY
		return true;
	default:
		return false;
	}
}

static void BuildReflection(glslang::TProgram &program, VkShaderStageFlagBits stage, ShaderReflection *pRefl)
{
	program.buildReflection(EShReflectionSeparateBuffers | EShReflectionAllBlockVariables);

	uint32_t nSampler = 0;

	// THE BLOCK BINDINGS, READ FIRST, BECAUSE A BLOCK MEMBER DOES NOT CARRY
	// ONE. Measured, not assumed: glslang reports getBinding() == -1 for every
	// variable inside a uniform block and the real binding only on the BLOCK.
	// A member's `index` is the index of its containing block (-1 for a
	// sampler, which is not in one), so the two join on that.
	//
	// This mattered the moment a stage had a block anywhere but binding 0:
	// without it every block member reported binding 0, and IProcess's
	// descriptor set layout -- which puts the vertex stage's block at 0 and
	// the pixel stage's at 1 -- would have declared both at 0 and collided.
	//
	// D3D9 had no equivalent question. A constant lived in a numbered
	// register file per stage and the two stages could not collide, so there
	// was nothing to look up.
	int nb = program.getNumUniformBlocks();
	std::vector<uint32_t> blockBinding((size_t)(nb > 0 ? nb : 0), 0);
	for (int i = 0; i < nb; i++) {
		const int b = program.getUniformBlock(i).getBinding();
		blockBinding[(size_t)i] = (b < 0) ? 0 : (uint32_t)b;
	}

	// ONE UNIFORM BLOCK PER STAGE is the convention every converted shader
	// honours, and it is what makes the single per-stage buffer below
	// correct: offsets are per-block, so two live blocks in one stage would
	// overlap in that buffer. glslang reports only LIVE uniforms -- a block
	// the entry point does not reference is not listed at all, which was
	// checked rather than assumed -- so this fires only when a stage really
	// does read two.
	if (nb > 1) {
		LogErr("BuildReflection: stage declares %d live uniform blocks; the "
			   "client stages one buffer per stage and expects exactly one", nb);
	}

	// The block members, in declaration order, kept so the aggregates below
	// can be reconstructed from them. See the note after the loop.
	struct Leaf { std::string name; uint32_t offset; };
	std::vector<Leaf> leaves;

	int n = program.getNumUniformVariables();
	for (int i = 0; i < n; i++)
	{
		const glslang::TObjectReflection &u = program.getUniform(i);

		// IS THIS A DESCRIPTOR OR BYTES IN A UNIFORM BLOCK?
		//
		// glslang's TType would answer directly (getBasicType() == EbtSampler)
		// but TType is only forward-declared by the public headers -- its
		// definition lives in glslang/MachineIndependent, which is not
		// installed. TObjectReflection's public fields answer it anyway:
		// glslang gives a block member its byte offset within the block, and
		// leaves offset at -1 for anything that is not in a block.
		//
		// In Vulkan GLSL that is an exact test rather than a heuristic,
		// because a non-opaque uniform is REQUIRED to be inside a block --
		// only opaque types (samplers, images) can sit at global scope. So
		// "no offset" means "descriptor", which is precisely the distinction
		// this flag exists to make.
		ShaderReflection::Var v = {};
		v.name = u.name;
		v.stage = stage;
		v.set = 0;
		v.bSampler = (u.offset < 0);
		v.offset = v.bSampler ? 0 : (uint32_t)u.offset;
		v.size = (u.size < 0) ? 0 : (uint32_t)u.size;
		// A sampler carries its own binding; a block member takes its block's.
		// See the note above.
		if (v.bSampler)
			v.binding = (u.getBinding() < 0) ? 0 : (uint32_t)u.getBinding();
		else if (u.index >= 0 && (size_t)u.index < blockBinding.size())
			v.binding = blockBinding[(size_t)u.index];
		else
			v.binding = 0;
		// Is it a samplerCube? The dim the declaration carries is the dim the
		// SPIR-V OpTypeImage carries, and the fallback texture bound for an
		// unset slot has to match it. See IsCubeGlType above and Var::bCube.
		v.bCube = v.bSampler && IsCubeGlType(u.glDefineType);
		v.samplerIndex = v.bSampler ? nSampler++ : 0;
		if (!v.bSampler) leaves.push_back({ v.name, v.offset });
		pRefl->Add(v);
	}

	uint32_t block = 0;
	for (int i = 0; i < nb; i++) block += (uint32_t)program.getUniformBlock(i).size;
	pRefl->SetBlockSize(block);

	// ------------------------------------------------------------------
	// THE AGGREGATES, WHICH GLSLANG DOES NOT REPORT AND D3DX DID.
	//
	// A D3DX constant table was a TREE. GetConstantByName("Const") on a
	// struct-typed constant returned a handle to the WHOLE struct, and
	// SetValue(dev, h, &CelData, sizeof(CelData)) wrote all of it in one go.
	// That is how the client uses it -- CSphereMgr asks for "Const" and
	// "Flow" and hands over a whole CelDataStruct and a whole CelDataFlow;
	// so do the tile managers, the haze and the glares.
	//
	// glslang's reflection is FLAT. Measured, not assumed: a block declared
	//
	//     layout(scalar, binding = 1) uniform CelSpherePS {
	//         CelDataStruct Const;
	//         CelDataFlow   Flow;
	//     };
	//
	// reflects as six variables named "Const.mWorld" ... "Flow.bBeta" and
	// NOTHING named "Const" or "Flow" at all. Looking either name up returns
	// NULL, SetPSConstants returns without writing, and the draw runs with an
	// uninitialised block -- silently, because a missing handle is not an
	// error anywhere in this path.
	//
	// So the aggregate entries are rebuilt here from the leaves. They are not
	// an invention: they are the nodes D3DX's table had and this one is
	// missing, and the client's call sites are written against them.
	//
	// THE EXTENT IS COMPUTED FROM OFFSETS, NOT FROM REPORTING ORDER, and it
	// has to be. Block members do arrive in declaration order, but glslang
	// ALSO appends array-level entries after everything else -- a
	// `Light gLights[16]` reflects its 112 element members in place and then
	// adds six more named "gLights.type", "gLights.diffuse" and so on at the
	// very end of the list, out of offset order. Measured on
	// VulkanClient.glsl, not assumed.
	//
	// So an aggregate is not a contiguous RUN of leaves. It is a range of
	// OFFSETS: it starts at the lowest offset of any member carrying its
	// prefix, and it ends at the lowest offset in the whole block that is
	// greater than the highest of them -- or at the end of the block if there
	// is none. A member's own byte size is never needed, which is what makes
	// this exact despite glslang reporting element counts rather than bytes.
	//
	// For CelDataStruct that gives offset 0 size 136 and for CelDataFlow
	// offset 136 size 8, which are sizeof() of the two C++ structs
	// CSphereMgr passes; for gLights, offset 1284 size 1216, which is
	// 16 * sizeof(LightStruct).
	//
	// Every prefix is emitted, not just the first level, so a nested
	// "Outer.Inner.x" yields both "Outer" and "Outer.Inner" -- the same two
	// handles D3DX would have had.
	//
	// A '[' OPENS AN AGGREGATE TOO, and leaving it out was a real omission
	// rather than a nicety: `Light gLights[MAX_LIGHTS]` reflects as
	// "gLights[0].type" ... "gLights[11].param", so splitting only at '.'
	// yields twelve aggregates named gLights[0] .. gLights[11] and NOTHING
	// named gLights -- while the client's one and only use is
	//
	//     eLights = FX->GetParameterByName("gLights");
	//     FX->SetValue(eLights, Locals, sizeof(LightStruct) * MaxLights);
	//
	// which writes the whole array in one go. Splitting at '[' as well gives
	// both: gLights (the array) and gLights[i] (one element).
	// ------------------------------------------------------------------
	{
		struct Span { uint32_t lo, hi; size_t first; };
		std::vector<std::string> order;			// prefixes, first-seen order
		std::map<std::string, Span> spans;

		for (size_t i = 0; i < leaves.size(); i++) {
			const std::string &nm = leaves[i].name;
			const uint32_t off = leaves[i].offset;
			for (size_t p = 0; p < nm.size(); p++) {
				if (nm[p] != '.' && nm[p] != '[') continue;
				if (p == 0) continue;
				const std::string pre = nm.substr(0, p);
				auto it = spans.find(pre);
				if (it == spans.end()) {
					Span s = { off, off, i };
					spans[pre] = s;
					order.push_back(pre);
				}
				else {
					if (off < it->second.lo) { it->second.lo = off; it->second.first = i; }
					if (off > it->second.hi) it->second.hi = off;
				}
			}
		}

		for (const std::string &pre : order) {
			// A name that is already a leaf is not an aggregate. This cannot
			// happen in GLSL -- a member and a struct cannot share a name in
			// one block -- but the table must not hold two entries for one
			// name either way, because GetConstantByName returns the first.
			if (pRefl->GetConstantByName(pre.c_str())) continue;

			const Span &s = spans[pre];

			// The end of the aggregate: the lowest member offset anywhere in
			// the block that lies past its last member, or the block's end.
			uint32_t end = block;
			for (size_t i = 0; i < leaves.size(); i++) {
				const uint32_t off = leaves[i].offset;
				if (off > s.hi && off < end) end = off;
			}

			ShaderReflection::Var a = {};
			a.name = pre;
			a.stage = stage;
			a.set = 0;
			// Every leaf of one aggregate is in one block, so its lowest
			// member answers for all of them. Read back rather than assumed,
			// exactly as the members' own bindings are.
			const ShaderReflection::Var *pFirst =
				pRefl->GetConstantByName(leaves[s.first].name.c_str());
			a.binding = pFirst ? pFirst->binding : 0;
			a.offset = s.lo;
			a.size = (end > s.lo) ? (end - s.lo) : 0;
			a.bSampler = false;
			a.samplerIndex = 0;
			pRefl->Add(a);
		}
	}
}


// ----------------------------------------------------------------------------------------------
//
static VkShaderModule CompileShaderStage(VulkanDevice *pDev, EShLanguage lang, VkShaderStageFlagBits stage,
										 const char *file, const char *function, const char *name,
										 const char *options, ShaderReflection **pConst)
{
	if (!pDev) return VK_NULL_HANDLE;

	WORD crc = 0;
	if (options) crc = crc16(options, strlen(options));

	std::string path(file);
	char filename[MAX_PATH];

	std::string last = path.substr(path.find_last_of("\\/") + 1);
	sprintf_s(filename, MAX_PATH, "Cache/VulkanClient/Shaders/%s_%s_%hX_%s.spv", name, function, crc, last.c_str());

	std::vector<unsigned int> spirv;

	// Browse Shader Cache --------------------
	//
	// Was CreateFile + GetFileTime on both files + CompareFileTime; the cache
	// is used only when it is newer than the source. Same three questions.
	//
	// THE CACHE IS NOT CONSULTED WHEN A CONSTANT TABLE IS WANTED, and that is
	// the one real divergence from the reference here.
	//
	// D3D9Util.cpp:1048 answers BOTH questions from the cached blob:
	//
	//     HR(pDev->CreatePixelShader(buffer, &pShader));
	//     HR(D3DXGetShaderConstantTable(buffer, pConst));
	//
	// D3DX reflects compiled bytecode. Nothing here reflects SPIR-V -- the
	// reflection this client builds comes out of glslang's TProgram while the
	// source is being parsed, so a cached module cannot produce one. When a
	// caller asks for a constant table the source therefore has to be parsed,
	// and reading the cache first can only waste the read.
	//
	// THIS BLOCK USED TO RUN ANYWAY, AND THE RECOVERY BELOW IT WAS INFINITE
	// RECURSION. On a cache hit with pConst set, the tail of this function
	// did
	//
	//     spirv.clear();
	//     return CompileShaderStage(pDev, lang, stage, file, function, name,
	//                               options ? options : "", pConst);
	//
	// -- the same arguments, so the callee read the same cache, hit again,
	// and recursed again. `spirv.clear()` clears the CALLER's vector and
	// tells the callee nothing. Every frame carries a 260-byte filename
	// buffer plus glslang's locals, so the stack was gone in a few thousand
	// frames: SIGSEGV inside the sprintf_s on the next frame's filename,
	// before the scene was ever created.
	//
	// It is silent until the cache is both present AND newer than the source,
	// which is exactly one run after a shader source is touched: the first
	// run compiles and WRITES the cache, and every run after it crashes at
	// startup until something makes a .glsl newer again. Measured on this
	// tree -- three files in Cache/VulkanClient/Shaders, all SketchTech, all
	// newer than Modules/VulkanClient/Sketchpad.glsl, and the crash was on
	// the first of the three.
	if (Config->ShaderCacheUse && !pConst)
	{
		time_t tCache = FileTime(filename);
		time_t tMain = FileTime(file);
		if (tCache && tMain && tCache > tMain)
		{
			std::string blob;
			if (ReadWholeFile(filename, blob) && blob.size() >= 4 && (blob.size() % 4) == 0)
			{
				spirv.resize(blob.size() / 4);
				memcpy(spirv.data(), blob.data(), blob.size());
			}
		}
	}

	bool bDisassemble = false;

	if (spirv.empty())
	{
		LogAlw("Compiling a Shader [%s] function [%s] name [%s]...", file, function, name);

		std::string src;
		if (!ReadWholeFile(file, src)) {
			LogErr("Failed to open shader source [%s]", file);
			return VK_NULL_HANDLE;
		}

		// D3DXMACRO array -> a #define preamble. The Windows loop splits the
		// option string on ";, " and stops at 16; glslang takes them as text,
		// so there is no array and no cap.
		std::string preamble;

		// THE STAGE DEFINE IS NEW, AND IT IS FORCED BY A LANGUAGE DIFFERENCE
		// RATHER THAN BY VULKAN.
		//
		// HLSL passes a stage's inputs as FUNCTION PARAMETERS -- `float4
		// PSMain(float x : TEXCOORD0)` -- so one .fx can declare a dozen
		// entry points for both stages and nothing about one is visible to
		// another. GLSL declares them as FILE-SCOPE globals with explicit
		// locations, so a file holding both a vertex and a pixel entry point
		// declares two sets of `in` variables at overlapping locations, and
		// in the vertex stage the pixel stage's interpolants would be read as
		// vertex ATTRIBUTES -- a location conflict with the real ones.
		//
		// So the compiler has to tell the source which stage it is building,
		// and the source guards its two blocks of declarations on it. Every
		// converted shader that holds both stages does this; the names are
		// distinct from every option token any .fx uses (checked across all
		// twenty-three).
		preamble += (stage == VK_SHADER_STAGE_VERTEX_BIT)
					? "#define _VERTEX_SHADER 1\n"
					: "#define _FRAGMENT_SHADER 1\n";

		// THE ENTRY-POINT DEFINE, FOR THE SAME REASON AND ONE STEP FURTHER.
		//
		// The stage define is not enough for a file that holds MANY entry
		// points of one stage, which VulkanClient.glsl does: twelve different
		// vertex shaders, each with its own set of interpolants. In HLSL
		// those are the members of twelve output STRUCTS and are invisible to
		// each other. In GLSL they are file-scope `out` variables with
		// explicit locations, and:
		//
		//   - two of them may not share a location. glslang rejects
		//     "overlapping use of location 0" at PARSE time, whether or not
		//     the entry point being compiled touches either one; and
		//
		//   - giving all forty-one a distinct location does not help, because
		//     glslang lists EVERY declared output in OpEntryPoint's interface
		//     -- measured, not assumed: a file declaring ten outputs and
		//     writing two names all ten -- so they all count against
		//     maxVertexOutputComponents, which is 128 on ordinary hardware.
		//
		// So the source has to be told WHICH entry point is being built, and
		// declare only that one's interpolants. `_EP_<name>` is that, and it
		// restores exactly the property HLSL had for free.
		if (function && *function) {
			preamble += "#define _EP_";
			preamble += function;
			preamble += " 1\n";
		}

		// A TOKEN MAY CARRY A VALUE, AND THAT IS NOT AN EXTENSION -- IT IS
		// WHAT D3DXMACRO ALREADY WAS.
		//
		// A D3DXMACRO is a PAIR: { Name, Definition }. D3D9Effect.cpp's
		// D3D9TechInit builds six of them with values --
		//
		//     macro[0].Name = "ANISOTROPY_MACRO";
		//     sprintf_s((char*)macro[0].Definition, 32, "%d", ...);
		//
		// -- and five with a Name only (_GLASS, _DEBUG, _ENVMAP, _LIGHTGLOW,
		// _IRRADIANCE), which D3DX treats as defined-to-empty. The client's
		// own option strings (ShaderClass's and ImageProcessing's) are the
		// Name-only kind, which is why splitting on ";, " and defining each
		// token to 1 was enough for them.
		//
		// It is NOT enough for the effect file, and that is a real bug rather
		// than a shortfall: `ANISOTROPY_MACRO=13` went through this loop as
		// one token and came out as `#define ANISOTROPY_MACRO=13 1`, which is
		// not a valid directive, so VulkanClient.glsl could not compile at
		// all. The pair is restored by splitting the token at its '=' --
		// Name before, Definition after -- which is the D3DXMACRO the caller
		// meant.
		if (options) {
			std::string opt(options);
			size_t a = 0;
			while (a < opt.size()) {
				size_t b = opt.find_first_of(";, ", a);
				if (b == std::string::npos) b = opt.size();
				std::string tok = opt.substr(a, b - a);
				if (tok == "DISASM") bDisassemble = true;
				else if (tok == "PARTIAL") { /* D3DXSHADER_PARTIALPRECISION: see note below */ }
				else if (!tok.empty()) {
					const size_t eq = tok.find('=');
					if (eq == std::string::npos) preamble += "#define " + tok + " 1\n";
					else preamble += "#define " + tok.substr(0, eq) + " " + tok.substr(eq + 1) + "\n";
				}
				a = b + 1;
			}
		}

		EnsureGlslang();

		glslang::TShader shader(lang);
		const char *pSrc = src.c_str();
		const char *pName = file;
		int len = (int)src.size();
		shader.setStringsWithLengthsAndNames(&pSrc, &len, &pName, 1);
		shader.setPreamble(preamble.c_str());
		shader.setEntryPoint(function);
		shader.setSourceEntryPoint(function);
		shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
		shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_2);
		shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_5);

		if (!shader.parse(GetDefaultResources(), 450, false, EShMsgDefault)) {
			LogErr("Compiling a Shader [%s] function [%s] Failed:\n %s", file, function, shader.getInfoLog());
			return VK_NULL_HANDLE;
		}

		glslang::TProgram program;
		program.addShader(&shader);
		if (!program.link(EShMsgDefault)) {
			LogErr("Linking a Shader [%s] function [%s] Failed:\n %s", file, function, program.getInfoLog());
			return VK_NULL_HANDLE;
		}

		glslang::SpvOptions spvOpts;
		spv::SpvBuildLogger logger;
		glslang::GlslangToSpv(*program.getIntermediate(lang), spirv, &logger, &spvOpts);

		if (spirv.empty()) {
			LogErr("Failed to emit SPIR-V for [%s] [%s]", file, function);
			return VK_NULL_HANDLE;
		}

		if (pConst) {
			*pConst = new ShaderReflection(stage);
			BuildReflection(program, stage, *pConst);

			// The reflected layout, so the offsets the client WRITES at can be
			// compared against offsetof() on the C++ struct it writes FROM.
			// Resolving by name proves the constant exists; it does not prove
			// it is in the same place. Env-gated.
			if (getenv("ORBITER_VK_TRACE_REFL")) {
				LogErr("REFLDUMP ---- %s / %s (%s) blockSize=%u ----",
					   RemovePath(file), function,
					   (stage == VK_SHADER_STAGE_VERTEX_BIT) ? "VS" : "PS",
					   (unsigned)(*pConst)->BlockSize());
				for (const ShaderReflection::Var &v : (*pConst)->Vars()) {
					if (v.bSampler)
						LogErr("REFLDUMP   sampler %-28s binding=%-3u cube=%d",
							   v.name.c_str(), v.binding, int(v.bCube));
					else
						LogErr("REFLDUMP   const   %-28s offset=%-6u size=%-6u binding=%u",
							   v.name.c_str(), v.offset, v.size, v.binding);
				}
			}
		}

		// Save Shader into a Cache
		if (Config->ShaderCacheUse)
		{
			FILE *fc = NULL;
			if (fopen_s(&fc, filename, "wb") == 0 && fc) {
				fwrite(spirv.data(), 4, spirv.size(), fc);
				fclose(fc);
			}
			else {
				LogErr("CreateShaderCache: fopen failed");
				LogErr("Path=[%s]", filename);
			}
		}

		// Was D3DXDisassembleShader into "<file>_<fn>_asm.html".
		if (bDisassemble) {
			char dname[MAX_PATH];
			sprintf_s(dname, MAX_PATH, "%s_%s_asm.spvasm", RemovePath(file), function);
			std::ofstream out(dname);
			if (out.good()) spv::Disassemble(out, spirv);
		}
	}
	// No `else if (pConst)` branch here any more, and none is needed: the
	// cache read above is skipped whenever pConst is set, so a caller that
	// wants a constant table always reaches the compile path and `spirv` is
	// never both non-empty and reflection-less. See the note on that block.

	VkShaderModuleCreateInfo ci = {};
	ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	ci.codeSize = spirv.size() * 4;
	ci.pCode = spirv.data();

	VkShaderModule mod = VK_NULL_HANDLE;
	HR(vkCreateShaderModule(pDev->GetDevice(), &ci, nullptr, &mod));
	return mod;
}


// ============================================================================
//
// D3DXSHADER_PARTIALPRECISION ("PARTIAL") HAS NO COUNTERPART. It asked ps_3_0
// for half precision, which SPIR-V expresses as a 16-bit float type in the
// shader source rather than a compile flag. The option is parsed and ignored
// so existing call sites keep working; where it actually matters the GLSL
// translation says float16_t.
//
VkShaderModule CompilePixelShader(VulkanDevice *pDev, const char *file, const char *function, const char *name, const char* options, ShaderReflection **pConst)
{
	return CompileShaderStage(pDev, EShLangFragment, VK_SHADER_STAGE_FRAGMENT_BIT,
							  file, function, name, options, pConst);
}

// ============================================================================
//
VkShaderModule CompileVertexShader(VulkanDevice *pDev, const char *file, const char *function, const char* name, const char *options, ShaderReflection **pConst)
{
	return CompileShaderStage(pDev, EShLangVertex, VK_SHADER_STAGE_VERTEX_BIT,
							  file, function, name, options, pConst);
}


// ============================================================================
//
const ShaderReflection::Var *ShaderReflection::GetConstantByName(const char *name) const
{
	if (!name) return NULL;
	for (auto &v : vars) if (v.name == name) return &v;
	return NULL;
}

void ShaderReflection::Add(const Var &v)
{
	vars.push_back(v);
}


// ============================================================================
// Was CreateVolumeTexture(pDevice, count, LPDIRECT3DTEXTURE9*, LPDIRECT3DVOLUMETEXTURE9*).
//
// The Windows version builds the volume in TWO textures: a D3DPOOL_SYSTEMMEM
// one it can LockBox, and a D3DPOOL_DEFAULT one it UpdateTexture()s into --
// because a DEFAULT-pool resource cannot be locked. Vulkan says the same
// thing more plainly (device-local memory is not mappable) and answers it the
// same way, except that the staging copy is a BUFFER rather than a second
// image, and the copy is an explicit vkCmdCopyBufferToImage. All of that is
// inside VulkanDevice::UploadTexture, so what is left here is the loop.
//
// The pitch check the Windows code performs --
//     (box.RowPitch == rect.Pitch) && (box.SlicePitch == rect.Pitch*height)
// -- guards against the driver padding rows of the volume differently from
// rows of the source. It has no counterpart: the staging buffer is packed to
// exactly the extent being copied, and bufferRowLength/bufferImageHeight left
// at 0 tell Vulkan so.
//
bool CreateVolumeTexture(VulkanDevice *pDevice, int count, VulkanTexture **pIn, VulkanTexture **pOut)
{
	if (count==0 || pDevice==NULL || pIn==NULL || pOut==NULL) return false;
	if (pIn[0]==NULL) return false;

	uint32_t width = pIn[0]->Width();
	uint32_t height = pIn[0]->Height();
	uint32_t mips = pIn[0]->Mips();
	VkFormat fmt = pIn[0]->Format();

	VulkanTexture *pVol = pDevice->CreateTexture3D(width, height, (uint32_t)count, mips, fmt,
		VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

	if (!pVol) {
		LogErr("CreateVolumeTexture: failed to create the 3D image");
		return false;
	}

	for (uint32_t m = 0; m < mips; m++)
	{
		for (int i = 0; i < count; i++)
		{
			if (!pIn[i]) {
				LogErr("CreateVolumeTexture: input %d is NULL", i);
				pDevice->DestroyTexture(pVol);
				return false;
			}

			void *pSrc = pIn[i]->Map();
			if (!pSrc) {
				LogErr("CreateVolumeTexture: source image is not host visible");
				pDevice->DestroyTexture(pVol);
				return false;
			}

			size_t bytes = SurfNative::StaticFormatSizeInBytes(fmt,
				std::max(1u, width >> m) * std::max(1u, height >> m));

			bool ok = pDevice->UploadTexture(pVol, m, (uint32_t)i, pSrc, bytes);
			pIn[i]->Unmap();

			if (!ok) {
				LogErr("CreateVolumeTexture: upload failed at mip %u slice %d", m, i);
				pDevice->DestroyTexture(pVol);
				return false;
			}
		}
	}

	*pOut = pVol;
	return true;
}


// ============================================================================
// Planet Texture Loader
//
// The Windows version reads a file that is SEVERAL DDS IMAGES CONCATENATED,
// walks it by decoding each header to find where the next one starts, and
// hands each slice to D3DXCreateTextureFromFileInMemoryEx.
//
// The walk survives unchanged -- it is arithmetic on the DDS header, not a
// D3D operation -- and this is the part worth keeping verbatim, because it
// carries a correction the reference makes for headers that declare neither
// DDSD_LINEARSIZE nor DDSD_PITCH.
//
// What changes is the decode at the end: there is no D3DX, so the DDS body is
// parsed and uploaded here. That parser is shared with VulkanSurface.cpp
// rather than written twice; see NatCreateTextureFromDDSInMemory.
//
// <ddraw.h> and the hand-rolled DDSURFACEDESC2_x64 struct are gone with it.
// The reference declares its own copy of the DirectDraw surface description
// because the SDK one changes size between 32- and 64-bit builds; the fields
// it actually reads -- dwFlags, dwHeight, dwWidth, dwLinearSize, the FourCC
// and dwRGBBitCount -- are at fixed offsets in the DDS FILE FORMAT, which is
// what NatDDSHeader below describes.
//
int LoadPlanetTextures(const char* fname, VulkanTexture** ppdds, DWORD flags, int amount)
{
	_TRACE;

	char path[MAX_PATH];

	if (g_client->TexturePath(fname, path)) {

		FILE* f;

		if (fopen_s(&f, path, "rb")) return 0;

		int ntex = 0;
		char* buffer, * location;
		fseek(f, 0, SEEK_END);
		long size = ftell(f);
		long BytesLeft = size;
		buffer = new char[size + 1];
		rewind(f);
		size_t got = fread(buffer, 1, size, f);
		fclose(f);
		BytesLeft = (long)got;

		location = buffer;
		while (ntex < amount && BytesLeft > 0)
		{
			DWORD Magic = *(DWORD*)location;
			if (Magic != MAKEFOURCC('D', 'D', 'S', ' ')) break;

			long bytes = NatDDSImageBytes(location, BytesLeft);
			if (bytes <= 0) break;

			VulkanTexture *pTex = NatCreateTextureFromDDSInMemory(location, (size_t)bytes);

			if (pTex) {
				ppdds[ntex] = pTex;
			}
			else {
				delete[] buffer;
				LogErr("Failed to surface tile (%d tiles loaded for %s)", ntex, fname);
				return ntex;
			}

			location += bytes;
			BytesLeft -= bytes;
			ntex++;
		}
		delete[] buffer;
		LogOk("Loaded %d textures for %s", ntex, fname);
		return ntex;
	}
	LogWrn("File %s not found", fname);
	return 0;
}


// ======================================================================================
// SketchMesh Interface
// ======================================================================================

SketchMesh* GetSketchMesh(const MESHHANDLE hMesh)
{
	if (MeshMap.find(hMesh) == MeshMap.end())
	{
		SketchMesh* pMesh = new SketchMesh(g_client->GetDevice());

		if (pMesh->LoadMeshFromHandle(hMesh))
		{
			MeshMap[hMesh] = pMesh;
			return pMesh;
		}
		delete pMesh;
		return NULL;
	}
	else return MeshMap[hMesh];
}


SketchMesh::SketchMesh(VulkanDevice *_pDev) :
	pVB(NULL),
	pIB(NULL),
	MaxVert(0), MaxIdx(0),
	nGrp(0), nMtrl(0), nTex(0),
	pDev(_pDev),
	Tex(NULL),
	Grp(NULL),
	Mtrl(NULL)
{
}


// ===============================================================================================
//
SketchMesh::~SketchMesh()
{
	SAFE_DELETEA(Mtrl);
	SAFE_DELETEA(Tex);
	SAFE_DELETEA(Grp);
	// Was SAFE_RELEASE. Vulkan objects are not reference counted; the device
	// that made them destroys them.
	if (pVB) { pDev->DestroyBuffer(pVB); pVB = NULL; }
	if (pIB) { pDev->DestroyBuffer(pIB); pIB = NULL; }
}


// ===============================================================================================
//
bool SketchMesh::LoadMeshFromHandle(MESHHANDLE hMesh)
{
	pVB = NULL;
	pIB = NULL;
	Mtrl = NULL;
	Tex = NULL;
	Grp = NULL;

	MaxVert = MaxIdx = 0;

	nGrp = oapiMeshGroupCount(hMesh);
	if (nGrp == 0) return false;

	Grp = new SKETCHGRP[nGrp];
	memset(Grp, 0, sizeof(SKETCHGRP) * nGrp);

	// -----------------------------------------------------------------------

	nTex = oapiMeshTextureCount(hMesh) + 1;
	Tex = new SURFHANDLE[nTex];
	Tex[0] = 0; // 'no texture'
	for (DWORD i = 1; i < nTex; i++) Tex[i] = SURFACE(oapiGetTextureHandle(hMesh, i));

	// -----------------------------------------------------------------------

	nMtrl = oapiMeshMaterialCount(hMesh);
	if (nMtrl) Mtrl = new FVECTOR4[nMtrl];
	for (DWORD i = 0; i < nMtrl; i++) {
		MATERIAL* pMat = oapiMeshMaterial(hMesh, i);
		if (pMat) {
			Mtrl[i].r = pMat->diffuse.r;
			Mtrl[i].g = pMat->diffuse.g;
			Mtrl[i].b = pMat->diffuse.b;
			Mtrl[i].a = pMat->diffuse.a;
		}
	}

	// -----------------------------------------------------------------------

	for (DWORD i = 0; i < nGrp; i++) {
		MESHGROUPEX* pEx = oapiMeshGroupEx(hMesh, i);
		Grp[i].MtrlIdx = pEx->MtrlIdx;
		Grp[i].TexIdx = pEx->TexIdx;
		Grp[i].nVert = pEx->nVtx;
		Grp[i].nIdx = pEx->nIdx;
		Grp[i].VertOff = MaxVert;
		Grp[i].IdxOff = MaxIdx;
		MaxVert += pEx->nVtx;
		MaxIdx += pEx->nIdx;
	}

	if (MaxVert == 0 || MaxIdx == 0) return false;

	// -----------------------------------------------------------------------

	if (Grp[0].MtrlIdx == SPEC_INHERIT) Grp[0].MtrlIdx = SPEC_DEFAULT;
	if (Grp[0].TexIdx == SPEC_INHERIT) Grp[0].TexIdx = SPEC_DEFAULT;

	for (DWORD i = 0; i < nGrp; i++) {

		if (Grp[i].MtrlIdx == SPEC_INHERIT) Grp[i].MtrlIdx = Grp[i - 1].MtrlIdx;

		if (Grp[i].TexIdx == SPEC_DEFAULT) Grp[i].TexIdx = 0;
		else if (Grp[i].TexIdx == SPEC_INHERIT) Grp[i].TexIdx = Grp[i - 1].TexIdx;
		else Grp[i].TexIdx++;
	}

	// -----------------------------------------------------------------------
	//
	// Was CreateVertexBuffer/CreateIndexBuffer with D3DPOOL_DEFAULT and then a
	// Lock/memcpy/Unlock per group. The buffers are host-visible here so the
	// same fill works, but the mapping is done ONCE around the whole loop
	// rather than twice per group: D3D9's Lock took a byte range and could be
	// called repeatedly on a live buffer, while vkMapMemory maps the whole
	// allocation and nesting maps of one allocation is invalid. The bytes
	// written are identical -- each group still lands at its own offset.

	pVB = pDev->CreateBuffer(MaxVert * sizeof(NTVERTEX), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
	pIB = pDev->CreateBuffer(MaxIdx * sizeof(WORD), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);

	if (!pVB || !pIB) {
		LogErr("SketchMesh: failed to create vertex/index buffers");
		return false;
	}

	NTVERTEX* pVert = (NTVERTEX*)pVB->Map();
	WORD* pIndex = (WORD*)pIB->Map();

	if (!pVert || !pIndex) {
		if (pVert) pVB->Unmap();
		if (pIndex) pIB->Unmap();
		LogErr("SketchMesh: buffer memory is not host visible");
		return false;
	}

	for (DWORD i = 0; i < nGrp; i++) {
		MESHGROUPEX* pEx = oapiMeshGroupEx(hMesh, i);
		memcpy(pIndex + Grp[i].IdxOff, pEx->Idx, sizeof(WORD) * pEx->nIdx);
		memcpy(pVert + Grp[i].VertOff, pEx->Vtx, sizeof(NTVERTEX) * pEx->nVtx);
	}

	pVB->Unmap();
	pIB->Unmap();

	return true;
}


// ===============================================================================================
//
// Was SetVertexDeclaration + SetStreamSource + SetIndices, three pieces of
// device state. In Vulkan the declaration is baked into whichever pipeline the
// caller binds, so only the two buffer binds remain -- and they are commands
// recorded into the frame's command buffer rather than state set on a device.
//
void SketchMesh::Init()
{
	VkCommandBuffer cmd = pDev->GetCommandBuffer();
	if (!cmd || !pVB || !pIB) return;

	VkBuffer vb = pVB->Buffer();
	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
	vkCmdBindIndexBuffer(cmd, pIB->Buffer(), 0, VK_INDEX_TYPE_UINT16);
}


// ===============================================================================================
//
// DrawIndexedPrimitive(TRIANGLELIST, BaseVertexIndex, MinIndex, NumVertices,
//                      StartIndex, PrimitiveCount)
// becomes
// vkCmdDrawIndexed(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance).
//
// The two differ in what they count: D3D9 takes a PRIMITIVE count (nIdx/3 for
// a triangle list) while Vulkan takes an INDEX count, so the division is
// removed rather than carried over. MinIndex/NumVertices have no counterpart
// -- they were a hint to the software vertex processing path.
//
void SketchMesh::RenderGroup(DWORD idx)
{
	if (!pVB) return;
	VkCommandBuffer cmd = pDev->GetCommandBuffer();
	if (!cmd) return;
	vkCmdDrawIndexed(cmd, Grp[idx].nIdx, 1, Grp[idx].IdxOff, (int32_t)Grp[idx].VertOff, 0);
}


// ===============================================================================================
//
SURFHANDLE SketchMesh::GetTexture(DWORD idx)
{
	assert(idx < nGrp);
	if (Grp[idx].TexIdx) return Tex[Grp[idx].TexIdx];
	return NULL;
}


// ===============================================================================================
//
FVECTOR4 SketchMesh::GetMaterial(DWORD idx)
{
	assert(idx < nGrp);
	if (Grp[idx].MtrlIdx != SPEC_DEFAULT && Mtrl) return Mtrl[Grp[idx].MtrlIdx];
	return FVECTOR4(1, 1, 1, 1);
}


// ==============================================================================================
// ShaderClass
//
// Same class, same call sites, same job. What is underneath is where D3D9 and
// Vulkan differ most in this file, so each method carries its own note.
// ==============================================================================================

ShaderClass::ShaderClass(VulkanDevice *pDev, const char* file, const char* vs, const char* ps, const char *name, const char* options) :
	pPSCB(NULL), pVSCB(NULL), pPS(VK_NULL_HANDLE), pVS(VK_NULL_HANDLE),
	pDev(pDev), fn(file), psn(ps), vsn(vs), sn(name),
	topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST),	// see SetTopology
	cullMode(CULL_NONE),							// see SetCullMode
	// pLayout, pSetLayout, pPSConst and pVSConst WERE NOT INITIALISED HERE and
	// had to be: GetPipeline tests `if (!pLayout)` and Setup tests the two
	// constant buffers, so an indeterminate value there is a wild handle
	// passed to vkCreateGraphicsPipelines. Not a D3D9 difference and not the
	// reference's -- D3D9Effect's ShaderClass held D3D9 interface pointers
	// that its own constructor zeroed -- so it is corrected rather than
	// carried. pScratchVB/pScratchIB are the new pair; see DrawUP.
	pLayout(VK_NULL_HANDLE), pSetLayout(VK_NULL_HANDLE),
	pArena(NULL), arenaSize(0), arenaUsed(0), vsSlice(0), psSlice(0),
	bVSBlock(false), bPSBlock(false),
	pScratchVB(NULL), pScratchIB(NULL),
	scratchVBSize(0), scratchIBSize(0),
	vkPool(VK_NULL_HANDLE), vkSet(VK_NULL_HANDLE),
	vsBinding(0), psBinding(1), bResources(false), pWhite(NULL), pWhiteCube(NULL),
	frameNo(0), nPoolSamplers(0)
{
	for (size_t i = 0; i < ARRAYSIZE(pTextures); i++) pTextures[i] = {};
	pPS = CompilePixelShader(pDev, file, ps, name, options, &pPSCB);
	pVS = CompileVertexShader(pDev, file, vs, name, options, &pVSCB);
	Instances.insert(this);
}


// The instance registry. See ResetFrame().
std::set<ShaderClass*> ShaderClass::Instances;


// ===========================================================================================
// Release every instance's descriptor sets, once per frame.
//
// Counterpart of nothing: D3D9 consumed a SetTexture immediately, where a
// Vulkan draw records a reference to a set that has to stay alive until the
// frame is submitted. See the declaration.
// ===========================================================================================
void ShaderClass::ResetFrame()
{
	for (auto p : Instances) if (p && p->pDev) p->RotateFrame();
}


// ===========================================================================================
// One instance's frame boundary.
//
// ROTATES rather than resets, for the reason set out beside FrameSet in the
// header: this used to reset a pool and free buffers the GPU was still
// reading, which the layer reported and the driver eventually answered with
// VK_ERROR_DEVICE_LOST.
// ===========================================================================================
void ShaderClass::RotateFrame()
{
	frameNo++;

	unsigned long long lag = orbiter_GetFramesInFlight();
	if (lag < 2) lag = 2;

	// ---- retire what this frame used -----------------------------------
	if (vkPool != VK_NULL_HANDLE || pArena || pScratchVB || pScratchIB) {
		FrameSet used;
		used.pool			= vkPool;
		used.arena			= pArena;
		used.arenaSize		= arenaSize;
		used.scratchVB		= pScratchVB;
		used.scratchVBSize	= scratchVBSize;
		used.scratchIB		= pScratchIB;
		used.scratchIBSize	= scratchIBSize;
		used.retiredAt		= frameNo;
		retired.push_back(used);
	}

	vkPool = VK_NULL_HANDLE;	vkSet = VK_NULL_HANDLE;
	pArena = NULL;				arenaSize = 0;
	pScratchVB = NULL;			scratchVBSize = 0;
	pScratchIB = NULL;			scratchIBSize = 0;

	// ---- take the oldest back, once the GPU is certainly done with it ----
	if (!retired.empty() && retired.front().retiredAt + lag <= frameNo) {
		FrameSet f = retired.front();
		retired.pop_front();
		vkPool		= f.pool;
		pArena		= f.arena;		arenaSize		= f.arenaSize;
		pScratchVB	= f.scratchVB;	scratchVBSize	= f.scratchVBSize;
		pScratchIB	= f.scratchIB;	scratchIBSize	= f.scratchIBSize;
		if (vkPool != VK_NULL_HANDLE)
			vkResetDescriptorPool(pDev->GetDevice(), vkPool, 0);
	}
	else if (bResources) {
		// Only for the first `lag` frames after the resources exist; after
		// that the rotation is closed and nothing further is allocated.
		// pArena stays NULL -- EnsureArena grows one on the next draw, which
		// is how it has always been made.
		CreateFrameSet();
	}

	arenaUsed = 0;

	// Arenas a mid-frame grow replaced, on the same lag and for the same
	// reason -- a bind naming one of them may still be executing.
	while (!retiredArena.empty() && retiredArena.front().second + lag <= frameNo) {
		pDev->DestroyBuffer(retiredArena.front().first);
		retiredArena.pop_front();
	}

	// And the samplers UpdateTextures replaced, for the same reason again.
	while (!retiredSampler.empty() && retiredSampler.front().second + lag <= frameNo) {
		vkDestroySampler(pDev->GetDevice(), retiredSampler.front().first, nullptr);
		retiredSampler.pop_front();
	}
}


// ===========================================================================================
// One frame's descriptor pool.
//
// Split out of CreateResources unchanged so that the rotation can build
// another while the previous ones are still being read by the GPU. The arena
// is NOT made here: EnsureArena grows one on first use, which is how it has
// always been made.
// ===========================================================================================
bool ShaderClass::CreateFrameSet()
{
	if (!pDev) return false;

	const uint32_t maxSets = 4096;
	VkDescriptorPoolSize sizes[2] = {};
	sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;	// see the layout
	sizes[0].descriptorCount = maxSets * 2;
	sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	sizes[1].descriptorCount = maxSets * (nPoolSamplers ? nPoolSamplers : 1);

	VkDescriptorPoolCreateInfo pi = {};
	pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pi.maxSets = maxSets;
	pi.poolSizeCount = 2;
	pi.pPoolSizes = sizes;

	if (vkCreateDescriptorPool(pDev->GetDevice(), &pi, nullptr, &vkPool) != VK_SUCCESS) {
		LogErr("ShaderClass(%s): vkCreateDescriptorPool failed", sn.c_str());
		vkPool = VK_NULL_HANDLE;
		return false;
	}
	return true;
}


// ===========================================================================================
// The per-frame uniform arena, grown on demand and never shrunk. Same shape as
// EnsureScratch below and as VulkanEffectFile's arena -- see the header note.
//
// GROWING IT MID-FRAME WOULD FREE MEMORY THE GPU IS ABOUT TO READ, which is
// exactly what EnsureScratch got wrong before defect 11, so the old buffer is
// retired rather than destroyed and released at the next ResetFrame.
// ===========================================================================================
bool ShaderClass::EnsureArena(VkDeviceSize bytes)
{
	if (bytes <= arenaSize && pArena) return true;

	VkDeviceSize want = arenaSize ? arenaSize : (VkDeviceSize)(64 * 1024);
	while (want < bytes) want *= 2;

	VulkanBuffer *pNew = pDev->CreateBuffer(want, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
	if (!pNew) return false;

	if (pArena) retiredArena.push_back(std::make_pair(pArena, frameNo));
	pArena = pNew;
	arenaSize = want;
	return true;
}


// ===========================================================================================
// The scratch buffers DrawUP copies into, grown on demand. Identical to
// VulkanEffectFile::EnsureScratch, and deliberately so -- see DrawUP.
// ===========================================================================================
bool ShaderClass::EnsureScratch(VkDeviceSize vbytes, VkDeviceSize ibytes)
{
	// RETIRED, NOT DESTROYED -- this used to call DestroyBuffer at the point
	// of growth, which frees a buffer that draws already recorded in THIS
	// frame's command buffer still name. VulkanEffectFile::EnsureScratch
	// carries the full account of what that costs; this copy of the same
	// function had never been given the same correction.
	if (vbytes > scratchVBSize) {
		if (pScratchVB) retiredArena.push_back(std::make_pair(pScratchVB, frameNo));
		scratchVBSize = vbytes * 2;
		pScratchVB = pDev->CreateBuffer(scratchVBSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
		if (!pScratchVB) { scratchVBSize = 0; return false; }
	}
	if (ibytes > scratchIBSize) {
		if (pScratchIB) retiredArena.push_back(std::make_pair(pScratchIB, frameNo));
		scratchIBSize = ibytes * 2;
		pScratchIB = pDev->CreateBuffer(scratchIBSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
		if (!pScratchIB) { scratchIBSize = 0; return false; }
	}
	return true;
}


// ===========================================================================================
// DrawPrimitiveUP / DrawIndexedPrimitiveUP. See the note on the declaration.
// ===========================================================================================
void ShaderClass::DrawUP(const void *pVtx, UINT nVtx, UINT stride,
						 const WORD *pIdx, UINT nIdx)
{
	if (!pVtx || nVtx == 0 || stride == 0) return;
	if (!pDev || !pDev->IsRecording()) return;

	const VkDeviceSize vbytes = VkDeviceSize(nVtx) * stride;
	const VkDeviceSize ibytes = pIdx ? VkDeviceSize(nIdx) * sizeof(WORD) : 0;

	if (!EnsureScratch(vbytes, ibytes ? ibytes : 2)) return;

	if (void *p = pScratchVB->Map()) { memcpy(p, pVtx, (size_t)vbytes); pScratchVB->Unmap(); }
	if (pIdx && ibytes) {
		if (void *p = pScratchIB->Map()) { memcpy(p, pIdx, (size_t)ibytes); pScratchIB->Unmap(); }
	}

	VkCommandBuffer cmd = pDev->GetCommandBuffer();

	VkBuffer vb = pScratchVB->Buffer();
	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);

	if (pIdx && nIdx) {
		vkCmdBindIndexBuffer(cmd, pScratchIB->Buffer(), 0, VK_INDEX_TYPE_UINT16);
		vkCmdDrawIndexed(cmd, nIdx, 1, 0, 0, 0);
	}
	else {
		vkCmdDraw(cmd, nVtx, 1, 0, 0);
	}
}



ShaderClass::~ShaderClass()
{
	// Was SAFE_RELEASE x4. A VkShaderModule is destroyed against the device
	// that made it; the reflection tables are plain heap objects.
	if (pDev) {
		if (pPS) vkDestroyShaderModule(pDev->GetDevice(), pPS, nullptr);
		if (pVS) vkDestroyShaderModule(pDev->GetDevice(), pVS, nullptr);
		// The scratch buffers DrawUP grew, and the pipeline objects this
		// class created. None of them existed in D3D9: the first is what
		// replaces DrawPrimitiveUP's hidden staging, and the other two are
		// pipeline objects D3D9 had no equivalent of at all.
		if (pScratchVB) { pDev->DestroyBuffer(pScratchVB); pScratchVB = NULL; }
		if (pScratchIB) { pDev->DestroyBuffer(pScratchIB); pScratchIB = NULL; }
		for (auto &x : Pipelines) if (x.second) vkDestroyPipeline(pDev->GetDevice(), x.second, nullptr);
		Pipelines.clear();
		if (pLayout) { vkDestroyPipelineLayout(pDev->GetDevice(), pLayout, nullptr); pLayout = VK_NULL_HANDLE; }
		if (pSetLayout) { vkDestroyDescriptorSetLayout(pDev->GetDevice(), pSetLayout, nullptr); pSetLayout = VK_NULL_HANDLE; }
		if (pArena) { pDev->DestroyBuffer(pArena); pArena = NULL; }
		for (auto &q : retiredArena) pDev->DestroyBuffer(q.first);
		retiredArena.clear();
		arenaSize = arenaUsed = 0;
		if (vkPool) { vkDestroyDescriptorPool(pDev->GetDevice(), vkPool, nullptr); vkPool = VK_NULL_HANDLE; }
		// And everything still in the rotation. The session is over and the
		// caller has waited for the device, so all of it is idle.
		for (FrameSet &f : retired) {
			if (f.pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(pDev->GetDevice(), f.pool, nullptr);
			if (f.arena) pDev->DestroyBuffer(f.arena);
			if (f.scratchVB) pDev->DestroyBuffer(f.scratchVB);
			if (f.scratchIB) pDev->DestroyBuffer(f.scratchIB);
		}
		retired.clear();
		for (auto &s : retiredSampler) vkDestroySampler(pDev->GetDevice(), s.first, nullptr);
		retiredSampler.clear();
		if (pWhite) { pDev->DestroyTexture(pWhite); pWhite = NULL; }
		if (pWhiteCube) { pDev->DestroyTexture(pWhiteCube); pWhiteCube = NULL; }
		for (size_t i = 0; i < ARRAYSIZE(pTextures); i++) {
			if (pTextures[i].pSampler) {
				vkDestroySampler(pDev->GetDevice(), pTextures[i].pSampler, nullptr);
				pTextures[i].pSampler = VK_NULL_HANDLE;
			}
		}
	}
	pPS = pVS = VK_NULL_HANDLE;
	SAFE_DELETE(pPSCB);
	SAFE_DELETE(pVSCB);
	Instances.erase(this);
}


// ===========================================================================================
// The descriptor set layout, the pipeline layout, the pool and the 1x1 white
// default. Built once, on the first UpdateTextures().
//
// See the note on the members in VulkanUtil.h for why none of this has a D3D9
// counterpart, and for the binding convention: VS block 0, PS block 1,
// samplers at whatever binding the GLSL declares.
// ===========================================================================================
bool ShaderClass::CreateResources()
{
	if (bResources) return pSetLayout != VK_NULL_HANDLE;
	bResources = true;

	if (!pDev) return false;

	VkDevice dev = pDev->GetDevice();

	std::vector<VkDescriptorSetLayoutBinding> binds;
	uint32_t nSampler = 0;

	// The two uniform blocks. A stage with no constants declares none, and
	// then there is no binding for it -- which is why the bindings are read
	// from reflection rather than laid out by rule.
	bool bVS = false, bPS = false;
	if (pVSCB) for (auto &v : pVSCB->Vars()) if (!v.bSampler) { vsBinding = v.binding; bVS = true; break; }
	if (pPSCB) for (auto &v : pPSCB->Vars()) if (!v.bSampler) { psBinding = v.binding; bPS = true; break; }

	// Recorded for BindResources: the layout's shape, not this draw's.
	bVSBlock = bVS;
	bPSBlock = bPS;

	// DYNAMIC, so one descriptor set can serve every draw in the frame with
	// each draw's own slice of the uniform arena supplied at bind time. See
	// the note on vsBlock in the header for why a plain UNIFORM_BUFFER --
	// which is what this was -- gave every draw the last draw's constants.
	if (bVS) {
		VkDescriptorSetLayoutBinding b = {};
		b.binding = vsBinding;
		b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		b.descriptorCount = 1;
		b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
		binds.push_back(b);
	}
	if (bPS) {
		VkDescriptorSetLayoutBinding b = {};
		b.binding = psBinding;
		b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		b.descriptorCount = 1;
		b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		binds.push_back(b);
	}

	// The samplers, from both stages. A vertex-stage sampler is a real thing
	// here -- SetTextureVS exists because D3D9 had vertex texture fetch on
	// slots 0..3 (D3DVERTEXTEXTURESAMPLER0), and Vulkan simply lets the
	// vertex stage sample like any other.
	auto addSamplers = [&](ShaderReflection *pCB, VkShaderStageFlagBits stage) {
		if (!pCB) return;
		for (auto &v : pCB->Vars()) {
			if (!v.bSampler) continue;
			bool bHave = false;
			for (auto &b : binds) if (b.binding == v.binding) { bHave = true; break; }
			if (bHave) continue;
			VkDescriptorSetLayoutBinding b = {};
			b.binding = v.binding;
			b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			b.descriptorCount = 1;
			b.stageFlags = stage;
			binds.push_back(b);
			nSampler++;
		}
	};
	addSamplers(pPSCB, VK_SHADER_STAGE_FRAGMENT_BIT);
	addSamplers(pVSCB, VK_SHADER_STAGE_VERTEX_BIT);

	VkDescriptorSetLayoutCreateInfo dsl = {};
	dsl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	dsl.bindingCount = (uint32_t)binds.size();
	dsl.pBindings = binds.empty() ? NULL : binds.data();

	if (vkCreateDescriptorSetLayout(dev, &dsl, nullptr, &pSetLayout) != VK_SUCCESS) {
		LogErr("ShaderClass(%s): vkCreateDescriptorSetLayout failed", sn.c_str());
		pSetLayout = VK_NULL_HANDLE;
		return false;
	}

	// THE POOL IS SIZED FOR A FRAME, NOT FOR A DRAW. The tile engines are the
	// heavy users: one set per patch per frame, and a busy planet is a few
	// hundred patches. Recycled once per frame by ResetFrame() -- through the
	// rotation, which needs to be able to make more of these, so the making
	// lives in CreateFrameSet and the sampler count it sized by is kept.
	nPoolSamplers = nSampler;
	if (!CreateFrameSet()) return false;

	// Every binding in the layout must be written before the set is used, and
	// an unset D3D9 sampler stage sampled white. Same decision, same 1x1
	// texture, as VulkanEffectFile's and IProcess's.
	if (nSampler) {
		const DWORD white = 0xFFFFFFFF;

		pWhite = pDev->CreateTexture(1, 1, 1, VK_FORMAT_B8G8R8A8_UNORM,
									 VK_IMAGE_USAGE_SAMPLED_BIT |
									 VK_IMAGE_USAGE_TRANSFER_DST_BIT |
									 VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
		if (pWhite) pDev->UploadTexture(pWhite, 0, 0, &white, sizeof(white));

		// AND A WHITE CUBE, because a cube sampler cannot be given a 2D view.
		//
		// The fallback above is a VK_IMAGE_VIEW_TYPE_2D view. Binding it to a
		// samplerCube -- which is what an unset EnvMapAS slot did -- is
		//
		//     VUID-vkCmdDrawIndexed-viewType-07752
		//     ... "EnvMapAS" VkImageViewType is VK_IMAGE_VIEW_TYPE_2D but the
		//     OpTypeImage has (Dim = Cube)
		//
		// and the GPU faults on it: VK_ERROR_DEVICE_LOST at vkQueueSubmit,
		// with no line of the client's own code to point at. D3D9 had no such
		// rule -- SetTexture took any IDirect3DBaseTexture9 and the runtime
		// reconciled it with the sampler declaration -- so the reference has
		// nothing corresponding to this and one fallback sufficed there.
		//
		// Only built when the shader actually declares a cube sampler.
		bool bAnyCube = false;
		if (pPSCB) for (auto &v : pPSCB->Vars()) if (v.bSampler && v.bCube) { bAnyCube = true; break; }
		if (!bAnyCube && pVSCB)
			for (auto &v : pVSCB->Vars()) if (v.bSampler && v.bCube) { bAnyCube = true; break; }

		if (bAnyCube) {
			pWhiteCube = pDev->CreateTextureCube(1, 1, VK_FORMAT_B8G8R8A8_UNORM,
												 VK_IMAGE_USAGE_SAMPLED_BIT |
												 VK_IMAGE_USAGE_TRANSFER_DST_BIT |
												 VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
			if (pWhiteCube)
				for (uint32_t f = 0; f < 6; f++)
					pDev->UploadTexture(pWhiteCube, 0, f, &white, sizeof(white));
		}
	}

	return true;
}


// ===========================================================================================
// Allocate a set, write it, bind it.
//
// Called from UpdateTextures(), which is where every call site in the client
// already says "what I have set should now reach the device" -- so no call
// site changes. Its D3D9 body did exactly that for textures and samplers; the
// uniform blocks join it because a Vulkan draw takes both through one set.
// ===========================================================================================
bool ShaderClass::BindResources()
{
	// EVERY ONE OF THESE WAS A SILENT `return false`, AND A DRAW WITH NO
	// DESCRIPTOR SET BOUND IS A DRAW THAT READS NOTHING. Reported once per
	// shader per reason, for the same rationale as Setup's null pipeline.
	auto bail = [&](const char *why) -> bool {
		static std::map<std::string, int> seen;
		const std::string key = sn + "/" + why;
		if (seen.find(key) == seen.end()) {
			seen[key] = 1;
			LogErr("ShaderClass(%s)::BindResources: %s -- no descriptor set bound",
				   sn.c_str(), why);
		}
		return false;
	};

	if (!pDev) return false;
	if (pSetLayout == VK_NULL_HANDLE) return bail("no descriptor set layout");
	if (vkPool == VK_NULL_HANDLE)     return bail("no descriptor pool");
	if (!pLayout)					  return bail("no pipeline layout (Setup has not run)");

	VkCommandBuffer cmd = pDev->GetCommandBuffer();
	if (!cmd) return bail("no command buffer");

	VkDescriptorSetAllocateInfo ai = {};
	ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	ai.descriptorPool = vkPool;
	ai.descriptorSetCount = 1;
	ai.pSetLayouts = &pSetLayout;

	if (vkAllocateDescriptorSets(pDev->GetDevice(), &ai, &vkSet) != VK_SUCCESS) {
		LogErr("ShaderClass(%s): descriptor pool exhausted this frame -- is "
			   "ShaderClass::ResetFrame() being called?", sn.c_str());
		return false;
	}

	std::vector<VkWriteDescriptorSet> writes;
	VkDescriptorBufferInfo bufs[2] = {};
	VkDescriptorImageInfo imgs[ARRAYSIZE(pTextures)] = {};

	// COMMIT EACH STAGE'S BLOCK TO ITS OWN SLICE OF THE FRAME'S ARENA.
	//
	// This is the half that makes per-draw constants actually per-draw. The
	// slice is bound with a DYNAMIC offset, so the descriptor set itself
	// carries no offset and the same layout serves every draw. See the note
	// on vsBlock in the header for why one shared buffer was wrong.
	const VkDeviceSize align =
		std::max<VkDeviceSize>(pDev->GetProperties()->limits.minUniformBufferOffsetAlignment, 1);

	// A block the layout DECLARES always gets a slice, even if this draw wrote
	// nothing into it: the descriptor must be written and the dynamic-offset
	// count must match the layout, or the bind is invalid.
	auto commit = [&](std::vector<char> &blk, ShaderReflection *pCB, uint32_t &slice) -> bool {
		size_t bytes = blk.size();
		if (pCB && pCB->BlockSize() > bytes) bytes = pCB->BlockSize();
		if (!bytes) bytes = 16;					// a legal, if unused, range
		if (blk.size() < bytes) blk.resize(bytes, 0);

		const VkDeviceSize at = ((arenaUsed + align - 1) / align) * align;
		if (!EnsureArena(at + bytes)) return false;
		char *p = (char *)pArena->Map();
		if (!p) return false;
		memcpy(p + at, blk.data(), bytes);
		pArena->Unmap();
		slice = (uint32_t)at;
		arenaUsed = at + bytes;
		return true;
	};

	vsSlice = psSlice = 0;
	const bool bVS = bVSBlock && commit(vsBlock, pVSCB, vsSlice);
	const bool bPS = bPSBlock && commit(psBlock, pPSCB, psSlice);

	// A dynamic uniform binding's range is the SLICE, not the buffer: the
	// offset is supplied at bind time and added to this one.
	auto addBuffer = [&](bool bHave, size_t bytes, uint32_t binding, int slot) {
		if (!bHave || !pArena) return;
		bufs[slot].buffer = pArena->Buffer();
		bufs[slot].offset = 0;
		bufs[slot].range = bytes;
		VkWriteDescriptorSet w = {};
		w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w.dstSet = vkSet;
		w.dstBinding = binding;
		w.descriptorCount = 1;
		w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		w.pBufferInfo = &bufs[slot];
		writes.push_back(w);
	};
	addBuffer(bVS, vsBlock.size(), vsBinding, 0);
	addBuffer(bPS, psBlock.size(), psBinding, 1);

	uint32_t nImg = 0;
	const size_t imgBase = writes.size();

	// The pixel stage's samplers index pTextures[0..15] and the vertex
	// stage's pTextures[16..19] -- the +16 SetTextureVS applies, which is how
	// D3D9's separate vertex sampler slots are kept apart from the pixel
	// ones. Both walks skip a binding another has already claimed.
	auto writeSamplers = [&](ShaderReflection *pCB, uint32_t base) {
		if (!pCB) return;
		for (auto &v : pCB->Vars()) {
			if (!v.bSampler) continue;
			bool bDone = false;
			for (size_t k = 0; k < nImg; k++)
				if (writes[imgBase + k].dstBinding == v.binding) { bDone = true; break; }
			if (bDone || nImg >= ARRAYSIZE(imgs)) continue;

			const uint32_t idx = v.samplerIndex + base;
			VulkanTexture *pTex = NULL;
			if (idx < ARRAYSIZE(pTextures) && pTextures[idx].pTex && pTextures[idx].pSampler) {
				pTex = pTextures[idx].pTex;
				imgs[nImg].sampler = pTextures[idx].pSampler;
			}
			if (!pTex) {
				// The fallback has to match the sampler's declared dimension:
				// a 2D view in a samplerCube is a GPU fault, not a wrong
				// colour. See Var::bCube and CreateResources.
				pTex = (v.bCube && pWhiteCube) ? pWhiteCube : pWhite;
				if (!pTextures[0].pSampler) {
					// A slot that has never been set has no sampler; build the
					// default one so the white binding is writable.
					pTextures[0].Flags = IPF_CLAMP | IPF_POINT;
					pTextures[0].bSamplerSet = false;
					pTextures[0].pTex = pWhite;
					UpdateTextures();
					pTextures[0].pTex = NULL;
				}
				imgs[nImg].sampler = pTextures[0].pSampler;
			}
			if (!pTex || pTex->View() == VK_NULL_HANDLE || !imgs[nImg].sampler) continue;

			imgs[nImg].imageView = pTex->View();
			imgs[nImg].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

			VkWriteDescriptorSet w = {};
			w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			w.dstSet = vkSet;
			w.dstBinding = v.binding;
			w.descriptorCount = 1;
			w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			w.pImageInfo = &imgs[nImg];
			writes.push_back(w);
			nImg++;
		}
	};
	writeSamplers(pPSCB, 0);
	writeSamplers(pVSCB, 16);

	if (!writes.empty())
		vkUpdateDescriptorSets(pDev->GetDevice(), (uint32_t)writes.size(), writes.data(), 0, nullptr);

	// THE DYNAMIC OFFSETS, IN BINDING ORDER. vkCmdBindDescriptorSets wants one
	// per dynamic descriptor in the set, ordered by binding number -- not in
	// the order the writes were pushed. vsBinding is 0 and psBinding is 1 by
	// the convention CreateResources sets up, but the sort makes that explicit
	// rather than assumed.
	uint32_t dyn[2] = {};
	uint32_t nDyn = 0;
	if (bVS && bPS) {
		if (vsBinding <= psBinding) { dyn[0] = vsSlice; dyn[1] = psSlice; }
		else						{ dyn[0] = psSlice; dyn[1] = vsSlice; }
		nDyn = 2;
	}
	else if (bVS) { dyn[0] = vsSlice; nDyn = 1; }
	else if (bPS) { dyn[0] = psSlice; nDyn = 1; }

	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pLayout, 0, 1, &vkSet,
							nDyn, nDyn ? dyn : nullptr);
	return true;
}


void ShaderClass::ClearTextures()
{
	for (size_t idx = 0; idx < ARRAYSIZE(pTextures); idx++)
	{
		pTextures[idx].pAssigned = NULL;
		pTextures[idx].pTex = NULL;
		pTextures[idx].bSamplerSet = false;
	}
}


// ----------------------------------------------------------------------------------------------
// Was sixteen SetTexture(i, NULL) plus four for the vertex samplers.
//
// There is no "unbind a texture" in Vulkan: a descriptor set is written, and a
// draw either uses it or does not. Detaching is therefore a matter of
// forgetting the bindings so the next UpdateTextures() writes a fresh set --
// which is exactly what ClearTextures() already does. The twenty device calls
// have no counterpart and nothing replaces them.
//
void ShaderClass::DetachTextures()
{
	ClearTextures();
}


// ----------------------------------------------------------------------------------------------
// Was: for each of twenty slots, if the sampler state has not been set, issue
// eight SetSamplerState calls (ADDRESSU/V/W, SRGBTEXTURE, MAXANISOTROPY,
// MAGFILTER, MINFILTER, MIPFILTER); then, if the texture changed, SetTexture.
//
// Vulkan has no sampler state on the device. A sampler is an OBJECT created
// from the same parameters, and a texture reaches a shader as a descriptor
// written into a set. So the eight state calls become the fields of a
// VkSamplerCreateInfo, and the whole loop becomes: make sure each slot has a
// sampler matching its IPF_ flags, then write one descriptor set.
//
// The IPF_ flag decoding below is carried over unchanged, including the order
// of the filter tests -- LINEAR, then PYRAMIDAL, then GAUSSIAN, then
// ANISOTROPIC, each overriding the last -- because that order is what decides
// the result when a caller passes more than one.
//
// D3DTEXF_PYRAMIDALQUAD and D3DTEXF_GAUSSIANQUAD HAVE NO COUNTERPART. They
// were D3D9 texture-filter modes that no PC driver has ever implemented (the
// caps bit is absent on every desktop part), so IPF_PYRAMIDAL and IPF_GAUSSIAN
// select linear filtering here, which is what the D3D9 runtime fell back to.
//
void ShaderClass::UpdateTextures()
{
	if (!pDev) return;

	for (size_t idx = 0; idx < ARRAYSIZE(pTextures); idx++)
	{
		if (pTextures[idx].pTex == NULL) continue;

		if (!pTextures[idx].bSamplerSet)
		{
			pTextures[idx].bSamplerSet = true;
			DWORD flags = pTextures[idx].Flags;

			VkSamplerCreateInfo si = {};
			si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;

			if (flags & IPF_CLAMP_U)		si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			else if (flags & IPF_MIRROR_U)	si.addressModeU = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
			else							si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;

			if (flags & IPF_CLAMP_V)		si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			else if (flags & IPF_MIRROR_V)	si.addressModeV = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
			else							si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;

			if (flags & IPF_CLAMP_W)		si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			else if (flags & IPF_MIRROR_W)	si.addressModeW = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
			else							si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;

			VkFilter filter = VK_FILTER_NEAREST;			// D3DTEXF_POINT

			if (flags & IPF_LINEAR) filter = VK_FILTER_LINEAR;
			if (flags & IPF_PYRAMIDAL) filter = VK_FILTER_LINEAR;	// see note above
			if (flags & IPF_GAUSSIAN) filter = VK_FILTER_LINEAR;	// see note above
			if (flags & IPF_ANISOTROPIC) filter = VK_FILTER_LINEAR;

			si.magFilter = filter;
			si.minFilter = filter;

			// D3DSAMP_MAXANISOTROPY plus D3DTEXF_ANISOTROPIC becomes one pair
			// of fields. Anisotropy is also a device feature here, which D3D9
			// expressed as a cap; asking for it when it is not enabled is a
			// validation error rather than a silent fallback.
			bool bAniso = (flags & IPF_ANISOTROPIC) && pTextures[idx].AnisoLvl > 1
						  && pDev->GetFeatures()->samplerAnisotropy;
			si.anisotropyEnable = bAniso ? VK_TRUE : VK_FALSE;
			si.maxAnisotropy = bAniso
				? std::min(float(pTextures[idx].AnisoLvl), pDev->GetProperties()->limits.maxSamplerAnisotropy)
				: 1.0f;

			// The Windows code sets MIPFILTER LINEAR for the sixteen pixel
			// samplers and POINT for the four vertex ones, because vertex
			// texture fetch had no mip selection. Vulkan applies the same
			// mipmapMode everywhere, and the vertex stage can sample normally,
			// so the split is gone -- but the intent is kept: a vertex-stage
			// sampler still reads one level.
			bool bVertexTex = (flags & IPF_VERTEXTEX) != 0;
			si.mipmapMode = bVertexTex ? VK_SAMPLER_MIPMAP_MODE_NEAREST
									   : VK_SAMPLER_MIPMAP_MODE_LINEAR;
			si.minLod = 0.0f;
			si.maxLod = bVertexTex ? 0.0f : VK_LOD_CLAMP_NONE;

			// D3DSAMP_SRGBTEXTURE was set false. sRGB is a property of the
			// image VIEW's format in Vulkan, not of the sampler, so there is
			// nothing to switch off here -- the views are created with UNORM
			// formats, which is the same decision made one level up.

			// RETIRED, NOT DESTROYED. This replaces the sampler whenever the
			// requested filtering changes, which happens DURING a frame --
			// and a draw already recorded this frame (or in a frame still in
			// flight) holds a descriptor set naming the old one:
			//
			//     VUID-vkDestroySampler-sampler-01082
			//     vkDestroySampler(): can't be called on VkSampler ... that
			//     is currently in use by a command buffer.
			//
			// D3D9 had no object here at all -- D3DSAMP_MINFILTER was a
			// device state word, overwritten in place -- so there is nothing
			// in the reference to convert; this is the lifetime Vulkan adds.
			// Freed on the same lag as the pool rotation. See RotateFrame.
			if (pTextures[idx].pSampler) {
				retiredSampler.push_back(std::make_pair(pTextures[idx].pSampler, frameNo));
				pTextures[idx].pSampler = VK_NULL_HANDLE;
			}
			HR(vkCreateSampler(pDev->GetDevice(), &si, nullptr, &pTextures[idx].pSampler));
		}

		if (pTextures[idx].pTex != pTextures[idx].pAssigned)
		{
			pTextures[idx].pAssigned = pTextures[idx].pTex;
		}
	}

	// AND THEN THE DESCRIPTOR SET, WHICH IS THE OTHER HALF OF WHAT THIS
	// FUNCTION MEANT ON WINDOWS.
	//
	// The D3D9 body ended with SetTexture(idx, tex) per slot -- "what I have
	// set should now reach the device". A texture reaches a Vulkan shader
	// only through a descriptor set, and so do the uniform blocks, so both go
	// in one set written here. Every call site in the client already calls
	// UpdateTextures() immediately before its draw, after setting its
	// constants and textures, so nothing above this line had to move.
	//
	// The recursion guard matters: BindResources falls back to the 1x1 white
	// texture for an unwritten binding and calls back into UpdateTextures()
	// to build slot 0's sampler when it has never been used.
	static bool bInBind = false;
	if (!bInBind) {
		bInBind = true;
		if (CreateResources()) BindResources();
		bInBind = false;
	}
}


// ----------------------------------------------------------------------------------------------
// Was: read the render target's size, set a viewport, set the two shaders, set
// the vertex declaration, then eleven SetRenderState calls selecting depth,
// blending and the blend equation.
//
// ALL OF THAT EXCEPT THE VIEWPORT IS PIPELINE STATE IN VULKAN, fixed when the
// VkPipeline is built. Which means Setup()'s three arguments -- the vertex
// declaration, the depth flag and the blend mode -- ARE the pipeline's key,
// and nothing else about the call varies. So this looks the pipeline up, and
// builds it on a miss.
//
// The viewport survives as a call because it is one of the few things Vulkan
// keeps dynamic. Its size comes from the frame rather than from
// GetRenderTarget(0)->GetDesc(): the client does not own the render target.
//
// The four blend modes are carried over exactly:
//   1  ADD, SRCALPHA, INVSRCALPHA      2  ADD, ONE, INVSRCALPHA
//   3  MAX, ONE, ONE                   4  ADD, SRCALPHA, ONE
//
void ShaderClass::Setup(const VertexDecl *pDecl, bool bZ, int blend)
{
	if (!pDev) return;

	// Bisection aid: ORBITER_VK_NODEPTH=1 takes the depth test out of the
	// picture so "is the geometry being rejected by depth?" can be answered
	// by looking rather than by reasoning. Diagnostic only.
	static const bool bNoDepth = (getenv("ORBITER_VK_NODEPTH") != NULL);
	if (bNoDepth) bZ = false;

	VkCommandBuffer cmd = pDev->GetCommandBuffer();
	if (!cmd) {
		LogErr("ShaderClass::Setup outside a frame -- no command buffer");
		return;
	}

	// Which ShaderClass shaders ever actually run, reported once each.
	if (getenv("ORBITER_VK_TRACE_TECH")) {
		static std::map<std::string, int> seen;
		if (seen.find(sn) == seen.end()) {
			seen[sn] = 1;
			LogErr("TECHTRACE shaderclass: %s", sn.c_str());
		}
	}

	// Name this shader in the GPU command stream; see orbiter_VkCheckpoint.
	// `sn` is this object's own std::string, so the pointer outlives the
	// submission. No-op unless ORBITER_VK_CHECKPOINTS is set.
	orbiter_VkCheckpoint(cmd, sn.c_str());

	// THE HEIGHT IS NEGATIVE, AND THAT IS THE Y AXIS OF CLIP SPACE.
	//
	// Scene::SetCameraAperture builds the projection matrix with exactly the
	// reference's numbers, and its note says Vulkan clip space matches
	// D3D9's. That is true of the DEPTH range -- both are 0..1, unlike
	// OpenGL's -1..1 -- and false of Y: D3D9 NDC has +Y UP, Vulkan has +Y
	// DOWN. An unaltered D3D9 projection therefore draws the scene upside
	// down, and that is not the half that hurts:
	//
	// FLIPPING Y ALSO REVERSES TRIANGLE WINDING IN SCREEN SPACE, so every
	// front face is presented to the rasteriser as a back face and
	// VK_CULL_MODE_BACK_BIT throws the entire solid scene away. What survived
	// was exactly what culling does not apply to -- the star points and the
	// constellation labels -- which is why the window came up black with
	// stars in it and the labels read mirrored.
	//
	// The fix belongs HERE rather than in the matrix: negating mProj.m22 would
	// flip Y too, but the projection should stay byte-identical to the
	// reference's, and the cull table below is a faithful conversion of
	// D3D9's. A negative-height viewport is core since Vulkan 1.1 and this
	// client declares 1.2.
	//
	// CAUTION, UNRESOLVED: an earlier version of this note claimed the
	// viewport flip happens AFTER winding is determined, so that D3DCULL_CW
	// still means what it meant. That is probably WRONG -- the spec derives a
	// triangle's orientation from the sign of its area in FRAMEBUFFER
	// coordinates, which is after the viewport transform, so a negative
	// height should invert the effective winding exactly as negating m22
	// would. If so, the cull table below needs its two frontFace values
	// swapped.
	//
	// It has not been possible to confirm that by experiment yet: forcing
	// VK_CULL_MODE_NONE everywhere (ORBITER_VK_NOCULL=1) does NOT make the
	// planet appear, so culling is not what is currently hiding it and the
	// question is still open. Do not "fix" the cull table on the strength of
	// the argument alone -- settle it with a two-triangle test, one of each
	// winding, before touching a table that matches the reference.
	VkViewport vp = {};
	vp.x = 0.0f;
	vp.y = float(pDev->GetFrameHeight());
	vp.width = float(pDev->GetFrameWidth());
	vp.height = -float(pDev->GetFrameHeight());
	vp.minDepth = 0.0f;
	vp.maxDepth = 1.0f;
	vkCmdSetViewport(cmd, 0, 1, &vp);

	VkRect2D sc = {};
	sc.offset = { 0, 0 };
	sc.extent = { pDev->GetFrameWidth(), pDev->GetFrameHeight() };
	vkCmdSetScissor(cmd, 0, 1, &sc);

	VkPipeline pipe = GetPipeline(pDecl, bZ, blend);
	if (pipe) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	else {
		// A NULL PIPELINE MUST NOT BE SILENT. Binding nothing leaves whatever
		// the previous Setup bound in place, so the draw goes ahead with
		// another shader's pipeline and the caller's geometry quietly does not
		// appear. D3D9 had no equivalent failure -- SetVertexShader/
		// SetPixelShader either worked or returned an error the caller saw.
		static std::map<std::string, int> seen;
		if (seen.find(sn) == seen.end()) {
			seen[sn] = 1;
			LogErr("ShaderClass(%s)::Setup: no pipeline (decl=%s bZ=%d blend=%d) -- "
				   "nothing was bound and this shader's draws will use whatever "
				   "pipeline was bound before them",
				   sn.c_str(), pDecl ? pDecl->Name() : "(null)", int(bZ), blend);
		}
	}
}


// A NULL HANDLE HERE IS SILENT, AND THAT IS WHY IT IS REPORTED.
//
// SetPSConstants/SetVSConstants/SetTexture all begin `if (!v) return;` -- a
// constant the shader does not declare is written nowhere and nothing says so.
// The reference had the same shape (its assert sits behind SHDCLSDBG, which is
// off), and on D3D9 it mattered less: an unset constant register kept whatever
// was there. Here an unwritten uniform block member is simply never uploaded,
// so a renamed or missing constant is a matrix of zeroes and the draw produces
// nothing at all -- with no error anywhere.
//
// VulkanEffectFile::GetParameterByName already logs exactly this for the
// effect file's parameters, and the same reasoning applies to ShaderClass,
// which is what draws the planet surface, the clouds and the celestial sphere.
// Reported once per name so a per-frame lookup does not flood the log.
static void ReportMissingConstant(const char *what, const std::string &shader, const char *name)
{
	static std::map<std::string, int> seen;
	const std::string key = shader + "/" + name;
	if (seen.find(key) != seen.end()) return;
	seen[key] = 1;
	LogWrn("ShaderClass(%s): no %s constant named '%s'", shader.c_str(), what, name);
}

HANDLE ShaderClass::GetPSHandle(const char* name)
{
	if (!pPSCB) return NULL;
	const ShaderReflection::Var *v = pPSCB->GetConstantByName(name);
	if (!v) ReportMissingConstant("pixel-shader", sn, name);
	return HANDLE(const_cast<ShaderReflection::Var*>(v));
}


HANDLE ShaderClass::GetVSHandle(const char* name)
{
	if (!pVSCB) return NULL;
	const ShaderReflection::Var *v = pVSCB->GetConstantByName(name);
	if (!v) ReportMissingConstant("vertex-shader", sn, name);
	return HANDLE(const_cast<ShaderReflection::Var*>(v));
}



void ShaderClass::SetTexture(const char* name, VulkanTexture *pTex, UINT flags, UINT aniso)
{
	SetTexture(GetPSHandle(name), pTex, flags, aniso);
}


void ShaderClass::SetTextureVS(const char* name, VulkanTexture *pTex, UINT flags, UINT aniso)
{
	SetTextureVS(GetVSHandle(name), pTex, flags, aniso);
}

void ShaderClass::SetPSConstants(const char* name, void* data, UINT bytes)
{
	SetPSConstants(GetPSHandle(name), data, bytes);
}

void ShaderClass::SetVSConstants(const char* name, void* data, UINT bytes)
{
	SetVSConstants(GetVSHandle(name), data, bytes);
}
	


void ShaderClass::SetTexture(HANDLE hVar, VulkanTexture *pTex, UINT flags, UINT aniso)
{
	const ShaderReflection::Var *v = (const ShaderReflection::Var*)hVar;
#ifdef SHDCLSDBG
	if (!v) {
		LogErr("Shader::SetTexture() Invalid handle. File[%s], Entrypoint[%s], Shader[%s]", fn.c_str(), psn.c_str(), sn.c_str());
		assert(false);
	}
#endif
	if (!v) return;
	DWORD idx = pPSCB ? pPSCB->GetSamplerIndex(v) : 0;
	if (idx >= ARRAYSIZE(pTextures)) return;

	if (!pTex) {
		pTextures[idx].pTex = NULL;
		return;
	}

	if (pTextures[idx].Flags != flags) pTextures[idx].bSamplerSet = false;
	if (pTextures[idx].AnisoLvl != aniso) pTextures[idx].bSamplerSet = false;

	pTextures[idx].pTex = pTex;
	pTextures[idx].Flags = flags;
	pTextures[idx].AnisoLvl = aniso;
}


void ShaderClass::SetTextureVS(HANDLE hVar, VulkanTexture *pTex, UINT flags, UINT aniso)
{
	const ShaderReflection::Var *v = (const ShaderReflection::Var*)hVar;
#ifdef SHDCLSDBG
	if (!v) {
		LogErr("Shader::SetTextureVS() Invalid handle. File[%s], Entrypoint[%s], Shader[%s]", fn.c_str(), vsn.c_str(), sn.c_str());
		assert(false);
	}
#endif
	if (!v) return;
	DWORD idx = (pVSCB ? pVSCB->GetSamplerIndex(v) : 0) + 16;
	assert(idx < 20);
	if (idx >= ARRAYSIZE(pTextures)) return;

	if (!pTex) {
		pTextures[idx].pTex = NULL;
		return;
	}

	if (pTextures[idx].Flags != flags) pTextures[idx].bSamplerSet = false;
	if (pTextures[idx].AnisoLvl != aniso) pTextures[idx].bSamplerSet = false;

	pTextures[idx].pTex = pTex;
	pTextures[idx].Flags = flags | IPF_VERTEXTEX;
	pTextures[idx].AnisoLvl = aniso;
}


// ----------------------------------------------------------------------------------------------
// THE WINDOWS BODY OF THIS FUNCTION WRITES THROUGH pVSCB.
//
//     if (pVSCB->SetValue(pDev, D3DXHANDLE(hVar), data, bytes) != S_OK)
//         LogErr("Shader::SetPSConstants() Failed. File[%s], Entrypoint[%s]",
//                fn.c_str(), vsn.c_str());
//
// The handle comes from GetPSHandle(), i.e. from pPSCB, and the const char*
// overload beside it correctly uses pPSCB -- so this is a copy-paste slip, not
// a deliberate aliasing. Its error message names vsn as well, which is how it
// stayed invisible: when it failed it complained about the vertex shader.
//
// It is live code. VPlanetAtmo.cpp takes eight PS handles (tCloud, tCloud2,
// tMask, tDiff, tShadowMap, Prm, Flow, Lights, Spotlight) and Mesh.h three
// more, and sets through them.
//
// In Vulkan there is no shared handle namespace to get wrong -- a Var carries
// the stage it was reflected from -- so the fix is to write where the handle
// says, and the stage assert makes the old mistake unspellable.
//
void ShaderClass::SetPSConstants(HANDLE hVar, void* data, UINT bytes)
{
	const ShaderReflection::Var *v = (const ShaderReflection::Var*)hVar;
#ifdef SHDCLSDBG
	if (!v) {
		LogErr("Shader::SetPSConstants() Invalid handle. File[%s], Entrypoint[%s], Shader[%s]", fn.c_str(), psn.c_str(), sn.c_str());
		assert(false);
	}
#endif
	if (!v) return;
	assert(v->stage == VK_SHADER_STAGE_FRAGMENT_BIT);
	if (!WriteConstants(pPSCB, v, data, bytes)) {
		LogErr("Shader::SetPSConstants() Failed. Variable[%s], File[%s], Entrypoint[%s]", v->name.c_str(), fn.c_str(), psn.c_str());
	}
}


void ShaderClass::SetVSConstants(HANDLE hVar, void* data, UINT bytes)
{
	const ShaderReflection::Var *v = (const ShaderReflection::Var*)hVar;
#ifdef SHDCLSDBG
	if (!v) {
		LogErr("Shader::SetVSConstants() Invalid handle. File[%s], Entrypoint[%s], Shader[%s]", fn.c_str(), vsn.c_str(), sn.c_str());
		assert(false);
	}
#endif
	if (!v) return;
	assert(v->stage == VK_SHADER_STAGE_VERTEX_BIT);
	if (!WriteConstants(pVSCB, v, data, bytes)) {
		LogErr("Shader::SetVSConstants() Failed. Variable[%s], File[%s], Entrypoint[%s]", v->name.c_str(), fn.c_str(), vsn.c_str());
	}
}


// ----------------------------------------------------------------------------------------------
// Counterpart of ID3DXConstantTable::SetValue.
//
// D3DX took a device because a D3D9 constant table wrote straight into the
// device's constant registers. Here the bytes go into a uniform buffer the
// pipeline reads, so the device is not a parameter -- the buffer is.
//
// The bounds test is new and is not defensiveness for its own sake: SetValue
// silently ignored a write past the end of a constant, whereas overrunning a
// mapped Vulkan allocation corrupts whatever is next in it.
//
bool ShaderClass::WriteConstants(ShaderReflection *pCB, const void *pVar, void *data, UINT bytes)
{
	const ShaderReflection::Var *v = (const ShaderReflection::Var*)pVar;
	if (!pCB || !v || !data || !pDev) return false;
	if (v->bSampler) return false;

	// INTO THE CPU-SIDE BLOCK, NOT STRAIGHT INTO A UNIFORM BUFFER.
	//
	// The bytes must not reach GPU memory here. Every call site sets its
	// constants and then draws, many times per frame, and a descriptor points
	// at memory rather than copying it -- so writing into one shared buffer
	// gave every draw in the frame the LAST draw's constants. The block is
	// accumulated here and committed to its own slice of the per-frame arena
	// by BindResources. See the note on vsBlock in the header.
	std::vector<char> &blk = (pCB == pPSCB) ? psBlock : vsBlock;

	const size_t need = std::max<size_t>(pCB->BlockSize(), size_t(v->offset) + bytes);
	if (!need) return false;
	if (blk.size() < need) blk.resize(need, 0);

	if (size_t(v->offset) + bytes > blk.size()) {
		LogErr("WriteConstants: [%s] offset %u + %u bytes exceeds the %u-byte block",
			v->name.c_str(), v->offset, bytes, (unsigned)blk.size());
		return false;
	}

	memcpy(blk.data() + v->offset, data, bytes);
	return true;
}


// ----------------------------------------------------------------------------------------------
// Build the VkPipeline for one (declaration, depth, blend) combination.
//
// This is where the eleven SetRenderState calls of the Windows Setup() end up.
// Each one is now a field of a create-info struct, set once, and immutable
// afterwards:
//
//   D3DRS_ZENABLE / D3DRS_ZWRITEENABLE      depthTestEnable / depthWriteEnable
//   D3DRS_ALPHABLENDENABLE                  blendEnable
//   D3DRS_BLENDOP / SRCBLEND / DESTBLEND    colorBlendOp / src / dstBlendFactor
//   D3DRS_COLORWRITEENABLE 0xF              colorWriteMask, all four channels
//   D3DRS_STENCILENABLE false               stencilTestEnable
//   D3DRS_ALPHATESTENABLE false             no counterpart -- alpha test was
//                                           removed from the API after D3D9;
//                                           a shader discards instead
//   D3DRS_POINTSPRITEENABLE false           no counterpart -- Vulkan has no
//                                           point sprites; gl_PointSize and a
//                                           quad do that work
//
// The vertex layout comes from the declaration, which is the whole reason
// Setup() takes one.
//
VkPipeline ShaderClass::GetPipeline(const VertexDecl *pDecl, bool bZ, int blend)
{
	// The render pass joins the key; see PipeKey in VulkanUtil.h. It is read
	// once here so the key and pi.renderPass below cannot disagree.
	VkRenderPass pass = pDev ? pDev->GetRenderPass() : VK_NULL_HANDLE;
	VkPolygonMode fill = pDev ? pDev->GetPolygonMode() : VK_POLYGON_MODE_FILL;

	PipeKey key = { pDecl, bZ, blend, topology, cullMode, pass, int(fill) };
	auto it = Pipelines.find(key);
	if (it != Pipelines.end()) return it->second;

	if (!pDev || !pPS || !pVS) return VK_NULL_HANDLE;

	// Which vertex shaders end up in a POINT_LIST pipeline. A point pipeline
	// whose vertex shader never writes gl_PointSize rasterises points of an
	// undefined size -- the layer says so as
	// VUID-VkGraphicsPipelineCreateInfo-topology-08773 -- and the D3D9
	// counterpart of that write is either an explicit `: PSIZE` output or the
	// D3DRS_POINTSIZE default of 1. Diagnostic only; env-gated.
	static const bool bTracePoints = (getenv("ORBITER_VK_TRACE_POINTS") != NULL);
	if (topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST && bTracePoints) {
		static std::map<std::string, int> seen;
		const std::string k = sn + "/" + vsn;
		if (seen.find(k) == seen.end()) {
			seen[k] = 1;
			LogErr("POINTTRACE shaderclass: file=%s vs=%s ps=%s name=%s",
				   fn.c_str(), vsn.c_str(), psn.c_str(), sn.c_str());
		}
	}

	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = pVS;
	stages[0].pName = vsn.c_str();
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = pPS;
	stages[1].pName = psn.c_str();

	VkVertexInputBindingDescription binding = {};
	VkVertexInputAttributeDescription attribs[16] = {};
	uint32_t nAttrib = 0;

	if (pDecl) {
		binding = pDecl->Binding(0);
		nAttrib = pDecl->Attributes(attribs, 16, 0);
	}

	VkPipelineVertexInputStateCreateInfo vi = {};
	vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vi.vertexBindingDescriptionCount = pDecl ? 1 : 0;
	vi.pVertexBindingDescriptions = pDecl ? &binding : nullptr;
	vi.vertexAttributeDescriptionCount = nAttrib;
	vi.pVertexAttributeDescriptions = nAttrib ? attribs : nullptr;

	VkPipelineInputAssemblyStateCreateInfo ia = {};
	ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	// Was hard-coded TRIANGLE_LIST, which was right for every caller until
	// HazeManager2, whose two draws are TRIANGLESTRIPs. See SetTopology.
	ia.topology = topology;

	VkPipelineViewportStateCreateInfo vp = {};
	vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	vp.viewportCount = 1;
	vp.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rs = {};
	rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rs.polygonMode = fill;		// D3DRS_FILLMODE; see VulkanDevice::SetPolygonMode
	// Bisection aid: ORBITER_VK_WIREFRAME=1 draws geometry as lines, which
	// separates "the geometry is not there / not transformed onto the screen"
	// from "the geometry is there and shades black". NOTE that line mode does
	// NOT disable back-face culling -- use ORBITER_VK_NOCULL with it.
	{
		static const bool bWire = (getenv("ORBITER_VK_WIREFRAME") != NULL);
		if (bWire) rs.polygonMode = VK_POLYGON_MODE_LINE;
	}
	// D3DRS_CULLMODE. Was hard-coded VK_CULL_MODE_NONE, which was right for
	// every caller until TileManager2<SurfTile>::Render, which sets
	// D3DCULL_CCW immediately after Setup(). See SetCullMode.
	//
	// THE TWO FIELDS ARE NOT INDEPENDENT: D3DCULL_CW and D3DCULL_CCW are the
	// SAME Vulkan cull mode -- VK_CULL_MODE_BACK_BIT -- and differ only in
	// frontFace, because "cull the clockwise triangles" and "cull the
	// counter-clockwise triangles" are one cull with opposite ideas of which
	// winding faces front. Same table as VulkanEffect.cpp's GetPipeline and
	// the .tech parser's ParseCullMode.
	switch (cullMode) {
	case CULL_CW:
		rs.cullMode = VkCullModeFlags(VK_CULL_MODE_BACK_BIT);
		rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		break;
	case CULL_CCW:
		rs.cullMode = VkCullModeFlags(VK_CULL_MODE_BACK_BIT);
		rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
		break;
	default:	// CULL_NONE
		rs.cullMode = VkCullModeFlags(VK_CULL_MODE_NONE);
		rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		break;
	}
	// Bisection aid: ORBITER_VK_NOCULL=1 removes back-face culling, to tell
	// "culled" apart from "not transformed onto the screen". Diagnostic only.
	{
		static const bool bNoCull = (getenv("ORBITER_VK_NOCULL") != NULL);
		if (bNoCull) rs.cullMode = VkCullModeFlags(VK_CULL_MODE_NONE);
	}
	rs.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo ms = {};
	ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo ds = {};
	ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	ds.depthTestEnable = bZ ? VK_TRUE : VK_FALSE;		// D3DRS_ZENABLE
	ds.depthWriteEnable = bZ ? VK_TRUE : VK_FALSE;		// D3DRS_ZWRITEENABLE
	ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	ds.stencilTestEnable = VK_FALSE;					// D3DRS_STENCILENABLE

	VkPipelineColorBlendAttachmentState cb = {};
	cb.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
					  | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;	// 0xF
	cb.blendEnable = (blend != 0) ? VK_TRUE : VK_FALSE;
	cb.colorBlendOp = VK_BLEND_OP_ADD;
	cb.alphaBlendOp = VK_BLEND_OP_ADD;
	cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;

	switch (blend) {
	case 1:		// ADD, SRCALPHA, INVSRCALPHA
		cb.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		break;
	case 2:		// ADD, ONE, INVSRCALPHA
		cb.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
		cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		break;
	case 3:		// MAX, ONE, ONE
		cb.colorBlendOp = VK_BLEND_OP_MAX;
		cb.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
		cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
		break;
	case 4:		// ADD, SRCALPHA, ONE
		cb.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
		break;
	default:
		cb.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
		cb.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
		break;
	}

	// ONE ATTACHMENT STATE PER COLOUR ATTACHMENT, which is a Vulkan
	// requirement with no D3D9 counterpart -- D3DRS_ALPHABLENDENABLE and its
	// siblings were device state shared by every bound target. Was hard-coded
	// to one, which is right for the core's pass and wrong for a multi-target
	// offscreen one. See VulkanDevice::GetRenderPassColourCount.
	VkPipelineColorBlendAttachmentState cbs[8];
	uint32_t nCb = pDev->GetRenderPassColourCount();
	if (nCb > ARRAYSIZE(cbs)) nCb = ARRAYSIZE(cbs);
	for (uint32_t i = 0; i < nCb; i++) cbs[i] = cb;

	VkPipelineColorBlendStateCreateInfo bs = {};
	bs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	bs.attachmentCount = nCb;
	bs.pAttachments = cbs;

	// The viewport and scissor are the two pieces Vulkan keeps dynamic, which
	// is why Setup() can still set them per frame.
	VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dsi = {};
	dsi.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dsi.dynamicStateCount = ARRAYSIZE(dyn);
	dsi.pDynamicStates = dyn;

	// THE DESCRIPTOR SET LAYOUT HAS TO EXIST BEFORE THE PIPELINE LAYOUT, and
	// getting that order wrong is not a warning -- it is a driver crash.
	//
	// CreateResources() builds pSetLayout, and it used to be called only from
	// the first UpdateTextures(). GetPipeline() runs earlier than that: the
	// pipeline is built when a technique is first set up, before any texture
	// is bound. So pSetLayout was VK_NULL_HANDLE here, the ternaries below
	// baked setLayoutCount = 0 into pLayout, and pLayout is created ONCE and
	// cached -- so it stayed a zero-set layout for the life of the shader even
	// after CreateResources() had built the real set layout.
	//
	// BindResources() then does vkCmdBindDescriptorSets(..., firstSet 0,
	// count 1, pLayout). Validation says exactly what that is:
	//
	//     VUID-vkCmdBindDescriptorSets-firstSet-00360
	//     firstSet (0) plus descriptorSetCount (1) is greater than
	//     VkPipelineLayoutCreateInfo::setLayoutCount (0)
	//
	// and without the layer the NVIDIA driver dereferences a null function
	// pointer inside libnvidia-glcore instead. That was the crash in
	// HazeManager2::RenderSky -- the first draw the client ever reached.
	//
	// CreateResources() is idempotent (it early-returns on bResources), so
	// calling it here is free on every later pipeline in the cache.
	CreateResources();

	if (!pLayout) {
		VkPipelineLayoutCreateInfo li = {};
		li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		li.setLayoutCount = pSetLayout ? 1 : 0;
		li.pSetLayouts = pSetLayout ? &pSetLayout : nullptr;
		HR(vkCreatePipelineLayout(pDev->GetDevice(), &li, nullptr, &pLayout));
	}

	VkGraphicsPipelineCreateInfo pi = {};
	pi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pi.stageCount = 2;
	pi.pStages = stages;
	pi.pVertexInputState = &vi;
	pi.pInputAssemblyState = &ia;
	pi.pViewportState = &vp;
	pi.pRasterizationState = &rs;
	pi.pMultisampleState = &ms;
	pi.pDepthStencilState = &ds;
	pi.pColorBlendState = &bs;
	pi.pDynamicState = &dsi;
	pi.layout = pLayout;
	// The render pass this pipeline may be bound in -- the core's outside an
	// offscreen pass, the offscreen one inside it, and the same handle that
	// went into the cache key above. See VulkanFrame.h.
	pi.renderPass = pass;
	pi.subpass = 0;

	VkPipeline pipe = VK_NULL_HANDLE;
	HR(vkCreateGraphicsPipelines(pDev->GetDevice(), pDev->GetPipelineCache(), 1, &pi, nullptr, &pipe));

	Pipelines[key] = pipe;
	return pipe;
}
