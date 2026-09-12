// ==============================================================
// ColTune.h -- tuning constants for the contact model. Every value was
// measured rather than chosen; the measurement is recorded with it.
// ==============================================================
#pragma once

namespace col {

// SHELL < GRAB < RELEASE is an invariant, not a preference: a point resting at
// its own equilibrium height must not already satisfy the release test, or it
// unlatches on the frame after it latches and the pair oscillates.
const double SHELL   = 0.01;    // [m] height the contact holds a point at
const double GRAB    = 0.02;    // [m] proximity-latch radius
const double RELEASE = 0.03;    // [m] rise needed to break contact

const double BETA    = 0.25;    // penetration fraction removed per frame

// Cap on the position-correction speed. A once-per-frame force can only arrest
// an approach over a whole frame, so a point keeps travelling while it is being
// stopped -- 3.3 m of it at 200 m/s -- and uncapped the correction undoes that
// in one frame and throws the body off at tens of m/s. Outer ceiling only:
// Capping the speed does not cap the force, mn*MAXSEP/dt. See PUSH_A.
const double MAXSEP  = 2.0;     // [m/s]

// Ceiling on the position correction, expressed as an acceleration.
//
// The correction asks for a relative normal speed of BETA*(SHELL-h)/dt by the
// end of the frame; that costs an impulse of mn*vnt, a force of mn*vnt/dt. A
// fixed speed cap therefore caps the force at mn*MAXSEP/dt, which is no cap at
// all: halve the frame time and the same overlap authorises twice the force.
// Measured on the Delta-glider and the ISS in their docked pose, at rest,
// engines off, so every newton below is the correction talking:
//
//        Fps      peak force      peak torque     KE invented
//         30          14 kN         32 kN.m            117 J
//         60          56 kN        284 kN.m            467 J
//        100         156 kN        792 kN.m            3.7 kJ
//        200        2227 kN       6045 kN.m          1606 kJ
//
// It sustains itself: the shove drives other sample points into the hull, that
// is real pressure, and real pressure buys Coulomb friction that resists
// getting out. As an acceleration the authorised force is mn*PUSH_A whatever
// the frame time. The same four runs at PUSH_A = 10:
//
//        Fps      peak force      peak torque     KE invented
//         30          14 kN         32 kN.m            117 J
//         60          56 kN        257 kN.m            468 J
//        100         114 kN        303 kN.m            1.3 kJ
//        200         134 kN        373 kN.m            1.8 kJ
//
// 10 m/s^2 is one gravity, the most a correction that only unsticks two meshes
// should apply, and the mildest value that flattens the table: 5 halves the
// peaks again, 30 leaves 200 fps at 472 kN. Nothing else moved -- the 0.5 to
// 200 m/s impact sweep holds arrest and peak contact energies to within 2% (so
// DMG_ENERGY below still stands, and penetration at 20 m/s improves from 0.30 m
// to 0.20 m), boxfric holds to 0.90*MU*W and slides at 1.10, and an undock
// still leaves at exactly 0.2000 m/s.
const double PUSH_A  = 10.0;    // [m/s^2]

const double MU      = 0.5;     // Coulomb friction coefficient

// Constraint force mixing. Do not remove it. A flat face on a flat face with
// more than three contact points is statically indeterminate: many force
// distributions give the same relative velocity, Gauss-Seidel picks an
// arbitrary one, and an asymmetric pick carries a net torque that spins a free
// pair up until it comes apart. Robust across 1e-4 to 3e-3; too large and the
// Contact becomes too compliant to hold.
const double CFM     = 0.001;

const int    ITERS   = 8;       // solver passes; converged by 8, 4 is 2e-4

// Time-acceleration gate, from two-body measurements at the worst case (equal
// masses) with a safety factor of 2 on each measured envelope. Above any of
// these the answer is wrong rather than merely coarse. Suspending the pair
// degrades to Orbiter's pass-through, which cannot drop a vessel through
// terrain: terrain belongs to the touchdown-point system, not to this module.
const double GATE_SAG    = 0.6;   // a_rel * dt^2   (envelope 1.3)
const double GATE_ABSORB = 0.37;  // a_rel * dt     (envelope 0.75)
const double GATE_TRAVEL = 0.10;  // v_rel * dt, as a fraction of body size

const double SAMPLE_CELL = 0.40;  // [m] contact sample point spacing
const double BROAD_PAD   = 2.0;   // [m] broad-phase margin

// Clearance a pair must reach after coming apart before contact is switched
// back on. Docking collars interlock: while still threaded together the contact
// model puts very large normal impulses on faces nearly parallel to the dock
// axis, the one direction the ship is trying to move. Measured with the
// Delta-glider leaving an ISS port at Orbiter's 0.2 m/s undock speed, 0.20 m of
// clearance is the first value that lets it go cleanly; 0.30 carries a safety
// factor of 1.5 and arms at about a metre of travel, where the collars are
// fully clear.
const double ARM_MARGIN  = 0.30;  // [m]

// ==============================================================
// Damage
// ==============================================================

// A group may be detached only if its bounding-box diagonal is under this
// fraction of the vessel's own diameter. Orbiter meshes are grouped by material,
// not by structure, so without this a single "group" can be the whole hull skin.
// Measured over four stock meshes (See GroupRef in ColSolve.h): 0.25 of span
// admits 93% of the Delta-glider's groups and excludes exactly the five that
// span the hull, and admits 45% of ProjectAlpha_ISS while excluding its modules.
// Expressed against 2*radius, which is what the collider has: 2*radius runs
// about 1.2x span (glider 23.2 vs 18.7, station 136 vs 113), so 0.25 of span is
// 0.20 of it. At 0.50 the station offered 30 of its 42 groups, including 50 m
// truss sections.
const double CHUNK_EXTENT = 0.20;   // fraction of 2 * collider radius

// Energy a single contact point must absorb before the structure around it
// fails, per square metre of the group's own area. Scaling by area is what makes
// one threshold work for a 24 t glider and a 450 t station.
//
// Measured with the offline harness driving this solver against the real
// Delta-glider and ISS meshes, peak single-Contact energy runs 2.2 kJ at
// 0.5 m/s, 10 kJ at 1, 24 kJ at 2, 43 kJ at 3, 172 kJ at 5, 636 kJ at 8 and
// 4.7 MJ at 20; the glider's detachable groups run 0.002 m2 smallest, 0.56
// median, 19 largest. At 2e5 J/m2, 5 m/s buys 0.9 m2 and 20 m/s buys 23 m2 --
// The largest panel on that part of the hull plus its neighbours.
const double DMG_ENERGY   = 2.0e5;  // [J/m^2]

// Absolute floor: below this nothing breaks at all, however small the piece.
// Without it the glider's 0.002 m2 greebles come off in any contact and a gentle
// dock sheds trim. 30 kJ is about 2.5 m/s of closing speed on the Delta-glider,
// so docking and light bumps are free and a real knock is not.
const double DMG_MIN_E    = 3.0e4;  // [J]

// How far from the impact point a group's centre may lie and still be part of
// the chunk. No minimum radius is needed: the energy budget already makes a
// harder hit reach further, since it affords more groups and groups are taken
// nearest-first. This is the hard stop, so a nose impact cannot strip a wingtip.
const double DMG_RADIUS_MAX = 0.45; // fraction of collider radius

// Most an impact may take off at once, as a fraction of the vessel's hull area.
// Without it a single very hard contact can strip a ship in one frame.
const double DMG_MAX_FRAC = 0.15;

// Separation speed given to a chunk as it comes away, and the spin. At 0.8 m/s
// and 0.6 rad/s a piece is still inside the silhouette of the wreck four seconds
// after it comes off, so a hit that shed sixteen pieces looked like one that
// shed none. 5 m/s clears a 12 m glider in about two seconds and is still an
// order below the closing speeds that produce it.
const double DMG_SEP_V    = 5.0;    // [m/s]
const double DMG_SPIN     = 1.5;    // [rad/s] peak random tumble

// How long a broken-off piece keeps flying, and how far, before it stops being
// drawn. A Chunk is a group of the parent's mesh moved out of place, so it is
// culled with the parent and cannot drift for ever. Its collider hole is
// permanent; only the drawing stops.
const double DMG_LIFE     = 90.0;   // [s]
const double DMG_FADE     = 500.0;  // [m] from the parent's centre of mass

// Ceiling on pieces in flight at once. The cost is per draw, not per piece: the
// vertices rewritten across every live chunk are bounded by the vessel's own
// mesh however many chunks there are, so the price is one oapiEditMeshGroup per
// Chunk per frame. 256 is affordable and about what a vehicle coming apart
// produces; 64 is what a scrape produces, and 64 is also a silent limiter --
// Measured on the Atlantis entry the first failure took exactly 64 pieces,
// filled the pool, and nothing else on the ship could break until chunks aged
// out at DMG_LIFE or passed DMG_FADE, so a fourteen-second breakup got four
// failure events instead of ten while the log reported a healthy-looking
// "64 pieces" each time.
const size_t DMG_MAX_LIVE = 256;

// How plate-like a piece has to be before it counts as a panel. GroupRef::thin
// is the smallest bounding-box side over the diagonal: a solar blanket or
// radiator fin ~0.01, a pressure module ~0.4, a cube 0.58. Below DMG_THIN the
// piece is charged proportionally less per square metre, down to DMG_THIN_MIN so
// nothing is free. An ISS solar blanket is a 12.6 m component of about 60 m2;
// priced as hull plate at DMG_ENERGY that is 12 MJ, a 30 m/s impact, so a glider
// could come to rest inside a solar wing with the wing undamaged. Thin film is
// not pressure hull, and the geometry is the only record of that in the mesh.
const double DMG_THIN     = 0.10;
const double DMG_THIN_MIN = 0.04;

// Smallest piece worth throwing. Splitting groups into components took the
// Delta-glider from 113 detachable pieces to 1628, mostly greebles -- aerials,
// hinges, panel lines -- and nearest-first selection then spent a 99 kJ impact
// on 41 of them totalling 0.50 m2: an average piece of 0.012 m2, 11 cm across,
// invisible at any distance, filling the 64-piece flight limit in one hit so the
// next impact could throw nothing at all. Below this the component stays on the
// hull and the budget goes to the next one up.
const double DMG_MIN_PIECE = 0.05;  // [m^2]


// ---------------------------------------------------------------------------
// Reentry breakup
//
// Not a new model. Orbiter already decides when an airframe fails, in
// DeltaGlider::TestDamage, and this follows that shape:
//
//     Double load = GetLift() / 190.0;
//     double dynp = GetDynPressure();
//     if (load > WINGLOAD_MAX || load < WINGLOAD_MIN || dynp > DYNP_MAX) {
//         double alpha = max((dynp-DYNP_MAX) * 1e-5,
//                            (load > 0 ? load-WINGLOAD_MAX : WINGLOAD_MIN-load) * 5e-5);
//         double p = 1.0 - exp(-alpha*dt);      // Probability of failure
//         if (oapiRand() < p) { ... }
//     }
//
// Excess over a limit sets a rate; a poisson term turns the rate into a per-step
// probability; a roll decides. DYNP_MAX and the 1e-5 below are the reference's
// own numbers, carried across unchanged. Heat is added, because the reference
// has none and a reentry is not a pressure problem: the Sutton-Graves proxy
// qdot ~ sqrt(rho) * v^3, which is what AERO_HEAT measures, scaled by 1e-9 so a
// Shuttle-like entry peaks near 3-4 and a steep one goes far past it. The DG's
// own aerodynamic wing and aileron failures are untouched; this sheds mesh,
// which no stock vessel does, so the two stack rather than fight.
// ---------------------------------------------------------------------------

// Dynamic pressure the airframe takes before it starts coming apart, and the
// rate per pascal of excess. Both are DeltaGlider.h's DYNP_MAX and the factor
// in TestDamage.
const double AERO_DYNP_MAX = 300e3;    // [Pa]
const double AERO_DYNP_K   = 1.0e-5;   // [1/(Pa s)]

// Heating, as sqrt(rho) * v^3 * 1e-9. The one number with no counterpart in the
// reference to inherit; set by flying an entry with the limit raised out of
// reach (`_dbg entry.scn`: 90 km, 7700 m/s, flight path -2 deg) and reading the
// profile off the log:
//
//     Alt 89.9 km  heat  0.92   q  0 kPa   mach 28.7
//     alt 76.3 km  heat  2.82   q  1 kPa   mach 27.5
//     alt 67.3 km  heat  5.42   q  4 kPa   mach 26.2
//     alt 59.3 km  heat  8.97   q 10 kPa   mach 24.9
//     alt 54.7 km  heat 11.56   q 18 kPa   mach 24.0   <- peak, then skips out
//
// A survivable orbital entry peaks near 11.5, so 4.0 is no limit at all: it tore
// the glider apart from 71 km down on an entry it should have walked away from.
// 16 leaves about 40% margin over a nominal entry and is reached by a steeper or
// faster one -- this must fire on a bad entry and never on a good one. Peak
// dynamic pressure on that same entry was 18 kPa against the 300 kPa limit,
// which is why q alone cannot discriminate a reentry and the heating term had
// to exist.
const double AERO_HEAT_MAX = 16.0;
const double AERO_HEAT_K   = 0.25;     // [1/s] per unit of excess

// Floor on what one failure throws off. Must stay above DMG_MIN_E or a failure
// would roll true and remove nothing.
const double AERO_E_MIN = 5.0e4;   // [J]

// ---------------------------------------------------------------------------
// Tearing up, as opposed to sanding down
//
// A fixed `E = AERO_E_MIN + AERO_E_K * over` with AERO_E_K = 8e5 J cannot break
// A ship. The energy did not know how big the ship was -- 8e5 J buys 4 m2 at
// DMG_ENERGY, a lot of a Delta-glider's 649 m2 and nothing at all of a
// shuttle's 2344 -- and hit() would only take pieces flagged `detachable`, i.e.
// CHUNK_EXTENT, 0.20 of the vessel's diameter. That flag stops a collision
// knocking a module off a station and is right for that; for an airframe being
// destroyed by the air it is wrong. On Atlantis it excludes both wings (19.0 m
// across a 41.3 m diameter), the payload bay doors, the radiators and the bay
// structure, leaving only trim: measured, at 157% of the heating limit the
// Shuttle shed 64 pieces totalling 43 m2, 1.8% of its hull, and was still
// flying, intact, with its wings on.
//
// So an entry failure is sized against the airframe, and grows more structural
// with the overload. `over` is the same normalised overload the reference's
// rate uses; `tear` is it clamped to AERO_TEAR_FULL.
// ---------------------------------------------------------------------------

// Share of the whole airframe that fails per second at one unit of overload. One
// failure over an interval dt is worth AERO_TEAR_RATE * over * dt of the hull,
// which at DMG_ENERGY J/m2 is the energy handed to hit(). Scale-free: the same
// number sheds trim off a glider and takes a wing off a shuttle, because both
// are a share of their own hull. At 0.30, Atlantis at 136% of the heating limit
// (over = 0.48) is worth 84 m2 per failure and rolls about 1.2 failures a second
// -- 4% of the hull a second, a vehicle coming apart over about ten seconds
// rather than one losing paint.
const double AERO_TEAR_RATE = 0.30;   // [1/s] of hull area, per unit overload

// Overload at which the failure is fully structural -- nothing off limits except
// the whole skin (See AERO_TEAR_EXTENT). Alpha = (heat - limit) * AERO_HEAT_K
// and over = alpha / 3, so 0.5 is heating 22 against a limit of 16: 38% past it,
// where an airframe should be failing and not flaking.
const double AERO_TEAR_FULL = 0.5;

// The largest piece a fully structural failure may take, as a fraction of the
// Vessel's diameter, interpolated up from CHUNK_EXTENT as `tear` goes 0 -> 1. A
// ceiling, not a target. On Atlantis, 0.60 of the 41.3 m diameter is 24.8 m,
// which admits the wings at 22.3 m -- the biggest thing the air is pressed
// against -- and still excludes the two fuselage skin components, 33.6 m and
// 40.0 m, which are the ship: a vehicle may lose any assembly, it may not lose
// its entire hull as one piece. At 0.50, which is 20.6 m, the wings miss by
// 1.7 m and the biggest admitted pieces are the payload bay doors and radiators
// up on the roof, so the ship sheds its back while the belly it is flying on
// stays perfect.
const double AERO_TEAR_EXTENT = 0.60;

// How much of a piece's own area has to face into the flow before the air can be
// said to have torn it off: proj_area() over its own area, so 1.0 is a panel
// square to the stream and 0 is edge-on or in the lee. A closed convex piece
// presents about a quarter of its surface whichever way it is turned, so 0.22
// admits a wing or a pod from any angle and excludes a door that faces away.
const double AERO_TEAR_FACE = 0.22;

// ---------------------------------------------------------------------------
// Past assembly scale: the primary structure itself
//
// With AERO_TEAR_EXTENT as the ceiling whatever the overload, a space Shuttle
// stays recognisably a space Shuttle at two and a half times its heating limit.
// Measured on Atlantis, the two components that ceiling excludes are:
//
//     307 m2   23.8 x 1.6 x 32.1 m   face 0.66   the entire windward skin
//     200 m2    7.0 x 6.1 x 32.3 m   face 0.11   the upper fuselage skin
//
// The first is the most windward thing on the vehicle and 13% of the hull, and
// the wing undersides are part of it, so with it pinned on the wings can never
// leave and the silhouette survives any entry.
//
// An airframe past its limit fails progressively: skin and trim, then assemblies
// -- a door, an elevon, a wing -- then, once loads go far enough past what the
// spars carry, the primary structure. So `tear` does not clamp at 1; it runs to
// AERO_TEAR_MAX and the size allowance runs with it, CHUNK_EXTENT ->
// AERO_TEAR_EXTENT over the first unit, then AERO_TEAR_EXTENT -> the whole
// vehicle over the rest. 2.0 puts "no intact primary structure" at over = 1.0,
// alpha = 3.0, heating 28 against a limit of 16 -- 175% of rated, where a
// survivable entry peaks at 72% and a bad one reaches 150%, so it is reached
// only by an entry that is destroying the vehicle. The reach interpolation is
// clamped at 1 because at tear = 1 it is already the whole diameter.
const double AERO_TEAR_MAX = 2.0;

// ---------------------------------------------------------------------------
// Structure goes with structure
//
// The windward test asks each component on its own whether the air can reach it,
// which is right for a single panel and wrong for a structure. On Atlantis the
// panels are modelled as two components, a windward skin and a lee skin, with
// bounding boxes on top of each other to the centimetre --
//
//     31/75  78.1 m2  2.8 x 2.1 x 18.7 m  face 0.43   taken
//      2/0   79.2 m2  2.8 x 2.1 x 18.7 m  face 0.01   left behind
//
// -- so the air tore one face off a door and left the other hanging in space.
// Every large piece on the ship is paired like that, which is why the wreck kept
// its shape: it shed skin and kept structure. A spar does not care which side
// the pressure was on, so a torn piece carries the components sharing its volume
// -- the other skin, the ribs, the fittings -- and those are not asked the
// windward question; they are losing their attachment, not being torn by the
// air.
//
// The carry is not charged to the energy budget. Charge it and it never fires:
// The selection fills the budget by construction, largest-first until the next
// piece does not fit, so nothing is left and every failure logs "0 carried"
// while the wreck keeps its wings. What bounds it instead:
//
//   - one hop. Only pieces the air actually tore carry anything; a carried piece
//     does not carry its own neighbours, so a failure cannot chain across a hull
//     and no cap is needed to stop it.
//   - co-location, not adjacency -- see boxes_colocated in ColSolve.h. At this
//     tiling density every component abuts several others, and "boxes touch"
//     chained 159 unrelated pieces off a 12-piece failure.
//   - the size allowance, which says how big a thing the present overload can
//     break at all, and DMG_MAX_LIVE.
//
// An area ratio fails in both directions as a fifth bound: at 1.0 a 60 m2
// failure could not carry an 84 m2 wing, so the wings stayed on; at 2.0 it moved
// 412 m2 on a 31 m2 budget.
//
// Known weakness: the first failure of an entry removes about a seventh of the
// hull in one interval, because the moment an airframe goes past its limit many
// small co-located pieces go at once. It is front-loaded, and has the least
// measurement behind it of anything here. Collisions do not use it -- a
// collision deposits its energy at one point and takes what is around it
// nearest-first, so the local geometry is already the answer.
const double AERO_CARRY_GAP = 0.10;   // [m] slack on the co-location test

// What a loose piece does in the airstream. Without drag a chunk sits beside the
// ship instead of falling behind it, and it is far worse at flying than the
// ship: hit() gives a piece mass by its share of the hull area, so every piece
// carries the hull's areal density -- about 33 kg/m2 on Atlantis -- against a
// tumbling body's mean projected area of a quarter of its surface. That is a
// ballistic coefficient near 110 kg/m2 where the Orbiter's own is 334, so it
// decelerates about three times harder and streams away behind.
const double DBR_CD    = 1.2;    // tumbling irregular piece
const double DBR_APROJ = 0.25;   // mean projected area, as a share of its own surface

// A Vessel is only tested this often. The Poisson term already makes the result
// frame-rate independent; this just keeps the hull scan off every step.
const double AERO_INTERVAL = 0.25;  // [s]


// ---------------------------------------------------------------------------
// Incandescence -- the hull and everything off it, glowing
//
// A piece torn off a ship at Mach 37 is in the same flow, at the same stagnation
// temperature, and thinner: it glows, and so does the hull it came off.
//
// One glow for the whole airframe, of necessity. The only per-object handle the
// SDK gives a module is oapiSetMaterial on the device mesh, and a material
// belongs to a mesh group; a chunk is a connected component of a group (See
// GroupRef in ColSolve.h), so no material is the chunk's alone and lighting one
// would light every other component of the same group, most still bolted to the
// ship. Measured on Atlantis, 59 groups hold 1865 components -- about thirty
// pieces per group. It is also the more correct of the two: the airframe at 250%
// of its heating limit is incandescent everywhere the air reaches and the pieces
// that have left it are in that same air.
//
// The driver is physical: the model's own heating number, sqrt(rho)*v^3*1e-9,
// the same one AERO_HEAT_MAX is measured against. The colour ramp is chosen -- a
// grey body goes dull red near 900 K and white past 1600 -- and these constants
// place that ramp against the airframe's limit rather than against a temperature
// the module does not compute.
// ---------------------------------------------------------------------------

// Where colour starts and where it is white, as fractions of AERO_HEAT_MAX.
// 0.25 of the limit is heating 4.0, which the Delta-glider's own logged entry
// reaches at about 70 km, where a re-entering vehicle starts to show. That
// entry peaks at 11.56, i.e. 0.72 of the limit, so a nominal entry stays low on
// this ramp and orange; only an entry destroying the ship goes white.
const double GLOW_ON   = 0.25;
const double GLOW_FULL = 2.50;

// How much emissive is added to every material at the top of the ramp.
//
// The unit is sunlight, which is why this is small: the shaders add
// GMtrl.emissive to the light before tone mapping -- vessel.fx builds
// base = ambient*SunAmbient + emissive and multiplies the texture by it, and
// PBR.fx, metalness.fx and mesh.fx all do the same -- so 1.0 means the surface
// emits as much again as the sun puts on it. Anything that swamps the diffuse
// term flattens the ship: every facet lands on the same value and the hull, the
// Debris and the holes between them stop being tellable apart. Measured by
// rendering, at 1.80 the Shuttle is a pale featureless blob from 95% of the
// heating limit onwards, with no faceting left and the debris the same colour as
// the hull it came off. Kept under the diffuse term, shape survives: a nominal
// entry is a warm tint you have to look for, and at 250% of the limit the
// shadowed side glows on its own while the lit side is clearly hot. Scaled by
// the ramp, so it fades in rather than switching on.
const double GLOW_MAX  = 1.40;

// The two ends of the ramp. Dull red first, the order a real surface heats in;
// A piece that flashes straight to white reads as a light rather than as metal.
const double GLOW_RED[3]   = { 1.00, 0.20, 0.03 };
const double GLOW_WHITE[3] = { 1.00, 0.90, 0.76 };

// Don't rewrite every material on every frame for a glow that has not moved:
// Materials are touched only when the level has changed by this much, or when
// the visual has been rebuilt underneath us.
const double GLOW_STEP = 0.02;


// ---------------------------------------------------------------------------
// Hull deformation -- a dent at the point of impact
//
// A Dent is a smooth radial depression pushed into the hull along the contact
// normal. It is applied to the device mesh from the pristine template every
// time, never incrementally, so it cannot drift or compound -- the same
// principle the flying chunks use.
//
// It happens below the break threshold: a knock too gentle to tear a panel off
// still leaves a mark, so the whole range from a nudge to a crash produces
// something visible. Hard-surface hulls do not deform gracefully, so these
// numbers are deliberately modest -- a dent you can see, not a crater that
// turns a panel inside out.
// ---------------------------------------------------------------------------

// Below this, no dent at all. About 1 m/s on a Delta-glider, from the same
// harness sweep DMG_ENERGY is tuned against: a docking contact is clean and
// anything harder marks the hull.
const double DENT_MIN_E   = 8.0e3;   // [J]

// Joules per metre of depth at the dent's centre.
const double DENT_ENERGY  = 2.5e5;   // [J/m]

// Deepest a single dent may go, and the deepest the hull may end up after any
// number of them overlapping. Without the second, repeated hits on one spot
// push the skin through the far side of the ship.
const double DENT_MAX_DEPTH = 0.45;  // [m]
const double DENT_MAX_SUM   = 0.90;  // [m]

// Width. The radius follows the depth so a harder hit spreads as well as
// deepens, with a floor so a shallow dent is a dish rather than a spike, and a
// ceiling as a fraction of collider radius so one hit cannot ripple across a
// whole ship.
const double DENT_RADIUS_K   = 7.0;  // radius = K * depth
const double DENT_RADIUS_MIN = 0.45; // [m]
const double DENT_RADIUS_MAX = 0.22; // fraction of collider radius

// Dents kept per vessel. Each one costs a vertex pass over the groups it
// touches whenever the visual is rebuilt, and the oldest is dropped past this.
const size_t DENT_MAX = 24;


// ---------------------------------------------------------------------------
// Impact sparks
//
// Not damage, and deliberately not gated on the damage model: a contact should
// be visible whether or not anything breaks. The threshold is two orders below
// DMG_MIN_E, so a scrape along a docking collar throws sparks and takes no
// pieces off. Emissive particles with the stock texture -- no new asset,
// nothing added to any mesh or add-on.
// ---------------------------------------------------------------------------

// Energy at one contact before it sparks. 2 kJ is about 0.5 m/s on a
// Delta-glider, from the same harness sweep DMG_ENERGY is tuned against.
const double SPK_MIN_E  = 2.0e3;   // [J]

// How long one burst keeps emitting, and the gap before the same place may throw
// another, so a grinding contact sparks in bursts rather than one continuous jet.
const double SPK_LIFE   = 0.45;    // [s]
const double SPK_REARM  = 0.15;    // [s]

// How far apart two tears have to be to count as two places. SPK_REARM on its
// own is a gap per vessel, right for one contact point sliding along one hull
// and wrong for a structural failure, which lets go in many places at once:
// Damage::hit throws a burst at each piece it takes, and without a distance test
// all but the first would be swallowed by the gap. 3 m is about the size of the
// structure a burst stands in for -- below it, two tears are the same event seen
// twice; above it they are two holes in the ship. On a Delta-glider, whose whole
// hull is 18 m, a scrape still reads as one travelling source; on Atlantis at
// 41 m the windward side supports a dozen distinct sites.
const double SPK_SITE   = 3.0;     // [m]

const double SPK_V0     = 22.0;    // [m/s] emission speed
const double SPK_SPREAD = 0.9;     // velocity spread at creation
const double SPK_SIZE   = 0.10;    // [m]   particle size at birth
const double SPK_RATE   = 900.0;   // [Hz]  creation rate at full level
const double SPK_PLIFE  = 1.1;     // [s]   particle lifetime

// How fast the air takes a spark. 0.0 means never: the core's integrator
// relaxes a particle towards the local air,
//
//     Pref = sqrt(rho) / 1.1371;
//     slow = exp(-beta * pref * dt);          // Beta is atmslowdown
//     P->vel = (p->vel - v_air) * slow + v_air;
//
// -- ExhaustStream::Update, OVP/VulkanClient/Particle.cpp -- and at beta = 0 the
// exponential is 1 and a spark keeps its birth velocity for ever. In vacuum it
// costs nothing either way, since the same function sets slow = 1.0 outright
// when rho is 0, so this constant cannot touch an orbital collision; in air, 0.0
// means sparks thrown off a re-entering ship hold the ship's speed while the
// ship brakes at three to six g, so they creep forward out of the nose instead
// of streaming aft.
//
// 3.0 is the core's own default (DefaultParticleStreamSpec, particle.cpp) and is
// also about right. Setting the core's decay rate equal to a real fragment's
// drag, k = 0.5*rho*v/bc, gives
//
//     Atmslowdown = 1.1371 * 0.5 * sqrt(rho) * v / bc
//
// And over the Atlantis entry this was measured on -- 70.1 km at 11.9 km/s down
// to 61.4 km at 11.4 -- a centimetre-scale lump of hull, bc = rho_al*s/Cd =
// 2700*0.01/1.2 = 22 kg/m2, asks for 2.8, 3.8 and 4.8. So 3.0 is about a
// centimetre of aluminium in that flow, which is what a spark is made of. On
// screen the spark falls behind at v * beta * sqrt(rho)/1.1371, ~350 m/s2
// mid-entry against the 67-100 m/s2 of a DBR_CD Chunk: sparks streak away from
// the tear three to five times faster than the pieces do.
const double SPK_SLOWDOWN = 3.0;

// Bursts in flight at once, across all vessels. Each is one Orbiter particle
// stream, and streams are not free.
const size_t SPK_MAX    = 8;

} // namespace col
