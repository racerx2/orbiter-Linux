// ===========================================================================================
// VulkanEffect.cpp
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2011-2026 Jarmo Nikkanen
// ===========================================================================================
//
// CONVERTED FROM OVP/D3D9Client/D3D9Effect.cpp, read end to end (1022 lines),
// against D3D9Effect.h (193) and the technique blocks of all eleven .fx files,
// each read end to end.
//
// THIS FILE IS WHERE D3DX DID THE MOST WORK IN THE WHOLE CLIENT, so it is
// worth stating exactly what is being replaced and what is not.
//
// D3DXCreateEffectFromFileA compiles ONE .fx file into an object holding
//   * TECHNIQUES -- named, each a list of PASSES, each pass naming a vertex
//     shader entry point, a pixel shader entry point, and a block of render
//     state (AlphaBlendEnable, SrcBlend, DestBlend, ZEnable, ZWriteEnable,
//     CullMode, StencilEnable...);
//   * PARAMETERS -- named, declared at file scope, SHARED BY EVERY TECHNIQUE.
//
// Vulkan has no such object and no equivalent library. But the .fx's
// structure is not D3D-specific: it is real content that the shaders and
// roughly seven hundred call sites are written against. So the structure
// survives and the machinery underneath it is rebuilt:
//
//   a technique  -> VulkanTechnique: a list of passes, each a shader pair,
//                   the pass's state, and a VkPipeline cache keyed on what
//                   the caller supplies at bind time (vertex layout,
//                   topology).
//   a parameter  -> a byte range in ONE effect-wide uniform block, or a
//                   descriptor binding for a texture. Effect-wide because
//                   that is what the .fx already declares -- see the note on
//                   `Params` in VulkanEffect.h.
//   SetTechnique -> select which pipeline set the next draws use.
//   BeginPass    -> the point at which every input is finally known, so this
//                   is where the pipeline is built or fetched, the uniform
//                   block is uploaded and the descriptor set is written.
//   CommitChanges-> nothing. D3DX needed it because parameters set after
//                   BeginPass would not otherwise reach the device; here the
//                   upload happens at BeginPass, so there is nothing pending.
//
// THE TECHNIQUE TABLE STAYS A FILE, and that is deliberate. The `technique {
// pass { ... } }` blocks are data -- twenty-six techniques, thirty-eight
// passes, each naming two entry points and six to eleven state values -- and
// D3DX read them from the .fx at load time. Baking them into a C++ table here
// would make a shader change a client rebuild. So they are copied verbatim
// into Modules/VulkanClient/VulkanClient.tech and this file parses them. Same
// data, same syntax, same load-time binding; only the reader changed.
//
// D3DXMACRO becomes a preprocessor option string. glslang takes the same
// -D definitions, so the six computed macros and the five conditional ones
// carry over one for one.
//
// DrawIndexedPrimitiveUP / DrawPrimitiveUP -- "draw from this pointer in my
// memory" -- have no Vulkan counterpart and cannot: a draw sources vertices
// from a VkBuffer bound to the command buffer. DrawUP() copies into a
// per-frame scratch buffer, which is what the D3D9 runtime did behind that
// call anyway. The one thing that moves is the PRIMITIVE TOPOLOGY: it was an
// argument to the draw and is now pipeline state, so it is declared with the
// vertex layout before Begin(). See SetTopology in VulkanEffect.h.
//
// pDev->SetRenderState(D3DRS_DESTBLEND, ...) inside Render2DPanel is the same
// story one level down: an independent piece of device state on Windows, part
// of the pipeline here, so `additive` becomes part of the pipeline key rather
// than a call between BeginPass and the draw.
// ===========================================================================================

#include "VulkanEffect.h"
#include "Log.h"
#include "Scene.h"
#include "VulkanSurface.h"
#include "VulkanConfig.h"
#include "Mesh.h"
#include "VectorHelpers.h"
#include "VulkanUtil.h"

#include <vector>
#include <string>
#include <string.h>

VulkanClient    * VulkanEffect::gc = 0;
VulkanEffectFile* VulkanEffect::FX = 0;
VulkanBuffer    * VulkanEffect::VB = 0;
FVECTOR4          VulkanEffect::atm_color;		// Earth glow color

VulkanMatExt VulkanEffect::mfdmat;
VulkanMatExt VulkanEffect::defmat;
VulkanMatExt VulkanEffect::night_mat;
VulkanMatExt VulkanEffect::emissive_mat;

// Some general rendering techniques
TECHHANDLE VulkanEffect::ePanelTech = 0;		// Used to draw a new style 2D panel
TECHHANDLE VulkanEffect::ePanelTechB = 0;		// Used to draw a new style 2D panel
TECHHANDLE VulkanEffect::eVesselTech = 0;		// Vessel exterior, surface bases.
TECHHANDLE VulkanEffect::eBBTech = 0;			// Bounding Box Tech
TECHHANDLE VulkanEffect::eTBBTech = 0;
TECHHANDLE VulkanEffect::eBSTech = 0;			// Bounding Sphere Tech
TECHHANDLE VulkanEffect::eSimple = 0;
TECHHANDLE VulkanEffect::eBaseShadowTech = 0;	// Used to draw transparent surface without texture
TECHHANDLE VulkanEffect::eBeaconArrayTech = 0;
TECHHANDLE VulkanEffect::eExhaust = 0;	// Render engine exhaust texture
TECHHANDLE VulkanEffect::eSpotTech = 0;	// Vessel beacons
TECHHANDLE VulkanEffect::eBaseTile = 0;
TECHHANDLE VulkanEffect::eRingTech = 0;	// Planet rings technique
TECHHANDLE VulkanEffect::eRingTech2 = 0;	// Planet rings technique
TECHHANDLE VulkanEffect::eShadowTech = 0; // Vessel ground shadows
TECHHANDLE VulkanEffect::eArrowTech = 0;  // (Grapple point) arrows
TECHHANDLE VulkanEffect::eAxisTech = 0;
TECHHANDLE VulkanEffect::eSimpMesh = 0;
TECHHANDLE VulkanEffect::eGeometry = 0;

// Planet Rendering techniques
TECHHANDLE VulkanEffect::ePlanetTile = 0;
TECHHANDLE VulkanEffect::eCloudTech = 0;
TECHHANDLE VulkanEffect::eCloudShadow = 0;
TECHHANDLE VulkanEffect::eSkyDomeTech = 0;
TECHHANDLE VulkanEffect::eHazeTech = 0;

// Particle effect texhniques
TECHHANDLE VulkanEffect::eDiffuseTech = 0;
TECHHANDLE VulkanEffect::eEmissiveTech = 0;


HANDLE VulkanEffect::eVP = 0;			// Combined View & Projection Matrix
HANDLE VulkanEffect::eW = 0;			// World matrix
HANDLE VulkanEffect::eLVP = 0;			// Light view projection
HANDLE VulkanEffect::eGT = 0;			// Mesh group transformation matrix
HANDLE VulkanEffect::eMat = 0;			// Material
HANDLE VulkanEffect::eWater = 0;		// Water
HANDLE VulkanEffect::eMtrl = 0;
HANDLE VulkanEffect::eTune = 0;
HANDLE VulkanEffect::eSun = 0;
HANDLE VulkanEffect::eNight = 0;
HANDLE VulkanEffect::eLights = 0;		// Additional light sources

HANDLE VulkanEffect::eTex0 = 0;			// Primary texture
HANDLE VulkanEffect::eTex1 = 0;			// Secondary texture
HANDLE VulkanEffect::eTex3 = 0;			// Tertiary texture
HANDLE VulkanEffect::eSpecMap = 0;
HANDLE VulkanEffect::eEmisMap = 0;
HANDLE VulkanEffect::eEnvMapA = 0;
HANDLE VulkanEffect::eEnvMapB = 0;
HANDLE VulkanEffect::eReflMap = 0;
HANDLE VulkanEffect::eRghnMap = 0;
HANDLE VulkanEffect::eMetlMap = 0;
HANDLE VulkanEffect::eHeatMap = 0;
HANDLE VulkanEffect::eShadowMap = 0;
HANDLE VulkanEffect::eTranslMap = 0;
HANDLE VulkanEffect::eTransmMap = 0;
HANDLE VulkanEffect::eIrradMap = 0;

HANDLE VulkanEffect::eSpecularMode = 0;
HANDLE VulkanEffect::eHazeMode = 0;
HANDLE VulkanEffect::eColor = 0;		// Auxiliary color input
HANDLE VulkanEffect::eFogColor = 0;		// Fog color input
HANDLE VulkanEffect::eTexOff = 0;		// Surface tile texture offsets
HANDLE VulkanEffect::eTime = 0;			// FLOAT Simulation elapsed time
HANDLE VulkanEffect::eMix = 0;			// FLOAT Auxiliary factor/multiplier
HANDLE VulkanEffect::eFogDensity = 0;	//
HANDLE VulkanEffect::ePointScale = 0;
HANDLE VulkanEffect::eSHD = 0;

HANDLE VulkanEffect::eAtmColor = 0;
HANDLE VulkanEffect::eProxySize = 0;
HANDLE VulkanEffect::eMtrlAlpha = 0;
HANDLE VulkanEffect::eKernel = 0;
HANDLE VulkanEffect::eAtmoParams = 0;

// Shader Flow Controls
HANDLE VulkanEffect::eFlow = 0;
HANDLE VulkanEffect::eModAlpha = 0;		// BOOL if true multiply material alpha with texture alpha
HANDLE VulkanEffect::eFullyLit = 0;		// BOOL
HANDLE VulkanEffect::eTextured = 0;		// BOOL
HANDLE VulkanEffect::eFresnel = 0;		// BOOL
HANDLE VulkanEffect::eSwitch = 0;		// BOOL
HANDLE VulkanEffect::eRghnSw = 0;		// BOOL
HANDLE VulkanEffect::eShadowToggle = 0;	// BOOL
HANDLE VulkanEffect::eEnvMapEnable = 0;	// BOOL
HANDLE VulkanEffect::eInSpace = 0;		// BOOL
HANDLE VulkanEffect::eNoColor = 0;		// BOOL
HANDLE VulkanEffect::eLightsEnabled = 0;// BOOL
HANDLE VulkanEffect::eTuneEnabled = 0;	// BOOL
HANDLE VulkanEffect::eBaseBuilding = 0;	// BOOL
HANDLE VulkanEffect::eOITEnable = 0;	// BOOL
// --------------------------------------------------------------
HANDLE VulkanEffect::eExposure = 0;
HANDLE VulkanEffect::eCameraPos = 0;
HANDLE VulkanEffect::eNorth = 0;
HANDLE VulkanEffect::eEast = 0;
HANDLE VulkanEffect::eDistScale = 0;
HANDLE VulkanEffect::eRadius = 0;
HANDLE VulkanEffect::eAttennuate = 0;
HANDLE VulkanEffect::eInScatter = 0;
HANDLE VulkanEffect::eInvProxySize = 0;
HANDLE VulkanEffect::eGlowConst = 0;
// --------------------------------------------------------------
HANDLE VulkanEffect::eGlobalAmb = 0;
HANDLE VulkanEffect::eSunAppRad = 0;
HANDLE VulkanEffect::eAmbient0 = 0;
HANDLE VulkanEffect::eDispersion = 0;

VulkanDevice *VulkanEffect::pDev = 0;

// Were D3DMATERIAL9: four D3DCOLORVALUE and a power. MATERIAL in
// OrbiterAPI.h:504 is four COLOUR4 and a float and is layout-identical, which
// is what lets CreateMatExt take it unchanged -- see VulkanUtil.h.
static MATERIAL _emissive_mat = {
	{0,0,0,1},
	{0,0,0,1},
	{0,0,0,1},
	{1,1,1,1},
	0.0
};

static MATERIAL _defmat = {
	{1,1,1,1},
	{1,1,1,1},
	{0,0,0,1},
	{0,0,0,1},10.0f
};

static MATERIAL _mfdmat = {
	{1,1,1,1},
	{1,1,1,1},
	{0,0,0,1},
	{1,1,1,1},10.0f
};

static MATERIAL _night_mat = {
	{1,1,1,1},
	{0,0,0,1},
	{0,0,0,1},
	{1,1,1,1},10.0f
};

static WORD billboard_idx[6] = {0,1,2, 3,2,1};

static NTVERTEX billboard_vtx[4] = {
	{0,-1, 1,  -1,0,0,  0,0},
	{0, 1, 1,  -1,0,0,  0,1},
	{0,-1,-1,  -1,0,0,  1,0},
	{0, 1,-1,  -1,0,0,  1,1}
};

static WORD exhaust_idx[12] = {0,1,2, 3,2,1, 4,5,6, 7,6,5};


NTVERTEX exhaust_vtx[8] = {
	{0,0,0, 0,0,0, 0.24f,0},
	{0,0,0, 0,0,0, 0.24f,1},
	{0,0,0, 0,0,0, 0.01f,0},
	{0,0,0, 0,0,0, 0.01f,1},
	{0,0,0, 0,0,0, 0.50390625f, 0.00390625f},
	{0,0,0, 0,0,0, 0.99609375f, 0.00390625f},
	{0,0,0, 0,0,0, 0.50390625f, 0.49609375f},
	{0,0,0, 0,0,0, 0.99609375f, 0.49609375f}
};


// TotalWeight = 21.337518, Count = 27, x - balance = -0.145075, y - balance = -0.012734
// The commented-out 27-entry weighted kernel stood here in the Windows file
// and is left commented exactly as it was: it is the author's alternative,
// not dead code this conversion introduced.
/*
static FVECTOR3 shadow_kernel_weighted[27] = { ... };  see D3D9Effect.cpp:190
*/

static FVECTOR3 shadow_kernel[27] = {
	{ -0.0000f, 0.0000f, 1.0000f },
	{ 0.1915f, 0.0188f, 1.0000f },
	{ 0.0558f, -0.2664f, 1.0000f },
	{ -0.2608f, -0.2076f, 1.0000f },
	{ -0.0706f, 0.3784f, 1.0000f },
	{ 0.3207f, -0.2869f, 1.0000f },
	{ -0.2438f, -0.4035f, 1.0000f },
	{ -0.4869f, -0.1488f, 1.0000f },
	{ -0.3108f, 0.4468f, 1.0000f },
	{ 0.4330f, 0.3819f, 1.0000f },
	{ -0.4208f, -0.4396f, 1.0000f },
	{ -0.6056f, -0.2017f, 1.0000f },
	{ -0.0612f, 0.6638f, 1.0000f },
	{ 0.6489f, -0.2457f, 1.0000f },
	{ 0.1850f, -0.6959f, 1.0000f },
	{ -0.7254f, 0.1711f, 1.0000f },
	{ -0.3309f, 0.6950f, 1.0000f },
	{ 0.6890f, -0.3937f, 1.0000f },
	{ -0.5610f, -0.5933f, 1.0000f },
	{ -0.7326f, -0.4086f, 1.0000f },
	{ -0.2997f, 0.8068f, 1.0000f },
	{ 0.8774f, -0.0892f, 1.0000f },
	{ -0.1133f, -0.8955f, 1.0000f },
	{ -0.9205f, -0.0673f, 1.0000f },
	{ 0.4487f, 0.8292f, 1.0000f },
	{ 0.7320f, 0.6245f, 1.0000f },
	{ 0.6067f, -0.7713f, 1.0000f }
};


// ###########################################################################
//
//   The effect framework. What ID3DXEffect was.
//
// ###########################################################################

// ---------------------------------------------------------------------------
// One pass of one technique.
//
// This is the `pass P0 { ... }` block, and every field below is one line of
// it. On Windows D3DX applied the state block with SetRenderState calls at
// BeginPass; here it is frozen into a VkPipeline, so the block is stored and
// consumed at pipeline-creation time instead.
// ---------------------------------------------------------------------------
struct VulkanPass
{
	std::string			vsEntry, psEntry;

	VkShaderModule		vs, ps;
	ShaderReflection   *vsRefl, *psRefl;

	// The render-state block, field for field with the .fx.
	bool				bAlphaBlend;
	VkBlendOp			blendOp;
	VkBlendFactor		srcBlend, dstBlend;
	bool				bZEnable;			// ZEnable      -- the depth TEST
	bool				bZWrite;			// ZWriteEnable -- the depth WRITE
	VkCullModeFlags		cullMode;
	VkFrontFace			frontFace;
	bool				bStencil;
	uint32_t			stencilRef, stencilMask;
	VkCompareOp			stencilFunc;
	VkStencilOp			stencilPass;
	bool				bPointSprite;

	// The pipelines built from this pass, keyed on everything the CALLER
	// supplies that Vulkan bakes in: the vertex layout, the topology, and the
	// six render states of PassOverride that are not dynamic. Every one of
	// those six is a D3DRS_ a call site sets between BeginPass and the draw
	// on Windows -- see PassOverride in VulkanEffect.h. Scissor and viewport
	// are absent because Vulkan keeps them dynamic and so does this.
	struct PipeKey {
		const VertexDecl   *pDecl;
		VkPrimitiveTopology topo;
		int					dstBlend;		// -1 none, else a VkBlendFactor
		int					blendEnable;
		int					colorWriteMask;
		int					depthTest;
		int					depthWrite;
		int					cullMode;		// PassOverride::CullMode
		// THE RENDER PASS IS PART OF THE KEY. A VkPipeline may only be bound
		// inside a render pass compatible with the one it was built against,
		// and compatibility requires the attachment formats to match -- so
		// the same technique drawn into the swapchain and into a shadow map
		// is two pipelines. D3D9 needed nothing like it: SetRenderTarget was
		// device state and an effect did not care what it pointed at. Same
		// addition, same reason, as ShaderClass::PipeKey's. See
		// VulkanDevice::BeginOffscreen.
		VkRenderPass		pass;
		// D3DRS_FILLMODE, global device state on Windows and pipeline state
		// here. See VulkanDevice::SetPolygonMode.
		int					fill;
		bool operator<(const PipeKey &o) const {
			if (pDecl != o.pDecl) return pDecl < o.pDecl;
			if (topo != o.topo) return topo < o.topo;
			if (dstBlend != o.dstBlend) return dstBlend < o.dstBlend;
			if (blendEnable != o.blendEnable) return blendEnable < o.blendEnable;
			if (colorWriteMask != o.colorWriteMask) return colorWriteMask < o.colorWriteMask;
			if (depthTest != o.depthTest) return depthTest < o.depthTest;
			if (depthWrite != o.depthWrite) return depthWrite < o.depthWrite;
			if (cullMode != o.cullMode) return cullMode < o.cullMode;
			if (pass != o.pass) return pass < o.pass;
			return fill < o.fill;
		}
	};
	std::map<PipeKey, VkPipeline> Pipelines;

	VulkanPass() :
		vs(VK_NULL_HANDLE), ps(VK_NULL_HANDLE), vsRefl(NULL), psRefl(NULL),
		bAlphaBlend(false), blendOp(VK_BLEND_OP_ADD),
		srcBlend(VK_BLEND_FACTOR_SRC_ALPHA),
		dstBlend(VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA),
		bZEnable(true), bZWrite(true),
		cullMode(VK_CULL_MODE_BACK_BIT), frontFace(VK_FRONT_FACE_CLOCKWISE),
		bStencil(false), stencilRef(0), stencilMask(0xFFFFFFFF),
		stencilFunc(VK_COMPARE_OP_ALWAYS), stencilPass(VK_STENCIL_OP_KEEP),
		bPointSprite(false)
	{}
};


// ---------------------------------------------------------------------------
// A technique: a name and its passes. What a TECHHANDLE points at.
// ---------------------------------------------------------------------------
class VulkanTechnique
{
public:
	VulkanTechnique(const char *n) : name(n) {}

	std::string					name;
	std::vector<VulkanPass>		Passes;
};


// ---------------------------------------------------------------------------
// The .fx render-state vocabulary.
//
// These tables are the whole of the D3D-to-Vulkan translation for the state
// block, and they are tables rather than if-chains so that a value appearing
// in a shader and NOT here is reported by name instead of silently defaulting.
// ---------------------------------------------------------------------------

static bool ParseBlendFactor(const std::string &v, VkBlendFactor *out)
{
	if (v == "Zero")			{ *out = VK_BLEND_FACTOR_ZERO; return true; }
	if (v == "One")				{ *out = VK_BLEND_FACTOR_ONE; return true; }
	if (v == "SrcColor")		{ *out = VK_BLEND_FACTOR_SRC_COLOR; return true; }
	if (v == "InvSrcColor")		{ *out = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR; return true; }
	if (v == "SrcAlpha")		{ *out = VK_BLEND_FACTOR_SRC_ALPHA; return true; }
	if (v == "InvSrcAlpha")		{ *out = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; return true; }
	if (v == "DestAlpha")		{ *out = VK_BLEND_FACTOR_DST_ALPHA; return true; }
	if (v == "InvDestAlpha")	{ *out = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA; return true; }
	if (v == "DestColor")		{ *out = VK_BLEND_FACTOR_DST_COLOR; return true; }
	if (v == "InvDestColor")	{ *out = VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR; return true; }
	if (v == "SrcAlphaSat")		{ *out = VK_BLEND_FACTOR_SRC_ALPHA_SATURATE; return true; }
	return false;
}

static bool ParseBlendOp(const std::string &v, VkBlendOp *out)
{
	if (v == "Add")			{ *out = VK_BLEND_OP_ADD; return true; }
	if (v == "Subtract")	{ *out = VK_BLEND_OP_SUBTRACT; return true; }
	if (v == "RevSubtract")	{ *out = VK_BLEND_OP_REVERSE_SUBTRACT; return true; }
	if (v == "Min")			{ *out = VK_BLEND_OP_MIN; return true; }
	if (v == "Max")			{ *out = VK_BLEND_OP_MAX; return true; }
	return false;
}

static bool ParseCompareOp(const std::string &v, VkCompareOp *out)
{
	if (v == "Never")		{ *out = VK_COMPARE_OP_NEVER; return true; }
	if (v == "Less")		{ *out = VK_COMPARE_OP_LESS; return true; }
	if (v == "Equal")		{ *out = VK_COMPARE_OP_EQUAL; return true; }
	if (v == "LessEqual")	{ *out = VK_COMPARE_OP_LESS_OR_EQUAL; return true; }
	if (v == "Greater")		{ *out = VK_COMPARE_OP_GREATER; return true; }
	if (v == "NotEqual")	{ *out = VK_COMPARE_OP_NOT_EQUAL; return true; }
	if (v == "GreaterEqual"){ *out = VK_COMPARE_OP_GREATER_OR_EQUAL; return true; }
	if (v == "Always")		{ *out = VK_COMPARE_OP_ALWAYS; return true; }
	return false;
}

static bool ParseStencilOp(const std::string &v, VkStencilOp *out)
{
	if (v == "Keep")	{ *out = VK_STENCIL_OP_KEEP; return true; }
	if (v == "Zero")	{ *out = VK_STENCIL_OP_ZERO; return true; }
	if (v == "Replace")	{ *out = VK_STENCIL_OP_REPLACE; return true; }
	if (v == "IncrSat")	{ *out = VK_STENCIL_OP_INCREMENT_AND_CLAMP; return true; }
	if (v == "DecrSat")	{ *out = VK_STENCIL_OP_DECREMENT_AND_CLAMP; return true; }
	if (v == "Invert")	{ *out = VK_STENCIL_OP_INVERT; return true; }
	if (v == "Incr")	{ *out = VK_STENCIL_OP_INCREMENT_AND_WRAP; return true; }
	if (v == "Decr")	{ *out = VK_STENCIL_OP_DECREMENT_AND_WRAP; return true; }
	return false;
}

static bool ParseBool(const std::string &v, bool *out)
{
	if (v == "true"  || v == "TRUE"  || v == "1") { *out = true; return true; }
	if (v == "false" || v == "FALSE" || v == "0") { *out = false; return true; }
	return false;
}

// CullMode is the one that inverts, and it is worth spelling out because
// getting it wrong shows up as "the model is inside out" rather than as an
// error.
//
// D3DRS_CULLMODE names WHICH WINDING TO DISCARD: D3DCULL_CCW discards
// counter-clockwise triangles. Vulkan names which FACE to discard
// (front/back) and separately which winding is the front. So "cull the CCW
// ones" is "back faces, and clockwise is the front".
static void ParseCullMode(const std::string &v, VkCullModeFlags *mode, VkFrontFace *face)
{
	if (v == "None" || v == "NONE" || v == "none") {
		*mode = VK_CULL_MODE_NONE;
		*face = VK_FRONT_FACE_CLOCKWISE;
	}
	else if (v == "CW") {
		*mode = VK_CULL_MODE_BACK_BIT;
		*face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	}
	else {	// CCW, which is also D3D9's default
		*mode = VK_CULL_MODE_BACK_BIT;
		*face = VK_FRONT_FACE_CLOCKWISE;
	}
}


// ---------------------------------------------------------------------------
// The technique-file reader.
//
// Reads the same `technique { pass { ... } }` blocks D3DX read, in the same
// syntax, from Modules/VulkanClient/VulkanClient.tech. See the file header for
// why the table stays a file.
//
// The parser is deliberately small: it recognises the eleven state names that
// actually appear across the eleven .fx files and reports by name anything it
// does not know, rather than defaulting silently. Comments (`//`) and the
// `compile vs_3_0` / `ps_3_0` profile words are skipped -- GLSL has no shader
// profiles, so the entry-point name is all that is left of that line.
// ---------------------------------------------------------------------------

namespace {

struct Tokenizer
{
	std::string s;
	size_t p;

	explicit Tokenizer(const std::string &text) : s(text), p(0) {}

	void SkipSpace()
	{
		for (;;) {
			while (p < s.size() && (unsigned char)s[p] <= ' ') p++;
			if (p + 1 < s.size() && s[p] == '/' && s[p+1] == '/') {
				while (p < s.size() && s[p] != '\n') p++;
				continue;
			}
			if (p + 1 < s.size() && s[p] == '/' && s[p+1] == '*') {
				p += 2;
				while (p + 1 < s.size() && !(s[p] == '*' && s[p+1] == '/')) p++;
				if (p + 1 < s.size()) p += 2;
				continue;
			}
			break;
		}
	}

	// One token: an identifier/number run, or a single punctuation character.
	std::string Next()
	{
		SkipSpace();
		if (p >= s.size()) return std::string();

		const char c = s[p];
		if (isalnum((unsigned char)c) || c == '_' || c == '.' || c == '-') {
			const size_t start = p;
			while (p < s.size() &&
				   (isalnum((unsigned char)s[p]) || s[p] == '_' || s[p] == '.' || s[p] == '-'))
				p++;
			return s.substr(start, p - start);
		}
		p++;
		return std::string(1, c);
	}

	std::string Peek()
	{
		const size_t save = p;
		const std::string t = Next();
		p = save;
		return t;
	}
};

// "vertexShader = compile vs_3_0 PBR_VS();" -> "PBR_VS"
//
// Everything between '=' and '(' is consumed and the LAST identifier is the
// entry point. That is what makes it profile-agnostic: `compile` and
// `vs_3_0` fall away on their own, and a future line without them still reads.
std::string ParseShaderAssignment(Tokenizer &tk)
{
	std::string last;
	for (;;) {
		const std::string t = tk.Next();
		if (t.empty() || t == ";") break;
		if (t == "(") {
			// swallow "()" and the ';'
			while (!t.empty()) {
				const std::string u = tk.Next();
				if (u.empty() || u == ";") break;
			}
			break;
		}
		if (t == "=" || t == "compile") continue;
		last = t;
	}
	return last;
}

// "SrcBlend = SrcAlpha;" -> value "SrcAlpha"
std::string ParseStateValue(Tokenizer &tk)
{
	std::string v;
	for (;;) {
		const std::string t = tk.Next();
		if (t.empty() || t == ";") break;
		if (t == "=") continue;
		v = t;
	}
	return v;
}

// "Texture = <gTex0>;" -> "gTex0"
//
// The same shape as ParseStateValue but keeping the last IDENTIFIER rather
// than the last token, because this is the one .fx state whose value is
// bracketed: D3DX's sampler_state writes a texture parameter reference as
// <name>, so the final token is '>' and not the name.
std::string ParseStateIdent(Tokenizer &tk)
{
	std::string v;
	for (;;) {
		const std::string t = tk.Next();
		if (t.empty() || t == ";") break;
		if (t.empty()) continue;
		const char c = t[0];
		if (isalpha((unsigned char)c) || c == '_') v = t;
	}
	return v;
}

// D3DTADDRESS_*, as the .fx spells them.
//
// MIRROR and BORDER do not appear in any of the eleven .fx files; they are
// accepted anyway because they are the rest of the D3D9 enumeration and
// leaving them out would make a future edit to a .tech file fail by silently
// wrapping instead of by name.
bool ParseAddressMode(const std::string &v, VkSamplerAddressMode *out)
{
	if      (v == "WRAP")   *out = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	else if (v == "CLAMP")  *out = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	else if (v == "MIRROR") *out = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
	else if (v == "BORDER") *out = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	else return false;
	return true;
}

bool ReadWholeFile(const char *path, std::string &out)
{
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n < 0) { fclose(f); return false; }
	out.resize((size_t)n);
	const size_t got = n ? fread(&out[0], 1, (size_t)n, f) : 0;
	fclose(f);
	return got == (size_t)n;
}

} // namespace


// ===========================================================================================
//
VulkanEffectFile::VulkanEffectFile(VulkanDevice *_pDev) :
	pDev(_pDev),
	pCurrent(NULL),
	pDecl(NULL),
	topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST),
	pScratchVB(NULL), pScratchIB(NULL),
	scratchVBSize(0), scratchIBSize(0),
	scratchVBUsed(0), scratchIBUsed(0),
	curPass(0),
	vkSetLayout(VK_NULL_HANDLE),
	vkPipeLayout(VK_NULL_HANDLE),
	blockSize(0),
	nSamplers(0),
	vkSampler(VK_NULL_HANDLE),
	pWhite(NULL),
	pWhiteCube(NULL),
	// vkSamplers is cleared in the body: an array member cannot be given a
	// brace-initialiser in a member initialiser list before C++20.
	vkPool(VK_NULL_HANDLE),
	pUniformArena(NULL),
	arenaSize(0), arenaUsed(0),
	curSet(VK_NULL_HANDLE),
	curUniformOffset(0),
	frameNo(0)
{
	memset(Textures, 0, sizeof(Textures));
	for (int i = 0; i < 3; i++) vkSamplers[i] = VK_NULL_HANDLE;
	memset(vkBindSampler, 0, sizeof(vkBindSampler));
	memset(bCubeBind, 0, sizeof(bCubeBind));
	Instances.insert(this);
}


// The instance registry. See ResetFrameAll().
std::set<VulkanEffectFile*> VulkanEffectFile::Instances;


// ===========================================================================================
// The per-frame reset, for every effect file rather than one of the four.
// See the declaration for what filling the other three cost.
// ===========================================================================================
void VulkanEffectFile::ResetFrameAll()
{
	for (auto p : Instances) if (p) p->ResetFrame();
}

// ===========================================================================================
//
VulkanEffectFile::~VulkanEffectFile()
{
	if (pDev) {
		VkDevice dev = pDev->GetDevice();

		for (auto &it : Techniques) {
			for (auto &pass : it.second->Passes) {
				for (auto &pipe : pass.Pipelines)
					if (pipe.second != VK_NULL_HANDLE)
						vkDestroyPipeline(dev, pipe.second, NULL);
				// The modules are the compiler's; the reflections are ours.
				if (pass.vs != VK_NULL_HANDLE) vkDestroyShaderModule(dev, pass.vs, NULL);
				if (pass.ps != VK_NULL_HANDLE) vkDestroyShaderModule(dev, pass.ps, NULL);
				delete pass.vsRefl;
				delete pass.psRefl;
			}
			delete it.second;
		}
		Techniques.clear();

		if (vkPipeLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(dev, vkPipeLayout, NULL);
		if (vkSetLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(dev, vkSetLayout, NULL);
		for (int i = 0; i < 3; i++)
			if (vkSamplers[i] != VK_NULL_HANDLE) vkDestroySampler(dev, vkSamplers[i], NULL);
		vkSampler = VK_NULL_HANDLE;		// an alias of vkSamplers[1], already destroyed
		for (int i = 0; i < 32; i++)
			if (vkBindSampler[i] != VK_NULL_HANDLE) vkDestroySampler(dev, vkBindSampler[i], NULL);
		if (pWhite) pDev->DestroyTexture(pWhite);
		if (pWhiteCube) pDev->DestroyTexture(pWhiteCube);
		if (vkPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev, vkPool, NULL);
		if (pUniformArena) pDev->DestroyBuffer(pUniformArena);
		if (pScratchVB) pDev->DestroyBuffer(pScratchVB);
		if (pScratchIB) pDev->DestroyBuffer(pScratchIB);
		// Scratch buffers a mid-frame grow replaced, which no ResetFrame()
		// came round to free. See retiredScratch in the header.
		for (auto &p : retiredScratch) pDev->DestroyBuffer(p.first);
		retiredScratch.clear();
		// And every set still in the rotation. The session is over and the
		// caller has waited for the device, so all of them are idle.
		for (FrameSet &f : retired) {
			if (f.pool != VK_NULL_HANDLE) vkDestroyDescriptorPool(dev, f.pool, NULL);
			if (f.arena) pDev->DestroyBuffer(f.arena);
			if (f.scratchVB) pDev->DestroyBuffer(f.scratchVB);
			if (f.scratchIB) pDev->DestroyBuffer(f.scratchIB);
		}
		retired.clear();
	}

	// Out of the registry before the object goes, or the next ResetFrameAll
	// walks a dangling pointer. Outside the `if (pDev)` because membership
	// does not depend on having a device.
	Instances.erase(this);
}


// ===========================================================================================
// Counterpart of D3DXCreateEffectFromFileA.
//
// Two files rather than one, and the split is the HLSL/GLSL boundary rather
// than a restructuring: `file` names the shader source that glslang compiles
// entry points out of, and the technique table sits beside it under the same
// stem with a .tech extension. D3DX had them in one file because its compiler
// understood both halves; glslang understands only the shader half.
// ===========================================================================================
bool VulkanEffectFile::Load(const char *file)
{
	if (!file || !pDev) return false;

	fname = file;

	std::string techPath(file);
	const size_t dot = techPath.find_last_of('.');
	if (dot != std::string::npos) techPath = techPath.substr(0, dot);
	techPath += ".tech";

	std::string text;
	if (!ReadWholeFile(techPath.c_str(), text)) {
		LogErr("VulkanEffectFile: cannot read technique table %s", techPath.c_str());
		return false;
	}

	// The preprocessor options. These are D3DXMACRO's counterpart: the same
	// six computed defines and five conditional ones that
	// D3D9Effect::D3D9TechInit built, in a form glslang takes.
	char options[512];
	{
		const int aniso = (Config->Anisotrophy > 2) ? Config->Anisotrophy : 2;
		int shadowFilter = Config->ShadowFilter;
		if (Config->ShadowMapMode == 0) shadowFilter = -1;

		const int kernelSize = (shadowFilter >= 3) ? 35 : 27;
		const float kernelWeight = (shadowFilter >= 3) ? 0.0285f : (1.0f / 27.0f);

		// NAME=VALUE, and NAME alone for the five flags -- which is exactly
		// what the eleven D3DXMACROs are. There is no "-D": the option string
		// is the client's own format, split on ";, " by CompileShaderStage
		// and turned back into a { Name, Definition } pair there. A "-D"
		// would become part of the macro name.
		int n = snprintf(options, sizeof(options),
			"ANISOTROPY_MACRO=%d LMODE=%d MAX_LIGHTS=%d "
			"SHDMAP=%d KERNEL_SIZE=%d KERNEL_WEIGHT=%f",
			aniso, Config->LightConfig, Config->MaxLights(),
			shadowFilter + 1, kernelSize, kernelWeight);

		if (Config->EnableGlass && n < (int)sizeof(options))
			n += snprintf(options + n, sizeof(options) - n, " _GLASS");
		if (Config->EnableMeshDbg && n < (int)sizeof(options))
			n += snprintf(options + n, sizeof(options) - n, " _DEBUG");
		if (Config->EnvMapMode && n < (int)sizeof(options))
			n += snprintf(options + n, sizeof(options) - n, " _ENVMAP");
		if (Config->PostProcess == PP_DEFAULT && n < (int)sizeof(options))
			n += snprintf(options + n, sizeof(options) - n, " _LIGHTGLOW");
		if (Config->bIrradiance && Config->EnvMapMode && n < (int)sizeof(options))
			n += snprintf(options + n, sizeof(options) - n, " _IRRADIANCE");
	}

	LogAlw("VulkanEffectFile: %s  options: %s", file, options);

	// ---------------- parse the technique table ----------------
	Tokenizer tk(text);
	int nTech = 0, nPass = 0, nSmp = 0;

	for (;;) {
		std::string t = tk.Next();
		if (t.empty()) break;

		// ------------------------------------------------------------
		// A sampler_state block, transcribed from the .fx. See the note on
		// SamplerStates in VulkanEffect.h for why these live here.
		//
		//     sampler ClampS
		//     {
		//         Texture = <gTex0>;
		//         MinFilter = ANISOTROPIC;
		//         ...
		//     };
		//
		// The `= sampler_state` the .fx writes between the name and the brace
		// is skipped the same way `compile vs_3_0` is: it says nothing the
		// keyword has not already said.
		// ------------------------------------------------------------
		if (t == "sampler") {
			const std::string smpName = tk.Next();
			std::string u = tk.Next();
			while (!u.empty() && u != "{" && u != "}") u = tk.Next();	// '=' sampler_state
			if (u != "{") {
				LogErr("VulkanEffectFile: '{' expected after sampler %s", smpName.c_str());
				return false;
			}

			SamplerState ss;
			// MaxAnisotropy defaults to the effect's own macro value, which
			// is what every anisotropic block in the .fx asks for by name.
			ss.maxAniso = float(Config->Anisotrophy > 2 ? Config->Anisotrophy : 2);

			for (;;) {
				const std::string k = tk.Next();
				if (k.empty() || k == "}") break;

				// "Texture = <gTex0>;" is the one line whose value is not the
				// last token before the ';' -- that is the '>' -- so it is
				// read with the identifier form. Every other state's value is
				// a bare word or a number.
				if (k == "Texture") {
					ss.texture = ParseStateIdent(tk);
					continue;
				}

				const std::string v = ParseStateValue(tk);

				if (k == "MinFilter") {
					if (v == "ANISOTROPIC") { ss.bAniso = true; ss.minFilter = VK_FILTER_LINEAR; }
					else if (v == "POINT" || v == "NONE") ss.minFilter = VK_FILTER_NEAREST;
					else ss.minFilter = VK_FILTER_LINEAR;
				}
				else if (k == "MagFilter") {
					if (v == "POINT" || v == "NONE") ss.magFilter = VK_FILTER_NEAREST;
					else ss.magFilter = VK_FILTER_LINEAR;
				}
				else if (k == "MipFilter") {
					if (v == "NONE") { ss.bMip = false; ss.mipMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; }
					else if (v == "POINT") ss.mipMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
					else ss.mipMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
				}
				else if (k == "MaxAnisotropy") {
					// The .fx writes ANISOTROPY_MACRO here, which is the
					// option macro built above; a literal is honoured too.
					if (!v.empty() && (isdigit((unsigned char)v[0]) || v[0] == '-'))
						ss.maxAniso = float(atof(v.c_str()));
				}
				else if (k == "MipMapLODBias") ss.lodBias = float(atof(v.c_str()));
				else if (k == "AddressU") ParseAddressMode(v, &ss.addrU);
				else if (k == "AddressV") ParseAddressMode(v, &ss.addrV);
				else if (k == "AddressW") ParseAddressMode(v, &ss.addrW);
				else if (k == "SRGBTexture" || k == "BorderColor") { /* unused by the .fx */ }
				else {
					LogWrn("VulkanEffectFile: unknown sampler state '%s = %s' in "
						   "sampler %s -- ignored", k.c_str(), v.c_str(), smpName.c_str());
				}
			}
			// The trailing ';' after the closing brace, if present.
			if (tk.Peek() == ";") tk.Next();

			SamplerStates[smpName] = ss;
			nSmp++;
			continue;
		}

		if (t != "technique") continue;

		const std::string techName = tk.Next();
		if (tk.Next() != "{") {
			LogErr("VulkanEffectFile: '{' expected after technique %s", techName.c_str());
			return false;
		}

		VulkanTechnique *pTech = new VulkanTechnique(techName.c_str());

		for (;;) {
			std::string u = tk.Next();
			if (u.empty() || u == "}") break;
			if (u != "pass") {
				LogErr("VulkanEffectFile: 'pass' expected in technique %s, got '%s'",
					   techName.c_str(), u.c_str());
				delete pTech;
				return false;
			}
			tk.Next();					// the pass name, P0/P1/... -- unused, as in D3DX
			if (tk.Next() != "{") {
				LogErr("VulkanEffectFile: '{' expected after pass in %s", techName.c_str());
				delete pTech;
				return false;
			}

			VulkanPass pass;

			for (;;) {
				std::string k = tk.Next();
				if (k.empty() || k == "}") break;

				if (k == "vertexShader" || k == "VertexShader")
					pass.vsEntry = ParseShaderAssignment(tk);
				else if (k == "pixelShader" || k == "PixelShader")
					pass.psEntry = ParseShaderAssignment(tk);
				else {
					const std::string v = ParseStateValue(tk);
					bool ok = true;
					if      (k == "AlphaBlendEnable") ok = ParseBool(v, &pass.bAlphaBlend);
					else if (k == "BlendOp")          ok = ParseBlendOp(v, &pass.blendOp);
					else if (k == "SrcBlend")         ok = ParseBlendFactor(v, &pass.srcBlend);
					else if (k == "DestBlend")        ok = ParseBlendFactor(v, &pass.dstBlend);
					else if (k == "ZEnable")          ok = ParseBool(v, &pass.bZEnable);
					else if (k == "ZWriteEnable")     ok = ParseBool(v, &pass.bZWrite);
					else if (k == "StencilEnable")    ok = ParseBool(v, &pass.bStencil);
					else if (k == "StencilRef")       pass.stencilRef = (uint32_t)atoi(v.c_str());
					else if (k == "StencilMask")      pass.stencilMask = (uint32_t)atoi(v.c_str());
					else if (k == "StencilFunc")      ok = ParseCompareOp(v, &pass.stencilFunc);
					else if (k == "StencilPass")      ok = ParseStencilOp(v, &pass.stencilPass);
					else if (k == "CullMode")         ParseCullMode(v, &pass.cullMode, &pass.frontFace);
					else if (k == "PointSpriteEnable") {
						ok = ParseBool(v, &pass.bPointSprite);
						// D3DRS_POINTSPRITEENABLE turned a point into a
						// textured quad with generated texture coordinates.
						// Vulkan has no such state: a point is one fragment
						// unless the vertex shader writes gl_PointSize, and
						// gl_PointCoord is always available in the fragment
						// shader. So the flag is recorded, the topology
						// becomes POINT_LIST, and the GLSL translation of
						// BeaconArray.fx writes gl_PointSize itself.
					}
					else {
						LogWrn("VulkanEffectFile: unknown pass state '%s = %s' in "
							   "technique %s -- ignored", k.c_str(), v.c_str(),
							   techName.c_str());
					}
					if (!ok) {
						LogErr("VulkanEffectFile: unknown value '%s' for state '%s' "
							   "in technique %s", v.c_str(), k.c_str(), techName.c_str());
					}
				}
			}

			if (pass.bPointSprite) { /* see the note above */ }

			// Compile the pass's two entry points. CompileVertexShader and
			// CompilePixelShader are VulkanUtil.cpp's -- the same glslang
			// path ShaderClass uses, so there is one compiler in the client
			// and one reflection format.
			if (!pass.vsEntry.empty()) {
				pass.vs = CompileVertexShader(pDev, file, pass.vsEntry.c_str(),
											  techName.c_str(), options, &pass.vsRefl);
			}
			if (!pass.psEntry.empty()) {
				pass.ps = CompilePixelShader(pDev, file, pass.psEntry.c_str(),
											 techName.c_str(), options, &pass.psRefl);
			}

			if (pass.vs == VK_NULL_HANDLE || pass.ps == VK_NULL_HANDLE) {
				LogErr("VulkanEffectFile: technique %s pass %d failed to compile "
					   "(%s / %s)", techName.c_str(), (int)pTech->Passes.size(),
					   pass.vsEntry.c_str(), pass.psEntry.c_str());
			}

			pTech->Passes.push_back(pass);
			nPass++;
		}

		Techniques[techName] = pTech;
		nTech++;
	}

	LogAlw("VulkanEffectFile: %d techniques, %d passes, %d sampler states from %s",
		   nTech, nPass, nSmp, techPath.c_str());

	if (nTech == 0) return false;

	return BuildParameterTable();
}


// ===========================================================================================
// Merge every pass's reflection into the ONE effect-wide parameter table.
//
// This is where "an .fx parameter is global" becomes a fact about the Vulkan
// objects rather than a claim. Every shader in the effect declares the same
// parameter block, so every name must land at the same offset in every stage
// that mentions it -- and if two disagree, that is a real bug in the GLSL and
// is reported here by name rather than showing up later as one technique
// reading another's numbers.
// ===========================================================================================
bool VulkanEffectFile::BuildParameterTable()
{
	uint32_t maxEnd = 0;
	uint32_t maxSampler = 0;

	for (auto &it : Techniques) {
		for (auto &pass : it.second->Passes) {

			ShaderReflection *refl[2] = { pass.vsRefl, pass.psRefl };

			for (int s = 0; s < 2; s++) {
				if (!refl[s]) continue;

				// THE BLOCK'S SIZE COMES FROM THE BLOCK, NOT FROM ITS LAST
				// MEMBER. It was computed as max(offset + size) over the
				// members, which is wrong twice over: glslang's `size` for a
				// block member is the ARRAY ELEMENT COUNT, not a byte count
				// (a mat4 reports 1 and a vec3[27] reports 27), and the
				// block's tail padding belongs to it either way. Measured on
				// VulkanClient.glsl: the members give 2257 where the block is
				// 2260, so the last four bytes of the parameter block were
				// outside Params and every write to them was refused by
				// WriteConstants' bounds test.
				if (refl[s]->BlockSize() > maxEnd) maxEnd = refl[s]->BlockSize();

				for (const ShaderReflection::Var &v : refl[s]->Vars()) {

					auto found = ParamMap.find(v.name);
					if (found != ParamMap.end()) {
						const ShaderReflection::Var &o = found->second;
						if (o.bSampler != v.bSampler ||
							(!v.bSampler && (o.offset != v.offset || o.size != v.size)) ||
							( v.bSampler && o.binding != v.binding)) {
							LogErr("VulkanEffectFile: parameter '%s' is declared "
								   "differently in two shaders (technique %s): "
								   "offset %u vs %u, binding %u vs %u. The shared "
								   "parameter block must be identical in every stage.",
								   v.name.c_str(), it.first.c_str(),
								   o.offset, v.offset, o.binding, v.binding);
						}
						continue;
					}

					ParamMap[v.name] = v;

					if (v.bSampler) {
						if (v.binding + 1 > maxSampler) maxSampler = v.binding + 1;
					}
					else {
						if (v.offset + v.size > maxEnd) maxEnd = v.offset + v.size;
					}
				}
			}
		}
	}

	blockSize = maxEnd;
	nSamplers = maxSampler;

	// WHICH SAMPLER BINDINGS ARE CUBES, from the same reflection that gave
	// the bindings. NewMesh.glsl declares
	//
	//     layout(set = 0, binding = 20) uniform samplerCube EnvMapAS;  // <gEnvMapA>
	//
	// and Mesh.cpp only fills it when a vessel actually has an environment
	// map -- `if (nEnv >= 1 && pEnv[0]) FX->SetTexture(eEnvMapA, pEnv[0]);`,
	// converted verbatim from the reference. Every other draw leaves the slot
	// unset, and the unset-slot fallback below then has to be a CUBE: binding
	// a 2D view to a cube declaration is
	//
	//     VUID-vkCmdDrawIndexed-viewType-07752 ... "EnvMapAS" VkImageViewType
	//     is VK_IMAGE_VIEW_TYPE_2D but the OpTypeImage has (Dim = Cube)
	//
	// and the GPU faults on it. D3D9 had nothing to convert here: SetTexture
	// took any IDirect3DBaseTexture9 and the runtime reconciled it with the
	// sampler declaration, so an unset stage sampled white whatever its
	// dimension.
	for (const auto &kv : ParamMap) {
		const ShaderReflection::Var &v = kv.second;
		if (v.bSampler && v.bCube && v.binding < ARRAYSIZE(bCubeBind))
			bCubeBind[v.binding] = true;
	}

	// ------------------------------------------------------------------
	// JOIN THE REFLECTED SAMPLER BINDINGS TO THE .tech SAMPLER STATES, AND
	// SYNTHESIZE THE TEXTURE PARAMETERS.
	//
	// The GLSL declares one `sampler2D` per .fx sampler_state and names it
	// after that block -- WrapS, ClampS, Planet0S -- so reflection gives the
	// BINDINGS. The .tech gives each block's `Texture = <gTex0>`. Together
	// they answer the question every FX->SetTexture asks: which bindings does
	// this texture parameter feed?
	//
	// The texture parameters are then added to ParamMap as sampler Vars in
	// their own right, because that is what GetParameterByName("gTex0") has
	// to find -- and gTex0 is a name no shader declares, since in GLSL there
	// is no such thing as a texture without a sampler. On Windows D3DX made
	// the same join, from the sampler_state blocks it had compiled itself.
	//
	// A texture parameter's Var carries binding 0xFFFFFFFF: it names no
	// single binding, and SetTexture dispatches on the name instead. Reading
	// it as a binding would silently write the uniform block's slot.
	// ------------------------------------------------------------------
	for (auto &it : SamplerStates) {
		if (it.second.texture.empty()) continue;
		auto found = ParamMap.find(it.first);
		if (found == ParamMap.end()) continue;			// declared, never used
		if (!found->second.bSampler) {
			LogErr("VulkanEffectFile: '%s' is a sampler_state in the .tech but "
				   "a value parameter in the GLSL", it.first.c_str());
			continue;
		}
		TexBindings[it.second.texture].push_back(found->second.binding);
	}

	for (auto &it : TexBindings) {
		if (ParamMap.find(it.first) != ParamMap.end()) {
			LogErr("VulkanEffectFile: texture parameter '%s' collides with a "
				   "name the GLSL already declares", it.first.c_str());
			continue;
		}
		ShaderReflection::Var v = {};
		v.name = it.first;
		v.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		v.set = 0;
		v.binding = 0xFFFFFFFFu;		// see the note above
		v.offset = 0;
		v.size = 0;
		v.bSampler = true;
		v.samplerIndex = 0;
		ParamMap[it.first] = v;
	}

	// Binding 0 is the parameter block by convention -- see VulkanTypes.h's
	// note on locations: a convention spelled out once and honoured by every
	// GLSL file beats a rule the reader has to infer.
	if (nSamplers > 0 && nSamplers <= 1) nSamplers = 1;
	if (nSamplers > 32) {
		LogErr("VulkanEffectFile: %u sampler bindings exceeds the 32 this "
			   "effect tracks", nSamplers);
		nSamplers = 32;
	}

	Params.assign(blockSize ? blockSize : 4, 0);

	LogAlw("VulkanEffectFile: parameter block %u bytes, %u parameters, %u samplers",
		   blockSize, (unsigned)ParamMap.size(), nSamplers);

	VkDevice dev = pDev->GetDevice();

	// ---- the descriptor set layout -------------------------------------
	std::vector<VkDescriptorSetLayoutBinding> binds;
	{
		VkDescriptorSetLayoutBinding b = {};
		b.binding = 0;
		b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		b.descriptorCount = 1;
		b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
		binds.push_back(b);
	}
	for (uint32_t i = 1; i < nSamplers; i++) {
		VkDescriptorSetLayoutBinding b = {};
		b.binding = i;
		b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		b.descriptorCount = 1;
		b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
		binds.push_back(b);
	}

	VkDescriptorSetLayoutCreateInfo dsl = {};
	dsl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	dsl.bindingCount = (uint32_t)binds.size();
	dsl.pBindings = binds.data();

	if (vkCreateDescriptorSetLayout(dev, &dsl, NULL, &vkSetLayout) != VK_SUCCESS) {
		LogErr("VulkanEffectFile: vkCreateDescriptorSetLayout failed");
		return false;
	}

	VkPipelineLayoutCreateInfo pl = {};
	pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pl.setLayoutCount = 1;
	pl.pSetLayouts = &vkSetLayout;

	if (vkCreatePipelineLayout(dev, &pl, NULL, &vkPipeLayout) != VK_SUCCESS) {
		LogErr("VulkanEffectFile: vkCreatePipelineLayout failed");
		return false;
	}

	// ---- the sampler ---------------------------------------------------
	//
	// One sampler for every texture parameter, and that is faithful rather
	// than a simplification. The .fx declares its sampler state INSIDE the
	// sampler_state blocks of the shader -- `AddressU = WRAP; MinFilter =
	// ANISOTROPIC;` -- and D3DX applied those per stage; the client never
	// varies them from C++ for this effect. Anisotropy is the one value it
	// does choose, and it arrives through ANISOTROPY_MACRO in the options
	// above, which is a shader-side define. So the filtering the shaders ask
	// for is what this sampler is built with.
	{
		const float maxAniso = pDev->GetProperties()->limits.maxSamplerAnisotropy;
		const bool bAniso = pDev->GetFeatures()->samplerAnisotropy && maxAniso > 1.0f;
		float want = float(Config->Anisotrophy > 2 ? Config->Anisotrophy : 2);
		if (want > maxAniso) want = maxAniso;

		// THREE samplers, for the three filter settings D3D9Pad::Flush picks
		// between with D3DSAMP_MIN/MAGFILTER: POINT, LINEAR and ANISOTROPIC.
		// D3D9 changed a number on a numbered device slot; a VkSampler is an
		// immutable object built from those numbers, so three settings means
		// three objects. Everything else about them is identical.
		for (int i = 0; i < 3; i++) {

			const VkFilter f = (i == 0) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
			const bool bA = (i == 2) && bAniso;

			VkSamplerCreateInfo si = {};
			si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
			si.magFilter = f;
			si.minFilter = f;
			si.mipmapMode = (i == 0) ? VK_SAMPLER_MIPMAP_MODE_NEAREST
									 : VK_SAMPLER_MIPMAP_MODE_LINEAR;
			si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			si.anisotropyEnable = bA ? VK_TRUE : VK_FALSE;
			// D3D9Pad::Flush asks for D3DSAMP_MAXANISOTROPY 8 explicitly and
			// the effect's own sampler_state blocks use ANISOTROPY_MACRO.
			// The pad's 8 wins for the anisotropic sampler because that is
			// the one the pad selects.
			si.maxAnisotropy = bA ? (want > 8.0f ? 8.0f : want) : 1.0f;
			si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
			si.minLod = 0.0f;
			si.maxLod = VK_LOD_CLAMP_NONE;

			if (vkCreateSampler(dev, &si, NULL, &vkSamplers[i]) != VK_SUCCESS) {
				LogErr("VulkanEffectFile: vkCreateSampler(%d) failed", i);
				return false;
			}
		}
		vkSampler = vkSamplers[1];		// LINEAR is the default, as in D3D9
	}

	// ---- one sampler per binding, from its sampler_state ----------------
	//
	// THE THREE ABOVE ARE THE OVERRIDE PATH, NOT THE NORMAL ONE. They exist
	// for D3D9Pad::Flush, which sets D3DSAMP_MIN/MAGFILTER per draw; the
	// filtering and, more importantly, the ADDRESSING that the effect's own
	// shaders ask for come from the .fx sampler_state blocks, which are now
	// in the .tech. Without this every binding sampled with REPEAT, so
	// ClampS, Planet0S, SimpleS, MFDSamp, Panel0S, ExhaustS, EnvMapAS and
	// EnvMapBS -- all of which say CLAMP -- wrapped their edge texels
	// instead. On a planet tile that is a visible seam on every edge.
	memset(vkBindSampler, 0, sizeof(vkBindSampler));
	{
		const float maxAniso = pDev->GetProperties()->limits.maxSamplerAnisotropy;
		const bool bAnisoOk = pDev->GetFeatures()->samplerAnisotropy && maxAniso > 1.0f;

		for (auto &it : SamplerStates) {
			auto found = ParamMap.find(it.first);
			if (found == ParamMap.end() || !found->second.bSampler) continue;
			const uint32_t b = found->second.binding;
			if (b == 0 || b >= 32 || b >= nSamplers) continue;

			const SamplerState &ss = it.second;
			const bool bA = ss.bAniso && bAnisoOk;
			float want = ss.maxAniso;
			if (want > maxAniso) want = maxAniso;
			if (want < 1.0f) want = 1.0f;

			VkSamplerCreateInfo si = {};
			si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
			si.magFilter = ss.magFilter;
			si.minFilter = ss.minFilter;
			si.mipmapMode = ss.mipMode;
			si.addressModeU = ss.addrU;
			si.addressModeV = ss.addrV;
			si.addressModeW = ss.addrW;
			si.anisotropyEnable = bA ? VK_TRUE : VK_FALSE;
			si.maxAnisotropy = bA ? want : 1.0f;
			si.mipLodBias = ss.lodBias;
			si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
			si.minLod = 0.0f;
			// MipFilter = NONE meant "sample the top level only", which is a
			// LOD clamp rather than a filter mode: D3D9 had no separate way
			// to say it and Vulkan does.
			si.maxLod = ss.bMip ? VK_LOD_CLAMP_NONE : 0.25f;

			if (vkCreateSampler(dev, &si, NULL, &vkBindSampler[b]) != VK_SUCCESS) {
				LogErr("VulkanEffectFile: vkCreateSampler for '%s' (binding %u) failed",
					   it.first.c_str(), b);
				vkBindSampler[b] = VK_NULL_HANDLE;
			}
		}

		// A binding the .tech describes with no sampler_state is a real gap,
		// not a default: the GLSL declared a sampler the technique table does
		// not know about, so nothing says how it should filter or address.
		for (uint32_t i = 1; i < nSamplers; i++) {
			if (vkBindSampler[i]) continue;
			LogWrn("VulkanEffectFile: sampler binding %u has no sampler_state "
				   "in the .tech file; using LINEAR/WRAP", i);
		}
	}

	// ---- the 1x1 white default ------------------------------------------
	// See the note by pWhite: D3D9 sampled white from an unset stage, Vulkan
	// samples undefined behaviour from an unwritten descriptor.
	{
		pWhite = pDev->CreateTexture(1, 1, 1, VK_FORMAT_B8G8R8A8_UNORM,
									 VK_IMAGE_USAGE_SAMPLED_BIT |
									 VK_IMAGE_USAGE_TRANSFER_DST_BIT |
									 VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
		if (!pWhite) {
			LogErr("VulkanEffectFile: could not create the default white texture");
			return false;
		}
		const DWORD white = 0xFFFFFFFF;
		pDev->UploadTexture(pWhite, 0, 0, &white, sizeof(white));

		// And a white CUBE, if any technique in this file declares one. See
		// the bCubeBind note above: a cube declaration cannot be given a 2D
		// view. Built once per effect file, and only when it is needed.
		bool bAnyCube = false;
		for (size_t i = 0; i < ARRAYSIZE(bCubeBind); i++)
			if (bCubeBind[i]) { bAnyCube = true; break; }

		if (bAnyCube) {
			pWhiteCube = pDev->CreateTextureCube(1, 1, VK_FORMAT_B8G8R8A8_UNORM,
												 VK_IMAGE_USAGE_SAMPLED_BIT |
												 VK_IMAGE_USAGE_TRANSFER_DST_BIT |
												 VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
			if (!pWhiteCube) {
				LogErr("VulkanEffectFile: could not create the default white cube");
				return false;
			}
			for (uint32_t f = 0; f < 6; f++)
				pDev->UploadTexture(pWhiteCube, 0, f, &white, sizeof(white));
		}
	}

	// ---- the per-frame arena -------------------------------------------
	//
	// Sized for the frame rather than for the draw. 4096 passes a frame at a
	// parameter block of a few hundred bytes is the shape of a busy scene --
	// every vessel, every beacon, every exhaust plume is one -- and the
	// allocation is one buffer made once, not one per draw.
	// The pool and the arena are made by CreateFrameSet, because ResetFrame
	// needs to make more of them: they rotate rather than being reset in
	// place. See the FrameSet note in the header.
	if (!CreateFrameSet()) return false;

	return true;
}


// ===========================================================================================
// One frame's worth of pool and arena.
//
// Split out of CreateResources unchanged so that ResetFrame can build another
// while the previous ones are still being read by the GPU. Nothing here is a
// departure from the reference -- D3DX allocated its constant storage once
// and recycled it internally, and this is where that recycling had to become
// visible.
// ===========================================================================================
bool VulkanEffectFile::CreateFrameSet()
{
	if (!pDev) return false;
	VkDevice dev = pDev->GetDevice();

	{
		VkDescriptorPoolSize sizes[2] = {};
		sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		sizes[0].descriptorCount = 4096;
		sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		sizes[1].descriptorCount = 4096 * (nSamplers ? nSamplers : 1);

		VkDescriptorPoolCreateInfo dp = {};
		dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		dp.maxSets = 4096;
		dp.poolSizeCount = 2;
		dp.pPoolSizes = sizes;
		// No FREE_DESCRIPTOR_SET_BIT: a whole pool is reset at once when the
		// rotation hands it back, which is cheaper than freeing sets.
		if (vkCreateDescriptorPool(dev, &dp, NULL, &vkPool) != VK_SUCCESS) {
			LogErr("VulkanEffectFile: vkCreateDescriptorPool failed");
			return false;
		}
	}

	{
		// Each pass's slice must start on the device's minimum uniform buffer
		// offset alignment -- a Vulkan rule with no D3D9 counterpart, because
		// D3D9 constants went into registers rather than into a buffer.
		const VkDeviceSize align = pDev->GetProperties()->limits.minUniformBufferOffsetAlignment;
		VkDeviceSize stride = blockSize ? blockSize : 4;
		if (align > 1) stride = ((stride + align - 1) / align) * align;

		arenaSize = stride * 4096;
		pUniformArena = pDev->CreateBuffer(arenaSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
		if (!pUniformArena) {
			LogErr("VulkanEffectFile: uniform arena allocation failed (%u bytes)",
				   (unsigned)arenaSize);
			return false;
		}
		arenaUsed = 0;
	}

	return true;
}


// ===========================================================================================
// Counterpart of ID3DXEffect::GetTechniqueByName.
//
// Returns NULL for a name the effect does not define, which is what D3DX did
// and what two call sites in D3D9Effect.cpp rely on without knowing it -- see
// the note in VulkanTechInit about eSkyDomeTech and eBaseShadowTech.
// ===========================================================================================
TECHHANDLE VulkanEffectFile::GetTechniqueByName(const char *name)
{
	if (!name) return NULL;
	auto it = Techniques.find(name);
	return (it == Techniques.end()) ? NULL : it->second;
}

// ===========================================================================================
// Counterpart of ID3DXEffect::GetParameterByName.
//
// The `0` first argument D3DX took was a parent handle, for looking a name up
// inside a struct or an annotation. Every one of the ~90 call sites passes 0,
// meaning "top level", so the parameter is gone rather than carried as an
// argument that is never anything else.
// ===========================================================================================
HANDLE VulkanEffectFile::GetParameterByName(const char *name)
{
	if (!name) return NULL;
	auto it = ParamMap.find(name);
	if (it == ParamMap.end()) {
		// D3DX returned NULL silently. It is logged here because a NULL
		// handle is how a parameter renamed in the shader and not in the
		// client turns into "that effect just stopped working", and the log
		// line is the difference between a minute and an afternoon.
		LogWrn("VulkanEffectFile: no parameter named '%s'", name);
		return NULL;
	}
	return (HANDLE)&it->second;
}

// ===========================================================================================
//
void VulkanEffectFile::SetTechnique(TECHHANDLE hTech)
{
	pCurrent = hTech;
}

void VulkanEffectFile::SetVertexDecl(const VertexDecl *p)
{
	pDecl = p;
}

void VulkanEffectFile::SetTopology(VkPrimitiveTopology t)
{
	topology = t;
}


// ===========================================================================================
// The parameter setters.
//
// All six write into the effect-wide block image; SetTexture records a
// descriptor instead. D3DX's SetMatrix took a transpose flag it never used
// here and SetVector took four floats -- both collapse into SetValue, which is
// where the bounds check lives.
// ===========================================================================================

bool VulkanEffectFile::SetValue(HANDLE h, const void *pData, UINT bytes)
{
	if (!h || !pData) return false;

	const ShaderReflection::Var *v = (const ShaderReflection::Var *)h;
	if (v->bSampler) {
		LogErr("VulkanEffectFile::SetValue on the texture parameter '%s'",
			   v->name.c_str());
		return false;
	}
	if (v->offset + bytes > Params.size()) {
		// D3DX silently truncated a write past the end of a parameter. This
		// refuses it: writing past a parameter lands in the NEXT one, which
		// shows up as an unrelated value changing.
		LogErr("VulkanEffectFile::SetValue('%s'): %u bytes at offset %u exceeds "
			   "the %u-byte parameter block",
			   v->name.c_str(), bytes, v->offset, (unsigned)Params.size());
		return false;
	}
	memcpy(&Params[v->offset], pData, bytes);
	return true;
}

bool VulkanEffectFile::SetMatrix(HANDLE h, const FMATRIX4 *pM)
{
	return pM ? SetValue(h, pM, sizeof(FMATRIX4)) : false;
}

// Counterpart of ID3DXEffect::GetValue. The parameter block is this object's
// own memory (Params), so this reads out of it -- there is no device query
// here and there could not be one. Bounds are checked the same way SetValue
// checks them, and for the same reason: a read past a parameter returns the
// NEXT one, which looks like the right kind of value and is the wrong one.
bool VulkanEffectFile::GetValue(HANDLE h, void *pData, UINT bytes) const
{
	if (!h || !pData) return false;

	const ShaderReflection::Var *v = (const ShaderReflection::Var *)h;
	if (v->bSampler) {
		LogErr("VulkanEffectFile::GetValue on the texture parameter '%s'",
			   v->name.c_str());
		return false;
	}
	if (v->offset + bytes > Params.size()) {
		LogErr("VulkanEffectFile::GetValue('%s'): %u bytes at offset %u exceeds "
			   "the %u-byte parameter block",
			   v->name.c_str(), bytes, v->offset, (unsigned)Params.size());
		return false;
	}
	memcpy(pData, &Params[v->offset], bytes);
	return true;
}

bool VulkanEffectFile::GetMatrix(HANDLE h, FMATRIX4 *pM) const
{
	return pM ? GetValue(h, pM, sizeof(FMATRIX4)) : false;
}

bool VulkanEffectFile::GetFloat(HANDLE h, float *pF) const
{
	// Added for vPlanet::RenderSphere and RenderCloudShadows, which read
	// eFogDensity back, scale it by the planet's distance scale for one draw
	// and then put the original value back. Same read-modify-restore shape as
	// CelSphere::RenderGridLabels's use of GetMatrix, and the same answer: the
	// value comes out of the effect's own staged parameter block, not from
	// the device, which could not answer.
	return pF ? GetValue(h, pF, sizeof(float)) : false;
}

bool VulkanEffectFile::SetVector(HANDLE h, const FVECTOR4 *pV)
{
	return pV ? SetValue(h, pV, sizeof(FVECTOR4)) : false;
}

bool VulkanEffectFile::SetFloat(HANDLE h, float f)
{
	return SetValue(h, &f, sizeof(float));
}

bool VulkanEffectFile::SetInt(HANDLE h, int i)
{
	return SetValue(h, &i, sizeof(int));
}

bool VulkanEffectFile::SetBool(HANDLE h, bool b)
{
	// A bool is 32 bits in HLSL and 32 bits in a GLSL uniform block, which is
	// the note at the head of TexFlow in VulkanEffect.h. It is written as an
	// int for that reason -- C++'s bool is one byte and would leave three
	// bytes of the shader's variable untouched.
	const int i = b ? 1 : 0;
	return SetValue(h, &i, sizeof(int));
}

bool VulkanEffectFile::SetTexture(HANDLE h, VulkanTexture *pTex)
{
	if (!h) return false;
	const ShaderReflection::Var *v = (const ShaderReflection::Var *)h;
	if (!v->bSampler) {
		LogErr("VulkanEffectFile::SetTexture on the value parameter '%s'",
			   v->name.c_str());
		return false;
	}
	// A TEXTURE PARAMETER FEEDS EVERY SAMPLER THAT NAMES IT, WHICH IS WHAT
	// D3DX DID. gTex0 alone is read by eight sampler_state blocks -- WrapS,
	// ClampS, MFDSamp, Panel0S, SimpleS, ExhaustS, RingS and Planet0S -- and
	// setting it on Windows made all eight sample the new texture, because
	// each block held a REFERENCE to the parameter rather than a copy. So one
	// SetTexture writes every binding in the join built by
	// BuildParameterTable.
	auto it = TexBindings.find(v->name);
	if (it != TexBindings.end()) {
		for (uint32_t b : it->second) if (b < 32) Textures[b] = pTex;
		return true;
	}

	// A sampler the GLSL declares that no sampler_state names -- addressed by
	// its own name rather than through a texture parameter.
	if (v->binding >= 32) return false;
	Textures[v->binding] = pTex;
	return true;
}


// ===========================================================================================
// Fetch or build the pipeline for this pass.
//
// This is what D3DX did between BeginPass and the first draw: apply the pass's
// state block. There it was a dozen SetRenderState calls that the driver
// turned into hardware state; here the same dozen values are fields of an
// immutable object that has to exist before the draw.
//
// `blendOverride` is Render2DPanel's `additive`. On Windows it was
// pDev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE) issued AFTER BeginPass,
// overriding what the pass had just set. There is no "after" here, so it is a
// key rather than a call. -1 means "use the pass's own DestBlend".
// ===========================================================================================
VkPipeline VulkanEffectFile::GetPipeline(VulkanPass *pPass, const PassOverride *ovr)
{
	if (!pPass || !pDecl) return VK_NULL_HANDLE;
	if (pPass->vs == VK_NULL_HANDLE || pPass->ps == VK_NULL_HANDLE) return VK_NULL_HANDLE;

	static const PassOverride none;
	if (!ovr) ovr = &none;

	// Read once, so the key and pi.renderPass below cannot disagree. See
	// VulkanPass::PipeKey.
	VkRenderPass rpass = pDev->GetRenderPass();
	VkPolygonMode rfill = pDev->GetPolygonMode();

	VulkanPass::PipeKey key = { pDecl, topology,
								ovr->dstBlend, ovr->blendEnable, ovr->colorWriteMask,
								ovr->depthTest, ovr->depthWrite, ovr->cullMode, rpass,
								int(rfill) };

	auto it = pPass->Pipelines.find(key);
	if (it != pPass->Pipelines.end()) return it->second;

	// The same POINT_LIST report ShaderClass::GetPipeline carries; see the
	// note there. Diagnostic only; env-gated.
	static const bool bTracePoints = (getenv("ORBITER_VK_TRACE_POINTS") != NULL);
	if (topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST && bTracePoints) {
		static std::map<std::string, int> seen;
		const std::string k = pPass->vsEntry + "/" + pPass->psEntry;
		if (seen.find(k) == seen.end()) {
			seen[k] = 1;
			LogErr("POINTTRACE fx-pass: technique=%s vs=%s ps=%s decl=%s",
				   pCurrent ? pCurrent->name.c_str() : "(none)",
				   pPass->vsEntry.c_str(), pPass->psEntry.c_str(), pDecl->Name());
		}
	}

	// THE ENTRY POINT NAME IS THE PASS'S, NOT "main".
	//
	// It said "main", and that would have failed pipeline creation for every
	// technique in the effect: pName must name an entry point the module
	// actually declares, and none of them declares "main". The .fx keeps its
	// entry-point names -- `compile vs_3_0 PBR_VS()` -- and glslang keeps
	// them too, because setEntryPoint(name) is what writes the OpEntryPoint.
	// Verified on the SPIR-V: CelSphere.glsl compiled for CelVS disassembles
	// to `OpEntryPoint Vertex %CelVS "CelVS"`.
	//
	// ShaderClass and ImageProcessing already pass their own entry names
	// here; this was the one place that did not.
	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = pPass->vs;
	stages[0].pName = pPass->vsEntry.c_str();
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = pPass->ps;
	stages[1].pName = pPass->psEntry.c_str();

	VkVertexInputBindingDescription binding = pDecl->Binding(0);
	VkVertexInputAttributeDescription attrs[16];
	const uint32_t nAttr = pDecl->Attributes(attrs, 16, 0);
	if (nAttr == 0) {
		LogErr("VulkanEffectFile: vertex declaration '%s' has more attributes "
			   "than this pipeline builder holds", pDecl->Name());
		return VK_NULL_HANDLE;
	}

	VkPipelineVertexInputStateCreateInfo vi = {};
	vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vi.vertexBindingDescriptionCount = 1;
	vi.pVertexBindingDescriptions = &binding;
	vi.vertexAttributeDescriptionCount = nAttr;
	vi.pVertexAttributeDescriptions = attrs;

	VkPipelineInputAssemblyStateCreateInfo ia = {};
	ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	ia.topology = topology;
	ia.primitiveRestartEnable = VK_FALSE;

	// The viewport and scissor are dynamic. They are the two pieces D3D9 also
	// kept outside the "state block" -- SetViewport was a device call, not a
	// render state -- and Vulkan agrees, which is why they are set on the
	// command buffer rather than baked in.
	VkPipelineViewportStateCreateInfo vp = {};
	vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	vp.viewportCount = 1;
	vp.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rs = {};
	rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rs.polygonMode = rfill;		// D3DRS_FILLMODE; see VulkanDevice::SetPolygonMode
	// D3DRS_CULLMODE, whether it comes from the .tech or from an override.
	//
	// THE TWO FIELDS ARE NOT INDEPENDENT. D3DCULL_CW and D3DCULL_CCW are the
	// SAME Vulkan cull mode -- VK_CULL_MODE_BACK_BIT -- and differ only in
	// frontFace, because "cull the clockwise triangles" and "cull the
	// counter-clockwise triangles" are one cull with opposite ideas of which
	// winding is the front. This is the same table ParseCullMode applies to
	// the .tech, written once more here for the override path; keeping them
	// in step is why the mapping is spelled out in both places rather than
	// left implicit.
	rs.cullMode = pPass->cullMode;
	rs.frontFace = pPass->frontFace;
	switch (ovr->cullMode) {
	case PassOverride::CULL_NONE:
		rs.cullMode = VkCullModeFlags(VK_CULL_MODE_NONE);
		rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
		break;
	case PassOverride::CULL_CW:
		rs.cullMode = VkCullModeFlags(VK_CULL_MODE_BACK_BIT);
		rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
		break;
	case PassOverride::CULL_CCW:
		rs.cullMode = VkCullModeFlags(VK_CULL_MODE_BACK_BIT);
		rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
		break;
	default:	// CULL_PASS -- the technique's own, already assigned above
		break;
	}
	rs.lineWidth = 1.0f;

	// A line topology is never culled. D3D9 did not cull lines either -- the
	// cull test is on triangle winding, which a line does not have -- but
	// Vulkan applies cullMode to whatever it is given, so the exemption has to
	// be stated.
	if (topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST ||
		topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP ||
		topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST)
		rs.cullMode = VK_CULL_MODE_NONE;

	VkPipelineMultisampleStateCreateInfo ms = {};
	ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo ds = {};
	ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	// ZEnable is the TEST and ZWriteEnable is the WRITE. They are separate in
	// both APIs and the .fx sets them separately -- RingTech has
	// ZWriteEnable=true with ZEnable=false, which is meaningless-looking but
	// is what the reference asks for and is preserved.
	ds.depthTestEnable = (ovr->depthTest >= 0)
		? (ovr->depthTest ? VK_TRUE : VK_FALSE)
		: (pPass->bZEnable ? VK_TRUE : VK_FALSE);
	ds.depthWriteEnable = (ovr->depthWrite >= 0)
		? (ovr->depthWrite ? VK_TRUE : VK_FALSE)
		: (pPass->bZWrite ? VK_TRUE : VK_FALSE);
	ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;	// D3DCMP_LESSEQUAL, D3D9's default
	ds.stencilTestEnable = pPass->bStencil ? VK_TRUE : VK_FALSE;
	if (pPass->bStencil) {
		VkStencilOpState so = {};
		so.failOp = VK_STENCIL_OP_KEEP;			// D3DRS_STENCILFAIL default
		so.passOp = pPass->stencilPass;
		so.depthFailOp = VK_STENCIL_OP_KEEP;	// D3DRS_STENCILZFAIL default
		so.compareOp = pPass->stencilFunc;
		so.compareMask = pPass->stencilMask;
		so.writeMask = pPass->stencilMask;
		so.reference = pPass->stencilRef;
		ds.front = so;
		ds.back = so;	// D3D9 two-sided stencil is off unless asked for
	}

	VkPipelineColorBlendAttachmentState cba = {};
	cba.blendEnable = (ovr->blendEnable >= 0)
		? (ovr->blendEnable ? VK_TRUE : VK_FALSE)
		: (pPass->bAlphaBlend ? VK_TRUE : VK_FALSE);
	cba.srcColorBlendFactor = pPass->srcBlend;
	cba.dstColorBlendFactor = (ovr->dstBlend >= 0)
		? (VkBlendFactor)ovr->dstBlend : pPass->dstBlend;
	cba.colorBlendOp = pPass->blendOp;
	// D3DRS_SEPARATEALPHABLENDENABLE is off everywhere in these techniques, so
	// alpha uses the same factors and op as colour, which is what D3D9 does
	// when the flag is clear.
	cba.srcAlphaBlendFactor = cba.srcColorBlendFactor;
	cba.dstAlphaBlendFactor = cba.dstColorBlendFactor;
	cba.alphaBlendOp = cba.colorBlendOp;
	// D3DRS_COLORWRITEENABLE. Its bits are RED|GREEN|BLUE|ALPHA = 1|2|4|8,
	// which is the same order as VK_COLOR_COMPONENT_*_BIT, so the mask
	// carries over unchanged. D3D9Pad::Flush uses 0x7 (colour only), 0x8
	// (alpha only) and 0xF (all) to implement its four blend states.
	cba.colorWriteMask = (ovr->colorWriteMask >= 0)
		? (VkColorComponentFlags)ovr->colorWriteMask
		: (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
		   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);

	// One attachment state per colour attachment -- a Vulkan requirement with
	// no D3D9 counterpart. Same addition, same reason, as ShaderClass's; see
	// VulkanDevice::GetRenderPassColourCount.
	VkPipelineColorBlendAttachmentState cbas[8];
	uint32_t nCba = pDev->GetRenderPassColourCount();
	if (nCba > ARRAYSIZE(cbas)) nCba = ARRAYSIZE(cbas);
	for (uint32_t i = 0; i < nCba; i++) cbas[i] = cba;

	VkPipelineColorBlendStateCreateInfo cb = {};
	cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	cb.attachmentCount = nCba;
	cb.pAttachments = cbas;

	const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dss = {};
	dss.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dss.dynamicStateCount = 2;
	dss.pDynamicStates = dyn;

	VkGraphicsPipelineCreateInfo gp = {};
	gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	gp.stageCount = 2;
	gp.pStages = stages;
	gp.pVertexInputState = &vi;
	gp.pInputAssemblyState = &ia;
	gp.pViewportState = &vp;
	gp.pRasterizationState = &rs;
	gp.pMultisampleState = &ms;
	gp.pDepthStencilState = &ds;
	gp.pColorBlendState = &cb;
	gp.pDynamicState = &dss;
	gp.layout = vkPipeLayout;
	// The render pass this pipeline may be bound in: the core's when drawing
	// into the swapchain image UIHost.cpp has already begun a pass on, the
	// offscreen one inside BeginOffscreen. Same handle as the cache key's.
	gp.renderPass = rpass;
	gp.subpass = 0;

	VkPipeline pipe = VK_NULL_HANDLE;
	if (vkCreateGraphicsPipelines(pDev->GetDevice(), pDev->GetPipelineCache(),
								  1, &gp, NULL, &pipe) != VK_SUCCESS) {
		LogErr("VulkanEffectFile: vkCreateGraphicsPipelines failed "
			   "(decl %s, topology %d)", pDecl->Name(), int(topology));
		return VK_NULL_HANDLE;
	}

	pPass->Pipelines[key] = pipe;
	return pipe;
}


// ===========================================================================================
// Begin / BeginPass / EndPass / End
//
// The shape is D3DX's and the call sites are unchanged. What moved is WHERE
// the work happens: D3DX applied the pass's render state at BeginPass and
// needed CommitChanges for anything set afterwards. Here BeginPass is the
// single point at which the technique, the pass, the vertex layout, the
// topology, the parameter values and the textures are ALL known, so it is
// where the pipeline is bound and the descriptor set written -- and there is
// nothing left over to commit.
// ===========================================================================================

bool VulkanEffectFile::Begin(UINT *passes, DWORD flags)
{
	// D3DXFX_DONOTSAVESTATE is what every call site passes. It told D3DX not
	// to snapshot and restore the device's render state around the effect --
	// there is no device state to snapshot here, so the flag has nothing to
	// select and is accepted and ignored rather than rejected.
	(void)flags;

	if (!pCurrent) {
		LogErr("VulkanEffectFile::Begin with no technique selected");
		if (passes) *passes = 0;
		return false;
	}
	if (passes) *passes = (UINT)pCurrent->Passes.size();
	return true;
}

bool VulkanEffectFile::BeginPass(UINT pass)
{
	return BeginPassEx(pass, NULL);
}

bool VulkanEffectFile::BeginPassEx(UINT pass, const PassOverride *ovr)
{
	static const PassOverride none;
	if (!ovr) ovr = &none;

	if (!pCurrent || pass >= pCurrent->Passes.size()) return false;
	if (!pDev->IsRecording()) {
		// No frame command buffer. On Windows a draw outside BeginScene was
		// silently dropped by the runtime; here it would be a use of a NULL
		// command buffer, so it is refused and named.
		LogErr("VulkanEffectFile::BeginPass(%u) outside the scene callback", pass);
		return false;
	}

	curPass = pass;
	VulkanPass *pPass = &pCurrent->Passes[pass];

	VkPipeline pipe = GetPipeline(pPass, ovr);
	if (pipe == VK_NULL_HANDLE) return false;

	VkCommandBuffer cmd = pDev->GetCommandBuffer();

	// Which techniques ever actually run, reported once each. Env-gated.
	if (getenv("ORBITER_VK_TRACE_TECH")) {
		static std::map<std::string, int> seen;
		if (seen.find(pCurrent->name) == seen.end()) {
			seen[pCurrent->name] = 1;
			LogErr("TECHTRACE fx-technique: %s", pCurrent->name.c_str());
		}
	}

	// Name this pass in the GPU command stream, so a device loss can be
	// attributed to a technique instead of to "somewhere in the frame". The
	// technique's name is a std::string owned by the VulkanTechnique, which
	// lives as long as this effect file, so the pointer outlives the
	// submission as orbiter_VkCheckpoint requires. No-op unless
	// ORBITER_VK_CHECKPOINTS is set.
	orbiter_VkCheckpoint(cmd, pCurrent->name.c_str());

	// ---- the parameter block, into this pass's own slice ----------------
	const VkDeviceSize align = pDev->GetProperties()->limits.minUniformBufferOffsetAlignment;
	VkDeviceSize stride = Params.size();
	if (align > 1) stride = ((stride + align - 1) / align) * align;

	if (arenaUsed + stride > arenaSize) {
		LogErr("VulkanEffectFile: the frame's uniform arena is full "
			   "(%u bytes, %u passes). ResetFrame is not being called.",
			   (unsigned)arenaSize, (unsigned)(arenaSize / (stride ? stride : 1)));
		return false;
	}

	curUniformOffset = arenaUsed;
	arenaUsed += stride;

	if (unsigned char *p = (unsigned char *)pUniformArena->Map()) {
		memcpy(p + curUniformOffset, Params.data(), Params.size());
		// Not unmapped: the arena is host-coherent and stays mapped for the
		// life of the effect. Mapping and unmapping per pass would be a
		// syscall per draw for no gain.
	}

	// ---- the descriptor set --------------------------------------------
	VkDescriptorSetAllocateInfo ai = {};
	ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	ai.descriptorPool = vkPool;
	ai.descriptorSetCount = 1;
	ai.pSetLayouts = &vkSetLayout;

	if (vkAllocateDescriptorSets(pDev->GetDevice(), &ai, &curSet) != VK_SUCCESS) {
		LogErr("VulkanEffectFile: descriptor pool exhausted this frame");
		return false;
	}

	std::vector<VkWriteDescriptorSet> writes;
	std::vector<VkDescriptorImageInfo> imgs(nSamplers ? nSamplers : 1);

	VkDescriptorBufferInfo bi = {};
	bi.buffer = pUniformArena->Buffer();
	bi.offset = 0;					// the dynamic offset supplies the rest
	bi.range = Params.size();

	{
		VkWriteDescriptorSet w = {};
		w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w.dstSet = curSet;
		w.dstBinding = 0;
		w.descriptorCount = 1;
		w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
		w.pBufferInfo = &bi;
		writes.push_back(w);
	}

	for (uint32_t i = 1; i < nSamplers; i++) {

		VulkanTexture *pTex = Textures[i];

		// EVERY sampler binding must be written, whether the technique uses
		// it or not: a descriptor left unwritten and then read is undefined
		// behaviour, and the validation layer reports it. D3D9 had no such
		// rule -- an unset texture stage sampled white. So an unset binding
		// gets the client's own 1x1 white texture, which is what sampling an
		// unset stage produced there.
		// The fallback has to match the DECLARATION's dimension, not just be
		// white: a cube slot given a 2D view faults the GPU. See bCubeBind.
		if (!pTex) pTex = (i < ARRAYSIZE(bCubeBind) && bCubeBind[i] && pWhiteCube)
						  ? pWhiteCube : pWhite;
		if (!pTex || pTex->View() == VK_NULL_HANDLE) continue;

		// D3DSAMP_MIN/MAGFILTER, which D3D9Pad::Flush sets per draw. A
		// VkSampler is an object rather than device state, so "change the
		// filter" is "pick a different sampler".
		//
		// With no override the binding's OWN sampler is used -- the one built
		// from the .fx sampler_state block that declared it, carrying that
		// block's filtering and addressing. vkSampler is the fallback for a
		// binding the .tech does not describe.
		imgs[i].sampler = (ovr->filter >= 0 && ovr->filter < 3)
			? vkSamplers[ovr->filter]
			: (vkBindSampler[i] ? vkBindSampler[i] : vkSampler);
		imgs[i].imageView = pTex->View();
		imgs[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		VkWriteDescriptorSet w = {};
		w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w.dstSet = curSet;
		w.dstBinding = i;
		w.descriptorCount = 1;
		w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		w.pImageInfo = &imgs[i];
		writes.push_back(w);
	}

	vkUpdateDescriptorSets(pDev->GetDevice(), (uint32_t)writes.size(),
						   writes.data(), 0, NULL);

	// ---- bind ------------------------------------------------------------
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);

	const uint32_t dynOffset = (uint32_t)curUniformOffset;
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipeLayout,
							0, 1, &curSet, 1, &dynOffset);

	// The viewport and scissor, which are dynamic. The extent is the frame's,
	// which the scene callback was handed -- the counterpart of the
	// D3DVIEWPORT9 CD3DFramework9::Initialize set once at start-up.
	//
	// THE HEIGHT IS NEGATIVE, AND THAT IS THE Y AXIS OF CLIP SPACE.
	// See the note in VulkanUtil.cpp's ShaderClass::Setup.
	VkViewport vpt = {};
	vpt.x = 0.0f;
	vpt.y = float(pDev->GetFrameHeight());
	vpt.width = float(pDev->GetFrameWidth());
	vpt.height = -float(pDev->GetFrameHeight());
	vpt.minDepth = 0.0f;
	vpt.maxDepth = 1.0f;
	vkCmdSetViewport(cmd, 0, 1, &vpt);

	// The scissor. D3DRS_SCISSORTESTENABLE plus SetScissorRect on Windows;
	// here the test is always on and "disabled" is a rectangle covering the
	// whole target, which is the same thing and one fewer piece of state.
	VkRect2D sc = {};
	if (ovr->scissor) {
		LONG l = ovr->scissor->left,   t = ovr->scissor->top;
		LONG r = ovr->scissor->right,  b = ovr->scissor->bottom;
		if (l < 0) l = 0;
		if (t < 0) t = 0;
		if (r > (LONG)pDev->GetFrameWidth())  r = (LONG)pDev->GetFrameWidth();
		if (b > (LONG)pDev->GetFrameHeight()) b = (LONG)pDev->GetFrameHeight();
		if (r < l) r = l;
		if (b < t) b = t;
		sc.offset = { (int32_t)l, (int32_t)t };
		sc.extent = { (uint32_t)(r - l), (uint32_t)(b - t) };
	}
	else {
		sc.offset = { 0, 0 };
		sc.extent = { pDev->GetFrameWidth(), pDev->GetFrameHeight() };
	}
	vkCmdSetScissor(cmd, 0, 1, &sc);

	return true;
}

bool VulkanEffectFile::EndPass()
{
	// D3DX restored the state the pass had changed. Nothing to restore: a
	// pipeline is replaced by the next bind rather than layered over.
	return true;
}

bool VulkanEffectFile::End()
{
	return true;
}

// ===========================================================================================
//
void VulkanEffectFile::ResetFrame()
{
	if (!pDev) return;

	frameNo++;

	// How long a frame's resources must be left alone. The core answers it;
	// see orbiter_GetFramesInFlight and the FrameSet note in the header.
	unsigned long long lag = orbiter_GetFramesInFlight();
	if (lag < 2) lag = 2;

	// ---- retire what this frame used -----------------------------------
	if (vkPool != VK_NULL_HANDLE || pUniformArena || pScratchVB || pScratchIB) {
		FrameSet used;
		used.pool			= vkPool;
		used.arena			= pUniformArena;
		used.arenaSize		= arenaSize;
		used.scratchVB		= pScratchVB;
		used.scratchVBSize	= scratchVBSize;
		used.scratchIB		= pScratchIB;
		used.scratchIBSize	= scratchIBSize;
		used.retiredAt		= frameNo;
		retired.push_back(used);
	}

	vkPool = VK_NULL_HANDLE;
	pUniformArena = NULL;	arenaSize = 0;
	pScratchVB = NULL;		scratchVBSize = 0;
	pScratchIB = NULL;		scratchIBSize = 0;

	// ---- take back the oldest set, if the GPU is certainly done with it --
	if (!retired.empty() && retired.front().retiredAt + lag <= frameNo) {
		FrameSet f = retired.front();
		retired.pop_front();
		vkPool			= f.pool;
		pUniformArena	= f.arena;	arenaSize		= f.arenaSize;
		pScratchVB		= f.scratchVB;	scratchVBSize	= f.scratchVBSize;
		pScratchIB		= f.scratchIB;	scratchIBSize	= f.scratchIBSize;
		if (vkPool != VK_NULL_HANDLE)
			vkResetDescriptorPool(pDev->GetDevice(), vkPool, 0);
	}
	else {
		// Not round yet -- this only happens for the first `lag` frames of a
		// session, after which the rotation is closed and nothing more is
		// allocated.
		if (!CreateFrameSet()) {
			static bool bSaid = false;
			if (!bSaid) {
				bSaid = true;
				LogErr("VulkanEffectFile: could not build a frame's pool and arena");
			}
		}
	}

	arenaUsed = 0;
	curSet = VK_NULL_HANDLE;
	scratchVBUsed = 0;
	scratchIBUsed = 0;

	// Scratch buffers a mid-frame grow replaced. Freed on the SAME lag, and
	// for the same reason: the frame that recorded a bind naming one of them
	// may still be executing. Freeing them at the next ResetFrame is what
	// produced VUID-vkDestroyBuffer-buffer-00922.
	while (!retiredScratch.empty() && retiredScratch.front().second + lag <= frameNo) {
		pDev->DestroyBuffer(retiredScratch.front().first);
		retiredScratch.pop_front();
	}
}


// ===========================================================================================
// The scratch buffers DrawUP copies into.
//
// Grown on demand and never shrunk, because the sizes involved are small and
// bounded: the largest user-memory draw in the client is Render2DPanel's mesh
// group, and a panel mesh is a few hundred vertices.
// ===========================================================================================
bool VulkanEffectFile::EnsureScratch(VkDeviceSize vbytes, VkDeviceSize ibytes)
{
	// THE OLD BUFFER IS RETIRED, NOT DESTROYED, and that one word is the
	// difference between a frame and a dead device.
	//
	// This used to call DestroyBuffer the moment a larger buffer was needed.
	// A grow happens DURING a frame, and by then earlier DrawUP calls have
	// already recorded vkCmdBindVertexBuffers naming the old buffer into the
	// frame's command buffer. Freeing it there is a use-after-free the driver
	// discovers at submit:
	//
	//     VUID-vkCmdBindVertexBuffers-commandBuffer-recording
	//     ... VkBuffer ... was destroyed
	//     Orbiter: Vulkan error in vkQueueSubmit: -4   (VK_ERROR_DEVICE_LOST)
	//
	// D3D9 had no such hazard: the runtime owned the buffer behind
	// DrawPrimitiveUP and kept it alive as long as the command stream needed
	// it. Retiring to a list freed at the next ResetFrame() is that guarantee,
	// restored in the one place that can give it.
	if (vbytes > scratchVBSize) {
		if (pScratchVB) retiredScratch.push_back(std::make_pair(pScratchVB, frameNo));
		scratchVBSize = (vbytes + scratchVBSize) * 2;
		pScratchVB = pDev->CreateBuffer(scratchVBSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
		if (!pScratchVB) { scratchVBSize = 0; return false; }
		// A fresh buffer starts empty; anything handed out of the old one is
		// still recorded against the old one, which is why it is kept alive.
		scratchVBUsed = 0;
	}
	if (ibytes > scratchIBSize) {
		if (pScratchIB) retiredScratch.push_back(std::make_pair(pScratchIB, frameNo));
		scratchIBSize = (ibytes + scratchIBSize) * 2;
		pScratchIB = pDev->CreateBuffer(scratchIBSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
		if (!pScratchIB) { scratchIBSize = 0; return false; }
		scratchIBUsed = 0;
	}
	return true;
}


// ===========================================================================================
// Counterpart of DrawIndexedPrimitiveUP and DrawPrimitiveUP.
//
// D3D9's "UP" calls take a pointer into the caller's memory. Vulkan has no
// such draw and cannot have one: a draw sources vertices from a VkBuffer that
// the command buffer names, and the GPU reads that buffer when it executes,
// long after the caller's stack frame is gone. So the data is copied into a
// buffer -- which is what the D3D9 runtime did behind DrawPrimitiveUP too,
// just without saying so.
//
// EACH CALL GETS ITS OWN BYTES, which the previous version did not give and
// which is not optional.
//
// It copied every batch to offset 0 of one buffer, on the reasoning that each
// caller "draws immediately and never revisits". That reasoning does not hold
// in Vulkan: recording vkCmdDraw does not consume the vertices, it only names
// the buffer. The GPU reads them at SUBMIT -- so with one shared offset, every
// DrawUP in a frame ends up drawing whatever the LAST one copied. The draws
// were right, the data underneath them was not.
//
// D3D9 had no such problem because the runtime took its own copy per
// DrawPrimitiveUP call. This restores that: a bump allocator over the scratch
// buffers, handing each call its own slice and binding at that offset, rewound
// once per frame by ResetFrame(). It is the allocator the previous comment
// said the next caller would need.
// ===========================================================================================
void VulkanEffectFile::DrawUP(const void *pVtx, UINT nVtx, UINT stride,
							  const WORD *pIdx, UINT nIdx)
{
	if (!pVtx || nVtx == 0 || stride == 0) return;
	if (!pDev->IsRecording()) return;

	const VkDeviceSize vbytes = VkDeviceSize(nVtx) * stride;
	const VkDeviceSize ibytes = pIdx ? VkDeviceSize(nIdx) * sizeof(WORD) : 0;

	// A vertex-buffer binding offset must be a multiple of the attribute
	// alignment; 16 covers every vertex format this client declares. An index
	// offset must be a multiple of the index size, which 16 also covers.
	auto alignUp = [](VkDeviceSize v) -> VkDeviceSize { return (v + 15) & ~VkDeviceSize(15); };

	const VkDeviceSize vOff = alignUp(scratchVBUsed);
	const VkDeviceSize iOff = alignUp(scratchIBUsed);

	if (!EnsureScratch(vOff + vbytes, ibytes ? (iOff + ibytes) : 2)) return;

	// EnsureScratch may have grown -- and therefore rewound -- either buffer,
	// so the offsets are re-read rather than reused from above.
	const VkDeviceSize vAt = alignUp(scratchVBUsed);
	const VkDeviceSize iAt = alignUp(scratchIBUsed);

	if (char *p = (char *)pScratchVB->Map()) memcpy(p + vAt, pVtx, (size_t)vbytes);
	if (pIdx && ibytes) {
		if (char *p = (char *)pScratchIB->Map()) memcpy(p + iAt, pIdx, (size_t)ibytes);
	}

	VkCommandBuffer cmd = pDev->GetCommandBuffer();

	VkBuffer vb = pScratchVB->Buffer();
	VkDeviceSize offset = vAt;
	vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);

	if (pIdx && nIdx) {
		// VK_INDEX_TYPE_UINT16 is D3DFMT_INDEX16, which every index buffer in
		// this client uses -- WORD throughout.
		vkCmdBindIndexBuffer(cmd, pScratchIB->Buffer(), iAt, VK_INDEX_TYPE_UINT16);
		vkCmdDrawIndexed(cmd, nIdx, 1, 0, 0, 0);
		scratchIBUsed = iAt + ibytes;
	}
	else {
		vkCmdDraw(cmd, nVtx, 1, 0, 0);
	}

	scratchVBUsed = vAt + vbytes;
}


// ###########################################################################
//
//   VulkanEffect -- the conversion of D3D9Effect
//
// ###########################################################################

// ===========================================================================================
//
VulkanEffect::VulkanEffect() : vulkanid(0x564C4B4E)		// 'V','L','K','N'
{
	// Was d3d9id('D3D9'). A multi-character literal is implementation-defined
	// -- GCC reports it as -Wmultichar -- so the tag is written as the number
	// MSVC would have produced, exactly as VulkanPolyBase::ALLOC_ID_POLY is.
}

// ===========================================================================================
//
VulkanEffect::~VulkanEffect()
{

}

// ===========================================================================================
//
void VulkanEffect::GlobalExit()
{
	LogAlw("====== VulkanEffect Global Exit =======");

	// Was SAFE_RELEASE(FX) and SAFE_RELEASE(VB). Vulkan objects are not
	// reference counted: the effect owns its pipelines and layouts and
	// destroys them in its destructor, and the device that made the buffer
	// destroys the buffer.
	delete FX;
	FX = NULL;

	if (VB && pDev) { pDev->DestroyBuffer(VB); VB = NULL; }
}

// ===========================================================================================
//
void VulkanEffect::ShutDown()
{
	if (!FX) return;
	FX->SetTexture(eTex0, NULL);
	FX->SetTexture(eTex1, NULL);
	FX->SetTexture(eTex3, NULL);
	FX->SetTexture(eSpecMap, NULL);
	FX->SetTexture(eEmisMap, NULL);
}


// ===========================================================================================
//
void VulkanEffect::VulkanTechInit(VulkanClient *_gc, VulkanDevice *_pDev, const char *folder)
{
	char name[256];

	pDev = _pDev;
	gc = _gc;

	gc->OutputLoadStatus("VulkanClient.glsl", 1);

	LogAlw("Starting to initialize VulkanClient.glsl a rendering technique...");

	// Was D3DXCreateEffectFromFileA with an 18-entry D3DXMACRO array built by
	// hand, six of whose Definitions were new char[32] and deleted afterwards.
	// The macros are now a preprocessor option string built inside
	// VulkanEffectFile::Load, which is where the file that consumes them is
	// read -- so there is no array to size, no allocation to match with a
	// delete, and nothing to get wrong about `memset(&macro, 0, 16*...)`
	// clearing sixteen of eighteen entries.
	sprintf_s(name, 256, "Modules/VulkanClient/VulkanClient.glsl");

	FX = new VulkanEffectFile(pDev);

	if (!FX->Load(name)) {
		// Was: MessageBoxA + FatalAppExitA(0, "Critical error has occured...").
		// FatalAppExitA is a Win32 entry point that terminates the process
		// without unwinding; the shim does not supply it and should not.
		// oapiWriteLog + the message box says the same thing to the user, and
		// returning lets the caller's failure path run -- which on this
		// platform means the session ends and the Launchpad comes back rather
		// than the process vanishing.
		LogErr("Failed to create an Effect (%s)", name);
		oapiWriteLog((char*)"Vulkan: FAIL: VulkanClient.glsl did not compile. "
						   "See Orbiter.log for details");
		MessageBoxA(0, "VulkanClient.glsl failed to compile. See Orbiter.log for details.",
					"VulkanClient.glsl Error", 0);
		delete FX;
		FX = NULL;
		return;
	}

	// The Config->ShaderDebug block stood here and disassembled the effect
	// with D3DXDisassembleEffect into D9D9Effect_asm.html. Its counterpart is
	// the SPIR-V disassembly glslang can emit, and VulkanUtil.cpp's
	// CompileShaderStage already writes it when Config->ShaderDebug is set --
	// per shader, where the compiler is, rather than per effect afterwards.

	// Techniques --------------------------------------------------------------

	eHazeTech	 = FX->GetTechniqueByName("HazeTech");
	ePlanetTile  = FX->GetTechniqueByName("PlanetTech");
	eBaseTile    = FX->GetTechniqueByName("BaseTileTech");
	eSimple      = FX->GetTechniqueByName("SimpleTech");
	eBBTech		 = FX->GetTechniqueByName("BoundingBoxTech");
	eTBBTech	 = FX->GetTechniqueByName("TileBoxTech");
	eBSTech		 = FX->GetTechniqueByName("BoundingSphereTech");
	eRingTech    = FX->GetTechniqueByName("RingTech");
	eRingTech2   = FX->GetTechniqueByName("RingTech2");
	eExhaust     = FX->GetTechniqueByName("ExhaustTech");
	eSpotTech    = FX->GetTechniqueByName("SpotTech");
	eShadowTech  = FX->GetTechniqueByName("ShadowTech");
	ePanelTech	 = FX->GetTechniqueByName("PanelTech");
	ePanelTechB	 = FX->GetTechniqueByName("PanelTechB");
	eCloudTech   = FX->GetTechniqueByName("PlanetCloudTech");
	eCloudShadow = FX->GetTechniqueByName("PlanetCloudShadowTech");
	eArrowTech   = FX->GetTechniqueByName("ArrowTech");
	eAxisTech    = FX->GetTechniqueByName("AxisTech");
	eSimpMesh	 = FX->GetTechniqueByName("SimplifiedTech");
	eGeometry    = FX->GetTechniqueByName("GeometryTech");
	eVesselTech		 = FX->GetTechniqueByName("VesselTech");
	eBeaconArrayTech = FX->GetTechniqueByName("BeaconArrayTech");
	eDiffuseTech     = FX->GetTechniqueByName("ParticleDiffuseTech");
	eEmissiveTech    = FX->GetTechniqueByName("ParticleEmissiveTech");

	// TWO LOOKUPS ARE GONE, AND THEY WERE ALREADY DEAD ON WINDOWS.
	//
	//     eSkyDomeTech    = FX->GetTechniqueByName("SkyDomeTech");
	//     eBaseShadowTech = FX->GetTechniqueByName("BaseShadowTech");
	//
	// NEITHER TECHNIQUE EXISTS. Searched across all eleven .fx files in
	// OVP/D3D9Client/shaders -- D3D9Client.fx, Particle.fx, Mesh.fx,
	// Vessel.fx, HorizonHaze.fx, Planet.fx, BeaconArray.fx, SceneTech.fx,
	// Sketchpad.fx, PBR.fx, Metalness.fx -- and the names appear nowhere but
	// in D3D9Effect.cpp and D3D9Effect.h themselves. GetTechniqueByName
	// returns NULL for a name it does not know, silently, so both handles
	// have been NULL for the life of the client; and nothing reads either
	// one, so nothing noticed.
	//
	// The two members stay declared, because removing them would be a change
	// to the class's shape for no gain, and they stay NULL -- which is what
	// they already were. Should either technique ever be written, one line
	// here restores it.

	// Flow Control Booleans -----------------------------------------------
	eModAlpha	  = FX->GetParameterByName("gModAlpha");
	eFullyLit	  = FX->GetParameterByName("gFullyLit");
	eFlow		  = FX->GetParameterByName("gCfg");
	eShadowToggle = FX->GetParameterByName("gShadowsEnabled");
	eEnvMapEnable = FX->GetParameterByName("gEnvMapEnable");
	eTextured	  = FX->GetParameterByName("gTextured");
	eFresnel      = FX->GetParameterByName("gFresnel");
	eSwitch		  = FX->GetParameterByName("gPBRSw");
	eRghnSw		  = FX->GetParameterByName("gRghnSw");
	eInSpace	  = FX->GetParameterByName("gInSpace");
	eNoColor	  = FX->GetParameterByName("gNoColor");
	eLightsEnabled = FX->GetParameterByName("gLightsEnabled");
	eTuneEnabled  = FX->GetParameterByName("gTuneEnabled");
	eBaseBuilding = FX->GetParameterByName("gBaseBuilding");
	eOITEnable	  = FX->GetParameterByName("gOITEnable");

	// General parameters --------------------------------------------------
	eSpecularMode = FX->GetParameterByName("gSpecMode");
	eLights		  = FX->GetParameterByName("gLights");
	eColor		  = FX->GetParameterByName("gColor");
	eDistScale    = FX->GetParameterByName("gDistScale");
	eProxySize    = FX->GetParameterByName("gProxySize");
	eTexOff		  = FX->GetParameterByName("gTexOff");
	eRadius       = FX->GetParameterByName("gRadius");
	eCameraPos	  = FX->GetParameterByName("gCameraPos");
	eNorth		  = FX->GetParameterByName("gNorth");
	eEast		  = FX->GetParameterByName("gEast");
	ePointScale   = FX->GetParameterByName("gPointScale");
	eMix		  = FX->GetParameterByName("gMix");
	eTime		  = FX->GetParameterByName("gTime");
	eMtrlAlpha	  = FX->GetParameterByName("gMtrlAlpha");
	eGlowConst    = FX->GetParameterByName("gGlowConst");
	eSHD		  = FX->GetParameterByName("gSHD");
	eKernel		  = FX->GetParameterByName("kernel");
	eAtmoParams	  = FX->GetParameterByName("gAtmo");
	// ----------------------------------------------------------------------
	eVP			  = FX->GetParameterByName("gVP");
	eW			  = FX->GetParameterByName("gW");
	eLVP		  = FX->GetParameterByName("gLVP");
	eGT			  = FX->GetParameterByName("gGrpT");
	// ----------------------------------------------------------------------
	eSun		  = FX->GetParameterByName("gSun");
	eMat		  = FX->GetParameterByName("gMat");
	eWater		  = FX->GetParameterByName("gWater");
	eMtrl		  = FX->GetParameterByName("gMtrl");
	eTune		  = FX->GetParameterByName("gTune");
	// ----------------------------------------------------------------------
	eTex0		  = FX->GetParameterByName("gTex0");
	eTex1		  = FX->GetParameterByName("gTex1");
	eTex3		  = FX->GetParameterByName("gTex3");
	eSpecMap	  = FX->GetParameterByName("gSpecMap");
	eEmisMap	  = FX->GetParameterByName("gEmisMap");
	eEnvMapA	  = FX->GetParameterByName("gEnvMapA");
	eEnvMapB	  = FX->GetParameterByName("gEnvMapB");
	eReflMap	  = FX->GetParameterByName("gReflMap");
	eRghnMap	  = FX->GetParameterByName("gRghnMap");
	eMetlMap	  = FX->GetParameterByName("gMetlMap");
	eHeatMap	  = FX->GetParameterByName("gHeatMap");
	eShadowMap	  = FX->GetParameterByName("gShadowMap");
	eTranslMap	  = FX->GetParameterByName("gTranslMap");
	eTransmMap	  = FX->GetParameterByName("gTransmMap");
	eIrradMap     = FX->GetParameterByName("gIrradianceMap");

	// Atmosphere -----------------------------------------------------------
	eGlobalAmb	  = FX->GetParameterByName("gGlobalAmb");
	eSunAppRad	  = FX->GetParameterByName("gSunAppRad");
	eAmbient0	  = FX->GetParameterByName("gAmbient0");
	eDispersion	  = FX->GetParameterByName("gDispersion");
	eFogDensity	  = FX->GetParameterByName("gFogDensity");
	eAttennuate	  = FX->GetParameterByName("gAttennuate");
	eInScatter	  = FX->GetParameterByName("gInScatter");
	eInvProxySize = FX->GetParameterByName("gInvProxySize");
	eFogColor	  = FX->GetParameterByName("gFogColor");
	eAtmColor	  = FX->GetParameterByName("gAtmColor");
	eHazeMode	  = FX->GetParameterByName("gHazeMode");
	eNight		  = FX->GetParameterByName("gNightTime");

	// Initialize default values --------------------------------------
	//
	FX->SetInt(eHazeMode, 0);
	FX->SetBool(eInSpace, false);
	FX->SetVector(eAttennuate, ptr(FVECTOR4(1,1,1,1)));
	FX->SetVector(eInScatter,  ptr(FVECTOR4(0,0,0,0)));
	FX->SetVector(eColor, ptr(FVECTOR4(0, 0, 0, 0)));

	FX->SetValue(eKernel, &shadow_kernel, sizeof(shadow_kernel));

	CreateMatExt(&_mfdmat, &mfdmat);
	CreateMatExt(&_defmat, &defmat);
	CreateMatExt(&_night_mat, &night_mat);
	CreateMatExt(&_emissive_mat, &emissive_mat);

	// Create a Circle Mesh --------------------------------------------
	//
	// Was CreateVertexBuffer(256 * sizeof(D3DXVECTOR3), 0, 0, D3DPOOL_DEFAULT)
	// followed by Lock/fill/Unlock. The buffer is host-visible here rather
	// than default-pool-plus-lock, which is the same thing said in Vulkan's
	// terms: D3DPOOL_DEFAULT with a lock meant "the driver will find you
	// somewhere writable", and this asks for it directly.
	if (!VB) {
		VB = pDev->CreateBuffer(256 * sizeof(FVECTOR3),
								VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
		if (VB) {
			FVECTOR3 *pVert = (FVECTOR3 *)VB->Map();
			if (pVert) {
				float angle = 0.0f, step = float(PI2) / 255.0f;
				pVert[0] = FVECTOR3(0, 0, 0);
				for (int i = 1; i < 256; i++) {
					pVert[i].x = 0;
					pVert[i].y = cos(angle);
					pVert[i].z = sin(angle);
					angle += step;
				}
				VB->Unmap();
			}
			else LogErr("Failed to Map vertex buffer");
		}
		else LogErr("Failed to create the circle vertex buffer");
	}

	(void)folder;	// unused on Windows too: the path is spelled out above
}


void VulkanEffect::SetViewProjMatrix(FMATRIX4 *pVP)
{
	FX->SetMatrix(eVP, pVP);
}


// ===========================================================================================
// Unchanged apart from the vector types. Every line of this is Orbiter SDK
// arithmetic -- oapiGetGlobalPos, oapiGetPlanetAtmConstants, the scattering
// terms -- and none of it ever touched Direct3D.
//
// D3DXVEC(v) built a D3DXVECTOR3 from a VECTOR3 and D3DXVEC4 a D3DXVECTOR4
// from a colour; both are _FV / _FV4 here, which VulkanUtil.h defines with the
// same bodies.
// ===========================================================================================
void VulkanEffect::UpdateEffectCamera(OBJHANDLE hPlanet)
{
	if (!hPlanet) return;
	VECTOR3 cam, pla, sun;
	OBJHANDLE hSun = oapiGetGbodyByIndex(0); // generalise later
	cam = gc->GetScene()->GetCameraGPos();
	oapiGetGlobalPos(hPlanet, &pla);
	oapiGetGlobalPos(hSun, &sun);

	double len  = length(cam - pla);
	double rad  = oapiGetSize(hPlanet);

	sun = unit(sun - cam);	// Vector pointing to sun from camera
	cam = unit(cam - pla);	// Vector pointing to cam from planet

	DWORD width, height;
	oapiGetViewportSize(&width, &height); // BUG:  Custom Camera may have different view size


	float radlimit = float(rad) + 1.0f;
	float rho0 = 1.0f;

	atm_color = FVECTOR4(0.5f, 0.5f, 0.5f, 1.0f);

	const ATMCONST *atm = oapiGetPlanetAtmConstants(hPlanet);
	VESSEL *hVessel = oapiGetFocusInterface();

	// THE NULL TEST IS TOO LATE AND IS IN THE WINDOWS SOURCE TOO.
	//
	// hVessel->GetGravityRef() is called on the next line; the
	// `if (hVessel==NULL)` guard is twelve lines further down. A NULL focus
	// interface therefore dereferences before it is tested. The test is moved
	// up here, which is the only change: it is the same test, doing what it
	// was written to do.
	if (hVessel == NULL) {
		LogErr("hVessel = NULL in UpdateEffectCamera()");
		return;
	}

	OBJHANDLE hGRef = hVessel->GetGravityRef();
	MATRIX3 grot;

	oapiGetRotationMatrix(hGRef, &grot);

	VECTOR3 polaraxis = mul(grot, _V(0, 1, 0));
	VECTOR3 east = unit(crossp(polaraxis, cam));
	VECTOR3 north = unit(crossp(cam, east));

	if (atm) {
		radlimit = float(atm->radlimit);
		atm_color = FVECTOR4(float(atm->color0.x), float(atm->color0.y),
							 float(atm->color0.z), 1.0f);
		rho0 = float(atm->rho0);
	}

	float av = (atm_color.x + atm_color.y + atm_color.z) * 0.3333333f;
	float fc = 1.5f;
	float alt = 1.0f - pow(float(hVessel->GetAtmDensity()/rho0), 0.2f);

	// Was three lines of D3DXVECTOR4 arithmetic:
	//     atm_color += D3DXVECTOR4(av,av,av,1.0)*fc;
	//     atm_color *= 1.0f/(fc+1.0f);
	//     atm_color *= float(Config->PlanetGlow) * alt;
	//
	// FVECTOR4 has no operator+= and its operator* against a float is
	// ambiguous under ISO rules where D3DXVECTOR4's was not -- GCC reports
	// both. Written out per component, which is the same arithmetic in the
	// same order and cannot be read two ways.
	{
		const float k = 1.0f / (fc + 1.0f);
		const float g = float(Config->PlanetGlow) * alt;
		atm_color.x = (atm_color.x + av * fc) * k * g;
		atm_color.y = (atm_color.y + av * fc) * k * g;
		atm_color.z = (atm_color.z + av * fc) * k * g;
		atm_color.w = (atm_color.w + 1.0f * fc) * k * g;
	}

	float ap = gc->GetScene()->GetCameraAperture();
	float rl = float(rad/len);
	const float rlc = (rl < 1.0f) ? rl : 1.0f;
	float proxy_size = asin(rlc) + float(40.0*PI/180.0);

	if (rl>1e-3) {
		const float s = pow(rl, 1.5f);
		atm_color.x *= s; atm_color.y *= s; atm_color.z *= s; atm_color.w *= s;
	}
	else atm_color = FVECTOR4(0,0,0,1);

	FVECTOR3 fEast(float(east.x), float(east.y), float(east.z));
	FVECTOR3 fNorth(float(north.x), float(north.y), float(north.z));
	FVECTOR3 fCam(float(cam.x), float(cam.y), float(cam.z));

	FX->SetValue(eEast, &fEast, sizeof(FVECTOR3));
	FX->SetValue(eNorth, &fNorth, sizeof(FVECTOR3));
	FX->SetValue(eCameraPos, &fCam, sizeof(FVECTOR3));
	FX->SetVector(eRadius, ptr(FVECTOR4((float)rad, radlimit, (float)len, (float)(len-rad))));
	FX->SetFloat(ePointScale, 0.5f*float(height)/tan(ap));
	FX->SetFloat(eProxySize, cos(proxy_size));
	FX->SetFloat(eInvProxySize, 1.0f/(1.0f-cos(proxy_size)));
	FX->SetFloat(eGlowConst, saturate(float(dotp(cam, sun))));
}


// ===========================================================================================
//
void VulkanEffect::EnablePlanetGlow(bool bEnabled)
{
	if (bEnabled) FX->SetVector(eAtmColor, &atm_color);
	else FX->SetVector(eAtmColor, ptr(FVECTOR4(0,0,0,0)));
}


// ===========================================================================================
//
void VulkanEffect::InitLegacyAtmosphere(OBJHANDLE hPlanet, float GlobalAmbient)
{
	VECTOR3 GS, GP;

	OBJHANDLE hS = oapiGetGbodyByIndex(0);	// the central star
	oapiGetGlobalPos (hS, &GS);				// sun position
	oapiGetGlobalPos (hPlanet, &GP);		// planet position

	float rs = (float)(oapiGetSize(hS) / length(GS-GP));

	const ATMCONST *atm = (oapiGetObjectType(hPlanet)==OBJTP_PLANET ? oapiGetPlanetAtmConstants (hPlanet) : NULL);

	FX->SetFloat(eGlobalAmb, GlobalAmbient);
	FX->SetFloat(eSunAppRad, rs);

	if (atm) {
		// min/max on two doubles. The shim supplies neither as a macro, and
		// std::min/std::max would need both arguments the same type -- log1p
		// returns double and 0.7 is a double, so these are written out.
		const double a0 = log1p(atm->rho0) * 0.4;
		const double d0 = log1p(atm->rho0);
		FX->SetFloat(eAmbient0, float(a0 < 0.7 ? a0 : 0.7));
		FX->SetFloat(eDispersion, float(d0 < 0.02 ? 0.02 : (d0 > 0.9 ? 0.9 : d0)));
	}
	else {
		FX->SetFloat(eAmbient0, 0.0f);
		FX->SetFloat(eDispersion, 0.0f);
	}
}


// ===========================================================================================
//
void VulkanEffect::Render2DPanel(const MESHGROUP *mg, const SURFHANDLE pTex, const FMATRIX4 *pW, float alpha, float scale, bool additive)
{
	UINT numPasses = 0;
	if (!pTex || !mg || !pW) return;

	// IsLimited() is false on this client -- see VulkanClient.h -- so the
	// POINT-filter fallback for non-power-of-two conditional hardware is
	// unreachable. It is kept because the first half of the test is not:
	// a non-power-of-two panel texture still selects PanelTech here, which is
	// the same decision the reference makes.
	if (SURFACE(pTex)->IsPowerOfTwo() || (!gc->IsLimited())) FX->SetTechnique(ePanelTech);
	else FX->SetTechnique(ePanelTechB);

	FX->SetMatrix(eW, pW);

	if (pTex) FX->SetTexture(eTex0, SURFACE(pTex)->GetTexture());
	else      FX->SetTexture(eTex0, NULL);

	FX->SetFloat(eMix, alpha);

	// Was pDev->SetVertexDeclaration(pNTVertexDecl) plus, between BeginPass
	// and the draw, pDev->SetRenderState(D3DRS_DESTBLEND, additive ? ONE :
	// INVSRCALPHA). Both are pipeline state here, so both are declared before
	// the pipeline is bound. See GetPipeline's blendOverride.
	FX->SetVertexDecl(pNTVertexDecl);
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);

	if (!FX->Begin(&numPasses, 0)) return;

	// additive is the blend override. It cannot be applied after BeginPass
	// because there is no "after": the pipeline is immutable once bound.
	VulkanEffectFile::PassOverride ovr;
	ovr.dstBlend = additive ? int(VK_BLEND_FACTOR_ONE)
							: int(VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);

	if (!FX->BeginPassEx(0, &ovr)) {
		FX->End();
		return;
	}

	FX->DrawUP(mg->Vtx, mg->nVtx, sizeof(NTVERTEX), mg->Idx, mg->nIdx);

	FX->EndPass();
	FX->End();

	(void)scale;	// unused on Windows too
}


// ===========================================================================================
//
void VulkanEffect::RenderReEntry(const SURFHANDLE pTex, const FVECTOR3 *vPosA, const FVECTOR3 *vPosB, const FVECTOR3 *vDir, float alpha_a, float alpha_b, float size)
{
	static WORD ReentryIdx[6] = {0,1,2, 3,2,1};

	static NTVERTEX ReentryVtxB[4] = {
		{0,-1,-1,   0,0,0, 0.51f, 0.01f},
		{0,-1, 1,   0,0,0, 0.99f, 0.01f},
		{0, 1,-1,   0,0,0, 0.51f, 0.49f},
		{0, 1, 1,   0,0,0, 0.99f, 0.49f}
	};

	float x = 4.5f + sin(fmod(float(oapiGetSimTime())*60.0f, 6.283185f)) * 0.5f;

	NTVERTEX ReentryVtxA[4] = {
		{0, 1, 1, 0,0,0, 0.49f, 0.01f},
		{0, 1,-x, 0,0,0, 0.49f, 0.99f},
		{0,-1, 1, 0,0,0, 0.01f, 0.01f},
		{0,-1,-x, 0,0,0, 0.01f, 0.99f},
	};

	UINT numPasses = 0;
	FMATRIX4 WA, WB;
	FVECTOR3 vCam;

	// Was D3DXVec3Normalize(&vCam, vPosA). VectorHelpers.h's unit() is the
	// same operation on the SDK's own type.
	vCam = unit(*vPosA);

	VMAT_CreateX_Billboard(&vCam, vPosB, size*(0.8f+x*0.02f), &WB);
	VMAT_CreateX_Billboard(&vCam, vPosA, vDir, size, size, &WA);

	FX->SetVertexDecl(pNTVertexDecl);
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);

	FX->SetTechnique(eExhaust);
	FX->SetTexture(eTex0, SURFACE(pTex)->GetTexture());
	FX->SetFloat(eMix, alpha_b);
	FX->SetMatrix(eW, &WB);

	if (!FX->Begin(&numPasses, 0)) return;
	if (!FX->BeginPass(0)) { FX->End(); return; }

	FX->DrawUP(&ReentryVtxB, 4, sizeof(NTVERTEX), ReentryIdx, 6);

	// Was: SetFloat + SetMatrix + FX->CommitChanges() + a second draw inside
	// the SAME pass. CommitChanges existed because D3DX buffered parameter
	// writes and a draw after BeginPass would otherwise not see them; here
	// the parameter block is uploaded at BeginPass, so a value changed after
	// it has nowhere to go until the next BeginPass. The second draw
	// therefore closes and reopens the pass rather than committing -- same
	// technique, same state, one more uniform slice out of the frame arena.
	FX->EndPass();

	FX->SetFloat(eMix, alpha_a);
	FX->SetMatrix(eW, &WA);

	if (FX->BeginPass(0)) {
		FX->DrawUP(&ReentryVtxA, 4, sizeof(NTVERTEX), ReentryIdx, 6);
		FX->EndPass();
	}

	FX->End();
}


// ===========================================================================================
// This is a special rendering routine used to render beacons
//
void VulkanEffect::RenderSpot(float alpha, const FVECTOR4 *pColor, const FMATRIX4 *pW, SURFHANDLE pTex)
{
	UINT numPasses = 0;
	FX->SetVertexDecl(pNTVertexDecl);
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	FX->SetTechnique(eSpotTech);
	FX->SetFloat(eMix, alpha);
	FX->SetValue(eColor, pColor, sizeof(FVECTOR4));
	FX->SetMatrix(eW, pW);
	FX->SetTexture(eTex0, SURFACE(pTex)->GetTexture());
	if (!FX->Begin(&numPasses, 0)) return;
	if (!FX->BeginPass(0)) { FX->End(); return; }
	FX->DrawUP(billboard_vtx, 4, sizeof(NTVERTEX), billboard_idx, 6);
	FX->EndPass();
	FX->End();
}


// ===========================================================================================
// Used by Render Star only
//
void VulkanEffect::RenderBillboard(const FMATRIX4 *pW, VulkanTexture *pTex, float alpha)
{
	UINT numPasses = 0;

	FX->SetVertexDecl(pNTVertexDecl);
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	FX->SetTechnique(eSimple);
	FX->SetMatrix(eW, pW);
	FX->SetFloat(eMix, alpha);
	FX->SetTexture(eTex0, pTex);
	if (!FX->Begin(&numPasses, 0)) return;
	if (!FX->BeginPass(0)) { FX->End(); return; }
	FX->DrawUP(billboard_vtx, 4, sizeof(NTVERTEX), billboard_idx, 6);
	FX->EndPass();
	FX->End();
}


// ===========================================================================================
// This is a special rendering routine used to render engine exhaust
//
// Everything down to the draw is arithmetic on the exhaust vertex table and
// converts unchanged.
//
void VulkanEffect::RenderExhaust(const FMATRIX4 *pW, VECTOR3 &cdir, EXHAUSTSPEC *es, SURFHANDLE def)
{
	SURFHANDLE pTex = SURFHANDLE(es->tex);
	if (!pTex) pTex = def;

	double alpha = *es->level;
	if (es->modulate) alpha *= ((1.0 - es->modulate)+(double)rand()* es->modulate/(double)RAND_MAX);

	VECTOR3 edir = -(*es->ldir);
	VECTOR3 ref =  (*es->lpos) - (*es->ldir)*es->lofs;

	const float flarescale = 7.0;
	VECTOR3 sdir = crossp(cdir, edir); normalise(sdir);
	VECTOR3 tdir = crossp(cdir, sdir); normalise(tdir);
	float rx = (float)ref.x;
	float ry = (float)ref.y;
	float rz = (float)ref.z;
	float sx = (float)(sdir.x*es->wsize);
	float sy = (float)(sdir.y*es->wsize);
	float sz = (float)(sdir.z*es->wsize);
	float ex = (float)(edir.x*es->lsize);
	float ey = (float)(edir.y*es->lsize);
	float ez = (float)(edir.z*es->lsize);

	exhaust_vtx[1].x = (exhaust_vtx[0].x = rx + sx) + ex;
	exhaust_vtx[1].y = (exhaust_vtx[0].y = ry + sy) + ey;
	exhaust_vtx[1].z = (exhaust_vtx[0].z = rz + sz) + ez;
	exhaust_vtx[3].x = (exhaust_vtx[2].x = rx - sx) + ex;
	exhaust_vtx[3].y = (exhaust_vtx[2].y = ry - sy) + ey;
	exhaust_vtx[3].z = (exhaust_vtx[2].z = rz - sz) + ez;

	double wscale = es->wsize;
	wscale *= flarescale, sx *= flarescale, sy *= flarescale, sz *= flarescale;

	float tx = (float)(tdir.x*wscale);
	float ty = (float)(tdir.y*wscale);
	float tz = (float)(tdir.z*wscale);
	exhaust_vtx[4].x = rx - sx + tx;   exhaust_vtx[5].x = rx + sx + tx;
	exhaust_vtx[4].y = ry - sy + ty;   exhaust_vtx[5].y = ry + sy + ty;
	exhaust_vtx[4].z = rz - sz + tz;   exhaust_vtx[5].z = rz + sz + tz;
	exhaust_vtx[6].x = rx - sx - tx;   exhaust_vtx[7].x = rx + sx - tx;
	exhaust_vtx[6].y = ry - sy - ty;   exhaust_vtx[7].y = ry + sy - ty;
	exhaust_vtx[6].z = rz - sz - tz;   exhaust_vtx[7].z = rz + sz - tz;

	UINT numPasses = 0;
	FX->SetVertexDecl(pNTVertexDecl);
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	FX->SetTechnique(eExhaust);
	FX->SetFloat(eMix, float(alpha));
	FX->SetMatrix(eW, pW);
	FX->SetTexture(eTex0, SURFACE(pTex)->GetTexture());
	if (!FX->Begin(&numPasses, 0)) return;
	if (!FX->BeginPass(0)) { FX->End(); return; }
	FX->DrawUP(exhaust_vtx, 8, sizeof(NTVERTEX), exhaust_idx, 12);
	FX->EndPass();
	FX->End();
}


// ===========================================================================================
//
void VulkanEffect::RenderBoundingBox(const FMATRIX4 *pW, const FMATRIX4 *pGT, const FVECTOR4 *bmin, const FVECTOR4 *bmax, const FVECTOR4 *color)
{
	// D3DXMatrixIdentity(&ident) stood here and `ident` was never used. It is
	// dropped rather than converted.

	// Were D3DVECTOR, which is three floats -- the same as FVECTOR3, and the
	// same as what pPositionDecl describes.
	static FVECTOR3 poly[10] = {
		{0, 0, 0},
		{1, 0, 0},
		{1, 1, 0},
		{0, 1, 0},
		{0, 0, 0},
		{0, 0, 1},
		{1, 0, 1},
		{1, 1, 1},
		{0, 1, 1},
		{0, 0, 1}
	};

	static FVECTOR3 list[6] = {
		{1, 0, 0},
		{1, 0, 1},
		{1, 1, 0},
		{1, 1, 1},
		{0, 1, 0},
		{0, 1, 1}
	};

	FX->SetVertexDecl(pPositionDecl);

	FX->SetMatrix(eW, pW);
	FX->SetMatrix(eGT, pGT);
	FX->SetVector(eAttennuate, bmin);
	FX->SetVector(eInScatter, bmax);
	FX->SetVector(eColor, color);
	FX->SetTechnique(eBBTech);

	UINT numPasses = 0;

	// TWO TOPOLOGIES, SO TWO PASSES. The Windows function issues
	// DrawPrimitiveUP(LINESTRIP) and DrawPrimitiveUP(LINELIST) inside one
	// BeginPass, because the primitive type was an argument to the draw. Here
	// it is pipeline state, so each topology needs its own bind -- same
	// technique, same parameters, two pipelines out of the same pass.
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_LINE_STRIP);
	if (!FX->Begin(&numPasses, 0)) return;
	if (FX->BeginPass(0)) {
		FX->DrawUP(&poly, 10, sizeof(FVECTOR3), NULL, 0);
		FX->EndPass();
	}
	FX->End();

	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
	if (!FX->Begin(&numPasses, 0)) return;
	if (FX->BeginPass(0)) {
		FX->DrawUP(&list, 6, sizeof(FVECTOR3), NULL, 0);
		FX->EndPass();
	}
	FX->End();
}


// ===========================================================================================
//
void VulkanEffect::RenderTileBoundingBox(const FMATRIX4 *pW, VECTOR4 *pVtx, const FVECTOR4 *color)
{
	FVECTOR3 poly[8];

	for (int i=0;i<8;i++) poly[i] = FVECTOR3(float(pVtx[i].x), float(pVtx[i].y), float(pVtx[i].z));

	WORD idc1[10] = { 0, 1, 3, 2, 0, 4, 5, 7, 6, 4 };
	WORD idc2[6] = { 1, 5, 3, 7, 2, 6};

	FX->SetVertexDecl(pPositionDecl);

	FX->SetMatrix(eW, pW);
	FX->SetVector(eColor, color);
	FX->SetTechnique(eTBBTech);

	UINT numPasses = 0;

	// Two topologies, two binds. See RenderBoundingBox.
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_LINE_STRIP);
	if (!FX->Begin(&numPasses, 0)) return;
	if (FX->BeginPass(0)) {
		FX->DrawUP(&poly, 8, sizeof(FVECTOR3), idc1, 10);
		FX->EndPass();
	}
	FX->End();

	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
	if (!FX->Begin(&numPasses, 0)) return;
	if (FX->BeginPass(0)) {
		FX->DrawUP(&poly, 8, sizeof(FVECTOR3), idc2, 6);
		FX->EndPass();
	}
	FX->End();
}


// ===========================================================================================
//
void VulkanEffect::RenderLines(const FVECTOR3 *pVtx, const WORD *pIdx, int nVtx, int nIdx, const FMATRIX4 *pW, DWORD color)
{
	UINT numPasses = 0;
	FX->SetVertexDecl(pPositionDecl);
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
	FX->SetMatrix(eW, pW);

	// Was (const D3DXVECTOR4 *)ptr(D3DXCOLOR(color)) -- unpack 0xAARRGGBB into
	// four floats and reinterpret the colour as a vector, which is legal
	// because D3DXCOLOR and D3DXVECTOR4 are both four floats in that order.
	// FVECTOR4 is the same four floats, so the unpack is written out and the
	// cast is gone with the type pun.
	FVECTOR4 c(float((color >> 16) & 0xFF) / 255.0f,
			   float((color >> 8) & 0xFF) / 255.0f,
			   float(color & 0xFF) / 255.0f,
			   float((color >> 24) & 0xFF) / 255.0f);

	FX->SetVector(eColor, &c);
	FX->SetTechnique(eTBBTech);
	if (!FX->Begin(&numPasses, 0)) return;
	if (!FX->BeginPass(0)) { FX->End(); return; }
	FX->DrawUP(pVtx, (UINT)nVtx, sizeof(FVECTOR3), pIdx, (UINT)nIdx);
	FX->EndPass();
	FX->End();
}


// ===========================================================================================
//
void VulkanEffect::RenderBoundingSphere(const FMATRIX4 *pW, const FMATRIX4 *pGT, const FVECTOR4 *bs, const FVECTOR4 *color)
{
	FMATRIX4 mW;

	FVECTOR3 vCam;
	FVECTOR3 vPos;

	// Was D3DXVec3TransformCoord: transform as a point (w=1) and divide
	// through by the resulting w. VMAT_VectorMatrixMultiply is the same
	// operation and is already in VulkanUtil.cpp.
	const FVECTOR3 vLocal(bs->x, bs->y, bs->z);
	if (!VMAT_VectorMatrixMultiply(&vPos, &vLocal, pW)) return;

	vCam = unit(vPos);
	VMAT_CreateX_Billboard(&vCam, &vPos, bs->w, &mW);

	FX->SetVertexDecl(pPositionDecl);
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_LINE_STRIP);

	FX->SetMatrix(eW, &mW);
	FX->SetVector(eColor, color);
	FX->SetTechnique(eBSTech);

	UINT numPasses = 0;
	if (!FX->Begin(&numPasses, 0)) return;
	if (!FX->BeginPass(0)) { FX->End(); return; }

	// Was SetStreamSource(0, VB, 0, sizeof(D3DXVECTOR3)) + DrawPrimitive.
	// This is the one draw in the file that already has its vertices in a
	// buffer -- the 256-point circle built in VulkanTechInit -- so it binds
	// that buffer rather than going through DrawUP.
	if (VB && pDev->IsRecording()) {
		VkCommandBuffer cmd = pDev->GetCommandBuffer();
		VkBuffer vb = VB->Buffer();
		VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
		// DrawPrimitive(D3DPT_LINESTRIP, 0, 255) -- 255 line segments, which
		// is 256 vertices.
		vkCmdDraw(cmd, 256, 1, 0, 0);
	}

	FX->EndPass();
	FX->End();

	(void)pGT;	// unused on Windows too
}


// ===========================================================================================
// This is a special rendering routine used to render (grapple point) arrows
//
void VulkanEffect::RenderArrow(OBJHANDLE hObj, const VECTOR3 *ofs, const VECTOR3 *dir, const VECTOR3 *rot, float size, const FVECTOR4 *pColor)
{
    static FVECTOR3 arrow[18] = {
        // Head (front- & back-face)
        {0.0, 0.0, 0.0},
        {0.0,-1.0, 1.0},
        {0.0, 1.0, 1.0},
        {0.0, 0.0, 0.0},
        {0.0, 1.0, 1.0},
        {0.0,-1.0, 1.0},
        // Body first triangle (front- & back-face)
        {0.0,-0.5, 1.0},
        {0.0,-0.5, 2.0},
        {0.0, 0.5, 2.0},
        {0.0,-0.5, 1.0},
        {0.0, 0.5, 2.0},
        {0.0,-0.5, 2.0},
        // Body second triangle (front- & back-face)
        {0.0,-0.5, 1.0},
        {0.0, 0.5, 2.0},
        {0.0, 0.5, 1.0},
        {0.0,-0.5, 1.0},
        {0.0, 0.5, 1.0},
        {0.0, 0.5, 2.0}
    };

    MATRIX3 grot;
    FMATRIX4 W;
    VECTOR3 camp, gpos;

    oapiGetRotationMatrix(hObj, &grot);
    oapiGetGlobalPos(hObj, &gpos);
    camp = gc->GetScene()->GetCameraGPos();

    VECTOR3 pos = gpos - camp;
    if (ofs) pos += mul (grot, *ofs);

    VECTOR3 z = mul (grot, unit(*dir)) * size;
    VECTOR3 y = mul (grot, unit(*rot)) * size;
    VECTOR3 x = mul (grot, unit(crossp(*dir, *rot))) * size;

    VMAT_Identity(&W);

    // Were W._11 .. W._43. FMATRIX4 spells the same elements m11..m43 -- the
    // leading underscore is D3DXMATRIX's own naming, not a different layout.
    W.m11 = float(x.x);
    W.m12 = float(x.y);
    W.m13 = float(x.z);

    W.m21 = float(y.x);
    W.m22 = float(y.y);
    W.m23 = float(y.z);

    W.m31 = float(z.x);
    W.m32 = float(z.y);
    W.m33 = float(z.z);

    W.m41 = float(pos.x);
    W.m42 = float(pos.y);
    W.m43 = float(pos.z);

    UINT numPasses = 0;
    FX->SetVertexDecl(pPositionDecl);	// Position only vertex decleration
    FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    FX->SetTechnique(eArrowTech);		// Use arrow shader
    FX->SetValue(eColor, pColor, sizeof(FVECTOR4));	// Setup arrow color
    FX->SetMatrix(eW, &W);
    if (!FX->Begin(&numPasses, 0)) return;
    if (!FX->BeginPass(0)) { FX->End(); return; }
    FX->DrawUP(&arrow, 18, sizeof(FVECTOR3), NULL, 0);	// Draw 6 triangles un-indexed
    FX->EndPass();
    FX->End();
}
