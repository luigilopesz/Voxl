#pragma once

// Break/place interaction: target selection, the progressive break timer, the
// sub-voxel mining mode, the placement legality rules, and the wireframe
// selection box.
//
// THE MINING VERB
// ---------------
// Left click is unchanged by default: hold to break the whole targeted block,
// timed by BlockType::hardness. `MiningMode::SubVoxel` is a mode the player
// opts into (see gameplay/MiningTool.hpp), in which the same button carves a
// brush-shaped hole out of the block instead of removing it. Nothing about the
// default path is conditional on the mode existing - with no sub-voxel hook
// installed the mode degrades to ordinary breaking.
//
// DEPENDENCY NOTE FOR THE INTEGRATOR
// ----------------------------------
// This module does not include src/physics/Raycast.hpp. The frozen contract set
// does not cover the raycast, so instead of guessing its shape this class takes
// the ray query as a callable (`RaycastFn`) and declares the small result type
// it needs (`InteractionHit`). Wiring is a one-line adapter, e.g.
//
//     interaction.setRaycaster([&](const glm::vec3& o, const glm::vec3& d, float r,
//                                 voxl::InteractionHit& out) {
//         const voxl::RayHit hit = voxl::raycastVoxels(world, registry, o, d, r);
//         if (!hit.hit) { return false; }
//         out = {hit.block, hit.blockId, hit.face, hit.point, hit.distance};
//         return true;
//     });
//
// The world writer, reader, sub-voxel carver and damage probe are injected the
// same way, so interaction has no compile-time dependency on the World type
// either. Every hook is optional: with none set, `update()` reports "no target"
// and `render()` draws nothing.
//
// Thread safety: none. Main-thread only - it reads the camera, mutates the world
// and issues GL calls.

#include "gameplay/MiningTool.hpp"
#include "render/Camera.hpp"
#include "world/Block.hpp"
#include "world/SubVoxel.hpp"
#include "world/VoxelTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace voxl {

/// One voxel selected by the interaction ray.
struct InteractionHit {
    /// World-space voxel coordinate of the block that was hit.
    BlockPos block{};
    BlockId  blockId = blocks::Air;
    /// Face of `block` the ray entered through. `block + offset(face)` is the
    /// placement candidate.
    Direction face = Direction::PosY;
    /// Exact world-space entry point, for particle/sound spawning.
    glm::vec3 point{0.0f};
    /// Distance from the ray origin, in blocks.
    float distance = 0.0f;
};

/// Ray query against the voxel world. Must skip air and liquids (see
/// docs/TECHNICAL_DESIGN.md §7) and return false when nothing is within reach.
using RaycastFn = std::function<bool(const glm::vec3& origin, const glm::vec3& direction,
                                    float maxDistance, InteractionHit& outHit)>;

/// Writes one voxel. Returns false when the chunk is not resident or the write
/// was refused, which the caller surfaces as a failed edit rather than retrying.
using BlockWriteFn = std::function<bool(const BlockPos& position, BlockId id)>;

/// Reads one voxel. Used only to check whether the placement cell is free.
using BlockReadFn = std::function<BlockId(const BlockPos& position)>;

/// What the world did with one requested carve.
///
/// Two values, not five, because the only decision this module makes on the
/// answer is "did that sub-voxel come out". Mapping World::EditResult onto it is
/// the integrator's one-liner and keeps World.hpp out of this header:
///
///     result == EditResult::Applied || result == EditResult::Deferred
///         ? CarveOutcome::Carved : CarveOutcome::Refused
///
/// Deferred counts as carved on purpose: a deferred edit is accepted and will be
/// replayed, and re-issuing it next frame would double-apply it.
enum class CarveOutcome : std::uint8_t {
    Carved  = 0,
    Refused = 1,
};

/// Carves the sub-voxel containing `worldPoint`. Maps onto
/// World::breakSubVoxelAt.
///
/// The point form rather than a block-plus-index pair because a brush spills
/// across block boundaries by design, and letting the world do the one floor
/// division keeps a single implementation of that split. This module always
/// passes the exact CENTRE of a sub-voxel cell, so the division can never land
/// on a boundary.
using SubVoxelBreakFn = std::function<CarveOutcome(const glm::vec3& worldPoint)>;

/// True when the block at `position` is partially destroyed. Maps onto
/// `!World::isBlockWhole(position)`. Optional; without it, damaged cells are
/// indistinguishable from whole ones, which placement already refuses.
using SubVoxelDamagedFn = std::function<bool(const BlockPos& position)>;

/// Per-frame button state, already debounced by the input layer.
struct InteractionInput {
    /// Left mouse held: drives the break timer.
    bool breakHeld = false;
    /// Right mouse edge: places immediately.
    bool placePressed = false;
    /// Right mouse held: places again every kPlaceRepeatSeconds.
    bool placeHeld = false;
};

/// Why the last placement attempt did or did not happen.
enum class PlaceResult : std::uint8_t {
    None = 0,          ///< no attempt this frame
    Placed,            ///< voxel written
    NoTarget,          ///< the ray hit nothing
    OutOfWorld,        ///< candidate cell is above or below the world
    Occupied,          ///< candidate cell holds a non-replaceable block
    Damaged,           ///< candidate cell holds a partially destroyed block
    IntersectsPlayer,  ///< would trap the player inside the new block
    NothingHeld,       ///< held block is air
    NoWorldSink,       ///< no BlockWriteFn installed
    WriteFailed,       ///< the world refused the write
};

/// Why the break timer is in the state it is.
enum class BreakResult : std::uint8_t {
    None = 0,      ///< not breaking
    InProgress,    ///< timer running
    Broken,        ///< the whole voxel was removed this frame
    Carved,        ///< sub-voxel mode removed at least one sub-voxel this frame
    CarveRefused,  ///< the world refused every sub-voxel of the brush
    NoTarget,      ///< the ray hit nothing
    Unbreakable,   ///< hardness < 0
    NoWorldSink,   ///< no BlockWriteFn installed
    WriteFailed,
};

[[nodiscard]] constexpr const char* toString(PlaceResult result) noexcept
{
    switch (result) {
        case PlaceResult::None:             return "None";
        case PlaceResult::Placed:           return "Placed";
        case PlaceResult::NoTarget:         return "NoTarget";
        case PlaceResult::OutOfWorld:       return "OutOfWorld";
        case PlaceResult::Occupied:         return "Occupied";
        case PlaceResult::Damaged:          return "Damaged";
        case PlaceResult::IntersectsPlayer: return "IntersectsPlayer";
        case PlaceResult::NothingHeld:      return "NothingHeld";
        case PlaceResult::NoWorldSink:      return "NoWorldSink";
        case PlaceResult::WriteFailed:      return "WriteFailed";
    }
    return "Unknown";
}

[[nodiscard]] constexpr const char* toString(BreakResult result) noexcept
{
    switch (result) {
        case BreakResult::None:         return "None";
        case BreakResult::InProgress:   return "InProgress";
        case BreakResult::Broken:       return "Broken";
        case BreakResult::Carved:       return "Carved";
        case BreakResult::CarveRefused: return "CarveRefused";
        case BreakResult::NoTarget:     return "NoTarget";
        case BreakResult::Unbreakable:  return "Unbreakable";
        case BreakResult::NoWorldSink:  return "NoWorldSink";
        case BreakResult::WriteFailed:  return "WriteFailed";
    }
    return "Unknown";
}

/// Everything the HUD and the debug overlay need to know about this frame's
/// interaction. Read-only snapshot; valid until the next `update()`.
struct InteractionState {
    bool           hasTarget = false;
    InteractionHit hit{};

    /// Cell a placement would fill (`hit.block` offset along `hit.face`).
    BlockPos placeTarget{};
    /// True when a placement there would be accepted right now.
    bool placeAllowed = false;

    /// 0 when not breaking, rising to 1 the instant the block gives way.
    float breakProgress = 0.0f;
    /// -1 when not breaking, otherwise 0..kBreakStageCount-1. Exposed so a crack
    /// overlay texture can be indexed by it once the texture array carries one.
    int breakStage = -1;

    /// True when the target's hardness is the "never breaks" sentinel.
    bool targetUnbreakable = false;

    // ---- mining mode ----

    /// Mode the verb is in, mirrored here so the HUD needs only this snapshot.
    MiningMode miningMode = MiningMode::WholeBlock;
    float      brushRadius = MiningTool::kDefaultBrushRadius;
    /// Sub-voxels one completed carve touches.
    std::size_t brushVolume = 1;

    /// Sub-voxel under the crosshair, valid only while `hasSubTarget`. Local
    /// coordinates within `hit.block`, each component in [0, 8).
    bool         hasSubTarget = false;
    glm::ivec3   subTarget{0};

    /// True while the mode is SubVoxel but this target cannot be carved, so the
    /// swing fell back to a whole-block break. The two reasons are a non-opaque
    /// material (World::editSubVoxel refuses those - the sub-voxel pass has no
    /// alpha cutoff) and no carve hook being installed at all.
    bool subVoxelFallback = false;

    /// Sub-voxels of the last completed carve the world accepted. Lower than
    /// `brushVolume` whenever the brush spilled into air or into a block that
    /// cannot be carved, which is the common case near a block face.
    std::size_t lastCarveCount = 0;

    PlaceResult lastPlace = PlaceResult::None;
    BreakResult lastBreak = BreakResult::None;
};

class BlockInteraction {
public:
    /// Arm's length in blocks. 5 is short enough that the selection box is
    /// unambiguous at the crosshair and long enough to build a wall from the
    /// ground.
    static constexpr float kDefaultReachBlocks = 5.0f;

    /// Seconds of held-button time per unit of BlockType::hardness. With the
    /// default registry this makes leaves ~0.1 s, stone ~0.75 s and planks 1 s.
    static constexpr float kBreakSecondsPerHardness = 0.5f;

    /// Number of discrete crack stages. Ten matches the classic destroy_stage
    /// texture strip, so the overlay can be dropped in without retuning.
    static constexpr int kBreakStageCount = 10;

    /// Auto-repeat interval while the place button stays down.
    static constexpr float kPlaceRepeatSeconds = 0.25f;

    explicit BlockInteraction(const BlockRegistry& registry) noexcept;
    ~BlockInteraction();

    // Owns GL objects whose lifetime is tied to `this`, and the destructor
    // deletes them, so copying or moving would double-delete.
    BlockInteraction(const BlockInteraction&)            = delete;
    BlockInteraction& operator=(const BlockInteraction&) = delete;
    BlockInteraction(BlockInteraction&&)                 = delete;
    BlockInteraction& operator=(BlockInteraction&&)      = delete;

    // ---- wiring ----

    void setRaycaster(RaycastFn raycaster);
    void setBlockWriter(BlockWriteFn writer);
    void setBlockReader(BlockReadFn reader);
    void setSubVoxelBreaker(SubVoxelBreakFn carve);
    void setSubVoxelDamageReader(SubVoxelDamagedFn damaged);

    [[nodiscard]] bool hasRaycaster() const noexcept { return static_cast<bool>(m_raycast); }
    [[nodiscard]] bool hasWorldSink() const noexcept { return static_cast<bool>(m_write); }
    [[nodiscard]] bool hasSubVoxelSink() const noexcept { return static_cast<bool>(m_carve); }

    // ---- mining mode ----

    [[nodiscard]] const MiningTool& tool() const noexcept { return m_tool; }

    [[nodiscard]] MiningMode miningMode() const noexcept { return m_tool.mode(); }

    /// Changing mode abandons any swing in progress. Progress belongs to a
    /// (target, mode) pair: carrying a half-finished whole-block break over into
    /// a carve would remove a sub-voxel the player never aimed at.
    void setMiningMode(MiningMode mode) noexcept;
    MiningMode toggleMiningMode() noexcept;

    [[nodiscard]] float brushRadius() const noexcept { return m_tool.brushRadius(); }
    void setBrushRadius(float subVoxels) noexcept;
    /// Wheel-style relative control; positive widens the brush.
    void adjustBrushRadius(int steps) noexcept;

    void  setReach(float blocks) noexcept;
    [[nodiscard]] float reach() const noexcept { return m_reach; }

    /// World-space player collision box, used to veto self-trapping placements.
    /// Until this is set (or after `clearPlayerAabb()`), the veto is skipped and
    /// `InteractionState::placeAllowed` only reflects the cell's contents - the
    /// graceful-degradation path for a build with no player physics yet.
    void setPlayerAabb(const Aabb& box) noexcept;
    void clearPlayerAabb() noexcept;
    [[nodiscard]] bool hasPlayerAabb() const noexcept { return m_hasPlayerAabb; }

    /// The block a place action would put down; normally driven from the hotbar.
    void setHeldBlock(BlockId id) noexcept { m_heldBlock = id; }
    [[nodiscard]] BlockId heldBlock() const noexcept { return m_heldBlock; }

    // ---- per-frame ----

    /// Re-targets the ray, advances the break timer and applies edits. Call once
    /// per frame after the camera has its final transform for the frame.
    void update(const Camera& camera, const InteractionInput& input, float deltaSeconds);

    [[nodiscard]] const InteractionState& state() const noexcept { return m_state; }

    /// Draws the selection outline plus, while breaking, an inner box that
    /// shrinks towards the block centre. Requires a current GL context; silently
    /// does nothing if the GL entry points it needs are unavailable, if there is
    /// no target, or if the outline is hidden.
    void render(const glm::mat4& viewProjection);

    void setSelectionVisible(bool visible) noexcept { m_selectionVisible = visible; }
    [[nodiscard]] bool selectionVisible() const noexcept { return m_selectionVisible; }

    /// Frees the GL objects. Called by the destructor; call it explicitly when
    /// the context is destroyed before this object is.
    void releaseGpuResources() noexcept;

private:
    /// What a swing in progress is aimed at.
    ///
    /// The sub-voxel is part of the identity, not just the block: in drill mode
    /// the thing being removed is the sub-voxel, so sliding the crosshair onto
    /// the one next door starts a new (short) swing. Keeping the rule uniform -
    /// progress belongs to whatever you are pointing at - is what makes "release
    /// or look away and you start over" true in both modes.
    struct BreakTarget {
        BlockPos   block{};
        glm::ivec3 sub{0};
        /// A whole-block swing and a carve at the same place are not the same
        /// swing, so the mode has to take part in the comparison.
        bool carving = false;

        [[nodiscard]] bool operator==(const BreakTarget& other) const noexcept
        {
            return block == other.block && carving == other.carving &&
                   (!carving || sub == other.sub);
        }
    };

    /// Applies the placement rules: something is held, inside the world, the
    /// cell is replaceable and undamaged, and the resulting cube does not
    /// overlap the player.
    [[nodiscard]] PlaceResult evaluatePlacement(const BlockPos& target) const noexcept;

    void resetBreakProgress() noexcept;

    /// Fills `m_state.hasSubTarget` / `m_state.subTarget` from the ray hit.
    void updateSubTarget() noexcept;

    /// True when a carve against the current target would be accepted by the
    /// world's material rule, so the swing should drill rather than break.
    [[nodiscard]] bool canCarveTarget() const noexcept;

    /// Issues the whole brush and returns how many sub-voxels came out.
    [[nodiscard]] std::size_t applyBrush();

    /// Says once, and only when the block changes, that this material cannot be
    /// drilled. Called every frame the player holds the button against glass, so
    /// it cannot log unconditionally.
    void noteFallback(const BlockPos& block, BlockId id);

    class SelectionRenderer;  // GL-owning, defined in the .cpp

    const BlockRegistry* m_registry = nullptr;

    RaycastFn         m_raycast;
    BlockWriteFn      m_write;
    BlockReadFn       m_read;
    SubVoxelBreakFn   m_carve;
    SubVoxelDamagedFn m_damaged;

    MiningTool m_tool;

    float   m_reach     = kDefaultReachBlocks;
    BlockId m_heldBlock = blocks::Stone;

    Aabb m_playerAabb{};
    bool m_hasPlayerAabb = false;

    InteractionState m_state{};

    /// What the break timer belongs to. Moving the crosshair off it resets the
    /// timer, so a player cannot chip away at several targets in parallel.
    BreakTarget m_breakTarget{};
    bool        m_breakingValid    = false;
    float       m_breakElapsed     = 0.0f;
    float       m_placeCooldown    = 0.0f;
    bool        m_selectionVisible = true;

    /// Last block reported as undrillable, so the message is printed once per
    /// block rather than once per frame.
    BlockPos m_loggedFallbackBlock{};
    bool     m_loggedFallback = false;

    std::unique_ptr<SelectionRenderer> m_selection;
};

}  // namespace voxl
