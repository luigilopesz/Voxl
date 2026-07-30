#pragma once

// Swept AABB collision against the voxel grid, plus the gravity/fluid integrator
// the player controller drives.
//
// DESIGN: MOVE ONE AXIS AT A TIME, Y FIRST
// ----------------------------------------
// A single combined sweep finds the earliest contact over all three axes and
// stops there. That is correct for a projectile and wrong for a walking
// character: brushing a wall at a shallow angle cancels the whole move, so the
// player snags on every block seam and grinding along a flat wall stutters.
// Resolving each axis independently lets the blocked component clamp while the
// others keep their full displacement, which is what "sliding along a wall"
// means.
//
// Y goes first because ground contact decides everything after it. Whether the
// player is standing on something determines the friction model, whether a jump
// is legal and whether a step-up may be attempted, so the horizontal pass needs
// the vertical answer already in hand. The two horizontal axes are then resolved
// larger-displacement-first: with a fixed order, walking diagonally into an
// inside corner resolves differently depending on which wall you approach, and
// ordering by magnitude removes that asymmetry without introducing a second
// sweep.
//
// Each axis pass is SWEPT, never move-then-test. At the 78 blocks/s terminal
// velocity a 60 Hz step covers 1.3 blocks, so a teleport-then-resolve would step
// straight past a one-block-thick floor. The pass enumerates every blocking
// voxel in the swept region and takes the nearest, so no thickness of floor can
// be tunnelled at any speed the clamp allows.
//
// UNITS: blocks and seconds throughout. Gravity is in blocks/s^2.
//
// Thread safety: VoxelCollider is a non-owning view over a BlockAccess and a
// BlockRegistry and holds no mutable state, so it is safe wherever the accessor
// is. All the free functions are pure.

#include "physics/Aabb.hpp"
#include "world/Block.hpp"
#include "world/BlockAccess.hpp"
#include "world/VoxelTypes.hpp"

#include <glm/vec3.hpp>

namespace voxl::physics {

/// Contact tolerance, in blocks. 1 mm: far below anything visible, far above the
/// float rounding error at world-scale coordinates.
///
/// It does two jobs. Resolved contacts stop this far short of the surface, so a
/// resting body never quite touches and the strict overlap test in
/// Aabb::intersects stays false. And candidate voxels are gathered from a box
/// shrunk by this much, so a face flush with a block boundary - or a millimetre
/// inside it after a landing - does not drag the neighbouring voxel into the
/// sweep. The second part is what makes an exactly-one-block step-up work.
inline constexpr float kCollisionSkin = 1.0e-3f;

/// Upper bound on the displacement one axis may request in a single step, in
/// blocks. It bounds the voxel enumeration loop, so a caller that passes a wild
/// dt cannot turn a physics step into a multi-second scan. 64 blocks is two
/// chunks: far beyond anything legitimate at the clamped 250 ms frame delta.
inline constexpr float kMaxStepDisplacement = 64.0f;

/// Tunables for the body being moved. Defaults are Minecraft-like: a snappier
/// gravity than Earth's because a realistic 9.81 makes block-scale jumps feel
/// floaty.
struct MotionParams {
    /// Downward acceleration, blocks/s^2.
    float gravity = 32.0f;
    /// Cap on downward speed, blocks/s. Without it a long fall accumulates a
    /// per-step displacement large enough to make the sweep loop expensive.
    float terminalVelocity = 78.4f;

    /// Highest ledge that can be walked onto without jumping, in blocks. 0
    /// disables step-up entirely.
    float stepHeight = 1.0f;

    /// Fraction of gravity that applies while submerged. Water is not buoyant
    /// enough to float in this game; it just slows the fall.
    float fluidGravityScale = 0.28f;
    /// Fraction of velocity retained per second while submerged; applied as
    /// pow(drag, dt) so it is frame-rate independent.
    float fluidDrag = 0.06f;
    /// Downward speed cap while submerged, blocks/s.
    float fluidTerminalVelocity = 3.0f;

    /// Contact tolerance; see kCollisionSkin.
    float skin = kCollisionSkin;
};

/// Outcome of one collision-resolved move.
struct MoveResult {
    /// Resolved box.
    Aabb box{};
    /// Input velocity with the blocked components zeroed.
    glm::vec3 velocity{0.0f};

    bool onGround   = false;
    bool hitCeiling = false;
    bool hitWallX   = false;
    bool hitWallZ   = false;

    /// A step-up was performed; `stepRise` is how far the body climbed.
    bool  steppedUp = false;
    float stepRise  = 0.0f;

    /// The body already overlapped solid geometry when the move began, so
    /// collision was skipped for this step to let it escape. See moveAabb.
    bool startedStuck = false;
};

/// How wet the body is.
struct FluidState {
    /// Any part of the body is inside a fluid voxel.
    bool inFluid = false;
    /// The bottom 0.4 blocks are in fluid - the "wading" test that governs
    /// footstep sounds and whether a jump becomes a swim stroke.
    bool feetInFluid = false;
    /// The top 0.2 blocks are in fluid; drives the drowning/overlay logic.
    bool headInFluid = false;
    /// Fraction of the body's volume inside fluid, in [0, 1]. Drag and gravity
    /// scale with it so entering water is continuous rather than a step change.
    float submerged = 0.0f;
};

/// Read-only view of the world for collision purposes.
///
/// Holds pointers rather than references so it stays copyable and assignable;
/// it does NOT own either object and both must outlive it.
class VoxelCollider {
public:
    VoxelCollider(const BlockAccess& world, const BlockRegistry& registry) noexcept
        : m_world(&world), m_registry(&registry)
    {
    }

    [[nodiscard]] const BlockAccess&   world() const noexcept { return *m_world; }
    [[nodiscard]] const BlockRegistry& registry() const noexcept { return *m_registry; }

    [[nodiscard]] BlockId blockAt(const BlockPos& pos) const noexcept
    {
        return m_world->getBlock(pos);
    }

    /// Only CollisionShape::Cube stops movement. Fluid occupies space but is
    /// passable, which is what makes water swimmable rather than a wall.
    [[nodiscard]] bool isBlocking(const BlockPos& pos) const noexcept
    {
        return m_registry->get(m_world->getBlock(pos)).collisionShape == CollisionShape::Cube;
    }

    [[nodiscard]] bool isFluid(const BlockPos& pos) const noexcept
    {
        return m_registry->get(m_world->getBlock(pos)).collisionShape == CollisionShape::Fluid;
    }

    /// True when any blocking voxel strictly overlaps `box`, ignoring overlaps
    /// smaller than the skin.
    [[nodiscard]] bool overlapsSolid(const Aabb& box, float skin = kCollisionSkin) const noexcept;

    [[nodiscard]] bool overlapsFluid(const Aabb& box, float skin = kCollisionSkin) const noexcept;

    /// Fraction of `box`'s volume inside fluid voxels, in [0, 1].
    [[nodiscard]] float fluidFraction(const Aabb& box) const noexcept;

    [[nodiscard]] FluidState sampleFluid(const Aabb& box) const noexcept;

private:
    const BlockAccess*   m_world;
    const BlockRegistry* m_registry;
};

// ------------------------------------------------------------- single axis --

/// One swept axis pass.
struct AxisMove {
    /// Signed distance actually travelled; same sign as the request, magnitude
    /// never larger.
    float travelled = 0.0f;
    /// A blocking voxel stopped the move short.
    bool blocked = false;
};

/// Sweeps `box` along one axis by `delta` and clamps at the nearest blocking
/// voxel. Enumerates the whole swept region, so it cannot tunnel.
[[nodiscard]] AxisMove sweepAxisAgainstWorld(const VoxelCollider& world, const Aabb& box, int axis,
                                            float delta, float skin = kCollisionSkin) noexcept;

/// True when a blocking voxel sits within `probe` blocks below the box. Cheaper
/// and more stable than remembering the last collision, and it stays correct
/// when the ground is removed from under a stationary body.
[[nodiscard]] bool isSupported(const VoxelCollider& world, const Aabb& box,
                              float probe = 2.0f * kCollisionSkin) noexcept;

// -------------------------------------------------------------- full move --

/// Moves `box` by `velocity * dt`, resolving collisions axis by axis and
/// attempting a step-up when a horizontal axis is blocked and the body is
/// grounded.
///
/// `onGroundBefore` is the caller's ground state from the previous step. It
/// matters because a body that walks off a ledge and immediately into a wall
/// within the same step should still be allowed to step up onto that wall if it
/// was standing a moment ago; requiring ground *after* the Y pass alone makes
/// step-up drop out intermittently on uneven terrain.
[[nodiscard]] MoveResult moveAabb(const VoxelCollider& world, const Aabb& box,
                                  const glm::vec3& velocity, float dt, const MotionParams& params,
                                  bool onGroundBefore) noexcept;

// ------------------------------------------------------------ integration --

/// Applies gravity and the appropriate terminal-velocity clamp for one step.
///
/// Split out from moveAabb so the integration and the resolution can be tested
/// independently, and so a flying/noclip body can skip it without threading a
/// flag through the resolver.
[[nodiscard]] float applyGravity(float verticalVelocity, float dt, const MotionParams& params,
                                 const FluidState& fluid) noexcept;

/// Applies submersion drag. Multiplicative per second and raised to dt, so the
/// result is identical at any step size.
[[nodiscard]] glm::vec3 applyFluidDrag(const glm::vec3& velocity, float dt,
                                       const MotionParams& params,
                                       const FluidState& fluid) noexcept;

}  // namespace voxl::physics
