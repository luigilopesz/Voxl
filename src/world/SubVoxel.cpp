// Sub-voxel store mutations and world-position splitting.
//
// Everything here exists to keep the invariant at the top of world/SubVoxel.hpp
// true after every single mutation. The store deliberately knows nothing about
// ChunkStorage, so each function below is written so that the map is never left
// in a state that violates the invariant even transiently - there is no window
// in which an entry is full or empty and still present.

#include "world/SubVoxel.hpp"

#include "core/Log.hpp"

#include <algorithm>
#include <cmath>

namespace voxl {
namespace {

/// Materialising an empty grid in add() would immediately be full at a
/// resolution of one, which would need the erase-on-full path to run before the
/// entry is ever inserted. 512 makes that unreachable; this pins the assumption.
static_assert(kSubVoxelCount > 1, "add() assumes a freshly materialised grid cannot already be full");

static_assert(kSubVoxelResolution * kSubVoxelResolution * kSubVoxelResolution ==
                  static_cast<std::int32_t>(kSubVoxelCount),
              "kSubVoxelCount must be the cube of the resolution");
static_assert((kSubVoxelResolution & kSubVoxelMask) == 0,
              "the resolution must be a power of two for the shift/mask split");
static_assert(std::int32_t{1} << kSubVoxelShift == kSubVoxelResolution,
              "kSubVoxelShift must match kSubVoxelResolution");

/// The sub-voxel offset of `coordinate` inside the block that starts at `block`.
///
/// `block` has already been floored by worldToBlockPos, so `coordinate - block`
/// is the fractional part even at negative coordinates - which is the whole
/// point. Casting `coordinate * 8` straight to int would truncate toward zero
/// and mirror every negative position onto the wrong sub-voxel, the same bug
/// class as truncating a block coordinate.
[[nodiscard]] std::int32_t subVoxelAxis(float coordinate, std::int32_t block) noexcept
{
    const float fraction = coordinate - static_cast<float>(block);
    const auto  offset   = static_cast<std::int32_t>(
        std::floor(fraction * static_cast<float>(kSubVoxelResolution)));

    // Clamp rather than trust the arithmetic. A coordinate a hair below an
    // integer (say -1e-45f) is floored to the block below, and adding that
    // block back rounds to exactly 1.0f in float, which would index one past
    // the last sub-voxel. Doing the subtraction in double would only move the
    // boundary, not remove it.
    return std::clamp(offset, 0, kSubVoxelResolution - 1);
}

}  // namespace

// ------------------------------------------------------------------- store --

SubVoxelEdit SubVoxelStore::remove(std::size_t blockIndex, BlockId blockId, std::size_t subIndex)
{
    VOXL_ASSERT(blockIndex < kChunkVolume, "sub-voxel block index out of range");
    VOXL_ASSERT(subIndex < kSubVoxelCount, "sub-voxel index out of range");
    VOXL_ASSERT(blockId != blocks::Air, "cannot carve a sub-voxel out of air");

    const auto key = static_cast<std::uint16_t>(blockIndex);
    const auto it  = lowerBound(key);

    if (it == m_grids.end() || it->blockIndex != key) {
        // No entry means the block is uniform, and the caller has told us it is
        // not air, so all 512 sub-voxels are present. Materialise them and drop
        // one. The grid is built and cleared before insertion so a full grid is
        // never visible in the store. Inserting at the lower bound is what keeps
        // the vector sorted.
        SubVoxelGrid grid = SubVoxelGrid::solid(blockId);
        grid.clear(subIndex);
        m_grids.insert(it, Entry{key, grid});
        return SubVoxelEdit::Modified;
    }

    SubVoxelGrid& grid = it->grid;
    VOXL_ASSERT(grid.material == blockId, "sub-voxel material diverged from the block id");

    if (!grid.test(subIndex)) {
        return SubVoxelEdit::Unchanged;
    }

    grid.clear(subIndex);
    if (grid.empty()) {
        // Last one gone: the block itself is now air. The entry must go with it,
        // otherwise the store would claim a partial block where storage says air.
        m_grids.erase(it);
        releaseIfEmpty();
        return SubVoxelEdit::BlockRemoved;
    }
    return SubVoxelEdit::Modified;
}

SubVoxelEdit SubVoxelStore::add(std::size_t blockIndex, BlockId blockId, std::size_t subIndex)
{
    VOXL_ASSERT(blockIndex < kChunkVolume, "sub-voxel block index out of range");
    VOXL_ASSERT(subIndex < kSubVoxelCount, "sub-voxel index out of range");
    VOXL_ASSERT(blockId != blocks::Air, "cannot restore a sub-voxel of air");

    const auto key = static_cast<std::uint16_t>(blockIndex);
    const auto it  = lowerBound(key);

    if (it == m_grids.end() || it->blockIndex != key) {
        // Mirror image of remove(). An absent entry means the block is uniform,
        // and the store cannot tell air from whole-and-solid on its own - only
        // ChunkStorage knows. Chunk::restoreSubVoxel is the sole supported
        // caller and short-circuits the whole-and-solid case, so reaching here
        // means the block is air and the grid materialises EMPTY.
        SubVoxelGrid grid;
        grid.material = blockId;
        grid.set(subIndex);
        m_grids.insert(it, Entry{key, grid});
        return SubVoxelEdit::Modified;
    }

    SubVoxelGrid& grid = it->grid;
    VOXL_ASSERT(grid.material == blockId, "sub-voxel material diverged from the block id");

    if (grid.test(subIndex)) {
        return SubVoxelEdit::Unchanged;
    }

    grid.set(subIndex);
    if (grid.full()) {
        // Whole again. Leaving a full entry behind would cost an Entry per
        // repaired block and, worse, keep isBlockWhole() reporting false so the
        // mesher would never re-merge the block into its neighbours.
        m_grids.erase(it);
        releaseIfEmpty();
        return SubVoxelEdit::BlockRestored;
    }
    return SubVoxelEdit::Modified;
}

void SubVoxelStore::erase(std::size_t blockIndex)
{
    const auto key = static_cast<std::uint16_t>(blockIndex);
    const auto it  = lowerBound(key);
    if (it != m_grids.end() && it->blockIndex == key) {
        m_grids.erase(it);
        releaseIfEmpty();
    }
}

std::vector<std::pair<std::uint16_t, SubVoxelGrid>> SubVoxelStore::sortedEntries() const
{
    // The backing vector is maintained in ascending block-index order by every
    // mutation, so this is a straight copy rather than a sort. Block indices are
    // unique, so that order is total and does not depend on insertion history -
    // which is what makes a save file byte-identical for identical worlds.
    std::vector<std::pair<std::uint16_t, SubVoxelGrid>> entries;
    entries.reserve(m_grids.size());
    for (const Entry& entry : m_grids) {
        entries.emplace_back(entry.blockIndex, entry.grid);
    }
    VOXL_ASSERT(std::is_sorted(entries.begin(), entries.end(),
                               [](const auto& lhs, const auto& rhs) noexcept {
                                   return lhs.first < rhs.first;
                               }),
                "sub-voxel store lost its ordering");
    return entries;
}

// ------------------------------------------------------- position splitting --

SubVoxelHit toSubVoxel(const glm::vec3& worldPosition) noexcept
{
    SubVoxelHit hit;
    hit.block = worldToBlockPos(worldPosition);
    hit.sx    = subVoxelAxis(worldPosition.x, hit.block.x);
    hit.sy    = subVoxelAxis(worldPosition.y, hit.block.y);
    hit.sz    = subVoxelAxis(worldPosition.z, hit.block.z);
    return hit;
}

}  // namespace voxl
