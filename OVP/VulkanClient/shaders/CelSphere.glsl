// ===========================================================================
// CelSphere.glsl -- converted from OVP/D3D9Client/shaders/CelSphere.hlsl
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Licensed under LGPL v2
// Copyright (C) 2021-2026 Jarmo Nikkanen
// ===========================================================================
//
// The seven conventions this file follows are written out once, in IPI.glsl.
// Only what is particular to THIS shader is noted here.
//
// THE FIRST ShaderClass SHADER. IPI, GDIOverlay, EnvMapBlur and
// IrradianceInteg are all ImageProcessing shaders; this one is compiled by
// ShaderClass (CSphereMgr.cpp's constructor, entry points CelVS and CelPS),
// which shares IProcess's binding convention: the VERTEX stage's uniform
// block at binding 0, the PIXEL stage's at binding 1, samplers after them.
//
// TWO BLOCKS, ONE PER STAGE, AND BOTH BEGIN WITH THE SAME `Const`. That is
// not duplication introduced by the conversion -- it is what D3D9 already
// had. A vertex shader and a pixel shader each carried their OWN constant
// table, so `uniform extern CelDataStruct Const` existed twice, once per
// stage, and CSphereMgr writes it twice to match:
//
//     pShader->SetPSConstants("Const", &CelData, sizeof(CelData));   // Render
//     pShader->SetVSConstants(hVSConst, &CelData, sizeof(CelData));  // RenderTile
//
// Here the two constant register files are two uniform buffers, one per
// stage, so the same declaration appears in both blocks at the same offsets
// and those two calls keep writing exactly what they wrote.
//
// The vertex stage reads only mWorld and mViewProj and the pixel stage only
// fAlpha and fBeta, so each block holds members its own stage never touches.
// They are declared anyway, and must be: the offsets are what the C++ writes
// against, and dropping a member would move every one after it. glslang does
// not report a member the entry point does not read, but it does not move the
// ones it does -- checked.
//
// `BOOL` IN CelDataFlow BECOMES `bool`, AND THE 32 BITS SURVIVE. The
// reference's own comment says bool is 32-bit in HLSL and that the C++ side
// must therefore use BOOL. GLSL forbids nothing here either: glslang lowers a
// bool inside a uniform block to a `uint` compared against zero
// (OpINotEqual), so the block member is four bytes and `if (Flow.bAlpha)`
// means the same thing it did. Verified on the SPIR-V, and the 136/140
// offsets of the two flags match CSphereMgr.h's static_asserts.
// ===========================================================================

#version 450
#extension GL_EXT_scalar_block_layout : require

// struct CelDataStruct { float4x4 mWorld; float4x4 mViewProj; float fAlpha; float fBeta; };
//
// The mirror of CSphereMgr::CelDataStruct, whose size and two interior
// offsets that header asserts: 136 bytes, mViewProj at 64, fAlpha at 128.
struct CelDataStruct
{
	mat4	mWorld;
	mat4	mViewProj;
	float	fAlpha;
	float	fBeta;
};

// Booleans go to sepatate structure. (not mixing different datatypes in a structure)
// Also bool is 32-bits in HLSL therefore must use BOOL in C++ structure
struct CelDataFlow
{
	bool	bAlpha;
	bool	bBeta;
};


#ifdef _VERTEX_SHADER

// uniform extern CelDataStruct Const;  -- the vertex stage's copy.
layout(set = 0, binding = 0, scalar) uniform CelSphereVSBlock
{
	CelDataStruct Const;
};

// struct TILEVERTEX  (VERTEX_2TEX)
//
// Locations are the element indices in PatchVertexDecl, which CSphereMgr's
// Setup() names: {POSITION0 vec3 @0, NORMAL0 vec3 @12, TEXCOORD0 vec2 @24,
// TEXCOORD1 float @32}. normalL and elev are declared by the C++ vertex
// declaration and read by neither stage of this shader, exactly as in the
// reference; a pipeline may describe an attribute its shader ignores, so
// they are simply absent here.
layout(location = 0) in vec3 posL;		// POSITION0
layout(location = 2) in vec2 tex0;		// TEXCOORD0

// struct CelSphereVS { float4 posH : POSITION0; float2 tex0 : TEXCOORD0; }
layout(location = 0) out vec2 vsTex0;

void CelVS()
{
	// The `CelSphereVS outVS = (CelSphereVS)0;` zero-initialise has no
	// counterpart and needs none: both members are assigned below.

	// mul(float4(vrt.posL, 1.0f), Const.mWorld).xyz -- see IPI.glsl note 2.
	vec3 posW = (Const.mWorld * vec4(posL, 1.0f)).xyz;
	gl_Position = Const.mViewProj * vec4(posW, 1.0f);
	vsTex0 = tex0;
}

#endif // _VERTEX_SHADER


#ifdef _FRAGMENT_SHADER

// uniform extern CelDataStruct Const;  -- the pixel stage's copy.
// uniform extern CelDataFlow Flow;
//
// One block, because the client stages one uniform buffer per stage; `Const`
// therefore keeps offset 0 and `Flow` follows it at 136, which is where
// SetPSConstants("Flow", &CelFlow, sizeof(CelFlow)) writes.
layout(set = 0, binding = 1, scalar) uniform CelSpherePSBlock
{
	CelDataStruct Const;
	CelDataFlow   Flow;
};

layout(set = 0, binding = 2) uniform sampler2D tTexA;
layout(set = 0, binding = 3) uniform sampler2D tTexB;

layout(location = 0) in vec2 psTex0;

layout(location = 0) out vec4 oColor;

void CelPS()
{
	vec3 vColor = vec3(0.0f);
	if (Flow.bAlpha) vColor += texture(tTexA, psTex0).rgb * Const.fAlpha;
	if (Flow.bBeta)  vColor += texture(tTexB, psTex0).rgb * Const.fBeta;
	oColor = vec4(vColor, 1.0);
}

#endif // _FRAGMENT_SHADER
