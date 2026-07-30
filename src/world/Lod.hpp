#pragma once

// Distance-based chunk level of detail.
//
// THIS HEADER IS A CONTRACT. Terrain generation, meshing, streaming and the
// renderer all agree on the level numbering and the selection policy here.
//
// A chunk at level L represents its 32^3 blocks as a grid of (32 >> L)^3 cells,
// each cell spanning 2^L blocks on a side. Level 0 is full resolution and is
// what the player interacts with; higher levels exist purely to make distant
// terrain cheap to generate, mesh, upload and draw.
//
// WHY THE PACKED VERTEX FORMAT DOES NOT CHANGE: a level-L cell boundary always
// lands on a multiple of 2^L, so every emitted quad corner is still an integer
// block coordinate in [0, 32]. That is exactly what MeshData.hpp's 6-bit
// position fields already encode, and a greedy quad still cannot exceed 32
// blocks on a side. LOD therefore costs no vertex bits and no shader change -
// only the mesher's sampling step and the streaming policy differ.

#include "world/VoxelTypes.hpp"

#include <cstdint>

namespace voxl {

/// 0 = full resolution. The maximum is bounded by the chunk size: at level 5 a
/// single cell would span the whole 32-block chunk, which carries no shape
/// information at all, so 3 (4x4x4 cells of 8 blocks) is the practical floor.
using LodLevel = std::uint8_t;

inline constexpr LodLevel kLodFull  = 0;
inline constexpr LodLevel kLodCount = 4;   ///< levels 0..3 are valid
inline constexpr LodLevel kLodMax   = kLodCount - 1;

/// Blocks spanned by one cell at this level: 1, 2, 4, 8.
[[nodiscard]] constexpr std::int32_t lodCellSize(LodLevel level) noexcept
{
    return std::int32_t{1} << level;
}

/// Cells along one chunk axis at this level: 32, 16, 8, 4.
[[nodiscard]] constexpr std::int32_t lodGridSize(LodLevel level) noexcept
{
    return kChunkSize >> level;
}

/// Cells in a whole chunk at this level: 32768, 4096, 512, 64.
[[nodiscard]] constexpr std::size_t lodCellCount(LodLevel level) noexcept
{
    const auto n = static_cast<std::size_t>(lodGridSize(level));
    return n * n * n;
}

/// Selection policy: which level a chunk should be drawn at, given its
/// horizontal distance from the player in chunks.
///
/// HYSTERESIS IS NOT OPTIONAL. With a single threshold per level, a player
/// walking along a boundary re-generates and re-meshes the same ring of chunks
/// every few steps - the same thrash the streaming radius already guards
/// against, but far more expensive because each transition rebuilds geometry.
/// `promoteAt` (moving to a finer level) is strictly nearer than `demoteAt`
/// (moving to a coarser one), so a chunk must be pushed a full band past the
/// boundary before it changes back.
struct LodPolicy {
    /// Distance in chunks at which each level BEGINS, walking outward. Index i
    /// is the distance beyond which level i+1 is used. Must be ascending.
    std::int32_t bandStart[kLodMax] = {5, 9, 14};

    /// Extra distance a chunk must travel outward past a band edge before it is
    /// demoted, and inward before it is promoted.
    ///
    /// CONSTRAINT: every band must be at least `hysteresis + 2` chunks wide.
    ///
    /// The two arms of levelFor() measure from opposite edges of a band: demotion
    /// tests `distance > bandStart[current] + hysteresis`, promotion tests
    /// `distance < bandStart[target] - hysteresis`. If a band is narrower than
    /// that, the two conditions leave a gap of distances satisfying neither, and
    /// a chunk that lands in it can be demoted but can never be promoted back -
    /// it stays coarse forever, silently, even as the player walks right up to
    /// it. The shipped defaults {5, 9, 14} with hysteresis 2 give widths 4, 5, 5
    /// against a minimum of 4, so the first band sits exactly on the limit: any
    /// narrowing of it, or any increase to hysteresis, breaks the invariant.
    /// ChunkManager::setLodPolicy clamps and warns rather than trusting a caller
    /// to have checked.
    std::int32_t hysteresis = 2;

    /// Set false to pin the whole world to level 0, for benchmarking a
    /// like-for-like comparison against the pre-LOD renderer.
    bool enabled = true;

    /// Level for a chunk `distanceInChunks` away that is not currently resident,
    /// i.e. with no previous level to be sticky about.
    [[nodiscard]] constexpr LodLevel levelFor(std::int32_t distanceInChunks) const noexcept
    {
        if (!enabled) {
            return kLodFull;
        }
        LodLevel level = kLodFull;
        for (LodLevel i = 0; i < kLodMax; ++i) {
            if (distanceInChunks > bandStart[i]) {
                level = static_cast<LodLevel>(i + 1);
            }
        }
        return level;
    }

    /// Level for a chunk that is already resident at `current`, applying the
    /// hysteresis band. Returns `current` when the chunk has not moved far
    /// enough to justify rebuilding it.
    [[nodiscard]] constexpr LodLevel levelFor(std::int32_t distanceInChunks,
                                              LodLevel current) const noexcept
    {
        if (!enabled) {
            return kLodFull;
        }
        const LodLevel target = levelFor(distanceInChunks);
        if (target == current) {
            return current;
        }
        // Require the chunk to be clear of the band edge by the hysteresis
        // margin, in whichever direction it is moving.
        if (target > current) {
            // Demoting: the edge being crossed is the start of `current + 1`.
            const std::int32_t edge = bandStart[current];
            return distanceInChunks > edge + hysteresis ? target : current;
        }
        // Promoting: the edge is the start of the level we are leaving.
        const std::int32_t edge = bandStart[target];
        return distanceInChunks < edge - hysteresis ? target : current;
    }
};

/// How a 2^L cube of blocks collapses into one cell.
///
/// Majority vote weighted toward solidity: a cell is solid when at least
/// `kLodSolidThreshold` of its blocks are solid, and takes the most common
/// solid material among them. Air wins ties only when the cell is mostly air.
///
/// Biasing toward solid is deliberate. A cell that erases a thin solid feature
/// leaves a hole the player can see through to the sky; a cell that invents a
/// little extra rock is invisible at the distances LOD applies to. Holes are
/// the defect people notice.
inline constexpr float kLodSolidThreshold = 0.4f;

/// Skirt depth, in blocks, extruded downward around the border of a chunk that
/// is coarser than kLodFull.
///
/// A coarse chunk's surface steps in 2^L increments while its finer neighbour
/// steps in 1, so their border heights disagree by up to 2^L - 1 blocks and the
/// player sees a crack straight through to the void. Rather than stitching the
/// two resolutions - which for cube geometry means emitting a transition band
/// that greedy meshing cannot merge - each coarse chunk hangs a curtain of
/// geometry down from its border. It is a few hundred extra triangles per chunk
/// against thousands for a stitched seam, and it cannot crack.
[[nodiscard]] constexpr std::int32_t lodSkirtDepth(LodLevel level) noexcept
{
    return level == kLodFull ? 0 : lodCellSize(level);
}

}  // namespace voxl
