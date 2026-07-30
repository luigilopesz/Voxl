#include <catch2/catch_test_macros.hpp>

#include "world/Chunk.hpp"
#include "world/Lod.hpp"
#include "world/TerrainGenerator.hpp"
#include "world/VoxelTypes.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace voxl;

namespace {

[[nodiscard]] bool sameVoxels(const Chunk& a, const Chunk& b)
{
    for (std::size_t i = 0; i < kChunkVolume; ++i) {
        if (a.getBlock(i) != b.getBlock(i) || a.getLight(i) != b.getLight(i)) {
            return false;
        }
    }
    return true;
}

/// Blocks that make up the terrain surface a player sees from a distance.
/// Water and ice are excluded because the ocean is a flat plane whose height is
/// not part of the height field being compared.
[[nodiscard]] bool isTerrain(BlockId id) noexcept
{
    return id != blocks::Air && id != blocks::Water && id != blocks::Ice;
}

/// Highest terrain block of every column of one chunk column, generated at
/// `level`. Index is localZ * kChunkSize + localX; kWorldMinY - 1 means "no
/// terrain at all", which cannot happen because bedrock floors the world.
[[nodiscard]] std::vector<std::int32_t> terrainTopProfile(const TerrainGenerator& generator,
                                                          std::int32_t chunkX, std::int32_t chunkZ,
                                                          LodLevel level)
{
    std::vector<std::int32_t> tops(static_cast<std::size_t>(kChunkSize) * kChunkSize,
                                   kWorldMinY - 1);
    for (std::int32_t section = 0; section < kWorldSectionCount; ++section) {
        Chunk chunk(ChunkPos{chunkX, section, chunkZ});
        generator.generate(chunk, level);
        const std::int32_t baseY = section * kChunkSize;
        for (std::int32_t ly = 0; ly < kChunkSize; ++ly) {
            for (std::int32_t lz = 0; lz < kChunkSize; ++lz) {
                for (std::int32_t lx = 0; lx < kChunkSize; ++lx) {
                    if (isTerrain(chunk.getBlock(lx, ly, lz))) {
                        tops[static_cast<std::size_t>(lz) * kChunkSize +
                             static_cast<std::size_t>(lx)] = baseY + ly;
                    }
                }
            }
        }
    }
    return tops;
}

/// Wall-clock cost of generating `positions` at `level`, best of `repeats` runs.
///
/// Best-of rather than mean: this runs on shared CI hardware where a descheduled
/// thread adds arbitrary time but nothing can make the work finish early, so the
/// minimum is the stable estimator.
[[nodiscard]] double generationMicros(const TerrainGenerator& generator,
                                      const std::vector<ChunkPos>& positions, LodLevel level,
                                      int repeats)
{
    double best = 0.0;
    for (int run = 0; run < repeats; ++run) {
        const auto start = std::chrono::steady_clock::now();
        for (const ChunkPos& pos : positions) {
            Chunk chunk(pos);
            generator.generate(chunk, level);
        }
        const auto end = std::chrono::steady_clock::now();
        const double micros =
            std::chrono::duration<double, std::micro>(end - start).count() /
            static_cast<double>(positions.size());
        if (run == 0 || micros < best) {
            best = micros;
        }
    }
    return best;
}

/// Chunks that all straddle the surface: the worst case for LOD, because none of
/// them can take the empty-sky fast path and all of them pay the structure pass.
[[nodiscard]] std::vector<ChunkPos> surfaceChunks(const TerrainGenerator& generator, int count)
{
    std::vector<ChunkPos> positions;
    for (std::int32_t i = 0; positions.size() < static_cast<std::size_t>(count); ++i) {
        const std::int32_t cx = 40 + (i % 8);
        const std::int32_t cz = 7 * (i / 8);
        const std::int32_t surface =
            generator.surfaceHeight(cx * kChunkSize + kChunkSize / 2, cz * kChunkSize + kChunkSize / 2);
        positions.push_back(ChunkPos{cx, blockToChunkAxis(surface), cz});
    }
    return positions;
}

/// Whole vertical columns: what the streaming system actually asks for, sky and
/// deep sections included.
[[nodiscard]] std::vector<ChunkPos> columnChunks(int columns)
{
    std::vector<ChunkPos> positions;
    for (std::int32_t i = 0; i < columns; ++i) {
        for (std::int32_t section = 0; section < kWorldSectionCount; ++section) {
            positions.push_back(ChunkPos{40 + (i % 4), section, 11 * (i / 4)});
        }
    }
    return positions;
}

}  // namespace

// ------------------------------------------------------------- level 0 identity

TEST_CASE("level 0 generation is the full-resolution generator", "[world][terrain][lod]")
{
    const TerrainGenerator generator{};

    for (const ChunkPos& pos : {ChunkPos{0, 3, 0}, ChunkPos{-7, 2, 11}, ChunkPos{413, 4, -908}}) {
        Chunk viaOverload(pos);
        Chunk viaPlain(pos);
        generator.generate(viaOverload, kLodFull);
        generator.generate(viaPlain);
        INFO("chunk " << pos.x << ',' << pos.y << ',' << pos.z);
        CHECK(sameVoxels(viaOverload, viaPlain));
    }
}

// ------------------------------------------------------------------ determinism

TEST_CASE("generating the same chunk twice at one level is bit-identical",
          "[world][terrain][lod]")
{
    const TerrainGenerator generator{};

    for (LodLevel level = 0; level < kLodCount; ++level) {
        for (const ChunkPos& pos :
             {ChunkPos{0, 3, 0}, ChunkPos{-7, 2, 11}, ChunkPos{413, 4, -908}, ChunkPos{5, 0, 5}}) {
            Chunk first(pos);
            Chunk second(pos);
            generator.generate(first, level);
            generator.generate(second, level);
            INFO("level " << static_cast<int>(level) << " chunk " << pos.x << ',' << pos.y << ','
                          << pos.z);
            CHECK(sameVoxels(first, second));
        }
    }
}

TEST_CASE("regenerating a chunk at a different level leaves nothing of the old one",
          "[world][terrain][lod]")
{
    const TerrainGenerator generator{};
    const ChunkPos         pos{19, 3, -4};

    for (LodLevel level = 0; level < kLodCount; ++level) {
        Chunk reference(pos);
        generator.generate(reference, level);

        // Walk every other level through the same chunk object first: streaming
        // re-generates in place when a chunk changes band, so a stale voxel from
        // the previous level would show up as a mismatch here.
        Chunk reused(pos);
        for (LodLevel previous = 0; previous < kLodCount; ++previous) {
            generator.generate(reused, previous);
        }
        generator.generate(reused, level);

        INFO("level " << static_cast<int>(level));
        CHECK(sameVoxels(reference, reused));
    }
}

TEST_CASE("per-level generation is identical across threads", "[world][terrain][lod]")
{
    const TerrainGenerator generator{};

    struct Job {
        ChunkPos pos;
        LodLevel level;
    };

    std::vector<Job> jobs;
    for (LodLevel level = 0; level < kLodCount; ++level) {
        for (const ChunkPos& pos : {ChunkPos{12, 3, 34}, ChunkPos{-99, 3, 250}, ChunkPos{41, 2, 0},
                                    ChunkPos{7, 0, -7}, ChunkPos{12, 7, 34}}) {
            jobs.push_back(Job{pos, level});
        }
    }

    std::vector<std::unique_ptr<Chunk>> reference;
    reference.reserve(jobs.size());
    for (const Job& job : jobs) {
        reference.push_back(std::make_unique<Chunk>(job.pos));
        generator.generate(*reference.back(), job.level);
    }

    std::atomic<int>         mismatches{0};
    std::atomic<std::size_t> nextOffset{0};
    std::vector<std::thread> workers;
    const unsigned           threadCount = 4;

    for (unsigned t = 0; t < threadCount; ++t) {
        workers.emplace_back([&] {
            // Each worker walks the list from a different offset so the chunks
            // really are generated in a different order per thread.
            const std::size_t offset = nextOffset.fetch_add(1);
            for (std::size_t step = 0; step < jobs.size(); ++step) {
                const std::size_t i = (offset * 7 + step) % jobs.size();
                Chunk             chunk(jobs[i].pos);
                generator.generate(chunk, jobs[i].level);
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

// ------------------------------------------------------- cross-level agreement

TEST_CASE("coarse and fine levels agree on the broad height field",
          "[world][terrain][lod]")
{
    // THIS IS THE PROPERTY THE WHOLE FEATURE HANGS ON. If the coarse and fine
    // versions of a chunk disagree about where the ground is, the player watches
    // the world rearrange itself as they walk toward it.
    //
    // TOLERANCE. Two effects separate the two versions, and neither is a bug:
    //   1. Quantisation. A level-L cell is solid while its centre is at or below
    //      the surface, so the terrain top is within +/- 2^L / 2 of the true one.
    //      For level 2 that alone allows a mean absolute difference near 1.
    //   2. Sub-cell detail. The detail octave has an amplitude of up to 8 voxels
    //      and a wavelength of about 50 blocks, so the height genuinely moves a
    //      little between a cell centre and the cell's corners.
    // A mean absolute difference of 2.5 voxels at level 2 (4-block cells) leaves
    // room for both while being far tighter than the ~10 voxels a *different*
    // noise setup would produce. The maximum is checked much more loosely: a
    // single cliff edge inside one cell legitimately differs by a lot.
    TerrainSettings settings;
    settings.generateTrees = false;  // compare the height field, not the canopy
    const TerrainGenerator generator(settings);

    struct Case {
        std::int32_t chunkX;
        std::int32_t chunkZ;
    };

    double totalAbsolute = 0.0;
    double samples       = 0.0;
    std::int32_t worst   = 0;

    for (const Case& c : {Case{41, 0}, Case{40, 3}, Case{-13, 77}}) {
        const std::vector<std::int32_t> fine =
            terrainTopProfile(generator, c.chunkX, c.chunkZ, kLodFull);
        const std::vector<std::int32_t> coarse =
            terrainTopProfile(generator, c.chunkX, c.chunkZ, LodLevel{2});

        REQUIRE(fine.size() == coarse.size());
        for (std::size_t i = 0; i < fine.size(); ++i) {
            REQUIRE(fine[i] >= kWorldMinY);
            REQUIRE(coarse[i] >= kWorldMinY);
            const std::int32_t difference = std::abs(fine[i] - coarse[i]);
            totalAbsolute += static_cast<double>(difference);
            samples += 1.0;
            worst = std::max(worst, difference);
        }
    }

    const double meanAbsolute = totalAbsolute / samples;
    WARN("level 0 vs level 2 terrain top: mean |difference| = " << meanAbsolute
                                                                << " voxels, worst = " << worst);
    CHECK(meanAbsolute < 2.5);
    CHECK(worst < 24);
}

TEST_CASE("every coarse level tracks the same height field", "[world][terrain][lod]")
{
    // Each successive level may be coarser, but none may wander off somewhere
    // else: the mean error must grow roughly with the cell size, not explode.
    TerrainSettings settings;
    settings.generateTrees = false;
    const TerrainGenerator generator(settings);

    const std::vector<std::int32_t> fine = terrainTopProfile(generator, 41, 0, kLodFull);

    for (LodLevel level = 1; level < kLodCount; ++level) {
        const std::vector<std::int32_t> coarse = terrainTopProfile(generator, 41, 0, level);
        double                          total  = 0.0;
        for (std::size_t i = 0; i < fine.size(); ++i) {
            total += static_cast<double>(std::abs(fine[i] - coarse[i]));
        }
        const double mean = total / static_cast<double>(fine.size());
        WARN("level " << static_cast<int>(level) << " mean |difference| = " << mean << " voxels");
        INFO("level " << static_cast<int>(level));
        // Generous but bounded: two cells' worth of error. A generator that used
        // different or cheaper noise at coarse levels would blow straight past it.
        CHECK(mean < 2.0 * static_cast<double>(lodCellSize(level)));
    }
}

TEST_CASE("the sampled cell centres reproduce the fine height field exactly",
          "[world][terrain][lod]")
{
    // The strongest form of the consistency guarantee: at the points a coarse
    // level actually samples, it reads the identical continuous field.
    const TerrainGenerator generator{};

    for (LodLevel level = 0; level < kLodCount; ++level) {
        for (std::int32_t z = -40; z <= 40; z += 3) {
            for (std::int32_t x = -40; x <= 40; x += 3) {
                const std::int32_t sx = lodSampleCoord(x, level);
                const std::int32_t sz = lodSampleCoord(z, level);
                REQUIRE(generator.surfaceHeight(sx, sz) == generator.sampleColumn(sx, sz).surfaceY);
                // The lattice is global: every coordinate in a cell resolves to
                // the same sample point regardless of which chunk asks.
                REQUIRE(lodSampleCoord(sx, level) == sx);
            }
        }
    }
}

// ---------------------------------------------------------------------- bedrock

TEST_CASE("bedrock floors the world at every level", "[world][terrain][lod]")
{
    const TerrainGenerator generator{};

    for (LodLevel level = 0; level < kLodCount; ++level) {
        for (const ColumnPos& column :
             {ColumnPos{0, 0}, ColumnPos{-31, 64}, ColumnPos{1000, -1000}, ColumnPos{41, 0}}) {
            Chunk chunk(ChunkPos{column.x, 0, column.z});
            generator.generate(chunk, level);
            INFO("level " << static_cast<int>(level) << " column " << column.x << ',' << column.z);

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
}

// ----------------------------------------------------------------- world bounds

TEST_CASE("no level writes terrain above the world's usable ceiling", "[world][terrain][lod]")
{
    const TerrainGenerator generator{};

    for (const ColumnPos& column : {ColumnPos{41, 0}, ColumnPos{-13, 77}, ColumnPos{13, 0}}) {
        // Padded by the structure reach, because a tree rooted in a neighbouring
        // column writes into this one.
        std::int32_t columnTop = kWorldMinY;
        for (std::int32_t z = -kMaxStructureRadius; z < kChunkSize + kMaxStructureRadius; ++z) {
            for (std::int32_t x = -kMaxStructureRadius; x < kChunkSize + kMaxStructureRadius; ++x) {
                columnTop = std::max(columnTop, generator
                                                    .sampleColumn(column.x * kChunkSize + x,
                                                                  column.z * kChunkSize + z)
                                                    .structureTopY);
            }
        }

        for (LodLevel level = 0; level < kLodCount; ++level) {
            // A coarse cell may round the terrain up, and a quantised structure
            // voxel fills its whole cell, so the bound widens by one cell.
            const std::int32_t bound = columnTop + lodCellSize(level);
            REQUIRE(bound <= kWorldMaxY);

            for (std::int32_t section = 0; section < kWorldSectionCount; ++section) {
                Chunk chunk(ChunkPos{column.x, section, column.z});
                generator.generate(chunk, level);
                const std::int32_t sectionMinY = section * kChunkSize;
                for (std::int32_t ly = 0; ly < kChunkSize; ++ly) {
                    const std::int32_t worldY = sectionMinY + ly;
                    if (worldY <= bound || worldY <= kSeaLevel) {
                        continue;
                    }
                    for (std::int32_t z = 0; z < kChunkSize; ++z) {
                        for (std::int32_t x = 0; x < kChunkSize; ++x) {
                            INFO("level " << static_cast<int>(level) << " y=" << worldY
                                          << " bound=" << bound);
                            REQUIRE(chunk.getBlock(x, ly, z) == blocks::Air);
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("no level puts water above sea level", "[world][terrain][lod]")
{
    const TerrainGenerator generator{};

    for (LodLevel level = 0; level < kLodCount; ++level) {
        for (std::int32_t cy = 0; cy < kWorldSectionCount; ++cy) {
            Chunk chunk(ChunkPos{3, cy, 3});
            generator.generate(chunk, level);
            for (std::int32_t ly = 0; ly < kChunkSize; ++ly) {
                const std::int32_t worldY = cy * kChunkSize + ly;
                if (worldY <= kSeaLevel) {
                    continue;
                }
                for (std::int32_t z = 0; z < kChunkSize; ++z) {
                    for (std::int32_t x = 0; x < kChunkSize; ++x) {
                        const BlockId id = chunk.getBlock(x, ly, z);
                        INFO("level " << static_cast<int>(level) << " y=" << worldY);
                        REQUIRE(id != blocks::Water);
                        REQUIRE(id != blocks::Ice);
                    }
                }
            }
        }
    }
}

// ------------------------------------------------------------------------ caves

TEST_CASE("coarse caves never remove the surface cell", "[world][terrain][lod]")
{
    // Sampling the carve field at cell centres is only safe because the depth
    // fades are evaluated at the SHALLOWEST block of the cell. Feed them the
    // centre instead and a level-3 cell, whose centre sits four blocks under the
    // surface, carves the surface away and opens a hole in a distant hillside.
    //
    // The check is exact, not approximate: with the guard in place the topmost
    // terrain block of every column must be precisely the quantised terrain top
    // of the cell it belongs to, i.e. no cave touched the surface cell anywhere.
    TerrainSettings settings;
    settings.generateTrees = false;  // a canopy would sit above the terrain top
    const TerrainGenerator generator(settings);

    for (LodLevel level = 1; level < kLodCount; ++level) {
        for (std::int32_t cz = 0; cz < 2; ++cz) {
            for (std::int32_t cx = 40; cx < 42; ++cx) {
                const std::vector<std::int32_t> tops = terrainTopProfile(generator, cx, cz, level);

                for (std::int32_t lz = 0; lz < kChunkSize; ++lz) {
                    for (std::int32_t lx = 0; lx < kChunkSize; ++lx) {
                        const std::int32_t worldX = cx * kChunkSize + lx;
                        const std::int32_t worldZ = cz * kChunkSize + lz;
                        const std::int32_t sampleX = lodSampleCoord(worldX, level);
                        const std::int32_t sampleZ = lodSampleCoord(worldZ, level);
                        // max() covers the degenerate case of a column whose
                        // surface is inside the bedrock cell.
                        const std::int32_t expected =
                            std::max(lodTerrainTop(generator.surfaceHeight(sampleX, sampleZ), level),
                                     lodCellSize(level) - 1);
                        INFO("level " << static_cast<int>(level) << " column (" << worldX << ','
                                      << worldZ << ')');
                        REQUIRE(tops[static_cast<std::size_t>(lz) * kChunkSize +
                                     static_cast<std::size_t>(lx)] == expected);
                    }
                }
            }
        }
    }
}

TEST_CASE("caves survive coarse sampling without eating the world",
          "[world][terrain][lod]")
{
    // Coarse sampling is NOT a strict subset of the fine carve: a cavern hit at
    // one cell centre inflates to the whole cell, which can carve slightly more
    // volume than the blocks under it would individually. What must hold is that
    // caves still exist and are still a small fraction of the deep world - the
    // same band the full-resolution test asserts.
    const TerrainGenerator generator{};

    for (LodLevel level = 0; level < kLodCount; ++level) {
        std::int64_t hollow = 0;
        std::int64_t total  = 0;
        for (std::int32_t cz = 0; cz < 2; ++cz) {
            for (std::int32_t cx = 40; cx < 42; ++cx) {
                Chunk chunk(ChunkPos{cx, 1, cz});  // y 32..63, comfortably underground
                generator.generate(chunk, level);
                for (std::size_t i = 0; i < kChunkVolume; ++i) {
                    if (chunk.getBlock(i) == blocks::Air) {
                        ++hollow;
                    }
                    ++total;
                }
            }
        }
        const double fraction = static_cast<double>(hollow) / static_cast<double>(total);
        WARN("level " << static_cast<int>(level) << " carved fraction " << fraction);
        INFO("level " << static_cast<int>(level));
        CHECK(fraction > 0.005);
        CHECK(fraction < 0.30);
    }
}

// ------------------------------------------------------------------ decorations

TEST_CASE("trees stop at the documented level", "[world][terrain][lod]")
{
    const TerrainGenerator generator{};

    // A forest column, found by probing rather than hard-coded, so the test
    // survives a change to the noise constants.
    ColumnPos forest{0, 0};
    bool      found = false;
    for (std::int32_t cz = 0; cz < 96 && !found; ++cz) {
        for (std::int32_t cx = 0; cx < 96 && !found; ++cx) {
            bool all = true;
            for (std::int32_t dz = 0; dz <= kChunkSize && all; dz += kChunkSize / 2) {
                for (std::int32_t dx = 0; dx <= kChunkSize && all; dx += kChunkSize / 2) {
                    all = generator.biomeAt(cx * kChunkSize + dx, cz * kChunkSize + dz) ==
                          BiomeId::Forest;
                }
            }
            if (all) {
                forest = ColumnPos{cx, cz};
                found  = true;
            }
        }
    }
    REQUIRE(found);

    std::array<int, kLodCount> woodVoxels{};
    for (LodLevel level = 0; level < kLodCount; ++level) {
        for (std::int32_t section = 0; section < kWorldSectionCount; ++section) {
            Chunk chunk(ChunkPos{forest.x, section, forest.z});
            generator.generate(chunk, level);
            for (std::size_t i = 0; i < kChunkVolume; ++i) {
                const BlockId id = chunk.getBlock(i);
                if (id == blocks::Wood || id == blocks::Leaves) {
                    ++woodVoxels[level];
                }
            }
        }
    }

    for (LodLevel level = 0; level < kLodCount; ++level) {
        INFO("level " << static_cast<int>(level) << " has " << woodVoxels[level]
                      << " tree voxels");
        if (lodPlacesTrees(level)) {
            CHECK(woodVoxels[level] > 0);
        } else {
            CHECK(woodVoxels[level] == 0);
        }
    }
}

TEST_CASE("tree generation can still be switched off at coarse levels",
          "[world][terrain][lod]")
{
    TerrainSettings settings;
    settings.generateTrees = false;
    const TerrainGenerator generator(settings);

    for (LodLevel level = 0; level < kLodCount; ++level) {
        for (std::int32_t section = 0; section < kWorldSectionCount; ++section) {
            Chunk chunk(ChunkPos{41, section, 0});
            generator.generate(chunk, level);
            for (std::size_t i = 0; i < kChunkVolume; ++i) {
                const BlockId id = chunk.getBlock(i);
                REQUIRE(id != blocks::Wood);
                REQUIRE(id != blocks::Leaves);
            }
        }
    }
}

// ------------------------------------------------------------------------- cost

TEST_CASE("generation cost falls sharply with the level", "[world][terrain][lod]")
{
    const TerrainGenerator generator{};

    struct Workload {
        const char*           name;
        std::vector<ChunkPos> positions;
    };

    const std::array<Workload, 2> workloads{
        Workload{"surface-only", surfaceChunks(generator, 24)},
        Workload{"whole-column", columnChunks(12)},
    };

    for (const Workload& workload : workloads) {
        REQUIRE(!workload.positions.empty());

        // Warm the noise tables and the allocator before anything is timed.
        for (const ChunkPos& pos : workload.positions) {
            Chunk chunk(pos);
            generator.generate(chunk, kLodFull);
        }

        std::array<double, kLodCount> micros{};
        for (LodLevel level = 0; level < kLodCount; ++level) {
            micros[level] = generationMicros(generator, workload.positions, level, 5);
        }

        std::string report;
        for (LodLevel level = 0; level < kLodCount; ++level) {
            report += "L" + std::to_string(static_cast<int>(level)) + "=" +
                      std::to_string(micros[level]) + "us  ";
        }
        WARN(workload.name << " per-chunk generation cost: " << report);

        for (LodLevel level = 1; level < kLodCount; ++level) {
            INFO(workload.name << " level " << static_cast<int>(level) << " cost " << micros[level]
                               << "us vs level " << static_cast<int>(level - 1) << " cost "
                               << micros[level - 1] << "us");
            CHECK(micros[level] < micros[level - 1]);
        }

        // The headline claim: a level-3 chunk is a different order of magnitude,
        // not a few percent cheaper. The bound is deliberately loose against the
        // measured ratio (about 0.10 for the surface-only worst case and 0.04
        // for whole columns) so a slow or contended CI box cannot flake it.
        INFO(workload.name << " level 3 cost " << micros[kLodMax] << "us vs level 0 cost "
                           << micros[kLodFull] << "us");
        CHECK(micros[kLodMax] < micros[kLodFull] * 0.25);
    }
}
