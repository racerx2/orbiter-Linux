// ==============================================================
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2006-2026 Martin Schweiger
//				 2012-2016 Jarmo Nikkanen
// ==============================================================
//
// The D3DX math types became the SDK's own: D3DXVECTOR2/3/4 -> FVECTOR2/3/4,
// D3DXMATRIX -> FMATRIX4, D3DXCOLOR -> FVECTOR4 (same .r/.g/.b/.a and the
// same DWORD packing), D3DCOLORVALUE -> COLOUR4, D3DMATERIAL9 -> MATERIAL --
// the last two field for field, not approximations.
//
// The shader-facing structs keep their exact byte layout, which was checked
// rather than assumed: they sit inside #pragma pack(push,4) as on Windows,
// but FVECTOR4 and FMATRIX4 are alignas(16) where D3DXVECTOR4 was not, so the
// packed layout could have changed silently -- a wrong material is a wrong
// colour, not a crash. Measured: pack(4) does lower alignas(16) for members
// and the offsets come out identical (0,16,32,44,56,68,80,92,100,104,120 /
// 124 bytes), which the static_asserts below hold.
//
// The vertex declarations became Vulkan vertex input descriptions.
// D3DVERTEXELEMENT9 binds a byte range to an HLSL semantic name; Vulkan
// matches by an explicit location number, so location = the element's index
// in the declaration, in the Windows order, with offsets unchanged. The
// stride is new -- D3D9 took it from SetStreamSource at bind time -- so each
// declaration names the struct it describes via sizeof(), which is why the
// VertexDecl objects are defined in VulkanUtil.cpp.
//
// Two bugs in the Windows source are fixed here, each recorded at the point
// of change in VulkanUtil.cpp: ShaderClass::SetPSConstants(HANDLE) wrote
// through the vertex shader's constant table, and startsWith() had the body
// of contains().
// ==============================================================

#ifndef __VULKANUTIL_H
#define __VULKANUTIL_H

#include "OrbiterAPI.h"
#include "Log.h"
#include "DrawAPI.h"
#include "VulkanTypes.h"
#include <string>
#include <map>
#include <deque>
#include <set>
#include <math.h>
#include "gcCore.h"

#define float2 FVECTOR2
#define float3 FVECTOR3
#define float4 FVECTOR4
#define float4x4 FMATRIX4

#ifdef _DEBUG
#ifndef _TRACE
#define _TRACE { LogTrace("[TRACE] %s Line:%d %s",__FILE__,__LINE__,__FUNCTION__); }
#endif
#else
#ifndef _TRACE
#define _TRACE
#endif
#endif


// Counterpart of the HRESULT form. The checked type is VkResult and the test
// is an equality against VK_SUCCESS rather than FAILED(): Vulkan's positive
// results (VK_SUBOPTIMAL_KHR, VK_TIMEOUT, VK_NOT_READY) are not failures, but
// they are not "carry on blindly" either, and FAILED() waves them through.

#ifndef HR
#define HR(x)                                      \
{                                                  \
	VkResult res = x;                              \
	if (res != VK_SUCCESS)                         \
	{												\
		LogErr("%s Line:%d Error:%d %s",__FILE__,__LINE__,int(res),(#x)); \
	}                                                \
}
#endif


inline bool _HROK(VkResult res, const char *file, int line)
{
	if (res == VK_SUCCESS) return true;
	else LogErr("%s Line:%d Error:%d", file, line, int(res));
	return false;
}

const char *_PTR(const void *p);


#ifndef HROK
#define HROK(x)	_HROK(x, __FILE__, __LINE__)
#endif

#define PI 3.141592653589793238462643383279

#define SURFACE(x) ((class SurfNative *)x)

// helper function to get address of a temporary
// The regular "easy" way no longer works on some compilers so lets use a hack to get a simple thing done.
// NB: use with caution

template<typename T>
T* ptr(T&& x) { return &x; }



// ------------------------------------------------------------------------------------
// Vertex Declaration equal to NTVERTEX
// ------------------------------------------------------------------------------------
//
// Each entry below was a D3DVERTEXELEMENT9 { Stream, Offset, Type, Method,
// Usage, UsageIndex } and is now a VertexAttrib { location, format, offset }.
// Stream and Method are dropped; Usage + UsageIndex became 'location', since
// HLSL semantic names have no Vulkan counterpart and the location is the
// element's index in the declaration. Two of the Type mappings produce a
// picture rather than an error when got wrong:
//
//   D3DCOLOR is a DWORD written 0xAARRGGBB. Byte by byte on a little-endian
//   machine that is B,G,R,A, which is VK_FORMAT_B8G8R8A8_UNORM -- not
//   R8G8B8A8, which would swap red and blue.
//
//   D3DDECLTYPE_SHORT4 delivers four signed shorts to the shader as floats
//   without normalising them. Vulkan's exact equivalent,
//   VK_FORMAT_R16G16B16A16_SSCALED, is optional and widely unsupported, so
//   this uses _SINT and the GLSL declares the input as ivec4 and converts.
//   The values are pixel coordinates, so the conversion is exact either way.
// ------------------------------------------------------------------------------------

const VertexAttrib BAVertexDecl[] = {
	{0,  VK_FORMAT_R32G32B32_SFLOAT,     0},
	{1,  VK_FORMAT_R32G32B32_SFLOAT,    12},
	{2,  VK_FORMAT_R32G32B32A32_SFLOAT, 24},
	{3,  VK_FORMAT_R32G32_SFLOAT,       40},
	{4,  VK_FORMAT_B8G8R8A8_UNORM,      48}
};

const VertexAttrib NTVertexDecl[] = {
	{0,  VK_FORMAT_R32G32B32_SFLOAT,     0},
	{1,  VK_FORMAT_R32G32B32_SFLOAT,    12},
	{2,  VK_FORMAT_R32G32_SFLOAT,       24}
};

const VertexAttrib MeshVertexDecl[] = {
	{0,  VK_FORMAT_R32G32B32_SFLOAT,     0},
	{1,  VK_FORMAT_R32G32B32_SFLOAT,    12},
	{2,  VK_FORMAT_R32G32B32_SFLOAT,    24},
	{3,  VK_FORMAT_R32G32B32_SFLOAT,    36}
};

const VertexAttrib PatchVertexDecl[] = {
	{0,  VK_FORMAT_R32G32B32_SFLOAT,     0},
	{1,  VK_FORMAT_R32G32B32_SFLOAT,    12},
	{2,  VK_FORMAT_R32G32_SFLOAT,       24},
	{3,  VK_FORMAT_R32_SFLOAT,          32}
};

const VertexAttrib PosTexDecl[] = {
	{0,  VK_FORMAT_R32G32B32_SFLOAT,     0},
	{1,  VK_FORMAT_R32G32_SFLOAT,       12}
};

const VertexAttrib PosColorDecl[] = {
	{0,  VK_FORMAT_R32G32B32_SFLOAT,     0},
	{1,  VK_FORMAT_B8G8R8A8_UNORM,      12}
};

const VertexAttrib PositionDecl[] = {
	{0,  VK_FORMAT_R32G32B32_SFLOAT,     0}
};

const VertexAttrib SketchpadDecl[] = {
	{0,  VK_FORMAT_R32G32B32_SFLOAT,     0},
	{1,  VK_FORMAT_R32G32B32A32_SFLOAT, 12},
	{2,  VK_FORMAT_B8G8R8A8_UNORM,      28},
	{3,  VK_FORMAT_B8G8R8A8_UNORM,      32}
};

const VertexAttrib Vector4Decl[] = {
	{0,  VK_FORMAT_R32G32B32A32_SFLOAT,  0}
};

const VertexAttrib GPUBlitDecl[] = {
	{0,  VK_FORMAT_R16G16B16A16_SINT,    0}
};

const VertexAttrib HazeVertexDecl[] = {
	{0,  VK_FORMAT_R32G32B32_SFLOAT,     0},
	{1,  VK_FORMAT_B8G8R8A8_UNORM,      12},
	{2,  VK_FORMAT_R32G32_SFLOAT,       16}
};

const VertexAttrib LocalLightsDecl[] = {
	{0,  VK_FORMAT_R32_SFLOAT,           0},	//Primitive Index
	{1,  VK_FORMAT_R32G32B32A32_SFLOAT,  4}		//Position .xyz and cone .w
};

typedef struct
{
	float index;
	FVECTOR3 pos;
	float cone;
} LocalLightsCompute;

typedef struct {
	float x;     ///< vertex x position
	float y;     ///< vertex y position
	float z;     ///< vertex z position
	float tu;    ///< vertex u texture coordinate
	float tv;    ///< vertex v texture coordinate
} SMVERTEX;

typedef struct {
	float x, y, z;
	float nx, ny, nz;
	float tx, ty, tz;
	float u, v, w;
} NMVERTEX;

typedef struct {
	short tx,ty;
	short sx,sy;
} GPUBLITVTX;


typedef struct {
	FVECTOR3 pos;					///< beacon position
	FVECTOR3 dir;					///< light direction
	float size, angle, on, off;		///< beacon size and light cone angle
	float bright, falloff;
	DWORD color;					///< beacon color
} BAVERTEX;


// ------------------------------------------------------------------------------------
// The shader-facing structs, and why they are packed. LightStruct, VulkanSun,
// VulkanMatExt and VulkanTune are copied raw into shader constant storage and
// read back by a matching struct in the shader source. D3DX copied them field
// by field from a tightly packed source, which is why the Windows LightStruct
// is 76 bytes and not the 96 an HLSL constant-register layout would need. Two
// things keep that true here:
//
//   FVECTOR4 and FMATRIX4 are alignas(16) where D3DXVECTOR4 and D3DXMATRIX
//   were not. Dropped into a struct unchanged, Diffuse would move from offset
//   8 to 16 and LightStruct would grow to 96 bytes, unreported -- a
//   misaligned light is a picture, not an error. The three structs Windows
//   packed keep their #pragma pack(4) and LightStruct gains one.
//
//   Vulkan's default uniform layout is not tight packing: GLSL std140 would
//   put vec4 diffuse at offset 16 and make Light 96 bytes, the same mismatch.
//   Rather than pad every struct here and in the shaders, the GLSL declares
//   these blocks layout(scalar) (GL_EXT_scalar_block_layout, core since
//   Vulkan 1.2), which is C layout.
// ------------------------------------------------------------------------------------

// ------------------------------------------------------------------------------------
// FVECTOR4P -- a 4-byte-aligned FVECTOR4, which the packed structs must use
// instead of FVECTOR4 itself.
//
// #pragma pack(4) lowers the alignment of the member, not of the type, so the
// compiler may still assume any FVECTOR4 lvalue is 16-byte aligned. In
// VulkanMatExt SpecialFX sits at offset 104 (8 mod 16), and GCC coalesces
// FVECTOR4's constructor into a single aligned store, so at -O2
// `new VulkanMatExt[1]` segfaults in it -- crashing the first mesh load, so no
// scenario would open. -O0 and -O1 survive. D3DXVECTOR4 was a plain 4-float
// type with no over-alignment, which is why Windows never saw this. The bytes
// are identical and it converts implicitly both ways, so nothing else changes.
// ------------------------------------------------------------------------------------
typedef union FVECTOR4P
{
	struct { float x, y, z, w; };
	struct { float r, g, b, a; };
	float data[4];

	FVECTOR4P() { x = y = z = w = 0.0f; }
	FVECTOR4P(float q) { x = y = z = w = q; }
	FVECTOR4P(float _x, float _y, float _z, float _w) { x = _x; y = _y; z = _z; w = _w; }
	FVECTOR4P(const oapi::FVECTOR4 &v) { x = v.x; y = v.y; z = v.z; w = v.w; }

	FVECTOR4P &operator=(const oapi::FVECTOR4 &v) { x = v.x; y = v.y; z = v.z; w = v.w; return *this; }
	operator oapi::FVECTOR4() const { return oapi::FVECTOR4(x, y, z, w); }
} FVECTOR4P;

static_assert(sizeof(FVECTOR4P) == 16, "FVECTOR4P must be four floats");
static_assert(alignof(FVECTOR4P) == 4, "FVECTOR4P must NOT be over-aligned -- that is its whole purpose");

// FMATRIX4P -- the same treatment for FMATRIX4, which is ORB_ALIGN16 too.
// ShaderParams::mWorld/mLVP, ConstParams::mVP and CelDataStruct::mWorld/
// mViewProj are all float4x4 inside a pack(4), so they have the identical
// hazard: sizeof(ShaderParams) is 348, so in any array the second element
// starts 12 mod 16 and every matrix in it is misaligned.
typedef union FMATRIX4P
{
	struct {
		float m11, m12, m13, m14;
		float m21, m22, m23, m24;
		float m31, m32, m33, m34;
		float m41, m42, m43, m44;
	};
	float data[16];

	FMATRIX4P() { for (int i = 0; i < 16; i++) data[i] = 0.0f; }
	FMATRIX4P(const oapi::FMATRIX4 &m) { for (int i = 0; i < 16; i++) data[i] = m.data[i]; }

	FMATRIX4P &operator=(const oapi::FMATRIX4 &m)
	{
		for (int i = 0; i < 16; i++) data[i] = m.data[i];
		return *this;
	}

	operator oapi::FMATRIX4() const
	{
		oapi::FMATRIX4 r;
		for (int i = 0; i < 16; i++) r.data[i] = data[i];
		return r;
	}

	void Zero() { for (int i = 0; i < 16; i++) data[i] = 0.0f; }
	void Ident() { Zero(); m11 = m22 = m33 = m44 = 1.0f; }
} FMATRIX4P;

static_assert(sizeof(FMATRIX4P) == 64, "FMATRIX4P must be sixteen floats");
static_assert(alignof(FMATRIX4P) == 4, "FMATRIX4P must NOT be over-aligned");

// No -Wpacked-not-aligned suppression: with FVECTOR4P in place there is
// nothing over-aligned left to lower, so the warning stays on and reports it
// if an over-aligned type is ever put back into one of these.
#pragma pack(push, 4)

typedef struct _LightStruct  {
    int			  Type;             ///< Type of light source
	float		  Dst2;				///< Square distance between camera and the light emitter
    FVECTOR4P     Diffuse;          ///< Color of light        (offset 8  -- 8 mod 16)
    FVECTOR3      Position;         ///< position in world space
    FVECTOR3      Direction;        ///< direction in world space
    FVECTOR3      Attenuation;      ///< Attenuation
	FVECTOR4P     Param;            ///< range, falloff, theta, phi (offset 60 -- 12 mod 16)
public : _LightStruct () :	Type(0),
							Dst2(0.0),
							Diffuse(0.0f),
							Position(0,0,0), Direction(1.0f, 0.0f, 0.0f), Attenuation(1.0f, 1.0f, 1.0f),
							Param(0,0,0,0)
							{}
} LightStruct;

#pragma pack(pop)

// The layout Mesh.cpp uploads and shaders/D3D9Client.fx 'struct Light' reads.
static_assert(sizeof(LightStruct) == 76, "LightStruct must stay tightly packed for shader upload");
static_assert(offsetof(LightStruct, Diffuse) == 8, "LightStruct::Diffuse moved");
static_assert(offsetof(LightStruct, Position) == 24, "LightStruct::Position moved");
static_assert(offsetof(LightStruct, Direction) == 36, "LightStruct::Direction moved");
static_assert(offsetof(LightStruct, Attenuation) == 48, "LightStruct::Attenuation moved");
static_assert(offsetof(LightStruct, Param) == 60, "LightStruct::Param moved");


class VulkanLight : public LightStruct
{
public:
				VulkanLight();
				VulkanLight(const LightEmitter *le, const class vObject *vo);
				~VulkanLight();

		float	GetIlluminance(FVECTOR3 &pos, float r) const;
		void	UpdateLight(const LightEmitter *le, const class vObject *vo);
		void	Reset();
		const   LightEmitter *GetEmitter() const;

		float	cone;
		int		GPUId;
private:
		float	cosp, tanp, cosu;
		float	range, range2;
		float	intensity;
		const   LightEmitter *le;
};



class SketchMesh
{

public:

	struct SKETCHGRP {			// mesh group definition
		DWORD VertOff;			// Main mesh Vertex Offset
		DWORD IdxOff;			// Index Offset
		DWORD nIdx;				// Index count
		DWORD nVert;			// Vertex count
		DWORD MtrlIdx;			// material index
		DWORD TexIdx;			// texture index 0=None
	};

	explicit		SketchMesh(VulkanDevice *pDev);
	~SketchMesh();

	void			Init();
	bool			LoadMeshFromHandle(MESHHANDLE hMesh);
	void			RenderGroup(DWORD idx);
	SURFHANDLE		GetTexture(DWORD idx);
	FVECTOR4		GetMaterial(DWORD idx);
	DWORD			GroupCount() const { return nGrp; }

private:

	VulkanBuffer   *pVB;		///< (Local) Vertex buffer pointer
	VulkanBuffer   *pIB;

	DWORD MaxVert;
	DWORD MaxIdx;

	DWORD nGrp;                 // number of mesh groups
	DWORD nMtrl;                // number of mesh materials
	DWORD nTex;                 // number of mesh textures

	VulkanDevice *pDev;
	SURFHANDLE* Tex;			// list of mesh textures
	SKETCHGRP* Grp;            // list of mesh groups
	FVECTOR4* Mtrl;
};

#pragma pack(push, 4)

typedef struct {
	FVECTOR3 Dir;
	FVECTOR3 Color;			// Color and Intensity of received sunlight 
	FVECTOR3 Ambient;		// Ambient light level (Base Objects Only, Vessels are using dynamic methods)
	FVECTOR3 Transmission;	// Visibility through atmosphere (1.0 = fully visible, 0.0 = obscured)
	FVECTOR3 Incatter;		// Amount of incattered light from haze
} VulkanSun;


#define VULKANMATEX_DIFFUSE		0x001
#define VULKANMATEX_AMBIENT		0x002
#define VULKANMATEX_SPECULAR	0x004
#define VULKANMATEX_EMISSIVE	0x008
#define VULKANMATEX_REFLECT		0x010
#define VULKANMATEX_FRESNEL		0x040
#define VULKANMATEX_ROUGHNESS	0x080
#define VULKANMATEX_EMISSION2	0x100
#define VULKANMATEX_METALNESS	0x200
#define VULKANMATEX_SPECIALFX	0x400


/**
 * \brief Material structure used in VulkanMesh. ModFlags is not loaded to shaders
 */
typedef struct {
	// Offsets 0 and 16 look safely aligned and are not: sizeof is 124, so in
	// `new VulkanMatExt[n]` element 1 starts at 124 and Diffuse lands at
	// 12 mod 16.
	FVECTOR4P	  Diffuse;
	FVECTOR4P     Specular;			///< Specular color, power in alpha
	FVECTOR3	  Ambient;
	FVECTOR3      Emissive;
	FVECTOR3      Reflect;			///< Color multiplier and intensity (alpha)
	FVECTOR3	  Emission2;		///<
	FVECTOR3	  Fresnel;			///< Fresnel reflection
	FVECTOR2	  Roughness;		///< 
	float		  Metalness;
	FVECTOR4P	  SpecialFX;		///< offset 104 -- 8 mod 16, so it cannot be FVECTOR4
	// -----------------------
	DWORD		  ModFlags;			///< Modification flags
} VulkanMatExt;


typedef struct {
	COLOUR4	Albedo;		// Tune Albedo
	COLOUR4	Emis;		// Tune Emission Maps
	COLOUR4	Spec;		// Tune Specular Maps
	COLOUR4	Refl;		// Tune Reflection Maps
	COLOUR4	Transl;		// Tune translucent effect
	COLOUR4	Transm;		// Tune transmissive effect
	COLOUR4	Norm;		// Tune normal map
	COLOUR4	Rghn;		// Tune roughness map
} VulkanTune;

#pragma pack(pop)

// The layouts shaders/D3D9Client.fx 'struct Sun', 'struct Mtrl' and
// 'struct Tune' read. ModFlags sits past the 120 bytes that reach the shader.
static_assert(sizeof(VulkanSun) == 60, "VulkanSun must stay tightly packed for shader upload");
static_assert(sizeof(VulkanMatExt) == 124, "VulkanMatExt must stay tightly packed for shader upload");
static_assert(offsetof(VulkanMatExt, SpecialFX) == 104, "VulkanMatExt::SpecialFX moved");
static_assert(offsetof(VulkanMatExt, ModFlags) == 120, "VulkanMatExt::ModFlags moved");
static_assert(sizeof(VulkanTune) == 128, "VulkanTune must stay tightly packed for shader upload");

typedef struct {
	class VulkanMesh *pMesh;		///< Mesh handle
	class vObject  *vObj;			///< Visual handle
	float			dist;			///< Distance to a pick point
	int				group;			///< Mesh group that was picked
	FVECTOR3		normal;			///< Normal vector in local vessel coordinates
	FVECTOR3		pos;			///< Position in local vessel coordinates
	int				idx;			///< Index that was picked
	float			u, v;			///< Barycentric coordinates
} VulkanPick;

typedef struct {
	FVECTOR3 _p;		// Position from camera
	FVECTOR3 _n;		// Normal
	int i;				// Face Index
	float d;			// Distance from camera
	float u, v;			
	double lng, lat, elev;
	class Tile * pTile;
} TILEPICK;

#define VulkanLRange 0
#define VulkanLFalloff 1
#define VulkanLTheta 2
#define VulkanLPhi 3


// ------------------------------------------------------------------------------------
// The vertex declarations themselves. On Windows these were
// IDirect3DVertexDeclaration9* globals built in D3D9Frame.cpp by
// pDevice->CreateVertexDeclaration() -- device objects created and released
// with the device. A Vulkan vertex layout is not an object but immutable data
// compiled into a VkPipeline, so these are constants defined in
// VulkanUtil.cpp beside the tables and strides that describe them, and the
// eleven CreateVertexDeclaration calls have no counterpart. The names and the
// pointer spelling are kept so the call sites do not change shape.
// ------------------------------------------------------------------------------------

extern const VertexDecl	*pMeshVertexDecl;
extern const VertexDecl	*pHazeVertexDecl;
extern const VertexDecl	*pNTVertexDecl;
extern const VertexDecl	*pBAVertexDecl;
extern const VertexDecl	*pPosColorDecl;
extern const VertexDecl	*pPositionDecl;
extern const VertexDecl	*pVector4Decl;
extern const VertexDecl	*pPosTexDecl;
extern const VertexDecl	*pPatchVertexDecl;
extern const VertexDecl	*pSketchpadDecl;
extern const VertexDecl *pLocalLightsDecl;


// ------------------------------------------------------------------------------------
// ShaderClass -- same class, same job, same call sites. LPD3DXCONSTANTTABLE
// became ShaderReflection, per-stage exactly as the two D3DX tables were, and
// Setup() now builds or fetches a VkPipeline: in D3D9 it set the shader pair,
// the vertex declaration and eight render states as independent pieces of
// device state, all of which Vulkan bakes into one immutable pipeline, so
// Setup()'s three arguments are exactly the pipeline's key. The viewport
// survives as a call, being one of the few pieces Vulkan keeps dynamic.
// ------------------------------------------------------------------------------------

class ShaderClass
{
	

public:
			ShaderClass(VulkanDevice *pDev, const char* file, const char* vs, const char* ps, const char* name, const char* options);
		    ~ShaderClass();
	void	ClearTextures();
	void    UpdateTextures();
	void	DetachTextures();
	void	Setup(const VertexDecl *pDecl, bool bZ, int blend);

	/// \brief The primitive topology the next Setup() builds its pipeline
	///        with; TRIANGLE_LIST by default. The value existed on Windows as
	///        DrawPrimitive's first argument, chosen per draw. Vulkan bakes
	///        topology into the VkPipeline, so it must be known before Setup()
	///        binds one, and it joins the pipeline cache key.
	void	SetTopology(VkPrimitiveTopology topo) { topology = topo; }

	/// \brief The three values D3DRS_CULLMODE can take. Same numbering as
	///        VulkanEffectFile::PassOverride::CullMode, declared again rather
	///        than shared because VulkanEffect.h includes this header.
	enum CullMode { CULL_NONE = 0, CULL_CW = 1, CULL_CCW = 2 };

	/// \brief The cull mode the next Setup() builds its pipeline with;
	///        CULL_NONE by default. On Windows this was a
	///        SetRenderState(D3DRS_CULLMODE, ...) issued after Setup() and
	///        before the draw -- TileManager2<SurfTile>::Render asks for
	///        D3DCULL_CCW so the far side of the planet is not drawn -- and
	///        Vulkan bakes the cull into the pipeline, so it has to arrive
	///        before the bind and joins the cache key.
	void	SetCullMode(int mode) { cullMode = mode; }

	HANDLE	GetPSHandle(const char* name);
	HANDLE	GetVSHandle(const char* name);

	void	SetTexture(const char* name, VulkanTexture *pTex, UINT Flags = IPF_CLAMP | IPF_ANISOTROPIC, UINT AnisoLvl = 4);
	void	SetTextureVS(const char* name, VulkanTexture *pTex, UINT flags = IPF_CLAMP | IPF_POINT, UINT AnisoLvl = 0);
	void	SetPSConstants(const char* name, void* data, UINT bytes);
	void	SetVSConstants(const char* name, void* data, UINT bytes);

	void	SetTexture(HANDLE hVar, VulkanTexture *pTex, UINT flags = IPF_CLAMP | IPF_POINT, UINT AnisoLvl = 0);
	void	SetTextureVS(HANDLE hVar, VulkanTexture *pTex, UINT flags, UINT aniso);
	void	SetPSConstants(HANDLE hVar, void* data, UINT bytes);
	void	SetVSConstants(HANDLE hVar, void* data, UINT bytes);
	VulkanDevice *GetDevice() { return pDev; }

	/// \brief Counterpart of DrawPrimitiveUP / DrawIndexedPrimitiveUP for a
	///        ShaderClass-bound draw -- Scene.cpp's
	///        ComputeLocalLightsVisibility and RenderGlares reach the device
	///        directly rather than through an effect. Vulkan has no such call,
	///        so this copies into a scratch buffer, which is what the D3D9
	///        runtime did behind it anyway. pIdx may be NULL for the
	///        unindexed form, and the count is vertices, not primitives.
	void	DrawUP(const void *pVtx, UINT nVtx, UINT stride,
				   const WORD *pIdx = NULL, UINT nIdx = 0);

	/// \brief Release every instance's descriptor sets. Must be called once
	///        per frame, from the client's frame boundary. A D3D9 SetTexture
	///        consumed its argument immediately; a Vulkan draw only records a
	///        reference to a descriptor set, so each draw needs its own and it
	///        stays live until the frame naming it has been submitted.
	///        Static, over a registry, because the several ShaderClass objects
	///        have no single owner to walk them.
	static void	ResetFrame();

private:

	// pSampler is new. D3D9 set eight sampler states on a numbered device slot;
	// Vulkan has no such slot, only a sampler object built from those same
	// eight parameters, so the slot has to own one. bSamplerSet still means
	// "unchanged since last time"; it now guards a create rather than eight
	// SetSamplerState calls.
	struct TexParams
	{
		VulkanTexture *pTex;
		VulkanTexture *pAssigned;
		UINT Flags;
		UINT AnisoLvl;
		bool bSamplerSet;
		VkSampler pSampler;
	} pTextures[20];

	/// \brief Fetch or build the pipeline for this (declaration, depth, blend).
	VkPipeline	GetPipeline(const VertexDecl *pDecl, bool bZ, int blend);

	/// \brief Counterpart of ID3DXConstantTable::SetValue: put 'bytes' bytes
	///        at the variable's offset in its stage's uniform block.
	bool		WriteConstants(ShaderReflection *pCB, const void *pVar, void *data, UINT bytes);

	ShaderReflection *pPSCB, *pVSCB;
	VkShaderModule pPS;
	VkShaderModule pVS;
	VulkanDevice *pDev;
	std::string fn, psn, vsn, sn;

	// The pipeline cache, keyed on Setup()'s three arguments. D3D9 needed none:
	// it had no pipeline object, only state set per draw and thrown away.
	struct PipeKey {
		const VertexDecl *pDecl;
		bool bZ;
		int blend;
		VkPrimitiveTopology topo;
		int cull;
		// The render pass is part of the key: a VkPipeline may only be bound
		// inside a render pass compatible with the one it was created
		// against, and compatibility requires matching attachment formats, so
		// the same shader pair drawn into the swapchain and into an
		// R32_SFLOAT shadow map are two pipelines. D3D9 needed nothing like
		// it: SetRenderTarget was device state and a shader did not care what
		// it pointed at.
		VkRenderPass pass;
		// D3DRS_FILLMODE, for the same reason: global device state there,
		// pipeline state here.
		int fill;
		bool operator<(const PipeKey &o) const {
			if (pDecl != o.pDecl) return pDecl < o.pDecl;
			if (bZ != o.bZ) return bZ < o.bZ;
			if (blend != o.blend) return blend < o.blend;
			if (topo != o.topo) return topo < o.topo;
			if (cull != o.cull) return cull < o.cull;
			if (pass != o.pass) return pass < o.pass;
			return fill < o.fill;
		}
	};
	std::map<PipeKey, VkPipeline> Pipelines;

	/// \brief See SetTopology. TRIANGLE_LIST unless a caller says otherwise.
	VkPrimitiveTopology	topology;
	/// \brief See SetCullMode. CULL_NONE unless a caller says otherwise.
	int					cullMode;
	VkPipelineLayout	pLayout;
	VkDescriptorSetLayout pSetLayout;

	// The uniform blocks are a per-frame arena rather than one buffer per
	// stage, because a Vulkan descriptor copies nothing: it points at memory
	// the GPU reads when the frame is submitted. A frame draws ~2000 surface
	// tiles through one ShaderClass, and with a single buffer the GPU reads
	// whatever the last tile left, collapsing every tile onto one. D3D9
	// needed none of this: ID3DXConstantTable::SetValue wrote the device's
	// constant registers and the runtime snapshotted them per draw.
	//
	// So Set*Constants accumulates into a CPU-side block per stage, and
	// BindResources copies that block into a fresh slice of the per-frame
	// arena, binding it with a dynamic offset. ResetFrame rewinds it.
	std::vector<char>	vsBlock, psBlock;	///< CPU-side, accumulated per draw
	VulkanBuffer *		pArena;				///< one per frame, sliced per draw
	VkDeviceSize		arenaSize, arenaUsed;
	uint32_t			vsSlice, psSlice;	///< this draw's dynamic offsets
	/// Whether the set layout declares each block. Fixed at CreateResources
	/// time, not per draw: vkCmdBindDescriptorSets wants exactly one dynamic
	/// offset per dynamic descriptor the layout declares, whether or not this
	/// draw wrote anything into it.
	bool				bVSBlock, bPSBlock;
	/// Arenas a mid-frame grow replaced, each carrying the frame number that
	/// retired it. Freed once the GPU is certainly done -- not at the point of
	/// growth, and not at the next ResetFrame either, since the frame that
	/// recorded the binding is still in flight one frame later.
	std::deque<std::pair<VulkanBuffer *, unsigned long long> > retiredArena;
	bool	EnsureArena(VkDeviceSize bytes);

	// The scratch vertex/index buffers DrawUP copies into. Same pair, same
	// reason, as VulkanEffectFile's; grown on demand and never shrunk.
	VulkanBuffer *pScratchVB, *pScratchIB;
	VkDeviceSize  scratchVBSize, scratchIBSize;
	bool	EnsureScratch(VkDeviceSize vbytes, VkDeviceSize ibytes);

	// ------------------------------------------------------------------
	// The rotation, counterpart of nothing in D3D9, which owned the constant
	// registers and sampler slots and recycled them itself. Resetting the
	// descriptor pool at the top of every frame is correct only with one frame
	// in flight; the core keeps orbiter_GetFramesInFlight() of them and its
	// vkWaitForFences covers only the slot it is about to record into, so the
	// driver answers with intermittent VK_ERROR_DEVICE_LOST. So what a frame
	// used is retired with that frame's number and taken back once the lag has
	// passed; only what vkPool / pArena / pScratchVB point at changes.
	struct FrameSet {
		VkDescriptorPool	pool;
		VulkanBuffer *		arena;
		VkDeviceSize		arenaSize;
		VulkanBuffer *		scratchVB;
		VkDeviceSize		scratchVBSize;
		VulkanBuffer *		scratchIB;
		VkDeviceSize		scratchIBSize;
		unsigned long long	retiredAt;
		FrameSet() : pool(VK_NULL_HANDLE), arena(NULL), arenaSize(0),
					 scratchVB(NULL), scratchVBSize(0),
					 scratchIB(NULL), scratchIBSize(0), retiredAt(0) {}
	};
	std::deque<FrameSet>	retired;	///< oldest first
	/// Samplers UpdateTextures replaced mid-frame, on the same lag. A sampler
	/// is an object in Vulkan and a device state word in D3D9, so this list
	/// has no counterpart to convert.
	std::deque<std::pair<VkSampler, unsigned long long> > retiredSampler;
	unsigned long long		frameNo;
	/// The sampler count CreateResources sized the pool with, kept so the
	/// rotation can size the next pool the same way.
	uint32_t				nPoolSamplers;
	/// \brief Build a fresh descriptor pool into vkPool. Called by
	///        CreateResources and again by the rotation when it has not yet
	///        come round.
	bool	CreateFrameSet();
	/// \brief The per-instance half of the static ResetFrame().
	void	RotateFrame();

	// ------------------------------------------------------------------
	// The descriptor machinery, none of which has a D3D9 counterpart: there a
	// constant went into a numbered register file and a texture onto a
	// numbered sampler slot, both device state the runtime kept. Here both
	// reach a shader only through a descriptor set, which has to be described
	// (a layout), allocated (from a pool) and written before the draw that
	// names it; without it every pipeline is built with setLayoutCount 0 and
	// can see neither a uniform block nor a texture. Binding convention, as
	// in IProcess: the vertex stage's block at 0, the pixel stage's at 1, and
	// the samplers wherever the GLSL declares them, read back by reflection.
	// ------------------------------------------------------------------

	/// \brief Build the set layout, the pipeline layout and the pool, once,
	///        from the reflection of the two compiled stages.
	bool	CreateResources();

	/// \brief Allocate one descriptor set, write the two uniform buffers and
	///        every sampler binding into it, and bind it. Called from
	///        UpdateTextures(), which is where every call site already says
	///        "what I have set should now reach the device".
	bool	BindResources();

	VkDescriptorPool		vkPool;
	VkDescriptorSet			vkSet;
	uint32_t				vsBinding;	///< where the VS block is declared
	uint32_t				psBinding;	///< where the PS block is declared
	bool					bResources;	///< CreateResources has run
	VulkanTexture		   *pWhite;		///< an unwritten binding samples this
	/// \brief The same, for a samplerCube binding. A cube sampler must be
	/// given a cube view; the 2D pWhite above faults the GPU.
	VulkanTexture		   *pWhiteCube;

	/// \brief Every live instance, so the static ResetFrame can walk them.
	static std::set<ShaderClass*> Instances;
};



inline void swap(double &a, double &b)
{
	double c = a; a = b; b = c;
}

// Jump between western and eastern hemispheres
inline double wrap(double a)
{
	if (a<-PI) return a+PI2;
	if (a>PI) return a-PI2;
	return a;
}

void LogMatrix(FMATRIX4 *pM, const char *name);
inline void LogSunLight(VulkanSun& s)
{
	LogAlw("Sunlight.Dir   = [%f, %f, %f]", s.Dir.x, s.Dir.y, s.Dir.z);
	LogAlw("Sunlight.Color = [%f, %f, %f]", s.Color.x, s.Color.y, s.Color.z);
	LogAlw("Sunlight.Ambie = [%f, %f, %f]", s.Ambient.x, s.Ambient.y, s.Ambient.z);
	LogAlw("Sunlight.Trans = [%f, %f, %f]", s.Transmission.x, s.Transmission.y, s.Transmission.z);
	LogAlw("Sunlight.Incat = [%f, %f, %f]", s.Incatter.x, s.Incatter.y, s.Incatter.z);
}

// -----------------------------------------------------------------------------------
// Conversion functions
//
// Several of these now convert between two types that are the same type --
// _FV took a D3DXVECTOR3 and returned an FVECTOR3, C2V took a D3DXCOLOR and
// returned a D3DXVECTOR4 -- and are kept as pass-throughs so their call sites
// need not be rewritten.
// ------------------------------------------------------------------------------------

// long(l) on Windows, LONG(l) here. RECT's fields are LONG, which is 32 bits
// on both platforms, but 'long' is 32 bits only on Win64 -- on Linux it is
// 64, so the Windows spelling narrows on every corner of every rectangle.
inline RECT _RECT(DWORD l, DWORD t, DWORD r, DWORD b)
{
	RECT rect = { LONG(l), LONG(t), LONG(r), LONG(b) };
	return rect;
}

inline VECTOR3 _V(FVECTOR3 &i)
{
	return _V(double(i.x), double(i.y), double(i.z));
}

inline oapi::FVECTOR3 _FV(FVECTOR3 &i)
{
	return oapi::FVECTOR3(i.x, i.y, i.z);
}

inline VECTOR3 _V(FVECTOR4 &i)
{
	return _V(double(i.x), double(i.y), double(i.z));
}

inline void COLORSWAP(FVECTOR4 *x)
{
	float a = x->r; x->r = x->b; x->b = a;
}

inline FVECTOR3 FVECTOR3f4(FVECTOR4 v)
{
	return FVECTOR3(v.x, v.y, v.z);
}

inline COLOUR4 COLOURMULT(const COLOUR4 *a, const COLOUR4 *b)
{
	COLOUR4 c;
	c.a = a->a * b->a;
	c.r = a->r * b->r;
	c.g = a->g * b->g;
	c.b = a->b * b->b;
	return c;
}

inline void MATRIX4toFMATRIX4 (const MATRIX4 &M, FMATRIX4 &D)
{
	D.m11 = (float)M.m11;  D.m12 = (float)M.m12;  D.m13 = (float)M.m13;  D.m14 = (float)M.m14;
	D.m21 = (float)M.m21;  D.m22 = (float)M.m22;  D.m23 = (float)M.m23;  D.m24 = (float)M.m24;
	D.m31 = (float)M.m31;  D.m32 = (float)M.m32;  D.m33 = (float)M.m33;  D.m34 = (float)M.m34;
	D.m41 = (float)M.m41;  D.m42 = (float)M.m42;  D.m43 = (float)M.m43;  D.m44 = (float)M.m44;
}

inline MATRIX4 _MATRIX4(const FMATRIX4 *M)
{
	MATRIX4 D;
	D.m11 = (double)M->m11;  D.m12 = (double)M->m12;  D.m13 = (double)M->m13;  D.m14 = (double)M->m14;
	D.m21 = (double)M->m21;  D.m22 = (double)M->m22;  D.m23 = (double)M->m23;  D.m24 = (double)M->m24;
	D.m31 = (double)M->m31;  D.m32 = (double)M->m32;  D.m33 = (double)M->m33;  D.m34 = (double)M->m34;
	D.m41 = (double)M->m41;  D.m42 = (double)M->m42;  D.m43 = (double)M->m43;  D.m44 = (double)M->m44;
	return D;
}

inline void TransformVertex(NMVERTEX *pVrt, const FMATRIX4 *pW)
{
	FVECTOR3 p = oapi::TransformCoord (FVECTOR3(pVrt->x, pVrt->y, pVrt->z), *pW);
	FVECTOR3 n = oapi::TransformNormal(FVECTOR3(pVrt->nx, pVrt->ny, pVrt->nz), *pW);
	FVECTOR3 t = oapi::TransformNormal(FVECTOR3(pVrt->tx, pVrt->ty, pVrt->tz), *pW);
	pVrt->x  = p.x;	pVrt->y  = p.y; pVrt->z  = p.z;
	pVrt->nx = n.x; pVrt->ny = n.y; pVrt->nz = n.z;
	pVrt->tx = t.x; pVrt->ty = t.y; pVrt->tz = t.z;
}

inline void FVEC (const VECTOR3 &v, FVECTOR3 &d3dv)
{
	d3dv.x = (float)v.x;
	d3dv.y = (float)v.y;
	d3dv.z = (float)v.z;
}

inline FVECTOR4 C2V(const FVECTOR4 &v)
{
	return FVECTOR4(v.r, v.g, v.b, v.a);
}

inline FVECTOR3 FVEC(const VECTOR3 &v)
{
	return FVECTOR3(float(v.x), float(v.y), float(v.z));
}

inline FVECTOR3 FVEC(const VECTOR4 &v)
{
	return FVECTOR3(float(v.x), float(v.y), float(v.z));
}

inline FVECTOR4 FVEC4(const VECTOR3 &v, float w)
{
	return FVECTOR4(float(v.x), float(v.y), float(v.z), w);
}

inline FVECTOR4 _FCOLOR(const VECTOR3 &v, float a = 1.0f)
{
	return FVECTOR4(float(v.x), float(v.y), float(v.z), a);
}

/// \brief `D3DXCOLOR(DWORD)`, which unpacks 0xAARRGGBB into four floats.
///
///        FVECTOR4 already has a DWORD constructor, but it reads the DWORD as
///        0xAABBGGRR -- ABGR, not ARGB -- while a D3DCOLOR is ARGB. So a
///        `D3DXCOLOR(c)` converted to `FVECTOR4(c)` compiles, looks right and
///        exchanges red and blue. Call sites that mean ARGB call this.
inline FVECTOR4 FCOLOR_ARGB(DWORD c)
{
	const float q = 1.0f / 255.0f;
	return FVECTOR4(float((c >> 16) & 0xFF) * q,	// r
					float((c >>  8) & 0xFF) * q,	// g
					float( c        & 0xFF) * q,	// b
					float((c >> 24) & 0xFF) * q);	// a
}

inline VECTOR3 _VF(const FVECTOR3 &v)
{
	return _V(double(v.x), double(v.y), double(v.z));
}

inline VECTOR4 _VF4(const FVECTOR4 &v)
{
	return _V(double(v.x), double(v.y), double(v.z), double(v.w));
}

inline float FVAL (double x)
{
	return (float)x;
}

int fgets2(char *buf, int cmax, FILE *file, DWORD param=0);

float Vec3Angle(FVECTOR3 a, FVECTOR3 b);
FVECTOR3 Perpendicular(FVECTOR3 *a);

const char *RemovePath(const char *in);
SketchMesh * GetSketchMesh(const MESHHANDLE hMesh);

// D3D9 had a separate interface for 3D textures (LPDIRECT3DVOLUMETEXTURE9);
// Vulkan has one image type and a VK_IMAGE_TYPE_3D flag, so the output is
// the same VulkanTexture the inputs are.
bool CreateVolumeTexture(VulkanDevice *pDevice, int count, VulkanTexture **pIn, VulkanTexture **pOut);

void CreateMatExt(const MATERIAL *pIn, VulkanMatExt *pOut);
void UpdateMatExt(const MATERIAL *pIn, VulkanMatExt *pOut);
void CreateDefaultMat(VulkanMatExt *pOut);
void GetMatExt(const VulkanMatExt *pIn, MATERIAL *pOut);
// D3D9 took LPDIRECT3DRESOURCE9 and branched on GetType() because vertex and
// index buffers were different interfaces. Vulkan has one VkBuffer, told
// apart only by the usage flags given at creation, so the branch is gone.
bool CopyBuffer(VulkanBuffer *pDst, VulkanBuffer *pSrc);
void VulkanTuneInit(VulkanTune *);
int LoadPlanetTextures(const char* fname, VulkanTexture** ppdds, DWORD flags, int amount);
float SunOcclusionByPlanet(OBJHANDLE hObj, VECTOR3 gpos);
float OcclusionFactor(float x, float sunrad, float plnrad);
float OcclusionFactor(float x, float r1, float r2, bool bReverse);
double Distance(vObject *a, vObject* b);
bool IsCastingShadows(vObject* body, vObject* ref, double* sunsize_out);

VkShaderModule CompilePixelShader(VulkanDevice *pDev, const char *file, const char *function, const char* name, const char *options, ShaderReflection **pConst);
VkShaderModule CompileVertexShader(VulkanDevice *pDev, const char *file, const char *function, const char* name, const char *options, ShaderReflection **pConst);

DWORD BuildDate();

// ------------------------------------------------------------------------------------
// Vector and matrix operations
//
// D3DMAT_ became VMAT_. These are the client's own functions -- only the
// prefix referred to Direct3D.
// ------------------------------------------------------------------------------------

float VMAT_BSScaleFactor(const FMATRIX4 *mat);
void VMAT_Identity (FMATRIX4 *mat);
void VMAT_ZeroMatrix(FMATRIX4 *mat);
void VMAT_Copy (FMATRIX4 *tgt, const FMATRIX4 *src);
void VMAT_SetRotation (FMATRIX4 *mat, const MATRIX3 *rot);
void VMAT_SetInvRotation (FMATRIX4 *mat, const MATRIX3 *rot);
void VMAT_RotationFromAxis (const FVECTOR3 &axis, float angle, FMATRIX4 *rot);
void VMAT_FromAxis(FMATRIX4 *out, const FVECTOR3 *x, const FVECTOR3 *y, const FVECTOR3 *z);
void VMAT_FromAxis(FMATRIX4 *out, const VECTOR3 *x, const VECTOR3 *y, const VECTOR3 *z);
void VMAT_FromAxisT(FMATRIX4 *out, const FVECTOR3 *x, const FVECTOR3 *y, const FVECTOR3 *z);
void VMAT_CreateX_Billboard(const FVECTOR3 *toCam, const FVECTOR3 *pos, float scale, FMATRIX4 *pOut);
void VMAT_CreateX_Billboard(const FVECTOR3 *toCam, const FVECTOR3 *pos, const FVECTOR3 *dir, float size, float stretch, FMATRIX4 *pOut);

// Set up a as matrix for ANTICLOCKWISE rotation r around x/y/z-axis
void VMAT_RotX (FMATRIX4 *mat, double r);
void VMAT_RotY (FMATRIX4 *mat, double r);

void VMAT_SetTranslation (FMATRIX4 *mat, const VECTOR3 *trans);
void VMAT_SetTranslation(FMATRIX4 *mat, const FVECTOR3 *trans);
bool VMAT_VectorMatrixMultiply (FVECTOR3 *res, const FVECTOR3 *v, const FMATRIX4 *mat);

// ------------------------------------------------------------------------------------
// The D3DX matrix functions the client used, written out because D3DX was a
// utility library and Vulkan ships no counterpart. They live here rather than
// in whichever file needed one first: a 4x4 multiply written once per file is
// how two of them end up disagreeing about row or column order.
//
// Row-vector convention, which is D3DX's and what the whole client and every
// one of its shaders assumes: a point is a row multiplied on the left,
// v' = v * M, so out = a * b applies a first and then b. The SDK's own
// mul(FVECTOR4, FMATRIX4) already does it that way, which is what makes
// FMATRIX4 a drop-in for D3DXMATRIX rather than a transpose of one.
// ------------------------------------------------------------------------------------

/// \brief out = a * b. Counterpart of D3DXMatrixMultiply.
///        Safe to alias: out may be either operand.
void VMAT_MatrixMultiply (FMATRIX4 *out, const FMATRIX4 *a, const FMATRIX4 *b);

/// \brief Counterpart of D3DXMatrixTransformation2D. Builds, in D3DX's order,
///            M = T(-Csc) * Rsc^-1 * S * Rsc * T(Csc) * T(-Crot) * R * T(Crot) * T(t)
///        Every pointer may be NULL, meaning the identity for that term, as
///        D3DX defines it.
void VMAT_Transformation2D (FMATRIX4 *out,
							const FVECTOR2 *pScalingCenter, float scalingRotation,
							const FVECTOR2 *pScaling,
							const FVECTOR2 *pRotationCenter, float rotation,
							const FVECTOR2 *pTranslation);

// Returns S_OK / E_INVALIDARG: Win32 HRESULT values the Linux shim still
// supplies, and this function's own return value rather than a device's, so
// they are not VkResult and HR() does not apply.
HRESULT VMAT_MatrixInvert (FMATRIX4 *res, FMATRIX4 *a);

/// \brief Counterpart of D3DXMatrixOrthoOffCenterLH. The formula is D3DX's
///        unchanged: its depth half is already right, since a left-handed D3D
///        ortho maps z into [0,1], which is Vulkan's range too. The Y half is
///        deliberately left alone -- Vulkan's NDC has +Y down where D3D's
///        points up, so a matrix built here draws mirrored unless the caller's
///        top/bottom arguments account for it, which
///        Scene::ComputeLocalLightsVisibility's (0, w, h, 0) do.
void VMAT_OrthoOffCenterLH (FMATRIX4 *out, float l, float r, float b, float t,
							float zn, float zf);

/// \brief Counterpart of D3DXMatrixOrthoOffCenterRH: the LH form with m33
///        negated, which is the whole of what "handed" means for an
///        orthographic projection.
void VMAT_OrthoOffCenterRH (FMATRIX4 *out, float l, float r, float b, float t,
							float zn, float zf);

/// \brief Counterpart of D3DXMatrixLookAtRH, transcribed from the D3DX
///        documentation's construction rather than rederived.
void VMAT_LookAtRH (FMATRIX4 *out, const FVECTOR3 *pEye, const FVECTOR3 *pAt,
					const FVECTOR3 *pUp);


// ------------------------------------------------------------------------------------
// Vertex formats
// ------------------------------------------------------------------------------------
struct VERTEX_XYZ { float x, y, z; };                   // transformed vertex
struct VERTEX_XYZC { float x, y, z; DWORD col; };       // untransformed vertex with single colour component

// untransformed lit vertex with texture coordinates
struct VERTEX_XYZ_TEX {
	float x, y, z;
	float tu, tv;
};

// untransformed unlit vertex with two sets of texture coordinates
struct VERTEX_2TEX {
	float x, y, z, nx, ny, nz;
	float tu0, tv0, e;
	inline VERTEX_2TEX() : x(0.0f), y(0.0f), z(0.0f), nx(0.0f), ny(0.0f), nz(0.0f),
		tu0(0.0f), tv0(0.0f), e(0.0f) {}
	inline VERTEX_2TEX(const FVECTOR3& p, const FVECTOR3& n, float u0, float v0, float u1, float v1)
		: x(p.x), y(p.y), z(p.z), nx(n.x), ny(n.y), nz(n.z),
		tu0(u0), tv0(v0), e(0.0f) {}
};

// The strides the vertex declarations above describe. Each offset in those
// tables was written by hand on Windows against these structs, and nothing
// checked that the two agreed. Now something does.
static_assert(sizeof(BAVERTEX) == 52, "BAVertexDecl offsets assume a 52-byte BAVERTEX");
static_assert(sizeof(NTVERTEX) == 32, "NTVertexDecl offsets assume a 32-byte NTVERTEX");
static_assert(sizeof(NMVERTEX) == 48, "MeshVertexDecl offsets assume a 48-byte NMVERTEX");
static_assert(sizeof(VERTEX_2TEX) == 36, "PatchVertexDecl offsets assume a 36-byte VERTEX_2TEX");
static_assert(sizeof(SMVERTEX) == 20, "PosTexDecl offsets assume a 20-byte SMVERTEX");
static_assert(sizeof(VERTEX_XYZC) == 16, "PosColorDecl offsets assume a 16-byte VERTEX_XYZC");
static_assert(sizeof(VERTEX_XYZ) == 12, "PositionDecl offsets assume a 12-byte VERTEX_XYZ");
static_assert(sizeof(GPUBLITVTX) == 8, "GPUBlitDecl offsets assume an 8-byte GPUBLITVTX");
static_assert(sizeof(LocalLightsCompute) == 20, "LocalLightsDecl offsets assume a 20-byte LocalLightsCompute");

// -----------------------------------------------------------------------------------
// String helper
// ------------------------------------------------------------------------------------

// trim from start
std::string &ltrim (std::string &s);

// trim from end
std::string &rtrim (std::string &s);

// trim from both ends
std::string &trim (std::string &s);

// uppercase complete string
void toUpper (std::string &s);

// lowercase complete string
//void toLower (std::string &s);

// string to double (returns quiet_NaN if conversion failed)
double toDoubleOrNaN (const std::string &str);

// case insensitive compare
bool startsWith (const std::string &haystack, const std::string &needle);

// case insensitive contains
bool contains (const std::string &haystack, const std::string &needle);

// case insensitive find
size_t find_ci (const std::string &haystack, const std::string &needle);

// case insensitive rfind
size_t rfind_ci (const std::string &haystack, const std::string &needle);

// parse assignments like "foo=bar", "foo = bar" or even "foo= bar ; with comment"
std::pair<std::string, std::string> &splitAssignment (const std::string &line, const char delim = '=');

// replace all occurrences of 's' in 'subj' by 't'
std::string::size_type replace_all (std::string &subj, const std::string &s, const std::string &t);

// -----------------------------------------------------------------------------------
// Resource handling
// ------------------------------------------------------------------------------------
//
// AutoHandle is gone: its whole body is Win32 kernel-handle management --
// HANDLE, INVALID_HANDLE_VALUE, CloseHandle -- and Linux has no such object.
// It is dropped rather than emulated because it has no call sites; the whole
// D3D9Client tree names it only where it is defined.

/**
 * \brief Kind of auto_file.
 * This simple wrapper acts like 'auto_ptr' but is for FILE pointer. It is used
 * to avoid any not-closed FILES leaks when the block scope is left (via thrown
 * exception or return e.g.)
 */
struct AutoFile
{
	FILE* pFile; ///< The file handle storage

	AutoFile () {
		pFile = NULL;
	}

	~AutoFile () {
		ForceClose();
	}

	/**
	 * \brief Check whether the file is not valid
	 */
	bool IsInvalid () {
		return pFile == NULL;
	}

	/**
	 * \brief Close the file
	 */
	void ForceClose()
	{
		if (!IsInvalid()) {
			fclose(pFile);
		}
		pFile = NULL;
	}
};


// ------------------------------------------------------------------------------------
// Miscellaneous helper functions
// ------------------------------------------------------------------------------------
//
// SAFE_RELEASE is gone with the COM reference counting it drove: a Vulkan
// object is destroyed with an explicit vkDestroy* against the device that
// made it, so a release macro taking only the object could not do the job.

#define DELETE_SURFACE(p) { if (p) { delete ((SurfNative*)p); p = NULL; } }
#define SAFE_DELETE(p)  { if(p) { delete (p);     (p)=NULL; } }
#define SAFE_DELETEA(p)  { if(p) { delete []p;     (p)=NULL; } }
#define CLEARARRAY(p) { memset(p, 0, sizeof(p)); }

#endif // !__VULKANUTIL_H
