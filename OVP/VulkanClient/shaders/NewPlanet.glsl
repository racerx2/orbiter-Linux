// ============================================================================
// NewPlanet.glsl -- converted from OVP/D3D9Client/shaders/NewPlanet.hlsl
//                   and, through its #include, Scatter.hlsl
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// licensed under LGPL v2
// Copyright (C) 2022-2026 Jarmo Nikkanen
// ============================================================================
//
// The seven general conventions are in IPI.glsl. This is a ShaderClass file --
// vPlanetAtmo.cpp builds six PlanetShaders from it and HazeMgr.cpp two more --
// so it follows CelSphere.glsl's binding convention: the VERTEX stage's
// uniform block at binding 0, the PIXEL stage's at binding 1, samplers after
// them.
//
//  A. `#include "Scatter.hlsl"` IS INLINED, because GLSL has no #include.
//     Scatter's declarations and helper functions are reproduced here in its
//     own order, ahead of this file's own code, exactly where the #include
//     sits. They are identical to Scatter.glsl's; that file is the same
//     source compiled as an ImageProcessing effect.
//
//     TWO THINGS FROM Scatter ARE DELIBERATELY NOT INLINED:
//
//       Its SIX ENTRY POINTS -- SunColor, SkyView, RingView, AmbientSky,
//       LandView, LandViewAtten -- which no PlanetShader ever compiles. In
//       HLSL they came in as dead functions and cost nothing. Here they would
//       bring their `float u : TEXCOORD0, float v : TEXCOORD1` interpolants,
//       which are declared at locations 0 and 1 and would collide with every
//       interpolant set in this file.
//
//       Its `Flo` UNIFORM AND ITS BLOCK PLACEMENT. `Flo` itself IS declared
//       below, in Scatter's order at the head of each block, because dropping
//       it would move `Prm` and everything after it. What changes is that
//       Scatter's block is a PIXEL block and this file needs `Const` in BOTH
//       stages -- HorizonVS, TerrainVS, CloudVS and GiantVS all read
//       Const.mVP -- so the declarations appear in each stage's own block.
//
//  B. TWO STAGES, TWO BLOCKS, AND A DIFFERENT TAIL ON EACH. Both begin with
//     Scatter's `Const` and `Flo`, then this file's `Prm`; after that the
//     vertex block has `FlowVS` and the pixel block has `Flow`, `Lights` and
//     `Spotlight`, which is exactly which stage reads which. Every member is
//     written by name at the offset reflection reports -- PlanetShader's
//     constructor takes eleven handles -- so the two tails need not agree.
//
//  C. `tex1D(tEclipse, x)` BECOMES A 2D FETCH AT v = 0. vPlanet::GlobalInit
//     creates the eclipse table as `CreateTexture(512, 1, 1, R32_SFLOAT)` --
//     a 512x1 TWO-dimensional image, on Windows as here -- and D3D9's tex1D
//     on such a texture is sugar for exactly that fetch. Declaring a
//     sampler1D would not match the image the client binds.
//
//  D. THE LOCAL `cNrm` INSIDE TerrainPS's microtexture block SHADOWS the
//     function-scope `cNrm` declared for the water ripples. That is the
//     reference's own naming and both languages allow it; the inner one is
//     `cMicroNrm` here so the two are distinguishable, and nothing else
//     changes.
// ============================================================================

#version 450
#extension GL_EXT_scalar_block_layout : require


// ############################################################################
// #include "Scatter.hlsl"
// ############################################################################

#if defined(_PERFORMANCE) // DO NOT CHANGE THESE, MUST MATCH WITH C++ CODE
#define Nc  6		//Z-dimension count in 3D texture
#define Wc  72		//3D texture size (pixels)
#define Qc  64		//2D texture size (pixels)
#else
#define Nc  8		//Z-dimension count in 3D texture
#define Wc  128		//3D texture size (pixels)
#define Qc  96		//2D texture size (pixels)
#endif


#define NSEG 5
#define iNSEG 1.0f / NSEG
#define MINANGLE -0.33f			// Minumum angle
#define ANGRNG (0.25f - MINANGLE)
#define iANGRNG (1.0f / ANGRNG)
#define LastLine (0.999999f - 1.0f / Qc)


// Per-Frame Params
//
// The mirror of vPlanet.h's ConstParams -- 432 bytes, asserted there.
//
struct AtmoParams
{
	mat4 mVP;				// View Projection Matrix
	vec3 CamPos;				// Geocentric Camera position
	vec3 toCam;				// Geocentric Camera direction (unit vector)
	vec3 toSun;				// Geocentric Sun direction (unit vector)
	vec3 SunAz;				// Atmo scatter ref.frame (unit vector) (toCam, ZeroAz, SunAz)
	vec3 ZeroAz;				// Atmo scatter ref.frame (unit vector)
	vec3 Up;					// Sun/Shadow Ref Frame (Unit Vector) (Up, toSun, ZeroAz)
	vec3 vTangent;			// Reference frame for normal mapping (Unit Vector)
	vec3 vBiTangent;			// Reference frame for normal mapping (Unit Vector)
	vec3 vPolarAxis;			// North Pole (unit vector)
	vec3 cSun;				// Sun Color and intensity
	vec3 RayWave;				// .rgb Rayleigh Wave lenghts
	vec3 MieWave;				// .rgb Mie Wave lenghts
	vec4 HG;					// Henyey-Greenstein Phase function params
	vec2 iH;					// Inverse scale height for ray(.r) and mie(.g) e.g. exp(-altitude * iH)
	vec2 rmO;					// Ray and Mie out-scatter factors
	vec2 rmI;					// Ray and Mie in-scatter factors
	vec3 cAmbient;			// Ambient light color at sealevel
	vec3 cGlare;				// Sun glare color
	float  PlanetRad;			// Planet Radius
	float  PlanetRad2;			// Planet Radius Squared
	float  AtmoAlt;				// Atmospehere upper altitude limit
	float  AtmoRad;				// Atmospehere outer radius
	float  AtmoRad2;			// Atmospehere outer radius squared
	float  CloudAlt;			// Cloud layer altitude for color and light calculations (not for phisical rendering)
	float  MinAlt;				// Minimum terrain altitude
	float  MaxAlt;				// Maximum terrain altitude
	float  iAltRng;				// 1.0 / (MaxAlt - MinAlt);
	float  AngMin;
	float  AngRng;
	float  iAngRng;
	float  AngCtr;				// Cos of View cone angle from planet center that's visible from camera location
	float  HrzDst;				// Distance to horizon (500 m) minimum if camera below sea level
	float  CamAlt;				// Camera Altitude
	float  CamElev;				// Camera Elevation above surface
	float  CamRad;				// Camera geo-distance
	float  CamRad2;				// Camera geo-distance squared
	float  Expo;				// "HDR" exposure factor (atmosphere only)
	float  Time;				// Simulation time / 180
	float  TrGamma;				// Terrain "Gamma" correction setting
	float  TrExpo;				// "HDR" exposure factor (terrain only)
	float  Ambient;				// Global ambient light level
	float  Clouds;				// Cloud layer intensity (if below), and Blue light inscatter scale factor (if camera Above clouds)
	float  TW_Terrain;			// Twilight intensity
	float  TW_Dst;				// Twilight distance behind terminator
	float  CosAlpha;			// Cosine of camera horizon angle i.e. PlanetRad/CamRad
	float  SinAlpha;			// Sine of ^^
	float  CamSpace;			// Camera in space scale factor 0.0 = surf, 1.0 = space
	float  Cr2;					// Camera radius on shadow plane (dot(cp.toCam, cp.Up) * cp.CamRad)^2
	float  ShdDst;
	float  SunVis;
	float  dCS;
	float  smi;
	float  ecc;
	float  trLS;
	float  wNrmStr;				// Water normal strength
	float  wSpec;				// Water smoothness
	float  wBrightness;
	float  wBoost;
};

// The mirror of vPlanet.h's sFlow -- 12 bytes, asserted there.
struct sFlow {
	bool bRay;					// True for rayleigh render pass
	bool bCamLit;				// True if camera is lit by sunlight
	bool bCamInSpace;			// True if camera is in space (i.e. not in atmosphere)
};


// ############################################################################
// NewPlanet.hlsl's own declarations
// ############################################################################

// The mirror of Surfmgr2.cpp's LightF -- 256 bytes: four float3 arrays of
// four (48 each, scalar stride 12) and one float4 array of four (64).
struct _Light
{
	vec3   position[4];         /* position in world space */
	vec3   direction[4];        /* direction in world space */
	vec3   diffuse[4];          /* diffuse color of light */
	vec3   attenuation[4];      /* Attenuation */
	vec4   param[4];            /* range, falloff, theta, phi */
};

#define Range   0
#define Falloff 1
#define Theta   2
#define Phi     3

#define ATMNOISE 0.25
#define GLARE_SIZE 5	// Larger value -> smaller


// Note: "bool" is 32-bits in a shaders (max count 16)
//
// The mirror of vPlanet.h's FlowControlPS -- 52 bytes, asserted there.
struct FlowControlPS
{
	bool bInSpace;				// Camera in space (not in atmosphere)
	bool bBelowClouds;			// Camera is below cloud layer
	bool bOverlay;				// Overlay on/off
	bool bShadows;				// Shadow Map on/off
	bool bLocals;				// Local Lights on/off
	bool bMicroNormals;			// Micro texture has normals
	bool bCloudShd;				// Cloud shadow textures valid and enabled
	bool bMask;					// Nightlights/water mask texture is enabled
	bool bRipples;				// Water riples texture is enabled
	bool bMicroTex;				// Micro textures exists and enabled
	bool bPlanetShadow;			// Use spherical approximation for shadow
	bool bEclipse;				// Eclipse is occuring
	bool bTexture;				// Surface texture exists
};

// The mirror of vPlanet.h's FlowControlVS -- 12 bytes, asserted there.
struct FlowControlVS
{
	bool bInSpace;				// Camera in space (not in atmosphere)
	bool bSpherical;			// Ignore elevation, render as sphere
	bool bElevOvrl;				// ElevOverlay on/off
};

// The mirror of vPlanet.h's ShaderParams -- 348 bytes, asserted there.
struct PerObjectParams
{
	mat4 mWorld;			// World Matrix
	mat4 mLVP;				// Light-View-Projection
	vec4   vSHD;				// Shadow Map Parameters
	vec4   vMSc[3];			// Micro Texture offset-scale
	vec4	 vTexOff;			// Texture offset-scale
	vec4   vCloudOff;			// Cloud texture offset-scale
	vec4   vMicroOff;			// Micro texture offset-scale
	vec4   vOverlayOff;       // Overlay texture offset-scale
	vec4   vOverlayCtrl[4];
	vec3	 vEclipse;			// Eclipse caster position (geocentric)
	float	 fEclipse;			// Eclipse data addressing scale factor. (to access tExlipse)
	float	 fAlpha;
	float	 fBeta;
	float	 fTgtScale;
};


// ----------------------------------------------------------------------------
// The two blocks. See note B in the header for why the tails differ.
// ----------------------------------------------------------------------------
#ifdef _VERTEX_SHADER
layout(set = 0, binding = 0, scalar) uniform NewPlanetVSBlock
{
	AtmoParams Const;
	sFlow Flo;
	PerObjectParams Prm;
	FlowControlVS FlowVS;
};
#endif

#ifdef _FRAGMENT_SHADER
layout(set = 0, binding = 1, scalar) uniform NewPlanetPSBlock
{
	AtmoParams Const;
	sFlow Flo;
	PerObjectParams Prm;
	FlowControlPS Flow;
	_Light Lights;			// Note: DX9 doesn't tolerate structure arrays outside FX framework
	bool Spotlight[4];
};

// Scatter.hlsl's nine samplers, in its order, at bindings 2..10.
layout(set = 0, binding =  2) uniform sampler2D tSun;
layout(set = 0, binding =  3) uniform sampler2D tCam;
layout(set = 0, binding =  4) uniform sampler2D tLndRay;
layout(set = 0, binding =  5) uniform sampler2D tLndMie;
layout(set = 0, binding =  6) uniform sampler2D tLndAtn;
layout(set = 0, binding =  7) uniform sampler2D tSunGlare;
layout(set = 0, binding =  8) uniform sampler2D tAmbient;
layout(set = 0, binding =  9) uniform sampler2D tSkyRayColor;
layout(set = 0, binding = 10) uniform sampler2D tSkyMieColor;

// NewPlanet.hlsl's seventeen, in its order, at bindings 11..27.
layout(set = 0, binding = 11) uniform sampler2D tDiff;				// Diffuse texture
layout(set = 0, binding = 12) uniform sampler2D tMask;				// Nightlights / Specular mask texture
layout(set = 0, binding = 13) uniform sampler2D tCloud;				// 1st Cloud shadow texture
layout(set = 0, binding = 14) uniform sampler2D tCloud2;			// 2nd Cloud shadow texture
layout(set = 0, binding = 15) uniform sampler2D tCloudMicro;
layout(set = 0, binding = 16) uniform sampler2D tCloudMicroNorm;
layout(set = 0, binding = 17) uniform sampler2D tNoise;				//
layout(set = 0, binding = 18) uniform sampler2D tOcean;				// Ocean Normal Map Texture
layout(set = 0, binding = 19) uniform sampler2D tMicroA;
layout(set = 0, binding = 20) uniform sampler2D tMicroB;
layout(set = 0, binding = 21) uniform sampler2D tMicroC;
layout(set = 0, binding = 22) uniform sampler2D tGlare;
layout(set = 0, binding = 23) uniform sampler2D tShadowMap;
layout(set = 0, binding = 24) uniform sampler2D tOverlay;
layout(set = 0, binding = 25) uniform sampler2D tMskOverlay;
layout(set = 0, binding = 26) uniform sampler2D tElvOverlay;
layout(set = 0, binding = 27) uniform sampler2D tEclipse;			// 512x1 2D image; see note C

layout(location = 0) out vec4 oColor;
#endif

// tElvOverlay is sampled by the VERTEX stage (TerrainVS's elevation overlay),
// which D3D9 did too -- vertex texture fetch on D3DVERTEXTEXTURESAMPLER0. Its
// binding has to stay clear of the pixel stage's, so it keeps the number it
// has above; ShaderClass builds one descriptor set for both stages and reads
// the bindings back from reflection.
#ifdef _VERTEX_SHADER
layout(set = 0, binding = 26) uniform sampler2D tElvOverlay;
#endif


#ifdef _FRAGMENT_SHADER

// ############################################################################
// Scatter.hlsl's helper functions, inlined. Identical to Scatter.glsl's.
// ############################################################################

// `static const float n[]` / `w[]` -- GLSL needs the size and a constructor.
#if NSEG == 5
const float n[NSEG] = float[NSEG]( 0.050, 0.25, 0.50, 0.75, 0.950 );
const float w[NSEG] = float[NSEG]( 0.125, 0.25, 0.25, 0.25, 0.125 );
#endif

#if NSEG == 7
const float n[NSEG] = float[NSEG]( 0.05, 0.167, 0.333, 0.500, 0.667, 0.833, 0.95 );
const float w[NSEG] = float[NSEG]( 0.08, 0.167, 0.167, 0.167, 0.167, 0.167, 0.08 );
#endif

// Gauss7 points and weights
const vec4 n0 = vec4(0.0714, 0.21428, 0.35714, 0.5 );
const vec4 w0 = vec4(0.1295, 0.27971, 0.38183, 0.41796);
const vec4 n1 = vec4(0.64285, 0.78571, 0.92857, 0 );
const vec4 w1 = vec4(0.38183, 0.27971, 0.1295, 0 );

// Gauss4 points and weights
const vec4 n4 = vec4( 0.06943, 0.33001, 0.66999, 0.93057 );
const vec4 w4 = vec4( 0.34786, 0.65215, 0.65215, 0.34786 );

float ilerp(float a, float b, float x)
{
	return clamp((x - a) / (b - a), 0.0f, 1.0f);
}

vec3 sqr(vec3 x)
{
	return x * x;
}

vec4 expc(vec4 x) { return exp(clamp(x, vec4(-20.0f), vec4(20.0f))); }
vec3 expc(vec3 x) { return exp(clamp(x, vec3(-20.0f), vec3(20.0f))); }
vec2 expc(vec2 x) { return exp(clamp(x, vec2(-20.0f), vec2(20.0f))); }
float expc(float x)  { return exp(clamp(x, -20.0f, 20.0f)); }


vec2 NrmToUV(vec3 vNrm)
{
	vec2 uv = vec2(dot(Const.ZeroAz, vNrm), dot(Const.SunAz, vNrm)) / Const.AngCtr;
	return (uv * 0.5 + 0.5);
}


vec3 uvToLoc(vec2 uv, float r)
{
	uv = (uv * 2.0 - 1.0);
	float w2 = uv.x * uv.x + uv.y * uv.y;
	if (w2 > 1.0f) uv *= inversesqrt(w2);
	uv *= r;
	float det = 1.0f - uv.x * uv.x - uv.y * uv.y;
	float cos_ab = det > 0.0f ? sqrt(det) : 0.0f;
	return vec3(uv.xy, cos_ab);
}


vec3 uvToNrm(vec2 uv)
{
	vec3 q = uvToLoc(uv, Const.AngCtr);
	return normalize(Const.SunAz * q.y + Const.ZeroAz * q.x + Const.toCam * q.z);
}


vec3 uvToDir(vec2 uv)
{
	uv.y = uv.y * inversesqrt(0.2f + uv.y * uv.y) * sqrt(1.2f);

	float x = uv.x * 2.0f - 1.0f;
	float y = sqrt(1.0f - x * x);
	float z = 1.0f - uv.y * Const.AngRng;
	float k = sqrt(1.0f - z * z);

	return normalize(Const.SunAz * x * k + Const.ZeroAz * y * k + Const.toCam * z);
}


vec2 DirToUV(vec3 uDir)
{
	float y = dot(uDir, Const.toCam);
	vec3 uOrt = normalize(uDir - Const.toCam * y);
	float x = dot(uOrt, Const.SunAz) * 0.5 + 0.5;
	vec2 uv = vec2(x, 1.0f - (y - Const.AngMin) * Const.iAngRng);
	uv.y = sqrt(0.2f) * uv.y * inversesqrt(1.2f - uv.y * uv.y);
	return uv;
}


vec3 HDR(vec3 x)
{
	return 1.0f - exp(-Const.Expo * x);
}


vec3 LightFX(vec3 x)
{
	//return x * rsqrt(1.0f + x*x) * 1.4f;
	return 2.0 * x / (1.0f + x);
}


float RayLength(float cos_dir, float r0, float r1)
{
	float y = r0 * cos_dir;
	float z2 = r0 * r0 - y * y;
	return sqrt(r1 * r1 - z2) + y;
}


float RayLength(float cos_dir, float r0)
{
	return RayLength(cos_dir, r0, Const.AtmoRad);
}


// Compute UV and blend factor for smaple3D routine
//
vec3 TransformUV(vec3 uv, float rc, float prc)
{
	float ipix = 1.0f / rc;

	uv = clamp(uv, 0.0f, 1.0f);
	uv.z *= (rc - 1.0f);
	uv.x *= ipix;
	uv.x += floor(uv.z) * ipix;
	return uv;
}


// Sample a 3D texture composed from an array of 2D textures
//
vec4 smaple3D(sampler2D tSamp, vec3 uv, float rc, float pix)
{
	float x = 1.0f / rc;

	vec4 a = texture(tSamp, uv.xy).rgba;
	vec4 b = texture(tSamp, uv.xy + vec2(x, 0)).rgba;
	return mix(a, b, fract(uv.z));
}


// Optical depth integral in atmosphere for a given distance
//
vec2 Gauss7(float cos_dir, float r0, float dist, vec2 ih0)
{
	int i;
	float x = 2.0 * r0 * cos_dir;
	float r2 = r0 * r0;

	// Compute altitudes of sample points
	vec4 d0 = dist * n0;
	vec4 a0 = sqrt(r2 + d0 * (d0 - x)) - Const.PlanetRad;
	vec4 d1 = dist * n1;
	vec4 a1 = sqrt(r2 + d1 * (d1 - x)) - Const.PlanetRad;

	vec2 sum = vec2(0.0f);
	for (i = 0; i < 4; i++) sum += expc(-a0[i] * ih0) * w0[i];
	for (i = 0; i < 3; i++) sum += expc(-a1[i] * ih0) * w1[i];
	return sum * dist * 0.5f;
}


// Optical depth integral in atmosphere for a given distance
//
vec2 Gauss4(float cos_dir, float r0, float dist, vec2 ih0)
{
	vec4 d0 = dist * n4;
	vec4 a0 = sqrt(r0 * r0 + d0 * d0 - 2.0 * r0 * d0 * cos_dir) - Const.PlanetRad;
	vec4 ray = expc(-a0 * ih0.x);
	vec4 mie = expc(-a0 * ih0.y);
	return vec2(dot(ray, w4), dot(mie, w4)) * dist * 0.5f;
}


// Rayleigh phase function
//
float RayPhase(float cw)
{
	return 0.25f * (4.0f + cw * cw);
}


// Henyey-Greenstein Phase function
//
float MiePhase(float cw)
{
	return 8.0f * Const.HG.x / (1.0f - Const.HG.y * cw) + Const.HG.w;
}


// Get a color of sunlight for a given altitude and normal-sun angle
//
vec3 GetSunColor(float dir, float alt)
{
	float maxalt = max(Const.MaxAlt, Const.CloudAlt);
	alt = ilerp(Const.MinAlt, maxalt, alt);
	dir = clamp((dir - MINANGLE) * iANGRNG, 0.0f, 1.0f);
	alt = sqrt(alt);
	return texture(tSun, vec2(dir, alt)).rgb;
}


vec3 ComputeCameraView(float a, float r, float d)
{
	vec2 rm = Gauss7(a, r, d, Const.iH) * Const.rmO;
	vec3 clr = Const.RayWave * rm.r + Const.MieWave * rm.g;
	return exp(-clr);
}


// Approximate multi-scatter effect to atmospheric color and light travel behind terminator
//
// GLSL has no default arguments; the `= true` forms become wrappers.
vec4 AmbientApprox(float dNS, bool bR)
{
	float fA = 1.0f - smoothstep(0.0f, Const.TW_Dst, -dNS);
	vec3 clr = (bR ? Const.RayWave : Const.cAmbient);
	return vec4(clr, fA);
}

vec4 AmbientApprox(vec3 vNrm, bool bR)
{
	float dNS = dot(vNrm, Const.toSun);
	return AmbientApprox(dNS, bR);
}

vec4 AmbientApprox(float dNS)  { return AmbientApprox(dNS, true); }
vec4 AmbientApprox(vec3 vNrm)  { return AmbientApprox(vNrm, true); }


struct RayData {
	float se;	// Distance to 'Shadow entry' point from a camera
	float sx;	// Shadow exit
	float ae;	// Atmosphere entry
	float ax;	// Atmosphere exit
	float hd;	// Horizon distance from a camera
	float ca;	// Closest approach distance
};

struct IData {
	float s0, s1, e0, e1;
};


// Compute ray passage information
// vRay must point away from the camera
//
RayData ComputeRayStats(in vec3 vRay, bool bPreProcessData)
{
	RayData dat;

	const float invalid = -1e9;

	// Projection of viewing ray on 'shadow' axes
	float u = dot(vRay, Const.Up);
	float t = dot(vRay, Const.ZeroAz);
	float z = dot(vRay, Const.toSun);

	// Shadow Entry and Exit points
	// Cosine 'a'
	float a = u * inversesqrt(u * u + t * t);

	float k2 = Const.Cr2 * a * a;
	float h2 = Const.Cr2 - k2;
	float w2 = Const.PlanetRad2 - h2;
	vec2 b = sqrt(vec2(w2, k2));
	float k  = b.y * sign(a);
	float v2 = 0.0f;
	float m  = Const.CamRad2 - Const.PlanetRad2;

	dat.se = k - b.x;
	dat.sx = dat.se + 2.0f * b.x;

	// Project distances back to 3D space
	float q = inversesqrt(max(2.5e-5, 1.0 - z * z));
	dat.se *= q;
	dat.sx *= q;

	// Compute atmosphere entry and exit points
	//
	a = -dot(Const.toCam, vRay);
	k2 = Const.CamRad2 * a * a;
	h2 = Const.CamRad2 - k2;
	v2 = Const.AtmoRad2 - h2;
	vec3 nv = sqrt(vec3(v2, k2, m));
	k = nv.y * sign(a);

	dat.hd  = m > 0.0 ? nv.z : 0.0;
	dat.ae = (k - nv.x);
	dat.ax = dat.ae + 2.0f * nv.x;
	dat.ca = Const.CamRad * a;

	// If the ray doesn't intersect atmosphere then set both distances to zero
	if (v2 < 0.0f) dat.ae = dat.ax = invalid;

	// If the ray doesn't intersect shadow then set both distances to atmo exit
	if (w2 < 0.0f) dat.se = dat.sx = invalid;

	if (bPreProcessData)
	{
		vec3 vEn = Const.CamPos + vRay * dat.se;
		vec3 vEx = Const.CamPos + vRay * dat.sx;

		// If shadow entry/exit point is Lit then set it to atmo exit point
		if (dot(vEn, Const.toSun) > 0.0f) dat.se = invalid;
		if (dot(vEx, Const.toSun) > 0.0f) dat.sx = invalid;
	}

	return dat;
}


IData PostProcessData(RayData sp)
{
	IData d;
	if (!Flo.bCamLit) {	// Camera in Shadow
		d.s0 = max(sp.sx, sp.ae);

		float lf = max(0.0f, sp.ca) / max(1.0f, abs(sp.hd)); // Lerp Factor
		float mp = mix((sp.ax + d.s0) * 0.5f, sp.hd, clamp(lf, 0.0f, 1.0f));

		d.e0 = mp;
		d.s1 = max(sp.sx, mp);
		d.e1 = sp.ax;
	}
	else { // Camera is Lit
		d.s0 = max(0.0f, sp.ae);

		float lf = max(0.0f, sp.ca) / max(1.0f, abs(sp.hd)); // Lerp Factor
		float mp = mix((sp.ax + d.s0) * 0.5f, sp.hd, clamp(lf, 0.0f, 1.0f));

		bool bA = (sp.se > sp.ax || sp.se < 0.0f);

		d.e0 = bA ? mp : sp.se;
		d.s1 = bA ? mp : max(sp.sx, sp.ae);
		d.e1 = sp.ax;
	}
	return d;
}

// Compute attennuation from vPos in atmosphere to camera (or atm exit point)
//
vec3 ComputeCameraView(vec3 vPos, vec3 vNrm, vec3 vRay, float r)
{
	float d;
	float a = dot(vNrm, vRay);
	if (Flo.bCamInSpace) d = RayLength(a, r);
	else d = dot(vPos - Const.CamPos, vRay);
	vec2 rm = Gauss7(a, r, d, Const.iH) * Const.rmO;
	vec3 clr = Const.RayWave * rm.r + Const.MieWave * rm.g;
	return exp(-clr);
}

// Integrate viewing ray for incatter color (.rgb) and optical depth (.a)
// vRay must point from camera to vOrig
//
vec4 IntegrateSegmentMP(vec3 vOrig, vec3 vRay, float len, float iH)
{
	vec4 ret = vec4(0.0f);
	vec3 vR = vRay * len;
	for (int i = 0; i < NSEG; i++)
	{
		vec3 pos = vOrig + vR * (iNSEG * (float(i) + 0.5f));
		vec3 nn = normalize(pos);
		float rad = dot(nn, pos);
		float alt = rad - Const.PlanetRad;
		vec3 x = GetSunColor(dot(nn, Const.toSun), alt);
		x *= ComputeCameraView(pos, nn, vRay, rad);
		float f = exp(-alt * iH) * iNSEG;
		ret.rgb += x * f;
		ret.a += f;
	}
	return ret * len;
}

vec4 IntegrateSegmentNS(vec3 vOrig, vec3 vRay, float len, float iH)
{
	// TODO: Could try to accummulation of CamView seg. by seg.

	vec4 ret = vec4(0.0f);
	vec3 vR = vRay * len;
	for (int i = 0; i < NSEG; i++)
	{
		vec3 pos = vOrig + vR * n[i];
		vec3 nn = normalize(pos);
		float rad = dot(nn, pos);
		float alt = rad - Const.PlanetRad;
		vec3 x = GetSunColor(dot(nn, Const.toSun), alt);
		x *= ComputeCameraView(pos, nn, vRay, rad);
		float f = exp(-alt * iH) * w[i];
		ret.rgb += x * f;
		ret.a += f;
	}
	return ret * len;
}


struct SkyOut
{
	vec4 ray;
	vec4 mie;
};


// Get a precomputed rayleight and mie color values for a given direction from a camera
//
SkyOut GetSkyColor(vec3 uDir)
{
	vec2 uv = DirToUV(uDir);

	SkyOut o;
	o.ray = texture(tSkyRayColor, uv).rgba;
	o.mie = texture(tSkyMieColor, uv).rgba;
	return o;
}


// Get a precomputed total (combined) sky color for a given direction from a camera
//
vec3 GetAmbient(vec3 vRay)
{
	return texture(tAmbient, DirToUV(vRay)).rgb;
}


struct LandOut
{
	vec4 ray;
	vec4 mie;
	vec4 atn;
};


// Get a precomputed rayleight and mie haze values for a given altitude and normal
//
LandOut GetLandView(float rad, vec3 vNrm)
{
	vec2 uv = NrmToUV(vNrm);

	float a = rad - Const.PlanetRad;
	float z = clamp((a - Const.MinAlt) * Const.iAltRng, 0.0f, 1.0f); // inverse lerp

	vec3 uvb = TransformUV(vec3(uv, z), Nc, Wc);

	LandOut o;
	o.ray = smaple3D(tLndRay, uvb, Nc, Wc);
	o.mie = smaple3D(tLndMie, uvb, Nc, Wc);
	o.atn = smaple3D(tLndAtn, uvb, Nc, Wc);
	return o;
}

#endif // _FRAGMENT_SHADER


// ############################################################################
// NewPlanet.hlsl's own code
// ############################################################################

// ----------------------------------------------------------------------------
// Vertex Shader to Pixel Shader datafeeds
// ----------------------------------------------------------------------------

// struct TileVS -- TerrainVS / GiantVS -> TerrainPS / GiantPS
//
//     float4 posH  : POSITION0;  -> gl_Position
//     float2 texUV : TEXCOORD0;  -> location 0   Texture coordinate
//     float4 camW  : TEXCOORD1;  -> location 1   Radius in .w
//     float3 nrmW  : TEXCOORD2;  -> location 2
//     float4 shdH  : TEXCOORD3;  -> location 3   (#if defined(_SHDMAP))
#if defined(_EP_TerrainVS) || defined(_EP_GiantVS) || \
	defined(_EP_TerrainPS) || defined(_EP_GiantPS)
	#define _IO_TileVS 1
#endif

#if defined(_IO_TileVS) && defined(_VERTEX_SHADER)
layout(location = 0) out vec2 vs_tv_texUV;
layout(location = 1) out vec4 vs_tv_camW;
layout(location = 2) out vec3 vs_tv_nrmW;
#if defined(_SHDMAP)
layout(location = 3) out vec4 vs_tv_shdH;
#endif
#endif

#if defined(_IO_TileVS) && defined(_FRAGMENT_SHADER)
layout(location = 0) in vec2 ps_tv_texUV;
layout(location = 1) in vec4 ps_tv_camW;
layout(location = 2) in vec3 ps_tv_nrmW;
#if defined(_SHDMAP)
layout(location = 3) in vec4 ps_tv_shdH;
#endif
#endif


// struct CldVS -- CloudVS -> CloudPS / GiantCloudPS
//
//     float4 posH  : POSITION0;  -> gl_Position
//     float2 texUV : TEXCOORD0;  -> location 0   Texture coordinate
//     float3 nrmW  : TEXCOORD1;  -> location 1
//     float3 posW  : TEXCOORD2;  -> location 2
#if defined(_EP_CloudVS) || defined(_EP_CloudPS) || defined(_EP_GiantCloudPS)
	#define _IO_CldVS 1
#endif

#if defined(_IO_CldVS) && defined(_VERTEX_SHADER)
layout(location = 0) out vec2 vs_cv_texUV;
layout(location = 1) out vec3 vs_cv_nrmW;
layout(location = 2) out vec3 vs_cv_posW;
#endif

#if defined(_IO_CldVS) && defined(_FRAGMENT_SHADER)
layout(location = 0) in vec2 ps_cv_texUV;
layout(location = 1) in vec3 ps_cv_nrmW;
layout(location = 2) in vec3 ps_cv_posW;
#endif


// struct HazeVS -- HorizonVS -> HorizonPS / HorizonRingPS
//
//     float4 posH  : POSITION0;  -> gl_Position
//     float2 texUV : TEXCOORD0;  -> location 0
//     float3 posW  : TEXCOORD1;  -> location 1
//     float  alpha : COLOR0;     -> location 2
#if defined(_EP_HorizonVS) || defined(_EP_HorizonPS) || defined(_EP_HorizonRingPS)
	#define _IO_HazeVS 1
#endif

#if defined(_IO_HazeVS) && defined(_VERTEX_SHADER)
layout(location = 0) out vec2 vs_hz_texUV;
layout(location = 1) out vec3 vs_hz_posW;
layout(location = 2) out float vs_hz_alpha;
#endif

#if defined(_IO_HazeVS) && defined(_FRAGMENT_SHADER)
layout(location = 0) in vec2 ps_hz_texUV;
layout(location = 1) in vec3 ps_hz_posW;
layout(location = 2) in float ps_hz_alpha;
#endif


#ifdef _FRAGMENT_SHADER

// ---------------------------------------------------------------------------------------------------
//
float SampleShadows(vec2 sp, float pd)
{
	if (sp.x < 0.0f || sp.y < 0.0f) return 0.0f;	// If a sample is outside border -> fully lit
	if (sp.x > 1.0f || sp.y > 1.0f) return 0.0f;

	if (pd < 0.0f) pd = 0.0f;
	if (pd > 2.0f) pd = 2.0f;

	vec2 dx = vec2(Prm.vSHD[1], 0) * 1.5f;
	vec2 dy = vec2(0, Prm.vSHD[1]) * 1.5f;
	float  va = 0.0f;

	sp -= dy;
	if ((texture(tShadowMap, sp - dx).r) > pd) va++;
	if ((texture(tShadowMap, sp).r) > pd) va++;
	if ((texture(tShadowMap, sp + dx).r) > pd) va++;
	sp += dy;
	if ((texture(tShadowMap, sp - dx).r) > pd) va++;
	if ((texture(tShadowMap, sp).r) > pd) va++;
	if ((texture(tShadowMap, sp + dx).r) > pd) va++;
	sp += dy;
	if ((texture(tShadowMap, sp - dx).r) > pd) va++;
	if ((texture(tShadowMap, sp).r) > pd) va++;
	if ((texture(tShadowMap, sp + dx).r) > pd) va++;

	return va * 0.1111111f;
}

// -------------------------------------------------------------------------------------------------------------
// Local light sources
//
void LocalLights(
	out vec3 diff_out,
	in vec3 nrmW,
	in vec3 posW)
{
	diff_out = vec3(0.0f);

	if (!Flow.bLocals) return;
	int i;

	// Relative positions
	vec3 p[4];
	for (i = 0; i < 4; i++) p[i] = posW - Lights.position[i];

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
	for (i = 0; i < 4; i++) att[i] = dot(Lights.attenuation[i].xyz, vec3(1.0, dst[i], dst[i] * dst[i]));

	att = 1.0f / att;

	// Spotlight factors
	vec4 spt = vec4(1.0f);

	for (i = 0; i < 4; i++) {
		spt[i] = (dot(p[i], Lights.direction[i]) - Lights.param[i][Phi]) * Lights.param[i][Theta];
		if (!Spotlight[i]) spt[i] = 1.0f;
	}

	spt = clamp(spt, 0.0f, 1.0f);

	// Diffuse light factors
	vec4 dif;
	for (i = 0; i < 4; i++) dif[i] = dot(-p[i], nrmW);

	dif = clamp(dif, 0.0f, 1.0f);
	dif *= (att * spt);

	for (i = 0; i < 4; i++) diff_out += Lights.diffuse[i].rgb * dif[i];
}


// Render Eclipse ------------------------------------------------------------
//
float GetEclipse(vec3 vVrt)
{
	if (Flow.bEclipse)
	{
		vec3 b = vVrt - Const.toSun * dot(vVrt, Const.toSun); // Flatten
		float  x = length(Prm.vEclipse - b) * Prm.fEclipse;
		// `tex1D(tEclipse, x)` on a 512x1 2D image; see note C in the header.
		return texture(tEclipse, vec2(clamp(x, 0.0f, 1.0f), 0.0f)).r;
	}
	return 1.0;
}

#endif // _FRAGMENT_SHADER



// ============================================================================
// Render SkyDome and Horizon
// ============================================================================

#if defined(_EP_HorizonVS)

// `float3 posL : POSITION0` (pHazeVertexDecl's position element; HazeMgr
// builds the dome from a position-only stream)
layout(location = 0) in vec3 posL;		// POSITION0

void HorizonVS()
{
	vs_hz_texUV = posL.xy*10.0;

	// A vertex input is read-only in GLSL where HLSL's by-value parameter was
	// not; the reference assigns to posL.xz and posL.y.
	vec3 p = posL;
	p.xz *= mix(Prm.vTexOff[0], Prm.vTexOff[1], p.y);
	p.y   = mix(Prm.vTexOff[2], Prm.vTexOff[3], p.y);

	vs_hz_posW = (Prm.mWorld * vec4(p, 1.0f)).xyz;
	gl_Position = Const.mVP * vec4(vs_hz_posW, 1.0f);

	// `alpha` is left at the zero the reference's `(HazeVS)0` gave it.
	vs_hz_alpha = 0.0f;
}

#endif // _EP_HorizonVS


// SkyDome Shader, Renders the sky from with-in atmosphere
//
#if defined(_EP_HorizonPS)
void HorizonPS()
{
	float fNoise = (textureLod(tNoise, ps_hz_texUV, 0.0f).r - 0.5f) * 0.03;

	vec3 uDir = normalize(ps_hz_posW);

	SkyOut sky = GetSkyColor(uDir);

	float ph = dot(uDir, Const.toSun);

	vec2  guv = vec2(dot(uDir, Const.ZeroAz), dot(uDir, Const.Up)) * GLARE_SIZE + 0.5f;
	float  cGlr = texture(tGlare, guv).r * clamp(ph, 0.0f, 1.0f) * Const.SunVis;

	vec3 color = HDR(sky.ray.rgb * RayPhase(ph) + (sky.mie.rgb + 0.0008f) * MiePhase(ph) * (0.75f + cGlr * Const.cGlare));

	oColor = vec4(color + fNoise, sky.ray.a);
}
#endif // _EP_HorizonPS


// Renders the horizon "ring" from space
//
#if defined(_EP_HorizonRingPS)
void HorizonRingPS()
{
	vec3 uDir = normalize(ps_hz_posW);
	vec3 uOrt = normalize(uDir - Const.toCam * dot(uDir, Const.toCam));
	vec3 vVrt = Const.CamPos + ps_hz_posW;
	float d = dot(uDir, ps_hz_posW);
	float x = dot(uOrt, Const.SunAz) * 0.5 + 0.5;
	float r = length(vVrt);
	float q = (r - Const.PlanetRad) / Const.AtmoAlt;

	vec2 uv = vec2(x, q > 0.0f ? sqrt(q) : 0.0f);

	vec4 cRay = texture(tSkyRayColor, uv).rgba;
	vec3 cMie = texture(tSkyMieColor, uv).rgb;

	float ph = dot(uDir, Const.toSun);

	vec3 color = HDR(cRay.rgb * RayPhase(ph) + cMie * MiePhase(ph));

	color *= GetEclipse(vVrt);

	oColor = vec4(color, cRay.a);
}
#endif // _EP_HorizonRingPS



// ============================================================================
// Planet Surface Renderer
// ============================================================================

#define AUX_DIST		0	// Vertex distance
#define AUX_NIGHT		1	// Night lights intensity
#define AUX_SLOPE		2   // Terrain slope factor 0.0=flat, 1.0=sloped
#define AUX_RAYDEPTH	3   // Optical depth of a ray

#if defined(_EP_TerrainVS)

// struct TILEVERTEX (VERTEX_2TEX, pPatchVertexDecl)
layout(location = 0) in vec3 posL;		// POSITION0
layout(location = 1) in vec3 normalL;	// NORMAL0
layout(location = 2) in vec2 tex0;		// TEXCOORD0
layout(location = 3) in float elev;		// TEXCOORD1

void TerrainVS()
{
	vec4 vElev = vec4(0.0f);
	vec3 vNrmW;

	// Apply a world transformation matrix
	vec3 vPosW = (Prm.mWorld * vec4(posL, 1.0f)).xyz;
	vec3 vVrt = Const.CamPos + vPosW;
	vec3 vPlN = normalize(vVrt);

	if (FlowVS.bElevOvrl)
	{
		// ----------------------------------------------------------
		// Elevation Overlay
		//
		vec2 vUVOvl = tex0.xy * Prm.vOverlayOff.zw + Prm.vOverlayOff.xy;

		// Sample Elevation Map
		vElev = textureLod(tElvOverlay, vUVOvl, 0.0f);

		// Construct world space normal
		vNrmW = vec3(vElev.xy, sqrt(clamp(1.0f - dot(vElev.xy, vElev.xy), 0.0f, 1.0f)));
		vNrmW = (Prm.mWorld * vec4(vNrmW, 0.0f)).xyz;

		// Reconstruct Elevation
		vPosW += normalize(Const.CamPos + vPosW) * (vElev.z - elev) * vElev.w;
	}
	else {
		vNrmW = (Prm.mWorld * vec4(normalL, 0.0f)).xyz;
	}

	// Disrecard elevation and make the surface spherical
	if (FlowVS.bSpherical) {
		vPosW = (normalize(Const.CamPos + vPosW) * Const.PlanetRad) - Const.CamPos;
		vNrmW = vPlN;
	}
	gl_Position = Const.mVP * vec4(vPosW, 1.0f);

#if defined(_SHDMAP)
	vs_tv_shdH = Prm.mLVP * vec4(vPosW, 1.0f);
#endif

	vs_tv_texUV.xy = tex0.xy;
	vs_tv_camW = vec4(-vPosW, dot(vVrt, vPlN));
	vs_tv_nrmW = vNrmW;
}

#endif // _EP_TerrainVS


#ifdef _FRAGMENT_SHADER

bool InRange(vec2 a)
{
	return (a.x > 0.0f && a.x < 1.0f) && (a.y > 0.0f && a.y < 1.0f);
}

float GGX_NDF(float dHN, float rgh)
{
	float r2 = rgh * rgh;
	float dHN2 = dHN * dHN;
	float d = (r2 * dHN2) + (1.0f - dHN2);
	return r2 / (3.14f * d * d);
}

#endif // _FRAGMENT_SHADER


#if defined(_EP_TerrainPS)
void TerrainPS()
{

	vec2 vUVSrf = ps_tv_texUV.xy * Prm.vTexOff.zw + Prm.vTexOff.xy;
	vec2 vUVWtr = ps_tv_texUV.xy * Prm.vMicroOff.zw + Prm.vMicroOff.xy;
	vec2 vUVCld = ps_tv_texUV.xy * Prm.vCloudOff.zw + Prm.vCloudOff.xy;

	vUVWtr.x += Const.Time / 180.0f;

	vec3 cNrm = vec3(0.5, 0.5, 1.0);
	float fChA = 0.0f, fChB = 0.0f;

#if defined(_RIPPLES)
	if (Flow.bTexture) cNrm = texture(tOcean, vUVWtr).xyz;
#endif

	// Fetch Main Textures
	vec4 cTex = vec4(0.5, 0.5, 0.5, 1.0);
	if (Flow.bTexture) cTex = texture(tDiff, vUVSrf);

	vec4 cMsk = vec4(0, 0, 0, 1);
	if (Flow.bMask) cMsk = texture(tMask, vUVSrf);

#if defined(_DEVTOOLS)
	if (Flow.bOverlay) {
		vec2 vUVOvl = ps_tv_texUV.xy * Prm.vOverlayOff.zw + Prm.vOverlayOff.xy;
		if (InRange(vUVOvl)) {
			vec4 cOvl = texture(tOverlay, vUVOvl);
			vec4 cWtr = texture(tMskOverlay, vUVOvl);
			cTex.rgb = mix(cTex.rgb, cOvl.rgb, cOvl.a * Prm.vOverlayCtrl[0].rgb);
			cMsk.rgb = mix(cMsk.rgb, cWtr.rgb, cOvl.a * Prm.vOverlayCtrl[1].rgb);
			cMsk.a = mix(cMsk.a, cWtr.a, Prm.vOverlayCtrl[1].a);
		}
	}
#endif

#if defined(_CLOUDSHD)
	if (Flow.bCloudShd) {
		fChA = texture(tCloud, vUVCld).a;
		fChB = texture(tCloud2, vUVCld - vec2(1, 0)).a;
	}
#endif

	float fShadow = 1.0f;

#if defined(_SHDMAP)
	if (Flow.bShadows) {
		// `frg.shdH` is an interpolant and read-only in GLSL; the reference
		// divides its by-value copy in place.
		vec4 shdH = ps_tv_shdH;
		shdH.xyz /= shdH.w;
		shdH.z = 1.0f - shdH.z;
		vec2 sp = shdH.xy * vec2(0.5f, -0.5f) + vec2(0.5f, 0.5f);
		float  pd = shdH.z + 0.05f * Prm.vSHD[3];
		fShadow = 1.0f - SampleShadows(sp, pd);
	}
#endif

	vec3 cFar, cMed, cLow;

#if defined(_MICROTEX)
	vec2 UV = ps_tv_texUV.xy;
	// Create normals
	if (Flow.bMicroTex)
	{
		if (Flow.bMicroNormals) {
			// Normal in .ag luminance in .b
			cFar = texture(tMicroC, UV * Prm.vMSc[2].zw + Prm.vMSc[2].xy).agb;	// High altitude micro texture C
			cMed = texture(tMicroB, UV * Prm.vMSc[1].zw + Prm.vMSc[1].xy).agb;	// Medimum altitude micro texture B
			cLow = texture(tMicroA, UV * Prm.vMSc[0].zw + Prm.vMSc[0].xy).agb;	// Low altitude micro texture A
		}
		else {
			// Color in .rgb no normals
			cFar = texture(tMicroC, UV * Prm.vMSc[2].zw + Prm.vMSc[2].xy).rgb;	// High altitude micro texture C
			cMed = texture(tMicroB, UV * Prm.vMSc[1].zw + Prm.vMSc[1].xy).rgb;	// Medimum altitude micro texture B
			cLow = texture(tMicroA, UV * Prm.vMSc[0].zw + Prm.vMSc[0].xy).rgb;	// Low altitude micro texture A
		}
	}
#endif

	vec3 cRfl = vec3(0.0f);
	vec3 nvrW = normalize(ps_tv_nrmW);			// Per-pixel surface normal vector
	vec3 vRay = normalize(ps_tv_camW.xyz);		// Unit viewing ray
	vec3 vVrt = Const.CamPos - ps_tv_camW.xyz;	// Geo-centric pixel position
	vec3 vPlN = normalize(vVrt);				// Planet mean normal
	vec3 hlvW = normalize(vRay + Const.toSun);
	float   dst = dot(vRay, ps_tv_camW.xyz);	// Pixel to camera distance
	float   rad = ps_tv_camW.w;					// Pixel geo-distance
	float   alt = rad - Const.PlanetRad;		// Pixel altitude over mean radius
	float  fSrf = (1.0 - Const.CamSpace);		// Camera colse to surface ?
	float fMask = (1.0 - cMsk.a);				// Specular Mask
	float  fSpe = 0.0f;
	float fAmpf = 1.0f;
	float fDRS = dot(vRay, Const.toSun);
	float fDPS = dot(vPlN, Const.toSun);		// Mean normal dot sun


#if defined(_WATER)
#if defined(_RIPPLES)

	// Compute world space normal for water rendering
	//
	cNrm.xy = (cNrm.xy - 0.5f) * 2.0f;
	cNrm.z *= Const.wNrmStr;
	cNrm = normalize(cNrm);

	vec3 wnrmW = (Const.vTangent * cNrm.r) + (Const.vBiTangent * cNrm.g) + (vPlN * cNrm.b);
	wnrmW = mix(nvrW, wnrmW, fMask);
	float fDWS = dot(wnrmW, Const.toSun); // Water normal dot sun

	// Render with specular ripples and fresnel water -------------------------
	//
	float fDCH = clamp(dot(vRay, hlvW), 0.0f, 1.0f);
	float fDCN = clamp(dot(vRay, wnrmW), 0.0f, 1.0f);
	float fDHN = dot(hlvW, wnrmW);

	vec3 f = 1.0 - vec3(fDCH, fDCN, fDWS);
	vec3 fFresnel4 = f * f * f;
	vec3 fF = (0.15f + fFresnel4 * 0.85f) * fMask * Const.wSpec;

	// Compute specular reflection intensity
	fSpe = GGX_NDF(fDHN, 0.1f + clamp(fDWS, 0.0f, 1.0f) * 0.1f) * fF.y;
	fSpe /= (4.0f * fDCH * max(fDWS, fDCN) + 1e-3);

	// Apply fresnel water only if close enough to a surface
	//
	if (!Flow.bInSpace)
	{
		cRfl = GetAmbient(reflect(-vRay, wnrmW)) * fF.y * fSrf;
		// Attennuate diffuse texture for fresnel refl.
		cTex.rgb *= clamp(1.0f - f.y * fSrf * fMask, 0.0f, 1.0f) * clamp(1.0f - f.z * fSrf * fMask, 0.0f, 1.0f);
	}

	cTex.rgb = clamp(cTex.rgb + vec3(0, 0.55, 1.0) * Const.wBrightness * fMask, 0.0f, 1.0f);

#else
	// Fallback to simple specular reflection
	float fDHN = dot(hlvW, nvrW);
	fSpe = pow(clamp(fDHN, 0.0f, 1.0f), 60.0f) * fMask * 5.0f;
#endif
#endif

	vec3 nrmW = nvrW; // Micro normal defaults to vertex normal

	// Render with surface microtextures --------------------------------------
	//
#if defined(_MICROTEX)

	if (Flow.bMicroTex)
	{
		float step1 = smoothstep(15000.0f, 3000.0f, dst);
		step1 *= (step1 * step1);
		vec3 cFnl = max(vec3(0.0f), min(vec3(2.0f), 1.333f * (cFar + cMed + cLow) - 1.0f));

		// Create normals
		if (Flow.bMicroNormals)
		{
			cFnl = cFnl.bbb;

#if defined(_SOFT)
			vec2 cMix = (cFar.rg + cMed.rg + cLow.rg) * 0.6666f;			// SOFT BLEND
#endif
#if defined(_MED)
			vec2 cMix = (cFar.rg + 0.5f) * (cMed.rg + 0.5f) * (cLow.rg + 0.5f);	// MEDIUM BLEND
			fAmpf = 2.0f;
#endif
#if defined(_HARD)
			vec2 cMix = cFar.rg * cMed.rg * cLow.rg * 8.0f;				// HARD BLEND
			fAmpf = 4.0f;
#endif

			// The reference names this `cNrm` too, shadowing the water one
			// declared at the top of the function. See note D in the header.
			vec3 cMicroNrm = vec3((cMix - 1.0f) * 2.0f, 0) * step1;
			cMicroNrm.z = cos(cMicroNrm.x * cMicroNrm.y * 1.57);

			// Approximate world space normal
			nrmW = normalize((Const.vTangent * cMicroNrm.x) + (Const.vBiTangent * cMicroNrm.y) + (nvrW * cMicroNrm.z));

			// Bend the normal towards sun a bit
			nrmW = normalize(nrmW + Const.toSun * 0.06f);
		}

		// Apply luminance
		cTex.rgb *= mix(vec3(1.0f), cFnl, step1);
	}
#endif


	// Render Eclipse ------------------------------------------------------------
	//
	float fECL = GetEclipse(vVrt);

	vec3 cDiffLocal = vec3(0.0f);

#if defined(_LOCALLIGHTS)
	LocalLights(cDiffLocal, nrmW, -ps_tv_camW.xyz);
#endif

#if defined(_NO_ATMOSPHERE)

	float fDNS = clamp(dot(nvrW, Const.toSun), 0.0f, 1.0f);
	float fDCN = clamp(dot(nvrW, Const.toCam), 0.0f, 1.0f);
	float fLvl = 2.0f * fDNS / (fDNS + fDCN + 0.5f);
	float fSHD = 1.0f;

	// Shadowing by planet
	if (Flow.bPlanetShadow) {
		float palt = sqrt(clamp(1.0f - fDPS * fDPS, 0.0f, 1.0f)) * rad - Const.PlanetRad;
		fSHD = fDPS > 0.0f ? 1.0f : ilerp(Const.MinAlt, Const.MaxAlt, palt);
	}

	// Amplify light and shadows
	fLvl += dot(nvrW - vPlN, Const.toSun) * fLvl * Const.trLS;

	// Add opposition surge
	fLvl += pow(clamp(fDRS, 0.0f, 1.0f), 4.0f) * 0.3f * fDNS;

#if defined(_MICROTEX)
	fLvl += dot(nrmW - nvrW, Const.toSun) * ilerp(0.0, 0.03, fLvl) * fAmpf;
#endif

	fLvl *= fSHD;	// Apply planet shadow
	fLvl *= fECL;	// Apply eclipse

	vec3 color = cTex.rgb * LightFX(max(fLvl, 0.0f) * fShadow + cDiffLocal);
	oColor = vec4(pow(clamp(color * Const.TrExpo, 0.0f, 1.0f), vec3(Const.TrGamma)), 1.0f);		// Gamma corrention
	return;
#else

	float fShd = 1.0f;

#if defined(_CLOUDSHD)
	// Do we render cloud shadows ?
	if (Flow.bCloudShd) {
		fShd = (vUVCld.x < 1.0 ? fChA : fChB);
		fShd = clamp(1.0 - fShd * Prm.fAlpha, 0.0f, 1.0f);
	}
#endif

	vec3 cNgt = vec3(0.0f);
	vec3 cNgt2 = vec3(0.0f);
	float fDNS = dot(nvrW, Const.toSun); // Vertex normal dot sun

#if defined(_NIGHTLIGHTS)

	// Night lights ?
	float fNgt = clamp(-fDPS * 4.0f + 0.05f, 0.0f, 1.0f) * Prm.fBeta; // Night lights intensity and 'on' time

	cMsk.b = (cMsk.b > 0.15f ? cMsk.b : 0.0f); // Blue dirt filter

	cNgt = cMsk.rgb * (1.0f - Const.CamSpace) * fNgt; // Nightlights surface texture illumination term
	cNgt2 = cMsk.rgb * Const.CamSpace * 4.0f * fNgt; // Nightlights orbital visibility
#endif

	float fNoise = (textureLod(tNoise, ps_tv_texUV.xy * 4.0f * Prm.fTgtScale, 0.0f).r - 0.5f) * ATMNOISE;

	// Terrain with gamma correction and attennuation
	cTex.rgb = pow(clamp(cTex.rgb, 0.0f, 1.0f), vec3(Const.TrGamma)) * Const.TrExpo;

	// Evaluate ambient approximation
	vec4 cAmb = AmbientApprox(vPlN, false);

	LandOut sct = GetLandView(rad, vPlN);

	// Get the color of sunlight and set maximum intensity to 1.0
	vec3 cSun = GetSunColor(fDPS, alt);
	vec3 cSF = cSun * Const.cSun;
	float fMx = max(max(cSF.r, cSF.g), cSF.b);
	cSF = fMx > 1.0 ? cSF / fMx : cSF;

	float  fL = Const.trLS * 0.3f;
	float  fZ = clamp(dot(nvrW - vPlN, Const.toSun) * Const.trLS, -fL, fL);
	float  fX = 1.0f - pow(1.0f - clamp(fDPS, 0.0f, 1.0f), 2.0f);

	fZ = fZ > 0.0f ? fZ * 2.0f : fZ;

#if defined(_MICROTEX)
	float  fG = dot(nrmW - nvrW, Const.toSun) * fAmpf;
#else
	float  fG = 0.0f;
#endif

	// Diffuse "lambertian" shading term
	float  fD = mix(fX + (fG + fZ) * fX, fDPS * fDPS, fMask);

	// Water masking
	float  fM = 0.5f - fMask * 0.25f;

	// Ambient light for terrain
	//					  Color					   Distance				  Altitude factor		   Particle Density
	vec3 cA = normalize(cAmb.rgb + cSF * 4.0f) * cAmb.a * cAmb.g * fM * exp(-alt * Const.iH.r) * Const.rmI.r * 6e5 * Const.TW_Terrain;

	fShd = clamp(fShd + (1.0f - fX), 0.0f, 1.0f);

	// Bake light and shadow terms
	vec3 cL = cSF * fD * fShadow * fShd;

	// Lit the texture with various things
	cTex.rgb *= cL * 2.0f + (cA + cDiffLocal + Const.cAmbient * Const.Ambient) * clamp(1.0f + fG + fZ, 0.0f, 1.0f) + cNgt;

	cTex.rgb = max(vec3(0, 0, 0), cTex.rgb);

	// Add Reflection
	cTex.rgb += cRfl * 0.75f;

	// Add Specular component
	cTex.rgb += cSun * fSpe * smoothstep(-0.001f, 0.03f, fDPS);

	// Amplify cloud shadows for orbital views
	float fOrbShd = 1.0f - (1.0f - fShd) * Const.CamSpace * 0.5f;

	// Add Haze and night lights
	cTex.rgb *= sct.atn.rgb;
	cTex.rgb += (sct.ray.rgb * RayPhase(-fDRS) + sct.mie.rgb * MiePhase(-fDRS)) * fOrbShd * (1.0f + fNoise);

	cTex.rgb *= fECL;	// Apply eclipse
	cTex.rgb += cNgt2;

	oColor = vec4(HDR(cTex.rgb), 1.0f);
	return;
#endif
}
#endif // _EP_TerrainPS







// ============================================================================
// Planet Cloud Renderer
// ============================================================================

#if defined(_EP_CloudVS)

// struct TILEVERTEX (VERTEX_2TEX, pPatchVertexDecl). elev is described by the
// vertex layout and read by neither stage of this pass.
layout(location = 0) in vec3 posL;		// POSITION0
layout(location = 1) in vec3 normalL;	// NORMAL0
layout(location = 2) in vec2 tex0;		// TEXCOORD0

void CloudVS()
{
	// Apply a world transformation matrix
	vec3 vPosW = (Prm.mWorld * vec4(posL, 1.0f)).xyz;
	vec3 vNrmW = (Prm.mWorld * vec4(normalL, 0.0f)).xyz;

	gl_Position = Const.mVP * vec4(vPosW, 1.0f);
	vs_cv_nrmW = vNrmW;
	vs_cv_posW = vPosW;
	vs_cv_texUV.xy = tex0.xy;						// Note: vrt.tex0 is un-used (hardcoded in Tile::CreateMesh and varies per tile)
}

#endif // _EP_CloudVS


// ============================================================================
//
#if defined(_EP_CloudPS)
void CloudPS()
{
	vec2 vUVTex = ps_cv_texUV.xy;
	vec4 cTex = texture(tDiff, vUVTex);
	vec3 vRay;
	vec3 vPxl;
	float  dRC;
	float  fNrm = 1.0f;

	if (Flow.bBelowClouds) {
		float  rRef = Const.PlanetRad + Const.smi * 0.5f;	// Reference altitude
		vec3 vRef = Const.toCam * rRef;
		vRay = normalize(Const.toCam * (Const.CamRad - rRef) + ps_cv_posW); // Viewing ray to the pixel
		dRC = dot(vRay, Const.toCam);
		float  fEca2 = 1.0f - dRC * dRC;			// Ray horizon angle^2
		float  fD = Const.smi * inversesqrt(1.0f - Const.ecc * Const.ecc * fEca2); // Distance to ellipse threshold
		vPxl = vRef + vRay * fD;					// Pretend the pixel being closer and lower
	}
	else {
		vRay = normalize(ps_cv_posW);				// Viewing ray to the pixel
		dRC = dot(vRay, Const.toCam);
		vPxl = Const.CamPos + ps_cv_posW;			// Pixel's geocentric location
	}

	vec3 vPlN = normalize(vPxl);					// Mean Normal at pixel's locatin
	vec3 vVrt = Const.CamPos + ps_cv_posW.xyz;		// Geo-centric pixel position
	vec3 nrm = vPlN;

	float dRS = dot(vRay, Const.toSun);
	float dMNus = dot(vPlN, Const.toSun);
	float dMN = clamp(dMNus, 0.0f, 1.0f);			// Mean normal sun angle
	float fPxR = dot(vPxl, vPlN);					// Pixel geo distance
	float fPxA = fPxR - Const.PlanetRad;			// Pixel altitude

	if (!Flow.bBelowClouds) fPxA = Const.CloudAlt;


	// -----------------------------------------------
	// Cloud layer rendering for Earth
	// -----------------------------------------------

#if defined(_CLOUDMICRO)
	vec2 vUVMic = ps_cv_texUV.xy * Prm.vMicroOff.zw + Prm.vMicroOff.xy;
	vec4 cMic = texture(tCloudMicro, vUVMic);
#endif


#if defined(_CLOUDNORMALS)
#if defined(_CLOUDMICRO)

	vec4 cMicNorm = texture(tCloudMicroNorm, vUVMic);  // Filename "cloud1_norm.dds"

	// Extract normal from transparency (height) data
	// Filter width
	float d = 2.0 / 512.0;

	float x1 = texture(tDiff, vUVTex + vec2(-d, 0)).a;
	float x2 = texture(tDiff, vUVTex + vec2(+d, 0)).a;
	nrm.x = (x1 * x1 - x2 * x2);

	float y1 = texture(tDiff, vUVTex + vec2(0, -d)).a;
	float y2 = texture(tDiff, vUVTex + vec2(0, +d)).a;
	nrm.y = (y1 * y1 - y2 * y2);

	// Blend in cloud normals only on moderately thick clouds, allowing the highest cloud tops to be smooth.
	nrm.xy = (nrm.xy + clamp((cTex.a * 10.0f) - 3.0f, 0.0f, 1.0f) * clamp(((1.0f - cTex.a) * 10.0f) - 1.0f, 0.0f, 1.0f) * (cMicNorm.rg - 0.5f)); // new

	// Increase normals contrast based on sun-earth angle.
	nrm.xyz = nrm.xyz * (1.0f + (0.5f * dMN));

	nrm.z = sqrt(1.0f - clamp(nrm.x * nrm.x + nrm.y * nrm.y, 0.0f, 1.0f));

	// Approximate world space normal from local tangent space
	nrm = normalize((Const.vTangent * nrm.x) + (Const.vBiTangent * nrm.y) + (vPlN * nrm.z));

	float dCS = dot(nrm, Const.toSun); // Cloud normal sun angle

	// Brighten the lighting model for clouds, based on sun-earth angle. Twice is better.
	// Low sun angles = greater effect. No modulation leads to washed out normals at high sun angles.
	dCS = clamp((1.0f - dMN) * (dCS * (1.0f - dCS)) + dCS, 0.0f, 1.0f);
	dCS = clamp((1.0f - dMN) * (dCS * (1.0f - dCS)) + dCS, 0.0f, 1.0f);

	// With a high sun angle, don't let the dCS go below 0.2 to avoid unnaturally dark edges.
	dCS = mix(0.2f * dMN, 1.0f, dCS);

	// Effect of normal/sun angle to color
	// Add some brightness (borrowing red channel from sunset attenuation)
	// Adding it to the sun illumination factor, taking care to keep from saturating
	fNrm = dCS +((1.0f - dCS) * 0.2f);
#endif
#endif

#if defined(_CLOUDMICRO)
	float f = cTex.a;
	float g = mix(1.0f, cMic.a, 1.0f - abs(dot(Const.vPolarAxis, vPlN)));
	float h = (g + 4.0f) * 0.2f;
	cTex.a = clamp(mix(g, h, f) * f, 0.0f, 1.0f);
#endif

	// Render Eclipse ------------------------------------------------------------
	//
	float fECL = GetEclipse(vVrt);

	if (Flow.bBelowClouds)
	{
		// Get sunlight color
		vec3 cSun = GetSunColor(dMNus, fPxA);

		// Get ambient information
		vec4 cMlt = AmbientApprox(vPlN);

		cSun *= clamp(dRS + 1.3f, 0.0f, 1.0f);
		float fPh = pow(clamp(1.0f - dRC, 0.0f, 1.0f), 32.0f) * pow(clamp(dRS, 0.0f, 1.0f), 10.0f); // Boost near horizon and close the sun
		cSun *= 1.0f + fPh * 8.0f;

		cSun *= Const.cSun * fNrm;
		cSun *= Const.Clouds;
		cSun += cMlt.rgb * cMlt.a * 0.2f;

		LandOut sct = GetLandView(fPxA + Const.PlanetRad, vPlN);

		cTex.rgb *= cSun;
		cTex.rgb *= sct.atn.rgb;
		cTex.rgb += sct.ray.rgb * 2.0f;
		cTex.rgb *= fECL;

		oColor = vec4(HDR(cTex.rgb), clamp(cTex.a, 0.0f, 1.0f));
		return;
	}
	else {

		// Get sunlight color
		vec3 cSun = GetSunColor(dMN, fPxA);

		// Get ambient information
		vec4 cAmb = AmbientApprox(dMNus);
		vec3 cMSC = Const.RayWave * Const.RayWave * Const.Clouds; // Multiscatter color

		cSun = sqrt(cMSC * cMSC + cSun * cSun * fNrm) * cAmb.a;

		LandOut sct = GetLandView(fPxA + Const.PlanetRad, vPlN);

		cTex.rgb *= cSun;
		cTex.rgb *= sct.atn.rgb;
		cTex.rgb += sct.ray.rgb;
		cTex.rgb *= fECL;

		oColor = vec4(sqr(HDR(cTex.rgb * 4.0f)), cTex.a * cAmb.a * cAmb.a);
		return;
	}
}
#endif // _EP_CloudPS








// ============================================================================
// Gas Giant Renderer
// ============================================================================

#if defined(_EP_GiantVS)

// struct TILEVERTEX (VERTEX_2TEX, pPatchVertexDecl)
layout(location = 0) in vec3 posL;		// POSITION0
layout(location = 1) in vec3 normalL;	// NORMAL0
layout(location = 2) in vec2 tex0;		// TEXCOORD0

void GiantVS()
{
	// Apply a world transformation matrix
	vec3 vPosW = (Prm.mWorld * vec4(posL, 1.0f)).xyz;
	vec3 vNrmW = (Prm.mWorld * vec4(normalL, 0.0f)).xyz;

	gl_Position = Const.mVP * vec4(vPosW, 1.0f);
	vs_tv_texUV.xy = tex0.xy;
	vs_tv_camW = vec4(-vPosW, 0);
	vs_tv_nrmW = vNrmW;

#if defined(_SHDMAP)
	// `shdH` is left at the zero the reference's `(TileVS)0` gave it: GiantVS
	// does not compute a shadow-map position, and GiantPS does not read one.
	vs_tv_shdH = vec4(0.0f);
#endif
}

#endif // _EP_GiantVS


// ============================================================================
//
#if defined(_EP_GiantPS)
void GiantPS()
{

	vec2 vUVSrf = ps_tv_texUV.xy * Prm.vTexOff.zw + Prm.vTexOff.xy;

	// Fetch Main Textures
	vec4 cTex = texture(tDiff, vUVSrf);

	vec3 nrmW = normalize(ps_tv_nrmW);			// Per-pixel surface normal vector
	vec3 vRay = normalize(ps_tv_camW.xyz);		// Unit viewing ray
	vec3 vVrt = Const.CamPos - ps_tv_camW.xyz;	// Geo-centric pixel position
	vec3 vPlN = normalize(vVrt);				// Planet mean normal
	float  fDPS = dot(vPlN, Const.toSun);
	vec3 cSun = vec3(clamp((fDPS + 0.1) * 5.0, 0.0f, 1.0f));


	// Render Eclipse ------------------------------------------------------------
	//
	cSun *= GetEclipse(vVrt);

	// Terrain with gamma correction and attennuation
	cTex.rgb = pow(clamp(cTex.rgb, 0.0f, 1.0f), vec3(Const.TrGamma)) * Const.TrExpo;

	vec3 color = cTex.rgb * LightFX(cSun + vec3(0.9, 0.9, 1.0) * Const.Ambient);

	oColor = vec4(HDR(color), 1.0f);
}
#endif // _EP_GiantPS


// ============================================================================
// Gas giant cloud layer renderer
// ============================================================================

#if defined(_EP_GiantCloudPS)
void GiantCloudPS()
{
	vec4 cTex = texture(tDiff, ps_cv_texUV.xy);
	vec3 vPlN = normalize(ps_cv_nrmW);
	vec3 vRay = normalize(ps_cv_posW);
	vec3 vVrt = Const.CamPos + ps_cv_posW.xyz;	// Geo-centric pixel position
	float  fDPS = dot(vPlN, Const.toSun);		// Planet mean normal sun angle

	vec3 cSun = vec3(clamp((fDPS + 0.1) * 5.0, 0.0f, 1.0f));

	// Render Eclipse ------------------------------------------------------------
	//
	float fECL = GetEclipse(vVrt);

	cTex.rgb *= LightFX(cSun + vec3(1.0, 1.0, 1.0) * Const.Ambient);
	cTex.rgb = pow(clamp(cTex.rgb, 0.0f, 1.0f), vec3(Const.TrGamma)) * Const.TrExpo;
	cTex.rgb *= fECL;

	oColor = vec4(HDR(cTex.rgb), clamp(cTex.a, 0.0f, 1.0f));
}
#endif // _EP_GiantCloudPS
