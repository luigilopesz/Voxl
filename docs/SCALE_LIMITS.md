# Scale limits — how big can this world be, and what breaks first

**Written:** 2026-08-01
**Hardware:** RTX 3050 6 GB Laptop (6144 MiB board; the driver reports a 6293 MB DEVICE_LOCAL
heap), i7-13650HX, 15.7 GB RAM, Windows 11 Pro 26200.
**Question asked:** `CHUNKS_PER_AXIS` is 16, giving a 64 m cube. Push it until something gives,
and say what gave and at what number.

---

## 0. The short answer

Four walls. They arrive in an order nobody would have guessed, and the first one is not a
resource limit at all.

| # | wall | arrives at | mechanism | § |
|---|---|---|---|---|
| 1 | **`CHUNKS_PER_AXIS` must be a power of two** | **24** — the very first step past 16 | one `&` where a `%` was meant, in chunk election. 70.4 % of the world is never generated, the player stands in the missing part, and **every capture at 24 and at 48 is empty sky and sea**. Exit code 0, nothing logged | §2 |
| 2 | **the chunk table exhausts VRAM** | works at **64** (2153.8 MB table, 3790 MiB process); crawls at **80**; past the card at **96**; dies at **128** | 8216 B per chunk resident whether the chunk holds rock or air, **plus another 8192 B per chunk in host RAM** that had never been counted. Process VRAM = 1785 MiB + 8216 × CPA³, to within 1.3 % | §3, §5 |
| 3 | **generation time** | usable to **64** (2672 frames, ~41 s uncontended); unusable at **80** (4000 frames at ~110 ms each) | `MAX_CHUNK_UPDATES_PER_FRAME` is 128, so generation is `CPA³/128` **frames** — a floor no GPU speed can move. Measured 0.65 s → 5.10 s → ~41 s at CPA 16 → 32 → 64 | §4 |
| 4 | the voxel **heap** cap | **never reached, at any world size** | the heap's floor is set by `MAX_CHUNK_UPDATES_PER_FRAME`, not by the world: 830 MB resident for 12.6–47 MB of voxels. Forced deliberately, a refusal is either completely harmless or a silent `0x80000003`, and once produced 4.6 MB of out-of-bounds GPU writes and a screenshot full of grey slabs | §6 |

**The single most valuable number here:** at `CHUNKS_PER_AXIS 64` the dense chunk table occupies
**2153.8 MB to hold 9.1 MB of information** — 99.7 % of its chunks are uniform. A directory-plus-pool
layout is **238× smaller** and should also be *faster*, because marching through air would stop
touching two cache lines 4 KB apart. §3.3.

**The recommendation: `CHUNKS_PER_AXIS 32`.** It costs +235 MB of table and +209 MiB of process
VRAM over 16, generates in 5.1 s, and doubles the island from 37 m to 80 m because `VOXL_ISLAND_R`
uncaps at 40 m. **64 is affordable but pointless today**: it costs 2.1 GB of VRAM, 2.1 GB of host
RAM and two minutes of startup to render *the same island*, because the scene generator caps the
content at a 40 m radius. `docs/images/scale/cpa32-vista.png` and `cpa64-vista.png` are the same
picture.

---

## 1. The rig, and why it is not `C:\voxl2`

Every number here was produced in **`C:\vsc`**, a copy of `C:\voxl2` in which every file belonging
to another agent was reverted to `HEAD` and only two files differ from it:
`voxels/impl/voxel_malloc.inl` (the `CHUNKS_PER_AXIS` under test) and `voxels/impl/voxel_world.cpp`
(the instrumentation described in §7).

This was not fastidiousness. The first three runs of this sweep, taken in the shared tree, produced:
one `0x80000003` abort with an empty stderr; one 445-second hang whose log contained
`GLSLANG [trace_primary.comp.glsl] ERROR: 'step_count_image_id' : no such field in structure`,
because a sibling agent had edited the shader but not yet the header; and one run whose VRAM peaked
at **5985 of 6144 MiB with an empty world**, because three other engine instances were on the card
at the time. `C:\voxl2\src` was being written to roughly once a minute for the whole session.

Two of this project's three measurement traps therefore bit within the first ten minutes, and the
third — a broken contention check — bit immediately after:

- **Trap (a), settings.** Handled: `VOXL_DATA_DIR` is set to a fresh per-run directory, and every
  run also pins `--render-scale 1.0 --gi true --reflections false --shadows true` on the command
  line. The effective settings are echoed into every log:
  `Graphics: Quality Preset=Balanced | Render Res Scale=1 | Render Shadows=on | TAA Method=Kajiya TAA | Update Sky=on | denoise_shadow_mask=off | global_illumination=on | ray_traced_reflections=off`
- **Trap (b), a second engine.** The obvious check does not work on this machine.
  `nvidia-smi --query-compute-apps=used_memory` returns `[N/A]` for every process on this driver,
  so a harness that sums it reports "0 MiB of other GPU clients" while four engines are running.
  **Count the processes instead.** Every row below carries `engines`, the maximum number of
  `gvox_engine.exe` processes seen at 2 Hz for the whole run, and 1 means "only mine".
- **Trap (c), a silent shader failure.** Every screenshot in this document was opened.
- **Trap (d), the working directory.** Every run passes one explicitly.

**Frame time could not be measured cleanly.** Three to five sibling engines ran continuously for the
entire session; the harness waits up to 60 s for a free GPU and then proceeds, because waiting for
zero never returned. One run reached a **p50 of 518 ms** — the documented "two instances thrash 6 GB
into 500 ms frames", reproduced exactly. Frame times below are therefore given as **p05 as well as
p50**, with the engine count beside them, and §5 re-measures the ones that matter in an uncontended
window. **The memory, generation and failure-mode results are unaffected by contention** and are the
substance of this document: the heap budget is derived from the device's total VRAM rather than its
free VRAM, generation is counted in frames, and per-process memory is read from the WDDM
`\GPU Process Memory(pid_N_*)\Dedicated Usage` counter rather than from the whole-device figure.

---

## 2. Wall 1: `CHUNKS_PER_AXIS` must be a power of two — and 24 is silent, total content loss

**This is the first thing that breaks, it breaks at the very first step past 16, and it does not
announce itself.**

`voxels/impl/voxel_malloc.inl:18` says the constant "must stay a multiple of 8 because
`CHUNKS_DISPATCH_SIZE` below divides by 8." That is necessary but not sufficient. Chunk election is

```glsl
// src/voxels/impl/voxel_world.comp.glsl:44
terrain_work_item.i = ivec3(gl_GlobalInvocationID.xyz) & (chunk_n - 1);
```

`x & (n-1)` is `x % n` only when `n` is a power of two. At `CHUNKS_PER_AXIS 24`, `chunk_n - 1` is
`23 = 0b10111`, and the dispatch covers `gl_GlobalInvocationID.x ∈ [0, 23]`:

| x | 0–7 | 8–15 | 16–23 |
|---|---|---|---|
| `x & 23` | 0–7 | **0–7 again** | 16–23 |

Slices 8–15 of every axis are **never elected and never generated**, and slices 0–7 are elected
twice per dispatch, wasting half the per-frame chunk budget on duplicates.

**Measured, exactly as predicted:**

```
[scale] CHUNKS_PER_AXIS 24 -> 13824 chunks, world 96 m cube, view radius 48 m
[scale] generating: 4096/13824 chunks (29.6%), frame 13056, t=59.29 s
```

4096 is 16³. Generation plateaus at **29.6 %** and stays there forever; the world never finishes
generating. The same arithmetic at CPA 48 leaves slices 16–31 of 0–47 unreachable and stops at
32³ = 32768 of 110592 — again 29.6 %.

**And the failure is worse than "some chunks missing", because of where the player stands.** The
volume is centred on the player, so the player sits at chunk 12 of 24 in each axis — inside the dead
band. Every capture at CPA 24 renders **nothing at all**:

| capture | what I saw when I opened it |
|---|---|
| `docs/images/scale/cpa24-vista.png` | sky and sea. The island, the tree, the cave hill: absent |
| `docs/images/scale/cpa24-close.png` | the camera is inside the cave tunnel at CPA 16 and 32. At 24 it is sky and sea |
| `docs/images/scale/cpa24-sky.png` | sky and sea |

The engine exits 0. Nothing is logged. The heap reports 6.88 MB in use rather than 13 MB — the only
hint, and only if you were looking for it.

**Legal values are 8, 16, 32, 64, 128** — power of two *and* a multiple of 8. The comment in
`voxel_malloc.inl` has been corrected and a `static_assert`/`#error` added so the next person is
told at compile time instead of discovering it in a screenshot.

**A second latent trap in the same place:** `voxels/impl/voxel_world.cpp:52` is
`assert(chunk_index < 50000)`, which is a no-op in Release (`/DNDEBUG`) but fires at any
`CHUNKS_PER_AXIS >= 40` in a Debug build (40³ = 64000). A Debug build of a large world aborts inside
`calc_chunk_index` for a reason that has nothing to do with the world being large.

---

## 3. Wall 2: the chunk table, which is resident whether or not the world contains anything

### 3.1 What it costs, and the half of it nobody had counted

`sizeof(VoxelLeafChunk)` is **8216 B** (`voxels/impl/voxels.inl:32`):

| field | bytes | share |
|---|---:|---:|
| `flags`, `update_index`, `uniformity_bits[3]` | 20 | 0.24 % |
| `VoxelMalloc_ChunkLocalPageSubAllocatorState` — `u64 page_allocation_infos[512]` | 4096 | 49.9 % |
| `PaletteHeader palette_headers[512]` | 4096 | 49.9 % |

**There is a second copy of the same size in host RAM.** `VoxelWorld::voxel_chunks` is a
`std::vector<CpuVoxelChunk>` resized to `CHUNKS_PER_AXIS³` in `record_startup`, and `CpuVoxelChunk`
is `std::array<CpuPaletteChunk, 512>` = **8192 B per chunk**. It scales identically and it had never
been named in any document in this project. **The real per-chunk cost of an empty world is 16408 B,
not 8216.**

| CPA | world edge | view radius | chunks | table (VRAM) | CPU mirror (host) | both |
|---:|---|---|---:|---:|---:|---:|
| 8 | 32 m | 16 m | 512 | 4.2 MB | 4.2 MB | 8.4 MB |
| **16** *(shipped)* | 64 m | 32 m | 4 096 | **33.7 MB** | 33.6 MB | 67.3 MB |
| 24 | 96 m | 48 m | 13 824 | 113.6 MB | 113.2 MB | 226.8 MB |
| **32** | 128 m | 64 m | 32 768 | **269.2 MB** | 268.4 MB | 537.6 MB |
| 48 | 192 m | 96 m | 110 592 | 908.6 MB | 905.9 MB | 1814.5 MB |
| **64** | 256 m | 128 m | 262 144 | **2153.8 MB** | 2147.5 MB | 4301.3 MB |
| 80 | 320 m | 160 m | 512 000 | 4206.6 MB | 4194.3 MB | 8400.9 MB |
| 96 | 384 m | 192 m | 884 736 | 7269.0 MB | 7247.8 MB | 14516.8 MB |
| 128 | 512 m | 256 m | 2 097 152 | 17230.2 MB | 17179.9 MB | 34410.1 MB |

The VRAM column is logged by the engine at startup and matched the arithmetic to the digit at every
value measured.

### 3.2 The other Vulkan limits, so they are not confused with this one

From `vulkaninfo` on this device: `maxStorageBufferRange = 4294967295` (4 GiB − 1),
`maxMemoryAllocationSize = 0xffffffffffffffff` (unbounded), `maxBufferSize = 0x10000000000` (1 TiB).
The chunk table is reached through a buffer *device address*, not a bound descriptor range, so the
4 GiB figure is not the binding constraint — but it is the one that would bite first if anything in
the engine ever bound it as a plain storage buffer, and it corresponds to **CPA 80**.

### 3.3 What the table actually contains — the census

The table is dense because `calc_chunk_index()` is an O(1) 3-D array lookup and `sample_lod()`
dereferences it once per DDA step, in the three most expensive passes in the renderer. The question
is not whether an index is needed but **how many chunks need any of the 8216 bytes.**

`log_table_census()` (added to `voxels/impl/voxel_world.cpp`) walks the CPU mirror once, the moment
generation completes, and classifies every chunk. At **CPA 64**, with the full 80 m island
generated:

```
[scale] table census: 262144 chunks | uniform 261297 (99.7%) | header-only 0 (0.0%) |
        paletted 847 (0.3%) | paletted regions 105535 of 134217728 (0.08%)
[scale] dense table 2153.8 MB -> pooled equivalent 9.1 MB (237.8x smaller); directory alone 2.10 MB
```

**99.7 % of chunks are completely uniform** — every one of their 512 palette regions holds the same
single value, almost always air. They need one word. **0.3 % — 847 chunks — hold the entire island**
and need the full record. Only **0.08 % of palette regions** are paletted at all.

**2153.8 MB of VRAM is being spent to store 9.1 MB of information.**

A three-tier layout costs, at any world size:

```
directory  = 8 B x CHUNKS_PER_AXIS^3         <- tag + inline uniform value or pool index
body pool  = 8216 B x (chunks with content)  <- only the 0.3%
```

**Measured at every world size that generates** — three separate runs, the census printed by the
engine itself:

| CPA | chunks | uniform | paletted | paletted regions | dense table | directory | **pooled total** | vs dense |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 16 | 4 096 | 3 783 (**92.4 %**) | 313 (7.6 %) | 36 506 of 2 097 152 (1.74 %) | 33.7 MB | 0.03 MB | **2.6 MB** | **12.9×** |
| 32 | 32 768 | 31 912 (**97.4 %**) | 856 (2.6 %) | 103 404 of 16 777 216 (0.62 %) | 269.2 MB | 0.26 MB | **7.3 MB** | **36.9×** |
| 64 | 262 144 | 261 297 (**99.7 %**) | 847 (0.3 %) | 105 535 of 134 217 728 (0.08 %) | 2153.8 MB | 2.10 MB | **9.1 MB** | **237.8×** |
| 128 *(extrapolated)* | 2 097 152 | — | ~850 | — | 17230.2 MB | 16.8 MB | **~24 MB** | **~718×** |

**The paletted chunk count barely moves between CPA 32 and CPA 64 — 856 then 847 — because the
content is identical.** `VOXL_ISLAND_R` in `brushes.glsl` is `min(40.0, VOXL_WORLD_HALF - 11.0)`,
so the island stops growing at CPA 32 and everything beyond that is empty volume. The dense table
grew 8× for it; a pooled table would have grown by 1.84 MB of directory. **The dense table charges
for the box; a pooled table charges for the contents.**

Note also that `header-only` is zero at every size: in this scene a chunk either has real content
or is uniform throughout, so the middle tier is not needed and the simpler two-tier
directory-plus-body layout captures the whole saving.

**It would very likely also be faster, and this is the part that is not obvious.** A DDA step in
empty space today touches `flags` at chunk_base+0 and `palette_headers[r]` at chunk_base+4116+8r —
two cache lines, 4 KB apart, and consecutive chunks along a ray are 8216 B apart so they never share
a line. With an 8-byte directory the whole of a CPA 64 world's directory is **2.10 MB — it fits in
L2** — eight neighbouring chunks share one 64-byte line, and a uniform chunk resolves with no second
load at all. The engine spends most of its time marching through air (`PERFORMANCE_PLAN.md` §3.3: a
ray that misses is 3.2× the cost of one that hits), which is precisely the case that gets cheaper.

**Cost to build, not built:** the directory and pool are a change to `VoxelLeafChunk`, to
`calc_chunk_index`'s two call sites in C++ and GLSL, to `sample_lod`/`sample_voxel_chunk`, and to
the chunk-alloc pass that promotes a chunk from uniform to paletted (and demotes it back). It needs
a GPU-side pool allocator for the bodies — but one already exists and is trivially reusable:
`DECL_SIMPLE_ALLOCATOR`, which is what `VoxelMallocPageAllocator` is built from. It does **not**
touch the marcher's arithmetic, the palette compression, the uniformity pyramid, or any brush.

---

## 4. Wall 3: generation is `CPA³ / 128` **frames**, and nothing can make a frame cheaper

`PerChunkCompute` elects at most `MAX_CHUNK_UPDATES_PER_FRAME` = **128** chunks per frame
(`voxel_malloc.inl:47`, `voxel_world.comp.glsl:28`), and at startup every chunk in the table has
`CHUNK_FLAGS_ACCEL_GENERATED` clear, so all of them are elected. **World generation is therefore
`CHUNKS_PER_AXIS³ / 128` frames, and it is a hard floor that no GPU speed can move.**

| CPA | chunks | floor (frames) | measured (frames) | best measured (s) |
|---:|---:|---:|---:|---:|
| 16 | 4 096 | 32 | **33** | **0.59–0.68** |
| 32 | 32 768 | 256 | **259–262** | **5.10** |
| 64 | 262 144 | 2 048 | **2 672** | 121.4 *(4 engines on the card)*; **~41 s** by CPA³ scaling |
| 80 | 512 000 | 4 000 | — | reached 4.2 % in 30 s |
| 96 | 884 736 | 6 912 | — | did not reach frame 256 in 30 s |
| 128 | 2 097 152 | 16 384 | — | never started, §5 |

The measured frame count tracks the floor to within 30 %, and the measured *time* tracks `CPA³`
almost exactly: 0.65 s → 5.10 s is **7.8× for 8× the chunks**. **Generation time is a startup cost
paid before the first usable frame,** and the CPA 64 world took two minutes to appear on a
contended card. That is the wall that is a *different kind of problem* — it is not memory, and the
answer to it is streaming or a coarser far field, not a smaller table.

Two things follow immediately:

- **`MAX_CHUNK_UPDATES_PER_FRAME` is the only knob.** Raising it to 512 would quarter the frame
  count, at 4× the transient `temp_voxel_chunks_buffer` (135 MB → 540 MB) and 4× the
  `chunk_update_heap` (268 MB → 1074 MB), both of which are already large and neither of which
  scales with the world. That trade has never been measured.
- **A world that regenerates from scratch every launch does not scale.** There is no persistence,
  so every launch pays the full `CPA³/128` frames.

---

## 5. Wall 4: the hard stop, and it is silent

Short runs at increasing `CHUNKS_PER_AXIS`, everything else fixed, 1280×720:

| CPA | table | what happened |
|---:|---:|---|
| 64 | 2153.8 MB | **works.** Generates fully, renders, exits 0. Process VRAM 3790 MiB of 6144 |
| 80 | 4206.6 MB | starts, exits 0, but crawls: **frame 256 at t = 28.07 s ≈ 110 ms/frame**, 4.2 % generated after 30 s. Generation would need ≈ 7 minutes |
| 96 | 7269.0 MB — **larger than the whole card** | **starts and exits 0 anyway.** WDDM satisfies the allocation by paging to system RAM. Did not reach frame 256 in 30 s. This is the "succeeds by paging, and every voxel trace walks over PCIe" outcome that `allocator.inl` warns about, and there is no diagnostic of any kind |
| 128 | 17230.2 MB | **dies.** Exit code **`0x80000003`**, empty stderr, empty log after the size line, no C++ exception. Silent |

**The hard stop is CPA 128, and it is exactly the silent 0x80000003 this project has already been
bitten by twice.** A `try`/`catch` was added around the chunk-table allocation for this experiment
and **did not fire** — Daxa aborts on a failed `create_buffer` rather than throwing, so there is
nothing to catch. The last thing in the log is the size that was being asked for, which is only
there because this work added it.

**The practical stop is much earlier: CPA 80.** Process VRAM tracks the table exactly —

```
process dedicated VRAM (MiB) = 1785 + 8216 x CPA^3 / 2^20
```

measured 1818 / 2027–2037 / 2637 / 3790 MiB at CPA 16 / 32 / 48 / 64 against 1817 / 2042 / 2652 /
3839 predicted, i.e. within 1.3 % at every point. That puts CPA 80 at **5796 MiB of a 6144 MiB
card** — 94 %, with the desktop still to pay for — and CPA 96 at 8717 MiB, 1.42× the card.

**Host RAM is the wall nobody had counted, and it arrives at the same time.** At CPA 64 the process
holds **3768 MiB of working set and 6883 MiB of private commit** on a 15.7 GB machine; at CPA 48 it
is 2155 / 4730 MiB. The CPU mirror is most of it.

---

## 6. The allocator cap: it cannot be reached, and when it is, it is not visible

The VRAM cap added to `utilities/allocator.inl` was written defensively after a device-lost, and
**this sweep did not reach it once**. Zero growth refusals and zero throttles at every
`CHUNKS_PER_AXIS` from 16 to 64, at every pose. The reason is arithmetic:

- The heap's **initial** capacity is `(FRAMES_IN_FLIGHT + 1) × PALETTES_PER_CHUNK ×
  MAX_CHUNK_UPDATES_PER_FRAME` = `2 × 512 × 128` = **131 072 pages = 278 MB**, and it is not
  derived from the world at all — only from the per-frame chunk budget.
- The **first** growth step takes it to **393 216 pages = 830 MB**, because
  `check_for_realloc()`'s geometric step is `(current + headroom) × 3/2` and the headroom is
  itself 131 072. This happens as soon as the GPU consumes a single page, and it never grows
  again in this scene.
- The island consumes **13 MB at CPA 16 and 40–47 MB at CPA 32–64** — about 6 100 to 22 000 pages
  against a capacity of 393 216 and a cap of 1 955 198.

**The heap is between 18× and 64× larger than the world needs, at every world size, and the
over-allocation is set by `MAX_CHUNK_UPDATES_PER_FRAME` rather than by the world.** 830 MB of a
6 GB card is spent on a safety margin for 40 MB of voxels. That is a bigger single line item than
the chunk table at every `CHUNKS_PER_AXIS` up to 48.

**Heap usage does not scale with `CHUNKS_PER_AXIS`** — it scales with *content*, and the content
stops growing at CPA 32 because `VOXL_ISLAND_R` in `brushes.glsl` is
`min(40.0, VOXL_WORLD_HALF - 11.0)`, i.e. capped at a 40 m radius. Raising `CHUNKS_PER_AXIS` past
32 therefore buys **empty volume only**. That is a property of the test scene, not of the engine,
and it is why the table cost and the heap cost separate so cleanly in this sweep.

### 6.1 So it was provoked on purpose

`VOXL_HEAP_BUDGET_MB` (added to `allocator.inl`) forces the computed budget down to a chosen figure.
Two pairs, each a control and a treatment differing only in that variable, `CHUNKS_PER_AXIS 16`,
same pose, same build within each pair:

| case | `MAX_CHUNK_UPDATES_PER_FRAME` | budget | capacity | heap **used** | refused | outcome |
|---|---:|---:|---:|---:|---|---|
| **capC** control | 128 (shipped) | none (4145 MB) | 393 216 pages / **830.5 MB** | 12.67 MB | no | correct world, p50 **9.544 ms** |
| **capD** treatment | 128 (shipped) | 300 MB | 131 072 pages / **276.8 MB** | 12.61 MB | **yes** | **correct world**, p50 **9.453 ms**, images pixel-comparable |
| **capA** control | 4 | none | 12 288 pages / 25.95 MB | 14.5 MB | no | correct world, generated in 1055 frames |
| **capB** treatment | 4 | 10 MB | 4 096 pages / 8.65 MB | 7.8 MB | **yes** | **died at `0x80000003`, 24 % generated, no image, empty stderr** |
| *cap2*, an earlier pass | 2 | 6 MB | 2 048 pages / **4.33 MB** | **8.96 MB — 2.07× the capacity** | yes | **kept running, exit 0**, and rendered `docs/images/scale/cap2-refused-exhausted.png` |

**Three findings, in order of how much they should change what happens next.**

**1. A refusal is not an error, and the message says the wrong thing.** capD refused on frame 1 and
then ran the whole scene correctly at the same frame time as the control, in **554 MB less VRAM**.
The heap is so far oversized that being denied growth costs nothing. But the log line says

> `GROWTH REFUSED. 131072 elements (278 MB) is the cap; 12 more would need 556 MB resident at once
> against a 315 MB budget. The world is larger than this card.`

**"The world is larger than this card" is false** — the world is using 12.6 MB of a 276.8 MB
capacity. What was actually refused is the *safety headroom*, the
`current + MAX_ELEMENT_ALLOCATIONS_PER_FRAME × (FIF+1)` margin that keeps the GPU's unchecked
`atomicAdd` in bounds. Those are different events and the diagnostic conflates them. **Anyone
reading that line would conclude the world was too big and shrink it, which is the wrong action.**

**2. When the cap actually bites, the engine does not degrade — it fails, and it fails silently,
and not the same way twice.** capB died at `0x80000003` with an empty stderr and no image. cap2, at
an even tighter budget, did the opposite: it survived, wrote a screenshot, exited 0, and reported
**heap usage of 8.96 MB against a 4.33 MB capacity**. That is 4.6 MB of GPU writes past the end of
the buffer, and the reason nothing caught them is in `utilities/allocator.glsl`: `malloc()` clamps
against `UserMaxElementCount`, which is `VOXEL_MALLOC_MAX_PAGE_COUNT` = `CPA³ × 512` = **2 097 152
pages at CPA 16 — the compile-time bound, not the runtime capacity of 2048.** The GPU has no idea
the buffer is smaller than that.

**`docs/images/scale/cap2-refused-exhausted.png`, which I opened**, is what that looks like: the
hillside is sliced open by huge flat featureless grey planes, a black wedge stands in front of the
cave portal, and the landform is cut through by surfaces that are not terrain. It is unmistakable
as *damage* rather than as a rendering setting — but **nothing in the engine says so.** Exit code 0,
a screenshot written, 9.17 ms a frame.

> **A cap that turns into invisible missing terrain is barely better than the crash it replaced —
> and this one turns into visible garbage, an abort, or nothing at all, depending on the run.**
> The single change that fixes it is the one `allocator.glsl` already records as an integration
> note: give `malloc()` the *runtime* capacity instead of the compile-time bound. That is one
> field in the allocator struct, which is a shared CPU/GPU layout, and it is the difference
> between a bounded, detectable failure and this.

**3. `MAX_CHUNK_UPDATES_PER_FRAME` sets the heap floor, and the coupling is enormous.** capA, with
the knob at 4 instead of 128, ran the identical world on a **25.95 MB** heap instead of 830 MB —
**32× less VRAM for the same voxels** — at the cost of 1055 generation frames instead of 33. The
830 MB is not the world; it is `(FIF+1) × PALETTES_PER_CHUNK × MAX_CHUNK_UPDATES_PER_FRAME`
compounded by one geometric growth step. **There is a real trade curve here between startup time
and resident VRAM that nobody has plotted, and it is worth more VRAM than the chunk table is at
every `CHUNKS_PER_AXIS` below 64.**

---

## 7. What was added, and what it prints

Three files, all owned by this work.

**`src/voxels/impl/voxel_world.cpp`**

| output | when | why |
|---|---|---|
| `[scale] CHUNKS_PER_AXIS N -> M chunks, world X m cube, view radius Y m` | startup | the world's actual extent, which had to be derived by hand every time |
| `[scale] chunk table: 8216 B/chunk x M = Z MB VRAM ... CPU mirror 8192 B/chunk = W MB host RAM` | startup, **before** the allocation | so a run that dies inside `create_buffer` still says what it was asking for. This is the only diagnostic that exists at CPA 128 |
| `[scale] world generation floor: M chunks / 128 per frame = F frames minimum` | startup | states the `CPA³/128` floor before a frame has run |
| `[scale] generating: a/b chunks (p%), frame f, t=s` | every 256 frames until complete | makes a world that never finishes generating diagnosable. This is how the power-of-two bug was caught |
| `[scale] WORLD GENERATED: ... in F frames, S s` | once | generation cost, in frames (contention-proof) and seconds |
| `[scale] table census: ... / dense table A MB -> pooled equivalent B MB (Cx smaller)` | once | §3.3 |
| a `World Gen` row in the F3 overlay | every frame | — |
| `try`/`catch` around the chunk table and the CPU mirror | — | catches the host-side failure. Does **not** catch the GPU one; see §5 |

**`src/utilities/allocator.inl`** — `VOXL_HEAP_BUDGET_MB` overrides the computed VRAM budget, so
the refusal path in `check_for_realloc()` can be provoked deliberately. Unset, nothing changes.

**`src/voxels/impl/voxel_malloc.inl`** — the "multiple of 8" comment corrected to "power of two
**and** a multiple of 8", with the mechanism, the measured consequence, and an `#error` that fails
the build instead of the screenshot. The measured VRAM ceiling is recorded beside it.

---

## 8. What to do, in order

1. **Land the `#error`.** It is three lines and it prevents the single most expensive mistake
   available here — a silently empty world that exits 0.
2. **Decide `CHUNKS_PER_AXIS` from this table, not from intuition.** 32 is free (269 MB table,
   +209 MiB of process VRAM over 16, 262 frames of generation, and it doubles the island to 80 m
   because `VOXL_ISLAND_R` uncaps at 40 m). **32 is the recommendation and it costs almost
   nothing.** 64 works and gives a 256 m box, but costs 2.1 GB of VRAM, 2.1 GB of host RAM and
   two minutes of generation for a world whose *content* does not grow at all.
3. **Do not raise `CHUNKS_PER_AXIS` to chase distant mountains.** §3.3 and §4 together say the
   dense table and the `CPA³/128` generation floor both charge for the box rather than the
   contents, and the far-field ladder in `PERFORMANCE_PLAN.md` §5.3 gets 16× the reach per level
   at a *constant* table size. This sweep is evidence for that plan, not against it.
4. **The sparse chunk table is the single highest-value memory change available** and §3.3 costs
   it: 238× at CPA 64, and it should make marching through air *faster* as well as smaller. It
   is what makes an L2 or L3 far-field level affordable at `CHUNKS_PER_AXIS` 32 rather than 16.
5. **Shrink the heap's fixed floor.** 830 MB for 40 MB of voxels, set by
   `MAX_CHUNK_UPDATES_PER_FRAME` and the `(current + headroom) × 3/2` step, not by the world.
6. **Give `create_buffer` failure a voice.** The engine's response to "the table does not fit" is
   `0x80000003` with an empty stderr. That is the third time this project has met that exit code.
7. **Close the `malloc()` bounds gap in `utilities/allocator.glsl`** — it clamps against the
   compile-time `VOXEL_MALLOC_MAX_PAGE_COUNT` rather than the runtime capacity, which is the only
   reason a refused growth turns into out-of-bounds writes rather than a bounded failure. That
   file already carries the note; §6.1 is the evidence that it matters.

---

## 9. The sweep

### 9.1 The command

Every row was produced by `scratchpad/scale_sweep.ps1`, which patches
`#define CHUNKS_PER_AXIS` in `src/voxels/impl/voxel_malloc.inl` (BOM-free — `Set-Content -Encoding
utf8` on PowerShell 5.1 writes one, and this header is `#include`d by every shader), rebuilds,
waits for a free GPU, and then runs:

```
gvox_engine.exe --unpause --exit-after <S> --screenshot docs\images\scale\<row>.png
    --screenshot-after <S-4> --width 1280 --height 720
    --pos <absolute X,Y,Z> --rot <yaw,pitch> --no-overlay --bench-csv <row>.csv
    --render-scale 1.0 --gi true --reflections false --shadows true
```

with `VOXL_DATA_DIR` set to a fresh directory per row, `nvidia-smi -lms 250` streaming whole-device
VRAM, and a 2 Hz sampler recording the engine count, this process's
`\GPU Process Memory(pid_N_*)\Dedicated Usage`, and its working set. Poses, in `docs/SCENE.md`
scene-local metres:

| pose | local position | rot (yaw, pitch) | what it frames |
|---|---|---|---|
| `vista` | `0.01, 0.02, 5.53` | `0.785, 1.096` | the documented spawn anchor: tree, cave hill, horizon |
| `close` | `13, 13, 3.2` | `0.785, 1.45` | inside the cave tunnel, looking at the emissive crystals |
| `sky` | `0, 0, 25` | `0.785, 1.62` | 25 m up, near level: almost every ray misses |
| `aerial` | `0, 0, 70` | `0.785, 1.15` | 70 m above the island. **Read its heap figure, not its image** — at 24° below horizontal from 70 m the frame centre lands ~157 m out, past the island, so the picture is sea at every CPA. The number that matters is how much world exists at all up there: **0 MB of heap at CPA 16, 0.92 MB at CPA 32** |

### 9.2 Per configuration

Chunk-table and generation columns are contention-proof. `eng` is the peak number of
`gvox_engine.exe` processes seen during the run; **1 means uncontended**, and only two rows in the
whole sweep achieved it.

| CPA | world | chunks | table MB | CPU mirror MB | gen floor | gen frames | best gen s | heap pages | heap used MB | refusals | proc VRAM MiB | proc host MiB |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 16 | 64 m | 4 096 | 33.7 | 33.6 | 32 | **33** | **0.65** | 393 216 | 12.8–13.0 | 0 | **1818** | 801–1336 |
| 24 | 96 m | 13 824 | 113.6 | 113.2 | 108 | **never** (4096/13824) | — | 393 216 | 6.9–8.2 | 0 | 1892 | 981–1966 |
| 32 | 128 m | 32 768 | 269.2 | 268.4 | 256 | **259–262** | **5.10** | 393 216 | 39.2–46.8 | 0 | **2027–2037** | 980–1445 |
| 48 | 192 m | 110 592 | 908.6 | 906.0 | 864 | **never** (32768/110592) | — | 393 216 | 9.0 | 0 | **2637** | 2155 (4730 commit) |
| 64 | 256 m | 262 144 | 2153.8 | 2147.5 | 2048 | **2672** | 121.4 *(4 engines)* | 393 216 | 40.0 | 0 | **3790** | 3768 (6883 commit) |
| 80 | 320 m | 512 000 | 4206.6 | 4194.3 | 4000 | 4.2 % in 30 s | — | — | — | — | ~5796 *(fitted)* | — |
| 96 | 384 m | 884 736 | 7269.0 | 7247.8 | 6912 | did not reach frame 256 in 30 s | — | — | — | — | 4201 *(capped, paging)* | 6199 (16168 commit) |
| 128 | 512 m | 2 097 152 | 17230.2 | 17179.9 | 16384 | **never starts** | — | — | — | — | — | — |

**Frame time, and read the caveat in §1.** p05 and p50 over the settled window, with the engine
count. Only the `eng 1` rows deserve to be quoted; the rest are recorded so the sweep is auditable.

| CPA | pose | eng | p05 ms | p50 ms |
|---:|---|---:|---:|---:|
| 16 | vista | 2 | 8.98 | 9.50 |
| 16 | close (cave) | 4 | 8.35 | 8.81 |
| 16 | sky | 3 | 4.79 | 6.32 |
| 16 | aerial (**empty world**) | 3 | 11.13 | 13.76 |
| 32 | vista | 4 | 12.71 | 16.53 |
| 32 | close (cave) | 3 | 8.98 | 9.55 |
| 32 | sky | 3 | 6.33 | 6.69 |
| 48 | vista (**empty world**) | 3 | 6.30 | 6.65 |
| 64 | vista | 4 | 20.15 | 30.23 |

An earlier, less contended pass of CPA 16 gave **vista 9.548 / close 8.690 / sky 6.107 ms** at p50,
against the project's documented 10.463 ms stock and 10.200 ms reflections-off at this resolution —
consistent. **No conclusion about how frame time scales with `CHUNKS_PER_AXIS` should be drawn from
this sweep.** The honest statement is that at CPA 16→32 the island doubles in size and the vista
frame gets more expensive, and that everything above that was measured on a GPU shared with three
to six other engines.

### 9.3 Screenshots, and what I saw in each

| file | what is in it |
|---|---|
| `docs/images/scale/cpa16-vista.png` | the 37 m island: rock dome with the documented black holes, conifer, cave portal with amber crystal glow, flowered meadow, sea horizon close in |
| `cpa16-close.png` | inside the cave tunnel; walls washed warm amber by bounce from the crystals. GI intact |
| `cpa16-sky.png` | sky and sea, a sliver of rock at the bottom edge |
| `cpa16-aerial.png` | sky and sea — but see the pose caveat above; the load-bearing figure is that **heap usage was 0 MB and the heap never grew past its 131 072-page floor.** From 70 m up at CPA 16 the world is genuinely, entirely empty: the volume is ±32 m about the player and the island is 56–70 m below |
| `cpa32-aerial.png` | also sea. Heap usage 0.92 MB — at CPA 32 the top few metres of the hill do exist up there, but the pose does not frame them |
| `cpa24-vista.png`, `cpa24-close.png`, `cpa24-sky.png` | **all three are empty sky and sea.** §2 |
| `cpa32-vista.png` | the 80 m island — meadow now runs to both edges of frame, and noticeably fewer black holes in the dome than at 16 |
| `cpa32-close.png` | the same cave interior, GI intact |
| `cpa32-sky.png` | sky and sea with the meadow at the bottom edge |
| `cpa48-vista.png` | **empty sky and sea.** §2 |
| `cpa64-vista.png` | the 80 m island again, complete and correct, no missing chunks and no corruption. **It is the same picture as `cpa32-vista.png`**, for 8× the table |
| `capC-shipped-control.png` / `capD-shipped-capped.png` | §6.1. Indistinguishable — 830 MB of heap versus 277 MB, with the second one refusing growth on frame 1 |
| `capA-control.png` | `MAX_CHUNK_UPDATES_PER_FRAME 4`, no cap: the correct island on a **25.95 MB** heap instead of 830 MB |
| **`cap2-refused-exhausted.png`** | **the damage.** Heap usage 8.96 MB against a 4.33 MB capacity. The hillside is sliced open by enormous flat featureless grey planes, a black wedge stands in front of the cave portal, and the landform is cut through by surfaces that are not terrain. Exit code 0, 9.17 ms a frame, screenshot written, nothing logged but one line |
| `cap3-control-mcupf2.png` | its control. Also incomplete, but for a different and innocent reason — at `MAX_CHUNK_UPDATES_PER_FRAME 2` the world needs 2048 frames to generate and the run only reached 762, so the hill has a wedge of *air* where the grey slabs are in `cap2` |
| `final-shared-tree-check.png` | the last check, run from `C:\voxl2` itself with everything landed: the island renders, exit code 0, and the `[scale]` lines print. The coarse brown terrain on the horizon is the far-field agent's L1 shell, not this work |

---

## 10. What this does not establish

1. **Frame time versus world size.** Three to six sibling engines were on the card for the entire
   session and one row read a p50 of 518 ms. The memory, generation and failure results are
   contention-proof; the frame times are not, and §9.2 says so per row.
2. **Whether the CPA 64 world is *playable*.** It generates and renders correctly, but the only
   uncontended figure for it is the generation frame count. There is no clean frame time and no
   moving soak.
3. **What a bigger world would cost with bigger content in it.** `VOXL_ISLAND_R` caps the island at
   a 40 m radius, so every measurement above CPA 32 grew the box and not the contents. The heap and
   the frame time would both move if the island grew with the world; the table and the generation
   floor would not.
4. **Whether the pooled table is as fast as it looks.** §3.3's cache argument is arithmetic on the
   struct layout, not a measurement. It needs the change built to settle.
5. **Why a refusal sometimes aborts and sometimes corrupts.** capB died and cap2 survived with
   damage, on the same mechanism at different budgets. Both are unacceptable, so this was not
   pursued further.
6. **`CHUNKS_PER_AXIS` 8.** Not measured; the table would be 4.2 MB and the world a 32 m cube.
7. **The rig is still on disk at `C:\vsc`** (5.8 GB, `.out` included) so every row here can be
   re-run. It is a copy, not a worktree, and nothing depends on it.
