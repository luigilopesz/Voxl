#pragma once

#include <voxels/impl/voxel_malloc.inl>
#include <voxels/gvox_model.inl>
#include <voxels/brushes.inl>
#include <voxels/far_field.inl>

#define VOXELS_ORIGINAL_IMPL

// 1364 daxa_u32's
// 10.65625 bytes per 8x8x8
struct TempVoxelChunkUniformity {
    daxa_u32 lod_x2[1024];
    daxa_u32 lod_x4[256];
    daxa_u32 lod_x8[64];
    daxa_u32 lod_x16[16];
    daxa_u32 lod_x32[4];
};

// 8 bytes per 8x8x8
struct PaletteHeader {
    daxa_u32 variant_n;
    VoxelMalloc_Pointer blob_ptr;
};

struct VoxelParentChunk {
    daxa_u32 is_uniform;
    daxa_u32 children[512];
    daxa_u32 is_ptr[16];
};
DAXA_DECL_BUFFER_PTR(VoxelParentChunk)

struct VoxelLeafChunk {
    daxa_u32 flags;
    daxa_u32 update_index;
    daxa_u32 uniformity_bits[3];
    // 8 bytes per 8x8x8
    VoxelMalloc_ChunkLocalPageSubAllocatorState sub_allocator_state;
    // 8 bytes per 8x8x8
    PaletteHeader palette_headers[PALETTES_PER_CHUNK];
};
DAXA_DECL_BUFFER_PTR(VoxelLeafChunk)

// DECL_SIMPLE_ALLOCATOR(VoxelLeafChunkAllocator, VoxelLeafChunk, 1, daxa_u32, (MAX_CHUNK_WORK_ITEMS_L2))
// DECL_SIMPLE_ALLOCATOR(VoxelParentChunkAllocator, VoxelParentChunk, 1, daxa_u32, (MAX_CHUNK_WORK_ITEMS_L0 + MAX_CHUNK_WORK_ITEMS_L1))

struct TempVoxelChunk {
    PackedVoxel voxels[CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE];
    TempVoxelChunkUniformity uniformity;
};
DAXA_DECL_BUFFER_PTR(TempVoxelChunk)

struct VoxelChunkUpdateInfo {
    daxa_i32vec3 i;
    daxa_i32vec3 chunk_offset;
    daxa_u32 brush_flags;
    BrushInput brush_input;
};

struct VoxelWorldGpuIndirectDispatch {
    daxa_u32vec3 chunk_edit_dispatch;
    daxa_u32vec3 subchunk_x2x4_dispatch;
    daxa_u32vec3 subchunk_x8up_dispatch;
};

struct BrushState {
    daxa_u32 initial_frame;
    daxa_f32vec3 initial_ray;
    daxa_u32 is_editing;
};

struct VoxelWorldGlobals {
    // THE FAR CHUNK TABLE'S DEVICE ADDRESS, AND WHY IT TRAVELS HERE RATHER THAN IN THE PUSH
    // CONSTANT. Adding the far table as a second DAXA_TH_BUFFER_PTR to VOXELS_USE_BUFFERS is the
    // obvious way to reach it from all eight voxel_trace() call sites, and it was the first
    // thing tried. IT DOES NOT FIT: the blob for the widest of those task heads was already 120
    // bytes of a 128-byte Vulkan push-constant limit, and daxa answers with
    //     push constant size of 136 exceeds the maximum size of 128
    // at pipeline creation -- which, because gpu_context.cpp registers null pipelines when the
    // first compile fails, presents as a SILENTLY MISSING PASS rather than an error. That is
    // measurement trap (c) in docs/HANDOFF.md, and it is how this constraint announced itself.
    //
    // So the address rides in a buffer that every one of those shaders already has bound, at a
    // cost of eight bytes of VRAM and zero bytes of push constant. far_field_perframe.comp.glsl
    // publishes it once per frame from its own binding.
    //
    // The task-graph dependency is NOT lost by doing this. VOXELS_USE_BUFFERS still declares the
    // far table with DAXA_TH_BUFFER -- an attachment that is "NOT represented at all within the
    // shader blob" (daxa/utils/task_graph.inl:21-23) but is still seen by the dependency solver.
    // The barrier between the far generation chain writing the table and the trace reading it is
    // therefore still inserted; only the pointer's delivery route changed.
    //
    // First member on purpose: a daxa_u64 is 8-aligned in both C++ and daxa's scalar buffer
    // layout, and at offset 0 there is no padding for the two to disagree about. Anywhere else
    // in this struct it would sit after 4-byte members and the two layouts would have to be
    // trusted to insert the same pad.
    daxa_u64 ff_voxel_chunks_addr;

    BrushInput brush_input;
    BrushState brush_state;
    VoxelWorldGpuIndirectDispatch indirect_dispatch;

    VoxelChunkUpdateInfo chunk_update_infos[MAX_CHUNK_UPDATES_PER_FRAME];
    daxa_i32vec3 prev_offset;
    daxa_u32 chunk_update_n; // Number of chunks to update
    daxa_i32vec3 offset;
    daxa_u32 chunk_update_heap_alloc_n;
};
DAXA_DECL_BUFFER_PTR(VoxelWorldGlobals)

struct CpuChunkUpdateInfo {
    daxa_u32 chunk_index;
    daxa_u32 flags;
};
struct ChunkUpdate {
    CpuChunkUpdateInfo info;
    PaletteHeader palette_headers[PALETTES_PER_CHUNK];
};
DAXA_DECL_BUFFER_PTR(ChunkUpdate)

// ---- the far-field buffers -------------------------------------------------------------------
//
// THE FAR FIELD IS THREADED IN THROUGH THESE THREE MACROS AND NOWHERE ELSE, which is the single
// reason this change is not a twenty-file refactor. VOXELS_USE_BUFFERS is expanded by 20 task
// headers, so declaring the far table here reaches every one of the eight voxel_trace() call
// sites at once, and VOXELS_BUFFER_USES_ASSIGN fills it from VoxelWorldBuffers on the C++ side.
// Not one of those twenty files is edited. WORLD_SCALE.md sec 7 item 2 estimated this as the
// mechanical bulk of the work; it is two lines, because voxel_trace() was already a single
// choke point.
//
// NOTE WHAT IS AND IS NOT DECLARED. The far table is a DAXA_TH_BUFFER -- a dependency-only
// attachment, "NOT represented at all within the shader blob" (daxa/utils/task_graph.inl:22).
// It buys the barrier between the far generation chain and the trace and costs zero push-constant
// bytes, which matters because there were only eight to spare; the pointer itself arrives through
// VoxelWorldGlobals::ff_voxel_chunks_addr and the comment there is the full story. The far
// GLOBALS are not declared at all: the only field the trace wants from them is `offset`, and both
// levels are handed the identical player_unit_offset every frame -- they differ in the shift
// applied to it, not in the value -- so the near globals answer for both.
//
// Under FF_ENABLE 0 the macros expand to exactly what they were, so the control build is
// textually the old engine rather than the new one with a flag off.
#if FF_ENABLE

#define VOXEL_BUFFER_USE_N 4

#define VOXELS_USE_BUFFERS(ptr_type, mode)                               \
    DAXA_TH_BUFFER_PTR(mode, ptr_type(VoxelWorldGlobals), voxel_globals) \
    DAXA_TH_BUFFER_PTR(mode, ptr_type(VoxelLeafChunk), voxel_chunks)     \
    DAXA_TH_BUFFER_PTR(mode, ptr_type(VoxelMallocPageAllocator), voxel_malloc_page_allocator) \
    DAXA_TH_BUFFER(mode, ff_voxel_chunks)

#define VOXELS_USE_BUFFERS_PUSH_USES(ptr_type)                           \
    ptr_type(VoxelWorldGlobals) voxel_globals = push.uses.voxel_globals; \
    ptr_type(VoxelLeafChunk) voxel_chunks = push.uses.voxel_chunks;      \
    ptr_type(VoxelMallocPageAllocator) voxel_malloc_page_allocator = push.uses.voxel_malloc_page_allocator;

#define VOXELS_BUFFER_USES_ASSIGN(TaskHeadName, voxel_buffers)                                                          \
    daxa::TaskViewVariant{std::pair{TaskHeadName::AT.voxel_globals, voxel_buffers.voxel_globals.task_resource}},        \
        daxa::TaskViewVariant{std::pair{TaskHeadName::AT.voxel_chunks, voxel_buffers.voxel_chunks.task_resource}},      \
        daxa::TaskViewVariant{std::pair{TaskHeadName::AT.voxel_malloc_page_allocator, voxel_buffers.voxel_malloc.task_allocator_buffer}}, \
        daxa::TaskViewVariant {                                                                                         \
        std::pair { TaskHeadName::AT.ff_voxel_chunks, voxel_buffers.ff_voxel_chunks.task_resource }                     \
    }

#else

#define VOXEL_BUFFER_USE_N 3

#define VOXELS_USE_BUFFERS(ptr_type, mode)                               \
    DAXA_TH_BUFFER_PTR(mode, ptr_type(VoxelWorldGlobals), voxel_globals) \
    DAXA_TH_BUFFER_PTR(mode, ptr_type(VoxelLeafChunk), voxel_chunks)     \
    DAXA_TH_BUFFER_PTR(mode, ptr_type(VoxelMallocPageAllocator), voxel_malloc_page_allocator)

#define VOXELS_USE_BUFFERS_PUSH_USES(ptr_type)                           \
    ptr_type(VoxelWorldGlobals) voxel_globals = push.uses.voxel_globals; \
    ptr_type(VoxelLeafChunk) voxel_chunks = push.uses.voxel_chunks;      \
    ptr_type(VoxelMallocPageAllocator) voxel_malloc_page_allocator = push.uses.voxel_malloc_page_allocator;

#define VOXELS_BUFFER_USES_ASSIGN(TaskHeadName, voxel_buffers)                                                       \
    daxa::TaskViewVariant{std::pair{TaskHeadName::AT.voxel_globals, voxel_buffers.voxel_globals.task_resource}},     \
        daxa::TaskViewVariant{std::pair{TaskHeadName::AT.voxel_chunks, voxel_buffers.voxel_chunks.task_resource}},   \
        daxa::TaskViewVariant {                                                                                      \
        std::pair { TaskHeadName::AT.voxel_malloc_page_allocator, voxel_buffers.voxel_malloc.task_allocator_buffer } \
    }

#endif

struct VoxelWorldOutput {
    VoxelMallocPageAllocatorGpuOutput voxel_malloc_output;
    // VoxelLeafChunkAllocatorGpuOutput voxel_leaf_chunk_output;
    // VoxelParentChunkAllocatorGpuOutput voxel_parent_chunk_output;
};

// VoxelBufferPtrs is UNCHANGED by the far field, and that is load-bearing rather than tidy.
// VOXELS_BUFFER_PTRS is expanded inside brushes.glsl (brush_flowers, and the near generator's
// neighbour sample), and brushes.glsl is compiled into ChunkEditCompute -- a shader that
// declares its buffers by hand rather than through VOXELS_USE_BUFFERS_PUSH_USES. Adding a field
// here therefore breaks a file the far field otherwise never touches, with
//     'ff_voxel_chunks' : undeclared identifier
// from inside a brush. The far table's address comes off VoxelWorldGlobals instead.
struct VoxelBufferPtrs {
    daxa_BufferPtr(VoxelMallocPageAllocator) allocator;
    daxa_BufferPtr(VoxelLeafChunk) voxel_chunks_ptr;
    daxa_BufferPtr(VoxelWorldGlobals) globals;
};
struct VoxelRWBufferPtrs {
    daxa_RWBufferPtr(VoxelMallocPageAllocator) allocator;
    daxa_RWBufferPtr(VoxelLeafChunk) voxel_chunks_ptr;
    daxa_RWBufferPtr(VoxelWorldGlobals) globals;
};

#define VOXELS_BUFFER_PTRS VoxelBufferPtrs(daxa_BufferPtr(VoxelMallocPageAllocator)(as_address(voxel_malloc_page_allocator)), daxa_BufferPtr(VoxelLeafChunk)(as_address(voxel_chunks)), daxa_BufferPtr(VoxelWorldGlobals)(as_address(voxel_globals)))
#define VOXELS_RW_BUFFER_PTRS VoxelRWBufferPtrs(daxa_RWBufferPtr(VoxelMallocPageAllocator)(as_address(voxel_malloc_page_allocator)), daxa_RWBufferPtr(VoxelLeafChunk)(as_address(voxel_chunks)), daxa_RWBufferPtr(VoxelWorldGlobals)(as_address(voxel_globals)))

#define MAX_CHUNK_UPDATES_PER_FRAME_VOXEL_COUNT (CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE * MAX_CHUNK_UPDATES_PER_FRAME)

#if defined(__cplusplus)

#include <utilities/allocator.inl>

struct VoxelWorldBuffers {
    TemporalBuffer voxel_globals;
    TemporalBuffer voxel_chunks;
    TemporalBuffer chunk_updates;
    TemporalBuffer chunk_update_heap;
    AllocatorBufferState<VoxelMallocPageAllocator> voxel_malloc;
#if FF_ENABLE
    // The far field: a second chunk table and a second globals, and NOTHING ELSE. It shares
    // voxel_malloc (one heap, one VRAM cap), it shares the transient temp_voxel_chunks (the
    // task graph serialises the two generation chains on it, so the 134 MB transient is paid
    // once), and it shares chunk_updates / chunk_update_heap by never writing them -- the L1
    // ChunkAlloc's CPU-mirror writes are compiled out, because the CPU mirror exists for
    // player collision and the player never stands on the far field.
    //
    // So the whole resident cost of the level is the table: 8216 B * 16^3 = 33.7 MB.
    TemporalBuffer ff_voxel_globals;
    TemporalBuffer ff_voxel_chunks;
#endif
    // AllocatorBufferState<VoxelLeafChunkAllocator> voxel_leaf_chunk_malloc;
    // AllocatorBufferState<VoxelParentChunkAllocator> voxel_parent_chunk_malloc;
};

#endif
