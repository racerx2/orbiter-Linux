// ==============================================================
// IrradianceInteg.glsl -- converted from
// OVP/D3D9Client/shaders/IrradianceInteg.hlsl (69 lines)
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// ==============================================================
//
// Three ImageProcessing pixel shaders in one file, built by
// Scene::IntegrateIrradiance and selected with Activate(): PSPreInteg (the
// constructor's entry), then PSInteg and PSPostBlur through CompileShader.
// The vertex half is IPI.glsl's VSMain. Conventions: see IPI.glsl.
//
// EACH ENTRY POINT IS COMPILED SEPARATELY, so each sees only the uniforms and
// samplers it actually reads -- glslang reports live uniforms only. That is
// what the D3DX constant table did too, per shader, and it is why `tSrc` and
// `tCube` can share this file without either shader paying for the other.
//
// IT IS ALSO WHY IProcess::BindResources WALKS THE ACTIVE SHADER FIRST.
// PSPreInteg and PSPostBlur both read `tSrc` at the same binding, and the
// compiled entries are held in a std::map ordered by name; filling that
// binding from whichever entry came first alphabetically would hand the
// active shader white. See the note there.
//
// `lerp` -> `mix`, `frac` -> `fract`, `texCUBE` -> `texture` on a samplerCube,
// `tex2D` -> `texture` on a sampler2D, `float3 x = 0` -> `vec3 x = vec3(0)`.
//
// `float2 sc : VPOS` IS DROPPED FROM PSInteg's PARAMETER LIST. VPOS is the
// ps_3_0 screen-space fragment position, whose GLSL counterpart is the
// built-in `gl_FragCoord` -- but the reference never reads `sc`, so there is
// nothing to translate. Naming it here would declare an interpolant the
// vertex shader does not write.
// ==============================================================

#version 450
#extension GL_EXT_scalar_block_layout : require

#define IKernelSize 150

// uniform extern float4 Kernel[IKernelSize]; float3 vNr, vUp, vCp;
// float2 fD; float fIntensity; bool bUp;
//
// With `scalar` an array of vec4 has a 16-byte stride and no trailing pad,
// which is what Scene's `SetFloat("Kernel", IKernel, sizeof(IKernel))` copies
// -- a flat FVECTOR4[150].
layout(set = 0, binding = 1, scalar) uniform IrradiancePSBlock
{
	vec4  Kernel[IKernelSize];
	vec3  vNr;			// North
	vec3  vUp;			// Up
	vec3  vCp;			// Forward (East)
	vec2  fD;
	float fIntensity;
	bool  bUp;
};

layout(set = 0, binding = 2) uniform samplerCube tCube;
layout(set = 0, binding = 3) uniform sampler2D   tSrc;

layout(location = 0) in float psX;		// TEXCOORD0
layout(location = 1) in float psY;		// TEXCOORD1

layout(location = 0) out vec4 oColor;


vec3 Paraboloidal_to_World(vec3 i)
{
	i.xy *= 1.1f;
	float d = (1.0f - dot(i.xy, i.xy)) * 0.5f;
	vec3  p = normalize(vec3(i.xy, d));
	return (vCp * p.x) + (vNr * p.y) + (vUp * p.z * i.z);
}


void PSPreInteg()
{
	vec3 color = vec3(0.0f);
	vec2 p = vec2(psX, psY);
	for (int j = 0; j < 8; j++) {
		for (int i = 0; i < 8; i++) {
			color += texture(tSrc, p + (vec2(i, j) - 4.0f) * fD).rgb;
		}
	}
	oColor = vec4(color * (1.0f / 64.0f), 1);
}


void PSInteg()
{
	vec2 qw = vec2(psX, psY) * 2.0f - 1.0f;
	vec3 vz = Paraboloidal_to_World(vec3(qw.xy, (bUp ? 1.0f : -1.0f)));

	vec3 q = mix(vUp, vCp, fract(psX * 21.0f));	// Randomize rotation
	vec3 w = mix(q, vNr, fract(psY * 17.0f));	// Randomize rotation
	vec3 vx = normalize(cross(vz, w));
	vec3 vy = normalize(cross(vz, vx));

	vec3 sum = vec3(0.0f);

	for (int i = 0; i < IKernelSize; i++) {
		vec3 d = (vx * Kernel[i].x) + (vy * Kernel[i].y) + (vz * Kernel[i].z);
		sum += texture(tCube, d).rgb * Kernel[i].w;
	}

	oColor = vec4(sqrt(sum * fIntensity * (0.7f / IKernelSize)), 1.0f);
}


void PSPostBlur()
{
	vec3 color = vec3(0.0f);
	vec2 p = vec2(psX, psY);
	color += texture(tSrc, p).rgb;
	color += texture(tSrc, p + vec2(0,  1) * fD).rgb;
	color += texture(tSrc, p + vec2(0, -1) * fD).rgb;
	// The reference samples (-1, 0) TWICE and never samples (+1, 0). Kept
	// exactly as written: it is what the Windows client computes, and the
	// 0.2 divisor below counts five taps either way, so "fixing" it would
	// change the image for a reason that has nothing to do with Linux.
	color += texture(tSrc, p + vec2(-1, 0) * fD).rgb;
	color += texture(tSrc, p + vec2(-1, 0) * fD).rgb;
	oColor = vec4(color * 0.2, 1);
}
