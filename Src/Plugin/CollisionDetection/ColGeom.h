// ==============================================================
// ColGeom.h -- triangle store, BVH, and the two geometry queries.
//
// Ported from the standalone harness the contact model was validated in.
// Nothing here knows about Orbiter's sign convention: a BVH is a BVH. The
// convention only matters where velocities and torques are formed, which is
// ColSolve.h.
// ==============================================================
#pragma once

#include "Orbitersdk.h"
#include <vector>
#include <algorithm>
#include <cmath>
#include <map>
#include <array>

namespace col {

struct Tri { VECTOR3 a, e1, e2; };

inline double cmp3(const VECTOR3 &v, int i) { return i == 0 ? v.x : (i == 1 ? v.y : v.z); }

struct Node { double lo[3], hi[3]; int left, start, count; };

// --------------------------------------------------------------
// Bounding volume hierarchy over a triangle list. Median split on the widest
// axis; leaves of four. Build cost measured at 8.5-12.7 ms for a full vessel
// mesh, which is why it is cached per vessel and not rebuilt per frame.
// --------------------------------------------------------------
class BVH {
public:
	std::vector<Node> nodes;
	std::vector<int>  idx;
	const std::vector<Tri> *tris;

	void tribox(int i, double *lo, double *hi) const
	{
		const Tri &t = (*tris)[i];
		VECTOR3 p1 = t.a + t.e1, p2 = t.a + t.e2;
		for (int k = 0; k < 3; k++) {
			lo[k] = std::min(cmp3(t.a,k), std::min(cmp3(p1,k), cmp3(p2,k)));
			hi[k] = std::max(cmp3(t.a,k), std::max(cmp3(p1,k), cmp3(p2,k)));
		}
	}

	int build_range(int start, int count, int depth)
	{
		int me = (int)nodes.size();
		nodes.push_back(Node());
		double lo[3] = { 1e300, 1e300, 1e300 }, hi[3] = { -1e300, -1e300, -1e300 };
		for (int i = 0; i < count; i++) {
			double tl[3], th[3];
			tribox(idx[start + i], tl, th);
			for (int k = 0; k < 3; k++) { lo[k] = std::min(lo[k], tl[k]); hi[k] = std::max(hi[k], th[k]); }
		}
		for (int k = 0; k < 3; k++) { nodes[me].lo[k] = lo[k]; nodes[me].hi[k] = hi[k]; }

		if (count <= 4 || depth > 40) {
			nodes[me].left = -1; nodes[me].start = start; nodes[me].count = count;
			return me;
		}
		int ax = 0; double ex = hi[0] - lo[0];
		if (hi[1] - lo[1] > ex) { ax = 1; ex = hi[1] - lo[1]; }
		if (hi[2] - lo[2] > ex) { ax = 2; }
		int mid = count / 2;
		std::nth_element(idx.begin() + start, idx.begin() + start + mid, idx.begin() + start + count,
			[&](int A, int B) {
				double al[3], ah[3], bl[3], bh[3];
				tribox(A, al, ah); tribox(B, bl, bh);
				return (al[ax] + ah[ax]) < (bl[ax] + bh[ax]);
			});
		int l = build_range(start, mid, depth + 1);
		int r = build_range(start + mid, count - mid, depth + 1);
		nodes[me].left = l; nodes[me].start = r; nodes[me].count = -1;
		return me;
	}

	void build(const std::vector<Tri> &t)
	{
		tris = &t;
		idx.resize(t.size());
		for (size_t i = 0; i < t.size(); i++) idx[i] = (int)i;
		nodes.clear();
		if (t.empty()) return;
		nodes.reserve(t.size() * 2);
		build_range(0, (int)t.size(), 0);
	}
	bool empty() const { return nodes.empty(); }
};

// --------------------------------------------------------------
// Nearest triangle hit by the segment org -> org + dir*tmax.
// Moller-Trumbore, no backface rejection: the caller decides sidedness.
// --------------------------------------------------------------
inline bool segment_hit(const BVH &bvh, const VECTOR3 &o, const VECTOR3 &d,
                        double tmax, double &best, int &bt)
{
	best = tmax; bt = -1;
	if (bvh.empty()) return false;
	double inv[3] = { 1.0 / (d.x ? d.x : 1e-300), 1.0 / (d.y ? d.y : 1e-300), 1.0 / (d.z ? d.z : 1e-300) };
	int st[64], sp = 0; st[sp++] = 0;
	while (sp) {
		const Node &n = bvh.nodes[st[--sp]];
		double t0 = 0.0, t1 = best; bool miss = false;
		for (int k = 0; k < 3 && !miss; k++) {
			double a = (n.lo[k] - cmp3(o,k)) * inv[k], b = (n.hi[k] - cmp3(o,k)) * inv[k];
			if (a > b) std::swap(a, b);
			if (a > t0) t0 = a;
			if (b < t1) t1 = b;
			if (t1 < t0) miss = true;
		}
		if (miss) continue;
		if (n.count >= 0) {
			for (int i = 0; i < n.count; i++) {
				int ti = bvh.idx[n.start + i];
				const Tri &t = (*bvh.tris)[ti];
				VECTOR3 pv = crossp(d, t.e2);
				double det = dotp(t.e1, pv);
				if (fabs(det) < 1e-14) continue;
				double id = 1.0 / det;
				VECTOR3 tv = o - t.a;
				double u = dotp(tv, pv) * id;
				if (u < -1e-9 || u > 1 + 1e-9) continue;
				VECTOR3 qv = crossp(tv, t.e1);
				double v = dotp(d, qv) * id;
				if (v < -1e-9 || u + v > 1 + 1e-9) continue;
				double tt = dotp(t.e2, qv) * id;
				if (tt > 1e-9 && tt < best) { best = tt; bt = ti; }
			}
		} else { st[sp++] = n.left; st[sp++] = n.start; }
	}
	return bt >= 0;
}

// --------------------------------------------------------------
// Closest point on a triangle (Ericson).
// --------------------------------------------------------------
inline VECTOR3 tri_closest(const VECTOR3 &p, const VECTOR3 &a, const VECTOR3 &ab, const VECTOR3 &ac)
{
	VECTOR3 ap = p - a; double d1 = dotp(ab, ap), d2 = dotp(ac, ap);
	if (d1 <= 0 && d2 <= 0) return a;
	VECTOR3 b = a + ab, c = a + ac;
	VECTOR3 bp = p - b; double d3 = dotp(ab, bp), d4 = dotp(ac, bp);
	if (d3 >= 0 && d4 <= d3) return b;
	double vc = d1 * d4 - d3 * d2;
	if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
	VECTOR3 cp = p - c; double d5 = dotp(ab, cp), d6 = dotp(ac, cp);
	if (d6 >= 0 && d5 <= d6) return c;
	double vb = d5 * d2 - d1 * d6;
	if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
	double va = d3 * d6 - d5 * d4;
	if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
	double den = 1.0 / (va + vb + vc);
	return a + ab * (vb * den) + ac * (vc * den);
}

inline bool closest_pt(const BVH &bvh, const VECTOR3 &p, double maxd,
                       VECTOR3 &q, double &dout, int &tout)
{
	if (bvh.empty()) return false;
	double best2 = maxd * maxd; bool found = false; VECTOR3 bq = _V(0,0,0); int bt = -1;
	int st[64], sp = 0; st[sp++] = 0;
	while (sp) {
		const Node &n = bvh.nodes[st[--sp]];
		double s2 = 0;
		for (int k = 0; k < 3; k++) {
			double v = cmp3(p,k);
			if (v < n.lo[k]) { double e = n.lo[k] - v; s2 += e * e; }
			else if (v > n.hi[k]) { double e = v - n.hi[k]; s2 += e * e; }
		}
		if (s2 > best2) continue;
		if (n.count >= 0) {
			for (int i = 0; i < n.count; i++) {
				int k2 = bvh.idx[n.start + i];
				const Tri &t = (*bvh.tris)[k2];
				VECTOR3 cq = tri_closest(p, t.a, t.e1, t.e2);
				VECTOR3 dv = p - cq; double dd = dotp(dv, dv);
				if (dd < best2) { best2 = dd; bq = cq; bt = k2; found = true; }
			}
		} else { st[sp++] = n.left; st[sp++] = n.start; }
	}
	if (!found) return false;
	q = bq; dout = sqrt(best2); tout = bt;
	return true;
}

// --------------------------------------------------------------
// Contact sample points: one per occupied cell of a regular grid, taken from
// triangle vertices and centroids. Grid dedup rather than every vertex keeps
// the count bounded on dense meshes -- a Delta-glider has 11893 triangles and
// would otherwise contribute 5891 Contact points.
// --------------------------------------------------------------
inline void sample_points(const std::vector<Tri> &tris, double cell, std::vector<VECTOR3> &pts)
{
	pts.clear();
	if (tris.empty() || cell <= 0.0) return;
	std::map<std::array<long long,3>, VECTOR3> grid;
	const double ic = 1.0 / cell;
	for (size_t i = 0; i < tris.size(); i++) {
		const Tri &t = tris[i];
		VECTOR3 v[4] = { t.a, t.a + t.e1, t.a + t.e2,
		                 t.a + (t.e1 + t.e2) * (1.0/3.0) };
		for (int q = 0; q < 4; q++) {
			std::array<long long,3> key = { (long long)llround(v[q].x * ic),
			                                (long long)llround(v[q].y * ic),
			                                (long long)llround(v[q].z * ic) };
			if (grid.find(key) == grid.end()) grid[key] = v[q];
		}
	}
	pts.reserve(grid.size());
	for (auto it = grid.begin(); it != grid.end(); ++it) pts.push_back(it->second);
}

} // namespace col
