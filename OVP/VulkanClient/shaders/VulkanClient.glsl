// ===========================================================================
// VulkanClient.glsl -- converted from OVP/D3D9Client/shaders/D3D9Client.fx
//                     and the six files it #includes:
//                         Particle.fx  Mesh.fx  Vessel.fx
//                         HorizonHaze.fx  Planet.fx  BeaconArray.fx
//                     and, through Vessel.fx:
//                         LightBlur.hlsl  Common.hlsl  PBR.fx  Metalness.fx
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2012 - 2026 Jarmo Nikkanen
// ===========================================================================
//
// The seven general conventions this port's shaders follow are written out
// once, in IPI.glsl. What is particular to THIS file:
//
//  A. IT IS ONE FILE BECAUSE THE .fx WAS ONE TRANSLATION UNIT. GLSL has no
//     #include, so the six included files are transcribed inline, in the
//     order D3D9Client.fx includes them, each under its own banner naming the
//     file it came from. Nothing is reordered inside a file.
//
//  B. THE TECHNIQUES ARE NOT HERE. `technique`, `pass` and the render states
//     are D3DX effect-framework syntax rather than HLSL, so they live in
//     VulkanClient.tech -- and so do the twenty-four `sampler_state` blocks,
//     for the same reason. See that file's header.
//
//  C. ONE UNIFORM BLOCK AT BINDING 0, SHARED BY BOTH STAGES. That is what an
//     .fx parameter block already was: `uniform extern float4x4 gVP;` and its
//     ninety siblings are file-scope and shared by every technique, and D3DX
//     kept one set of values across SetTechnique. layout(scalar) because the
//     client memcpy's whole C++ structs into it -- VulkanSun, VulkanMatExt,
//     VulkanTune, LightStruct and TexFlow -- and those are packed to C rules,
//     not to std140's. See VulkanUtil.h's note on the shader-facing structs.
//
//  D. SAMPLERS AT BINDINGS 1..24, ONE PER .fx sampler_state, NAMED AFTER IT.
//     In HLSL a `texture` and a `sampler_state` are two objects and tex2D
//     takes the sampler; in GLSL a sampler2D is the combination and there is
//     no way to spell them apart. So the shader declares the SAMPLERS -- the
//     things its code actually names -- and VulkanClient.tech carries each
//     one's `Texture = <gTex0>` line, which is what lets the client keep
//     setting textures by texture name. The bindings are the .fx's own
//     declaration order.
//
//  E. `_EP_<entry>` GUARDS EACH INTERPOLANT SET. In HLSL a vertex shader's
//     outputs are the members of a struct it returns, so twelve entry points
//     have twelve independent sets. In GLSL they are file-scope `out`
//     variables with explicit locations, two of which may not overlap --
//     glslang rejects "overlapping use of location 0" at parse time even for
//     an entry point that touches neither -- and giving all forty-one their
//     own location does not help, because glslang lists EVERY declared output
//     in OpEntryPoint's interface, so they would all count against
//     maxVertexOutputComponents. So each set is declared only while its own
//     entry points are being compiled, which restores what HLSL gave for
//     free. The define comes from CompileShaderStage's preamble; see the note
//     there.
//
//  F. THE HLSL SEMANTICS THAT BECOME BUILT-INS:
//         POSITION0 (VS out)  -> gl_Position
//         PSIZE     (VS out)  -> gl_PointSize
//         VPOS      (PS in)   -> gl_FragCoord
//         : COLOR   (PS out)  -> layout(location = 0) out vec4 oColor
//     and D3DRS_POINTSPRITEENABLE's generated texture coordinate, which the
//     reference reads as the interpolant `tex0`, is gl_PointCoord. See
//     BeaconArrayPS.
//
//  G. THE HLSL INTRINSICS THAT ARE SPELLED DIFFERENTLY:
//         saturate(x)      clamp(x, 0.0, 1.0)
//         lerp(a,b,t)      mix(a,b,t)
//         rsqrt(x)         inversesqrt(x)
//         rcp(x)           1.0 / x
//         frac(x)          fract(x)
//         clip(-1)         discard
//         tex2D(s,uv)      texture(s, uv)
//         texCUBElod(s,f4) textureLod(s, f4.xyz, f4.w)
//         sincos(a,s,c)    s = sin(a); c = cos(a)
//         any(float3)      any(notEqual(v, vec3(0.0)))
//         [unroll]         nothing -- GLSL has no such hint and glslang
//                          unrolls a constant-trip loop anyway
//     and HLSL's implicit scalar-to-vector promotion (`float4 c = 1;`) has to
//     be written out (`vec4 c = vec4(1.0);`).
// ===========================================================================

#version 450
#extension GL_EXT_scalar_block_layout : require


#define NIGHT_CLOUDS 0.05f          // range(0.0f-0.1f) Cloud ambient level at night
#define CLOUD_INTENSITY 1.8f        // range(0.5f-2.0f)
#define NIGHT_LIGHTS 0.7f           // range(0.2f-1.0f)

struct Mat
{
	vec4 diffuse;
	vec4 ambient;
	vec4 specular;
	vec4 emissive;
	float  specPower;
};

struct Mtrl
{
	vec4 diffuse;
	vec4 specular;
	vec3 ambient;
	vec3 emissive;
	vec3 reflect;
	vec3 emission2;
	vec3 fresnel;
	vec2 roughness;
	float  metalness;
	vec4 specialfx;			// x = Heat
};

struct Sun
{
	vec3 Dir;
	vec3 Color;			// Color and Intensity of received sunlight
	vec3 Ambient;			// Ambient light level (Base Objects Only, Vessels are using dynamic methods)
	vec3 Transmission;	// Visibility through atmosphere (1.0 = fully visible, 0.0 = obscured)
	vec3 Inscatter;		// Amount of incattered light from haze
};

struct Light
{
	int      type;       	   /* Is is spotlight */
	float    dst2;			   /* Camera-Light Emitter distance squared */
	vec4     diffuse;          /* diffuse color of light */
	vec3     position;         /* position in world space */
	vec3     direction;        /* direction in world space */
	vec3     attenuation;      /* Attenuation */
	vec4     param;            /* range, falloff, theta, phi */
};

// Must match with counterpart in VulkanEffect.h

struct Flow
{
	bool Emis;		// Enable Emission Maps
	bool Spec;		// Enable Specular Maps
	bool Refl;		// Enable Reflection Maps
	bool Transl;	// Enble translucent effect
	bool Transm;	// Enable transmissive effect
	bool Rghn;		// Enable roughness map
	bool Norm;		// Enable normal map
	bool Metl;		// Enable metalness map
	bool Heat;		// Enable heat map
};


// Must match with counterpart VulkanTune in VulkanUtil.h

struct Tune
{
	vec4 Albe;		// Tune Diffese Maps
	vec4 Emis;		// Tune Emission Maps
	vec4 Spec;		// Tune Specular Maps
	vec4 Refl;		// Tune Reflection Maps
	vec4 Transl;	// Tune translucent effect
	vec4 Transm;	// Tune transmissive effect
	vec4 Norm;		// Tune normal map
	vec4 Rghn;		// Tune roughness map
};


#define Range   0
#define Falloff 1
#define Theta   2
#define Phi     3


#define SH_SIZE		0
#define SH_INVSIZE	1


// ---------------------------------------------------------------------------
// The parameter block. Every `uniform extern` of D3D9Client.fx, in its order.
//
// The order is the reference's and nothing depends on it -- the client writes
// each name at the offset reflection reports -- but keeping it makes the two
// files diffable, and keeps the C++ mirrors (VulkanSun, VulkanMatExt,
// VulkanTune, LightStruct, TexFlow, MATERIAL) aligned with the structs above.
//
// THE TEXTURE PARAMETERS ARE NOT HERE. `uniform extern texture gTex0;` and
// its fourteen siblings are descriptors rather than bytes, and in GLSL a
// texture without a sampler cannot be declared at all; they appear below as
// the samplers that read them, and VulkanClient.tech carries the join.
// ---------------------------------------------------------------------------
layout(set = 0, binding = 0, scalar) uniform VulkanClientBlock
{
	vec3     kernel[KERNEL_SIZE];

	mat4     gW;			    // World matrix
	mat4     gLVP;			    // Light view projection
	mat4     gVP;			    // Combined View and Projection matrix
	mat4     gGrpT;	            // Mesh group transformation matrix
	vec4     gAttennuate;       // (Mesh Constant Fog) Attennuation of fragment color
	vec4     gInScatter;        // (Mesh Constant Fog) In scattering light
	vec4     gColor;            // General purpose color parameter
	vec4     gFogColor;         // Distance fog color in "Legacy" implementation
	vec4     gAtmColor;         // Earth glow color
	vec4     gTexOff;			// Texture offsets used by surface manager
	vec4     gRadius;           // PlanetRad, AtmOuterLimit, CameraRad, CameraAlt
	vec4     gSHD;				// ShadowMap data
	vec3     gCameraPos;        // Planet relative camera position, Unit vector
	vec3     gNorth;
	vec3     gEast;
	Sun		 gSun;				// Sun light direction
	Mat      gMat;			    // Material input structure  TODO:  Remove all reference to this. Use gMtrl
	Mat      gWater;			// Water material input structure
	Mtrl     gMtrl;			    // Material input structure
	Tune     gTune;			    // Texture tuning parameters
	Light	 gLights[MAX_LIGHTS];
	bool	 gLightsEnabled;
	bool     gTuneEnabled;
	bool     gModAlpha;		    // Configuration input
	bool     gFullyLit;			// Always fully lit bypass lighting calculations
	bool     gTextured;			// Enable Diffuse Texturing
	bool     gFresnel;			// Enable fresnel material
	bool     gPBRSw;			// Legacy / PBR Switch
	bool     gRghnSw;			// Roughness converter switch
	bool     gNight;			// Nighttime/Daytime
	bool     gShadowsEnabled;	// Enable shadow maps
	bool     gEnvMapEnable;		// Enable Environment mapping
	bool	 gInSpace;			// True if a mesh is located in space
	bool	 gNoColor;			// No color flag
	bool	 gBaseBuilding;
	bool	 gOITEnable;
	int      gSpecMode;
	int      gHazeMode;
	float    gProxySize;		// Cosine of the angular size of the Proxy Gbody. (one half)
	float	 gInvProxySize;		// = 1.0 / (1.0f-gProxySize)
	float    gPointScale;
	float    gDistScale;
	float    gFogDensity;
	float    gTime;
	float    gMix;				// General purpose parameter (multible uses)
	float 	 gMtrlAlpha;
	float	 gGlowConst;
	float	 gNightTime;		// 1 for nighttime, 0 for daytime
	Flow	 gCfg;

	// Legacy Atmosphere --------------------------------------------------------

	float    gGlobalAmb;        // Global Ambient Level
	float    gSunAppRad;        // Sun apparent size (Radius / Distance)
	float    gDispersion;
	float    gAmbient0;
};


// ----------------------------------------------------------------------------
// Texture Sampler implementations
//
// One per sampler_state block of D3D9Client.fx, in its declaration order, at
// bindings 1..24. Their filtering and addressing are in VulkanClient.tech --
// see note D above. gTex0 alone is read by eight of them, which is why the
// texture name cannot be the binding.
// ----------------------------------------------------------------------------

layout(set = 0, binding =  1) uniform sampler2D   IrradS;    // <gIrradianceMap>
layout(set = 0, binding =  2) uniform sampler2D   ShadowS;   // <gShadowMap>
layout(set = 0, binding =  3) uniform sampler2D   WrapS;     // <gTex0>
layout(set = 0, binding =  4) uniform sampler2D   ClampS;    // <gTex0>
layout(set = 0, binding =  5) uniform sampler2D   SpecS;     // <gSpecMap>
layout(set = 0, binding =  6) uniform sampler2D   EmisS;     // <gEmisMap>
layout(set = 0, binding =  7) uniform sampler2D   ReflS;     // <gReflMap>
layout(set = 0, binding =  8) uniform sampler2D   MetlS;     // <gMetlMap>
layout(set = 0, binding =  9) uniform sampler2D   HeatS;     // <gHeatMap>
layout(set = 0, binding = 10) uniform sampler2D   RghnS;     // <gRghnMap>
layout(set = 0, binding = 11) uniform sampler2D   TranslS;   // <gTranslMap>
layout(set = 0, binding = 12) uniform sampler2D   TransmS;   // <gTransmMap>
layout(set = 0, binding = 13) uniform sampler2D   Tex1S;     // <gTex1>
layout(set = 0, binding = 14) uniform sampler2D   Nrm0S;     // <gTex3>
layout(set = 0, binding = 15) uniform sampler2D   MFDSamp;   // <gTex0>
layout(set = 0, binding = 16) uniform sampler2D   Panel0S;   // <gTex0>
layout(set = 0, binding = 17) uniform sampler2D   SimpleS;   // <gTex0>
layout(set = 0, binding = 18) uniform sampler2D   ExhaustS;  // <gTex0>
layout(set = 0, binding = 19) uniform sampler2D   RingS;     // <gTex0>
layout(set = 0, binding = 20) uniform samplerCube EnvMapAS;  // <gEnvMapA>
layout(set = 0, binding = 21) uniform samplerCube EnvMapBS;  // <gEnvMapB>
layout(set = 0, binding = 22) uniform sampler2D   Planet0S;  // <gTex0>
layout(set = 0, binding = 23) uniform sampler2D   Planet1S;  // <gTex1>
layout(set = 0, binding = 24) uniform sampler2D   Planet3S;  // <gTex3>


// ----------------------------------------------------------------------------
// The pixel stage's one output. `float4 f(...) : COLOR` in every pixel entry
// point of the reference, so it is declared once here rather than per entry:
// only one fragment entry point is ever compiled at a time, and they all
// write this.
// ----------------------------------------------------------------------------
#ifdef _FRAGMENT_SHADER
layout(location = 0) out vec4 oColor;
#endif


// ----------------------------------------------------------------------------
// Atmospheric Haze implementation
//
// att = attennuation, ins = inscatter, depth = pixel depth [0 to 1],
// posW = camera centric world space position of the vertex
// ----------------------------------------------------------------------------

void AtmosphericHaze(out vec4 att, out vec4 ins, in float depth, in vec3 posW)
{
	if (gHazeMode==0) {
		att = vec4(1.0f);
		ins = vec4(0.0f);
		return;
	}
	else if (gHazeMode==1) {
		att = gAttennuate;
		ins = gInScatter;
		return;
	}
	else if (gHazeMode==2) {
		float fogFact = 1.0f / exp(max(0.0f,depth) * gFogDensity);
		att = vec4(fogFact);
		ins = vec4((1.0f-fogFact) * gFogColor.rgb, 0.0f);
		return;
	}
	// The reference falls out of the three tests with att and ins UNWRITTEN
	// for any other gHazeMode. In HLSL that is a use of an uninitialised
	// `out`; in GLSL it is the same, and glslang will not compile a function
	// whose `out` parameter can escape unassigned. Writing the gHazeMode==0
	// answer here is the smallest thing that keeps the three real cases
	// byte-identical -- and gHazeMode only ever holds 0, 1 or 2 (D3D9Effect's
	// eHazeMode is set from a two-valued flag and from the constant 2).
	att = vec4(1.0f);
	ins = vec4(0.0f);
}


// ----------------------------------------------------------------------------
// Legacy sun color on planet surface. Used for planet surface, base tiles and
// buildings.  See SurfaceLighting() in VulkanUtil.cpp
// ----------------------------------------------------------------------------

void LegacySunColor(out vec4 diff, out float ambi, out float nigh, in vec3 normalW)
{
	float   h = dot(-gSun.Dir, normalW);
	float   s = clamp((h+gSunAppRad)/(2.0f*gSunAppRad), 0.0f, 1.0f);
	vec3   r0 = 1.0 - vec3(0.65, 0.75, 1.0) * gDispersion;

	if (gDispersion!=0.0f) { // case 1: planet has atmosphere
		vec3 di = (r0 + (1.0-r0) * clamp(h*5.780, 0.0f, 1.0f)) * s;
		float  ni = (h+0.242)*2.924;
		float  am = clamp(max(gAmbient0*clamp(ni, 0.0f, 1.0f)-0.05, gGlobalAmb), 0.0f, 1.0f);

		diff = vec4(di*(1.0-am*0.5),1);
		ambi = am;
		nigh = clamp(-ni-0.2, 0.0f, 1.0f);
	}
	else { // case 2: planet has no atmosphere
		diff = vec4(r0*s, 1);
		ambi = gGlobalAmb;
		nigh = 0.0f;
	}
}


// ----------------------------------------------------------------------------
// struct SimpleVS -- BasicVS's output, read by five pixel shaders.
//
//     float4 posH   : POSITION0;   -> gl_Position
//     float2 tex0   : TEXCOORD0;   -> location 0
//     float3 nrmW   : TEXCOORD1;   -> location 1
//     float3 toCamW : TEXCOORD2;   -> location 2
// ----------------------------------------------------------------------------
#if defined(_EP_BasicVS)      || defined(_EP_SimpleTechPS) || \
	defined(_EP_PanelTechPS)  || defined(_EP_PanelTechBPS) || \
	defined(_EP_ExhaustTechPS)|| defined(_EP_SpotTechPS)
	#define _IO_SimpleVS 1
#endif

#if defined(_IO_SimpleVS) && defined(_VERTEX_SHADER)
layout(location = 0) out vec2 vs_tex0;
layout(location = 1) out vec3 vs_nrmW;
layout(location = 2) out vec3 vs_toCamW;
#endif

#if defined(_IO_SimpleVS) && defined(_FRAGMENT_SHADER)
layout(location = 0) in vec2 ps_tex0;
layout(location = 1) in vec3 ps_nrmW;
layout(location = 2) in vec3 ps_toCamW;
#endif


// ----------------------------------------------------------------------------
// struct HazeVS -- HazeTechVS (HorizonHaze.fx) -> HazeTechPS
//
//     float4 posH  : POSITION0;  -> gl_Position
//     float4 color : TEXCOORD0;  -> location 0
//     float2 tex0  : TEXCOORD1;  -> location 1
// ----------------------------------------------------------------------------
#if defined(_EP_HazeTechVS) || defined(_EP_HazeTechPS)
	#define _IO_HazeVS 1
#endif

#if defined(_IO_HazeVS) && defined(_VERTEX_SHADER)
layout(location = 0) out vec4 vs_hz_color;
layout(location = 1) out vec2 vs_hz_tex0;
#endif

#if defined(_IO_HazeVS) && defined(_FRAGMENT_SHADER)
layout(location = 0) in vec4 ps_hz_color;
layout(location = 1) in vec2 ps_hz_tex0;
#endif


// ----------------------------------------------------------------------------
// struct BShadowVS -- ArrowTechVS, and Mesh.fx's ShadowMapVS, BoundingBoxVS
//                     and BoundingSphereVS -> ArrowTechPS, ShadowMapPS,
//                     BoundingBoxPS
//
//     float4 posH  : POSITION0;  -> gl_Position
//     float2 dstW  : TEXCOORD0;  -> location 0
//     float  alpha : TEXCOORD1;  -> location 1
// ----------------------------------------------------------------------------
#if defined(_EP_ArrowTechVS)      || defined(_EP_ShadowMapVS) || \
	defined(_EP_BoundingBoxVS)    || defined(_EP_BoundingSphereVS) || \
	defined(_EP_ArrowTechPS)      || defined(_EP_ShadowMapPS) || \
	defined(_EP_BoundingBoxPS)
	#define _IO_BShadowVS 1
#endif

#if defined(_IO_BShadowVS) && defined(_VERTEX_SHADER)
layout(location = 0) out vec2 vs_bs_dstW;
layout(location = 1) out float vs_bs_alpha;
#endif

#if defined(_IO_BShadowVS) && defined(_FRAGMENT_SHADER)
layout(location = 0) in vec2 ps_bs_dstW;
layout(location = 1) in float ps_bs_alpha;
#endif


// ----------------------------------------------------------------------------
// struct ShadowTexVS -- Mesh.fx's ShadowMeshTechVS, ShadowMeshTechExVS and
//                       ShadowMapOIT_VS -> ShadowTechPS, ShadowMapOIT_PS
//
//     float4 posH : POSITION0;  -> gl_Position
//     float2 dstW : TEXCOORD0;  -> location 0
//     float3 tex0 : TEXCOORD1;  -> location 1
// ----------------------------------------------------------------------------
#if defined(_EP_ShadowMeshTechVS)   || defined(_EP_ShadowMeshTechExVS) || \
	defined(_EP_ShadowMapOIT_VS)    || defined(_EP_ShadowTechPS)       || \
	defined(_EP_ShadowMapOIT_PS)
	#define _IO_ShadowTexVS 1
#endif

#if defined(_IO_ShadowTexVS) && defined(_VERTEX_SHADER)
layout(location = 0) out vec2 vs_st_dstW;
layout(location = 1) out vec3 vs_st_tex0;
#endif

#if defined(_IO_ShadowTexVS) && defined(_FRAGMENT_SHADER)
layout(location = 0) in vec2 ps_st_dstW;
layout(location = 1) in vec3 ps_st_tex0;
#endif


// ----------------------------------------------------------------------------
// Vertex shader implementations
// ----------------------------------------------------------------------------

#if defined(_EP_BasicVS)

// struct NTVERTEX -- Orbiter Mesh vertex layout (pNTVertexDecl)
layout(location = 0) in vec3 posL;		// POSITION0
layout(location = 1) in vec3 nrmL;		// NORMAL0
layout(location = 2) in vec2 tex0;		// TEXCOORD0

void BasicVS()
{
	// `SimpleVS outVS = (SimpleVS)0;` has no counterpart and needs none:
	// every member is assigned. Same everywhere below, and not repeated.
	vec3 posW  = (gW * vec4(posL, 1.0f)).xyz;
	gl_Position = gVP * vec4(posW, 1.0f);
	vs_nrmW    = (gW * vec4(nrmL, 0.0f)).xyz;
	vs_toCamW  = -posW;
	vs_tex0    = tex0;
}

#endif // _EP_BasicVS


// ----------------------------------------------------------------------------
// PixelShader Implementations
// ----------------------------------------------------------------------------

#if defined(_EP_SimpleTechPS)
void SimpleTechPS()
{
	vec4 c = texture(SimpleS, ps_tex0);
	oColor = vec4(c.rgb, c.a * gMix);
}
#endif

#if defined(_EP_PanelTechPS)
void PanelTechPS()
{
	vec4 cTex = texture(SimpleS, ps_tex0);
	oColor = vec4(cTex.rgb, cTex.a*gMix);
}
#endif

#if defined(_EP_PanelTechBPS)
void PanelTechBPS()
{
	vec4 cTex = texture(Panel0S, ps_tex0);
	oColor = vec4(cTex.rgb, cTex.a*gMix);
}
#endif

#if defined(_EP_ExhaustTechPS)
void ExhaustTechPS()
{
	vec4 c = texture(ExhaustS, ps_tex0);
	oColor = vec4(c.rgb, c.a*gMix);
}
#endif

#if defined(_EP_SpotTechPS)
void SpotTechPS()
{
	oColor = (texture(SimpleS, ps_tex0) * gColor) * gMix;
}
#endif


// ###########################################################################
// #include "Particle.fx"
// ###########################################################################

// ----------------------------------------------------------------------------
// struct ParticleVS
//
//     float4 posH  : POSITION0;   -> gl_Position
//     float2 tex0  : TEXCOORD0;   -> location 0
//     float  light : TEXCOORD1;   -> location 1
// ----------------------------------------------------------------------------
#if defined(_EP_ParticleDiffuseVS) || defined(_EP_ParticleEmissiveVS) || \
	defined(_EP_ParticleDiffusePS) || defined(_EP_ParticleEmissivePS) || \
	defined(_EP_ParticleShadowPS)
	#define _IO_ParticleVS 1
#endif

#if defined(_IO_ParticleVS) && defined(_VERTEX_SHADER)
layout(location = 0) out vec2 vs_pv_tex0;
layout(location = 1) out float vs_pv_light;
#endif

#if defined(_IO_ParticleVS) && defined(_FRAGMENT_SHADER)
layout(location = 0) in vec2 ps_pv_tex0;
layout(location = 1) in float ps_pv_light;
#endif


#if defined(_EP_ParticleDiffuseVS)

// struct NTVERTEX (pNTVertexDecl). nrmL is declared by the vertex layout and
// read by neither stage -- the reference's own `dot(-gSun.Dir, vrt.nrmL)` is
// commented out -- so it is absent here; a pipeline may describe an attribute
// its shader ignores.
layout(location = 0) in vec3 posL;		// POSITION0
layout(location = 2) in vec2 tex0;		// TEXCOORD0

void ParticleDiffuseVS()
{
	vs_pv_tex0    = tex0;
	vs_pv_light   = 1.0f; // saturate(dot(-gSun.Dir, vrt.nrmL) * 2.0f);
	gl_Position   = gVP * vec4(posL, 1.0f);
}

#endif // _EP_ParticleDiffuseVS


#if defined(_EP_ParticleEmissiveVS)

// struct EPVERTEX (pPosTexDecl)
layout(location = 0) in vec3 posL;		// POSITION0
layout(location = 1) in vec2 tex0;		// TEXCOORD0

void ParticleEmissiveVS()
{
	// `light` is left at the zero the reference's `(ParticleVS)0` gave it.
	// GLSL has no struct-wide zero cast, so it is written out -- and it has
	// to be, because ParticleDiffusePS multiplies by it.
	vs_pv_light  = 0.0f;
	vs_pv_tex0   = tex0;
	gl_Position  = gVP * vec4(posL, 1.0f);
}

#endif // _EP_ParticleEmissiveVS



// ----------------------------------------------------------------------------
// gMix is the particle opacity computed from time and halflife
// gColor is hardcoded to [1,1,1] in exhaust streams and [1, 0.7, 0.5] in reentry streams
// frg.light is a sun light intensity level illuminating a particles. Light color is [1,1,1]
// ----------------------------------------------------------------------------


#if defined(_EP_ParticleDiffusePS)
void ParticleDiffusePS()
{
	vec4 color = texture(WrapS, ps_pv_tex0);
	oColor = vec4(color.rgb*ps_pv_light, color.a*gMix);
}
#endif

#if defined(_EP_ParticleEmissivePS)
void ParticleEmissivePS()
{
	vec4 color = texture(WrapS, ps_pv_tex0);
	oColor = vec4(color.rgb*gColor.rgb, color.a*gMix);
}
#endif

#if defined(_EP_ParticleShadowPS)
void ParticleShadowPS()
{
	vec4 color = texture(WrapS, ps_pv_tex0);
	oColor = vec4(0,0,0,color.a*gMix*2.0);
}
#endif


// ###########################################################################
// #include "Mesh.fx"
// ###########################################################################

// ----------------------------------------------------------------------------
// struct TileMeshVS -- BaseTileVS -> BaseTilePS
//
//     float4 posH  : POSITION0;  -> gl_Position
//     float3 CamW  : TEXCOORD0;  -> location 0
//     float2 tex0  : TEXCOORD1;  -> location 1
//     float3 nrmW  : TEXCOORD2;  -> location 2
//     float4 atten : COLOR0;     -> location 3
//     float4 insca : COLOR1;     -> location 4
// ----------------------------------------------------------------------------
#if defined(_EP_BaseTileVS) || defined(_EP_BaseTilePS)
	#define _IO_TileMeshVS 1
#endif

#if defined(_IO_TileMeshVS) && defined(_VERTEX_SHADER)
layout(location = 0) out vec3 vs_tm_CamW;
layout(location = 1) out vec2 vs_tm_tex0;
layout(location = 2) out vec3 vs_tm_nrmW;
layout(location = 3) out vec4 vs_tm_atten;
layout(location = 4) out vec4 vs_tm_insca;
#endif

#if defined(_IO_TileMeshVS) && defined(_FRAGMENT_SHADER)
layout(location = 0) in vec3 ps_tm_CamW;
layout(location = 1) in vec2 ps_tm_tex0;
layout(location = 2) in vec3 ps_tm_nrmW;
layout(location = 3) in vec4 ps_tm_atten;
layout(location = 4) in vec4 ps_tm_insca;
#endif


// ----------------------------------------------------------------------------
// struct MeshVS -- TinyMeshTechVS / AxisTechVS -> TinyMeshTechPS, RingTechPS,
//                  RingTech2PS, AxisTechPS
//
//     float4 posH : POSITION0;  -> gl_Position
//     float3 CamW : TEXCOORD0;  -> location 0
//     float2 tex0 : TEXCOORD1;  -> location 1
//     float3 nrmW : TEXCOORD2;  -> location 2
// ----------------------------------------------------------------------------
#if defined(_EP_TinyMeshTechVS) || defined(_EP_AxisTechVS)   || \
	defined(_EP_TinyMeshTechPS) || defined(_EP_RingTechPS)   || \
	defined(_EP_RingTech2PS)    || defined(_EP_AxisTechPS)
	#define _IO_MeshVS 1
#endif

#if defined(_IO_MeshVS) && defined(_VERTEX_SHADER)
layout(location = 0) out vec3 vs_mv_CamW;
layout(location = 1) out vec2 vs_mv_tex0;
layout(location = 2) out vec3 vs_mv_nrmW;
#endif

#if defined(_IO_MeshVS) && defined(_FRAGMENT_SHADER)
layout(location = 0) in vec3 ps_mv_CamW;
layout(location = 1) in vec2 ps_mv_tex0;
layout(location = 2) in vec3 ps_mv_nrmW;
#endif


// struct TileMeshNMVS IS NOT DECLARED HERE. It is Mesh.fx's output struct for
// BaseTileNMVS/BaseTileNMPS, and neither of those functions exists: the only
// pass that named them is the one commented out at the top of BaseTileTech.
// An HLSL struct nothing returns costs nothing; a GLSL interpolant set that
// nothing writes costs seven locations in every pipeline built from this
// file, because glslang lists every declared output in OpEntryPoint's
// interface. So it is left out, with this note in its place.


#if defined(_EP_TinyMeshTechVS)

// struct MESH_VERTEX (pMeshVertexDecl). tanL is described by the vertex
// layout and read by neither stage of this technique.
layout(location = 0) in vec3 posL;		// POSITION0
layout(location = 1) in vec3 nrmL;		// NORMAL0
layout(location = 3) in vec3 tex0;		// TEXCOORD0

void TinyMeshTechVS()
{
	vec3 posW = (gW * vec4(posL, 1.0f)).xyz;	// Apply world transformation matrix
	gl_Position = gVP * vec4(posW, 1.0f);
	vec3 nrmW = (gW * vec4(nrmL, 0.0f)).xyz;	// Apply world transformation matri
	vs_mv_nrmW  = normalize(nrmW);
	vs_mv_CamW  = -posW;
	vs_mv_tex0  = tex0.xy;
}

#endif // _EP_TinyMeshTechVS


#if defined(_EP_TinyMeshTechPS)
void TinyMeshTechPS()
{
	oColor = vec4(0,1,0,1);
	return;

	// EVERYTHING BELOW IS UNREACHABLE IN THE REFERENCE TOO -- the `return
	// float4(0,1,0,1);` above is the reference's own first statement, and
	// what follows it is the shader the author left in place. It is
	// converted rather than dropped, because dropping it would be a decision
	// about what he meant to keep. GLSL accepts unreachable statements, as
	// HLSL does, and glslang removes them from the SPIR-V.

	// Normalize input
	vec3 nrmW = normalize(ps_mv_nrmW);
	vec3 CamW = normalize(ps_mv_CamW);
	vec4 cSpec = gMtrl.specular;
	vec4 cTex = vec4(1.0f);

	if (gTextured) {
		if (gNoColor) cTex.a = texture(WrapS, ps_mv_tex0.xy).a;
		else cTex = texture(WrapS, ps_mv_tex0.xy);
	}

	if (gFullyLit) {
		oColor = vec4(cTex.rgb*clamp(gMtrl.diffuse.rgb + gMtrl.emissive.rgb, 0.0f, 1.0f), cTex.a);
		return;
	}

	cTex.a *= gMtrlAlpha;

	// Sunlight calculations. Saturate with cSpec.a to gain an ability to disable specular light
	float  d = clamp(-dot(gSun.Dir, nrmW), 0.0f, 1.0f);
	float  s = pow(clamp(dot(reflect(gSun.Dir, nrmW), CamW), 0.0f, 1.0f), cSpec.a) * clamp(cSpec.a, 0.0f, 1.0f);

	if (d == 0.0f) s = 0.0f;

	vec3 diff = gMtrl.diffuse.rgb * (d * clamp(gSun.Color, 0.0f, 1.0f)); // Compute total diffuse light
	diff += (gMtrl.ambient.rgb*gSun.Ambient) + (gMtrl.emissive.rgb);

	vec3 cTot = cSpec.rgb * (s * gSun.Color);	// Compute total specular light

	cTex.rgb *= clamp(diff, 0.0f, 1.0f);	// Lit the diffuse texture

#if defined(_GLASS)
	cTex.a = clamp(cTex.a + max(max(cTot.r, cTot.g), cTot.b), 0.0f, 1.0f);	// Re-compute output alpha for alpha blending stage
#endif

	cTex.rgb += cTot.rgb;											// Apply reflections to output color

	oColor = cTex;
}
#endif



// ============================================================================
// Planet Rings Technique
// ============================================================================

#if defined(_EP_RingTechPS)
void RingTechPS()
{
	vec4 color = texture(RingS, ps_mv_tex0);

	vec3 pp = gCameraPos*gRadius[2] - ps_mv_CamW*gDistScale;

	float  da = dot(normalize(pp), gSun.Dir);
	float  r  = sqrt(dot(pp,pp) * (1.0-da*da));

	float sh  = max(0.05, smoothstep(gRadius[0], gRadius[1], r));

	if (da<0.0f) sh = 1.0f;

	if ((dot(ps_mv_nrmW, ps_mv_CamW)*dot(ps_mv_nrmW, gSun.Dir))>0.0f) {
		oColor = vec4(color.rgb*0.35f*sh, color.a);
		return;
	}
	oColor = vec4(color.rgb*sh, color.a);
}
#endif

#if defined(_EP_RingTech2PS)
void RingTech2PS()
{
	vec3 pp  = gCameraPos*gRadius[2] - ps_mv_CamW*gDistScale;
	float  dpp = dot(pp,pp);
	float  len = sqrt(dpp);

	len = clamp(smoothstep(gTexOff.x, gTexOff.y, len), 0.0f, 1.0f);

	vec4 color = texture(RingS, vec2(len, 0.5));
	color.a = color.r*0.75;

	float  da = dot(normalize(pp), gSun.Dir);
	float  r  = sqrt(dpp*(1.0-da*da));

	float sh  = max(0.05, smoothstep(gRadius[0], gRadius[1], r));

	if (da<0.0f) sh = 1.0f;

	color.rgb *= sh;

	if ((dot(ps_mv_nrmW, ps_mv_CamW)*dot(ps_mv_nrmW, gSun.Dir))>0.0f) {
		oColor = vec4(color.rgb*0.35f, color.a);
		return;
	}
	oColor = vec4(color.rgb, color.a);
}
#endif


// ============================================================================
// Base Tile Rendering Technique
// ============================================================================

#if defined(_EP_BaseTileVS)

// struct NTVERTEX (pNTVertexDecl)
layout(location = 0) in vec3 posL;		// POSITION0
layout(location = 1) in vec3 nrmL;		// NORMAL0
layout(location = 2) in vec2 tex0;		// TEXCOORD0

void BaseTileVS()
{
	vec3 posW  = (gW * vec4(posL, 1.0f)).xyz;
	gl_Position = gVP * vec4(posW, 1.0f);
	vs_tm_nrmW   = (gW * vec4(nrmL, 0.0f)).xyz;
	vs_tm_tex0   = tex0;
	vs_tm_CamW   = -posW;

	// Atmospheric haze -------------------------------------------------------

	AtmosphericHaze(vs_tm_atten, vs_tm_insca, gl_Position.z, posW);

	vec4 diffuse;
	float ambi, nigh;

	LegacySunColor(diffuse, ambi, nigh, vs_tm_nrmW);

	vs_tm_insca *= (diffuse+ambi);
	vs_tm_insca.a = nigh;
}

#endif // _EP_BaseTileVS


#if defined(_EP_BaseTilePS)
void BaseTilePS()
{
	// Normalize input
	vec3 nrmW = normalize(ps_tm_nrmW);
	vec3 CamW = normalize(ps_tm_CamW);

	vec4 cTex = texture(ClampS, ps_tm_tex0);

	vec3 r = reflect(gSun.Dir, nrmW);
	float  s = pow(clamp(dot(r, CamW), 0.0f, 1.0f), 20.0f) * (1.0f-cTex.a);
	float  d = clamp(dot(-gSun.Dir, nrmW), 0.0f, 1.0f);

	if (d<=0.0f) s = 0.0f;

	vec3 clr = cTex.rgb * clamp(d * gSun.Color + s * gSun.Color + gSun.Ambient, 0.0f, 1.0f);

	if (gNight) clr += texture(Tex1S, ps_tm_tex0).rgb;

	oColor = vec4(clr.rgb*ps_tm_atten.rgb+ps_tm_insca.rgb, cTex.a);
	//return float4(clr.rgb*frg.atten.rgb+frg.insca.rgb, cTex.a*(1-frg.insca.a));	// Make basetiles transparent during night
}
#endif


// ============================================================================
// Vessel Axis vector technique
// ============================================================================

#if defined(_EP_AxisTechVS)

// struct MESH_VERTEX (pMeshVertexDecl)
layout(location = 0) in vec3 posL;		// POSITION0
layout(location = 1) in vec3 nrmL;		// NORMAL0
layout(location = 3) in vec3 tex0;		// TEXCOORD0

void AxisTechVS()
{
	float  stretch = tex0.x * gMix;
	vec3 posX = posL + vec3(0.0, stretch, 0.0);
	vec3 posW = (gW * vec4(posX, 1.0f)).xyz;			// Apply world transformation matrix
	gl_Position = gVP * vec4(posW, 1.0f);
	vec3 nrmW = (gW * vec4(nrmL, 0.0f)).xyz;			// Apply world transformation matrix

	vs_mv_nrmW  = normalize(nrmW);
	vs_mv_CamW  = -posW;
	// tex0 is left at the zero the reference's `(MeshVS)0` gave it. Written
	// out because GLSL has no struct-wide zero cast; AxisTechPS never reads
	// it, but an interpolant left unwritten is undefined rather than zero.
	vs_mv_tex0  = vec2(0.0f);
}

#endif // _EP_AxisTechVS


#if defined(_EP_AxisTechPS)
void AxisTechPS()
{
	vec3 nrmW = normalize(ps_mv_nrmW);
	float  d = clamp(dot(-gSun.Dir, nrmW), 0.0f, 1.0f);
	vec3 clr = gColor.rgb * clamp(max(d,0.0f) + 0.5, 0.0f, 1.0f);
	oColor = vec4(clr, gColor.a);
}
#endif


// ============================================================================
// Mesh Shadow Technique
// ============================================================================

#if defined(_EP_ShadowMeshTechVS)

// struct POSTEX (pPosTexDecl)
layout(location = 0) in vec3 posL;		// POSITION0
layout(location = 1) in vec2 tex0;		// TEXCOORD0

void ShadowMeshTechVS()
{
	vec3 posW = (gW * vec4(posL.xyz, 1.0f)).xyz;
	float alpha = dot(posL.xyz, gInScatter.xyz) + gInScatter.w;
	gl_Position = gVP * vec4(posW, 1.0f);
	vs_st_tex0  = vec3(tex0.xy, alpha);
	vs_st_dstW  = gl_Position.zw;
}

#endif // _EP_ShadowMeshTechVS


#if defined(_EP_ShadowMeshTechExVS)

// struct POSTEX (pPosTexDecl)
layout(location = 0) in vec3 posL;		// POSITION0
layout(location = 1) in vec2 tex0;		// TEXCOORD0

void ShadowMeshTechExVS()
{
	float alpha = dot(posL.xyz, gColor.xyz) + gColor.w;
	vec3 posX = (gGrpT * vec4(posL.xyz, 1.0f)).xyz;
	vec3 posW = (gW * vec4(posX, 1.0f)).xyz;
	gl_Position = gVP * vec4(posW, 1.0f);
	vs_st_tex0  = vec3(tex0.xy, alpha);
	vs_st_dstW  = gl_Position.zw;
}

#endif // _EP_ShadowMeshTechExVS


#if defined(_EP_ShadowTechPS)
void ShadowTechPS()
{
	if (ps_st_tex0.b < 0.0f) discard;
	if (gOITEnable) {
		vec4 alpha = texture(WrapS, ps_st_tex0.xy);
		if (alpha.a < 0.5f) discard;
	}
	oColor = vec4(0.0f, 0.0f, 0.0f, gMix);
}
#endif


// -----------------------------------------------------------------------------------
// Shadow Map rendering with plain geometry (without texture)
//
#if defined(_EP_ShadowMapVS)

// struct SHADOW_VERTEX -- `float4 posL : POSITION0` (pVector4Decl)
layout(location = 0) in vec4 posL;		// POSITION0

void ShadowMapVS()
{
	vec3 posW = (gW * vec4(posL.xyz, 1.0f)).xyz;
	gl_Position = gLVP * vec4(posW, 1.0f);
	vs_bs_dstW = gl_Position.zw;
	// `alpha` is left at the zero the reference's `(BShadowVS)0` gave it.
	vs_bs_alpha = 0.0f;
}

#endif // _EP_ShadowMapVS


#if defined(_EP_ShadowMapPS)
void ShadowMapPS()
{
	// `return 1 - (frg.dstW.x / frg.dstW.y);` -- a scalar returned from a
	// float4 function, which HLSL broadcasts to all four channels.
	oColor = vec4(1.0f - (ps_bs_dstW.x / ps_bs_dstW.y));
}
#endif


// -----------------------------------------------------------------------------------
// Shadow Map rendering with texture alpha included
//
#if defined(_EP_ShadowMapOIT_VS)

// struct POSTEX (pPosTexDecl)
layout(location = 0) in vec3 posL;		// POSITION0
layout(location = 1) in vec2 tex0;		// TEXCOORD0

void ShadowMapOIT_VS()
{
	vec3 posW = (gW * vec4(posL.xyz, 1.0f)).xyz;
	gl_Position = gLVP * vec4(posW, 1.0f);
	vs_st_tex0 = vec3(tex0.xy, 0);
	vs_st_dstW = gl_Position.zw;
}

#endif // _EP_ShadowMapOIT_VS


#if defined(_EP_ShadowMapOIT_PS)
void ShadowMapOIT_PS()
{
	if (gOITEnable) {
		float alpha = texture(WrapS, ps_st_tex0.xy).a;
		if (alpha < 0.5f) { oColor = vec4(1.0f); return; }
	}
	oColor = vec4(1.0f - (ps_st_dstW.x / ps_st_dstW.y));
}
#endif


// =============================================================================
// Mesh Bounding Box Technique
// =============================================================================

#if defined(_EP_BoundingBoxVS)

// `float3 posL : POSITION0` (pPositionDecl)
layout(location = 0) in vec3 posL;		// POSITION0

void BoundingBoxVS()
{
	vec3 pos;
	pos.x = gAttennuate.x * posL.x + gInScatter.x * (1.0f-posL.x);
	pos.y = gAttennuate.y * posL.y + gInScatter.y * (1.0f-posL.y);
	pos.z = gAttennuate.z * posL.z + gInScatter.z * (1.0f-posL.z);

	vec3 posX = (gGrpT * vec4(pos, 1.0f)).xyz;		// Apply meshgroup specific transformation
	vec3 posW = (gW * vec4(posX, 1.0f)).xyz;		// Apply world transformation matrix
	gl_Position = gVP * vec4(posW, 1.0f);
	vs_bs_dstW = vec2(0.0f);						// `(BShadowVS)0`
	vs_bs_alpha = 0.0f;
}

#endif // _EP_BoundingBoxVS


#if defined(_EP_BoundingSphereVS)

// `float3 posL : POSITION0` (pPositionDecl)
layout(location = 0) in vec3 posL;		// POSITION0

void BoundingSphereVS()
{
	vec3 posW = (gW * vec4(posL, 1.0f)).xyz;		// Apply world transformation matrix
	gl_Position = gVP * vec4(posW, 1.0f);
	vs_bs_dstW = vec2(0.0f);						// `(BShadowVS)0`
	vs_bs_alpha = 0.0f;
}

#endif // _EP_BoundingSphereVS


#if defined(_EP_BoundingBoxPS)
void BoundingBoxPS()
{
	oColor = gColor;
}
#endif


// ###########################################################################
// #include "Vessel.fx"
// ###########################################################################

// `float3 cLuminosity = { 0.4, 0.7, 0.3 };`
//
// A file-scope initialised global. In an .fx that is a parameter with a
// default value; in GLSL a non-const global may not have an initialiser, and
// nothing ever writes this one, so it is const. Same value, same name.
const vec3 cLuminosity = vec3(0.4, 0.7, 0.3);


float cmax(vec3 color)		// `inline` -- GLSL has no such keyword
{
	return max(max(color.r, color.g), color.b);
}

// Sun light brightness for diffuse and specular lighting
// #include "LightBlur.hlsl"
//
// LightBlur.hlsl IS ALSO A SHADER IN ITS OWN RIGHT -- Scene.cpp compiles it
// as an ImageProcessing effect -- and it is converted separately, as
// LightBlur.glsl. What this #include contributes to THIS effect is two
// defines and nothing else: its uniforms, its four samplers (tBack, tBlur,
// tCLUT, tTone) and its three entry points (PSMain, PSDepth, PSNormal) are
// named by no technique in VulkanClient.tech and by no line of the six files
// that make up this one. Checked across all of them.
//
// Inlining the whole file would put four more sampler bindings into this
// effect's descriptor set layout that nothing ever writes, so only the live
// half is carried, with this note saying where the rest went.
#define fSunIntensity		3.14	// Sunlight intensity
#define fInvSunIntensity	(1.0/fSunIntensity)


// ###########################################################################
// #include "Common.hlsl"    (from Vessel.fx -- "Incluse Light and Shadow")
// ###########################################################################

#define KERNEL_RADIUS 2.0f
#define SHADOW_THRESHOLD 0.1f       // 0.3 to 7.0


// ============================================================================
//
vec4 Paraboloidal_LVLH(sampler2D s, vec3 i)
{
	float z = dot(gCameraPos, i);
	vec2 p = vec2(dot(gEast, i), dot(gNorth, i)) / (1.0f + abs(z));
	p *= vec2(0.2273f, 0.4545f);
	vec4 A = texture(s, p + vec2(0.25f, 0.5f));
	vec4 B = texture(s, p + vec2(0.75f, 0.5f));
	return mix(A, B, smoothstep(-0.03, 0.03, z));
}

vec3 Sq(vec3 x)
{
	return x * x;
}

vec4 Sq(vec4 x)
{
	return x * x;
}


// ==========================================================================================================
// Local light sources
// ==========================================================================================================


vec3 Light_fx(vec3 x)
{
	return clamp(x, 0.0f, 1.0f);  //1.5 - exp2(-x.rgb)*1.5f;
}

// `uniform int x` and `uniform bool bSpec` are HLSL's way of saying "this
// parameter is a compile-time constant". GLSL has no such qualifier and needs
// none: every call below passes a literal, so glslang folds them the same way.
void LocalLights(
	out vec3 diff_out,
	out vec3 spec_out,
	in vec3 nrmW,
	in vec3 posW,
	in float sp,
	int x,
	bool bSpec)
{

	vec3 posWN = normalize(-posW);
	vec3 p[4];
	vec4 spe = vec4(0.0f);
	int i;

	// Relative positions
	for (i = 0; i < 4; i++) p[i] = posW - gLights[i + x].position;

	// Square distances
	vec4 sd;
	for (i = 0; i < 4; i++) sd[i] = dot(p[i], p[i]);

	// Normalize
	sd = inversesqrt(sd);
	for (i = 0; i < 4; i++) p[i] *= sd[i];

	// Distances
	vec4 dst = 1.0f / sd;

	// Attennuation factors
	vec4 att;
	for (i = 0; i < 4; i++) att[i] = dot(gLights[i + x].attenuation.xyz, vec3(1.0, dst[i], dst[i] * dst[i]));

	att = 1.0f / att;

	// Spotlight factors
	vec4 spt;
	for (i = 0; i < 4; i++) {
		spt[i] = (dot(p[i], gLights[i + x].direction) - gLights[i + x].param[Phi]) * gLights[i + x].param[Theta];
		if (gLights[i + x].type == 0) spt[i] = 1.0f;
	}

	spt = clamp(spt, 0.0f, 1.0f);

	// Diffuse light factors
	vec4 dif;
	for (i = 0; i < 4; i++) dif[i] = dot(-p[i], nrmW);

	dif = clamp(dif, 0.0f, 1.0f);
	dif *= (att * spt);

	// Specular lights factors
	if (bSpec) {

		// `* (dif[i] > 0)` -- HLSL promotes the bool to 1.0f/0.0f; GLSL needs
		// the conversion written out.
		for (i = 0; i < 4; i++) spe[i] = dot(reflect(p[i], nrmW), posWN) * float(dif[i] > 0.0f);

		spe = pow(clamp(spe, 0.0f, 1.0f), vec4(sp));
		spe *= (att * spt);
	}

	diff_out = vec3(0.0f);
	spec_out = vec3(0.0f);

	for (i = 0; i < 4; i++) diff_out += gLights[i + x].diffuse.rgb * dif[i];

	if (bSpec) {
		for (i = 0; i < 4; i++) spec_out += gLights[i + x].diffuse.rgb * spe[i];
	}
}


void LocalLightsBeckman(
	out vec3 diff_out,
	out vec3 spec_out,
	in vec3 nrmW,
	in vec3 posW,
	in float fRgh,
	int x,
	bool bSpec)
{

	vec3 camW = normalize(-posW);
	vec3 p[4];
	vec3 H[4];
	vec4 spe = vec4(0.0f);
	vec4 dHN = vec4(0.0f);
	int i;

	// Relative positions
	for (i = 0; i < 4; i++) p[i] = posW - gLights[i + x].position;

	// Square distances
	vec4 sd;
	for (i = 0; i < 4; i++) sd[i] = dot(p[i], p[i]);

	// Normalize
	sd = inversesqrt(sd);
	for (i = 0; i < 4; i++) p[i] *= sd[i];

	// Distances
	vec4 dst = 1.0f / sd;

	if (bSpec) {

		// Halfway Vectors
		vec4 hd;
		for (i = 0; i < 4; i++) H[i] = (camW - p[i]);
		for (i = 0; i < 4; i++) hd[i] = dot(H[i], H[i]);

		hd = inversesqrt(hd);

		for (i = 0; i < 4; i++) H[i] *= hd[i];
		for (i = 0; i < 4; i++) dHN[i] = dot(H[i], nrmW);
	}



	// Attennuation factors
	vec4 att;
	for (i = 0; i < 4; i++) att[i] = dot(gLights[i + x].attenuation.xyz, vec3(1.0, dst[i], dst[i] * dst[i]));

	att = 1.0f / att;

	// Spotlight factors
	vec4 spt;
	for (i = 0; i < 4; i++) {
		spt[i] = (dot(p[i], gLights[i + x].direction) - gLights[i + x].param[Phi]) * gLights[i + x].param[Theta];
		if (gLights[i + x].type == 0) spt[i] = 1.0f;
	}

	spt = clamp(spt, 0.0f, 1.0f);

	// Diffuse light factors
	vec4 dif;
	for (i = 0; i < 4; i++) dif[i] = dot(-p[i], nrmW);

	dif = clamp(dif, 0.0f, 1.0f);

	// Specular lights factors

	if (bSpec) {

		float r2 = fRgh * fRgh;
		vec4 d2 = dHN * dHN;
		vec4 w = 1.0f / (3.14 * r2 * d2 * d2);
		vec4 q = 1.0f / (r2 * d2);

		spe = (att * spt * dif) * w * exp((d2 - 1.0f) * q);
	}

	dif *= (att * spt);

	diff_out = vec3(0.0f);
	spec_out = vec3(0.0f);

	for (i = 0; i < 4; i++) diff_out += Sq(gLights[i + x].diffuse.rgb * dif[i]);

	if (bSpec) {
		for (i = 0; i < 4; i++) spec_out += gLights[i + x].diffuse.rgb * spe[i];
	}
}



void LocalLightsEx(out vec3 cDiffLocal, out vec3 cSpecLocal, in vec3 nrmW, in vec3 posW, in float sp, bool ubBeckman)
{
	// THE TWO ZEROES ARE WRITTEN FIRST, AND THAT IS A CORRECTION.
	//
	// The reference zeroes these only in the LMODE == 0 arm and in the
	// `!gLightsEnabled` test above it -- neither of which returns -- and then
	// every other arm does `cDiffLocal += dd;`. Reading an `out` parameter
	// before it is written is undefined in HLSL and in GLSL alike; on D3D9 it
	// happened to read zero, which is plainly what the accumulation wants.
	// Hoisting the zeroes to the top is the smallest change that makes every
	// arm mean what it says, and it leaves the LMODE == 0 arm's own
	// assignments exactly where they were.
	cDiffLocal = vec3(0.0f);
	cSpecLocal = vec3(0.0f);

#if LMODE !=0
    if (!gLightsEnabled) {
        cDiffLocal = vec3(0.0f);
        cSpecLocal = vec3(0.0f);
    }
#endif

#if LMODE == 0
    cDiffLocal = vec3(0.0f);
    cSpecLocal = vec3(0.0f);
#elif (LMODE & 1) == 1 // partial
    vec3 dd, ss;
	int i;
    if (ubBeckman) {
        for (i = 0; i < MAX_LIGHTS; i += 4) {
            LocalLightsBeckman(dd, ss, nrmW, posW, sp, i, false);
            cDiffLocal += dd;
            cSpecLocal += ss;
        }
    }
    else {
        for (i = 0; i < MAX_LIGHTS; i += 4) {
            LocalLights(dd, ss, nrmW, posW, sp, i, false);
            cDiffLocal += dd;
            cSpecLocal += ss;
        }
    }
#elif (LMODE & 1) == 0 // full
    vec3 dd, ss;
    int i;
    if (ubBeckman) {
        for (i = 0; i < MAX_LIGHTS; i += 4) {
            LocalLightsBeckman(dd, ss, nrmW, posW, sp, i, true);
            cDiffLocal += dd;
            cSpecLocal += ss;
        }
    }
    else {
        for (i = 0; i < MAX_LIGHTS; i += 4) {
            LocalLights(dd, ss, nrmW, posW, sp, i, true);
            cDiffLocal += dd;
            cSpecLocal += ss;
        }
    }
#endif
}


// ==========================================================================================================
// Object Self Shadows
// ==========================================================================================================
//
// ProjectShadows stood here, commented out in the reference. It is not
// carried: a commented-out function has nothing to convert, and the reference
// keeps its own copy.


// ---------------------------------------------------------------------------------------------------
//
float SampleShadows(vec2 sp, float pd)
{

	vec2 dx = vec2(gSHD[1], 0) * 1.5f;
	vec2 dy = vec2(0, gSHD[1]) * 1.5f;
	float  va = 0.0f;

	sp -= dy;
	if ((texture(ShadowS, sp - dx).r) > pd) va++;
	if ((texture(ShadowS, sp).r) > pd) va++;
	if ((texture(ShadowS, sp + dx).r) > pd) va++;
	sp += dy;
	if ((texture(ShadowS, sp - dx).r) > pd) va++;
	if ((texture(ShadowS, sp).r) > pd) va++;
	if ((texture(ShadowS, sp + dx).r) > pd) va++;
	sp += dy;
	if ((texture(ShadowS, sp - dx).r) > pd) va++;
	if ((texture(ShadowS, sp).r) > pd) va++;
	if ((texture(ShadowS, sp + dx).r) > pd) va++;

	return va * 0.1111111f;
}


// ---------------------------------------------------------------------------------------------------
//
float SampleShadows2(vec2 sp, float pd)
{

	float val = 0.0f;
	float m = KERNEL_RADIUS * gSHD[1];

	for (int i = 0; i < KERNEL_SIZE; i++) {
		if ((texture(ShadowS, sp + kernel[i].xy * m).r) > pd) val += kernel[i].z;
	}

	return clamp(val * KERNEL_WEIGHT, 0.0f, 1.0f);
}


// ---------------------------------------------------------------------------------------------------
//
float SampleShadows3(vec2 sp, float pd, vec4 frame)
{

	float val = 0.0f;
	frame *= KERNEL_RADIUS * gSHD[1];

	for (int i = 0; i < KERNEL_SIZE; i++) {
		vec2 ofs = frame.xy * kernel[i].x + frame.zw * kernel[i].y;
		if (texture(ShadowS, sp + ofs).r > pd) val += kernel[i].z;
	}

	return clamp(val * KERNEL_WEIGHT, 0.0f, 1.0f);
}


// ---------------------------------------------------------------------------------------------------
//
float SampleShadowsEx(vec2 sp, float pd, vec4 sc)
{

#if SHDMAP == 1
	return SampleShadows(sp, pd);
#elif SHDMAP == 2 || SHDMAP == 4
	return SampleShadows2(sp, pd);
#else
	float si, co;
	sc += vec4(gSHD[2] * 2.0f);
	// sincos(a, si, co) -- GLSL has the two calls and not the pair.
	float a = sc.y + sc.x * 149.0f;
	si = sin(a); co = cos(a);
	return SampleShadows3(sp, pd, vec4(si, co, co, -si));
#endif
}


// ---------------------------------------------------------------------------------------------------
//
float ComputeShadow(vec4 shdH, float dLN, vec4 sc)
{
	if (!gShadowsEnabled) return 1.0f;

	shdH.xyz /= shdH.w;
	shdH.z = 1.0f - shdH.z;
	vec2 sp = shdH.xy * vec2(0.5f, -0.5f) + vec2(0.5f, 0.5f);

	sp += gSHD[1] * 0.5f;

	if (sp.x < 0.0f || sp.y < 0.0f) return 1.0f;	// If a sample is outside border -> fully lit
	if (sp.x > 1.0f || sp.y > 1.0f) return 1.0f;

	float fShadow;

	float kr = gSHD[0] * KERNEL_RADIUS;
	float dx = inversesqrt(1.0 - dLN * dLN);
	float ofs = kr / (dLN * dx);
	float omx = min(0.05 + ofs, 0.5);

	float  pd = shdH.z + omx * gSHD[3];

	if (pd < 0.0f) pd = 0.0f;
	if (pd > 1.0f) pd = 1.0f;

	fShadow = SampleShadowsEx(sp, pd, sc);

	return 1.0f - fShadow;
}


// ###########################################################################
// #include "PBR.fx"        (from Vessel.fx -- "Must be included here")
// ###########################################################################

// ----------------------------------------------------------------------------
// struct PBRData -- PBR_VS / AdvancedVS / MetalnessVS
//                   -> PBR_PS / AdvancedPS / MetalnessPS
//
//     float4 posH : POSITION0;  -> gl_Position
//     float3 camW : TEXCOORD0;  -> location 0
//     float2 tex0 : TEXCOORD1;  -> location 1
//     float3 nrmW : TEXCOORD2;  -> location 2
//     float4 tanW : TEXCOORD3;  -> location 3   (handiness in .w)
//     float4 shdH : TEXCOORD4;  -> location 4   (#if SHDMAP > 0)
// ----------------------------------------------------------------------------
#if defined(_EP_PBR_VS)      || defined(_EP_AdvancedVS) || defined(_EP_MetalnessVS) || \
	defined(_EP_PBR_PS)      || defined(_EP_AdvancedPS) || defined(_EP_MetalnessPS)
	#define _IO_PBRData 1
#endif

#if defined(_IO_PBRData) && defined(_VERTEX_SHADER)
layout(location = 0) out vec3 vs_pbr_camW;
layout(location = 1) out vec2 vs_pbr_tex0;
layout(location = 2) out vec3 vs_pbr_nrmW;
layout(location = 3) out vec4 vs_pbr_tanW;
#if SHDMAP > 0
layout(location = 4) out vec4 vs_pbr_shdH;
#endif
#endif

#if defined(_IO_PBRData) && defined(_FRAGMENT_SHADER)
layout(location = 0) in vec3 ps_pbr_camW;
layout(location = 1) in vec2 ps_pbr_tex0;
layout(location = 2) in vec3 ps_pbr_nrmW;
layout(location = 3) in vec4 ps_pbr_tanW;
#if SHDMAP > 0
layout(location = 4) in vec4 ps_pbr_shdH;
#endif
#endif


// ========================================================================================================================
// Vertex shader for physics based rendering
//
#if defined(_EP_PBR_VS)

// struct MESH_VERTEX (pMeshVertexDecl)
layout(location = 0) in vec3 posL;		// POSITION0
layout(location = 1) in vec3 nrmL;		// NORMAL0
layout(location = 2) in vec3 tanL;		// TANGENT0
layout(location = 3) in vec3 tex0;		// TEXCOORD0

void PBR_VS()
{
	vec3 posW = (gW * vec4(posL, 1.0f)).xyz;
	vec3 nrmW = (gW * vec4(nrmL, 0.0f)).xyz;

	vs_pbr_nrmW = nrmW;
	vs_pbr_tanW = vec4((gW * vec4(tanL, 0.0f)).xyz, tex0.z);
	gl_Position = gVP * vec4(posW, 1.0f);

#if SHDMAP > 0
	vs_pbr_shdH = gLVP * vec4(posW, 1.0f);
#endif

	vs_pbr_camW = -posW;
	vs_pbr_tex0 = tex0.xy;
}

#endif // _EP_PBR_VS


// ============================================================================
//
#if defined(_EP_PBR_PS)

// `float4 sc : VPOS` -- the pixel's window position, which is gl_FragCoord.
void PBR_PS()
{
	vec3 nrmT;
	vec3 nrmW;
	vec3 cEmis;
	vec3 cRefl, cRefl2, cRefl3;
	vec3 cFrsl = vec3(1.0f);
	vec4 cDiff;
	vec4 cSpec;
	vec4 sMask = vec4(1.0f, 1.0f, 1.0f, 1024.0f);
	float  fRghn;
	vec3 cDiffLocal;
	vec3 cSpecLocal;


	// ----------------------------------------------------------------------
	// Start fetching texture data
	// ----------------------------------------------------------------------

	if (gTextured) cDiff = texture(WrapS, ps_pbr_tex0.xy);
	else		   cDiff = vec4(1.0f);

	if (gOITEnable) if (cDiff.a < 0.5f) discard;

	// Fetch a normal map
	//
	if (gCfg.Norm) nrmT = texture(Nrm0S, ps_pbr_tex0.xy).rgb;


	// Sample specular map
	if (gCfg.Spec) cSpec = texture(SpecS, ps_pbr_tex0.xy).rgba * sMask;
	else 		   cSpec = gMtrl.specular.rgba;


	// Use _refl color for both
	if (gCfg.Refl) cRefl = texture(ReflS, ps_pbr_tex0.xy).rgb;
	else		   cRefl = gMtrl.reflect.rgb;


	// Roughness map
	if (gCfg.Rghn) fRghn = texture(RghnS, ps_pbr_tex0.xy).g;
	else		   fRghn = gMtrl.roughness.r;


	// Sample emission map. (Note: Emissive materials and textures need to go different stages, material is added to light)
	if (gCfg.Emis) cEmis = texture(EmisS, ps_pbr_tex0.xy).rgb;
	else		   cEmis = vec3(0.0f);



	// ----------------------------------------------------------------------
	// Now do other calculations while textures are being fetched
	// ----------------------------------------------------------------------

	vec3 CamD = normalize(ps_pbr_camW);
	vec3 cSun = clamp(gSun.Color, 0.0f, 1.0f);


	// ----------------------------------------------------------------------
	// Texture tuning controls for add-on developpers
	// ----------------------------------------------------------------------

#if defined(_DEBUG)
	if (gTuneEnabled) {

		nrmT *= gTune.Norm.rgb;

		cDiff.rgb = pow(abs(cDiff.rgb), vec3(gTune.Albe.a)) * gTune.Albe.rgb;
		cRefl.rgb = pow(abs(cRefl.rgb), vec3(gTune.Refl.a)) * gTune.Refl.rgb;
		cEmis.rgb = pow(abs(cEmis.rgb), vec3(gTune.Emis.a)) * gTune.Emis.rgb;
		fRghn = pow(abs(fRghn), gTune.Rghn.a) * gTune.Rghn.g;
		cSpec.rgba = cSpec.rgba * gTune.Spec.rgba;

		cDiff = clamp(cDiff, 0.0f, 1.0f);
		cRefl = clamp(cRefl, 0.0f, 1.0f);
		fRghn = clamp(fRghn, 0.0f, 1.0f);
		cSpec = min(cSpec, sMask);
	}
#endif


	// Use alpha zero to mask off specular reflections
	cSpec.rgb *= clamp(cSpec.a, 0.0f, 1.0f);

	// ----------------------------------------------------------------------
	// "Legacy/PBR" switch
	// ----------------------------------------------------------------------

	if (gPBRSw) {
		cRefl2 = cRefl*cRefl;
		cRefl3 = cRefl2*cRefl;
		cSpec.rgb = cRefl2;
		cSpec.a = exp2(fRghn * 12.0f);					// Compute specular power
	}
	else {
		cRefl3 = cRefl2 = cRefl;
	}

	float fRefl = cmax(cRefl3);


	// ----------------------------------------------------------------------
	// cSpec.pwr to fRghn Converter
	// ----------------------------------------------------------------------

	if (gRghnSw) {
		fRghn = log2(cSpec.a+1.0f) * 0.1f;
	}




	// ----------------------------------------------------------------------
	// Construct a proper world space normal
	// ----------------------------------------------------------------------

	if (gCfg.Norm) {
		vec3 bitW = cross(ps_pbr_tanW.xyz, ps_pbr_nrmW) * ps_pbr_tanW.w;
		nrmT.rg = nrmT.rg * 2.0f - 1.0f;
		nrmW = ps_pbr_nrmW*nrmT.z + ps_pbr_tanW.xyz*nrmT.x + bitW*nrmT.y;
	}
	else nrmW = ps_pbr_nrmW;

	nrmW = normalize(nrmW);



	// ----------------------------------------------------------------------
	// Compute reflection vector and some required dot products
	// ----------------------------------------------------------------------

	vec3 RflW = reflect(-CamD, nrmW);							// Reflection vector
	float dRS = clamp(-dot(RflW, gSun.Dir), 0.0f, 1.0f);		// Reflection/sun angle
	float dLN = clamp(-dot(gSun.Dir, nrmW), 0.0f, 1.0f);		// Diffuse lighting term
	float dLNx = clamp(dLN * 80.0f, 0.0f, 1.0f);				// Specular, Fresnel shadowing term


	// ----------------------------------------------------------------------
	// Add vessel self-shadows
	// ----------------------------------------------------------------------

#if SHDMAP > 0
	cSun *= smoothstep(0.0f, 0.72f, ComputeShadow(ps_pbr_shdH, dLN, gl_FragCoord));
#endif


	// ----------------------------------------------------------------------
	// Compute a fresnel terms fFrsl, iFrsl, fFLbe
	// ----------------------------------------------------------------------

	float fFrsl = 0.0f;	// Fresnel angle co-efficiency factor
	float iFrsl = 0.0f;	// Fresnel intensity
	float fFLbe = 0.0f;	// Fresnel lobe

#if defined(_GLASS)

	if (gFresnel) {

		float dCN = clamp(dot(CamD, nrmW), 0.0f, 1.0f);

		// Compute a fresnel term
		fFrsl = pow(1.0f - dCN, gMtrl.fresnel.x);

		// Compute a specular lobe for fresnel reflection
		fFLbe = pow(dRS, gMtrl.fresnel.z) * dLNx * float(any(notEqual(cRefl, vec3(0.0f))));

		// Modulate with material
		cFrsl *= gMtrl.fresnel.y;

		// Compute intensity term. Fresnel is always on a top of a multi-layer material
		// therefore it remains strong and attennuates other properties to maintain energy conservation using (1.0 - iFrsl)
		iFrsl = cmax(cFrsl) * fFrsl;
	}
#endif




	// ----------------------------------------------------------------------
	// Compute a specular and diffuse lighting
	// ----------------------------------------------------------------------

	// Compute a specular lobe for base material
	float fLobe = pow(dRS, cSpec.a) * dLNx;


	// ----------------------------------------------------------------------
	// Compute Local Light Sources
	// ----------------------------------------------------------------------

	LocalLightsEx(cDiffLocal, cSpecLocal, nrmW, -ps_pbr_camW, cSpec.a, false);


	// ----------------------------------------------------------------------
	// Compute Earth glow
	// ----------------------------------------------------------------------

	float angl = clamp((-dot(gCameraPos, nrmW) - gProxySize) * gInvProxySize, 0.0f, 1.0f);
	cDiffLocal += gAtmColor.rgb * max(0.0f, angl*gGlowConst);

	// Bake material props and lights together
	vec3 diffBaked = Light_fx(gMtrl.diffuse.rgb * (dLN * cSun + cDiffLocal) + gMtrl.emissive.rgb + gMtrl.ambient.rgb*gSun.Ambient);

#if LMODE > 0
	cSun = Light_fx(cSun + cSpecLocal);	// Add local light sources
#endif

	// Special alpha only texture in use, set the .rgb to 1.0f
	// Used for panel background lighting in Delta Glider
	if (gNoColor) cDiff.rgb = vec3(1.0f);

	// ------------------------------------------------------------------------
	cDiff.rgb *= diffBaked;				// Lit the texture
	cDiff.a *= gMtrlAlpha;				// Modulate material alpha


	// ------------------------------------------------------------------------
	// Compute total reflected sun light from a material
	//
	vec3 cBase = cSpec.rgb * (1.0f - iFrsl) * fLobe;

#if defined(_GLASS)
	cBase += cFrsl.rgb * fFrsl * fFLbe;
#endif

	cSpec.rgb = cSun * clamp(cBase, 0.0f, 1.0f);







	// ----------------------------------------------------------------------
	// Compute a environment reflections
	// ----------------------------------------------------------------------

	vec3 cEnv = vec3(0.0f);

#if defined(_ENVMAP)

	if (gEnvMapEnable) {

#if defined(_GLASS)

		if (gFresnel) {

			// Compute LOD level for fresnel reflection
			float fLODf = max(0.0f, (10.0f - log2(gMtrl.fresnel.z)));

			// Always mirror clear reflection for low angles
			fLODf *= (1.0f - fFrsl);

			// Fresnel based environment reflections
			cEnv = (cFrsl * fFrsl) * textureLod(EnvMapAS, RflW, fLODf).rgb;
		}
#endif

		// Compute LOD level for blur effect
		float fLOD = (1.0f - fRghn) * 8.0f;

		// Add a metallic reflections from a base material
		cEnv += cRefl3 * (1.0f-iFrsl) * textureLod(EnvMapAS, RflW, fLOD).rgb;
	}

#endif




	// ----------------------------------------------------------------------
	// Combine all results together
	// ----------------------------------------------------------------------

	// Compute total reflected light
	float fTot = cmax(cEnv + cSpec.rgb);

	// Attennuate diffuse surface beneath
	cDiff.rgb *= (1.0f - fTot);

#if defined(_ENVMAP)
	// Attennuate diffuse surface beneath
	cDiff.rgb *= (1.0f - fRefl);

#if defined(_GLASS)
	// Further attennuate diffuse surface beneath
	cDiff.rgb *= (1.0f - iFrsl*iFrsl);			// note: (1-iFrsl) goes black too quick
#endif
#endif

	// Re-compute output alpha for alpha blending stage
	//
	// PBR.fx:346 is `cDiff.a = saturate(cDiff.a + fTot);` and fTot is
	// `cmax(cEnv + cSpec.rgb)` -- an environment reflection PLUS the direct
	// sun lobe. Only the first of those belongs in the alpha: glass that
	// mirrors its surroundings does stop being see-through, but a specular
	// highlight is light ADDED on top of what is behind, and the line below
	// already adds it (`cDiff.rgb += cSpec.rgb`).
	//
	// It matters because a virtual cockpit is never self-shadowed --
	// Scene::RenderMainScene sets `smap.pShadowMap = NULL` before the
	// cockpit pass, so gShadowsEnabled is false there (measured) and nothing
	// attenuates a sun the hull should be blocking. The Delta-glider's HUD
	// combiner (deltaglider_vc group 130, material HUD_glass: diffuse alpha
	// 0.15, specular 0.851/0.859/0.784 with power 5) is tilted 26 deg up and
	// aft, so with the sun high and astern it mirrors the sun straight into
	// the pilot's eye: measured dLN 0.886, dRS 0.996, cEnv 0, cSpec.rgb 0.51,
	// which took the pane from its 15% glass to a blend alpha of 0.66 -- an
	// opaque green wall in front of the windscreen.
	//
	// THIS IS A DELIBERATE DEVIATION FROM THE REFERENCE, not a conversion of
	// it, and it is the only one in this shader. Everything else here is
	// PBR.fx statement for statement (verified mechanically: 130 of 130).
	// The rgb is untouched -- `cDiff.rgb *= (1 - fTot)` below still spends
	// the diffuse on the reflection, so the glass keeps its sheen.
	cDiff.a = clamp(cDiff.a + cmax(cEnv), 0.0f, 1.0f);

	// Add reflections to output
	cDiff.rgb += cEnv;

	// Add specular to output
	cDiff.rgb += cSpec.rgb;

	// Add emission texture to output, modulate with material
	cDiff.rgb = max(cDiff.rgb, cEmis * gMtrl.emission2.rgb);

#if defined(_DEBUG)
	//if (gDebugHL) cDiff = cDiff*0.5f + gColor;
	cDiff = cDiff * (vec4(1.0f) - gColor*0.5f) + gColor;
#endif

	cDiff.rgb *= gSun.Transmission;
	cDiff.rgb += gSun.Inscatter;

	oColor = cDiff;
}

#endif // _EP_PBR_PS


// ============================================================================
// Fast legacy Implementation no additional textures
// ============================================================================

// ----------------------------------------------------------------------------
// struct FASTData -- FAST_VS -> FAST_PS, XRHUD_PS
//
//     float4 posH : POSITION0;  -> gl_Position
//     float3 camW : TEXCOORD0;  -> location 0
//     float2 tex0 : TEXCOORD1;  -> location 1
//     float3 nrmW : TEXCOORD2;  -> location 2
//     float4 shdH : TEXCOORD4;  -> location 3   (#if SHDMAP > 0)
//
// shdH sits at TEXCOORD4 in the reference, skipping TEXCOORD3, because this
// struct is PBRData minus tanW and the author kept the numbering. A GLSL
// location is not a semantic index and nothing outside this file reads it, so
// it takes location 3 and the pair still agrees. Leaving a hole would cost a
// location in every pipeline built from this entry point.
// ----------------------------------------------------------------------------
#if defined(_EP_FAST_VS) || defined(_EP_FAST_PS) || defined(_EP_XRHUD_PS)
	#define _IO_FASTData 1
#endif

#if defined(_IO_FASTData) && defined(_VERTEX_SHADER)
layout(location = 0) out vec3 vs_fast_camW;
layout(location = 1) out vec2 vs_fast_tex0;
layout(location = 2) out vec3 vs_fast_nrmW;
#if SHDMAP > 0
layout(location = 3) out vec4 vs_fast_shdH;
#endif
#endif

#if defined(_IO_FASTData) && defined(_FRAGMENT_SHADER)
layout(location = 0) in vec3 ps_fast_camW;
layout(location = 1) in vec2 ps_fast_tex0;
layout(location = 2) in vec3 ps_fast_nrmW;
#if SHDMAP > 0
layout(location = 3) in vec4 ps_fast_shdH;
#endif
#endif


// ============================================================================
// Vertex shader for physics based rendering
//
#if defined(_EP_FAST_VS)

// struct MESH_VERTEX (pMeshVertexDecl). tanL is described by the layout and
// read by neither stage of this pass.
layout(location = 0) in vec3 posL;		// POSITION0
layout(location = 1) in vec3 nrmL;		// NORMAL0
layout(location = 3) in vec3 tex0;		// TEXCOORD0

void FAST_VS()
{
	vec3 posW = (gW * vec4(posL, 1.0f)).xyz;
	vec3 nrmW = (gW * vec4(nrmL, 0.0f)).xyz;

	vs_fast_nrmW = nrmW;
	gl_Position = gVP * vec4(posW, 1.0f);
	vs_fast_camW = -posW;
	vs_fast_tex0 = tex0.xy;

#if SHDMAP > 0
	vs_fast_shdH = gLVP * vec4(posW, 1.0f);
#endif
}

#endif // _EP_FAST_VS


// ============================================================================
//
#if defined(_EP_FAST_PS)

// `float4 sc : VPOS` -> gl_FragCoord
void FAST_PS()
{

	vec3 cEmis;
	vec4 cDiff;
	vec3 cDiffLocal;
	vec3 cSpecLocal;

	// Start fetching texture data -------------------------------------------
	//
	if (gTextured) cDiff = texture(WrapS, ps_fast_tex0.xy);
	else		   cDiff = vec4(1.0f);

	if (gOITEnable) if (cDiff.a < 0.5f) discard;

	if (gFullyLit) {
		if (gNoColor) cDiff.rgb = vec3(1.0f);
		cDiff.rgb *= clamp(gMtrl.diffuse.rgb + gMtrl.emissive.rgb, 0.0f, 1.0f);
	}
	else {

		// Sample emission map. (Note: Emissive materials and textures need to go different stages, material is added to light)
		if (gCfg.Emis) cEmis = texture(EmisS, ps_fast_tex0.xy).rgb;
		else		   cEmis = vec3(0.0f);

		vec3 nrmW  = normalize(ps_fast_nrmW);
		vec4 cSpec = gMtrl.specular.rgba;
		vec3 cSun  = clamp(gSun.Color, 0.0f, 1.0f);
		float  dLN   = clamp(-dot(gSun.Dir, nrmW), 0.0f, 1.0f);

		//cSpec.rgb *= 0.33333f;

		if (gNoColor) cDiff.rgb = vec3(1.0f);

		// ----------------------------------------------------------------------
		// Add vessel self-shadows
		// ----------------------------------------------------------------------

#if SHDMAP > 0
		float fShadow = smoothstep(0.0f, 0.72f, ComputeShadow(ps_fast_shdH, dLN, gl_FragCoord));
		dLN *= fShadow;
#endif

		// ----------------------------------------------------------------------
		// Compute Local Light Sources
		// ----------------------------------------------------------------------

		LocalLightsEx(cDiffLocal, cSpecLocal, nrmW, -ps_fast_camW, cSpec.a, false);


		// ----------------------------------------------------------------------
		// Compute Earth glow
		// ----------------------------------------------------------------------

		float angl = clamp((-dot(gCameraPos, nrmW) - gProxySize) * gInvProxySize, 0.0f, 1.0f);
		cDiffLocal += gAtmColor.rgb * max(0.0f, angl*gGlowConst);

		cDiff.rgb *= clamp( (gMtrl.diffuse.rgb*(dLN * cSun + cDiffLocal)) + (gMtrl.ambient.rgb*gSun.Ambient) + gMtrl.emissive.rgb, 0.0f, 1.0f );

		vec3 CamD = normalize(ps_fast_camW);
		vec3 HlfW = normalize(CamD - gSun.Dir);
		float  fSun = pow(clamp(dot(HlfW, nrmW), 0.0f, 1.0f), gMtrl.specular.a);

#if SHDMAP > 0
		fSun *= fShadow;
#endif


		if (dLN == 0.0f) fSun = 0.0f;

#if LMODE > 0
		vec3 specLight = clamp((fSun * cSun) + cSpecLocal, 0.0f, 1.0f);
#else
		vec3 specLight = (fSun * cSun);
#endif
		cDiff.rgb += (cSpec.rgb * specLight);

		cDiff.rgb += cEmis;
	}

#if defined(_DEBUG)
	//if (gDebugHL) cDiff = cDiff*0.5f + gColor;
	cDiff = cDiff * (vec4(1.0f) - gColor*0.5f) + gColor;
#endif

	cDiff.a *= gMtrlAlpha;

	cDiff.rgb *= gSun.Transmission;
	cDiff.rgb += gSun.Inscatter;

	oColor = cDiff;
}

#endif // _EP_FAST_PS


// ========================================================================================================================
//
#if defined(_EP_XRHUD_PS)
void XRHUD_PS()
{
	oColor = texture(WrapS, ps_fast_tex0.xy);
}
#endif


// ###########################################################################
// #include "Metalness.fx"    (from Vessel.fx -- "Must be included here")
// ###########################################################################

#define eps 0.001f

// ============================================================================
// Vertex shader for physics based rendering
//
#if defined(_EP_MetalnessVS)

// struct MESH_VERTEX (pMeshVertexDecl)
layout(location = 0) in vec3 posL;		// POSITION0
layout(location = 1) in vec3 nrmL;		// NORMAL0
layout(location = 2) in vec3 tanL;		// TANGENT0
layout(location = 3) in vec3 tex0;		// TEXCOORD0

void MetalnessVS()
{
	vec3 posW = (gW * vec4(posL, 1.0f)).xyz;
	vec3 nrmW = (gW * vec4(nrmL, 0.0f)).xyz;

#if SHDMAP > 0
	vs_pbr_shdH = gLVP * vec4(posW, 1.0f);
#endif

	vs_pbr_nrmW = nrmW;
	vs_pbr_tanW = vec4((gW * vec4(tanL, 0.0f)).xyz, tex0.z);
	gl_Position = gVP * vec4(posW, 1.0f);
	vs_pbr_camW = -posW;
	vs_pbr_tex0 = tex0.xy;
}

#endif // _EP_MetalnessVS


// ============================================================================
//
float BeckmanNDF(float dHN, float rgh)
{
	float r2 = rgh*rgh;
	float dHN2 = dHN*dHN;
	vec2 w = 1.0f / vec2(3.14f * r2 * dHN2*dHN2, r2*dHN2);
	return w.x * exp((dHN2 - 1.0f) * w.y);
}

// ============================================================================
//
float GGX_NDF(float dHN, float rgh)
{
	float r2 = rgh*rgh;
	float dHN2 = dHN*dHN;
	float d = (r2 * dHN2) + (1.0f - dHN2);
	return r2 / (3.14f * d * d);
}


// ============================================================================
//
float SchlickBeckmanGSF(float dLN, float dCN, float rgh) // pre-devided by dLN * dCN
{
	vec2 dots = clamp(vec2(dLN, dCN), vec2(eps), vec2(1.0f)); // Avoid div-by-zero
	float  r2 = rgh * rgh;
	vec2 e; e.xy = vec2(1.0f - r2);
	vec2 w = 1.0f / ((dots * e) + r2);
	return w.x*w.y;
}


// ============================================================================
//
float DiffuseRetroReflectance(float dLN, float dCN, float dLH, float rgh, float mtl)
{
	vec2 q = (1.0f-vec2(dLN, dCN));	q *= q*q;
	float z = 0.5f + 1.6f * dLH*dLH * rgh;
	return ((1.0f - q.x) + z*q.x) * ((1.0f - q.y) + z*q.y);
}


// ============================================================================
//
vec3 LightFX(vec3 c)
{
	float q = cmax(c);
	return c * inversesqrt(2.0f + q*q) * 1.8f;
}

// ============================================================================
//
vec3 LightFXSq(vec3 c)
{
	c = sqrt(c);
	float q = cmax(c);
	return c * inversesqrt(2.0f + q*q) * 1.8f;
}


// ============================================================================
//
void SampleEnvMap(out vec3 cE, float dCN, float fRgh, float fMetal, vec3 rflW, vec3 nrmW)
{
	// Sharpen reflection at low angles
	fRgh = clamp(fRgh - 0.1f, 0.0f, 1.0f);
	float fLOD = fRgh * mix(dCN * 2.0f, (0.2f + dCN*0.8f) * 2.5f, fMetal);	// Compute LOD level for blur effect

	fLOD *= 5.0f * inversesqrt(1.0f + fLOD*fLOD);

	cE = textureLod(EnvMapAS, rflW, fLOD).rgb;
}


// ============================================================================
//
void Transmittance(inout vec4 cDiff, float uLN, float uLC, vec2 uv, vec3 cSun)
{
	vec4 cTransm = vec4(cDiff.rgb, 1.0f);
	vec3 cTransl = cDiff.rgb;

	if (gCfg.Transm) {
		cTransm = texture(TransmS, uv);
		cTransm.a *= 1024.0f;
	}

	if (gCfg.Transl) cTransl = texture(TranslS, uv).rgb;

	float sunLightFromBehind = clamp(-uLN, 0.0f, 1.0f);
	float sunSpotFromBehind = clamp(pow(clamp(-uLC, 0.0f, 1.0f), cTransm.a) * 3.0, 0.0f, 1.0f); // "3.0" Causes the transmittance (sun spot) effect to fall off at very shallow angles

	cDiff.rgb += (1.0f - cDiff.rgb) * cTransl.rgb * clamp(cSun * sunLightFromBehind, 0.0f, 1.0f);
	cDiff.rgb += cTransm.rgb * (sunSpotFromBehind * cSun);
}


// ============================================================================
// A Shader for a typical "Metalness" PBR workflow.
// ============================================================================

#if defined(_EP_MetalnessPS)

// `float4 sc : VPOS` -> gl_FragCoord
void MetalnessPS()
{
	vec3 nrmT;
	vec3 nrmW;
	vec3 cEmis;
	vec4 cSpecularMap;																							// Added
	vec4 cDiff;
	float  fHeat;
	float  fSmth, fMetal;
	vec3 cDiffLocal;
	vec3 cSpecLocal;

	// ======================================================================
	// Start fetching texture data
	// ======================================================================

	if (gTextured) cDiff = texture(WrapS, ps_pbr_tex0.xy);
	else		   cDiff = vec4(1.0f);

	if (gOITEnable) if (cDiff.a < 0.5f) discard;

	// Fetch a normal map
	//
	if (gCfg.Norm) nrmT = texture(Nrm0S, ps_pbr_tex0.xy).rgb;

	// Fetch Smoothness map (i.e. *_rghn.dds)
	//
	if (gCfg.Rghn) fSmth = texture(RghnS, ps_pbr_tex0.xy).g;
	else		   fSmth = 1.0f;

	// Fetch Metalness map
	//
	if (gCfg.Metl) fMetal = texture(MetlS, ps_pbr_tex0.xy).g;
	else		   fMetal = gMtrl.metalness;

	// Sample emission map. (Note: Emissive materials and textures need to go different stages, material is added to light)
	//
	if (gCfg.Emis) cEmis = texture(EmisS, ps_pbr_tex0.xy).rgb;
	else		   cEmis = vec3(0.0f);

	// Sample specular map																										// Added
	//																															// Added
	if (gCfg.Spec) cSpecularMap = texture(SpecS, ps_pbr_tex0.xy).rgba;															// Added

	// Fetch Heat map
	//
	if (gCfg.Heat) fHeat = clamp((texture(HeatS, ps_pbr_tex0.xy).g) - 1.0f + (gMtrl.specialfx.x * gMtrl.specialfx.x * gMtrl.specialfx.x), 0.0f, 1.0f);
	else fHeat = (gMtrl.specialfx.x * gMtrl.specialfx.x * gMtrl.specialfx.x);

	// ----------------------------------------------------------------------
	// Now do other calculations while textures are being fetched
	// ----------------------------------------------------------------------

	vec3 camW = normalize(ps_pbr_camW);
	vec3 cSun = gSun.Color * mix(vec3(1.1, 1.1, 0.9), vec3(1,1,1), clamp(gRadius[3]*2e-5, 0.0f, 1.0f));


	// ======================================================================
	// Construct a proper world space normal
	// ======================================================================

	vec3 tanW = ps_pbr_tanW.xyz;
	vec3 bitW = cross(tanW, ps_pbr_nrmW) * ps_pbr_tanW.w;

	if (gCfg.Norm) {
		nrmT.rg = nrmT.rg * 2.0f - 1.0f;
		nrmW = ps_pbr_nrmW*nrmT.z + tanW*nrmT.x + bitW*nrmT.y;
	}
	else nrmW = ps_pbr_nrmW;

	nrmW = normalize(nrmW);



	// ======================================================================
	// Typical compatibility requirements
	// ======================================================================

	if (gNoColor) cDiff.rgb = vec3(1.0f);
	cDiff = clamp(cDiff * vec4(gMtrl.diffuse.rgb, gMtrlAlpha), 0.0f, 1.0f);


	// ======================================================================
	// Some Precomputations
	// ======================================================================

	vec3 sunW = -gSun.Dir;
	vec3 cEnv = vec3(0.0f);

	vec3 rflW = reflect(-camW, nrmW);
	vec3 hlvW = normalize(camW + sunW);

	// Dot Products
	float uLN = dot(sunW, nrmW);
	float uLC = dot(sunW, camW);
	float dLN = clamp(uLN, 0.0f, 1.0f);
	float dLH = clamp(dot(sunW, hlvW), 0.0f, 1.0f);
	float dCN = clamp(dot(camW, nrmW), 0.0f, 1.0f);
	float dHN = clamp(dot(hlvW, nrmW), 0.0f, 1.0f);

	// Apply a proper curve to a texture data, modulate with material value and clamp
	fSmth = pow(abs(fSmth), gMtrl.roughness.y) * gMtrl.roughness.x;

	// Apply fresnel and Fresnell cut-off to fSmth
	fSmth = fSmth + ((1.0f - fSmth) * pow(abs(1.0f - dCN), 4.0f)) * pow(abs(fSmth), 0.5f);

	float fRgh = clamp(1.0f - fSmth, 0.0f, 1.0f);
	float fRgh3 = fRgh*fRgh*fRgh;


	// ======================================================================
	// Compute Local Light Sources
	// ======================================================================

	LocalLightsEx(cDiffLocal, cSpecLocal, nrmW, -ps_pbr_camW, fRgh3, true);


#if defined(_ENVMAP)

	if (gEnvMapEnable) {

		// ======================================================================
		// Sample Env Map
		SampleEnvMap(cEnv, dCN, fRgh, fMetal, rflW, nrmW);
	}

#if defined(_IRRADIANCE)
	// ======================================================================
	// Sample Irradiance Map
	vec3 cAmbient = Paraboloidal_LVLH(IrradS, nrmW).rgb;
	cAmbient *= cAmbient;

	//cAmbient = saturate(cAmbient * (1.0f + 15.0f * gNightTime));
	// Apply base ambient light
	cAmbient = max(cAmbient, gSun.Ambient);
#endif
#endif

#if !defined(_ENVMAP) || !defined(_IRRADIANCE)
	// ======================================================================
	// Compute Earth glow
	float angl = clamp((-dot(gCameraPos, nrmW) - gProxySize) * gInvProxySize, 0.0f, 1.0f);
	vec3 cAmbient = gAtmColor.rgb * max(0.0f, angl * gGlowConst) + gSun.Ambient;
#endif


	cAmbient *= (1.0f - fMetal); // No ambient for metals


	// ======================================================================
	// Add vessel self-shadows
	// ======================================================================
#if SHDMAP > 0
	cSun *= smoothstep(0.0f, 0.72f, ComputeShadow(ps_pbr_shdH, dLN, gl_FragCoord));
#endif


	// ======================================================================
	// Main shader core MetalnessPS
	// ======================================================================
	float  fD = GGX_NDF(dHN, mix(0.01f, 1.0f, fRgh3));
	float  fG = SchlickBeckmanGSF(dLN, dCN, fRgh);
	float  fR = DiffuseRetroReflectance(dLN, dCN, dLH, fRgh, fMetal);

	vec3 cS2 = vec3(0.0f);																									// Added
	vec3 cSpec2 = vec3(0.0f);																								// Added

	// Base material color for reflections. Use cDiff for metals and very rough plastics, white for the rest.
	// cDiff for rough plastics is to avoid washed-out(white) look of black and rough parts.
	vec3 cSpec = mix(cDiff.rgb, vec3(1, 1, 1), (1.0f - fMetal) * (1.0f - fRgh3));

	// Fresnel power 2.5 for glossy, 5.0 for rough
	float fFrs = pow(1.0f - dCN, fRgh*2.5 + 2.5f);

	// Fresnel cut-off below X of fSmth
	fFrs *= clamp(0.3f - fRgh*fRgh, 0.0f, 1.0f) * 3.3f;

	// Assume that plastics absorve 50-90% of specular light
	float  fP = mix(0.1f + (1.0f - fRgh)*0.4f, 1.0f, fMetal);


	// ======================================================================												// Start of Added section
	// Add multilayer texture effect
	// ======================================================================
	if (gCfg.Spec) {
		cSpec2 = cSpecularMap.rgb;
		cSpecularMap.a = 1.0f - cSpecularMap.a; // use this
		// cSpecularMap.a = 0.0f;
		float fD2 = GGX_NDF(dHN, mix(0.01f, 1.0f, cSpecularMap.a)); // makes the sun glint larger at full smoothness so that it doesn't disappear

		// Specular Color
		cS2 = (fD2 * cSpec2 * fP) * cSpecularMap.a ; // (4.0f*dLN*dCN) removed to avoid division by zero, compensation in GSF
		// cS2 = (fD2 * cSpec2 * fG * fP) * 0.25f; // (4.0f*dLN*dCN) removed to avoid division by zero, compensation in GSF
	}																														// end of Added section

	// Fresnel color shift
	vec3 cF = cSpec + (1.0f - cSpec) * fFrs;

	// Specular Color
	vec3 cS = (fD * cF * fG * fP) * 0.25f; // (4.0f*dLN*dCN) removed to avoid division by zero, compensation in GSF


	// How plastics reflect the environment
	float  R = 0.1f * fSmth;
	float  frP = R + (1.0f - R) * fFrs;

	vec3 cE = (cEnv * cF * mix(frP, 1.0f, fMetal));

	// Attennuate diffuse color for Metals & Fresnel
	float  fA = (1.0f - fFrs) * (1.0f - fMetal);

	// Add a faint diffuse hue for rough metals. Rough metal doesn't look good if it's totally black
	fA += fRgh * fMetal * 0.05f;

	vec3 zD = cDiff.rgb * fA * LightFXSq(Sq(cSun * fR * dLN) + cDiffLocal + Sq(cAmbient) + Sq(gMtrl.emissive.rgb));

	// Combine specular terms
	// float3 zS = cS * (cSun * dLN) + cSpec * LightFX(cSpecLocal) * 0.5f;
	vec3 zS = cS * (cSun * dLN) + cSpec * LightFX(cSpecLocal) * 0.5f  + cS2 * (cSun * dLN) + cSpec2 * LightFX(cSpecLocal) * 0.5f;		// Modified

	cDiff.rgb = zD + zS + cE;

	// Override material alpha to make reflections visible
	cDiff.a = clamp(cDiff.a + cmax(zS + cE), 0.0f, 1.0f);


	// ======================================================================
	// Add texture transmittance
	// ======================================================================

	if (gCfg.Transm || gCfg.Transl) Transmittance(cDiff, uLN, uLC, ps_pbr_tex0.xy, cSun);



	// Add emission texture to output, modulate with material
	cDiff.rgb = max(cDiff.rgb, cEmis * gMtrl.emission2.rgb);

	// Add heat glow
	//float3 cHeat = float3(pow(abs(fHeat), 0.5f), pow(abs(fHeat), 1.5f), pow(abs(fHeat), 8.0f));
	vec3 cHeat = pow(vec3(abs(fHeat)), vec3(0.5f, 1.5f, 8.0f));
	cDiff.rgb = cDiff.rgb + cHeat;

#if defined(_DEBUG)
	cDiff = cDiff * (vec4(1.0f) - gColor*0.5f) + gColor;
#endif

#if defined(_LIGHTGLOW)
	cDiff.rgb *= gSun.Transmission;
	cDiff.rgb += gSun.Inscatter;
	oColor = cDiff;
	return;
#else
	vec3 h2 = cDiff.rgb * cDiff.rgb;
	cDiff.rgb *= pow(max(vec3(0.0f), 1.0f + h2 * h2), vec3(-0.25));

	cDiff.rgb *= gSun.Transmission;
	cDiff.rgb += gSun.Inscatter;

	oColor = cDiff;
	return;
#endif
}

#endif // _EP_MetalnessPS


// ###########################################################################
// Vessel.fx, continued -- its own two entry points, after the four #includes
// ###########################################################################

// ============================================================================
// Vertex shader for physics based rendering
//
#if defined(_EP_AdvancedVS)

// struct MESH_VERTEX (pMeshVertexDecl)
layout(location = 0) in vec3 posL;		// POSITION0
layout(location = 1) in vec3 nrmL;		// NORMAL0
layout(location = 2) in vec3 tanL;		// TANGENT0
layout(location = 3) in vec3 tex0;		// TEXCOORD0

void AdvancedVS()
{
	vec3 posW = (gW * vec4(posL, 1.0f)).xyz;
	vec3 nrmW = (gW * vec4(nrmL, 0.0f)).xyz;

#if SHDMAP > 0
	vs_pbr_shdH = gLVP * vec4(posW, 1.0f);
#endif

	vs_pbr_nrmW = nrmW;
	vs_pbr_tanW = vec4((gW * vec4(tanL, 0.0f)).xyz, tex0.z);
	gl_Position = gVP * vec4(posW, 1.0f);
	vs_pbr_camW = -posW;
	vs_pbr_tex0 = tex0.xy;
}

#endif // _EP_AdvancedVS



// ============================================================================
//
#if defined(_EP_AdvancedPS)

// `float4 sc : VPOS` -> gl_FragCoord
void AdvancedPS()
{
	vec3 bitW;
	vec3 nrmT;
	vec3 cRefl;
	vec3 cEmis;
	vec4 cSpec;
	vec4 cTex;

	vec3 cDiffLocal;
	vec3 cSpecLocal;

	if (gTextured) cTex = texture(WrapS, ps_pbr_tex0.xy);
	else		   cTex = vec4(1.0f);

	if (gOITEnable) if (cTex.a < 0.5f) discard;

	if (gCfg.Norm) nrmT  = texture(Nrm0S, ps_pbr_tex0.xy).rgb;

	if (gCfg.Spec) cSpec = texture(SpecS, ps_pbr_tex0.xy);
	else		   cSpec = gMtrl.specular;

	if (gCfg.Refl) cRefl = texture(ReflS, ps_pbr_tex0.xy).rgb;
	else		   cRefl = gMtrl.reflect.rgb;

	// Sample emission map. (Note: Emissive materials and textures need to go different stages, material is added to light)
	if (gCfg.Emis) cEmis = texture(EmisS, ps_pbr_tex0.xy).rgb;
	else		   cEmis = vec3(0.0f);


	vec3 nrmW = ps_pbr_nrmW;
	vec3 tanW = ps_pbr_tanW.xyz;
	vec3 cSun = clamp(gSun.Color, 0.0f, 1.0f);
	vec3 CamD = normalize(ps_pbr_camW);
	vec3 Base = (gMtrl.ambient.rgb*gSun.Ambient) + (gMtrl.emissive.rgb);


	// Compute World space normal -------------------------------------------
	//
	if (gCfg.Norm) {
		nrmT = nrmT * 2.0 - 1.0;
		bitW = cross(tanW, nrmW) * ps_pbr_tanW.w;
		nrmW = nrmW*nrmT.z + tanW*nrmT.x + bitW*nrmT.y;
	}

	nrmW  = normalize(nrmW);

	vec3 TnrmW = -nrmW;
	vec3 RflW  = reflect(-CamD, nrmW);
	float  dLN   = clamp(-dot(gSun.Dir, nrmW), 0.0f, 1.0f);

	if (gCfg.Spec) cSpec.a *= 255.0f;

	// Approximate roughness
	float fRghn = log2(cSpec.a) * 0.1f;

	// Sunlight calculation
	float fSun = pow(clamp(-dot(RflW, gSun.Dir), 0.0f, 1.0f), cSpec.a) * clamp(cSpec.a, 0.0f, 1.0f);

	if (dLN == 0.0f) fSun = 0.0f;

	// Special alpha only texture in use
	if (gNoColor) cTex.rgb = vec3(1.0f);


	// ----------------------------------------------------------------------
	// Add vessel self-shadows
	// ----------------------------------------------------------------------

#if SHDMAP > 0
	cSun.rgb *= ComputeShadow(ps_pbr_shdH, dLN, gl_FragCoord);
#endif



	// ----------------------------------------------------------------------
	// Compute Local Light Sources
	// ----------------------------------------------------------------------

	LocalLightsEx(cDiffLocal, cSpecLocal, nrmW, -ps_pbr_camW, cSpec.a, false);


	// Lit the diffuse texture
	cTex.rgb *= clamp(Base + gMtrl.diffuse.rgb * Light_fx(cDiffLocal + cSun * dLN), 0.0f, 1.0f);

	// Lit the specular surface
	cSpec.rgb *= clamp(cSpecLocal + fSun * cSun, 0.0f, 1.0f);


	// Compute Transluciency effect --------------------------------------------------------------
	//

	if (gCfg.Transm || gCfg.Transl) {

		vec4 cTransm = vec4(cTex.rgb, 1.0f);

		if (gCfg.Transm) {
			cTransm = texture(TransmS, ps_pbr_tex0.xy);
			cTransm.a *= 1024.0f;
		}

		vec3 cTransl = cTex.rgb;

		if (gCfg.Transl) {
			cTransl = texture(TranslS, ps_pbr_tex0.xy).rgb;
		}

		// Texture Tuning -------------------------------------------------------
		//
		if (gTuneEnabled) {
			cTransm *= gTune.Transm.rgba;
			cTransl *= gTune.Transl.rgb;
		}

		float sunLightFromBehind = clamp(dot(gSun.Dir, nrmW), 0.0f, 1.0f);
		float sunSpotFromBehind = pow(clamp(dot(gSun.Dir, CamD), 0.0f, 1.0f), cTransm.a);
		sunSpotFromBehind *= clamp(sunLightFromBehind * 3.0f, 0.0f, 1.0f);// Causes the transmittance (sun spot) effect to fall off at very shallow angles

		cTransl.rgb *= clamp(cSun * sunLightFromBehind, 0.0f, 1.0f);

		cTex.rgb += (1.0f - cTex.rgb) * cTransl.rgb;
		cTex.rgb += cTransm.rgb * (sunSpotFromBehind * cSun);
	}


	float fFrsl = 1.0f;
	float fInt = 0.0f;

	// Compute reflectivity
	float fRefl = cmax(cRefl);


#if defined(_ENVMAP)


	// Compute environment map/fresnel effects --------------------------------
	//
	if (gEnvMapEnable) {

		// Do we need fresnel code for this render pass ?

		if (gFresnel) {

			fFrsl = gMtrl.fresnel.y;

			// Get mirror reflection for fresnel
			vec3 cEnvFres = textureLod(EnvMapAS, RflW, 0.0f).rgb;

			float  dCN = clamp(dot(CamD, nrmW), 0.0f, 1.0f);

			// Compute a fresnel term with compensations included
			fFrsl *= pow(1.0f - dCN, gMtrl.fresnel.x) * (1.0 - fRefl) * float(any(notEqual(cRefl, vec3(0.0f))));

			// Sunlight reflection for fresnel material
			cSpec.rgb = clamp(cSpec.rgb + fSun * fFrsl * cSun, 0.0f, 1.0f);

			// Compute total reflected light with fresnel reflection
			// and accummulate in cSpec
			cSpec.rgb = clamp(cSpec.rgb + fFrsl * cEnvFres, 0.0f, 1.0f);

			// Compute intensity
			fInt = clamp(dot(cSpec.rgb, cLuminosity), 0.0f, 1.0f);

			// Attennuate diffuse surface
			cTex.rgb *= (1.0f - fInt);
		}

		// Compute LOD level for blur effect
		float fLOD = (1.0f - fRghn) * 10.0f;

		vec3 cEnv = textureLod(EnvMapAS, RflW, fLOD).rgb;

		// Compute total reflected light, accummulate in cSpec
		cSpec.rgb += cRefl.rgb * cEnv;
	}

#endif

	// Attennuate diffuse surface
	cTex.rgb *= (1.0f - fRefl);

	// Re-compute output alpha for alpha blending stage
	// NOTE: Without fresnel fInt remains zero
	cTex.a = clamp(cTex.a + fInt, 0.0f, 1.0f);

	// Add reflections to output
	cTex.rgb += cSpec.rgb;

	// Add emissive textures to output
	cTex.rgb += cEmis;

#if defined(_DEBUG)
	//if (gDebugHL) cTex = cTex*0.5f + gColor;
	cTex = cTex * (vec4(1.0f) - gColor*0.5f) + gColor;
#endif

	cTex.rgb *= gSun.Transmission;
	cTex.rgb += gSun.Inscatter;

	oColor = cTex;
}

#endif // _EP_AdvancedPS


// ###########################################################################
// #include "HorizonHaze.fx"
// ###########################################################################

#if defined(_EP_HazeTechVS)

// struct HZVERTEX (pHazeVertexDecl)
//
//     float3 posL  : POSITION0;  location 0, offset 0
//     float4 color : COLOR0;     location 1, offset 12, D3DCOLOR
//     float2 tex0  : TEXCOORD0;  location 2, offset 16
//
// The colour is a D3DCOLOR -- one DWORD, 0xAARRGGBB -- which the vertex
// declaration reads as VK_FORMAT_B8G8R8A8_UNORM, so it arrives here already
// normalised and in RGBA order. Same value the D3D9 vertex shader saw.
layout(location = 0) in vec3 posL;		// POSITION0
layout(location = 1) in vec4 color;		// COLOR0
layout(location = 2) in vec2 tex0;		// TEXCOORD0

void HazeTechVS()
{
	vec3 posW = (gW * vec4(posL, 1.0f)).xyz;
	gl_Position = gVP * vec4(posW, 1.0f);
	vs_hz_tex0  = tex0;
	vs_hz_color = color;
}

#endif // _EP_HazeTechVS


// Horizon haze pixel-shader frg.tex0.y is the altitude. 0.0 = Horizon (ground level) 1.0 = top of atmosphere
//
#if defined(_EP_HazeTechPS)
void HazeTechPS()
{
	oColor = ps_hz_color * texture(ClampS, ps_hz_tex0);

	//return float4(frg.color.rgb, frg.color.a*frg.tex0.y*frg.tex0.y);
	//return float4(frg.color.rgb*(frg.tex0.y+0.30), frg.color.a*frg.tex0.y*frg.tex0.y);
}
#endif


// ###########################################################################
// #include "Planet.fx"
// ###########################################################################

// ----------------------------------------------------------------------------
// struct TileVS -- PlanetTechVS -> PlanetTechPS, CloudTechPS
//
//     float4 posH    : POSITION0;  -> gl_Position
//     float2 tex0    : TEXCOORD0;  -> location 0
//     float3 normalW : TEXCOORD1;  -> location 1
//     float3 toCamW  : TEXCOORD2;  -> location 2   Vector to the camera
//     float3 posW    : TEXCOORD3;  -> location 3   World space vertex position
//     float4 aux     : TEXCOORD4;  -> location 4   Specular, Diffuse, Twilight, Night Texture Intensity,
//     float4 diffuse : TEXCOORD5;  -> location 5   Sun light
//     float4 atten   : COLOR0;     -> location 6   Attennuate incoming fragment color
//     float4 insca   : COLOR1;     -> location 7   "Inscatter" Add to incoming fragment color
// ----------------------------------------------------------------------------
#if defined(_EP_PlanetTechVS) || defined(_EP_PlanetTechPS) || defined(_EP_CloudTechPS)
	#define _IO_TileVS 1
#endif

#if defined(_IO_TileVS) && defined(_VERTEX_SHADER)
layout(location = 0) out vec2 vs_tv_tex0;
layout(location = 1) out vec3 vs_tv_normalW;
layout(location = 2) out vec3 vs_tv_toCamW;
layout(location = 3) out vec3 vs_tv_posW;
layout(location = 4) out vec4 vs_tv_aux;
layout(location = 5) out vec4 vs_tv_diffuse;
layout(location = 6) out vec4 vs_tv_atten;
layout(location = 7) out vec4 vs_tv_insca;
#endif

#if defined(_IO_TileVS) && defined(_FRAGMENT_SHADER)
layout(location = 0) in vec2 ps_tv_tex0;
layout(location = 1) in vec3 ps_tv_normalW;
layout(location = 2) in vec3 ps_tv_toCamW;
layout(location = 3) in vec3 ps_tv_posW;
layout(location = 4) in vec4 ps_tv_aux;
layout(location = 5) in vec4 ps_tv_diffuse;
layout(location = 6) in vec4 ps_tv_atten;
layout(location = 7) in vec4 ps_tv_insca;
#endif


// ----------------------------------------------------------------------------
// struct ShadowVS (Planet.fx) -- CloudShadowTechVS -> CloudShadowPS
//
//     float4 posH  : POSITION0;  -> gl_Position
//     float2 tex0  : TEXCOORD0;  -> location 0
//     float4 atten : TEXCOORD2;  -> location 1
//
// atten sits at TEXCOORD2 in the reference, skipping TEXCOORD1. A GLSL
// location is not a semantic index and nothing outside this file reads it,
// so the pair uses 0 and 1 and the hole costs nothing.
// ----------------------------------------------------------------------------
#if defined(_EP_CloudShadowTechVS) || defined(_EP_CloudShadowPS)
	#define _IO_PlanetShadowVS 1
#endif

#if defined(_IO_PlanetShadowVS) && defined(_VERTEX_SHADER)
layout(location = 0) out vec2 vs_cs_tex0;
layout(location = 1) out vec4 vs_cs_atten;
#endif

#if defined(_IO_PlanetShadowVS) && defined(_FRAGMENT_SHADER)
layout(location = 0) in vec2 ps_cs_tex0;
layout(location = 1) in vec4 ps_cs_atten;
#endif


#if defined(_EP_PlanetTechVS)

// struct TILEVERTEX (pPatchVertexDecl). elev is described by the layout and
// read by neither stage.
layout(location = 0) in vec3 posL;		// POSITION0
layout(location = 1) in vec3 normalL;	// NORMAL0
layout(location = 2) in vec2 tex0;		// TEXCOORD0

void PlanetTechVS()
{
	// Apply a mesh group transformation matrix
	vec3 posW = (gW * vec4(posL, 1.0f)).xyz;
	vec3 nrmW = normalize((gW * vec4(normalL, 0.0f)).xyz);

	// Convert transformed vertex position into a "screen" space using a combined (World, View and Projection) Matrix
	gl_Position = gVP * vec4(posW, 1.0f);

	// A vector from the vertex to the camera
	vec3 tocam  = normalize(-posW);
	vec3 sundir = gSun.Dir;

	float diff    = clamp(dot(-sundir, nrmW), 0.0f, 1.0f);
	float dotr    = max(dot(reflect(sundir, nrmW), tocam), 0.0f);
	float spec    = pow(diff,0.25f) * pow(dotr, gWater.specPower);
	float nigh    = 0.0f;
	float ambi    = 0.0f;

	vs_tv_tex0    = vec2(tex0.x*gTexOff[0] + gTexOff[1], tex0.y*gTexOff[2] + gTexOff[3]);
	vs_tv_toCamW  = tocam;
	vs_tv_normalW = nrmW;
	vs_tv_posW    = gCameraPos*gRadius[2] + posW*gDistScale;

	LegacySunColor(vs_tv_diffuse, ambi, nigh, nrmW);

	vs_tv_aux     = vec4(spec, diff, ambi, nigh);

	AtmosphericHaze(vs_tv_atten, vs_tv_insca, gl_Position.z, posW);

	vs_tv_insca *= (vs_tv_diffuse+ambi);
}

#endif // _EP_PlanetTechVS



#if defined(_EP_PlanetTechPS)
void PlanetTechPS()
{

	vec4 diff  = ps_tv_aux.g*(gMat.diffuse*ps_tv_diffuse) + (gMat.ambient*ps_tv_aux.b);
	vec4 vSpe = ps_tv_aux.r * (gWater.specular*ps_tv_diffuse);
	vec4 vEff = texture(Planet1S, ps_tv_tex0);

	if (gSpecMode==2) vSpe *= 1.0f - vEff.a;
	if (gSpecMode==0) vSpe = vec4(0.0f);

	vec3 cTex = texture(Planet0S, ps_tv_tex0).rgb;
	vec3 color = diff.rgb * cTex.rgb + ps_tv_aux.a*vEff.rgb + vSpe.rgb;

	oColor = vec4(color*ps_tv_atten.rgb+gColor.rgb+ps_tv_insca.rgb, 1.0f);
}
#endif



#if defined(_EP_CloudTechPS)
void CloudTechPS()
{

	vec4 data  = (gMat.ambient*ps_tv_aux.b);
	vec4 color = texture(Planet0S, ps_tv_tex0);
	float  alpha = color.a;

	if (dot(ps_tv_normalW, ps_tv_toCamW)<0.0f) {    // Render cloud layer from below
		vec4 diff = (min(1.0f,ps_tv_aux.g*2.0f) * ps_tv_diffuse) * gMat.diffuse + data;
		oColor = vec4(color.rgb*diff.rgb, alpha);
	}

	else { // Render cloud layer from above
		vec4 diff = (min(1.0f,ps_tv_aux.g*1.5f) * ps_tv_diffuse) * gMat.diffuse + data;
		oColor = vec4(color.rgb*diff.rgb, alpha);
	}
}
#endif









// -----------------------------------------------------------------------------
// Cloud Shadow Techs
// -----------------------------------------------------------------------------

#if defined(_EP_CloudShadowTechVS)

// struct TILEVERTEX (pPatchVertexDecl)
layout(location = 0) in vec3 posL;		// POSITION0
layout(location = 2) in vec2 tex0;		// TEXCOORD0

void CloudShadowTechVS()
{
	vec3 posW = (gW * vec4(posL, 1.0f)).xyz;
	gl_Position = gVP * vec4(posW, 1.0f);
	vs_cs_tex0  = vec2(tex0.x*gTexOff[0] + gTexOff[1], tex0.y*gTexOff[2] + gTexOff[3]);

	vec4 none;

	AtmosphericHaze(vs_cs_atten, none, gl_Position.z, posW);
}

#endif // _EP_CloudShadowTechVS


#if defined(_EP_CloudShadowPS)
void CloudShadowPS()
{
	oColor = vec4(0,0,0, texture(Planet0S, ps_cs_tex0).a * ps_cs_atten.b);
}
#endif


// ###########################################################################
// #include "BeaconArray.fx"
// ###########################################################################

// ----------------------------------------------------------------------------
// struct BeaconVS -- BeaconArrayVS -> BeaconArrayPS
//
//     float4 posH : POSITION0;  -> gl_Position
//     float4 colr : COLOR0;     -> location 0
//     float2 tex0 : TEXCOORD0;  -> gl_PointCoord, see below
//     float  size : PSIZE;      -> gl_PointSize
//     float  haze : COLOR1;     -> location 1
//
// tex0 IS NOT AN INTERPOLANT HERE, AND IT WAS NOT REALLY ONE THERE EITHER.
// BeaconArrayVS never writes it: the pass sets D3DRS_POINTSPRITEENABLE, and
// D3D9 then REPLACED the texture coordinate of a point's four generated
// vertices with the sprite's own 0..1 coordinates. Vulkan has no such state
// -- a point is one fragment unless the vertex shader writes gl_PointSize --
// and the sprite coordinate is the always-available gl_PointCoord, so the
// interpolant disappears and the pixel shader reads gl_PointCoord instead.
// See VulkanEffect.cpp's note on PointSpriteEnable.
// ----------------------------------------------------------------------------
#if defined(_EP_BeaconArrayVS) || defined(_EP_BeaconArrayPS)
	#define _IO_BeaconVS 1
#endif

#if defined(_IO_BeaconVS) && defined(_VERTEX_SHADER)
layout(location = 0) out vec4 vs_ba_colr;
layout(location = 1) out float vs_ba_haze;
#endif

#if defined(_IO_BeaconVS) && defined(_FRAGMENT_SHADER)
layout(location = 0) in vec4 ps_ba_colr;
layout(location = 1) in float ps_ba_haze;
#endif


#if defined(_EP_BeaconArrayVS)

// struct BAVERTEX (pBAVertexDecl)
//
//     float3 posL  : POSITION0;  location 0, offset 0
//     float3 dirL  : NORMAL0;    location 1, offset 12
//     float4 data  : TEXCOORD0;  location 2, offset 24
//     float2 dataB : TEXCOORD1;  location 3, offset 40
//     float4 colr  : COLOR0;     location 4, offset 48, D3DCOLOR
layout(location = 0) in vec3 posL;		// POSITION0
layout(location = 1) in vec3 dirL;		// NORMAL0
layout(location = 2) in vec4 data;		// TEXCOORD0
layout(location = 3) in vec2 dataB;		// TEXCOORD1
layout(location = 4) in vec4 colr;		// COLOR0

void BeaconArrayVS()
{
	//float3 posX   = mul(float4(vrt.posL, 1.0f), gW).xyz;
	//float dist    = length(posX);	// distance to a beacon
	//float3 offL   = float3(0, dist*2e-3, 0);
	//float3 posW   = mul(float4(vrt.posL+offL, 1.0f), gW).xyz;

	vec3 posW   = (gW * vec4(posL, 1.0f)).xyz;
	vec3 dirW   = (gW * vec4(dirL, 0.0f)).xyz;
	gl_Position = gVP * vec4(posW, 1.0f);

	//posW = posW * gDistScale;

	float fog     =  exp(-abs(gl_Position.z * gFogDensity * 20.0f)); // Haze effect scale factor

	float dist    = length(posW);	// distance to a beacon

	// Viewing angle dependency
	float dota    = -dot(normalize(posW), dirW);
	float angl    = clamp((dota - data[1])/(1.0f-data[1]), 0.0f, 1.0f); // beacon visibility from a current point 1.0 to 0.0
	float disp    = pow(angl, dataB[1]); // apply a proper curve into a visibility

	// Distance attennuation
	float att0    = dataB[0] / (1.0f+dist*2e-4);
	float att1    = clamp(pow(att0, 2.0f), 0.0f, 1.0f);

	if (gTime<data[2] || gTime>data[3]) disp = 0.0f;

	vs_ba_colr    = vec4(colr.rgb, min(1.0f, colr.a * disp * att1 * 1.2f));
	// `outVS.size : PSIZE` -- the point's size in pixels. D3D9 took it from
	// the interpolant; Vulkan takes it from gl_PointSize, which a vertex
	// shader feeding a POINT_LIST pipeline must write itself.
	gl_PointSize  = (5.5f + data[0] * gPointScale / dist) * disp;
	vs_ba_haze    = clamp(fog*1.8f, 0.3f, 1.8f);
}

#endif // _EP_BeaconArrayVS


#if defined(_EP_BeaconArrayPS)
void BeaconArrayPS()
{
	// `tex2D(ClampS, frg.tex0)` -- the point sprite's generated coordinate.
	// See the note on struct BeaconVS above.
	vec4 cTex = texture(ClampS, gl_PointCoord);
	//return float4(frg.colr.rgb*saturate(pow(abs(cTex.a),0.3f)*gMix), pow(abs(cTex.a), frg.haze) * frg.colr.a);
	oColor = vec4(ps_ba_colr.rgb, pow(abs(cTex.a), ps_ba_haze) * ps_ba_colr.a);
}
#endif


// ###########################################################################
// D3D9Client.fx, continued -- its last two entry points, after the #includes
// ###########################################################################

#if defined(_EP_ArrowTechVS)

// `float3 posL : POSITION0` (pPositionDecl)
layout(location = 0) in vec3 posL;		// POSITION0

void ArrowTechVS()
{
	vec3 posW = (gW * vec4(posL, 1.0f)).xyz;	// Apply world transformation matrix
	gl_Position = gVP * vec4(posW, 1.0f);		// Apply view projection matrix
	vs_bs_dstW = vec2(0.0f);					// `(BShadowVS)0`
	vs_bs_alpha = 0.0f;
}

#endif // _EP_ArrowTechVS


#if defined(_EP_ArrowTechPS)
void ArrowTechPS()
{
	oColor = gColor;
}
#endif
