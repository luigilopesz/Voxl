# Side-by-side comparisons

Every significant change in this project was captured as a **pair** — a control and
a treatment, framed identically from the same camera position — because a single
screenshot cannot show that a change did what it claimed. These are those pairs,
stitched together so the comparison is the thing you look at.

Regenerate the whole set at any time:

```powershell
./tools/build_comparisons.ps1
```

Pairs are declared in that script rather than discovered, because only a person
knows which two images are meant to be compared and what the comparison is for.
Add a row there when you capture a new pair. A missing source is skipped with a
warning, so it stays runnable while captures are still being produced.

Individual images can be stitched directly:

```powershell
./tools/montage.ps1 -Left before.png -Right after.png -LeftLabel "Before" -RightLabel "After" -Title "What changed" -Out out.png
```

---

## The set

| | What it shows | Why it matters |
|---|---|---|
| `01-gi-cave-lit-vs-dark.png` | An emissive crystal lighting a sealed chamber, beside the same chamber with the light removed | **The most important image in the project.** The control proves the cave is genuinely enclosed, so no sunlight reaches it — which means every bit of light on the left is the crystal bouncing off the walls. That is path-traced global illumination working, demonstrated rather than asserted. |
| `02-gi-cave-lookback.png` | The same pair looking back toward the tunnel | The faint cool light in the control is skylight bleeding down the tunnel — the gradient you would expect, not a uniform fill. |
| `03-scene-vs-demo-world.png` | Our island against gvox_engine's own demo world, same build | Both at 16 voxels/m. Ours: 10.93 ms, 830 MB heap. Demo: 17.97 ms, 2906 MB. The engine is not the limit; the content is. |
| `04-subvoxel-bore.png` | A bore carved through a slab, before and after the UV-phase fix | Wall stepping far finer than one block is what proves the carve is genuinely *sub-voxel* rather than block removal. |
| `05-lod-rings.png` | Distant terrain with chunk LOD off and on | 124× fewer triangles per chunk at distance — 6.4× more world for 1.5× the geometry. |
| `06-lod-ocean-fix.png` | An ocean-horizon artefact at LOD boundaries, fixed | |
| `07-glowstone-emission.png` | An emissive block before and after light propagation existed | Block light travelling independently of sunlight. |
| `08-night-horizon-fix.png` | The night horizon before and after fog was matched to the sky | A hard seam where fog and sky disagree is the most common way an otherwise good sky looks wrong. |
| `09-white-artifact-fix.png` | A washed-out white artefact, diagnosed and fixed | |
| `10-engine-generations.png` | The first working render of the original engine, beside the current one | 1 m rasterised blocks → 16 voxels/m path traced. 4096× more voxels per unit volume, and per-voxel colour instead of texture mapping. |

## How to read these

The **left** panel is always the control or the earlier state; the **right** is the
treatment or the current state. Both panels are scaled to a common height at their
own aspect ratio, so a full frame and a zoomed crop can sit together without either
being distorted.

Where a comparison exists to prove a feature works — `01` and `02` especially —
the control is the load-bearing half. It is easy to make a lit cave; the evidence
that the lighting is *real* is that removing the light makes it dark.
