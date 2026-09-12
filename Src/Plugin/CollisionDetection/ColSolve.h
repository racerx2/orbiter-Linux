// ==============================================================
// ColSolve.h -- per-Vessel collider cache and the pairwise contact solve.
//
// Sign convention. Orbiter is left-handed and its core is consistent about it:
//
//     Velocity of a body-fixed point   v = v_cm + R * crossp(r_local, omega_local)
//     torque of force F applied at r   tau = crossp(F, r)          [both local]
//
// Verified at two independent sites in the core: SuperVessel::ComponentStateVectors
// and Vessel::AddSurfaceForces. Both are the mirror of the usual right-handed
// ordering, so every cross product ported from the right-handed harness has its
// arguments swapped.
//
// The effective mass is the one place the convention cancels: with w = d x r and
// dv = r x (I^-1 w), dotp(dv,d) reduces to sum(w_k^2 / (m*pmi_k)), which is the
// same positive quantity either way. That is not luck -- it is two reversals in
// the same expression -- but it is worth knowing, because it means a sign error
// here does NOT show up as a negative effective mass.
// ==============================================================
#pragma once

#include "ColGeom.h"
#include "ColTune.h"

namespace col {

// --------------------------------------------------------------
// Per-Vessel cached geometry. Built once from the vessel's mesh templates and
// reused until the mesh set changes.
//
// The BVH is built in raw mesh space and the per-mesh offset is applied at
// query time. Baking the offset in would silently decouple the collider from
// the visible ship the first time a module calls ShiftMesh or ShiftCG -- which
// is exactly what staging does -- and nothing would ever rebuild it.
// --------------------------------------------------------------
// One mesh group, kept so the damage model can detach whole groups.
//
// A Chunk is always an entire group. Splitting a group means generating new
// geometry, and the standing rule on this project is that no mesh is altered --
// So the unit of breakage is whatever unit the mesh author already drew.
//
// `extent` is what decides whether a group may come off at all. Orbiter meshes
// are split by material and texture rather than by structure, so a group can be
// "every red panel on the ship". Measured on four stock meshes: the Delta-glider
// is 93% compact groups (median 1.36 m on an 18.7 m ship) but its five largest
// span the whole hull -- glider4-1 covers 1.33 of the ship's span. Detaching one
// of those strips the paint off the entire vessel. ProjectAlpha_ISS is worse:
// `iss issmod` is 14781 vertices spanning 132 m of a 113 m station.
// The unit of breakage is the connected component, not the group.
//
// The paragraph above is why, and it understates the problem: on
// projectAlpha_ISS only 18 of 42 groups are compact enough to come off at all,
// and none of the solar arrays are, so flying into a wing did nothing
// whatsoever. But the group is not the structure -- it is a material. Splitting
// each group into connected components recovers the structure that grouping
// flattened:
//
//     Grp 19  135.9 m, 3814 m2  ->  64 components, every one 12.6 m
//     grp 20  124.4 m, 1907 m2  ->  32 components, every one 12.6 m
//     grp 24  108.3 m, 1264 m2  -> 168 components, ~4.2 m each
//
// Those 12.6 m pieces are the individual solar blankets. They were always
// separate geometry; they merely shared a texture. Components take the ISS from
// 18 detachable pieces to over 6500.
//
// And components share no vertices by definition, which is what makes this
// cheap: a piece can be flown or removed by rewriting only its own vertices,
// with no risk of dragging a neighbour's triangle into a stretched tear. That
// is the whole reason the split is done this way rather than on a spatial grid.
struct GroupRef {
	UINT    mesh;        // mesh index on the vessel
	DWORD   grp;         // group index within that mesh
	DWORD   comp;        // connected component within that group
	VECTOR3 centre;      // bounding-box centre, vessel-local
	VECTOR3 lo, hi;      // and the box itself. Two components that occupy the
	                     // Same volume are two skins of one structure; see the
	                     // carry in Damage::hit.
	double  extent;      // bounding-box diagonal
	double  area;        // summed triangle area, a stand-in for its share of hull mass
	double  thin;        // smallest bbox side / extent -- see below
	DWORD   ntri;
	bool    detachable;  // extent is small enough relative to the vessel

	// Which way the piece faces, as the area it presents along each axis:
	// Aproj[k] = sum over its triangles of area * max(0, n . Axis), for
	// +x,-x,+y,-y,+z,-z. proj_area() below interpolates it to any direction.
	//
	// A component's centre cannot answer "is this on the windward side" on a
	// Vessel that is not a sphere: at 40 degrees angle of attack the payload
	// bay doors sit forward of the CoG, so they score windward on position
	// while the air is arriving underneath them. What faces the flow is a
	// question about normals, and this is the cheapest honest form of it.
	double  aproj[6];

	// The component's own vertices, for flying it and for removing it. Indices
	// into the template group's vertex array.
	std::vector<WORD> vidx;
};

/// \brief are two components parts of the same structure?
///
/// Components share no vertices by construction, so there is no topological
/// link to follow -- but two skins of one structure still sit on top of each
/// other in space, and that is recoverable. Measured on Atlantis: the windward
/// and lee halves of a payload bay door are components 31/75 and 2/0, with
/// bounding boxes 2.8 x 2.1 x 18.7 m at the same centre to a centimetre. The
/// air can only ever reach one of them.
///
/// Co-Located, not merely adjacent. An overlap test is not enough: on a hull
/// tiled at this density every component abuts several others, and treating
/// "boxes touch" as "bolted together" chained 159 unrelated pieces off a
/// 12-piece failure. Two parts of one structure interpenetrate -- one's centre
/// lies inside the other's box -- while two neighbouring panels only share a
/// face. That is the discriminator, and it costs six comparisons.
inline bool boxes_colocated(const GroupRef &a, const GroupRef &b, double gap)
{
	const VECTOR3 &p = b.centre;
	if (p.x >= a.lo.x - gap && p.x <= a.hi.x + gap &&
	    p.y >= a.lo.y - gap && p.y <= a.hi.y + gap &&
	    p.z >= a.lo.z - gap && p.z <= a.hi.z + gap) return true;
	const VECTOR3 &q = a.centre;
	return q.x >= b.lo.x - gap && q.x <= b.hi.x + gap
	    && q.y >= b.lo.y - gap && q.y <= b.hi.y + gap
	    && q.z >= b.lo.z - gap && q.z <= b.hi.z + gap;
}

/// \brief the area this piece presents to a direction, from the six axis
///        projections. Exact for axis-aligned faces and a fair estimate for
///        anything else, which is all the windward test needs.
inline double proj_area(const GroupRef &g, const VECTOR3 &d)
{
	return (d.x > 0 ? d.x * g.aproj[0] : -d.x * g.aproj[1])
	     + (d.y > 0 ? d.y * g.aproj[2] : -d.y * g.aproj[3])
	     + (d.z > 0 ? d.z * g.aproj[4] : -d.z * g.aproj[5]);
}

struct Collider {
	std::vector<Tri>      tris;
	BVH                   bvh;
	std::vector<VECTOR3>  pts;      // contact sample points, vessel-local
	std::vector<GroupRef> groups;   // for the damage model
	double                area;     // total, so a group's share can be taken
	double                radius;   // bounding radius about the vessel origin
	UINT                  nmesh;    // mesh count the cache was built from
	bool                  valid;

	Collider() : area(0.0), radius(0.0), nmesh(0), valid(false) {}
};

// Latch state for one ordered pair (A's points against B's mesh). Everything
// is stored in B's LOCAL frame so it follows B without extra bookkeeping.
struct PairState {
	std::vector<char>    latched;
	std::vector<VECTOR3> pn, pp;   // latch plane normal and point, B-local
	std::vector<VECTOR3> prev;     // A's point last frame, B-local
	std::vector<char>    haveprev;
	double               lastseen; // simt, for eviction

	void size_to(size_t n)
	{
		if (latched.size() == n) return;
		latched.assign(n, 0);
		pn.assign(n, _V(0,0,0));
		pp.assign(n, _V(0,0,0));
		prev.assign(n, _V(0,0,0));
		haveprev.assign(n, 0);
	}
};

// One contact constraint, assembled in world coordinates.
struct Contact {
	VECTOR3 rA, rB;        // lever arms from each centre of mass, WORLD
	VECTOR3 lA, lB;        // the same contact point in each vessel's LOCAL frame
	VECTOR3 n;             // world normal, pointing out of B's surface
	double  vnt;           // target relative normal velocity at end of frame
	double  vbias;         // the part of vnt that is position correction, not physics
	double  mn;            // effective mass along n, constant over the solve
	double  lam;           // accumulated normal impulse, >= 0
	VECTOR3 jt;            // accumulated tangential impulse
	double  vn0;           // closing speed along n before the solve, for the damage model
};

// Everything the solver needs about one body, gathered once per frame.
struct BodyRef {
	VESSEL  *v;
	VECTOR3  pos, vel, omega;   // omega is LOCAL, as Orbiter reports it
	MATRIX3  R;
	double   mass;
	VECTOR3  pmi;
	VECTOR3  aext;              // external (non-contact) acceleration, WORLD
	double   size;
};

// --------------------------------------------------------------
// Velocity of a body-fixed point, world frame. Orbiter's ordering.
// --------------------------------------------------------------
inline VECTOR3 point_vel(const BodyRef &b, const VECTOR3 &r_local)
{
	return b.vel + mul(b.R, crossp(r_local, b.omega));
}

// Angular contribution to the effective mass along world direction d.
inline double ang_term(const BodyRef &b, const VECTOR3 &r_local, const VECTOR3 &d_world)
{
	VECTOR3 d = tmul(b.R, d_world);
	VECTOR3 w = crossp(d, r_local);                        // torque per unit impulse
	return (w.x*w.x)/(b.mass*b.pmi.x)
	     + (w.y*w.y)/(b.mass*b.pmi.y)
	     + (w.z*w.z)/(b.mass*b.pmi.z);
}

// Apply a world-frame impulse J at a body-local point, to provisional velocities.
inline void apply_impulse(const BodyRef &b, const VECTOR3 &r_local, const VECTOR3 &J_world,
                          VECTOR3 &pv, VECTOR3 &pw)
{
	pv = pv + J_world * (1.0 / b.mass);
	VECTOR3 Jl = tmul(b.R, J_world);
	VECTOR3 t  = crossp(Jl, r_local);                      // Orbiter: tau = F x r
	pw = pw + _V(t.x/(b.mass*b.pmi.x), t.y/(b.mass*b.pmi.y), t.z/(b.mass*b.pmi.z));
}

} // namespace col

namespace col {

// --------------------------------------------------------------
// Is every sample point of a further than margin from B's mesh?
//
// This is the test that re-arms a pair after it comes apart. The contact model
// itself cannot answer the question: inside a docking collar it reports
// contacts that blink in and out as sample points sweep past collar features,
// so "the gather found nothing this frame" is true several times on the way out
// while the two are still thoroughly tangled. Measured on the Delta-glider
// leaving an ISS port at 0.2 m/s: arming on a bare empty gather happens at
// 0.24 m and the ship is knocked back; arming on a 0.20 m clearance is the
// first value that lets it go, and 0.30 m arms at 0.96 m of travel, which is
// where an independent sweep says the collars are fully clear.
//
// Distance, not elapsed time, is the right quantity -- a time window is wrong
// by the warp factor and by the undock speed, which is what the flat one-second
// window it replaces got wrong.
//
// Cost: closest_pt prunes on the BVH box, so a separated pair is rejected at
// the root and this is one box test per point; and it early-outs on the first
// point that is too close. Measured at no detectable cost over 3000 frames.
// --------------------------------------------------------------
inline bool clear_of(const BodyRef &A, const BodyRef &B,
                     const Collider &cA, const Collider &cB, double margin)
{
	for (size_t i = 0; i < cA.pts.size(); i++) {
		VECTOR3 pw = A.pos + mul(A.R, cA.pts[i]);
		VECTOR3 q  = tmul(B.R, pw - B.pos);
		VECTOR3 cq; double d; int ti;
		if (closest_pt(cB.bvh, q, margin, cq, d, ti) && d < margin) return false;
	}
	return true;
}

// --------------------------------------------------------------
// One direction of the geometry pass: A's sample points swept through B's
// mesh. Appends constraints to con. Touches the BVH once per point.
//
// The swept segment runs [prev, cur + vrel*dt] -- one frame behind and one
// frame ahead. The backward half gives an unambiguous crossing direction (the
// point was outside there, or it would already be latched); the forward half
// is the lead a once-per-frame evaluation needs. A forward-only probe from the
// current position cannot tell inside from outside.
// --------------------------------------------------------------
inline void gather_dir(const BodyRef &A, const BodyRef &B,
                       const Collider &cA, const Collider &cB,
                       PairState &st, double dt, std::vector<Contact> &con)
{
	st.size_to(cA.pts.size());

	for (size_t i = 0; i < cA.pts.size(); i++) {
		const VECTOR3 &p = cA.pts[i];
		VECTOR3 pw = A.pos + mul(A.R, p);            // world
		VECTOR3 q  = tmul(B.R, pw - B.pos);          // B-local

		if (!st.haveprev[i]) { st.prev[i] = q; st.haveprev[i] = 1; }

		// Relative velocity of the point with respect to B, including the
		// increment the external forces will add over this frame
		VECTOR3 vrel_w = (point_vel(A, p) + A.aext * dt) - (point_vel(B, q) + B.aext * dt);
		VECTOR3 vrel_b = tmul(B.R, vrel_w);

		if (!st.latched[i]) {
			VECTOR3 seg = (q + vrel_b * dt) - st.prev[i];
			double L = length(seg);
			if (L > 1e-12) {
				VECTOR3 dir = seg * (1.0 / L);
				double t; int ti;
				if (segment_hit(cB.bvh, st.prev[i], dir, L, t, ti)) {
					const Tri &tr = cB.tris[ti];
					VECTOR3 n = crossp(tr.e1, tr.e2);
					double ln = length(n);
					if (ln > 1e-12) {
						n = n * (1.0 / ln);          // outward face normal
						// Orient from the geometry and reject exits. Orienting
						// "against the motion" is exact for a point entering a
						// solid and exactly backwards for one leaving it -- at
						// speed a point can be through before any sweep sees
						// it, then latch on the rebound with an inverted normal
						// and be held down inside the hull.
						if (dotp(n, dir) <= 0) {
							st.latched[i] = 1; st.pn[i] = n;
							st.pp[i] = st.prev[i] + dir * t;
						}
					}
				}
			}
		}

		if (!st.latched[i]) {
			// Proximity latch. A crossing-only rule leaves points that come to
			// rest without ever crossing invisible, and the contact manifold
			// ends up too small to hold the body still.
			VECTOR3 cq; double d; int ti;
			if (closest_pt(cB.bvh, q, GRAB, cq, d, ti) && d < GRAB) {
				const Tri &tr = cB.tris[ti];
				VECTOR3 nt = crossp(tr.e1, tr.e2);
				double ln = length(nt);
				if (ln > 1e-12) {
					nt = nt * (1.0 / ln);
					VECTOR3 dv = q - cq;
					if (dotp(dv, nt) > 0.0) {        // outward side
						st.latched[i] = 1;
						st.pn[i] = (d > 1e-9) ? dv * (1.0 / d) : nt;
						st.pp[i] = cq;
					}
				}
			}
		}

		st.prev[i] = q;                              // frame-start pose
		if (!st.latched[i]) continue;

		// Refresh the plane point and normal. Refreshing only the point is a
		// trap on a curved surface: h would be a projection onto a stale normal
		// and read less than the true gap for ever. Adopt (q - cq) as the
		// normal only while it still agrees with the latched side -- once the
		// point is genuinely inside it flips inward and must be ignored so h
		// goes negative and the solver pushes the right way.
		bool fresh = false;
		{
			VECTOR3 cq; double d; int ti;
			if (closest_pt(cB.bvh, q, RELEASE, cq, d, ti) && d < RELEASE) {
				st.pp[i] = cq;
				fresh = true;
				if (d > 1e-9) {
					VECTOR3 nd = (q - cq) * (1.0 / d);
					if (dotp(nd, st.pn[i]) > 0.0) st.pn[i] = nd;
				}
			}
		}

		double h = dotp(q - st.pp[i], st.pn[i]);
		if (h > RELEASE) { st.latched[i] = 0; continue; }

		// The plane is stale: no triangle of B lies within RELEASE of the
		// point. Two very different situations look alike here.
		//
		// The point may be buried well inside the hull, where the nearest face
		// is a long way off and the stale plane is the only thing that still
		// knows which way is out. Dropping the latch there is the fix that was
		// tried once and reverted, because it let a body arriving fast enough
		// sink three quarters of a metre into a floor.
		//
		// Or the point may simply have slid out sideways. Then h is the
		// projection onto a plane that is no longer anywhere near it, it stays
		// negative for ever, h > RELEASE never fires, and the latch is
		// immortal: measured, two ISS collar points held a Delta-glider "in
		// Contact" at three metres of separation, and they would have gone on
		// demanding the full MAXSEP push-out the whole time.
		//
		// Tell the two apart by asking the only question that distinguishes
		// them -- is there still a surface between this point and the outside,
		// along the direction the plane claims is out? The reach is bounded by
		// the penetration itself, so the query stays local and costs one ray.
		if (!fresh) {
			double t; int ti;
			double reach = (h < 0.0 ? -h : 0.0) + RELEASE;
			if (!segment_hit(cB.bvh, q, st.pn[i], reach, t, ti)) {
				st.latched[i] = 0; continue;
			}
		}

		// Target relative normal velocity at the end of this frame.
		double vnt = (h < SHELL) ? BETA * (SHELL - h) / dt
		                         :        -(h - SHELL) / dt;
		// Never launch it back out. A once-per-frame force can only arrest the
		// approach over a whole frame, so the point keeps travelling while it
		// is being stopped; uncapped, the correction tries to undo that in one
		// frame and throws the body off.
		//
		// The binding half of this is PUSH_A, not MAXSEP. Buying vnt costs
		// mn*vnt of impulse, which is mn*vnt/dt of force, so a cap on the
		// speed is a cap on the force only at one frame rate -- at 200 fps
		// the same millimetre of overlap was authorising 2.2 MN on a pair
		// that was standing still. PUSH_A*dt caps the acceleration instead,
		// which is the quantity that should not depend on the machine.
		// MAXSEP stays as the outer ceiling for a very long frame.
		double vcap = PUSH_A * dt;
		if (vcap > MAXSEP) vcap = MAXSEP;
		if (vnt > vcap) vnt = vcap;

		// Split the target. Inside the shell the whole of it is baumgarte --
		// Position correction, which exists to remove an overlap and is there
		// in full whether or not the surfaces are pressed together. Outside
		// the shell the target is purely speculative and carries no bias. The
		// solver needs the two apart to size the friction cone; see the note
		// on lamf in solve_and_deposit.
		double vbias = (h < SHELL) ? vnt : 0.0;

		Contact c;
		c.rA = pw - A.pos;
		c.rB = pw - B.pos;
		c.lA = p;
		c.lB = q;
		c.n  = mul(B.R, st.pn[i]);
		c.vnt   = vnt;
		c.vbias = vbias;
		c.mn  = 0.0;
		c.lam = 0.0;
		c.jt  = _V(0,0,0);
		c.vn0 = 0.0;
		con.push_back(c);
	}
}

// --------------------------------------------------------------
// Sequential-impulse solve over the fixed contact set, then deposit.
//
// Constraint force mixing regularises what is otherwise a statically
// indeterminate system: a flat face on a flat face with more than three points
// admits many force distributions, Gauss-Seidel picks an arbitrary one, and an
// asymmetric pick carries a net torque that spins a free pair up over tens of
// seconds. Measured, diagnosed and fixed; do not remove the CFM term.
// --------------------------------------------------------------
inline void solve_and_deposit(const BodyRef &A, const BodyRef &B,
                              std::vector<Contact> &con, double dt)
{
	if (con.empty()) return;

	VECTOR3 pvA = A.vel + A.aext * dt, pwA = A.omega;
	VECTOR3 pvB = B.vel + B.aext * dt, pwB = B.omega;

	// Provisional relative velocity at a contact, world frame.
	auto vrel = [&](const Contact &c) {
		VECTOR3 va = pvA + mul(A.R, crossp(c.lA, pwA));
		VECTOR3 vb = pvB + mul(B.R, crossp(c.lB, pwB));
		return va - vb;
	};
	auto eff = [&](const Contact &c, const VECTOR3 &d) {
		return 1.0 / (1.0/A.mass + 1.0/B.mass
		            + ang_term(A, c.lA, d) + ang_term(B, c.lB, d));
	};

	// The effective mass along the normal depends only on geometry and mass, so
	// it is fixed for the whole solve. It is also what converts the baumgarte
	// target into the impulse that target buys.
	//
	// vn0 is recorded in the same sweep: the closing speed the pair arrived
	// with, before any impulse has been applied. The damage model needs it and
	// it is only available here -- one iteration later the solve has already
	// removed it. Negative means approaching.
	for (size_t k = 0; k < con.size(); k++) {
		con[k].mn  = eff(con[k], con[k].n);
		con[k].vn0 = dotp(vrel(con[k]), con[k].n);
	}

	for (int it = 0; it < ITERS; it++) {
		for (size_t k = 0; k < con.size(); k++) {
			Contact &c = con[k];

			double vn = dotp(vrel(c), c.n);
			double dl = c.mn * (c.vnt - vn) - CFM * c.lam;
			double nl = c.lam + dl; if (nl < 0.0) nl = 0.0;
			dl = nl - c.lam; c.lam = nl;
			if (dl != 0.0) {
				VECTOR3 J = c.n * dl;
				apply_impulse(A, c.lA,  J, pvA, pwA);
				apply_impulse(B, c.lB, -J, pvB, pwB);
			}

			// Friction cone. Build it on the pressure, not on the whole
			// impulse. The baumgarte part of lam is position correction: it is
			// present in full even when the two surfaces are not pressed
			// together at all, because its only job is to remove an overlap.
			// Charging Coulomb friction against it is what welded a glider to
			// A station -- docking-collar faces lie nearly parallel to the
			// dock axis, so separating along that axis is almost pure sliding,
			// and mu times the impulse that corrects a few millimetres of mesh
			// overlap inside one 10 ms frame came to several hundred kN,
			// beating the ship's engines. Measured: normal +408 kN outward,
			// friction -715 kN inward, net -307 kN back into the port.
			//
			// Mn*vbias is exactly the impulse the bias bought; what is left is
			// the part that answers to real contact pressure. A resting body
			// keeps its friction -- its weight still has to be carried by lam
			// over and above the bias -- while a pair that is merely being
			// un-overlapped gets none.
			double lamf = c.lam - c.mn * c.vbias;
			if (lamf < 0.0) lamf = 0.0;

			VECTOR3 v  = vrel(c);
			VECTOR3 vt = v - c.n * dotp(v, c.n);
			double vts = length(vt);
			if (vts > 1e-12) {
				VECTOR3 td  = vt * (1.0 / vts);
				VECTOR3 njt = c.jt + td * (-eff(c, td) * vts);
				double cone = MU * lamf, njm = length(njt);
				if (njm > cone) njt = njt * (cone / njm);
				VECTOR3 dj = njt - c.jt; c.jt = njt;
				apply_impulse(A, c.lA,  dj, pvA, pwA);
				apply_impulse(B, c.lB, -dj, pvB, pwB);
			}
		}
	}

	// Depositing only the physical part of lam was tried here and is wrong.
	//
	// The reasoning was sound and the measurement killed it, so it is written
	// down rather than left for someone to try again. Lam is two impulses in
	// one: the part that answers to real contact pressure, and the baumgarte
	// part that exists only to remove an overlap. Depositing the whole of it
	// puts both into the bodies' momentum, and textbook practice is to keep
	// the second out of the real velocities -- split impulses, pseudo-
	// Velocities. Written as a difference, deposit = lam - (last frame's bias
	// impulse), that can be done without owning the integrator, and the extra
	// velocity then telescopes to the current correction speed and is handed
	// back when the overlap goes.
	//
	// It depends on the contact set being the same set frame to frame, and
	// here it is not: sample points latch and unlatch continuously as two
	// hulls grind past each other, so the refund lands at a different place,
	// on a different normal, from the push it was meant to cancel -- and a
	// refund larger than this frame's lam is an attractive normal impulse,
	// which pulls the hulls together and deepens the overlap it was supposed
	// to fix. Measured on the docked pose at rest: energy invented over 4 s
	// went from 1.1 kJ to 75 kJ, and the peak from 1.3 kJ to 187 kJ.
	//
	// The whole lam goes in. What keeps the correction from behaving like a
	// catapult is the ceiling on the acceleration it may apply, which is
	// where that belongs and where the real defect turned out to be: see
	// PUSH_A in ColTune.h.
	//
	// Hand over as force and attack point. AddForce takes the force in LOCAL
	// Vessel coordinates -- Vessel::GetIntermediateMoments rotates Flin_add by
	// the vessel rotation before use -- and forms the torque itself, in its own
	// convention. This module never computes a contact torque.
	const double idt = 1.0 / dt;
	for (size_t k = 0; k < con.size(); k++) {
		VECTOR3 f = (con[k].n * con[k].lam + con[k].jt) * idt;
		A.v->AddForce(tmul(A.R,  f), con[k].lA);
		B.v->AddForce(tmul(B.R, -f), con[k].lB);
	}
}

} // namespace col
