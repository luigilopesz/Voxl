# World scale: what it costs to march a big world

**Recorded:** 2026-07-31
**Tree measured:** `C:\voxl2_ws`, a `robocopy /MIR` mirror of `C:\voxl2` at commit `4594a45`
("Add the Voxl test scene, a memory cap, and command-line capture"), plus the research changes in
sec 0.2. **Nothing here was measured in `C:\voxl2` and nothing was changed there.**
**Machine:** RTX 3050 6GB Laptop (6144 MiB, driver 610.74), i7-13650HX, Windows 11.
**Resolution:** 1280x720 native. **GPU verified** as the discrete card -- the overlay in
`docs/images/world-scale/01-anchor-voxl-spawn.png` reads `GPU: NVIDIA GeForce RTX 3050 6GB Laptop GPU`, not the Intel iGPU.

**Anchor:** the Voxl test scene at the default spawn measures **10.900 ms / 91.7 fps**
(`Q-anchor-voxl-spawn`, 3016 settled frames, uncontended), against the brief's stated 10.93 ms /
91.5 fps. Everything below is on that footing.

---

## 0. Read this first

### 0.1 The five findings

1. **There is no far field to make cheap.** The world is a cube of `CHUNKS_PER_AXIS`^3 four-metre
   chunks that wraps around the player -- currently 16 chunks, a **64 m cube**. Outside it nothing
   exists, and a ray that leaves is shaded as sky. "Mountains in the distance" is not yet a
   performance problem, it is a *representation* problem. `docs/images/world-scale/04-world-32m.png` -- a 32 m
   world -- is a scrap of green at the bottom of an otherwise empty frame.

2. **The engine has an empty-space skipper, not an LOD pyramid.** The in-chunk uniformity pyramid
   is real and six levels deep, but each level stores one *bit* per block ("is this block
   uniform"), not a filtered colour and normal. A ray can be told "you may take a 4 m step here";
   it can never be told "stop here and shade this 4 m block".

3. **Naive distance-LOD termination is a net LOSS on this renderer -- 16-20% slower, not faster.**
   This is the finding I most expect to be surprising, it is measured in sec 3.2, and the mechanism in
   sec 4.1 explains it: terminating rays early manufactures phantom *surfaces* where there was sky,
   and on this renderer a surface pixel costs a shadow ray, a diffuse ray, a reflection ray and the
   denoising stack while a sky pixel costs almost nothing. The march saved is much smaller than
   the shading added. **Distance LOD only pays if the coarse hit is a real surface** -- i.e. if
   there is a correct filtered far field -- which is exactly what sec 6 proposes.

4. **View distance costs about 0.11 ms per metre, linearly, and that is the number that decides
   the target.** Measured over 4 m to 64 m in a fully generated 128 m world with a fixed camera
   (sec 4.2), the marginal cost per added metre is between 0.086 and 0.126 ms and shows no sign of
   flattening. Extrapolated -- as an order-of-magnitude argument only -- a 500 m view at 16 voxels/m
   is about **60 ms per frame** against a 4.17 ms budget. That, not memory, is the first-order
   reason the far field has to be coarse. The flip side: **32 m of full detail costs 2.8 ms**, so
   the "grass close to the player" half of the target is affordable.
   Separately, near geometry is worse than distance per pixel -- looking down at grass one metre
   away is **2.06x** the cost of looking at the sky (sec 4.1).

5. **The chunk table is what kills a big world, and it is indifferent to voxel size.** It costs
   `8216 x CHUNKS_PER_AXIS^3` bytes, resident, empty or not -- 2054 MB at a 256 m world, 131 GB at a
   1 km one (sec 5.1). But a chunk is always 64^3 voxels, so its size *in metres* is set purely by
   `LOG2_VOXEL_SIZE`, and the table cost **does not depend on it** (sec 5.2). Coarser nested volumes
   buy view distance for free in table terms and shorten the march by the same factor. sec 6 puts
   numbers on it: **128 MB of table for a 2 km view radius**, against 32 MB for today's 32 m.

### 0.2 How to reproduce anything here

```powershell
# mirror the tree, clear the stale absolute paths out of the CMake cache, build (36 s)
robocopy C:\voxl2 C:\voxl2_ws /MIR /MT:16
Remove-Item -Recurse -Force C:\voxl2_ws\.out\cl-x86_64-windows-msvc\CMakeCache.txt, `
    C:\voxl2_ws\.out\cl-x86_64-windows-msvc\CMakeFiles
powershell -File C:\voxl2_ws\tools\build.ps1 -Config Release -Reconfigure
```

Every frame-time number comes from `--bench-csv`, one row per frame:

```powershell
$env:VOXL_DATA_DIR = 'C:\voxl2_ws\_data'      # see 0.3 -- this matters more than it looks
C:\voxl2_ws\.out\cl-x86_64-windows-msvc\Release\gvox_engine.exe `
    --pos -182.99,-109.98,-46.97 --rot 0.7854,1.0964 --exit-after 45 `
    --bench-csv out.csv --width 1280 --height 720 --unpause --no-overlay
```

Scripts are in the session scratchpad under `wsx\`: `m.ps1` (one run + analysis), `mr.ps1`
(repeats), `pool.ps1` / `settled.ps1` (re-analysis from raw CSVs), and the knob setters
`knob.ps1`, `steps.ps1`, `cpa.ps1`, `parts.ps1`. Three research changes exist in the mirror:

```glsl
// src/voxels/impl/trace.glsl
#define VOXL_EXP_MAX_DIST_CM 0   // clamp the march loop to N centimetres. 0 = off.
#define VOXL_EXP_CONE_K_E4   0   // x10000; apply the depth prepass's distance LOD to the main
                                 // trace too, with this constant. 0 = off (exact trace).
// src/voxels/brushes.glsl
#define VOXL_EXP_NO_PARTICLE_SPAWN 0   // suppress particle spawns, leaving voxels untouched
```

plus a `VOXL_DATA_DIR` override in `ui.cpp` (sec 0.3). The two trace knobs are integers **on
purpose**: the GLSL preprocessor's `#if` is integer-only, a float literal there is a compile error,
and daxa runs with `register_null_pipelines_when_first_compile_fails`, so the mistake presents as a
hang rather than a diagnostic. The distance knob clamps a local `march_limit`, **not**
`info.max_dist`, because `result.dist` is seeded from `info.max_dist` and callers test
`dist == MAX_DIST` to mean "missed, shade as sky" (`trace_primary.comp.glsl:140`) -- lowering
`info.max_dist` would turn every clipped ray into a bogus *hit* at the clip plane and drag a full
GI shade in behind it.

### 0.3 The measurement hazard that invalidated a day of numbers

**All four engine builds on this machine share one settings file, and other agents were editing it
while I measured.** `AppUi` derives its data directory from `sago::getDataHome()` (`ui.cpp:87`),
which resolves through the Win32 known-folder API and **ignores `%APPDATA%`** -- I verified that by
overriding the environment variable and watching the file not move. So `C:\voxl2`,
`C:\voxl2_prof`, `C:\voxl2rs` and this mirror all read and write
`%APPDATA%\GabeVoxelGame\user_settings.json`, and the settings UI rewrites it on every change
(`ui.cpp:395`, `:772`).

When I finally inspected that file it contained **`Render Res Scale` 0.5** (default 1.0),
**`Update Sky` false** (default true) and **`ray_traced_reflections` false** -- none of which I set
 --  with an mtime *inside* my measurement window. That is why my 70-second captures came out with a
black sky. Two sweeps of the same configuration disagreed by 2x in opposite directions, and a
cone-LOD result reversed sign once the settings were pinned.

**The fix, and the thing to carry forward: `VOXL_DATA_DIR`.** I added a scratch-tree override in
`ui.cpp` that points the build at its own directory. The file it writes there has
`"categories": null`, so every setting falls back to its compiled-in default and nothing outside
the harness can change it mid-experiment. **This is worth landing in `C:\voxl2` for everyone**  -- 
concurrent agents cannot measure anything reliably while they share mutable render settings.

Consequences for this document:

- **Only measurements taken with settings pinned are reported as results.** Those are the `Q`, `R`,
  `S` and `T` series. With pinning, repeatability is excellent: `Q-anchor` 10.900 ms and `R-cone0`
  10.911 ms are the same configuration measured 20 minutes apart, 0.1% apart.
- **The earlier `C`, `H`, `K`, `L`, `M`, `N` series are discarded as frame-time evidence** and I
  have not quoted their timings. They are not merely offset -- the settings changed *between and
  within* sweeps, so they are internally inconsistent. Their *screenshots* are still valid
  qualitative evidence and their *memory* figures are unaffected (heap use depends on world
  content, not on render settings), so both are still cited.
- **GPU contention is real but was not the main problem.** A second `gvox_engine.exe` was alive in
  78 of 90 one-second samples (an 87% duty cycle). `m.ps1` waits for the GPU to be free, then
  watches for an intruder and reports `CONTENDED`. **Every pinned number quoted below has
  `CONTENDED=False` over its whole run.** I also ruled out thermal throttling: 60 s of `nvidia-smi`
  sampling during runs gives 1822-1927 MHz SM clock (5.8% spread) at 68-78  degC.

### 0.4 Two bugs of my own, because they produce plausible wrong numbers silently

- The harness formatted frame times in the machine's pt-BR locale (`10,887`) and the caller
  re-parsed them with the invariant-culture `[double]` cast, turning a 10.9 ms frame into 10887  -- 
  and hence into "the fastest sample in the sweep". Now invariant-culture on both sides. Same class
  of bug as the one `cli.cpp:10` is commented about.
- A `(?m)^#define CHUNKS_PER_AXIS \d+$` regex silently failed against CRLF line endings in
  `voxel_malloc.inl` (in .NET multiline mode `$` matches before the `\n`, so `\d+$` never clears the
  `\r`). A world-size sweep therefore built the same world three times and produced three
  "different" answers. The knob setters now use `\r?$` **and read the value back and assert**.

---

## 1. What the storage actually is

From `src/voxels/impl/voxel_malloc.inl`, `voxels.inl`, `voxels.glsl`.

### 1.1 The shape of the world

| | |
|---|---|
| `CHUNK_SIZE` | 64 -- a chunk is 64^3 voxels |
| `LOG2_VOXEL_SIZE` | -4 -- a voxel is 1/16 m = 6.25 cm |
| chunk edge in metres | `64 x 0.0625` = **4 m** |
| `CHUNKS_PER_AXIS` | **16** (32 upstream; lowered under parallel allocator work) |
| world edge | `16 x 4` = **64 m**, i.e. +/-32 m about the player |
| `PALETTE_REGION_SIZE` | 8 -- compression and the bottom of the pyramid work on 8^3 blocks |
| `PALETTES_PER_CHUNK` | 512 |
| `MAX_STEPS` | 512 (`utilities/gpu/math.glsl:8`) |

The world **wraps**: `calc_chunk_index()` (`voxels.glsl:94`) takes the chunk index modulo
`CHUNKS_PER_AXIS` about the player's chunk offset, and `PerChunkComputeShader`
(`voxel_world.comp.glsl:60-83`) invalidates and regenerates the chunks falling off the trailing
edge as the player moves. The cube is a *window* that slides with the player, not a level.

### 1.2 The two resident structures

**The chunk table** -- `voxel_chunks`, a flat array of `CHUNKS_PER_AXIS^3` `VoxelLeafChunk`,
allocated once at startup (`voxel_world.cpp:121`), resident whether or not anything is in it:

```
struct VoxelLeafChunk {
    u32 flags;                          //    4
    u32 update_index;                   //    4
    u32 uniformity_bits[3];             //   12   <- accel levels x16, x32, x64
                                        // +  4   padding to align the u64 below
    u64 page_allocation_infos[512];     // 4096   chunk-local page sub-allocator
    PaletteHeader palette_headers[512]; // 4096   {u32 variant_n; u32 blob_ptr;} per 8^3 region
};                                      // = 8216 bytes
```

**8216 bytes per chunk** (layout arithmetic; the same figure appears independently at
`voxel_malloc.inl:14` and in `BASELINE.md`). **8192 of those 8216 -- 99.7% -- are the
per-palette-region arrays, resident for a chunk of pure air exactly as for a chunk of rock.** That
is the fact sec 5 turns on.

**The voxel heap** -- `VoxelMallocPageAllocator`, 2112-byte pages, grown 1.5x on demand, never
shrunk. Each 8^3 palette region gets one allocation: 3 u32s of acceleration bits, then either
`variant_n` palette entries plus `ceil_log2(variant_n)` bits per voxel, or -- above
`PALETTE_MAX_COMPRESSED_VARIANT_N` = 367 distinct values -- 512 raw u32s. A region whose 512 voxels
are identical has `variant_n < 2` and **stores no blob at all**: the "pointer" field holds the
voxel (`voxels.glsl:156`). That is why an empty world costs table but not heap, and why heap use
tracks surface area rather than volume.

### 1.3 What is not there

- **No inter-chunk acceleration.** `VoxelParentChunk` is declared and `DAXA_DECL_BUFFER_PTR`'d at
  `voxels.inl:25-30`; the allocator declaration, buffer, clear, realloc and per-frame update are
  all commented out across `voxels.inl`, `voxel_world.cpp` and `perframe.comp.glsl`. Upstream
  started a two-level hierarchy and abandoned it.
- **No streaming and no persistence.** Chunks are generated on the GPU from the brush and dropped
  when they leave the window.
- **No mip pyramid over the voxel data.** See sec 2.3.

---

## 2. The acceleration structure, exactly

### 2.1 Six levels, 6.25 cm to 4 m

`sample_lod()` (`voxels.glsl:190-240`) returns the finest level at which the block containing the
sample point is **non-uniform**; the marcher turns that into a step of `2^(lod-1)` voxels
(`trace.glsl:127`):

| return | block that is non-uniform | step taken |
|---|---|---|
| 0 | the voxel itself is solid | hit |
| 1 | 2^3 | 1 voxel = 6.25 cm |
| 2 | 4^3 | 2 voxels = 12.5 cm |
| 3 | 8^3 | 4 voxels = 25 cm |
| 4 | 16^3 | 8 voxels = 50 cm |
| 5 | 32^3 | 16 voxels = 1 m |
| 6 | 64^3 | 32 voxels = 2 m |
| 7 | nothing -- whole chunk uniform | 64 voxels = **4 m** |

Level 7 is also returned for a chunk not yet generated
(`flags & CHUNK_FLAGS_ACCEL_GENERATED == 0`) -- **an ungenerated chunk reads as empty air**. Worth
knowing before trusting any screenshot: `docs/images/world-scale/11-ungenerated-world-reads-as-empty.png` is a blank sky because that run was
contended down to ~18 fps and never finished generating its world.

### 2.2 Where the bits live, and what a step really costs

- **x2, x4, x8** live in the first `PALETTE_ACCELERATION_STRUCTURE_SIZE_U32S` = **3 u32s of the
  heap blob** for that 8^3 region -- a dependent pointer chase through the allocator
  (`voxel_malloc_address_to_u32_ptr`). `voxels.glsl:34-46` packs 4^3 = 64 x2-bits into u32s [0..1],
  2^3 = 8 x4-bits into bits 0-7 of u32 [2], and the single x8 bit into bit 8 of u32 [2].
- **x16, x32, x64** live in `VoxelLeafChunk.uniformity_bits[3]`, i.e. in the resident table.

Overhead: 12 bytes per 8^3 region plus 12 bytes per chunk -- about **0.023 bytes per voxel**. Cheap,
and not the problem.

A march step is not a bit test, though. Because `VOXEL_ACCEL_UNIFORMITY` is 1,
`voxels.glsl:211-224` runs the full palette decode `sample_palette()` on **every step, before any
uniformity test** -- so each step is a chunk-table read, a palette-header read, a pointer chase into
a multi-hundred-megabyte heap, a bit-unpack, and only then the uniformity tests. That is why
crawling through grass at 6.25 cm per step costs what sec 4.1 measures.

### 2.3 The thing that decides everything

**The pyramid stores uniformity, one bit per block. It does not store a filtered voxel.** No
averaged colour, no representative normal, no coverage fraction, at any level above 1^3.

A marcher can use these bits to *skip*. The moment it tries to *stop* on a coarse block it has
nothing to shade with -- `sample_lod` hands back `voxel_data` sampled at the exact point, which in a
non-uniform block is very often air. sec 3 is what that costs.

---

## 3. Distance LOD: what exists, and why terminating early makes it slower

### 3.1 The engine already has a cone-LOD termination -- in the depth prepass only

`trace.glsl:115-121`:

```glsl
#if TraceDepthPrepassComputeShader
    hit_surface = lod < clamp(sqrt(t_curr * info.angular_coverage * VOXEL_SCL), 1, 7);
#else
    hit_surface = lod == 0;                       // the main trace is exact
#endif
```

`angular_coverage` is `16.0 / height * clip_to_view[1][1]` (`trace_primary.comp.glsl:27`) -- a
per-pixel solid-angle term, so the threshold grows with distance exactly like a cone trace. The
prepass runs at half resolution (`PREPASS_SCL` 2) and its output is used only as a *starting
distance* for the full-resolution trace, so a wrong early hit costs a few wasted steps and nothing
else. **This is the only distance-dependent LOD in the marcher, and it never touches the image.**

### 3.2 Extending it to the main trace makes the frame slower

Voxl test scene, `CHUNKS_PER_AXIS` 16, default spawn, **settings pinned**, one 45 s run per point,
median of settled frames, all uncontended:

| `VOXL_EXP_CONE_K_E4` | K | frame | fps | vs exact | frames |
|---|---|---|---|---|---|
| 0 | exact trace | **10.911 ms** | 91.7 | -- | 3042 |
| 167 | 0.0167 -- the prepass's own value | 10.678 ms | 93.6 | -2% (noise) | 3109 |
| 500 | 0.05 | **12.660 ms** | 79.0 | **+16% slower** | 2610 |
| 1500 | 0.15 | **13.143 ms** | 76.1 | **+20% slower** | 2501 |
| 5000 | 0.5 | 11.437 ms | 87.4 | +5% slower | 2886 |

Three readings.

**The distance LOD the engine ships is tuned for a world much larger than the one it has.** Solving
`sqrt(t*K*16) = L` for the distance at which threshold level L is reached: at the prepass's own
K = 0.0167 the threshold reaches level 3 only at 34 m and level 4 at 60 m -- and the world is 64 m
across. That row changes nothing because it never gets to act.

**Early termination is a net loss, and sec 4.1 says why.** At K = 0.05 the threshold reaches level 2
at 5 m and level 7 at 61 m, and the frame gets 16% *worse*. `docs/images/world-scale/02-cone-lod-k0.05-phantom-slabs.png`, viewed: near grass
unchanged, and the sky full of **large flat grey and black slabs** at 1-4 m granularity hanging
above and beside the hill, with the hill's silhouette eaten into. Those slabs are pixels that were
sky -- nearly free -- and are now surfaces that pay for a shadow ray, a diffuse ray, a reflection ray
and denoising. The marching saved is much less than the shading added.

**The recovery at K = 0.5 is consistent with that.** `docs/images/world-scale/03-cone-lod-k0.5-black-sky.png` shows the **sky entirely
black**: the cone LOD lives inside `voxel_trace()` and so applies to every caller -- the sun-shadow
ray at `trace_secondary.comp.glsl:47`, and the diffuse, reflection and ircache rays too -- so every
ambient ray terminates on a phantom blocker within a few metres. A scene with no sky light and no
long rays is cheap again, for the worst possible reason.

**What this means for sec 6.** It is not an argument against distance LOD. It is a precise statement
of the requirement: **a coarse hit has to be a real surface with real shading data.** Replacing an
expensive fine hit with a cheap correct coarse hit is a win; manufacturing a hit where there was
sky is a loss. That is the difference between terminating on occupancy bits and having a filtered
far field.

---

## 4. What the cost actually does as view distance grows

### 4.1 Near geometry costs more than distance

Voxl test scene, `CHUNKS_PER_AXIS` 16, camera fixed at the default spawn, sweeping pitch only
(1.571 is level, lower looks down). **Settings pinned**, one 40-45 s run per point, all
uncontended:

| pitch | looking at | frame | fps | frames |
|---|---|---|---|---|
| 3.05 | nearly straight up -- all sky | **5.628 ms** | 177.7 | 4851 |
| 1.571 | level, at the horizon | 8.624 ms | 115.9 | 3223 |
| 1.0964 | the default, 27 deg down | 10.900 ms | 91.7 | 3016 |
| 0.15 | nearly straight down -- grass at your feet | **11.590 ms** | 86.3 | 2429 |

Grass one metre away is **2.06x** the cost of sky. Two reasons, both load-bearing for everything
above:

1. **A sky pixel does almost no work.** `trace_secondary.comp.glsl:46` gates the sun-shadow ray on
   `depth != 0.0`, and the ReSTIR diffuse, ReSTIR reflection and denoiser passes all skip invalid
   pixels. A sky pixel costs one primary ray. A surface pixel costs a primary ray, a shadow ray, a
   diffuse ray, a reflection ray and the whole denoising stack.
2. **Grass is the worst possible content for this marcher.** A thin, highly non-uniform 3D field at
   6.25 cm makes `sample_lod` return 1 or 2 at nearly every step, so the ray crawls in 6.25-12.5 cm
   increments -- with a full palette decode per step (sec 2.2) -- through exactly the region where the
   pyramid gives no help.

**Consequence for the target: distant mountains are not automatically the expensive half. Near
grass is.** A mountain 500 m away occupies few pixels and each shades once.

This also invalidates an obvious control, which I tried and am discarding: pointing the camera at
the sky to isolate "pure empty-space marching" does not work, because an all-sky frame skips the
shadow, GI and reflection traces and measures the fixed pipeline instead.

### 4.2 The controlled view-distance curve

`CHUNKS_PER_AXIS` 32 -- a fully generated **128 m** world -- camera fixed, **settings pinned**,
sweeping only how far the marcher may travel. The world and camera do not move; only the distance
rays may see. Clipped rays report a genuine miss and shade as sky (sec 0.2). One 50 s run per point.

| ray may travel | frame | fps | frames | added over previous row | per added metre |
|---|---|---|---|---|---|
| 4 m | **7.632 ms** | 131.0 | 4380 | -- | -- |
| 8 m | 8.076 ms | 123.8 | 4185 | +0.444 ms | 0.111 ms/m |
| 16 m | 8.762 ms | 114.1 | 3865 | +0.686 ms | 0.086 ms/m |
| 32 m | 10.440 ms | 95.8 | 3258 | +1.678 ms | 0.105 ms/m |
| 64 m | 14.461 ms | 69.2 | 2364 | +4.021 ms | 0.126 ms/m |
| unclipped (box wall, <=111 m) | **16.870 ms** | 59.3 | 2023 | +2.409 ms | 0.051 ms/m |

**Cost is linear in view distance, at roughly 0.11 ms per metre**, and it is remarkably steady:
the marginal cost of the fourth metre and the marginal cost of the sixtieth are within 20% of each
other. The last row falls off only because the box runs out -- beyond 64 m most rays have already
left the world.

Three things follow.

**This is the number that rules out a full-detail distant view.** Extrapolating 0.11 ms/m -- which
is only legitimate as an order-of-magnitude argument, because the terrain eventually saturates the
frame -- a 500 m view distance at 16 voxels/m would cost roughly 7 + 55 = **60 ms per frame**. The
target is 4.17 ms. This is the quantitative case for sec 6: the far field cannot be made of 6.25 cm
voxels, not because of memory alone but because the per-metre cost of marching and shading them is
about a tenth of a millisecond.

**But 32 m of full detail is affordable.** Going from 4 m to 32 m of view distance costs 2.8 ms  -- 
that is the "detail close to the player" half of the target, and at 10.44 ms it is already close to
today's whole frame.

**I cannot split this into marching versus shading.** Both grow together: a longer ray marches
further *and* is more likely to land on a surface that then pays for a shadow ray, a diffuse ray, a
reflection ray and denoising. sec 8.2 is the change that would separate them, and it is small.

A caution on reading this sweep's *images*: `docs/images/world-scale/10-clip-4m-particles-persist.png` (4 m) and `docs/images/world-scale/08-clip-16m-particles-persist.png` (16 m)
look almost identical and neither looks like a 4 m or 16 m view, because grass and flowers are
**rasterised particles, not marched voxels** (sec 4.3) and the knob does not touch them. The hill
stays visible as a ghostly cloud of floating green specks over blue sky long after its solid
surface is clipped away. The timings measure what is wanted -- the cost of letting the marcher see
further -- but the screenshots cannot be read as "here is what you give up at each distance".

### 4.3 The finest detail is not voxels at all

Grass, flowers, trees and fire are a **separate rasterised particle system**
(`src/voxels/particles/`), spawned by the brush when a chunk is generated, held in static
allocators and drawn as cubes and splats -- never marched. `grass/sim.comp.glsl` is dispatched over
**all** `MAX_GRASS_BLADES` = 2^2^0 = 1,048,576 slots every frame regardless of how many are alive
(`grass.inl:132`, a plain `dispatch`, not indirect), and each live blade emits 2-4 vertices to a
cube pass, a shadow pass and a splat pass.

That cap is worth a moment: a fully grassed 64 m world at 16 voxels/m has
`(64 x 16)^2 = 1024^2 = 1,048,576` surface voxels -- **exactly `MAX_GRASS_BLADES`**. By design or by
luck, the grass budget is precisely one 64 m world, and it is already saturated. A 128 m world
would need four times it.

Good news for sec 6: the expensive fine detail already has its own representation and is already
bounded. Caveat: it has no distance LOD of its own, its per-frame dispatch cost is fixed, and any
far field must arrange for it not to be spawned out there.

---

## 5. Memory

### 5.1 The chunk table is what kills a big world

`8216 x CHUNKS_PER_AXIS^3`, resident, empty or not:

| CPA | chunks | table | world edge | view radius |
|---|---|---|---|---|
| 8 | 512 | 4.0 MB | 32 m | 16 m |
| 16 | 4096 | **32.1 MB** | 64 m | 32 m |
| 32 | 32768 | 256.8 MB | 128 m | 64 m |
| 64 | 262144 | **2054 MB** | 256 m | 128 m |
| 128 | 2097152 | 16.4 GB | 512 m | 256 m |
| 256 | 16777216 | 131.4 GB | 1024 m | 512 m |

On a 6144 MiB card, with the demo world already at 1263 MB of heap plus 257 MB of table at CPA 32,
**CPA 64 is unreachable and CPA 128 is off by a factor of three on the index alone.** A
full-detail 1 km view radius at 16 voxels/m is off by more than four orders of magnitude. No amount
of compression fixes it: the table is the *index*, paid per chunk whether the chunk holds anything
or not.

### 5.2 The lever

```
chunk edge (m) = CHUNK_SIZE x voxel_size = 64 x voxel_size
world edge (m) = CHUNKS_PER_AXIS x 64 x voxel_size
table (bytes)  = 8216 x CHUNKS_PER_AXIS^3          <-- independent of voxel_size
max march step = one chunk = 64 x voxel_size       <-- scales with voxel_size
```

**The resident table cost does not depend on voxel size at all.** Making voxels 16x bigger makes
the same table cover 16x the distance *and* makes the largest march step 16x longer. Both of the
things that scale badly scale the right way at once, off one constant.

### 5.3 Heap, measured

Per-process, from the engine's own `--bench-csv`, settled, demo world, camera at
(-182.99, -109.98, -42). **These figures are unaffected by the sec 0.3 settings problem** -- heap use
depends on world content, not on render settings:

| CPA | world | heap in use | heap capacity |
|---|---|---|---|
| 8 | 32 m | 1.2 MB | 553.7 MB |
| 16 | 64 m | 107.5 MB | 830.5 MB |
| 32 | 128 m | 1262.8 MB | 1660.9 MB |

Capacity is the allocator's high-water mark and never shrinks (`allocator.inl`), so it overstates
what the content needs -- the Voxl island scene at CPA 16 uses 13 MB against the same 830 MB
capacity. The CPA 8 row is really "the 32 m box missed the terrain", so this is two useful points,
not three. **1263 MB of a 6 GB card for a 128 m world is the practical wall**, and it arrives
before the table does.

---

## 6. A concrete near/far architecture

### 6.1 The proposal

Four nested volumes sharing the existing `VoxelLeafChunk` layout, the existing palette compression,
the existing uniformity pyramid and the existing DDA. The only thing that differs between them is
`LOG2_VOXEL_SIZE`.

| level | `LOG2_VOXEL_SIZE` | voxel | chunk | CPA | world edge | view radius | table |
|---|---|---|---|---|---|---|---|
| L0 near | -4 | 6.25 cm | 4 m | 16 | 64 m | 32 m | 32.1 MB |
| L1 | -2 | 25 cm | 16 m | 16 | 256 m | 128 m | 32.1 MB |
| L2 | 0 | 1 m | 64 m | 16 | 1024 m | 512 m | 32.1 MB |
| L3 far | +2 | 4 m | 256 m | 16 | 4096 m | 2048 m | 32.1 MB |
| | | | | | | **2 km** | **128.4 MB** |

### 6.2 Why those numbers

Each level's radius should be the distance at which its voxels shrink to about one pixel, and that
falls out almost exactly.

At 74 deg **vertical** FOV (`voxel_app.cpp:50`; `view_to_clip.y.y = 1/tan(fov/2)` at `player.cpp:369`
confirms it is vertical) and 720 render rows, the view-space vertical extent at unit depth is
`2*tan(37 deg)` = 1.5071, so one pixel at the image centre subtends `1.5071 / 720` = **2.093 mrad**. A
voxel of size `s` covers one pixel at `d = s / 0.002093` = **478*s**.

A level's view radius at CPA 16 is `16 x 64 x s / 2` = **512*s**.

`512*s` against `478*s` -- **each level's outer boundary sits within 7% of the distance at which its
own voxels reach one pixel.** That is a coincidence of `CHUNK_SIZE` 64, CPA 16 and 74 deg at 720p, but
a convenient one: CPA 16 per level and a 4x voxel-size step per level are both the right choices.

Two caveats. A higher internal resolution shrinks the pixel and makes every level too coarse at its
outer edge (at 1080p internal, `d = 717*s`, ~40% too coarse); the fix is CPA 32 on the outer levels
at 8x the table, or accepting blur where FSR is upscaling anyway. Conversely the GI passes shade at
half resolution (`SHADING_SCL` 2) and would tolerate one level coarser than the primary ray -- an
optimisation available later, not a problem now.

### 6.3 What it does to the march

Crossing a level in empty space costs at most `CHUNKS_PER_AXIS` = 16 steps, because the largest
step is one chunk. **Four levels is 64 steps to cross 2 km**, against a `MAX_STEPS` budget of 512.
Covering the same 2 km with today's 4 m chunks needs 500 steps of pure vacuum-crossing before
hitting anything, which does not fit the budget at all.

### 6.4 What it does to chunk generation

The cost nobody counts. `MAX_CHUNK_UPDATES_PER_FRAME` is 128, and `PerChunkComputeShader`
invalidates the trailing face of the window every time the player crosses a chunk boundary  -- 
`CHUNKS_PER_AXIS^2` chunks per crossing. At CPA 16 that is 256 chunks per 4 m travelled; at CPA 32,
1024 per 4 m. `BASELINE.md` measures the consequence: the demo world at CPA 32 costs 21.11 ms while
walking against 17.97 ms settled -- **3.1 ms of the frame is chunk generation** when moving.

Coarse levels regenerate far less often because their chunks are bigger *in metres*: L3 crosses a
chunk boundary every 256 m instead of every 4 m. Generation work per metre travelled drops by the
ratio of chunk sizes -- 64x between L0 and L3. The same lever again.

### 6.5 The genuinely new work: filtered voxels

Everything above reuses existing code. This does not, and sec 3.2 is the evidence for why it is
mandatory rather than optional: **a coarse hit that is not a real surface makes the frame slower,
not faster.** A 1 m voxel in L2 stands for 16^3 = 4096 voxels of L0 and needs a colour, a normal, a
roughness and a material that genuinely represent them.

- **Generated terrain: easy.** `brushgen_world_terrain()` (`brushes.glsl:305`) is analytic noise
  evaluated at `voxel_pos`; evaluating it at 1 m spacing already yields a coarser sampling of the
  same height field. What must be added is judgement about content -- `try_spawn_grass` and
  `try_spawn_tree` must not fire in coarse levels, and material choice needs the dominant material
  of the block rather than a point sample, or distant hillsides will shimmer between rock and grass
  as the camera moves.
- **Player edits: hard, and I would not solve it in v1.** Propagating a 6.25 cm edit up three levels
  needs a downsample pass per level per edited region. Since edits happen only in L0 and L0 covers
  32 m, the honest v1 position is that edits are not visible beyond 32 m -- fine for a distant
  mountain, not fine for a player-built tower.

### 6.6 Expected cost, as a range rather than a number

- **Measured:** sec 4.2's curve, 0.11 ms per metre of view distance at 6.25 cm voxels. If a coarse
  level's per-metre cost scaled with its voxel size -- 4x coarser voxels, 4x longer steps, 4x fewer
  surface voxels per metre -- L1 would cost ~0.028 ms/m, L2 ~0.007, L3 ~0.0017, and the four-level
  2 km stack would come to roughly `32x0.11 + 96x0.028 + 384x0.007 + 1536x0.0017` = **11.9 ms**.
  **Treat that as an illustration of the shape, not a prediction.** It assumes the per-metre cost
  falls exactly as the voxel size rises, which is the thing that has not been measured, and it
  ignores that coarse levels put more *surface* on screen (see the next bullet). It is included
  because it shows the right order of magnitude comes out of the level structure rather than out
  of optimism, and because it makes the sensitivity explicit: if the far levels only get half the
  scaling assumed here, the stack roughly doubles.
- **Measured, and negative:** sec 3.2. Do not budget a saving from distance LOD until the far field is
  correct; today the same mechanism costs 16-20%.
- **Unmeasured, and the thing I would worry about:** L1-L3 turn pixels that are currently sky -- and
  therefore nearly free (sec 4.1) -- into surface pixels that pay for a shadow ray, a diffuse ray, a
  reflection ray and denoising. **There is no far field to measure this on.** sec 4.2 is the closest
  proxy and it is a proxy.
- **Heap:** each level costs roughly the same, because extent and voxel size scale together, so
  every level has the same `(CPA x 64 / 8)^2 = 128^2` surface palette regions per layer. Four levels
  should cost ~4x one level's surface heap. Measured single-level surface heap is 107.5 MB (CPA 16
  demo world, camera above terrain), so **~0.4-0.6 GB of heap for four levels** is my estimate.
  **That is a scaling argument from one scene, not a measurement.**

---

## 7. What it would take to build

`voxel_trace()` is a single choke point -- one implementation selected at compile time, called from
**8 sites** (`rt.glsl`, `ircache_trace_common`, `trace_accessibility`, `diffuse_trace_common`,
`reflection_trace_common`, `trace_primary`, `trace_secondary`, `sim_particle`) -- and
`min_impl/trace.glsl` proves the abstraction is real by swapping the whole marcher for an SDF.
That is the best single fact about this codebase for this change.

1. **Make the voxel size a per-volume value instead of a global `#define`.** The bulk of the work,
   mechanical rather than difficult. `VOXEL_SIZE` appears 82 times, `CHUNKS_PER_AXIS` 41,
   `CHUNK_SIZE` 34, `VOXEL_SCL` 17, `LOG2_VOXEL_SIZE` 17, `CHUNK_WORLDSPACE_SIZE` 8, across 19
   files. **The indexing arithmetic is already correctly parameterised** -- every chunk-index shift
   is written `>> (6 + LOG2_VOXEL_SIZE)`, right for positive `LOG2_VOXEL_SIZE` as well as negative
 -- so this is threading a parameter, not re-deriving the maths. The 7 particle files can stay
   hard-wired to L0.
2. **Extra buffer sets.** `VOXELS_USE_BUFFERS` (`voxels.inl:96`) is used by 20 task headers; each
   level needs its own `voxel_chunks` and allocator. `voxel_globals` can be shared.
3. **A multi-level march in `voxel_trace()`.** March L0 to its boundary; if no hit, continue in L1
   from that point, and so on. One function; the existing box-intersect and DDA are reusable
   verbatim. The subtleties are (a) coarse levels must be **hollow** where a finer level covers
   them, or the ray hits the coarse version of terrain it should be seeing in detail, and (b) the
   hand-off needs the same epsilon care the current code shows at `trace.glsl:130`.
4. **Chunk generation per level.** `PerChunkComputeShader`, `ChunkEditComputeShader` and the two
   `ChunkOpt` stages already work at any voxel size; they need to run per level with their own
   update budget, and coarse levels should get a small one since they regenerate rarely (sec 6.4).
5. **Filtered generation** (sec 6.5) -- the only genuinely new algorithm, and only for the brush.
6. **Raise `MAX_STEPS` and verify it.** See sec 8.1.
7. **Land `VOXL_DATA_DIR`** (sec 0.3). Not part of this feature, but nothing here can be measured
   reliably until concurrent builds stop sharing one mutable settings file.

**Not needed, and worth saying: the GI does not have to change.** The irradiance cache is already a
12-level cascade with cell diameter `1/8 x 2^cascade` metres (`ircache_grid.glsl:78`,
`settings.inl:80-82`) -- 32 cells across at 256 m per cell in the outermost cascade, **8192 m of
coverage**. The probe structure already spans forty times the world the voxels can represent.

See also `docs/design/PRIOR_ART_PERFORMANCE.md`, written in parallel, for the frame-rate side of
the target; this document is only about world extent.

---

## 8. What I could not determine

1. **Whether `MAX_STEPS` = 512 is already binding.** I built the experiment (`wsx\steps.ps1` flips
   it in `math.glsl`, runtime-compiled, no rebuild) and spent the remaining GPU time re-measuring
   sec 3 and sec 4 after discovering the settings problem. The arithmetic says it should bind: the
   minimum step is 6.25 cm, so **512 steps carries a ray at most 32 m** if it stays in fine
   geometry, and the CPA 16 box diagonal is already 111 m. A ray that exhausts the cap is reported
   as a *miss* and shaded as sky, so the symptom is holes in distant geometry, not a slowdown.
   **Check this before trusting any large-world screenshot.**
2. **Actual step counts.** `VoxelTraceResult.step_n` is computed and thrown away  -- 
   `trace_primary.comp.glsl:93` assigns it to a variable nothing reads, and the prepass writes only
   depth into its image although the consumer at :84 reads a `.y` channel for steps that is never
   written. Wiring `step_n` into the debug image would turn most of sec 4 from inference into
   measurement. **This is the highest-value next step in this area** and it is small.
3. **Whether the world-size comparison (CPA 8/16/32 at a fixed camera) is monotonic.** My run of it
   predates the settings pinning and I discarded its timings (sec 0.3). Its heap figures survive and
   are in sec 5.3. Redoing it costs three rebuilds and about fifteen minutes.
4. **What a far field actually costs to shade** (sec 6.6). Not measurable without building one.
5. **Whether heap scales as surface area across levels.** sec 5.3 is two usable points on one scene,
   and the sec 6.6 heap estimate rests on it.
6. **Anything at 1920x1080, or with FSR in the loop.** Everything here is 1280x720 native, and
   sec 6.2 makes the level radii explicitly resolution-dependent.
7. **Whole-device VRAM.** Not usable with several engine instances resident; the heap figures are
   the engine's own per-process numbers instead.
8. **`CHUNKS_PER_AXIS` 64.** sec 5.1 rules it out arithmetically (2054 MB of table before any content)
   and I did not spend a build proving the card falls over.
