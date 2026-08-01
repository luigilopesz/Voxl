# Handoff — state as of 2026-07-31

Written at a session boundary. Everything below is committed; the working tree is
clean and the build is green.

## Where things stand

`C:\voxl2` is a fork of GabeRundlett/gvox_engine — a Vulkan/Daxa path-traced voxel
renderer at 16 voxels per metre. Upstream is dormant (last commit 2024-11), so this
codebase is ours to maintain. Six commits, builds with `./tools/build.ps1`.

**This repository has no remote.** All of it is local only. That is the single
biggest risk right now and the first thing worth fixing.

The previous engine — an OpenGL rasterised voxel sandbox, 335 tests — is at
`C:\Users\luigi\projects\Voxl` and is pushed to `github.com/luigilopesz/Voxl`. It
is kept as reference; several of its subsystems (physics, audio, persistence, UI)
are portable and none of them exist here yet.

## What works, and is verified

- The engine builds and runs at 10.93 ms / 91.5 fps on the RTX 3050, 37 m island.
- Path-traced GI is real, and the proof is `docs/comparisons/01-gi-cave-lit-vs-dark.png`:
  a sealed chamber lit only by an emissive crystal, beside the same chamber with the
  crystal removed and near-black. The control is the evidence.
- A GPU profiler exists (`VOXL_GPU_PROFILE=out.csv`), timing 94 of 96 passes.
- A full frame breakdown, resolution curve and trade curve are in
  `docs/PROFILE.md` and `docs/design/PERFORMANCE_PLAN.md`.

## What is landed but NOT verified

The last workflow's four implementation agents completed; its verification agent
and three adversarial reviewers were cut off by a session limit before running.
So the following builds clean and is self-reported, but has not been checked by
anything other than the code that produced it:

- **The optimisations.** VOXL_DATA_DIR settings isolation, CLI quality knobs,
  `round_frame_dim` rounding, the reflections toggle, and Quality/Balanced/
  Performance presets with Balanced shipped as default.
- **The editing tools.** Brush id and radius on `BrushInput`, `brushgen_a`/`_b`
  switching on the id, radius honoured by every brush, radius-aware chunk
  election, `brush_add_ball`, a tool selector and a game HUD.

**Start the next session by running that verification.** The workflow script is at
`scratchpad/v2s3.js` and can be resumed with `resumeFromRunId` so the completed
agents replay from cache rather than re-running.

## The three measurement traps

Each of these has already corrupted a result in this project. Honour them.

1. **Shared settings file.** All quality settings lived in one file under
   `%APPDATA%`, so parallel engine instances silently overwrote each other. This
   reversed the sign of one agent's headline result. `VOXL_DATA_DIR` now fixes it —
   the negative control is 3.67 ms unset versus 6.78 ms set, because unset it reads
   a poisoned config and renders less.
2. **A second engine instance** roughly doubles frame time; two at 1080p thrash
   6 GB of VRAM into 500 ms frames. Check the GPU is uncontended for the whole run.
3. **A failed shader compile silently deletes passes** rather than failing loudly.
   A frame that got faster may simply have stopped drawing something. Always open
   the image.

And one environment trap: `main.cpp` walks up from the working directory looking
for `.out` or `assets`. The old engine has an `assets/` directory, so launching the
voxl2 binary from there silently rebinds it to the wrong tree and Daxa aborts with
`0x80000003` and no message. Always pass an explicit working directory.

## The performance answer

240 fps is reachable with GI intact — measured 220 fps standing, 269 moving — but
only at 640×360 internal upscaled to 720p, on a 37 m island with no mountains.

At native 720p it is arithmetically impossible: deleting the entire GI stack leaves
4.900 ms, and a frame with no voxel geometry at all still costs 5.846 ms, both above
the 4.17 ms budget. There is a fixed 3.030 ms floor that caps the engine near
330 fps regardless of configuration.

The finding that reframes the work: **a half-resolution render with full GI costs
5.264 ms, and a full-resolution render with no GI costs 5.264 ms** — identical to
three decimals from two independent runs. Pixels are the lever, not features.
Cost fits `frame_ms = 3.030 + 7.520 × internal_megapixels`.

Recommended target is the Balanced tier at ~120 fps, not 240.

## Next, in order

1. **Give this repository a remote.** It is one machine failure from gone.
2. **Run the verification** that was cut off — resume `scratchpad/v2s3.js`.
3. **Fix the surface artefacts.** Black patches on the hill and rock, confirmed not
   holes, not the cave SDF, not the normal policy. Documented as defect #1 in
   `docs/SCENE.md` and visible in most captures.
4. **Build one far-field level.** `PERFORMANCE_PLAN.md` §5 proposes four nested
   volumes at coarser voxel sizes; build the first (25 cm voxels, 128 m radius) and
   it converts the whole cost budget from arithmetic into a measurement. Note first
   that a ray which MISSES is the most expensive ray in the engine, and that 512
   steps at 6.25 cm reaches only 32 m against a 111 m box diagonal — so distant
   geometry will show holes before it shows slowdown.
5. **The asset pipeline.** The reference engine's vegetation is professional polygon
   models (PlantFactory, Quixel Megascans) voxelised, not procedurally generated.
   That is very likely the largest single reason its scenes look authored and ours
   look procedural, and it needs no renderer change. Research was cut off before it
   ran; the brief is at `scratchpad/bmf2.js`.

## Known limitations

- Black surface artefacts (defect #1 above), unattributed.
- The scene is 37 m across; the world is a 64 m cube with nowhere to put a mountain.
- `brush_add_ball` writes plain grey — correct, but crude as level design.
- The bench-CSV settings header is designed but not landed; it must move out of the
  constructor, which runs before CLI overrides are applied and would record
  pre-override settings.
- No persistence, no audio, no player physics beyond the engine's own naive
  point-sampling collision. The old engine has better versions of all three.
