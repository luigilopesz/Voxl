#include "world/ChunkManager.hpp"

#include "core/Log.hpp"

#include <algorithm>
#include <utility>

#include <glm/geometric.hpp>

namespace voxl {
namespace {

/// Distance below which the view-direction bonus is meaningless because the
/// player is inside the chunk.
constexpr float kMinPriorityDistance = 1e-3f;

/// Chunks this close (in blocks) block the visible frame, so their jobs go in
/// the High queue ahead of ordinary streaming.
constexpr float kHighPriorityDistance = static_cast<float>(2 * kChunkSize);

[[nodiscard]] glm::vec3 chunkCentreWorld(const ChunkPos& pos) noexcept
{
    const BlockPos origin = pos.originBlock();
    const float    half   = static_cast<float>(kChunkSize) * 0.5f;
    return glm::vec3{static_cast<float>(origin.x) + half, static_cast<float>(origin.y) + half,
                     static_cast<float>(origin.z) + half};
}

/// Total ordering on candidates. Score alone is not enough: two chunks at the
/// same distance must be dispatched in a fixed order, or the same seed and the
/// same walk produce different job orders on different runs, which makes a
/// streaming bug unreproducible.
[[nodiscard]] bool betterCandidate(const ChunkPos& a, const ChunkPos& b) noexcept
{
    if (a.x != b.x) {
        return a.x < b.x;
    }
    if (a.y != b.y) {
        return a.y < b.y;
    }
    return a.z < b.z;
}

}  // namespace

ChunkManager::ChunkManager(JobSystem& jobs, const StreamingConfig& config) : m_jobs(jobs)
{
    setConfig(config);
}

ChunkManager::~ChunkManager()
{
    // Order matters. First let the in-flight jobs finish, because they hold a
    // raw `this` and are mid-generate or mid-mesh; only then clear the liveness
    // flag, which is what stops the upload closures still sitting in the
    // main-thread queue from touching a destroyed manager.
    if (!waitForPendingJobs(std::chrono::milliseconds{5000})) {
        VOXL_LOG_WARN("ChunkManager destroyed with {} job(s) still in flight; "
                      "was the JobSystem cancelled?",
                      jobsInFlight());
    }
    m_alive->store(false, std::memory_order_release);
}

// ------------------------------------------------------------------ wiring --

void ChunkManager::setGenerator(ChunkGenerateFn generator)
{
    VOXL_CHECK(jobsInFlight() == 0, "setGenerator() while jobs are in flight");
    m_generate = std::move(generator);
}

void ChunkManager::setMesher(ChunkMeshFn mesher)
{
    VOXL_CHECK(jobsInFlight() == 0, "setMesher() while jobs are in flight");
    m_mesh = std::move(mesher);
}

void ChunkManager::setMeshReleaser(ChunkReleaseFn release)
{
    m_release = std::move(release);
}

void ChunkManager::setRetireHook(ChunkRetireFn retire)
{
    m_retire = std::move(retire);
}

void ChunkManager::setConfig(const StreamingConfig& config)
{
    m_config               = config;
    m_config.loadRadius    = std::max(0, m_config.loadRadius);
    m_config.unloadPadding = std::max(1, m_config.unloadPadding);
    m_config.verticalRadius = std::clamp(m_config.verticalRadius, 0, kWorldSectionCount);
    m_config.viewBias      = std::clamp(m_config.viewBias, 0.0f, 0.95f);
    m_config.maxScheduledPerUpdate = std::max<std::size_t>(1, m_config.maxScheduledPerUpdate);
}

// --------------------------------------------------------------- residency --

ChunkPtr ChunkManager::find(const ChunkPos& position) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    const auto it = m_chunks.find(position);
    return it != m_chunks.end() ? it->second.chunk : nullptr;
}

ConstChunkPtr ChunkManager::findReadable(const ChunkPos& position) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    const auto it = m_chunks.find(position);
    if (it == m_chunks.end() || !it->second.chunk->hasVoxels()) {
        return nullptr;
    }
    return it->second.chunk;
}

bool ChunkManager::isResident(const ChunkPos& position) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    return m_chunks.find(position) != m_chunks.end();
}

std::size_t ChunkManager::residentCount() const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    return m_chunks.size();
}

ChunkNeighbourhood ChunkManager::captureNeighbourhood(const ChunkPos& centre) const
{
    // One critical section for all 27 lookups; the lambda must therefore use the
    // raw map rather than findReadable(), which would try to re-lock.
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    return voxl::captureNeighbourhood(centre, [this](const ChunkPos& pos) -> ConstChunkPtr {
        const auto it = m_chunks.find(pos);
        if (it == m_chunks.end() || !it->second.chunk->hasVoxels()) {
            return nullptr;
        }
        return it->second.chunk;
    });
}

bool ChunkManager::isStillResident(const ChunkPtr& chunk) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    const auto it = m_chunks.find(chunk->position());
    return it != m_chunks.end() && it->second.chunk.get() == chunk.get();
}

// ----------------------------------------------------------------- ranges --

std::int64_t ChunkManager::horizontalDistanceSq(const ChunkPos& a, const ChunkPos& b) noexcept
{
    const std::int64_t dx = static_cast<std::int64_t>(a.x) - b.x;
    const std::int64_t dz = static_cast<std::int64_t>(a.z) - b.z;
    return dx * dx + dz * dz;
}

bool ChunkManager::inLoadRange(const ChunkPos& centre, const ChunkPos& chunk) const noexcept
{
    if (chunk.y < 0 || chunk.y >= kWorldSectionCount) {
        return false;
    }
    if (std::abs(chunk.y - centre.y) > m_config.verticalRadius) {
        return false;
    }
    const std::int64_t radius = m_config.loadRadius;
    return horizontalDistanceSq(centre, chunk) <= radius * radius;
}

bool ChunkManager::inKeepRange(const ChunkPos& centre, const ChunkPos& chunk) const noexcept
{
    if (chunk.y < 0 || chunk.y >= kWorldSectionCount) {
        return false;
    }
    if (std::abs(chunk.y - centre.y) > m_config.verticalRadius + m_config.unloadPadding) {
        return false;
    }
    const std::int64_t radius = m_config.unloadRadius();
    return horizontalDistanceSq(centre, chunk) <= radius * radius;
}

bool ChunkManager::neighboursInLoadRange(const ChunkPos& centre, const ChunkPos& chunk) const noexcept
{
    for (std::int32_t dy = -1; dy <= 1; ++dy) {
        const std::int32_t sectionY = chunk.y + dy;
        if (sectionY < 0 || sectionY >= kWorldSectionCount) {
            continue;  // answered by the out-of-world rules; never needs a chunk
        }
        for (std::int32_t dz = -1; dz <= 1; ++dz) {
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                if (!inLoadRange(centre, ChunkPos{chunk.x + dx, sectionY, chunk.z + dz})) {
                    return false;
                }
            }
        }
    }
    return true;
}

float ChunkManager::priorityScore(const StreamingView& view, const ChunkPos& pos) const noexcept
{
    const glm::vec3 offset   = chunkCentreWorld(pos) - view.position;
    const float     distance = glm::length(offset);
    if (distance <= kMinPriorityDistance) {
        return 0.0f;
    }
    const float facing = glm::dot(offset / distance, view.forward);
    // A hole in front of the player is visible; a hole behind is not. Only the
    // forward half of the sphere gets the discount, so turning around does not
    // penalise the chunks that are already built.
    return distance * (1.0f - m_config.viewBias * std::max(0.0f, facing));
}

// ---------------------------------------------------------------- updating --

void ChunkManager::update(const StreamingView& view, std::uint64_t frameIndex)
{
    VOXL_CHECK(!m_jobs.onWorkerThread(), "ChunkManager::update() called from a worker thread");

    StreamingView normalised = view;
    const float   length     = glm::length(normalised.forward);
    normalised.forward = length > kMinPriorityDistance ? normalised.forward / length
                                                       : glm::vec3{0.0f, 0.0f, -1.0f};

    m_frameIndex             = frameIndex;
    const ChunkPos centre    = toChunkPos(worldToBlockPos(normalised.position));
    m_centre                 = centre;

    collect(normalised, centre, frameIndex);
    dispatch();
    retireDistant(centre, frameIndex);
}

void ChunkManager::collect(const StreamingView& view, const ChunkPos& centre,
                           std::uint64_t frameIndex)
{
    m_candidates.clear();

    const std::int32_t radius = m_config.loadRadius;
    const std::int64_t radiusSq = static_cast<std::int64_t>(radius) * radius;
    const std::int32_t minY = std::max(0, centre.y - m_config.verticalRadius);
    const std::int32_t maxY = std::min(kWorldSectionCount - 1, centre.y + m_config.verticalRadius);

    // Residency and candidate selection share one pass and one lock acquisition:
    // doing them separately meant two hash lookups per chunk per frame, which at
    // a radius of 8 is ~4000 lookups of pure overhead.
    std::unique_lock<std::shared_mutex> lock(m_mutex);

    for (std::int32_t dz = -radius; dz <= radius; ++dz) {
        for (std::int32_t dx = -radius; dx <= radius; ++dx) {
            const std::int64_t horizontalSq =
                static_cast<std::int64_t>(dx) * dx + static_cast<std::int64_t>(dz) * dz;
            if (horizontalSq > radiusSq) {
                continue;
            }

            for (std::int32_t y = minY; y <= maxY; ++y) {
                const ChunkPos position{centre.x + dx, y, centre.z + dz};

                auto it = m_chunks.find(position);
                if (it == m_chunks.end()) {
                    it = m_chunks.emplace(position, Slot{Chunk::create(position), 0, 0}).first;
                    m_chunksCreated.fetch_add(1, std::memory_order_relaxed);
                }

                const ChunkPtr& chunk = it->second.chunk;
                chunk->setLastTouchedFrame(frameIndex);

                const ChunkState state = chunk->state();
                WorkKind         kind  = WorkKind::Generate;
                if (state == ChunkState::Empty && m_generate) {
                    kind = WorkKind::Generate;
                } else if (m_mesh && state == ChunkState::Generated &&
                           neighboursInLoadRange(centre, position)) {
                    kind = WorkKind::Mesh;
                } else if (m_mesh && state == ChunkState::Ready && chunk->needsRemesh()) {
                    // A remesh is not pre-filtered: the chunk was meshable once,
                    // so its neighbours are almost certainly still resident, and
                    // an edit the player can see must not wait on the rim rule.
                    kind = WorkKind::Mesh;
                } else {
                    continue;
                }

                m_candidates.push_back(
                    Candidate{priorityScore(view, position), position, chunk, kind, state});
            }
        }
    }
}

void ChunkManager::dispatch()
{
    std::sort(m_candidates.begin(), m_candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  if (a.score != b.score) {
                      return a.score < b.score;
                  }
                  return betterCandidate(a.position, b.position);
              });

    std::size_t dispatched = 0;
    std::size_t skipped    = 0;

    for (const Candidate& candidate : m_candidates) {
        if (dispatched >= m_config.maxScheduledPerUpdate) {
            skipped = m_candidates.size() - dispatched;
            break;
        }

        // Within two chunks of the eye the player is looking at a hole right now.
        // A remesh of an already-Ready chunk is the player's own edit landing, so
        // it must never queue behind background streaming either.
        JobPriority priority = candidate.score <= kHighPriorityDistance ? JobPriority::High
                                                                       : JobPriority::Normal;
        if (candidate.kind == WorkKind::Mesh && candidate.state == ChunkState::Ready) {
            priority = JobPriority::High;
        }

        bool started = false;
        if (candidate.kind == WorkKind::Generate) {
            if (m_generateInFlight.load(std::memory_order_acquire) <
                m_config.maxGenerateJobsInFlight) {
                started = dispatchGenerate(candidate.chunk, priority);
            }
        } else if (m_meshInFlight.load(std::memory_order_acquire) < m_config.maxMeshJobsInFlight) {
            started = dispatchMesh(candidate, priority);
        }

        if (started) {
            ++dispatched;
        } else {
            ++skipped;
        }
    }

    m_queuedChunks.store(skipped, std::memory_order_relaxed);
}

bool ChunkManager::dispatchGenerate(const ChunkPtr& chunk, JobPriority priority)
{
    // The CAS is the entire duplicate-generation guard: two schedulers, or one
    // scheduler running twice before the job starts, produce exactly one winner.
    if (!chunk->tryTransition(ChunkState::Empty, ChunkState::Generating)) {
        return false;
    }

    noteJobStarted(WorkKind::Generate);
    m_jobs.submitDetached(priority, [this, alive = m_alive, chunk] {
        if (!alive->load(std::memory_order_acquire)) {
            return;  // the manager is gone; the chunk dies with our shared_ptr
        }

        m_generate(*chunk);

        // Only unloadAll() can steal a busy chunk's state. Losing the CAS means
        // the chunk was retired, so the terrain we just wrote is discarded.
        if (chunk->tryTransition(ChunkState::Generating, ChunkState::Generated)) {
            chunk->markDirty();
        }
        noteJobFinished(WorkKind::Generate);
    });
    return true;
}

bool ChunkManager::dispatchMesh(const Candidate& candidate, JobPriority priority)
{
    const ChunkPtr& chunk = candidate.chunk;

    // Border faces are culled against the neighbours, so an incomplete
    // neighbourhood bakes a wrong seam into the mesh - and because a mesh is only
    // rebuilt when something dirties the chunk, that seam would be permanent.
    // The 3x3x3 (not just the 6 faces) is required because ambient occlusion
    // samples the diagonals.
    ChunkNeighbourhood snapshot = captureNeighbourhood(candidate.position);
    if (!snapshot.complete()) {
        return false;
    }

    if (!chunk->tryTransition(candidate.state, ChunkState::Meshing)) {
        return false;
    }

    // Cleared before dispatch, not after: an edit that lands while the job runs
    // must re-set the flag and earn a follow-up mesh.
    chunk->clearRemeshFlag();
    const std::uint32_t version = chunk->contentVersion();

    noteJobStarted(WorkKind::Mesh);
    m_jobs.submitDetached(priority, [this, alive = m_alive, chunk, version,
                                     snapshot = std::move(snapshot)] {
        if (!alive->load(std::memory_order_acquire)) {
            return;
        }

        ChunkMeshUpload result = m_mesh(snapshot);

        if (!chunk->tryTransition(ChunkState::Meshing, ChunkState::Meshed)) {
            // Retired by unloadAll() while we worked; drop the geometry.
            m_meshesDropped.fetch_add(1, std::memory_order_relaxed);
            noteJobFinished(WorkKind::Mesh);
            return;
        }

        m_pendingUploads.fetch_add(1, std::memory_order_release);
        m_jobs.mainThreadQueue().push([this, alive, chunk, version, result = std::move(result)] {
            if (!alive->load(std::memory_order_acquire)) {
                return;
            }
            m_pendingUploads.fetch_sub(1, std::memory_order_release);

            if (!isStillResident(chunk)) {
                // Unloaded mid-job. Uploading now would attach GPU buffers to a
                // position that may already hold a different chunk object.
                m_meshesDropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            // Leave Meshed before uploading: if the upload throws, the chunk is
            // still in a schedulable state instead of stuck waiting forever.
            chunk->tryTransition(ChunkState::Meshed, ChunkState::Ready);

            if (chunk->contentVersion() != version) {
                // The voxels moved on. needsRemesh() is already set by whoever
                // bumped the version, so the follow-up mesh is guaranteed.
                m_meshesDropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            chunk->setMeshedVersion(version);
            {
                std::unique_lock<std::shared_mutex> lock(m_mutex);
                const auto it = m_chunks.find(chunk->position());
                if (it != m_chunks.end()) {
                    it->second.gpuBytes  = result.gpuBytes;
                    it->second.triangles = result.triangles;
                }
            }
            if (result.upload) {
                result.upload();
            }
            m_meshesUploaded.fetch_add(1, std::memory_order_relaxed);
        });

        noteJobFinished(WorkKind::Mesh);
    });
    return true;
}

void ChunkManager::retireDistant(const ChunkPos& centre, std::uint64_t frameIndex)
{
    // Callbacks run outside the lock: the release hook deletes GPU buffers and
    // the retire hook may queue a save, neither of which should block a worker's
    // residency lookup.
    std::vector<ChunkPtr> retired;
    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        for (auto it = m_chunks.begin(); it != m_chunks.end();) {
            if (retired.size() >= m_config.maxUnloadsPerUpdate) {
                break;
            }

            const ChunkPtr& chunk = it->second.chunk;
            if (inKeepRange(centre, it->first)) {
                ++it;
                continue;
            }

            const ChunkState state = chunk->state();
            if (isChunkBusy(state) || state == ChunkState::Unloading) {
                // A worker owns it. Retiring now is exactly the use-after-free
                // the state machine exists to prevent; it will be out of range
                // again next frame.
                ++it;
                continue;
            }

            const std::uint64_t touched = chunk->lastTouchedFrame();
            const std::uint64_t age     = frameIndex > touched ? frameIndex - touched : 0;
            if (age < m_config.unloadGraceFrames) {
                ++it;
                continue;
            }

            if (!chunk->tryTransition(state, ChunkState::Unloading)) {
                ++it;  // raced with a worker's own transition; try again later
                continue;
            }

            retired.push_back(chunk);
            it = m_chunks.erase(it);
        }
    }

    for (const ChunkPtr& chunk : retired) {
        if (m_retire) {
            m_retire(chunk);
        }
        if (m_release) {
            m_release(chunk->position());
        }
    }
    m_chunksUnloaded.fetch_add(retired.size(), std::memory_order_relaxed);
}

void ChunkManager::unloadAll()
{
    VOXL_CHECK(!m_jobs.onWorkerThread(), "ChunkManager::unloadAll() called from a worker thread");

    std::unordered_map<ChunkPos, Slot> retired;
    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        retired.swap(m_chunks);
    }

    for (auto& [position, slot] : retired) {
        // forceState rather than a CAS: this path must succeed even for a chunk a
        // worker is mid-generate on. The worker's transition then fails and it
        // drops its result, which is why every completion path checks.
        slot.chunk->forceState(ChunkState::Unloading);
        if (m_retire) {
            m_retire(slot.chunk);
        }
        if (m_release) {
            m_release(position);
        }
    }
    m_chunksUnloaded.fetch_add(retired.size(), std::memory_order_relaxed);
}

// ------------------------------------------------------------- job counters --

void ChunkManager::noteJobStarted(WorkKind kind) noexcept
{
    if (kind == WorkKind::Generate) {
        m_generateInFlight.fetch_add(1, std::memory_order_release);
    } else {
        m_meshInFlight.fetch_add(1, std::memory_order_release);
    }
}

void ChunkManager::noteJobFinished(WorkKind kind) noexcept
{
    if (kind == WorkKind::Generate) {
        m_generateInFlight.fetch_sub(1, std::memory_order_release);
    } else {
        m_meshInFlight.fetch_sub(1, std::memory_order_release);
    }

    // The empty critical section orders the decrement against a waiter sitting
    // between its predicate check and its sleep - the classic lost wakeup.
    {
        std::lock_guard<std::mutex> lock(m_jobMutex);
    }
    m_jobIdle.notify_all();
}

bool ChunkManager::waitForPendingJobs(std::chrono::milliseconds timeout)
{
    VOXL_CHECK(!m_jobs.onWorkerThread(), "waitForPendingJobs() called from a worker thread");

    std::unique_lock<std::mutex> lock(m_jobMutex);
    const auto idle = [this] { return jobsInFlight() == 0; };
    if (timeout.count() <= 0) {
        m_jobIdle.wait(lock, idle);
        return true;
    }
    return m_jobIdle.wait_for(lock, timeout, idle);
}

// ------------------------------------------------------------------- stats --

WorldStats ChunkManager::stats() const
{
    WorldStats out;
    {
        std::shared_lock<std::shared_mutex> lock(m_mutex);
        out.loadedChunks = m_chunks.size();
        for (const auto& [position, slot] : m_chunks) {
            static_cast<void>(position);
            out.chunksByState[static_cast<std::size_t>(slot.chunk->state())] += 1;
            out.cpuVoxelBytes += slot.chunk->memoryUsageBytes();
            out.gpuMeshBytes += slot.gpuBytes;
            out.triangles += slot.triangles;
        }
    }

    out.generatingChunks = out.chunksByState[static_cast<std::size_t>(ChunkState::Generating)];
    out.meshingChunks    = out.chunksByState[static_cast<std::size_t>(ChunkState::Meshing)];
    out.readyChunks      = out.chunksByState[static_cast<std::size_t>(ChunkState::Ready)];

    out.queuedChunks         = m_queuedChunks.load(std::memory_order_relaxed);
    out.visibleChunks        = m_visibleChunks.load(std::memory_order_relaxed);
    out.generateJobsInFlight = m_generateInFlight.load(std::memory_order_acquire);
    out.meshJobsInFlight     = m_meshInFlight.load(std::memory_order_acquire);
    out.pendingUploads       = m_pendingUploads.load(std::memory_order_acquire);

    out.chunksCreated  = m_chunksCreated.load(std::memory_order_relaxed);
    out.chunksUnloaded = m_chunksUnloaded.load(std::memory_order_relaxed);
    out.meshesUploaded = m_meshesUploaded.load(std::memory_order_relaxed);
    out.meshesDropped  = m_meshesDropped.load(std::memory_order_relaxed);

    out.centre       = m_centre;
    out.loadRadius   = m_config.loadRadius;
    out.unloadRadius = m_config.unloadRadius();
    return out;
}

WorldMemoryStats ChunkManager::memoryStats() const
{
    WorldMemoryStats out;
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    out.residentChunks = m_chunks.size();
    for (const auto& [position, slot] : m_chunks) {
        static_cast<void>(position);
        out.cpuVoxelBytes += slot.chunk->memoryUsageBytes();
        out.gpuMeshBytes += slot.gpuBytes;
    }
    return out;
}

}  // namespace voxl
