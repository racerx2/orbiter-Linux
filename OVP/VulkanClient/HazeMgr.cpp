// ============================================================================
// HazeMgr.cpp
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2007-2026 Martin Schweiger
//				 2011 - 2016 Jarmo Nikkanen
// ============================================================================

// ============================================================================
// class HazeManager (implementation)
//
// Planetary atmospheric haze rendering
// Implemented as transparent overlay on planetary disc
// ============================================================================
//
// Two changes here propagated into the layers underneath. HazeManager::Render
// draws the inner haze ring with the winding reversed (D3DRS_CULLMODE =
// D3DCULL_CW), which VulkanEffectFile::PassOverride could not express while its
// cull field was a bool -- hence the three-valued cullMode. And ShaderClass
// gained SetTopology, because HazeManager2's draws are triangle strips where
// ShaderClass built every pipeline as a triangle list: topology is baked into a
// VkPipeline, so it has to be known before the bind.
//
// HazeManager2's D3DCULL_NONE / D3DCULL_CCW pairs have no counterpart and need
// none. ShaderClass builds every pipeline with VK_CULL_MODE_NONE, so the "set"
// is already true and the "restore" has nothing global to put back -- the next
// pipeline bind carries its own cull mode whatever this one did.
// ============================================================================

#include "HazeMgr.h"
#include "VPlanet.h"
#include "VulkanSurface.h"
#include "VulkanUtil.h"
#include "VectorHelpers.h"
#include "VulkanEffect.h"
#include "VulkanConfig.h"
#include "Scene.h"

using namespace oapi;

HazeManager::HazeManager (const VulkanClient *gclient, const vPlanet *vplanet) : VulkanEffect()
{
	vp = vplanet;
	obj = vp->Object();
	rad = oapiGetSize (obj);
	const ATMCONST *atmc = oapiGetPlanetAtmConstants (obj);
	if (atmc) {
		basecol = *(VECTOR3*)oapiGetObjectParam (obj, OBJPRM_PLANET_HAZECOLOUR);
		hralt = (float)(atmc->horizonalt / rad);
		dens0 = (float)(min (1.0, atmc->horizonalt/64e3 * *(double*)oapiGetObjectParam(obj, OBJPRM_PLANET_HAZEDENSITY)));
	} else {
		basecol = _V(1,1,1);
		hralt = 0.01f;
		dens0 = 1.0f;
	}
	if (*(bool*)oapiGetObjectParam (obj, OBJPRM_PLANET_HASCLOUDS)) {
		hshift = *(double*)oapiGetObjectParam (obj, OBJPRM_PLANET_HAZESHIFT);
		cloudalt = *(double*)oapiGetObjectParam (obj, OBJPRM_PLANET_CLOUDALT);
	} else
		hshift = 0;
	hscale = (float)(1.0 - *(double*)oapiGetObjectParam (obj, OBJPRM_PLANET_HAZEEXTENT));
}

// -----------------------------------------------------------------------

void HazeManager::GlobalInit(VulkanClient *gclient)
{
	int i;
	for (i = 0; i < HORIZON_NSEG; i++) Idx[i*2] = WORD(i*2+1), Idx[i*2+1] = WORD(i*2);
	Idx[i*2] = 1, Idx[i*2+1] = 0;

	for (i = 0; i < HORIZON_NSEG; i++) {
		Vtx[i*2].tu = Vtx[i*2+1].tu = (float)(i%2);
		Vtx[i*2].tv = 1.0f;
		Vtx[i*2+1].tv = 0.0f;
		double phi = (double)i/(double)HORIZON_NSEG * PI*2.0;
		CosP[i] = (float)cos(phi), SinP[i] = (float)sin(phi);
	}
	horizon = gclient->clbkLoadTexture("Horizon.dds");
}

// -----------------------------------------------------------------------

void HazeManager::GlobalExit()
{
	DELETE_SURFACE(horizon);
}

// -----------------------------------------------------------------------

void HazeManager::Render(VulkanDevice *pDevice, FMATRIX4 &wmat, bool dual)
{
	FMATRIX4 imat, transm;

	VECTOR3 psun;
	int i, j;
	double phi, csun, alpha, colofs;
	float cosp, sinp, cost, sint, h1, h2, r1, r2, intr, intg, intb;

	VMAT_MatrixInvert (&imat, &wmat);
	VECTOR3 rpos = {imat.m41, imat.m42, imat.m43};   // camera in local coords (planet radius = 1)
	double cdist = length (rpos);

	alpha = dens0 * min (1.0, (cdist-1.0)*200.0);
	if (!dual) alpha = 1.0-alpha;
	if (alpha <= 0.0) return;  // nothing to do

	// Problem: the top part of horizon haze is rendered twice
	if (dual && cdist<1.001) return;	// Enabled 04.06.2011

	VECTOR3 cpos = {0,cdist,0};
	double id = 1.0 / max (cdist, 1.001);
	double visrad = acos (id);             // aperture of visibility sector
	double sinv = sin(visrad);

	h1 = (float)id;
	h2 = h1 + (float)(hralt*id);
	r1 = (float)sinv, r2 = (1.0f+hralt)*r1;

	if (!dual) { // pull lower horizon edge below surface to avoid problems with elevations < 0
		h1 *= (float)(vp->prm.horizon_minrad);
		r1 *= (float)(vp->prm.horizon_minrad);
	}

	if (hshift) {
		if (cdist-1.0 > cloudalt/rad) {
			float dr = (float)(hshift*sinv);
			float dh = (float)(hshift*id);
			h1 += dh, h2 += dh;
			r1 += dr, r2 += dr;
		}
	}

	float dens = (float)max (1.0, 1.4 - 0.3/hralt*(cdist-1.0)); // saturate haze colour at low altitudes
	if (dual) dens *= (float)(0.5 + 0.5/cdist);                 // scale down intensity at large distances

	normalise (rpos);
	cost = (float)rpos.y, sint = (float)sqrt (1.0-cost*cost);
	phi = atan2 (rpos.z, rpos.x), cosp = (float)cos(phi), sinp = (float)sin(phi);

	FMATRIX4 rmat = FMATRIX4(cost*cosp, -sint, cost*sinp, 0,
		              sint*cosp,  cost, sint*sinp, 0,
					  -sinp,      0,    cosp,      0,
					  0,          0,    0,         1);

	VMAT_MatrixMultiply(&transm, &rmat, &wmat);

	MATRIX3 rrmat = {cost*cosp, -sint, cost*sinp,
		             sint*cosp,  cost, sint*sinp,
					 -sinp,      0,    cosp     };
	MATRIX3 grot;
	VECTOR3 gpos;
	oapiGetRotationMatrix (obj, &grot);
	oapiGetGlobalPos (obj, &gpos);
	psun = tmul (grot, -gpos); // sun in planet coords
	psun = mul (rrmat, psun);  // sun in camera-relative horizon coords
	VECTOR3 cs = psun-cpos; normalise(cs); // camera->sun
	normalise (psun);
	// float psunx = (float)psun.x, psuny = (float)psun.y, psunz = (float)psun.z;

	colofs = (dual ? 0.4 : 0.3);

	for (i = j = 0; i < HORIZON_NSEG; i++) {
		VECTOR3 hp = {Vtx[j].x = r1*CosP[i], Vtx[j].y = h1, Vtx[j].z = r1*SinP[i]};
		csun = dotp (hp, psun);
		VECTOR3 cp = hp-cpos; normalise(cp);
		double colsh = 0.5*(dotp (cp,cs) + 1.0);

		// compose a colourful sunset
		double maxred   = colofs-0.18*colsh,  minred   = maxred-0.4;
		double maxgreen = colofs-0.1*colsh,  mingreen = maxgreen-0.4;
		double maxblue  = colofs/*+0.0*colsh*/,  minblue  = maxblue-0.4;
		if      (csun > maxred) intr = 1.0f;
		else if (csun < minred) intr = 0.0f;
		else                    intr = (float)((csun-minred)*2.5);
		if      (csun > maxgreen) intg = 1.0f;
		else if (csun < mingreen) intg = 0.0f;
		else                      intg = (float)((csun-mingreen)*2.5);
		if      (csun > maxblue) intb = 1.0f;
		else if (csun < minblue) intb = 0.0f;
		else                     intb = (float)((csun-minblue)*2.5);

		// Was a D3DXCOLOR assigned to a DWORD, invoking D3DXCOLOR's conversion
		// operator: clamp each channel and pack 0xAARRGGBB. dword_argb() is
		// that operator, under a name that says which byte order it produces.
		FVECTOR4 col = FVECTOR4(intr*min(1.0f,dens*(float)basecol.x), intg*min(1.0f,dens*(float)basecol.y), intb*min(1.0f,dens*(float)basecol.z), (float)alpha);

		Vtx[j].dcol = col.dword_argb();
		j++;
		Vtx[j].x = r2*CosP[i];
		Vtx[j].y = h2;
		Vtx[j].z = r2*SinP[i];
		Vtx[j].dcol = col.dword_argb();
		j++;
	}
	
	FX->SetTechnique(eHazeTech);
	FX->SetMatrix(eW, &transm);
	FX->SetTexture(eTex0, SURFACE(horizon)->GetTexture());	

	// The topology joins SetVertexDecl because a D3DPT_TRIANGLESTRIP argument
	// to DrawIndexedPrimitiveUP is pipeline state here.
	FX->SetVertexDecl(pHazeVertexDecl);
	FX->SetTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);

	UINT numPasses = 0;
	FX->Begin(&numPasses, 0);		// was D3DXFX_DONOTSAVESTATE
	FX->BeginPass(0);
	
	// DrawIndexedPrimitiveUP's fourth argument is a primitive count, and a
	// strip of N triangles has N+2 indices -- which is exactly nIdx,
	// HORIZON_NSEG*2+2. DrawUP takes the index count.
	FX->DrawUP(Vtx, 2*HORIZON_NSEG, sizeof(HVERTEX), Idx, nIdx);
	
	if (dual) {

		h2 = h1;
		r2 = hscale * r1*r1; 

		for (i = j = 0; i < HORIZON_NSEG; i++) {
			j++;
			Vtx[j].x = r2*CosP[i];
			Vtx[j].y = h2;
			Vtx[j].z = r2*SinP[i];
			j++;
		}

		// Was SetRenderState(D3DRS_CULLMODE, D3DCULL_CW), the draw, then
		// D3DCULL_CCW again. D3DCULL_CW and D3DCULL_CCW are the same Vulkan
		// cull mode with opposite front-face windings, so "not none" is not
		// enough information -- which is why PassOverride's cull field is
		// three-valued. The restore is just the end of the override's scope.
		VulkanEffectFile::PassOverride ovr;
		ovr.cullMode = VulkanEffectFile::PassOverride::CULL_CW;

		FX->EndPass();
		if (FX->BeginPassEx(0, &ovr))
			FX->DrawUP(Vtx, 2*HORIZON_NSEG, sizeof(HVERTEX), Idx, nIdx);
	}

	FX->EndPass();
	FX->End();	
}

// ============================================================================
// static member initialisation

WORD     HazeManager::Idx[HORIZON_NSEG*2+2];
struct   HazeManager::HVERTEX HazeManager::Vtx[HORIZON_NSEG*2];
DWORD    HazeManager::nIdx = HORIZON_NSEG*2+2;
float HazeManager::CosP[HORIZON_NSEG];
float HazeManager::SinP[HORIZON_NSEG];

SURFHANDLE HazeManager::horizon = 0;












// ============================================================================
// class HazeManager2 (implementation)
//
// Planetary atmospheric haze rendering with scattering technique
// ============================================================================

HazeManager2::HazeManager2(vPlanet *vplanet) 
{
	vp = vplanet;
	obj = vp->Object();
	rad = oapiGetSize(obj);
}

// -----------------------------------------------------------------------

HazeManager2::~HazeManager2()
{
	
}

// -----------------------------------------------------------------------

void HazeManager2::GlobalInit(VulkanClient *gclient)
{
	pDev = gclient->GetDevice();
	pNoise = gclient->GetNoiseTex();
	for (int i=0;i<6;i++) pSkyVB[i]=NULL;
	CreateRingBuffers();
	CreateSkydomeBuffers(0);
	CreateSkydomeBuffers(1);
	CreateSkydomeBuffers(2);
	CreateSkydomeBuffers(3);
	CreateSkydomeBuffers(4);
	CreateSkydomeBuffers(5);

	pDome = new ShaderClass(pDev, "Modules/VulkanClient/NewPlanet.glsl", "HorizonVS", "HorizonPS", "Dome", NULL);
	pRing = new ShaderClass(pDev, "Modules/VulkanClient/NewPlanet.glsl", "HorizonVS", "HorizonRingPS", "Ring", NULL);
}

// -----------------------------------------------------------------------

void HazeManager2::GlobalExit()
{
	// Were SAFE_RELEASE calls on a COM refcount; a Vulkan buffer is destroyed
	// by the device that made it.
	for (int i=0;i<6;i++) { if (pSkyVB[i]) { pDev->DestroyBuffer(pSkyVB[i]); pSkyVB[i] = NULL; } }
	if (pRingVB) { pDev->DestroyBuffer(pRingVB); pRingVB = NULL; }
	SAFE_DELETE(pDome);
	SAFE_DELETE(pRing);
}

// -----------------------------------------------------------------------

void HazeManager2::Render(FMATRIX4 &wmat, float horizontal_aperture_deg)
{
	Scene* scn = vp->GetScene();
	VECTOR3 cdir = scn->GetCameraGDir();
	double calt = vp->CamDist() - rad;	// Camera altitude	
	double halt = vp->GetHorizonAlt();
	double melv = vp->GetMinElevation();

	melv -= 1000.0;

	if (calt>halt)	RenderRing(vp->PosFromCamera(), cdir, rad+melv, halt);
	else			RenderSky(vp->PosFromCamera(), cdir, rad+melv, horizontal_aperture_deg);
}

// -----------------------------------------------------------------------

void HazeManager2::RenderSky(VECTOR3 cpos, VECTOR3 cdir, double rad, double apr)
{
	cpos = -cpos;

	double cr = length(cpos); if (cr<(rad+100.0)) cr=rad+100.0;
	double hd = sqrt(cr*cr - rad*rad) * 10.0;
	double al = asin(rad/cr);

	VECTOR3 ur = unit(cpos);
	VECTOR3 ux = unit(crossp(cdir, ur));
	VECTOR3 uy = unit(crossp(ur, ux));

	// _D3DXVECTOR3(v) built a D3DXVECTOR3 from a VECTOR3; FVEC(v) is that.
	FMATRIX4 mWL, mL;
	VMAT_Identity(&mWL);
	VMAT_FromAxisT(&mWL, ptr(FVEC(ux)), ptr(FVEC(ur)), ptr(FVEC(uy)));

	double a = 15.0*RAD;
	double b = (PI-asin(rad/cr))/6.0;
	
	FVECTOR3 vTileCenter = FVECTOR3(float(sin(15.0*RAD)), 1.0f, float(1.0+cos(15.0*RAD))) * 0.5f;
	// D3DXMatrixRotationAxis(&mL, axis, angle) -> VMAT_RotationFromAxis,
	// which is the client's own and takes its arguments the other way round.
	VMAT_RotationFromAxis(FVEC(ur), float(-a*0.5), &mL);
	VMAT_MatrixMultiply(&mWL, &mWL, &mL);
	VMAT_RotationFromAxis(FVEC(ur), float(-a), &mL);

	//vp->GetScatterConst()->mVP = vp->GetScene()->PushCameraFrustumLimits(hd * 0.1, hd * 5.0);

	// Setup builds the pipeline, so the topology has to be set before that
	// call rather than at the draw.
	pDome->SetTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
	pDome->Setup(pPositionDecl, false, 2);
	pDome->ClearTextures();

	pDome->SetTexture("tSkyRayColor", vp->GetScatterTable(RAY_COLOR), IPF_LINEAR | IPF_CLAMP);
	pDome->SetTexture("tSkyMieColor", vp->GetScatterTable(MIE_COLOR), IPF_LINEAR | IPF_CLAMP);
	pDome->SetTexture("tGlare", vp->GetScene()->GetSunGlareAtm(), IPF_LINEAR | IPF_CLAMP);
	pDome->SetTexture("tNoise", pNoise, IPF_POINT | IPF_WRAP);
	pDome->SetPSConstants("Const", vp->GetScatterConst(), sizeof(ConstParams));
	pDome->SetVSConstants("Const", vp->GetScatterConst(), sizeof(ConstParams));
	pDome->UpdateTextures();

	// SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE) stood here, and the matching
	// CCW restore after the loop. See the file header.

	for (int i=0;i<24;i++) {
		double x = al;
		VMAT_MatrixMultiply(&mWL, &mWL, &mL);
		for (int j=0;j<6;j++) {
			float r1 =  float(sin(x));	 float h1 = -float(cos(x));
			float r2 =  float(sin(x+b)); float h2 = -float(cos(x+b)); 
			// D3DXVECTOR3 * D3DXVECTOR3 was a component-wise product, not a
			// dot or a cross. FVECTOR3 has no such operator, so it is written
			// out per component.
			const float s = (r1+r2)*0.5f;
			FVECTOR3 vCnt(vTileCenter.x * s, (h1+h2)*0.5f, vTileCenter.z * s);
			vCnt = TransformCoord(vCnt, mWL);
			if (vp->GetScene()->IsVisibleInCamera(&vCnt, float(sin(a*0.5)*1.5))) RenderSkySegment(mWL, hd, x, x+b, j);	
			x+=b;
		}
	}

	//vp->GetScatterConst()->mVP = vp->GetScene()->PopCameraFrustumLimits();
}

// -----------------------------------------------------------------------

void HazeManager2::RenderSkySegment(FMATRIX4 &wmat, double rad, double dmin, double dmax, int index)
{
	float r1 =  float(rad * sin(dmin));
	float h1 = -float(rad * cos(dmin));
	float r2 =  float(rad * sin(dmax));
	float h2 = -float(rad * cos(dmax)); 

	ShaderParams sprm;
	// memcpy_s's destination size is its second argument; with both sizes equal
	// the check can never fire, so plain memcpy is the same copy.
	memcpy(&sprm.mWorld, &wmat, sizeof(sprm.mWorld));
	sprm.vTexOff = FVECTOR4(r1, r2, h1, h2);
	
	int xres = xreslvl[index];
	int yres = yreslvl[index];

	UINT prims = xres * yres * 2 - 2;

	pDome->SetVSConstants("Prm", &sprm, sizeof(ShaderParams));

	// This call has no line in the reference, and the draw is wrong without it.
	// SetVSConstants on Windows wrote through ID3DXConstantTable into the
	// device's constant registers, so the next DrawPrimitive saw the new value.
	// Here Set*Constants only accumulates into a CPU-side block, and
	// UpdateTextures() is what copies it into this frame's uniform arena and
	// binds the descriptor set.
	//
	// The cost was the whole daytime sky: all 144 sky-dome segments drew with
	// whatever `Prm` had last been committed -- the previous frame's final
	// segment, or zero -- so HorizonVS transformed every vertex by the same
	// stale matrix and the dome collapsed. What reached the screen was the
	// shader's noise term alone: a measured grey gradient of 1..4/255 where the
	// sky should be. RenderRing, the same class's other draw, already did this
	// correctly.
	pDome->UpdateTextures();

	// SetStreamSource + DrawPrimitive(TRIANGLESTRIP, 0, prims). A strip of
	// `prims` triangles has prims+2 vertices, and vkCmdDraw takes vertices.
	if (!pDev->IsRecording() || !pSkyVB[index]) return;
	VkCommandBuffer cmd = pDev->GetCommandBuffer();
	VkBuffer vb = pSkyVB[index]->Buffer();
	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
	vkCmdDraw(cmd, prims + 2, 1, 0, 0);
}


// -----------------------------------------------------------------------

void HazeManager2::RenderRing(VECTOR3 cpos, VECTOR3 cdir, double rad, double hralt)
{
	cpos = -cpos;
	double cr = length(cpos);
	double hd = sqrt(cr*cr - rad*rad);
	double al = asin(rad/cr);
	double mx = hralt * 4.0;

	double r1 =  hd * sin(al);
	double h1 = -hd * cos(al);
	double qw = (hralt + (cr-rad)) * 0.8;
	if (qw>mx) qw=mx;
	
	double r2 = r1 + qw * cos(al);
	double h2 = h1 + qw * sin(al); 

	vp->GetScatterConst()->mVP = vp->GetScene()->PushCameraFrustumLimits(hd * 0.1, hd * 5.0);
	
	VECTOR3 ur = unit(cpos);
	VECTOR3 ux = unit(crossp(cdir, ur));
	VECTOR3 uy = unit(crossp(ur, ux));

	FMATRIX4 mW;
	VMAT_Identity(&mW);
	VMAT_FromAxisT(&mW, ptr(FVEC(ux)), ptr(FVEC(ur)), ptr(FVEC(uy)));

	ShaderParams sprm;
	pRing->SetTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
	pRing->Setup(pPositionDecl, false, 2);
	pRing->ClearTextures();
	vp->InitEclipse(pRing);

	memcpy(&sprm, vp->GetTerrainParams(), sizeof(ShaderParams));
	memcpy(&sprm.mWorld, &mW, sizeof(sprm.mWorld));

	sprm.vTexOff = FVECTOR4(r1, r2, h1, h2);
	sprm.fAlpha = float(qw);

	pRing->SetTexture("tSkyRayColor", vp->GetScatterTable(RAY_COLOR), IPF_LINEAR | IPF_CLAMP);
	pRing->SetTexture("tSkyMieColor", vp->GetScatterTable(MIE_COLOR), IPF_LINEAR | IPF_CLAMP);
	pRing->SetPSConstants("Const", vp->GetScatterConst(), sizeof(ConstParams));
	pRing->SetVSConstants("Const", vp->GetScatterConst(), sizeof(ConstParams));
	pRing->SetVSConstants("Prm", &sprm, sizeof(ShaderParams));
	pRing->SetPSConstants("Prm", &sprm, sizeof(ShaderParams));
	pRing->SetPSConstants("Flow", vp->GetFlowControl(), sizeof(FlowControlPS));
	pRing->UpdateTextures();

	UINT nPrims = HORIZON2_NSEG * HORIZON2_NRING * 2 - 2;

	// The two SetRenderState(D3DRS_CULLMODE, ...) around this draw are gone:
	// cull is immutable pipeline state here, carried on the manager instead.
	if (pDev->IsRecording() && pRingVB) {
		VkCommandBuffer cmd = pDev->GetCommandBuffer();
		VkBuffer vb = pRingVB->Buffer();
		VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &offset);
		vkCmdDraw(cmd, nPrims + 2, 1, 0, 0);
	}

	// Pop previous frustum configuration, must initialize mVP
	vp->GetScatterConst()->mVP = vp->GetScene()->PopCameraFrustumLimits();
}


// -----------------------------------------------------------------------

void HazeManager2::CreateRingBuffers()
{
	int v = 0;
	int nvrt = HORIZON2_NSEG * 2 * HORIZON2_NRING + 2;

	FVECTOR3 *pVrt = new FVECTOR3[nvrt];
	FVECTOR3 *pBuf = NULL;

	float d = 1.0f/float(HORIZON2_NRING);
	double phi = 0.0;
	double dphi = PI2/double(HORIZON2_NSEG-1);
	float x = float(cos(phi));
	float z = float(sin(phi));
	float y = 0.0f;

	for (int k=0;k<HORIZON2_NRING;k++) {  // TODO: No need for grid anymore (jarmonik 01-Feb-2024)
		for (int i=0;i<HORIZON2_NSEG;i++) {
			pVrt[v++] = FVECTOR3(x, y, z);
			phi+=dphi;
			x = float(cos(phi));
			z = float(sin(phi));
			pVrt[v++] = FVECTOR3(x, y+d, z);
		}
		y+=d;
	}

	// Was CreateVertexBuffer(..., D3DPOOL_DEFAULT) plus Lock/memcpy/Unlock: one
	// host-visible buffer with the vertex usage flag, then Map/memcpy/Unmap.
	pRingVB = pDev->CreateBuffer(v*sizeof(FVECTOR3), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);

	if (pRingVB && (pBuf = (FVECTOR3 *)pRingVB->Map()) != NULL) {
		memcpy(pBuf, pVrt, v*sizeof(FVECTOR3));
		pRingVB->Unmap();
	}
	else LogErr("HazeManager2: ring vertex buffer allocation failed");

	delete []pVrt;
	pVrt = NULL;
}

// -----------------------------------------------------------------------
void HazeManager2::CreateSkydomeBuffers(int index)
{
	int k = 0;

	int xseg = xreslvl[index];
	int yseg = yreslvl[index];

	FVECTOR3 *pVrt = new FVECTOR3[xseg*yseg*2+2];
	FVECTOR3 *pBuf = NULL;

	double sa = 0.0, ca = 1.0;
	double db = 1.0/double(yseg);
	double dc = (1.0-cos(15.0*RAD))/double(xseg-1);
	double ds = sin(15.0*RAD)/double(xseg-1);
	double b  = 0.0;
	
	for (int s=0;s<yseg;s++) {
		for (int i=0;i<xseg;i++) {
			pVrt[k++]=FVECTOR3(float(sa), float(b),    float(ca));
			pVrt[k++]=FVECTOR3(float(sa), float(b+db), float(ca));
			sa += ds; ca -= dc;
		}
		ds = -ds; dc = -dc;	sa += ds; ca -= dc;	b += db;
	}

	pSkyVB[index] = pDev->CreateBuffer(k*sizeof(FVECTOR3), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);

	if (pSkyVB[index] && (pBuf = (FVECTOR3 *)pSkyVB[index]->Map()) != NULL) {
		memcpy(pBuf, pVrt, k*sizeof(FVECTOR3));
		pSkyVB[index]->Unmap();
	}
	else LogErr("HazeManager2: sky dome vertex buffer allocation failed");

	delete []pVrt;
	pVrt = NULL;
}

// -----------------------------------------------------------------------

ShaderClass* HazeManager2::pDome;
ShaderClass* HazeManager2::pRing;
VulkanDevice *HazeManager2::pDev;
VulkanTexture *HazeManager2::pNoise;
VulkanBuffer *HazeManager2::pSkyVB[6];
VulkanBuffer *HazeManager2::pRingVB = NULL;
int HazeManager2::xreslvl[6] = {9,6,5,4,3,2};
int HazeManager2::yreslvl[6] = {11,8,6,5,5,4};
