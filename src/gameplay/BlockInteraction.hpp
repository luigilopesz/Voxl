#pragma once

// Break/place interaction: target selection, the progressive break timer, the
// placement legality rules, and the wireframe selection box.
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
// The world writer and reader are injected the same way, so interaction has no
// compile-time dependency on the World type either. Every hook is optional: with
// none set, `update()` reports "no target" and `render()` draws nothing.
//
// Thread safety: none. Main-thread only - it reads the camera, mutates the world
// and issues GL calls.

#include "render/Camera.hpp"
#include "world/Block.hpp"
#include "world/VoxelTypes.hpp"

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
    IntersectsPlayer,  ///< would trap the player inside the new block
    NothingHeld,       ///< held block is air
    NoWorldSink,       ///< no BlockWriteFn installed
    WriteFailed,       ///< the world refused the write
};

/// Why the break timer is in the state it is.
enum class BreakResult : std::uint8_t {
    None = 0,     ///< not breaking
    InProgress,   ///< timer running
    Broken,       ///< voxel removed this frame
    NoTarget,     ///< the ray hit nothing
    Unbreakable,  ///< hardness < 0
    NoWorldSink,  ///< no BlockWriteFn installed
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
        case BreakResult::None:        return "None";
        case BreakResult::InProgress:  return "InProgress";
        case BreakResult::Broken:      return "Broken";
        case BreakResult::NoTarget:    return "NoTarget";
        case BreakResult::Unbreakable: return "Unbreakable";
        case BreakResult::NoWorldSink: return "NoWorldSink";
        case BreakResult::WriteFailed: return "WriteFailed";
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

    [[nodiscard]] bool hasRaycaster() const noexcept { return static_cast<bool>(m_raycast); }
    [[nodiscard]] bool hasWorldSink() const noexcept { return static_cast<bool>(m_write); }

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
    /// Applies the three placement rules: inside the world, cell is replaceable,
    /// and the resulting cube does not overlap the player.
    [[nodiscard]] PlaceResult evaluatePlacement(const BlockPos& target) const noexcept;

    void resetBreakProgress() noexcept;

    class SelectionRenderer;  // GL-owning, defined in the .cpp

    const BlockRegistry* m_registry = nullptr;

    RaycastFn    m_raycast;
    BlockWriteFn m_write;
    BlockReadFn  m_read;

    float   m_reach     = kDefaultReachBlocks;
    BlockId m_heldBlock = blocks::Stone;

    Aabb m_playerAabb{};
    bool m_hasPlayerAabb = false;

    InteractionState m_state{};

    /// Voxel the break timer belongs to. Moving the crosshair off it resets the
    /// timer, so a player cannot chip away at several blocks in parallel.
    BlockPos m_breakingBlock{};
    bool     m_breakingValid   = false;
    float    m_breakElapsed    = 0.0f;
    float    m_placeCooldown   = 0.0f;
    bool     m_selectionVisible = true;

    std::unique_ptr<SelectionRenderer> m_selection;
};

}  // namespace voxl
