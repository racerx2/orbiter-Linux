// ===========================================================================
// SceneTech.glsl -- converted from OVP/D3D9Client/shaders/SceneTech.fx
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2012 - 2026 Jarmo Nikkanen
// ===========================================================================
//
// The seven general conventions are in IPI.glsl; the effect-file ones (one
// shared uniform block at binding 0, samplers from binding 1, `_EP_<entry>`
// guarding each interpolant set, the techniques and sampler_state blocks in
// the .tech) are in VulkanClient.glsl's header. Only what is particular to
// this file is noted here.
//
// `sampler Tex0S : register (s0)` -- the explicit register assignment has no
// counterpart and needs none. It pinned the sampler to D3D9's slot 0; here
// the binding is declared below and the client reads it back from reflection,
// so nothing has to agree by convention.
// ===========================================================================

#version 450
#extension GL_EXT_scalar_block_layout : require


// ----------------------------------------------------------------------------
// The parameter block. Every `uniform extern` of SceneTech.fx, in its order.
//
// gTex0 is not here: a texture parameter is a descriptor rather than bytes,
// and in GLSL a texture without a sampler cannot be declared at all. It
// appears below as Tex0S, and SceneTech.tech carries the join.
// ----------------------------------------------------------------------------
layout(set = 0, binding = 0, scalar) uniform SceneTechBlock
{
	mat4     gWVP;			    // Combined World, View and Projection matrix
	mat4     gVP;			    // Combined World, View and Projection matrix
	vec4     gColor;		    // Line Color
};

layout(set = 0, binding = 1) uniform sampler2D Tex0S;	// <gTex0>


#ifdef _FRAGMENT_SHADER
layout(location = 0) out vec4 oColor;
#endif


// ----------------------------------------------------------------------------
// Line Tech Vertex/Pixel shader implementation
// ----------------------------------------------------------------------------

// struct LineOutputVS { float4 posH : POSITION0; } -- gl_Position and nothing
// else, so this pair has no interpolants at all.

#if defined(_EP_LineTechVS)

// `float3 posL : POSITION0` (pPositionDecl)
layout(location = 0) in vec3 posL;		// POSITION0

void LineTechVS()
{
	gl_Position = gWVP * vec4(posL, 1.0f);
}

#endif // _EP_LineTechVS


#if defined(_EP_LineTechPS)
void LineTechPS()
{
	oColor = gColor;
}
#endif


// ----------------------------------------------------------------------------
// Star rendering technique
// ----------------------------------------------------------------------------

// struct StarOutputVS
//
//     float4 posH : POSITION0;  -> gl_Position
//     float4 col  : COLOR0;     -> location 0
#if defined(_EP_StarTechVS) || defined(_EP_StarTechPS)
	#define _IO_StarOutputVS 1
#endif

#if defined(_IO_StarOutputVS) && defined(_VERTEX_SHADER)
layout(location = 0) out vec4 vs_star_col;
#endif

#if defined(_IO_StarOutputVS) && defined(_FRAGMENT_SHADER)
layout(location = 0) in vec4 ps_star_col;
#endif


#if defined(_EP_StarTechVS)

// `float3 posL : POSITION0, float4 col : COLOR0` (pPosColorDecl)
//
// The colour is a D3DCOLOR -- one DWORD, 0xAARRGGBB -- read by the vertex
// declaration as VK_FORMAT_B8G8R8A8_UNORM, so it arrives normalised and in
// RGBA order, which is what the D3D9 vertex shader saw.
layout(location = 0) in vec3 posL;		// POSITION0
layout(location = 1) in vec4 col;		// COLOR0

void StarTechVS()
{
	gl_Position = gWVP * vec4(posL, 1.0f);
	vs_star_col = col;

	// The reference writes no PSIZE and nothing sets D3DRS_POINTSIZE for this
	// draw, so D3D9 used the render state's default of 1.0 -- one-pixel stars,
	// which is what the sky looked like on Windows. Vulkan has no such device
	// state and REQUIRES the shader to say it for a POINT_LIST pipeline:
	//
	//     VUID-VkGraphicsPipelineCreateInfo-topology-08773
	//     ... topology is VK_PRIMITIVE_TOPOLOGY_POINT_LIST and the shader
	//     does not write PointSize.
	//
	// So the D3D9 default is written out. It is a conversion of the default,
	// not a new decision.
	gl_PointSize = 1.0f;
}

#endif // _EP_StarTechVS


#if defined(_EP_StarTechPS)
void StarTechPS()
{
	oColor = ps_star_col;
}
#endif


// ----------------------------------------------------------------------------
// Label technique
// ----------------------------------------------------------------------------

// struct LabelVS
//
//     float4 posH : POSITION0;  -> gl_Position
//     float2 tex0 : TEXCOORD0;  -> location 0
#if defined(_EP_LabelTechVS) || defined(_EP_LabelTechPS)
	#define _IO_LabelVS 1
#endif

#if defined(_IO_LabelVS) && defined(_VERTEX_SHADER)
layout(location = 0) out vec2 vs_lbl_tex0;
#endif

#if defined(_IO_LabelVS) && defined(_FRAGMENT_SHADER)
layout(location = 0) in vec2 ps_lbl_tex0;
#endif


#if defined(_EP_LabelTechVS)

// `float3 posL : POSITION0, float2 tex0 : TEXCOORD0` (pPosTexDecl)
layout(location = 0) in vec3 posL;		// POSITION0
layout(location = 1) in vec2 tex0;		// TEXCOORD0

void LabelTechVS()
{
	gl_Position = gWVP * vec4(posL, 1.0f);
	vs_lbl_tex0 = tex0;
}

#endif // _EP_LabelTechVS


#if defined(_EP_LabelTechPS)
void LabelTechPS()
{
	vec4 col;
	col = texture(Tex0S, ps_lbl_tex0);
	col.rgb = gColor.rgb;
	oColor = col;
}
#endif
