// ==============================================================
// GDIOverlay.glsl -- converted from
// OVP/D3D9Client/shaders/GDIOverlay.hlsl (18 lines)
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2016-2026 Jarmo Nikkanen
// ==============================================================
//
// An ImageProcessing pixel shader: Scene.cpp builds it as
// `new ImageProcessing(pDevice, "Modules/VulkanClient/GDIOverlay.glsl",
// "PSMain")`, so its vertex half is IPI.glsl's VSMain and the two
// interpolants arrive at locations 0 and 1. The conventions are written out
// once in IPI.glsl.
//
// `clip(-1)` BECOMES `discard`. clip(x) is HLSL's "kill this fragment if x is
// negative", and the constant -1 makes it unconditional inside the branch --
// so the whole construct is exactly `discard`. GLSL has no clip() and needs
// none.
//
// The colour key comparison is unchanged. Note that it is `abs(vClr -
// vColorKey.rgb)` against a tolerance, not an equality: the overlay is
// keyed on 0xF08040 (see Scene's constructor) and the surface it comes from
// has been through a filter, so an exact match would let the key show
// through.
// ==============================================================

#version 450
#extension GL_EXT_scalar_block_layout : require

// uniform extern float4 vColorKey;
//
// Written by Scene::Render2DOverlay with
// pGDIOverlay->SetFloat("vColorKey", &clr, sizeof(clr)).
layout(set = 0, binding = 1, scalar) uniform GDIOverlayPSBlock
{
	vec4 vColorKey;
};

layout(set = 0, binding = 2) uniform sampler2D tSrc;

#define tol 0.02

layout(location = 0) in float psX;		// TEXCOORD0
layout(location = 1) in float psY;		// TEXCOORD1

layout(location = 0) out vec4 oColor;

void PSMain()
{
	vec3 vClr = texture(tSrc, vec2(psX, psY)).rgb;
	vec3 c = abs(vClr - vColorKey.rgb);
	if (c.r < tol && c.g < tol && c.b < tol) discard;
	oColor = vec4(vClr, 1.0f);
}
