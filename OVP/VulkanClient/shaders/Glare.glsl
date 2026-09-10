// ===================================================
// Glare.glsl -- converted from OVP/D3D9Client/shaders/Glare.hlsl
// Copyright (C) 2022-2026 Jarmo Nikkanen
// licensed under MIT
// ===================================================
//
// The seven general conventions are in IPI.glsl. What is particular here:
//
//  A. ONE FILE, THREE CONSUMERS. Scene.cpp builds three objects from it:
//       ShaderClass     (VisibilityVS, VisibilityPS)  -- pLocalCompute
//       ShaderClass     (GlareVS, GlarePS)            -- pRenderGlares
//       ImageProcessing (CreateSunGlarePS, plus CreateSunGlareAtmPS,
//                        CreateLocalGlarePS and CreateSunTexPS via
//                        CompileShader/Activate)      -- pCreateGlare
//     Both classes share one binding convention -- the VERTEX stage's uniform
//     block at binding 0, the PIXEL stage's at binding 1, samplers after them
//     -- and each object builds its own descriptor set layout from its own
//     reflection, so a binding only has to be unique among the entry points
//     that are compiled together.
//
//  B. TWO DIFFERENT BLOCKS WANT BINDING 0, AND TWO WANT BINDING 1. The
//     Visibility pair reads cbPS and cbKernel; the Glare pair reads Const.
//     They are separate compilations, so each may have the binding to itself
//     -- but a GLSL file may not declare two blocks at one binding, even if
//     no entry point reads both. So each is guarded by the entry points that
//     use it, exactly as the interpolant sets are. See VulkanClient.glsl's
//     note E on `_EP_<entry>`.
//
//  C. tVis IS A VERTEX-STAGE SAMPLER. Scene.cpp sets it with SetTextureVS and
//     GlareVS samples it. That is not new: D3D9 had vertex texture fetch on
//     D3DVERTEXTEXTURESAMPLER0..3, which is why ShaderClass has SetTextureVS
//     at all. Vulkan lets the vertex stage sample like any other, so the
//     declaration says nothing special -- only the binding has to stay clear
//     of the pixel stage's, because they share one descriptor set.
//
//  D. A VERTEX INPUT IS READ-ONLY IN GLSL, where HLSL's was a by-value
//     parameter the shader could assign to. GlareVS's `posL.xy *= ...` and
//     the four Create*PS's `u = u * 2.0 - 1.0;` therefore copy into a local
//     first. The arithmetic is unchanged.
//
//  E. `uniform extern struct { ... } cbPS;` DECLARES AN ANONYMOUS TYPE, which
//     GLSL has no spelling for. The two structs are named CBPS and GlareConst
//     here; the INSTANCE names -- cbPS and Const -- are what Scene.cpp looks
//     up by name, and they are unchanged.
// ===================================================

#version 450
#extension GL_EXT_scalar_block_layout : require


// ====================================================================
// GPU based computation of local lights visibility (including the Sun)
// ====================================================================

float ilerp(float a, float b, float x)
{
	return clamp((x - a) / (b - a), 0.0f, 1.0f);
}

vec3 HDRtoLDR(vec3 hdr)
{
	vec3 h2 = hdr * hdr;
	return hdr * pow(max(vec3(0.0f), 1.0f + h2 * h2), vec3(-0.25));
}

#define LocalKernelSize 57
// `static const float iLKS` -- `static` has no counterpart in GLSL, where a
// file-scope const already has internal linkage.
const float iLKS = 1.0f / float(LocalKernelSize);

// The mirror of Scene::ComputeLocalLightsVisibility's ComputeData:
//     FMATRIX4 mVP; FMATRIX4 mSVP; FVECTOR4 vSrc; FVECTOR3 vDir;
// at offsets 0, 64, 128 and 144. Its C++ sizeof is 160 rather than 156,
// because FMATRIX4 and FVECTOR4 are alignas(16) and the struct's own
// alignment rises with them; the trailing pad is written and never read, and
// no field moves.
struct CBPS {
	mat4 mVP;
	mat4 mSVP;
	vec4 vSrc;
	vec3 vDir;
};

// The mirror of Scene::RenderGlares' Const:
//     FMATRIX4 mVP; float4 Pos, Color; float GPUId, Alpha, Blend;
// at offsets 0, 64, 80, 96, 100 and 104. Same trailing-pad note as above:
// the C++ sizeof is 112 where the block is 108.
struct GlareConst {
	mat4	mVP;
	vec4	Pos;
	vec4	Color;
	float	GPUId;
	float	Alpha;
	float	Blend;
};


#if defined(_EP_VisibilityVS)
layout(set = 0, binding = 0, scalar) uniform VisibilityVSBlock
{
	CBPS cbPS;
};
#endif

#if defined(_EP_VisibilityPS)
// cbKernel FOLLOWS cbPS AT 156, AND THE ORDER OF THE TWO WRITES MATTERS.
// Scene::ComputeLocalLightsVisibility passes sizeof(ComputeData), which is
// 160 rather than the 156 the struct's fields occupy, so the write of cbPS
// spills four bytes into cbKernel[0].x -- and the very next line writes the
// whole of cbKernel over it:
//
//     SetPSConstants("cbPS", &ComputeData, sizeof(ComputeData));
//     SetPSConstants("cbKernel", DepthSampleKernel, sizeof(DepthSampleKernel));
//
// That was equally true on Windows -- D3DX's SetValue copied the same 160
// bytes into the same place -- so the behaviour is carried, not introduced.
// The declaration order here is the reference's; reversing it would break the
// pair.
layout(set = 0, binding = 1, scalar) uniform VisibilityPSBlock
{
	CBPS cbPS;
	vec2 cbKernel[LocalKernelSize];
};

layout(set = 0, binding = 2) uniform sampler2D tDepth;
#endif


#ifdef _FRAGMENT_SHADER
layout(location = 0) out vec4 oColor;
#endif


// ----------------------------------------------------------------------------
// struct LData -- VisibilityVS -> VisibilityPS
//
//     float4 posH : POSITION0;  -> gl_Position   For rendering
//     float4 smpH : TEXCOORD0;  -> location 0    For sampling screen depth bufer
//     float3 posW : TEXCOORD1;  -> location 1    Camera centric location ECL
//     float  cone : TEXCOORD3;  -> location 2    Attennuation by light-cone
//
// cone sits at TEXCOORD3 in the reference, skipping TEXCOORD2. A GLSL
// location is not a semantic index and nothing outside this file reads it, so
// the pair uses 0..2 and the hole costs nothing. VisibilityPS never reads
// cone; it is carried because the struct declares it and the vertex shader
// writes it.
// ----------------------------------------------------------------------------
#if defined(_EP_VisibilityVS) || defined(_EP_VisibilityPS)
	#define _IO_LData 1
#endif

#if defined(_IO_LData) && defined(_VERTEX_SHADER)
layout(location = 0) out vec4 vs_ld_smpH;
layout(location = 1) out vec3 vs_ld_posW;
layout(location = 2) out float vs_ld_cone;
#endif

#if defined(_IO_LData) && defined(_FRAGMENT_SHADER)
layout(location = 0) in vec4 ps_ld_smpH;
layout(location = 1) in vec3 ps_ld_posW;
layout(location = 2) in float ps_ld_cone;
#endif


#if defined(_EP_VisibilityVS)

// `float posL : POSITION0, float4 posW : TEXCOORD0` (pLocalLightsDecl):
// the primitive index at offset 0 and position.xyz + cone.w at offset 4.
layout(location = 0) in float posL;		// POSITION0
layout(location = 1) in vec4 posW;		// TEXCOORD0

void VisibilityVS()
{
	gl_Position  = cbPS.mVP * vec4(posL + 0.5f, 0.0f, 0.0f, 1.0f);	// Render projection
	vs_ld_smpH   = cbPS.mSVP * vec4(posW.xyz, 1.0f);				// Depth sampling projection
	vs_ld_posW   = posW.xyz;
	vs_ld_cone   = posW.a;

	// Drawn as a POINT LIST (Scene::ComputeLocalLightsVisibility, which was
	// DrawPrimitiveUP(D3DPT_POINTLIST, ...)). The reference's LData declares
	// no PSIZE, so D3D9 used D3DRS_POINTSIZE's default of 1.0 -- one pixel
	// per light, which is exactly what this pass wants: it reads back one
	// texel per light. Vulkan requires the shader to say so; see the same
	// note in SceneTech.glsl's StarTechVS.
	gl_PointSize = 1.0f;
}

#endif // _EP_VisibilityVS


// Check sun/light "glare" visibility
//
#if defined(_EP_VisibilityPS)
void VisibilityPS()
{
	vec4 smpH = ps_ld_smpH;
	smpH.xyz /= smpH.w;
	vec2 sp = smpH.xy * vec2(0.5f, -0.5f) + vec2(0.5f, 0.5f); // Scale and offset to 0-1 range

	if (sp.x < 0.0f || sp.y < 0.0f) { oColor = vec4(0.0f); return; }		// If a sample is outside border -> obscured
	if (sp.x > 1.0f || sp.y > 1.0f) { oColor = vec4(0.0f); return; }

	vec2 vScale = 40.0f * cbPS.vSrc.zz;					// Kernel scale factor (from unit kernel)
	float fDepth = dot(ps_ld_posW, cbPS.vDir) - 0.25f;	// Depth to compare
	float fRet = 0.0f;

	for (int i = 0; i < LocalKernelSize; i++) {
		vec2 s = sp + cbKernel[i].xy * vScale * 0.4f;
		if (s.x < 0.0f || s.x > 1.0f) { fRet += iLKS;	continue; }
		if (s.y < 0.0f || s.y > 1.0f) { fRet += iLKS;	continue; }
		float d = texture(tDepth, s).a;
		fRet += (d > 0.1f && d < fDepth) ? iLKS : 0.0f;
	}

	oColor = vec4(1.0f - fRet);
}
#endif // _EP_VisibilityPS



// ====================================================================
// Rendering of glares for the Sun and local lights
// ====================================================================

#if defined(_EP_GlareVS)
layout(set = 0, binding = 0, scalar) uniform GlareVSBlock
{
	GlareConst Const;
};

layout(set = 0, binding = 3) uniform sampler2D tVis;	// Pre-computed visibility factors
#endif

#if defined(_EP_GlarePS)
layout(set = 0, binding = 1, scalar) uniform GlarePSBlock
{
	GlareConst Const;
};

layout(set = 0, binding = 4) uniform sampler2D tTex0;	// Main Glare
layout(set = 0, binding = 5) uniform sampler2D tTex1;	// Atmospheric Glare
#endif


// ----------------------------------------------------------------------------
// struct OutputVS -- GlareVS -> GlarePS
//
//     float4 posH : POSITION0;  -> gl_Position
//     float3 uvi  : TEXCOORD0;  -> location 0
// ----------------------------------------------------------------------------
#if defined(_EP_GlareVS) || defined(_EP_GlarePS)
	#define _IO_OutputVS 1
#endif

#if defined(_IO_OutputVS) && defined(_VERTEX_SHADER)
layout(location = 0) out vec3 vs_gl_uvi;
#endif

#if defined(_IO_OutputVS) && defined(_FRAGMENT_SHADER)
layout(location = 0) in vec3 ps_gl_uvi;
#endif


#if defined(_EP_GlareVS)

// `float3 posL : POSITION0, float2 tex0 : TEXCOORD0` (pPosTexDecl)
layout(location = 0) in vec3 posL;		// POSITION0
layout(location = 1) in vec2 tex0;		// TEXCOORD0

void GlareVS()
{
	// `tex2Dlod(tVis, float4(u, v, 0, lod))` -- HLSL packs the coordinate and
	// the LOD into one float4; GLSL takes them apart.
	float visibility = smoothstep(0.5f, 1.0f, textureLod(tVis, vec2(Const.GPUId, 0.5f), 0.0f).r);

	// A vertex input is read-only here; the reference assigned to its
	// by-value parameter. See note D.
	vec3 p = posL;
	p.xy *= Const.Pos.zw * (0.01f + visibility);
	p.xy += Const.Pos.xy;

	gl_Position = Const.mVP * vec4(p.xy - 0.5f, 0.0f, 1.0f);
	vs_gl_uvi = vec3(tex0.xy, visibility);
}

#endif // _EP_GlareVS


#if defined(_EP_GlarePS)
void GlarePS()
{
	float t0 = max(0.0f, texture(tTex0, ps_gl_uvi.xy).r - 0.1f);  // Texture intensity
	//float t1 = max(0, tex2D(tTex1, frg.uvi.xy).r - 0.1f);  // Texture intensity
	//float t = lerp(t1, t0, Const.Blend);
	float a = clamp(1.0f - exp(-ps_gl_uvi.z * Const.Alpha * t0), 0.0f, 1.0f);
	oColor = vec4(HDRtoLDR(Const.Color.rgb * sqrt(t0 + 1.0f)), a);
}
#endif // _EP_GlarePS








// ====================================================================
// Creation of "glare" textures
// ====================================================================
//
// These four are ImageProcessing pixel shaders: their vertex shader is
// IPI.glsl's VSMain, which writes vsX at location 0 and vsY at location 1.
// `float u : TEXCOORD0, float v : TEXCOORD1` is how each of them takes them.
//
// The declaration is guarded because GlarePS already claims location 0 with a
// vec3, and two variables may not share a location -- see note B.

#if defined(_EP_CreateSunGlarePS)   || defined(_EP_CreateSunGlareAtmPS) || \
	defined(_EP_CreateLocalGlarePS) || defined(_EP_CreateSunTexPS)
layout(location = 0) in float psU;		// TEXCOORD0
layout(location = 1) in float psV;		// TEXCOORD1
#endif


// ======================================================================
// Render sun "Glare" (seen in space)
//
#if defined(_EP_CreateSunGlarePS)
void CreateSunGlarePS()
{
	float u = psU * 2.0 - 1.0;	float v = psV * 2.0 - 1.0;

	float a = atan(u, v);
	float r = sqrt(u * u + v * v);

	float q = 0.5f + 0.3f * pow(sin(3.0f * a), 4.0f);
	float w = 0.5f + 0.2f * pow(sin(30.0f * a), 2.0f) * pow(sin(41.0f * a), 2.0f);

	//float I = pow(max(0, 2.0f * (1 - r / q)), 12.0f);
	//float K = pow(max(0, 2.0f * (1 - r / w)), 12.0f);
	//float I = exp(max(0, 10.0f * (1 - r / q))) - 1.0f;
	//float K = exp(max(0, 10.0f * (1 - r / w))) - 1.0f;

	float L = pow(max(0.0f, (1.0f - r / q)), 6.0f) * 3.0f;	// Low frequency spikes
	float H = pow(max(0.0f, (1.0f - r / w)), 6.0f) * 5.0f;	// High frequency spikes
	float C = ilerp(0.03, 0.01, r) * 7.0f;					// Core
	float S = ilerp(1.7f, 0.35f, r);						// Skirt

	C *= C;
	C += S * S * 0.40f;

	oColor = vec4(max(L + C, H + C), 0, 0, 1);
}
#endif



// ======================================================================
// Render sun "Glare" (seen in atmosphere)
//
#if defined(_EP_CreateSunGlareAtmPS)
void CreateSunGlareAtmPS()
{
	float u = psU * 2.0 - 1.0;	float v = psV * 2.0 - 1.0;

	float a = atan(u, v);
	float r = sqrt(u * u + v * v);

	float q = 0.5f + 1.0f * pow(sin(3.0f * a), 4.0f);
	float w = 0.5f + 0.8f * pow(sin(30.0f * a), 2.0f) * pow(sin(41.0f * a), 2.0f);

	float I = pow(max(0.0f, (1.0f - r / q)), 6.0f) * 4.0f;
	float K = pow(max(0.0f, (1.0f - r / w)), 6.0f) * 8.0f;

	float L = ilerp(0.05, 0.01, r) * 16.0f;
	float T = max(0.0f, max(I + L, K + L)) * 2.0f;

	oColor = vec4(T, 0, 0, 1);
}
#endif



// ======================================================================
// Render "Glare" for local light sources
//
#if defined(_EP_CreateLocalGlarePS)
void CreateLocalGlarePS()
{
	float u = psU * 2.0 - 1.0;	float v = psV * 2.0 - 1.0;

	float a = atan(u, v);
	float r = sqrt(u * u + v * v);

	float q = 0.5f + 0.5f * pow(sin(3.0f * a), 4.0f);
	float w = 0.5f + 0.3f * pow(sin(30.0f * a), 2.0f) * pow(sin(41.0f * a), 2.0f);

	float L = pow(max(0.0f, (1.0f - r / q)), 6.0f) * 4.0f;
	float H = pow(max(0.0f, (1.0f - r / w)), 6.0f) * 8.0f;
	float C = ilerp(0.15, 0.10, r) * 4.0f;

	oColor = vec4(max(L + C, H + C), 0, 0, 1);
}
#endif




// ======================================================================
// Render regular Sun texture [ NOT IN USE ]
//
// "NOT IN USE" is the reference's own note and is no longer quite true:
// Scene.cpp compiles CreateSunTexPS and renders pSunTex with it.
//
#if defined(_EP_CreateSunTexPS)
void CreateSunTexPS()
{
	float u = psU * 2.0 - 1.0;	float v = psV * 2.0 - 1.0;

	float a = atan(u, v);
	float r = sqrt(u * u + v * v);

	float q = 0.5f + 1.0f * pow(sin(3.0f * a), 4.0f);
	float w = 0.5f + 0.8f * pow(sin(30.0f * a), 2.0f) * pow(sin(41.0f * a), 2.0f);

	float I = pow(max(0.0f, (1.0f - r / q)), 6.0f) * 1.0f;
	float K = pow(max(0.0f, (1.0f - r / w)), 6.0f) * 3.0f;

	float L = ilerp(0.08, 0.03, r) * 8.0f;
	float T = max(0.0f, max(I + L, K + L));

	T = clamp(1.0f - exp(-T), 0.0f, 1.0f);
	oColor = vec4(1, 1, 1, T);
}
#endif
