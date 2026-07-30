// Terrain generation: per chunk across biomes, across LOD levels, and as a
// throughput test through the job system.

#include "Cases.hpp"
#include "Fixtures.hpp"

#include "core/JobSystem.hpp"
#include "world/Chunk.hpp"
#include "world/ChunkStorage.hpp"
#include "world/Lod.hpp"
#include "world/TerrainGenerator.hpp"
#include "world/VoxelTypes.hpp"

#include <array>
#include <cstddef>
#include <format>
#include <memory>
#include <string>
#include <vector>

namespace voxl::bench {

namespace {

constexpr const char* kGroup = "terrain";

/// Section straddling sea level. Chosen for the batch and LOD cases because it
/// is the only band that contains all three of solid terrain, water and air; a
/// section picked deeper is nearly uniform stone and generates far faster than
/// anything the streaming system actually spends its time on.
constexpr std::int32_t kBatchSectionY = kSeaLevel / kChunkSize;

/// 16 x 16 chunk columns. Large enough that the job system's wake-up and
/// steal costs are amortised the way they are during real streaming, small
/// enough that a single-threaded run of the same set stays under a second.
constexpr std::int32_t kBatchSide  = 16;
constexpr std::size_t  kBatchCount = static_cast<std::size_t>(kBatchSide) * kBatchSide;

/// One chunk, regenerated in place every run.
///
/// TerrainGenerator::generate is documented as idempotent - it resets the voxels
/// and light first - so reusing the allocation measures generation rather than
/// the allocator, and the palette starts each run already grown to its final
/// width exactly as it would on a chunk being re-generated at a new LOD level.
struct SingleChunkFixture {
    ChunkPtr chunk;
    LodLevel level = kLodFull;
};

void addPerBiomeCase(Runner& runner, BiomeId biome)
{
    const std::string name = std::format("gen_chunk_{}", toString(biome));
    if (!runner.selected(kGroup, name)) {
        return;
    }

    const BiomeSite site = findBiomeSite(biome);
    if (!site.found) {
        runner.addUnavailable(kGroup, name,
                              std::format("seed {:#x} has no {} column within the scan radius",
                                          seed(), toString(biome)));
        return;
    }

    auto fixture = std::make_shared<SingleChunkFixture>();

    Case testCase;
    testCase.group     = kGroup;
    testCase.name      = name;
    testCase.unit      = "chunk";
    testCase.opsPerRun = 1.0;
    testCase.setup     = [fixture, site](CaseContext& context) {
        fixture->chunk = Chunk::create(site.surfaceSection);
        generator().generate(*fixture->chunk, kLodFull);
        context.counter("surface_y", static_cast<double>(site.surfaceY), "block");
        context.counter("chunk_y_section", static_cast<double>(site.surfaceSection.y));
        context.counter("non_air_blocks", static_cast<double>(countNonAir(*fixture->chunk)),
                            "blocks");
        context.counter("palette_entries",
                            static_cast<double>(fixture->chunk->storage().paletteSize()));
        context.counter("bits_per_index",
                            static_cast<double>(fixture->chunk->storage().bitsPerIndex()));
        context.counter("storage_heap", static_cast<double>(fixture->chunk->storage().heapBytes()),
                            "bytes");
    };
    testCase.body = [fixture](CaseContext&) {
        generator().generate(*fixture->chunk, kLodFull);
        keep(static_cast<std::uint64_t>(fixture->chunk->storage().paletteSize()));
    };
    runner.add(std::move(testCase));
}

void addLodCase(Runner& runner, LodLevel level)
{
    const std::string name = std::format("gen_lod{}", static_cast<int>(level));
    if (!runner.selected(kGroup, name)) {
        return;
    }

    auto fixture   = std::make_shared<SingleChunkFixture>();
    fixture->level = level;

    Case testCase;
    testCase.group     = kGroup;
    testCase.name      = name;
    testCase.unit      = "chunk";
    testCase.opsPerRun = 1.0;
    testCase.setup     = [fixture](CaseContext& context) {
        fixture->chunk = Chunk::create(ChunkPos{0, kBatchSectionY, 0});
        fixture->chunk->setLod(fixture->level);
        generator().generate(*fixture->chunk, fixture->level);
        context.counter("cells_per_chunk", static_cast<double>(lodCellCount(fixture->level)));
        context.counter("cell_size", static_cast<double>(lodCellSize(fixture->level)), "blocks");
        context.counter("non_air_blocks", static_cast<double>(countNonAir(*fixture->chunk)),
                            "blocks");
    };
    testCase.body = [fixture](CaseContext&) {
        generator().generate(*fixture->chunk, fixture->level);
        keep(static_cast<std::uint64_t>(fixture->chunk->storage().paletteSize()));
    };
    runner.add(std::move(testCase));
}

/// Shared by the single-threaded and job-system batch cases so both regenerate
/// byte-identical chunks and the speedup is a like-for-like comparison.
struct BatchFixture {
    std::vector<ChunkPtr>      chunks;
    std::unique_ptr<JobSystem> jobs;

    void allocate()
    {
        chunks.clear();
        chunks.reserve(kBatchCount);
        for (std::int32_t cz = 0; cz < kBatchSide; ++cz) {
            for (std::int32_t cx = 0; cx < kBatchSide; ++cx) {
                chunks.push_back(Chunk::create(ChunkPos{cx, kBatchSectionY, cz}));
            }
        }
    }
};

void addBatchCases(Runner& runner)
{
    auto fixture = std::make_shared<BatchFixture>();

    if (runner.selected(kGroup, "gen_batch_single_thread")) {
        Case single;
        single.group      = kGroup;
        single.name       = "gen_batch_single_thread";
        single.unit       = "chunk";
        single.opsPerRun  = static_cast<double>(kBatchCount);
        single.warmupRuns = 2;
        single.sampleRuns = 7;
        single.setup      = [fixture](CaseContext& context) {
            fixture->allocate();
            context.counter("chunks_per_run", static_cast<double>(kBatchCount), "chunks");
        };
        single.body = [fixture](CaseContext&) {
            for (const ChunkPtr& chunk : fixture->chunks) {
                generator().generate(*chunk, kLodFull);
            }
            keep(static_cast<std::uint64_t>(fixture->chunks.size()));
        };
        runner.add(std::move(single));
    }

    if (runner.selected(kGroup, "gen_batch_job_system")) {
        Case parallel;
        parallel.group      = kGroup;
        parallel.name       = "gen_batch_job_system";
        parallel.unit       = "chunk";
        parallel.opsPerRun  = static_cast<double>(kBatchCount);
        parallel.warmupRuns = 2;
        parallel.sampleRuns = 7;
        parallel.setup      = [fixture](CaseContext& context) {
            if (fixture->chunks.empty()) {
                fixture->allocate();
            }
            // Default thread count: hardware_concurrency() - 1, leaving the
            // main thread a core, which is exactly what the game runs with.
            fixture->jobs = std::make_unique<JobSystem>();
            context.counter("worker_threads", static_cast<double>(fixture->jobs->workerCount()));
            context.counter("chunks_per_run", static_cast<double>(kBatchCount), "chunks");
            context.note("one job per chunk, JobPriority::Normal, timed to waitIdle()");
        };
        parallel.body = [fixture](CaseContext&) {
            for (const ChunkPtr& chunk : fixture->chunks) {
                Chunk* raw = chunk.get();
                fixture->jobs->submitDetached(JobPriority::Normal,
                                              [raw] { generator().generate(*raw, kLodFull); });
            }
            fixture->jobs->waitIdle();
            keep(static_cast<std::uint64_t>(fixture->chunks.size()));
        };
        runner.add(std::move(parallel));
    }
}

}  // namespace

void registerTerrainCases(Runner& runner)
{
    for (std::size_t i = 0; i < kBiomeCount; ++i) {
        addPerBiomeCase(runner, static_cast<BiomeId>(i));
    }
    for (LodLevel level = 0; level < kLodCount; ++level) {
        addLodCase(runner, level);
    }
    addBatchCases(runner);
}

void reportTerrainDerived(Runner& runner)
{
    // ---- LOD cost ratios ----
    const CaseResult* level0 = runner.find(kGroup, "gen_lod0");
    for (LodLevel level = 1; level < kLodCount; ++level) {
        const CaseResult* coarse = runner.find(kGroup, std::format("gen_lod{}", static_cast<int>(level)));
        if (level0 == nullptr || coarse == nullptr || coarse->stats.median <= 0.0) {
            continue;
        }
        const double ratio = level0->stats.median / coarse->stats.median;

        // A level-L chunk decides (32 >> L)^3 cells instead of 32^3 blocks, so
        // the work drops by 8^L before any fixed per-chunk cost. Anything under
        // 4x at level 3 means the coarse path is dominated by something that
        // does not scale with the cell count - the column pre-pass, the
        // allocation, the light seed - and LOD is not buying what it claims to.
        const double expected = level == 3 ? 4.0 : (level == 2 ? 2.5 : 1.5);
        const std::string verdict =
            ratio >= expected
                ? "ok"
                : std::format("*** FLAG: expected >= {:.1f}x cheaper than level 0", expected);

        runner.addDerived(Derived{std::string(kGroup),
                                  std::format("gen_lod{}", static_cast<int>(level)),
                                  "speedup_vs_lod0", ratio, "x", verdict});
    }

    // ---- job-system scaling ----
    const CaseResult* single   = runner.find(kGroup, "gen_batch_single_thread");
    const CaseResult* parallel = runner.find(kGroup, "gen_batch_job_system");
    if (single != nullptr && parallel != nullptr && parallel->stats.median > 0.0) {
        const double speedup = single->stats.median / parallel->stats.median;
        runner.addDerived(Derived{std::string(kGroup), "gen_batch_job_system", "speedup_vs_1_thread",
                                  speedup, "x", ""});

        double workers = 0.0;
        for (const Counter& counter : parallel->counters) {
            if (counter.name == "worker_threads") {
                workers = counter.value;
            }
        }
        if (workers > 0.0) {
            runner.addDerived(Derived{std::string(kGroup), "gen_batch_job_system",
                                      "parallel_efficiency", 100.0 * speedup / workers, "%", ""});
        }
    }
}

}  // namespace voxl::bench
