#pragma once

#include <utilities/gpu/math.glsl>
#include <voxels/impl/voxels.glsl>
#include <voxels/far_field.glsl>

// --- The far field ---------------------------------------------------------------------------
//
// VOXL_FF_TRACE decides whether THIS shader continues a missed ray into the L1 volume. See
// voxels/far_field.inl for the two switches behind it and docs/FAR_FIELD.md for what each
// setting measured. FF_ALL_RAYS 0 restricts the far field to primary visibility, which is the
// cheap answer; 1 gives it to the sun shadow, the ReSTIR diffuse and reflection rays and the
// irradiance cache as well, which is the correct one.
#if FF_ENABLE && FF_RAYS == 2
#define VOXL_FF_TRACE 1
#elif FF_ENABLE && FF_RAYS == 1 && (TracePrimaryComputeShader || TraceDepthPrepassComputeShader)
#define VOXL_FF_TRACE 1
#else
#define VOXL_FF_TRACE 0
#endif

// --- Ray reach controls -------------------------------------------------------------------
//
// These are integers on purpose. The GLSL preprocessor's `#if` is integer-only, a float
// literal there is a compile error, and daxa runs with
// `register_null_pipelines_when_first_compile_fails`, so the mistake presents as a missing
// pass (a silently *cheaper* frame) rather than as a diagnostic. Fixed-point x1e4 instead.
//
// This file is runtime-compiled: editing it and relaunching is enough, no C++ rebuild.
//
// VOXL_MAX_STEPS
//   0  = honour the caller's info.max_steps (which every call site sets to MAX_STEPS, 512,
//        from utilities/gpu/math.glsl -- a file this agent does not own).
//   >0 = override it here for every caller. The DDA's minimum step is one voxel, 6.25 cm, so
//        512 steps carries a ray 32 m through fine geometry against a 111 m box diagonal at
//        CHUNKS_PER_AXIS 16. A ray that exhausts the budget reports dist == max_dist, which
//        every caller reads as "missed, shade as sky" -- so the symptom of a binding cap is
//        HOLES IN DISTANT GEOMETRY, not a slowdown. Measure with the step heatmap
//        (VOXL_DEBUG_PASS='voxel step count') before assuming either way.
#if !defined(VOXL_MAX_STEPS)
#define VOXL_MAX_STEPS 0
#endif
//
// VOXL_STEP_FLOOR_E4
//   Distance-proportional floor on the *step size*, x1e4. 0 = off (exact march).
//   At parameter K the marcher refuses to advance in cells smaller than `t * K` metres,
//   rounded up to the next uniformity level. This is NOT the cone-LOD termination measured
//   in docs/design/WORLD_SCALE.md sec 3.2, which cost 16-20%: that changed the *hit test*
//   (`hit_surface = lod < threshold`) and so manufactured phantom surfaces out of sky, and a
//   surface pixel on this renderer drags a shadow ray, a diffuse ray, a reflection ray and
//   the denoiser in behind it. This changes only the *stride*; the hit test stays
//   `lod == 0`, so a hit is still a point that is genuinely solid. Its failure mode is the
//   opposite one -- stepping over thin distant geometry, i.e. holes, not phantom slabs.
#if !defined(VOXL_STEP_FLOOR_E4)
#define VOXL_STEP_FLOOR_E4 0
#endif

// What budget a call site's `max_steps` argument will actually be given. Callers that want to
// know whether a trace ran out of budget must compare against this, not against their own
// argument, or the override above silently makes the test wrong.
#if VOXL_MAX_STEPS > 0
#define VOXL_TRACE_MAX_STEPS(caller_max_steps) uint(VOXL_MAX_STEPS)
#else
#define VOXL_TRACE_MAX_STEPS(caller_max_steps) uint(caller_max_steps)
#endif

VoxelTraceResult voxel_trace(in VoxelTraceInfo info, in out vec3 ray_pos) {

    VoxelTraceResult result;
    result.dist = info.max_dist;
    // Only `dist` and `vel` used to be initialised here, and two of this function's exits return
    // before the march loop assigns the rest. Reading an undefined value is what the step
    // heatmap caught first -- it reported step counts of ~730 against a 512 budget, on 0.13% of
    // the pixels of the default vista -- but `nrm` matters far more: the `lod == 0` exit below
    // returns dist 0.0, which every caller reads as a HIT, and then shades it with whatever was
    // in the register. Those pixels are the black triangular patches on the rock dome recorded
    // as defect #1 in docs/SCENE.md; the smaller of the two disappears with this line.
    // -ray_dir is the right default for that exit specifically: the trace started inside solid,
    // so the only face the viewer could see is the one pointing back along the ray.
    result.step_n = 0;
    result.nrm = normalize(-info.ray_dir);
    result.voxel_data = PackedVoxel(0);

    result.vel = vec3(deref(info.ptrs.globals).offset - deref(info.ptrs.globals).prev_offset);

    uvec3 chunk_n = uvec3(CHUNKS_PER_AXIS);

#if VOXL_FF_TRACE
    // Captured before the near box's `+= offset` and `intersect()` mangle it. Needed only by
    // the path where the ray misses the near box entirely and the far field is its first
    // volume; every other path hands over from a point the near march already computed.
    vec3 ff_ray_origin = ray_pos;
#endif

    {
        daxa_BufferPtr(VoxelLeafChunk) voxel_chunks_ptr = info.ptrs.voxel_chunks_ptr;
        vec3 offset = vec3((deref(info.ptrs.globals).offset) & ((1 << (6 + LOG2_VOXEL_SIZE)) - 1)) + vec3(chunk_n) * CHUNK_WORLDSPACE_SIZE * 0.5;
        ray_pos += offset;
        BoundingBox b;
        b.bound_min = vec3(0.0);
        b.bound_max = b.bound_min + vec3(chunk_n) * CHUNK_WORLDSPACE_SIZE;

        // if (ENABLE_CHUNK_WRAPPING == 0) {
        intersect(ray_pos, info.ray_dir, vec3(1) / info.ray_dir, b);
        // }

        ray_pos += info.ray_dir * 0.01 * VOXEL_SIZE;

        if (false) {
            result.dist = 0.0;
            if (!inside(ray_pos, b)) {
                if (info.extend_to_max_dist) {
                    result.dist = info.max_dist;
                }
            } else {
                PackedVoxel dontcare;
                uint lod = sample_lod(info.ptrs.globals, info.ptrs.allocator, voxel_chunks_ptr, chunk_n, ray_pos, dontcare);
                Voxel voxel = Voxel(0, 0, vec3(0), vec3(0));
                voxel.color = vec3(0);
                switch (lod) {
                case 0: voxel.color = vec3(0.0, 0.0, 0.0); break;
                case 1: voxel.color = vec3(1.0, 0.0, 0.0); break;
                case 2: voxel.color = vec3(0.0, 1.0, 0.0); break;
                case 3: voxel.color = vec3(1.0, 1.0, 0.0); break;
                case 4: voxel.color = vec3(0.0, 0.0, 1.0); break;
                case 5: voxel.color = vec3(1.0, 0.0, 1.0); break;
                case 6: voxel.color = vec3(0.0, 1.0, 1.0); break;
                case 7: voxel.color = vec3(1.0, 1.0, 1.0); break;
                }
                result.voxel_data = pack_voxel(voxel);
                result.nrm = vec3(0, 0, 1);
            }
            ray_pos -= offset;
            return result;
        }

        if (!inside(ray_pos, b)) {
            ray_pos -= offset;
#if VOXL_FF_TRACE
            // The ray never entered the near volume. That is not a miss yet -- the far volume
            // is four times wider and still ahead of it. Irradiance-cache probes and bounce
            // rays more than 32 m from the player arrive here.
            {
                vec3 ff_pos = ff_ray_origin;
                if (ff_trace_far(info, ff_pos, 0.0, result)) {
                    ray_pos = ff_pos;
                    return result;
                }
            }
#endif
            if (info.extend_to_max_dist) {
                result.dist = info.max_dist;
            } else {
                result.dist = 0.0;
            }
            return result;
        }

        vec3 delta = vec3(
            info.ray_dir.x == 0 ? 3.0 * info.max_steps : abs(1.0 / info.ray_dir.x),
            info.ray_dir.y == 0 ? 3.0 * info.max_steps : abs(1.0 / info.ray_dir.y),
            info.ray_dir.z == 0 ? 3.0 * info.max_steps : abs(1.0 / info.ray_dir.z));
        uint lod = sample_lod(info.ptrs.globals, info.ptrs.allocator, voxel_chunks_ptr, chunk_n, ray_pos, result.voxel_data);
        if (lod == 0) {
            result.dist = 0.0;
            ray_pos -= offset;
            return result;
        }
        float cell_size = float(1l << (lod - 1)) * VOXEL_SIZE;
        vec3 t_start;
        if (info.ray_dir.x < 0) {
            t_start.x = (ray_pos.x / cell_size - floor(ray_pos.x / cell_size)) * cell_size * delta.x;
        } else {
            t_start.x = (ceil(ray_pos.x / cell_size) - ray_pos.x / cell_size) * cell_size * delta.x;
        }
        if (info.ray_dir.y < 0) {
            t_start.y = (ray_pos.y / cell_size - floor(ray_pos.y / cell_size)) * cell_size * delta.y;
        } else {
            t_start.y = (ceil(ray_pos.y / cell_size) - ray_pos.y / cell_size) * cell_size * delta.y;
        }
        if (info.ray_dir.z < 0) {
            t_start.z = (ray_pos.z / cell_size - floor(ray_pos.z / cell_size)) * cell_size * delta.z;
        } else {
            t_start.z = (ceil(ray_pos.z / cell_size) - ray_pos.z / cell_size) * cell_size * delta.z;
        }
        float t_curr = min(min(t_start.x, t_start.y), t_start.z);
        vec3 current_pos = ray_pos;
        vec3 t_next = t_start;
        bool hit_surface = false;
#if VOXL_MAX_STEPS > 0
        uint max_steps = uint(VOXL_MAX_STEPS);
#else
        uint max_steps = info.max_steps;
#endif
        // The loop counter lives in the result so callers can tell a ray that ran out of
        // budget (step_n == max_steps, dist == max_dist) from one that genuinely left the
        // world box (step_n < max_steps, dist == max_dist). Both are reported as a miss.
        // Split out of the compound break below so the far field can tell WHY the near march
        // stopped. Only "left the box" earns a continuation: a ray that spent its whole step
        // budget was crawling through fine geometry and should have hit, and one that reached
        // max_dist has no reach left to spend.
        bool left_box = false;
        for (result.step_n = 0; result.step_n < max_steps; ++result.step_n) {
            current_pos = ray_pos + info.ray_dir * t_curr;
            if (t_curr > info.max_dist) {
                break;
            }
            if (!inside(current_pos + info.ray_dir * 0.001, b)) {
                left_box = true;
                break;
            }
            lod = sample_lod(info.ptrs.globals, info.ptrs.allocator, voxel_chunks_ptr, chunk_n, current_pos, result.voxel_data);
#if TraceDepthPrepassComputeShader
            hit_surface = lod < clamp(sqrt(t_curr * info.angular_coverage * VOXEL_SCL), 1, 7);
#else
            hit_surface = lod == 0;
#endif
            if (hit_surface) {
                result.nrm = sign(info.ray_dir) * (sign(t_next - min(min(t_next.x, t_next.y), t_next.z).xxx) - 1);
                result.dist = t_curr;
                break;
            }
            uint step_lod = lod;
#if VOXL_STEP_FLOOR_E4 > 0
            // Smallest cell we are willing to stride in, in voxels, growing linearly with t.
            float floor_voxels = t_curr * (float(VOXL_STEP_FLOOR_E4) * 0.0001) * VOXEL_SCL;
            uint floor_lod = uint(clamp(ceil(log2(max(floor_voxels, 1.0))) + 1.0, 1.0, 7.0));
            step_lod = max(step_lod, floor_lod);
#endif
            cell_size = float(1l << (step_lod - 1)) * VOXEL_SIZE;
            t_next = (0.5 + sign(info.ray_dir) * (0.5 - fract(current_pos / cell_size))) * cell_size * delta;
            // HACK to mitigate fp imprecision...
            t_next += 0.0001 * (sign(info.ray_dir) * -0.5 + 0.5);
            // t_curr += (min(min(t_next.x, t_next.y), t_next.z));
            t_curr += (min(min(t_next.x, t_next.y), t_next.z) + 0.0003 * VOXEL_SIZE);
        }
        ray_pos -= offset;

        if (hit_surface) {

            if (info.extend_to_max_dist) {
                t_curr = result.dist;
            }
            ray_pos = ray_pos + info.ray_dir * t_curr;
            result.dist = t_curr;

#if PER_VOXEL_NORMALS
            Voxel voxel = unpack_voxel(result.voxel_data);

            // vec3 del = info.ray_dir;
            // if (dot(voxel.normal, del) > -1.0 && dot(result.nrm, voxel.normal) < 0.0) {
            //     voxel.normal *= -1;
            // }

            result.nrm = voxel.normal;
#endif
        }
#if VOXL_FF_TRACE
        else if (left_box) {
            // ray_pos is the point t is measured from -- the near box's entry, which is where
            // `-= offset` above put it. t_curr is where the ray crossed out. Hand both over
            // unchanged; see the header of voxels/far_field.glsl for why sharing the parameter
            // rather than re-intersecting is what makes the seam impossible.
            vec3 ff_pos = ray_pos;
            if (ff_trace_far(info, ff_pos, t_curr, result)) {
                ray_pos = ff_pos;
            }
            // The near segment did NOT exhaust its budget -- it left the box, which is why we
            // are here. Callers read `step_n >= VOXL_TRACE_MAX_STEPS(...)` as "ran out of steps,
            // this pixel is a hole" (renderer/trace_primary.comp.glsl:123), so the far field's
            // steps must not be allowed to push the total across that line and fake one.
            result.step_n = min(result.step_n, max_steps - 1);
        }
#endif
    }

    return result;
}
