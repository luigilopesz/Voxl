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

---
---

# Visual review: lighting, day/night, persistence, mining, audio, menus

Second pass, covering the milestone that added light propagation, the day/night
cycle, persistence, the sub-voxel mining verb, audio and the menus. Same machine
and settings as above: `RelWithDebInfo`, seed `8525033820662387641`, load radius
20 unless a shot says otherwise, RTX 3050 Laptop, 1600x900 client area. Every
image below was opened and looked at; every brightness claim is backed by a mean
pixel measurement over a stated box, not by an impression.

Shots from this pass are prefixed `ml_`. The first review's shots (`vr_`, `m1_`,
`m3_`) are untouched.

## 1. How these shots were taken

`tools/visual_review.ps1` unchanged, plus two things it cannot do.

**Persistence.** Every terrain shot passes `--no-save`. Without it the first run
writes a save and later runs load it, so `--seed` stops deciding what is on
screen and two shots meant to differ by one flag can differ by an hour of someone
else's edits. `--no-save` is the reproducibility flag for this whole document.

**Input.** The pause menu, the settings panel and placing a glowstone cannot be
reached from argv. Those shots were driven by a companion script that injects
input after the settle wait. Two things about it are worth recording, because
both cost an hour to find and both will bite the next person:

* **Mouse buttons must be posted, not injected.** With the cursor captured
  (`GLFW_CURSOR_DISABLED`) the game acts on injected *keys* but never on injected
  *mouse buttons* - a break or place silently does nothing while the hotbar digit
  works, which reads like a gameplay bug and is not one. Posting
  `WM_RBUTTONDOWN`/`WM_RBUTTONUP` straight to the main window with `PostMessage`
  works. In menu mode (cursor normal) ordinary `SendInput` clicks work fine.
* **Menu clicks are in client space and the window frame is not.** `capture.ps1`
  grabs the client rectangle, so coordinates read off a screenshot are client
  coordinates; a click has to be offset by `ClientToScreen` of the window origin
  (9, 38 here) or it lands one title bar too high.

Repositioning the cursor with `SetCursorPos` is safe for the framing: the window
enables `GLFW_RAW_MOUSE_MOTION` whenever the cursor is captured, and `SetCursorPos`
generates no raw motion, so the camera does not turn. Verified by comparing the
framing either side of a cursor-centring step.

*Integration note:* if this is worth keeping, `tools/visual_review.ps1` could take
a `-Script` parameter with a small step vocabulary (`key:NAME`, right-click,
left-hold, absolute click, centre cursor, wait). I did not add it, because I do
not own that file.

## 2. Defect table

| # | Defect | Severity | Evidence | Status |
| - | ------ | -------- | -------- | ------ |
| L1 | No tone mapping anywhere in the surface shader. `sun + ambient` already reaches ~1.4 at noon, so any albedo above ~0.71 leaves `voxlShadeSurface` above 1.0 and clips at the encode. A snow patch a few blocks from the camera at midday renders as flat **255,255,255 with no texture at all** - a white hole in the ground, at the default spawn | **Medium** | `docs/images/ml_white_nohud.png`, `ml_fix_before_zoom.png` | **Fix written and verified, not applied** - see section 4 |
| L2 | LOD water skirts read as a row of dark trapezoids lying on the sea whenever it is viewed from above at a shallow angle. Present at every time of day. This is the first review's D5, and "three faint serrated lines" understates it: from 10 blocks up it is a band of hard-edged wedges across the whole visible ocean | **Medium** | `ml_day_ground_zoom_water.png`, `ml_day_stand.png`, `ml_tod_dawn.png` | **Open**, mesher owner |
| L3 | 1-pixel dark hairlines drawn over the sky, in a chunk-spaced grid, whenever the camera is under or inside terrain. Unlit geometry seen edge-on. **Not** an LOD artefact: identical with `--lod-off` | Low-Medium | `ml_cave_mouth.png`, `ml_cave_mouth_lines.png` vs `ml_cave_mouth_lodoff_zoom.png` | **Open**, mesher owner |
| U1 | The gameplay crosshair keeps drawing over the pause menu and over the settings panel. It lands on the top edge of the "Settings" button | Low | `ml_ui_pause_zoom.png`, `ml_ui_settings.png` | **Open**, UI owner |
| U2 | Settings sliders centre the value text under the grab handle, so the handle covers it. "Field of view" reads as `70` with the handle sitting on the degree sign; render distance, fog and anisotropy are all partly covered | Low | `ml_ui_settings_zoom.png` | **Open**, UI owner |
| U3 | The pause menu's "Resume" and "Settings" labels ghost faintly through the settings panel drawn on top of them | Very low | `ml_ui_settings_zoom2.png` | **Open**, UI owner |
| G1 | Right-click placement against a solid face 4 blocks away works; the same injected click in a large cavern, with the crosshair reporting a valid target, never placed anything in five attempts across three aim angles. Left-click break behaved the same way there | Low, **unconfirmed** | `ml_glow_slab_after.png` works; cave attempts produced no change | **Open, needs an owner to reproduce by hand** - I cannot separate this from my input harness |

Nothing in the required list came back black, and no lighting discontinuity was
found at any chunk or LOD boundary in any shot.

## 3. Per-shot verdicts

### Open daylit ground - PASS

**`ml_day_ground.png`** (`--pos 0,170,0 --look 180,-10 --time noon`) and
**`ml_day_stand.png`**. Uniformly bright, no patchiness, no chunk-shaped variation
in the grass. Grass tops measure luma 141-153 across the whole frame from the
foreground to the fog line; the variation is fog, not light. `sun/blk(sky)=15/0`
in the log at the eye, `0/0` at bedrock, which is the correct answer for both.

**`ml_day_aerial.png`** (`--pos 0,235,0 --look 40,-28`). A whole landscape of
forest, snow peaks and plains at every LOD level in one frame. Lighting is
continuous across all of it.

### Cave interior - PASS

**`ml_cavefog_noon.png`** (`--pos 0,31,0`, 130 blocks down). Dark, and dark in a
readable way rather than black: near walls sit around luma 25-50 against lit
surfaces at 100+. There is no chunk-border stepping anywhere - the falloff from a
lit opening into the dark follows the geometry.

A trap worth recording, because it cost me two shots: the pale patches in this
frame are **not** fogged cave walls and **not** a light leak. They are the sky,
seen through gaps. `ml_cavefog_midnight.png` is the same framing at midnight and
the patches are dark navy **with stars in them** - `ml_cavefog_mid_zoom.png` at 4x
shows individual stars inside a voxel-stepped silhouette. Anyone reviewing an
underground shot should do this check before filing a light-leak bug.

**`ml_cavefog_nolight.png`** is the same framing with `--no-light`, and is the
control that proves the light pass is actually doing the work: with it, the intact
slab face in `ml_sub_bore.png` measures luma 108.6; without, 41.8.

**`ml_cave_dark.png` / `ml_cave_night_check.png`** - the same location at radius
20 by day and by night, same conclusion.

### Glowstone in the dark - PASS

**`ml_glow_slab_before.png`** then **`ml_glow_slab_after.png`**. The debug tunnel
slab at midnight, camera 4 blocks off its face, one glowstone placed on it. The
wall goes from luma 44 to 123-131 and the pool has a smooth radial falloff with no
banding and no chunk-border step: 131 beside the block, 127 one block up, 123 at
the far edge of the face, 116 in the far bottom corner. The glowstone's own face
keeps its texture at night - the bright speckles are clearly individual.

**Caveat, stated plainly:** this is a night surface scene against a stone slab, not
a natural cave, because I could not get a scripted placement to land in a cave -
see G1. The falloff behaviour is the same code path either way, but if you want
the literal shot, place one by hand.

### Time of day - PASS, and the fog matches the sky at all four

Identical framing, `--pos 0,170,0 --look 180,-10`, four times.

* **`ml_tod_dawn.png`** - purple zenith into an orange horizon band, stars still
  faintly up high, terrain warm-tinted, water carrying the sky colour.
* **`ml_tod_noon.png`** - blue, neutral, bright.
* **`ml_tod_dusk.png`** - the strongest of the four; a red horizon band under a
  mauve sky with the stars already out.
* **`ml_tod_night.png`** - dim but readable, which was the requirement. Grass is
  still legibly green, the sand strip is still distinguishable from the water, and
  the star field is dense without being noisy.

**No hard horizon seam at any of the four.** This is the defect the brief flagged
as most likely and it is genuinely absent, because each shader fogs toward
`voxlSkyColour()` evaluated along its own view ray instead of toward one flat
colour - so the fogged distance *is* the sky, by construction rather than by
tuning. `ml_tod_night_horizon.png` and `ml_tod_dusk_horizon.png` are 2x crops of
the horizon band: continuous gradient, no step, no colour mismatch between the
fogged terrain and the sky above it.

### LOD bands - PASS

**`ml_lod_stress.png`** (`--lod-bands 2,3,4`, which puts almost the whole frame at
coarse levels) against **`ml_lod_stress_off.png`**. No lighting discontinuity at
any band boundary - the coarse cells are lit exactly like the fine ones and the
band rings are invisible in the shading. The only difference across a boundary is
resolution, which is the point of the feature.

### Water at a shoreline - PASS for the shore itself

**`ml_shoreline_zoom.png`** (3x). Water meets sand on a crisp voxel edge with no
z-fighting, no gap, no double-blended fringe, and the sand and grass either side
are lit identically. The shallow-water darkening reads correctly as depth.

The sea *away* from the shore is where L2 lives.

### Carved sub-voxel tunnel - PASS

**`ml_sub_bore.png`** plus **`ml_sub_bore_zoom.png`** (4x), straight down the bore,
against **`ml_sub_bore_nolight.png`** with `--no-light`.

The carved surface is lit consistently with the intact blocks around it, and it
grades: intact slab face 108.6, bore wall at the mouth 67.8, the same wall one
block deeper 50.3, ceiling 51.6, floor 90.3. With `--no-light` those become
41.8 / 29.8 / 26.0 / 23.4 / 32.5 - every carved surface moves with the light pass
in the same proportion as the intact face beside it, which is the actual claim
being tested. My first impression from the crop was "the walls are black"; the
measurement says they are a correct tunnel falloff and the impression was wrong.

**`ml_sub_slab.png`** - three-quarter view. The bore mouth is a clean circle in an
unbroken intact face, no z-fighting at the rim, no double-drawn geometry.

### Menus - PASS with three cosmetic defects

**`ml_menu_title.png`** - title screen. Readable, well-spaced, version in the
corner.

**`ml_ui_pause.png`** - pause menu over a live dimmed world. Buttons legible,
world still streaming behind. U1 is here.

**`ml_ui_settings.png`** - Video tab, with Controls / Audio / World alongside.
Every control is legible and the help text under the frame-limit checkbox is a
genuinely useful sentence. U2 and U3 are here.

## 4. L1: the highlight clipping, and the fix

`assets/shaders/chunk_common.glsl`, end of `voxlShadeSurface`:

```glsl
return albedo * kVoxlFaceShade[direction] * aoTerm * (sun + ambient + block);
```

Nothing clamps or tone-maps this, and the terms are not small. With the shipped
`SkySettings`, at noon on a top face: `sun` is about (1.08, 1.04, 0.95),
`ambient` (0.30, 0.37, 0.50) - so the multiplier is already ~1.4 before any block
light. Snow's albedo is ~0.95, so a sunlit snow top leaves this function at ~1.35
and the sRGB encode clamps it to white. Add an emissive block, whose
`blockLightColour * blockLightGain` contributes another (1.25, 0.90, 0.50), and
the sum is over 2.4.

Measured: the snow patch at the default spawn, from `--pos 0,161,0 --look 180,-45`,
is **exactly 255,255,255 over a 100x80 box** - not "nearly white", saturated, with
the texture entirely gone (`ml_white_nohud.png`). From 15 blocks up it is
(244, 249, 254) (`ml_white_down.png`). At midnight the same region is (78, 91, 134)
(`ml_white_night.png`), so this is a midday-only failure.

Ordinary terrain has headroom - grass tops 141-153, sand 219, stone 89-113 - so
this only bites high-albedo blocks at close range, and emissive blocks in daylight.

### The fix, and the evidence it works

A soft shoulder that is **identity below the knee**, so nothing already in range
changes at all:

```glsl
    vec3 lit = albedo * kVoxlFaceShade[direction] * aoTerm * (sun + ambient + block);

    // Soft shoulder. sun + ambient already reaches ~1.4 at noon and an emissive
    // block adds ~1.25 on top, so a high-albedo surface - snow, or any glowstone
    // in daylight - left this function above 1.0 and clipped to flat 255,255,255
    // at the encode, losing its texture. Identity below the knee, so everything
    // that was already in range is unchanged; only the highlights that had
    // nowhere to go are compressed.
    const float knee = 0.8;
    vec3 over = max(lit - vec3(knee), vec3(0.0));
    return min(lit, vec3(knee)) + (1.0 - knee) * (over / (over + (1.0 - knee)));
```

I did **not** commit this. `assets/shaders/chunk_common.glsl` is shared by the
chunk, water and sub-voxel programs and is not mine to change. To prove it works
without touching the source I patched the **build output copy**
(`build/RelWithDebInfo/bin/assets/shaders/chunk_common.glsl`), re-captured, then
restored it - the tree is clean and the build copy is byte-identical to source
again.

| Sample | Before | After |
| ------ | ------ | ----- |
| Spawn snow, point blank, noon (`ml_fix_before_zoom.png` / `ml_fix_after_zoom.png`) | 255.0, 255.0, 255.0 | 245.6, 247.1, 249.0 - texture visible again |
| Snow from 15 blocks up | 244.4, 249.4, 254.4 | 238.8, 242.4, 246.2 |
| Grass in sun | 103.9, 159.2, 71.0 | 103.9, 159.2, 71.0 - **identical** |
| Water | 69.7, 122.4, 184.7 | 69.7, 122.4, 184.7 - **identical** |
| Grass at midnight | 27.9, 50.4, 30.8 | 27.9, 50.4, 30.8 - **identical** |
| Sand | 235.3, 219.0, 165.9 | 231.4, 219.0, 165.9 - red only, just over the knee |

`ml_fix_white_nohud.png` is the re-capture. A knee of 0.8 is deliberately
conservative - it buys back the texture but the snow is still very bright. 0.6 or
0.7 would restore more relief at the cost of visibly darkening every bright
surface, which is a look decision for whoever owns the renderer, not mine.

## 5. What I could not settle

**G1, placement in a cavern.** Against the debug slab, hotbar 9 then a posted
right-click placed a glowstone first time, every time. At `--pos 0,31,0` in a large
cavern the same script placed nothing in five attempts at three aim angles, with
the crosshair rendering in its orange "valid target" state and the selection
wireframe drawn around a block each time; a held left-click there broke nothing
either. Both worked at the slab. I cannot tell from outside whether the edits were
rejected, endlessly deferred by `isEditBlocked`, or whether my injected clicks were
being dropped in that particular session, and the engine logs no `PlaceResult`. It
needs ten seconds of hand-testing by whoever owns `BlockInteraction`: fly into a
cavern, aim at a wall five blocks off, right-click.

**L2 and L3** are mesher-side and I have only characterised them.

**A natural cave mouth with a photographed light gradient.** `ml_cave_mouth.png`
(`--pos 0,120,0`, under the spawn hill) is the closest I got: dark unlit ceiling and
floor framing a bright landscape. The dark-to-light transition there is a geometry
edge rather than a propagated gradient, so it does not actually test the thing the
brief asks about. The gradient claim is instead supported by the bore measurements
in section 3 and by the glowstone falloff, both of which are numbers rather than
impressions.

**Minor, not filed as a defect:** `--no-hud` does not hide the block selection
wireframe, which puts a black box in the middle of otherwise clean shots - visible
around the bore in `ml_sub_bore_zoom.png`. Harmless in play, mildly annoying for
review shots.

