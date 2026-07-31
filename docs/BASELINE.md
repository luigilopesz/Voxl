# voxl2 — Stage 0 baseline

**Recorded:** 2026-07-31
**Commit:** `1e2fef0` — *Import gvox_engine as voxl2, with the four fixes that make it build here*
**Upstream base:** `da8e4923` on `compute-rt` (GabeRundlett/gvox_engine, last upstream commit 2024-11-19)

This is the reference every later change is measured against. Each number below is followed by
the command that produced it. Nothing here is inherited from an earlier session except where a
row explicitly says **not re-measured**.

---

## 1. What was measured

The engine's own demo world, unmodified, at 1280×720 on the machine described in §6. The
world is a fixed 128 m cube: `CHUNK_SIZE 64`, `CHUNKS_PER_AXIS 32`, `LOG2_VOXEL_SIZE (-4)`
(`src/voxels/impl/voxel_malloc.inl`). That last constant is the whole point of the pivot and
must not change.

**16 voxels per metre, re-confirmed at runtime** from the debug overlay in both screenshots —
`Player Pos 0.986 → Player Pos (voxel) 15.771` and `0.052 → 0.829`. Both are ×16 to within the
overlay's 3-decimal rounding.

---

## 2. Frame time

Two states, because they differ by 3.1 ms and quoting only one would be misleading. Both come
from a single run of the command in §2.3, and both are visible in the committed screenshots.

### 2.1 Moving over dense terrain — `docs/images/01-baseline-moving-frametime.png`

Captured the instant a 30-second forward-walking soak ends, so the overlay's 200-frame ring
buffer (`src/application/ui.hpp:28`, ≈5 s of history) contains nothing but moving frames.

| Measurement | Value |
|---|---|
| Full frame-time | **avg 21.11 ms (47.37 fps)**, min 15.23, max 22.53 |
| CPU-only frame-time | avg 1.02 ms (982.36 fps), min 0.88, max 1.59 |
| GPU Heap | **1376256 pages (2906.65 MB)** |
| GPU Heap Usage | 1629.15 MB |

### 2.2 Settled, elevated vista — `docs/images/00-baseline-demo-world.png`

Captured after backing out of the hillside the soak walked into, then holding still for 15 s so
the temporally-accumulated GI converges and the ring buffer refills.

| Measurement | Value |
|---|---|
| Full frame-time | **avg 17.97 ms (55.65 fps)**, min 15.09, max 18.87 |
| CPU-only frame-time | avg 0.97 ms (1035.94 fps), min 0.75, max 1.36 |
| GPU Heap | **1376256 pages (2906.65 MB)** |
| GPU Heap Usage | 636.76 MB |

### 2.3 Command

```powershell
powershell -File C:\voxl2\tools\run.ps1 -Overlay -ExpandGraphs -Soak -Seconds 30 `
    -SoakShot   docs\images\01-baseline-moving-frametime.png `
    -Screenshot docs\images\00-baseline-demo-world.png -Quit
```

### 2.4 A third state, for context

Stationary at spawn, before any movement, the heap has not yet grown:

| Measurement | Value |
|---|---|
| Full frame-time | avg 23.41 ms (42.72 fps), min 18.68, max 24.58 |
| GPU Heap | **786432 pages (1660.94 MB)** |
| GPU Heap Usage | 1319.03 MB |

**Read those two heap figures together.** Thirty seconds of walking takes the heap from 786432
to 1376256 pages — it grows by 75% and then never comes back down, because
`src/utilities/allocator.inl` has no shrink path. `GPU Heap Usage` moves freely (636 MB at the
vista, 1629 MB inside the terrain); the *capacity* only ratchets up.

---

## 3. VRAM

`nvidia-smi --query-gpu=memory.used --format=csv,noheader` sampled once per second by
`tools/run.ps1`. Four consecutive runs of the same command, to show the spread:

| Run | Before launch | Peak during 30 s soak | After exit |
|---|---|---|---|
| 1 | 280 MiB | 4591 MiB | 287 MiB |
| 2 | 273 MiB | 4590 MiB | 284 MiB |
| 3 | 278 MiB | 4593 MiB | 282 MiB |
| 4 | 279 MiB | 4592 MiB | 288 MiB |

**Peak 4590–4593 MiB of 6144 MiB — 74.8%.** Spread across four runs is 3 MiB, so treat any
future change smaller than about ±10 MiB as noise. Idle desktop baseline is 273–288 MiB, and
every run returned to it: **no leak, clean release on exit.** The process exits with code **0**
on `WM_CLOSE`; it is not being killed.

Stationary, with no soak at all, the same sampling gives a peak of **3384–3417 MiB** — so the
30-second walk is worth about 1.2 GB of VRAM. Quote the moving figure, not the stationary one.

Raw command, if you want it without the harness:

```powershell
& "$env:SystemRoot\System32\nvidia-smi.exe" --query-gpu=memory.used --format=csv,noheader
```

---

## 4. The headroom problem, reproduced exactly

The observed heap of 1376256 pages is *identical* to the figure in
`docs/design/GVOX_ENGINE_EVALUATION.md` §3.5 — the same ceiling was hit here independently.
At `VOXEL_MALLOC_PAGE_SIZE_BYTES` = 2112:

```
current heap   : 1376256 pages = 2906.65 MB
next grow step : 2064384 pages = 4359.98 MB      (check_for_realloc: next * 3 / 2)
transient peak : 7266.63 MB                      (old and new buffers held simultaneously)
RTX 3050 6 GB  : 6144 MiB total, 4592 MiB already in use at peak
```

There is no VRAM cap, no OOM check and no degradation path in `check_for_realloc`
(`src/utilities/allocator.inl:264`). One more growth step does not fit and will produce a
device-lost, not a stutter. **This is the Stage 2 work item and this baseline is the evidence
for it.** It was not triggered here — reaching it needs heavier editing or a denser generator.

Also unfixed and worth knowing before you trust the overlay past 2,033,601 pages:
`src/voxels/impl/voxel_world.cpp:213` computes
`current_element_count * VOXEL_MALLOC_PAGE_SIZE_BYTES` in `daxa_u32` *before* the cast to
`double`, so the MB figure silently wraps. At 1376256 pages we are well below that, so the
2906.65 MB above is trustworthy.

---

## 5. Build

```powershell
powershell -File C:\voxl2\tools\build.ps1 -Config Release
```

| Build | Elapsed | Transient peak disk |
|---|---|---|
| No-op (`ninja: no work to do`) | 0 s | 0 GB |
| Full recompile of the engine's own sources — 15 TUs + blue-noise-sampler + link, 16 ninja edges | **10 s** | ~0 GB |
| First build after the move: CMake reconfigure + vcpkg re-verify + full recompile | 45 s | 0.59 GB |
| Cold build from an empty `.out` and no vcpkg clone | **not re-measured** — recorded in the evaluation as 25–30 min, peak 14.2 GB |

The 0.59 GB in row 3 is vcpkg downloading its own pinned toolchain (CMake 4.4.0, 7zip 26.02,
PowerShell 7.6.3) into `vcpkg/downloads` on first use after the move. It is a one-off.

The gap between rows 2 and 4 is the whole story of this build: the engine's own C++ is trivial
to compile, and essentially all of the cold-build cost is vcpkg building 46 packages from a
2023-era baseline. Do not clean `.out` casually.

### Disk

| Path | Size |
|---|---|
| `.out/` (build tree + `vcpkg_installed` + SPIR-V cache) | 2.969 GB |
| `vcpkg/` (clone made inside the source tree) | 0.722 GB |
| `deps/` (vendored Daxa, gvox, blue-noise-sampler) | 0.015 GB |
| `vksdk/` (synthesised minimal Vulkan SDK) | 0.013 GB |
| `.git/` | 0.013 GB |
| everything else (`src`, `assets`, `docs`, `cmake`, `packaging`, `tools`) | 0.010 GB |
| **Total `C:\voxl2`** | **3.74 GB** |
| **Committed** | **580 files, 37.72 MB of content, 13.18 MiB packed** |

C: had 33.08 GB free after all of the above.

---

## 6. The machine

| | |
|---|---|
| CPU | 13th Gen Intel Core i7-13650HX, 14C/20T |
| GPU | NVIDIA GeForce RTX 3050 6GB Laptop GPU, 6144 MiB, driver 610.74 |
| RAM | 16 GB |
| OS | Windows 11 Pro 10.0.26200 |
| Compiler | MSVC 19.44 (`cl.exe` 14.44.35207, VS 2022 Build Tools) |
| CMake / Ninja | 3.31.6-msvc6 and the Ninja bundled with VS Build Tools |
| Vulkan | **No LunarG SDK installed.** Build uses `vksdk/`; runtime uses the driver's own loader 1.4.341.0 |

The discrete GPU is selected correctly over the Intel iGPU — the overlay reads
`GPU: NVIDIA GeForce RTX 3050 6GB Laptop GPU`.

---

## 7. Startup

Window appears **0.53–0.55 s** after launch, consistently across four runs. That is *not* the
whole startup: roughly 127 GLSL shaders keep compiling behind the window for several seconds
afterwards, and **keystrokes sent during that period are silently dropped**. `tools/run.ps1`
waits `-SettleSec` (default 8) before sending any input for exactly this reason; the first
attempt at this baseline lost its F3 to it.

The SPIR-V cache at `.out/spirv_cache` went 103 → 188 entries on the first run after the move
and 188 → 206 over the runs that followed, so **the cache key does not survive relocating the
tree** and the first post-move launch pays full compilation cost again.

---

## 8. Reproducing this exactly

```powershell
# build
powershell -File C:\voxl2\tools\build.ps1 -Config Release

# measure
powershell -File C:\voxl2\tools\run.ps1 -Overlay -ExpandGraphs -Soak -Seconds 30 `
    -SoakShot   docs\images\01-baseline-moving-frametime.png `
    -Screenshot docs\images\00-baseline-demo-world.png -Quit
```

Two things will trip you up, both handled by the scripts but worth knowing:

- **`show_debug_info` is persisted** to `%APPDATA%\GabeVoxelGame\user_settings.json` and F3
  *toggles* it. A blind F3 turns the overlay **off** whenever the previous run left it on.
  `run.ps1` reads the persisted value first and only taps F3 when it needs to.
- **The frame-time graphs are collapsed ImGui tree nodes**, and ImGui does not persist tree
  state in `imgui.ini`. They must be clicked open on every run — `-ExpandGraphs` does it, at
  client coordinates calibrated for 1280×720 (`-ExpandY1 15 -ExpandY2 175`).

Running the app rewrites `imgui.ini` (the Debug Menu's auto-computed height). That is a
tracked file; the committed value is the collapsed default.

---

## 9. What this baseline does *not* cover

Stated plainly, so nobody quotes a number this document did not measure.

1. **1920×1080.** Everything here is 1280×720. The renderer already shades at half resolution
   (`PREPASS_SCL`/`SHADING_SCL` 2 in `src/application/settings.inl`) and ships FSR 2.2, so
   1080p is expected to be 720p-internal upscaled rather than native.
2. **The cold build.** Row 4 of §5 is inherited, not re-measured.
3. **Heap behaviour under editing.** §4 is arithmetic on a measured heap size plus a read of
   the allocator; the growth failure was not triggered.
4. **Long sessions.** The longest continuous run here was about 70 s.
5. **A Voxl-authored world.** This is Gabe's demo terrain, generator and palette. Nothing in
   §2 predicts what a different generator does to the heap — that is Stage 1's job.
