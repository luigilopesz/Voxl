# Where the frame actually goes

**Recorded:** 2026-07-31
**Commit:** `4594a45` plus the instrumentation described in §9 (uncommitted at the time of writing)
**Hardware:** RTX 3050 6 GB Laptop GPU (driver 610.74), i7-13650HX, 16 GB, Windows 11 Pro 26200

Every number below comes from GPU timestamps written by the engine itself, one pair per render
pass, read back three frames late. Every table names the command that produced it. Nothing here
is an estimate.

---

## The one-page answer

The Voxl test scene at the spawn, 1280×720, RTX 3050 6 GB Laptop. 4.17 ms is 240 fps.

| configuration | GPU span | wall | fps | what it costs you |
|---|---:|---:|---:|---|
| 1920×1055, stock | 20.98 | 21.00 | 48 | |
| 1600×900, stock | 16.10 | 16.12 | 62 | |
| the inherited demo world, 1280×720 | 12.73 | 13.06 | 77 | (different scene, for comparison) |
| **1280×720, stock — the baseline** | **11.22** | **11.34** | **88** | — |
| FSR 2.2 instead of the kajiya TAA | 10.45 | 10.68 | 93 | different AA character |
| 960×540, stock | 7.72 | 7.74 | 129 | a small window |
| **half-res render, GI intact** | **5.26** | **5.34** | **187** | softer grass, chunkier needles; GI look intact |
| GI stack removed entirely | 5.26 | 5.40 | 185 | flat and bright; the cave glow gone |
| GI + sun shadows removed | 4.52 | 4.65 | 215 | no shadows at all |
| GI + shadows + TAA + sky removed | 2.96 | 3.01 | 332 | black sky, aliased, unusable |

Three facts to take away:

1. **The GI stack is 53 % of the frame** (5.95 of 11.22 ms), and two thirds of that is ReStIR
   diffuse. The single most expensive pass in the renderer, `RtdgiTraceCompute`, is 17 % of the
   frame on its own.
2. **Deleting all of it still does not reach 240 fps at 1280×720 native.** The floor with the
   whole GI stack switched off is 5.26 ms — 190 fps. 4.17 ms is below the floor.
3. **Halving the render resolution buys exactly as much as deleting the GI stack** — 5.264 ms
   either way, from two independent runs — and keeps the look. **The lever with a future is
   pixels, not features.**

---

## 0. Read this first: three things that will corrupt your measurement

All three cost a wasted run here before they were caught. They are not hypothetical.

1. **`%APPDATA%\GabeVoxelGame\user_settings.json` decides what the renderer builds, and it is
   shared mutable state.** Several agents are driving this engine at once and every one of them
   writes that file. The first profiled run in this session came back at **5.06 ms instead of
   11.34 ms** because a sibling had left `Graphics/global_illumination = false`; the run looked
   entirely normal and the frame time looked like a triumph. A later one came back at 5.17 ms
   because `Render Res Scale` had been left at **0.5** — and worse, a settings template built by
   copying the live file and resetting every entry to its own `user_default` still carried the
   0.5, because whoever wrote it had written *both* fields. Neither the engine nor `--bench-csv`
   says a word about any of this.
   *Mitigation:* write a known settings file immediately before launch and read the six
   `Graphics` keys back three seconds after launch — the file is read exactly once, in the
   `AppUi` constructor, so a write after that does not affect the run. The harness in §6.2 does
   both. Whatever harness you use, **print the settings that were in force next to every frame
   time you quote.**

2. **A second engine instance on the same GPU roughly doubles the frame time.** 26 ms against a
   clean 11 ms. Checking the process table before and after the run is not enough — a sibling
   that starts one second in and exits one second before you do is invisible that way. Sample
   the live process count for the whole run and reject anything above ~2 % shared.

3. **A failed shader compile silently deletes a pass rather than failing.**
   `register_null_pipelines_when_first_compile_fails` is on (`gpu_context.cpp:31`) and
   `Task::callback` returns early on `!pipeline->is_valid()` (`utilities/gpu_task.hpp`). A frame
   that is missing its entire GI stack renders, runs fast, and reports nothing. The profiler
   makes this visible — the CSV simply has no `Rtdgi*` columns — which is how the case in (1)
   was diagnosed.

---

## 1. The GPU question, settled

**The engine runs on the discrete RTX 3050, not the Intel iGPU.** Three independent pieces of
evidence:

```powershell
# while the engine is up:
nvidia-smi --query-compute-apps=pid,process_name,used_gpu_memory --format=csv,noheader
#   ... 24400, C:\voxl2\.out\...\gvox_engine.exe, [N/A]
nvidia-smi --query-gpu=index,name,utilization.gpu,memory.used --format=csv,noheader
#   0, NVIDIA GeForce RTX 3050 6GB Laptop GPU, 100 %, 2063 MiB      (idle before launch: 25 %, 365 MiB)
```

- The process appears in nvidia-smi's own compute-apps table on GPU 0.
- `utilization.gpu` goes from 25 % idle to **100 %** for the duration and device memory rises
  from 365 MiB to 2063–3840 MiB, tracking the engine's lifetime.
- `clocks.sm` sits at **1897–1935 MHz** (near the part's boost ceiling) throughout, and
  `temperature.gpu` peaks at 73–78 °C with no sustained clock drop. There is no thermal
  throttling in a 30 s run.

The device selection is `daxa_instance.choose_device({}, {})` (`utilities/gpu_context.cpp:14`),
which prefers a discrete adapter, and the in-app overlay reports
`GPU: NVIDIA GeForce RTX 3050 6GB Laptop GPU`. **No iGPU problem exists. Every existing
measurement in `docs/BASELINE.md` and `docs/SCENE.md` stands on this point.**

---

## 2. The frame is GPU-bound end to end, with no idle

This is worth stating precisely, because "GPU-bound 10:1" in the brief was inferred from a CPU
timer, not measured on the GPU. It is now measured on the GPU:

| scenario | wall frame (ms) | GPU span (ms) | GPU span as % of wall |
|---|---:|---:|---:|
| island, stationary | 11.340 | 11.216 | **98.9 %** |
| island, moving | 8.533 | 8.424 | **98.7 %** |
| demo world, stationary | 13.063 | 12.732 | **97.5 %** |
| facing rock 2 m away | 11.700 | 11.616 | **99.3 %** |
| elevated, mostly sky and sea | 6.725 | 6.683 | **99.4 %** |

"GPU span" is the interval from the first timestamp of the frame (opening the first sky-LUT
pass) to the last (closing the ImGui draw). It excludes only `vkQueuePresent` and the CPU work
that overlaps it. **There is nothing to win on the CPU and there is no queue starvation to
recover** — between 0.6 % and 2.5 % of the frame is everything outside the render graph put
together, and the engine's own `cpu_ms` counter reads 1.0 ms of *overlapped* CPU work against
that 11.2 ms GPU frame.

---

## 3. The breakdown

### 3.1 Scenario 1 — the 37 m island, stationary at the spawn

1280×720, stock settings, 1235 settled frames from t = 12 s to t = 26 s, uncontended.
Wall frame **11.340 ms (88 fps)**, GPU span **11.216 ms** = 98.9 % of wall.

```powershell
$env:VOXL_GPU_PROFILE = 'out\S1.prof.csv'; $env:VOXL_GPU_PROFILE_START_S = '12'
C:\voxl2_prof\bin\gvox_engine.exe --unpause --exit-after 26 --width 1280 --height 720 `
  --pos -182.99,-109.98,-46.97 --rot 0.785,1.096 --no-overlay --bench-csv out\S1.bench.csv
```

| stage | ms | % of GPU frame |
|---|---:|---:|
| **ReStIR diffuse (rtdgi)** | **3.506** | **31.3** |
| primary visibility (depth prepass + primary trace + blit) | 1.420 | 12.7 |
| ReStIR reflections (rtr) | 1.221 | 10.9 |
| TAA | 1.050 | 9.4 |
| irradiance cache | 1.027 | 9.2 |
| sun shadow trace | 0.754 | 6.7 |
| particle raster (grass, flowers, mushrooms) | 0.544 | 4.8 |
| barriers / layout transitions / unscoped tasks | 0.341 | 3.0 |
| sky and IBL cube | 0.312 | 2.8 |
| SSAO | 0.261 | 2.3 |
| particle simulation | 0.255 | 2.3 |
| post and exposure | 0.172 | 1.5 |
| light composition | 0.171 | 1.5 |
| reprojection map | 0.122 | 1.1 |
| chunk generation / voxel world | 0.053 | 0.5 |
| UI and buffer upload/download | 0.007 | 0.1 |
| **total GPU span** | **11.216** | **100.0** |

**Top twelve individual passes** (of 96):

| # | pass | mean ms | p50 | p99 | % of frame |
|---|---|---:|---:|---:|---:|
| 1 | `RtdgiTraceCompute` | 1.903 | 1.946 | 2.526 | **17.0** |
| 2 | `TracePrimaryCompute` | 1.075 | 1.077 | 1.465 | **9.6** |
| 3 | `TraceSecondaryCompute` (sun shadows) | 0.754 | 0.743 | 1.201 | **6.7** |
| 4 | `RtrRestirResolveCompute` | 0.484 | 0.336 | 4.244 | 4.3 |
| 5 | `IrcacheValidateCompute` | 0.391 | 0.378 | 0.859 | 3.5 |
| 6 | `GrassStrandCubeParticleRaster` | 0.383 | 0.383 | 0.398 | 3.4 |
| 7 | `TraceIrradianceCompute` | 0.376 | 0.364 | 0.816 | 3.4 |
| 8 | `RtdgiRestirResolveCompute` | 0.343 | 0.339 | 0.789 | 3.1 |
| 9 | `TaaCompute` | 0.337 | 0.330 | 0.776 | 3.0 |
| 10 | `TraceDepthPrepassCompute` | 0.303 | 0.263 | 0.543 | 2.7 |
| 11 | `RtrTemporalFilterCompute` | 0.265 | 0.267 | 0.577 | 2.4 |
| 12 | `RtdgiRestirTemporalCompute` | 0.249 | 0.247 | 0.560 | 2.2 |

The remaining 84 passes together are 3.7 ms; **about 60 of them cost under 0.05 ms each and
total 0.6 ms.** The sum of all 96 passes is 10.875 ms, 97.0 % of the span; the missing 0.341 ms
is barriers, layout transitions and the handful of unscoped inline tasks.

### 3.2 The three required scenarios side by side

| stage (ms) | island still | island moving | **demo world** | rock wall 2 m | elevated vista |
|---|---:|---:|---:|---:|---:|
| primary visibility | 1.420 | 1.396 | 1.613 | 0.859 | **1.992** |
| sun shadow trace | 0.754 | 0.395 | **1.471** | 1.189 | 0.152 |
| irradiance cache | 1.027 | 0.606 | 0.876 | 1.323 | 0.535 |
| ReStIR diffuse | **3.506** | 1.890 | **4.383** | **4.237** | 0.938 |
| ReStIR reflections | 1.221 | 0.849 | 1.059 | 1.269 | 0.496 |
| SSAO | 0.261 | 0.243 | 0.259 | 0.287 | 0.182 |
| light composition | 0.171 | 0.187 | 0.174 | 0.163 | 0.210 |
| TAA | 1.050 | 1.059 | 1.084 | 1.039 | 1.051 |
| post / exposure | 0.172 | 0.174 | 0.187 | 0.170 | 0.171 |
| particle sim | 0.255 | 0.234 | 0.394 | 0.237 | 0.183 |
| particle raster | 0.544 | 0.211 | 0.497 | 0.086 | 0.066 |
| chunk generation | 0.053 | **0.425** | 0.043 | 0.047 | 0.048 |
| sky | 0.312 | 0.310 | 0.314 | 0.312 | 0.335 |
| reprojection | 0.122 | 0.098 | 0.107 | 0.125 | 0.073 |
| barriers / unscoped | 0.341 | 0.338 | 0.267 | 0.266 | 0.245 |
| **GPU span** | **11.216** | **8.424** | **12.732** | **11.616** | **6.683** |
| **wall frame** | 11.340 | 8.533 | 13.063 | 11.700 | 6.725 |
| **fps** | 88 | 117 | 77 | 85 | 149 |
| **GI as % of frame** | 53.6 % | 42.6 % | 51.7 % | **61.3 %** | 32.2 % |
| voxel heap in use | 12.8 MB | 13.4 MB | **146.6 MB** | 14.6 MB | 6.7 MB |

**Moving is *cheaper* than standing still** in this scene, and it is not a measurement error: the
patrol circle spends most of a lap looking outward over the sea, where there is no near geometry
for rtdgi to trace against. What moving *adds* is **chunk generation, 0.053 → 0.425 ms**, and a
p99 of 20 ms against a p50 of 8.0 — the hitches are chunk generation, not a raised floor. That
matches `docs/SCENE.md` §8.2.

**The demo world costs 13.06 ms against the Voxl scene's 11.34 — 15 % more — and the extra is
almost entirely GI and shadows**: ReStIR diffuse +0.88 ms and the sun shadow trace +0.72 ms,
which together are 1.60 of the 1.73 ms difference. Its terrain is thin, noisy and everywhere,
so far more of the frame is near-field surface. Note two caveats: the demo world is measured at
the **engine's own default spawn**, not the Voxl pose (pointing the camera at the Voxl spawn
coordinates in the demo terrain frames something unrelated), so this is a scene comparison and
not a pose-matched A/B; and it was run from a separate tree, `C:\voxl2_demo`, which is the same
binary with `VOXL_TEST_SCENE 0` in its own copy of `src/`. Its voxel heap sits at **146.6 MB
against 12.8 MB**, which reproduces `docs/SCENE.md` §8.1 exactly.

---

## 4. The four questions, answered

### 4.1 Top three passes, and the GI share

**Top three, unchanged across every scenario:** `RtdgiTraceCompute`, `TracePrimaryCompute`,
`TraceSecondaryCompute`. Together 3.73 ms of an 11.22 ms frame — **33.3 %** — and all three are
the same thing under the hood: a hierarchical DDA march through the voxel volume
(`voxels/impl/trace.glsl`).

**GI versus primary visibility versus everything else**, stationary at the spawn:

| | ms | % |
|---|---:|---:|
| GI: irradiance cache + ReStIR diffuse + ReStIR reflections + SSAO | **6.014** | **53.6** |
| primary visibility | 1.420 | 12.7 |
| sun shadow trace | 0.754 | 6.7 |
| everything else (TAA, post, particles, sky, chunk gen, barriers) | 3.028 | 27.0 |

The GI share ranges from 32 % (nothing but sky and sea in frame) to 61 % (a rock face two metres
away). **Half the frame is GI, and two thirds of the GI is ReStIR diffuse.**

### 4.2 Scaling with resolution — linear in pixels, plus a 3.2 ms fixed floor

Same pose, same settings, only `--width`/`--height` or `Render Res Scale` changed. All five runs
verified uncontended and stock apart from the stated override.

| config | internal render | Mpx | GPU span (ms) | wall (ms) | fps |
|---|---|---:|---:|---:|---:|
| 1280×720 output, `Render Res Scale` 0.5 | 640×360 | 0.230 | **5.264** | 5.335 | 187 |
| 960×540 | 960×540 | 0.518 | **7.718** | 7.740 | 129 |
| **1280×720 (baseline)** | 1280×720 | 0.922 | **11.216** | 11.340 | 88 |
| 1600×900 | 1600×900 | 1.440 | **16.098** | 16.119 | 62 |
| 1920×1080 | 1920×1055 † | 2.026 | **20.984** | 21.003 | 48 |

† Windows clamps the window to the desktop work area, so the swapchain is 25 px shorter than
asked for. `docs/SCENE.md` §8.3 records the same thing.

The four native-resolution points are **linear in pixels with a fixed offset** — not
super-linear:

```
GPU span (ms) = 3.2 + 8.8 x Mpx        (max residual 0.27 ms over 0.52 -> 2.03 Mpx)
```

Per stage, across the 3.9× pixel range from 960×540 to 1920×1055:

| stage | 960×540 | 1280×720 | 1600×900 | 1920×1055 | behaviour |
|---|---:|---:|---:|---:|---|
| ReStIR diffuse | 2.079 | 3.506 | 5.635 | 7.528 | **linear** (×3.62 for ×3.91 pixels) |
| primary visibility | 0.858 | 1.420 | 2.022 | 2.489 | sub-linear (×2.90) |
| sun shadow trace | 0.623 | 0.754 | 1.290 | 2.135 | **super-linear** (×3.43, and noisy) |
| ReStIR reflections | 0.666 | 1.221 | 1.693 | 2.269 | linear-ish (×3.41) |
| TAA | 0.624 | 1.050 | 1.608 | 2.224 | linear (×3.56) |
| irradiance cache | 0.989 | 1.027 | 1.018 | 1.026 | **flat** |
| sky and IBL | 0.323 | 0.312 | 0.334 | 0.315 | **flat** |
| particle simulation | 0.256 | 0.255 | 0.277 | 0.262 | **flat** |
| chunk generation | 0.057 | 0.053 | 0.043 | 0.044 | **flat** |

The four flat stages total **1.65 ms** and never move. They are half of the 3.2 ms intercept; the
rest is per-pass fixed overhead (dispatch setup, cache warm-up) spread across the 96 passes,
plus the 0.22–0.40 ms barrier gap, which is also flat.

**Half-resolution render with upscale.** `Render Res Scale 0.5` at a 1280×720 output renders at
640×360 and costs **5.264 ms — 2.13× faster than native 720p** — while TAA and post still run at
the output resolution. Compare `docs/images/profile/S1-island-still.png` with
`docs/images/profile/R3-720p-half.png`; **I opened both**. The grass reads slightly softer and
the conifer needles chunkier, and the dark ambient-occlusion pockets between grass blades are
less defined. Everything else — the amber bounce inside the cave, the sky gradient, the sun
terminator on the hill — is intact. It is visibly a lower-resolution image, not a different
renderer.

The practical reading: **1080p → 720p buys 1.87×; 720p native → 720p from a half-resolution
render buys another 2.13×.** The second is the better trade per unit of image quality, because
the ReStIR and SSAO stages are already at half of `render_resolution`
(`kajiya/rtdgi.inl:373`, `kajiya/rtr.inl:410`, `ssao.inl:77`), so halving it again puts them at
quarter resolution where they are cheapest, and leaves the two stages that most affect perceived
sharpness — TAA and post — at full output resolution.

### 4.3 Scaling with view distance — the counter-intuitive result

The brief asks about distant mountains. The answer is not what a rasteriser's intuition
predicts. Six poses, all 1280×720, all stock, all uncontended, **and every capture was opened
and looked at** (`docs/images/profile/`) — the "what is in it" column is what the image shows,
not what the pose was meant to give. Two of the six were originally named for what I expected and
had to be renamed after looking.

| frame | what is in it | GPU span | primary vis. | GI | sun shadow | `TracePrimary` | `RtdgiTrace` |
|---|---|---:|---:|---:|---:|---:|---:|
| rock wall 2 m away | rock fills the frame, no sky | 11.616 | **0.859** | **7.116** | 1.189 | **0.508** | **2.469** |
| steeply down at grass | grass and flowers, no horizon | 12.043 | 1.182 | 6.584 | 0.997 | 0.882 | 2.196 |
| spawn (baseline) | meadow, tree, hill, band of sea and sky | 11.216 | 1.420 | 6.015 | 0.754 | 1.075 | 1.903 |
| 22 m up, whole island | island, horizon, large area of sea | 7.638 | 2.037 | 2.741 | 0.344 | 1.443 | 0.545 |
| up at the horizon | sky and sea only | 6.683 | 1.992 | 2.151 | 0.152 | 1.418 | 0.226 |
| straight up | ~95 % sky, hilltop at the bottom edge | **6.814** | **2.112** | **2.197** | 0.130 | **1.626** | **0.178** |

Two things fall out, and they point in opposite directions:

1. **The GI stack is driven by how much near geometry is in frame, not by view distance.**
   `RtdgiTraceCompute` costs **2.47 ms** against a rock face and **0.18 ms** against open sky —
   a 14× swing across the same scene at the same resolution. This is the dominant term.
2. **Primary rays get *more* expensive the *less* they hit.** `TracePrimaryCompute` is
   **0.508 ms** facing a wall two metres away and **1.626 ms** facing the sky — **3.2× more for
   the emptier frame**. The reason is in `voxels/impl/trace.glsl:98`: the DDA loop runs until it hits
   a voxel (`lod == 0`), leaves the resident volume, or burns `MAX_STEPS` (512,
   `utilities/gpu/math.glsl:8`). `MAX_DIST` is `1.0e9`, so it is not a distance cutoff. A ray
   that hits nothing walks the entire volume at whatever LOD the empty space resolves to and
   then exits. **Empty space is not free; a miss is the most expensive primary ray in the
   engine.**

For "mountains in the distance" that is the whole story in one line: **the cost of the distance
is paid by the rays that travel it, whether or not there is a mountain at the end.** Growing the
world grows the number of DDA steps a sky ray takes before it escapes, and it does so for every
pixel of sky. This scene cannot be used to measure it further — the world is a 64 m cube
(`CHUNKS_PER_AXIS` 16, `voxels/impl/voxel_malloc.inl:20`) and nothing outside ±32 m exists at
any instant, so there is no such thing as a distant mountain to look at yet. **Whoever raises
`CHUNKS_PER_AXIS` must re-measure `TracePrimaryCompute` on a sky-heavy frame; it is the pass
that will move, and it will move on the frames with the *least* content.**

**The number that matters most for the 240 fps target is in the last row of that table.** A frame
containing essentially nothing — 95 % sky, one hilltop in the bottom edge, capture opened and
checked — still costs **6.81 ms, 147 fps**. That is not a content problem that a smaller scene or
better culling can fix. It is the standing cost of the pipeline on an almost empty g-buffer:
2.11 ms of primary DDA at full resolution, 1.02 ms of TAA, 0.66 ms of irradiance-cache
maintenance, 0.87 ms of ReStIR diffuse, 0.49 ms of ReStIR reflections, 0.32 ms of sky LUTs,
0.21 ms of particle simulation. **At 1280×720 native with this pipeline intact, 4.17 ms is not
reachable in an empty room.**

### 4.4 Anything pathological?

Six findings, strongest first. Only the first two are worth much.

1. **`TraceSecondaryCompute` — the sun shadow trace — runs at full render resolution and is
   not denoised.** `trace_secondary.inl:26` sizes the shadow mask at
   `gpu_context.render_resolution`, one shadow ray per full-resolution pixel, and
   `Graphics/denoise_shadow_mask` defaults to **false** (`kajiya/kajiya.hpp:23`), so the whole
   `ShadowBitPack` / `ShadowSpatialFilter` / `ShadowTemporalFilter` chain is never recorded into
   the graph. Cost: 0.75 ms stationary, 1.19 ms against a wall, **2.14 ms at 1080p** — where it
   is the third most expensive stage and where it grows *super*-linearly (×3.43 for ×3.91
   pixels; every other traced stage is linear or better). Every other ray-traced stage in this
   renderer is half resolution — ReStIR diffuse, ReStIR reflections and SSAO all run at
   `render_resolution / 2`. This one is the outlier, and the denoiser that would let it be half
   resolution is already in the tree, switched off. Switching shadows off entirely is worth
   **0.75 ms** (GPU span 5.264 → 4.516 ms in the GI-off configuration), so a half-resolution
   denoised shadow should be worth roughly 0.4 ms at 720p and 1.1 ms at 1080p **minus** the
   denoiser's own three passes, which have never been run in this tree and are therefore
   unmeasured. **This is the cheapest plausible win the profile exposes, and it is the one to
   try first.**

   **[Correction added 2026-07-31 by the performance-plan pass.** The denoiser is *not*
   unmeasured — `docs/design/RENDERER_OPTIMISATION.md` §6 and §8.3, written in parallel with this
   document, measured `Graphics/denoise_shadow_mask = true` at **+1.05 ms** (10.463 → 11.508 ms
   p50, un-profiled). It also runs at full `render_resolution`
   (`kajiya/shadow_denoiser.inl:102-130`), so switching it on does not buy a cheaper trace. Against
   a ~0.4 ms half-resolution trace saving at 720p that is a **net loss**, and this item is
   therefore not the cheapest win available. It may still pay at 1080p, where the trace saving is
   ~1.1 ms. See `docs/design/PERFORMANCE_PLAN.md` §6.2 item M1.**]**
2. **`RtdgiTraceCompute` is 17 % of the frame on its own** and runs at
   `(render_resolution + 1) / 2` (`kajiya/rtdgi.inl:373`). It is not mis-sized — it is simply
   the price of one ReStIR diffuse candidate ray per half-resolution pixel. At 1280×720 that is
   230 400 rays for 1.90 ms, about 8.2 ns per ray including the ircache lookups. Any plan that
   reaches 4.17 ms has to change what this pass does, not where it runs. **Note that it is
   already the pass that responds best to resolution**: it is the most exactly linear stage in
   the sweep (§4.2), so it is also the stage that a half-resolution render helps most —
   3.51 → 1.03 ms.
3. **Six of the twelve irradiance-cache cascades can never be populated.**
   `IRCACHE_GRID_CELL_DIAMETER` is 1/8 m and `IRCACHE_CASCADE_COUNT` is 12
   (`application/settings.inl:80-82`), so cascade *n* has a cell diameter of `0.125 × 2^n` and
   cascade 11 spans ±2048 m. The resident world is ±32 m. Cascades 6–11 are addressable, are
   allocated in `MAX_GRID_CELLS` (32³ × 12 = 393 216 cells), are scrolled every frame, and can
   never contain an entry. The *time* cost is small — `IrcacheScrollCascadesCompute` is
   0.025 ms — so this is a memory and clarity finding, not a frame-time one. It becomes a real
   one only if someone tries to buy performance by cutting cascades and is surprised by how
   little it returns.
4. **The blur pyramid runs nine `BlurCompute` dispatches for 0.13 ms total.** Nine passes, nine
   barriers, 0.0036–0.074 ms each. Not worth optimising, but it is nine of the 96 entries in the
   profile and it is why the pass count looks alarming.
5. **`RtrRestirResolveCompute` has a p99 of 4.23 ms against a mean of 0.41 ms** — a 10×
   outlier, seen in the stationary run only. Nothing else in the frame shows that shape. Not
   chased down; flagged because a rare 4 ms spike in one pass is the kind of thing that shows up
   later as an unexplained hitch.
6. **`ImGuiDraw` costs 0.0001 ms with the overlay off and is still recorded**, and the four
   particle types each contribute three raster passes whether or not they have any particles
   (`FireParticleSplatParticleRaster`, 0.0003 ms). Pure noise; listed so nobody reads the
   96-pass count as 96 pieces of real work. **Roughly 60 of the 96 passes cost under 0.05 ms
   each and total 0.6 ms.**

What is *not* pathological, checked and ruled out:

- **Barriers and layout transitions are 0.25–0.44 ms, 2.3–4.0 % of the frame.** The task graph
  is not over-synchronising. (This is the `gpu_gap` column: GPU span minus the sum of the
  scoped passes. It also contains the handful of tasks that are not individually scoped.)
- **Chunk generation is 0.05 ms standing still and 0.43 ms moving** — under 5 % even while
  walking. Nothing to win there.
- **The CPU is 1.0 ms against an 11.2 ms GPU frame**, confirmed independently by the 98.9 %
  span-to-wall ratio. The brief's "GPU-bound by roughly ten to one" is correct and now measured
  on the GPU rather than inferred from a CPU timer.

---

## 5. The floor

`Graphics/global_illumination` is a runtime checkbox that removes the whole GI stack from the
task graph — the pass count drops from **96 to 60** and the profiler's CSV loses every
`Ircache*`, `Rtdgi*`, `Rtr*`, `Ssao*` and `Downscale*` column. Same pose, same 1280×720, same
binary, uncontended, settings read back and confirmed.

| | GI on | GI off | delta |
|---|---:|---:|---:|
| primary visibility | 1.420 | 1.266 | −0.154 |
| sun shadow trace | 0.754 | 0.867 | +0.113 |
| irradiance cache | 1.027 | — | −1.027 |
| ReStIR diffuse | 3.506 | — | −3.506 |
| ReStIR reflections | 1.221 | — | −1.221 |
| SSAO | 0.261 | — | −0.261 |
| light composition | 0.171 | 0.164 | |
| TAA | 1.050 | 1.036 | |
| post / exposure | 0.172 | 0.182 | |
| particle sim | 0.255 | 0.262 | |
| particle raster | 0.544 | 0.529 | |
| chunk generation | 0.053 | 0.094 | |
| sky and IBL | 0.312 | 0.409 | +0.097 ‡ |
| reprojection | 0.122 | 0.124 | |
| barriers / unscoped | 0.341 | 0.325 | |
| **GPU span** | **11.216** | **5.264** | **−5.952** |
| **wall frame** | 11.340 | 5.438 | |
| **fps** | 88 | **184** | |

‡ `ConvolveCubeCompute` gets *more* expensive with GI off — 0.060 → 0.213 ms. That is deliberate:
`sky.inl` passes `do_global_illumination` into `push.flags` and the shader takes a different
branch. Everything else outside the GI stack is unchanged to within 0.15 ms, so the subtraction
is clean.

**Three conclusions, and the third is the one that matters.**

1. **The GI stack is 5.95 ms of an 11.22 ms frame — 53 %.** Two thirds of that is ReStIR
   diffuse.
2. **The floor with GI deleted is 5.26 ms — 190 fps.** That is the ceiling any GI-preserving
   optimisation can approach at 1280×720 native. It is *not* 240 fps.
3. **4.17 ms is not reachable at 1280×720 native on this GPU with this renderer, even with the
   entire path-traced GI stack removed.** The remaining 5.26 ms is primary DDA (1.27), TAA
   (1.04), shadow rays (0.87), particle raster (0.53), sky (0.41), barriers (0.33), particle sim
   (0.26), post (0.18), light composition (0.16), chunk gen (0.09), reprojection (0.12). There
   is no single pass to delete. Getting under 4.17 ms therefore requires **rendering fewer
   pixels**, not removing features.

And the fact that makes the whole trade curve interesting:

> **A half-resolution render with the full GI stack costs 5.264 ms. A full-resolution render with
> no GI at all costs 5.264 ms.** Same number, to three decimal places, from two independent runs.
> Halving the render resolution buys exactly as much as deleting every ReStIR, irradiance-cache
> and SSAO pass in the renderer — and it keeps the path-traced look, which is the entire reason
> for the pivot to this engine.

That is the knee. **The direction with a future is resolution, not features.**

### 5.1 How much further the switches go

Each row adds to the row above. All 1280×720, same pose, uncontended, settings read back.
**I opened and looked at every capture** (`docs/images/profile/`); the last column is what the
image actually shows, not what the switch is supposed to do.

| configuration | passes | GPU span | wall | fps | what the image looks like |
|---|---:|---:|---:|---:|---|
| stock | 96 | 11.216 | 11.340 | 88 | reference |
| `global_illumination` off | 60 | 5.264 | 5.438 | 184 | flat and bright: the grass loses its ambient-occlusion pockets, the rock dome loses all indirect shading, and the amber bounce inside the cave is gone — the crystals still emit but light nothing |
| + `Render Shadows` off | 59 | 4.516 | 4.653 | 215 | no sun shadows anywhere; the tree stops casting, the hill has no terminator |
| + `TAA Method` None + `Update Sky` off | 48 | **2.961** | 3.009 | **332** | **black sky, no sea**, hard voxel aliasing, uniformly lit — an unlit model viewer, not a game |

The 332 fps row is included because the brief asked for the floor and this is it. It is not a
configuration anyone would ship. Its 1.56 ms saving over the row above splits into **TAA
1.081 → 0.038 ms** and **sky 0.465 → 0.000 ms** — turning off `Update Sky` leaves the atmosphere
LUTs unwritten, so the sky and the sea render black. Read the **215 fps** row as the realistic
"every runtime switch off" number, and note that it is 4.65 ms — still not 4.17.

### 5.2 FSR 2.2 instead of the kajiya TAA

`Graphics/TAA Method = 2` replaces the seven-pass kajiya TAA with a single FSR2 dispatch — 96
passes become 90.

| | TAA stage | GPU span | wall | fps |
|---|---:|---:|---:|---:|
| kajiya TAA (stock) | 1.050 | 11.216 | 11.340 | 88 |
| FSR 2.2 | **0.658** | **10.446** | 10.677 | 93 |

**0.66 ms of wall, 6 %**, of which 0.39 ms is the antialiasing stage itself. Worth having, not a
strategy. Note this is FSR2 at 1:1 — the engine constructs it with
`render_resolution == output_resolution` unless `Render Res Scale` is lowered (`fsr.cpp:181`,
`renderer.cpp:181`), so this measures FSR2's *cost*, not FSR2 as an upscaler. **FSR2 driven from
a genuinely lower render resolution is the obvious next experiment and is not measured here.**

### 5.3 Run-to-run spread

Two runs of the identical stock configuration, minutes apart, both uncontended:

| | GPU span | wall |
|---|---:|---:|
| S1 | 11.216 | 11.340 |
| O1 | 11.488 | 11.506 |

**2.4 % on the span, 1.5 % on the wall time.** Treat anything under ~0.3 ms at this frame time as
noise.

*(Both rows are averaged over the profiler CSV's own window, t ≥ 7 s. §7's overhead A/B averages
over the `--bench-csv` window, t ≥ 5 s, and gets 11.588 ms for the same O1 run. The two windows
differ by about 0.1 ms; each comparison is internally consistent, so do not mix them.)*

---

## 6. Reproducing all of this

### 6.1 The profiler

Set one environment variable. Nothing else changes.

```powershell
$env:VOXL_GPU_PROFILE = 'C:\path\to\frame.csv'   # unset => not a single timestamp is written
$env:VOXL_GPU_PROFILE_START_S = '12'             # skip the first 12 s (default 4)
C:\voxl2\.out\cl-x86_64-windows-msvc\Release\gvox_engine.exe --unpause --exit-after 26 `
    --pos -182.99,-109.98,-46.97 --rot 0.785,1.096 --no-overlay --width 1280 --height 720
```

The CSV is one row per frame and one column per pass, in submission order:

```
frame,t_s,prev_frame_ms,gpu_span_ms,gpu_sum_ms,gpu_gap_ms,SkyTransmittanceCompute,...,ImGuiDraw
```

`gpu_span_ms` is last-close minus first-open. `gpu_sum_ms` is the sum of the individual passes.
`gpu_gap_ms` is the difference: barriers, layout transitions and the few tasks that carry no
scope. `prev_frame_ms` is the engine's own `delta_time` for the *previous* frame, present only
so a row can be lined up against `--bench-csv`.

### 6.2 A controlled run

Every number in this document came out of this. It is reproduced in full rather than referenced,
because the original lives in a session scratchpad that will not survive.

```powershell
# --- controlled-run.ps1 ---------------------------------------------------------------------
param([string]$Name, [string]$Pos, [string]$Rot, [double]$Converge = 12, [double]$Seconds = 26,
      [uint32]$Width = 1280, [uint32]$Height = 720, [string[]]$Set = @(),
      [string]$Exe = 'C:\voxl2\.out\cl-x86_64-windows-msvc\Release\gvox_engine.exe',
      [string]$OutDir = '.\out', [string]$Stock = '.\stock_settings.json')
$ErrorActionPreference = 'Stop'
$inv = [Globalization.CultureInfo]::InvariantCulture
$cfg = Join-Path $env:APPDATA 'GabeVoxelGame\user_settings.json'
New-Item -ItemType Directory -Force $OutDir | Out-Null
$prof  = "$OutDir\$Name.prof.csv"; $bench = "$OutDir\$Name.bench.csv"; $apps = "$OutDir\$Name.apps.txt"

# 1. exclusivity -- a sibling instance roughly doubles the frame time
while (@(Get-Process gvox_engine -EA SilentlyContinue).Count -gt 0) { Start-Sleep 2 }
Start-Sleep -Milliseconds 400

# 2. a known settings file; retried, because a sibling often holds it open
$j = Get-Content $Stock -Raw | ConvertFrom-Json
foreach ($s in $Set) {
    $k,$v = $s -split '=',2; $cat,$key = $k -split '/',2
    $n = $j.categories.$cat.$key
    if ($v -in 'true','false')      { $n.data.setting.value = [bool]::Parse($v) }
    elseif ($v -match '^-?\d+$')    { $n.data.setting.value = [int]$v }
    else                            { $n.data.setting.value = [double]::Parse($v,$inv) }
}
$json = $j | ConvertTo-Json -Depth 40
for ($i=0; $i -lt 40; $i++) { try { Set-Content $cfg $json -Encoding utf8 -EA Stop; break } catch { Start-Sleep -m 250 } }

# 3. contention sampler. Integer milliseconds and a semicolon: a culture-sensitive "{0:F2}"
#    writes "0,04" on a pt-BR machine and every line then parses wrong.
$w = Start-Process powershell -PassThru -WindowStyle Hidden -ArgumentList @('-NoProfile','-Command',
  "`$sw=[Diagnostics.Stopwatch]::StartNew(); [IO.File]::WriteAllText('$apps','');
   while(`$sw.Elapsed.TotalSeconds -lt $($Seconds+60)){
     `$n=@(Get-Process gvox_engine -EA SilentlyContinue).Count
     [IO.File]::AppendAllText('$apps',(('{0};{1}' -f [int]`$sw.ElapsedMilliseconds,`$n)+[Environment]::NewLine))
     Start-Sleep -Milliseconds 250 }")

# 4. run
$env:VOXL_GPU_PROFILE = $prof; $env:VOXL_GPU_PROFILE_START_S = $Converge.ToString('0.###',$inv)
$a = @('--unpause','--exit-after',$Seconds.ToString('0.###',$inv),'--width',$Width,'--height',$Height,
       '--no-overlay','--bench-csv',$bench)
if ($Pos) { $a += @('--pos',$Pos) }; if ($Rot) { $a += @('--rot',$Rot) }
$p = Start-Process $Exe -ArgumentList $a -WorkingDirectory (Split-Path $Exe) -PassThru -NoNewWindow
$null = $p.Handle
Start-Sleep 3                                    # the engine has read the settings by now
$used = (Get-Content $cfg -Raw | ConvertFrom-Json).categories.Graphics
$null = $p.WaitForExit(([int]$Seconds + 300) * 1000)
Remove-Item Env:\VOXL_GPU_PROFILE; Start-Sleep -m 400; try { $w.Kill() } catch {}

# 5. accept or reject
$s = @(Get-Content $apps | ForEach-Object { ($_ -split ';') } | Where-Object { $_ }); $tot=0; $bad=0
foreach ($line in (Get-Content $apps)) { $f = $line -split ';'; if ($f.Count -ge 2) { $tot++; if ([int]$f[1] -gt 1) { $bad++ } } }
[pscustomobject]@{
    name = $Name; exit = $p.ExitCode
    shared_gpu_pct = [math]::Round(100.0*$bad/[math]::Max($tot,1),1)
    gi = $used.global_illumination.data.setting.value
    render_res_scale = $used.'Render Res Scale'.data.setting.value
    taa = $used.'TAA Method'.data.setting.value
    shadows = $used.'Render Shadows'.data.setting.value
} | Format-List
# reject the run if shared_gpu_pct > 2, or if any of the settings above is not what was asked for
```

`stock_settings.json` is a full settings file with the six `Graphics` keys pinned to the values
in the engine source (`kajiya/kajiya.hpp:22-23`, `trace_secondary.inl:30`, `renderer.cpp:30-31`,
`voxel_app.cpp:57`): `global_illumination` true, `denoise_shadow_mask` false, `Render Shadows`
true, `TAA Method` 1, `Update Sky` true, `Render Res Scale` 1.0. **Do not build it by copying the
live file and trusting its `user_default` fields** — a sibling agent had written 0.5 into *both*
`data` and `user_default` for `Render Res Scale`, and a template built that way silently produced
a 5.2 ms "baseline".

Deleting `user_settings.json` instead of writing a template does give factory defaults, but the
file the engine then writes contains only the settings registered before `AppUi` is constructed —
the whole `Graphics` category is registered later, in `record_tasks()` — so there is nothing to
patch and nothing to read back.

### 6.3 Poses used

Scene-local metres (`docs/SCENE.md` §1: `local = absolute + (183, 110, 52.5)`).

| name | local pos | rot (yaw, pitch) | what is in frame |
|---|---|---|---|
| island still | 0.01, 0.02, 5.53 | 0.785, 1.096 | meadow, tree, hill, cave portal, sea band |
| island moving | 6, 6, 5.5 + `--patrol 13,15` | 0, 1.45 | one 13 m circle every 15 s |
| rock wall | 11, 11, 4.5 | 0.785, 1.571 | rock face fills the frame |
| elevated vista | −14, −14, 22 | 0.785, 1.75 | sky and sea, island out of frame low |
| island from above | −14, −14, 22 | 0.785, 1.00 | the whole island, horizon, sea |
| steeply down | 0.01, 0.02, 5.53 | 0.785, 0.80 | grass and flowers fill the frame |

**Correction to `docs/SCENE.md` §3:** that document says pitch "1.571 is level, smaller looks
up". It is the other way round — **smaller looks down**. Pitch 0.80 frames the grass at the
camera's feet and 1.75 frames the sky; both were checked against the captured images. The
startup pitch of 1.096 is 27° *below* the horizon, which is consistent with the rest of that
section and with what the spawn frame actually shows.

---

## 7. How the measurement works, and what it does not measure

`src/utilities/gpu_profiler.*` owns a ring of four `daxa::TimelineQueryPool`s of 2048 queries
each. Every profiled scope writes a **BOTTOM_OF_PIPE** timestamp before and after the task's own
commands.

**Why BOTTOM_OF_PIPE at both ends.** The task graph records its pipeline barriers *between* task
callbacks, so a scope's opening timestamp only resolves once the preceding barrier has. Barrier
and layout-transition time therefore falls in the gap between scopes rather than being charged
to whichever pass happens to follow it. That is the `gpu_gap_ms` column, and it is reported
rather than hidden.

**Readback is three frames late.** `end_frame()` reads the pool written at `frame − 3`, and
every read checks the per-query availability bit. A same-frame readback would stall the CPU on
the GPU and destroy the number being measured. Across every run in this document, **zero** rows
were dropped as unavailable.

**Coverage.** 96 scopes at 1280×720 with stock settings. Every task that goes through
`GpuContext::add()` is scoped automatically (`utilities/gpu_task.hpp`), which is 94 of the call
sites in `src/`; `GpuInputUpload`, `GpuOutputDownload`, `ImGuiDraw`, `UpscaleBlit` and `FSR2`
are scoped by hand. A handful of small inline tasks (the histogram clear/copy, the ircache and
RTR lookup-table uploads) are not scoped and land in `gpu_gap_ms`.

**What it does not measure.** `vkQueuePresent` and swapchain acquisition; anything the driver
does between submits; occupancy, bandwidth or cache behaviour inside a pass. It tells you *which*
pass is expensive, not *why*. For the why, the next step is Nsight Graphics on
`RtdgiTraceCompute` specifically.

**Caveat on overlapping dispatches.** Consecutive tasks with no dependency between them can
overlap on the GPU, in which case the two scopes both include the other's work and the sum
over-counts. In this graph almost every pass consumes the previous one's output, so the
serialisation is real; the check is that `gpu_sum_ms` stays *below* `gpu_span_ms` in every run
(96–97 % of it), which it does. If a future change makes `gpu_sum` exceed `gpu_span`, that is
overlap and the per-pass numbers stop being additive.

---

## 8. Every run in this document

`controlled-run.ps1` is §6.2. All poses are `-Local` values from §6.3 converted with
`--pos = local − (183, 110, 52.5)`; the spawn pose is `--pos -182.99,-109.98,-46.97 --rot
0.785,1.096`.

```powershell
powershell -File C:\voxl2\tools\build.ps1     # gpu_profiler.cpp is in the target now

# --- section 3: the three scenarios -----------------------------------------------------------
.\controlled-run.ps1 -Name S1-island-still  -Pos -182.99,-109.98,-46.97 -Rot 0.785,1.096
.\controlled-run.ps1 -Name S2-island-moving -Pos -177,-104,-47 -Rot 0,1.45   # plus --patrol 13,15
.\controlled-run.ps1 -Name D1-demo-still    -Exe C:\voxl2_demo\bin\gvox_engine.exe   # no -Pos: default spawn

# --- section 4.2: resolution --------------------------------------------------------------------
.\controlled-run.ps1 -Name R5-960x540   -Width  960 -Height  540 -Pos ... -Rot ...
.\controlled-run.ps1 -Name R1-1600x900  -Width 1600 -Height  900 -Pos ... -Rot ...
.\controlled-run.ps1 -Name R2-1920x1080 -Width 1920 -Height 1080 -Pos ... -Rot ...
.\controlled-run.ps1 -Name R3-720p-half -Set 'Graphics/Render Res Scale=0.5' -Pos ... -Rot ...

# --- section 4.3: view distance -------------------------------------------------------------------
.\controlled-run.ps1 -Name V1-wall        -Pos -172,-99,-48    -Rot 0.785,1.571
.\controlled-run.ps1 -Name V2-vista       -Pos -197,-124,-30.5 -Rot 0.785,1.75
.\controlled-run.ps1 -Name V3-ground      -Pos -182.99,-109.98,-46.97 -Rot 0.785,0.80
.\controlled-run.ps1 -Name V4-island-down -Pos -197,-124,-30.5 -Rot 0.785,1.00
.\controlled-run.ps1 -Name V5-sky-up      -Pos -182.99,-109.98,-46.97 -Rot 0.785,2.30

# --- section 5: the floor --------------------------------------------------------------------------
.\controlled-run.ps1 -Name F1-no-gi        -Set 'Graphics/global_illumination=false' -Pos ... -Rot ...
.\controlled-run.ps1 -Name F2-no-gi-no-sh  -Set 'Graphics/global_illumination=false','Graphics/Render Shadows=false' -Pos ... -Rot ...
.\controlled-run.ps1 -Name F3-everything-off -Set 'Graphics/global_illumination=false','Graphics/Render Shadows=false','Graphics/TAA Method=0','Graphics/Update Sky=false' -Pos ... -Rot ...
.\controlled-run.ps1 -Name T1-fsr2         -Set 'Graphics/TAA Method=2' -Pos ... -Rot ...

# --- section 7: profiler overhead --------------------------------------------------------------------
.\controlled-run.ps1 -Name O1-prof-on  -Pos ... -Rot ...
$env:VOXL_GPU_PROFILE = $null   # then the same run again, with the profiler off
```

### 8.1 The raw data

**`docs/benchmarks/profile-2026-07-31.csv`** holds the per-pass reduction of all 17 runs quoted
here — every one of them uncontended and with its settings read back — so the tables above can be
checked without re-running anything:

```
run,pass,submission_order,mean_ms,p50_ms,p99_ms,frames
```

`submission_order` is the pass's index in the frame graph (−1 for the four synthetic rows
`__wall_frame__`, `__gpu_span__`, `__gpu_sum__`, `__gpu_gap__`). All rows are averaged over
t ≥ 7 s. 1572 rows, 100 kB. The per-frame CSVs behind it are ~2 MB each and were not committed.

Reduce a `.prof.csv` with any tool that can average columns. The stage groupings used in this
document are the pass names listed in §3.1 and the submission order in the CSV header; the
mapping is: `Sky*`/`ConvolveCube` → sky; `TraceDepthPrepass`/`TracePrimary`/`R32D32Blit` →
primary visibility; `TraceSecondary` → sun shadow; `Ircache*`/`PrefixScan*`/`AgeIrcacheEntries`/
`TraceIrradiance`/`SumUpIrradiance` → irradiance cache; `Rtdgi*` → ReStIR diffuse; `Rtr*` →
ReStIR reflections; `Ssao*`/`Downscale*` → SSAO; `Taa*`/`FSR2`/`UpscaleBlit` → TAA;
`Blur*`/`CalculateHistogram`/`PostprocessingRaster` → post; `*Particle*`/`GrassStrand*`/`Flower*`
→ particles; `Chunk*`/`PerChunk`/`VoxelWorldPerframe` → chunk generation.

---

## 9. What was changed in the tree to make this possible

Small and reversible: **22 inserted lines across five existing files**, plus two new sources and
two new documents. `git diff --stat` on the shared tree is the whole of it.

| file | change |
|---|---|
| `src/utilities/gpu_profiler.hpp` | **new** — the public surface, 70 lines |
| `src/utilities/gpu_profiler.cpp` | **new** — ring of query pools, deferred readback, CSV writer |
| `CMakeLists.txt` | +1: add `src/utilities/gpu_profiler.cpp` to the target |
| `src/utilities/gpu_task.hpp` | +5: one `#include`, one `gpu_profiler::Scope` in each of the two `Task::callback` overloads. **This one line is what scopes 94 of the 96 passes.** |
| `src/voxel_app.cpp` | +13: `begin_frame` / `end_frame` / `shutdown`, one `#include`, and three named scopes on inline tasks |
| `src/renderer/renderer.cpp` | +1: a named scope on the `UpscaleBlit` task |
| `src/renderer/fsr.cpp` | +1: a named scope on the `FSR2` task |
| `docs/PROFILE.md` | **new** — this file |
| `docs/benchmarks/profile-2026-07-31.csv` | **new** — the per-pass data behind it, 17 runs |
| `docs/images/profile/*.png` | **new** — the eight captures this document describes, 4.2 MB |

Two trees outside `C:\voxl2` were created for measurement hygiene and can be deleted:
`C:\voxl2_prof` (the built binary plus a private copy of `assets/` and `src/`, so a sibling
agent's rebuild or shader edit cannot change the binary or the SPIR-V cache underneath a run) and
`C:\voxl2_demo` (the same, with `VOXL_TEST_SCENE 0`). 28 MB each. `src/main.cpp:35` walks up from
the executable looking for `.out` or `assets`, which is what makes a self-contained tree work.

**Overhead when the profiler is off.** When `VOXL_GPU_PROFILE` is unset, `scope_begin` returns after
one bool test and no Vulkan call is made. Measured A/B, same binary, same pose, both runs
uncontended and stock:

| | wall frame | fps |
|---|---:|---:|
| profiler on (192 timestamps per frame) | 11.588 ms | 86.3 |
| profiler off | **10.690 ms** | **93.5** |

**The instrumentation costs 0.90 ms, 8.4 %.** That is not the cost of 192 `vkCmdWriteTimestamp`
calls; it is mostly the serialisation they impose. A BOTTOM_OF_PIPE timestamp between two
independent dispatches prevents them overlapping, so a profiled frame is a slightly more
serialised frame than the real one.

**What that means for every number in this document.** The per-pass times are honest times for
those passes when they are not overlapped, and their *relative* sizes — which is what the
breakdown is for — are unaffected. But the absolute frame times measured with the profiler on
are about 8 % pessimistic, and roughly 0.9 ms of inter-pass overlap exists in the real frame that
the profiler suppresses. **Quote 10.69 ms / 93 fps as the un-instrumented stationary frame time,
and 11.22 ms as the profiled frame the breakdown adds up to.** `docs/SCENE.md` §8.1's 11.13 ms
and `docs/BASELINE.md`'s figures were taken with the overlay on, which costs a little more again;
all three agree to within a few per cent.

**Not measured:** the instrumented-but-disabled binary against the *pre*-instrumentation binary.
The disabled path is one `enabled()` bool test per task, about 110 per frame on the CPU, and the
disabled run's `cpu_ms` of 1.009 ms sits inside the 0.94–1.02 ms range `docs/SCENE.md` and
`docs/BASELINE.md` record for the same scene before any of this existed — so there is no
detectable cost — but it was not A/B'd directly and should not be claimed as zero.

**Verified from the shared tree**, not only from the private measurement copy: the command in
§6.1 run against `C:\voxl2\.out\cl-x86_64-windows-msvc\Release\gvox_engine.exe` writes 1144 rows
of 102 columns (6 header fields + 96 passes), exit code 0, zero rows dropped.

---

## 10. What this document does not establish

Stated plainly, so nobody cites a number that was not measured.

1. **The demo world is not pose-matched to the Voxl scene.** It is measured at the engine's own
   default spawn, because the Voxl coordinates frame something unrelated in that terrain. The
   15 % difference in §3.2 is therefore a scene comparison, not a controlled A/B of one variable.
   `docs/SCENE.md` §8.1's 12.83 versus 11.13 ms is the pose-matched pair; it has no per-pass
   breakdown.
2. **1080p with a half-resolution render.** Attempted four times; every attempt shared the GPU
   with another 6 GB-class instance and read 8.5–500 ms. The 1080p VRAM footprint (2262–4200 MiB
   measured) does not leave room for two instances on a 6 GB part, and the results were
   worthless. The four other points on the resolution curve are clean.
3. **Why `RtdgiTraceCompute` costs what it costs.** The profiler says which pass, not why. It is
   1.90 ms for 230 400 half-resolution rays — 8.2 ns each including the irradiance-cache
   lookups — but whether that is bound by the DDA's memory traffic, by divergence, or by
   occupancy is not established. Nsight Graphics on that one pass is the obvious next step and
   would change what any optimisation proposal should attempt.
4. **`RtrRestirResolveCompute`'s 4.23 ms p99** against a 0.41 ms mean, seen in the stationary run
   only. Not reproduced, not explained.
5. **Overlap.** The per-pass numbers assume consecutive passes do not overlap on the GPU. The
   evidence that they mostly do not is that the sum of the passes stays at 96–97 % of the frame
   span in every run. The evidence that they *partly* do is the 0.90 ms the instrumentation costs
   (§7). Treat individual sub-0.1 ms passes as indicative rather than exact.
6. **Long sessions and thermals.** The longest run here was 80 s. Clocks held 1897–1935 MHz and
   temperature peaked at 78 °C, so nothing throttled, but a 10-minute session was not tried.
7. **The 240 fps question at resolutions below 960×540**, and FSR 2.2 driven from a genuinely
   lower render resolution. Both are cheap experiments and neither was run.
