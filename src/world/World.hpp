#pragma once

// The voxel world: a BlockAccess over the resident chunk set, plus the gameplay
// edit API.
//
// World is the only type the application, physics and interaction code talk to.
// It owns the ChunkManager (and therefore the chunk map) and adds the things
// that need a world-space view of several chunks at once: floor-divided
// coordinate lookups, edits that dirty the neighbour across a chunk seam, and
// the deferral of edits that would race a mesh job.
//
// THREADING
// ---------
// Main thread only: setBlock, update, unloadAll, the wiring setters.
// Any thread:       getBlock, getLight, chunkAt, isChunkReady, stats,
//                   captureNeighbourhood - all of which go through the
//                   ChunkManager's shared_mutex.

#include "core/JobSystem.hpp"
#include "world/Block.hpp"
#include "world/BlockAccess.hpp"
#include "world/Chunk.hpp"
#include "world/ChunkManager.hpp"
#include "world/VoxelTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace voxl {

/// Why a setBlock did not take effect immediately.
enum class EditResult : std::uint8_t {
    /// Written, and every affected chunk was marked for remesh and for save.
    Applied = 0,
    /// A worker owns the chunk right now. The edit is queued and will be applied
    /// at the start of a later update; treat it as accepted.
    Deferred = 1,
    /// Outside the world's vertical bounds.
    OutOfBounds = 2,
    /// The chunk is not resident, so there is nothing to edit. The player cannot
    /// reach such a block, so this only happens to scripted or test edits.
    NotLoaded = 3,
    /// The deferral queue is full; the edit was dropped.
    Rejected = 4,
};

[[nodiscard]] constexpr const char* toString(EditResult result) noexcept
{
    switch (result) {
        case EditResult::Applied:     return "Applied";
        case EditResult::Deferred:    return "Deferred";
        case EditResult::OutOfBounds: return "OutOfBounds";
        case EditResult::NotLoaded:   return "NotLoaded";
        case EditResult::Rejected:    return "Rejected";
    }
    return "Unknown";
}

class World final : public BlockAccess, public BlockAccessOps<World> {
public:
    /// Edits that cannot be applied at once are held here. The bound exists only
    /// so that a scripted edit storm against a chunk stuck in Meshing cannot grow
    /// the queue without limit.
    static constexpr std::size_t kMaxDeferredEdits = 4096;

    /// `jobs` and `registry` must outlive the world. The registry must already be
    /// finalised - workers read it without synchronisation.
    World(JobSystem& jobs, const BlockRegistry& registry, const StreamingConfig& config = {});

    World(const World&)            = delete;
    World& operator=(const World&) = delete;
    World(World&&)                 = delete;
    World& operator=(World&&)      = delete;

    // ---- BlockAccess (any thread) ----

    /// Never fails; out-of-world and unloaded reads return the values fixed by
    /// src/world/BlockAccess.hpp.
    [[nodiscard]] BlockId      getBlock(const BlockPos& pos) const noexcept override;
    [[nodiscard]] std::uint8_t getLight(const BlockPos& pos) const noexcept override;

    // ---- gameplay queries ----

    [[nodiscard]] const BlockRegistry& blockRegistry() const noexcept { return m_registry; }
    [[nodiscard]] const BlockType&     blockType(const BlockPos& pos) const noexcept
    {
        return m_registry.get(getBlock(pos));
    }
    [[nodiscard]] bool isSolid(const BlockPos& pos) const noexcept
    {
        return m_registry.isSolid(getBlock(pos));
    }
    [[nodiscard]] bool isOpaque(const BlockPos& pos) const noexcept
    {
        return m_registry.isOpaque(getBlock(pos));
    }
    [[nodiscard]] bool isLiquid(const BlockPos& pos) const noexcept
    {
        return m_registry.isLiquid(getBlock(pos));
    }
    /// True when a placement may overwrite this block. Liquids are included here
    /// but are deliberately NOT `replaceable` in the registry, because the
    /// interaction raycast has to be able to skip them without also skipping air.
    [[nodiscard]] bool isReplaceable(const BlockPos& pos) const noexcept
    {
        const BlockType& type = blockType(pos);
        return type.replaceable || type.liquid;
    }

    // ---- gameplay edits (main thread) ----

    /// Writes one voxel.
    ///
    /// Bumps the chunk's content version and marks it for remesh and for save
    /// (Chunk::setBlock does that part), then marks every neighbouring chunk the
    /// edit is visible from as needing a remesh. Skipping that leaves a stale face
    /// frozen into the neighbour's mesh at the seam.
    EditResult setBlock(const BlockPos& pos, BlockId id);

    /// Convenience for the interaction code: fails rather than replacing a block
    /// that is not replaceable.
    EditResult placeBlock(const BlockPos& pos, BlockId id);
    EditResult breakBlock(const BlockPos& pos) { return setBlock(pos, blocks::Air); }

    [[nodiscard]] std::size_t deferredEditCount() const noexcept { return m_deferredEdits.size(); }

    // ---- streaming (main thread) ----

    /// One streaming step. Applies whatever edits were deferred first, so a
    /// deferred edit is visible to this frame's meshing decisions.
    void update(const StreamingView& view, std::uint64_t frameIndex);
    /// Uses an internal monotonic frame counter; for callers with no FrameClock.
    void update(const StreamingView& view) { update(view, ++m_frameIndex); }

    [[nodiscard]] std::uint64_t frameIndex() const noexcept { return m_frameIndex; }

    void unloadAll();

    // ---- chunk access ----

    [[nodiscard]] ChunkManager&       chunks() noexcept { return m_chunks; }
    [[nodiscard]] const ChunkManager& chunks() const noexcept { return m_chunks; }

    [[nodiscard]] ChunkPtr chunkAt(const ChunkPos& position) const { return m_chunks.find(position); }
    [[nodiscard]] ChunkPtr chunkContaining(const BlockPos& pos) const
    {
        return m_chunks.find(toChunkPos(pos));
    }
    [[nodiscard]] bool isChunkReady(const ChunkPos& position) const;

    [[nodiscard]] ChunkNeighbourhood captureNeighbourhood(const ChunkPos& centre) const
    {
        return m_chunks.captureNeighbourhood(centre);
    }

    // ---- wiring, forwarded for the application's convenience ----

    void setGenerator(ChunkGenerateFn generator) { m_chunks.setGenerator(std::move(generator)); }
    void setMesher(ChunkMeshFn mesher) { m_chunks.setMesher(std::move(mesher)); }
    void setMeshReleaser(ChunkReleaseFn release) { m_chunks.setMeshReleaser(std::move(release)); }
    void setRetireHook(ChunkRetireFn retire) { m_chunks.setRetireHook(std::move(retire)); }

    // ---- observation ----

    [[nodiscard]] WorldStats       stats() const;
    [[nodiscard]] WorldMemoryStats memoryStats() const { return m_chunks.memoryStats(); }
    void setVisibleChunkCount(std::size_t count) noexcept
    {
        m_chunks.setVisibleChunkCount(count);
    }

private:
    struct PendingEdit {
        BlockPos position{};
        BlockId  id = blocks::Air;
    };

    /// Applies the edit to an already-resolved chunk, including the neighbour
    /// dirty marks. Caller has verified the chunk is writable.
    void writeBlock(const ChunkPtr& chunk, const BlockPos& pos, BlockId id);

    /// True when writing to `chunk` right now would race a worker thread.
    ///
    /// It is not enough to test the edited chunk's own state. A mesh job for a
    /// neighbouring chunk captures a 3x3x3 ChunkNeighbourhood and reads a
    /// one-voxel skirt out of all 26 surrounding chunks, which stay Ready while
    /// that job runs. ChunkStorage reallocates its palette and index vectors as
    /// the palette grows, so a concurrent write is a use-after-free in the
    /// worker, not merely a torn read. A chunk is therefore writable only when
    /// no chunk in its own 3x3x3 neighbourhood is meshing - that set is exactly
    /// the set of chunks whose mesh job could hold a pointer to it.
    [[nodiscard]] bool isEditBlocked(const ChunkPtr& chunk, const ChunkPos& position) const;

    /// Marks every chunk whose mesh depends on `pos` as needing a remesh. That is
    /// up to seven neighbours, not one: a face is culled against its direct
    /// neighbour, but ambient occlusion samples the diagonals too, so an edited
    /// corner voxel changes the shading of quads in three chunks that touch it
    /// only edge-on.
    void markSeamNeighboursDirty(const BlockPos& pos);

    /// Retries the queue. Returns the number still waiting.
    std::size_t applyDeferredEdits();

    JobSystem&           m_jobs;
    const BlockRegistry& m_registry;
    ChunkManager         m_chunks;

    std::vector<PendingEdit> m_deferredEdits;
    std::uint64_t            m_frameIndex = 0;
};

}  // namespace voxl
