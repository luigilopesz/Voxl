// World container and chunk streaming.
//
// Every test drives the real JobSystem, so each one settles the pipeline with
// `waitForPendingJobs()` followed by a `drainAll()` of the main-thread queue -
// that pair is what "the streaming has quiesced" means, and skipping the drain
// leaves chunks stuck in Meshed.

#include "core/JobSystem.hpp"
#include "world/Block.hpp"
#include "world/ChunkManager.hpp"
#include "world/VoxelTypes.hpp"
#include "world/World.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <glm/vec3.hpp>

using namespace voxl;

namespace {

/// Deterministic filler: a solid floor under sea level, air above. Cheap enough
/// that a full load radius settles in milliseconds.
void generateFlat(Chunk& chunk)
{
    const BlockPos origin = chunk.position().originBlock();
    if (origin.y + kChunkSize <= kSeaLevel) {
        chunk.storage().fill(blocks::Stone);
        return;
    }
    if (origin.y > kSeaLevel) {
        return;  // already air
    }
    for (std::int32_t y = 0; y < kChunkSize; ++y) {
        if (origin.y + y > kSeaLevel) {
            break;
        }
        for (std::int32_t z = 0; z < kChunkSize; ++z) {
            for (std::int32_t x = 0; x < kChunkSize; ++x) {
                chunk.storage().set(x, y, z, blocks::Stone);
            }
        }
    }
}

/// Bytes the fake mesher claims per chunk, so the GPU memory estimate has
/// something to account for.
constexpr std::size_t kFakeMeshBytes = 1024;

/// Streaming test harness: one job system, one registry, one world, plus the
/// settle() helper.
///
/// The mesher records its observations in atomics rather than asserting: it runs
/// on a worker thread, and Catch2's assertion macros are not thread safe.
struct Fixture {
    explicit Fixture(const StreamingConfig& config, unsigned workers = 2)
        : jobs(workers), registry(createDefaultBlockRegistry()), world(jobs, registry, config)
    {
        world.setGenerator(generateFlat);
        world.setMesher([this](const ChunkNeighbourhood& snapshot) {
            meshCalls.fetch_add(1, std::memory_order_relaxed);
            if (!snapshot.complete() || snapshot.centre() == nullptr) {
                incompleteSnapshots.fetch_add(1, std::memory_order_relaxed);
            }
            ChunkMeshUpload result;
            result.gpuBytes  = kFakeMeshBytes;
            result.triangles = 2;
            result.upload    = [this] { uploads.fetch_add(1, std::memory_order_relaxed); };
            return result;
        });
    }

    /// Runs update/settle until nothing changes, so a chunk that had to wait for
    /// its neighbours to exist before it could mesh gets its turn.
    void settle(const StreamingView& view, int iterations = 8)
    {
        for (int i = 0; i < iterations; ++i) {
            world.update(view);
            REQUIRE(world.chunks().waitForPendingJobs(std::chrono::milliseconds{10000}));
            jobs.mainThreadQueue().drainAll();
        }
    }

    JobSystem        jobs;
    BlockRegistry    registry;
    World            world;
    std::atomic<int> meshCalls{0};
    std::atomic<int> incompleteSnapshots{0};
    std::atomic<int> uploads{0};
};

[[nodiscard]] StreamingView viewAt(float x, float y, float z)
{
    return StreamingView{glm::vec3{x, y, z}, glm::vec3{0.0f, 0.0f, -1.0f}};
}

[[nodiscard]] StreamingConfig tightConfig()
{
    StreamingConfig config;
    config.loadRadius             = 2;
    config.unloadPadding          = 2;
    config.verticalRadius         = kWorldSectionCount;
    config.unloadGraceFrames      = 0;
    config.maxScheduledPerUpdate  = 4096;
    config.maxGenerateJobsInFlight = 4096;
    config.maxMeshJobsInFlight    = 4096;
    config.maxUnloadsPerUpdate    = 4096;
    return config;
}

}  // namespace

// ---------------------------------------------------------------------------
//  Coordinates: the floor-division cases
// ---------------------------------------------------------------------------

TEST_CASE("setBlock and getBlock round-trip across chunk borders", "[world]")
{
    StreamingConfig config = tightConfig();
    config.loadRadius      = 3;  // chunk (-2, y, -2) is at squared distance 8
    Fixture fixture(config);
    // Sits on the (0,0) / (-1,-1) chunk corner so both signs of every axis are in
    // the load radius.
    const StreamingView view = viewAt(0.5f, static_cast<float>(kSeaLevel) + 2.0f, 0.5f);
    fixture.settle(view);

    struct Case {
        BlockPos position;
        ChunkPos expectedChunk;
        BlockPos expectedLocal;
    };

    // Every one of these is wrong under truncating division: -1 / 32 == 0 and
    // -1 % 32 == -1, which folds the first block west of the origin into chunk 0.
    const std::vector<Case> cases = {
        {BlockPos{0, 100, 0}, ChunkPos{0, 3, 0}, BlockPos{0, 4, 0}},
        {BlockPos{31, 100, 31}, ChunkPos{0, 3, 0}, BlockPos{31, 4, 31}},
        {BlockPos{32, 100, 0}, ChunkPos{1, 3, 0}, BlockPos{0, 4, 0}},
        {BlockPos{-1, 100, -1}, ChunkPos{-1, 3, -1}, BlockPos{31, 4, 31}},
        {BlockPos{-32, 100, -32}, ChunkPos{-1, 3, -1}, BlockPos{0, 4, 0}},
        {BlockPos{-33, 100, -33}, ChunkPos{-2, 3, -2}, BlockPos{31, 4, 31}},
        {BlockPos{-1, 96, 0}, ChunkPos{-1, 3, 0}, BlockPos{31, 0, 0}},
    };

    BlockId id = blocks::Stone;
    for (const Case& item : cases) {
        // A distinct id per case proves the write did not land in a shared slot.
        id = static_cast<BlockId>(id == blocks::Ice ? blocks::Stone : id + 1);

        REQUIRE(toChunkPos(item.position) == item.expectedChunk);
        REQUIRE(toLocalPos(item.position) == item.expectedLocal);

        REQUIRE(fixture.world.setBlock(item.position, id) == EditResult::Applied);
        CHECK(fixture.world.getBlock(item.position) == id);

        // ... and it is really in the chunk the floor division names, at the
        // local index the contract fixes.
        const ChunkPtr chunk = fixture.world.chunkAt(item.expectedChunk);
        REQUIRE(chunk != nullptr);
        CHECK(chunk->getBlock(item.expectedLocal) == id);
    }
}

TEST_CASE("out-of-world and unloaded reads follow the BlockAccess contract", "[world]")
{
    Fixture fixture(tightConfig());
    fixture.settle(viewAt(0.5f, static_cast<float>(kSeaLevel) + 2.0f, 0.5f));

    CHECK(fixture.world.getBlock(BlockPos{0, kWorldMaxY + 1, 0}) == kAboveWorldBlock);
    CHECK(fixture.world.getLight(BlockPos{0, kWorldMaxY + 1, 0}) == kAboveWorldLight);
    CHECK(fixture.world.getBlock(BlockPos{0, kWorldMinY - 1, 0}) == kBelowWorldBlock);
    CHECK(fixture.world.getLight(BlockPos{0, kWorldMinY - 1, 0}) == kBelowWorldLight);

    // Far outside the load radius, so no chunk is resident there.
    CHECK(fixture.world.getBlock(BlockPos{100000, 100, 100000}) == kMissingChunkBlock);
    CHECK(fixture.world.setBlock(BlockPos{100000, 100, 100000}, blocks::Stone) ==
          EditResult::NotLoaded);
    CHECK(fixture.world.setBlock(BlockPos{0, kWorldMaxY + 1, 0}, blocks::Stone) ==
          EditResult::OutOfBounds);
}

// ---------------------------------------------------------------------------
//  Seam invalidation
// ---------------------------------------------------------------------------

TEST_CASE("a border setBlock marks the neighbouring chunk dirty", "[world]")
{
    Fixture fixture(tightConfig());
    fixture.settle(viewAt(0.5f, static_cast<float>(kSeaLevel) + 2.0f, 0.5f));

    const ChunkPos centre{0, 3, 0};
    const ChunkPos westward{-1, 3, 0};
    const ChunkPos eastward{1, 3, 0};

    const ChunkPtr centreChunk = fixture.world.chunkAt(centre);
    const ChunkPtr westChunk   = fixture.world.chunkAt(westward);
    const ChunkPtr eastChunk   = fixture.world.chunkAt(eastward);
    REQUIRE(centreChunk != nullptr);
    REQUIRE(westChunk != nullptr);
    REQUIRE(eastChunk != nullptr);

    SECTION("editing local x == 0 dirties the chunk to the west, not the east")
    {
        centreChunk->clearRemeshFlag();
        westChunk->clearRemeshFlag();
        eastChunk->clearRemeshFlag();

        REQUIRE(fixture.world.setBlock(BlockPos{0, 100, 5}, blocks::Planks) == EditResult::Applied);

        CHECK(centreChunk->needsRemesh());
        CHECK(westChunk->needsRemesh());
        CHECK_FALSE(eastChunk->needsRemesh());
        // The neighbour's voxels did not change, so it must not be queued for a
        // save just because its mesh went stale.
        CHECK_FALSE(westChunk->needsSave());
        CHECK(centreChunk->needsSave());
    }

    SECTION("an interior edit dirties nobody else")
    {
        centreChunk->clearRemeshFlag();
        westChunk->clearRemeshFlag();
        eastChunk->clearRemeshFlag();

        REQUIRE(fixture.world.setBlock(BlockPos{16, 100, 16}, blocks::Planks) ==
                EditResult::Applied);

        CHECK(centreChunk->needsRemesh());
        CHECK_FALSE(westChunk->needsRemesh());
        CHECK_FALSE(eastChunk->needsRemesh());
    }

    SECTION("a corner edit dirties the diagonal neighbours too, for ambient occlusion")
    {
        const ChunkPtr diagonal = fixture.world.chunkAt(ChunkPos{-1, 3, -1});
        const ChunkPtr below    = fixture.world.chunkAt(ChunkPos{0, 2, 0});
        REQUIRE(diagonal != nullptr);
        REQUIRE(below != nullptr);
        diagonal->clearRemeshFlag();
        below->clearRemeshFlag();
        westChunk->clearRemeshFlag();

        // Local (0, 0, 0) of chunk (0, 3, 0).
        REQUIRE(fixture.world.setBlock(BlockPos{0, 96, 0}, blocks::Planks) == EditResult::Applied);

        CHECK(westChunk->needsRemesh());
        CHECK(diagonal->needsRemesh());
        CHECK(below->needsRemesh());
    }
}

// ---------------------------------------------------------------------------
//  Streaming residency and hysteresis
// ---------------------------------------------------------------------------

TEST_CASE("streaming loads exactly the chunks inside the load radius", "[world][streaming]")
{
    Fixture fixture(tightConfig());
    fixture.settle(viewAt(0.5f, static_cast<float>(kSeaLevel) + 2.0f, 0.5f));

    const ChunkManager& manager = fixture.world.chunks();
    const ChunkPos      centre{0, 3, 0};

    // Circular, not square: (2, 2) is at squared distance 8, outside radius 2.
    CHECK(manager.isResident(ChunkPos{0, 0, 0}));
    CHECK(manager.isResident(ChunkPos{2, 3, 0}));
    CHECK(manager.isResident(ChunkPos{-2, 3, 0}));
    CHECK(manager.isResident(ChunkPos{0, 3, 2}));
    CHECK(manager.isResident(ChunkPos{1, 3, 1}));
    CHECK_FALSE(manager.isResident(ChunkPos{2, 3, 2}));
    CHECK_FALSE(manager.isResident(ChunkPos{3, 3, 0}));

    CHECK(manager.inLoadRange(centre, ChunkPos{1, 3, 1}));
    CHECK_FALSE(manager.inLoadRange(centre, ChunkPos{2, 3, 2}));
    // Nothing is loaded outside the world's vertical bounds, whatever the radius.
    CHECK_FALSE(manager.inLoadRange(centre, ChunkPos{0, kWorldSectionCount, 0}));
    CHECK_FALSE(manager.isResident(ChunkPos{0, kWorldSectionCount, 0}));

    // A full vertical column per loaded position: 13 columns x 8 sections.
    const std::size_t expectedColumns = 13;
    CHECK(manager.residentCount() ==
          expectedColumns * static_cast<std::size_t>(kWorldSectionCount));

    const WorldStats stats = fixture.world.stats();
    CHECK(stats.loadedChunks == manager.residentCount());
    CHECK(stats.generatingChunks == 0);
    CHECK(stats.meshingChunks == 0);
    CHECK(stats.cpuVoxelBytes > 0);

    // Only chunks whose whole neighbourhood is loaded may be meshed. At radius 2
    // that is the centre column alone: (1, y, 0) needs (2, y, 1), which is at
    // squared distance 5 and therefore never loaded.
    CHECK(stats.readyChunks == static_cast<std::size_t>(kWorldSectionCount));
    CHECK(fixture.world.chunkAt(ChunkPos{0, 3, 0})->state() == ChunkState::Ready);
    CHECK(fixture.world.chunkAt(ChunkPos{1, 3, 0})->state() == ChunkState::Generated);
    CHECK(fixture.world.chunkAt(ChunkPos{2, 3, 0})->state() == ChunkState::Generated);

    // GPU accounting tracks exactly the chunks whose mesh was uploaded.
    CHECK(stats.gpuMeshBytes == stats.readyChunks * kFakeMeshBytes);
    CHECK(stats.meshesUploaded == stats.readyChunks);
    CHECK(static_cast<std::size_t>(fixture.uploads.load()) == stats.readyChunks);
    CHECK(fixture.incompleteSnapshots.load() == 0);
    CHECK(stats.meshesDropped == 0);
}

TEST_CASE("unloading uses hysteresis instead of thrashing the boundary",
          "[world][streaming]")
{
    Fixture fixture(tightConfig());
    // load radius 2, unload radius 4.
    const StreamingView origin = viewAt(0.5f, static_cast<float>(kSeaLevel) + 2.0f, 0.5f);
    fixture.settle(origin);

    const ChunkManager& manager = fixture.world.chunks();
    const ChunkPos      boundary{-2, 3, 0};
    const ChunkPtr      boundaryChunk = fixture.world.chunkAt(boundary);
    REQUIRE(boundaryChunk != nullptr);

    SECTION("a chunk between the load and unload radii stays resident")
    {
        // Two chunks east: the boundary chunk is now 4 away - out of load range,
        // still inside keep range.
        const StreamingView moved =
            viewAt(2.0f * static_cast<float>(kChunkSize) + 0.5f,
                   static_cast<float>(kSeaLevel) + 2.0f, 0.5f);
        fixture.settle(moved);

        const ChunkPos newCentre{2, 3, 0};
        CHECK_FALSE(manager.inLoadRange(newCentre, boundary));
        CHECK(manager.inKeepRange(newCentre, boundary));
        REQUIRE(manager.isResident(boundary));
        // Same object: it was never retired and rebuilt.
        CHECK(fixture.world.chunkAt(boundary).get() == boundaryChunk.get());
    }

    SECTION("walking back and forth across the load boundary unloads nothing")
    {
        const std::uint64_t unloadedBefore = fixture.world.stats().chunksUnloaded;
        const std::uint64_t createdBefore  = fixture.world.stats().chunksCreated;

        // One chunk east and back, repeatedly. Without the padding, every step
        // would retire the trailing ring and rebuild it on the way back.
        for (int step = 0; step < 6; ++step) {
            const float x = (step % 2 == 0) ? static_cast<float>(kChunkSize) + 0.5f : 0.5f;
            fixture.settle(viewAt(x, static_cast<float>(kSeaLevel) + 2.0f, 0.5f), 2);
        }

        const WorldStats after = fixture.world.stats();
        CHECK(after.chunksUnloaded == unloadedBefore);
        CHECK(manager.isResident(boundary));
        CHECK(fixture.world.chunkAt(boundary).get() == boundaryChunk.get());
        // The one extra column ring entered on the first step is allowed to be
        // created; nothing may be created twice.
        CHECK(after.chunksCreated > createdBefore);
        CHECK(after.chunksUnloaded == 0);
    }

    SECTION("a chunk beyond the unload radius is retired")
    {
        const StreamingView faraway =
            viewAt(20.0f * static_cast<float>(kChunkSize), static_cast<float>(kSeaLevel) + 2.0f,
                   0.0f);
        fixture.settle(faraway);

        CHECK_FALSE(manager.isResident(boundary));
        CHECK_FALSE(manager.isResident(ChunkPos{0, 3, 0}));
        CHECK(fixture.world.stats().chunksUnloaded > 0);
        // Use-after-free canary: the retired chunk is still alive because this
        // test holds a shared_ptr, and it was left in the terminal state.
        CHECK(boundaryChunk->state() == ChunkState::Unloading);
    }
}

TEST_CASE("streaming keeps the whole vertical column of a loaded position",
          "[world][streaming]")
{
    Fixture fixture(tightConfig());
    fixture.settle(viewAt(0.5f, static_cast<float>(kSeaLevel) + 2.0f, 0.5f));

    for (std::int32_t y = 0; y < kWorldSectionCount; ++y) {
        CHECK(fixture.world.chunks().isResident(ChunkPos{0, y, 0}));
    }
}

// ---------------------------------------------------------------------------
//  Concurrency invariants
// ---------------------------------------------------------------------------

TEST_CASE("no chunk enters Generating twice", "[world][streaming][threading]")
{
    StreamingConfig config = tightConfig();
    // Small per-update budgets so the same chunk is a candidate on several
    // consecutive updates while its job is still queued - the exact window in
    // which a scheduler without the compare-exchange guard double-submits.
    config.maxScheduledPerUpdate   = 3;
    config.maxGenerateJobsInFlight = 64;
    config.maxMeshJobsInFlight     = 64;

    std::mutex                              mutex;
    std::unordered_map<ChunkPos, int>        generationCount;
    std::unordered_map<ChunkPos, ChunkState> stateOnEntry;

    JobSystem     jobs(4);
    BlockRegistry registry = createDefaultBlockRegistry();
    World         world(jobs, registry, config);

    world.setGenerator([&](Chunk& chunk) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            generationCount[chunk.position()] += 1;
            stateOnEntry.emplace(chunk.position(), chunk.state());
        }
        generateFlat(chunk);
    });
    world.setMesher([](const ChunkNeighbourhood&) { return ChunkMeshUpload{}; });

    const StreamingView view = viewAt(0.5f, static_cast<float>(kSeaLevel) + 2.0f, 0.5f);

    // Hammer update() without waiting, so schedule decisions are made while jobs
    // are still queued and running.
    for (int i = 0; i < 200; ++i) {
        world.update(view);
        jobs.mainThreadQueue().drain(std::chrono::microseconds{200});
    }
    REQUIRE(world.chunks().waitForPendingJobs(std::chrono::milliseconds{10000}));
    jobs.mainThreadQueue().drainAll();

    std::lock_guard<std::mutex> lock(mutex);
    REQUIRE_FALSE(generationCount.empty());
    for (const auto& [position, count] : generationCount) {
        INFO("chunk " << position.x << ", " << position.y << ", " << position.z);
        CHECK(count == 1);
        // The generator must own the chunk exclusively while it runs.
        CHECK(stateOnEntry.at(position) == ChunkState::Generating);
    }
}

TEST_CASE("a mesh job never sees an incomplete neighbourhood", "[world][streaming][threading]")
{
    StreamingConfig config = tightConfig();
    config.loadRadius      = 4;  // wide enough that several rings are meshable
    Fixture fixture(config);
    fixture.settle(viewAt(0.5f, static_cast<float>(kSeaLevel) + 2.0f, 0.5f));

    const WorldStats stats = fixture.world.stats();
    CHECK(fixture.meshCalls.load() > 0);
    // The scheduler must not hand a mesher a hole in the 3x3x3 - the resulting
    // seam would be baked in permanently, because nothing would dirty the chunk
    // again once the neighbour arrived.
    CHECK(fixture.incompleteSnapshots.load() == 0);
    CHECK(stats.meshesUploaded > 0);
    CHECK(stats.meshesDropped == 0);

    // Every chunk that was meshed is Ready; every chunk that was not is still
    // Generated, waiting for neighbours it will never get at this radius.
    CHECK(stats.readyChunks + stats.chunksByState[static_cast<std::size_t>(ChunkState::Generated)] ==
          stats.loadedChunks);
}

TEST_CASE("unloadAll drops in-flight results instead of resurrecting chunks",
          "[world][streaming][threading]")
{
    StreamingConfig config = tightConfig();
    Fixture         fixture(config);
    const StreamingView view = viewAt(0.5f, static_cast<float>(kSeaLevel) + 2.0f, 0.5f);

    fixture.world.update(view);
    // Retire everything while the generate/mesh jobs are still in flight. Their
    // completion paths must notice and drop what they produced.
    fixture.world.unloadAll();
    REQUIRE(fixture.world.chunks().waitForPendingJobs(std::chrono::milliseconds{10000}));
    fixture.jobs.mainThreadQueue().drainAll();

    CHECK(fixture.world.chunks().residentCount() == 0);
    CHECK(fixture.world.stats().gpuMeshBytes == 0);

    // The world recovers: a later update rebuilds the volume from scratch.
    fixture.settle(view);
    CHECK(fixture.world.chunks().residentCount() > 0);
}

TEST_CASE("an edit against a busy chunk is deferred, not lost", "[world]")
{
    StreamingConfig config = tightConfig();
    Fixture         fixture(config);
    const BlockPos  target{4, 100, 4};

    // First update only creates the chunks and queues generation, so the target
    // chunk is Empty or Generating right now.
    fixture.world.update(viewAt(0.5f, static_cast<float>(kSeaLevel) + 2.0f, 0.5f));
    const EditResult result = fixture.world.setBlock(target, blocks::Glowstone);
    REQUIRE((result == EditResult::Deferred || result == EditResult::Applied));

    if (result == EditResult::Deferred) {
        CHECK(fixture.world.deferredEditCount() == 1);
        CHECK(fixture.world.stats().deferredEdits == 1);
    }

    fixture.settle(viewAt(0.5f, static_cast<float>(kSeaLevel) + 2.0f, 0.5f));

    CHECK(fixture.world.deferredEditCount() == 0);
    CHECK(fixture.world.getBlock(target) == blocks::Glowstone);
}

TEST_CASE("the load and keep radii are separated by the hysteresis padding",
          "[world][streaming]")
{
    StreamingConfig config = tightConfig();
    config.loadRadius      = 4;
    config.unloadPadding   = 1;
    Fixture fixture(config);

    const ChunkManager& manager = fixture.world.chunks();
    const ChunkPos      centre{0, 3, 0};

    CHECK(manager.config().unloadRadius() == 5);
    CHECK(manager.inLoadRange(centre, ChunkPos{4, 3, 0}));
    CHECK_FALSE(manager.inLoadRange(centre, ChunkPos{5, 3, 0}));
    CHECK(manager.inKeepRange(centre, ChunkPos{5, 3, 0}));
    CHECK_FALSE(manager.inKeepRange(centre, ChunkPos{6, 3, 0}));
    CHECK(ChunkManager::horizontalDistanceSq(ChunkPos{-3, 0, 4}, ChunkPos{1, 7, 1}) == 16 + 9);
}
