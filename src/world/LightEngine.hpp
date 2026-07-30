#pragma once

// Voxel light: sunlight from the sky, block light from emissive blocks, and the
// incremental update that keeps both correct after the player edits the world.
//
// TWO ENTRY POINTS, TWO THREADING RULES
// -------------------------------------
//  * `lightColumn` / `lightChunk` run on a WORKER and rebuild a chunk's light
//    from scratch. They may only be handed chunks the caller owns exclusively -
//    a section still in ChunkState::Generating, or a LOD shadow nobody can see
//    yet. Everything else they touch is read-only.
//  * `applySeeds` / `voxelChanged` run on the MAIN THREAD against the live
//    world and write light into whatever chunks the flood reaches. They must be
//    called only when World::isEditBlocked() says the affected chunks are
//    writable, for exactly the reason that rule exists (see World.hpp). Both
//    open a LightWorld::Pass for their duration, which is what bounds the
//    lifetime of the reader's chunk memo - see LightWorld.
//
// THE LIGHT MODEL
// ---------------
//  * Sunlight enters the top of the world at level 15 and falls straight down
//    through fully transparent air with NO loss, which is what makes open
//    ground uniformly bright. Any other direction costs 1 per block. A block
//    with BlockType::lightAttenuation costs that much extra on entry. An opaque
//    block cannot be entered at all.
//  * Block light is an ordinary BFS from every block with lightEmission > 0,
//    costing 1 per block in all six directions. The emitting cell holds its own
//    emission even when the block is opaque, so a glowstone block lights the
//    room it is embedded in.
//  * A PARTIALLY DESTROYED BLOCK IS TREATED AS FULLY TRANSPARENT - no opacity,
//    no attenuation, whatever its material. Two reasons. The mesher already
//    rewrites such a block to air in its cache (see GreedyMesher::loadCacheFull),
//    so anything else would light geometry that is not drawn and cull faces that
//    are; and occupancy-proportional attenuation would make the light level
//    change on every chip, i.e. a chunk-wide relight and remesh for each of the
//    512 sub-voxels a player grinds through. Transparent is the choice that
//    agrees with what is on screen and is stable while the player digs.

#include "world/Block.hpp"
#include "world/BlockAccess.hpp"
#include "world/Chunk.hpp"
#include "world/ChunkStorage.hpp"
#include "world/Lod.hpp"
#include "world/VoxelTypes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace voxl {

namespace detail {
struct LightScratch;
}

/// Brightest level either channel can hold. Named here so lighting code does not
/// have to spell out the storage layout constant.
inline constexpr std::uint8_t kFullLight = ChunkStorage::kMaxLightLevel;

/// Coarsest LOD level that still gets a full per-voxel flood.
///
/// LOD POLICY. A level-2 chunk is an 8x8x8 approximation of 32768 blocks and
/// lives at least 9 chunks (288 blocks) from the player; a level-3 one at least
/// 14. At those distances the only lighting anybody can resolve is "is this
/// surface under the sky or not", so levels 2 and 3 get a top-down sky-exposure
/// sweep - one pass down each of the 1024 columns, no queue at all - plus a
/// block-light flood only when the chunk actually contains an emissive block.
/// Levels 0 and 1 get the real thing.
///
/// This cannot disagree visibly at a band boundary: the sweep and the flood
/// produce the SAME value for any cell whose light arrives from directly above,
/// which is every surface cell in an outdoor chunk. They differ only where light
/// had to turn a corner - cave interiors and the undersides of overhangs - and
/// those are not visible from 288 blocks away. The band edge itself is drawn
/// with skirts, which take their shading from the surface cell above them.
inline constexpr LodLevel kMaxPropagatedLod = 1;

// ------------------------------------------------------------------- seeds --

/// One cell of light that has to be pushed into a chunk the producer was not
/// allowed to write.
///
/// A worker rebuilding a chunk's light may only write that chunk. Light that
/// crosses out of it into an already-lit neighbour therefore comes back to the
/// main thread as a list of these, and is applied with the ordinary propagation
/// pass. `sunlight` / `blockLight` are the levels the target cell should reach,
/// not the levels of the cell that produced them.
struct LightSeed {
    BlockPos     position{};
    std::uint8_t sunlight   = 0;
    std::uint8_t blockLight = 0;
};

using LightSpill = std::vector<LightSeed>;

// -------------------------------------------------------------- LightWorld --

/// Mutable, MAIN-THREAD view of the resident chunks the light engine may write.
///
/// Built from two callables rather than from a World reference so that the very
/// same propagation and removal code runs over the live streaming world and over
/// a handful of chunks in a unit test. `lookup` returns null for a chunk that is
/// not resident; `writable` answers "may the main thread write this chunk right
/// now", which for the live world is World::isEditBlocked inverted.
///
/// The class also records which chunks it changed and which of their faces, so
/// the caller can mark exactly the right meshes stale. A light change one block
/// inside a chunk is invisible to its neighbours; one ON a chunk face is read by
/// the neighbour's mesher as part of its skirt, and leaving it unmarked freezes a
/// lighting discontinuity into the seam.
///
/// THE MEMO IS SCOPED TO A PASS, AND THAT IS A CORRECTNESS RULE, NOT A TUNING
/// KNOB. `resolve()` memoises the last chunk it looked up together with the
/// verdict on whether the main thread may write it. Both answers are only true
/// of an instant: a chunk that was writable this frame may be owned by a mesh or
/// a light job the next, and one that was resident may have been retired and
/// replaced. A memo that outlives the pass that filled it therefore writes light
/// into a chunk a worker owns - invariant 1 in Chunk.hpp - and, because the memo
/// holds a shared_ptr, keeps an unloaded chunk alive to absorb writes the
/// resident replacement never sees.
///
/// So the memo only exists inside a `LightWorld::Pass`, which clears it on entry
/// AND on exit, and the two main-thread entry points (LightEngine::voxelChanged
/// and LightEngine::applySeeds) open one themselves. Outside a pass every query
/// is a fresh lookup: forgetting a Pass costs speed, never correctness, which is
/// the only failure mode worth having.
class LightWorld {
public:
    using LookupFn   = std::function<ChunkPtr(const ChunkPos&)>;
    using WritableFn = std::function<bool(const ChunkPos&)>;

    /// A chunk whose light this pass changed, plus which of its six faces were
    /// touched. Bit `static_cast<int>(Direction)` is set when a cell on that face
    /// slab changed.
    struct Touched {
        ChunkPos     position{};
        std::uint8_t faceMask = 0;
    };

    LightWorld(LookupFn lookup, WritableFn writable);

    [[nodiscard]] BlockId      blockAt(const BlockPos& pos) const;
    [[nodiscard]] std::uint8_t lightAt(const BlockPos& pos) const;
    [[nodiscard]] std::uint8_t sunlightAt(const BlockPos& pos) const
    {
        return ChunkStorage::unpackSunlight(lightAt(pos));
    }
    [[nodiscard]] std::uint8_t blockLightAt(const BlockPos& pos) const
    {
        return ChunkStorage::unpackBlockLight(lightAt(pos));
    }

    /// False when the block is partially destroyed, i.e. when the light engine
    /// must ignore its material and treat it as empty space.
    [[nodiscard]] bool wholeAt(const BlockPos& pos) const;

    /// True when `pos` is inside the world, resident, and writable right now.
    [[nodiscard]] bool writableAt(const BlockPos& pos) const;

    /// Writes one nibble, leaving the other alone. Returns false and counts a
    /// refusal when the target chunk is not writable, which is the signal that
    /// the relight is incomplete and somebody has to retry.
    bool setSunlightAt(const BlockPos& pos, std::uint8_t level);
    bool setBlockLightAt(const BlockPos& pos, std::uint8_t level);

    [[nodiscard]] const std::vector<Touched>& touched() const noexcept { return m_touched; }
    void clearTouched() noexcept { m_touched.clear(); }

    /// Number of writes that were refused because the target chunk was busy.
    /// Non-zero means the relight is incomplete and something has to retry.
    [[nodiscard]] std::size_t refusedWrites() const noexcept { return m_refused; }
    void resetCounters() noexcept { m_refused = 0; }

    /// Forgets the memoised chunk, its owning shared_ptr and its writability
    /// verdict, so the next query asks the world again.
    ///
    /// Called for you at both ends of a `Pass`. Calling it by hand is only ever
    /// needed if something outside this file learns that residency changed in
    /// the middle of one, which nothing on the main thread can currently do.
    void invalidate() const noexcept;

    /// The window in which the memo is allowed to answer.
    ///
    /// One pass is one main-thread operation: a flood walks neighbouring cells,
    /// so consecutive queries land in the same chunk overwhelmingly often and
    /// the memo turns a hash lookup plus a shared_ptr copy into two integer
    /// compares. Nothing can change residency or writability underneath it -
    /// every marker that makes a chunk unwritable is established on the main
    /// thread, and this IS the main thread - so within the pass the memo is
    /// exact. Across a frame boundary it is a lie; hence the destructor.
    ///
    /// Nestable, so a caller may hold one open across several engine calls that
    /// each open their own.
    class Pass {
    public:
        explicit Pass(const LightWorld& world) noexcept : m_world(&world)
        {
            world.invalidate();
            ++world.m_passDepth;
        }
        ~Pass()
        {
            if (--m_world->m_passDepth == 0) {
                m_world->invalidate();
            }
        }

        Pass(const Pass&)            = delete;
        Pass& operator=(const Pass&) = delete;
        Pass(Pass&&)                 = delete;
        Pass& operator=(Pass&&)      = delete;

    private:
        const LightWorld* m_world;
    };

    /// True while a Pass is open. Tests assert on it; nothing else needs it.
    [[nodiscard]] bool inPass() const noexcept { return m_passDepth != 0; }

private:
    /// Resolves `pos` to its chunk. See the class comment for why the memo this
    /// fills is only readable inside a Pass.
    struct Resolved {
        Chunk* chunk    = nullptr;
        bool   writable = false;
    };
    [[nodiscard]] const Resolved& resolve(const ChunkPos& position) const;

    bool setNibble(const BlockPos& pos, std::uint8_t level, bool sun);

    void noteTouched(const ChunkPos& position, std::int32_t lx, std::int32_t ly, std::int32_t lz);

    LookupFn   m_lookup;
    WritableFn m_writable;

    mutable ChunkPos m_cachedPos{0, -1, 0};  // y is never negative for a real chunk
    /// Keeps the resolved chunk alive for as long as the raw pointer beside it is
    /// in use. Dropped by invalidate(), which is what stops a retired chunk being
    /// held - and written to - after its replacement has taken its place.
    mutable ChunkPtr m_cachedOwner;
    mutable Resolved m_cached;
    /// Set only while a Pass is open; see the class comment.
    mutable bool         m_cacheValid = false;
    mutable std::int32_t m_passDepth  = 0;

    std::vector<Touched> m_touched;
    std::size_t          m_refused = 0;
};

// -------------------------------------------------------------- work items --

/// One column-lighting job: the sections a worker owns, plus everything it is
/// allowed to read while lighting them.
///
/// WHY A WHOLE COLUMN AND NOT A SECTION. Sunlight is a vertical phenomenon: the
/// level entering the top of section y is whatever left the bottom of section
/// y + 1. Lighting sections independently therefore forces a strict top-down
/// order, one section per streaming update, and a chunk lit before the one above
/// it is simply wrong until something relights it. Taking the whole column as one
/// job makes the sky exact in a single pass with no ordering constraint at all,
/// and costs one job dispatch and one snapshot per column instead of eight.
struct LightColumnWork {
    static constexpr std::size_t kRegionColumns = 9;  ///< the 3x3 of columns

    ColumnPos column{};

    /// Sections this job owns and will write, indexed by section y. Null where
    /// the section is not being lit - already lit, not resident, or its terrain
    /// is not written yet.
    std::array<ChunkPtr, kWorldSectionCount> targets{};

    /// Every chunk the job may read, including the targets. Null means "not
    /// available", which the engine treats as empty space in darkness rather
    /// than as the bright air BlockAccess uses for meshing: guessing bright
    /// would inflate light that nothing can ever take back, whereas guessing
    /// dark is a lower bound that the next pass corrects.
    std::array<ConstChunkPtr, kRegionColumns * kWorldSectionCount> region{};

    [[nodiscard]] static constexpr std::size_t regionIndex(std::int32_t dx, std::int32_t dz,
                                                           std::int32_t sectionY) noexcept
    {
        return static_cast<std::size_t>(((dz + 1) * 3 + (dx + 1)) * kWorldSectionCount + sectionY);
    }

    [[nodiscard]] const Chunk* at(std::int32_t dx, std::int32_t dz,
                                  std::int32_t sectionY) const noexcept
    {
        if (sectionY < 0 || sectionY >= kWorldSectionCount) {
            return nullptr;
        }
        return region[regionIndex(dx, dz, sectionY)].get();
    }

    [[nodiscard]] bool empty() const noexcept
    {
        for (const ChunkPtr& target : targets) {
            if (target != nullptr) {
                return false;
            }
        }
        return true;
    }
};

/// What an incremental pass actually did. Handed to the debug overlay and used
/// by the tests to prove the work really is bounded.
struct LightUpdateStats {
    std::size_t cellsCleared   = 0;  ///< visited by a removal BFS
    std::size_t cellsPropagated = 0;  ///< visited by a propagation BFS
    std::size_t writesRefused  = 0;  ///< target chunk was owned by a worker
    /// True when a BFS hit LightEngine::kMaxIncrementalCells and stopped early.
    /// The result is then merely a good approximation, and it is logged.
    bool exhausted = false;

    LightUpdateStats& operator+=(const LightUpdateStats& other) noexcept
    {
        cellsCleared += other.cellsCleared;
        cellsPropagated += other.cellsPropagated;
        writesRefused += other.writesRefused;
        exhausted = exhausted || other.exhausted;
        return *this;
    }
};

// ------------------------------------------------------------- LightEngine --

/// Sunlight and block-light propagation.
///
/// One instance is not thread safe - it owns a ~200 KB scratch volume and a set
/// of reusable queues, and every entry point uses them. Give each worker its own
/// (`thread_local LightEngine`, exactly as the mesher is wired) and keep one more
/// for the main thread.
class LightEngine {
public:
    /// Hard ceiling on the cells one BFS phase may visit.
    ///
    /// THE BOUND. Block light reaches 15 blocks, so a removal or a re-flood
    /// touches at most the L1 ball of radius 15 - 4991 cells - and a whole edit
    /// (remove then re-propagate, two channels) stays under ~20000. Sunlight is
    /// the only unbounded case: cutting a lit column drops the shaft below it all
    /// the way to the first blocker, up to 256 blocks, and each of those cells
    /// spreads 15 sideways. This ceiling stops a pathological edit at the top of a
    /// deep shaft from stalling a frame; hitting it leaves the region slightly too
    /// bright rather than wrong in a way that looks broken, and sets
    /// LightUpdateStats::exhausted.
    static constexpr std::size_t kMaxIncrementalCells = 1u << 17;  // 131072

    /// `registry` must be finalised and must outlive the engine; only the
    /// per-block opacity, attenuation and emission are copied out of it.
    explicit LightEngine(const BlockRegistry& registry);
    ~LightEngine();

    LightEngine(const LightEngine&)            = delete;
    LightEngine& operator=(const LightEngine&) = delete;
    LightEngine(LightEngine&&) noexcept;
    LightEngine& operator=(LightEngine&&) noexcept;

    // ---- worker side: rebuild from scratch ----

    /// Lights every section in `work.targets`, top down, and returns the light
    /// that crossed out into chunks the job did not own.
    ///
    /// WORKER THREAD. Each target must be exclusively owned by the caller.
    LightSpill lightColumn(const LightColumnWork& work);

    /// Lights one chunk from a 3x3x3 snapshot whose centre is that chunk. Used
    /// for the LOD shadow rebuild, where the replacement chunk is invisible to
    /// everything else until it is swapped in.
    ///
    /// WORKER THREAD, same ownership rule. `spill` may be null.
    void lightChunk(Chunk& chunk, const ChunkNeighbourhood& around, LightSpill* spill);

    // ---- main thread: incremental ----

    /// Brings the light around `pos` back into agreement with the voxels after
    /// the block (or its sub-voxel occupancy) at `pos` changed.
    ///
    /// MAIN THREAD. Runs the removal pass then the propagation pass for both
    /// channels; see the comment on `removeSunlight` for why a bare re-flood is
    /// not enough.
    LightUpdateStats voxelChanged(LightWorld& world, const BlockPos& pos);

    /// Pushes spill seeds produced by a worker into the live world.
    ///
    /// MAIN THREAD. A seed whose target is already at least that bright is
    /// dropped, so replaying a stale spill is harmless.
    LightUpdateStats applySeeds(LightWorld& world, const LightSeed* seeds, std::size_t count);

    // ---- material queries, exposed for tests and for the streaming gate ----

    [[nodiscard]] bool         opaque(BlockId id) const noexcept { return m_opaque[index(id)]; }
    [[nodiscard]] std::uint8_t attenuation(BlockId id) const noexcept
    {
        return m_attenuation[index(id)];
    }
    [[nodiscard]] std::uint8_t emission(BlockId id) const noexcept { return m_emission[index(id)]; }

    /// True when replacing `before` with `after` cannot change any light level,
    /// so the whole incremental pass can be skipped. Stone becoming dirt is the
    /// common case and it is a no-op for lighting.
    [[nodiscard]] bool affectsLight(BlockId before, BlockId after) const noexcept
    {
        return opaque(before) != opaque(after) || attenuation(before) != attenuation(after) ||
               emission(before) != emission(after);
    }

private:
    [[nodiscard]] std::size_t index(BlockId id) const noexcept
    {
        // Same fallback as BlockRegistry::get: an id from a newer save reads as
        // air rather than running off the end of the table.
        return id < m_opaque.size() ? static_cast<std::size_t>(id) : std::size_t{blocks::Air};
    }

    // ---- the two channels, shared code parameterised by a flag ----

    /// Cost of moving light of `level` into the cell at `target`, or a value
    /// greater than `level` when no light gets through.
    ///
    /// The sunlight special case lives here: a full-strength (15) sun ray moving
    /// DOWN into a cell with no attenuation of its own costs nothing, which is
    /// the entire reason open ground is uniformly bright instead of fading with
    /// depth.
    [[nodiscard]] static std::uint8_t stepCost(std::uint8_t level, bool sun, bool downward,
                                               std::uint8_t targetAttenuation) noexcept
    {
        const std::uint8_t base =
            (sun && downward && level == kFullLight) ? std::uint8_t{0} : std::uint8_t{1};
        return static_cast<std::uint8_t>(base + targetAttenuation);
    }

    // Worker-side volume passes.
    void loadSection(const Chunk& target, const LightColumnWork* work,
                     const ChunkNeighbourhood* around, std::int32_t sectionY);
    void seedSection(bool propagated);
    /// Writes the section outright when it is uniformly clear under an unbroken
    /// sky, which needs no flood at all. Returns false when the shape does not
    /// qualify and the general path has to run.
    bool trySkyFastPath(Chunk& target, const LightColumnWork* work,
                        const ChunkNeighbourhood* around, std::int32_t sectionY,
                        LightSpill* spill);
    void floodVolume(bool sun);
    void sweepSkyColumns();
    void writeSection(Chunk& target) const;
    void collectSpill(const BlockPos& origin, const LightColumnWork* work,
                      const ChunkNeighbourhood* around, std::int32_t sectionY,
                      LightSpill& spill) const;
    void lightSection(Chunk& target, const LightColumnWork* work,
                      const ChunkNeighbourhood* around, LightSpill* spill);

    /// Attenuation/opacity of one cell, honouring sub-voxel damage.
    void cellMaterial(const Chunk* chunk, std::size_t blockIndex, std::uint8_t& outAttenuation,
                      std::uint8_t& outOpaque, std::uint8_t& outEmission) const noexcept;

    // Main-thread world passes.
    LightUpdateStats removeAndRefill(LightWorld& world, const BlockPos& origin, bool sun);
    LightUpdateStats propagate(LightWorld& world, bool sun);

    /// The frontier of a main-thread flood. Positions rather than indices,
    /// because the flood crosses chunks; `level` is carried so a removal knows
    /// what the cell used to hold after it has already been cleared.
    struct Front {
        BlockPos     position{};
        std::uint8_t level = 0;
    };

    std::vector<std::uint8_t> m_opaque;
    std::vector<std::uint8_t> m_attenuation;
    std::vector<std::uint8_t> m_emission;

    std::unique_ptr<detail::LightScratch> m_scratch;

    /// Reused across calls so a steady stream of edits allocates nothing. Both
    /// are reserved once, in the constructor, to kMaxIncrementalCells' working
    /// set; `clear()` keeps the capacity.
    std::vector<Front> m_removeQueue;
    std::vector<Front> m_lightQueue;
};

}  // namespace voxl
