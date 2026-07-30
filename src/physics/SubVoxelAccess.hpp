#pragma once

// Read-only sub-voxel occupancy lookup for physics, raycasting and interaction,
// plus the integer coordinate arithmetic those three share.
//
// WHY THIS IS NOT PART OF BlockAccess
// -----------------------------------
// BlockAccess answers "which block is at this position" and is called tens of
// thousands of times per chunk by the mesher. Sub-voxel damage lives in a sparse
// side table (see world/SubVoxel.hpp) that is empty for all untouched terrain,
// so folding the query into BlockAccess would make every mesher read pay for a
// second lookup that returns nullptr essentially always. Physics touches a
// handful of blocks per step and can afford the query - and only asks about
// blocks that already tested as blocking.
//
// A NULL SubVoxelAccess POINTER MEANS "NOTHING IS DAMAGED". Every consumer in
// this directory takes the accessor as an optional pointer defaulting to null,
// which is byte-for-byte the behaviour they had before sub-voxels existed. That
// is what lets the existing call sites, the tests and the mesher-free unit
// harnesses keep compiling and behaving identically.
//
// Thread safety: an implementation is as safe as the storage it reads. The
// ChunkSubVoxelAccess below reads through a caller-supplied chunk lookup and
// holds no state of its own, so it is safe wherever that lookup is - but the
// data it returns is subject to the same no-write-while-a-neighbour-is-meshing
// rule as everything else (World::isEditBlocked).

#include "world/Chunk.hpp"
#include "world/SubVoxel.hpp"
#include "world/VoxelTypes.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>

#include <glm/vec3.hpp>

namespace voxl::physics {

/// Sub-voxel occupancy reader.
class SubVoxelAccess {
public:
    virtual ~SubVoxelAccess() = default;

    /// Occupancy grid of the block at `pos`, or nullptr when the block is
    /// UNIFORM - air, or solid with all 512 sub-voxels intact. Callers must
    /// treat nullptr as "derive from the block id"; see the invariant at the top
    /// of world/SubVoxel.hpp.
    [[nodiscard]] virtual const SubVoxelGrid* subVoxelsAt(const BlockPos& pos) const noexcept = 0;

protected:
    SubVoxelAccess()                                 = default;
    SubVoxelAccess(const SubVoxelAccess&)            = default;
    SubVoxelAccess& operator=(const SubVoxelAccess&) = default;
};

/// SubVoxelAccess backed by a chunk lookup.
///
/// Deliberately uncached. A cache would have to be invalidated on every edit and
/// every streaming unload, and a stale ConstChunkPtr for a chunk position that
/// has since been unloaded and re-created reads perfectly valid memory
/// describing the wrong world - a bug with no crash to point at it. The lookup
/// itself is a hash probe under a shared lock and runs a few dozen times per
/// physics step, which is not worth that risk.
///
/// `lookup` must not throw: it is called from noexcept context.
class ChunkSubVoxelAccess final : public SubVoxelAccess {
public:
    /// Returns the chunk at a position, or nullptr when it is not resident.
    using ChunkLookupFn = std::function<ConstChunkPtr(const ChunkPos&)>;

    explicit ChunkSubVoxelAccess(ChunkLookupFn lookup) noexcept : m_lookup(std::move(lookup)) {}

    [[nodiscard]] const SubVoxelGrid* subVoxelsAt(const BlockPos& pos) const noexcept override
    {
        if (!m_lookup || !isInsideWorld(pos)) {
            return nullptr;
        }
        const ConstChunkPtr chunk = m_lookup(toChunkPos(pos));
        if (chunk == nullptr || !chunk->hasSubVoxelDamage()) {
            return nullptr;
        }
        return chunk->subVoxels().find(localIndex(toLocalPos(pos)));
    }

private:
    ChunkLookupFn m_lookup;
};

// ------------------------------------------------- global sub-voxel coords --

/// A sub-voxel addressed in world space rather than relative to its block.
///
/// A carving brush is a ball of sub-voxels around a point and routinely spills
/// across a block boundary. Expressing that as `block * 8 + sub` turns the
/// spill into plain integer addition and one floor-division on the way out,
/// instead of six special cases for the faces plus twelve for the edges.
struct SubVoxelCoord {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;

    friend constexpr bool operator==(const SubVoxelCoord&, const SubVoxelCoord&) noexcept = default;

    /// Arithmetic right shift, so negative world coordinates floor rather than
    /// truncate toward zero - the same rule as blockToChunkAxis.
    [[nodiscard]] constexpr BlockPos block() const noexcept
    {
        return BlockPos{x >> kSubVoxelShift, y >> kSubVoxelShift, z >> kSubVoxelShift};
    }

    [[nodiscard]] constexpr std::int32_t localX() const noexcept { return x & kSubVoxelMask; }
    [[nodiscard]] constexpr std::int32_t localY() const noexcept { return y & kSubVoxelMask; }
    [[nodiscard]] constexpr std::int32_t localZ() const noexcept { return z & kSubVoxelMask; }

    /// Index within the parent block's 8^3 grid.
    [[nodiscard]] constexpr std::size_t index() const noexcept
    {
        return subVoxelIndex(localX(), localY(), localZ());
    }

    [[nodiscard]] constexpr SubVoxelCoord offset(std::int32_t dx, std::int32_t dy,
                                                 std::int32_t dz) const noexcept
    {
        return SubVoxelCoord{x + dx, y + dy, z + dz};
    }
};

[[nodiscard]] constexpr SubVoxelCoord toGlobalSubVoxel(const BlockPos& block, std::int32_t sx,
                                                       std::int32_t sy, std::int32_t sz) noexcept
{
    return SubVoxelCoord{block.x * kSubVoxelResolution + sx, block.y * kSubVoxelResolution + sy,
                         block.z * kSubVoxelResolution + sz};
}

[[nodiscard]] constexpr SubVoxelCoord toGlobalSubVoxel(const BlockPos& block,
                                                       const glm::ivec3& sub) noexcept
{
    return toGlobalSubVoxel(block, sub.x, sub.y, sub.z);
}

/// Sub-voxel of `block` that contains `point`.
///
/// CLAMPED, not wrapped. An interaction hit point lies exactly on a block face,
/// where the scaled local coordinate is exactly 0 or 8 and a rounding error can
/// land it on either side. Clamping picks the sub-voxel just inside the block,
/// which is the one the player is pointing at; wrapping would silently address a
/// neighbouring block's grid.
[[nodiscard]] inline glm::ivec3 subVoxelOfPoint(const glm::vec3& point,
                                                const BlockPos& block) noexcept
{
    constexpr float resolution = static_cast<float>(kSubVoxelResolution);
    const glm::vec3 local{(point.x - static_cast<float>(block.x)) * resolution,
                          (point.y - static_cast<float>(block.y)) * resolution,
                          (point.z - static_cast<float>(block.z)) * resolution};
    return glm::ivec3{
        std::clamp(static_cast<std::int32_t>(std::floor(local.x)), 0, kSubVoxelResolution - 1),
        std::clamp(static_cast<std::int32_t>(std::floor(local.y)), 0, kSubVoxelResolution - 1),
        std::clamp(static_cast<std::int32_t>(std::floor(local.z)), 0, kSubVoxelResolution - 1)};
}

}  // namespace voxl::physics
