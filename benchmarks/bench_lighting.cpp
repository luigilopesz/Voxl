// Light propagation: the two worker-side rebuilds and the main-thread edit path.
//
// ===========================================================================
//  THIS CASE TARGETS A MODULE THAT WAS WRITTEN IN PARALLEL.
// ===========================================================================
//  src/world/LightEngine.hpp does not exist in every tree this file has to
//  compile in, so the include sits behind __has_include and the whole group
//  degrades to a loud "NOT MEASURED" row rather than a build break. Defining
//  VOXL_BENCH_NO_LIGHT_ENGINE compiles the group out even when the header is
//  present, which is what benchmarks/CMakeLists.txt does automatically while
//  LightEngine.cpp is not yet part of voxl::engine - that combination compiles
//  and then fails to LINK.
//
// WHAT IS MEASURED, AND WHY THESE THREE
// -------------------------------------
//  * light_column      - the streaming path. A whole 8-section column is one
//                        job, because sunlight is vertical and lighting sections
//                        independently would impose a top-down ordering (see the
//                        comment on LightColumnWork). This is the number that
//                        governs how fast a world can stream in.
//  * light_chunk       - the LOD-shadow path: one section from a 3x3x3 snapshot.
//                        Costs less than a column but is dispatched far more
//                        often, once per chunk that changes level.
//  * edit_block_relight- the only one with a frame budget. A player breaking a
//                        sunlit surface block drops a shaft of sky light and
//                        re-floods it, on the MAIN THREAD, inside the frame that
//                        registered the click.
//
// Each run starts from cleared light, untimed, so an engine that early-outs on
// already-correct light cannot report the cost of the early-out.

#include "Cases.hpp"
#include "Fixtures.hpp"

#include "core/Log.hpp"
#include "world/Block.hpp"
#include "world/BlockAccess.hpp"
#include "world/Chunk.hpp"
#include "world/ChunkStorage.hpp"
#include "world/VoxelTypes.hpp"

#if __has_include("world/LightEngine.hpp") && !defined(VOXL_BENCH_NO_LIGHT_ENGINE)
    #define VOXL_BENCH_HAS_LIGHT_ENGINE 1
    #include "world/LightEngine.hpp"
#else
    #define VOXL_BENCH_HAS_LIGHT_ENGINE 0
#endif

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace voxl::bench {

namespace {

constexpr const char* kGroup = "lighting";

#if VOXL_BENCH_HAS_LIGHT_ENGINE

/// Sunlit or block-lit voxels, so the report can state that the pass produced
/// light rather than merely returning.
[[nodiscard]] std::size_t countLitVoxels(const Chunk& chunk)
{
    std::size_t lit = 0;
    for (std::size_t index = 0; index < kChunkVolume; ++index) {
        lit += chunk.getLight(index) != 0 ? 1u : 0u;
    }
    return lit;
}

// ------------------------------------------------------------- light_chunk --

struct ChunkFixture {
    Scene                        scene;
    std::unique_ptr<LightEngine> engine;
    LightSpill                   spill;
};

void addChunkCase(Runner& runner)
{
    const std::string name = "light_chunk";
    if (!runner.selected(kGroup, name)) {
        return;
    }

    auto fixture = std::make_shared<ChunkFixture>();

    Case testCase;
    testCase.group      = kGroup;
    testCase.name       = name;
    testCase.unit       = "chunk";
    testCase.opsPerRun  = 1.0;
    testCase.sampleRuns = 15;
    testCase.setup      = [fixture](CaseContext& context) {
        fixture->engine = std::make_unique<LightEngine>(registry());
        // A section straddling sea level: partly open to the sky, partly buried.
        // The only shape in which both the sky sweep and the sideways flood do
        // real work.
        fixture->scene = generatedScene(ChunkPos{0, kSeaLevel / kChunkSize, 0}, kLodFull);

        fixture->scene.centre->fillLight(0, 0);
        fixture->spill.clear();
        fixture->engine->lightChunk(*fixture->scene.centre, fixture->scene.neighbourhood,
                                        &fixture->spill);

        context.counter("non_air_blocks", static_cast<double>(countNonAir(*fixture->scene.centre)),
                            "blocks");
        context.counter("lit_voxels", static_cast<double>(countLitVoxels(*fixture->scene.centre)),
                            "voxels");
        context.counter("spill_seeds", static_cast<double>(fixture->spill.size()), "seeds");
        context.note("worker path: one section from a 3x3x3 snapshot, light zeroed before "
                          "every run");
    };
    testCase.prepare = [fixture](CaseContext&) {
        fixture->scene.centre->fillLight(0, 0);
        fixture->spill.clear();
    };
    testCase.body = [fixture](CaseContext&) {
        fixture->engine->lightChunk(*fixture->scene.centre, fixture->scene.neighbourhood,
                                    &fixture->spill);
        keep(static_cast<std::uint64_t>(fixture->spill.size()));
    };
    runner.add(std::move(testCase));
}

// ------------------------------------------------------------ light_column --

struct ColumnFixture {
    LightColumnWork              work;
    std::unique_ptr<LightEngine> engine;
    LightSpill                   spill;
};

/// Builds the 3x3 columns x 8 sections region a column job reads, with the
/// centre column as the target.
void buildColumnWork(ColumnFixture& fixture, const ColumnPos& column)
{
    fixture.work = LightColumnWork{};
    fixture.work.column = column;

    for (std::int32_t dz = -1; dz <= 1; ++dz) {
        for (std::int32_t dx = -1; dx <= 1; ++dx) {
            for (std::int32_t sectionY = 0; sectionY < kWorldSectionCount; ++sectionY) {
                ChunkPtr chunk = generateChunk(
                    ChunkPos{column.x + dx, sectionY, column.z + dz}, kLodFull);
                if (dx == 0 && dz == 0) {
                    fixture.work.targets[static_cast<std::size_t>(sectionY)] = chunk;
                }
                fixture.work.region[LightColumnWork::regionIndex(dx, dz, sectionY)] = chunk;
            }
        }
    }
}

void addColumnCase(Runner& runner)
{
    const std::string name = "light_column";
    if (!runner.selected(kGroup, name)) {
        return;
    }

    auto fixture = std::make_shared<ColumnFixture>();

    Case testCase;
    testCase.group     = kGroup;
    testCase.name      = name;
    testCase.unit      = "section";
    testCase.opsPerRun = static_cast<double>(kWorldSectionCount);
    testCase.warmupRuns = 2;
    testCase.sampleRuns = 11;
    testCase.setup     = [fixture](CaseContext& context) {
        fixture->engine = std::make_unique<LightEngine>(registry());
        buildColumnWork(*fixture, ColumnPos{0, 0});

        fixture->spill = fixture->engine->lightColumn(fixture->work);

        std::size_t lit   = 0;
        std::size_t solid = 0;
        for (const ChunkPtr& target : fixture->work.targets) {
            if (target != nullptr) {
                lit += countLitVoxels(*target);
                solid += countNonAir(*target);
            }
        }
        context.counter("sections_per_run", static_cast<double>(kWorldSectionCount), "sections");
        context.counter("non_air_blocks", static_cast<double>(solid), "blocks");
        context.counter("lit_voxels", static_cast<double>(lit), "voxels");
        context.counter("spill_seeds", static_cast<double>(fixture->spill.size()), "seeds");
        context.note("streaming path: the whole 8-section column as one job, light zeroed "
                          "before every run");
    };
    testCase.prepare = [fixture](CaseContext&) {
        for (const ChunkPtr& target : fixture->work.targets) {
            if (target != nullptr) {
                target->fillLight(0, 0);
            }
        }
    };
    testCase.body = [fixture](CaseContext&) {
        fixture->spill = fixture->engine->lightColumn(fixture->work);
        keep(static_cast<std::uint64_t>(fixture->spill.size()));
    };
    runner.add(std::move(testCase));
}

// ------------------------------------------------------ edit_block_relight --

struct EditFixture {
    std::unordered_map<ChunkPos, ChunkPtr> chunks;
    std::unique_ptr<LightEngine>           engine;
    std::unique_ptr<LightWorld>            world;
    BlockPos                               target{};
    BlockId                                material = blocks::Stone;
};

void addEditCase(Runner& runner)
{
    const std::string name = "edit_block_relight";
    if (!runner.selected(kGroup, name)) {
        return;
    }

    auto fixture = std::make_shared<EditFixture>();

    Case testCase;
    testCase.group      = kGroup;
    testCase.name       = name;
    testCase.unit       = "edit";
    testCase.opsPerRun  = 1.0;
    testCase.sampleRuns = 31;
    testCase.setup      = [fixture](CaseContext& context) {
        fixture->engine = std::make_unique<LightEngine>(registry());

        // A 3x3 of full columns, so the flood has somewhere real to go in every
        // horizontal direction and is not clipped by a missing chunk.
        for (std::int32_t dz = -1; dz <= 1; ++dz) {
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                for (std::int32_t sectionY = 0; sectionY < kWorldSectionCount; ++sectionY) {
                    const ChunkPos position{dx, sectionY, dz};
                    fixture->chunks.emplace(position, generateChunk(position, kLodFull));
                }
            }
        }

        auto lookup = [fixture](const ChunkPos& position) -> ChunkPtr {
            const auto it = fixture->chunks.find(position);
            return it == fixture->chunks.end() ? nullptr : it->second;
        };
        // Nothing else is running: every resident chunk is writable, which is
        // what World::isEditBlocked reports for an idle streaming system too.
        auto writable = [](const ChunkPos&) { return true; };
        fixture->world = std::make_unique<LightWorld>(lookup, writable);

        // Light the whole 3x3 region first: an incremental update measured
        // against unlit chunks would be measuring the initial flood.
        for (std::int32_t dz = -1; dz <= 1; ++dz) {
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                ColumnFixture column;
                column.engine = std::make_unique<LightEngine>(registry());
                column.work.column = ColumnPos{dx, dz};
                for (std::int32_t rz = -1; rz <= 1; ++rz) {
                    for (std::int32_t rx = -1; rx <= 1; ++rx) {
                        for (std::int32_t sectionY = 0; sectionY < kWorldSectionCount;
                             ++sectionY) {
                            const ChunkPos position{dx + rx, sectionY, dz + rz};
                            const auto     it = fixture->chunks.find(position);
                            if (it == fixture->chunks.end()) {
                                continue;
                            }
                            if (rx == 0 && rz == 0) {
                                column.work.targets[static_cast<std::size_t>(sectionY)] =
                                    it->second;
                            }
                            column.work.region[LightColumnWork::regionIndex(rx, rz, sectionY)] =
                                it->second;
                        }
                    }
                }
                (void)column.engine->lightColumn(column.work);
            }
        }

        // The topmost solid block of the centre column: breaking it opens a
        // sunlit shaft, which is the expensive half of the sunlight channel.
        const std::int32_t worldX  = kChunkSize / 2;
        const std::int32_t worldZ  = kChunkSize / 2;
        const std::int32_t surface = generator().surfaceHeight(worldX, worldZ);
        fixture->target            = BlockPos{worldX, surface, worldZ};

        const ChunkPos owner = toChunkPos(fixture->target);
        const auto     it    = fixture->chunks.find(owner);
        VOXL_CHECK(it != fixture->chunks.end(), "surface block is outside the built region");
        const BlockPos local = toLocalPos(fixture->target);
        fixture->material    = it->second->getBlock(local);

        context.counter("surface_y", static_cast<double>(surface), "block");
        context.counter("chunks_resident", static_cast<double>(fixture->chunks.size()), "chunks");
        context.note("main-thread path: break one sunlit surface block, then "
                          "LightEngine::voxelChanged; the block is restored and re-lit "
                          "untimed between runs");
    };
    // Restore the block and converge the light again, untimed, so every timed
    // run starts from the same fully lit world.
    testCase.prepare = [fixture](CaseContext&) {
        const ChunkPos owner = toChunkPos(fixture->target);
        const auto     it    = fixture->chunks.find(owner);
        if (it == fixture->chunks.end()) {
            return;
        }
        const BlockPos local = toLocalPos(fixture->target);
        it->second->setBlock(local, fixture->material);
        (void)fixture->engine->voxelChanged(*fixture->world, fixture->target);
        fixture->world->clearTouched();
        fixture->world->resetCounters();
    };
    testCase.body = [fixture](CaseContext&) {
        const ChunkPos owner = toChunkPos(fixture->target);
        const auto     it    = fixture->chunks.find(owner);
        const BlockPos local = toLocalPos(fixture->target);
        it->second->setBlock(local, blocks::Air);
        const LightUpdateStats stats = fixture->engine->voxelChanged(*fixture->world,
                                                                     fixture->target);
        keep(static_cast<std::uint64_t>(stats.cellsPropagated + stats.cellsCleared));
    };
    runner.add(std::move(testCase));
}

#endif  // VOXL_BENCH_HAS_LIGHT_ENGINE

}  // namespace

#if VOXL_BENCH_HAS_LIGHT_ENGINE

void registerLightingCases(Runner& runner)
{
    addColumnCase(runner);
    addChunkCase(runner);
    addEditCase(runner);
}

#else

void registerLightingCases(Runner& runner)
{
    runner.addUnavailable(kGroup, "light_column",
                          "world/LightEngine.hpp is unavailable in this build (missing header, or "
                          "VOXL_BENCH_NO_LIGHT_ENGINE was defined because LightEngine.cpp is not "
                          "yet linked into voxl::engine); see benchmarks/bench_lighting.cpp");
}

#endif  // VOXL_BENCH_HAS_LIGHT_ENGINE

}  // namespace voxl::bench
