#pragma once

// Shared, deterministic world fixtures for the benchmark cases.
//
// Every fixture is a pure function of the seed, so two runs of the harness
// measure the same voxels. Anything that searches the world for a scene - "a
// plains chunk", "the cave-riddled section with the most surface area" - does so
// with a fixed scan order and returns the first/best hit, never a random pick:
// a benchmark whose input changes between runs cannot be diffed between commits.
//
// Fixture construction happens in a case's `setup`, which the harness does not
// time. Generating 27 neighbour chunks costs tens of milliseconds and would
// otherwise swamp the microseconds being measured.

#include "world/Block.hpp"
#include "world/BlockAccess.hpp"
#include "world/Chunk.hpp"
#include "world/Lod.hpp"
#include "world/TerrainGenerator.hpp"
#include "world/VoxelTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace voxl::bench {

// ------------------------------------------------------------- singletons --

/// The finalised default registry. Immutable, shared by every case and every
/// worker thread - which is the property BlockRegistry documents as the reason
/// it is safe to share at all.
[[nodiscard]] const BlockRegistry& registry();

/// Replaces the seed used by `generator()`. MUST be called before the first
/// `generator()` call - i.e. from argument parsing, before any case runs.
void setSeed(std::uint64_t seed);
[[nodiscard]] std::uint64_t seed() noexcept;

/// The shared generator. Immutable after construction and safe to sample from
/// any number of worker threads.
[[nodiscard]] const TerrainGenerator& generator();

// ------------------------------------------------------------ biome search --

/// A world column the generator classified as a particular biome, plus the
/// chunk section its surface falls in.
struct BiomeSite {
    bool         found    = false;
    BiomeId      biome    = BiomeId::Plains;
    std::int32_t worldX   = 0;
    std::int32_t worldZ   = 0;
    std::int32_t surfaceY = 0;
    /// Section containing `surfaceY`. This is the interesting chunk to mesh: a
    /// section entirely below the surface is solid stone and one entirely above
    /// is empty air, and neither says anything about the biome.
    ChunkPos surfaceSection{};
};

/// Scans chunk columns outward from the origin in a fixed spiral, sampling each
/// chunk's centre column, and returns the first that reports `biome`.
///
/// Sampling the centre column rather than every column keeps the search to one
/// `sampleColumn` per chunk. A biome that only ever occupies a few columns would
/// be missed by that, which is why the result carries `found` instead of
/// asserting - a case whose biome is not present in this seed reports itself
/// unavailable rather than silently measuring the wrong terrain.
[[nodiscard]] BiomeSite findBiomeSite(BiomeId biome, std::int32_t maxRadiusChunks = 96);

// ------------------------------------------------------- chunk construction --

/// Terrain-generated chunk at `level`, left in state Ready with its LOD recorded
/// so the mesher picks the same level up.
[[nodiscard]] ChunkPtr generateChunk(const ChunkPos& position, LodLevel level = kLodFull);

/// Single-material chunk, full sunlight. `optimise` is not needed: fill() puts
/// the storage straight into the uniform representation.
[[nodiscard]] ChunkPtr uniformChunk(const ChunkPos& position, BlockId fill);

/// Alternating solid/air on `(x + y + z) & 1`: the worst case greedy meshing can
/// be handed. Every solid block has six air neighbours, so nothing is culled and
/// no two faces are mergeable - the merge ratio is exactly 1.0 and the quad count
/// is the naive face count.
[[nodiscard]] ChunkPtr checkerboardChunk(const ChunkPos& position);

/// Chunk-local block indices of the `count` solid blocks the sub-voxel cases
/// damage, spread evenly through the chunk in localIndex order. Fewer than
/// `count` entries when the chunk does not hold that many solid blocks.
[[nodiscard]] std::vector<std::size_t> spreadSolidBlocks(const Chunk& chunk, std::size_t count);

// ---------------------------------------------------------- neighbourhoods --

/// A neighbourhood together with a WRITABLE handle on its centre chunk.
///
/// ChunkNeighbourhood stores `ConstChunkPtr`, which is right for meshing but
/// useless to a case that has to mutate the centre between runs (zeroing light
/// before a propagation pass, say). Handing back the ChunkPtr that went in is
/// the honest way to get that; const_casting the snapshot back to mutable would
/// hide exactly the kind of write the threading contract exists to forbid.
struct Scene {
    ChunkPtr           centre;
    ChunkNeighbourhood neighbourhood;
};

/// Centre chunk with all 26 neighbours generated at the same level. This is what
/// a real mesh job sees, and the only configuration in which cross-seam face
/// culling does any work.
[[nodiscard]] Scene generatedScene(const ChunkPos& centre, LodLevel level);

/// `generatedScene(...).neighbourhood`, for cases that never write to the centre.
[[nodiscard]] ChunkNeighbourhood generatedNeighbourhood(const ChunkPos& centre, LodLevel level);

/// Centre chunk alone. The 26 empty slots read as air (BlockAccess.hpp), so every
/// border face is emitted - the right framing for the synthetic solid and
/// checkerboard scenes, where surrounding material would cull away the thing
/// being measured.
[[nodiscard]] ChunkNeighbourhood isolated(const ChunkPtr& centre);

/// Centre chunk surrounded by 26 copies of a uniform material.
[[nodiscard]] ChunkNeighbourhood surrounded(const ChunkPtr& centre, BlockId material);

// ----------------------------------------------------------- scene metrics --

/// Non-air blocks in the chunk. The naive face count - what a mesher that did no
/// hidden-face removal and no merging would emit - is six times this.
[[nodiscard]] std::size_t countNonAir(const Chunk& chunk);

/// Faces on the boundary between a non-air and an air block, counting only pairs
/// wholly inside the chunk plus the chunk's own outer shell against air. This is
/// the count hidden-face removal alone gets you, before any greedy merging, and
/// it is what makes the cave scene worth its own case.
[[nodiscard]] std::size_t countExposedFaces(const Chunk& chunk);

/// Searches a fixed grid of underground sections and returns the one with the
/// most exposed faces, i.e. the most cave surface. Deterministic, and reported
/// with its face count so the document can say how dense "dense" was.
struct CaveSite {
    bool        found        = false;
    ChunkPos    position{};
    std::size_t exposedFaces = 0;
    std::size_t solidBlocks  = 0;
};
[[nodiscard]] CaveSite findDensestCaveSection(std::int32_t searchRadiusChunks = 3,
                                              std::int32_t sectionY          = 2);

}  // namespace voxl::bench
