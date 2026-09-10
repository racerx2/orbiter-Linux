// ==============================================================
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2006-2026 Martin Schweiger
//				 2012-2016 Jarmo Nikkanen
// ==============================================================
//
// CONVERTED FROM OVP/D3D9Client/D3D9Util.h, read end to end (776 lines).
// What changed, and why:
//
//  1. <d3d9.h> AND <d3dx9.h> BECAME "VulkanTypes.h". Those were system
//     headers; their Linux counterpart is <vulkan/vulkan.h> plus the small
//     set of names Vulkan does not bundle. VulkanTypes.h explains that split
//     in full and defines nothing else.
//
//  2. THE D3DX MATH TYPES BECAME THE SDK's OWN. D3DXVECTOR2/3/4 ->
//     FVECTOR2/3/4, D3DXMATRIX/LPD3DXMATRIX -> FMATRIX4, D3DXCOLOR ->
//     FVECTOR4 (which carries .r/.g/.b/.a and the same DWORD packing),
//     D3DCOLORVALUE -> COLOUR4, D3DVECTOR -> FVECTOR3, D3DMATERIAL9 ->
//     MATERIAL. The last three are not approximations: COLOUR4 is
//     {float r,g,b,a} and MATERIAL is four COLOUR4 plus a float, which is
//     D3DCOLORVALUE and D3DMATERIAL9 field for field.
//
//  3. THE SHADER-FACING STRUCTS KEEP THEIR EXACT BYTE LAYOUT, and this was
//     checked rather than assumed. VulkanSun/VulkanMatExt/VulkanTune sit
//     inside #pragma pack(push,4) as they did on Windows, but FVECTOR4 and
//     FMATRIX4 are alignas(16) where D3DXVECTOR4 was not, so the packed
//     layout could have silently changed -- a wrong material is not a
//     crash, it is a wrong colour. Measured on this toolchain: #pragma
//     pack(4) does lower alignas(16) for members, and the offsets come out
//     identical (0,16,32,44,56,68,80,92,100,104,120 / 124 bytes). The
//     static_asserts below make that a build error rather than a surprise
//     anywhere it is not true.
//
//  4. THE VERTEX DECLARATIONS BECAME VULKAN VERTEX INPUT DESCRIPTIONS.
//     D3DVERTEXELEMENT9 binds a byte range to an HLSL semantic name;
//     Vulkan matches vertex inputs by an explicit location number, so the
//     semantic had to become a number. The rule is mechanical: LOCATION =
//     THE ELEMENT'S INDEX IN THE DECLARATION, in the order the Windows
//     declaration listed them. Offsets are carried over unchanged.
//
//     The stride is new. D3D9 never put it in the declaration -- it came
//     from SetStreamSource at bind time -- but a VkVertexInputBindingDescription
//     needs it, so each declaration now names the struct it describes via
//     sizeof(). That is why the VertexDecl objects are defined in
//     VulkanUtil.cpp: the strides need the vertex structs, which the
//     Windows file declares AFTER the declaration arrays.
//
//     They also moved file. On Windows the pXxxDecl globals lived in
//     D3D9Frame.cpp and were built by pDevice->CreateVertexDeclaration(),
//     because a D3D9 vertex declaration is a device object whose lifetime
//     is the device's. A Vulkan vertex layout is not an object at all -- it
//     is immutable data baked into a VkPipeline -- so there is nothing to
//     create, nothing to release, and no device to tie it to. They are
//     constants now, and they live with the data that describes them.
//
//  5. EVERY NAME CONTAINING D3D IS GONE. D3D9* client types became Vulkan*,
//     the D3DMAT_ matrix helpers became VMAT_, and the handful of helpers
//     named after D3DX entry points (D3DXVec3Angle, D3DXVEC, D3DXC2V,
//     _D3DXCOLOR, D3DVAL...) took names that describe what they do. These
//     are the client's own functions; only their names referred to
//     something that no longer exists.
//
//  6. HR() NOW CHECKS A VkResult. It wrapped D3D9 calls returning HRESULT;
//     after conversion those call sites are Vulkan calls returning
//     VkResult, so the macro follows them. The Win32 HRESULT names the shim
//     supplies (S_OK, E_INVALIDARG) are still used where the Windows file
//     used them for its OWN return values rather than a device's --
//     VMAT_MatrixInvert is the only such case.
//
//  7. TWO BUGS IN THE WINDOWS SOURCE ARE FIXED HERE, both recorded at the
//     point of change in VulkanUtil.cpp: ShaderClass::SetPSConstants(HANDLE)
//     wrote through the VERTEX shader constant table, and startsWith() had
//     the body of contains(). Neither is a platform difference; both would
//     have been carried forward silently.
//
// NOT CONVERTED, because none of it needed converting: the string helpers,
// AutoFile, fgets2 and the scalar maths. They are plain C++ and portable as
// written.
// ==============================================================

#ifndef __VULKANUTIL_H
#define __VULKANUTIL_H

#include "OrbiterAPI.h"
#include "Log.h"
#include "DrawAPI.h"
#include "VulkanTypes.h"
#include <string>
#include <map>      // the ShaderClass pipeline cache; D3D9 needed none
#include <deque>    // the per-frame resource rotation; see ShaderClass::ResetFrame()
#include <set>      // the ShaderClass instance registry; see ResetFrame()
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


// Counterpart of the HRESULT form. The wrapped call sites are Vulkan calls
// now, so the checked type is VkResult and the test is an equality against
// VK_SUCCESS rather than FAILED(): Vulkan's positive results (VK_SUBOPTIMAL_KHR,
// VK_TIMEOUT, VK_NOT_READY) are not failures but they are not "carry on
// blindly" either, and FAILED() would have waved them through.

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
// Each entry below was a D3DVERTEXELEMENT9:
//     { Stream, Offset, Type, Method, Usage, UsageIndex }
// and is now a VertexAttrib:
//     { location, format, offset }
//
//   Stream      -- dropped. Every declaration in this client used stream 0,
//                  and the binding number now lives in VertexDecl::Binding().
//   Method      -- dropped. D3DDECLMETHOD_DEFAULT was the only value used,
//                  and it means "no tessellator", which is Vulkan's only
//                  behaviour for a vertex input.
//   Usage +
//   UsageIndex  -- became 'location'. POSITION0/NORMAL0/TEXCOORD0... are HLSL
//                  semantic names and Vulkan has none; the location is the
//                  element's index in the declaration, counting from 0 in the
//                  order the Windows declaration listed them.
//   Type        -- became a VkFormat:
//                    D3DDECLTYPE_FLOAT1    VK_FORMAT_R32_SFLOAT
//                    D3DDECLTYPE_FLOAT2    VK_FORMAT_R32G32_SFLOAT
//                    D3DDECLTYPE_FLOAT3    VK_FORMAT_R32G32B32_SFLOAT
//                    D3DDECLTYPE_FLOAT4    VK_FORMAT_R32G32B32A32_SFLOAT
//                    D3DDECLTYPE_D3DCOLOR  VK_FORMAT_B8G8R8A8_UNORM
//                    D3DDECLTYPE_SHORT4    VK_FORMAT_R16G16B16A16_SINT
//   Offset      -- carried over unchanged.
//
// Two of those type mappings are worth spelling out because getting them
// wrong produces a picture rather than an error:
//
//   D3DCOLOR is a DWORD written 0xAARRGGBB. Read back byte by byte on a
//   little-endian machine that is B,G,R,A, which is exactly
//   VK_FORMAT_B8G8R8A8_UNORM -- not R8G8B8A8, which would swap red and blue.
//
//   D3DDECLTYPE_SHORT4 delivers four signed shorts to the shader as floats
//   WITHOUT normalising them. Vulkan's exact equivalent is
//   VK_FORMAT_R16G16B16A16_SSCALED, but SSCALED vertex formats are optional
//   and widely unsupported, so this uses _SINT and the GLSL translation
//   declares the input as ivec4 and converts. The values are pixel
//   coordinates (GPUBLITVTX), so the conversion is exact either way.
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
// THE SHADER-FACING STRUCTS AND WHY THEY ARE PACKED
//
// LightStruct, VulkanSun, VulkanMatExt and VulkanTune are not ordinary
// structs: they are copied raw into shader constant storage and read back by
// a matching struct in the shader source. Mesh.cpp does
//
//     FX->SetValue(eLights, Locals, sizeof(LightStruct) * MaxLights)
//     FX->SetValue(eSun,   &sunLight, sizeof(D3D9Sun))
//
// against 'struct Light', 'struct Sun', 'struct Mtrl' and 'struct Tune' in
// shaders/D3D9Client.fx. D3DX copies those field by field from a TIGHTLY
// PACKED source, which is why the Windows LightStruct is 76 bytes and not
// the 96 an HLSL constant-register layout would need.
//
// Two things had to be handled to keep that true on Linux:
//
//   FVECTOR4 AND FMATRIX4 ARE alignas(16); D3DXVECTOR4 AND D3DXMATRIX WERE
//   NOT. Dropped into a struct unchanged, Diffuse would move from offset 8
//   to 16 and LightStruct would grow from 76 to 96 bytes -- and nothing
//   would report it, because a misaligned light is a picture, not an error.
//   The three structs Windows already packed keep their #pragma pack(4);
//   LightStruct, which Windows did not need to pack, gets one. This was
//   measured on this toolchain rather than assumed: #pragma pack(4) does
//   lower alignas(16) for members here. The static_asserts below turn any
//   toolchain where it does not into a build error.
//
//   VULKAN'S DEFAULT UNIFORM LAYOUT IS NOT TIGHT PACKING. GLSL std140 would
//   put vec4 diffuse at offset 16 and make Light 96 bytes, exactly the
//   mismatch above. Rather than pad every struct here and every struct in
//   the shaders to std140 -- which would change the data as well as its
//   spelling -- the GLSL translations declare these blocks
//   layout(scalar), from GL_EXT_scalar_block_layout / VkPhysicalDevice
//   scalarBlockLayout, core since Vulkan 1.2. Scalar layout is C layout, so
//   these structs stay byte for byte what the Windows client uploaded.
// ------------------------------------------------------------------------------------

// ------------------------------------------------------------------------------------
// FVECTOR4P -- a 4-byte-aligned FVECTOR4, and why the packed structs must use
// it instead of FVECTOR4 itself.
//
// THIS FIXED A CRASH, so the reasoning is worth keeping in full.
//
// An earlier version of this header put FVECTOR4 straight into the packed
// structs and silenced -Wpacked-not-aligned, on the grounds that the pack
// lowering alignas(16) is exactly what keeps the Windows layout and that the
// static_asserts below hold that layout. The layout half of that is true. The
// conclusion was wrong, and the warning was reporting a real defect:
//
//   THE static_asserts HOLD THE LAYOUT. NOTHING HELD THE ACCESS ALIGNMENT.
//
// #pragma pack(4) lowers the alignment of the MEMBER. It does not change the
// TYPE: FVECTOR4 is still declared alignas(16), so the compiler is entitled to
// assume any FVECTOR4 lvalue is 16-byte aligned. In VulkanMatExt, SpecialFX
// sits at offset 104 (8 mod 16); in LightStruct, Diffuse at 8 and Param at 60.
// GCC coalesces FVECTOR4's constructor -- `r = g = b = a = 0.0f` -- into a
// single 16-byte aligned store, and at -O2 `new VulkanMatExt[1]` SEGFAULTS in
// that constructor. Reproduced standalone: -O0 and -O1 survive, -O2 faults, on
// a struct with exactly these offsets. It crashed vObject::GlobalInit's very
// first mesh load, so the client could not open a scenario at all.
//
// D3DXVECTOR4 was a plain 4-float type with no over-alignment, which is why
// Windows never had this. So the faithful counterpart of D3DXVECTOR4 inside a
// packed struct is a 4-byte-aligned 4-float type -- this one -- not the SDK's
// over-aligned FVECTOR4. The bytes are identical, so every static_assert
// below still holds and the shader upload is unchanged.
//
// It converts implicitly both ways, so call sites keep reading and writing
// FVECTOR4 exactly as they did.
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

// No -Wpacked-not-aligned suppression any more. With FVECTOR4P in place there
// is nothing over-aligned left to lower, so the warning stays ON and will
// report it if an over-aligned type is ever put back into one of these.
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
	// Diffuse and Specular sit at 0 and 16, which look safely aligned -- and
	// are not. The STRUCT's alignment is 4 (the pack), and sizeof is 124, so
	// in `new VulkanMatExt[n]` element 1 begins at offset 124: Diffuse then
	// lands at 12 mod 16 and Specular at 12 mod 16 again. An over-aligned type
	// is unsafe ANYWHERE in a packed struct, not merely at an odd offset.
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
// The vertex declarations themselves.
//
// On Windows these were IDirect3DVertexDeclaration9* globals, defined in
// D3D9Frame.cpp and built there by pDevice->CreateVertexDeclaration() --
// device objects, created with the device and released with it. A Vulkan
// vertex layout is not an object: it is immutable data compiled into a
// VkPipeline. There is nothing to create, nothing to release and no device
// to tie it to, so these are constants defined once in VulkanUtil.cpp, next
// to the tables and the strides that describe them, and the eleven
// CreateVertexDeclaration calls in D3D9Frame.cpp have no counterpart.
//
// The names and the pointer spelling are kept so that the call sites that
// bind them do not change shape.
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
// ShaderClass
//
// Same class, same job, same call sites. What changed underneath:
//
//   LPDIRECT3DPIXELSHADER9 / LPDIRECT3DVERTEXSHADER9 became VkShaderModule.
//
//   LPD3DXCONSTANTTABLE became ShaderReflection. D3DX built the constant
//   table as a side product of compiling HLSL; the Vulkan counterpart is
//   reflection over the SPIR-V module, which carries its own interface
//   description. The type is per-stage exactly as the two D3DX tables were.
//
//   Setup() now builds or fetches a VkPipeline. In D3D9 it set the shader
//   pair, the vertex declaration and eight render states as independent
//   pieces of device state. Vulkan has no such state: the shaders, the
//   vertex layout, the depth test and the blend equation are all baked into
//   one immutable VkPipeline at creation time. Its arguments -- the
//   declaration, the depth flag and the blend mode -- are therefore exactly
//   the pipeline's key, which is why they can be cached on those three
//   values and nothing else.
//
//   The viewport survives as a call rather than as pipeline state, because
//   it is one of the few pieces Vulkan does keep dynamic.
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
	///        with. Defaults to TRIANGLE_LIST, which is what every caller
	///        wanted until HazeManager2.
	///
	///        NEW, AND NOT AN ADDITION TO THE MODEL -- the same value existed
	///        on Windows as DrawPrimitive's first argument, chosen per draw.
	///        Vulkan bakes topology into the VkPipeline, so it has to be
	///        known before Setup() binds one, and it joins the pipeline cache
	///        key for the same reason the declaration and the blend mode do.
	///        Exactly the reasoning behind VulkanEffectFile::SetTopology; see
	///        that one for the longer version.
	void	SetTopology(VkPrimitiveTopology topo) { topology = topo; }

	/// \brief The three values D3DRS_CULLMODE can take.
	///
	///        SAME THREE VALUES AS VulkanEffectFile::PassOverride::CullMode,
	///        and deliberately the same numbering, but declared again here
	///        rather than shared: VulkanEffect.h includes this header, so
	///        naming its enum from here would be a cycle. The translation
	///        table -- and the reason the two Vulkan fields are not
	///        independent -- is written out once in VulkanEffect.cpp's
	///        GetPipeline and once in this class's, beside each use.
	enum CullMode { CULL_NONE = 0, CULL_CW = 1, CULL_CCW = 2 };

	/// \brief The cull mode the next Setup() builds its pipeline with.
	///        Defaults to CULL_NONE, which is what every pipeline this class
	///        built had until Surfmgr2.
	///
	///        NEW, AND FOR EXACTLY THE REASON SetTopology IS. The value
	///        existed on Windows as a SetRenderState(D3DRS_CULLMODE, ...)
	///        issued AFTER Setup() and before the draw --
	///        TileManager2<SurfTile>::Render does precisely that, asking for
	///        D3DCULL_CCW so the far side of the planet is not drawn. Vulkan
	///        bakes the cull into the VkPipeline, so it has to be known
	///        before Setup() binds one, and it joins the pipeline cache key
	///        for the same reason the declaration and the blend mode do.
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
	///        ShaderClass-bound draw -- "draw from this pointer in my memory".
	///
	///        THE SAME FUNCTION VulkanEffectFile ALREADY HAS, and it is here
	///        because the second family of call sites reaches the device
	///        directly rather than through an effect: Scene.cpp's
	///        ComputeLocalLightsVisibility and RenderGlares both do
	///        `pDevice->DrawPrimitiveUP(D3DPT_POINTLIST, ...)` immediately
	///        after a ShaderClass::Setup. Vulkan has no such call and cannot:
	///        a draw sources its vertices from a VkBuffer bound to the command
	///        buffer, so the caller's array has to reach one. This copies it
	///        into a scratch buffer and draws from that, which is what the
	///        D3D9 runtime did behind DrawPrimitiveUP anyway.
	///
	///        pIdx may be NULL for the unindexed form, which is what both
	///        current callers want. THE COUNT IS VERTICES, not primitives --
	///        the same trap as VulkanEffectFile::DrawUP's.
	void	DrawUP(const void *pVtx, UINT nVtx, UINT stride,
				   const WORD *pIdx = NULL, UINT nIdx = 0);

	/// \brief Release every instance's descriptor sets. Must be called once
	///        per frame, from the client's frame boundary.
	///
	///        NEW, AND THE COUNTERPART OF NOTHING. A D3D9 SetTexture consumed
	///        its argument immediately -- the device recorded the binding and
	///        the call was over. A Vulkan draw only records a REFERENCE to a
	///        descriptor set, so a set stays live until the frame that named
	///        it has been submitted, and each draw needs its own. They are
	///        allocated from a pool per instance and the pool is recycled
	///        here, which is the same arrangement, for the same reason, as
	///        VulkanEffectFile::ResetFrame.
	///
	///        Static and over a registry of instances because there are
	///        several ShaderClass objects -- one per tile manager, two for
	///        the haze, two in Scene -- with no single owner to walk them.
	static void	ResetFrame();

private:

	// pSampler is new. D3D9 set eight sampler states on a numbered device slot
	// and the runtime kept them; Vulkan has no such slot -- a sampler is an
	// object built from those same eight parameters, so the slot has to own
	// one. bSamplerSet still means "the parameters have not changed since the
	// last time", exactly as it did; it now guards a create instead of eight
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
	///        Those three are exactly what Setup() varies, so they are the
	///        whole key -- see the note on Setup() above.
	VkPipeline	GetPipeline(const VertexDecl *pDecl, bool bZ, int blend);

	/// \brief Counterpart of ID3DXConstantTable::SetValue: put 'bytes' bytes
	///        at the variable's offset in its stage's uniform block.
	bool		WriteConstants(ShaderReflection *pCB, const void *pVar, void *data, UINT bytes);

	ShaderReflection *pPSCB, *pVSCB;
	VkShaderModule pPS;
	VkShaderModule pVS;
	VulkanDevice *pDev;
	std::string fn, psn, vsn, sn;

	// The pipeline cache, keyed on Setup()'s three arguments. D3D9 needed no
	// such thing because it had no pipeline object: the equivalent state was
	// set per draw and thrown away.
	struct PipeKey {
		const VertexDecl *pDecl;
		bool bZ;
		int blend;
		VkPrimitiveTopology topo;
		int cull;
		// THE RENDER PASS IS PART OF THE KEY, and it has to be. A VkPipeline
		// may only be bound inside a render pass COMPATIBLE with the one it
		// was created against, and compatibility requires the attachment
		// formats to match -- so the same shader pair drawn into the
		// swapchain and into an R32_SFLOAT shadow map are two pipelines.
		// D3D9 needed nothing like this: SetRenderTarget was device state
		// and a shader did not care what it pointed at. See
		// VulkanDevice::GetRenderPass and BeginOffscreen.
		VkRenderPass pass;
		// D3DRS_FILLMODE, for the same reason: it was global device state and
		// is pipeline state here. See VulkanDevice::SetPolygonMode.
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

	// THE UNIFORM BLOCKS, AND WHY THEY ARE A PER-FRAME ARENA RATHER THAN TWO
	// BUFFERS.
	//
	// This used to be `VulkanBuffer *pPSConst, *pVSConst;` -- one buffer per
	// stage, written by WriteConstants at the constant's offset and bound at
	// offset 0 by BindResources. That is correct for ONE draw and wrong for a
	// frame, because a Vulkan descriptor does not COPY anything: it points at
	// memory the GPU reads when the frame is submitted.
	//
	// A frame draws ~2000 surface tiles through one ShaderClass. Each tile
	// wrote its own Prm.mWorld into that single buffer and issued its draw;
	// the CPU wrote 2000 times and the GPU, executing every draw at submit,
	// read whatever the LAST tile left behind. So every tile was transformed
	// by one tile's world matrix, all of them collapsing onto one another --
	// which is why the planet never appeared while every other measurement
	// (draw issued, buffers valid, mWorld correct, block layout exact) came
	// back right.
	//
	// D3D9 needed none of this: ID3DXConstantTable::SetValue wrote into the
	// device's constant REGISTERS and the runtime snapshotted register state
	// per draw call, so "set, draw, set, draw" is correct there by
	// construction. It is the same defect DrawUP had with its scratch vertex
	// buffer, and the same shape VulkanEffectFile already solved with a
	// per-frame arena and dynamic offsets.
	//
	// So: the Set*Constants calls accumulate into a CPU-side block per stage,
	// and BindResources copies that block into a fresh slice of a per-frame
	// arena, binding the slice with a DYNAMIC offset. ResetFrame rewinds it.
	std::vector<char>	vsBlock, psBlock;	///< CPU-side, accumulated per draw
	VulkanBuffer *		pArena;				///< one per frame, sliced per draw
	VkDeviceSize		arenaSize, arenaUsed;
	uint32_t			vsSlice, psSlice;	///< this draw's dynamic offsets
	/// Whether the SET LAYOUT declares each block. Fixed at CreateResources
	/// time and NOT per draw: vkCmdBindDescriptorSets wants exactly one
	/// dynamic offset per dynamic descriptor the layout declares, whether or
	/// not this particular draw wrote anything into it.
	bool				bVSBlock, bPSBlock;
	/// Arenas a mid-frame grow replaced. Freed at the point the GPU is
	/// certainly done with them, never at the point of growth -- and never at
	/// the NEXT ResetFrame either, which is what this used to do and what the
	/// validation layer reported as VUID-vkDestroyBuffer-buffer-00922: the
	/// frame that recorded the binding is still in flight one frame later.
	/// Each carries the frame number that retired it.
	std::deque<std::pair<VulkanBuffer *, unsigned long long> > retiredArena;
	bool	EnsureArena(VkDeviceSize bytes);

	// The scratch vertex/index buffers DrawUP copies into. Same pair, same
	// reason, as VulkanEffectFile's; grown on demand and never shrunk.
	VulkanBuffer *pScratchVB, *pScratchIB;
	VkDeviceSize  scratchVBSize, scratchIBSize;
	bool	EnsureScratch(VkDeviceSize vbytes, VkDeviceSize ibytes);

	// ------------------------------------------------------------------
	// THE ROTATION. Counterpart of nothing in D3D9, which owned the constant
	// registers and the sampler slots and recycled them itself.
	//
	// ResetFrame used to call vkResetDescriptorPool on vkPool at the top of
	// every frame. That is only correct with ONE frame in flight; the core
	// keeps orbiter_GetFramesInFlight() of them, and its vkWaitForFences
	// covers only the slot it is about to record into. The layer said so --
	//
	//     VUID-vkResetDescriptorPool-descriptorPool-00313
	//     ... currently in use by VkCommandBuffer ...
	//
	// -- and the driver answered with intermittent VK_ERROR_DEVICE_LOST.
	//
	// So the pool, the arena and the two scratch buffers are ROTATED: what a
	// frame used is retired with that frame's number and taken back only once
	// the lag has passed. Every use site keeps naming vkPool / pArena /
	// pScratchVB; only what they point at changes, once per frame.
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
	/// is an OBJECT in Vulkan and a device state word in D3D9, so this list
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
	// THE DESCRIPTOR MACHINERY. None of it has a D3D9 counterpart, because
	// D3D9 had no descriptors: a constant went into a numbered register file
	// and a texture onto a numbered sampler slot, both of them device state
	// the runtime kept. Here both reach a shader only through a descriptor
	// set, which has to be described (a layout), allocated (from a pool) and
	// written before the draw that names it.
	//
	// WITHOUT THIS THE CLASS BOUND NOTHING. pSetLayout was declared and never
	// created, so GetPipeline built every pipeline with setLayoutCount 0 --
	// a layout that cannot see a uniform block or a texture at all. Every
	// ShaderClass draw in the client (both tile engines, both haze rings, the
	// celestial sphere, the glares) would have run with no inputs.
	//
	// The binding convention matches IProcess's, which is the other place two
	// stages share one set: the VERTEX stage's uniform block at binding 0,
	// the PIXEL stage's at binding 1, and the samplers at whatever bindings
	// the GLSL declares -- read back from reflection rather than assumed.
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
	/// \brief The same, for a samplerCube binding.
	///
	/// A cube sampler must be given a CUBE view; the 2D pWhite above faults
	/// the GPU. Built only when a shader in this class declares one. See
	/// Var::bCube and CreateResources.
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
// Several of these convert between two types that are now the SAME type --
// _FV took a D3DXVECTOR3 and returned an FVECTOR3, C2V took a D3DXCOLOR and
// returned a D3DXVECTOR4. They are kept as pass-throughs rather than deleted
// because they are spelled as functions at their call sites, and a
// pass-through is a smaller change than rewriting every one of them.
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
///        THIS EXISTS BECAUSE OF A TRAP, not because the arithmetic is hard.
///        FVECTOR4 already has a DWORD constructor and it reads the DWORD as
///        **0xAABBGGRR -- ABGR, not ARGB** (DrawAPI.h:424). A D3DCOLOR is
///        ARGB. So every `D3DXCOLOR(c)` in the Windows source that is
///        "converted" to `FVECTOR4(c)` compiles, looks right, and exchanges
///        red and blue -- a wrong ambient colour, which is exactly the kind
///        of thing that is noticed as "the planet looks a bit off" three
///        months later. Call sites that mean ARGB call this.
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
// The two D3DX matrix functions the client used and that have to be written
// out, because D3DX was a Direct3D UTILITY library and Vulkan ships no
// counterpart at all -- there is no vkMatrixMultiply and there never will be.
//
// They live here, with the other VMAT_ functions, rather than in whichever
// file needed one first: D3DXMatrixMultiply has call sites across the pad,
// the scene and the vessel visual, and writing a 4x4 multiply once per file
// is how two of them end up disagreeing about row or column order.
//
// ROW-VECTOR CONVENTION, which is D3DX's and is what the whole client and
// every one of its shaders assumes: a point is a row and is multiplied on the
// LEFT, v' = v * M, so out = a * b applies a FIRST and then b.
// Orbitersdk/include/DrawAPI.h's own mul(FVECTOR4, FMATRIX4) at line 708
// already does it that way, which is what makes FMATRIX4 a drop-in for
// D3DXMATRIX rather than a transpose of one.
// ------------------------------------------------------------------------------------

/// \brief out = a * b. Counterpart of D3DXMatrixMultiply.
///        Safe to alias: out may be either operand.
void VMAT_MatrixMultiply (FMATRIX4 *out, const FMATRIX4 *a, const FMATRIX4 *b);

/// \brief Counterpart of D3DXMatrixTransformation2D.
///
///        Builds, in D3DX's own order,
///            M = T(-Csc) * Rsc^-1 * S * Rsc * T(Csc) * T(-Crot) * R * T(Crot) * T(t)
///        where Csc is the scaling centre, Rsc the scaling rotation, S the
///        scale, Crot the rotation centre, R the rotation and t the
///        translation. Every pointer may be NULL, meaning the identity for
///        that term -- again as D3DX defines it.
void VMAT_Transformation2D (FMATRIX4 *out,
							const FVECTOR2 *pScalingCenter, float scalingRotation,
							const FVECTOR2 *pScaling,
							const FVECTOR2 *pRotationCenter, float rotation,
							const FVECTOR2 *pTranslation);

// Returns S_OK / E_INVALIDARG. These are the Win32 HRESULT values the Linux
// shim still supplies, and they are this function's OWN return value rather
// than a device's, so they are not VkResult and HR() does not apply.
HRESULT VMAT_MatrixInvert (FMATRIX4 *res, FMATRIX4 *a);

/// \brief Counterpart of D3DXMatrixOrthoOffCenterLH, written out for the same
///        reason the two above are: D3DX supplied it and Vulkan ships nothing.
///
///        THE FORMULA IS D3DX'S, UNCHANGED, and the depth half of it happens
///        to be exactly right: a left-handed D3D ortho maps z into [0,1],
///        which is Vulkan's depth range too (OpenGL's [-1,1] is what would
///        have needed correcting).
///
///        THE Y HALF IS NOT, AND IS LEFT ALONE DELIBERATELY. Vulkan's NDC has
///        +Y pointing DOWN where D3D's points up, so a matrix built by this
///        draws vertically mirrored against what the Windows client showed,
///        unless the caller's own top/bottom arguments already account for it
///        -- which Scene::ComputeLocalLightsVisibility's (0, w, h, 0) do.
///        Flipping it here would silently disagree with every other matrix in
///        the client, all of which come from Orbiter's own camera code; the
///        handedness question belongs in one place and this is not it.
void VMAT_OrthoOffCenterLH (FMATRIX4 *out, float l, float r, float b, float t,
							float zn, float zf);

/// \brief Counterpart of D3DXMatrixOrthoOffCenterRH. Identical to the LH form
///        except for the sign of m33 -- which is the whole of what "handed"
///        means for an orthographic projection. Scene::RenderShadowMap builds
///        its light projection with it.
void VMAT_OrthoOffCenterRH (FMATRIX4 *out, float l, float r, float b, float t,
							float zn, float zf);

/// \brief Counterpart of D3DXMatrixLookAtRH, transcribed from the D3DX
///        documentation's own construction rather than rederived:
///            zaxis = normal(Eye - At)
///            xaxis = normal(cross(Up, zaxis))
///            yaxis = cross(zaxis, xaxis)
///        and the translation row is the three negated dot products.
///        Scene::RenderShadowMap builds its light view with it.
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
// AutoHandle DID NOT CONVERT, and this is the one thing in the file with no
// Linux counterpart at all. Its whole body is Win32 kernel-handle
// management: a HANDLE, INVALID_HANDLE_VALUE and CloseHandle. Linux has no
// such object, the shim defines no INVALID_HANDLE_VALUE, and the client's
// only use of raw kernel handles was the CreateFile/ReadFile shader cache in
// D3D9Util.cpp, which is ordinary stdio here.
//
// It is dropped rather than emulated because it has ZERO call sites: the
// whole D3D9Client tree names AutoHandle only in D3D9Util.h itself, where it
// is defined. AutoFile below is the one that is actually used --
// MaterialMgr.cpp three times and WindowMgr.cpp once -- and it is plain C
// stdio, so it crosses unchanged.

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
// SAFE_RELEASE is gone with the COM reference counting it drove: Vulkan
// objects are not reference counted and are destroyed with an explicit
// vkDestroy* against the device that made them, so a release macro that took
// only the object could not do the job. The client's own resource wrappers
// are deleted, which is what SAFE_DELETE already does.

#define DELETE_SURFACE(p) { if (p) { delete ((SurfNative*)p); p = NULL; } }
#define SAFE_DELETE(p)  { if(p) { delete (p);     (p)=NULL; } }
#define SAFE_DELETEA(p)  { if(p) { delete []p;     (p)=NULL; } }
#define CLEARARRAY(p) { memset(p, 0, sizeof(p)); }

#endif // !__VULKANUTIL_H
