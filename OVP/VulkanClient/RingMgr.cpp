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
//
// CONVERTED FROM OVP/D3D9Client/RingMgr.cpp, read end to end (199 lines).
//
// CreateRing is pure geometry and converts unchanged -- it fills an
// NTVERTEX/WORD pair and hands it to the mesh constructor, which is the same
// on both platforms. What changes:
//
//   D3DXCreateTextureFromFileExA. D3DX read the DDS header, chose a format
//   and uploaded every mip level in one call. There is no D3DX here and the
//   client reads DDS itself, so this becomes: read the file into memory, then
//   NatCreateTextureFromDDSInMemory (VulkanSurface.cpp). That yields a BARE
//   VulkanTexture*, which is what D3DXCreateTextureFromFileExA yielded and
//   what pTex has always been -- so the destructor's release is unchanged in
//   meaning.
//
//   D3DCAPS9::MaxTextureWidth -> VkPhysicalDeviceLimits::maxImageDimension2D.
//   Same question, the Vulkan spelling of the answer.
//
//   D3DXVec3Length, and the D3DXVECTOR3 locals it worked on. FVECTOR3 is the
//   same three floats; the length is written out because D3DX is a Direct3D
//   UTILITY library with no Vulkan counterpart.
//
//   D3DMAT_FromAxisT -> VMAT_FromAxisT. The client's own helper, renamed.
//
//   mWorld._11 / _21 / _31 -> m11 / m21 / m31. The same three elements under
//   FMATRIX4's naming.
//
// A BUG IN THE WINDOWS SOURCE IS FIXED AT THE END OF CreateRing: `grp->Idx`
// and `grp->Vtx` are allocated with new[] and freed with plain `delete`,
// which is undefined behaviour. See the note there.
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
	// Was pTex->Release(). ReleaseTex is that, under the name the rest of
	// this file already uses for it.
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

	// D3DCAPS9::MaxTextureWidth is VkPhysicalDeviceLimits::maxImageDimension2D
	// -- the same question, and one of the few D3DCAPS9 fields that has an
	// exact Vulkan counterpart.
	const VkPhysicalDeviceProperties *caps = gc->GetHardwareCaps();

	int size = max(min((int)caps->limits.maxImageDimension2D, 8192), 2048);

	sprintf_s(temp, ARRAYSIZE(temp), "%s_ring_%d.dds", fname, size);

	// Was:
	//   D3DXCreateTextureFromFileExA(pDev, path, 0,0, D3DFMT_FROM_FILE, 0,
	//                                D3DFMT_FROM_FILE, D3DPOOL_DEFAULT,
	//                                D3DX_DEFAULT, D3DX_DEFAULT, 0,
	//                                NULL, NULL, &pTex) == S_OK
	//
	// One D3DX call that opened the file, read the DDS header, picked a
	// format ("FROM_FILE"), allocated the texture and uploaded every mip.
	// There is no D3DX here, so the two halves are separate: read the bytes,
	// then hand them to the client's own DDS decoder, which is the same one
	// NatLoadSurface and LoadPlanetTextures use. D3DPOOL_DEFAULT and the two
	// D3DX_DEFAULT mip/filter arguments have no counterpart -- memory
	// placement follows the usage flags and no filtering happens on load.
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
	// D3DXVECTOR3 -> FVECTOR3, and D3DXVec3Length written out. mWorld._11 is
	// mWorld.m11.
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

	// Was `delete grp->Idx; delete grp->Vtx;`. BOTH WERE ALLOCATED WITH
	// new[], eight lines above, and freeing an array with scalar delete is
	// undefined behaviour -- for WORD and NTVERTEX it happens to work on the
	// usual allocators, which is why it has never been noticed. The brackets
	// are the whole fix and change nothing else.
	delete []grp->Idx;
	delete []grp->Vtx;
	delete grp;
	return msh;
}

oapi::VulkanClient *RingManager::gc = 0;
