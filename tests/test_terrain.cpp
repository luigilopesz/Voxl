#include <catch2/catch_test_macros.hpp>

#include "world/Chunk.hpp"
#include "world/TerrainGenerator.hpp"
#include "world/VoxelTypes.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace voxl;

namespace {

/// Every generated voxel of a rectangular block of chunks, keyed by world
/// position. Assembling a region is what lets the structure tests see a tree
/// that spans a chunk border as one object.
class Region {
public:
    Region(const TerrainGenerator& generator, std::int32_t chunkX0, std::int32_t chunkZ0,
           std::int32_t chunkCountXZ, std::int32_t sectionY0, std::int32_t sectionY1)
        : m_minX(chunkX0 * kChunkSize),
          m_minZ(chunkZ0 * kChunkSize),
          m_maxX(m_minX + chunkCountXZ * kChunkSize - 1),
          m_maxZ(m_minZ + chunkCountXZ * kChunkSize - 1),
          m_minY(sectionY0 * kChunkSize),
          m_maxY(sectionY1 * kChunkSize + kChunkSize - 1)
    {
        for (std::int32_t cz = 0; cz < chunkCountXZ; ++cz) {
            for (std::int32_t cx = 0; cx < chunkCountXZ; ++cx) {
                for (std::int32_t cy = sectionY0; cy <= sectionY1; ++cy) {
                    Chunk chunk(ChunkPos{chunkX0 + cx, cy, chunkZ0 + cz});
                    generator.generate(chunk);
                    const BlockPos origin = chunk.originBlock();
                    for (std::int32_t ly = 0; ly < kChunkSize; ++ly) {
                        for (std::int32_t lz = 0; lz < kChunkSize; ++lz) {
                            for (std::int32_t lx = 0; lx < kChunkSize; ++lx) {
                                const BlockId id = chunk.getBlock(lx, ly, lz);
                                if (id != blocks::Air) {
                                    m_blocks.emplace(
                                        BlockPos{origin.x + lx, origin.y + ly, origin.z + lz}, id);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    [[nodiscard]] BlockId at(std::int32_t x, std::int32_t y, std::int32_t z) const
    {
        const auto it = m_blocks.find(BlockPos{x, y, z});
        return it == m_blocks.end() ? blocks::Air : it->second;
    }

    [[nodiscard]] std::int32_t minX() const { return m_minX; }
    [[nodiscard]] std::int32_t maxX() const { return m_maxX; }
    [[nodiscard]] std::int32_t minZ() const { return m_minZ; }
    [[nodiscard]] std::int32_t maxZ() const { return m_maxZ; }
    [[nodiscard]] std::int32_t minY() const { return m_minY; }
    [[nodiscard]] std::int32_t maxY() const { return m_maxY; }

private:
    std::unordered_map<BlockPos, BlockId> m_blocks;
    std::int32_t                          m_minX, m_minZ, m_maxX, m_maxZ, m_minY, m_maxY;
};

[[nodiscard]] bool sameVoxels(const Chunk& a, const Chunk& b)
{
    for (std::size_t i = 0; i < kChunkVolume; ++i) {
        if (a.getBlock(i) != b.getBlock(i) || a.getLight(i) != b.getLight(i)) {
            return false;
        }
    }
    return true;
}

/// Finds a chunk column whose four corners all sit in the requested biome, so
/// the structure tests do not depend on hard-coded coordinates surviving a
/// change to the noise constants.
[[nodiscard]] bool findBiomeChunk(const TerrainGenerator& generator, BiomeId wanted,
                                  std::int32_t searchChunks, ChunkPos& out)
{
    for (std::int32_t cz = 0; cz < searchChunks; ++cz) {
        for (std::int32_t cx = 0; cx < searchChunks; ++cx) {
            const std::int32_t x0 = cx * kChunkSize;
            const std::int32_t z0 = cz * kChunkSize;
            bool               all = true;
            for (std::int32_t dz = 0; dz <= kChunkSize && all; dz += kChunkSize / 2) {
                for (std::int32_t dx = 0; dx <= kChunkSize && all; dx += kChunkSize / 2) {
                    all = generator.biomeAt(x0 + dx, z0 + dz) == wanted;
                }
            }
            if (all) {
                out = ChunkPos{cx, 0, cz};
                return true;
            }
        }
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------- determinism

TEST_CASE("generating the same chunk twice produces identical voxels", "[world][terrain]")
{
    const TerrainGenerator generator{};

    for (const ChunkPos& pos :
         {ChunkPos{0, 3, 0}, ChunkPos{-7, 2, 11}, ChunkPos{413, 4, -908}, ChunkPos{5, 0, 5}}) {
        Chunk first(pos);
        Chunk second(pos);
        generator.generate(first);
        generator.generate(second);
        INFO("chunk " << pos.x << ',' << pos.y << ',' << pos.z);
        CHECK(sameVoxels(first, second));
    }
}

TEST_CASE("regenerating into a populated chunk is idempotent", "[world][terrain]")
{
    const TerrainGenerator generator{};
    const ChunkPos         pos{19, 3, -4};

    Chunk reference(pos);
    generator.generate(reference);

    Chunk reused(pos);
    generator.generate(reused);
    // Dirty the chunk the way a player edit would, then regenerate over it.
    reused.setBlock(0, 0, 0, blocks::Glowstone);
    reused.setBlock(31, 31, 31, blocks::Planks);
    generator.generate(reused);

    CHECK(sameVoxels(reference, reused));
}

TEST_CASE("terrain is identical when generated concurrently on many threads", "[world][terrain]")
{
    const TerrainGenerator generator{};

    // A vertical column plus scattered neighbours: covers the all-air fast path,
    // the surface sections and the fully underground sections.
    const std::vector<ChunkPos> positions{
        {12, 0, 34}, {12, 1, 34}, {12, 2, 34}, {12, 3, 34},
        {12, 4, 34}, {12, 5, 34}, {12, 6, 34}, {12, 7, 34},
        {-99, 3, 250}, {41, 3, 0}, {0, 3, 0}, {7, 2, -7},
    };

    // Single-threaded reference, generated in order.
    std::vector<std::unique_ptr<Chunk>> reference;
    reference.reserve(positions.size());
    for (const ChunkPos& pos : positions) {
        reference.push_back(std::make_unique<Chunk>(pos));
        generator.generate(*reference.back());
    }

    std::atomic<int>         mismatches{0};
    std::atomic<std::size_t> nextIndex{0};
    std::vector<std::thread> workers;
    const unsigned           threadCount = 4;

    for (unsigned t = 0; t < threadCount; ++t) {
        workers.emplace_back([&] {
            // Every worker walks the whole list, starting at a different offset,
            // so the chunks really are generated in different orders per thread.
            const std::size_t offset = nextIndex.fetch_add(1);
            for (std::size_t step = 0; step < positions.size(); ++step) {
                const std::size_t i = (offset * 3 + step) % positions.size();
                Chunk             chunk(positions[i]);
                generator.generate(chunk);
                if (!sameVoxels(chunk, *reference[i])) {
                    mismatches.fetch_add(1);
                }
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    CHECK(mismatches.load() == 0);
}

TEST_CASE("the same seed reproduces the world and a different seed does not", "[world][terrain]")
{
    TerrainSettings settings;
    settings.seed = 0x0123456789ABCDEFull;

    TerrainSettings other = settings;
    other.seed            = settings.seed + 1;  // adjacent seeds must not correlate

    const TerrainGenerator a(settings);
    const TerrainGenerator b(settings);
    const TerrainGenerator c(other);

    const ChunkPos pos{6, 3, -2};

    Chunk fromA(pos);
    Chunk fromB(pos);
    a.generate(fromA);
    b.generate(fromB);
    CHECK(sameVoxels(fromA, fromB));

    // Compare a whole vertical column so the test does not depend on the chosen
    // section happening to intersect the surface under both seeds.
    bool anyDifference = false;
    for (std::int32_t section = 0; section < kWorldSectionCount && !anyDifference; ++section) {
        Chunk left(ChunkPos{pos.x, section, pos.z});
        Chunk right(ChunkPos{pos.x, section, pos.z});
        a.generate(left);
        c.generate(right);
        anyDifference = !sameVoxels(left, right);
    }
    CHECK(anyDifference);
}

// -------------------------------------------------------------------- bedrock

TEST_CASE("bedrock floors the world and is never carved away", "[world][terrain]")
{
    const TerrainGenerator generator{};

    for (const ChunkPos& pos :
         {ChunkPos{0, 0, 0}, ChunkPos{-31, 0, 64}, ChunkPos{1000, 0, -1000}, ChunkPos{41, 0, 0}}) {
        Chunk chunk(pos);
        generator.generate(chunk);
        INFO("chunk " << pos.x << ',' << pos.y << ',' << pos.z);

        std::int32_t floorVoxels = 0;
        for (std::int32_t z = 0; z < kChunkSize; ++z) {
            for (std::int32_t x = 0; x < kChunkSize; ++x) {
                if (chunk.getBlock(x, 0, z) == blocks::Bedrock) {
                    ++floorVoxels;
                }
            }
        }
        CHECK(floorVoxels == kChunkSize * kChunkSize);
    }
}

TEST_CASE("caves never reach the bedrock shelf", "[world][terrain]")
{
    // The scattered bedrock above the solid floor must survive carving too,
    // otherwise a tunnel can end in an exposed hole in the world's floor.
    const TerrainGenerator generator{};

    for (std::int32_t cz = 0; cz < 2; ++cz) {
        for (std::int32_t cx = 40; cx < 42; ++cx) {
            Chunk chunk(ChunkPos{cx, 0, cz});
            generator.generate(chunk);
            for (std::int32_t y = 0; y <= 3; ++y) {
                for (std::int32_t z = 0; z < kChunkSize; ++z) {
                    for (std::int32_t x = 0; x < kChunkSize; ++x) {
                        const BlockId id = chunk.getBlock(x, y, z);
                        INFO("y=" << y << " block=" << id);
                        // Solid ground of some kind; never carved to air.
                        REQUIRE(id != blocks::Air);
                    }
                }
            }
        }
    }
}

// --------------------------------------------------------------- world bounds

TEST_CASE("the height field always leaves room for structures inside the world",
          "[world][terrain]")
{
    const TerrainGenerator generator{};

    std::int32_t highest = kWorldMinY;
    std::int32_t lowest  = kWorldMaxY;
    for (std::int32_t z = -4000; z <= 4000; z += 61) {
        for (std::int32_t x = -4000; x <= 4000; x += 61) {
            const ColumnSample sample = generator.sampleColumn(x, z);
            REQUIRE(sample.surfaceY >= kWorldMinY);
            REQUIRE(sample.surfaceY <= kWorldMaxY);
            // A structure rooted here must fit under the world ceiling; this is
            // what stops a tree on a peak from being sliced off.
            REQUIRE(sample.surfaceY + kMaxStructureHeight <= kWorldMaxY);
            REQUIRE(sample.structureTopY <= kWorldMaxY);
            REQUIRE(sample.structureTopY >= sample.surfaceY);
            highest = std::max(highest, sample.surfaceY);
            lowest  = std::min(lowest, sample.surfaceY);
        }
    }
    // Sanity: the world actually uses its vertical range rather than being flat.
    CHECK(lowest < kSeaLevel);
    CHECK(highest > kSeaLevel + 60);
}

TEST_CASE("every section of a column generates without escaping the world", "[world][terrain]")
{
    const TerrainGenerator generator{};

    // Generating all eight sections exercises the floor section, the surface
    // sections and the sky fast path. Any write outside [0, kWorldMaxY] would
    // have to go through a local index, so the observable invariant is that the
    // topmost section stays empty above the tallest possible structure.
    for (const ColumnPos& column : {ColumnPos{41, 0}, ColumnPos{-13, 77}, ColumnPos{13, 0}}) {
        // Padded, because a tree rooted in the neighbouring column reaches in.
        std::int32_t columnTop = kWorldMinY;
        for (std::int32_t z = -kMaxStructureRadius; z < kChunkSize + kMaxStructureRadius; ++z) {
            for (std::int32_t x = -kMaxStructureRadius; x < kChunkSize + kMaxStructureRadius; ++x) {
                columnTop = std::max(columnTop,
                                     generator.sampleColumn(column.x * kChunkSize + x,
                                                            column.z * kChunkSize + z)
                                         .structureTopY);
            }
        }

        for (std::int32_t section = 0; section < kWorldSectionCount; ++section) {
            Chunk chunk(ChunkPos{column.x, section, column.z});
            generator.generate(chunk);
            const std::int32_t sectionMinY = section * kChunkSize;
            for (std::int32_t ly = 0; ly < kChunkSize; ++ly) {
                const std::int32_t worldY = sectionMinY + ly;
                if (worldY <= columnTop || worldY <= kSeaLevel) {
                    continue;
                }
                for (std::int32_t z = 0; z < kChunkSize; ++z) {
                    for (std::int32_t x = 0; x < kChunkSize; ++x) {
                        INFO("y=" << worldY << " columnTop=" << columnTop);
                        REQUIRE(chunk.getBlock(x, ly, z) == blocks::Air);
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------- water

TEST_CASE("water never appears above sea level", "[world][terrain]")
{
    const TerrainGenerator generator{};

    for (std::int32_t cz = 0; cz < 3; ++cz) {
        for (std::int32_t cx = 0; cx < 3; ++cx) {
            for (std::int32_t cy = 0; cy < kWorldSectionCount; ++cy) {
                Chunk chunk(ChunkPos{cx, cy, cz});
                generator.generate(chunk);
                for (std::int32_t ly = 0; ly < kChunkSize; ++ly) {
                    const std::int32_t worldY = cy * kChunkSize + ly;
                    if (worldY <= kSeaLevel) {
                        continue;
                    }
                    for (std::int32_t z = 0; z < kChunkSize; ++z) {
                        for (std::int32_t x = 0; x < kChunkSize; ++x) {
                            const BlockId id = chunk.getBlock(x, ly, z);
                            INFO("y=" << worldY);
                            REQUIRE(id != blocks::Water);
                            REQUIRE(id != blocks::Ice);
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("submerged columns are filled with water up to sea level", "[world][terrain]")
{
    const TerrainGenerator generator{};

    ChunkPos ocean{};
    REQUIRE(findBiomeChunk(generator, BiomeId::Ocean, 48, ocean));

    // The water column spans several sections: sea level (96) is the *bottom*
    // voxel of section 3, so everything submerged lives in the sections below it.
    std::int32_t deepest = kSeaLevel;
    for (std::int32_t z = 0; z < kChunkSize; ++z) {
        for (std::int32_t x = 0; x < kChunkSize; ++x) {
            deepest = std::min(deepest,
                               generator.sampleColumn(ocean.x * kChunkSize + x,
                                                      ocean.z * kChunkSize + z)
                                   .surfaceY);
        }
    }
    const Region region(generator, ocean.x, ocean.z, 1, blockToChunkAxis(deepest),
                        blockToChunkAxis(kSeaLevel));

    int submergedColumns = 0;
    for (std::int32_t z = region.minZ(); z <= region.maxZ(); ++z) {
        for (std::int32_t x = region.minX(); x <= region.maxX(); ++x) {
            const ColumnSample sample = generator.sampleColumn(x, z);
            if (sample.surfaceY >= kSeaLevel) {
                continue;
            }
            ++submergedColumns;
            for (std::int32_t y = sample.surfaceY + 1; y <= kSeaLevel; ++y) {
                const BlockId id = region.at(x, y, z);
                INFO("column (" << x << ',' << z << ") y=" << y << " block=" << id);
                REQUIRE((id == blocks::Water || id == blocks::Ice));
            }
            // ... and the voxel holding up the water is solid ground, not a hole
            // punched by a cave.
            INFO("sea floor at (" << x << ',' << sample.surfaceY << ',' << z << ')');
            REQUIRE(region.at(x, sample.surfaceY, z) != blocks::Air);
        }
    }
    CHECK(submergedColumns > 0);
}

// ---------------------------------------------------------------------- caves

TEST_CASE("caves never break through the surface", "[world][terrain]")
{
    const TerrainGenerator generator{};

    for (std::int32_t cz = 0; cz < 2; ++cz) {
        for (std::int32_t cx = 40; cx < 42; ++cx) {
            for (std::int32_t cy = 2; cy < 7; ++cy) {
                Chunk chunk(ChunkPos{cx, cy, cz});
                generator.generate(chunk);
                const BlockPos origin = chunk.originBlock();
                for (std::int32_t z = 0; z < kChunkSize; ++z) {
                    for (std::int32_t x = 0; x < kChunkSize; ++x) {
                        const ColumnSample sample =
                            generator.sampleColumn(origin.x + x, origin.z + z);
                        if (sample.surfaceY < origin.y ||
                            sample.surfaceY >= origin.y + kChunkSize) {
                            continue;
                        }
                        const BlockId id =
                            chunk.getBlock(x, blockToLocalAxis(sample.surfaceY), z);
                        INFO("surface voxel at (" << origin.x + x << ',' << sample.surfaceY << ','
                                                  << origin.z + z << ')');
                        REQUIRE(id != blocks::Air);
                    }
                }
            }
        }
    }
}

TEST_CASE("caves carve a meaningful but bounded amount of the deep world", "[world][terrain]")
{
    const TerrainGenerator generator{};

    std::int64_t hollow = 0;
    std::int64_t total  = 0;
    for (std::int32_t cz = 0; cz < 2; ++cz) {
        for (std::int32_t cx = 40; cx < 42; ++cx) {
            Chunk chunk(ChunkPos{cx, 1, cz});  // y 32..63, comfortably underground
            generator.generate(chunk);
            for (std::size_t i = 0; i < kChunkVolume; ++i) {
                if (chunk.getBlock(i) == blocks::Air) {
                    ++hollow;
                }
                ++total;
            }
        }
    }
    const double fraction = static_cast<double>(hollow) / static_cast<double>(total);
    INFO("carved fraction " << fraction);
    CHECK(fraction > 0.005);  // caves exist at all
    CHECK(fraction < 0.30);   // and did not eat the world

    TerrainSettings noCaves;
    noCaves.generateCaves = false;
    const TerrainGenerator solid(noCaves);
    Chunk                  chunk(ChunkPos{40, 1, 0});
    solid.generate(chunk);
    for (std::size_t i = 0; i < kChunkVolume; ++i) {
        REQUIRE(chunk.getBlock(i) != blocks::Air);
    }
}

// --------------------------------------------------------------------- biomes

TEST_CASE("all required biomes occur and select their own surface", "[world][terrain]")
{
    const TerrainGenerator generator{};

    std::array<int, kBiomeCount> seen{};
    for (std::int32_t z = -3000; z <= 3000; z += 41) {
        for (std::int32_t x = -3000; x <= 3000; x += 41) {
            seen[static_cast<std::size_t>(generator.biomeAt(x, z))]++;
        }
    }
    for (std::size_t i = 0; i < kBiomeCount; ++i) {
        INFO("biome " << toString(static_cast<BiomeId>(i)));
        CHECK(seen[i] > 0);
    }

    // The description table must be addressable for every id and never alias.
    CHECK(biomeDescription(BiomeId::Desert).surface == blocks::Sand);
    CHECK(biomeDescription(BiomeId::Forest).treeDensity >
          biomeDescription(BiomeId::Plains).treeDensity);
    CHECK(biomeDescription(BiomeId::Ocean).treeDensity == 0.0f);
}

TEST_CASE("biome borders are a gradient rather than a cliff", "[world][terrain]")
{
    const TerrainGenerator generator{};

    // Height contributions are blended across biomes, so crossing a border must
    // not produce a step. Mountains are excluded: a ridge crest legitimately has
    // steep faces, and that is terrain, not a border artefact.
    std::int32_t worstBorderStep = 0;
    for (std::int32_t z = -1200; z <= 1200; z += 17) {
        ColumnSample previous = generator.sampleColumn(-1200, z);
        for (std::int32_t x = -1199; x <= 1200; ++x) {
            const ColumnSample current = generator.sampleColumn(x, z);
            if (current.biome != previous.biome && current.biome != BiomeId::Mountains &&
                previous.biome != BiomeId::Mountains) {
                worstBorderStep =
                    std::max(worstBorderStep, std::abs(current.surfaceY - previous.surfaceY));
            }
            previous = current;
        }
    }
    INFO("worst non-mountain biome-border step " << worstBorderStep);
    CHECK(worstBorderStep <= 6);
}

// ---------------------------------------------------------------------- trees

TEST_CASE("trees straddling a chunk border are whole, not clipped", "[world][terrain][structures]")
{
    const TerrainGenerator generator{};

    ChunkPos forest{};
    REQUIRE(findBiomeChunk(generator, BiomeId::Forest, 96, forest));

    // Work out which sections the canopies live in.
    std::int32_t lowest  = kWorldMaxY;
    std::int32_t highest = kWorldMinY;
    for (std::int32_t z = -kMaxStructureRadius; z < 3 * kChunkSize + kMaxStructureRadius; ++z) {
        for (std::int32_t x = -kMaxStructureRadius; x < 3 * kChunkSize + kMaxStructureRadius; ++x) {
            const ColumnSample s =
                generator.sampleColumn(forest.x * kChunkSize + x, forest.z * kChunkSize + z);
            lowest  = std::min(lowest, s.surfaceY);
            highest = std::max(highest, s.structureTopY);
        }
    }
    const std::int32_t section0 = std::max(0, blockToChunkAxis(lowest) - 1);
    const std::int32_t section1 =
        std::min(kWorldSectionCount - 1, blockToChunkAxis(highest) + 1);

    const Region region(generator, forest.x, forest.z, 3, section0, section1);

    // The assembled region spans several chunks, so a tree that a per-chunk
    // generator had clipped would show up here as a canopy missing everything on
    // one side of a chunk boundary.
    int trees       = 0;
    int borderTrees = 0;
    int thinCanopy  = 0;
    for (std::int32_t y = region.minY() + kMaxStructureHeight;
         y <= region.maxY() - kMaxStructureHeight; ++y) {
        for (std::int32_t z = region.minZ() + kMaxStructureRadius + 1;
             z <= region.maxZ() - kMaxStructureRadius - 1; ++z) {
            for (std::int32_t x = region.minX() + kMaxStructureRadius + 1;
                 x <= region.maxX() - kMaxStructureRadius - 1; ++x) {
                if (region.at(x, y, z) != blocks::Wood ||
                    region.at(x, y + 1, z) == blocks::Wood) {
                    continue;  // only the top log of each trunk
                }
                ++trees;

                int leaves = 0;
                for (std::int32_t dy = -kMaxStructureHeight; dy <= kMaxStructureRadius; ++dy) {
                    for (std::int32_t dz = -kMaxStructureRadius; dz <= kMaxStructureRadius; ++dz) {
                        for (std::int32_t dx = -kMaxStructureRadius; dx <= kMaxStructureRadius;
                             ++dx) {
                            if (region.at(x + dx, y + dy, z + dz) == blocks::Leaves) {
                                ++leaves;
                            }
                        }
                    }
                }
                INFO("tree top at " << x << ',' << y << ',' << z << " has " << leaves << " leaves");
                if (leaves < 15) {
                    ++thinCanopy;
                }

                const std::int32_t localX = blockToLocalAxis(x);
                const std::int32_t localZ = blockToLocalAxis(z);
                if (localX < kMaxStructureRadius || localX >= kChunkSize - kMaxStructureRadius ||
                    localZ < kMaxStructureRadius || localZ >= kChunkSize - kMaxStructureRadius) {
                    ++borderTrees;
                }
            }
        }
    }

    REQUIRE(trees > 0);
    CHECK(thinCanopy == 0);
    // Without this the test would pass vacuously on a world where no tree
    // happens to sit near a chunk boundary.
    CHECK(borderTrees > 0);
}

TEST_CASE("trees stand on soil above the waterline", "[world][terrain][structures]")
{
    const TerrainGenerator generator{};

    ChunkPos forest{};
    REQUIRE(findBiomeChunk(generator, BiomeId::Forest, 96, forest));

    std::int32_t lowest  = kWorldMaxY;
    std::int32_t highest = kWorldMinY;
    for (std::int32_t z = 0; z < 2 * kChunkSize; ++z) {
        for (std::int32_t x = 0; x < 2 * kChunkSize; ++x) {
            const ColumnSample s =
                generator.sampleColumn(forest.x * kChunkSize + x, forest.z * kChunkSize + z);
            lowest  = std::min(lowest, s.surfaceY);
            highest = std::max(highest, s.structureTopY);
        }
    }
    const Region region(generator, forest.x, forest.z, 2,
                        std::max(0, blockToChunkAxis(lowest) - 1),
                        std::min(kWorldSectionCount - 1, blockToChunkAxis(highest) + 1));

    int checked = 0;
    for (std::int32_t y = region.minY() + 1; y <= region.maxY(); ++y) {
        for (std::int32_t z = region.minZ(); z <= region.maxZ(); ++z) {
            for (std::int32_t x = region.minX(); x <= region.maxX(); ++x) {
                if (region.at(x, y, z) != blocks::Wood ||
                    region.at(x, y - 1, z) == blocks::Wood) {
                    continue;  // only the bottom log of each trunk
                }
                const BlockId ground = region.at(x, y - 1, z);
                INFO("trunk base at " << x << ',' << y << ',' << z << " stands on " << ground);
                CHECK((ground == blocks::Grass || ground == blocks::Snow));
                CHECK(y - 1 > kSeaLevel);
                ++checked;
            }
        }
    }
    CHECK(checked > 0);
}

TEST_CASE("tree generation can be switched off", "[world][terrain][structures]")
{
    ChunkPos forest{};
    {
        const TerrainGenerator probe{};
        REQUIRE(findBiomeChunk(probe, BiomeId::Forest, 96, forest));
    }

    TerrainSettings settings;
    settings.generateTrees = false;
    const TerrainGenerator generator(settings);

    for (std::int32_t section = 0; section < kWorldSectionCount; ++section) {
        Chunk chunk(ChunkPos{forest.x, section, forest.z});
        generator.generate(chunk);
        for (std::size_t i = 0; i < kChunkVolume; ++i) {
            const BlockId id = chunk.getBlock(i);
            REQUIRE(id != blocks::Wood);
            REQUIRE(id != blocks::Leaves);
        }
    }
}
