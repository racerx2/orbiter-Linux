# Working rules for the Linux port

## 1. Read the code. Do not grep it.

Grep only returns what you already suspected. It cannot show you the thing you
did not know to look for, and on this port that has been where every real bug
lived.

Read files end to end before changing anything that touches them.

### What grepping cost, concretely

Every one of these was found by reading a file, and every one had been missed
by repeated searching of the same file:

- **`Launchpad.cpp:39`** — `const DWORD dlgcol = 0xF0F4F8;`
  The Launchpad has its own background colour. As a COLORREF that is
  RGB(248,244,240), not the COLOR_3DFACE grey that was being painted. No
  search would surface a constant whose name you have not guessed.

- **`Launchpad.cpp:384`** — `WM_CTLCOLORSTATIC` for `IDC_BLACKBOX`
  sets pale text on black and returns `BLACK_BRUSH`. That panel is black for
  this reason and no other; its template entry carries no special style. The
  appearance is in the message handler, not the resource.

- **`Launchpad.cpp:218`** — `Resize()` repositions the buttons, the copyright
  panel, the shadow, the version label and the page container on every
  `WM_SIZE`, and `Create()` calls it once at startup. The layout is
  template-then-Resize, not template alone.

- **`Util.cpp:64`** — `GetClientPos` is `GetWindowRect` + `ScreenToClient`,
  so the whole geometry system is pixels. Storing dialog units past dialog
  creation put every control Resize touched at about two-thirds scale.

- **`LpadTab.cpp:92`** — each tab page is a child dialog parented to
  `IDC_MNU_PAGECONTAINER`, and `TabProcHook` keeps its object pointer at
  `DWLP_USER`.

- **`DlgCtrl.cpp:18-21`** — a gauge keeps position, range min, range max and
  flags at byte offsets 0, 8, 16, 24 of its `cbWndExtra`.

- **`CustomControls.cpp:30`** — `CustomCtrl::SetHwnd` stores `this` at window
  word 0 and reads `GWLP_HWNDPARENT` for its parent.

The last three together are one bug: index 0 means "message result" on a
dialog and "the control's own state" on a custom-class window. Two address
spaces sharing one slot. Reading three files made it obvious; searching for
symbol names never would have, because the collision is between things that
have no name in common.

## 2. The appearance is in the tree. Distinguish two cases.

- **Orbiter draws it** — the owner-drawn statics, the `DlgCtrl` gauges and
  switches, the property list, `CustomControls`. There is nothing to imitate
  here. Make the GDI calls work and the real appearance follows, because it is
  Orbiter's own code running.

- **Windows draws it** — button, static, edit, combo, listbox, tree view, tab
  control, trackbar, up-down. Only these are imitated, and `Orbiter.rc` is the
  specification for where they go.

Guessing at the first category produces something that looks invented. Settle
which category a control is in before changing it.

## 3. Constraints on this port

- Targeted edits. The original code is preserved; changes are `#ifdef`-guarded
  or additive wherever possible.
- No rewriting of Orbiter's own logic. The Launchpad especially: a previous
  attempt at rewriting it failed, and keeping the source untouched means a
  missing piece shows up as an undefined symbol rather than as subtly wrong
  behaviour.
- Native Linux. No Wine, no compatibility layer beyond the in-process Win32
  shim under `Src/Orbiter/Linux/`.
- Vulkan for graphics.
- The goal is that it looks and behaves as it does on Windows, not merely that
  it runs.

## 4. DO NOT TRUST THE SERVER PORT. Cross-reference it against Win32.

The Linux port of the Orbiter core -- everything under `Src/Orbiter/Linux/`,
plus the `NOT WIN32` additions scattered through `Src/`, `Orbitersdk/` and the
CMake files -- was written before these rules were, and without the
file-by-file cross-check against the Windows original that the graphics client
got. **Treat it as suspect code, not as reference code.**

The reference is exact and always available: the base commit
`ce32858854aa67277ea61528afad9cfadd19852b` **is** upstream Win32 Orbiter, so

    git show HEAD:<path>

gives the pristine Windows original for any server file, exactly the way
`OVP/D3D9Client` is the reference for the Vulkan client. Same workflow: read
the reference end to end, read the port end to end, compare, and account for
every difference.

A useful first check on any server file:

    git show HEAD:<path> > /tmp/ref.cpp
    diff /tmp/ref.cpp <path>          # lines marked '<' are reference code
                                      # the port CHANGED or DELETED

A port that only ever *adds* is one you can reason about. Lines the port
removed or rewrote are where to look first.

### Why this rule exists — the score so far

Every defect found on 2026-09-02 was server-side. Not one was in the graphics
client:

| Defect | File |
|---|---|
| Render-complete semaphore indexed by the rotating counter | `Linux/UIHost.cpp` |
| Submitted frame abandoned before present on SUBOPTIMAL | `Linux/UIHost.cpp` |
| `SemaphoreIndex` never reset on swapchain rebuild | `Linux/UIHost.cpp` |
| Render pass destroyed under the client on every resize | `Linux/UIHost.cpp` |
| Shared depth image unsynchronised between frames | `Linux/UIHost.cpp` |
| `CMAKE_DEBUG_POSTFIX` renaming every module | root `CMakeLists.txt` |
| `DllMain` defined twice once `-u` forces the archive member in | `Src/Orbitersdk/Orbitersdk.cpp` |
| `InitLib` reference silently unmangled | `Src/Orbitersdk/Orbitersdk.cpp` |
| Module-to-module linking decomposed to `-l<name>` | celbody / Atlantis CMake |

The last two are worth studying as a pair, because they are the shape this
port's bugs keep taking: **something that works only by accident, and stops
the moment you touch something unrelated.** `InitLib` resolved only because a
dead `DllMain` happened to appear earlier in the file and fixed the symbol's
linkage; guarding that dead function to Windows -- which changed nothing that
runs -- broke the link of the Orbiter executable itself.

### Corollary: a working build directory proves nothing

`build-dbg` reported success for days while a fresh configure was broken,
because its artifacts predated its own CMakeCache and ninja is satisfied by a
file that already exists. **Verify in a clean build directory**, and build the
whole target set, not just `Orbiter` and the client:

    cmake -S . -B /tmp/probe -G Ninja -DCMAKE_BUILD_TYPE=Debug \
          -DORBITER_BUILD_VULKANCLIENT=ON
    cmake --build /tmp/probe -j"$(nproc)" -- -k 0

`-k 0` so ninja reports the complete failure set instead of stopping at the
first. `Utils/` (plsplit, pltex, texpack, tileedit) is expected to fail: those
are unported Windows command-line tools.

## 5. Verify before claiming

"It builds" is not "it works". "It runs" is not "it looks right". A change is
described by what has actually been established about it, and what is still
missing is stated plainly rather than left behind what succeeded.
