# Handoff — state as of 2026-08-01

Written at a session boundary. **Tree:** `C:\voxl2`, HEAD `dbbd54e`, build green.
Working tree is clean except for four untracked documents in `docs/design/` (see §0).

---

## 0. Read this first — the three things that will otherwise cost you an hour

**1. `git push` will not do what you want, and one wrong form is destructive.**

```
local branch:  main                      -> dbbd54e   (no upstream configured)
origin/voxl2-engine                      -> dbbd54e   THIS IS THE VOXL2 BRANCH
origin/main                              -> 5d10169   THIS IS THE OLD OPENGL ENGINE
```

`github.com/luigilopesz/Voxl` hosts **both** projects. `git push origin main` would push voxl2's
history over the old engine's `main`. **Always `git push origin main:voxl2-engine`**, or set the
upstream once with `git push -u origin main:voxl2-engine`. (The previous handoff said "this
repository has no remote". That is stale — the remote exists and HEAD is pushed.)

**2. Four documents in `docs/design/` are untracked and unpushed.** They are the entire output of
the cache-research phase and nothing else references them from a committed file:

```
docs/design/GIGAVOXELS_NOTES.md      the paper, the 2011 thesis and the GigaSpace source, read
docs/design/CACHE_CURRENT_STATE.md   voxl2's memory model today, measured
docs/design/CACHE_PRIOR_ART.md       the seventeen years since, ranked for this case
docs/design/VOXEL_CACHE_DESIGN.md    the decision document — start here
```

**Commit them before doing anything else.**

**3. Two engine instances cannot start on this card, and that blocks the measurement protocol.**
Since the far field landed, the allocator's doubled margin means a second `gvox_engine.exe` dies at
buffer creation with `DAXA_RESULT_ERROR_OUT_OF_DEVICE_MEMORY` before writing a CSV row. This
project's whole method is *interleaved* control-and-test runs. **Fixing it is the next step (§2)
and it is the reason it is the next step.**

---

## 1. What was decided this session

The session researched GigaVoxels-style ray-guided streaming with an LRU pool, asked whether it
should replace voxl2's dense chunk table, and produced `docs/design/VOXEL_CACHE_DESIGN.md`. It was
then reviewed adversarially and revised in place. **The revision changed the answer, not just the
prose** — read §10 of that document, it is self-contained.

**The verdict, in three lines:**

- **No LRU cache.** The pooled index is 9.1 MB on a 6144 MiB card (measured census). The pool that
  could actually fill is the `voxel_malloc` heap, and its natural eviction key here is **distance,
  not recency** — the engine already has a distance policy (chunk wrapping) and a coarse fallback
  (the far field). Recency thrashes on camera turns and its fallback is a hole.
- **No directory rewrite next either.** It is a good design and it is now six defects lighter, but
  it is a rewrite of the engine's central data structure with **no intermediate state in which the
  engine runs**, aimed at a 6× near-detail gain — while far-field L2 and L3 give **16× visible
  extent for about thirty lines per level**, with a shipped precedent.
- **Build the far field instead, and fix the allocator first**, which `FAR_FIELD.md` §7.1 item 3
  already said and the cache design's first draft had misattributed.

**Four corrections a fresh session should not have to rediscover:**

1. **`CHUNKS_PER_AXIS` is view radius, not world size.** `ENABLE_CHUNK_WRAPPING` is on; the player
   already walks forever. The axis is `CPA × 2` metres of 6.25 cm detail.
2. **voxl2 already destroys edits, today, with no cache involved.** Walk 32 m, the chunk leaves the
   wrapped volume, `PerChunkCompute` clears `ACCEL_GENERATED` and re-elects it, and `ChunkEdit`
   regenerates it from the terrain generator. This is an authored-delta-store problem, not a cache
   problem, and no plan in this project currently contains the delta store.
3. **The brief's summary of the GigaVoxels paper is wrong in four of five claims** — it is not
   move-to-front, not GPU-side, not 1 MB, and it does not cluster co-accessed data. The table in
   `VOXEL_CACHE_DESIGN.md` §4 has the verbatim quotes. Do not re-derive this.
4. **Every measurement in this project so far is on one scene** — an 80 m island in an empty box,
   whose radius stops growing at CPA 32 by construction. That is why §3 below is the one measurement
   that matters.

---

## 2. The next concrete step

### Stage 1: bound-check the GPU allocator and give coarse levels their own election budget

Fully specified at `docs/design/VOXEL_CACHE_DESIGN.md` §10.1. Summary:

**Why this first:** it is the largest single VRAM line in the engine (1660.9 MB of capacity holding
52.0 MB); it is already blocking measurement (§0 item 3); `FAR_FIELD.md` §7.1 item 3 names it a
prerequisite for L2 and L3; and it is a prerequisite for any future pool's failure mode to be
diagnosable rather than corrupt. It is on the critical path of every route.

**Two files, plus one constant:**

- `src/utilities/allocator.inl` — add `daxa_i32 element_capacity` to the shared allocator struct
  (`:13-20`), written by the CPU in `create()` (`:314-322`) and on every successful growth in
  `check_for_realloc()` (`:476`).
- `src/utilities/allocator.glsl` — in `FUNC_NAME(malloc)`, replace the compile-time
  `UserMaxElementCount` clamp at `:37-40` with a runtime test against `element_capacity`, and on
  failure **return a sentinel** instead of clamping. The comment at `:30-36` already says why
  clamping is wrong and what is needed. Give `VoxelMalloc_malloc`'s fallback loop
  (`voxel_malloc.glsl:150-189`, which currently spins until it succeeds) that path.
- `src/voxels/impl/voxel_malloc.inl:132` — replace the `× 2` on
  `VOXEL_MALLOC_MAX_PAGE_ALLOCATIONS_PER_FRAME` with a runtime per-level election budget in
  `try_elect` (L0 keeps 128, L1 gets 32), which takes the margin from ×2 to about ×1.25.

**Acceptance, all four:**

1. Byte-identical images at vista, sky, cave and both ends of a `--patrol` capture (`fc /b`). A
   bound-check that changes a pixel means something was depending on the overflow.
2. Heap capacity at the vista settles **under 300 MB** against today's 1660.9 MB.
3. Zero growth refusals and zero malloc refusals in a 60 s `--patrol`.
4. **Two `gvox_engine.exe` instances start simultaneously and both render.** They cannot today.
   This is the criterion that matters, because it restores the measurement protocol.

No pose more than 1 % slower at p50. It is a memory change; it should be invisible in time.

### Then, in order

1. **Per-class ray reach** — `FAR_FIELD.md` §7.1 item 2. One mask. The crude single-clamp version is
   already measured at a 10× recovery (+16.6 ms → +1.69 ms from one integer).
2. **Far-field L2 and L3** — `FAR_FIELD.md` §0 finding 4 measured L1 at *12 lines of constants, 2 of
   buffer plumbing, one march function and no new generation code*. Takes the visible radius from
   **128 m to 2048 m** for +2–3 ms and +67 MB of table.
3. **`update_index` out of the dense chunk table** — `VOXEL_CACHE_DESIGN.md` §7 Stage 2a. ~10 lines,
   cannot change a pixel, and it deletes `CPA³ × 64 B` of dirty L2 lines per frame. Also a
   prerequisite for any future directory.

**Do not start the directory + body pool** (`VOXEL_CACHE_DESIGN.md` §3) without running §3 below
first. It has no intermediate running state and this project has been cut off mid-implementation
four sessions running.

---

## 3. The one measurement that would most change the answer

**Run `log_table_census()` and read the `voxel_malloc` heap-in-use figure on a reference-density
scene at CPA 16 and CPA 32 — not on the island.**

```
VOXL_VEG_FOREST=1 VOXL_VEG_TREE_SPACING=3.0      (the R1 config, DENSITY_LIMITS.md sec 4)
CHUNKS_PER_AXIS 16 and 32, vista pose, --bench-csv, fresh VOXL_DATA_DIR, uncontended card
```

No new code — the instrument is already in the tree at `src/voxels/impl/voxel_world.cpp:147-191`
and prints everything needed. Report the census line (`uniform` / `header_only` / `paletted`) and
heap **in use** at t = 30 s.

**Why it decides things.** Every "nothing to evict" number in this project comes from the island,
where the heap holds 13–52 MB. But `WORLD_SCALE.md` §5.3 measured a *second* scene — the demo world,
terrain across the whole box — at **1262.8 MB of heap in use at CPA 32**, and
`PERFORMANCE_PLAN.md:372` concluded "1263 MB of heap for a 128 m world is the practical wall, and it
arrives before the table does." Nobody has reconciled the two.

| if the census says | then |
|---|---|
| heap in use stays under ~300 MB at CPA 32 | the island generalises, the **table** is the binding pool, and the directory is worth its risk after the far field |
| heap in use approaches 1262.8 MB | the **payload** binds long before the index, the directory buys nothing, the work is permanently LOD, and eviction gets its first real case — with a distance key, which means it is the far field |
| `header_only` is non-zero | `VOXEL_CACHE_DESIGN.md` §3.4's fifth residency state is load-bearing rather than theoretical |

Two cheaper secondary measurements, listed so they are not lost:

- `VOXL_GPU_PROFILE` on `PerChunkCompute` **alone** at CPA 16/32/48/64, standing still, world fully
  generated. It is `O(CPA³)` every frame forever, not just at startup, and the ×64 extrapolation to
  ≈3.4 ms of a 7.1 ms frame at CPA 64 is unmeasured. `VOXEL_CACHE_DESIGN.md` §2.4.
- The ~40-line touch probe specified exactly at `CACHE_CURRENT_STATE.md` §6. It replaces this
  project's only empty-box estimate with a measurement, and it needs a copied tree.

---

## 4. Where the project stands

`C:\voxl2` is a fork of GabeRundlett/gvox_engine — a Vulkan/Daxa path-traced voxel renderer at
16 voxels per metre. Upstream is dormant (last commit 2024-11), so this codebase is ours.
Build: `powershell -File C:/voxl2/tools/build.ps1`.

**Landed and measured:**

- The engine renders the 37 m island at **p50 10.36 ms** at 1280×720 vista with GI
  (`CACHE_CURRENT_STATE.md` §7), and **7.855 ms** at the ridge with the far field and the 48 m
  secondary clamp (`FAR_FIELD.md` §5.4).
- **Path-traced GI is real** and the proof is a control: `docs/comparisons/01-gi-cave-lit-vs-dark.png`.
- A GPU profiler (`VOXL_GPU_PROFILE=out.csv`) times 94 of 96 passes.
- **The first far-field level** — 25 cm voxels, 256 m box, 128 m visible radius, a real ridgeline
  for +0.70 ms of primary cost. `docs/FAR_FIELD.md`.
- **The scale sweep** — works at CPA 64, crawls at 80, dies at 128, and the wall is the dense chunk
  table at 8216 B/chunk resident whether the chunk holds rock or air. `docs/SCALE_LIMITS.md`.
- **The density sweep** — memory is not the vegetation wall; the rasterised particle system is, and
  it has two silent failure modes. `docs/DENSITY_LIMITS.md`.
- Editing tools (brush id, radius, tool HUD), CLI quality knobs, and Quality/Balanced/Performance
  presets with Balanced shipped.

**The performance answer, unchanged:** `frame_ms = 3.030 + 7.520 × internal_megapixels`, with a
fixed 3.030 ms floor that caps the engine near 330 fps regardless of configuration. Pixels are the
lever, not features — a half-resolution render with full GI and a full-resolution render with no GI
both cost 5.264 ms, from two independent runs. Recommended target is Balanced at ~120 fps, not 240.

---

## 5. The measurement traps — each of these has already corrupted a result here

1. **Shared settings file.** All quality settings lived in one file under `%APPDATA%`, so parallel
   instances silently overwrote each other. This *reversed the sign* of one agent's headline result.
   `VOXL_DATA_DIR` fixes it — use a fresh one per run.
2. **A second engine instance** roughly doubles frame time; two at 1080p thrash 6 GB of VRAM into
   500 ms frames. Verify the card is uncontended **by process count** — `nvidia-smi
   --query-compute-apps` returns `[N/A]` on this driver and will tell you it is idle when it is not.
3. **A failed shader compile silently deletes passes** rather than failing loudly. A frame that got
   faster may simply have stopped drawing something. **Always open the image.**
4. **Working directory.** `main.cpp` walks up looking for `.out` or `assets`. The old engine at
   `C:\Users\luigi\projects\Voxl` has an `assets/` directory, so launching the voxl2 binary from
   anywhere near it silently rebinds to the wrong tree and Daxa aborts with `0x80000003` and no
   message. Always pass an explicit working directory of `C:\voxl2`.
5. **Poses are absolute, the docs are scene-local.** `--pos` takes absolute metres; every pose in
   `SCENE.md` is scene-local, `local = absolute + (183, 110, 52.5)`. Passing the local triple puts
   the player 210 m out in open water — **and the run still exits 0 with a plausible-looking
   screenshot.**

**The reference poses:** vista `-182.990,-109.980,-46.970` rot `0.785,1.096`; sky
`-183.0,-110.0,-27.5` rot `0.785,1.62`; cave `-170.0,-97.0,-49.3` rot `0.785,1.45`; plus `--patrol`.

**The pinned settings line:** `--render-scale 1.0 --gi true --reflections false --shadows true
--no-overlay --set UI/show_tool_hud=false`.

**Control and test runs interleaved, never batched.**

---

## 6. Known defects and open questions

- **Black wedges on near and far terrain.** `FAR_FIELD.md` §8.1 found and fixed the derived-normal
  cause; **some survive** in `FF02`, on both the far terrain and the near hill. `SCENE.md` defect #1
  is the near-field record of the same class. Anyone reopening it should start at `FAR_FIELD.md`
  §8.1 rather than from scratch.
- **`FAR_FIELD.md`'s cave and patrol controls are rejected** (§9.2): two `FF_RAYS 0` control rows
  read ~14.1 ms against a ~6–7 ms treatment. A control slower than its own treatment is an intruder,
  not a measurement. **Re-shoot both on a quiet machine.**
- **The allocator's unchecked `atomicAdd`** — §2 Stage 1. `SCALE_LIMITS.md` §6 provoked it and got
  4.6 MB of out-of-bounds GPU writes, grey slabs through the hillside, a written screenshot and
  **exit code 0**.
- **No persistence.** No audio, no player physics beyond naive point-sampling collision. The old
  engine has better versions of all three; none is ported.
- **`brush_add_ball` writes plain grey** — correct, but crude as level design.
- **The bench-CSV settings header** is designed but not landed; it must move out of the constructor,
  which runs before CLI overrides are applied and would record pre-override settings.
- **The reference engine's vegetation is voxelised polygon models** (PlantFactory, Quixel), not
  procedural. That is very likely the largest single reason its scenes look authored and ours look
  procedural, and it needs no renderer change. Never researched; brief at `scratchpad/bmf2.js`.

**The highest-value unread source:** GigaVoxels DP, HPG 2024, DOI 10.1145/3675389. Unread by all
three research agents — ACM returned 403 and HAL is behind a bot check. By title it addresses
starvation, i.e. the request-latency problem any future request-driven election would inherit.

---

## 7. File ownership

Other agents have worked in this tree in parallel and the convention has held. If you spawn
parallel work, keep it: one owner per file, and every cross-file edit listed in the owning
document's integration-notes section (`FAR_FIELD.md` §8 is the model).

**Never modify `C:\Users\luigi\projects\Voxl`** — that is the old OpenGL engine, kept as reference.
