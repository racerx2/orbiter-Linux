// ==============================================================
//                 Orbiter module: CollisionDetection
//
// CollisionDetection.cpp
// Vessel-to-Vessel contact against the actual mesh triangles.
//
// Enabled from the Launchpad "Modules" tab like any other add-on. No vessel,
// mesh or add-on file is modified, and nothing in the Orbiter core changes:
// The module reads vessel state and meshes through the public SDK and deposits
// Contact forces with Vessel::AddForce, which the SDK documents as the way to
// "bypass Orbiter's built-in thrust and aerodynamics model completely and
// replace it by a user-defined model", called from a pre-step callback.
//
// Why a pre-step callback is the right slot
//
// Orbiter::UpdateWorld runs
//
//     ModulePreStep();      <- plugin clbkPreStep, then vessel ModulePreStep
//     G_psys->Update();     <- BeginStateUpdate, UpdateBodyForces, integrate
//     ModulePostStep();
//
// So clbkPreStep lands before any vessel integrates, with every vessel's state
// still the frozen end-of-last-frame snapshot. That is what a contact pre-pass
// needs: one consistent view of every body, and a deposit point that reaches
// the integrator before it runs. Because both halves of every pair are written
// in the same pass from the same snapshot, the forces are equal and opposite by
// construction rather than by luck -- momentum drift measured at exactly zero.
//
// What this does not do
//
// Contact against planetary terrain is NOT handled here and must not be.
// Orbiter already does it, and does it with a model this cannot replace:
// Vessel::AddSurfaceForces queries an elevation heightfield per touchdown point
// and runs a suspension model with per-point stiffness, damping, wheel brakes
// and separate longitudinal and lateral friction. That is landing gear. This
// Module covers what Orbiter leaves empty: vessel against vessel.
// ==============================================================

#define ORBITER_MODULE
#include "Orbitersdk.h"
#include "ColSolve.h"
#include "ColDamage.h"
#include <map>
#include <vector>
#include <cstdio>

// Tuning lives in ColTune.h, with the measurement behind each value.

// ==============================================================
// Collider cache
// ==============================================================

// Build a collider from a vessel's mesh templates.
//
// Only meshes flagged MESHVIS_EXTERNAL are collidable: that flag is how a
// Module says a mesh is part of the exterior hull, and it is also how a stage
// that has been jettisoned or hidden stops being part of it.
//
// Triangles go in raw mesh space with the per-mesh offset added at build time
// only because GetMeshOffset is cheap to re-read; the cache is invalidated
// whenever the mesh set changes, which is what ShiftCG does via the mesh count
// staying equal but offsets moving. See the note in ColSolve.h.
static bool build_collider(VESSEL *v, col::Collider &c, OBJHANDLE h, const col::Damage &dmg)
{
	c.tris.clear();
	c.pts.clear();
	c.groups.clear();
	c.area   = 0.0;
	c.radius = 0.0;
	c.valid  = false;

	UINT nm = v->GetMeshCount();
	c.nmesh = nm;
	for (UINT i = 0; i < nm; i++) {
		WORD vis = v->GetMeshVisibilityMode(i);
		if (!(vis & MESHVIS_EXTERNAL)) continue;

		const MESHHANDLE hm = v->GetMeshTemplate(i);
		if (!hm) continue;

		VECTOR3 ofs;
		if (!v->GetMeshOffset(i, ofs)) ofs = _V(0,0,0);

		DWORD ng = oapiMeshGroupCount(hm);
		for (DWORD g = 0; g < ng; g++) {

			MESHGROUP *mg = oapiMeshGroup(hm, g);
			if (!mg || !mg->Vtx || !mg->Idx) continue;

			// ---- split the group into connected components ----------------
			//
			// Union-find over the triangles' vertex indices. See GroupRef in
			// ColSolve.h for why the component and not the group is the piece:
			// A mesh group is a MATERIAL, and on the ISS one material is the
			// entire 136 m solar array, which nothing could ever break.
			//
			// Done here rather than cached because the collider is already
			// rebuilding every triangle and the BVH around them; the find pass
			// is small beside that.
			std::vector<WORD> parent(mg->nVtx);
			for (DWORD k = 0; k < mg->nVtx; k++) parent[k] = (WORD)k;

			struct UF {
				std::vector<WORD> &p;
				UF(std::vector<WORD> &pp) : p(pp) {}
				WORD find(WORD x) {
					while (p[x] != x) { p[x] = p[p[x]]; x = p[x]; }
					return x;
				}
				void join(WORD a, WORD b) {
					a = find(a); b = find(b);
					if (a != b) p[b] = a;
				}
			} uf(parent);

			for (DWORD k = 0; k + 2 < mg->nIdx; k += 3) {
				WORD i0 = mg->Idx[k], i1 = mg->Idx[k+1], i2 = mg->Idx[k+2];
				if (i0 >= mg->nVtx || i1 >= mg->nVtx || i2 >= mg->nVtx) continue;
				uf.join(i0, i1);
				uf.join(i0, i2);
			}

			// Root -> component index, and the component each root owns.
			std::map<WORD, DWORD> cid;
			std::vector<col::GroupRef> pieces;
			std::vector<VECTOR3> plo, phi;

			for (DWORD k = 0; k + 2 < mg->nIdx; k += 3) {
				WORD i0 = mg->Idx[k], i1 = mg->Idx[k+1], i2 = mg->Idx[k+2];
				if (i0 >= mg->nVtx || i1 >= mg->nVtx || i2 >= mg->nVtx) continue;

				const WORD root = uf.find(i0);
				std::map<WORD, DWORD>::iterator f = cid.find(root);
				DWORD ci;
				if (f == cid.end()) {
					ci = (DWORD)pieces.size();
					cid[root] = ci;
					col::GroupRef gr;
					gr.mesh = i; gr.grp = g; gr.comp = ci;
					gr.area = 0.0; gr.ntri = 0; gr.thin = 1.0;
					gr.detachable = false;
					for (int q = 0; q < 6; q++) gr.aproj[q] = 0.0;
					pieces.push_back(gr);
					plo.push_back(_V(1e30,1e30,1e30));
					phi.push_back(_V(-1e30,-1e30,-1e30));
				}
				else ci = f->second;

				// A component that has been broken off is not part of this
				// Vessel any more. Same set that removes it from the picture,
				// so what you see and what you hit cannot drift apart.
				if (dmg.is_hidden(h, i, g, ci)) continue;

				// The same displacement the picture gets. Damage::deform is
				// the identity for an undented hull and costs nothing then;
				// where there is a dent, the collider follows it, so what you
				// See and what you hit stay one surface. See ColDamage.h.
				VECTOR3 a = dmg.deform(h, i, _V(mg->Vtx[i0].x, mg->Vtx[i0].y, mg->Vtx[i0].z) + ofs);
				VECTOR3 b = dmg.deform(h, i, _V(mg->Vtx[i1].x, mg->Vtx[i1].y, mg->Vtx[i1].z) + ofs);
				VECTOR3 d = dmg.deform(h, i, _V(mg->Vtx[i2].x, mg->Vtx[i2].y, mg->Vtx[i2].z) + ofs);
				col::Tri t; t.a = a; t.e1 = b - a; t.e2 = d - a;
				c.tris.push_back(t);

				col::GroupRef &gr = pieces[ci];
				// Crossp(e1,e2) has magnitude 2*area and the face direction,
				// so half of it is area * unit normal -- the six projections
				// come straight off its components.
				const VECTOR3 an = crossp(t.e1, t.e2) * 0.5;
				gr.area += length(an);
				if (an.x > 0) gr.aproj[0] += an.x; else gr.aproj[1] -= an.x;
				if (an.y > 0) gr.aproj[2] += an.y; else gr.aproj[3] -= an.y;
				if (an.z > 0) gr.aproj[4] += an.z; else gr.aproj[5] -= an.z;
				gr.ntri++;

				const WORD vv3[3] = { i0, i1, i2 };
				for (int q = 0; q < 3; q++) gr.vidx.push_back(vv3[q]);

				const VECTOR3 *vv[3] = { &a, &b, &d };
				VECTOR3 &lo = plo[ci], &hi = phi[ci];
				for (int q = 0; q < 3; q++) {
					const VECTOR3 &p = *vv[q];
					if (p.x < lo.x) lo.x = p.x;  if (p.x > hi.x) hi.x = p.x;
					if (p.y < lo.y) lo.y = p.y;  if (p.y > hi.y) hi.y = p.y;
					if (p.z < lo.z) lo.z = p.z;  if (p.z > hi.z) hi.z = p.z;
					double r = length(p);
					if (r > c.radius) c.radius = r;
				}
			}

			for (size_t q = 0; q < pieces.size(); q++) {
				col::GroupRef &gr = pieces[q];
				if (!gr.ntri) continue;

				const VECTOR3 ext = phi[q] - plo[q];
				gr.lo = plo[q]; gr.hi = phi[q];
			gr.centre = (plo[q] + phi[q]) * 0.5;
				gr.extent = length(ext);

				// How plate-like it is. The smallest bounding-box side over the
				// diagonal: ~0 for a solar blanket or a radiator panel, ~0.5
				// for a box. It is the only thing the geometry says about what
				// A piece is made of, and a thin panel should not need the same
				// energy per square metre as a pressure hull. See DMG_THIN in
				// ColTune.h.
				double smin = ext.x;
				if (ext.y < smin) smin = ext.y;
				if (ext.z < smin) smin = ext.z;
				gr.thin = (gr.extent > 1e-6) ? (smin / gr.extent) : 1.0;

				// Vertex list, deduplicated -- it was pushed once per triangle
				// corner above.
				std::sort(gr.vidx.begin(), gr.vidx.end());
				gr.vidx.erase(std::unique(gr.vidx.begin(), gr.vidx.end()), gr.vidx.end());

				c.area += gr.area;
				c.groups.push_back(gr);
			}
		}
	}

	if (c.tris.empty()) return false;

	// A group may only come off if it is small relative to the ship. Orbiter
	// meshes are grouped by material rather than by structure, so without this
	// one "group" can be the whole hull skin -- the Delta-glider's glider4-1
	// spans 1.33 of the ship's own span, and ProjectAlpha_ISS's issmod is
	// 14781 vertices across 132 m. See GroupRef in ColSolve.h.
	const double maxext = col::CHUNK_EXTENT * 2.0 * c.radius;
	for (size_t i = 0; i < c.groups.size(); i++)
		c.groups[i].detachable = (c.groups[i].extent <= maxext);

	c.bvh.build(c.tris);
	col::sample_points(c.tris, col::SAMPLE_CELL, c.pts);
	c.valid = !c.pts.empty();
	return c.valid;
}

// ==============================================================
// the module
// ==============================================================

namespace oapi {

class CollisionDetection : public Module {
public:
	CollisionDetection(HINSTANCE hDLL);
	~CollisionDetection();

	void clbkSimulationStart(RenderMode mode);
	void clbkSimulationEnd();
	void clbkPreStep(double simt, double simdt, double mjd);
	void clbkPostStep(double simt, double simdt, double mjd);

private:
	const col::Collider *collider(OBJHANDLE h, VESSEL *v);
	col::PairState      &pairstate(OBJHANDLE a, OBJHANDLE b);
	static bool          joined(VESSEL *a, OBJHANDLE hb);

	std::map<OBJHANDLE, col::Collider> m_col;
	std::map<std::pair<OBJHANDLE,OBJHANDLE>, col::PairState> m_pair;

	// Pairs that must not be given contact forces yet. 2 = rigidly joined right
	// now, 1 = came apart and has not yet separated far enough to be safe. An
	// entry is erased on the first frame the pair produces no contacts at all,
	// which is the only honest definition of "clear".
	//
	// This used to be a flat one-second window, copied from the way
	// Vessel::PostUpdate gates its own docking scan on undock_t + 1.0. That is
	// the wrong quantity here: at Orbiter's 0.2 m/s undock speed one second is
	// 0.2 m of travel, and a Delta-glider is still tangled in an ISS docking
	// collar at 0.5 m -- it only comes fully clear at 1.0 m. Switching contact
	// on inside the collar puts a very large normal impulse on faces that lie
	// nearly parallel to the dock axis, and the ship gets dragged back in.
	std::map<std::pair<OBJHANDLE,OBJHANDLE>, char> m_disarm;

	// Structural damage. Owns the set of groups that have been broken off each
	// Vessel, which is also what build_collider consults, so the hole in the
	// picture and the hole in the collider are the same hole.
	col::Damage m_dmg;

	bool   m_announced;
	// Said-once flags for the two ways an impact produces no debris. See the
	// Damage block in clbkPreStep for why silence was the wrong answer.
	bool   m_dmgOffSaid, m_dmgWeakSaid;
	double m_reportT;
	long   m_contacts, m_gated, m_disarmed;
};

} // namespace oapi

static oapi::CollisionDetection *g_cd = 0;

DLLCLBK void InitModule(HINSTANCE hDLL)
{
	g_cd = new oapi::CollisionDetection(hDLL);
	oapiRegisterModule(g_cd);
}

DLLCLBK void ExitModule(HINSTANCE hDLL)
{
	delete g_cd;
	g_cd = 0;
}

oapi::CollisionDetection::CollisionDetection(HINSTANCE hDLL)
	: Module(hDLL), m_announced(false),
	  m_dmgOffSaid(false), m_dmgWeakSaid(false), m_reportT(0.0),
	  m_contacts(0), m_gated(0), m_disarmed(0)
{
}

oapi::CollisionDetection::~CollisionDetection()
{
}

void oapi::CollisionDetection::clbkSimulationStart(RenderMode mode)
{
	m_col.clear();
	m_pair.clear();
	m_disarm.clear();
	m_dmg.clear();
	m_announced = false;
	m_dmgOffSaid = m_dmgWeakSaid = false;
	m_reportT = 0.0;
	m_contacts = m_gated = m_disarmed = 0;
	oapiWriteLog((char*)"CollisionDetection: enabled - vessel contact against mesh triangles");

	// The damage model's state, at the top of every session. It is a global
	// Orbiter setting read per vessel through Vessel::GetDamageModel, so one
	// line answers it for the whole session -- and answering it here means
	// nobody has to crash a ship to find out that breakup was switched off.
	oapiWriteLog(oapiGetVesselCount() && oapiGetVesselInterface(oapiGetVesselByIndex(0)) &&
	             oapiGetVesselInterface(oapiGetVesselByIndex(0))->GetDamageModel()
		? (char*)"CollisionDetection: damage model ON - impacts can break mesh groups off"
		: (char*)"CollisionDetection: damage model OFF - no breakup. Enable \"Damage and "
		         "failure simulation\" on the Launchpad's Parameters tab (Vessel).");
}

void oapi::CollisionDetection::clbkSimulationEnd()
{
	m_col.clear();
	m_pair.clear();
	m_disarm.clear();
	m_dmg.clear();
}

// Broken-off pieces are flown here, after every vessel has integrated, so a
// Chunk is placed against its parent's final pose for the frame rather than the
// one it had before the step.
void oapi::CollisionDetection::clbkPostStep(double simt, double simdt, double mjd)
{
	m_dmg.update(simt, simdt);
	m_dmg.spark_update(simt);
}

// --------------------------------------------------------------

const col::Collider *oapi::CollisionDetection::collider(OBJHANDLE h, VESSEL *v)
{
	auto it = m_col.find(h);
	if (it != m_col.end()) {
		// Cheap invalidation: the mesh set changing is what matters, and
		// GetMeshCount moves whenever a module inserts, deletes or clears a
		// mesh -- which is what staging and cargo release do.
		if (it->second.nmesh == v->GetMeshCount())
			return it->second.valid ? &it->second : 0;
		m_col.erase(it);
	}
	col::Collider &c = m_col[h];
	if (!build_collider(v, c, h, m_dmg)) return 0;

	size_t ndet = 0;
	for (size_t i = 0; i < c.groups.size(); i++) if (c.groups[i].detachable) ndet++;

	char buf[240];
	snprintf(buf, sizeof(buf),
		"CollisionDetection: collider for '%s' - %zu triangles, %zu contact points, "
		"r=%.1f m, %zu groups (%zu detachable), %.0f m2",
		v->GetName(), c.tris.size(), c.pts.size(), c.radius,
		c.groups.size(), ndet, c.area);
	oapiWriteLog(buf);
	return &c;
}

col::PairState &oapi::CollisionDetection::pairstate(OBJHANDLE a, OBJHANDLE b)
{
	return m_pair[std::make_pair(a, b)];
}

// Are the two vessels rigidly joined? A docked or attached pair must never be
// given contact forces -- they are already held together by a constraint, and
// their collars are interpenetrating by construction.
bool oapi::CollisionDetection::joined(VESSEL *a, OBJHANDLE hb)
{
	UINT nd = a->DockCount();
	for (UINT i = 0; i < nd; i++) {
		DOCKHANDLE hd = a->GetDockHandle(i);
		if (hd && a->GetDockStatus(hd) == hb) return true;
	}
	DWORD na = a->AttachmentCount(false);
	for (DWORD i = 0; i < na; i++) {
		ATTACHMENTHANDLE ah = a->GetAttachmentHandle(false, i);
		if (ah && a->GetAttachmentStatus(ah) == hb) return true;
	}
	na = a->AttachmentCount(true);
	for (DWORD i = 0; i < na; i++) {
		ATTACHMENTHANDLE ah = a->GetAttachmentHandle(true, i);
		if (ah && a->GetAttachmentStatus(ah) == hb) return true;
	}
	return false;
}

// --------------------------------------------------------------
// The contact pre-pass. One evaluation per frame, before any vessel
// integrates, from one frozen snapshot of every body.

void oapi::CollisionDetection::clbkPreStep(double simt, double simdt, double mjd)
{
	if (simdt <= 0.0) return;

	// One vessel is enough to have work to do, and it did not used to be.
	//
	// This read `if (nv < 2) return;`, which is right for contact -- it takes
	// Two to collide -- and wrong for everything else the per-Vessel pass below
	// now does. Reentry breakup is the case that exposed it: a ship coming down
	// through the atmosphere is almost always alone in the scenario, so the
	// stress test never ran once and an entry could not break anything however
	// steep it was.
	//
	// The pair loop further down is bounded by j = i+1, so it does nothing on
	// its own with a single body and needs no guard of its own.
	const DWORD nv = oapiGetVesselCount();
	if (nv < 1) return;

	// ---- gather one consistent snapshot of every vessel ----
	std::vector<col::BodyRef>      body;
	std::vector<OBJHANDLE>         hnd;
	std::vector<const col::Collider*> col_;
	body.reserve(nv); hnd.reserve(nv); col_.reserve(nv);

	for (DWORD i = 0; i < nv; i++) {
		OBJHANDLE h = oapiGetVesselByIndex(i);
		if (!h) continue;
		VESSEL *v = oapiGetVesselInterface(h);
		if (!v) continue;

		// Re-assert what has been broken off. A device mesh is rebuilt whenever
		// the visual is, so without this, flying away and back brings a missing
		// wing back with it.
		if (m_dmg.any_hidden(h)) m_dmg.enforce(h, v);
		// Dents go back in whenever the visual has been rebuilt; reshape()
		// decides that for itself by comparing the handle, so this is a map
		// lookup on an undamaged ship.
		if (m_dmg.any_dent(h)) m_dmg.reshape(h, v);

		// Aerodynamic heating, as light. Not gated on the damage model and not
		// gated on having a collider either -- a ship glowing on entry is not
		// Damage, and it should happen whether or not anything is breaking.
		// In vacuum this is two accessors and a comparison. See ColTune.h.
		m_dmg.glow(h, v);

		const col::Collider *c = collider(h, v);
		if (!c) continue;

		// Reentry breakup. Not a collision, but the same breakup model: a ship
		// torn apart by the air should come apart the way a ship torn apart by
		// A space station does. Gated on Orbiter's damage model like everything
		// else here, and a no-op in vacuum. See ColTune.h for the stress test,
		// which is DeltaGlider::TestDamage's.
		if (v->GetDamageModel() && m_dmg.aero(h, v, *c, simt, simdt)) {
			// The collider it was just handed is now stale; drop it and take
			// this vessel up again next frame rather than solving against
			// geometry that has gone.
			m_col.erase(h);
			continue;
		}

		col::BodyRef b;
		b.v = v;
		v->GetGlobalPos(b.pos);
		v->GetGlobalVel(b.vel);
		v->GetAngularVel(b.omega);      // LOCAL frame, as Orbiter reports it
		v->GetRotationMatrix(b.R);
		b.mass = v->GetMass();
		v->GetPMI(b.pmi);
		b.size = c->radius;

		// External (non-Contact) acceleration, world frame. The individual
		// accessors are used rather than GetForceVector because that one
		// includes whatever this module deposited last frame, which would feed
		// back. All four are reported in LOCAL Vessel coordinates.
		VECTOR3 G = _V(0,0,0), T = _V(0,0,0), D = _V(0,0,0), L = _V(0,0,0);
		v->GetWeightVector(G);
		v->GetThrustVector(T);
		v->GetDragVector(D);
		v->GetLiftVector(L);
		b.aext = (b.mass > 0.0) ? mul(b.R, G + T + D + L) * (1.0 / b.mass) : _V(0,0,0);

		if (b.mass <= 0.0 || b.pmi.x <= 0.0 || b.pmi.y <= 0.0 || b.pmi.z <= 0.0) continue;

		body.push_back(b); hnd.push_back(h); col_.push_back(c);
	}

	const size_t n = body.size();
	if (n < 2) return;

	long contacts = 0, gated = 0, disarmed_n = 0;

	// Colliders made stale by damage. They are erased after the pair loop, never
	// inside it: col_[] holds pointers into m_col, and erasing while the loop is
	// still running leaves the remaining pairs reading freed geometry.
	std::vector<OBJHANDLE> stale;

	// ---- broad phase, then narrow phase on the survivors ----
	for (size_t i = 0; i < n; i++) {
		for (size_t j = i + 1; j < n; j++) {
			const col::BodyRef &A = body[i], &B = body[j];

			// Bounding spheres, plus a conservative time-of-impact bound. A
			// separating pair is rejected with one dot product and no distance
			// computation, which is what keeps the O(N^2) sweep under a
			// microsecond at realistic vessel counts.
			VECTOR3 d  = B.pos - A.pos;
			double  Rr = A.size + B.size + col::BROAD_PAD;
			double  d2 = dotp(d, d);
			if (d2 > Rr * Rr) {
				VECTOR3 vr = B.vel - A.vel;
				double dv = dotp(d, vr);
				if (dv >= 0) continue;                 // separating
				double vv = dotp(vr, vr);
				if (vv < 1e-12) continue;
				double tca = -dv / vv;
				if (tca > simdt) tca = simdt;
				VECTOR3 dc = d + vr * tca;
				if (dotp(dc, dc) > Rr * Rr) continue;
			}

			std::pair<OBJHANDLE,OBJHANDLE> pk(hnd[i], hnd[j]);
			auto dj = m_disarm.find(pk);

			if (joined(A.v, hnd[j])) {
				// Rigidly joined. Orbiter is already holding these two
				// together and their collars interpenetrate by construction,
				// so there is nothing for contact to do. Drop the latch state
				// as well, so that the first frame after they come apart
				// starts from a clean sweep instead of one whose "previous"
				// pose is minutes old.
				if (dj == m_disarm.end()) m_disarm[pk] = 2;
				else                      dj->second   = 2;
				m_pair.erase(std::make_pair(hnd[i], hnd[j]));
				m_pair.erase(std::make_pair(hnd[j], hnd[i]));
				continue;
			}

			// A pair that has just come apart stays disarmed until the geometry
			// says it is clear, by a margin. Nothing is gathered and nothing is
			// deposited until then, so a ship backing out of a docking collar
			// is left entirely to Orbiter, as it was before this module
			// existed. See ColTune.h for the measurement behind ARM_MARGIN.
			if (dj != m_disarm.end()) {
				if (dj->second == 2) dj->second = 1;   // first frame apart
				if (col::clear_of(A, B, *col_[i], *col_[j], col::ARM_MARGIN) &&
				    col::clear_of(B, A, *col_[j], *col_[i], col::ARM_MARGIN))
					m_disarm.erase(dj);
				else
					{ disarmed_n++; continue; }
			}

			// ---- the time-acceleration gate ----
			//
			// Keyed on the physics, not on the warp number: a fixed warp
			// threshold would be wrong by three orders of magnitude between a
			// pair coasting in orbit and a pair under 1 g.
			VECTOR3 arel = A.aext - B.aext;
			VECTOR3 vrel = A.vel  - B.vel;
			double  am   = length(arel), vm = length(vrel);
			double  sz   = (A.size < B.size ? A.size : B.size);
			if (am * simdt * simdt > col::GATE_SAG    ||
			    am * simdt         > col::GATE_ABSORB ||
			    vm * simdt         > col::GATE_TRAVEL * sz) { gated++; continue; }

			// ---- narrow phase, both directions into one contact set ----
			std::vector<col::Contact> con;
			col::gather_dir(A, B, *col_[i], *col_[j], pairstate(hnd[i], hnd[j]), simdt, con);

			size_t nAB = con.size();
			col::gather_dir(B, A, *col_[j], *col_[i], pairstate(hnd[j], hnd[i]), simdt, con);
			// The second gather phrased its constraints with the roles swapped.
			// Put them in the same terms as the first so one solver sees a
			// homogeneous list: swap the lever arms and flip the normal so it
			// again points out of B's surface.
			for (size_t k = nAB; k < con.size(); k++) {
				VECTOR3 t;
				t = con[k].rA; con[k].rA = con[k].rB; con[k].rB = t;
				t = con[k].lA; con[k].lA = con[k].lB; con[k].lB = t;
				con[k].n = -con[k].n;
			}

			if (con.empty()) continue;
			contacts += (long)con.size();
			col::solve_and_deposit(A, B, con, simdt);

			// ---- structural damage ----
			//
			// Energy absorbed at a contact is the impulse that stopped the
			// approach times the speed it was stopped from, halved because the
			// force builds from nothing to lam over the frame. Lam and vn0 are
			// the solver's own numbers, so this is the real constraint impulse
			// rather than the speed heuristic most games use.
			//
			// The hardest single contact is what breaks the ship, not the sum:
			// A hundred light touches spread over a hull are a landing, one
			// heavy one is a crash.
			{
				size_t kA = con.size(), kB = con.size();
				double eA = 0.0, eB = 0.0;
				for (size_t k = 0; k < con.size(); k++) {
					if (con[k].lam <= 0.0 || con[k].vn0 >= 0.0) continue;
					double E = 0.5 * con[k].lam * (-con[k].vn0);
					if (E > eA) { eA = E; kA = k; }
					if (E > eB) { eB = E; kB = k; }
				}

				const bool bDmgA = (A.v->GetDamageModel() != 0);
				const bool bDmgB = (B.v->GetDamageModel() != 0);

				// Sparks first, and not gated on the damage model. A Contact
				// should be visible whether or not anything breaks, and the
				// threshold is two orders below DMG_MIN_E so a scrape shows.
				// Con[k].n points out of B's surface, so it is the outward
				// direction for a and its negative is the one for B.
				if (kA < con.size() && eA >= col::SPK_MIN_E) {
					m_dmg.spark(A.v, hnd[i], con[kA].lA,
					            tmul(A.R,  con[kA].n), simt);
					m_dmg.spark(B.v, hnd[j], con[kA].lB,
					            tmul(B.R, -con[kA].n), simt);
				}

				// A hit hard enough to break something, with damage switched
				// OFF. Said once per session, and the energy is computed above
				// whether or not the gate is open so that it can be said.
				//
				// Without it there is nothing to tell a feature that is
				// switched off from one that is broken: a deliberate crash
				// sheds nothing and the log stays silent. GetDamageModel() is
				// Orbiter's own global setting, off by default, and the
				// Launchpad is the only place it can be changed -- the
				// in-simulation options dialog greys the checkbox out.
				if (!bDmgA && !bDmgB && !m_dmgOffSaid &&
				    (eA >= col::DMG_MIN_E || eB >= col::DMG_MIN_E)) {
					m_dmgOffSaid = true;
					char buf[320];
					snprintf(buf, sizeof(buf),
						"CollisionDetection: a %.0f kJ impact would have broken pieces "
						"off, but Orbiter's damage model is OFF so nothing can. Tick "
						"\"Damage and failure simulation\" on the Launchpad's Parameters "
						"tab, under Vessel -- it is greyed out in the in-flight Options "
						"dialog and cannot be changed once a session has started.",
						(eA > eB ? eA : eB) * 1e-3);
					oapiWriteLog(buf);
				}

				// Dents come before pieces, and at a lower threshold, so the
				// whole range from a nudge to a crash leaves something on the
				// hull. Con[k].n points out of B, so into a's hull is +n and
				// into B's is -n -- the opposite of the outward directions the
				// sparks above use, and worth writing down because getting it
				// backwards pulls the skin outward into a blister.
				const long dwas = m_dmg.dents();
				if (kA < con.size()) {
					if (bDmgA && m_dmg.dent(hnd[i], A.v, *col_[i], con[kA].lA,
					                        tmul(A.R,  con[kA].n), eA))
						stale.push_back(hnd[i]);
					if (bDmgB && m_dmg.dent(hnd[j], B.v, *col_[j], con[kA].lB,
					                        tmul(B.R, -con[kA].n), eB))
						stale.push_back(hnd[j]);
				}

				const long was = m_dmg.broken();

				if (kA < con.size() && bDmgA)
					if (m_dmg.hit(hnd[i], A.v, *col_[i], A, con[kA].lA, eA, simt))
						stale.push_back(hnd[i]);
				if (kB < con.size() && bDmgB)
					if (m_dmg.hit(hnd[j], B.v, *col_[j], B, con[kB].lB, eB, simt))
						stale.push_back(hnd[j]);

				// What the impact actually did. One line per event, and only
				// when something happened, so a long grinding contact that
				// sheds nothing stays quiet.
				if (m_dmg.broken() != was) {
					char buf[240];
					snprintf(buf, sizeof(buf),
						"CollisionDetection: %.0f kJ impact broke off %ld piece(s) "
						"and made %ld dent(s); %zu in flight",
						(eA > eB ? eA : eB) * 1e-3,
						m_dmg.broken() - was, m_dmg.dents() - dwas, m_dmg.live());
					oapiWriteLog(buf);
				}
				else if (m_dmg.dents() != dwas) {
					// Dented but nothing torn off. Reported because it is the
					// common case between DENT_MIN_E and DMG_MIN_E, and
					// otherwise a visible mark on the hull has no line in the
					// log to match it against.
					char buf[200];
					snprintf(buf, sizeof(buf),
						"CollisionDetection: %.1f kJ impact dented the hull "
						"(%ld dent(s) total); nothing broke off",
						(eA > eB ? eA : eB) * 1e-3, m_dmg.dents());
					oapiWriteLog(buf);
				}
				else if ((bDmgA || bDmgB) && !m_dmgWeakSaid &&
				         (eA > 0.0 || eB > 0.0) &&
				         (eA < col::DMG_MIN_E && eB < col::DMG_MIN_E)) {
					// Damage is on and the hit was simply too gentle. Said once,
					// with the threshold, because "how hard do I have to hit it"
					// is the next question and the answer is a number.
					m_dmgWeakSaid = true;
					char buf[240];
					snprintf(buf, sizeof(buf),
						"CollisionDetection: hardest contact so far absorbed %.1f kJ, "
						"below the %.0f kJ needed to break anything off (about 2.5 m/s "
						"on a Delta-glider). Damage model is on.",
						(eA > eB ? eA : eB) * 1e-3, col::DMG_MIN_E * 1e-3);
					oapiWriteLog(buf);
				}
			}
		}
	}

	// Safe now that nothing holds a pointer into m_col.
	for (size_t i = 0; i < stale.size(); i++) m_col.erase(stale[i]);

	m_contacts = contacts;
	m_gated    = gated;
	m_disarmed = disarmed_n;

	if (!m_announced) {
		char buf[200];
		snprintf(buf, sizeof(buf),
			"CollisionDetection: %zu vessels with colliders, SimDT %.4f s", n, simdt);
		oapiWriteLog(buf);
		m_announced = true;
	}
	if ((contacts || disarmed_n) && simt > m_reportT) {
		char buf[200];
		snprintf(buf, sizeof(buf),
			"CollisionDetection: %ld contact points active "
			"(%ld pairs gated by SimDT, %ld disarmed after separation)",
			contacts, gated, disarmed_n);
		oapiWriteLog(buf);
		m_reportT = simt + 2.0;
	}
}
