// ==============================================================
// RingMgr.cpp
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2007-2026 Martin Schweiger
//				 2011 - 2106 Jarmo Nikkanen (D3D9Client modification)  
// ==============================================================

// ==============================================================
// class RingManager (implementation)
// ==============================================================

#include "RingMgr.h"
#include "VulkanCatalog.h"
#include "VulkanSurface.h"

using namespace oapi;

void ReleaseTex(VulkanTexture *pTex);



RingManager::RingManager (const vPlanet *vplanet, double inner_rad, double outer_rad)
{
	vp = vplanet;
	irad = inner_rad;
	orad = outer_rad;
	rres = (DWORD)-1;
	tres = 0;
	ntex = 0;
	pTex = NULL;

	for (DWORD i = 0; i < MAXRINGRES; i++) {
		mesh[i] = 0;
		tex[i] = 0;
	}
}

RingManager::~RingManager ()
{
	DWORD i;
	for (i = 0; i < 3; i++)	if (mesh[i]) delete mesh[i];
	for (i = 0; i < ntex; i++) ReleaseTex(tex[i]);
	// Was pTex->Release().
	if (pTex) ReleaseTex(pTex);
}

void RingManager::GlobalInit(VulkanClient *gclient)
{
	gc = gclient;
}

void RingManager::SetMeshRes(DWORD res)
{
	if (res != rres) {
		rres = res;
		if (!mesh[res])	mesh[res] = CreateRing (irad, orad, 8+res*4);
		if (!ntex) ntex = LoadTextures();
		tres = min (rres, ntex-1);
	}
}


DWORD RingManager::LoadTextures ()
{
	char fname[128] = { '\0' };
	char temp[128];
	char path[MAX_PATH] = { '\0' };


	oapiGetObjectName (vp->Object(), fname, ARRAYSIZE(fname));

	// D3DCAPS9::MaxTextureWidth is VkPhysicalDeviceLimits::maxImageDimension2D,
	// one of the few D3DCAPS9 fields with an exact Vulkan counterpart.
	const VkPhysicalDeviceProperties *caps = gc->GetHardwareCaps();

	int size = max(min((int)caps->limits.maxImageDimension2D, 8192), 2048);

	sprintf_s(temp, ARRAYSIZE(temp), "%s_ring_%d.dds", fname, size);

	// Was one D3DXCreateTextureFromFileExA that opened the file, read the DDS
	// header, picked a format (D3DFMT_FROM_FILE), allocated the texture and
	// uploaded every mip. With no D3DX the two halves are separate: read the
	// bytes, then hand them to the client's own DDS decoder. D3DPOOL_DEFAULT
	// and the two D3DX_DEFAULT mip/filter arguments have no counterpart --
	// memory placement follows the usage flags, and nothing filters on load.
	if (gc->TexturePath(temp, path)) {
		FILE *f = NULL;
		if (fopen_s(&f, path, "rb") == 0 && f) {
			fseek(f, 0, SEEK_END);
			long bytes = ftell(f);
			fseek(f, 0, SEEK_SET);
			if (bytes > 0) {
				BYTE *data = new BYTE[bytes];
				if (fread(data, 1, bytes, f) == (size_t)bytes)
					pTex = NatCreateTextureFromDDSInMemory(data, (size_t)bytes);
				delete []data;
			}
			fclose(f);
		}
		if (pTex) LogAlw("High resolution ring texture loaded [%s]", path);
	}
	
	// Fallback for old method
	strcat_s(fname, ARRAYSIZE(fname), "_ring.tex");
	
	return LoadPlanetTextures(fname, tex, 0, MAXRINGRES);
}

bool RingManager::Render(VulkanDevice *dev, FMATRIX4 &mWorld, bool front)
{
	MATRIX3 grot;
	// D3DXVec3Length written out; D3DX has no Vulkan counterpart.
	FVECTOR3 q(mWorld.m11, mWorld.m21, mWorld.m31);
	float scale = sqrt(q.x*q.x + q.y*q.y + q.z*q.z);
	
	oapiGetRotationMatrix(vp->Object(), &grot);
	
	VECTOR3 gdir; oapiCameraGlobalDir(&gdir);

	VECTOR3 yaxis =  mul(grot, _V(0,1,0));
	VECTOR3 xaxis = unit(crossp(gdir, yaxis));
	VECTOR3 zaxis = unit(crossp(xaxis, yaxis));

	if (!front) {
		xaxis = -xaxis;
		zaxis = -zaxis;
	}

	FVECTOR3 x(float(xaxis.x), float(xaxis.y), float(xaxis.z)); 
	FVECTOR3 y(float(yaxis.x), float(yaxis.y), float(yaxis.z)); 
	FVECTOR3 z(float(zaxis.x), float(zaxis.y), float(zaxis.z)); 

	FMATRIX4 World = mWorld;

	x*=scale; y*=scale;	z*=scale;

	VMAT_FromAxisT(&World, &x, &y, &z);

	float rad = float(vp->GetSize());
	
	if (pTex) {
		mesh[rres]->RenderRings2(&World, pTex, float(irad)*rad, float(orad)*rad);
	}
	else mesh[rres]->RenderRings(&World, tex[tres]);
	return true;
}

// =======================================================================
// CreateRing
// Creates mesh for rendering planetary ring system. Creates a ring
// with nsect quadrilaterals. Smoothing the corners of the mesh is
// left to texture transparency. Nsect should be an even number.
// Disc is in xz-plane centered at origin facing up. Size is such that
// a ring of inner radius irad (>=1) and outer radius orad (>irad)
// can be rendered on it.

VulkanMesh *RingManager::CreateRing(double irad, double orad, int nsect)
{
	int i, j;

	MESHGROUPEX *grp = new MESHGROUPEX; 
	
	memset(grp,0,sizeof(MESHGROUPEX));
	
	int count = nsect/2 + 1;
	grp->nVtx = 2*count;
	grp->nIdx = 6*(count-1);
	grp->Idx = new WORD[grp->nIdx+12];
	grp->Vtx = new NTVERTEX[grp->nVtx+4];
	grp->TexIdx = 1;
	
	NTVERTEX *Vtx = grp->Vtx;
	WORD *Idx = grp->Idx;

	double alpha = PI/(double)nsect;
	float nrad = (float)(orad/cos(alpha)); // distance for outer nodes
	float ir = (float)irad;
	float fo = (float)(0.5*(1.0-orad/nrad));
	float fi = (float)(0.5*(1.0-irad/nrad));

	for (i = j = 0; i < count; i++) {
		double phi = i*2.0*alpha;
		float cosp = (float)cos(phi), sinp = (float)sin(phi);
		Vtx[i*2].x = nrad*cosp;  Vtx[i*2+1].x = ir*cosp;
		Vtx[i*2].z = nrad*sinp;  Vtx[i*2+1].z = ir*sinp;
		Vtx[i*2].y = Vtx[i*2+1].y = 0.0;
		Vtx[i*2].nx = Vtx[i*2+1].nx = Vtx[i*2].nz = Vtx[i*2+1].nz = 0.0;
		Vtx[i*2].ny = Vtx[i*2+1].ny = 1.0;

		if (!(i&1)) Vtx[i*2].tu = fo,  Vtx[i*2+1].tu = fi;  //fac;
		else        Vtx[i*2].tu = 1.0f-fo,  Vtx[i*2+1].tu = 1.0f-fi; //1.0f-fac;

		//Vtx[i*2].tv = 0.05f, Vtx[i*2+1].tv = 1.00f;
		Vtx[i*2].tv = 0.0f, Vtx[i*2+1].tv = 1.00f;

		if ((DWORD)j<=grp->nIdx-6) {
			Idx[j++] = WORD(i*2);
			Idx[j++] = WORD(i*2+1);
			Idx[j++] = WORD(i*2+2);
			Idx[j++] = WORD(i*2+3);
			Idx[j++] = WORD(i*2+2);
			Idx[j++] = WORD(i*2+1);
		}
	}

	MATERIAL mat = {{1,1,1,1},{0,0,0,1},{0,0,0,1},{0,0,0,0},20.0f};

	VulkanMesh *msh = new VulkanMesh(grp, &mat, NULL);

	// Was `delete grp->Idx; delete grp->Vtx;`. Both are allocated with new[]
	// above, and freeing an array with scalar delete is undefined behaviour --
	// for WORD and NTVERTEX it happens to work on the usual allocators, which
	// is why it was never noticed. The brackets are the whole fix.
	delete []grp->Idx;
	delete []grp->Vtx;
	delete grp;
	return msh;
}

oapi::VulkanClient *RingManager::gc = 0;
