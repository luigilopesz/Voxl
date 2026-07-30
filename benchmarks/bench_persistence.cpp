// Chunk serialisation round-trip.
//
// ===========================================================================
//  THIS CASE TARGETS A MODULE THAT IS BEING WRITTEN IN PARALLEL.
// ===========================================================================
//  src/world/WorldSave.hpp does not exist in every tree this file has to
//  compile in, so the include sits behind __has_include and the whole group
//  degrades to a loud "NOT MEASURED" row rather than a build break.
//
//  Only the CODEC is exercised - `WorldSave::encodeChunk` and
//  `WorldSave::decodeChunk`, both static. That is deliberate: they are the pure
//  CPU half, they need no JobSystem, no directory and no disk, and they are the
//  half that runs on the main thread inside the frame (see the threading note at
//  the top of WorldSave.hpp). Region I/O is a disk benchmark, which measures the
//  machine's SSD rather than this engine, and would make the numbers here
//  incomparable between machines.
//
//  Define VOXL_BENCH_NO_WORLD_SAVE (or -DVOXL_BENCH_NO_WORLD_SAVE=ON at
//  configure time) to compile the group out - needed while WorldSave.hpp is in
//  the tree but WorldSave.cpp is not yet wired into src/CMakeLists.txt, because
//  that combination compiles and then fails to LINK.

#include "Cases.hpp"
#include "Fixtures.hpp"

#include "world/Chunk.hpp"
#include "world/ChunkStorage.hpp"
#include "world/SubVoxel.hpp"
#include "world/VoxelTypes.hpp"

#if __has_include("world/WorldSave.hpp") && !defined(VOXL_BENCH_NO_WORLD_SAVE)
    #define VOXL_BENCH_HAS_WORLD_SAVE 1
    #include "world/WorldSave.hpp"
#else
    #define VOXL_BENCH_HAS_WORLD_SAVE 0
#endif

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace voxl::bench {

namespace {

constexpr const char* kGroup = "persistence";

#if VOXL_BENCH_HAS_WORLD_SAVE

/// A dense section: 2 bytes of block id plus 1 byte of packed light per voxel.
/// Every payload size is reported against this rather than against 64 KB alone,
/// because the light array is a real part of what has to be stored.
constexpr double kRawSectionBytes = static_cast<double>(kChunkVolume) * 3.0;

struct SaveFixture {
    ChunkPtr               source;
    ChunkPtr               destination;
    std::vector<std::byte> payload;
};

/// How the three fixture flavours differ. The codec's cost is dominated by which
/// of its three representations the chunk is in - uniform, paletted, paletted
/// plus a damage table - so each gets its own scene rather than one "average".
enum class Scenario {
    Terrain,   ///< a real generated section straddling sea level
    Uniform,   ///< a solid stone section: the case WorldSave.hpp says is ~3 bytes
    Damaged,   ///< terrain plus 64 partially destroyed blocks
};

constexpr std::size_t kDamagedBlocks = 64;
constexpr std::int32_t kCarveEdge    = 4;

ChunkPtr buildSource(Scenario scenario)
{
    if (scenario == Scenario::Uniform) {
        return uniformChunk(ChunkPos{0, 2, 0}, blocks::Stone);
    }

    ChunkPtr chunk = generateChunk(ChunkPos{0, kSeaLevel / kChunkSize, 0}, kLodFull);
    if (scenario == Scenario::Damaged) {
        for (const std::size_t blockIndex : spreadSolidBlocks(*chunk, kDamagedBlocks)) {
            for (std::int32_t sy = 0; sy < kCarveEdge; ++sy) {
                for (std::int32_t sz = 0; sz < kCarveEdge; ++sz) {
                    for (std::int32_t sx = 0; sx < kCarveEdge; ++sx) {
                        chunk->breakSubVoxel(blockIndex, subVoxelIndex(sx, sy, sz));
                    }
                }
            }
        }
    }
    return chunk;
}

void addSceneCounters(CaseContext& context, const SaveFixture& fixture)
{
    const double bytes = static_cast<double>(fixture.payload.size());
    context.counter("payload_bytes", bytes, "bytes");
    context.counter("bytes_per_voxel", bytes / static_cast<double>(kChunkVolume), "B/voxel");
    context.counter("vs_raw_voxels_and_light", bytes > 0.0 ? kRawSectionBytes / bytes : 0.0, "x");
    context.counter("palette_entries", static_cast<double>(fixture.source->storage().paletteSize()));
    context.counter("bits_per_index",
                    static_cast<double>(fixture.source->storage().bitsPerIndex()));
    context.counter("damaged_blocks", static_cast<double>(fixture.source->subVoxels().size()),
                    "blocks");
    context.counter("non_air_blocks", static_cast<double>(countNonAir(*fixture.source)), "blocks");
}

/// Shared setup: build the scene, encode once so the size counters are real, and
/// prove the round trip actually round-trips before anything is timed.
void buildFixture(CaseContext& context, SaveFixture& fixture, Scenario scenario)
{
    fixture.source      = buildSource(scenario);
    fixture.destination = Chunk::create(fixture.source->position());
    fixture.payload     = WorldSave::encodeChunk(*fixture.source);

    const ChunkLoadResult loaded = WorldSave::decodeChunk(fixture.payload, *fixture.destination);
    VOXL_CHECK(!loaded.regenerate(),
               "decodeChunk rejected a payload encodeChunk had just produced");

    addSceneCounters(context, fixture);
}

const char* scenarioName(Scenario scenario) noexcept
{
    switch (scenario) {
        case Scenario::Terrain: return "terrain";
        case Scenario::Uniform: return "uniform";
        case Scenario::Damaged: return "damaged";
    }
    return "unknown";
}

void addRoundTripCase(Runner& runner, Scenario scenario)
{
    const std::string name = std::string("round_trip_") + scenarioName(scenario);
    if (!runner.selected(kGroup, name)) {
        return;
    }

    auto fixture = std::make_shared<SaveFixture>();

    Case testCase;
    testCase.group      = kGroup;
    testCase.name       = name;
    testCase.unit       = "chunk";
    testCase.opsPerRun  = 1.0;
    testCase.sampleRuns = 21;
    testCase.setup      = [fixture, scenario](CaseContext& context) {
        buildFixture(context, *fixture, scenario);
        context.note("encode allocates a fresh std::vector<std::byte> every call, exactly as "
                          "WorldSave::saveChunk does; the allocation is part of the measurement");
    };
    testCase.body = [fixture](CaseContext&) {
        fixture->payload = WorldSave::encodeChunk(*fixture->source);
        const ChunkLoadResult result =
            WorldSave::decodeChunk(fixture->payload, *fixture->destination);
        keep(static_cast<std::uint64_t>(fixture->payload.size()) +
             static_cast<std::uint64_t>(result.status));
    };
    runner.add(std::move(testCase));
}

void addHalfCase(Runner& runner, bool encode)
{
    const std::string name = encode ? "encode_terrain" : "decode_terrain";
    if (!runner.selected(kGroup, name)) {
        return;
    }

    auto fixture = std::make_shared<SaveFixture>();

    Case testCase;
    testCase.group      = kGroup;
    testCase.name       = name;
    testCase.unit       = "chunk";
    testCase.opsPerRun  = 1.0;
    testCase.sampleRuns = 21;
    testCase.setup      = [fixture](CaseContext& context) {
        buildFixture(context, *fixture, Scenario::Terrain);
    };
    if (encode) {
        testCase.body = [fixture](CaseContext&) {
            fixture->payload = WorldSave::encodeChunk(*fixture->source);
            keep(static_cast<std::uint64_t>(fixture->payload.size()));
        };
    } else {
        testCase.body = [fixture](CaseContext&) {
            const ChunkLoadResult result =
                WorldSave::decodeChunk(fixture->payload, *fixture->destination);
            keep(static_cast<std::uint64_t>(result.status));
        };
    }
    runner.add(std::move(testCase));
}

#endif  // VOXL_BENCH_HAS_WORLD_SAVE

}  // namespace

#if VOXL_BENCH_HAS_WORLD_SAVE

void registerPersistenceCases(Runner& runner)
{
    addRoundTripCase(runner, Scenario::Terrain);
    addRoundTripCase(runner, Scenario::Uniform);
    addRoundTripCase(runner, Scenario::Damaged);
    addHalfCase(runner, /*encode=*/true);
    addHalfCase(runner, /*encode=*/false);
}

#else

void registerPersistenceCases(Runner& runner)
{
    runner.addUnavailable(kGroup, "round_trip_terrain",
                          "world/WorldSave.hpp is unavailable in this build (missing header, or "
                          "VOXL_BENCH_NO_WORLD_SAVE was defined because WorldSave.cpp is not yet "
                          "linked into voxl::engine); see benchmarks/bench_persistence.cpp");
}

#endif  // VOXL_BENCH_HAS_WORLD_SAVE

}  // namespace voxl::bench
