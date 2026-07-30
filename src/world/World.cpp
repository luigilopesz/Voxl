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
        m_deferredEdits.push_back(PendingEdit{pos, id, 0, EditKind::Block});
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

    // Identical rule to setBlock, and for a stronger reason: SubVoxelStore is an
    // unordered_map that rehashes on insert, so a carve landing while a
    // neighbour's mesh job walks the store is a use-after-free, not a torn read.
    if (isEditBlocked(chunk, chunkPos)) {
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
        switch (edit.kind) {
            case EditKind::Block:
                writeBlock(chunk, edit.position, edit.id);
                break;
            case EditKind::SubVoxelBreak:
                writeSubVoxel(chunk, edit.position, edit.subIndex, blocks::Air, false);
                break;
            case EditKind::SubVoxelRestore:
                writeSubVoxel(chunk, edit.position, edit.subIndex, edit.id, true);
                break;
        }
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
