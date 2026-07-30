# Validation screenshots

Every image here was captured from the running game and used to verify a feature
or diagnose a defect. They are grouped by the milestone that produced them and
numbered so a directory listing reads in the order the work happened.

Captures are reproducible: the game takes debug flags that pin the camera, the
seed, the render distance, the time of day and the sub-voxel test rigs, so any
shot can be retaken. Run `voxl.exe --help`, or see `tools/capture.ps1` and
`tools/organise_images.ps1`.

---

## 01 — Vertical slice

The first milestone: window, camera, one chunk, collision, raycast, break/place.

| Image | What it shows |
|---|---|
| `01-first-successful-render-forest-and-mountains.png` | First render that produced real terrain — snow-capped mountains, forest, plains, distance fog. |
| `02-after-side-texture-rotation-fix.png` | After fixing side textures that were rotated 90° on two of four faces, because the mesher's tangent frame is chosen for winding, not for texture orientation. |

## 02 — Level of detail

Distant chunks generated and meshed at 1/2, 1/4 and 1/8 resolution. The defect to
hunt in these is a **crack or hole at a band boundary**.

| Image | What it shows |
|---|---|
| `01-lod-long-view-radius-20.png` | Long view across all four LOD bands at render distance 20. |
| `02-zoom-dark-recess-confirmed-not-a-crack.png` | Pixel-sampled to prove a dark slot was an unlit recess (R28–62), not a crack showing sky (R154 G186 B231). |
| `03-default-spawn-reference.png` | Unmodified spawn, the baseline other shots are compared against. |
| `04..05-lod-rings-*` | LOD banding from above, enabled vs disabled. |
| `06-lod-debug-overlay.png` | F3 overlay showing per-level resident/visible/triangle counts. |
| `07..09-lod-seam-*` | Band edges under deliberately hostile band settings, checking skirts cover the height disagreement. |
| `10..14-lod-ocean-*` | An ocean-horizon artefact, before and after its fix, plus LOD-disabled control. |
| `15..16-lod-stress-*` | Many visible chunks at once, with and without LOD. |

## 03 — Destructible sub-voxels

Each block virtually contains an 8×8×8 grid; only damaged blocks cost anything.
The thing to look for is **stepping finer than a block**, which is what proves the
carve is genuinely sub-voxel and not just block removal.

| Image | What it shows |
|---|---|
| `01..03-*` | Three failed framings — the rig anchors to the player, so overriding `--pos` moved the target. Kept because they document why `--carve-at` exists. |
| `04-bore-through-slab-CONFIRMED-subvoxel-resolution.png` | **The confirming shot.** A bore carved through a raised slab, daylight visible through the far mouth, wall stepping far finer than one block. |
| `05..08-bore-*` | Bore under visual review, and the state before the UV-phase fix. |
| `09..10-crater-rig-*` | A sphere carved from the natural terrain surface; the rim is where damaged blocks sit against intact ones. |
| `11..12-*` | Carved slab and tunnel interior. |
| `13..16-*` | The same rigs once light propagation existed, with a lighting-disabled control. |

## 04 — Light propagation

Before this, every voxel sat at full sunlight and caves were as bright as open
ground. The check is simple: **open ground uniformly bright, caves dark, with a
smooth gradient between them and no step at a chunk border**.

| Image | What it shows |
|---|---|
| `01..05-daylight-*` | Open ground, standing and aerial, plus horizon and water detail. |
| `06-cave-interior-is-dark.png` | The headline result — a cave that is actually dark. |
| `07..09-cave-mouth-*` | The gradient from daylight to darkness at a cave mouth, including a scanline analysis. |
| `10-cave-at-night.png` | Cave lighting with no sun contribution. |
| `11..14-cave-fog-*` | Interaction between cave darkness and distance fog at noon and midnight. |
| `15..16-glowstone-*` | An emissive block before and after — block light propagating independently of sunlight. |
| `17..22-white-artifact-*` | A washed-out white artefact, its diagnosis and its fix. |
| `23-water-shoreline-zoom.png` | Water at a shoreline. |

## 05 — Day/night and sky

Sun arc, moon, stars, and colour graded across the cycle. The likeliest defect is
**a hard seam at the horizon** where fog colour stops matching the sky.

| Image | What it shows |
|---|---|
| `01..04-time-of-day-*` | The same view at dawn, noon, dusk and night. |
| `05..06-*-horizon-fog-sky-match.png` | Horizon close-ups checking fog tracks the sky. |
| `07..08-sky-dome-*` | Sky gradient at dawn and noon. |
| `09..10-night-*` | Star field at zenith, and the moon. |
| `11..12-night-horizon-before/after-fix.png` | A horizon defect and its correction. |
| `13..16-crop-*` | Tight crops used to judge fine gradients that are invisible at full size. |
| `17..18-difference-*` | Difference images isolating a changed star field and a terrain band. |

## 06 — Menus and settings

| Image | What it shows |
|---|---|
| `01-main-menu-title-screen.png` | Title screen with world creation and load. |
| `02..03-pause-menu*` | Pause menu. |
| `04..06-settings-panel*` | Settings, including render distance, FOV and the frame limiter. |

---

## A note on the flat files

Images also still exist under `docs/images/` with their original capture-time
names (`ml_tod_dusk.png`, `vr_lod_seam_hard.png`). Those are the originals; the
copies above are the readable arrangement. The flat set is removed once no capture
process is still referring to it — see `tools/organise_images.ps1 -Move`.
