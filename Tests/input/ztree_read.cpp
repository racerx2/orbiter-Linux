// Read a texpack archive back with THE ENGINE'S OWN READER.
//
// texpack_roundtrip.py proves texpack can read what texpack wrote, which is a
// closed loop: a wrong header field or a wrong TOC stride would round-trip
// perfectly and still be unreadable by Orbiter. This links
// Src/Orbiter/ZTreeMgr.cpp -- the file ElevationManager loads packed
// elevation through -- and extracts every tile with it, so the comparison is
// against the reader that matters.
//
// Build:
//   g++ -std=c++17 -fpermissive -o ztree_read Tests/input/ztree_read.cpp \
//       Src/Orbiter/ZTreeMgr.cpp -I Src/Orbiter/Linux -I Src/Orbiter \
//       -I <zlib-src> -I <zlib-build> -lz
// Run:
//   ztree_read <planet-dir> <layer-index> <maxlevel> <out-dir>

#include "ZTreeMgr.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <filesystem>

int main(int argc, char **argv)
{
	if (argc < 5) {
		fprintf(stderr, "usage: %s <planet-dir> <layer 0..5> <maxlvl> <out-dir>\n", argv[0]);
		return 2;
	}
	const char *root   = argv[1];
	const int   layer  = atoi(argv[2]);
	const int   maxlvl = atoi(argv[3]);
	const std::string out = argv[4];

	static const char *ext[6] = { "dds", "dds", "elv", "elv", "lab", "dds" };

	ZTreeMgr *mgr = ZTreeMgr::CreateFromFile(root, (ZTreeMgr::Layer)layer);
	if (!mgr) {
		fprintf(stderr, "ZTreeMgr::CreateFromFile FAILED for %s layer %d\n", root, layer);
		return 1;
	}
	printf("nodes in TOC: %u\n", (unsigned)mgr->TOC().size());

	int written = 0, empty = 0;
	for (int lvl = 1; lvl <= maxlvl; lvl++) {
		const int nlat = (lvl <= 4) ? 1 : (1 << (lvl - 4));
		const int nlng = (lvl <= 3) ? 1 : (lvl == 4 ? 2 : (2 << (lvl - 4)));
		for (int ilat = 0; ilat < nlat; ilat++) {
			for (int ilng = 0; ilng < nlng; ilng++) {
				const DWORD idx = mgr->Idx(lvl, ilat, ilng);
				if (idx == (DWORD)-1) continue;
				BYTE *buf = 0;
				const DWORD n = mgr->ReadData(idx, &buf);
				if (!n || !buf) { empty++; continue; }

				char dir[512], path[640];
				snprintf(dir, sizeof(dir), "%s/%02d/%06d", out.c_str(), lvl, ilat);
				std::error_code ec;
				std::filesystem::create_directories(dir, ec);
				snprintf(path, sizeof(path), "%s/%06d.%s", dir, ilng, ext[layer]);
				FILE *f = fopen(path, "wb");
				if (!f) { fprintf(stderr, "cannot write %s\n", path); return 1; }
				fwrite(buf, 1, n, f);
				fclose(f);
				mgr->ReleaseData(buf);
				written++;
			}
		}
	}
	printf("tiles written: %d, empty nodes skipped: %d\n", written, empty);
	delete mgr;
	return 0;
}
