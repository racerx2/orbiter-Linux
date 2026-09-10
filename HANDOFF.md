# Orbiter Linux Port — Session Handoff

Written at the end of the session of 2026-08-30. Read this first, then
`PORTING-RULES.md`, then `Src/Orbiter/Linux/LAUNCHPAD-TRACE.md`.

---

## 1. RULES — these are not negotiable, and I broke every one of them

The user set these rules and had to repeat them many times because I kept
violating them. Do not repeat that. The rules exist because they *work*: every
significant bug this session was found by obeying them, and every wrong turn
came from ignoring them.

### 1.1 NO GREP. NONE. EVER.

This includes all of the following, all of which I did and all of which the
user counted as grep:

- `grep`, `rg`, `ag`
- `pgrep` (it is grep for processes — use `pidof`)
- `awk '/pattern/'`, `sed -n '/pattern/p'` used to *find* things
- `python3 -c "... if 'x' in line ..."` filtering a build log
- `re.findall` / `re.compile` scans over source files
- `python3 - <<EOF` blocks doing `assert old in s; s.replace(old, new)`

For edits use `desktop-commander:edit_block`. For reading use
`desktop-commander:read_file`. For build results use `ninja … | tail -N` and
read the output — ninja prints `FAILED:` with the compiler message at the end.

Note: the container's `str_replace` tool CANNOT see `/home/racerx`. Edits on the
user's machine must go through `desktop-commander:edit_block`.

`find -name` for locating files appears to be tolerated; `grep` for content is
not. When in doubt, list the directory and read.

### 1.2 NO SKIP-READING

Do not read a file at a guessed offset. Do not jump to where you think a
function lives. Read files from line 1, in order, to the end.

I read `Win32Dlg.cpp` at offsets 1, 80, 1090, 2180, 2280 hunting for functions
in a file *I wrote myself*, and `windows.h` at 1284, 1516, 1584, 1651 looking
for `sprintf_s` — which turned out to be at line 1027. That is the entire
argument for the rule.

When I finally read `windows.h` end to end (1723 lines, three sequential
reads), it took one turn and I had the whole file.

### 1.3 READ THE WINDOWS CODE AND CROSS-REFERENCE — DO NOT COPY THE SCREENSHOT

The user sends side-by-side screenshots as *evidence that something is wrong*,
not as a spec to pixel-match. The specification is:

- `Src/Orbiter/Orbiter.rc` — every dialog, every control, every style word
- each plugin's `.rc`
- the Orbiter `.cpp` sources themselves

Find the mistake **in the code**. I wasted three rounds on tree lines by
reacting to a screenshot: I drew them from a style bit, the user objected, I
*invented a rule* to hide them instead of rechecking, then the real Windows
screenshot showed they belong after all and my rendering was simply ugly.
Reacting to the complaint instead of rereading the code turned one wrong
rendering into a wrong rule.

### 1.4 DO NOT ASSUME SOMETHING IS ABSENT — LOOK

I told the user four times that TerrainToolKit, GenericCamera, DrawOrbits and
DX9ExtMFD were "not in this repository." All four are in
`OVP/D3D9Client/samples/`. I inferred absence from `Src/Plugin` not containing
them and stated it as fact. **Everything is in the repository. Look for it.**

### 1.5 Other standing rules

- No rewrites of Orbiter's own logic. Targeted, additive edits, `#ifdef`-guarded
  where it touches shared code.
- Never stub a feature that has a real implementation.
- Never claim something works without visually/behaviourally confirming it.
- Do not ask questions whose answers are in the code.
- Do not make premature claims ("it's running", "that's fixed").

---

## 2. Environment

```
Working tree : /home/racerx/orbiter-native/
Reference    : /home/racerx/Orbiter/orbiter/   (read-only, pristine)
Build        : /home/racerx/orbiter-native/build-dbg   (Debug, Ninja)
```

Build and run:

```bash
cd /home/racerx/orbiter-native
ninja -C build-dbg Orbiter -j16 2>&1 | tail -8

cd build-dbg
export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 \
       DISPLAY=:0 XAUTHORITY=/run/user/1000/xauth_dZAGap
if pidof Orbiter > /dev/null; then kill $(pidof Orbiter); sleep 2; fi
rm -f Orbiter.log
setsid ./Orbiter > /tmp/run.txt 2>&1 < /dev/null & disown
sleep 8; echo "pid: $(pidof Orbiter)"; cat Orbiter.log
```

Force X11 instead of Wayland: `export ORBITER_GLFW_PLATFORM=x11`
Message tracing: `ORBITER_TRACE_MSG=1 ./Orbiter`

Long builds: `desktop-commander:start_process` with `timeout_ms` up to 900000.

---

## 3. IMMEDIATE NEXT TASK — finish TerrainToolKit

It is the **only** module still failing. Everything it needs is declared; two
functions are declared but **not implemented**, so the tree is currently in a
state where `TerrainToolKit` will fail to link. Fix this first.

Declared in `Src/Orbiter/Linux/windows.h` (near the end, by the clipboard
block), implementations still required:

1. **`TransparentBlt`** → implement in `Src/Orbiter/Linux/Gdi.cpp`, next to the
   existing `BitBlt` / `StretchBlt`.
   ```c
   BOOL TransparentBlt(HDC dst, int x, int y, int w, int h,
                       HDC src, int sx, int sy, int sw, int sh,
                       UINT transparentColour);
   ```
   A masked blit: pixels equal to `transparentColour` are not copied.

2. **`TrackMouseEvent`** → implement in `Src/Orbiter/Linux/Platform.cpp`.
   ```c
   BOOL TrackMouseEvent(LPTRACKMOUSEEVENT tme);
   ```
   Arms a `WM_MOUSELEAVE` post to `tme->hwndTrack` when the pointer leaves it.
   The `TRACKMOUSEEVENT` struct and `TME_LEAVE` / `WM_MOUSELEAVE` are already
   defined.

Then: `ninja -C build-dbg TerrainToolKit -j8` and keep going — expect a few
more shim gaps, and add them the same way.

---

## 4. SECOND TASK — module categories are cross-assigned at runtime

**Symptom.** The Modules tab shows modules under the *wrong* categories, and
`LuaConsole` — which exports no category symbol at all — displays one anyway.

| Module | Its own `.rc` says | Launchpad shows |
|---|---|---|
| LuaMFD | MFD modes | Miscellaneous |
| ScriptMFD | Script tools and drivers | Tools and dialogs |
| DrawOrbits | Miscellaneous | Tools and dialogs |
| DX9ExtMFD | Tools and dialogs | SDK sample plugins |
| LuaConsole | *(has no `.rc`)* | SDK sample plugins |

**The generated strings are correct** — verified by reading the generated
`*_modulestrings.cpp` files and `nm -D` on the `.so` files. This is not a
conversion bug.

**Diagnosis.** Every module exports an identically-named
`orbiterModuleCategory` / `orbiterModuleDescription`.
`ModuleTab::RefreshLists` calls `LoadString(hMod, 1000/1001)`, and the Linux
`LoadString` resolves those by `dlsym`. `LuaConsole` having no such symbol yet
still showing a category proves the lookup is **not scoped to the handle it was
given** — it is finding another module's definition.

**Where to fix.** `Src/Orbiter/Linux/Platform.cpp`, functions `LoadLibraryExA`
and `LoadStringA`. Read them together, in full. Expected fix: `dlopen` the
`LOAD_LIBRARY_AS_DATAFILE` case with `RTLD_LOCAL | RTLD_NOW` into its own
handle, and `dlsym` strictly against that handle, never `RTLD_DEFAULT` /
global scope. Also check whether `Orbiter::LoadModules` already `dlopen`ed the
module with `RTLD_GLOBAL`, which would pollute the global namespace.

---

## 5. Module status

**Building and loading (16):**
AscentMFD, DrawOrbits, DX9ExtMFD, ExtMFD, FlightData, Framerate,
GenericCamera, LuaConsole, LuaMFD, Meshdebug, Notes, Rcontrol, ScnEditor,
ScriptMFD, TrackIR, TransX

**Failing (1):** TerrainToolKit — see §3.

**Not built, deliberately:** XRSound. It is in `Sound/XRSound`, gated behind
`ORBITER_BUILD_XRSOUND`. Windows lists it under "Sound module for Orbiter".
irrKlang has real Linux `.so` support — **implement it properly, do not stub
it.**

**Correctly "Miscellaneous":** ExtMFD, FlightData, Framerate, Notes, Rcontrol
have **no `.rc` anywhere in this repository**, so they have no category
resource. `ModuleTab`'s default applies. This is right for this source tree;
the released Windows builds the user compares against were built from a tree
where those files exist.

---

## 6. What was accomplished this session

### 6.1 Plugin loading was completely broken

`Orbiter::LoadModules` scanned for `extension() == ".dll"`, which can never
match a `.so`, and `FindStandaloneDll` / `FindDllInPluginFolder` built paths
with backslashes *and* a hardcoded `.dll`. **No plugin had ever loaded.** Fixed
with a `MODULE_EXT` define and forward slashes.

### 6.2 The Modules tab activation sequence never ran

`ModuleTab::OnNotify` counts `NM_CUSTOMDRAW` notifications — a real tree view
sends one per paint — to drive its entire activation sequence:

```cpp
case NM_CUSTOMDRAW:
    if (counter >= 0 && counter < 4) {
        if (counter == 2) PostMessage(hDlg, WM_USER, 0, 0);  // InitActivation
        counter++;
    } else if (counter == 4) ActivateFromList();             // Load/UnloadModule
```

My tree sent none, so nothing was ticked, nothing expanded, and checking a box
could not load anything. Added `orbiter_NotifyTreeCustomDraw`.

### 6.3 `TVM_INSERTITEM` never assigned a state image

A tree created with `TVS_CHECKBOXES` gives every item state image 1 (unchecked
box). Proof: `InitActivation` explicitly *removes* them from category rows with
`SetItemState(0, TVIS_STATEIMAGEMASK)`. Mine started at 0, so no module had a
check box at all.

The style cannot be read live — `ModuleTab::OnInitDialog` rewrites `GWL_STYLE`
*without* `TVS_CHECKBOXES` before any item is inserted. Recorded from the
template instead (`Window::treeCheckboxes`).

### 6.4 Font was 15% too small

`ImGui::AddFontFromFileTTF`'s size argument is **not** the em size — it goes to
`stbtt_ScaleForPixelHeight`, which scales so *ascent+descent* equals it. Tahoma's
line box is 2472/2048 = 1.207 em, so `11.0f` gave an em of 9.1px where Windows
uses `-MulDiv(8,96,72)` = 10.667px. Now computed:

```cpp
const float kEmPx      = 8.0f * 96.0f / 72.0f;   // 10.667
const float kLineBoxEm = 2472.0f / 2048.0f;      // 1.207
const float kFontPx    = kEmPx * kLineBoxEm;     // 12.88
```

### 6.5 Window resize did nothing

Two missing halves: the swapchain was only rebuilt on
`VK_ERROR_OUT_OF_DATE_KHR`, which Wayland does not reliably raise (so the old
frame was scaled — the "stretched fonts"), and `WM_SIZE` was never sent, so
`LaunchpadDialog::Resize` never ran. Both fixed in `orbiter_PumpFrame`.

### 6.6 Control rendering was branching on class, ignoring style

Every one of these was in `Orbiter.rc` the whole time:

- **`TVS_HASBUTTONS`** — no Launchpad tree except `IDC_EXT_LIST` has it. I drew
  expander triangles on all of them. Windows draws `[+]`/`[-]` **boxes**, not
  triangles.
- **`TVS_HASLINES` / `TVS_LINESATROOT`** — the scenario list has lines; the
  Extra list has lines *and* root lines. Must be dotted, not solid.
- **`TVS_FULLROWSELECT`** — absent everywhere, so the selection highlight
  covers the **label**, not the row.
- **`BS_RIGHT`** — every Launchpad tab button carries it
  (`PUSHBUTTON "Scenarios ",IDC_MNU_SCN,…,BS_RIGHT | WS_GROUP`). I centred
  them all; my helper was even *named* `drawCentred` with a comment asserting
  push buttons always centre.
- **`ES_MULTILINE`** — description panes.

Also: the label was offset 18px for an icon even when no image list existed
(`tx = tex ? ix + 18 : ix`).

### 6.7 Other UI fixes

- **Double drop-down arrows on every combo.** `ImGui::BeginCombo` with
  `NoPreview` still draws its own arrow button at the cursor position (left
  edge). Replaced with `InvisibleButton` + own `BeginPopup`.
- **Folders would not collapse.** `TVM_SELECTITEM` expanded the path starting
  *at* the selected item, so any click that selected a folder re-expanded it,
  and the double-click toggle always undid a collapse. Now walks from
  `->parent`.
- **Sibling draw order.** Reverted to template order. The Options page proves
  it: `IDC_OPT_SPLIT` is listed *first* and spans both panes; if first were
  topmost the tab would be unusable on Windows too. Last-created is topmost.
  The sibling-exclusion in the custom-control branch is what actually fixes
  splitter click-swallowing.

### 6.8 Module description/category conversion

`Src/Orbiter/Linux/rcstrings.py` converts a plugin's `.rc` STRINGTABLE into
exported `orbiterModuleDescription` / `orbiterModuleCategory` symbols, wired
from `Src/Plugin/CMakeLists.txt`, `OVP/CMakeLists.txt` and
`Orbitersdk/samples/CMakeLists.txt` at **configure** time (build time fails —
`GENERATED` is directory-scoped and cannot be set on a target in a subdirectory).

Details that mattered:
- RC doubles a quote to escape it (`""Scenario editor""` → `\"`).
- A backslash not starting a known escape is literal (`Doc\TransX`).
- STRINGTABLE blocks use **`BEGIN`/`END` or braces** — both appear in this tree.
- `Orbits.rc` and `ExtMFD.rc` `#include <winres.h>`; without a shim for it the
  preprocessor aborted and the parser silently reported "no strings".

### 6.9 The OVP samples were gated behind DirectX for no reason

`OVP/CMakeLists.txt` only descended into `D3D9Client` when
`ORBITER_BUILD_D3D9CLIENT` was set. But DrawOrbits, DX9ExtMFD, GenericCamera
and TerrainToolKit link only `${OrbiterTgt}` and `Orbitersdk` — their
`add_dependencies(… D3D9Client)` is build *ordering*, not code. Added a
non-Windows path that builds them and guarded the dependency with
`if(TARGET D3D9Client)`.

### 6.10 Separator bugs (found by reading, never by searching)

Nine now, in five distinct shapes — which is exactly why a pattern search
misses them:

| Site | Literal | Effect |
|---|---|---|
| `Vessel::OpenConfigFile` | `"Vessels\\"` | 12 vessel configs unloadable → `TerminateOnError()` |
| `Vessel::RegisterModule` | `"Modules\\%s.dll"` | **every** vessel module fails to load |
| `Orbiter::FindStandaloneDll` | `"%s\\%s.dll"` | every plugin fails to load |
| `Orbiter::FindDllInPluginFolder` | `"%s\\%s\\%s.dll"` | same |
| `Orbiter::LoadModule` | `"%s\\%s"` (cwd join) | same |
| `Planet::ScanBases` | `"%s\\%s"` | every surface base unloadable |
| `Planet::Planet` | `strcat(cbuf,"\\")` | per-planet labels never found |
| `PlanetarySystem::Read` | `push_back('\\')` | marker directory unopenable |
| `TransX/globals.cpp` | `"Config\\MFD\\TransX.cfg"` | config unreadable |

### 6.11 MSVC-vs-standard C++ bugs

- **`friend class X;` used as a declaration.** MSVC injects friend names into
  the enclosing namespace (non-conforming). Hit twice: `ConsoleConfig` in
  `LuaConsole.h`, `gcGUIApp` in `gcGUI.h`. Both need a real forward
  declaration.
- **Circular include.** `ConsoleInterpreter.h` ↔ `LuaConsole.h`; whichever
  guard sets first leaves the other's type undeclared.
- **`sprintf_s(buf, fmt, …)`** — MSVC has a template overload deducing the
  array size. Only the explicit-size form existed (at `windows.h:1027`), so the
  format bound to the size parameter.
- **`ifstream::open(path, NULL)`** — MSVC extension; standard needs
  `ios_base::openmode`.
- **`inline void Swap(long*, long*)` on `RECT` fields** — `LONG` is 32-bit on
  every Windows target, so `long` matches there by coincidence; not on LP64.
- **`FMATRIX4::_x/_y/_z/_p`** — an anonymous struct of `FVECTOR4`, which GCC
  rejects in a union because `FVECTOR4` has constructors. Added
  `FMATRIX4::SetRow(int, const FVECTOR4&)` naming the same storage; converted
  `DrawOrbits/Draw.cpp` (2 sites) and `TerrainToolKit/Basics.cpp` (1 site).
  **The old comment claiming these are "referenced only by OVP/D3D9Client,
  which is Windows-only" was false.**
- **`-lLuaInterpreter`** — same as `-lVsop87`: the module is `LuaInterpreter.so`
  with no `lib` prefix, so it must be linked by full path with an rpath.

### 6.12 Header aliases added (case/name variants)

`gcCoreAPI.h` → `gcCore.h` (**the file was renamed upstream; this alone
unblocked LuaInterpreter, LuaConsole, LuaMFD and ScriptMFD**),
`OrbiterSDK.h`, `OrbiterSdk.h`, `ORBITERSDK.H`, `Orbitersdk.H`, `MfdApi.h`,
`Commctrl.h`, `Commdlg.h`, `CommDlg.h`.

New shim headers: `intrin.h`, `winres.h`, `commdlg.h`.

New in `windows.h`: tab control (`TCM_*`, `TCITEM`, `TC_ITEM`, `TabCtrl_*`,
`TCN_SELCHANGE`), `TVS_*`, `WMSZ_*`, `WM_SIZING`, `_countof`, `sscanf_s`,
`_isnan`/`_finite`/`_isinf`, `DEFAULT_CHARSET`/`FF_*` font constants, `CS_*`
class styles, `GetWindowLong`, `CreateDialogParamA`, `WS_EX_STATICEDGE`,
`INVALID_FILE_ATTRIBUTES`/`FILE_ATTRIBUTE_*`, clipboard + `GlobalAlloc` family,
`PtInRect`, `TRACKMOUSEEVENT`.

New in `Platform.cpp`: `GetFileAttributesA`, `CreateDirectoryA`,
`DeleteFileA`, `GlobalAlloc`/`Lock`/`Unlock`/`Free`/`Size`, `OpenClipboard`,
`CloseClipboard`, `EmptyClipboard`, `SetClipboardData`, `GetClipboardData`.

New in `commctrl.h`: `TB_*` trackbar notification codes.

### 6.13 Build system

- `Orbitersdk/samples/CMakeLists.txt` was the one module directory missing the
  prefix-clearing block — hence `libAscentMFD` in the Modules list.
- `TrackIR` linked `UxTheme.lib`; now Windows-only.

---

## 7. Launchpad trace status

`Src/Orbiter/Linux/LAUNCHPAD-TRACE.md` tracks every Launchpad control to the
simulator function it ultimately drives.

**Read end to end:** `Vessel.cpp` (9045), `Planet.cpp` (1054),
`Rigidbody.cpp`/`.h`, `Vesselbase.cpp`, `UIHost.cpp` (~1990), `windows.h`
(1723), `Launchpad.cpp` (618), `TabModule.cpp` (429), `TabExtra.cpp` (600 of
1586), `OptionsPages.cpp` (2422), `TabScenario.cpp` (761).

**Partially read:** `Win32Dlg.cpp` (continuous 1–1180 and 2180–end; gap
1180–2180), `Config.cpp`, `Psys.cpp`, `Vessel.h`.

**Traced to an end function (12):** the entire Physics page (nonspherical
gravity → Pines harmonics; radiation pressure → `UpdateRadiationForces`;
gravity-gradient torque → `EulerInv_full`; atmospheric wind →
`Planet::WindVelocity` → `airvel_ship`), three of four Vessel settings
(limited fuel, complex flight model, damage), particle streams, cloud layers,
cloud shadows, max resolution level, time propagation.

**Still open:** `bPadRefuel`'s caller, Instruments → `Pane.cpp`, joystick →
`DInput::OptionChanged`, most Visual settings (need the Vulkan client),
Video tab → `clbkCreateRenderWindow`, Launch button → `InitializeWorld`.

---

## 8. Known-good architecture notes

- Geometry is **pixels** after template instantiation (dialog units converted
  once, base units 6/13).
- Window words: `DWLP_MSGRESULT`/`DLGPROC`/`USER` on dialogs; byte-offset
  `cbWndExtra` on custom-class windows. Offset 0 means different things for
  each — the window's kind disambiguates.
- `WM_CTLCOLOR*`: the **return value is the HBRUSH**, not TRUE/FALSE.
- `WS_CHILD` = nested tab page; `WS_POPUP` + owner = top-level modal.
- Notifications are **posted, not sent** — sending re-enters `ImGui::NewFrame`
  from inside the paint pass and asserts.
- Up-down controls report `iDelta = -1` for the **up** arrow; consumers negate
  it.

---

## 9. CORRECTIONS — session of 2026-08-30, later

Everything in §3 and §4 above was written without building or running. Four
of its claims are wrong. Verified by compiling, running and sampling pixels.

### 9.1 §3 was wrong. TerrainToolKit is DONE and in the Modules list.

It did not fail on the two functions §3 names. It failed to *compile*, in
`gcTableView.cpp`, on nine missing declarations. Now implemented:

`CreateCompatibleBitmap`, `FillRect`, `GetTextExtentExPointA`, `HRGN`,
`CreateRectRgn`, `SelectClipRgn`, `ExcludeClipRect`, `TextOutW` (windows.h +
Gdi.cpp); `TBS_NOTICKS` / `TBS_TRANSPARENTBKGND` / `TBS_BOTH` (commctrl.h).
`TransparentBlt` is a real masked blit — alpha zeroed where pixels match the
key, cached per (bitmap, key).

**`TrackMouseEvent` was never needed.** Nothing in the module calls it. It is
still declared and still unimplemented; that is harmless.

`Msimg32.lib` / `Comctl32.lib` in the module's CMakeLists are now `if(WIN32)`.

### 9.2 `rcstrings.py` fails silently, and it cost the category again

`ToolKit.rc` includes `<richedit.h>`, which had no shim. The script
preprocesses with `cpp -P -DRC_INVOKED`, the include aborted it, and the
parser reported "no STRINGTABLE" — so
`IDS_TYPE "Developer resources and samples"` was dropped and the module
defaulted to Miscellaneous. Added `Src/Orbiter/Linux/richedit.h`.

**This is the same failure as the `winres.h` one in §6.8 and it will recur.**
`rcstrings.py` must fail the build on a preprocessor error instead of
reporting zero strings. NOT YET DONE.

### 9.3 PORTING-RULES.md's first bullet is wrong — dlgcol is NOT the Launchpad colour

`Launchpad.cpp` lines 398-399:

    //	case WM_CTLCOLORDLG:
    //		return (LRESULT)hDlgBrush;

Commented out. The main dialog never uses `dlgcol`. `hDlgBrush` is returned
only by `WaitProc` — `dlgcol` is the LOADING SCREEN colour. The Launchpad
takes the default dialog face, and `IDC_MNU_PAGECONTAINER` is filled
explicitly with `GetSysColorBrush(COLOR_3DFACE)` in `WM_DRAWITEM`.

Measured: Windows Launchpad page background is (245,245,245) neutral. Linux
was painting (248,244,240) warm. Fixed in `UIHost.cpp` (`kDialogBk` is now
COLOR_3DFACE; `kWaitPageBk` holds dlgcol for the wait page). Verified
(240,240,240).

### 9.4 §4 points at the wrong file

`LoadStringA` is **not in Platform.cpp**. `nm -C --defined-only` on the
objects puts it in **`Win32Dlg.cpp`** (`T LoadStringA`). `LoadLibraryExA` IS
in Platform.cpp and is already correct — it forwards to `LoadLibraryA`, which
uses `RTLD_NOW | RTLD_LOCAL`. So the global-scope leak is inside
`LoadStringA` itself. Read `Win32Dlg.cpp` end to end and fix it there.

Still visibly wrong, confirmed against a running Windows Launchpad:
TerrainToolKit, LuaMFD -> Miscellaneous; DX9ExtMFD, LuaConsole -> SDK sample
plugins; DrawOrbits, ScriptMFD -> Tools and dialogs. "Script tools and
drivers" never appears. Correct on both sides: Meshdebug, TrackIR,
GenericCamera, TransX, AscentMFD, ScnEditor.

### 9.5 BitBlt and StretchBlt were discarding the source rectangle

Every blit drew the whole source image squashed into the destination.
`PropertyList::OnPaint` in `DlgCtrl.cpp` picks its expand/collapse arrow out
of a 14px strip with `BitBlt(..., 28 + (expanded ? 0 : 14), 0, SRCCOPY)`, so
the property list arrows were wrong too. Now carried through to UVs. FIXED.

### 9.6 YOU CAN SEE THE SCREEN. USE IT.

    spectacle -b -n -f -o /tmp/shot.png     # works on this KDE Wayland session

Then crop/downscale with PIL and read it back with
`desktop-commander:read_file` (it renders images). `im.getpixel((x,y))` gives
exact RGB. The user keeps a Windows Launchpad open on the right monitor —
diff against it numerically instead of guessing or asking.

Do not claim a visual fix without doing this.

### 9.7 Open, in the order worth doing

1. `LoadStringA` in `Win32Dlg.cpp` — the category cross-assignment.
2. Tab page does not fill the window. Windows' Modules tree runs to the
   bottom of the frame; the Linux page stops ~190px short and leaves grey.
   `LaunchpadDialog::Resize` computes `tabAreaH` and calls
   `tab->TabAreaResized()`; either that is not arriving or the tabs ignore it.
3. Description pane has no scrollbar (`IDC_SCN_DESC` is `WS_VSCROLL`).
4. `rcstrings.py` silent preprocessor failure (9.2).
5. XRSound — irrKlang ships `.so`; `Sound/XRSound/CMakeLists.txt` still
   hardcodes `winx64-visualStudio`. Its absence is why the whole
   "Sound module for Orbiter" category is missing.
6. Eight celbody modules (Ariel, Deimos, Miranda, Oberon, Phobos, Titania,
   Triton, Umbriel) are prebuilt 32-bit Windows PE DLLs checked into
   `Src/Celbody/<name>/` with NO SOURCE. They cannot ever load on Linux.
   Their `.cfg` files name `Module = <name>`.
