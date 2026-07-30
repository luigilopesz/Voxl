#pragma once

// Axis-aligned bounding box and the swept-box primitives collision resolution
// is built on.
//
// NAMESPACE NOTE: this type lives in `voxl::physics`, not `voxl`, because
// `src/render/Camera.hpp` already defines a `voxl::Aabb` for frustum culling.
// The two are structurally identical but serve different callers, and
// `gameplay/Player.hpp` must include both headers, so a flat name would be a
// hard redefinition error. See the integration notes.
//
// UNITS: everything here is in blocks (1 block == 1 world unit) and the boxes
// are half-open in spirit: two boxes that merely share a face do NOT intersect.
// That rule is load bearing. Resting on a floor means `min.y == floor.max.y`,
// and if touching counted as penetration the resolver would report a collision
// on every axis for a player standing still.
//
// Thread safety: value type, no shared state. Safe anywhere.

#include "world/VoxelTypes.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

namespace voxl::physics {

/// Floor-then-truncate. A plain cast rounds toward zero and would place every
/// negative coordinate one block too high.
[[nodiscard]] inline std::int32_t floorToInt(float value) noexcept
{
    return static_cast<std::int32_t>(std::floor(value));
}

/// Axis index used throughout the physics code: 0 = X, 1 = Y, 2 = Z. Matches
/// glm's component order so `vec[axis]` is always valid.
inline constexpr int kAxisX = 0;
inline constexpr int kAxisY = 1;
inline constexpr int kAxisZ = 2;

struct Aabb {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};

    // ---------------------------------------------------------- factories --

    [[nodiscard]] static Aabb fromMinMax(const glm::vec3& lo, const glm::vec3& hi) noexcept
    {
        return Aabb{glm::min(lo, hi), glm::max(lo, hi)};
    }

    [[nodiscard]] static Aabb fromCentreExtents(const glm::vec3& centre,
                                                const glm::vec3& halfExtents) noexcept
    {
        const glm::vec3 absolute = glm::abs(halfExtents);
        return Aabb{centre - absolute, centre + absolute};
    }

    /// The unit cube occupied by one voxel.
    [[nodiscard]] static Aabb fromBlock(const BlockPos& block) noexcept
    {
        const glm::vec3 lo{static_cast<float>(block.x), static_cast<float>(block.y),
                           static_cast<float>(block.z)};
        return Aabb{lo, lo + glm::vec3{1.0f}};
    }

    /// A character volume: `feet` is the centre of the bottom face, which is the
    /// point gameplay code treats as "the player's position". Building the box
    /// from the feet rather than the centre keeps ground contact exact when the
    /// height changes (crouching) - only `max.y` moves.
    [[nodiscard]] static Aabb fromFeet(const glm::vec3& feet, float width, float height) noexcept
    {
        const float half = width * 0.5f;
        return Aabb{glm::vec3{feet.x - half, feet.y, feet.z - half},
                    glm::vec3{feet.x + half, feet.y + height, feet.z + half}};
    }

    // ------------------------------------------------------------ queries --

    [[nodiscard]] glm::vec3 centre() const noexcept { return (min + max) * 0.5f; }
    [[nodiscard]] glm::vec3 size() const noexcept { return max - min; }
    [[nodiscard]] glm::vec3 halfExtents() const noexcept { return (max - min) * 0.5f; }

    [[nodiscard]] float volume() const noexcept
    {
        const glm::vec3 s = glm::max(max - min, glm::vec3{0.0f});
        return s.x * s.y * s.z;
    }

    /// Degenerate boxes (zero or inverted on any axis) can never overlap
    /// anything, so the resolver treats them as absent rather than special
    /// casing them at every call site.
    [[nodiscard]] bool valid() const noexcept
    {
        return max.x > min.x && max.y > min.y && max.z > min.z;
    }

    [[nodiscard]] bool contains(const glm::vec3& point) const noexcept
    {
        return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y &&
               point.z >= min.z && point.z <= max.z;
    }

    [[nodiscard]] bool contains(const Aabb& other) const noexcept
    {
        return other.min.x >= min.x && other.max.x <= max.x && other.min.y >= min.y &&
               other.max.y <= max.y && other.min.z >= min.z && other.max.z <= max.z;
    }

    /// STRICT overlap: boxes sharing only a face, edge or corner do not
    /// intersect. See the header comment for why this is not negotiable.
    [[nodiscard]] bool intersects(const Aabb& other) const noexcept
    {
        return min.x < other.max.x && max.x > other.min.x && min.y < other.max.y &&
               max.y > other.min.y && min.z < other.max.z && max.z > other.min.z;
    }

    /// Non-strict companion: true when the boxes overlap or are flush. Useful
    /// for "is this block adjacent to me" queries, never for penetration tests.
    [[nodiscard]] bool touchesOrIntersects(const Aabb& other) const noexcept
    {
        return min.x <= other.max.x && max.x >= other.min.x && min.y <= other.max.y &&
               max.y >= other.min.y && min.z <= other.max.z && max.z >= other.min.z;
    }

    [[nodiscard]] bool overlapsOnAxis(const Aabb& other, int axis) const noexcept
    {
        return min[axis] < other.max[axis] && max[axis] > other.min[axis];
    }

    /// Overlap volume, 0 when the boxes do not strictly intersect. Used to work
    /// out how submerged a body is.
    [[nodiscard]] float intersectionVolume(const Aabb& other) const noexcept
    {
        const glm::vec3 lo = glm::max(min, other.min);
        const glm::vec3 hi = glm::min(max, other.max);
        const glm::vec3 s  = hi - lo;
        if (s.x <= 0.0f || s.y <= 0.0f || s.z <= 0.0f) {
            return 0.0f;
        }
        return s.x * s.y * s.z;
    }

    // ------------------------------------------------------- construction --

    [[nodiscard]] Aabb translated(const glm::vec3& delta) const noexcept
    {
        return Aabb{min + delta, max + delta};
    }

    /// Moves one axis only. The resolver works axis by axis, and doing it with a
    /// vec3 of zeros invited sign bugs.
    [[nodiscard]] Aabb translatedOnAxis(int axis, float delta) const noexcept
    {
        Aabb result = *this;
        result.min[axis] += delta;
        result.max[axis] += delta;
        return result;
    }

    [[nodiscard]] Aabb expanded(float amount) const noexcept
    {
        return Aabb{min - glm::vec3{amount}, max + glm::vec3{amount}};
    }

    [[nodiscard]] Aabb expanded(const glm::vec3& amount) const noexcept
    {
        return Aabb{min - amount, max + amount};
    }

    /// Shrinks symmetrically, never past degenerate. Collision uses this to keep
    /// a face that is exactly flush with a block boundary from dragging the
    /// neighbouring block into the candidate set.
    [[nodiscard]] Aabb shrunk(float amount) const noexcept
    {
        Aabb result{min + glm::vec3{amount}, max - glm::vec3{amount}};
        for (int axis = kAxisX; axis <= kAxisZ; ++axis) {
            if (result.min[axis] > result.max[axis]) {
                const float mid = (min[axis] + max[axis]) * 0.5f;
                result.min[axis] = mid;
                result.max[axis] = mid;
            }
        }
        return result;
    }

    /// Broad-phase hull of the whole sweep: the union of the box at t=0 and at
    /// t=1. Everything the box can possibly touch during the move is inside it.
    [[nodiscard]] Aabb sweptHull(const glm::vec3& delta) const noexcept
    {
        const Aabb moved = translated(delta);
        return Aabb{glm::min(min, moved.min), glm::max(max, moved.max)};
    }

    /// Same, restricted to one axis.
    [[nodiscard]] Aabb sweptHullOnAxis(int axis, float delta) const noexcept
    {
        Aabb result = *this;
        if (delta >= 0.0f) {
            result.max[axis] += delta;
        } else {
            result.min[axis] += delta;
        }
        return result;
    }

    [[nodiscard]] Aabb merged(const Aabb& other) const noexcept
    {
        return Aabb{glm::min(min, other.min), glm::max(max, other.max)};
    }

    friend bool operator==(const Aabb&, const Aabb&) = default;
};

// ------------------------------------------------------------------ sweeps --

/// Result of sweeping one box against another.
struct SweepResult {
    /// True only when contact happens strictly before the end of the motion.
    bool hit = false;
    /// Fraction of `delta` travelled before contact, in [0, 1]. 1 when missed.
    float time = 1.0f;
    /// Axis of first contact (0/1/2), or -1 when missed.
    int axis = -1;
    /// Outward normal of the struck face along `axis`: -1 or +1. It points back
    /// towards where the moving box came from.
    float normalSign = 0.0f;

    [[nodiscard]] glm::vec3 normal() const noexcept
    {
        glm::vec3 n{0.0f};
        if (axis >= 0) {
            n[axis] = normalSign;
        }
        return n;
    }
};

/// Continuous slab sweep of `moving` displaced by `delta` against a static
/// `target`.
///
/// WHY SWEPT AND NOT MOVE-THEN-TEST: at terminal velocity a 60 Hz step covers
/// more than one block, so moving first and testing afterwards misses the floor
/// entirely and the player falls through the world. This is the tunnelling
/// defect the collision code exists to prevent.
///
/// Contact behaviour matches Aabb::intersects: boxes that start flush and move
/// apart do not register, and boxes that start flush and move together register
/// at time 0.
[[nodiscard]] inline SweepResult sweep(const Aabb& moving, const glm::vec3& delta,
                                       const Aabb& target) noexcept
{
    SweepResult result{};
    if (!moving.valid() || !target.valid()) {
        return result;
    }

    constexpr float kInfinity = std::numeric_limits<float>::infinity();
    float entryTime = -kInfinity;
    float exitTime  = kInfinity;
    int   entryAxis = -1;

    for (int axis = kAxisX; axis <= kAxisZ; ++axis) {
        const float step = delta[axis];
        if (step == 0.0f) {
            // No motion on this axis, so the slabs must already overlap or the
            // boxes can never meet. Strict, so sliding along a flush face is a
            // miss rather than a permanent contact.
            if (!moving.overlapsOnAxis(target, axis)) {
                return result;
            }
            continue;
        }

        const float toNear = target.min[axis] - moving.max[axis];
        const float toFar  = target.max[axis] - moving.min[axis];
        const float t0     = toNear / step;
        const float t1     = toFar / step;

        const float axisEntry = std::min(t0, t1);
        const float axisExit  = std::max(t0, t1);

        if (axisEntry > entryTime) {
            entryTime = axisEntry;
            entryAxis = axis;
        }
        exitTime = std::min(exitTime, axisExit);
    }

    if (entryAxis < 0) {
        // Zero displacement: the boxes overlap on every axis or we returned
        // above. Report a contact at t=0 so callers can detect being stuck.
        if (moving.intersects(target)) {
            result.hit  = true;
            result.time = 0.0f;
        }
        return result;
    }

    // exitTime <= 0 means the boxes are separating; entryTime >= 1 means contact
    // would happen after the end of the motion.
    if (entryTime > exitTime || exitTime <= 0.0f || entryTime >= 1.0f) {
        return result;
    }

    result.hit        = true;
    result.time       = std::clamp(entryTime, 0.0f, 1.0f);
    result.axis       = entryAxis;
    result.normalSign = delta[entryAxis] > 0.0f ? -1.0f : 1.0f;
    return result;
}

/// Free distance available to `moving` before it touches `target`, along one
/// axis, in the direction of `delta`'s sign.
///
/// Returns |delta| when the target is out of reach, is behind the mover, or does
/// not overlap on the two static axes. When the boxes already overlap on `axis`
/// the result is 0: the caller refuses to travel further in, but never pushes,
/// so the opposite direction stays free and a body that somehow ended up inside
/// geometry can still walk out.
[[nodiscard]] inline float axisGap(const Aabb& moving, const Aabb& target, int axis,
                                   float delta) noexcept
{
    const float reach = std::abs(delta);
    if (delta == 0.0f) {
        return 0.0f;
    }
    for (int other = kAxisX; other <= kAxisZ; ++other) {
        if (other != axis && !moving.overlapsOnAxis(target, other)) {
            return reach;
        }
    }
    const float gap = delta > 0.0f ? target.min[axis] - moving.max[axis]
                                   : moving.min[axis] - target.max[axis];
    if (gap < 0.0f) {
        // Negative means either "already interpenetrating" or "entirely behind
        // us". Only the first is an obstruction; conflating them would stop a
        // body dead the moment it turned around.
        return moving.overlapsOnAxis(target, axis) ? 0.0f : reach;
    }
    return std::min(gap, reach);
}

}  // namespace voxl::physics
