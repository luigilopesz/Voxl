#include <renderer/trace_primary.inl>

#include <renderer/kajiya/inc/camera.glsl>
#include <voxels/voxels.glsl>

// --- Step-count instrument ------------------------------------------------------------------
//
// VoxelTraceResult.step_n used to be computed and thrown away here, and the prepass consumer
// below read a `.y` channel the prepass never wrote. Both are wired up now, because the
// question "does MAX_STEPS bind?" is otherwise unanswerable except by inference -- and a ray
// that exhausts its budget is reported as a MISS and shaded as sky, so the symptom is a hole
// in distant geometry, which looks exactly like correct sky.
//
// Everything is normalised by VOXL_STEP_VIS_SCALE rather than by max_steps, so heatmaps taken
// at different step budgets are directly comparable. 1024 = twice the shipped MAX_STEPS.
#define VOXL_STEP_VIS_SCALE 1024.0
// Rows at the bottom of the heatmap that carry a known 0..1 ramp instead of data. The image
// reaches the PNG through an sRGB swapchain; the ramp lets the analysis *measure* that
// transfer and invert it rather than assume it. Excluded from every histogram.
#define VOXL_STEP_VIS_CALIB_ROWS 4

#if TraceDepthPrepassComputeShader

DAXA_DECL_PUSH_CONSTANT(TraceDepthPrepassComputePush, push)
daxa_BufferPtr(GpuInput) gpu_input = push.uses.gpu_input;
daxa_ImageViewIndex render_depth_prepass_image = push.uses.render_depth_prepass_image;
VOXELS_USE_BUFFERS_PUSH_USES(daxa_BufferPtr)

#define INPUT deref(gpu_input)
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main() {
    vec4 output_tex_size;
    output_tex_size.xy = deref(gpu_input).frame_dim;
    output_tex_size.zw = vec2(1.0, 1.0) / output_tex_size.xy;
    vec2 uv = get_uv(gl_GlobalInvocationID.xy * PREPASS_SCL, output_tex_size);

    ViewRayContext vrc = vrc_from_uv(gpu_input, uv);
    vec3 ray_dir = ray_dir_ws(vrc);
    vec3 cam_pos = ray_origin_ws(vrc);
    vec3 ray_pos = cam_pos;

#if ENABLE_DEPTH_PREPASS
    VoxelTraceResult trace_result = voxel_trace(VoxelTraceInfo(VOXELS_BUFFER_PTRS, ray_dir, MAX_STEPS, MAX_DIST, 16.0 * output_tex_size.w * deref(gpu_input).player.cam.clip_to_view[1][1], true), ray_pos);
#endif

    float depth = length(ray_pos - cam_pos);
#if ENABLE_DEPTH_PREPASS
    float steps = float(trace_result.step_n);
#else
    float steps = 0.0;
#endif

    if (any(greaterThanEqual(gl_GlobalInvocationID.xy, (uvec2(output_tex_size.xy)) / PREPASS_SCL))) {
        return;
    }
    // .y is the step count the consumer in TracePrimaryComputeShader has always read and
    // nothing has ever written. It is raw here; the /4 the consumer used to apply was a
    // leftover from an upstream packing that no longer exists.
    imageStore(daxa_image2D(render_depth_prepass_image), ivec2(gl_GlobalInvocationID.xy), vec4(depth, steps, 0, 0));
}
#undef INPUT

#endif

#if TracePrimaryComputeShader

DAXA_DECL_PUSH_CONSTANT(TracePrimaryComputePush, push)
daxa_BufferPtr(GpuInput) gpu_input = push.uses.gpu_input;
VOXELS_USE_BUFFERS_PUSH_USES(daxa_BufferPtr)
daxa_ImageViewIndex blue_noise_vec2 = push.uses.blue_noise_vec2;
daxa_ImageViewIndex debug_texture = push.uses.debug_texture;
daxa_ImageViewIndex render_depth_prepass_image = push.uses.render_depth_prepass_image;
daxa_ImageViewIndex g_buffer_image_id = push.uses.g_buffer_image_id;
daxa_ImageViewIndex velocity_image_id = push.uses.velocity_image_id;
daxa_ImageViewIndex vs_normal_image_id = push.uses.vs_normal_image_id;
daxa_ImageViewIndex depth_image_id = push.uses.depth_image_id;
daxa_ImageViewIndex step_count_image_id = push.uses.step_count_image_id;

#include <renderer/atmosphere/sky.glsl>

#define PIXEL_I gl_GlobalInvocationID.xy
#define INPUT deref(gpu_input)
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
void main() {
    vec4 output_tex_size = vec4(deref(gpu_input).frame_dim, 0, 0);
    output_tex_size.zw = vec2(1.0, 1.0) / output_tex_size.xy;
    vec2 uv = get_uv(PIXEL_I, output_tex_size);

    ViewRayContext vrc = vrc_from_uv(gpu_input, uv);
    vec3 ray_dir = ray_dir_ws(vrc);
    vec3 cam_pos = ray_origin_ws(vrc);

#if !ENABLE_DEPTH_PREPASS
    float prepass_depth = 0.0;
    float prepass_steps = 0.0;
#else
    float max_depth = MAX_DIST;
    float prepass_depth = max_depth;
    float prepass_steps = 0.0;

    for (int yi = -1; yi <= 1; ++yi) {
        for (int xi = -1; xi <= 1; ++xi) {
            ivec2 pt = ivec2(PIXEL_I / PREPASS_SCL) + ivec2(xi, yi);
            pt = clamp(pt, ivec2(0), ivec2(deref(gpu_input).frame_dim / PREPASS_SCL - 1));
            vec2 prepass_data = texelFetch(daxa_texture2D(render_depth_prepass_image), pt, 0).xy;
            float loaded_depth = prepass_data.x - VOXEL_SIZE;
            prepass_depth = max(min(prepass_depth, loaded_depth), 0);
            if (prepass_depth == loaded_depth || prepass_depth == max_depth) {
                prepass_steps = prepass_data.y;
            }
        }
    }
#endif

    vec3 ray_pos = cam_pos + ray_dir * prepass_depth;

    VoxelTraceResult trace_result = voxel_trace(VoxelTraceInfo(VOXELS_BUFFER_PTRS, ray_dir, MAX_STEPS, MAX_DIST, 0.0, true), ray_pos);
    uint step_n = trace_result.step_n;
    // voxel_trace's loop runs `for (step_n = 0; step_n < max_steps; ++step_n)`, so step_n only
    // equals the budget when the loop was exhausted -- a ray that left the world box or found a
    // surface stops short. voxel_trace may override the budget itself (VOXL_MAX_STEPS in
    // voxels/impl/trace.glsl), so read it back from the result rather than assuming MAX_STEPS.
    bool ran_out_of_steps = (step_n >= VOXL_TRACE_MAX_STEPS(MAX_STEPS));

    uvec4 output_value = uvec4(0);

    // float depth = length(cam_pos - ray_pos);
    // transform depth to ndc space
    vec4 vs_pos = (deref(gpu_input).player.cam.world_to_view * vec4(ray_pos, 1));
    vec4 prev_vs_pos = (deref(gpu_input).player.cam.world_to_view * vec4(ray_pos + trace_result.vel, 1)); // when animated objects exist, this is where they'd put their velocity
    vec4 ss_pos = (deref(gpu_input).player.cam.view_to_sample * vs_pos);
    float depth = ss_pos.z / ss_pos.w;
    vec3 vs_nrm = vec3(0);
    vec3 vs_velocity = vec3(0);

#if !PER_VOXEL_NORMALS && defined(VOXELS_ORIGINAL_IMPL)
    bool is_valid = true;
    if (trace_result.dist != MAX_DIST) {
        uvec3 chunk_n = uvec3(CHUNKS_PER_AXIS);
        PackedVoxel voxel_data = sample_voxel_chunk(VOXELS_BUFFER_PTRS, chunk_n, ray_pos, trace_result.nrm * 0.5);
        Voxel voxel = unpack_voxel(voxel_data);
        is_valid = voxel.material_type == 0;
    }
    vec3 old_nrm = trace_result.nrm;
    if (!is_valid) {
        trace_result.nrm = vec3(0.0);
    }
    vec3 valid_nrm = vec3(0);
    uint thread2x2_root_index = gl_SubgroupInvocationID & (~(8 | 1));
    valid_nrm.x += subgroupBroadcast(trace_result.nrm.x, thread2x2_root_index + 0);
    valid_nrm.y += subgroupBroadcast(trace_result.nrm.y, thread2x2_root_index + 0);
    valid_nrm.z += subgroupBroadcast(trace_result.nrm.z, thread2x2_root_index + 0);
    valid_nrm.x += subgroupBroadcast(trace_result.nrm.x, thread2x2_root_index + 1);
    valid_nrm.y += subgroupBroadcast(trace_result.nrm.y, thread2x2_root_index + 1);
    valid_nrm.z += subgroupBroadcast(trace_result.nrm.z, thread2x2_root_index + 1);
    valid_nrm.x += subgroupBroadcast(trace_result.nrm.x, thread2x2_root_index + 8);
    valid_nrm.y += subgroupBroadcast(trace_result.nrm.y, thread2x2_root_index + 8);
    valid_nrm.z += subgroupBroadcast(trace_result.nrm.z, thread2x2_root_index + 8);
    valid_nrm.x += subgroupBroadcast(trace_result.nrm.x, thread2x2_root_index + 9);
    valid_nrm.y += subgroupBroadcast(trace_result.nrm.y, thread2x2_root_index + 9);
    valid_nrm.z += subgroupBroadcast(trace_result.nrm.z, thread2x2_root_index + 9);
    if (!is_valid) {
        trace_result.nrm = normalize(valid_nrm);
    }
    if (dot(valid_nrm, valid_nrm) == 0.0) {
        trace_result.nrm = old_nrm;
    }
#endif

    if (trace_result.dist == MAX_DIST) {
        output_value.y = nrm_to_u16(vec3(0, 0, 1));
        depth = 0.0;
    } else {
        output_value.x = trace_result.voxel_data.data;
        output_value.y = nrm_to_u16(trace_result.nrm);
        vs_nrm = (deref(gpu_input).player.cam.world_to_view * vec4(trace_result.nrm, 0)).xyz;
        vs_velocity = (prev_vs_pos.xyz / prev_vs_pos.w) - (vs_pos.xyz / vs_pos.w);
    }
    output_value.z = floatBitsToUint(depth);

    if (any(greaterThanEqual(gl_GlobalInvocationID.xy, uvec2(output_tex_size.xy)))) {
        return;
    }

    vs_nrm *= -sign(dot(ray_dir_vs(vrc), vs_nrm));

    imageStore(daxa_uimage2D(g_buffer_image_id), ivec2(gl_GlobalInvocationID.xy), output_value);
    imageStore(daxa_image2D(velocity_image_id), ivec2(gl_GlobalInvocationID.xy), vec4(vs_velocity, 0));
    imageStore(daxa_image2D(vs_normal_image_id), ivec2(gl_GlobalInvocationID.xy), vec4(vs_nrm * 0.5 + 0.5, 0));
    imageStore(daxa_image2D(depth_image_id), ivec2(gl_GlobalInvocationID.xy), vec4(depth, 0, 0, 0));

    // --- the step heatmap -------------------------------------------------------------------
    //   R  main-trace steps / VOXL_STEP_VIS_SCALE
    //   G  depth-prepass steps for this pixel's prepass texel, same scale
    //   B  0.00 = clean miss (left the world box; genuinely sky)
    //      0.50 = hit a surface
    //      1.00 = RAN OUT OF STEPS. Reported as a miss and shaded as sky, but there may be
    //             geometry behind it. Every such pixel is a hole.
    // The bottom VOXL_STEP_VIS_CALIB_ROWS rows carry a 0..1 ramp instead, so the analysis can
    // measure the swapchain's transfer function out of the PNG rather than assuming sRGB.
    vec3 vis;
    if (gl_GlobalInvocationID.y + VOXL_STEP_VIS_CALIB_ROWS >= uint(output_tex_size.y)) {
        float ramp = float(gl_GlobalInvocationID.x) / max(output_tex_size.x - 1.0, 1.0);
        vis = vec3(ramp);
    } else {
        vis = vec3(
            float(step_n) * (1.0 / VOXL_STEP_VIS_SCALE),
            prepass_steps * (1.0 / VOXL_STEP_VIS_SCALE),
            ran_out_of_steps ? 1.0 : (trace_result.dist == MAX_DIST ? 0.0 : 0.5));
    }
    imageStore(daxa_image2D(step_count_image_id), ivec2(gl_GlobalInvocationID.xy), vec4(vis, 1.0));
}
#undef INPUT

#endif
