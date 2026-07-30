// Chunk payload codec, async save queue and world metadata file.
//
// The codec half of this file is the actual on-disk format for a chunk. Its
// body layout, little-endian throughout:
//
//   u8   bitsPerIndex        0, 1, 2, 4, 8 or 16 - 0 means a uniform section
//   u32  paletteCount        1 when uniform, holding the single block id
//   u16  palette[paletteCount]
//   u64  indexWords[...]     ChunkStorage::wordCountFor(bitsPerIndex); none when uniform
//   u8   lightEncoding       0 uniform, 1 raw, 2 run-length
//        0: u8  uniformLight
//        1: u8  light[32768]
//        2: u32 runCount, then runCount x (u8 length, u8 value)
//   u32  damageCount
//        repeat: u16 blockIndex, u16 material, u64 bits[8]
//
// Voxel order inside a section is voxl::localIndex order - x fastest, then z,
// then y - which is the ordering world/VoxelTypes.hpp fixes for the whole
// engine. Sub-voxel entries are written in ascending block index, taken from
// SubVoxelStore::sortedEntries() precisely so that two identical worlds produce
// identical bytes; the decoder REQUIRES that order and rejects a payload without
// it, which also rules out duplicate entries for one block.

#include "world/WorldSave.hpp"

#include "core/Log.hpp"
#include "world/ChunkStorage.hpp"
#include "world/SubVoxel.hpp"

#include <bit>
#include <chrono>
#include <fstream>
#include <system_error>
#include <utility>

namespace voxl {
namespace {

/// "VXLW" little-endian.
constexpr std::uint32_t kMetadataMagic = 0x574C5856u;
constexpr std::size_t   kMetadataBytes = 56;
/// A metadata file larger than this is not one of ours; refuse rather than read.
constexpr std::size_t kMaxMetadataBytes = 4096;

constexpr std::uint8_t kLightUniform = 0;
constexpr std::uint8_t kLightRaw     = 1;
constexpr std::uint8_t kLightRuns    = 2;

/// Bytes one sub-voxel damage entry occupies: block index, material, 512 bits.
constexpr std::size_t kDamageEntryBytes = 2 + 2 + kSubVoxelWords * 8;

/// How long flush() waits before giving up and saying so. A bounded wait turns
/// "the job system was cancelled out from under us" from a hang at shutdown into
/// a line in the log.
constexpr std::chrono::seconds kFlushTimeout{30};

[[nodiscard]] constexpr bool isLegalBitsPerIndex(std::uint8_t bits) noexcept
{
    return bits == 0 || bits == 1 || bits == 2 || bits == 4 || bits == 8 || bits == 16;
}

[[nodiscard]] constexpr std::uint32_t bitsLog2Of(std::uint8_t bits) noexcept
{
    return bits == 1 ? 0u : bits == 2 ? 1u : bits == 4 ? 2u : bits == 8 ? 3u : 4u;
}

/// Extracts one packed palette index.
///
/// This mirrors ChunkStorage's private packing, which the contract at the top of
/// world/ChunkStorage.hpp declares to be versioned save-format data rather than
/// an implementation detail. It is duplicated here rather than exposed because
/// the alternative is a bulk "adopt these words" setter on ChunkStorage, and a
/// setter that installs unvalidated index words is precisely what a corrupt file
/// needs to turn into an out-of-bounds palette read. Going through set() below
/// means a damaged payload can produce wrong terrain but never an invalid
/// ChunkStorage.
[[nodiscard]] std::uint32_t readPackedIndex(const std::vector<std::uint64_t>& words,
                                            std::uint8_t bits, std::size_t voxel) noexcept
{
    const std::uint32_t bitsLog2  = bitsLog2Of(bits);
    const std::uint32_t slotsLog2 = 6u - bitsLog2;
    const std::size_t   word      = voxel >> slotsLog2;
    const std::uint32_t slot      = static_cast<std::uint32_t>(voxel) & ((1u << slotsLog2) - 1u);
    const std::uint32_t shift     = slot << bitsLog2;
    const std::uint64_t mask      = (std::uint64_t{1} << bits) - 1u;
    return static_cast<std::uint32_t>((words[word] >> shift) & mask);
}

[[nodiscard]] ChunkLoadResult corruptResult(SaveError error) noexcept
{
    return ChunkLoadResult{ChunkLoadStatus::Corrupt, error, kLodFull};
}

/// Appends the light section, choosing the cheapest of the three encodings.
void appendLight(const ChunkStorage& storage, std::vector<std::byte>& out)
{
    if (!storage.hasLightData()) {
        bytes::putU8(out, kLightUniform);
        bytes::putU8(out, storage.uniformLight());
        return;
    }

    const std::vector<std::uint8_t>& light = storage.lightData();
    VOXL_ASSERT(light.size() == kChunkVolume, "light array is not one byte per voxel");

    // Light varies slowly - a sunlit section is long runs of 0xF0, a cave is long
    // runs of 0x00 - so run-length coding usually turns 32 KB into a few hundred
    // bytes. The loop abandons the attempt the moment the runs stop paying for
    // themselves, so the encoded size is never worse than raw.
    std::vector<std::uint8_t> runs;
    runs.reserve(512);

    std::size_t index = 0;
    while (index < kChunkVolume) {
        const std::uint8_t value = light[index];
        std::size_t        run   = 1;
        while (index + run < kChunkVolume && run < 255 && light[index + run] == value) {
            ++run;
        }
        runs.push_back(static_cast<std::uint8_t>(run));
        runs.push_back(value);
        index += run;

        if (runs.size() + 5 >= kChunkVolume) {
            break;  // encoding header + runs would exceed the raw array
        }
    }

    if (index < kChunkVolume) {
        bytes::putU8(out, kLightRaw);
        const auto* first = reinterpret_cast<const std::byte*>(light.data());
        out.insert(out.end(), first, first + light.size());
        return;
    }

    bytes::putU8(out, kLightRuns);
    bytes::putU32(out, static_cast<std::uint32_t>(runs.size() / 2));
    const auto* first = reinterpret_cast<const std::byte*>(runs.data());
    out.insert(out.end(), first, first + runs.size());
}

}  // namespace

// ------------------------------------------------------------------ codec --

std::vector<std::byte> WorldSave::encodeChunk(const Chunk& chunk)
{
    const ChunkStorage& storage = chunk.storage();
    const std::uint8_t  bits    = storage.bitsPerIndex();

    const std::vector<std::pair<std::uint16_t, SubVoxelGrid>> damage =
        chunk.subVoxels().sortedEntries();

    std::vector<std::byte> out;
    out.reserve(64 + storage.palette().size() * 2 + storage.indexWords().size() * 8 +
                damage.size() * kDamageEntryBytes);

    bytes::putU8(out, bits);
    if (bits == 0) {
        bytes::putU32(out, 1u);
        bytes::putU16(out, storage.uniformValue());
    } else {
        const std::vector<BlockId>& palette = storage.palette();
        bytes::putU32(out, static_cast<std::uint32_t>(palette.size()));
        for (const BlockId id : palette) {
            bytes::putU16(out, id);
        }

        const std::vector<std::uint64_t>& words = storage.indexWords();
        VOXL_ASSERT(words.size() == ChunkStorage::wordCountFor(bits),
                    "index word count disagrees with bitsPerIndex");
        for (const std::uint64_t word : words) {
            bytes::putU64(out, word);
        }
    }

    appendLight(storage, out);

    bytes::putU32(out, static_cast<std::uint32_t>(damage.size()));
    for (const auto& [blockIndex, grid] : damage) {
        bytes::putU16(out, blockIndex);
        bytes::putU16(out, grid.material);
        for (const std::uint64_t word : grid.bits) {
            bytes::putU64(out, word);
        }
    }

    return out;
}

ChunkLoadResult WorldSave::decodeChunk(std::span<const std::byte> payload, Chunk& chunk)
{
    if (payload.empty()) {
        return corruptResult(SaveError::EmptyPayload);
    }

    bytes::Reader reader{payload};

    // ---- voxels ----

    const std::uint8_t bits = reader.u8();
    if (!isLegalBitsPerIndex(bits)) {
        return corruptResult(SaveError::MalformedPayload);
    }

    const std::uint32_t paletteCount = reader.u32();
    if (!reader.ok()) {
        return corruptResult(SaveError::Truncated);
    }
    // A palette must be able to address itself at this width, and no section can
    // hold more distinct ids than it has voxels plus the one it started uniform
    // at. Both bounds are checked before the count is used to size anything.
    const std::uint64_t widthCapacity = bits == 0 ? 1u : (std::uint64_t{1} << bits);
    if (paletteCount == 0 || paletteCount > widthCapacity ||
        (bits == 0 && paletteCount != 1)) {
        return corruptResult(SaveError::MalformedPayload);
    }
    if (static_cast<std::size_t>(paletteCount) * 2u > reader.remaining()) {
        return corruptResult(SaveError::Truncated);
    }

    std::vector<BlockId> palette;
    palette.reserve(paletteCount);
    for (std::uint32_t i = 0; i < paletteCount; ++i) {
        palette.push_back(reader.u16());
    }

    const std::size_t          wordCount = ChunkStorage::wordCountFor(bits);
    std::vector<std::uint64_t> words;
    if (wordCount != 0) {
        if (wordCount * 8u > reader.remaining()) {
            return corruptResult(SaveError::Truncated);
        }
        words.resize(wordCount);
        for (std::size_t i = 0; i < wordCount; ++i) {
            words[i] = reader.u64();
        }
    }
    if (!reader.ok()) {
        return corruptResult(SaveError::Truncated);
    }

    // Materialise the voxels into scratch first. Nothing is written to the chunk
    // until every field has been validated, so a rejected payload leaves the
    // chunk exactly as it was rather than half-loaded or zeroed.
    std::vector<BlockId> voxels(kChunkVolume, palette[0]);
    if (bits != 0) {
        for (std::size_t i = 0; i < kChunkVolume; ++i) {
            const std::uint32_t index = readPackedIndex(words, bits, i);
            if (index >= paletteCount) {
                // The single most dangerous field in the format: an unchecked
                // index here is an out-of-bounds palette read on every later
                // getBlock() call.
                return corruptResult(SaveError::MalformedPayload);
            }
            voxels[i] = palette[index];
        }
    }

    // ---- light ----

    const std::uint8_t        lightEncoding = reader.u8();
    std::vector<std::uint8_t> light;
    std::uint8_t              uniformLight = 0;

    if (lightEncoding == kLightUniform) {
        uniformLight = reader.u8();
        if (!reader.ok()) {
            return corruptResult(SaveError::Truncated);
        }
    } else if (lightEncoding == kLightRaw) {
        if (reader.remaining() < kChunkVolume) {
            return corruptResult(SaveError::Truncated);
        }
        light.resize(kChunkVolume);
        if (!reader.copy(light.data(), kChunkVolume)) {
            return corruptResult(SaveError::Truncated);
        }
    } else if (lightEncoding == kLightRuns) {
        const std::uint32_t runCount = reader.u32();
        if (!reader.ok()) {
            return corruptResult(SaveError::Truncated);
        }
        if (runCount == 0 || static_cast<std::size_t>(runCount) * 2u > reader.remaining()) {
            return corruptResult(SaveError::Truncated);
        }
        light.reserve(kChunkVolume);
        for (std::uint32_t i = 0; i < runCount; ++i) {
            const std::uint8_t run   = reader.u8();
            const std::uint8_t value = reader.u8();
            if (run == 0 || light.size() + static_cast<std::size_t>(run) > kChunkVolume) {
                return corruptResult(SaveError::MalformedPayload);
            }
            light.insert(light.end(), run, value);
        }
        if (light.size() != kChunkVolume) {
            return corruptResult(SaveError::MalformedPayload);
        }
    } else {
        return corruptResult(SaveError::MalformedPayload);
    }

    // ---- sub-voxel damage ----

    const std::uint32_t damageCount = reader.u32();
    if (!reader.ok()) {
        return corruptResult(SaveError::Truncated);
    }
    if (damageCount > kChunkVolume) {
        return corruptResult(SaveError::MalformedPayload);
    }
    if (static_cast<std::size_t>(damageCount) * kDamageEntryBytes > reader.remaining()) {
        return corruptResult(SaveError::Truncated);
    }

    std::vector<std::pair<std::uint16_t, SubVoxelGrid>> damage;
    damage.reserve(damageCount);
    std::int32_t previousIndex = -1;
    for (std::uint32_t i = 0; i < damageCount; ++i) {
        const std::uint16_t blockIndex = reader.u16();
        const auto          material   = static_cast<BlockId>(reader.u16());

        SubVoxelGrid grid;
        grid.material = material;
        for (std::uint64_t& word : grid.bits) {
            word = reader.u64();
        }

        // Every one of these rules is half of the invariant at the top of
        // world/SubVoxel.hpp. Accepting a violation here would put the store and
        // the storage into exactly the disagreement that invariant forbids, and
        // the symptom would surface a long way from this function.
        if (blockIndex >= kChunkVolume) {
            return corruptResult(SaveError::MalformedPayload);
        }
        if (static_cast<std::int32_t>(blockIndex) <= previousIndex) {
            return corruptResult(SaveError::MalformedPayload);  // unordered or duplicated
        }
        previousIndex = static_cast<std::int32_t>(blockIndex);

        const std::size_t present = grid.count();
        if (present == 0 || present == kSubVoxelCount) {
            return corruptResult(SaveError::MalformedPayload);  // must be strictly partial
        }
        if (material == blocks::Air || material != voxels[blockIndex]) {
            return corruptResult(SaveError::MalformedPayload);
        }

        damage.emplace_back(blockIndex, grid);
    }

    if (!reader.ok()) {
        return corruptResult(SaveError::Truncated);
    }
    if (!reader.exhausted()) {
        // Trailing bytes mean the writer and the reader disagree about the
        // format even though the CRC passed, which is worse than a bit flip.
        return corruptResult(SaveError::MalformedPayload);
    }

    // ---- commit ----
    //
    // From here on nothing can fail, which is what makes "a rejected payload
    // never touches the chunk" true.

    ChunkStorage& storage = chunk.storage();
    chunk.subVoxels().clear();

    storage.fill(voxels[0]);
    for (std::size_t i = 0; i < kChunkVolume; ++i) {
        // fill() left every voxel at voxels[0]; writing only the differences
        // skips the palette lookup for the run of identical blocks that
        // dominates a real section.
        if (voxels[i] != voxels[0]) {
            storage.set(i, voxels[i]);
        }
    }

    if (light.empty()) {
        storage.fillLight(ChunkStorage::unpackSunlight(uniformLight),
                          ChunkStorage::unpackBlockLight(uniformLight));
    } else {
        storage.fillLight(ChunkStorage::unpackSunlight(light[0]),
                          ChunkStorage::unpackBlockLight(light[0]));
        for (std::size_t i = 0; i < kChunkVolume; ++i) {
            if (light[i] != light[0]) {
                storage.setLight(i, light[i]);
            }
        }
    }

    // Damage is re-applied by CARVING, not by installing grids: Chunk::breakSubVoxel
    // is the only sanctioned mutation path (world/SubVoxel.hpp), and driving the
    // load through it means the store and the storage cannot end up disagreeing
    // even if this decoder has a bug. The block already holds `material`, and
    // every grid was validated to have at least one sub-voxel left, so no call
    // here can report BlockRemoved.
    for (const auto& [blockIndex, grid] : damage) {
        for (std::size_t word = 0; word < kSubVoxelWords; ++word) {
            std::uint64_t missing = ~grid.bits[word];
            while (missing != 0) {
                const auto bit = static_cast<std::size_t>(std::countr_zero(missing));
                missing &= missing - 1u;

                [[maybe_unused]] const SubVoxelEdit edit =
                    chunk.breakSubVoxel(blockIndex, word * 64u + bit);
                VOXL_ASSERT(edit == SubVoxelEdit::Modified,
                            "restoring saved damage broke the sub-voxel invariant");
            }
        }
    }

    // The chunk has voxels but no geometry, and it matches what is on disk.
    chunk.markDirty();
    chunk.markSaved();

    return ChunkLoadResult{ChunkLoadStatus::Loaded, SaveError::None, chunk.lod()};
}

// -------------------------------------------------------------- lifecycle --

WorldSave::WorldSave(JobSystem& jobs, std::filesystem::path directory, std::uint64_t seed)
    : m_jobs(jobs), m_directory(std::move(directory)), m_seed(seed)
{
    std::error_code code;
    std::filesystem::create_directories(m_directory, code);
    if (code) {
        VOXL_LOG_ERROR("cannot create save directory '{}': {}", m_directory.string(),
                       code.message());
    }
}

WorldSave::~WorldSave()
{
    // Blocks on the queued writes, so the JobSystem must still be alive. That is
    // the documented ownership order and it is what the Application already has.
    // Wrapped because flush() waits on a condition variable, and letting that
    // throw out of a destructor would call std::terminate during shutdown - at
    // which point the log line below is the only evidence anyone would get.
    try {
        flush();
    } catch (const std::exception& failure) {
        VOXL_LOG_ERROR("WorldSave shutdown flush failed: {}", failure.what());
    } catch (...) {
        VOXL_LOG_ERROR("WorldSave shutdown flush failed");
    }
}

void WorldSave::flush()
{
    VOXL_CHECK(!m_jobs.onWorkerThread(), "WorldSave::flush() called from a worker thread");

    {
        std::unique_lock<std::mutex> lock(m_idleMutex);
        const bool settled = m_idleCv.wait_for(lock, kFlushTimeout, [this] {
            return m_writesInFlight.load(std::memory_order_acquire) == 0;
        });
        if (!settled) {
            VOXL_LOG_ERROR("WorldSave::flush() timed out with {} write(s) still outstanding; "
                           "those chunks are not on disk",
                           m_writesInFlight.load(std::memory_order_acquire));
        }
    }

    std::shared_lock<std::shared_mutex> lock(m_regionMutex);
    for (const auto& entry : m_regions) {
        entry.second->flush();
    }
}

void WorldSave::noteWriteStarted() noexcept
{
    m_writesInFlight.fetch_add(1, std::memory_order_release);
}

void WorldSave::noteWriteFinished() noexcept
{
    if (m_writesInFlight.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        // Taken briefly so a flush() that has just evaluated the predicate as
        // false cannot miss this notification and sleep out the full timeout.
        {
            std::lock_guard<std::mutex> lock(m_idleMutex);
        }
        m_idleCv.notify_all();
    }
}

SaveStats WorldSave::stats() const
{
    SaveStats out;
    out.chunksEncoded = m_chunksEncoded.load(std::memory_order_relaxed);
    out.chunksWritten = m_chunksWritten.load(std::memory_order_relaxed);
    out.writeFailures = m_writeFailures.load(std::memory_order_relaxed);
    out.chunksLoaded  = m_chunksLoaded.load(std::memory_order_relaxed);
    out.chunksAbsent  = m_chunksAbsent.load(std::memory_order_relaxed);
    out.chunksCorrupt = m_chunksCorrupt.load(std::memory_order_relaxed);
    out.bytesWritten  = m_bytesWritten.load(std::memory_order_relaxed);
    out.writesInFlight = m_writesInFlight.load(std::memory_order_relaxed);

    std::shared_lock<std::shared_mutex> lock(m_regionMutex);
    out.openRegions = m_regions.size();
    return out;
}

// ----------------------------------------------------------- region cache --

std::shared_ptr<RegionFile> WorldSave::acquireRegion(const RegionCoord& coord,
                                                     bool               createIfMissing)
{
    const std::uint64_t tick = m_useTick.fetch_add(1, std::memory_order_relaxed) + 1u;

    {
        std::shared_lock<std::shared_mutex> lock(m_regionMutex);
        const auto                          it = m_regions.find(coord);
        if (it != m_regions.end()) {
            it->second->noteUse(tick);
            return it->second;
        }
    }

    const std::filesystem::path path = RegionFile::pathFor(m_directory, coord.x, coord.z);
    if (!createIfMissing) {
        std::error_code code;
        if (!std::filesystem::exists(path, code) || code) {
            // Nothing has ever been saved here, so do not populate the cache with
            // a handle for it: a pure-read workload walking new terrain would
            // otherwise evict the regions it is actually using.
            return nullptr;
        }
    }

    // Constructed outside the lock because opening and validating a region reads
    // 16 KB from disk, and holding the map's writer lock across that would stall
    // every other save and load in the process.
    auto region = std::make_shared<RegionFile>(path, coord.x, coord.z, m_seed);
    region->noteUse(tick);

    std::unique_lock<std::shared_mutex> lock(m_regionMutex);
    const auto [it, inserted] = m_regions.emplace(coord, std::move(region));
    if (!inserted) {
        // Another thread won the race. Use its object: exactly one RegionFile per
        // file is what makes the per-file mutex a real exclusion, and the loser's
        // object is harmless because construction never writes anything.
        it->second->noteUse(tick);
        return it->second;
    }
    evictRegionsLocked();
    return it->second;
}

void WorldSave::evictRegionsLocked()
{
    while (m_regions.size() > kMaxOpenRegions) {
        auto victim = m_regions.begin();
        for (auto it = m_regions.begin(); it != m_regions.end(); ++it) {
            if (it->second->lastUse() < victim->second->lastUse()) {
                victim = it;
            }
        }
        // Erasing only drops the cache's reference. A worker mid-write still owns
        // a shared_ptr, so its file object stays alive and closes - flushing - the
        // moment that job returns.
        m_regions.erase(victim);
    }
}

// ------------------------------------------------------------------- save --

bool WorldSave::saveChunk(const ChunkPtr& chunk)
{
    VOXL_CHECK(!m_jobs.onWorkerThread(), "WorldSave::saveChunk() called from a worker thread");

    if (!chunk) {
        return false;
    }

    const ChunkState state = chunk->state();
    if (state == ChunkState::Empty) {
        return false;  // never generated; there is nothing here to write
    }
    if (state == ChunkState::Generating) {
        // A worker owns the voxels right now. Reading them from here is the
        // use-after-free World::isEditBlocked exists to prevent, with the roles
        // reversed. The chunk stays dirty and the next autosave picks it up.
        return false;
    }

    // AND THE STATE IS NOT THE WHOLE ANSWER. A column light job rewrites the
    // light array of every unlit section in its column from a worker, and
    // lighting deliberately does NOT put a chunk into a busy state - see the
    // LIGHTING note in ChunkManager.hpp for why it must not. So a chunk sitting
    // in Generated or Ready can still have a worker inside it, and encodeChunk
    // below reads the whole thing, light included. ChunkStorage materialises its
    // light array on first write, which reallocates; reading it from here while
    // that happens is the same use-after-free the Generating test above refuses,
    // arriving through a door the state machine does not watch.
    //
    // The probe is wired to isNeighbourhoodBusy(), the predicate the mesh path
    // already uses; it covers both worker readers, so this is the same rule
    // stated once more rather than a new one. Refusing is free: the chunk keeps
    // needsSave() and the next autosave tick - or the retire hook, which
    // ChunkManager already holds back for a claimed column - writes it then.
    //
    // AND IT IS DELIBERATELY WIDER THAN A READ STRICTLY NEEDS. KEEP IT THAT WAY.
    //
    // Narrowing this to the chunk's OWN column has been proposed and rejected
    // once already, so here is the reasoning in full. The argument for narrowing
    // is sound as far as it goes: a save is a READ, the only worker that WRITES
    // a chunk is a light job on its own column (or its own generate job, caught
    // above), a mesh job and a neighbouring column's light job merely read it,
    // and two readers do not race. isNeighbourhoodBusy() is the predicate that
    // blocks WRITES, borrowed wholesale.
    //
    // It is rejected because the cost of the two answers is not symmetric. The
    // wide predicate costs some refused saves, and a refused save is always
    // retried - the whole point of the paragraph above. The narrow one is only
    // correct for as long as "a light job writes nothing outside its claimed
    // column" stays true, and that fact is an emergent property of LightEngine
    // spread over a couple of thousand lines, not something the type system or
    // any assertion holds down. The day it stops being true, this returns a torn
    // chunk to disk and nothing anywhere reports it. Trading a bounded, retried
    // latency cost for an unbounded, silent correctness risk is the wrong side
    // of that trade, and the profiler has never once pointed here.
    if (m_busyProbe && m_busyProbe(chunk->position())) {
        return false;
    }

    if (chunk->lod() != kLodFull) {
        // A coarse chunk's voxels are a 2^L downsample of terrain the seed
        // reproduces exactly. Writing it would put an approximation where the
        // real chunk belongs and a later load would restore it as if it were
        // full resolution.
        return false;
    }

    const ChunkPos position = chunk->position();
    if (position.y < 0 || position.y >= kWorldSectionCount) {
        return false;
    }

    // THE READ HAPPENS HERE, ON THE MAIN THREAD. See the header comment: the
    // worker below never sees a Chunk, only the bytes this produced.
    auto buffer = std::make_shared<std::vector<std::byte>>(encodeChunk(*chunk));
    m_chunksEncoded.fetch_add(1, std::memory_order_relaxed);
    if (buffer->empty()) {
        return false;
    }

    // Cleared optimistically so a save that is already queued is not queued again
    // by the next autosave tick. The worker puts it back if the write fails.
    chunk->markSaved();

    std::weak_ptr<Chunk> weak = chunk;
    noteWriteStarted();

    m_jobs.submitDetached(JobPriority::Low, [this, position, buffer, weak]() {
        try {
            SaveError error = SaveError::None;
            bool      ok    = false;

            if (const std::shared_ptr<RegionFile> region =
                    acquireRegion(toRegionCoord(position), true)) {
                ok = region->write(position, std::span<const std::byte>{*buffer}, kLodFull, error);
            } else {
                error = SaveError::Io;
            }

            if (ok) {
                m_chunksWritten.fetch_add(1, std::memory_order_relaxed);
                m_bytesWritten.fetch_add(buffer->size(), std::memory_order_relaxed);
            } else {
                m_writeFailures.fetch_add(1, std::memory_order_relaxed);
                VOXL_LOG_ERROR("failed to save chunk ({}, {}, {}): {}", position.x, position.y,
                               position.z, toString(error));
                // Restore the dirty flag on the MAIN THREAD, which is where the
                // flag's contract puts it, and only if the chunk is still alive.
                m_jobs.mainThreadQueue().push([weak] {
                    if (const ChunkPtr live = weak.lock()) {
                        live->markModified();
                    }
                });
            }
        } catch (const std::exception& failure) {
            m_writeFailures.fetch_add(1, std::memory_order_relaxed);
            VOXL_LOG_ERROR("save job threw: {}", failure.what());
        } catch (...) {
            m_writeFailures.fetch_add(1, std::memory_order_relaxed);
            VOXL_LOG_ERROR("save job threw a non-std exception");
        }
        // Outside the try so a throwing write can never strand flush() forever.
        noteWriteFinished();
    });

    return true;
}

std::size_t WorldSave::saveChunks(std::span<const ChunkPtr> chunks)
{
    std::size_t queued = 0;
    for (const ChunkPtr& chunk : chunks) {
        if (chunk && chunk->needsSave() && saveChunk(chunk)) {
            ++queued;
        }
    }
    return queued;
}

void WorldSave::onChunkRetired(const ChunkPtr& chunk)
{
    if (chunk && chunk->needsSave()) {
        (void)saveChunk(chunk);
    }
}

std::function<void(const ChunkPtr&)> WorldSave::retireHook()
{
    return [this](const ChunkPtr& chunk) { onChunkRetired(chunk); };
}

void WorldSave::setBusyProbe(ChunkBusyProbeFn probe)
{
    VOXL_CHECK(!m_jobs.onWorkerThread(), "WorldSave::setBusyProbe() called from a worker thread");
    m_busyProbe = std::move(probe);
}

// ------------------------------------------------------------------- load --

ChunkLoadResult WorldSave::loadChunk(Chunk& chunk)
{
    const ChunkPos position = chunk.position();

    if (position.y < 0 || position.y >= kWorldSectionCount) {
        m_chunksAbsent.fetch_add(1, std::memory_order_relaxed);
        return ChunkLoadResult{};
    }

    if (chunk.lod() != kLodFull) {
        // Only level 0 is ever written; coarser levels come from the seed by
        // design, so this is an absence rather than a miss.
        m_chunksAbsent.fetch_add(1, std::memory_order_relaxed);
        return ChunkLoadResult{};
    }

    const std::shared_ptr<RegionFile> region = acquireRegion(toRegionCoord(position), false);
    if (!region) {
        m_chunksAbsent.fetch_add(1, std::memory_order_relaxed);
        return ChunkLoadResult{};
    }

    std::vector<std::byte> payload;
    LodLevel               storedLod = kLodFull;
    SaveError              error     = SaveError::None;

    const ChunkLoadStatus status = region->read(position, payload, storedLod, error);
    if (status == ChunkLoadStatus::Absent) {
        m_chunksAbsent.fetch_add(1, std::memory_order_relaxed);
        return ChunkLoadResult{};
    }
    if (status == ChunkLoadStatus::Corrupt) {
        m_chunksCorrupt.fetch_add(1, std::memory_order_relaxed);
        VOXL_LOG_WARN("chunk ({}, {}, {}) could not be read ({}); regenerating it from the seed",
                      position.x, position.y, position.z, toString(error));
        return corruptResult(error);
    }
    if (storedLod != kLodFull) {
        m_chunksCorrupt.fetch_add(1, std::memory_order_relaxed);
        VOXL_LOG_WARN("chunk ({}, {}, {}) was stored at LOD {} but only level 0 is authored; "
                      "regenerating it from the seed",
                      position.x, position.y, position.z, storedLod);
        return ChunkLoadResult{ChunkLoadStatus::Corrupt, SaveError::LodMismatch, storedLod};
    }

    const ChunkLoadResult result = decodeChunk(std::span<const std::byte>{payload}, chunk);
    if (result.status == ChunkLoadStatus::Loaded) {
        m_chunksLoaded.fetch_add(1, std::memory_order_relaxed);
    } else {
        m_chunksCorrupt.fetch_add(1, std::memory_order_relaxed);
        VOXL_LOG_WARN("chunk ({}, {}, {}) payload rejected ({}); regenerating it from the seed",
                      position.x, position.y, position.z, toString(result.error));
    }
    return result;
}

// --------------------------------------------------------------- metadata --

std::filesystem::path WorldSave::metadataPath(const std::filesystem::path& directory)
{
    return directory / "level.vxw";
}

bool WorldSave::writeMetadata(const WorldMetadata& metadata)
{
    std::vector<std::byte> out;
    out.reserve(kMetadataBytes);

    bytes::putU32(out, kMetadataMagic);
    bytes::putU16(out, kSaveFormatVersion);
    bytes::putU16(out, 0u);
    bytes::putU64(out, metadata.seed);
    bytes::putF32(out, metadata.playerPosition.x);
    bytes::putF32(out, metadata.playerPosition.y);
    bytes::putF32(out, metadata.playerPosition.z);
    bytes::putF32(out, metadata.yawDegrees);
    bytes::putF32(out, metadata.pitchDegrees);
    bytes::putU8(out, metadata.hotbarSlot);
    bytes::putU8(out, 0u);
    bytes::putU16(out, 0u);
    bytes::putF32(out, metadata.timeOfDay);
    bytes::putF64(out, metadata.playTimeSeconds);
    bytes::putU32(out, crc32(std::span<const std::byte>{out}));

    VOXL_ASSERT(out.size() == kMetadataBytes, "metadata layout drifted from kMetadataBytes");

    // Temp-and-rename, unlike a region file. This one is small enough that
    // rewriting it wholesale is free, and it is the file whose loss makes every
    // region in the directory unreachable - so it gets the strongest guarantee
    // available without platform calls: the old file is intact until a single
    // directory operation replaces it.
    const std::filesystem::path final     = metadataPath(m_directory);
    std::filesystem::path       temporary = final;
    temporary += ".tmp";

    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream.is_open()) {
            VOXL_LOG_ERROR("cannot open '{}' to write world metadata", temporary.string());
            return false;
        }
        stream.write(reinterpret_cast<const char*>(out.data()),
                     static_cast<std::streamsize>(out.size()));
        stream.flush();
        if (!stream) {
            VOXL_LOG_ERROR("failed writing world metadata to '{}'", temporary.string());
            return false;
        }
    }

    std::error_code code;
    std::filesystem::rename(temporary, final, code);
    if (code) {
        VOXL_LOG_ERROR("cannot replace '{}' with the new world metadata: {}", final.string(),
                       code.message());
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    return true;
}

bool readWorldMetadata(const std::filesystem::path& directory, WorldMetadata& out,
                       SaveError& error)
{
    error = SaveError::None;

    const std::filesystem::path path = WorldSave::metadataPath(directory);

    std::error_code      code;
    const std::uintmax_t size = std::filesystem::file_size(path, code);
    if (code) {
        return false;  // missing is not an error: this is a brand-new world
    }
    if (size < kMetadataBytes || size > kMaxMetadataBytes) {
        error = SaveError::Truncated;
        VOXL_LOG_ERROR("world metadata '{}' is {} bytes; expected {}", path.string(), size,
                       kMetadataBytes);
        return false;
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        error = SaveError::Io;
        return false;
    }

    std::vector<std::byte> buffer(kMetadataBytes);
    stream.read(reinterpret_cast<char*>(buffer.data()),
                static_cast<std::streamsize>(buffer.size()));
    if (static_cast<std::size_t>(stream.gcount()) != buffer.size()) {
        error = SaveError::Truncated;
        return false;
    }

    const std::uint32_t storedCrc = [&buffer] {
        bytes::Reader tail{std::span<const std::byte>{buffer}.subspan(kMetadataBytes - 4)};
        return tail.u32();
    }();
    if (crc32(std::span<const std::byte>{buffer}.subspan(0, kMetadataBytes - 4)) != storedCrc) {
        error = SaveError::BadChecksum;
        VOXL_LOG_ERROR("world metadata '{}' failed its checksum; the world will start with "
                       "default player state",
                       path.string());
        return false;
    }

    bytes::Reader reader{std::span<const std::byte>{buffer}};
    if (reader.u32() != kMetadataMagic) {
        error = SaveError::BadMagic;
        VOXL_LOG_ERROR("world metadata '{}' is not a Voxl world file", path.string());
        return false;
    }

    const std::uint16_t version = reader.u16();
    if (version != kSaveFormatVersion) {
        error = SaveError::UnsupportedVersion;
        VOXL_LOG_ERROR("world metadata '{}' uses save format version {}, this build understands "
                       "{}. Refusing to parse it rather than guess at the layout",
                       path.string(), version, kSaveFormatVersion);
        return false;
    }

    WorldMetadata parsed;
    parsed.formatVersion = version;
    (void)reader.u16();  // reserved
    parsed.seed            = reader.u64();
    parsed.playerPosition.x = reader.f32();
    parsed.playerPosition.y = reader.f32();
    parsed.playerPosition.z = reader.f32();
    parsed.yawDegrees      = reader.f32();
    parsed.pitchDegrees    = reader.f32();
    parsed.hotbarSlot      = reader.u8();
    (void)reader.u8();   // reserved
    (void)reader.u16();  // reserved
    parsed.timeOfDay       = reader.f32();
    parsed.playTimeSeconds = reader.f64();

    if (!reader.ok()) {
        error = SaveError::Truncated;
        return false;
    }

    out = parsed;
    return true;
}

bool WorldSave::readMetadata(WorldMetadata& out, SaveError& error) const
{
    return readWorldMetadata(m_directory, out, error);
}

// --------------------------------------------------------------- autosave --

void WorldSave::setAutosaveIntervalSeconds(double seconds) noexcept
{
    // A sub-second autosave would encode the whole dirty set every frame; the
    // floor is a guard rail, not a policy.
    m_autosaveInterval = seconds < 1.0 ? 1.0 : seconds;
}

bool WorldSave::tickAutosave(double nowSeconds) noexcept
{
    if (!m_autosaveArmed) {
        // The first call establishes the epoch. Firing immediately would save an
        // untouched world on the first frame after load, which is pure I/O for
        // no change.
        m_autosaveArmed = true;
        m_lastAutosave  = nowSeconds;
        return false;
    }
    if (nowSeconds - m_lastAutosave < m_autosaveInterval) {
        return false;
    }
    m_lastAutosave = nowSeconds;
    return true;
}

}  // namespace voxl
