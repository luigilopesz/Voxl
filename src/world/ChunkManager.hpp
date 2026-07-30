#pragma once

// Chunk residency plus the streaming pipeline that fills it.
//
// The manager owns the resident chunk map and is the only place that decides
// which chunks exist, which are generated, and which are meshed. Terrain
// generation and meshing are injected as callables rather than compiled in:
// world/ deliberately does not include mesh/ or render/ (see
// docs/TECHNICAL_DESIGN.md section 2), so the mesh job hands back a type-erased
// "upload this on the main thread" closure instead of a ChunkMeshData.
//
// THREADING
// ---------
// Main thread only: update(), unloadAll(), setConfig(), the four setters,
//                   setVisibleChunkCount(), and everything the completion
//                   closures do.
// Any thread:       find(), findReadable(), isResident(), residentCount(),
//                   captureNeighbourhood(), forEachChunk(), stats().
//
// The chunk map is guarded by a shared_mutex. Reads are short (a hash lookup and
// a shared_ptr copy) and never overlap a mesh job's multi-millisecond scan,
// because a mesher works from a ChunkNeighbourhood snapshot instead of the map.

#include "core/JobSystem.hpp"
#include "world/BlockAccess.hpp"
#include "world/Chunk.hpp"
#include "world/VoxelTypes.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include <glm/vec3.hpp>

namespace voxl {

// ------------------------------------------------------------------ inputs --

/// Where the streamer should centre its work and which way the player looks.
///
/// A plain struct rather than a `const Camera&` because world/ must not depend
/// on render/. The application builds one per frame from its camera:
/// `world.update({camera.position(), camera.forward()})`.
struct StreamingView {
    glm::vec3 position{0.0f, static_cast<float>(kSeaLevel), 0.0f};
    /// Need not be normalised; the manager normalises defensively.
    glm::vec3 forward{0.0f, 0.0f, -1.0f};
};

/// Streaming policy. All radii are in chunks.
struct StreamingConfig {
    /// Chunks within this horizontal radius of the player are made resident.
    /// Membership is by squared distance, so the loaded region is a cylinder
    /// rather than a square: the corners of a square are 1.41x further away and
    /// cost 27% more chunks for terrain the player can barely see.
    std::int32_t loadRadius = 8;

    /// Extra radius a chunk must leave before it is retired. MUST be >= 1:
    /// unloading at exactly `loadRadius` makes a player walking along the
    /// boundary destroy and rebuild the same chunk every other frame, which
    /// costs a full generate + mesh each time.
    std::int32_t unloadPadding = 2;

    /// Vertical reach in sections. The default covers the whole world, so a
    /// column is loaded as a unit; smaller values exist for tests and for
    /// profiling how much of the cost is vertical.
    std::int32_t verticalRadius = kWorldSectionCount;

    /// A chunk must have been out of load range for this many frames before it
    /// is retired. Second line of defence behind `unloadPadding` for the case
    /// where the player is moving fast enough to cross the padding in one frame.
    std::uint64_t unloadGraceFrames = 30;

    /// Caps on work in flight. Without them, the first update after a teleport
    /// submits thousands of jobs at once, and the nearest chunk ends up queued
    /// behind hundreds of distant ones that were scheduled in the same burst.
    std::size_t maxGenerateJobsInFlight = 64;
    std::size_t maxMeshJobsInFlight     = 32;
    /// Jobs dispatched per update, across both kinds.
    std::size_t maxScheduledPerUpdate = 24;
    /// Chunks retired per update. Retiring is cheap but the release callback
    /// deletes GPU buffers, which is not.
    std::size_t maxUnloadsPerUpdate = 16;

    /// How much being in front of the player discounts a chunk's distance, in
    /// [0, 1). 0.5 means a chunk dead ahead is treated as half as far as one
    /// directly behind at the same range.
    float viewBias = 0.5f;

    [[nodiscard]] std::int32_t unloadRadius() const noexcept
    {
        return loadRadius + (unloadPadding > 0 ? unloadPadding : 1);
    }
};

// ----------------------------------------------------------------- results --

/// A finished CPU mesh, expressed without naming any mesh/ type.
struct ChunkMeshUpload {
    /// Performs the GPU upload. Invoked on the MAIN THREAD at most once, and
    /// only if the chunk is still resident and the mesh is not stale. Leave
    /// empty when the chunk produced no geometry at all.
    std::function<void()> upload;
    /// Reported to the debug overlay; the manager tracks the per-chunk figure so
    /// retiring a chunk subtracts the right amount.
    std::size_t gpuBytes  = 0;
    std::size_t triangles = 0;
};

/// Fills a newly created chunk with terrain.
///
/// Runs on a WORKER with exclusive ownership of the chunk (state Generating).
/// Must be a pure function of the chunk position and the world seed - see the
/// determinism rule in docs/TECHNICAL_DESIGN.md section 4.
using ChunkGenerateFn = std::function<void(Chunk&)>;

/// Builds geometry from an immutable neighbourhood snapshot. Runs on a WORKER.
/// The centre chunk is in state Meshing for the duration, so reads are race-free
/// without synchronisation.
using ChunkMeshFn = std::function<ChunkMeshUpload(const ChunkNeighbourhood&)>;

/// Releases whatever the renderer holds for a chunk that has left the resident
/// set. MAIN THREAD.
using ChunkReleaseFn = std::function<void(const ChunkPos&)>;

/// Last look at a chunk before it is dropped - the hook the save system will
/// use. The chunk is already in state Unloading. MAIN THREAD.
using ChunkRetireFn = std::function<void(const ChunkPtr&)>;

// ------------------------------------------------------------------- stats --

/// Everything the debug overlay needs. Sampled without a global snapshot, so
/// individual fields are accurate but need not agree with each other.
struct WorldStats {
    std::size_t loadedChunks = 0;
    std::array<std::size_t, kChunkStateCount> chunksByState{};
    std::size_t generatingChunks = 0;
    std::size_t meshingChunks    = 0;
    std::size_t readyChunks      = 0;
    /// Chunks that wanted work this update but did not fit in the budget.
    std::size_t queuedChunks = 0;
    /// Reported by the renderer after culling; the world does no culling itself.
    std::size_t visibleChunks = 0;

    std::size_t generateJobsInFlight = 0;
    std::size_t meshJobsInFlight     = 0;
    std::size_t pendingUploads       = 0;
    /// Edits waiting for their chunk to leave a worker-owned state. World only.
    std::size_t deferredEdits = 0;

    std::size_t cpuVoxelBytes = 0;
    std::size_t gpuMeshBytes  = 0;
    std::size_t triangles     = 0;

    std::uint64_t chunksCreated  = 0;
    std::uint64_t chunksUnloaded = 0;
    std::uint64_t meshesUploaded = 0;
    /// Meshes thrown away because the chunk was retired or edited mid-job. A
    /// number that climbs steadily means the streaming radii are thrashing.
    std::uint64_t meshesDropped = 0;

    ChunkPos     centre{};
    std::int32_t loadRadius   = 0;
    std::int32_t unloadRadius = 0;
};

/// The subset the frame loop passes to the overlay's memory panel.
struct WorldMemoryStats {
    std::size_t cpuVoxelBytes  = 0;
    std::size_t gpuMeshBytes   = 0;
    std::size_t residentChunks = 0;
};

// ------------------------------------------------------------ ChunkManager --

class ChunkManager {
public:
    /// `jobs` must outlive the manager. The manager only holds a reference
    /// because the application owns exactly one pool shared by every subsystem.
    explicit ChunkManager(JobSystem& jobs, const StreamingConfig& config = {});
    ~ChunkManager();

    ChunkManager(const ChunkManager&)            = delete;
    ChunkManager& operator=(const ChunkManager&) = delete;
    ChunkManager(ChunkManager&&)                 = delete;
    ChunkManager& operator=(ChunkManager&&)      = delete;

    // ---- wiring: call during start-up, before the first update() ----

    void setGenerator(ChunkGenerateFn generator);
    void setMesher(ChunkMeshFn mesher);
    void setMeshReleaser(ChunkReleaseFn release);
    void setRetireHook(ChunkRetireFn retire);

    [[nodiscard]] bool hasGenerator() const noexcept { return static_cast<bool>(m_generate); }
    [[nodiscard]] bool hasMesher() const noexcept { return static_cast<bool>(m_mesh); }

    [[nodiscard]] const StreamingConfig& config() const noexcept { return m_config; }
    /// Clamps the padding to at least one chunk; see StreamingConfig.
    void setConfig(const StreamingConfig& config);

    // ---- residency (any thread) ----

    [[nodiscard]] ChunkPtr find(const ChunkPos& position) const;
    /// Null unless the chunk is resident AND its voxels are safe to read.
    [[nodiscard]] ConstChunkPtr findReadable(const ChunkPos& position) const;
    [[nodiscard]] bool        isResident(const ChunkPos& position) const;
    [[nodiscard]] std::size_t residentCount() const;

    /// 3x3x3 shared_ptr snapshot around `centre`, taken in one critical section.
    /// Slots holding a chunk that is not yet readable are left null, so
    /// `ChunkNeighbourhood::complete()` answers "may this be meshed yet".
    [[nodiscard]] ChunkNeighbourhood captureNeighbourhood(const ChunkPos& centre) const;

    /// Visits every resident chunk as `fn(const ChunkPos&, const ChunkPtr&)`.
    /// The shared lock is held throughout: `fn` must not call back into the
    /// manager and must not block.
    template <typename Fn>
    void forEachChunk(Fn&& fn) const
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        for (const auto& [position, slot] : m_chunks) {
            fn(position, slot.chunk);
        }
    }

    // ---- streaming (main thread) ----

    /// One streaming step: make the load volume resident, dispatch the most
    /// valuable generate/mesh jobs within budget, retire what has drifted out of
    /// keep range. `frameIndex` must be monotonically increasing.
    void update(const StreamingView& view, std::uint64_t frameIndex);

    /// Retires everything, including chunks a worker is currently using - safe
    /// because in-flight jobs hold their own shared_ptr and every completion path
    /// re-checks residency before publishing.
    void unloadAll();

    /// Blocks until no generate or mesh job of this manager's is queued or
    /// running. Zero timeout waits forever. Returns false on timeout.
    bool waitForPendingJobs(std::chrono::milliseconds timeout = std::chrono::milliseconds{0});

    // ---- range predicates ----

    [[nodiscard]] static std::int64_t horizontalDistanceSq(const ChunkPos& a,
                                                           const ChunkPos& b) noexcept;
    [[nodiscard]] bool inLoadRange(const ChunkPos& centre, const ChunkPos& chunk) const noexcept;
    /// True while a chunk is close enough to keep. Strictly wider than
    /// `inLoadRange` - that gap is the anti-thrash hysteresis.
    [[nodiscard]] bool inKeepRange(const ChunkPos& centre, const ChunkPos& chunk) const noexcept;

    /// True when every in-world neighbour of `chunk` is inside the load radius,
    /// i.e. when the chunk can plausibly be meshed at all.
    ///
    /// Pure arithmetic, so it prunes the rim of the loaded region before the
    /// scheduler pays for a 27-entry snapshot. Without it, the outermost ring -
    /// which can never satisfy ChunkNeighbourhood::complete(), because its
    /// neighbours are outside the load radius by definition - would re-capture a
    /// doomed neighbourhood every single frame forever.
    [[nodiscard]] bool neighboursInLoadRange(const ChunkPos& centre,
                                             const ChunkPos& chunk) const noexcept;

    // ---- observation ----

    [[nodiscard]] WorldStats       stats() const;
    [[nodiscard]] WorldMemoryStats memoryStats() const;
    [[nodiscard]] ChunkPos         centre() const noexcept { return m_centre; }

    /// The renderer owns culling, so it reports the count it actually drew.
    void setVisibleChunkCount(std::size_t count) noexcept
    {
        m_visibleChunks.store(count, std::memory_order_relaxed);
    }

private:
    /// Map value: the chunk plus the main-thread-only accounting for the mesh
    /// the renderer currently holds for it.
    struct Slot {
        ChunkPtr    chunk;
        std::size_t gpuBytes  = 0;
        std::size_t triangles = 0;
    };

    enum class WorkKind : std::uint8_t { Generate, Mesh };

    struct Candidate {
        float      score = 0.0f;
        ChunkPos   position{};
        ChunkPtr   chunk;
        WorkKind   kind  = WorkKind::Generate;
        ChunkState state = ChunkState::Empty;
    };

    // update() steps
    void collect(const StreamingView& view, const ChunkPos& centre, std::uint64_t frameIndex);
    void dispatch();
    void retireDistant(const ChunkPos& centre, std::uint64_t frameIndex);

    bool dispatchGenerate(const ChunkPtr& chunk, JobPriority priority);
    bool dispatchMesh(const Candidate& candidate, JobPriority priority);

    /// True when `chunk` is still the object the map holds for its position.
    /// Comparing addresses rather than positions is the whole point: a retired
    /// chunk may already have been replaced by a fresh one at the same position,
    /// and publishing into that would resurrect dead work.
    [[nodiscard]] bool isStillResident(const ChunkPtr& chunk) const;

    [[nodiscard]] float priorityScore(const StreamingView& view, const ChunkPos& pos) const noexcept;

    void noteJobStarted(WorkKind kind) noexcept;
    void noteJobFinished(WorkKind kind) noexcept;

    [[nodiscard]] std::size_t jobsInFlight() const noexcept
    {
        return m_generateInFlight.load(std::memory_order_acquire) +
               m_meshInFlight.load(std::memory_order_acquire);
    }

    JobSystem&      m_jobs;
    StreamingConfig m_config;

    ChunkGenerateFn m_generate;
    ChunkMeshFn     m_mesh;
    ChunkReleaseFn  m_release;
    ChunkRetireFn   m_retire;

    mutable std::shared_mutex                    m_mutex;
    std::unordered_map<ChunkPos, Slot>           m_chunks;

    /// Reused across frames so the per-frame scheduling pass does not allocate.
    std::vector<Candidate> m_candidates;

    ChunkPos      m_centre{};
    std::uint64_t m_frameIndex = 0;

    /// Jobs and main-thread closures capture a copy and bail out if the manager
    /// died before they ran. Needed because the main-thread queue can still hold
    /// our upload closures after teardown, and the application cannot be forced
    /// to destroy the world before the job system.
    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);

    std::atomic<std::size_t> m_generateInFlight{0};
    std::atomic<std::size_t> m_meshInFlight{0};
    std::atomic<std::size_t> m_pendingUploads{0};
    std::atomic<std::size_t> m_queuedChunks{0};
    std::atomic<std::size_t> m_visibleChunks{0};

    std::atomic<std::uint64_t> m_chunksCreated{0};
    std::atomic<std::uint64_t> m_chunksUnloaded{0};
    std::atomic<std::uint64_t> m_meshesUploaded{0};
    std::atomic<std::uint64_t> m_meshesDropped{0};

    /// Only the sleep/wake handshake for waitForPendingJobs; the counters above
    /// are atomics so stats() never contends with a worker.
    mutable std::mutex      m_jobMutex;
    std::condition_variable m_jobIdle;
};

}  // namespace voxl
