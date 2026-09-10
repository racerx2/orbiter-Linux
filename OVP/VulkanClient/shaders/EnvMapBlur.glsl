// ==============================================================
// EnvMapBlur.glsl -- converted from
// OVP/D3D9Client/shaders/EnvMapBlur.hlsl (41 lines)
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// ==============================================================
//
// An ImageProcessing pixel shader, built by Scene::RenderBlurredMap as
// `new ImageProcessing(pDev, "Modules/VulkanClient/EnvMapBlur.glsl", "PSBlur")`
// -- so the vertex half is IPI.glsl's VSMain. Conventions: see IPI.glsl.
//
// `texCUBE(s, dir)` BECOMES `texture(s, dir)` ON A `samplerCube`. HLSL had a
// separate intrinsic per sampler dimension (tex2D, texCUBE, tex3D); GLSL
// overloads `texture()` on the sampler type, so the dimension is carried by
// the declaration instead of by the call. That is also why the declaration
// has to say `samplerCube` where the .hlsl said only `sampler` -- HLSL
// inferred it from the intrinsic.
//
// TWO DECLARATIONS IN THE REFERENCE ARE DEAD AND ARE KEPT AS WRITTEN.
// `sampler tSrc` is never sampled by PSBlur, and the `coeff[8]` table is
// never read. Both are the author's and both are left in place; glslang
// reports only LIVE uniforms, so neither reaches the descriptor set layout
// and neither costs anything. Deleting them would be a change to the
// author's code rather than a conversion.
// ==============================================================

#version 450
#extension GL_EXT_scalar_block_layout : require

// uniform extern float3 vDir, vUp, vCp; float fD; bool bDir;
//
// All five are written by name from Scene::RenderBlurredMap. A `bool` in a
// uniform block is 32 bits, which is what ImageProcessing::SetBool writes --
// it widens the C++ bool to an int before the copy, exactly as
// ID3DXConstantTable::SetBoolArray took a BOOL.
layout(set = 0, binding = 1, scalar) uniform EnvMapBlurPSBlock
{
	vec3  vDir;
	vec3  vUp;
	vec3  vCp;
	float fD;
	bool  bDir;
};

layout(set = 0, binding = 2) uniform samplerCube tCube;
layout(set = 0, binding = 3) uniform sampler2D   tSrc;		// declared, never sampled

// static float coeff[8] = { ... } -- declared and never read; see the header.
const float coeff[8] = float[8](0.13298076, 0.125794409, 0.106482669, 0.080656908,
								0.054670025, 0.033159046, 0.017996989, 0.00874063);

layout(location = 0) in float psX;		// TEXCOORD0
layout(location = 1) in float psY;		// TEXCOORD1

layout(location = 0) out vec4 oColor;

void PSBlur()
{
	// The reference assigns back into its x and y parameters. GLSL's stage
	// inputs are not writable, so the two get locals; the arithmetic is
	// unchanged.
	float x = psX * 2.0f - 1.0f;
	float y = psY * 2.0f - 1.0f;

	vec3 vD;

	vec3 dir = vDir - vUp * y + vCp * x;

	if (bDir) vD = cross(dir, vCp);
	else	  vD = cross(dir, vUp);

	vD = normalize(vD) * fD;

	vec3 color = texture(tCube, dir).rgb;
	vec3 vX = vec3(0.0f);
	float f = 0.75f;
	float a = 0.5f;

	for (int i = 1; i < 16; i++) {
		vX += vD;
		color += f * texture(tCube, dir + vX).rgb;
		color += f * texture(tCube, dir - vX).rgb;
		a += f;
		f *= 0.75f;
	}
	color /= (a * 2.0f);
	oColor = vec4(color, 1);
}
