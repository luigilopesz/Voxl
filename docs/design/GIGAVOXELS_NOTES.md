# GigaVoxels — an implementable specification, and an assessment against voxl2

Research and design only. Nothing in `C:\voxl2` was modified to produce this document except
this file. No refactor is proposed for this session.

Every claim below is tagged:

| Tag | Meaning |
|---|---|
| **PAPER** | Crassin, Neyret, Lefebvre, Eisemann, *GigaVoxels: Ray-Guided Streaming for Efficient and Detailed Voxel Rendering*, I3D 2009 says it |
| **THESIS** | Crassin's 2011 PhD thesis says it (far more detailed; supersedes the paper where they differ) |
| **SOURCE** | I read it in the shipped GigaVoxels/GigaSpace source, v1.0.126 |
| **LATER WORK** | Someone other than the GigaVoxels authors established it |
| **MEASURED** | Measured on this machine, in this repository, with the command given |
| **DERIVED** | My arithmetic on top of the above. The inputs are cited; the conclusion is mine |
| **UNVERIFIED** | I could not confirm it. Flagged, not hidden |

## Sources

| # | Source | URL | Retrieved |
|---|---|---|---|
| S1 | Crassin, Neyret, Lefebvre, Eisemann. "GigaVoxels: Ray-Guided Streaming for Efficient and Detailed Voxel Rendering." *I3D 2009.* 8 pp. | https://maverick.inria.fr/Publications/2009/CNLE09/CNLE09.pdf | full text extracted |
| S2 | Crassin. "GigaVoxels: A Voxel-Based Rendering Pipeline For Efficient Exploration Of Large And Detailed Scenes." PhD thesis, Université de Grenoble, July 2011. 184 pp. | http://maverick.inria.fr/Membres/Cyril.Crassin/thesis/CCrassinThesis_EN_Web.pdf | full text extracted |
| S3 | GigaVoxels / GigaSpace SDK v1.0.126, BSD-3. Source **is still available**: 549,945,122 B Linux tarball, 552,602,032 B Windows SDK, both HTTP 200 as of this session. | https://gigavoxels.inria.fr/download.html · http://gigavoxels.imag.fr/download/Linux/gigaspace_1.0.126~trusty.tar.gz | cache + page-table sources extracted and read |
| S4 | Project page / "What is GigaVoxels" | http://gigavoxels.inrialpes.fr/WhatIsGigaVoxels.html | read |
| S5 | GitHub maintenance mirror (PascalGuehl). **Contains no code** — one PNG, a README and a licence. Do not send anyone here for the source. | https://github.com/PascalGuehl/GigaVoxelsGigaSpace | read |
| S6 | Richermoz, Neyret. "GigaVoxels DP: Starvation-Less Render and Production for Large and Detailed Volumetric Worlds Walkthrough." *HPG 2024*, PACMCGIT. DOI 10.1145/3675389 | https://dl.acm.org/doi/10.1145/3675389 · https://hal.science/hal-04654692v1 | **metadata only** — see §10 |
| S7 | van Straaten. "Real-time Ray tracing and Editing of Large Voxel Scenes." MSc thesis, Utrecht University, 2020 ("BrickMap"). | https://studenttheses.uu.nl/handle/20.500.12932/20460 · impl: https://github.com/stijnherfst/BrickMap | **abstract + README only** — see §10 |
| L1 | `docs/SCALE_LIMITS.md` — the world-size sweep | in repo | — |
| L2 | `docs/design/PERFORMANCE_PLAN.md`, `docs/FAR_FIELD.md`, `docs/PROFILE.md`, `docs/DENSITY_LIMITS.md` | in repo | — |

---

# 0. The verdict, up front

**GigaVoxels solves two separate problems. voxl2 has one of them and not the other, and the half
voxl2 needs is the cheap, safe half.**

The two halves are:

1. **Sparsity** — a *page table + fixed-size page pool*, so that storage is proportional to
   content rather than to volume. This is the node-pool / brick-pool split of §4 (S1 §4.1;
   S2 §5.2.1).
2. **Residency** — a *ray-guided request loop with LRU eviction*, so that the resident set is
   proportional to what is *visible* rather than to what *exists*. This is §7 of the thesis.

The brief's premise is that residency is what makes voxl2's world unbounded. **The measurement in
this repository says otherwise.** From L1 §3.3, the engine's own census at `CHUNKS_PER_AXIS 64`:

```
[scale] table census: 262144 chunks | uniform 261297 (99.7%) | header-only 0 (0.0%) |
        paletted 847 (0.3%) | paletted regions 105535 of 134217728 (0.08%)
[scale] dense table 2153.8 MB -> pooled equivalent 9.1 MB (237.8x smaller); directory alone 2.10 MB
```

**MEASURED (L1 §3.3):** a 256 m world's entire *content* is **9.1 MB** in a pooled layout. The
2153.8 MB is the dense table charging for empty boxes. At `CHUNKS_PER_AXIS 32` — the shipped
recommendation — it is 269.2 MB dense against **7.3 MB** pooled, a 36.9× overcharge.

**DERIVED.** A 9.1 MB working set on a 6144 MiB card never evicts anything. An LRU is a policy for
deciding what to throw away when the pool is full. voxl2's pool would be 0.15 % full. **The LRU
is answering a question voxl2 is not asking.**

So:

| Half of GigaVoxels | Buys voxl2 | Costs voxl2 | Verdict |
|---|---|---|---|
| **Page table + pool** (§4) | 36.9× at CPA 32, 237.8× at CPA 64, measured; and the directory fits in L2 so the empty-space march gets *faster* (L1 §3.3) | a directory indirection, a GPU pool allocator that already exists (`DECL_SIMPLE_ALLOCATOR`) | **Do this.** It is already specified in L1 §3.3 and this document validates it against the paper |
| **Ray-guided request loop** (§7.3.3, §7.3.5) | the ability to *generate* content on demand — an unbounded procedural world rather than a pre-generated box | a request buffer, a compaction pass, a producer pass, and a multi-frame convergence artefact | **Later, and for a different reason than memory.** See §8.3 |
| **LRU eviction** (§7.3.4, §7.3.7) | nothing at present content density | breaks live editing (§7), risks the GI stack (§6.7) | **Do not do this.** Revisit only when a measured working set approaches the pool |
| **The octree above chunk level** | the stride the far field needs — fact #1 in the brief | makes an edit O(depth) instead of O(1) — §7.2 | **The real trade. Read §7 before choosing.** |

One more finding that reframes the whole thing, and it is the single most useful sentence in
184 pages of thesis. **THESIS (S2 §10, Perspectives, p. 182):** speaking of his own
dynamic-object scheme, Crassin writes that it *"is not compatible with our on-demand loading
scheme"*, and that pre-filtering *"has to be done from bottom to top […] which means that a high
resolution voxelization is always needed, whatever the resolution actually required for
rendering."*

**Streaming is top-down and refuses to keep the leaves resident. Pre-filtering is bottom-up and
requires exactly that.** The author of GigaVoxels says his own system cannot do both. That is
§7 of this document, and it is the dealbreaker the brief predicted.

---

# 1. The data structure

## 1.1 Why bricks and not per-voxel nodes

**PAPER (S1, Preliminaries, p. 3):** a brick is *"a small voxel grid of some predefined size M³
[…] that approximates the part of the original volume that corresponds to the octree's node."*
Four reasons, all given explicitly:

1. **Hardware trilinear interpolation and 3D texture cache.** Bricks live in a 3D texture; a
   per-voxel octree cannot use the texture units at all (S1 §4; S2 §5.1.1).
2. **The tree gets shallower by log₂M levels.** With M = 8 the octree loses 3 levels of pointer
   chasing per traversal.
3. **Constant-size records.** Both nodes and bricks are fixed size, so they pool trivially and
   never fragment (S2 §5.1.1). This is what makes the whole cache mechanism possible — see §3.4.
4. **The mip pyramid falls out for free.** Interior-node bricks are downsampled children, which is
   what gives cone tracing its anti-aliasing (S1 §5.2; S2 §5.1.1).

**THESIS (S2 §5.3, p. 104-105) — the measured brick-size trade-off, and it is not what people
assume.** On Sponza and Coral pre-voxelized to 512³:

| Brick resolution | Sponza brick storage (MB) | Sponza octree (MB) |
|---|---|---|
| 2³ | 19.52 (no border) / 65.89 (bordered) | 8.766 |
| **4³** | 35.06 / **68.48** | **1.935** |
| 8³ | 61.92 / 88.17 | 0.408 |
| 16³ | 104.34 / 125.16 | 0.078 |
| 32³ | 159.38 / 191.16 | 0.012 |

Optimum is **4³ corner-centred**: 68.48 MB bricks + 1.93 MB octree = **70.41 MB**, versus
**1023 MB** for a dense 512³ RGBA8 mip pyramid — **6.88 % of dense**. Coral: 4.9 %.

And separately, **THESIS (S2 §6.3.2, p. 115-116, GTX 480, Mandelbulb):** *smaller bricks are also
faster.* 141.24 FPS at 4³, 128.21 at 8³, 109.89 at 16³. Small bricks win on both axes.

**The catch, and it is large. THESIS (S2 §5.1.4, Fig. 5.6, p. 99):** bricks must replicate
boundary voxels so the hardware can interpolate across brick edges. The overhead factor:

| Brick res | 2 |4 |8 |16 |32 |64 |
|---|---|---|---|---|---|---|
| 2-voxel border, node-centred | 8.00 | 3.38 | 1.95 | 1.42 | 1.20 | 1.10 |
| 1-voxel border, corner-centred | 3.38 | 1.95 | 1.42 | 1.20 | 1.10 | 1.05 |

At the optimum 4³ corner-centred the border costs **95 %** — it nearly doubles brick storage
(S2 §5.3). That is already priced into the 70.41 MB above. **This border is also the thing that
makes an edit expensive; see §7.2.**

## 1.2 Node encoding — the exact layout

**PAPER (S1 §4.1, Fig. 5, p. 4)** and **THESIS (S2 §5.2.2, Fig. 5.10, p. 102)**. A node is
**two 32-bit words = 8 bytes**. **SOURCE (S3, `GvCore/GsPageTableKernel.inl`)** confirms the bit
masks in shipping code:

```
word 0 — structure
  bits  0..29   childIdx : index of the child NODE TILE in the node pool (0 == no children)
  bit   30      terminal : 1 == the source volume has no more detail here    [0x40000000]
  bit   31      brick-present / node-has-data flag                            [0x80000000]
                (mask 0x3FFFFFFF isolates the 30-bit address — seen in setPointerImpl)

word 1 — data
  if the "constant" bit is set:  an RGBA8 constant value for a homogeneous region
  else:                          a brick pointer, encoded as 3 x 10-bit XYZ brick-pool coords
```

**PAPER (S1 §4.1, p. 4):** without high-quality mip filtering the node would compress to
**32 bits**, because then only leaves need pointers and child/brick/constant become mutually
exclusive in the same 30 bits. The second word exists *only* to pay for the mip pyramid.
Against Gobbetti et al. [GMAG08], which needed eight RGBA32 texels per node, this is a
**16× reduction** (S1 §4.1).

**SOURCE (S3, `GvStructure/GsNode.h`)** confirms the 8-byte node in shipping code —

```cpp
struct GsNode {
    uint childAddress;   // packed
    uint brickAddress;   // packed
#ifdef GV_USE_BRICK_MINMAX
    uint metaDataAddress;      // optional per-brick min/max, for extra empty-space skipping
#endif
#ifdef GS_USE_NODE_META_DATA
    uint _metaData;
#endif
};
```

— and reveals a divergence from both papers that matters a great deal to voxl2. The struct
carries this comment:

> `@todo: Rename functions isBrick() hasBrick() (old naming convention when we had constant values).`

**SOURCE. The shipped library dropped the constant-value-in-node feature.** That is the feature
S1 §4.1 and S2 §5.1.2 credit with reducing memory *"enormously"* for homogeneous regions — the
bit that says "this node is uniform, here is its value inline, do not allocate a brick." It
survives in the two published descriptions and not in the code.

**DERIVED, and this is the sharpest single point of contact between the paper and voxl2's
measurements.** That dropped feature is **exactly** the one L1 §3.3 measures as the whole win:
97.4 % of voxl2's chunks at CPA 32 are completely uniform and need one word. Whatever GigaVoxels'
reasons were, **voxl2 must keep the inline-constant bit** — see §6.2. Without it the directory
degenerates into a pointer array and the 36.9× disappears.

**Why one child pointer and not eight (S1 §4.1; S2 §5.2):** children are stored contiguously as an
**N³ node tile**. One pointer plus an integer offset addresses all eight. This is the single most
copyable idea in the paper.

**Addressing — this is where the brief's "64 GB" comes from. THESIS (S2 §5.2.2, p. 103):**
pointers do not address bytes, they address *groups*.

- Node pointer = index of a node **tile**. 2³⁰ tiles × 8 nodes × 8 B = **64 GB of addressable
  node pool.**
- Brick pointer = 3 × 10-bit XYZ. 2³⁰ bricks × 8³ voxels × 4 B = **2 TB of addressable brick
  pool.**

See §5.1 for what that claim does and does not mean.

## 1.3 The two pools

**THESIS (S2 §5.2.1, p. 100-101):**

- **Node pool** — *linear* global memory, read through the 1D/texture cache. Stored **SOA**: the
  `ChildIdx` array and the `BrickIdx` array are two separate allocations, deliberately not
  interleaved, *"since each part of the node description is not used in the same rendering
  sequence"* — better coalescing and cache hit rate.
- **Brick pool** — a large **3D texture**, for hardware trilinear filtering, 3D addressing and the
  3D-locality texture cache. Multiple *layers* (colour, normal, …) as separate allocations with a
  one-to-one element mapping.

**The elegant part, and it is genuinely clever. THESIS (S2 §7.3.2, p. 130-131):** there is no
separate page table. **The octree is the page table.** The `ChildIdx` array is the page table for
the node cache; the `BrickIdx` array is the page table for the brick cache. So the page tables are
themselves hierarchical, virtualised and cached — which is what removes any fixed ceiling on the
addressable domain, and costs *zero* extra indirection because the renderer was already chasing
those pointers.

## 1.4 A reference configuration, with real sizes

**THESIS (S2 §7.3.8, "Implementation summary", p. 141-142)** — their typical scenario, verbatim
arithmetic:

| Region | Size | Formula |
|---|---|---|
| Node pool | **16 MB** | 262 144 node tiles × 8 nodes × 8 B |
| Brick pool | **432 MB** | 48³ = 110 592 bricks × 8³ voxels × 8 B (RGBA8 colour + packed normal) |
| — node cache usage buffer | 1 MB | 262 144 × 4 B |
| — node cache LRU page list | 1 MB | 262 144 × 4 B |
| — brick cache usage buffer | 432 KB | 110 592 × 4 B |
| — brick cache LRU page list | 432 KB | 110 592 × 4 B |
| — shared request buffer | 8 MB | 2 097 152 × 4 B (one slot per **node**) |
| — shared request list | 1 MB | 262 144 × 4 B |
| — shared localization buffer | 2 MB | 262 144 × 8 B |
| **Cache bookkeeping total** | **13.84 MB** | **3.08 % overhead** on 448 MB managed |

**Note the property that matters most for voxl2 (DERIVED from S2 §7.3.3, §7.3.8):** every one of
those bookkeeping buffers is sized by the **resident pool**, not by the virtual world. The request
buffer is one slot per *resident node*, not per world position. That is precisely the property
voxl2's chunk table lacks — L1 §3.3 measured the dense table growing 8× between CPA 32 and CPA 64
for **identical content** (856 → 847 paletted chunks).

---

# 2. The ray-guided feedback loop

This is the heart of the technique and the part the brief is right to focus on.

## 2.1 The idea

**THESIS (S2 §7.1.2, p. 122-123):** *"visibility information and data requests are collected
directly during the GPU traversal of the structure for rendering, on a per-ray basis."* A ray that
front-to-back marches an ordered space subdivision, with early ray termination, visits **exactly**
the visible set at **exactly** the needed LOD. No occlusion queries, no CPU-side culling, no
mirrored tree traversal. Frustum culling, occlusion culling and LOD selection all fall out of the
march for free, in one pass.

**THESIS (S2 §7.1.2, Fig. 7.5, p. 123):** they demonstrate this by disabling loading and showing
that occluded interior geometry was never fetched at all.

**The generality is worth noting for voxl2 specifically.** Because each ray reports its own needs,
rays can be shadow rays, reflection rays, or GI rays — S2 §7.1.2 says so explicitly. That is a
feature *and*, for voxl2, a hazard: see §6.7.

## 2.2 How requests avoid an atomic storm — the key trick

**THESIS (S2 §7.3.3, p. 132-133).** The naive design is a queue with atomic append. Crassin
rejects it for two named reasons: atomics serialise writes, and you would additionally need
deduplication, *"which represents a very costly operation."*

The actual mechanism:

> A **request buffer** in global memory, **one slot per page-table entry**, laid out in one-to-one
> correspondence with the page table. A request is a **flag written to the slot for that entry**.

Three consequences, all stated in the source:

1. **Uniqueness is free.** A thousand rays wanting the same node write the same slot. One slot,
   one request. *Deduplication is structural, not algorithmic.*
2. **No atomics needed.** *"there is no need to enforce any write ordering when a concurrent
   access is done since all threads write the same request flag value."* A plain racing store is
   correct because every racer stores the same value.
3. **No clear pass.** The flag is not a boolean — it is a **32-bit timestamp** set to the current
   pass counter. *"Using such timestamps prevents us from clearing request and usage buffers at
   each frame."* An entry is "requested this pass" iff `slot == currentPass`.

**This is the single most transplantable idea in the paper and it costs almost nothing to
implement.** It is a scatter into a dense array of counters, not a queue.

**Optional refinement (S2 §7.3.3, "Fine-grained usage", p. 133):** replace the timestamp with an
*atomically incremented counter* so requests can be prioritised by how many rays wanted them
(≈ screen footprint). This **does** require atomics and **does** require clearing the buffers each
pass. S2 §7.3.5 says it *"has a more important impact on the rendering performance than the simple
boolean flag and is not used uppermost."* Take the cheap version first.

## 2.3 Turning the buffer into a work list

**THESIS (S2 §7.3.5, Fig. 7.13, p. 136).** One **stream compaction** over the request buffer,
with the predicate `slot == currentPass`, produces:

- a **compacted request list** of page-table indices needing a load, and
- **its length** — which is the number of pages to load, obtained for free from the scan.

Two implementation notes they call out: the compaction's scatter step is modified to write the
*page-table index* directly rather than reading a value; and (S2 §7.3.8, p. 141-142) the node
cache and brick cache **share one request buffer**, discriminated by a single bit, because both
are indexed by the same node domain. Since compaction turned out to be *"one of the most costly
operations done for the management of a cache"*, they run **one** compaction over the whole buffer
first to extract all valid requests, then the two per-cache compactions over that much smaller
result.

## 2.4 How many are serviced per frame

**THESIS (S2 §7.3.5, "Prioritizing requests", p. 136-137):** *"the number n of data requests
handled per frame can be limited in order to bound the time taken by the loading of data"*, and
with the counter variant the list is sorted by request weight and only the first n are answered.
**No specific n is published.** **SOURCE (S3, `GsCacheManager.inl::genericWrite`)** shows the hard
clamp actually shipped:

```cpp
if ( numElems > _numElemsNotUsed ) {
    std::cout << "CacheManager< " << cacheId << " >: Warning: " << _numElemsNotUsed
              << " non-used slots available, but ask for : " << numElems << std::endl;
    numElems = std::min( numElems, _numElemsNotUsed );
}
```

**SOURCE.** The over-subscription policy is: print a warning, silently drop the surplus requests,
and let the rays keep rendering from coarser data. There is no back-pressure and no frame-time
protection — quality degrades, frame time does not. This is the same policy the thesis describes
abstractly in §7.5.2 as *"quality reduction strategies"*. **It is worth knowing that the shipped
strategy is "drop it on the floor".**

**PAPER (S1 §6.1.2, p. 7):** the 2009 version was harder-capped still — the compacted feedback
texture was a single RGBA32 target holding four node indices per pixel, and *"if this limit is
exceeded the requests are automatically postponed to the next frame."*

## 2.5 How the 2009 paper did it, which is different and worth knowing

The 2009 feedback path is a *rasterisation-era* design and is **superseded**, but it shows what
the problem looks like without compute:

- **PAPER (S1 §6.1.1, p. 6):** rays write visited node IDs into **node-list textures** via MRTs —
  the first render target is colour, the remainder hold 4 nodes each (30-bit index + 1 subdivision
  bit). Seven such targets give 28 nodes per ray; they found **three MRTs, 12 nodes per pixel** to
  be the best choice.
- **Spatial coherence (S1 §6.1.1, Fig. 7):** rays are grouped in 2×2 packages and each pixel of
  the quad stores a *different slice* of the traversal — upper-left stores nodes 1-12, upper-right
  13-24, and so on: **48 node indices per quad**.
- **Temporal coherence (S1 §6.1.1):** the traversal pushes nodes into a FIFO; frame 1 records the
  first 48, frame 2 the next 48, sliding a 48-element window across frames.
- **Compaction without sorting (S1 §6.1.2, Fig. 8):** *"Sorting is expensive and we would further
  sort many elements that would afterwards just be deleted."* Instead a selection mask is built by
  comparing each pixel's list against its neighbours' — and only element *i* against neighbours'
  *i-1, i, i+1*, which *"can induce false positives, but it remains conservative"*. The mask is
  then reduced with **HistoPyramids** [ZTTS06] and read back to the CPU.
- **A lovely detail (S1 §6.1.1):** *"there is no need to send collapse information. If the
  rendering algorithm no longer descends into a node, its index will never be put into the list,
  thus the LRU mechanism will not reset its timestamp and thus replace the data at some point."*
  Coarsening is **the absence of a signal**. Nothing is ever explicitly freed.

**The 2011 request-buffer design is strictly better** and is what you would implement. But note
the two designs disagree about *where the loop closes*: 2009 reads back to the CPU; 2011 never
leaves the GPU.

## 2.6 The update strategies

**THESIS (S2 §7.2.1-§7.2.2, p. 125-127).** Refinement is **top-down, one level per pass**
(Fig. 7.6 walks a single ray through four passes to convergence). Three strategies:

| Strategy | Passes/frame | Priority | Behaviour |
|---|---|---|---|
| **Real-time** | 1 render + 1 update | bricks before subdivision | always produces a complete image, using a coarser mip where the right level is missing. Slower to converge — *"leads to a lot of data loaded at lower resolution before the actual needed resolution is reached"* |
| **Quality-first** | many interleaved | subdivision before bricks | rays *suspend* on a miss and resume next pass from a saved context. Correct image, unbounded frame time |
| **Balanced** | n quality passes, then one real-time pass | — | n chosen from the remaining frame budget and an estimate of the final pass |

**PAPER (S1 §6):** the justification for substituting a coarser mip is perceptual — *"it has been
shown that we perceive less details during strong motion, when it is most likely to encounter
missing data. Once the camera settles, an accurate rendering is achieved within a few frames."*

---

# 3. The LRU — the user summary versus what the paper actually does

The brief asked me to get this right. The user summary is **partly right, partly right about the
wrong document, and wrong on two points**, one of which would waste real engineering effort.

## 3.1 Claim by claim

> **"A one-dimensional array acts as a storage pool for voxel data."**

**Correct** (S2 §7.3.1). Both pools are flat arrays of fixed-size pages: a page of the brick pool
holds one brick, a page of the node pool holds one node tile (S2 §7.3.2).

> **"Any voxel that gets rendered is moved to the front of the array."**

**Wrong granularity, and the mechanism is not what "moved" suggests.**

- Not voxels. **Pages.** A page is a node tile (8 nodes) or a brick (8³ = 512 voxels, 1728 with
  border). Per-voxel would be 512× the bookkeeping (S2 §7.3.2).
- Nothing in the pool moves. **The payload never moves.** What is reordered is a separate
  `LRU page list` of *addresses* (S2 §7.3.4, p. 134). The bricks stay exactly where they are in
  the 3D texture. This matters — see §3.4.
- Not per-element. Once per pass the *entire* address list is **stably partitioned** by two
  order-preserving stream compactions (S2 §7.3.4, Fig. 7.12, p. 135).

> **"New data overwrites the least-recently-used entries at the back."**

**Correct in substance.** *"When n new pages need to be inserted in the cache, replacing the n
pages corresponding to the last n entries of the list perfectly respects the LRU scheme"*
(S2 §7.3.4). **SOURCE** shows the shipped code uses the *mirror* orientation — see §3.3.

> **"The whole thing runs in compute shaders on the GPU."**

**True of the 2011 thesis. FALSE of the 2009 paper the brief cites.** See §3.2. This is the single
most important correction in this document, because the user attributed a 2011 mechanism to a 2009
source.

> **"Constantly swapping positions also clusters co-accessed data, improving locality."**

**Not supported by either source. I searched both for any such claim and found none.** It is also
**mechanically impossible** as described: the compaction reorders a list of *addresses*, and the
data those addresses point to is never relocated. Reordering a free list cannot change where
bricks sit in the brick pool, therefore it cannot change texture-cache locality.

**This one matters practically.** Anyone implementing "move-to-front for locality" would be
tempted to *migrate the payload* to make the locality claim true — copying 1728-voxel bricks
around a 3D texture every frame. That would be catastrophically expensive and buys nothing the
paper claims. **Do not do it.** Locality in GigaVoxels comes from three other things, all cited:
node tiles grouping siblings contiguously (S1 §4.1), SOA splitting of the node arrays (S2 §5.2.1),
and the 3D texture cache over bricks (S2 §5.2.1).

> **"Reported to hold rendering stable under a 1 MB pool."**

**Not found in either source, and I believe this is a misreading.** The smallest brick cache
either source *renders* from is **128 MB** (S2 §7.5.2, Fig. 7.23 — 5.9 % miss rate, GTX 480). The
only "1 MB" is the left end of the x-axis of **Fig. 7.25** (S2 §7.5.4, p. 154), which is a
microbenchmark of **LRU management cost** against pool size with 64-byte pages — GPU 0.19 ms vs
CPU 0.33 ms at 1 MB. That figure is about the *cost of the bookkeeping*, not about rendering a
scene out of a 1 MB pool. **Treat the 1 MB claim as unsupported.**

> **"Addresses up to 64 GB of voxel tree data."**

**Real, but it is an address-space bound, not a demonstrated dataset** — and it is the *smaller*
of the two bounds. See §5.1.

## 3.2 The 2009 paper's LRU is on the CPU

This is the discrepancy the brief asked me to look for, and it is real.

**PAPER (S1, Preliminaries, p. 3):**
> *"Each brick in the pool has a certain timestamp that is reset upon usage. If an octree node
> needs a subdivision and new bricks are transferred to the GPU, the algorithm will use the memory
> locations that were previously reserved for the oldest, thus unused, bricks."*

and, decisively:

> *"To keep track of the current data organization and facilitate updates, the structure is
> mirrored on the CPU. […] This facilitates some of the operations, such as the LRU ordering on
> the CPU."*

**PAPER (S1 §6.1, p. 5-6):** *"Let's suppose for a moment that the CPU had access to this
information for all rays. Then it can conveniently update the timestamp of the pool elements
(nodes and bricks) in its mirrored data structure. […] It even knows where to store the brick in
the pool because timestamps are maintained in host memory."*

**So in 2009: timestamps live in host memory, the LRU order is maintained on the CPU against a
mirrored copy of the whole tree, and the GPU's only job is to compact the feedback and ship it
across the bus.** The brief's recollection — "timestamp per page plus a compaction/sort rather
than literal per-element swaps" — is **correct**, and it is correct for both documents; the 2009
version just runs the ordering on the wrong side of the bus.

**THESIS (S2 §7.1.2, p. 121):** the thesis calls this out as the thing it fixed —
*"It also removes the need to maintain a mirrored version in system memory of the GPU data
structure. This was previously required […] [GM05, GMAG08]."*

## 3.3 The 2011 mechanism, exactly, and verified against shipping code

**THESIS (S2 §7.3.4, p. 134-135).** Sorting is **explicitly rejected**: *"Sorting the LRU page
list at each rendering pass on the GPU using a sorting algorithm would be prohibitive [SA08].
Instead, we rely on an incremental sorting scheme that makes use of two order-maintaining
stream-compactions."*

The algorithm, per cache, per pass:

```
1. usage buffer   : one 32-bit timestamp per PAGE. A ray that uses a page stores currentPass.
                    Racing stores are safe: every writer stores the same value. No atomics.
2. usage mask     : one thread per entry of the LRU page list. Flag it if
                    usageBuffer[thatPage] == currentPass.
3. compaction A   : order-preserving compact of the page list by "used"     -> U+
4. compaction B   : order-preserving compact of the page list by "not used" -> U-
5. concatenate    : the new page list is U+ then U-.
                    Because each compaction preserves relative order, U- still has the
                    oldest entries at its far end. The list is therefore sorted by recency
                    without ever running a sort.
6. allocate       : take the n pages from the least-recently-used end.
```

**SOURCE (S3, `GvCache/GsCacheManager.inl::updateTimeStamps`, lines ~380-600).** The shipping
implementation is exactly this, with **two `cudppCompact` calls** and a buffer swap. The header
comment reads:

> `Update the list of available elements according to their timestamps.`
> `Unused and recycled elements will be placed first.`

and the two compactions are annotated:

> `// Algorithme : WRITE in temporary buffer _d_elemAddressListTmp, the compacted list of NON-USED elements`
> `// - elements are written at the beginning of the array`

> `// Algorithme : WRITE in temporary buffer _d_elemAddressListTmp, the compacted list of USED elements`
> `// - elements are written at the end of the array, i.e. after the previous NON-USED ones`

**So the shipped code puts UNUSED at the front and allocates from the front** — the mirror image
of the thesis's Fig. 7.12 caption, which says used pages are kept at the beginning. **THESIS
(S2 §7.3.4) is internally inconsistent on this point**: the prose at p. 135 says used pages are
put *"at the end of the list"* while the figure caption on the same page says *"beginning"*. The
code settles it, and in any case **the orientation is a naming convention with no semantic
content** — what matters is that used and unused go to opposite ends and allocation comes from the
unused end.

**SOURCE** also reveals a detail absent from both papers: the source ships a **CPU LRU reference
implementation** behind `#if GPUCACHE_BENCH_CPULRU==1`, a straightforward two-pass loop over the
host timestamp array. That is the baseline for Fig. 7.25.

## 3.4 Two consequences that are easy to miss

**Consequence 1 — the payload never moves, so there is no relocation cost and no locality win.**
Already covered in §3.1, but restated because it is the crux of the user summary's error.

**Consequence 2 — recycling a page requires a full page-table scan.**
**THESIS (S2 §7.3.7, "LRU invalidation procedure", p. 139-140).** When a page is recycled, every
page-table entry still pointing at it must be nulled — and *"pages in the cache could be
referenced by more than just a single page table entry"* because of brick instancing. Their
solution is two data-parallel steps: flag the recycled pages (reusing the usage buffer with a
reserved value of zero, so it costs no extra memory), then **test every entry of the page table**
against the flags and null the matches.

**DERIVED.** That second step is O(page-table size) per frame in which *any* eviction happens — a
full sweep of the node pool, not of the evicted set. In their reference configuration that is
2,097,152 entries per frame. It is cheap per element and perfectly parallel, but it is not free
and it does not appear in the 4 %-of-frame cache-management figure discussion as a separate line.

**And the gap that kills editing. THESIS (S2 §7.3.7, final paragraph, p. 140):**
> *"This scheme also allows the producer to implement a write-back procedure for cached data. Such
> a procedure is not useful in our application, since cached data are simply used for rendering,
> and thus are not modified."*

**There is no write-back. GigaVoxels' cache is read-only.** An evicted page is simply gone, and
the producer regenerates it from the source volume next time. **If a page held a player edit, the
edit is destroyed on eviction.** This is stated as a hypothetical extension, not implemented. See
§7.4.

---

# 4. The numbers they report

## 4.1 I3D 2009

**PAPER (S1 §7, p. 7).** Hardware: **Core 2 Duo E6600 @ 2.4 GHz, NVIDIA 8800 GTS 512 (G92),
512 MB.** All images **512 × 512**.

| Example | Config | Resolution | Performance |
|---|---|---|---|
| Trabecular bone, tiled 8× per axis | N=2, M=16 | 1024³ real → **8192³ virtual** | **20-40 Hz** with mip filtering; **~60 fps** without |
| Sierpinski (one instanced 81³ brick) | N=3 | up to 8.4M³ (float precision limits zoom to 2¹⁹) | often **90 Hz**, usually ~60 |
| Hypertextured mesh, 20 octaves Perlin | — | 1024³ | **~20 FPS** |
| Cumulus cloud [BNM08] | N=2, M=32, 5 octaves | 2048³ virtual | — |

Memory (S1 §7): **node pool 4 MB** (64³ entries); with 16³ bricks that references 1024³ indices.
**Brick pool 430 MB**, room for 42³ bricks. Claimed ~50 % speedup over Gobbetti et al. [GMAG08],
*"nevertheless, this is based on our implementation."*

## 4.2 Thesis 2011

**THESIS.** Hardware: **GTX 480 + Core 2 Duo E6850 @ 3 GHz** for the cache work (S2 §7.5);
**GTX 280** for some rendering examples.

| Result | Value | Where |
|---|---|---|
| Cost split, Mandelbulb walkthrough, 1× speed | rendering **56 %**, cache management **4 %**, node loading **1.5 %**, brick production **30 %** | §7.5.1 |
| Same at 4× camera speed | rendering **34 %**, cache management **2.5 %**, brick production **57 %** | §7.5.1 |
| Cost of *emitting* requests + usage from the ray-caster | **+5 %** on total render time | §7.5.1 |
| Brick cache miss rate, 4× speed, **512 MB** cache | **2.6 %** | §7.5.2 |
| Brick cache miss rate, 4× speed, **128 MB** cache | **5.9 %** | §7.5.2 |
| GPU kernel-fetch streaming vs CPU `cudaMemcpyToArray` | **4394 MB/s vs 200 MB/s** (18³ bricks, 484 bricks) — ~½ of PCIe 2.0's 8 GB/s theoretical, vs 1/40th for the CPU path | §7.5.3 |
| GPU LRU vs CPU LRU management cost, 64 B pages | 64 MB pool: **0.75 ms vs 20.60 ms (27.5×)**; 1 MB pool: **0.19 ms vs 0.33 ms (1.7×)** | §7.5.4 |
| Raycasting vs rasterisation, San Miguel 12M tris @ 16× MSAA, 512² | **1265 vs 107 FPS** at the far view; crossover estimated at **20-30 triangles/pixel** | §6.3.1 |
| Brick size vs speed, Mandelbulb, GTX 480 | 4³ **141 FPS**, 8³ **128 FPS**, 16³ **110 FPS** | §6.3.2 |
| Silhouette rays vs interior rays | **1.5× more expensive** | §6.3.2 |
| Octree traversal vs brick marching, 4³ bricks | **75 % traversal / 25 % marching**; inverts to 17/83 at 16³ | §6.3.2, Fig. 6.16 |
| Largest datasets rendered | **8192³** medical volume at 20-40 FPS (GTX 280); a 2048³ CT scan, **32 GB on disc**, at 20 FPS | Fig. 7.1, Fig. 6.1 |
| Out-of-core BVH triangle demo (genericity) | Power Plant 13M tris, 512 MB in system RAM, **50 MB** triangle cache, **15-30 FPS** (GTX 280) | §7.5.5 |

## 4.3 What 2009 hardware implies for a 2020s laptop GPU

**DERIVED**, using the thesis's own bus figure (S2 §1.3 cites 8 GB/s theoretical for PCIe 2.0 ×16)
and the hardware from the brief (RTX 3050 6GB Laptop, 6144 MiB, MEASURED).

| | 8800 GTS 512 (2009) | RTX 3050 6GB Laptop (this machine) | Ratio |
|---|---|---|---|
| VRAM | 512 MB | **6144 MiB** | **12×** |
| Host↔device bus, theoretical | 8 GB/s (PCIe 2.0 ×16) | ~32 GB/s (PCIe 4.0 ×16) — **UNVERIFIED on this machine, lane count not checked** | ~4× |
| Compute | ~0.5 TFLOP/s FP32 | multi-TFLOP/s | ~10× |
| Render resolution used | 512 × 512 = 0.26 Mpx | voxl2 internal, Balanced ≈ 0.4-0.9 Mpx | 1.5-3.5× |

**The conclusion is the opposite of "so it will be even better now."** GigaVoxels existed because
**a 512 MB card had to render a 32 GB dataset — a 64:1 deficit.** VRAM grew 12× and buses 4×, but
the pressure that justified the machinery is what changed most:

**MEASURED (L1 §3.3).** voxl2's entire content at a 256 m world is **9.1 MB pooled** against
**6144 MiB** of VRAM — a **1:675 surplus**. GigaVoxels' deficit has become voxl2's surplus, by
about four and a half orders of magnitude. **DERIVED: the eviction machinery has nothing to
evict.**

The one number that transfers cleanly is **cache management at 4 % of frame time** (S2 §7.5.1).
**MEASURED (brief, from `docs/PROFILE.md`):** voxl2's frame is 97.5-99.4 % GPU-bound with a
3.030 ms fixed floor. 4 % of a 7.1 ms Balanced frame is **0.28 ms** — about 9 % of the fixed
floor, spent on bookkeeping for a pool that would never fill.

---

# 5. What it does not solve

## 5.1 The 64 GB claim is an address-space bound, not a capability

**THESIS (S2 §5.2.2, p. 103).** The full context: 30-bit pointers *"when used as traditional
pointers would only allow us to address 1GB of data"*, so they index **groups** instead.
2³⁰ node-tiles × 8 nodes × 8 B = **64 GB node pool**; 2³⁰ bricks × 8³ × 4 B = **2 TB brick pool**.

**DERIVED.** Three caveats the summary loses:

1. It bounds the **pool**, i.e. what could be resident, not what could be streamed through it.
2. **64 GB is the node pool.** The brick pool — where the voxels actually are — bounds at 2 TB.
   Quoting 64 GB as "the amount of voxel tree data addressable" quotes the tighter of the two.
3. No dataset near either bound was rendered. The largest demonstrated is **8192³** and a **32 GB**
   on-disc CT scan (§4.2).

## 5.2 Latency on a miss — you cannot see the data on the frame you asked for

**THESIS (S2 §7.2.1, Fig. 7.6, p. 125-126).** Refinement is one octree level per pass. Their
worked example takes **four passes** for a single ray to converge in a *trivial* configuration.
In the real-time strategy (1 pass/frame) that is four frames minimum, and depth grows with world
size.

**THESIS (S2 §7.2.2, p. 127) names the pathology:** giving bricks priority *"leads to a lot of
data loaded at lower resolution before the actual needed resolution is reached. Thus, this
real-time strategy leads to higher convergence time."* You pay bandwidth for mip levels you will
throw away in three frames.

## 5.3 Popping, and the honest name for the mitigation

**PAPER (S1 §6):** the mitigation for a miss is to render from a coarser mip. The justification is
*perceptual*: *"we perceive less details during strong motion, when it is most likely to encounter
missing data."* **This is a bet, not a fix.** It works because a blurred substitute during camera
motion is less objectionable than a hole — but it means image quality is a function of camera
velocity and of how much the streamer got done last frame. Frame time stays flat; **quality is the
variable that absorbs the error.**

Mip *blending* across three levels (S1 §5.2, three levels being *"a must"* for proper blending)
makes the transition smooth once the data arrives, but does not make the substitution invisible.

## 5.4 The feedback pass is not free

Measured costs from §4.2: **+5 %** on the ray-caster to emit requests and usage, plus **4 %** of
the frame for cache management at 1× speed. **THESIS (S2 §7.3.8, p. 141-142):** stream compaction
*"appeared to be one of the most costly operations done for the management of a cache"* — which is
why they added a pre-pass to shrink the buffer before the two per-cache compactions.

And the part that is genuinely expensive is not the cache at all: **brick production is 30 % of
the frame at 1× and 57 % at 4×** (S2 §7.5.1). The streaming machinery is cheap; *making the data*
is not.

## 5.5 Fragmentation — actually solved, but at a price

**DERIVED from S2 §7.3.1.** There is no fragmentation, because every page is the same size. That
is the *reason* for fixed-size bricks. The price is that **the structure cannot hold
variable-length payloads**, which is a direct conflict with voxl2 — see §6.4.

## 5.6 Working set larger than the pool

**THESIS (S2 §7.5.2, p. 152-153):** *"Trashing can only appear when the total amount of data
required for a given frame is larger than the size of the cache. In this case, quality reduction
strategies have to be employed to ensure real-time rendering."* **No such strategy is specified.**

**SOURCE** shows what actually ships: the warning-and-truncate in §2.4. Requests beyond the
available slot count are dropped, and rays keep rendering from whatever coarse data is resident.
Frame time is protected; the image degrades without bound. **In a first-person sandbox that
manifests as terrain visibly failing to resolve, which is a worse failure than a frame-time
spike.**

## 5.7 Secondary rays multiply the working set

**THESIS (S2 §7.1.2, p. 123)** presents arbitrary secondary rays as a *feature*: shadow rays,
reflections, GI, *"rays can be launched in any direction."* Each of them also emits requests.

**DERIVED, and this is a specific hazard for voxl2.** GI is **53.6 % of voxl2's frame** (brief,
from `docs/PROFILE.md`). GI rays go where primary rays do not — behind the camera, into occluded
cavities, up at the sky. Under ray-guided residency they would each pull their target resident,
so the working set becomes *the union of everything any ray touched*, not *what is visible*. That
is a much larger and much less coherent set than the one all of GigaVoxels' published numbers were
measured against.

**Tellingly, Crassin's own GI work does not use the streaming cache.** THESIS Ch. 9's structure
adds **neighbour pointers**, uses **3³ corner-centred bricks**, is **re-voxelized every frame**,
and allocates *"roughly 512MB on the GPU"* fully resident (S2 §9.4.1, §9.8). And **S2 §10, p. 182
says why**: that scheme *"is not compatible with our on-demand loading scheme."* **The one
GigaVoxels application whose workload most resembles voxl2's is the one that abandoned the cache.**

---

# 6. Mapping onto voxl2's actual structure

## 6.1 What voxl2 already has

From `src/voxels/impl/voxel_malloc.inl` and `src/voxels/impl/voxels.inl`:

```
LOG2_VOXEL_SIZE      -4          -> 1/16 m voxels  (DO NOT CHANGE — brief)
CHUNKS_PER_AXIS      16 shipped  -> 64 m world; 32 recommended by L1 -> 128 m
PALETTE_REGION_SIZE  8           -> 0.5 m regions, 8x8x8 = 512 per chunk
chunk                            -> 64^3 voxels = 4 m
MAX_CHUNK_UPDATES_PER_FRAME 128

VoxelLeafChunk { flags; update_index; uniformity_bits[3]; sub_allocator_state;
                 palette_headers[512]; }                       = 8216 B, DENSE
VoxelUniformityChunk { lod_x2[1024]; lod_x4[256]; lod_x8[64];
                       lod_x16[16]; lod_x32[4]; }              = 1364 u32, per chunk
VoxelMalloc                                                    = variable-size page heap
```

**The correspondence is closer than it looks:**

| GigaVoxels | voxl2 today | Same? |
|---|---|---|
| Brick pool (fixed-size pages, 3D texture) | `VoxelMalloc` heap (**variable**-size pages, linear) | in role, not in shape |
| Node pool = page table | `voxel_chunks[]` — **dense**, 8216 B/chunk, resident always | **no: this is the defect** |
| Node = 8 B, one child pointer + one data word | `VoxelLeafChunk` = 8216 B, no children | **no** |
| Mip pyramid across octree levels | `uniformity_bits` / `lod_x2..x32`, **within one chunk only** | **no: this is the stride ceiling** |
| Timestamp usage buffer, request buffer | nothing | absent |
| LRU page list | nothing | absent |

## 6.2 The chunk table is a page table that forgot to be sparse

**MEASURED (L1 §3.3):** 8216 B per chunk resident whether the chunk holds rock or air, plus
another 8192 B per chunk in host RAM. `Process VRAM = 1785 MiB + 8216 × CPA³, to within 1.3 %`.

**The fix L1 §3.3 already specifies is the GigaVoxels node-pool/brick-pool split**, in the same
shape:

```
directory  = 8 B x CHUNKS_PER_AXIS^3      <- tag + inline uniform value OR pool index
body pool  = 8216 B x (chunks with content)
```

**8 bytes per directory entry is the same 8 bytes as a GigaVoxels node**, and for the same reason
(S1 §4.1): one bit says "this is a constant value" and the payload word is either the value or a
pool index. It is the identical trick, independently arrived at, and the paper validates it.

**With one warning from §1.2: keep the inline-constant bit.** The shipped GigaSpace dropped it
(SOURCE, `GsNode.h`), and it is the single feature voxl2's measurement says the entire saving
depends on — 97.4 % uniform chunks at CPA 32, 99.7 % at CPA 64 (MEASURED, L1 §3.3). A directory
of pure pool indices would still be 8 B/chunk but every uniform chunk would then need a body,
and the win would collapse from 36.9× to nothing.

**MEASURED (L1 §3.3):** 33.7 MB → 2.6 MB at CPA 16 (12.9×); 269.2 → 7.3 MB at CPA 32 (36.9×);
2153.8 → 9.1 MB at CPA 64 (237.8×).

**And L1 argues it is also faster**, which is the part GigaVoxels' node pool gets right too: a
CPA 64 directory is 2.10 MB and **fits in L2**; eight neighbouring chunks share a 64-byte line; a
uniform chunk resolves with no second load. That is the same reasoning as S2 §5.2.1's SOA layout
and node-tile grouping. **MEASURED (L1, from `docs/DENSITY_LIMITS.md` §…):** a ray that misses is
the engine's most expensive ray — precisely the case that gets cheaper.

**DERIVED: this change is worth doing, it is worth doing now, and it needs no LRU, no request
buffer, no producer, and no streaming.** It is GigaVoxels §4 without GigaVoxels §7.

## 6.3 The stride ceiling and the octree are the same question

**Brief, fact #1 (MEASURED):** the uniformity pyramid tops out at one chunk = 4 m, so crossing
empty space costs one step per 4 m regardless of `MAX_STEPS`; 512 → 2048 is byte-identical at
identical cost.

**DERIVED, and this is the structural reading.** Look at the table in §6.1: voxl2's mip pyramid
(`lod_x2` … `lod_x32`) is **per-chunk**. It runs from 1 voxel to 32 voxels and stops, because
there is no node above the chunk to hang a coarser level on. GigaVoxels' pyramid runs all the way
to the root because *there are nodes all the way to the root*.

**The stride ceiling is not a marching bug. It is the absence of the octree.** Extending the
pyramid above chunk level — whether as a full N³-tree or as the far-field's nested volumes
(`docs/design/PERFORMANCE_PLAN.md` §5, already partially built per `docs/FAR_FIELD.md`) — is what
raises the ceiling. **This is the one place GigaVoxels' data structure genuinely answers a
measured voxl2 defect, and it is §4 of the paper, not §7.**

But read §7.2 of this document before choosing the full octree, because the depth that buys the
stride is the same depth that multiplies the edit.

## 6.4 A shape conflict: fixed-size bricks vs palette compression

**DERIVED.** GigaVoxels' pool works because every page is identical in size (§5.5). voxl2's
`VoxelMalloc` deliberately allocates **variable**-size blobs per 8³ palette region — that is the
compression. `brushes.glsl` rule 2, cited in `docs/FAR_FIELD.md`: *a bit-identical region
allocates nothing.*

You cannot naively drop a GigaVoxels brick pool in. Three options:

1. **Keep variable-size pages and give up "the LRU list is an array of equal slots".** Eviction
   then needs a real allocator with free-list merging — i.e. reintroduce the fragmentation that
   fixed-size pages exist to avoid.
2. **Fixed-size pages, palette compression inside a page.** Round each region up to a page. Costs
   internal fragmentation, gains the whole GigaVoxels cache design unchanged.
3. **Fixed-size *directory*, variable-size *body*, no eviction.** L1 §3.3's proposal. Sidesteps
   the conflict entirely because nothing is ever evicted.

**Option 3 is the one the measurement supports.** Options 1 and 2 only become interesting if a
future measurement shows the resident content approaching VRAM, which at 9.1 MB it does not.

## 6.5 The request buffer would be cheap

**DERIVED.** If voxl2 ever wants the feedback loop, the 2011 request mechanism (§2.2) transplants
almost verbatim into the existing chunk-election pass:

```
requests[CPA^3] : u32 timestamp array, 4 B per chunk
   CPA 16 -> 16 KB   CPA 32 -> 128 KB   CPA 64 -> 1 MB
```

A ray that reaches a non-resident chunk stores `frame_index` into `requests[chunk_index]`. No
atomics, deduplication is structural, no clear pass. One stream compaction produces the work list,
and `MAX_CHUNK_UPDATES_PER_FRAME` is *already* the service cap the thesis says you need (S2
§7.3.5) — voxl2 has the throttle and is missing only the demand signal.

**MEASURED (L1 §4):** today, `PerChunkCompute` elects every ungenerated chunk at startup, so world
generation is a hard floor of `CPA³ / 128` frames — it generates the whole box before you can see
it. **DERIVED: a demand signal would replace "generate the box" with "generate what a ray asked
for", which is the far more valuable half of GigaVoxels for voxl2** — not because of memory, but
because it decouples time-to-playable from world volume. See §8.3.

## 6.6 Where GigaVoxels' costs would land in voxl2's budget

**Brief, fact #4 (MEASURED):** `frame_ms = 3.030 + 7.520 × internal-megapixels`, fixed floor
3.030 ms.

**DERIVED**, scaling S2 §7.5.1's proportions onto a 7.1 ms Balanced frame:

| GigaVoxels phase | Their share | voxl2 equivalent | Note |
|---|---|---|---|
| Emitting requests + usage from rays | +5 % of render | ~+0.2 ms | inside the march, which is already GPU-bound |
| Cache management (LRU + request compaction) | 4 % of frame | ~0.28 ms | **lands on the 3.030 ms fixed floor**, i.e. ~9 % of the thing that caps the engine at 330 fps |
| Brick production | 30 % of frame | — | voxl2 already pays this as chunk generation |

**Fact #3 in the brief cuts both ways.** "CPU-side cleverness buys nothing" is an argument *for*
the 2011 GPU-side design over the 2009 CPU-side one. But it is equally an argument that adding
0.28 ms of GPU bookkeeping to a 97.5-99.4 % GPU-bound frame is a straight subtraction from the
render budget, with nothing on the CPU to absorb it.

## 6.7 The GI hazard, restated as a voxl2 risk

**DERIVED**, from §5.7. voxl2's GI is 53.6 % of the frame. If GI rays participate in the feedback
loop, the resident set is the union over all ray types, not the visible set. If they do *not*
participate, they read stale or absent data and the GI is wrong in exactly the regions the player
cannot see directly — which is where GI matters most. **Neither branch is obviously acceptable,
and no published GigaVoxels result covers this case** — Crassin's own GI chapter uses a separate
fully-resident structure (S2 §9.4.1) that he states is incompatible with the cache (S2 §10).

**This should be treated as an open design question, not an implementation detail.**

---

# 7. Editability — the dealbreaker

The brief called this the most likely dealbreaker and asked that it not be a footnote. It is
worse than a dealbreaker for the *cache*; it is a serious argument against the full *octree* too.

## 7.1 The paper does not address editing at all

**THESIS (S2 §1.1.1, p. 25):** *"Voxels are usually envisioned as a totally alternate scene
representation, dedicated to rigid motionless data."* Crassin sets out to challenge this, and then
scopes it out: *"The animation and deformation aspects are not the primary focus of this thesis."*

**PAPER (S1 §8, Conclusion, p. 7):** *"Currently, animation is a big problem for volume data. In
the future, we would like to investigate possible solutions."* That is the entire treatment of
dynamics in the I3D paper.

**Nothing in either source describes an incremental edit to a resident, streamed voxel structure.**
The closest is thesis Ch. 9, which re-voxelizes from triangles into a **separate, fully resident,
non-streamed** octree — and which S2 §10 declares incompatible with the cache.

## 7.2 What an edit costs in this structure — the arithmetic

**DERIVED**, from the structure as specified in §1, at voxl2's parameters.

Take CPA 32 (128 m world) at 16 voxels/m: 2048 voxels per axis = 2¹¹. With 8³ bricks the leaf
brick grid is 2⁸ = 256 per axis, so the octree above the bricks is **8 levels deep**.

Editing **one voxel** invalidates:

1. **Its own brick.** 1 write.
2. **Every ancestor brick, because they are downsampled children.** 8 more bricks, one per level up
   to the root. **Each ancestor must be recomputed from its 8 children, which must therefore be
   resident.** *(S1 §5.2; S2 §5.1.1.)*
3. **The replicated border voxels in neighbouring bricks.** With corner-centred voxels and a
   1-voxel border (S2 §5.1.4) a face-adjacent edit touches 1 neighbour, an edge 3, a corner **7**;
   with node-centred 2-voxel borders it is up to 26. And this applies **at every level of the mip
   chain**.
4. **Possible forced subdivision of neighbours.** S2 §5.1.4, Fig. 5.7: if an edit makes a
   boundary voxel non-empty, a previously-unsubdivided neighbour *"could get subdivided in order to
   provide a brick at the same resolution"* — an edit that **allocates new pages in the pool**.

**Cost per edited voxel, corner-centred 8³ bricks, 8 levels:**

| Case | Bricks written |
|---|---|
| Interior voxel | **9** (self + 8 ancestors) |
| Face-adjacent | ~18 |
| Corner voxel, worst case | **up to 8 × 9 = 72**, plus any forced subdivisions |

And it **grows with world size** — one more brick per edited voxel every time the world edge
doubles.

**Compare voxl2 today.** A brush edits one chunk; the chunk's palette compression is rebuilt for
the touched 8³ regions and its `lod_x2..x32` pyramid is recomputed — **all inside one 8216 B
record, with no neighbour and no ancestor involvement**, because the pyramid stops at the chunk.
`MAX_CHUNK_UPDATES_PER_FRAME = 128` bounds a frame's edit work absolutely.

> **The stride ceiling and O(1) edits are the same property.**
> voxl2's pyramid stops at the chunk. That is *why* an empty-space step is 4 m (brief fact #1)
> **and** *why* an edit is O(1). Building the octree that raises the stride is exactly the change
> that makes an edit O(depth) with border replication. **These are not two decisions. They are
> one decision, and the brief's fact #1 and voxl2's editing requirement pull in opposite
> directions.**

## 7.3 Crassin says the two mechanisms are incompatible, in his own words

**THESIS (S2 §10, Perspectives, p. 182)**, on his Ch. 9 dynamic-object scheme:

> *"First it is not compatible with our on-demand loading scheme, and thus does not scale with a
> large number of animated objects. The voxelization of an animated object has to be done at each
> frame, whenever it is visible or not. In addition, this voxelization has to be done from bottom
> to top in order to compute pre-filtering, which means that a high resolution voxelization is
> always needed, whatever the resolution actually required for rendering."*

**Unpack that.** Pre-filtering (the mip chain) is computed **bottom-up**: to know the value of a
coarse voxel you must have all the fine voxels under it. Ray-guided streaming is **top-down**: it
loads coarse first and refuses to load fine data nobody looked at.

**To keep the mip chain correct after an edit you must hold the leaves resident. That is precisely
what the cache exists to avoid. The two mechanisms want opposite things.** This is the author's
own assessment, in his own conclusion, and it is the cleanest statement of the dealbreaker
available anywhere.

## 7.4 There is no write-back, so an evicted edit is a destroyed edit

**THESIS (S2 §7.3.7, p. 140), quoted in full in §3.4 above:** write-back is described as a thing
one *could* imagine, and is not implemented, because *"cached data are simply used for rendering,
and thus are not modified."*

**DERIVED, and this is fatal on its own.** In voxl2, bricks are not derived from an immutable
source volume — the player *authors* them. Under GigaVoxels' cache:

- Player digs a tunnel. Bricks are modified in the pool.
- Player walks away. Those bricks stop being touched, drift to the eviction end of the LRU list.
- Something else needs the slots. The bricks are recycled, page-table entries nulled.
- Player walks back. The producer regenerates from the source — **the terrain generator** —
  and the tunnel is gone.

Making this correct requires **all** of:

1. a **write-back path** in the producer interface — the hook exists (S2 §7.3.7 mentions calling
   an invalidation function per invalidated entry) but the mechanism does not;
2. a **dirty bit** per page, so clean pages can be discarded and dirty ones must be persisted;
3. **somewhere to write back to** — a CPU-side or disk-side authored-delta store, which voxl2 does
   not have (`docs/HANDOFF.md`: "No persistence");
4. **pinning of dirty pages** until write-back completes, which means the LRU can no longer freely
   evict, which is the beginning of the end of a clean LRU;
5. and the mip chain must be re-derived on **reload**, bottom-up, which is §7.3's problem again.

**That is not an extension to GigaVoxels. That is a different system that happens to share a data
structure.**

## 7.5 What the thesis does offer for editing, and it is not nothing

**THESIS (S2 §9.4.2, p. 169)**, in the GI chapter, describes the only editing-adjacent mechanism
in 184 pages:

> *"in real video-game situations, large parts of the environment are usually static or updated
> punctually on user-interaction. This allows us to voxelize these parts once in the octree, and
> to update them only when necessary, while full dynamic objects are re-voxelized at each frame.
> Both semi-static and fully dynamic objects are stored in the same octree structure […] A
> timestamp mechanism is used to differentiate both types, in order to prevent semi-static parts
> of the scene to get destructed at each frame."*

**This is the right instinct and it is the model voxl2 should take** — a **semi-static tier**
updated only on user interaction, distinguished from a per-frame tier by a timestamp. But note
where it sits: in the **fully resident, non-streamed, ~512 MB** Ch. 9 structure, not in the
streamed one.

## 7.6 Later work has answered this, and the answer is "don't use an octree"

**LATER WORK (S7).** van Straaten, *Real-time Ray tracing and Editing of Large Voxel Scenes*
(MSc, Utrecht University, 2020) — the "BrickMap" — attacks exactly this problem. From the
abstract: previous solutions *"use compression schemes involving hierarchical data layouts such as
sparse voxel octrees that require preprocessing, which prevents efficient editing"*; the method
therefore *"keeps data in raw format to avoid preprocessing and make it directly editable"*, uses
**layered grids** rather than a deep tree, and adds an **occlusion-based streaming system** —
i.e. it keeps GigaVoxels' ray-guided streaming and discards GigaVoxels' octree.

**LATER WORK.** The reference implementation (https://github.com/stijnherfst/BrickMap) uses
**8³ bricks grouped into 16³-brick superchunks**, indexed rather than pointered, with three LOD
levels (8³, 2³, 1³) and a 32-bit index (12 bits address, 3 bits streaming flags, 8 bits LOD).

**This shape is much closer to voxl2's than GigaVoxels' is** — voxl2's chunk is already a flat 64³
grid of 8³ palette regions, i.e. a brickmap. **DERIVED: the convergent conclusion from a
literature that has had eleven years to think about it is that if you need editing, you use a
shallow grid-of-grids and you get the stride from a small number of explicit LOD levels rather
than from a deep tree. That is what voxl2 already is, and what `PERFORMANCE_PLAN.md` §5's nested
far-field volumes already propose.**

## 7.7 Editability verdict

| Question | Answer |
|---|---|
| Does GigaVoxels address editing? | **No.** S1 §8 calls animation "a big problem"; S2 §1.1.1 scopes it out |
| What does an edit cost in the structure? | **9 to ~72 brick writes per edited voxel** at CPA 32, growing with world size, versus **O(1) within one chunk** today (DERIVED, §7.2) |
| Can the cache hold player edits? | **No, not without a write-back layer that does not exist** (S2 §7.3.7, §7.4) |
| Is the incompatibility fundamental or incidental? | **Fundamental**, and stated by the author: pre-filtering is bottom-up, streaming is top-down (S2 §10) |
| Is there a way to have both? | Yes, but by **abandoning the deep octree**, not by fixing the cache (LATER WORK, S7) |

---

# 8. Recommendation

## 8.1 Do this next — sparsity without residency

Implement **L1 §3.3's directory + body pool**, which this document has now validated as
GigaVoxels' node-pool/brick-pool split (§6.2), and stop there.

- **MEASURED gain:** 269.2 MB → 7.3 MB at CPA 32 (36.9×); 2153.8 → 9.1 MB at CPA 64 (237.8×).
- Likely also **faster**, because a CPA 64 directory is 2.10 MB and fits in L2 (L1 §3.3).
- **No LRU, no eviction, no write-back problem, no edit-cost regression** — the body pool holds
  every chunk that has content, forever. Nothing is ever evicted, so nothing can be lost.
- The GPU pool allocator it needs already exists: `DECL_SIMPLE_ALLOCATOR` (L1 §3.3).

This is the whole of GigaVoxels that voxl2's measurements justify today.

## 8.2 Do not do this yet — the LRU

**DERIVED.** Revisit when, and only when, a measurement shows resident content approaching a
meaningful fraction of the VRAM budget. At 9.1 MB against 6144 MiB it is 0.15 %. The trigger to
watch for is not world size — L1 measured the paletted-chunk count as *flat* between CPA 32 and
CPA 64 — but **content density**: `docs/DENSITY_LIMITS.md`'s sweep is the right instrument.

## 8.3 The reason to revisit ray-guided feedback is generation, not memory

**MEASURED (L1 §4):** world generation is `CPA³ / 128` frames because every chunk is elected at
startup — *"a hard floor that no GPU speed can move."* CPA 64 = 2048 frames.

**DERIVED.** The feedback loop's real value to voxl2 is that it replaces *"generate the box"* with
*"generate what a ray asked for."* That decouples time-to-playable from world volume, and it is
the only mechanism in the paper that makes an *unbounded* world genuinely possible rather than
merely a bigger box. It needs §2.2's request buffer (a 128 KB array at CPA 32) and the existing
`MAX_CHUNK_UPDATES_PER_FRAME` throttle. **It does not need the LRU.** Requests and eviction are
separable, and voxl2 should take the first without the second.

## 8.4 The trade to decide deliberately

**The stride ceiling (brief fact #1) and O(1) edits are the same structural property (§7.2).**
Before committing to a deep octree, price the edit. The far field's nested fixed-resolution
volumes (`PERFORMANCE_PLAN.md` §5, partly built per `docs/FAR_FIELD.md`) buy stride with a
**bounded, constant** number of levels — 2 to 4 — rather than a depth that grows with world size,
and each level can be regenerated independently of an edit rather than derived bottom-up from it.
**LATER WORK (S7) reached the same conclusion independently.** That looks like the better trade
for a sandbox, and it is the trade voxl2 has already half-made.

---

# 9. Implementation sketch, if and when the feedback loop is wanted

Recorded so the next session does not have to re-derive it. **Not a proposal to build now.**

```
--- per-frame, GPU only, no CPU round trip -------------------------------------

A. RENDER (inside the existing march, trace.glsl)
   on reaching chunk C:
       usage[C]    = frame_index          // plain store, no atomic: all writers agree
       if C not resident:
           requests[C] = frame_index      // same
           fall back to the coarsest resident ancestor, keep marching

B. COMPACT REQUESTS                                        [S2 7.3.5]
   one stream compaction over requests[] with predicate (slot == frame_index)
   -> request_list[]  (chunk indices) and its length n
   scatter step writes the index directly, no read-back

C. MAINTAIN LRU  (only if eviction is ever needed)         [S2 7.3.4]
   mask[i] = (usage[page_list[i]] == frame_index)
   compactA: !mask -> tmp[0 ..]          // unused, order preserved
   compactB:  mask -> tmp[n_unused ..]   // used,   order preserved
   swap(page_list, tmp)
   allocate the n new pages from the front (the unused end)

D. PRODUCE                                                 [S2 7.3.6]
   n = min(n, MAX_CHUNK_UPDATES_PER_FRAME)       // the throttle already exists
   one workgroup per requested page; the producer returns the page index it
   actually wrote — which may differ, if it decided the region is uniform
   (write a constant into the directory instead) or wants to alias an existing page

E. INVALIDATE  (only with eviction)                        [S2 7.3.7]
   step 1: flag each recycled page (reuse usage[] with reserved value 0 — no extra memory)
   step 2: sweep the directory; null every entry pointing at a flagged page

--- sizes, at voxl2's parameters ---------------------------------------------
   requests[] : 4 B x CPA^3   ->  CPA 16: 16 KB   CPA 32: 128 KB   CPA 64: 1 MB
   usage[]    : 4 B per resident page
   page_list  : 4 B per resident page
```

**Traps carried over from the sources, all of them cited:**

- The **timestamp-not-boolean** trick (S2 §7.3.3) is what removes the per-frame clear. Do not
  "simplify" it to a bool.
- Counter-based prioritisation needs **atomics and a clear pass** and S2 §7.3.5 says it is *"not
  used uppermost"*. Take the timestamp version first.
- Sharing one request buffer between two caches needs the **1-bit discriminator** and accepts a
  one-pass delay on conflicts (S2 §7.3.8).
- The producer must be allowed to **decline** — to write a constant instead of a page, or to alias
  an existing page (S2 §7.3.6). Without this, uniform regions consume pool slots.
- The invalidation sweep is **O(directory)**, not O(evicted) (S2 §7.3.7).
- **Compaction is the expensive step** (S2 §7.3.8). Pre-compact once before splitting per cache.

---

# 10. What I could not determine

| # | Item | Status |
|---|---|---|
| 1 | **The "1 MB pool" claim.** Not found in S1 or S2 as a rendering result. Smallest cache *rendered* from is 128 MB (S2 §7.5.2). The only 1 MB figure is the x-axis of the LRU-cost microbenchmark, Fig. 7.25. **I believe the user summary conflates the two, but I cannot rule out a GTC/SIGGRAPH talk or the GPU Pro chapter making the claim** — I did not obtain the GPU Pro 2010 chapter (Crassin, Neyret, Sainz, Eisemann), which is paywalled |
| 2 | **GigaVoxels DP (HPG 2024, S6).** Directly relevant — it is the modern successor and by title it addresses *starvation*, i.e. §5.2's latency problem, via CUDA dynamic parallelism, with the same 8³ bricks and brick pool. **I could not read it.** ACM DL returns 403; HAL is behind a bot check that I did not attempt to bypass. Title, authors (Richermoz, Neyret), venue and DOI are confirmed from three independent listings. **Worth an explicit fetch next session** — it is the only source that may have already solved §5.2 |
| 3 | **BrickMap thesis (S7).** Abstract and implementation README obtained; **the PDF itself returns 403 to non-browser clients.** The structural claims in §7.6 come from the abstract and the repository README, not from the thesis body. The specific numbers (12/3/8-bit index split, 16³ superchunks, three LOD levels) are from the README |
| 4 | **PCIe generation and lane count on this machine.** §4.3's "~32 GB/s" assumes PCIe 4.0 ×16. Not verified; laptop RTX 3050s are frequently ×8. If streaming is ever seriously considered, measure this first |
| 5 | **The `MAX_STEPS` 512 → 2048 byte-identical result** (brief fact #1) is stated in the brief as measured by the ray-reach agent, and `docs/FAR_FIELD.md` line 555 confirms that agent's `VOXL_MAX_STEPS` work landed in `trace.glsl`. **I did not find the document recording the measurement itself** — it may not have been written. Worth locating before it is lost |
| 6 | **GigaVoxels' behaviour with a full GI stack riding on the cache.** §6.7. No published result covers it; Crassin's own GI chapter avoided the cache. This is a genuine unknown, not an omission in my search |
| 7 | **The thesis contradicts itself on LRU list orientation** (S2 §7.3.4 prose says used→end, Fig. 7.12 caption says used→beginning). **SOURCE settles it** — shipped code puts unused first — but the discrepancy is in the published text and is noted here so nobody else loses an hour to it |
| 8 | **GigaSpace SDK.** I extracted and read `GvCache/` (~8700 lines), `GvCore/GsPageTable*` and `GvStructure/GsNode.h` by streaming the 550 MB tarball rather than storing it. I did **not** read the producer sources or the full renderer; §2 and §9 rest on the thesis text for those, not on code |
| 9 | **Why GigaSpace dropped the constant-value node** (§1.2). The code says it happened; nothing says why. Given L1 §3.3, voxl2's answer must be different from theirs, so the reason matters less than the fact — but if the reason was a correctness problem with interpolating across a constant/brick boundary, it would be worth knowing before building the directory |

---

*Written 2026-08-01. Sources S1-S3 read in full or in the relevant chapters; S6 and S7 by
metadata and abstract only (see §10). voxl2 measurements are cited to `docs/SCALE_LIMITS.md`,
`docs/DENSITY_LIMITS.md`, `docs/FAR_FIELD.md`, `docs/PROFILE.md` and
`docs/design/PERFORMANCE_PLAN.md`; none were re-run for this document and no engine code was
modified.*
