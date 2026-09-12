// Linux <d3dtypes.h> — DirectX 7 POD types, no runtime.
//
// Utils/meshc is an offline mesh compiler: it reads .msh files, transforms
// vertices and writes them back out. It includes <d3d.h>/<d3dtypes.h> purely
// for the vertex and vector structs that Orbiter's mesh format is expressed
// in, and never calls a Direct3D function or touches a GPU.
//
// Layouts match the DirectX 7 SDK exactly, because Orbiter's .msh binary
// format is written directly from these structs.

#ifndef ORBITER_LINUX_D3DTYPES_H
#define ORBITER_LINUX_D3DTYPES_H

#ifdef _WIN32
#error "This header is for non-Windows builds only."
#endif

#include <windows.h>

// The D3D*_DEFINED guards below are the SDK's own, and they matter here
// because on Linux this header and a Direct3D 9 header can land in the same
// translation unit, which on Windows they never do. <d3d9.h> defines
// D3DVALUE, D3DVECTOR, D3DMATRIX, D3DCOLORVALUE and D3DCOLOR itself under
// exactly these guards. On Windows the real <ddraw.h> does not include
// <d3dtypes.h> -- the DX7 types arrive only via <d3d.h>, which nothing
// including <d3d9.h> also includes. This tree's <ddraw.h> does include it
// (Texture.h wants DDSURFACEDESC2 and the DirectX 7 types together), so the
// two meet, in either order depending on the .cpp, and whichever is read
// second must yield rather than redefine.

#ifndef D3DVALUE_DEFINED
#define D3DVALUE_DEFINED
typedef float D3DVALUE, *LPD3DVALUE;
#endif

#define D3DVAL(x)    ((float)(x))
#define D3DDivide(a, b) (float)((double)(a) / (double)(b))

#ifndef D3DVECTOR_DEFINED
#define D3DVECTOR_DEFINED
typedef struct _D3DVECTOR {
    union { D3DVALUE x, dvX; };
    union { D3DVALUE y, dvY; };
    union { D3DVALUE z, dvZ; };
} D3DVECTOR, *LPD3DVECTOR;
#endif

// Position, normal, texture coordinate. This is D3DFVF_VERTEX, and it is the
// exact byte layout Orbiter's mesh files store.
typedef struct _D3DVERTEX {
    union { D3DVALUE x, dvX; };
    union { D3DVALUE y, dvY; };
    union { D3DVALUE z, dvZ; };
    union { D3DVALUE nx, dvNX; };
    union { D3DVALUE ny, dvNY; };
    union { D3DVALUE nz, dvNZ; };
    union { D3DVALUE tu, dvTU; };
    union { D3DVALUE tv, dvTV; };
} D3DVERTEX, *LPD3DVERTEX;

#ifndef D3DMATRIX_DEFINED
#define D3DMATRIX_DEFINED
typedef struct _D3DMATRIX {
    D3DVALUE _11, _12, _13, _14;
    D3DVALUE _21, _22, _23, _24;
    D3DVALUE _31, _32, _33, _34;
    D3DVALUE _41, _42, _43, _44;
} D3DMATRIX, *LPD3DMATRIX;
#endif

#ifndef D3DCOLORVALUE_DEFINED
#define D3DCOLORVALUE_DEFINED
typedef struct _D3DCOLORVALUE {
    union { D3DVALUE r, dvR; };
    union { D3DVALUE g, dvG; };
    union { D3DVALUE b, dvB; };
    union { D3DVALUE a, dvA; };
} D3DCOLORVALUE, *LPD3DCOLORVALUE;
#endif

#ifndef D3DCOLOR_DEFINED
#define D3DCOLOR_DEFINED
typedef DWORD D3DCOLOR, *LPD3DCOLOR;
#endif

// meshc does `memcpy(dst, src, n * sizeof(D3DMATERIAL7))` when merging mesh
// materials, so this layout has to match the DirectX 7 SDK field for field.
typedef struct _D3DMATERIAL7 {
    union { D3DCOLORVALUE diffuse,  dcvDiffuse;  };
    union { D3DCOLORVALUE ambient,  dcvAmbient;  };
    union { D3DCOLORVALUE specular, dcvSpecular; };
    union { D3DCOLORVALUE emissive, dcvEmissive; };
    union { D3DVALUE      power,    dvPower;     };
} D3DMATERIAL7, *LPD3DMATERIAL7;

// Flexible vertex format flags
#define D3DFVF_RESERVED0    0x001
#define D3DFVF_XYZ          0x002
#define D3DFVF_NORMAL       0x010
#define D3DFVF_DIFFUSE      0x040
#define D3DFVF_SPECULAR     0x080
#define D3DFVF_TEX1         0x100
#define D3DFVF_VERTEX       (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1)

// Texture coordinate sizes. In the SDK these shift a 2-bit size code into the
// per-stage field of the FVF word; size 2 (u,v) is encoded as 0.
#define D3DFVF_TEXTUREFORMAT1  3
#define D3DFVF_TEXTUREFORMAT2  0
#define D3DFVF_TEXTUREFORMAT3  1
#define D3DFVF_TEXTUREFORMAT4  2
#define D3DFVF_TEXCOORDSIZE1(i) (D3DFVF_TEXTUREFORMAT1 << (i * 2 + 16))
#define D3DFVF_TEXCOORDSIZE2(i) (D3DFVF_TEXTUREFORMAT2)
#define D3DFVF_TEXCOORDSIZE3(i) (D3DFVF_TEXTUREFORMAT3 << (i * 2 + 16))
#define D3DFVF_TEXCOORDSIZE4(i) (D3DFVF_TEXTUREFORMAT4 << (i * 2 + 16))

// Primitive types
#ifndef D3DPRIMITIVETYPE_DEFINED
#define D3DPRIMITIVETYPE_DEFINED
typedef enum _D3DPRIMITIVETYPE {
    D3DPT_POINTLIST     = 1,
    D3DPT_LINELIST      = 2,
    D3DPT_LINESTRIP     = 3,
    D3DPT_TRIANGLELIST  = 4,
    D3DPT_TRIANGLESTRIP = 5,
    D3DPT_TRIANGLEFAN   = 6,
    D3DPT_FORCE_DWORD   = 0x7fffffff
} D3DPRIMITIVETYPE;
#endif

// Render states — only those referenced.
#define D3DRENDERSTATE_ZBIAS    47
#define D3DRENDERSTATE_WRAP0    128
#define D3DRENDERSTATE_AMBIENT  139
#define D3DRENDERSTATE_ALPHABLENDENABLE 27
#define D3DRENDERSTATE_TEXTUREPERSPECTIVE 4
#define D3DRENDERSTATE_ZENABLE  7
#define D3DRENDERSTATE_ZWRITEENABLE 14
#define D3DRENDERSTATE_CULLMODE 22
#define D3DRENDERSTATE_LIGHTING 137

// Packs four 0..1 floats into a D3DCOLOR (0xAARRGGBB).
#define D3DRGBA(r, g, b, a) \
    ((D3DCOLOR)( \
        (((DWORD)((a) * 255.0f + 0.5f)) << 24) | \
        (((DWORD)((r) * 255.0f + 0.5f)) << 16) | \
        (((DWORD)((g) * 255.0f + 0.5f)) <<  8) | \
         ((DWORD)((b) * 255.0f + 0.5f))))

#define D3DRGB(r, g, b) D3DRGBA(r, g, b, 1.0f)

#define D3DWRAP_U               0x00000001
#define D3DWRAP_V               0x00000002

#define D3DSTATUS_DEFAULT       0x00003000

// DirectDraw surfaces appear only as opaque texture handles in this tree, so
// an undefined struct is sufficient and prevents accidental dereference.
struct IDirectDrawSurface7;
typedef struct IDirectDrawSurface7 *LPDIRECTDRAWSURFACE7;

// GroupSpec in Mesh.h carries a vertex-buffer handle per mesh group, and
// Mesh.cpp calls Release() on it when freeing groups. That is the only method
// the core ever touches.
struct IDirect3DVertexBuffer7 {
    virtual ULONG Release () = 0;
protected:
    ~IDirect3DVertexBuffer7() = default;
};
typedef struct IDirect3DVertexBuffer7 *LPDIRECT3DVERTEXBUFFER7;

// The Direct3D 7 root object. Referenced only in declarations.
struct IDirect3D7;
typedef struct IDirect3D7 *LPDIRECT3D7;

// Orbiter.cpp declares ConfirmDevice(DDCAPS*, D3DDEVICEDESC7*) as a
// device-enumeration callback. Nothing on Linux enumerates D3D devices, so the
// contents are never read and an incomplete type compiles the declaration.
struct _D3DDEVICEDESC7;
typedef struct _D3DDEVICEDESC7 D3DDEVICEDESC7, *LPD3DDEVICEDESC7;

// Mesh::Render() in Utils/meshc is a real Direct3D 7 draw path, so the device
// cannot be an incomplete type even though meshc never has a device to invoke
// it with. The methods are pure virtual, which is what COM interfaces really
// are: every call is vtable dispatch, so Mesh.cpp links with no bodies
// anywhere and a call on a bogus device faults instead of silently
// succeeding.
struct IDirect3DDevice7 {
    virtual HRESULT ComputeSphereVisibility (LPD3DVECTOR centres, LPD3DVALUE radii,
                                             DWORD count, DWORD flags, LPDWORD ret) = 0;
    virtual HRESULT SetMaterial             (LPD3DMATERIAL7 mtrl) = 0;
    virtual HRESULT SetTexture              (DWORD stage, LPDIRECTDRAWSURFACE7 tex) = 0;
    virtual HRESULT SetRenderState          (DWORD state, DWORD value) = 0;
    virtual HRESULT GetRenderState          (DWORD state, LPDWORD value) = 0;
    virtual HRESULT DrawPrimitive           (D3DPRIMITIVETYPE type, DWORD fvf,
                                             LPVOID vtx, DWORD nvtx,
                                             DWORD flags) = 0;
    virtual HRESULT DrawIndexedPrimitive    (D3DPRIMITIVETYPE type, DWORD fvf,
                                             LPVOID vtx, DWORD nvtx,
                                             LPWORD idx, DWORD nidx, DWORD flags) = 0;
protected:
    ~IDirect3DDevice7() = default;
};
typedef struct IDirect3DDevice7 *LPDIRECT3DDEVICE7;

#endif // ORBITER_LINUX_D3DTYPES_H
