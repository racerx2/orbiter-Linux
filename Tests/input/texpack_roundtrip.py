#!/usr/bin/env python3
# Round-trip test for Utils/texpack on Linux.
#
# Builds a synthetic planet tile tree with the quadtree shape texpack expects
# (levels 1-3 single tiles, level 4 two quadtree roots, levels 5+ full), packs
# it into Archive/<layer>.tree, extracts it back into a second directory and
# compares every file byte for byte.
#
# Also checks the archive header against the layout Src/Orbiter/ZTreeMgr.cpp
# reads, so a pack that round-trips through texpack alone but that the engine
# cannot open is still caught.

import os, shutil, struct, subprocess, sys, hashlib, random

ROOT   = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "/tmp/texpack_test")
BIN    = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else
                         "/home/racerx/orbiter-native/build/Utils/texpack/texpack")
LAYER  = "Surf"
EXT    = "dds"
MAXLVL = 6

def tiles():
    """(lvl, ilat, ilng) for the tree texpack's MemTree can represent."""
    yield (1, 0, 0)
    yield (2, 0, 0)
    yield (3, 0, 0)
    for ilng in range(2):
        yield (4, 0, ilng)
    for lvl in range(5, MAXLVL + 1):
        nlat = 1 << (lvl - 4)
        nlng = 2 << (lvl - 4)
        for ilat in range(nlat):
            for ilng in range(nlng):
                yield (lvl, ilat, ilng)

def build(base):
    random.seed(20260908)
    made = {}
    for (lvl, ilat, ilng) in tiles():
        d = os.path.join(base, LAYER, "%02d" % lvl, "%06d" % ilat)
        os.makedirs(d, exist_ok=True)
        p = os.path.join(d, "%06d.%s" % (ilng, EXT))
        # Varying sizes, including one over the 32768-byte initial buffer so
        # the grow path in AddSubtree and WriteSubtreeData is exercised.
        n = 40000 if (lvl, ilat, ilng) == (5, 0, 0) else random.randint(64, 4096)
        data = bytes(random.getrandbits(8) for _ in range(n))
        with open(p, "wb") as f:
            f.write(data)
        made[(lvl, ilat, ilng)] = hashlib.sha256(data).hexdigest()
    return made

def run(args):
    r = subprocess.run([BIN] + args, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout[-4000:]); print(r.stderr[-4000:])
        raise SystemExit("texpack failed: %s" % args)
    return r.stdout

def check_header(path, expect_nodes):
    """The header layout Src/Orbiter/ZTreeMgr.cpp's TreeFileHeader::fread reads."""
    with open(path, "rb") as f:
        hdr = f.read(48)
    magic, size, flags, dataOfs, dataLength, nodeCount = \
        struct.unpack_from("<4sIIIqI", hdr, 0)
    roots = struct.unpack_from("<5I", hdr, 28)
    problems = []
    if magic != b"TX\x01\x00":         problems.append("magic %r" % magic)
    if size != 48:                     problems.append("header size %d, want 48" % size)
    if flags != 1:                     problems.append("flags %d, want 1 (TREE_DEFLATE)" % flags)
    if nodeCount != expect_nodes:      problems.append("nodeCount %d, want %d" % (nodeCount, expect_nodes))
    if dataOfs != 48 + 32 * nodeCount: problems.append("dataOfs %d, want %d" % (dataOfs, 48 + 32*nodeCount))
    if dataLength <= 0:                problems.append("dataLength %d" % dataLength)
    return problems, dict(size=size, flags=flags, dataOfs=dataOfs,
                          dataLength=dataLength, nodeCount=nodeCount, roots=roots)

def main():
    shutil.rmtree(ROOT, ignore_errors=True)
    src = os.path.join(ROOT, "src", "TestBody")
    dst = os.path.join(ROOT, "dst", "TestBody")
    os.makedirs(src); os.makedirs(dst)

    want = build(src)
    print("built %d tiles" % len(want))

    out = run([src, LAYER, "-L%d" % MAXLVL])
    added = out.count("deflating ")
    print("packed:", added, "tiles deflated")

    arc = os.path.join(src, "Archive", "%s.tree" % LAYER)
    if not os.path.isfile(arc):
        raise SystemExit("no archive written at %s" % arc)
    problems, info = check_header(arc, len(want))
    print("header:", info)

    # Extract into a clean tree.
    shutil.copytree(os.path.join(src, "Archive"), os.path.join(dst, "Archive"))
    run([dst, LAYER, "-e", "-L%d" % MAXLVL])

    bad = []
    for key, digest in want.items():
        lvl, ilat, ilng = key
        p = os.path.join(dst, LAYER, "%02d" % lvl, "%06d" % ilat, "%06d.%s" % (ilng, EXT))
        if not os.path.isfile(p):
            bad.append("missing %s" % p); continue
        with open(p, "rb") as f:
            if hashlib.sha256(f.read()).hexdigest() != digest:
                bad.append("differs %s" % p)

    extra = []
    for dirpath, _, names in os.walk(os.path.join(dst, LAYER)):
        for n in names:
            rel = os.path.relpath(os.path.join(dirpath, n), os.path.join(dst, LAYER))
            parts = rel.split(os.sep)
            key = (int(parts[0]), int(parts[1]), int(parts[2].split(".")[0]))
            if key not in want:
                extra.append(rel)

    print("---")
    print("tiles packed      :", len(want))
    print("tiles extracted   :", len(want) - len([b for b in bad if b.startswith("missing")]))
    print("byte mismatches   :", len([b for b in bad if b.startswith("differs")]))
    print("unexpected files  :", len(extra))
    print("header problems   :", problems if problems else "none")
    ok = not bad and not extra and not problems
    print("RESULT:", "PASS" if ok else "FAIL")
    for b in bad[:20]: print("  ", b)
    for e in extra[:20]: print("   extra", e)
    return 0 if ok else 1

if __name__ == "__main__":
    sys.exit(main())
