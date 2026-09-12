// ==============================================================
// ColDamage.h -- structural damage: pieces come off where the ship was hit.
//
// Why the debris is not a vessel
//
// The obvious way to fly a broken-off piece away is to spawn a vessel for it.
// That cannot be done from inside this module: oapiCreateVessel resolves a class
// name only against $CONFIG/Vessels/ and $CONFIG/ (Vessel::OpenConfigFile), with
// no plugin-local search path, and a miss does not fail the call -- it reaches
// g_pOrbiter->TerminateOnError(). So debris-as-a-vessel means shipping a class
// config into Orbiter's shared vessel namespace, outside the add-on, with a
// fatal failure mode if it is ever removed. The add-on is one .so and nothing
// else, and that is worth more than the alternative buys.
//
// So a chunk stays a group of the parent's own mesh and the module flies it.
// Every frame the group's vertices are rewritten, through oapiEditMeshGroup on
// the device mesh, to put the piece where its own trajectory says it should be.
// It keeps the ship's textures, materials and lighting because it is still the
// ship's mesh. Nothing is created, nothing is installed, no file is touched.
//
// What that costs, honestly
//
// A Chunk is a visual object owned by this module, not a body Orbiter knows
// about. It does not collide with anything, it has no mass of its own, and it is
// culled with the parent. It drifts and tumbles correctly relative to the ship
// and then expires. For pieces coming off a hull that is the right trade; for
// Debris you can later fly into, it is not, and that would need the vessel route
// and a config file with it.
//
// The transform
//
// Orbiter renders a device-mesh vertex x at  P_p + R_p * (x + meshofs).
// The chunk should appear at its own pose (P_c, R_c), so for each original
// template vertex v (raw, before the mesh offset):
//
//     X = d + M * (v + meshofs - c0) - meshofs
//     M = R_p^T R_c        d = R_p^T (P_c - P_p)
//
// With c0 the chunk's centre in vessel coordinates. At the instant of
// detachment M is the identity and d is c0, which gives back x = v exactly --
// The piece does not jump when it lets go.
//
// Relative motion is integrated in a non-rotating frame riding on the parent's
// centre of mass, so the chunk keeps its own attitude while the ship manoeuvres
// around it, and gravity cancels between the two rather than being applied
// twice. The parent's own acceleration enters as -a_parent, which is what makes
// A piece fall behind a ship that is under thrust.
//
// What is broken off is also what is not collided
//
// m_hidden is the single source of truth. build_collider skips the groups it
// names, so the hole in the hull is a hole in the collider too. The visual side
// is re-asserted every frame anyway -- a device mesh is rebuilt whenever the
// Visual is, and a live chunk is rewritten each frame in any case.
// ==============================================================
#pragma once

#include "ColSolve.h"
#include <map>
#include <set>
#include <vector>
#include <utility>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace col {

// One breakable piece: a connected component of one group of one mesh.
//
// The group used to be the unit and a std::pair was enough. It is not the unit
// any more -- see GroupRef in ColSolve.h -- because on the ISS a group is a
// material and one material is the whole solar array.
struct PieceKey {
	UINT  mesh;
	DWORD grp;
	DWORD comp;
	PieceKey() : mesh(0), grp(0), comp(0) {}
	PieceKey(UINT m, DWORD g, DWORD c) : mesh(m), grp(g), comp(c) {}
	bool operator<(const PieceKey &o) const
	{
		if (mesh != o.mesh) return mesh < o.mesh;
		if (grp  != o.grp ) return grp  < o.grp;
		return comp < o.comp;
	}
};

// A removed piece keeps its vertex list, because removing it means collapsing
// those vertices rather than setting a group's do-not-render flag: UsrFlag is
// per group and a group now holds many pieces. Collapsing a component's
// vertices to one point makes its triangles degenerate, which draws nothing,
// and touches no other piece because components share no vertices.
typedef std::vector<WORD> VtxList;

// A piece that has come off and is still being drawn.
struct Chunk {
	OBJHANDLE parent;
	UINT      mesh;
	DWORD     grp;
	DWORD     comp;      // which component of that group
	VtxList   vidx;      // its own vertices -- the only ones this chunk moves
	VECTOR3   c0;        // chunk centre in vessel coordinates, at detachment
	VECTOR3   rpos;      // position relative to the parent's CoG, WORLD-oriented
	VECTOR3   rvel;      // and its rate
	MATRIX3   Rc;        // the chunk's own world orientation
	VECTOR3   omega;     // its tumble, WORLD
	double    t0;        // when it came off

	// Its own size, so that update() can fly it through the air rather than
	// coast it. See DBR_CD in ColTune.h.
	double    area;      // [m^2] its own surface
	double    mass;      // [kg]  its share of the hull it came off
};

// A Dent pushed into the hull at a contact.
//
// Vessel frame throughout -- `c` is comparable with a template vertex plus its
// mesh offset, which is the space build_collider already works in, so one
// displacement function serves both the picture and the collider and they
// cannot disagree.
struct Dent {
	UINT    mesh;     ///< which of the vessel's meshes
	VECTOR3 c;        ///< centre, vessel frame
	VECTOR3 dir;      ///< unit, pointing INTO the hull
	double  depth;    ///< [m] displacement at the centre
	double  radius;   ///< [m] falloff radius
};

// The falloff. (1 - t^2)^2 is zero in value and slope at the rim, so the dent
// melts into undisturbed hull instead of creasing a ring around itself -- which
// is what a linear or cosine falloff does on a flat painted panel, and it reads
// as a crater edge rather than a dent.
inline double dent_weight(double d, double radius)
{
	if (radius <= 0.0 || d >= radius) return 0.0;
	const double t = d / radius;
	const double s = 1.0 - t * t;
	return s * s;
}


// One burst of impact sparks: an Orbiter particle stream that is switched on
// at a contact point, faded out over SPK_LIFE and deleted.
//
// The level is a member and the pool is a fixed array, and that is not an
// accident. Vessel::AddParticleStream takes `double *lvl` and the core keeps
// that pointer, reading it every frame for as long as the stream lives. Put
// these in a std::vector and the first reallocation moves the doubles, leaving
// the core dereferencing freed memory -- the same defect as a dangling texture,
// one layer up. A fixed array never moves, so the pointer handed out at
// creation is valid until DelExhaustStream.
struct Burst {
	OBJHANDLE      parent;
	PSTREAM_HANDLE ps;
	double         level;      ///< the core holds a pointer to THIS
	double         t0;
	VECTOR3        pos;        ///< where on the hull, vessel frame -- see SPK_SITE
	bool           used;
};

class Damage {
public:
	Damage() : m_nbroken(0), m_ndent(0), m_peakQ(0.0), m_peakH(0.0),
	           m_glowSaid(false)
	{
		for (size_t i = 0; i < SPK_MAX; i++) {
			m_spark[i].used   = false;
			m_spark[i].ps     = 0;
			m_spark[i].parent = 0;
			m_spark[i].level  = 0.0;
			m_spark[i].t0     = 0.0;
			m_spark[i].pos    = _V(0,0,0);
		}
	}

	void clear()
	{
		m_hidden.clear(); m_chunk.clear(); m_nbroken = 0;
		m_dent.clear(); m_vis.clear(); m_ndent = 0;
		m_aeroT.clear(); m_peakQ = m_peakH = 0.0;
		m_glow.clear(); m_glowSaid = false;
		spark_clear();
	}

	// ---- hull deformation -------------------------------------------------

	/// \brief record a dent. Returns true if one was actually made, which means
	///        the parent's collider is now stale.
	bool dent(OBJHANDLE h, VESSEL *v, const Collider &c, const VECTOR3 &p_local,
	          const VECTOR3 &n_into, double energy);

	bool any_dent(OBJHANDLE h) const
	{ return m_dent.find(h) != m_dent.end(); }

	long dents() const { return m_ndent; }

	/// \brief where a hull point has been pushed to. Vessel frame in, vessel
	///        frame out; the identity for a vessel with no dents.
	///
	///        build_collider calls this on every triangle vertex and the
	///        renderer calls it on every vertex it rewrites, so the collider
	///        and the picture are the same surface by construction rather than
	///        by two implementations agreeing.
	VECTOR3 deform(OBJHANDLE h, UINT mesh, const VECTOR3 &p) const
	{
		std::map<OBJHANDLE, std::vector<Dent> >::const_iterator it = m_dent.find(h);
		if (it == m_dent.end()) return p;

		VECTOR3 q = p;
		double got = 0.0;
		for (size_t i = 0; i < it->second.size(); i++) {
			const Dent &d = it->second[i];
			if (d.mesh != mesh) continue;
			const double w = dent_weight(length(p - d.c), d.radius);
			if (w <= 0.0) continue;
			// Summed, then clamped as a whole: overlapping hits deepen the
			// same dish instead of each pushing the skin through the hull.
			double s = d.depth * w;
			if (got + s > DENT_MAX_SUM) s = DENT_MAX_SUM - got;
			if (s <= 0.0) continue;
			got += s;
			q = q + d.dir * s;
		}
		return q;
	}

	/// \brief push every recorded dent into the vessel's device meshes.
	void reshape(OBJHANDLE h, VESSEL *v) const;

	// ---- reentry breakup --------------------------------------------------

	/// \brief one aerodynamic-stress test for one vessel. Returns true if
	///        something came off, which means the collider is stale.
	///
	///        Same shape as DeltaGlider::TestDamage -- see ColTune.h. The
	///        stress decides a rate, the rate becomes a per-step probability,
	///        and a roll decides. Nothing happens in vacuum or below the
	///        limits.
	bool aero(OBJHANDLE h, VESSEL *v, const Collider &c, double simt, double simdt);

	/// \brief worst dynamic pressure and heating seen so far, for the log.
	double peak_q() const    { return m_peakQ; }
	double peak_heat() const { return m_peakH; }

	/// \brief make the vessel -- hull, and every piece that has come off it --
	///        Glow with its own aerodynamic heating.
	///
	///        Called once per vessel per frame and NOT gated on the damage
	///        model: a ship glowing on entry is not damage, and a nominal
	///        entry that loses nothing should still come in orange. Cheap on
	///        an unheated vessel: two accessors and a comparison.
	void glow(OBJHANDLE h, VESSEL *v);

	// Throw sparks from a contact. p_local and n_local are in the vessel's own
	// frame, n pointing away from whatever it hit. Not gated on the damage
	// model -- see the note in ColTune.h.
	void spark(VESSEL *v, OBJHANDLE h, const VECTOR3 &p_local,
	           const VECTOR3 &n_local, double simt);

	// Fade the live bursts and delete the finished ones.
	void spark_update(double simt);

	void spark_clear();

	void forget(OBJHANDLE h) { m_hidden.erase(h); }

	bool any_hidden(OBJHANDLE h) const
	{ return m_hidden.find(h) != m_hidden.end(); }

	bool is_hidden(OBJHANDLE h, UINT mesh, DWORD grp, DWORD comp) const
	{
		std::map<OBJHANDLE, std::map<PieceKey, VtxList> >::const_iterator it = m_hidden.find(h);
		if (it == m_hidden.end()) return false;
		return it->second.find(PieceKey(mesh, grp, comp)) != it->second.end();
	}

	long   broken() const { return m_nbroken; }
	size_t live() const   { return m_chunk.size(); }

	/// \brief the pieces in flight right now. Read-only -- update() owns them.
	///        Present so that the debris can be inspected outside the
	///        simulator, where there is no visual to look at: an offline
	///        harness can record where every piece went and check it against
	///        what the device-mesh edits did.
	const std::vector<Chunk> &chunks() const { return m_chunk; }

	// Is this group in the air right now?
	//
	// A group is in m_hidden from the instant it breaks off, because that set
	// is what takes it out of the collider -- build_collider skips it, so the
	// hole in the hull is a hole you can fly through immediately. But it must
	// still be drawn for as long as its piece is flying: the piece is that
	// group, moved out of place by update().
	//
	// Those two meanings were the same set and the same answer, and enforce()
	// read it as "do not render". So every chunk was flown correctly and
	// hidden in the same frame -- the nose came off and vanished on the spot
	// instead of tumbling away. Rendering is now the narrower question, and
	// this is what narrows it.
	bool flying(OBJHANDLE h, UINT mesh, DWORD grp, DWORD comp) const
	{
		for (size_t i = 0; i < m_chunk.size(); i++) {
			const Chunk &c = m_chunk[i];
			if (c.parent == h && c.mesh == mesh && c.grp == grp && c.comp == comp)
				return true;
		}
		return false;
	}

	// Re-assert the do-not-render bit on a vessel's visual, for pieces that have
	// finished flying. A device mesh is rebuilt with the visual, and a rebuilt
	// one has the template's flags again.
	void enforce(OBJHANDLE h, VESSEL *v) const;

	// One contact deposited `energy` joules into the hull at `p_local`.
	// Returns true if the parent's collider is now stale.
	//
	// `tear` is 0 for a collision and (0,1] for a structural failure -- see
	// the AERO_TEAR block in ColTune.h. It changes two things and nothing
	// else: how big a piece may come away, and which pieces are preferred.
	bool hit(OBJHANDLE h, VESSEL *v, const Collider &c, const BodyRef &b,
	         const VECTOR3 &p_local, double energy, double simt,
	         double tear = 0.0, const VECTOR3 *wind = 0);

	// Fly every live chunk and write it into its parent's visual.
	void update(double simt, double simdt);

private:
	/// \brief remove a piece from the picture by collapsing its own vertices to
	///        A point. Degenerate triangles draw nothing, and no other piece
	///        shares those vertices. Replaces the old per-group UsrFlag.
	static void collapse(VESSEL *v, VISHANDLE vis, UINT mesh, DWORD grp,
	                     const VtxList &vidx);

	std::map<OBJHANDLE, std::map<PieceKey, VtxList> > m_hidden;
	std::vector<Chunk> m_chunk;
	long m_nbroken;
	Burst m_spark[SPK_MAX];      ///< fixed storage -- see the note on Burst

	std::map<OBJHANDLE, std::vector<Dent> > m_dent;
	// The visual each vessel's dents were last written into. A device mesh is
	// rebuilt with its visual and comes back with the TEMPLATE's vertices, so
	// the dents have to go in again -- and comparing the handle is what says
	// when, instead of rewriting every hull every frame.
	mutable std::map<OBJHANDLE, VISHANDLE> m_vis;
	long m_ndent;

	// Reentry breakup: when each vessel was last tested, and the worst stress
	// any of them has seen. The peaks exist so a normal entry can be flown and
	// the numbers read before the thresholds are argued about.
	std::map<OBJHANDLE, double> m_aeroT;
	double m_peakQ, m_peakH;

	// Incandescence. `lvl` is where the ramp is now, `put` is where it was when
	// the materials were last written, and `vis` is the visual they were
	// written into -- a device mesh is rebuilt with its visual and comes back
	// with the TEMPLATE's materials, so a rebuild has to be noticed and
	// answered exactly as reshape() answers it for dents.
	struct Glow { double lvl, put; VISHANDLE vis; };
	std::map<OBJHANDLE, Glow> m_glow;
	bool m_glowSaid;
};

// --------------------------------------------------------------

// --------------------------------------------------------------
// Hull deformation
// --------------------------------------------------------------

inline bool Damage::dent(OBJHANDLE h, VESSEL *v, const Collider &c,
                         const VECTOR3 &p_local, const VECTOR3 &n_into,
                         double energy)
{
	if (!v || energy < DENT_MIN_E) return false;

	const double nl = length(n_into);
	if (nl < 1e-6) return false;

	double depth = energy / DENT_ENERGY;
	if (depth > DENT_MAX_DEPTH) depth = DENT_MAX_DEPTH;
	if (depth <= 1e-3) return false;

	double radius = DENT_RADIUS_K * depth;
	if (radius < DENT_RADIUS_MIN) radius = DENT_RADIUS_MIN;
	const double rcap = DENT_RADIUS_MAX * c.radius;
	if (rcap > 0.0 && radius > rcap) radius = rcap;

	// Which mesh owns the contact. The point is in the vessel frame and each
	// mesh sits at its own offset, so the dent belongs to whichever mesh has
	// geometry nearest it -- picking mesh 0 would dent the hull when the strike
	// landed on a separately-offset part.
	UINT best = 0;
	double bestd = 1e30;
	const UINT nm = v->GetMeshCount();
	for (UINT i = 0; i < nm; i++) {
		WORD vis = v->GetMeshVisibilityMode(i);
		if (!(vis & MESHVIS_EXTERNAL)) continue;
		VECTOR3 ofs;
		if (!v->GetMeshOffset(i, ofs)) ofs = _V(0,0,0);
		const double d = length(p_local - ofs);
		if (d < bestd) { bestd = d; best = i; }
	}

	std::vector<Dent> &list = m_dent[h];

	// The oldest goes when the list is full. A hull that has been hit thirty
	// times is already a wreck; what matters is that the newest mark shows.
	if (list.size() >= DENT_MAX) list.erase(list.begin());

	Dent d;
	d.mesh   = best;
	d.c      = p_local;
	d.dir    = n_into * (1.0 / nl);
	d.depth  = depth;
	d.radius = radius;
	list.push_back(d);
	m_ndent++;

	// Force a rewrite on the next reshape() even though the visual has not
	// changed -- see m_vis.
	m_vis.erase(h);
	return true;
}


// Write the dents into the vessel's device meshes.
//
// Absolute, from the template, never additive. GRPEDIT_VTXCRDADD exists and is
// the wrong tool here: the device mesh is rebuilt from the template whenever
// its visual is, so an additive edit is lost on a rebuild and applied twice if
// it is not. Reading the pristine template and writing the final position makes
// the operation idempotent, which is what lets it simply be repeated whenever
// the visual changes.
//
// Normals are recomputed from the deformed geometry and then blended in by the
// same weight as the displacement. Recomputing outright would throw away the
// mesh's authored normals -- Orbiter meshes carry deliberate hard and soft
// edges, and a flat-shaded panel re-averaged from its triangles stops matching
// its neighbours. Blending leaves undisturbed hull exactly as the artist left
// it and only takes geometric normals where the surface has actually moved.
inline void Damage::reshape(OBJHANDLE h, VESSEL *v) const
{
	std::map<OBJHANDLE, std::vector<Dent> >::const_iterator it = m_dent.find(h);
	if (it == m_dent.end() || it->second.empty() || !v) return;

	VISHANDLE *pv = oapiObjectVisualPtr(h);
	if (!pv || !*pv) return;                  // not being drawn

	std::map<OBJHANDLE, VISHANDLE>::const_iterator vs = m_vis.find(h);
	if (vs != m_vis.end() && vs->second == *pv) return;   // already in this visual
	m_vis[h] = *pv;

	const UINT nm = v->GetMeshCount();
	std::vector<VECTOR3> pos, nml;
	std::vector<NTVERTEX> out;
	std::vector<WORD>     idx;

	for (UINT m = 0; m < nm; m++) {

		bool any = false;
		for (size_t i = 0; i < it->second.size() && !any; i++)
			if (it->second[i].mesh == m) any = true;
		if (!any) continue;

		DEVMESHHANDLE dm = v->GetDevMesh(*pv, m);
		const MESHHANDLE tpl = v->GetMeshTemplate(m);
		if (!dm || !tpl) continue;

		VECTOR3 ofs;
		if (!v->GetMeshOffset(m, ofs)) ofs = _V(0,0,0);

		const DWORD ng = oapiMeshGroupCount(tpl);
		for (DWORD g = 0; g < ng; g++) {

			// Pieces of this group that have broken off are not there to dent;
			// their vertices are collapsed and reshape() must not move them
			// back out. Checked per vertex below via the removed set.
			// (A group is only skipped entirely if every piece of it is gone,
			// which the per-vertex test covers for free.)

			MESHGROUP *mg = oapiMeshGroup(tpl, g);
			if (!mg || !mg->Vtx || !mg->nVtx) continue;

			// ---- deformed positions, and which vertices actually moved ----
			// Vertices belonging to a removed or flying piece are off limits:
			// Writing a dented position over a collapsed vertex would put the
			// piece back, and over a flying one would fight update().
			std::set<WORD> off;
			{
				std::map<OBJHANDLE, std::map<PieceKey, VtxList> >::const_iterator
					hit2 = m_hidden.find(h);
				if (hit2 != m_hidden.end())
					for (std::map<PieceKey, VtxList>::const_iterator k = hit2->second.begin();
					     k != hit2->second.end(); ++k)
						if (k->first.mesh == m && k->first.grp == g)
							off.insert(k->second.begin(), k->second.end());
			}

			pos.resize(mg->nVtx);
			bool moved = false;
			for (DWORD k = 0; k < mg->nVtx; k++) {
				const NTVERTEX &s = mg->Vtx[k];
				const VECTOR3 p = _V(s.x, s.y, s.z) + ofs;
				pos[k] = deform(h, m, p);
				if (!moved && length(pos[k] - p) > 1e-6) moved = true;
			}
			if (!moved) continue;

			// ---- geometric normals of the deformed surface ----
			nml.assign(mg->nVtx, _V(0,0,0));
			if (mg->Idx) {
				for (DWORD k = 0; k + 2 < mg->nIdx; k += 3) {
					const WORD a = mg->Idx[k], b = mg->Idx[k+1], cc = mg->Idx[k+2];
					if (a >= mg->nVtx || b >= mg->nVtx || cc >= mg->nVtx) continue;
					const VECTOR3 fn = crossp(pos[b] - pos[a], pos[cc] - pos[a]);
					nml[a] = nml[a] + fn;
					nml[b] = nml[b] + fn;
					nml[cc] = nml[cc] + fn;
				}
			}

			// ---- write back only the vertices that moved ----
			out.clear();
			idx.clear();
			for (DWORD k = 0; k < mg->nVtx; k++) {
				const NTVERTEX &s = mg->Vtx[k];
				const VECTOR3 p = _V(s.x, s.y, s.z) + ofs;
				const VECTOR3 q = pos[k];
				const VECTOR3 dv = q - p;
				const double  dl = length(dv);
				if (dl <= 1e-6) continue;
				if (off.find((WORD)k) != off.end()) continue;   // gone or flying

				NTVERTEX t = s;
				const VECTOR3 r = q - ofs;          // back into mesh coordinates
				t.x = (float)r.x; t.y = (float)r.y; t.z = (float)r.z;

				// Blend weight: how far this vertex moved, against the deepest
				// any vertex is allowed to move. Cheap, bounded, and it tracks
				// the dent profile because the displacement does.
				double w = dl / DENT_MAX_DEPTH;
				if (w > 1.0) w = 1.0;

				const double gl = length(nml[k]);
				if (gl > 1e-12) {
					const VECTOR3 gn = nml[k] * (1.0 / gl);
					VECTOR3 nn = _V(s.nx, s.ny, s.nz) * (1.0 - w) + gn * w;
					const double l = length(nn);
					if (l > 1e-12) {
						nn = nn * (1.0 / l);
						t.nx = (float)nn.x; t.ny = (float)nn.y; t.nz = (float)nn.z;
					}
				}

				out.push_back(t);
				idx.push_back((WORD)k);
			}
			if (out.empty()) continue;

			GROUPEDITSPEC ges;
			ges.flags   = GRPEDIT_VTXCRD | GRPEDIT_VTXNML;
			ges.UsrFlag = 0;
			ges.Vtx     = &out[0];
			ges.nVtx    = (DWORD)out.size();
			ges.vIdx    = &idx[0];
			oapiEditMeshGroup(dm, g, &ges);
		}
	}
}


// --------------------------------------------------------------
// Reentry breakup
//
// The stress test is DeltaGlider::TestDamage's, carried across unchanged; what
// it drives is different. There, a failure distorts the airfoil and trips the
// master warning. Here it tears actual pieces off the windward side, through
// the same hit() every collision uses -- so a ship that comes apart on entry
// comes apart the same way a ship that hits a station does, and there is one
// breakup model rather than two.
// --------------------------------------------------------------

inline bool Damage::aero(OBJHANDLE h, VESSEL *v, const Collider &c,
                         double simt, double simdt)
{
	if (!v || simdt <= 0.0) return false;

	// Nothing in vacuum. GetDynPressure is already zero there, but the
	// airspeed vector below is meaningless too, so leave early.
	const double rho = v->GetAtmDensity();
	if (rho <= 0.0) return false;

	// Tested on an interval, not every frame. The Poisson term below is what
	// makes the outcome frame-rate independent; this only keeps the hull scan
	// off every step. dt for the probability is the real elapsed time since
	// the last test, so skipping frames does not skip risk.
	double dt = simdt;
	{
		std::map<OBJHANDLE, double>::iterator it = m_aeroT.find(h);
		if (it != m_aeroT.end()) {
			dt = simt - it->second;
			if (dt < AERO_INTERVAL) return false;
			it->second = simt;
		}
		else { m_aeroT[h] = simt; return false; }   // first sighting: start the clock
	}
	if (dt <= 0.0) return false;

	const double dynp = v->GetDynPressure();
	const double vair = v->GetAirspeed();

	// Sutton-Graves shape, scaled for readability. See ColTune.h.
	const double heat = sqrt(rho) * vair * vair * vair * 1e-9;

	if (dynp > m_peakQ) m_peakQ = dynp;
	if (heat > m_peakH) m_peakH = heat;

	// The entry profile, while it is happening.
	//
	// AERO_HEAT_MAX is the one number here with no counterpart in the
	// reference to inherit -- the heating term is an addition -- so it has to
	// be set against a real entry rather than argued about. This prints the
	// track from a quarter-load upwards, which is what a survivable entry
	// looks like and what an unsurvivable one exceeds.
	if (getenv("ORBITER_TRACE_AERO")
	    ? (rho > 0.0)
	    : (dynp > AERO_DYNP_MAX * 0.25 || heat > AERO_HEAT_MAX * 0.25)) {
		static double lastT = -1e9;
		if (simt - lastT > 2.0) {
			lastT = simt;
			char buf[240];
			snprintf(buf, sizeof(buf),
				"CollisionDetection: aero stress '%s' -- q %.0f kPa (%.0f%%), "
				"heat %.2f (%.0f%%), alt %.1f km, mach %.1f",
				v->GetName(), dynp * 1e-3, 100.0 * dynp / AERO_DYNP_MAX,
				heat, 100.0 * heat / AERO_HEAT_MAX,
				v->GetAltitude() * 1e-3, v->GetMachNumber());
			oapiWriteLog(buf);
		}
	}

	if (dynp <= AERO_DYNP_MAX && heat <= AERO_HEAT_MAX) return false;

	// The reference's own arithmetic: excess over the limit sets a rate, the
	// worst of the stresses wins, and the rate becomes a probability for this
	// interval.
	const double aq = (dynp > AERO_DYNP_MAX) ? (dynp - AERO_DYNP_MAX) * AERO_DYNP_K : 0.0;
	const double ah = (heat > AERO_HEAT_MAX) ? (heat - AERO_HEAT_MAX) * AERO_HEAT_K : 0.0;
	const double alpha = (aq > ah) ? aq : ah;
	if (alpha <= 0.0) return false;

	const double p = 1.0 - exp(-alpha * dt);
	if (oapiRand() >= p) return false;

	// ---- something let go. Decide where, and how much. -------------------
	//
	// The windward face, not a random spot. The ship flies along its airspeed
	// vector, so the air arrives on the surface facing that way and that is
	// what burns and tears first. Pushing the point out to the hull radius and
	// letting hit() take the nearest pieces to it is what makes entry damage
	// eat the leading edge instead of pitting the ship all over.
	VECTOR3 av;
	if (!v->GetAirspeedVector(FRAME_LOCAL, av)) return false;
	const double al = length(av);
	if (al < 1e-6) return false;

	// On the hull, not on the bounding sphere.
	//
	// This was `av * (c.radius / al)` alone, and that is a point out on the
	// bounding sphere along the airflow -- which is only near the ship if the
	// ship is round. Hit() then looks for pieces within DMG_RADIUS_MAX of it,
	// 0.45 of the radius, and on a wide flat vehicle nothing is within reach:
	// The space Shuttle at 40 degrees angle of attack puts that point 13 m
	// below an airframe whose belly is 3 m below the axis, so every candidate
	// is filtered out and the entry silently takes nothing off. Measured --
	// Atlantis at 157% of the heating limit for sixteen seconds, dozens of
	// failure rolls passing, and not one piece lost.
	//
	// The nearest triangle to that point is where the air actually arrives, so
	// take it. One BVH query, and the reach cannot exceed the bounding sphere
	// by construction. The Delta-glider is compact enough that this barely
	// moves its point, which is why the defect did not show up there.
	VECTOR3 p_local = av * (c.radius / al);
	{
		VECTOR3 cq; double d; int ti;
		if (closest_pt(c.bvh, p_local, 2.0 * c.radius, cq, d, ti)) p_local = cq;
	}

	// How much comes off scales with how far past the limit it is, so a
	// marginal entry sheds trim and a hopeless one comes apart.
	//
	// Sized against the airframe, not in absolute joules. The failure is worth
	// A share of the vessel's own hull area over this interval, which is what
	// makes one constant serve a 649 m2 glider and a 2344 m2 Shuttle. See the
	// AERO_TEAR block in ColTune.h for the version this replaces and why it
	// could not break a ship.
	const double over = alpha / (AERO_DYNP_MAX * AERO_DYNP_K);

	double E = DMG_ENERGY * c.area * AERO_TEAR_RATE * over * dt;
	if (E < AERO_E_MIN) E = AERO_E_MIN;
	if (E < DMG_MIN_E)  E = DMG_MIN_E;

	// And how structural it is.
	// 0 is a collision-shaped nibble at the skin, 1 is an assembly letting go,
	// and past 1 is the primary structure going with it -- see AERO_TEAR_MAX.
	// This used to clamp at 1, which is what put a ceiling on how thoroughly
	// any entry could take a ship apart.
	double tear = over / AERO_TEAR_FULL;
	if (tear > AERO_TEAR_MAX) tear = AERO_TEAR_MAX;
	if (tear < 1e-3) tear = 1e-3;   // never 0 -- that would mean "collision"

	BodyRef b;
	b.v = v;
	v->GetGlobalPos(b.pos);
	v->GetGlobalVel(b.vel);
	v->GetAngularVel(b.omega);
	v->GetRotationMatrix(b.R);
	b.mass = v->GetMass();
	b.size = v->GetSize();

	// The flow direction in the vessel's own frame, so that hit() can ask each
	// piece whether the air can actually reach it.
	const VECTOR3 wind = av * (1.0 / al);

	const long was = m_nbroken;
	const bool stale = hit(h, v, c, b, p_local, E, simt, tear, &wind);

	// Particles at the tear. A structure letting go at Mach 30-odd does not
	// do it quietly.
	//
	// The bursts are thrown by hit(), one per piece, not here. This used to be
	// A single spark() call on the windward point after the fact, which marked
	// A whole airframe coming apart with one 0.1 m source; hit() knows where
	// every piece it took was standing, so that is where the sparks belong.
	// `wind` is passed down for it, and the note below is about what hit()
	// then does with it.
	//
	// Emitted along -wind, i.e. Aft: in the vessel's frame the air is moving
	// from nose to tail, so anything shed streams back into the wake rather
	// than standing off the hull.
	//
	// The sign survives two negations and is right. Vessel::AddParticleStream
	// hands clbkCreateExhaustStream MakeVECTOR3(-dir), and ExhaustStream::Update
	// then emits along mul(vR,*dir) * (-speed) -- because for a thruster `dir` is
	// the thrust direction and the exhaust goes the other way. The two cancel, so
	// A Particle leaves at +SPK_V0 along the vector passed in here. Do not
	// "fix" it.
	//
	// The particles inherit the ship's velocity and are then slowed against the
	// air by the stream's own integrator, so they fall behind on their own. That
	// needs SPK_SLOWDOWN non-zero: at 0 the integrator's exponential is switched
	// off and sparks hold the ship's speed while the ship brakes, drifting
	// forward out of the nose.
	//
	// The vehicle's overall plasma is not this and never was: the core gives
	// every vessel one from Vessel::SetDefaultReentryStream, through
	// clbkCreateReentryStream, which both clients implement.

	if (m_nbroken != was) {
		char buf[260];
		snprintf(buf, sizeof(buf),
			"CollisionDetection: REENTRY BREAKUP on '%s' -- q %.0f kPa (limit %.0f), "
			"heat %.2f (limit %.2f), %ld piece(s) off the windward side",
			v->GetName(), dynp * 1e-3, AERO_DYNP_MAX * 1e-3,
			heat, AERO_HEAT_MAX, m_nbroken - was);
		oapiWriteLog(buf);
	}
	return stale;
}


// --------------------------------------------------------------
// Incandescence
//
// One emissive level for the whole airframe, driven by the same heating number
// aero() tests against, written into the device mesh materials. See the GLOW_
// block in ColTune.h for why it cannot be per-piece and why that is the right
// answer anyway.
//
// The template is read and never written. Each material is rebuilt from the
// Vessel's own pristine template every time and the sum is written to the
// device copy, exactly as reshape() does for dents. That makes the operation
// idempotent -- repeating it cannot compound -- and it means level 0 restores
// the artist's material rather than an approximation of it. Nothing in the
// Vessel's folder is touched, and no other vessel of the same class is
// affected, because the device mesh belongs to one visual.
// --------------------------------------------------------------

inline void Damage::glow(OBJHANDLE h, VESSEL *v)
{
	if (!v) return;

	// ---- where on the ramp is it ----
	//
	// The heating proxy, identical to aero()'s. Vacuum is zero by
	// construction, so a ship in orbit runs the two accessors and the
	// comparison below and does nothing else.
	const double rho = v->GetAtmDensity();
	double lvl = 0.0;
	if (rho > 0.0 && AERO_HEAT_MAX > 0.0) {
		const double vair = v->GetAirspeed();
		const double heat = sqrt(rho) * vair * vair * vair * 1e-9;
		const double f    = heat / AERO_HEAT_MAX;
		lvl = (f - GLOW_ON) / (GLOW_FULL - GLOW_ON);
		if (lvl < 0.0) lvl = 0.0;
		if (lvl > 1.0) lvl = 1.0;
	}

	std::map<OBJHANDLE, Glow>::iterator it = m_glow.find(h);
	if (it == m_glow.end()) {
		if (lvl <= 0.0) return;             // cold and never written: nothing to do
		Glow g; g.lvl = lvl; g.put = -1.0; g.vis = 0;
		it = m_glow.insert(std::make_pair(h, g)).first;
	}
	it->second.lvl = lvl;

	VISHANDLE *pv = oapiObjectVisualPtr(h);
	if (!pv || !*pv) return;                // not being drawn

	// A rebuilt visual has the template's materials back, so it needs writing
	// again whatever the level is doing -- including a level of 0, which is
	// already what the template says and so costs one comparison below.
	const bool newvis = (it->second.vis != *pv);
	// Going out is always written, however small the last step was. Otherwise a
	// ship that cools from just inside GLOW_STEP of zero keeps a sliver of
	// emissive for the rest of the session, and "level 0 restores the artist's
	// material" stops being true.
	const bool off = (lvl <= 0.0 && it->second.put > 0.0);
	if (!newvis && !off && fabs(lvl - it->second.put) < GLOW_STEP) return;
	if (!newvis && lvl <= 0.0 && it->second.put <= 0.0) return;

	// ---- the colour ----
	//
	// Dull red into white, and the amount grows with the ramp as well, so it
	// fades up out of the unlit material instead of switching on at GLOW_ON.
	const double amt = GLOW_MAX * lvl;
	const float  er  = (float)(amt * (GLOW_RED[0] + (GLOW_WHITE[0] - GLOW_RED[0]) * lvl));
	const float  eg  = (float)(amt * (GLOW_RED[1] + (GLOW_WHITE[1] - GLOW_RED[1]) * lvl));
	const float  eb  = (float)(amt * (GLOW_RED[2] + (GLOW_WHITE[2] - GLOW_RED[2]) * lvl));

	int wrote = 0;
	const UINT nm = v->GetMeshCount();
	for (UINT m = 0; m < nm; m++) {
		// Only the exterior. The same test build_collider uses, and for the
		// same reason: the virtual cockpit is not part of the hull, and it
		// should not light up because the outside of the ship is on fire.
		if (!(v->GetMeshVisibilityMode(m) & MESHVIS_EXTERNAL)) continue;

		DEVMESHHANDLE dm = v->GetDevMesh(*pv, m);
		const MESHHANDLE tpl = v->GetMeshTemplate(m);
		if (!dm || !tpl) continue;

		const DWORD nmat = oapiMeshMaterialCount(tpl);
		for (DWORD i = 0; i < nmat; i++) {
			const MATERIAL *base = oapiMeshMaterial(tpl, i);
			if (!base) continue;
			MATERIAL hot = *base;
			hot.emissive.r = base->emissive.r + er;
			hot.emissive.g = base->emissive.g + eg;
			hot.emissive.b = base->emissive.b + eb;
			if (oapiSetMaterial(dm, i, &hot) == 0) wrote++;
		}
	}

	it->second.put = lvl;
	it->second.vis = *pv;

	// Whether the renderer took it, said once. oapiSetMaterial returns 1 with
	// no graphics engine attached and 2 when the client does not implement the
	// call, and both of those look exactly like a glow that is simply too dim.
	// The answer is a number in the log rather than an evening of nudging
	// GLOW_MAX.
	if (!m_glowSaid && lvl > 0.0) {
		m_glowSaid = true;
		char buf[240];
		if (wrote)
			snprintf(buf, sizeof(buf),
				"CollisionDetection: reentry heating is now lighting '%s' -- %d "
				"material(s) made emissive, level %.2f of 1.00 (GLOW_MAX %.2f)",
				v->GetName(), wrote, lvl, GLOW_MAX);
		else
			snprintf(buf, sizeof(buf),
				"CollisionDetection: '%s' is hot enough to glow but no material "
				"could be set -- the graphics client did not accept "
				"oapiSetMaterial, so the hull and its debris will stay grey.",
				v->GetName());
		oapiWriteLog(buf);
	}
}

// --------------------------------------------------------------
// Impact sparks
//
// One burst is one particle stream, switched on at the contact and faded out.
// The spec is EMISSIVE with the stock texture (tex = NULL), so nothing is added
// to any mesh or add-on.
//
// Atmsmap = ATM_FLAT matters and is not a default. The other two mappings scale
// A stream's opacity by atmospheric density, which is right for a re-entry
// trail and fatal here: every impact this addon exists to show happens in
// vacuum, where those mappings render nothing at all.
// --------------------------------------------------------------

inline void Damage::spark(VESSEL *v, OBJHANDLE h, const VECTOR3 &p_local,
                          const VECTOR3 &n_local, double simt)
{
	if (!v) return;

	// A gap before the same place sparks again. A sustained scrape then reads
	// as a shower of bursts rather than one endless jet.
	//
	// The distance test is not decoration. This used to be a gap per vessel,
	// which is right for a scrape -- one contact travelling along one hull --
	// And wrong for the thing it now has to survive: a structural failure that
	// takes eighteen pieces off eighteen different parts of the windward side
	// in the same step. With the old guard, seventeen of those returned here
	// and a whole airframe letting go was marked by one 0.1 m spark source.
	// Measured on Atlantis: eleven bursts over a fourteen-second breakup that
	// shed a hundred and sixteen pieces, and nothing visible on screen.
	//
	// Two tears further apart than SPK_SITE are two places, and each gets its
	// own burst. The pool and SPK_MAX still bound the total.
	for (size_t i = 0; i < SPK_MAX; i++)
		if (m_spark[i].used && m_spark[i].parent == h &&
		    simt - m_spark[i].t0 < SPK_REARM &&
		    length(m_spark[i].pos - p_local) < SPK_SITE) return;

	size_t slot = SPK_MAX;
	for (size_t i = 0; i < SPK_MAX; i++)
		if (!m_spark[i].used) { slot = i; break; }
	if (slot == SPK_MAX) return;                  // all busy; the next hit will

	double nl = length(n_local);
	VECTOR3 dir = (nl > 1e-6) ? n_local * (1.0 / nl) : _V(0, 1, 0);

	static PARTICLESTREAMSPEC pss = {
		0,                              // flags
		SPK_SIZE,                       // srcsize
		SPK_RATE,                       // srcrate
		SPK_V0,                         // v0
		SPK_SPREAD,                     // srcspread
		SPK_PLIFE,                      // lifetime
		0.0,                            // growthrate -- a spark does not bloom
		SPK_SLOWDOWN,                   // atmslowdown -- WAS 0.0. See ColTune.h.
		PARTICLESTREAMSPEC::EMISSIVE,
		PARTICLESTREAMSPEC::LVL_LIN,    // alpha follows the level we drive
		0.0, 1.0,
		PARTICLESTREAMSPEC::ATM_FLAT,   // see the note above
		1.0, 1.0,                       // <- amin. NOT 0. See below.
		NULL                            // stock particle texture
	};

	// The amin above was 0.0, and that alone is why this has never drawn
	// anything.
	//
	// The emitter's gate is
	//
	//     Alpha0 = Level2Alpha(*level) * Atm2Alpha(Vessel->GetAtmDensity())
	//     if (level && *level > 0 && alpha0 > 0.01) { ...create particles... }
	//
	// And Atm2Alpha for ATM_FLAT returns amin, ignoring the density entirely
	// -- that is what flat means. With amin at 0 the product is 0, the gate
	// never opens, and not one particle is ever created no matter how hard
	// anything is hit. Every stock vessel that uses ATM_FLAT sets a non-zero
	// amin: Atlantis and its SRB use 1,1 and the Delta-glider's pressure
	// subsystem uses 0.1,0.1.

	Burst &b = m_spark[slot];
	b.parent = h;
	b.t0     = simt;
	b.pos    = p_local;
	b.level  = 1.0;
	b.used   = true;
	// &b.level is handed to the core and read every frame until the stream is
	// deleted. It is stable because m_spark is a fixed array.
	b.ps = v->AddParticleStream(&pss, p_local, dir, &b.level);

	if (!b.ps) {
		// This used to say the client could not make one. That was wrong
		// twice, and it cost three sessions of believing sparks were
		// impossible.
		//
		// It said Vessel::AddParticleStream routes to
		// clbkCreateParticleStream, which is indeed unimplemented in both
		// clients. It does not. Vessel::AddParticleStream calls
		// clbkCreateExhaustStream, which is implemented in both -- the client
		// builds a real ExhaustStream and hands it to the scene. The name is
		// the trap: AddParticleStream and clbkCreateParticleStream have
		// nothing to do with each other.
		//
		// So a null here is not a missing renderer. There is exactly one way
		// to get one, and it is the only thing Vessel::AddParticleStream
		// checks before it does anything else.
		static bool bSaid = false;
		if (!bSaid) {
			bSaid = true;
			oapiWriteLog((char *)"CollisionDetection: impact sparks are off because "
			                     "particle streams are disabled -- tick \"Particle "
			                     "streams\" in the Launchpad's Visual effects tab. "
			                     "Debris and dents are unaffected.");
		}
		b.used = false;
	}
}

inline void Damage::spark_update(double simt)
{
	for (size_t i = 0; i < SPK_MAX; i++) {
		Burst &b = m_spark[i];
		if (!b.used) continue;

		const double a = (simt - b.t0) / SPK_LIFE;
		if (a < 1.0) {
			// Squared fall-off: bright at the strike, gone quickly.
			const double f = 1.0 - a;
			b.level = f * f;
			continue;
		}

		VESSEL *v = oapiGetVesselInterface(b.parent);
		if (v && b.ps) v->DelExhaustStream(b.ps);
		b.used  = false;
		b.ps    = 0;
		b.level = 0.0;
	}
}

inline void Damage::spark_clear()
{
	for (size_t i = 0; i < SPK_MAX; i++) {
		Burst &b = m_spark[i];
		if (b.used && b.ps) {
			VESSEL *v = oapiGetVesselInterface(b.parent);
			if (v) v->DelExhaustStream(b.ps);
		}
		b.used  = false;
		b.ps    = 0;
		b.level = 0.0;
	}
}

// --------------------------------------------------------------

// Removal is a collapse, not a flag.
//
// It used to be GRPEDIT_ADDUSERFLAG with UsrFlag 2, "do not render", which is
// the right call when the unit of breakage is the whole group. It is not any
// more: a group now holds many components and UsrFlag has no per-component
// form, so hiding a group would take the entire solar array off the station
// because one blanket was hit.
//
// Writing a component's own vertices to a single point leaves every triangle in
// it degenerate -- zero area, nothing rasterised -- and cannot disturb anything
// else, because components share no vertices by construction. The template is
// untouched, so this is undone simply by not doing it.
inline void Damage::collapse(VESSEL *v, VISHANDLE vis, UINT mesh, DWORD grp,
                             const VtxList &vidx)
{
	if (vidx.empty()) return;
	DEVMESHHANDLE dm = v->GetDevMesh(vis, mesh);
	const MESHHANDLE tpl = v->GetMeshTemplate(mesh);
	if (!dm || !tpl) return;
	MESHGROUP *mg = oapiMeshGroup(tpl, grp);
	if (!mg || !mg->Vtx || !mg->nVtx) return;

	// Collapse to the component's own first vertex: a point that is certainly
	// inside the ship's bounding volume, so nothing is left poking out even if
	// A renderer keeps the degenerate triangles.
	const WORD v0 = vidx[0];
	if (v0 >= mg->nVtx) return;
	const NTVERTEX &s0 = mg->Vtx[v0];

	std::vector<NTVERTEX> out(vidx.size());
	for (size_t i = 0; i < vidx.size(); i++) {
		if (vidx[i] >= mg->nVtx) return;
		NTVERTEX t = mg->Vtx[vidx[i]];
		t.x = s0.x; t.y = s0.y; t.z = s0.z;
		out[i] = t;
	}

	GROUPEDITSPEC ges;
	ges.flags   = GRPEDIT_VTXCRD;
	ges.UsrFlag = 0;
	ges.Vtx     = &out[0];
	ges.nVtx    = (DWORD)out.size();
	ges.vIdx    = const_cast<WORD *>(&vidx[0]);
	oapiEditMeshGroup(dm, grp, &ges);
}

inline void Damage::enforce(OBJHANDLE h, VESSEL *v) const
{
	std::map<OBJHANDLE, std::map<PieceKey, VtxList> >::const_iterator it = m_hidden.find(h);
	if (it == m_hidden.end() || it->second.empty()) return;

	VISHANDLE *pv = oapiObjectVisualPtr(h);
	if (!pv || !*pv) return;          // not being drawn, nothing to remove yet

	for (std::map<PieceKey, VtxList>::const_iterator k = it->second.begin();
	     k != it->second.end(); ++k) {
		// Not one that is still tumbling clear -- see flying(). Update()
		// collapses it the moment it retires, and that makes it permanent.
		if (flying(h, k->first.mesh, k->first.grp, k->first.comp)) continue;
		collapse(v, *pv, k->first.mesh, k->first.grp, k->second);
	}
}

// --------------------------------------------------------------

inline bool Damage::hit(OBJHANDLE h, VESSEL *v, const Collider &c, const BodyRef &b,
                        const VECTOR3 &p_local, double energy, double simt,
                        double tear, const VECTOR3 *wind)
{
	if (energy < DMG_MIN_E || c.groups.empty() || c.area <= 0.0) return false;

	// A structural failure is not a big collision.
	//
	// A collision deposits its energy at one point and tears out what is
	// around it, so the pieces are taken nearest-first and nothing larger than
	// CHUNK_EXTENT comes away -- a ship should not be able to knock a module
	// off a station by hitting it.
	//
	// The air destroying an airframe is the other thing entirely. It is not
	// local, and what fails is an assembly: a wing, a door, a radiator. So
	// past the limit the reach opens out towards the whole hull, the size
	// allowance opens out towards AERO_TEAR_EXTENT, and what goes first is
	// the biggest thing that has failed rather than the nearest. Taking the
	// nearest first at this scale spends a wing's worth of budget on two
	// hundred tiles and fills DMG_MAX_LIVE with confetti.
	if (tear < 0.0) tear = 0.0;
	if (tear > AERO_TEAR_MAX) tear = AERO_TEAR_MAX;
	const bool structural = (tear > 0.0);

	// A full flight pool stops a collision and must not stop a breakup.
	//
	// DMG_MAX_LIVE bounds how many pieces are drawn tumbling, which is a
	// rendering cost, and it was being used as a bound on how much of a ship
	// could be destroyed -- so an entry that filled the pool on its first
	// failure could not break anything else until chunks aged out. Measured
	// in-game: two failure events in a whole entry, 256 pieces each, because
	// the pool refilled exactly as fast as it drained.
	//
	// Past the pool a piece is still removed from the hull and the collider --
	// It is simply collapsed instead of flown. The hole is right; the debris
	// is capped. A collision keeps the old behaviour, because it is what the
	// impact sweep is measured against and a collision has no reason to strip
	// A hull it cannot draw.
	if (!structural && m_chunk.size() >= DMG_MAX_LIVE) return false;

	// How big a thing may fail, in two stages. The first unit of `tear` opens
	// the allowance from a panel to an assembly; past that it keeps opening
	// towards the whole vehicle, because an airframe far enough past its limit
	// has no intact primary structure left. See AERO_TEAR_MAX in ColTune.h for
	// what the ceiling used to cost -- a 307 m2 windward skin that could never
	// come off, with the wing undersides inside it.
	double sizef = 0.0;
	if (structural) {
		sizef = (tear <= 1.0)
			? CHUNK_EXTENT + (AERO_TEAR_EXTENT - CHUNK_EXTENT) * tear
			: AERO_TEAR_EXTENT + (1.0 - AERO_TEAR_EXTENT)
			                     * ((tear - 1.0) / (AERO_TEAR_MAX - 1.0));
		if (sizef > 1.0) sizef = 1.0;
	}
	const double maxext = structural ? 2.0 * c.radius * sizef : 0.0;

	// Energy buys area. Charging per square metre of what is being torn off is
	// what lets one constant serve a 24 t glider and a 450 t station: a big
	// panel costs proportionally more to remove than a small one, so the
	// threshold scales with the thing itself rather than with the ship.
	double budget = energy / DMG_ENERGY;
	double cap    = DMG_MAX_FRAC * c.area;
	if (budget > cap) budget = cap;
	if (budget <= 0.0) return false;

	// Reach. A collision is local to the point. A structural failure opens out
	// to the whole vessel, and the scale for that is the diameter, not the
	// radius: the windward point sits on the hull, so the far end of the ship
	// is most of a diameter away from it. Measured on Atlantis with the
	// windward point on the nose underside, the bodyflap, the OMS pods, the
	// elevons and the whole aft fuselage are 25 to 29 m from it against a
	// radius of 20.6 -- so with the radius as the ceiling the back half of the
	// ship could not fail at all, whatever the overload.
	// Clamped at 1: at tear = 1 this is already the whole diameter, and `tear`
	// now runs past 1 to open the size allowance rather than the reach.
	const double treach = (tear > 1.0) ? 1.0 : tear;
	const double rmax = structural
		? 2.0 * c.radius * (0.5 * DMG_RADIUS_MAX + (1.0 - 0.5 * DMG_RADIUS_MAX) * treach)
		: DMG_RADIUS_MAX * c.radius;

	// The sort key: distance from the point for a collision, and minus the
	// area for a structural failure, so that one ascending sort serves both.
	std::vector<std::pair<double, size_t> > cand;
	for (size_t i = 0; i < c.groups.size(); i++) {
		const GroupRef &g = c.groups[i];
		if (structural) {
			if (g.extent > maxext) continue;                    // too big even now

			// And it has to be in the wind. Without this the largest-first
			// rule takes the largest piece anywhere, and on a shuttle the
			// largest pieces are on the roof: measured, 58% of everything
			// removed was the payload bay doors, the radiators and the bay
			// liner, none of which the air can reach, while the belly it was
			// flying on stayed perfect. A piece has to present some of itself
			// to the flow to be torn off by it.
			if (wind && proj_area(g, *wind) < AERO_TEAR_FACE * g.area) continue;
		}
		else            { if (!g.detachable) continue; }        // structure
		if (g.area < DMG_MIN_PIECE) continue;                   // confetti
		if (is_hidden(h, g.mesh, g.grp, g.comp)) continue;      // already gone
		double d = length(g.centre - p_local);
		if (d > rmax) continue;
		cand.push_back(std::make_pair(structural ? -g.area : d, i));
	}
	if (cand.empty()) return false;
	std::sort(cand.begin(), cand.end());

	// Two running totals, and they are not the same number. `spent` is what the
	// energy budget is charged (discounted for thin panels); `got` is the real
	// surface area that leaves the ship. Mass and the log line must use the
	// real one -- charging a solar blanket less to tear off does not make it
	// lighter, and briefly conflating the two reported a 12-piece impact as
	// 0.37 m2 when the pieces alone were 0.60 m2 or more.
	std::vector<size_t> take;
	double got = 0.0, spent = 0.0;
	for (size_t k = 0; k < cand.size(); k++) {
		const GroupRef &g = c.groups[cand[k].second];

		// A thin panel is not hull plate. `thin` is the smallest bounding-box
		// side over the diagonal, which is the only thing the geometry says
		// about what a piece is made of: a solar blanket or a radiator fin
		// reports ~0, a pressure module ~0.4. Charging both the same joules per
		// square metre is why a glider could lie inside an ISS solar wing with
		// the wing intact -- 60 m2 of blanket priced as 60 m2 of hull needs
		// 12 MJ, which is a 30 m/s impact.
		//
		// The discount is capped so nothing becomes free, and it only ever
		// makes a piece weaker.
		double cost = g.area;
		if (g.thin < DMG_THIN) {
			double f = g.thin / DMG_THIN;                // 0 .. 1
			if (f < DMG_THIN_MIN) f = DMG_THIN_MIN;
			cost *= f;
		}

		if (spent + cost > budget) continue;    // too big for what is left, but a
		                                        // Smaller piece further out may
		                                        // still come away
		if (!structural && m_chunk.size() + take.size() >= DMG_MAX_LIVE) break;
		take.push_back(cand[k].second);
		spent += cost;
		got   += g.area;
	}
	if (take.empty()) return false;

	// ---- structure goes with structure ----
	//
	// What the air tore off carries whatever was attached to it. Attachment is
	// not in the mesh -- components share no vertices by construction, which is
	// the whole reason they are safe to fly -- so it is recovered from SPACE:
	// Two components occupying the same volume are two parts of one assembly.
	// On Atlantis that is a panel's windward and lee skins, whose boxes sit on
	// top of each other to the centimetre, and it is the wing upper skin lying
	// on the windward skin the wing is built from.
	//
	// A carried piece is NOT asked the windward question. It is not being torn
	// by the air; it is losing its attachment, and a spar does not care which
	// side the pressure was on. It still has to be within the size allowance
	// and still has to be worth drawing.
	//
	// See the AERO_CARRY block in ColTune.h for what bounds it, and for why
	// charging it to the energy budget made it dead code.
	const size_t ndirect = take.size();
	if (structural) {
		std::vector<char> taken(c.groups.size(), 0);
		for (size_t k = 0; k < take.size(); k++) taken[take[k]] = 1;

		// One hop, and no area allowance.
		//
		// What bounds a carry is not a budget, it is the question being asked:
		// What was bolted to the piece that just failed. So only the pieces the
		// air tore are asked it -- `ndirect`, not the growing list -- and a
		// carried piece does not go on to carry its own neighbours. That is
		// what makes it impossible for a failure to chain across a hull through
		// touching bounding boxes, without needing an arbitrary cap to stop it.
		//
		// An area ratio was tried in place of this and is worse in both
		// directions: at 1.0 a 60 m2 failure could not carry an 84 m2 wing, so
		// the wings stayed on; at 2.0 it carried 89 pieces off a 12-piece
		// failure at a third of assembly scale. The size allowance and the
		// energy budget that bounded the direct tear are the honest bounds.
		std::vector<std::pair<double, size_t> > att;
		for (size_t k = 0; k < ndirect; k++) {
			const GroupRef &g = c.groups[take[k]];

			// Biggest first, exactly as the direct pass does, and for exactly
			// the same reason. Taking whatever happens to come first in index
			// order spent the whole of DMG_MAX_LIVE on fifty-Two greebles that
			// shared a bounding box with a door, and left the door's other skin
			// — the piece that actually matters — behind.
			att.clear();
			for (size_t i = 0; i < c.groups.size(); i++) {
				if (taken[i]) continue;
				const GroupRef &o = c.groups[i];
				if (o.extent > maxext) continue;
				if (o.area < DMG_MIN_PIECE) continue;
				if (is_hidden(h, o.mesh, o.grp, o.comp)) continue;
				if (!boxes_colocated(g, o, AERO_CARRY_GAP)) continue;
				att.push_back(std::make_pair(-o.area, i));
			}
			std::sort(att.begin(), att.end());

			for (size_t j = 0; j < att.size(); j++) {
				const size_t i = att[j].second;
				if (taken[i]) continue;              // carried by an earlier piece
				// DMG_MAX_FRAC applies to what leaves, not only to what is paid
				// for. It caps `budget` above, which the carry does not spend --
				// So without this one failure could take a third of a hull in a
				// single interval, which is the exact thing that constant was
				// written to prevent.
				if (got + c.groups[i].area > DMG_MAX_FRAC * c.area) continue;
				taken[i] = 1;
				take.push_back(i);
				got += c.groups[i].area;
			}
		}
	}

	// Mass by area share of the empty mass -- the hull is what is being torn,
	// not the propellant. The ship really does get lighter.
	double pm = v->GetEmptyMass();
	double m  = pm * (got / c.area);
	if (m > pm * 0.5) m = pm * 0.5;
	if (m > pm - 1.0) m = pm - 1.0;
	if (m > 0.0) v->SetEmptyMass(pm - m);

	std::map<PieceKey, VtxList> &gone = m_hidden[h];
	for (size_t k = 0; k < take.size(); k++) {
		const GroupRef &g = c.groups[take[k]];
		gone[PieceKey(g.mesh, g.grp, g.comp)] = g.vidx;

		// Each component flies as its own piece: they were separate geometry in
		// the mesh, so they separate here, and a scatter reads better than one
		// rigid slab.
		Chunk ch;
		ch.parent = h;
		ch.mesh   = g.mesh;
		ch.grp    = g.grp;
		ch.comp   = g.comp;
		ch.vidx   = g.vidx;
		ch.c0     = g.centre;
		ch.rpos   = mul(b.R, g.centre);                 // world offset from the CoG
		ch.Rc     = b.R;                                // starts with the ship's attitude
		ch.t0     = simt;

		// Its own share of what just left the ship, so update() can put it in
		// the airstream. Same area share the mass above was computed from.
		ch.area   = g.area;
		ch.mass   = (got > 0.0) ? m * (g.area / got) : 0.0;

		// It leaves with the velocity that part of the hull already had, plus a
		// push outward from the centre of mass.
		VECTOR3 vpt = mul(b.R, crossp(g.centre, b.omega));   // Orbiter's ordering
		double  ol  = length(ch.rpos);
		VECTOR3 out = (ol > 1e-6) ? ch.rpos * (1.0 / ol) : _V(0,1,0);
		ch.rvel  = vpt + out * DMG_SEP_V;
		ch.omega = mul(b.R, b.omega)
		         + _V(oapiRand()-0.5, oapiRand()-0.5, oapiRand()-0.5) * (2.0 * DMG_SPIN);

		if (m_chunk.size() < DMG_MAX_LIVE) m_chunk.push_back(ch);
		m_nbroken++;

		// A Burst where this piece was.
		//
		// One per piece, not one per event. The burst exists to mark where the
		// hull let go -- the one thing the debris itself cannot show, because a
		// piece is already metres away by the time you notice it -- and a
		// failure that takes eighteen pieces let go in eighteen places. Doing
		// it here rather than in the caller is also what makes it true of a
		// COLLISION: a ship that knocks six panels off a station sparks at all
		// six, which is what a tearing contact looks like.
		//
		// Direction: into the wake for a structural failure, since that is
		// where everything shed goes, and straight out from the centre of mass
		// for a collision, where there is no flow to carry it. Both in the
		// Vessel frame, which is what spark() wants.
		{
			const double cl = length(g.centre);
			const VECTOR3 outl = (cl > 1e-6) ? g.centre * (1.0 / cl) : _V(0,1,0);
			spark(v, h, g.centre, wind ? -(*wind) : outl, simt);
		}
	}

	// What the failure was allowed to do, not just what it did. `tear` and the
	// size allowance are the two numbers that decide whether an entry takes a
	// ship apart or sands it, and without them in the log a wreck that keeps
	// its wings looks like a tuning problem when it is a ceiling.
	char buf[300];
	snprintf(buf, sizeof(buf),
		"CollisionDetection: '%s' lost %zu piece(s) (%zu torn, %zu carried), "
		"%.2f m2, %.0f kg at (%.2f %.2f %.2f) -- %.1f kJ, tear %.2f, "
		"pieces up to %.1f m of %.1f, budget %.0f m2",
		v->GetName(), take.size(), ndirect, take.size() - ndirect, got, m,
		p_local.x, p_local.y, p_local.z, energy * 1e-3,
		tear, maxext, 2.0 * c.radius, budget);
	oapiWriteLog(buf);
	return true;
}

// --------------------------------------------------------------

inline void Damage::update(double simt, double simdt)
{
	if (m_chunk.empty() || simdt <= 0.0) return;

	std::vector<NTVERTEX> scratch;

	for (size_t i = 0; i < m_chunk.size(); ) {
		Chunk &ch = m_chunk[i];

		VESSEL *v = oapiGetVesselInterface(ch.parent);
		if (!v) { m_chunk.erase(m_chunk.begin() + i); continue; }

		// ---- fly it, relative to a non-rotating frame on the parent's CoG ----
		//
		// Gravity cancels between the two bodies in that frame, so only the
		// ship's own acceleration shows up, negated. That is what makes a piece
		// fall behind a ship under thrust and stay beside one that is coasting.
		double mass = v->GetMass();
		VECTOR3 T = _V(0,0,0), D = _V(0,0,0), L = _V(0,0,0);
		v->GetThrustVector(T); v->GetDragVector(D); v->GetLiftVector(L);
		MATRIX3 Rp; v->GetRotationMatrix(Rp);
		VECTOR3 aship = (mass > 0.0) ? mul(Rp, T + D + L) * (1.0 / mass) : _V(0,0,0);

		ch.rvel = ch.rvel - aship * simdt;

		// And its own drag, which is the whole reason debris trails.
		//
		// Without this a chunk coasts: it is given DMG_SEP_V outward and then
		// nothing ever acts on it except the ship's own acceleration, so it
		// drifts alongside for ever. In air that is badly wrong. The piece is
		// in the same flow the ship is in, at very nearly the same speed --
		// Rvel is metres per second against kilometres per second -- and its
		// ballistic coefficient is about a third of the ship's, so it
		// decelerates about three times harder and falls behind. See DBR_CD.
		//
		// A vacuum collision is unaffected: GetAtmDensity is zero there and
		// this does nothing at all.
		const double rho = v->GetAtmDensity();
		if (rho > 0.0 && ch.mass > 0.0 && ch.area > 0.0) {
			VECTOR3 av;
			if (v->GetAirspeedVector(FRAME_GLOBAL, av)) {
				const double vv = length(av);
				if (vv > 1.0) {
					const double acc = 0.5 * rho * vv * vv
					                 * DBR_CD * (ch.area * DBR_APROJ) / ch.mass;
					ch.rvel = ch.rvel - av * (acc / vv) * simdt;
				}
			}
		}

		ch.rpos = ch.rpos + ch.rvel * simdt;

		// Small-angle attitude update about the world tumble axis, then
		// re-orthonormalise so the matrix cannot drift.
		{
			VECTOR3 c0 = _V(ch.Rc.m11, ch.Rc.m21, ch.Rc.m31);
			VECTOR3 c1 = _V(ch.Rc.m12, ch.Rc.m22, ch.Rc.m32);
			c0 = c0 + crossp(ch.omega, c0) * simdt;
			c1 = c1 + crossp(ch.omega, c1) * simdt;
			double l0 = length(c0); if (l0 > 1e-12) c0 = c0 * (1.0/l0);
			c1 = c1 - c0 * dotp(c0, c1);
			double l1 = length(c1); if (l1 > 1e-12) c1 = c1 * (1.0/l1);
			VECTOR3 c2 = crossp(c0, c1);
			ch.Rc.m11 = c0.x; ch.Rc.m21 = c0.y; ch.Rc.m31 = c0.z;
			ch.Rc.m12 = c1.x; ch.Rc.m22 = c1.y; ch.Rc.m32 = c1.z;
			ch.Rc.m13 = c2.x; ch.Rc.m23 = c2.y; ch.Rc.m33 = c2.z;
		}

		// ---- retire it once it is far enough away or old enough ----
		double dist = length(ch.rpos);
		if (simt - ch.t0 > DMG_LIFE || dist > DMG_FADE) {
			VISHANDLE *pv = oapiObjectVisualPtr(ch.parent);
			if (pv && *pv) collapse(v, *pv, ch.mesh, ch.grp, ch.vidx);
			m_chunk.erase(m_chunk.begin() + i);
			continue;
		}

		// ---- draw it where it now is ----
		VISHANDLE *pv = oapiObjectVisualPtr(ch.parent);
		if (!pv || !*pv) { i++; continue; }      // off screen; keep flying it
		DEVMESHHANDLE dm = v->GetDevMesh(*pv, ch.mesh);
		const MESHHANDLE tpl = v->GetMeshTemplate(ch.mesh);
		if (!dm || !tpl) { i++; continue; }
		MESHGROUP *mg = oapiMeshGroup(tpl, ch.grp);
		if (!mg || !mg->Vtx || !mg->nVtx) { i++; continue; }

		VECTOR3 mofs = _V(0,0,0);
		v->GetMeshOffset(ch.mesh, mofs);

		// X = d + M*(v + meshofs - c0) - meshofs,  M = Rp^T Rc,  d = Rp^T rpos
		VECTOR3 d  = tmul(Rp, ch.rpos);
		VECTOR3 e0 = tmul(Rp, _V(ch.Rc.m11, ch.Rc.m21, ch.Rc.m31));
		VECTOR3 e1 = tmul(Rp, _V(ch.Rc.m12, ch.Rc.m22, ch.Rc.m32));
		VECTOR3 e2 = tmul(Rp, _V(ch.Rc.m13, ch.Rc.m23, ch.Rc.m33));

		// Only this component's vertices. The group may hold sixty-three other
		// solar blankets that are still bolted to the station; rewriting the
		// whole group would fly all of them.
		if (ch.vidx.empty()) { i++; continue; }
		scratch.resize(ch.vidx.size());
		bool bad = false;
		for (size_t q = 0; q < ch.vidx.size(); q++) {
			const WORD vi = ch.vidx[q];
			if (vi >= mg->nVtx) { bad = true; break; }
			const NTVERTEX &s = mg->Vtx[vi];
			VECTOR3 p  = _V(s.x, s.y, s.z) + mofs - ch.c0;
			VECTOR3 qq = e0 * p.x + e1 * p.y + e2 * p.z;
			VECTOR3 r  = d + qq - mofs;
			VECTOR3 nn = _V(s.nx, s.ny, s.nz);
			VECTOR3 nq = e0 * nn.x + e1 * nn.y + e2 * nn.z;
			NTVERTEX &t = scratch[q];
			t = s;
			t.x  = (float)r.x;  t.y  = (float)r.y;  t.z  = (float)r.z;
			t.nx = (float)nq.x; t.ny = (float)nq.y; t.nz = (float)nq.z;
		}
		if (bad) { i++; continue; }

		GROUPEDITSPEC ges;
		ges.flags   = GRPEDIT_VTXCRD | GRPEDIT_VTXNML;
		ges.UsrFlag = 0;
		ges.Vtx     = &scratch[0];
		ges.nVtx    = (DWORD)ch.vidx.size();
		ges.vIdx    = &ch.vidx[0];
		oapiEditMeshGroup(dm, ch.grp, &ges);

		i++;
	}
}

} // namespace col
