
// ===================================================
// Copyright (C) 2021-2026 Jarmo Nikkanen
// licensed under LGPL v2
//
// LINUX/VULKAN CONVERSION OF OVP/D3D9Client/IProcess.cpp
//
// The class note is in IProcess.h. What follows is the list of things this
// FILE does differently, in the order they appear, so a reader comparing the
// two side by side knows where to look.
//
//   1. THE VIEWPORT IS NO LONGER PUSHED. SetupViewPort() still measures the
//      first render target and builds the same orthographic matrix from it,
//      because that matrix is what the vertex shader uses. What it does not
//      do is SetViewport(): VulkanDevice::BeginOffscreen sets the viewport
//      and the scissor from the extent of the attachments it is given, which
//      is the same number reached from the same place.
//
//   2. EVERY SetRenderState IN Execute() IS PIPELINE STATE. Fill mode, cull
//      mode, blending, alpha test, stencil and the four colour write masks
//      were eleven independent pieces of device state in D3D9 and are fields
//      of one immutable VkPipeline here. So Execute() does not set them -- it
//      asks GetPipeline() for the pipeline that HAS them, and the cache key
//      is exactly the set of them that Execute() varies. The save/restore of
//      D3DRS_FILLMODE around the call disappears with them: this class's
//      pipelines are built with VK_POLYGON_MODE_FILL and nothing global
//      changed, so there is nothing to put back.
//
//   3. SetRenderTarget BECOMES BeginOffscreen/EndOffscreen. Which also means
//      the four GetRenderTarget calls and the depth-surface save vanish --
//      see the note on pRtgBak in the header.
//
//   4. THE SAMPLER STATES BECOME SAMPLER OBJECTS. The eight SetSamplerState
//      calls per slot are the fields of one VkSamplerCreateInfo, and the
//      resulting VkSampler is cached on the slot and rebuilt only when the
//      IPF_ flags change. The decoding of those flags is carried over line
//      for line, including the order of the filter tests.
//
//   5. THE TWO DrawUP CALLS COPY THROUGH A SCRATCH BUFFER. There is no
//      "draw from this pointer in my memory" in Vulkan: a draw sources its
//      vertices from a VkBuffer bound to the command buffer. That is what the
//      D3D9 runtime did behind DrawPrimitiveUP anyway.
//
//   6. D3DPT_TRIANGLEFAN HAS NO COUNTERPART IN THE CONVERTED PIPELINE. It is
//      in core Vulkan as VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN, but it is
//      OPTIONAL -- portability-subset implementations (MoltenVK) and some
//      drivers refuse it -- and the octagon template is the client's only
//      user. The ten fan vertices are therefore expanded into eight triangles
//      by an index list built once in the constructor, which draws exactly
//      the same eight triangles from exactly the same ten vertices. See
//      cOcta below.
//
//   7. bInScene HAS NO COUNTERPART. It guarded BeginScene/EndScene, which
//      existed because a D3D9 draw outside a scene was an error. BeginOffscreen
//      records into a command buffer of its own and EndOffscreen submits and
//      waits, whether or not the caller is inside the core's render pass, so
//      the flag has nothing to guard. It stays in the signature because
//      gcIPInterface's public API passes it.
// ===================================================

#include <string.h>
#include "IProcess.h"
#include "VulkanUtil.h"
#include "VulkanSurface.h"
#include "VulkanConfig.h"
#include <sstream>
// <fstream> was reached through D3D9Util.h on Windows, which included it for
// its own shader cache. VulkanUtil.h includes it too, but the dependency is
// named here because this file uses std::ifstream directly.
#include <fstream>


// ================================================================================================
// The index list that turns the octagon template's ten-vertex TRIANGLEFAN into
// a TRIANGLELIST. See note 6 in the file header.
//
// A fan of N+1 vertices is N-1 triangles (0,1,2), (0,2,3) ... (0,N-1,N). Ten
// vertices is eight triangles, which is what DrawPrimitiveUP(D3DPT_TRIANGLEFAN,
// 8, ...) asked for -- its count was PRIMITIVES.
// ================================================================================================
static const WORD cOcta[24] = {
	0, 1, 2,	0, 2, 3,	0, 3, 4,	0, 4, 5,
	0, 5, 6,	0, 6, 7,	0, 7, 8,	0, 8, 9
};


// ================================================================================================
//
ImageProcessing::ImageProcessing(VulkanDevice *pDev, const char *_file, const char *_psentry, const char *_ppf, const char *_vsentry)
	// THE ORDER OF THIS LIST IS THE ORDER OF THE DECLARATIONS, which the
	// Windows one was free to ignore -- MSVC does not warn, GCC does
	// (-Wreorder), and a member initialised out of order is initialised in
	// declaration order anyway, so a list that reads differently from what
	// runs is a trap. Same members, same values; only the sequence moves.
	: mesh_cull(gcIPInterface::ipicull::None)
	, pMesh(NULL)
	, pDevice(pDev)
	, pDepth(NULL)
	, pVSConst(NULL)
	, pPSConst(NULL)
	, desc()
	, iVP()
	, hVP(NULL)
	, hPos(NULL)
	, hSiz(NULL)
	, mesh_tex_idx(-1)
	// pDepthBak is gone; see the header. Everything from here down is new and
	// is zeroed for the same reason ShaderClass's had to be: GetPipeline and
	// CreateResources test these handles, and an indeterminate value there is
	// a wild handle handed to Vulkan.
	, vkSetLayout(VK_NULL_HANDLE)
	, vkPipeLayout(VK_NULL_HANDLE)
	, vkPool(VK_NULL_HANDLE)
	, vkSet(VK_NULL_HANDLE)
	, pVSBuf(NULL)
	, pPSBuf(NULL)
	, vsBinding(0)
	, psBinding(1)
	, pScratchVB(NULL)
	, pScratchIB(NULL)
	, scratchVBSize(0)
	, scratchIBSize(0)
	, pWhite(NULL)
	, pWhiteCube(NULL)
{
	for (size_t i=0;i<ARRAYSIZE(pTextures);i++) {
		pTextures[i].hTex = NULL;
		pTextures[i].flags = 0;
		pTextures[i].pSampler = VK_NULL_HANDLE;
		pTextures[i].smpFlags = 0xFFFFFFFF;		// "no sampler built yet"
		pTextures[i].binding = 0;
	}
	// Was: for (i<4) pRtg[i] = pRtgBak[i] = NULL. There is no pRtgBak.
	for (int i=0;i<4;i++) pRtg[i] = NULL;

	// "Modules/D3D9Client/IPI.hlsl" becomes "Modules/VulkanClient/IPI.glsl".
	// The entry point names are unchanged: glslang takes an entry point by
	// name exactly as D3DXCompileShaderFromFile did.
	if (_vsentry) {
		pVertex = CompileVertexShader(pDevice, _file, _vsentry, "IPIVS", NULL, &pVSConst);
		strcpy_s(vsentry, 32, _vsentry);
	}
	else {
		pVertex = CompileVertexShader(pDevice, "Modules/VulkanClient/IPI.glsl", "VSMain", "IPIVS", NULL, &pVSConst);
		strcpy_s(vsentry, 32, "VSMain");
	}

	pPixel   = CompilePixelShader(pDevice, _file, _psentry, "IPIPS", _ppf, &pPSConst);
	pOcta	 = new SMVERTEX[10];

	Shaders[string(_psentry)].pPixel = pPixel;
	Shaders[string(_psentry)].pPSConst = pPSConst;

	// GetConstantByName drops D3DX's first argument. It was the parent
	// constant to search inside; NULL meant the top level, which is the only
	// value the client ever passed.
	if (pVSConst) {
		hVP = pVSConst->GetConstantByName("mVP");
		hPos = pVSConst->GetConstantByName("vPos");
		hSiz = pVSConst->GetConstantByName("vTgtSize");
		SetTemplate();
	}

	if (!hVP || !hPos) LogErr("Failed to get ImageProcessing::hVP handle");

	double w = 22.5 * RAD;
	double q = w;
	double r = 1.0 / cos(w);
	
	pOcta[0].x = 0.0f;
	pOcta[0].y = 0.0f;
	pOcta[0].z = 0.0f;
	pOcta[0].tu = 0.0f;
	pOcta[0].tv = 0.0f;
	
	for (int i = 1; i < 10; i++) {
		pOcta[i].x = float(cos(q) * r);
		pOcta[i].y = float(sin(q) * r);
		pOcta[i].z = 0.0f;
		pOcta[i].tu = pOcta[i].x;
		pOcta[i].tv = pOcta[i].y;
		q += w*2.0;
	}

	strcpy_s(file, 256, _file);
	strcpy_s(entry, 32, _psentry);
	if (_ppf) strcpy_s(ppf, 256, _ppf);
	else strcpy_s(ppf, 32, "");

	// Create a database of defines ----------------------------------------------------------------
	std::string line;
	std::ifstream fs(_file);
	while (std::getline(fs, line)) {
		if (!line.length() || line.find("//") == 0) continue;
		if (line.find("#define") == 0) def.push_front(line.substr(line.find("#define") + 8));
	}
	fs.close();
}


// ================================================================================================
//
ImageProcessing::~ImageProcessing()
{
	// Was SAFE_RELEASE(pVSConst) / SAFE_RELEASE(pVertex). A constant table was
	// a COM object; a ShaderReflection is a plain heap object. A VkShaderModule
	// is destroyed against the device that made it.
	SAFE_DELETE(pVSConst);
	if (pDevice && pVertex) vkDestroyShaderModule(pDevice->GetDevice(), pVertex, NULL);
	pVertex = VK_NULL_HANDLE;
	SAFE_DELETEA(pOcta);

	for (auto x : Shaders) {
		if (pDevice && x.second.pPixel) vkDestroyShaderModule(pDevice->GetDevice(), x.second.pPixel, NULL);
		SAFE_DELETE(x.second.pPSConst);
	}
	Shaders.clear();
	// pPSConst aliased whichever entry Activate() last selected and has just
	// been deleted with it; pPixel aliased that entry's module.
	pPSConst = NULL;
	pPixel = VK_NULL_HANDLE;

	// Everything below has no counterpart in the Windows destructor because
	// none of it existed: D3D9 had no pipeline object, no descriptor set, no
	// sampler object and no uniform buffer, and DrawPrimitiveUP staged its
	// vertices where the caller could not see them.
	if (pDevice) {
		VkDevice dev = pDevice->GetDevice();
		for (auto &x : Pipelines) if (x.second) vkDestroyPipeline(dev, x.second, NULL);
		Pipelines.clear();
		if (vkPipeLayout) { vkDestroyPipelineLayout(dev, vkPipeLayout, NULL); vkPipeLayout = VK_NULL_HANDLE; }
		if (vkSetLayout) { vkDestroyDescriptorSetLayout(dev, vkSetLayout, NULL); vkSetLayout = VK_NULL_HANDLE; }
		if (vkPool) { vkDestroyDescriptorPool(dev, vkPool, NULL); vkPool = VK_NULL_HANDLE; }
		for (size_t i = 0; i < ARRAYSIZE(pTextures); i++) {
			if (pTextures[i].pSampler) {
				vkDestroySampler(dev, pTextures[i].pSampler, NULL);
				pTextures[i].pSampler = VK_NULL_HANDLE;
			}
		}
		if (pVSBuf) { pDevice->DestroyBuffer(pVSBuf); pVSBuf = NULL; }
		if (pPSBuf) { pDevice->DestroyBuffer(pPSBuf); pPSBuf = NULL; }
		if (pScratchVB) { pDevice->DestroyBuffer(pScratchVB); pScratchVB = NULL; }
		if (pScratchIB) { pDevice->DestroyBuffer(pScratchIB); pScratchIB = NULL; }
		if (pWhite) { pDevice->DestroyTexture(pWhite); pWhite = NULL; }
		if (pWhiteCube) { pDevice->DestroyTexture(pWhiteCube); pWhiteCube = NULL; }
	}
}


// ================================================================================================
//
bool ImageProcessing::CompileShader(const char *Entry)
{
	string name(Entry);
	ShaderReflection *pPSC = NULL;
	Shaders[name].pPixel = CompilePixelShader(pDevice, file, Entry, "IPIPS2", ppf, &pPSC);
	Shaders[name].pPSConst = pPSC;
	return ((Shaders[name].pPixel != VK_NULL_HANDLE) && (Shaders[name].pPSConst != NULL));
}


// ================================================================================================
//
bool ImageProcessing::Activate(const char *Entry)
{
	SetTemplate();
	if (!Entry) return Activate(entry);
	string name(Entry);
	if (Shaders.count(name) == 0) {
		LogErr("ImageProcessing::Activate() FAILED Entry=%s", Entry);
		return false;
	}
	strcpy_s(entry, 31, Entry);
	pPixel = Shaders[name].pPixel;
	pPSConst = Shaders[name].pPSConst;
	return true;
}


// ================================================================================================
//
int ImageProcessing::FindDefine(const char *_key)
{
	int retval;
	std::string key;
	auto it = def.begin();
	while (it!=def.end()) {
		std::istringstream iss(*it);
		iss >> key >> retval;
		if (key.compare(_key) == 0) return retval;
		it++;
	}
	return 0;
}


// ================================================================================================
// ================================================================================================
// THE SIX FUNCTIONS BELOW ARE NEW. Nothing in the Windows file corresponds to
// them, because every one of them replaces a piece of D3D9 DEVICE STATE with
// an object that has to be created, and D3D9 created none of these.
// ================================================================================================
// ================================================================================================


// ================================================================================================
// The descriptor set layout, the pipeline layout, the two uniform blocks and
// the descriptor pool. Built once, on the first Execute(), because it needs
// the reflection of the shaders and the pixel shader may still be swapped by
// Activate() before then -- but the LAYOUT does not depend on which pixel
// shader is active, only on how many sampler bindings the widest of them
// declares, so once is enough.
//
// THE BINDING CONVENTION. Two uniform blocks, one per stage, at the bindings
// the GLSL declares them at; the samplers at the bindings the GLSL declares
// THEM at. D3D9 needed no convention because the two constant tables wrote
// into separate register files that could not collide, and a sampler lived in
// a third. In one Vulkan descriptor set they share a number space, so the
// numbers are read out of the reflection rather than assumed -- which is also
// what makes a shader that declares its own layout work without changing this
// file.
// ================================================================================================
bool ImageProcessing::CreateResources()
{
	if (vkSetLayout) return true;			// already built
	if (!pDevice) return false;

	VkDevice dev = pDevice->GetDevice();

	// Which binding does each stage's uniform block sit at, and how many
	// sampler bindings are there in total? A non-sampler Var carries the
	// binding of the block it belongs to; a sampler Var carries its own.
	uint32_t nSampler = 0;
	uint32_t maxBinding = 0;
	bool bVS = false, bPS = false;

	if (pVSConst) {
		for (auto &v : pVSConst->Vars()) {
			if (v.bSampler) continue;
			vsBinding = v.binding;
			bVS = true;
			break;
		}
	}

	// Every compiled pixel shader is examined, not just the active one: the
	// layout has to satisfy all of them, because Activate() swaps between
	// them without rebuilding it.
	for (auto &s : Shaders) {
		if (!s.second.pPSConst) continue;
		for (auto &v : s.second.pPSConst->Vars()) {
			if (v.bSampler) {
				nSampler++;
				if (v.binding > maxBinding) maxBinding = v.binding;
			}
			else if (!bPS) { psBinding = v.binding; bPS = true; }
		}
	}

	std::vector<VkDescriptorSetLayoutBinding> binds;

	if (bVS) {
		VkDescriptorSetLayoutBinding b = {};
		b.binding = vsBinding;
		b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		b.descriptorCount = 1;
		b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
		binds.push_back(b);
	}
	if (bPS) {
		VkDescriptorSetLayoutBinding b = {};
		b.binding = psBinding;
		b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		b.descriptorCount = 1;
		b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
		binds.push_back(b);
	}

	// One combined image sampler per sampler binding any of the pixel shaders
	// declares. Bindings the active shader does not use are still written in
	// BindResources -- see the note there.
	for (auto &s : Shaders) {
		if (!s.second.pPSConst) continue;
		for (auto &v : s.second.pPSConst->Vars()) {
			if (!v.bSampler) continue;
			bool bHave = false;
			for (auto &b : binds) if (b.binding == v.binding) { bHave = true; break; }
			if (bHave) continue;
			VkDescriptorSetLayoutBinding b = {};
			b.binding = v.binding;
			b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			b.descriptorCount = 1;
			b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
			binds.push_back(b);
		}
	}

	VkDescriptorSetLayoutCreateInfo dsl = {};
	dsl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	dsl.bindingCount = (uint32_t)binds.size();
	dsl.pBindings = binds.empty() ? NULL : binds.data();

	if (vkCreateDescriptorSetLayout(dev, &dsl, NULL, &vkSetLayout) != VK_SUCCESS) {
		LogErr("ImageProcessing(%s): vkCreateDescriptorSetLayout failed", file);
		return false;
	}

	VkPipelineLayoutCreateInfo pl = {};
	pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pl.setLayoutCount = 1;
	pl.pSetLayouts = &vkSetLayout;

	if (vkCreatePipelineLayout(dev, &pl, NULL, &vkPipeLayout) != VK_SUCCESS) {
		LogErr("ImageProcessing(%s): vkCreatePipelineLayout failed", file);
		return false;
	}

	// The pool. 64 sets is the mesh template's group count with room to
	// spare, and it is reset at the top of every Execute() so the figure is
	// per call rather than per frame.
	{
		VkDescriptorPoolSize sizes[2] = {};
		sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		sizes[0].descriptorCount = 64 * 2;
		sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		sizes[1].descriptorCount = 64 * (nSampler ? nSampler : 1);

		VkDescriptorPoolCreateInfo pi = {};
		pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		pi.maxSets = 64;
		pi.poolSizeCount = 2;
		pi.pPoolSizes = sizes;

		if (vkCreateDescriptorPool(dev, &pi, NULL, &vkPool) != VK_SUCCESS) {
			LogErr("ImageProcessing(%s): vkCreateDescriptorPool failed", file);
			return false;
		}
	}

	// The two uniform blocks. A block of zero bytes is not a legal buffer, so
	// a stage that declares no constants gets none and its binding is absent
	// from the layout above.
	if (bVS && pVSConst && pVSConst->BlockSize()) {
		pVSBuf = pDevice->CreateBuffer(pVSConst->BlockSize(), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
		if (!pVSBuf) { LogErr("ImageProcessing(%s): VS uniform block failed", file); return false; }
	}
	if (bPS) {
		// Sized for the LARGEST of the compiled pixel shaders, because
		// Activate() swaps between them and each writes into this one buffer.
		uint32_t n = 0;
		for (auto &s : Shaders) {
			if (!s.second.pPSConst) continue;
			if (s.second.pPSConst->BlockSize() > n) n = s.second.pPSConst->BlockSize();
		}
		if (n) {
			pPSBuf = pDevice->CreateBuffer(n, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
			if (!pPSBuf) { LogErr("ImageProcessing(%s): PS uniform block failed", file); return false; }
		}
	}

	// The 1x1 white default. EVERY binding in the layout must be written
	// before the set is used, whether the active shader reads it or not: a
	// descriptor left unwritten and then read is undefined behaviour and the
	// validation layer says so. D3D9 had no such rule -- an unset texture
	// stage sampled white -- so white is what an unset binding gets, which
	// makes the two behave the same. Same decision, same reason, as
	// VulkanEffectFile's pWhite.
	if (nSampler) {
		pWhite = pDevice->CreateTexture(1, 1, 1, VK_FORMAT_B8G8R8A8_UNORM,
										VK_IMAGE_USAGE_SAMPLED_BIT |
										VK_IMAGE_USAGE_TRANSFER_DST_BIT |
										VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
		if (!pWhite) {
			LogErr("ImageProcessing(%s): could not create the default white texture", file);
			return false;
		}
		const DWORD white = 0xFFFFFFFF;
		pDevice->UploadTexture(pWhite, 0, 0, &white, sizeof(white));

		// AND A WHITE CUBE, if any of these shaders declares a samplerCube.
		// THE THIRD AND LAST OF THE THREE PLACES THAT NEEDED THIS -- the
		// other two are ShaderClass and VulkanEffectFile, and this one was
		// left as a bare `pTex = pWhite;` after they were fixed.
		//
		// Filling a cube-dimensioned binding with a 2D view is not a wrong
		// colour, it is a GPU fault:
		//
		//     VUID-VkDescriptorImageInfo-imageView-07752
		//     ... the image view type does not match the descriptor's
		//     declared dimensionality.
		//
		// D3D9 needed no such match -- an unset sampler stage sampled white
		// whatever the shader declared -- so the fallback has to be built per
		// dimension here. EnvMapBlur and IrradianceInteg are the two
		// ImageProcessing shaders that read cubes.
		bool bAnyCube = false;
		for (auto &s : Shaders) {
			if (!s.second.pPSConst) continue;
			for (auto &v : s.second.pPSConst->Vars())
				if (v.bSampler && v.bCube) { bAnyCube = true; break; }
			if (bAnyCube) break;
		}

		if (bAnyCube) {
			pWhiteCube = pDevice->CreateTextureCube(1, 1, VK_FORMAT_B8G8R8A8_UNORM,
													VK_IMAGE_USAGE_SAMPLED_BIT |
													VK_IMAGE_USAGE_TRANSFER_DST_BIT |
													VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
			if (!pWhiteCube) {
				LogErr("ImageProcessing(%s): could not create the default white cube", file);
				return false;
			}
			for (uint32_t f = 0; f < 6; f++)
				pDevice->UploadTexture(pWhiteCube, 0, f, &white, sizeof(white));
		}
	}

	return true;
}


// ================================================================================================
// Counterpart of ID3DXConstantTable::SetValue, and the destination of every
// Set* function in this file.
//
// D3DX took a device because a D3D9 constant table wrote straight into the
// device's constant registers. Here the bytes go into a uniform buffer the
// pipeline reads, so the device is not a parameter -- the buffer is. The
// bounds test is new for the same reason it is new in
// ShaderClass::WriteConstants: SetValue silently ignored a write past the end
// of a constant, whereas overrunning a mapped Vulkan allocation corrupts
// whatever is next in it.
// ================================================================================================
bool ImageProcessing::WriteConstants(ShaderReflection *pCB, const ShaderReflection::Var *v,
									 const void *data, int bytes)
{
	if (!pCB || !v || !data || bytes <= 0 || !pDevice) return false;
	if (v->bSampler) return false;

	if (!CreateResources()) return false;

	VulkanBuffer *pBuf = (pCB == pVSConst) ? pVSBuf : pPSBuf;
	if (!pBuf) return false;

	if (VkDeviceSize(v->offset) + bytes > pBuf->Size()) {
		LogErr("ImageProcessing::WriteConstants [%s] offset %u + %d bytes exceeds the %llu-byte block",
			v->name.c_str(), v->offset, bytes, (unsigned long long)pBuf->Size());
		return false;
	}

	char *p = (char*)pBuf->Map();
	if (!p) return false;
	memcpy(p + v->offset, data, (size_t)bytes);
	pBuf->Unmap();
	return true;
}


// ================================================================================================
// One slot's VkSampler. This is where the eight SetSamplerState calls that
// Execute() issued per slot on Windows actually go.
//
// The IPF_ decoding below is the Windows loop line for line, including the
// order of the filter tests -- LINEAR, then PYRAMIDAL, then GAUSSIAN, each
// overriding the last -- because that order is what decides the result when a
// caller passes more than one.
//
// D3DTEXF_PYRAMIDALQUAD and D3DTEXF_GAUSSIANQUAD HAVE NO COUNTERPART. They
// were D3D9 filter modes no PC driver ever implemented, so they select linear
// here, which is what the D3D9 runtime fell back to. Same finding as
// ShaderClass::UpdateTextures.
//
// D3DSAMP_MIPFILTER was D3DTEXF_NONE -- "sample level 0 only" -- which is
// maxLod 0 here, not a mipmapMode. There is no "no mip filter" enum in
// Vulkan; clamping the LOD range is how the same thing is said.
// ================================================================================================
void ImageProcessing::UpdateSampler(int idx)
{
	if (!pDevice) return;
	if (pTextures[idx].smpFlags == pTextures[idx].flags && pTextures[idx].pSampler) return;

	DWORD flags = pTextures[idx].flags;

	VkSamplerCreateInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;

	if (flags&IPF_CLAMP_U)			si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	else if (flags&IPF_MIRROR_U)	si.addressModeU = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
	else							si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;

	if (flags&IPF_CLAMP_V)			si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	else if (flags&IPF_MIRROR_V)	si.addressModeV = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
	else							si.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;

	if (flags&IPF_CLAMP_W)			si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	else if (flags&IPF_MIRROR_W)	si.addressModeW = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
	else							si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;

	VkFilter filter = VK_FILTER_NEAREST;		// D3DTEXF_POINT

	if (flags&IPF_LINEAR) filter = VK_FILTER_LINEAR;
	if (flags&IPF_PYRAMIDAL) filter = VK_FILTER_LINEAR;		// see note above
	if (flags&IPF_GAUSSIAN) filter = VK_FILTER_LINEAR;		// see note above

	si.magFilter = filter;
	si.minFilter = filter;
	si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;			// D3DTEXF_NONE
	si.minLod = 0.0f;
	si.maxLod = 0.0f;
	si.anisotropyEnable = VK_FALSE;
	si.maxAnisotropy = 1.0f;

	if (pTextures[idx].pSampler) {
		vkDestroySampler(pDevice->GetDevice(), pTextures[idx].pSampler, NULL);
		pTextures[idx].pSampler = VK_NULL_HANDLE;
	}
	if (vkCreateSampler(pDevice->GetDevice(), &si, NULL, &pTextures[idx].pSampler) != VK_SUCCESS) {
		LogErr("ImageProcessing(%s): vkCreateSampler failed for slot %d", file, idx);
		return;
	}
	pTextures[idx].smpFlags = flags;
}


// ================================================================================================
// Allocate one descriptor set, write the two uniform blocks and every sampler
// binding into it, and bind it.
//
// This is the counterpart of the SetTexture(idx, ...) calls at the end of the
// Windows Execute() -- and of the two constant tables reaching the device,
// which they did invisibly at SetValue time. A fresh set per call; see the
// note on vkPool in the header for why it cannot be one set rewritten.
// ================================================================================================
bool ImageProcessing::BindResources(VkCommandBuffer cmd)
{
	if (!pDevice || !vkSetLayout || !vkPool) return false;

	VkDescriptorSetAllocateInfo ai = {};
	ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	ai.descriptorPool = vkPool;
	ai.descriptorSetCount = 1;
	ai.pSetLayouts = &vkSetLayout;

	if (vkAllocateDescriptorSets(pDevice->GetDevice(), &ai, &vkSet) != VK_SUCCESS) {
		LogErr("ImageProcessing(%s): descriptor pool exhausted", file);
		return false;
	}

	std::vector<VkWriteDescriptorSet> writes;
	VkDescriptorBufferInfo bufs[2] = {};
	VkDescriptorImageInfo imgs[ARRAYSIZE(pTextures)] = {};

	if (pVSBuf) {
		bufs[0].buffer = pVSBuf->Buffer();
		bufs[0].offset = 0;
		bufs[0].range = pVSBuf->Size();
		VkWriteDescriptorSet w = {};
		w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w.dstSet = vkSet;
		w.dstBinding = vsBinding;
		w.descriptorCount = 1;
		w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		w.pBufferInfo = &bufs[0];
		writes.push_back(w);
	}
	if (pPSBuf) {
		bufs[1].buffer = pPSBuf->Buffer();
		bufs[1].offset = 0;
		bufs[1].range = pPSBuf->Size();
		VkWriteDescriptorSet w = {};
		w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w.dstSet = vkSet;
		w.dstBinding = psBinding;
		w.descriptorCount = 1;
		w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		w.pBufferInfo = &bufs[1];
		writes.push_back(w);
	}

	// Every sampler binding the LAYOUT declares is written, not merely the
	// ones the active pixel shader reads. See the note by pWhite in
	// CreateResources: an unwritten descriptor that is then read is undefined
	// behaviour, where an unset D3D9 sampler stage simply produced white.
	//
	// The slot a binding takes its texture from is the sampler INDEX in the
	// ACTIVE shader's reflection -- which is the number SetTexture() used to
	// index pTextures, and the number GetSamplerIndex() answered on Windows.
	// A binding that belongs only to some other compiled entry point has no
	// slot here and gets white.
	//
	// THE ACTIVE SHADER IS WALKED FIRST, AND IT HAS TO BE. Two entry points in
	// one file routinely declare the SAME sampler name at the same binding --
	// IrradianceInteg.glsl's PSPreInteg and PSPostBlur both read `tSrc` -- and
	// the entries are walked out of a std::map, which orders them by name
	// rather than by which one is active. Filling a binding from whichever
	// entry point came first alphabetically would hand the active shader
	// white for a texture the caller had just set. So the active shader
	// claims its bindings, and the rest fill in what is left.
	uint32_t nImg = 0;
	const size_t imgBase = writes.size();

	auto writeSampler = [&](const ShaderReflection::Var &v, bool bActive) {
		for (size_t k = 0; k < nImg; k++)
			if (writes[imgBase + k].dstBinding == v.binding) return;		// already claimed
		if (nImg >= ARRAYSIZE(imgs)) return;

		VulkanTexture *pTex = NULL;
		if (bActive) {
			uint32_t idx = v.samplerIndex;
			if (idx < ARRAYSIZE(pTextures) && pTextures[idx].hTex) {
				UpdateSampler((int)idx);
				pTex = pTextures[idx].hTex;
				imgs[nImg].sampler = pTextures[idx].pSampler;
			}
		}
		if (!pTex) {
			// White, through slot 0's sampler -- built here if the slot has
			// never had one, since its parameters do not matter for a 1x1
			// image.
			//
			// A CUBE-DECLARED BINDING GETS THE WHITE CUBE, not the white 2D:
			// the view type must match the declaration or the draw faults.
			// See pWhiteCube in CreateResources.
			pTex = (v.bCube && pWhiteCube) ? pWhiteCube : pWhite;
			UpdateSampler(0);
			imgs[nImg].sampler = pTextures[0].pSampler;
		}
		if (!pTex || pTex->View() == VK_NULL_HANDLE || !imgs[nImg].sampler) return;

		imgs[nImg].imageView = pTex->View();
		imgs[nImg].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

		VkWriteDescriptorSet w = {};
		w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w.dstSet = vkSet;
		w.dstBinding = v.binding;
		w.descriptorCount = 1;
		w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		w.pImageInfo = &imgs[nImg];
		writes.push_back(w);
		nImg++;
	};

	if (pPSConst)
		for (auto &v : pPSConst->Vars())
			if (v.bSampler) writeSampler(v, true);

	for (auto &s : Shaders) {
		if (!s.second.pPSConst || s.second.pPSConst == pPSConst) continue;
		for (auto &v : s.second.pPSConst->Vars())
			if (v.bSampler) writeSampler(v, false);
	}

	if (!writes.empty()) {
		vkUpdateDescriptorSets(pDevice->GetDevice(), (uint32_t)writes.size(), writes.data(), 0, NULL);
	}

	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkPipeLayout, 0, 1, &vkSet, 0, NULL);
	return true;
}


// ================================================================================================
// The scratch buffers the template draws copy into, grown on demand and never
// shrunk. Identical to ShaderClass::EnsureScratch and VulkanEffectFile's, and
// deliberately so -- see DrawUP in VulkanUtil.h for the argument.
// ================================================================================================
bool ImageProcessing::EnsureScratch(VkDeviceSize vbytes, VkDeviceSize ibytes)
{
	if (vbytes > scratchVBSize) {
		if (pScratchVB) pDevice->DestroyBuffer(pScratchVB);
		scratchVBSize = vbytes * 2;
		pScratchVB = pDevice->CreateBuffer(scratchVBSize, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
		if (!pScratchVB) { scratchVBSize = 0; return false; }
	}
	if (ibytes > scratchIBSize) {
		if (pScratchIB) pDevice->DestroyBuffer(pScratchIB);
		scratchIBSize = ibytes * 2;
		pScratchIB = pDevice->CreateBuffer(scratchIBSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
		if (!pScratchIB) { scratchIBSize = 0; return false; }
	}
	return true;
}


// ================================================================================================
// Build the VkPipeline for one Execute(). This is where the fifteen
// SetRenderState / SetVertexShader / SetPixelShader / SetVertexDeclaration
// calls at the top of the Windows Execute() end up, each one now a field set
// once and immutable afterwards:
//
//   SetVertexShader / SetPixelShader        pStages[0] / pStages[1]
//   SetVertexDeclaration(pPosTexDecl)       pVertexInputState, from pDecl
//   D3DRS_FILLMODE   = D3DFILL_SOLID        polygonMode = FILL
//   D3DRS_CULLMODE   = D3DCULL_NONE / CCW   cullMode + frontFace; see below
//   D3DRS_ALPHABLENDENABLE (blendop != 0)   blendEnable
//   D3DRS_BLENDOP/SRCBLEND/DESTBLEND        colorBlendOp / src / dst
//   D3DRS_ALPHATESTENABLE = false           no counterpart -- the alpha test
//                                           was removed from the API after
//                                           D3D9; a shader discards instead
//   D3DRS_STENCILENABLE = false             stencilTestEnable
//   D3DRS_COLORWRITEENABLE = 0xF            colorWriteMask, all four channels
//   D3DRS_COLORWRITEENABLE1/2/3 = 0xF       the same mask on attachments 1..3,
//                                           which is why the attachment array
//                                           below is filled rather than one
//                                           state being set four times
//   D3DRS_ZENABLE / ZWRITEENABLE            depthTestEnable / depthWriteEnable
//
// D3DCULL_CW AND D3DCULL_CCW ARE THE SAME VULKAN CULL MODE. Both are
// VK_CULL_MODE_BACK_BIT and differ only in frontFace, because "cull the
// clockwise triangles" and "cull the counter-clockwise ones" are one cull with
// opposite ideas of which winding faces front. Same table as ShaderClass's
// GetPipeline and VulkanEffect.cpp's.
// ================================================================================================
VkPipeline ImageProcessing::GetPipeline(DWORD blendop, VkPrimitiveTopology topo, bool bCull,
										bool bDepth, const VertexDecl *pDecl)
{
	// The render pass joins the key, and it is read once here so that the key
	// and pi.renderPass below cannot disagree. See VulkanFrame.h: a pipeline
	// may be bound only in a render pass COMPATIBLE with the one it was built
	// against, and this class draws into a different set of attachments on
	// nearly every call.
	VkRenderPass pass = pDevice ? pDevice->GetRenderPass() : VK_NULL_HANDLE;

	PipeKey key = { blendop, topo, bCull, bDepth, pDecl, pass, pPixel };
	auto it = Pipelines.find(key);
	if (it != Pipelines.end()) return it->second;

	if (!pDevice || !pVertex || !pPixel || !vkPipeLayout) return VK_NULL_HANDLE;

	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = pVertex;
	// The SPIR-V entry point name, which is the one glslang was given. Same
	// two names D3DXCompileShaderFromFile took; they simply have to survive
	// as far as the pipeline here. See vsentry in the header.
	stages[0].pName = vsentry;
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = pPixel;
	stages[1].pName = entry;

	VkVertexInputBindingDescription binding = {};
	VkVertexInputAttributeDescription attribs[16] = {};
	uint32_t nAttrib = 0;

	if (pDecl) {
		binding = pDecl->Binding(0);
		nAttrib = pDecl->Attributes(attribs, 16, 0);
	}

	VkPipelineVertexInputStateCreateInfo vi = {};
	vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vi.vertexBindingDescriptionCount = pDecl ? 1 : 0;
	vi.pVertexBindingDescriptions = pDecl ? &binding : NULL;
	vi.vertexAttributeDescriptionCount = nAttrib;
	vi.pVertexAttributeDescriptions = nAttrib ? attribs : NULL;

	VkPipelineInputAssemblyStateCreateInfo ia = {};
	ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	ia.topology = topo;

	VkPipelineViewportStateCreateInfo vp = {};
	vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	vp.viewportCount = 1;
	vp.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rs = {};
	rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rs.polygonMode = VK_POLYGON_MODE_FILL;			// D3DFILL_SOLID
	if (bCull) {									// D3DCULL_CCW; see the note
		rs.cullMode = VkCullModeFlags(VK_CULL_MODE_BACK_BIT);
		rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
	}
	else {											// D3DCULL_NONE
		rs.cullMode = VkCullModeFlags(VK_CULL_MODE_NONE);
		rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	}
	rs.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo ms = {};
	ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineDepthStencilStateCreateInfo ds = {};
	ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	ds.depthTestEnable = bDepth ? VK_TRUE : VK_FALSE;		// D3DRS_ZENABLE
	ds.depthWriteEnable = bDepth ? VK_TRUE : VK_FALSE;		// D3DRS_ZWRITEENABLE
	ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	ds.stencilTestEnable = VK_FALSE;						// D3DRS_STENCILENABLE

	VkPipelineColorBlendAttachmentState cb = {};
	cb.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
					  | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;	// 0xF
	cb.blendEnable = (blendop != 0) ? VK_TRUE : VK_FALSE;
	cb.colorBlendOp = VK_BLEND_OP_ADD;
	cb.alphaBlendOp = VK_BLEND_OP_ADD;
	cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;

	// The Windows code sets the blend equation only for blendop == 1; any
	// other non-zero value enabled blending and left whatever equation the
	// device happened to carry. That is not expressible here -- a pipeline has
	// no "leave it as it was" -- and it was never a deliberate mode: the only
	// callers pass 0 or 1. So blendop 1 gets its stated equation and any other
	// non-zero value gets the same one, which is what a freshly reset D3D9
	// device would have given it.
	if (blendop != 0) {			// D3DBLENDOP_ADD, SRCALPHA, INVSRCALPHA
		cb.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	}
	else {
		cb.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
		cb.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
	}

	// ONE ATTACHMENT STATE PER COLOUR ATTACHMENT -- a Vulkan requirement with
	// no D3D9 counterpart, and the place D3DRS_COLORWRITEENABLE1/2/3 land.
	VkPipelineColorBlendAttachmentState cbs[8];
	uint32_t nCb = pDevice->GetRenderPassColourCount();
	if (nCb > ARRAYSIZE(cbs)) nCb = ARRAYSIZE(cbs);
	for (uint32_t i = 0; i < nCb; i++) cbs[i] = cb;

	VkPipelineColorBlendStateCreateInfo bs = {};
	bs.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	bs.attachmentCount = nCb;
	bs.pAttachments = cbs;

	VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dsi = {};
	dsi.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dsi.dynamicStateCount = ARRAYSIZE(dyn);
	dsi.pDynamicStates = dyn;

	VkGraphicsPipelineCreateInfo pi = {};
	pi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pi.stageCount = 2;
	pi.pStages = stages;
	pi.pVertexInputState = &vi;
	pi.pInputAssemblyState = &ia;
	pi.pViewportState = &vp;
	pi.pRasterizationState = &rs;
	pi.pMultisampleState = &ms;
	pi.pDepthStencilState = &ds;
	pi.pColorBlendState = &bs;
	pi.pDynamicState = &dsi;
	pi.layout = vkPipeLayout;
	pi.renderPass = pass;
	pi.subpass = 0;

	VkPipeline pipe = VK_NULL_HANDLE;
	if (vkCreateGraphicsPipelines(pDevice->GetDevice(), pDevice->GetPipelineCache(),
								  1, &pi, NULL, &pipe) != VK_SUCCESS) {
		LogErr("ImageProcessing(%s): vkCreateGraphicsPipelines failed. Entrypoint[%s]", file, entry);
		return VK_NULL_HANDLE;
	}

	Pipelines[key] = pipe;
	return pipe;
}


// ================================================================================================
//
bool ImageProcessing::SetupViewPort()
{

	// Check that the first render target is valid
	//
	// pRtg[0]->GetDesc(&desc) becomes desc = pRtg[0]->Desc(). A D3D9 surface
	// answered questions about itself; a VkImage answers none, which is why
	// VulkanTexture records what it was asked for. See VulkanTypes.h.
	if (pRtg[0]) desc = pRtg[0]->Desc();
	else {
		LogErr("ImageProcessing(%s): No render target is set", _PTR(this));
		return false;
	}

	VulkanImageDesc ds;

	// Check that all additional render targets have the same size
	//
	for (int i=1;i<4;i++) {
		if (pRtg[i]) {
			ds = pRtg[i]->Desc();
			if ((ds.Height!=desc.Height) || (ds.Width!=desc.Width)) {
				LogErr("ImageProcessing(%s): All render targets must be same the size", _PTR(this));
				return false;
			}
		}
		else break;
	}

	// Setup view-projection matrix and viewport
	//
	// D3DXMatrixOrthoOffCenterLH becomes VMAT_OrthoOffCenterLH -- the same
	// matrix, written out in VulkanUtil.cpp from the D3DX documentation's own
	// definition, because D3DX is a Direct3D utility library with no Vulkan
	// counterpart. The DEPTH RANGE needs no adjustment: D3D9 clip space is
	// 0 <= z <= w and so is Vulkan's. It is OpenGL, with -w <= z <= w, that
	// would have needed a different matrix. Same finding, argued at length,
	// in VulkanPad.cpp's SetViewMode.
	VMAT_OrthoOffCenterLH(&mVP, 0.0f, (float)desc.Width, (float)desc.Height, 0.0f, 0.0f, 1.0f);

	// The viewport is still computed here and from the same numbers, but it
	// is NOT pushed: VulkanDevice::BeginOffscreen sets the viewport and the
	// scissor from the extent of the attachments it is handed, which is this
	// same rectangle reached from the same place. So SetViewport() has no
	// call here -- see note 1 in the file header.
	iVP.x = 0.0f;
	iVP.y = 0.0f;
	iVP.width  = (float)desc.Width;
	iVP.height = (float)desc.Height;
	iVP.minDepth = 0.0f;
	iVP.maxDepth = 1.0f;

	// pVSConst->SetMatrix/SetVector(pDevice, handle, value) becomes
	// WriteConstants(table, handle, bytes). D3DX took the device because a
	// constant table wrote into the device's registers; the bytes go into a
	// uniform buffer here, so the buffer is the destination and the device is
	// not a parameter.
	if (!WriteConstants(pVSConst, hVP, &mVP, sizeof(FMATRIX4))) return false;
	FVECTOR4 vSize(float(desc.Width), float(desc.Height),
				   1.0f/float(desc.Width), 1.0f/float(desc.Height));
	if (!WriteConstants(pVSConst, hSiz, &vSize, sizeof(FVECTOR4))) return false;
	if (!WriteConstants(pVSConst, hPos, &vTemplate, sizeof(FVECTOR4))) return false;
	return true;
}


// ================================================================================================
//
void ImageProcessing::SetTemplate(float w, float h, float x, float y)
{
	vTemplate = FVECTOR4(w, h, x, y);
}


// ================================================================================================
//
void ImageProcessing::SetMesh(const MESHHANDLE hMesh, const char *tex, gcIPInterface::ipicull cull)
{
	pMesh = GetSketchMesh(hMesh);

	mesh_cull = cull;

	if (tex) {
		const ShaderReflection::Var *hVar = pPSConst ? pPSConst->GetConstantByName(tex) : NULL;
		if (!hVar) {
			LogErr("IPInterface::SetSketchMesh() Invalid variable name [%s]", tex);
			return;
		}
		mesh_tex_idx = pPSConst->GetSamplerIndex(hVar);
		// The slot also has to remember which BINDING it feeds; on Windows the
		// sampler index WAS the device slot, so one number answered both
		// questions. See the note on pTextures::binding in the header.
		if (mesh_tex_idx >= 0 && mesh_tex_idx < int(ARRAYSIZE(pTextures)))
			pTextures[mesh_tex_idx].binding = hVar->binding;
	}
	else mesh_tex_idx = -1;
}


// ================================================================================================
//
bool ImageProcessing::Execute(bool bInScene)
{
	return Execute(0, bInScene, gcIPInterface::ipitemplate::Rect);
}


// ================================================================================================
//
bool ImageProcessing::Execute(const char *shader, bool bInScene, DWORD blendop)
{
	Activate(shader);
	return Execute(blendop, bInScene, gcIPInterface::ipitemplate::Rect);
}


// ================================================================================================
//
bool ImageProcessing::Execute(DWORD blendop, bool bInScene, gcIPInterface::ipitemplate mode, int grp)
{
	// bInScene has no counterpart; see note 7 in the file header. The
	// parameter stays because gcIPInterface passes it.
	(void)bInScene;

	if (!IsOK()) return false;
	if (!CreateResources()) return false;
	if (!SetupViewPort()) return false;

	// Set device state -------------------------------------------------------
	//
	// EVERY LINE OF THE WINDOWS BLOCK THAT STOOD HERE IS PIPELINE STATE. The
	// two SetShader calls, SetVertexDeclaration and the eleven
	// SetRenderState calls are fields of the VkPipeline GetPipeline() builds,
	// listed one by one in the note above it. The save and restore of
	// D3DRS_FILLMODE around the call go with them: this class's pipelines are
	// built with VK_POLYGON_MODE_FILL and nothing global is changed, so there
	// is nothing to put back.

	// Define vertices --------------------------------------------------------
	//
	SMVERTEX Vertex[4] = {
		{0, 0, 0, 0, 0},
		{0, 1, 0, 0, 1},
		{1, 1, 0, 1, 1},
		{1, 0, 0, 1, 0}
	};

	static WORD cIndex[6] = {0, 2, 1, 0, 3, 2};

	// Set render targets -----------------------------------------------------
	//
	// Was four GetRenderTarget/SetRenderTarget pairs plus the three
	// COLORWRITEENABLE1/2/3 states. All of it becomes the attachment list
	// BeginOffscreen is handed: the colour write masks are per-attachment
	// pipeline state now (see GetPipeline), and there is nothing to save --
	// EndOffscreen restores the frame's command buffer itself.
	//
	// The count stops at the first NULL, which is what SetOutput's own
	// documentation says it means: "After a NULL render target all later
	// targets are ignored."
	uint32_t nRtg = 0;
	while (nRtg < 4 && pRtg[nRtg]) nRtg++;
	if (nRtg == 0) return false;

	// Set Depth-Stencil surface ----------------------------------------------
	//
	// The two ZENABLE/ZWRITEENABLE branches become one bool that joins the
	// pipeline key; the surface itself becomes BeginOffscreen's depth
	// attachment. pDepthBak has nothing to save into -- see the header.
	const bool bDepth = (pDepth != NULL);

	// Set textures and samplers -----------------------------------------------
	//
	// Was, per slot: nine SetSamplerState calls decoding the IPF_ flags, then
	// SetTexture. The decoding is in UpdateSampler() and the binding is in
	// BindResources(); this loop is what is left of the outer one -- make
	// sure each occupied slot owns a VkSampler matching its flags.
	for (size_t idx=0;idx<ARRAYSIZE(pTextures);idx++) {

		if (pTextures[idx].hTex==NULL) continue;

		UpdateSampler((int)idx);
	}

	// A fresh set of descriptor sets for this call; see the note on vkPool.
	vkResetDescriptorPool(pDevice->GetDevice(), vkPool, 0);

	// Execute ----------------------------------------------------------------
	//
	// BeginScene/EndScene become BeginOffscreen/EndOffscreen, which is not the
	// same pairing spelled differently: BeginScene only said "I am about to
	// draw", where this builds or fetches a render pass for these attachment
	// formats, a framebuffer for these exact images, transitions them, and
	// swaps in a command buffer of its own. EndOffscreen submits and waits,
	// which is the guarantee EndScene() gave on a render target the next call
	// sampled.
	if (!pDevice->BeginOffscreen(pRtg, nRtg, pDepth)) {
		LogErr("ImageProcessing(%s): BeginOffscreen failed. Entrypoint[%s]", file, entry);
		return false;
	}

	VkCommandBuffer cmd = pDevice->GetCommandBuffer();

	// THE VIEWPORT, WITH A NEGATIVE HEIGHT, AND IT IS PUSHED AFTER ALL.
	//
	// SetupViewPort builds VMAT_OrthoOffCenterLH(0, W, H, 0, 0, 1) -- the
	// reference's matrix unchanged -- which sends screen y = 0 to clip +1
	// because in D3D9 clip +1 is the TOP of the target. In Vulkan clip +1 is
	// the BOTTOM, so against BeginOffscreen's plain top-left viewport every
	// image this class draws comes out MIRRORED VERTICALLY, and is then
	// sampled as though it were not.
	//
	// That is not a subtle shading difference. vPlanet::UpdateScatter builds
	// all seven atmospheric lookup tables through this path, and Scatter.glsl
	// indexes them by altitude on V; upside down, every lookup returns the
	// value for the opposite altitude.
	//
	// The flip is issued HERE rather than in BeginOffscreen because
	// BeginOffscreen is shared: the sketchpad draws MFD and HUD content into
	// offscreen surfaces with no projection matrix of its own and is correct
	// against the plain viewport, so flipping it there turns those upside
	// down instead. ShaderClass and VulkanEffectFile are indifferent -- both
	// re-issue their own viewport at bind time.
	iVP.y = float(desc.Height);
	iVP.height = -float(desc.Height);
	vkCmdSetViewport(cmd, 0, 1, &iVP);

	bool bOK = true;

	if (mode == gcIPInterface::ipitemplate::Rect)
	{
		// DrawIndexedPrimitiveUP(TRIANGLELIST, 0, 4, 2, cIndex, INDEX16,
		// Vertex, sizeof(SMVERTEX)). The two counts were VERTICES and
		// PRIMITIVES; vkCmdDrawIndexed takes INDICES, so 2 primitives is 6.
		VkPipeline pipe = GetPipeline(blendop, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
									  false, bDepth, pPosTexDecl);

		if (!pipe || !EnsureScratch(sizeof(Vertex), sizeof(cIndex))) bOK = false;
		else {
			if (void *p = pScratchVB->Map()) { memcpy(p, Vertex, sizeof(Vertex)); pScratchVB->Unmap(); }
			if (void *p = pScratchIB->Map()) { memcpy(p, cIndex, sizeof(cIndex)); pScratchIB->Unmap(); }
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
			if (!BindResources(cmd)) bOK = false;
			else {
				VkBuffer vb = pScratchVB->Buffer();
				VkDeviceSize off = 0;
				vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
				vkCmdBindIndexBuffer(cmd, pScratchIB->Buffer(), 0, VK_INDEX_TYPE_UINT16);
				vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);
			}
		}
	}


	if (mode == gcIPInterface::ipitemplate::Octagon)
	{
		// DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 8, pOcta, sizeof(SMVERTEX)) --
		// eight triangles from ten fan vertices. The fan becomes an indexed
		// TRIANGLELIST over the same ten vertices; see note 6 in the file
		// header for why the topology is not carried over literally.
		VkPipeline pipe = GetPipeline(blendop, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
									  false, bDepth, pPosTexDecl);

		const VkDeviceSize vbytes = 10 * sizeof(SMVERTEX);

		if (!pipe || !EnsureScratch(vbytes, sizeof(cOcta))) bOK = false;
		else {
			if (void *p = pScratchVB->Map()) { memcpy(p, pOcta, (size_t)vbytes); pScratchVB->Unmap(); }
			if (void *p = pScratchIB->Map()) { memcpy(p, cOcta, sizeof(cOcta)); pScratchIB->Unmap(); }
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
			if (!BindResources(cmd)) bOK = false;
			else {
				VkBuffer vb = pScratchVB->Buffer();
				VkDeviceSize off = 0;
				vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
				vkCmdBindIndexBuffer(cmd, pScratchIB->Buffer(), 0, VK_INDEX_TYPE_UINT16);
				vkCmdDrawIndexed(cmd, ARRAYSIZE(cOcta), 1, 0, 0, 0);
			}
		}
	}


	if (mode == gcIPInterface::ipitemplate::Mesh)
	{
		// SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW) becomes the bCull
		// argument, which is pipeline state; see GetPipeline.
		//
		// THE VERTEX DECLARATION CHANGES HERE and it did on Windows too --
		// silently. SketchMesh renders NTVERTEX, not SMVERTEX, so the
		// declaration set at the top of the function was wrong for this path
		// and D3D9 tolerated it because SetVertexDeclaration was device state
		// SketchMesh::Init could and did overwrite. Vulkan bakes the layout
		// into the pipeline, so the right declaration has to be named here.
		VkPipeline pipe = GetPipeline(blendop, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
									  true, bDepth, pNTVertexDecl);

		if (!pipe || !pMesh) bOK = false;
		else {
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);

			pMesh->Init();

			DWORD nGrp = pMesh->GroupCount();
			if (grp < 0) for (DWORD i=0;i<nGrp;i++) {
				if (mesh_tex_idx >= 0) {
					SURFHANDLE hTex = pMesh->GetTexture(i);
					pTextures[mesh_tex_idx].hTex = hTex ? SURFACE(hTex)->GetTexture() : NULL;
				}
				// One descriptor set per group, because the texture changes
				// between them; see the note on vkPool in the header.
				if (!BindResources(cmd)) { bOK = false; break; }
				pMesh->RenderGroup(i);
			}
			else {
				if (mesh_tex_idx >= 0) {
					SURFHANDLE hTex = pMesh->GetTexture(grp);
					pTextures[mesh_tex_idx].hTex = hTex ? SURFACE(hTex)->GetTexture() : NULL;
				}
				if (!BindResources(cmd)) bOK = false;
				else pMesh->RenderGroup(grp);
			}
		}
	}

	pDevice->EndOffscreen();

	// Disconnect render targets ----------------------------------------------
	//
	// The two Windows blocks that stood here -- put the depth surface back,
	// put the four render targets back and release the references
	// GetRenderTarget added -- have no counterpart: EndOffscreen has just
	// restored the frame's command buffer, and no reference was taken because
	// nothing here is reference counted.

	// Disconnect textures -----------------------------------------------------
	//
	// Was SetTexture(idx, NULL) for every occupied slot. There is no "unbind a
	// texture" in Vulkan: a descriptor set is written and a draw either uses
	// it or does not, and the next Execute() allocates a fresh set. Same
	// finding as ShaderClass::DetachTextures.

	return bOK;
}


// ================================================================================================
// The Set* family.
//
// ID3DXConstantTable::SetFloatArray / SetIntArray / SetBoolArray / SetValue
// all become one WriteConstants, because all four did the same thing: put N
// bytes at the constant's offset. D3DX needed four because it also converted
// -- SetBoolArray took a BOOL (32-bit int) array and SetFloatArray a float
// one, and the table knew which registers to touch. In a uniform block the
// layout is the shader's and the bytes are written as given, so the only
// conversion that survives is the bool one below, which is a real widening
// from C++ bool to the 32-bit bool of a std140/scalar block.
//
// THE COUNT ARGUMENTS ARE GONE FOR THE SAME REASON. bytes>>2 was "how many
// floats"; WriteConstants takes bytes, which is what the caller already had.
// ================================================================================================


// ================================================================================================
//
void ImageProcessing::SetFloat(const char *var, const void *val, int bytes)
{
	const ShaderReflection::Var *hVar = pPSConst ? pPSConst->GetConstantByName(var) : NULL;

	if (!hVar) {
		LogErr("IPInterface::SetFloat() Invalid variable name [%s]. File[%s], Entrypoint[%s]", var, file, entry);
		return;
	}

	if (!WriteConstants(pPSConst, hVar, val, bytes)) {
		LogErr("IPInterface::SetFloat() Failed. Variable[%s], File[%s], Entrypoint[%s]", var, file, entry);
	}
}


// ================================================================================================
//
void ImageProcessing::SetInt(const char *var, const int *val, int bytes)
{
	const ShaderReflection::Var *hVar = pPSConst ? pPSConst->GetConstantByName(var) : NULL;

	if (!hVar) {
		LogErr("IPInterface::SetInt() Invalid variable name [%s]. File[%s], Entrypoint[%s]", var, file, entry);
		return;
	}

	if (!WriteConstants(pPSConst, hVar, val, bytes)) {
		LogErr("IPInterface::SetInt() Failed. Variable[%s], File[%s], Entrypoint[%s]", var, file, entry);
	}
}


// ================================================================================================
//
void ImageProcessing::SetBool(const char *var, const bool *val, int bytes)
{
	const ShaderReflection::Var *hVar = pPSConst ? pPSConst->GetConstantByName(var) : NULL;

	if (!hVar) {
		LogErr("IPInterface::SetBool() Invalid variable name [%s]. File[%s], Entrypoint[%s]", var, file, entry);
		return;
	}

	// 'bytes' is the COUNT of bools here, not a byte count -- the callers
	// pass sizeof(bool), which is 1 -- and the widening loop below is the
	// Windows one unchanged. What changes is the destination: a BOOL array in
	// a constant register file becomes 32-bit uints in a uniform block, so
	// the write is count * 4 bytes.
	int *data = new int[bytes];
	for (int i=0;i<bytes;i++) data[i] = val[i];

	if (!WriteConstants(pPSConst, hVar, data, bytes * int(sizeof(int)))) {
		LogErr("IPInterface::SetBool() Failed. Variable[%s], File[%s], Entrypoint[%s]", var, file, entry);
	}

	delete []data;
	data = NULL;
}


// ================================================================================================
//
void ImageProcessing::SetStruct(const char *var, const void *val, int bytes)
{
	const ShaderReflection::Var *hVar = pPSConst ? pPSConst->GetConstantByName(var) : NULL;

	if (!hVar) {
		LogErr("IPInterface::SetStruct() Invalid variable name [%s]. File[%s], Entrypoint[%s]", var, file, entry);
		return;
	}

	if (!WriteConstants(pPSConst, hVar, val, bytes)) {
		LogErr("IPInterface::SetStruct() Failed. Variable[%s], File[%s], Entrypoint[%s]", var, file, entry);
	}
}


// ================================================================================================
//
void ImageProcessing::SetFloat(const char *var, float val)
{
	SetFloat(var, (const float*)&val, sizeof(float));
}


// ================================================================================================
//
void ImageProcessing::SetInt(const char *var, int val)
{
	SetInt(var, (const int*)&val, sizeof(int));
}


// ================================================================================================
//
void ImageProcessing::SetBool(const char *var, bool val)
{
	SetBool(var, (const bool*)&val, sizeof(bool));
}


// ================================================================================================
// THE FOUR VS FUNCTIONS BELOW ALL WRITE THROUGH pPSConst ON WINDOWS.
//
//     D3DXHANDLE hVar = pVSConst->GetConstantByName(NULL, var);
//     if (pPSConst->SetFloatArray(pDevice, hVar, ...) != S_OK) ...
//
// The handle comes from the VERTEX table and the write goes through the PIXEL
// one, in all four, and the error messages say "IPInterface::SetFloat()"
// rather than SetVSFloat -- so this is one copy-paste slip propagated four
// times, not a deliberate aliasing. It is the SAME MISTAKE, in the same
// shape, as the one in ShaderClass::SetPSConstants; see the note there.
//
// In D3DX it was survivable: a D3DXHANDLE is an opaque pointer with no idea
// which table it came from, and SetFloatArray on the wrong table with a
// foreign handle failed quietly. Here a Var carries the stage it was
// reflected from and WriteConstants writes into that stage's buffer, so the
// mistake cannot be spelled -- the handle decides the destination. That is
// the fix, and it is a correction rather than a translation.
// ================================================================================================
void ImageProcessing::SetVSFloat(const char *var, const void *val, int bytes)
{
	const ShaderReflection::Var *hVar = pVSConst ? pVSConst->GetConstantByName(var) : NULL;

	if (!WriteConstants(pVSConst, hVar, val, bytes)) {
		LogErr("IPInterface::SetVSFloat() Failed. Variable[%s], File[%s], Entrypoint[%s]", var, file, entry);
	}
}


// ================================================================================================
//
void ImageProcessing::SetVSInt(const char *var, const int *val, int bytes)
{
	const ShaderReflection::Var *hVar = pVSConst ? pVSConst->GetConstantByName(var) : NULL;

	if (!WriteConstants(pVSConst, hVar, val, bytes)) {
		LogErr("IPInterface::SetVSInt() Failed. Variable[%s], File[%s], Entrypoint[%s]", var, file, entry);
	}
}


// ================================================================================================
//
void ImageProcessing::SetVSBool(const char *var, const bool *val, int bytes)
{
	const ShaderReflection::Var *hVar = pVSConst ? pVSConst->GetConstantByName(var) : NULL;

	if (!hVar) return;
	int *data = new int[bytes];
	for (int i = 0; i<bytes; i++) data[i] = val[i];

	if (!WriteConstants(pVSConst, hVar, data, bytes * int(sizeof(int)))) {
		LogErr("IPInterface::SetVSBool() Failed. Variable[%s], File[%s], Entrypoint[%s]", var, file, entry);
	}

	delete[]data;
}


// ================================================================================================
//
void ImageProcessing::SetVSStruct(const char *var, const void *val, int bytes)
{
	const ShaderReflection::Var *hVar = pVSConst ? pVSConst->GetConstantByName(var) : NULL;
	if (!hVar) return;
	if (!WriteConstants(pVSConst, hVar, val, bytes)) {
		LogErr("IPInterface::SetVSStruct() Failed. Variable[%s], File[%s], Entrypoint[%s]", var, file, entry);
	}
}


// ================================================================================================
//
void ImageProcessing::SetVSFloat(const char *var, float val)
{
	SetVSFloat(var, (const float*)&val, sizeof(float));
}


// ================================================================================================
//
void ImageProcessing::SetVSInt(const char *var, int val)
{
	SetVSInt(var, (const int*)&val, sizeof(int));
}


// ================================================================================================
//
void ImageProcessing::SetVSBool(const char *var, bool val)
{
	SetVSBool(var, (const bool*)&val, sizeof(bool));
}


// ================================================================================================
//
void ImageProcessing::SetTexture(const char *var, SURFHANDLE hTex, DWORD flags)
{
	const ShaderReflection::Var *hVar = pPSConst ? pPSConst->GetConstantByName(var) : NULL;

	if (!hVar) {
		LogErr("IPInterface::SetTexture() Invalid variable name [%s]. File[%s], Entrypoint[%s]", var, file, entry);
		return;
	}

	DWORD idx = pPSConst->GetSamplerIndex(hVar);
	if (idx >= ARRAYSIZE(pTextures)) return;

	if (!hTex) {
		pTextures[idx].hTex = NULL;
		pTextures[idx].flags = 0;
		return;
	}

	pTextures[idx].hTex = SURFACE(hTex)->GetTexture();
	pTextures[idx].flags = flags;
	// The binding this slot feeds; see the note on pTextures::binding.
	pTextures[idx].binding = hVar->binding;
}


// ================================================================================================
//
void ImageProcessing::SetTextureNative(const char *var, VulkanTexture *hTex, DWORD flags)
{
	const ShaderReflection::Var *hVar = pPSConst ? pPSConst->GetConstantByName(var) : NULL;

	if (!hVar) {
		LogErr("IPInterface::SetTextureNative() Invalid variable name [%s]. File[%s], Entrypoint[%s]", var, file, entry);
		return;
	}

	DWORD idx = pPSConst->GetSamplerIndex(hVar);
	if (idx >= ARRAYSIZE(pTextures)) return;

	if (!hTex) {
		pTextures[idx].hTex = NULL;
		pTextures[idx].flags = 0;
		return;
	}

	pTextures[idx].hTex = hTex;
	pTextures[idx].flags = flags;
	pTextures[idx].binding = hVar->binding;
}


// ================================================================================================
//
void ImageProcessing::SetOutput(int id, SURFHANDLE hTex)
{
	if (id<0) id=0;
	if (id>3) id=3;

	// GetSurface() still answers "the image this surface renders into", which
	// is what LPDIRECT3DSURFACE9 was. See VulkanSurface.h on why GetSurface()
	// and GetTexture() are now the same image.
	if (hTex) pRtg[id] = SURFACE(hTex)->GetSurface();
	else 	  pRtg[id] = NULL;
}


// ================================================================================================
//
void ImageProcessing::SetDepthStencil(VulkanTexture *hSrf)
{
	pDepth = hSrf;
}


// ================================================================================================
//
void ImageProcessing::SetOutputNative(int id, VulkanTexture *hSrf)
{
	if (id<0) id=0;
	if (id>3) id=3;
	pRtg[id] = hSrf;
}


// ================================================================================================
//
bool ImageProcessing::IsOK()
{
	for (auto x : Shaders) {
		if (x.second.pPixel == VK_NULL_HANDLE) return false;
		if (x.second.pPSConst == NULL) return false;
	}
	return (pVertex && pVSConst && pDevice && hVP && hPos && hSiz);
}








// ================================================================================================
// PUBLIC INTERFACE
// ================================================================================================
//
// Not one line of this block changes. gcIPInterface is the SDK's face on this
// class, every method forwards, and nothing it names is a Direct3D type.
//

gcIPInterface::~gcIPInterface()
{

}

bool gcIPInterface::CompileShader(const char *Entry)
{
	return pIPI->CompileShader(Entry);
}

bool gcIPInterface::Activate(const char *Shader)
{
	return pIPI->Activate(Shader);
}
	
void gcIPInterface::SetFloat(const char *var, float val)
{
	pIPI->SetFloat(var, val);
}

void gcIPInterface::SetInt(const char *var, int val)
{
	pIPI->SetInt(var, val);
}

void gcIPInterface::SetBool(const char *var, bool val)
{
	pIPI->SetBool(var, val);
}

void gcIPInterface::SetFloat(const char *var, const void *val, int bytes)
{
	pIPI->SetFloat(var, val, bytes);
}

void gcIPInterface::SetInt(const char *var, const int *val, int bytes)
{
	pIPI->SetInt(var, val, bytes);
}

void gcIPInterface::SetBool(const char *var, const bool *val, int bytes)
{
	pIPI->SetBool(var, val, bytes);
}

void gcIPInterface::SetStruct(const char *var, const void *val, int bytes)
{
	pIPI->SetStruct(var, val, bytes);
}

void gcIPInterface::SetVSFloat(const char *var, float val)
{
	pIPI->SetVSFloat(var, val);
}

void gcIPInterface::SetVSInt(const char *var, int val)
{
	pIPI->SetVSInt(var, val);
}

void gcIPInterface::SetVSBool(const char *var, bool val)
{
	pIPI->SetVSBool(var, val);
}

void gcIPInterface::SetVSFloat(const char *var, const void *val, int bytes)
{
	pIPI->SetVSFloat(var, val, bytes);
}

void gcIPInterface::SetVSInt(const char *var, const int *val, int bytes)
{
	pIPI->SetVSInt(var, val, bytes);
}

void gcIPInterface::SetVSBool(const char *var, const bool *val, int bytes)
{
	pIPI->SetVSBool(var, val, bytes);
}

void gcIPInterface::SetVSStruct(const char *var, const void *val, int bytes)
{
	pIPI->SetVSStruct(var, val, bytes);
}

void gcIPInterface::SetTexture(const char *var, SURFHANDLE hTex, DWORD flags)
{
	pIPI->SetTexture(var, hTex, flags);
}

void gcIPInterface::SetOutput(int id, SURFHANDLE hSrf)
{
	pIPI->SetOutput(id, hSrf);
}
	
bool gcIPInterface::IsOK()
{
	return pIPI->IsOK();
}

void gcIPInterface::SetOutputRegion(float w, float h, float x, float y)
{
	pIPI->SetTemplate(w, h, x, y);
}

void gcIPInterface::SetMesh(MESHHANDLE hMesh, const char *tex, ipicull cull)
{
	pIPI->SetMesh(hMesh, tex, cull);
}

bool gcIPInterface::Execute(bool bInScene)
{
	return pIPI->Execute(bInScene);
}

bool gcIPInterface::Execute(DWORD blendop, bool bInScene, ipitemplate mde)
{
	return pIPI->Execute(blendop, bInScene, mde);
}

int gcIPInterface::FindDefine(const char *key)
{
	return pIPI->FindDefine(key);
}
