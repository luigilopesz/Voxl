#pragma once

// NOTE(grundlett): Merged together frame_constants.hlsl and uv.hlsl

#include <utilities/gpu/math.glsl>
#include <application/input.inl>
// VOXEL_SIZE and the FF_* constants, for the secondary-ray origin bias below. This file used
// VOXEL_SIZE already but never included the header that defines it -- it worked only because
// every translation unit happened to include voxels.glsl first. The bias now has to pick
// between two levels' voxel sizes, so the dependency is made explicit rather than left to
// include order.
#include <voxels/far_field.inl>

vec2 get_uv(ivec2 pix, vec4 tex_size) { return (vec2(pix) + 0.5) * tex_size.zw; }
vec2 get_uv(vec2 pix, vec4 tex_size) { return (pix + 0.5) * tex_size.zw; }
vec2 cs_to_uv(vec2 cs) { return cs * vec2(0.5, -0.5) + vec2(0.5, 0.5); }
vec2 uv_to_cs(vec2 uv) { return (uv - 0.5) * vec2(2, -2); }

struct ViewRayContext {
    vec4 ray_dir_cs;
    vec4 ray_dir_vs_h;
    vec4 ray_dir_ws_h;
    vec4 ray_origin_cs;
    vec4 ray_origin_vs_h;
    vec4 ray_origin_ws_h;
    vec4 ray_hit_cs;
    vec4 ray_hit_vs_h;
    vec4 ray_hit_ws_h;
};

vec3 ray_dir_vs(in ViewRayContext vrc) { return normalize(vrc.ray_dir_vs_h.xyz); }
vec3 ray_dir_ws(in ViewRayContext vrc) { return normalize(vrc.ray_dir_ws_h.xyz); }
vec3 ray_origin_vs(in ViewRayContext vrc) { return vrc.ray_origin_vs_h.xyz / vrc.ray_origin_vs_h.w; }
vec3 ray_origin_ws(in ViewRayContext vrc) { return vrc.ray_origin_ws_h.xyz / vrc.ray_origin_ws_h.w; }
vec3 ray_hit_vs(in ViewRayContext vrc) { return vrc.ray_hit_vs_h.xyz / vrc.ray_hit_vs_h.w; }
vec3 ray_hit_ws(in ViewRayContext vrc) { return vrc.ray_hit_ws_h.xyz / vrc.ray_hit_ws_h.w; }
vec3 biased_secondary_ray_origin_ws(in ViewRayContext vrc) {
    return ray_hit_ws(vrc) - ray_dir_ws(vrc) * (length(ray_hit_vs(vrc)) + length(ray_hit_ws(vrc))) * 1e-4;
}
// THE SECONDARY-RAY ORIGIN BIAS MUST CLEAR THE VOXEL THE SURFACE IS ACTUALLY MADE OF.
//
// This was `1.5 * VOXEL_SIZE` -- 1.5 near voxels, 9.4 cm -- and that is correct for every
// surface in the 64 m near box. It is WRONG for a far-field surface, and the arithmetic is
// the whole bug:
//
//   far voxel                       FF_VOXEL_SIZE = 25 cm
//   bias that was applied           1.5 * VOXEL_SIZE = 9.4 cm
//   9.4 cm < 25 cm, so the biased origin is STILL INSIDE the voxel it is standing on.
//
// It is worse than the ratio suggests, because vrc_from_uv_and_depth() snaps the hit position
// to the centre of a NEAR voxel (`floor(p * VOXEL_SCL) + 0.5`), a 6.25 cm grid that has no
// relationship to the 25 cm cell the far surface occupies; that snap alone can move the origin
// up to 5.4 cm further in.
//
// WHAT IT LOOKED LIKE. voxel_trace() sees an origin outside the near box, hands straight to
// ff_trace_far() with t_begin 0, which biases by another 0.01 * FF_VOXEL_SIZE = 2.5 mm and
// samples: lod == 0, solid at the hand-off point, reported as an occluder at distance zero.
// trace_secondary.comp.glsl reads `hit = (dist == MAX_DIST)`, so the pixel is fully shadowed.
// MEASURED at the vista pose: 1.06% of terrain pixels pure black with the sun ray in the far
// field, 0.17% with sun shadows off, i.e. every one of the chunk-sized black wedges across the
// distant mountains was a far-field surface shadowing itself. See docs/HANDOFF.md.
//
// The threshold is the near field's own view radius, so it tracks CHUNKS_PER_AXIS. Near
// geometry does exist slightly past it -- the box is a cube, so its corners reach
// sqrt(3) * radius -- but a 37.5 cm bias at 32 m and beyond is well under one pixel, while at
// less than 32 m it would leak light through grass blades and the spruce.
// AND THE NORMAL IS NOT A DIRECTION YOU CAN TRUST TO ESCAPE ALONG. The bias is applied along
// gbuffer.normal, which under PER_VOXEL_NORMALS is the voxel's own packed normal -- an 8-bit
// octahedral index, and at L1 an average over a 1 m filter footprint rather than the face the
// ray actually crossed. Where it disagrees with the face, `normal * k` walks along the surface
// instead of away from it and no magnitude of k escapes the voxel. MEASURED: sizing the bias
// to the far voxel alone (VOXL_SECONDARY_BIAS_NRM 1.5, VIEW 0) cleaned the distant peaks but
// left the mid-ground hills wedged, 1.056% -> 0.984%.
//
// -ray_dir_ws is the direction that cannot fail. The primary ray reached this point THROUGH
// EMPTY SPACE, so retreating along it is retreating into space that was just proven empty,
// whatever the stored normal claims. The normal term is kept because it is what lifts the
// origin off a surface the view ray is grazing.
#define VOXL_SECONDARY_BIAS_NRM 1.5
#define VOXL_SECONDARY_BIAS_VIEW 1.5

#if FF_ENABLE
#define VOXL_SECONDARY_BIAS_VOXEL(view_dist) \
    (((view_dist) > (float(CHUNKS_PER_AXIS) * CHUNK_WORLDSPACE_SIZE * 0.5)) ? float(FF_VOXEL_SIZE) : float(VOXEL_SIZE))
#else
#define VOXL_SECONDARY_BIAS_VOXEL(view_dist) float(VOXEL_SIZE)
#endif

vec3 biased_secondary_ray_origin_ws_with_normal(in ViewRayContext vrc, vec3 normal) {
    vec3 ws_abs = abs(ray_hit_ws(vrc));
    float max_comp = max(max(ws_abs.x, ws_abs.y), max(ws_abs.z, -ray_hit_vs(vrc).z));
    vec3 origin = ray_hit_ws(vrc) + (normal - ray_dir_ws(vrc)) * max(1e-4, max_comp * 1e-6);
#if PER_VOXEL_NORMALS
    float view_dist = length(ray_hit_vs(vrc));
    float bias_voxel = VOXL_SECONDARY_BIAS_VOXEL(view_dist);
    // The view term is far-field only: at 6.25 cm the normal bias has always been sufficient,
    // and retreating along the view ray inside the near box would pull secondary origins off
    // grass blades and thin foliage that the near march resolves correctly today.
    float view_k = (bias_voxel > float(VOXEL_SIZE)) ? float(VOXL_SECONDARY_BIAS_VIEW) : 0.0;
    return origin + (normal * float(VOXL_SECONDARY_BIAS_NRM) - ray_dir_ws(vrc) * view_k) * bias_voxel;
#else
    return origin;
#endif
}
ViewRayContext vrc_from_uv(daxa_BufferPtr(GpuInput) gpu_input, vec2 uv) {
    ViewRayContext res;
    res.ray_dir_cs = vec4(uv_to_cs(uv), 0.0, 1.0);
    res.ray_dir_vs_h = deref(gpu_input).player.cam.sample_to_view * res.ray_dir_cs;
    res.ray_dir_ws_h = deref(gpu_input).player.cam.view_to_world * res.ray_dir_vs_h;
    res.ray_origin_cs = vec4(uv_to_cs(uv), 1.0, 1.0);
    res.ray_origin_vs_h = deref(gpu_input).player.cam.sample_to_view * res.ray_origin_cs;
    res.ray_origin_ws_h = deref(gpu_input).player.cam.view_to_world * res.ray_origin_vs_h;
    return res;
}
ViewRayContext unjittered_vrc_from_uv(daxa_BufferPtr(GpuInput) gpu_input, vec2 uv) {
    ViewRayContext res;
    res.ray_dir_cs = vec4(uv_to_cs(uv), 0.0, 1.0);
    res.ray_dir_vs_h = deref(gpu_input).player.cam.clip_to_view * res.ray_dir_cs;
    res.ray_dir_ws_h = deref(gpu_input).player.cam.view_to_world * res.ray_dir_vs_h;
    res.ray_origin_cs = vec4(uv_to_cs(uv), 1.0, 1.0);
    res.ray_origin_vs_h = deref(gpu_input).player.cam.clip_to_view * res.ray_origin_cs;
    res.ray_origin_ws_h = deref(gpu_input).player.cam.view_to_world * res.ray_origin_vs_h;
    return res;
}
ViewRayContext vrc_from_uv_and_depth(daxa_BufferPtr(GpuInput) gpu_input, vec2 uv, float depth) {
    ViewRayContext res;
    res.ray_dir_cs = vec4(uv_to_cs(uv), 0.0, 1.0);
    res.ray_dir_vs_h = deref(gpu_input).player.cam.sample_to_view * res.ray_dir_cs;
    res.ray_dir_ws_h = deref(gpu_input).player.cam.view_to_world * res.ray_dir_vs_h;
    res.ray_origin_cs = vec4(uv_to_cs(uv), 1.0, 1.0);
    res.ray_origin_vs_h = deref(gpu_input).player.cam.sample_to_view * res.ray_origin_cs;
    res.ray_origin_ws_h = deref(gpu_input).player.cam.view_to_world * res.ray_origin_vs_h;
    res.ray_hit_cs = vec4(uv_to_cs(uv), depth, 1.0);
    res.ray_hit_vs_h = deref(gpu_input).player.cam.sample_to_view * res.ray_hit_cs;
    res.ray_hit_ws_h = deref(gpu_input).player.cam.view_to_world * res.ray_hit_vs_h;
#if PER_VOXEL_NORMALS
    res.ray_hit_ws_h = vec4(res.ray_hit_ws_h.xyz / res.ray_hit_ws_h.w, 1.0);
    res.ray_hit_ws_h.xyz = (floor(res.ray_hit_ws_h.xyz * VOXEL_SCL) + 0.5) * VOXEL_SIZE;
    res.ray_hit_vs_h = deref(gpu_input).player.cam.world_to_view * res.ray_hit_ws_h;
#endif
    return res;
}
ViewRayContext vrc_from_uv_and_biased_depth(daxa_BufferPtr(GpuInput) gpu_input, vec2 uv, float depth) {
#if PER_VOXEL_NORMALS
    // When using per-voxel normals, we want to ensure the depth represents one within a voxel.
    // We do this because we'll likely round the position to be the center of the voxel.
    const float BIAS = uintBitsToFloat(0x3f7ffe00); // uintBitsToFloat(0x3f7ffe00) == 0.999969482421875
#else
    const float BIAS = uintBitsToFloat(0x3f800040); // uintBitsToFloat(0x3f800040) == 1.00000762939453125
#endif
    return vrc_from_uv_and_depth(gpu_input, uv, min(1.0, depth * BIAS));
}

vec3 get_eye_position(daxa_BufferPtr(GpuInput) gpu_input) {
    vec4 eye_pos_h = deref(gpu_input).player.cam.view_to_world * vec4(0, 0, 0, 1);
    return eye_pos_h.xyz / eye_pos_h.w;
}
vec3 get_prev_eye_position(daxa_BufferPtr(GpuInput) gpu_input) {
    vec4 eye_pos_h = deref(gpu_input).player.cam.prev_view_to_prev_world * vec4(0, 0, 0, 1);
    return eye_pos_h.xyz / eye_pos_h.w;
}
vec3 direction_view_to_world(daxa_BufferPtr(GpuInput) gpu_input, vec3 v) {
    return (deref(gpu_input).player.cam.view_to_world * vec4(v, 0)).xyz;
}
vec3 direction_world_to_view(daxa_BufferPtr(GpuInput) gpu_input, vec3 v) {
    return (deref(gpu_input).player.cam.world_to_view * vec4(v, 0)).xyz;
}
vec3 position_world_to_view(daxa_BufferPtr(GpuInput) gpu_input, vec3 v) {
    return (deref(gpu_input).player.cam.world_to_view * vec4(v, 1)).xyz;
}
vec3 position_view_to_world(daxa_BufferPtr(GpuInput) gpu_input, vec3 v) {
    return (deref(gpu_input).player.cam.view_to_world * vec4(v, 1)).xyz;
}

vec3 position_world_to_sample(daxa_BufferPtr(GpuInput) gpu_input, vec3 v) {
    vec4 p = deref(gpu_input).player.cam.world_to_view * vec4(v, 1);
    p = deref(gpu_input).player.cam.view_to_sample * p;
    return p.xyz / p.w;
}

vec3 position_world_to_clip(daxa_BufferPtr(GpuInput) gpu_input, vec3 v) {
    vec4 p = (deref(gpu_input).player.cam.world_to_view * vec4(v, 1));
    p = (deref(gpu_input).player.cam.view_to_clip * p);
    return p.xyz / p.w;
}
