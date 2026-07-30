#include "world/World.hpp"

#include "core/Log.hpp"

namespace voxl {

World::World(JobSystem& jobs, const BlockRegistry& registry, const StreamingConfig& config)
    : m_jobs(jobs), m_registry(registry), m_chunks(jobs, config)
{
    m_deferredEdits.reserve(64);
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

    if (isEditBlocked(chunk, toChunkPos(pos))) {
        if (m_deferredEdits.size() >= kMaxDeferredEdits) {
            VOXL_LOG_WARN("dropping edit at ({}, {}, {}): deferral queue full", pos.x, pos.y, pos.z);
            return EditResult::Rejected;
        }
        m_deferredEdits.push_back(PendingEdit{pos, id});
        return EditResult::Deferred;
    }

    writeBlock(chunk, pos, id);
    return EditResult::Applied;
}

bool World::isEditBlocked(const ChunkPtr& chunk, const ChunkPos& position) const
{
    // Empty and Generating are owned by a generator that is about to overwrite
    // everything; Meshing is owned by a mesher reading this chunk's palette.
    const ChunkState state = chunk->state();
    if (state == ChunkState::Empty || isChunkBusy(state)) {
        return true;
    }

    // A mesh job running for any chunk in the surrounding 3x3x3 holds a
    // ConstChunkPtr to this one and reads its voxels as a border skirt.
    for (std::int32_t dy = -1; dy <= 1; ++dy) {
        const std::int32_t ny = position.y + dy;
        if (ny < 0 || ny >= kWorldSectionCount) {
            continue;
        }
        for (std::int32_t dz = -1; dz <= 1; ++dz) {
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0 && dz == 0) {
                    continue;  // already tested above
                }
                const ChunkPtr other = m_chunks.find(ChunkPos{position.x + dx, ny, position.z + dz});
                if (other != nullptr && other->state() == ChunkState::Meshing) {
                    return true;
                }
            }
        }
    }
    return false;
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
    // Chunk::setBlock already bumps the content version and sets both the remesh
    // and the save flag for this chunk, so only the seam neighbours are left.
    chunk->setBlock(blockToLocalAxis(pos.x), blockToLocalAxis(pos.y), blockToLocalAxis(pos.z), id);
    markSeamNeighboursDirty(pos);
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
    for (const PendingEdit& edit : m_deferredEdits) {
        const ChunkPtr chunk = m_chunks.find(toChunkPos(edit.position));
        if (chunk == nullptr) {
            continue;  // the chunk left the world; the edit is meaningless now
        }
        if (chunk->state() == ChunkState::Unloading) {
            continue;
        }
        if (isEditBlocked(chunk, toChunkPos(edit.position))) {
            retry.push_back(edit);
            continue;
        }
        writeBlock(chunk, edit.position, edit.id);
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
    m_chunks.update(view, frameIndex);
}

void World::unloadAll()
{
    m_deferredEdits.clear();
    m_chunks.unloadAll();
}

WorldStats World::stats() const
{
    WorldStats out       = m_chunks.stats();
    out.deferredEdits    = m_deferredEdits.size();
    return out;
}

}  // namespace voxl
