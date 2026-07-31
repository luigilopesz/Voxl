#pragma once

#include <utilities/gpu/random.glsl>
#include <utilities/gpu/noise.glsl>

#include <g_samplers>
#include <g_value_noise>

bool mandelbulb(in vec3 c, in out vec3 color) {
    vec3 z = c;
    uint i = 0;
    const float n = 8 + floor(good_rand(brush_input.pos) * 5);
    const uint MAX_ITER = 4;
    float m = dot(z, z);
    vec4 trap = vec4(abs(z), m);
    for (; i < MAX_ITER; ++i) {
        float r = length(z);
        float p = atan(z.y / z.x);
        float t = acos(z.z / r);
        z = vec3(
            sin(n * t) * cos(n * p),
            sin(n * t) * sin(n * p),
            cos(n * t));
        z = z * pow(r, n) + c;
        trap = min(trap, vec4(abs(z), m));
        m = dot(z, z);
        if (m > 256.0)
            break;
    }
    color = vec3(m, trap.yz) * trap.w;
    return i == MAX_ITER;
}

vec4 terrain_noise(vec3 p) {
    FractalNoiseConfig noise_conf = FractalNoiseConfig(
        /* .amplitude   = */ 1.0,
        /* .persistance = */ 0.2,
        /* .scale       = */ 0.005,
        /* .lacunarity  = */ 4.5,
        /* .octaves     = */ 6);
    vec4 val = fractal_noise(g_value_noise_tex, g_sampler_llr, p, noise_conf);
    // const float ground_level = 6362000.0;
    const float ground_level = 0.0;
    val.x += (p.z - ground_level + 100.0) * 0.003 - 0.4;
    val.yzw = normalize(val.yzw + vec3(0, 0, 0.003));
    // val.x += -0.24;
    return val;
}

struct TreeSDF {
    float wood;
    float leaves;
};

struct TreeSDFNrm {
    float wood;
    float leaves;
    vec3 wood_nrm;
    vec3 leaves_nrm;
};

void sd_spruce_branch(in out TreeSDF val, in vec3 p, in vec3 origin, in vec3 dir, in float scl) {
    vec3 bp0 = origin;
    vec3 bp1 = bp0 + dir;
    val.wood = min(val.wood, sd_capsule(p, bp0, bp1, 0.10));
    val.leaves = min(val.leaves, sd_sphere(p - bp1, 0.15 * scl));
    bp0 = bp1, bp1 = bp0 + dir * 0.5 + vec3(0, 0, 0.2);
    val.wood = min(val.wood, sd_capsule(p, bp0, bp1, 0.07));
    val.leaves = min(val.leaves, sd_sphere(p - bp1, 0.15 * scl));
}

TreeSDF sd_spruce_tree(in vec3 p, in vec3 seed) {
    TreeSDF val = TreeSDF(1e5, 1e5);
    val.wood = min(val.wood, sd_capsule(p, vec3(0, 0, 0), vec3(0, 0, 4.5), 0.15));
    val.leaves = min(val.leaves, sd_capsule(p, vec3(0, 0, 4.5), vec3(0, 0, 5.0), 0.15));
    for (uint i = 0; i < 5; ++i) {
        float scl = 1.0 / (1.0 + i * 0.5);
        float scl2 = 1.0 / (1.0 + i * 0.1);
        uint branch_n = 8 - i;
        for (uint branch_i = 0; branch_i < branch_n; ++branch_i) {
            float angle = (1.0 / branch_n * branch_i) * 2.0 * M_PI + good_rand(seed + i + 1.0 * branch_i) * 0.5;
            sd_spruce_branch(val, p, vec3(0, 0, 1.0 + i * 0.8) * 1.0, normalize(vec3(cos(angle), sin(angle), +0.0)) * scl, scl2 * 1.5);
        }
    }
    return val;
}

// Forest generation
#define TREE_MARCH_STEPS 4

vec3 get_closest_surface(vec3 center_cell_world, float current_noise, float rep, inout float scale) {
    vec3 offset = hash33(center_cell_world);
    scale = offset.z * .3 + .7;
    center_cell_world.xy += (offset.xy * 2 - 1) * max(0, rep / scale - 5);

    float step_size = rep / 2 / TREE_MARCH_STEPS;

    // Above terrain
    if (current_noise > 0) {
        for (uint i = 0; i < TREE_MARCH_STEPS; i++) {
            center_cell_world.z -= step_size;
            if (terrain_noise(center_cell_world).x < 0)
                return center_cell_world;
        }
    }
    // Inside terrain
    else {
        for (uint i = 0; i < TREE_MARCH_STEPS; i++) {
            center_cell_world.z += step_size;
            if (terrain_noise(center_cell_world).x > 0)
                return center_cell_world - vec3(0, 0, step_size);
        }
    }

    return vec3(0);
}

void try_spawn_tree(in out Voxel voxel, vec3 forest_biome_color, vec3 nrm) {
    float upwards = dot(nrm, vec3(0, 0, 1));

    // Meters per cell
    float rep = 6;

    // Global cell ID
    vec3 qid = floor(voxel_pos / rep);
    // Local coordinates in current cell (centered at 0 [-rep/2, rep/2])
    vec3 q = mod(voxel_pos, rep) - rep / 2;
    // Current cell's center voxel (world space)
    vec3 cell_center_world = qid * rep + rep / 2.;

    // Query terrain noise at current cell's center
    vec4 center_noise = terrain_noise(cell_center_world);

    // Optimization: only run for chunks near enough the terrain surface
    bool can_spawn = center_noise.x >= -0.01 * rep / 4 && center_noise.x < 0.03 * rep / 4;

    // Forest density
    float forest_noise = fbm2(qid.xy / 10.);
    float forest_density = .45;

    if (forest_noise > forest_density)
        can_spawn = false;

    if (can_spawn) {
        // Tree scale
        float scale;
        // Try to get the nearest point on the surface below (in the starting cell)
        vec3 hitPoint = get_closest_surface(cell_center_world, center_noise.x, rep, scale);

        if (hitPoint == vec3(0) && center_noise.x > 0) {
            // If no terrain was found, try again for the bottom cell (upper tree case)
            scale = forest_noise;
            vec3 down_neighbor_cell_center_world = cell_center_world - vec3(0, 0, rep);
            hitPoint = get_closest_surface(down_neighbor_cell_center_world, terrain_noise(down_neighbor_cell_center_world).x, rep, scale);
        }

        // Debug space repetition boundaries
        // float tresh = 1. / 8.;
        // if (abs(abs(q.x)-rep/2.) <= tresh && abs(abs(q.y)-rep/2.) <= tresh ||
        //     abs(abs(q.x)-rep/2.) <= tresh && abs(abs(q.z)-rep/2.) <= tresh ||
        //     abs(abs(q.z)-rep/2.) <= tresh && abs(abs(q.y)-rep/2.) <= tresh) {
        //     voxel.material_type = 1;
        //     voxel.color = vec3(0,0,0);
        // }

        // Distance to tree
        TreeSDF tree = sd_spruce_tree((voxel_pos - hitPoint) / scale, qid);

        vec3 h_cell = vec3(0);  // hash33(qid);
        vec3 h_voxel = vec3(0); // hash33(voxel_pos);

        // Colorize tree
        if (tree.wood < 0) {
            voxel.material_type = 1;
            voxel.color = vec3(.68, .4, .15) * 0.16;
            voxel.roughness = 0.99;
        } else if (tree.leaves < 0) {
            voxel.material_type = 1;
            voxel.color = forest_biome_color * 0.5;
            voxel.roughness = 0.95;
        }
    }
}

// Color palettes
vec3 palette(in float t, in vec3 a, in vec3 b, in vec3 c, in vec3 d) {
    return a + b * cos(6.28318 * (c * t + d));
}
vec3 forest_biome_palette(float t) {
    return pow(vec3(85, 154, 78) / 255.0, vec3(2.2)); // palette(t + .5, vec3(0.07, 0.22, 0.03), vec3(0.03, 0.05, 0.01), vec3(-1.212, -2.052, 0.058), vec3(1.598, 6.178, 0.380));
}

#define UserAllocatorType GrassStrandAllocator
#define UserIndexType uint
#define UserMaxElementCount MAX_GRASS_BLADES
#include <utilities/allocator.glsl>

#define UserAllocatorType FlowerAllocator
#define UserIndexType uint
#define UserMaxElementCount MAX_FLOWERS
#include <utilities/allocator.glsl>

#define UserAllocatorType TreeParticleAllocator
#define UserIndexType uint
#define UserMaxElementCount MAX_TREE_PARTICLES
#include <utilities/allocator.glsl>

#define UserAllocatorType FireParticleAllocator
#define UserIndexType uint
#define UserMaxElementCount MAX_FIRE_PARTICLES
#include <utilities/allocator.glsl>

void spawn_grass(in out Voxel voxel) {
    GrassStrand grass_strand;
    grass_strand.origin = voxel_pos;
    grass_strand.packed_voxel = pack_voxel(voxel);
    grass_strand.flags = 1;

    uint index = GrassStrandAllocator_malloc(grass_allocator);
    daxa_RWBufferPtr(GrassStrand) grass_strands = deref(grass_allocator).heap;
    if (index < MAX_GRASS_BLADES) {
        deref(advance(grass_strands, index)) = grass_strand;
    }
}
void spawn_flower(in out Voxel voxel, uint flower_type) {
    Flower flower;
    flower.origin = voxel_pos;
    flower.packed_voxel = pack_voxel(voxel);
    flower.type = flower_type;

    uint index = FlowerAllocator_malloc(flower_allocator);
    daxa_RWBufferPtr(Flower) flowers = deref(flower_allocator).heap;
    if (index < MAX_FLOWERS) {
        deref(advance(flowers, index)) = flower;
    }
}
void spawn_tree_particle(in out Voxel voxel) {
    TreeParticle tree_particle;
    tree_particle.origin = voxel_pos;
    tree_particle.packed_voxel = pack_voxel(voxel);
    tree_particle.flags = 1;

    uint index = TreeParticleAllocator_malloc(tree_particle_allocator);
    daxa_RWBufferPtr(TreeParticle) tree_particles = deref(tree_particle_allocator).heap;
    if (index < MAX_TREE_PARTICLES) {
        deref(advance(tree_particles, index)) = tree_particle;
    }
}
void spawn_fire_particle(in out Voxel voxel) {
    FireParticle fire_particle;
    fire_particle.origin = voxel_pos;
    fire_particle.packed_voxel = pack_voxel(voxel);
    fire_particle.flags = 1;

    uint index = FireParticleAllocator_malloc(fire_particle_allocator);
    daxa_RWBufferPtr(FireParticle) fire_particles = deref(fire_particle_allocator).heap;
    if (index < MAX_FIRE_PARTICLES) {
        deref(advance(fire_particles, index)) = fire_particle;
    }
}

void try_spawn_grass(in out Voxel voxel, vec3 nrm) {
    // randomly spawn grass
    float r2 = good_rand(voxel_pos.xy);
    float upwards = dot(nrm, vec3(0, 0, 1));
    if (upwards > 0.35 && r2 < 0.75) {
        voxel.color = pow(vec3(85, 166, 78) / 255.0 * 0.5, vec3(2.2));
        voxel.material_type = 1;
        voxel.roughness = 1.0;
        voxel.normal = nrm;

        // spawn strand!!

        if (r2 < 0.2) {
            if (r2 < 0.99 * 0.2) {
                spawn_grass(voxel);
            } else {
                FractalNoiseConfig noise_conf = FractalNoiseConfig(
                    /* .amplitude   = */ 1.0,
                    /* .persistance = */ 0.5,
                    /* .scale       = */ 0.1,
                    /* .lacunarity  = */ 2,
                    /* .octaves     = */ 3);
                vec4 flower_noise_val = fractal_noise(g_value_noise_tex, g_sampler_llr, vec3(voxel_pos.xy, 0), noise_conf);
                float v = flower_noise_val.x * (1.0 / 0.875);

                uint flower_type = 0;
                if (v < 0.4) {
                    flower_type = FLOWER_TYPE_DANDELION;
                } else if (v < 0.5) {
                    flower_type = FLOWER_TYPE_DANDELION_WHITE;
                } else if (v < 0.65) {
                    flower_type = FLOWER_TYPE_TULIP;
                } else {
                    flower_type = FLOWER_TYPE_LAVENDER;
                }
                spawn_flower(voxel, flower_type);
            }
        }
    }
}

#define ENABLE_TREE_GENERATION 0

void brushgen_world_terrain(in out Voxel voxel) {
    vec4 val4 = terrain_noise(voxel_pos);
    float val = val4.x;
    vec3 nrm = normalize(val4.yzw); // terrain_nrm(voxel_pos);
    float upwards = dot(nrm, vec3(0, 0, 1));

    // Smooth noise depending on 2d position only
    float voxel_noise_xy = fbm2(voxel_pos.xy / 8 / 40);
    // Smooth biome color
    vec3 forest_biome_color = forest_biome_palette(voxel_noise_xy * 2 - 1);

    if (val < 0) {
        voxel.material_type = 1;
        const bool SHOULD_COLOR_WORLD = true;
        voxel.normal = nrm;
        voxel.roughness = 1.0;
        if (SHOULD_COLOR_WORLD) {
            float r = good_rand(-val);
            if (val > -0.05 && upwards > 0.25) {
                voxel.color = vec3(0.13, 0.09, 0.05);
                if (r < 0.5) {
                    voxel.color.r *= 0.5;
                    voxel.color.g *= 0.5;
                    voxel.color.b *= 0.5;
                    voxel.roughness = 0.99;
                } else if (r < 0.52) {
                    voxel.color.r *= 1.5;
                    voxel.color.g *= 1.5;
                    voxel.color.b *= 1.5;
                    voxel.roughness = 0.95;
                }
            } else if (val < -0.01 && val > -0.07 && upwards > 0.2) {
                voxel.color = vec3(0.17, 0.15, 0.07);
                if (r < 0.5) {
                    voxel.color.r *= 0.75;
                    voxel.color.g *= 0.75;
                    voxel.color.b *= 0.75;
                }
                voxel.roughness = 0.85;
            } else {
                voxel.color = vec3(0.11, 0.10, 0.07);
                voxel.roughness = 0.9;
            }
        } else {
            voxel.color = vec3(0.25);
        }
    } else if (true) {
        vec4 grass_val4 = terrain_noise(voxel_pos - vec3(0, 0, VOXEL_SIZE));
        float grass_val = grass_val4.x;
        if (grass_val < 0.0) {
            try_spawn_grass(voxel, nrm);
        } else if (ENABLE_TREE_GENERATION != 0) {
            try_spawn_tree(voxel, forest_biome_color, nrm);
        }
    }
}

// ============================================================================
//   VOXL TEST SCENE
// ============================================================================
//
// A deliberately small, hand-authored level: rolling ground, one conifer, a cave
// with a lit chamber, and ground detail. It exists to answer three questions with
// numbers rather than opinion:
//
//   1. does a Voxl-authored world fit in this engine's memory budget?
//   2. does the path-traced GI carry an emissive voxel through a dark interior?
//   3. what does 16 voxels/metre actually cost when the content is authored at
//      that scale instead of scaled down from metre blocks?
//
// Layout, coordinates and camera positions: docs/SCENE.md.
//
// -- FOUR RULES THIS FILE OBEYS, AND WHY --------------------------------------
//
// (1) EVERY RANDOM NUMBER IS A HASH OF A *WORLD* COORDINATE.
//     voxel_world.comp.glsl:193 seeds the per-invocation PRNG with `voxel_i`,
//     which is the *chunk-buffer* index, not the world position. Chunk indices
//     wrap modulo 32 around the player (ENABLE_CHUNK_WRAPPING), so rand() returns
//     a different stream for the same piece of world after you walk away and come
//     back. Anything placed with rand() would move. Everything here goes through
//     voxl_hash3()/good_rand()/fbm2() of `voxel_pos`, which is absolute metres.
//
// (2) INTERIORS ARE UNIFORM, BECAUSE UNIFORM IS FREE.
//     The palette compressor allocates nothing at all for an 8^3 region whose
//     voxels are bit-identical -- the single value is stored inline in the
//     header's blob_ptr and no heap page is taken (voxel_world.comp.glsl:908-915).
//     So colour grain, roughness variation and geometric normals are confined to
//     a thin skin near a surface; below ~1.1 m the rock is one flat colour. The
//     inherited demo generator does the opposite (it writes an analytic normal to
//     every solid voxel, `brushgen_world_terrain` above, line ~319) and its heap
//     settles at 1376256 pages / 2906 MB (docs/BASELINE.md sec 2).
//
// (3) BIG SMOOTH SURFACES GET AN ANALYTIC NORMAL; VOXEL-SIZED DETAIL DOES NOT.
//     The chunk-edit post-process regenerates a normal from the 5^3 neighbourhood
//     for any surface voxel whose normal is still the default +Z
//     (voxel_world.comp.glsl:419-422), and nullifies the normal of any fully
//     occluded voxel (line 411-414). For needles, pebbles and mushrooms that is
//     exactly right: the derived normal matches the voxel shape and the buried
//     interior stays uniform, which is free.
//
//     It is NOT right for the hillside, and this was measured rather than assumed.
//     The first version of this file left every normal at the default; the result
//     had 1-voxel black seams along every chunk boundary and metre-wide patches of
//     unlit surface (scratchpad t02-approach.png). Cause: that 5^3 neighbourhood
//     is read with get_temp_voxel(), which crosses chunk boundaries, and only
//     MAX_CHUNK_UPDATES_PER_FRAME (128) chunks are generated per frame out of
//     CHUNKS_PER_AXIS^3. A chunk generated before its neighbour reads that
//     neighbour as air, derives a normal tilted into it, and is never revisited.
//     On the demo's thin noisy terrain that is invisible; on a smooth 10 m
//     hillside of one colour it is a black triangle.
//
//     So: terrain within 0.5 m of the surface gets the height-field normal, cave
//     walls within 0.25 m of the void get the SDF gradient, and everything else is
//     left to the engine. This is the same choice the inherited generator makes
//     (`voxel.normal = nrm`, brushgen_world_terrain above) -- it just is not
//     obvious *why* until you leave it out.
//
// (4) DETAIL IS AUTHORED AT THE SIZE OF A VOXEL.
//     A voxel is 1/16 m = 6.25 cm. A needle clump is 2-3 voxels, a mushroom cap
//     is 5 voxels across, a pebble is 6-13. Nothing here is a 1 m shape sampled
//     finely -- that is what produces smooth blobs.
//
// Set VOXL_TEST_SCENE to 0 to fall back to the inherited demo terrain, which is
// left intact above so the two can be A/B'd in a one-character edit.
#define VOXL_TEST_SCENE 1

// Bisection switches. Each removes one subsystem from the generator so a visual
// defect can be attributed instead of guessed at. They cost nothing at runtime
// (the compiler folds them) and they are the reason the chunk-boundary normal
// artefact in rule (3) is described as measured rather than suspected.
#define VOXL_DEBUG_NO_CAVE 0
#define VOXL_DEBUG_NO_PROPS 0
// Added 2026-07-31 while attributing the black holes in the hill. See docs/SCENE.md sec 7.1:
// BARE_HEIGHTFIELD 1 renders nothing but the terrain solid test in one colour; FLAT_ROCK 1 keeps
// all the geometry but gives every solid voxel the same packed value. The holes disappear under
// either, and under nothing else -- which is what localised them to the voxel-heap path rather
// than to this file's geometry.
#define VOXL_DEBUG_BARE_HEIGHTFIELD 0
#define VOXL_DEBUG_FLAT_ROCK 0
#define VOXL_DEBUG_NO_LIGHT 0

// -- placement ----------------------------------------------------------------
// The scene is authored *around the spawn*, not the other way round: the spawn is
// hardcoded at player_unit_offset (-183,-110,-47) + pos (0.01,0.02,0.03) in
// src/application/player.cpp, which this file does not own. VOXL_ORIGIN.z is then
// chosen so the camera (which sits 0.2 m *below* PLAYER.pos, player.cpp:305)
// starts exactly 1.73 m above the spawn pad -- standing eye height.
//
//   camera at spawn, absolute : (-182.99, -109.98, -47.17)
//   camera at spawn, local    : (   0.01,    0.02,    5.33)
//   spawn pad surface, local  :                        3.60
//
// The spawn also fixes the *direction* everything is laid out in. Startup yaw is
// pi*0.25 and pitch pi*0.349 (player.cpp:56-57), which works out as a view
// direction of (0.629, 0.629, -0.456): 45 degrees in plan, 27 degrees below the
// horizon. Every feature below therefore sits on the +X+Y diagonal, in order of
// distance, so the whole scene is one walk: hold W from the spawn and you pass
// the tree, reach the cave mouth, and end up in the lit chamber.
#define VOXL_ORIGIN vec3(-183.0, -110.0, -52.50)

// HOW BIG THE ISLAND IS ALLOWED TO BE, and why it is computed rather than typed.
// The engine's world is a cube of CHUNKS_PER_AXIS^3 chunks that wraps modulo
// CHUNKS_PER_AXIS around the player (ENABLE_CHUNK_WRAPPING, voxel_malloc.inl),
// so at any instant only +/- CHUNKS_PER_AXIS * 2 metres in each axis exists.
// Terrain authored beyond that is not "far away", it is *absent*, and the island
// would show a cut edge at the volume boundary. Deriving the radius from the
// constant means this file stays correct if that constant moves:
//
//     CHUNKS_PER_AXIS 32 -> 128 m world -> R 40 m -> island ~71 m across
//     CHUNKS_PER_AXIS 16 ->  64 m world -> R 21 m -> island ~37 m across
//
// The *content* (spawn, tree, hill, cave) is fixed and spans 34 m along the
// diagonal, which fits inside the smaller of those with room to spare. Sizing the
// content for the small world and letting only the surrounding ground grow is the
// safe direction: a scene that fits a 64 m world also fits a 128 m one.
#define VOXL_WORLD_HALF (float(CHUNKS_PER_AXIS) * CHUNK_WORLDSPACE_SIZE * 0.5)
#define VOXL_ISLAND_C vec2(10.0, 10.0)
#define VOXL_ISLAND_R min(40.0, VOXL_WORLD_HALF - 11.0)

// Level pad at the spawn, 3.6 m above the general ground. It is not decoration:
// it makes the camera's height above ground a known constant (1.73 m) instead of
// whatever the noise did at (0,0), and standing that much above the clearing is
// what puts the tree crown and the hill top inside the 74-degree vertical FOV at
// the startup pitch. Both are within 3 degrees of the top of the frame; lower the
// pad and they leave it.
#define VOXL_PAD_Z 3.60

// The hill the cave is cut into. Its profile is a plateau rather than a dome
// (see voxl_mound) so there is 2.6-4.0 m of rock over the whole chamber ceiling
// instead of only over its centre.
#define VOXL_HILL_C vec2(17.0, 17.0)
#define VOXL_HILL_R 10.0
#define VOXL_HILL_H 8.5

// The tree: 17.3 m from the spawn, 5.0 m to the right of the walk line, and 5.0 m
// clear of the cave mouth. Off the line so walking forward passes it; clear of the
// mouth so it does not stand in front of the thing it is next to.
#define VOXL_TREE_XY vec2(15.2, 8.2)

// Cave. The tunnel runs along the spawn view axis. VOXL_TUNNEL_A is where the
// tunnel *capsule* starts, not where the cave opens -- and getting that wrong is
// worth recording: the first version started it at (9.5, 9.5), 13 m out, where the
// ground is around 1 m and the capsule's underside sits at 0.4 m, so it carved a
// 2.9 m wide circular trench straight across the meadow before it ever reached the
// hill (scratchpad u02-approach.png). A tunnel only removes rock that is there, so
// the fix is to start it exactly where the rising hillside meets its underside: at
// (10.6, 10.6) the ground is 0.60 m and the capsule's underside is 0.70 m, so
// nothing outside is touched. The roof then closes 1.4 m further in, at
// VOXL_MOUTH_XY = (11.6, 11.6), 16.4 m from the spawn.
#define VOXL_TUNNEL_A vec3(10.6, 10.6, 2.35)
#define VOXL_TUNNEL_B vec3(17.2, 17.4, 2.05)
#define VOXL_TUNNEL_R 1.30
#define VOXL_MOUTH_XY vec2(11.75, 11.75)
#define VOXL_CHAMBER_C vec3(18.4, 18.7, 2.25)
#define VOXL_CHAMBER_R vec3(3.10, 2.95, 2.35)
// Bounding box for the whole void, used to skip the cave SDF everywhere else. It
// is padded 1 m beyond the void because voxl_rock_material tests the *distance*
// to the cave, not just the sign, when deciding which rock a voxel is.
#define VOXL_CAVE_BB_C vec3(15.5, 15.6, 3.00)
#define VOXL_CAVE_BB_R vec3(7.6, 7.7, 3.6)

// The emissive cluster, on the chamber floor 3.4 m across from where the tunnel
// enters -- far enough that you have to walk in to see it, close enough that its
// glow on the chamber wall is visible from the last few metres of tunnel.
#define VOXL_LIGHT_C vec3(19.50, 19.90, 1.26)

// The chamber and tunnel floor: one gently inward-sloping plane. The constant is
// not arbitrary -- it is calibrated to 1.50 m at the tunnel's outer end, which is
// where the hillside's own surface is. Set it lower (0.55 was the first attempt)
// and the tunnel's approach becomes a 1 m deep trench cut across the meadow, which
// reads as a stone pipe lying in the grass rather than as a cave. It falls to
// 1.34 m in the chamber, leaving 2.15 m of headroom in the tunnel and 3.26 m in
// the chamber -- both clear of the 1.75 m player height (player.cpp:39).
#define VOXL_FLOOR_Z(s) (1.50 - 0.010 * ((s).x + (s).y - 21.2))

// -- small helpers ------------------------------------------------------------

// A stable [0,1) hash of an integer cell. The per-component scales are different
// primes on purpose: good_rand_hash(uvec3) folds the components with XOR
// (random.glsl:21), so (a,b,c) and (a,c,b) would otherwise collide.
float voxl_hash3(vec3 cell) {
    return good_rand(cell * vec3(0.7071, 1.3117, 2.1783) + vec3(0.5, 1.7, 3.1));
}
// Hash of the cell of size 1/scl metres that contains p.
float voxl_cell_rand(vec3 p, float scl, float salt) {
    return voxl_hash3(floor(p * scl) + salt);
}

// Colours are authored the way the inherited generator authors them: an 8-bit
// sRGB-ish triple, darkened *before* the 2.2 gamma decode, giving linear albedo.
// Note the storage budget this has to survive: pack_rgb() keeps 6 bits per channel
// in gamma space (pack_unpack.glsl:24-32), so 63 steps per channel. Any variation
// finer than ~1.6% of gamma range is quantised away; every "jitter" below is
// therefore a choice between a few discrete colours, never a continuous ramp.
// Discrete also keeps palette regions small, which is rule (2).
vec3 voxl_col(vec3 rgb255, float k) {
    return pow(rgb255 * (k / 255.0), vec3(2.2));
}

// Mound profile: a plateau, flat out to u=0.42 and falling smoothly to 0 at u=1.
// A dome ((1-u^2)^2, the obvious choice) was tried first and rejected: it only
// holds full height at its very centre, which left barely 1 m of rock over the far
// half of the chamber ceiling. A flat-topped hill is also simply what a bluff with
// a cave in it looks like.
float voxl_mound(float u) {
    return 1.0 - smoothstep(0.42, 1.0, u);
}

// Cheap pseudo-3D noise in [-1.5,1.5], built from the engine's 2-D integer-lattice
// value noise on three orthogonal planes. Used only to roughen cave walls, where
// the exact spectrum does not matter but the absence of a texture lookup does:
// fbm2() hashes integers, so it is bit-reproducible for a given world coordinate
// and needs no sampler state inside the brush.
float voxl_wobble(vec3 p, float scl) {
    return (fbm2(p.xy * scl) - 0.5) + (fbm2(p.yz * scl + 7.0) - 0.5) + (fbm2(p.zx * scl + 23.0) - 0.5);
}

// -- terrain ------------------------------------------------------------------

// Surface height in scene-local metres. Called once per voxel (a whole vertical
// column re-evaluates it, which is the price of the engine's one-invocation-per-
// voxel dispatch), so it is kept to four value-noise lookups and two mounds.
float voxl_ground_h(vec2 q) {
    // Gentle rolling: +/-0.58 m at an 11 m wavelength, +/-0.19 m at 4.1 m.
    float h = (fbm2(q * (1.0 / 11.0)) - 0.5) * 1.55 +
              (fbm2(q * (1.0 / 4.1) + 41.0) - 0.5) * 0.50;

    // The hill the cave is cut into. Its radius is warped by +/-0.6 m of noise so
    // the silhouette is not a surface of revolution. The amplitude is deliberately
    // small: the mound is steep, so warping u by 0.06 already moves the surface
    // 1.3 m vertically on the flank, and the chamber ceiling only has 2.4-4.6 m of
    // rock over it. Larger warps were tried and breached the roof.
    float hu = length(q - VOXL_HILL_C) / VOXL_HILL_R + (fbm2(q * 0.19) - 0.5) * 0.16;
    h += VOXL_HILL_H * voxl_mound(hu);

    // Island rim: flat inside 72% of the radius, then falling 18 m over the last
    // 28%. Because the underside (voxl_ground_base) sits at -6 m out there, the
    // top surface meets the underside before it reaches the nominal radius and the
    // island simply ends -- no cut face, no vertical cliff giving the volume away.
    // Solid terrain therefore stops at about 0.88 * VOXL_ISLAND_R.
    float t = clamp((length(q - VOXL_ISLAND_C) - VOXL_ISLAND_R * 0.72) / (VOXL_ISLAND_R * 0.28), 0.0, 1.0);
    h -= 18.0 * t * t;

    // The spawn pad. mix() rather than max() so the pad blends into the rolling
    // ground instead of stamping a mesa on top of it.
    return mix(h, VOXL_PAD_Z, 1.0 - smoothstep(5.0, 17.0, length(q)));
}

// Underside of the island: a shallow dome, so flying beneath it shows a landform
// rather than a slab with a flat bottom.
float voxl_ground_base(vec2 q) {
    float r = length(q - VOXL_ISLAND_C) / (VOXL_ISLAND_R * 0.90);
    return -6.0 - 5.0 * sqrt(max(0.0, 1.0 - r * r));
}

// Surface normal by central difference of the height field. The 0.25 m arm is
// four voxels: short enough to follow the 4.1 m noise octave, long enough that the
// result does not chase individual voxel steps. See rule (3) for why this exists
// at all rather than letting the engine derive it.
vec3 voxl_ground_nrm(vec2 q) {
    const float E = 0.25;
    float gx = voxl_ground_h(q + vec2(E, 0.0)) - voxl_ground_h(q - vec2(E, 0.0));
    float gy = voxl_ground_h(q + vec2(0.0, E)) - voxl_ground_h(q - vec2(0.0, E));
    return normalize(vec3(-gx, -gy, 2.0 * E));
}

// Bare rock. Only on the hill, and only above ~3.6 m of it, with the boundary
// wobbled by a 2.9 m noise field so it is not a contour line. The `onhill` factor
// is not cosmetic: the spawn pad stands at 3.60 m, the same height, and without it
// the pad would turn to stone.
float voxl_bare(vec3 s) {
    float onhill = 1.0 - smoothstep(9.0, 12.0, length(s.xy - VOXL_HILL_C));
    return onhill * smoothstep(2.6, 5.0, s.z + (fbm2(s.xy * 0.35) - 0.5) * 2.4);
}

// -- cave ---------------------------------------------------------------------

// Signed distance to the cave void; negative means air. Tunnel + chamber, walls
// roughened, then flattened by a sloping floor plane so the whole thing is
// walkable.
//
// WHY THE CHAMBER IS ACTUALLY DARK, stated with the numbers, because "it looks
// enclosed" is not evidence. The sun defaults to Angle X 210, Angle Y 25
// (renderer/atmosphere/sky.inl:67-68), giving a direction-to-sun of
// (-0.366, -0.211, +0.906): 65 degrees above the horizon, and in plan within 15
// degrees of the tunnel axis. So sunlight *does* shine straight into the mouth --
// which is what makes the entrance gradient worth photographing -- but it travels
// 2.14 m down for every 1 m forward. A 2.15 m portal therefore puts the last
// direct sunlight on the floor about 1.0 m inside. The chamber starts 7 m further
// in, behind a 2.6 m aperture, under 2.9-4.6 m of rock. Nothing but bounce gets
// there, which is exactly what is being tested.
float voxl_cave_sd(vec3 s) {
    float d = sd_capsule(s, VOXL_TUNNEL_A, VOXL_TUNNEL_B, VOXL_TUNNEL_R);
    d = sd_smooth_union(d, sd_ellipsoid(s - VOXL_CHAMBER_C, VOXL_CHAMBER_R), 0.9);
    // Roughen the walls. +/-0.25 m is +/-4 voxels, enough to kill the analytic
    // silhouette without pinching the tunnel below 2.4 m of headroom.
    d += voxl_wobble(s, 0.55) * 0.22;
    // Floor: intersect away everything below the floor plane, wobbled by about one
    // voxel. The wobble is applied here and not to `d` so the floor stays level
    // while the walls stay rough.
    return max(d, VOXL_FLOOR_Z(s) + voxl_wobble(s, 1.4) * 0.06 - s.z);
}

// Outward normal of a cave wall -- the direction from the rock into the void, so
// the negated SDF gradient. Four extra voxl_cave_sd() evaluations via the standard
// tetrahedron trick, paid only by voxels inside VOXL_CAVE_WALL of the void; see
// rule (3) for why the engine's derived normal is not good enough on these
// surfaces. The 0.08 m arm is a little over one voxel.
#define VOXL_CAVE_WALL 0.25
vec3 voxl_cave_nrm(vec3 s) {
    const vec2 k = vec2(1.0, -1.0);
    const float e = 0.08;
    vec3 g = k.xyy * voxl_cave_sd(s + k.xyy * e) +
             k.yyx * voxl_cave_sd(s + k.yyx * e) +
             k.yxy * voxl_cave_sd(s + k.yxy * e) +
             k.xxx * voxl_cave_sd(s + k.xxx * e);
    float l = length(g);
    return (l > 1e-6) ? (-g / l) : vec3(0.0, 0.0, 1.0);
}

// -- the conifer --------------------------------------------------------------
//
// At 16 voxels/m a 6.6 m spruce is 106 voxels tall, so there is room for real
// structure: a bare trunk to 1.85 m, then nine whorls of branches that shorten
// and lift toward the crown.
//
// The foliage is NOT an SDF that gets filled solid. `foliage` below is a distance
// *normalised by the whorl's envelope radius*, so 0 is a branch axis and 1 is the
// edge of that branch's needle envelope; the caller turns it into a probability
// and fills 2-3 voxel clumps stochastically. That is the whole difference between
// needles and a green blob, and it only works because a clump is a few voxels:
// at 1 m blocks there is nothing to be stochastic about.
#define VOXL_TREE_H 6.60
#define VOXL_TREE_BARE 1.85
#define VOXL_TREE_WHORLS 9

struct VoxlTree {
    float wood;    // signed distance in metres
    float foliage; // 0 at a branch axis, 1 at the envelope edge
};

VoxlTree voxl_conifer(vec3 p) {
    VoxlTree t = VoxlTree(1e5, 1e5);

    // A very slight lean, applied to the whole tree, so the silhouette is not a
    // perfect axis of revolution. 0.17 m of offset by the crown.
    p.xy -= vec2(0.0040, 0.0026) * p.z * p.z;

    // Trunk. It starts 0.9 m below the ground height sampled at the tree's own
    // centre, which is deeper than the rolling noise can drop across the trunk's
    // 0.3 m width -- so the base is flush with whatever ground is actually there
    // and never a floating disc.
    t.wood = sd_round_cone(p, vec3(0.0, 0.0, -0.90), vec3(0.0, 0.0, VOXL_TREE_H), 0.150, 0.026);

    // The leader: needles carry on past the top whorl to the tip.
    t.foliage = sd_capsule(p, vec3(0.0, 0.0, VOXL_TREE_H - 1.35), vec3(0.0, 0.0, VOXL_TREE_H - 0.06), 0.0) * (1.0 / 0.17);

    for (int i = 0; i < VOXL_TREE_WHORLS; ++i) {
        float f = float(i) / float(VOXL_TREE_WHORLS - 1); // 0 lowest .. 1 highest
        float z = mix(VOXL_TREE_BARE, VOXL_TREE_H - 0.55, f);
        float len = mix(1.95, 0.38, f);
        float rise = mix(-0.42, 0.34, f * f); // lower branches droop, upper lift
        float env = mix(0.36, 0.155, f);      // needle envelope radius

        // Skip whorls that cannot reach this voxel. Without this the inner loop
        // runs 63 times per voxel over the tree's bounding cylinder; with it,
        // typically 2-3 whorls.
        if (abs(p.z - z) > len + env + 0.6) {
            continue;
        }

        int n = 7 - (i / 3); // 7,7,7,6,6,6,5,5,5 branches
        float a0 = 2.39996323 * float(i); // golden angle: whorls never line up
        vec3 base = vec3(0.0, 0.0, z);
        for (int j = 0; j < 7; ++j) {
            if (j >= n) {
                break;
            }
            float a = a0 + 6.28318530 * float(j) / float(n);
            // Per-branch length jitter, keyed on the loop indices rather than on
            // world position: the tree is a single authored object, so its shape
            // must not change if it is ever moved.
            float bl = len * (0.80 + 0.34 * voxl_hash3(vec3(float(i), float(j), 3.0)));
            vec3 tip = base + vec3(cos(a), sin(a), rise) * bl;
            t.wood = min(t.wood, sd_capsule(p, base, tip, mix(0.046, 0.016, f)));
            // Needles start a fifth of the way out, leaving the inner canopy open.
            t.foliage = min(t.foliage, sd_capsule(p, mix(base, tip, 0.22), tip, 0.0) * (1.0 / env));
        }
    }
    return t;
}

// Returns true if it claimed the voxel.
bool voxl_tree_voxel(in out Voxel v, vec3 s) {
    // Reject on the bounding cylinder BEFORE evaluating the ground height: this
    // function is called for every air voxel in the scene and voxl_ground_h() is
    // four value-noise lookups. Radius is the longest branch (1.95 * 1.14) plus
    // the needle envelope.
    vec2 dxy = s.xy - VOXL_TREE_XY;
    if (dot(dxy, dxy) > 2.62 * 2.62) {
        return false;
    }
    vec3 p = vec3(dxy, s.z - voxl_ground_h(VOXL_TREE_XY));
    if (p.z < -1.0 || p.z > VOXL_TREE_H + 0.5) {
        return false;
    }

    VoxlTree t = voxl_conifer(p);

    if (t.wood < 0.0) {
        // Bark. The cells are 1/13 m across and 5.9/13 = 0.45 m tall, which at
        // 6.25 cm makes vertical streaks about one voxel wide and seven tall --
        // fissured conifer bark, not noise.
        float b = voxl_cell_rand(vec3(voxel_pos.xy, voxel_pos.z * (1.0 / 5.9)), 13.0, 5.3);
        v.material_type = 1;
        v.roughness = 0.99;
        v.color = voxl_col(vec3(96.0, 66.0, 44.0), 0.30);
        if (b < 0.34) {
            v.color = voxl_col(vec3(70.0, 48.0, 32.0), 0.30);
        } else if (b > 0.84) {
            v.color = voxl_col(vec3(120.0, 86.0, 58.0), 0.30);
        }
        return true;
    }

    float env = 1.0 - t.foliage; // > 0 inside the needle envelope
    if (env <= 0.0) {
        return false;
    }

    // Clump at 1/7 m ~ 2.3 voxels. Per-*voxel* noise would be correct-looking at
    // one metre and invisible at twenty; clumps this size survive the distance.
    // Density never reaches 1, so the canopy has gaps all the way through and the
    // sun dapples the ground under it -- which is the reference look.
    float dens = 0.22 + 0.62 * env * env;
    if (voxl_cell_rand(voxel_pos, 7.0, 3.1) >= dens) {
        return false;
    }

    // Five greens. Biasing the *choice* by height costs no extra palette entries
    // and still gives the crown its lighter new growth.
    float ci = clamp(voxl_cell_rand(voxel_pos, 7.0, 9.7) - 0.24 * (p.z / VOXL_TREE_H - 0.45), 0.0, 0.999);
    v.material_type = 1;
    v.roughness = 0.95;
    if (ci < 0.16) {
        v.color = voxl_col(vec3(126.0, 158.0, 76.0), 0.62); // sunlit new growth
    } else if (ci < 0.40) {
        v.color = voxl_col(vec3(88.0, 140.0, 68.0), 0.60);
    } else if (ci < 0.68) {
        v.color = voxl_col(vec3(60.0, 116.0, 62.0), 0.58);
    } else if (ci < 0.88) {
        v.color = voxl_col(vec3(44.0, 94.0, 58.0), 0.56); // shaded interior
    } else {
        v.color = voxl_col(vec3(74.0, 130.0, 100.0), 0.54); // cool blue-green
    }
    return true;
}

// -- the light in the cave ----------------------------------------------------
//
// HOW EMISSION IS REPRESENTED HERE, because it is not where you would look for it.
// There are no analytic or punctual lights anywhere in this engine -- the
// triangle-light sampling block is commented out and every light except the sun
// is an emissive voxel resolved through the irradiance cache
// (renderer/kajiya/ircache/ircache_trace_common.inc.glsl:129). The voxel format is
// 2 bits of material type + 4 of roughness + 8 of normal + 18 of colour
// (pack_unpack.glsl:47), and there is no room for an intensity field, so emission
// strength is smuggled through the ROUGHNESS bits:
//
//     gbuffer.glsl:20   emissive = color * float(material_type == 3) * (2.0 * roughness + 0.01)
//     gbuffer.glsl:24   albedo   = color * float(material_type == 1 || material_type == 2)
//
// Two consequences worth knowing before tuning this: roughness 1.0 is the ceiling,
// giving 2.01x the packed colour, and a type-3 voxel has *zero* albedo, so it is a
// pure emitter that does not itself receive bounce.
//
// Roughness also quantises hard -- it is stored as round(sqrt(r)*15)/15 and squared
// back, so the only values that survive are (k/15)^2. 1.0 and 0.5378 below are
// exact; anything else silently snaps.
bool voxl_cave_light(in out Voxel v, vec3 s) {
#if VOXL_DEBUG_NO_LIGHT
    // The control for docs/images/15-cave-interior-dark.png. Deletes the crystals and their
    // floor crust and changes NOTHING else -- same geometry, same materials, same sun, same
    // camera -- so the pair is an A/B of one variable. Set to 1, relaunch (the shader is
    // compiled at runtime, so no C++ rebuild), shoot, set back to 0.
    return false;
#endif
    vec3 p = s - VOXL_LIGHT_C;
    if (dot(p, p) > 2.0 * 2.0) {
        return false;
    }

    // Five shards, leaning apart, tallest in the middle: 1.1 m of emitting height
    // and ~1.5 m^2 of emitting area. Area matters as much as radiance here -- the
    // irradiance cache fires one bounce per probe per frame, so a pinpoint emitter
    // converges as visible sparkle where a hand-sized one converges clean.
    float d = sd_round_cone(p, vec3(0.00, 0.00, -0.04), vec3(0.02, 0.04, 1.08), 0.200, 0.025);
    d = min(d, sd_round_cone(p, vec3(0.35, -0.25, -0.04), vec3(0.62, -0.45, 0.78), 0.150, 0.022));
    d = min(d, sd_round_cone(p, vec3(-0.32, 0.22, -0.04), vec3(-0.58, 0.46, 0.68), 0.140, 0.020));
    d = min(d, sd_round_cone(p, vec3(0.10, 0.35, -0.04), vec3(0.26, 0.66, 0.90), 0.130, 0.020));
    d = min(d, sd_round_cone(p, vec3(-0.20, -0.30, -0.04), vec3(-0.35, -0.54, 0.54), 0.110, 0.018));

    if (d < 0.0) {
        v.material_type = 3;
        v.roughness = 1.0; // maximum: emissive = colour * 2.01
        v.color = vec3(1.00, 0.52, 0.16);
        return true;
    }

    // A dimmer crust spreading over the floor around the shards. It roughly
    // doubles the emitting area for half the radiance, which is what makes the
    // amber bounce on the far wall read as a wash rather than as five separate
    // speckle sources.
    float floor_z = VOXL_FLOOR_Z(s) + voxl_wobble(s, 1.4) * 0.06;
    if (s.z < floor_z + 0.075 && dot(p.xy, p.xy) < 1.55 * 1.55) {
        float fade = 1.0 - length(p.xy) * (1.0 / 1.55);
        if (voxl_cell_rand(voxel_pos, 5.0, 211.0) < fade * 0.85) {
            v.material_type = 3;
            v.roughness = 0.5378; // exactly representable; emissive = colour * 1.086
            v.color = vec3(0.86, 0.40, 0.12);
            return true;
        }
    }
    return false;
}

// -- rock, soil and cave walls ------------------------------------------------
//
// `depth` is metres below the surface, `cave_d` the distance to the cave void.
// Everything deeper than 1.1 m and further than 0.55 m from the cave gets ONE
// colour and ONE roughness with no jitter at all: those voxels are also fully
// occluded, so the post-process nullifies their normals too, and the resulting
// 8^3 regions are bit-uniform and cost zero heap pages. That single decision is
// most of the difference between this scene's heap and the demo's.
void voxl_rock_material(in out Voxel v, vec3 s, float depth, float cave_d, float bare, vec3 nrm) {
    v.material_type = 1;
    v.normal = nrm;

#if VOXL_DEBUG_FLAT_ROCK
    // Bisection: one colour and one roughness for all solid terrain, normals untouched. Isolates
    // per-voxel palette variety from geometry.
    v.roughness = 0.9;
    v.color = vec3(0.30, 0.06, 0.06);
    return;
#endif

    if (cave_d < 0.55) {
        // Cave wall: pale limestone-ish rock, deliberately brighter than the
        // outside stone. Bounce light is albedo times irradiance, and the point
        // of the chamber is to see the bounce.
        float g = voxl_cell_rand(voxel_pos, 6.0, 29.0);
        v.roughness = 0.90;
        if (g < 0.30) {
            v.color = voxl_col(vec3(150.0, 148.0, 142.0), 0.82);
        } else if (g < 0.80) {
            v.color = voxl_col(vec3(168.0, 166.0, 158.0), 0.82);
        } else {
            v.color = voxl_col(vec3(184.0, 180.0, 170.0), 0.82);
        }
        // A damp darker band in the first half metre above the floor.
        if (s.z < VOXL_FLOOR_Z(s) + 0.55) {
            v.color *= 0.62;
        }
        return;
    }

    if (bare > 0.5 && depth < 0.9) {
        // Exposed rock on the crown of the hill and on the steepest faces.
        float g = voxl_cell_rand(voxel_pos, 5.0, 37.0);
        v.roughness = 0.93;
        v.color = (g < 0.42) ? voxl_col(vec3(126.0, 122.0, 114.0), 0.70)
                             : ((g < 0.86) ? voxl_col(vec3(142.0, 137.0, 126.0), 0.70)
                                           : voxl_col(vec3(110.0, 108.0, 104.0), 0.70));
        return;
    }

    if (depth < 0.34) {
        // Topsoil. Visible wherever grass did not take: bare patches, steep
        // ground, the lip of the cave mouth.
        float g = voxl_cell_rand(voxel_pos, 6.0, 43.0);
        v.roughness = 0.97;
        v.color = (g < 0.55) ? voxl_col(vec3(134.0, 96.0, 62.0), 0.52)
                             : voxl_col(vec3(112.0, 80.0, 52.0), 0.52);
        return;
    }

    if (depth < 1.10) {
        // Subsoil: paler, stonier.
        float g = voxl_cell_rand(voxel_pos, 5.0, 47.0);
        v.roughness = 0.94;
        v.color = (g < 0.70) ? voxl_col(vec3(146.0, 118.0, 84.0), 0.48)
                             : voxl_col(vec3(126.0, 104.0, 78.0), 0.48);
        return;
    }

    // Bedrock. Uniform on purpose -- see the note above this function.
    v.roughness = 0.90;
    v.color = voxl_col(vec3(122.0, 120.0, 116.0), 0.62);
}

// -- ground props: mushrooms and pebbles --------------------------------------
//
// Both are placed on a lattice of world cells, with existence and offset hashed
// from the cell index, so they are stable under chunk wrapping (rule 1) and
// identical between runs.
bool voxl_props(in out Voxel v, vec3 s) {
    // --- mushrooms, 2.6 m lattice --------------------------------------------
    // 0.38 m tall, cap 0.31 m across: 6 voxels by 5. That is about the smallest
    // object at this resolution that still reads as a shape rather than a speck,
    // and it is the same size the reference screenshots show.
    {
        vec2 cell = floor(s.xy * (1.0 / 2.6));
        vec2 off = vec2(voxl_hash3(vec3(cell, 71.0)), voxl_hash3(vec3(cell, 83.0)));
        vec2 mq = (cell + 0.15 + off * 0.70) * 2.6;
        vec2 dq = s.xy - mq;
        // Denser in the damp shade under the tree and around the cave mouth.
        float pr = 0.09 +
                   0.32 * max(0.0, 1.0 - length(mq - VOXL_TREE_XY) * (1.0 / 7.0)) +
                   0.24 * max(0.0, 1.0 - length(mq - VOXL_MOUTH_XY) * (1.0 / 8.0));
        if (voxl_hash3(vec3(cell, 61.0)) < pr && dot(dq, dq) < 0.048) {
            float mh = voxl_ground_h(mq);
            // Not on the bare rock of the hill, and not in the cutting the tunnel
            // carves out of the slope. Both tests use the mushroom's own position,
            // not this voxel's, so a mushroom is never half-culled.
            if (voxl_bare(vec3(mq, mh)) < 0.5 && voxl_cave_sd(vec3(mq, mh + 0.15)) > 0.35) {
                float scl = 0.72 + 0.55 * voxl_hash3(vec3(cell, 97.0));
                vec3 mp = vec3(dq, s.z - mh);
                float stem = sd_capsule(mp, vec3(0.0, 0.0, -0.03), vec3(0.0, 0.0, 0.235 * scl), 0.032 * scl);
                float cap = sd_ellipsoid(mp - vec3(0.0, 0.0, 0.245 * scl), vec3(0.150, 0.150, 0.092) * scl);
                if (min(stem, cap) < 0.0) {
                    v.material_type = 1;
                    v.roughness = 0.92;
                    v.color = (cap < 0.0) ? voxl_col(vec3(238.0, 232.0, 218.0), 0.92)
                                          : voxl_col(vec3(224.0, 214.0, 196.0), 0.74);
                    return true;
                }
            }
        }
    }

    // --- pebbles and small rocks, 4.4 m lattice -------------------------------
    {
        vec2 cell = floor(s.xy * (1.0 / 4.4));
        vec2 off = vec2(voxl_hash3(vec3(cell, 137.0)), voxl_hash3(vec3(cell, 149.0)));
        vec2 rq = (cell + 0.12 + off * 0.76) * 4.4;
        vec2 dq = s.xy - rq;
        float rr = 0.17 + 0.26 * voxl_hash3(vec3(cell, 151.0));
        if (voxl_hash3(vec3(cell, 131.0)) < 0.34 && dot(dq, dq) < (rr + 0.12) * (rr + 0.12)) {
            float rh = voxl_ground_h(rq);
            // Rocks are allowed on the bare hill (they belong there); only the
            // tunnel cutting is excluded.
            if (voxl_cave_sd(vec3(rq, rh + 0.15)) > 0.35) {
                vec3 ax = rr * vec3(1.0 + 0.34 * (voxl_hash3(vec3(cell, 157.0)) - 0.5),
                                    1.0 + 0.34 * (voxl_hash3(vec3(cell, 163.0)) - 0.5),
                                    0.62 + 0.30 * voxl_hash3(vec3(cell, 167.0)));
                // Sunk 40% into the ground, so it is bedded rather than resting on
                // the surface like a prop.
                float d = sd_ellipsoid(vec3(dq, s.z - rh + rr * 0.40), ax);
                // Chip the last voxel and a half off the silhouette. An analytic
                // ellipsoid at 6.25 cm reads as a CG ball; stone does not have a
                // smooth outline at any scale.
                if (d < (voxl_cell_rand(voxel_pos, 9.0, 173.0) - 0.5) * 0.16) {
                    float g = voxl_cell_rand(voxel_pos, 7.0, 179.0);
                    v.material_type = 1;
                    v.roughness = 0.93;
                    v.color = (g < 0.40) ? voxl_col(vec3(132.0, 130.0, 124.0), 0.72)
                                         : ((g < 0.84) ? voxl_col(vec3(148.0, 144.0, 134.0), 0.72)
                                                       : voxl_col(vec3(116.0, 116.0, 116.0), 0.72));
                    return true;
                }
            }
        }
    }
    return false;
}

// -- grass skin, grass strands and flowers ------------------------------------
//
// The grass skin is the ONE air voxel directly above the terrain surface, turned
// solid and coloured green. That is how the inherited generator does it too, and
// the shape of it is load-bearing rather than stylistic: the grass-strand
// simulation frees any strand whose spawner voxel no longer has air above it and
// whose material type and roughness no longer match the voxel it sits on
// (particles/grass/sim.comp.glsl:41-51). Spawning from a voxel *inside* the ground
// would make every blade delete itself on the next simulation tick.
//
// Worth remembering when reading a screenshot of this scene: grass and flowers are
// rasterised particles, not voxels. They are invisible to voxel_trace(), so they
// do not bounce light, do not appear in reflections, and cast shadows only through
// the 40 m ortho shadow map. The tree, the mushrooms and the pebbles are real
// voxels and do all three.
void voxl_ground_cover(in out Voxel v, vec3 s, vec3 nrm, float bare) {
    // 0.50 is a 60-degree slope. The hill's flank runs at about 65, so grass
    // climbs its skirt and stops partway up, which is what leaves the rocky bluff
    // reading as rock rather than as a mud cone.
    float r2 = good_rand(voxel_pos.xy);
    if (nrm.z <= 0.50 || bare > 0.5 || r2 >= 0.82) {
        return; // steep, rocky, or one of the 18% of cells left as bare soil
    }

    v.material_type = 1;
    v.roughness = 1.0;
    // The one place in this file that writes an explicit normal. A geometric
    // normal on a near-flat lawn is a staircase; the height-field normal is not,
    // and it varies slowly enough that a 0.5 m palette region still sees only a
    // handful of distinct values after octahedral packing.
    v.normal = nrm;

    // Five greens, chosen per voxel but *biased* by a 22 m noise field, so the
    // lawn has large soft patches of yellower and cooler grass without a single
    // extra palette entry: the patch varies which colour is picked, not the colour.
    float ci = clamp(good_rand(voxel_pos.yx + 3.7) * 0.80 + (fbm2(voxel_pos.xy * 0.045) - 0.5) * 0.70, 0.0, 0.999);
    if (ci < 0.18) {
        v.color = voxl_col(vec3(118.0, 178.0, 78.0), 0.48); // dry, yellow-green
    } else if (ci < 0.42) {
        v.color = voxl_col(vec3(104.0, 176.0, 84.0), 0.50);
    } else if (ci < 0.70) {
        v.color = voxl_col(vec3(88.0, 160.0, 74.0), 0.50);
    } else if (ci < 0.90) {
        v.color = voxl_col(vec3(74.0, 148.0, 68.0), 0.50);
    } else {
        v.color = voxl_col(vec3(64.0, 132.0, 72.0), 0.52); // cool, in the shade
    }

    if (r2 >= 0.24) {
        return; // coloured, but no particle on this voxel
    }

    if (r2 < 0.2372) {
        // 256 surface voxels/m^2 * 0.82 * 0.2372 = ~50 blades/m^2. Over the ~900
        // m^2 of lawn on the 37 m island that is ~44k strands, and ~250k on the
        // 71 m one -- against a MAX_GRASS_BLADES of 1<<20 = 1.05 M
        // (particles/grass/grass.inl), which is itself sized to the world.
        // Headroom matters here because a chunk regenerated by world wrapping
        // spawns a *second* strand on a voxel that already has one: the sim only
        // frees a strand when its spawner voxel changes (sim.comp.glsl:44-51), and
        // a deterministic generator regenerates it identically. Inherited
        // behaviour, not introduced here, and bounded by the pool rather than by
        // VRAM -- but it is why these numbers are two orders under the cap.
        spawn_grass(v);
        return;
    }

    // 256 * 0.82 * 0.0028 = ~0.6 flowers/m^2, against MAX_FLOWERS 65536
    // (flower/flower.inl:12). This was 1.8/m^2 in the first pass and it was far too
    // many: a flower is ~0.4 m tall and the camera stands 1.7 m up, so at 0.6/m^2
    // they read as scattered colour and at 1.8 they read as a solid carpet with no
    // grass visible between them. Density is the one number to retune if the
    // island size changes.
    // The four hardcoded types are exactly the four colours in the reference
    // screenshots: tulip red, lavender purple, dandelion yellow, dandelion white.
    // Choosing by a 13 m noise field with a per-voxel jitter gives drifts of one
    // species rather than confetti.
    float fv = fbm2(voxel_pos.xy * 0.075) + (good_rand(voxel_pos.yx - 5.1) - 0.5) * 0.22;
    uint ft = FLOWER_TYPE_TULIP;
    if (fv > 0.44) {
        ft = FLOWER_TYPE_LAVENDER;
    }
    if (fv > 0.56) {
        ft = FLOWER_TYPE_DANDELION;
    }
    if (fv > 0.70) {
        ft = FLOWER_TYPE_DANDELION_WHITE;
    }
    spawn_flower(v, ft);
}

// -- the generator ------------------------------------------------------------

void brushgen_voxl_scene(in out Voxel voxel) {
    vec3 s = voxel_pos - VOXL_ORIGIN;

    // Cheap rejects first. Every voxel in the wrapping volume runs this function,
    // and the island occupies well under a tenth of it. The bounds are the
    // highest possible ground (hill 8.5 + rolling 0.8) and the deepest possible
    // underside (-11), with a metre of slack.
    if (s.z > 10.5 || s.z < -12.0) {
        return;
    }
    vec2 di = s.xy - VOXL_ISLAND_C;
    if (dot(di, di) > VOXL_ISLAND_R * VOXL_ISLAND_R) {
        return;
    }

    float h = voxl_ground_h(s.xy);

#if VOXL_DEBUG_BARE_HEIGHTFIELD
    // Bisection: nothing but the height field, in one flat colour. If a hole in the hill
    // survives this, it is not authored by this file -- everything that could carve one (the
    // cave, the props, the tree, the grass skin, every material branch) is gone.
    if (s.z < h && s.z > voxl_ground_base(s.xy)) {
        voxel.material_type = 1;
        voxel.roughness = 0.9;
        voxel.color = vec3(0.30, 0.06, 0.06);
    }
    return;
#endif

    // The cave SDF costs three fbm2 pairs; skip it outside the cave's box.
    float cave = 1e5;
    if (VOXL_DEBUG_NO_CAVE == 0 && all(lessThan(abs(s - VOXL_CAVE_BB_C), VOXL_CAVE_BB_R))) {
        cave = voxl_cave_sd(s);
    }

    if (cave < 0.0) {
        // Inside the void. The only thing here is the light.
        voxl_cave_light(voxel, s);
        return;
    }

    if (s.z < h && s.z > voxl_ground_base(s.xy)) {
        float depth = h - s.z;
        // Both of these are confined to voxels that can actually be seen. `bare`
        // decides the surface material, so it is pointless below 1.1 m; the
        // analytic normal is pointless below 0.5 m because those voxels are fully
        // occluded and the post-process nullifies their normals anyway -- and
        // writing one there would break the bit-uniformity that makes the buried
        // interior cost zero heap.
        float bare = (depth < 1.1) ? voxl_bare(s) : 0.0;
        vec3 nrm = vec3(0.0, 0.0, 1.0); // the default: engine derives it
        if (cave < VOXL_CAVE_WALL) {
            nrm = voxl_cave_nrm(s); // cave wall, at any depth below the surface
        } else if (depth < 0.5) {
            nrm = voxl_ground_nrm(s.xy);
        }
        voxl_rock_material(voxel, s, depth, cave, bare, nrm);
        return;
    }

    // Air above the ground. Three things can still fill it, in this order,
    // because they overlap: the tree wins over a pebble at its foot, and a pebble
    // wins over the grass skin it is sitting on.
    if (voxl_tree_voxel(voxel, s)) {
        return;
    }
    if (VOXL_DEBUG_NO_PROPS == 0 && s.z > h - 0.6 && s.z < h + 1.3 && voxl_props(voxel, s)) {
        return;
    }

    // The grass skin: exactly the one air voxel above the top solid voxel.
    if (s.z >= h && s.z < h + VOXEL_SIZE) {
        voxl_ground_cover(voxel, s, voxl_ground_nrm(s.xy), voxl_bare(s));
    }
}

void brushgen_planet(in out Voxel voxel) {
    if (length(voxel_pos) <= deref(gpu_input).sky_settings.atmosphere_bottom * 1000.0) {
        voxel.color = vec3(0.8);
        voxel.material_type = 1;
        voxel.roughness = 0.8;
    }
}

#define GEN_MODEL 0

void brushgen_world(in out Voxel voxel) {
    if (false) { // Mandelbulb world
        vec3 mandelbulb_color;
        if (mandelbulb((voxel_pos / 64 - 1) * 1, mandelbulb_color)) {
            voxel.color = vec3(0.02);
            voxel.material_type = 1;
            voxel.roughness = 0.5;
        }
    } else if (false) { // Solid world
        voxel.material_type = 1;
        voxel.color = vec3(0.5, 0.1, 0.8);
        voxel.roughness = 0.5;
    } else if (false) { // test
        float map_scale = 2.0;
        vec2 map_uv = voxel_pos.xy / (4097.0 * VOXEL_SIZE) / map_scale;

        const float offset = 1.0 / 512.0;
        vec4 heights = textureGather(daxa_sampler2D(test_texture, g_sampler_llc), map_uv);
        heights = heights * 4097.0 * VOXEL_SIZE - 128.0;
        heights = heights * map_scale * 0.6;
        vec2 w = fract(map_uv * 4097.0 - 0.5 + offset);
        float map_height = mix(mix(heights.w, heights.z, w.x), mix(heights.x, heights.y, w.x), w.y);
        vec3 map_color = texture(daxa_sampler2D(test_texture2, g_sampler_llc), map_uv).rgb;
        bool solid = voxel_pos.z < map_height;
        if (solid) {
            voxel.color = pow(map_color, vec3(2.2));
            voxel.material_type = 1;
            voxel.roughness = 0.99;

            vec3 pos_origin = floor(voxel_pos);
            pos_origin.z = heights.w;
            vec3 pos_down = pos_origin + vec3(0, map_scale * VOXEL_SIZE, 0);
            pos_down.z = heights.x;
            vec3 pos_right = pos_origin + vec3(map_scale * VOXEL_SIZE, 0, 0);
            pos_right.z = heights.z;
            vec3 vertical_dir = normalize(pos_origin - pos_down);
            vec3 horizontal_dir = normalize(pos_origin - pos_right);
            voxel.normal = normalize(cross(horizontal_dir, vertical_dir));
        }
    } else if (GEN_MODEL != 0) { // Model world
        uint packed_col_data = sample_gvox_palette_voxel(gvox_model, world_voxel, 0);
        // voxel.material_type = sample_gvox_palette_voxel(gvox_model, world_voxel, 0);
        voxel.color = uint_rgba8_to_f32vec4(packed_col_data).rgb;
        voxel.material_type = ((packed_col_data >> 0x18) != 0 || voxel.color != vec3(0)) ? 1 : 0;
        voxel.roughness = 0.9;

        // float test = length(vec3(1.0, 0.25, 0.0) - voxel.color);
        // if (test <= 0.7) {
        //     voxel.material_type = 3;
        //     voxel.roughness = test * 0.1;
        // }
        // uint packed_emi_data = sample_gvox_palette_voxel(gvox_model, world_voxel, 2);
        // if (voxel.material_type != 0) {
        //     voxel.material_type = 2;
        // }
        if (voxel_pos.z == -1.0 * VOXEL_SIZE) {
            voxel.color = vec3(0.1);
            voxel.material_type = 1;
        }

        // if (voxel.material_type == 1) {
        //     voxel.color = vec3(0.95, 0.05, 0.05);
        //     voxel.roughness = 0.001;
        // }
    } else if (false) { // Planet world
        brushgen_planet(voxel);
    } else if (VOXL_TEST_SCENE != 0) { // Voxl test scene -- see docs/SCENE.md
        brushgen_voxl_scene(voxel);
    } else if (true) { // Terrain world (the inherited gvox_engine demo)
        brushgen_world_terrain(voxel);
    } else if (true) { // Ball world (each ball is centered on a chunk center)
        if (length(fract(voxel_pos / 8) - 0.5) < 0.15) {
            voxel.material_type = 1;
            voxel.color = vec3(0.1);
            voxel.roughness = 0.5;
        }
    } else if (false) { // Checker board world
        uvec3 voxel_i = uvec3(voxel_pos / 8);
        if ((voxel_i.x + voxel_i.y + voxel_i.z) % 2 == 1) {
            voxel.material_type = 1;
            voxel.color = vec3(0.1);
            voxel.roughness = 0.5;
        }
    }
}

void brush_remove_grass(in out Voxel voxel) {
    float sd = sd_capsule(voxel_pos, brush_input.pos + brush_input.pos_offset, brush_input.prev_pos + brush_input.prev_pos_offset, 32.0 * VOXEL_SIZE);
    float diff = length(voxel.color - pow(vec3(85, 166, 78) / 255.0 * 0.5, vec3(2.2)));

    if (sd < 0 && voxel.material_type == 1 && diff < 0.025) {
        voxel.color = vec3(0, 0, 0);
        voxel.material_type = 0;
    }
    if (sd < 2.5 * VOXEL_SIZE) {
        voxel.normal = vec3(0, 0, 1);
    }
}

void brush_remove_ball(in out Voxel voxel) {
    float sd = sd_capsule(voxel_pos, brush_input.pos + brush_input.pos_offset, brush_input.prev_pos + brush_input.prev_pos_offset, 32.0 * VOXEL_SIZE);
    if (sd < 0) {
        voxel.color = vec3(0, 0, 0);
        voxel.material_type = 0;
    }
    if (sd < 2.5 * VOXEL_SIZE) {
        voxel.normal = vec3(0, 0, 1);
    }
}

void brushgen_a(in out Voxel voxel) {
    PackedVoxel voxel_data = sample_voxel_chunk(voxel_malloc_page_allocator, voxel_chunk_ptr, inchunk_voxel_i);
    Voxel prev_voxel = unpack_voxel(voxel_data);

    voxel.color = prev_voxel.color;
    voxel.material_type = prev_voxel.material_type;
    voxel.normal = prev_voxel.normal;
    voxel.roughness = prev_voxel.roughness;

    // brush_remove_grass(voxel);
    brush_remove_ball(voxel);
}

void brush_grass_ball(in out Voxel voxel) {
    float sd = sd_capsule(voxel_pos, brush_input.pos + brush_input.pos_offset, brush_input.prev_pos + brush_input.prev_pos_offset, 32.0 * VOXEL_SIZE);
    vec3 nrm = normalize(voxel_pos - (brush_input.pos + brush_input.pos_offset));
    if (sd < 0) {
        voxel.material_type = 1;
        voxel.color = vec3(0.95, 0.95, 0.95);
        voxel.roughness = 0.9;
    } else {
        float grass_val = sd_capsule(voxel_pos - vec3(0, 0, VOXEL_SIZE), brush_input.pos + brush_input.pos_offset, brush_input.prev_pos + brush_input.prev_pos_offset, 32.0 * VOXEL_SIZE);
        if (grass_val < 0.0) {
            try_spawn_grass(voxel, nrm);
        }
    }
    if (sd < 2.5 * VOXEL_SIZE) {
        voxel.normal = vec3(0, 0, 1);
    }
}
void brush_flowers(in out Voxel voxel) {
    float sd = sd_capsule(voxel_pos, brush_input.pos + brush_input.pos_offset, brush_input.prev_pos + brush_input.prev_pos_offset, 32.0 * VOXEL_SIZE);
    PackedVoxel temp_voxel_data = sample_voxel_chunk(VOXELS_BUFFER_PTRS, chunk_n, voxel_pos + vec3(0, 0, VOXEL_SIZE), vec3(0));
    Voxel above_voxel = unpack_voxel(temp_voxel_data);
    if (sd < 0 && voxel.material_type != 0 && above_voxel.material_type == 0) {
        float r2 = good_rand(voxel_pos.xy);
        if (r2 < 0.01) {
            voxel.color = pow(vec3(85, 166, 78) / 255.0 * 0.5, vec3(2.2));
            voxel.material_type = 1;
            voxel.roughness = 1.0;
            voxel.normal = vec3(0, 0, 1);
            spawn_flower(voxel, FLOWER_TYPE_DANDELION);
        }
    }
}
void brush_light_ball(in out Voxel voxel) {
    float sd = sd_capsule(voxel_pos, brush_input.pos + brush_input.pos_offset, brush_input.prev_pos + brush_input.prev_pos_offset, 32.0 * VOXEL_SIZE);
    vec3 nrm = normalize(voxel_pos - (brush_input.pos + brush_input.pos_offset));
    if (sd < 0) {
        voxel.material_type = 3;
        voxel.color = vec3(0.95, 0.15, 0.05);
        voxel.roughness = 0.9;
    }
    if (sd < 2.5 * VOXEL_SIZE) {
        voxel.normal = vec3(0, 0, 1);
    }
}
void brush_lantern(in out Voxel voxel) {
    float sd_housing = FLT_MAX;
    float sd_flame = FLT_MAX;

    vec3 lantern_c = brush_input.pos + brush_input.pos_offset;

    sd_housing = sd_union(sd_housing, sd_box_frame(voxel_pos - lantern_c - vec3(0, 0, 0.4), vec3((VOXEL_SIZE * 4).xx, 0.4), VOXEL_SIZE));
    sd_housing = sd_union(sd_housing, sd_box(voxel_pos - lantern_c, vec3((VOXEL_SIZE * 3).xx, VOXEL_SIZE)));
    sd_housing = sd_union(sd_housing, sd_box(voxel_pos - lantern_c - vec3(0, 0, 0.8), vec3((VOXEL_SIZE * 3).xx, VOXEL_SIZE)));
    sd_housing = sd_union(sd_housing, sd_box(voxel_pos - lantern_c - vec3(0, 0, 0.8 + VOXEL_SIZE * 1), vec3((VOXEL_SIZE * 1).xx, VOXEL_SIZE)));

    sd_flame = sd_union(sd_flame, sd_box(voxel_pos - lantern_c - vec3(0, 0, 0.4), vec3((VOXEL_SIZE * 3).xx, 0.4)));

    if (sd_housing < 0) {
        voxel.material_type = 1;
        voxel.color = vec3(0.05, 0.05, 0.05);
        voxel.roughness = 0.9;
        voxel.normal = vec3(0, 0, 1);
    } else if (sd_flame < 0) {
        voxel.material_type = 3;
        voxel.color = vec3(0.95, 0.35, 0.05);
        voxel.roughness = 0.9;
        voxel.normal = vec3(0, 0, 1);
    }
}
void brush_fire(in out Voxel voxel) {
    float sd_base = FLT_MAX;
    float sd_flame = FLT_MAX;

    vec3 lantern_c = brush_input.pos + brush_input.pos_offset;

    sd_base = sd_union(sd_base, sd_box(voxel_pos - lantern_c, vec3((VOXEL_SIZE * 3).xx, VOXEL_SIZE)));

    sd_flame = sd_union(sd_flame, sd_round_cone(voxel_pos - (lantern_c + vec3((VOXEL_SIZE * -0.5).xx, 0.2)), VOXEL_SIZE * 4, VOXEL_SIZE * 2, 0.4));
    float flame_rand = good_rand(voxel_pos);

    if (sd_base < 0) {
        voxel.material_type = 1;
        voxel.color = vec3(0.05, 0.05, 0.05);
        voxel.roughness = 0.9;
        voxel.normal = vec3(0, 0, 1);
    } else if (sd_flame < 0) {
        voxel.material_type = 3;
        voxel.color = vec3(0.95, 0.2 + floor((flame_rand + voxel_pos.z - lantern_c.z) * 2.0) * 0.05, 0.05);
        voxel.roughness = 0.3 + flame_rand * 0.3;
        voxel.normal = vec3(0, 0, 1);
        if (sd_flame > -VOXEL_SIZE) {
            spawn_fire_particle(voxel);
        }
    }
}
void brush_torch(in out Voxel voxel) {
    float sd_base = FLT_MAX;
    float sd_flame = FLT_MAX;

    vec3 lantern_c = brush_input.pos + brush_input.pos_offset;

    sd_base = sd_union(sd_base, sd_box(voxel_pos - (lantern_c + vec3(0, 0, 1.0)), vec3((VOXEL_SIZE * 2.5).xx, VOXEL_SIZE)));
    sd_base = sd_union(sd_base, sd_box(voxel_pos - (lantern_c + vec3(0, 0, 0.5)), vec3((VOXEL_SIZE * 1.5).xx, 0.5)));

    sd_flame = sd_union(sd_flame, sd_round_cone(voxel_pos - (lantern_c + vec3(0, 0, 1.0 + VOXEL_SIZE * 2)), VOXEL_SIZE * 2.0, VOXEL_SIZE * 0.5, 0.2));
    float flame_rand = good_rand(voxel_pos);

    if (sd_base < 0) {
        voxel.material_type = 1;
        voxel.color = vec3(.68, .4, .15) * 0.16;
        voxel.roughness = 0.9;
        voxel.normal = vec3(0, 0, 1);
    } else if (sd_flame < 0) {
        voxel.material_type = 3;
        voxel.color = vec3(0.95, 0.15, 0.05);
        voxel.roughness = 0.3 + flame_rand * 0.3;
        voxel.normal = vec3(0, 0, 1);

        if (sd_flame > -VOXEL_SIZE) {
            spawn_fire_particle(voxel);
        }
    }
}

void sd_maple_branch(in out TreeSDFNrm val, in vec3 p, in vec3 origin, in vec3 dir, in float scl) {
    float upwards_curl_factor = 0.2;
    vec3 bp0 = origin;
    for (uint segment_i = 0; segment_i < 4; ++segment_i) {
        vec3 bp1 = bp0 + dir * scl + vec3(0, 0, upwards_curl_factor);
        upwards_curl_factor += 0.2;
        val.wood = sd_union(val.wood, sd_capsule(p, bp0, bp1, 0.10));
        bp0 = bp1;
        if (segment_i < 2)
            continue;
        float leaves_dist = sd_sphere(p - bp1, scl * 0.4 + 0.6);
        if (leaves_dist < val.leaves) {
            val.leaves = leaves_dist;
            val.leaves_nrm = normalize(p - bp1);
        }
        // val.leaves = sd_smooth_union(val.leaves, leaves_dist, 1.0);
    }
}

TreeSDFNrm sd_maple_tree(in vec3 p, in vec3 seed) {
    TreeSDFNrm val = TreeSDFNrm(1e5, 1e5, vec3(0, 0, 1), vec3(0, 0, 1));

    float sd_trunk_base = sd_round_cone(
        p,
        vec3(0, 0, 0),
        vec3(0, 0, 5),
        0.5, 0.4);
    float sd_trunk_mid = sd_round_cone(
        p,
        vec3(0, 0, 5),
        vec3(0, 0, 8),
        0.4, 0.41);
    float sd_trunk_top = sd_round_cone(
        p,
        vec3(0, 0, 5),
        vec3(0, 0, 16),
        0.41, 0.2);

    val.wood = sd_union(sd_trunk_base, sd_union(sd_trunk_mid, sd_trunk_top));

    for (uint i = 0; i < 7; ++i) {
        float scl = (1 - 0.05 * pow(i, 2)) * 0.02 * pow(i, 2) + 1.6 - i * 0.13;
        uint branch_n = 8 - i / 2;
        for (uint branch_i = 0; branch_i < branch_n; ++branch_i) {
            float angle = (1.0 / branch_n * branch_i) * 2.0 * M_PI + good_rand(seed + i + 1.0 * branch_i) * 0.5 + branch_i * 10;
            float branch_base = 4.0 + i * 1.8 + branch_i * 0.1;
            vec3 dir = normalize(vec3(cos(angle), sin(angle), +0.0));
            sd_maple_branch(val, p, vec3(0, 0, branch_base), dir, scl);
        }
    }
    return val;
}

void brush_maple_tree(in out Voxel voxel) {
    vec3 tree_pos = brush_input.pos + brush_input.pos_offset;

    float tree_size = good_rand(tree_pos);
    float space_scl = 1.5 - tree_size * 0.5;
    TreeSDFNrm tree = sd_maple_tree((voxel_pos - tree_pos) * space_scl, tree_pos);
    tree.wood /= space_scl;
    tree.leaves /= space_scl;

    float leaf_rand = good_rand(voxel_pos);

    uint prev_mat_type = voxel.material_type;

    if (tree.wood < 0) {
        voxel.material_type = 1;
        voxel.color = vec3(.68, .4, .15) * 0.16;
        voxel.roughness = 0.99;
        voxel.normal = vec3(0, 0, 1);
    } else if (tree.leaves * 5.0 + leaf_rand * 15.0 < 0) {
        voxel.material_type = 1;
        voxel.color = vec3(.28, .8, .15) * 0.5;
        voxel.roughness = 0.95;
        voxel.normal = tree.leaves_nrm;
        if (tree.leaves - leaf_rand > -VOXEL_SIZE) {
            // should be a particle spawner
            // voxel.color = vec3(.9, .1, .9);
            if (prev_mat_type == 0) {
                spawn_tree_particle(voxel);
            }
        }
    }
}

void brush_spruce_tree(in out Voxel voxel) {
    TreeSDF tree = sd_spruce_tree(voxel_pos - brush_input.pos, brush_input.pos);

    if (tree.wood < 0) {
        voxel.material_type = 1;
        voxel.color = vec3(.68, .4, .15) * 0.16;
        voxel.roughness = 0.99;
        voxel.normal = vec3(0, 0, 1);
    } else if (tree.leaves < 0) {
        voxel.material_type = 1;
        voxel.color = vec3(.28, .8, .15) * 0.5;
        voxel.roughness = 0.95;
        voxel.normal = vec3(0, 0, 1);
    }
}

void brushgen_b(in out Voxel voxel) {
    PackedVoxel voxel_data = sample_voxel_chunk(voxel_malloc_page_allocator, voxel_chunk_ptr, inchunk_voxel_i);
    Voxel prev_voxel = unpack_voxel(voxel_data);

    voxel.color = prev_voxel.color;
    voxel.material_type = prev_voxel.material_type;
    voxel.normal = prev_voxel.normal;
    voxel.roughness = prev_voxel.roughness;

    // brush_grass_ball(voxel);
    // brush_flowers(voxel);

    // brush_light_ball(voxel);
    // brush_lantern(voxel);
    brush_fire(voxel);
    // brush_torch(voxel);

    // brush_maple_tree(voxel);
    // brush_spruce_tree(voxel);
}
