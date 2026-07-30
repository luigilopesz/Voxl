#include "physics/Collision.hpp"

#include <algorithm>
#include <cmath>

namespace voxl::physics {
namespace {

/// Inclusive voxel range touched by a box.
struct BlockRange {
    std::int32_t min[3]{};
    std::int32_t max[3]{};
};

/// Voxels a box strictly overlaps, after shrinking it by `shrink`.
///
/// The shrink is the whole point: a box resting on a floor has min.y within a
/// skin of the boundary, and a box sliding along a wall has a face exactly on
/// one. Without the shrink `floor()` would include the voxel on the far side of
/// that boundary and the resolver would report a contact against geometry the
/// body is merely flush with - which reads as sticking on flat floors and as
/// step-up refusing to work on an exactly-one-block ledge.
[[nodiscard]] BlockRange blockRangeOf(const Aabb& box, float shrink) noexcept
{
    const Aabb probe = box.shrunk(shrink);
    BlockRange range{};
    for (int axis = kAxisX; axis <= kAxisZ; ++axis) {
        range.min[axis] = floorToInt(probe.min[axis]);
        range.max[axis] = floorToInt(probe.max[axis]);
        // A box thinner than 2 * shrink collapses to a point; still test the one
        // voxel that contains it.
        if (range.max[axis] < range.min[axis]) {
            range.max[axis] = range.min[axis];
        }
    }
    return range;
}

/// Voxels a swept box can touch while moving along one axis.
///
/// The skin is applied asymmetrically and that asymmetry is deliberate. On the
/// TRAILING edge it must be applied: a body flush against a wall it is moving
/// away from sits a rounding error inside that wall's voxel, and without the
/// shrink the resolver would find a zero gap and refuse to let it leave. On the
/// LEADING edge it must not be: shrinking there drops a voxel the sweep lands
/// exactly on, the pass then reports the full displacement, and the body comes
/// to rest a rounding error inside the geometry instead of a skin short of it.
[[nodiscard]] BlockRange blockRangeForSweep(const Aabb& box, int axis, float delta,
                                            float shrink) noexcept
{
    Aabb probe = box.shrunk(shrink);
    if (delta >= 0.0f) {
        probe.max[axis] = box.max[axis] + delta;
    } else {
        probe.min[axis] = box.min[axis] + delta;
    }

    BlockRange range{};
    for (int component = kAxisX; component <= kAxisZ; ++component) {
        range.min[component] = floorToInt(probe.min[component]);
        range.max[component] = floorToInt(probe.max[component]);
        if (range.max[component] < range.min[component]) {
            range.max[component] = range.min[component];
        }
    }
    return range;
}

[[nodiscard]] float lerp(float a, float b, float t) noexcept
{
    return a + (b - a) * t;
}

/// Horizontal (XZ) distance between two boxes' origins.
[[nodiscard]] float horizontalDistance(const Aabb& from, const Aabb& to) noexcept
{
    const float dx = to.min.x - from.min.x;
    const float dz = to.min.z - from.min.z;
    return std::sqrt(dx * dx + dz * dz);
}

/// Resolves both horizontal axes in the given order, reporting which were
/// blocked. Shared by the normal pass and the step-up retry so the two cannot
/// drift apart.
[[nodiscard]] Aabb resolveHorizontal(const VoxelCollider& world, Aabb box, const glm::vec3& delta,
                                     int firstAxis, int secondAxis, float skin, bool& blockedX,
                                     bool& blockedZ) noexcept
{
    const int order[2]{firstAxis, secondAxis};
    for (const int axis : order) {
        const AxisMove move = sweepAxisAgainstWorld(world, box, axis, delta[axis], skin);
        box                 = box.translatedOnAxis(axis, move.travelled);
        if (move.blocked) {
            if (axis == kAxisX) {
                blockedX = true;
            } else {
                blockedZ = true;
            }
        }
    }
    return box;
}

struct StepAttempt {
    bool  accepted = false;
    Aabb  box{};
    float rise     = 0.0f;
    bool  blockedX = false;
    bool  blockedZ = false;
};

/// Rise, move, settle.
///
/// Doing it as a retry of the ordinary horizontal pass at a raised height is
/// what makes "1-block ledge yes, 2-block wall no" fall out of the geometry
/// instead of needing a hand-written height comparison against the blocking
/// voxel: a 2-block wall blocks the raised attempt exactly as it blocked the
/// first one, so the attempt gains no ground and is rejected.
[[nodiscard]] StepAttempt tryStepUp(const VoxelCollider& world, const Aabb& start, const Aabb& slid,
                                    const glm::vec3& delta, int firstAxis, int secondAxis,
                                    const MotionParams& params, float skin) noexcept
{
    StepAttempt attempt{};

    const AxisMove rise = sweepAxisAgainstWorld(world, start, kAxisY, params.stepHeight, skin);
    // A partial rise cannot clear a full-height obstacle, so a low ceiling is an
    // outright refusal rather than a shorter step.
    if (rise.travelled < params.stepHeight - skin) {
        return attempt;
    }
    const Aabb raised = start.translatedOnAxis(kAxisY, rise.travelled);

    bool       blockedX = false;
    bool       blockedZ = false;
    const Aabb moved =
        resolveHorizontal(world, raised, delta, firstAxis, secondAxis, skin, blockedX, blockedZ);

    if (horizontalDistance(raised, moved) <= horizontalDistance(start, slid) + skin) {
        return attempt;
    }

    // Settle back down by exactly what we climbed. If nothing catches the body it
    // was a hop over a gap, not a step onto a ledge.
    const AxisMove drop = sweepAxisAgainstWorld(world, moved, kAxisY, -rise.travelled, skin);
    if (!drop.blocked) {
        return attempt;
    }
    const Aabb settled = moved.translatedOnAxis(kAxisY, drop.travelled);

    // Landing lower than we started is a fall dressed up as a step; let the
    // ordinary passes handle it next frame.
    if (settled.min.y < start.min.y - skin) {
        return attempt;
    }
    if (world.overlapsSolid(settled, skin)) {
        return attempt;  // defensive: never hand back a penetrating box
    }

    attempt.accepted = true;
    attempt.box      = settled;
    attempt.rise     = settled.min.y - start.min.y;
    attempt.blockedX = blockedX;
    attempt.blockedZ = blockedZ;
    return attempt;
}

}  // namespace

// ------------------------------------------------------------- collider ----

bool VoxelCollider::overlapsSolid(const Aabb& box, float skin) const noexcept
{
    if (!box.valid()) {
        return false;
    }
    const BlockRange range = blockRangeOf(box, skin);
    for (std::int32_t y = range.min[kAxisY]; y <= range.max[kAxisY]; ++y) {
        for (std::int32_t z = range.min[kAxisZ]; z <= range.max[kAxisZ]; ++z) {
            for (std::int32_t x = range.min[kAxisX]; x <= range.max[kAxisX]; ++x) {
                if (isBlocking(BlockPos{x, y, z})) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool VoxelCollider::overlapsFluid(const Aabb& box, float skin) const noexcept
{
    if (!box.valid()) {
        return false;
    }
    const BlockRange range = blockRangeOf(box, skin);
    for (std::int32_t y = range.min[kAxisY]; y <= range.max[kAxisY]; ++y) {
        for (std::int32_t z = range.min[kAxisZ]; z <= range.max[kAxisZ]; ++z) {
            for (std::int32_t x = range.min[kAxisX]; x <= range.max[kAxisX]; ++x) {
                if (isFluid(BlockPos{x, y, z})) {
                    return true;
                }
            }
        }
    }
    return false;
}

float VoxelCollider::fluidFraction(const Aabb& box) const noexcept
{
    const float total = box.volume();
    if (!(total > 0.0f)) {
        return 0.0f;
    }
    // No shrink here: this is a true geometric measure, not a contact test, and
    // shaving a skin off each face would bias shallow water downwards.
    const BlockRange range = blockRangeOf(box, 0.0f);
    float            wet   = 0.0f;
    for (std::int32_t y = range.min[kAxisY]; y <= range.max[kAxisY]; ++y) {
        for (std::int32_t z = range.min[kAxisZ]; z <= range.max[kAxisZ]; ++z) {
            for (std::int32_t x = range.min[kAxisX]; x <= range.max[kAxisX]; ++x) {
                const BlockPos pos{x, y, z};
                if (isFluid(pos)) {
                    wet += box.intersectionVolume(Aabb::fromBlock(pos));
                }
            }
        }
    }
    return std::clamp(wet / total, 0.0f, 1.0f);
}

FluidState VoxelCollider::sampleFluid(const Aabb& box) const noexcept
{
    FluidState state{};
    if (!box.valid()) {
        return state;
    }
    state.submerged = fluidFraction(box);
    state.inFluid   = state.submerged > 0.0f;
    if (!state.inFluid) {
        return state;
    }

    const float height = box.max.y - box.min.y;

    Aabb feet = box;
    feet.max.y = box.min.y + std::min(0.4f, height);
    state.feetInFluid = overlapsFluid(feet);

    Aabb head = box;
    head.min.y = box.max.y - std::min(0.2f, height);
    state.headInFluid = overlapsFluid(head);

    return state;
}

// ----------------------------------------------------------- single axis ----

AxisMove sweepAxisAgainstWorld(const VoxelCollider& world, const Aabb& box, int axis, float delta,
                               float skin) noexcept
{
    AxisMove result{};
    if (delta == 0.0f || !box.valid()) {
        return result;
    }

    const float request  = std::clamp(delta, -kMaxStepDisplacement, kMaxStepDisplacement);
    const float reach    = std::abs(request);
    const bool  positive = request > 0.0f;

    const BlockRange range = blockRangeForSweep(box, axis, request, skin);

    // Every voxel in `range` already strictly overlaps the box on the two static
    // axes, because only `axis` was extended. So the inner test reduces to a
    // signed gap along one axis.
    float free    = reach;
    bool  blocked = false;

    for (std::int32_t y = range.min[kAxisY]; y <= range.max[kAxisY]; ++y) {
        for (std::int32_t z = range.min[kAxisZ]; z <= range.max[kAxisZ]; ++z) {
            for (std::int32_t x = range.min[kAxisX]; x <= range.max[kAxisX]; ++x) {
                const BlockPos pos{x, y, z};
                if (!world.isBlocking(pos)) {
                    continue;
                }
                const std::int32_t coordinate[3]{x, y, z};
                const float        low = static_cast<float>(coordinate[axis]);
                // Facing face of the voxel: its near side when moving up the
                // axis, its far side when moving down.
                const float gap = positive ? low - box.max[axis] : box.min[axis] - (low + 1.0f);
                // Stop a skin short so the resting box never strictly overlaps.
                // Negative gaps mean pre-existing penetration; clamping to 0
                // refuses to push deeper without shoving the body out.
                const float available = std::max(gap - skin, 0.0f);
                if (available < free) {
                    free    = available;
                    blocked = true;
                }
            }
        }
    }

    result.travelled = positive ? free : -free;
    result.blocked   = blocked;
    return result;
}

bool isSupported(const VoxelCollider& world, const Aabb& box, float probe) noexcept
{
    if (!(probe > 0.0f)) {
        return false;
    }
    return sweepAxisAgainstWorld(world, box, kAxisY, -probe).blocked;
}

// -------------------------------------------------------------- full move ----

MoveResult moveAabb(const VoxelCollider& world, const Aabb& box, const glm::vec3& velocity,
                    float dt, const MotionParams& params, bool onGroundBefore) noexcept
{
    MoveResult out{};
    out.box      = box;
    out.velocity = velocity;
    if (!(dt > 0.0f) || !box.valid()) {
        return out;
    }

    const float skin = params.skin > 0.0f ? params.skin : kCollisionSkin;

    glm::vec3 delta = velocity * dt;
    for (int axis = kAxisX; axis <= kAxisZ; ++axis) {
        delta[axis] = std::clamp(delta[axis], -kMaxStepDisplacement, kMaxStepDisplacement);
    }

    if (world.overlapsSolid(box, skin)) {
        // A body that begins the step inside geometry (a block was placed on it,
        // or it was teleported into terrain) has every axis clamped to zero and
        // would be frozen for good. Letting one unresolved step through is the
        // lesser evil, and the next step resolves normally once it is clear.
        out.box          = box.translated(delta);
        out.startedStuck = true;
        return out;
    }

    // ---- vertical first: ground contact gates everything below ----
    const AxisMove vertical = sweepAxisAgainstWorld(world, out.box, kAxisY, delta.y, skin);
    out.box                 = out.box.translatedOnAxis(kAxisY, vertical.travelled);
    if (vertical.blocked) {
        if (delta.y > 0.0f) {
            out.hitCeiling = true;
        } else {
            out.onGround = true;
        }
        out.velocity.y = 0.0f;
    }
    const Aabb afterVertical = out.box;

    // ---- horizontals, larger displacement first ----
    const int firstAxis  = std::abs(delta.x) >= std::abs(delta.z) ? kAxisX : kAxisZ;
    const int secondAxis = firstAxis == kAxisX ? kAxisZ : kAxisX;

    bool blockedX = false;
    bool blockedZ = false;
    out.box = resolveHorizontal(world, afterVertical, delta, firstAxis, secondAxis, skin, blockedX,
                                blockedZ);

    // ---- step-up ----
    if (params.stepHeight > 0.0f && (blockedX || blockedZ) && (onGroundBefore || out.onGround)) {
        const StepAttempt attempt = tryStepUp(world, afterVertical, out.box, delta, firstAxis,
                                              secondAxis, params, skin);
        if (attempt.accepted) {
            out.box       = attempt.box;
            out.steppedUp = true;
            out.stepRise  = attempt.rise;
            out.onGround  = true;
            // The climb succeeded, so horizontal momentum must survive: zeroing
            // it here is what makes a stepped-up character stall on every ledge.
            blockedX = attempt.blockedX;
            blockedZ = attempt.blockedZ;
        }
    }

    out.hitWallX = blockedX;
    out.hitWallZ = blockedZ;
    if (blockedX) {
        out.velocity.x = 0.0f;
    }
    if (blockedZ) {
        out.velocity.z = 0.0f;
    }

    // A body that did not move down this step can still be standing on
    // something (idle, or walking on flat ground). Probing is more robust than
    // remembering the last contact: it also goes false the instant the ground is
    // mined out from under a stationary player.
    if (!out.onGround && delta.y <= 0.0f) {
        out.onGround = isSupported(world, out.box, 2.0f * skin);
    }

    return out;
}

// ------------------------------------------------------------ integration ----

float applyGravity(float verticalVelocity, float dt, const MotionParams& params,
                   const FluidState& fluid) noexcept
{
    if (!(dt > 0.0f)) {
        return verticalVelocity;
    }
    const float wetness  = std::clamp(fluid.submerged, 0.0f, 1.0f);
    const float scale    = lerp(1.0f, params.fluidGravityScale, wetness);
    const float terminal = lerp(params.terminalVelocity, params.fluidTerminalVelocity, wetness);

    const float result = verticalVelocity - params.gravity * scale * dt;
    return std::max(result, -std::abs(terminal));
}

glm::vec3 applyFluidDrag(const glm::vec3& velocity, float dt, const MotionParams& params,
                         const FluidState& fluid) noexcept
{
    const float wetness = std::clamp(fluid.submerged, 0.0f, 1.0f);
    if (!(dt > 0.0f) || wetness <= 0.0f) {
        return velocity;
    }
    // Multiplicative per second, so pow() makes the result independent of the
    // step size. A per-step multiply would make drag depend on the tick rate.
    const float retained = lerp(1.0f, std::clamp(params.fluidDrag, 0.0f, 1.0f), wetness);
    return velocity * std::pow(retained, dt);
}

}  // namespace voxl::physics
