// ==============================================================
// CelSphere.cpp
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2006-2026 Martin Schweiger
//				 2012-2016 Jarmo Nikkanen
// ==============================================================

// ==============================================================
// Class CelestialSphere (implementation)
//
// This class is responsible for rendering the celestial sphere
// background (stars, constellations, grids, labels, etc.)
// ==============================================================
//
// Six render functions share one Windows shape, and one change dominates: the
// primitive topology moves from the draw call into the pipeline. On Windows a
// D3DPRIMITIVETYPE is an argument to DrawPrimitive; in Vulkan it is baked into
// the VkPipeline that BeginPass builds, so FX->SetTopology must be called
// before Begin(). Four are used here: POINT_LIST for stars, LINE_LIST for the
// constellations, LINE_STRIP for the grids, TRIANGLE_LIST for the labels.
// DrawPrimitive's primitive counts also become vkCmdDraw's vertex counts; that
// arithmetic is written out at each call.
//
// The conversion fixes a bug in the Windows source: RenderConstellationLines
// and RenderConstellationBoundaries pass a vertex count to
// DrawPrimitive(D3DPT_LINELIST, ...), whose last argument is a primitive
// count, so D3D9 was told to read twice the buffer. vkCmdDraw takes vertices,
// so the same variable is now correct.
// ==============================================================

#include "CelSphere.h"
#include "CSphereMgr.h"
#include "Scene.h"
#include "VulkanConfig.h"
#include "VulkanSurface.h"
#include "VulkanEffect.h"
#include "VulkanPad.h"
#include "AABBUtil.h"

using std::min;

#define NSEG 64 // number of segments in celestial grid lines

using namespace oapi;

// ==============================================================
// D3DXCOLOR's conversion to a packed D3DCOLOR, written out: clamp each channel
// to [0,1], round to eight bits, pack 0xAARRGGBB -- which is what
// VERTEX_XYZC::col holds and what PosColorDecl's VK_FORMAT_B8G8R8A8_UNORM
// reads back byte for byte on a little-endian host.

static inline DWORD PackColour(float r, float g, float b, float a)
{
	auto ch = [](float f) -> DWORD {
		if (f <= 0.0f) return 0;
		if (f >= 1.0f) return 255;
		return (DWORD)(f * 255.0f + 0.5f);
	};
	return (ch(a) << 24) | (ch(r) << 16) | (ch(g) << 8) | ch(b);
}

// ==============================================================

VulkanCelestialSphere::VulkanCelestialSphere(VulkanClient *gc, Scene *scene)
	: oapi::CelestialSphere(gc)
	, m_gc(gc)
	, m_scene(scene)
	, m_pDevice(gc->GetDevice())
	// Was gc->GetHardwareCaps()->MaxPrimitiveCount; Vulkan has no per-draw
	// primitive limit. See MAX_STAR_CHUNK in CelSphere.h.
	, maxNumVertices(MAX_STAR_CHUNK)
{
	for (auto&& vtx : m_azGridLabelVtx)
		vtx = nullptr;
	m_elGridLabelVtx = nullptr;
	m_GridLabelIdx = nullptr;
	m_GridLabelTex = nullptr;

	InitStars();
	InitConstellationLines();
	InitConstellationBoundaries();
	LoadConstellationLabels();
	AllocGrids();
	m_bkgImgMgr = new CSphereManager(gc, scene);

	m_textBlendAdditive = true;
	m_mjdPrecessionChecked = -1e10;

	SURFHANDLE hSurf = m_gc->clbkLoadTexture("gridlabel.dds", 0);
	if (hSurf) m_GridLabelTex = SURFACE(hSurf);
	if (!m_GridLabelTex)
		oapiWriteLogError("Failed to load texture gridlabel.dds");
}

// ==============================================================

VulkanCelestialSphere::~VulkanCelestialSphere()
{
	ClearStars();
	// Were Release() calls on a COM refcount; a Vulkan buffer is destroyed by
	// the device that made it.
	if (m_clVtx) m_pDevice->DestroyBuffer(m_clVtx);
	if (m_cbVtx) m_pDevice->DestroyBuffer(m_cbVtx);
	if (m_grdLngVtx) m_pDevice->DestroyBuffer(m_grdLngVtx);
	if (m_grdLatVtx) m_pDevice->DestroyBuffer(m_grdLatVtx);

	for (auto vtx : m_azGridLabelVtx)
		if (vtx) m_pDevice->DestroyBuffer(vtx);
	if (m_elGridLabelVtx)
		m_pDevice->DestroyBuffer(m_elGridLabelVtx);
	if (m_GridLabelIdx)
		m_pDevice->DestroyBuffer(m_GridLabelIdx);

	delete m_bkgImgMgr;
	if (m_GridLabelTex)
		DELETE_SURFACE(m_GridLabelTex);
}

// ==============================================================

void VulkanCelestialSphere::InitCelestialTransform()
{
	m_rotCelestial = Ecliptic_CelestialAtEpoch();

	m_transformCelestial.m11 = (float)m_rotCelestial.m11; m_transformCelestial.m12 = (float)m_rotCelestial.m12; m_transformCelestial.m13 = (float)m_rotCelestial.m13; m_transformCelestial.m14 = 0.0f;
	m_transformCelestial.m21 = (float)m_rotCelestial.m21; m_transformCelestial.m22 = (float)m_rotCelestial.m22; m_transformCelestial.m23 = (float)m_rotCelestial.m23; m_transformCelestial.m24 = 0.0f;
	m_transformCelestial.m31 = (float)m_rotCelestial.m31; m_transformCelestial.m32 = (float)m_rotCelestial.m32; m_transformCelestial.m33 = (float)m_rotCelestial.m33; m_transformCelestial.m34 = 0.0f;
	m_transformCelestial.m41 = 0.0f;                      m_transformCelestial.m42 = 0.0f;                      m_transformCelestial.m43 = 0.0f;                      m_transformCelestial.m44 = 1.0f;

	m_mjdPrecessionChecked = oapiGetSimMJD();
}

// ==============================================================

bool VulkanCelestialSphere::LocalHorizonTransform(MATRIX3& R, FMATRIX4& T)
{
	MATRIX3 rot;
	if (LocalHorizon_Ecliptic(rot)) {
		R = transp(rot);
		// FMATRIX4's sixteen-float constructor takes the same row order the
		// braced D3DXMATRIX did.
		T = FMATRIX4(
			(float)R.m11, (float)R.m12, (float)R.m13, 0.0f,
			(float)R.m21, (float)R.m22, (float)R.m23, 0.0f,
			(float)R.m31, (float)R.m32, (float)R.m33, 0.0f,
			0.0f,         0.0f,         0.0f,         1.0f
		);
		return true;
	}
	return false;
}

// ==============================================================

void VulkanCelestialSphere::InitStars ()
{
	ClearStars();

	if (*(bool*)m_gc->GetConfigParam(CFGPRM_CSPHEREUSESTARDOTS)) {

		const std::vector<oapi::CelestialSphere::StarRenderRec> sList = LoadStars();
		m_nsVtx = sList.size();
		if (!m_nsVtx) return;

		DWORD i, j, nv, idx = 0;

		// convert star database to vertex buffers
		DWORD nbuf = (m_nsVtx + maxNumVertices - 1) / maxNumVertices; // number of buffers required
		m_sVtx.resize(nbuf);
		for (auto it = m_sVtx.begin(); it != m_sVtx.end(); it++) {
			nv = min((DWORD)maxNumVertices, m_nsVtx - idx);
			// CreateVertexBuffer(D3DUSAGE_WRITEONLY, D3DPOOL_DEFAULT) -> one
			// host-visible buffer with the vertex usage flag.
			*it = m_pDevice->CreateBuffer(nv * sizeof(VERTEX_XYZC),
										  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
			if (!*it) { LogErr("CelSphere: star vertex buffer allocation failed"); return; }
			VERTEX_XYZC* vbuf = (VERTEX_XYZC *)(*it)->Map();
			if (!vbuf) return;
			for (j = 0; j < nv; j++) {
				const oapi::CelestialSphere::StarRenderRec& rec = sList[idx];
				VERTEX_XYZC& v = vbuf[j];
				v.x = (float)rec.pos.x;
				v.y = (float)rec.pos.y;
				v.z = (float)rec.pos.z;
				// Was D3DXCOLOR(r,g,b,1) assigned to a D3DCOLOR member, which
				// invoked D3DXCOLOR's conversion operator. See PackColour.
				v.col = PackColour((float)rec.col.x, (float)rec.col.y, (float)rec.col.z, 1.0f);
				idx++;
			}
			(*it)->Unmap();
		}

		m_starCutoffIdx = ComputeStarBrightnessCutoff(sList);

		(void)i;
	}
}

// ==============================================================

void VulkanCelestialSphere::ClearStars()
{
	for (auto it = m_sVtx.begin(); it != m_sVtx.end(); it++)
		if (*it) m_pDevice->DestroyBuffer(*it);
	m_sVtx.clear();
	m_nsVtx = 0;
}

// ==============================================================

int VulkanCelestialSphere::MapLineBuffer(const std::vector<VECTOR3>& lineVtx, VulkanBuffer *& buf) const
{
	size_t nv = lineVtx.size();
	if (!nv) return 0;

	// create vertex buffer
	buf = m_pDevice->CreateBuffer(sizeof(VERTEX_XYZ) * nv,
								  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
	if (!buf) { LogErr("CelSphere: line vertex buffer allocation failed"); return 0; }
	VERTEX_XYZ* vbuf = (VERTEX_XYZ *)buf->Map();
	if (!vbuf) return 0;
	for (size_t i = 0; i < nv; i++) {
		vbuf[i].x = (float)lineVtx[i].x;
		vbuf[i].y = (float)lineVtx[i].y;
		vbuf[i].z = (float)lineVtx[i].z;
	}
	buf->Unmap();

	return (int)nv;
}

// ==============================================================

void VulkanCelestialSphere::InitConstellationLines()
{
	m_nclVtx = MapLineBuffer(LoadConstellationLines(), m_clVtx);
}

// ==============================================================

void VulkanCelestialSphere::InitConstellationBoundaries()
{
	m_ncbVtx = MapLineBuffer(LoadConstellationBoundaries(), m_cbVtx);
}

// ==============================================================

void VulkanCelestialSphere::AllocGrids ()
{
	int i, j, idx;
	double lng, lat, xz, y;
	VERTEX_XYZ *vbuf;

	m_grdLngVtx = m_pDevice->CreateBuffer(sizeof(VERTEX_XYZ)*(NSEG+1)*11,
										  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
	if (!m_grdLngVtx) { LogErr("CelSphere: grid vertex buffer allocation failed"); return; }
	vbuf = (VERTEX_XYZ *)m_grdLngVtx->Map();
	if (vbuf) {
		for (j = idx = 0; j <= 10; j++) {
			lat = (j-5)*15*RAD;
			xz = cos(lat);
			y  = sin(lat);
			for (i = 0; i <= NSEG; i++) {
				lng = 2.0*PI * (double)i/(double)NSEG;
				vbuf[idx].x = (float)(xz * cos(lng));
				vbuf[idx].z = (float)(xz * sin(lng));
				vbuf[idx].y = (float)y;
				idx++;
			}
		}
		m_grdLngVtx->Unmap();
	}

	m_grdLatVtx = m_pDevice->CreateBuffer(sizeof(VERTEX_XYZ)*(NSEG+1)*12,
										  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
	if (!m_grdLatVtx) { LogErr("CelSphere: grid vertex buffer allocation failed"); return; }
	vbuf = (VERTEX_XYZ *)m_grdLatVtx->Map();
	if (vbuf) {
		for (j = idx = 0; j < 12; j++) {
			lng = j*15*RAD;
			for (i = 0; i <= NSEG; i++) {
				lat = 2.0*PI * (double)i/(double)NSEG;
				xz = cos(lat);
				y  = sin(lat);
				vbuf[idx].x = (float)(xz * cos(lng));
				vbuf[idx].z = (float)(xz * sin(lng));
				vbuf[idx].y = (float)y;
				idx++;
			}
		}
		m_grdLatVtx->Unmap();
	}
}

// ==============================================================

void VulkanCelestialSphere::AllocGridLabels()
{
	VERTEX_XYZ_TEX* vbuf;
	WORD* ibuf;

	const MESHHANDLE hMesh = GridLabelMesh();
	MESHGROUP* grp = oapiMeshGroup(hMesh, 0);

	// create vertex buffers for longitude labels (azimuth/hour angle/longitude)
	for (size_t idx = 0; idx < m_azGridLabelVtx.size(); idx++) {
		VulkanBuffer *& vb = m_azGridLabelVtx[idx];
		vb = m_pDevice->CreateBuffer(sizeof(VERTEX_XYZ_TEX) * grp->nVtx,
									 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
		if (!vb) { LogErr("CelSphere: grid label vertex buffer allocation failed"); return; }
		vbuf = (VERTEX_XYZ_TEX *)vb->Map();
		if (!vbuf) return;
		// nVtx is a DWORD and the counter was an int on Windows too; the cast
		// settles -Wsign-compare.
		for (int i = 0; i < (int)grp->nVtx; i++) {
			vbuf[i].x = (float)grp->Vtx[i].x;
			vbuf[i].y = (float)grp->Vtx[i].y;
			vbuf[i].z = (float)grp->Vtx[i].z;
			vbuf[i].tu = (float)(grp->Vtx[i].tu + idx * 0.1015625);
			vbuf[i].tv = (float)grp->Vtx[i].tv;
		}
		vb->Unmap();
	}

	// the index list is used for both azimuth and elevation grid labels
	// D3DFMT_INDEX16 is not a buffer property in Vulkan: the index type is
	// given to vkCmdBindIndexBuffer at the draw.
	m_GridLabelIdx = m_pDevice->CreateBuffer(sizeof(WORD) * grp->nIdx,
											 VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
	if (!m_GridLabelIdx) { LogErr("CelSphere: grid label index buffer allocation failed"); return; }
	ibuf = (WORD *)m_GridLabelIdx->Map();
	if (ibuf) {
		memcpy(ibuf, grp->Idx, sizeof(WORD) * grp->nIdx);
		m_GridLabelIdx->Unmap();
	}

	// create vertex buffer for latitude labels (just one shared between all grids)
	grp = oapiMeshGroup(hMesh, 1);
	m_elGridLabelVtx = m_pDevice->CreateBuffer(sizeof(VERTEX_XYZ_TEX) * grp->nVtx,
											   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
	if (!m_elGridLabelVtx) { LogErr("CelSphere: grid label vertex buffer allocation failed"); return; }
	vbuf = (VERTEX_XYZ_TEX *)m_elGridLabelVtx->Map();
	if (!vbuf) return;
	for (int i = 0; i < (int)grp->nVtx; i++) {		// see the cast above
		vbuf[i].x = (float)grp->Vtx[i].x;
		vbuf[i].y = (float)grp->Vtx[i].y;
		vbuf[i].z = (float)grp->Vtx[i].z;
		vbuf[i].tu = (float)grp->Vtx[i].tu;
		vbuf[i].tv = (float)grp->Vtx[i].tv;
	}
	m_elGridLabelVtx->Unmap();
}

// ==============================================================

void VulkanCelestialSphere::OnOptionChanged(DWORD cat, DWORD item)
{
	switch (cat) {
	case OPTCAT_CELSPHERE:
		switch (item) {
		case OPTITEM_CELSPHERE_ACTIVATESTARDOTS:
		case OPTITEM_CELSPHERE_STARDISPLAYPARAM:
			InitStars();
			break;
		case OPTITEM_CELSPHERE_ACTIVATESTARIMAGE:
		case OPTITEM_CELSPHERE_STARIMAGECHANGED:
		case OPTITEM_CELSPHERE_ACTIVATEBGIMAGE:
		case OPTITEM_CELSPHERE_BGIMAGECHANGED:
			delete m_bkgImgMgr;
			m_bkgImgMgr = new CSphereManager(m_gc, m_scene);
			break;
		case OPTITEM_CELSPHERE_BGIMAGEBRIGHTNESS:
			if (m_bkgImgMgr) {
				double intens = *(double*)m_gc->GetConfigParam(CFGPRM_CSPHEREINTENS);
				m_bkgImgMgr->SetBgBrightness(intens);
			}
			break;
		}
		break;
	}
}

// ==============================================================

void VulkanCelestialSphere::Render(VulkanDevice *pDevice, const VECTOR3& skyCol)
{
	SetSkyColour(skyCol);

	// Get celestial sphere render flags
	DWORD renderFlag = *(DWORD*)m_gc->GetConfigParam(CFGPRM_PLANETARIUMFLAG);

	// celestial sphere background image
	RenderBkgImage(pDevice);

	if (renderFlag & PLN_ENABLE) {

		// HR() around these is gone: FX->SetTechnique and FX->SetMatrix
		// return bool, not a VkResult, and HR() now checks a VkResult.
		s_FX->SetTechnique(s_eLine);
		s_FX->SetMatrix(s_eWVP, m_scene->GetProjectionViewMatrix());

		// render ecliptic grid
		if (renderFlag & PLN_EGRID) {
			// ColorAdjusted returns an FVECTOR4 in the SDK; the D3DXVECTOR4
			// locals were only there because SetVector took one.
			FVECTOR4 baseCol1(0.0f, 0.2f, 0.3f, 1.0f);
			FVECTOR4 vColor1 = ColorAdjusted(baseCol1);
			s_FX->SetVector(s_eColor, &vColor1);
			RenderGrid(s_FX, false);
			FVECTOR4 baseCol2(0.0f, 0.4f, 0.6f, 1.0f);
			FVECTOR4 vColor2 = ColorAdjusted(baseCol2);
			s_FX->SetVector(s_eColor, &vColor2);
			RenderGreatCircle(s_FX);
			MATRIX3 ident = _M(1, 0, 0, 0, 1, 0, 0, 0, 1);
			double dphi = ElevationScaleRotation(ident);
			RenderGridLabels(s_FX, 2, baseCol2, ident, dphi);
		}

		// render galactic grid
		if (renderFlag & PLN_GGRID) {
			static const MATRIX3& R = Ecliptic_Galactic();
			static FMATRIX4 T((float)R.m11, (float)R.m12, (float)R.m13, 0.0f,
							  (float)R.m21, (float)R.m22, (float)R.m23, 0.0f,
							  (float)R.m31, (float)R.m32, (float)R.m33, 0.0f,
							  0.0f,         0.0f,         0.0f,         1.0f);
			FMATRIX4 rot;
			VMAT_MatrixMultiply(&rot, &T, m_scene->GetProjectionViewMatrix());
			s_FX->SetMatrix(s_eWVP, &rot);
			FVECTOR4 baseCol1(0.3f, 0.0f, 0.0f, 1.0f);
			FVECTOR4 vColor1 = ColorAdjusted(baseCol1);
			s_FX->SetVector(s_eColor, &vColor1);
			RenderGrid(s_FX, false);
			FVECTOR4 baseCol2(0.7f, 0.0f, 0.0f, 1.0f);
			FVECTOR4 vColor2 = ColorAdjusted(baseCol2);
			s_FX->SetVector(s_eColor, &vColor2);
			RenderGreatCircle(s_FX);
			double dphi = ElevationScaleRotation(R);
			RenderGridLabels(s_FX, 2, baseCol2, R, dphi);
		}

		// render celestial grid
		if (renderFlag & PLN_CGRID) {
			if (fabs(m_mjdPrecessionChecked - oapiGetSimMJD()) > 1e3)
				InitCelestialTransform();
			FMATRIX4 rot;
			VMAT_MatrixMultiply(&rot, &m_transformCelestial, m_scene->GetProjectionViewMatrix());
			s_FX->SetMatrix(s_eWVP, &rot);
			FVECTOR4 baseCol1(0.3f, 0.0f, 0.3f, 1.0f);
			FVECTOR4 vColor1 = ColorAdjusted(baseCol1);
			s_FX->SetVector(s_eColor, &vColor1);
			RenderGrid(s_FX, false);
			FVECTOR4 baseCol2(0.7f, 0.0f, 0.7f, 1.0f);
			FVECTOR4 vColor2 = ColorAdjusted(baseCol2);
			s_FX->SetVector(s_eColor, &vColor2);
			RenderGreatCircle(s_FX);
			double dphi = ElevationScaleRotation(m_rotCelestial);
			RenderGridLabels(s_FX, 1, baseCol2, m_rotCelestial, dphi);
		}

		//  render local horizon grid
		if (renderFlag & PLN_HGRID) {
			MATRIX3 R;
			FMATRIX4 T, rot;
			if (LocalHorizonTransform(R, T)) {
				VMAT_MatrixMultiply(&rot, &T, m_scene->GetProjectionViewMatrix());
				s_FX->SetMatrix(s_eWVP, &rot);
				oapi::FVECTOR4 baseCol1(0.2f, 0.2f, 0.0f, 1.0f);
				FVECTOR4 vColor1 = ColorAdjusted(baseCol1);
				s_FX->SetVector(s_eColor, &vColor1);
				RenderGrid(s_FX, false);
				oapi::FVECTOR4 baseCol2(0.5f, 0.5f, 0.0f, 1.0f);
				FVECTOR4 vColor2 = ColorAdjusted(baseCol2);
				s_FX->SetVector(s_eColor, &vColor2);
				RenderGreatCircle(s_FX);
				double dphi = ElevationScaleRotation(R);
				RenderGridLabels(s_FX, 0, baseCol2, R, dphi);
			}
		}

		// render equator of target celestial body
		if (renderFlag & PLN_EQU) {
			OBJHANDLE hRef = oapiCameraProxyGbody();
			if (hRef) {
				MATRIX3 R;
				oapiGetRotationMatrix(hRef, &R);
				FMATRIX4 iR(
					(float)R.m11, (float)R.m21, (float)R.m31, 0.0f,
					(float)R.m12, (float)R.m22, (float)R.m32, 0.0f,
					(float)R.m13, (float)R.m23, (float)R.m33, 0.0f,
					0.0f,         0.0f,         0.0f,         1.0f
				);
				FMATRIX4 rot;
				VMAT_MatrixMultiply(&rot, &iR, m_scene->GetProjectionViewMatrix());
				s_FX->SetMatrix(s_eWVP, &rot);
				FVECTOR4 baseCol(0.0f, 0.6f, 0.0f, 1.0f);
				FVECTOR4 vColor = ColorAdjusted(baseCol);
				s_FX->SetVector(s_eColor, &vColor);
				RenderGreatCircle(s_FX);
			}
		}

		// render constellation boundaries ----------------------------------------
		if (renderFlag & PLN_CNSTBND) { // for now, hijack the constellation line flag
			s_FX->SetMatrix(s_eWVP, m_scene->GetProjectionViewMatrix());
			RenderConstellationBoundaries(s_FX);
		}

		// render constellation lines ---------------------------------------------
		if (renderFlag & PLN_CONST) {
			s_FX->SetMatrix(s_eWVP, m_scene->GetProjectionViewMatrix());
			RenderConstellationLines(s_FX);
		}
	}

	// render stars
	s_FX->SetTechnique(s_eStar);
	s_FX->SetMatrix(s_eWVP, m_scene->GetProjectionViewMatrix());
	RenderStars(s_FX);

	// render markers and labels
	if (renderFlag & PLN_ENABLE) {

		// Sketchpad for planetarium mode labels and markers
		VulkanPad* pSketch = m_scene->GetPooledSketchpad(0);

		if (pSketch) {

			// constellation labels --------------------------------------------------
			if (renderFlag & PLN_CNSTLABEL) {
				RenderConstellationLabels((oapi::Sketchpad**)&pSketch, renderFlag & PLN_CNSTLONG);
			}
			// celestial marker (stars) names ----------------------------------------
			if (renderFlag & PLN_CCMARK) {
				RenderCelestialMarkers((oapi::Sketchpad**)&pSketch);
			}
			pSketch->EndDrawing(); //SKETCHPAD_LABELS
		}
	}

}

// ==============================================================

void VulkanCelestialSphere::RenderStars(VulkanEffectFile *FX)
{
	_TRACE;

	if (!m_nsVtx) return; // nothing to do

	// render in chunks, because some graphics cards have a limit in the
	// vertex list size -- see MAX_STAR_CHUNK in the header.
	UINT i, j, numPasses = 0;

	int bgidx = min(255, (int)(GetSkyBrightness() * 256.0));
	int ns = m_starCutoffIdx[bgidx];

	VulkanDevice *pDev = FX->GetDevice();
	if (!pDev->IsRecording()) return;
	VkCommandBuffer cmd = pDev->GetCommandBuffer();

	FX->SetVertexDecl(pPosColorDecl);
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_POINT_LIST);
	FX->Begin(&numPasses, 0);		// was D3DXFX_DONOTSAVESTATE; nothing is saved
	FX->BeginPass(0);
	// UINT against int on Windows too; ns indexes a 256-entry table and is
	// never negative.
	for (i = j = 0; i < (UINT)ns; i += maxNumVertices, j++) {
		if (j >= m_sVtx.size() || !m_sVtx[j]) break;
		VkBuffer vb = m_sVtx[j]->Buffer();
		VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
		// For a point list the primitive count is the vertex count.
		vkCmdDraw(cmd, min((UINT)ns - i, maxNumVertices), 1, 0, 0);
	}
	FX->EndPass();
	FX->End();
}

// ==============================================================

void VulkanCelestialSphere::RenderConstellationLines(VulkanEffectFile *FX)
{
	const FVECTOR4 baseCol(0.5f, 0.3f, 0.2f, 1.0f);
	FVECTOR4 vColor = ColorAdjusted(baseCol);
	s_FX->SetVector(s_eColor, &vColor);

	_TRACE;
	UINT numPasses = 0;

	VulkanDevice *pDev = FX->GetDevice();
	if (!pDev->IsRecording() || !m_clVtx) return;
	VkCommandBuffer cmd = pDev->GetCommandBuffer();

	FX->SetVertexDecl(pPositionDecl);
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
	FX->Begin(&numPasses, 0);
	FX->BeginPass(0);
	VkBuffer vb = m_clVtx->Buffer();
	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
	// Was DrawPrimitive(D3DPT_LINELIST, 0, m_nclVtx), whose last argument is a
	// primitive count while m_nclVtx counts vertices -- so D3D9 read 2x the
	// buffer. vkCmdDraw takes vertices, so this is now what it always meant.
	vkCmdDraw(cmd, m_nclVtx, 1, 0, 0);
	FX->EndPass();
	FX->End();
}

// ==============================================================

void VulkanCelestialSphere::RenderConstellationBoundaries(VulkanEffectFile *FX)
{
	const FVECTOR4 baseCol(0.25f, 0.2f, 0.15f, 1.0f);
	FVECTOR4 vColor = ColorAdjusted(baseCol);
	s_FX->SetVector(s_eColor, &vColor);

	_TRACE;
	UINT numPasses = 0;

	VulkanDevice *pDev = FX->GetDevice();
	if (!pDev->IsRecording() || !m_cbVtx) return;
	VkCommandBuffer cmd = pDev->GetCommandBuffer();

	FX->SetVertexDecl(pPositionDecl);
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
	FX->Begin(&numPasses, 0);
	FX->BeginPass(0);
	VkBuffer vb = m_cbVtx->Buffer();
	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
	// Same primitive-vs-vertex count as RenderConstellationLines above.
	vkCmdDraw(cmd, m_ncbVtx, 1, 0, 0);
	FX->EndPass();
	FX->End();
}

// ==============================================================

void VulkanCelestialSphere::RenderGreatCircle(VulkanEffectFile *FX)
{
	_TRACE;
	UINT numPasses = 0;

	VulkanDevice *pDev = FX->GetDevice();
	if (!pDev->IsRecording() || !m_grdLngVtx) return;
	VkCommandBuffer cmd = pDev->GetCommandBuffer();

	FX->SetVertexDecl(pPositionDecl);
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_LINE_STRIP);
	FX->Begin(&numPasses, 0);
	FX->BeginPass(0);
	VkBuffer vb = m_grdLngVtx->Buffer();
	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
	// DrawPrimitive(D3DPT_LINESTRIP, 5*(NSEG+1), NSEG): NSEG segments is
	// NSEG+1 vertices; the start vertex becomes firstVertex unchanged.
	vkCmdDraw(cmd, NSEG + 1, 1, 5*(NSEG+1), 0);
	FX->EndPass();
	FX->End();
	
}

// ==============================================================

void VulkanCelestialSphere::RenderGrid(VulkanEffectFile *FX, bool eqline)
{
	_TRACE;
	int i;
	UINT numPasses = 0;

	VulkanDevice *pDev = FX->GetDevice();
	if (!pDev->IsRecording() || !m_grdLngVtx || !m_grdLatVtx) return;
	VkCommandBuffer cmd = pDev->GetCommandBuffer();

	FX->SetVertexDecl(pPositionDecl);
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_LINE_STRIP);
	VkDeviceSize offset = 0;
	VkBuffer vb = m_grdLngVtx->Buffer();
	vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
	FX->Begin(&numPasses, 0);
	FX->BeginPass(0);
	for (i = 0; i <= 10; i++)
		if (eqline || i != 5)
			vkCmdDraw(cmd, NSEG + 1, 1, i*(NSEG+1), 0);
	vb = m_grdLatVtx->Buffer();
	vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
	for (i = 0; i < 12; i++)
		vkCmdDraw(cmd, NSEG + 1, 1, i*(NSEG+1), 0);
	FX->EndPass();
	FX->End();
}

// ==============================================================

void VulkanCelestialSphere::RenderGridLabels(VulkanEffectFile *FX, int az_idx, const oapi::FVECTOR4& baseCol, const MATRIX3& R, double dphi)
{
	if (!m_GridLabelTex) return;
	// int against size_t; az_idx is 0, 1 or 2 at every call site.
	if (az_idx >= (int)m_azGridLabelVtx.size()) return;
	if (!m_azGridLabelVtx[az_idx])
		AllocGridLabels();
	if (!m_azGridLabelVtx[az_idx] || !m_GridLabelIdx || !m_elGridLabelVtx) return;

	VulkanDevice *pDev = FX->GetDevice();
	if (!pDev->IsRecording()) return;
	VkCommandBuffer cmd = pDev->GetCommandBuffer();

	UINT numPasses = 0;
	VkDeviceSize offset = 0;
	VkBuffer vb = m_azGridLabelVtx[az_idx]->Buffer();

	FX->SetVertexDecl(pPosTexDecl);
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
	vkCmdBindIndexBuffer(cmd, m_GridLabelIdx->Buffer(), 0, VK_INDEX_TYPE_UINT16);
	FX->SetTechnique(s_eLabel);
	// Was SetTexture(0, ...) after BeginPass. It moves before: the bind goes
	// through the effect parameter rather than a numbered device slot (see
	// s_eTex0 in CelSphere.h), and BeginPass is where the descriptor set is
	// written, so a texture set after it would not reach this draw.
	FX->SetTexture(s_eTex0, m_GridLabelTex->GetTexture());
	FX->Begin(&numPasses, 0);
	FX->BeginPass(0);
	// DrawIndexedPrimitive's last argument was 24*2 triangles = 24*2*3 indices.
	vkCmdDrawIndexed(cmd, 24 * 2 * 3, 1, 0, 0, 0);
	FX->EndPass();
	FX->End();

	FMATRIX4 T0, T1;
	if (dphi) {
		// GetMatrix reads the parameter back out of the effect's own staged
		// block, as D3DX did out of its copy.
		FX->GetMatrix(s_eWVP, &T0);
		double cosp = cos(dphi), sinp = sin(dphi);
		FMATRIX4 R2(
			(float)(cosp * R.m11 + sinp * R.m31),  (float)(cosp * R.m12 + sinp * R.m32),  (float)(cosp * R.m13 + sinp * R.m33),  0.0f,
			(float)R.m21,                          (float)R.m22,                          (float)R.m23,                          0.0f,
			(float)(-sinp * R.m11 + cosp * R.m31), (float)(-sinp * R.m12 + cosp * R.m32), (float)(-sinp * R.m13 + cosp * R.m33), 0.0f,
			0.0f,                                  0.0f,                                  0.0f,                                  1.0f
		);
		VMAT_MatrixMultiply(&T1, &R2, m_scene->GetProjectionViewMatrix());
		FX->SetMatrix(s_eWVP, &T1);
	}

	vb = m_elGridLabelVtx->Buffer();
	vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
	// The second SetTexture(0, ...), moved for the same reason.
	FX->SetTexture(s_eTex0, m_GridLabelTex->GetTexture());
	FX->Begin(&numPasses, 0);
	FX->BeginPass(0);
	// 11*2 triangles -> 11*2*3 indices.
	vkCmdDrawIndexed(cmd, 11 * 2 * 3, 1, 0, 0, 0);
	FX->EndPass();
	FX->End();

	FX->SetTechnique(s_eLine); // Restore default tech

	if (dphi)
		FX->SetMatrix(s_eWVP, &T0);
}

// ==============================================================

void VulkanCelestialSphere::RenderBkgImage(VulkanDevice *dev)
{
	m_bkgImgMgr->Render(dev, 8, GetSkyBrightness());
	// Was dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW), restoring the cull
	// mode after CSphereManager's draw. Cull mode is immutable pipeline state
	// in Vulkan: nothing was changed globally, so nothing needs restoring.
}

// ==============================================================

bool VulkanCelestialSphere::EclDir2WindowPos(const VECTOR3& dir, int& x, int& y) const
{
	// Was D3DXVec3TransformCoord: transform and divide by w. DrawAPI.h declares
	// exactly that as TransformCoord, in the same row-vector convention.
	FVECTOR3 fdir((float)dir.x, (float)dir.y, (float)dir.z);
	FVECTOR3 homog = TransformCoord(fdir, *m_scene->GetProjectionViewMatrix());

	if (homog.x >= -1.0f && homog.x <= 1.0f &&
		homog.y >= -1.0f && homog.y <= 1.0f &&
		homog.z < 1.0f) {

		if (std::hypot(homog.x, homog.y) < 1e-6) {
			x = m_scene->ViewW() / 2;
			y = m_scene->ViewH() / 2;
		}
		else {
			x = (int)(m_scene->ViewW() * 0.5 * (1.0 + homog.x));
			y = (int)(m_scene->ViewH() * 0.5 * (1.0 - homog.y));
		}
		return true;
	}
	else {
		return false;
	}
}

void VulkanCelestialSphere::VulkanTechInit(VulkanEffectFile *fx)
{
	s_FX = fx;
	s_eStar = fx->GetTechniqueByName("StarTech");
	s_eLine = fx->GetTechniqueByName("LineTech");
	s_eLabel = fx->GetTechniqueByName("LabelTech");
	// GetParameterByName's first argument was D3DX's parent parameter, always 0
	// (file scope) here. There is only file scope now, so it is gone.
	s_eColor = fx->GetParameterByName("gColor");
	s_eWVP = fx->GetParameterByName("gWVP");
	// New lookup, for a parameter the Windows code went around rather than
	// used. See s_eTex0 in CelSphere.h.
	s_eTex0 = fx->GetParameterByName("gTex0");
}

VulkanEffectFile* VulkanCelestialSphere::s_FX = 0;
TECHHANDLE VulkanCelestialSphere::s_eStar = 0;
TECHHANDLE VulkanCelestialSphere::s_eLine = 0;
TECHHANDLE VulkanCelestialSphere::s_eLabel = 0;
HANDLE VulkanCelestialSphere::s_eColor = 0;
HANDLE VulkanCelestialSphere::s_eWVP = 0;
HANDLE VulkanCelestialSphere::s_eTex0 = 0;
