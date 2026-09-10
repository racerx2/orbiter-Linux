# Orbiter Vulkan Client — Session Handoff
Written 2026-09-01. Read this first in a new session.

## Rules for working on this project
1. NO grep / pgrep / ripgrep / findstr. Use read_file, edit_block, `find -name`,
   awk, sed line ranges.
2. NO skip-reading. Read files from line 1, in order, to the end.
3. Read the ACTUAL source — D3D9Client, the Orbiter core, VSG — not notes or
   assumptions.
4. Never claim something works without visual or measured confirmation.
5. Targeted additive edits. No rewrites of working Orbiter logic.
6. Leave the tree building. Half-finished edits caused at least four bugs in
   the last session.

## Build and run
    cd /home/racerx/orbiter-native
    ninja -C build-dbg Orbiter VulkanClient -j16

    cd build-dbg
    export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 \
           DISPLAY=:0 XAUTHORITY=/run/user/1000/xauth_GtxuEj
    ./Orbiter -s "Delta-glider/Cape Canaveral"

Validation layer (use it early, it finds real bugs):
    VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation ./Orbiter ... 2>&1 | grep-free check
    Count errors by reading the output for "Validation Error".

Backup: /home/racerx/orbiter-native-backup-20260901-005337.zip (649 MB, verified)

## WORKING (visually confirmed)
- Vessel meshes, fully textured, external view and F8 virtual cockpit
- Camera: right-drag rotate, scroll zoom, keyboard via Orbiter's own input path
- Mesh read/edit (clbkGetMesh / GetMeshGroup / EditMeshGroup)
- Mesh visibility filtered by camera mode
- Sun billboard
- clbkRender2DPanel: the DG 2D instrument panel renders, textured
- 0 Vulkan validation errors

## NOT IMPLEMENTED
- MFD render surfaces (clbkCreateSurfaceEx + render-to-texture) — the two black
  panels in the cockpit
- HUD/panel text — needs a glyph atlas. NOTE: text is 39 of 51 HUD commands.
- Planets, terrain, stars
- Lighting on meshes (needs the view descriptor set bound; Builder omits it)

## THE BIG BUG OF THE LAST SESSION — do not reintroduce
`viewer->compile()` sizes the state-slot replay limit by SCANNING THE SCENE at
that moment. This client's scene is empty then (vessels are built per frame on
first sight), so slot 2 — where BindDescriptorSet with firstSet=1 lives — was
never recorded. Every mesh sampled whatever texture was bound first.

Fix, in clbkCreateRenderWindow:
    auto hints = vsg::ResourceHints::create();
    hints->maxSlots.state = 4;
    hints->maxSlots.view  = 2;
    hints->numDescriptorSets = 4096;
    m_viewer->compile(hints);

Hours were lost to this. It looked like a texture bug and was not.

## HARD-WON FACTS
- `SPEC_INHERIT` means INHERIT the previous group's texture/material, NOT skip
  the group. See D3D9Client::ProcessInherit.
- `UsrFlag & 2` means DO NOT DRAW. The group must still exist for
  GetMeshGroup/EditMeshGroup — DeltaGlider's FuelMFD sets the flag then reads
  the group back and dereferences the result without checking. Dropping it
  segfaults the VC.
- `TEXIDX_MFD0 = (UINT)(-1) - MAXMFD` — near the TOP of the unsigned range.
- clbkRender2DPanel's MATRIX3 T is a PROJECTION, not a vertex transform. Only
  m11, m22, m13, m23 are used:
      screen_x = v.x * m11 + m13
      screen_y = v.y * m22 + m23
  It is called several times per frame with different matrices (HUD, panel), so
  bake it per vertex — do not set a shared camera projection.
- hSurf[TexIdx] is 0-BASED. The mesh's own texture list is 1-based via
  oapiGetTextureHandle(hMesh, TexIdx+1).
- vsg_Color is INSTANCE rate. One vec4 per draw, not one per vertex. Supplying
  a per-vertex array silently draws nothing.
- The class is BindDescriptorSet (SINGULAR). VSG also has BindDescriptorSets;
  casting to the plural matches nothing and fails silently.
- The overlay camera must put geometry at DEPTH 0 (eye at origin, near/far
  0..1). Depth 1.0 fails the less-than test against a cleared buffer.
- Per-group TEXWRAP: bit 0x01 = U, 0x02 = V. Default is NO wrapping (clamp).
- Winding IS reversed here (i, i+2, i+1) because toBodyAxes has determinant -1.
  D3D9Client does not reverse, because it never transforms vertices.
- Orbiter acquires a Sketchpad MANY times per frame; only some acquisitions
  draw. Capture geometry at clbkReleaseSketchpad, while the recording is intact.

## DOCUMENTATION — read it, it is good
- Doc/Orbiter Developer Manual/MESH.tex        — mesh format, FLAG bits, TEXWRAP
- Doc/Orbiter Developer Manual/SURFACES.tex    — surface types, SKETCHPAD ==
                                                 RENDERTARGET, 'D' = decompress
- Doc/Orbiter Developer Manual/GRAPHICS_CLIENT.tex — particle streams
- Doc/Orbiter Developer Manual/FLOW.tex        — frame loop, callback order
- OVP/D3D9Client/doc/D3D9Client.html           — user manual, material model
- OVP/D3D9Client/doc/D3D9Client_API_Reference.chm — extract with 7z

## REFERENCE SOURCE
- OVP/D3D9Client/ — the working, current client. THE reference. Read it first.
- ~/Downloads/VulkanSceneGraph-master/ — VSG 1.1.16 source
- ~/Downloads/vsgImGui-master/ — for in-session dialogs; submodules are EMPTY,
  needs a recursive git clone
- Skybolt client was deleted; it is 4 years stale and has no HUD at all.

## DIAGNOSTIC ENV VARS
  ORBITER_VK_EXTERNAL     start in external view, close to the focus vessel
  ORBITER_VK_LOOKAT=NAME  aim the camera at a named vessel
  ORBITER_VK_STANDOFF=N   camera distance for the above
  ORBITER_VK_GRID         reference cube grid
  ORBITER_VK_NOSUN        omit the Sun billboard
  ORBITER_VK_TRACEMESH    per-mesh group/texture/material counts
  ORBITER_VK_TRACETEX     every clbkLoadTexture call
  ORBITER_VK_TRACEVIS     per-vessel mesh visibility
  ORBITER_VK_TRACEMAT     per-group material and texture state
  ORBITER_VK_TRACECAM     camera, projection, bearings
  ORBITER_VK_TRACEPAD     Sketchpad lifecycle and overlay publishing
  ORBITER_VK_TRACEKEY     key events and DIK mapping
