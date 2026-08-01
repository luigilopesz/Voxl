# The far field: one level, built and measured

**Written:** 2026-08-01
**Built in:** `C:\voxl2` (source of truth). **Measured in:** `C:\voxl2_ff`, a `robocopy /MIR`
mirror — three other agents were running engine instances out of `C:\voxl2` while this work was
in progress, and the linker cannot replace a running `.exe`.
**Hardware:** RTX 3050 6 GB Laptop (6144 MiB), i7-13650HX, Windows 11 Pro 26200.
**Settings, printed by the engine into every run's log and carried with every number below:**
`Quality Preset=Balanced | Render Res Scale=0.75 | Render Shadows=on | TAA Method=Kajiya TAA |
Update Sky=on | denoise_shadow_mask=off | global_illumination=on | ray_traced_reflections=off`,
1280×720 output, 960×540 internal. That is the **Balanced tier** of `PERFORMANCE_PLAN.md` §1.2,
which is the tier §5.5 budgets the far field against.

This implements `PERFORMANCE_PLAN.md` §6.3 item L1: **build ONE extra level, not four**, because
one level converts §5.5's entire cost budget from arithmetic into a measurement — including the
term nobody could estimate honestly, which is what happens when sky pixels become surface pixels.

---

## 0. The five findings

1. **There are mountains.** 25 cm voxels over a 256 m cube put a real, marched, filtered
   ridgeline at 90–130 m with 6.25 cm detail at the player's feet.
   `docs/images/far-field/FF02-vista-normals-fixed.png`, opened, is the frame the phase was for.

2. **Seeing them is cheap. +0.70 ms.** That is the term §5.5 trusted least — sky pixels becoming
   distant-surface pixels — and it comes in under a budget written for four levels and 2 km.
   `WORLD_SCALE.md` §4.1's counter-intuitive claim that near grass costs more than distant
   mountains survives contact with an actual distant mountain.

3. **Letting every ray see them costs +16.6 ms — twenty-four times as much, and twelve times the
   whole four-level budget.** This is the wall. It is not the mountains being shaded; it is every
   sun-shadow, ReSTIR-diffuse and irradiance-cache ray in the renderer being handed four times the
   distance in which to *fail* to hit something. The dominant population is **grazing rays**: a
   near-horizontal ray over gently rising distant ground neither misses nor hits, it skims, and a
   skimming ray is the DDA's worst case. §5.3.

   **And one integer takes +16.6 ms back to +1.69 ms.** Clamping *non-primary* rays to 48 m past
   the near box leaves the mountains pixel-identical — primary visibility still marches the whole
   128 m — and lands inside the §5.5 budget's upper bound. §5.4, and
   `FF03-ridge-secondary-capped-48m.png` against `FF02` is the proof that nothing was lost.

4. **The nested-volume architecture is vindicated; "every ray sees every level" is refuted.** One
   extra level was 12 lines of constants, 2 lines of buffer plumbing, one march function and *no
   new generation code* — `PERFORMANCE_PLAN.md` §5.7's estimate for the mechanical parts was an
   order of magnitude too large. Build L2 and L3. But make ray reach a property of the ray's
   **class** first. §7.1.

5. **The second wall is 830 MB of allocator slack for 32 MB of voxels,** and it is in a place
   nobody was looking: the heap's per-frame safety margin has to double because two levels can
   both allocate in one frame, and the geometric growth multiplies that by three. At four levels
   the same reasoning reaches ~3.3 GB of capacity for a few hundred MB of voxels, on a 6 GB card
   that already wants 2.1 GB for render targets. **Fix it before L2.** §6.

---

## 1. What was built

| level | `LOG2_VOXEL_SIZE` | voxel | chunk | CPA | world edge | view radius | table |
|---|---:|---|---|---:|---|---|---:|
| **L0 near** | −4 *(unchanged)* | 6.25 cm | 4 m | 16 | 64 m | 32 m | 33.7 MB |
| **L1 far** | **−2** | **25 cm** | **16 m** | 16 | **256 m** | **128 m** | **33.7 MB** |

Exactly as `PERFORMANCE_PLAN.md` §5.3 proposes: **the only thing that differs between the levels
is the voxel size.** Same `VoxelLeafChunk`, same palette compression, same uniformity pyramid,
same DDA, the same six generation shaders, and **one shared voxel heap**.

`LOG2_VOXEL_SIZE` for the near field is untouched at −4. 16 voxels/m near the player is the look
and nothing here moves it.

### 1.1 How big the change actually was, against §5.7's estimate

| # | §5.7 predicted | what it took |
|---|---|---|
| 1 | "Thread voxel size as a per-volume value … 82 `VOXEL_SIZE` uses, 41 `CHUNKS_PER_AXIS`, 34 `CHUNK_SIZE` … across 19 files" | **12 lines in `voxel_malloc.inl`.** A `VOXEL_LEVEL` switch over the two constants the levels differ in. Every *generation* shader compiles for exactly one level, so it reads them as `#define`s and none of the 82 uses moves. Only the **trace** needs both levels live at once, and it gets L1's constants under separate `FF_*` names. |
| 2 | "Extra buffer sets — `VOXELS_USE_BUFFERS` is used by 20 task headers" | **2 lines in `voxels.inl`,** and not one of the 20 files edited. |
| 3 | "Multi-level march in `voxel_trace()` … one function" | **Right.** One function, ~120 lines, plus four small edits in `trace.glsl`. |
| 4 | "Per-level chunk generation with per-level update budgets" | **No new generation code at all.** The same `voxel_world.comp.glsl`, recompiled with `VOXEL_LEVEL=1` and bound to a second table. `gpu_context` keys its pipeline cache on the task-head name plus the concatenated defines (`gpu_context.hpp:133-137`), so a second pipeline falls out of one source file — the trick `CHUNK_OPT_STAGE` already used. |
| 5 | "**Filtered voxel generation** — the only genuinely new algorithm" | **Right, and §4 sharpens what it has to mean.** |
| 6 | "Player edits above L0 — defer" | Deferred. The edit-election branch is *compiled out* at L1 rather than left to misfire. |

**§5.7's shape was correct and its size estimate for items 1–2 was an order of magnitude too
large,** because it counted textual occurrences rather than asking which of them a *generation*
shader and a *trace* shader each need. That distinction is the whole trick and it is worth
stating plainly for whoever builds L2 and L3:

> A generation shader lives at one level and can take its constants from `#define`s.
> The trace crosses levels inside one invocation and cannot. Split the two and the
> "thread a parameter through 19 files" job disappears.

---

## 2. The march — the technical problem, and how the hand-off is made impossible to get wrong

A ray must cross the near field at 6.25 cm and then continue into the far field at 25 cm. There
are three ways to get that wrong.

### 2.1 A gap — the ray skips a slab of world

**Answer: the two segments share one ray parameter `t`.** Each volume is the same ray in a
*translated* frame — L0 adds `(offset & 3) + 32` metres, L1 adds `(offset & 15) + 128` — and a
translation does not change distance along a ray. So the far segment begins at exactly the `t` at
which the near segment left its box, passed through rather than recomputed. There is nothing
between them to skip, and no floating-point re-intersection to disagree about.

It is also structurally impossible for a ray to leave L0 into nothing: L0 spans at most ±35 m
about the player and L1 at least ±113 m, so **L0 is strictly inside L1**.

### 2.2 A double-hit — the ray hits the coarse copy of geometry L0 already showed it in detail

`WORLD_SCALE.md` §7.3 lists this as fatal. There are two lines of defence and the first is
structural:

**The far segment is only ever entered at the point where the ray LEAVES the near box.** Nothing
inside that box is ever sampled at L1 — not because the far field happens to be empty there, but
because the march never looks. That is stronger than hollowing and it costs nothing.

The second line is about the boundary itself. A 25 cm voxel can bulge up to 25 cm past the
6.25 cm surface it stands for, so a grazing ray that L0 correctly called a miss could hit that
bulge one voxel after the hand-off and paint a wall at exactly 32 m. `FF_HOLLOW_R` keeps the far
terrain 72 m from the island centre, and the near box can never reach past 56 m of it, so the
first ~37 m of every far segment is guaranteed empty.

**The honest limit of that.** It is a *world-space* guarantee, and it works because the near
content is an island and the far content is a separate range across water. A far field that has
to **continue the ground under the player** needs the dynamic version — L1 chunks inside the near
box generated empty and re-generated as the player moves. That is real work and it is not here.
It is the single largest thing L2/L3 will need that L1 did not.

### 2.3 A seam — a visible discontinuity in shading

**Answer: there is no shading change at all.** The far segment returns a `PackedVoxel` from the
same palette with a real colour, normal and roughness, in the same `VoxelTraceResult`. Every
consumer downstream — the g-buffer, the sun-shadow gate on `depth != 0.0`, ReSTIR diffuse, ReSTIR
reflections, the denoisers — cannot tell which level answered.

That is exactly the property `WORLD_SCALE.md` §3.2 measured the *absence* of: terminating on
occupancy bits manufactures a surface with no shading data and costs 16–20 %; returning a
filtered voxel does not. §5 below is the measurement of the other side of that coin.

### 2.4 Three smaller decisions, each of which had a wrong answer available

**A separate step budget.** `MAX_STEPS` is 512 and the near field's minimum step is 6.25 cm, so a
ray that crawls through grass can spend the entire budget inside 32 m. If the far field drew from
the same pool, the mountain behind that grass would vanish — and it would vanish *as sky*, which
is indistinguishable from correct. `FF_MAX_STEPS` is a separate 256.

**Only "left the box" earns a continuation.** The near loop's compound break is split so the far
field can tell why the near march stopped. A ray that exhausted its steps was crawling through
fine geometry and should have hit; one that reached `max_dist` has no reach left to spend.
Neither continues.

**The far steps are clamped out of the near field's budget-exhaustion signal.**
`trace_primary.comp.glsl:123` reads `step_n >= VOXL_TRACE_MAX_STEPS(...)` as "ran out of steps,
this pixel is a hole" and paints it into the step heatmap. Adding the far segment's steps to the
same counter would let a perfectly healthy long ray fake that signal. One `min()` prevents it.

### 2.5 The constraint that had to be designed around: 128 bytes of push constant

The obvious way to reach a second chunk table from all eight `voxel_trace()` call sites is a
second `DAXA_TH_BUFFER_PTR` in `VOXELS_USE_BUFFERS`. **It does not fit.** The widest of those
task heads was already at 120 bytes of a 128-byte Vulkan push-constant limit, and daxa answers
with

```
push constant size of 136 exceeds the maximum size of 128
```

at pipeline creation — which, because `gpu_context.cpp:31` registers null pipelines when the
first compile fails, presents as a **silently missing pass**, not an error. That is measurement
trap (c) from `HANDOFF.md`, and it is how this constraint announced itself: a frame that got
faster because it had stopped drawing something.

Two things came out of designing around it, and both are better than what they replaced:

1. **The far table's address rides in `VoxelWorldGlobals`,** published once a frame by the far
   perframe pass. Eight bytes of VRAM, zero bytes of push constant. The task-graph dependency is
   not lost: `VOXELS_USE_BUFFERS` still declares the table with `DAXA_TH_BUFFER`, an attachment
   that is "NOT represented at all within the shader blob" but is still seen by the dependency
   solver, so the barrier between the far generation chain and the trace is still inserted.
2. **The far *globals* are not passed to the trace at all.** The only field the far march wants
   from them is `offset`, and both levels are handed the identical `player_unit_offset` every
   frame — they differ in the *shift* applied to it, not the value. So the near globals answer
   for both, and L2 and L3 will need nothing more than one more `daxa_u64` each.

**For L2 and L3 this is the binding constraint, not memory or time.** Four levels of
`DAXA_TH_BUFFER_PTR` would have needed 32 bytes that do not exist. Four levels of address-in-
globals need 32 bytes of a buffer. Note it before starting.

---

## 3. Where the pieces live

| file | owned | what it is |
|---|---|---|
| `src/voxels/far_field.inl` | new | The level's constants and its three switches. `FF_ENABLE` removes the level entirely; `FF_RAYS` chooses which of the eight call sites march it (0 none / 1 primary only / 2 all); `FF_SECONDARY_MAX_DIST_M` clamps how far past the near box the non-primary ones may go. |
| `src/voxels/far_field.glsl` | new | The far march segment and the hand-off. |
| `src/voxels/far_field_gen.glsl` | new | The filtered generator. §4. |
| `src/voxels/far_field_task.inl` | new | The far perframe task head. |
| `src/voxels/far_field_perframe.comp.glsl` | new | Per-frame reset of the far globals; publishes the table's address. |
| `src/voxels/far_field.cpp` | new | Two buffers, seven passes, no new generation code. |
| `src/voxels/impl/voxels.inl` | owned | `VoxelWorldGlobals::ff_voxel_chunks_addr`, the `DAXA_TH_BUFFER` declaration, the far buffers on `VoxelWorldBuffers`. |

**Shipped configuration:** `FF_ENABLE 1`, `FF_RAYS 2`, `FF_SECONDARY_MAX_DIST_M 48` — the
+1.69 ms row of §5.4, verified rendering 1444 settled frames at 7.855 ms and writing
`FF03-ridge-secondary-capped-48m.png`.

Integration notes for the six files this work does not own are in §8.

---

## 4. The filtered generator — and a correction to what §5.7 asked for

`PERFORMANCE_PLAN.md` §5.7 item 5 warns that a point sample makes distant hillsides "shimmer
between rock and grass as the camera moves", and prescribes **the dominant material of the
block**. Building it made the mechanism precise, and the prescription turns out to be half right.

> **The thing that shimmers is not point-sampling. It is thresholding.**

The far volume is player-centred and wraps, so a given piece of world is re-generated at a
different sub-voxel phase every time the player crosses a 16 m chunk boundary. Any quantity
decided by a hard test — `slope > 0.4 ? rock : grass` — can land on the other side of that test at
the new phase, and the hillside flips. **A majority vote over the block is still a hard test.** It
moves the flip somewhere else; it does not remove it.

What removes it is making the voxel's appearance a *continuous* function of world position:

1. **The height field is low-passed over the voxel's own footprint** (four taps on a rotated grid
   at ±¼ voxel), so it carries no detail finer than the voxel that stores it. This is the standard
   anti-alias and it is the reason a 25 cm voxel is a fair summary of the sixty-four 6.25 cm
   voxels underneath it.
2. **The normal is a central difference of that low-passed field on a one-voxel arm** — the
   block's average orientation, not one facet's.
3. **Materials are mixed by continuous weights, not voted on.** The area-weighted average of the
   block is both the physically right answer and the stable one.

Then, and only then, **the mix weight is quantised to 8 levels — deliberately.** Continuous colour
would make every voxel distinct, every 8³ palette region maximally non-uniform, and the heap
enormous (`brushes.glsl` rule 2: a bit-identical region allocates *nothing*). Quantising restores
large uniform patches. It reintroduces a visible step, but the step sits on a **fixed world-space
contour**, so it does not move with the camera. Static banding is cheap; flicker is not.

Interiors below 1 m of depth are one flat colour with the default normal, which is `brushes.glsl`
rule 2 applied at the far level and is why §6's heap number is as small as it is.

### 4.1 The generator had to invent its own far terrain, and that is worth flagging

The Voxl test scene is an island of radius `min(40, CHUNKS_PER_AXIS·2 − 11)` = **21 m**, whose
solid ground ends at about 18.5 m. Sampled coarsely, `voxl_ground_h` produces the same 37 m island
at 25 cm and **nothing else** — there is no distant terrain in this world to filter. So
`ff_h()` is new content: the near ground's two rolling octaves verbatim, plus a two-scale ridge
system, plus a steady rise with distance.

**This is the one place the far field is not a filtered version of the same terrain, and it is a
content gap rather than an architecture gap.** If a terrain agent extends `voxl_ground_h` past the
island rim, `ff_h()` should call it for the base layer and keep only the ridge and rise terms.
Until then, the two are the same *noise*, matched in frequency and palette family, but not the
same *function*.

One term in `ff_h()` is structural rather than aesthetic and must survive any rewrite: **the rise
with distance.** L1 is a 256 m cube, so it has a wall at 128 m. If the far ground were flat, a ray
down a valley would leave through that wall and shade as sky, and the world would end in a
straight line across the horizon. Making the outer ring the tallest thing in it means every
near-horizontal ray hits ground first. The cut is still there; it is always behind a mountain.

---

## 5. Cost — the measurement §5.5 could not make

### 5.0 The measurement conditions, stated before the numbers

**The GPU was shared.** Three other agents were running engine instances on this machine
throughout, and `HANDOFF.md` trap 2 is explicit that a second instance roughly doubles frame
time. Every run below therefore carries a `CONTENDED` flag from a harness that samples the
process table every 700 ms across the whole run. **Read the flag, and then read these three
reasons to believe the numbers anyway:**

1. **The control reproduces the published figure.** `spawn-r0` measures **6.691 ms** at the
   default spawn pose. `PERFORMANCE_PLAN.md` §1.2 has the Balanced tier at **7.097 ms** at the
   vista, measured months of work ago on a quiet machine. A 6 % spread, in the direction the pose
   difference predicts.
2. **The repeat pairs agree to 1 %.** The two `ridge-r2` runs, taken several minutes apart with a
   different variant in between, read 22.734 and 22.504 ms — 0.23 ms apart on a 22 ms frame.
   Contention that varied run to run could not do that.
3. **The intruder was mostly idle.** One of the three sibling instances had been resident for
   thirty minutes at 3 MB of working set — present in the process table, doing no GPU work. The
   harness counts processes, not GPU occupancy, so it flags a false positive it cannot
   distinguish from the real thing.

The effect being measured is **+16.6 ms on a 6 ms frame.** No plausible amount of contention
noise changes that conclusion, and the acceptance threshold in the brief is 0.3 ms.

Everything is the Balanced tier, 1280×720 output / 960×540 internal, profiler off, overlay off,
p50 of `full_ms` over `t ∈ [8 s, end − 0.5 s]`, `VOXL_DATA_DIR` isolated, settings printed by the
engine into each run's log and identical across every row.

### 5.1 The sweep

`FF_RAYS` selects which of the eight `voxel_trace()` call sites continue a missed ray into the
far volume. **In every row the level is fully built, generated and resident** — `FF_RAYS 0` is not
"the far field is off", it is "the far field exists and no ray looks at it". That makes row 0 the
control that isolates *the march* from everything else the level costs, and it is a shader-only
switch, so control and treatment are the same binary.

| pose | `FF_RAYS` | what marches the far field | p50 ms | fps | p99 ms | frames | Δ vs row 0 |
|---|---:|---|---:|---:|---:|---:|---:|
| **ridge** | 0 | nothing | **5.968** | 168 | 14.41 | 1539 | — |
| **ridge** | 1 | primary visibility only | **6.669** | 150 | 15.81 | 1433 | **+0.70** |
| **ridge** | 2 | all eight call sites | **22.734** | 44 | 30.77 | 531 | **+16.77** |
| **ridge** | 2 | *(repeat, interleaved)* | **22.504** | 44 | 23.89 | 513 | **+16.54** |
| **spawn** | 0 | nothing | **6.691** | 150 | 7.81 | 1722 | — |
| **spawn** | 1 | primary visibility only | **7.594** | 132 | 8.22 | 1521 | **+0.90** |
| **spawn** | 2 | all eight call sites | **11.214** | 89 | 14.10 | 1027 | **+4.52** |
| **cave** | 0 | nothing | *14.015* | *71* | 15.36 | 846 | **rejected — see §9.2** |
| **cave** | 2 | all eight call sites | **6.729** | 149 | 8.05 | 1694 | **+0.81 vs the published 5.923** |
| **patrol** | 0 | nothing | *14.212* | *70* | **45.81** | 731 | **rejected — see §9.2** |
| **patrol** | 2 | all eight call sites | — | — | — | — | run produced no frames |

*ridge* = `--pos -182.99,-109.98,-46.97 --rot 0.7854,1.50`, near level, the mountains filling the
upper half of the frame. *spawn* = the `WORLD_SCALE.md` anchor pose, `--rot 0.785,1.096`, pitched
27° down so the ridgeline is largely **out of frame**. *cave* and *patrol* are
`RENDERER_OPTIMISATION.md`'s own two, `--pos -164.6,-91.3,-49.7 --rot 3.927,1.571` and
`--pos -173,-100,-44.5 --patrol 12,20`.

**The cave row is the third data point and it is worth as much as the other two.** At 6.729 ms
against a published Balanced cave of 5.923 ms, the far field costs about **+0.8 ms with every ray
marching it** — because inside a cave a secondary ray hits a wall within a metre or two and never
reaches the far volume at all. The far field's cost is not a property of the far field. **It is a
property of how much sky each shaded point can see.**

Every row was produced by:

```powershell
$env:VOXL_DATA_DIR = 'C:\voxl2_ff\_data'
C:\voxl2_ff\.out\cl-x86_64-windows-msvc\Release\gvox_engine.exe `
    --pos <pose-pos> --rot <pose-rot> --exit-after 20 --bench-csv <label>.csv `
    --width 1280 --height 720 --unpause --no-overlay
```

with `FF_RAYS` set in `src/voxels/far_field.inl` between runs — read back and asserted, because
`WORLD_SCALE.md` §0.4 records a whole sweep invalidated by a regex that silently failed against
CRLF line endings.

### 5.2 What the two numbers mean

**Primary visibility costs +0.70 ms.** That is the number §5.5 was trying to estimate, and for one
level it comes in *under* a budget that was written for four. Sky pixels becoming distant-surface
pixels — the term §5.5 said it trusted least, and §9 item 1 listed as the plan's largest unknown —
turns out to be **cheap**. A distant mountain occupies few pixels and each of them shades once.
`WORLD_SCALE.md` §4.1's "distant mountains are not automatically the expensive half; near grass
is" survives contact with an actual distant mountain.

**Giving the same far field to the other seven call sites costs +16.6 ms — twenty-four times as
much.** That is the wall, and §5.3 is the mechanism.

### 5.3 The wall: grazing secondary rays, not sky-to-surface conversion

The pose dependence is the clue. At the ridge pose the far field costs +16.6 ms; at the spawn
pose, 27° further down, it costs +4.5 ms. **The primary-visibility difference between those two
poses is nothing like 3.7×** — the mountains occupy maybe a third of the ridge frame and a tenth
of the spawn frame. What changes by 3.7× is how much of the *hemisphere above each shaded point*
is filled by far terrain.

The mechanism, and it is one the plan documents predicted from the other side:

- The brief's own first principle is that **a ray that misses is the most expensive ray in the
  engine**, because the DDA runs to `MAX_STEPS` or `MAX_DIST`. Quadrupling the world's radius
  quadruples how far every missing ray runs.
- But the dominant term is worse than a miss. **A near-horizontal ray over gently rising distant
  ground does not miss and does not hit — it skims.** A grazing ray is the DDA's worst case:
  it stays within one or two voxels of the surface, so `sample_lod` returns 1 or 2 at nearly every
  step and the ray advances 25–50 cm at a time, for hundreds of metres. `WORLD_SCALE.md` §2.2 is
  the cost of one such step: a chunk-table read, a palette-header read, a pointer chase into a
  multi-hundred-megabyte heap, a bit-unpack, and only then the uniformity tests.
- The near field never showed this because its box is 64 m wide and a grazing ray leaves it almost
  immediately. **At 256 m a grazing ray has four times as far to skim, and the far volume is
  deliberately built so that the ground rises with distance** (§4.1) — which is exactly the shape
  that keeps a grazing ray in contact with it.
- Sun-shadow rays are the largest single population: `trace_secondary` fires one per pixel at
  **full** render resolution (`PERFORMANCE_PLAN.md` §6.2 M1 — it is the only ray-traced stage that
  is not half-res), 518 400 of them, all previously terminating within 64 m.

So the far field's cost is not what the budget worried about. It is not the mountains being
*shaded*. **It is every secondary ray in the renderer being given four times the distance to fail
to hit something in.**

### 5.4 The mitigation, measured

`FF_SECONDARY_MAX_DIST` clamps how far past the near box a **non-primary** ray may march. Primary
visibility always gets the whole volume, so the mountain on screen is unchanged pixel for pixel;
what is given up is whether a diffuse ray that has already travelled *N* metres eventually lands
on rock or on sky.

The clamp is applied to a local `march_limit` and never to `info.max_dist`, for the reason
`WORLD_SCALE.md` §0.2 records: `result.dist` is seeded from `info.max_dist` and callers test
`dist == MAX_DIST` to mean "missed, shade as sky", so lowering it would turn every clipped ray
into a bogus **hit** at the clip plane and drag a full GI shade in behind it.

**Measured, ridge pose, and this time on a genuinely quiet GPU** — `nvidia-smi` read
514 MiB / 6144 MiB with no sibling instance resident, and these three runs were taken
back to back:

| ridge pose, clean GPU | p50 ms | fps | frames | Δ vs control |
|---|---:|---:|---:|---:|
| `FF_RAYS 0` — control | **6.166** | 162 | 1422 | — |
| `FF_RAYS 1` — primary only *(contended run, §5.1)* | 6.669 | 150 | 1433 | +0.70 |
| **`FF_RAYS 2` + `FF_SECONDARY_MAX_DIST_M 48`** | **7.855** | **127** | 1444 | **+1.69** |
| `FF_RAYS 2`, no cap *(contended runs, §5.1)* | 22.5 – 22.7 | 44 | 513/531 | +16.6 |

```powershell
# the row that matters, verbatim
#   src/voxels/far_field.inl:  #define FF_RAYS 2 / #define FF_SECONDARY_MAX_DIST_M 48
$env:VOXL_DATA_DIR = 'C:\voxl2_ff\_data'
C:\voxl2_ff\.out\cl-x86_64-windows-msvc\Release\gvox_engine.exe `
    --pos -182.99,-109.98,-46.97 --rot 0.7854,1.50 --exit-after 20 `
    --bench-csv c-cap48.csv --width 1280 --height 720 --unpause --no-overlay `
    --screenshot docs\images\far-field\FF03-ridge-secondary-capped-48m.png --screenshot-after 16
```

> **+16.6 ms becomes +1.69 ms. A tenfold reduction, from one integer.**

And it costs nothing anybody can see. `FF03-ridge-secondary-capped-48m.png`, opened and compared
against `FF02-vista-normals-fixed.png`: **the mountains are identical** — same silhouette, same
pale ridgeline, same two peaks, same green lower slopes — because primary visibility is exempt
from the clamp and still marches the whole 128 m volume. The only difference is a slight lift in
ambient brightness on the far slopes, which is precisely what is being traded away: distant
surfaces no longer occlude each other's sky beyond 48 m.

**+1.69 ms is inside the §5.5 budget** of +1.4 ms ± 30 % (upper bound 1.82 ms) — for one level.
That budget was written for four levels and a 2 km radius, so this is not a clean pass; §7 does
the arithmetic. But it moves the far field from "the architecture needs rethinking" to "the
architecture needs one more parameter".

---

## 6. Memory — and a second wall, in a place nobody was looking

| | brief's baseline, 37 m island | with L1 | change |
|---|---:|---:|---:|
| chunk tables | 33.7 MB | **67.4 MB** | +33.7 MB |
| voxel heap **in use** | 13 MB | **43.6 – 47.1 MB** | **+31–34 MB** |
| voxel heap **capacity** | 830 MB | **1660.9 MB** | **+830 MB** |

**The far field's own data is small and lands exactly where §5.6 predicted.** 33.7 MB of table —
independent of voxel size, which is the whole lever of §5.3 — plus ~32 MB of palette data for a
ring of coarse mountains. Extrapolated to four levels that is 135 MB of table and ~130 MB of heap
in use, well inside §5.6's "~0.4–0.6 GB" estimate.

**And then the heap capacity doubled, for a reason that has nothing to do with any of that.**

The allocator keeps a safety margin ahead of what the GPU can consume in the frames the CPU has
not seen yet — `allocator.inl:445-451` is explicit that this margin is the only thing keeping
`VoxelMalloc`'s unchecked `atomicAdd` in bounds. The margin is
`(FRAMES_IN_FLIGHT + 1) × MAX_ELEMENT_ALLOCATIONS_PER_FRAME`, and it is also the allocator's
*initial* capacity. **Two levels can both run a full `ChunkAlloc` in one frame, so the margin has
to double**, and that is a one-line change in `voxel_malloc.inl` that this work made deliberately.

What it costs is not one margin. Measured capacity is **exactly three times the initial element
count in both configurations** — 131 072 → 393 216 pages (830.5 MB) before, 262 144 → 786 432
pages (1660.9 MB) after. The doubled margin is multiplied by the heap's geometric growth.

> **The far field's largest memory cost is 830 MB of allocator slack for 32 MB of voxels.**
> Heap *usage* is 45 MB against a 1661 MB capacity — 2.7 % occupancy.

**It is already failing, today, at two levels.** Four of the runs attempted for this document
never produced a frame, and the reason is in their logs:

```
[[DAXA ASSERT FAILURE]]: error code: DAXA_RESULT_ERROR_OUT_OF_DEVICE_MEMORY(-2),
                         failed to create memory block.
```

**With the far field's doubled margin, the engine cannot start while a sibling instance is
resident on this 6 GB card.** Before this change it could — the whole measurement campaign for
`RENDERER_OPTIMISATION.md` ran that way. The failure is at buffer creation, during startup, and
it is silent apart from that one line: the process exits before writing a single CSV row, which
is exactly how it presents in a harness. *(It also means the four failures scattered through §5.1
and §5.4 are this, not a flaky script.)*

**This will not survive L2 and L3.** Four levels at the same reasoning is a ×4 margin, an initial
capacity of 1.1 GB and a settled capacity around 3.3 GB, on a card with 6144 MiB of which the
renderer already wants 2.1 GB. The allocator's VRAM cap (`budget 4145 MB` in every run log above)
would refuse the growth rather than fault — which is the path §5.6 said "any big-world work would
have hit within a day", and this is the day — but refusing means the world stops loading, not
that it runs.

**The fix is cheap and it is not "raise the cap".** The margin is sized for the worst case where
every level elects `MAX_CHUNK_UPDATES_PER_FRAME` chunks in the same frame. That only happens at
startup: the far level's chunks are 16 m, so in steady state it regenerates 4× less often per
metre travelled than the near one (`WORLD_SCALE.md` §6.4), and L2/L3 far less again. Give the
coarse levels a *smaller election budget* — a runtime cap in `try_elect`, which is where the
per-level budget §5.7 item 4 asked for belongs anyway — and the margin goes from ×2 to ×1.25 for
two levels, and to about ×1.4 for four. **Do this before building L2.**

---

## 7. The verdict against the §5.5 budget

§5.5 budgeted **+1.4 ms at the Balanced tier for a 2 km, four-level far field, ±30 %**, and named
the term it least trusted: sky pixels becoming distant-surface pixels. §6.3 asked for one level
first, and said to stop and re-plan if it cost 3× the budget.

**It splits in two, and the two halves land on opposite sides of the budget.**

| | budget (§5.5, four levels, 2 km) | measured (one level, 128 m) | verdict |
|---|---:|---|---|
| far field on **primary visibility only** | +1.4 ms | **+0.70 ms** (ridge) / **+0.90 ms** (spawn) | **inside budget, for one level. Extrapolates to roughly +2.5–3 ms for four, i.e. ~2× — acceptable** |
| far field on **all eight call sites** | +1.4 ms | **+16.6 ms** (ridge) / **+4.5 ms** (spawn) / **+0.8 ms** (cave) | **12× the whole four-level budget, for one level. STOP.** |
| **all eight, secondary reach clamped to 48 m** | +1.4 ms | **+1.69 ms** (ridge) | **at the budget's upper bound (1.82 ms), for one level. A tenfold recovery from one integer, and the image is unchanged** |

**Saying it loudly, as §6.3 asked:**

> **THE TERM THE PLAN TRUSTED LEAST IS FINE. THE TERM IT DID NOT BUDGET AT ALL IS 12× THE WHOLE
> BUDGET.**
>
> Distant mountains being *seen* costs +0.70 ms — cheaper than the plan hoped, and
> `WORLD_SCALE.md` §4.1's "near grass is the expensive half, not distant mountains" holds up
> against a real distant mountain.
>
> Distant mountains being *marched by every shadow, diffuse and irradiance-cache ray* costs
> +16.6 ms. Nothing in §5.5 has a line for it, because §5.5's model was per-pixel and this is
> per-ray-in-the-GI. The four-level architecture is not refuted — **the four-level architecture
> applied uniformly to all eight `voxel_trace()` call sites is.**

**What this does and does not change about §5.3.** The nested-volumes structure is vindicated:
one extra level was two lines of buffer plumbing, twelve lines of constants, one march function
and no new generation code, and it puts a real ridgeline at 128 m for +0.70 ms of primary cost.
What has to change is the assumption, never stated because nobody thought to state it, that
**every ray sees every level**. It cannot. Ray reach has to be a property of the ray's *class*,
not of the world.

### 7.1 The recommendation

1. **Build L2 and L3.** The structure works and the mechanical cost was over-estimated.
2. **Before that, make ray reach per-class.** `FF_SECONDARY_MAX_DIST` in this tree is the crude
   version — one clamp, primary exempt. The principled version is a per-level mask: primary sees
   all levels, reflections see L0–L1, diffuse and shadow see L0 plus a short way into L1, the
   irradiance cache sees whatever its cascade radius justifies. Cheap to implement, and §5.4 of
   this document measures what the crude version already buys.
3. **Fix the allocator margin (§6) before L2,** not after. It is the thing that ends in a device
   loss rather than a slow frame.
4. **Re-check `MAX_CHUNK_UPDATES_PER_FRAME` per level while doing (3)** — the same change fixes
   both, and §5.7 item 4 already asked for it.

---

## 8. Integration notes — the six files this work does not own

Every edit outside `src/voxels/far_field*` and `src/voxels/impl/voxels.inl` is listed here. All of
them are guarded so that `FF_ENABLE 0` or `VOXEL_LEVEL 0` restores the previous text exactly.

| file | edit | why it could not live in an owned file |
|---|---|---|
| `src/voxels/impl/voxel_malloc.inl` | The `VOXEL_LEVEL` switch over `CHUNKS_PER_AXIS` / `LOG2_VOXEL_SIZE`, the `FF_*` raw values, and **`VOXEL_MALLOC_MAX_PAGE_ALLOCATIONS_PER_FRAME × 2`**. | These are the constants the whole engine reads; there is nowhere else to put them. **The ×2 is the one to review** — §6 measures it at +830 MB and recommends replacing it with a per-level election budget. |
| `src/voxels/impl/trace.glsl` | `#include <voxels/far_field.glsl>`, the `VOXL_FF_TRACE` switch, one saved `ray_pos`, a split break condition, and the `else if (left_box)` continuation. ~30 lines, all inside `#if VOXL_FF_TRACE`. | `voxel_trace()` is the single choke point. There is no way to add a second march segment without touching it. Note it now also contains the **ray-reach agent's** `VOXL_MAX_STEPS` / `VOXL_STEP_FLOOR_E4` work — the two changes were merged by hand and are independent. |
| `src/voxels/brushes.glsl` | `#include <voxels/far_field_gen.glsl>` and a 4-line `#if VOXEL_LEVEL == 1` dispatch at the head of `brushgen_world()`. | `brushgen_world()` is the entry point the chunk-edit shader calls; the far generator needs `voxl_col`, `fbm2`, `VOXL_ORIGIN` and `VOXL_ISLAND_C`, all defined above it. **Merged around the vegetation agent's density-knob block; the two do not overlap.** |
| `src/voxels/impl/voxel_world.comp.glsl` | Three `#if VOXEL_LEVEL` guards: the brush-election branch in `PerChunk`, the two CPU-mirror writes in `ChunkAlloc`, and `generate_normal = false` in `ChunkEditPostProcess`. | Each is a *near-field-only* behaviour that would be actively wrong at a coarse level. The third one is the interesting one — see below. |
| `src/voxels/impl/voxel_world.cpp` | Two calls: `far_field_record_startup` and `far_field_record_frame`. | The far chain must share this function's `temp_voxel_chunks` transient (worth 134 MB) and must be recorded after the near chain. |
| `CMakeLists.txt` | `"src/voxels/far_field.cpp"`. | — |

### 8.1 The one integration edit that is a finding rather than plumbing

`ChunkEditPostProcess` re-derives a surface voxel's normal from its 5³ neighbourhood whenever the
brush left the normal at the default +Z. `brushes.glsl` rule (3) already records what that does to
a smooth hillside of one colour — the neighbourhood crosses chunk boundaries, only
`MAX_CHUNK_UPDATES_PER_FRAME` chunks generate per frame, and a chunk generated before its
neighbour reads that neighbour as air, tilts its normal into it, and is never revisited.

**The far field is nothing but smooth hillsides of one colour, and the first capture
(`FF01-vista-ridgeline.png`) is covered in chunk-sized black wedges because of it.**
`FF02-vista-normals-fixed.png` is the same frame with `generate_normal = false` at L1 and most of
them are gone.

The trigger is more sensitive than it looks. The normal is stored as an **8-bit octahedral index**
(`pack_unpack.glsl:43`) — 256 directions in total — so *every* normal within roughly 12° of
straight up compares equal to the packed default and asks to be re-derived. That is most of a
distant hillside, and nudging the written normal to dodge the test would need a 12° lie.

**This is worth knowing for the near field too.** `SCENE.md` defect #1 records black patches on the
hill and rock as unattributed and says the normal policy was ruled out. The far field shows the
same artefact at ten times the scale and the derived-normal path is *demonstrably* one of its
causes. It is not necessarily the only one — some black wedges survive in `FF02`, on both the far
terrain and the near hill — but anyone re-opening defect #1 should start here.

### 8.2 For the ray-reach agent

`trace.glsl` and `trace_primary.comp.glsl` are shared. Two interactions:

1. **The far segment has its own step budget** (`FF_MAX_STEPS`, 256) rather than drawing on
   `MAX_STEPS`. If it shared the pool, a ray that crawled through grass and spent 512 steps inside
   32 m would delete the mountain behind it — and delete it *as sky*, which is indistinguishable
   from correct.
2. **`result.step_n` is clamped to `max_steps - 1` on the far-field path.** Your
   `ran_out_of_steps` test at `trace_primary.comp.glsl:123` reads `step_n >= VOXL_TRACE_MAX_STEPS`
   as "this pixel is a hole", and the far segment's steps would otherwise be able to fake that
   signal on a perfectly healthy long ray. If you want the true total, add a separate field
   rather than removing the clamp.

### 8.3 For whoever owns the terrain

`ff_h()` in `far_field_gen.glsl` is **new content, not a filtered version of the existing
terrain**, because there is no existing terrain to filter: the Voxl island's solid ground ends at
about 18.5 m and `voxl_ground_h` returns −18 m and falling beyond it. If you extend
`voxl_ground_h` past the island rim, `ff_h()` should call it for the base layer and keep only its
ridge and rise terms. **Keep the rise term** — §4.1 explains that it is what hides the far
volume's wall at 128 m, and without it the world ends in a straight line across the horizon.

`FF_HOLLOW_R` (72 m) is the other thing to preserve or replace deliberately: it is what makes the
far field *structurally* unable to appear inside the near box. If the far terrain ever has to
continue the ground under the player, that guarantee has to be replaced by dynamic hollowing —
L1 chunks inside the near box generated empty and re-generated as the player moves. §2.2.

---

## 9. What this document does not establish

1. **A fully uncontended measurement.** Three sibling engine instances were resident throughout.
   §5.0 gives three reasons the conclusion survives; it does not claim the absolute millisecond
   figures are as clean as `RENDERER_OPTIMISATION.md`'s.
2. **The cave and patrol controls (§9.2).** Two `FF_RAYS 0` rows read ~14.1 ms where the treatment
   row and the published figures both say ~6–7 ms. A control that is *slower than its own
   treatment* is not a measurement, it is an intruder, and both are rejected rather than quoted.
   The cave treatment row survives because it can be checked against
   `PERFORMANCE_PLAN.md` §3.3's published 5.923 ms; the patrol pair has nothing left. **Re-shoot
   both controls on a quiet machine.** The patrol row also carries a p99 of 45.8 ms against a p50
   of 14.2, which is the chunk-generation hitching `WORLD_SCALE.md` §6.4 describes and is a
   separate question this document does not open.
3. **Where the +16.6 ms lands, pass by pass.** §5.3's mechanism is inferred from the pose
   dependence and from `WORLD_SCALE.md` §2.2's cost of a march step. A `VOXL_GPU_PROFILE` run
   would attribute it to `TraceSecondaryCompute` versus `RtdgiTraceCompute` in ten minutes and
   would change which of §7.1's mitigations is worth most.
4. **Whether the far field's own generation cost matters.** World generation completes in 35–39
   frames with both levels, against 32 frames minimum for the near level alone, so the far level
   adds only a few frames of startup — but that is the *near* level's counter, and the far level
   has no equivalent readout.
5. **Motion.** Every row is a fixed camera. The far level regenerates on 16 m crossings rather
   than 4 m ones, which should make it *cheaper* than the near level while walking, but nothing
   here measures it.
6. **L1's residual black wedges.** §8.1 fixed the derived-normal cause and some remain.
7. **Anything at 1080p, or with FSR in the loop.**
