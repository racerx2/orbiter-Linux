// ===========================================================================
// NewMesh.glsl -- converted from OVP/D3D9Client/shaders/NewMesh.hlsl
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2022-2026 Jarmo Nikkanen
// ===========================================================================
//
// The seven general conventions are in IPI.glsl. This is a ShaderClass file,
// so it follows CelSphere.glsl's binding convention: the VERTEX stage's
// uniform block at binding 0, the PIXEL stage's at binding 1, samplers after
// them. Mesh.cpp builds three MeshShader instances from it -- (ShdMapVS,
// ShdMapPS), (ShdMapOIT_VS, ShdMapOIT_PS) and (NormalDepth_VS,
// NormalDepth_PS) -- and each is compiled on its own, so each gets its own
// pair of blocks.
//
// EVERY BLOCK MEMBER IS DECLARED IN EVERY INSTANCE, including the ones a
// given entry point never reads. It has to be: MeshShader's C++ structs are
// written whole at the offset reflection reports, so dropping a member an
// entry point ignores would move every member after it. glslang reports a
// declared block member whether the entry point reads it or not -- checked on
// CelSphere.glsl, whose vertex stage reports Const.fAlpha and Const.fBeta
// although only the pixel stage reads them.
//
// `#define BOOL bool` in the reference exists so that the HLSL declaration
// and the C++ one can be written the same way, C++ BOOL being 32 bits. GLSL
// needs no such define: a bool inside a uniform block is lowered by glslang
// to a uint compared against zero, so it is four bytes there too. See
// CelSphere.glsl's header, where the same question is settled with the
// SPIR-V.
// ===========================================================================

#version 450
#extension GL_EXT_scalar_block_layout : require


// Constant Buffers --------------------------------------
//
// The mirrors of MeshShader::VSConst, ::PSConst and ::PSBools in Mesh.h,
// whose sizes and offsets that header asserts.

struct VSConst {
	mat4 mVP;	// View Projection Matrix
	mat4 mW;	// World Matrix
};

struct PSConst {
	vec3 Cam_X;
	vec3 Cam_Y;
	vec3 Cam_Z;
};

struct PSBools {
	bool bOIT;		// Enable order independent transparency
};


#ifdef _VERTEX_SHADER
layout(set = 0, binding = 0, scalar) uniform NewMeshVSBlock
{
	VSConst vs_const;
};
#endif

#ifdef _FRAGMENT_SHADER
layout(set = 0, binding = 1, scalar) uniform NewMeshPSBlock
{
	PSConst ps_const;
	PSBools ps_bools;
};

layout(set = 0, binding = 2) uniform sampler2D tDiff;

layout(location = 0) out vec4 oColor;
#endif


// Vertex data input layouts -----------------------------
//
// struct MESH_VERTEX   (pMeshVertexDecl)  posL 0, nrmL 1, tanL 2, tex0 3
// struct POSTEX        (pPosTexDecl)      posL 0, tex0 1
// struct SHADOW_VERTEX (pVector4Decl)     posL 0  -- a float4
//
// They are declared inside each entry point's own guard below rather than
// once here, because three different layouts want location 0 and two of them
// want different types there.


// Internal data feeds between VS and PS ------------------
//
// struct PBRData AND struct BasicData ARE NOT CARRIED. NewMesh.hlsl declares
// both -- with a `#if SHDMAP > 0` member each -- and no entry point in the
// file returns either one. An HLSL struct nothing returns costs nothing; a
// GLSL interpolant set that nothing writes costs five locations in every
// pipeline built from this file, because glslang lists every declared output
// in OpEntryPoint's interface. So they are left out, with this note in their
// place. (The same two structs, live, are in VulkanClient.glsl.)


// ----------------------------------------------------------------------------
// struct BShadowVS -- ShdMapVS -> ShdMapPS
//
//     float4 posH : POSITION0;  -> gl_Position
//     float2 dstW : TEXCOORD0;  -> location 0
// ----------------------------------------------------------------------------
#if defined(_EP_ShdMapVS) || defined(_EP_ShdMapPS)
	#define _IO_BShadowVS 1
#endif

#if defined(_IO_BShadowVS) && defined(_VERTEX_SHADER)
layout(location = 0) out vec2 vs_bs_dstW;
#endif

#if defined(_IO_BShadowVS) && defined(_FRAGMENT_SHADER)
layout(location = 0) in vec2 ps_bs_dstW;
#endif


// -----------------------------------------------------------------------------------
// Shadow Map rendering with plain geometry (without texture)
//
#if defined(_EP_ShdMapVS)

// struct SHADOW_VERTEX (pVector4Decl)
layout(location = 0) in vec4 posL;		// POSITION0

void ShdMapVS()
{
	vec3 posW = (vs_const.mW * vec4(posL.xyz, 1.0f)).xyz;
	gl_Position = vs_const.mVP * vec4(posW, 1.0f);
	vs_bs_dstW = gl_Position.zw;
}

#endif // _EP_ShdMapVS


#if defined(_EP_ShdMapPS)
void ShdMapPS()
{
	// `return 1 - (frg.dstW.x / frg.dstW.y);` -- a scalar returned from a
	// float4 function, which HLSL broadcasts to all four channels.
	oColor = vec4(1.0f - (ps_bs_dstW.x / ps_bs_dstW.y));
}
#endif




// ----------------------------------------------------------------------------
// struct ShadowTexVS -- ShdMapOIT_VS -> ShdMapOIT_PS
//
//     float4 posH : POSITION0;  -> gl_Position
//     float4 tex0 : TEXCOORD0;  -> location 0   (distance in .zw)
// ----------------------------------------------------------------------------
#if defined(_EP_ShdMapOIT_VS) || defined(_EP_ShdMapOIT_PS)
	#define _IO_ShadowTexVS 1
#endif

#if defined(_IO_ShadowTexVS) && defined(_VERTEX_SHADER)
layout(location = 0) out vec4 vs_st_tex0;
#endif

#if defined(_IO_ShadowTexVS) && defined(_FRAGMENT_SHADER)
layout(location = 0) in vec4 ps_st_tex0;
#endif


// -----------------------------------------------------------------------------------
// Shadow Map rendering with texture alpha included
//
#if defined(_EP_ShdMapOIT_VS)

// struct POSTEX (pPosTexDecl)
layout(location = 0) in vec3 posL;		// POSITION0
layout(location = 1) in vec2 tex0;		// TEXCOORD0

void ShdMapOIT_VS()
{
	vec3 posW = (vs_const.mW * vec4(posL.xyz, 1.0f)).xyz;
	gl_Position = vs_const.mVP * vec4(posW, 1.0f);
	vs_st_tex0 = vec4(tex0.xy, gl_Position.zw);
}

#endif // _EP_ShdMapOIT_VS


#if defined(_EP_ShdMapOIT_PS)
void ShdMapOIT_PS()
{
	if (ps_bools.bOIT) {
		float alpha = texture(tDiff, ps_st_tex0.xy).a;
		if (alpha < 0.75f) { oColor = vec4(1.0f); return; }
	}
	oColor = vec4(1.0f - (ps_st_tex0.z / ps_st_tex0.w));
}
#endif




// ----------------------------------------------------------------------------
// struct NormalTexVS -- NormalDepth_VS -> NormalDepth_PS
//
//     float4 posH : POSITION0;  -> gl_Position
//     float3 posW : TEXCOORD0;  -> location 0
//     float3 nrmW : TEXCOORD1;  -> location 1
//     float2 tex0 : TEXCOORD2;  -> location 2
// ----------------------------------------------------------------------------
#if defined(_EP_NormalDepth_VS) || defined(_EP_NormalDepth_PS)
	#define _IO_NormalTexVS 1
#endif

#if defined(_IO_NormalTexVS) && defined(_VERTEX_SHADER)
layout(location = 0) out vec3 vs_nt_posW;
layout(location = 1) out vec3 vs_nt_nrmW;
layout(location = 2) out vec2 vs_nt_tex0;
#endif

#if defined(_IO_NormalTexVS) && defined(_FRAGMENT_SHADER)
layout(location = 0) in vec3 ps_nt_posW;
layout(location = 1) in vec3 ps_nt_nrmW;
layout(location = 2) in vec2 ps_nt_tex0;
#endif


// -----------------------------------------------------------------------------------
// Render Normal and depth buffer
//
#if defined(_EP_NormalDepth_VS)

// struct MESH_VERTEX (pMeshVertexDecl). tanL is described by the vertex
// layout and read by neither stage of this pass.
layout(location = 0) in vec3 posL;		// POSITION0
layout(location = 1) in vec3 nrmL;		// NORMAL0
layout(location = 3) in vec3 tex0;		// TEXCOORD0	(handiness in .z)

void NormalDepth_VS()
{
	vs_nt_posW = (vs_const.mW * vec4(posL.xyz, 1.0f)).xyz;
	vs_nt_nrmW = (vs_const.mW * vec4(nrmL, 0.0f)).xyz;
	gl_Position = vs_const.mVP * vec4(vs_nt_posW, 1.0f);
	vs_nt_tex0 = tex0.xy;
}

#endif // _EP_NormalDepth_VS


#if defined(_EP_NormalDepth_PS)
void NormalDepth_PS()
{
	if (ps_bools.bOIT) {
		if (texture(tDiff, ps_nt_tex0.xy).a < 0.75f) discard;
	}
	//if (dot(frg.nrmW, ps_const.Cam_Z) > 0) clip(-1);

	float D = length(ps_nt_posW);
	float x = dot(ps_nt_nrmW, ps_const.Cam_X);
	float y = dot(ps_nt_nrmW, ps_const.Cam_Y);
	float z = sqrt(clamp(1.0 - (x * x + y * y), 0.0f, 1.0f));
	oColor = vec4(x, y, z, D);
}
#endif
