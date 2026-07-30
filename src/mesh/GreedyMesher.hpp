#pragma once

// Greedy voxel mesher: one ChunkNeighbourhood in, one ChunkMeshData out.
//
// The mesher is the single hottest CPU stage in the engine - it runs for every
// chunk that enters the view distance and again for every edit - so the design
// is dominated by two decisions:
//
//  1. The (G+2)^3 cell region the sweep needs (G^3 centre plus a one-cell skirt
//     from the neighbours, because face culling reads across the seam and
//     ambient occlusion reads the diagonals) is copied into flat scratch arrays
//     ONCE per chunk. Every subsequent read is an array index instead of a
//     palette lookup behind three bounds checks, which is what turns ~1.8M
//     neighbour queries from milliseconds into microseconds. G is 32 at full
//     resolution, and 32 >> level otherwise.
//
//  2. Merge compatibility is reduced to a single 64-bit key per face, so the
//     greedy rectangle expansion compares one integer instead of a struct. See
//     the key layout in the .cpp.
//
// LEVEL OF DETAIL
// ---------------
// A chunk is meshed at Chunk::lod(). The sweep runs over the (32 >> L)^3 cell
// grid and every quad corner is scaled back into BLOCK space by 2^L, so the
// packed vertex format of MeshData.hpp is untouched: a cell boundary is always a
// multiple of 2^L, hence still an integer block coordinate in [0, 32], and a
// greedy quad still cannot exceed 32 blocks on a side. See world/Lod.hpp.
//
// Nothing here touches OpenGL: this runs on JobSystem worker threads, where GL
// calls are forbidden.

#include "mesh/MeshData.hpp"
#include "world/Block.hpp"
#include "world/BlockAccess.hpp"
#include "world/Lod.hpp"
#include "world/VoxelTypes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace voxl {

namespace detail {
/// Flat scratch arrays owned by a GreedyMesher instance. Defined in the .cpp;
/// heap-allocated so that a GreedyMesher stays cheap to move and does not put
/// ~160 KB of arrays inline in whatever holds it.
struct MesherScratch;
}  // namespace detail

// ----------------------------------------------------------- water surface --

/// How far below the top of its voxel a water surface is drawn, in blocks.
///
/// THIS IS APPLIED BY THE VERTEX SHADER, NOT BY THE MESHER. The packed vertex
/// format (src/mesh/MeshData.hpp) stores positions as 6-bit integers, so there
/// is no way to express a fractional Y here; the mesher emits the water top face
/// at the full block height and the renderer subtracts this from any vertex
/// whose direction is Direction::PosY and whose texture layer equals
/// waterSurfaceTextureLayer(). Pass that layer to the shader as a uniform.
///
/// 1/8 of a block is the smallest drop that still reads as a surface at a
/// glance while keeping the gap invisible against an adjacent solid block.
inline constexpr float kWaterSurfaceDropBlocks = 0.125f;

/// Texture-array layer used by water's top face; the value the shader compares
/// against to recognise a water-surface quad. Looked up rather than hard-coded
/// so a registry that renumbers textures cannot desynchronise the two.
[[nodiscard]] std::uint16_t waterSurfaceTextureLayer(const BlockRegistry& registry) noexcept;

// ------------------------------------------------------ LOD cell occupancy --

/// Minimum number of non-air blocks a level-L cell must contain before the
/// mesher treats the cell as solid.
///
/// Exported so tests, tooling and the terrain generator all round
/// kLodSolidThreshold the same way; deriving it twice is how two subsystems end
/// up disagreeing about where a coarse surface is. The rounding is `ceil`, and
/// the result is never less than 1, so a level whose cell holds few enough
/// blocks that the threshold rounds to zero still cannot invent solid air.
///
/// Level 0 returns 1: a cell is one block and is solid exactly when it is not
/// air, which is the pre-LOD behaviour.
[[nodiscard]] std::int32_t lodCellSolidBlocks(LodLevel level) noexcept;

// ------------------------------------------------------------ the mesher --

/// Builds the renderable geometry for the centre chunk of a neighbourhood.
///
/// THREAD SAFETY: an instance owns mutable scratch buffers and is NOT thread
/// safe. Give every worker thread its own instance (a member of the worker's
/// job context, or a `thread_local`) and reuse it across chunks - constructing
/// one allocates the scratch arrays, so a per-call instance throws away the
/// whole point.
///
/// The registry is captured by pointer and must outlive the mesher. It is only
/// ever read, which is what makes sharing one finalised registry across all
/// worker instances safe.
class GreedyMesher {
public:
    struct Stats {
        /// Individual cell faces that survived hidden-face removal. At level 0 a
        /// cell is a block, so this is the block-face count as before.
        std::size_t facesEmitted = 0;
        /// Quads actually written after greedy merging; facesEmitted /
        /// quadsEmitted is the merge ratio the debug overlay shows. Includes the
        /// skirt quads counted separately below.
        std::size_t quadsEmitted = 0;
        /// Border-curtain quads included in `quadsEmitted`. Always 0 at level 0.
        std::size_t skirtQuads = 0;
        /// Level the last chunk was meshed at, i.e. Chunk::lod() clamped to
        /// kLodMax.
        LodLevel level = kLodFull;
    };

    explicit GreedyMesher(const BlockRegistry& registry);
    ~GreedyMesher();

    GreedyMesher(const GreedyMesher&)            = delete;
    GreedyMesher& operator=(const GreedyMesher&) = delete;
    GreedyMesher(GreedyMesher&&) noexcept;
    GreedyMesher& operator=(GreedyMesher&&) noexcept;

    /// Meshes `neighbourhood.centre()` into `out`, at the centre chunk's
    /// Chunk::lod().
    ///
    /// `out` is cleared first but keeps its capacity, so pooling ChunkMeshData
    /// objects across calls removes essentially all allocation from the steady
    /// state. Returns false when no geometry was produced (all-air chunk, fully
    /// occluded chunk, or a neighbourhood with no centre), in which case `out`
    /// holds only `position` and `contentVersion`.
    ///
    /// An incomplete neighbourhood is meshed rather than rejected: unloaded
    /// neighbours read as air (see BlockAccess.hpp), which over-emits seam faces
    /// instead of punching permanent holes. Deciding whether that is acceptable
    /// belongs to the scheduler - check ChunkNeighbourhood::complete() there.
    bool mesh(const ChunkNeighbourhood& neighbourhood, ChunkMeshData& out);

    /// Convenience overload for tests and tooling; allocates a fresh mesh.
    [[nodiscard]] ChunkMeshData mesh(const ChunkNeighbourhood& neighbourhood);

    [[nodiscard]] const Stats& lastStats() const noexcept { return m_stats; }

    [[nodiscard]] const BlockRegistry& registry() const noexcept { return *m_registry; }

private:
    /// Copies the (G+2)^3 region into the flat scratch arrays. `conservativeFace`
    /// is indexed by Direction and marks the chunk faces whose neighbour renders
    /// at a different resolution; those are loaded as air so nothing culls
    /// against them. See the long comment at the definition.
    void loadCache(const ChunkNeighbourhood& neighbourhood);

    /// Level-0 loader: a straight 34^3 copy, plus the sub-voxel and
    /// differing-neighbour patches.
    void loadCacheFull(const ChunkNeighbourhood&                neighbourhood,
                       const bool (&conservativeFace)[kDirectionCount]);

    /// Level > 0 loader: downsamples 2^L cubes of blocks into cells.
    void loadCacheLod(const ChunkNeighbourhood&                neighbourhood,
                      const bool (&conservativeFace)[kDirectionCount]);

    /// Collapses the 2^L cube of blocks whose minimum corner is
    /// (`blockX`, `blockY`, `blockZ`) - centre-chunk-local block coordinates,
    /// possibly negative - into one cell written at `destination`.
    void reduceCell(const ChunkNeighbourhood& neighbourhood, std::int32_t blockX,
                    std::int32_t blockY, std::int32_t blockZ, std::size_t destination);

    /// Builds the per-slice face mask for one of the six face directions and
    /// merges it into quads.
    void sweep(Direction direction, ChunkMeshData& out);

    /// Hangs the level > 0 border curtain. No-op at level 0.
    void emitSkirts(ChunkMeshData& out);

    /// Writes one merged quad. `n` is the slice index along the face normal
    /// axis, (`u`, `v`) the quad origin in the slice's tangent axes, `w` x `h`
    /// its extent - all in CELLS - and `key` the packed merge key it was built
    /// from.
    void emitQuad(ChunkMeshData& out, Direction direction, std::int32_t n, std::int32_t u,
                  std::int32_t v, std::int32_t w, std::int32_t h, std::uint64_t key);

    /// Writes one quad whose origin and extents are already in BLOCK space. The
    /// single place a PackedVertex is produced, so the packed-field bounds are
    /// asserted exactly once.
    void emitQuadBlocks(ChunkMeshData& out, Direction direction, const std::int32_t (&base)[3],
                        std::int32_t widthBlocks, std::int32_t heightBlocks, std::uint64_t key);

    /// Culling/occlusion flags per block id, snapshotted from the registry at
    /// construction. Legal because the registry is immutable once finalised.
    [[nodiscard]] std::uint8_t blockFlags(BlockId id) const noexcept;

    const BlockRegistry*                   m_registry = nullptr;
    std::unique_ptr<detail::MesherScratch> m_scratch;
    std::vector<std::uint8_t>              m_blockFlags;
    Stats                                  m_stats{};

    /// Quads emitted per render layer by the previous chunk AT THE SAME LEVEL,
    /// used to pre-size the next one. Neighbouring chunks have very similar
    /// complexity, so this removes almost every reallocation from a streaming
    /// run without ever reserving a worst case that never materialises. Keyed by
    /// level as well because a level-3 chunk holds two orders of magnitude fewer
    /// quads than a level-0 one and would otherwise reserve for it.
    std::array<std::array<std::size_t, kRenderLayerCount>, kLodCount> m_quadHint{};
};

}  // namespace voxl
