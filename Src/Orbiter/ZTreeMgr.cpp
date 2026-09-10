// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "ZTreeMgr.h"
#include "zlib.h"
#include "util.h"

// THE PATH SEPARATOR IN OpenArchive, and why this one survived.
//
// The line was `sprintf (fname, "%s\\Archive\\%s.tree", ...)`. A backslash is
// a legal filename character on Linux, so the fopen does not fail loudly: it
// asks for one file literally named `Earth\Archive\Elev.tree`, gets ENOENT,
// and OpenArchive returns false. CreateFromFile then deletes the manager and
// hands back null, which is exactly what it does for a planet that simply has
// no archive -- so a broken path and an absent file are indistinguishable at
// every call site.
//
// That is the whole reason this instance was missed while the other four
// (Config.cpp, elevmgr.cpp, TabScenario.cpp, VectorMap.cpp) were found, and
// why the graphics client's own copy of this file was converted -- see
// OVP/VulkanClient/ZTreeMgr.cpp, whose comment calls itself the fourth
// instance -- while the core's copy was not: no planet in this installation
// ships an Archive directory, so nothing has ever exercised it.
//
// What it costs when it is exercised: ElevationManager's constructor asks for
// LAYER_ELEV and LAYER_ELEVMOD through CreateFromFile, so a planet whose
// elevation is packed rather than loose gets treeMgr[0] == 0 and every
// Elevation() query falls back to datum. That is the same failure the
// ORB_SEP note in elevmgr.cpp describes -- the PHYSICS believing the planet
// is a smooth sphere while the graphics client, which builds its own paths
// and now has them right, draws the real terrain.
//
// Only the separators are resolved: `Archive` and the six layer names are the
// directory and file names the format defines, and they are the same on both
// platforms.
#ifdef _WIN32
#define ORB_SEP "\\"
#else
#define ORB_SEP "/"
#endif

// =======================================================================
// File header for compressed tree files

TreeFileHeader::TreeFileHeader ()
{
	magic[0] = 'T';
	magic[1] = 'X';
	magic[2] = 1;
	magic[3] = 0;
	size = sizeof(TreeFileHeader);
	flags = 0;
	nodeCount = 0;
	dataOfs = size;
	dataLength = 0;
	rootPos1 = rootPos2 = rootPos3 = rootPos4[0] = rootPos4[1] = (DWORD)-1;
}

// -----------------------------------------------------------------------

size_t TreeFileHeader::fwrite(FILE *f)
{
	return ::fwrite(this, sizeof(TreeFileHeader), 1, f);
}

// -----------------------------------------------------------------------

bool TreeFileHeader::fread(FILE *f)
{
	BYTE buf[4];
	DWORD sz, flags;
	if (::fread(buf, 1, 4, f) < 4 || memcmp(buf, magic, 4))
		return false;
	if (::fread(&sz, sizeof(DWORD), 1, f) != 1 || sz != size)
		return false;
	::fread(&flags, sizeof(DWORD), 1, f);
	::fread(&dataOfs, sizeof(DWORD), 1, f);
	::fread(&dataLength, sizeof(__int64), 1, f);
	::fread(&nodeCount, sizeof(DWORD), 1, f);
	::fread(&rootPos1, sizeof(DWORD), 1, f);
	::fread(&rootPos2, sizeof(DWORD), 1, f);
	::fread(&rootPos3, sizeof(DWORD), 1, f);
	::fread(rootPos4, sizeof(DWORD), 2, f);
	return true;
}

// =======================================================================
// Tree table of contents

TreeTOC::TreeTOC()
{
	ntree = 0;
	ntreebuf = 0;
	tree = NULL;
	totlength = 0;
}

// -----------------------------------------------------------------------

TreeTOC::~TreeTOC()
{
	if (ntreebuf) {
		delete[] tree;
		tree = NULL;
	}
}

// -----------------------------------------------------------------------

size_t TreeTOC::fread(DWORD size, FILE *f)
{
	if (ntreebuf != size) {
		TreeNode *tmp = new TreeNode[size];
		if (ntreebuf) delete []tree;
		tree = tmp;
		ntree = ntreebuf = size;
	}
	return ::fread(tree, sizeof(TreeNode), size, f);
}

// =======================================================================
// ZTreeMgr class: manage a single layer tree for a planet

ZTreeMgr *ZTreeMgr::CreateFromFile(const char *PlanetPath, Layer _layer)
{
	ZTreeMgr *mgr = new ZTreeMgr(PlanetPath, _layer);
	if (!mgr->TOC().size()) {
		delete mgr;
		mgr = 0;
	}
	return mgr;
}

// -----------------------------------------------------------------------

ZTreeMgr::ZTreeMgr(const char *PlanetPath, Layer _layer)
{
	path = new char[strlen(PlanetPath)+1];
	strcpy(path, PlanetPath);
	layer = _layer;
	treef = 0;
	OpenArchive();
}

// -----------------------------------------------------------------------

ZTreeMgr::~ZTreeMgr()
{
	delete []path;
	path = NULL;
	if (treef) fclose(treef);
}

// -----------------------------------------------------------------------

bool ZTreeMgr::OpenArchive()
{
	const char *name[6] = { "Surf", "Mask", "Elev", "Elev_mod", "Label", "Cloud" };
	char fname[256];
	sprintf (fname, "%s" ORB_SEP "Archive" ORB_SEP "%s.tree", path, name[layer]);
	treef = fopen(fname, "rb");
	if (!treef) return false;

	TreeFileHeader tfh;
	if (!tfh.fread(treef)) {
		fclose(treef);
		treef = 0;
		return false;
	}
	rootPos1 = tfh.rootPos1;
	rootPos2 = tfh.rootPos2;
	rootPos3 = tfh.rootPos3;
	for (int i = 0; i < 2; i++)
		rootPos4[i] = tfh.rootPos4[i];
	dofs = (__int64)tfh.dataOfs;

	if (!toc.fread(tfh.nodeCount, treef)) {
		fclose(treef);
		treef = 0;
		return false;
	}
	toc.totlength = tfh.dataLength;

	return true;
}

// -----------------------------------------------------------------------

DWORD ZTreeMgr::Idx(int lvl, int ilat, int ilng)
{
	if (lvl <= 4) {
		return (lvl == 1 ? rootPos1 : lvl == 2 ? rootPos2 : lvl == 3 ? rootPos3 : rootPos4[ilng]);
	} else {
		int plvl = lvl-1;
		int pilat = ilat/2;
		int pilng = ilng/2;
		DWORD pidx = Idx(plvl, pilat, pilng);
		if (pidx == (DWORD)-1)
			return pidx;
		int cidx = ((ilat&1) << 1) + (ilng&1);
		return toc[pidx].child[cidx];
	}
}

// -----------------------------------------------------------------------

DWORD ZTreeMgr::ReadData(DWORD idx, BYTE **outp)
{
	if (idx == (DWORD)-1) return 0; // sanity check

	DWORD esize = NodeSizeInflated(idx);
	if (!esize) // node doesn't have data, but has descendants with data
		return 0;

	if (_fseeki64(treef, toc[idx].pos+dofs, SEEK_SET))
		return 0;

	DWORD zsize = NodeSizeDeflated(idx);
	BYTE *zbuf = new BYTE[zsize];	
	fread(zbuf, 1, zsize, treef);

	BYTE *ebuf = new BYTE[esize];

	DWORD ndata = Inflate(zbuf, zsize, ebuf, esize);
	delete []zbuf;
	zbuf = NULL;

	if (!ndata) {
		delete []ebuf;
		ebuf = 0;
	}
	*outp = ebuf;
	return ndata;
}

// -----------------------------------------------------------------------

DWORD ZTreeMgr::Inflate(const BYTE *inp, DWORD ninp, BYTE *outp, DWORD noutp)
{
#ifdef _WIN32
	DWORD ndata = noutp;
	if (uncompress (outp, &ndata, inp, ninp) != Z_OK)
		return 0;
	return ndata;
#else
	// zlib's uLongf is `unsigned long`: 32 bits on Windows and identical to
	// DWORD, 64 bits on LP64 Linux. Passing a DWORD* here would have zlib
	// write eight bytes into four, so a correctly typed temporary is used.
	uLongf ndata = noutp;
	if (uncompress (outp, &ndata, inp, ninp) != Z_OK)
		return 0;
	return (DWORD)ndata;
#endif
}

// -----------------------------------------------------------------------

void ZTreeMgr::ReleaseData(BYTE *data)
{
	delete []data;
	data = NULL;
}