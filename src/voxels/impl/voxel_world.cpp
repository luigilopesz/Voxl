#include "voxel_world.inl"
#include <fmt/format.h>
#include <chrono>

// ---------------------------------------------------------------------------------------------
// World-scale instrumentation.
// ---------------------------------------------------------------------------------------------
// Added for docs/SCALE_LIMITS.md. Three quantities decide how large this world can be and until
// now none of them was printed anywhere:
//
//   1. THE CHUNK TABLE, which is resident whether or not the world contains a single voxel.
//      sizeof(VoxelLeafChunk) * CHUNKS_PER_AXIS^3 in VRAM, plus sizeof(CpuVoxelChunk) *
//      CHUNKS_PER_AXIS^3 in *system* RAM -- the CPU mirror is 8192 B/chunk and nobody had
//      noticed it scales identically.
//   2. WORLD GENERATION TIME. Every chunk starts with CHUNK_FLAGS_ACCEL_GENERATED clear, so at
//      startup all CHUNKS_PER_AXIS^3 of them are elected, at most MAX_CHUNK_UPDATES_PER_FRAME
//      per frame. Generation is therefore O(CHUNKS_PER_AXIS^3 / 128) FRAMES, not seconds of
//      compute -- a hard floor that no GPU speed can move.
//   3. Whether generation ever finishes at all.
//
// Counters are function-local statics rather than members because VoxelWorld is declared in
// voxels/impl/voxel_world.inl, which this work does not own. There is exactly one VoxelWorld.
namespace {
    using scale_clock = std::chrono::steady_clock;

    struct WorldGenProgress {
        scale_clock::time_point first_frame{};
        bool started = false;
        uint64_t cumulative_updates = 0; // chunk regenerations seen, including re-visits
        uint64_t distinct_chunks = 0;    // chunks generated at least once
        uint64_t frames = 0;
        bool complete_logged = false;
        std::vector<bool> seen; // one bit per chunk in the table
    };
    WorldGenProgress g_worldgen;
} // namespace

static uint32_t calc_chunk_index(glm::uvec3 chunk_i, glm::ivec3 offset) {
    // Modulate the chunk index to be wrapped around relative to the chunk offset provided.
    auto temp_chunk_i = (glm::ivec3(chunk_i) + (offset >> glm::ivec3(6 + LOG2_VOXEL_SIZE))) % glm::ivec3(CHUNKS_PER_AXIS);
    if (temp_chunk_i.x < 0) {
        temp_chunk_i.x += CHUNKS_PER_AXIS;
    }
    if (temp_chunk_i.y < 0) {
        temp_chunk_i.y += CHUNKS_PER_AXIS;
    }
    if (temp_chunk_i.z < 0) {
        temp_chunk_i.z += CHUNKS_PER_AXIS;
    }
    chunk_i = glm::uvec3(temp_chunk_i);
    uint32_t chunk_index = chunk_i.x + chunk_i.y * CHUNKS_PER_AXIS + chunk_i.z * CHUNKS_PER_AXIS * CHUNKS_PER_AXIS;
    assert(chunk_index < 50000);
    return chunk_index;
}

static uint32_t calc_palette_region_index(glm::uvec3 inchunk_voxel_i) {
    glm::uvec3 palette_region_i = inchunk_voxel_i / uint32_t(PALETTE_REGION_SIZE);
    return palette_region_i.x + palette_region_i.y * PALETTES_PER_CHUNK_AXIS + palette_region_i.z * PALETTES_PER_CHUNK_AXIS * PALETTES_PER_CHUNK_AXIS;
}

static uint32_t calc_palette_voxel_index(glm::uvec3 inchunk_voxel_i) {
    glm::uvec3 palette_voxel_i = inchunk_voxel_i & uint32_t(PALETTE_REGION_SIZE - 1);
    return palette_voxel_i.x + palette_voxel_i.y * PALETTE_REGION_SIZE + palette_voxel_i.z * PALETTE_REGION_SIZE * PALETTE_REGION_SIZE;
}

PackedVoxel sample_palette(CpuPaletteChunk palette_header, uint32_t palette_voxel_index) {
    // daxa_RWBufferPtr(uint) blob_u32s;
    // voxel_malloc_address_to_u32_ptr(allocator, palette_header.blob_ptr, blob_u32s);
    // blob_u32s = advance(blob_u32s, PALETTE_ACCELERATION_STRUCTURE_SIZE_U32S);
    auto const *blob_u32s = palette_header.blob_ptr;
    if (palette_header.variant_n > PALETTE_MAX_COMPRESSED_VARIANT_N) {
        return PackedVoxel(blob_u32s[palette_voxel_index]);
    }
    auto bits_per_variant = ceil_log2(palette_header.variant_n);
    auto mask = (~0u) >> (32 - bits_per_variant);
    auto bit_index = palette_voxel_index * bits_per_variant;
    auto data_index = bit_index / 32;
    auto data_offset = bit_index - data_index * 32;
    auto my_palette_index = (blob_u32s[palette_header.variant_n + data_index + 0] >> data_offset) & mask;
    if (data_offset + bits_per_variant > 32) {
        auto shift = bits_per_variant - ((data_offset + bits_per_variant) & 0x1f);
        my_palette_index |= (blob_u32s[palette_header.variant_n + data_index + 1] << shift) & mask;
    }
    auto voxel_data = blob_u32s[my_palette_index];

    // debug_utils::Console::add_log(fmt::format("\nCPU {} ", bit_index));

    // debug_utils::Console::add_log("\nCPU ");
    // for (uint32_t i = 0; i < palette_header.variant_n; ++i) {
    //     debug_utils::Console::add_log(fmt::format("{} ", blob_u32s[i]));
    // }
    // debug_utils::Console::add_log("\n");

    return PackedVoxel(voxel_data);
}

PackedVoxel sample_voxel_chunk(CpuVoxelChunk const &voxel_chunk, glm::uvec3 inchunk_voxel_i) {
    auto palette_region_index = calc_palette_region_index(inchunk_voxel_i);
    auto palette_voxel_index = calc_palette_voxel_index(inchunk_voxel_i);
    CpuPaletteChunk palette_header = voxel_chunk.palette_chunks[palette_region_index];
    if (palette_header.variant_n < 2) {
        return PackedVoxel(static_cast<uint32_t>(std::bit_cast<uint64_t>(palette_header.blob_ptr)));
    }
    return sample_palette(palette_header, palette_voxel_index);
}

// PackedVoxel sample_voxel_chunk(VoxelBufferPtrs ptrs, glm::uvec3 chunk_n, glm::vec3 voxel_p, glm::vec3 bias) {
//     vec3 offset = glm::vec3((deref(ptrs.globals).offset) & ((1 << (6 + LOG2_VOXEL_SIZE)) - 1)) + glm::vec3(chunk_n) * CHUNK_WORLDSPACE_SIZE * 0.5;
//     uvec3 voxel_i = glm::uvec3(floor((voxel_p + offset) * VOXEL_SCL + bias));
//     uvec3 chunk_i = voxel_i / CHUNK_SIZE;
//     uint chunk_index = calc_chunk_index(ptrs.globals, chunk_i, chunk_n);
//     return sample_voxel_chunk(ptrs.allocator, advance(ptrs.voxel_chunks_ptr, chunk_index), voxel_i - chunk_i * CHUNK_SIZE);
// }

bool VoxelWorld::sample(daxa_f32vec3 pos, daxa_i32vec3 player_unit_offset) {
    glm::vec3 offset = glm::vec3(std::bit_cast<glm::ivec3>(player_unit_offset) & ((1 << (6 + LOG2_VOXEL_SIZE)) - 1)) + glm::vec3(CHUNKS_PER_AXIS) * CHUNK_WORLDSPACE_SIZE * 0.5f;
    glm::uvec3 voxel_i = glm::uvec3(floor((std::bit_cast<glm::vec3>(pos) + glm::vec3(offset)) * float(VOXEL_SCL)));
    glm::uvec3 chunk_i = voxel_i / uint32_t(CHUNK_SIZE);
    uint32_t chunk_index = calc_chunk_index(chunk_i, std::bit_cast<glm::ivec3>(player_unit_offset));

    auto packed_voxel = sample_voxel_chunk(voxel_chunks[chunk_index], voxel_i - chunk_i * uint32_t(CHUNK_SIZE));
    auto material_type = (packed_voxel.data >> 0) & 3;

    return material_type != 0;
}

// ---------------------------------------------------------------------------------------------
// WHAT A SPARSE CHUNK TABLE WOULD ACTUALLY COST.
// ---------------------------------------------------------------------------------------------
// The dense table charges sizeof(VoxelLeafChunk) = 8216 B for a chunk of pure air, and
// CpuVoxelChunk charges 8192 B again in host RAM. 99.76% of that is two 512-entry
// per-palette-region arrays:
//   u64           page_allocation_infos[512]  4096 B -- never touched unless a region allocates
//   PaletteHeader palette_headers[512]        4096 B -- a uniform region stores its voxel inline
// So the question is not "how big is the table" but "how many chunks need any of it". This walks
// the CPU mirror once, when generation completes, and classifies every chunk:
//   uniform      -- all 512 regions uniform AND all the same value. One word would do.
//   header-only  -- all 512 regions uniform, values differ. Needs palette_headers and never
//                   touches page_allocation_infos, so 4096 B of the 8216 is dead.
//   paletted     -- at least one region with variant_n > 1. Needs the whole record.
// "pooled" is what a three-tier layout would occupy: a dense directory of CHUNKS_PER_AXIS^3
// eight-byte entries, plus a body only for the chunks that need one.
//
// MEASURED at CHUNKS_PER_AXIS 64, whole island generated (docs/SCALE_LIMITS.md sec 3.3):
//   262144 chunks | uniform 261297 (99.7%) | paletted 847 (0.3%)
//   dense table 2153.8 MB -> pooled equivalent 9.1 MB, 237.8x smaller
static void log_table_census(std::vector<CpuVoxelChunk> const &chunks) {
    auto const n = static_cast<uint64_t>(chunks.size());
    if (n == 0) {
        return;
    }
    uint64_t uniform = 0;
    uint64_t header_only = 0;
    uint64_t paletted = 0;
    uint64_t paletted_regions = 0;
    for (auto const &c : chunks) {
        bool any_paletted = false;
        bool all_same = true;
        auto const *first = c.palette_chunks[0].blob_ptr;
        for (auto const &r : c.palette_chunks) {
            if (r.variant_n > 1) {
                ++paletted_regions;
                any_paletted = true;
            } else if (r.blob_ptr != first) {
                all_same = false;
            }
        }
        if (any_paletted) {
            ++paletted;
        } else if (all_same) {
            ++uniform;
        } else {
            ++header_only;
        }
    }
    auto const regions = n * uint64_t{PALETTES_PER_CHUNK};
    auto const dense = static_cast<uint64_t>(sizeof(VoxelLeafChunk)) * n;
    auto const pooled = 8ULL * n + static_cast<uint64_t>(sizeof(VoxelLeafChunk)) * paletted + 4096ULL * header_only;
    debug_utils::Console::add_log(fmt::format(
        "[scale] table census: {} chunks | uniform {} ({:.1f}%) | header-only {} ({:.1f}%) | "
        "paletted {} ({:.1f}%) | paletted regions {} of {} ({:.2f}%)\n",
        n, uniform, 100.0 * double(uniform) / double(n),
        header_only, 100.0 * double(header_only) / double(n),
        paletted, 100.0 * double(paletted) / double(n),
        paletted_regions, regions, 100.0 * double(paletted_regions) / double(regions)));
    debug_utils::Console::add_log(fmt::format(
        "[scale] dense table {:.1f} MB -> pooled equivalent {:.1f} MB ({:.1f}x smaller); "
        "directory alone {:.2f} MB\n",
        double(dense) / 1'000'000.0, double(pooled) / 1'000'000.0,
        double(dense) / std::max<double>(1.0, double(pooled)), double(8ULL * n) / 1'000'000.0));
}

void VoxelWorld::init_gpu_malloc(GpuContext &gpu_context) {
    if (!gpu_malloc_initialized) {
        gpu_malloc_initialized = true;
        buffers.voxel_malloc.create(gpu_context);
        // buffers.voxel_leaf_chunk_malloc.create(device);
        // buffers.voxel_parent_chunk_malloc.create(device);
    }
}

void VoxelWorld::record_startup(GpuContext &gpu_context) {
    buffers.chunk_updates = gpu_context.find_or_add_temporal_buffer({
        .size = sizeof(ChunkUpdate) * MAX_CHUNK_UPDATES_PER_FRAME * (FRAMES_IN_FLIGHT + 1),
        .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
        .name = "chunk_updates",
    });
    buffers.chunk_update_heap = gpu_context.find_or_add_temporal_buffer({
        .size = sizeof(uint32_t) * MAX_CHUNK_UPDATES_PER_FRAME_VOXEL_COUNT * (FRAMES_IN_FLIGHT + 1),
        .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
        .name = "chunk_update_heap",
    });

    buffers.voxel_globals = gpu_context.find_or_add_temporal_buffer({
        .size = sizeof(VoxelWorldGlobals),
        .name = "voxel_globals",
    });

    auto chunk_n = (CHUNKS_PER_AXIS);
    chunk_n = chunk_n * chunk_n * chunk_n;

    // THE ONE NUMBER THIS PROJECT KEPT HAVING TO DERIVE BY HAND. Printed before the allocation
    // rather than after, so that a run which dies inside create_buffer() still tells you how big
    // the buffer it was asking for was. 64-bit throughout: at CHUNKS_PER_AXIS 128 the product is
    // 17.2 GB and a 32-bit intermediate would wrap to a plausible-looking small number.
    auto const table_bytes = static_cast<uint64_t>(sizeof(VoxelLeafChunk)) * static_cast<uint64_t>(chunk_n);
    auto const cpu_mirror_bytes = static_cast<uint64_t>(sizeof(CpuVoxelChunk)) * static_cast<uint64_t>(chunk_n);
    debug_utils::Console::add_log(fmt::format(
        "[scale] CHUNKS_PER_AXIS {} -> {} chunks, world {:.0f} m cube, view radius {:.0f} m\n",
        CHUNKS_PER_AXIS, chunk_n,
        double(CHUNKS_PER_AXIS) * double(CHUNK_WORLDSPACE_SIZE),
        double(CHUNKS_PER_AXIS) * double(CHUNK_WORLDSPACE_SIZE) * 0.5));
    debug_utils::Console::add_log(fmt::format(
        "[scale] chunk table: {} B/chunk x {} = {:.1f} MB VRAM, resident when empty; "
        "CPU mirror {} B/chunk = {:.1f} MB host RAM\n",
        sizeof(VoxelLeafChunk), chunk_n, double(table_bytes) / 1'000'000.0,
        sizeof(CpuVoxelChunk), double(cpu_mirror_bytes) / 1'000'000.0));
    // Generation is rate-limited by MAX_CHUNK_UPDATES_PER_FRAME and nothing else, so the floor is
    // arithmetic and can be stated before a single frame has run.
    debug_utils::Console::add_log(fmt::format(
        "[scale] world generation floor: {} chunks / {} per frame = {} frames minimum\n",
        chunk_n, MAX_CHUNK_UPDATES_PER_FRAME,
        (chunk_n + MAX_CHUNK_UPDATES_PER_FRAME - 1) / MAX_CHUNK_UPDATES_PER_FRAME));

    // Make a failed chunk-table allocation SAY SO. Daxa turns VK_ERROR_OUT_OF_DEVICE_MEMORY into
    // an exception, nothing in this engine catches one, and the observable behaviour of asking for
    // a table larger than the card is a silent 0x80000003 with an empty stderr and an empty log.
    //
    // MEASURED, and read the caveat: at CHUNKS_PER_AXIS 128 (a 17.2 GB table) THIS DOES NOT FIRE.
    // Daxa aborts on a failed create_buffer rather than throwing, so there is nothing to catch and
    // the process dies at 0x80000003 regardless. The size line printed above it is the only
    // diagnostic there is. Kept because it costs nothing and does catch the host-side failure.
    try {
        buffers.voxel_chunks = gpu_context.find_or_add_temporal_buffer({
            .size = sizeof(VoxelLeafChunk) * chunk_n,
            .name = "voxel_chunks",
        });
    } catch (std::exception const &e) {
        debug_utils::Console::add_log(fmt::format(
            "[scale] FATAL: could not allocate the {:.1f} MB chunk table ({} chunks): {}\n",
            double(table_bytes) / 1'000'000.0, chunk_n, e.what()));
        throw;
    }
    try {
        voxel_chunks.resize(chunk_n);
    } catch (std::exception const &e) {
        debug_utils::Console::add_log(fmt::format(
            "[scale] FATAL: could not allocate the {:.1f} MB CPU mirror ({} chunks): {}\n",
            double(cpu_mirror_bytes) / 1'000'000.0, chunk_n, e.what()));
        throw;
    }
    g_worldgen = {};
    g_worldgen.seen.assign(static_cast<size_t>(chunk_n), false);

    init_gpu_malloc(gpu_context);

    gpu_context.frame_task_graph.use_persistent_buffer(buffers.chunk_updates.task_resource);
    gpu_context.frame_task_graph.use_persistent_buffer(buffers.chunk_update_heap.task_resource);
    gpu_context.frame_task_graph.use_persistent_buffer(buffers.voxel_globals.task_resource);
    gpu_context.frame_task_graph.use_persistent_buffer(buffers.voxel_chunks.task_resource);
    buffers.voxel_malloc.for_each_task_buffer([&gpu_context](auto &task_buffer) { gpu_context.frame_task_graph.use_persistent_buffer(task_buffer); });

    gpu_context.startup_task_graph.use_persistent_buffer(buffers.voxel_globals.task_resource);
    gpu_context.startup_task_graph.use_persistent_buffer(buffers.voxel_chunks.task_resource);
    buffers.voxel_malloc.for_each_task_buffer([&gpu_context](auto &task_buffer) { gpu_context.startup_task_graph.use_persistent_buffer(task_buffer); });

    // buffers.voxel_leaf_chunk_malloc.for_each_task_buffer([&gpu_context](auto &task_buffer) { gpu_context.frame_task_graph.use_persistent_buffer(task_buffer); });
    // buffers.voxel_parent_chunk_malloc.for_each_task_buffer([&gpu_context](auto &task_buffer) { gpu_context.frame_task_graph.use_persistent_buffer(task_buffer); });

    gpu_context.startup_task_graph.add_task({
        .attachments = {
            daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_WRITE, buffers.voxel_globals.task_resource),
            daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_WRITE, buffers.voxel_chunks.task_resource),
            daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_WRITE, buffers.voxel_malloc.task_element_buffer),
            // daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_WRITE, buffers.voxel_leaf_chunk_malloc.task_element_buffer),
            // daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_WRITE, buffers.voxel_parent_chunk_malloc.task_element_buffer),
        },
        .task = [this](daxa::TaskInterface const &ti) {
            ti.recorder.clear_buffer({
                .buffer = buffers.voxel_globals.task_resource.get_state().buffers[0],
                .offset = 0,
                .size = sizeof(VoxelWorldGlobals),
                .clear_value = 0,
            });

            auto chunk_n = (CHUNKS_PER_AXIS);
            chunk_n = chunk_n * chunk_n * chunk_n;
            ti.recorder.clear_buffer({
                .buffer = buffers.voxel_chunks.task_resource.get_state().buffers[0],
                .offset = 0,
                .size = sizeof(VoxelLeafChunk) * chunk_n,
                .clear_value = 0,
            });

            buffers.voxel_malloc.clear_buffers(ti.recorder);
            // buffers.voxel_leaf_chunk_malloc.clear_buffers(ti.recorder);
            // buffers.voxel_parent_chunk_malloc.clear_buffers(ti.recorder);
        },
        .name = "clear chunk editor",
    });

    gpu_context.startup_task_graph.add_task({
        .attachments = {
            daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_WRITE, buffers.voxel_malloc.task_allocator_buffer),
            // daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_WRITE, buffers.voxel_leaf_chunk_malloc.task_allocator_buffer),
            // daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_WRITE, buffers.voxel_parent_chunk_malloc.task_allocator_buffer),
        },
        .task = [this](daxa::TaskInterface const &ti) {
            buffers.voxel_malloc.init(ti.device, ti.recorder);
            // buffers.voxel_leaf_chunk_malloc.init(ti.device, ti.recorder);
            // buffers.voxel_parent_chunk_malloc.init(ti.device, ti.recorder);
        },
        .name = "Initialize",
    });

    // The far field's table and globals, and their startup clear. Must be before the startup
    // compute below, which expands VOXELS_BUFFER_USES_ASSIGN and so now binds them.
    // docs/FAR_FIELD.md.
    far_field_record_startup(gpu_context, buffers);

    gpu_context.add(ComputeTask<VoxelWorldStartupCompute::Task, VoxelWorldStartupComputePush, NoTaskInfo>{
        .source = daxa::ShaderFile{"voxels/impl/startup.comp.glsl"},
        .views = std::array{
            daxa::TaskViewVariant{std::pair{VoxelWorldStartupCompute::AT.gpu_input, gpu_context.task_input_buffer}},
            VOXELS_BUFFER_USES_ASSIGN(VoxelWorldStartupCompute, buffers),
        },
        .callback_ = [](daxa::TaskInterface const &ti, daxa::ComputePipeline &pipeline, VoxelWorldStartupComputePush &push, NoTaskInfo const &) {
            ti.recorder.set_pipeline(pipeline);
            set_push_constant(ti, push);
            ti.recorder.dispatch({1, 1, 1});
        },
        .task_graph_ptr = &gpu_context.startup_task_graph,
    });
}

void VoxelWorld::begin_frame(daxa::Device &device, GpuInput const &gpu_input, VoxelWorldOutput const &gpu_output) {
    buffers.voxel_malloc.check_for_realloc(device, gpu_output.voxel_malloc_output.current_element_count);
    // buffers.voxel_leaf_chunk_malloc.check_for_realloc(device, gpu_output.voxel_leaf_chunk_output.current_element_count);
    // buffers.voxel_parent_chunk_malloc.check_for_realloc(device, gpu_output.voxel_parent_chunk_output.current_element_count);

    bool needs_realloc = false;
    needs_realloc = needs_realloc || buffers.voxel_malloc.needs_realloc();
    // needs_realloc = needs_realloc || buffers.voxel_leaf_chunk_malloc.needs_realloc();
    // needs_realloc = needs_realloc || buffers.voxel_parent_chunk_malloc.needs_realloc();

    // --- heap readouts ---------------------------------------------------------------------
    // The MB figure used to be computed as
    //     static_cast<double>(current_element_count * VOXEL_MALLOC_PAGE_SIZE_BYTES) / 1e6
    // where both operands are 32-bit, so the multiply wrapped before the cast to double and the
    // number silently went wrong above 4294967295 / 2112 = 2 033 601 pages. The cast now happens
    // first. This matters more than a cosmetic bug: it is the number every later stage of this
    // project reads to decide whether a change made the world cheaper or more expensive, and it
    // starts lying just below the page counts a denser generator will reach.
    auto const &heap = buffers.voxel_malloc;
    auto const page_bytes = static_cast<double>(VOXEL_MALLOC_PAGE_SIZE_BYTES);
    auto const capacity_mb = static_cast<double>(heap.current_element_count) * page_bytes / 1'000'000.0;
    auto const cap_mb = static_cast<double>(heap.max_element_count) * page_bytes / 1'000'000.0;
    auto const used_mb = static_cast<double>(gpu_output.voxel_malloc_output.current_element_count) * page_bytes / 1'000'000.0;

    // Kept deliberately short. The Debug Menu auto-sizes to its widest row and is pinned to the
    // right edge, so a verbose string here silently widens the panel over a third of the screen
    // -- and moves the coordinates tools/run.ps1 clicks to open the frame-time graphs.
    debug_utils::DebugDisplay::set_debug_string(
        "GPU Heap",
        fmt::format("{} pages ({:.0f} MB)", heap.current_element_count, capacity_mb));
    debug_utils::DebugDisplay::set_debug_string(
        "GPU Heap Usage",
        fmt::format("{:.0f} MB ({:.0f}%)", used_mb,
                    heap.current_element_count == 0 ? 0.0 : 100.0 * used_mb / capacity_mb));
    // The line that would have told you the crash was coming. `cap` is what the VRAM budget
    // allows; `steps` is how many further 1.5x growths still fit under it, so 0 means the next
    // growth is already going to be trimmed or refused.
    debug_utils::DebugDisplay::set_debug_string(
        "GPU Heap Cap",
        heap.growth_refusals > 0
            ? fmt::format("{} MB, REFUSED x{}", static_cast<int>(cap_mb), heap.growth_refusals)
            : fmt::format("{} MB, {} step{}{}", static_cast<int>(cap_mb),
                          heap.growth_headroom_steps(), heap.growth_headroom_steps() == 1 ? "" : "s",
                          heap.growth_throttled ? ", trimmed" : ""));

    if (needs_realloc) {
        auto temp_task_graph = daxa::TaskGraph({
            .device = device,
            .name = "temp_task_graph",
        });

        buffers.voxel_malloc.for_each_task_buffer([&temp_task_graph](auto &task_buffer) { temp_task_graph.use_persistent_buffer(task_buffer); });
        // buffers.voxel_leaf_chunk_malloc.for_each_task_buffer([&temp_task_graph](auto &task_buffer) { temp_task_graph.use_persistent_buffer(task_buffer); });
        // buffers.voxel_parent_chunk_malloc.for_each_task_buffer([&temp_task_graph](auto &task_buffer) { temp_task_graph.use_persistent_buffer(task_buffer); });
        temp_task_graph.add_task({
            .attachments = {
                daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_READ, buffers.voxel_malloc.task_old_element_buffer),
                daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_WRITE, buffers.voxel_malloc.task_element_buffer),
                // daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_READ, buffers.voxel_leaf_chunk_malloc.task_old_element_buffer),
                // daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_WRITE, buffers.voxel_leaf_chunk_malloc.task_element_buffer),
                // daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_READ, buffers.voxel_parent_chunk_malloc.task_old_element_buffer),
                // daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_WRITE, buffers.voxel_parent_chunk_malloc.task_element_buffer),
            },
            .task = [this](daxa::TaskInterface const &ti) {
                if (buffers.voxel_malloc.needs_realloc()) {
                    buffers.voxel_malloc.realloc(ti.device, ti.recorder);
                }
                // if (buffers.voxel_leaf_chunk_malloc.needs_realloc()) {
                //     buffers.voxel_leaf_chunk_malloc.realloc(ti.device, ti.recorder);
                // }
                // if (buffers.voxel_parent_chunk_malloc.needs_realloc()) {
                //     buffers.voxel_parent_chunk_malloc.realloc(ti.device, ti.recorder);
                // }
            },
            .name = "Transfer Task",
        });

        temp_task_graph.submit({});
        temp_task_graph.complete({});
        temp_task_graph.execute({});
    }

    {
        auto const offset = (gpu_input.frame_index + 0) % (FRAMES_IN_FLIGHT + 1);
        auto const *output_heap = device.get_host_address_as<uint32_t>(buffers.chunk_update_heap.resource_id).value() + offset * MAX_CHUNK_UPDATES_PER_FRAME_VOXEL_COUNT;
        auto const *chunk_updates = device.get_host_address_as<ChunkUpdate>(buffers.chunk_updates.resource_id).value() + offset * MAX_CHUNK_UPDATES_PER_FRAME;
        auto copied_bytes = 0u;

        // --- world-generation progress ------------------------------------------------------
        // Every chunk in the table is elected exactly once at startup (its ACCEL_GENERATED flag
        // starts clear), so counting DISTINCT chunk indices seen here measures precisely when the
        // world has finished generating. Counting all updates instead would never terminate: the
        // wrapping volume re-generates the trailing face for as long as the player moves.
        if (!g_worldgen.started) {
            g_worldgen.started = true;
            g_worldgen.first_frame = scale_clock::now();
        }
        ++g_worldgen.frames;

        for (uint32_t chunk_update_i = 0; chunk_update_i < MAX_CHUNK_UPDATES_PER_FRAME; ++chunk_update_i) {
            if (chunk_updates[chunk_update_i].info.flags != 1) {
                // copied_bytes += sizeof(uint32_t);
                continue;
            }
            auto chunk_update = chunk_updates[chunk_update_i];
            ++g_worldgen.cumulative_updates;
            if (chunk_update.info.chunk_index < g_worldgen.seen.size() &&
                !g_worldgen.seen[chunk_update.info.chunk_index]) {
                g_worldgen.seen[chunk_update.info.chunk_index] = true;
                ++g_worldgen.distinct_chunks;
            }
            copied_bytes += sizeof(chunk_update);
            auto &chunk = voxel_chunks[chunk_update.info.chunk_index];
            for (uint32_t palette_region_i = 0; palette_region_i < PALETTES_PER_CHUNK; ++palette_region_i) {
                auto const &palette_header = chunk_update.palette_headers[palette_region_i];
                auto &palette_chunk = chunk.palette_chunks[palette_region_i];
                auto palette_size = palette_header.variant_n;
                auto compressed_size = 0u;
                if (palette_chunk.variant_n > 1) {
                    delete[] palette_chunk.blob_ptr;
                }
                palette_chunk.variant_n = palette_size;
                auto bits_per_variant = ceil_log2(palette_size);
                if (palette_size > PALETTE_MAX_COMPRESSED_VARIANT_N) {
                    compressed_size = PALETTE_REGION_TOTAL_SIZE;
                } else if (palette_size > 1) {
                    compressed_size = palette_size + (bits_per_variant * PALETTE_REGION_TOTAL_SIZE + 31) / 32;
                } else {
                    // no blob
                }
                if (compressed_size != 0) {
                    palette_chunk.blob_ptr = new uint32_t[compressed_size];
                    memcpy(palette_chunk.blob_ptr, output_heap + palette_header.blob_ptr, compressed_size * sizeof(uint32_t));
                    copied_bytes += compressed_size * sizeof(uint32_t);
                } else {
                    palette_chunk.blob_ptr = std::bit_cast<uint32_t *>(size_t(palette_header.blob_ptr));
                }
            }
        }

        // if (copied_bytes > 0) {
        //     debug_utils::Console::add_log(fmt::format("{} MB copied", double(copied_bytes) / 1'000'000.0));
        // }

        if (!g_worldgen.complete_logged && g_worldgen.distinct_chunks >= g_worldgen.seen.size() &&
            !g_worldgen.seen.empty()) {
            g_worldgen.complete_logged = true;
            log_table_census(voxel_chunks);
            auto const secs = std::chrono::duration<double>(scale_clock::now() - g_worldgen.first_frame).count();
            debug_utils::Console::add_log(fmt::format(
                "[scale] WORLD GENERATED: {} chunks in {} frames, {:.2f} s ({:.0f} chunks/frame avg)\n",
                g_worldgen.distinct_chunks, g_worldgen.frames, secs,
                double(g_worldgen.cumulative_updates) / double(g_worldgen.frames)));
        }
        // Progress, so a run that never finishes generating is diagnosable rather than just slow.
        // Once every 256 frames: at 128 chunks/frame that is one line per 32768 chunks.
        if (!g_worldgen.complete_logged && (g_worldgen.frames % 256) == 0) {
            auto const secs = std::chrono::duration<double>(scale_clock::now() - g_worldgen.first_frame).count();
            debug_utils::Console::add_log(fmt::format(
                "[scale] generating: {}/{} chunks ({:.1f}%), frame {}, t={:.2f} s\n",
                g_worldgen.distinct_chunks, g_worldgen.seen.size(),
                100.0 * double(g_worldgen.distinct_chunks) / double(g_worldgen.seen.size()),
                g_worldgen.frames, secs));
        }
        debug_utils::DebugDisplay::set_debug_string(
            "World Gen",
            g_worldgen.complete_logged
                ? fmt::format("{} chunks done", g_worldgen.seen.size())
                : fmt::format("{}/{}", g_worldgen.distinct_chunks, g_worldgen.seen.size()));
    }
}

void VoxelWorld::record_frame(GpuContext &gpu_context, daxa::TaskBufferView task_gvox_model_buffer, VoxelParticles &particles) {
    gpu_context.add(ComputeTask<VoxelWorldPerframeCompute::Task, VoxelWorldPerframeComputePush, NoTaskInfo>{
        .source = daxa::ShaderFile{"voxels/impl/perframe.comp.glsl"},
        .views = std::array{
            daxa::TaskViewVariant{std::pair{VoxelWorldPerframeCompute::AT.gpu_input, gpu_context.task_input_buffer}},
            daxa::TaskViewVariant{std::pair{VoxelWorldPerframeCompute::AT.gpu_output, gpu_context.task_output_buffer}},
            daxa::TaskViewVariant{std::pair{VoxelWorldPerframeCompute::AT.chunk_updates, buffers.chunk_updates.task_resource}},
            VOXELS_BUFFER_USES_ASSIGN(VoxelWorldPerframeCompute, buffers),
        },
        .callback_ = [](daxa::TaskInterface const &ti, daxa::ComputePipeline &pipeline, VoxelWorldPerframeComputePush &push, NoTaskInfo const &) {
            ti.recorder.set_pipeline(pipeline);
            set_push_constant(ti, push);
            ti.recorder.dispatch({1, 1, 1});
        },
    });

    gpu_context.add(ComputeTask<PerChunkCompute::Task, PerChunkComputePush, NoTaskInfo>{
        .source = daxa::ShaderFile{"voxels/impl/voxel_world.comp.glsl"},
        .views = std::array{
            daxa::TaskViewVariant{std::pair{PerChunkCompute::AT.gpu_input, gpu_context.task_input_buffer}},
            daxa::TaskViewVariant{std::pair{PerChunkCompute::AT.gvox_model, task_gvox_model_buffer}},
            daxa::TaskViewVariant{std::pair{PerChunkCompute::AT.voxel_globals, buffers.voxel_globals.task_resource}},
            daxa::TaskViewVariant{std::pair{PerChunkCompute::AT.voxel_chunks, buffers.voxel_chunks.task_resource}},
            daxa::TaskViewVariant{std::pair{PerChunkCompute::AT.value_noise_texture, gpu_context.task_value_noise_image_view}},
        },
        .callback_ = [](daxa::TaskInterface const &ti, daxa::ComputePipeline &pipeline, PerChunkComputePush &push, NoTaskInfo const &) {
            ti.recorder.set_pipeline(pipeline);
            set_push_constant(ti, push);
            auto const dispatch_size = CHUNKS_DISPATCH_SIZE;
            ti.recorder.dispatch({dispatch_size, dispatch_size, dispatch_size});
        },
    });

    auto task_temp_voxel_chunks_buffer = gpu_context.frame_task_graph.create_transient_buffer({
        .size = sizeof(TempVoxelChunk) * MAX_CHUNK_UPDATES_PER_FRAME,
        .name = "temp_voxel_chunks_buffer",
    });

    gpu_context.add(ComputeTask<ChunkEditCompute::Task, ChunkEditComputePush, NoTaskInfo>{
        .source = daxa::ShaderFile{"voxels/impl/voxel_world.comp.glsl"},
        .views = std::array{
            daxa::TaskViewVariant{std::pair{ChunkEditCompute::AT.gpu_input, gpu_context.task_input_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkEditCompute::AT.gvox_model, task_gvox_model_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkEditCompute::AT.voxel_globals, buffers.voxel_globals.task_resource}},
            daxa::TaskViewVariant{std::pair{ChunkEditCompute::AT.voxel_chunks, buffers.voxel_chunks.task_resource}},
            daxa::TaskViewVariant{std::pair{ChunkEditCompute::AT.voxel_malloc_page_allocator, buffers.voxel_malloc.task_allocator_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkEditCompute::AT.temp_voxel_chunks, task_temp_voxel_chunks_buffer}},
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
        .views = std::array{
            daxa::TaskViewVariant{std::pair{ChunkEditPostProcessCompute::AT.gpu_input, gpu_context.task_input_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkEditPostProcessCompute::AT.gvox_model, task_gvox_model_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkEditPostProcessCompute::AT.voxel_globals, buffers.voxel_globals.task_resource}},
            daxa::TaskViewVariant{std::pair{ChunkEditPostProcessCompute::AT.voxel_chunks, buffers.voxel_chunks.task_resource}},
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

    gpu_context.add(ComputeTask<ChunkOptCompute::Task, ChunkOptComputePush, NoTaskInfo>{
        .source = daxa::ShaderFile{"voxels/impl/voxel_world.comp.glsl"},
        .extra_defines = {{"CHUNK_OPT_STAGE", "0"}},
        .views = std::array{
            daxa::TaskViewVariant{std::pair{ChunkOptCompute::AT.gpu_input, gpu_context.task_input_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkOptCompute::AT.voxel_globals, buffers.voxel_globals.task_resource}},
            daxa::TaskViewVariant{std::pair{ChunkOptCompute::AT.temp_voxel_chunks, task_temp_voxel_chunks_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkOptCompute::AT.voxel_chunks, buffers.voxel_chunks.task_resource}},
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
        .extra_defines = {{"CHUNK_OPT_STAGE", "1"}},
        .views = std::array{
            daxa::TaskViewVariant{std::pair{ChunkOptCompute::AT.gpu_input, gpu_context.task_input_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkOptCompute::AT.voxel_globals, buffers.voxel_globals.task_resource}},
            daxa::TaskViewVariant{std::pair{ChunkOptCompute::AT.temp_voxel_chunks, task_temp_voxel_chunks_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkOptCompute::AT.voxel_chunks, buffers.voxel_chunks.task_resource}},
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

    gpu_context.add(ComputeTask<ChunkAllocCompute::Task, ChunkAllocComputePush, NoTaskInfo>{
        .source = daxa::ShaderFile{"voxels/impl/voxel_world.comp.glsl"},
        .views = std::array{
            daxa::TaskViewVariant{std::pair{ChunkAllocCompute::AT.gpu_input, gpu_context.task_input_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkAllocCompute::AT.voxel_globals, buffers.voxel_globals.task_resource}},
            daxa::TaskViewVariant{std::pair{ChunkAllocCompute::AT.temp_voxel_chunks, task_temp_voxel_chunks_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkAllocCompute::AT.voxel_chunks, buffers.voxel_chunks.task_resource}},
            daxa::TaskViewVariant{std::pair{ChunkAllocCompute::AT.voxel_malloc_page_allocator, buffers.voxel_malloc.task_allocator_buffer}},
            daxa::TaskViewVariant{std::pair{ChunkAllocCompute::AT.chunk_updates, buffers.chunk_updates.task_resource}},
            daxa::TaskViewVariant{std::pair{ChunkAllocCompute::AT.chunk_update_heap, buffers.chunk_update_heap.task_resource}},
        },
        .callback_ = [](daxa::TaskInterface const &ti, daxa::ComputePipeline &pipeline, ChunkAllocComputePush &push, NoTaskInfo const &) {
            ti.recorder.set_pipeline(pipeline);
            set_push_constant(ti, push);
            ti.recorder.dispatch_indirect({
                .indirect_buffer = ti.get(ChunkAllocCompute::AT.voxel_globals).ids[0],
                // NOTE: This should always have the same value as the chunk edit dispatch, so we're re-using it here
                .offset = offsetof(VoxelWorldGlobals, indirect_dispatch) + offsetof(VoxelWorldGpuIndirectDispatch, chunk_edit_dispatch),
            });
        },
    });

    // The far field's own generation chain, recorded LAST and on purpose: it reuses this
    // function's temp_voxel_chunks transient, and the task graph orders the two chains on that
    // shared resource in recording order. docs/FAR_FIELD.md.
    far_field_record_frame(gpu_context, buffers, task_gvox_model_buffer, task_temp_voxel_chunks_buffer, particles);
}
