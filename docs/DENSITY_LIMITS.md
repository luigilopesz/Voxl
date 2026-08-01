# Vegetation density limits

**Measured:** 2026-08-01, RTX 3050 6 GB Laptop (6144 MiB), i7-13650HX, Windows 11 Pro 26200.
**Configuration:** 1280x720 output, engine defaults (Balanced preset — 0.75 internal scale,
reflections off), profiler off, overlay off, `VOXL_DATA_DIR` isolated per run, GPU verified
uncontended for the whole of every row quoted.
**Statistic:** p50 of `full_ms` over `t` in [8 s, end − 0.5 s], 1600–5000 settled frames per row.
**Scene:** the 37 m Voxl island (`docs/SCENE.md`), `CHUNKS_PER_AXIS 16`.

Every number here was taken in a dedicated tree, `C:\voxl2_veg`, which is **git HEAD plus exactly
one file** — `src/voxels/brushes.glsl` — with its own binary built from those sources. §9 says why
that was necessary and how to reproduce it.

---

## 1. The answer in one table

Cost of each vegetation axis, relative to the shipped scene (one conifer, 50 blades/m²,
0.6 flowers/m², no undergrowth) at the spawn.

| axis | shipped | what breaks | at what number | mechanism |
|---|---|---|---|---|
| **trees** | 1 | nothing on this island | **242 stems / 3906 stems/ha still fine** — 46.8 MB heap of an 830 MB pool | none reached; heap is 5.6 % of capacity |
| **grass blades** | 50/m² | **near-camera blades stop rendering** | **between 126 and 147 blades/m²** | particle raster, *not* `MAX_GRASS_BLADES` — §5 |
| **flowers** | 0.6/m² | pool exhaustion, then flying quads | **~42/m² visible artefacts, hard stop by 126/m²** | `MAX_FLOWERS` 65536 + unbounded vertex write — §6 |
| **undergrowth (ferns)** | none | nothing up to 1 fern/m² | 1/m² costs 2.5 MB and 0.39 ms | none reached |

**Headline: memory is not the wall, and it is not close.** A reference-density forest —
1111 stems/ha, ferns, 126 blades/m², 6 flowers/m² — costs **30.7 MB of voxel heap against a
capacity of 830 MB and a hard cap of 4145 MB**, and **2239 MiB of a 6144 MiB card**. There were
**zero allocator growth refusals in any row of this sweep**, and heap capacity never moved off its
initial 393216 pages. The engine will run out of *frame time* and out of *particle-renderer
correctness* long before it runs out of memory.

**The thing that actually limits vegetation is the rasterised particle system**, which is where
grass and flowers live. It has two hard, silent failure modes and both are reachable at densities a
long way below "looks like the reference".

---

## 2. The unit rates — the numbers to do arithmetic with

These are the deliverable. Everything else in this document is the evidence for them.

| unit | voxel heap | frame time at the spawn | notes |
|---|---:|---:|---|
| **one conifer** | **229 KB** (fit over 1–74 stems, R² 0.984) | **+24.9 µs** (fit over 1–74 stems, R² 0.819) | falls to **141 KB / stem** over 1–242 stems as canopies start sharing palette regions |
| **1000 grass blades** | **0 bytes** | **0 ms, unmeasurable** | particles, never touch the heap; see §5 for the ceiling that is not about cost |
| **one flower per m²** (≈900 flowers island-wide) | 0 bytes | **+24.3 µs per flower/m²**, i.e. **+35.6 µs/m² below 59/m²** | R² 0.920 over 0.6–126/m² |
| **one fern per m²** (≈1000 ferns island-wide) | **+2.86 MB** | **+0.367 ms** | R² 0.930 / 0.774; ≈ **2.9 KB and 0.37 µs per fern** |

**A tree is 80 times more expensive in memory than a fern and 2500 times more expensive than a
blade of grass, and a blade of grass is free.** That is the whole shape of the problem: the palette
compressor makes solid terrain nearly free (13 MB for a whole island) and makes *foliage* the only
thing that costs, because a needle clump is 2–3 voxels of one of five greens interleaved with air
and no 8³ region a canopy touches is ever bit-uniform.

**Worked example.** A 100 m × 100 m forest at a realistic 800 stems/ha, with 0.5 ferns/m² and
grass at the safe ceiling:

```
  800 stems      x 229 KB/stem  =  179 MB heap,  +19.9 ms   <- frame time, not memory, is the wall
  5000 ferns     x 2.9 KB/fern  =   14 MB heap,   +1.8 ms   (0.5/m^2 over 1 ha)
  1.26 M blades  x 0            =    0 MB heap,    0.0 ms   (126 blades/m^2, the safe ceiling)
                                    193 MB heap,  +21.7 ms
```

The memory is affordable on this card. **The frame time is not**, and it is dominated by things
that are *near the camera*, which is exactly what §4 measures and what `PERFORMANCE_PLAN.md` §3.3
already predicted: near grass costs 2.06× what sky costs, and the far field is the cheap half.

---

## 3. Trees

`VOXL_VEG_FOREST 1` adds a lattice stand around the authored hero conifer, so the series is nested
— every denser point contains every sparser one. Stems are counted exactly, not estimated: a cell
qualifies on its *unjittered* centre against three distances, so the count is a closed-form
function of the spacing (`brushes.glsl`, the `VOXL_VEG_FOREST` block).

| spacing (m) | stems | stems/ha | p50 ms | fps | heap MB | VRAM MiB | gen settle (s) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| — (shipped) | **1** | 16 | **6.93** | 144 | **13.80** | 2029 | 0.66 |
| 16.0 | 4 | 39 | 7.78 | 129 | 14.38 | 2029 | 0.73 |
| 9.0 | 10 | 123 | 8.24 | 121 | 17.22 | 2028 | 0.67 |
| 5.0 | 22 | 400 | 8.34 | 120 | 18.01 | 2033 | 0.63 |
| 4.0 | 43 | 625 | 8.63 | 116 | 22.40 | 2030 | 0.75 |
| 3.0 | **74** | 1111 | **9.34** | 107 | 30.64 | 2038 | 0.79 |
| 2.0 | 142 | 2500 | 8.71 | 115 | 36.15 | 2024 | 0.74 |
| 1.6 | **242** | 3906 | 8.66 | 116 | **46.75** | 2025 | 0.87 |

```powershell
# each row, from the sweep driver; KNOBS restated in full on every row
scratchpad\veg\sweep.ps1 -ConvergeSec 16 -Seconds 21 -Points @(
  'T074|vista|VOXL_VEG_FOREST=1;VOXL_VEG_TREE_SPACING=3.0', ... )
```

### 3.1 Frame time peaks at 74 stems and then *falls*

This is the most useful counter-intuitive result in the sweep and it confirms, from the content
side, the mechanism `PERFORMANCE_PLAN.md` §1 states from the renderer side.

```
   1 stems  6.93 ms  |=========================
   4        7.78     |============================
  10        8.24     |==============================
  22        8.34     |==============================
  43        8.63     |===============================
  74        9.34     |==================================   <- peak
 142        8.71     |===============================      <- denser, but FASTER
 242        8.66     |===============================
```

**A ray that misses is the most expensive ray in the engine** — it runs the DDA to `MAX_STEPS` or
`MAX_DIST` and costs about 3.2× a ray that hits. Up to 74 stems, each tree adds surface without
removing much sky, so cost rises at a clean 24.9 µs/stem. Past that the canopy closes: the frame
stops containing sky and distant sea, every primary ray terminates within a few metres, and the
saving on the terminated rays cancels the cost of the extra geometry.

`docs/images/density/D04-trees-74-stems.png` is the peak and I opened it: the camera is inside the
canopy, the whole frame is needles, no sky and no horizon. `D03-trees-22-stems.png` is 400 stems/ha
— a clearing ringed with conifers, hill and sky still visible through the gap — and is the better
picture of the two.

**Consequence for content planning: budget trees by how much sky they take away, not by how many
there are.** The expensive configuration is a *sparse* stand against an open horizon, not a closed
forest.

### 3.2 Heap per tree falls as the stand closes

Marginal heap between consecutive points, in KB per stem: 302, 485, 67, 214, 272, 83, 109. Noisy,
because a tree lands where the lattice puts it relative to the 8³ palette-region grid, but the
trend across the range is real — **229 KB/stem below 74 stems, 141 KB/stem averaged to 242**.
Overlapping canopies share regions that are already allocated, so a dense stand is cheaper per tree
than a sparse one. Memory, like frame time, rewards closing the canopy.

---

## 4. Poses, and the reference-density forest

`R1` is the reference-density configuration: 74 stems (1111 stems/ha), 0.11 ferns/m²,
126 blades/m², 5.9 flowers/m². Control is the shipped scene. Runs interleaved, GPU uncontended.

| pose | control ms | R1 ms | delta | control heap | R1 heap |
|---|---:|---:|---:|---:|---:|
| **vista** (spawn, stationary) | 6.93 | **10.20** | **+3.26** | 13.80 MB | **30.74 MB** |
| **forest** (inside the stand, eye level) | — | 10.83 | — | — | 30.43 MB |
| **patrol** (13 m circle, 2.7 m/s) | 5.18 | **7.37** | **+2.19** | 13.00 MB | 31.23 MB |
| **cave** (chamber interior) | 6.05 | **6.50** | **+0.44** | 12.96 MB | 29.14 MB |

`R2`, denser still (142 stems, 0.41 ferns/m²), costs **9.24 ms** at the vista — again *faster* than
R1, for the §3.1 reason — and 36.05 MB.

Three things worth reading off this table:

- **The cave barely moves (+0.44 ms).** Vegetation is a near-field, outdoor cost. It is invisible
  from inside geometry, which is a useful validity check on the whole sweep: if the cave row had
  tracked the vista row, something other than vegetation would have been moving.
- **Moving is cheaper than standing still**, as `PERFORMANCE_PLAN.md` §3.3 found — but the
  vegetation *penalty* survives motion (+2.19 ms) and the p99 goes to **17.8 ms against a p50 of
  7.37**. That p99 is chunk generation: a forest costs more to generate per chunk, and the frames
  where the wrapping volume pulls in new chunks are the ones that spike.
- **A reference-density forest costs the Balanced tier about 3.3 ms at the worst pose**, taking it
  from 144 fps to 98 fps. It does not threaten the memory budget at all.

`docs/images/density/D12-reference-density-forest.png` is R1 at the vista and I opened it: dark
closed canopy overhead, a continuous fern-and-grass understorey, flowers scattered through it. It
reads as a forest floor, not as a lawn with trees on it.

---

## 5. THE WALL: grass blades stop rendering between 126 and 147 blades/m²

**This is the finding that matters most, because it is silent in every instrument except the
image.**

| blades/m² | `GRASS_FRAC` | p50 ms | heap MB | blades rendered? |
|---:|---:|---:|---:|---|
| 49.8 *(shipped)* | 0.2372 | 6.93 | 13.80 | **yes** — thick carpet |
| 99.6 | 0.4746 | 6.88 | 13.23 | **yes** |
| 126.0 | 0.60 | 6.76 | 13.93 | **yes**, thinning |
| **146.9** | 0.70 | 7.27 | 13.24 | **NO — near lawn flat** |
| 167.9 | 0.80 | 6.94 | 13.00 | yes (non-monotonic) |
| 188.9 | 0.90 | 7.77 | 13.62 | partial |
| 209.9 | 1.00 | 6.83 | 13.69 | **NO** |
| 256.0 | skin 1.0 × 1.0 | 7.18 | 13.05 | **NO** |

**Frame time does not move. Heap does not move. VRAM does not move. No error is printed. The
shader compiles. The grass is simply not in the picture.** Compare
`D05-grass-100-per-m2-ok.png` with `D07-grass-210-per-m2-blades-gone.png`: same pose, same
everything, and the second has a geometrically flat lawn where the first has a blade carpet.

This is the content-side twin of measurement trap (c) in the brief. A run that reported 6.83 ms at
210 blades/m² and 6.88 ms at 100 blades/m² would, without the image, have been written up as
"grass density is free up to 4× the shipped value". It is not; the renderer stopped drawing it.

### 5.1 What it is not

- **It is not `MAX_GRASS_BLADES`.** Doubling it from `1 << 20` to `1 << 21` — which doubles the
  strand pool, the three vertex buffers and the simulation dispatch, and costs 64 MiB of resident
  VRAM — **does not move the threshold at all.** 210 blades/m² is still a flat lawn
  (`P21-G210`, rebuilt binary, verified). 50 blades/m² still renders correctly on the same build,
  so the doubling itself is sound.
- **It is not the strand count.** The island carries roughly 1000 m² of grassable surface, so
  210 blades/m² is ~210 000 strands against a pool of 1 048 576. Five times the headroom.
- **It is not an accumulating leak.** A capture at t = 3 s looks the same as one at t = 14 s and
  t = 45 s. It is saturated from the first generated frames.
- **It is not the heap or the allocator.** Zero growth refusals, heap flat at 13 MB throughout.

### 5.2 What it is

**The near-camera cube path in the shared particle rasteriser.**
`D08-grass-210-near-gone-far-present.png` is the discriminator and it is unambiguous: at 210
blades/m² from a standing pose, blades **are** rendered on the mid-distance hill skirt and **are
not** rendered on the lawn in front of the camera. `src/voxels/particles/particle.glsl:70-102`
splits each blade by projected size — under 5 px it goes to the splat buffer, over 5 px to the cube
buffer — and it is the cube half that fails. Near blades are large; distant blades are splats;
distant blades survive.

The write itself is unbounded:

```glsl
// src/voxels/particles/particle.glsl:98
uint my_render_index = atomicAdd(PARTICLE_RENDER_PARAMS.cube_draw_params.instance_count, 1);
deref(advance(cube_rendered_particle_verts, my_render_index)) = packed_vertex;
```

There is no check that `my_render_index` is inside the buffer, and `instance_count` is fed straight
to `draw_indirect`. That is the shape of the defect. **The binding resource is not sized by
`MAX_GRASS_BLADES`**, because scaling that constant does not move the threshold, so naming the
exact buffer needs the owner of the particle system; the same unbounded write is what produces the
flying quads in §6, which is strong circumstantial evidence that this one path is the cause of
both.

The non-monotonicity (0.80 renders, 0.70 and 1.00 do not) is consistent with an overflow whose
victim depends on which particular voxels the hash selects, and not with a clean capacity limit.

### 5.3 What to do about it

**Treat 126 blades/m² as the supported ceiling — 2.5× the shipped density — and do not raise
`MAX_GRASS_BLADES` hoping to go further, because it does not help and it costs 64 MiB.** Fixing the
ceiling properly means bounding the two `atomicAdd` writes in `particle.glsl` and deciding what to
drop when the buffer is full; that is a change in a file this work does not own.

---

## 6. Flowers: the one pool that really is exhausted, and it is visible

Unlike grass, flowers cost real frame time — **+24.3 µs per flower/m²**, R² 0.920 — and unlike
grass, their pool genuinely runs out.

| flowers/m² | p50 ms | delta vs control | what the image shows |
|---:|---:|---:|---|
| 0.59 *(shipped)* | 6.93 | — | scattered colour in the grass |
| 5.88 | 6.96 | +0.03 | dense meadow, still reads as grass with flowers |
| 8.40 | 7.61 | +0.68 | |
| 16.79 | 7.68 | +0.75 | |
| 25.19 | 7.90 | +0.97 | |
| **41.98** | 8.47 | +1.53 | carpet; grass no longer visible between them |
| **58.78** | **9.07** | **+2.14** | **solid carpet + large white quads floating in the sky** |
| **125.95** | **9.98** | **+3.05** | **hard purple/green boundary where the pool ran out** |

`MAX_FLOWERS` is `1 << 16` = 65536 (`flower/flower.inl:11`). At ~1000 m² of lawn the pool is
exhausted at about **65 flowers/m²**, and `D10-flowers-126-per-m2-pool-exhausted.png` shows exactly
that: the near half of the island is a solid lavender carpet, the far half is bare green, and the
boundary is a hard line rather than a fade — flowers were allocated in chunk-generation order until
the allocator stopped handing out slots. `utilities/allocator.glsl:38` clamps an over-budget
allocation to the *last valid element*, so every excess flower writes to the same slot.

**The flying quads arrive before the pool does.** `D09-flowers-59-per-m2-flying-quads.png` shows
metre-scale grey rectangles hanging in the sky above the sea at 59/m², which is under the pool
limit. Same unbounded vertex write as §5.2: `CONSERVATIVE_PARTICLE_PER_FLOWER` is 27, so a flower
emits up to 27 vertices against a buffer of `MAX_FLOWERS × 27`, and once live flowers exceed the
pool the indices leave the buffer.

**Supported ceiling: ~25 flowers/m².** That is already 40× the shipped density and is well past the
point where the meadow reads as flowers-in-grass rather than as a flower carpet — the aesthetic
limit (about 6/m²) arrives long before the technical one.

---

## 7. Undergrowth: ferns, and the one axis that behaves

Ferns were added by this work (`voxl_fern_voxel` in `brushes.glsl`) precisely because grass and
flowers are particles and therefore cannot answer the question the brief asks — what does
non-uniform *volume* at ankle height cost. A fern is real voxels: it bounces light, appears in
reflections, and casts ray-traced shadows.

| ferns/m² | spacing/prob | p50 ms | heap MB | delta heap |
|---:|---|---:|---:|---:|
| 0 | off | 6.93–7.16 | 13.50 | — |
| 0.061 | 3.0 / 0.55 | 7.03 | 13.31 | −0.19 (noise) |
| 0.114 | 2.2 / 0.55 | 7.24 | 13.20 | −0.30 (noise) |
| 0.408 | 1.4 / 0.80 | 7.31 | 14.86 | +1.36 |
| **1.000** | 1.0 / 1.0 | **7.43** | **16.01** | **+2.51** |

```
heap_MB = 13.27 + 2.860 x ferns/m^2      R^2 = 0.930
frame_ms =  7.09 + 0.367 x ferns/m^2     R^2 = 0.774
```

**Nothing breaks.** One fern per square metre over the whole island — about 1000 of them, visibly a
continuous understorey in `D11-ferns-1-per-m2.png` — costs 2.5 MB of heap and 0.39 ms. This is the
axis with the most headroom left and the one that most changes how the ground reads, because it is
the only near-ground element that is actually three-dimensional to the ray tracer.

**Grass palette (the visual gap).** `docs/images/density/D01-palette-before.png` against
`D02-palette-after.png`. Before: five greens spanning one hue at five brightnesses, which integrate
at viewing distance into a single flat mint. After: eleven entries chosen by *two* independent
noise fields — a 22 m species field and a 6.5 m warm/cool field — so patches of parched yellow,
blue-grey and rust drift independently instead of in stripes. Cost, measured as an A/B at the same
pose: **7.155 ms / 13.19 MB before, 6.933 ms / 13.80 MB after** — the frame-time difference is
0.22 ms, under the project's 0.3 ms noise floor, and the heap difference is 0.6 MB on 13 MB.
`pack_rgb` keeps 6 bits per channel, so every one of the new entries was already representable; the
only thing that changed is *which* discrete value a voxel picks. Shipped on by default
(`VOXL_VEG_GRASS_PALETTE 1`).

---

## 8. Generation time

`gen settle` is the wall time from the first non-zero heap byte to the heap reaching 99.5 % of its
final size. It is not sensitive to vegetation density in any useful way:

| configuration | gen settle (s) | worst frame in the window (ms) |
|---|---:|---:|
| shipped | 0.66 | 30 |
| 242 stems | 0.87 | 83 |
| 1 fern/m² | 1.90 | 32 |
| R1 reference forest | 2.28 | 49 |

**The reference forest takes 3.5× as long to fill the world and its worst generation frame is 49 ms
rather than 30.** That is a one-off at startup when standing still, but while *walking* it recurs
every time the wrapping volume pulls in a chunk face — which is what the R1 patrol p99 of 17.8 ms
against a p50 of 7.37 ms is. Vegetation makes chunk-generation hitches roughly 2.4× worse.

A caution for anyone re-running this: the first 5–10 frames of every run are dominated by **SPIR-V
recompilation**, not by world generation, because editing a knob invalidates the cache for
`voxel_world.comp.glsl`. Those frames cost 300–2000 ms regardless of density. The window used here
starts at the first non-zero heap byte, by which point the pipeline necessarily exists.

---

## 9. How these numbers were taken, and why not in `C:\voxl2`

**Three agents shared this GPU and this repository while the sweep ran, and both facts corrupted
results before they were controlled for.** Recording it because the next person will hit the same
thing.

1. **The shared tree is not a stable measurement target.** An in-flight edit to
   `trace_primary.comp.glsl` referenced a push-constant field the prebuilt binary did not have;
   because GLSL is compiled at runtime and the matching `.inl` needs a C++ rebuild, the primary
   trace pass was deleted and the process hung for 320 s. Later, a far-field change to
   `VOXELS_BUFFER_PTRS` in `voxels.inl` named buffers the world-generation task head does not
   provide, and world generation stopped compiling entirely. **Every measurement here was therefore
   taken in `C:\voxl2_veg` — git HEAD plus exactly one file, `src/voxels/brushes.glsl`, with its
   own binary built from those sources.** That binary is byte-size-identical to the one HEAD
   produces (5 801 472 bytes), which is the check that the isolation did not change anything else.
2. **A second engine instance roughly doubles frame time; four of them produce 1384 ms frames and
   5.8 GB of 6.1 GB VRAM, and the process aborts with `0x80000003`.** The driver samples the
   `gvox_engine` process count every 400 ms and records the *fraction* of samples that saw more
   than one. **Every row quoted in this document has `cont_frac = 0.000`.** Rows taken during a
   contention window were discarded, not corrected.
3. **Engine stdout is captured and grepped** for `GLSLANG`, `compilation errors` and growth
   refusals, and the row is marked invalid rather than recorded. No row here has a shader error.
   That instrument is necessary but not sufficient — §5 is a case where every instrument was clean
   and the picture was still wrong, which is why every screenshot in this document was opened.

The knob block lives at the head of the vegetation section of `brushes.glsl` and is the only thing
a sweep changes. The defaults reproduce `docs/SCENE.md` exactly, and the control row confirms it:
**13.19 MB of heap against the 13 MB recorded in `docs/SCENE.md` §6.1.**

---

## 10. What to build next, in order

1. **Bound the two `atomicAdd` writes in `src/voxels/particles/particle.glsl`** (lines 95 and 98,
   and the shadow write at 103). They are the cause of both silent failures in this document and
   the fix is a comparison and an early return. Until then, 126 blades/m² and 25 flowers/m² are
   hard content limits enforced by nothing but this document.
2. **Ferns, or something like them, are the cheapest way to make the ground read as authored.**
   0.4/m² costs 1.4 MB and 0.27 ms and is the single largest visual change measured here.
3. **Do not raise `MAX_GRASS_BLADES`.** It costs 64 MiB per doubling and buys nothing measurable —
   the pool is at 20 % occupancy at the density where rendering already fails.
4. **Budget trees by sky removed, not by stem count.** The expensive stand is the sparse one.
5. **When the far field lands, re-run §3 with it enabled.** Every frame-time number here was taken
   with an empty horizon, and §3.1 shows the sky is what dominates the cost curve — a far field
   that fills the horizon will change the shape of that curve, probably in the direction that makes
   dense vegetation look cheaper still.
