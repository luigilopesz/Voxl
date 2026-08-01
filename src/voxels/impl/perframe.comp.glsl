#include "voxel_world.inl"

DAXA_DECL_PUSH_CONSTANT(VoxelWorldPerframeComputePush, push)
daxa_BufferPtr(GpuInput) gpu_input = push.uses.gpu_input;
daxa_RWBufferPtr(GpuOutput) gpu_output = push.uses.gpu_output;
daxa_RWBufferPtr(ChunkUpdate) chunk_updates = push.uses.chunk_updates;
VOXELS_USE_BUFFERS_PUSH_USES(daxa_RWBufferPtr)

#include <renderer/kajiya/inc/camera.glsl>
#include <voxels/voxels.glsl>
// #include <voxels/voxel_particle.glsl>

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
void main() {
    VoxelRWBufferPtrs ptrs = VOXELS_RW_BUFFER_PTRS;

    uint frame_index = deref(gpu_input).frame_index % (FRAMES_IN_FLIGHT + 1);
    for (uint i = 0; i < MAX_CHUNK_UPDATES_PER_FRAME; ++i) {
        deref(ptrs.globals).chunk_update_infos[i].brush_flags = 0;
        deref(ptrs.globals).chunk_update_infos[i].i = INVALID_CHUNK_I;

        deref(advance(chunk_updates, frame_index * MAX_CHUNK_UPDATES_PER_FRAME + i)).info.flags = 0;
    }

    deref(ptrs.globals).chunk_update_n = 0;
    deref(ptrs.globals).chunk_update_heap_alloc_n = 0;

    deref(ptrs.globals).prev_offset = deref(ptrs.globals).offset;
    deref(ptrs.globals).offset = deref(gpu_input).player.player_unit_offset;

    deref(ptrs.globals).indirect_dispatch.chunk_edit_dispatch = uvec3(CHUNK_SIZE / 8, CHUNK_SIZE / 8, 0);
    deref(ptrs.globals).indirect_dispatch.subchunk_x2x4_dispatch = uvec3(1, 64, 0);
    deref(ptrs.globals).indirect_dispatch.subchunk_x8up_dispatch = uvec3(1, 1, 0);

    VoxelMallocPageAllocator_perframe(ptrs.allocator);
    // VoxelLeafChunkAllocator_perframe(ptrs.voxel_leaf_chunk_allocator);
    // VoxelParentChunkAllocator_perframe(ptrs.voxel_parent_chunk_allocator);

    deref(advance(gpu_output, deref(gpu_input).fif_index)).voxel_world.voxel_malloc_output.current_element_count =
        VoxelMallocPageAllocator_get_consumed_element_count(daxa_BufferPtr(VoxelMallocPageAllocator)(as_address(ptrs.allocator)));
    // deref(advance(gpu_output, deref(gpu_input).fif_index)).voxel_world.voxel_leaf_chunk_output.current_element_count =
    //     VoxelLeafChunkAllocator_get_consumed_element_count(voxel_leaf_chunk_allocator);
    // deref(advance(gpu_output, deref(gpu_input).fif_index)).voxel_world.voxel_parent_chunk_output.current_element_count =
    //     VoxelParentChunkAllocator_get_consumed_element_count(voxel_parent_chunk_allocator);

    // Brush stuff
    vec2 frame_dim = deref(gpu_input).frame_dim;
    vec2 inv_frame_dim = vec2(1.0) / frame_dim;
    vec2 uv = get_uv(deref(gpu_input).mouse.pos, vec4(frame_dim, inv_frame_dim));
    ViewRayContext vrc = unjittered_vrc_from_uv(gpu_input, uv);
    vec3 ray_dir = ray_dir_ws(vrc);
    vec3 cam_pos = ray_origin_ws(vrc);
    vec3 ray_pos = cam_pos;
    VoxelTraceResult pick = voxel_trace(VoxelTraceInfo(VOXELS_BUFFER_PTRS, ray_dir, MAX_STEPS, MAX_DIST, 0.0, true), ray_pos);

    if (deref(ptrs.globals).brush_state.is_editing == 0) {
        // THE PICK DISTANCE COMES FROM THE TRACE RESULT, NOT FROM WHERE IT LEFT ray_pos.
        // This line used to read `ray_pos - cam_pos`, and voxel_trace() only advances ray_pos
        // when it actually HITS something (voxels/impl/trace.glsl:123-128). On a miss it hands
        // the origin straight back, so that vector was zero and the brush point landed inside
        // the camera. It was invisible for as long as the only brush was remove-terrain -- it
        // dug a hole around the player's feet and looked like a targeting quirk -- and it
        // entombs the player the moment an add brush exists. Reproducible from a cold start:
        // gpu_input.mouse.pos is (0,0) until the mouse first physically moves, which aims the
        // picking ray at the corner of the frame, which is usually sky.
        //
        // result.dist is the honest answer in both cases: the hit distance, or max_dist (1e9)
        // on a miss because extend_to_max_dist is set. Clamping it gives the tool a reach and
        // keeps 1e9 out of the chunk-index arithmetic downstream.
        float pick_dist = clamp(pick.dist, BRUSH_PICK_MIN_M, BRUSH_PICK_MAX_M);
        deref(ptrs.globals).brush_state.initial_ray = ray_dir * pick_dist;
    }

    // ---- the tool and its size ------------------------------------------------------------
    //
    // brush_id and radius live in VoxelWorldGlobals rather than being recomputed each frame
    // because they are player state that has to persist, and this buffer is the only place a
    // shader has to keep any. This block, and nothing else, decides what is in them; the
    // election shader and every brush read them.
    //
    // The globals buffer starts zero-filled, so "radius is not a sane number yet" is the
    // signal for a first frame. Written as !(r >= MIN) rather than (r < MIN) so a NaN --
    // which compares false against everything -- also lands on the default instead of
    // propagating into every capsule distance in the frame.
    uint brush_id = deref(ptrs.globals).brush_input.brush_id;
    float brush_radius = deref(ptrs.globals).brush_input.radius;
    uint brush_flags = deref(ptrs.globals).brush_input.flags;
    if (!(brush_radius >= BRUSH_RADIUS_MIN)) {
        brush_radius = BRUSH_RADIUS_DEFAULT;
    }

#if BRUSH_SELECTION_FROM_CPU
    // The C++ tool UI owns the selection. See brushes.inl: add BrushSelection to GpuInput,
    // fill it from the tool state each frame, and flip BRUSH_SELECTION_FROM_CPU to 1.
    brush_id = deref(gpu_input).brush_selection.brush_id % BRUSH_ID_COUNT;
    brush_radius = clamp(deref(gpu_input).brush_selection.radius, BRUSH_RADIUS_MIN, BRUSH_RADIUS_MAX);
#else
    // The shader-driven fallback. NOT COMPILED TODAY -- the tool UI landed and
    // BRUSH_SELECTION_FROM_CPU is 1 -- and kept anyway, because it is what makes the editing
    // system stand on its own: with this branch the brushes are selectable and sizeable with
    // no C++ at all. It uses the same BRUSH_RADIUS_STEP as ToolBelt::adjust_radius(), so the
    // two paths cannot drift into different feels or different end stops. If you enable it,
    // set BRUSH_SELECTION_FROM_CPU to 0 as well or both sides will service the wheel.
    //
    // GLFW reports a held key as a LEVEL, not an edge -- 1 on press, 2 once auto-repeat
    // starts, 0 on release (voxel_app.cpp:379 stores the raw GLFW action) -- so one press
    // would otherwise run through every tool. brush_input.flags carries the latch.
    bool cycle_down = deref(gpu_input).actions[GAME_ACTION_TOGGLE_BRUSH] != 0;
    bool cycle_was_down = (brush_flags & BRUSH_INPUT_FLAG_CYCLE_LATCH) != 0;
    if (cycle_down && !cycle_was_down) {
        brush_id = (brush_id + 1) % BRUSH_ID_COUNT;
    }
    brush_flags &= ~BRUSH_INPUT_FLAG_CYCLE_LATCH;
    if (cycle_down) {
        brush_flags |= BRUSH_INPUT_FLAG_CYCLE_LATCH;
    }

    // MULTIPLICATIVE, not additive. The range is 0.125 m to 8 m -- a factor of 64 -- so a
    // linear step is either unusably coarse at the bottom or interminable at the top. At
    // 1.25x a notch it is 19 notches end to end and every notch feels the same size.
    // scroll_delta accumulates within a frame and the CPU clears it after the frame graph
    // runs (voxel_app.cpp:246), so this reads exactly one frame's worth of wheel.
    float scroll = deref(gpu_input).mouse.scroll_delta.y;
    if (scroll != 0.0) {
        brush_radius = clamp(brush_radius * pow(BRUSH_RADIUS_STEP, scroll), BRUSH_RADIUS_MIN, BRUSH_RADIUS_MAX);
    }
#endif

    // ---- the stroke -----------------------------------------------------------------------
    //
    // prev_pos is last frame's pos, and the brushes draw a capsule between the two so a
    // dragged tool paints a stroke rather than a dotted line of spheres.
    vec3 prev_world = deref(ptrs.globals).brush_input.pos + deref(ptrs.globals).brush_input.pos_offset;
    vec3 new_world = length(deref(ptrs.globals).brush_state.initial_ray) * ray_dir + cam_pos +
                     deref(gpu_input).player.player_unit_offset;

    // THE STROKE IS SHORTENED WHEN IT WOULD OUTGROW THE CHUNK BUDGET, and it is done here
    // rather than in either consumer so that both consumers see the same capsule. The
    // election shader picks chunks from the AABB of (pos, prev_pos) inflated by radius; the
    // edit shader draws that capsule. If they disagree the stroke is cut off at a chunk
    // boundary. A frame's worth of movement is centimetres, so this only bites when the view
    // swings hard mid-drag or when the radius is near its maximum -- at radius 8 m the tool
    // simply cannot be dragged, which is the honest consequence of a 5-chunk budget.
    vec3 stroke = prev_world - new_world;
    float stroke_len = length(stroke);
    float stroke_max = max(0.0, BRUSH_MAX_SPAN_M - 2.0 * brush_radius);
    if (stroke_len > stroke_max) {
        prev_world = new_world + stroke * (stroke_max / stroke_len);
    }

    // prev_pos keeps last frame's offset, so the value stored has to be taken back out of
    // world space against that offset rather than against this frame's.
    deref(ptrs.globals).brush_input.prev_pos = prev_world - deref(ptrs.globals).brush_input.pos_offset;
    deref(ptrs.globals).brush_input.prev_pos_offset = deref(ptrs.globals).brush_input.pos_offset;
    deref(ptrs.globals).brush_input.pos = new_world - deref(gpu_input).player.player_unit_offset;
    deref(ptrs.globals).brush_input.pos_offset = deref(gpu_input).player.player_unit_offset;
    deref(ptrs.globals).brush_input.brush_id = brush_id;
    deref(ptrs.globals).brush_input.radius = brush_radius;
    deref(ptrs.globals).brush_input.flags = brush_flags;

    // initial_frame is now stamped for EITHER button, not just B. The one-shot gate in
    // voxel_world.comp.glsl reads it, and which tools are one-shot is a property of the tool
    // (BRUSH_ONE_SHOT_MASK) rather than of which button was pressed -- placing a tree wants
    // to fire once whichever button did it, and painting terrain wants to fire every frame.
    if (deref(gpu_input).actions[GAME_ACTION_BRUSH_A] != 0 ||
        deref(gpu_input).actions[GAME_ACTION_BRUSH_B] != 0) {
        if (deref(ptrs.globals).brush_state.is_editing == 0) {
            deref(ptrs.globals).brush_state.initial_frame = deref(gpu_input).frame_index;
        }
        deref(ptrs.globals).brush_state.is_editing = 1;
    } else {
        deref(ptrs.globals).brush_state.is_editing = 0;
    }
}
