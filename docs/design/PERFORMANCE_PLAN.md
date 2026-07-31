# The performance plan

**Written:** 2026-07-31
**Synthesises:** `docs/PROFILE.md` (per-pass GPU timings), `docs/design/RENDERER_OPTIMISATION.md`
(the settings and resolution curve), `docs/design/WORLD_SCALE.md` (world extent and the march),
`docs/design/PRIOR_ART_PERFORMANCE.md` (what anyone else achieves), and `docs/BASELINE.md` /
`docs/SCENE.md`.
**Hardware:** RTX 3050 6 GB Laptop (driver 610.74), i7-13650HX, 16 GB, Windows 11 Pro 26200.
**GPU question:** settled four independent ways — nvidia-smi compute-apps table, `utilization.gpu`
25 % → 100 %, `vulkaninfo`, and the engine's own overlay in four separate captures. **It is the
discrete RTX 3050, not the Intel iGPU.** No measurement in this project is invalidated by hybrid
graphics.

This document creates no new measurements. Every number in it is traceable to one of the four
research documents; each is tagged with its source and with whether the profiler was running.
Where two documents disagree, §2.4 says which one to believe and why.

---

## 1. The verdict on 240 fps

**240 fps is reachable today with the path-traced GI fully intact — measured at 4.538 ms / 220 fps
standing still and 3.720 ms / 269 fps moving — but only at 640×360 internal upscaled to 1280×720,
and only on the 37 m island, which has an empty sea horizon and no mountains in it**
(`RENDERER_OPTIMISATION.md` §10). **At native 1280×720 it is arithmetically out of reach and always
will be: deleting the entire GI stack leaves 4.900 ms (204 fps), and a frame containing no voxel
geometry at all — sky and sea only — still costs 5.846 ms (171 fps), both above the 4.17 ms
budget.** **The half of the target that cannot yet be answered is the other half: there are no
mountains in any of those numbers, the world is a 64 m cube with nowhere to put one, and the cost
of a far field has never been measured because no far field exists.**

### 1.1 What is and is not reachable, in one table

| question | answer | the number that settles it |
|---|---|---|
| 240 fps at **native 1280×720**, GI intact? | **No, and not by any tuning** | GI deleted entirely: **4.900 ms / 204 fps** (R §6). Empty frame, no voxels at all: **5.846 ms / 171 fps** (R §9.1) |
| 240 fps at **native 1280×720**, GI deleted, shadows deleted, TAA deleted? | Only by also blanking the sky | 4.653 ms / 215 fps with GI+shadows off; 3.009 ms / 332 fps only once TAA and sky are off too, which renders **black sky, no sea** (P §5.1) |
| 240 fps at **1280×720 output, 640×360 internal**, GI intact? | **Yes, measured** | 4.877 ms / 205 fps at 0.50 alone; **4.538 ms / 220 fps** with reflections off; **3.720 ms / 269 fps** moving (R §4, §10) |
| 240 fps at **1920×1055 output**, any internal resolution? | **No** | The output-resolution floor at 1080p is **~3.5 ms**, but the best measured point is 7.666 ms / 130 fps at 0.50 internal (R §5.2). Nothing measured comes within 3 ms of 4.17 |
| Any frame rate above **330 fps**, ever? | **No** | Fixed floor 3.030 ms, independent of internal resolution, fitted across 8 points from 512×288 to 2560×1440 (R §4) |
| 240 fps **with distant mountains** | **Not established; the arithmetic says no** | §5 below budgets 5.4 ms / 185 fps at 0.50 internal with a 2 km far field, ±30 %. That is arithmetic on measured components, not a measurement |

### 1.2 What to actually target

Three defensible targets, in the order I would offer them:

| tier | configuration | measured today (37 m island) | with a 2 km far field (**estimated**, §5.4) |
|---|---|---|---|
| **Quality** | 1920×1055 output, 1440×791 internal, FSR 2.2, reflections off | **10.922 ms · 92 fps** (R §5.2) | ~12.5–14 ms · 71–80 fps |
| **Balanced** — the one to build toward | 1280×720 output, 960×540 internal (0.75), reflections off | **7.097 ms · 141 fps** still, 5.475 ms · 183 fps moving, 5.923 ms · 169 fps in the cave (R §10) | ~8.5 ms · 118 fps |
| **Performance** | 1280×720 output, 640×360 internal (0.50), reflections off | **4.538 ms · 220 fps** still, 3.720 ms · 269 fps moving, 4.290 ms · 233 fps cave (R §10) | ~5.4 ms · 185 fps |

**The honest recommendation is the Balanced tier at 120 fps, not 240 fps.** It survives a far field
with margin, it survives the cave (the worst frame in the scene), it survives motion, and
`docs/images/renderer/R10-native-vs-presets.png` — which I opened — shows it is genuinely hard to
distinguish from native at viewing distance. The Performance tier reaches 240 fps today and is
worth shipping as an explicit option, but §6.3 shows what it does to grass in motion and I would
not make it the default.

**If the user wants a 240 Hz display filled, the route is FSR 3 frame generation from ~120 real
fps** (`PRIOR_ART_PERFORMANCE.md` §5.7; measured elsewhere at +67 % frame rate on hardware-agnostic
FSR 3, with ~18 ms worse latency). That is what the industry does — NVIDIA's own answer to "4K
240 Hz path tracing" is DLSS 4.5 Multi Frame Generation 6×, one rendered frame in six. **It is 240
displayed frames at 120 fps of input responsiveness, and the user should choose that knowingly.**

### 1.3 Why no amount of cleverness changes the native-720p answer

The arithmetic is closed, and this is the single most important paragraph in the document.

```
frame_ms = 3.030 + 7.520 × (internal pixels / 921600)          R §4, 8 points, max residual 0.44 ms
```

Solve for 4.17 ms: internal scale **0.389**, i.e. **498×280**. There is no configuration of
features that reaches 240 fps at 1280×720 internal, because the slope term alone at 1280×720 is
7.52 ms and the whole budget is 4.17 ms. **Every optimisation that removes a feature is competing
for a 7.52 ms pool that resolution takes at a stroke.** The proof is the coincidence the profiling
agent found and it deserves quoting:

> A half-resolution render with the **full** GI stack costs **5.264 ms**. A full-resolution render
> with **no GI at all** costs **5.264 ms**. Same number to three decimal places, two independent
> runs. (P §5)

Halving the render resolution buys exactly as much as deleting every ReSTIR, irradiance-cache and
SSAO pass in the renderer — and it keeps the look, which is the entire reason for the pivot to this
engine.

### 1.4 The strategic consequence nobody's document could see alone

Once you take the resolution lever, **every per-pixel feature saving shrinks by the same factor,
and the fixed floor becomes the dominant term.** This is measured, not argued:

| saving | at 1280×720 internal | at 640×360 internal |
|---|---:|---:|
| reflections off | **0.98 ms** (R §7.2) | **0.339 ms** (4.877 → 4.538, R §10) |
| software VRS on GI traces (*estimated*) | ~0.7–0.9 ms | ~0.2 ms |
| half-resolution sun shadows (*estimated*) | ~0.4 ms | ~0.1 ms |
| **fixed floor, unmoved** | 3.030 ms = **29 %** of the frame | 3.030 ms = **62 %** of the frame |

**After the resolution lever, the target is the 3.03 ms fixed floor, not the per-pixel passes.**
Each of the four research documents measured at native resolution, where the floor's components
each look like noise; at the frame time we are actually aiming for, they are the largest remaining
pool. §6 ranks them accordingly, and this reordering is the main thing this plan adds to the four
documents underneath it.

---

## 2. The frame breakdown

### 2.1 Where the 10.9 ms goes

1280×720 native, stock settings, Voxl island at the spawn, stationary, uncontended, 1235 settled
frames. GPU timestamps, one pair per pass, read back three frames late (P §3.1). The profiler
itself costs 0.90 ms of serialisation, so **this frame sums to 11.216 ms while the same frame
un-instrumented is 10.690 ms** — the *proportions* are what to read.

| stage | ms | % | scales with |
|---|---:|---:|---|
| **ReStIR diffuse (rtdgi)** | **3.506** | **31.3** | internal pixels — the most exactly linear stage in the renderer |
| primary visibility (prepass + primary trace + blit) | 1.420 | 12.7 | internal pixels, sub-linearly |
| ReStIR reflections (rtr) | 1.221 | 10.9 | internal pixels |
| TAA | 1.050 | 9.4 | **output** pixels |
| irradiance cache | 1.027 | 9.2 | **nothing** — probe count, indirect dispatch |
| sun shadow trace | 0.754 | 6.7 | internal pixels, **super**-linearly |
| particle raster (grass, flowers, mushrooms) | 0.544 | 4.8 | live particle count |
| barriers / layout transitions / unscoped | 0.341 | 3.0 | pass count |
| sky and IBL cube | 0.312 | 2.8 | **nothing** |
| SSAO | 0.261 | 2.3 | internal pixels |
| particle simulation | 0.255 | 2.3 | **nothing** — 1 048 576 slots dispatched every frame regardless of live count |
| post and exposure | 0.172 | 1.5 | output pixels |
| light composition | 0.171 | 1.5 | internal pixels |
| reprojection map | 0.122 | 1.1 | internal pixels |
| chunk generation | 0.053 | 0.5 | **nothing** standing still; 0.425 ms moving |
| UI and buffer upload/download | 0.007 | 0.1 | — |
| **total GPU span** | **11.216** | **100.0** | |

Independently measured by a second agent with the same instrument at the same pose
(`RENDERER_OPTIMISATION.md` §3): RTDGI 3.444, primary 1.479, RTR 1.074, TAA 1.011, ircache 0.952,
shadow 0.808, particles 0.738, SSAO 0.254, sky 0.258, post 0.169. **Every stage agrees within
0.08 ms.** The breakdown is not in doubt.

**Top three passes**, unchanged across every scenario and all three the same thing under the hood —
a hierarchical DDA march through `src/voxels/impl/trace.glsl`:

| pass | mean ms | % of frame |
|---|---:|---:|
| `RtdgiTraceCompute` | 1.903 | **17.0** |
| `TracePrimaryCompute` | 1.075 | **9.6** |
| `TraceSecondaryCompute` (sun shadows) | 0.754 | **6.7** |

Roughly 60 of the 96 passes cost under 0.05 ms each and total 0.6 ms.

### 2.2 Grouped four ways

| | ms | % |
|---|---:|---:|
| **GI** (irradiance cache + ReStIR diffuse + ReStIR reflections + SSAO) | **6.014** | **53.6** |
| primary visibility | 1.420 | 12.7 |
| sun shadows | 0.754 | 6.7 |
| everything else (TAA, post, particles, sky, chunk gen, barriers) | 3.028 | 27.0 |

GI ranges from **32 %** of the frame (nothing but sky and sea) to **61 %** (a rock face two metres
away). Two thirds of the GI is ReStIR diffuse.

### 2.3 The frame is GPU-bound end to end, measured on the GPU

| scenario | wall (ms) | GPU span (ms) | span / wall |
|---|---:|---:|---:|
| island, stationary | 11.340 | 11.216 | **98.9 %** |
| island, moving | 8.533 | 8.424 | 98.7 % |
| demo world | 13.063 | 12.732 | 97.5 % |
| elevated, mostly sky | 6.725 | 6.683 | 99.4 % |

The brief's "GPU-bound roughly ten to one" was inferred from a CPU timer; it is now measured on the
GPU and it is correct. **Two consequences.** First, CPU optimisation is worth at most 0.12 ms and
the brief's instruction to ignore it stands. Second — and this closes an open flag from
`RENDERER_OPTIMISATION.md` §11 — **`FRAMES_IN_FLIGHT 1` is not costing anything measurable.** The
worry was that at a 4 ms target, 1.0 ms of CPU would stop overlapping. But the wall-minus-span gap
is **0.071 ms at a 5.335 ms frame** (P §4.2, the half-resolution row), so the CPU is still fully
overlapped at the frame time we are targeting. **De-prioritise it**; re-check only if a
configuration below 3.7 ms is ever pursued.

### 2.4 Where the four documents disagree, and which to believe

The two per-pass documents were produced with different harnesses. Their differences are systematic
and understanding them is a prerequisite for setting acceptance thresholds.

| quantity | PROFILE.md | RENDERER_OPTIMISATION.md | resolution |
|---|---:|---:|---|
| stock 1280×720 stationary | 11.216 span / 11.340 wall (profiled); **10.690 un-instrumented** | **10.463** p50 | Both right. Profiler overhead is 0.90 ms; the overlay is another 0.46 ms; the brief's 10.93 ms includes the overlay. **Quote 10.5–10.9 ms as the baseline** and never mix a profiled with an un-profiled number |
| `Render Res Scale` 0.5 | 5.264 span / 5.335 wall | **4.877** p50 | Same 0.39 ms profiler delta. Consistent |
| GI off | 5.264 span / 5.438 wall | **4.900** p50 | Consistent |
| **sun shadows off** | −0.748 ms | **−0.28 ms** | **Disagree.** P deletes the pass and charges the profiled 0.75 ms; R deletes it and the wall clock moves 0.28 ms. The difference is inter-pass overlap the profiler suppresses. **Believe R** |
| **FSR 2.2 vs kajiya TAA at 1:1** | −0.663 ms wall | **+0.03 ms** | **Disagree beyond the noise floor and unexplained.** Believe R (un-profiled A/B) and treat FSR2-vs-TAA as **cost-neutral at 1:1**. FSR2's value is as an *upscaler*, where R §5.2 measures it clearly |
| `denoise_shadow_mask` | "never been run in this tree and is therefore unmeasured" (P §4.4) | **measured: +1.05 ms** (R §6, §8.3) | **P is out of date.** See §6.2 item M1 — this changes P's "cheapest plausible win" into a marginal one |

**The rule that falls out of this, and it governs every acceptance threshold in §6:**

> **A per-pass number from the profiler is an upper bound on what deleting that pass saves.
> Measured deletions run between 37 % (shadows) and 100 % (reflections, GI stack) of the profiled
> pass cost. Never accept a profiler number as a saving. Require a wall-clock deletion A/B with the
> profiler off.**

### 2.5 The three traps that corrupted measurements for three different agents

All three are documented independently in three of the four research documents. They are not
hypothetical; each cost a day.

1. **`%APPDATA%\GabeVoxelGame\user_settings.json` is shared mutable state across every build on
   this machine.** `AppUi` derives its data directory from `sago::getDataHome()`
   (`src/application/ui.cpp:87`), which uses the Win32 known-folder API and **ignores `%APPDATA%`**
   — so `C:\voxl2`, `C:\voxl2_prof`, `C:\voxl2rs` and `C:\voxl2_ws` all read and write the same
   file, and the settings UI rewrites it on every change (`ui.cpp:395`, `:772`). One agent's first
   profiled run read 5.06 ms instead of 11.34 because a sibling had left
   `global_illumination = false`. Another's headline result **reversed sign**. **The fix is item F1
   in §6.1 and it is the first thing to do.**
2. **A second engine instance roughly doubles the frame time** (26 ms vs 11), and two 6 GB-class
   instances at 1080p thrash VRAM into 500 ms frames. Before/after process checks miss it; sample
   continuously and reject on any shared sample.
3. **A failed shader compile silently deletes passes** rather than failing —
   `register_null_pipelines_when_first_compile_fails` (`gpu_context.cpp:31`) plus the early return
   in `Task::callback`. A frame missing its entire GI stack renders, runs fast, and reports
   nothing.

---

## 3. The trade curve

Pick a row. Everything is the 37 m Voxl island; the far-field cost is §5, not here. **"P" rows were
measured with the GPU profiler on and are ~5–8 % pessimistic; "R" rows are un-profiled p50 of
`full_ms` over the settled window and are the ones to quote.** Rows are sorted by frame time.

### 3.1 1280×720 output

| internal | other changes | ms | fps | src | what it costs you, from a capture I or the source agent opened |
|---|---|---:|---:|---|---|
| 1280×720 | **stock** | **10.463** | **96** | R | reference |
| 1280×720 | FSR 2.2 instead of kajiya TAA | 10.489 | 95 | R | different AA character, no cost change (§2.4) |
| 1280×720 | `Render Shadows` off | 10.186 | 98 | R | no sun shadows at all; the tree stops casting. Not worth it |
| 1280×720 | reflections off | 10.200 | 98 | R | slightly dimmer broad specular on matte voxels; the sea is pixel-identical |
| 960×540 (0.75) | — | 7.259 | 138 | R | needle clumps and canopy sky-gaps still resolve; near grass loses its finest contact detail |
| **960×540 (0.75)** | **reflections off** | **7.097** | **141** | R | **the Balanced tier** |
| 853×480 (0.667) | — | 6.432 | 156 | R | visibly softer; needle clumps merging. **Setting is unreliable — §6.1 F4** |
| 1280×720 | `global_illumination` off | 4.900 | 204 | R | **flat and dead.** The cave's amber bounce is gone entirely. Not a quality setting, a different renderer |
| 640×360 (0.50) | — | 4.877 | 205 | R | needle clumps merge into blobs; the top spire loses its shape |
| 1280×720 | GI off + shadows off | 4.653 | 215 | P | no indirect light and no shadows |
| **640×360 (0.50)** | **reflections off** | **4.538** | **220** | R | **the Performance tier.** See §6.3 for what it does in motion |
| 512×288 (0.40) | — | 3.974 | 252 | R | the voxel character of the foliage starts to go |
| 1280×720 | GI + shadows + TAA + sky off | 3.009 | 332 | P | **black sky, no sea**, hard aliasing. The floor, and unshippable |

### 3.2 1920×1055 output (the desktop clamps 1080 to 1055)

| internal | upscaler | other | ms | fps | src |
|---|---|---|---:|---:|---|
| 1920×1055 | — | native | **20.465** | **49** | R |
| 1440×791 (0.75) | kajiya TAA | — | 12.473 | 80 | R |
| 1280×703 (0.667) | kajiya TAA | — | 11.088 | 90 | R |
| 1280×703 (0.667) | FSR 2.2 | — | 10.809 | 93 | R |
| **1440×791 (0.75)** | **FSR 2.2** | **reflections off** | **10.922** | **92** | R |
| 960×527 (0.50) | kajiya TAA | — | 7.666 | 130 | R |
| 960×527 (0.50) | FSR 2.2 | — | 7.976 | 125 | R |

**The single best-value setting found anywhere in this research:** *1920×1055 output from a
1280×703 internal render costs 10.809 ms — the same as rendering 1280×720 natively (10.463 ms).*
**1080p-class presentation is essentially free relative to 720p native.** Ship 0.75 rather than
0.667 until F4 lands (§6.1), which puts it at 10.922 ms with reflections also off.

### 3.3 Sensitivity to what is in frame

The same configuration at different poses, so nobody quotes a still meadow as if it were the worst
case (R §10, P §4.3):

| pose | Stock | Balanced (0.75 + no rtr) | Performance (0.50 + no rtr) |
|---|---:|---:|---:|
| vista, still | 10.463 · 96 | **7.097 · 141** | **4.538 · 220** |
| patrol, moving 3.8 m/s | 8.577 · 117 | 5.475 · 183 | 3.720 · 269 |
| cave interior | 9.155 · 109 | 5.923 · 169 | 4.290 · 233 |
| rock face 2 m away (worst measured, profiled) | 11.616 · 85 | — | — |
| grass at your feet (worst measured, profiled) | 12.043 · 83 | — | — |

**The speed-up is roughly constant across all three poses** — 1.47–1.57× for Balanced, 2.13–2.31×
for Performance. These are not settings that only help on a still frame of open meadow.

**Two counter-intuitive results worth internalising before planning any content work:**

- **Moving is cheaper than standing still** in this scene (8.577 vs 10.463 ms). The patrol circle
  spends most of a lap looking outward over the sea. What moving *adds* is chunk generation
  (0.053 → 0.425 ms) and a p99 of 20 ms against a p50 of 8.0 — **the hitches are chunk generation,
  not a raised floor.**
- **Grass one metre away costs 2.06× as much as sky** (11.590 vs 5.628 ms, WS §4.1). A sky pixel
  costs one primary ray; a surface pixel costs a primary ray, a shadow ray, a diffuse ray, a
  reflection ray and the whole denoising stack (`trace_secondary.comp.glsl:46` gates on
  `depth != 0.0`). **The brief's intuition that distant mountains are the expensive half is
  inverted. Near grass is.**

### 3.4 The two hard ceilings

| output | fixed floor | ceiling | derivation |
|---|---:|---:|---|
| 1280×720 | **3.030 ms** | **330 fps** | R §4, fitted over 8 points |
| 1920×1055 | **~3.5 ms** | **~282 fps** | 7.666 measured at 0.50 internal, minus the fitted slope term 7.520 × 0.549 = 4.13 |

**Neither ceiling is approachable in practice** — the lowest real frame measured anywhere in this
research with the sky intact is 3.974 ms. The floor is: irradiance cache 0.95–1.03 (indirect
dispatch, probe-driven), sky LUTs 0.26–0.31, particle simulation 0.255, chunk bookkeeping 0.04–0.05
— **1.65 ms of genuinely resolution-independent work** (P §4.2) — plus the output-resolution share
of TAA and post, plus per-pass fixed overhead across 96 passes, plus a 0.22–0.40 ms barrier gap.

---

## 4. What is *not* wrong with this renderer

Worth stating, because it bounds how much is available and it is all measured.

- **The port is not accidentally slow.** kajiya's own author published 8.4 ms at 1920×1080 on a
  Radeon RX 6800 XT — a GPU with ~3× our compute and ~3.6× our bandwidth. Scaled, kajiya's own
  frame would be 25–30 ms at 1080p here; we measure 10.5 ms at 720p on a simpler scene.
  **There is no forgotten 3× sitting in the port** (PA §2.1).
- **No pass is mis-sized** except the sun shadow trace. The depth prepass is at `render_res / 2`,
  SSAO at `render_res / 2`, the ReSTIR diffuse candidate trace at half render res. Scaling across
  0.40×/1.00×/1.50× is consistent with every pass's declared size (R §11).
- **Workgroups are fine.** 65 of 72 compute shaders are 8×8×1 = 2 warps; a 720p dispatch is 14 400
  workgroups against 20 SMs (R §11).
- **Barriers are 2.3–4.0 % of the frame.** The task graph is not over-synchronising (P §4.4).
- **The GI tuning parameters are not the answer.** Every one measured is at or below the 0.3 ms
  noise floor, and one is a regression: `IRCACHE_CASCADE_COUNT` 12 → 8 costs **+0.40 ms** because
  cache lookups miss more often and `RtdgiTraceCompute` rises 1.95 → 2.13 ms (R §7.3). **Do not
  shrink the irradiance cache to save memory without measuring the trace.**
- **`MAX_STEPS` 512 → 96 changes nothing** (−0.25 ms, at the noise floor; R §7.3). The DDA finds a
  hit or leaves the 64 m volume in well under 96 steps. **Ray length is free headroom for a much
  larger world.** Caveat in §6.1 T2: nobody looked at the image, so this is consistent with "not
  binding" rather than proof of it.
- **The engine ignores 20 RT cores.** `gpu_context.cpp:14` is `choose_device({}, {})`; zero matches
  for `rayQuery|accelerationStructure|VK_KHR_ray|traceRay` in `src/`. The driver exposes
  `VK_KHR_ray_query`, `VK_KHR_acceleration_structure`, `VK_KHR_ray_tracing_pipeline` and
  `VK_NV_ray_tracing_invocation_reorder` (PA §1.3). This is the largest unexploited hardware
  capability in the project and §6.4 says what to do about it.

**Three defects found in passing that will produce wrong numbers if not fixed** (all in §6.1):
`Update Sky = false` renders the sky and sea **black**, not frozen (R §8.2,
`docs/images/renderer/R09-update-sky-off-is-black.png`); `Render Res Scale = 0.6667` silently
no-ops in about 1 run in 3 with no error and a plausible frame time (R §9.2); and
`round_frame_dim()` (`src/voxel_app.cpp:24`) is a no-op whose rounding body is commented out with
the comment `// not necessary, since it rounds up!`, which is not true of
`static_cast<daxa_u32>(1280.0f * 0.6667f)` = 853.

---

## 5. The big-world architecture

This is the half of the target that no frame-rate work touches. **The world is a 64 m cube that
wraps around the player** — `CHUNKS_PER_AXIS 16` × 4 m chunks, `voxel_malloc.inl:20`, and the file's
own comment says "there is no streaming and nothing outside it exists." `calc_chunk_index()` takes
the chunk index modulo `CHUNKS_PER_AXIS` about the player, and `PerChunkComputeShader` regenerates
the chunks falling off the trailing edge as the player moves. **The cube is a window that slides
with the player, not a level.** No amount of optimisation puts a mountain on that horizon.

### 5.1 Why brute force does not work — two independent walls

**Memory.** The chunk table is `sizeof(VoxelLeafChunk) × CHUNKS_PER_AXIS³` = **8216 bytes per
chunk**, resident whether the chunk holds rock or pure air. **8192 of those 8216 — 99.7 % — are the
per-palette-region arrays** (`u64 page_allocation_infos[512]` + `PaletteHeader palette_headers[512]`).

| CPA | world edge | view radius | table | measured heap in use |
|---:|---|---|---:|---:|
| 16 | 64 m | 32 m | **32.1 MB** | 107.5 MB (demo world) |
| 32 | 128 m | 64 m | 256.8 MB | **1262.8 MB** |
| 64 | 256 m | 128 m | **2054 MB** | — (unreachable) |
| 128 | 512 m | 256 m | 16.4 GB | — |
| 256 | 1024 m | 512 m | 131.4 GB | — |

**1263 MB of heap for a 128 m world is the practical wall, and it arrives before the table does.**
A full-detail 1 km view radius at 16 voxels/m is off by more than four orders of magnitude. No
compression fixes it: the table is the *index*, paid per chunk whether or not the chunk holds
anything.

**Time, and this is the harder wall.** Fixed camera, fully generated 128 m world, clipping only how
far rays may travel (WS §4.2, settings pinned, one 50 s run per point, all uncontended):

| ray may travel | 4 m | 8 m | 16 m | 32 m | 64 m | unclipped |
|---|---:|---:|---:|---:|---:|---:|
| frame (ms) | 7.632 | 8.076 | 8.762 | 10.440 | 14.461 | 16.870 |
| ms per added metre | — | 0.111 | 0.086 | 0.105 | 0.126 | 0.051 (box ends) |

**View distance costs ~0.11 ms per metre, linearly, at 16 voxels/m.** Extrapolated as an
order-of-magnitude argument only: **500 m of full detail ≈ 60 ms per frame.** That, not memory, is
the first-order reason the far field must be coarse. The flip side is the good news: **32 m of full
detail costs 2.8 ms**, so "grass close to the player" is affordable.

### 5.2 Why naive distance-LOD is a trap — measured, and it makes things *slower*

The obvious cheap answer is to terminate rays on a coarse block at distance. **It is a 16–20 %
regression** (WS §3.2, settings pinned, one 45 s run per point):

| cone-LOD K | frame | vs exact |
|---|---:|---|
| exact (shipped) | **10.911 ms** | — |
| 0.0167 (the depth prepass's own value) | 10.678 ms | −2 % (noise; never gets to act in a 64 m world) |
| 0.05 | **12.660 ms** | **+16 % slower** |
| 0.15 | **13.143 ms** | **+20 % slower** |
| 0.50 | 11.437 ms | +5 % slower |

**Mechanism, and it is the design requirement for everything below.** The in-chunk pyramid stores
**one bit per block — "is this block uniform" — not a filtered colour and normal**
(`voxels.glsl:190-240`). A ray can be told "you may take a 4 m step here"; it can never be told
"stop here and shade this 4 m block". Terminating early therefore manufactures phantom *surfaces*
where there was sky — `docs/images/world-scale/02-cone-lod-k0.05-phantom-slabs.png` shows large
flat grey and black slabs hanging in the sky — and a surface pixel pays a shadow ray, a diffuse
ray, a reflection ray and the denoising stack while a sky pixel pays almost nothing. The march
saved is far less than the shading added. At K = 0.5 the sky goes **entirely black**
(`03-cone-lod-k0.5-black-sky.png`), because `voxel_trace()` is shared by every caller including the
ambient and shadow rays.

> **Requirement, stated once: a coarse hit must be a real surface with real shading data.
> Replacing an expensive fine hit with a cheap correct coarse hit is a win; manufacturing a hit
> where there was sky is a loss. That is the difference between terminating on occupancy bits and
> having a filtered far field, and it is why §5.3's filtered generation is mandatory rather than
> optional.**

### 5.3 The proposal: four nested volumes differing only in voxel size

From WS §6, which I endorse without modification. Four volumes sharing the existing
`VoxelLeafChunk` layout, the existing palette compression, the existing uniformity pyramid and the
existing DDA. **The only thing that differs between levels is `LOG2_VOXEL_SIZE`.**

| level | `LOG2_VOXEL_SIZE` | voxel | chunk | CPA | world edge | view radius | table |
|---|---:|---|---|---:|---|---|---:|
| **L0 near** | **−4 (unchanged)** | **6.25 cm** | 4 m | 16 | 64 m | **32 m** | 32.1 MB |
| L1 | −2 | 25 cm | 16 m | 16 | 256 m | 128 m | 32.1 MB |
| L2 | 0 | 1 m | 64 m | 16 | 1024 m | 512 m | 32.1 MB |
| L3 far | +2 | 4 m | 256 m | 16 | 4096 m | **2048 m** | 32.1 MB |
| | | | | | | **2 km** | **128.4 MB** |

**The lever, and it is the whole idea:**

```
table (bytes)  = 8216 × CHUNKS_PER_AXIS³        <- independent of voxel size
max march step = CHUNK_SIZE × voxel_size        <- scales with voxel size
```

**The resident table cost does not depend on voxel size at all.** Making voxels 16× bigger makes
the same table cover 16× the distance *and* makes the longest march step 16× longer. Both of the
things that scale badly scale the right way at once, off one constant. **128 MB of table buys a
2 km view radius**, against 32 MB for today's 32 m.

**Crossing the whole 2 km in empty space costs at most 4 × 16 = 64 steps** against a `MAX_STEPS`
budget of 512, which R §7.3 measured as free headroom. Covering the same 2 km with today's 4 m
chunks needs 500 steps of pure vacuum before hitting anything.

**Chunk generation gets cheaper too.** `PerChunkComputeShader` invalidates the trailing face every
time the player crosses a chunk boundary — `CPA²` chunks per crossing, i.e. 256 chunks per 4 m
travelled at CPA 16. L3 crosses a chunk boundary every 256 m instead of every 4 m: **generation
work per metre travelled drops 64× between L0 and L3.** Given that `BASELINE.md` measures 3.1 ms of
the frame going to chunk generation while walking in a CPA 32 world, this matters.

**And the GI needs no change at all.** The irradiance cache is already a 12-cascade structure with
cell diameter `⅛ × 2ⁿ` metres — **8192 m of coverage** (`settings.inl:80-82`). Six of those twelve
cascades can never be populated in today's ±32 m world (P §4.4). **The probe structure was built
for the big world and is currently 40× oversized for the world it has.** With the four-level
proposal, all twelve become useful.

### 5.4 Sizing the levels — and how it interacts with the resolution lever

Each level's outer radius should be about the distance at which its voxels shrink to one pixel. At
74° vertical FOV, one pixel subtends `1.5071 / rows` radians, so a voxel of size `s` covers one
pixel at `d = s × rows / 1.5071`. A level's outer radius at CPA 16 is `16 × 64 × s / 2` = **512·s**.

| internal rows | `d₁ₚₓ` | radius / `d₁ₚₓ` | verdict |
|---|---|---:|---|
| 360 (720p out, 0.50) | 239·s | **2.14×** | 2× finer than needed — headroom |
| 540 (720p out, 0.75) | 358·s | 1.43× | comfortable |
| 720 (720p native) | 478·s | 1.07× | near-exact |
| **791 (1080p out, 0.75)** | **525·s** | **0.98×** | **exact** |
| 1055 (1080p native) | 700·s | 0.73× | ~30 % too coarse at each shell's outer edge |

**Both recommended tiers sit in or above the comfortable band, and the Quality tier
(1440×791 internal) is almost exactly optimal.** This is the useful synergy: *the resolution lever
and the LOD ladder help each other.* The only configuration that needs finer levels is native
1080p, which §3.2 already argues against on cost grounds. If it is ever wanted, the fix is CPA 32
on the outer levels at 8× the table (257 MB each).

### 5.5 Expected cost — a budget, explicitly not a measurement

**Everything in this subsection is arithmetic on measured components. No far field exists, so none
of it has been measured.** Two independent estimates are given because they rest on different
assumptions and it is worth knowing they agree.

**Estimate A (WS §6.6, per-metre scaling).** If a coarse level's per-metre cost falls with its
voxel size — 4× coarser voxels, 4× longer steps, 4× fewer surface voxels per metre — then L1 costs
~0.028 ms/m, L2 ~0.007, L3 ~0.0017, and the stack comes to `32×0.11 + 96×0.028 + 384×0.007 +
1536×0.0017` ≈ **11.9 ms at 1280×720 native.** Rescaling through the R §4 fit
(`3.03 + (11.9 − 3.03) × scale²`): **8.02 ms at 0.75 internal, 5.25 ms at 0.50 internal.**

**Estimate B (pixel-class, built from P §4.3).** Take the measured Balanced tier, 7.097 ms, and add:

| added cost | ms at 0.75 internal | basis |
|---|---:|---|
| longer marches for rays that today exit into sky | +0.5 | `TracePrimaryCompute` is 3.2× *more* expensive on a sky-heavy frame than against a wall (1.626 vs 0.508 ms) because a miss walks the whole volume. 4× the empty-space steps over ~30 % of the frame |
| sky pixels becoming distant-surface pixels that pay shadow + diffuse + reflection + denoise | +0.5 | An all-sky frame costs 6.68 ms span vs 11.22 for the baseline; converting ~30 % of pixels at roughly half the cost of *near* surface |
| irradiance-cache probes for a larger visible volume (**does not shrink with resolution**) | +0.4 | `IrcacheValidateCompute` is 0.391 ms today; assume it doubles |
| extra chunk generation across four levels | +0.1 | 64× less per metre at L3; small |
| **total** | **≈ 8.5 ms · 118 fps** | |

**A and B land within 6 % of each other (8.02 vs 8.5 ms).** They are not independent enough to call
that validation — both assume the far field behaves like the near field, scaled — but the shape is
consistent, and the shape is what a plan needs.

**The resulting budget, all figures ± 30 % and the middle column the only measured one:**

| tier | 37 m island, measured | + 2 km far field, **estimated** | if the far field costs 2× my estimate |
|---|---:|---:|---:|
| Quality, 1080p out / 0.75 | 10.922 ms · 92 fps | ~12.5–14 ms · 71–80 fps | ~16 ms · 62 fps |
| **Balanced, 720p out / 0.75** | **7.097 ms · 141 fps** | **~8.5 ms · 118 fps** | ~9.9 ms · 101 fps |
| Performance, 720p out / 0.50 | 4.538 ms · 220 fps | ~5.4 ms · 185 fps | ~6.2 ms · 161 fps |

**Read the right-hand column as the honest downside case, and note that 240 fps is not in any of
it.** The term I would most expect to be underestimated is the second row of the budget — sky
pixels becoming surface pixels — because it is the one WS explicitly flags as unmeasurable without
building a far field, and because §5.2 measured exactly that mechanism costing 16–20 % when the
surfaces were phantoms.

### 5.6 Memory, end to end

| component | today (island, 720p) | with four levels, **estimated** |
|---|---:|---:|
| chunk tables | 32.1 MB | **128.4 MB** (measured arithmetic, not an estimate) |
| voxel heap in use | 13 MB (island) / 107.5 MB (demo) | ~0.4–0.6 GB (WS §6.6 — a scaling argument from one scene) |
| everything else (render targets, ircache, particles, FSR2, Daxa) | ~2.0 GB at 720p | unchanged at 720p |
| **whole-process VRAM** | **2119 MiB measured** | **~3.3 GB at 720p output** |

That fits on a 6144 MiB card with room. **1080p output with 0.75 internal is tighter** — R §11
measured whole-process VRAM at 3276 MiB at 0.5× internal and 5373 MiB at 2.0×, so a big world at
the Quality tier lands around 3.7–4.0 GB. Workable, not comfortable.

**One precondition is already met.** `src/utilities/allocator.inl` now enforces a runtime VRAM
budget on the growable heap, including mid-realloc, and refuses growth visibly rather than
faulting. Measured effect on a CPA 32 moving soak: capacity 1376256 → 1168766 pages, peak VRAM
4592 → 4119 MiB, frame time 21.11 → 21.46 ms, heap *usage* unchanged (1629 → 1647 MB) — the trimmed
capacity was pure slack. Without that cap the next geometric growth step from 1376256 pages wanted
a 7266 MB transient peak on a 6144 MiB card, which is a device-lost, not a stutter. **Any big-world
work would have hit that within a day.**

### 5.7 What is genuinely new work, and what is not

Almost all of it is already written. `voxel_trace()` is a **single choke point** — one
implementation selected at compile time, called from **8 sites** — and `min_impl/trace.glsl` proves
the abstraction is real by swapping the whole marcher for an SDF.

| # | work | size | new? |
|---|---|---|---|
| 1 | Thread voxel size as a per-volume value instead of a global `#define` | 82 `VOXEL_SIZE` uses, 41 `CHUNKS_PER_AXIS`, 34 `CHUNK_SIZE`, 17 `VOXEL_SCL`, 17 `LOG2_VOXEL_SIZE`, 8 `CHUNK_WORLDSPACE_SIZE`, across 19 files. **The index arithmetic is already written `>> (6 + LOG2_VOXEL_SIZE)` and is correct for positive values** — this is threading a parameter, not re-deriving maths | mechanical |
| 2 | Extra buffer sets — `VOXELS_USE_BUFFERS` is used by 20 task headers; each level needs its own `voxel_chunks` and allocator, `voxel_globals` can be shared | mechanical | no |
| 3 | Multi-level march in `voxel_trace()`: march L0 to its boundary, continue in L1 from that point. Existing box-intersect and DDA reusable verbatim. Subtleties: coarse levels must be **hollow** where a finer level covers them; the hand-off needs the epsilon care `trace.glsl:130` already shows | one function | small |
| 4 | Per-level chunk generation with per-level update budgets | mechanical | no |
| 5 | **Filtered voxel generation** — a 1 m L2 voxel stands for 4096 L0 voxels and needs a colour, normal, roughness and material that genuinely represent them. Generated terrain is easy (`brushgen_world_terrain()` is analytic noise, evaluable at any spacing) but needs *dominant material of the block*, not a point sample, or distant hillsides shimmer between rock and grass as the camera moves. `try_spawn_grass` / `try_spawn_tree` must not fire in coarse levels | **the only genuinely new algorithm** | **yes** |
| 6 | Player edits above L0 | — | **defer.** Propagating a 6.25 cm edit up three levels needs a downsample pass per level per edited region. The honest v1 position is that edits are not visible beyond 32 m — fine for a distant mountain, not fine for a player-built tower |

**One more thing the far field must arrange:** grass, flowers and trees are a **separate rasterised
particle system**, never marched (`src/voxels/particles/`), and `MAX_GRASS_BLADES` is 1 048 576 —
**exactly the surface-voxel count of one fully-grassed 64 m world at 16 voxels/m.** The budget is
precisely one L0 volume and it is already saturated. Coarse levels must not spawn particles.

---

## 6. Ranked work plan

Ordered by measured or estimated value per unit of effort. **"Measured"** means a number exists in
one of the four research documents. **"Estimated"** means I derived it here. **"Speculative"** means
nobody has a number.

**Every acceptance threshold below requires the same A/B protocol**, because §2.4 showed that a
profiler number is an upper bound, not a saving:

> Land F1 first. Then: profiler **off**, overlay **off**, settings written and read back, GPU
> verified uncontended for the whole run, p50 of `full_ms` over `t ∈ [8 s, end − 0.5 s]`, three
> poses (vista / patrol / cave), control run interleaved with the treatment run. **Reject anything
> under 0.3 ms as noise.** Re-shoot `docs/images/renderer/R05-cave-gi-on-vs-off.png` and open it.

### 6.1 Free wins — do these regardless of which tier is chosen

Total measured value: **3.2–9.5 ms depending on output resolution**, plus three defect fixes and
the measurement hygiene without which nothing else can be trusted. None of these is a refactor.

| # | change | value | effort | accept if | how to A/B |
|---|---|---|---|---|---|
| **F1** | **Land `VOXL_DATA_DIR`** — a data-directory override in `ui.cpp` so each build reads its own settings instead of the shared `%APPDATA%\GabeVoxelGame\user_settings.json`. Written and used already in `C:\voxl2_ws` | **0 ms — and every other row is unreliable without it.** This bug reversed the sign of one agent's headline result and cost three agents a day each | ~23 lines | Two identical runs 20 minutes apart agree within 0.15 % (WS achieved 10.900 vs 10.911) | run the WS anchor twice with a sibling engine deliberately writing settings in between |
| **F2** | **Print the effective `Graphics` settings into the `--bench-csv` header and the overlay** | 0 ms; makes a tampered run visibly rejectable instead of silently wrong | hours | a run whose settings changed mid-flight is detectable from its CSV alone | inspect the header of any existing CSV |
| **F3** | **`--render-scale`, `--taa`, `--gi`, `--reflections` on the command line.** No quality knob is reachable from the CLI today; every harness in this project patches JSON | 0 ms; turns the whole of §3 into a CI check | hours | the full R §4 eight-point sweep runs from one script with no JSON patching | re-run R §4 and reproduce all eight points |
| **F4** | **Fix `round_frame_dim()`** (`voxel_app.cpp:24`) — restore the commented-out rounding, to a multiple of 8 to match the 8×8 workgroups. Several passes derive half-res extents as `(x+1)/2` | 0 ms; likely derisks the `Render Res Scale = 0.6667` intermittent no-op (observed in **4 of 11 runs**, never at 0.40/0.50/0.70/0.75/1.25/1.50/2.00) | ~5 lines | 20 consecutive runs at 0.6667 all apply | R §9.2's Laplacian-variance metric on a fixed tree-silhouette crop — **not** the frame time, which looks legitimate when the setting fails |
| **F5** | **`Render Res Scale` 0.75 as the 720p default** | **measured −3.20 ms** (10.463 → 7.259; 96 → 138 fps) | 1 line | `R02-tree-four-render-scales.png` re-shot: needle clumps and canopy sky-gaps still resolve | already shot and opened; re-shoot after F4 |
| **F6** | **1920×1055 output from 1440×791 internal + FSR 2.2** | **measured −8.0 ms** (20.465 → 12.473; 49 → 80 fps), or **−9.5 ms** with F7 (→ 10.922, 92 fps) | setting only | indistinguishable from native at 2× on stills | `R03-tree-1080p-taa-vs-fsr.png` |
| **F7** | **A `ray_traced_reflections` toggle.** Exists in `C:\voxl2rs` only; port it. One trap recorded: `RtrRenderer::next_frame()` swaps eight `PingPongImage`s that only `trace()` creates, so skipping the trace and still calling `next_frame()` takes an access violation two frames in — the guard is one line | **measured −0.98 ms** vista, **−1.21 ms** cave (9 % of the frame) at native; −0.34 ms at 0.50 internal | ~10 lines | `R06-reflections-on-vs-off.png`: the sea is pixel-for-pixel identical; the only change is a slightly dimmer broad specular lift on matte voxels. **Revisit the moment water, glass, ice or metal exist** — that is what this stack is for | R06 plus the cave pair |
| **F8** | **Wire `VoxelTraceResult.step_n` into the debug image.** It is computed and thrown away (`trace_primary.comp.glsl:93`); the prepass consumer at `:84` reads a `.y` channel that is never written | 0 ms; **converts most of WS §4 from inference into measurement** and is the prerequisite for judging any far-field change | small | a step-count heatmap renders and its maximum is readable | compare a wall frame against a sky frame — the prediction is that the *sky* frame has the higher counts |

### 6.2 Medium — worth doing, each needs its own measurement

| # | change | value | status | accept if |
|---|---|---|---|---|
| **M1** | **Half-resolution sun shadow trace.** `trace_secondary.inl:26` sizes the shadow mask at full `render_resolution` and it is the only ray-traced stage that is not half-res (rtdgi, rtr and SSAO all are). It is 0.754 ms at 720p, **2.135 ms at 1080p**, and grows **super**-linearly (×3.43 for ×3.91 pixels) | **Downgraded from PROFILE §4.4's "cheapest plausible win".** P proposed pairing it with the shipped denoiser; R measured that denoiser at **+1.05 ms** (`denoise_shadow_mask = true`, 10.463 → 11.508), and `shadow_denoiser.inl` runs it at full `render_resolution` too. Half-res trace saves ~0.4 ms at 720p against a ~1.05 ms denoiser — **a net loss at 720p unless the denoiser is also halved**. At 1080p the trace saving is ~1.1 ms and it may pay | *estimated* | ≥0.3 ms at 720p **and** no visible shadow noise increase on the tree terminator in `R10`. If it fails at 720p, retry at 1080p before discarding |
| **M2** | **Attack the fixed floor — the highest-value pool at the target frame time** (§1.4). Three items: (a) the grass simulation dispatches over all 1 048 576 slots every frame regardless of live count (`grass.inl:132`, a plain `dispatch`, not indirect) — 0.255 ms, flat; (b) sky LUTs behind a dirty flag — 0.2–0.26 ms, `Sun/Animate` defaults false and nothing in the transmittance/multiscattering/sky-view LUTs changes; (c) collapse the ~60 passes that cost <0.05 ms each and total 0.6 ms, starting with the nine-dispatch blur pyramid (0.13 ms total) and the particle raster passes that run with zero particles | ~0.5–0.7 ms of a 3.03 ms floor = **12–17 % of a 4.17 ms frame.** At native resolution each looked like noise, which is why no document ranked it | *estimated*, from measured components | ≥0.4 ms combined, measured at **0.50 internal scale** where it matters, not at native. **(b) is gated on fixing the black-sky bug** — `Update Sky = false` today renders sky and sea black (`R09`), and `Renderer::begin_frame` running the sky graph once on `frame_index == 0` is demonstrably not sufficient, especially since `record_tasks()` runs twice on the first frame whenever the resolution scale ≠ 1.0 |
| **M3** | **Software VRS on the GI traces.** Classify 8×8 tiles by a Sobel edge test on the previous frame's denoised GI and shade at 1×1 / 2×1 / 1×2 / 2×2. Hardware VRS is unusable — it applies to pixel shaders only and this renderer is compute end to end | Published at **27–32 % of raytracing cost** elsewhere. Our GI trace pool is ~2.7 ms at native → 0.7–0.9 ms; **but only ~0.2 ms at 0.50 internal** (§1.4). Small and reversible | *speculative for our case* | ≥0.3 ms at the tier being shipped. If the shipped tier is Performance, **this probably does not clear the noise floor and should be dropped** |
| **M4** | **The ray-query scratch benchmark.** One standalone Vulkan app, outside `C:\voxl2\src`, one fixed set of camera rays, traced against (a) the current software DDA and (b) a BLAS of per-brick AABBs with an intersection shader | 0 ms directly. **It settles the largest open decision in the project** with a number instead of an argument, and PA §7.1 flags the absence of that number as its own biggest gap | *speculative* — no head-to-head of ray query vs software DDA over a voxel brickmap could be found anywhere | a rays/second figure for both, on this GPU, at a representative ray distribution. **Note the honest counter-case before starting:** custom AABB/procedural primitives do *not* get RT-core-accelerated intersection — only BVH *traversal* is hardware-accelerated, the intersection shader runs on ordinary shader cores, and NVIDIA's own guidance is "use triangles over AABBs". The win, if any, comes from hardware traversal, `VK_NV_ray_tracing_invocation_reorder`, and unbounded world size — not from the intersection test |
| **M5** | **Verify `MAX_STEPS` is not binding, and raise it before the far field exists.** Arithmetic: minimum step 6.25 cm × 512 steps = **32 m**, against a CPA 16 box diagonal of 111 m. A capped ray is reported as a *miss* and shaded as sky, so the symptom is **holes in distant geometry**, not a slowdown | R §7.3's "512 → 96 changes nothing" is *consistent with* not binding but nobody looked at the image, so it is not proof. The experiment is already built (`wsx\steps.ps1`, runtime-compiled, no rebuild) | measured-adjacent | the step-count heatmap from F8 never reaches the cap on any pose. **Do this before trusting any large-world screenshot** |

### 6.3 Large projects — one of them is required, the others are not

| # | project | value | verdict |
|---|---|---|---|
| **L1** | **The multi-level far field** (§5). **Required by half the target; nothing else answers it.** | Estimated +1.4 ms at the Balanced tier for a 2 km view radius, ±30 % | **Do it — but build ONE extra level first, not four.** Add L1 alone (25 cm voxels, 256 m cube, 128 m radius) in a scratch tree. That is one extra buffer set, one extra march segment and one filtered generator, it puts a visible ridgeline at 128 m, and it **converts the entire §5.5 budget from arithmetic into a measurement** — including the one term (sky pixels becoming surface pixels) that nobody can estimate honestly. If L1 costs what the budget says, L2 and L3 are mechanical repetition. If it costs 3× the budget, the whole architecture needs rethinking and you will have learned that for one level's worth of work |
| **L2** | **FSR 3 frame generation** | ~1.6–2× displayed frame rate; the only literal route to 240 with a big world. FSR 3 FG is hardware-agnostic and works on this GPU; **DLSS FG does not** — it needs the 40-series optical flow accelerator | **Last, and only after an explicit conversation.** It does not improve responsiveness: from 120 real fps it gets ~240 displayed at ~120 fps of input latency, plus ~18 ms. We already ship FidelityFX for FSR 2.2, so the dependency exists |
| **L3** | **DLSS Ray Reconstruction** replacing the kajiya denoiser stack | Our denoiser + TAA pool is ~3.5 ms at native. **But RR runs at output resolution**, so at 640×360 → 720p it would be a *fixed* ~1.5–2 ms against a 4.5 ms frame — it does not shrink with the resolution lever, which is the lever we are taking | **Only for the Quality tier, never for Performance.** In Bevy Solari, DLSS-RR was **5.75 ms of an 8.22 ms frame**; it is emphatically not free. Gate on measuring our denoiser stack in isolation first |
| **L4** | **Hardware ray query rewrite of the voxel tracer** | Unknown. Both Dennis Gustafsson (Teardown) and **Gabe Rundlett — the author of this codebase** — independently moved to hardware RT with intersection shaders and no triangles for exactly this workload, and the `compute-rt` branch we forked has not moved since 2024-11. Gustafsson's stated benefit is the one our target needs: "unlimited world size" | **Not this phase, and possibly not this year.** New acceleration structure over bricks, BLAS/TLAS build and refit for a *destructible* world, and every one of the 8 `voxel_trace()` call sites rewritten. **Gate it entirely on M4.** |
| **L5** | **Radiance cascades** | At most a fraction of the diffuse GI, which is 2.3 ms of kajiya's 8.4 — capped around 27 % of the frame even if free. radiance.wiki states 3D "remains an open problem"; the circulating "0.3 ms on a GTX 970" figure is a demo with no denoising or temporal accumulation. Our irradiance cache is already a probe-based amortisation doing a related job at 0.95 ms | **Do not build this quarter.** Worth one person reading Split Radiance Cascades (arXiv 2607.20384, nine days old) — PA could not retrieve its performance table |

### 6.4 Explicitly rejected, with the measurement

| change | why not |
|---|---|
| `global_illumination = false` | −5.56 ms, and it destroys the entire reason for the pivot. §7 |
| `IRCACHE_CASCADE_COUNT` 12 → 8 | **costs +0.40 ms.** Cache misses push `RtdgiTraceCompute` 1.95 → 2.13 ms |
| `denoise_shadow_mask = true` as shipped | **costs +1.05 ms.** Already off; keep it off. See M1 for the version that might pay |
| `TAA Method = None` | −0.88 ms and all temporal antialiasing gone, on 16 voxels/m content which needs it most |
| `Render Shadows = false` | −0.28 ms (noise floor) for no sun shadows at all |
| `battery_saving_mode` | **costs +6.17 ms.** It is a `sleep_for(10ms)` |
| Naive cone-LOD termination in the main trace | **+16 to +20 %.** §5.2 |
| `IRCACHE_SAMPLES_PER_FRAME` 4 → 2, rtdgi spatial reuse 2 → 1, `MAX_STEPS` 512 → 192 | 0.19–0.29 ms each, all at or under the noise floor |
| Hardware VRS | Pixel-shader only; this renderer is compute end to end |
| CPU optimisation of any kind | 0.071–0.12 ms available, total. §2.3 |
| Changing `LOG2_VOXEL_SIZE` | Out of scope by instruction, and §5.3 shows it is unnecessary — the far field gets its coarseness from *nested volumes*, with L0 unchanged at −4 |

---

## 7. What protects the look

The user pivoted to this engine for the path-traced look. An optimisation that reaches 240 fps by
destroying it has not succeeded. These are the properties that are not negotiable, what threatens
each, and the regression test for each.

### 7.1 Coloured indirect bounce, and a cave that is dark

**The regression test is `docs/images/renderer/R05-cave-gi-on-vs-off.png`. Name it as such; re-shoot
it after every change in §6; open it every time.** I opened it while writing this document. With GI
on, the tunnel is bathed in warm amber bounce from crystals *behind the camera*, falling off toward
a cooler far end, with the meadow visible through the portal. With GI off it is a uniform cold
blue-grey — not darker, **flat**. The emissive crystals contribute nothing at all; the only light is
a constant sky ambient term. There are no light sources in that scene except the sun and 1.5 m² of
emissive voxels. **That single image is the entire justification for spending 5.7 ms a frame on GI.**

Supporting frames, all already captured: `docs/images/16-cave-lookback-gi.png` paired with
`17-cave-lookback-dark-control.png` (SCENE.md §3.3 calls this "the single best GI frame" — amber
bounce in the near half of the tunnel, blue sky bounce in the far half); and
`14-cave-interior-lit.png` / `15-cave-interior-dark-control.png`, with SCENE.md's caveat that
auto-exposure flatters the second pair, so **quote the lookback pair.**

Reassuringly, `docs/images/renderer/R04-cave-gi-variants.png` shows the same frame with reflections
off, at 0.667 internal and at 0.50 internal: **all three keep the amber bounce, the warm-to-cool
gradient and the meadow through the portal.** The variation between them is far smaller than the gap
to the GI-off frame.

| threat | severity | mitigation |
|---|---|---|
| `global_illumination = false` | **fatal** | never ship it; it is a cost budget, not a setting |
| `IRCACHE_CASCADE_COUNT` reduction | real, and it is also **slower** | rejected in §6.4 |
| Software VRS (M3) too aggressive | moderate — published as "a small impact on GI noise" | acceptance threshold includes the cave pair |
| DLSS-RR (L3) over-aggressiveness | moderate — Gustafsson: "a bit aggressive in some scenarios" | Quality tier only |
| Auto-exposure differing between runs | a *measurement* hazard, not a look hazard | SCENE.md §7.5; use the lookback pair, which has something bright to anchor to |

### 7.2 16 voxels per metre near the player

`LOG2_VOXEL_SIZE = −4` is the look being chased and is out of scope by instruction. **Nothing in
this plan changes it.** The far-field proposal keeps L0 at −4 with a 32 m radius, and §5.1 measures
that 32 m of full detail costs 2.8 ms — affordable.

| threat | severity | mitigation |
|---|---|---|
| Naive distance-LOD termination inside 32 m | **fatal to the near look and 16–20 % slower** | measured and rejected, §5.2. `02-cone-lod-k0.05-phantom-slabs.png` shows the hill silhouette eaten into |
| A far field that is not hollow where L0 covers it | fatal — the ray hits the coarse version of terrain it should be seeing in detail | §5.7 item 3; it is a known requirement, not a discovery waiting to happen |
| Filtered generation using a point sample instead of the dominant material | visible as distant hillsides **shimmering** between rock and grass as the camera moves | §5.7 item 5 |

### 7.3 Sub-pixel voxel detail in near grass — the property the 240 fps tier actually spends

This is the honest cost of the Performance tier and it must not be buried.
**`docs/images/renderer/R08-upscalers-in-motion.png`, which I opened**, is four crops of grass at 3×
with the camera moving at 3.8 m/s:

- **native** — individual grass voxels, hard-edged flowers, some temporal sparkle.
- **0.50 + kajiya TAA** — the grass becomes a mush; individual voxels are gone, flowers survive as
  blobs, and there is **visible cyan/teal fringing** around the pink flower edges that is not
  present at native.
- **0.50 + FSR 2.2** — the same blur and the same fringing; flowers marginally better defined.
- **0.667 + FSR 2.2** — partially recovers the grass texture; still clearly softer than native.

**Still images flatter both upscalers substantially** — at 0.50 on a static camera the same content
merely looks "soft" (`R02`, `R07`). Sharp sub-pixel voxel edges are exactly what temporal upscalers
handle worst. **That is why the Balanced tier stops at 0.75 and why 0.75, not 0.50, is the
recommended default.**

One caveat that must be re-checked: at native resolution the near grass is full of hard black
pockets — the voxel-heap hole defect of SCENE.md §7.1 showing through at sub-pixel scale — and
reducing resolution *hides* it. **Part of what currently looks like "no loss at 0.667" is an
artefact being blurred away. Every resolution comparison must be re-shot once the hole defect is
fixed** (`docs/images/22-defect-hole-closeup.png` is the defect; it is visible in the hill dome in
`R10-native-vs-presets.png` in all three panels).

### 7.4 The sky and the sea

| threat | severity | mitigation |
|---|---|---|
| `Update Sky = false` | **fatal — renders sky and sea pure black**, not frozen (`R09-update-sky-off-is-black.png`). Terrain, grass and the cave glow stay correctly lit, so the LUTs the GI samples are still valid; it is the background that is lost | **Do not ship it and do not quote any benchmark that used it** — one preset table in this project already had to be re-measured because of it. M2(b) is the correct fix: a dirty flag on the sun/atmosphere settings, leaving the camera-dependent aerial-perspective LUT on every frame |
| Cone-LOD at K ≥ 0.5 | **fatal — sky entirely black** (`03-cone-lod-k0.5-black-sky.png`), because `voxel_trace()` is shared by the ambient and shadow rays, so every ambient ray terminates on a phantom blocker | rejected in §6.4 |
| `TAA Method = None` + `Update Sky = false` together | the 332 fps row of §3.1 | listed only to document the floor |

### 7.5 The one visual property that gets *better*

**Distant mountains.** Every capture in this project ends in an empty water horizon
(`R10`, `10-scene-wide-sunlit.png`, PA §1.2). The far field of §5 is the only item in this plan that
*adds* to the look rather than trading against it, and it is the half of the user's request that no
frame-rate work touches.

---

## 8. The first week

Ordered. Days 1–2 are pure free wins and cost nothing in image quality. Nothing here is a refactor;
the one experimental build lives in a scratch tree.

**Day 1 — make measurement trustworthy. Nothing else counts until this is done.**

1. **F1: land `VOXL_DATA_DIR`.** Port the 23-line `ui.cpp` override from `C:\voxl2_ws`. Verify by
   running the anchor twice, 20 minutes apart, with a second engine instance deliberately writing
   `user_settings.json` in between. **Accept at ≤0.15 % spread.**
2. **F2: print the effective `Graphics` settings into the `--bench-csv` header and the overlay.**
3. Re-establish the clean anchor and record it: `10.9 ms / 91.7 fps` at the Voxl spawn is the WS
   figure to reproduce. **Everything after this is measured against that number, not against the
   brief's.**

**Day 2 — the free wins.**

4. **F3: `--render-scale`, `--taa`, `--gi`, `--reflections` on the CLI.** Then re-run the R §4
   eight-point resolution sweep from one script and confirm all eight points reproduce. That sweep
   becomes the project's standing performance test.
5. **F4: fix `round_frame_dim()`.** Then 20 consecutive runs at 0.6667, checked with R §9.2's
   Laplacian-variance metric, **not** with the frame time.
6. **F7: port the reflections toggle** from `C:\voxl2rs`, including the `next_frame()` guard.
7. Re-measure the three-tier preset table (§1.2) with everything landed, at all three poses, and
   **re-shoot and open `R05-cave-gi-on-vs-off.png` and `R10-native-vs-presets.png`.**

**Day 3 — choose the tier, and instrument the marcher.**

8. Put the §3 trade curve and the re-shot `R10` in front of the user and **get a tier decision.**
   Everything after this point is sized differently depending on the answer. My recommendation is
   Balanced (0.75 internal, reflections off) with Performance shipped as an explicit option.
9. **F8: wire `step_n` into the debug image.** Confirm the counter-intuitive prediction — that a
   *sky* frame has higher step counts than a wall frame — which is the single best sanity check that
   the marcher behaves as P §4.3 says.
10. **M5: verify `MAX_STEPS` is not binding**, using the heatmap from step 9 and the existing
    `wsx\steps.ps1`. **Do this before anyone screenshots a large world.**

**Day 4 — the fixed floor, measured at the tier that was chosen.**

11. **M2(a): make the grass simulation dispatch indirect** over live blades instead of all
    1 048 576 slots.
12. **M2(b): diagnose the black-sky bug**, then put the three camera-independent atmosphere LUTs
    behind a dirty flag, leaving the aerial-perspective LUT per-frame.
13. Measure both **at 0.50 internal scale**, where the floor is 62 % of the frame, not at native
    where each looks like noise. **Accept at ≥0.4 ms combined.**

**Day 5 — convert the biggest unknown into a number.**

14. **L1, one level only.** In a scratch tree (`robocopy /MIR`, clear the CMake cache, 36 s build),
    add a single L1 volume: `LOG2_VOXEL_SIZE −2`, 25 cm voxels, CPA 16, 256 m cube, 128 m radius.
    L0 hollow-punched out of it, one filtered generator using the dominant material of the block,
    no particle spawns above L0.
15. Measure it at the chosen tier, at all three poses, and **open the screenshot.** The two
    questions it answers are the two nobody could answer: *what does a real distant ridgeline cost*,
    and *what happens when sky pixels become surface pixels*.
16. **Compare against the §5.5 budget** — which predicts +0.5 ms at 0.75 internal for the L1 shell
    alone. If it lands within ±50 %, L2 and L3 are mechanical and the four-level architecture is
    approved. If it costs 3×, stop and re-plan.

**Explicitly not in the first week:** DLSS-RR, radiance cascades, frame generation, any hardware
ray-tracing work beyond the M4 scratch benchmark, and any change to `LOG2_VOXEL_SIZE`.

---

## 9. What this plan does not establish

1. **Everything in §5.5 is arithmetic, not measurement.** No far field exists. The term I trust
   least is sky pixels becoming distant-surface pixels, and §5.2 measured a closely related
   mechanism costing 16–20 % when the surfaces were phantoms. Day 5 of §8 is designed to replace
   the whole subsection with a number.
2. **The FSR 2.2 disagreement in §2.4 is unresolved.** 0.663 ms (profiled) against +0.03 ms
   (un-profiled), on the same change. One un-profiled A/B pair settles it in ten minutes.
3. **No number anywhere in this project was measured at 1080p with a reduced internal render *and*
   the profiler on.** Every attempt shared the GPU with a second 6 GB-class instance and read
   8.5–500 ms. The four other resolution points are clean.
4. **Whether the 0.667 and 0.75 visual conclusions survive the grass hole defect being fixed.**
   Reducing resolution currently hides that defect (§7.3).
5. **Why `RtdgiTraceCompute` costs 8.2 ns per ray.** 230 400 half-resolution rays for 1.90 ms;
   whether that is bound by the DDA's memory traffic, by divergence or by occupancy is not
   established. **Nsight Graphics on that one pass** is the obvious next step and it would change
   what any GI optimisation should attempt. Related and also unmeasured: the 96-bit / 144 GB/s bus
   makes bandwidth-bound a strong hypothesis, and if true it would raise M4's expected value.
6. **`RtrRestirResolveCompute`'s 4.23 ms p99** against a 0.41 ms mean, seen in one run, not
   reproduced, not explained.
7. **No head-to-head of `VK_KHR_ray_query` against a software DDA over a voxel brickmap exists**
   anywhere that could be found. M4's entire justification is practitioner choice and architecture.
8. **Long sessions.** The longest continuous run anywhere in this project is 300 s (SCENE.md §8.2);
   most are under 45 s. Clocks held 1822–1935 MHz at 68–78 °C with no throttling, but a
   ten-minute session at the Performance tier has not been tried.
9. **Whether four levels' heap really is ~4× one level's.** WS's estimate rests on two usable data
   points from one scene.
