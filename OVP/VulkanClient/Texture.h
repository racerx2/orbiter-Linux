// ==============================================================
// Texture.h
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2006-2026 Martin Schweiger
// ==============================================================

// ==============================================================
// Texture loading and management routines for the Vulkan client.
//
// Methods for loading single (.dds) and multi-texture files (.tex)
// stored in DXT? format.
// ==============================================================
//
// CONVERTED FROM OVP/D3D9Client/Texture.h, read end to end (82 lines).
//
// A name -> SURFHANDLE repository, a linked list and a checksum. Three
// renames: oapi::D3D9Client -> oapi::VulkanClient,
// LPDIRECT3DDEVICE9 -> VulkanDevice*, LPDIRECT3DTEXTURE9 -> VulkanTexture*.
//
// The header comment's "DIRECTDRAWSURFACE7" is corrected: it named a DirectX
// 7 type that this class stopped using long before the D3D9 client existed
// -- it deals in SURFHANDLEs, which is what the code says.
//
// WORTH KNOWING: `LoadTextures` (plural) is declared here and DEFINED
// NOWHERE, on Windows or here. It is dead, and converted rather than deleted
// for the same reason CSphereManager::CreateDeviceObjects was.
// ==============================================================

#ifndef __TEXTURE_H
#define __TEXTURE_H

#include "VulkanClient.h"
#include <stdio.h>

// ==============================================================
// Class TextureManager

class TextureManager {
public:

	explicit TextureManager(oapi::VulkanClient *gclient);
	~TextureManager();
	
	HRESULT LoadTexture(const char *fname, SURFHANDLE *ppdds, int flags);

	// Declared and never defined; see the file header.
	int LoadTextures(const char *fname, VulkanTexture **ppdds, DWORD flags, int count);
	// Read a texture from file 'fname' into the surface pointed to by 'ppdds'.

	bool GetTexture(const char *fname, SURFHANDLE *ppdds,int flags);
	// Retrieve a texture. First scans the repository of loaded textures.
	// If not found, loads the texture from file and adds it to the repository

	bool IsInRepository (SURFHANDLE p);

protected:

	DWORD MakeTexId(const char *fname);
	// simple checksum of a string. Used for speeding up texture searches.

private:
	oapi::VulkanClient *gc;
	VulkanDevice *pDev;

	// simple repository of loaded textures: linked list
	struct TexRec {
		SURFHANDLE tex;
		char fname[64];
		DWORD id;
		struct TexRec *next;
	} *firstTex;

	//TexRec *pRepo;

	// Some repository management functions below.
	// This could be made more sophisticated (defining a maximum size of
	// the repository, deallocating unused textures as required, etc.)
	// Would also require a reference counter and a size parameter in the
	// TexRec structure.

	TexRec *ScanRepository (const char *fname);
	// Return a matching texture entry from the repository, if found.
	// Otherwise, return NULL.

	void AddToRepository (const char *fname, SURFHANDLE pdds);
	// Add a new entry to the repository

	void ClearRepository ();
	// De-allocates the repository and releases the textures
};


// ==============================================================
// Non-member utility functions

#endif // !__TEXTURE_H
