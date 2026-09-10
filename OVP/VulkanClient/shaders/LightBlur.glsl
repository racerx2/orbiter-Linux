// ===========================================================================
// LightBlur.glsl -- converted from OVP/D3D9Client/shaders/LightBlur.hlsl
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2016-2026 Jarmo Nikkanen
//				 2016 SolarLiner (Nathan Graule)
// ===========================================================================
//
// The seven general conventions are in IPI.glsl. This is an ImageProcessing
// shader, so it follows that file's binding convention: the VERTEX stage's
// uniform block at binding 0 -- which belongs to IPI.glsl, since Scene.cpp
// supplies no _vsentry and ImageProcessing then pairs every pixel shader here
// with IPI's VSMain -- the PIXEL stage's block at binding 1, and the samplers
// from binding 2 in declaration order.
//
// THE INTERPOLANTS ARE IPI'S VSMain OUTPUTS, and that is why they are two
// separate floats rather than a vec2: `float x : TEXCOORD0, float y :
// TEXCOORD1` is how all three pixel shaders here take them, and IPI.glsl
// writes them as vsX at location 0 and vsY at location 1. Folding them would
// change an interface two files are written to.
//
// THE `#define`s ARE DATA, NOT ONLY COMPILE-TIME CONSTANTS. Scene.cpp reads
// three of them back out of this file's TEXT --
//
//     BufSize      = pLightBlur->FindDefine("BufferDivider");
//     BufFmt       = pLightBlur->FindDefine("BufferFormat");
//     iGensPerFrame= pLightBlur->FindDefine("PassCount");
//
// -- with ImageProcessing::FindDefine, which scans the source for a line
// beginning `#define ` at column zero and reads the name and an integer after
// it. So those three must stay spelled exactly this way, at column zero, and
// keep an integer as their first token. Same requirement as on Windows; the
// scanner is the same one, converted.
// ===========================================================================

#version 450
#extension GL_EXT_scalar_block_layout : require

// Shader configurations -----------------------------------------------
//
//#define fGlowIntensity	0.1		// Overal glow brightness multiplier
#define radius			20		// Radius of the glow
#define rate			0.7     // glow "linearity" [0.7 to 0.95]
#define fMinThreshold	1.1		// Glow starts to appear when back buffer intensity reaches this level
#define fMaxThreshold	2.5		// Glow reaches it's maximum intensity when backbuffer goes above this level


// Orher configurations ------------------------------------------------
//
#define fSunIntensity		3.14	// Sunlight intensity
#define fInvSunIntensity	(1.0/fSunIntensity)

// ---------------------------------------------------------------------
// Client configuration parameters
//
#define BufferDivider	2		// Blur buffer size in pixels = ScreenSize / BufferDivider
#define PassCount		1		// Number of "bBlur" passes
#define BufferFormat	1		// Render buffer format, 2=RGB10A2, 1 = RGBA_16F, 0=DEFAULT (RGBX8)
// ---------------------------------------------------------------------


#ifdef _FRAGMENT_SHADER

// The eleven `uniform extern` parameters, in the reference's order. Scene.cpp
// writes seven of them by name; PassId is declared and read by nothing, here
// and there, and is kept so the block matches the file it came from.
layout(set = 0, binding = 1, scalar) uniform LightBlurPSBlock
{
	float   fIntensity;
	float   fDistance;
	float   fThreshold;
	float   fGamma;
	vec2    vSB;
	vec2    vBB;
	bool    bDir;
	bool    bBlur;
	bool    bBlendIn;
	bool    bSample;

	int     PassId;	// NOTE: CANNOT be used to toggle code section on and off efficiently, any code effected by PassId must be minimized
};

layout(set = 0, binding = 2) uniform sampler2D tBack;
layout(set = 0, binding = 3) uniform sampler2D tBlur;
layout(set = 0, binding = 4) uniform sampler2D tCLUT;	// 2D D3D9Clut.dds texture
layout(set = 0, binding = 5) uniform sampler2D tTone;	// 4x4 mipmap of backbuffer


// IPI.glsl's VSMain outputs, in its own order. See the header.
layout(location = 0) in float psX;		// TEXCOORD0
layout(location = 1) in float psY;		// TEXCOORD1

layout(location = 0) out vec4 oColor;


// `static const float3 cMult = { 3.0f, 1.0f, 5.0f };`
//
// A file-scope constant that nothing in the file reads. Carried across
// unchanged rather than dropped; `static` has no counterpart in GLSL, where a
// file-scope const already has internal linkage.
const vec3 cMult = vec3(3.0f, 1.0f, 5.0f);

float Desaturate (vec3 color)
{
	return dot(color, vec3(0.2, 0.7, 0.1) );
}



vec3 HDRtoLDR(vec3 hdr)
{
	vec3 h2 = hdr*hdr;
	return hdr * pow(max(vec3(0.0f), 1.0f + h2*h2), vec3(-0.25));
}


#if defined(_EP_PSMain)
void PSMain()
{
	vec2 vPos = vec2(psX,psY);

	vec2 vX = vec2(vSB.x, 0);		// Delta between two pixels in a "glow" buffer
	vec2 vY = vec2(0, vSB.y);

	vec2 sX = vec2(vBB.x, 0);		// Delta between two pixels in backbuffer
	vec2 sY = vec2(0, vBB.y);

	vec3 color = vec3(0.0f);


	// Sample a backbuffer into a glow buffer --------------------------------
	//
	if (bSample) {
		vec3 res = texture(tBack, vPos).rgb;
		//res += tex2D(tBack, vPos + sX).rgb;
		//res += tex2D(tBack, vPos + sY).rgb;
		//res += tex2D(tBack, vPos + sX + sY).rgb;
		//res *= 0.25f;
		float s = Desaturate(res);
		res *= smoothstep(fThreshold, fThreshold*1.5f, s) * 3.0f * inversesqrt(1.0f + s*s);
		oColor = vec4(abs(res), 1);
		return;
	}


	// Construct a glow gradient ---------------------------------------------
	//
	if (bBlur) {

		if (bDir) vX = vY;

		vec2 pos = vPos;
		float  f = 1.0f;
		float  d = 1.0f;

		color += texture(tBlur, pos).rgb;

		for (int i = 1; i<radius; i++)
		{
			vec2 vXi = float(i)*vX;
			color += f * texture(tBlur, pos - vXi).rgb;
			color += f * texture(tBlur, pos + vXi).rgb;
			d += f * 2.0f;
			f *= fDistance * 0.4f + 0.5;
		}
		oColor = vec4(color/d, 1);
		return;
	}


	// Blend the glow buffer back into a backbuffer ---------------------------
	//
	if (bBlendIn) {

		vec3 L = texture(tBlur, vPos).rgb * fIntensity;
		vec3 B = texture(tBack, vPos).rgb;

		//float w = Desaturate(B);
		float q = Desaturate(L);


		L *= inversesqrt(1.0f + q*q);
		//B *= rsqrt(4 + w*w) * 2.24f;

		color = B + L;

		//float m = max(1, max(color.r, max(color.g, color.b)));
		//float k = max(0, m - 1);
		//color = 1 - ((1 - B)*(1 - L)); // Screen add
		//color = color / m; // lerp(color / m, float3(1, 1, 1), k * rsqrt(1 + k*k));

		color = HDRtoLDR(color);

		color = pow(abs(color), vec3(fGamma*0.6f + 0.4f));

		oColor = vec4(color, 1.0);
		return;
	}

	oColor = vec4(0.0f);
}
#endif // _EP_PSMain


// --------------------------------------------------------------
// Scale -2 to 3
//
vec4 HeightToColor(float a)
{
	if (a < -2.0f)		return vec4(0, 0, 0, 1);
	if (a >  3.0f)		return vec4(0, 0, 0, 1);
	if (a < -1.0f)		return clamp(vec4(0, 0, 2 + a, 1.0), 0.0f, 1.0f);
	else if (a < 0.0f)	return clamp(vec4(0, 1 + a, 1, 1.0), 0.0f, 1.0f);
	else if (a < 1.0f)	return clamp(vec4(a, 1, 1 - a, 1.0), 0.0f, 1.0f);
	else if (a < 2.0f)	return clamp(vec4(1, 2 - a, 0, 1.0), 0.0f, 1.0f);
	return clamp(vec4(1, a - 2, a - 2, 1.0), 0.0f, 1.0f);
}


// Visualize screen depth
//
#if defined(_EP_PSDepth)
void PSDepth()
{
	float z = texture(tBack, vec2(psX,psY)).a;
	if (z <= 0.0f) { oColor = vec4(0, 0, 0, 1); return; }
	float q = 3.0f - log(1.0f + sqrt(max(0.0f, z - 5.0)));
	oColor = vec4(HeightToColor(q).rgb, 1);
}
#endif // _EP_PSDepth

// Visualize screen space normals
//
#if defined(_EP_PSNormal)
void PSNormal()
{
	vec3 q = texture(tBack, vec2(psX,psY)).xyz;
	vec2 xy = (q.xy + 1.0f) * 0.5f;
	oColor = vec4(xy, abs(q.z), 1.0f);
}
#endif // _EP_PSNormal

#endif // _FRAGMENT_SHADER
