#pragma once

#include <voxels/impl/voxel_world.inl>

// The far level's per-frame reset. A cut-down VoxelWorldPerframeCompute: it advances
// offset/prev_offset, clears the election list and re-seeds the indirect dispatches for the far
// globals, and does nothing else.
//
// It is a separate shader rather than a second instance of the near one because of what the
// near one ALSO does, all of which would be wrong here:
//   * VoxelMallocPageAllocator_perframe() -- the two levels share one heap, and running its
//     per-frame bookkeeping twice in a frame would corrupt the free-page stack;
//   * the gpu_output write of the heap's consumed page count -- one writer, or the readback
//     races itself;
//   * clearing chunk_updates -- the far level never writes the CPU mirror (see the
//     VOXEL_LEVEL == 0 guards in voxel_world.comp.glsl), so clearing it here would fight the
//     near level for the same slots;
//   * the whole brush pick and tool-state block -- the far field takes no edits.
//
// It also PUBLISHES THE FAR CHUNK TABLE'S DEVICE ADDRESS into the near globals. This pass is the
// only one in the engine that holds both a pointer to the far table and a writable pointer to
// the near globals, which makes it the natural and only place to do it. See the comment on
// VoxelWorldGlobals::ff_voxel_chunks_addr for why the address travels that way instead of in a
// push constant: there were eight spare bytes and it needed sixteen.
DAXA_DECL_TASK_HEAD_BEGIN(FarFieldPerframeCompute)
DAXA_TH_BUFFER_PTR(COMPUTE_SHADER_READ, daxa_BufferPtr(GpuInput), gpu_input)
DAXA_TH_BUFFER_PTR(COMPUTE_SHADER_READ_WRITE, daxa_RWBufferPtr(VoxelWorldGlobals), voxel_globals)
DAXA_TH_BUFFER_PTR(COMPUTE_SHADER_READ_WRITE, daxa_RWBufferPtr(VoxelWorldGlobals), near_voxel_globals)
DAXA_TH_BUFFER_PTR(COMPUTE_SHADER_READ, daxa_BufferPtr(VoxelLeafChunk), ff_voxel_chunks)
DAXA_DECL_TASK_HEAD_END
struct FarFieldPerframeComputePush {
    DAXA_TH_BLOB(FarFieldPerframeCompute, uses)
};
