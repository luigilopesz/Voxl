#include "physics/Raycast.hpp"

#include <cmath>

namespace voxl::physics {
namespace {

/// Shared body for both public entry points. `selectable` decides whether a
/// voxel terminates the ray.
template <typename SelectFn>
RayHit castWith(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                SelectFn&& selectable) noexcept
{
    RayHit result{};

    const float lengthSquared = glm::dot(direction, direction);
    if (!(lengthSquared > 0.0f)) {
        return result;
    }
    // The traversal normalises internally; we need the same unit vector here to
    // reconstruct the exact hit point from the reported distance.
    const glm::vec3 unitDirection = direction / std::sqrt(lengthSquared);

    bool first = true;
    traverseVoxels(origin, direction, maxDistance,
                   [&](const BlockPos& voxel, Direction face, float distance) {
                       const BlockId id = selectable.world().getBlock(voxel);
                       if (!selectable(id)) {
                           first = false;
                           return false;
                       }

                       const std::size_t faceIndex = static_cast<std::size_t>(face);
                       const glm::ivec3& offset    = kDirectionOffsets[faceIndex];

                       result.hit           = true;
                       result.block         = voxel;
                       result.blockId       = id;
                       result.face          = face;
                       result.normal        = offset;
                       result.placement     = voxel.offset(offset.x, offset.y, offset.z);
                       result.distance      = distance;
                       result.point         = origin + unitDirection * distance;
                       result.startedInside = first;
                       return true;
                   });

    return result;
}

/// Predicate carrying the world reference, so the lambda above stays a single
/// template rather than two near-identical copies.
class BlockPredicate {
public:
    BlockPredicate(const BlockAccess& world, const BlockRegistry& registry,
                   const RaycastFilter& filter) noexcept
        : m_world(&world), m_registry(&registry), m_filter(filter)
    {
    }

    [[nodiscard]] const BlockAccess& world() const noexcept { return *m_world; }

    [[nodiscard]] bool operator()(BlockId id) const noexcept
    {
        const BlockType& type = m_registry->get(id);
        if (type.replaceable) {
            return false;  // air
        }
        if (m_filter.skipLiquids && type.liquid) {
            return false;
        }
        if (m_filter.requireCollision && type.collisionShape == CollisionShape::None) {
            return false;
        }
        return true;
    }

private:
    const BlockAccess*   m_world;
    const BlockRegistry* m_registry;
    RaycastFilter        m_filter;
};

class SolidPredicate {
public:
    SolidPredicate(const BlockAccess& world, const BlockRegistry& registry) noexcept
        : m_world(&world), m_registry(&registry)
    {
    }

    [[nodiscard]] const BlockAccess& world() const noexcept { return *m_world; }

    [[nodiscard]] bool operator()(BlockId id) const noexcept { return m_registry->isSolid(id); }

private:
    const BlockAccess*   m_world;
    const BlockRegistry* m_registry;
};

}  // namespace

RayHit raycastBlocks(const BlockAccess& world, const BlockRegistry& registry,
                     const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
                     const RaycastFilter& filter) noexcept
{
    return castWith(origin, direction, maxDistance, BlockPredicate{world, registry, filter});
}

RayHit raycastSolid(const BlockAccess& world, const BlockRegistry& registry,
                    const glm::vec3& origin, const glm::vec3& direction,
                    float maxDistance) noexcept
{
    return castWith(origin, direction, maxDistance, SolidPredicate{world, registry});
}

}  // namespace voxl::physics
