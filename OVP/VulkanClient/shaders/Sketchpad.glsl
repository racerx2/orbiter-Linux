// =================================================================================================================================
// Sketchpad.glsl -- converted from OVP/D3D9Client/shaders/Sketchpad.fx
//
// The MIT Lisence:
//
// Copyright (C) 2013-2026 Jarmo Nikkanen
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy,
// modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software
// is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
// IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// =================================================================================================================================
//
// The seven general conventions are in IPI.glsl; the effect-file ones -- ONE
// shared uniform block at binding 0, samplers from binding 1, `_EP_<entry>`
// guarding each interpolant set, and the techniques and sampler_state blocks
// in Sketchpad.tech -- are in VulkanClient.glsl's header.
//
// `sampler TexS : register(s0)` and its two siblings lose their explicit
// register assignments, which pinned them to D3D9 sampler slots 0..2. Here
// the binding is declared below and VulkanEffectFile reads it back from
// reflection, so nothing has to agree by convention. Their sampler_state is
// in Sketchpad.tech, with the `Texture = <gTex0>` line that lets
// VulkanPad::eTex0 keep setting them by texture name.
//
// THE COLOUR SWIZZLES ARE UNCHANGED, and they have to be. `v.clr.bgra` reads
// a D3DCOLOR vertex element -- a DWORD written 0xAARRGGBB, which in memory is
// the bytes B,G,R,A, which is exactly VK_FORMAT_B8G8R8A8_UNORM. So the shader
// input arrives as (R,G,B,A) here as it did there, and .bgra means the same
// thing. See the note on D3DDECLTYPE_D3DCOLOR in VulkanUtil.h.
// =================================================================================================================================

#version 450
#extension GL_EXT_scalar_block_layout : require


// ----------------------------------------------------------------------------
// Sketchpad Implementation
//
// The parameter block: every `uniform extern` of Sketchpad.fx, in its order.
// The three texture parameters are not here -- a texture without a sampler
// cannot be declared in GLSL -- they appear below as the samplers that read
// them, and Sketchpad.tech carries the join.
// ----------------------------------------------------------------------------
layout(set = 0, binding = 0, scalar) uniform SketchpadBlock
{
	mat4	 gColorMatrix;
	mat4     gVP;			    // Projection matrix
	mat4     gW;			    // World matrix
	mat4     gWVP;				// World View Projection

	// Colors
	vec4     gPen;
	vec4     gKey;
	vec4     gMtrl;
	vec4     gNoiseColor;
	vec4     gGamma;

	vec3     gPos;				// Clipper sphere direction [unit vector]
	vec3     gPos2;				// Clipper cone direction [unit vector]
	vec4     gCov;				// Clipper sphere coverage parameters
	vec4     gSize;				// Inverse Texture size in .xy [pixels]
	vec4     gTarget;			// Inverse Screen size in .xy [pixels], Screen Size in .zw [pixels]
	vec3	 gWidth;			// Pen width in .x, and pattern scale in .y, pixel offset in .z
	float	 gFov;				// atan( 2 * tan(fov/2) / H )
	float	 gRandom;
	bool     gDashEn;
	bool     gTexEn;
	bool	 gFntEn;
	bool     gKeyEn;
	bool     gWide;				// Unused
	bool     gShade;
	bool     gClipEn;
	bool	 gClearEn;			// Unused
	bool	 gEffectsEn;
};


// ColorKey tolarance
#define tol 0.01f

layout(set = 0, binding = 1) uniform sampler2D TexS;		// <gTex0>
layout(set = 0, binding = 2) uniform sampler2D FntS;		// <gFnt0>
layout(set = 0, binding = 3) uniform sampler2D NoiseS;		// <gNoiseTex>


#define SSW 3	// Point side switch
#define TSW 2	// Fragment, Pen, Texture switch
#define CSW 1   // ColorKey, Font switch
#define LSW 0   // Length switch


#ifdef _FRAGMENT_SHADER
layout(location = 0) out vec4 oColor;
#endif


// ----------------------------------------------------------------------------
// struct OutputVS -- OrthoVS / Sketch3DVS -> SketchpadPS
//
//     float4 posH  : POSITION0;  -> gl_Position
//     float4 sw    : TEXCOORD0;  -> location 0
//     float4 tex   : TEXCOORD1;  -> location 1
//     float  len   : TEXCOORD2;  -> location 2
//     float4 posW  : TEXCOORD3;  -> location 3
//     float4 color : COLOR0;     -> location 4
// ----------------------------------------------------------------------------
#if defined(_EP_OrthoVS) || defined(_EP_Sketch3DVS) || defined(_EP_SketchpadPS)
	#define _IO_OutputVS 1
#endif

#if defined(_IO_OutputVS) && defined(_VERTEX_SHADER)
layout(location = 0) out vec4 vs_sk_sw;
layout(location = 1) out vec4 vs_sk_tex;
layout(location = 2) out float vs_sk_len;
layout(location = 3) out vec4 vs_sk_posW;
layout(location = 4) out vec4 vs_sk_color;
#endif

#if defined(_IO_OutputVS) && defined(_FRAGMENT_SHADER)
layout(location = 0) in vec4 ps_sk_sw;
layout(location = 1) in vec4 ps_sk_tex;
layout(location = 2) in float ps_sk_len;
layout(location = 3) in vec4 ps_sk_posW;
layout(location = 4) in vec4 ps_sk_color;
#endif


// ----------------------------------------------------------------------------
// struct SkpMshVS -- SketchMeshVS -> SketchMeshPS
//
//     float4 posH : POSITION0;  -> gl_Position
//     float3 nrmW : TEXCOORD0;  -> location 0
//     float2 tex  : TEXCOORD1;  -> location 1
// ----------------------------------------------------------------------------
#if defined(_EP_SketchMeshVS) || defined(_EP_SketchMeshPS)
	#define _IO_SkpMshVS 1
#endif

#if defined(_IO_SkpMshVS) && defined(_VERTEX_SHADER)
layout(location = 0) out vec3 vs_sm_nrmW;
layout(location = 1) out vec2 vs_sm_tex;
#endif

#if defined(_IO_SkpMshVS) && defined(_FRAGMENT_SHADER)
layout(location = 0) in vec3 ps_sm_nrmW;
layout(location = 1) in vec2 ps_sm_tex;
#endif


float cmax(vec3 v)
{
	return max(v.x, max(v.y, v.z));
}

// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------

#if defined(_EP_Sketch3DVS)

// struct InputVS (pSketchpadDecl)
//
//     float3 pos : POSITION0;   location 0, offset 0    vertex x, y
//     float4 dir : TEXCOORD0;   location 1, offset 12   Texture coord or inbound direction
//     float4 clr : COLOR0;      location 2, offset 28   Color      (D3DCOLOR)
//     float4 fnc : COLOR1;      location 3, offset 32   Function switch (D3DCOLOR)
layout(location = 0) in vec3 v_pos;
layout(location = 1) in vec4 v_dir;
layout(location = 2) in vec4 v_clr;
layout(location = 3) in vec4 v_fnc;

void Sketch3DVS()
{
	vec3 posW = (gW * vec4(v_pos.xy, 0.0f, 1.0f)).xyz;
	vec3 prvW = (gW * vec4(v_dir.zw, 0.0f, 1.0f)).xyz;
	vec3 nxtW = (gW * vec4(v_dir.xy, 0.0f, 1.0f)).xyz;

	vs_sk_len = v_pos.z;

	vec3 posN = normalize(posW);
	vec3 prvN = normalize(prvW);
	vec3 nxtN = normalize(nxtW);
	if (v_fnc[LSW]>0.5f) vs_sk_len = min(1.0f, length(posN-prvN)) / gFov;
	else				 vs_sk_len = v_pos.z;

	float fSide = round(v_fnc[SSW] * 2.0 - 1.0);
	float fPosD = dot(posN, posW);

	// THE SAME NaN GUARD AS OrthoVS, for the same reason -- see the long note
	// there. Sketchpad.fx:163-172 computes latN unconditionally and cancels it
	// with `* fSide`; normalize(nxtS + prvS) is 0/0 = NaN when the two are
	// opposite, and SPIR-V keeps NaN through a multiply by zero, which kills
	// the primitive.
	//
	// fSide IS HOISTED ABOVE THE COMPUTATION rather than left below it. That
	// is the only reordering: nothing between the two lines reads latN, and
	// where fSide is +/-1 the arithmetic is identical to the reference.
	//
	// Guarded here as well as in OrthoVS deliberately. Only the ortho path is
	// known to have been bitten, but this is the same expression fed by the
	// same vertex builders, and closing one shape of a defect is not closing
	// the class -- see section A-L of the porting notes.
	if (fSide != 0.0f) {
		vec3 nxtS = normalize(cross(nxtN - posN, posN));
		vec3 prvS = normalize(cross(posN - prvN, posN));
		vec3 latN = normalize(nxtS + prvS) * (0.45*gWidth.x) * inversesqrt(max(0.1, 0.5f + dot(nxtS, prvS)*0.5f));

		posW += latN * (fSide * fPosD * gFov);
	}

	vs_sk_color.rgba = v_clr.bgra;
	vs_sk_posW = vec4(posW, fPosD);
	vs_sk_sw = v_fnc;
	gl_Position = gVP * vec4(posW.xyz, 1.0f);
	vs_sk_tex = vec4(v_dir.xy * gSize.xy, v_dir.xy * gSize.zw);
}

#endif // _EP_Sketch3DVS



// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------

#if defined(_EP_OrthoVS)

// struct InputVS (pSketchpadDecl) -- see Sketch3DVS above
layout(location = 0) in vec3 v_pos;
layout(location = 1) in vec4 v_dir;
layout(location = 2) in vec4 v_clr;
layout(location = 3) in vec4 v_fnc;

void OrthoVS()
{
	vec4 posH = gWVP * vec4(v_pos.xy, 0.0f, 1.0f);
	vec4 prvH = gWVP * vec4(v_dir.zw, 0.0f, 1.0f);

	if (v_fnc[LSW]>0.5f) vs_sk_len = length((posH.xy - prvH.xy) * gTarget.zw * 0.5);
	else				 vs_sk_len = v_pos.z;

	vs_sk_posW = vec4(0.0f);

	if (gWide) {

		vec4 nxtH = gWVP * vec4(v_dir.xy, 0.0f, 1.0f);
		float fSide = round(v_fnc[SSW] * 2.0 - 1.0);
		vec2 pixH = gTarget.xy * gWidth.z * abs(fSide);

		nxtH.xy -= pixH;
		posH.xy -= pixH;
		prvH.xy -= pixH;

		// THE ONE PLACE THIS SHADER HAS TO DIVERGE FROM Sketchpad.fx, AND IT
		// IS FORCED BY SPIR-V's FLOATING-POINT RULES.
		//
		// Sketchpad.fx:205-209 computes latW unconditionally and cancels it
		// with `* fSide` on the last line. That is safe on D3D9 and NOT safe
		// here, because normalize(float2(0,0)) is 0/0 = NaN and SPIR-V
		// defines NaN * 0 as NaN. A NaN reaching gl_Position discards the
		// whole primitive, so the draw silently produces no fragments at all.
		//
		// nxtS + prvS IS EXACTLY ZERO ON A COPY/STRETCH QUAD, not merely
		// close to it. SkpVtxII (CopyRect, StretchRect, ColorKey) writes the
		// SOURCE TEXCOORD into nx,ny and never writes px,py -- so for the
		// first vertex "next" and "previous" sit on opposite sides of the
		// position and prvS comes out as exactly -nxtS. Their sum is (0,0)
		// and normalize gives NaN every time.
		//
		// WHAT IT COST: the Delta-glider's registration panel. The DG builds
		// it by blitting idpanel1.dds into a render target and drawing the
		// vessel name over it; because that .dds is DXT1, clbkScaleBlt cannot
		// take either BlitTexture path (both are gated on !bSC) and is forced
		// onto the Sketchpad fallback, which lands here. The lettering drew --
		// text vertices carry real prev/next points, so no NaN -- and the
		// background did not, leaving a black rectangle on each wing with a
		// clean log from top to bottom. Measured: a correct full-size quad
		// (v0 (-0.5,-0.5) .. v2 (255.5,255.5), fnc=800000FF, vmode=ORTHO,
		// blend=ALPHABLEND, cull NONE) that produced zero fragments and left
		// the target's previous contents perfectly intact.
		//
		// The guard is not an approximation of the reference: fSide is 0, +1
		// or -1, and where it is +/-1 this computes exactly what the line
		// above computed. Only the case the reference meant to cancel is
		// skipped, and it is skipped before the NaN can be created rather
		// than after.
		if (fSide != 0.0f) {
			vec2 nxtS = normalize(nxtH.xy - posH.xy);
			vec2 prvS = normalize(posH.xy - prvH.xy);
			vec2 latW = normalize(nxtS + prvS) * (0.45*gWidth.x) * inversesqrt(max(0.1, 0.5f + dot(nxtS, prvS)*0.5f));

			posH += vec4(latW.y, -latW.x, 0, 0) * gTarget * fSide;
		}
	}

	vs_sk_color.rgba = v_clr.bgra;
	vs_sk_sw = v_fnc;
	gl_Position = vec4(posH.xyz, 1.0f);

	vs_sk_tex = vec4(v_dir.xy * gSize.xy, v_dir.xy * gSize.zw);
}

#endif // _EP_OrthoVS





// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------

#if defined(_EP_SketchpadPS)

// `float4 sc : VPOS` -> gl_FragCoord
void SketchpadPS()
{
	vec4 t = vec4(1.0f);
	vec3 u = vec3(1.0f);

	if (gFntEn)	u = texture(FntS, ps_sk_tex.zw).rgb;
	if (gTexEn) t = texture(TexS, ps_sk_tex.xy);

	float f = max(u.r*0.7f, u.g);

	// Select Color source
	vec4 c = ps_sk_color;
	if (ps_sk_sw[TSW] > 0.2f) c = gPen;
	if (ps_sk_sw[TSW] > 0.8f) c = t;
	if (ps_sk_sw[CSW] > 0.8f) c.a *= f;

	// Color keying
	if (gTexEn && gKeyEn) {
		vec4 x = abs(c - gKey);
		if ((x.r < tol) && (x.g < tol) && (x.b < tol)) {
			if (ps_sk_sw[CSW] > 0.2f && ps_sk_sw[CSW] < 0.8) discard;
		}
	}

	if (gDashEn) {
		float q;
		// HLSL's modf(x, out ip) and GLSL's modf(x, out i) are the same
		// function with the same argument order: the fractional part is the
		// return value and the integral part goes to the out parameter.
		if (modf(ps_sk_len*gWidth.y, q) > 0.5f) discard;
	}

	if (gClipEn) {
		vec3 posN = normalize(ps_sk_posW.xyz);
		if ((dot(gPos,  posN) > gCov.x) && (ps_sk_posW.w > gCov.y)) discard;
		if ((dot(gPos2, posN) > gCov.z) && (ps_sk_posW.w > gCov.w)) discard;
	}

	if (gEffectsEn) {

		// Apply color matrix
		c = gColorMatrix * c;

		// Apply gamma correction
		c.rgb = pow(max(c.rgb, vec3(0.0f)), gGamma.rgb);

		// Color overboost correction beyond 0-1 range
		c.rgb += clamp(cmax(c.rgb) - 1.0f, 0.0f, 1.0f);

		// Apply noise
		float noise = (texture(NoiseS, gl_FragCoord.xy*(1.0f / 128.0f) + vec2(gRandom, gRandom*7.0)).r * 2.0f) - 1.0f;
		c.rgb += mix(vec3(1, 1, 1), c.rgb, gNoiseColor.a) * gNoiseColor.rgb * noise;
	}

	oColor = clamp(c, 0.0f, 1.0f);
}

#endif // _EP_SketchpadPS



// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------

#if defined(_EP_SketchMeshVS)

// struct NTVERTEX -- D3D9Client Mesh vertex layout (pNTVertexDecl)
layout(location = 0) in vec3 posL;		// POSITION0
layout(location = 1) in vec3 nrmL;		// NORMAL0
layout(location = 2) in vec2 tex0;		// TEXCOORD0

void SketchMeshVS()
{
	vec3 posW = (gW * vec4(posL, 1.0f)).xyz;
	vec3 nrmW = (gW * vec4(nrmL, 0.0f)).xyz;
	gl_Position = gVP * vec4(posW, 1.0f);
	vs_sm_tex = tex0;
	vs_sm_nrmW = nrmW;
}

#endif // _EP_SketchMeshVS


#if defined(_EP_SketchMeshPS)
void SketchMeshPS()
{
	vec4 cTex = vec4(1.0f);
	float fS = 1.0f;
	if (gTexEn) cTex = texture(TexS, ps_sm_tex);
	if (gShade) fS = dot(normalize(ps_sm_nrmW), vec3(0, 0, -1));

	cTex.rgba *= gPen.bgra;
	cTex.rgba *= gMtrl.rgba;
	cTex.rgb  *= clamp(fS, 0.0f, 1.0f);

	oColor = cTex;
}
#endif // _EP_SketchMeshPS
