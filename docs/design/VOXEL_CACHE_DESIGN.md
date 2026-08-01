# The voxel cache design

**Written:** 2026-08-01. **Revised:** 2026-08-01, after adversarial review. **Tree:** `C:\voxl2` at
`dbbd54e`, clean.
**Hardware:** RTX 3050 6 GB Laptop (GA107, 6144 MiB), i7-13650HX, 15.7 GB RAM, Windows 11.

This is the decision document for the cache-optimisation phase. It follows three research
documents written the same day and does not repeat them:

- `docs/design/GIGAVOXELS_NOTES.md` — GigaVoxels as an implementable spec, read out of the paper,
  the 2011 thesis and the shipped GigaSpace source.
- `docs/design/CACHE_CURRENT_STATE.md` — what voxl2's memory model is today, measured.
- `docs/design/CACHE_PRIOR_ART.md` — the seventeen years since, ranked for this case.

Claims are tagged **[PAPER]** (Crassin, Neyret, Lefebvre, Eisemann, *GigaVoxels: Ray-Guided
Streaming for Efficient and Detailed Voxel Rendering*, I3D 2009,
<https://maverick.inria.fr/Publications/2009/CNLE09/CNLE09.pdf>), **[THESIS]** (Crassin, *GigaVoxels:
A Voxel-Based Rendering Pipeline for Efficient Exploration of Large and Detailed Scenes*, Grenoble
2011, <http://maverick.inria.fr/Membres/Cyril.Crassin/thesis/CCrassinThesis_EN_Web.pdf>),
**[SOURCE]** (read out of this tree, file and line given), **[MEASURED]** (measured in this project,
with the document that holds the command), **[DERIVED]** (arithmetic on the above, shown).

**No file in `C:\voxl2\src` was modified for this document, and nothing in
`C:\Users\luigi\projects\Voxl` was touched.**

### What this revision changed, and why

The first draft was reviewed adversarially. **Every source-level defect the review named was checked
against the tree and every one of them is real.** The changes are not cosmetic:

| # | change | driver |
|---|---|---|
| 1 | §1 is re-headed on the **census**, which is measured. The frustum figure is demoted to what its own script calls it — an **occlusion-free upper bound** — and no longer carries `[MEASURED]` | the source tags it `[DERIVED, scratchpad/frustum_chunks.py]` and its docstring says it "ignores occlusion… what the bundle would touch through an **empty box**" |
| 2 | **New §2.3.** The whole verdict rested on one scene, an 80 m island in an empty box. This project has already measured a second scene where the same quantity is **1262.8 MB** | `WORLD_SCALE.md` §5.3, `PERFORMANCE_PLAN.md` §5.1 |
| 3 | **The plan is re-sequenced.** The directory drops from Stage 1 to Stage 5. Stage 1 is now the allocator bound-check | `FAR_FIELD.md` §7.1 item 3 names it a prerequisite; §6 measures the blocker the draft misattributed |
| 4 | §3.4 gains a **fifth residency state**. The draft's demote test was wrong and the engine's own census function already had the bucket for it | `voxel_world.cpp:157-175`, the `header_only` branch |
| 5 | §3.1 **unpacks** `state`, and `update_index` moves to its own buffer. Packed, an existing full assignment would have cleared `HAS_BODY` on every regenerated chunk | `voxel_world.comp.glsl:895` |
| 6 | §3.2's macro call was **wrong arity** and the omitted argument doubles as the pool's initial capacity — the mistake costs 1.08 GB | `allocator.inl:12`, `:318` |
| 7 | §5.4's "a full pool declines to elect" is **not implementable before the bound-check**, so the provocation run moves after it | `allocator.inl:465-475`, `allocator.glsl:37-40` |
| 8 | §4's entry condition watched the **wrong pool** — the body pool is the index; GigaVoxels' LRU manages the payload, which here is `voxel_malloc` | §2.3 |

Four points from the review are **partly rejected**; each has a note in §11 rather than being
silently dropped, so the argument is not relitigated.

---

## 1. The verdict

**Do not build a ray-guided streaming cache, and do not build the directory next either.**

**The measurement that decides the first half is the census, and it is measured.** At
`CHUNKS_PER_AXIS 64` the dense chunk table is **2153.8 MB holding 9.1 MB of information** —
99.7 % of its chunks uniform, 847 of 262 144 holding the entire island — and a pooled equivalent of
the identical content is **9.1 MB on a 6144 MiB card**, 0.15 % occupancy, with nothing to evict
([MEASURED] `SCALE_LIMITS.md` §3.3, printed by `log_table_census()` at
[SOURCE] `voxels/impl/voxel_world.cpp:147-191`). voxl2 has [PAPER] §4.1's **sparsity** problem and
does not, on this scene, have its **residency** problem.

**And note the shape of the trap: the cache is unreachable without the fix that makes it
unnecessary.** You cannot evict entry 1234 of a dense O(1)-indexed array without first making the
index indirect; once the index is indirect the thing you were going to cache costs 9.1 MB.

**The second half is what this revision changed.** The draft ranked that indirection — a directory
plus a body pool — as the next work. It is not, for three reasons that its own sources supply:

1. **It is not what blocks the far field.** The draft claimed the directory turns "four levels =
   135 MB of table" into 131 KB, "which is what unblocks L2 and L3". `FAR_FIELD.md` §6 says the
   opposite in terms: 135 MB of table is *"well inside"* budget, and the thing that
   *"will not survive L2 and L3"* is the **allocator margin** — ~3.3 GB of settled capacity for a
   few hundred MB of voxels. `FAR_FIELD.md` §7.1 item 3: *"Fix the allocator margin (§6) before L2,
   not after."* `PERFORMANCE_PLAN.md` §5.3 sizes four nested levels at **128.4 MB of dense table for
   a 2048 m view radius** — affordable today, with no directory at all.
2. **It buys the wrong axis, and the axis it buys is capped by something else.** The directory buys
   *near-detail radius at 6.25 cm*; four far-field levels buy *visible radius at coarse resolution*.
   Measured: one extra level was *"12 lines of constants, 2 lines of buffer plumbing, one march
   function and no new generation code"* for **+0.70 ms** of primary cost, +1.69 ms with all eight
   `voxel_trace()` call sites and one integer clamp ([MEASURED] `FAR_FIELD.md` §0, §5.1, §7).
   **16× linear visible extent for about thirty lines, against 6× linear detail radius for a rewrite
   of the engine's central data structure.**
3. **The 6× is contingent on the world staying as empty as the island, and the world the project
   intends to ship is not.** §2.3.

**What the LRU verdict now rests on, stated so it can be attacked.** Not on the frustum figure — that
was a supporting observation and it was mistagged. It rests on: the pooled index is **9.1 MB**
(measured); the payload pool that *could* fill is `voxel_malloc`, and the natural key for evicting
from it here is **distance, not recency**, because the engine already has a distance mechanism
(chunk wrapping) and a coarse fallback representation (the far field), whereas recency thrashes on
camera turns and has no fallback but a hole. **The far field *is* voxl2's eviction policy.** That is
not a rejection of GigaVoxels; it is a choice between its two axes, and [PAPER] §4.1's own fallback
for an unresolved node is the coarser level, not a hole.

**The answer in metres, with the caveat the draft omitted.** Today: **32 m** of 6.25 cm detail
shipped, **128 m** at the memory wall, dead at 256 m. Four far-field levels reach **2048 m of
visible extent** for 128.4 MB and ~+3 ms, with no new data structure. The directory would move the
*table* wall past 512 m — but on a densely vegetated world the **voxel heap** binds first, at
roughly 100–150 m ([DERIVED], §2.3), and the directory does not touch the heap. So:

> **Buy visible extent, which is cheap, measured and revertible. Do not buy near-detail radius
> until one census on a dense scene says which pool actually fills.**

---

## 2. The problem, in this engine

### 2.1 The wall that was measured

`voxel_chunks` is a dense array of `VoxelLeafChunk`, `sizeof` = **8216 B**
([SOURCE] `voxels/impl/voxels.inl:33-41`), indexed O(1) by `calc_chunk_index()`, resident whether
the chunk holds rock or air. 99.8 % of the record is two 512-entry per-palette-region arrays
(`u64 page_allocation_infos[512]` at offset 24 and `PaletteHeader palette_headers[512]` at offset
4120), neither of which a chunk of pure air ever touches. There is a second copy of the same size in
host RAM (`CpuVoxelChunk`, 8192 B), which no document before `SCALE_LIMITS.md` §3.1 had counted.

[MEASURED] `SCALE_LIMITS.md` §3.3, §5 — the census and the ceiling:

| CPA | near view radius | table (VRAM) | mirror (host) | chunks with content | pooled equivalent | ratio |
|---:|---:|---:|---:|---:|---:|---:|
| 16 *(shipped)* | 32 m | 33.7 MB | 33.6 MB | 313 (7.6 %) | **2.6 MB** | 12.9× |
| 32 | 64 m | 269.2 MB | 268.4 MB | 856 (2.6 %) | **7.3 MB** | 36.9× |
| 64 | 128 m | 2153.8 MB | 2147.5 MB | 847 (0.3 %) | **9.1 MB** | 237.8× |
| 80 | 160 m | 4206.6 MB | 4194.3 MB | — | — | starts, ~110 ms/frame, driver paging |
| 128 | 256 m | 17230.2 MB | 17179.9 MB | — | — | **dies, `0x80000003`, empty stderr** |

**The content is flat and the table is cubic.** 856 chunks at CPA 32 and 847 at CPA 64 is the same
island — `VOXL_ISLAND_R` is `min(40.0, VOXL_WORLD_HALF - 11.0)`
([SOURCE] `voxels/brushes.glsl:482`) and so caps at 40 m — indexed by a table 8× larger.
`docs/images/scale/cpa32-vista.png` and `cpa64-vista.png` are the same picture for 8× the memory.

### 2.2 Read the axis correctly: this is view radius, not world extent

**The near volume already wraps around the player and always has.** `ENABLE_CHUNK_WRAPPING` is 1
([SOURCE] `application/settings.inl:62`); `player_fix_chunk_offset()` folds the player's position
into `player_unit_offset` every frame ([SOURCE] `application/player.cpp:16-28`);
`calc_chunk_index()` modulates by that offset ([SOURCE] `voxels/impl/voxels.glsl:94-101`); and
`PerChunkCompute` clears `CHUNK_FLAGS_ACCEL_GENERATED` on every chunk in the slab that left the
volume and re-elects it ([SOURCE] `voxels/impl/voxel_world.comp.glsl:57-81`).

So the player can already walk forever. `CHUNKS_PER_AXIS` is not the size of the world; it is
**how far you can see at 6.25 cm**, and it is `CPA × 2` metres. This matters in two places — §5
(walking away already destroys edits, today, with no cache involved) and here, because the honest
headline is not "a 256 m world becomes unbounded" but "a **32 m** detail radius becomes a larger
one, and something other than memory then binds".

### 2.3 Two scenes, and the pool that actually fills — the section the draft did not have

**This is the most important correction in the revision.** Every "nothing to evict" number above
comes from one scene: an 80 m island in an otherwise empty box, whose radius stops growing at
CPA 32 by construction. `SCALE_LIMITS.md` §10 item 3 already flags the gap —
*"what a bigger world would cost with bigger content in it"* — as unmeasured.

**The project has already measured a second scene, and the draft never cited it.**
[MEASURED] `WORLD_SCALE.md` §5.3, repeated in `PERFORMANCE_PLAN.md` §5.1, from the engine's own
`--bench-csv`, settled, demo world (procedural terrain across the whole box rather than an island),
camera at (-182.99, -109.98, -42):

| CPA | box | **voxel heap in use** | heap capacity |
|---:|---|---:|---:|
| 8 | 32 m | 1.2 MB | 553.7 MB |
| 16 | 64 m | 107.5 MB | 830.5 MB |
| 32 | 128 m | **1262.8 MB** | 1660.9 MB |

`PERFORMANCE_PLAN.md:372` draws the conclusion the draft should have engaged with:
*"1263 MB of heap for a 128 m world is the practical wall, and it arrives before the table does."*

**And the unit rates say the same thing from a different direction.** `DENSITY_LIMITS.md` §2 fits
**229 KB of voxel heap per conifer** (R² 0.984 over 1–74 stems), falling to **141 KB/stem** averaged
to 242 stems as canopies start sharing palette regions, plus **2.9 KB per fern**. Its own worked
example: a 100 m × 100 m forest at a realistic 800 stems/ha is **193 MB of heap**. Scaled to the
400 m × 400 m footprint of a 200 m detail radius [DERIVED]:

```
  16 ha x 800 stems/ha = 12 800 stems
     at 141 KB/stem (the large-N rate)  =  1.80 GB
     at 229 KB/stem (the small-N fit)   =  2.93 GB
  0.5 ferns/m^2 over 160 000 m^2 = 80 000 ferns x 2.9 KB = 0.23 GB
                                              -----------------------
                                       total  2.0 - 3.2 GB of heap IN USE
```

against a fixed renderer cost of **1785 MiB** ([MEASURED] `SCALE_LIMITS.md` §5's fit,
`VRAM = 1785 MiB + 8216 × CPA³`, within 1.3 % at four points) on a 6144 MiB card — and before the
allocator's measured **3× capacity-over-usage** ([MEASURED] `FAR_FIELD.md` §6: capacity is
"exactly three times the initial element count in both configurations"). **It does not fit, by a
factor of two to five, and the directory does not touch any of it.**

**So there are two pools and the draft conflated them:**

| | what it is | GigaVoxels analogue | island, CPA 64 | dense world |
|---|---|---|---:|---|
| **chunk table** | the *index*: 8216 B/chunk, dense, resident | node pool / page table | 2153.8 MB holding 9.1 MB | same — index cost is content-independent |
| **`voxel_malloc` heap** | the *payload*: 2112 B pages of palette blobs | **brick pool** | 52.0 MB in use, 1660.9 MB capacity | **1262.8 MB in use at CPA 32** |

The directory + body pool fixes the first column. **[PAPER] §6.1's LRU manages the second.** The
draft's §4 entry condition — "body-pool occupancy above 60 % of 1.5 GB" — watches the index and is
close to unfalsifiable; the pool that can fill is the heap, and on one measured scene it already
holds 1.26 GB at a 128 m box.

**Three honest qualifications, because this is now load-bearing:**

- The 1262.8 MB figure is *measured* but on a scene whose generator settings are not recorded in
  `WORLD_SCALE.md` and which may not be reproducible verbatim today. It is a strong signal, not a
  substitute for the run in §10.
- The 2.0–3.2 GB extrapolation is [DERIVED] from a fit over ≤242 stems on one island. Canopy palette
  sharing was still improving the rate at the top of that range, so the true number is plausibly
  lower. It is not plausibly *four times* lower.
- **Frame time does not scale with area the way heap does.** `DENSITY_LIMITS.md` §1 measures the
  vegetation cost as dominated by what is *near the camera*, so the +19.9 ms per hectare in its
  worked example does not multiply by 16. Heap does multiply, because the heap is resident
  regardless of distance. **That asymmetry is the entire argument for the far field:** the way to
  afford content at 200 m is to not store it at 6.25 cm.

### 2.4 What the world becomes, with the caveat attached

[DERIVED], all walls at once. Body pool sized from the *island* census with terrain treated as a
surface (content ∝ area, so bodies scale as CPA²) — **which §2.3 has just shown is the optimistic
case, not the expected one**:

| CPA | near radius | dense table | directory + pool | startup gen | movement budget at 5 m/s |
|---:|---:|---:|---:|---:|---:|
| 16 | 32 m | 33.7 MB | **2.6 MB** | 33 frames, 0.65 s | 2 of 80 frames |
| 32 | 64 m | 269.2 MB | **7.3 MB** | 259 frames, 5.10 s | 8 of 80 |
| 64 | 128 m | 2153.8 MB | **9.1 MB** | 2672 frames, ~41 s | 32 of 80 |
| 128 | 256 m | 17230.2 MB *(dies)* | **~44.8 MB** | ~16 384 frames, ~5.5 min | **128 of 80 — cannot keep up** |
| 256 | 512 m | 137 842 MB | **~246 MB** | ~131 072 frames, ~44 min | 512 of 80 |

- Startup generation is `CPA³ / MAX_CHUNK_UPDATES_PER_FRAME` **frames**, a floor no GPU speed can
  move ([MEASURED] `SCALE_LIMITS.md` §4: 33 / 259 / 2672 frames at CPA 16 / 32 / 64, and 19.7 ms per
  generation frame in both of the two uncontended rows).
- Movement: crossing one 4 m chunk boundary invalidates a `CPA²` slab, needing `CPA²/128` frames of
  budget; a 5 m/s walk gives `4 / (5 × 0.010)` = **80 frames** to spend. Sustainable while
  `CPA² / 128 ≤ 80`, i.e. **CPA ≤ 101**. At 20 m/s it is CPA ≤ 50. [DERIVED, consistent with
  `PROFILE.md` §3's measured chunk-generation cost of 0.053 ms standing and 0.425 ms moving at
  CPA 16.]

> **So there are four walls, not two, and the directory moves only one of them.** Table: CPA 64 →
> past CPA 256. **Heap: unmoved, and §2.3 puts it at CPA 50–75 on a dense scene.** Startup
> generation: CPA ≈ 96–128. Movement: CPA ≈ 101. On the island the answer is a ~200 m detail radius;
> on a world with vegetation in it the answer is **100–150 m and the binding pool is the one the
> directory does not touch.**

**A third wall the draft omitted entirely, and it is an argument *for* the directory.**
`PerChunkCompute` is `O(CPA³)` **every frame forever**, not only at startup: `CHUNKS_DISPATCH_SIZE³`
= `(CPA/8)³` workgroups of 8×8×8 ([SOURCE] `voxel_world.comp.glsl:39`,
`voxel_world.cpp:556-557`), one thread per chunk, each reading `flags` at `:57` and writing
`update_index` at `:202` — unconditionally, at the end of `main()`, into a dense array at 8216 B
stride. `flags` is at offset 0 and `update_index` at offset 4, so **the write dirties exactly the
cache line the marcher reads.** At CPA 32 that is 32 768 lines ≈ 2.1 MB of dirty L2 per frame; at
CPA 64, 16.8 MB. `PROFILE.md` §3 measures chunk generation at 0.053 ms standing still at CPA 16;
scaled ×64 that is ≈3.4 ms of a 7.1 ms Balanced frame at CPA 64 with nothing happening. **This is
unmeasured and it is the cheapest probe in this document** (§7 Stage 2).

### 2.5 Entries against bytes — retagged, and demoted from the headline

A visit to a chunk reads `flags` (and `uniformity_bits`, same line) at offset 0 and one
`PaletteHeader` at offset 4120 — **two cache lines, 128 B, out of an 8216 B record**
([SOURCE] `voxels/impl/voxels.inl:33-41`). Against the distinct chunks a primary ray bundle would
reach **through an empty box**:

| CPA | distinct chunks, empty box | % of *entries* | bytes live (× 128 B) | dense table | % of *bytes* |
|---:|---:|---:|---:|---:|---:|
| 16 | 876 | 21.4 % | 112.1 KB | 33.65 MB | **0.33 %** |
| 32 | 5 964 | 18.2 % | 763.4 KB | 269.25 MB | **0.28 %** |
| 64 | 43 740 | 16.7 % | 5.60 MB | 2153.78 MB | **0.26 %** |

> **[DERIVED, `scratchpad/frustum_chunks.py`], not [MEASURED], and the draft got this wrong.** The
> script's own docstring: *"This is an UPPER bound on primary visibility: it ignores occlusion, so
> it is what the bundle would touch through an empty box."* `CACHE_CURRENT_STATE.md:232` tags it
> correctly; the draft laundered the tag and wrote "actually reach". **The convergence on the
> frustum's solid angle (2.02 sr / 4π = 16.0 %) is a structural consequence of assuming an empty
> box** — a ray with nothing to stop it runs to the boundary, so the swept volume *is* the frustum.
> It is arithmetic about a cone, not evidence about a world.
>
> The engine's own measured heatmap says the opposite for a real scene: at the vista **97.33 % of
> pixels hit, hit p50 = 1.6 steps**; at the cave, 100 % hit, p50 = 5.6
> ([MEASURED] `CACHE_CURRENT_STATE.md` §2.2). Rays here mostly stop almost immediately, so the
> distinct set under occlusion is materially below 876 / 5964 / 43740 and is **unmeasured**. The
> probe is specified at `CACHE_CURRENT_STATE.md` §6 and is ~40 lines.

**What survives, and it is enough.** The *byte* column is arithmetic on a struct layout and holds
regardless of occlusion: a visit reads 128 B of an 8216 B record, so **99.7 % of the resident table
is never fetched, and the fraction widens with world size** (0.33 → 0.26 %). That is a *layout*
defect, fixed by changing where the bytes live, and it needs no residency policy at all.

**Which is why the dense table costs nothing today.** Take the worst case — *every* chunk in the
volume touched — and the live footprint at CPA 16 is still `4096 × 2 × 64 B = 524 KB` [DERIVED],
several times over inside this GPU's L2 (GA107, ~2 MB; **specification, not measured**). At CPA 64
the same worst case is 33.5 MB, past L2, and every DDA step becomes a DRAM round trip. **The dense
table is not slow because it is big; it becomes slow at exactly the size where its touched slices
stop fitting in L2 — and §2.4's third wall says `PerChunkCompute` is evicting them once a frame
anyway.**

---

## 3. The design — corrected, and now a hypothesis rather than a plan

**Read §7 first.** This section describes the two-tier structure the draft proposed. It is still the
right *shape* if the indirection is ever built, and six defects in it have been fixed. It is no
longer the next work.

Two tiers, no eviction, and one strictly-safe piece of the feedback loop. This is [PAPER] §4.1's
node-pool / brick-pool split with the constant-value node kept, and [PAPER] §6.1's LRU deliberately
omitted. Note that the shipped GigaSpace library **dropped** the constant-value node — the
`GsNode.h` TODO says so ([SOURCE] read in `GIGAVOXELS_NOTES.md` §1.2) — and it is the single feature
the entire 237.8× depends on here. Keep it.

### 3.1 The directory — 8 bytes, dense, indexed exactly as today

```c
// voxels/impl/voxels.inl — replaces VoxelLeafChunk in the dense table.
// 8 bytes. Eight neighbouring chunks share one 64-byte cache line ALONG X ONLY (see below).
struct VoxelChunkDir {
    daxa_u32 state;    // bits 0..7 : CHUNK_FLAGS_* exactly as today (ACCEL_GENERATED = 1<<0)
                       // bit  8    : CHUNK_STATE_HAS_BODY
                       // bits 9..31: reserved, MUST read back unchanged
    daxa_u32 payload;  // HAS_BODY ? body index into the body pool
                       //          : the PackedVoxel every voxel in this chunk holds
};
DAXA_DECL_BUFFER_PTR(VoxelChunkDir)
```

**Two corrections from the draft, and the first one is a bug that would have shipped.**

**(a) `update_index` does NOT live here. It gets its own buffer.** The draft packed it into
`state` bits 16..31. Three passes write that word today, and one of them does a **full assignment**:

```glsl
// voxel_world.comp.glsl:895, in ChunkOpt x8up, gl_LocalInvocationIndex == 0
deref(voxel_chunk_ptr).flags = CHUNK_FLAGS_ACCEL_GENERATED;
```

Packed, that store clears `HAS_BODY` on **every regenerated chunk**. The marcher then reads a body
*index* as an inline `PackedVoxel` — garbage voxels — and the body leaks. Giving `update_index` its
own `daxa_u32[CPA³]` buffer removes the hazard, and it does three other things worth having:

1. It removes §2.4's third wall: the per-frame `O(CPA³)` write stops dirtying the marcher's cache
   line and moves to a 4 B-stride array that is 1.05 MB at CPA 64 instead of 2.15 GB at 8216 B
   stride.
2. It makes the directory **read-mostly** — `state` changes only on generation and invalidation
   events, ≤128 chunks plus one slab per frame — which is precisely the access pattern the L2
   argument in §2.5 needs and does not currently have.
3. It is **independently testable in ~10 lines and cannot change a pixel**, which makes it the
   repaired probe (§7 Stage 2).

**(b) Every writer of `state` must preserve the bits it does not own.** With the packing gone this
is cheap but it is not optional, because `HAS_BODY` and `ACCEL_GENERATED` now share a word that two
different passes touch:

| site | today | must become |
|---|---|---|
| `voxel_world.comp.glsl:80` (wrap invalidation) | `flags &= ~CHUNK_FLAGS_ACCEL_GENERATED` | `atomicAnd(dir.state, ~CHUNK_FLAGS_ACCEL_GENERATED)` |
| `voxel_world.comp.glsl:895` (ChunkOpt finish) | `flags = CHUNK_FLAGS_ACCEL_GENERATED` | `atomicOr(dir.state, CHUNK_FLAGS_ACCEL_GENERATED)` |
| `PerChunkCompute` demote sweep (§3.4, new) | — | `atomicAnd(dir.state, ~CHUNK_STATE_HAS_BODY)` |

The task graph serialises these passes, so the atomics are not guarding a race — they are guarding
a read-modify-write that a plain store would silently widen. One atomic per generated chunk,
≤128/frame. Free.

Buffer sizes: directory `8 B × CPA³` = **32.8 KB / 262 KB / 2.10 MB / 16.8 MB** at CPA
16 / 32 / 64 / 128; `update_index` `4 B × CPA³` = half that.

**One overstatement corrected.** The draft said "eight neighbouring chunks share one 64-byte cache
line". True **along x only**: `calc_chunk_index` is `x + y·N + z·N²`
([SOURCE] `voxels/impl/voxels.glsl:99`), so a z-major ray shares nothing between consecutive steps.
The L2-residency argument survives — 2.10 MB at CPA 64 is the whole structure, not a line — but the
per-line argument is best-case, not typical. A Morton or tiled index would fix it and is out of
scope.

### 3.2 The body — 8272 bytes, pooled, one per non-uniform chunk

```c
// Allocated only for chunks with content: 313 of 4096 at CPA 16, 847 of 262144 at CPA 64.
struct VoxelChunkBody {
    daxa_u32 uniformity_bits[3];                                     //   12 B  @    0
    daxa_u32 region_nonuniform_bits[16];                             //   64 B  @   12  (see 3.4)
    daxa_u32 value_and;                                              //    4 B  @   76  (see 3.4)
    daxa_u32 value_or;                                               //    4 B  @   80  (see 3.4)
    daxa_u32 _pad[3];                                                //   12 B  @   84
    VoxelMalloc_ChunkLocalPageSubAllocatorState sub_allocator_state; // 4096 B  @   96  (u64[512])
    PaletteHeader palette_headers[PALETTES_PER_CHUNK];               // 4096 B  @ 4192
};                                                                   // 8288 B total
DAXA_DECL_BUFFER_PTR(VoxelChunkBody)

// SIX arguments. The fifth is MAX_ELEMENT_ALLOCATIONS_PER_FRAME and it is NOT the pool size.
DECL_SIMPLE_ALLOCATOR(VoxelChunkBodyAllocator, VoxelChunkBody, 1, daxa_u32,
                      (MAX_CHUNK_UPDATES_PER_FRAME), (VOXL_MAX_CHUNK_BODIES))
```

**The arity was wrong in the draft and the mistake costs a gigabyte.** `DECL_SIMPLE_ALLOCATOR` takes
six arguments ([SOURCE] `utilities/allocator.inl:12`); the draft passed five, matching the
**commented-out dead form** at [SOURCE] `voxels/impl/voxels.inl:44-45`. The omitted argument is
`MAX_ELEMENT_ALLOCATIONS_PER_FRAME`, and `allocator.inl:318` uses it as the pool's **initial
capacity** — `element_count = (FRAMES_IN_FLIGHT + 1) × MAX_ELEMENT_ALLOCATIONS_PER_FRAME` — while
`:284-285` uses it as `PER_FRAME_HEADROOM`. Passing `VOXL_MAX_CHUNK_BODIES = 65536` there gives an
initial capacity of `2 × 65536 × 8288 B` = **1.09 GB**, settling at the measured 3× to **3.26 GB**
[DERIVED] — recreating in the body pool the exact defect §7 Stage 1 exists to remove. The correct
value is `MAX_CHUNK_UPDATES_PER_FRAME` = 128, because at most one body is allocated per elected
chunk per frame: initial capacity `2 × 128 × 8288` = **2.12 MB**.

`_pad` is load-bearing: `sub_allocator_state` is `u64[512]` and must land 8-aligned in both C++ and
daxa's scalar buffer layout. The same reasoning is written out at
[SOURCE] `voxels/impl/voxels.inl:73-97` and it is the sort of thing that fails as a silently wrong
pointer rather than a compile error.

`DECL_SIMPLE_ALLOCATOR` is the macro `VoxelMallocPageAllocator` is already built from
([SOURCE] `voxels/impl/voxel_malloc.inl:182`). It gives, already written and already debugged: a
GPU-resident growable pool, an available-element stack, a released-element stack, `malloc`/`free`
callable from a compute shader, a per-frame release→available move, a VRAM budget cap, a
`growth_refusals` counter and an overlay readout. **The allocator this design needs exists; only its
element type is new — and its bound-check does not exist, which is §7 Stage 1.**

**`VOXL_MAX_CHUNK_BODIES`: do not pick a number yet.** The draft's 65 536 is 77× the island's
measured 847 and looks generous, but the review is right that it is mid-range for a dense CPA 128
box (128² surface columns × 3–8 vertical chunks ≈ 49k–131k). §2.3 says CPA 128 with dense content is
unreachable for an unrelated reason, so the honest position is that **this constant must be sized
from a census on the scene the project intends to ship** (§10), not from a box volume. Until then
use 65 536 and treat any growth refusal as a measurement, not a failure.

**Pooled totals on the island** [DERIVED, reproducing `SCALE_LIMITS.md` §3.3]:

| CPA | directory + update_index | bodies (8288 B) | **total** | vs dense |
|---:|---:|---:|---:|---:|
| 16 | 0.049 MB | 313 × 8288 = 2.59 MB | **2.64 MB** | 12.8× |
| 32 | 0.393 MB | 856 × 8288 = 7.09 MB | **7.49 MB** | 35.9× |
| 64 | 3.15 MB | 847 × 8288 = 7.02 MB | **10.17 MB** | 212× |

### 3.3 The march

```glsl
// voxels/impl/voxels.glsl — the outer sample_lod(), one call per DDA step.
uint sample_lod(allocator, bodies, dir_ptr, chunk_i, inchunk_voxel_i, out PackedVoxel voxel_data) {
    VoxelChunkDir d = deref(dir_ptr);                 // ONE 8-byte load.

    if ((d.state & CHUNK_FLAGS_ACCEL_GENERATED) == 0)
        return 7;                                     // ungenerated: step 64 voxels = 4 m

    if ((d.state & CHUNK_STATE_HAS_BODY) == 0) {      // the constant-value node, [PAPER] §4.1
        voxel_data = PackedVoxel(d.payload);
        return (unpack_voxel(voxel_data).material_type != 0) ? 0 : 7;
    }

    daxa_BufferPtr(VoxelChunkBody) body = advance(bodies, d.payload);
    /* ... today's code verbatim, with voxel_chunk_ptr replaced by body ... */
}
```

The empty-space case — **92.4 % of chunks at CPA 16, 99.7 % at CPA 64** — becomes one 8-byte load
from a 2.10 MB structure instead of two loads 4 KB apart from a 2153.8 MB one. **A ray that misses
is 2.7× more expensive to march than one that hits (brief, measured fact #2), so the case that gets
cheaper is the case that dominates.** That is the performance claim of this whole design and **it
has never been built or measured.**

Note what does *not* change: the DDA arithmetic, `cell_size = (1 << (lod-1)) × VOXEL_SIZE`, the
palette decoder, the brushes, and `LOG2_VOXEL_SIZE`.

### 3.4 Residency states — there are **five**, not four, and none of them is "evicted"

**The draft's demote test was wrong, and the engine's own census function already knew why.** The
draft demoted a chunk when all 16 words of `region_nonuniform_bits` were zero and wrote "the inline
value" into `payload`. *No region is non-uniform* ≠ *the chunk has one value*: 512 regions can each
hold a single value and those values can differ. `log_table_census()` has a dedicated third bucket
for exactly this, and charges it 4096 B in the pooled formula
([SOURCE] `voxels/impl/voxel_world.cpp:157-175`):

```cpp
if (r.variant_n > 1) { ++paletted_regions; any_paletted = true; }
else if (r.blob_ptr != first) { all_same = false; }
...
if (any_paletted) ++paletted; else if (all_same) ++uniform; else ++header_only;
```

It measures **0 on this island**, which is why nobody caught it — but any surface aligned to an
8-voxel region plane (a flat sea level, a flat cave floor, a flat authored world) makes it non-zero,
and the draft's demote would then silently repaint a whole 4 m chunk one colour.

**The fix, two extra words and two atomics per region workgroup.** `value_and` initialised to
`0xFFFFFFFF` and `value_or` to `0` by ChunkOpt x8up's existing single-thread block
([SOURCE] `voxel_world.comp.glsl:894-896`, which runs before `ChunkAlloc` in the task graph). Each
of `ChunkAlloc`'s 512 region workgroups, on the uniform path, does
`atomicAnd(body.value_and, my_value)` and `atomicOr(body.value_or, my_value)`. **Order-free, exact,
and no cross-workgroup reduction is required** — which matters, because `ChunkAlloc` dispatches
**one workgroup per palette region, 512 per chunk** ([SOURCE] `voxel_world.comp.glsl:958-975`) and
region 0 cannot see region 511's vote. The demote condition becomes
`all 16 bitfield words zero AND value_and == value_or`, read next frame by `PerChunkCompute` off one
cache line.

| state | encoding | marcher does | island, CPA 64 |
|---|---|---|---:|
| **VOID** | `state == 0` | returns 7, steps 4 m | varies (all of it at startup) |
| **UNIFORM** | `ACCEL_GENERATED`, `!HAS_BODY` | one 8-byte load, answers 0 or 7 | **99.7 %** |
| **HEADER-ONLY** | `ACCEL_GENERATED`, `HAS_BODY`, no region paletted | directory + body's `palette_headers`; never touches the heap | **0 % here, non-zero on flat worlds** |
| **BODIED** | `ACCEL_GENERATED`, `HAS_BODY`, ≥1 region paletted | directory + body + palette blob | 0.3 % |
| **UPDATING** | any of the above, `update_index != 0` | neighbour reads `temp_voxel_chunks[update_index-1]` | ≤128 chunks, one frame |

HEADER-ONLY and BODIED share an encoding and differ only in whether the heap is touched; the
distinction exists so the census, the sizing arithmetic and the demote test agree with each other.
A future refinement could give HEADER-ONLY a 4192 B body type and save half; it is not worth a
second allocator instantiation until the census says the bucket is non-empty.

Transitions, and which pass owns each:

- **VOID → UNIFORM/BODIED.** `PerChunkCompute::try_elect()` admits the chunk and, **if and only if
  `!HAS_BODY`**, allocates a body. *Allocating at election is deliberate:* election is one thread
  per chunk with no race and is already the single place where work is admitted or silently dropped
  ([SOURCE] `voxel_world.comp.glsl:24-36`). Cost: up to 128 bodies (1.06 MB) held transiently for
  chunks that turn out uniform.
- **BODIED → UNIFORM.** The only genuinely new transition, and the demote test above. `ChunkAlloc`
  already frees a palette region's pages when it becomes uniform
  ([SOURCE] `voxel_world.comp.glsl:1036-1046`); the sweep adds the body free.
- **UNIFORM → BODIED.** An edit that adds content. Body allocated at election, as above.
- **any → VOID.** Wrap invalidation clears `ACCEL_GENERATED`
  ([SOURCE] `voxel_world.comp.glsl:78-81`). **It must NOT free the body** — the same pass re-elects
  the chunk **in the same dispatch** and `ChunkAlloc` needs the old `palette_headers` to decide
  realloc-vs-malloc per region.

> **The `!HAS_BODY` guard on election is a correction, not a detail.** The draft said invalidation
> "must NOT free the body" *and* that `try_elect()` "admits the chunk **and allocates its body**",
> with no conditional between them. At `:78-81` invalidation re-elects in the same dispatch, so
> those two sentences together leak a body per invalidated chunk. Crossing one 4 m boundary
> invalidates a `CPA²` slab — 4096 chunks at CPA 64 — **on the hottest path in the engine.**

### 3.5 The one piece of the feedback loop worth building — and it is not about memory

[THESIS] §7.3.3's request buffer is genuinely elegant and genuinely cheap, and there is a reason to
want it here that has nothing to do with residency: **generation is `CPA³/128` frames and every
chunk in the box is elected at startup whether or not a ray will ever reach it**
([MEASURED] `SCALE_LIMITS.md` §4).

```glsl
// in sample_lod, only under TracePrimaryComputeShader / TraceSecondaryComputeShader:
if ((d.state & CHUNK_FLAGS_ACCEL_GENERATED) == 0)
    deref(advance(requests, chunk_index)) = frame_index;   // plain store. see the three notes.
```

Three properties, all from [THESIS] §7.3.3, all worth not re-deriving:

1. **Deduplication is structural** — one slot per chunk, laid out one-to-one with the directory, so
   a thousand rays wanting the same chunk write the same slot.
2. **No atomics** — every writer stores the identical value, so no ordering is needed. The thesis
   says so explicitly. Do not "improve" this into an `atomicAdd` counter; [THESIS] §7.3.5 says the
   counter variant needs atomics *and* a per-frame clear and is "not used uppermost".
3. **No clear pass** — the slot holds a **timestamp**, not a boolean. Requested-this-frame is
   `slot == frame_index`.

Buffer: `4 B × CPA³` = 16 KB / 128 KB / 1.05 MB at CPA 16 / 32 / 64.

**No stream compaction.** [THESIS] §7.3.5 compacts because the 2009 design read the request list
back to the CPU and §7.3.8 calls compaction "one of the most costly operations". voxl2 does not read
back: `PerChunkCompute` already runs one thread per chunk and can read its own request slot in the
same pass that reads its directory entry.

**Two restrictions, and both are load-bearing.**

- **Primary and shadow rays only.** `FAR_FIELD.md` §5.4 measured that ray *class* dominates: GI and
  irradiance-cache rays leave the frustum in every direction from arbitrary points
  ([SOURCE] eight `voxel_trace()` call sites, `ircache_trace_common.inc.glsl:17`,
  `diffuse_trace_common.inc.glsl:35`, `reflection_trace_common.inc.glsl:54`, `rt.glsl:95`). Let them
  emit requests and the request set is the whole sphere. Gate at compile time; it costs nothing.
- **The request signal is a *priority function over work that would happen anyway*, never a gate on
  whether work happens.** `PerChunkCompute` elects requested chunks first and then continues to
  elect any ungenerated chunk with the remaining budget. Worst case is byte-identical to today; best
  case is a large cut in time-to-playable. **This is the property that makes it impossible for the
  feedback loop to put a hole in the image.**

> **The review's sharpest point about this section, accepted with a boundary.** Priority-not-gate
> guarantees the `O(CPA³)` sweep never goes away, and §2.4's third wall says that sweep is
> ≈3.4 ms/frame at CPA 64 doing nothing. So the rule is right for *image correctness* and wrong as a
> permanent architecture. The resolution is to separate the two things the sweep does: the
> **election scan** (which chunk needs work) can become request-driven and sparse; the
> **`update_index` write** (§3.1a) should not be in the dense array at all. Neither requires the
> request buffer to become a gate. Ray-guided *generation* as a replacement for the exhaustive scan
> — rather than a reordering of it — is the piece of [PAPER]'s actual named contribution
> (*"the rays themselves will indicate through their traversal which precision is needed"*, §6.1
> p.5) that no document in this phase evaluated, and it is listed in §9.

---

## 4. The LRU decision, argued — and relocated to the right pool

The brief describes move-to-front: a 1-D pool, any rendered voxel moved to the **front**, running
entirely in compute shaders, stable under a **1 MB** pool, addressing **64 GB**, with the swapping
clustering co-accessed data.

**Four of those five claims do not survive the sources**, and `GIGAVOXELS_NOTES.md` §3 and
`CACHE_PRIOR_ART.md` §2 have the full case:

| claim | verdict |
|---|---|
| "runs in compute shaders on the GPU" | **False of [PAPER].** §2 p.3: *"the LRU ordering on the CPU"*; §6.1 p.5: *"timestamps are maintained in host memory"*. True of [THESIS] §7.1.2, a different system |
| "any **voxel** moved to the FRONT" | **Wrong granularity and wrong verb.** Per *page*, and nothing moves: [THESIS] §7.3.4 says sorting *"would be prohibitive"* and uses **two order-maintaining stream compactions** over a list of *addresses*. The payload never relocates |
| "stable under a 1 MB pool" | **Unattributed.** [PAPER] §7 renders with a 4 MB node pool and a **430 MB** brick pool on a 512 MB 8800 GTS. The 1 MB is the x-axis end of [THESIS] Fig. 7.25, a microbenchmark of LRU management *cost* |
| "addresses 64 GB" | **Real but misquoted** — an address-space bound of a 30-bit pointer ([THESIS] §5.2), not a demonstrated working set. The string "GB" does not occur in the 8-page paper |
| "clusters co-accessed data" | **Not claimed in either source, and mechanically impossible as described.** Permuting an address list cannot move bricks in the brick pool |

**Adopted: none of it.** But the reason has moved, and this is the substantive change.

**The draft's reason was occupancy, and it watched the wrong pool.** It argued a 1.5 GB *body* pool
holds 181 400 bodies against 847 measured, a 1:675 surplus, and set the entry condition at "body-pool
occupancy above 60 %". §2.3 shows the body pool is the *index*; [PAPER] §6.1's LRU manages the
*brick* pool, whose analogue here is `voxel_malloc`, **already at 1660.9 MB of capacity holding
52.0 MB on a bare island** ([MEASURED] `CACHE_CURRENT_STATE.md` §1.2) and **measured at 1262.8 MB
in use on the demo world at CPA 32** (§2.3). The stated entry condition was close to unfalsifiable.

**The corrected entry condition, on the pool that can actually fill:** `voxel_malloc` heap **in
use** sustained above **1.5 GB** in a scene the project intends to ship, *with* the far field
already carrying everything past its radius. Today, island vista: **52.0 MB**. Demo world, CPA 32:
**1262.8 MB — 84 % of that threshold, on a scene this project has already run.** That is not a
comfortable margin and §10 exists to close it.

**And now the four reasons an LRU is still the wrong answer, one of them new.**

1. **The eviction key should be distance, not recency, and voxl2 already has one.** Recency is the
   right key when the cost of a miss is a PCIe round trip to disk and the geometry of access is
   unpredictable. Here the "backing store" is a **procedural generator running on the same GPU at
   0.073 ms per chunk** ([DERIVED]: 19.70 ms per full generation frame against a 10.36 ms idle p50
   at the same pose = 9.34 ms for 128 chunks; `SCALE_LIMITS.md` §4, `CACHE_CURRENT_STATE.md` §7),
   and access is dominated by a smoothly moving camera. A distance key is **stable** — it does not
   thrash when the player turns around — and it has a **graceful fallback**, the coarser level,
   rather than a hole. **Chunk wrapping is already that policy for the near volume and the far field
   is already that fallback.** [PAPER] §4.1's own answer to an unresolved node is the coarser level
   too. Building an LRU beside a working distance policy is adding a second, worse eviction
   mechanism.
2. **Eviction changes the edit contract from "distance" to "attention", which is strictly worse for
   a sandbox.** §5.3.
3. **Recycling is O(directory), not O(evicted).** [THESIS] §7.3.7: flag the recycled pages, then
   test *every* page-table entry. At CPA 64 that is 262 144 entries swept every frame to reclaim a
   handful — and §2.4's third wall says this engine already has one such sweep it cannot afford.
4. **GI is 53.6 % of the frame and Crassin's own GI chapter does not use the cache.** [THESIS]
   §9.4.1 uses a separate, fully resident, re-voxelized-per-frame octree, and §10 says why: it
   *"is not compatible with our on-demand loading scheme."* The one GigaVoxels application that
   resembles voxl2's workload is the one that abandoned the cache.

**Working set > pool has no published answer, and this project has a photograph of it.**
[THESIS] §7.5.2 says only that *"quality reduction strategies have to be employed"*; the shipped
GigaSpace code prints a warning, truncates, and renders coarse
([SOURCE] `GsCacheManager.inl::genericWrite`, read in `GIGAVOXELS_NOTES.md` §2). Here that reads as
terrain visibly failing to resolve — `docs/images/scale/cap2-refused-exhausted.png`: grey slabs
through the hillside, exit code 0, nothing logged.

**Kept from the LRU literature, because they are free:** the constant-value node ([PAPER] §4.1 —
this is the whole 237.8×), the timestamp-not-boolean request slot ([THESIS] §7.3.3), and the
producer's right to *decline* — to write a constant instead of consuming a pool slot
([THESIS] §7.3.6), which is exactly §3.4's demote sweep.

---

## 5. Editing

Non-negotiable, and the answer has a correction in it that changes the question. **This section
survived review essentially intact and §5.1 is the most useful paragraph in the phase.**

### 5.1 voxl2 already destroys edits, today, with no cache involved

Walk 32 m and the chunk you dug leaves the wrapped near volume; `PerChunkCompute` clears its
`ACCEL_GENERATED` and re-elects it in the same dispatch; `ChunkEdit` regenerates all 64³ voxels from
the world generator with `BRUSH_FLAGS_WORLD_BRUSH` and no brush
([SOURCE] `voxel_world.comp.glsl:57-81`, and §2.2). There is no persistence anywhere in the tree.
**The "an evicted edit is a destroyed edit" property that `GIGAVOXELS_NOTES.md` §7.4 correctly
identifies as GigaVoxels' dealbreaker is already this engine's behaviour** — it is an
authored-delta-store problem, not a cache problem, and it is unchanged by everything in this
document.

That reframes the question from "does the cache break editing" to **"does the cache make the
edit-loss contract worse"**, and the two halves answer differently.

### 5.2 The directory + pool: unchanged semantics, by construction

The design does not change *when* a chunk is regenerated — election, invalidation and the brush's
swept-capsule AABB are untouched. It changes *where the chunk's bytes live*.

- **A brush on a UNIFORM chunk** promotes it: `try_elect()` allocates a body (if `!HAS_BODY`),
  `ChunkAlloc` mallocs the pages. This is the only new failure point in the edit path — see 5.4.
- **A brush that empties a chunk** demotes it one frame later via §3.4's corrected sweep.
- **A brush on a paged-out chunk cannot happen.** There is no paged-out state (§3.4). Every chunk
  slot in the volume is VOID, UNIFORM, HEADER-ONLY or BODIED, and a brush on a VOID one elects it
  exactly as today. This is not a dodge; it is the reason to prefer this shape over a cache.
- **The CPU mirror is safe, and this is the hazard everyone will expect and it is not there.** The
  mirror is fed by a *copy* — `ChunkAlloc` marshals the palette headers into `chunk_updates` and the
  blob bytes into `chunk_update_heap`, and `VoxelWorld::begin_frame` reads those two buffers
  `FRAMES_IN_FLIGHT + 1 = 2` frames later ([SOURCE] `voxel_world.comp.glsl:1069-1084`,
  `voxels/impl/voxel_world.cpp:468-493` — note the path: three files in this tree are named
  `voxel_world.cpp` and only `voxels/impl/` is the live one). **The CPU never dereferences a body.**
  So a body freed in frame N and reused in frame N+1 cannot corrupt a mirror read in frame N+2, and
  no pin is required. Verify this before building the pool anyway; it is the one assumption whose
  failure is a player falling through the floor.
- **Intra-frame neighbour sampling is safe for the same reason it is today.** `ChunkEdit` reads a
  neighbour's `palette_headers` through `sample_temp_voxel_chunk()`, and the task graph serialises
  `ChunkEdit` before `ChunkAlloc`.

**One thing must be added to the CPU-mirror contract:** `CpuChunkUpdateInfo.flags` currently means
"this slot has an update" ([SOURCE] `voxels/impl/voxels.inl:111-118`). It needs a second bit, "this
chunk became uniform", plus a `daxa_u32 uniform_value`, so the CPU can drop its own body and shrink
from 8192 B/chunk to 4 B/chunk plus a body vector.

### 5.3 An LRU: the contract gets worse in a way players feel

Today an edit survives **deterministically, for as long as you stay within 32 m**, and is lost at a
boundary the player can learn. Under a view-driven LRU it would be lost **when you look away**, at a
distance that depends on how much else is on screen, non-reproducibly. A tunnel that is there when
you turn around and gone when you turn around twice is a worse contract than one that is reliably
gone after a walk, and it is much harder to debug. **Both are broken; one is broken predictably.**
This is the same argument as §4 item 1 seen from the player's side: a distance key is learnable, a
recency key is not.

Fixing it properly needs a write-back path, dirty bits, an authored-delta store, pinning of dirty
pages (which ends the clean LRU), and bottom-up mip re-derivation on reload. [THESIS] §7.3.7 is
explicit that GigaVoxels has no write-back: *"cached data are simply used for rendering, and thus
are not modified."*

**The thing that would actually fix editing in voxl2 is an authored-delta store** — a sparse map
from world chunk coordinate to a brush-stroke list, replayed after the generator during `ChunkEdit`.
That is orthogonal to everything here, it is smaller than the body pool, and it should be built
first if persistence is wanted. `PERFORMANCE_PLAN.md`'s work plan does not contain it and it should.

### 5.4 The one new failure mode, and why it cannot be tested before Stage 1

A brush that promotes a chunk when the body pool is full. The intent is that `try_elect()` declines,
the edit does not apply to that chunk, and the stroke is silently incomplete — the same shape as
today's `try_elect()` overflow past 128, which is bounded upstream (radius ≤ 8 m,
`2r + stroke ≤ 16 m`, 5³ = 125 ≤ 128, reasoning at
[SOURCE] `voxel_world.comp.glsl:116-121`) so it cannot happen.

> **"A pool that is full declines to elect" is not implementable today, and the review is right.**
> The GPU cannot know: [SOURCE] `allocator.inl:465-475` states it in terms — *"the GPU-side
> allocator bumps `element_count` with an unchecked atomicAdd and writes into `heap[index]`
> immediately. **Nothing on the GPU knows the buffer's size.**"* The only guard is the
> **compile-time** `UserMaxElementCount` at [SOURCE] `allocator.glsl:37-40`, whose documented
> behaviour is to clamp to the last element, with the comment saying plainly that *"two regions then
> share a page and one of them renders wrong."*
>
> **Consequence for the plan:** a provocation run at `VOXL_MAX_CHUNK_BODIES = 64` would produce two
> chunks sharing a body — *corrupt* terrain, exactly the `cap2-refused-exhausted.png` failure the
> acceptance criterion says must not happen. The bound-check is a **prerequisite**, not a follow-up,
> and that is one of the two reasons it is now Stage 1.

**Required when the pool is built:** a counter incremented on every declined election, logged once
per frame it is non-zero, and shown in the debug overlay beside the existing `growth_refusals`.
`SCALE_LIMITS.md` §6 is the cautionary tale — a pool exhaustion that produced 4.6 MB of
out-of-bounds GPU writes, grey slabs through the terrain, a written screenshot and **exit code 0**.

---

## 6. Interaction with what already exists

| existing thing | what it already does | what changes |
|---|---|---|
| **`VoxelMallocPageAllocator`** ([SOURCE] `voxel_malloc.glsl`, `allocator.inl`) | GPU-resident pool, 2112 B pages, 24-slot bitfield per page, available + released stacks, shader-callable malloc/realloc/free, VRAM cap, refusal counter. **This is [PAPER] §6.1's brick pool minus the eviction discipline, already built** — and per §2.3 it is the pool that can actually fill | Mechanism unchanged. But `sub_allocator_state` moves out of the record into the body, so every `VoxelMalloc_*` signature changes from `daxa_RWBufferPtr(VoxelLeafChunk)` to `daxa_RWBufferPtr(VoxelChunkBody)`. One macro (`PAGE_ALLOC_INFOS`, `voxel_malloc.glsl:80`) and ~10 call sites — **but it means the allocator and the directory must be designed together, not sequentially** |
| **`DECL_SIMPLE_ALLOCATOR`** | The generic pool the above is instantiated from. **Six arguments**; the fifth is also the initial capacity (§3.2) | Instantiated a second time for `VoxelChunkBody`. No new allocator code |
| **The uniformity pyramid** (`lod_x2`…`lod_x32`, `uniformity_bits[3]`) | Empty-space skipping to one chunk = **4 m**, the brief's measured fact #1 | Meaning unchanged; `uniformity_bits` moves into the body. A UNIFORM chunk has no bits at all and the marcher answers LOD 7 **one load earlier**. **The 4 m stride ceiling is untouched.** Raising it needs a node above the chunk — the far field's job |
| **The far field** (`FF_CHUNKS_PER_AXIS 16`, `FF_LOG2_VOXEL_SIZE -2`, [SOURCE] `voxel_malloc.inl:55-56`) | One extra level, hand-unrolled: 25 cm voxels, 256 m box, **128 m radius**, same struct, same shaders, 33.7 MB of table. **Measured at +0.70 ms primary / +1.69 ms all-call-sites-with-clamp** | **Comes along free** if the directory is ever built. **But it does not need it:** `FAR_FIELD.md` §6 puts four levels at 135 MB of table, *"well inside"* budget, and `PERFORMANCE_PLAN.md` §5.3 sizes four nested levels at **128.4 MB for a 2048 m view radius**. **The draft's claim that the directory "unblocks L2 and L3" was wrong** — see the next row for what actually blocks them |
| **The allocator's heap floor and margin** ([SOURCE] `voxel_malloc.inl:132`, `allocator.inl:284-285`, `:318`) | A safety margin for `VoxelMalloc`'s unchecked `atomicAdd`: `(FIF+1) × VOXEL_MALLOC_MAX_PAGE_ALLOCATIONS_PER_FRAME` = 262 144 pages = **553.6 MB initial**, settling at the measured 3× to **1660.9 MB holding 52.0 MB** | **This is what blocks L2 and L3, and it is now Stage 1.** `FAR_FIELD.md` §6: four levels at the same reasoning is a ×4 margin, **1.1 GB initial and ~3.3 GB settled** on a card with 6144 MiB of which the renderer already wants 1785. It is *already* failing — with two levels the engine cannot start while a sibling instance is resident (`DAXA_RESULT_ERROR_OUT_OF_DEVICE_MEMORY`), which **blocks the interleaved-control measurement protocol this project depends on.** `FAR_FIELD.md` §7.1 item 3: *"Fix the allocator margin (§6) before L2, not after."* |
| **`MAX_CHUNK_UPDATES_PER_FRAME = 128`** | [PAPER] §6's fixed upload budget, same shape and same silent-drop failure mode | Unchanged, but recognise what it is: it sizes `temp_voxel_chunks` (134.9 MB), `chunk_update_heap` (268.4 MB) and the 553.6 MB allocator floor. **Four of the five largest buffers in this engine are sized by this one constant and none of them by the world.** [DERIVED] a full-budget generation frame is worth **9.34 ms**: 19.70 ms per generation frame (0.65 s / 33 frames and 5.10 s / 259 frames agree to three digits, `SCALE_LIMITS.md` §4) against a **10.36 ms** idle p50 at the same pose (`CACHE_CURRENT_STATE.md` §7), i.e. 9.34 ms for 128 chunks = **0.073 ms per chunk generated**. Any streaming work should treat it as the master knob and make it per-level |
| **`log_table_census()`** ([SOURCE] `voxels/impl/voxel_world.cpp:147-191`) | Already prints `dense → pooled equivalent` **and the `header_only` bucket §3.4 needed** | Becomes the acceptance instrument, and §10's one measurement is a single run of it on a different scene |

**What must not change:** `LOG2_VOXEL_SIZE` stays at −4; `CHUNKS_PER_AXIS` stays a power of two and a
multiple of 8 (the `#error` at `voxel_malloc.inl:73` is load-bearing and the failure it prevents is
silent — `SCALE_LIMITS.md` §2, 70 % of the world never generated, exit code 0); and the eight-byte
`VoxelWorldGlobals` members stay at the front of the struct for the layout reason at
`voxels.inl:73-97`.

**Push-constant budget.** New buffer addresses ride in `VoxelWorldGlobals` as `daxa_u64`s
immediately after `ff_voxel_chunks_addr`, **not** as `DAXA_TH_BUFFER_PTR` on `VOXELS_USE_BUFFERS`.
The widest of the twenty task heads that expand that macro was already at 120 of Vulkan's 128 push
constant bytes, and overflowing it presents as a **silently missing pass**, not an error
([SOURCE] `voxels.inl:73-97` — this constraint has already bitten this project once).

---

## 7. The plan, re-sequenced

**The draft ranked the directory first. It is now fifth.** The ordering criterion is *world-extent
gained per unit of risk*, with a hard constraint this project has earned the right to impose:
**after four consecutive sessions cut off by usage limits, every stage must leave the engine
building, running and revertible.**

| # | stage | gains | effort | risk |
|---|---|---|---|---|
| **1** | **Allocator bound-check + per-level election budget** | unblocks L2/L3; frees ~1.3 GB; makes every later failure mode diagnosable instead of corrupt; **restores two-instance measurement** | 2 files | **low** — named a prerequisite by `FAR_FIELD.md` §7.1 |
| **2** | **`update_index` out of the dense table** | removes §2.4's third wall; tests half the L2 hypothesis for 10 lines; required by any later directory | 3 files | **low** — cannot change a pixel |
| **3** | **Per-class ray reach** | makes L2/L3 affordable at all | one mask | low — the crude version is already measured at 10× |
| **4** | **Far-field L2 + L3** | **128 m → 2048 m visible radius, 16× linear**, +2–3 ms, +67 MB of table | ~30 lines/level, precedent shipped | low |
| **5** | **Directory + body pool** (§3) | 32 m → 100–200 m *near* radius, contingent on §10 | rewrite of the central data structure | **high** |
| **6** | **Request-driven election** (§3.5) | time-to-playable, not memory | one buffer + a sort | low |
| **7** | **Eviction** | nothing, on current evidence | — | — |

**Measurement protocol for all of them**, from this project's own hard-won practice: `VOXL_DATA_DIR`
fresh per run; explicit working directory `C:\voxl2` (launching from elsewhere aborts at
`0x80000003` with empty stderr); GPU verified uncontended by process count, **not** by
`nvidia-smi --query-compute-apps` which returns `[N/A]` on this driver (`SCALE_LIMITS.md` §1);
control and test runs **interleaved, never batched**; `--render-scale 1.0 --gi true
--reflections false --shadows true --no-overlay --set UI/show_tool_hud=false` pinned on the command
line; and **every image opened and read**, because a failed shader compile deletes a pass silently.

Poses: vista (absolute `-182.990,-109.980,-46.970` rot `0.785,1.096`), sky
(`-183.0,-110.0,-27.5` rot `0.785,1.62`), cave (`-170.0,-97.0,-49.3` rot `0.785,1.45`), plus
`--patrol` for a moving pose. **`--pos` takes absolute metres; every pose in `SCENE.md` is
scene-local, `local = absolute + (183, 110, 52.5)`** — passing the local triple puts the player 210 m
out in open water and the run still exits 0 with a plausible-looking screenshot.

---

### Stage 1 — bound-check the GPU allocator and give coarse levels their own election budget

Two files. Neither deletes anything. This is the stage specified in full in §10.

### Stage 2 — `update_index` out of the dense table, and the repaired directory probe

**2a (do this alone first).** Move `update_index` from `VoxelLeafChunk` offset 4 into its own
`daxa_u32[CPA³]` buffer. Sites: the write at `voxel_world.comp.glsl:202`, the read in
`sample_temp_voxel_chunk()` ([SOURCE] `voxels.glsl:170-186`), the struct, one address in
`VoxelWorldGlobals`, one `TemporalBuffer`. **~10 lines, cannot change a pixel, and it deletes
`CPA³ × 64 B` of dirty L2 lines per frame.**

Acceptance: byte-identical screenshots at all three poses (`fc /b`); no pose slower than 1 %; and
report at **CPA 16 and CPA 32**, because the mechanism scales with `CPA³` and CPA 16 is where §2.5
predicts ≈0.

**2b (only if 2a is not negative).** The directory as a *redundant derived side-table*, behind
`#if VOXL_CHUNK_DIR`. Five sites — but **six, not five: the draft's list omitted the wrap
invalidation at `voxel_world.comp.glsl:78-81`.** A derived directory not cleared when
`ACCEL_GENERATED` is cleared answers UNIFORM with stale terrain the first time the player crosses a
chunk boundary, and only `--patrol` catches it.

**The `HAS_BODY` derivation is NOT "region 0 thread 0 in ChunkAlloc".** `ChunkAlloc` dispatches one
workgroup per palette region, 512 per chunk ([SOURCE] `voxel_world.comp.glsl:958-975`), with no
cross-workgroup reduction; region 0 cannot know region 511's vote. Use §3.4's mechanism —
`region_nonuniform_bits` by `atomicOr`, plus `value_and`/`value_or` — initialised by ChunkOpt x8up's
existing single-thread block at `:894-896` and read the *next* frame in `PerChunkCompute`. That
makes the derived directory one frame stale, which is fine for a side-table and is **not** fine for
the real thing; note it and move on.

**Acceptance, and the stop rule is different from the draft's:**

- Byte-identical screenshots at all three poses **plus `--patrol`**.
- **The sky pose ≥ 3 % faster at p50, measured at CPA 32 — not CPA 16.** §2.5's own model predicts
  ≈0 at CPA 16, where the touched slices are 524 KB and already L2-resident. A gate set where the
  mechanism cannot act is a gate set to produce a false negative.
- No pose more than 1 % slower.
- **The gain must grow from CPA 16 to CPA 32.** A flat gain means the mechanism is not the one
  claimed.

> **On the stop rule.** The draft said *"if this fails, stop."* That is wrong in one direction. Both
> arms of 2b still carry the dense table and still write it every frame, so 2b measures only the
> **read-side** half of the hypothesis and is biased *against* the treatment. **A pass is
> trustworthy; a fail is not decisive** — it means "not worth the rewrite on this evidence", which
> is the right conclusion anyway, but it is not "the mechanism does not exist". 2a is what removes
> the bias, which is why it comes first.

### Stages 3–4 — per-class ray reach, then far-field L2 and L3

`FAR_FIELD.md` §7.1 items 1–2, unchanged, and they are that document's recommendation rather than
this one's. The only thing this document adds is the sequencing claim: **they come before the
directory, not after it**, and Stage 1 comes before them.

### Stage 5 — the directory and body pool

§3, and only if §10's census says the table is the binding pool. **This stage has no intermediate
state in which the engine runs**: `VoxelLeafChunk` → `VoxelChunkDir` + `VoxelChunkBody`, second
`DECL_SIMPLE_ALLOCATOR`, body allocated in `try_elect()`, demote sweep, and every `VoxelMalloc_*`
signature retargeted from the record to the body, across `sample_lod` ×2, `sample_voxel_chunk` ×2,
`sample_temp_voxel_chunk`, `VoxelUniformityChunk_lod_nonuniform_{16,32,64}`, election ×2, `ChunkOpt`,
`ChunkAlloc`, all of `voxel_malloc.glsl`, the CPU mirror and two startup clears
(`CACHE_CURRENT_STATE.md` §4.2 enumerates them). **§8 item 8(b) of the draft said this about itself
and was right: the allocator and the directory must be designed together.** That is a
disqualifying shape for a session that may be cut off, and it is why Stage 1 and Stage 2a exist as
separable pieces of the same work.

Acceptance if it is ever run: frame time within 1 % of Stage 2 at all four poses; `log_table_census`
actual matches prediction (**2.64 MB at CPA 16**); `cpa64-vista.png` identical and process VRAM at
CPA 64 down from **3790 MiB to under 1850 MiB** ([DERIVED] from `SCALE_LIMITS.md` §5's fit:
`1785 MiB + 10.17 MB` = **1794.7 MiB predicted**); and the provocation run at
`VOXL_MAX_CHUNK_BODIES = 64` showing *missing* terrain, not *corrupt* terrain — **which requires
Stage 1 to have landed** (§5.4).

### Stage 6 — request-driven election, priority only

§3.5. Acceptance: time to first complete frame at CPA 32 falls materially from the measured
**259 frames / 5.10 s**, target ≥ 3×; the exhaustive fallback still completes the world (census at
t = 30 s reports the same paletted count); **zero new holes** in the seven-shot regression set and a
5-second `--patrol`; and `TracePrimaryCompute` grows by **< 0.05 ms** of its measured 1.075 ms,
**measured at CPA 64 where the 1.05 MB request buffer is not L2-resident**, not at CPA 16 where it
is.

### Stage 7 — eviction. Do not start it.

Entry condition, corrected to the right pool (§4): **`voxel_malloc` heap in use sustained above
1.5 GB in a shippable scene with the far field already carrying everything past its radius.** If
that day comes, read §4 item 1 first — the key should probably still be distance — and only then
[THESIS] §7.3.4 verbatim, with `GIGAVOXELS_NOTES.md` §3.3 alongside it, because the thesis
contradicts itself on the list's orientation and the shipped code settles it.

---

## 8. What would make this a mistake

**1. Stage 1 changes an image.** The bound-check is on a path that currently writes out of bounds;
if clamping it changes a pixel, something was depending on the overflow. **Detect:** `fc /b` at four
poses. **Threshold:** zero differing bytes.

**2. The indirection is slower than the dense array — the one that would kill §3.** **Detect:**
Stage 2b's A/B, which deletes nothing. **Threshold:** sky ≥ 3 % faster **at CPA 32**, nothing > 1 %
slower, gain grows from CPA 16 to CPA 32.

**3. `PerChunkCompute` is not the cost §2.4 claims.** The ×64 extrapolation from 0.053 ms is
unmeasured. **Detect:** `VOXL_GPU_PROFILE=out.csv` on `PerChunkCompute` alone at CPA 16/32/48/64,
standing still, world fully generated. **Threshold:** if it does not grow as `CPA³`, Stage 2a's
rationale is wrong and it is merely a hygiene fix — still worth doing, no longer a probe.

**4. Far-field L2/L3 costs what L1 cost.** +0.70 ms per level compounds. **Detect:** the same sweep
`FAR_FIELD.md` §5 ran. **Threshold:** `PERFORMANCE_PLAN.md` §5.5's budget, +1.4 ms ± 30 % per level.
**Prior:** L1 came in *under* budget on primary and 12× over on all call sites until the 48 m
secondary clamp took +16.6 ms back to +1.69 ms. Per-class reach (Stage 3) is not optional.

**5. The world the project ships is dense, and none of this reaches it.** §2.3. **Detect:** §10.
**Threshold:** `voxel_malloc` heap in use > 1.5 GB at CPA 32 on the reference-density scene means
the binding pool is the heap, Stage 5 is pointless, and the work is LOD — Stages 3–4 — plus, for the
first time in this project, a real case for §4's eviction with a distance key.

**6. Pool exhaustion putting a hole in the world, silently.** **Detect:** Stage 5's provocation run,
**after** Stage 1. **Prior:** `SCALE_LIMITS.md` §6 provoked exactly this class of failure and got
4.6 MB of out-of-bounds GPU writes, grey slabs through the hillside, a written screenshot and
**exit code 0**. The pass condition is not "it does not happen"; it is "it is impossible to miss
when it does".

**7. Popping as coarse levels substitute.** Reachable from Stage 6. **Detect:** arrive at the same
pose by two routes and capture at frame N and N+120. **Threshold:** zero differing pixels after 120
settled frames. **Mitigation designed in:** §3.5's priority-not-gate rule.

**8. Two engineering hazards that are not performance.** (a) The push-constant ceiling: the widest
task head is at 120 of 128 bytes and overflow presents as a *silently missing pass*
([SOURCE] `voxels.inl:73-97`). (b) The allocator/directory coupling: `sub_allocator_state` is 4096 B
of per-chunk allocator state living *inside* the record, reached by every malloc, realloc and free
([SOURCE] `voxel_malloc.glsl:80`). A chunk cannot become 8 bytes until it moves.

---

## 9. What this document does not establish

1. **Which pool binds on a scene the project intends to ship.** §10. Everything in §2.4 and §3 is
   contingent on it and it is one run.
2. **That the directory is faster.** Argued from cache lines, built by nobody. Stage 2b exists for
   it and is deliberately conservative.
3. **The measured distinct-chunk working set across all eight `voxel_trace()` call sites.** §2.5
   uses an occlusion-free upper bound and the dereference count (10.9–32.4 M/frame, [MEASURED]) as
   handles. The union under real occlusion is unmeasured; `CACHE_CURRENT_STATE.md` §6 specifies the
   ~40-line probe. **It cannot change the LRU verdict** — that now rests on the census and on §4
   item 1's distance-versus-recency argument — but it would sharpen §4's entry condition and it is
   the number the draft mislabelled.
4. **The L2 story.** GA107's 2 MB is a specification figure. The knee has not been measured;
   sweeping CPA with the island held at constant size on an uncontended GPU would show it.
5. **`PerChunkCompute`'s cost at scale.** §8 item 3.
6. **Ray-guided *generation* as a replacement for the `O(CPA³)` election scan**, rather than a
   reordering of it. This is [PAPER]'s actual named contribution (§6.1 p.5) and no document in this
   phase evaluated it, because the brief framed GigaVoxels as an LRU and four documents spent
   themselves refuting an LRU. §3.5's priority-not-gate rule is the right safety property and the
   wrong permanent architecture; the reconciliation is sketched at the end of §3.5 and is not
   designed.
7. **PCIe generation and lane count on this machine.** Irrelevant here because nothing streams over
   PCIe — which is itself §4 item 1's premise — but it would matter to any future that does.
8. **GigaVoxels DP (HPG 2024, DOI 10.1145/3675389).** The modern successor, unread by all three
   research agents — ACM 403, HAL behind a bot check. By title it addresses starvation, i.e. the
   request-latency problem Stage 6 would inherit. **The highest-value unread source.**
9. **Whether an authored-delta store is the right answer to §5.1.** Asserted, not designed.

---

## 10. Recommendation

### NO-GO on the ray-guided streaming cache. NO-GO on the directory as the next work. GO on Stage 1 below.

Plainly:

- **The LRU: NO.** The pooled index is 9.1 MB on a 6144 MiB card, the payload pool's natural
  eviction key here is distance rather than recency, and the engine already has both a distance
  policy (chunk wrapping) and a coarse fallback (the far field). §4.
- **The directory + body pool: NOT NEXT.** It is a well-shaped design, six defects lighter than it
  was, and it remains the only route to a large *near-detail* radius. But it is a rewrite of the
  engine's central data structure with no intermediate running state, aimed at a 6× gain, while four
  far-field levels give **16× visible extent for about thirty lines per level with a shipped
  precedent** — and §2.3 says the 6× is contingent on a scene emptier than the one the project wants
  to ship. Revisit it after §10.3.
- **What to build instead: Stage 1 now, then per-class ray reach, then far-field L2 and L3.** Each
  is one or two files, each leaves the engine running, each is individually revertible, and the
  first of the three is a prerequisite named by `FAR_FIELD.md` §7.1 rather than by this document.

### 10.1 Stage 1, in full — one sitting

**Bound-check the GPU allocator, and give coarse far-field levels their own election budget.**

Why this and not something else: it is the **largest single VRAM line in the engine** (1660.9 MB of
capacity holding 52.0 MB, [MEASURED] `CACHE_CURRENT_STATE.md` §1.2); it is **already blocking
measurement**, because with two levels resident the engine cannot start alongside a sibling instance
and this project's entire protocol is interleaved control-and-test runs; it is the **prerequisite
`FAR_FIELD.md` §7.1 item 3 names** for L2 and L3; and it is the prerequisite for the body pool's
failure mode to be diagnosable rather than corrupt (§5.4). It is on the critical path of every other
route.

**The work, two files:**

1. **`src/utilities/allocator.inl`** — put the runtime capacity into the shared allocator struct.
   The struct at `:13-20` has `element_count`, `available_element_stack_size` and
   `released_element_stack_size` and no capacity; add `daxa_i32 element_capacity`, written by the
   CPU in `create()` (`:314-322`) and on every successful `check_for_realloc()` growth
   (`:476` onwards).
2. **`src/utilities/allocator.glsl`** — in `FUNC_NAME(malloc)`, replace the compile-time
   `UserMaxElementCount` clamp at `:37-40` with a runtime test against `element_capacity`, and on
   failure **decrement and return a sentinel** rather than clamping to the last element. The
   existing comment at `:30-36` says exactly why clamping is wrong (*"two regions then share a page
   and one of them renders wrong"*) and exactly what is needed (*"The real fix needs a failure
   return the caller can act on"*). Give `VoxelMalloc_malloc`'s fallback loop
   ([SOURCE] `voxel_malloc.glsl:150-189`, which currently spins until it succeeds) that path, and
   increment a `refusals` counter the overlay already knows how to display.
3. **`src/voxels/impl/voxel_malloc.inl:132`** — the `× 2` on
   `VOXEL_MALLOC_MAX_PAGE_ALLOCATIONS_PER_FRAME` exists because two levels can both run a full
   `ChunkAlloc` in one frame. With the bound-check in place, replace it with a **runtime per-level
   election budget in `try_elect`** — L0 keeps 128, L1 gets 32 — so the margin goes from ×2 to about
   ×1.25 (`FAR_FIELD.md` §6's own recommendation, and §5.7 item 4 already asked for it).

**Acceptance — all four must hold:**

| # | criterion | how |
|---|---|---|
| 1 | **Byte-identical images** at vista, sky, cave and a `--patrol` capture at both ends | `fc /b` against the committed controls. A bound-check that changes a pixel means something depended on the overflow |
| 2 | **Heap capacity at the vista settles under 300 MB**, against today's **1660.9 MB** | the engine's own `[heap]` log line and the overlay, at t = 30 s settled |
| 3 | **Zero growth refusals** in a 60 s `--patrol` run, and the new malloc-refusal counter also zero | overlay + log; a non-zero count here means the per-level budget is too tight, which is a tuning result not a failure |
| 4 | **Two `gvox_engine.exe` instances start simultaneously on this 6 GB card and both render** — *they cannot today* | this is the criterion that matters most, because it restores the measurement protocol every later stage depends on |

**No pose may be more than 1 % slower at p50.** This is a memory change; it should be invisible in
time.

**The measurement that proves it worked** is criterion 4, and it is binary. Today the second
instance dies at buffer creation with
`[[DAXA ASSERT FAILURE]]: error code: DAXA_RESULT_ERROR_OUT_OF_DEVICE_MEMORY(-2)` before writing a
single CSV row ([MEASURED] `FAR_FIELD.md` §6, which attributes four failed runs in that document to
exactly this). After Stage 1 both must produce frames and both must write CSV. **Run it, capture
both logs, and put the pair in `docs/`.**

**It leaves the engine building and running by construction** — the change is a bound on a write
that is currently unbounded, plus one constant becoming a runtime budget. Reverting is two `git
checkout`s.

### 10.2 If Stage 1 lands, the next two are already specified elsewhere

Per-class ray reach (`FAR_FIELD.md` §7.1 item 2 — one mask; the crude single-clamp version is
already measured at a 10× recovery) and then far-field L2 and L3 (`FAR_FIELD.md` §0 finding 4:
12 lines of constants, 2 of buffer plumbing, one march function, no new generation code, per level).
That sequence takes the visible radius from **128 m to 2048 m** for a measured-and-extrapolated
+2–3 ms and +67 MB of table. Nothing in this document needs to be true for it to work.

### 10.3 The one measurement that would most change the answer

**Run `log_table_census()` and read the `voxel_malloc` heap-in-use figure on a
reference-density scene at CPA 16 and CPA 32 — not on the island.**

- **Command:** `VOXL_VEG_FOREST=1 VOXL_VEG_TREE_SPACING=3.0` (the `R1` reference-density
  configuration, `DENSITY_LIMITS.md` §4: 74 stems / 1111 stems/ha, 0.11 ferns/m²), at
  `CHUNKS_PER_AXIS` 16 and 32, vista pose, `--bench-csv`, `VOXL_DATA_DIR` fresh, uncontended card,
  one run each. Report the census line, `paletted`, `header_only`, and heap **in use** at t = 30 s.
- **Cost:** two runs and a rebuild. No new code — the instrument is already in the tree at
  `voxels/impl/voxel_world.cpp:147-191` and already prints everything needed.

**Why it is the one.** Four numbers in this document turn on it:

| if the census says | then |
|---|---|
| **heap in use stays under ~300 MB at CPA 32** and `paletted` scales as CPA² | the island generalises, the table is the binding pool, §2.4's ~200 m detail radius is real, and **Stage 5 is worth its risk after Stage 4** |
| **heap in use approaches the demo world's 1262.8 MB** | the payload binds long before the index, **Stage 5 buys nothing**, the work is permanently LOD, and §4's eviction gets its first real case — **with a distance key, which means it is the far field, not an LRU** |
| **`header_only` is non-zero** | §3.4's fifth state is load-bearing rather than theoretical, and any future directory must carry it |

It also sizes `VOXL_MAX_CHUNK_BODIES`, which §3.2 deliberately declines to pick, and it closes
`SCALE_LIMITS.md` §10 item 3, which has been open since the scale sweep.

**Two secondary measurements, both cheap, both listed here so they are not lost:** `VOXL_GPU_PROFILE`
on `PerChunkCompute` alone at CPA 16/32/48/64 standing still (§8 item 3 — decides whether §2.4's
third wall is real), and `CACHE_CURRENT_STATE.md` §6's ~40-line touch probe (§9 item 3 — the number
the draft mislabelled, and the only way to replace the empty-box bound with a measurement).

---

## 11. Considered and rejected

Recorded so these are not relitigated.

**1. "A lower touched fraction under occlusion is an argument *for* a residency scheme."** The
review is right that the empty-box bound is an upper bound and that the true fraction is lower.
The inference does not follow **for the index**: the thing a residency policy would evict is an
8-byte directory entry from a 2.10 MB structure, and no touched fraction makes that worth managing.
It *does* follow for the payload, which is why §2.3 and §4 relocate the whole question to
`voxel_malloc`. Accepted as fact, redirected as inference.

**2. "Stage 1 is not a valid experiment — control and treatment share the mechanism under test."**
Partly. Both arms of Stage 2b keep the dense table and keep writing it every frame, so the shared
mechanism biases the result **against** the treatment. That makes it underpowered, not invalid: a
pass is trustworthy, a fail is not decisive. The draft's *"if this fails, stop"* is what was wrong.
Stage 2a exists to remove the bias, and the ≥ 3 % gate has moved to CPA 32 where §2.5's own model
says the mechanism can act.

**3. "`VOXL_MAX_CHUNK_BODIES = 65536` is under-sized for CPA 128."** The arithmetic is right
(128² columns × 3–8 vertical chunks ≈ 49k–131k). But §2.3 says a dense CPA 128 box is unreachable
for an unrelated reason — 2–3 GB of heap — so tuning a constant against a box volume that cannot
exist is the wrong exercise. §3.2 now declines to pick a number and defers to §10.3.

**4. "Eviction: none, on the island evidence."** Too strong. §2.3's demo-world figure makes
eviction *conceivable* on the payload pool for the first time in this project, and §4 item 1 argues
it should then be keyed on **distance**, not recency — at which point the mechanism is the far field
and the coarse level is the fallback, which is [PAPER] §4.1's own answer to an unresolved node. The
verdict "no LRU" is unchanged; the reasoning is no longer "there is nothing to evict".

**5. "`HANDOFF.md`'s no-remote gate is stale."** Confirmed: `git remote -v` in `C:\voxl2` returns
`origin https://github.com/luigilopesz/Voxl.git`. The gate on Stage 5 is satisfied and `HANDOFF.md`
has been corrected.

---

*Sources: [PAPER] and [THESIS] as cited, read by the research agents whose documents are named at the
head of this file. voxl2 figures are cited to `docs/SCALE_LIMITS.md`, `docs/DENSITY_LIMITS.md`,
`docs/FAR_FIELD.md`, `docs/PROFILE.md`, `docs/design/WORLD_SCALE.md`,
`docs/design/PERFORMANCE_PLAN.md` and `docs/design/CACHE_CURRENT_STATE.md`, each of which carries the
command that produced it. Source citations are file:line at `dbbd54e` and every one in this revision
was re-read in the tree. No measurement was re-run for this document and no engine code was
modified.*
