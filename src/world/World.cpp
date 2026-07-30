#include "world/World.hpp"

#include "core/Log.hpp"

#include <algorithm>
#include <utility>

namespace voxl {

World::World(JobSystem& jobs, const BlockRegistry& registry, const StreamingConfig& config)
    : m_jobs(jobs),
      m_registry(registry),
      m_chunks(jobs, config),
      m_light(registry),
      // findReadableMutable, NOT find: the incremental pass runs on the main
      // thread and must never touch a chunk a worker owns. A chunk in
      // Generating has its palette grown underneath the reader and one whose
      // column is being lit has its light array materialised, and both
      // reallocate. An unreadable chunk resolves to null, which lightAt()
      // already reports as darkness and setNibble() already declines to write,
      // so the pass degrades to "do nothing here for now" and World replays the
      // batch once the worker has finished.
      m_lightWorld([this](const ChunkPos& position) {
                       return m_chunks.findReadableMutable(position);
                   },
                   [this](const ChunkPos& position) {
                       const ChunkPtr chunk = m_chunks.find(position);
                       if (chunk == nullptr) {
                           return false;
                       }
                       const ChunkState state = chunk->state();
                       if (state == ChunkState::Empty || state == ChunkState::Unloading ||
                           isChunkBusy(state)) {
                           return false;
                       }
                       return !m_chunks.isNeighbourhoodBusy(position);
                   }),
      m_lighting(config.lighting)
{
    m_deferredEdits.reserve(64);
    if (m_lighting) {
        installLighter();
    }
}

void World::installLighter()
{
    ChunkLighter lighter;

    // One engine per worker, exactly as the mesher is wired: the scratch volume
    // is 200 KB and is written all over during a flood, so sharing one across
    // threads would need a lock around the whole pass. thread_local binds the
    // registry at first use per thread, which is correct because a process has
    // one World and one registry, and the registry is immutable once finalised.
    lighter.column = [this](const LightColumnWork& work) {
        thread_local LightEngine engine{m_registry};
        LightSpill spill = engine.lightColumn(work);
        // Deliberately here and not in the engine: which neighbours a job was
        // allowed to see is a scheduling fact, not a lighting one, and World is
        // where the retry that repairs it lives.
        noteBlindSeams(work);
        return spill;
    };
    lighter.chunk = [this](Chunk& chunk, const ChunkNeighbourhood& around, LightSpill& spill) {
        thread_local LightEngine engine{m_registry};
        engine.lightChunk(chunk, around, &spill);
    };
    lighter.spill = [this](LightSpill&& spill) {
        if (m_pendingLight.empty()) {
            m_pendingLight = std::move(spill);
        } else {
            m_pendingLight.insert(m_pendingLight.end(), spill.begin(), spill.end());
        }
        if (m_pendingLight.size() > kMaxPendingLightSeeds) {
            VOXL_LOG_WARN("dropping {} light seed(s): the backlog exceeded {}",
                          m_pendingLight.size() - kMaxPendingLightSeeds, kMaxPendingLightSeeds);
            m_pendingLight.resize(kMaxPendingLightSeeds);
        }
    };

    m_chunks.setLighter(std::move(lighter));
}

// ----------------------------------------------------------- BlockAccess --

BlockId World::getBlock(const BlockPos& pos) const noexcept
{
    if (pos.y > kWorldMaxY) {
        return kAboveWorldBlock;
    }
    if (pos.y < kWorldMinY) {
        return kBelowWorldBlock;
    }
    const ConstChunkPtr chunk = m_chunks.findReadable(toChunkPos(pos));
    if (chunk == nullptr) {
        return kMissingChunkBlock;
    }
    return chunk->getBlock(blockToLocalAxis(pos.x), blockToLocalAxis(pos.y),
                           blockToLocalAxis(pos.z));
}

std::uint8_t World::getLight(const BlockPos& pos) const noexcept
{
    if (pos.y > kWorldMaxY) {
        return kAboveWorldLight;
    }
    if (pos.y < kWorldMinY) {
        return kBelowWorldLight;
    }
    const ConstChunkPtr chunk = m_chunks.findReadable(toChunkPos(pos));
    if (chunk == nullptr) {
        return kMissingChunkLight;
    }
    return chunk->getLight(blockToLocalAxis(pos.x), blockToLocalAxis(pos.y),
                           blockToLocalAxis(pos.z));
}

bool World::isChunkReady(const ChunkPos& position) const
{
    const ChunkPtr chunk = m_chunks.find(position);
    return chunk != nullptr && chunk->state() == ChunkState::Ready;
}

// ------------------------------------------------------------------ edits --

EditResult World::setBlock(const BlockPos& pos, BlockId id)
{
    VOXL_CHECK(!m_jobs.onWorkerThread(), "World::setBlock() called from a worker thread");

    if (!isInsideWorld(pos)) {
        return EditResult::OutOfBounds;
    }

    const ChunkPtr chunk = m_chunks.find(toChunkPos(pos));
    if (chunk == nullptr) {
        return EditResult::NotLoaded;
    }

    if (chunk->state() == ChunkState::Unloading) {
        return EditResult::NotLoaded;
    }

    const ChunkPos chunkPos = toChunkPos(pos);
    const bool     blockedForEdit = isEditBlocked(chunk, chunkPos);

    // Reading the old id is only safe once the chunk is known not to be owned by
    // a worker, so the relight decision has to come after the edit test.
    const BlockId beforeId =
        blockedForEdit ? blocks::Air
                       : chunk->getBlock(blockToLocalAxis(pos.x), blockToLocalAxis(pos.y),
                                         blockToLocalAxis(pos.z));
    const bool relight = !blockedForEdit && m_lighting && m_light.affectsLight(beforeId, id);

    if (blockedForEdit || (relight && isRelightBlocked(pos))) {
        if (m_deferredEdits.size() >= kMaxDeferredEdits) {
            VOXL_LOG_WARN("dropping edit at ({}, {}, {}): deferral queue full", pos.x, pos.y, pos.z);
            return EditResult::Rejected;
        }
        m_deferredEdits.push_back(PendingEdit{pos, id, 0, EditKind::Block});
        return EditResult::Deferred;
    }

    writeBlock(chunk, pos, id);
    if (relight) {
        relightAround(pos);
    }
    return EditResult::Applied;
}

bool World::isEditBlocked(const ChunkPtr& chunk, const ChunkPos& position) const
{
    // Empty and Generating are owned by a generator - or, since lighting, by the
    // light pass that has not run yet - and are about to be overwritten;
    // Meshing is owned by a mesher reading this chunk's palette.
    const ChunkState state = chunk->state();
    if (state == ChunkState::Empty || isChunkBusy(state)) {
        return true;
    }
    return m_chunks.isNeighbourhoodBusy(position);
}

bool World::isRelightBlocked(const BlockPos& pos) const
{
    // The memo below only answers inside a Pass - outside one, LightWorld caches
    // nothing at all, which is what stops a stale chunk pointer surviving into a
    // later frame. This walk is the one reader that is not already inside an
    // engine call, so it opens its own: without it the loop costs a map lookup
    // and a shared_ptr copy for every one of up to 256 blocks instead of roughly
    // eight. Safe because Pass invalidates at both ends, and correctness does not
    // depend on it either way.
    const LightWorld::Pass pass{m_lightWorld};

    // How far down a sunlight change can reach: the shaft under `pos` runs to the
    // first block that stops light, and cells beside the shaft can carry it 15
    // further. Walked through the light engine's memoising reader so a 200-block
    // shaft costs a handful of chunk lookups rather than 200.
    std::int32_t lowest = pos.y;
    for (std::int32_t y = pos.y - 1; y >= kWorldMinY; --y) {
        const BlockPos below{pos.x, y, pos.z};
        if (m_registry.isOpaque(m_lightWorld.blockAt(below)) && m_lightWorld.wholeAt(below)) {
            break;
        }
        lowest = y;
    }

    const std::int32_t minY = std::max(kWorldMinY, lowest - kFullLight);
    const std::int32_t maxY = std::min(kWorldMaxY, pos.y + kFullLight);

    // Horizontally 15 blocks cannot leave the edited chunk's own 3x3 - the chunk
    // is 32 wide - so only the vertical extent has to be turned into chunks.
    const ChunkPos minChunk{blockToChunkAxis(pos.x - kFullLight), blockToChunkAxis(minY),
                            blockToChunkAxis(pos.z - kFullLight)};
    const ChunkPos maxChunk{blockToChunkAxis(pos.x + kFullLight), blockToChunkAxis(maxY),
                            blockToChunkAxis(pos.z + kFullLight)};
    return m_chunks.isRegionBusy(minChunk, maxChunk);
}

bool World::subVoxelEditMayRelight(const ChunkPtr& chunk, std::size_t blockIndex,
                                   std::size_t subIndex, BlockId material, bool restore)
{
    // Mirrors, case for case, what Chunk::breakSubVoxel / Chunk::restoreSubVoxel
    // will do and therefore what writeSubVoxel's `wasWhole != isWhole ||
    // beforeId != afterId` test will see. Everything here is a store lookup plus
    // at most eight popcounts, so it is cheaper than the isRelightBlocked walk it
    // guards and far cheaper than the flood it would otherwise let through.
    const BlockId       current = chunk->getBlock(blockIndex);
    const SubVoxelGrid* grid    = chunk->subVoxels().find(blockIndex);

    if (!restore) {
        if (current == blocks::Air) {
            // Carving air is a no-op: Chunk::breakSubVoxel bails and the
            // invariant guarantees there is no entry to disagree with it.
            return false;
        }
        if (grid == nullptr) {
            return true;  // whole -> partial: opaque becomes transparent
        }
        // Partial. Only emptying the grid changes opacity, and that happens
        // exactly when the one sub-voxel still present is the one being cleared -
        // the last swing of mining any block.
        return grid->test(subIndex) && grid->count() == 1;
    }

    if (current == blocks::Air) {
        // Air -> partial, the mirror case. The block stops being air, which is
        // the same opacity change run backwards, so it needs the same test.
        return material != blocks::Air;
    }
    if (grid == nullptr) {
        return false;  // solid with no entry is whole already: Unchanged
    }
    if (grid->test(subIndex)) {
        return false;  // already present: Unchanged
    }
    return grid->count() == kSubVoxelCount - 1;  // partial -> whole
}

// -------------------------------------------------------------- lighting --

void World::relightAround(const BlockPos& pos)
{
    m_lightWorld.resetCounters();
    m_lightWorld.clearTouched();
    m_lastLightUpdate = m_light.voxelChanged(m_lightWorld, pos);
    markLitChunksDirty();
}

void World::markLitChunksDirty()
{
    for (const LightWorld::Touched& touched : m_lightWorld.touched()) {
        const ChunkPtr chunk = m_chunks.find(touched.position);
        if (chunk != nullptr) {
            // Only the mesh is stale. The voxels did not change, so this must
            // NOT set the save flag - light is derived data and is recomputed on
            // load, and marking it modified would also pin the chunk at its
            // current LOD level (see StreamingConfig::preserveEditedChunks).
            chunk->markDirty();
        }
        if (touched.faceMask == 0) {
            continue;
        }

        // A light value on a chunk face is read by the neighbour's mesher as
        // part of its skirt, and by the ambient-occlusion ring one step further
        // round, so an edge or a corner reaches up to seven chunks. Same product
        // of per-axis offsets as markSeamNeighboursDirty, driven by which faces
        // the flood actually touched instead of by one voxel's position.
        //
        // THREE SLOTS PER AXIS, NOT TWO. markSeamNeighboursDirty gets away with
        // two because it starts from a single voxel, which can only be against
        // ONE face of any given axis. A flood is not a voxel: a relight that
        // crosses a chunk end to end touches -X and +X both, and then this axis
        // needs 0, -1 and +1 all three. Sizing it [2] wrote one past the row -
        // silently corrupting the next axis's offsets, and on the last axis
        // running off the array and taking out the stack cookie.
        std::int32_t offsets[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
        std::int32_t counts[3]     = {1, 1, 1};
        const std::uint8_t low[3]  = {
            static_cast<std::uint8_t>(1u << static_cast<int>(Direction::NegX)),
            static_cast<std::uint8_t>(1u << static_cast<int>(Direction::NegY)),
            static_cast<std::uint8_t>(1u << static_cast<int>(Direction::NegZ))};
        const std::uint8_t high[3] = {
            static_cast<std::uint8_t>(1u << static_cast<int>(Direction::PosX)),
            static_cast<std::uint8_t>(1u << static_cast<int>(Direction::PosY)),
            static_cast<std::uint8_t>(1u << static_cast<int>(Direction::PosZ))};

        for (std::size_t axis = 0; axis < 3; ++axis) {
            if ((touched.faceMask & low[axis]) != 0) {
                offsets[axis][counts[axis]++] = -1;
            }
            if ((touched.faceMask & high[axis]) != 0) {
                offsets[axis][counts[axis]++] = 1;
            }
        }

        for (std::int32_t ix = 0; ix < counts[0]; ++ix) {
            for (std::int32_t iy = 0; iy < counts[1]; ++iy) {
                for (std::int32_t iz = 0; iz < counts[2]; ++iz) {
                    const std::int32_t dx = offsets[0][ix];
                    const std::int32_t dy = offsets[1][iy];
                    const std::int32_t dz = offsets[2][iz];
                    if (dx == 0 && dy == 0 && dz == 0) {
                        continue;  // handled above
                    }
                    const ChunkPos neighbourPos{touched.position.x + dx, touched.position.y + dy,
                                                touched.position.z + dz};
                    if (neighbourPos.y < 0 || neighbourPos.y >= kWorldSectionCount) {
                        continue;
                    }
                    const ChunkPtr neighbourChunk = m_chunks.find(neighbourPos);
                    if (neighbourChunk != nullptr) {
                        neighbourChunk->markDirty();
                    }
                }
            }
        }
    }
    m_lightWorld.clearTouched();
}

void World::applyPendingLight()
{
    if (m_pendingLight.empty()) {
        return;
    }

    const std::size_t take = std::min(m_pendingLight.size(), kLightSeedsPerUpdate);

    m_lightWorld.resetCounters();
    m_lightWorld.clearTouched();
    const LightUpdateStats stats = m_light.applySeeds(m_lightWorld, m_pendingLight.data(), take);
    markLitChunksDirty();

    if (stats.writesRefused == 0) {
        m_pendingLight.erase(m_pendingLight.begin(),
                             m_pendingLight.begin() + static_cast<std::ptrdiff_t>(take));
        return;
    }
    // A chunk in the flood's path was owned by a worker, so part of this batch
    // never landed. Replaying it is idempotent - a seed whose target is already
    // at least that bright is dropped on sight - so the cheapest correct answer
    // is to leave the batch queued and try again next update, by which time the
    // mesh or light job that owned the chunk has finished.
}

// ------------------------------------------------------- blind column seams --
//
// See the BlindSeam block in World.hpp for what this is repairing. In short: two
// neighbouring columns lit at the same time each see the other as an unloaded
// wall, so no light crosses between them, ever.
//
// THE ROOT FIX IS ELSEWHERE, AND IT HAS LANDED. ChunkManager::claimColumnLight
// now refuses a claim whose face-adjacent column is already in m_lightColumns,
// which restores the "whichever is second reads the first" ordering the design
// assumes, at the cost of some lighting parallelism. Two neighbours can no
// longer be in flight together at all.
//
// THIS PASS IS KEPT AS A NET, not as the cure, because the ordering fix covers
// simultaneity and nothing else. A neighbour that is resident but simply not lit
// yet still arrives as a null slot, and while that case IS self-correcting in
// principle - the neighbour's own job reads us and spills back - the
// self-correction depends on that job ever running. A tolerant claim leaves
// sections unlit for good (see ChunkManager::claimColumnLight), and a column
// that has left the load volume gets no further jobs at all. This pass settles
// those from the main thread after the fact. It is idempotent and
// self-cancelling: every seed it raises is dropped as non-raising once the two
// sides already agree, and the uniform fast path in seedAcrossSeam makes
// rock-against-rock and sky-against-sky cost two loads.

namespace {

/// The four column neighbours a light job can be blind to. Faces only: the
/// engine's shell is loaded from the six face slabs, so a diagonal column
/// contributes nothing the faces do not already carry.
constexpr std::int32_t kSeamDx[4] = {-1, 1, 0, 0};
constexpr std::int32_t kSeamDz[4] = {0, 0, -1, 1};

}  // namespace

void World::noteBlindSeams(const LightColumnWork& work)
{
    for (std::size_t face = 0; face < 4; ++face) {
        const std::int32_t dx = kSeamDx[face];
        const std::int32_t dz = kSeamDz[face];

        bool blind = false;
        for (std::int32_t y = 0; y < kWorldSectionCount && !blind; ++y) {
            if (work.targets[static_cast<std::size_t>(y)] == nullptr) {
                continue;  // not a section this job lit, so it owes nothing here
            }
            if (work.at(dx, dz, y) != nullptr) {
                continue;  // the neighbour was readable; the engine handled it
            }
            // A null slot is either "not resident" or "resident but not final".
            // Only the second matters. A section that does not exist yet will be
            // generated and lit later, and THAT job sees ours and spills back;
            // one that is already resident may never look at us again.
            blind =
                m_chunks.find(ChunkPos{work.column.x + dx, y, work.column.z + dz}) != nullptr;
        }
        if (!blind) {
            continue;
        }

        // Canonical form - always the lower column looking toward the higher one
        // - so the jobs on either side of one boundary record the same entry.
        BlindSeam seam{work.column, dx, dz, 0};
        if (dx < 0) {
            seam.column.x -= 1;
            seam.dx = 1;
        }
        if (dz < 0) {
            seam.column.z -= 1;
            seam.dz = 1;
        }

        const std::lock_guard<std::mutex> lock(m_blindSeamMutex);
        bool                              known = false;
        for (const BlindSeam& queued : m_blindSeams) {
            if (queued.column == seam.column && queued.dx == seam.dx && queued.dz == seam.dz) {
                known = true;
                break;
            }
        }
        if (known || m_blindSeams.size() >= kMaxBlindSeams) {
            continue;
        }
        m_blindSeams.push_back(seam);
    }
}

void World::reconcileBlindSeams()
{
    std::vector<BlindSeam> pending;
    {
        const std::lock_guard<std::mutex> lock(m_blindSeamMutex);
        if (m_blindSeams.empty()) {
            return;
        }
        pending.swap(m_blindSeams);
    }

    std::size_t scanned = 0;
    std::size_t index   = 0;
    std::vector<BlindSeam> unfinished;
    for (; index < pending.size() && scanned < kSeamCellsPerUpdate; ++index) {
        BlindSeam seam = pending[index];
        if (reconcileSeam(seam, scanned)) {
            continue;
        }
        // A section on one side is resident but not lit yet. Worth waiting for -
        // but not forever: a tolerant claim can leave a section unlit for good,
        // and a seam that waits on one would be retried every update for the
        // life of the process.
        if (++seam.attempts < kMaxSeamAttempts) {
            unfinished.push_back(seam);
        }
    }

    const std::lock_guard<std::mutex> lock(m_blindSeamMutex);
    // Whatever the budget did not reach goes back first, so a long queue drains
    // in order instead of starving its tail.
    m_blindSeams.insert(m_blindSeams.begin(), pending.begin() + static_cast<std::ptrdiff_t>(index),
                        pending.end());
    m_blindSeams.insert(m_blindSeams.end(), unfinished.begin(), unfinished.end());
}

bool World::reconcileSeam(const BlindSeam& seam, std::size_t& scanned)
{
    bool settled = true;
    for (std::int32_t y = 0; y < kWorldSectionCount; ++y) {
        const ChunkPos nearPos{seam.column.x, y, seam.column.z};
        const ChunkPos farPos{seam.column.x + seam.dx, y, seam.column.z + seam.dz};

        // findReadableMutable is the light engine's own gate: it hands back only
        // chunks that are final in BOTH voxels and light. A section that is not
        // lit yet has nothing to exchange, and reading one would read an array a
        // worker is still filling in.
        const ChunkPtr nearChunk = m_chunks.findReadableMutable(nearPos);
        const ChunkPtr farChunk  = m_chunks.findReadableMutable(farPos);
        if (nearChunk == nullptr || farChunk == nullptr) {
            // Not lit yet, or gone. Only the first is worth coming back for: an
            // unloaded section has no seam left to settle.
            if (m_chunks.isResident(nearPos) && m_chunks.isResident(farPos)) {
                settled = false;
            }
            continue;
        }

        scanned += seedAcrossSeam(*nearChunk, *farChunk, seam.dx, seam.dz);
        scanned += seedAcrossSeam(*farChunk, *nearChunk, -seam.dx, -seam.dz);
    }
    return settled;
}

std::size_t World::seedAcrossSeam(const Chunk& from, const Chunk& to, std::int32_t dx,
                                  std::int32_t dz)
{
    // A dark section gives nothing to anybody, and a section that has never been
    // written cell by cell says so in one load.
    if (!from.storage().hasLightData() && from.storage().uniformLight() == 0) {
        return 0;
    }
    if (!from.storage().hasLightData() && !to.storage().hasLightData()) {
        // Both flat. Nothing can cross when the source, less the CHEAPEST
        // possible step of one, still fails to beat what the destination already
        // holds everywhere - and a real step is never cheaper than one, so this
        // is exact rather than an approximation. Open sky against open sky and
        // rock against rock, which is most of a column, end here.
        const int sourceLight      = from.storage().uniformLight();
        const int destinationLight = to.storage().uniformLight();
        const int sourceSun        = ChunkStorage::unpackSunlight(
            static_cast<std::uint8_t>(sourceLight));
        const int sourceBlock = ChunkStorage::unpackBlockLight(
            static_cast<std::uint8_t>(sourceLight));
        const int destinationSun = ChunkStorage::unpackSunlight(
            static_cast<std::uint8_t>(destinationLight));
        const int destinationBlock = ChunkStorage::unpackBlockLight(
            static_cast<std::uint8_t>(destinationLight));
        if (sourceSun <= destinationSun + 1 && sourceBlock <= destinationBlock + 1) {
            return 0;
        }
    }

    // Exactly one of dx, dz is non-zero, so one of these pins the face and the
    // other is swept by `t`.
    const bool         toHigher = dx > 0 || dz > 0;
    const std::int32_t fromFace = toHigher ? kChunkSize - 1 : 0;
    const std::int32_t toFace   = toHigher ? 0 : kChunkSize - 1;
    const BlockPos     toOrigin = to.originBlock();

    for (std::int32_t y = 0; y < kChunkSize; ++y) {
        for (std::int32_t t = 0; t < kChunkSize; ++t) {
            const std::int32_t fromX = dx != 0 ? fromFace : t;
            const std::int32_t fromZ = dz != 0 ? fromFace : t;
            const std::int32_t toX   = dx != 0 ? toFace : t;
            const std::int32_t toZ   = dz != 0 ? toFace : t;

            const std::size_t toIndex = localIndex(toX, y, toZ);
            const BlockId     toId    = to.getBlock(toIndex);

            // Same material rule as LightEngine::cellMaterial: a partially
            // destroyed block transmits freely whatever it is made of.
            const bool whole = to.isBlockWhole(toIndex);
            if (whole && m_light.opaque(toId)) {
                continue;  // light may not enter this cell at all
            }
            // Same arithmetic as LightEngine::collectSpill for a side face. The
            // sunlight free-fall rule is downward-only, so a horizontal step
            // always costs at least one.
            const std::uint8_t cost = static_cast<std::uint8_t>(
                1 + (whole ? m_light.attenuation(toId) : std::uint8_t{0}));

            const std::uint8_t fromPacked = from.getLight(localIndex(fromX, y, fromZ));
            const std::uint8_t toPacked   = to.getLight(toIndex);

            const std::uint8_t sourceSun   = ChunkStorage::unpackSunlight(fromPacked);
            const std::uint8_t sourceBlock = ChunkStorage::unpackBlockLight(fromPacked);
            const std::uint8_t sunOut =
                sourceSun > cost ? static_cast<std::uint8_t>(sourceSun - cost) : std::uint8_t{0};
            const std::uint8_t blockOut = sourceBlock > cost
                                              ? static_cast<std::uint8_t>(sourceBlock - cost)
                                              : std::uint8_t{0};

            const bool raisesSun   = sunOut > ChunkStorage::unpackSunlight(toPacked);
            const bool raisesBlock = blockOut > ChunkStorage::unpackBlockLight(toPacked);
            if (!raisesSun && !raisesBlock) {
                continue;
            }
            if (m_pendingLight.size() >= kMaxPendingLightSeeds) {
                return static_cast<std::size_t>(kChunkSize) * kChunkSize;
            }
            m_pendingLight.push_back(
                LightSeed{BlockPos{toOrigin.x + toX, toOrigin.y + y, toOrigin.z + toZ},
                          raisesSun ? sunOut : std::uint8_t{0},
                          raisesBlock ? blockOut : std::uint8_t{0}});
        }
    }
    return static_cast<std::size_t>(kChunkSize) * kChunkSize;
}

EditResult World::placeBlock(const BlockPos& pos, BlockId id)
{
    if (!isInsideWorld(pos)) {
        return EditResult::OutOfBounds;
    }
    if (!isReplaceable(pos)) {
        return EditResult::Rejected;
    }
    return setBlock(pos, id);
}

void World::writeBlock(const ChunkPtr& chunk, const BlockPos& pos, BlockId id)
{
    const std::int32_t localX = blockToLocalAxis(pos.x);
    const std::int32_t localY = blockToLocalAxis(pos.y);
    const std::int32_t localZ = blockToLocalAxis(pos.z);

    // Replacing a block discards its damage. Chunk::setBlock is inline in the
    // frozen header and deliberately does not do this, so it has to happen here -
    // and it is not cosmetic: leaving the entry behind gives a grid whose
    // `material` no longer matches ChunkStorage, which is a direct violation of
    // the SubVoxel.hpp invariant and would render the old block's carved geometry
    // over the new block.
    chunk->subVoxels().erase(localIndex(localX, localY, localZ));

    // Chunk::setBlock already bumps the content version and sets both the remesh
    // and the save flag for this chunk, so only the seam neighbours are left.
    chunk->setBlock(localX, localY, localZ, id);
    markSeamNeighboursDirty(pos);
}

// ------------------------------------------------------------- sub-voxels --

EditResult World::breakSubVoxel(const BlockPos& pos, std::size_t subIndex)
{
    return editSubVoxel(pos, subIndex, blocks::Air, false);
}

EditResult World::restoreSubVoxel(const BlockPos& pos, std::size_t subIndex, BlockId material)
{
    return editSubVoxel(pos, subIndex, material, true);
}

EditResult World::breakSubVoxelAt(const glm::vec3& worldPosition)
{
    const SubVoxelHit hit = toSubVoxel(worldPosition);
    return editSubVoxel(hit.block, hit.index(), blocks::Air, false);
}

EditResult World::restoreSubVoxelAt(const glm::vec3& worldPosition, BlockId material)
{
    const SubVoxelHit hit = toSubVoxel(worldPosition);
    return editSubVoxel(hit.block, hit.index(), material, true);
}

EditResult World::editSubVoxel(const BlockPos& pos, std::size_t subIndex, BlockId material,
                               bool restore)
{
    VOXL_CHECK(!m_jobs.onWorkerThread(), "World sub-voxel edit called from a worker thread");
    VOXL_CHECK(subIndex < kSubVoxelCount, "sub-voxel index out of range");

    if (!isInsideWorld(pos)) {
        return EditResult::OutOfBounds;
    }

    const ChunkPos  chunkPos = toChunkPos(pos);
    const ChunkPtr  chunk    = m_chunks.find(chunkPos);
    if (chunk == nullptr || chunk->state() == ChunkState::Unloading) {
        return EditResult::NotLoaded;
    }

    // Only opaque materials may be damaged at sub-voxel resolution.
    //
    // Sub-voxel geometry is a single stream drawn in the opaque pass with one
    // program that has no alpha cutoff and no blending (assets/shaders/
    // subvoxel.frag). Carving a Cutout or Translucent block - leaves, glass, ice,
    // all of which are CollisionShape::Cube and so are perfectly valid raycast
    // targets - would re-emit it through that program and turn a see-through
    // block into a solid one the moment it was chipped. Rejecting the edit is
    // honest; the alternative is splitting the sub-voxel mesh into per-layer
    // index ranges with their own blend state, which is real work and is not
    // justified until something in the game actually needs to chip glass.
    const BlockId existing = getBlock(pos);
    if (!restore && m_registry.renderLayer(existing) != RenderLayer::Opaque) {
        return EditResult::Rejected;
    }
    if (restore && existing != blocks::Air &&
        m_registry.renderLayer(existing) != RenderLayer::Opaque) {
        return EditResult::Rejected;
    }
    if (restore && existing == blocks::Air &&
        m_registry.renderLayer(material) != RenderLayer::Opaque) {
        return EditResult::Rejected;
    }

    // Identical rule to setBlock, and for a stronger reason: SubVoxelStore is a
    // sorted vector that reallocates as it grows, so a carve landing while a
    // neighbour's mesh job walks the store is a use-after-free, not a torn read.
    bool blocked = isEditBlocked(chunk, chunkPos);
    if (!blocked && m_lighting) {
        // A partially destroyed block already transmits light, so most carves
        // move no light at all and must not pay for the footprint walk, nor be
        // deferred by a busy chunk they cannot affect. But "already partial" is
        // NOT the same as "cannot change opacity": the carve that clears the last
        // sub-voxel turns the block to air. subVoxelEditMayRelight answers the
        // question writeSubVoxel will actually ask, so the two cannot disagree.
        const std::size_t blockIndex = localIndex(blockToLocalAxis(pos.x),
                                                  blockToLocalAxis(pos.y),
                                                  blockToLocalAxis(pos.z));
        if (subVoxelEditMayRelight(chunk, blockIndex, subIndex, material, restore) &&
            isRelightBlocked(pos)) {
            blocked = true;
        }
    }
    if (blocked) {
        if (m_deferredEdits.size() >= kMaxDeferredEdits) {
            VOXL_LOG_WARN("dropping sub-voxel edit at ({}, {}, {}): deferral queue full", pos.x,
                          pos.y, pos.z);
            return EditResult::Rejected;
        }
        m_deferredEdits.push_back(
            PendingEdit{pos, material, static_cast<std::uint16_t>(subIndex),
                        restore ? EditKind::SubVoxelRestore : EditKind::SubVoxelBreak});
        return EditResult::Deferred;
    }

    writeSubVoxel(chunk, pos, subIndex, material, restore);
    return EditResult::Applied;
}

SubVoxelEdit World::writeSubVoxel(const ChunkPtr& chunk, const BlockPos& pos, std::size_t subIndex,
                                  BlockId material, bool restore)
{
    const std::size_t blockIndex = localIndex(blockToLocalAxis(pos.x), blockToLocalAxis(pos.y),
                                              blockToLocalAxis(pos.z));

    const bool    wasWhole = chunk->isBlockWhole(blockIndex);
    const BlockId beforeId = chunk->getBlock(blockIndex);

    const SubVoxelEdit result = restore ? chunk->restoreSubVoxel(blockIndex, subIndex, material)
                                        : chunk->breakSubVoxel(blockIndex, subIndex);
    if (result == SubVoxelEdit::Unchanged) {
        // The sub-voxel was already in the requested state. Dirtying anything
        // here would remesh the chunk for nothing, and a player holding the
        // break button on an already-carved cell would do it every frame.
        return result;
    }

    // Chunk::breakSubVoxel/restoreSubVoxel already bump the content version and
    // set both the remesh and the save flag, exactly as Chunk::setBlock does for
    // a whole block, so only the seam neighbours are left.
    //
    // The same border rule as a whole block: a sub-voxel on a chunk face changes
    // what the neighbour may cull, and one on an edge or corner changes the
    // ambient occlusion of quads in up to seven chunks.
    markSeamNeighboursDirty(pos);

    if (m_lighting) {
        // The light engine treats any partially destroyed block as empty space,
        // so chipping one more sub-voxel out of an already-damaged block moves no
        // light at all. Only crossing the whole/partial boundary does - which is
        // also exactly what makes carving a tunnel into a hillside let daylight
        // in - and so does the block appearing or disappearing entirely.
        const bool    isWhole = chunk->isBlockWhole(blockIndex);
        const BlockId afterId = chunk->getBlock(blockIndex);
        if (wasWhole != isWhole || beforeId != afterId) {
            relightAround(pos);
        }
    }
    return result;
}

const SubVoxelGrid* World::subVoxelsAt(const BlockPos& pos) const
{
    if (!isInsideWorld(pos)) {
        return nullptr;
    }
    const ConstChunkPtr chunk = m_chunks.findReadable(toChunkPos(pos));
    if (chunk == nullptr) {
        return nullptr;
    }
    return chunk->subVoxels().find(localIndex(blockToLocalAxis(pos.x), blockToLocalAxis(pos.y),
                                              blockToLocalAxis(pos.z)));
}

bool World::isBlockWhole(const BlockPos& pos) const
{
    // An unloaded or out-of-world block is not partially destroyed, so it is
    // "whole" - callers use this to decide whether to fall back to the block id,
    // and the block id is exactly what BlockAccess already defines out there.
    return subVoxelsAt(pos) == nullptr;
}

LodLevel World::lodAt(const BlockPos& pos) const
{
    const ChunkPtr chunk = m_chunks.find(toChunkPos(pos));
    return chunk != nullptr ? chunk->lod() : kLodFull;
}

void World::markSeamNeighboursDirty(const BlockPos& pos)
{
    const std::int32_t lx = blockToLocalAxis(pos.x);
    const std::int32_t ly = blockToLocalAxis(pos.y);
    const std::int32_t lz = blockToLocalAxis(pos.z);

    // An edit only affects a neighbour along an axis where it sits on that
    // chunk's boundary. Collecting the per-axis offsets first and then taking the
    // product covers the face, edge and corner neighbours uniformly.
    std::int32_t offsets[3][2] = {{0, 0}, {0, 0}, {0, 0}};
    std::int32_t counts[3]     = {1, 1, 1};
    const std::int32_t locals[3] = {lx, ly, lz};

    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (locals[axis] == 0) {
            offsets[axis][1] = -1;
            counts[axis]     = 2;
        } else if (locals[axis] == kChunkSize - 1) {
            offsets[axis][1] = 1;
            counts[axis]     = 2;
        }
    }

    const ChunkPos origin = toChunkPos(pos);
    for (std::int32_t ix = 0; ix < counts[0]; ++ix) {
        for (std::int32_t iy = 0; iy < counts[1]; ++iy) {
            for (std::int32_t iz = 0; iz < counts[2]; ++iz) {
                const std::int32_t dx = offsets[0][ix];
                const std::int32_t dy = offsets[1][iy];
                const std::int32_t dz = offsets[2][iz];
                if (dx == 0 && dy == 0 && dz == 0) {
                    continue;  // the edited chunk itself was handled by setBlock
                }

                const ChunkPos neighbourPos{origin.x + dx, origin.y + dy, origin.z + dz};
                if (neighbourPos.y < 0 || neighbourPos.y >= kWorldSectionCount) {
                    continue;
                }
                const ChunkPtr neighbourChunk = m_chunks.find(neighbourPos);
                if (neighbourChunk == nullptr) {
                    continue;
                }
                // Only the mesh is stale; the neighbour's voxels did not change,
                // so it must NOT be marked as needing a save.
                neighbourChunk->markDirty();
            }
        }
    }
}

std::size_t World::applyDeferredEdits()
{
    if (m_deferredEdits.empty()) {
        return 0;
    }

    std::vector<PendingEdit> retry;

    // ORDER AT A POSITION HAS TO SURVIVE THE RETRY. The relight test below is
    // per-position, not per-chunk, so two edits queued against the same block can
    // get different answers within one pass; letting the second through while the
    // first still waits would apply them backwards and leave the wrong block
    // behind. Once a position is held back, everything queued behind it at that
    // position is held back with it.
    const auto heldBack = [&retry](const BlockPos& position) {
        for (const PendingEdit& waiting : retry) {
            if (waiting.position == position) {
                return true;
            }
        }
        return false;
    };

    for (const PendingEdit& edit : m_deferredEdits) {
        const ChunkPos chunkPos = toChunkPos(edit.position);
        const ChunkPtr chunk    = m_chunks.find(chunkPos);
        if (chunk == nullptr) {
            continue;  // the chunk left the world; the edit is meaningless now
        }
        if (chunk->state() == ChunkState::Unloading) {
            continue;
        }
        if (isEditBlocked(chunk, chunkPos) || heldBack(edit.position)) {
            retry.push_back(edit);
            continue;
        }

        if (edit.kind == EditKind::Block) {
            // MIRRORS setBlock, AND HAS TO GO ON MIRRORING IT. The replay used to
            // be a bare writeBlock: every edit that happened to be deferred - which
            // near the streaming frontier is most of them - landed with its
            // lighting left stale, and permanently so, because nothing dirties
            // light that has already been published.
            const BlockId beforeId = chunk->getBlock(blockToLocalAxis(edit.position.x),
                                                     blockToLocalAxis(edit.position.y),
                                                     blockToLocalAxis(edit.position.z));
            const bool relight = m_lighting && m_light.affectsLight(beforeId, edit.id);

            // isEditBlocked answers for the 3x3x3 the write itself touches; a
            // sunlight change runs the height of the world. Re-testing the taller
            // footprint here is what keeps the replay from flooding light into a
            // chunk a worker owns - the very race the deferral exists to avoid.
            if (relight && isRelightBlocked(edit.position)) {
                retry.push_back(edit);
                continue;
            }

            writeBlock(chunk, edit.position, edit.id);
            if (relight) {
                relightAround(edit.position);
            }
            continue;
        }

        // Sub-voxels. writeSubVoxel relights by itself, so the omission above was
        // never present here - but the footprint test was missing just the same.
        // Carving the last whole block out of a sunlit column moves light exactly
        // as a setBlock does, and only editSubVoxel was checking that the column
        // the flood would run down was free.
        //
        // MIRRORS editSubVoxel and has to go on mirroring it, hence the shared
        // predicate: a replay that admitted a carve editSubVoxel would have
        // deferred is the same race, one frame later.
        const bool restore = edit.kind == EditKind::SubVoxelRestore;
        if (m_lighting) {
            const std::size_t blockIndex = localIndex(blockToLocalAxis(edit.position.x),
                                                      blockToLocalAxis(edit.position.y),
                                                      blockToLocalAxis(edit.position.z));
            const BlockId material = restore ? edit.id : blocks::Air;
            if (subVoxelEditMayRelight(chunk, blockIndex, edit.subIndex, material, restore) &&
                isRelightBlocked(edit.position)) {
                retry.push_back(edit);
                continue;
            }
        }
        writeSubVoxel(chunk, edit.position, edit.subIndex, restore ? edit.id : blocks::Air,
                      restore);
    }

    m_deferredEdits.swap(retry);
    return m_deferredEdits.size();
}

// -------------------------------------------------------------- streaming --

void World::update(const StreamingView& view, std::uint64_t frameIndex)
{
    VOXL_CHECK(!m_jobs.onWorkerThread(), "World::update() called from a worker thread");

    m_frameIndex = frameIndex;
    // Before scheduling, so an edit that was waiting on a mesh job is part of the
    // content this frame's mesh decisions see.
    applyDeferredEdits();
    // Boundaries between two columns that were lit at the same time never
    // exchanged any light at all. Settling them BEFORE the flood means the seeds
    // it produces are applied in this same update rather than the next one.
    reconcileBlindSeams();
    // Likewise for light that crossed a chunk border on a worker: applying it
    // here means the remesh it dirties is picked up by this frame's collect()
    // instead of the next one, with a visibly wrong seam in between.
    applyPendingLight();
    m_chunks.update(view, frameIndex);
}

void World::unloadAll()
{
    m_deferredEdits.clear();
    m_pendingLight.clear();
    {
        // Cleared under the lock: a light job that is still in flight can be
        // recording seams against columns that are being retired right now.
        const std::lock_guard<std::mutex> lock(m_blindSeamMutex);
        m_blindSeams.clear();
    }
    m_chunks.unloadAll();
}

WorldStats World::stats() const
{
    WorldStats out        = m_chunks.stats();
    out.deferredEdits     = m_deferredEdits.size();
    out.pendingLightSeeds = m_pendingLight.size();
    return out;
}

}  // namespace voxl
