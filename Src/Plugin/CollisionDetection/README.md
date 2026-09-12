# CollisionDetection

Vessel-to-vessel contact against the actual mesh triangles, and the same damage model
driven by the air on reentry. See the **Collision detection** and **Reentry breakup**
sections of the [top-level README](../../../README.md).

One Orbiter module, built with the tree as `Modules/Plugin/CollisionDetection.so`
(`.dll` on Windows) and switched on from the Launchpad's **Modules** tab like any other
add-on. Nothing else is installed and nothing in the Orbiter core changes: it reads
vessel state and meshes through the public SDK and deposits contact forces with
`VESSEL::AddForce` from a pre-step callback.

Breakup also wants Orbiter's own **Damage and failure simulation** ticked, on the
Launchpad's *Parameters* tab under Vessel. It is off by default and cannot be changed
once a session has started.

No vessel, mesh, texture or add-on file is modified. Debris is drawn out of the parent
vessel's own mesh rather than spawned as a new vessel — see the note at the top of
`ColDamage.h` for why that constraint shapes the design.

| file | |
|---|---|
| `CollisionDetection.cpp` | the module, the collider cache and the per-frame contact pre-pass |
| `ColSolve.h` | the contact solver, the BVH and the per-vessel collider |
| `ColGeom.h` | triangle geometry and sampling |
| `ColDamage.h` | structural damage, reentry breakup, debris, dents, glow and sparks |
| `ColTune.h` | every tuning constant, with the measurement behind it |

Test scenarios are under `Scenarios/Collision detection/`.
