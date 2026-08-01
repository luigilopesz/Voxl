#pragma once

// =============================================================================================
//  THE FAR-FIELD GENERATOR -- filtered terrain for L1
// =============================================================================================
//
// Included from voxels/brushes.glsl, which dispatches here instead of brushgen_voxl_scene when
// the chunk-edit shader is compiled at VOXEL_LEVEL 1. Everything in this file therefore runs
// with VOXEL_SIZE == 0.25 and CHUNK_WORLDSPACE_SIZE == 16, because voxel_malloc.inl switched
// them; `voxel_pos` is still absolute metres and means the same thing it always did.
//
// ---------------------------------------------------------------------------------------------
//  WHY THIS IS THE ONLY GENUINELY NEW ALGORITHM IN THE FAR FIELD
// ---------------------------------------------------------------------------------------------
//
// Everything else about L1 is L0 with a different constant. This is not, and WORLD_SCALE.md
// sec 3.2 is the measurement that says why it is mandatory rather than nice-to-have:
// terminating rays on a coarse block WITHOUT filtered shading data costs 16-20% and paints
// grey slabs across the sky, because a manufactured surface pixel drags a shadow ray, a
// diffuse ray, a reflection ray and the denoiser in behind it. A coarse hit has to be a REAL
// surface -- real colour, real normal, real roughness -- or the whole idea loses money.
//
// ---------------------------------------------------------------------------------------------
//  FILTERING: WHAT "DOMINANT MATERIAL OF THE BLOCK" ACTUALLY HAS TO MEAN
// ---------------------------------------------------------------------------------------------
//
// PERFORMANCE_PLAN.md sec 5.7 item 5 warns that a point sample makes distant hillsides
// "shimmer between rock and grass as the camera moves", and prescribes the dominant material of
// the block. Building it made the mechanism precise, and the prescription is half right:
//
//   THE THING THAT SHIMMERS IS NOT POINT-SAMPLING. IT IS THRESHOLDING.
//
// The far volume is player-centred and wraps, so a given piece of world is re-generated at a
// different sub-voxel phase every time the player crosses a 16 m chunk boundary. Any quantity
// computed by a hard test -- `slope > 0.4 ? rock : grass` -- can land on either side of that
// test at the new phase, and the hillside flips. A *majority vote* over the block is still a
// hard test; it just moves the flip to a different place. What removes the flip is making the
// voxel's appearance a CONTINUOUS function of world position:
//
//   1. the height field is low-passed over the voxel's own footprint (ff_h_filtered), so it
//      carries no detail finer than the voxel that stores it -- the standard anti-alias, and
//      the reason a 25 cm voxel is a fair summary of the sixty-four 6.25 cm voxels under it;
//   2. the normal is a central difference of that low-passed field at the L1 arm, so it is the
//      block's average orientation and not one 6.25 cm facet's;
//   3. materials are MIXED by continuous weights rather than voted on. The area-weighted
//      average of the block is both the physically right answer and the stable one.
//
// Then, and only then, the mix weight is quantised to FF_MAT_STEPS levels -- deliberately, and
// not as a compromise. Continuous colour would make every voxel distinct, every 8^3 palette
// region maximally non-uniform, and the heap would be enormous (brushes.glsl rule 2: a
// bit-identical region allocates nothing at all). Quantising restores large uniform patches.
// It reintroduces a visible step, but the step sits on a FIXED WORLD-SPACE CONTOUR, so it does
// not move with the camera and does not shimmer. Static banding is cheap; flicker is not.

// ---- the shape ------------------------------------------------------------------------------
//
// The far terrain is a ring of ridges outside the island, across open water. That is a content
// choice with a structural reason behind it: see FF_HOLLOW_R in voxels/far_field.inl. The far
// field must not exist anywhere the near box can reach, and a static world-space radius is a
// free way to guarantee it, where carving a sphere around a moving player is not.
//
// It shares the near ground's two rolling octaves verbatim -- same function, same frequencies,
// same offsets -- so it is recognisably the same world's weather and not a different tileset.
#define FF_BASE_Z (-14.0)  // underside of the far landmass, scene-local metres
#define FF_SEA_Z (-2.0)    // notional shoreline; the ridges are measured up from here
#define FF_RIDGE_AMP 42.0  // tallest ridge above FF_SEA_Z
#define FF_MAT_STEPS 8.0   // quantisation of the material mix; see the banner above

// Ridged noise in [0,1], peaking along the zero contour of the underlying field. fbm2 runs
// [0.125, 0.875] about 0.5 (two octaves at persistence 0.5), so the normalisation is /0.375.
float ff_ridge(vec2 q, float inv_scale, float salt) {
    float n = (fbm2(q * inv_scale + salt) - 0.5) * (1.0 / 0.375);
    float r = 1.0 - abs(n);
    return r * r;
}

// Surface height of the far terrain, scene-local metres. Four value-noise pairs.
//
// Three terms, each with a job:
//   rolling  the near ground's own octaves, so the two read as one world;
//   ridge    the mountains -- two scales, 60 m and 26 m, so the range has both a silhouette
//            and a texture at the distance it is actually seen from;
//   rise     a steady climb with distance, and this one is structural rather than aesthetic.
//            L1 is a 256 m cube, so it has a wall at 128 m. If the ground out there were flat,
//            a ray down a valley would leave through that wall and be shaded as sky, and the
//            world would end in a visible straight line. Making the outer ring the tallest
//            thing in it means every near-horizontal ray hits ground first, and the box wall is
//            never seen. The cut is still there; it is simply always behind a mountain.
//   sink     the inverse of rise at the inner edge: the land drops 34 m over the 22 m outside
//            FF_HOLLOW_R, which takes it below FF_BASE_Z and dissolves it into nothing rather
//            than ending it at a cliff.
float ff_h(vec2 q) {
    float d = length(q - VOXL_ISLAND_C);

    float rolling = (fbm2(q * (1.0 / 11.0)) - 0.5) * 1.55 +
                    (fbm2(q * (1.0 / 4.1) + 41.0) - 0.5) * 0.50;

    float ridge = ff_ridge(q, 1.0 / 60.0, 13.0) * 0.78 +
                  ff_ridge(q, 1.0 / 26.0, 77.0) * 0.22;
    float amp = FF_RIDGE_AMP * smoothstep(float(FF_HOLLOW_R), float(FF_HOLLOW_R) + 50.0, d);

    float rise = 20.0 * smoothstep(float(FF_HOLLOW_R) + 18.0, 130.0, d);
    float sink = 34.0 * (1.0 - smoothstep(float(FF_HOLLOW_R), float(FF_HOLLOW_R) + 22.0, d));

    return FF_SEA_Z + rolling + ridge * amp + rise - sink;
}

// ff_h low-passed over one L1 voxel. Four taps on a rotated grid at +/-1/4 voxel: the cheapest
// arrangement that removes the frequencies a 25 cm sample cannot represent, at 4x the noise
// cost of a point sample. Point-sampling instead is a one-line change and it is worth knowing
// what it looks like -- the far ridges crawl by about a voxel whenever the volume re-centres.
float ff_h_filtered(vec2 q) {
    const float E = float(VOXEL_SIZE) * 0.25;
    return 0.25 * (ff_h(q + vec2(+E, +E)) + ff_h(q + vec2(-E, +E)) +
                   ff_h(q + vec2(+E, -E)) + ff_h(q + vec2(-E, -E)));
}

// Normal of the FILTERED field, on a one-voxel arm. Deliberately not the true gradient of ff_h:
// this is the average orientation of the 25 cm block, which is what a 25 cm voxel should be
// shaded with.
vec3 ff_nrm(vec2 q) {
    const float E = float(VOXEL_SIZE);
    float gx = ff_h_filtered(q + vec2(E, 0.0)) - ff_h_filtered(q - vec2(E, 0.0));
    float gy = ff_h_filtered(q + vec2(0.0, E)) - ff_h_filtered(q - vec2(0.0, E));
    return normalize(vec3(-gx, -gy, 2.0 * E));
}

// ---- the generator ---------------------------------------------------------------------------

void brushgen_far_field(in out Voxel voxel) {
    vec3 s = voxel_pos - VOXL_ORIGIN;

    // Cheap rejects, and they carry most of the generation cost. The far volume is a 256 m cube
    // and the terrain occupies a 76 m band of it, so ~70% of invocations leave here having
    // evaluated no noise at all. The distance reject is the hollowness guarantee as well as an
    // optimisation -- nothing this function can write is inside the near box.
    if (s.z > FF_SEA_Z + FF_RIDGE_AMP + 22.0 || s.z < FF_BASE_Z - 1.0) {
        return;
    }
    float d = length(s.xy - VOXL_ISLAND_C);
    if (d < float(FF_HOLLOW_R)) {
        return;
    }

    float h = ff_h_filtered(s.xy);
    if (s.z >= h || s.z <= FF_BASE_Z) {
        return;
    }

    voxel.material_type = 1;
    float depth = h - s.z;

    // INTERIORS ARE BIT-IDENTICAL, which is brushes.glsl rule 2 and it is worth as much here as
    // it is in the near field: a palette region whose 512 voxels are equal stores its single
    // value inline in the header and takes no heap page at all. One metre of skin at 25 cm is
    // four voxels, which is all the depth anything can see into a mountain at 90 m.
    if (depth > 1.0) {
        voxel.normal = vec3(0.0, 0.0, 1.0);
        voxel.roughness = 0.9;
        voxel.color = voxl_col(vec3(58.0, 54.0, 48.0), 1.0);
        return;
    }

    vec3 nrm = ff_nrm(s.xy);
    voxel.normal = nrm;

    // ---- the filtered material ---------------------------------------------------------------
    //
    // Three continuous weights, mixed, then quantised to FF_MAT_STEPS. No branch chooses a
    // material anywhere in here; see the banner at the top of the file for why that is the
    // whole point.
    float upwards = nrm.z;

    // Steep ground is bare rock. The slope is read off the filtered normal, so it is the
    // block's slope and not a facet's.
    float rock_f = 1.0 - smoothstep(0.55, 0.86, upwards);
    // High ground is pale -- the thing that actually makes a ridgeline legible at 100 m is the
    // tone break along the tops, not the geometry.
    float high_f = smoothstep(16.0, 30.0, s.z) * (0.35 + 0.65 * upwards);

    // Quantise. floor(x * N + 0.5) / N: the steps land on fixed world-space contours because
    // both inputs are pure functions of world position.
    rock_f = floor(clamp(rock_f, 0.0, 1.0) * FF_MAT_STEPS + 0.5) * (1.0 / FF_MAT_STEPS);
    high_f = floor(clamp(high_f, 0.0, 1.0) * FF_MAT_STEPS + 0.5) * (1.0 / FF_MAT_STEPS);

    // The three end members. Chosen to sit in the same family as the near island's palette --
    // voxl_col() applies the same pre-gamma darkening the rest of the scene is authored with --
    // so the far range does not read as a different game's asset.
    vec3 grass = voxl_col(vec3(74.0, 92.0, 52.0), 0.62);
    vec3 rock = voxl_col(vec3(96.0, 90.0, 82.0), 0.55);
    vec3 pale = voxl_col(vec3(150.0, 152.0, 158.0), 0.72);

    vec3 col = mix(grass, rock, rock_f);
    col = mix(col, pale, high_f);

    voxel.color = col;
    voxel.roughness = mix(0.95, 0.82, rock_f);
}
