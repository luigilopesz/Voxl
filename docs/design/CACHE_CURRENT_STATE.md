# Cache optimisation, part 1: what voxl2 actually has today

The design brief for this phase is GigaVoxels-style ray-guided streaming with an LRU pool. This
document is the *precondition* for that design: it establishes what the engine's memory model is
now, how much of it a frame really touches, which parts of GigaVoxels are already built here under
other names, and what an edit does today. It proposes nothing. The proposal is the next document.

Every claim is tagged:

- **[PAPER]** — Crassin, Neyret, Lefebvre, Eisemann, *GigaVoxels: Ray-Guided Streaming for
  Efficient and Detailed Voxel Rendering*, I3D 2009.
  <https://maverick.inria.fr/Publications/2009/CNLE09/CNLE09.pdf> — section numbers are the
  paper's own.
- **[LATER]** — established after the 2009 paper, by the same group or others.
- **[SOURCE]** — read out of `C:\voxl2` at commit `dbbd54e`; file and line given.
- **[MEASURED]** — measured in this session on the target machine; the command is in §7.
- **[DERIVED]** — arithmetic on [SOURCE] constants or [MEASURED] values, shown.

Hardware: RTX 3050 6 GB Laptop (GA107), i7-13650HX, 16 GB RAM, Windows 11. Binary
`.out\cl-x86_64-windows-msvc\Release\gvox_engine.exe`, built 2026-08-01 02:28, tree clean at
`dbbd54e`. **No file in `C:\voxl2\src` was modified for this document.**

---

## 0. The answers, in brief

1. **The memory model.** Nine buffers. Two scale as the cube of the world edge and are resident
   whether or not the world contains anything (the L0 chunk table in VRAM and its CPU mirror in
   host RAM). One is a growable pool whose *floor* is a 553.6 MB safety margin unrelated to
   content. The rest are fixed. **The 269 MB-at-CPA-32 figure is confirmed** — derived from
   `sizeof(VoxelLeafChunk)` = 8216 B and printed by the engine itself.
2. **How much is touched.** Two different numbers, and conflating them is the trap.
   *Chunks holding data*: **7.6 %** at CPA 16, 2.6 % at CPA 32, 0.3 % at CPA 64 — and the absolute
   count barely moves (313 → 856 → 847), because it is set by the island, not by the box.
   *Chunk records dereferenced by the marcher*: **10.9 M to 32.4 M per frame** for primary
   visibility alone [MEASURED], covering ~17–21 % of the table from the frustum's solid angle at
   **any** world size [DERIVED], and effectively all of it once GI and shadow rays are counted.
   **A cache over the bodies wins. A cache over a flat directory thrashes.** §2.5 is the argument.
3. **What already resembles GigaVoxels.** More than expected. `VoxelMallocPageAllocator` is a
   GPU-resident pooled allocator with a free list — GigaVoxels' brick pool minus the LRU. The
   palette `variant_n < 2` case is the paper's constant-value node, exactly. The far field is the
   first two levels of the N³-tree, built by hand. The chunk update budget is the paper's fixed
   upload budget. **What is missing is the tree and the feedback loop, not the pool.**
4. **What would change.** Adding an LRU beside `voxel_malloc` is incremental. Replacing the flat
   `voxel_chunks` array with an indirection is a rewrite of six functions across four files, and it
   is the change that actually buys the world size. §4 separates them.
5. **The edit path.** An edit re-runs the *entire* generate-compress-allocate pipeline for every
   chunk the brush's swept capsule touches, every frame the button is held, and frees and
   reallocates every palette region in those chunks. Under a cache, an edited chunk is a cache
   *write*, and today's path has no concept of one. §5.

**One correction to the brief, and it matters for the design.** The mechanism described in the
brief — a 1-D array, used elements moved to the *front*, running entirely in compute shaders,
stable under a 1 MB pool, addressing 64 GB — **is not in the 2009 paper.** The paper's LRU is
timestamp-based and **CPU-managed**: §6.1 says each node and brick carries a timestamp reset on
use, the timestamps live in host memory in a mirrored structure, and the CPU decides placement and
pushes changes to the GPU. The paper's own figures (§7) are a **4 MB node pool** and a **430 MB
brick pool** on a 512 MB card. The front-of-array, GPU-resident formulation is [LATER] work —
Crassin's 2011 thesis and the GigaVoxels/GigaSpace library
(<https://maverick.inria.fr/Membres/Cyril.Crassin/thesis/>,
<http://gigavoxels.inria.fr/WhatIsGigaVoxels.html>). **I could not verify the 1 MB / 64 GB figures
to any page in either source in this session; treat them as unattributed until someone does.** The
distinction is not pedantry: a CPU-managed LRU costs a readback per frame and voxl2 is 97.5–99.4 %
GPU-bound, so the 2009 design is the wrong one to copy here and the later one is the right one.

---

## 1. The memory model

### 1.1 The record that everything else follows from

`VoxelLeafChunk` — `src/voxels/impl/voxels.inl:33-42` [SOURCE]:

| field | offset | bytes | share |
|---|---:|---:|---:|
| `flags` | 0 | 4 | 0.05 % |
| `update_index` | 4 | 4 | 0.05 % |
| `uniformity_bits[3]` | 8 | 12 | 0.15 % |
| *(pad to 8-byte alignment for the u64 array)* | 20 | 4 | |
| `VoxelMalloc_ChunkLocalPageSubAllocatorState` — `u64 page_allocation_infos[512]` | 24 | 4096 | 49.9 % |
| `PaletteHeader palette_headers[512]` (`{u32 variant_n; u32 blob_ptr;}`) | 4120 | 4096 | 49.9 % |
| **total** | | **8216** | |

Confirmed against the engine's own startup line [MEASURED]:

```
[scale] chunk table: 8216 B/chunk x 4096 = 33.7 MB VRAM, resident when empty;
        CPU mirror 8192 B/chunk = 33.6 MB host RAM
```

`8216 x 32768 = 269 254 656 B` = **269.25 MB at CHUNKS_PER_AXIS 32. The figure in the brief is
correct**, and it is a decimal-MB figure (256.8 MiB). Both halves of the 99.8 % are per-palette-region
arrays of 512 entries, and neither is touched at all by a chunk of pure air.

### 1.2 Every buffer, what it costs, and what it scales with

Shipped configuration: `CHUNKS_PER_AXIS 16`, `FF_CHUNKS_PER_AXIS 16`, `FRAMES_IN_FLIGHT 1`
(`src/application/settings.inl:95`), `MAX_CHUNK_UPDATES_PER_FRAME 128`, 1280×720, Balanced.

| # | buffer | memory | size | shipped | scales with |
|---|---|---|---|---:|---|
| 1 | `voxel_chunks` (L0 table) | VRAM | `8216 × CPA³` | **33.65 MB** | **world edge ³ — resident when empty** |
| 2 | `ff_voxel_chunks` (L1 table) | VRAM | `8216 × FF_CPA³` | **33.65 MB** | far world edge ³, same |
| 3 | `voxel_malloc` element buffer | VRAM | `2112 B × capacity` | **1660.9 MB** *(52.0 MB used)* | content, but **floored at 553.6 MB** |
| 4 | `voxel_malloc` avail/released stacks | VRAM | `8 B × capacity` | 6.3 MB | with #3 |
| 5 | `temp_voxel_chunks` | VRAM (transient) | `1 054 032 × 128` | **134.9 MB** | `MAX_CHUNK_UPDATES_PER_FRAME` — fixed |
| 6 | `chunk_update_heap` | host-visible | `4 × 64³ × 128 × 2` | **268.4 MB** | fixed |
| 7 | `chunk_updates` | host-visible | `4104 × 128 × 2` | 1.05 MB | fixed |
| 8 | `voxel_globals` + `ff_voxel_globals` | VRAM | ~11 KB each | 0.02 MB | fixed |
| 9 | `VoxelWorld::voxel_chunks` (CPU mirror) | **host RAM** | `8192 × CPA³` | **33.55 MB** | **world edge ³** |

Sources: #1 `voxel_world.cpp:254`; #2 `far_field.cpp:33`; #3/#4 `allocator.inl:270-278`
(`TOTAL_BYTES_PER_ELEMENT` = 2112 + 8 = 2120 B, and the engine prints `2120 B/element`);
#5 `voxel_world.cpp:562`; #6 `voxel_world.cpp:208`; #7 `voxel_world.cpp:203`; #9
`voxel_world.cpp:265`.

Three things in that table are worth saying out loud.

**(a) The heap is 32× over-provisioned, and 553.6 MB of it is structural.** [MEASURED] At the vista
pose the heap settles at **786 432 pages of capacity = 1660.9 MB, holding 52.0 MB of palette data**
— and it never shrinks. The floor is not content: `PER_FRAME_HEADROOM`
(`allocator.inl:284-285`) is `VOXEL_MALLOC_MAX_PAGE_ALLOCATIONS_PER_FRAME × (FIF+1)`
`= (512 × 128 × 2) × 2 = 262 144 pages = 553.6 MB`, and it exists only because the GPU-side
allocator bumps `element_count` with an unchecked `atomicAdd` and nothing on the GPU knows the
buffer's size (`allocator.inl:468-475`). The engine's own frame-0 row reads
`heap_pages 262144, heap_capacity_mb 553.65`; one 1.5× growth step later it is 786 432 /
1660.94 MB and stays there for the remaining 2280 frames. **The ×2 in
`VOXEL_MALLOC_MAX_PAGE_ALLOCATIONS_PER_FRAME` was added by the far field** (`voxel_malloc.inl:132`),
so the far field doubled a 276.8 MB floor to 553.6 MB. A pool design that wants to be tight has to
address this before it addresses anything else — **it is ten times the size of the entire chunk
table at the shipped world size.**

**(b) `chunk_update_heap` is 268.4 MB of host-visible memory and is pure edit-path plumbing.** It
exists so the CPU can mirror palette blobs for `VoxelWorld::sample()`, which exists for player
collision (`voxel_world.comp.glsl:1061-1068`, `player.cpp:267`). It does not scale with the world;
it scales with `MAX_CHUNK_UPDATES_PER_FRAME`.

**(c) The far field costs exactly one more table and nothing else.** It shares the heap, shares the
134.9 MB transient, and writes no CPU mirror. 33.65 MB buys a 256 m volume at 25 cm — **the same
33.65 MB that buys the near level 64 m at 6.25 cm.** That is already the multi-resolution argument
made concrete, and it is the strongest evidence in the tree that the GigaVoxels direction is right.

### 1.3 The cube, drawn

```
world edge (metres)      64      128       256        512
CHUNKS_PER_AXIS          16       32        64        128
chunks                4 096   32 768   262 144  2 097 152
------------------------------------------------------------------
L0 table (VRAM)      33.7 MB  269.2 MB  2153.8 MB  17230.2 MB   <- cube law, content-independent
CPU mirror (host)    33.6 MB  268.4 MB  2147.5 MB  17179.9 MB   <- cube law, content-independent
heap floor (VRAM)   553.6 MB  553.6 MB   553.6 MB    553.6 MB   <- flat
transient (VRAM)    134.9 MB  134.9 MB   134.9 MB    134.9 MB   <- flat
chunks WITH DATA        313      856       847         ~850     <- flat, measured (SCALE_LIMITS 3.3)
```

The last two rows are the whole argument in four numbers. **The content is flat. The table is
cubic.** At CPA 64 the engine spends 2153.8 MB of VRAM and 2147.5 MB of host RAM to index 847
chunks that hold anything.

---

## 2. How much of it is touched in a frame

This is the question the brief calls the most valuable thing in the phase, so it is worth being
precise about what "touched" means, because the two plausible meanings give answers three orders of
magnitude apart.

### 2.1 What one DDA step actually reads

`sample_lod()` (`src/voxels/impl/voxels.glsl:190-247`) [SOURCE] is called **exactly once per DDA
step** by `voxel_trace()` (`trace.glsl:163, 212`). Per call it dereferences exactly one
`VoxelLeafChunk`, reading:

- `flags` — 4 B at `chunk_base + 0`; if `CHUNK_FLAGS_ACCEL_GENERATED` is clear it returns 7 here
  and reads nothing else;
- `palette_headers[r]` — 8 B at `chunk_base + 4120 + 8r`;
- for a paletted region, a `voxel_malloc` blob — a third address in a different buffer entirely;
- up to `uniformity_bits[0..2]` — 12 B at `chunk_base + 8`.

**So a chunk record is 8216 B and a single visit reads 12–24 of them, split across two cache lines
about 4 KB apart, and consecutive chunks along a ray are 8216 B apart so they never share a line.**
`SCALE_LIMITS.md` §3.3 makes the same observation; the numbers below quantify it.

### 2.2 Chunk-record dereferences per frame — measured

The engine already exports the exact quantity needed: `trace_primary.comp.glsl:192-211` writes a
step-count heatmap with `R = step_n / 1024`, `G = prepass steps / 1024`, `B` classifying
miss / hit / ran-out-of-steps, **plus a 0..1 calibration ramp in the bottom 4 rows** so the
swapchain's transfer function can be inverted out of the PNG rather than assumed. Since one step is
one chunk-record dereference, summing the decoded step counts over the frame *is* the touch count.

Captured at three poses with `VOXL_DEBUG_PASS='voxel step count'`, HUD off, 1280×720, uncontended
GPU (§7 has the commands). Decoded with `scratchpad/decode_steps.py`; the ramp came back monotone
and spanning 0..255 in all three, so the inversion is sound. [MEASURED]

| pose | sky (miss) | hit | out-of-steps | steps/px main | steps/px prepass | **chunk-record touches / frame** |
|---|---:|---:|---:|---:|---:|---:|
| vista (island, horizon) | 2.67 % | 97.33 % | **0.0000 %** | mean 6.0, p95 29.6, max 306.6 | mean 23.7, max 130.5 | 5.49 M + 5.43 M = **10.92 M** |
| sky (25 m up, near level) | 62.96 % | 37.04 % | **0.0001 %** | mean 31.2, p95 66.5, max 511.6 | mean 16.7 | 28.55 M + 3.83 M = **32.38 M** |
| cave (inside the tunnel) | 0.00 % | 100 % | **0.0013 %** | mean 2.2, p95 5.6 | mean 9.4 | 2.06 M + 2.16 M = **4.22 M** |

*(prepass runs at half resolution, `PREPASS_SCL 2`, and its count is replicated across the four
full-res pixels that read it, so the prepass column is divided by 4 before summing.)*

Four things fall out of this table.

**The step budget never binds.** Maximum main-trace step count across 916 480 pixels is **306.6**
against a budget of 512, and out-of-steps pixels are 0.0000–0.0013 % — the residue is a handful of
pixels, not a hole. This is an independent confirmation, from a different instrument, of the brief's
measured fact #1 (512 → 2048 is byte-identical). **A ray in this scene is not reach-limited.**

**A miss ray touches an order of magnitude more chunk records than a hit ray.** At vista, miss
p50 = 66.5 steps against hit p50 = 1.6. This is the brief's measured fact #2 seen from the memory
side rather than the timing side: *the expensive ray is expensive because it walks the table.*

**The sky pose does three times the table traffic of the vista pose** (32.4 M vs 10.9 M) while being
the cheaper frame overall. So the chunk table is *most* stressed by exactly the frames the profiler
calls cheap.

**And the traffic is enormous.** At two cache lines per touch, vista is **1.40 GB/frame** of chunk
table reads and sky is **4.14 GB/frame** — for primary visibility alone, before GI, shadow,
reflection or irradiance-cache rays, which are 53.6 % of the frame. 1.40 GB in a 10.36 ms frame is
135 GB/s; the sky figure is several times any plausible DRAM bandwidth for this part. **The GPU's L2
must be absorbing most of it**, which leads directly to §2.5.

### 2.3 Distinct chunks — the frustum bound

Touch *count* is not working-set *size*. For the distinct count I ran an exact chunk-granularity DDA
over a full 1280×720 ray bundle from the box centre (the player sits at the centre —
`trace.glsl:97` adds `chunk_n × CHUNK_WORLDSPACE_SIZE × 0.5`), 74° vertical FOV
(`voxel_app.cpp:79`), through an **empty** box, and counted distinct chunk indices.
[DERIVED, `scratchpad/frustum_chunks.py`]

| CPA | box | table | primary-frustum distinct chunks | **% of table** |
|---:|---|---:|---:|---:|
| 16 | 64 m | 4 096 | 876 | **21.4 %** |
| 32 | 128 m | 32 768 | 5 964 | **18.2 %** |
| 64 | 256 m | 262 144 | 43 740 | **16.7 %** |

**The fraction does not fall with world size.** It converges on the frustum's solid angle —
2.02 sr / 4π = 16.0 % — which is the sanity check that the DDA is right. The reason is the one §2.2
just measured: *there is no reach limit inside the near volume.* 512 steps at a 4 m chunk stride
carries a ray 2048 m; the box diagonal at CPA 64 is 443 m. **Growing the world does not put any of
it out of the marcher's reach.**

This is the result that disciplines the design, and it is the opposite of the intuition in the
brief. The touched fraction is small only if something *stops* rays: occlusion, or a coarser
representation at distance. The far field is the second of those and it already exists.

### 2.4 Distinct chunks — the content bound

The other bound is content, and the engine already measures it. `log_table_census()`
(`voxel_world.cpp:147-191`) walks the CPU mirror the moment generation completes. Reproduced this
session at the shipped configuration [MEASURED]:

```
[scale] table census: 4096 chunks | uniform 3787 (92.5%) | header-only 0 (0.0%) |
        paletted 309 (7.5%) | paletted regions 35709 of 2097152 (1.70%)
[scale] WORLD GENERATED: 4096 chunks in 38 frames, 1.11 s
```

That reproduces `SCALE_LIMITS.md` §3.3's CPA 16 row (3783 / 313 / 36 506) to within run-to-run
generation noise, so the census is trustworthy and the earlier numbers stand. **309 of 4096 chunks
hold any data at all**, and at CPA 64 it is 847 of 262 144.

> A pose caveat that cost me a run and will cost the next person one too. `--pos` takes
> **absolute** world metres; every pose in `SCENE.md` and `SCALE_LIMITS.md` §9.1 is **scene-local**,
> and `local = absolute + (183, 110, 52.5)` (`SCENE.md:25-28`). Passing the local triple directly
> puts the player 210 m from the island in empty water. The run still exits 0, still writes a
> screenshot, and the census reports `uniform 4096 (100.0%) | paletted 0` — which reads exactly
> like a broken CPU mirror. It is not; it is an empty near volume, and the 52 MB of heap in that
> run was the far field's. This is measurement trap (a) wearing a new hat.

### 2.5 What that means for an LRU cache, in one paragraph

**The bodies and the directory have to be answered separately, and they give opposite answers.**

*The bodies* — the palette blobs, the 49.9 % of each record that is `page_allocation_infos`, and the
49.9 % that is `palette_headers` — are needed by **7.5 %** of chunks at CPA 16 and **0.3 %** at CPA
64, and the absolute count is flat in world size. A pooled, LRU-managed body store is straightforwardly
correct, and §3 shows most of it is already built.

*The directory* — whatever answers "which chunk is at this index, and is it empty" — is hit by
**10.9 M to 32.4 M dereferences per frame** [MEASURED] spread over **17–21 % of the table at any
world size** [DERIVED], and effectively all of it once secondary rays are included. **An LRU cache
over a flat directory would thrash**: the miss rate would be set by the frustum's solid angle, not
by locality, and every miss would be a stall inside the innermost loop of the most expensive pass in
the engine.

There is a hardware number that makes this concrete. At CPA 16 the *touched* part of the whole
chunk table is `4096 chunks × 2 cache lines × 64 B = 524 KB` [DERIVED] — it fits several times over
in this GPU's L2 (GA107-class, on the order of 2 MB; **specification, not measured here**). That is
almost certainly why a dense 33.7 MB table costs nothing today: **the hardware is already caching
it, for free, with no code.** At CPA 64 the same figure is 33.5 MB — well past L2 — and the engine
would be paying a DRAM round trip per DDA step. **The dense table is not slow because it is small
enough to hide. The cache work is what happens when it stops being small enough.**

So the shape of the answer is: **make the directory small enough to stay resident and hardware-cached
at any world size (which means hierarchical, not flat), and put the bodies in an LRU pool.** That is
precisely GigaVoxels' node-pool / brick-pool split — [PAPER] §4.1 — and §3 is how close voxl2
already is.

---

## 3. What already resembles GigaVoxels

| GigaVoxels | citation | voxl2 today | how close |
|---|---|---|---|
| N³-tree of nodes, each holding a **brick pointer or a constant value** | [PAPER] §4 | `PaletteHeader` — `variant_n < 2` means the region is uniform and `blob_ptr` **is the voxel value inline**, no indirection (`voxels.glsl:156-158`) | **Identical idea, one level down.** The paper puts it on tree nodes; voxl2 puts it on 8³ palette regions. 98.3 % of regions take this path [MEASURED] |
| **Node pool** — nodes grouped in N³ blocks, one pointer per block, kept in a 3-D texture | [PAPER] §4.1 | `voxel_chunks`, a **flat dense 3-D array** indexed by `calc_chunk_index()` (`voxels.glsl:94-101`). No pointers, no blocks, no tree | **This is the gap.** O(1) lookup, zero traversal cost — and no way to represent "this whole region is absent" in less than a full record each |
| **Brick pool** with LRU replacement | [PAPER] §6.1, [LATER] | `VoxelMallocPageAllocator` — a GPU-resident page pool with an available-page stack and a released-page stack (`allocator.inl`, `voxel_malloc.glsl`), `malloc`/`realloc`/`free` callable from compute shaders | **Very close. It is a pool with a free list; it is not an LRU.** Nothing ever evicts, because nothing is ever absent |
| Multi-resolution: coarser data further away | [PAPER] §4, §6 | The far field: L1 at 25 cm over 256 m beside L0 at 6.25 cm over 64 m, **same struct, same shaders, `VOXEL_LEVEL` define** (`far_field.inl`) | **Two levels of the tree, hand-unrolled.** The engine already proves nested volumes work; it just has no general mechanism for level *n* |
| Empty-space skipping via the hierarchy | [PAPER] §4, §5.1 | The uniformity pyramid: `uniformity_bits` + per-palette bits, `sample_lod()` returns an LOD 0..7 | **Present but capped at one chunk.** The pyramid ends at 64 voxels = 4 m; GigaVoxels' ends at the root. This is exactly the brief's measured fact #1 |
| **Ray-guided feedback**: rays report which nodes they used, GPU-side | [PAPER] §6.1.1 | **Nothing.** No pass records which chunks a ray touched | Missing entirely. §6 says how to build it |
| Feedback **compaction** before readback (selection mask + HistoPyramid) | [PAPER] §6.1.2 | n/a | Missing. Also probably unnecessary here — see below |
| Fixed **upload budget** per frame for stable frame rate | [PAPER] §6 | `MAX_CHUNK_UPDATES_PER_FRAME = 128`, enforced by `try_elect()` dropping work silently (`voxel_world.comp.glsl`) | **Already there, same shape, same failure mode** |
| Timestamps in **host memory**, CPU decides placement | [PAPER] §6.1 | `VoxelWorld::begin_frame` already runs a per-frame CPU mirror update over 128 chunk records | The machinery for a CPU-side manager exists — **and should not be used**, see below |
| Constant/homogeneous nodes avoid marching entirely | [PAPER] §4 | `sample_lod()` early-outs on `variant_n < 2` and on `!ACCEL_GENERATED` | Present |

**Two observations that change the plan.**

**(1) The pool is built; the tree is not.** The brief guessed that "some of the work may already be
done and merely not organised as a cache". That is right, and it is specifically the *brick pool*:
`DECL_SIMPLE_ALLOCATOR` already gives a GPU-resident, growable, shader-callable pool with a free
list, and `voxel_malloc.glsl` already does page-level sub-allocation with a 24-slot bitfield per
2112 B page. Adding an LRU discipline to it is a smaller job than it looks. What is *not* built is
any hierarchy above one chunk, and that is the thing that makes a big world cheap.

**(2) The paper's CPU-managed LRU is the wrong half to copy.** [PAPER] §6.1 keeps timestamps on the
host and transfers modifications with texture updates; §6.1.2 spends a whole subsection on
compacting feedback because the readback bandwidth would otherwise be prohibitive on 2009 hardware.
voxl2 is **97.5–99.4 % GPU-bound** (brief, measured fact #3), so a per-frame readback-and-decide
loop puts the decision on the one processor that has spare time and the wrong latency. The [LATER]
GigaVoxels/GigaSpace formulation — pool management in compute kernels, no host round trip — is the
one that fits, and daxa's task graph plus the existing indirect-dispatch pattern
(`voxel_world.cpp:663-670`) already supplies the plumbing for it.

---

## 4. What would have to change

Split hard, because the brief asks for it and because the two halves have different risk.

### 4.1 A layer beside the existing one — incremental, reversible

None of these touch `calc_chunk_index()` or the marcher's arithmetic.

| change | files | why it is cheap |
|---|---|---|
| **Touch feedback.** Record, per frame, which chunk indices were dereferenced | `voxels.inl` (one `daxa_u64` on `VoxelWorldGlobals`), `voxels.glsl` (`sample_lod`), `voxel_world.inl` (one attachment on `VoxelWorldPerframeCompute` only), `perframe.comp.glsl`, `voxel_world.cpp` | The far field already proved the pattern: publish a device address through `VoxelWorldGlobals` rather than a push constant, because the widest task head has **8 bytes of a 128-byte push constant left** (`voxels.inl:73-97`). §6 specifies it |
| **LRU discipline on `voxel_malloc`.** Age pages, evict cold ones, refuse growth instead of growing | `allocator.inl`, `voxel_malloc.glsl` | The free list and the per-frame hook (`VoxelMallocPageAllocator_perframe`) already exist. The cap machinery (`max_element_count`, `growth_refusals`) already exists and already logs |
| **Shrink the heap floor.** 553.6 MB of safety margin for an unchecked `atomicAdd` | `voxel_malloc.inl:132`, `allocator.inl:284` | Bounds-check the GPU allocator and the margin can collapse. Independent of everything else here, and it is the single largest VRAM line in the engine |
| **Third far-field level.** L2 at 1 m over 1 km | `far_field.*` | 33.65 MB, and the mechanism is already generalised over `VOXEL_LEVEL` |

### 4.2 Replacing the chunk table — a rewrite, and the one that matters

The change is: `voxel_chunks[i]` stops being an 8216 B record and becomes an 8 B directory entry
holding a tag plus either an inline uniform voxel value or an index into a pooled body.
`SCALE_LIMITS.md` §3.3 already costed the two-tier version at **2.6 MB / 7.3 MB / 9.1 MB** for CPA
16 / 32 / 64 against 33.7 / 269.2 / 2153.8 MB dense.

Every site that would have to change:

| site | file:line | what it does now |
|---|---|---|
| `sample_lod` (both overloads) | `voxels.glsl:190`, `:242` | `deref(voxel_chunk_ptr).flags`, `.palette_headers[r]`, `.uniformity_bits[i]` |
| `sample_voxel_chunk` (both overloads) | `voxels.glsl:152`, `:162` | `.palette_headers[r]` |
| `sample_temp_voxel_chunk` | `voxels.glsl:170` | `.update_index` |
| `VoxelUniformityChunk_lod_nonuniform_{16,32,64}` | `voxels.glsl:81-89` | `.uniformity_bits[index]` |
| chunk election | `voxel_world.comp.glsl:57`, `:80` | `CHUNKS(i).flags` read/clear |
| `ChunkOpt` x2x4 / x8up | `voxel_world.comp.glsl:895` | writes `.flags`, `.uniformity_bits` |
| `ChunkAlloc` | `voxel_world.comp.glsl:987-1044` | reads and writes `.palette_headers[r]`, calls malloc/realloc/free against `.sub_allocator_state` |
| `VoxelMalloc_*` | `voxel_malloc.glsl` | **every one takes a `voxel_chunk_ptr` and indexes `page_allocation_infos[512]` inside the record** |
| CPU mirror update | `voxel_world.cpp:468-493` | writes `CpuVoxelChunk::palette_chunks[512]` |
| CPU sampling | `voxel_world.cpp:97-125` | `VoxelWorld::sample()` for collision |
| startup clear | `voxel_world.cpp:308-313`, `far_field.cpp:61-64` | `clear_buffer` over the whole table |

**The hard one is `VoxelMalloc`.** `page_allocation_infos[512]` is 4096 B of *per-chunk allocator
state* living inside the record, and `voxel_malloc.glsl` reaches into it on every malloc, realloc
and free. A chunk cannot become 8 bytes until that state moves into the body — which is the right
place for it, since only chunks with content ever allocate, but it means the allocator and the
directory have to be redesigned together rather than one after the other.

**And the L1 table comes along for free**, because it is the same struct behind the same shaders.

### 4.3 What must not change

- `LOG2_VOXEL_SIZE` stays at −4. The directory change is orthogonal to it.
- The uniformity pyramid's *contents* — the change is where the bits live, not what they mean.
- `CHUNKS_PER_AXIS` must stay a power of two and a multiple of 8; the `#error` at
  `voxel_malloc.inl:73` is load-bearing and the failure it prevents is silent
  (`SCALE_LIMITS.md` §2).

---

## 5. The edit path, traced

What happens today when the player holds a mouse button. Every frame, not every stroke.

1. **`VoxelWorldPerframeCompute`** (1 thread, `perframe.comp.glsl`) clears all 128
   `chunk_update_infos` and all 128 `chunk_updates[].info.flags`, resets `chunk_update_n` and
   `chunk_update_heap_alloc_n`, advances `offset`/`prev_offset`, reseeds the three indirect
   dispatches, runs `VoxelMallocPageAllocator_perframe`, publishes the heap's consumed page count to
   `gpu_output`, and then **traces a full `voxel_trace()` from the mouse position** to find the
   brush point (`perframe.comp.glsl:52-56`).
2. **`PerChunkCompute`** (one thread per chunk, `CHUNKS_DISPATCH_SIZE³` workgroups) elects chunks.
   For an edit it builds the **axis-aligned bounding box of the swept capsule** between `prev_pos`
   and `pos`, inflated by the radius, and calls `try_elect()` for every chunk in it
   (`voxel_world.comp.glsl:100-120`). `try_elect()` `atomicAdd`s the three indirect dispatch
   counters and appends to `chunk_update_infos`, **dropping work silently past 128**. Radius is
   clamped to 8 m and the stroke shortened so `2r + stroke ≤ 16 m`, which is what keeps the box
   inside the budget.
3. **`ChunkEditCompute`** regenerates **all 64³ = 262 144 voxels** of every elected chunk into
   `temp_voxel_chunks`, running the *whole* world generator plus the brush — not a delta.
4. **`ChunkEditPostProcessCompute`**, then **`ChunkOptCompute` ×2** (x2x4 and x8up) rebuild the
   uniformity pyramid for those chunks and write `flags = CHUNK_FLAGS_ACCEL_GENERATED`.
5. **`ChunkAllocCompute`** (`voxel_world.comp.glsl:958-1084`) runs one workgroup of 8³ threads per
   palette region — **512 workgroups per elected chunk**. Per region it re-votes the palette, then:
   - `palette_size > 367`: store raw. If the region was already paletted, **`VoxelMalloc_realloc`**;
     else `VoxelMalloc_malloc`.
   - `1 < palette_size ≤ 367`: compressed. Same realloc-or-malloc.
   - `palette_size == 1`: uniform. **If the region was paletted, `VoxelMalloc_free`**, and the
     header's `blob_ptr` becomes the voxel value inline.
   Then, under `#if VOXEL_LEVEL == 0` only, it writes the CPU-mirror record: `info.flags = 1`,
   `info.chunk_index`, and all 512 `palette_headers`, into
   `chunk_updates[frame_index × 128 + temp_chunk_index]`, and the blob bytes into
   `chunk_update_heap`.
6. **`VoxelWorld::begin_frame`** on the CPU, `FRAMES_IN_FLIGHT + 1 = 2` frames later, reads that
   slot, and for every record with `flags == 1` **`delete[]`s and `new[]`s every one of the 512
   palette blobs** into `CpuVoxelChunk` (`voxel_world.cpp:468-493`). This is the only reason
   `chunk_update_heap` is 268.4 MB of host-visible memory, and the only consumer is player
   collision.

**Five properties a cache design has to answer to.**

- **An edit is a full chunk regeneration, at 64³ voxels and 512 palette regions, per chunk, per
  frame held.** There is no partial write anywhere in the path.
- **The heap already handles the promote/demote transitions** — a region that becomes uniform is
  freed, a region that becomes complex is realloc'd. **That is exactly the eviction/insert edge of
  an LRU, already written and already correct.**
- **The edit path is the only writer of the CPU mirror,** and world generation uses the same path,
  so a cache that changes how chunk data reaches the CPU breaks player collision.
- **The far field takes no edits at all**, by a compiled-out branch with a comment explaining that
  leaving it in would be worse than a no-op (`voxel_world.comp.glsl:1085-1096`). Any cache spanning
  both levels inherits the question the far field deferred: propagating a 6.25 cm edit up to 25 cm
  needs a downsample pass that does not exist.
- **Under a cache, an edited chunk is a dirty cache line.** Today "regenerate it" and "make it
  resident" are the same operation, because everything is always resident. Once they are different
  operations, the edit path needs a *pin* — an edited chunk must not be evicted between the frame
  the brush writes it and the frame the CPU mirror reads it back, or collision silently reads a
  stale or recycled body.

---

## 6. The measurement I could not make, and exactly how to make it

**What is missing: the distinct set of chunk indices dereferenced per frame, across all eight
`voxel_trace()` call sites.** §2.3 bounds it geometrically for primary rays through an empty box and
§2.4 bounds it by content, but neither is the measured union under real occlusion with GI, shadow
and irradiance-cache rays included — and that union is the number that sizes the pool.

I did not build it because it needs a C++ rebuild, and building it inside `C:\voxl2` would have
meant holding edits in five files shared with agents working in parallel. The project's own practice
is to measure in a copied tree (`SCALE_LIMITS.md` §1 used `C:\vsc` for exactly this reason); copying
the 4.2 GB tree was not available to me in this session. **Recommend the next session start with
this probe, in a copy, before anything else.** It is about 40 lines.

Design, following the far field's proven pattern:

1. `voxels.inl` — add `daxa_u64 touch_probe_addr;` to `VoxelWorldGlobals` **immediately after
   `ff_voxel_chunks_addr`**, so both 8-byte members sit at the front where C++ and daxa's scalar
   layout cannot disagree about padding (the reasoning is already written out at
   `voxels.inl:73-97`). **Costs zero push-constant bytes**, which is mandatory: the widest task head
   is at 120 of 128, and overflowing it presents as a *silently missing pass*, not an error.
2. `voxel_world.cpp` — one `TemporalBuffer` of `4 × CHUNKS_PER_AXIS³` bytes (16 KB at CPA 16) with
   `HOST_ACCESS_RANDOM`.
3. `voxel_world.inl` — one `DAXA_TH_BUFFER_PTR(COMPUTE_SHADER_READ_WRITE, ...)` on
   **`VoxelWorldPerframeCompute` only** (that head has room), which both publishes the address into
   `VoxelWorldGlobals` and clears the buffer.
4. `voxels.glsl` — in `sample_lod()`'s outer overload, under `#if VOXL_TOUCH_PROBE`, an
   `atomicAdd` of 1 at `chunk_index` through a `daxa_RWBufferPtr(daxa_u32)` built from the address.
   Millions of atomics per frame will wreck the frame time; that does not matter, because the
   quantity wanted is a count, not a duration. Take timings from a build with the probe off.
5. `voxel_world.cpp::begin_frame` — read the buffer at the same `FRAMES_IN_FLIGHT + 1` lag as
   `chunk_updates` and log distinct-nonzero, sum, and the top-*k* histogram.

Report it at the three poses in §2.2 plus one moving pose, and at CPA 16 and CPA 32. **The single
number that decides the whole design is the ratio of `sum` to `distinct` — the reuse factor.** If
it is large, an LRU pool has a high hit rate and the indirection is affordable; if it is near 1,
every dereference is a cold touch and no cache helps.

Two smaller unknowns worth closing at the same time:

- **The L2 claim in §2.5 is a specification figure, not a measurement.** Confirm GA107's L2 size,
  or better, measure the knee directly: sweep `CHUNKS_PER_AXIS` on an uncontended GPU with the
  island held at constant size and look for the frame-time discontinuity where the touched table
  stops fitting in L2. `SCALE_LIMITS.md` §10 says explicitly that its own frame-time column cannot
  answer this because three to six sibling engines were on the card.
- **The far field's contribution to the touch count is not in §2.2 at all.** The heatmap is written
  by `trace_primary.comp.glsl` from `result.step_n`, and `trace.glsl:272` deliberately clamps the
  far segment's steps out of that total so a far-field ray is not mistaken for a hole. So every
  number in §2.2 is L0 traffic only, and L1's is unmeasured.

---

## 7. Method — the commands, so every number above is reproducible

Environment for every run: `VOXL_DATA_DIR` set to a fresh per-run directory (measurement trap (a));
explicit working directory `C:\voxl2` (trap (d) — launching from elsewhere aborts at `0x80000003`
with an empty stderr); GPU verified uncontended at 265 MiB / 6144 MiB used and no other
`gvox_engine.exe` process (trap (b)); every image opened and read (trap (c)).

**§1.2, §2.4 — the census, the heap, the frame time:**

```powershell
Start-Process -FilePath "C:\voxl2\.out\cl-x86_64-windows-msvc\Release\gvox_engine.exe" `
  -WorkingDirectory "C:\voxl2" -Wait `
  -ArgumentList "--unpause","--exit-after","14","--width","960","--height","540",
                "--no-overlay","--bench-csv","<out>.csv" `
  -RedirectStandardOutput "<out>.log"
```

No `--pos`: the default spawn *is* the vista anchor, absolute `(-182.990, -109.980, -46.970)`.
Gives the `[scale]` census, `[heap]` budget line, and the per-frame CSV. Steady state over the last
1000 frames of the 1280×720 vista run: **p05 9.43 / p50 10.36 / p95 10.77 ms**, heap 52.01 MB used
against 1660.94 MB capacity, flat from frame 35 to frame 2319.

**§2.2 — the step-count heatmaps:**

```powershell
$env:VOXL_DEBUG_PASS = "voxel step count"
# vista: -182.99,-109.98,-46.97  rot 0.785,1.096
# sky:   -183.0,-110.0,-27.5     rot 0.785,1.62
# cave:  -170.0,-97.0,-49.3      rot 0.785,1.45
Start-Process ... -ArgumentList "--unpause","--exit-after","10",
  "--screenshot","<out>.png","--screenshot-after","8","--width","1280","--height","720",
  "--pos",<pos>,"--rot",<rot>,"--no-overlay","--set","UI/show_tool_hud=false",
  "--render-scale","1.0","--gi","true","--reflections","false","--shadows","true"
```

`--set UI/show_tool_hud=false` is **required**: `--no-overlay` suppresses only the F3 overlay, and
the game HUD (`ui_tools.cpp:176`) draws an opaque 542×126 panel plus a crosshair over the heatmap.
Left on, it contributes ~455 saturated pixels that decode as 1024 steps against a 512 budget, and
it inflated the first pass of this measurement's out-of-steps figure from 0.0000 % to 0.0496 % and
the vista touch count from 10.92 M to a spurious 10.49 M-plus-artefacts. **The HUD-on figures in an
earlier draft of this file were wrong; the table in §2.2 is the HUD-off re-run.**

Decoders, both in the session scratchpad and both self-contained:
`decode_steps.py` (inverts the calibration ramp, classifies by the B channel, sums R and G/4) and
`frustum_chunks.py` (the chunk-granularity frustum DDA of §2.3).

---

## 8. What this document does not establish

1. **The distinct-chunk working set under real occlusion with all ray types.** §6. This is the gap.
2. **Any far-field traffic figure.** §2.2 is L0 only, by construction (`trace.glsl:272`).
3. **Whether the L2 story in §2.5 is the actual mechanism.** It is consistent with the traffic
   figures and with the dense table costing nothing today, but the knee has not been measured.
4. **Frame time versus `CHUNKS_PER_AXIS`.** Not attempted here; `SCALE_LIMITS.md` §10 explains why
   its own attempt does not answer it either.
5. **Whether an 8-byte directory is faster.** `SCALE_LIMITS.md` §3.3 argues it should be, from
   cache-line reasoning that §2.1 here confirms structurally. Nobody has built it and measured it.
6. **The provenance of the "1 MB pool / 64 GB" figures in the brief.** Not in the 2009 paper; not
   found on the GigaVoxels project page. Someone should locate them in Crassin's thesis before they
   are used as a target.
7. **Whether `VoxelMalloc`'s per-chunk allocator state can move into a pooled body without
   restructuring the allocator.** §4.2 flags it as the hard part; it was not attempted.
