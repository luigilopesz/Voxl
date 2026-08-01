#pragma once

// =================================================================================================
// THE BRUSH CONTRACT, shared verbatim between C++ and GLSL.
//
// This header is the whole interface between the application's tool UI and the two compute stages
// that apply an edit. It lives here, rather than in a shader, because both sides need the same
// names: the shader needs the id to dispatch on, and the C++ side needs it to select a tool, label
// a HUD and persist a choice. Everything below is either a plain integer #define or a struct of
// 4-byte scalars, so it parses identically under both compilers.
//
// LAYOUT. Daxa compiles every shared struct under GL_EXT_scalar_block_layout (see
// deps/Daxa/include/daxa/daxa.glsl -- `layout(buffer_reference, scalar, ...)` and
// `layout(push_constant, scalar)`). Under scalar layout a member's alignment is its *scalar*
// component's alignment, so daxa_f32vec3 is 12 bytes aligned to 4 -- exactly what the C++ struct
// does -- and there is no vec3-to-vec4 padding to reason about. The rule that remains is simply
// KEEP EVERY MEMBER 4 BYTES WIDE AND IN THE SAME ORDER. BrushInput is 60 bytes on both sides.
// =================================================================================================

// -- the tool set ---------------------------------------------------------------------------
//
// Ids are contiguous from 0 so the shader can `% BRUSH_ID_COUNT` to cycle and the C++ side can
// index an array of labels. ORDER IS PART OF THE INTERFACE: it is the order the tool cycles in,
// so the two terrain tools lead, the vegetation tools follow, then the lights, then the trees --
// destructive-and-cheap first, expensive-and-one-shot last.
//
// Every one of these except ADD_BALL already existed in brushes.glsl and was reachable only by
// editing the file and recompiling. ADD_BALL is new: the engine had a remove with no counterpart,
// so it could subtract terrain but never add any.
#define BRUSH_ID_REMOVE_BALL 0  // carve a sphere/capsule of terrain away
#define BRUSH_ID_ADD_BALL 1     // fill one in with stone
#define BRUSH_ID_GRASS_BALL 2   // white ball, grass strands on the shell above it
#define BRUSH_ID_REMOVE_GRASS 3 // strip the grass skin, leave the ground
#define BRUSH_ID_FLOWERS 4      // scatter flower particles on lit ground
#define BRUSH_ID_LIGHT_BALL 5   // emissive sphere
#define BRUSH_ID_LANTERN 6      // framed lantern, dark housing + flame
#define BRUSH_ID_FIRE 7         // campfire with fire particles
#define BRUSH_ID_TORCH 8        // wall/ground torch
#define BRUSH_ID_MAPLE_TREE 9   // broadleaf, ~200 capsule SDFs per voxel -- see BRUSH_ONE_SHOT_MASK
#define BRUSH_ID_SPRUCE_TREE 10 // conifer
#define BRUSH_ID_COUNT 11

// -- one-shot tools -------------------------------------------------------------------------
//
// A held button paints a stroke; that is right for terrain and grass and absurd for a tree --
// sixty maples a second, each re-running its own SDF over ~100 chunks. Tools in this mask fire
// exactly once per press (the chunk-election shader gates them on
// brush_state.initial_frame == frame_index). Placement props are one-shot for the same reason:
// they are objects, not paint.
#define BRUSH_ONE_SHOT_MASK (                    \
    (1u << BRUSH_ID_LANTERN) |                   \
    (1u << BRUSH_ID_FIRE) |                      \
    (1u << BRUSH_ID_TORCH) |                     \
    (1u << BRUSH_ID_MAPLE_TREE) |                \
    (1u << BRUSH_ID_SPRUCE_TREE))
#define BRUSH_ID_IS_ONE_SHOT(id) (((BRUSH_ONE_SHOT_MASK >> (id)) & 1u) != 0u)
// How many frames from the start of a press a one-shot tool keeps firing. Not 1: see the long
// comment at the gate in voxels/impl/voxel_world.comp.glsl. These brushes are idempotent, so the
// only cost of repeating is the chunk regeneration, and the cost of NOT repeating is a placement
// that silently does not happen.
#define BRUSH_ONE_SHOT_FRAMES 4u

// -- the secondary button -----------------------------------------------------------------------
//
// Two independent bindings survive (BRUSH_FLAGS_USER_BRUSH_A / _B in utilities/gpu/defs.glsl).
// Primary applies the selected tool; secondary applies its inverse, which for everything except
// "remove terrain" means removing terrain. This is deliberately NOT the selected tool's own
// geometry: the secondary is always a plain ball of the current radius, which keeps it predictable
// for the player and keeps the chunk-election box for button B a single cheap case.
#define BRUSH_SECONDARY_ID(id) ((id) == BRUSH_ID_REMOVE_BALL ? BRUSH_ID_ADD_BALL : BRUSH_ID_REMOVE_BALL)

// -- radius ---------------------------------------------------------------------------------
//
// `radius` is the tool's half-extent in METRES. For the ball-shaped tools it is literally the
// sphere radius; for the props and trees it is the half-width of their footprint (see
// BRUSH_TREE_* below). 2.0 m is the default because that is 32 * VOXEL_SIZE -- the value every
// brush in brushes.glsl used to hardcode -- so the shipped feel is unchanged at the default.
//
// THE CEILING IS ARITHMETIC, NOT TASTE. try_elect() in voxel_world.comp.glsl hands at most
// MAX_CHUNK_UPDATES_PER_FRAME (128) chunks to the edit pass per frame and *silently drops the
// rest*, so an over-large tool does not fail loudly, it produces a half-applied edit. A chunk is
// CHUNK_WORLDSPACE_SIZE = 4 m, a sphere of radius r spans at most ceil(2r/4)+1 chunks per axis,
// and 5^3 = 125 is the largest cube that still fits under 128. r = 8 m is the largest radius that
// stays inside a 5-chunk span. Anything above it would be a lie.
#define BRUSH_RADIUS_MIN 0.125f   // 2 voxels; below this the capsule can miss every voxel centre
#define BRUSH_RADIUS_MAX 8.0f     // see above: 5^3 = 125 <= MAX_CHUNK_UPDATES_PER_FRAME
#define BRUSH_RADIUS_DEFAULT 2.0f // == 32 * VOXEL_SIZE, the size every brush hardcoded before
#define BRUSH_RADIUS_STEP 1.25f   // one wheel notch, MULTIPLICATIVE -- see perframe.comp.glsl

// -- reach ----------------------------------------------------------------------------------
//
// How far in front of the camera the tool may land when the picking ray does not return a usable
// distance. BOTH BOUNDS ARE LOAD-BEARING, and the lower one is a bug fix rather than a nicety:
// voxel_trace() only advances its ray_pos when it actually hits something (voxels/impl/trace.glsl
// :123-128), so on a miss `length(ray_pos - cam_pos)` is ZERO and the brush point lands inside the
// camera. With the engine's old hardcoded remove-ball that merely dug a hole around the player's
// feet and nobody noticed; with an add brush it entombs them in stone, which is how it was found.
// The upper bound is the tool's reach, and also what it does when aimed at open sky -- the trace
// reports MAX_DIST = 1e9 there, and 1e9 fed to a chunk index is not a number anything downstream
// should see.
#define BRUSH_PICK_MIN_M 1.0f
#define BRUSH_PICK_MAX_M 24.0f

// Chunks a single edit may span, per axis, and the world-space extent that guarantees. A span of
// N chunks contains a box of (N-1) * 4 m whatever its alignment, so 5 chunks guarantees 16 m.
// perframe.comp.glsl shortens the drag capsule to fit this, which is what keeps the region the
// election shader picks and the region the edit shader draws the same region.
#define BRUSH_MAX_CHUNK_SPAN 5
#define BRUSH_MAX_SPAN_M 16.0f // (BRUSH_MAX_CHUNK_SPAN - 1) * CHUNK_WORLDSPACE_SIZE

// -- trees ------------------------------------------------------------------------------------
//
// A tree is not a ball and cannot pretend to be one: it is anchored at its base, grows upward, and
// its crown is wider than its trunk. `radius` is taken as the CROWN RADIUS and the height follows
// from it. The cap exists because a tree must still fit BRUSH_MAX_SPAN_M: at the cap the crown is
// 12 m across and the trunk 12 m tall, both inside 16 m.
#define BRUSH_TREE_RADIUS_MAX 6.0f
#define BRUSH_TREE_HEIGHT_PER_RADIUS 2.0f
// How far below the anchor a tree may still write, in metres. Roots and the base flare need a
// little, and the anchor lands on a surface that the brush point may sit slightly above.
#define BRUSH_TREE_DOWN_M 1.5f

// -- BrushInput.flags ---------------------------------------------------------------------------
// Bit 0 is the edge-detect latch for the tool-cycle key. It lives in the persistent globals buffer
// because GLFW reports a held key as a LEVEL (1 on press, 2 once auto-repeat starts, 0 on release)
// and a shader has nowhere else to remember last frame's value. Without it, one press of the cycle
// key runs through the entire tool list.
#define BRUSH_INPUT_FLAG_CYCLE_LATCH 1u

// -- who drives the selection -------------------------------------------------------------------
//
// 0 (today): perframe.comp.glsl drives it -- the tool-cycle action steps brush_id and the mouse
//   wheel scales radius. Self-contained, so the editing system works without any C++ change.
// 1: the C++ tool UI drives it. To land that side, add `BrushSelection brush_selection;` to
//   GpuInput (application/input.inl), fill it from the tool UI each frame, and flip this to 1.
//   perframe.comp.glsl already contains the read, behind the #if. Nothing else changes, and the
//   ids and radius bounds the C++ side writes are the ones defined above.
// Flipped to 1 by the tool-UI workstream, which is the handshake this comment asks for:
// GpuInput::brush_selection exists (application/input.inl) and ToolBelt::perframe()
// (application/ui_tools.cpp) fills it every frame from the belt. With this at 1 the #else branch
// below is compiled out, so the wheel and the tool-cycle action are serviced once, on the CPU,
// rather than by both sides at once. This is the ONLY line in this file that workstream touched.
#define BRUSH_SELECTION_FROM_CPU 1

struct BrushSelection {
    daxa_u32 brush_id; // one of BRUSH_ID_*
    daxa_f32 radius;   // metres, in [BRUSH_RADIUS_MIN, BRUSH_RADIUS_MAX]
};

struct BrushInput {
    // The stroke. `pos` is this frame's brush point and `prev_pos` last frame's; the brushes draw
    // a CAPSULE between them, which is what makes a dragged tool paint a continuous stroke instead
    // of a dotted line of spheres. Each carries its own integer-metre offset because the world
    // origin moves with the player and the two frames can straddle a unit boundary.
    daxa_f32vec3 pos;
    daxa_i32vec3 pos_offset;
    daxa_f32vec3 prev_pos;
    daxa_i32vec3 prev_pos_offset;
    // The tool. Written once per frame by perframe.comp.glsl and then copied, unchanged, into
    // every VoxelChunkUpdateInfo the edit touches.
    daxa_u32 brush_id; // BRUSH_ID_*
    daxa_f32 radius;   // metres
    daxa_u32 flags;    // BRUSH_INPUT_FLAG_*
};

struct PackedVoxel {
    daxa_u32 data;
};

struct Voxel {
    daxa_u32 material_type; // 2 bits (empty, dielectric, metallic, emissive)
    daxa_f32 roughness;     // 4 bits
    daxa_f32vec3 normal;    // 8 bits
    daxa_f32vec3 color;     // 18 bits
};

#if defined(__cplusplus)
// Labels for the tool UI, indexed by BRUSH_ID_*. Here rather than in the UI file so the array and
// the enumeration cannot drift apart -- add an id above and this fails to compile until it is
// named. (static_assert on the size does the enforcing.)
inline constexpr char const *BRUSH_ID_NAMES[BRUSH_ID_COUNT] = {
    "Remove terrain",
    "Add terrain",
    "Grass",
    "Remove grass",
    "Flowers",
    "Light ball",
    "Lantern",
    "Fire",
    "Torch",
    "Maple tree",
    "Spruce tree",
};
static_assert(sizeof(BRUSH_ID_NAMES) / sizeof(BRUSH_ID_NAMES[0]) == BRUSH_ID_COUNT,
              "BRUSH_ID_NAMES must name every BRUSH_ID_*");
// The GLSL side is laid out by GL_EXT_scalar_block_layout, which packs exactly as C++ does for a
// struct of 4-byte scalars. If this ever fires, someone added a member that is not 4 bytes wide.
static_assert(sizeof(BrushInput) == 60, "BrushInput must stay 15 tightly-packed 4-byte scalars");
#endif
