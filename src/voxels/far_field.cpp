#include <voxels/far_field_task.inl>
#include <utilities/gpu_context.hpp>
#include <fmt/format.h>

// =============================================================================================
//  THE FAR FIELD -- C++ side
// =============================================================================================
//
// Two buffers and seven passes. There is no new generation code anywhere in this file: six of
// the seven passes are the SAME voxel_world.comp.glsl the near field runs, recompiled with
// VOXEL_LEVEL=1 and pointed at the far chunk table. gpu_context keys its pipeline cache on the
// task-head name plus the concatenated defines (gpu_context.hpp:133-137), so adding the define
// produces a second pipeline from one source file, exactly the way ChunkOptCompute already
// produces two from CHUNK_OPT_STAGE.
//
// That is the whole reason PERFORMANCE_PLAN.md sec 5.7 could call items 1-4 "mechanical": the
// levels differ only in voxel size, and voxel size is a #define.

#if FF_ENABLE

void far_field_record_startup(GpuContext &gpu_context, VoxelWorldBuffers &buffers) {
    buffers.ff_voxel_globals = gpu_context.find_or_add_temporal_buffer({
        .size = sizeof(VoxelWorldGlobals),
        .name = "ff_voxel_globals",
    });

    // 8216 B * 16^3 = 33.7 MB, resident whether the far world holds rock or air. This is the
    // entire fixed cost of the level, and WORLD_SCALE.md sec 5.2 is why it does not grow with
    // the level's reach: the table is 8216 * CHUNKS_PER_AXIS^3 and does not depend on voxel
    // size at all, so the same 33.7 MB that buys L0 a 32 m radius buys L1 a 128 m one.
    auto chunk_n = static_cast<size_t>(FF_CHUNKS_PER_AXIS);
    chunk_n = chunk_n * chunk_n * chunk_n;
    buffers.ff_voxel_chunks = gpu_context.find_or_add_temporal_buffer({
        .size = sizeof(VoxelLeafChunk) * chunk_n,
        .name = "ff_voxel_chunks",
    });

    gpu_context.frame_task_graph.use_persistent_buffer(buffers.ff_voxel_globals.task_resource);
    gpu_context.frame_task_graph.use_persistent_buffer(buffers.ff_voxel_chunks.task_resource);
    gpu_context.startup_task_graph.use_persistent_buffer(buffers.ff_voxel_globals.task_resource);
    gpu_context.startup_task_graph.use_persistent_buffer(buffers.ff_voxel_chunks.task_resource);

    gpu_context.startup_task_graph.add_task({
        .attachments = {
            daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_WRITE, buffers.ff_voxel_globals.task_resource),
            daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_WRITE, buffers.ff_voxel_chunks.task_resource),
        },
        .task = [&buffers, chunk_n](daxa::TaskInterface const &ti) {
            ti.recorder.clear_buffer({
                .buffer = buffers.ff_voxel_globals.task_resource.get_state().buffers[0],
                .offset = 0,
                .size = sizeof(VoxelWorldGlobals),
                .clear_value = 0,
            });
            // Zeroing the table is what makes an un-generated far world SAFE rather than
            // undefined: flags == 0 means CHUNK_FLAGS_ACCEL_GENERATED is clear, and sample_lod
            // returns 7 for such a chunk (voxels.glsl:204) -- "whole chunk uniform, take the
            // longest step". So before the generator has run, the far field is empty air and
            // the frame looks exactly like the control. It fills in over the first ~32 frames.
            ti.recorder.clear_buffer({
                .buffer = buffers.ff_voxel_chunks.task_resource.get_state().buffers[0],
                .offset = 0,
                .size = sizeof(VoxelLeafChunk) * chunk_n,
                .clear_value = 0,
            });
        },
        .name = "clear far field",
    });
}

void far_field_record_frame(GpuContext &gpu_context, VoxelWorldBuffers &buffers,
                            daxa::TaskBufferView task_gvox_model_buffer,
                            daxa::TaskBufferView task_temp_voxel_chunks_buffer,
                            VoxelParticles &particles) {
    // Every far pass carries this. It is what selects FF_CHUNKS_PER_AXIS and
    // FF_LOG2_VOXEL_SIZE inside voxel_malloc.inl, sends brushgen_world() to
    // brushgen_far_field(), and compiles out the CPU-mirror writes and the edit election.
    auto const ff = std::vector<daxa::ShaderDefine>{{"VOXEL_LEVEL", "1"}};

    gpu_context.add(ComputeTask<FarFieldPerframeCompute::Task, FarFieldPerframeComputePush, NoTaskInfo>{
        .source = daxa::ShaderFile{"voxels/far_field_perframe.comp.glsl"},
        .views = std::array{
            daxa::TaskViewVariant{std::pair{FarFieldPerframeCompute::AT.gpu_input, gpu_context.task_input_buffer}},
            daxa::TaskViewVariant{std::pair{FarFieldPerframeCompute::AT.voxel_globals, buffers.ff_voxel_globals.task_resource}},
            daxa::TaskViewVariant{std::pair{FarFieldPerframeCompute::AT.near_voxel_globals, buffers.voxel_globals.task_resource}},
            daxa::TaskViewVariant{std::pair{FarFieldPerframeCompute::AT.ff_voxel_chunks, buffers.ff_voxel_chunks.task_resource}},
        },
        .callback_ = [](daxa::TaskInterface const &ti, daxa::ComputePipeline &pipeline, FarFieldPerframeComputePush &push, NoTaskInfo const &) {
            ti.recorder.set_pipeline(pipeline);
            set_push_constant(ti, push);
            ti.recorder.dispatch({1, 1, 1});
        },
    });

    // Election. 16^3 threads deciding which far chunks need generating -- the same shader the
    // near field runs, so the trailing-face invalidation on player movement comes for free. It
    // fires 4x less often per metre travelled than the near one, because a far chunk is 16 m
    // rather than 4 m (WORLD_SCALE.md sec 6.4).
    gpu_context.add(ComputeTask<PerChunkCompute::Task, PerChunkComputePush, NoTaskInfo>{
        .source = daxa::ShaderFile{"voxels/impl/voxel_world.comp.glsl"},
        .extra_defines = ff,
        .views = std::array{
            daxa::TaskViewVariant{std::pair{PerChunkCompute::AT.gpu_input, gpu_context.task_input_buffer}},
            daxa::TaskViewVariant{std::pair{PerChunkCompute::AT.gvox_model, task_gvox_model_buffer}},
            daxa::TaskViewVariant{std::pair{PerChunkCompute::AT.voxel_globals, buffers.ff_voxel_globals.task_resource}},
            daxa::TaskViewVariant{std::pair{PerChunkCompute::AT.voxel_chunks, buffers.ff_voxel_chunks.task_resource}},
            daxa::TaskViewVariant{std::pair{PerChunkCompute::AT.value_noise_texture, gpu_context.task_value_noise_image_view}},
        },
        .callback_ = [](daxa::TaskInterface const &ti, daxa::ComputePipeline &pipeline, PerChunkComputePush &push, NoTaskInfo const &) {
            ti.recorder.set_pipeline(pipeline);
            set_push_constant(ti, push);
            auto const dispatch_size = FF_CHUNKS_PER_AXIS / 8;
            ti.recorder.dispatch({dispatch_size, dispatch_size, dispatch_size});
        },
    });

    // THE TEMP CHUNK BUFFER IS SHARED WITH THE NEAR CHAIN, and that is worth 134 MB. It is
    // sizeof(TempVoxelChunk) * MAX_CHUNK_UPDATES_PER_FRAME, and a second one would be a second
    // 134 MB. The task graph sees that the near chain's last reader (its ChunkAlloc) and this
    // chain's first writer touch the same resource and inserts the barrier itself, so the two
    // generation chains run one after the other in the order they were recorded. Serialising
    // them costs nothing that matters: chunk generation is 0.05 ms standing still.
    gpu_context.add(ComputeTask<ChunkEditCompute::Task, ChunkEditComputePush, NoTaskInfo>{
        .source = daxa::ShaderFile{"voxels/impl/voxel_world.comp.glsl"},
        .extra_defines = ff,
        .views = std::array{
            daxa::TaskViewVariant{std::pair{ChunkEditCompute::AT.gpu_input, gpu_context.task_input_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkEditCompute::AT.gvox_model, task_gvox_model_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkEditCompute::AT.voxel_globals, buffers.ff_voxel_globals.task_resource}},
            daxa::TaskViewVariant{std::pair{ChunkEditCompute::AT.voxel_chunks, buffers.ff_voxel_chunks.task_resource}},
            daxa::TaskViewVariant{std::pair{ChunkEditCompute::AT.voxel_malloc_page_allocator, buffers.voxel_malloc.task_allocator_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkEditCompute::AT.temp_voxel_chunks, task_temp_voxel_chunks_buffer}},
            // Bound because the task head requires them; never written, because
            // brushgen_far_field() calls no spawn function. See the VOXEL_LEVEL == 1 branch in
            // brushgen_world() for why a coarse level must not touch the particle budget.
            SIMPLE_STATIC_ALLOCATOR_BUFFER_USES_ASSIGN(ChunkEditCompute, GrassStrandAllocator, particles.grass.grass_allocator),
            SIMPLE_STATIC_ALLOCATOR_BUFFER_USES_ASSIGN(ChunkEditCompute, FlowerAllocator, particles.flowers.flower_allocator),
            SIMPLE_STATIC_ALLOCATOR_BUFFER_USES_ASSIGN(ChunkEditCompute, TreeParticleAllocator, particles.tree_particles.tree_particle_allocator),
            SIMPLE_STATIC_ALLOCATOR_BUFFER_USES_ASSIGN(ChunkEditCompute, FireParticleAllocator, particles.fire_particles.fire_particle_allocator),
            daxa::TaskViewVariant{std::pair{ChunkEditCompute::AT.value_noise_texture, gpu_context.task_value_noise_image_view}},
            daxa::TaskViewVariant{std::pair{ChunkEditCompute::AT.test_texture, gpu_context.task_test_texture}},
            daxa::TaskViewVariant{std::pair{ChunkEditCompute::AT.test_texture2, gpu_context.task_test_texture2}},
        },
        .callback_ = [](daxa::TaskInterface const &ti, daxa::ComputePipeline &pipeline, ChunkEditComputePush &push, NoTaskInfo const &) {
            ti.recorder.set_pipeline(pipeline);
            set_push_constant(ti, push);
            ti.recorder.dispatch_indirect({
                .indirect_buffer = ti.get(ChunkEditCompute::AT.voxel_globals).ids[0],
                .offset = offsetof(VoxelWorldGlobals, indirect_dispatch) + offsetof(VoxelWorldGpuIndirectDispatch, chunk_edit_dispatch),
            });
        },
    });

    gpu_context.add(ComputeTask<ChunkEditPostProcessCompute::Task, ChunkEditPostProcessComputePush, NoTaskInfo>{
        .source = daxa::ShaderFile{"voxels/impl/voxel_world.comp.glsl"},
        .extra_defines = ff,
        .views = std::array{
            daxa::TaskViewVariant{std::pair{ChunkEditPostProcessCompute::AT.gpu_input, gpu_context.task_input_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkEditPostProcessCompute::AT.gvox_model, task_gvox_model_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkEditPostProcessCompute::AT.voxel_globals, buffers.ff_voxel_globals.task_resource}},
            daxa::TaskViewVariant{std::pair{ChunkEditPostProcessCompute::AT.voxel_chunks, buffers.ff_voxel_chunks.task_resource}},
            daxa::TaskViewVariant{std::pair{ChunkEditPostProcessCompute::AT.voxel_malloc_page_allocator, buffers.voxel_malloc.task_allocator_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkEditPostProcessCompute::AT.temp_voxel_chunks, task_temp_voxel_chunks_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkEditPostProcessCompute::AT.value_noise_texture, gpu_context.task_value_noise_image_view}},
        },
        .callback_ = [](daxa::TaskInterface const &ti, daxa::ComputePipeline &pipeline, ChunkEditPostProcessComputePush &push, NoTaskInfo const &) {
            ti.recorder.set_pipeline(pipeline);
            set_push_constant(ti, push);
            ti.recorder.dispatch_indirect({
                .indirect_buffer = ti.get(ChunkEditPostProcessCompute::AT.voxel_globals).ids[0],
                .offset = offsetof(VoxelWorldGlobals, indirect_dispatch) + offsetof(VoxelWorldGpuIndirectDispatch, chunk_edit_dispatch),
            });
        },
    });

    // The uniformity pyramid. Pure index arithmetic over a 64^3 chunk, so it is identical at
    // both levels -- the two ChunkOpt stages needed no thought at all, which is the clearest
    // single demonstration that "nested volumes differing only in voxel size" is the right
    // shape for this engine.
    //
    // Written out twice rather than looped, because Task::callback_ is a raw function pointer
    // (gpu_task.hpp:19) and a lambda that captured the stage would not convert to one.
    gpu_context.add(ComputeTask<ChunkOptCompute::Task, ChunkOptComputePush, NoTaskInfo>{
        .source = daxa::ShaderFile{"voxels/impl/voxel_world.comp.glsl"},
        .extra_defines = {{"VOXEL_LEVEL", "1"}, {"CHUNK_OPT_STAGE", "0"}},
        .views = std::array{
            daxa::TaskViewVariant{std::pair{ChunkOptCompute::AT.gpu_input, gpu_context.task_input_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkOptCompute::AT.voxel_globals, buffers.ff_voxel_globals.task_resource}},
            daxa::TaskViewVariant{std::pair{ChunkOptCompute::AT.temp_voxel_chunks, task_temp_voxel_chunks_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkOptCompute::AT.voxel_chunks, buffers.ff_voxel_chunks.task_resource}},
        },
        .callback_ = [](daxa::TaskInterface const &ti, daxa::ComputePipeline &pipeline, ChunkOptComputePush &push, NoTaskInfo const &) {
            ti.recorder.set_pipeline(pipeline);
            set_push_constant(ti, push);
            ti.recorder.dispatch_indirect({
                .indirect_buffer = ti.get(ChunkOptCompute::AT.voxel_globals).ids[0],
                .offset = offsetof(VoxelWorldGlobals, indirect_dispatch) + offsetof(VoxelWorldGpuIndirectDispatch, subchunk_x2x4_dispatch),
            });
        },
    });

    gpu_context.add(ComputeTask<ChunkOptCompute::Task, ChunkOptComputePush, NoTaskInfo>{
        .source = daxa::ShaderFile{"voxels/impl/voxel_world.comp.glsl"},
        .extra_defines = {{"VOXEL_LEVEL", "1"}, {"CHUNK_OPT_STAGE", "1"}},
        .views = std::array{
            daxa::TaskViewVariant{std::pair{ChunkOptCompute::AT.gpu_input, gpu_context.task_input_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkOptCompute::AT.voxel_globals, buffers.ff_voxel_globals.task_resource}},
            daxa::TaskViewVariant{std::pair{ChunkOptCompute::AT.temp_voxel_chunks, task_temp_voxel_chunks_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkOptCompute::AT.voxel_chunks, buffers.ff_voxel_chunks.task_resource}},
        },
        .callback_ = [](daxa::TaskInterface const &ti, daxa::ComputePipeline &pipeline, ChunkOptComputePush &push, NoTaskInfo const &) {
            ti.recorder.set_pipeline(pipeline);
            set_push_constant(ti, push);
            ti.recorder.dispatch_indirect({
                .indirect_buffer = ti.get(ChunkOptCompute::AT.voxel_globals).ids[0],
                .offset = offsetof(VoxelWorldGlobals, indirect_dispatch) + offsetof(VoxelWorldGpuIndirectDispatch, subchunk_x8up_dispatch),
            });
        },
    });

    // Palette compression and heap allocation, against THE SAME HEAP as the near field. One
    // allocator, one VRAM budget, one cap to reason about -- and the far field's pages are
    // therefore competing with the near field's for the same ceiling, which is the interaction
    // docs/FAR_FIELD.md measures rather than assumes.
    //
    // chunk_updates and chunk_update_heap are bound to the near field's buffers and never
    // written: the writes are inside `#if VOXEL_LEVEL == 0` in voxel_world.comp.glsl.
    gpu_context.add(ComputeTask<ChunkAllocCompute::Task, ChunkAllocComputePush, NoTaskInfo>{
        .source = daxa::ShaderFile{"voxels/impl/voxel_world.comp.glsl"},
        .extra_defines = ff,
        .views = std::array{
            daxa::TaskViewVariant{std::pair{ChunkAllocCompute::AT.gpu_input, gpu_context.task_input_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkAllocCompute::AT.voxel_globals, buffers.ff_voxel_globals.task_resource}},
            daxa::TaskViewVariant{std::pair{ChunkAllocCompute::AT.temp_voxel_chunks, task_temp_voxel_chunks_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkAllocCompute::AT.voxel_chunks, buffers.ff_voxel_chunks.task_resource}},
            daxa::TaskViewVariant{std::pair{ChunkAllocCompute::AT.voxel_malloc_page_allocator, buffers.voxel_malloc.task_allocator_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkAllocCompute::AT.chunk_updates, buffers.chunk_updates.task_resource}},
            daxa::TaskViewVariant{std::pair{ChunkAllocCompute::AT.chunk_update_heap, buffers.chunk_update_heap.task_resource}},
        },
        .callback_ = [](daxa::TaskInterface const &ti, daxa::ComputePipeline &pipeline, ChunkAllocComputePush &push, NoTaskInfo const &) {
            ti.recorder.set_pipeline(pipeline);
            set_push_constant(ti, push);
            ti.recorder.dispatch_indirect({
                .indirect_buffer = ti.get(ChunkAllocCompute::AT.voxel_globals).ids[0],
                .offset = offsetof(VoxelWorldGlobals, indirect_dispatch) + offsetof(VoxelWorldGpuIndirectDispatch, chunk_edit_dispatch),
            });
        },
    });
}

#else

void far_field_record_startup(GpuContext &, VoxelWorldBuffers &) {}
void far_field_record_frame(GpuContext &, VoxelWorldBuffers &, daxa::TaskBufferView,
                            daxa::TaskBufferView, VoxelParticles &) {}

#endif
