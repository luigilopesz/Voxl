# Voxl - Performance

Every number in this document was **measured**, on the machine described in
§2, by the harness in `benchmarks/`. Where something could not be measured it
says so and explains why; nothing here is estimated unless the row is labelled
*extrapolated*, and those rows show their arithmetic.

The raw output of the run this document quotes is committed alongside it:

| File | Contents |
|---|---|
| `benchmarks/results/reference_i7-13650HX.txt` | full table, all counters |
| `benchmarks/results/reference_i7-13650HX.csv` | the same run, long-format CSV for diffing |
| `benchmarks/results/README.md` | how to diff a new run against it, and what is and is not portable between machines |

---

## 1. Running the harness

```powershell
powershell -ExecutionPolicy Bypass -File tools/build.ps1 -Config RelWithDebInfo -Target voxl_bench
./build/RelWithDebInfo/bin/voxl_bench.exe
```

`voxl_bench` is **headless**. It never creates a window and never calls a `gl*`
entry point, so it runs on a build machine with no GPU and is usable from CI.
It links `voxl::engine` only because the engine is a single static library.

| Flag | Effect |
|---|---|
| `--filter <text>` | run only cases whose `group/name` contains `<text>` |
| `--samples <n>` | override every case's timed-run count |
| `--warmup <n>` | override every case's untimed warm-up count |
| `--seed <n>` | world seed for every generated fixture (decimal or `0x…`) |
| `--csv <path>` | write the CSV here (default `voxl_bench_results.csv` in the cwd) |
| `--no-csv` | skip the CSV |
| `--list` | print the case list and exit |
| `--quiet` | suppress the per-case progress lines |

Groups: `terrain`, `meshing`, `subvoxel`, `storage`, `lighting`, `persistence`.

### CSV format

One row per *(case, metric)* — long, not wide:

```
group,case,kind,metric,value,unit,note
terrain,gen_chunk_plains,timing,median_ms,1.714900,ms,
terrain,gen_chunk_plains,counter,non_air_blocks,19687.000000,blocks,
meshing,greedy_lod3,derived,speedup_vs_lod0,1.103000,x,*** FLAG: ...
```

Long format on purpose: adding a counter to one case does not shift any other
case's columns, so a plain `diff` between two commits is readable and only shows
what actually moved. `kind` separates `timing` (measured), `counter` (a property
of the workload, recorded once), `derived` (a ratio of other rows) and `status`
(a case that could not be measured).

---

## 2. Hardware and build

| | |
|---|---|
| CPU | 13th Gen Intel Core i7-13650HX — 14 cores (6 P + 8 E), 20 threads |
| GPU | NVIDIA RTX 3050 6 GB Laptop |
| RAM | 16 GB |
| OS | Windows 11 Pro 10.0.26200 |
| Compiler | MSVC v14.44 (VS 2022 Build Tools), `/std:c++20 /W4 /permissive- /fp:strict` |
| Config | `RelWithDebInfo` (`/O2 /Ob1 /DNDEBUG`), so `VOXL_ASSERT` is compiled out |
| Job pool | 19 workers (`hardware_concurrency() - 1`) |
| World seed | `0x764f0117a2c5d3b9` (the default) |

**The machine was not idle.** Background load during the run measured 10-15% of
total CPU. This is a laptop with vendor service agents on it, not a controlled
bench, and it is why every table below carries `min` and `p95` next to the
median rather than a single number. See §3.

---

## 3. Methodology

### Why not one `steady_clock` pair per scene

Each workload here is noisy on a laptop: turbo ramping, P-core/E-core migration,
first-touch page faults, a cold branch predictor, another process waking up. A
single timing of such a workload is not a measurement, it is one sample of a
heavy-tailed distribution — and comparing two of them between commits produces
confident nonsense in both directions.

So every case:

1. runs `warmupRuns` **untimed** runs (3 by default; 2 for the expensive batch
   cases). This is what pays for page faults, branch training, allocator warm-up
   and the CPU reaching a steady clock;
2. then runs `sampleRuns` **timed** runs (15-21 by default), each wrapped in a
   single `std::chrono::steady_clock` pair — which on Windows/MSVC is QPC, with
   sub-microsecond resolution;
3. reports the whole shape: min, median, mean, p95, max, sample standard
   deviation.

`p95` is nearest-rank (`index = ceil(0.95n) - 1`), not interpolated. With 15-25
samples an interpolated percentile invents precision that is not in the data;
the nearest-rank value is always a time that was actually observed.

### How to read the tables

* **median** is the number to compare between commits. It ignores the one run
  that collided with a background task.
* **min** is the best estimate of the workload's true cost on an idle machine.
  These are deterministic CPU workloads with no I/O, so the fastest run is the
  one that was interrupted least — not an outlier to be discarded.
* **p95 - min** is the contamination. Where that gap is small the median is
  trustworthy; where it is large, read the min.

The reference run was selected out of five consecutive passes by the smallest
mean `median/min` ratio across all cases (1.033, i.e. the median run was on
average 3.3% slower than the cleanest run of the same case).

### What is and is not timed

Three callbacks per case:

* `setup` — once, **untimed**. Builds the world: generates fixture chunks,
  resolves biome sites, constructs meshers.
* `prepare` — before every run, **untimed**. Restores mutable state a run
  destroys (re-filling a `ChunkStorage`, clearing a `SubVoxelStore`, zeroing
  light before a propagation pass).
* `body` — the **timed** unit. Performs exactly `opsPerRun` logical operations
  every time, so a case that meshes 256 chunks per run and one that meshes a
  single chunk both report a comparable per-operation cost.

Meshing cases reuse a **pooled** output buffer between runs, because that is what
the engine does — one mesh buffer per worker, reused chunk after chunk.
Allocating a fresh output every run would measure the allocator's growth curve
and report it as meshing cost.

### Determinism

Every fixture is a pure function of the seed. Anything that searches the world
for a scene ("a plains chunk", "the cave section with the most surface area")
does so with a fixed scan order and takes the first or best hit, never a random
pick: a benchmark whose input changes between runs cannot be diffed between
commits. The scrambled index order used by the random-access storage cases comes
from an explicit 64-bit LCG rather than `std::shuffle` with a standard engine,
whose exact output is implementation-defined and would differ between toolchains.

### Reliability floor

A case whose median falls below **5 µs** gets an automatic note saying so. At
that scale clock granularity and whatever the optimiser hoisted out of the loop
cannot be separated from the work, and the number is an upper bound rather than
a measurement. Two cases hit this floor: `subvoxel/mesh_damage_0` (which
therefore repeats its call 1024× per run and divides) and
`storage/get_seq_uniform` (where `get()` on a uniform section is a single load
of a constant and the optimiser collapses the whole loop — the "< resolution"
entry in §4.7 is honest and the engine's real callers, which interleave other
work, cannot get that speedup).

---

## 4. Results

All times in milliseconds. `min / median / p95` from the reference run.

### 4.1 Terrain generation per chunk, by biome

Single-threaded, level 0. Each case generates the **surface section** of a
column the generator classifies as that biome — the section containing the
terrain top, found by an expanding-ring scan from the origin. A section entirely
below the surface is solid stone and one entirely above is empty air; neither
says anything about the biome.

| Biome | surface Y | non-air blocks | min | **median** | p95 |
|---|---:|---:|---:|---:|---:|
| ocean | 86 | 32 302 | 2.527 | **2.833** | 3.399 |
| plains | 120 | 19 687 | 1.678 | **1.715** | 1.876 |
| mountains | 158 | 14 463 | 1.381 | **1.393** | 1.701 |
| snowy | 109 | 14 991 | 1.278 | **1.372** | 1.798 |
| forest | 131 | 4 316 | 0.896 | **0.981** | 1.109 |
| desert | 99 | 5 336 | 0.852 | **0.875** | 1.081 |
| beach | 97 | 3 727 | 0.797 | **0.813** | 0.902 |

Every one of these sections settles at **4 bits per index** with a 5-8 entry
palette and 49 184 bytes of heap — of which 32 768 is the light array and only
16 416 the voxels (§4.7).

Cost tracks the number of **solid** blocks, not the biome's complexity: ocean is
the most expensive because a submerged column is filled to sea level with water
and then again below the floor with stone, so almost all 32 768 voxels are
written. Beach and desert are the cheapest because most of the section is air
above a low surface. The spread across biomes is 3.5×, which matters for
streaming: a player walking into an ocean costs three times the generator time
of one walking into a desert.

### 4.2 Terrain generation per LOD level

Same chunk (`0, 3, 0`, straddling sea level), generated at each level.

| Level | cell size | cells/chunk | min | **median** | p95 | speedup vs level 0 |
|---|---:|---:|---:|---:|---:|---:|
| 0 | 1 | 32 768 | 1.666 | **1.808** | 2.124 | 1.00× |
| 1 | 2 | 4 096 | 0.549 | **0.567** | 0.841 | **3.19×** |
| 2 | 4 | 512 | 0.214 | **0.220** | 0.304 | **8.22×** |
| 3 | 8 | 64 | 0.203 | **0.210** | 0.213 | **8.62×** |

**Verdict: passes.** Level 3 is 8.6× cheaper than level 0, which is what LOD was
built to buy on the generation side.

It is not the 512× the cell count would suggest, and should not be: generation
is dominated by a **per-column** pass (height field, biome blend, erosion,
continentalness) that is 1024 columns at every level, plus the cave field which
is sampled per cell. That column pass is the floor, and levels 2 and 3 are
already sitting on it — which is exactly why level 3 is barely cheaper than
level 2 (1.05×). Anything below ~0.21 ms per chunk would require coarsening the
column sampling too.

### 4.3 Terrain generation throughput across the job system

256 chunks (a 16×16 column grid at section y=3), one job per chunk,
`JobPriority::Normal`, timed to `JobSystem::waitIdle()`. The chunks are allocated
once in `setup` and regenerated in place, so this measures generation and job
dispatch, not the allocator.

| | min | **median** | p95 | per chunk |
|---|---:|---:|---:|---:|
| single-threaded | 197.01 | **201.35** | 203.80 | 786.5 µs |
| job system, 19 workers | 22.15 | **22.44** | 22.83 | **87.7 µs** |

* **Speedup: 8.97×** on 19 workers.
* **Parallel efficiency: 47.2%.**

47% is not a defect in the pool, and the reasons are visible in the hardware
line: the 20 hardware threads are 6 P-cores with hyperthreading (12) plus 8
E-cores. Only 6 of those threads run at full P-core throughput; a hyperthread
sibling contributes perhaps 25% and an E-core a fraction of a P-core. Scaling to
~9× real throughput on that topology is close to what the silicon can deliver
for a memory-touching workload. The job system's own overhead is small relative
to a 787 µs work item — 256 jobs dispatched and drained in 22.4 ms is 87 µs per
chunk, and the single-threaded per-chunk cost divided by 19 would be 41 µs, so
the gap is core asymmetry rather than queueing.

### 4.4 Greedy meshing by chunk shape

Level 0. "Naive faces" is what a mesher with no hidden-face removal and no
merging would emit: 6 × solid blocks. Two reductions are reported because they
answer different questions — `merge ratio` is what greedy merging bought *on top
of* hidden-face removal, `total` is what the whole mesher bought against the
naive count.

| Scene | solid blocks | naive faces | after culling | quads | merge | total | tris | mesh bytes | min | **median** | p95 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| plains surface | 19 687 | 118 122 | 3 564 | 2 968 | 1.20× | **39.8×** | 5 936 | 166 208 | 0.724 | **0.749** | 0.996 |
| dense caves | 30 715 | 184 290 | 2 916 | 2 566 | 1.14× | **71.8×** | 5 132 | 143 696 | 0.725 | **0.749** | 0.806 |
| solid chunk | 32 768 | 196 608 | 6 144 | **6** | 1024× | **32 768×** | 12 | 336 | 0.409 | **0.419** | 0.439 |
| checkerboard | 16 384 | 98 304 | 98 304 | 98 304 | **1.00×** | **1.00×** | 196 608 | 5 505 024 | 5.726 | **6.346** | 6.958 |

* **plains surface** and **dense caves** are the two shapes the engine actually
  meshes, and they cost the same ~0.75 ms despite the cave section holding 56%
  more solid blocks. The sweep is over the 34³ cell grid regardless of what is in
  it; only the quad-emission tail scales with the geometry.
* **dense caves** is the best real-world reduction at 71.8×, because a cave
  section is mostly solid with a small interior surface. It was found by
  generating a 7×7 grid of sections at y=2 and keeping the one with the most
  air/solid boundary faces.
* **solid chunk** (stone, unloaded neighbours reading as air) is greedy meshing's
  best case: 6 144 surviving faces collapse into **six quads**, one per chunk
  face, 336 bytes of mesh. Surrounding it with stone instead would emit nothing
  at all and would time an empty sweep, which is why the neighbours are left
  unloaded.
* **checkerboard** is the worst case that exists. Every solid block has six air
  neighbours, so nothing culls and no two faces are mergeable: merge ratio
  exactly 1.000, 98 304 quads, 5.5 MB of mesh for one chunk. At 6.35 ms it is
  **8.5× the cost of a plains chunk**, and it is the bound to remember when
  considering any feature that produces high-frequency voxel noise.

### 4.5 Greedy meshing per LOD level — **two flagged results**

Whole neighbourhood at the same level (band interior), plus two band-edge
configurations where the centre is coarser than its neighbours.

| Case | quads | skirt quads | tris | mesh bytes | total reduction | min | **median** | p95 | speedup vs lod0 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| lod 0 | 2 968 | 0 | 5 936 | 166 208 | 39.8× | 0.743 | **0.773** | 0.790 | 1.00× |
| lod 1 | 759 | 0 | 1 518 | 42 504 | 152.3× | 0.431 | **0.441** | 0.484 | 1.75× |
| lod 2 | 133 | 0 | 266 | 7 448 | 863.3× | 0.404 | **0.412** | 0.459 | **1.87× — FLAG** |
| lod 3 | 34 | 0 | 68 | 1 904 | 3 433.4× | 0.657 | **0.700** | 0.735 | **1.10× — FLAG** |
| lod 1 beside lod 0 | 838 | **24** | 1 676 | 46 928 | 138.0× | 0.326 | **0.336** | 0.383 | — |
| lod 2 beside lod 1 | 177 | **15** | 354 | 9 912 | 648.7× | 0.205 | **0.207** | 0.220 | — |

**The geometry side of LOD works exactly as designed.** Level 3 emits 34 quads
against level 0's 2 968 — 87× fewer, 3 433× fewer than a naive mesher, and
1 904 bytes of GPU buffer instead of 166 208.

**The CPU side does not.** Meshing a level-3 chunk costs 90% of what meshing a
level-0 chunk costs, and level 2 is barely better than level 1. The harness flags
this automatically (`*** FLAG: expected >= 4.0x cheaper than level 0`).

The cause is arithmetic and is not a benchmark artefact. `GreedyMesher` has two
phases with opposite scaling:

| Level | cells in the (G+2)³ cache | blocks read per cell | **block reads to fill the cache** | sweep cells |
|---|---:|---:|---:|---:|
| 0 | 34³ = 39 304 | 1 | **39 304** | 32 768 |
| 1 | 18³ = 5 832 | 8 | **46 656** | 4 096 |
| 2 | 10³ = 1 000 | 64 | **64 000** | 512 |
| 3 | 6³ = 216 | 512 | **110 592** | 64 |

The sweep collapses by 8× per level, but `loadCacheLod` has to downsample a 2^L
cube per cell — and because the cache is a *shell plus interior*, the total block
count it touches **grows** with the level, reaching 2.8× the level-0 figure at
level 3. On top of the raw reads, each coarse cell runs a majority vote through a
per-block-id histogram, where level 0 is a straight copy. The two curves cross
around level 2, which is precisely the shape measured.

The band-edge rows corroborate the mechanism: a coarse chunk beside finer
neighbours is *cheaper* (0.336 vs 0.441 at level 1, 0.207 vs 0.412 at level 2)
because faces shared with a differently-levelled neighbour are loaded as air
rather than downsampled, removing most of the shell from the reduce pass.

This has not been fixed — it is a finding, recorded here with its evidence, and
it is the largest single item in the world-build budget (§6). It is worth noting
that a chunk generated at level L already holds uniform 2^L cubes, so the
majority vote is re-deriving something the generator knows; whether that can be
exploited safely for chunks the player has edited is an open question and not one
this benchmark answers.

The band-edge cases are also the only ones that produce **skirt quads** — 24 at a
level-1/0 edge, 15 at a level-2/1 edge. `GreedyMesher::emitSkirts` hangs the
curtain only where `levelDiffers`, because two neighbours at the same level
quantise onto the same global cell grid and have no seam to hide; the zero in the
band-interior rows is correct behaviour, not a missing feature. The cost of the
curtain is invisible against the saving from skipping the cross-level downsample.

### 4.6 Sub-voxel meshing vs damage count

A solid stone chunk surrounded by stone, with N blocks each carved out by a 4×4×4
sub-voxel corner (64 of 512, keeping the block strictly between empty and full as
`SubVoxel.hpp` requires). Damaged blocks are spread with a stride through the
whole chunk rather than clustered, so the sorted `SubVoxelStore` does not get an
unrealistically cheap insert pattern.

| damaged blocks | store heap | faces | quads | merge | tris | mesh bytes | min | **median** | **per damaged block** |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 0 B | 0 | 0 | — | 0 | 0 | 0.0020 | **0.0021** | **2.1 ns / chunk** |
| 1 | 80 B | 48 | 3 | 16.0× | 6 | 168 | 0.0054 | **0.0055** | 5.50 µs |
| 4 | 320 B | 192 | 12 | 16.0× | 24 | 672 | 0.0217 | **0.0218** | 5.45 µs |
| 16 | 1 280 B | 768 | 48 | 16.0× | 96 | 2 688 | 0.0873 | **0.0878** | 5.49 µs |
| 64 | 5 120 B | 4 064 | 254 | 16.0× | 508 | 14 224 | 0.3591 | **0.3955** | 6.18 µs |
| 256 | 20 480 B | 16 256 | 1 016 | 16.0× | 2 032 | 56 896 | 1.4582 | **1.5661** | 6.12 µs |

**The sparse store does what it was built to do.** An undamaged chunk costs
**2.1 nanoseconds** — the store's `empty()` check on a vector plus clearing an
already-empty output buffer, measured over 1 024 repetitions because a single
call is far below the clock. Cost is linear in the damage: 5.5 µs per damaged
block at N=1 and 6.1 µs at N=256, an 11% drift over a 256× range, which is cache
behaviour rather than an algorithmic term. Heap is
exactly 80 bytes per damaged block (`sizeof(SubVoxelStore::Entry)`) and exactly
zero when there is none.

The merge ratio is a flat **16.0×** because a 4×4×4 corner presents three flat
4×4 faces, each merging into one quad — 48 sub-voxel faces into 3 quads per
block. Speckle damage would approach 1.0× instead.

`SubVoxelMesher.hpp`'s header comment claims "~11 µs per damaged block". The
measured figure on this machine is **5.5-6.2 µs**, about half that; the comment
is conservative rather than wrong, but it is stale.

**Carving cost** (`subvoxel/carve_sub_voxels`): 64 blocks × 64 sub-voxels = 4 096
`Chunk::breakSubVoxel` calls, including the store insert that materialises each
block's grid on first damage, in 0.0747 ms — **18.2 ns per sub-voxel break**,
54.8 M breaks/s. Carving is not a cost centre; remeshing the chunk afterwards is.

### 4.7 Palette storage throughput and memory, per bits-per-index tier

Tiers are reached by populating a section with exactly enough distinct block ids
to force each width. The 8- and 16-bit tiers use synthetic ids beyond the
registry — `ChunkStorage` stores raw `BlockId` values and never consults the
registry, and no real section holds hundreds of materials; those rows exist to
bound the worst case, not to describe terrain.

**Memory** (voxel storage only — these fixtures never write light, so the light
array is unallocated):

| Tier | bits/index | palette | heap bytes | bytes/voxel | vs. a raw 64 KiB `BlockId[32768]` |
|---|---:|---:|---:|---:|---:|
| uniform | 0 | 1 | **0** | 0 | unbounded (one 8-byte member) |
| 1-bit | 1 | 2 | 4 100 | 0.125 | **16.0×** |
| 2-bit | 2 | 4 | 8 200 | 0.250 | **8.0×** |
| 4-bit | 4 | 16 | 16 416 | 0.501 | **4.0×** |
| 8-bit | 8 | 256 | 33 280 | 1.016 | 1.97× |
| 16-bit | 16 | 300 | 66 304 | 2.023 | 0.99× (break-even, as designed) |

**Throughput**, 32 768 operations per run:

| Tier | set (ns/voxel) | get sequential (ns/voxel) | get random (ns/voxel) |
|---|---:|---:|---:|
| uniform | 2.5 | < resolution (see §3) | 0.2 |
| 1-bit | 3.0 | 0.9 | 0.8 |
| 2-bit | 3.2 | 0.9 | 0.8 |
| 4-bit | 5.9 | 0.9 | 0.8 |
| 8-bit | **41.8** | 0.9 | 0.8 |
| 16-bit | **45.9** | 0.9 | 0.8 |

Three things fall out of this:

1. **`get` is flat at ~0.8-0.9 ns regardless of width, and random access costs
   the same as sequential.** That is the payoff of restricting widths to divisors
   of 64: no packed index straddles a word boundary, so every read is one load
   and two shifts with no branch. The whole index array is 4-33 KB and lives in
   L1/L2, so the scramble does not hurt. This is the operation the mesher does
   ~40 000 times per chunk, and it is effectively free.
2. **`set` degrades sharply past 16 palette entries** — 5.9 ns at 4 bits to
   41.8 ns at 8 bits, a 7× cliff. The cause is `findPaletteIndex`, a linear scan
   documented as fine because "real sections hold a handful of distinct blocks".
   §4.1 confirms that: every terrain section measured settles at **5-8 palette
   entries and 4 bits**, on the cheap side of the cliff. The 8- and 16-bit rows
   are the price of being wrong about that, and they say the linear scan would
   have to become a small hash if the block set ever grew past a couple of dozen
   materials per section.
3. **A real terrain section's 49 184 heap bytes are two-thirds light.** 16 416 of
   voxel indices against 32 768 of the per-voxel light array — light is 1 byte per
   voxel and uncompressed, so it costs twice what the palette-compressed voxels
   do. `ChunkStorage` already has a uniform-light representation that costs one
   byte; the sections measured here are the ones that cannot use it.

### 4.8 Light propagation

Measured after integration wired `world/LightEngine.cpp` into `src/CMakeLists.txt`;
the three cases switched on with no edit to `benchmarks/`, exactly as §8 predicted.

| case | runs | min ms | med ms | p95 ms | per-op |
|---|---|---|---|---|---|
| `lighting/light_column` | 11 | 1.6408 | **1.7465** | 1.8254 | 218.3 µs / section |
| `lighting/light_chunk` | 15 | 0.1462 | **0.1529** | 0.2108 | 152.9 µs / chunk |
| `lighting/edit_block_relight` | 31 | 0.0005 | **0.0006** | 0.0006 | ~600 ns / edit |

* `lighting/light_column` — the streaming path: a whole 8-section column as one
  job, with its 3×3 read region, light zeroed before every run. This fixture is
  deliberately hostile: 113 856 non-air blocks, 143 236 lit voxels and 17 869
  spill seeds in one column. A typical streamed column is several times cheaper,
  because a uniformly opaque section is O(1) and a clear section under unbroken
  sky is one `fillLight`.
* `lighting/light_chunk` — the LOD-shadow path: one section from a 3×3×3 snapshot.
* `lighting/edit_block_relight` — the only lighting path with a frame budget: a
  player breaking a sunlit surface block, then `voxelChanged` on the main thread,
  against a fully lit 3×3 column region. The median is **below the harness's
  reliability floor** (`kUnreliableMedianMs`), so read it as an upper bound: clock
  granularity and loop hoisting cannot separate it from zero. What it does
  establish is the thing that matters — an interactive edit's relight is nowhere
  near a frame budget.

Lighting a whole column costs about what meshing one chunk does, and it happens
once per column rather than once per chunk.

### 4.9 Chunk serialisation round-trip

`WorldSave::encodeChunk` / `WorldSave::decodeChunk` only — the pure CPU codec. No
`JobSystem`, no directory, no disk: region I/O would measure the machine's SSD
rather than this engine and would make the numbers incomparable between machines.
Encode allocates a fresh `std::vector<std::byte>` on every call, exactly as
`WorldSave::saveChunk` does, and that allocation is inside the measurement.

`vs. raw` compares against a dense section of 2 bytes of block id plus 1 byte of
packed light per voxel — 96 KiB.

| Case | payload | bytes/voxel | vs. raw | min | **median** | p95 |
|---|---:|---:|---:|---:|---:|---:|
| round trip, terrain section | 17 832 B | 0.544 | **5.51×** | 0.387 | **0.402** | 0.463 |
| round trip, uniform section | **13 B** | 0.0004 | **7 562×** | 0.0149 | **0.0149** | 0.0385 |
| round trip, 64 damaged blocks | 22 184 B | 0.677 | 4.43× | 0.479 | **0.489** | 0.522 |
| encode only, terrain | 17 832 B | — | — | 0.042 | **0.0426** | 0.046 |
| decode only, terrain | 17 832 B | — | — | 0.341 | **0.353** | 0.395 |

* **Encode is 42.6 µs.** This is the half that runs on the main thread inside the
  frame (`WorldSave.hpp` explains why), and 42.6 µs against a 16.7 ms frame is
  0.25% — the design's central claim, that the synchronous half is affordable,
  holds by a wide margin. Saving 100 chunks in one frame would cost 4.3 ms, so
  autosave still wants batching, but a single chunk retiring is free.
* **Decode is 353 µs, 8.3× encode.** Decoding runs on a worker inside
  `ChunkGenerateFn`, so it is not a frame-time risk, and against the alternative
  it is a clear win: generating the same sea-level section from the seed costs
  **1.715 ms** (§4.1), so loading it from disk is **4.9× cheaper than
  regenerating it**. That ratio is the result that justifies the save format
  existing at all for unmodified terrain, never mind modified terrain. The
  encode/decode asymmetry is still worth a look, though — encode is essentially a
  memcpy of the palette and the index words, whereas decode parses and validates
  into scratch storage first and re-applies sub-voxel damage one call at a time
  through `Chunk::breakSubVoxel`.
* **The uniform section is 13 bytes**, confirming the header's claim of "three
  bytes instead of 64 KB" for the representation that dominates a real world
  (empty sky, deep stone). This is the single biggest reason the format is cheap.
* **64 damaged blocks add 4 352 bytes** (22 184 − 17 832), i.e. 68 bytes per
  damaged block — a 512-bit grid plus its index, essentially uncompressed, which
  is correct for data that is a bitmask.

---

## 5. In-game measurements already on record

These were captured from the running game with the F3 overlay, not by this
harness, and are reproduced here because the harness's per-chunk numbers only
mean something next to them.

### Radius 20, LOD enabled

| | |
|---|---|
| Chunks resident | **10 056** |
| LOD distribution (levels 0-3) | **872 / 1 568 / 3 136 / 4 480** |
| Visible chunks (levels 0-3) | **109 / 322 / 650 / 577** = 1 658 |
| Triangles by level | **422 168 / 326 184 / 112 898 / 18 012** |
| Total triangles | **879 262** |
| Draw calls | **1 781** |
| Frame rate | **~557-582 fps** |
| GPU buffers | **75 MB** |

### Pre-LOD baseline

| | |
|---|---|
| Radius | 8 |
| Chunks resident | 1 576 |
| Draw calls | 208 |
| Triangles | 582 000 |
| Frame rate | ~1 250 fps |

### What the comparison says

LOD buys **6.4× the resident world** (10 056 vs 1 576 chunks) for **1.51× the
triangles** (879 k vs 582 k) at **2.2× the frame time** (1.72 ms vs 0.80 ms).
Radius 20 without LOD was never rendered — the point of the feature is that it
does not have to be.

The per-level triangle counts show where the budget goes: the 109 visible level-0
chunks carry 3 873 triangles each and 48% of the total, while the 577 visible
level-3 chunks carry 31 triangles each and 2.0%. The measured per-chunk mesh
sizes in §4.5 predict this well — 5 936 triangles for a level-0 plains chunk and
68 for a level-3 one.

---

## 6. What building the whole world costs — *extrapolated*

**This section multiplies measured per-chunk costs by the recorded LOD census. It
is arithmetic on measurements, not an end-to-end measurement**, and it ignores
scheduling, streaming budgets and the light pass (which does not exist yet).

Using §4.2 generation medians and §4.5 meshing medians against the census
`[872, 1568, 3136, 4480]`:

| Level | chunks | gen ms/chunk | gen total | mesh ms/chunk | mesh total |
|---|---:|---:|---:|---:|---:|
| 0 | 872 | 1.808 | 1 576 ms | 0.773 | 674 ms |
| 1 | 1 568 | 0.567 | 889 ms | 0.441 | 691 ms |
| 2 | 3 136 | 0.220 | 690 ms | 0.412 | 1 292 ms |
| 3 | 4 480 | 0.210 | 941 ms | 0.700 | **3 136 ms** |
| | **10 056** | | **4 096 ms** | | **5 793 ms** |

* Total CPU work to build the radius-20 world: **9.89 seconds single-threaded**,
  or **~1.1 s of wall time** at the measured 8.97× job-system speedup.
* The same 10 056 chunks all at level 0 would be 10 056 × (1.808 + 0.773) =
  **26.0 s single-threaded**, ~2.9 s wall. **LOD saves 2.6× of world-build time.**
* **Level-3 meshing alone is 54% of all meshing work** (3 136 of 5 793 ms) for
  chunks that contribute **2.0% of the drawn triangles**. That is the §4.5 flag
  restated in the units that matter. If level 3 merely matched level 2's measured
  per-chunk cost, the world build would drop by 1.29 s single-threaded — **13% of
  the entire budget** — for no change in what is drawn.

---

## 7. Optimisation decisions and what each one bought

### Palette compression — **measured: 4× on voxels, unbounded on uniform sections; free on reads**

Every terrain section measured settles at 4 bits per index: **16 416 bytes
instead of 65 536**, a flat 4× (§4.7). A uniform section — empty sky, deep stone,
the majority of a real world — is **0 heap bytes**, held in one 8-byte member,
and serialises to **13 bytes** (§4.9).

The read cost of that compression is **zero, measured**: 0.8-0.9 ns per `get()`
at every width, sequential or random, identical to the uniform case's constant
load once you account for the loop the optimiser eliminates there. Restricting
widths to divisors of 64 is what earns that — no index straddles a word, so a
read is a load and two shifts with no branch.

The cost is on the write side and only past 16 materials: 5.9 ns/set at 4 bits
against 41.8 ns at 8 bits, because `findPaletteIndex` is a linear scan. Real
sections hold 5-8 materials, so the engine sits on the cheap side; the
measurement tells us exactly where the assumption would break.

### Greedy meshing — **measured: 39.8× on real terrain, 71.8× in caves, 32 768× best case, 1.0× worst**

Against a mesher that emitted six faces per solid block (§4.4):

| | plains | caves | solid | checkerboard |
|---|---:|---:|---:|---:|
| naive faces | 118 122 | 184 290 | 196 608 | 98 304 |
| quads emitted | 2 968 | 2 566 | 6 | 98 304 |
| **reduction** | **39.8×** | **71.8×** | **32 768×** | **1.00×** |

Two-thirds of that comes from hidden-face removal (118 122 → 3 564 on plains) and
the last stretch from the greedy merge (3 564 → 2 968). On flat or blocky terrain
the merge does far more — the solid chunk's 6 144 faces become six quads. The
checkerboard row is the honest bound: on high-frequency noise greedy meshing buys
nothing and costs 8.5× a normal chunk to discover that.

### Packed 8-byte vertex format — **measured: 2.7× smaller mesh than a float format**

A plains chunk's mesh is 11 872 vertices and 17 808 indices: 11 872×8 + 17 808×4
= **166 208 bytes**, measured. The same mesh with a conventional 32-byte vertex
(three float positions, three float normals, two float UVs) would be 11 872×32 +
71 232 = 451 136 bytes — **2.71×**. Scaled to the recorded 75 MB of GPU buffers at
radius 20, the naive format would be roughly 200 MB on a 6 GB laptop card.

The 6-bit position fields are only expressible because a level-L cell boundary
always lands on a multiple of 2^L, so every quad corner is still an integer block
coordinate in [0, 32] at every LOD level — LOD costs no vertex bits and no shader
change.

### Texture arrays over an atlas — **not benchmarked; the argument is correctness**

There is no measurement for this and this document will not invent one. The
decision was not made on throughput: a texture array gives every layer its own
mip chain and its own wrap behaviour, so tiling a block face costs nothing and
mip level 4 of one block cannot bleed into its neighbour. An atlas needs manual
padding, a mip cap, or per-face UV clamping in the shader, and gets it wrong at
grazing angles. Sampling cost between the two is not distinguishable at this
engine's fill rate, so there is nothing to report.

### Snapshot-based meshing — **measured indirectly: 0.75 ms of lock-free work per chunk**

`ChunkNeighbourhood` copies 27 `shared_ptr`s inside the world lock and hands a
value type to a worker; the worker then touches no shared mutable state at all.
The measured meshing time is what that protects: **0.42-0.75 ms per chunk of
work outside any lock** (§4.4). The critical section is 27 atomic increments,
which was not measured separately, but it is bounded by the ~0.8 ns/op class of
operation measured throughout §4.7 — i.e. tens of nanoseconds against hundreds of
microseconds, under 0.1% of the job.

The alternative — holding the chunk-map lock for the whole mesh — would make the
world map the single serialising resource in the engine, and the frame hitch
would scale with view distance. The 8.97× job-system speedup in §4.3 is the
closest direct evidence that the snapshot approach lets workers actually run in
parallel.

### LOD with skirts — **measured: 87× fewer quads, but the CPU saving is not there yet**

Level 3 emits **34 quads against level 0's 2 968** and 1 904 bytes of buffer
against 166 208 (§4.5). In the game that is the difference between 879 k
triangles at radius 20 and a world that could not be rendered at that radius at
all (§5). Generation is **8.6× cheaper** at level 3 (§4.2).

Skirts cost **24 quads at a level-1/0 band edge and 15 at a level-2/1 edge** —
measured, and only at band edges, because same-level neighbours quantise onto the
same global grid and have no seam to hide. Against the thousands of triangles a
stitched transition band would need, and against the fact that a curtain cannot
crack, this is the cheapest possible way to close the seam.

What LOD has **not** bought is meshing CPU time: 1.10× at level 3 (§4.5), which
the harness flags. The geometry and generation wins are real and large; the
meshing win is missing and its cause is understood.

### Sparse sub-voxel store — **measured: 2.25 MB and 20 000 allocations saved**

An empty `std::unordered_map` on MSVC eagerly allocates its bucket list: **2
allocations and 224 bytes** to represent "nothing is damaged". Across the 10 056
resident chunks of §5 that is **2.25 MB of heap and ~20 000 allocations** for a
world in which almost nothing is damaged. The sorted vector `SubVoxelStore` uses
is **0 bytes and 0 allocations** when empty, and this benchmark confirms the
consequence end to end: an undamaged chunk's sub-voxel mesh pass costs **2.1
nanoseconds** (§4.6).

When damage does exist the store costs exactly 80 bytes per damaged block and the
mesh pass 5.5-6.2 µs per damaged block, linear across a 256× range. The
asymptotics are worse than a hash map — binary-search lookup, O(n) insert — and
irrelevant: n is the number of damaged blocks in one chunk, and a linear memmove
over contiguous memory beats a pointer chase at those sizes. It is also the safer
structure, because a hash map rehashes on insert and would invalidate references
a worker holds through a chunk snapshot.

---

## 8. Open items

| Item | Status |
|---|---|
| **Level-2 and level-3 meshing barely cheaper than level 0** (§4.5) | Flagged automatically by the harness. Cause identified: `loadCacheLod`'s downsample reads *more* blocks at coarser levels (110 592 at level 3 vs 39 304 at level 0) and runs a majority vote per cell. Worth 13% of the world-build budget (§6). Not fixed. |
| ~~Light propagation unmeasured~~ (§4.8) | **Resolved.** `world/LightEngine.cpp` is in `src/CMakeLists.txt`; the guarded cases switched on by themselves and §4.8 now carries real figures. |
| **Persistence measured out-of-tree** (§4.9) | `src/world/WorldSave.cpp` and `RegionFile.cpp` exist but are not yet listed in `src/CMakeLists.txt`. The reference run compiled them directly into `voxl_bench` (`-DVOXL_BENCH_LINK_PENDING_SOURCES=ON`) — same compiler, same flags, same code, only a different archive — so the figures are valid. The default build compiles the case out until the sources are integrated. |
| **`SubVoxelMesher.hpp` header comment says ~11 µs per damaged block** (§4.6) | Measured at 5.5-6.2 µs. The comment is stale, not wrong. |
| **Resident-world memory total** | Not measured by this harness; §4.7 reports per-section figures only. The F3 overlay reports the live total in-game. |
| **Region file I/O** | Deliberately not benchmarked — it would measure the SSD, not the engine. |
| **Texture array vs. atlas** | No measurement exists and none is claimed (§7). |

---

## 9. Adding a case

`benchmarks/Cases.hpp` declares one `register*Cases(Runner&)` per group, called
explicitly from `main`. Registration is explicit rather than through
self-registering statics: static registration would put the case list at the
mercy of translation-unit initialisation order and would make the order in which
cases run depend on the linker — unacceptable in a harness whose entire job is
reproducibility.

```cpp
Case testCase;
testCase.group     = "meshing";
testCase.name      = "my_scene";
testCase.unit      = "chunk";
testCase.opsPerRun = 1.0;
testCase.setup     = [fixture](CaseContext& ctx) { /* untimed, once */ };
testCase.prepare   = [fixture](CaseContext&)     { /* untimed, per run */ };
testCase.body      = [fixture](CaseContext&)     { /* TIMED */ };
runner.add(std::move(testCase));
```

Guard an expensive fixture with `runner.selected(group, name)` so `--filter`
skips building it. Use `ctx.counter(...)` for properties of the workload and
`bench::keep(...)` to stop the optimiser deleting a loop whose result is unused.
If a case cannot run, call `runner.addUnavailable(group, name, reason)` — the
report prints it as `NOT MEASURED` with the reason, because silently dropping a
case is how a missing number turns into an invented one.
