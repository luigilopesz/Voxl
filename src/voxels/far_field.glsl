#pragma once

// =============================================================================================
//  THE FAR-FIELD MARCH SEGMENT
// =============================================================================================
//
// Included by voxels/impl/trace.glsl, after voxels/impl/voxels.glsl. Everything here runs in
// the same shader invocation as the near-field march, which is the reason this file exists
// rather than a second VOXEL_LEVEL compile: one ray crosses both levels, so both levels'
// constants have to be live at once.
//
// ---------------------------------------------------------------------------------------------
//  THE HAND-OFF, WHICH IS THE WHOLE TECHNICAL PROBLEM
// ---------------------------------------------------------------------------------------------
//
// A ray must cross the near field at 6.25 cm and then continue into the far field at 25 cm with
// no seam, no double-hit and no gap. There are three ways to get that wrong and each has a
// specific answer here.
//
// (1) A GAP -- the ray skips a slab of world between the two volumes.
//     Answer: THE TWO SEGMENTS SHARE ONE RAY PARAMETER t. Both volumes are the same ray in a
//     *translated* frame -- L0 adds `(offset & 3) + 32`, L1 adds `(offset & 15) + 128` -- and a
//     translation does not change distance along a ray. So the far segment starts at exactly
//     the t at which the near segment left its box, not at a recomputed intersection, and
//     `t_begin` is passed through rather than re-derived. There is nothing between them to
//     skip. It is also not possible for the ray to leave L0 into empty space: L0 spans at most
//     +/-35 m about the player and L1 spans at least +/-113 m, so L0 is strictly inside L1.
//
// (2) A DOUBLE-HIT -- the ray hits the coarse copy of geometry the near field already showed
//     it in detail. WORLD_SCALE.md sec 7.3 calls this fatal.
//     Answer, first line: THE FAR SEGMENT IS ONLY EVER ENTERED AT THE POINT WHERE THE RAY LEAVES
//     THE NEAR BOX. Nothing inside that box is sampled at L1 by construction -- not "because
//     the far field is empty there", but because the march never looks. That is stronger than
//     hollowing and it costs nothing.
//     Answer, second line: at the boundary itself a 25 cm voxel can bulge up to 25 cm past the
//     6.25 cm surface it stands for, so a grazing ray that L0 correctly called a miss could hit
//     that bulge one voxel after the hand-off and paint a wall at 32 m. FF_HOLLOW_R
//     (far_field.inl) keeps the far terrain 72 m away from anywhere the near box can reach, so
//     the first 37 m of every far segment is guaranteed empty.
//
// (3) A SEAM -- a visible discontinuity in shading where one volume ends and the other begins.
//     Answer: there is no shading change at all. The far segment produces a PackedVoxel from
//     the same palette, with a real colour, a real normal and a real roughness, and hands it
//     back in the same VoxelTraceResult. Every consumer downstream -- the g-buffer, the shadow
//     gate on `depth != 0.0`, ReSTIR, the denoisers -- cannot tell which level answered. That
//     is exactly the property WORLD_SCALE.md sec 3.2 measured the absence of: terminating on
//     occupancy bits manufactures a surface with no shading data and costs 16-20%; returning a
//     filtered voxel does not.
//
// ---------------------------------------------------------------------------------------------
//  WHAT IS DELIBERATELY NOT DONE
// ---------------------------------------------------------------------------------------------
//
// The far segment does NOT apply the depth prepass's cone-LOD test. The prepass seeds the full
// trace's starting distance, so a prepass hit that is too NEAR only wastes a few steps, while
// one that is too FAR deletes geometry. `lod == 0` in the far segment is exact and can only err
// near. Cheap insurance against the failure mode that does not announce itself.

// ---- indexing --------------------------------------------------------------------------------

// The far level's copy of calc_chunk_index (voxels/impl/voxels.glsl:94), character for
// character apart from the shift and the axis count. Kept identical on purpose rather than
// "improved": it leans on `%` over a signed vector being a floored modulo, which glslang gives
// and the GLSL spec does not promise, and the near field has leaned on that since upstream. If
// it is ever wrong it must be wrong for both levels at once, or the bug is unfindable.
// ONE `offset` SERVES BOTH LEVELS. It is the player's integer position in metres, written from
// gpu_input.player.player_unit_offset into each level's globals every frame -- the same number
// in both. What differs is the shift applied to it: the near level divides by a 4 m chunk and
// the far level by a 16 m one. So the far march reads the NEAR globals and shifts differently,
// and the far globals never have to reach a trace shader at all.
uint ff_calc_chunk_index(daxa_BufferPtr(VoxelWorldGlobals) globals, uvec3 chunk_i) {
#if ENABLE_CHUNK_WRAPPING
    chunk_i = uvec3((ivec3(chunk_i) + (deref(globals).offset >> ivec3(FF_LOG2_CHUNK_WORLDSPACE_SIZE))) % ivec3(FF_CHUNKS_PER_AXIS));
#endif
    return chunk_i.x + chunk_i.y * FF_CHUNKS_PER_AXIS + chunk_i.z * FF_CHUNKS_PER_AXIS * FF_CHUNKS_PER_AXIS;
}

// The far level's sample. THE INNER sample_lod() IS REUSED VERBATIM -- it takes chunk and
// in-chunk voxel indices, not metres, so it is already independent of voxel size. Only the
// wrapper that turns a position into those indices has to know which level it is on. That is
// the entire "thread voxel size as a per-volume value" job from WORLD_SCALE.md sec 7 item 1, for
// the trace: one wrapper, not 82 substitutions.
uint ff_sample_lod(in VoxelBufferPtrs ptrs, daxa_BufferPtr(VoxelLeafChunk) ff_chunks, vec3 p_local, out PackedVoxel voxel_data) {
    uvec3 voxel_i = uvec3(p_local * FF_VOXEL_SCL);
    uvec3 chunk_i = voxel_i / CHUNK_SIZE;
    uint chunk_index = ff_calc_chunk_index(ptrs.globals, chunk_i);
    return sample_lod(ptrs.allocator, advance(ff_chunks, chunk_index), chunk_i,
                      voxel_i - chunk_i * CHUNK_SIZE, voxel_data);
}

// ---- the segment -----------------------------------------------------------------------------
//
//   base_ws   in : the point t is measured from -- the near box's entry point, or the ray
//                  origin for a ray that never entered the near box. This is the same
//                  convention voxel_trace() already uses for result.dist, preserved so that a
//                  far-field miss leaves every existing number bit-identical.
//            out : on a hit, the hit position. Untouched on a miss.
//   t_begin       the ray parameter at which the near segment gave up, measured from base_ws.
//   result        voxel_data / nrm / dist / step_n are written on a hit; step_n accumulates
//                 either way so the step heatmap counts the whole ray.
//
//   returns       true if the far field produced a surface.
bool ff_trace_far(in VoxelTraceInfo info, in out vec3 base_ws, float t_begin, in out VoxelTraceResult result) {
    // The far chunk table, published into the near globals once a frame by
    // far_field_perframe.comp.glsl. Zero until that pass has run for the first time, which is
    // the one frame between the startup graph's clear and the first frame graph -- checked
    // rather than assumed, because dereferencing address 0 is a device loss, not a black pixel.
    daxa_u64 ff_addr = deref(info.ptrs.globals).ff_voxel_chunks_addr;
    if (ff_addr == 0) {
        return false;
    }
    daxa_BufferPtr(VoxelLeafChunk) ff_chunks = daxa_BufferPtr(VoxelLeafChunk)(ff_addr);

    vec3 offset = vec3((deref(info.ptrs.globals).offset) & ((1 << FF_LOG2_CHUNK_WORLDSPACE_SIZE) - 1)) +
                  vec3(FF_CHUNKS_PER_AXIS) * FF_CHUNK_WORLDSPACE_SIZE * 0.5;

    BoundingBox b;
    b.bound_min = vec3(0.0);
    b.bound_max = vec3(FF_CHUNKS_PER_AXIS) * FF_CHUNK_WORLDSPACE_SIZE;

    // How far this ray is allowed to see. Primary visibility always gets the whole volume; every
    // other caller can be clamped, and FF_SECONDARY_MAX_DIST_M in far_field.inl says why.
    //
    // The clamp is applied to a LOCAL limit and never to info.max_dist, for the reason
    // WORLD_SCALE.md sec 0.2 records: result.dist is seeded from info.max_dist and callers test
    // `dist == MAX_DIST` to mean "missed, shade as sky", so lowering it would turn every clipped
    // ray into a bogus HIT at the clip plane and drag a full GI shade in behind it.
    float march_limit = info.max_dist;
#if FF_SECONDARY_MAX_DIST_M > 0 && !(TracePrimaryComputeShader || TraceDepthPrepassComputeShader)
    march_limit = min(march_limit, t_begin + float(FF_SECONDARY_MAX_DIST_M));
#endif

    if (t_begin > march_limit) {
        return false;
    }

    // Enter the far volume. For a ray handed over from the near field this is a no-op --
    // intersect() returns immediately when already inside, and L0 is strictly inside L1 -- so
    // `t_enter` is 0 and the hand-off is exact. It does real work only for a ray that never
    // entered the near box at all, which is an irradiance-cache probe or a bounce ray more than
    // 32 m from the player.
    vec3 p = base_ws + info.ray_dir * t_begin + offset;
    vec3 p_before = p;
    intersect(p, info.ray_dir, vec3(1) / info.ray_dir, b);
    float t_enter = dot(p - p_before, info.ray_dir);

    p += info.ray_dir * 0.01 * FF_VOXEL_SIZE;
    if (!inside(p, b)) {
        return false;
    }

    // t of `p` itself, in the caller's parameterisation. Everything below adds to this.
    float t_base = t_begin + t_enter;
    if (t_base > march_limit) {
        return false;
    }

    vec3 delta = vec3(
        info.ray_dir.x == 0 ? 3.0 * FF_MAX_STEPS : abs(1.0 / info.ray_dir.x),
        info.ray_dir.y == 0 ? 3.0 * FF_MAX_STEPS : abs(1.0 / info.ray_dir.y),
        info.ray_dir.z == 0 ? 3.0 * FF_MAX_STEPS : abs(1.0 / info.ray_dir.z));

    PackedVoxel voxel_data = result.voxel_data;
    uint lod = ff_sample_lod(info.ptrs, ff_chunks, p, voxel_data);
    if (lod == 0) {
        // SOLID AT THE HAND-OFF POINT. The comment here used to say this was "only reachable if
        // FF_HOLLOW_R is set too small for the scene". That was wrong, and it was the single
        // biggest visual defect in the merged engine: it is reached by EVERY secondary ray cast
        // from a far-field surface, because such a ray's origin is outside the near box, so
        // voxel_trace() arrives here with t_begin 0 and `p` is the shading point itself.
        //
        // For a secondary ray that is never a real occlusion. An occluder at distance zero means
        // the origin is inside geometry, which is a statement about the bias, not about the
        // world -- and trace_secondary.comp.glsl reads `hit = (dist == MAX_DIST)`, so it lands as
        // "fully shadowed": the chunk-sized black wedges across the distant mountains.
        // camera.glsl's origin bias is now sized to FF_VOXEL_SIZE and retreats along the view
        // ray, which removes most of them; this removes the rest, and removes them for any
        // future caller whose bias is imperfect rather than only for today's two.
        //
        // Primary visibility keeps the old behaviour, where "solid at the entry point" is a
        // legitimate answer -- it means the camera is inside a mountain and the pixel is rock.
        // Same guard as the FF_SECONDARY_MAX_DIST_M clamp above.
#if !(TracePrimaryComputeShader || TraceDepthPrepassComputeShader)
        return false;
#else
        // The face normal is unknown here, so the packed voxel's own normal is used.
        result.voxel_data = voxel_data;
        result.dist = t_base;
        result.nrm = unpack_voxel(voxel_data).normal;
        base_ws = base_ws + info.ray_dir * t_base;
        return true;
#endif
    }

    float cell_size = float(1l << (lod - 1)) * FF_VOXEL_SIZE;
    vec3 t_start;
    if (info.ray_dir.x < 0) {
        t_start.x = (p.x / cell_size - floor(p.x / cell_size)) * cell_size * delta.x;
    } else {
        t_start.x = (ceil(p.x / cell_size) - p.x / cell_size) * cell_size * delta.x;
    }
    if (info.ray_dir.y < 0) {
        t_start.y = (p.y / cell_size - floor(p.y / cell_size)) * cell_size * delta.y;
    } else {
        t_start.y = (ceil(p.y / cell_size) - p.y / cell_size) * cell_size * delta.y;
    }
    if (info.ray_dir.z < 0) {
        t_start.z = (p.z / cell_size - floor(p.z / cell_size)) * cell_size * delta.z;
    } else {
        t_start.z = (ceil(p.z / cell_size) - p.z / cell_size) * cell_size * delta.z;
    }

    float t_curr = min(min(t_start.x, t_start.y), t_start.z);
    vec3 t_next = t_start;
    bool hit_surface = false;

    // A SEPARATE STEP BUDGET, and it matters. MAX_STEPS is 512 and the near field's minimum
    // step is 6.25 cm, so a ray that crawled through grass can spend the entire budget inside
    // 32 m. If the far field drew from the same pool, the mountain behind that grass would
    // vanish -- and it would vanish as *sky*, which is indistinguishable from correct.
    // 256 far steps is 64 m at the 25 cm minimum and 4 km at the 16 m chunk stride.
    uint step_i = 0;
    for (; step_i < FF_MAX_STEPS; ++step_i) {
        vec3 current_pos = p + info.ray_dir * t_curr;
        if (t_base + t_curr > march_limit) {
            break;
        }
        if (!inside(current_pos + info.ray_dir * 0.001, b)) {
            break;
        }
        lod = ff_sample_lod(info.ptrs, ff_chunks, current_pos, voxel_data);
        hit_surface = lod == 0;
        if (hit_surface) {
            result.nrm = sign(info.ray_dir) * (sign(t_next - min(min(t_next.x, t_next.y), t_next.z).xxx) - 1);
            break;
        }
        cell_size = float(1l << (lod - 1)) * FF_VOXEL_SIZE;
        t_next = (0.5 + sign(info.ray_dir) * (0.5 - fract(current_pos / cell_size))) * cell_size * delta;
        // Same fp-imprecision hack as the near march, scaled to this level's voxel.
        t_next += 0.0001 * (sign(info.ray_dir) * -0.5 + 0.5);
        t_curr += (min(min(t_next.x, t_next.y), t_next.z) + 0.0003 * FF_VOXEL_SIZE);
    }
    result.step_n += step_i;

    if (!hit_surface) {
        return false;
    }

    result.voxel_data = voxel_data;
    result.dist = t_base + t_curr;
    base_ws = base_ws + info.ray_dir * result.dist;

#if PER_VOXEL_NORMALS
    result.nrm = unpack_voxel(voxel_data).normal;
#endif
    return true;
}
