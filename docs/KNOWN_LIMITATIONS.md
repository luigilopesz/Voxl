# Known Limitations

An honest account of what is missing, deferred, or working only within stated
bounds. Nothing here blocks normal sandbox play unless it says so.

Last verified against commit following the LOD / sub-voxel milestone, on a
13th Gen Core i7-13650HX (14C/20T), RTX 3050 6GB Laptop, 16 GB RAM, Windows 11.

## Not implemented

Light propagation, persistence, audio, menus/settings, the sub-voxel mining verb
and the day/night cycle all landed in the milestone that follows. What is listed
below is what those systems do **not** do.

| Area | Bounds it works within |
|---|---|
| **Light propagation** | Levels 0 and 1 get a full flood; levels 2 and 3 get a top-down sky sweep only, so a cave in the far ring is lit as though it had a roof and no walls. It is never visible at that distance. With `StreamingConfig::verticalRadius < kWorldSectionCount` the sections above a column are absent, an absent chunk counts as an opaque wall, and the world comes out **completely dark** — the shipped default loads the whole column, but `test_lod_stream.cpp` runs at `verticalRadius = 1` and its worlds are simply unlit. |
| **Persistence** | Only level-0 chunks are written; coarser levels come from the seed. No hardware write barrier (`std::fstream::flush()` reaches the OS, not the platter), so a power cut can lose the one chunk being written — bounded by a per-payload CRC, and that chunk regenerates. No region compaction: a single long session that repeatedly rewrites growing payloads leaves holes. Light is **not** restored from disk; a loaded chunk is re-lit by the normal pipeline. |
| **Audio** | Amplitude panning plus a distance low-pass, not HRTF: front/back is disambiguated only by a mild gain and brightness cue. Voices are capped, not stolen — past 48 simultaneous voices new plays are dropped and counted in `AudioStats::voicesDropped`. PCM is uncompressed float32, 6.2 MiB resident. |
| **Menus and settings** | No key rebinding; the Controls tab says so on screen. `readSettings` preserves unknown keys verbatim but discards hand-written comments and section headers on the next save. |
| **Day/night** | The moon is always full — `FrameUniforms` carries no phase input. Water depth for absorption is estimated from the view slant and the mesher's AO rather than a depth prepass. |
| **Main menu world switching** | Creating or loading a world from the title screen rebuilds the terrain generator and the save, and unloads every chunk — but the `World` object itself is reused rather than reconstructed. That is deliberate (see the comment on `Application::closeWorld`); the practical limit is that streaming config changes still go through `applySettings` rather than through world construction. |

## Level of detail

- **LOD does not reduce CPU voxel memory.** A coarse chunk still allocates a
  full `ChunkStorage`; only meshing, GPU buffers and draw calls get cheaper.
  Resident voxel data is roughly 111 MB at render distance 20 and grows linearly
  with the loaded volume. Storing coarse chunks at their own resolution is the
  fix and has not been done.
- **Chunks at the edge of the loaded region cannot change level.** A rebuild
  needs all neighbours resident to mesh borders correctly, so the outermost ring
  keeps whatever level it was first generated at until the player moves and it
  is no longer on the rim.
- **Player-edited chunks refuse LOD transitions** (`preserveEditedChunks`). A
  transition regenerates the chunk from the seed, which would erase the edit.
  This is deliberate and only resolves once persistence exists to reload the
  edit after a rebuild. The practical effect is that heavily built-in areas stay
  at the level they were edited at.
- **Band widths are constrained.** Every LOD band must be at least
  `hysteresis + 2` chunks wide or a chunk can be demoted and never promoted
  back. The shipped defaults `{5, 9, 14}` with `hysteresis = 2` sit exactly on
  that limit, so narrowing the first band or raising the hysteresis breaks it.
  `ChunkManager::setLodPolicy` clamps and warns rather than trusting the caller;
  the rule is documented in `src/world/Lod.hpp`.
- **Cracks are hidden with skirts, not stitching.** A coarse chunk hangs a
  curtain of geometry around its border. This cannot crack, but it does draw a
  few hundred triangles per coarse chunk that are usually invisible. Stitching
  would be cheaper in triangles and far more complex, and for cube geometry it
  produces a transition band that greedy meshing cannot merge.

## Sub-voxel destruction

- **No gameplay verb drives it.** The full pipeline works — sparse store,
  mesher, dedicated GPU buffers and shader program, sub-voxel collision and
  raycasting — but nothing in normal play creates damage. The break timer,
  per-sub-voxel targeting reticle, and tool rules that would decide when a swing
  chips a block instead of removing it are a gameplay design that has not been
  written. The primitive is exposed on **F6** (carve the sub-voxel under the
  crosshair) as a debug affordance, and `--carve crater|tunnel|both` builds test
  rigs. Normal left-click still removes a whole block, unchanged.
- **Only opaque materials can be damaged.** Sub-voxel geometry is one stream
  drawn through one program with no alpha cutoff and no blending, so carving a
  Cutout or Translucent block (leaves, glass, ice — all full cubes and valid
  raycast targets) would turn a see-through block solid. `World::editSubVoxel`
  rejects those edits. Splitting the sub-voxel mesh into per-layer index ranges
  with their own blend state is the fix, deferred until something needs it.
- **Merging is within one block.** Greedy merging runs inside a single 8³ grid
  and never across two adjacent damaged blocks, which is why the extent fields
  are 3 bits. Carving a wide tunnel emits one quad set per damaged block rather
  than one for the whole surface. Acceptable because the path only runs on blocks
  the player has damaged; widening extents to 9 bits is the obvious next step if
  damage ever becomes widespread.
- **Lighting is inherited per block, not per sub-voxel.** All 512 sub-voxels of a
  block share its light and ambient-occlusion values. Per-sub-voxel lighting
  would multiply the lighting cost by 512 for a difference invisible at 1/8-block
  scale.
- **Sub-voxel damage is ignored at LOD > 0.** A damaged block is treated as solid
  in coarse chunks. Honouring 1/8-block detail at LOD distance would defeat the
  purpose of LOD.
- **`SubVoxelEdit` cannot distinguish "block materialised in air" from
  "sub-voxel restored".** There is no `BlockAdded` result, so a caller restoring
  the first sub-voxel into empty air may under-invalidate seam neighbours. Only
  reachable through the restore path, which no gameplay verb currently uses.

## Platform and rendering

- **vsync is advisory on hybrid-graphics laptops.** With `GLFW_SRGB_CAPABLE` and
  swap interval 1, this machine still runs at ~1250 fps at radius 8 because the
  Optimus WGL swap-control is not honoured. Nothing may assume `swapBuffers()`
  throttles the loop. A real frame limiter was scoped with the settings work and
  did not land, so the GPU spins at full power.
- **sRGB double-encode is guarded but untested on hardware that triggers it.**
  This GPU does not grant an sRGB default framebuffer, so the branch that scopes
  `GL_FRAMEBUFFER_SRGB` to the world passes is dormant here. It is correct by
  inspection but has not been seen running.
- **Textures are generated procedurally at runtime**, not authored. Original and
  coherent, but pixel art by algorithm; `src/render/TextureGen.cpp` is structured
  so PNGs can replace it through stb_image.

## Build

- **21 `D9025` warnings** (`overriding '/W3' with '/W0'`) come from the vendored
  GLFW and ImGui targets, where warnings are deliberately suppressed because the
  code is not ours. No engine translation unit emits a warning at `/W4`.
- **ASan disables container annotations.** `VOXL_ENABLE_ASAN` defines
  `_DISABLE_STRING_ANNOTATION` / `_DISABLE_VECTOR_ANNOTATION`, because only
  first-party targets link `voxl::compile_options`, and mixing annotated and
  unannotated translation units is a hard `LNK2038` failure that stopped
  `voxl_tests` linking at all. The cost is container-overflow detection inside
  `std::vector` / `std::string`; use-after-free detection, which is what this
  codebase's threading rules are actually about, is unaffected.

## Testing

- Tests are headless by contract — nothing in `tests/` may create a GL context —
  so shader correctness and anything GPU-resident is verified by inspection and
  by screenshot review (`docs/VISUAL_REVIEW.md`), not by the suite.
- There is no automated screenshot comparison. Capture scenes are reproducible
  via `--scene` / `--carve`, but a human still has to look at the images.
