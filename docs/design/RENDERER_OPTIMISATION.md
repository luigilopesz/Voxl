# What the renderer can give back, and at what visual cost

**Recorded:** 2026-07-31
**Binary under test:** `4594a45` (`C:\voxl2`), plus a scratch mirror at `C:\voxl2rs` used for the
compile-time experiments in §7 and §8.
**Hardware:** RTX 3050 6GB Laptop, driver 610.74; i7-13650HX; 16 GB; Windows 11 Pro 26200.

This is a measurement document. Every number is followed by the command that produced it, every
screenshot referenced here was opened and looked at, and where a number is inside the noise floor
it says so rather than being rounded into a claim.

---

## 0. The five sentences

1. **Frame time on this scene is `3.03 ms + 7.52 ms × (internal pixels / 921600)`** — a fixed
   floor of about 3 ms and a resolution-proportional term. The fit reproduces eight measured
   points from 512×288 to 2560×1440 to within 0.44 ms (§4).
2. **240 fps is reachable today, with the path-traced GI fully intact** — 3.720 ms / 269 fps
   moving and 4.538 ms / 220 fps standing still, at 640×360 internal upscaled to 720p with
   reflections off. The honest qualification is that this is a soft image in motion (§5.3) and a
   37 m island with no distant mountains in it.
3. **240 fps at *native* 1280×720 is not reachable by tuning.** A frame containing no voxel
   geometry at all — sky and sea only — still costs **5.846 ms** (§9.1), and the fixed floor caps
   the whole renderer at 330 fps whatever the resolution.
4. **The knee of the curve is 0.75 internal scale plus reflections off**: 7.10 ms / 141 fps at
   720p and **10.92 ms / 92 fps at 1920×1055**, against 20.47 ms / 49 fps for native 1920×1055.
   1080p-class output for less than the price of 720p native, and the still frames are hard to
   tell apart (§10).
5. **Two bugs were found on the way and both matter**: `Graphics/Update Sky = false` renders the
   sky and sea **black**, not frozen (§8.2); and `Graphics/Render Res Scale = 0.6667` silently
   fails to apply in about a third of runs, with no error and a plausible-looking frame time
   (§9.2).

---

## 1. First: which GPU

The brief asked for this to be verified before anything else was believed. It is the RTX 3050.

- `nvidia-smi --query-gpu=name,driver_version,memory.total --format=csv` →
  `NVIDIA GeForce RTX 3050 6GB Laptop GPU, 610.74, 6144 MiB`
- The engine's own overlay, read off the capture in
  `docs/images/renderer/R00-control-overlay-1280x720.png`, prints
  `GPU: NVIDIA GeForce RTX 3050 6GB Laptop GPU`. That string comes from
  `gpu_context.device.properties().device_name` (`src/voxel_app.cpp:60`), i.e. the Vulkan
  physical device actually in use, not a guess.

The hybrid-graphics warning still has a residue worth knowing about, in §11.

---

## 2. How everything below was measured

The engine already takes `--pos/--rot/--patrol/--exit-after/--screenshot/--bench-csv`
(`src/application/cli.hpp`), so the camera is reproducible and the frame time comes out of the
engine rather than off a screenshot. What it does **not** have is any way to set a quality
setting from the command line: every knob is an `AppSettings` entry loaded once from
`%APPDATA%\GabeVoxelGame\user_settings.json` (`src/application/ui.cpp:173`) and otherwise only
reachable through the ImGui panel.

The harness therefore writes that JSON before each launch. It lives in this session's scratchpad,
not in the repo:

```
scratchpad\knob.ps1              patch settings -> launch -> parse the CSV -> one JSON result line
scratchpad\sweep.ps1             run a list of configurations back to back
scratchpad\rebuild-measure.ps1   apply one source edit to C:\voxl2rs, rebuild, measure, restore
scratchpad\passdiff.py           median per-pass GPU ms, grouped, across several profiler CSVs
scratchpad\compare.py            side-by-side nearest-neighbour crops of several captures
```

A representative invocation, and the one that produced the control row:

```powershell
powershell -File scratchpad\knob.ps1 -Name s00-control-noverlay `
    -Local "0.01,0.02,5.53" -Rot "0.785,1.096" -ConvergeSec 22
# -> engine args: --unpause --exit-after 24 --width 1280 --height 720
#    --bench-csv ... --screenshot ... --screenshot-after 22
#    --pos -182.99,-109.98,-46.97 --rot 0.785,1.096 --no-overlay
```

**The statistic quoted everywhere below is the p50 of `full_ms` over `t ∈ [8 s, ConvergeSec−0.5 s]`.**
The lower bound is past world generation and past the temporal warm-up; the upper bound excludes
the screenshot frame, which stalls on `device.wait_idle()` for a readback and is worth ~250 ms on
its own. p50 rather than mean, for the reason in the next paragraph.

### 2.1 The contention caveat — read this before comparing any two numbers here

Other agents were running this same engine against this same GPU throughout. Two instances do not
fit in 6 GB: the loser spends whole seconds at ~700 ms per frame while the driver pages its heap
over PCIe. One early run produced **26 frames in 32 seconds**. The harness now (a) waits for any
other `gvox_engine` process to exit before launching and (b) reports `stall_frac`, the fraction of
settled frames longer than 3× the median. **Any row with a non-zero `stall_frac` was contended and
its mean is meaningless; its p50 usually survives.** Rows quoted below have `stall_frac = 0` unless
noted.

### 2.2 Repeatability

The same control configuration, measured five times across the session:

| Run | p50 ms | Notes |
|---|---|---|
| `s00-control-noverlay` (stock binary) | 10.463 | |
| `s00-control-noverlay` (repeat) | 10.712 | |
| `z16-control` (scratch binary) | 10.539 | scratch tree, same sources plus a reflections toggle |
| `c01-control` (overlay on) | 10.925 | the debug overlay costs about 0.46 ms |
| `x00-scratch-control` (GPU profiler on) | 11.135 | the profiler costs about 0.6 ms |

**Treat any difference under about 0.3 ms as noise** unless it is corroborated by the per-pass
profiler, which measures the pass directly instead of inferring it from the frame.

### 2.3 The scene, and what it is not

Everything here is the Voxl test scene (`docs/SCENE.md`): a 37 m island, one conifer, a cave with
an emissive chamber, dense grass and flowers. Two poses are used throughout:

| Pose | `-Local` | `-Rot` / `-Patrol` | What it stresses |
|---|---|---|---|
| **vista** | `0.01,0.02,5.53` | `0.785,1.096` | dense near grass; the frame the 10.93 ms baseline came from |
| **cave** | `18.4,18.7,2.8` | `3.927,1.571` | the GI regression frame — bounce light in a dark interior |
| **patrol** | `10,10,8` | `--patrol 12,20` | a moving camera at 3.8 m/s, added in §9 after the still-camera results turned out to flatter the upscalers |
| *(diagnostic)* | `22,10,8` | `1.571,1.571` | a frame with **no voxel geometry at all** — sky and sea. Used once, in §9.1, and it is the most important single number here |

**This scene has no distant mountains in it.** Every conclusion below is about the cost of the
*renderer*, holding content fixed. Growing the world will move both terms of the equation in §4
and nothing here predicts by how much.

---

## 3. Where the 10.5 ms goes

A per-pass GPU timing facility landed in the tree during this session
(`src/utilities/gpu_profiler.hpp`, enabled by `$env:VOXL_GPU_PROFILE`). It writes two
`vkCmdWriteTimestamp` per pass and reads back three frames late. It was not written for this work
but it is exactly the right instrument, so §3, §7 and §8 use it.

Vista pose, 1280×720, GI on, median of 993 settled frames (`x00-passes.csv`):

| Subsystem | ms | % | What it is |
|---|---:|---:|---|
| **RTDGI** — ReSTIR diffuse | **3.444** | 33.1 | `RtdgiTraceCompute` alone is **1.953 ms**, the largest single pass in the frame |
| **PRIMARY** — visibility | 1.479 | 14.2 | `TracePrimaryCompute` 1.070, half-res depth prepass 0.261 |
| **RTR** — ReSTIR reflections | 1.074 | 10.3 | six passes, none above 0.35 ms |
| **TAA / upscale** | 1.011 | 9.7 | seven passes, at **output** resolution |
| **IRCACHE** — irradiance cache | 0.952 | 9.2 | indirect dispatch, driven by probe count not pixel count |
| **SHADOW** — sun trace | 0.808 | 7.8 | one pass, `TraceSecondaryCompute` |
| **PARTICLES** — grass/flowers | 0.738 | 7.1 | `GrassStrandCubeParticleRaster` 0.382 + sims |
| **SSAO / SSGI** | 0.254 | 2.4 | runs at render_res / `SHADING_SCL` |
| **SKY** — atmosphere LUTs | 0.258 | 2.5 | five passes, **completely independent of resolution** |
| **POST** — tonemap/exposure | 0.169 | 1.6 | |
| **WORLD** — chunk generation | 0.040 | 0.4 | static camera: nothing to generate. See §9 |
| OTHER (`LightGbufferCompute` etc.) | 0.178 | 1.7 | |
| **sum of 96 passes** | **10.404** | | `gpu_span` 11.154, of which 0.390 is barrier/layout gap |

The whole GI stack (RTDGI + IRCACHE + RTR + SSAO) is **5.724 ms**. Turning
`Graphics/global_illumination` off measures **−5.56 ms** (10.463 → 4.900). Two independent methods,
agreeing to 3%.

---

## 4. The one equation

Output pinned at 1280×720 so the window manager can never clamp anything; `Graphics/Render Res Scale`
swept from 0.4 to 2.0, which drives the internal render target from 512×288 to 2560×1440. All
`stall_frac = 0` except the 0.40 row (0.0003).

| Scale | Internal | Measured p50 | Fit | Error | fps |
|---:|---|---:|---:|---:|---:|
| 0.40 | 512×288 | 3.974 | 4.233 | −0.259 | **251.6** |
| 0.50 | 640×360 | 4.877 | 4.910 | −0.033 | 205.0 |
| 0.667 | 853×480 | 6.432 | 6.373 | +0.059 | 155.5 |
| 0.75 | 960×540 | 7.259 | 7.260 | −0.001 | 137.8 |
| 1.00 | 1280×720 | 10.463 | 10.550 | −0.087 | 95.6 |
| 1.25 | 1600×900 | 14.945 | 14.780 | +0.165 | 66.9 |
| 1.50 | 1920×1080 | 20.391 | 19.950 | +0.441 | 49.0 |
| 2.00 | 2560×1440 | 32.824 | 33.109 | −0.285 | 30.5 |

```
frame_ms = 3.030 + 7.520 × (internal pixels / 921600)
```

Two consequences, and they are the shape of the whole problem:

- **The fixed floor is 3.03 ms — a hard ceiling of 330 fps** that no amount of resolution
  reduction can pass. §3 says where it lives: irradiance cache (0.95, indirect), sky LUTs (0.26,
  resolution-independent), TAA at output resolution (~0.39 of the 1.01), particle simulation
  (~0.24), post (0.17), chunk bookkeeping.
- **Solving for targets**: 240 fps needs 0.389× (498×280); 144 fps needs 0.721× (923×519);
  120 fps needs 0.840× (1075×605); 100 fps needs 0.963× (1232×693).

**Both coefficients are properties of this camera, not of the renderer.** The same fit at a pose
looking out to open sea gives a far smaller resolution term — §9.1 measures 5.846 ms for a frame
with no geometry in it at all. Use the equation to reason about the *shape* of the trade, and the
per-pose tables in §9 and §10 for actual numbers.

A cross-check that the equation is about pixels and not about windows: a real 1920×1080 window
(which the window manager gives back as **1920×1055** — the requested height does not survive the
title bar on this 1536×864 display) measures **20.465 ms**, against 20.391 ms for the same pixel
count rendered inside a 720p window. The output window size itself is free; only the internal
render target costs anything.

---

## 5. Resolution and upscaling — the big lever, and where it starts to hurt

### 5.1 720p output

`docs/images/renderer/R02-tree-four-render-scales.png` — the conifer at 3×, nearest-neighbour, at
1.00 / 0.75 / 0.67 / 0.50 internal scale. Looking at it:

- **0.75** — very slightly softer than native. The individual 1–3 voxel needle clumps and the sky
  gaps between whorls all still resolve.
- **0.67** — visibly softer. Needle clumps begin to merge and the small dark holes in the canopy
  fill in. Still unambiguously a voxel tree with hard edges.
- **0.50** — the needle clumps merge into blobs and the top spire loses its shape. This is the
  first setting where the *voxel* character of the foliage is being eroded rather than just
  softened.

`docs/images/renderer/R01-vista-native-vs-067.png` — the same two frames whole. At full-frame
viewing distance the 0.67 image is not worse. It is arguably cleaner, and that deserves an honest
caveat: **the near grass at native resolution is full of hard black pockets** (visible in
`docs/images/renderer/R07-near-grass-render-scales.png`), which is the known voxel-heap hole defect of
`docs/SCENE.md §7.1` showing through at sub-pixel scale. Reducing resolution hides it. If that
defect is fixed, this comparison must be re-shot, because part of what looks like "no loss" here
is an artefact being blurred away.

### 5.2 1920×1055 output — the practically interesting case

`docs/images/renderer/R03-tree-1080p-taa-vs-fsr.png`, 2× nearest-neighbour on the conifer:

| Config | Internal | p50 ms | fps | What it looks like |
|---|---|---:|---:|---|
| native | 1920×1055 | 20.465 | 48.9 | reference |
| Kajiya TAA | 1440×791 (0.75) | 12.473 | 80.2 | not distinguishable from native at this zoom |
| **FSR 2.2** | **1280×703 (0.67)** | **10.809** | **92.5** | marginally softer; needle structure intact |
| Kajiya TAA | 1280×703 (0.67) | 11.088 | 90.2 | same, a shade less contrasty than FSR |
| Kajiya TAA | 960×527 (0.50) | 7.666 | 130.4 | clearly softer; clumps merging |
| FSR 2.2 | 960×527 (0.50) | 7.976 | 125.4 | as above; trunk marginally crisper than TAA |
| FSR 2.2, **reflections off** | 1440×791 (0.75) | **10.922** | **91.6** | the Balanced preset of §10 at 1080p |

**The headline of this section: 1920×1055 output from a 1280×703 internal render costs 10.81 ms —
the same as rendering 1280×720 natively (10.46 ms). The 1080p-class presentation is essentially
free relative to 720p native.** That is the single best-value setting found in this work. The last
row is the version to actually ship: 0.75 rather than the unreliable 0.667 (§9.2), reflections off,
and it lands at the same 10.9 ms.

FSR 2.2 and the built-in Kajiya TAA are within 0.3 ms of each other and within noise of each other
visually on this content. There is no reason to prefer one on cost. Turning the upscaler off
entirely (`TAA Method = None`) saves 0.88 ms (10.463 → 9.587) and costs all temporal
antialiasing — on 16 voxels/m content that is a bad trade and it is not recommended.

### 5.3 In motion, the upscalers cost real detail — and this is the honest picture

`docs/images/renderer/R08-upscalers-in-motion.png`, 3× on the grass, camera moving at 3.8 m/s,
frames captured at the same point on the same path. Looking at it:

- **native** — individual grass voxels and hard-edged flowers, with some temporal sparkle.
- **0.50, Kajiya TAA** — the grass texture becomes a mush; individual grass voxels are gone and the
  flowers survive only as blobs. There is visible **cyan/teal fringing** around the pink flowers
  that is not present at native — a chroma artefact of the upscaler under motion.
- **0.50, FSR 2.2** — the same amount of blur and the same fringing, flowers marginally better
  defined.
- **0.667, FSR 2.2** — partially recovers the grass texture; still clearly softer than native.

**Compare this against §5.1, where 0.50 on a still camera looked merely "soft".** The still images
flatter both upscalers substantially. That is the expected behaviour of a temporal upscaler on
16 voxels/m content — sharp sub-pixel-scale voxel edges are exactly what they handle worst — and
it is the reason the recommendation in §12 stops at 0.75 for anything that wants to look like the
still frames.

---

## 6. Every runtime setting that trades quality for speed

The complete list, from `grep -rn "AppSettings::add" src/` — these are all of them, not a
selection. Vista pose, 1280×720, stock binary.

| Setting | Value | p50 ms | Δ vs 10.463 | Verdict |
|---|---|---:|---:|---|
| `Graphics/Render Res Scale` | 0.75 | 7.259 | **−3.20** | see §5 |
| | 0.667 | 6.432 | **−4.03** | **unreliable — silently no-ops in ~1 run in 3, §9.2** |
| | 0.50 | 4.877 | **−5.59** | see §5 |
| | 0.40 | 3.974 | **−6.49** | see §5 |
| `Graphics/global_illumination` | false | 4.900 | **−5.56** | **destroys the look — §7.1** |
| `Graphics/TAA Method` | None (0) | 9.587 | −0.88 | loses all temporal AA. No |
| | FSR 2.2 (2) | 10.489 | +0.03 | equivalent to Kajiya TAA in cost |
| `Graphics/Render Shadows` | false | 10.186 | −0.28 | removes sun shadows for 2.7%. No |
| `Graphics/denoise_shadow_mask` | true | 11.508 | **+1.05** | already off; **leave it off** |
| `Graphics/Update Sky` | false | 10.810 / 10.046 | inconclusive by frame time | **broken — blanks the sky to black, §8.2** |
| `General/battery_saving_mode` | true | 16.633 | +6.17 | a `sleep_for(10ms)`; never enable |
| `Camera/*`, `Sun/*`, `Atmosphere*` | — | — | — | appearance only, no cost knob |
| `Player/*`, `UI/*` | — | — | — | not renderer settings |
| the F3 debug overlay | on | 10.925 | +0.46 | worth knowing when benchmarking |

`Graphics/Render Res Scale` and `Graphics/global_illumination` are the only two runtime settings
worth more than 1 ms, and one of them is not usable. **There is no free lunch already sitting in
the settings panel** other than resolution.

---

## 7. The GI stack

### 7.1 What is being protected, shown

`docs/images/renderer/R05-cave-gi-on-vs-off.png` is the regression pair, same pose, same binary,
28 s of convergence each:

- **GI on, 9.155 ms.** The tunnel is bathed in warm amber bounce from the crystals behind the
  camera, falling off toward the portal, with the meadow visible through the mouth.
- **GI off, 3.758 ms.** The tunnel is a uniform cold blue-grey. Not darker — *flat*. The emissive
  crystals contribute nothing at all; the only light is a constant sky ambient term.

That is the whole demonstration and it is unambiguous. **`global_illumination = false` is 5.4 ms
in this frame and it is not a quality setting, it is a different renderer.** It is listed in §6 for
completeness and as the cost budget for the GI stack; it is not a recommendation.

The reassuring half of the same test is
`docs/images/renderer/R04-cave-gi-variants.png` — the same frame at 2× with reflections off, at
0.667 internal and at 0.50 internal. **All three keep the amber bounce, the warm-to-cool gradient
along the tunnel and the meadow through the portal.** The variation between them is far smaller
than the gap to the GI-off frame. One honest note: the control frame reads cooler on the floor near
the portal than the other three, but auto-exposure differs between runs (`docs/SCENE.md §7.5`) and
that difference cannot be cleanly attributed to any one setting.

### 7.2 ReSTIR reflections — the one GI component that is nearly free to remove

There is no runtime switch for the reflection stack separately from the whole of GI, so one was
added **in the scratch tree only** (`C:\voxl2rs\src\renderer\kajiya\kajiya.hpp`, a
`Graphics/ray_traced_reflections` checkbox that routes `rtr` to a cleared image the way the GI-off
path already does). One trap worth recording: `RtrRenderer::next_frame()` swaps eight
`PingPongImage`s that only `RtrRenderer::trace()` ever creates, so skipping the trace and still
calling `next_frame()` takes an access violation two frames in. The guard is one line.

| | vista p50 | cave p50 |
|---|---:|---:|
| control | 10.539 | 9.155 |
| reflections off | 10.200 (profiled build: 10.170 vs 11.154) | 7.949 |
| per-pass RTR total | 1.074 ms | — |

**Measured cost: 0.98 ms at the vista (per-pass profiler, unambiguous), 1.21 ms in the cave.**

`docs/images/renderer/R06-reflections-on-vs-off.png`, 3× on the cave mouth: with reflections off
the rock face and the amber wash inside the mouth are slightly dimmer, and that is the entire
difference. The sea, checked separately, is pixel-for-pixel identical — it is a flat shaded surface,
not a reflective one. On matte voxel materials the reflection stack contributes a small broad
specular lift, not a visible reflection.

**Caveat, and it is the reason this is "take with eyes open" rather than "take":** the moment the
project has water, glass, ice or metal, this stack is what makes them work. Removing it buys 9% of
the frame in exchange for a material class the game does not have *yet*.

### 7.3 The GI quality parameters, and why none of them are the answer

Found by reading the stack; each was changed in the scratch tree, rebuilt (10–16 s) and measured
with the per-pass profiler against the same profiled control (11.154 ms `gpu_span`).

| Parameter | File | 4594a45 | Tried | `gpu_span` | Δ |
|---|---|---|---|---:|---:|
| `IRCACHE_SAMPLES_PER_FRAME` | `kajiya/ircache/ircache_constants.glsl` | 4 | 2 | 10.954 | −0.20 |
| `spatial_reuse_pass_count` | `kajiya/rtdgi.inl:211` | 2 | 1 | 10.865 | −0.29 |
| `MAX_STEPS` | `utilities/gpu/math.glsl:8` | 512 | 192 | 10.962 | −0.19 |
| `MAX_STEPS` | as above | 512 | 96 | 10.900 | −0.25 |
| `IRCACHE_CASCADE_COUNT` | `application/settings.inl:82` | 12 | 8 | 11.555 | **+0.40** |

Every one of them is at or under the 0.3 ms noise floor of §2.2, and one is a regression. Read
that as the finding it is: **the GI cost is not in its tuning parameters, it is in the number of
rays, and the number of rays is set by the pixel count.** `RtdgiTraceCompute` already runs at
*half* the render resolution (`rtdgi.inl:373`, `gbuffer_half_res`) and is still 1.95 ms.

Two of those rows are interesting beyond their size:

- **`MAX_STEPS` 512 → 96 changes nothing (−0.25 ms, and 192 and 96 are indistinguishable).** The
  hierarchical LOD DDA in `voxels/impl/trace.glsl` finds a hit or leaves the 64 m volume in well
  under 96 steps. **The step budget is free headroom for a much larger world** — which is directly
  relevant to putting mountains in the distance. It is not a saving here.
- **Shrinking the irradiance cache made things slower.** With 8 cascades instead of 12,
  `RtdgiTraceCompute` went *up*, 1.953 → 2.127 ms: cache lookups miss more often and fall back to
  longer traces. Do not shrink the ircache to save memory without measuring the trace.

Also read and considered, not changed: `RESTIR_TEMPORAL_M_CLAMP 250`,
`RTDGI_INTERLEAVED_VALIDATION_PERIOD 16`, `MAX_SAMPLE_COUNT 8` in `rtr/resolve.comp.glsl`,
`MAX_PATH_LENGTH 1` in the ircache (already the minimum — one bounce, then the cache is sampled at
the last vertex).

---

## 8. Temporal amortisation

### 8.1 What is already amortised

The kajiya port is not naive about this and the per-pass data shows it: ReSTIR diffuse spends every
16th frame validating instead of tracing (`RTDGI_INTERLEAVED_VALIDATION_PERIOD`), the irradiance
cache fires 4 rays per probe per frame and accumulates, and the DDA already resolves distant
geometry at a coarser LOD as a function of distance
(`hit_surface = lod < clamp(sqrt(t_curr · angular_coverage · VOXEL_SCL), 1, 7)`,
`voxels/impl/trace.glsl:105`). "Run this pass at a reduced rate for distant pixels" is largely
already implemented.

### 8.2 The one clear candidate that is not amortised: the sky — and the checkbox for it is broken

Five atmosphere LUT passes run **every frame** and cost **0.258 ms**, completely independent of
resolution (0.258 / 0.258 / 0.260 at 0.40× / 1.00× / 1.50×). `Sun/Animate` defaults to **false**.
Nothing in the transmittance, multiscattering or sky-view LUT changes from frame to frame in the
default configuration. That is ~2.5% of the frame recomputed for a sun that does not move.

`Graphics/Update Sky = false` makes the five passes disappear from the profiler CSV entirely, so
they are certainly skippable. Two things then go wrong.

**It is not measurable at frame level.** By frame time the two runs came out 10.810 and 10.046
against controls of 10.539 and 10.463 — once slower and once faster, both inside the ±0.3 ms noise
of §2.2. The profiled pair puts `gpu_span` at 11.154 → 10.948. **Call it 0.2–0.26 ms and treat it
as real per-pass but unproven per-frame.**

**And the checkbox as shipped does not freeze the sky, it blanks it.**
`docs/images/renderer/R09-update-sky-off-is-black.png` — same pose, same build, `Update Sky` on and
off. With it off, **the sky and the sea render pure black.** The terrain, the grass and the cave
glow are unaffected and correctly lit, so the LUTs the GI samples are evidently still valid; it is
the background that is lost. `Renderer::begin_frame` (`src/renderer/renderer.cpp:87`) does run the
sky graph once on `frame_index == 0`, which is clearly not sufficient — and note that
`record_tasks()` runs a second time on the first frame whenever the resolution scale differs from
1.0, which is exactly the configuration this was found in.

**Do not ship `Update Sky = false`, and do not quote any benchmark that used it.** The first
version of the preset table in §10 did, and had to be re-measured. The correct change is a dirty
flag on the sun/atmosphere settings so the three camera-independent LUTs recompute only when
something actually changed — leaving the aerial-perspective LUT, which is camera-dependent, on
every frame. Small, contained, worth about 2% of the frame, and it needs the black-sky bug
understood first.

### 8.3 What is not worth amortising

`TraceSecondaryCompute` (sun shadows, 0.808 ms) is the obvious "run at half rate" candidate, and
the engine already ships the machinery for the alternative — a shadow denoiser that would let the
mask be traced more cheaply. It is **off** by default and turning it **on costs 1.05 ms**
(10.463 → 11.508). Whatever it was tuned for, on this content it is a net loss. Leave it off, and
do not reach for it as a way to cheapen shadows.

---

## 9. What happens when the camera moves

Everything in §5 is a static camera with 22 s of accumulation, which is the case a temporal
upscaler is best at. `--patrol 12,20` flies a 12 m circle about local `(10,10,8)` in 20 s — 3.8 m/s,
a running pace — looking along the tangent. All rows below are the same path; the per-frame CSV
confirms the camera is at the same position to within 0.02 m at t = 10/15/20 s in every run.

| Config | Internal | p50 ms | fps | runs |
|---|---|---:|---:|---|
| native, Kajiya TAA | 1280×720 | 8.577 / 8.598 / 8.667 | 116 | m01, p01, q01 |
| 0.50, Kajiya TAA | 640×360 | 4.123 / 4.491 | 223–243 | m03, p02 |
| 0.50, FSR 2.2 | 640×360 | 4.108 | 243.4 | m04 |
| 0.667, FSR 2.2 | 853×480 | 4.045 | 247.2 | m05 |
| 0.667, Kajiya TAA | 853×480 | **8.635 / 8.639 / 8.536** | 116 | **see §9.2** |

### 9.1 It is not chunk generation, and resolution scaling does work while moving

The obvious hypothesis for "moving costs more" is world generation. It is wrong, and the profiler
says so: `VOXEL WORLD` is **0.031 ms** while moving. What actually changes is the GI, and it changes
a great deal (medians over the patrol, `p01-passes.csv` / `p02-passes.csv`):

| Group | still, sky-only pose | moving 1.0× | moving 0.5× |
|---|---:|---:|---:|
| RTDGI | 0.692 | 2.246 | 0.687 |
| IRCACHE | **0.088** | **0.584** | 0.572 |
| RTR | 0.435 | 0.805 | 0.257 |
| SHADOW | **0.102** | **0.531** | 0.185 |
| `gpu_span` | 5.846 | 8.713 | 4.610 |

The irradiance cache costs 6× more while moving (probes are allocated and validated for cells that
were not visible last frame) and the sun-shadow trace 5× more. Resolution scaling itself works
normally: 1.0× → 0.5× takes `gpu_span` from 8.713 to 4.610.

**The other number in that table is the most decision-relevant one in this document.** The
"still, sky-only pose" column is a frame containing *no voxel geometry at all* — camera at local
`(22,10,8)` looking level out to sea, verified by opening the capture: horizon, water, sky, nothing
else. It costs **5.846 ms at 1280×720**. That is a hard **171 fps ceiling at native 720p on an
empty world**, before a single voxel is drawn. 240 fps at native 1280×720 is not reachable in this
renderer by any means short of changing the renderer.

### 9.2 A real bug: `Render Res Scale = 0.6667` silently does not apply, about a third of the time

Three separate runs put 0.667 + Kajiya TAA in motion at exactly the native frame time. That is not
a plausible performance result, so it was checked against the images rather than the clock, using
the variance of a 3×3 Laplacian over a fixed region — an image rendered smaller and upscaled has
strictly less high-frequency energy whatever it looks like by eye.

Tree-silhouette region, `(700,10)–(900,260)`, calibrated against the known-good sweep:

| Run | Setting | lap. variance | Applied? |
|---|---|---:|---|
| `s00`, `z16` | native | 967, 1061 | — |
| `s07` | 0.75 | 925 | yes |
| `s08` | 0.667 | 860 | yes |
| `s09` | 0.50 | 807 | yes |
| `r04` | 0.40 | 759 | yes |
| **`q03`** | **0.667** (+2 others) | **1012** | **NO** |

Grass region of the patrol frames, same method: native 407/420/431; 0.50 → 235/244 (applied);
0.667 + FSR → 270 (applied); **0.667 + Kajiya TAA → 387/447/455 (not applied, three times)**.

So the setting takes effect sometimes and not others, with **no error, no warning, and a frame time
that looks like a legitimate "this scene is fixed-cost bound" result.**

**It is intermittent, not deterministic.** A dedicated diagnostic sweep re-ran the exact
configuration that failed as `q03` and it worked:

| Run | Binary | Setting | p50 ms | tree lap. var | Applied? |
|---|---|---|---:|---:|---|
| `q03` | scratch | 0.6667 + 2 others | 10.768 | 1012 | **no** |
| `d04` | scratch | 0.6667 + the same 2 others | 5.582 | 657 | yes |
| `d02` | scratch | 0.6667 alone | 6.564 | 882 | yes |
| `d01` | stock | 0.6667 alone | 6.122 | 720 | yes |
| `d03` | scratch | 0.70 alone | 6.663 | 899 | yes |

Same binary, same pose, same three settings, opposite outcomes. So it is a **race or a startup
ordering problem, not a bad value**. Overall it was observed in **4 of 11** runs that set 0.6667,
and never in any run that set 0.40, 0.50, 0.70, 0.75, 1.25, 1.50 or 2.00.

Where to look. `Render Res Scale` is applied not at load but on the first `on_resize`
(`src/voxel_app.cpp:405`), which compares the member `render_res_scl` against the setting and
re-records the whole task graph if they differ. `on_resize` is both called every iteration of
`run()` and installed as the GLFW framebuffer-size callback, and it reassigns `window_size` from
`swapchain.get_surface_extent()` in the middle of doing so. Related and worth fixing at the same
time: `round_frame_dim()` (`src/voxel_app.cpp:23`) is a **no-op whose rounding body is commented
out**, with the comment `// not necessary, since it rounds up!` — which is not true of
`static_cast<daxa_u32>(1280.0f * 0.6667f)` = 853. An odd internal width is not the cause here (0.70
gives an even 896 and 0.6667 succeeded five times at 853) but several passes derive half-resolution
extents as `(x + 1) / 2` and it should still be rounded to a multiple of 8 to match the workgroups.

**Practical guidance while this stands: prefer 0.75 or 0.50, and verify the setting took effect
rather than trusting the frame time** — the cheapest check is the Laplacian-variance one above, or
simply that the frame time moved at all. Every 0.667 figure quoted in §5 and §10 of this document
was checked against the sharpness metric before being quoted.

---

## 10. Two presets, measured at three poses each

There is no preset system in the engine (§11). These are two *settings combinations*, each measured
still, moving and inside the cave, so that nobody has to extrapolate from a single frame. Neither
includes `Update Sky = false`, for the reason in §8.2 — an earlier version of this table did, and
was measuring a black sky.

| | **Stock** | **Balanced** | **Performance** |
|---|---|---|---|
| `Render Res Scale` | 1.0 | **0.75** | **0.50** |
| `ray_traced_reflections` | on | **off** | **off** |
| everything else | default | default | default |
| **vista, still** | 10.463 ms · 96 fps | **7.097 ms · 141 fps** | **4.538 ms · 220 fps** |
| **patrol, moving** | 8.577 ms · 117 fps | **5.475 ms · 183 fps** | **3.720 ms · 269 fps** |
| **cave** | 9.155 ms · 109 fps | **5.923 ms · 169 fps** | **4.290 ms · 233 fps** |
| **1920×1055 out, FSR 2.2** | 20.465 ms · 49 fps | **10.922 ms · 92 fps** | — |
| speed-up: still / moving / cave | 1.00× | **1.47× / 1.57× / 1.55×** | **2.31× / 2.31× / 2.13×** |
| speed-up at 1920×1055 | 1.00× | **1.87×** | — |

`ray_traced_reflections` is a **scratch-tree-only** checkbox (§7.2); in `C:\voxl2` as it stands
there is no way to reach this configuration without the one-line change described there.

Both presets were verified to have actually applied, by the sharpness metric of §9.2 and by the
mean brightness of a sky patch (a check added after the `Update Sky` incident): tree Laplacian
variance 1060 → 919 → 755 across the three columns, sky mean 146.0 / 146.2 / 146.3.

**What each preset looks like.** `docs/images/renderer/R10-native-vs-presets.png` is all three at
the vista, full frame, same pose, same binary. Composition, colour, the sky and sea, the cave glow
and the flower palette are identical in all three. Balanced is a shade softer in the near grass and
is genuinely hard to tell from stock at viewing distance. Performance is visibly softer in the
grass but still unmistakably the same 16 voxels/m scene. **See §5.3 for what both do in motion** —
that is where the cost is real and where a still-frame comparison flatters them.

**Notice the shape of the table.** The speed-up is roughly constant across all three poses — it does
not collapse in the cave, where the fixed floor is proportionally largest, nor in motion, where the
GI is working hardest. That is the useful property: these are not settings that only help on a
still frame of open meadow. The **absolute worst frame** measured under Balanced anywhere in this
scene is 7.10 ms (141 fps), and under Performance 4.54 ms (220 fps).

---

## 11. Things that look misconfigured for this part

Checked, because the brief asked. Most of the obvious suspects came back clean.

| Suspect | Finding |
|---|---|
| A pass at full res that should be half | **No.** The depth prepass is at `render_res / PREPASS_SCL` (2), SSAO at `render_res / SHADING_SCL` (2), the ReSTIR diffuse candidate trace at half render res. The scaling of every pass across 0.40×/1.00×/1.50× is consistent with its declared size. |
| Workgroup sizes unsuited to Ampere | **No.** 65 of 72 compute shaders are `8×8×1` = 64 threads = 2 warps, which is a reasonable default for image-space work with 2D locality. A 1280×720 dispatch is 14 400 workgroups against 20 SMs; occupancy is not the problem. |
| A quality preset tuned for a 3070 | **No preset system exists.** There is one set of constants and no tiering. That is itself the finding: there is nowhere to put "Low/Medium/High" today, which is why §10 is expressed as settings rather than as a preset name. |
| Sky LUTs recomputed for a static sun | **Yes.** §8.2. ~0.26 ms/frame, ~2%. |
| `denoise_shadow_mask` | Correctly off; on it costs 1.05 ms (§8.3). |
| `FRAMES_IN_FLIGHT 1` (`settings.inl:95`) | **Flagged, not measured.** At today's 10.5 ms against 1.0 ms of CPU it cannot matter. At a 4.17 ms target it is a hard CPU/GPU serialisation point of the same order as the CPU frame. Raising it is not a one-line change — `gpu_input` is staged into a single buffer per frame — so it was left alone. |
| Hybrid graphics | The app is on the discrete GPU (§1). The residue: **VRAM peaked at 3.3–5.4 GB of 6.1 GB in these runs**, and the peak rises with internal resolution (3276 MiB at 0.5×, 5373 MiB at 2.0×). That is the mechanism behind §2.1 and it is a real shipping risk on this card, not just a measurement nuisance. |

---

## 12. Ranked

Vista pose, 1280×720 output, against the 10.463 ms control, unless a row says otherwise. "Saved" is
measured, not estimated; rows whose saving is below the ±0.3 ms noise floor of §2.2 are marked.

| # | Change | Saved | Visual cost, in one sentence | Verdict |
|---|---|---:|---|---|
| 1 | **Present 1920×1055 from a 1440×791 internal render (0.75) with FSR 2.2** | **8.0 ms** (20.465 → 12.473), or **9.5 ms** with reflections also off (→ 10.922) | Indistinguishable from native 1080p at 2× zoom on a still frame; slightly softer grass in motion. | **Take** — 1080p-class output for less than the price of 720p native |
| 2 | **`Render Res Scale` 0.75** at 720p output | **3.20 ms** | Needle clumps and canopy sky-gaps still resolve; near grass loses its finest contact detail. | **Take** — this is the knee of the curve |
| 3 | **Reflections off** (needs the §7.2 one-liner) | **0.98 ms** vista, **1.21 ms** cave | A slightly dimmer broad specular lift on matte voxels; the sea is pixel-for-pixel identical. | **Take now, revisit later** — the moment water, glass or metal exist this is what makes them work |
| 4 | **`Render Res Scale` 0.50** | **5.59 ms** (→ 205 fps) | Needle clumps merge; in motion the grass texture becomes mush with cyan fringing on flower edges. | **Take only as an explicit "Performance" tier**, not as a default |
| 5 | **`Render Res Scale` 0.40** | **6.49 ms** (→ 252 fps) | As above but more so; this is where the voxel character starts to go. | **Take only to hit a hard 240 fps number** |
| 6 | **Sky LUTs behind a dirty flag** | 0.2–0.26 ms (per-pass; not resolvable per-frame) | None, if implemented correctly. | **Take, but implement it** — the shipped `Update Sky` checkbox blanks the sky to black (§8.2) |
| 7 | ReSTIR diffuse spatial reuse 2 → 1 pass | 0.29 ms *(noise floor)* | Not assessed — too small for a screenshot pair to mean anything. | Leave |
| 8 | `denoise_shadow_mask` | **costs 1.05 ms** | — | Already off; **keep it off** |
| 9 | `IRCACHE_SAMPLES_PER_FRAME` 4 → 2 | 0.20 ms *(noise floor)* | Slower irradiance-cache convergence, i.e. worse in motion for no real gain. | Leave |
| 10 | `MAX_STEPS` 512 → 192 | 0.19 ms *(noise floor)* | None here — the DDA never reaches the cap in a 64 m world. | Leave, but **remember the headroom** when the world grows |
| 11 | `Graphics/Render Shadows` off | 0.28 ms *(noise floor)* | No sun shadows at all. | Leave |
| 12 | `TAA Method = None` | 0.88 ms | All temporal antialiasing gone, on the content that needs it most. | Leave |
| 13 | `IRCACHE_CASCADE_COUNT` 12 → 8 | **costs 0.40 ms** | Cache misses push `RtdgiTraceCompute` from 1.95 to 2.13 ms. | **Leave — it is a regression** |
| 14 | `global_illumination = false` | 5.56 ms | Destroys the coloured bounce light and the dark-cave behaviour entirely (§7.1). | **Leave** |
| 15 | `battery_saving_mode` | **costs 6.17 ms** | It is a `sleep_for(10ms)`. | Never enable |

### 12.1 So, the 240 fps question

**Reachable on this scene, today, with the path-traced GI intact — at 512×288 to 640×360 internal
upscaled to 720p.** Measured: 3.974 ms / 252 fps at 0.40 scale alone; and on the Performance preset
(0.50 + reflections off) **3.720 ms / 269 fps moving, 4.290 ms / 233 fps in the cave, 4.538 ms /
220 fps standing at the vista.**

Three qualifications, all of them measured rather than hedged:

1. **It is not free.** §5.3 shows what 0.50 does to grass in motion. This is a soft image.
2. **It is capped.** The fixed floor is 3.03 ms (§4) — **330 fps is the ceiling** at any resolution,
   and a frame with *no voxels in it at all* still costs 5.846 ms at native 720p (§9.1), so
   **240 fps at native 1280×720 is not reachable by tuning**.
3. **It is this scene.** 37 m of island, no mountains. The term of the equation that will grow
   fastest with world size is the one that resolution cannot touch: the irradiance cache is driven
   by probe count, which follows visible world volume.

The most useful framing for the actual goal — a big map with mountains far away and grass close —
is not "240 fps at 720p" but **"1920×1055 at 12.5 ms / 80 fps with everything intact, or 5.5 ms /
183 fps at 720p on the Balanced preset"**, and then spending the difference on world size rather
than on frame rate.

---

## 13. What this work did not determine

Stated plainly, so nothing here is quoted beyond what it measured.

1. **Anything about a bigger world.** Every number is the 37 m Voxl test scene. The §4 equation's
   two coefficients will both move when the world grows, and the fixed term is the one to watch:
   the irradiance cache scales with *probe* count, which scales with visible world volume, not with
   pixels. Nothing here predicts it.
2. **Anything about distant mountains.** The scene has none. The one relevant datum is negative and
   useful: `MAX_STEPS` is not currently a limit (§7.3), so ray length is not what stops the view
   from being longer.
3. **What the motion bottleneck is**, beyond the profiler evidence in §9.
4. **Whether the 0.67-scale conclusion survives the hole defect being fixed** (§5.1).
5. **FSR 2.2's real quality**, as opposed to its cost. It was compared on a static camera, which is
   the case temporal upscalers are best at, and on a 22 s converged frame.
6. **`FRAMES_IN_FLIGHT`**, and anything else that needs a change too invasive for a research phase.
7. **Long sessions.** The longest continuous run here was 34 s.
8. **The mechanism of either bug in §8.2 and §9.2.** Both are reproduced and characterised; neither
   is diagnosed.

---

## Appendix — reproducing any row

Nothing in `C:\voxl2` was changed by this work except this file and
`docs/images/renderer/`. The one source change — the `Graphics/ray_traced_reflections` checkbox —
lives only in the scratch mirror `C:\voxl2rs`, which is a byte copy of `C:\voxl2` reconfigured to
build from its own sources (`CMAKE_HOME_DIRECTORY:INTERNAL=C:/voxl2rs`; a copy that keeps the
original CMake cache will silently compile `C:\voxl2`'s sources instead, which cost one wasted
build here). An incremental rebuild there is 10–16 s.

**Run it from the directory the exe is in.** The shader root paths in
`src/utilities/gpu_context.cpp:18` are relative (`"src"`, `"assets"`), so a launch with the wrong
working directory resolves them somewhere else on disk and the process aborts with `0x80000003`
before printing a single line — no message, no log. Two ad-hoc runs at the end of this session were
lost to it and briefly looked like a broken build.

The three poses, straight from the engine's own command line, no harness:

```powershell
$rel = 'C:\voxl2\.out\cl-x86_64-windows-msvc\Release'
$exe = "$rel\gvox_engine.exe"      # and Start-Process -WorkingDirectory $rel

# vista  -- the 10.463 ms control
& $exe --unpause --exit-after 24 --width 1280 --height 720 `
       --pos -182.99,-109.98,-46.97 --rot 0.785,1.096 --no-overlay `
       --screenshot vista.png --screenshot-after 22 --bench-csv vista.csv

# cave   -- the GI regression frame, 9.155 ms
& $exe --unpause --exit-after 30 --width 1280 --height 720 `
       --pos -164.6,-91.3,-49.7 --rot 3.927,1.571 --no-overlay `
       --screenshot cave.png --screenshot-after 28 --bench-csv cave.csv

# patrol -- 12 m circle in 20 s, 3.8 m/s, 8.577 ms
& $exe --unpause --exit-after 26 --width 1280 --height 720 `
       --pos -173,-100,-44.5 --patrol 12,20 --no-overlay `
       --screenshot motion.png --screenshot-after 24 --bench-csv motion.csv
```

Quality settings have no command line. Write them into
`%APPDATA%\GabeVoxelGame\user_settings.json` before launching (and set `UI/autosave` to `false`, or
the run writes its own state back over yours). Per-pass GPU timings:
`$env:VOXL_GPU_PROFILE = 'passes.csv'`.

Then the statistic: p50 of `full_ms` over `t ∈ [8 s, screenshot-after − 0.5 s]`, and **check
`Render Res Scale` actually applied** before believing any resolution row (§9.2).
