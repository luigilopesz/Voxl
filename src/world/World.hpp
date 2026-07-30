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

//
// LIGHTING
// --------
// World owns the main-thread half of the light engine. The worker half - the
// initial lighting of a freshly streamed column, and of a LOD shadow - is wired
// into the ChunkManager here, because World is the first place that has both the
// BlockRegistry and the streamer.
//
// Two things happen on this thread. An edit relights its own neighbourhood
// immediately, inside setBlock, so the block the player just broke is lit in the
// same frame; and light that a worker computed but was not allowed to write -
// because it crossed into a chunk somebody else owned - is applied at the start
// of the next update.

#include "core/JobSystem.hpp"
#include "world/Block.hpp"
#include "world/BlockAccess.hpp"
#include "world/Chunk.hpp"
#include "world/ChunkManager.hpp"
#include "world/LightEngine.hpp"
#include "world/Lod.hpp"
#include "world/SubVoxel.hpp"
#include "world/VoxelTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

#include <glm/vec3.hpp>

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

    // ---- sub-voxel edits (main thread) ----
    //
    // setBlock's sibling for partial destruction. Everything setBlock guarantees
    // applies unchanged: the same vertical-bounds and residency checks, the same
    // deferral when a worker owns the chunk or one of its neighbours (see
    // isEditBlocked), and the same seam-neighbour invalidation when the damaged
    // block sits on a chunk border. A sub-voxel on a border face changes the
    // neighbour's culling and ambient occlusion exactly as a whole block would.
    //
    // The store/storage invariant itself is NOT maintained here - it belongs to
    // Chunk::breakSubVoxel/restoreSubVoxel, which are the only sanctioned way to
    // touch it (see world/SubVoxel.hpp). World's job is the world-space view:
    // finding the chunk, the threading rule, and the neighbours.

    /// Carves one sub-voxel out of the block at `pos`. `subIndex` comes from
    /// voxl::subVoxelIndex() and must be < kSubVoxelCount. Breaking the last
    /// sub-voxel turns the block to air, which Chunk::breakSubVoxel handles.
    EditResult breakSubVoxel(const BlockPos& pos, std::size_t subIndex);

    /// Puts one sub-voxel back. `material` is used only when the block is
    /// currently air, i.e. when this is the first sub-voxel of a rebuild.
    EditResult restoreSubVoxel(const BlockPos& pos, std::size_t subIndex, BlockId material);

    /// World-space convenience for the interaction code, which has a ray hit
    /// point rather than a block-plus-index pair.
    EditResult breakSubVoxelAt(const glm::vec3& worldPosition);
    EditResult restoreSubVoxelAt(const glm::vec3& worldPosition, BlockId material);

    /// Damage grid for the block at `pos`, or nullptr when the block is uniform
    /// (air, or solid and intact). Callers MUST handle nullptr by falling back
    /// to the block id - see the invariant in world/SubVoxel.hpp.
    [[nodiscard]] const SubVoxelGrid* subVoxelsAt(const BlockPos& pos) const;

    /// False when the block at `pos` is partially destroyed, so collision and
    /// face culling must stop treating it as a full cube.
    [[nodiscard]] bool isBlockWhole(const BlockPos& pos) const;

    [[nodiscard]] std::size_t deferredEditCount() const noexcept { return m_deferredEdits.size(); }

    // ---- lighting (main thread) ----

    [[nodiscard]] bool lightingEnabled() const noexcept { return m_lighting; }

    /// What the last incremental relight cost. Zeroed by an edit that changed no
    /// light property at all, which is most of them.
    [[nodiscard]] const LightUpdateStats& lastLightUpdate() const noexcept
    {
        return m_lastLightUpdate;
    }

    /// Spilled light still waiting to be applied. Should hover near zero once
    /// streaming settles; a figure that keeps growing means the flood is being
    /// refused faster than it is being retried.
    [[nodiscard]] std::size_t pendingLightSeeds() const noexcept { return m_pendingLight.size(); }

    /// Column boundaries that were lit blind and still have to exchange light.
    /// Rises while chunks stream in and must fall back to zero once they stop;
    /// a figure that never settles means seams are being recorded faster than
    /// they can be reconciled. See BlindSeam.
    [[nodiscard]] std::size_t pendingLightSeams() const
    {
        const std::lock_guard<std::mutex> lock(m_blindSeamMutex);
        return m_blindSeams.size();
    }

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

    // ---- level of detail ----

    [[nodiscard]] const LodPolicy& lodPolicy() const noexcept { return m_chunks.lodPolicy(); }
    void setLodPolicy(const LodPolicy& policy) { m_chunks.setLodPolicy(policy); }

    /// Resolution the chunk containing `pos` is currently built at, or kLodFull
    /// when nothing is resident there. Physics and interaction should use this
    /// to refuse to act on coarse terrain: a level-2 chunk's voxels are a 4x4x4
    /// approximation and a block break there would carve a hole the size of a
    /// house once the chunk came back to full resolution.
    [[nodiscard]] LodLevel lodAt(const BlockPos& pos) const;

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
    /// What a deferred edit was, so the retry does the same thing the original
    /// call would have. One queue for both kinds rather than two, because the
    /// ORDER matters: a whole-block place over a partially destroyed block and
    /// the sub-voxel carve that damaged it must be replayed in the order the
    /// player made them.
    enum class EditKind : std::uint8_t { Block, SubVoxelBreak, SubVoxelRestore };

    struct PendingEdit {
        BlockPos      position{};
        BlockId       id       = blocks::Air;
        std::uint16_t subIndex = 0;
        EditKind      kind     = EditKind::Block;
    };

    /// Applies the edit to an already-resolved chunk, including the neighbour
    /// dirty marks. Caller has verified the chunk is writable.
    void writeBlock(const ChunkPtr& chunk, const BlockPos& pos, BlockId id);

    /// Sub-voxel counterpart of writeBlock. Returns what the chunk reported so
    /// the caller can skip dirtying anything when nothing actually changed.
    SubVoxelEdit writeSubVoxel(const ChunkPtr& chunk, const BlockPos& pos, std::size_t subIndex,
                               BlockId material, bool restore);

    /// Shared front half of the two public sub-voxel entry points: bounds,
    /// residency, and the defer-or-apply decision.
    EditResult editSubVoxel(const BlockPos& pos, std::size_t subIndex, BlockId material,
                            bool restore);

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
    ///
    /// Since lighting arrived there is a second kind of worker reader: a column
    /// light job reads the light of every chunk in a 3x3 block of columns. The
    /// rule is unchanged, only the set is bigger, and
    /// ChunkManager::isNeighbourhoodBusy answers for both in one lock.
    [[nodiscard]] bool isEditBlocked(const ChunkPtr& chunk, const ChunkPos& position) const;

    /// True when an incremental relight around `pos` would have to write a chunk
    /// a worker owns.
    ///
    /// Light reaches 15 blocks, so HORIZONTALLY the relight never leaves the
    /// 3x3x3 isEditBlocked already covers. Vertically it does: opening or cutting
    /// a sunlit column changes every cell beneath it down to the first blocker,
    /// which can be most of the world's height. Those extra sections have to be
    /// tested too, or the removal pass stops at a chunk border and leaves a
    /// column of light hanging in the air with nothing to take it away.
    [[nodiscard]] bool isRelightBlocked(const BlockPos& pos) const;

    /// True when the sub-voxel edit about to be made would make writeSubVoxel
    /// relight, so the caller knows it must test the taller relight footprint.
    ///
    /// THIS PREDICATE MUST STAY IN STEP WITH THE RELIGHT CONDITION AT THE FOOT OF
    /// writeSubVoxel. It used to be approximated by "the block is still whole, or
    /// this is a restore", on the reasoning that an already-damaged block is
    /// already transparent to the light engine so chipping it further moves no
    /// light. That is true of every carve but one: clearing the LAST remaining
    /// sub-voxel turns the block to air, which is a full opacity change and opens
    /// the sunlight shaft under it - and it is the final act of mining any block,
    /// so it is the most common edit in the game. The approximation let that carve
    /// skip isRelightBlocked and run a world-height flood into chunks a worker
    /// owns, which is invariant 1 (no writing to a chunk a worker may be reading).
    ///
    /// Only safe to call once isEditBlocked() has answered false: it reads the
    /// chunk's storage and sub-voxel store.
    [[nodiscard]] static bool subVoxelEditMayRelight(const ChunkPtr& chunk, std::size_t blockIndex,
                                                     std::size_t subIndex, BlockId material,
                                                     bool restore);

    /// Rebuilds the light around a voxel that just changed, then marks every
    /// chunk whose light moved - and its seam neighbours - for a remesh.
    void relightAround(const BlockPos& pos);

    /// Applies whatever a light worker could not write itself, within a budget.
    void applyPendingLight();

    // ---- blind column seams ----
    //
    // A column light job may only read neighbouring chunks that are final in
    // BOTH voxels and light. A neighbour column that is being lit at the same
    // moment is neither, so it arrives as a null region slot, which the engine
    // has no choice but to treat as a solid wall. That is safe, and it is also
    // supposed to be self-correcting: whichever column is lit second sees the
    // first, reads its light, and spills back across the boundary.
    //
    // TWO COLUMNS LIT AT THE SAME TIME BREAK THAT ARGUMENT. Neither is second.
    // Each sees the other as a wall, neither reads the other, and neither spills
    // into the other - and because published light is never recomputed, the seam
    // between them stays dark permanently. It is not a rare interleaving either:
    // the streaming sweep claims several columns per update, so adjacent columns
    // being in flight together is the normal case at the loading frontier.
    //
    // The root fix belongs in ChunkManager::claimColumnLight, which is what
    // decides that two neighbours may run concurrently (see the note at the top
    // of the seam code in World.cpp). What follows is the main thread doing
    // afterwards, once both columns are resident and lit, exactly the exchange
    // the two workers were not allowed to do at the time: read both sides of the
    // boundary and queue the light that should have crossed as ordinary spill
    // seeds. It is idempotent and self-cancelling - a seam with nothing to move
    // produces no seeds - so it stays correct, and costs almost nothing, if the
    // scheduler is fixed underneath it.

    struct BlindSeam {
        /// The lower of the two columns; the neighbour is at +dx / +dz. Storing
        /// it in this canonical form is what lets the two jobs either side of one
        /// boundary record the same entry, so the duplicate can be dropped.
        ColumnPos     column{};
        std::int32_t  dx       = 0;  ///< exactly one of dx, dz is 1; the other 0
        std::int32_t  dz       = 0;
        std::uint16_t attempts = 0;  ///< gives up on a section that is never lit
    };

    /// WORKER THREAD. Records which neighbour columns `work` had to treat as a
    /// wall, so the main thread can settle the boundary later.
    void noteBlindSeams(const LightColumnWork& work);

    /// Settles recorded seams within a per-update cell budget.
    void reconcileBlindSeams();

    /// One seam. Returns false when a section on either side is resident but not
    /// lit yet, so the seam has to be tried again. `scanned` accumulates the
    /// cells looked at, which is what the budget is spent in.
    [[nodiscard]] bool reconcileSeam(const BlindSeam& seam, std::size_t& scanned);

    /// Queues the light that should cross the shared face from `from` into `to`.
    /// `dx`/`dz` point from `from` toward `to`. Returns the cells examined.
    std::size_t seedAcrossSeam(const Chunk& from, const Chunk& to, std::int32_t dx,
                               std::int32_t dz);

    /// Dirties the chunks LightWorld recorded, plus the neighbours that sample
    /// across the faces it touched.
    void markLitChunksDirty();

    /// Wires the worker half of the light engine into the ChunkManager.
    void installLighter();

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

    /// Main-thread light engine. The workers use their own thread_local ones;
    /// this instance only ever runs on the thread that owns the edits.
    LightEngine m_light;
    LightWorld  m_lightWorld;
    bool        m_lighting = true;

    /// Light a worker produced that crossed into a chunk it did not own.
    ///
    /// Applying it is main-thread work, so it is budgeted rather than done all at
    /// once: a cold start can spill thousands of border cells in a single update
    /// and blowing the frame for light nobody can see yet would be a worse bug
    /// than the seam it fixes.
    std::vector<LightSeed> m_pendingLight;
    LightUpdateStats       m_lastLightUpdate;

    /// Spill seeds applied per update. Each one is a compare and, when it wins,
    /// a short flood; 4096 is a few tens of microseconds.
    static constexpr std::size_t kLightSeedsPerUpdate = 4096;
    /// Ceiling on the backlog. Reached only if the flood is refused faster than
    /// it is retried, which means something is wedged; dropping the excess costs
    /// a dim seam rather than unbounded memory.
    static constexpr std::size_t kMaxPendingLightSeeds = 1u << 16;

    /// Boundaries recorded by the light workers, waiting to be settled. Written
    /// from workers, drained by update(), hence the lock.
    std::vector<BlindSeam> m_blindSeams;
    mutable std::mutex     m_blindSeamMutex;

    /// Cells one update may look at while settling seams. A boundary between two
    /// uniform sections - solid rock at zero, open sky at fifteen, which is most
    /// of a column - is decided in two loads and spends none of this, so the
    /// budget only ever bites on the handful of sections that hold real detail.
    static constexpr std::size_t kSeamCellsPerUpdate = 1u << 15;
    /// Updates a seam may wait for a section that never becomes lit before it is
    /// dropped. A tolerant claim can leave a section unlit indefinitely, and a
    /// seam that waits for it forever would be retried forever.
    static constexpr std::uint16_t kMaxSeamAttempts = 600;
    /// Ceiling on the backlog, for the same reason as the light seed one.
    static constexpr std::size_t kMaxBlindSeams = 4096;

    std::vector<PendingEdit> m_deferredEdits;
    std::uint64_t            m_frameIndex = 0;
};

}  // namespace voxl
