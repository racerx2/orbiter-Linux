// ==============================================================
// Part of the ORBITER VISUALISATION PROJECT (OVP)
// Dual licensed under GPL v3 and LGPL v3
// Copyright (C) 2012-2026 Jarmo Nikkanen
// ==============================================================
//
// D3DFORMAT -> VkFormat: both D3DFMT_A8B8G8R8 and D3DFMT_X8B8G8R8 become
// VK_FORMAT_R8G8B8A8_UNORM. That looks inverted but is not -- Vulkan format
// names give byte order, D3D names give the packed DWORD, and 0xAABBGGRR is
// R,G,B,A in memory on a little-endian machine. Getting it backwards swaps red
// and blue in every surface tile. Vulkan has no X8 variant; a format with
// unused alpha is the same format with alpha ignored.
//
// GCC resolves names in templates where they are written and never searches a
// dependent base, so the derived managers need Objmgr<T>(pD, n) and this->pDev
// where MSVC accepted Objmgr(pD, n) and pDev.
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
		// 'es' in the Windows source -- a typo that never had to compile,
		// because nothing in the tree instantiates FreeSize().
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
	// Initialiser list reordered to declaration order (-Wreorder); MSVC does
	// not warn.
	Objmgr(VulkanDevice *pD, std::string n) : pDev(pD), name(n)	{ }
	// Made virtual: the three derived managers are deleted through an Objmgr*
	// in VulkanClient.cpp. GCC warns (-Wdelete-non-virtual-dtor); MSVC's
	// C4265 is off by default.
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
		// Code 1 was D3DFMT_X8B8G8R8, which maps to the same Vulkan format as
		// A8B8G8R8, so it is now unreachable. Left in the numbering below so
		// the packed codes still match the Windows values.
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

	// Was x->Release() with an assert that the count reached zero; nothing
	// here is reference counted. It cannot become `delete x` either: T is only
	// forward-declared in this header, so that would run no destructor and
	// leak the image and its memory.
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
		// tile loader maps and writes; the buffer kind is now a usage flag.
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
		// D3DFMT_INDEX16 is not a buffer property in Vulkan: the index type is
		// given to vkCmdBindIndexBuffer at bind time instead.
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
