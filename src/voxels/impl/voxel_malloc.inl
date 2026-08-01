#pragma once

#include <core.inl>
#include <utilities/allocator.inl>

#define CHUNK_SIZE 64 // A chunk = 64^3 voxels

// The world is a fixed cube of CHUNKS_PER_AXIS^3 chunks that wraps around the player; there is
// no streaming and nothing outside it exists. At LOG2_VOXEL_SIZE -4 a chunk is 4 m, so this is
// directly the world's edge length in chunks.
//
// 32 (a 128 m cube) is upstream's value and it does not fit this project's brief or its card:
//   - the voxel_chunks table is sizeof(VoxelLeafChunk) * CHUNKS_PER_AXIS^3 and is resident even
//     for an empty world -- 8216 B * 32768 = 269.2 MB, versus 33.7 MB at 16.
//   - the demo world at 32 settled at a 2906 MB voxel heap on a 6 GB card, one growth step from
//     a device-lost (docs/BASELINE.md sec 4).
// 16 gives a 64 m cube: still 1024^3 voxel slots, still 16 voxels/metre, and comfortably large
// enough for the tree/cave/lighting test scene this iteration is for.
//
// ---- LEGAL VALUES ARE 8, 16, 32, 64. NOT "any multiple of 8". ------------------------------
//
// This comment used to say "must stay a multiple of 8 because CHUNKS_DISPATCH_SIZE below divides
// by 8". That is necessary and NOT sufficient, and the difference is not subtle:
//
//   src/voxels/impl/voxel_world.comp.glsl:44
//       terrain_work_item.i = ivec3(gl_GlobalInvocationID.xyz) & (chunk_n - 1);
//
// `x & (n-1)` is `x % n` only when n is a POWER OF TWO. At CHUNKS_PER_AXIS 24, chunk_n - 1 is 23
// = 0b10111, so slices 8..15 of every axis are never elected and never generated, and slices
// 0..7 are elected twice. MEASURED (docs/SCALE_LIMITS.md sec 2): generation plateaus at
// 4096/13824 chunks = 29.6% and never completes; at 48 it plateaus at 32768/110592, again 29.6%.
// The player stands at the centre of the volume, which is inside the dead band, so EVERY capture
// at 24 renders sky and sea and nothing else. The engine exits 0 and logs nothing.
//
// The #error below turns that into a compile failure. Delete it only by also fixing line 44.
//
// ---- AND THE CEILING, MEASURED ON THE 6 GB CARD --------------------------------------------
// Process VRAM = 1785 MiB + 8216 B * CHUNKS_PER_AXIS^3, within 1.3% at 16/32/48/64.
//   32 -> 269 MB table, 2027 MiB total.  64 -> 2154 MB table, 3790 MiB total: works.
//   80 -> 4207 MB table, ~5796 MiB: starts, but ~110 ms/frame -- the driver is paging.
//   96 -> 7269 MB table, larger than the card: starts only when nothing else is on the GPU.
//  128 -> 17230 MB table: dies at 0x80000003, empty stderr, no exception to catch.
// The host-side CpuVoxelChunk mirror costs another 8192 B/chunk and scales identically.
// ---- the far-field level (L1) ------------------------------------------------------------
//
// PERFORMANCE_PLAN.md sec 5.3 proposes four nested volumes that differ ONLY in voxel size.
// This is the second of them, and the only one built: 25 cm voxels, a 16 m chunk, a 256 m
// cube, a 128 m view radius. See docs/FAR_FIELD.md.
//
// The level a shader compiles for is chosen by VOXEL_LEVEL, which far_field.cpp passes as an
// extra_define on the six generation passes. Everything else -- the whole renderer, all eight
// voxel_trace() call sites, and all of C++ -- compiles at level 0 and reaches the far field
// through the FF_* constants in voxels/far_field.inl instead. That is deliberate: the TRACE
// needs both levels live in one shader, so it cannot use these #defines for L1.
#define FF_CHUNKS_PER_AXIS 16
#define FF_LOG2_VOXEL_SIZE (-2)

#ifndef VOXEL_LEVEL
#define VOXEL_LEVEL 0
#endif

#if VOXEL_LEVEL == 1
#define CHUNKS_PER_AXIS FF_CHUNKS_PER_AXIS
#define LOG2_VOXEL_SIZE FF_LOG2_VOXEL_SIZE
#else
#define CHUNKS_PER_AXIS 16
#define LOG2_VOXEL_SIZE (-4)
#endif
#define CHUNKS_DISPATCH_SIZE (CHUNKS_PER_AXIS / 8)
// Both constraints, stated where they bite. The power-of-two one comes from the `&` in chunk
// election; the multiple-of-8 one from CHUNKS_DISPATCH_SIZE dividing by 8. Written as one
// preprocessor test so it works identically in C++ and in GLSL.
#if (CHUNKS_PER_AXIS & (CHUNKS_PER_AXIS - 1)) != 0 || (CHUNKS_PER_AXIS % 8) != 0
#error "CHUNKS_PER_AXIS must be a power of two AND a multiple of 8 (8, 16, 32, 64, ...). See the note above: chunk election in voxel_world.comp.glsl masks with (chunk_n - 1), so a non-power-of-two silently leaves most of the world ungenerated."
#endif
#if LOG2_VOXEL_SIZE <= 0
#define VOXEL_SCL (1 << (-LOG2_VOXEL_SIZE))
#define VOXEL_SIZE (1.0 / VOXEL_SCL)
#else
#define VOXEL_SIZE (1 << LOG2_VOXEL_SIZE)
#define VOXEL_SCL (1.0 / VOXEL_SIZE)
#endif
#define CHUNK_WORLDSPACE_SIZE (float(CHUNK_SIZE) * float(VOXEL_SIZE))
#if LOG2_VOXEL_SIZE < -6
#error "this is not currently supported"
#endif

#define PALETTE_REGION_SIZE 8
#define PALETTE_REGION_TOTAL_SIZE (PALETTE_REGION_SIZE * PALETTE_REGION_SIZE * PALETTE_REGION_SIZE)
#define PALETTE_MAX_COMPRESSED_VARIANT_N 367

#if PALETTE_REGION_SIZE != 8
#error Unsupported Palette Region Size
#endif

#define PALETTES_PER_CHUNK_AXIS (CHUNK_SIZE / PALETTE_REGION_SIZE)
#define PALETTES_PER_CHUNK (PALETTES_PER_CHUNK_AXIS * PALETTES_PER_CHUNK_AXIS * PALETTES_PER_CHUNK_AXIS)

#define MAX_CHUNK_UPDATES_PER_FRAME 128

#define PALETTE_ACCELERATION_STRUCTURE_SIZE_U32S 3
// Minimum size allocation is 76 bytes, aka 19 daxa_u32s
// This is because a palette of size 2 has 1 bit per
// voxel, and 2 daxa_u32s. This counts to 512 bits, plus
// 2 * 32 bits for the 2 palette entries, plus 32 bits
// for the allocation meta-data.
#define VOXEL_MALLOC_U32S_PER_PAGE_BITFIELD_BIT (512 / 32 + 2 + 1 + PALETTE_ACCELERATION_STRUCTURE_SIZE_U32S)
#define VOXEL_MALLOC_BYTES_PER_PAGE_BITFIELD_BIT (VOXEL_MALLOC_U32S_PER_PAGE_BITFIELD_BIT * 4)
#define VOXEL_MALLOC_BITS_PER_PAGE_BITFIELD_BIT (VOXEL_MALLOC_BYTES_PER_PAGE_BITFIELD_BIT * 8)
// Because of this, the max number of allocations that
// will need to be tracked by the per-page bitfield needs
// to be at least 27. Below, we assert that this is
// large enough to hold the maximum allocation size of
// 2052 bytes (513 uints)
#define VOXEL_MALLOC_MAX_ALLOCATIONS_IN_PAGE_BITFIELD 24
// Useful variable for knowing how many bits are necessarily
// reserved for a page-local index.
#define VOXEL_MALLOC_CEIL_LOG2_MAX_ALLOCATIONS_IN_PAGE_BITFIELD 5

#define VOXEL_MALLOC_PAGE_SIZE_U32S (VOXEL_MALLOC_U32S_PER_PAGE_BITFIELD_BIT * VOXEL_MALLOC_MAX_ALLOCATIONS_IN_PAGE_BITFIELD)
#define VOXEL_MALLOC_PAGE_SIZE_BYTES (VOXEL_MALLOC_PAGE_SIZE_U32S * 4)
#define VOXEL_MALLOC_PAGE_SIZE_BITS (VOXEL_MALLOC_PAGE_SIZE_BYTES * 8)

#define VOXEL_MALLOC_MAX_ALLOCATIONS_PER_CHUNK PALETTES_PER_CHUNK

// x2 FOR THE SECOND LEVEL. This is not a buffer size, it is the safety margin the growable
// heap keeps ahead of what the GPU can consume in the frames the CPU has not seen yet
// (allocator.inl:445-451) -- the only thing keeping VoxelMalloc's unchecked atomicAdd in
// bounds. L0 and L1 both run a full ChunkAlloc in the same frame, so both can allocate
// MAX_CHUNK_UPDATES_PER_FRAME * PALETTES_PER_CHUNK pages before the CPU sees either. The
// margin costs (FIF+1) * this * 4 bytes = 1 MB at these numbers.
#define VOXEL_MALLOC_MAX_PAGE_ALLOCATIONS_PER_FRAME (VOXEL_MALLOC_MAX_ALLOCATIONS_PER_CHUNK * MAX_CHUNK_UPDATES_PER_FRAME * 2)

// A provable upper bound on how many pages the world can ever consume, used by the allocator as
// one half of its cap (the other half is the runtime VRAM budget; it takes the smaller).
//
// The bound: every palette region allocates at most one page's worth. A region is 8^3 voxels, so
// its blob is at most 512 u32s of voxel data plus 512 palette entries -- but above
// PALETTE_MAX_COMPRESSED_VARIANT_N (367) the region is stored raw at 512 u32s = 2048 B, and the
// largest compressed form is 367 + ceil(9*512/32) = 511 u32s = 2044 B. A page holds
// VOXEL_MALLOC_MAX_ALLOCATIONS_IN_PAGE_BITFIELD * VOXEL_MALLOC_BYTES_PER_PAGE_BITFIELD_BIT
// = 24 * 88 = 2112 B, so one page always suffices for one region and pages are shared between
// regions when their blobs are smaller. Hence: chunks * regions-per-chunk pages, worst case.
//
// At CHUNKS_PER_AXIS 16 that is 4096 * 512 = 2 097 152 pages (4446 MB) -- larger than the VRAM
// budget, so VRAM is what actually binds here. It binds instead at CHUNKS_PER_AXIS 8 or smaller,
// which is the point: growing the heap past what the world can address is pure waste.
#define VOXEL_MALLOC_MAX_PAGE_COUNT (daxa_u64(CHUNKS_PER_AXIS) * CHUNKS_PER_AXIS * CHUNKS_PER_AXIS * PALETTES_PER_CHUNK)

#define VOXEL_MALLOC_LOG2_MAX_GLOBAL_PAGE_COUNT (32 - VOXEL_MALLOC_CEIL_LOG2_MAX_ALLOCATIONS_IN_PAGE_BITFIELD)

// bits
//  [ 0-8 ]: chunk_local_allocator_page_index
//  [ 9-13]: page_bits_consumed
//  [14-31]: unused
#define VoxelMalloc_AllocationMetadata daxa_u32

// bits
// N = VOXEL_MALLOC_CEIL_LOG2_MAX_ALLOCATIONS_IN_PAGE_BITFIELD
//  [0 - N-1] local_page_alloc_offset
//  [N -  31] global_page_index
#define VoxelMalloc_Pointer daxa_u32

#define VoxelMalloc_PageIndex daxa_u32

// bits (needs to be packed as we use atomics to sync access)
// N = VOXEL_MALLOC_MAX_ALLOCATIONS_IN_PAGE_BITFIELD
// M = VOXEL_MALLOC_LOG2_MAX_GLOBAL_PAGE_COUNT
//  [0 - N-1]: local_consumption_bitmask
//  [N - N+M]: global_page_index
//  [ rest? ]: unused
#define VoxelMalloc_PageInfo daxa_u64

#if (VOXEL_MALLOC_MAX_ALLOCATIONS_IN_PAGE_BITFIELD + VOXEL_MALLOC_LOG2_MAX_GLOBAL_PAGE_COUNT) > 64
#error There are not enough bits in a daxa_u64 to represent the desired allocation amount, as well as the global page index.
#endif

struct VoxelMalloc_ChunkLocalPageSubAllocatorState {
    VoxelMalloc_PageInfo page_allocation_infos[VOXEL_MALLOC_MAX_ALLOCATIONS_PER_CHUNK];
};

DECL_SIMPLE_ALLOCATOR(VoxelMallocPageAllocator, daxa_u32, VOXEL_MALLOC_PAGE_SIZE_U32S, VoxelMalloc_PageIndex, (VOXEL_MALLOC_MAX_PAGE_ALLOCATIONS_PER_FRAME), (VOXEL_MALLOC_MAX_PAGE_COUNT))
