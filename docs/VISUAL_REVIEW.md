# Visual review: chunk LOD and sub-voxel destruction

Reviewed against `RelWithDebInfo`, seed `8525033820662387641` (the default), load
radius 20, RTX 3050 Laptop, 1600x900. Build is warning-free and 181/181 tests pass
at the state described here.

Three defects were found and fixed, two of which are invisible to unit tests and
to any still taken at the default settings. Two more remain open and are argued
below. Every claim in this document is backed by a PNG in `docs/images/` that was
opened and looked at, not inferred.

---

## 1. How the shots were taken

Comparing "LOD on" with "LOD off", or "before a fix" with "after", only means
anything if the two frames are the SAME frame. A hand-driven first-person camera
cannot reproduce a framing, so the review drives a scripted one:

* `src/app/Main.cpp` parses a debug command line (`voxl --help`). It places the
  player, aims the camera, freezes physics, overrides `LodPolicy`, and builds a
  carve rig. Everything is inert without arguments - a plain `voxl.exe` run is
  byte-for-byte the run it always was. See `voxl::DebugStartup` in
  `src/app/Application.hpp` for the rationale and the deletion criterion.
* `tools/visual_review.ps1` launches one process per shot, waits for streaming to
  settle, pins the window to the desktop origin (GLFW's default placement puts the
  bottom of the client area behind the taskbar, which `capture.ps1` would then
  grab), captures through `tools/capture.ps1`, and kills the process. A fresh
  process per shot is deliberate: chunk residency, LOD hysteresis and the
  sub-voxel store all carry state forward.

The two carve rigs:

| Rig | What it is | Why |
| --- | --- | --- |
| `--carve crater` | A 1.5-block-radius sphere of sub-voxels removed from the natural terrain surface | Puts partially destroyed grass and dirt directly against intact blocks under the same sky - the shading and texture comparison the review needs |
| `--carve tunnel` | A 5x6x7 stone slab raised 3 blocks clear of the ground, bored end to end with a 1-block-radius cylinder | Sky behind the far mouth turns any missing face into an obvious bright hole, and a slab standing in open air is the only place a carved surface is guaranteed to be lit while there is no light propagation to carry daylight underground |

Reproducing any shot, from the repository root:

```
powershell -ExecutionPolicy Bypass -File tools/visual_review.ps1 `
    -Out docs/images/vr_lod_ocean_on.png -SettleSeconds 18 `
    -GameArgs "--pos 0,178,0 --look 180,-5 --freeze --no-hud"
```

---

## 2. Defect table

| # | Defect | Severity | Where it shows | Status |
| - | ------ | -------- | -------------- | ------ |
| D1 | LOD skirt emitted on all four sides of every coarse chunk, including seams where both neighbours are at the same level. Over water the curtain is a second translucent quad coincident with the surface, and the doubled blend draws a dark lattice along every chunk border in the sea | **High** - covers the entire visible ocean | `vr_lod_ocean_before_fix_zoom.png` | **Fixed** |
| D2 | Skirt hung from the topmost NON-AIR cell, so an ocean column hangs a water curtain. Same doubled-blend mechanism as D1, surviving at LOD band boundaries after D1 was fixed | Medium | diff of the two fix stages: thin slivers along each band ring | **Fixed** |
| D3 | `SubVoxelMesher::resolveNeighbours` treated a partially destroyed neighbour as opaque when resolving light, so a cavity whose walls are all partial blocks fell through to the sealed-pocket fallback - and the fallback found only "solid" neighbours and stayed at zero. A bored tunnel rendered pitch black with daylight visible through the far end | **High** - any carved cavity more than one block across | `vr_subvoxel_bore_before_fix_zoom.png` vs `vr_subvoxel_bore_zoom.png` | **Fixed** |
| D4 | Sub-voxel UVs were `quad extent x corner selector`, i.e. zero at the quad's origin corner. A sub-voxel quad does not start on a block boundary, so every carved quad sampled the same strip from the edge of the texture. Carved surfaces rendered as smeared 1-D bands that did not line up with the intact faces beside them | Medium | `vr_subvoxel_crater_zoom.png` (after); the before state was a flat unvarying rim | **Fixed** |
| D5 | The LOD water plane steps 1-2 blocks between bands, and both sides draw a translucent border wall at the seam, leaving a serrated darker band around each LOD ring on open water | Low-Medium | `vr_lod_ocean_on_zoom.png` | **Open** - see §5 |
| D6 | At level 3 a thin ridge alternates between passing and failing the 40% solidity test, so distant spires break into a stack of apparently floating 8-block cubes | Low | `vr_lod_rings_on.png`, upper left, versus `vr_lod_rings_off.png` | **Open** - see §5 |

---

## 3. Per-shot verdicts

### LOD

**`vr_lod_rings_on.png` / `vr_lod_rings_off.png`** - aerial, `--pos 0,235,0 --look 40,-28`,
default bands `{5, 9, 14}`. LOD on: 872/1568/3136/4480 chunks resident at levels
0-3, 1 157 561 triangles. LOD off, fully streamed after 50 s: 10 056 chunks at
level 0, 7 317 628 triangles. A 6.3x triangle reduction for the same framing.

No cracks, no holes, no sky visible through terrain anywhere in either frame. The
coarse rings read as a loss of detail, which is what they are, not as damage. The
one genuine difference beyond resolution is D6: in the LOD frame the far-left
snowfield's ridges break into detached-looking cubes; the same ridges are
continuous with LOD off.

**`vr_lod_overlay.png`** - same framing with F3 open. Confirms all four levels are
actually in use rather than the policy silently doing nothing: visible
140/370/651/583 chunks and 603 080/423 992/112 036/18 452 triangles by level. The
outer 583 visible level-3 chunks contribute 1.6% of the triangles. 432 fps,
1892 draw calls. `transitions=0 dropped=0 inflight=0` - nothing was mid-rebuild
when the shutter opened.

**`vr_lod_ocean_on.png` / `vr_lod_ocean_off.png` / the two `_zoom` crops** - the
shot that found D1 and D2, `--pos 0,178,0 --look 180,-5`. This is why "look at the
water" belongs in every voxel LOD review: the defect was invisible on land because
matching opaque material hides coincident geometry, and total on water because
translucent material does not.

*Before:* a dark lattice, one line per chunk border, over the entire visible sea
(`vr_lod_ocean_before_fix_zoom.png`). *LOD off:* a clean uniform surface, proving
it was LOD and not the water shader. *After:* the lattice is gone
(`vr_lod_ocean_on_zoom.png`); three faint serrated lines remain at the band
boundaries - that is D5.

**`vr_lod_seam_hard.png` + `vr_lod_seam_hard_zoom.png`** - the primary crack hunt.
`--lod-bands 2,2,2` collapses the policy to a single level-0 -> level-3
transition at 64 blocks, which is the largest silhouette disagreement the system
can produce: 8-block cells against 1-block cells, up to 7 blocks of height
difference at the seam.

Verdict: **no crack, no hole, no z-fighting.** The zoom shows the boundary running
diagonally across the frame with fine 1-block terraces and detailed trees on the
near side and 8-block cells beyond, meeting cleanly. Two further checks in the
same frame:

* Texture density is identical across the boundary. An 8-block coarse dirt wall
  carries eight rows of the dirt pattern; a 1-block fine terrace carries one. One
  repeat per block at every level, exactly as `emitQuad` intends.
* The thin dark wedges along the boundary (visible near the sand at the right of
  the full frame) are real closed geometry - a 1-block step where the coarse cell
  quantised the ground down, with the fine side's dirt face exposed. Watertight,
  not a crack. Confirmed by zooming to 12x.

**`vr_lod_seam_tight.png`** - `--lod-bands 1,2,3`, everything past 96 blocks at
level 3. A whole landscape of coarse cells at close range. No cracks, no holes.
The recessed dark cells are cave mouths with unlit interiors, not gaps; a gap
would show sky. The snow/stone checkerboarding on distant peaks is the
majority-vote material choice flipping between two nearly equal counts - cosmetic,
inherent to the reduction rule.

**`vr_default_spawn.png`** - `voxl.exe` with no arguments at all, as a guard that
the harness did not become load-bearing. Spawn view, hotbar and crosshair present,
LOD active in the distance, no cracks.

**Popping** could not be judged from stills and is reported as untested here. The
mechanism is covered elsewhere: `LodPolicy`'s hysteresis is unit-tested, "a chunk
keeps its old mesh until the new level is ready" is unit-tested, and every
captured run reported `lodTransitionsDropped == 0`.

### Sub-voxels

**`vr_subvoxel_crater.png` + `_zoom`** - close-up of a sphere carved out of natural
grass and dirt. The rim is the comparison: partially destroyed blocks sit directly
against intact grass with no seam, no z-fighting and no double-drawn geometry. The
dirt on the carved wall is the block's own dirt texture at the block's own texel
density, and after D4 it varies from quad to quad as the intact faces do. Grass
tops survive on the sub-voxels that still have sky above them, which is the
correct per-direction texture choice.

The crater floor is dark. That is not a sub-voxel defect: there is no light
propagation in this build and the generator seeds sunlight 0 for everything at or
below the surface. Faces that genuinely see sky - the up-facing sub-voxels on the
rim - are lit at full sunlight in the same frame, which is what makes the gradient
readable. That a block-resolution pit dug in the same place would be equally dark
is read off the code rather than photographed: `GreedyMesher` takes a face's light
from the neighbouring cell, and a block carved to air below the surface keeps the
solid block's stored zero.

**`vr_subvoxel_slab.png`** - the bored slab from outside, three-quarter view. Bore
mouth is a clean circle in a face that is otherwise unbroken intact stone. No
z-fighting where carved geometry meets the intact block faces around it: the block
mesher rewrites damaged blocks to air in its cache, so nothing is drawn twice.

**`vr_subvoxel_bore.png` + `_zoom`, against `vr_subvoxel_bore_before_fix*.png`** -
straight down the bore. Before D3 was fixed the tunnel was a black ring with sky at
the end of it. After, the floor steps are bright, the walls mid-grey and the
ceiling darker, in the ratio `kVoxlFaceShade` gives the block path. Sky and
terrain visible through the far mouth with no leakage around the rim - if a face
were missing anywhere along the bore, that is where sky would appear.

**`vr_subvoxel_tunnel_inside.png`** - camera inside the bore, carved wall filling
the frame beside the intact outer faces of the slab. This is the strictest version
of "textured consistently with the intact blocks beside it": the carved wall
carries the same mottled stone at the same texel size as the intact face two metres
to its left, continuous across the sub-voxel steps, with no visible tiling seam and
no frequency mismatch.

---

## 4. What was changed

| File | Change |
| ---- | ------ |
| `src/mesh/GreedyMesher.cpp` | D1: `MesherScratch::levelDiffers[]`, written in `loadCache`, gates `emitSkirts` so a curtain is only hung where the neighbour is at another level or absent. D2: the skirt's `column` lambda now finds the topmost OPAQUE cell instead of the topmost non-air one |
| `src/mesh/SubVoxelMesher.cpp` | D3: `resolveNeighbours` rewritten in two passes. A partially destroyed neighbour no longer seals the block off - it feeds the open-side fallback - but is still not sampled directly, because its stored light is the solid block's light and sampling it drags carved pockets darker than the fallback they replaced |
| `assets/shaders/subvoxel.vert` | D4: texture coordinates derived from the vertex's own block-space position along the frame's tangent axes |
| `tests/test_lod_mesh.cpp` | The cross-level border case now asserts three curtains rather than four, plus a new section pinning "a coarse chunk fully surrounded at its own level hangs no curtain" |
| `src/app/Application.{hpp,cpp}`, `src/app/Main.cpp`, `tools/visual_review.ps1` | The review harness described in §1 |

### ACTION REQUIRED on a frozen header

`src/mesh/SubVoxelMesh.hpp` was NOT edited - it is a frozen contract. But its
"GLSL side (keep byte-for-byte in sync)" comment block now documents a UV
derivation the shader no longer uses:

```
//      vec2 size = vec2(((aData1 >> 12) & 7u) + 1u, ((aData1 >> 15) & 7u) + 1u);
//      vec2 uv   = size * (1.0 / 8.0) * corner_selector;   // one repeat per BLOCK
```

**No bit of the format moved.** `width`, `height` and `corner` are still packed and
still round-trip through `packSubVoxelVertex`/`unpackSubVoxelVertex`; the shader
simply no longer reads them, because they cannot express where in the block a quad
starts and that is exactly what the UV needs. The owner of that header should
replace the snippet above with the derivation now in `assets/shaders/subvoxel.vert`
and decide whether the three fields stay (they are the only record of the greedy
merge and cost nothing) or the freed bits get reused.

---

## 5. Open defects and recommendations

**D5 - stepped water plane and serrated ring at LOD band boundaries.** Water fills
up to y == 96 with its surface at y == 97, and the majority vote puts the coarse
surface at y == 98 at level 1 and y == 96 at levels 2 and 3, because a one-block
layer of water is 50% of a level-1 cell but only 25% and 12.5% of a level-2 and
level-3 one. The planes therefore step by 1-2 blocks at each ring, both sides draw
a translucent border wall to cover the step, and the doubled blend reads as a
darker serrated band.

Not fixed here because every available fix is a design decision above the level of
this review:

* Special-case a fluid surface in `reduceCell` so a cell containing any water at
  sea level reduces to water with its top face at the true sea level. Cheapest and
  most targeted, but it puts a material-specific rule into a function whose
  contract is currently material-agnostic.
* Skip the cross-level conservative border for self-culling translucent blocks
  (`loadCache`), which removes the doubled wall but risks a genuine hole where the
  planes really do differ.
* Draw distant water opaque. Removes the whole class of problem and is what most
  voxel renderers do, but it is a renderer decision, not a mesher one.

Recommend the first.

**D6 - thin ridges fragment at level 3.** `kLodSolidThreshold` is 0.4 and lives in
the frozen `world/Lod.hpp`. A ridge one or two blocks wide fills roughly an eighth
of an 8x8x8 cell, so cells along it pass or fail almost at random and the ridge
becomes a stack of detached cubes. The header's own reasoning - "a cell that erases
a thin solid feature leaves a hole" - is right, and 0.4 is simply not aggressive
enough at level 3.

Two options, neither taken here because both change a frozen constant or the
reduction rule: make the threshold level-dependent (roughly `1/cellSize`, so a
level-3 cell needs only ~64 of 512 blocks), or make the vote a max-over-columns for
the topmost solid cell so a surface can never be erased. Worth measuring the
triangle cost of either before choosing.

**Not a defect, but noted:** carved cavities underground are near-black, and the
crater floor in `vr_subvoxel_crater.png` is the visible face of that. It is the
absence of the lighting propagation pass, not a sub-voxel bug - the block path
behaves identically - and it will resolve itself when lighting lands. The
sub-voxel path already reads its light from exactly the position `GreedyMesher`
reads it from, so it will pick the pass up for free.
