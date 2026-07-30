// Voxel lighting: sunlight, block light, removal, and the cross-chunk seam.
//
// Every test drives the real LightEngine against real Chunk objects, because the
// things that break in a lighting engine - the removal pass leaving stale light
// behind, and a seam disagreeing with the same geometry inside one chunk - only
// show up against the actual storage and the actual neighbourhood plumbing.
//
// Geometry is placed in the TOP section (y == kWorldSectionCount - 1) wherever a
// test needs open sky, because that is the one section whose sky slab is the
// world boundary and therefore full sunlight with no neighbour required.

#include "core/JobSystem.hpp"
#include "world/Block.hpp"
#include "world/BlockAccess.hpp"
#include "world/Chunk.hpp"
#include "world/ChunkManager.hpp"
#include "world/ChunkStorage.hpp"
#include "world/LightEngine.hpp"
#include "world/SubVoxel.hpp"
#include "world/VoxelTypes.hpp"
#include "world/WorldSave.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace voxl;

namespace {

constexpr std::int32_t kTopSection = kWorldSectionCount - 1;

using ChunkMap = std::unordered_map<ChunkPos, ChunkPtr>;

/// A chunk filled with one material, already readable so a neighbourhood capture
/// will pick it up.
ChunkPtr makeChunk(ChunkMap& map, const ChunkPos& position, BlockId fill)
{
    ChunkPtr chunk = Chunk::create(position);
    chunk->storage().fill(fill);
    chunk->storage().fillLight(0, 0);
    chunk->forceState(ChunkState::Ready);
    map[position] = chunk;
    return chunk;
}

ChunkNeighbourhood snapshotAround(const ChunkMap& map, const ChunkPos& centre)
{
    return voxl::captureNeighbourhood(centre, [&map](const ChunkPos& position) -> ConstChunkPtr {
        const auto it = map.find(position);
        return it != map.end() ? ConstChunkPtr{it->second} : nullptr;
    });
}

/// Every chunk in `map` is writable; the streaming states that make a chunk
/// unwritable in the real world are exercised in test_world.cpp.
LightWorld makeLightWorld(ChunkMap& map)
{
    return LightWorld(
        [&map](const ChunkPos& position) -> ChunkPtr {
            const auto it = map.find(position);
            return it != map.end() ? it->second : nullptr;
        },
        [](const ChunkPos&) { return true; });
}

[[nodiscard]] std::uint8_t sunAt(const Chunk& chunk, std::int32_t x, std::int32_t y, std::int32_t z)
{
    return ChunkStorage::unpackSunlight(chunk.getLight(localIndex(x, y, z)));
}

[[nodiscard]] std::uint8_t blockLightAt(const Chunk& chunk, std::int32_t x, std::int32_t y,
                                        std::int32_t z)
{
    return ChunkStorage::unpackBlockLight(chunk.getLight(localIndex(x, y, z)));
}

/// Carves a vertical shaft from the top of the chunk down to `bottomY`.
void carveShaft(Chunk& chunk, std::int32_t x, std::int32_t z, std::int32_t bottomY)
{
    for (std::int32_t y = kChunkSize - 1; y >= bottomY; --y) {
        chunk.storage().set(x, y, z, blocks::Air);
    }
}

void carveRow(Chunk& chunk, std::int32_t fromX, std::int32_t toX, std::int32_t y, std::int32_t z)
{
    for (std::int32_t x = fromX; x <= toX; ++x) {
        chunk.storage().set(x, y, z, blocks::Air);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
//  Sunlight
// ---------------------------------------------------------------------------

TEST_CASE("sunlight falls through an empty column without attenuating", "[light][sun]")
{
    const BlockRegistry registry = createDefaultBlockRegistry();
    LightEngine         engine(registry);

    ChunkMap       map;
    const ChunkPos position{0, kTopSection, 0};
    ChunkPtr       chunk = makeChunk(map, position, blocks::Air);
    for (std::int32_t z = 0; z < kChunkSize; ++z) {
        for (std::int32_t x = 0; x < kChunkSize; ++x) {
            chunk->storage().set(x, 0, z, blocks::Stone);
        }
    }

    engine.lightChunk(*chunk, snapshotAround(map, position), nullptr);

    // THE property that makes open ground uniformly bright: 31 blocks of fall and
    // the level is still 15. A cost of 1 per block would leave the ground at 0.
    for (std::int32_t y = 1; y < kChunkSize; ++y) {
        INFO("y = " << y);
        CHECK(sunAt(*chunk, 16, y, 16) == 15);
    }
    CHECK(sunAt(*chunk, 16, 0, 16) == 0);  // inside the stone floor
}

TEST_CASE("a roof casts a dark region beneath it", "[light][sun]")
{
    const BlockRegistry registry = createDefaultBlockRegistry();
    LightEngine         engine(registry);

    ChunkMap       map;
    const ChunkPos position{0, kTopSection, 0};
    ChunkPtr       chunk = makeChunk(map, position, blocks::Air);

    constexpr std::int32_t kRoofY = 20;
    for (std::int32_t z = 0; z < kChunkSize; ++z) {
        for (std::int32_t x = 0; x < kChunkSize; ++x) {
            chunk->storage().set(x, kRoofY, z, blocks::Stone);
        }
    }

    engine.lightChunk(*chunk, snapshotAround(map, position), nullptr);

    CHECK(sunAt(*chunk, 16, kRoofY + 1, 16) == 15);
    CHECK(sunAt(*chunk, 16, kRoofY, 16) == 0);
    // The roof spans the chunk and the unloaded neighbours are treated as walls,
    // so nothing can creep in from the side.
    for (std::int32_t y = 0; y < kRoofY; ++y) {
        INFO("y = " << y);
        CHECK(sunAt(*chunk, 16, y, 16) == 0);
    }
}

TEST_CASE("sunlight spreading sideways loses exactly one level per block",
          "[light][sun]")
{
    const BlockRegistry registry = createDefaultBlockRegistry();
    LightEngine         engine(registry);

    ChunkMap       map;
    const ChunkPos position{0, kTopSection, 0};
    ChunkPtr       chunk = makeChunk(map, position, blocks::Stone);

    // A shaft down to a one-voxel corridor running away from it. The corridor is
    // the only path, so every cell's level is fixed by its distance alone.
    constexpr std::int32_t kCorridorY = 16;
    constexpr std::int32_t kCorridorZ = 16;
    constexpr std::int32_t kShaftX    = 4;
    carveShaft(*chunk, kShaftX, kCorridorZ, kCorridorY);
    carveRow(*chunk, kShaftX, kChunkSize - 1, kCorridorY, kCorridorZ);

    engine.lightChunk(*chunk, snapshotAround(map, position), nullptr);

    CHECK(sunAt(*chunk, kShaftX, kCorridorY, kCorridorZ) == 15);
    for (std::int32_t step = 1; step <= 15; ++step) {
        INFO("step = " << step);
        CHECK(sunAt(*chunk, kShaftX + step, kCorridorY, kCorridorZ) ==
              static_cast<std::uint8_t>(15 - step));
    }
    CHECK(sunAt(*chunk, kShaftX + 16, kCorridorY, kCorridorZ) == 0);
}

TEST_CASE("water subtracts its attenuation from the sky column", "[light][sun]")
{
    const BlockRegistry registry = createDefaultBlockRegistry();
    LightEngine         engine(registry);
    REQUIRE(engine.attenuation(blocks::Water) == 2);

    ChunkMap       map;
    const ChunkPos position{0, kTopSection, 0};
    ChunkPtr       chunk = makeChunk(map, position, blocks::Air);
    // Two full layers, not a single column: an isolated water block would be lit
    // round the side by its air neighbours and the test would measure that path
    // instead of the vertical one.
    for (std::int32_t z = 0; z < kChunkSize; ++z) {
        for (std::int32_t x = 0; x < kChunkSize; ++x) {
            chunk->storage().set(x, 20, z, blocks::Water);
            chunk->storage().set(x, 19, z, blocks::Water);
        }
    }

    engine.lightChunk(*chunk, snapshotAround(map, position), nullptr);

    CHECK(sunAt(*chunk, 16, 21, 16) == 15);
    // The free fall costs nothing but the water's own attenuation still applies.
    CHECK(sunAt(*chunk, 16, 20, 16) == 13);
    // Below 15 the ordinary per-block cost is back, on top of the attenuation.
    CHECK(sunAt(*chunk, 16, 19, 16) == 10);
    CHECK(sunAt(*chunk, 16, 18, 16) == 9);
}

// ---------------------------------------------------------------------------
//  Block light
// ---------------------------------------------------------------------------

TEST_CASE("an emissive block lights a radius equal to its emission", "[light][block]")
{
    const BlockRegistry registry = createDefaultBlockRegistry();
    LightEngine         engine(registry);
    REQUIRE(engine.emission(blocks::Glowstone) == 15);

    ChunkMap       map;
    const ChunkPos position{0, 3, 0};  // buried: no sky, so only the lamp lights it
    ChunkPtr       chunk = makeChunk(map, position, blocks::Air);
    chunk->storage().set(16, 16, 16, blocks::Glowstone);

    engine.lightChunk(*chunk, snapshotAround(map, position), nullptr);

    CHECK(blockLightAt(*chunk, 16, 16, 16) == 15);
    for (std::int32_t step = 1; step <= 15; ++step) {
        INFO("step = " << step);
        CHECK(blockLightAt(*chunk, 16 + step, 16, 16) == static_cast<std::uint8_t>(15 - step));
        CHECK(blockLightAt(*chunk, 16, 16 + step, 16) == static_cast<std::uint8_t>(15 - step));
        CHECK(blockLightAt(*chunk, 16, 16, 16 + step) == static_cast<std::uint8_t>(15 - step));
    }
    // Diagonals follow the same L1 metric, which is what a six-way BFS produces.
    CHECK(blockLightAt(*chunk, 20, 19, 18) == static_cast<std::uint8_t>(15 - (4 + 3 + 2)));
    CHECK(blockLightAt(*chunk, 16, 0, 16) == 0);
}

// ---------------------------------------------------------------------------
//  Removal - the half that a re-flood alone cannot do
// ---------------------------------------------------------------------------

TEST_CASE("removing an emissive block returns the region to darkness", "[light][block][removal]")
{
    const BlockRegistry registry = createDefaultBlockRegistry();
    LightEngine         engine(registry);

    ChunkMap       map;
    const ChunkPos position{0, 3, 0};
    ChunkPtr       chunk = makeChunk(map, position, blocks::Air);

    const BlockPos    origin     = position.originBlock();
    const std::size_t lampIndex  = localIndex(16, 16, 16);
    const BlockPos    lampWorld{origin.x + 16, origin.y + 16, origin.z + 16};

    chunk->storage().set(lampIndex, blocks::Glowstone);
    engine.lightChunk(*chunk, snapshotAround(map, position), nullptr);
    REQUIRE(blockLightAt(*chunk, 16, 16, 16) == 15);
    REQUIRE(blockLightAt(*chunk, 22, 16, 16) == 9);

    // Take the lamp out. Propagation alone can never fix this: every stale value
    // is at least as large as anything a re-flood would try to write, so every
    // write loses the comparison and the glow stays forever.
    chunk->setBlock(lampIndex, blocks::Air);
    LightWorld world = makeLightWorld(map);
    const LightUpdateStats stats = engine.voxelChanged(world, lampWorld);
    CHECK_FALSE(stats.exhausted);
    CHECK(stats.writesRefused == 0);
    CHECK(stats.cellsCleared > 0);

    // Exhaustive, not spot-checked: a removal bug typically leaves a shell of
    // survivors at one particular radius, which a handful of samples walks past.
    std::size_t litCells = 0;
    for (std::size_t i = 0; i < kChunkVolume; ++i) {
        if (ChunkStorage::unpackBlockLight(chunk->getLight(i)) != 0) {
            ++litCells;
        }
    }
    CHECK(litCells == 0);
}

TEST_CASE("removing one of two lamps leaves the other's field exactly intact",
          "[light][block][removal]")
{
    const BlockRegistry registry = createDefaultBlockRegistry();
    LightEngine         engine(registry);

    ChunkMap       map;
    const ChunkPos position{0, 3, 0};
    const BlockPos origin = position.originBlock();

    // Reference: what the world should look like with only the surviving lamp.
    ChunkMap referenceMap;
    ChunkPtr reference = makeChunk(referenceMap, position, blocks::Air);
    reference->storage().set(localIndex(22, 16, 16), blocks::Glowstone);
    engine.lightChunk(*reference, snapshotAround(referenceMap, position), nullptr);

    ChunkPtr chunk = makeChunk(map, position, blocks::Air);
    chunk->storage().set(localIndex(16, 16, 16), blocks::Glowstone);
    chunk->storage().set(localIndex(22, 16, 16), blocks::Glowstone);
    engine.lightChunk(*chunk, snapshotAround(map, position), nullptr);

    // The two fields overlap, so clearing the first lamp's contribution wrongly
    // erases cells the second one owns unless the removal pass collects them as
    // a boundary and re-propagates from there.
    REQUIRE(blockLightAt(*chunk, 19, 16, 16) == 12);

    chunk->setBlock(localIndex(16, 16, 16), blocks::Air);
    LightWorld world = makeLightWorld(map);
    engine.voxelChanged(world, BlockPos{origin.x + 16, origin.y + 16, origin.z + 16});

    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < kChunkVolume; ++i) {
        if (ChunkStorage::unpackBlockLight(chunk->getLight(i)) !=
            ChunkStorage::unpackBlockLight(reference->getLight(i))) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);
}

TEST_CASE("placing an opaque block darkens the column behind it", "[light][sun][removal]")
{
    const BlockRegistry registry = createDefaultBlockRegistry();
    LightEngine         engine(registry);

    ChunkMap       map;
    const ChunkPos position{0, kTopSection, 0};
    const BlockPos origin = position.originBlock();
    ChunkPtr       chunk  = makeChunk(map, position, blocks::Stone);

    constexpr std::int32_t kShaftX      = 16;
    constexpr std::int32_t kShaftZ      = 16;
    constexpr std::int32_t kShaftBottom = 4;
    constexpr std::int32_t kPlugY       = 20;
    carveShaft(*chunk, kShaftX, kShaftZ, kShaftBottom);

    engine.lightChunk(*chunk, snapshotAround(map, position), nullptr);
    REQUIRE(sunAt(*chunk, kShaftX, kShaftBottom, kShaftZ) == 15);

    chunk->setBlock(localIndex(kShaftX, kPlugY, kShaftZ), blocks::Stone);
    LightWorld world = makeLightWorld(map);
    const LightUpdateStats stats =
        engine.voxelChanged(world, BlockPos{origin.x + kShaftX, origin.y + kPlugY,
                                            origin.z + kShaftZ});
    CHECK_FALSE(stats.exhausted);

    CHECK(sunAt(*chunk, kShaftX, kPlugY + 1, kShaftZ) == 15);
    CHECK(sunAt(*chunk, kShaftX, kPlugY, kShaftZ) == 0);
    for (std::int32_t y = kShaftBottom; y < kPlugY; ++y) {
        INFO("y = " << y);
        CHECK(sunAt(*chunk, kShaftX, y, kShaftZ) == 0);
    }
}

TEST_CASE("breaking the plug lets the sunlight column back in", "[light][sun][removal]")
{
    const BlockRegistry registry = createDefaultBlockRegistry();
    LightEngine         engine(registry);

    ChunkMap       map;
    const ChunkPos position{0, kTopSection, 0};
    const BlockPos origin = position.originBlock();
    ChunkPtr       chunk  = makeChunk(map, position, blocks::Stone);

    constexpr std::int32_t kShaftX = 16;
    constexpr std::int32_t kShaftZ = 16;
    constexpr std::int32_t kPlugY  = 20;
    carveShaft(*chunk, kShaftX, kShaftZ, 4);
    chunk->storage().set(kShaftX, kPlugY, kShaftZ, blocks::Stone);

    engine.lightChunk(*chunk, snapshotAround(map, position), nullptr);
    REQUIRE(sunAt(*chunk, kShaftX, 4, kShaftZ) == 0);

    chunk->setBlock(localIndex(kShaftX, kPlugY, kShaftZ), blocks::Air);
    LightWorld world = makeLightWorld(map);
    engine.voxelChanged(world,
                        BlockPos{origin.x + kShaftX, origin.y + kPlugY, origin.z + kShaftZ});

    for (std::int32_t y = 4; y <= kPlugY; ++y) {
        INFO("y = " << y);
        CHECK(sunAt(*chunk, kShaftX, y, kShaftZ) == 15);
    }
}

// ---------------------------------------------------------------------------
//  Chunk borders
// ---------------------------------------------------------------------------

namespace {

/// Sunlight along a one-voxel corridor at (startX + k, y, z), read out of
/// whichever chunk owns that column.
std::vector<std::uint8_t> corridorProfile(const ChunkMap& map, std::int32_t sectionY,
                                          std::int32_t startX, std::int32_t z, std::int32_t y,
                                          std::int32_t length)
{
    std::vector<std::uint8_t> out;
    out.reserve(static_cast<std::size_t>(length));
    for (std::int32_t k = 0; k < length; ++k) {
        const std::int32_t worldX = startX + k;
        const auto it = map.find(ChunkPos{blockToChunkAxis(worldX), sectionY, blockToChunkAxis(z)});
        REQUIRE(it != map.end());
        out.push_back(sunAt(*it->second, blockToLocalAxis(worldX), y, blockToLocalAxis(z)));
    }
    return out;
}

/// A stone chunk with a one-voxel corridor running along x, plus an optional
/// sky shaft at `shaftLocalX`.
ChunkPtr makeCorridorChunk(ChunkMap& map, const ChunkPos& position, std::int32_t fromX,
                           std::int32_t toX, std::int32_t y, std::int32_t z,
                           std::int32_t shaftLocalX)
{
    ChunkPtr chunk = makeChunk(map, position, blocks::Stone);
    carveRow(*chunk, fromX, toX, y, z);
    if (shaftLocalX >= 0) {
        carveShaft(*chunk, shaftLocalX, z, y);
    }
    return chunk;
}

}  // namespace

TEST_CASE("light crossing a chunk border matches the same corridor inside one chunk",
          "[light][seam]")
{
    const BlockRegistry registry = createDefaultBlockRegistry();
    LightEngine         engine(registry);

    constexpr std::int32_t kY      = 16;
    constexpr std::int32_t kZ      = 16;
    constexpr std::int32_t kLength = 25;

    // ---- reference: the whole corridor inside one chunk ----
    ChunkMap       referenceMap;
    const ChunkPos referencePos{0, kTopSection, 0};
    makeCorridorChunk(referenceMap, referencePos, 4, 4 + kLength - 1, kY, kZ, 4);
    engine.lightChunk(*referenceMap.at(referencePos), snapshotAround(referenceMap, referencePos),
                      nullptr);
    const std::vector<std::uint8_t> expected =
        corridorProfile(referenceMap, kTopSection, 4, kZ, kY, kLength);

    // Sanity: the corridor really is a gradient, so a seam bug has something to
    // show up against rather than a flat row of zeroes.
    REQUIRE(expected.front() == 15);
    REQUIRE(expected[10] == 5);
    REQUIRE(expected.back() == 0);

    // ---- split: the same 25 cells straddling the border at world x == 32 ----
    // Corridor from world 20 (in chunk 0) to world 44 (in chunk 1), shaft at 20.
    const ChunkPos leftPos{0, kTopSection, 0};
    const ChunkPos rightPos{1, kTopSection, 0};

    SECTION("the far chunk is lit second and reads the near one directly")
    {
        ChunkMap map;
        makeCorridorChunk(map, leftPos, 20, kChunkSize - 1, kY, kZ, 20);
        makeCorridorChunk(map, rightPos, 0, 12, kY, kZ, -1);

        engine.lightChunk(*map.at(leftPos), snapshotAround(map, leftPos), nullptr);
        engine.lightChunk(*map.at(rightPos), snapshotAround(map, rightPos), nullptr);

        CHECK(corridorProfile(map, kTopSection, 20, kZ, kY, kLength) == expected);
    }

    SECTION("the far chunk is lit first, so the near one has to spill into it")
    {
        ChunkMap map;
        makeCorridorChunk(map, leftPos, 20, kChunkSize - 1, kY, kZ, 20);
        makeCorridorChunk(map, rightPos, 0, 12, kY, kZ, -1);

        // Worst case for a streaming pipeline: the neighbour is lit while the
        // chunk that will feed it is still dark, so its corridor comes out black
        // and only the spill can rescue it.
        engine.lightChunk(*map.at(rightPos), snapshotAround(map, rightPos), nullptr);
        CHECK(sunAt(*map.at(rightPos), 0, kY, kZ) == 0);

        LightSpill spill;
        engine.lightChunk(*map.at(leftPos), snapshotAround(map, leftPos), &spill);
        REQUIRE_FALSE(spill.empty());

        LightWorld world = makeLightWorld(map);
        const LightUpdateStats stats = engine.applySeeds(world, spill.data(), spill.size());
        CHECK(stats.writesRefused == 0);

        CHECK(corridorProfile(map, kTopSection, 20, kZ, kY, kLength) == expected);
    }
}

TEST_CASE("a lamp beside a chunk border lights both sides identically", "[light][seam][block]")
{
    const BlockRegistry registry = createDefaultBlockRegistry();
    LightEngine         engine(registry);

    // Reference: lamp in the middle of one chunk, nothing to cross.
    ChunkMap       referenceMap;
    const ChunkPos referencePos{0, 3, 0};
    ChunkPtr       reference = makeChunk(referenceMap, referencePos, blocks::Air);
    reference->storage().set(localIndex(16, 16, 16), blocks::Glowstone);
    engine.lightChunk(*reference, snapshotAround(referenceMap, referencePos), nullptr);

    // Split: lamp one block inside the left chunk's +X face.
    ChunkMap       map;
    const ChunkPos leftPos{0, 3, 0};
    const ChunkPos rightPos{1, 3, 0};
    ChunkPtr       left  = makeChunk(map, leftPos, blocks::Air);
    ChunkPtr       right = makeChunk(map, rightPos, blocks::Air);
    left->storage().set(localIndex(kChunkSize - 1, 16, 16), blocks::Glowstone);

    LightSpill spill;
    engine.lightChunk(*right, snapshotAround(map, rightPos), &spill);
    engine.lightChunk(*left, snapshotAround(map, leftPos), &spill);

    LightWorld world = makeLightWorld(map);
    engine.applySeeds(world, spill.data(), spill.size());

    // Walk out of the lamp across the seam and compare against the same distance
    // measured inside the reference chunk.
    for (std::int32_t step = 0; step <= 14; ++step) {
        INFO("step = " << step);
        const std::int32_t worldX = (kChunkSize - 1) + step;
        const auto it = map.find(ChunkPos{blockToChunkAxis(worldX), 3, 0});
        REQUIRE(it != map.end());
        CHECK(blockLightAt(*it->second, blockToLocalAxis(worldX), 16, 16) ==
              blockLightAt(*reference, 16 + step, 16, 16));
    }
}

// ---------------------------------------------------------------------------
//  Whole columns
// ---------------------------------------------------------------------------

TEST_CASE("a column light job carries the sky down through every section",
          "[light][sun][column]")
{
    const BlockRegistry registry = createDefaultBlockRegistry();
    LightEngine         engine(registry);

    constexpr std::int32_t kGroundY = kSeaLevel;  // 96: solid at and below this

    ChunkMap        map;
    LightColumnWork work;
    work.column = ColumnPos{0, 0};
    for (std::int32_t y = 0; y < kWorldSectionCount; ++y) {
        const ChunkPos position{0, y, 0};
        ChunkPtr       chunk = makeChunk(map, position, blocks::Air);
        for (std::int32_t ly = 0; ly < kChunkSize; ++ly) {
            if (y * kChunkSize + ly > kGroundY) {
                break;
            }
            for (std::int32_t lz = 0; lz < kChunkSize; ++lz) {
                for (std::int32_t lx = 0; lx < kChunkSize; ++lx) {
                    chunk->storage().set(lx, ly, lz, blocks::Stone);
                }
            }
        }
        work.targets[static_cast<std::size_t>(y)]           = chunk;
        work.region[LightColumnWork::regionIndex(0, 0, y)]  = chunk;
    }

    const LightSpill spill = engine.lightColumn(work);
    CHECK(spill.empty());  // nothing around it to spill into

    const auto sunAtWorldY = [&map](std::int32_t worldY) {
        const ChunkPtr& chunk = map.at(ChunkPos{0, blockToChunkAxis(worldY), 0});
        return sunAt(*chunk, 16, blockToLocalAxis(worldY), 16);
    };

    // 159 blocks of free fall across five section boundaries. Lighting sections
    // independently and in the wrong order is exactly what leaves a dark band at
    // one of those boundaries.
    for (std::int32_t worldY = kGroundY + 1; worldY <= kWorldMaxY; ++worldY) {
        INFO("world y = " << worldY);
        CHECK(sunAtWorldY(worldY) == 15);
    }
    CHECK(sunAtWorldY(kGroundY) == 0);
    CHECK(sunAtWorldY(kGroundY - 1) == 0);
}

TEST_CASE("a cave under the surface stays dark while the surface is lit",
          "[light][sun][column]")
{
    const BlockRegistry registry = createDefaultBlockRegistry();
    LightEngine         engine(registry);

    constexpr std::int32_t kGroundY = kSeaLevel;
    constexpr std::int32_t kCaveY   = kSeaLevel - 30;  // two sections down

    ChunkMap        map;
    LightColumnWork work;
    work.column = ColumnPos{0, 0};
    for (std::int32_t y = 0; y < kWorldSectionCount; ++y) {
        const ChunkPos position{0, y, 0};
        ChunkPtr       chunk = makeChunk(map, position, blocks::Air);
        for (std::int32_t ly = 0; ly < kChunkSize; ++ly) {
            const std::int32_t worldY = y * kChunkSize + ly;
            if (worldY > kGroundY) {
                break;
            }
            const BlockId fill = worldY == kCaveY ? blocks::Air : blocks::Stone;
            for (std::int32_t lz = 0; lz < kChunkSize; ++lz) {
                for (std::int32_t lx = 0; lx < kChunkSize; ++lx) {
                    chunk->storage().set(lx, ly, lz, fill);
                }
            }
        }
        work.targets[static_cast<std::size_t>(y)]          = chunk;
        work.region[LightColumnWork::regionIndex(0, 0, y)] = chunk;
    }

    engine.lightColumn(work);

    const ChunkPtr& caveChunk = map.at(ChunkPos{0, blockToChunkAxis(kCaveY), 0});
    // This is the whole point of the feature: a sealed cavern is black, not as
    // bright as the field above it.
    CHECK(sunAt(*caveChunk, 16, blockToLocalAxis(kCaveY), 16) == 0);
    CHECK(ChunkStorage::unpackBlockLight(
              caveChunk->getLight(localIndex(16, blockToLocalAxis(kCaveY), 16))) == 0);

    const ChunkPtr& skyChunk = map.at(ChunkPos{0, blockToChunkAxis(kGroundY + 1), 0});
    CHECK(sunAt(*skyChunk, 16, blockToLocalAxis(kGroundY + 1), 16) == 15);
}

// ---------------------------------------------------------------------------
//  Sub-voxels
// ---------------------------------------------------------------------------

TEST_CASE("a carved sub-voxel tunnel admits daylight", "[light][subvoxel]")
{
    const BlockRegistry registry = createDefaultBlockRegistry();
    LightEngine         engine(registry);

    ChunkMap       map;
    const ChunkPos position{0, kTopSection, 0};
    ChunkPtr       chunk = makeChunk(map, position, blocks::Stone);

    constexpr std::int32_t kTunnelX      = 16;
    constexpr std::int32_t kTunnelZ      = 16;
    constexpr std::int32_t kTunnelBottom = 20;

    engine.lightChunk(*chunk, snapshotAround(map, position), nullptr);
    REQUIRE(sunAt(*chunk, kTunnelX, kTunnelBottom, kTunnelZ) == 0);

    // One sub-voxel out of each block is enough: the engine treats any damaged
    // block as empty space, the same way the mesher stops drawing it as a cube.
    for (std::int32_t y = kChunkSize - 1; y >= kTunnelBottom; --y) {
        const SubVoxelEdit edit =
            chunk->breakSubVoxel(localIndex(kTunnelX, y, kTunnelZ), subVoxelIndex(0, 0, 0));
        REQUIRE(edit == SubVoxelEdit::Modified);
    }
    REQUIRE(chunk->hasSubVoxelDamage());

    engine.lightChunk(*chunk, snapshotAround(map, position), nullptr);

    for (std::int32_t y = kChunkSize - 1; y >= kTunnelBottom; --y) {
        INFO("y = " << y);
        CHECK(sunAt(*chunk, kTunnelX, y, kTunnelZ) == 15);
    }
    // The intact rock beside the tunnel still blocks light completely...
    CHECK(sunAt(*chunk, kTunnelX + 1, kTunnelBottom, kTunnelZ) == 0);
    // ...and the shaft stops where the damage stops.
    CHECK(sunAt(*chunk, kTunnelX, kTunnelBottom - 1, kTunnelZ) == 0);
}

TEST_CASE("chipping a block incrementally opens it to light", "[light][subvoxel][removal]")
{
    const BlockRegistry registry = createDefaultBlockRegistry();
    LightEngine         engine(registry);

    ChunkMap       map;
    const ChunkPos position{0, kTopSection, 0};
    const BlockPos origin = position.originBlock();
    ChunkPtr       chunk  = makeChunk(map, position, blocks::Stone);

    constexpr std::int32_t kX    = 16;
    constexpr std::int32_t kZ    = 16;
    constexpr std::int32_t kPlug = 24;
    carveShaft(*chunk, kX, kZ, kPlug + 1);
    for (std::int32_t y = kPlug - 1; y >= 16; --y) {
        chunk->storage().set(kX, y, kZ, blocks::Air);
    }

    engine.lightChunk(*chunk, snapshotAround(map, position), nullptr);
    REQUIRE(sunAt(*chunk, kX, kPlug + 1, kZ) == 15);
    REQUIRE(sunAt(*chunk, kX, 16, kZ) == 0);

    // Damage the plug. It becomes transparent, so the shaft joins up.
    REQUIRE(chunk->breakSubVoxel(localIndex(kX, kPlug, kZ), subVoxelIndex(0, 0, 0)) ==
            SubVoxelEdit::Modified);

    LightWorld world = makeLightWorld(map);
    engine.voxelChanged(world, BlockPos{origin.x + kX, origin.y + kPlug, origin.z + kZ});

    for (std::int32_t y = 16; y <= kPlug; ++y) {
        INFO("y = " << y);
        CHECK(sunAt(*chunk, kX, y, kZ) == 15);
    }
}

// ---------------------------------------------------------------------------
//  Bookkeeping the streamer relies on
// ---------------------------------------------------------------------------

TEST_CASE("a light pass records which chunk faces changed", "[light][seam]")
{
    const BlockRegistry registry = createDefaultBlockRegistry();
    LightEngine         engine(registry);

    ChunkMap       map;
    const ChunkPos position{0, 3, 0};
    const BlockPos origin = position.originBlock();
    makeChunk(map, position, blocks::Air);

    LightWorld world = makeLightWorld(map);
    const LightSeed seed{BlockPos{origin.x + 0, origin.y + 16, origin.z + 16}, 0, 15};
    engine.applySeeds(world, &seed, 1);

    REQUIRE(world.touched().size() == 1);
    CHECK(world.touched().front().position == position);
    // The seed sits on the -X face, so the neighbour across it has to remesh:
    // its greedy sweep samples our light as part of its skirt.
    const std::uint8_t negX = static_cast<std::uint8_t>(1u << static_cast<int>(Direction::NegX));
    CHECK((world.touched().front().faceMask & negX) != 0);
}

TEST_CASE("an edit that changes no light property is recognised as a no-op", "[light]")
{
    const BlockRegistry registry = createDefaultBlockRegistry();
    LightEngine         engine(registry);

    // Same opacity, same attenuation, same emission: nothing to recompute.
    CHECK_FALSE(engine.affectsLight(blocks::Stone, blocks::Dirt));
    CHECK(engine.affectsLight(blocks::Stone, blocks::Air));
    CHECK(engine.affectsLight(blocks::Air, blocks::Glowstone));
    CHECK(engine.affectsLight(blocks::Air, blocks::Water));
}

TEST_CASE("coarse LOD chunks still end up lit from the sky", "[light][lod]")
{
    const BlockRegistry registry = createDefaultBlockRegistry();
    LightEngine         engine(registry);

    ChunkMap       map;
    const ChunkPos position{0, kTopSection, 0};
    ChunkPtr       chunk = makeChunk(map, position, blocks::Air);
    chunk->setLod(3);  // past kMaxPropagatedLod: the cheap top-down sweep
    for (std::int32_t z = 0; z < kChunkSize; ++z) {
        for (std::int32_t x = 0; x < kChunkSize; ++x) {
            chunk->storage().set(x, 8, z, blocks::Stone);
        }
    }

    engine.lightChunk(*chunk, snapshotAround(map, position), nullptr);

    // The surface - the only thing a chunk this far away contributes to the
    // picture - agrees exactly with what the full flood would produce.
    CHECK(sunAt(*chunk, 16, 9, 16) == 15);
    CHECK(sunAt(*chunk, 16, 31, 16) == 15);
    CHECK(sunAt(*chunk, 16, 8, 16) == 0);
    CHECK(sunAt(*chunk, 16, 7, 16) == 0);
}

namespace {

/// Builds a whole column and times one lightColumn over it. `caveDepth` is how
/// far below the surface the cave layer reaches; everything deeper is solid, so
/// those sections stay uniform and take the fast path.
std::int64_t timeColumn(LightEngine& engine, std::int32_t caveDepth)
{
    ChunkMap        map;
    LightColumnWork work;
    work.column = ColumnPos{0, 0};
    for (std::int32_t y = 0; y < kWorldSectionCount; ++y) {
        const ChunkPos position{0, y, 0};
        ChunkPtr       chunk = makeChunk(map, position, blocks::Air);
        for (std::int32_t ly = 0; ly < kChunkSize; ++ly) {
            const std::int32_t worldY = y * kChunkSize + ly;
            for (std::int32_t lz = 0; lz < kChunkSize; ++lz) {
                for (std::int32_t lx = 0; lx < kChunkSize; ++lx) {
                    const std::int32_t surface = kSeaLevel + ((lx * 7 + lz * 11) % 9);
                    if (worldY > surface) {
                        continue;
                    }
                    const bool cave = worldY > surface - caveDepth &&
                                      ((lx + worldY * 3 + lz * 5) % 23) < 4;
                    chunk->storage().set(lx, ly, lz, cave ? blocks::Air : blocks::Stone);
                }
            }
        }
        // What the real generator does before handing a chunk on, and it matters
        // here: without it a section of nothing but stone stays paletted instead
        // of collapsing to the uniform representation, and the measurement would
        // miss the fast path that most of a real column takes.
        chunk->storage().optimise();
        work.targets[static_cast<std::size_t>(y)]          = chunk;
        work.region[LightColumnWork::regionIndex(0, 0, y)] = chunk;
    }

    const auto start = std::chrono::steady_clock::now();
    engine.lightColumn(work);
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - start)
        .count();
}

}  // namespace

// ---------------------------------------------------------------------------
//  Concurrency: what the main-thread reader is allowed to remember
// ---------------------------------------------------------------------------

TEST_CASE("the chunk memo does not outlive the pass that filled it",
          "[light][threading][regression]")
{
    const BlockRegistry registry = createDefaultBlockRegistry();
    LightEngine         engine(registry);

    const ChunkPos position{0, 3, 0};
    const BlockPos origin = position.originBlock();
    const BlockPos probe{origin.x + 16, origin.y + 16, origin.z + 16};

    SECTION("a chunk swapped out between reads is not served from the memo")
    {
        ChunkMap map;
        makeChunk(map, position, blocks::Stone);
        const LightWorld world = makeLightWorld(map);

        REQUIRE(world.blockAt(probe) == blocks::Stone);

        // Exactly what streaming does when a chunk is retired and a fresh one
        // takes its place, or when a LOD rebuild swaps in its shadow: same
        // position, different object. A memo that survives the read that filled
        // it keeps answering from the object that is no longer resident - and
        // keeps it alive to absorb writes nobody will ever see.
        makeChunk(map, position, blocks::Air);

        CHECK(world.blockAt(probe) == blocks::Air);
    }

    SECTION("a writability verdict is re-asked on the next pass")
    {
        ChunkMap map;
        makeChunk(map, position, blocks::Air);

        bool       writable = true;
        LightWorld world(
            [&map](const ChunkPos& at) -> ChunkPtr {
                const auto it = map.find(at);
                return it != map.end() ? it->second : nullptr;
            },
            [&writable](const ChunkPos&) { return writable; });

        const LightSeed first{probe, 0, 15};
        const LightUpdateStats before = engine.applySeeds(world, &first, 1);
        REQUIRE(before.writesRefused == 0);
        REQUIRE(blockLightAt(*map.at(position), 16, 16, 16) == 15);

        // A worker has taken the chunk since the last pass - a mesh job, or a
        // light job on its column. The verdict cached during the pass above is
        // now a licence to write into a chunk somebody else owns, which is
        // invariant 1 exactly.
        writable = false;
        world.resetCounters();

        // The chunk's own corner: 48 blocks of L1 distance from the first seed,
        // so the flood above cannot have reached it and anything found here came
        // from this pass. Same CHUNK, though, which is what puts the stale memo
        // in the way.
        const LightSeed        second{origin, 0, 15};
        const LightUpdateStats after = engine.applySeeds(world, &second, 1);

        CHECK(after.writesRefused >= 1);
        CHECK(blockLightAt(*map.at(position), 0, 0, 0) == 0);
    }

    SECTION("the memo is live inside a pass and dead outside one")
    {
        ChunkMap map;
        makeChunk(map, position, blocks::Air);
        const LightWorld world = makeLightWorld(map);

        CHECK_FALSE(world.inPass());
        {
            const LightWorld::Pass pass{world};
            CHECK(world.inPass());
            {
                const LightWorld::Pass nested{world};
                CHECK(world.inPass());
            }
            // Nesting must not close the outer pass early.
            CHECK(world.inPass());
        }
        CHECK_FALSE(world.inPass());
    }
}

TEST_CASE("a seed batch that was entirely refused reports the refusals",
          "[light][threading][regression]")
{
    const BlockRegistry registry = createDefaultBlockRegistry();
    LightEngine         engine(registry);

    ChunkMap       map;
    const ChunkPos position{0, 3, 0};
    const BlockPos origin = position.originBlock();
    makeChunk(map, position, blocks::Air);

    // Every chunk resident, none writable: a worker owns the whole neighbourhood,
    // which is the ordinary case for a spill seed - it lands in the column the
    // producer did NOT own, and that is the column most likely to still be busy.
    LightWorld world(
        [&map](const ChunkPos& at) -> ChunkPtr {
            const auto it = map.find(at);
            return it != map.end() ? it->second : nullptr;
        },
        [](const ChunkPos&) { return false; });

    const LightSeed seeds[2] = {
        LightSeed{BlockPos{origin.x + 16, origin.y + 16, origin.z + 16}, 15, 0},
        LightSeed{BlockPos{origin.x + 8, origin.y + 8, origin.z + 8}, 0, 12},
    };

    const LightUpdateStats stats = engine.applySeeds(world, seeds, 2);

    // The whole point: World::applyPendingLight keeps the batch queued for a
    // retry only when this is non-zero, and drops it otherwise. Counting the
    // refusals from inside propagate() alone missed every refusal the seeding
    // loop itself incurred, so a batch in which nothing landed reported success
    // and the light was thrown away for good.
    CHECK(stats.writesRefused == 2);
    CHECK(world.refusedWrites() == 2);
    CHECK(sunAt(*map.at(position), 16, 16, 16) == 0);
    CHECK(blockLightAt(*map.at(position), 8, 8, 8) == 0);
}

namespace {

/// Terrain that needs no generator dependency: solid to sea level, air above.
void generateFlat(Chunk& chunk)
{
    const BlockPos origin = chunk.position().originBlock();
    for (std::int32_t ly = 0; ly < kChunkSize; ++ly) {
        if (origin.y + ly > kSeaLevel) {
            break;
        }
        for (std::int32_t lz = 0; lz < kChunkSize; ++lz) {
            for (std::int32_t lx = 0; lx < kChunkSize; ++lx) {
                chunk.storage().set(lx, ly, lz, blocks::Stone);
            }
        }
    }
    chunk.storage().optimise();
}

/// A real lighter, with an optional gate the test can hold the light job open
/// on. A fresh engine per call rather than a thread_local one: the registry is
/// a test-local object and a thread_local would outlive it.
struct GatedLighter {
    const BlockRegistry* registry = nullptr;

    std::mutex              mutex;
    std::condition_variable signal;
    bool                    gated  = false;
    bool                    inJob  = false;
    bool                    opened = false;
    /// Every column a light job has entered, in arrival order. Guarded by mutex.
    std::vector<ColumnPos> entered;

    [[nodiscard]] ChunkLighter make()
    {
        ChunkLighter lighter;
        lighter.column = [this](const LightColumnWork& work) {
            waitForGate(work.column);
            LightEngine engine{*registry};
            return engine.lightColumn(work);
        };
        lighter.chunk = [this](Chunk& chunk, const ChunkNeighbourhood& around, LightSpill& spill) {
            LightEngine engine{*registry};
            engine.lightChunk(chunk, around, &spill);
        };
        lighter.spill = [](LightSpill&&) {};
        return lighter;
    }

    /// BOUNDED, and that is not paranoia. A worker parked here holds a job the
    /// ChunkManager destructor waits on and the JobSystem destructor drains, so
    /// a gate that is never opened - which is what an early REQUIRE failure
    /// produces, because Catch2 throws and the test never reaches open() - hangs
    /// the whole binary rather than failing one case.
    void waitForGate(const ColumnPos& column)
    {
        std::unique_lock<std::mutex> lock(mutex);
        entered.push_back(column);
        if (!gated) {
            return;
        }
        inJob = true;
        signal.notify_all();
        signal.wait_for(lock, std::chrono::seconds{30}, [this] { return opened; });
    }

    [[nodiscard]] bool waitUntilInJob()
    {
        std::unique_lock<std::mutex> lock(mutex);
        return signal.wait_for(lock, std::chrono::seconds{10}, [this] { return inJob; });
    }

    /// Waits until `count` light jobs have reached the gate. Bounded for the same
    /// reason waitForGate is.
    [[nodiscard]] bool waitUntilParked(std::size_t count)
    {
        std::unique_lock<std::mutex> lock(mutex);
        return signal.wait_for(lock, std::chrono::seconds{15},
                               [this, count] { return entered.size() >= count; });
    }

    /// While the gate is shut nothing ever leaves it, so "entered" and "parked
    /// right now" are the same set - which is what makes the concurrency
    /// assertion below exact rather than a sample.
    [[nodiscard]] std::vector<ColumnPos> parkedColumns()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return entered;
    }

    void open()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            opened = true;
        }
        signal.notify_all();
    }
};

/// Opens the gate on the way out however the test leaves - including through a
/// failed REQUIRE. Must be declared AFTER the ChunkManager it protects, so it
/// runs before the manager's destructor waits on the job it is holding.
struct GateOpener {
    GatedLighter& lighter;
    ~GateOpener() { lighter.open(); }
};

[[nodiscard]] StreamingConfig oneColumnConfig()
{
    StreamingConfig config;
    config.loadRadius              = 0;  // exactly one column, so the test is exact
    config.unloadPadding           = 4;
    config.verticalRadius          = kWorldSectionCount;
    config.unloadGraceFrames       = 1000;
    config.maxScheduledPerUpdate   = 4096;
    config.maxGenerateJobsInFlight = 4096;
    config.lod.enabled             = false;  // keep every chunk saveable at kLodFull
    return config;
}

const StreamingView kOriginView{glm::vec3{0.5f, static_cast<float>(kSeaLevel) + 2.0f, 0.5f},
                                glm::vec3{0.0f, 0.0f, -1.0f}};

/// Scratch directory that removes itself, as in tests/test_persistence.cpp.
class TempDir {
public:
    explicit TempDir(std::string label)
    {
        static std::atomic<std::uint32_t> counter{0};
        const auto stamp = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        m_path = std::filesystem::temp_directory_path() /
                 ("voxl_light_" + label + "_" + std::to_string(stamp) + "_" +
                  std::to_string(counter.fetch_add(1)));
        std::error_code code;
        std::filesystem::remove_all(m_path, code);
        std::filesystem::create_directories(m_path, code);
    }

    ~TempDir()
    {
        std::error_code code;
        std::filesystem::remove_all(m_path, code);
    }

    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;
    TempDir(TempDir&&)                 = delete;
    TempDir& operator=(TempDir&&)      = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

private:
    std::filesystem::path m_path;
};

}  // namespace

TEST_CASE("no light claim is taken from a generate worker", "[light][column][threading][regression]")
{
    const BlockRegistry registry = createDefaultBlockRegistry();
    GatedLighter        lighter;
    lighter.registry = &registry;

    JobSystem    jobs(2);
    ChunkManager manager(jobs, oneColumnConfig());
    manager.setGenerator(generateFlat);
    manager.setLighter(lighter.make());

    std::uint64_t frame = 0;

    // One update dispatches the whole column's terrain; the wait drains every
    // job the pool has, light jobs included.
    manager.update(kOriginView, ++frame);
    REQUIRE(manager.waitForPendingJobs(std::chrono::milliseconds{20000}));
    jobs.mainThreadQueue().drainAll();

    // THE ASSERTION. The claim entered into m_lightColumns is a marker that makes
    // World::isEditBlocked() say no, and the main thread's check-then-write is
    // only sound while the main thread is the only thing that can raise one. If a
    // generate worker started this column's light itself, the wait above would
    // have run that light job to completion and this would be non-zero.
    CHECK(manager.stats().lightColumnsLit == 0);
    CHECK(manager.stats().lightSectionsLit == 0);

    // ...and the main-thread sweep still does the work, one update later.
    manager.update(kOriginView, ++frame);
    REQUIRE(manager.waitForPendingJobs(std::chrono::milliseconds{20000}));
    jobs.mainThreadQueue().drainAll();

    CHECK(manager.stats().lightColumnsLit >= 1);
    CHECK(manager.stats().lightSectionsLit >= 1);
}

TEST_CASE("two face-adjacent columns are never lit at the same time",
          "[light][column][threading][regression]")
{
    // REGRESSION. claimColumnLight fills a job's region with visible() chunks
    // only, so a column being lit at this instant arrives as a null slot and
    // LightEngine::loadSection walls it off. That is supposed to be
    // self-correcting - whichever column is lit SECOND reads the first and
    // collectSpill spills back whatever should have crossed - but when two
    // neighbours run together neither is second. Each sees a wall, neither reads
    // the other, neither spills, and published light is never recomputed, so the
    // seam between them stays dark for good.
    //
    // It was the normal case, not a rare interleaving: sweepPendingLight starts
    // up to maxLightColumnsPerUpdate columns in one tight main-thread loop, and
    // the only claim it used to refuse was a column already being lit itself -
    // never one whose NEIGHBOUR was.
    //
    // The observable is the claim set, sampled while every job that has one is
    // parked. It is exact rather than a sample: claims are taken synchronously
    // on the main thread inside update(), and none can be released while the
    // gate is shut.
    StreamingConfig config = oneColumnConfig();
    // A disc of 13 columns. Big enough that any maximal independent set has at
    // least three members, so the refusal cannot starve the sweep down to one
    // column and make the assertion below vacuous.
    config.loadRadius               = 2;
    config.verticalRadius           = 1;  // three sections per column; the test is horizontal
    config.maxLightColumnsPerUpdate = 4096;
    config.maxLightJobsInFlight     = 4096;

    const BlockRegistry registry = createDefaultBlockRegistry();
    GatedLighter        lighter;
    lighter.registry = &registry;
    lighter.gated    = true;

    // One worker per column that could possibly be claimed, so "dispatched" and
    // "parked" cannot differ merely because the pool ran out of threads.
    JobSystem        jobs(16);
    ChunkManager     manager(jobs, config);
    const GateOpener opener{lighter};
    manager.setGenerator(generateFlat);
    manager.setLighter(lighter.make());

    std::uint64_t frame = 0;

    // Terrain for every column first. No light job can run yet - the generate
    // worker only files its column as pending - so this wait really does drain.
    manager.update(kOriginView, ++frame);
    REQUIRE(manager.waitForPendingJobs(std::chrono::milliseconds{20000}));
    jobs.mainThreadQueue().drainAll();
    REQUIRE(manager.stats().lightJobsInFlight == 0);

    // The sweep. Every claim it takes is still held when this returns.
    manager.update(kOriginView, ++frame);
    const std::size_t dispatched = manager.stats().lightJobsInFlight;

    // The situation under test really arose: more than one column went in flight
    // together, so an adjacent pair was available to be got wrong.
    REQUIRE(dispatched >= 2);
    REQUIRE(lighter.waitUntilParked(dispatched));

    const std::vector<ColumnPos> claimed = lighter.parkedColumns();
    REQUIRE(claimed.size() == dispatched);

    // THE ASSERTION. No two of them share a face.
    for (std::size_t a = 0; a < claimed.size(); ++a) {
        for (std::size_t b = a + 1; b < claimed.size(); ++b) {
            const std::int32_t dx = claimed[a].x - claimed[b].x;
            const std::int32_t dz = claimed[a].z - claimed[b].z;
            const bool faceAdjacent = (dx == 0 && (dz == 1 || dz == -1)) ||
                                      (dz == 0 && (dx == 1 || dx == -1));
            INFO("columns (" << claimed[a].x << ", " << claimed[a].z << ") and ("
                             << claimed[b].x << ", " << claimed[b].z
                             << ") were being lit at the same moment");
            CHECK_FALSE(faceAdjacent);
        }
    }

    // ...and refusing strands nobody: a held-back column stays in m_lightPending
    // - the refusal deliberately does not erase it - so later sweeps light it.
    // open() is enough to release every job, present and future: waitForGate
    // checks `opened` before it sleeps.
    lighter.open();
    for (int pass = 0; pass < 16; ++pass) {
        manager.update(kOriginView, ++frame);
        REQUIRE(manager.waitForPendingJobs(std::chrono::milliseconds{20000}));
        jobs.mainThreadQueue().drainAll();
    }

    // loadRadius 2 is a disc of 13 columns: dx*dx + dz*dz <= 4. Every one of them
    // must have been lit.
    const std::vector<ColumnPos> everyEntry = lighter.parkedColumns();
    const std::set<std::pair<std::int32_t, std::int32_t>> distinct = [&everyEntry] {
        std::set<std::pair<std::int32_t, std::int32_t>> out;
        for (const ColumnPos& column : everyEntry) {
            out.emplace(column.x, column.z);
        }
        return out;
    }();
    CHECK(distinct.size() == 13);
}

TEST_CASE("a chunk is not encoded while a light job owns its column",
          "[light][column][save][threading][regression]")
{
    const BlockRegistry registry = createDefaultBlockRegistry();
    GatedLighter        lighter;
    lighter.registry = &registry;
    lighter.gated    = true;

    JobSystem    jobs(3);
    ChunkManager manager(jobs, oneColumnConfig());
    const GateOpener opener{lighter};
    manager.setGenerator(generateFlat);
    manager.setLighter(lighter.make());

    std::uint64_t frame = 0;
    manager.update(kOriginView, ++frame);  // terrain
    REQUIRE(manager.waitForPendingJobs(std::chrono::milliseconds{20000}));
    jobs.mainThreadQueue().drainAll();

    manager.update(kOriginView, ++frame);  // the sweep claims the column and dispatches
    REQUIRE(lighter.waitUntilInJob());

    const ChunkPos target{0, blockToChunkAxis(kSeaLevel), 0};
    const ChunkPtr chunk = manager.find(target);
    REQUIRE(chunk != nullptr);
    REQUIRE(manager.isNeighbourhoodBusy(target));
    chunk->markModified();

    TempDir   directory("column_claim");
    WorldSave save(jobs, directory.path(), 0xA11CEull);
    // The wiring Application does at world open. Without it a WorldSave has no
    // way to see a worker inside a chunk, which is the whole defect.
    save.setBusyProbe(
        [&manager](const ChunkPos& position) { return manager.isNeighbourhoodBusy(position); });

    // A light job is rewriting this chunk's light array right now. encodeChunk
    // reads the whole chunk, light included, and ChunkStorage materialises that
    // array on first write - which reallocates. Lighting deliberately does not
    // put the chunk in a busy state, so ChunkState alone cannot see this.
    CHECK_FALSE(save.saveChunk(chunk));
    CHECK(chunk->needsSave());  // still dirty, so the next tick retries it

    lighter.open();
    REQUIRE(manager.waitForPendingJobs(std::chrono::milliseconds{20000}));
    jobs.mainThreadQueue().drainAll();

    // And once the worker has let go, the save goes through as it always did.
    REQUIRE_FALSE(manager.isNeighbourhoodBusy(target));
    CHECK(save.saveChunk(chunk));
    save.flush();
}

TEST_CASE("lighting a column stays well inside the streaming budget", "[light][performance]")
{
    const BlockRegistry registry = createDefaultBlockRegistry();
    LightEngine         engine(registry);

    // Two shapes, because the cost is dominated by how many sections can take the
    // uniform fast path rather than by the flood itself. `typical` is what a
    // streamed column really looks like - sky, a broken surface, solid rock -
    // and `worst` riddles the entire height with connected caves so every one of
    // the eight sections has to be flooded cell by cell.
    const std::int64_t typical = timeColumn(engine, 24);
    const std::int64_t worst   = timeColumn(engine, kWorldHeight);

    WARN("lightColumn: typical " << typical << " us (" << (typical / kWorldSectionCount)
                                 << " us/chunk), worst case " << worst << " us ("
                                 << (worst / kWorldSectionCount) << " us/chunk)");

    // Generous by an order of magnitude: a guard against an accidental
    // O(volume^2), not a benchmark. Real numbers are in the report.
    CHECK(worst < 200000);
}
