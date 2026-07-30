#pragma once

// Voxel ray traversal (Amanatides & Woo DDA) and the interaction raycast.
//
// The traversal visits every voxel whose interior the ray passes through, in
// order, with no gaps: unlike ray-marching by a fixed step it cannot skip a thin
// sliver where the ray grazes a block near a corner, and unlike a per-block
// plane intersection it does no redundant work. That exactness is what makes
// block breaking feel accurate along diagonal sight lines.
//
// Thread safety: pure functions over a caller-supplied BlockAccess. Safe on any
// thread on which the accessor is safe (a ChunkNeighbourhood snapshot always
// is).

#include "world/Block.hpp"
#include "world/BlockAccess.hpp"
#include "world/VoxelTypes.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

namespace voxl::physics {

/// Default interaction reach, in blocks, measured from the eye.
inline constexpr float kDefaultReach = 6.0f;

/// Hard ceiling on traversal iterations. A ray is bounded by its max distance,
/// so this only trips if a caller passes an absurd distance; it turns a
/// multi-second stall into a truncated result.
inline constexpr std::size_t kMaxTraversalSteps = 4096;

/// Axis of a Direction: NegX/PosX -> 0, NegY/PosY -> 1, NegZ/PosZ -> 2.
[[nodiscard]] constexpr int directionAxis(Direction direction) noexcept
{
    return static_cast<int>(static_cast<std::uint8_t>(direction) >> 1);
}

/// The face a ray travelling along `axis` in the given sign enters a block
/// through. Travelling +X enters through the block's NegX face, whose outward
/// normal points back at the ray origin - which is exactly the normal block
/// placement wants.
[[nodiscard]] constexpr Direction entryFace(int axis, bool positiveStep) noexcept
{
    return static_cast<Direction>(static_cast<std::uint8_t>(axis * 2 + (positiveStep ? 0 : 1)));
}

/// Walks the voxel grid along a ray.
///
/// `visit` is called as `bool visit(const BlockPos& voxel, Direction enteredFace,
/// float distance)` and returns true to stop. `distance` is measured along the
/// normalised direction, so it is a true world-space distance; it is 0 for the
/// voxel containing `origin`. For that first voxel `enteredFace` is synthetic:
/// it is the face the ray would have come through on its dominant axis, which
/// keeps "start inside a block" hits usable for placement.
///
/// A zero-length or non-finite direction visits nothing.
template <typename Visitor>
void traverseVoxels(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                    Visitor&& visit)
{
    const float lengthSquared = glm::dot(direction, direction);
    // Written as a positive test so that NaN falls through to the early return.
    if (!(lengthSquared > 0.0f) || !(maxDistance >= 0.0f)) {
        return;
    }
    const glm::vec3 dir = direction / std::sqrt(lengthSquared);

    const BlockPos     start = worldToBlockPos(origin);
    std::int32_t       voxel[3]{start.x, start.y, start.z};
    const float        rayOrigin[3]{origin.x, origin.y, origin.z};
    const float        rayDir[3]{dir.x, dir.y, dir.z};

    constexpr float kInfinity = std::numeric_limits<float>::infinity();
    std::int32_t    step[3]{0, 0, 0};
    float           tMax[3]{kInfinity, kInfinity, kInfinity};
    float           tDelta[3]{kInfinity, kInfinity, kInfinity};

    int   dominantAxis = 0;
    float dominantMagnitude = -1.0f;

    for (int axis = 0; axis < 3; ++axis) {
        const float d = rayDir[axis];
        if (std::abs(d) > dominantMagnitude) {
            dominantMagnitude = std::abs(d);
            dominantAxis      = axis;
        }
        if (d > 0.0f) {
            step[axis]   = 1;
            tDelta[axis] = 1.0f / d;
            // Distance to the next grid plane above the current voxel. Exact
            // when the origin sits precisely on a boundary.
            tMax[axis] = (static_cast<float>(voxel[axis]) + 1.0f - rayOrigin[axis]) / d;
        } else if (d < 0.0f) {
            step[axis]   = -1;
            tDelta[axis] = -1.0f / d;
            tMax[axis]   = (static_cast<float>(voxel[axis]) - rayOrigin[axis]) / d;
        }
    }

    Direction face     = entryFace(dominantAxis, rayDir[dominantAxis] > 0.0f);
    float     distance = 0.0f;

    for (std::size_t iteration = 0; iteration < kMaxTraversalSteps; ++iteration) {
        if (visit(BlockPos{voxel[0], voxel[1], voxel[2]}, face, distance)) {
            return;
        }

        // Advance along whichever axis reaches its next boundary first. On an
        // exact edge or corner crossing two or three values tie; picking the
        // lowest index is arbitrary but deterministic, and the voxels skipped by
        // the tie are entered for zero length anyway.
        int axis = 0;
        if (tMax[1] < tMax[axis]) {
            axis = 1;
        }
        if (tMax[2] < tMax[axis]) {
            axis = 2;
        }

        // Positive test again: an infinite tMax (axis-aligned ray) exits here.
        if (!(tMax[axis] <= maxDistance)) {
            return;
        }

        distance = tMax[axis];
        voxel[axis] += step[axis];
        face = entryFace(axis, step[axis] > 0);
        tMax[axis] += tDelta[axis];
    }
}

/// What the crosshair is pointing at.
struct RayHit {
    bool hit = false;

    /// The voxel that was struck.
    BlockPos block{};
    BlockId  blockId = blocks::Air;

    /// Face of `block` the ray entered through; its outward normal points back
    /// towards the ray origin.
    Direction  face = Direction::PosY;
    glm::ivec3 normal{0, 0, 0};

    /// Where a newly placed block goes: the empty voxel against the struck face.
    BlockPos placement{};

    /// Exact world-space intersection point and its distance from the origin.
    glm::vec3 point{0.0f};
    float     distance = 0.0f;

    /// The ray started inside the block it reports. `point` is then the origin
    /// and `face`/`normal` are derived from the ray's dominant axis, so
    /// placement still resolves to a sensible neighbour.
    bool startedInside = false;

    [[nodiscard]] explicit operator bool() const noexcept { return hit; }
};

/// Which blocks a raycast is allowed to select.
struct RaycastFilter {
    /// Skip liquids so that placing a block while standing in water puts it
    /// where the crosshair is instead of selecting the water. `replaceable` (air
    /// only) is always skipped; see docs/TECHNICAL_DESIGN.md section 7.
    bool skipLiquids = true;

    /// Skip blocks with no collision volume at all (foliage-style decoration).
    /// Off by default: the interaction ray should still be able to break them.
    bool requireCollision = false;
};

/// Interaction raycast against the world.
///
/// `direction` need not be normalised. `maxDistance` is in blocks along the
/// normalised direction.
[[nodiscard]] RayHit raycastBlocks(const BlockAccess& world, const BlockRegistry& registry,
                                   const glm::vec3& origin, const glm::vec3& direction,
                                   float maxDistance = kDefaultReach,
                                   const RaycastFilter& filter = {}) noexcept;

/// Raycast that stops at the first block with a solid collision volume,
/// regardless of the interaction rules. Used by camera collision and by the
/// "is there ground under me" queries that want geometry, not selectability.
[[nodiscard]] RayHit raycastSolid(const BlockAccess& world, const BlockRegistry& registry,
                                  const glm::vec3& origin, const glm::vec3& direction,
                                  float maxDistance = kDefaultReach) noexcept;

}  // namespace voxl::physics
