# GPU Vertex Pulling for Chunk Geometry — design

**Status:** design only. No engine code has been written for this. Every number
attributed to Voxl below is either read out of `docs/PERFORMANCE.md` /
`benchmarks/results/reference_i7-13650HX.csv`, or is arithmetic on those numbers
with the arithmetic shown. Numbers attributed to other projects carry a URL.
Where something has *not* been measured this document says so rather than
guessing — most importantly in §4, where the whole question of whether draw
calls matter is currently **unmeasured** and §6 stage 0 exists to answer it.

**Audience:** whoever implements this next week. Bit layouts are given to the
bit, the files and functions that change are named, and every stage ships.

---

## 0. The technique in one paragraph

Today a quad reaches the GPU as four `PackedVertex` records plus six indices,
and the hardware's vertex-fetch unit hands each vertex's two `uint` lanes to
`chunk.vert`. Under vertex pulling the quad reaches the GPU as **one** record in
a shader storage buffer, the draw is issued with no vertex attributes at all,
and the vertex shader reconstructs the four corners itself from `gl_VertexID`.
The redundancy this removes is not incidental — a greedy quad is *defined* by an
origin, a face direction and a size, so three quarters of the per-vertex bytes
Voxl uploads today are four copies of the same fact.
([voxel.wiki](https://voxel.wiki/wiki/vertex-pulling/),
[ktstephano](https://ktstephano.github.io/rendering/opengl/prog_vtx_pulling))

The technique is old — Daniel Rákos published it as "Programmable Vertex
Pulling" in *OpenGL Insights* (2012), originally through `samplerBuffer` /
`texelFetch` rather than SSBOs
([Packt summary](https://subscription.packtpub.com/book/game-development/9781838986193/3/ch03lvl1sec27/implementing-programmable-vertex-pulling-pvp-in-opengl)) —
and it is now the default in serious voxel renderers.
`cgerikj/binary-greedy-meshing` stores **8 bytes per quad**, renders by vertex
pulling, and draws every chunk with one `glMultiDrawElementsIndirect`
([repo](https://github.com/cgerikj/binary-greedy-meshing)). `omar-owis/VoxelEngine`
packs quads into 8 bytes and drives them with MDI + persistent mapped buffers +
compute-shader frustum culling
([repo](https://github.com/omar-owis/VoxelEngine)). *Exile* takes the instanced
variant: a four-element triangle strip per face, one instance per quad, with all
four corner AO values carried in the quad record and **interpolated bilinearly
in the fragment shader**
([thenumb.at](https://thenumb.at/Voxel-Meshing-in-Exile/)).

Note what the two 8-byte layouts have in common: **neither stores per-corner
ambient occlusion**. binary-greedy-meshing v2 dropped AO outright (v1 had it).
Voxl cannot drop it — per-corner AO plus per-corner smooth light is the single
largest field in Voxl's quad and the reason its record is 12 bytes rather than
8. §3.8 works that through.

---

## 1. What it replaces, precisely

### 1.1 The current cost per quad

From `src/mesh/MeshData.hpp`:

| | |
|---|---|
| `sizeof(PackedVertex)` | 8 bytes (`static_assert` on line 113) |
| vertices per quad | 4, pushed by `MeshLayerData::addQuad` |
| `sizeof(MeshIndex)` | 4 bytes (`using MeshIndex = std::uint32_t`) |
| indices per quad | 6, inserted by `addQuad` |

**4 × 8 + 6 × 4 = 56 bytes per quad.** Of that, 32 bytes are the vertex stream
and **24 bytes — 43% — are the index buffer**, which exists only to say "these
two triangles share two corners".

This is not a derivation, it is checkable: `MeshLayerData::byteSize()` is what
the benchmark harness reports as `mesh_bytes`, and every row of
`benchmarks/results/reference_i7-13650HX.csv` divides exactly by 56.

| case | quads | `mesh_bytes` | quads × 56 |
|---|---:|---:|---:|
| `meshing/greedy_plains_surface` | 2 968 | 166 208 | 166 208 |
| `meshing/greedy_dense_caves` | 2 566 | 143 696 | 143 696 |
| `meshing/greedy_solid_chunk` | 6 | 336 | 336 |
| `meshing/greedy_checkerboard` | 98 304 | 5 505 024 | 5 505 024 |
| `meshing/greedy_lod1` | 759 | 42 504 | 42 504 |
| `meshing/greedy_lod3` | 34 | 1 904 | 1 904 |
| `subvoxel/mesh_damage_64` | 254 | 14 224 | 14 224 |

Fourteen of fourteen rows agree. 56 bytes per quad is exact, at every LOD level,
in both mesh formats.

### 1.2 Where the 56 bytes go, bit by bit

Per quad, today, counting every bit actually stored:

| what | how it is stored now | bits/quad |
|---|---|---:|
| corner position | 3 × 6 bits, **per vertex** — genuinely differs per corner | 72 |
| direction | 3 bits × 4 vertices — identical four times | 12 |
| sunlight | 4 bits × 4 — differs per corner | 16 |
| block light | 4 bits × 4 — differs per corner | 16 |
| ambient occlusion | 2 bits × 4 — differs per corner | 8 |
| texture layer | 12 bits × 4 — identical four times | 48 |
| width, height | 5 + 5 bits × 4 — identical four times | 40 |
| corner selector | 2 bits × 4 — pure addressing overhead | 8 |
| reserved | 9 bits × 4 | 36 |
| indices | 6 × 32 | 192 |
| **total** | | **448 bits = 56 bytes** |

Only **40 bits** (AO + sunlight + block light, per corner) are irreducibly
per-corner. The corner *positions* look per-corner but are not information: the
mesher derives all four from `base`, `widthBlocks`, `heightBlocks` and the face
frame in `GreedyMesher::emitQuadBlocks`, so 18 bits of origin + 10 bits of
extent + 3 bits of direction regenerate all 72. Everything else — 12 + 48 + 40 +
8 + 36 + 192 = **336 of the 448 bits, 75%** — is duplication, padding, or index
plumbing.

### 1.3 The pulled layout, and the ratio

The record designed in §2 is **12 bytes** for the block path. Corner selection
moves to `gl_VertexID`; the index buffer becomes one static, globally shared
allocation rather than per-chunk bytes.

| | bytes/quad | ratio |
|---|---:|---:|
| today: 4 × 8 vertex + 6 × 4 index | **56** | 1.00× |
| pulled, 12-byte record, shared index buffer | **12** | **4.67×** |
| pulled, 16-byte record (unified with sub-voxels, §2.3) | **16** | **3.50×** |
| pulled, 8 bytes (binary-greedy-meshing — no AO, not viable here) | 8 | 7.00× |

### 1.4 Against the measured 75 MiB

`ChunkRenderStats::gpuBytes` sums `ChunkMesh::byteSize`, which is
`totalVertexBytes + totalIndexBytes` — live data, at 56 bytes per quad. The F3
overlay renders it with the `B / KiB / MiB / GiB` ladder
(`src/ui/DebugOverlay.cpp:49`), so the recorded figure is **75 MiB =
78 643 200 bytes**, i.e. **≈ 1.404 million quads resident** at radius 20.

| | live GPU bytes | change |
|---|---:|---:|
| today | **75.00 MiB** | — |
| 12-byte record | **16.07 MiB** | **−58.93 MiB (−78.6%)** |
| 16-byte record | **21.43 MiB** | −53.57 MiB (−71.4%) |
| + shared index buffer (one-off, §2.4) | +2.25 MiB | |

**Net, at 12 bytes: 75.00 MiB → 18.32 MiB, a 4.09× reduction including the
shared index buffer.**

Two second-order savings that `gpuBytes` does not show:

* **Allocation overhead.** `GpuBuffer::growthTarget` (`GpuBuffer.cpp:18`) adds
  25% head-room and rounds up to 1 024 bytes. With two buffers per chunk over
  10 056 chunks that is **20 112 GL buffer objects**; expected rounding waste is
  ~512 B each ≈ 9.8 MiB, on top of the 25%. Real allocated VRAM today is
  therefore roughly 75 × 1.25 + 9.8 ≈ **104 MiB**, not 75. Pulling deletes the
  per-chunk index buffer outright, halving the object count to 10 056 and
  cutting rounding waste to ~4.9 MiB: allocated ≈ 16.07 × 1.25 + 4.9 ≈
  **25 MiB**.
* **Upload traffic.** `ChunkRenderer::upload` issues two `glNamedBufferSubData`
  calls per layer (`ChunkRenderer.cpp:164-165`), so up to six per chunk. Pulling
  makes it up to three, moving 4.67× less data.

Put the memory saving in context with `docs/KNOWN_LIMITATIONS.md`: resident CPU
voxel data at radius 20 is **~111 MB** and LOD does not reduce it. After pulling,
GPU geometry stops being a comparable line item — which is exactly the point.
It is also the concrete reason to do this: the brief's own §6 frames LOD as
"make a bigger world affordable", and 59 MiB back at radius 20 is what buys
radius 24 on a 6 GB card.

---

## 2. The proposed record layout

### 2.1 `PackedQuad` — the block path, 12 bytes

New header `src/mesh/QuadData.hpp`, written in the same contract style as
`MeshData.hpp` (the layout is mirrored in GLSL; change both in one commit; bump
the version constant).

```
inline constexpr std::uint32_t kQuadFormatVersion = 1;

struct PackedQuad { std::uint32_t w0, w1, w2; };   // 12 bytes, alignof 4
```

```
 w0  ---------------------------------------------------------------------
  bits  0..5    posX          6   0..32, chunk-local BLOCK coords of the
  bits  6..11   posY          6   quad's Origin corner. 33 distinct values,
  bits 12..17   posZ          6   so 5 bits is not enough - same argument
                                  as MeshData.hpp.
  bits 18..20   direction     3   voxl::Direction 0..5, indexes kVoxlNormals
  bits 21..25   width  - 1    5   extent along the frame's U axis, 1..32
  bits 26..30   height - 1    5   extent along the frame's V axis, 1..32
  bit  31       diagonalFlip  1   0 = split 0-2, 1 = split 1-3.  See §3.9.

 w1  ---------------------------------------------------------------------
  bits  0..11   textureLayer 12   layer in the block texture array, 0..4095
  bits 12..21   corner0      10   Origin  : ao 2 | sunlight 4 | blockLight 4
  bits 22..31   corner1      10   +U      : same encoding

 w2  ---------------------------------------------------------------------
  bits  0..9    corner2      10   +U+V
  bits 10..19   corner3      10   +V
  bits 20..31   reserved     12   must be 0
```

Every bit is accounted for; 12 spare bits sit in `w2` for a future skirt flag,
per-quad tint, or a wind/animation index.

The 10-bit corner encoding is **byte-for-byte the encoding
`GreedyMesher::encodeCorner` already produces** (`ao | sunlight << 2 |
blockLight << 6`, `GreedyMesher.cpp:172`). The mesher's 64-bit merge key already
carries `blockId(16) | corner0(10) | corner1(10) | corner2(10) | corner3(10)`.
So `emitQuadBlocks` builds the record by *copying* four 10-bit fields straight
out of the key instead of splatting them across four `packVertex` calls. That is
not a coincidence to be admired — it is the reason the format falls out cleanly,
and it means the shading fields need no re-derivation and cannot drift.

`corner0..3` are in `QuadCorner` order (Origin, +U, +U+V, +V), unchanged.

### 2.2 What the mesher stops doing

`MeshLayerData` loses `vertices` and `indices` and gains
`std::vector<PackedQuad> quads`. `addQuad(c0,c1,c2,c3)` becomes
`addQuad(const PackedQuad&)`, and `reserveQuads(n)` reserves `n` records instead
of `4n` vertices and `6n` indices. `emitQuadBlocks` (`GreedyMesher.cpp:952`)
loses its four-iteration corner loop and its `std::array<PackedVertex, 4>`
entirely — the per-corner `VOXL_ASSERT` on position is replaced by one assert on
`base` plus the existing extent assert, which is strictly the same coverage
because the corners are `base + width·û + height·v̂` by construction.

`packVertex` / `unpackVertex` / `PackedVertex` / `QuadCorner` all survive stage 1
untouched (the classic path still uses them) and are deleted in stage 3.

### 2.3 The sub-voxel format: do the two converge?

`SubVoxelMesh.hpp` states the reason two formats exist, and the reason is
**entirely about the per-vertex position field**: sub-voxel corners run 0..256,
need 9 bits per axis, and 27 bits of position will not fit in an 8-byte vertex
beside everything else. Widening the main format would cost 50% on the millions
of vertices that are not sub-voxels.

**Pulling removes that constraint.** Position is stored once per quad, not four
times, so widening it costs 9 extra bits *per quad* rather than 36 extra bits
per quad-worth-of-vertices. A unified record:

```
 UNIFIED RECORD - 16 bytes (uvec4)

 w0 : posX 9 | posY 9 | posZ 9 | direction 3 | diagonalFlip 1 | reserved 1
      positions in SUB-VOXEL units, 0..256 (a block quad stores 8 x its
      block coordinate; 32 blocks x 8 = 256, exactly the 9-bit maximum)
 w1 : width-1 8 | height-1 8 | textureLayer 12 | renderLayer 2 | reserved 2
      extents in SUB-VOXEL units, 1..256 (a 32-block quad is 256 sub-voxels,
      so width-1 = 255 - exactly 8 bits)
 w2 : corner0 10 | corner1 10 | corner2 10 | reserved 2
 w3 : corner3 10 | reserved 22
```

101 of 128 bits used. This is a real convergence, not a cosmetic one, and it
collapses four things: `SubVoxelMesh.hpp` and `MeshData.hpp` become one header,
`subvoxel.vert` and `chunk.vert` become one program, `m_subVoxelVertexArray`
disappears, and `ChunkRenderer::drawSubVoxels` becomes a fourth quad range
rather than a separate pass with its own program switch.

The texture-coordinate rule converges too, and in the sub-voxel path's
direction. `subvoxel.vert:47-71` already explains that block UV
(`size × cornerSelector`) is only correct because a block quad starts on a block
boundary, and that taking `uv` from the vertex's own position along the frame's
tangent axes "reduces to the block path's value at a block boundary". That
reduction is exact for block geometry at every LOD level, because a level-L cell
boundary is a multiple of 2^L and therefore an integer block coordinate
(`Lod.hpp:14-18`). **One UV rule, position-derived, serves both.** The
`uv = uv.yx` swap for PosX/NegZ is already identical in both shaders.

**Cost of converging: 16 bytes instead of 12 on the block stream** — 21.43 MiB
instead of 16.07 MiB resident, i.e. **5.36 MiB**, 0.09% of a 6 GB card.

**Recommendation: converge, but not in the same stage.** The 5.36 MiB is noise
against deleting a second vertex format, a second shader program, a second VAO,
a second draw pass and the entire class of bug where two mirrored bit layouts
disagree. But the sub-voxel path is where standing invariant 2 lives, and it is
the last thing that should be touched. Ship the block path at 12 bytes first
(stages 1-3), converge at 16 bytes later (stage 4), and accept the 12→16
regression on the block stream at that point as the price of one code path. If
stage 4 is never done, the two formats coexist exactly as they do now, both
pulled, and nothing is lost.

### 2.4 How the four corners are addressed — three submission modes

`gl_VertexID` must map to (quad, corner). Three ways, all worth having behind
the A/B switch because the right answer is hardware-dependent:

**(a) `pull-arrays` — `glDrawArrays(GL_TRIANGLES, 6·firstQuad, 6·quadCount)`**

```glsl
uint quad   = uint(gl_VertexID) / 6u;
uint rawIdx = uint(gl_VertexID) % 6u;
const uint kTriCorner[6] = uint[6](0u, 1u, 2u, 0u, 2u, 3u);
uint corner = (kTriCorner[rawIdx] + flip) & 3u;
```
No index buffer at all. `first` carries the per-layer offset, so `quad` indexes
the chunk's SSBO directly and no extra uniform is needed. **Cost: 6 vertex
shader invocations per quad instead of 4** — see §5.2. Simplest possible first
cut; implement this one first.

**(b) `pull-indexed` — `glDrawElements` against one static, globally shared
index buffer** *(recommended default)*

The buffer holds, for quad q, the six values `4q+0, 4q+1, 4q+2, 4q+0, 4q+2,
4q+3`. Under `glDrawElements`, `gl_VertexID` **is** the fetched index value, so:

```glsl
uint quad   = uint(gl_VertexID) >> 2;
uint corner = ((uint(gl_VertexID) & 3u) + flip) & 3u;
```
Starting the draw at byte offset `6·firstQuad·4` makes `quad` come out absolute
within the chunk with no uniform and no `baseVertex`. **Deliberately not
`glDrawElementsBaseVertex`:** whether `basevertex` is added to `gl_VertexID`
differs between the base-vertex and the instanced-base-vertex-base-instance
commands
([docs.gl](https://docs.gl/gl4/glDrawElementsBaseVertex),
[Khronos wiki](https://wikis.khronos.org/opengl/GlDrawElementsBaseVertex)),
and this design must not depend on which reading a driver took.

Four VS invocations per quad, and the six indices reference four distinct values
so the hardware's batch-level vertex reuse (§5.2) actually fires.

Size it for the mesher's true worst case, the checkerboard chunk at 98 304
quads: 98 304 × 6 × 4 B = **2.25 MiB**, allocated once at startup, shared by
every chunk and both mesh formats. Add
`VOXL_CHECK(quadCount <= kSharedIndexQuadCapacity)` in `upload`; with the buffer
sized to the mesher's own bound there is no split path to write.

**(c) `pull-instanced` — `glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, n)`**

What *Exile* does. `gl_InstanceID` is the quad, `gl_VertexID` is 0..3, no index
buffer, 4 VS invocations. Strip order must be `{c1, c2, c0, c3}` to produce the
0-2 diagonal (GL strips emit triangle *i* as `(i,i+1,i+2)` for even *i* and
`(i+1,i,i+2)` for odd, so `v0..v3 = c1,c2,c0,c3` gives `(c1,c2,c0)` and
`(c0,c2,c3)` — the same two triangles as today). **Caveat:** `gl_InstanceID`
does **not** include `baseinstance` in OpenGL, so the per-layer quad offset needs
a uniform. **Risk:** four-vertex instances are the classic small-instance case
where the geometry front-end underutilises a warp. Keep it in the switch,
expect it to lose, be pleased if it doesn't.

### 2.5 The GLSL side

`chunk_common.glsl`'s `voxlUnpackVertex(uint, uint)` becomes
`voxlPullQuadVertex(uint quadIndex, uint corner)` returning the same `VoxlVertex`
struct, so `chunk_vertex.glsl`, `chunk.frag`, `water.frag` and every shading
function below the unpack are **unchanged**. `water.vert` includes
`chunk_vertex.glsl` and therefore comes along for free — but it must be rebuilt
in the same commit, and `tests/test_shaders.cpp` should gain an assertion that
no shader still declares `layout(location = 0) in uint aData0` once stage 3
lands.

```glsl
layout(std430, binding = 1) readonly restrict buffer VoxlQuads { uint uQuads[]; };

VoxlVertex voxlPullQuadVertex(uint quad, uint rawCorner)
{
    uint base = quad * 3u;                 // 12-byte record, flat uint array
    uint w0 = uQuads[base + 0u];
    uint w1 = uQuads[base + 1u];
    uint w2 = uQuads[base + 2u];

    uint flip   = (w0 >> 31) & 1u;
    uint corner = (rawCorner + flip) & 3u;

    vec3 origin = vec3(float( w0        & 63u),
                       float((w0 >>  6) & 63u),
                       float((w0 >> 12) & 63u));
    uint dir    = (w0 >> 18) & 7u;
    vec2 size   = vec2(float(((w0 >> 21) & 31u) + 1u),
                       float(((w0 >> 26) & 31u) + 1u));

    // Corner offsets along the frame's own U and V axes. kVoxlFaceU/V mirror
    // kFaceFrames in GreedyMesher.cpp and must move with it.
    float du = float(corner == 1u || corner == 2u) * size.x;
    float dv = float(corner == 2u || corner == 3u) * size.y;

    VoxlVertex v;
    v.position = origin + kVoxlFaceU[dir] * du + kVoxlFaceV[dir] * dv;
    v.direction = dir;
    v.textureLayer = w1 & 4095u;
    v.uv = vec2(du, dv);                    // unchanged semantics
    if (dir == 1u || dir == 4u) { v.uv = v.uv.yx; }   // PosX, NegZ - as today

    uint packedCorner = (corner == 0u) ? ((w1 >> 12) & 1023u)
                      : (corner == 1u) ? ((w1 >> 22) & 1023u)
                      : (corner == 2u) ? ( w2        & 1023u)
                                       : ((w2 >> 10) & 1023u);
    v.ao         = float( packedCorner        & 3u)  / 3.0;
    v.sunlight   = float((packedCorner >> 2)  & 15u) / 15.0;
    v.blockLight = float((packedCorner >> 6)  & 15u) / 15.0;
    return v;
}
```

**New GLSL data the CPU currently owns.** The face frame table
(`kFaceFrames`, `GreedyMesher.cpp:213`) has never crossed into GLSL, because the
CPU expanded the corners. It has to now: `kVoxlFaceU[6]` / `kVoxlFaceV[6]` are
the unit vectors of `uAxis` and `vAxis` per direction. This is a **new mirrored
contract** and it carries exactly the hazard `MeshData.hpp` warns about — a
transposed row renders whole faces backwards. Mitigate the same way
`SubVoxelMesher.cpp:56-72` already does: assert the geometric property
(`û × v̂ == outward normal`) as a `static_assert` on the C++ side, and add a text
assertion in `tests/test_shaders.cpp` that the six GLSL rows match the six C++
rows literally.

Selecting the corner payload with a chain of ternaries compiles to a branchless
select on every desktop compiler, but an indexed `uint c[4] = uint[4](...)`
lookup is also fine and arguably clearer; both are one register file access.

---

## 3. How it composes with every existing optimisation

### 3.1 Palette-compressed storage — untouched

Entirely CPU-side and entirely upstream. `GreedyMesher::loadCacheFull` /
`loadCacheLod` read through `ChunkStorage::get` and `storage.isUniform()`
exactly as now; nothing about what the mesher *emits* changes what it *reads*.
The measured 0.8-0.9 ns/`get` (PERFORMANCE.md §4.7) and the uniform-section fast
paths are unaffected. **No change.**

### 3.2 Greedy meshing — pulling is the natural fit

A merged quad's extents are per-quad by definition. Today they are stored four
times (40 bits/quad) alongside a 2-bit corner selector (8 bits/quad) whose only
job is to let the shader re-derive which corner it is looking at. Pulling
deletes both: extents once, corner from `gl_VertexID`.

Two things worth being precise about:

* **The ratio is constant, not merge-dependent.** 56 → 12 bytes holds for a
  6-quad solid chunk and for the 98 304-quad checkerboard alike. Pulling does
  not reward better merging; it removes a fixed 4.67× overhead from whatever the
  merger produces. The checkerboard's 5 505 024 bytes become **1 179 648**.
* **The merge rule does not relax.** `sweep()` merges iff the 64-bit keys are
  equal, which requires identical AO *and* identical smooth light at all four
  corners (`GreedyMesher.cpp:152-166`). Nothing in this design lets two quads
  with different corner shading merge, and nothing should — that constraint is
  what stops the banding the comment describes.

Small CPU win as a side effect: `emitQuadBlocks` currently runs four
`packVertex` calls plus a 6-element `indices.insert`. It becomes three stores.
PERFORMANCE.md §4.4 notes "only the quad-emission tail scales with the geometry",
so the checkerboard case (6.346 ms median, 98 304 quads) is where this should be
visible and is the right acceptance checkpoint for stage 1.

### 3.3 The packed vertex format — replaced, but its reasoning survives

`PackedVertex` goes away in stage 3. Its *arguments* do not: 6-bit positions
because a greedy corner reaches 32 and 5 bits is not enough; 12-bit texture
layer; 5-bit extents. All carry over verbatim into `w0`/`w1`. What goes away is
the corner selector — the one field that existed purely because the format was
per-vertex.

`kVertexFormatVersion` (`MeshData.hpp:24`) retires; `kQuadFormatVersion`
replaces it and starts at 1.

### 3.4 Texture arrays — strictly improved

The layer index becomes per-quad: **12 bits instead of 48** per quad. The
fragment side is untouched — `flat out uint vTextureLayer` still carries it, the
`sampler2DArray` binding, the mip chains and the anisotropy setting in
`Renderer.cpp` are all unaware anything happened. One subtlety worth stating:
because the value is genuinely identical across a quad's four corners, the
provoking-vertex convention for `flat` outputs is irrelevant here (the two
triangles of a quad have different provoking vertices under GL's default
last-vertex rule, and it does not matter).

### 3.5 Snapshot-based threaded meshing — untouched, and invariant 1 is safe

Record construction stays exactly where vertex construction is today: inside
`GreedyMesher::emitQuadBlocks`, on a worker, writing into the worker's own
`ChunkMeshData`. It reads `ChunkNeighbourhood` and writes nothing shared.
**Standing invariant 1 is not touched by this design at any stage** — no new
read of a chunk happens on any thread, and `World::isEditBlocked` /
`ChunkManager::isNeighbourhoodBusy` keep exactly the responsibilities they have.

The main-thread half changes: `ChunkRenderer::upload` writes one SSBO instead of
a vertex buffer and an index buffer, with three `update` calls instead of six,
and 4.67× less data crossing the PCIe bus per upload. The
`contentVersion` staleness check (`MeshData.hpp:265`, `ChunkRenderer.cpp:127`)
and standing invariant 3 (a dirty flag survives a chunk swap) are untouched.

### 3.6 Chunk LOD, including skirts — untouched

`Lod.hpp:14-18` argues the packed format needs no change at any level because a
level-L cell boundary is a multiple of 2^L and every quad corner therefore stays
an integer block coordinate in [0, 32]. **That argument transfers verbatim to
`w0`'s three 6-bit fields and to the 5-bit extents** — `emitQuad` computes
`base = n·cellSize + …` and passes `w·cellSize`, `h·cellSize` to
`emitQuadBlocks`, which already asserts both are in [1, 32]. Pulling does not
touch that arithmetic.

Skirts are the case worth checking explicitly, and they are fine: `emitSkirts`
routes through the *same* `emitQuadBlocks` with a `uniformKey`, so a skirt quad
is just a record whose four corner payloads are equal. Nothing in the curtain's
reasoning (`GreedyMesher.cpp:806-830`) depends on how the quad reaches the GPU.
**One trap:** with equal corners, `ao0 + ao2 > ao1 + ao3` is *false*, so
`addQuad` takes the **else** branch — every uniform quad, including every skirt
and every flat-lit surface, currently uses the **1-3 diagonal**. `diagonalFlip`
must therefore be **1** for those. Get this backwards and the triangle stream
changes for the majority of quads in the world; it would be invisible on screen
and would silently break any test that hashes geometry. §3.9 states the rule as
a single line to copy.

The per-LOD stats path (`drawCallsPerLod`, `trianglesPerLod`) is unaffected in
shape: `LayerRange::triangles` still exists, and `LayerRange` merely swaps
`indexCount` / `indexByteOffset` / `baseVertex` for `firstQuad` / `quadCount`.

### 3.7 The sparse sub-voxel store and its separate pass

Stages 1-3 leave it completely alone: `SubVoxelMesher`, `SubVoxelMeshData`,
`m_subVoxelVertexArray`, `subvoxel.vert` and `drawSubVoxels` all keep working on
the old 8-byte vertex + 32-bit index format. The two streams already have
separate buffers, VAOs and programs (`ChunkRenderer.hpp:289-295`), so there is
no coupling to break.

The store itself — the sorted vector, the strictly-partial invariant, the
`hasSubVoxelDamage()` early-out that makes an undamaged chunk cost 2.1 ns — is
untouched by anything here at any stage. Stage 4 changes only what
`SubVoxelMesher::sweep` writes at the very end (`out.addQuad(...)`), and the
same 56 → 16 byte arithmetic applies to it: the 64-damaged-block case drops from
14 224 to 4 064 bytes. That is irrelevant as memory and is not the reason to do
stage 4; §2.3 is.

### 3.8 Ambient occlusion — the sharp edge

**The problem as posed:** pulling stores per-quad data; AO differs per corner.

**The answer:** all four corners live in the record, and `gl_VertexID` selects
one. AO stays a genuine per-vertex output and interpolates across the triangle
exactly as it does today. **Nothing about the visual result changes.**

The bit cost, precisely:

| | today | pulled |
|---|---:|---:|
| AO + sunlight + blockLight | 10 bits × 4 vertices = **40 bits** | 4 × 10 = **40 bits** |
| as a share of the quad record | 40 / 448 = 8.9% | 40 / 96 = **41.7%** |

So per-corner shading **costs nothing extra** — it is simply the one field that
did not compress, and after everything else shrinks by 4.67× it becomes the
largest field in the record. That is the honest statement, and it is also why
Voxl's record is 12 bytes where binary-greedy-meshing's is 8: that project
[dropped AO in v2](https://github.com/cgerikj/binary-greedy-meshing). Trading
Voxl's smooth per-corner light and AO for four bytes would be trading the thing
the mesher's whole merge-key design exists to protect.

Three things that are *not* required, and should be resisted:

* **Do not move AO to the fragment shader.** *Exile* stores all four AO values
  per quad and bilinearly interpolates them in the fragment stage
  ([thenumb.at](https://thenumb.at/Voxel-Meshing-in-Exile/)). That is
  mathematically nicer — exact bilinear AO, and the diagonal artefact stops
  existing rather than being worked around. But it moves 3 channels × 3 `mix`
  operations plus a 40-bit unpack from ~4 vertices per quad to *every fragment*
  the quad covers, and Voxl's greedy quads are large. It is a fill-rate trade in
  a renderer whose fill cost is unmeasured. Keep it in the back pocket; it is a
  quality improvement to evaluate separately, not part of this change.
* **Do not relax the merge rule.** Already covered in §3.2.
* **Do not compress the corner payload.** Dropping sunlight/blockLight to 3 bits
  each would fit a unified record into 12 bytes (§2.3) and would put visible
  banding into every lighting gradient in the game.

### 3.9 The anisotropy / triangle-flip rule — preserved exactly, for one bit

`MeshLayerData::addQuad` (`MeshData.hpp:223-244`) chooses the diagonal:

```
if (ao0 + ao2 > ao1 + ao3)  ->  {0,1,2, 0,2,3}     // diagonal 0-2
else                        ->  {1,2,3, 1,3,0}     // diagonal 1-3
```

This is the standard rule from
[0fps](https://0fps.net/2013/07/03/ambient-occlusion-for-minecraft-like-worlds/),
and it exists because a bilinear AO field cannot be represented by two linearly
interpolated triangles unless the split runs the right way.

Under a shared index buffer the pattern is fixed at `{0,1,2, 0,2,3}` and cannot
vary per quad — which looks fatal. It is not. **Rotating the corner assignment
by one is exactly equivalent to flipping the diagonal:**

with `corner = (rawCorner + 1) & 3`, the vertices at raw positions 0,1,2,3
become geometric corners 1,2,3,0, so the fixed index pattern `(0,1,2)` and
`(0,2,3)` emits the triangles **(c1,c2,c3)** and **(c1,c3,c0)** — literally the
else-branch above, in the same order, with the same winding (both are CCW
sub-triangles of the CCW quad).

So:

```cpp
// PackedQuad::w0 bit 31. Copy this line; do not re-derive it.
const bool flip = !(ao0 + ao2 > ao1 + ao3);   // equal corners => flip == true
```

and in GLSL, one add: `corner = (rawCorner + flip) & 3u`. The same trick works
for the triangle-strip mode (§2.4c): strip order `{1,2,0,3}` plus one gives
`{2,3,1,0}`, which produces the flipped pair.

**Where the rule lives.** The AO comparison stays in C++, in the mesher, where
it is today — GLSL only reads a bit. That is deliberate: this codebase's
documented failure mode is a CPU/GLSL layout disagreement, and duplicating a
*rule* across the boundary is worse than duplicating a *layout*. The GLSL could
recompute `flip` from the corner payload it already unpacks, and that would save
one bit; don't. Delete `MeshLayerData::addQuad`'s index emission, keep its
comparison, and move it into `PackedQuad` construction.

**Regression test to write in stage 1** (`tests/test_mesher.cpp`): for a fixture
with anisotropic corner AO, assert that the record's `diagonalFlip` bit equals
the branch the old `addQuad` took, for both branches, *and* for the equal-AO
case. That last one is the trap in §3.6.

---

## 4. What it unlocks — and whether Voxl is draw-call bound at all

### 4.1 The honest answer first: this is currently unmeasured

`docs/PERFORMANCE.md` §5 records **1 781 draw calls at ~557-582 fps**. It does
not record how that 1.72 ms frame divides between CPU submission, CPU culling,
and GPU execution — and it cannot, because `src/core/Profiler.hpp` is a
**CPU-only** scope timer with no GL timer queries anywhere in the engine. There
is no measurement in this repository that distinguishes "the driver is the
bottleneck" from "the GPU is the bottleneck", and every claim about what MDI
would buy depends on which it is.

**That gap is why stage 0 of §6 exists and why it is unconditional.**

What can be said from arithmetic, offered as a hypothesis to test, not a result:

**The draw loop issues ~7 100 GL entry points per frame.** Per visible
chunk-layer, `ChunkRenderer::drawLayer` calls
`glVertexArrayVertexBuffer`, `glVertexArrayElementBuffer`, `glUniform3fv`
(through `ShaderProgram::setVec3`) and `glDrawElementsBaseVertex` — four calls ×
1 781 = 7 124 per frame. At 580 fps that is **4.13 million GL calls per second**.
For comparison, a conventional 60 fps target of 2 000 draws per frame is 8 000
draws/s. Voxl is submitting at a rate two-and-a-half orders of magnitude higher
because its frames are so short. It is entirely plausible that submission is a
large share of 1.72 ms; it is also possible the driver's DSA fast paths make it
cheap. **Measure it.**

**The cull walk may cost as much as the draws.** `ChunkRenderer::cull` iterates
`std::unordered_map<ChunkPos, ChunkMesh>` over **all 10 056 resident chunks**
every frame — including the 8 398 that fail the frustum test. `ChunkMesh` is
~240 bytes (two `GpuBuffer`s, a `SubVoxelGeometry` holding two more, three
`LayerRange`s, two `Aabb`s), so with MSVC's node overhead the walk pointer-chases
through roughly **2.6 MiB in allocation order** — past L2, resident in L3, and
effectively random after thousands of insert/erase cycles. Then it sorts up to
four vectors totalling ~1 800 entries. None of this is on the GPU and **vertex
pulling does not improve any of it**.

**The GPU is probably not triangle-bound.** 879 262 triangles at 580 fps is
510 M tri/s. That is a load a GA107-class part should absorb; if the GPU is the
limit it is more likely fill or bandwidth than setup. Unverified.

### 4.2 What each downstream step would actually be worth

| Step | Requires | Draw calls | Honest assessment |
|---|---|---:|---|
| Pulling alone (stages 1-3) | — | **1 781 → 1 781** | Pulling by itself changes *nothing* about draw-call count. Per draw it replaces two DSA buffer bindings with one SSBO binding, so 4 GL calls become 3 — a 25% cut in entry points, not a structural change. The win is memory and upload bandwidth. |
| Vertex pool + MDI (stage 5) | one SSBO for all chunks; suballocator; per-draw parameters | **1 781 → ~4** | This is where the draw-call win lives, and it is real *only if* stage 0 shows submission-bound. Prior art: [nickmcd's vertex pooling](https://nickmcd.me/2021/04/04/high-performance-voxel-engine/) measured **18-23% frame-time improvement** over a VAO/VBO-per-chunk renderer at equal geometry (10.6→8.7 ms and 79→61 ms), and up to 40% with draw masking, and concluded draw-call overhead was the bottleneck *for that renderer*. Voxl already shares one VAO across all chunks (`ChunkRenderer.hpp:11-13`), so it has already banked the largest part of that particular win. |
| GPU frustum culling (stage 6) | MDI + bounds in a GPU buffer + compute pass | 4 | Removes the 10 056-node CPU walk. But **a flat SoA cull array would remove most of the same cost for ~200 lines and no GPU work** — positions, `Aabb`s and layer masks in parallel `std::vector`s, iterated linearly, with the `ChunkMesh` map consulted only for survivors. Do that first; it is measurable in a day and it is a strict prerequisite for knowing whether the compute version is worth writing. |

**Two GL-4.5 constraints stage 5 must respect** (Voxl requests a 4.5 core
context):

* `glMultiDrawElementsIndirect` requires every sub-draw to share one element
  buffer *and one set of bound buffers*. An SSBO binding is per-draw state, not
  per-indirect-command, so **all chunks' quads must live in one SSBO** — hence
  the suballocator. That re-introduces fragmentation and a free-list, which the
  current one-buffer-per-chunk model does not have. It is a real cost, not a
  free upgrade.
* Per-draw parameters (chunk origin, quad base) need `gl_DrawID`, which is
  **core only in 4.6** (`ARB_shader_draw_parameters`). The 4.5-safe alternative
  is the AZDO `baseInstance` trick: a divisor-1 instanced attribute indexing a
  per-chunk array, honoured by `glMultiDrawElementsIndirect` via
  `ARB_base_instance` (core 4.2). Query the extension, pick the path, and note
  that this is a second place where the design forks by driver.

### 4.3 So: is the draw-call count the bottleneck?

**Unknown, and the profile does not say.** The arithmetic in §4.1 makes it
plausible that CPU-side work — submission *and* the cull walk together — is a
large fraction of a 1.72 ms frame, and both hypotheses are testable in an
afternoon with stage 0. What the profile *does* say, loudly, is documented in
PERFORMANCE.md §6 and §8 and has nothing to do with the renderer: **level-3
meshing is 54% of all meshing work for 2.0% of the drawn triangles**, worth 13%
of the world-build budget, cause identified, unfixed. And
KNOWN_LIMITATIONS.md records **~111 MB of resident CPU voxel data at radius 20**
that LOD does not reduce — half again the GPU geometry this document is about.
See §7.

---

## 5. Risks and costs

### 5.1 SSBO reads versus the vertex-fetch hardware path

This is the risk that most deserves respect, and it is vendor-specific.

* **AMD GCN and later:** the driver already rewrites fixed-function fetch into
  pull instructions in the shader, so pulling costs nothing there and may
  cost less. This is also why PSOs must carry the vertex format.
* **NVIDIA and Intel:** historically prefer push, because the fetch unit can
  prefetch vertices and hide latency that manual buffer reads expose as stalls.
  Timothy Lottes' position (relayed by Matias Goldberg) is that NVIDIA supports
  pull better than benchmarks suggest, and that measured gaps usually come from
  "non-optimal in-app implementation for pull model, specifically bad data
  layout or memory access patterns."
  ([yosoygames](https://www.yosoygames.com.ar/wp/2018/03/vertex-formats-part-2-fetch-vs-pull/))
* voxel.wiki's blunter summary: "Pulling from a vertex-stream or SSBO/UBO is
  generally the same speed as from a VBO/EBO."
  ([voxel.wiki](https://voxel.wiki/wiki/vertex-pulling/))

**Voxl's reference machine is an RTX 3050 — precisely the vendor where pulling
is least certain to be free.** That is not a reason to skip it; it is the reason
the A/B switch in §6 is mandatory rather than nice-to-have.

Two things make Voxl's access pattern the good case rather than the bad one:
consecutive `gl_VertexID` values map to consecutive quad records, so the reads
are perfectly sequential; and a whole quad's record is 12 contiguous bytes, so
the four (or six) invocations that need it hit the same cache line. If pulling
loses here it will not be for the reason Lottes names.

### 5.2 Loss of vertex reuse on a non-indexed draw

The naive framing — "non-indexed re-runs the vertex shader for shared corners" —
is right in direction but the mechanism is not the textbook post-transform
cache. Kerbl et al. (HPG 2018) showed **modern GPUs of all major vendors do not
behave as though a global post-transform cache exists**; they use *batch-level*
reuse, NVIDIA at 32 triangles / 96 indices per batch, Intel 620 irregularly at
~22-24 triangles
([Interplay of Light summary](https://interplayoflight.wordpress.com/2021/11/14/shaded-vertex-reuse-on-modern-gpus/),
[paper](https://dl.acm.org/doi/10.1145/3233302)).

The practical consequence for Voxl, quantified against the recorded frame:

| mode | VS invocations / quad | VS invocations / frame (439 631 visible quads) | at 580 fps |
|---|---:|---:|---:|
| today (indexed, 4 verts) | 4 | 1 758 524 | 1.02 G/s |
| `pull-arrays` (§2.4a) | **6** | **2 637 786** | **1.53 G/s** |
| `pull-indexed` (§2.4b) | 4 | 1 758 524 | 1.02 G/s |
| `pull-instanced` (§2.4c) | 4 | 1 758 524 | 1.02 G/s |

`pull-arrays` costs **+879 262 vertex shader invocations per frame, +50%**. The
shader is small (an unpack, a matrix multiply, seven varyings), so this may be
absorbed — or it may not, and it is the single easiest way for a naive
implementation of this design to come out slower than what it replaced. This is
the reason `pull-indexed` is the recommended default despite needing a 2.25 MiB
side buffer, and the reason all three modes exist behind the switch.

### 5.3 std430 layout and alignment

* Declare the SSBO as a **flat `uint` array**, `uint uQuads[]`, and stride by 3
  manually. In std430 an array of `uint` has stride 4, which is unambiguous. A
  `struct { uint a, b, c; }` *should* have array stride 12 under std430, but
  struct-stride handling is historically the buggiest corner of driver GLSL
  layout support and there is nothing to gain by testing it.
* Qualify it `readonly restrict`. `readonly` lets the compiler treat the loads
  as invariant; `restrict` removes aliasing assumptions against any other
  buffer.
* If stage 5's suballocator ever uses `glBindBufferRange`, the offset must be a
  multiple of `GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT` — **query it**; it is
  as low as 16 on some drivers and 256 on others, and a hard-coded 16 will fail
  silently-then-loudly on the first machine that says 256.
* A 12-byte record means a quad occasionally straddles a 16-byte boundary. This
  costs nothing for sequential access (the next quad's bytes are already in the
  line) but it is the argument for the 16-byte unified record in §2.3 if
  measurement ever shows a fetch penalty.

### 5.4 Availability: SSBOs in a vertex shader are not guaranteed

`GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS` has a **specified minimum of 0**
([ARB_shader_storage_buffer_object](https://www.khronos.org/registry/OpenGL/extensions/ARB/ARB_shader_storage_buffer_object.txt)),
and real hardware does report 0 — it is a recurring support issue in engines
that assumed otherwise. A GL 4.5 context does **not** promise you can read an
SSBO from a vertex shader.

Mitigations, in order of preference:

1. `glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &n)` at startup; if
   `n < 1`, fall back. Because of the A/B switch this fallback is free — it is
   the classic path, which is still there and still tested.
2. Rákos's original formulation: a **buffer texture**
   (`usamplerBuffer` + `texelFetch`, `GL_RGB32UI` or `GL_RGBA32UI`). Vertex
   texture units are guaranteed (≥ 16) where vertex SSBOs are not, and a
   `GL_RGBA32UI` fetch is one 16-byte read through the texture cache — which
   pairs naturally with the 16-byte unified record. Keep this in mind as a
   fourth submission mode if any target machine reports 0.

### 5.5 Debuggability

A pulled draw shows **no vertex attributes in RenderDoc**. The Mesh Viewer's
"VS Input" tab is empty, the pipeline state shows a VAO with nothing bound, and
the geometry can only be inspected as "VS Output" after the shader has run. When
a face renders backwards, the fastest diagnosis today — look at the input
positions — stops existing.

Three cheap mitigations, all worth doing regardless:

* **Keep the classic path through at least one shipped release.** A RenderDoc
  capture on `--geometry classic` shows the same geometry with full vertex
  input. This is the strongest argument for staging the migration rather than
  switching in one commit.
* **`glObjectLabel` every buffer** with the chunk position, so a capture's
  buffer list is readable. This is a small, independent improvement to
  `GpuBuffer::allocate` that should land in stage 0.
* **A CPU-side decoder.** `unpackQuad(PackedQuad) -> QuadAttributes` mirroring
  `unpackVertex`, plus a debug command that dumps a chunk's records as text.
  Costs nothing at runtime and is the tool that will actually find the bug.

### 5.6 A new mirrored contract

§2.5: the face frame table has to exist in GLSL for the first time. This is the
one place the change *adds* a synchronisation hazard rather than removing one.
Guard it with the geometric `static_assert` already used in
`SubVoxelMesher.cpp:56-72` plus a text assertion in `tests/test_shaders.cpp`.

### 5.7 Things that are explicitly *not* risks here

Worth stating so nobody spends time on them: threading (§3.5 — no new
cross-thread read), the sub-voxel invariant (untouched until stage 4, and then
only at the emit site), determinism (record construction is the same integer
arithmetic in the same order), LOD correctness (§3.6), and persistence (the save
format stores voxels, not meshes).

---

## 6. Staged migration plan

Every stage builds, runs, passes the suite and is shippable. Every stage has a
measurement and a stated threshold. The A/B mechanism arrives in stage 0 so that
every later stage can be compared against the one before it on the same machine
in the same session.

### The A/B mechanism

* **`--geometry classic|pull-arrays|pull-indexed|pull-instanced`** in
  `src/app/Main.cpp`, next to the existing `--freeze` / `--radius` / `--carve`
  flags, plus a `geometry_mode` key in `Settings` (which preserves unknown keys,
  so an older binary will not eat it).
* **Runtime switching forces a full re-upload.** Both paths cannot share a
  chunk's GPU buffers, and keeping both resident would double VRAM. Switching
  calls `ChunkRenderer::clear()` and marks every resident chunk dirty; at
  radius 20 that is roughly the 1.1 s of wall time PERFORMANCE.md §6 computes
  for a full world build. Acceptable for a benchmarking toggle, and it must be
  documented as such rather than presented as a graphics option.
* **`--bench-frames N --bench-out <path>`**: run N frames from a fixed
  `--pos`/`--look`/`--freeze`/`--freeze-time` viewpoint, then write one
  long-format CSV row per (pass, metric) in exactly the
  `group,case,kind,metric,value,unit,note` shape `benchmarks/results/` already
  uses, so a new run diffs cleanly against the old one. This is what makes the
  A/B reproducible instead of anecdotal.
* **The reference viewpoint** must be pinned once and reused by every stage.
  Suggest recording it in `docs/design/VOXEL_PULLING_BENCH.md` alongside the
  first result table.

---

### Stage 0 — Instrument. No format change.

**Why first:** §4 cannot be answered without it, and stages 5-6 must not be
attempted on a hunch.

**Do:** add a small double-buffered `GpuTimer` (a ring of
`GL_TIME_ELAPSED` query objects, read one frame late so nothing stalls) and wrap
`drawSky`, the opaque pass, the sub-voxel pass, the cutout pass and the
translucent pass. Add CPU `ProfileScope`s around `ChunkRenderer::cull`, around
each `drawLayer`, and around the sort block. Surface both columns in the F3
overlay. Add `glObjectLabel` in `GpuBuffer::allocate` (§5.5). Add
`--bench-frames` / `--bench-out`.

**Files:** `src/core/GpuTimer.{hpp,cpp}` (new), `src/render/Renderer.{hpp,cpp}`,
`src/render/ChunkRenderer.{hpp,cpp}`, `src/render/GpuBuffer.cpp`,
`src/ui/DebugOverlay.cpp`, `src/app/Main.cpp`, `src/core/CMakeLists.txt`.

**Measure:** at the reference viewpoint, 600 frames — CPU ms and GPU ms per
pass, CPU ms for cull, total frame ms, and the three sums. Commit the table.

**Acceptance:** none — this stage exists to produce a number, and the number
decides the fate of stages 5 and 6. Instrumentation overhead must be < 1% of
frame time or the timers are in the wrong place.

**Decision this stage produces:**
`CPU submission + cull` vs `GPU total`. If GPU dominates, stages 5-6 are dead
and stages 1-4 are a pure memory play. If CPU dominates, stage 5 is on.

---

### Stage 1 — `PackedQuad` becomes the mesher's output. Renderer unchanged.

**Do:** add `src/mesh/QuadData.hpp` with the §2.1 layout, `packQuad` /
`unpackQuad`, and the `static_assert` round-trips `MeshData.hpp` already models.
Change `MeshLayerData` to hold `std::vector<PackedQuad> quads`, and add
`expandToVertices(std::vector<PackedVertex>&, std::vector<MeshIndex>&) const` —
the exact inverse of what `addQuad` does today. `GreedyMesher::emitQuadBlocks`
emits one record. `ChunkRenderer::upload` calls `expandToVertices` into a
reusable main-thread scratch buffer and uploads exactly the bytes it does now.

**Why this shape:** it moves the risky part (the mesher) behind a shim that is
*provably* equivalent, and it keeps the renderer — the part with no test
coverage, because the suite is headless — completely still.

**Files:** `src/mesh/QuadData.hpp` (new), `src/mesh/MeshData.hpp`,
`src/mesh/GreedyMesher.cpp`, `src/render/ChunkRenderer.cpp`,
`tests/test_mesher.cpp`, `tests/test_lod_mesh.cpp`, `benchmarks/bench_meshing.cpp`
(the `mesh_bytes` counter now reports record bytes — report **both**, so the
CSV stays diffable across the change).

**Tests to add:**
1. **Stream equivalence.** For each of the four `bench_meshing` fixtures, mesh
   with the new path, `expandToVertices`, and compare byte-for-byte against a
   golden vertex+index stream captured from the current build. This is the whole
   safety net for stage 1 and it must be exact, not approximate.
2. **Flip bit.** Three cases — `ao0+ao2 > ao1+ao3`, `<`, and `==` — asserting
   the bit matches the branch `addQuad` takes. The `==` case is the §3.6 trap.
3. **Skirt records.** A level-1-beside-level-0 fixture: 24 skirt quads, all with
   four equal corner payloads and `diagonalFlip == 1`.

**Measure:** `meshing/greedy_lod0` and `meshing/greedy_checkerboard` medians;
`mesh_bytes` in records.

**Acceptance:** golden streams byte-identical. `greedy_lod0` median must not
regress by more than 2%. `greedy_checkerboard` median is *expected to improve*
(four `packVertex` calls and a 6-element insert become three stores per quad);
if it regresses at all, something is wrong with the emit path.

**Shippable:** yes — nothing about the rendered frame or the GPU has changed.

---

### Stage 2 — The pulled draw path, behind the switch. Classic still default.

**Do:** `ChunkRenderer` grows a second residency representation: one SSBO per
chunk (`GpuBuffer` unchanged — it is already just an immutable-storage
allocation), `LayerRange` gains `firstQuad` / `quadCount`, and the shared index
buffer (§2.4b) is created once in the constructor. Add `chunk_pull.vert` /
`water_pull.vert` and the `voxlPullQuadVertex` function in
`chunk_common.glsl`; `Renderer::loadShaders` picks the program pair by mode.
Implement all three submission modes; they differ only in the draw call and four
lines of GLSL.

**Files:** `src/render/ChunkRenderer.{hpp,cpp}`, `src/render/Renderer.{hpp,cpp}`,
`src/app/Settings.{hpp,cpp}`, `src/app/Main.cpp`,
`assets/shaders/chunk_common.glsl`, `assets/shaders/chunk_pull.vert` (new),
`assets/shaders/water_pull.vert` (new), `tests/test_shaders.cpp`.

**Guard:** `glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, …)` at
`Renderer` construction; log and force `classic` when it is 0 (§5.4).

**Measure:** at the reference viewpoint, for all four modes — GPU ms per pass,
CPU ms in the draw loop, `gpuBytes`, draw calls, frame ms. Plus a screenshot
pair per mode through `tools/capture.ps1`, reviewed against the
`docs/VISUAL_REVIEW.md` protocol. The suite cannot check this; a human has to
look.

**Acceptance to flip the default:**
* screenshots visually identical to classic at the reference viewpoint and at a
  cave mouth (AO gradients) and an LOD band edge (skirts) — the two places a
  corner-ordering or flip mistake would show; **and**
* `gpuBytes` drops by ≥ 3.0× (predicted 4.09× including the shared index
  buffer); **and**
* GPU ms for the opaque pass ≤ 105% of classic in the winning mode.

If the best pulled mode is more than 5% slower on GPU ms, **do not flip the
default** — ship the mode switch, record the numbers in
`docs/PERFORMANCE.md`, and stop. That is a legitimate outcome and it is what
§5.1 says may happen on NVIDIA. The memory win would still be available to
anyone who wants it, and the measurement is the deliverable.

**Shippable:** yes — default behaviour is unchanged.

---

### Stage 3 — Delete the classic path.

Only after stage 2's default has flipped **and shipped in a release**. Remove
`PackedVertex`, `packVertex`, `unpackVertex`, `QuadCorner`, `MeshIndex`,
`expandToVertices`, the per-chunk index buffers, `kAttribData0/1` and the
`setIntegerAttribute` calls, `chunk.vert` / `water.vert`'s attribute
declarations, and `kVertexFormatVersion`.

**Do not delete the tests that covered them** — port each one to the record
(standing rule: never delete a test to make the suite pass). The pack/unpack
round-trip `static_assert`s in `MeshData.hpp:167-175` have direct `PackedQuad`
equivalents and must be written, not dropped.

**Measure:** `gpuBytes` at the reference viewpoint (predict ≈ 18.3 MiB), GL
buffer object count (20 112 → 10 056), meshing medians unchanged.

**Acceptance:** 325/325 plus the new tests; `gpuBytes` within 5% of the 18.3 MiB
prediction. If it is not, the arithmetic in §1.4 is wrong and that is worth
knowing before going further.

---

### Stage 4 — Converge the sub-voxel format (optional, low priority)

Move both streams onto the 16-byte unified record of §2.3: one header, one
program, one VAO, one draw path, position-derived UV for both. `SubVoxelMesher`
changes only at `out.addQuad(...)`; the store, the invariant, and the
`hasSubVoxelDamage()` early-out are untouched.

**Measure:** `subvoxel/mesh_damage_*` medians (must not regress — the 2.1 ns
undamaged case especially), `subVoxelGpuBytes`, block-stream `gpuBytes`
(16.07 → 21.43 MiB, expected and accepted), and carved-surface screenshots
against `docs/images/03-milestone3-subvoxels/`.

**Acceptance:** carved geometry pixel-comparable to the stage-3 build;
`subvoxel/mesh_damage_0` unchanged; the block stream's regression is bounded at
the predicted +5.36 MiB and no more.

**Value:** a deleted shader program and a deleted format contract. Not
performance. Do it when the sub-voxel path is next being touched for another
reason, not as a project of its own.

---

### Stage 5 — Vertex pool + `glMultiDrawElementsIndirect`

**Gate: only if stage 0 measured CPU submission at > 25% of frame time.**

One large SSBO with a fixed-bucket suballocator (nickmcd's vertex pooling,
adapted to quad records), a `DrawElementsIndirectCommand` buffer rebuilt per
frame from the cull result, and per-draw parameters via `gl_DrawID` (4.6) or the
`baseInstance` + divisor-1 attribute trick (4.5). Note the two constraints in
§4.2 before starting.

**Measure:** draw calls, CPU draw-loop ms, GPU ms, frame ms, and **pool
fragmentation** (allocated buckets vs live records) — the cost this stage adds
that the current model does not have.

**Acceptance:** draw calls ≤ 8; CPU draw-loop ms down ≥ 50%; **total frame ms
improved by ≥ 5%**. If frame time does not move, revert — a suballocator with
fragmentation is a permanent complexity cost and an unmeasurable win does not
pay for it.

---

### Stage 6 — GPU frustum culling

**Gate: only after a flat SoA cull array has been tried and measured
(§4.2).** If the SoA rewrite already takes cull below ~5% of frame time, stop
there; a compute pass, a bounds buffer, an atomic counter and a
`GL_SHADER_STORAGE_BARRIER_BIT` are a lot of machinery for the remainder.

---

## 7. Recommendation

**Do stage 0 now, unconditionally, whatever else is decided.** The engine cannot
currently distinguish a CPU-bound frame from a GPU-bound one, three of the six
stages below depend on that distinction, and the instrumentation is a day's
work that pays for itself the next time any rendering question is asked. This is
the highest-value item in this document and it is not vertex pulling.

**Do stages 1-3 — pull the block path.** The case is memory and it is
arithmetic, not hope: 56 bytes per quad becomes 12, verified against every row
of the committed benchmark CSV; **75 MiB of live GPU geometry becomes 18.3 MiB
including the shared index buffer, a 4.09× reduction**, with allocated VRAM
falling from ~104 MiB to ~25 MiB and 10 056 GL buffer objects deleted. Upload
traffic falls by the same 4.67×. No visual change, no threading change, no LOD
change, and the mesher gets slightly cheaper on the emit tail. Even in the worst
case for §5.1 — pulling being exactly break-even on this NVIDIA part — the
memory is banked, and memory is what buys render distance on a 6 GB laptop card.

Use **`pull-indexed`** as the default submission mode. The 2.25 MiB shared index
buffer is a rounding error against the 59 MiB saved, and it is what keeps the
vertex shader at 4 invocations per quad instead of 6 (§5.2) — the single most
likely way to make this change a regression.

**Stage 4 (sub-voxel convergence): yes, but not on its own schedule.** It is a
simplification worth 5.36 MiB of *cost*, not benefit. Its value is deleting a
second vertex format, a second program and a second pass, and pulling is what
makes it possible at all (§2.3). Fold it into the next piece of sub-voxel work.

**Stage 5 (MDI): conditional, and probably smaller than it looks.** Voxl has
already banked the largest part of the classic draw-call win — it shares one VAO
across all chunks rather than one per chunk, which is exactly the difference
nickmcd measured at 18-23%. What remains is ~7 100 GL entry points per frame,
and whether that matters is unmeasured. It also carries a permanent cost the
current design does not have: a suballocator with fragmentation, replacing a
model where a chunk's memory is simply its own. Gate it on stage 0 and honour
the revert threshold.

**Stage 6 (GPU culling): not yet.** Try the flat SoA cull array first. It is a
fraction of the work, it needs no GPU involvement, and it is very likely to
capture most of the win from replacing a 10 056-node pointer chase through
2.6 MiB of unordered-map nodes.

### And the thing this document is not

Vertex pulling is the right renderer-side change and it should be done. It is
**not the largest win available in this engine**, and the profile says so
plainly:

* **Level-3 meshing is 54% of all meshing work for 2.0% of the drawn
  triangles** (PERFORMANCE.md §6). The cause is identified — `loadCacheLod`
  reads *more* blocks at coarser levels (110 592 at level 3 against 39 304 at
  level 0) and runs a majority vote per cell where level 0 is a straight copy.
  Fixing it is worth **13% of the entire world-build budget** and the harness
  already flags it automatically. It is listed as unfixed in §8.
* **~111 MB of resident CPU voxel data at radius 20**, which LOD does not reduce
  because a coarse chunk still allocates a full `ChunkStorage`
  (KNOWN_LIMITATIONS.md). That is half again the GPU geometry this document is
  about, and storing coarse chunks at their own resolution would save more than
  pulling does.

If the goal is "make radius 24 possible", pulling contributes 59 MiB of VRAM,
the LOD storage fix contributes considerably more system RAM, and the level-3
meshing fix contributes the streaming latency. Pulling is the cheapest and
safest of the three, and it is a good place to start — but it should be started
with clear eyes about where it sits in the ranking.

---

## Sources

* [Vertex Pulling — Voxel.Wiki](https://voxel.wiki/wiki/vertex-pulling/)
* [Programmable Vertex Pulling — J Stephano](https://ktstephano.github.io/rendering/opengl/prog_vtx_pulling)
* [Implementing programmable vertex pulling (PVP) in OpenGL — 3D Graphics Rendering Cookbook (Rákos background)](https://subscription.packtpub.com/book/game-development/9781838986193/3/ch03lvl1sec27/implementing-programmable-vertex-pulling-pvp-in-opengl)
* [nlguillemot/ProgrammablePulling — experiment harness for the OpenGL Insights technique](https://github.com/nlguillemot/ProgrammablePulling)
* [Vertex Formats Part 2: Fetch vs Pull — Yosoygames](https://www.yosoygames.com.ar/wp/2018/03/vertex-formats-part-2-fetch-vs-pull/)
* [cgerikj/binary-greedy-meshing — 8-byte quads, vertex pulling, one MDI draw](https://github.com/cgerikj/binary-greedy-meshing)
* [omar-owis/VoxelEngine — GPU-driven voxel engine, MDI + compute culling](https://github.com/omar-owis/VoxelEngine)
* [Exile: Voxel Rendering Pipeline — instanced pulling, per-quad AO, bilinear AO in the fragment stage](https://thenumb.at/Voxel-Meshing-in-Exile/)
* [High Performance Voxel Engine: Vertex Pooling — Nick's Blog (measured 18-23% frame-time win)](https://nickmcd.me/2021/04/04/high-performance-voxel-engine/)
* [Ambient occlusion for Minecraft-like worlds — 0 FPS (the flip rule)](https://0fps.net/2013/07/03/ambient-occlusion-for-minecraft-like-worlds/)
* [Shaded vertex reuse on modern GPUs — Interplay of Light](https://interplayoflight.wordpress.com/2021/11/14/shaded-vertex-reuse-on-modern-gpus/)
* [Revisiting the Vertex Cache — Kerbl et al., PACMCGIT 1(2), 2018](https://dl.acm.org/doi/10.1145/3233302)
* [ARB_shader_storage_buffer_object — MAX_VERTEX_SHADER_STORAGE_BLOCKS minimum is 0](https://www.khronos.org/registry/OpenGL/extensions/ARB/ARB_shader_storage_buffer_object.txt)
* [glDrawElementsBaseVertex — docs.gl](https://docs.gl/gl4/glDrawElementsBaseVertex) and [Khronos wiki](https://wikis.khronos.org/opengl/GlDrawElementsBaseVertex) (the `gl_VertexID` + `basevertex` ambiguity)
* [Rendering — Voxel.Wiki (MDI and GPU culling overview)](https://voxel.wiki/wiki/rendering/)

In-repo sources for every Voxl figure: `docs/PERFORMANCE.md` §4.4, §4.5, §4.6,
§5, §6, §8; `benchmarks/results/reference_i7-13650HX.csv`;
`docs/KNOWN_LIMITATIONS.md`.
