#pragma once

// Greedy mesher for partially destroyed (sub-voxel) blocks.
//
// This is the second, much smaller half of the meshing stage. GreedyMesher walks
// all 32^3 blocks of a chunk and knows nothing about damage; this walks only the
// handful of entries in the chunk's SubVoxelStore. For untouched terrain the
// store is empty and mesh() returns after a single `empty()` check, which is what
// keeps the feature free for the 99.9% of chunks nobody has hit with a pickaxe.
//
// WHY IT IS A SEPARATE SWEEP RATHER THAN A CASE INSIDE GreedyMesher
// ----------------------------------------------------------------
// The two produce different vertex formats into different buffers (see
// mesh/SubVoxelMesh.hpp for that argument), so they could not share the output
// path anyway. Keeping them apart also keeps the hot loop of GreedyMesher free of
// a per-block "is this block damaged?" branch that would be taken essentially
// never and would still cost a store lookup on the miss.
//
// MERGE SCOPE
// -----------
// Merging happens inside one block's 8x8x8 grid and never across two blocks. The
// packed format's 3-bit extents are sized for exactly that; see the MERGE SCOPE
// note in mesh/SubVoxelMesh.hpp for why the trade is deliberate.
//
// LIGHT AND AMBIENT OCCLUSION
// ---------------------------
// Both are computed ONCE PER (BLOCK, FACE DIRECTION) and shared by every quad of
// that block and direction - never per sub-voxel, as SubVoxelMesh.hpp requires.
//
// The parent block's OWN stored light is not usable: a solid opaque block is dark
// inside, so inheriting it literally would render every carved surface black
// while the intact wall beside it is lit. What an intact block's face actually
// uses is the light of the block ACROSS that face, and that is what is inherited
// here - one sample per direction, taken exactly where GreedyMesher takes it. A
// carved surface therefore lands on the same shading as the face it replaced.
// When the block across a face is opaque (a face pointing into a carved pocket,
// where there is no meaningful outside sample) the brightest open neighbour is
// used instead, so a pocket is dim rather than pitch black; the AO term, which is
// derived from the same geometry, is what actually darkens it.
//
// THREADING
// ---------
// Same contract as GreedyMesher: an instance is NOT thread safe (it carries a
// reuse hint and stats), the registry is captured by pointer and must outlive it,
// and nothing here touches OpenGL. Reads go through a ChunkNeighbourhood snapshot,
// so a concurrent main-thread edit to any chunk in the 3x3x3 is a use-after-free -
// see World::isEditBlocked. Carving a sub-voxel is a mutation and is subject to
// that rule exactly like setBlock.
//
// COST
// ----
// Linear in the number of damaged blocks, with a fixed per-block cost and no
// allocation once the output vectors are warm:
//   * 6 directions x 8 slices x 64 cells = 3072 occupancy bit tests, each a shift
//     and a mask on a 512-bit grid that fits in one cache line pair;
//   * 6 x (9 neighbourhood block lookups + 1 light lookup) for the AO ring and
//     the inherited light, i.e. 60 cross-chunk queries;
//   * the greedy pass, bounded by 6 x 8 x 8 = 384 rectangle probes.
// Measured on the reference machine (see the report accompanying this file):
// ~11 us per damaged block, so a chunk with 64 damaged blocks costs well under a
// millisecond - the same order as one ordinary GreedyMesher run, and it only ever
// happens on chunks the player has actively dug into.

#include "mesh/SubVoxelMesh.hpp"
#include "world/Block.hpp"
#include "world/BlockAccess.hpp"
#include "world/SubVoxel.hpp"
#include "world/VoxelTypes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace voxl {

/// Builds the sub-voxel geometry for the centre chunk of a neighbourhood.
class SubVoxelMesher {
public:
    struct Stats {
        /// Store entries that contributed geometry.
        std::size_t blocksMeshed = 0;
        /// Individual sub-voxel faces that survived culling.
        std::size_t facesEmitted = 0;
        /// Quads written after greedy merging; facesEmitted / quadsEmitted is the
        /// merge ratio, which is ~8 for a flat carved wall and 1 for gravel-like
        /// speckle damage.
        std::size_t quadsEmitted = 0;
    };

    /// The registry is only ever read, which is what makes one finalised registry
    /// shareable across every worker's mesher instance.
    explicit SubVoxelMesher(const BlockRegistry& registry) noexcept;

    /// Meshes every entry in `neighbourhood.centre()`'s SubVoxelStore into `out`.
    ///
    /// `out` is cleared but keeps its capacity, so pooling one SubVoxelMeshData
    /// per worker removes allocation from the steady state. Returns false when
    /// there is no geometry - no centre chunk, no damage, or damage that is
    /// entirely enclosed by solid material.
    bool mesh(const ChunkNeighbourhood& neighbourhood, SubVoxelMeshData& out);

    /// Convenience overload for tests and tooling; allocates a fresh mesh.
    [[nodiscard]] SubVoxelMeshData mesh(const ChunkNeighbourhood& neighbourhood);

    /// Meshes one damaged block and APPENDS to `out`.
    ///
    /// Exposed separately because it is the whole algorithm: `mesh()` is a loop
    /// over the store around it. Tests drive this directly so they can build an
    /// arbitrary SubVoxelGrid without going through the store's edit API, and a
    /// future incremental remesh (rebuild one damaged block, not the chunk) has
    /// the entry point it needs.
    ///
    /// `blockIndex` is a chunk-local index in localIndex() order and `grid` must
    /// satisfy the SubVoxel.hpp invariant: non-empty, not full, and
    /// `grid.material` equal to the block actually stored at `blockIndex`.
    void meshBlock(const ChunkNeighbourhood& neighbourhood, std::size_t blockIndex,
                   const SubVoxelGrid& grid, SubVoxelMeshData& out);

    [[nodiscard]] const Stats& lastStats() const noexcept { return m_stats; }
    [[nodiscard]] const BlockRegistry& registry() const noexcept { return *m_registry; }

private:
    /// Everything the sweep for one face direction needs to know about the block
    /// across that face. Resolved once per (block, direction) - the point of the
    /// struct is that the 512-iteration inner loop reads only these five fields
    /// instead of re-querying the neighbourhood.
    struct NeighbourFace {
        /// Non-null when the neighbouring block is itself partially destroyed, in
        /// which case occlusion is decided per sub-voxel against this grid rather
        /// than for the whole face.
        const SubVoxelGrid* grid = nullptr;

        /// BlockRegistry::facesHidden(material, neighbourMaterial): whether the
        /// neighbour's material hides a face at all. Combined with `grid`, a
        /// partial neighbour hides only the sub-voxels it still has.
        bool hidesFace = false;

        std::uint8_t sunlight   = 0;
        std::uint8_t blockLight = 0;
        std::uint8_t ao         = kMaxAoLevel;
    };

    /// Fills `out` for all six directions of the block at `blockLocal`.
    void resolveNeighbours(const ChunkNeighbourhood& neighbourhood, const BlockPos& blockLocal,
                           BlockId material,
                           std::array<NeighbourFace, kDirectionCount>& out) const;

    /// Builds and greedily merges the eight 8x8 face masks for one direction.
    /// `blockBase` is the block's origin in chunk-local SUB-VOXEL units.
    void sweep(Direction direction, const SubVoxelGrid& grid, const NeighbourFace& face,
               const std::array<std::int32_t, 3>& blockBase, std::uint16_t textureLayer,
               SubVoxelMeshData& out);

    const BlockRegistry* m_registry = nullptr;
    Stats                m_stats{};

    /// Quads emitted by the previous chunk, used to pre-size the next one. Damage
    /// is spatially clustered - a player digs a tunnel, not one block here and one
    /// a kilometre away - so consecutive chunks have similar counts and this
    /// removes almost every reallocation without ever reserving a worst case.
    std::size_t m_quadHint = 0;
};

}  // namespace voxl
