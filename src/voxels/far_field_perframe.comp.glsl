#include <voxels/far_field_task.inl>

DAXA_DECL_PUSH_CONSTANT(FarFieldPerframeComputePush, push)
daxa_BufferPtr(GpuInput) gpu_input = push.uses.gpu_input;
daxa_RWBufferPtr(VoxelWorldGlobals) voxel_globals = push.uses.voxel_globals;
daxa_RWBufferPtr(VoxelWorldGlobals) near_voxel_globals = push.uses.near_voxel_globals;
daxa_BufferPtr(VoxelLeafChunk) ff_voxel_chunks = push.uses.ff_voxel_chunks;

#include <voxels/impl/voxels.glsl>

// NOTE ON THE CONSTANTS THIS SHADER USES. It compiles at VOXEL_LEVEL 0, so CHUNK_SIZE and
// MAX_CHUNK_UPDATES_PER_FRAME are the shared values -- which is correct, because both are the
// same at both levels: a chunk is 64^3 voxels whatever a voxel is, and the two levels share the
// update budget and the temp-chunk buffer. The only per-level quantity here is `offset`, and it
// is stored in metres and read with a different shift by each level's calc_chunk_index, so it
// is written identically for both.
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
void main() {
    // Publish the far table's address where the eight voxel_trace() call sites can reach it.
    // Rewritten every frame rather than once: the buffer is a TemporalBuffer and a resolution
    // change or any other event that recreates it would leave a stale address behind, and a
    // stale device address is a device loss rather than a wrong pixel.
    deref(near_voxel_globals).ff_voxel_chunks_addr = as_address(ff_voxel_chunks);
    deref(voxel_globals).ff_voxel_chunks_addr = as_address(ff_voxel_chunks);

    for (uint i = 0; i < MAX_CHUNK_UPDATES_PER_FRAME; ++i) {
        deref(voxel_globals).chunk_update_infos[i].brush_flags = 0;
        deref(voxel_globals).chunk_update_infos[i].i = INVALID_CHUNK_I;
    }

    deref(voxel_globals).chunk_update_n = 0;
    deref(voxel_globals).chunk_update_heap_alloc_n = 0;

    deref(voxel_globals).prev_offset = deref(voxel_globals).offset;
    deref(voxel_globals).offset = deref(gpu_input).player.player_unit_offset;

    deref(voxel_globals).indirect_dispatch.chunk_edit_dispatch = uvec3(CHUNK_SIZE / 8, CHUNK_SIZE / 8, 0);
    deref(voxel_globals).indirect_dispatch.subchunk_x2x4_dispatch = uvec3(1, 64, 0);
    deref(voxel_globals).indirect_dispatch.subchunk_x8up_dispatch = uvec3(1, 1, 0);
}
