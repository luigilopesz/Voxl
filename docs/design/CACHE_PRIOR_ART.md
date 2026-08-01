# Cache and streaming prior art: GigaVoxels at seventeen

**Written:** 2026-08-01
**Phase:** research and design. Nothing in `C:\voxl2\src` was modified to produce this document.
**Question asked:** the user supplied Crassin, Neyret, Lefebvre and Eisemann, *GigaVoxels:
Ray-Guided Streaming for Efficient and Detailed Voxel Rendering*, I3D 2009, and asked whether its
cache mechanism solves the wall that `SCALE_LIMITS.md` just measured. Establish what seventeen years
did to it.

**Every claim below is tagged.**

| tag | means |
|---|---|
| **PAPER** | the 2009 I3D paper says it. Page/section cited. |
| **THESIS** | Crassin's 2011 PhD thesis says it — *later than the paper, and often different from it*. |
| **LATER WORK** | somebody else established it. Cited. |
| **MEASURED** | measured in this project, on this hardware. The command is given. |
| **DERIVED** | my arithmetic on top of one of the above. The arithmetic is shown so you can check it. |
| **UNDETERMINED** | I looked and could not establish it. Listed in §8. |

---

## 0. The short answer

**The user's summary of the mechanism is substantially wrong, and wrong in the direction that
matters.** Four of its five claims do not survive contact with the source:

| the summary says | what the source actually says | tag |
|---|---|---|
| "any voxel that gets rendered is moved to the FRONT of the array" | Correct in spirit, wrong in mechanism and wrong about granularity. Nothing moves per-element. Once per frame the *whole* page list is stably partitioned into used/unused by **two order-preserving stream compactions** and the halves concatenated. Granularity is a **brick** (16³ or 32³ voxels), never a voxel. | THESIS §7.3.4, p.134–135 |
| "the whole thing runs in compute shaders on the GPU" | **False for the 2009 paper.** In the paper the LRU is a **CPU-side timestamp** over a mirrored copy of the structure; the GPU only emits a compacted node-usage list that is **read back to the CPU**. The GPU-resident cache arrived two years later, in the thesis. | PAPER §2 p.3, §6.1 p.5 |
| "holds rendering stable under a 1 MB pool" | **No such claim exists.** The paper's pools are a **4 MB node pool and a 430 MB brick pool**. "1 MB" is the smallest x-axis point in a *cache-management-cost* chart in the thesis, not a rendering configuration. | PAPER §7 p.7; THESIS Fig 7.25 p.154 |
| "addresses up to 64 GB of voxel tree data" | It is an **address-space limit of a pointer encoding**, not a demonstrated working set. 2³⁰ node tiles × 8 nodes × 8 B = 64 GB *addressable*. The same section quotes 2 TB addressable for bricks. Nobody streamed 64 GB. | THESIS §5.2 p.103 |
| "swapping clusters co-accessed data, improving locality" | **Does not happen.** The LRU list stores *references to* pages. Reordering the list does not move one byte of voxel data. The pool's physical layout is untouched by the sort. | THESIS §7.3.4 p.134 |

**And the strategic conclusion is the opposite of the one the brief anticipated.**

The brief reasons: the chunk table is dense and resident, therefore we need ray-guided streaming with
an LRU. But `SCALE_LIMITS.md` §3.3 measured that at CPA 64 the entire *content* of the world is
**9.1 MB**. GigaVoxels' LRU exists to evict, and eviction is only needed when the working set exceeds
the budget. Ours does not exceed it — it is three orders of magnitude under it.

> **The problem we measured is not a cache problem. It is a compaction problem.**
> GigaVoxels bundles two separable ideas: a **directory-plus-pool layout** (which is what fixes a
> dense resident table) and an **LRU with ray-guided feedback** (which is what fixes a working set
> larger than VRAM). We have the first problem and not the second. Build the pool. Do not build the
> cache — yet.

**When does the cache become necessary?** DERIVED, arithmetic in §6.2: at roughly a **1.2 km world**
of the same surface character. Below that, a fixed pool with no eviction policy at all holds
everything. That single number is the most decision-relevant thing in this document, because it
converts "do we need a GPU LRU" from an architecture argument into a scheduling one: **not this
year.**

---

## 1. What the 2009 paper actually describes

Source: <https://maverick.inria.fr/Publications/2009/CNLE09/CNLE09.pdf> (I3D 2009, 8 pp).
The PDF does not yield to naive text extraction; I extracted it with PyMuPDF and read the body.

### 1.1 The structure — PAPER §4, p.3

An **N³-tree** (N=2 is an octree; they also use N=3). Each node either points to a **brick** — a
small voxel grid of size M³, "usually M = 32" — or is tagged constant/empty. Bricks live in one
large 3D texture, the **brick pool**. Nodes live in a **node pool**.

This is a directory-plus-pool layout with a mipmap. It is the same shape as the layout
`SCALE_LIMITS.md` §3.3 arrives at independently from the census.

### 1.2 The cache — PAPER §2 p.3 and §6.1 p.5, and this is the part the summary gets wrong

> PAPER §2, p.3: "Each brick in the pool has a certain timestamp that is reset upon usage. If an
> octree node needs a subdivision and new bricks are transferred to the GPU, the algorithm will use
> the memory locations that were previously reserved for the oldest, thus unused, bricks (this
> concept is referred to as LRU)."

And immediately after, the sentence that kills the "runs entirely on the GPU" claim:

> PAPER §2, p.3: "To keep track of the current data organization and facilitate updates, the
> structure is mirrored on the CPU. […] This facilitates some of the operations, such as **the LRU
> ordering on the CPU**."

Reinforced in §6.1, p.5: the CPU "can conveniently update the timestamps of the pool elements
(nodes and bricks) in its mirrored data structure […] **It even knows where to store the brick in
the pool because timestamps are maintained in host memory.** All necessary modifications are then
transferred to the GPU via texture-update calls."

**In the 2009 paper the LRU is a CPU data structure.** The GPU's job is only to report what it
touched.

### 1.3 The genuinely novel bit — ray-guided feedback, PAPER §6.1.1–6.1.2, p.6

This is what the paper is actually *for*, and it is the part that has aged best.

- Each ray writes, alongside colour, the indices of the nodes it traversed, plus a
  "needs subdivision" bit — into MRT render targets. 30 bits of index, 1 bit of tag.
- Capacity is small: "seven node-list textures allow for a 7×4 = 28-node output per ray", but "in
  practice we found that three MRTs are the best choice", so 12 nodes per ray.
- Two coherence tricks recover the shortfall: **spatial** (2×2 ray packets divide the node range
  between them, 48 nodes per packet) and **temporal** (a FIFO window shifted across frames).
- Then a **selection mask** compares each ray's list only against its neighbours' lists at offsets
  i−1, i, i+1 — "conservative", admits false positives — and a **HistoPyramid** reduction
  (Ziegler et al. 2006) compacts it. Final readback is "one single RGBA32 texture".
- **Collapse is implicit and this is elegant:** PAPER §6.1.1 p.6 — "there is no need to send collapse
  information. If the rendering algorithm no longer descends into a node, its index will never be
  put into the list, thus the LRU mechanism will not reset its timestamp and thus replace the data at
  some point. This establishes a lazy evaluation scheme."

Note explicitly why they built the compaction: not for speed of the cache, but because
**readback bandwidth** would otherwise be "enormous". The compaction exists to shrink a PCIe
transfer. That motivation is much weaker in 2026 than in 2009, and weaker still for us, because we
would not read back at all.

### 1.4 The numbers — PAPER §7, p.7

- Hardware: Core 2 E6600, **NVIDIA 8800 GTS 512 (G92), 512 MB**. Images at 512×512.
- Node pool **4 MB** (64³ entries); brick pool **430 MB** (42³ bricks of 16³).
- Trabecular bone, 1024³ tiled to a virtual 8192³: **20–40 Hz** with mipmapping, ~60 fps without.
- Hypertexture with 20 octaves of Perlin: **~20 fps** at 1024³.
- Sierpinski, procedural: 60–90 Hz.

**430 MB of brick pool on a 512 MB card.** The paper is not a memory-frugality result; it is an
out-of-core result. It fills the card and streams through it.

### 1.5 What the paper says it cannot do — PAPER §8, p.7

> "Currently, animation is a big problem for volume data. In the future, we would like to
> investigate possible solutions."

**The 2009 paper does not support editing or animation.** For a sandbox game this is the headline
limitation and the paper states it in one line in the conclusion. Everything in §4 below about
editability is later work.

---

## 2. The thesis is a different system, and it is the one people mean

Crassin, *GigaVoxels: A Voxel-Based Rendering Pipeline For Efficient Exploration Of Large and
Detailed Scenes*, PhD thesis, Université de Grenoble, 2011.
<https://maverick.inria.fr/Membres/Cyril.Crassin/thesis/CCrassinThesis_EN_Web.pdf> (207 pp).

Chapter 7, "Out-of-core data management", replaces the paper's CPU-side LRU with a **generic GPU
cache**. When people say "the GigaVoxels LRU runs on the GPU", they are describing this, not the
paper.

### 2.1 The mechanism, exactly — THESIS §7.3.4, pp.134–135

The list:

> "We implement this LRU caching scheme on the GPU by maintaining a list of all the pages present in
> a cache, sorted by the time since they were last used. This list is called the **LRU page list** […]
> **Each entry stores the reference (or pointer) to a page in the cache.** A reference is a 32bit
> value."

The update, and this is the answer to the user's question 2:

> "**Sorting the LRU page list at each rendering pass on the GPU using a sorting algorithm would be
> prohibitive.** Instead, we rely on an incremental sorting scheme that makes use of **two
> order-maintaining stream-compactions**."
>
> "The LRU page list then undergoes two stream compaction steps. The first stream compaction will
> create a list U+ that only contains the used elements, the second a list that contains all unused
> elements U−. Because inside each of these sublists, **the order remains the same**, the list of the
> unused elements will still have the oldest elements at the end […] Therefore, when concatenating
> U+ to the beginning of the U− we inherently sort the usage list."

So the answer to *"does anyone actually do move-to-front swapping on a GPU"* is:

> **No. Nobody swaps. It is a bulk stable partition of the entire list, once per frame, built out of
> two parallel prefix-sum compactions.** A per-element swap would need atomics and would serialise;
> the thesis explicitly rejects the sort, and §7.3.3 p.132–133 separately rejects an atomically-built
> request queue because atomics are "a very costly operation on a parallel architecture […] it forces
> the serialization of write operations."

Three consequences the user's summary hides:

1. **The cost is O(pool size) per frame, not O(elements touched).** The compaction sweeps every entry
   whether or not anything moved. A cache that nothing touched costs the same as a cache that
   thrashed. This is the single most important cost property of the design.
2. **Usage is a timestamp compared against the frame number** (§7.3.4 p.134: "If the stored timestamp
   matches the time of the current frame, the element is flagged"). So the primitive really is
   *timestamp + periodic compaction* — which is exactly one of the two options the brief asked me to
   choose between. It is not clock/second-chance.
3. **The list holds references. The data never moves.** Which disposes of the locality claim — §2.3.

### 2.2 What it costs — THESIS §7.5.4, Fig 7.25, p.154

The thesis benchmarks GPU-managed LRU against a CPU-managed one, 64 B pages:

| pool size | **GPU LRU management, ms/frame** | CPU LRU management, ms/frame | speed-up |
|---:|---:|---:|---:|
| 1 MB | **0.19** | 0.33 | 1.7× |
| 2 MB | 0.20 | 0.68 | 3.4× |
| 4 MB | 0.24 | 1.30 | 5.4× |
| 8 MB | 0.26 | 1.90 | 7.3× |
| 16 MB | 0.36 | 5.10 | 14.2× |
| 32 MB | 0.45 | 7.05 | 15.7× |
| 64 MB | **0.75** | 20.60 | **27.5×** |

THESIS. **The GPU for this figure is not stated.** Chapter 7 reports GTX 280 elsewhere in its results
(§7.6) and other chapters report GTX 480, so read these as 2009–2011 hardware and do not attach a
specific part to them.

Read this the right way round. The GPU curve is nearly flat — 0.19 → 0.75 ms while the page count
goes 16,384 → 1,048,576, a 64× increase in pages for 3.9× the time. That is the signature of a
fixed launch overhead dominating a bandwidth-trivial workload. **The floor, ~0.19 ms, is dispatch
overhead, not work.** That matters enormously for us (§6.3).

And the thesis is candid that compaction is the expensive part: §7.4 p.139 — "In our tests, **this
stream compaction appeared to be one of the most costly operations done for the management of a
cache.**" They bolt on an extra pre-pass compaction specifically to avoid paying for it twice.

### 2.3 The locality argument does not survive — and here is why

The user's summary says swapping "clusters co-accessed data, improving locality". Against the actual
design this is false, for a structural reason:

**The LRU page list contains 32-bit references to pages (THESIS §7.3.4 p.134). Sorting it permutes
pointers. The bricks stay exactly where they are in the brick pool.** A brick that has been hot for
a thousand frames sits at whatever physical texture coordinate it was first written to. There is no
compaction of the data, no relocation, no defragmentation.

There *is* a faint second-order effect — pages evicted together are refilled together, so bricks
loaded in the same frame occupy slots that were freed in the same frame — but those slots are
scattered across the pool by the previous eviction cycle, so the effect is weak and nobody claims it.
DERIVED, from the structure of the algorithm; no source asserts a locality benefit from the sort.

Relocating the data to get locality is a real technique, but it is a different technique
(defragmenting compaction), it costs a copy of every moved page, and GigaVoxels does not do it.

**If somebody wants locality from a voxel pool, the lever is the pool's *insertion* policy — allocate
spatially-neighbouring chunks adjacent — not the LRU's sort order.**

---

## 3. What seventeen years did: the timeline

| year | work | what it changed | editable? |
|---|---|---|---|
| 2009 | **GigaVoxels** (I3D) | ray-guided feedback; CPU-side LRU | no |
| 2010 | **Efficient Sparse Voxel Octrees**, Laine & Karras | contours, beam optimisation; the traversal baseline everyone cites | no |
| 2011 | **Crassin thesis** ch.7 | LRU moves onto the GPU; generic page cache; also applied to a BVH (§7.5.5) | no |
| 2013 | **High Resolution Sparse Voxel DAGs**, Kämpe, Sintorn, Assarsson (SIGGRAPH) | subtree deduplication. **(128k)³ in under 1 GB** | **no — static only** |
| 2016 | **GVDB Voxels**, Hoetzlein (HPG) | indexed memory pooling for **dynamic topology**; VDB-style grid hierarchy on GPU | **yes (topology rebuild)** |
| 2020 | **HashDAG**, Careil, Billeter, Eisemann (CGF/EG) | makes a DAG editable without decompression | **yes, with caveats** |
| 2021 | **NanoVDB**, Museth (SIGGRAPH Talks) | linearised, pointer-free VDB for GPU | **no — static topology** |
| 2021 | **Nanite** (SIGGRAPH Advances) | virtualised geometry at production scale: GPU requests, CPU policy | n/a (static meshes) |
| 2023 | **Editing Compressed High-resolution Voxel Scenes with Attributes**, Molenaar & Eisemann (CGF) | extends HashDAG-style editing to attributes | yes |
| 2024 | **GigaVoxels DP**, Richermoz & Neyret (HPG) | the original authors' group revisits it: GPU dynamic parallelism against core starvation, **2× gain** | no |
| 2025 | **Aokana** (I3D) | SVDAG chunks + LOD + streaming for open worlds; tens of billions of voxels | **explicitly no** |

### 3.1 GVDB Voxels — the one that solved dynamic topology, and *didn't* solve streaming

Hoetzlein, *GVDB: Raytracing Sparse Voxel Database Structures on the GPU*, HPG 2016.
<https://ramakarl.com/pdfs/2016_Hoetzlein_GVDB.pdf>. Code (BSD-3):
<https://github.com/NVIDIA/gvdb-voxels>.

Two pool groups: **P0** holds 40-byte node headers plus bitmasks; **P1** holds child lists. Brick
voxels live in a 3D **texture atlas** (§3, p.3–4). Dynamic topology is handled by pooling: when a
child list overflows, "we either dynamically reallocate that pool or move the child list to a larger
pool."

**GVDB makes our argument for us, in its own words** (§3.2, p.4):

> "The node header size (N) is padded to 40 bytes. […] the topology size of a large data set,
> containing 500k leaf bricks, would average around **20MB**. Compare this to a two-level hierarchy
> using an indirection table with size 512³ which requires around **500MB** assuming 4 byte pointers."

That is a **25× ratio between a pooled topology and a dense indirection table**, argued at NVIDIA in
2016. `SCALE_LIMITS.md` §3.3 measures **237.8×** for our table at CPA 64, from the census. Same
argument, independently, an order of magnitude more extreme in our case because our per-chunk record
is 8216 B rather than 4 B.

Performance, §5, Quadro M6000 12 GB: 100×–200× faster than CPU OpenVDB; the waterjet scene renders
at 34 ms/frame (29 fps) against OpenVDB's 2235 ms.

**But GVDB is fully resident.** Out-of-core rendering is listed as an *extension* in the future-work
section (p.9), not implemented. GVDB is the pool without the cache — which, per §0, is precisely the
half we need.

### 3.2 NanoVDB — the brief guessed it would be the modern answer. It is not, for us.

Museth, *NanoVDB: A GPU-Friendly and Portable VDB Data Structure*, SIGGRAPH Talks 2021,
doi:10.1145/3450623.3464653. FAQ:
<https://github.com/AcademySoftwareFoundation/openvdb/blob/master/doc/nanovdb/FAQ.md>.

NanoVDB is a "linear snapshot of an OpenVDB data structure" that "explicitly avoids memory pointers"
— one contiguous, `memcpy`-able block, portable to CUDA/OpenCL/GLSL/HLSL/DX12/OptiX. It ships HDDA
for empty-space skipping. Adopted by Arnold, Blender, Houdini, Omniverse.

**And the FAQ states the disqualifying limitation outright:**

> "The most important limitation of NanoVDB is the fact that it assumes the topology of the tree
> structure to be **static**. More specifically, while values can be modified in a NanoVDB grid **its
> tree topology cannot**."

LATER WORK: the FAQ has since been softened — some operations "generate new NanoVDB grids (as opposed
to in-place modifications in OpenVDB)" on the GPU, and it is "no longer limited to static
applications". But **regenerating the grid is not editing a voxel**; for a player carving a hole
sixty times a second it is the wrong primitive.

NanoVDB is the right answer to "how do I ship a static volume to a GPU renderer". It is the wrong
answer to "how do I run a sandbox".

### 3.3 GigaVoxels DP (HPG 2024) — the same group, fifteen years on

Richermoz & Neyret, *GigaVoxels DP: Starvation-Less Render and Production for Large and Detailed
Volumetric Worlds Walkthrough*, Proc. ACM CGIT (HPG) 2024, doi:10.1145/3675389.
<https://hal.science/hal-04654692v1>.

Their own framing of what was still broken: "on-demand production of data during rendering is still
challenging in terms of **synchronization and starvation of GPU cores**." Their fix is GPU dynamic
parallelism plus a "GPU-cores timeline" profiling tool. **Reported 2× gain.**

The signal here is more useful than the technique. **Fifteen years after the paper, the original lab
is still publishing on the fact that the on-demand production path starves the GPU.** For a
single-developer project on a 6 GB laptop card, that is a warning that the ray-guided *production*
loop — not the LRU, the loop that makes the data when a ray asks for it — is where the difficulty
actually lives.

I could not extract the paper's full text: HAL serves it behind an Anubis proof-of-work
anti-scraping challenge, and the ACM DL copy is paywalled. The claims above are from the abstract
and the HAL landing page. Flagged in §8.

### 3.4 Aokana (I3D 2025) — the closest modern analogue, and it gives up editing

Xu et al., *Aokana: A GPU-Driven Voxel Rendering Framework for Open World Games*, I3D 2025,
doi:10.1145/3728299, arXiv:2505.02017 <https://arxiv.org/html/2505.02017v1>.

Architecture (§3.1–3.4): the map is cut into cubic regions, each a **256³ chunk compressed as its own
small SVDAG** — "multiple SVDAGs with smaller depths" specifically because one deep DAG is
cache-hostile. Nodes 8–40 B, 8-bit child mask, 64-bit bitmap for 4³ leaf chunks. LOD n aggregates
eight LOD n−1 chunks, still 256³, with a density threshold of 2. Colour is decoupled from geometry
and run-length blocked with a 0.05 difference threshold.

Streaming (§3.4): CPU keeps an implicit octree; chunks load when
`LODError = ChunkSize × StreamingFactor − ‖ChunkCentre − Camera‖` goes negative. **Distance-driven,
CPU-directed, no LRU and no ray-guided feedback.** They kept GigaVoxels' pool and threw away
GigaVoxels' feedback loop.

Numbers, RTX 3060 Ti, Vulkan (§4.1–4.2). San Miguel at 64K³: **23,233 MB on disk, 424 MB resident** —
2% resident. Against HashDAG: 424 MB vs 3,475 MB at 64K (8.2×), 386 vs 1,128 at 32K, 293 vs 352 at
16K. ~6 ms/frame at 64K; 2–4× faster than HashDAG at ≥32K.

**And §4.4 says the quiet part:** the framework "**does not support runtime modification of voxels**".
Their own suggestion for a Minecraft-like game is a hybrid — "dense array near player, meshes for
LOD 0, SVDAG for LOD 1+".

That recommendation is worth taking seriously, because it is what our engine already is: a dense
editable near field, and (since `FAR_FIELD.md`) a coarser far field. Aokana, arriving at open-world
voxel streaming from the DAG direction in 2025, recommends the architecture we arrived at from the
other direction in 2026.

### 3.5 Nanite — the highest-profile virtualised-geometry system, and it reads back to the CPU

Karis, Stubbe, Wihlidal, *Nanite: A Deep Dive*, SIGGRAPH 2021 Advances in Real-Time Rendering.
<https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf>

Slide 30, p.30, states the GigaVoxels thesis in three lines: "Entire tree doesn't need to be in
memory at once / Can mark any cut of the tree as leaves and toss the rest / Request data on demand
during rendering / **Like virtual texturing**."

The loop, slide 127, p.127 — read this carefully, because it is the strongest evidence about
current best practice:

> "Persistent shader outputs page requests during culling traversals / Requests page ranges with
> priority based on LOD Error / Updates priorities of already loaded pages / **Asynchronous CPU
> readback of requests** / Add any missing DAG dependencies / Issue IO page requests for pages with
> highest total priority / **Evicts low-priority pages** / Handle completed IO requests / Install
> pages on GPU / Patch GPU-side pointers"

**Epic, in 2021, with a AAA budget, chose GPU-requests plus CPU-policy — the shape of the 2009 paper,
not the shape of the 2011 thesis.** Note also that requests are emitted "even for resident pages to
update their priorities", which is exactly GigaVoxels' usage-timestamp refresh; and that the policy
is **priority by LOD error**, not recency. That is a meaningful refinement: LRU is a proxy for "will
be needed again", and LOD error is a direct estimate of it.

Compression, p.145: Raw 25.90 GB → memory format 7.67 GB → compressed disk format 4.61 GB. Two
representations, one random-access for rendering, one transcode-on-stream for disk. Transcoding runs
"~50 GB/s […] on PS5".

### 3.6 Virtual texturing — where the feedback loop became routine

The GPU-feedback idea predates GigaVoxels in 2D (id Tech 5 / MegaTexture, ~2007) and is now standard.
A good current practitioner account: <https://www.shlom.dev/articles/how-virtual-textures-really-work/>.
The loop: a **reduced-resolution feedback pass** writes (page index, mip) packed to a few bits; the
CPU page manager decodes it, splits requests into resident (mark used this frame) and missing (async
load); "If the cache is full, the **least recently used page** is selected for eviction"; and
low-detail pages are **pinned** so there is always something valid to sample.

Two transferable details: **the feedback pass runs at reduced resolution**, because request
information is far more spatially coherent than shading; and **pinning** is how you make a
partially-resident structure never produce a hole. GigaVoxels' equivalent of pinning is falling back
to a coarser mipmap (PAPER §6, p.5), which is the same trick.

DX12 **Sampler Feedback** moved the feedback pass into fixed-function hardware for textures. There is
no equivalent for a hand-rolled voxel DDA — we would write the feedback ourselves.

---

## 4. Editable structures, ranked by how well they tolerate an edit

This is the axis that decides it for a sandbox, and it is where the compression literature falls over.

**Rank 1 — flat / directory-plus-pool grids of bricks. Edits are O(edited voxels). No recompression.**

The brickmap (**Thijs van Wingerden**, *Real-time Ray tracing and Editing of Large Voxel Scenes*, MSc
thesis, **Utrecht University, 2015**, <https://studenttheses.uu.nl/handle/20.500.12932/20460>;
author/year/institution verified against <https://hgpu.org/?p=14319> because the UU repository returns
403 — **note for whoever reconciles this with `GIGAVOXELS_NOTES.md`, which cites the same handle as
"van Straaten, 2020"; the recorded metadata says van Wingerden, 2015**) is the canonical
statement: a top-level **brickgrid** indexing fixed **8×8×8 brickmaps**, with an occlusion-based
streaming system, and the explicit design argument that keeping data **raw** — no hierarchical
compression, no preprocessing — is what makes it "directly editable". Open implementations:
<https://github.com/stijnherfst/BrickMap>, <https://github.com/dubiousconst282/VoxelRT>.
(I could not retrieve the thesis PDF itself — Utrecht returns 403 to non-browser clients — so the
structural description is from the abstract, the Semantic Scholar record, and
<https://uygarb.dev/posts/0003_brickmap_rundown/>. Flagged in §8.)

**This is what voxl2 already is**, minus the pool: our `VoxelLeafChunk` is a 4 m brick with palette
compression, and the brickgrid is exactly the directory `SCALE_LIMITS.md` §3.3 proposes.

**Rank 2 — VDB-style pooled hierarchies (GVDB).** Designed for dynamic topology; pool reallocation on
overflow. Costs a rebuild of affected nodes, not of the scene. Real caveat: GVDB is a 2016 codebase,
CUDA-only, and NVIDIA's repo is effectively dormant (the active fork is nTopology's).

**Rank 3 — HashDAG.** Careil, Billeter & Eisemann, CGF 39(2), 2020, doi:10.1111/cgf.13916. Code and
paper PDF: <https://github.com/Phyronnaz/HashDAG>.

The most impressive result in this section and still, for us, a trap. What it costs:

- **Trace overhead 1.5×–2×.** §4, p.6: "Our virtual memory scheme introduces an overhead of 1.5× to
  2× for the raytracing of the geometry". Attribute resolution needs a second SVO traversal and
  "introduces an overhead of similar magnitude". On a frame that is 97.5–99.4 % GPU-bound
  (`PROFILE.md`), paying 1.5–2× on the trace to save memory we are not short of is backwards.
- **Edits run on the CPU.** §4, p.5: "Our implementation performs modifications on the CPU and renders
  the DAG structures on the GPU, using CUDA." Multithreaded; scales to ~4 threads then goes memory
  bound.
- **Garbage collection is mandatory.** §3.5, p.5: every modification appends new nodes and leaves the
  old DAG valid (free undo/redo, but unbounded growth). GC walks the reachable set level by level and
  compacts.
- **Memory overhead is large and tuning-sensitive.** §4, p.7: Epic Citadel at (64k)³ is 380 MB of
  data, and the structure adds ≈105 MB (128-word pages) / 260 MB (256) / 550 MB (512); by bucket
  count, ≈105 MB (2¹⁶) / 480 MB (2¹⁸) / **1165 MB (2²⁰)**.
- Hardware: RTX 2080, i7-8700K. The demo needs "at least 8GB of VRAM (6 might work too)".

**Rank 4 — static SVDAG.** Kämpe, Sintorn & Assarsson 2013 achieve (128k)³ in under 1 GB. Editing
requires de- and re-compression of every affected subtree up to the root, and any edit that breaks a
shared subtree forces a split. The brief's instinct that "DAG compression is famously hostile to
editing" is correct and the field agrees — HashDAG exists precisely because of it, and Aokana simply
declines to support editing.

**Rank 5 — NanoVDB.** Static topology by construction (§3.2). Editing means regenerating the grid.

**Rank 6 — hardware sparse residency (Vulkan `sparseResidency` / DX12 tiled resources).** Worth
naming because it looks like a free answer — let the MMU do the paging — and it is not. Reported on
NVIDIA's own developer forum
(<https://forums.developer.nvidia.com/t/sparse-texture-binding-is-painfully-slow/259105>):
"multiple seconds (yes, not milliseconds) to bind 1000 pages in a 1024³ texture" on an A4000 / driver
535; another user "0.25 ms to bind 1 sparse page" but "3600 ms to bind 9200 sparse pages (390 µs per
page average)"; and cost reported as "proportional to the number of pages that are already bound in
all sparse textures". NVIDIA acknowledged it as tracked internally with no timeline, and
`vkQueueBindSparse` blocking defeats client-side workarounds.

> **At 390 µs per page, binding a single page costs more than our entire frame. Hardware sparse
> binding is unusable as a per-frame streaming mechanism.** A software page table in a plain storage
> buffer — one indirection in the shader — is both faster and portable.

---

## 5. What shipping voxel games actually do

Weaker evidence than the papers, and I want to be explicit about how much weaker. Games publish
blog posts and conference talks, not methods sections; several of the entries the brief asked for
have no primary technical source at all.

### 5.1 Teardown — the best-documented, and it just independently endorsed our plan

Dennis Gustafsson, Voxagon Blog, <https://blog.voxagon.se/>.

Shipped Teardown: voxel volumes, no triangles, GPU marches along the ray. **8-bit palette per voxel —
one byte, up to 255 materials.** No true GI: ray tracing is used for ambient occlusion, soft shadows
and specular occlusion.

The next engine — *Year summary*, 2024-12-29,
<https://blog.voxagon.se/2024/12/29/year-summary.html>:

> a "sparse voxel format that splits each shape into **8x8x8 voxel 'chunks', which are tracked with a
> 3D bitmap**", which "saves a lot of memory and enables shapes to put voxels everywhere inside the
> shape without worrying too much about empty space."

Plus hardware ray tracing "using intersection shaders, still no triangles", DLSS Ray Reconstruction,
and "unlimited world size […] true reflections and global illumination, since all scene information
is available on the GPU."

**A shipped commercial voxel game, rewriting its engine in 2024, chose a bitmap directory over 8³
chunks.** That is the same structural answer as our census, as GVDB's pools, and as the brickmap. It
is the strongest single piece of evidence in this document that the directory-plus-pool step is the
right one, because it is the only one that comes with a shipping product and a business behind it.

Note also what he did *not* choose: no DAG, no LRU, no ray-guided streaming. Just sparse chunks.

### 5.2 Minecraft Bedrock RTX

NVIDIA + Microsoft, GTC 2020, "Crafting a Real-Time Path-Tracer for Minecraft RTX" (Boksansky et al.),
<https://developer.nvidia.com/blog/gtc-digital-crafting-a-real-time-path-tracer-for-minecraft-rtx>.
The public material covers the path tracer and, at length, the denoisers, plus the PBR material
pipeline. **The chunk/BLAS memory budget and the streaming model are not published.** UNDETERMINED.

### 5.3 Enshrouded

Keen Games' "Holistic Engine", voxel terrain with full destructibility and building. I found **no
primary technical source** — no GDC talk, no engineering blog, no paper. Everything returned by
search was SEO-generated marketing copy with no numbers in it, and I am not going to cite that.
UNDETERMINED, and I would treat any figure attributed to Enshrouded's engine with suspicion unless it
traces to Keen directly.

### 5.4 No Man's Sky, John Lin, Douglas Dwyer

- **No Man's Sky** — the published material is about procedural generation (superformula, planet
  generation), not about a voxel residency cache. Not a useful comparable. UNDETERMINED.
- **John Lin** — an impressive path-traced voxel engine with public captures
  (<https://voxely.net/blog/the-perfect-voxel-engine/>), but the data structure and streaming model
  are not documented in any detail I could verify. Screenshots are not a method. UNDETERMINED.
- **Douglas Dwyer** — the Octo voxel game engine, <https://github.com/DouglasDwyer/octo-release>.
  Release repository; no published architecture document. UNDETERMINED.

**Summary of §5:** exactly one shipping voxel game (Teardown) documents its structure well enough to
learn from, and what it documents is a sparse chunk grid with a bitmap directory. Everything else the
brief asked about is undocumented. The papers are, in this instance, better evidence than the games.

---

## 6. Ranked for our case

Constraints, all measured, all in this repository: RTX 3050 6 GB laptop; 16 voxels/m near the player
and `LOG2_VOXEL_SIZE` is not moving; path-traced GI at 53.6 % of frame (`PROFILE.md`); live editing;
frame 97.5–99.4 % GPU-bound; `frame_ms = 3.030 + 7.520 × internal-Mpx`; one developer.

### 6.1 The measurement that reframes everything

MEASURED, `SCALE_LIMITS.md` §3.3, produced by `scratchpad/scale_sweep.ps1` running

```
gvox_engine.exe --unpause --exit-after <S> --screenshot docs\images\scale\<row>.png
    --screenshot-after <S-4> --width 1280 --height 720
    --pos <X,Y,Z> --rot <yaw,pitch> --no-overlay --bench-csv <row>.csv
    --render-scale 1.0 --gi true --reflections false --shadows true
```

with `VOXL_DATA_DIR` set fresh per row, and `log_table_census()` in
`voxels/impl/voxel_world.cpp` printing at end of generation:

```
[scale] table census: 262144 chunks | uniform 261297 (99.7%) | header-only 0 (0.0%) |
        paletted 847 (0.3%) | paletted regions 105535 of 134217728 (0.08%)
[scale] dense table 2153.8 MB -> pooled equivalent 9.1 MB (237.8x smaller); directory alone 2.10 MB
```

**847 chunks hold the entire island. 2153.8 MB of VRAM holds 9.1 MB of information.**

### 6.2 Therefore: when do we need an LRU at all?

DERIVED, and this is the number the next session should plan against.

- Content: 847 chunks × 8216 B = **6.96 MB** for the ~80 m island (MEASURED, above).
- Pool budget: `FAR_FIELD.md` §6 notes render targets already want ~2.1 GB on a 6 GB card, and §6
  flags 830 MB of allocator slack as a defect to fix before L2. Allow the body pool **1.5 GB**.
- 1.5 GB / 8216 B = **182,600 chunks**, i.e. **215×** the current content.
- Terrain is a *surface*: content chunks scale with area, not volume. √215 = 14.7.
- 80 m × 14.7 ≈ **1,175 m**.

> **A ~1.2 km world of the same surface character fits entirely in a 1.5 GB pool with no eviction
> policy whatsoever.** Today's world is 37 m of content in a 64 m box. The LRU is roughly a 30×
> linear extent away from being needed.

Two honest caveats. Caves and overhangs make content grow faster than area — the demo world is
"thin, noisy and everywhere" (`PROFILE.md`) and would cost more. And the far field multiplies levels:
`FAR_FIELD.md` §6 warns that four levels reach ~3.3 GB of *capacity* for a few hundred MB of voxels,
which is an allocator-slack problem, not a content problem, and is the thing to fix first. Neither
caveat moves the conclusion by an order of magnitude.

### 6.3 And if we did build the GPU LRU, what would it cost here?

DERIVED, from THESIS Fig 7.25 plus `PROFILE.md`.

The thesis costs 0.19 ms at 16,384 pages and 0.75 ms at 1,048,576 pages. Our pool would be
~4k–32k pages — the *left edge* of that chart, where the curve is flat and dominated by dispatch
overhead. Two stream compactions over 32,768 4-byte references is ~131 KB per pass, a handful of
passes: bandwidth-trivial on any modern card.

The real cost is dispatch count, and this engine has a measured price for that. `PROFILE.md`: "about
60 of them cost under 0.05 ms each and total 0.6 ms" — a mean of ~0.01 ms for a trivial pass. A cache
manager is roughly 5 such passes (flag, scan, scatter ×2, patch).

> **Estimate: +0.05 to +0.25 ms/frame, or 0.5–2 % of an 11 ms frame.** Affordable. Just not yet
> necessary — and against a 3.030 ms fixed floor, not free either.

### 6.4 The ranking

**Tier 1 — do this, and it is not a cache.**

1. **Directory plus body pool.** MEASURED 12.9×/36.9×/237.8× at CPA 16/32/64. Endorsed independently
   by GVDB (25×, §3.1), by the brickmap (§4), and by Teardown's 2024 rewrite (§5.1). Edits stay
   O(edited voxels). No feedback loop, no eviction, no readback, no pop-in, **no new failure mode
   that can put a hole in the image**. `SCALE_LIMITS.md` §3.3 already scopes it — the pool allocator
   exists (`DECL_SIMPLE_ALLOCATOR`) — and argues it should also be *faster*, because a CPA 64
   directory is 2.10 MB and **fits in L2**, whereas today a DDA step in air touches two cache lines
   4 KB apart and consecutive chunks are 8216 B apart so they never share a line. Given that a miss
   ray is the more expensive ray to march (**2.7×**, per the current A/B re-measurement; the earlier
   per-pass figure in `PERFORMANCE_PLAN.md` §5 is **3.2×** for `TracePrimaryCompute` on a sky-heavy
   frame versus against a wall, 1.626 vs 0.508 ms — different measurements, same direction), the case
   that gets cheaper is the case that dominates.
   **This is the whole recommendation.** Everything below is contingent on it.
2. **Kill the CPU mirror, or shrink it.** `SCALE_LIMITS.md` §3.1 found a second 8192 B/chunk copy in
   host RAM that no document had counted — 2147.5 MB at CPA 64. The pool must fix both sides or it
   fixes half the problem.

**Tier 2 — plausible next, in this order.**

3. **Feedback without eviction.** If a residency signal is wanted, emit it from the march the way
   GigaVoxels §6.1.1 and Nanite p.127 do, and use it *only to prioritise generation* — which we
   already meter at `MAX_CHUNK_UPDATES_PER_FRAME` = 128. This gets ray-guided *production* (the thing
   that makes a big world usable) without ray-guided *eviction* (the thing that risks holes). Cheap,
   reversible, and it is the piece that composes with the far field. Note the far-field lesson
   though: `FAR_FIELD.md` §5.4 showed ray *class* matters enormously — a feedback pass should be
   driven by primary rays, not by every GI ray, or it will request the world.
4. **GPU LRU over the pool, timestamp + two stream compactions.** The thesis design, verbatim. Only
   when §6.2's 1.2 km is in sight. Budget +0.05–0.25 ms.

**Tier 3 — research projects. Do not start these.**

5. **HashDAG.** 1.5–2× trace overhead on a GPU-bound frame, CPU-side edits, mandatory GC, and up to
   1165 MB of tuning-dependent overhead. It buys memory we have measured we do not need.
6. **SVDAG / Aokana-style chunked DAG.** Aokana's own §4.4 says no runtime edits, and recommends a
   dense near field for a Minecraft-like game — which we have.
7. **NanoVDB.** Static topology. Excellent library, wrong problem.
8. **Hardware sparse residency.** 390 µs per page bind. Ruled out on measured third-party numbers.
9. **Move-to-front swapping as described in the brief.** Does not exist in the literature, would need
   atomics, and the locality benefit that motivates it is not real (§2.3).

### 6.5 The one thing that is not a cache problem at all

Worth stating because it is adjacent and cheaper than any of the above. `SCALE_LIMITS.md` §4:
generation is `CPA³ / MAX_CHUNK_UPDATES_PER_FRAME` **frames** — 2672 frames at CPA 64, ~41 s
uncontended. A pool makes the table small; it does not make generation faster. **A world that streams
must also generate on demand, and today the generation rate is a fixed per-frame constant with no
notion of what is visible.** Whatever residency signal §6.4 item 3 produces should drive that budget.
This is likely a larger practical win than the LRU and is a fraction of the work.

---

## 7. Answers to the five questions, compressed

1. **What superseded GigaVoxels?** Nothing wholesale; it forked. The **pool** became GVDB (2016) and
   NanoVDB (2021). The **feedback loop** became virtual texturing and Nanite (2021). The
   **compression** became SVDAG (2013) → HashDAG (2020) → Aokana (2025). The original authors' group
   is still working on it — GigaVoxels DP, HPG 2024, 2× gain against GPU core starvation. The most
   directly relevant modern paper for us is **Aokana**, and its main lesson is a negative one: it
   drops editing.
2. **GPU LRU best practice?** **Timestamp plus periodic bulk compaction** — specifically two
   order-preserving stream compactions per cache per frame (THESIS §7.3.4). Not clock, not
   second-chance, and emphatically **not per-element move-to-front swapping**, which nobody does
   because it needs atomics. Cost is **O(pool), not O(touched)**, 0.19–0.75 ms for 16k–1M pages on
   2011 hardware. Current *shipping* practice (Nanite, virtual texturing) is more conservative still:
   **GPU emits requests, CPU decides policy**, and priority is by **LOD error**, not recency. The
   locality argument for swapping is false: the list holds references, the data never moves.
3. **Editable, ranked?** Flat/brickmap grids ≫ VDB pools ≫ HashDAG ≫ static SVDAG ≫ NanoVDB. DAG
   compression is as hostile to editing as the brief suspected; the two 2025-era systems that use it
   for games (Aokana) or that fixed it (HashDAG) either decline to edit or pay 1.5–2× on every ray.
4. **Shipping games?** Only **Teardown** is documented well enough to learn from, and its 2024 rewrite
   chose **8³ chunks tracked by a 3D bitmap** — a directory over a pool. Minecraft RTX, Enshrouded,
   No Man's Sky, John Lin and Octo are all undocumented at this level. §5.
5. **Realistic for us?** The directory-plus-pool. That is it. It is measured at 237.8× at CPA 64, it
   is what three independent lines of work converged on, it preserves O(edit) editing, it should make
   the miss ray — our dominant cost — cheaper, and it introduces no mechanism that can put a hole in
   the frame. The LRU is a real technique that solves a real problem we will not have until roughly a
   1.2 km world.

---

## 8. What I could not determine

1. **GigaVoxels DP (HPG 2024) full text.** HAL serves it behind an Anubis proof-of-work challenge;
   ACM DL is paywalled. §3.3 rests on the abstract and landing page. The "2× gain" is unqualified as
   to scene and hardware.
2. **The brickmap thesis PDF.** `studenttheses.uu.nl` returns 403 to non-browser clients.
   Structure in §4 is from the abstract, Semantic Scholar, and a third-party rundown whose author
   states the paper "stops too short" on streaming and LOD. **Brick byte sizes, the streaming
   residency policy, and any performance numbers are unverified.**
3. **Minecraft Bedrock RTX memory budget and streaming model.** Not published.
4. **Enshrouded's engine.** No primary technical source exists that I could find. All search results
   were generated marketing copy.
5. **No Man's Sky, John Lin's engine, Douglas Dwyer's Octo** — no published architecture at the level
   needed to compare.
6. **Where the "1 MB pool" and "64 GB" figures in the brief came from.** I found their likely
   originals (THESIS Fig 7.25's smallest x-axis point, and THESIS §5.2's pointer-encoding limit) but
   the summary the user was given is not a faithful reading of either, and I could not identify the
   secondary source that garbled them. **Treat that summary as unreliable in full.**
7. **Whether anyone runs a fully GPU-resident cache with no CPU readback in a shipping game.** I
   searched specifically and found none. Every documented shipping system (Nanite, virtual texturing)
   reads back. Absence of evidence, but the absence is consistent across every system I could check.
8. **Our actual per-frame working set** — how many distinct chunks the rays touch in one frame, as
   opposed to how many exist. Nobody has measured it. **It is the single measurement that would most
   sharpen §6.2**, it needs one atomic-tagged counter in `sample_lod`, and it is the cheapest
   experiment in this document.

---

## 9. Sources

**Primary papers**
- Crassin, Neyret, Lefebvre, Eisemann. *GigaVoxels: Ray-Guided Streaming for Efficient and Detailed
  Voxel Rendering.* I3D 2009. <https://maverick.inria.fr/Publications/2009/CNLE09/CNLE09.pdf>
- Crassin. *GigaVoxels: A Voxel-Based Rendering Pipeline For Efficient Exploration Of Large and
  Detailed Scenes.* PhD thesis, Grenoble, 2011. Ch.7 esp. §7.3.4, §7.5.4.
  <https://maverick.inria.fr/Membres/Cyril.Crassin/thesis/CCrassinThesis_EN_Web.pdf>
- Richermoz, Neyret. *GigaVoxels DP: Starvation-Less Render and Production…* HPG 2024.
  doi:10.1145/3675389. <https://hal.science/hal-04654692v1>
- Hoetzlein. *GVDB: Raytracing Sparse Voxel Database Structures on the GPU.* HPG 2016.
  <https://ramakarl.com/pdfs/2016_Hoetzlein_GVDB.pdf> · code <https://github.com/NVIDIA/gvdb-voxels>
- Museth. *NanoVDB: A GPU-Friendly and Portable VDB Data Structure.* SIGGRAPH Talks 2021.
  doi:10.1145/3450623.3464653 · FAQ
  <https://github.com/AcademySoftwareFoundation/openvdb/blob/master/doc/nanovdb/FAQ.md>
- Careil, Billeter, Eisemann. *Interactively Modifying Compressed Sparse Voxel Representations.*
  CGF 39(2), 2020. doi:10.1111/cgf.13916 · code + PDF <https://github.com/Phyronnaz/HashDAG>
- Kämpe, Sintorn, Assarsson. *High Resolution Sparse Voxel DAGs.* SIGGRAPH 2013.
  doi:10.1145/2461912.2462024
- Molenaar, Eisemann. *Editing Compressed High-resolution Voxel Scenes with Attributes.* CGF 2023.
  doi:10.1111/cgf.14757
- Xu et al. *Aokana: A GPU-Driven Voxel Rendering Framework for Open World Games.* I3D 2025.
  arXiv:2505.02017 <https://arxiv.org/html/2505.02017v1> · doi:10.1145/3728299
- van Wingerden, Thijs. *Real-time Ray tracing and Editing of Large Voxel Scenes.* MSc, Utrecht
  University, 2015. <https://studenttheses.uu.nl/handle/20.500.12932/20460> · metadata verified via
  <https://hgpu.org/?p=14319> (UU repository 403s to non-browser clients)
- Karis, Stubbe, Wihlidal. *Nanite: A Deep Dive.* SIGGRAPH 2021 Advances.
  <https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf>

**Practitioner and industry**
- Gustafsson. Voxagon Blog. <https://blog.voxagon.se/> · *Year summary*, 2024-12-29
  <https://blog.voxagon.se/2024/12/29/year-summary.html>
- *Teardown Developer Breaks Down Multiplayer and Voxel Destruction Tech*, 80.lv
  <https://80.lv/articles/teardown-developer-breaks-down-multiplayer-and-voxel-destruction-tech>
- NVIDIA. *Crafting a Real-Time Path-Tracer for Minecraft RTX*, GTC 2020.
  <https://developer.nvidia.com/blog/gtc-digital-crafting-a-real-time-path-tracer-for-minecraft-rtx>
- *How Virtual Textures Really Work.* <https://www.shlom.dev/articles/how-virtual-textures-really-work/>
- Vulkan sparse binding cost thread, NVIDIA Developer Forums.
  <https://forums.developer.nvidia.com/t/sparse-texture-binding-is-painfully-slow/259105>
- Vulkan Guide, Sparse Resources. <https://docs.vulkan.org/guide/latest/sparse_resources.html>
- Ria (Bink). *Fast Voxel Data Structures*, 2024. <https://bink.eu.org/fast-voxel-datastructures/>
- BrickMap rundown. <https://uygarb.dev/posts/0003_brickmap_rundown/>

**This project** — all measured on the RTX 3050 6 GB laptop
- `C:\voxl2\docs\SCALE_LIMITS.md` §3.1, §3.3, §4, §9.1 — the census, the CPU mirror, the commands
- `C:\voxl2\docs\PROFILE.md` — per-pass breakdown, GI at 53.6 %, the trivial-pass cost
- `C:\voxl2\docs\FAR_FIELD.md` §5.4, §6 — ray class, allocator slack
- `C:\voxl2\docs\design\PERFORMANCE_PLAN.md` §3.3, §5 — miss-vs-hit cost, far-field design
