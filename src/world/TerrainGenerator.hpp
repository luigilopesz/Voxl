#pragma once

// Procedural world generation: height field, biomes, caves, water and trees.
//
// DETERMINISM IS THE HEADLINE PROPERTY. Every voxel a chunk receives is a pure
// function of (world position, seed). Nothing in here depends on which chunk was
// generated first, on which thread ran it, or on how many chunks came before:
// there is no sequential RNG, no per-generator scratch state, and no cache. That
// is what lets the streaming system generate chunks out of order on N workers
// and still produce one coherent world, and it is what makes a save file
// optional rather than mandatory.
//
// THREAD SAFETY: TerrainGenerator is immutable after construction. Any number of
// worker threads may call the const members concurrently on one shared instance.

#include "world/Block.hpp"
#include "world/VoxelTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace voxl {

class Chunk;

/// Coarse climate/terrain classification of a world column.
///
/// Numeric values index the description table and may end up in debug UI, but
/// they are NOT written to disk (biomes are recomputed from the seed), so they
/// are cheaper to change than a BlockId.
enum class BiomeId : std::uint8_t {
    Ocean     = 0,
    Beach     = 1,
    Plains    = 2,
    Forest    = 3,
    Desert    = 4,
    Mountains = 5,
    Snowy     = 6,
};

inline constexpr std::size_t kBiomeCount = 7;

[[nodiscard]] constexpr const char* toString(BiomeId biome) noexcept
{
    switch (biome) {
        case BiomeId::Ocean:     return "ocean";
        case BiomeId::Beach:     return "beach";
        case BiomeId::Plains:    return "plains";
        case BiomeId::Forest:    return "forest";
        case BiomeId::Desert:    return "desert";
        case BiomeId::Mountains: return "mountains";
        case BiomeId::Snowy:     return "snowy";
    }
    return "unknown";
}

/// Furthest a structure rooted in one column can reach horizontally.
///
/// This is a hard contract with the generator's structure pass: a chunk samples
/// its columns padded by this radius so that a tree rooted outside the chunk
/// still deposits its canopy inside it. Raising it without raising the padding
/// would reintroduce trees sliced off at chunk borders.
inline constexpr std::int32_t kMaxStructureRadius = 3;

/// Furthest a structure rooted at surface height `s` can reach vertically; its
/// topmost voxel is at `s + kMaxStructureHeight`. Used to clamp the height field
/// away from the world ceiling so no structure is ever truncated.
inline constexpr std::int32_t kMaxStructureHeight = 12;

/// Static, tuned description of one biome. Read-only after start-up.
struct BiomeDescription {
    const char* name = "plains";

    /// Block placed at the topmost terrain voxel of a column.
    BlockId surface = blocks::Grass;
    /// Block placed for `subsurfaceDepth` voxels directly beneath the surface.
    BlockId      subsurface      = blocks::Dirt;
    std::int32_t subsurfaceDepth = 4;
    /// Block placed for a further `fillerDepth` voxels; stone below that. Lets
    /// deserts sit on sandstone without a special case in the column loop.
    BlockId      filler      = blocks::Stone;
    std::int32_t fillerDepth = 0;

    /// Probability in [0,1] that this biome's tree-grid candidate sprouts. Not a
    /// per-column probability: see the structure pass in the .cpp.
    float treeDensity = 0.0f;

    /// Height offset in voxels, blended across biomes rather than applied at a
    /// hard boundary. See TerrainGenerator's height-field comment.
    float heightBias = 0.0f;
    /// Amplitude in voxels of the high-frequency detail octave, also blended.
    float detailAmplitude = 3.0f;
};

[[nodiscard]] const BiomeDescription& biomeDescription(BiomeId biome) noexcept;

/// Tunables that a world's metadata may pin so that a save reproduces exactly.
struct TerrainSettings {
    /// Any 64-bit value. It is hashed per noise field, so adjacent seeds produce
    /// completely unrelated worlds rather than shifted ones.
    std::uint64_t seed = 0x764F0117A2C5D3B9ull;

    /// Water surface height. Defaults to the world-wide kSeaLevel; overriding it
    /// is supported for tests and flat/creative presets.
    std::int32_t seaLevel = kSeaLevel;

    bool generateCaves = true;
    bool generateTrees = true;

    /// Writes a coarse sunlight seed (full sky light above the terrain, damped
    /// through water, zero underground) so that a freshly generated chunk is
    /// visible before the real lighting pass has run. The lighting system
    /// overwrites this; it is not an attempt at correct light.
    bool seedSunlight = true;
};

/// Everything the generator derives about one world column, in one place so the
/// terrain pass, the structure pass and the debug overlay agree.
struct ColumnSample {
    /// Topmost terrain voxel. May be below `seaLevel`, in which case the column
    /// is submerged. Always within [kWorldMinY + 1, kWorldMaxY - kMaxStructureHeight - 2].
    std::int32_t surfaceY = 0;
    /// Highest voxel any structure rooted in this column can occupy. Cheap
    /// conservative bound used to skip empty sky sections outright.
    std::int32_t structureTopY = 0;

    float continentalness = 0.0f;  ///< [-1,1], large-scale land/ocean mass.
    float erosion         = 0.0f;  ///< [-1,1], high means flattened.
    float mountainFactor  = 0.0f;  ///< [0,1], how much of the height is ridge uplift.
    float temperature     = 0.0f;  ///< [-1,1].
    float humidity        = 0.0f;  ///< [-1,1].

    BiomeId biome = BiomeId::Plains;
};

/// Builds terrain for one chunk at a time.
///
/// PINNED: non-copyable and non-movable. Workers hold a `const TerrainGenerator&`
/// for the lifetime of the world, so the object must not relocate; there is also
/// no reason to duplicate the noise tables.
class TerrainGenerator {
public:
    explicit TerrainGenerator(const TerrainSettings& settings = {});
    ~TerrainGenerator();

    TerrainGenerator(const TerrainGenerator&)            = delete;
    TerrainGenerator& operator=(const TerrainGenerator&) = delete;
    TerrainGenerator(TerrainGenerator&&)                 = delete;
    TerrainGenerator& operator=(TerrainGenerator&&)      = delete;

    /// Fills `chunk` with terrain, water, caves and structures.
    ///
    /// Callable from a worker thread that owns the chunk (state == Generating).
    /// The caller performs the Generating -> Generated transition; the generator
    /// deliberately does not touch the state machine so that it can also be used
    /// by tests and tooling on a bare Chunk.
    ///
    /// Idempotent: the chunk's voxels and light are reset first, so regenerating
    /// an already-populated chunk yields the same result as generating a fresh
    /// one. Leaves the chunk marked dirty for meshing.
    void generate(Chunk& chunk) const;

    /// Pure function of (worldX, worldZ, seed). The whole generator is built on
    /// this; it allocates nothing and is safe to call from any thread.
    [[nodiscard]] ColumnSample sampleColumn(std::int32_t worldX, std::int32_t worldZ) const noexcept;

    [[nodiscard]] std::int32_t surfaceHeight(std::int32_t worldX, std::int32_t worldZ) const noexcept;
    [[nodiscard]] BiomeId biomeAt(std::int32_t worldX, std::int32_t worldZ) const noexcept;

    [[nodiscard]] const TerrainSettings& settings() const noexcept { return m_settings; }
    [[nodiscard]] std::uint64_t seed() const noexcept { return m_settings.seed; }

private:
    struct NoiseFields;

    /// True when a cave should replace the terrain voxel at this position.
    /// `surfaceY` comes from the column sample and is what keeps caves from
    /// venting into the sky.
    [[nodiscard]] bool carvesCave(std::int32_t worldX, std::int32_t worldY, std::int32_t worldZ,
                                 std::int32_t surfaceY) const noexcept;

    TerrainSettings m_settings;
    /// Held behind a pointer purely to keep FastNoiseLite out of this header;
    /// const because the noise fields are never reconfigured after construction,
    /// which is what makes concurrent sampling safe.
    std::unique_ptr<const NoiseFields> m_noise;
};

}  // namespace voxl
