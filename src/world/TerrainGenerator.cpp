#include "world/TerrainGenerator.hpp"

#include "core/Log.hpp"
#include "world/Chunk.hpp"
#include "world/ChunkStorage.hpp"

#include "FastNoiseLite.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace voxl {
namespace {

// ============================================================================
//  Small deterministic maths helpers
// ============================================================================

[[nodiscard]] constexpr float saturate(float v) noexcept
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

[[nodiscard]] constexpr float lerp(float a, float b, float t) noexcept
{
    return a + (b - a) * t;
}

/// Hermite ramp between two edges. `edge0 > edge1` is legal and inverts the
/// ramp, which is how the "cold" and "dry" climate memberships are expressed.
[[nodiscard]] constexpr float smoothstep(float edge0, float edge1, float x) noexcept
{
    const float t = saturate((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

/// Floor division. `/` truncates toward zero, which would make the tree grid
/// cells straddle x == 0 and duplicate a row of trees along that line.
[[nodiscard]] constexpr std::int32_t floorDiv(std::int32_t a, std::int32_t b) noexcept
{
    const std::int32_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

// ============================================================================
//  Integer hashing
// ============================================================================
//
// Structure placement must not use a sequential RNG: chunks are generated out of
// order on several threads, so "the Nth random number" is not a stable concept.
// Everything stochastic here is instead a stateless hash of the world position
// and the seed, which is by construction independent of call order.

[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t v) noexcept
{
    // MurmurHash3 finaliser: full avalanche in three rounds, so neighbouring
    // block coordinates produce unrelated values instead of a visible lattice.
    v ^= v >> 33;
    v *= 0xFF51AFD7ED558CCDull;
    v ^= v >> 33;
    v *= 0xC4CEB9FE1A85EC53ull;
    v ^= v >> 33;
    return v;
}

[[nodiscard]] constexpr std::uint64_t hash2(std::int32_t x, std::int32_t z, std::uint32_t salt,
                                            std::uint64_t seed) noexcept
{
    std::uint64_t h = seed ^ (static_cast<std::uint64_t>(salt) * 0x165667B19E3779F9ull);
    h = mix64(h ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) * 0x9E3779B97F4A7C15ull));
    h = mix64(h ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(z)) * 0xC2B2AE3D27D4EB4Full));
    return h;
}

[[nodiscard]] constexpr std::uint64_t hash3(std::int32_t x, std::int32_t y, std::int32_t z,
                                            std::uint32_t salt, std::uint64_t seed) noexcept
{
    const std::uint64_t h = hash2(x, z, salt, seed);
    return mix64(h ^ (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y)) * 0xD6E8FEB86659FD93ull));
}

/// Uniform in [0,1). Uses the top 24 bits because that is exactly the float
/// mantissa width, so the division is exact and the result is bit-reproducible
/// on any conforming compiler.
[[nodiscard]] constexpr float unitFloat(std::uint64_t h) noexcept
{
    return static_cast<float>(h >> 40) * (1.0f / 16777216.0f);
}

// Hash salts. Distinct values keep independent decisions from correlating.
constexpr std::uint32_t kSaltTreeCell   = 0x7134'0001u;
constexpr std::uint32_t kSaltTreeShape  = 0x9F27'0002u;
constexpr std::uint32_t kSaltLeafTrim   = 0x2C81'0003u;
constexpr std::uint32_t kSaltBedrock    = 0x51A3'0004u;
constexpr std::uint32_t kSaltSeaFloor   = 0x6B0D'0005u;

// ============================================================================
//  Height-field tuning
// ============================================================================

/// Continentalness -> base elevation, as a spline through hand-placed knots.
///
/// A spline rather than `seaLevel + noise * amplitude` because the interesting
/// structure of a coastline is the *rate* of change: a wide shallow shelf, then
/// a short steep shore, then a slowly rising interior. A single multiply gives a
/// uniform slope everywhere and reads as noise, not geography.
struct SplineKnot {
    float t;
    float height;
};

constexpr std::array<SplineKnot, 9> kContinentSpline{{
    {-1.00f, 38.0f},    // abyssal plain
    {-0.72f, 54.0f},    // deep ocean
    {-0.50f, 72.0f},    // continental shelf
    {-0.36f, 86.0f},    // shore break: steep on purpose, so the beach band that
    {-0.24f, 101.0f},   //   the biome selector derives from it stays a strip
    {-0.08f, 108.0f},   // coastal lowland
    {0.20f, 118.0f},    // interior lowland
    {0.55f, 136.0f},    // upland
    {1.00f, 164.0f},    // plateau
}};

/// Smoothstep interpolation between knots. Linear segments would leave a visible
/// crease along every knot once the mountain term multiplies the result.
[[nodiscard]] float evaluateSpline(float t) noexcept
{
    if (t <= kContinentSpline.front().t) {
        return kContinentSpline.front().height;
    }
    for (std::size_t i = 1; i < kContinentSpline.size(); ++i) {
        const SplineKnot& hi = kContinentSpline[i];
        if (t <= hi.t) {
            const SplineKnot& lo = kContinentSpline[i - 1];
            return lerp(lo.height, hi.height, smoothstep(lo.t, hi.t, t));
        }
    }
    return kContinentSpline.back().height;
}

/// Peak-to-trough range the ridge term can add on top of the spline, before the
/// erosion and land masks scale it down. Sized so that a maximal ridge on a
/// plateau lands just under the structure-clamped ceiling instead of flat-topping.
constexpr float kMountainMaxRise = 100.0f;

/// Ridge uplift, in voxels, at which a column reads as fully mountainous.
constexpr float kMountainFullRise = 34.0f;

/// Ridge values below this contribute no uplift at all.
///
/// Without a dead band the ridge field lifts *every* land column by its median,
/// which raises the whole world instead of building ranges: measured over a
/// 6 km square this is the difference between 18% of the world reading as
/// mountainous and 9%.
constexpr float kRidgeFloor = 0.38f;

/// Snow line at temperature 0, and how far a degree of climate noise moves it.
constexpr float kSnowLineBase           = 150.0f;
constexpr float kSnowLineTemperatureSlope = 36.0f;

/// Mountains below this much elevation under the snow line keep soil cover, so a
/// mountain range has a green skirt instead of being bare rock to its base.
constexpr float kMountainSoilBand = 28.0f;

constexpr float kFreezingTemperature = -0.38f;

// ============================================================================
//  Cave tuning
// ============================================================================

/// No carving at or below this height. Keeps the bedrock floor (y == 0 plus the
/// scattered layer above it) intact and stops tunnels from bottoming out into an
/// unreachable void.
constexpr std::int32_t kCaveFloorY = 5;

/// Minimum voxels of untouched ground between a cave and the surface. Without
/// this, a tunnel whose noise happens to peak near the terrain top removes the
/// surface voxel and leaves a hole in the sky (and, below sea level, an
/// un-flooded shaft under the ocean floor).
constexpr std::int32_t kCaveSurfaceMargin = 4;

/// Distance over which cave size ramps up once past the surface margin.
constexpr float kCaveSurfaceFade = 8.0f;
constexpr float kCaveFloorFade   = 6.0f;

/// Half-width of the |noise| < r band that forms a tunnel. Two independent
/// fields must both be inside their band, which is what turns two sheets into a
/// one-dimensional worm rather than a slab.
constexpr float kTunnelRadius = 0.085f;

/// Vertical scaling applied to the tunnel noise input. > 1 compresses the noise
/// vertically, i.e. makes tunnels wider than they are tall, which is both more
/// walkable and more cave-like.
constexpr float kCaveVerticalSquash = 1.7f;

/// Large open caverns only exist in the deep; above this they would break
/// through into terrain features far too often.
constexpr std::int32_t kCavernTopY      = 72;
constexpr float        kCavernThreshold = 0.58f;

// ============================================================================
//  Structure tuning
// ============================================================================

/// One tree candidate per cell of this size, jittered inside the cell. A grid
/// gives free minimum spacing (no two trunks inside one cell) and makes the
/// structure pass O(cells) rather than O(columns), while staying a pure hash of
/// the cell coordinate.
constexpr std::int32_t kTreeCellSize = 5;

/// Maximum surface height difference between a trunk and its four neighbours.
/// Rejects trees on cliff edges, which would otherwise stand on one corner.
constexpr std::int32_t kMaxTreeSlope = 2;

constexpr std::int32_t kBedrockScatterTopY = 3;

// Columns are sampled padded by kMaxStructureRadius so that trees rooted outside
// the chunk can still write into it, plus one extra ring so the slope test at a
// padded anchor always has its four neighbours available. Without that extra
// ring the slope test would be unavailable at the pad edge and two chunks could
// disagree about whether a border tree exists.
constexpr std::int32_t kSamplePad  = kMaxStructureRadius + 1;
constexpr std::int32_t kSampleSpan = kChunkSize + 2 * kSamplePad;

/// Column samples for a chunk plus its structure padding, in world coordinates.
struct SampleGrid {
    std::vector<ColumnSample> cells;
    std::int32_t              originX = 0;
    std::int32_t              originZ = 0;

    [[nodiscard]] const ColumnSample& at(std::int32_t worldX, std::int32_t worldZ) const noexcept
    {
        const std::int32_t dx = worldX - originX + kSamplePad;
        const std::int32_t dz = worldZ - originZ + kSamplePad;
        VOXL_ASSERT(dx >= 0 && dx < kSampleSpan && dz >= 0 && dz < kSampleSpan,
                    "column sample outside the padded grid");
        return cells[static_cast<std::size_t>(dz) * static_cast<std::size_t>(kSampleSpan) +
                     static_cast<std::size_t>(dx)];
    }
};

// ----------------------------------------------------------- LOD sampling --

/// Cells of padding a level-L sample grid needs on each side so that the
/// vertical bound below covers every tree that can reach into the chunk. Zero
/// when the level places no trees, which is the whole point: the coarse levels
/// that dominate the chunk count sample nothing outside their own footprint.
[[nodiscard]] constexpr std::int32_t lodPadCells(LodLevel level, bool withTrees) noexcept
{
    if (!withTrees) {
        return 0;
    }
    const std::int32_t cell  = lodCellSize(level);
    const std::int32_t reach = kMaxStructureRadius + cell - 1;
    return (reach + cell - 1) / cell;
}

/// Worst-case padded span, over every level that can place trees, so the coarse
/// column grid can live on the stack. Level 1 is the widest: 16 cells plus two
/// rings of padding.
constexpr std::int32_t kLodMaxSpan =
    lodGridSize(kLodTreeMaxLevel) + 2 * lodPadCells(kLodTreeMaxLevel, true);
constexpr std::size_t kMaxLodColumns =
    static_cast<std::size_t>(kLodMaxSpan) * static_cast<std::size_t>(kLodMaxSpan);

/// Column samples on the level-L cell lattice for a chunk plus `pad` rings.
///
/// Lookups are by world coordinate and are quantised to the lattice, which is
/// global (chunk origins are multiples of 32, hence of every cell size), so two
/// chunks asking about the same world column at the same level land on the same
/// cell and therefore the same sample.
struct LodSampleGrid {
    std::array<ColumnSample, kMaxLodColumns> cells{};
    std::int32_t shift       = 0;  ///< log2 of the cell size
    std::int32_t cellOriginX = 0;  ///< chunk origin, in cells
    std::int32_t cellOriginZ = 0;
    std::int32_t pad         = 0;
    std::int32_t span        = 0;

    [[nodiscard]] const ColumnSample& atCell(std::int32_t cx, std::int32_t cz) const noexcept
    {
        const std::int32_t ix = cx + pad;
        const std::int32_t iz = cz + pad;
        VOXL_ASSERT(ix >= 0 && ix < span && iz >= 0 && iz < span, "lod cell outside the grid");
        return cells[static_cast<std::size_t>(iz) * static_cast<std::size_t>(span) +
                     static_cast<std::size_t>(ix)];
    }
};

/// Cells in one chunk at the finest coarse level - the worst case the per-cell
/// scratch buffer has to hold. Level 1 is 16^3; every coarser level is smaller.
constexpr std::size_t kMaxLodCells = lodCellCount(static_cast<LodLevel>(kLodFull + 1));

[[nodiscard]] constexpr std::size_t lodCellIndex(std::int32_t cx, std::int32_t cy, std::int32_t cz,
                                                 std::int32_t gridSize) noexcept
{
    return static_cast<std::size_t>((cy * gridSize + cz) * gridSize + cx);
}

/// Which material covers most of a coarse chunk.
///
/// A linear table rather than a map: a chunk holds a handful of distinct ids, so
/// the scan is shorter than a hash would be to set up. Overflowing the table
/// only costs a worse guess - the result is a fill hint, never a correctness
/// input - so it degrades instead of failing.
struct LodMaterialTally {
    static constexpr std::size_t     kCapacity = 12;
    std::array<BlockId, kCapacity>      ids{};
    std::array<std::int32_t, kCapacity> counts{};
    std::size_t                         size = 0;

    void add(BlockId id) noexcept
    {
        for (std::size_t i = 0; i < size; ++i) {
            if (ids[i] == id) {
                ++counts[i];
                return;
            }
        }
        if (size < kCapacity) {
            ids[size]    = id;
            counts[size] = 1;
            ++size;
        }
    }

    [[nodiscard]] BlockId dominant() const noexcept
    {
        BlockId      best      = blocks::Air;
        std::int32_t bestCount = 0;
        for (std::size_t i = 0; i < size; ++i) {
            if (counts[i] > bestCount) {
                bestCount = counts[i];
                best      = ids[i];
            }
        }
        return best;
    }
};

// ============================================================================
//  Biome table
// ============================================================================

constexpr std::array<BiomeDescription, kBiomeCount> kBiomes{{
    // name        surface            subsurface      subDepth  filler                fillDepth  trees  bias  detail
    {"ocean",     blocks::Sand,      blocks::Sand,     3,       blocks::Stone,        0,         0.00f, 0.0f, 2.5f},
    {"beach",     blocks::Sand,      blocks::Sand,     3,       blocks::Sandstone,    6,         0.00f, 0.0f, 1.2f},
    {"plains",    blocks::Grass,     blocks::Dirt,     4,       blocks::Stone,        0,         0.07f, -1.0f, 2.0f},
    {"forest",    blocks::Grass,     blocks::Dirt,     4,       blocks::Stone,        0,         0.82f, 1.0f, 3.5f},
    {"desert",    blocks::Sand,      blocks::Sand,     4,       blocks::Sandstone,    9,         0.00f, 2.0f, 5.0f},
    {"mountains", blocks::Stone,     blocks::Stone,    3,       blocks::Stone,        0,         0.14f, 4.0f, 8.0f},
    {"snowy",     blocks::Snow,      blocks::Dirt,     4,       blocks::Stone,        0,         0.40f, 1.0f, 3.0f},
}};

// ============================================================================
//  Column material resolution
// ============================================================================

struct SurfaceMaterials {
    BlockId      surface;
    BlockId      subsurface;
    std::int32_t subsurfaceDepth;
    BlockId      filler;
    std::int32_t fillerDepth;
};

/// Pure function of the column sample plus the seed, so every chunk that touches
/// this column derives the same materials.
[[nodiscard]] SurfaceMaterials resolveMaterials(const ColumnSample& sample, std::int32_t worldX,
                                                std::int32_t worldZ, std::int32_t seaLevel,
                                                std::uint64_t seed) noexcept
{
    const BiomeDescription& desc = biomeDescription(sample.biome);
    SurfaceMaterials        out{desc.surface, desc.subsurface, desc.subsurfaceDepth, desc.filler,
                         desc.fillerDepth};

    const float surfaceF = static_cast<float>(sample.surfaceY);
    const float snowLine = kSnowLineBase - sample.temperature * kSnowLineTemperatureSlope;

    if (sample.biome == BiomeId::Mountains && surfaceF < snowLine - kMountainSoilBand) {
        out.surface    = blocks::Grass;
        out.subsurface = blocks::Dirt;
    }

    const bool capsWithSnow = sample.biome != BiomeId::Desert && sample.biome != BiomeId::Ocean &&
                              sample.biome != BiomeId::Beach;
    if (capsWithSnow && surfaceF >= snowLine) {
        out.surface = blocks::Snow;
    }

    // A submerged column must never be capped with grass or snow, whatever the
    // climate says: the biome boundary is derived from the pre-detail height and
    // can disagree with the final surface by a couple of voxels near the shore.
    if (sample.surfaceY < seaLevel) {
        const float roll = unitFloat(hash2(worldX, worldZ, kSaltSeaFloor, seed));
        out.surface      = roll < 0.55f ? blocks::Sand
                           : (roll < 0.85f ? blocks::Gravel : blocks::Clay);
        if (out.subsurface == blocks::Dirt || out.subsurface == blocks::Snow) {
            out.subsurface = blocks::Sand;
        }
    }

    return out;
}

/// Caves may only replace ground. Water is excluded so a tunnel cannot drain a
/// lake, and bedrock is excluded so the world floor is unbreakable even where
/// the scattered bedrock layer reaches up.
[[nodiscard]] constexpr bool isCarveable(BlockId id) noexcept
{
    switch (id) {
        case blocks::Stone:
        case blocks::Dirt:
        case blocks::Grass:
        case blocks::Sand:
        case blocks::Sandstone:
        case blocks::Gravel:
        case blocks::Clay:
        case blocks::Snow:
            return true;
        default:
            return false;
    }
}

// ============================================================================
//  Trees
// ============================================================================

struct TreeShape {
    std::int32_t trunkHeight = 6;  ///< Log voxels, starting at surfaceY + 1.
    bool         conifer     = false;
};

/// Stepped rather than smooth silhouette: alternating ring radii read far better
/// on cube voxels than a mathematically clean cone, which turns into a staircase.
[[nodiscard]] constexpr std::int32_t coniferRadius(std::int32_t fromTop,
                                                   std::int32_t trunkHeight) noexcept
{
    if (fromTop <= 0) {
        return 0;
    }
    if (fromTop == 1) {
        return 1;
    }
    if ((fromTop % 2) != 0) {
        return 1;
    }
    return (trunkHeight >= 9 && fromTop >= 6) ? 3 : 2;
}

[[nodiscard]] TreeShape treeShapeFor(BiomeId biome, std::uint64_t cellHash) noexcept
{
    TreeShape shape;
    shape.conifer   = biome == BiomeId::Snowy || biome == BiomeId::Mountains;
    const float roll = unitFloat(mix64(cellHash ^ kSaltTreeShape));
    if (shape.conifer) {
        shape.trunkHeight = 7 + static_cast<std::int32_t>(roll * 5.0f);  // 7..11
    } else {
        shape.trunkHeight = 5 + static_cast<std::int32_t>(roll * 3.0f);  // 5..7
    }
    return shape;
}

/// Writes one structure voxel if it lands inside `chunk`, returning whether it
/// did. Positions outside the chunk are silently dropped: the neighbouring chunk
/// enumerates the very same tree and writes them itself, which is what makes a
/// border tree whole instead of clipped.
///
/// `overwriteLeaves` distinguishes trunks (which may replace their own canopy)
/// from leaves (which only fill air). Both refuse to overwrite terrain, so a
/// tree never punches a hole through a hillside.
bool placeOneStructureVoxel(Chunk& chunk, std::int32_t worldX, std::int32_t worldY,
                            std::int32_t worldZ, BlockId id, bool overwriteLeaves)
{
    if (worldY < kWorldMinY || worldY > kWorldMaxY) {
        return false;
    }
    const ChunkPos& pos = chunk.position();
    if (blockToChunkAxis(worldX) != pos.x || blockToChunkAxis(worldY) != pos.y ||
        blockToChunkAxis(worldZ) != pos.z) {
        return false;
    }

    const std::size_t index = localIndex(blockToLocalAxis(worldX), blockToLocalAxis(worldY),
                                         blockToLocalAxis(worldZ));
    ChunkStorage&     storage  = chunk.storage();
    const BlockId     existing = storage.get(index);
    if (existing != blocks::Air && !(overwriteLeaves && existing == blocks::Leaves)) {
        return false;
    }
    storage.set(index, id);
    return true;
}

/// Level-aware structure write: fills the whole level-L cell that contains the
/// voxel, one block at a time so the "never overwrite terrain" rule still holds
/// per block. At kLodFull the cell is one block and this is the identity.
///
/// WHY QUANTISE AT ALL: a coarse chunk is consumed one sample per cell, so a
/// lone trunk voxel sitting in a 2-block cell is a coin flip between "there is a
/// tree here" and "there is not". Inflating it to the cell is the same
/// bias-toward-solid trade Lod.hpp makes for terrain - a slightly chunky tree is
/// invisible at these distances, a tree that half exists is not.
bool placeStructureVoxel(Chunk& chunk, std::int32_t worldX, std::int32_t worldY, std::int32_t worldZ,
                         BlockId id, bool overwriteLeaves, LodLevel level)
{
    if (level == kLodFull) {
        return placeOneStructureVoxel(chunk, worldX, worldY, worldZ, id, overwriteLeaves);
    }

    const std::int32_t shift = static_cast<std::int32_t>(level);
    const std::int32_t cell  = lodCellSize(level);
    const std::int32_t x0    = (worldX >> shift) << shift;
    const std::int32_t y0    = (worldY >> shift) << shift;
    const std::int32_t z0    = (worldZ >> shift) << shift;

    bool placed = false;
    for (std::int32_t dy = 0; dy < cell; ++dy) {
        for (std::int32_t dz = 0; dz < cell; ++dz) {
            for (std::int32_t dx = 0; dx < cell; ++dx) {
                placed |= placeOneStructureVoxel(chunk, x0 + dx, y0 + dy, z0 + dz, id,
                                                 overwriteLeaves);
            }
        }
    }
    return placed;
}

void emitTree(Chunk& chunk, std::int32_t anchorX, std::int32_t anchorZ, std::int32_t surfaceY,
              const TreeShape& shape, std::uint64_t seed, LodLevel level)
{
    const std::int32_t baseY = surfaceY + 1;
    const std::int32_t h     = shape.trunkHeight;

    // Trunk first so the canopy's air test naturally skips the trunk column.
    for (std::int32_t dy = 0; dy < h; ++dy) {
        placeStructureVoxel(chunk, anchorX, baseY + dy, anchorZ, blocks::Wood, true, level);
    }

    // Layer descriptor: vertical offset from baseY, and the ring radius.
    struct Layer {
        std::int32_t dy;
        std::int32_t radius;
        bool         plus;  ///< true == 4-neighbour cross only (used for the cap)
    };
    std::array<Layer, 12> layers{};
    std::size_t           layerCount = 0;

    if (shape.conifer) {
        for (std::int32_t dy = 3; dy <= h; ++dy) {
            const std::int32_t fromTop = h - dy;
            const std::int32_t radius  = coniferRadius(fromTop, h);
            VOXL_ASSERT(radius <= kMaxStructureRadius, "conifer canopy exceeds the structure pad");
            layers[layerCount++] = Layer{dy, radius, fromTop == 0};
            if (layerCount == layers.size()) {
                break;
            }
        }
    } else {
        layers[layerCount++] = Layer{h - 3, 2, false};
        layers[layerCount++] = Layer{h - 2, 2, false};
        layers[layerCount++] = Layer{h - 1, 1, false};
        layers[layerCount++] = Layer{h, 1, true};
    }

    for (std::size_t i = 0; i < layerCount; ++i) {
        const Layer&       layer = layers[i];
        const std::int32_t y     = baseY + layer.dy;
        const std::int32_t r     = layer.radius;
        for (std::int32_t dz = -r; dz <= r; ++dz) {
            for (std::int32_t dx = -r; dx <= r; ++dx) {
                const std::int32_t d2 = dx * dx + dz * dz;
                if (layer.plus) {
                    if (d2 > 1) {
                        continue;
                    }
                } else {
                    if (d2 > r * r + r) {
                        continue;  // rounds the square ring off at the corners
                    }
                    // Ragged outer edge. Hashed on the world position, so the
                    // same voxel is trimmed identically from every chunk that
                    // enumerates this tree.
                    if (d2 >= r * r && r > 0 &&
                        (hash3(anchorX + dx, y, anchorZ + dz, kSaltLeafTrim, seed) & 1u) != 0u) {
                        continue;
                    }
                }
                placeStructureVoxel(chunk, anchorX + dx, y, anchorZ + dz, blocks::Leaves, false,
                                    level);
            }
        }
    }
}

/// Structure pass.
///
/// APPROACH: padded enumeration with world-space clipping. The chunk enumerates
/// every tree-grid cell that overlaps its own footprint expanded by
/// kMaxStructureRadius, builds each of those trees *in full world coordinates*,
/// and writes only the voxels that land inside itself. A tree straddling a chunk
/// border is therefore emitted twice - once by each chunk - and each chunk keeps
/// its own half. No deferred queue and no inter-chunk communication is needed,
/// which matters because the neighbour may not be resident (or may already have
/// been meshed) when this chunk generates.
///
/// The two properties that make this correct:
///   1. A tree's existence, position, species and every trimmed leaf are pure
///      hashes of world coordinates and the seed, so both chunks build the
///      identical block set.
///   2. Cells are visited in ascending (cellZ, cellX) order, which is a *global*
///      total order restricted to a subset. Two overlapping trees therefore
///      resolve in the same order from either side of a border, so the winner at
///      a contested voxel is a pure function of that voxel.
///
/// LOD: `columnAt` is how the pass reads the height field, and at coarse levels
/// it samples the exact column rather than a cell centre. That is deliberate and
/// cheap - tree candidates are sparse (one per 5x5 cell), so a few dozen exact
/// samples buy EXACTLY the same set of trees, in the same places, as level 0.
/// Only the base height is snapped, to the coarse ground the tree stands on, and
/// only the voxel writes are cell-quantised. Deciding tree existence from a cell
/// centre instead would make trees blink in and out as a chunk changed level.
template <typename ColumnAt>
void placeTrees(Chunk& chunk, std::int32_t originX, std::int32_t originZ,
                const TerrainSettings& settings, LodLevel level, ColumnAt&& columnAt)
{
    // Widened by one cell short of the full cell: a quantised structure voxel
    // fills its whole cell, so an anchor kMaxStructureRadius + cell - 1 blocks
    // outside the chunk can still reach in. At kLodFull the widening is zero and
    // this is the original bound.
    const std::int32_t reach   = kMaxStructureRadius + lodCellSize(level) - 1;
    const std::int32_t padMinX = originX - reach;
    const std::int32_t padMaxX = originX + kChunkSize - 1 + reach;
    const std::int32_t padMinZ = originZ - reach;
    const std::int32_t padMaxZ = originZ + kChunkSize - 1 + reach;

    const std::int32_t cellMinX = floorDiv(padMinX, kTreeCellSize);
    const std::int32_t cellMaxX = floorDiv(padMaxX, kTreeCellSize);
    const std::int32_t cellMinZ = floorDiv(padMinZ, kTreeCellSize);
    const std::int32_t cellMaxZ = floorDiv(padMaxZ, kTreeCellSize);

    for (std::int32_t cellZ = cellMinZ; cellZ <= cellMaxZ; ++cellZ) {
        for (std::int32_t cellX = cellMinX; cellX <= cellMaxX; ++cellX) {
            const std::uint64_t cellHash = hash2(cellX, cellZ, kSaltTreeCell, settings.seed);

            const std::int32_t anchorX =
                cellX * kTreeCellSize + static_cast<std::int32_t>((cellHash >> 3) % kTreeCellSize);
            const std::int32_t anchorZ =
                cellZ * kTreeCellSize + static_cast<std::int32_t>((cellHash >> 19) % kTreeCellSize);

            // A cell can jitter its anchor outside the padded footprint. Such a
            // tree is more than kMaxStructureRadius away from the chunk, so it
            // provably cannot reach it - and skipping it here is exactly what
            // keeps the sample grid large enough.
            if (anchorX < padMinX || anchorX > padMaxX || anchorZ < padMinZ || anchorZ > padMaxZ) {
                continue;
            }

            const ColumnSample site    = columnAt(anchorX, anchorZ);
            const float        density = biomeDescription(site.biome).treeDensity;
            if (density <= 0.0f) {
                continue;
            }
            if (unitFloat(mix64(cellHash)) >= density) {
                continue;
            }
            if (site.surfaceY <= settings.seaLevel) {
                continue;  // nothing grows with its roots underwater
            }

            const SurfaceMaterials materials =
                resolveMaterials(site, anchorX, anchorZ, settings.seaLevel, settings.seed);
            if (materials.surface != blocks::Grass && materials.surface != blocks::Snow) {
                continue;  // bare rock, sand and gravel stay bare
            }

            const std::int32_t slope =
                std::max({std::abs(columnAt(anchorX + 1, anchorZ).surfaceY - site.surfaceY),
                          std::abs(columnAt(anchorX - 1, anchorZ).surfaceY - site.surfaceY),
                          std::abs(columnAt(anchorX, anchorZ + 1).surfaceY - site.surfaceY),
                          std::abs(columnAt(anchorX, anchorZ - 1).surfaceY - site.surfaceY)});
            if (slope > kMaxTreeSlope) {
                continue;
            }

            // Stand the trunk on the ground this level actually built, not on the
            // exact height field, or a coarse tree floats or sinks by up to half
            // a cell. Identity at kLodFull.
            const std::int32_t groundY = lodTerrainTop(
                columnAt(lodSampleCoord(anchorX, level), lodSampleCoord(anchorZ, level)).surfaceY,
                level);

            const TreeShape shape = treeShapeFor(site.biome, cellHash);
            emitTree(chunk, anchorX, anchorZ, groundY, shape, settings.seed, level);
        }
    }
}

}  // namespace

// ============================================================================
//  Biome descriptions
// ============================================================================

const BiomeDescription& biomeDescription(BiomeId biome) noexcept
{
    const std::size_t index = static_cast<std::size_t>(biome);
    return index < kBiomes.size() ? kBiomes[index] : kBiomes[static_cast<std::size_t>(BiomeId::Plains)];
}

// ============================================================================
//  Noise fields
// ============================================================================

/// One FastNoiseLite instance per field.
///
/// THREAD SAFETY: FastNoiseLite stores only its configuration and has no mutable
/// members; `GetNoise` is const and reads nothing else. A single `const` instance
/// is therefore safe to sample from any number of threads at once, which is why
/// the generator keeps one shared set rather than thread_local copies. Nothing
/// here may be reconfigured after construction - that is what the `const` on
/// TerrainGenerator::m_noise enforces.
struct TerrainGenerator::NoiseFields {
    FastNoiseLite continent;
    FastNoiseLite erosion;
    FastNoiseLite ridges;
    FastNoiseLite detail;
    FastNoiseLite temperature;
    FastNoiseLite humidity;
    FastNoiseLite tunnelA;
    FastNoiseLite tunnelB;
    FastNoiseLite cavern;

    explicit NoiseFields(std::uint64_t seed)
    {
        // Each field gets an independently hashed seed rather than seed + k, so
        // that two worlds whose seeds differ by one share no structure at all.
        auto derive = [seed](std::uint32_t salt) {
            return static_cast<int>(
                static_cast<std::uint32_t>(mix64(seed ^ (static_cast<std::uint64_t>(salt) * 0x9E3779B97F4A7C15ull))));
        };

        auto configureFbm = [](FastNoiseLite& noise, int fieldSeed, float frequency, int octaves,
                               float gain) {
            noise.SetSeed(fieldSeed);
            noise.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
            noise.SetFractalType(FastNoiseLite::FractalType_FBm);
            noise.SetFrequency(frequency);
            noise.SetFractalOctaves(octaves);
            noise.SetFractalLacunarity(2.0f);
            noise.SetFractalGain(gain);
        };

        // Continentalness is the slowest field in the world: a ~1.6 km feature
        // size, so a continent takes minutes to cross on foot.
        configureFbm(continent, derive(0x01u), 0.00062f, 5, 0.42f);
        configureFbm(erosion, derive(0x02u), 0.0024f, 3, 0.45f);
        configureFbm(detail, derive(0x04u), 0.019f, 3, 0.48f);
        configureFbm(temperature, derive(0x05u), 0.00085f, 2, 0.40f);
        configureFbm(humidity, derive(0x06u), 0.00110f, 2, 0.40f);

        // Ridged fractal: the absolute-value folding is what produces sharp
        // crests instead of the rounded blobs an FBm sum gives.
        ridges.SetSeed(derive(0x03u));
        ridges.SetNoiseType(FastNoiseLite::NoiseType_OpenSimplex2);
        ridges.SetFractalType(FastNoiseLite::FractalType_Ridged);
        ridges.SetFrequency(0.0043f);
        ridges.SetFractalOctaves(4);
        ridges.SetFractalLacunarity(2.05f);
        ridges.SetFractalGain(0.50f);

        // Tunnels: two independent low-octave fields. A voxel is inside a tunnel
        // only where BOTH are near zero, i.e. on the intersection of two
        // iso-surfaces, which is a curve - a worm - rather than a sheet.
        configureFbm(tunnelA, derive(0x07u), 0.0125f, 2, 0.50f);
        configureFbm(tunnelB, derive(0x08u), 0.0125f, 2, 0.50f);
        configureFbm(cavern, derive(0x09u), 0.0205f, 3, 0.50f);
    }
};

// ============================================================================
//  TerrainGenerator
// ============================================================================

TerrainGenerator::TerrainGenerator(const TerrainSettings& settings)
    : m_settings(settings), m_noise(std::make_unique<const NoiseFields>(settings.seed))
{
    VOXL_CHECK(m_settings.seaLevel > kWorldMinY && m_settings.seaLevel < kWorldMaxY,
               "sea level {} outside the world", m_settings.seaLevel);
}

TerrainGenerator::~TerrainGenerator() = default;

ColumnSample TerrainGenerator::sampleColumn(std::int32_t worldX, std::int32_t worldZ) const noexcept
{
    const float fx = static_cast<float>(worldX);
    const float fz = static_cast<float>(worldZ);

    ColumnSample sample;
    sample.continentalness = m_noise->continent.GetNoise(fx, fz);
    sample.erosion         = m_noise->erosion.GetNoise(fx, fz);
    sample.temperature     = m_noise->temperature.GetNoise(fx, fz);
    sample.humidity        = m_noise->humidity.GetNoise(fx, fz);

    const float ridgeRaw = m_noise->ridges.GetNoise(fx, fz);
    const float detail   = m_noise->detail.GetNoise(fx, fz);

    // Erosion is a *modifier*, not a height: high erosion flattens whatever the
    // other fields propose. This is what separates a plain from a mountain range
    // that happen to share the same continentalness.
    const float relief = 1.0f - 0.82f * smoothstep(-0.5f, 0.5f, sample.erosion);

    const float ridge01 = saturate(ridgeRaw * 0.5f + 0.5f);
    // Dead band, then squared: only the upper tail of the ridge field becomes a
    // peak, and it grows superlinearly so ranges have flanks rather than a
    // uniform swell.
    const float ridgeCrest = saturate((ridge01 - kRidgeFloor) / (1.0f - kRidgeFloor));
    const float peak       = ridgeCrest * ridgeCrest;
    // No mountains rise out of the sea floor: the mask reaches zero well before
    // the coastline so the continental shelf stays a shelf.
    const float landMask = saturate((sample.continentalness + 0.30f) * 2.6f);
    const float peakRise = peak * kMountainMaxRise * relief * landMask;

    sample.mountainFactor = saturate(peakRise / kMountainFullRise);

    const float baseHeight = evaluateSpline(sample.continentalness) + peakRise;
    const float seaF       = static_cast<float>(m_settings.seaLevel);
    const float aboveSea   = baseHeight - seaF;

    // ---- blended biome contributions -----------------------------------
    //
    // Biome membership is a set of smooth functions of the climate fields and the
    // pre-detail height, and by construction the weights sum to exactly 1. The
    // per-biome height bias and detail amplitude are then a weighted average, so
    // crossing from plains into desert changes the terrain's character over tens
    // of voxels instead of dropping a two-voxel step along the border. The biome
    // *materials* are still chosen crisply below - a visible material boundary is
    // fine, a cliff is not.
    const float ocean = 1.0f - smoothstep(-6.0f, -2.0f, aboveSea);
    const float shore = smoothstep(-6.0f, -2.0f, aboveSea);
    const float land  = smoothstep(1.0f, 5.0f, aboveSea);
    const float beach = shore - land;  // shore >= land everywhere by construction

    const float hot       = smoothstep(0.05f, 0.55f, sample.temperature);
    const float cold      = smoothstep(-0.05f, -0.55f, sample.temperature);
    const float dry       = smoothstep(0.10f, -0.35f, sample.humidity);
    const float wet       = smoothstep(-0.05f, 0.35f, sample.humidity);
    const float temperate = saturate(1.0f - hot - cold);

    const float inland = land * (1.0f - sample.mountainFactor);

    std::array<float, kBiomeCount> weights{};
    weights[static_cast<std::size_t>(BiomeId::Ocean)]     = ocean;
    weights[static_cast<std::size_t>(BiomeId::Beach)]     = beach;
    weights[static_cast<std::size_t>(BiomeId::Mountains)] = land * sample.mountainFactor;
    weights[static_cast<std::size_t>(BiomeId::Desert)]    = inland * hot * dry;
    weights[static_cast<std::size_t>(BiomeId::Snowy)]     = inland * cold;
    weights[static_cast<std::size_t>(BiomeId::Forest)]    = inland * temperate * wet;
    weights[static_cast<std::size_t>(BiomeId::Plains)] =
        std::max(0.0f, inland - weights[static_cast<std::size_t>(BiomeId::Desert)] -
                           weights[static_cast<std::size_t>(BiomeId::Snowy)] -
                           weights[static_cast<std::size_t>(BiomeId::Forest)]);

    float blendedBias   = 0.0f;
    float blendedDetail = 0.0f;
    for (std::size_t i = 0; i < kBiomeCount; ++i) {
        const BiomeDescription& desc = kBiomes[i];
        blendedBias += weights[i] * desc.heightBias;
        blendedDetail += weights[i] * desc.detailAmplitude;
    }

    float height = baseHeight + blendedBias + detail * blendedDetail * relief;

    // Pull the shoreline band toward sea level so a coast reads as a beach rather
    // than a wall of sand. Weighted by the (continuous) beach membership, so this
    // flattening fades in and out instead of forming a rim.
    height = lerp(height, seaF + 1.0f, 0.45f * beach);

    // Clamped away from the ceiling by the tallest structure so that a tree on a
    // peak is never truncated by the top of the world.
    const std::int32_t maxSurface = kWorldMaxY - kMaxStructureHeight - 2;
    sample.surfaceY = std::clamp(static_cast<std::int32_t>(std::floor(height)), kWorldMinY + 1, maxSurface);
    sample.structureTopY = std::min(sample.surfaceY + kMaxStructureHeight, kWorldMaxY);

    // ---- crisp biome choice --------------------------------------------
    std::size_t best = static_cast<std::size_t>(BiomeId::Plains);
    for (std::size_t i = 0; i < kBiomeCount; ++i) {
        if (weights[i] > weights[best]) {  // strict: ties resolve to the lower id
            best = i;
        }
    }
    sample.biome = static_cast<BiomeId>(best);

    return sample;
}

std::int32_t TerrainGenerator::surfaceHeight(std::int32_t worldX, std::int32_t worldZ) const noexcept
{
    return sampleColumn(worldX, worldZ).surfaceY;
}

BiomeId TerrainGenerator::biomeAt(std::int32_t worldX, std::int32_t worldZ) const noexcept
{
    return sampleColumn(worldX, worldZ).biome;
}

bool TerrainGenerator::carvesCave(std::int32_t worldX, std::int32_t worldY, std::int32_t worldZ,
                                  std::int32_t surfaceY) const noexcept
{
    return carvesCaveSpan(worldX, worldY, worldZ, worldY, worldY, surfaceY);
}

bool TerrainGenerator::carvesCaveSpan(std::int32_t worldX, std::int32_t noiseY, std::int32_t worldZ,
                                      std::int32_t shallowestY, std::int32_t deepestY,
                                      std::int32_t surfaceY) const noexcept
{
    if (deepestY <= kCaveFloorY) {
        return false;  // bedrock floor is never carved
    }
    const std::int32_t depth = surfaceY - shallowestY;
    if (depth < kCaveSurfaceMargin) {
        return false;  // never break through into the sky
    }

    // Two independent fades: caves thin out as they approach the surface and as
    // they approach the world floor. Both reach exactly zero, so there is no
    // discontinuity at the hard rejections above. Both are evaluated at the
    // worst block of the span, so a coarse cell can only ever carve LESS than
    // the blocks it covers would individually - never more.
    const float fade = saturate(static_cast<float>(depth - kCaveSurfaceMargin) / kCaveSurfaceFade) *
                       saturate(static_cast<float>(deepestY - kCaveFloorY) / kCaveFloorFade);
    if (fade <= 0.0f) {
        return false;
    }

    const float fx = static_cast<float>(worldX);
    const float fz = static_cast<float>(worldZ);
    const float fy = static_cast<float>(noiseY) * kCaveVerticalSquash;

    // Field A is evaluated first and rejects the overwhelming majority of voxels,
    // so field B costs almost nothing in aggregate. This ordering is the reason
    // 3D cave noise is affordable at all in the per-voxel loop.
    const float radius = kTunnelRadius * fade;
    if (std::fabs(m_noise->tunnelA.GetNoise(fx, fy, fz)) < radius &&
        std::fabs(m_noise->tunnelB.GetNoise(fx, fy, fz)) < radius) {
        return true;
    }

    if (shallowestY < kCavernTopY) {
        const float cavern = m_noise->cavern.GetNoise(fx, static_cast<float>(noiseY) * 0.85f, fz);
        if (cavern > kCavernThreshold + (1.0f - fade) * 0.55f) {
            return true;
        }
    }
    return false;
}

void TerrainGenerator::generate(Chunk& chunk) const
{
    const ChunkPos& pos = chunk.position();
    VOXL_CHECK(pos.y >= 0 && pos.y < kWorldSectionCount, "chunk section y={} outside the world", pos.y);

    const BlockPos     origin      = pos.originBlock();
    const std::int32_t sectionMinY = origin.y;

    ChunkStorage& storage = chunk.storage();
    // Reset first so generate() is idempotent: the determinism tests regenerate
    // into the same chunk, and the streaming path may retry a cancelled job.
    storage.fill(blocks::Air);
    storage.fillLight(0, 0);

    SampleGrid grid;
    grid.originX = origin.x;
    grid.originZ = origin.z;
    grid.cells.resize(static_cast<std::size_t>(kSampleSpan) * static_cast<std::size_t>(kSampleSpan));

    std::int32_t highestStructureTop = kWorldMinY;
    for (std::int32_t dz = 0; dz < kSampleSpan; ++dz) {
        for (std::int32_t dx = 0; dx < kSampleSpan; ++dx) {
            ColumnSample& cell = grid.cells[static_cast<std::size_t>(dz) *
                                                static_cast<std::size_t>(kSampleSpan) +
                                            static_cast<std::size_t>(dx)];
            cell = sampleColumn(origin.x + dx - kSamplePad, origin.z + dz - kSamplePad);
            highestStructureTop = std::max(highestStructureTop, cell.structureTopY);
        }
    }

    // Sky sections are the majority of a loaded world. The bound uses the padded
    // grid because a tree rooted in a neighbouring chunk can reach in here.
    if (sectionMinY > std::max(highestStructureTop, m_settings.seaLevel)) {
        if (m_settings.seedSunlight) {
            storage.fillLight(ChunkStorage::kMaxLightLevel, 0);
        }
        chunk.markDirty();
        return;
    }

    const std::int32_t seaLevel = m_settings.seaLevel;

    for (std::int32_t localZ = 0; localZ < kChunkSize; ++localZ) {
        for (std::int32_t localX = 0; localX < kChunkSize; ++localX) {
            const std::int32_t  worldX = origin.x + localX;
            const std::int32_t  worldZ = origin.z + localZ;
            const ColumnSample& sample = grid.at(worldX, worldZ);
            const SurfaceMaterials materials =
                resolveMaterials(sample, worldX, worldZ, seaLevel, m_settings.seed);

            const bool freezes = sample.temperature < kFreezingTemperature;

            for (std::int32_t localY = 0; localY < kChunkSize; ++localY) {
                const std::int32_t worldY = sectionMinY + localY;
                BlockId            id     = blocks::Air;

                if (worldY == kWorldMinY) {
                    id = blocks::Bedrock;
                } else if (worldY <= sample.surfaceY) {
                    const std::int32_t depth = sample.surfaceY - worldY;
                    if (depth == 0) {
                        id = materials.surface;
                    } else if (depth <= materials.subsurfaceDepth) {
                        id = materials.subsurface;
                    } else if (depth <= materials.subsurfaceDepth + materials.fillerDepth) {
                        id = materials.filler;
                    } else {
                        id = blocks::Stone;
                    }

                    // Rough bedrock shelf above the solid floor. Hashed per voxel,
                    // and checked before carving so caves cannot eat it.
                    if (worldY <= kBedrockScatterTopY &&
                        unitFloat(hash3(worldX, worldY, worldZ, kSaltBedrock, m_settings.seed)) <
                            static_cast<float>(kBedrockScatterTopY + 1 - worldY) * 0.25f) {
                        id = blocks::Bedrock;
                    } else if (m_settings.generateCaves && isCarveable(id) &&
                               carvesCave(worldX, worldY, worldZ, sample.surfaceY)) {
                        id = blocks::Air;
                    }
                } else if (worldY <= seaLevel) {
                    id = (freezes && worldY == seaLevel) ? blocks::Ice : blocks::Water;
                }

                const std::size_t index = localIndex(localX, localY, localZ);
                if (id != blocks::Air) {
                    storage.set(index, id);
                }

                if (m_settings.seedSunlight) {
                    // Coarse seed only; the lighting pass recomputes this. Water
                    // damps sky light so a lake bed is not lit like open ground.
                    std::uint8_t sunlight = 0;
                    if (worldY > sample.surfaceY) {
                        sunlight = worldY > seaLevel
                                       ? ChunkStorage::kMaxLightLevel
                                       : static_cast<std::uint8_t>(std::max(
                                             0, static_cast<int>(ChunkStorage::kMaxLightLevel) -
                                                    2 * (seaLevel - worldY + 1)));
                    }
                    if (sunlight != 0) {
                        storage.setLight(index, ChunkStorage::packLight(sunlight, 0));
                    }
                }
            }
        }
    }

    if (m_settings.generateTrees) {
        placeTrees(chunk, grid.originX, grid.originZ, m_settings, kLodFull,
                   [&grid](std::int32_t x, std::int32_t z) -> const ColumnSample& {
                       return grid.at(x, z);
                   });
    }

    // One O(volume) sweep reclaims the palette slots that carving and structure
    // overwrites left unreferenced, and collapses uniform sections back to 8 bytes.
    storage.optimise();
    chunk.markDirty();
}

// ============================================================================
//  Level-of-detail generation
// ============================================================================

void TerrainGenerator::generate(Chunk& chunk, LodLevel level) const
{
    VOXL_CHECK(level < kLodCount, "lod level {} out of range", static_cast<int>(level));
    if (level == kLodFull) {
        generate(chunk);  // identical by construction, not by coincidence
        return;
    }
    generateCoarse(chunk, level);
}

void TerrainGenerator::generateCoarse(Chunk& chunk, LodLevel level) const
{
    const ChunkPos& pos = chunk.position();
    VOXL_CHECK(pos.y >= 0 && pos.y < kWorldSectionCount, "chunk section y={} outside the world", pos.y);
    VOXL_ASSERT(level > kLodFull && level < kLodCount, "generateCoarse called at level 0");

    const std::int32_t cell  = lodCellSize(level);
    const std::int32_t half  = cell >> 1;
    const std::int32_t grid  = lodGridSize(level);
    const bool         trees = m_settings.generateTrees && lodPlacesTrees(level);

    const BlockPos     origin      = pos.originBlock();
    const std::int32_t sectionMinY = origin.y;
    const std::int32_t seaLevel    = m_settings.seaLevel;

    ChunkStorage& storage = chunk.storage();
    // Same reset-first rule as the full-resolution path: regenerating a chunk at
    // a new level must not leave any of the old level's voxels behind.
    storage.fill(blocks::Air);
    storage.fillLight(0, 0);

    LodSampleGrid columns;
    columns.shift       = static_cast<std::int32_t>(level);
    columns.pad         = lodPadCells(level, trees);
    columns.span        = grid + 2 * columns.pad;
    columns.cellOriginX = origin.x >> columns.shift;
    columns.cellOriginZ = origin.z >> columns.shift;
    VOXL_ASSERT(static_cast<std::size_t>(columns.span) * static_cast<std::size_t>(columns.span) <=
                    kMaxLodColumns,
                "lod sample grid larger than its worst-case bound");

    // A coarse cell's terrain top can sit up to `half - 1` blocks above the
    // sampled surface, and a quantised tree adds at most one more cell on top of
    // structureTopY, so `+ cell` is a sound bound in both cases.
    std::int32_t highestTop = kWorldMinY;
    for (std::int32_t iz = 0; iz < columns.span; ++iz) {
        for (std::int32_t ix = 0; ix < columns.span; ++ix) {
            const std::int32_t cx = ix - columns.pad;
            const std::int32_t cz = iz - columns.pad;
            ColumnSample&      c  = columns.cells[static_cast<std::size_t>(iz) *
                                            static_cast<std::size_t>(columns.span) +
                                        static_cast<std::size_t>(ix)];
            c = sampleColumn(origin.x + cx * cell + half, origin.z + cz * cell + half);
            highestTop = std::max(highestTop, (trees ? c.structureTopY : c.surfaceY) + cell);
        }
    }

    if (sectionMinY > std::max(highestTop, seaLevel)) {
        if (m_settings.seedSunlight) {
            storage.fillLight(ChunkStorage::kMaxLightLevel, 0);
        }
        chunk.markDirty();
        return;
    }

    // PASS 1 - decide every cell. Kept separate from the writes so the dominant
    // material can be chosen before anything is stored: filling the section with
    // it up front turns the majority of the 32768 per-block writes into nothing
    // at all, and those writes - not the noise - are what otherwise stops the
    // cost from falling any further past level 2.
    std::array<BlockId, kMaxLodCells> cellIds{};
    LodMaterialTally                  tally;

    for (std::int32_t cz = 0; cz < grid; ++cz) {
        for (std::int32_t cx = 0; cx < grid; ++cx) {
            const ColumnSample& sample  = columns.atCell(cx, cz);
            const std::int32_t  centreX = origin.x + cx * cell + half;
            const std::int32_t  centreZ = origin.z + cz * cell + half;
            const SurfaceMaterials materials =
                resolveMaterials(sample, centreX, centreZ, seaLevel, m_settings.seed);

            const bool freezes = sample.temperature < kFreezingTemperature;

            for (std::int32_t cy = 0; cy < grid; ++cy) {
                const std::int32_t cellMinY = sectionMinY + cy * cell;
                const std::int32_t cellMaxY = cellMinY + cell - 1;
                const std::int32_t centreY  = cellMinY + half;

                BlockId id = blocks::Air;

                if (cellMinY == kWorldMinY) {
                    // The bottom cell is bedrock outright. It replaces both the
                    // solid floor and the scattered shelf above it: every block
                    // down there is opaque rock at full resolution too, it is
                    // unreachable, and a uniform cell is what a cell-sampling
                    // consumer needs to see a floor at all.
                    id = blocks::Bedrock;
                } else if (centreY <= sample.surfaceY) {
                    const std::int32_t depth = sample.surfaceY - centreY;
                    // The topmost solid cell always takes the surface material.
                    // Choosing it by `depth == 0` as the fine path does would
                    // give grass to only one cell in 2^L and turn a green world
                    // brown at distance - the most visible LOD defect there is.
                    if (centreY + cell > sample.surfaceY) {
                        id = materials.surface;
                    } else if (depth <= materials.subsurfaceDepth) {
                        id = materials.subsurface;
                    } else if (depth <= materials.subsurfaceDepth + materials.fillerDepth) {
                        id = materials.filler;
                    } else {
                        id = blocks::Stone;
                    }

                    if (m_settings.generateCaves && isCarveable(id) &&
                        carvesCaveSpan(centreX, centreY, centreZ, cellMaxY, cellMinY,
                                       sample.surfaceY)) {
                        id = blocks::Air;
                    }
                } else if (cellMaxY <= seaLevel) {
                    // Water fills only cells that are ENTIRELY submerged, so the
                    // ocean surface lands on a cell boundary at or just below
                    // sea level instead of stepping up to half a cell above it
                    // and drowning the beaches. With the default sea level this
                    // is one block low at every coarse level - the same one
                    // block at all of them, so there is no step between levels.
                    id = (freezes && cellMaxY + cell > seaLevel) ? blocks::Ice : blocks::Water;
                }

                cellIds[lodCellIndex(cx, cy, cz, grid)] = id;
                tally.add(id);
            }
        }
    }

    // A coarse chunk holds a handful of distinct materials, and usually one of
    // them - stone underground, air near the sky, water at sea - covers most of
    // it. Seeding the section with that one collapses to a single 8-byte uniform
    // value and leaves only the minority cells to write.
    const BlockId dominant = tally.dominant();
    storage.fill(dominant);

    // PASS 2 - expand the cells into blocks.
    for (std::int32_t cz = 0; cz < grid; ++cz) {
        for (std::int32_t cx = 0; cx < grid; ++cx) {
            const std::int32_t surfaceY = columns.atCell(cx, cz).surfaceY;

            for (std::int32_t cy = 0; cy < grid; ++cy) {
                const BlockId id         = cellIds[lodCellIndex(cx, cy, cz, grid)];
                const bool    writeBlock = id != dominant;

                for (std::int32_t by = 0; by < cell; ++by) {
                    const std::int32_t localY = cy * cell + by;
                    const std::int32_t worldY = sectionMinY + localY;

                    std::uint8_t sunlight = 0;
                    if (m_settings.seedSunlight && worldY > surfaceY) {
                        sunlight = worldY > seaLevel
                                       ? ChunkStorage::kMaxLightLevel
                                       : static_cast<std::uint8_t>(std::max(
                                             0, static_cast<int>(ChunkStorage::kMaxLightLevel) -
                                                    2 * (seaLevel - worldY + 1)));
                    }
                    if (!writeBlock && sunlight == 0) {
                        continue;
                    }

                    for (std::int32_t bz = 0; bz < cell; ++bz) {
                        // x is the fastest-varying axis of localIndex, so a cell
                        // row is a contiguous run and the index just increments.
                        std::size_t index = localIndex(cx * cell, localY, cz * cell + bz);
                        for (std::int32_t bx = 0; bx < cell; ++bx, ++index) {
                            if (writeBlock) {
                                storage.set(index, id);
                            }
                            if (sunlight != 0) {
                                storage.setLight(index, ChunkStorage::packLight(sunlight, 0));
                            }
                        }
                    }
                }
            }
        }
    }

    if (trees) {
        // Exact columns, on demand. Candidates are sparse enough that this costs
        // far less than the padded full-resolution grid the fine path builds,
        // and it is what makes the level-1 tree set identical to level 0's.
        placeTrees(chunk, origin.x, origin.z, m_settings, level,
                   [this](std::int32_t x, std::int32_t z) { return sampleColumn(x, z); });
    }

    storage.optimise();
    chunk.markDirty();
}

}  // namespace voxl
