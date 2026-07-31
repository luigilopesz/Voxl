# gvox_engine — Adoption Plan

**Status:** GO, WITH CONDITIONS
**Date:** 2026-07-30
**Subject:** Adopting `GabeRundlett/gvox_engine` as the basis for a 16-voxel/metre, path-traced Voxl
**Scope:** This document is a plan, not a debate. The decision to pivot has been made.

---

## 1. Verdict

**Both hard gates pass.** The licence is MIT and you can ship a commercial game on it. The engine builds on this machine with MSVC 19.44 and runs on your RTX 3050 6 GB at **42.5 fps** (23.52 ms) at 1280×720 over dense terrain, using **3.4–4.6 GB of your 6.1 GB**, with a clean exit and no leak.

**The condition is memory, and it is specific.** The voxel heap allocator grows by 1.5× while holding the old and new buffers simultaneously, with no cap and no out-of-memory handling. At the reference world's heap size the next growth step would transiently need **7.27 GB for the heap alone** — that does not fit in 6 GB and will produce a device-lost crash, not a stutter. This is a ~30-line fix in `src/utilities/allocator.inl` and it must be Stage 2 of the plan, not an afterthought.

**One correction to the premise, and it is good news.** The reference screenshot's `GPU Heap 1376256 pages (7986.65 MB)` is a misread. That page count prints as **2906.65 MB**, not 7986.65 MB. I verified this two ways: the page size is 2112 bytes (`VOXEL_MALLOC_PAGE_SIZE_BYTES`, computed below), so 1376256 × 2112 = 2,906,652,672 bytes = 2906.65 MB at the `1'000'000.0` divisor the code actually uses; and my own run on your 3050 reached the *identical* page count and printed `2906.65 MB` on screen. The reference world was therefore using ~2.9 GB of heap on an 8 GB card, not 8 GB. That is why it fits on yours.

Everything below is measured on your hardware or quoted from source. Nothing is inferred from the screenshot.

---

## 2. Licence position

### 2.1 Can you ship a game?

**Yes — commercial or non-commercial, closed-source.** `gvox_engine/LICENSE` is the unmodified MIT template, "Copyright (c) 2023 Gabe Rundlett". MIT grants the right to "use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies" with no source-disclosure obligation on your own code.

Every dependency is permissive: Daxa (MIT), gvox (MIT), kajiya (MIT, Embark Studios), FFX (MIT, AMD), assimp (BSD-3), glfw/minizip (Zlib), imgui/glm/fmt/nlohmann-json/stb (MIT).

### 2.2 The three things you must fix before shipping

The repository's blanket MIT is factually wrong about at least three things it contains. None is a blocker; all three are an afternoon's work.

**(a) `assets/STBN.zip` is NVIDIA's, shipped with no licence and no attribution.** I opened it: 64 entries named `STBN/stbn_vec2_2Dx1D_128x128x64_0.png` … `_63.png`, which is verbatim NVIDIA's naming from `NVIDIA-RTX/STBN`. I grepped the whole tree — there is no NVIDIA notice anywhere outside unrelated vcpkg port files. NVIDIA's licence is dual: a **Non-Commercial Use License** whose §3.3 restricts use to "research or evaluation purposes only", and a **Commercial Use License** (NVIDIA RTX SDKs) which permits shipping but requires the notice *"This software contains source code provided by NVIDIA Corporation."*, requires your app to have material additional functionality, and excludes avionics/military/medical use.

> This is a licence-compliance defect in gvox_engine that you would inherit silently. Fix it: either elect the commercial branch and add the notice, or regenerate the blue-noise textures yourself (NVIDIA publishes the generator). Regenerating is the clean answer.

**(b) FreeImage is GPL-2.0 OR GPL-3.0 OR FIPL — elect FIPL, or better, delete it.** It is a required linked dependency (`CMakeLists.txt:42,71`) used for *exactly three things*: FreeImage init/deinit (`src/main.cpp:33,38`), the window icon (`src/application/window.hpp:73-90`), and decoding the STBN PNGs (`src/utilities/gpu_context.cpp:182-187`). `stb` is **already** a declared dependency. Swapping in `stb_image` deletes the entire GPL-adjacent subtree — including **libraw**, which is LGPL-2.1/CDDL and which the build ships as a DLL (`install(FILES ".../raw.dll")`, `CMakeLists.txt:114`), obliging you to permit relinking and supply source. It also removes `jxrlib`, the port that is the single most-reported build failure (issue #20). Do this on day one: it is a licence win, a build win, and a size win at once.

**(c) `packaging/infos/license.txt` says, in full, "Gabe Rundlett / All Rights Reserved"** — and it is wired into the installer via `set(CPACK_RESOURCE_FILE_LICENSE ...)` at `CMakeLists.txt:126`. Any installer you build today presents an All-Rights-Reserved EULA. This was reported as issue #12 in 2024 and closed without changing the file. Replace it.

### 2.3 Attribution you will owe

MIT/BSD/Zlib notices for: Gabe Rundlett (gvox_engine, gvox), Ipotrick (Daxa), **Embark Studios (kajiya)**, **AMD (FFX denoiser, FSR2)**, Jasper-Bekkers (blue-noise-sampler), assimp, glfw, imgui, glm, fmt, nlohmann/json, platform_folders, nativefiledialog, minizip, stb. Plus Apache-2.0 NOTICE for Roboto Mono and an OFL notice for Inter Tight. Voxl already has `docs/THIRD_PARTY_NOTICES.md`; this becomes a much longer file.

**Flagging one thing as an honesty matter, not a legal one:** `src/renderer/kajiya/` is **14,547 lines across 99 files, out of 31,024 lines in `src/` — 47% of the codebase**. Its own README says "the essential parts are nearly just direct ports" of Embark Studios' kajiya. The path-traced GI you are adopting is Embark's algorithm, ported by Gabe. It is MIT and you may ship it, but "I wrote a path tracer" would not be accurate, and you will be maintaining 14.5k lines of someone else's renderer alone.

---

## 3. Build and run status on this machine

### 3.1 It builds and it runs — verified twice

A working build exists at `C:\gvx\gvox_engine\.out\cl-x86_64-windows-msvc\Release\gvox_engine.exe`. I launched it again today, after the build tree had been pruned, and sampled VRAM:

```
### baseline VRAM:
NVIDIA GeForce RTX 3050 6GB Laptop GPU, 6144 MiB, 283 MiB
### started pid=1956
### window up after 1s
t+3s   gpu_total_used=2601 MiB
t+6s   gpu_total_used=3391 MiB
t+21s  gpu_total_used=3422 MiB
### hasExited=True
### VRAM after exit: 284 MiB
```

Clean startup, clean shutdown, all memory released. From the in-app overlay during a 30-second moving soak:

| Measurement | Value |
|---|---|
| Full frame-time | **avg 23.52 ms (42.51 fps)**, min 18.47, max 24.56 |
| CPU-only frame-time | **avg 0.98 ms (1021 fps)**, min 0.84, max 1.30 |
| Window size | 1280 × 720 |
| GPU | NVIDIA GeForce RTX 3050 6GB Laptop GPU (discrete correctly selected over the Intel iGPU) |
| GPU Heap | 1376256 pages (**2906.65 MB**) |
| GPU Heap Usage | 1171.35 MB |
| Peak VRAM, moving | **4589 MiB of 6144 MiB (75%)** |
| Peak VRAM, stationary at spawn | 3422 MiB of 6144 MiB (56%) |
| Startup | 2.87 s cold (compiles 127 GLSL shaders at runtime), ~1 s warm |

**Read the frame rate carefully: 42.5 fps is at 1280×720.** At 1920×1080 that is 2.25× the pixels. The engine already shades at half resolution (`PREPASS_SCL 2`, `SHADING_SCL 2` in `src/application/settings.inl:59-60`) and ships FSR 2.2, so the realistic 1080p configuration is 720p-internal upscaled — which is roughly what you measured. **Do not plan on native 1080p.**

### 3.2 The four fixes required, quoted

None is large. Three are genuine source bugs.

1. **Vulkan SDK absent.** `find_package(Vulkan)` fails: `Could NOT find Vulkan (missing: Vulkan_LIBRARY Vulkan_INCLUDE_DIR)`. `VULKAN_SDK` is unset and `C:\VulkanSDK` does not exist. The working build uses a synthesised minimal SDK at `C:\gvx\vksdk` (Vulkan-Headers v1.3.260 plus an import library generated from your driver's own `vulkan-1.dll`). **You should install the real LunarG SDK** (~200 MB, lunarg.com). The *runtime* does not need it — your driver loader 1.4.341.0 suffices.

2. **`cmake/toolchains/cl-x86_64-windows-msvc.cmake:4-7`** reads `${MSVC_ENV_Path}`, but `vcvars.cmake:89` only special-cases the exact name `Path`; your `cmd` exports `PATH` uppercase, so the cache gets `MSVC_ENV_PATH` and the hint list is empty:
   `Could not find CMAKE_C_COMPILER using the following names: cl`
   **Workaround:** run the configure from inside a `vcvars64.bat` environment so `cl` is already on PATH.

3. **`deps/gvox` does not compile with MSVC 19.44.** `src/adapters/parse/magicavoxel.cpp:15` and `src/adapters/serialize/gvox_palette.cpp:21` use `using namespace std::chrono_literals;` with no `#include <chrono>`:
   `error C2039: 'chrono_literals': is not a member of 'std'`
   Modern MSVC's STL no longer pulls `<chrono>` in transitively. Two added lines.

4. **`src/utilities/debug.cpp:32`** uses `std::cout << str << std::endl` with no `#include <iostream>`:
   `error C2039: 'cout': is not a member of 'std'`
   One added line.

The complete patch is three lines:

```
 deps/gvox               | 0
 src/utilities/debug.cpp | 1 +
 2 files changed, 1 insertion(+)
--- submodule deps/gvox ---
 src/adapters/parse/magicavoxel.cpp      | 1 +
 src/adapters/serialize/gvox_palette.cpp | 1 +
 2 files changed, 2 insertions(+)
```

### 3.3 Two environment traps that are not project bugs

- **MAX_PATH.** `cmake/vcpkg.cmake:12` clones vcpkg *inside* the source tree. From a deep path the result exceeds 260 chars and git aborts with `Filename too long`. `LongPathsEnabled` is `0` on your system, and `subst` does **not** help — the kernel resolves it back. **Build from a short path.** `C:\voxl2` is ideal; `C:\Users\luigi\projects\Voxl2` (28 chars) is fine. (Enabling long paths is a registry change under `HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem` — your call, not something to do casually.)
- **vcpkg parallelism kills the compiler.** `ninja -j21` combined with vcpkg's internal `/MP4` means up to 84 concurrent `cl.exe` on 16 GB of RAM. Result: `FAILED: [code=4294967295]` on spirv-tools with **no diagnostic at all**, twice, while the same translation unit compiles fine alone. **Set `VCPKG_MAX_CONCURRENCY=2`** — 6 was still not enough.

### 3.4 Cost of a first build

**~25–30 minutes** knowing the fixes (36 minutes including diagnosis of all six failed attempts). Almost all of it is vcpkg building 46 packages from a 2023-era baseline; the engine's own 16 `.cpp` files link in **3 seconds**. **Peak disk 14.2 GB**, retained 3.7 GB after pruning `buildtrees`/`packages`/`downloads`. Your C: drive was at 96% during the build — free space up first.

### 3.5 The memory ceiling — the actual condition on this GO

Fixed costs, computed from the structs:

| Buffer | Size | Source |
|---|---|---|
| `voxel_chunks` (32768 × 8216 B) | **269.2 MB** | always resident, even for an empty world |
| `chunk_update_heap` | 268.4 MB | host-visible |
| `temp_voxel_chunks` | 134.9 MB | transient per frame |
| Grass (4.19 M blades) | ~268 MB | `grass.inl:5` |
| ircache (65536 entries) | ~50–60 MB | `settings.inl:81-86` |
| Voxel heap | 276.8 MB initial → **2906.65 MB observed** | grows, never shrinks |

The page size is worth showing because every heap number depends on it — `src/voxels/impl/voxel_malloc.inl:41-56`:

```c
#define VOXEL_MALLOC_U32S_PER_PAGE_BITFIELD_BIT (512 / 32 + 2 + 1 + PALETTE_ACCELERATION_STRUCTURE_SIZE_U32S)
#define VOXEL_MALLOC_MAX_ALLOCATIONS_IN_PAGE_BITFIELD 24
#define VOXEL_MALLOC_PAGE_SIZE_BYTES (VOXEL_MALLOC_PAGE_SIZE_U32S * 4)
```

= (16 + 2 + 1 + 3) × 4 × 24 = **2112 bytes**.

**The failure mode.** `check_for_realloc` in `src/utilities/allocator.inl:264-300` does:

```cpp
current_element_count = std::max(next_element_count * 3 / 2, max_count_after_cpu_catch_up);
auto new_element_buffer = device.create_buffer({ .size = ELEM_SIZE_BYTES * current_element_count, ... });
...
task_old_element_buffer.swap_buffers(task_element_buffer);   // old buffer still alive
```

There is **no VRAM cap, no OOM check, and no shrink path**. From the observed 1,376,256 pages:

```
current heap  : 1376256 pages = 2906.65 MB
next grow step: 2064384 pages = 4359.98 MB
TRANSIENT peak (old + new held together): 7266.63 MB
RTX 3050 6GB usable: 6442.45 MB  ->  DOES NOT FIT
```

You have headroom for the world you have seen, and **exactly zero headroom for one more growth step**. Heavy editing or a denser generator will hit this. Mitigations, in order of leverage:

- **Cap the allocator** (Stage 2 below). Fail the growth, log it, degrade — do not let `create_buffer` fault.
- `CHUNKS_PER_AXIS 32 → 16` (`voxel_malloc.inl:7`): 128 m → 64 m world, chunk table 269 → 33.6 MB, heap ~8× smaller.
- `MAX_GRASS_BLADES 1<<22 → 1<<20` (`grass.inl:5`): ~200 MB.
- "Render Res Scale" slider and FSR 2.2 are already exposed in the UI.
- `LOG2_VOXEL_SIZE -4 → -3` gives 8 voxels/m and ~8× less heap — **but that is the look you are pivoting for. Don't.**

### 3.6 Project health — the standing risk

Last commit **2024-11-19** (`da8e4923`, "Fix missing include from Daxa"), ~20 months ago; the last *feature* work was 2024-03-29. Single author (474 of 483 commits). Default branch is `compute-rt`, not `master`. Of 7 open issues, four are build failures — the newest, **#21 (2026-02-26) "Does not compile"**, has zero comments and no maintainer reply. The newest release (0.1.13) is from 2023-04-27 and predates the kajiya renderer entirely, so **downloading a release will not show you the screenshot**. 23 forks, all with 0 stars, all plain mirrors — I found no evidence anyone but the author has ever built a project on this.

The author is active, just elsewhere: he pushed to `Sunset-Flock/Timberdoodle` on 2026-07-12. Do not expect him to fix your build. Daxa itself is healthy (pushed 2026-07-04), though the pin is **349 commits behind** master.

**Plan on owning this codebase outright.** That is the deal.

---

## 4. What the engine is

### 4.1 In one paragraph

Voxl draws the world by building triangles: it looks at the blocks, merges their faces into as few rectangles as it can, and hands ~879,000 triangles to the GPU to rasterise. gvox_engine never builds triangles for terrain at all. Instead, for **every pixel on screen**, a compute shader walks a ray forwards through the voxel grid until it hits something — like a very fast game of "which cube does this line of sight touch first". Because the renderer is already answering "what does this ray hit", it can cheaply ask more rays: one towards the sun for shadows, and a spray of extra rays that bounce off surfaces to gather light from whatever they land on. That bouncing is what makes red rock tint nearby grass warm — the light genuinely bounces. The result of a handful of random rays is extremely noisy, so most of the renderer's complexity is not tracing rays but *denoising* — reusing results across neighbouring pixels and across previous frames until the noise averages out. Voxels are 1/16 m instead of 1 m, and each stores a colour directly instead of a texture reference, so surfaces read as per-voxel colour with no texture mapping anywhere.

### 4.2 The detail

**API.** Vulkan 1.3 via **Daxa 3.0.2**. Device creation is one line (`src/utilities/gpu_context.cpp:14`) and the implicit-feature mask is empty — so ray tracing is **never even requested**.

**No hardware ray tracing anywhere.** `grep` for `rayQuery|GL_EXT_ray|traceRay|accelerationStructure|Tlas|Blas` across `src/` returns zero live hits (the two `acceleration_structure` references are commented out). There are 57 `.comp.glsl` files and zero `.rgen`/`.rchit`. **Your 3050's weak RT cores are irrelevant** — this is the single most favourable fact about your hardware. It also means Vulkan is not strictly required by the *technique* (see §8).

**Required device features** (`deps/Daxa/src/impl_features.cpp:168-201`) include `bufferDeviceAddress`, `shaderInt64`, full bindless descriptor indexing, `dynamicRendering`, `synchronization2`, `timelineSemaphore`, `subgroupSizeControl`. **An RTX 3050 (Ampere) supports every one.** This gates pre-Turing hardware and most integrated GPUs, not yours. Note `src/renderer/trace_primary.comp.glsl:119-131` does `subgroupBroadcast` on a hardcoded 2×2 quad assuming 32-wide subgroups — fine on NVIDIA, fragile on AMD/Intel.

**Storage.** Not an octree, not a VDB. A flat **wrapping 3-D array of fixed-size chunks**, each palette-compressed per 8³ region, backed by a GPU page allocator, with a 6-level uniformity bitmask pyramid as the traversal accelerator. From `src/voxels/impl/voxel_malloc.inl:6-24`:

```c
#define CHUNK_SIZE 64          // a chunk = 64^3 voxels
#define CHUNKS_PER_AXIS 32
#define LOG2_VOXEL_SIZE (-4)   // -> VOXEL_SCL 16, VOXEL_SIZE 1/16 m
#define PALETTE_REGION_SIZE 8
#define MAX_CHUNK_UPDATES_PER_FRAME 128
```

**That is your 16 voxels/metre, confirmed at runtime** — the overlay in my run read `Player Pos (camera): 0.861` against `Player Pos (voxel): 13.780`, and 0.861 × 16 = 13.78.

A chunk is 4 m; the world is 32 × 4 m = **a fixed 128-metre cube**, 2048³ = 8.59 × 10⁹ voxel slots. `ENABLE_CHUNK_WRAPPING` makes chunk indices wrap modulo 32 around the player, so the volume is a torus that scrolls; chunks leaving the trailing edge are **regenerated from noise**. Nothing outside 128 m exists, and nothing is ever written to disk. Voxl at render distance 20 covers roughly 640 m.

**Voxel format — 32 bits, no textures** (`src/voxels/pack_unpack.glsl:34-49`): 2 bits material type (empty/diffuse/metal/**emissive**), 4 bits roughness, 8 bits octahedral normal, 18 bits colour at 6:6:6. There is **no UV anywhere in the codebase**. 18 bits of colour is the entire surface description.

**Frame structure** (`src/renderer/renderer.cpp:104-198`): sky/atmosphere LUTs → half-res depth prepass → **primary rays, one per pixel, no bounces** → particles rasterised into the same G-buffer → sun shadow ray per pixel → irradiance cache → SSAO → ReSTIR diffuse GI → ReSTIR reflections → composite → TAA or FSR2 → auto-exposure and tonemap.

**Where the screenshot's light comes from.** Two stacked ReSTIR systems. A **world-space irradiance cache** (12 cascades × 32³ cells, base cell 1/8 m, up to 65536 probes storing spherical harmonics) where each probe fires **one** bounce per frame and then samples the cache itself — multi-bounce emerges from the cache feeding itself across frames. On top, **screen-space ReSTIR diffuse** at half res with temporal and spatial reservoir reuse. The warm bounce onto foliage is `rtdgi_tex` applied at `light_gbuffer.comp.glsl:146`. The emissive white object is `gbuffer.glsl:20`:

```glsl
res.emissive = voxel.color * float(voxel.material_type == 3) * (2.0 * voxel.roughness + 0.01);
```

Note the hack: **emission strength is smuggled through the roughness field.** And note this: **there are no analytic or punctual lights at all** — the triangle-light sampling block is commented out. Every light except the sun is an emissive voxel resolved through the GI cache. That is why it looks so good, and also why a newly-placed light takes several frames to propagate.

**Editing is chunk-granular, and this matters for a sandbox.** There is no "write one voxel" path. Every edit runs: elect chunks against the brush AABB (capped at 128/frame) → **re-evaluate all 262,144 voxels in each elected chunk** → normal fixups → rebuild the uniformity pyramid → re-palettise all 512 regions and reallocate their blobs. The good news is there is **no BVH rebuild** — the pyramid is recomputed inline, which is exactly why avoiding hardware RT pays off. The brushes themselves are hardcoded GLSL in `src/voxels/brushes.glsl` (738 lines), not data.

**It is an application, not a library.** `grep -rn "add_library|install(EXPORT|EXPORT_NAME"` returns nothing. One `add_executable`, 17 `.cpp` files, no exported headers, no public API, no plugin ABI. `CPACK_PACKAGE_NAME` is `"GabeVoxelGame"` and the description says it is "documented on Gabe's YouTube channel". Tags read `Episode-9`; branches read `episodes_1_10`. **This is the companion repo to a devlog series.** "Using gvox_engine" does not mean linking a library — it means forking the application and editing it in place. Only 21% of the tree is C++; 79% is GLSL and shader-shared headers.

---

## 5. Inventory — what the pivot actually costs

Voxl today: **35,832 lines** in `src/`, **15,309** in `tests/` across **335 `TEST_CASE`s**, 2,841 in `benchmarks/`.

### 5.1 Survives near-intact — ~5,700 lines

| Subsystem | Lines | Note |
|---|---|---|
| `src/core/` — JobSystem, Log, Time, Profiler, FrameLimiter | 2,169 | gvox_engine's entire thread pool is `src/utilities/thread_pool.hpp`, 90 lines. Your work-stealing pool with three priorities is strictly better. Caveat: a GPU-driven engine has far less CPU work to schedule — your CPU frame time there is **0.98 ms**. |
| `src/audio/` — AudioEngine, SoundBank, SynthSounds | 2,815 | **Strictly additive.** miniaudio is API-agnostic. See §5.3. |
| `src/app/Settings` | 747 | Portable but redundant; gvox has its own ImGui-driven registry. |

### 5.2 Survives with substantial rework — ~8,400 lines

| Subsystem | Lines | What survives |
|---|---|---|
| `src/physics/` | 1,712 | Y-first per-axis swept AABB and Amanatides & Woo DDA are correct at any resolution. `BlockAccess`/`BlockRegistry`/`SubVoxelAccess` have no counterpart and the data now lives on the GPU. |
| `src/gameplay/` — BlockInteraction, Hotbar, MiningTool | 2,140 | **Already decoupled** via `setRaycaster`/`setBlockWriter`/`setBlockReader`/`setSubVoxelBreaker` — that indirection is exactly what makes it portable. But it indexes 18 block ids with `hardness` and `textureLayer`; gvox voxels have none of those. |
| `src/ui/` | 2,845 | Both use ImGui; yours is on the GL backend, gvox's goes through Daxa's ImGui utility. Rework, not rewrite. |
| `src/world/TerrainGenerator.cpp` | 1,283 | **Design portable, code is a rewrite.** Stateless-hash determinism, the 9-knot continentalness spline, smoothstep biome membership, the 5-cell tree grid — all sound. But it is C++ on 19 threads producing 1 m blocks, and the target is GLSL on the GPU at 6.25 cm. It also depends on FastNoiseLite, a C++ header. |
| `src/platform/Window` | 565 | GLFW survives; the GL 4.5 context does not. |
| `src/render/Camera.hpp` | 308 | Maths survive, **but the depth convention flips** — gvox sets `GLM_FORCE_DEPTH_ZERO_TO_ONE` (`settings.inl:97`) and `TECHNICAL_DESIGN.md` §1 states Voxl explicitly does not. Your frustum extraction depends on that. |

### 5.3 Discarded — ~13,000 lines

| Subsystem | Lines | Why |
|---|---|---|
| `src/mesh/` — GreedyMesher, SubVoxelMesher, MeshData | **2,483** | A ray marcher has no terrain triangles. 100% dead. |
| `src/render/` except Camera | **~3,916** | GL 4.5 → Vulkan/Daxa. Includes the 8-byte packed vertex format, the `GL_TEXTURE_2D_ARRAY` 21-layer contract, the AO shading, the water and sub-voxel programs, and the 945-line procedural `TextureGen.cpp` — **there is no texture mapping in gvox_engine at all.** |
| `src/world/LightEngine` (.cpp 1,143 + .hpp 483) | **1,626** | Replaced by path-traced GI. At 16 voxels/m, Voxl's one byte of light per voxel would be 134 MB *per 32 m cube*. |
| `src/world/WorldSave.cpp` + `RegionFile.cpp` | **1,813** | gvox has no persistence, and at 16 voxels/m the format's premises collapse. See §5.4. |
| `src/world/SubVoxel` + mesher + access + damage store | **~1,200** | **The sharpest loss.** At 16 voxels/m a voxel is 6.25 cm; 1/8 of that is 7.8 mm. Sub-voxel destruction simply *is* voxel editing at this resolution. Your 8³ damage grids, 3-bit extents, dedicated GPU buffers and per-block greedy merge exist only because blocks are 1 m. This was your last shipped milestone. |
| LOD band/skirt machinery (`Lod.hpp` + `ChunkManager`) | ~1,000 | gvox's "LOD" is the in-chunk uniformity pyramid used to *skip empty space while marching* — a different mechanism for a different purpose. |
| `src/world/ChunkStorage` | 227 | Conceptually alive but redundant; gvox already has a GPU-side palette scheme. |
| `docs/design/VOXEL_PULLING.md` | 1,119 | Optimising rasterised chunk geometry. Moot. |

### 5.4 What gvox_engine does NOT have that your game still needs

**This is the real cost of the pivot, and it is the easiest thing to underestimate when the renderer looks that much better.** The renderer is complete and excellent. Everything around it is missing.

| Need | Voxl today | gvox_engine | You must |
|---|---|---|---|
| **Persistence** | 1,813 lines, region files, 44 `TEST_CASE`s, measured 0.544 B/voxel round-trip | **NONE.** I grepped: the only `save` is `AppSettings::save` → `user_settings.json`. The "autosave" checkbox saves *settings*. Quit and the world is gone. | Design from scratch. At 16 voxels/m this is the hardest single item in this document. |
| **Audio** | 2,815 lines, working | **NONE.** `src/application/audio.cpp` is 55 lines, 34 of them comments, and `AppAudio()`'s body is empty. `soloud` is commented out of both `CMakeLists.txt:37` and `vcpkg.json`. | Port yours. Nearly free — this is the cleanest win in the whole move. |
| **Collision** | 1,712 lines, swept AABB, 27 `TEST_CASE`s | Naive triple loop of point samples over a ±2-voxel box, **no sweep at all** (`src/application/player.cpp:198,220`), against a CPU mirror added in a commit literally titled *"testing with simple player collisions"*. | Port yours. **Yours is meaningfully better.** |
| **Player physics** | Gravity, jump, sprint/crouch, fly | ~200 lines of ad-hoc voxel probing | Port yours |
| **Voxel raycast (picking)** | DDA, tested | Exists on the GPU for rendering; CPU-side picking rides the CPU mirror | Bridge GPU↔CPU |
| **Mining & placement** | 1,164 lines, block ids, hardness, tool tiers, 19 `TEST_CASE`s | Hardcoded SDF brushes in GLSL, **chunk-granular**, no block ids, no hardness, no metadata | Invent a material table; rewrite brushes in GLSL |
| **Block registry** | 18 ids with names, hardness, texture layers | 2 bits of material type + 18 bits of colour. No ids, no names, no behaviour | Invent |
| **UI / menus / HUD** | 2,845 lines — MainMenu, PauseMenu, SettingsPanel, Hud, DebugOverlay | ImGui debug panels and a thin main menu | Port yours |
| **Settings** | 747 lines, 20 `TEST_CASE`s | Has its own | Pick one |
| **Entities** | — | One player struct and nothing else | Later |
| **World extent** | ~640 m at render distance 20 | Fixed 128 m cube | Accept, or rearchitect storage |

### 5.5 The test suite

| Bucket | `TEST_CASE`s | Files |
|---|---|---|
| **Survive intact** | **66 (20%)** | jobsystem 24, audio 20, settings 20, log 2 |
| **Survive with rework** | **104 (31%)** | physics 27, mining 19, terrain 17, world 15, daynight 14, chunkstorage 12 |
| **Discarded** | **165 (49%)** | persistence 44, lighting 24, subvoxel 20, lod_stream 19, mesher 16, lod_terrain 15, lod_mesh 14, subvoxel_mesh 10, shaders 3 |

**Roughly half your test suite tests things that stop existing.** The 879,262 triangles / 1,781 draw calls / 557–582 fps you measured describe a renderer that ceases to exist.

One more thing worth stating plainly: `docs/ROADMAP.md` M0 records the rule *"no new dependency is ever added."* gvox_engine's `vcpkg.json` pulls 32 packages and the build installs 46. **Adopting it is the deliberate abandonment of that rule.** Make that choice consciously.

---

## 6. The plan

Each stage ends in something you can run and look at. Stages 0–2 are the ones that decide whether this works.

### The riskiest assumption

> **That you can put your own world into this engine.**

Everything else is now measured. It builds. It runs at 42.5 fps. It fits in 6 GB at the reference world's size. What has never been done by anyone — including the author, whose demo world is the only world this engine has ever rendered — is **replace the generator and keep it running inside the memory budget**. Voxl's terrain is 1,283 lines of C++ calling FastNoiseLite across 19 threads; the target is GLSL on the GPU sustaining ~2 G voxels/s (`MAX_CHUNK_UPDATES_PER_FRAME 128` × 64³ per frame at 60 fps — **5.4× your entire 19-thread CPU pool**, while simultaneously path tracing). That translation is unproven and it gates everything downstream. Stage 1 exists to attack it on day one.

---

### Stage 0 — Own the build (1 day)

Reproduce the working build yourself, from your own clone, in a location that will survive.

- Free up disk (peak 14.2 GB; C: was at 96%).
- Install the **real LunarG Vulkan SDK**.
- Clone to a **short path** — `C:\voxl2`. Not the scratchpad.
- Apply the three source fixes from §3.2.
- Set `VCPKG_MAX_CONCURRENCY=2`. Configure from inside a `vcvars64.bat` shell.
- **Delete FreeImage** (§2.2b) — swap `stb_image` in at the three call sites. Do it now, while the tree is pristine: it removes libraw, jxrlib, OpenEXR and jasper, kills the most-reported build failure, and resolves the GPL question permanently.
- Replace `packaging/infos/license.txt`.

**Done when:** `gvox_engine.exe`, built by you, from your clone, renders the demo world with the debug overlay, and `nvidia-smi` shows VRAM returning to baseline after exit. Record your own frame-time and heap numbers as the baseline for every later stage.

---

### Stage 1 — Your terrain, their renderer (2–3 days) ← **proves the riskiest assumption**

Deliberately small. One GLSL function.

Port Voxl's terrain *shape* into `brushgen_world_terrain()` in `src/voxels/brushes.glsl` (currently lines 305–360). Start with the 9-knot continentalness spline from `TerrainGenerator.cpp` and one biome colour ramp. Ignore trees, grass, caves, ores. You are not trying to make it pretty — you are trying to prove that a Voxl-authored landscape can exist here at all.

Two things to instrument while you do it:
- The heap page count after 60 seconds of walking, versus your Stage 0 baseline.
- Frame time over the same path.

**Done when:** you can walk around a landscape whose silhouette is recognisably Voxl's, path-traced with GI, at ≥30 fps at 720p, and you know what it did to the heap. **If the heap grows past ~1.9 M pages you have found the wall early, which is exactly the point of doing this first.**

---

### Stage 2 — Make memory survivable (3–4 days)

Now that you know your world's memory profile, cap it.

- Add a hard ceiling to `check_for_realloc` (`src/utilities/allocator.inl:264`). Compute the budget from the actual device heap, reserve for the fixed buffers and render targets, and **refuse the growth** rather than calling `create_buffer` and faulting. Surface it in the debug overlay.
- Decide the degradation policy when the heap is full: stop generating new chunks, evict the furthest, or reduce detail. Any deliberate answer beats a device-lost.
- If Stage 1 showed pressure, drop `CHUNKS_PER_AXIS` to 16 (64 m world, chunk table 269 → 33.6 MB) and/or `MAX_GRASS_BLADES` to `1<<20`.
- Fix the 32-bit overflow at `voxel_world.cpp:213` while you are in there — `current_element_count * VOXEL_MALLOC_PAGE_SIZE_BYTES` is computed in `daxa_u32` before the cast to `double`, so the overlay silently wraps above 2,033,601 pages. You will be reading that number constantly from here on; make it trustworthy.

**Done when:** you can run a 10-minute soak with aggressive editing and the app either holds steady or degrades visibly and deliberately — but does not crash. Verified with `nvidia-smi` sampling, not by feel.

---

### Stage 3 — Edit and persist (2–3 weeks) ← **second riskiest**

- Route mining and placement through the brush system. `BlockInteraction`'s existing `setBlockWriter`/`setBlockReader` seams are the integration point — this is where that indirection pays off.
- Invent the material table: map your 18 block ids onto (2-bit type, 18-bit colour, 4-bit roughness). Accept that hardness and tool tiers now live in a CPU-side table keyed by something you carry yourself, because the voxel has no room for an id.
- **Design a save format for a 128 m box.** This is new work with no precedent in either codebase. The engine's own palette compression achieves ~0.197 B/voxel empirically — start by serialising the compressed blobs rather than raw voxels.
- Budget for the fact that the smallest edit costs a full 64³ chunk regeneration, re-palettisation and reallocation.

**Done when:** you dig a hole, quit the app, relaunch, and the hole is still there. That single loop is the whole stage.

---

### Stage 4 — The game shell (2 weeks)

Port from Voxl, in this order, easiest first:

1. **Audio** (`src/audio/`, 2,815 lines) — miniaudio is API-agnostic; this should mostly compile as-is and is the fastest visible win.
2. **Settings** (747 lines) — or adopt gvox's and port your schema.
3. **Menus and HUD** (`src/ui/`, 2,845 lines) — ImGui logic survives; swap the GL backend for Daxa's.
4. **Hotbar and MiningTool** — pure logic.

**Done when:** title screen → world → pause → settings → quit, with sound, on a build with no ImGui debug panel visible.

---

### Stage 5 — Physics and collision (1–2 weeks)

Port Voxl's swept AABB and DDA raycast against the CPU voxel mirror (`VoxelWorld::sample`, `voxel_world.cpp:78-90`). Watch the mirror's allocation pattern — it does a `new uint32_t[]` per palette region per updated chunk, which will need attention at your edit rates. Re-enable `test_physics` (27 `TEST_CASE`s) against the new backend.

**Done when:** you cannot walk through walls, you can step up a 1-voxel rise without jumping, and 27 physics tests pass again.

---

### Stage 6 — Content (ongoing, months)

See §7. This is where the screenshot actually comes from, and it is the longest stage by far.

---

## 7. Authored versus inherited

**A working engine renders an empty world.** If you replace `brushes.glsl` with your own generator and stop there, you will have a correct path tracer looking at a bare landscape, and it will be a shock after the screenshot. Be clear about which half is which.

### Inherited — comes free with the engine

- 16 voxels/metre (`LOG2_VOXEL_SIZE (-4)`)
- Per-voxel colour, zero texture mapping
- Path-traced GI: irradiance cache + ReSTIR diffuse + ReSTIR reflections
- The denoising stack — and it is **not optional**; without it the GI is unusable noise
- Emissive materials feeding the GI
- Physical sky/atmosphere with LUTs and an IBL cube
- Auto-exposure, histogram, tonemap, TAA/FSR2
- GPU palette compression, page allocator, hierarchical DDA traversal

### Authored — this is Gabe's content and you must replace it

- **Terrain shape.** `terrain_noise()`, 6-octave fractal value noise, in `brushes.glsl`.
- **The palette.** The specific red-brown rock and that particular green are hand-chosen biome colour blends. This is art direction, not code.
- **Trees.** `sd_spruce_tree()`, `try_spawn_tree()`, forest density fields — SDFs in GLSL.
- **Flowers.** `MAX_FLOWERS (1 << 16)` with **five hardcoded types**: `DANDELION`, `DANDELION_WHITE`, `TULIP`, `LAVENDER`. The red flowers in the screenshot are these.
- **The emissive white object.** A brush the author placed — `brush_light_ball` / `brush_lantern` / `brush_torch` all exist in `brushes.glsl` and are **currently commented out**.
- **The world seed.** Hardcoded: `src/voxel_app.cpp:125`, `auto seed = 15512089755474631791ull;` with the `std::hash(world_seed_str)` call commented out.

### The one that will surprise you

**The grass is not voxels.** `src/voxels/particles/grass/` defines `MAX_GRASS_BLADES (1 << 22)` = 4.19 million blades, GPU-simulated and **rasterised** as cubes and splats directly into the same G-buffer the ray tracer fills. They spawn on and free themselves from surfaces whose material matches.

Because they live outside the voxel structure, they are invisible to `voxel_trace()`. So: **grass does not bounce light, does not appear in reflections, and casts shadows only via a 2048² ortho shadow map covering 40 m.** The engine is a *hybrid*, not a pure path tracer. That is not obvious from the screenshot, and it changes what "adopting the engine" means — you inherit a particle-vegetation system with fixed content and those specific limits, not a general one.

**The proportion that matters:** roughly 70% of what makes that image striking is *density plus per-voxel colour* — grass modelled blade by blade, rock modelled grain by grain. Maybe 30% is the bouncing light. Adding GI to 1-metre blocks would have produced a very nicely lit Minecraft. Raising resolution is what gets you most of the way, and that is inherited. But the *specific* grass, rock and flowers are content, and content is months.

---

## 8. Fallback

**If a stage fails — most plausibly Stage 2 (memory) or Stage 3 (persistence) — the escape hatch is to keep Voxl's C++/OpenGL 4.5 codebase and implement the technique rather than adopt the engine.** GL 4.5 has compute shaders, SSBOs and `imageStore`; that is everything a DDA ray marcher needs, and since gvox_engine uses **zero hardware ray tracing**, you give up nothing by staying on GL. (For the record: no OpenGL ray-tracing extension has ever existed — `NV_ray_tracing`, `EXT_ray_tracing` and `AMD_ray_tracing` all 404 on the Khronos registry — but it is moot.) You are closer to this than it looks: `src/world/SubVoxel.hpp` already defines `SubVoxelGrid` as an 8³ occupancy bitmask in eight `uint64_t`s, which is structurally a brickmap brick. The work is a GPU-resident sparse structure with an empty-space-skipping pyramid, a GPU allocator for it, and the marcher — realistically 3,000–5,000 lines of new GPU code, with two MIT reference implementations you can read and copy from: gvox_engine's own `src/voxels/impl/trace.glsl` and `src/renderer/kajiya/`, and **dubiousconst282/VoxelRT** (MIT, last active 2025-10-20), whose README is the best-documented comparison of brickmap and tree traversal that exists, with per-structure Mrays/s and edit-cost columns. Defer GI; take the density first. You keep your persistence, physics, audio, gameplay and 335 tests, and you get most of the look on your own terms. **The one caveat: VoxelRT requires ReBAR, and its published benchmarks were run on an integrated GPU with the author only estimating 5–10× on discrete hardware — confirm ReBAR is enabled in your laptop's firmware before relying on its numbers.**

---

## 9. What to do with this repository

**Recommendation: keep Voxl exactly where it is, tag it, and start the new work in a sibling repository. Do not migrate, do not archive, do not merge histories.**

Three reasons. Half the test suite dies, so pulling it forward would mean deleting working tests wholesale. The build systems have nothing in common — CMake + vcpkg + Vulkan + 46 packages versus Voxl's deliberately dependency-free GL build. And the portable subsystems (`src/core/`, `src/audio/`, `src/physics/`, `src/gameplay/`) are **more valuable to you buildable and tested in place** than half-ported into a tree that does not compile yet. Voxl becomes your reference library: when Stage 4 needs audio, you copy from a repo where `test_audio`'s 20 cases still pass.

Also worth saying: Voxl is a finished, working, tested game. That has standing value independent of this pivot, and it costs nothing to preserve.

### Git mechanics

Tag today's state so the rasterised engine is always recoverable by name:

```bash
cd C:/Users/luigi/projects/Voxl
git tag -a v1.0-raster -m "Feature-complete rasterised 1m-block sandbox, 335 tests, 557-582 fps"
git push origin v1.0-raster        # if you have a remote
```

Start the new work beside it, on a **short path** (§3.3):

```bash
cd C:/
git clone https://github.com/GabeRundlett/gvox_engine voxl2
cd voxl2
git submodule update --init --recursive
git remote rename origin upstream  # keep upstream reachable for cherry-picks
git checkout -b voxl
```

Renaming the remote to `upstream` matters more than it looks. The project is dormant *now*, but Daxa is alive and 349 commits ahead of the pin; if you ever need to re-pin Daxa or pull a fix, you want that remote there and you want your work on a branch that is clearly yours.

Then bring portable subsystems across **as file copies, not merges**, recording provenance in the commit message:

```bash
cp -r C:/Users/luigi/projects/Voxl/src/audio C:/voxl2/src/voxl_audio
git add src/voxl_audio
git commit -m "Port Voxl audio engine (from Voxl v1.0-raster src/audio/)"
```

Add a short note at the top of Voxl's `README.md` pointing at the successor and at this document, so that in six months it is obvious which repo is live. That edit is yours to make — this document is the only file this evaluation has added to the Voxl repository, and nothing else here was modified.

---

## Appendix — what remains unverified

Stated plainly, because a plan you commit weeks to should be honest about its edges.

1. **Frame rate at 1080p.** Everything measured is at 1280×720. The 2.25× pixel increase is not linear in a half-res-shaded, upscaled renderer, but expect substantially less than 42 fps.
2. **Whether the heap survives heavy *editing*.** I confirmed the growth path is uncapped and computed that one more step does not fit. I did not trigger it. This is the most likely way the engine falls over on your card, and Stage 2 exists for it.
3. **The reference screenshot's exact scene.** Its heap usage (1693.46 MB) exceeds mine (1171.35 MB) at the identical heap size, so its world state differed. I could not reproduce it.
4. **Optimus behaviour under sustained load.** Device selection is correct (it picked the discrete 3050), and a 30-second moving soak was stable. `docs/KNOWN_LIMITATIONS.md` already records that vsync is advisory on your hybrid setup under WGL; the Vulkan path has its own hybrid-GPU quirks and is untested over long sessions.
5. **Voxl's light-propagation cost.** The committed reference benchmark run records `lighting light_column — NOT MEASURED` (`world/LightEngine.hpp` unavailable in that build), so any claim about scaling flood-fill to 16 voxels/m is structural reasoning, not arithmetic on a measurement.
6. **How palette compression behaves on *your* terrain at 6.25 cm.** The 0.197 B/voxel figure is gvox_engine's own empirical density on Gabe's content. Yours will land somewhere else, and Stage 1 is what tells you where.

### Build artifacts outside this repository

A working build and its dependencies live at `C:\gvx` (3.3 GB) — including `C:\gvx\vksdk`, the synthesised minimal Vulkan SDK, and `C:\gvx\gvox_engine` with the three source fixes applied. A vcpkg binary cache sits at `%LOCALAPPDATA%\vcpkg\archives` (~0.5 GB). Neither is inside `C:\Users\luigi\projects\Voxl`. Both are disposable once Stage 0 produces your own build: `Remove-Item -Recurse -Force C:\gvx`.
