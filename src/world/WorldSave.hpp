#pragma once

// World persistence: the chunk payload codec, the async save queue and the
// world metadata file.
//
// WorldSave sits above RegionFile (world/RegionFile.hpp, which owns the on-disk
// container and the corruption handling) and below the streaming pipeline. It
// knows how a Chunk turns into bytes and back; it does not know when to save,
// which is the caller's policy.
//
// ---------------------------------------------------------------------------
//  WHY ENCODING HAPPENS ON THE MAIN THREAD AND ONLY THE WRITE IS ASYNC
// ---------------------------------------------------------------------------
// Saving reads a chunk's voxels, light and damage table. Doing that on a worker
// while holding a shared_ptr snapshot is exactly the use-after-free the engine's
// first invariant exists to prevent: ChunkStorage reallocates as its palette
// grows and SubVoxelStore reallocates as it grows, so a main-thread edit landing
// mid-read frees the buffer the reader is walking. Deferring the edit instead is
// not an option either - the save path would then have to participate in
// World::isEditBlocked, and a background autosave would start blocking gameplay
// edits for as long as the disk took.
//
// So the split is drawn at the byte buffer. `saveChunk()` runs on the MAIN
// THREAD and does the whole read there, where by definition no other thread is
// writing the chunk; it produces a self-contained std::vector<std::byte>. The
// worker job that follows owns that buffer outright and never touches a Chunk,
// a ChunkStorage or a SubVoxelStore again. The expensive half - the file I/O -
// is the half that goes off-thread, and the cheap half - a few memcpys, tens of
// microseconds for the largest possible section - is the half that stays.
//
// The main thread is therefore never blocked on disk, and a save job can never
// observe a chunk at all, let alone a torn one.
//
// LOADING is the mirror image and needs no such care: it runs inside the
// ChunkGenerateFn on a worker that has exclusive ownership of a chunk in state
// Generating (see the threading contract in world/Chunk.hpp), so it may write
// voxels for the same reason the terrain generator may.
//
// ---------------------------------------------------------------------------
//  LEVEL OF DETAIL
// ---------------------------------------------------------------------------
// Only level-0 chunks are ever written. A coarser chunk's voxels are a 2^L
// downsample of terrain that the seed reproduces exactly, so storing them buys
// nothing and risks a load restoring a 4x4x4 approximation as if it were the
// real thing. Every payload records the level it was captured at and a load
// refuses a level that does not match the chunk asking for it.

#include "core/JobSystem.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/Lod.hpp"
#include "world/RegionFile.hpp"
#include "world/VoxelTypes.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glm/vec3.hpp>

namespace voxl {

// --------------------------------------------------------------- metadata --

/// Everything about a world that is not a chunk.
///
/// Small, rewritten wholesale, and the one file whose loss makes the save
/// useless - so unlike a region it is written with temp-file-and-rename, which
/// is genuinely atomic at the directory level and costs nothing for a few dozen
/// bytes.
struct WorldMetadata {
    std::uint16_t formatVersion = kSaveFormatVersion;
    std::uint64_t seed          = 0;

    glm::vec3 playerPosition{0.0f, static_cast<float>(kSeaLevel) + 2.0f, 0.0f};
    float     yawDegrees   = 0.0f;
    float     pitchDegrees = 0.0f;

    std::uint8_t hotbarSlot = 0;

    /// Fraction of a day, [0, 1). 0 is midnight, 0.5 is noon.
    float timeOfDay = 0.25f;

    /// Wall-clock seconds the player has spent in this world, across sessions.
    double playTimeSeconds = 0.0;
};

/// What happened when a chunk was asked for.
struct ChunkLoadResult {
    ChunkLoadStatus status = ChunkLoadStatus::Absent;
    SaveError       error  = SaveError::None;
    /// Level the payload was captured at; only meaningful when `Loaded`.
    LodLevel lod = kLodFull;

    /// True when the caller must fall back to generating the chunk from the
    /// seed - which is the correct response to absence AND to every flavour of
    /// corruption.
    [[nodiscard]] bool regenerate() const noexcept { return status != ChunkLoadStatus::Loaded; }
};

/// Counters for the debug overlay and for tests.
struct SaveStats {
    std::uint64_t chunksEncoded     = 0;
    std::uint64_t chunksWritten     = 0;
    std::uint64_t writeFailures     = 0;
    std::uint64_t chunksLoaded      = 0;
    /// Chunks that had nothing on disk and were generated instead.
    std::uint64_t chunksAbsent = 0;
    /// Chunks whose stored bytes were rejected and were regenerated instead.
    /// A number that is anything other than zero is worth investigating.
    std::uint64_t chunksCorrupt = 0;
    std::uint64_t bytesWritten  = 0;

    std::size_t writesInFlight = 0;
    std::size_t openRegions    = 0;
};

/// Reads `directory / "level.vxw"` without constructing a WorldSave.
///
/// WHY THIS IS FREE AND NOT A MEMBER. Two callers need a world's metadata
/// before they can decide anything: the title screen, which enumerates every
/// save directory to build its load list, and world startup, which must know
/// the stored seed before it can build the TerrainGenerator - a WorldSave
/// constructed with the wrong seed would refuse every region it then opened.
/// Both would otherwise have to construct a throwaway WorldSave per candidate,
/// which opens a region cache and pins a seed to answer a question about a
/// single 48-byte file.
///
/// Returns false with `error == SaveError::None` when the file is simply
/// absent, which is the normal state of a brand-new world.
[[nodiscard]] bool readWorldMetadata(const std::filesystem::path& directory, WorldMetadata& out,
                                     SaveError& error);

// -------------------------------------------------- the seed's second copy --
//
// WHY ANY OF THIS EXISTS. The seed used to live in exactly one place: the
// 56-byte level.vxw. When that file was truncated or failed its checksum the
// world silently fell back to whatever seed the caller happened to ask for, and
// since every region header records the seed it was written with, EVERY region
// then refused to open with SeedMismatch - the world became unloadable and
// unsavable at the same time and the whole thing was discarded. One damaged
// 56-byte file destroyed an entire world.
//
// The region headers are the seed's second copy. They are written once per
// region, spread over as many files as the world has regions, and each is
// protected by its own CRC, so the odds of losing all of them at once are not
// comparable to the odds of losing one small file that is rewritten on every
// autosave. Reading one is 64 bytes and needs no RegionFile object, no region
// cache and - crucially - no seed, which is the whole point: RegionFile's
// constructor takes the seed it is supposed to verify, so it cannot be the thing
// that tells you what the seed is.

/// What a region file's 64-byte header claims about itself.
struct RegionHeaderInfo {
    std::uint64_t seed          = 0;
    std::uint16_t formatVersion = 0;
    std::int32_t  regionX       = 0;
    std::int32_t  regionZ       = 0;
};

/// Reads and validates a region header without constructing a RegionFile.
///
/// Validates magic, the header CRC, the format version and the geometry, in that
/// order, so a file that merely happens to be 64 bytes long can never be mined
/// for a seed. Returns false with `error` set on every rejection.
[[nodiscard]] inline bool readRegionHeader(const std::filesystem::path& path,
                                           RegionHeaderInfo& out, SaveError& error);

/// Every `r.*.vxr` in `directory`, sorted by path.
///
/// Sorted because `directory_iterator` order is unspecified and the seed
/// recovery below must pick the SAME file on every machine and every run - the
/// project's determinism rule applies to recovery paths too, or a bug reproduces
/// on one box and not the next. Quarantined files (`*.corrupt`) are excluded by
/// the extension test.
[[nodiscard]] inline std::vector<std::filesystem::path> listRegionFiles(
    const std::filesystem::path& directory);

/// Seed of the first region in `directory` whose header validates.
///
/// `source` receives that region's path, for the log line the caller owes the
/// player. Returns false when the directory holds no region with a readable
/// header, which is both "brand-new world" and "everything is damaged"; the
/// caller tells those apart with `listRegionFiles(...).empty()`.
[[nodiscard]] inline bool recoverSeedFromRegions(const std::filesystem::path& directory,
                                                 std::uint64_t&               seed,
                                                 std::filesystem::path&       source);

/// Where the seed a session runs on came from.
enum class SeedSource : std::uint8_t {
    /// Nothing on disk had an opinion; the caller's seed stands.
    Requested = 0,
    /// level.vxw, the normal path.
    Metadata = 1,
    /// A region header, because level.vxw was missing or rejected.
    RegionHeader = 2,
};

/// Everything a caller needs to know before it can build a TerrainGenerator for
/// a world directory.
struct WorldSeedResolution {
    std::uint64_t seed   = 0;
    SeedSource    source = SeedSource::Requested;

    bool          hasMetadata   = false;
    WorldMetadata metadata{};
    /// Why level.vxw was rejected. `None` together with `hasMetadata == false`
    /// means the file is simply absent, which is a new world, not damage.
    SaveError metadataError = SaveError::None;

    /// Region the seed was recovered from; empty unless `source` is
    /// `RegionHeader`.
    std::filesystem::path seedFile{};

    /// True when region files exist but not one header could be read. This is
    /// the genuinely bad case: falling back to the requested seed is about to
    /// orphan real data, and the caller must say so loudly.
    bool regionsPresentButUnreadable = false;
};

/// Decides which seed to open `directory` with: level.vxw, else a region header,
/// else `requestedSeed`.
///
/// Deliberately free of logging so it stays a pure function of the directory -
/// the caller knows the world's name and is the one that must be loud. Silently
/// running on the wrong seed is what made this failure so damaging, so no caller
/// should use the result without reporting `source`.
[[nodiscard]] inline WorldSeedResolution resolveWorldSeed(const std::filesystem::path& directory,
                                                          std::uint64_t requestedSeed);

// -------------------------------------------------------------- WorldSave --

class WorldSave {
public:
    /// Regions kept open at once. Each costs a file handle, a 16 KB table and a
    /// sector map; a radius-20 world spans about a dozen, so 64 is generous
    /// enough that a walking player never evicts one they are about to reuse.
    static constexpr std::size_t kMaxOpenRegions = 64;

    /// `jobs` must outlive the WorldSave: the destructor flushes, which needs
    /// the pool alive to finish the writes it is waiting on. `directory` is
    /// created if it does not exist.
    WorldSave(JobSystem& jobs, std::filesystem::path directory, std::uint64_t seed);
    ~WorldSave();

    WorldSave(const WorldSave&)            = delete;
    WorldSave& operator=(const WorldSave&) = delete;
    WorldSave(WorldSave&&)                 = delete;
    WorldSave& operator=(WorldSave&&)      = delete;

    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return m_directory; }
    [[nodiscard]] std::uint64_t                seed() const noexcept { return m_seed; }

    // ------------------------------------------------------------- codec --

    /// Serialises a chunk into a self-contained payload body.
    ///
    /// Writes the PALETTE and the packed index words rather than 32768 block
    /// ids: ChunkStorage is already palette-compressed, so this is both a direct
    /// memcpy and, for the uniform sections that dominate a real world, three
    /// bytes instead of 64 KB.
    ///
    /// Light is stored, not recomputed. Recomputing needs the whole column plus
    /// its neighbours resident before a flood fill can start, which turns a
    /// chunk load into a dependency on chunks that may not be loaded yet, and it
    /// cannot reproduce light from a block the player has since removed. Storing
    /// it costs nothing for the uniform case (one byte, which is most sections)
    /// and is run-length coded otherwise, because light varies slowly and the
    /// raw array would otherwise dwarf the voxels it belongs to.
    ///
    /// Reads the chunk, so it is subject to the engine's first invariant: MAIN
    /// THREAD, or a worker that owns the chunk exclusively.
    [[nodiscard]] static std::vector<std::byte> encodeChunk(const Chunk& chunk);

    /// Rebuilds a chunk from a payload body.
    ///
    /// The payload is fully parsed and validated into scratch storage BEFORE the
    /// chunk is touched, so a rejected payload leaves the chunk exactly as it
    /// was - never half-written, never silently zeroed. Sub-voxel damage is
    /// re-applied through Chunk::breakSubVoxel, the sanctioned mutation path, so
    /// the invariant at the top of world/SubVoxel.hpp holds by construction
    /// rather than by the decoder being careful.
    ///
    /// On success the chunk is marked dirty (it has no mesh yet) and marked
    /// saved (it matches disk).
    static ChunkLoadResult decodeChunk(std::span<const std::byte> payload, Chunk& chunk);

    // -------------------------------------------------------- async save --

    /// Encodes `chunk` now and queues the disk write on a worker.
    ///
    /// MAIN THREAD. Returns false without queueing anything when the chunk has
    /// nothing worth saving: no voxels yet, a worker is generating it, or it is
    /// not at full resolution. Clears the chunk's needsSave() flag on success;
    /// if the write later fails, the flag is restored from the main thread so
    /// the next autosave retries.
    bool saveChunk(const ChunkPtr& chunk);

    /// Saves every chunk in the batch whose needsSave() flag is still set.
    /// MAIN THREAD. Returns how many writes were queued.
    std::size_t saveChunks(std::span<const ChunkPtr> chunks);

    /// ChunkManager retire-hook body: saves the chunk if it diverges from disk.
    /// MAIN THREAD, called with the chunk already in state Unloading.
    void onChunkRetired(const ChunkPtr& chunk);

    /// `onChunkRetired` bound into the shape ChunkManager::setRetireHook wants.
    /// The returned callable holds `this`, so it must not outlive the WorldSave.
    [[nodiscard]] std::function<void(const ChunkPtr&)> retireHook();

    /// Teaches saveChunk() which chunks a worker may be reading right now.
    ///
    /// WHY IT IS NEEDED. saveChunk() encodes a whole chunk - voxels AND light -
    /// on the main thread. That is exactly the read invariant 1 forbids while a
    /// worker owns the chunk, and a column light job owns and REWRITES the light
    /// array of every unlit section in its column without the chunk ever
    /// entering a busy ChunkState (see the LIGHTING note in ChunkManager.hpp,
    /// which explains why it must not). ChunkState alone therefore cannot see
    /// it. Refusing while the neighbourhood is busy is the same answer saveChunk
    /// already gives for ChunkState::Generating: the chunk keeps needsSave() and
    /// the next autosave tick writes it.
    ///
    /// WHY IT IS INJECTED. A WorldSave is constructed before the world it saves
    /// and deliberately knows nothing about residency; holding a ChunkManager
    /// would invert that and tangle two lifetimes that are independent today.
    /// Application wires this to ChunkManager::isNeighbourhoodBusy. Unset - every
    /// WorldSave unit test, and every offline tool - the answer is "not busy",
    /// which is correct because nothing is streaming.
    ///
    /// MAIN THREAD, set before streaming starts. The callable must not outlive
    /// the thing it probes.
    using ChunkBusyProbeFn = std::function<bool(const ChunkPos&)>;
    void setBusyProbe(ChunkBusyProbeFn probe);

    // -------------------------------------------------------------- load --

    /// Populates `chunk` from disk if there is anything there.
    ///
    /// Safe from any thread, and meant to be called from inside the
    /// ChunkGenerateFn: `if (save.loadChunk(chunk).regenerate()) generate(chunk);`
    /// Every failure mode - absent, truncated, bad magic, bad checksum, offset
    /// past the end of the file, zero-length payload - returns `regenerate()`
    /// true and leaves the chunk untouched.
    ChunkLoadResult loadChunk(Chunk& chunk);

    // ------------------------------------------- divergence from terrain --
    //
    // WHY THIS IS NOT Chunk::needsSave().
    //
    // needsSave() means one thing: "this chunk differs from the bytes on disk".
    // The saver clears it, by design - that is how the next autosave knows not
    // to rewrite a chunk nothing has touched.
    //
    // Streaming was reading it as a second, incompatible thing: "the player
    // edited this, so do NOT regenerate it from the seed at another LOD"
    // (StreamingConfig::preserveEditedChunks, ChunkManager::lodTargetFor). That
    // reading holds for exactly one autosave interval. The player builds a
    // structure, needsSave() goes true, thirty seconds later the autosave writes
    // it and calls markSaved(), the flag goes false, the player walks away, the
    // chunk is no longer protected, it demotes to LOD 1 and the rebuild
    // regenerates it from the seed - deleting a build that was on disk a moment
    // earlier. Nothing logs, and the next save writes the regenerated terrain
    // over the good bytes.
    //
    // This registry is the sticky signal the LOD decision actually wanted. It
    // answers "does the chunk at this position diverge from generated terrain",
    // it is keyed on POSITION rather than on a live chunk object, and surviving
    // a save does not clear it. Being keyed on what is on disk also protects a
    // RELOADED build, which the flag cannot express at all: a chunk freshly
    // decoded from a region has needsSave() false by definition, so under the
    // old rule a player's structure lost its protection the moment they walked
    // away and came back.
    //
    // Membership is conservative in the safe direction. A false positive pins a
    // chunk at its current level and costs some memory; a false negative deletes
    // player work.

    /// True when this world has bytes on disk for `position`, i.e. the chunk
    /// there is player work rather than something the seed reproduces.
    ///
    /// ANY THREAD, and `noexcept` because the intended caller is
    /// `ChunkManager::lodTargetFor`, which is itself noexcept - an exception
    /// escaping into it would be std::terminate rather than a lost frame. If the
    /// lock itself fails the answer is "yes", which pins the chunk instead of
    /// regenerating over it.
    [[nodiscard]] bool hasStoredChunk(const ChunkPos& position) const noexcept;

    /// Records that `position` has, or is about to have, bytes on disk.
    ///
    /// ANY THREAD. Called after a save is queued and after a load succeeds; both
    /// mean the same thing to the LOD decision, which is why this is one call
    /// and not two flags.
    void noteStoredChunk(const ChunkPos& position);

    /// Seeds the registry by reading every region table in the directory.
    ///
    /// MAIN THREAD, at world open, before anything streams. This is what makes a
    /// build from a previous session protected: without it the registry would
    /// only know about chunks this session happened to save or load, and a build
    /// the player has not walked back to yet would demote and be regenerated the
    /// first time it came into range. Regions belonging to another seed are
    /// skipped - RegionFile will refuse to read them anyway, so their chunks
    /// really do come from the seed. Returns how many stored chunks were indexed.
    std::size_t indexStoredChunks();

    [[nodiscard]] std::size_t storedChunkCount() const;

    /// `hasStoredChunk` bound into the shape `ChunkManager` wants. The returned
    /// callable holds `this`, so it must not outlive the WorldSave - the same
    /// contract `retireHook()` already has.
    [[nodiscard]] std::function<bool(const ChunkPos&)> storedChunkPredicate() const;

    // ---------------------------------------------------------- metadata --

    [[nodiscard]] static std::filesystem::path metadataPath(const std::filesystem::path& directory);

    /// Writes the metadata file via a temporary and a rename. MAIN THREAD (it is
    /// a handful of bytes; making it async would buy nothing and add a race with
    /// shutdown).
    bool writeMetadata(const WorldMetadata& metadata);

    /// Reads the metadata file. Returns false and leaves `out` untouched when
    /// the file is missing, damaged, or written by a newer format.
    ///
    /// Thin wrapper over `readWorldMetadata(directory(), ...)`; prefer the free
    /// function when you only want to peek at a directory, because constructing
    /// a WorldSave to read one file also opens a region cache and pins a seed.
    [[nodiscard]] bool readMetadata(WorldMetadata& out, SaveError& error) const;

    // ---------------------------------------------------------- autosave --

    void setAutosaveIntervalSeconds(double seconds) noexcept;
    [[nodiscard]] double autosaveIntervalSeconds() const noexcept { return m_autosaveInterval; }

    /// Returns true at most once per interval, recording the time when it does.
    /// The caller supplies the clock so the frame loop's existing time source is
    /// the only one in play. MAIN THREAD.
    bool tickAutosave(double nowSeconds) noexcept;

    // --------------------------------------------------------- lifecycle --

    /// Blocks until every queued write has finished and every open region has
    /// been flushed. MAIN THREAD ONLY - calling it from a worker would wait on
    /// work that may be behind this thread in the queue.
    void flush();

    /// Not noexcept: it takes the region-cache lock to report the open handle
    /// count, and locking can fail.
    [[nodiscard]] SaveStats stats() const;

private:
    /// Returns the region for `position`, opening or creating the object on
    /// demand. `createIfMissing` false avoids constructing an object for a
    /// region that has never been written, which keeps a pure-read workload from
    /// filling the cache with empty handles.
    [[nodiscard]] std::shared_ptr<RegionFile> acquireRegion(const RegionCoord& coord,
                                                            bool createIfMissing);

    /// Drops the least recently used region once the cache is over its cap.
    /// Caller holds the unique lock. Evicting only removes it from the map; a
    /// worker mid-write still holds its shared_ptr and finishes normally.
    void evictRegionsLocked();

    void noteWriteStarted() noexcept;
    void noteWriteFinished() noexcept;

    JobSystem&            m_jobs;
    std::filesystem::path m_directory;
    std::uint64_t         m_seed = 0;

    mutable std::shared_mutex                                     m_regionMutex;
    std::unordered_map<RegionCoord, std::shared_ptr<RegionFile>> m_regions;
    /// Monotonic tick stamped onto a region every time it is used, so eviction
    /// can pick a victim without an intrusive list.
    std::atomic<std::uint64_t> m_useTick{0};

    std::atomic<std::uint64_t> m_chunksEncoded{0};
    std::atomic<std::uint64_t> m_chunksWritten{0};
    std::atomic<std::uint64_t> m_writeFailures{0};
    std::atomic<std::uint64_t> m_chunksLoaded{0};
    std::atomic<std::uint64_t> m_chunksAbsent{0};
    std::atomic<std::uint64_t> m_chunksCorrupt{0};
    std::atomic<std::uint64_t> m_bytesWritten{0};

    std::atomic<std::size_t> m_writesInFlight{0};
    mutable std::mutex       m_idleMutex;
    std::condition_variable  m_idleCv;

    /// Positions this world has bytes on disk for. A plain mutex rather than the
    /// shared_mutex above because the critical section is one hash lookup and
    /// the reader is one main-thread call per streamed chunk per update, not a
    /// hot loop.
    mutable std::mutex           m_storedMutex;
    std::unordered_set<ChunkPos> m_storedChunks;

    /// See setBusyProbe(). Main thread only, both to set and to call, so it needs
    /// no lock: saveChunk() is already VOXL_CHECKed to be off the workers.
    ChunkBusyProbeFn m_busyProbe;

    double m_autosaveInterval = 30.0;
    double m_lastAutosave     = 0.0;
    bool   m_autosaveArmed    = false;
};

// ===========================================================================
//  INLINE DEFINITIONS
// ===========================================================================
//
// These live in the header rather than in WorldSave.cpp because the seed
// recovery has to be callable from a context that has no WorldSave - by
// definition, since it exists to work out what seed a WorldSave should be
// constructed with.

inline bool readRegionHeader(const std::filesystem::path& path, RegionHeaderInfo& out,
                             SaveError& error)
{
    error = SaveError::None;

    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        error = SaveError::Io;
        return false;
    }

    std::array<std::byte, kRegionHeaderBytes> header{};
    stream.read(reinterpret_cast<char*>(header.data()),
                static_cast<std::streamsize>(header.size()));
    if (static_cast<std::size_t>(stream.gcount()) != header.size()) {
        error = SaveError::Truncated;
        return false;
    }

    bytes::Reader       reader{std::span<const std::byte>{header}};
    const std::uint32_t magic       = reader.u32();
    const std::uint16_t version     = reader.u16();
    const std::uint16_t columns     = reader.u16();
    const std::uint16_t sections    = reader.u16();
    const std::uint16_t sectorShift = reader.u16();
    const std::int32_t  regionX     = reader.i32();
    const std::int32_t  regionZ     = reader.i32();
    const std::uint64_t seed        = reader.u64();
    const std::uint32_t storedCrc   = reader.u32();

    if (magic != kRegionMagic) {
        error = SaveError::BadMagic;
        return false;
    }
    // Checked before anything is believed. The CRC is what separates "this file
    // says the seed is 12345" from "these 8 bytes happen to sit at offset 20".
    if (crc32(std::span<const std::byte>{header.data(), kRegionHeaderCrcBytes}) != storedCrc) {
        error = SaveError::BadChecksum;
        return false;
    }
    if (version != kSaveFormatVersion) {
        error = SaveError::UnsupportedVersion;
        return false;
    }
    if (columns != static_cast<std::uint16_t>(kRegionSizeColumns) ||
        sections != static_cast<std::uint16_t>(kWorldSectionCount) ||
        sectorShift != static_cast<std::uint16_t>(kRegionSectorShift)) {
        error = SaveError::UnsupportedVersion;
        return false;
    }

    out = RegionHeaderInfo{seed, version, regionX, regionZ};
    return true;
}

inline std::vector<std::filesystem::path> listRegionFiles(const std::filesystem::path& directory)
{
    std::vector<std::filesystem::path> paths;

    std::error_code code;
    if (!std::filesystem::is_directory(directory, code) || code) {
        return paths;
    }

    // The non-throwing overload: a directory that disappears mid-scan, or one
    // entry that cannot be stat'd, must not take down a recovery path.
    for (const auto& entry : std::filesystem::directory_iterator(directory, code)) {
        std::error_code entryCode;
        if (!entry.is_regular_file(entryCode) || entryCode) {
            continue;
        }
        const std::filesystem::path& path = entry.path();
        if (path.extension() != ".vxr") {
            continue;
        }
        if (path.filename().string().rfind("r.", 0) != 0) {
            continue;
        }
        paths.push_back(path);
    }

    std::sort(paths.begin(), paths.end());
    return paths;
}

inline bool recoverSeedFromRegions(const std::filesystem::path& directory, std::uint64_t& seed,
                                   std::filesystem::path& source)
{
    for (const std::filesystem::path& path : listRegionFiles(directory)) {
        RegionHeaderInfo info;
        SaveError        error = SaveError::None;
        if (!readRegionHeader(path, info, error)) {
            continue;  // keep looking; one damaged region is not a damaged world
        }
        seed   = info.seed;
        source = path;
        return true;
    }
    return false;
}

inline WorldSeedResolution resolveWorldSeed(const std::filesystem::path& directory,
                                            std::uint64_t                requestedSeed)
{
    WorldSeedResolution out;
    out.seed = requestedSeed;

    out.hasMetadata = readWorldMetadata(directory, out.metadata, out.metadataError);
    if (out.hasMetadata) {
        out.seed   = out.metadata.seed;
        out.source = SeedSource::Metadata;
        return out;
    }

    std::uint64_t         recovered = 0;
    std::filesystem::path from;
    if (recoverSeedFromRegions(directory, recovered, from)) {
        out.seed     = recovered;
        out.source   = SeedSource::RegionHeader;
        out.seedFile = std::move(from);
        return out;
    }

    // Only now is the caller's seed the right answer. Distinguish "nothing here"
    // from "everything here is damaged": in the second case the fallback is
    // about to orphan real data and somebody has to say so.
    out.regionsPresentButUnreadable = !listRegionFiles(directory).empty();
    return out;
}

inline bool WorldSave::hasStoredChunk(const ChunkPos& position) const noexcept
{
    try {
        const std::lock_guard<std::mutex> lock(m_storedMutex);
        return m_storedChunks.find(position) != m_storedChunks.end();
    } catch (...) {
        // See the declaration: the conservative answer keeps the chunk at its
        // current level, which is a memory cost. The other answer is data loss.
        return true;
    }
}

inline void WorldSave::noteStoredChunk(const ChunkPos& position)
{
    const std::lock_guard<std::mutex> lock(m_storedMutex);
    m_storedChunks.insert(position);
}

inline std::size_t WorldSave::storedChunkCount() const
{
    const std::lock_guard<std::mutex> lock(m_storedMutex);
    return m_storedChunks.size();
}

inline std::function<bool(const ChunkPos&)> WorldSave::storedChunkPredicate() const
{
    return [this](const ChunkPos& position) { return hasStoredChunk(position); };
}

inline std::size_t WorldSave::indexStoredChunks()
{
    std::vector<ChunkPos>  found;
    std::vector<std::byte> table;

    for (const std::filesystem::path& path : listRegionFiles(m_directory)) {
        RegionHeaderInfo info;
        SaveError        error = SaveError::None;
        if (!readRegionHeader(path, info, error)) {
            continue;
        }
        if (info.seed != m_seed) {
            // RegionFile will refuse this file with SeedMismatch, so its chunks
            // genuinely do come from the generator and must stay demotable.
            continue;
        }

        std::ifstream stream(path, std::ios::binary);
        if (!stream.is_open()) {
            continue;
        }
        stream.seekg(static_cast<std::streamoff>(kRegionHeaderBytes), std::ios::beg);

        table.assign(kRegionTableBytes, std::byte{0});
        stream.read(reinterpret_cast<char*>(table.data()),
                    static_cast<std::streamsize>(table.size()));
        if (static_cast<std::size_t>(stream.gcount()) != table.size()) {
            continue;  // header without a table: nothing has ever been written
        }

        bytes::Reader reader{std::span<const std::byte>{table}};
        for (std::size_t entry = 0; entry < kRegionEntryCount; ++entry) {
            const std::uint32_t sector = reader.u32();
            (void)reader.u16();  // sectorCount
            (void)reader.u8();   // lod
            (void)reader.u8();   // flags
            if (sector == 0) {
                continue;  // never written; see the table contract in RegionFile.hpp
            }

            // Inverse of regionEntryIndex(): sections are the fastest axis, then
            // the local column x, then z.
            constexpr auto    kSections = static_cast<std::size_t>(kWorldSectionCount);
            constexpr auto    kColumns  = static_cast<std::size_t>(kRegionSizeColumns);
            const std::size_t column    = entry / kSections;
            found.push_back(ChunkPos{
                info.regionX * kRegionSizeColumns + static_cast<std::int32_t>(column % kColumns),
                static_cast<std::int32_t>(entry % kSections),
                info.regionZ * kRegionSizeColumns + static_cast<std::int32_t>(column / kColumns)});
        }
    }

    const std::lock_guard<std::mutex> lock(m_storedMutex);
    for (const ChunkPos& position : found) {
        m_storedChunks.insert(position);
    }
    return found.size();
}

}  // namespace voxl
