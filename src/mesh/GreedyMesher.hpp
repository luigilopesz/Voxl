#pragma once

// Greedy voxel mesher: one ChunkNeighbourhood in, one ChunkMeshData out.
//
// The mesher is the single hottest CPU stage in the engine - it runs for every
// chunk that enters the view distance and again for every edit - so the design
// is dominated by two decisions:
//
//  1. The 34^3 voxel region the sweep needs (32^3 centre plus a one-block skirt
//     from the neighbours, because face culling reads across the seam and
//     ambient occlusion reads the diagonals) is copied into flat scratch arrays
//     ONCE per chunk. Every subsequent read is an array index instead of a
//     palette lookup behind three bounds checks, which is what turns ~1.8M
//     neighbour queries from milliseconds into microseconds.
//
//  2. Merge compatibility is reduced to a single 64-bit key per face, so the
//     greedy rectangle expansion compares one integer instead of a struct. See
//     the key layout in the .cpp.
//
// Nothing here touches OpenGL: this runs on JobSystem worker threads, where GL
// calls are forbidden.

#include "mesh/MeshData.hpp"
#include "world/Block.hpp"
#include "world/BlockAccess.hpp"
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
        /// Individual block faces that survived hidden-face removal.
        std::size_t facesEmitted = 0;
        /// Quads actually written after greedy merging; facesEmitted /
        /// quadsEmitted is the merge ratio the debug overlay shows.
        std::size_t quadsEmitted = 0;
    };

    explicit GreedyMesher(const BlockRegistry& registry);
    ~GreedyMesher();

    GreedyMesher(const GreedyMesher&)            = delete;
    GreedyMesher& operator=(const GreedyMesher&) = delete;
    GreedyMesher(GreedyMesher&&) noexcept;
    GreedyMesher& operator=(GreedyMesher&&) noexcept;

    /// Meshes `neighbourhood.centre()` into `out`.
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
    /// Copies the 34^3 region into the flat scratch arrays.
    void loadCache(const ChunkNeighbourhood& neighbourhood);

    /// Builds the per-slice face mask for one of the six face directions and
    /// merges it into quads.
    void sweep(Direction direction, ChunkMeshData& out);

    /// Writes one merged quad. `n` is the slice index along the face normal
    /// axis, (`u`, `v`) the quad origin in the slice's tangent axes, `w` x `h`
    /// its extent, and `key` the packed merge key it was built from.
    void emitQuad(ChunkMeshData& out, Direction direction, std::int32_t n, std::int32_t u,
                  std::int32_t v, std::int32_t w, std::int32_t h, std::uint64_t key);

    /// Culling/occlusion flags per block id, snapshotted from the registry at
    /// construction. Legal because the registry is immutable once finalised.
    [[nodiscard]] std::uint8_t blockFlags(BlockId id) const noexcept;

    const BlockRegistry*                   m_registry = nullptr;
    std::unique_ptr<detail::MesherScratch> m_scratch;
    std::vector<std::uint8_t>              m_blockFlags;
    Stats                                  m_stats{};

    /// Quads emitted per render layer by the previous chunk, used to pre-size
    /// the next one. Neighbouring chunks have very similar complexity, so this
    /// removes almost every reallocation from a streaming run without ever
    /// reserving a worst case that never materialises.
    std::array<std::size_t, kRenderLayerCount> m_quadHint{};
};

}  // namespace voxl
