# Validation screenshots

Every image in this directory was captured from the running engine and used to verify a
feature or diagnose a defect. They are numbered so a directory listing reads in the order
the work happened, and named for **what the image proves**, not for what it depicts —
`03-cave-interior-is-dark.png` is worth more in six months than `cave.png`.

The convention is carried over unchanged from the previous engine
(`C:\Users\luigi\projects\Voxl\docs\images\`), because the review habit is the same one.

---

## How to capture

**Use `tools\shot.ps1`.** Since 2026-07-31 the engine takes `--pos/--rot/--screenshot/
--exit-after/--overlay/--expand-graphs`, so a capture no longer involves sending keystrokes at a
window and reading pixels back off the screen.

```powershell
# One shot. -Local is the scene-local frame of docs/SCENE.md; -Pos is absolute world metres.
pwsh -File tools\shot.ps1 -Name 13-cave-mouth -Local "7.4,7.4,4.6" -Rot "0.785,1.42" -ConvergeSec 18

# With the debug overlay and both frame-time graphs open, plus a per-frame CSV.
pwsh -File tools\shot.ps1 -Name 18-debug-overlay -Local "0.01,0.02,5.53" -Rot "0.785,1.096" `
     -ConvergeSec 30 -Seconds 34 -Graphs -Bench

# Magnify a region to judge voxel-scale detail. NEAREST-NEIGHBOUR, never smoothed.
pwsh -File tools\crop.ps1 -In docs\images\12-foliage-closeup.png -Region "540,55,240,200" `
     -Scale 4 -Out zoom.png
```

Four things this fixed, each of which had already cost a wasted run:

- **The image is the swapchain, not the screen.** `capture.ps1` BitBlts the desktop (a Vulkan
  swapchain is presented by the compositor and never goes through the window's device context,
  so `PrintWindow` returns black). That works, but it means the window must be visible and
  unobscured. `--screenshot` reads the rendered image itself.
- **The overlay is forced, not toggled.** `show_debug_info` is persisted to
  `%APPDATA%\GabeVoxelGame\user_settings.json` and F3 *toggles* it, so a blind F3 turns it
  **off** half the time. This produced one baseline screenshot with no overlay in it.
  `--overlay` / `--no-overlay` set it outright.
- **The frame-time graphs open by flag, not by click.** `bench.ps1` synthesised a click at
  `$cw - 290 + 14`, computed from the panel's width — and the panel auto-sizes to its widest
  row and is pinned to the right edge, so any new debug string moved the target and the click
  landed on the world, silently. `--expand-graphs` replaces the whole mechanism.
- **The camera is where you asked.** Two runs of the same command now frame the same thing, so
  a difference between two images is a difference in the build.

`tools\capture.ps1` is still the tool for cropping an already-running window, and
`tools\bench.ps1` still writes overlay crops into `docs\benchmarks\` — those are measurement
evidence, not visual review, and they do not belong here.

---

## What is here

| File | What it proves |
|---|---|
| `00-baseline-demo-world.png` | Stage 0 baseline: the inherited demo world at `CHUNKS_PER_AXIS 32`, 17.97 ms, heap 2906 MB. |
| `01-baseline-moving-frametime.png` | The same, moving. |
| `10-scene-wide-sunlit.png` | The whole 37 m island in one frame: hill, cave mouth, conifer, flowering meadow, sea, sky. |
| `11-tree-full-height.png` | The conifer at full height, 6.6 m / 106 voxels, bare trunk and nine whorls. |
| `12-foliage-closeup.png` | **Needles resolve as individual 1–3 voxel clumps with sky through the canopy** — the thing that fails if the foliage is a solid SDF. |
| `13-cave-mouth.png` | The portal from outside, with the crystal glow visible through it, and the flower palette. |
| `14-cave-interior-lit.png` | The emitter and the amber wash on the chamber walls. Also shows the auto-exposure problem (§7.5 of SCENE.md). |
| `15-cave-interior-dark-control.png` | **The control.** Identical pose and build, `VOXL_DEBUG_NO_LIGHT 1`. Near-black. |
| `16-cave-lookback-gi.png` | The best GI frame: amber bounce in the near tunnel, cool sky bounce at the far end, meadow through the portal. |
| `17-cave-lookback-dark-control.png` | Its control. Tunnel black except the portal disc. |
| `18-debug-overlay.png` | Overlay legible: 11.11 ms (90.01 fps), heap 393216 pages / 830 MB, usage 13 MB (2%), cap 4129 MB. |
| `19-soak-5min-patrol.png` | End of the 300 s / 38 496-frame soak. Heap unchanged. |
| `20-ab-demo-world-same-build.png` | The demo world at the **identical pose in the identical binary** (`VOXL_TEST_SCENE 0`): 12.83 ms, heap in use 146 MB against this scene's 12.7 MB. |
| `21-defect-bare-heightfield-control.png` | `VOXL_DEBUG_BARE_HEIGHTFIELD 1` — the dome with no holes, which is what localised the defect. |
| `22-defect-hole-closeup.png` | The hole with its rim, close up. This is why SCENE.md §7.1 now says "holes" where it used to say "not holes". |

The `shapes/`, `union.png`, `difference.png` … files are upstream's SDF documentation figures,
not captures.

## How to reproduce a capture

**You cannot yet, exactly.** The old engine took debug flags that pinned the camera, the
seed, the render distance and the time of day, so any shot could be retaken pixel-for-pixel.
This engine takes **no command-line arguments at all** — `src\main.cpp:22` is
`auto main() -> int`, with no `argc`/`argv` anywhere in `src\` or `deps\Daxa\src\`. The
world seed is hardcoded at `src\voxel_app.cpp:125` and the spawn point at
`src\application\player.cpp:46`, so a fresh launch is repeatable, but anything reached by
walking is not.

Until that is fixed, **record the camera position printed in the overlay** (`Player Pos`,
`Player Rot`) in the table below for any shot worth retaking. The precise source changes
that would restore reproducibility are written up as an integration note in the footer of
`tools\bench.ps1`.

---

## 00 — Stage 0 baseline

The engine's own demo world, unmodified, at 1280×720. This is what every later change is
compared against; the numbers are in `docs\BASELINE.md`.

| Image | What it shows |
|---|---|
| `00-baseline-demo-world.png` | Settled elevated vista after a 30 s soak and a 15 s hold, so the temporally-accumulated GI has converged. Overlay reads 17.97 ms (55.65 fps), heap 1376256 pages. |
| `01-baseline-moving-frametime.png` | Captured the instant the soak ends, so the overlay's 200-frame ring buffer (≈5 s) contains nothing but moving frames: 21.11 ms (47.37 fps), heap usage 1629.15 MB. The two images differ by 3.1 ms and quoting only one would be misleading. |

**16 voxels per metre is confirmed in both**, and it is the single thing worth re-checking
in every future shot: `Player Pos 0.986` against `Player Pos (voxel) 15.771` is ×16.
`LOG2_VOXEL_SIZE (-4)` is the look this project pivoted for and must not change.

---

## Inherited images — not ours, do not renumber

These came with gvox_engine and are referenced by `docs\README.md`, the upstream brush
documentation. They are diagrams of SDF operators, not captures of this project's work:

| Path | What it is |
|---|---|
| `shapes\*.png` (19 files) | One render per SDF primitive — box, capsule, torus, and so on. |
| `union.png`, `intersection.png`, `difference.png` | The three boolean combination operators. |
| `smooth_union.png`, `smooth_intersection.png`, `smooth_difference.png` | Their smoothed variants. |

Leave them where they are: `docs\README.md` links to them by relative path.

---

## Naming

```
docs/images/NN-what-the-image-proves.png            while a stage is still open
docs/images/NN-stage-name/NN-what-it-proves.png     once the stage closes and shots group
```

Numbers restart inside each stage directory. Prefer a name that states a *finding* —
`04-bore-through-slab-CONFIRMED-subvoxel-resolution.png` from the old project is the model.
Keep failed or misframed shots if they explain why something exists; annotate them in the
table rather than deleting them, because a shot that documents a wrong turn is the cheapest
form of institutional memory this repository has.
