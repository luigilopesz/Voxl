# Voxl - Roadmap

Milestones are gated: a milestone is not done until **every** acceptance
criterion below it is checkable by someone else on a clean clone. "It runs on my
machine" is not a criterion; each one names the command or the observable.

Performance numbers assume the reference machine: a desktop with 8+ hardware
threads and a GPU supporting OpenGL 4.5 core.

---

## M0 - Build foundation - **DONE**

Repository, toolchain, dependency cache, logging, window and GL context.

- [x] `tools/build.ps1 -Config RelWithDebInfo` configures and builds from a clean
      clone with no manual steps.
- [x] Dependencies resolve into `.deps/` (GLFW 3.4, GLM 1.0.3, ImGui
      1.92.9-docking, stb, FastNoiseLite 1.1.1, miniaudio 0.11.25, Catch2 3.11)
      and no new dependency is ever added.
- [x] `tools/syntax_check.ps1 <file.cpp>` compiles a single TU with `/W4` and no
      link step.
- [x] MSVC `/W4` clean across the tree.
- [x] `src/platform/Window.hpp` owns GLFW and the GL 4.5 core context; it is the
      only file including `<GLFW/glfw3.h>`.
- [x] `VOXL_LOG_*`, `VOXL_CHECK`, `VOXL_ASSERT` available and writing to console
      and file.
- [x] Frozen contracts published: `world/VoxelTypes.hpp`, `world/Block.hpp`,
      `core/Log.hpp`, `platform/Window.hpp`, `core/JobSystem.hpp`,
      `core/Time.hpp`, `world/ChunkStorage.hpp`, `world/Chunk.hpp`,
      `world/BlockAccess.hpp`, `mesh/MeshData.hpp`, `render/Camera.hpp`.

---

## M1 - A chunk on screen

One hand-built chunk, one shader, one camera. No streaming, no generation.

- [ ] `ChunkStorage` unit tests pass: uniform round trip; palette growth through
      every width `1 -> 2 -> 4 -> 8 -> 16`; read-back of all 32768 voxels after
      each growth; `optimise()` collapses two ids to 1 bit and one id to uniform;
      light nibbles independent of the palette.
- [ ] `Chunk` transition tests pass: `isLegalChunkTransition` matches the table in
      `docs/TECHNICAL_DESIGN.md` for all 49 pairs; two concurrent
      `tryTransition(Ready, Meshing)` calls from different threads yield exactly
      one success.
- [ ] Vertex pack/unpack round trip is lossless for every field at its minimum and
      maximum (`x = 32`, `textureLayer = 4095`, `width = height = 32`,
      `sunlight = blockLight = 15`, `ao = 3`, all six directions).
- [ ] A texture array of the 21 documented layers loads from
      `assets/textures/blocks/` and the layer order matches `enum TextureLayer`.
- [ ] A naive per-face mesher turns a hand-filled 32³ chunk into a
      `ChunkMeshData` and the renderer draws it with correct per-face textures.
- [ ] WASD + mouse-look fly camera; `Camera::frustum()` planes verified against a
      known view-projection in a unit test.
- [ ] `tools/build.ps1 -RunTests` is green.

**Observable:** launch the game, see a single textured 32³ chunk, fly around it,
faces on all six sides look right and grass has a distinct top/side/bottom.

---

## M2 - Generated, streamed world

Deterministic terrain, background generation and meshing, chunk streaming.

- [ ] Terrain generation is a pure function of `(seed, ChunkPos)`: generating the
      same chunk twice, in any thread order, produces byte-identical storage.
      Regression test hashes 16 chunks and compares against a stored digest.
- [ ] Generation and meshing run on `JobSystem` workers; a test asserts no GL
      entry point is reachable from a worker (link-time or a debug guard that
      trips on `gl*` off the main thread).
- [ ] Mesh uploads go exclusively through `JobSystem::mainThreadQueue()` with a
      per-frame budget; raising view distance from 4 to 16 chunks never produces a
      frame longer than 33 ms on the reference machine.
- [ ] Chunks stream in and out around the player. Resident chunk count is stable
      when standing still and returns to the same count after a round trip out and
      back.
- [ ] Greedy meshing: a solid 32³ stone chunk with air above emits at most a few
      hundred triangles, not 12 × 32² per face.
- [ ] `ChunkNeighbourhood::complete()` gates meshing; no chunk is ever meshed with
      a missing resident neighbour, verified by a counter that must stay at 0.
- [ ] Shutdown with 200+ jobs in flight joins cleanly with no crash, no leak
      (`_CrtDumpMemoryLeaks` or equivalent) and no hang.
- [ ] Debug overlay shows FPS, job counts, chunk-state histogram, voxel memory and
      GPU mesh memory.

**Observable:** fly in a straight line for two minutes; terrain appears ahead
without hitching, memory plateaus, the overlay's outstanding-job count returns to
0 whenever you stop.

---

## M3 - Lighting and player physics

Sunlight and block light, ambient occlusion, a player you can walk around as.

- [ ] Sunlight floods from `y = kWorldMaxY` down; an unroofed surface voxel reads
      15 and a sealed cave reads 0.
- [ ] Block light propagates from emitters: a glowstone in a dark cave produces a
      radius-15 gradient, symmetric in all six directions.
- [ ] Water attenuates 2 per block and leaves 1, so a lake bottom and a forest
      floor are visibly darker than open ground.
- [ ] Light updates are incremental: breaking one block relights and remeshes only
      the affected chunks, and the count of remeshed chunks is asserted `<= 27`.
- [ ] Smooth ambient occlusion in the corners; the AO diagonal-flip in
      `MeshLayerData::addQuad` verified by a unit test on corner AO values.
- [ ] Player capsule collides with `CollisionShape::Cube` blocks - no falling
      through the world, no sticking on flat floors at any frame rate from 30 to
      300 fps (fixed timestep test).
- [ ] Swimming/buoyancy in `CollisionShape::Fluid`; the raycast skips liquids and
      air so a block placed while standing in water goes where the crosshair is.
- [ ] Break and place blocks; the edit bumps `contentVersion()` and a mesh job
      that started before the edit is discarded rather than uploaded (regression
      test for the stale-mesh bug).

**Observable:** walk, jump, swim, dig a cave, place a glowstone and watch the
light propagate; no visible seams at chunk boundaries.

---

## M4 - World feel

Biomes, caves, trees, water, sky, sound.

- [ ] At least four biomes with distinct surface blocks and height profiles,
      selected deterministically from the seed.
- [ ] Cave systems that connect and do not open into the void below `y = 1`;
      bedrock layer is unbroken.
- [ ] Trees, ore-like clusters and surface decoration, all deterministic.
- [ ] Water bodies at `kSeaLevel` render translucent, back-to-front, with no
      internal faces between adjacent water blocks.
- [ ] Cutout pass renders leaves with alpha test and no face culling against
      neighbours.
- [ ] Day/night cycle drives sunlight intensity and sky colour.
- [ ] Positional block-break/place/footstep audio via miniaudio, driven by
      `BlockType::soundGroup`.
- [ ] Save and load: quitting and reloading restores the edited world exactly,
      verified by hashing the resident chunks before and after.

**Observable:** spawn into a world you would want to explore; edits survive a
restart.

---

## M5 - Ship quality

Performance, robustness, polish.

- [ ] 60+ fps at 16-chunk view distance on the reference machine, measured over a
      60-second flight path, with the 99th-percentile frame under 20 ms.
- [ ] Frustum culling plus distance sorting; the overlay shows drawn vs resident
      chunk counts and drawn is a small fraction while looking at a wall.
- [ ] Total resident memory bounded and reported; no unbounded growth over a
      30-minute session (measured start vs end).
- [ ] Zero leaks and zero data races under a 10-minute stress run (edit + stream
      + save concurrently).
- [ ] Settings menu: view distance, FOV, vsync, mouse sensitivity, volume -
      persisted across runs.
- [ ] Crash-free startup on a machine with no GL 4.5 support produces a clear
      error dialog rather than a hard fault.
- [ ] `tools/build.ps1 -Config Release -RunTests` green, `/W4` clean, and the
      shipped build launches with `/SUBSYSTEM:WINDOWS` and no console.

**Observable:** hand the build to someone with no context; they can play it.
