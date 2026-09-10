// ==============================================================
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2012-2026 Jarmo Nikkanen
// ==============================================================
//
// CONVERTED FROM OVP/D3D9Client/D3D9Catalog.h, read end to end (368 lines).
//
// Four pools -- raw memory, tile textures, tile vertex buffers, tile index
// buffers -- that hand out recycled objects by size rather than allocating
// per tile. The pooling logic is generic and crosses unchanged; only the
// three Alloc/Delete pairs at the bottom actually touch the device.
//
// WHAT CHANGED
//
//  1. Alloc() CREATES THROUGH VulkanDevice. D3DXCreateTexture,
//     CreateVertexBuffer and CreateIndexBuffer become CreateTexture and
//     CreateBuffer -- the same responsibility on the same object. Vulkan
//     tells a vertex buffer from an index buffer by usage flags rather than
//     by type, so the two buffer managers differ only in the flag they pass.
//
//  2. Delete() IS delete, NOT Release(). The Windows code asserts that the
//     COM reference count reached zero:
//
//         void Delete(T x) { UINT q = x->Release(); assert(q == 0); }
//
//     Vulkan objects are not reference counted; the client's wrapper owns its
//     handles and destroys them in its destructor. There is no count to
//     assert on, so the assert goes with the refcount rather than being
//     reproduced as something weaker.
//
//  3. D3DFORMAT BECAME VkFormat. The mapping is exact, and the first entry is
//     the one worth stating because it looks wrong:
//
//         D3DFMT_A8B8G8R8 -> VK_FORMAT_R8G8B8A8_UNORM
//         D3DFMT_X8B8G8R8 -> VK_FORMAT_R8G8B8A8_UNORM  (alpha present, unused)
//         D3DFMT_DXT1     -> VK_FORMAT_BC1_RGBA_UNORM_BLOCK
//         D3DFMT_DXT3     -> VK_FORMAT_BC2_UNORM_BLOCK
//         D3DFMT_DXT5     -> VK_FORMAT_BC3_UNORM_BLOCK
//
//     A D3DFMT_A8B8G8R8 pixel is the DWORD 0xAABBGGRR, so its bytes in memory
//     on a little-endian machine run R,G,B,A -- which is R8G8B8A8, not
//     A8B8G8R8. Vulkan format names describe BYTE order unless they end in
//     _PACK32; D3D names describe the packed DWORD. Getting this backwards
//     swaps red and blue in every surface tile on the planet.
//
//     Vulkan has no X8 variant. A format with unused alpha is the same
//     format with alpha ignored, so both map to R8G8B8A8_UNORM and the
//     distinction disappears -- which is what it already meant.
//
//  4. TWO GCC TWO-PHASE-LOOKUP FIXES, both in the three derived managers.
//     MSVC parses templates lazily and resolves these at instantiation; GCC
//     resolves them where written and never searches a dependent base:
//
//         Objmgr(pD, n)  ->  Objmgr<T>(pD, n)   (injected-class-name)
//         pDev           ->  this->pDev         (member of dependent base)
//
//  5. Memgr::FreeSize() DID NOT COMPILE, and could not have on either
//     platform:
//
//         for (auto x : Fre) for (auto y : x.second) { es += ...; }
//
//     There is no 'es'; the accumulator is 'ec'. A template member is only
//     compiled when it is instantiated, so this survived because NOTHING IN
//     THE TREE CALLS FreeSize(). Corrected to 'ec' rather than left as a
//     trap for the first caller.
// ==============================================================

#ifndef __VULKANCATALOG_H
#define __VULKANCATALOG_H

#include <set>
#include <map>
#include <list>
#include <string>
#include <assert.h>
#include <mutex>
#include <vulkan/vulkan.h>
#include "OrbiterAPI.h"
#include "VulkanTypes.h"
#include "VulkanFrame.h"
#include "VulkanConfig.h"
#include "VulkanUtil.h"

template <typename T>
class VulkanCatalog {
public:
	VulkanCatalog ()				{}
	~VulkanCatalog ()				{ Clear(); }

	void	Add (T entry)			{ _data.insert(entry); }
	void	Clear ()				{ _data.clear();  }
//	T		Seek (T entry) const	{ return _data.find(entry) - _data.begin(); }
	size_t	CountEntries () const	{ return _data.size(); }
	bool	Remove (T entry)		{ return _data.erase(entry) == 1; }

	typedef std::set<T> TSet;
	typedef typename TSet::iterator iterator;
	typedef typename TSet::const_iterator const_iterator;

	iterator		begin () const	{ return _data.begin(); }
	iterator		end () const	{ return _data.end(); }
	const_iterator	cbegin () const	{ return _data.cbegin(); }
	const_iterator	cend ()	const	{ return _data.cend(); }

private:
	TSet _data;
};


// ---------------------------------------------------------------
// Memory Manager
// ---------------------------------------------------------------

template <typename T>
class Memgr
{
private:

	std::string name;
	std::map<DWORD, std::list<T*>> Fre;
	std::map<T*, DWORD> Rsv;
	std::mutex mm;

public:
	Memgr(std::string n) : name(n)
	{
	}

	~Memgr()
	{
#ifdef _DEBUG
		for (auto x : Fre) {
			size_t size = 0; DWORD ent = 0;
			for (auto y : x.second) { ent++; size += x.first; delete y; }
			oapiWriteLogV("Memgr[%s] Size[%u]: Total of %u bytes in %u entries", name.c_str(), x.first, size * sizeof(T), ent);
		}
		if (Rsv.size() == 0) oapiWriteLogV("Memgr[%s] All clear",name.c_str());
		else for (auto x : Rsv) {
			oapiWriteLogV("Memgr[%s] Leaking %u bytes", name.c_str(), x.second * sizeof(T));
			delete x.first;
		}
#else
		for (auto x : Fre) for (auto y : x.second) delete y;
		for (auto x : Rsv) delete x.first;
#endif // DEBUG
	}

	T* New(DWORD size)
	{
		mm.lock();
		if (Fre.find(size) != Fre.end()) { // Do we have entries of size 'size'
			auto& r = Fre[size];
			if (r.size() > 0) {			// Any any unused exists ?
				auto p = r.front();		// Get top entry
				r.pop_front();			// Remove it
				Rsv[p] = size;			// List it as used
				mm.unlock();
				return p;
			}
		}
		auto x = new T[size];			// Allocate new entry
		Rsv[x] = size;					// List it as used
		mm.unlock();
		return x;
	}

	void Free(T* p)
	{
		mm.lock();
		auto it = Rsv.find(p);			// Find the entry (log2 complexity)
		assert(it != Rsv.end());
		Fre[it->second].push_front(p);	// Add it in a fron of free entries
		Rsv.erase(it);					// Remove from used (reserved)
		mm.unlock();
	}

	size_t UsedSize()
	{
		mm.lock();
		size_t ec = 0;
		for (auto x : Rsv) ec += x.second * sizeof(T);
		mm.unlock();
		return ec;
	}

	size_t FreeSize()
	{
		mm.lock();
		size_t ec = 0;
		// 'es' in the Windows source. See the file header: never instantiated,
		// so never compiled, on either platform.
		for (auto x : Fre) for (auto y : x.second) { (void)y; ec += x.first * sizeof(T); }
		mm.unlock();
		return ec;
	}
};



// ---------------------------------------------------------------
// Object Manager, Base
// ---------------------------------------------------------------

template <typename T>
class Objmgr
{

public:
	// pDev before name: it is declared first (protected, below), and GCC's
	// -Wreorder reports an initialiser list that does not follow declaration
	// order. The Windows list reads name(n), pDev(pD); MSVC does not warn.
	Objmgr(VulkanDevice *pD, std::string n) : pDev(pD), name(n)	{ }
	// virtual, which the Windows declaration is not. Objmgr is polymorphic --
	// Delete() and UnitSize() are virtual and are what Texmgr, Vtxmgr and
	// Idxmgr override -- and VulkanClient.cpp deletes the three derived
	// managers with SAFE_DELETE. GCC reports the non-virtual destructor under
	// -Wall (-Wdelete-non-virtual-dtor); MSVC's C4265 is off by default. Same
	// fix, same argument, as gcConst's and WindowManager's.
	virtual ~Objmgr() {
		Fre.clear();
		Rsv.clear();
	}

	void CleanUp()
	{
		mm.lock();
#ifdef _DEBUG
		for (auto x : Fre) {
			size_t size = 0; DWORD ent = 0;
			for (auto y : x.second) { ent++; size += UnitSize(x.first); Delete(y); }
			oapiWriteLogV("Objmgr[%s] Size[%u]: Total of %u bytes in %u entries", name.c_str(), x.first, size, ent);
		}	
		if (Rsv.size() == 0) oapiWriteLogV("Objmgr[%s] All clear", name.c_str());
		else for (auto x : Rsv) {
			oapiWriteLogV("Objmgr[%s] Leaking %u bytes", name.c_str(), UnitSize(x.second));
			Delete(x.first);
		}
#else
		for (auto x : Fre) for (auto y : x.second) Delete(y);
		for (auto x : Rsv) Delete(x.first);
#endif // DEBUG
		mm.unlock();
	}


	T New(DWORD size)
	{
		mm.lock();
		auto a = Fre.find(size);
		auto b = Fre.end();
		if (a != b) { // Do we have entries of size 'size'
			auto& r = Fre[size];
			if (r.size() > 0) {			// Any any unused exists ?
				auto p = r.front();		// Get top entry
				r.pop_front();			// Remove it
				Rsv[p] = size;			// List it as used
				mm.unlock();
				return p;
			}
		}
		auto x = Alloc(size);
		Rsv[x] = size;					// List it as used
		mm.unlock();
		return x;
	}

	void Free(T p)
	{
		mm.lock();
		auto it = Rsv.find(p);			// Find the entry (log2 complexity)
		assert(it != Rsv.end());
		Fre[it->second].push_front(p);	// Add it in a fron of free entries
		Rsv.erase(it);					// Remove from used (reserved)
		mm.unlock();
	}

	size_t UsedSize()
	{
		mm.lock();
		size_t s = 0;
		for (auto x : Rsv) s += UnitSize(x.second);
		mm.unlock();
		return s;
	}

	size_t FreeSize()
	{
		mm.lock();
		size_t s = 0;
		for (auto x : Fre) for (auto y : x.second) { (void)y; s += UnitSize(x.first); }
		mm.unlock();
		return s;
	}

	int UsedCount()
	{
		mm.lock();
		int c = Rsv.size();
		mm.unlock();
		return c;
	}

	int FreeCount()
	{
		mm.lock();
		int c = 0;
		for (auto x : Fre) c += x.second.size();
		mm.unlock();
		return c;
	}

protected:
	virtual T Alloc(DWORD size) { assert(false); return nullptr; };
	virtual void Delete(T x) { assert(false); };
	virtual size_t UnitSize(DWORD size) { assert(false); return size; }
	VulkanDevice *pDev;

private:
	std::string name;
	std::map<DWORD, std::list<T>> Fre;
	std::map<T, DWORD> Rsv;
	std::mutex mm;
};



// ---------------------------------------------------------------
// Tile Texture Manager
// ---------------------------------------------------------------

template <typename T>
class Texmgr : public Objmgr<T>
{
public:
	Texmgr(VulkanDevice *pD, std::string n) : Objmgr<T>(pD, n) { }
	~Texmgr() {}

	T New(DWORD size, VkFormat Format)
	{
		DWORD fmt = 0;
		// D3DFMT_X8B8G8R8 and D3DFMT_A8B8G8R8 are the same Vulkan format --
		// the X form only said "alpha unused" -- so the tag that told them
		// apart is gone and code 1 is unreachable. It is kept so the packed
		// codes below still line up with the Windows numbering.
		if (Format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK) fmt = 2;
		if (Format == VK_FORMAT_BC2_UNORM_BLOCK) fmt = 3;
		if (Format == VK_FORMAT_BC3_UNORM_BLOCK) fmt = 4;

		return Objmgr<T>::New(size + (fmt << 16));
	}

protected:

	T Alloc(DWORD prm)
	{
		VkFormat Format = VK_FORMAT_R8G8B8A8_UNORM;

		UINT size = prm & 0xFFFF;
		UINT frmt = prm >> 16;
		UINT Mips = (Config->TileMipmaps == 1) ? 6 : 1;

		if (frmt == 1) Format = VK_FORMAT_R8G8B8A8_UNORM;
		if (frmt == 2) Format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
		if (frmt == 3) Format = VK_FORMAT_BC2_UNORM_BLOCK;
		if (frmt == 4) Format = VK_FORMAT_BC3_UNORM_BLOCK;

		VulkanTexture *pT = this->pDev->CreateTexture(size, size, Mips, Format,
			VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);

		if (!pT)
		{
			oapiWriteLog((char*)"Failed to create texture for surface tile. Likely [Out of Video Memory]");
			abort();
		}
		return (T)pT;
	}

	// Was x->Release() with an assert on the count reaching zero. Nothing is
	// reference counted here, and this cannot be `delete x`: T is only
	// forward-declared in this header, so deleting it would run no destructor
	// and leak the image and its memory. See VulkanDevice::DestroyTexture.
	void Delete(T x) {
		this->pDev->DestroyTexture(x);
	}
	size_t UnitSize(DWORD size) { return (size & 0xFFFF) * (size & 0xFFFF); }
};



// ---------------------------------------------------------------
// Tile VertexBuffer Manager
// ---------------------------------------------------------------

template <typename T>
class Vtxmgr : public Objmgr<T>
{
public:
	Vtxmgr(VulkanDevice *pD, std::string n) : Objmgr<T>(pD, n) { }
protected:
	T Alloc(DWORD size)
	{
		// D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY -> host-visible memory the
		// tile loader maps and writes. The buffer kind is the usage flag.
		VulkanBuffer *pVB = this->pDev->CreateBuffer(size * sizeof(VERTEX_2TEX),
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
		if (!pVB)
		{
			oapiWriteLog((char*)"Failed to create vertex buffer for surface tile. Likely [Out of Video Memory]");
			abort();
		}
		return (T)pVB;
	}
	void Delete(T x) {
		this->pDev->DestroyBuffer(x);
	}
	size_t UnitSize(DWORD size) { return size * sizeof(VERTEX_2TEX); }
};



// ---------------------------------------------------------------
// Tile IndexBuffer Manager
// ---------------------------------------------------------------

template <typename T>
class Idxmgr : public Objmgr<T>
{
public:
	Idxmgr(VulkanDevice *pD, std::string n) : Objmgr<T>(pD, n) { }
protected:
	T Alloc(DWORD size)
	{
		// D3DFMT_INDEX16 is not a buffer property in Vulkan: the index type
		// is given to vkCmdBindIndexBuffer at bind time, so it disappears
		// from creation and reappears at the draw. The size arithmetic --
		// three 16-bit indices per triangle -- is unchanged.
		VulkanBuffer *pIB = this->pDev->CreateBuffer(size * sizeof(WORD) * 3,
			VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
		if (!pIB)
		{
			oapiWriteLog((char*)"Failed to create index buffer for surface tile. Likely [Out of Video Memory]");
			abort();
		}
		return (T)pIB;
	}
	void Delete(T x) {
		this->pDev->DestroyBuffer(x);
	}
	size_t UnitSize(DWORD size) { return size * sizeof(WORD) * 3; }
};

#endif // !__VULKANCATALOG_H
