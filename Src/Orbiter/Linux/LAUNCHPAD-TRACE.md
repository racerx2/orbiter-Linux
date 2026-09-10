# Launchpad end-to-end traces

Every Launchpad control, followed from its dialog template through its handler,
its config field, and on to the code in the simulator that actually consumes
it. A control is only marked **traced** when the last column names a specific
function that reads the value and does something with it -- "written to
Orbiter.cfg" is not an end function.

Cross-checked against the read-only reference at `/home/racerx/Orbiter/orbiter/`.

Legend: **OK** verified working · **FIXED** was broken, now works ·
**OPEN** trace incomplete · **N/A** no simulator effect by design

---

## 1. Scenarios tab (IDD_PAGE_SCN, dialog 172)

| Control | id | Handler | Config field | End function | State |
|---|---|---|---|---|---|
| Start paused | 1092 | `BM_SETCHECK`/`BM_GETCHECK` | `CfgLogicPrm.bStartPaused` | OPEN -- consumer not yet read | OPEN |
| "Simulation scenarios" | -1 | none | - | label only | N/A |
| Info | 1097 | `WM_COMMAND` | - | `OpenScenarioHelp()` -> `::OpenHelp` | FIXED (path separator) |
| Scenario tree | 1090 | `TVN_SELCHANGED`, `NM_DBLCLK` | - | `ScenarioChanged()`, `IDLAUNCH` | OK |
| Description (HTML) | 1098 | `DisplayHTMLStr` | - | `HtmlCtrl` markup->text | FIXED (class was unregistered) |
| Description (text) | 1091 | `SetWindowText` | - | multi-line edit | FIXED (was one clipped line) |
| Splitter | 1292 | `SplitterCtrl` | `CfgWindowPos.LaunchpadScnListWidth` | `Refresh()` repositions panes | FIXED (hit area) |
| Save current... | 1093 | `WM_COMMAND` | - | `SaveCurScenarioAs()` writes .scn | FIXED (modal was a child) |
| Clear quicksaves | 1095 | `WM_COMMAND` | - | `fs::remove_all` on Quicksave dir | FIXED (MessageBox was a stub) |
| directory watcher | - | `threadWatchScnList` | - | `RefreshList(true)` | FIXED (was spinning a core) |

Upstream notes: `scnhelp` is cleared in `ScenarioChanged` but never assigned
anywhere in the file, so `IDC_SCN_INFO` is permanently disabled. Matches
Windows.

---

## 2. Options tab (IDD_PAGE_OPT, dialog 173) -- 11 pages

All pages write `Cfg()->Cfg*Prm` fields; most also call
`g_pOrbiter->OnOptionChanged(cat, item)`, which fans out to
`gclient->clbkOptionChanged`, `pDI->OptionChanged`, `g_psys->OptionChanged`
and `g_pane->OptionChanged`. During the Launchpad phase all four are null, so
the config field is the only live path -- verified by round-trip test.

### 2.2 Physics settings (IDD_OPTIONS_PHYSICS)

| Control | id | Config field | End function | State |
|---|---|---|---|---|
| Nonspherical gravity sources | 1093 | `CfgPhysicsPrm.bNonsphericalGrav` | `SetDefaultCaps` -> static `bGPerturb` -> `RigidBody::UseComplexGravity()` -> `SingleGacc_perturbation()` -> Pines spherical harmonics (`pinesAccel`) or J2/J3 zonal terms -> added to gravitational acceleration -> integrated | **traced** |
| Radiation pressure | 1094 | `CfgPhysicsPrm.bRadiationPressure` | `Vessel::SetDefaultState` -> `rpressure` -> `UpdateBodyForces()`: `if (rpressure) UpdateRadiationForces()` -> `IlluminationFactor()` x `GetMomentumFlux()` -> `F = mflux*(cs*albedo)` -> `Flin_add += F` -> `GetIntermediateMoments` -> `acc += mul(state.Q, F/mass)` -> integrated | **traced** |
| Gravity-gradient torque | 1095 | `CfgPhysicsPrm.bDistributedMass` | `SetDefaultCaps` -> `bDistmass` -> `RigidBody::Update` sets `bIgnoreGravTorque` -> `GetIntermediateMoments` computes `tau = crossp(pmi*Re,Re)*3GM/r^3` -> `EulerInv_full` -> integrated by `PropMode[].propagator` | **traced** |
| Atmospheric wind | 1096 | `CfgPhysicsPrm.bAtmWind` | `Planet::Setup()` -> static `Planet::bEnableWind` -> `Planet::WindVelocity()`: `if (bEnableWind && HasAtmosphere())` computes a cubic-interpolated altitude wind profile plus temporally-correlated gusts -> `SurfParam::Set` subtracts it from ground velocity to give `airvel_ship` -> drives lift, drag, side force and control surfaces in `UpdateAerodynamicForces` | **traced** |

### 2.1 Visual settings (IDD_OPTIONS_VISUAL) -- 18 controls

| Control | id | Config field | End function | State |
|---|---|---|---|---|
| Particle streams | 1089 | `CfgVisualPrm.bParticleStreams` | guards `Vessel::AddParticleStream`, `AddExhaustStream` and `AddReentryStream` -- all three return 0 without creating the stream, so no exhaust contrails and no reentry plasma | **traced** |
| Cloud layers | 1075 | `CfgVisualPrm.bClouds` | `Planet::Planet` -> `bHasCloudlayer = Cfg()->CfgVisualPrm.bClouds && MinCloudResolution >= 1` -> exposed as `OBJPRM_PLANET_HASCLOUDS`; also gates `cloudalt`, `cloudrot` in `Planet::Update` | **traced** |
| Cloud shadows | 1076 | `CfgVisualPrm.bCloudShadows` | `Planet::Planet` -> `cloudshadowcol = CloudShadowDepth` when set, else 1.0 (no shadow) -> `OBJPRM_PLANET_CLOUDSHADOWCOL` | **traced** |
| Max resolution level | 1085 | `CfgVisualPrm.PlanetMaxLevel` | `Planet::Planet` -> `max_patch_level = min(MaxPatchResolution, Cfg()->CfgVisualPrm.PlanetMaxLevel)` -> `OBJPRM_PLANET_SURFACEMAXLEVEL` -- caps surface tile subdivision | **traced** |
| Specular ripples | 1080 | `CfgVisualPrm.bSpecularRipple` | reaches the client as `OBJPRM_PLANET_SURFACERIPPLE` (`Planet::bWaterMicrotex`) | partial |
| Horizon haze | 1077 | `CfgVisualPrm.bHaze` | OPEN (graphics client) | OPEN |
| Distance fog | 1078 | `CfgVisualPrm.bFog` | OPEN (graphics client) | OPEN |
| Specular water reflections | 1079 | `CfgVisualPrm.bWaterreflect` | OPEN (graphics client) | OPEN |
| Night lights | 1081 | `CfgVisualPrm.bNightlights` | OPEN (graphics client) | OPEN |
| Night light level | 1082 | `CfgVisualPrm.LightBrightness` | OPEN (graphics client) | OPEN |
| Surface elevation | 1083 | `CfgVisualPrm.ElevMode` | OPEN (`elevmgr.cpp`) | OPEN |
| Elevation mode | 1084 | `CfgVisualPrm.ElevMode` | OPEN (`elevmgr.cpp`) | OPEN |
| Vessel shadows | 1086 | `CfgVisualPrm.bVesselShadows` | OPEN (graphics client) | OPEN |
| Reentry flames | 1087 | `CfgVisualPrm.bReentryFlames` | OPEN | OPEN |
| Object shadows | 1088 | `CfgVisualPrm.bShadows` | OPEN (graphics client) | OPEN |
| Specular object reflections | 1090 | `CfgVisualPrm.bSpecular` | OPEN (graphics client) | OPEN |
| Local light sources | 1091 | `CfgVisualPrm.bLocalLight` | OPEN (graphics client) | OPEN |
| Ambient light level | 1092 | `CfgVisualPrm.AmbientLevel` | `Config::SetAmbientLevel` -> `AmbientColour = level * 0x01010101` -> read by the graphics client | partial |

Note: most Visual settings are consumed by the graphics client through
`clbkOptionChanged` / `Config::GetParam`. No client exists yet on Linux, so
their end function is the client's -- they cannot be closed until the Vulkan
client is written. The label on the page says as much: *"Some graphics clients
may ignore, override or extend some of these settings."*

### 2.4 Vessel settings (IDD_OPTIONS_VESSEL)

| Control | id | Config field | End function | State |
|---|---|---|---|---|
| Limited fuel | 1071 | `CfgLogicPrm.bLimitedFuel` | `Vessel::DefaultGenericCaps` -> member `burnfuel` -> propellant consumption. Also live: `Vessel::OptionChanged(OPTCAT_VESSEL, OPTITEM_VESSEL_LIMITEDFUEL)` re-reads it mid-session | **traced** |
| Refuel on pad | 1072 | `CfgLogicPrm.bPadRefuel` | `Vessel::Refuel()` sets every tank to `maxmass`; caller not yet read | partial |
| Complex flight model | 1073 | `CfgLogicPrm.FlightModelLevel` | `Vessel::LoadModule` -> member `flightmodel` -> passed to the vessel module as `ovcInit(hVessel, flightmodel)` and to `VESSEL::VESSEL(hvessel, fmodel)`; modules read it back via `VESSEL::GetFlightModel()` to select simple vs complex aerodynamics and systems | **traced** |
| Damage / systems failure | 1074 | `CfgLogicPrm.DamageSetting` | `VESSEL::GetDamageModel()` returns it directly to every vessel module, which uses it to enable damage and systems-failure simulation | **traced** |

## Reading position

Tracing a control to its end function means reading the simulator, not just
the dialog code. Files read in full so far:

| File | Lines | State |
|---|---|---|
| `TabScenario.cpp` | 761 | complete |
| `TabOptions.cpp` | 79 | complete |
| `OptionsPages.cpp` | 2422 | complete |
| `LpadTab.cpp` | 153 | complete |
| `TabModule.cpp` | 300 of 420 | partial |
| `TabVideo.cpp` | 260 | complete |
| `TabExtra.cpp` | 280 of 1586 | partial |
| `TabAbout.cpp` | 93 | complete |
| `Launchpad.cpp` | 618 | complete |
| `CustomControls.cpp` | 240 | complete |
| `Rigidbody.cpp` | 551 | complete |
| `Rigidbody.h` | 282 | complete |
| `Vesselbase.cpp` | 433 | complete |
| `Planet.cpp` | 1054 | complete |
| `Vessel.cpp` | 9045 | **complete** |
| `Psys.cpp` | 630 of 872 | partial |
| `Config.cpp` | ~700 of 1585 | partial |
| `Vessel.h` | 520 of 1887 | partial |

## Separator bugs found by reading

Six so far, in four distinct shapes. Each needed a different search pattern,
which is why they survived the tree-wide conversion and why reading rather
than searching found them.

| Site | Literal | Effect |
|---|---|---|
| `Vessel::OpenConfigFile` | `"Vessels\\"` | 12 stock vessel class configs unloadable; failure calls `TerminateOnError()` |
| `Vessel::RegisterModule` | `"Modules\\%s.dll"` | every vessel module fails to load; all vessels fall back to the generic core `VESSEL` |
| `Planet::ScanBases` | `"%s\\%s"` | every surface base unloadable |
| `Planet::Planet` | `strcat(cbuf,"\\")` | per-planet surface labels never found |
| `PlanetarySystem::Read` | `push_back('\\')` | planetarium marker directory unopenable |
| `Orbiter.cpp`, `DeltaGlider.cpp` | `"Playback\\"`, `"DG\\Skins\\"` | playback and skin directories unreachable |

Related config consumed by the same path, not exposed on this page but set from
the Extra tab:

| Field | End function |
|---|---|
| `bOrbitStabilise` | `SetDefaultCaps` -> `bCanUpdateStabilised` -> `RigidBody::Update` chooses Encke stabilised propagation |
| `Stabilise_SLimit` / `Stabilise_PLimit` | thresholds in `RigidBody::Update` for entering Encke mode |
| `PropSubMax` | `SetDefaultCaps` -> `PropSubMax` -> substep count in `SetPropagator` |

Bug found while tracing: `PlanetarySystem::Read` appended a backslash to
`MarkerPath`, leaving the planetarium marker directory unopenable. FIXED.

### Time propagation (Extra tab -> Dynamic state propagators)

`CfgPhysicsPrm.PropMode[i]` / `PropTTgt` / `PropATgt` / `PropTLim` / `PropALim`
-> `RigidBody::SetupPropagationModes()` binds
`PropMode[i].propagator = &RigidBody::RK4_LinAng` (RK2..RK8, SY2..SY8)
-> `RigidBody::SetPropagator()` picks the level from the step size
-> `RigidBody::Update()` calls `((*this).*(PropMode[PropLevel].propagator))(dt, nPropSubsteps, i)`
which is the numerical integration of every vessel's state vector. **traced**

---

## Control-type infrastructure (shared by all tabs)

| Mechanism | State |
|---|---|
| checkbox `BM_SETCHECK`/`BM_GETCHECK` | OK, round-trip verified |
| edit `SetWindowText`/`GetWindowText` | OK, round-trip verified |
| combo `CB_*` | OK, round-trip verified |
| multi-select list `LB_SETSEL`/`LB_GETSEL` | FIXED (was unimplemented) |
| up-down `UDN_DELTAPOS` | FIXED (sign was inverted) |
| gauge `oapiSetGaugePos` + `WM_HSCROLL` | OK (needed `cbWndExtra` + drag fixes) |
| scrollbar `SB_CTL` + `WM_VSCROLL` | FIXED (class was unrendered) |
| `ScrollWindow` page scrolling | FIXED (was a no-op) |
| sibling z-order / splitter hit area | FIXED (splitter swallowed all clicks) |
| dialog owner vs parent (`WS_POPUP`) | FIXED (modals drawn as children) |
| keyboard -> `IsDialogMessage` | FIXED (no key messages were queued) |
| `WM_CLOSE` -> `UpdateConfig` -> `Write` | FIXED (settings were discarded on exit) |

---

## Still to trace

- Scenarios: `bStartPaused` consumer
- Options: Visual (18 controls), Instruments, Vessel, UI, Joystick,
  CelSphere, VisHelper, Planetarium, Labels, Forces, Axes
- Modules tab: activation -> `LoadModule` -> `clbkSimulationStart`
- Video tab: client selection -> `clbkCreateRenderWindow`
- Extra tab: each `LaunchpadItem` -> `clbkWriteConfig`
- About tab
- Launchpad frame: Launch / Help / Exit, tab buttons, wait page
