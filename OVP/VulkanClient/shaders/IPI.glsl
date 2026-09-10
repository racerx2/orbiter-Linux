// ===========================================================================
// IPI.glsl -- converted from OVP/D3D9Client/shaders/IPI.hlsl (39 lines)
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// ===========================================================================
//
// THE FIRST SHADER CONVERTED, so the conventions the other twenty-two follow
// are written out here once. Every one of them was CHECKED against glslang
// rather than assumed; where a check settled something, it says so.
//
//  1. ENTRY POINTS KEEP THEIR NAMES. glslang accepts a GLSL source whose
//     entry point is not called `main`, selected with setSourceEntryPoint() --
//     and picks the right one out of a file that declares several. Verified
//     by compiling a two-entry-point source and confirming the SPIR-V is the
//     one asked for, not the first. That is what lets a .tech file name
//     `VSMain` and `PSBlur` the way the .fx did, and it is why the technique
//     tables convert verbatim.
//
//  2. `mul(v, M)` BECOMES `M * v`. HLSL's mul() with the vector first is a
//     ROW-vector times a matrix. GLSL's mat4 is column-major, so uploading
//     the client's row-major FMATRIX4 bytes unchanged makes the GLSL matrix
//     the transpose of the C++ one -- and `M_glsl * v` is then exactly
//     `mul(v, M_cpp)`. Nothing is transposed on the C++ side; the storage
//     order does the work. A matrix-matrix `mul(A, B)` reverses for the same
//     reason and becomes `B * A`.
//
//  3. ONE UNIFORM BLOCK PER STAGE, `layout(scalar)`, at a fixed binding:
//     the vertex stage's at binding 0 and the pixel stage's at binding 1 for
//     an ImageProcessing shader; ONE block shared by both stages at binding 0
//     for a .tech effect (that convention was fixed earlier -- see
//     VulkanEffect.h). Samplers follow, from binding 2 and binding 1
//     respectively, in declaration order.
//
//     `scalar` and not std140: the client writes whole structs at the offset
//     reflection reports, so the block has to pack the way C++ does.
//     Confirmed against glslang -- a vec3 followed by a float reflects at
//     offsets 80 and 92, with no std140 pad to 96.
//
//     ONE block per stage, because offsets are per-block and the client
//     stages one buffer per stage; two live blocks would overlap in it.
//     glslang reports only LIVE uniforms -- a block an entry point does not
//     reference is not listed at all, checked -- so declaring both blocks in
//     one file is safe as long as neither stage reads the other's.
//
//  4. VERTEX INPUT LOCATION = THE ELEMENT'S INDEX IN THE C++ DECLARATION.
//     `pPosTexDecl` is {POSITION0 vec3 @0, TEXCOORD0 vec2 @12}, so location 0
//     is the position and location 1 the texture coordinate.
//
//  5. `POSITION0` OUT BECOMES `gl_Position`; a `: COLOR` return becomes a
//     `layout(location = 0) out vec4`. Interpolants keep their order:
//     TEXCOORDn becomes `layout(location = n)`, matched between the stages.
//
//  6. `tex2D(s, uv)` BECOMES `texture(s, uv)`; a bare HLSL `sampler` becomes
//     a `sampler2D`, because Vulkan GLSL has no separate sampler and texture
//     the way D3D9 did -- a combined image sampler is one descriptor.
//
//  7. `_VERTEX_SHADER` / `_FRAGMENT_SHADER` GUARD THE TWO HALVES OF A FILE
//     THAT HOLDS BOTH STAGES, and that is a language difference rather than a
//     Vulkan one. HLSL passes a stage's inputs as FUNCTION PARAMETERS, so
//     nothing about one entry point is visible to another; GLSL declares them
//     as file-scope globals with explicit locations, so the pixel stage's
//     interpolants would be read as vertex ATTRIBUTES when the vertex stage
//     is built -- a location conflict with the real ones. The define comes
//     from CompileShaderStage's preamble; see the note there.
//
// WHAT THIS FILE IS. IPI.hlsl is ImageProcessing's DEFAULT vertex shader: the
// one IProcess.cpp compiles when a caller supplies no _vsentry of its own, so
// its VSMain is paired with a pixel shader from a different file. PSMain is
// the author's example and is kept -- the reference's own comment calls it
// "Example of pixel shader" -- but nothing selects it.
// ===========================================================================

#version 450
#extension GL_EXT_scalar_block_layout : require

// uniform extern float4x4 mVP; float4 vTgtSize; float4 vPos;
//
// The three the C++ writes by name in ImageProcessing::SetupViewPort, through
// hVP, hSiz and hPos. Their order here is the order the reference declares
// them; nothing depends on it, because every write goes to the offset
// reflection reports.
layout(set = 0, binding = 0, scalar) uniform IPIVSBlock
{
	mat4 mVP;
	vec4 vTgtSize;
	vec4 vPos;
};


#ifdef _VERTEX_SHADER

// struct OutputVS { float4 posH : POSITION0; float x : TEXCOORD0; float y : TEXCOORD1; }
//
// posH is gl_Position. The two floats stay two separate interpolants rather
// than being folded into a vec2: the pixel shaders that pair with this vertex
// shader take them as `float x : TEXCOORD0, float y : TEXCOORD1`, and folding
// them would change the interface every one of those files is written to.
layout(location = 0) out float vsX;
layout(location = 1) out float vsY;

layout(location = 0) in vec3 posL;		// POSITION0
layout(location = 1) in vec2 tex0;		// TEXCOORD0

void VSMain()
{
	// The `OutputVS outVS = (OutputVS)0;` zero-initialise has no counterpart
	// and needs none: every member is assigned below, and GLSL has no
	// struct-wide cast-from-zero.

	vec2 p = posL.xy;

	p *= vPos.xy;
	p += vPos.zw;
	p *= vTgtSize.xy;

	// mul(float4(posL.xy-0.5f, 0, 1), mVP). See note 2.
	//
	// THE -0.5f IS GONE, AND IT IS THE ONE LINE IN THIS FILE THAT COULD NOT
	// BE CARRIED OVER. It is the Direct3D 9 HALF-PIXEL OFFSET, and it exists
	// only because of a D3D9 rasterisation rule that Vulkan does not share --
	// which is exactly the case the conversion is allowed to depart on.
	//
	// D3D9 maps NDC to the screen such that an integer screen coordinate
	// lands on a pixel CENTRE, so a quad drawn over [0,W] covers centres
	// 0..W-1 only if it is first shifted back by half a pixel. Every D3D9
	// program that blits one texel to one pixel carries this subtraction for
	// that reason, and the reference is no exception.
	//
	// Vulkan (like D3D10 and after) puts pixel centres at i+0.5 and maps NDC
	// [-1,1] onto framebuffer [0,W] exactly, so a full-target quad already
	// covers every centre. Applying the D3D9 shift on top moves the quad half
	// a pixel off the target, and the top-left fill rule then drops the LAST
	// ROW AND COLUMN entirely -- they are never rasterised and keep whatever
	// the image was cleared to, which is nothing.
	//
	// That is not cosmetic here. ImageProcessing draws every atmospheric
	// lookup table this way, and Scatter.glsl reads them with CLAMPed
	// coordinates that saturate to exactly v = 1.0 -- the dead row:
	//
	//     GetSunColor()  ->  texture(tSun, vec2(dir, alt))   with dir, alt
	//                        both clamp()ed to [0,1]
	//
	// so every in-scatter sample returned zero. IntegrateSegmentNS multiplies
	// by it, so LandViewRay, LandViewMie and MieSkyView came out ALL ZERO and
	// RaySkyView held only its ambient term. Measured: every table's last row
	// read 0.0000 while the row before it was 0.203. With the tables empty the
	// terrain is lit by the ambient approximation alone and the daytime sky
	// renders black -- which is what the window showed.
	gl_Position = mVP * vec4(p, 0.0f, 1.0f);
	vsX = tex0.x;
	vsY = tex0.y;
}

#endif // _VERTEX_SHADER


#ifdef _FRAGMENT_SHADER

// Example of pixel shader

layout(set = 0, binding = 2) uniform sampler2D mySmp;

layout(location = 0) in float psX;		// TEXCOORD0
layout(location = 1) in float psY;		// TEXCOORD1

layout(location = 0) out vec4 oColor;

void PSMain()
{
	oColor = texture(mySmp, vec2(psX, psY));
}

#endif // _FRAGMENT_SHADER
