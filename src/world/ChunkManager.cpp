#include "world/ChunkManager.hpp"

#include "core/Log.hpp"

#include <algorithm>
#include <cmath>
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

/// floor(sqrt(value)) for a non-negative value, exactly.
///
/// std::sqrt on a double is correctly rounded, so the seed is at most one off in
/// either direction for the magnitudes involved here (a radius of a few hundred
/// chunks); the two correction loops iterate at most once. Doing it this way
/// rather than trusting the rounded double keeps LOD selection a pure integer
/// function - see horizontalDistanceChunks().
[[nodiscard]] std::int32_t integerSqrt(std::int64_t value) noexcept
{
    if (value <= 0) {
        return 0;
    }
    auto root = static_cast<std::int64_t>(std::sqrt(static_cast<double>(value)));
    while (root > 0 && root * root > value) {
        --root;
    }
    while ((root + 1) * (root + 1) <= value) {
        ++root;
    }
    return static_cast<std::int32_t>(root);
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
    setLodPolicy(m_config.lod);
}

void ChunkManager::setLodPolicy(const LodPolicy& policy)
{
    m_config.lod = policy;

    // LodPolicy::levelFor walks bandStart outward and takes the last band it
    // clears, so a non-ascending table silently produces a level nobody asked
    // for. Repair it here rather than at every read.
    m_config.lod.hysteresis = std::max(0, m_config.lod.hysteresis);
    for (LodLevel i = 1; i < kLodMax; ++i) {
        m_config.lod.bandStart[i] =
            std::max(m_config.lod.bandStart[i], m_config.lod.bandStart[i - 1]);
    }

    // A hysteresis wider than a band makes that band UNREACHABLE FROM OUTSIDE,
    // which is a silent, permanent loss of detail rather than a wobble.
    //
    // The promote arm of LodPolicy::levelFor requires `distance < bandStart[L] -
    // hysteresis` while `distance > bandStart[L - 1]` is what makes L the target
    // in the first place. Those two can only both hold when the band is at least
    // hysteresis + 2 chunks wide (and, for level 0, when bandStart[0] exceeds
    // the hysteresis at all). Otherwise a chunk that has once been demoted can
    // never come back and the terrain the player walks into stays blocky
    // forever. The shipped defaults sit exactly on this limit, which is not a
    // coincidence - but a caller retuning the bands will not notice they have
    // crossed it, because nothing fails, things merely stop improving.
    std::int32_t maximum = m_config.lod.bandStart[0] - 1;
    for (LodLevel i = 1; i < kLodMax; ++i) {
        maximum = std::min(maximum, m_config.lod.bandStart[i] - m_config.lod.bandStart[i - 1] - 2);
    }
    maximum = std::max(0, maximum);
    if (m_config.lod.hysteresis > maximum) {
        VOXL_LOG_WARN("LOD hysteresis {} is wider than the narrowest band allows; clamping to {} "
                      "so demoted chunks can still be promoted back",
                      m_config.lod.hysteresis, maximum);
        m_config.lod.hysteresis = maximum;
    }
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

std::int32_t ChunkManager::horizontalDistanceChunks(const ChunkPos& a, const ChunkPos& b) noexcept
{
    return integerSqrt(horizontalDistanceSq(a, b));
}

LodLevel ChunkManager::desiredLod(const ChunkPos& centre, const ChunkPos& chunk) const noexcept
{
    return m_config.lod.levelFor(horizontalDistanceChunks(centre, chunk));
}

LodLevel ChunkManager::desiredLod(const ChunkPos& centre, const ChunkPos& chunk,
                                  LodLevel current) const noexcept
{
    return m_config.lod.levelFor(horizontalDistanceChunks(centre, chunk), current);
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

// --------------------------------------------------------- level of detail --

LodLevel ChunkManager::lodTargetFor(const ChunkPtr& chunk, ChunkState state,
                                    std::int32_t distanceInChunks) const noexcept
{
    const LodLevel current = chunk->lod();
    if (state != ChunkState::Ready || !m_generate || !m_mesh) {
        return current;
    }
    if (m_config.preserveEditedChunks && chunk->needsSave()) {
        // Regenerating would erase the edit; see StreamingConfig.
        return current;
    }
    // The two-argument overload is the one with the hysteresis band. Passing the
    // chunk's present level is what stops a player pacing across a band edge
    // from rebuilding the same ring every few steps.
    return m_config.lod.levelFor(distanceInChunks, current);
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

            // One integer square root per column, not per section: LOD selection
            // is horizontal only, so all eight sections of a column share it.
            const std::int32_t distanceInChunks = integerSqrt(horizontalSq);

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

                // A chunk with no voxels yet has nothing to be sticky about and
                // nothing to throw away, so it takes the plain (non-hysteretic)
                // level and is generated at the right resolution first time. Done
                // every update while it stays Empty, so a chunk that waits several
                // frames for a generate slot still tracks a moving player.
                if (state == ChunkState::Empty) {
                    chunk->setLod(m_config.lod.levelFor(distanceInChunks));
                }

                // Ready is the only state a rebuild may start from: its voxels
                // are final, and its mesh is already on screen to cover the
                // transition. Checked ahead of the plain remesh below because a
                // rebuild produces geometry from the current neighbours anyway
                // and therefore subsumes whatever dirtied the chunk.
                const LodLevel targetLod = lodTargetFor(chunk, state, distanceInChunks);

                WorkKind kind = WorkKind::Generate;
                if (state == ChunkState::Empty && m_generate) {
                    kind = WorkKind::Generate;
                } else if (m_mesh && state == ChunkState::Generated &&
                           neighboursInLoadRange(centre, position)) {
                    kind = WorkKind::Mesh;
                } else if (targetLod != chunk->lod() && neighboursInLoadRange(centre, position)) {
                    kind = WorkKind::LodRebuild;
                } else if (m_mesh && state == ChunkState::Ready && chunk->needsRemesh()) {
                    // A remesh is not pre-filtered: the chunk was meshable once,
                    // so its neighbours are almost certainly still resident, and
                    // an edit the player can see must not wait on the rim rule.
                    kind = WorkKind::Mesh;
                } else {
                    continue;
                }

                m_candidates.push_back(Candidate{priorityScore(view, position), position, chunk,
                                                 kind, state, targetLod});
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

    std::size_t dispatched    = 0;
    std::size_t lodDispatched = 0;
    std::size_t skipped       = 0;

    for (const Candidate& candidate : m_candidates) {
        if (dispatched >= m_config.maxScheduledPerUpdate) {
            skipped = m_candidates.size() - dispatched;
            break;
        }

        // Within two chunks of the eye the player is looking at a hole right now.
        JobPriority priority = candidate.score <= kHighPriorityDistance ? JobPriority::High
                                                                       : JobPriority::Normal;
        // A remesh of an already-Ready LEVEL 0 chunk is the player's own edit
        // landing, so it must never queue behind background streaming.
        //
        // Restricted to level 0 deliberately. Since LOD arrived, a Ready remesh
        // is no longer necessarily an edit: completing a rebuild dirties its 26
        // neighbours so their seams pick up the new resolution, and at four
        // transitions per update that is up to a hundred remeshes. Promoting all
        // of those to High would let distant housekeeping outrank the block the
        // player just broke. Level 0 is exactly the interactive region - it ends
        // at LodPolicy::bandStart[0] - so the original intent survives intact,
        // and with LOD disabled every chunk is level 0 and nothing changes.
        if (candidate.kind == WorkKind::Mesh && candidate.state == ChunkState::Ready &&
            candidate.chunk->lod() == kLodFull) {
            priority = JobPriority::High;
        }

        bool started = false;
        if (candidate.kind == WorkKind::Generate) {
            if (m_generateInFlight.load(std::memory_order_acquire) <
                m_config.maxGenerateJobsInFlight) {
                started = dispatchGenerate(candidate.chunk, priority);
            }
        } else if (candidate.kind == WorkKind::LodRebuild) {
            // A demotion has no deadline at all - the chunk is moving away and
            // its old, finer mesh is still perfectly good to look at, so it goes
            // in Low where it cannot delay the terrain the player is walking
            // into. A promotion is the visible direction (coarse geometry
            // resolving as the player approaches) and rides with ordinary
            // streaming.
            const JobPriority lodPriority = candidate.targetLod < candidate.chunk->lod()
                                                ? JobPriority::Normal
                                                : JobPriority::Low;
            if (lodDispatched < m_config.maxLodTransitionsPerUpdate &&
                m_lodInFlight.load(std::memory_order_acquire) < m_config.maxLodJobsInFlight) {
                started = dispatchLodRebuild(candidate, lodPriority);
                if (started) {
                    ++lodDispatched;
                }
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

bool ChunkManager::dispatchLodRebuild(const Candidate& candidate, JobPriority priority)
{
    const ChunkPtr& live   = candidate.chunk;
    const LodLevel  target = candidate.targetLod;

    // Same rule as a first mesh: the geometry job reads a one-voxel skirt out of
    // all 26 neighbours, and an incomplete neighbourhood bakes a wrong seam in.
    ChunkNeighbourhood snapshot = captureNeighbourhood(candidate.position);
    if (!snapshot.complete()) {
        return false;
    }

    // THE serialisation point. Ready -> Meshing is the same transition an
    // ordinary remesh takes, so a remesh and a rebuild - or two rebuilds at two
    // different levels - cannot both win. It is also what makes
    // World::isEditBlocked() refuse writes to this chunk and to all 26 of its
    // neighbours for as long as the job runs, which is precisely the set the job
    // is about to read.
    if (!live->tryTransition(ChunkState::Ready, ChunkState::Meshing)) {
        return false;
    }

    // The rebuild's mesh is built from current voxels, so it satisfies any
    // pending remesh request. An edit landing later re-sets the flag.
    live->clearRemeshFlag();

    // The shadow. Nothing else can reach it until the swap, so the job owns it
    // outright and its state transitions are uncontended by construction - they
    // are performed anyway so the object arrives in the map indistinguishable
    // from one that came through the ordinary pipeline.
    ChunkPtr shadow = Chunk::create(candidate.position);
    shadow->setLod(target);
    shadow->tryTransition(ChunkState::Empty, ChunkState::Generating);

    // Centre swapped for the shadow: the mesher must see the NEW voxels in the
    // middle and the OLD, live voxels around the rim.
    snapshot.setChunk(0, 0, 0, shadow);

    noteJobStarted(WorkKind::LodRebuild);
    m_jobs.submitDetached(priority, [this, alive = m_alive, live, shadow, target,
                                     snapshot = std::move(snapshot)] {
        if (!alive->load(std::memory_order_acquire)) {
            return;
        }

        m_generate(*shadow);
        // The generator is free to reset the level along with everything else,
        // so re-assert it before anyone can read it back.
        shadow->setLod(target);
        // Every one of these is a legal edge of the diagram in Chunk.hpp, and
        // every one is uncontended: the shadow is reachable only from this
        // closure until the swap. They are done anyway so that the object that
        // lands in the map is indistinguishable from a normally streamed chunk,
        // and so the debug assert in tryTransition still has something to check.
        shadow->tryTransition(ChunkState::Generating, ChunkState::Generated);
        shadow->tryTransition(ChunkState::Generated, ChunkState::Meshing);

        ChunkMeshUpload result = m_mesh(snapshot);
        shadow->tryTransition(ChunkState::Meshing, ChunkState::Meshed);

        m_pendingUploads.fetch_add(1, std::memory_order_release);
        m_jobs.mainThreadQueue().push([this, alive, live, shadow, result = std::move(result)] {
            if (!alive->load(std::memory_order_acquire)) {
                return;
            }
            m_pendingUploads.fetch_sub(1, std::memory_order_release);
            finishLodRebuild(live, shadow, result);
        });

        noteJobFinished(WorkKind::LodRebuild);
    });
    return true;
}

void ChunkManager::finishLodRebuild(const ChunkPtr& live, const ChunkPtr& shadow,
                                    const ChunkMeshUpload& result)
{
    // A remesh request that landed on the outgoing chunk while the rebuild was
    // in flight has to survive the swap.
    //
    // The shadow was meshed against a neighbourhood snapshot taken when the job
    // was dispatched. If a neighbour finished its own LOD rebuild in that window
    // it called markNeighboursDirty, which set the flag on `live` - the object
    // about to be thrown away. Clearing the shadow's flag and dropping live's
    // would leave the replacement holding border geometry built against the
    // neighbour's OLD level, with nothing left to ask for a correction: the
    // chunk is Ready and clean, so it is never rescheduled and the seam persists
    // for as long as the chunk stays resident.
    const bool inheritedDirty = live->needsRemesh();

    // Finished off to the side, before the shadow becomes visible to anything.
    shadow->setMeshedVersion(shadow->contentVersion());
    shadow->clearRemeshFlag();
    shadow->setLastTouchedFrame(m_frameIndex);
    shadow->tryTransition(ChunkState::Meshed, ChunkState::Ready);

    // THE SWAP. The residency test and the replacement are one critical section
    // rather than an isStillResident() call followed by a write: comparing
    // addresses is only meaningful if nothing can intervene, and folding them
    // together means there is no window at all rather than a small one.
    //
    // Everything from here to the end of result.upload() happens inside one
    // main-thread task, so no frame is ever drawn with the slot updated but the
    // mesh not - or the other way round. Before this the renderer holds the old
    // chunk's geometry and draws it; after it, the new one. There is no
    // in-between in which the chunk is absent.
    {
        std::unique_lock<std::shared_mutex> lock(m_mutex);
        const auto it = m_chunks.find(shadow->position());
        if (it == m_chunks.end() || it->second.chunk.get() != live.get()) {
            // unloadAll() got here first: the position is gone, or already holds
            // a different object. Publishing now would resurrect dead work.
            m_lodTransitionsDropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        it->second.chunk     = shadow;
        it->second.gpuBytes  = result.gpuBytes;
        it->second.triangles = result.triangles;
    }
    if (result.upload) {
        result.upload();
    }

    // Retire the old object by hand. Deliberately NOT through the release hook:
    // that deletes the renderer's mesh for this position, which is now the mesh
    // we just uploaded. The retire (save) hook fires only if the chunk actually
    // diverged from generated terrain - with preserveEditedChunks on it never
    // can, but a caller who turned that off should still get told.
    //
    // Two steps because Meshing -> Unloading is not an edge of the diagram: a
    // busy chunk may never be retired, and the way out of Meshing is Meshed. We
    // are the thread that put it into Meshing, so we are the one entitled to
    // take it out again.
    live->tryTransition(ChunkState::Meshing, ChunkState::Meshed);
    live->tryTransition(ChunkState::Meshed, ChunkState::Unloading);
    if (m_retire && live->needsSave()) {
        m_retire(live);
    }

    // Now that the shadow is the resident chunk, hand it the request the
    // outgoing object was carrying. Done after the swap so a dropped publish
    // cannot leave a dirty flag on an object nobody will ever mesh again.
    if (inheritedDirty) {
        shadow->markDirty();
    }

    markNeighboursDirty(shadow->position());

    m_lodTransitions.fetch_add(1, std::memory_order_relaxed);
    m_meshesUploaded.fetch_add(1, std::memory_order_relaxed);
}

void ChunkManager::markNeighboursDirty(const ChunkPos& position)
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    for (std::int32_t dy = -1; dy <= 1; ++dy) {
        const std::int32_t ny = position.y + dy;
        if (ny < 0 || ny >= kWorldSectionCount) {
            continue;
        }
        for (std::int32_t dz = -1; dz <= 1; ++dz) {
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0 && dz == 0) {
                    continue;  // the replacement chunk arrived already meshed
                }
                const auto it = m_chunks.find(ChunkPos{position.x + dx, ny, position.z + dz});
                if (it != m_chunks.end()) {
                    // Only the mesh is stale; the neighbour's voxels are
                    // untouched, so it must not be queued for a save.
                    it->second.chunk->markDirty();
                }
            }
        }
    }
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
    } else if (kind == WorkKind::LodRebuild) {
        m_lodInFlight.fetch_add(1, std::memory_order_release);
    } else {
        m_meshInFlight.fetch_add(1, std::memory_order_release);
    }
}

void ChunkManager::noteJobFinished(WorkKind kind) noexcept
{
    if (kind == WorkKind::Generate) {
        m_generateInFlight.fetch_sub(1, std::memory_order_release);
    } else if (kind == WorkKind::LodRebuild) {
        m_lodInFlight.fetch_sub(1, std::memory_order_release);
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
            const LodLevel level = slot.chunk->lod();
            if (level < kLodCount) {
                out.chunksByLod[level] += 1;
            }
            out.cpuVoxelBytes += slot.chunk->memoryUsageBytes();
            out.gpuMeshBytes += slot.gpuBytes;
            out.triangles += slot.triangles;
            out.damagedBlocks += slot.chunk->subVoxels().size();
            out.subVoxelBytes += slot.chunk->subVoxels().memoryUsageBytes();
        }
    }

    out.generatingChunks = out.chunksByState[static_cast<std::size_t>(ChunkState::Generating)];
    out.meshingChunks    = out.chunksByState[static_cast<std::size_t>(ChunkState::Meshing)];
    out.readyChunks      = out.chunksByState[static_cast<std::size_t>(ChunkState::Ready)];

    out.queuedChunks         = m_queuedChunks.load(std::memory_order_relaxed);
    out.visibleChunks        = m_visibleChunks.load(std::memory_order_relaxed);
    out.generateJobsInFlight = m_generateInFlight.load(std::memory_order_acquire);
    out.meshJobsInFlight     = m_meshInFlight.load(std::memory_order_acquire);
    out.lodJobsInFlight      = m_lodInFlight.load(std::memory_order_acquire);
    out.pendingUploads       = m_pendingUploads.load(std::memory_order_acquire);

    out.chunksCreated  = m_chunksCreated.load(std::memory_order_relaxed);
    out.chunksUnloaded = m_chunksUnloaded.load(std::memory_order_relaxed);
    out.meshesUploaded = m_meshesUploaded.load(std::memory_order_relaxed);
    out.meshesDropped  = m_meshesDropped.load(std::memory_order_relaxed);

    out.lodTransitions        = m_lodTransitions.load(std::memory_order_relaxed);
    out.lodTransitionsDropped = m_lodTransitionsDropped.load(std::memory_order_relaxed);

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
