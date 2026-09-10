// ==============================================================
//   ORBITER VISUALISATION PROJECT (OVP)
//   D3D9 Client module
//   Copyright (C) 2006-2026 Martin Schweiger
//   Dual licensed under GPL v3 and LGPL v3
// ==============================================================

// --------------------------------------------------------------
// ZTreeMgr.cpp
// Class ZTreeMgr (implementation)
//
// Manage compressed and packed tile trees for planetary surface
// and cloud layers.
// --------------------------------------------------------------
//
// CONVERTED FROM OVP/D3D9Client/ZTreeMgr.cpp, read end to end (219 lines).
//
// NO GRAPHICS API APPEARS IN THIS FILE ON EITHER PLATFORM. It is a file
// reader: a header, a table of contents, an fseek and oapiInflate. Every
// Microsoft spelling it uses -- MAKEFOURCC, lstrlen, strcpy_s, sprintf_s,
// fopen_s, _fseeki64, MAX_PATH -- the shim already supplies, and each one
// compiles unchanged.
//
// Two things change:
//
//   THE PATH SEPARATOR IN OpenArchive. This is the fourth instance in the
//   port of one defect class -- a Windows-spelled path handed to a POSIX
//   open -- and it is the only change here that is forced by the platform.
//   See the function.
//
//   Three -Wreorder initialiser lists, in TreeFileHeader and TreeTOC. Bugs
//   in the Windows source of the same kind as Objmgr's and ScatterParams';
//   see the constructors.
// --------------------------------------------------------------

#include "ZTreeMgr.h"
#include "OrbiterAPI.h"

// =======================================================================
// File header for compressed tree files

// The list is reordered to the declaration order -- magic, size, flags,
// dataOfs, dataLength, nodeCount -- because a member is initialised in
// DECLARATION order regardless of how the list is written, and dataOfs
// appeared before flags while nodeCount appeared before dataLength. Every
// value here is a constant, so nothing observable changes; it is written
// correctly because the next reader should not have to check.
TreeFileHeader::TreeFileHeader () :
	magic(MAKEFOURCC('T','X',1,0)),
	size(sizeof(TreeFileHeader)),
	flags(0), dataOfs(sizeof(TreeFileHeader)),
	dataLength(0), nodeCount(0)
{
	rootPos1 = rootPos2 = rootPos3 = rootPos4[0] = rootPos4[1] = (DWORD)-1;
}

// -----------------------------------------------------------------------

size_t TreeFileHeader::fwrite (FILE *f)
{
	return ::fwrite(this, sizeof(TreeFileHeader), 1, f);
}

// -----------------------------------------------------------------------

bool TreeFileHeader::fread (FILE *f)
{
	DWORD mg, sz;

	if (::fread(&mg, sizeof(DWORD), 1, f) != 1 || mg != magic) { return false; }
	if (::fread(&sz, sizeof(DWORD), 1, f) != 1 || sz != size) { return false; }
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

// Declaration order is tree, ntree, ntreebuf, totlength. Same reordering,
// same reason as TreeFileHeader above.
TreeTOC::TreeTOC () :
	tree(NULL),
	ntree(0),	ntreebuf(0), totlength(0)
{
}

// -----------------------------------------------------------------------

TreeTOC::~TreeTOC ()
{
	if (ntreebuf) {
		delete []tree;
		tree = NULL;
	}
}

// -----------------------------------------------------------------------

size_t TreeTOC::fread (DWORD size, FILE *f)
{
	if (ntreebuf != size) {
		TreeNode *tmp = new TreeNode[size];
		if (ntreebuf) { delete []tree; }
		tree = tmp;
		ntree = ntreebuf = size;
	}
	return ::fread(tree, sizeof(TreeNode), size, f);
}

// =======================================================================
// ZTreeMgr class: manage a single layer tree for a planet

ZTreeMgr *ZTreeMgr::CreateFromFile (const char *PlanetPath, Layer _layer)
{
	ZTreeMgr *mgr = new ZTreeMgr(PlanetPath, _layer);
	if (!mgr->TOC().size()) {
		delete mgr;
		mgr = NULL;
	}
	return mgr;
}

// -----------------------------------------------------------------------

ZTreeMgr::ZTreeMgr (const char *PlanetPath, Layer _layer) :
	layer(_layer), treef(NULL)
{
	int len = lstrlen(PlanetPath) + 1;
	path = new char[len];
	strcpy_s(path, len, PlanetPath);
	OpenArchive();
}

// -----------------------------------------------------------------------

ZTreeMgr::~ZTreeMgr ()
{
	delete []path;
	path = NULL;
	if (treef) { fclose(treef); }
}

// -----------------------------------------------------------------------

bool ZTreeMgr::OpenArchive ()
{
	const char *name[6] = { "Surf", "Mask", "Elev", "Elev_mod", "Label", "Cloud" };
	char fname[MAX_PATH];
	// Was "%s\\Archive\\%s.tree". A backslash is a LEGAL FILENAME CHARACTER
	// on Linux, so the Windows spelling does not fail as a bad path -- it
	// asks for one file literally named `Earth\Archive\Surf.tree`, which
	// does not exist, and fopen_s reports the same "not found" that a planet
	// with no tile archive gives. That is the whole defect class recorded in
	// the porting notes: the miss is indistinguishable from the
	// normal answer, and it surfaces far away as a planet that renders at
	// base resolution and never sharpens.
	//
	// A forward slash is the only spelling that opens the file. Case is NOT
	// resolved here: `Archive` and the six layer names are the directory
	// names Orbiter itself ships, and Config::ResolveConfigPath -- the one
	// place that walks components case-insensitively -- is a file-static in
	// Config.cpp and not reachable from a client.
	sprintf_s (fname, MAX_PATH, "%s/Archive/%s.tree", path, name[layer]);
	if (fopen_s(&treef, fname, "rb")) {
		return false;
	}
	TreeFileHeader tfh;
	if (!tfh.fread(treef)) {
		fclose(treef);
		treef = NULL;
		return false;
	}
	rootPos1 = tfh.rootPos1;
	rootPos2 = tfh.rootPos2;
	rootPos3 = tfh.rootPos3;
	for (int i = 0; i < 2; ++i) {
		rootPos4[i] = tfh.rootPos4[i];
	}
	dofs = (__int64)tfh.dataOfs;

	if (!toc.fread(tfh.nodeCount, treef)) {
		fclose(treef);
		treef = NULL;
		return false;
	}
	toc.totlength = tfh.dataLength;

	return true;
}

// -----------------------------------------------------------------------

DWORD ZTreeMgr::Idx (int lvl, int ilat, int ilng)
{
	if (lvl <= 4) {
		return (lvl == 1 ? rootPos1 : lvl == 2 ? rootPos2 : lvl == 3 ? rootPos3 : rootPos4[ilng]);
	} else {
		int plvl = lvl-1;
		int pilat = ilat/2;
		int pilng = ilng/2;
		DWORD pidx = Idx(plvl, pilat, pilng);
		if (pidx == (DWORD)-1) { return pidx; }
		int cidx = ((ilat&1) << 1) + (ilng&1);
		return toc[pidx].child[cidx];
	}
}

// -----------------------------------------------------------------------

DWORD ZTreeMgr::ReadData (DWORD idx, BYTE **outp)
{
	if (idx == (DWORD)-1) { return 0; } // sanity check

	DWORD esize = NodeSizeInflated(idx);
	if (!esize) {// node doesn't have data, but has descendants with data
		return 0;
	}

	if (_fseeki64(treef, toc[idx].pos+dofs, SEEK_SET)) {
		return 0;
	}

	DWORD zsize = NodeSizeDeflated(idx);
	BYTE *zbuf = new BYTE[zsize];
	fread(zbuf, 1, zsize, treef);

	BYTE *ebuf = new BYTE[esize];

	DWORD ndata = Inflate(zbuf, zsize, ebuf, esize);
	delete []zbuf;
	zbuf = NULL;

	if (!ndata) {
		delete []ebuf;
		ebuf = NULL;
	}
	*outp = ebuf;
	return ndata;
}

// -----------------------------------------------------------------------

DWORD ZTreeMgr::Inflate (const BYTE *inp, DWORD ninp, BYTE *outp, DWORD noutp)
{
	return oapiInflate(inp, ninp, outp, noutp);
}

// -----------------------------------------------------------------------

void ZTreeMgr::ReleaseData (BYTE *data)
{
	delete []data;
	data = NULL;
}

