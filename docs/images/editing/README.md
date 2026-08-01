# Editing verbs: before / after

One composite per brush. **Left is before, right is after; magenta is the seam.** Both halves are
the same binary, the same seed, the same pose and the same convergence time — the *only* difference
between them is the `--edit` argument. The tool HUD is in both halves, so the tool name and the
radius that produced the change are legible in the image itself rather than trusted from a script.

## How these were made

`--edit` drives the brush from the command line. No synthetic mouse or keyboard input is involved:

```powershell
$exe = 'C:\voxl2\.out\cl-x86_64-windows-msvc\Release\gvox_engine.exe'
# before
& $exe --pos -182.99,-109.98,-46.97 --rot 0.7854,1.0964 --unpause `
       --screenshot before.png --exit-after 20 --no-overlay
# after -- identical but for the --edit
& $exe --pos -182.99,-109.98,-46.97 --rot 0.7854,1.0964 `
       --edit 10,remove-terrain,3,0.5 `
       --screenshot after.png --exit-after 20 --no-overlay
```

`--edit T,BRUSH,RADIUS[,HOLD][,rmb]` is repeatable and implies `--unpause`. A brush name that does
not resolve is fatal at startup (exit 2), so a typo cannot silently produce a run of the default
tool — which is exactly the failure the synthetic-input harness kept producing.

**Pitch: 1.571 is level, SMALLER LOOKS DOWN.** Getting this backwards stuck a campfire to the
tunnel ceiling and photographed it. See the comment on `--rot` in `src/application/cli.hpp`.

## What each one proves

| File | Pose (scene-local) | Verdict |
|---|---|---|
| `v01-remove-terrain-AB.png` | spawn | 6 m crater, soil rim, stone floor. Terrain removed. |
| `v02-add-terrain-AB.png` | spawn | Stone ball fills the view. Note the HUD's RMB label flips to "Remove terrain" — `BRUSH_SECONDARY_ID`. |
| `v03-grass-AB.png` | spawn | Pale ball with grass only on its upper shell. |
| `v04-remove-grass-AB.png` | spawn | Bare earth mown out of the meadow; **the ground shape is unchanged** and the rim is intact. |
| `v05-flowers-AB.png` | spawn | Yellow and purple flowers scattered over the brush footprint; grass and terrain untouched. |
| `v06-light-ball-AB.png` | `11,11,2.6` | Emissive ball, and **the whole tunnel turns warm** — GI responding to a runtime edit. |
| `v07c-lantern-AB.png` | `13,13,2.3` | Dark box frame with lit panels; tunnel washed gold. |
| `v08c-fire-AB.png` | `13,13,2.3` | Flame cone on a dark base; tunnel washed orange. |
| `v09d-torch-AB.png` | `13,13,2.3` | Post with a flame on top; walls washed warm. |
| `v10-maple-AB.png` | `2,2,6` | Whole maple placed at the crosshair. HUD reads "click to place" — one-shot. |
| `v11-spruce-AB.png` | `2,2,6` | Conifer at the crosshair, confirming the `pos_offset` fix. |
| `v12-remove-rmax-AB.png` | `2,2,14` | **Chunk-boundary straddle.** 16 m sphere on a 4 m chunk grid — 4-5 chunks per axis. The bowl is one continuous surface: no steps, no notches, no missing 4 m cubes. |
| `v13-add-rmax-AB.png` | `2,2,14` | The same straddle additively, where a seam would be easier to see. Also continuous. |

The black patches on the rock dome are `docs/SCENE.md` §7.1's known heap defect. They are present
in the *before* halves too, and they move between byte-identical runs, so they are not an edit
artefact — do not read them as one.

## Prop scale

Lantern, fire and torch are authored in absolute voxel sizes, so `radius` *scales* them:
`brush_prop_scl() = clamp(radius / 2.0, 0.4, 4.0)`. At radius 1 m they are half size and a few
pixels across at 7 m — which reads as "the brush did nothing". `v07`/`v08`/`v09` at radius 1 m were
exactly that false negative; the captures kept here use radius 2-4 m at 2-3 m range.
