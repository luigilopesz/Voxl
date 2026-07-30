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
        // A partially destroyed block already transmits light, so the only
        // sub-voxel edits that move a light level are the ones that cross the
        // whole/partial boundary. Testing the relight footprint only for those
        // keeps a player grinding through 512 sub-voxels from paying for it 511
        // times, and from being deferred by a busy chunk it cannot affect.
        const std::size_t blockIndex = localIndex(blockToLocalAxis(pos.x),
                                                  blockToLocalAxis(pos.y),
                                                  blockToLocalAxis(pos.z));
        if ((chunk->isBlockWhole(blockIndex) || restore) && isRelightBlocked(pos)) {
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
        const bool restore = edit.kind == EditKind::SubVoxelRestore;
        if (m_lighting) {
            const std::size_t blockIndex = localIndex(blockToLocalAxis(edit.position.x),
                                                      blockToLocalAxis(edit.position.y),
                                                      blockToLocalAxis(edit.position.z));
            if ((chunk->isBlockWhole(blockIndex) || restore) && isRelightBlocked(edit.position)) {
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
