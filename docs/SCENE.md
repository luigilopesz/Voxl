# The test scene

**Recorded:** 2026-07-31
**Source:** `src/voxels/brushes.glsl` — `brushgen_voxl_scene()` and everything under the
`VOXL TEST SCENE` banner. Selected by `#define VOXL_TEST_SCENE 1`; set it to `0` and the
inherited gvox_engine demo terrain comes back, unmodified, in the same file.

A small hand-authored level at the engine's native 16 voxels/metre: rolling ground, one
conifer, a cave with a lit chamber, and ground detail. It exists to answer three questions
with numbers instead of opinion — whether a Voxl-authored world fits the memory budget,
whether the path-traced GI carries an emissive voxel through a dark interior, and what
16 voxels/metre costs when the content is authored *at* that scale rather than scaled down
from metre blocks.

Short answers: **830 MB of heap against the demo's 2906 MB, 11.06 ms against 17.97 ms, and
yes — the amber crystal light bleeds visibly onto grey stone 4 m away.**

---

## 1. Coordinates

Everything below is in **scene-local metres**, `s = voxel_pos - VOXL_ORIGIN`, with

```
VOXL_ORIGIN = vec3(-183.0, -110.0, -52.50)      // absolute world metres
```

so **local = absolute + (183, 110, 52.5)**. Local `+X` and `+Y` are the two horizontal world
axes and `+Z` is up. The debug overlay (F3) shows `Player Unit Offset` (integer metres) and
`Player Pos` (the fraction); **absolute position is their sum**, so

```
local = Player Unit Offset + Player Pos + (183, 110, 52.5)
```

and the camera is a further 0.2 m *below* `Player Pos` (`src/application/player.cpp:305`).

The scene is authored around the spawn rather than the other way round, because the spawn is
hardcoded in `player.cpp` and this file does not own that file. The startup camera looks
along the **+X+Y diagonal at 45 degrees in plan, 27 degrees below the horizon**
(`player.cpp:56-57`), so every feature is laid out along that diagonal in order of distance.
**The whole scene is one walk: hold W from the spawn and you pass the tree, reach the cave
mouth, and end up in the lit chamber.**

| Feature | Local (x, y, z) | Distance from spawn | Note |
|---|---|---|---|
| Spawn, player feet | (0.01, 0.02, 3.78) | 0 | on a level pad at z = 3.60 |
| Spawn, camera | (0.01, 0.02, 5.33) | 0 | 1.73 m above the pad |
| Tree base | (15.2, 8.2, ~0) | 17.3 m | 5.0 m right of the walk line |
| Cave portal | (11.75, 11.75, 1.5–3.65) | 16.6 m | 2.6 m wide, 2.15 m tall |
| Tunnel axis | (10.6, 10.6, 2.35) → (17.2, 17.4, 2.05) | — | radius 1.30 m |
| Chamber centre | (18.4, 18.7, 2.25) | 26.3 m | 6.2 × 5.9 × 4.7 m ellipsoid |
| Chamber floor | z = 1.34 | — | 5.7 × 5.4 m of flat floor |
| Emissive crystals | (19.50, 19.90, 1.26) | 27.9 m | five shards, 1.1 m tall |
| Hill centre | (17, 17) | 24.0 m | radius 10 m, height 8.5 m |
| Island centre | (10, 10) | 14.1 m | see §2 for the radius |

---

## 2. How big the world is, and why it is computed rather than typed

The engine's world is a cube of `CHUNKS_PER_AXIS`³ four-metre chunks that wraps modulo
`CHUNKS_PER_AXIS` around the player. **Nothing outside ±`CHUNKS_PER_AXIS`×2 metres exists at
any instant** — terrain authored beyond that is not "far away", it is absent, and the island
would show a cut face at the volume boundary. So the island radius is derived from the
constant instead of hardcoded:

```glsl
#define VOXL_WORLD_HALF (float(CHUNKS_PER_AXIS) * CHUNK_WORLDSPACE_SIZE * 0.5)
#define VOXL_ISLAND_R   min(40.0, VOXL_WORLD_HALF - 11.0)
```

| `CHUNKS_PER_AXIS` | world | `VOXL_ISLAND_R` | solid terrain ends at | island across |
|---|---|---|---|---|
| 16 *(current)* | 64 m cube | 21 m | ~18.5 m | **~37 m** |
| 32 *(upstream)* | 128 m cube | 40 m | ~35 m | ~71 m |

**This is smaller than the 64–128 m the brief asked for, and the reason is a change that
landed under this work rather than a choice made in it.** `CHUNKS_PER_AXIS` was 32 when this
scene was laid out and is 16 now (`src/voxels/impl/voxel_malloc.inl:20`, changed in parallel
by the allocator work, with its own well-argued rationale). At 16 the whole scene has to fit
inside ±32 m per axis or its far side falls outside the resident volume.

The response was to size the *content* for the smaller world and let only the surrounding
ground scale: spawn, tree, hill and cave span 34 m along the diagonal and sit inside a 21 m
radius disc, so the same file gives a correct 37 m island now and a correct 71 m island if
`CHUNKS_PER_AXIS` ever goes back to 32. A scene that fits a 64 m world also fits a 128 m one;
the reverse is not true. **If a bigger island is wanted, raise `CHUNKS_PER_AXIS` first — this
file will follow it without an edit.**

The island is a disc with a domed underside, not a box: the surface rolls off over the last
28 % of the radius and meets the underside before reaching it, so the landform simply ends
and there is no vertical cliff giving the volume away.

---

## 3. Where to stand to photograph each feature

Every position below is reachable from the spawn with the keyboard alone, which matters
because the harness can only send keys and mouse deltas — there is no teleport. **`F` toggles
fly** (the game starts *in* fly mode at 15 m/s; on foot it is 1.5 m/s and gravity and
collision apply). Mouse deltas are radians × 1000 at the default sensitivity of 1.0, so
`dy = -400` pitches up 0.4 rad ≈ 23°.

Startup pitch is 1.096 rad; **level is 1.571**, so `mouse dy = -475` looks at the horizon.
The overlay's `Player Rot (Y/P/R)` reads it back.

### 3.1 The tree from outside — and the whole scene in one frame

**Do nothing.** The startup camera at (0.01, 0.02, 5.33) already frames the tree, the cave
hill and the portal together; the spawn pad height was chosen for exactly this (drop it and
the tree crown and hill top leave the top of the 74° vertical FOV). Wait ~15 s for the GI to
converge before capturing.

For a **tree portrait**, from the spawn: `mouse 0:-400` (level the view), then walk or fly
about 10 m forward and turn right ~17° (`mouse 300:0`). The tree is 6.6 m tall with a bare
trunk to 1.85 m and nine whorls above it.

### 3.2 The cave mouth from outside

`tap F` (leave fly), `wait 3` (fall to the pad), `mouse 0:-400`, then **hold W for 11 s**
(~16 m at 1.5 m/s). That puts you at roughly local (11, 11) with the portal filling the
middle of the frame and the crystal glow visible through it. This is the shot for judging the
**light gradient at the entrance** from outside.

Reference capture: `scratchpad/y02-meadow.png`.

### 3.3 The cave interior with the light on

Continue from 3.2: **hold W for a further 7 s** (~10 m) to stand inside the tunnel looking at
the chamber, then a further 3 s (~4.5 m) to stand in the chamber itself. **Wait at least
12 s** before capturing — the irradiance cache fires one bounce per probe per frame and a
dark interior lit only by bounce is the slowest thing in this scene to converge.

Three captures worth taking, all of which exist as references:

| Shot | Where | What it shows |
|---|---|---|
| `y03-portal.png` | in the tunnel, looking in | walls fade cool grey → warm amber along the tunnel |
| `z02-chamber-perf.png` | in the chamber, 2 m from the crystals | the emitter itself, walls washed amber |
| `z03-look-back.png` | in the chamber, turned 180° (`mouse 1600:0` twice) | **the single best GI frame**: amber bounce in the near half of the tunnel, blue sky bounce in the far half, meadow through the portal |

`z03` is the one to keep. The amber-to-blue gradient along a single stone tunnel, with no
light in the scene except the sun and 1.5 m² of emissive voxels, is the whole demonstration.

---

## 4. What is in the scene, and how it is built

### 4.1 Ground

A height field, four value-noise lookups deep: ±0.58 m of rolling at an 11 m wavelength plus
±0.19 m at 4.1 m, the cave hill, the island rim, and a level pad blended in at the spawn.
Solid where `s.z` is between that height and a domed underside at −6 to −11 m.

Materials by depth, and the depth bands are load-bearing rather than decorative — see §5:

| Band | Material |
|---|---|
| the one air voxel above the surface | grass skin: one of five greens, plus a grass or flower particle |
| bare rock on the hill above ~3.6 m | three greys, boundary wobbled by a 2.9 m noise field |
| 0 – 0.34 m | topsoil, two browns |
| 0.34 – 1.10 m | subsoil, two paler browns |
| below 1.10 m | **one flat grey, no variation of any kind** |

### 4.2 The conifer

6.6 m tall = **106 voxels**, so there is room for real structure: a tapered trunk with
vertical bark streaks one voxel wide and seven tall, bare to 1.85 m, then nine whorls of 5–7
branches that shorten and lift toward the crown, spaced by the golden angle so no two whorls
line up.

**The foliage is not a solid SDF.** `voxl_conifer()` returns a distance *normalised by each
whorl's envelope radius*, and the caller turns that into a fill probability
(`0.22 + 0.62·env²`, never reaching 1) evaluated on ~1/7 m cells — about 2.3 voxels. So the
needles are 2–3 voxel clumps with gaps all the way through the canopy, and the sun dapples the
ground under it. Colour is one of five greens, chosen per clump with the choice *biased* by
height so the crown carries the lighter new growth without costing a single extra palette
entry.

This is the part that does not survive being ported from a metre-block generator: at 1 m
there is nothing to be stochastic about, and a solid leaf SDF at 6.25 cm reads as a green
blob.

### 4.3 The cave

A capsule tunnel smooth-unioned with an ellipsoid chamber, walls roughened by ±0.25 m
(±4 voxels) of pseudo-3D noise, then intersected with a gently sloping floor plane so the
whole thing is walkable — 2.15 m of headroom in the tunnel, 3.26 m in the chamber.

**Why the chamber is genuinely dark, with the arithmetic, because "it looks enclosed" is not
evidence.** The sun defaults to Angle X 210, Angle Y 25 (`renderer/atmosphere/sky.inl:67-68`),
giving a direction-to-sun of (−0.366, −0.211, +0.906): 65° above the horizon and, in plan,
within 15° of the tunnel axis. Sunlight therefore shines straight into the mouth — which is
what makes the entrance gradient worth photographing — but it drops **2.14 m for every 1 m
forward**. A 2.15 m portal puts the last direct sun on the floor about **1.0 m inside**. The
chamber starts 7 m further in, behind a 2.6 m aperture, under 2.9–4.6 m of rock. Nothing but
bounce reaches it.

### 4.4 The light

There are **no analytic or punctual lights anywhere in this engine** — the triangle-light
sampling block is commented out and every light except the sun is an emissive voxel resolved
through the irradiance cache
(`renderer/kajiya/ircache/ircache_trace_common.inc.glsl:129`). The voxel is 2 bits of
material type + 4 of roughness + 8 of normal + 18 of colour (`pack_unpack.glsl:47`), with no
room for an intensity field, so **emission strength is smuggled through the roughness bits**:

```glsl
gbuffer.glsl:20   emissive = color * float(material_type == 3) * (2.0 * roughness + 0.01)
gbuffer.glsl:24   albedo   = color * float(material_type == 1 || material_type == 2)
```

Two consequences worth knowing before retuning it: roughness 1.0 is the ceiling, giving
2.01× the packed colour, and a type-3 voxel has **zero albedo** — it is a pure emitter that
does not itself receive bounce. Roughness also quantises to `(k/15)²`, so only 1.0 and
0.5378 (the two values used) survive the round trip exactly.

The cluster is five leaning shards, 1.1 m tall, at roughness 1.0 and colour (1.00, 0.52,
0.16), plus a dimmer floor crust at roughness 0.5378 spreading 1.55 m around them. **Area
matters as much as radiance**: the irradiance cache fires one bounce per probe per frame, so
a pinpoint emitter converges as visible sparkle where ~1.5 m² converges clean.

Amber on pale grey limestone was chosen over the reference's white deliberately — colour
bleed you can name is better evidence that the GI is working than a brightness change.

### 4.5 Ground detail

- **Flowers** use the engine's own particle system, whose four hardcoded types are exactly
  the four colours in the reference screenshots: tulip red, lavender purple, dandelion
  yellow, dandelion white. Species is chosen by a 13 m noise field with a per-voxel jitter,
  so they drift in patches instead of scattering as confetti. **~0.6 per m².**
- **Grass** is ~50 blades/m² of the same particle system — about 44k strands on the 37 m
  island, against a `MAX_GRASS_BLADES` of 1<<20 that was itself resized to match the smaller
  world in parallel work. If the island grows, check that number before the flowers.
- **Mushrooms**, 0.38 m tall with a 0.31 m cap (6 voxels by 5), on a 2.6 m lattice, thicker
  in the damp shade under the tree and around the cave mouth, never on the bare rock.
- **Pebbles**, 0.34–0.86 m, on a 4.4 m lattice, bedded 40 % into the ground, with the last
  1.5 voxels of the silhouette dithered away so they are chipped rather than analytic.

Mushrooms and pebbles are real voxels: they bounce light, appear in reflections and cast ray
shadows. **Grass and flowers do not** — they are rasterised particles, invisible to
`voxel_trace()`, casting shadows only through the 40 m ortho shadow map. That is inherited,
not a choice made here, and it is worth remembering when reading a screenshot of this scene.

---

## 5. Determinism

**The same seed gives the same scene, and there is no seed** — every random number is a hash
of an absolute world coordinate, so the scene is a pure function of position.

This is not stylistic. `voxel_world.comp.glsl:193` seeds the per-invocation PRNG with
`voxel_i`, the **chunk-buffer** index, not the world position, and chunk indices wrap modulo
`CHUNKS_PER_AXIS` around the player. Anything placed with `rand()` therefore *moves* when you
walk away and come back. Everything here goes through `voxl_hash3()`, `good_rand()` or
`fbm2()` of `voxel_pos`. `fbm2()` in particular hashes integer lattice points rather than
sampling `g_value_noise_tex`, so it needs no sampler state inside the brush and is
bit-reproducible.

**Two things are still not bit-identical between runs, and neither is the voxels.** Grass and
flower *particles* take their per-blade height and sway from `rand_seed(particle_index)` in
the simulation, and the index depends on allocation order. And a chunk regenerated by world
wrapping spawns a *second* strand on a voxel that already has one — the simulation only frees
a strand when its spawner voxel changes (`particles/grass/sim.comp.glsl:44-51`), and a
deterministic generator regenerates it identically. Inherited behaviour, bounded by the fixed
pool rather than by VRAM, and the reason the densities here sit two orders of magnitude under
`MAX_GRASS_BLADES`.

---

## 6. Measurements

All at 1280×720 on the machine in `docs/BASELINE.md` §6 (RTX 3050 6 GB laptop), with the
debug overlay on and both frame-time tree nodes expanded.

### 6.1 Frame time and heap

| | Full frame-time | CPU-only | GPU Heap | Heap usage |
|---|---|---|---|---|
| **Stationary at spawn** | **10.93 ms (91.47 fps)** 9.63–12.14 | 0.86 ms (1168 fps) | **393216 pages (830 MB)** | **13 MB (2%)** |
| Stationary at spawn, repeat | 11.06 ms (90.45 fps) 10.19–11.69 | 0.85 ms (1172 fps) | 393216 pages | 13 MB |
| Inside the chamber | 9.01 ms (110.95 fps) 8.24–9.88 | 0.82 ms (1217 fps) | 393216 pages | 13 MB |
| In the tunnel, looking out | 9.79 ms (102.17 fps) 8.83–10.83 | 0.87 ms (1144 fps) | 393216 pages | 13 MB |
| *Baseline, demo world, settled* | *17.97 ms (55.65 fps)* | *0.97 ms* | *1376256 pages (2906 MB)* | *636 MB* |
| *Baseline, demo world, moving* | *21.11 ms (47.37 fps)* | *1.02 ms* | *1376256 pages* | *1629 MB* |

The two spawn rows are separate runs; treat 0.13 ms as the run-to-run noise floor. Command
that produced the first:

```powershell
powershell -File C:\voxl2\tools\run.ps1 -Overlay -ExpandGraphs -Seconds 40 -ConvergeSec 12 `
    -Screenshot <path>.png -Quit
```

The chamber and tunnel rows were captured by a scratch driver that walks the player in
(`F`, then `W` in timed holds) and clicks the same two tree nodes open before unpausing;
`tools/run.ps1` cannot reach them because its `-Soak` flies at 15 m/s and leaves the island
in two seconds.

### 6.2 What that comparison is and is not

**It is not a clean A/B.** Between `docs/BASELINE.md` and these numbers, `CHUNKS_PER_AXIS`
went 32 → 16 in parallel work, which by itself halves the world, cuts the always-resident
chunk table from 269 MB to 33.7 MB, and reduces how much geometry is in front of the camera.
Some unknown share of the frame-time win is that change, not this scene.

**The heap-usage figure is attributable, though, and it is the interesting one.** 13 MB in
use against the demo's 636–1629 MB is a 50–125× difference, and it does not come from world
size — it comes from one decision, described in §5 of `brushes.glsl` as rule (2):

> The palette compressor allocates **nothing at all** for an 8³ region whose voxels are
> bit-identical — the single value is stored inline in the header's `blob_ptr` and no heap
> page is taken (`voxel_world.comp.glsl:908-915`).

So colour grain, roughness variation and written normals are confined to a thin skin near a
surface, and below 1.1 m the rock is one flat colour with the normal left at its default, which
the post-process then nullifies because the voxel is occluded. Buried volume is free. The
inherited generator does the opposite — it writes an analytic normal to *every* solid voxel —
and that is a large part of why its heap settles at 2906 MB.

### 6.3 VRAM

Sampled once per second by `tools/run.ps1` via `nvidia-smi`, whole-device including the
desktop:

| | |
|---|---|
| Before launch | 263–264 MiB |
| Steady, 40 s stationary at spawn | **2119 MiB of 6144 (34.5%)**, flat to the MiB across all 8 samples |
| Transient peak, cold SPIR-V cache | **3896 MiB (63%)** |
| After exit | 267 MiB, **exit code 0** |

The two figures are both real and the gap matters. On a warm cache the run sits flat at
2119 MiB from the first sample. On the first launch after editing `brushes.glsl` — which
invalidates the shader cache and forces the whole world to be generated while the app is also
recompiling — it climbed to 3896 MiB between t=5 s and t=15 s and then **fell back to
~2100 MiB and stayed there**. That fall is worth noticing: it is not the voxel heap, which has
no shrink path (`src/utilities/allocator.inl`), so it is transient generation working memory
being released. **Quote 2119 MiB as the steady figure and 3896 MiB as the worst first-launch
transient.**

Baseline for comparison: 4590–4593 MiB peak over a 30 s moving soak, 3384–3417 MiB
stationary. No leak; the driver returns to idle on exit in every run.

---

## 7. Known defects

Stated plainly, because a scene document that only lists what works is not much use.

1. **Black holes in the hill.** THE headline defect: three to six openings of 0.5–2 m in the
   rock dome, plus a scatter of 1–3 voxel specks, present in every exterior shot.

   **Correction to an earlier entry here, which said "not holes".** They are holes. Fly to
   `--pos` local `8,8,13` looking at the dome and the rim is unmistakable: lit rock all round,
   a voxel-stepped edge, and unlit interior behind it —
   `docs/images/22-defect-hole-closeup.png`. The earlier reading came from a distant aerial
   frame in which they happened not to be visible.

   **Attributed, by bisection, to the voxel-heap path — not to this file's geometry.** Two
   switches at the head of `brushes.glsl` were added to do it, and each is a one-character edit:

   | Build | Holes? | What it proves |
   |---|---|---|
   | shipped | yes | — |
   | `VOXL_DEBUG_NO_CAVE 1` | **yes** | not the cave SDF |
   | `VOXL_DEBUG_NO_DERIVED_NORMALS 1` (`voxel_world.comp.glsl`) | **yes** | not the derived-normal bug |
   | 45 s convergence instead of 5 s | **yes**, unchanged | not the denoiser or the irradiance cache |
   | `VOXL_DEBUG_BARE_HEIGHTFIELD 1` (terrain solid test only, one colour) | **no** | not the height field, which cannot make a hole anyway |
   | `VOXL_DEBUG_FLAT_ROCK 1` (all geometry, props, grass and tree intact; every solid voxel one packed value) | **no** | ← the discriminator |

   The last two rows are the result. Geometry, props, grass and the tree are all present in the
   `FLAT_ROCK` build and the dome is solid; the *only* thing that changed is that every 8³
   palette region became bit-uniform. A uniform region takes the `palette_size <= 1` branch at
   `voxel_world.comp.glsl:920-928`, stores its single value inline in the header and **allocates
   nothing**. Every other region calls `VoxelMalloc_malloc`/`VoxelMalloc_realloc`. So: *a region
   only ever becomes a hole if it allocates from the voxel heap.*

   This is not heap exhaustion — usage sits at 12.8 MB against 830 MB of capacity and a 4129 MB
   cap, with zero growth refusals. Two further facts for whoever picks this up:
   `MAX_CHUNK_UPDATES_PER_FRAME 4` instead of 128 makes it **worse**, not better, which argues
   against a race between concurrently generating chunks; and `utilities/allocator.glsl`'s
   `malloc()` was returning an out-of-range index whenever the element count overflowed (fixed
   2026-07-31, see the comment there), which is the right neighbourhood but demonstrably not the
   whole story, because the holes survive that fix.

   Worth knowing: the inherited demo world shows the same class of speck
   (`docs/images/20-ab-demo-world-same-build.png`). Its terrain is thin, noisy and
   multi-coloured, so it hides them; a smooth 10 m hillside of three greys does not.

   Separately and now fixed: the 1-voxel *seams* that used to run along every chunk boundary
   were engine-derived normals gone wrong. `generate_normal_from_geometry()`
   (`voxel_world.comp.glsl:304-353`) reads a 5³ neighbourhood through `get_temp_voxel()`, which
   crosses chunk boundaries, and only `MAX_CHUNK_UPDATES_PER_FRAME` chunks generate per frame
   out of `CHUNKS_PER_AXIS`³. A chunk generated before its neighbour reads that neighbour as
   air, derives a normal tilted into it, and is never revisited. Writing analytic normals for
   the surface skin (rule 3) removed the seams entirely. That work stands; it is simply not the
   cause of the holes.

2. **The scene is 37 m across rather than the 64–128 m asked for.** §2 explains why and how to
   get it back.

3. **The hill reads as a smooth dome.** Its radius is warped by ±0.6 m of noise, which is as
   far as it can go: the mound is steep enough that warping the radius by 0.06 moves the
   surface 1.3 m vertically, and the chamber ceiling only has 2.9 m of rock over it. Larger
   warps were tried and breached the roof. Breaking the silhouette properly needs either a
   bigger hill or a chamber placed deeper.

4. ~~Nothing has been measured while moving through the scene on foot.~~ **Fixed 2026-07-31.**
   The engine now takes `--patrol RADIUS,PERIOD` and walks a closed circle at a chosen speed;
   a 300 s / 38 496-frame soak is in §8 below.

5. **Auto-exposure makes the chamber read as warm fog** when you stand next to the crystals —
   correct behaviour for a camera adapting to a dark room, but it flatters the light. Compare
   `docs/images/14-cave-interior-lit.png` (crystals blown to white, walls a flat amber wash)
   with `docs/images/16-cave-lookback-gi.png`, where the entrance is in shot and the exposure
   has something bright to anchor to. Quote the second one.

---

## 8. Measured on 2026-07-31, with the engine's own instrumentation

Everything in §6 was read off a screenshot by eye. It no longer has to be: `--bench-csv` writes
one row per frame (`frame, t_s, full_ms, cpu_ms, heap_pages, heap_capacity_mb, heap_used_mb,
heap_cap_pages, px, py, pz, yaw, pitch`) and `tools/shot.ps1` reduces it. Every row below names
the command that produced it and the CSV it came from.

### 8.1 Stationary at the spawn, this scene vs the inherited demo world

Same build, same binary, same camera pose, same 30 s convergence; the *only* difference is
`VOXL_TEST_SCENE` (`brushes.glsl:429`). This is the clean A/B that §6.2 said did not exist.

| | Voxl test scene | Demo world | |
|---|---|---|---|
| Full frame-time, mean | **11.13 ms (89.8 fps)** | 12.83 ms (77.9 fps) | −13% |
| p50 / p99 | 11.15 / 11.74 ms | 12.85 / 13.52 ms | |
| CPU-only, mean | 0.94 ms | 0.97 ms | |
| Heap capacity | 393216 pages (830 MB) | 393216 pages (830 MB) | same |
| **Heap in use** | **12.7 MB (2%)** | **145.7 MB (18%)** | **11.5×** |
| VRAM peak (whole device) | 2114 MiB | 2114 MiB | same |
| Frames sampled | 2921 | 2695 | |

```powershell
pwsh -File tools\shot.ps1 -Name 18-debug-overlay -Local "0.01,0.02,5.53" -Rot "0.785,1.096" `
     -ConvergeSec 30 -Seconds 34 -Graphs -Bench
```
`docs/benchmarks/18-debug-overlay.csv`, `docs/benchmarks/20-ab-demo-world-same-build.csv`.

The heap figure is the one that matters and it is now attributable to one decision rather than to
world size: both worlds are the same 64 m cube in the same binary. §6.2 rule (2) — keeping buried
volume bit-uniform — is worth 11.5× on this hardware.

### 8.2 Five-minute moving soak

`--patrol 13,30`: a 13 m circle about local (6, 6) at z = 5.5, one lap every 30 s (2.7 m/s, a
chunk boundary crossed roughly every 1.5 s), ten laps, camera looking along the tangent.

```powershell
pwsh -File tools\shot.ps1 -Name 19-soak-5min-patrol -Local "6,6,5.5" -Rot "0,1.45" `
     -Patrol "13,30" -ConvergeSec 296 -Seconds 300 -Bench -Graphs -VramIntervalMs 250
```

| | |
|---|---|
| Duration / frames | 300 s, **38 496 frames**, exit code 0 |
| Full frame-time | mean 7.81 ms (128 fps), p50 7.58, p90 8.99, **p99 18.27**, p99.9 20.68 |
| Frames over 33.3 ms | **1 of 36 496** (0.003%) |
| By lap phase (10-lap mean) | 6.88 ms looking out to sea → 9.78 ms looking into the hill |
| CPU-only | 0.83 ms |
| **Heap capacity** | grew once, at **t = 0.20 s**, 131072 → **393216 pages**, and never again |
| **Heap in use** | 12.23 / 12.76 / 12.66 / 12.76 / 12.70 MB at t = 30 / 60 / 120 / 180 / 240 s |
| Growth refusals | 0 |
| VRAM (1176 samples at 4 Hz) | 276 MiB before → **peak 2117 MiB (34% of 6144)** → 309 MiB after |

**No creep.** The heap reaches its working size in the first fifth of a second and holds it for
five minutes of continuous chunk regeneration, at 20% of the allocator's 1 955 198-page cap with
two further 1.5× growth steps still affordable. The p99 of 18.27 ms against a p50 of 7.58 is
chunk generation: the frames where the wrapping volume pulls in new chunks cost about 2.4× a
steady frame, and the overlay graph in `docs/images/19-soak-5min-patrol.png` shows them as
isolated spikes rather than a raised floor.

### 8.3 1920×1080

Reachable now that `--width`/`--height` exist. Same pose as §8.1, 20 s convergence:

| | 1280×720 | 1920×1055 |
|---|---|---|
| Full frame-time | 10.82 ms (92.4 fps) | **19.77 ms (50.6 fps)** |
| Heap capacity / in use | 393216 pages / 12.8 MB | 393216 pages / **14.5 MB** |
| VRAM peak | 2112 MiB | **2391 MiB (39% of 6144)** |

2.14× the pixels costs 1.83× the frame time and 279 MiB, and the voxel heap barely moves — the
extra memory is render targets, which is what the 1536 MiB engine reserve in
`utilities/allocator.inl` was sized to absorb. It absorbs it with room to spare.

**1055, not 1080**: Windows clamps the window to the desktop work area, so the swapchain surface
is 25 px shorter than asked for. That mattered — the first 1920×1080 run **died with
`0xC0000409` at exactly the frame the screenshot was due** (t = 19.90 s, the `--bench-csv` row
truncated mid-write, no PNG). The staging buffer and the image copy are now both sized from
`swapchain.get_surface_extent()` rather than from `window_size`, with an explicit size check
before the CPU-side read; after that the same command exits 0 and writes a correct
1920×1055 PNG. Which of the two paths held the stale number was not pinned down further — the
capture reads the swapchain's own extent now, so neither can be stale.
