#pragma once

// =============================================================================================
//  THE FAR FIELD -- L1
// =============================================================================================
//
// One extra nested volume, coarser than the near field and covering sixteen times its area.
// PERFORMANCE_PLAN.md sec 5.3 proposes four of these; sec 6.3 says build exactly one first,
// because one level converts the whole sec 5.5 cost budget from arithmetic into a measurement.
// This is that one level. The result is written up in docs/FAR_FIELD.md.
//
//   level       voxel     chunk    CPA   world edge   view radius   table
//   L0 near     6.25 cm    4 m      16      64 m          32 m      33.7 MB   (unchanged)
//   L1 far      25 cm     16 m      16     256 m         128 m      33.7 MB   (this file)
//
// The ONLY thing that differs between the levels is the voxel size. Same VoxelLeafChunk, same
// palette compression, same uniformity pyramid, same DDA, same generation shaders -- the six
// generation passes are the *same* voxel_world.comp.glsl recompiled with VOXEL_LEVEL=1 and
// bound to a second chunk table. The two levels also share ONE voxel heap, so there is one
// allocator and one VRAM cap to reason about rather than two.
//
// WHAT THE TRACE CANNOT DO WITH #defines, AND WHY THIS FILE EXISTS.
// A generation shader compiles for one level, so it can take CHUNKS_PER_AXIS and
// LOG2_VOXEL_SIZE from voxel_malloc.inl's VOXEL_LEVEL switch. The TRACE cannot: a single ray
// crosses both levels inside one shader invocation, so it needs both sets of constants live at
// once. Everything outside the generation passes -- the whole renderer, all eight
// voxel_trace() call sites, and all of C++ -- therefore compiles at level 0 and reaches L1
// through the FF_* names below.

#include <voxels/impl/voxel_malloc.inl>

// ---- the master switches --------------------------------------------------------------------
//
// FF_ENABLE 0 removes the far field completely: no second table, no second generation chain,
// no second march segment, and voxel_trace() is textually the code that was there before. That
// is the control run for every measurement in docs/FAR_FIELD.md, and it is a compile-time
// switch rather than a setting because a runtime branch in voxel_trace() would itself cost
// something and would muddy the number this whole exercise exists to produce.
#define FF_ENABLE 1

// WHICH RAYS SEE THE FAR FIELD. voxel_trace() is shared by eight call sites (WORLD_SCALE.md
// sec 7) and this is the knob that decides how many of them pay for 128 m of extra volume.
//
//   0  none. The level is still built, generated and resident; no ray marches it. This is the
//      control that ISOLATES THE MARCH from everything else the level costs, and unlike
//      FF_ENABLE 0 it needs no C++ rebuild -- trace.glsl is compiled at runtime, so flipping
//      this and relaunching is the whole experiment.
//   1  primary visibility only -- the far field is seen, and nothing else changes.
//   2  every ray: sun shadows, ReSTIR diffuse, ReSTIR reflections, the irradiance cache.
//
// This is a knob and not a constant because WORLD_SCALE.md sec 3.2 is a measured warning about
// exactly this: applying a distance change to every caller, including the ambient and shadow
// rays, is how the naive cone-LOD experiment turned a 16% saving into a 20% regression and a
// black sky. The difference between 1 and 2 is measured in docs/FAR_FIELD.md rather than
// assumed, because 2 is the physically correct answer (a mountain should occlude and should
// bounce light) and 1 is the cheap one.
#define FF_RAYS 2

// HOW FAR A NON-PRIMARY RAY MAY GO INTO THE FAR FIELD, IN WHOLE METRES. 0 = no limit.
//
// AN INTEGER ON PURPOSE, and the reason is written at the head of voxels/impl/trace.glsl: the
// GLSL preprocessor's `#if` is integer-only, a float literal there is a compile error, and daxa
// runs with `register_null_pipelines_when_first_compile_fails`, so the mistake presents as
// silently missing passes rather than as a diagnostic. This constant was written `0.0` first and
// deleted every pass in the engine; the runs looked plausible until the log was read.
//
// This exists because of what FF_RAYS 2 measured, and docs/FAR_FIELD.md sec 5 is the number:
// giving the far field to all eight call sites costs 24x what giving it to primary visibility
// alone costs, and almost none of that is the sky-to-surface conversion the budget worried
// about. It is GRAZING RAYS. A near-horizontal GI or shadow ray over gently rising distant
// ground does not miss and does not hit; it skims the surface for hundreds of metres at a
// fraction of a chunk per step. The near field never showed this because its box is 64 m wide
// and a grazing ray leaves it almost immediately. At 256 m it has four times as far to skim.
//
// Clamping the SECONDARY rays keeps what the far field is for -- a mountain you can see, that
// occludes and bounces light for the first few tens of metres past the near box -- and drops
// the part nobody can perceive: whether a diffuse ray that has already travelled 100 m
// eventually lands on rock or on sky.
#define FF_SECONDARY_MAX_DIST_M 48

// 48 IS THE SHIPPED DEFAULT AND IT IS A MEASURED CHOICE, not a placeholder. At the ridge pose,
// Balanced tier: 0 (unclamped) costs +16.6 ms over a far field no ray marches; 48 costs +1.69 ms;
// and FF03-ridge-secondary-capped-48m.png against FF02-vista-normals-fixed.png shows the two are
// the same picture, because primary visibility is exempt and still marches the whole 128 m. What
// 48 gives up is distant surfaces occluding each other's sky beyond 48 m, which reads as a slight
// lift in ambient brightness on the far slopes and nothing else.

// The far field's own step budget, counted separately from MAX_STEPS so that a near-field ray
// which crawled through grass and exhausted its 512 steps does not silently delete the
// mountain behind it. 256 steps of L1 is 64 m at the 25 cm minimum and 4096 m at the 16 m
// chunk step, against a 256 m box diagonal of 443 m -- so empty space is crossed in at most
// CHUNKS_PER_AXIS = 16 chunk-sized steps and this budget only binds inside coarse geometry.
#define FF_MAX_STEPS 256

// ---- the level's geometry --------------------------------------------------------------------
// Mirrors the derivation in voxel_malloc.inl for LOG2_VOXEL_SIZE, at the far level's value.
#if FF_LOG2_VOXEL_SIZE <= 0
#define FF_VOXEL_SCL (1 << (-FF_LOG2_VOXEL_SIZE))
#define FF_VOXEL_SIZE (1.0 / FF_VOXEL_SCL)
#else
#define FF_VOXEL_SIZE (1 << FF_LOG2_VOXEL_SIZE)
#define FF_VOXEL_SCL (1.0 / FF_VOXEL_SIZE)
#endif
#define FF_CHUNK_WORLDSPACE_SIZE (float(CHUNK_SIZE) * float(FF_VOXEL_SIZE))
#define FF_WORLD_EDGE (float(FF_CHUNKS_PER_AXIS) * FF_CHUNK_WORLDSPACE_SIZE)
#define FF_VIEW_RADIUS (FF_WORLD_EDGE * 0.5)

// The chunk-index shift, spelled out once. calc_chunk_index() writes it as
// `>> (6 + LOG2_VOXEL_SIZE)` because 6 = log2(CHUNK_SIZE); the same identity at the far level.
#define FF_LOG2_CHUNK_WORLDSPACE_SIZE (6 + FF_LOG2_VOXEL_SIZE)

// ---- HOLLOWNESS ------------------------------------------------------------------------------
//
// WORLD_SCALE.md sec 7.3 lists "a far field that is not hollow where L0 covers it" as FATAL:
// the ray hits the coarse version of terrain it should be seeing in detail. The march makes
// that structurally impossible -- L1 is entered only at the point where the ray LEAVES the L0
// box, so nothing inside that box is ever sampled at L1 (see far_field.glsl) -- but there is a
// second-order version of the same problem at the boundary itself. A 25 cm voxel can bulge up
// to 25 cm past the 6.25 cm surface it stands for, so a grazing ray that L0 correctly reported
// as a miss can hit that bulge one voxel after the hand-off and paint a wall at exactly 32 m.
//
// The fix here is world-space rather than dynamic: THE FAR TERRAIN SIMPLY DOES NOT EXIST
// within FF_HOLLOW_R of the island centre, so the L0 box can never contain any of it. The L0
// box reaches at most 35 m from the player and the player stays within ~21 m of the island
// centre, so 72 m leaves 16 m of margin. It costs nothing at runtime and needs no
// regeneration, which a sphere carved around a moving player would.
//
// THE LIMIT OF THAT, STATED PLAINLY: it works because the near content is an island and the
// far content is a separate ring across water. A far field that has to CONTINUE the ground
// under the player needs the dynamic version -- L1 chunks within the L0 box generated empty,
// and re-generated when the player moves. That is a real piece of work and it is not here.
#define FF_HOLLOW_R 72.0

// ---- C++ ---------------------------------------------------------------------------------
#if defined(__cplusplus)

struct GpuContext;
struct VoxelWorldBuffers;
struct VoxelParticles;

// Creates the L1 chunk table and globals and clears them. Called from VoxelWorld::record_startup.
void far_field_record_startup(GpuContext &gpu_context, VoxelWorldBuffers &buffers);
// Registers the L1 perframe + the six generation passes. Called from VoxelWorld::record_frame,
// AFTER the near chain and with the near chain's own temp_voxel_chunks view, so the two share
// that 134 MB transient rather than each paying for one.
void far_field_record_frame(GpuContext &gpu_context, VoxelWorldBuffers &buffers,
                            daxa::TaskBufferView task_gvox_model_buffer,
                            daxa::TaskBufferView task_temp_voxel_chunks_buffer,
                            VoxelParticles &particles);

#endif
