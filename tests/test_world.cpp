// World container and chunk streaming.
//
// Every test drives the real JobSystem, so each one settles the pipeline with
// `waitForPendingJobs()` followed by a `drainAll()` of the main-thread queue -
// that pair is what "the streaming has quiesced" means, and skipping the drain
// leaves chunks stuck in Meshed.

#include "core/JobSystem.hpp"
#include "world/Block.hpp"
#include "world/ChunkManager.hpp"
#include "world/ChunkStorage.hpp"
#include "world/SubVoxel.hpp"
#include "world/VoxelTypes.hpp"
#include "world/World.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

/// A straight bore through the rock with a single lamp in it, crossing the
/// boundary between chunk columns (0, 0) and (1, 0).
///
/// Deep underground, so the sky contributes nothing and the ONLY way the far
/// half of the tunnel can be lit is by block light crossing that boundary.
constexpr std::int32_t kTunnelY = 70;   // inside section 2, well below the floor
constexpr std::int32_t kTunnelZ = 16;
constexpr std::int32_t kLampX   = 24;   // column (0, 0), 8 blocks short of the seam

void generateTunnel(Chunk& chunk)
{
    generateFlat(chunk);

    const BlockPos origin = chunk.position().originBlock();
    if (kTunnelY < origin.y || kTunnelY >= origin.y + kChunkSize) {
        return;
    }
    if (kTunnelZ < origin.z || kTunnelZ >= origin.z + kChunkSize) {
        return;
    }

    const std::int32_t localY = kTunnelY - origin.y;
    const std::int32_t localZ = kTunnelZ - origin.z;
    for (std::int32_t localX = 0; localX < kChunkSize; ++localX) {
        chunk.storage().set(localX, localY, localZ, blocks::Air);
    }
    if (kLampX >= origin.x && kLampX < origin.x + kChunkSize) {
        chunk.storage().set(kLampX - origin.x, localY, localZ, blocks::Glowstone);
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

/// Every light value in the cube of radius `radius` around `centre`, in a fixed
/// order, so two edits made in identical surroundings can be compared cell for
/// cell. Sunlight and block light are kept apart because a glowstone changes
/// both and only one of them survives a missing relight.
[[nodiscard]] std::vector<std::uint8_t> lightAround(const World& world, const BlockPos& centre,
                                                    std::int32_t radius)
{
    std::vector<std::uint8_t> out;
    out.reserve(static_cast<std::size_t>(2 * radius + 1) * static_cast<std::size_t>(2 * radius + 1) *
                static_cast<std::size_t>(2 * radius + 1) * 2);
    for (std::int32_t dy = -radius; dy <= radius; ++dy) {
        for (std::int32_t dz = -radius; dz <= radius; ++dz) {
            for (std::int32_t dx = -radius; dx <= radius; ++dx) {
                const std::uint8_t packed =
                    world.getLight(BlockPos{centre.x + dx, centre.y + dy, centre.z + dz});
                out.push_back(ChunkStorage::unpackSunlight(packed));
                out.push_back(ChunkStorage::unpackBlockLight(packed));
            }
        }
    }
    return out;
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

// ---------------------------------------------------------------------------
//  Regression: a deferred edit must be relit exactly as an immediate one
// ---------------------------------------------------------------------------

TEST_CASE("a deferred edit is relit exactly as an immediate one", "[world][light][regression]")
{
    // REGRESSION. World::setBlock relights around the edit; World::applyDeferredEdits
    // - the path every edit made near the streaming frontier goes down - used to
    // call a bare writeBlock() and nothing else. The block landed, the light did
    // not move, and nothing ever came back to fix it: light that has already been
    // published is not recomputed, so the stale value was permanent.
    //
    // The same replay also has to re-test the RELIGHT footprint, not just the
    // 3x3x3 isEditBlocked covers. A sunlight change runs the height of the world,
    // which is exactly why setBlock defers on isRelightBlocked as well; a replay
    // that only re-tested isEditBlocked would flood light into a chunk a worker
    // owns and reintroduce the race the deferral exists to prevent.
    StreamingConfig config = tightConfig();
    config.loadRadius      = 3;
    REQUIRE(config.lighting);

    Fixture fixture(config);
    const StreamingView view = viewAt(16.5f, 200.0f, 16.5f);
    fixture.settle(view);

    // Both sit in the interior of chunk (0, 6, 0), in identical open air well
    // above the flat floor, and 32 blocks apart - twice the reach of a glowstone,
    // so neither can light the other and their neighbourhoods must come out
    // identical cell for cell.
    const BlockPos immediate{8, 200, 8};
    const BlockPos deferred{24, 200, 24};

    REQUIRE(ChunkStorage::unpackSunlight(fixture.world.getLight(immediate)) == 15);
    REQUIRE(ChunkStorage::unpackSunlight(fixture.world.getLight(deferred)) == 15);
    REQUIRE(ChunkStorage::unpackBlockLight(fixture.world.getLight(deferred)) == 0);

    // ---- the control: the same edit, applied at once ----
    REQUIRE(fixture.world.setBlock(immediate, blocks::Glowstone) == EditResult::Applied);
    REQUIRE(ChunkStorage::unpackBlockLight(fixture.world.getLight(immediate)) == 15);

    // ---- force the deferral, through the relight footprint alone ----
    //
    // Chunk (0, 3, 0) is four sections below the edit: outside the 3x3x3 that
    // isEditBlocked tests, inside the column the sunlight change would run down.
    // With it busy, setBlock must defer even though the edited chunk itself is
    // perfectly writable - which is the state the replay has to handle.
    const ChunkPtr blocker = fixture.world.chunkAt(ChunkPos{0, 3, 0});
    REQUIRE(blocker != nullptr);
    const ChunkState blockerState = blocker->state();
    blocker->forceState(ChunkState::Meshing);

    REQUIRE(fixture.world.setBlock(deferred, blocks::Glowstone) == EditResult::Deferred);
    REQUIRE(fixture.world.getBlock(deferred) == blocks::Air);

    // One update with the blocker still busy. The replay must notice that the
    // relight is still unsafe and leave the edit queued; writing the block here
    // and skipping the relight is precisely the defect.
    fixture.world.update(view);
    CHECK(fixture.world.deferredEditCount() == 1);
    CHECK(fixture.world.getBlock(deferred) == blocks::Air);

    // ---- let it through ----
    blocker->forceState(blockerState);
    fixture.settle(view);

    CHECK(fixture.world.deferredEditCount() == 0);
    REQUIRE(fixture.world.getBlock(deferred) == blocks::Glowstone);

    // The whole point: a deferred edit and an immediate one leave the same light.
    CHECK(lightAround(fixture.world, deferred, 4) == lightAround(fixture.world, immediate, 4));
    // Spelled out as well, so a failure says what is wrong rather than "vectors
    // differ": the lamp lights its own cell, and the cell under it one less.
    CHECK(ChunkStorage::unpackBlockLight(fixture.world.getLight(deferred)) == 15);
    CHECK(ChunkStorage::unpackBlockLight(
              fixture.world.getLight(BlockPos{deferred.x, deferred.y - 1, deferred.z})) == 14);
    // ... and it casts a shadow: the cell below an opaque block can only be lit
    // sideways, which costs one.
    CHECK(ChunkStorage::unpackSunlight(
              fixture.world.getLight(BlockPos{deferred.x, deferred.y - 1, deferred.z})) == 14);
}

// ---------------------------------------------------------------------------
//  Regression: two columns lit at the same time must still exchange light
// ---------------------------------------------------------------------------

TEST_CASE("light crosses the seam between columns that were lit at the same time",
          "[world][light][regression]")
{
    // REGRESSION. A column light job may only read neighbours that are final in
    // both voxels and light, so a neighbour column that is in flight at the same
    // moment arrives as a null slot and the engine treats it as a solid wall.
    // That is supposed to be self-correcting - whichever column is lit SECOND
    // reads the first and spills back - but when two neighbours are lit at the
    // same time neither is second. Each sees a wall, neither reads the other,
    // neither spills, and since published light is never recomputed the seam
    // between them stays dark for good.
    //
    // Claiming every outstanding column in one sweep is exactly the situation:
    // after the first settle they are all pending together, so every adjacent
    // pair goes in flight together. Normal streaming produces the same state at
    // the loading frontier, several columns at a time.
    StreamingConfig config = tightConfig();
    config.loadRadius               = 2;
    config.maxLightColumnsPerUpdate = 4096;
    config.maxLightJobsInFlight     = 4096;
    REQUIRE(config.lighting);

    Fixture fixture(config, 4);
    fixture.world.setGenerator(generateTunnel);

    const StreamingView view = viewAt(16.5f, 200.0f, 16.5f);

    std::size_t seamsRecorded = 0;
    for (int step = 0; step < 10; ++step) {
        fixture.world.update(view);
        REQUIRE(fixture.world.chunks().waitForPendingJobs(std::chrono::milliseconds{10000}));
        fixture.jobs.mainThreadQueue().drainAll();
        // Sampled between the light jobs finishing and the next update settling
        // what they recorded.
        seamsRecorded = std::max(seamsRecorded, fixture.world.pendingLightSeams());
    }

    // The situation under test really arose. Had the columns been lit one after
    // another, every neighbour would have been readable and nothing recorded.
    REQUIRE(seamsRecorded > 0);

    // The tunnel is where the generator put it, and it is genuinely enclosed.
    REQUIRE(fixture.world.getBlock(BlockPos{34, kTunnelY, kTunnelZ}) == blocks::Air);
    REQUIRE(fixture.world.getBlock(BlockPos{34, kTunnelY + 1, kTunnelZ}) == blocks::Stone);
    REQUIRE(ChunkStorage::unpackSunlight(
                fixture.world.getLight(BlockPos{34, kTunnelY, kTunnelZ})) == 0);

    // The lamp end, inside column (0, 0), is lit by its own column's job.
    REQUIRE(ChunkStorage::unpackBlockLight(
                fixture.world.getLight(BlockPos{kLampX, kTunnelY, kTunnelZ})) == 15);
    CHECK(ChunkStorage::unpackBlockLight(
              fixture.world.getLight(BlockPos{31, kTunnelY, kTunnelZ})) == 8);

    // The far end is in column (1, 0). Nothing crossed the boundary while the two
    // columns were being lit, so every one of these is zero unless the seam is
    // reconciled afterwards. One block per step, from 7 at the first cell across.
    CHECK(ChunkStorage::unpackBlockLight(
              fixture.world.getLight(BlockPos{32, kTunnelY, kTunnelZ})) == 7);
    CHECK(ChunkStorage::unpackBlockLight(
              fixture.world.getLight(BlockPos{34, kTunnelY, kTunnelZ})) == 5);
    CHECK(ChunkStorage::unpackBlockLight(
              fixture.world.getLight(BlockPos{38, kTunnelY, kTunnelZ})) == 1);
}

// ---------------------------------------------------------------------------
//  Regression: the light seam fan-out must survive both faces of one axis
// ---------------------------------------------------------------------------

TEST_CASE("a relight spanning a whole section dirties the right neighbours",
          "[world][light][regression]")
{
    // REGRESSION. World::markLitChunksDirty turns the set of chunk faces a
    // relight touched into the set of neighbours that must remesh, by building
    // a per-axis offset list and taking the product. That list was sized for
    // two entries per axis - copied from markSeamNeighboursDirty, where the
    // source is a single voxel and so can only be against ONE face of any given
    // axis.
    //
    // A flood is not a voxel. An opaque block dropped into an open column
    // shadows every section beneath it from top to bottom, so a section fully
    // inside that shadow has its light change at local y == 0 AND at
    // local y == 31: both the NegY and the PosY bit. The axis then needs three
    // offsets (0, -1, +1) and the third write ran off the row - corrupting the
    // Z axis's zero entry on the way past, and on the last axis taking out the
    // stack cookie and killing the process with no log at all.
    //
    // The observable below is the corruption, not the crash: with the row
    // overflowing, the Z offset list lost its zero and every neighbour was
    // dirtied one chunk further along +Z than it should have been.
    StreamingConfig config = tightConfig();
    config.loadRadius      = 3;
    REQUIRE(config.lighting);

    Fixture fixture(config);
    fixture.settle(viewAt(16.5f, 200.0f, 16.5f));

    ChunkManager& chunks = fixture.world.chunks();

    // Sections 4 (y 128..159) and 5 (y 160..191) sit entirely between the floor
    // and the block placed below, so both are fully shadowed by it.
    const ChunkPos shadowed{0, 5, 0};
    const ChunkPos above{0, 6, 0};
    const ChunkPos below{0, 4, 0};
    const ChunkPos acrossZ{0, 5, 1};
    for (const ChunkPos& position : {shadowed, above, below, acrossZ}) {
        const ChunkPtr chunk = chunks.find(position);
        REQUIRE(chunk != nullptr);
        chunk->clearRemeshFlag();
    }

    // Interior of its own section (local 16, 8, 16), so nothing here is a seam
    // edit: every dirty flag observed below comes from the light pass.
    REQUIRE(fixture.world.setBlock(BlockPos{16, 200, 16}, blocks::Stone) == EditResult::Applied);

    // The shaft runs down the middle of the column, so the light that changed
    // never reaches a Z face - and no neighbour across Z has any reason to
    // remesh. This is the assertion the overflow used to fail.
    CHECK_FALSE(chunks.find(acrossZ)->needsRemesh());

    // The sections the shadow actually crossed, and their vertical neighbours,
    // do have to remesh: each one samples the other's light as its skirt.
    CHECK(chunks.find(shadowed)->needsRemesh());
    CHECK(chunks.find(below)->needsRemesh());
    CHECK(chunks.find(above)->needsRemesh());
}

// ---------------------------------------------------------------------------
//  Regression: the last sub-voxel of a block is a full opacity change
// ---------------------------------------------------------------------------

TEST_CASE("carving the last sub-voxel defers on the relight footprint",
          "[world][light][subvoxel][regression]")
{
    // REGRESSION. World::editSubVoxel skipped the isRelightBlocked footprint test
    // whenever the block was already partial, on the reasoning that an
    // already-damaged block is already transparent to the light engine so
    // chipping it further cannot move light. True of every carve but one: when
    // the LAST sub-voxel goes, the block becomes air, and writeSubVoxel relights
    // - a world-height sunlight flood, run against chunks the footprint test
    // would have excluded. That is invariant 1 violated (no writing to a chunk a
    // worker may be reading), on the final swing of mining any block, which is
    // the single most common edit in the game.
    //
    // The observable is the deferral: carving the last sub-voxel must be held
    // back by exactly the same busy chunk that holds back breaking the whole
    // block, because it does exactly the same thing to the light.
    StreamingConfig config = tightConfig();
    config.loadRadius      = 3;
    REQUIRE(config.lighting);

    Fixture             fixture(config);
    const StreamingView view = viewAt(16.5f, 200.0f, 16.5f);
    fixture.settle(view);

    // Four independent blocks in the open air of chunk (0, 6, 0), 16 apart so no
    // two are inside each other's 15-block light reach, and all far above the
    // flat floor at kSeaLevel so each one stands in its own full sunlight column.
    const BlockPos control{8, 200, 8};    // carved to its last sub-voxel, unblocked
    const BlockPos carved{24, 200, 24};   // carved to its last sub-voxel, blocked
    const BlockPos whole{8, 200, 24};     // broken outright, blocked - the parity case
    const BlockPos chipped{24, 200, 8};   // stays partial, blocked - must NOT defer
    const BlockPos refilled{16, 200, 16}; // air restored to partial, blocked

    for (const BlockPos& pos : {control, carved, whole, chipped}) {
        REQUIRE(fixture.world.setBlock(pos, blocks::Stone) == EditResult::Applied);
    }
    REQUIRE(fixture.world.getBlock(refilled) == blocks::Air);

    // Grind each of the two down to one remaining sub-voxel, index 0. The first
    // carve crosses whole -> partial and so does run the footprint test; nothing
    // is busy yet, so it applies. The 510 after it move no light at all.
    const auto grindToLastSubVoxel = [&fixture](const BlockPos& pos) {
        for (std::size_t sub = 1; sub < kSubVoxelCount; ++sub) {
            REQUIRE(fixture.world.breakSubVoxel(pos, sub) == EditResult::Applied);
        }
        const SubVoxelGrid* grid = fixture.world.subVoxelsAt(pos);
        REQUIRE(grid != nullptr);
        REQUIRE(grid->count() == 1);
        REQUIRE(grid->test(0));
    };
    grindToLastSubVoxel(control);
    grindToLastSubVoxel(carved);

    // `chipped` only loses one, so it stays comfortably partial.
    REQUIRE(fixture.world.breakSubVoxel(chipped, 0) == EditResult::Applied);

    // ---- the control: the same last carve, with nothing busy ----
    REQUIRE(fixture.world.breakSubVoxel(control, 0) == EditResult::Applied);
    REQUIRE(fixture.world.getBlock(control) == blocks::Air);
    REQUIRE(fixture.world.subVoxelsAt(control) == nullptr);

    // ---- force the deferral, through the relight footprint alone ----
    //
    // Chunk (0, 3, 0) is three sections below the edits: outside the 3x3x3 that
    // isEditBlocked tests, inside the column a sunlight change would run down.
    // Every deferral below is therefore attributable to isRelightBlocked and
    // nothing else.
    const ChunkPtr blocker = fixture.world.chunkAt(ChunkPos{0, 3, 0});
    REQUIRE(blocker != nullptr);
    const ChunkState blockerState = blocker->state();
    blocker->forceState(ChunkState::Meshing);

    // The parity assertion, and the defect. Breaking the whole block defers;
    // clearing the last sub-voxel of a block does the same thing to the light and
    // so must defer identically. Before the fix this returned Applied and flooded
    // light through the busy chunk.
    REQUIRE(fixture.world.breakBlock(whole) == EditResult::Deferred);
    REQUIRE(fixture.world.breakSubVoxel(carved, 0) == EditResult::Deferred);
    CHECK(fixture.world.getBlock(carved) == blocks::Stone);
    CHECK(fixture.world.subVoxelsAt(carved) != nullptr);

    // The mirror case: air -> partial puts an opaque-to-air block back into an
    // open column, so it needs the same test. (This one was already correct, and
    // is here so a later tightening of the predicate cannot quietly lose it.)
    REQUIRE(fixture.world.restoreSubVoxel(refilled, 0, blocks::Stone) == EditResult::Deferred);

    // ...and the property the whole optimisation exists for must survive the fix:
    // chipping a block that stays partial moves no light, so a busy chunk three
    // sections away has no business stopping it.
    CHECK(fixture.world.breakSubVoxel(chipped, 1) == EditResult::Applied);

    // One update with the blocker still busy. applyDeferredEdits mirrors
    // editSubVoxel, so the replay must reach the same verdict rather than letting
    // the carve through one frame later.
    fixture.world.update(view);
    CHECK(fixture.world.deferredEditCount() == 3);
    CHECK(fixture.world.getBlock(carved) == blocks::Stone);

    // ---- let them through ----
    blocker->forceState(blockerState);
    fixture.settle(view);

    CHECK(fixture.world.deferredEditCount() == 0);
    REQUIRE(fixture.world.getBlock(carved) == blocks::Air);
    CHECK(fixture.world.subVoxelsAt(carved) == nullptr);
    CHECK(fixture.world.getBlock(whole) == blocks::Air);
    CHECK(fixture.world.getBlock(refilled) == blocks::Stone);

    // The point of deferring at all: the deferred carve leaves exactly the light
    // the immediate one did. `control` and `carved` were prepared identically and
    // both ended as air, with no other block inside either radius-4 cube.
    CHECK(lightAround(fixture.world, carved, 4) == lightAround(fixture.world, control, 4));
    CHECK(ChunkStorage::unpackSunlight(fixture.world.getLight(carved)) == 15);
    CHECK(ChunkStorage::unpackSunlight(
              fixture.world.getLight(BlockPos{carved.x, carved.y - 1, carved.z})) == 15);
}
