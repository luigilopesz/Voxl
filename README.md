# voxl2

A path-traced voxel engine at **16 voxels per metre**, forked from
[GabeRundlett/gvox_engine](https://github.com/GabeRundlett/gvox_engine) at `da8e4923`
(branch `compute-rt`, last upstream commit 2024-11-19).

It is the successor to Voxl (`C:\Users\luigi\projects\Voxl`), a rasterised OpenGL 4.5
sandbox with 1-metre blocks. Voxl still builds, still passes its 335 tests, and is kept as
a reference library — portable subsystems (job system, audio, physics, gameplay) are copied
across from it as they are needed, not merged. The reasoning behind the pivot, the memory
analysis and the staged plan are in
`C:\Users\luigi\projects\Voxl\docs\design\GVOX_ENGINE_EVALUATION.md`. That decision is made;
this repository executes it.

---

## What it actually is

There are no triangles for terrain. For every pixel, a compute shader walks a ray forward
through the voxel grid until it hits something, then fires more rays — one at the sun for
shadows, and a spray that bounces off surfaces to gather light from wherever they land.
That bouncing is why red rock tints nearby grass warm: the light genuinely bounces. A
handful of random rays per pixel is extremely noisy, so most of the renderer is not tracing
rays but denoising them — reusing results across neighbouring pixels and across frames
until the noise averages out.

Concretely:

| | |
|---|---|
| API | Vulkan 1.3 through [Daxa](https://github.com/Ipotrick/Daxa) 3.0.2. **No hardware ray tracing anywhere** — 57 `.comp.glsl` files, zero `.rgen`/`.rchit`. Weak RT cores are irrelevant to it. |
| Voxel size | 1/16 m. `LOG2_VOXEL_SIZE (-4)` in `src/voxels/impl/voxel_malloc.inl`. **This is the look the project exists for. Do not change it.** |
| Voxel format | 32 bits: 2 material type, 4 roughness, 8 octahedral normal, 18 colour at 6:6:6. **No UV coordinates exist anywhere in the codebase.** Every surface is per-voxel colour. |
| World | A fixed **128 m cube** — 64³ chunks, 32 per axis — that wraps around the player like a torus. Nothing outside it exists, and nothing is written to disk. |
| Lighting | Physical sky, plus a world-space irradiance cache feeding screen-space ReSTIR diffuse and ReSTIR reflections. There are **no punctual lights at all**; every light except the sun is an emissive voxel resolved through the GI cache. |
| Grass | **Not voxels.** 4.19 M GPU-simulated blades rasterised into the same G-buffer, so they do not bounce light, do not appear in reflections, and cast shadows only through a 2048² ortho map covering 40 m. The engine is a hybrid, not a pure path tracer. |

It is an **application, not a library**: one `add_executable`, no exported headers, no
plugin ABI. 79% of `src/` is GLSL and shader-shared headers; only 21% is C++. Using it means
editing it in place.

Measured on this machine (RTX 3050 6 GB laptop, 1280×720, the imported demo world):
**20.13 ms / 49.68 fps** moving over dense terrain, **12.92 ms / 77.37 fps** stationary at
spawn, peak **4588 MiB of 6144**. Full numbers, the commands that produced them and the
exact binary each was taken against are in `docs/BASELINE.md` and
`docs/benchmarks/index.csv`. Quote a row, not a memory — the tree changes underneath these
figures, which is why `index.csv` pins every run to an `exe_mtime` and `exe_bytes` as well
as to a commit.

---

## Build

```powershell
pwsh -File tools\build.ps1                 # incremental Release
pwsh -File tools\build.ps1 -Config Debug
pwsh -File tools\build.ps1 -Reconfigure    # force a fresh CMake configure
pwsh -File tools\build.ps1 -Clean          # delete .out, full rebuild (~25-30 min, peaks ~14 GB disk)
```

`build.ps1` exists because a plain `cmake --preset` does not work here, and every reason is
commented in the script. In short: it imports a `vcvars64` environment (the toolchain file
reads `MSVC_ENV_Path` but `cmd` exports `PATH` uppercase, so `cl` is otherwise not found),
pins `VCPKG_MAX_CONCURRENCY=2` (higher spawns up to 84 concurrent `cl.exe` on 16 GB and
fails with `FAILED: [code=4294967295]` and no diagnostic), and points `VULKAN_SDK` at the
vendored `vksdk/`.

**The LunarG Vulkan SDK is not installed and does not need to be.** `vksdk/` is a minimal
synthesised SDK — Vulkan-Headers v1.3.260 plus an import library generated from the NVIDIA
driver's own `vulkan-1.dll`. At runtime nothing from it is used; the driver's loader
suffices.

**The repository must stay at a short path.** `cmake/vcpkg.cmake` clones vcpkg *inside* the
source tree, and from a deep root the result exceeds `MAX_PATH` and git aborts with
`Filename too long`. `LongPathsEnabled` is `0` on this machine and `subst` does not help.
`C:\voxl2` is chosen for exactly that reason.

Incremental builds of the engine's own sources take about 10 seconds. A cold build takes
25–30 minutes, essentially all of it vcpkg compiling 46 packages from a 2023-era baseline.
**Do not clean `.out` casually.**

### Four source fixes are already applied

Upstream does not compile with MSVC 19.44 as-is. All four are in the import commit:
missing `<iostream>` in `src/utilities/debug.cpp`, missing `<chrono>` in
`deps/gvox/src/adapters/parse/magicavoxel.cpp` and
`deps/gvox/src/adapters/serialize/gvox_palette.cpp`, plus the synthesised `vksdk/`.

`deps/Daxa`, `deps/gvox` and `deps/blue-noise-sampler` are **vendored**, not submodules, and
are built as vcpkg overlay ports from local source — so an edit there is on the build's
critical path, not a reference.

---

## Run

```powershell
pwsh -File tools\run.ps1                                        # launch and leave running
pwsh -File tools\run.ps1 -Overlay -ExpandGraphs -Soak -Seconds 30 -Quit
```

Controls are the engine's own: `ESC` toggles pause and mouse capture, `F3` toggles the debug
overlay, WASD moves.

Two things will trip you up on a first run, and both are handled by the scripts:

- **The window appears in ~0.5 s but ~127 GLSL shaders keep compiling behind it for several
  seconds**, and keystrokes sent during that period are silently dropped.
- **`show_debug_info` is persisted** to `%APPDATA%\GabeVoxelGame\user_settings.json` and F3
  *toggles* it, so a blind F3 turns the overlay **off** if the last run left it on.

### The engine's command line

**Added 2026-07-31**, closing the integration note that used to sit here. Full rationale and
every flag: `src/application/cli.hpp`; the parser is `src/application/cli.cpp`. With no
arguments the engine behaves exactly as it did before.

| Flag | What it does |
|---|---|
| `--pos X,Y,Z` | player position in absolute world metres — the sum the overlay shows as `Player Unit Offset` + `Player Pos`. The camera is 0.2 m below it. |
| `--rot YAW,PITCH` | radians, matching `Player Rot (Y/P/R)`. Pitch 1.571 is level; **smaller looks down**. |
| `--lock-camera` | freeze the pose and drop all input. Implied by `--pos`; `--no-lock-camera` opts out. |
| `--patrol RADIUS,PERIOD` | fly a closed horizontal circle about `--pos`, one lap per PERIOD seconds, looking along the tangent. |
| `--exit-after S` | quit cleanly at a frame boundary, exit code 0. |
| `--screenshot PATH` `--screenshot-after S` | write the **swapchain** to a PNG — not the window, so nothing overlapping it can get into the shot. |
| `--bench-csv PATH` | one row per frame: time, full and CPU frame time, heap capacity/usage/cap, camera pose. |
| `--overlay` / `--no-overlay` | force the debug overlay, rather than toggling it. |
| `--expand-graphs` | force the two frame-time tree nodes open. |
| `--unpause` | start with the game running instead of in the pause menu. |
| `--width N` `--height N` | window size. Was hardcoded 1280×720. |
| `--seed N` | world seed. |

A bad value for any option is **fatal** (exit 2), not ignored. That is deliberate: this machine's
locale renders `-26.5` as `-26,5`, `--pos` splits on commas, and for one afternoon the result was
capture runs framed from the default spawn with the requested *rotation* applied — images that
looked entirely plausible and were of the wrong place.

Times are measured from the first rendered frame, not from process launch, so shader compilation
(0.7 s warm, 20–40 s on a cold SPIR-V cache) does not eat into a fixed-length run.

```powershell
# One reproducible screenshot, plus a per-frame CSV and 4 Hz VRAM sampling.
pwsh -File tools\shot.ps1 -Name 13-cave-mouth -Local "7.4,7.4,4.6" -Rot "0.785,1.42" -ConvergeSec 18

# Five-minute moving soak on a closed circle.
pwsh -File tools\shot.ps1 -Name soak -Local "6,6,5.5" -Rot "0,1.45" -Patrol "13,30" `
     -ConvergeSec 296 -Seconds 300 -Bench -Graphs
```

`tools/shot.ps1` takes `-Local` in the scene-local frame of `docs/SCENE.md` and converts; it
prints exit code, VRAM before/peak/after, and frame-time mean/p50/p99/max straight from the CSV.
`tools/crop.ps1` magnifies a region with nearest-neighbour sampling, which is the only correct
filter for judging whether something is one voxel or two.

---

## Measure

Every performance claim in this repository has a command behind it. That is not a style
preference: the project's central risk is the voxel heap allocator's growth path
(`src/utilities/allocator.inl`, `check_for_realloc`), which holds the old and the new buffer
simultaneously while growing by 1.5×. As imported it had no cap, no out-of-memory check and
no shrink path, and one growth step from the observed heap size does not fit in 6 GB.
Capping it is Stage 2 of the plan. "It felt fine" does not detect any of that.

```powershell
# 30 s walking forward, which is what actually grows the heap.
pwsh -File tools\bench.ps1 -Label baseline-soak -Soak -Seconds 30

# Read frame time and heap size off the overlay PNG it captured, then record them.
pwsh -File tools\bench.ps1 -Amend <run-id> -FrameTimeMs 20.13 -Fps 49.68 `
     -HeapPages 1376256 -HeapUsageMb 1624.15
```

`bench.ps1` streams GPU memory, utilisation, clock, power and temperature from `nvidia-smi`
at 2 Hz (or faster), writes one CSV per run plus an appended summary row to
`docs/benchmarks/index.csv`, and captures the debug overlay as evidence. Sub-second sampling
is not decoration: a 5210 MiB transient spike that 1 Hz sampling misses entirely was caught
at `t=1.508 s` of a 90-second soak.

`bench.ps1` does **not** read frame time programmatically — it predates `--bench-csv` and drives
the app with synthetic keystrokes. **For anything new, use `tools/shot.ps1`**, which passes
`--pos/--rot/--patrol/--bench-csv/--screenshot/--exit-after` to the engine, sends no keys, reads
no window, and reports mean/p50/p99/max from 30 000-row CSVs. `bench.ps1` is kept because
`docs/benchmarks/index.csv` and the pre-2026-07-31 rows in it were produced by it.

Screenshots for visual review go through `tools/shot.ps1` (or `tools/capture.ps1` for cropping an
already-running window); the conventions and the existing images are catalogued in
`docs/images/README.md`.

---

## Layout

```
src/            engine source, 206 files — 79% GLSL/.inl, 21% C++
  application/  window, input, player, settings, ImGui
  renderer/     the frame graph; renderer/kajiya/ is the GI stack
  voxels/       chunk storage, the page allocator, brushes, particles
deps/           vendored Daxa, gvox, blue-noise-sampler (vcpkg overlay ports)
vksdk/          synthesised minimal Vulkan SDK
tools/          build.ps1, run.ps1, capture.ps1, bench.ps1
docs/           BASELINE.md, benchmarks/, images/, README.md (upstream brush docs)
.out/           [gitignored] build tree, vcpkg_installed, SPIR-V cache — ~3 GB
vcpkg/          [gitignored] the clone cmake/vcpkg.cmake makes inside the tree
```

---

## Licence and attribution

**This engine is MIT and a commercial, closed-source game may ship on it.** `LICENSE` is the
unmodified MIT template, "Copyright (c) 2023 Gabe Rundlett". Every dependency is permissive:
Daxa (MIT), gvox (MIT), kajiya (MIT, Embark Studios), FFX (MIT, AMD), assimp (BSD-3),
glfw and minizip (Zlib), imgui/glm/fmt/nlohmann-json/stb (MIT).

### Roughly half of `src/` is Embark Studios' renderer

`src/renderer/kajiya/` is **14,640 lines across 103 files, out of 31,118 in `src/` — 47.0%**
(`find src -type f -exec cat {} + | wc -l`, 2026-07-31). Its own README says the essential
parts are nearly direct ports of Embark Studios'
[kajiya](https://github.com/EmbarkStudios/kajiya). The path-traced GI here is Embark's
algorithm, ported by Gabe Rundlett. It is MIT and it may be shipped, but *"I wrote a path
tracer"* would not be an accurate sentence, and maintaining 14.6k lines of someone else's
renderer alone is part of the deal. The MIT text travelled with it and is already in the
tree at `src/renderer/kajiya/LICENSE-MIT` — keep it there.

### Three inherited defects that must be fixed before shipping

These are gvox_engine's, not this project's, and they are inherited silently. None is a
blocker; all three are an afternoon's work. They are **not yet fixed here**.

1. **`assets/STBN.zip` is NVIDIA's, with no licence and no attribution.** 64 entries named
   `stbn_vec2_2Dx1D_128x128x64_*.png`, verbatim NVIDIA's naming from `NVIDIA-RTX/STBN`.
   There is no NVIDIA notice anywhere in the tree. NVIDIA's licence is dual: a
   Non-Commercial Use License restricted to "research or evaluation purposes only", and a
   Commercial Use License that permits shipping but requires the notice *"This software
   contains source code provided by NVIDIA Corporation."*. Elect the commercial branch and
   add the notice, or regenerate the blue-noise textures with NVIDIA's published generator.
   Regenerating is the clean answer.

2. **FreeImage is GPL-2.0 OR GPL-3.0 OR FIPL, and it is a required linked dependency**
   (`CMakeLists.txt:43,71`) used for exactly three things: init/deinit
   (`src/main.cpp:33,38`), the window icon (`src/application/window.hpp:73-90`), and
   decoding the STBN PNGs (`src/utilities/gpu_context.cpp:182-187`). `stb` is *already* a
   declared dependency. Swapping in `stb_image` deletes the whole GPL-adjacent subtree —
   including **libraw**, which is LGPL-2.1/CDDL and which the build ships as a DLL
   (`CMakeLists.txt:114`), obliging you to permit relinking and supply source. It also
   removes `jxrlib`, the port behind upstream's most-reported build failure. This is a
   licence win, a build win and a size win at once.

3. **`packaging/infos/license.txt` reads, in full, "Gabe Rundlett / All Rights Reserved"** —
   and it is wired into the installer as `CPACK_RESOURCE_FILE_LICENSE`
   (`CMakeLists.txt:126`). Any installer built today presents an All-Rights-Reserved EULA.
   Reported upstream as issue #12 in 2024 and closed without changing the file.

### Notices still owed

There is **no `THIRD_PARTY_NOTICES.md` in this repository yet**, and shipping requires one.
It must carry MIT/BSD/Zlib notices for Gabe Rundlett (gvox_engine, gvox), Ipotrick (Daxa),
**Embark Studios (kajiya)**, **AMD (FFX denoiser, FSR2)**, Jasper-Bekkers
(blue-noise-sampler), assimp, glfw, imgui, glm, fmt, nlohmann/json, platform_folders,
nativefiledialog, minizip and stb — plus an Apache-2.0 NOTICE for Roboto Mono and an OFL
notice for Inter Tight. Voxl's `docs/THIRD_PARTY_NOTICES.md` is the starting point; this one
will be considerably longer.

---

## Upstream, and what that means

Last upstream commit **2024-11-19**; the last feature work was 2024-03-29. Single author,
474 of 483 commits. Four of seven open issues are build failures, the newest with no
maintainer reply. Twenty-three forks, all plain mirrors — there is no evidence anyone but
the author has ever built a project on this. Daxa itself is healthy but the pin is ~349
commits behind its master.

The `upstream`, `upstream-daxa` and `upstream-gvox` remotes are configured so a fix can
still be cherry-picked, but **plan on owning this codebase outright.** That is the deal.
