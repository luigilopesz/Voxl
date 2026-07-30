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
//
// LEVEL OF DETAIL
// ---------------
// Each resident chunk carries a LodLevel (world/Lod.hpp) chosen from its
// horizontal distance to the player. Changing that level means regenerating the
// voxels and rebuilding the geometry, which is far too slow to do in place while
// the player is looking at the result.
//
// So a transition is a SHADOW REBUILD. A brand-new Chunk object is created off
// to the side at the same position, generated at the new level and meshed on a
// worker; only when its geometry is ready does one main-thread closure swap the
// map slot and hand the new mesh to the renderer. Until that instant the old
// chunk is still in the map and its old mesh is still on the GPU, so the chunk
// never disappears for a frame - which is the single most visible way to get
// this wrong.
//
// Two properties make the shadow safe, and both are worth stating plainly:
//
//  * The shadow is invisible to everything else until the swap, so the terrain
//    job writes voxels nobody can read. There is no "write while a neighbour is
//    meshing" hazard on the generate half at all.
//  * The *mesh* half still reads a one-voxel skirt out of the 26 live
//    neighbours, so the main thread must not write them while it runs. That is
//    guaranteed by putting the LIVE chunk into ChunkState::Meshing for the
//    duration: World::isEditBlocked() already refuses to write any chunk whose
//    own 3x3x3 contains a Meshing chunk, which is exactly this set. Reusing that
//    one CAS is also what makes "a chunk can only be rebuilding at one level at
//    a time" true by construction - there is no second busy flag to keep in
//    step, and an ordinary remesh and a LOD rebuild contend for the same
//    Ready -> Meshing transition.

//
// LIGHTING
// --------
// A freshly generated section is NOT handed to the mesher until its light has
// been computed: a mesh bakes light into its vertices, and nothing would rebuild
// it afterwards. The light pass runs on a worker and WRITES voxel light, so it
// may only touch chunks no other thread can read (invariant 1 in Chunk.hpp).
//
// THE LIT FLAG, AND WHY IT IS NOT A CHUNK STATE. The obvious implementation is a
// `Lighting` state between Generated and Meshing, or - without touching the
// frozen state machine - leaving the section in `Generating` until it is lit.
// Both make an unlit chunk BUSY, and busy is a promise that a worker owns it
// right now. A section whose column is only half generated is not owned by
// anybody; it is merely waiting. Marking it busy makes it un-meshable (correct),
// un-editable (correct) and un-RETIRABLE (wrong) - so a column the player walks
// away from before it finishes generating strands its sections in a busy state
// forever, which is a leak and trips every "nothing is stranded" assertion in
// the streaming tests.
//
// So the section goes to `Generated` exactly as it always did, and residency
// carries a separate `lit` flag. Unlit chunks are simply invisible to
// findReadable() and captureNeighbourhood(), which is all that is needed: no
// mesh job can capture one, so none can read the light being written, and the
// scheduler naturally waits for it the same way it already waits for a missing
// neighbour. Nothing outside this file can observe the flag.
//
// Lighting is scheduled a WHOLE COLUMN at a time - see LightColumnWork for why -
// and the job also READS the eight surrounding columns. A main-thread light or
// voxel write into any of those would race it, so an active light job registers
// its column and isNeighbourhoodBusy() reports it exactly as it reports a mesh
// job.
//
// THAT REGISTRATION IS TAKEN ON THE MAIN THREAD AND NOWHERE ELSE. It is the same
// kind of marker as ChunkState::Meshing, and the reason the main thread may test
// one and then write is that no other thread can raise one behind its back. A
// generate job that completes a column therefore only files the column as
// pending; sweepPendingLight() takes the claim and dispatches, one update later.

#include "core/JobSystem.hpp"
#include "world/BlockAccess.hpp"
#include "world/Chunk.hpp"
#include "world/LightEngine.hpp"
#include "world/Lod.hpp"
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
#include <unordered_set>
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
    ///
    /// 20 rather than the pre-LOD 8. The old value was chosen when every chunk
    /// was meshed and drawn at full resolution; with `lod` enabled, everything
    /// past 14 chunks is built from 4x4x4-block cells and contributes roughly a
    /// sixty-fourth of the geometry it used to. 20 is also the smallest radius
    /// that gives LodPolicy's outermost band (bandStart[2] == 14) a ring wide
    /// enough to be worth having; anything less and level 3 is never used.
    std::int32_t loadRadius = 20;

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
    std::size_t maxGenerateJobsInFlight = 96;
    std::size_t maxMeshJobsInFlight     = 32;

    /// Jobs dispatched per update, across every kind.
    ///
    /// Raised from the pre-LOD 24 because the load volume grew 6.25x while the
    /// chunks that fill the new space cost a small fraction of an old one to
    /// generate and mesh. At 24 a cold start takes ~420 updates to fill a radius
    /// of 20, which is seven seconds of terrain visibly arriving; at 48 it is
    /// three and a half, and the per-update work is still below what 24
    /// full-resolution chunks used to cost.
    std::size_t maxScheduledPerUpdate = 48;
    /// Chunks retired per update. Retiring is cheap but the release callback
    /// deletes GPU buffers, which is not.
    std::size_t maxUnloadsPerUpdate = 16;

    /// How much being in front of the player discounts a chunk's distance, in
    /// [0, 1). 0.5 means a chunk dead ahead is treated as half as far as one
    /// directly behind at the same range.
    float viewBias = 0.5f;

    // ------------------------------------------------------ level of detail --

    /// Which resolution a chunk is generated and meshed at, by distance. Set
    /// `lod.enabled = false` to pin the whole world to level 0 for a
    /// like-for-like comparison against the pre-LOD renderer.
    LodPolicy lod{};

    /// LOD rebuilds STARTED per update.
    ///
    /// A transition regenerates a chunk and rebuilds its geometry, so it costs
    /// roughly what streaming a brand-new chunk costs. A band edge at radius 14
    /// is ~88 columns, i.e. ~700 chunk sections, and letting a whole edge flip in
    /// one update is a multi-frame stall on the workers plus a burst of uploads
    /// on the main thread. Four per update drains that edge in about three
    /// seconds of continuous walking, and because the old mesh keeps being drawn
    /// the whole time the delay is invisible - a chunk a little too coarse for a
    /// second or two is not a defect a player can see at 450 blocks.
    std::size_t maxLodTransitionsPerUpdate = 4;

    /// Rebuild jobs allowed to be in flight at once, across all updates. Bounds
    /// how much worker time LOD can steal from ordinary streaming when the
    /// player is moving fast enough to keep the per-update cap saturated.
    std::size_t maxLodJobsInFlight = 8;

    // ------------------------------------------------------------- lighting --

    /// Columns the main-thread light sweep may start per update.
    ///
    /// EVERY column comes through the sweep. A generate job used to start its own
    /// column's light the instant the last section landed, which saved an update
    /// of latency and cost a data race: the claim is what makes a chunk
    /// unwritable, and a claim taken on a worker can appear between the main
    /// thread's writability check and its write. See dispatchGenerate.
    ///
    /// The budget has room to spare. A cold start admits maxScheduledPerUpdate
    /// (48) generate jobs per update, which completes at most six eight-section
    /// columns in that time, so 32 drains the backlog several times over and the
    /// cap only ever bites on a teleport.
    std::size_t maxLightColumnsPerUpdate = 32;

    /// Light jobs allowed to be in flight at once, across all updates. Bounds how
    /// much worker time lighting can steal from the generation it depends on.
    std::size_t maxLightJobsInFlight = 64;

    /// Set false to stream with no lighting at all, which is what the pipeline
    /// did before the light engine existed: terrain goes straight from
    /// Generating to Generated and the mesher sees whatever light the generator
    /// left behind. For A/B comparisons and for isolating a streaming bug from a
    /// lighting one; the game always runs with it on.
    bool lighting = true;

    /// Refuse to REGENERATE a chunk that diverges from generated terrain.
    ///
    /// A rebuild reruns the ChunkGenerateFn, which for ordinary terrain is free
    /// and for an authored chunk is destruction. Chunk::needsSave() is one half
    /// of the marker and it is NOT exact - it means "dirty since the last
    /// write", not "edited". The first autosave calls Chunk::markSaved() and
    /// clears it while the build is still on disk, and a chunk decoded back off
    /// disk has it false from birth, so on its own it protected an edit for
    /// exactly one autosave interval. The other half is setDivergedPredicate(),
    /// a sticky per-position answer supplied by the save layer. With no
    /// predicate set the behaviour is the old, leaky one, which is what keeps
    /// ChunkManager usable standalone.
    ///
    /// WHAT IS REFUSED IS THE REGENERATION, NOT EVERY LEVEL CHANGE. Freezing a
    /// divergent chunk at whatever level it happens to be holding is a bug of
    /// its own, and a self-perpetuating one: kLodFull is the only level a save
    /// ever holds, so a build streams back in at the coarse level its distance
    /// implies - the load misses and the seed fills it - and a frozen chunk can
    /// never be refined afterwards. The player walks up to their own house and
    /// it stays blocky for the rest of the session.
    ///
    /// So lodTargetFor() permits exactly one transition for a divergent chunk:
    /// a rebuild AT kLodFull, for a position the diverged predicate says is on
    /// disk, which the generator satisfies by RELOADING rather than
    /// regenerating. Everything else is still refused - every demotion, and also
    /// the intermediate promotion (3 -> 1, say) that would land the chunk on a
    /// level no save can hold and therefore regenerate it just as surely as a
    /// demotion would.
    bool preserveEditedChunks = true;

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

/// Answers "does the chunk at this position diverge from generated terrain".
///
/// Injected because the manager has no save layer to ask; WorldSave supplies it
/// through WorldSave::storedChunkPredicate(). See the divergence note in
/// world/WorldSave.hpp, and preserveEditedChunks above for why the per-chunk
/// dirty flag cannot answer this question. MAIN THREAD.
///
/// A TRUE ANSWER IS ALSO A PROMISE: that the injected ChunkGenerateFn will
/// restore this position's authored content when asked to generate it at
/// kLodFull. That promise is what makes a promotion to full resolution safe for
/// a chunk the seed cannot reproduce, and it is exactly how Application wires
/// the pair - WorldSave::hasStoredChunk against a generator that tries
/// WorldSave::loadChunk before it touches the terrain sampler. A predicate
/// answering for something the generator cannot read back would turn that
/// promotion into a silent regeneration, which is the very thing it exists to
/// prevent.
///
/// Invoked from lodTargetFor(), which is noexcept - so the target must not
/// throw. WorldSave::hasStoredChunk() is noexcept for exactly this reason.
using ChunkDivergedFn = std::function<bool(const ChunkPos&)>;

/// The three callables the streamer needs to light what it streams.
///
/// Injected rather than compiled in for the same reason meshing is: the manager
/// has no BlockRegistry and world/ keeps its dependencies one-directional. When
/// the lighter is not set the pipeline behaves exactly as it did before lighting
/// existed - terrain goes straight from Generating to Generated - which is what
/// keeps ChunkManager usable on its own in tests and tools.
struct ChunkLighter {
    /// Rebuilds the light of every section in the work item. Runs on a WORKER
    /// with exclusive ownership of `work.targets`; everything else it touches is
    /// read-only. Returns the light that crossed into chunks it did not own.
    std::function<LightSpill(const LightColumnWork&)> column;

    /// Lights the LOD shadow chunk, which nobody else can see yet. Runs on a
    /// WORKER, between the shadow's terrain and its geometry.
    std::function<void(Chunk&, const ChunkNeighbourhood&, LightSpill&)> chunk;

    /// Applies spilled light to the live world. MAIN THREAD.
    std::function<void(LightSpill&&)> spill;

    [[nodiscard]] bool valid() const noexcept
    {
        return static_cast<bool>(column) && static_cast<bool>(chunk) && static_cast<bool>(spill);
    }
};

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

    /// Resident chunks per LodLevel, indexed by level. The debug overlay shows
    /// this as the LOD distribution; a healthy walk has all four buckets
    /// populated and the level-0 bucket roughly constant.
    std::array<std::size_t, kLodCount> chunksByLod{};

    std::size_t generateJobsInFlight = 0;
    std::size_t meshJobsInFlight     = 0;
    std::size_t lodJobsInFlight      = 0;
    std::size_t lightJobsInFlight    = 0;
    std::size_t pendingUploads       = 0;
    /// Edits waiting for their chunk to leave a worker-owned state. World only.
    std::size_t deferredEdits = 0;
    /// Cross-border light a worker computed that the main thread has not applied
    /// yet. World only.
    std::size_t pendingLightSeeds = 0;

    std::size_t cpuVoxelBytes = 0;
    std::size_t gpuMeshBytes  = 0;
    std::size_t triangles     = 0;

    /// Partially destroyed blocks summed over resident chunks, and what their
    /// sparse side tables cost. Both are zero for untouched terrain, which is the
    /// claim the sub-voxel design rests on - so they are worth being able to read
    /// rather than assume.
    std::size_t damagedBlocks = 0;
    std::size_t subVoxelBytes = 0;

    std::uint64_t chunksCreated  = 0;
    std::uint64_t chunksUnloaded = 0;
    std::uint64_t meshesUploaded = 0;
    /// Meshes thrown away because the chunk was retired or edited mid-job. A
    /// number that climbs steadily means the streaming radii are thrashing.
    std::uint64_t meshesDropped = 0;

    /// Column light jobs that finished, and the sections they lit. A world that
    /// has settled should see both stop climbing; a `columnsLit` that keeps
    /// rising with the player standing still means light is being invalidated in
    /// a loop.
    std::uint64_t lightColumnsLit  = 0;
    std::uint64_t lightSectionsLit = 0;

    /// LOD rebuilds that completed and swapped in. Divided by elapsed time this
    /// is the transition rate; if it grows without the player moving, the
    /// hysteresis band is too narrow for the band edges in use.
    std::uint64_t lodTransitions = 0;
    /// Rebuilds finished after their chunk had already been retired or replaced.
    std::uint64_t lodTransitionsDropped = 0;

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

    /// Teaches the LOD decision which positions must never be regenerated from
    /// the seed; see ChunkDivergedFn and StreamingConfig::preserveEditedChunks.
    /// Leaving it unset keeps the old dirty-flag-only behaviour.
    void setDivergedPredicate(ChunkDivergedFn diverged);

    /// Enables the lighting stage. Leaving it unset keeps the pre-lighting
    /// pipeline exactly as it was; see ChunkLighter.
    void setLighter(ChunkLighter lighter);

    [[nodiscard]] bool hasGenerator() const noexcept { return static_cast<bool>(m_generate); }
    [[nodiscard]] bool hasMesher() const noexcept { return static_cast<bool>(m_mesh); }
    [[nodiscard]] bool hasLighter() const noexcept { return m_lighter.valid(); }

    [[nodiscard]] const StreamingConfig& config() const noexcept { return m_config; }
    /// Clamps the padding to at least one chunk; see StreamingConfig.
    void setConfig(const StreamingConfig& config);

    [[nodiscard]] const LodPolicy& lodPolicy() const noexcept { return m_config.lod; }
    /// Replaces the selection policy. Resident chunks are not rebuilt here; the
    /// next update() picks up the new bands through the ordinary transition path,
    /// so the change is spread over several frames like any other.
    void setLodPolicy(const LodPolicy& policy);

    // ---- residency (any thread) ----

    [[nodiscard]] ChunkPtr find(const ChunkPos& position) const;
    /// Null unless the chunk is resident AND its voxels are safe to read.
    [[nodiscard]] ConstChunkPtr findReadable(const ChunkPos& position) const;

    /// findReadable's predicate, but hands back a MUTABLE pointer.
    ///
    /// For the main-thread incremental light pass, which reads a neighbourhood
    /// and then writes part of it. `find()` is the wrong tool there and was a
    /// real crash: it hands back a chunk in ANY state, including one a generator
    /// is filling and one whose column a light job is writing. ChunkStorage grows
    /// its palette and materialises its light array in place, so reading such a
    /// chunk from the main thread is the mirror image of the use-after-free
    /// World::isEditBlocked exists to prevent - the reader, not the writer, is
    /// the one on the wrong thread.
    ///
    /// Writability is a strictly stronger test and still belongs to the caller;
    /// see LightWorld's predicate, which additionally consults isRegionBusy().
    [[nodiscard]] ChunkPtr findReadableMutable(const ChunkPos& position) const;
    [[nodiscard]] bool        isResident(const ChunkPos& position) const;
    [[nodiscard]] std::size_t residentCount() const;

    /// 3x3x3 shared_ptr snapshot around `centre`, taken in one critical section.
    /// Slots holding a chunk that is not yet readable are left null, so
    /// `ChunkNeighbourhood::complete()` answers "may this be meshed yet".
    [[nodiscard]] ChunkNeighbourhood captureNeighbourhood(const ChunkPos& centre) const;

    /// True when a worker may be reading any chunk whose contents a main-thread
    /// write to `position` would change under it.
    ///
    /// This is the single predicate behind World::isEditBlocked, and it covers
    /// both kinds of worker reader:
    ///  * a MESH job for any chunk in `position`'s own 3x3x3, which captured a
    ///    snapshot including `position` and reads a one-voxel skirt out of it;
    ///  * a LIGHT job for any of the nine columns around `position`, which reads
    ///    the light of every chunk in that block while it recomputes the middle.
    /// Both are answered in one shared lock rather than one per neighbour, which
    /// is also strictly cheaper than the per-chunk find() loop it replaced.
    [[nodiscard]] bool isNeighbourhoodBusy(const ChunkPos& position) const
    {
        return isRegionBusy(position, position);
    }

    /// isNeighbourhoodBusy for every chunk in the inclusive box, in one critical
    /// section. An incremental relight can reach down a whole column, so its
    /// writable set is a box rather than a single 3x3x3 and testing it a chunk
    /// at a time would take the lock dozens of times per edit.
    [[nodiscard]] bool isRegionBusy(const ChunkPos& minChunk, const ChunkPos& maxChunk) const;

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

    /// Floor of the horizontal distance in chunks - the unit LodPolicy speaks.
    ///
    /// Computed as an exact integer square root rather than a rounded
    /// `std::sqrt`, because the level a chunk lands in must be a pure function
    /// of two integers: a chunk that flips level because a double rounded the
    /// other way on a different machine is a determinism bug that only shows up
    /// as a mesh count differing between two runs of the same seed.
    [[nodiscard]] static std::int32_t horizontalDistanceChunks(const ChunkPos& a,
                                                               const ChunkPos& b) noexcept;

    /// Level the policy wants for `chunk` given the player is at `centre`.
    /// `current` is the chunk's present level, which engages the hysteresis; pass
    /// the two-argument form only for chunks that are actually resident.
    [[nodiscard]] LodLevel desiredLod(const ChunkPos& centre, const ChunkPos& chunk,
                                      LodLevel current) const noexcept;
    /// Level for a chunk that has no present level yet.
    [[nodiscard]] LodLevel desiredLod(const ChunkPos& centre, const ChunkPos& chunk) const noexcept;

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
    /// Where a chunk is in the lighting pipeline. Allocated only when lighting
    /// is enabled, and held behind a shared_ptr so a worker can update it without
    /// taking the map lock - and so a job that outlives the map entry still has
    /// somewhere safe to write.
    struct ChunkLightFlags {
        /// Terrain is final. Set by the generate worker before it publishes the
        /// chunk, so a light job can tell a section that is merely waiting from
        /// one that is still being written.
        std::atomic<bool> terrainReady{false};
        /// Light is final. Until this is set the chunk is invisible to
        /// findReadable() and captureNeighbourhood(); see the LIGHTING note at
        /// the top of this file.
        std::atomic<bool> lit{false};
    };

    /// Map value: the chunk plus the main-thread-only accounting for the mesh
    /// the renderer currently holds for it.
    struct Slot {
        ChunkPtr    chunk;
        std::size_t gpuBytes  = 0;
        std::size_t triangles = 0;

        /// Null when lighting is disabled, which then reads as "always lit".
        std::shared_ptr<ChunkLightFlags> light;

        [[nodiscard]] bool lit() const noexcept
        {
            return light == nullptr || light->lit.load(std::memory_order_acquire);
        }
        /// Readable by a mesh job, a physics query or another light job.
        [[nodiscard]] bool visible() const noexcept { return chunk->hasVoxels() && lit(); }
    };

    enum class WorkKind : std::uint8_t { Generate, Mesh, LodRebuild, Light };

    struct Candidate {
        float      score = 0.0f;
        ChunkPos   position{};
        ChunkPtr   chunk;
        WorkKind   kind  = WorkKind::Generate;
        ChunkState state = ChunkState::Empty;
        /// Only meaningful for WorkKind::LodRebuild.
        LodLevel targetLod = kLodFull;
    };

    /// isRegionBusy with the caller already holding m_mutex.
    ///
    /// Exists because retireDistant() runs its whole sweep inside one exclusive
    /// lock and std::shared_mutex is not recursive, and because the retire hold
    /// and WorldSave's busy probe MUST be the same predicate rather than two
    /// hand-written approximations of it - see retireDistant().
    [[nodiscard]] bool isRegionBusyLocked(const ChunkPos& minChunk,
                                          const ChunkPos& maxChunk) const;

    // update() steps
    void collect(const StreamingView& view, const ChunkPos& centre, std::uint64_t frameIndex);
    void dispatch();
    void retireDistant(const ChunkPos& centre, std::uint64_t frameIndex);

    /// Level `chunk` should be rebuilt to, or its current level when it must
    /// stay put. Returning "no change" rather than a bool keeps every reason a
    /// rebuild is refused - wrong state, no generator, player edits, policy
    /// hysteresis - in one place instead of scattered through collect().
    [[nodiscard]] LodLevel lodTargetFor(const ChunkPtr& chunk, ChunkState state,
                                        std::int32_t distanceInChunks) const noexcept;

    bool dispatchGenerate(const ChunkPtr& chunk, JobPriority priority);
    bool dispatchMesh(const Candidate& candidate, JobPriority priority);

    // ---- lighting ----

    /// A claimed column light job: what the engine needs, plus the residency
    /// flags to flip when it is done. Kept apart from LightColumnWork so the
    /// engine stays ignorant of how the streamer tracks residency.
    struct ColumnLightJob {
        LightColumnWork work;
        std::array<std::shared_ptr<ChunkLightFlags>, kWorldSectionCount> flags{};
    };

    /// Takes the column's light claim and fills `work` with everything the job
    /// may touch, in ONE critical section. Fails - without taking the claim -
    /// when the column is already claimed or when there is nothing to light.
    ///
    /// MAIN THREAD ONLY, and that is a threading invariant rather than a
    /// convenience: the claim is a marker that makes chunks unwritable, and the
    /// main thread's check-then-write pattern is only safe while the main thread
    /// is the only one that can establish such a marker. See dispatchGenerate.
    ///
    /// `tolerant` decides what to do about a section that has no terrain yet.
    /// Strict refuses the whole column, which is what gives the common case its
    /// quality: waiting for the last section of a column means the sky is exact
    /// on the first pass. Tolerant treats it as an unknown wall and lights the
    /// rest, which is right once the column has left the load volume and no more
    /// terrain is coming - see sweepPendingLight().
    bool claimColumnLight(const ColumnPos& column, ColumnLightJob& job, bool tolerant);
    void releaseColumnLight(const ColumnPos& column);

    /// Claims and submits, or does nothing. MAIN THREAD - see claimColumnLight.
    ///
    /// Does NOT consult maxLightJobsInFlight itself; sweepPendingLight, its only
    /// caller, applies that cap before it asks.
    bool startColumnLight(const ColumnPos& column, JobPriority priority, bool tolerant);

    /// Starts the light job for every column whose terrain has landed. MAIN
    /// THREAD, once per update, and the ONLY thing that dispatches column light.
    ///
    /// IT IS ALSO WHAT STOPS CHUNKS BEING STRANDED. A section waiting to be lit
    /// is invisible to findReadable(), and the only thing that makes it visible
    /// is a light job. If the player turns away before the rest of its column is
    /// generated, that column is never in the load volume again and no further
    /// generate job runs in it; without this sweep its sections stay unlit - and
    /// therefore un-meshable - for as long as they stay resident.
    void sweepPendingLight();

    /// Body of a light job. Runs on a WORKER.
    void runColumnLight(const ColumnLightJob& job);

    /// Starts a shadow rebuild at `candidate.targetLod`. See the LEVEL OF DETAIL
    /// section of the file header for why it is a shadow and not an in-place
    /// regeneration.
    bool dispatchLodRebuild(const Candidate& candidate, JobPriority priority);

    /// Main-thread tail of a rebuild: publishes `shadow` in place of `live` and
    /// uploads its mesh, both inside this one call so no frame can observe the
    /// position without geometry. MAIN THREAD.
    void finishLodRebuild(const ChunkPtr& live, const ChunkPtr& shadow,
                          const ChunkMeshUpload& result);

    /// Flags the 26 chunks around `position` for a remesh.
    ///
    /// Called after a LOD swap, and not optional. Skirts (Lod.hpp) hide the
    /// crack a level mismatch opens along a seam, but they do not fix face
    /// culling: the mesher decides per chunk face whether the neighbour across
    /// it renders at the same resolution, and only culls against it when it
    /// does. Every neighbour of a chunk that just changed level made that
    /// decision against the OLD level, so its border faces are wrong in the
    /// direction that shows - culled where they should now be drawn. Because a
    /// mesh is only rebuilt when something dirties the chunk, the resulting hole
    /// would be permanent.
    void markNeighboursDirty(const ChunkPos& position);

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
               m_meshInFlight.load(std::memory_order_acquire) +
               m_lodInFlight.load(std::memory_order_acquire) +
               m_lightInFlight.load(std::memory_order_acquire);
    }

    JobSystem&      m_jobs;
    StreamingConfig m_config;

    ChunkGenerateFn m_generate;
    ChunkMeshFn     m_mesh;
    ChunkReleaseFn  m_release;
    ChunkRetireFn   m_retire;
    ChunkDivergedFn m_diverged;
    ChunkLighter    m_lighter;

    mutable std::shared_mutex                    m_mutex;
    std::unordered_map<ChunkPos, Slot>           m_chunks;

    /// Columns with a light job in flight. Only busy columns are in here, so it
    /// holds at most maxLightJobsInFlight entries and needs no pruning; a set of
    /// "every column that was ever lit" would grow without bound as the player
    /// walks. Guarded by m_mutex together with the chunk map, because claiming a
    /// column and reading the chunks in it has to be one atomic step.
    std::unordered_set<ColumnPos>                m_lightColumns;

    /// Columns holding at least one section that has terrain but no light yet.
    /// Added by the generate worker, cleared when a light job claims the column;
    /// swept once per update so nothing can be stranded. Guarded by m_mutex.
    std::unordered_set<ColumnPos>                m_lightPending;

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
    /// LOD rebuilds are counted apart from ordinary meshing so they can have
    /// their own in-flight budget; jobsInFlight() sums all three, which is what
    /// keeps waitForPendingJobs() and the destructor honest.
    std::atomic<std::size_t> m_lodInFlight{0};
    /// Counted apart so lighting gets its own budget and cannot crowd out the
    /// generation it depends on; jobsInFlight() sums all four.
    std::atomic<std::size_t> m_lightInFlight{0};
    std::atomic<std::size_t> m_pendingUploads{0};
    std::atomic<std::size_t> m_queuedChunks{0};
    std::atomic<std::size_t> m_visibleChunks{0};

    std::atomic<std::uint64_t> m_chunksCreated{0};
    std::atomic<std::uint64_t> m_chunksUnloaded{0};
    std::atomic<std::uint64_t> m_meshesUploaded{0};
    std::atomic<std::uint64_t> m_meshesDropped{0};
    std::atomic<std::uint64_t> m_lodTransitions{0};
    std::atomic<std::uint64_t> m_lodTransitionsDropped{0};
    std::atomic<std::uint64_t> m_lightColumnsLit{0};
    std::atomic<std::uint64_t> m_lightSectionsLit{0};

    /// Only the sleep/wake handshake for waitForPendingJobs; the counters above
    /// are atomics so stats() never contends with a worker.
    mutable std::mutex      m_jobMutex;
    std::condition_variable m_jobIdle;
};

}  // namespace voxl
