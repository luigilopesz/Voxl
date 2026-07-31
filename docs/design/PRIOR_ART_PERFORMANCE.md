# Prior art: what voxel path tracers actually achieve

**Recorded:** 2026-07-31
**Scope:** research and two local verification measurements. No renderer code was changed.
**Owner of this file:** the prior-art research pass. Nothing else in `C:/voxl2` was written.

This document exists to answer one question with published numbers instead of first principles:
**is 240 fps with path-traced GI something anyone does, and if not, where is the knee of the
curve?**

The short version is in §0. Everything after it is the evidence.

---

## 0. The five findings that matter

1. **We are running a port of kajiya, and kajiya's author published its cost: 8.4 ms at
   1920×1080 on a Radeon RX 6800 XT.** That is 119 fps on a GPU with roughly 3× our compute and
   3.6× our memory bandwidth, rendering a *scene*, not a game. It is the tightest bound in this
   document and it is not close to 240.

2. **Nobody renders path-traced GI at 240 fps natively.** The closest published figures are
   kajiya's 8.4 ms (119 fps, RX 6800 XT, no gameplay) and Quake II RTX at roughly 90 fps on an
   RTX 2080 with 1997-era geometry and hardware RT. NVIDIA's own answer to "4K 240 Hz path
   tracing" is DLSS 4.5 Multi Frame Generation 6× — that is, render ~40 real frames and
   synthesise five out of every six. **240 fps path tracing is a frame-generation number, not a
   rendering number**, and every source that discusses it says so.

3. **The engine does not use the RTX 3050's RT cores at all.** It is a software DDA loop in
   compute (`src/voxels/impl/trace.glsl:98`). The driver exposes `VK_KHR_ray_query`,
   `VK_KHR_acceleration_structure`, `VK_KHR_ray_tracing_pipeline` and
   `VK_NV_ray_tracing_invocation_reorder`; the engine requests none of them
   (`src/utilities/gpu_context.cpp:14`, `choose_device({}, {})`). 20 RT cores are idle every frame.

4. **The two people best placed to judge this both abandoned compute DDA for hardware ray
   tracing — and one of them is this engine's author.** Dennis Gustafsson (Teardown) spent 2024
   building a new voxel renderer that "relies heavily on hardware raytracing (using intersection
   shaders, still no triangles!)" with DLSS Ray Reconstruction for denoising, and handed the
   prototype to **Gabe Rundlett** — the author of gvox_engine, the codebase we forked — for
   optimisation. gvox_engine's `compute-rt` branch, the one we are on, has not moved since
   2024-11. We forked the approach its author moved on from.

5. **"Mountains in the distance" is blocked by architecture, not by frame rate.** The world is a
   64 m cube that wraps around the player (`CHUNKS_PER_AXIS 16` × 4 m chunks,
   `src/voxels/impl/voxel_malloc.inl:20`). Nothing outside ±32 m exists at any instant. No amount
   of optimisation puts a mountain on that horizon. This half of the target needs a far-field
   representation and is independent of the 240 fps question.

---

## 1. What I measured here

Two things, both required before any of the research below can be trusted.

### 1.1 Which GPU is actually running the app — verified, it is the RTX 3050

This machine has two Vulkan devices:

```powershell
vulkaninfo --summary
```
```
apiVersion    = 1.4.341    driverVersion = 610.74.0.0   deviceName = NVIDIA GeForce RTX 3050 6GB Laptop GPU
apiVersion    = 1.4.323    driverVersion = 101.7084     deviceName = Intel(R) RaptorLake-S Mobile Graphics Controller
```

The engine prints its chosen device into the F3 overlay (`src/voxel_app.cpp:60`). Captured and
read:

```powershell
powershell -File C:\voxl2\tools\run.ps1 -Overlay -Seconds 22 `
    -Screenshot <scratchpad>\gpu-check.png -Quit
```

The overlay reads **`GPU: NVIDIA GeForce RTX 3050 6GB Laptop GPU`**. It is not on the iGPU. The
hybrid-graphics concern raised in the brief does not apply here, and every number in
`docs/BASELINE.md` stands on that point.

### 1.2 A frame-time anchor, read off the overlay

```powershell
powershell -File C:\voxl2\tools\run.ps1 -Overlay -ExpandGraphs -Seconds 25 `
    -Screenshot <scratchpad>\pa-baseline-frametime.png -Quit
```

Screenshot viewed. The overlay showed:

| Measurement | Value |
|---|---|
| Full frame-time | **avg 10.00 ms (99.99 fps)**, min 8.77, max 10.78 |
| CPU-only frame-time | avg 0.95 ms (1047.38 fps), min 0.75, max 1.23 |
| GPU | NVIDIA GeForce RTX 3050 6GB Laptop GPU |
| GPU Heap | 393216 pages (830 MB), usage 13 MB (2%) |

**Caveat, and it is a real one:** the harness reported `VRAM before launch 1574 MiB` and
`VRAM after exit 3365 MiB`, against an idle desktop baseline of 273–288 MiB in
`docs/BASELINE.md`. Another agent's engine instance was resident on the same GPU during this
run. **Do not quote the VRAM figures from this run.** The frame time is plausibly a slight
*over*-estimate for the same reason; it agrees with the brief's 10.93 ms and BASELINE.md's
methodology closely enough to serve as an anchor, but it is not a clean-room measurement and
should not replace one.

What the screenshot shows visually: the Voxl test scene — dense grass and flowers in the
foreground at 16 voxels/m, one conifer, a rock hill with a cave mouth and the amber crystal
glow visible inside, and **an empty water-and-sky horizon**. There are no distant mountains
because there is nowhere for them to be. This is finding 5 in picture form.

**CPU 0.95 ms against a 10.00 ms frame confirms the brief: GPU-bound by about 10.5 : 1.**

### 1.3 Does the engine use RT cores — no

```
grep -rE "rayQuery|accelerationStructure|VK_KHR_ray|traceRay" C:/voxl2/src     →  0 matches
```

`src/utilities/gpu_context.cpp:14` is `device = daxa_instance.create_device_2(daxa_instance.choose_device({}, {}));`
— zero implicit features requested. Daxa itself supports `BASIC_RAY_TRACING`,
`RAY_TRACING_PIPELINE`, `RAY_TRACING_INVOCATION_REORDER` and `RAY_TRACING_POSITION_FETCH`
(`deps/Daxa/include/daxa/device.hpp:287-290`), so the capability is one flag away at the device
level, even though using it is not.

The tracer is a software DDA loop:

```glsl
// src/voxels/impl/trace.glsl:98
for (result.step_n = 0; result.step_n < info.max_steps; ++result.step_n) {
```

and `vulkaninfo` confirms the driver exposes what the engine is declining:

```
VK_KHR_acceleration_structure        : extension revision 13
VK_KHR_ray_query                     : extension revision 1
VK_KHR_ray_tracing_pipeline          : extension revision 1
VK_NV_ray_tracing_invocation_reorder : extension revision 1
```

**Note for whoever picks this up:** when I began, `src/utilities/gpu_profiler.hpp` existed as a
header with zero call sites. A parallel agent wired it during this session — by the time I
finished writing, `gpu_profiler.cpp` existed and there were 10 call sites across `renderer.cpp`,
`voxel_app.cpp`, `fsr.cpp` and `gpu_task.hpp`. **I did not have its output.** A per-pass breakdown
of *our* frame was therefore not available to me, and several judgements below are explicitly
gated on reading one. Turn it on with `$env:VOXL_GPU_PROFILE = 'out.csv'`.

---

## 2. The bounding data point: kajiya's own numbers

We are running a C++/GLSL port of Embark's kajiya. Its author, Tomasz Stachowiak (h3r2tic),
published both a total and a full per-pass breakdown.

> "Here's a 1920x1080 image rendered by `kajiya` in 8.4 milliseconds on a Radeon RX 6800 XT."
> — [kajiya/docs/gi-overview.md](https://github.com/EmbarkStudios/kajiya/blob/main/docs/gi-overview.md)

| Pass | Cost | Resolution |
|---|---|---|
| G-buffer | ~1.15 ms | full |
| Indirect diffuse (ReSTIR) | ~2.3 ms | **half** |
| Reflections (ReSTIR) | ~2.2 ms | **half** |
| Irradiance cache | ~0.55 ms | sparse clip maps |
| Direct sun shadows | ~0.52 ms | full |
| SSAO | ~0.17 ms | — |
| Sky & atmosphere | ~0.1 ms | — |
| **Total frame** | **~8.4 ms** | 1920×1080, RX 6800 XT |

Independently, a scene demo was reported at
["Around 11ms/frame here on a Radeon 6800 XT at 1080p"](https://hothardware.com/news/kajiya-renderer-real-time-path-tracing)
— sun and emissive materials only, no particles, no conventional game rendering.

The ray budget behind that ([h3r2tic's ray count breakdown](https://gist.github.com/h3r2tic/5671b999b8b6e5dde29665bdece4fd1a)):
**(0.65/pixel + 128k) gbuffer rays and (1.65/pixel + 384k) shadow rays per frame.** The indirect
diffuse and reflection traces are half-res, and every third frame is a ReSTIR *validation* frame
that re-checks old candidates instead of tracing new ones. This is already an aggressively
economised path tracer — the obvious savings have been taken.

### 2.1 What that implies for an RTX 3050 6GB Laptop

| | RX 6800 XT | RTX 3050 6GB Laptop |
|---|---|---|
| FP32 | ~20.7 TFLOPS | **~6.9 TFLOPS** |
| Memory bus | 256-bit | **96-bit** |
| Bandwidth | ~512 GB/s | **~144 GB/s** |

Sources: [videocardz / notebookcheck RTX 3050 6GB Laptop specs](https://www.notebookcheck.net/NVIDIA-GeForce-RTX-3050-6GB-Laptop-GPU-GPU-Benchmarks-and-Specs.692276.0.html)
(2560 CUDA cores, 96-bit bus, 12 Gbps effective, 35–80 W TGP; 20 RT cores, 80 tensor cores).

**This is an estimate, not a measurement**, and cross-vendor TFLOPS comparisons are crude: kajiya
traces triangle BVHs and we trace a voxel DDA, so the workloads are not the same shape. With
that caveat, ~3× compute and ~3.6× bandwidth says kajiya's own 1080p frame would land somewhere
around **25–30 ms (33–40 fps) on this laptop**, and around **12–15 ms at 720p**.

**We measure 10.00 ms at 1280×720 on a much simpler scene.** That is *consistent with*, and if
anything slightly better than, what the performance model predicts. The useful and slightly
deflating conclusion: **the port is not accidentally slow. There is no forgotten 3× sitting in
it.** Any large win has to come from changing what the renderer does, not from tuning it.

### 2.2 A flag on the 96-bit bus

144 GB/s is very narrow — a desktop RTX 3060 Ti has 448 GB/s. A renderer whose hot loop is
incoherent voxel memory reads is a strong candidate for being **bandwidth-bound rather than
ALU-bound** on this specific part. If true, that changes which optimisations pay: fewer/smaller
memory touches and better cache locality would beat reducing arithmetic, and it would also
predict that hardware BVH traversal (which is bandwidth-optimised in fixed function) helps more
than a raw FLOPS comparison suggests.

**This is a hypothesis. I did not measure it.** It is the first thing the `gpu_profiler` work
should test, ideally with Nsight occupancy/bandwidth counters rather than pass timings alone.

### 2.3 One more thing about kajiya

The repository was **archived by its owner on 1 June 2026** and is described as "no longer
maintained", "a spare-time experiment"
([kajiya README](https://github.com/EmbarkStudios/kajiya/blob/main/README.md)). We own our port
outright, but there is no upstream to pull fixes from and no one to ask.

---

## 3. Comparable projects, with their numbers

Every row here has a URL. Rows marked *(no published timing)* are honest gaps.

| Project | Technique | Hardware RT? | Published performance | Source |
|---|---|---|---|---|
| **kajiya** (what we run) | ReSTIR DI+GI, irradiance cache, half-res traces | Yes (DXR/VK RT) | **8.4 ms @ 1080p, RX 6800 XT**; ~11 ms in a demo scene | [gi-overview](https://github.com/EmbarkStudios/kajiya/blob/main/docs/gi-overview.md), [HotHardware](https://hothardware.com/news/kajiya-renderer-real-time-path-tracing) |
| **Bevy Solari** (2025) | ReSTIR DI+GI + DLSS-RR, 2 rays/px | Yes, inline ray queries | **8.22 ms** (PICA PICA), **14.55 ms** (Bistro), RTX 3080, 1600×900 → 3200×1800 DLSS-RR perf | [jms55](https://jms55.github.io/posts/2025-09-20-solari-bevy-0-17/) |
| **Teardown** (shipped) | Software DDA + MIP space-skipping, screen-space + voxel-space lighting | No | *(no published per-frame timing)* — volumetric shadow map for one level is a "1752×100×1500 3D texture, a 262MB chonker" | [acko.net frame teardown](https://acko.net/blog/teardown-frame-teardown/) |
| **Teardown 2 / next renderer** | Path tracing, **intersection shaders, no triangles**, DLSS Ray Reconstruction | **Yes** | *(no published timing)* | [Voxagon year summary 2024](https://blog.voxagon.se/2024/12/29/year-summary.html) |
| **Minecraft RTX** (shipped) | Full path tracing over meshed voxels | Yes | ~**30 fps** on an RTX 3050 with RT on; default 8-chunk RT render distance, max 24; dropping 8→4-6 chunks "can double FPS" | [Tom's Hardware](https://www.tomshardware.com/news/minecraft-rtx-gpus-benchmarked), [Minecraft Wiki](https://minecraft.wiki/w/Ray_Tracing) |
| **Quake II RTX** (shipped) | Full path tracing, 1997 geometry | Yes | RTX 2060 **~60 fps** @1080p (medium GI); RTX 2070 ~80; **RTX 2080 ~90** | [BabelTech](https://babeltechreviews.com/rtx-quake-ii-iq-and-ultra-performance-with-all-rtx-cards-the-gtx-1080-ti-1660-ti/), [FPS Review](https://www.thefpsreview.com/2019/06/08/quake-ii-rtx-performance-review/) |
| **Aokana** (2025 paper) | SVDAG + LOD + streaming, **no GI at all** | No, compute ray marching | **~6 ms average** at 64K voxel resolution ("ten billion scale"), RTX 3060 Ti; 424 MB VRAM resident of 23 GB on disk | [arXiv 2505.02017](https://arxiv.org/abs/2505.02017) |
| **enkisoftware SVO-DAG path tracer** | Naive compute octree traversal, 1 spp | No | 720p on RTX 4070: ray cast no shadows **13 ms**, ray cast + shadow **25 ms**, path-traced shadows **72 ms**, **path traced everything 447 ms** | [enkisoftware devlog](https://www.enkisoftware.com/devlogpost-20230823-1-Implementing-a-GPU-Voxel-Octree-Path-Tracer) |
| **Sparse 64-tree voxel tracing** | Software traversal, memory-optimised | No | 6358 cycles/ray after optimisation (from 16903 naive); ~0.19–0.62 bytes/voxel vs ESVO ~0.57–1.02 | [dubiousconst282](https://dubiousconst282.github.io/2024/10/03/voxel-ray-tracing/) |
| **John Lin voxel engine** | Path traced voxels, grass/flowers/trees at scale | **Yes** — "100% #rtx using the @NVIDIAGameDev @VulkanAPI ray tracing extension" | *(no published fps)* | [x.com/programmerlin](https://x.com/programmerlin/status/1209000354676760576) |
| **EA SEED GIBS** (shipped) | Surfel GI + hardware RT | Yes | **60 fps target on PS5 / XSX / XSS**, shipped in College Football 25 | [EA](https://www.ea.com/technology/news/gibs-lighting-ea-sports-college-football-25), [SIGGRAPH 2021](https://advances.realtimerendering.com/s2021/) |
| **Douglas Dwyer** | — | — | **Could not find published numbers.** Named in the brief; my searches returned nothing citable. Flagging rather than guessing. | — |
| **gvox_engine** (upstream) | This codebase | No | **No published performance figures found** by its author for the `compute-rt` branch | — |

The enkisoftware row is the most instructive contrast in the table: **447 ms for naive 1-spp path
tracing at 720p on an RTX 4070**, versus kajiya's 8.4 ms at 1080p. The two-orders-of-magnitude gap
is entirely ReSTIR, irradiance caching, half-res traces and denoising. That machinery is what we
already have. It is why the frame is 10 ms and not 400, and it is also why there is not much slack
left in it.

---

## 4. Is 240 fps with path-traced GI a thing anyone does?

**No, not natively. This is the most important finding in this milestone and it should not be
softened.**

240 fps is 4.17 ms. Laid against the table above:

- The best published path-traced GI frame anywhere in this research is **kajiya's 8.4 ms on an
  RX 6800 XT** — a desktop card roughly 3× this laptop's GPU, rendering a static scene with no
  game logic, no animation, no UI. That is 119 fps. Halving it again to reach 240 would require
  another 2× on hardware that is already 3× ahead of ours.
- Every *shipped* path-traced game in the table targets 30–60 fps on hardware in our class.
  Minecraft RTX is ~30 fps on an RTX 3050. Quake II RTX — a 1997 game — is ~60 fps at 1080p on an
  RTX 2060 with hardware RT.
- GIBS, the most production-hardened GI in the list, targets **60 fps**.

The industry's own answer to "240 Hz with path tracing" is explicit and it is frame generation:
NVIDIA markets DLSS 4.5 Dynamic Multi Frame Generation 6× as what "unlock[s] the full potential
of 4K 240Hz OLED gaming displays"
([NVIDIA](https://www.nvidia.com/en-us/geforce/news/dlss-4-5-dynamic-multi-frame-generation-6x-mode-released/)).
6× mode means one rendered frame in six. The commentary around it is consistent that
["brute-forcing a cinematic AAA game with … ray tracing to run at 240 FPS … isn't even possible
with today's gaming GPUs"](https://www.digitalcitizen.life/how-to-use-frame-generation-properly-in-pc-games/),
and that a generated 240 fps "cannot provide the same latency as native 240 FPS."

**And DLSS Frame Generation does not run on this GPU.** It requires the RTX 40-series optical
flow accelerator. FSR 3 frame generation is hardware-agnostic and would work on the 3050
([TechSpot](https://www.techspot.com/article/2747-amd-fsr-3-tech/)) — FSR FG was measured at a
**67% frame-rate increase** versus 42% for DLSS FG, though with 18 ms worse latency.

### What this means for the target, stated plainly

The user's target has two halves and they fail for different reasons:

- **"A map big enough to see mountains in the distance"** — currently impossible at *any* frame
  rate. The world is a 64 m wrapping cube. This is a world-representation problem.
- **"240+ fps"** with the path-traced look intact, natively, at 16 voxels/m, on a laptop RTX
  3050 — **no published work supports this being reachable.** I would put the honest native
  ceiling for this renderer on this hardware, after the work in §5, somewhere in the
  **90–160 fps** band at a reduced internal resolution, and I would want measurements before
  defending even that.

The realistic shape of a 240 number is: **~120 real fps at a reduced internal resolution, doubled
by FSR 3 frame generation.** That is a legitimate way to fill a 240 Hz display and it is exactly
what the industry does. It is not 240 fps of input responsiveness, and the user should decide
knowingly which of those two they wanted.

---

## 5. Techniques, ranked by expected value per unit of work

Ranked best-first. "Backed" means someone published a number; "speculative" means I am reasoning.

### 1. Internal resolution + a better upscaler — **highest EV, lowest risk**

*Backed.* Every expensive pass in kajiya's breakdown is per-pixel, and the two biggest (indirect
diffuse 2.3 ms, reflections 2.2 ms) are already half-res — which is itself the proof that this
lever works. The engine already has the whole apparatus: an FSR 2.2 path with motion vectors,
depth and jitter wired (`src/renderer/fsr.cpp`, `renderer.cpp:181`), and a `Render Res Scale`
slider (`voxel_app.cpp:407`).

DLSS Super Resolution scale factors are Quality 66.7%, Balanced 58%, Performance 50%, Ultra
Performance 33.3% linear — Performance mode is **4× fewer pixels**. DLSS SR is available on the
3050 and integrates via Vulkan NGX.

- **Cost:** low for scaling with the existing FSR 2.2 (a slider sweep, essentially free);
  moderate for adding DLSS SR (NGX Vulkan integration into a slot that already has the right inputs).
- **Buys:** close to linear in pixel count for the per-pixel passes. Plausibly the single
  largest available win.
- **Look:** this is where it costs. At 1280×720 *output*, Performance mode is 640×360 internal,
  and 16 voxels/m aliases hard. Sources warn DLSS benefits diminish below 1440p. **This lever
  probably argues for rendering at a higher output resolution with more upscaling, rather than
  720p native.** Needs A/B screenshots, not just numbers.
- **Note:** a parallel agent owns the resolution/upscaling sweep (tasks #29). I deliberately did
  not run it.

### 2. DLSS Ray Reconstruction, replacing the kajiya denoiser stack — **high EV, must measure first**

*Backed, with a caveat.* This is precisely what Gustafsson did for the Teardown successor: "For
denoising I've been using NVIDIA DLSS Ray Reconstruction, and overall I've been very pleased with
the results." Bevy Solari does the same. RR replaces *multiple* denoisers with one network and
NVIDIA states this "can also offer a performance boost." It is supported on RTX 30-series and
exposed to Vulkan via `NVSDK_NGX_VK_DLSSD_Eval_Params`.

We currently run a shadow denoiser, rtdgi temporal/spatial denoising, rtr denoising, SSAO and
TAA. Collapsing that into RR would also collapse the upscaler into the same pass.

- **Cost:** moderate-to-high. NGX integration, and RR is demanding about its inputs (HDR only,
  needs clean normals/roughness/motion vectors — we have most of these).
- **Buys:** unknown until we know what our denoiser stack costs. **In Solari, DLSS-RR was 5.75 ms
  of an 8.22 ms frame** — it is emphatically *not* free, though that was at 1600×900 → 3200×1800.
  At 720p output it would be far cheaper.
- **Look:** likely *improves* it (that is RR's whole purpose), at the cost of RR's known
  over-aggressiveness — Gustafsson: "a bit aggressive in some scenarios, but still very impressive."
- **Gate:** do not start this before `gpu_profiler` tells us what the current denoisers cost. If
  they are 1.5 ms of a 10 ms frame, RR cannot win.

### 3. Software VRS / adaptive sampling on the GI passes — **good EV, small and reversible**

*Backed.* Hardware VRS is **not usable here** — it only applies to pixel shaders, and this
renderer is entirely compute. But software VRS is: classify 8×8 tiles by a Sobel edge test on the
previous frame's denoised GI, and shade at 1×1 / 2×1 / 1×2 / 2×2 accordingly. Published results:
**~32% reduction in raytracing cost with per-thread rejection, 27.3–28.1% with per-wave**
([Interplay of Light](https://interplayoflight.wordpress.com/2022/05/29/accelerating-raytracing-using-software-vrs/)).

- **Cost:** low. One classification pass plus a branch in the GI trace kernels.
- **Buys:** ~25–30% of the GI trace cost. On a 10 ms frame where GI is maybe half, call it 1–1.5 ms.
- **Look:** "a small impact on the GI noise", visually "very similar". Cheapest real win in this list.
- **Fits the phase:** small, reversible, measurable. **This is the one I would prototype first.**

### 4. Hardware ray query over the voxel structure — **highest ceiling, highest risk, most work**

*Backed by practitioner choice; NOT backed by a head-to-head number I could find.*

The case for it is strong and specific:
- Both Dennis Gustafsson and Gabe Rundlett independently moved to hardware RT with **intersection
  shaders and no triangles** for exactly this workload. Rundlett is the author of this codebase.
- Gustafsson's stated benefit is the one our target needs: "it no longer uses a shadow volume,
  which enables **unlimited world size**, sharp shadows, no light leakage and vast amounts of
  moving objects at a very small cost." Unlimited world size is finding 5's solution.
- John Lin's grass/flowers/trees voxel scenes — the closest visual match to the user's stated
  target that exists — are "100% RTX using the Vulkan ray tracing extension."
- 20 RT cores currently do nothing every frame.

**The honest counter-case, which is important:** custom AABB/procedural primitives do *not* get
RT-core-accelerated intersection. Only BVH *traversal* is hardware-accelerated; the intersection
shader runs on ordinary shader cores. NVIDIA's own best-practices guide says
["Use triangles over AABBs. RTX GPUs excel in accelerating traversal of AS created from triangle
geometry"](https://developer.nvidia.com/blog/best-practices-for-using-nvidia-rtx-ray-tracing-updated/),
and an NVIDIA engineer on their forum states plainly that to accelerate primitive intersection
"the way to do that right now is to use the OptiX triangle API"
([devtalk](https://forums.developer.nvidia.com/t/using-rtx-acceleration-for-voxel-tracing/74007)).
One cited measurement puts analytic intersection at **about 2× slower than ray-triangle**.
Gustafsson also notes "raytracing voxels inside an arbitrarily skewed bounding box was a much
harder problem than anticipated."

So the win, if it exists, comes from hardware BVH traversal, hardware ray scheduling/reordering
(`VK_NV_ray_tracing_invocation_reorder` is exposed on this GPU) and the architectural unlock of
unbounded world size — **not** from the intersection test itself.

- **Cost:** very large. New acceleration structure over bricks, BLAS/TLAS build and refit for a
  destructible world, and every one of the ~18 files that call `voxel_trace` rewritten. This is a
  renderer rewrite and **must not be attempted in this phase.**
- **Buys:** unknown. I could not find a published head-to-head of ray query vs software DDA over
  a voxel brickmap, and I am explicitly flagging that as the largest unknown in this document.
- **Recommended next step, and it fits the phase:** a **standalone scratch benchmark** — one
  Vulkan app, one fixed set of camera rays, traced against (a) the current DDA and (b) a BLAS of
  per-brick AABBs with an intersection shader. A few hundred lines, outside `C:/voxl2/src`, and it
  converts the biggest decision in this project from argument into measurement. **Highest-value
  single experiment available.**

### 5. Radiance cascades — **interesting, but not the lever, and partly already present**

*Speculative for our case.* I want to be careful here because RC is currently fashionable and the
headline numbers circulating do not mean what they appear to.

- The widely-quoted **"0.3 ms on a GTX 970"** comes from a demo by Asbjørn Lystrup, described as
  running "with no denoising or temporal accumulation"
  ([80.lv](https://80.lv/articles/radiance-cascades-new-approach-to-calculating-global-illumination)).
  The same article's other figure is **~12 ms** for a scene-independence demonstration. These are
  not comparable to a full 3D GI budget and should not be quoted as "RC costs 0.3 ms".
- radiance.wiki states outright that **"extending RC to 3D remains an open problem"**, and that
  leading implementations "either run in 2D or screenspace, due to the prohibitive costs of
  storing high-detail volumetric radiance information."
- In 3D, "eight samples of cascade i+1 are required for each sample of cascade i" — the memory and
  bandwidth scaling is the problem, and **we are on a 144 GB/s bus**.
- Ringing artefacts are a known open issue; some fixes reportedly cost "doubling the frame time".
- The state of the art is nine days old: **Split Radiance Cascades** (Rouli Freeman and Alexander
  Sannikov, [arXiv 2607.20384](https://arxiv.org/abs/2607.20384), submitted 22 July 2026), which
  uses a sparse world-space hashmap of probes specifically because dense 3D storage is
  prohibitive. **I could not retrieve its performance table** — both PDF and HTML fetches failed.
  Someone should read that paper properly; it is the most relevant new work in this space.

The structural argument against it here matters more than any of that: **RC would replace the
diffuse GI, which is 2.3 ms of kajiya's 8.4 ms.** Even at zero cost that caps out around 27% of
the frame. And RC still has to trace *something* — against our voxel structure — so the ray cost
does not vanish. Meanwhile kajiya's irradiance cache is already a probe-based amortisation scheme
doing a related job at 0.55 ms. RC's celebrated "constant cost independent of scene complexity"
is already largely true of a voxel DDA.

- **Cost:** high (new GI system, new artefact class, active research area).
- **Buys:** at most a fraction of 2.3 ms-equivalent, unproven in our architecture.
- **Look:** RC's zero-latency, no-temporal-accumulation property is genuinely attractive for a
  fast-moving player — that is its real selling point for us, not raw speed.
- **Verdict:** worth one person reading the Split RC paper. Not worth building this quarter.

### 6. Surfel / probe GI instead of per-pixel ReSTIR — **modest EV; we partly have it already**

*Backed at 60 fps, not beyond.* GIBS ships at a **60 fps target on PS5/XSX/XSS**. SurfelPlus, a
low-end-hardware variant, reports "2560×1440 while keeping over 120 FPS" and a "15% fps increase"
from a non-uniform acceleration structure — but names no GPU, so I would not lean on it.

The key point: **kajiya's irradiance cache already is a sparse world-space probe cache** (12
sparsely-allocated 32×32×32 clip maps, <16k entries), and it costs 0.55 ms. Replacing per-pixel
ReSTIR with surfels trades noise for blur and low-frequency leaking. Given the user pivoted here
*for the path-traced look*, that trade is against the grain.

- **Cost:** high. **Buys:** unclear. **Look:** softer, blurrier GI. **Verdict:** low priority.

### 7. Frame generation (FSR 3) — **the only literal route to 240, with an asterisk**

*Backed.* DLSS FG is 40-series-only; **FSR 3 FG works on the 3050**. Measured at +67% frame rate
(vs +42% for DLSS FG) with 18 ms worse latency. FSR 3.1 decoupled FG from its upscaler, so it can
sit on top of whatever upscaling we choose.

- **Cost:** moderate (FFX FG integration; we already ship FidelityFX for FSR 2.2 and the shadow
  denoiser, so the dependency exists).
- **Buys:** roughly 1.6–2× displayed frame rate.
- **Look:** preserved in stills; interpolation artefacts on fast motion and on the UI.
- **The asterisk:** it does not improve responsiveness. From 120 real fps it gets you to ~240
  displayed at ~120 fps of input latency. **Do this last, and only after being explicit with the
  user that this is what the number means.**

### 8. Hardware VRS — **not applicable, closing it out**

Hardware VRS only affects pixel shaders. This renderer is compute end to end. See §5.3 for the
software equivalent, which is the usable form.

---

## 6. The finding that is not about frame rate

**The world is a 64 m cube that wraps around the player.** `CHUNKS_PER_AXIS 16`, 4 m chunks
(`src/voxels/impl/voxel_malloc.inl:20`), and the file's own comment says it: "The world is a fixed
cube of CHUNKS_PER_AXIS^3 chunks that wraps around the player; there is no streaming and nothing
outside it exists."

Doubling to `CHUNKS_PER_AXIS 32` gives a 128 m cube and, per `docs/BASELINE.md`, a heap that
ratchets to 2906 MB with a next-grow-step transient peak of 7266 MB against 6144 MB of VRAM. So
brute force does not reach even 256 m, let alone a horizon.

"Mountains in the distance" therefore requires a **far-field representation** — streamed LOD
bricks, an SVDAG with LOD, or a heightfield/impostor shell beyond the detail volume. Aokana is the
directly relevant prior art: SVDAG plus LOD plus streaming, **~6 ms on an RTX 3060 Ti at ten-billion-voxel
scale with only 424 MB resident** ([arXiv 2505.02017](https://arxiv.org/abs/2505.02017)) — but
note carefully that **Aokana implements no global illumination at all**, only normal-based
shading. It proves the *visibility and streaming* half of the target is affordable. It says
nothing about doing GI on top.

That split is probably the most useful way to think about the whole project:
**a huge voxel world at high frame rate is a solved problem (Aokana, 6 ms). Path-traced GI at
high frame rate is a solved problem at ~120 fps on a 3× GPU (kajiya, 8.4 ms). Nobody has published
both at once, and certainly not at 240 fps on a laptop 3050.**

---

## 7. What I could not determine

Flagging these rather than filling them with plausible-sounding numbers.

1. **A head-to-head measurement of `VK_KHR_ray_query` vs software DDA over a voxel brickmap.**
   The single most important unknown here. The recommendation in §5.4 rests on practitioner
   choice and architectural reasoning, not on a number. §5.4's scratch benchmark is how to fix that.
2. **Our own per-pass breakdown.** The `gpu_profiler` was wired by a parallel agent while I was
   writing this and I never saw its output. Without it I cannot say what fraction of our 10 ms is
   GI vs primary visibility vs denoising, which gates the DLSS-RR judgement in §5.2 and the
   bandwidth hypothesis in §2.2. **Read that CSV before acting on §5.2.**
3. **Split Radiance Cascades performance figures** — PDF and HTML fetches both failed
   (`maxContentLength` and 404). The paper is nine days old and is the most relevant current work
   on 3D RC.
4. **Teardown's actual frame timings.** Gustafsson has never published a per-frame or per-pass
   cost that I could find, in either the shipped renderer or the successor.
5. **Douglas Dwyer's numbers.** Named in the brief; my searches surfaced nothing citable under
   that name. Possibly I searched for the wrong thing.
6. **Minecraft RTX exact fps tables.** Tom's Hardware, OC3D and GameGPU all returned 403 or
   truncated content to the fetcher. The ~30 fps RTX 3050 figure and the "8→4-6 chunks doubles
   FPS" claim come from search summaries of those pages, not from pages I read directly — treat
   them as weaker than the kajiya and Solari figures, which I read at source.
7. **Whether the 96-bit bus is the actual bottleneck** (§2.2). Hypothesis only.
8. **My 10.00 ms anchor was taken with another agent's engine instance resident on the GPU.**
   It agrees with `docs/BASELINE.md` but is not clean-room. The VRAM figures from that run are
   unusable.

---

## 8. Recommended order of work

1. **Wire `gpu_profiler` and get a per-pass breakdown.** Everything else is guessing until this
   exists. Already owned by another task.
2. **Software VRS on the GI traces.** Small, reversible, ~27–32% of GI trace cost, published.
3. **The ray-query scratch benchmark** (§5.4). A few hundred lines outside the engine tree that
   settles the largest open question in the project.
4. **Resolution/upscaler curve** (owned elsewhere, task #29) — likely the biggest single lever.
5. **Far-field world representation.** Independent of frame rate and required by half the target.
6. Only then: DLSS-RR, and only if the profiler says the denoiser stack is worth replacing.
7. Frame generation last, with an explicit conversation about what "240 fps" then means.

---

## Sources

- [kajiya — GI overview and per-pass timings](https://github.com/EmbarkStudios/kajiya/blob/main/docs/gi-overview.md)
- [kajiya — README, supported hardware, archived Jun 2026](https://github.com/EmbarkStudios/kajiya/blob/main/README.md)
- [h3r2tic — kajiya ray count breakdown](https://gist.github.com/h3r2tic/5671b999b8b6e5dde29665bdece4fd1a)
- [HotHardware — kajiya at ~11 ms on an RX 6800 XT at 1080p](https://hothardware.com/news/kajiya-renderer-real-time-path-tracing)
- [Voxagon Blog — Dennis Gustafsson, year summary 2024 (hardware RT, intersection shaders, DLSS-RR, handed to Gabe Rundlett)](https://blog.voxagon.se/2024/12/29/year-summary.html)
- [acko.net — Teardown Frame Teardown](https://acko.net/blog/teardown-frame-teardown/)
- [jms55 — Realtime Raytracing in Bevy 0.17 (Solari) pass timings](https://jms55.github.io/posts/2025-09-20-solari-bevy-0-17/)
- [arXiv 2505.02017 — Aokana: A GPU-Driven Voxel Rendering Framework for Open World Games](https://arxiv.org/abs/2505.02017)
- [arXiv 2607.20384 — Split Radiance Cascades (Freeman & Sannikov, 22 Jul 2026)](https://arxiv.org/abs/2607.20384)
- [radiance.wiki — Radiance Cascades index; 3D "remains an open problem"](https://radiance.wiki/)
- [80.lv — Radiance Cascades, the 0.3 ms / 12 ms figures in context](https://80.lv/articles/radiance-cascades-new-approach-to-calculating-global-illumination)
- [enkisoftware — Implementing a GPU Voxel Octree Path Tracer (720p RTX 4070 timings)](https://www.enkisoftware.com/devlogpost-20230823-1-Implementing-a-GPU-Voxel-Octree-Path-Tracer)
- [dubiousconst282 — A guide to fast voxel ray tracing using sparse 64-trees](https://dubiousconst282.github.io/2024/10/03/voxel-ray-tracing/)
- [Interplay of Light — Accelerating raytracing using software VRS (27–32%)](https://interplayoflight.wordpress.com/2022/05/29/accelerating-raytracing-using-software-vrs/)
- [NVIDIA — Best Practices for Using NVIDIA RTX Ray Tracing ("use triangles over AABBs")](https://developer.nvidia.com/blog/best-practices-for-using-nvidia-rtx-ray-tracing-updated/)
- [NVIDIA DevTalk — Using RTX Acceleration for Voxel Tracing](https://forums.developer.nvidia.com/t/using-rtx-acceleration-for-voxel-tracing/74007)
- [EA — GIBS lighting in EA SPORTS College Football 25 (60 fps target)](https://www.ea.com/technology/news/gibs-lighting-ea-sports-college-football-25)
- [SurfelPlus — surfel GI for low-end hardware](http://wangruipeng.com/SurfelPlus/)
- [NVIDIA — DLSS 4.5 Dynamic Multi Frame Generation 6X (the 4K 240 Hz claim)](https://www.nvidia.com/en-us/geforce/news/dlss-4-5-dynamic-multi-frame-generation-6x-mode-released/)
- [VideoCardz — DLSS 4.5 Ray Reconstruction across RTX 20/30/40/50](https://videocardz.com/newz/nvidia-dlss-4-5-ray-reconstruction-coming-in-august-for-rtx-20-30-40-and-50-series)
- [TechSpot — AMD FSR 3 Frame Generation analysed (+67% vs +42%, 18 ms latency)](https://www.techspot.com/article/2747-amd-fsr-3-tech/)
- [Notebookcheck — RTX 3050 6GB Laptop GPU specs (2560 cores, 96-bit bus)](https://www.notebookcheck.net/NVIDIA-GeForce-RTX-3050-6GB-Laptop-GPU-GPU-Benchmarks-and-Specs.692276.0.html)
- [Tom's Hardware — Minecraft RTX GPUs benchmarked](https://www.tomshardware.com/news/minecraft-rtx-gpus-benchmarked)
- [BabelTech — Quake II RTX across all RTX cards](https://babeltechreviews.com/rtx-quake-ii-iq-and-ultra-performance-with-all-rtx-cards-the-gtx-1080-ti-1660-ti/)
- [John Lin — 100% RTX voxel scenes with grass, flowers and trees](https://x.com/programmerlin/status/1209000354676760576)
