// Distance-based level of detail in the streaming pipeline.
//
// The interesting property here is not "the right level gets chosen" - that is
// a pure function and cheap to check - but that CHANGING a level never puts a
// hole on the screen and never runs twice for the same chunk at once. Both of
// those are races, so the tests drive the real JobSystem and, where the timing
// matters, hold a mesh job open on a gate while the main thread inspects the
// world mid-transition.
//
// Every settling test uses the same pair the rest of the suite does:
// `waitForPendingJobs()` followed by `drainAll()`. Skipping the drain leaves
// chunks stuck in Meshed and leaves a LOD swap unpublished, because the swap is
// deliberately the last thing a main-thread closure does.

#include "core/JobSystem.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/ChunkManager.hpp"
#include "world/Lod.hpp"
#include "world/SubVoxel.hpp"
#include "world/VoxelTypes.hpp"
#include "world/World.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <glm/vec3.hpp>

using namespace voxl;

namespace {

/// Top of the flat test terrain. Deliberately well inside the section the tests
/// look at rather than exactly on `kSeaLevel`, so that a "block in the middle of
/// this chunk" is solid and is not also sitting on a chunk border.
constexpr std::int32_t kGroundTop = kSeaLevel + 16;

/// Deterministic filler: solid up to kGroundTop, air above. Writes through
/// ChunkStorage rather than Chunk::setBlock on purpose - setBlock would set the
/// "needs save" flag, and StreamingConfig::preserveEditedChunks reads that flag
/// to decide a chunk is player-edited and must not be rebuilt.
void generateFlat(Chunk& chunk)
{
    const BlockPos origin = chunk.position().originBlock();
    if (origin.y + kChunkSize <= kGroundTop) {
        chunk.storage().fill(blocks::Stone);
        return;
    }
    if (origin.y > kGroundTop) {
        return;
    }
    for (std::int32_t y = 0; y < kChunkSize; ++y) {
        if (origin.y + y > kGroundTop) {
            break;
        }
        for (std::int32_t z = 0; z < kChunkSize; ++z) {
            for (std::int32_t x = 0; x < kChunkSize; ++x) {
                chunk.storage().set(x, y, z, blocks::Stone);
            }
        }
    }
}

/// What the fake renderer holds for one position.
struct GpuMesh {
    LodLevel      level    = kLodFull;
    std::uint64_t sequence = 0;  ///< bumped on every upload, so "replaced" is visible
};

/// One world plus a fake renderer, a mesh-concurrency tracker and a gate that
/// can hold a chosen mesh job open.
///
/// The fake renderer's map is touched only from upload closures and the release
/// hook, both of which run on the main thread, so it needs no lock. The
/// concurrency tracker runs on workers and does.
struct LodFixture {
    explicit LodFixture(const StreamingConfig& config, unsigned workers = 3)
        : jobs(workers), registry(createDefaultBlockRegistry()), world(jobs, registry, config)
    {
        world.setGenerator([this](Chunk& chunk) {
            generateCalls.fetch_add(1, std::memory_order_relaxed);
            generateFlat(chunk);
        });

        world.setMesher([this](const ChunkNeighbourhood& snapshot) {
            const ChunkPos pos = snapshot.centrePos();
            const LodLevel level =
                snapshot.centre() != nullptr ? snapshot.centre()->lod() : kLodFull;
            if (!snapshot.complete() || snapshot.centre() == nullptr) {
                incompleteSnapshots.fetch_add(1, std::memory_order_relaxed);
            }

            enterMesh(pos);
            waitIfGated(pos, level);
            leaveMesh(pos);

            ChunkMeshUpload out;
            out.gpuBytes  = 1024;
            out.triangles = 2;
            out.upload    = [this, pos, level] {
                gpu[pos] = GpuMesh{level, ++uploadSequence};
            };
            return out;
        });

        world.setMeshReleaser([this](const ChunkPos& position) {
            gpu.erase(position);
            releases[position] += 1;
        });
    }

    // ---- mesh-job concurrency, per position ----

    void enterMesh(const ChunkPos& pos)
    {
        std::lock_guard<std::mutex> lock(meshMutex);
        const int depth = ++meshInFlight[pos];
        maxConcurrentPerPosition = std::max(maxConcurrentPerPosition, depth);
    }

    void leaveMesh(const ChunkPos& pos)
    {
        std::lock_guard<std::mutex> lock(meshMutex);
        meshInFlight[pos] -= 1;
    }

    // ---- the gate ----

    /// Makes the next mesh job for (`pos`, `level`) block until `openGate()`.
    void armGate(const ChunkPos& pos, LodLevel level)
    {
        std::lock_guard<std::mutex> lock(gateMutex);
        gateArmed = true;
        gateOpen  = false;
        gateTarget = pos;
        gateLevel  = level;
        gateHits   = 0;
    }

    void waitIfGated(const ChunkPos& pos, LodLevel level)
    {
        std::unique_lock<std::mutex> lock(gateMutex);
        if (!gateArmed || !(pos == gateTarget) || level != gateLevel) {
            return;
        }
        ++gateHits;
        gateCv.notify_all();
        gateCv.wait(lock, [this] { return gateOpen; });
    }

    void openGate()
    {
        {
            std::lock_guard<std::mutex> lock(gateMutex);
            gateArmed = false;
            gateOpen  = true;
        }
        gateCv.notify_all();
    }

    [[nodiscard]] int gateHitCount()
    {
        std::lock_guard<std::mutex> lock(gateMutex);
        return gateHits;
    }

    /// Pumps update()/drain() - never waitForPendingJobs, which would deadlock
    /// against the gate - until a job is parked on the gate. False on timeout.
    bool pumpUntilGated(const StreamingView& view, int maxUpdates = 4000)
    {
        for (int i = 0; i < maxUpdates; ++i) {
            world.update(view);
            jobs.mainThreadQueue().drain(std::chrono::microseconds{500});
            if (gateHitCount() > 0) {
                return true;
            }
        }
        return false;
    }

    /// Runs update/settle until nothing changes. LOD rebuilds need several
    /// passes: each one is capped per update, and a completed swap dirties its
    /// 26 neighbours, which then want a remesh of their own.
    void settle(const StreamingView& view, int iterations = 24)
    {
        for (int i = 0; i < iterations; ++i) {
            world.update(view);
            REQUIRE(world.chunks().waitForPendingJobs(std::chrono::milliseconds{20000}));
            jobs.mainThreadQueue().drainAll();
        }
    }

    JobSystem     jobs;
    BlockRegistry registry;
    World         world;

    std::atomic<int> generateCalls{0};
    std::atomic<int> incompleteSnapshots{0};

    // Main thread only.
    std::unordered_map<ChunkPos, GpuMesh> gpu;
    std::unordered_map<ChunkPos, int>     releases;
    std::uint64_t                         uploadSequence = 0;

    std::mutex                        meshMutex;
    std::unordered_map<ChunkPos, int> meshInFlight;
    int                               maxConcurrentPerPosition = 0;

    std::mutex              gateMutex;
    std::condition_variable gateCv;
    bool                    gateArmed = false;
    bool                    gateOpen  = false;
    ChunkPos                gateTarget{};
    LodLevel                gateLevel = kLodFull;
    int                     gateHits  = 0;
};

[[nodiscard]] StreamingView viewAt(float x, float y, float z)
{
    return StreamingView{glm::vec3{x, y, z}, glm::vec3{0.0f, 0.0f, -1.0f}};
}

/// A view centred on the middle of chunk column (`chunkX`, `chunkZ`), at the
/// height of the one section the tests keep meshable.
[[nodiscard]] StreamingView viewAtChunk(std::int32_t chunkX, std::int32_t chunkZ)
{
    const float half = static_cast<float>(kChunkSize) * 0.5f;
    return viewAt(static_cast<float>(chunkX * kChunkSize) + half,
                  static_cast<float>(kSeaLevel) + 2.0f,
                  static_cast<float>(chunkZ * kChunkSize) + half);
}

/// Small, fast streaming setup with tight LOD bands.
///
/// verticalRadius 1 rather than the whole world: only three sections per column
/// are resident and only the middle one can ever satisfy the neighbour rule, so
/// a test settles in a fraction of the time while still exercising every
/// horizontal code path. Bands at 1/2/3 chunks put all four levels well inside
/// the radius, with room to spare at the rim - a chunk whose own neighbours have
/// left the load radius is not rebuildable (it could not be meshed either), so
/// the interesting chunks must sit a couple of rings in.
[[nodiscard]] StreamingConfig lodConfig()
{
    StreamingConfig config;
    config.loadRadius              = 9;
    config.unloadPadding           = 12;  // nothing retires during a walk test
    config.verticalRadius          = 1;
    config.unloadGraceFrames       = 0;
    config.maxScheduledPerUpdate   = 4096;
    config.maxGenerateJobsInFlight = 4096;
    config.maxMeshJobsInFlight     = 4096;
    config.maxUnloadsPerUpdate     = 4096;

    config.maxLodTransitionsPerUpdate = 64;
    config.maxLodJobsInFlight         = 64;
    // 2/5/8 with a hysteresis of 1 is the tightest band layout ChunkManager will
    // accept: a band must be hysteresis + 2 chunks wide or a demoted chunk can
    // never be promoted back (see setLodPolicy). Squeezing the bands together
    // any further gets the hysteresis clamped and the test silently loses the
    // property it is trying to check.
    config.lod.bandStart[0] = 2;
    config.lod.bandStart[1] = 5;
    config.lod.bandStart[2] = 8;
    config.lod.hysteresis   = 1;
    return config;
}

/// The section every test looks at: the one the view sits in.
constexpr std::int32_t kSectionY = kSeaLevel / kChunkSize;

}  // namespace

// ---------------------------------------------------------------------------
//  Selection: the policy, and the integer distance fed to it
// ---------------------------------------------------------------------------

TEST_CASE("the LOD policy picks the documented level for each distance", "[lod]")
{
    const LodPolicy policy;  // bandStart {5, 9, 14}

    // A band's start distance still belongs to the FINER level; the next level
    // begins one chunk beyond it.
    CHECK(policy.levelFor(0) == 0);
    CHECK(policy.levelFor(5) == 0);
    CHECK(policy.levelFor(6) == 1);
    CHECK(policy.levelFor(9) == 1);
    CHECK(policy.levelFor(10) == 2);
    CHECK(policy.levelFor(14) == 2);
    CHECK(policy.levelFor(15) == 3);
    CHECK(policy.levelFor(100000) == kLodMax);

    SECTION("disabling the policy pins everything to full resolution")
    {
        LodPolicy off;
        off.enabled = false;
        CHECK(off.levelFor(0) == kLodFull);
        CHECK(off.levelFor(1000) == kLodFull);
        CHECK(off.levelFor(1000, kLodMax) == kLodFull);
    }
}

TEST_CASE("horizontal chunk distance is an exact integer square root", "[lod]")
{
    // Exactness matters: a distance that rounds differently on two machines
    // would put the same chunk in different LOD bands for the same seed.
    CHECK(ChunkManager::horizontalDistanceChunks(ChunkPos{0, 0, 0}, ChunkPos{0, 0, 0}) == 0);
    CHECK(ChunkManager::horizontalDistanceChunks(ChunkPos{0, 0, 0}, ChunkPos{3, 0, 4}) == 5);
    CHECK(ChunkManager::horizontalDistanceChunks(ChunkPos{0, 0, 0}, ChunkPos{-3, 0, -4}) == 5);
    // 1.41 floors to 1, 2.83 to 2: the cylinder is measured, not the square.
    CHECK(ChunkManager::horizontalDistanceChunks(ChunkPos{0, 0, 0}, ChunkPos{1, 0, 1}) == 1);
    CHECK(ChunkManager::horizontalDistanceChunks(ChunkPos{0, 0, 0}, ChunkPos{2, 0, 2}) == 2);
    // The vertical axis is deliberately ignored.
    CHECK(ChunkManager::horizontalDistanceChunks(ChunkPos{0, 0, 0}, ChunkPos{5, 7, 0}) == 5);
}

TEST_CASE("hysteresis makes the resident overload sticky", "[lod]")
{
    LodPolicy policy;
    policy.bandStart[0] = 2;
    policy.bandStart[1] = 5;
    policy.bandStart[2] = 8;
    policy.hysteresis   = 1;

    // Without a current level, the plain bands apply.
    CHECK(policy.levelFor(3) == 1);
    CHECK(policy.levelFor(6) == 2);

    // A resident level-0 chunk drifting outward holds on until it is a full
    // hysteresis band past the edge it is crossing.
    CHECK(policy.levelFor(2, kLodFull) == kLodFull);
    CHECK(policy.levelFor(3, kLodFull) == kLodFull);
    CHECK(policy.levelFor(4, kLodFull) == 1);

    // ... and coming back in, it holds its coarse level until it is a full band
    // inside. That asymmetry is the whole point: the two thresholds never meet,
    // so no single step can flip a chunk back and forth.
    CHECK(policy.levelFor(4, 1) == 1);
    CHECK(policy.levelFor(3, 1) == 1);
    CHECK(policy.levelFor(2, 1) == 1);
    CHECK(policy.levelFor(1, 1) == 1);
    CHECK(policy.levelFor(0, 1) == kLodFull);

    // A band narrower than hysteresis + 2 is a trap: the promote arm needs
    // `distance < bandStart[target] - hysteresis` while `distance >
    // bandStart[target - 1]` is what selects that target at all, so with the
    // shipped bands squeezed to 1/2/3 a demoted chunk can NEVER come back.
    // ChunkManager::setLodPolicy clamps the hysteresis rather than let a world
    // quietly lose its detail; the raw policy has no such guard.
    LodPolicy tooTight;
    tooTight.bandStart[0] = 1;
    tooTight.bandStart[1] = 2;
    tooTight.bandStart[2] = 3;
    tooTight.hysteresis   = 2;
    CHECK(tooTight.levelFor(0, kLodMax) == kLodMax);  // stuck at the coarsest level
}

// ---------------------------------------------------------------------------
//  Residency: which level chunks are actually built at
// ---------------------------------------------------------------------------

TEST_CASE("a chunk is generated at the level its distance implies", "[lod][streaming]")
{
    LodFixture fixture(lodConfig());
    fixture.settle(viewAtChunk(0, 0));

    const World&    world  = fixture.world;
    const LodPolicy policy = world.lodPolicy();
    const ChunkPos  centre = world.chunks().centre();
    REQUIRE(centre.y == kSectionY);

    // Nothing has moved since the chunks were born, so every one of them must be
    // at exactly the level the plain (non-sticky) policy names for its distance.
    std::array<std::size_t, kLodCount> expected{};
    std::size_t                        counted = 0;
    world.chunks().forEachChunk([&](const ChunkPos& position, const ChunkPtr& chunk) {
        const std::int32_t distance =
            ChunkManager::horizontalDistanceChunks(centre, position);
        INFO("chunk " << position.x << ", " << position.y << ", " << position.z
                      << " at distance " << distance);
        CHECK(chunk->lod() == policy.levelFor(distance));
        expected[chunk->lod()] += 1;
        ++counted;
    });

    const WorldStats stats = world.stats();
    CHECK(stats.loadedChunks == counted);
    for (LodLevel level = 0; level < kLodCount; ++level) {
        INFO("level " << static_cast<int>(level));
        CHECK(stats.chunksByLod[level] == expected[level]);
    }

    // The per-level counts must add up to the resident set, or the overlay's LOD
    // distribution silently loses chunks.
    const std::size_t summed =
        stats.chunksByLod[0] + stats.chunksByLod[1] + stats.chunksByLod[2] + stats.chunksByLod[3];
    CHECK(summed == stats.loadedChunks);

    // ... and the test is not vacuous: with bands at 1/2/3 inside a radius of 6,
    // every level is populated.
    for (LodLevel level = 0; level < kLodCount; ++level) {
        INFO("level " << static_cast<int>(level));
        CHECK(stats.chunksByLod[level] > 0);
    }

    // Hand-checked spot values against bands 2/5/8.
    CHECK(world.chunkAt(ChunkPos{0, kSectionY, 0})->lod() == 0);
    CHECK(world.chunkAt(ChunkPos{2, kSectionY, 0})->lod() == 0);  // on the edge: still finest
    CHECK(world.chunkAt(ChunkPos{3, kSectionY, 0})->lod() == 1);
    CHECK(world.chunkAt(ChunkPos{5, kSectionY, 0})->lod() == 1);
    CHECK(world.chunkAt(ChunkPos{6, kSectionY, 0})->lod() == 2);
    CHECK(world.chunkAt(ChunkPos{8, kSectionY, 0})->lod() == 2);
    CHECK(world.chunkAt(ChunkPos{9, kSectionY, 0})->lod() == 3);
    CHECK(world.lodAt(BlockPos{9 * kChunkSize + 1, kSeaLevel, 1}) == 3);

    CHECK(fixture.incompleteSnapshots.load() == 0);
}

TEST_CASE("a hysteresis wider than a band is clamped rather than trapping chunks",
          "[lod][streaming]")
{
    StreamingConfig config   = lodConfig();
    config.lod.bandStart[0]  = 1;
    config.lod.bandStart[1]  = 2;
    config.lod.bandStart[2]  = 3;
    config.lod.hysteresis    = 4;
    LodFixture fixture(config);

    // Bands one chunk wide leave no room for any hysteresis at all.
    CHECK(fixture.world.lodPolicy().hysteresis == 0);
    // ... and every level is reachable again in both directions.
    CHECK(fixture.world.lodPolicy().levelFor(0, kLodMax) == kLodFull);

    SECTION("an out-of-order band table is sorted rather than mis-selecting levels")
    {
        StreamingConfig broken  = lodConfig();
        broken.lod.bandStart[0] = 9;
        broken.lod.bandStart[1] = 4;  // inverted
        broken.lod.bandStart[2] = 20;
        LodFixture other(broken);
        const LodPolicy& fixed = other.world.lodPolicy();
        CHECK(fixed.bandStart[0] <= fixed.bandStart[1]);
        CHECK(fixed.bandStart[1] <= fixed.bandStart[2]);
    }
}

TEST_CASE("pinning the policy to level 0 reproduces the pre-LOD world", "[lod][streaming]")
{
    StreamingConfig config = lodConfig();
    config.lod.enabled     = false;
    LodFixture fixture(config);
    fixture.settle(viewAtChunk(0, 0));

    const WorldStats stats = fixture.world.stats();
    CHECK(stats.chunksByLod[0] == stats.loadedChunks);
    CHECK(stats.lodTransitions == 0);
}

// ---------------------------------------------------------------------------
//  Transitions
// ---------------------------------------------------------------------------

TEST_CASE("walking away demotes a chunk and walking back promotes it",
          "[lod][streaming]")
{
    LodFixture fixture(lodConfig());
    World&     world = fixture.world;

    fixture.settle(viewAtChunk(0, 0));

    const ChunkPos target{3, kSectionY, 0};
    REQUIRE(world.chunkAt(target) != nullptr);
    REQUIRE(world.chunkAt(target)->lod() == 1);
    REQUIRE(world.chunkAt(target)->state() == ChunkState::Ready);

    // Four chunks west: `target` is now seven away, one past the level-2 edge
    // plus the hysteresis margin.
    fixture.settle(viewAtChunk(-4, 0));
    REQUIRE(world.chunkAt(target) != nullptr);
    CHECK(world.chunkAt(target)->lod() == 2);
    CHECK(world.stats().lodTransitions > 0);

    // Coming back promotes it again.
    fixture.settle(viewAtChunk(0, 0));
    REQUIRE(world.chunkAt(target) != nullptr);
    CHECK(world.chunkAt(target)->lod() == 1);

    CHECK(fixture.incompleteSnapshots.load() == 0);
    CHECK(world.stats().lodTransitionsDropped == 0);
}

TEST_CASE("a chunk keeps its old mesh until the new level is ready",
          "[lod][streaming][threading]")
{
    LodFixture fixture(lodConfig());
    World&     world = fixture.world;

    fixture.settle(viewAtChunk(0, 0));

    const ChunkPos target{3, kSectionY, 0};
    REQUIRE(world.chunkAt(target) != nullptr);
    const ChunkPtr      oldChunk = world.chunkAt(target);
    const LodLevel      oldLevel = oldChunk->lod();
    REQUIRE(oldLevel == 1);
    REQUIRE(fixture.gpu.count(target) == 1);
    const GpuMesh oldMesh = fixture.gpu.at(target);
    REQUIRE(oldMesh.level == oldLevel);

    // Hold the rebuild's mesh job open at the moment it has the new voxels but
    // has not yet produced geometry - the exact window in which a naive
    // implementation has already thrown the old chunk away.
    fixture.armGate(target, 2);
    const StreamingView away = viewAtChunk(-4, 0);
    REQUIRE(fixture.pumpUntilGated(away));

    // MID-TRANSITION. Everything the renderer and the player can see must be
    // unchanged: the old chunk is still the resident one, still at its old
    // level, and its old mesh is still on the GPU.
    {
        const ChunkPtr current = world.chunkAt(target);
        REQUIRE(current != nullptr);
        CHECK(current.get() == oldChunk.get());
        CHECK(current->lod() == oldLevel);
        CHECK(current->state() == ChunkState::Meshing);

        REQUIRE(fixture.gpu.count(target) == 1);
        CHECK(fixture.gpu.at(target).level == oldLevel);
        CHECK(fixture.gpu.at(target).sequence == oldMesh.sequence);
        // The mesh must not have been dropped and re-uploaded; a release here is
        // exactly the frame the chunk vanishes.
        CHECK(fixture.releases[target] == 0);
    }

    fixture.openGate();
    fixture.settle(away);

    // AFTER. A different Chunk object is resident, at the new level, and the
    // renderer's mesh for the position was replaced rather than removed.
    const ChunkPtr newChunk = world.chunkAt(target);
    REQUIRE(newChunk != nullptr);
    CHECK(newChunk.get() != oldChunk.get());
    CHECK(newChunk->lod() == 2);
    CHECK(newChunk->state() == ChunkState::Ready);

    REQUIRE(fixture.gpu.count(target) == 1);
    CHECK(fixture.gpu.at(target).level == 2);
    CHECK(fixture.gpu.at(target).sequence > oldMesh.sequence);
    CHECK(fixture.releases[target] == 0);

    // The old object is retired, not leaked back into the world.
    CHECK(oldChunk->state() == ChunkState::Unloading);
}

TEST_CASE("no chunk is rebuilt at two levels at once", "[lod][streaming][threading]")
{
    StreamingConfig config = lodConfig();
    // Small budgets so the same chunk is still a candidate on the next few
    // updates while its rebuild is queued - the window a second, parallel
    // "busy" flag would fail to close.
    config.maxScheduledPerUpdate      = 6;
    config.maxLodTransitionsPerUpdate = 3;
    LodFixture fixture(config, 4);

    // Teleport around without ever letting the pipeline settle, so schedule
    // decisions are made while generate, mesh and rebuild jobs are all running.
    const std::array<StreamingView, 4> stops = {viewAtChunk(0, 0), viewAtChunk(3, 0),
                                                viewAtChunk(3, 3), viewAtChunk(0, 2)};
    for (int pass = 0; pass < 120; ++pass) {
        fixture.world.update(stops[static_cast<std::size_t>(pass) % stops.size()]);
        fixture.jobs.mainThreadQueue().drain(std::chrono::microseconds{300});
    }
    REQUIRE(fixture.world.chunks().waitForPendingJobs(std::chrono::milliseconds{20000}));
    fixture.jobs.mainThreadQueue().drainAll();

    {
        std::lock_guard<std::mutex> lock(fixture.meshMutex);
        // One mesh job per position at a time, whether it is an ordinary remesh
        // or a LOD rebuild: they contend for the same Ready -> Meshing CAS.
        CHECK(fixture.maxConcurrentPerPosition == 1);
        for (const auto& [position, depth] : fixture.meshInFlight) {
            INFO("chunk " << position.x << ", " << position.y << ", " << position.z);
            CHECK(depth == 0);
        }
    }

    CHECK(fixture.incompleteSnapshots.load() == 0);
    CHECK(fixture.world.stats().lodJobsInFlight == 0);

    // Every resident chunk ended in a coherent state - nothing stranded in
    // Meshing because a rebuild lost a race and never restored it.
    fixture.settle(stops[0]);
    fixture.world.chunks().forEachChunk([](const ChunkPos& position, const ChunkPtr& chunk) {
        INFO("chunk " << position.x << ", " << position.y << ", " << position.z);
        CHECK_FALSE(chunk->isBusy());
    });
}

TEST_CASE("LOD transitions are capped per update", "[lod][streaming]")
{
    StreamingConfig config            = lodConfig();
    config.maxLodTransitionsPerUpdate = 2;
    LodFixture fixture(config);

    fixture.settle(viewAtChunk(0, 0));
    const std::uint64_t before = fixture.world.stats().lodTransitions;

    // A four-chunk jump makes a large fraction of the resident set want a new
    // level at once. Without the cap the whole ring would rebuild in this single
    // update, which is the stall the budget exists to prevent.
    fixture.world.update(viewAtChunk(-4, 0));
    REQUIRE(fixture.world.chunks().waitForPendingJobs(std::chrono::milliseconds{20000}));
    fixture.jobs.mainThreadQueue().drainAll();

    const std::uint64_t after = fixture.world.stats().lodTransitions;
    CHECK(after > before);  // the jump really did want transitions
    CHECK(after - before <= config.maxLodTransitionsPerUpdate);
}

TEST_CASE("hysteresis stops a pacing player from rebuilding the same ring",
          "[lod][streaming]")
{
    // A one-chunk step moves every band edge's ring across that edge and back.
    SECTION("a one-chunk pace causes no rebuilds at all")
    {
        LodFixture fixture(lodConfig());
        fixture.settle(viewAtChunk(0, 0));
        fixture.settle(viewAtChunk(1, 0));
        const std::uint64_t settled = fixture.world.stats().lodTransitions;

        for (int step = 0; step < 8; ++step) {
            fixture.settle(viewAtChunk(step % 2 == 0 ? 0 : 1, 0), 4);
        }

        // Not merely "bounded": with a hysteresis of two chunks, a one-chunk
        // step can never carry a chunk far enough past an edge to justify a
        // rebuild, in either direction.
        CHECK(fixture.world.stats().lodTransitions == settled);
    }

    SECTION("a three-chunk pace settles, and only because of the hysteresis")
    {
        // Same walk, run twice: once with the hysteresis the policy ships with,
        // once with none. The count that keeps climbing is the bug the band
        // exists to prevent.
        const auto walk = [](std::int32_t hysteresis) {
            StreamingConfig config = lodConfig();
            config.lod.hysteresis  = hysteresis;
            LodFixture fixture(config);

            // Two warm-up round trips: chunks born at a band edge are allowed
            // one legitimate transition each, and this exhausts them.
            for (int step = 0; step < 4; ++step) {
                fixture.settle(viewAtChunk(step % 2 == 0 ? 0 : 3, 0), 8);
            }
            const std::uint64_t warm = fixture.world.stats().lodTransitions;

            for (int step = 0; step < 8; ++step) {
                fixture.settle(viewAtChunk(step % 2 == 0 ? 0 : 3, 0), 8);
            }
            return fixture.world.stats().lodTransitions - warm;
        };

        const std::uint64_t withBand    = walk(1);
        const std::uint64_t withoutBand = walk(0);

        INFO("steady-state rebuilds: hysteresis 1 -> " << withBand << ", hysteresis 0 -> "
                                                       << withoutBand);
        // A three-chunk step cannot clear the band in both directions, so the
        // walk reaches a fixed point and stays there.
        CHECK(withBand == 0);
        // With no band it never does: every round trip pays for the same ring.
        CHECK(withoutBand > 0);
    }
}

// ---------------------------------------------------------------------------
//  Sub-voxel edits
// ---------------------------------------------------------------------------

TEST_CASE("a sub-voxel edit on a chunk border marks the neighbour dirty", "[world][subvoxel]")
{
    StreamingConfig config = lodConfig();
    config.lod.enabled     = false;  // keep every chunk at full resolution
    LodFixture fixture(config);
    World&     world = fixture.world;
    fixture.settle(viewAtChunk(0, 0));

    const ChunkPos centre{0, kSectionY, 0};
    const ChunkPos westward{-1, kSectionY, 0};
    const ChunkPos eastward{1, kSectionY, 0};

    const ChunkPtr centreChunk = world.chunkAt(centre);
    const ChunkPtr westChunk   = world.chunkAt(westward);
    const ChunkPtr eastChunk   = world.chunkAt(eastward);
    REQUIRE(centreChunk != nullptr);
    REQUIRE(westChunk != nullptr);
    REQUIRE(eastChunk != nullptr);

    // Solid stone, so there is something to carve.
    // Both inside section kSectionY and, apart from `border`'s x, away from every
    // chunk face, so each SECTION below isolates exactly one seam rule.
    const BlockPos border{0, kSeaLevel + 4, 5};
    const BlockPos interior{16, kSeaLevel + 4, 16};
    REQUIRE(world.getBlock(border) == blocks::Stone);
    REQUIRE(world.getBlock(interior) == blocks::Stone);

    SECTION("carving local x == 0 dirties the chunk to the west, not the east")
    {
        centreChunk->clearRemeshFlag();
        westChunk->clearRemeshFlag();
        eastChunk->clearRemeshFlag();

        REQUIRE(world.breakSubVoxel(border, subVoxelIndex(0, 0, 0)) == EditResult::Applied);

        CHECK(centreChunk->needsRemesh());
        CHECK(westChunk->needsRemesh());
        CHECK_FALSE(eastChunk->needsRemesh());
        // The neighbour's voxels did not change, so it must not be queued for a
        // save just because its mesh went stale.
        CHECK_FALSE(westChunk->needsSave());
        CHECK(centreChunk->needsSave());

        // The block is no longer whole, which is what stops the neighbour from
        // culling its face against it.
        CHECK_FALSE(world.isBlockWhole(border));
        CHECK(world.subVoxelsAt(border) != nullptr);
        CHECK(centreChunk->hasSubVoxelDamage());
    }

    SECTION("an interior carve dirties nobody else")
    {
        centreChunk->clearRemeshFlag();
        westChunk->clearRemeshFlag();
        eastChunk->clearRemeshFlag();

        REQUIRE(world.breakSubVoxel(interior, subVoxelIndex(1, 1, 1)) == EditResult::Applied);

        CHECK(centreChunk->needsRemesh());
        CHECK_FALSE(westChunk->needsRemesh());
        CHECK_FALSE(eastChunk->needsRemesh());
    }

    SECTION("a corner carve dirties the diagonal neighbours too, for ambient occlusion")
    {
        const ChunkPtr diagonal = world.chunkAt(ChunkPos{-1, kSectionY, -1});
        const ChunkPtr below    = world.chunkAt(ChunkPos{0, kSectionY - 1, 0});
        REQUIRE(diagonal != nullptr);
        REQUIRE(below != nullptr);
        diagonal->clearRemeshFlag();
        below->clearRemeshFlag();
        westChunk->clearRemeshFlag();

        // Local (0, 0, 0) of the centre chunk.
        const BlockPos corner{0, kSectionY * kChunkSize, 0};
        REQUIRE(world.breakSubVoxel(corner, subVoxelIndex(0, 0, 0)) == EditResult::Applied);

        CHECK(westChunk->needsRemesh());
        CHECK(diagonal->needsRemesh());
        CHECK(below->needsRemesh());
    }

    SECTION("restoring the last sub-voxel puts the block back and leaves the store empty")
    {
        REQUIRE(world.breakSubVoxel(interior, subVoxelIndex(2, 3, 4)) == EditResult::Applied);
        REQUIRE_FALSE(world.isBlockWhole(interior));

        REQUIRE(world.restoreSubVoxel(interior, subVoxelIndex(2, 3, 4), blocks::Stone) ==
                EditResult::Applied);
        CHECK(world.isBlockWhole(interior));
        CHECK(world.subVoxelsAt(interior) == nullptr);
        CHECK(world.getBlock(interior) == blocks::Stone);
    }

    SECTION("an unloaded or out-of-world carve is refused, not crashed")
    {
        CHECK(world.breakSubVoxel(BlockPos{0, kWorldMaxY + 1, 0}, 0) == EditResult::OutOfBounds);
        CHECK(world.breakSubVoxel(BlockPos{100000, kSeaLevel, 100000}, 0) ==
              EditResult::NotLoaded);
    }
}

TEST_CASE("a sub-voxel edit against a busy chunk is deferred, not lost", "[world][subvoxel]")
{
    StreamingConfig config = lodConfig();
    config.lod.enabled     = false;
    LodFixture fixture(config);

    const BlockPos target{4, kSeaLevel + 4, 4};

    // The first update only creates the chunks and queues generation, so the
    // target chunk is Empty or Generating right now.
    fixture.world.update(viewAtChunk(0, 0));
    const EditResult result = fixture.world.breakSubVoxel(target, subVoxelIndex(0, 0, 0));
    REQUIRE((result == EditResult::Deferred || result == EditResult::Applied ||
             result == EditResult::NotLoaded));

    if (result == EditResult::Deferred) {
        CHECK(fixture.world.deferredEditCount() == 1);
        CHECK(fixture.world.stats().deferredEdits == 1);

        fixture.settle(viewAtChunk(0, 0));
        CHECK(fixture.world.deferredEditCount() == 0);
        CHECK_FALSE(fixture.world.isBlockWhole(target));
    }
}
