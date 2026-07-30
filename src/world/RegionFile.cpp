// Region file I/O: header/table validation, sector allocation, payload commit.
//
// Everything in this file is written on the assumption that the bytes on disk
// are hostile. Every length is bounded before it is used to size an allocation,
// every offset is checked against the real file size rather than against another
// number read from the same file, and no read hands back data that has not
// already passed its CRC. The unit test suite corrupts each of those fields in
// turn; if a check here disappears, one of those tests turns into a crash.

#include "world/RegionFile.hpp"

#include "core/Log.hpp"

#include <algorithm>
#include <array>
#include <system_error>
#include <utility>

namespace voxl {
namespace {

/// Reflected CRC-32 table, built once at compile time so there is no
/// initialisation order or thread-safety question about it at all.
[[nodiscard]] constexpr std::array<std::uint32_t, 256> makeCrcTable() noexcept
{
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256u; ++i) {
        std::uint32_t value = i;
        for (int bit = 0; bit < 8; ++bit) {
            value = (value & 1u) != 0u ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
        }
        table[i] = value;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> kCrcTable = makeCrcTable();

/// The well-known check value for CRC-32/ISO-HDLC over "123456789". If the table
/// or the loop below is ever mangled, this fails at compile time rather than
/// silently invalidating every save file already on disk.
[[nodiscard]] constexpr std::uint32_t crcOfCheckString() noexcept
{
    constexpr char   kCheck[] = "123456789";
    std::uint32_t    crc      = 0xFFFFFFFFu;
    for (std::size_t i = 0; i + 1 < sizeof(kCheck); ++i) {
        crc = kCrcTable[(crc ^ static_cast<std::uint8_t>(kCheck[i])) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}
static_assert(crcOfCheckString() == 0xCBF43926u, "CRC-32 implementation does not match the standard");

[[nodiscard]] std::uint64_t sectorToOffset(std::uint32_t sector) noexcept
{
    return static_cast<std::uint64_t>(sector) << kRegionSectorShift;
}

[[nodiscard]] std::uint64_t fileSizeOf(const std::filesystem::path& path) noexcept
{
    std::error_code    code;
    const std::uintmax_t size = std::filesystem::file_size(path, code);
    return code ? 0u : static_cast<std::uint64_t>(size);
}

}  // namespace

// ------------------------------------------------------------------ enums --

const char* toString(SaveError error) noexcept
{
    switch (error) {
        case SaveError::None:               return "None";
        case SaveError::Io:                 return "Io";
        case SaveError::BadMagic:           return "BadMagic";
        case SaveError::UnsupportedVersion: return "UnsupportedVersion";
        case SaveError::Truncated:          return "Truncated";
        case SaveError::BadChecksum:        return "BadChecksum";
        case SaveError::OffsetOutOfRange:   return "OffsetOutOfRange";
        case SaveError::EmptyPayload:       return "EmptyPayload";
        case SaveError::MalformedPayload:   return "MalformedPayload";
        case SaveError::LodMismatch:        return "LodMismatch";
        case SaveError::SeedMismatch:       return "SeedMismatch";
        case SaveError::ReadOnly:           return "ReadOnly";
    }
    return "Unknown";
}

const char* toString(ChunkLoadStatus status) noexcept
{
    switch (status) {
        case ChunkLoadStatus::Loaded:  return "Loaded";
        case ChunkLoadStatus::Absent:  return "Absent";
        case ChunkLoadStatus::Corrupt: return "Corrupt";
    }
    return "Unknown";
}

// -------------------------------------------------------------------- crc --

std::uint32_t crc32(std::span<const std::byte> data) noexcept
{
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const std::byte value : data) {
        crc = kCrcTable[(crc ^ static_cast<std::uint8_t>(value)) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

// ------------------------------------------------------------- RegionFile --

std::filesystem::path RegionFile::pathFor(const std::filesystem::path& directory,
                                          std::int32_t regionX, std::int32_t regionZ)
{
    return directory / ("r." + std::to_string(regionX) + "." + std::to_string(regionZ) + ".vxr");
}

RegionFile::RegionFile(std::filesystem::path path, std::int32_t regionX, std::int32_t regionZ,
                       std::uint64_t seed)
    : m_path(std::move(path)), m_regionX(regionX), m_regionZ(regionZ), m_seed(seed)
{
    m_table.assign(kRegionEntryCount, Entry{});

    std::error_code code;
    if (!std::filesystem::exists(m_path, code) || code) {
        // Not an error: the file is created by the first write. Until then every
        // read is Absent, which is exactly "generate from the seed".
        return;
    }

    adoptExisting(seed);
}

void RegionFile::adoptExisting(std::uint64_t seed)
{
    m_fileBytes = fileSizeOf(m_path);

    m_stream.open(m_path, std::ios::binary | std::ios::in | std::ios::out);
    if (!m_stream.is_open()) {
        m_openError = SaveError::Io;
        VOXL_LOG_ERROR("region {}: cannot open '{}' for update", m_path.filename().string(),
                       m_path.string());
        return;
    }

    if (m_fileBytes < kRegionHeaderBytes + kRegionTableBytes) {
        m_openError         = SaveError::Truncated;
        m_quarantinePending = true;
        VOXL_LOG_ERROR("region '{}' is truncated ({} bytes, header+table needs {}); "
                       "its chunks will be regenerated from the seed",
                       m_path.filename().string(), m_fileBytes,
                       kRegionHeaderBytes + kRegionTableBytes);
        return;
    }

    std::array<std::byte, kRegionHeaderBytes> header{};
    if (!readAt(0, header.data(), header.size())) {
        m_openError         = SaveError::Io;
        m_quarantinePending = true;
        return;
    }

    bytes::Reader          reader{std::span<const std::byte>{header}};
    const std::uint32_t    magic          = reader.u32();
    const std::uint16_t    version        = reader.u16();
    const std::uint16_t    columns        = reader.u16();
    const std::uint16_t    sections       = reader.u16();
    const std::uint16_t    sectorShift    = reader.u16();
    const std::int32_t     storedX        = reader.i32();
    const std::int32_t     storedZ        = reader.i32();
    const std::uint64_t    storedSeed     = reader.u64();
    const std::uint32_t    storedHeaderCrc = reader.u32();

    if (magic != kRegionMagic) {
        m_openError         = SaveError::BadMagic;
        m_quarantinePending = true;
        VOXL_LOG_ERROR("region '{}' has magic 0x{:08X}, expected 0x{:08X}; not a Voxl region file. "
                       "Its chunks will be regenerated from the seed",
                       m_path.filename().string(), magic, kRegionMagic);
        return;
    }

    const std::uint32_t computedCrc =
        crc32(std::span<const std::byte>{header.data(), kRegionHeaderCrcBytes});
    if (computedCrc != storedHeaderCrc) {
        m_openError         = SaveError::BadChecksum;
        m_quarantinePending = true;
        VOXL_LOG_ERROR("region '{}' header checksum is 0x{:08X}, expected 0x{:08X}; "
                       "its chunks will be regenerated from the seed",
                       m_path.filename().string(), computedCrc, storedHeaderCrc);
        return;
    }

    if (version > kSaveFormatVersion) {
        // The one case where refusing to touch the file matters more than being
        // able to save here: overwriting it would destroy a world a newer build
        // can still read.
        m_openError = SaveError::UnsupportedVersion;
        m_readOnly  = true;
        VOXL_LOG_ERROR("region '{}' was written by a NEWER save format (version {}, this build "
                       "understands {}). Refusing to read or modify it; its chunks will be "
                       "regenerated from the seed and nothing will be written over them",
                       m_path.filename().string(), version, kSaveFormatVersion);
        return;
    }
    if (version < kSaveFormatVersion) {
        m_openError = SaveError::UnsupportedVersion;
        m_readOnly  = true;
        VOXL_LOG_ERROR("region '{}' uses save format version {}, which this build no longer "
                       "reads (current {}). Its chunks will be regenerated from the seed",
                       m_path.filename().string(), version, kSaveFormatVersion);
        return;
    }

    if (columns != static_cast<std::uint16_t>(kRegionSizeColumns) ||
        sections != static_cast<std::uint16_t>(kWorldSectionCount) ||
        sectorShift != static_cast<std::uint16_t>(kRegionSectorShift)) {
        m_openError = SaveError::UnsupportedVersion;
        m_readOnly  = true;
        VOXL_LOG_ERROR("region '{}' has incompatible geometry ({}x{} columns, {} sections, sector "
                       "shift {}); refusing to touch it",
                       m_path.filename().string(), columns, columns, sections, sectorShift);
        return;
    }

    if (storedX != m_regionX || storedZ != m_regionZ) {
        m_openError         = SaveError::MalformedPayload;
        m_quarantinePending = true;
        VOXL_LOG_ERROR("region '{}' says it is ({}, {}) but was opened as ({}, {}); "
                       "its chunks will be regenerated from the seed",
                       m_path.filename().string(), storedX, storedZ, m_regionX, m_regionZ);
        return;
    }

    if (storedSeed != seed) {
        // Another world's chunks in this directory. Loading them would splice a
        // different terrain into this one, which is worse than losing them.
        m_openError = SaveError::SeedMismatch;
        m_readOnly  = true;
        VOXL_LOG_ERROR("region '{}' belongs to world seed {} but this world is seed {}; "
                       "refusing to read or modify it",
                       m_path.filename().string(), storedSeed, seed);
        return;
    }

    std::vector<std::byte> table(kRegionTableBytes);
    if (!readAt(kRegionHeaderBytes, table.data(), table.size())) {
        m_openError         = SaveError::Truncated;
        m_quarantinePending = true;
        VOXL_LOG_ERROR("region '{}': offset table is unreadable; its chunks will be regenerated",
                       m_path.filename().string());
        return;
    }

    bytes::Reader tableReader{std::span<const std::byte>{table}};
    for (std::size_t i = 0; i < kRegionEntryCount; ++i) {
        Entry& entry      = m_table[i];
        entry.sector      = tableReader.u32();
        entry.sectorCount = tableReader.u16();
        entry.lod         = tableReader.u8();
        entry.flags       = tableReader.u8();
    }
    if (!tableReader.ok()) {
        m_openError         = SaveError::Truncated;
        m_quarantinePending = true;
        return;
    }

    rebuildSectorMap();
    m_healthy = true;
}

void RegionFile::rebuildSectorMap()
{
    const std::size_t fileSectors =
        static_cast<std::size_t>((m_fileBytes + kRegionSectorSize - 1) / kRegionSectorSize);
    m_sectorUsed.assign(std::max(fileSectors, kRegionFirstDataSector), 0u);

    // The header and table are permanently occupied.
    for (std::size_t i = 0; i < kRegionFirstDataSector; ++i) {
        m_sectorUsed[i] = 1u;
    }

    // Both passes below index `claims` with sectors that passed `plausible`, so
    // the bound they are tested against must be the one `claims` was sized to and
    // must not move underneath them. Snapshotting it here rather than re-reading
    // m_sectorUsed.size() makes that independent of whether markSectorsUsed ever
    // grows the map.
    const std::size_t mapSectors = m_sectorUsed.size();

    // An entry is only worth looking at if the span it claims could physically
    // exist in this file. The two rejected shapes are a payload that would sit
    // inside the header/table, and one that runs past the end of the file; read()
    // refuses both, and the allocator must not reserve space for either.
    const auto plausible = [mapSectors](const Entry& entry) noexcept {
        if (entry.sector == 0 || entry.sectorCount == 0) {
            return false;
        }
        if (entry.sector < kRegionFirstDataSector) {
            return false;
        }
        const std::uint64_t end = static_cast<std::uint64_t>(entry.sector) + entry.sectorCount;
        return end <= mapSectors;
    };

    // PASS 1 - how many entries claim each sector, saturating at two. A sector
    // claimed twice is contested: the table is damaged and there is no way to
    // tell which of the claimants holds the real payload.
    std::vector<std::uint8_t> claims(mapSectors, 0u);
    for (const Entry& entry : m_table) {
        if (!plausible(entry)) {
            continue;
        }
        const std::size_t end = static_cast<std::size_t>(entry.sector) + entry.sectorCount;
        for (std::size_t i = entry.sector; i < end; ++i) {
            claims[i] = static_cast<std::uint8_t>(std::min<int>(claims[i] + 1, 2));
        }
    }

    // PASS 2 - reserve, then decide ownership.
    //
    // Anything the table does not point at is free, which is how sectors orphaned
    // by a crash between "payload written" and "entry committed" come back into
    // circulation.
    //
    // A contested sector is still RESERVED - handing it out would overwrite
    // whichever claimant is the genuine payload - but it is owned by nobody, so
    // no entry may ever release it. Ownership is recorded per entry precisely so
    // that write() can check it before freeing anything; see the asymmetry note
    // there. An implausible entry is not reserved and not owned either: it never
    // cost the allocator a sector, so replacing it frees nothing.
    for (Entry& entry : m_table) {
        entry.ownedSector = 0;
        entry.ownedCount  = 0;
        if (!plausible(entry)) {
            continue;
        }

        const std::size_t end       = static_cast<std::size_t>(entry.sector) + entry.sectorCount;
        bool              exclusive = true;
        for (std::size_t i = entry.sector; i < end; ++i) {
            exclusive = exclusive && claims[i] == 1u;
        }

        markSectorsUsed(entry.sector, entry.sectorCount);
        if (exclusive) {
            entry.ownedSector = entry.sector;
            entry.ownedCount  = entry.sectorCount;
        }
    }
}

bool RegionFile::createFresh()
{
    if (m_quarantinePending) {
        quarantineDamagedFile();
    }

    if (m_stream.is_open()) {
        m_stream.close();
    }
    m_stream.clear();

    {
        // A separate truncating stream is the portable way to bring the file
        // into existence; fstream with in|out refuses to create one.
        std::ofstream create(m_path, std::ios::binary | std::ios::trunc);
        if (!create.is_open()) {
            VOXL_LOG_ERROR("cannot create region file '{}'", m_path.string());
            return false;
        }
    }

    m_stream.open(m_path, std::ios::binary | std::ios::in | std::ios::out);
    if (!m_stream.is_open()) {
        VOXL_LOG_ERROR("cannot reopen region file '{}' for update", m_path.string());
        return false;
    }

    m_table.assign(kRegionEntryCount, Entry{});
    m_fileBytes = 0;

    std::array<std::byte, kRegionHeaderBytes> header{};
    encodeHeader(m_regionX, m_regionZ, m_seed, header.data());
    if (!writeAt(0, header.data(), header.size())) {
        return false;
    }

    // Zero the table and the padding up to the first data sector in one go, so
    // the file is never shorter than its own fixed structures.
    const std::size_t      prefixBytes = kRegionFirstDataSector * kRegionSectorSize;
    std::vector<std::byte> zeros(prefixBytes - kRegionHeaderBytes, std::byte{0});
    if (!writeAt(kRegionHeaderBytes, zeros.data(), zeros.size())) {
        return false;
    }
    if (!m_stream.flush()) {
        return false;
    }

    rebuildSectorMap();
    m_openError         = SaveError::None;
    m_quarantinePending = false;
    m_healthy           = true;
    return true;
}

void RegionFile::quarantineDamagedFile()
{
    if (m_stream.is_open()) {
        m_stream.close();
    }
    m_stream.clear();

    std::filesystem::path quarantine = m_path;
    quarantine += ".corrupt";

    std::error_code code;
    std::filesystem::remove(quarantine, code);  // best effort; a stale one may exist
    code.clear();
    std::filesystem::rename(m_path, quarantine, code);
    if (code) {
        VOXL_LOG_WARN("could not move damaged region '{}' aside ({}); it will be overwritten",
                      m_path.string(), code.message());
        return;
    }
    VOXL_LOG_WARN("damaged region '{}' moved to '{}'; a fresh region takes its place",
                  m_path.filename().string(), quarantine.filename().string());
}

void RegionFile::encodeHeader(std::int32_t regionX, std::int32_t regionZ, std::uint64_t seed,
                              std::byte out[kRegionHeaderBytes]) noexcept
{
    std::vector<std::byte> buffer;
    buffer.reserve(kRegionHeaderBytes);
    bytes::putU32(buffer, kRegionMagic);
    bytes::putU16(buffer, kSaveFormatVersion);
    bytes::putU16(buffer, static_cast<std::uint16_t>(kRegionSizeColumns));
    bytes::putU16(buffer, static_cast<std::uint16_t>(kWorldSectionCount));
    bytes::putU16(buffer, static_cast<std::uint16_t>(kRegionSectorShift));
    bytes::putI32(buffer, regionX);
    bytes::putI32(buffer, regionZ);
    bytes::putU64(buffer, seed);

    // Covers every field written so far; the trailing reserved bytes are zero
    // and deliberately outside the checksum so a future field can be added
    // without invalidating existing files at the header level.
    VOXL_ASSERT(buffer.size() == kRegionHeaderCrcBytes, "header checksum coverage drifted");
    bytes::putU32(buffer, crc32(std::span<const std::byte>{buffer}));

    std::fill_n(out, kRegionHeaderBytes, std::byte{0});
    std::copy(buffer.begin(), buffer.end(), out);
}

void RegionFile::encodeEntry(const Entry& entry, std::byte out[kRegionEntryBytes]) noexcept
{
    std::vector<std::byte> buffer;
    buffer.reserve(kRegionEntryBytes);
    bytes::putU32(buffer, entry.sector);
    bytes::putU16(buffer, entry.sectorCount);
    bytes::putU8(buffer, entry.lod);
    bytes::putU8(buffer, entry.flags);
    std::copy(buffer.begin(), buffer.end(), out);
}

// -------------------------------------------------------------------- i/o --

bool RegionFile::readAt(std::uint64_t offset, void* destination, std::size_t count)
{
    if (count == 0) {
        return true;
    }
    m_stream.clear();
    m_stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!m_stream) {
        return false;
    }
    m_stream.read(static_cast<char*>(destination), static_cast<std::streamsize>(count));
    return m_stream.good() &&
           static_cast<std::size_t>(m_stream.gcount()) == count;
}

bool RegionFile::writeAt(std::uint64_t offset, const void* source, std::size_t count)
{
    if (count == 0) {
        return true;
    }
    m_stream.clear();
    m_stream.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!m_stream) {
        return false;
    }
    m_stream.write(static_cast<const char*>(source), static_cast<std::streamsize>(count));
    if (!m_stream) {
        return false;
    }
    m_fileBytes = std::max(m_fileBytes, offset + count);
    return true;
}

// -------------------------------------------------------------- allocator --

void RegionFile::markSectorsUsed(std::uint32_t sector, std::uint16_t count)
{
    const std::size_t end = static_cast<std::size_t>(sector) + count;
    if (m_sectorUsed.size() < end) {
        m_sectorUsed.resize(end, 0u);
    }
    for (std::size_t i = sector; i < end; ++i) {
        m_sectorUsed[i] = 1u;
    }
}

void RegionFile::releaseSectors(std::uint32_t sector, std::uint16_t count)
{
    // The header and the offset table are never free, whatever a caller believes
    // it owns. Clamping here rather than trusting the call sites means a single
    // bad number can waste sectors but can never unmark the file's own structure.
    const std::size_t begin = std::max<std::size_t>(sector, kRegionFirstDataSector);
    const std::size_t end   = std::min<std::size_t>(static_cast<std::size_t>(sector) + count,
                                                    m_sectorUsed.size());
    for (std::size_t i = begin; i < end; ++i) {
        m_sectorUsed[i] = 0u;
    }
}

std::uint32_t RegionFile::reserveSectors(std::uint16_t count)
{
    // write() derives `count` from a payload that is already known to be
    // non-empty, so a zero-sector request would mean the caller lost track of its
    // own record layout.
    VOXL_ASSERT(count != 0, "the sector allocator was asked for a zero-length span");

    // First fit. The number of holes in a region is tiny (one per chunk that
    // grew or shrank since the file was opened), so a linear scan over at most a
    // few tens of thousands of bytes beats maintaining a free list that has to
    // stay correct across crash recovery.
    std::size_t run = 0;
    for (std::size_t i = kRegionFirstDataSector; i < m_sectorUsed.size(); ++i) {
        run = m_sectorUsed[i] == 0u ? run + 1 : 0;
        if (run >= count) {
            const auto start = static_cast<std::uint32_t>(i + 1 - count);
            markSectorsUsed(start, count);
            return start;
        }
    }

    // Nothing big enough between the live payloads, so grow the file. `run` is
    // now the length of the free run trailing the map; starting inside it rather
    // than past it keeps a grow from stranding the sectors immediately below the
    // new payload, which would otherwise never be reclaimed until the next open.
    std::size_t start = std::max(m_sectorUsed.size(), kRegionFirstDataSector);
    start -= std::min(run, start - kRegionFirstDataSector);

    const auto sector = static_cast<std::uint32_t>(start);
    markSectorsUsed(sector, count);
    return sector;
}

// ------------------------------------------------------------------- read --

ChunkLoadStatus RegionFile::read(const ChunkPos& position, std::vector<std::byte>& payload,
                                 LodLevel& lod, SaveError& error)
{
    payload.clear();
    lod   = kLodFull;
    error = SaveError::None;

    if (position.y < 0 || position.y >= kWorldSectionCount) {
        error = SaveError::OffsetOutOfRange;
        return ChunkLoadStatus::Corrupt;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_openError != SaveError::None) {
        // The file exists and is unusable. Every chunk in it must regenerate, and
        // saying Corrupt rather than Absent is what lets the caller count and
        // report that instead of silently pretending the world is new.
        error = m_openError;
        return ChunkLoadStatus::Corrupt;
    }
    if (!m_healthy) {
        return ChunkLoadStatus::Absent;  // never written
    }

    const Entry entry = m_table[regionEntryIndex(position)];
    if (entry.sector == 0) {
        return ChunkLoadStatus::Absent;
    }
    if (entry.sectorCount == 0) {
        error = SaveError::EmptyPayload;
        return ChunkLoadStatus::Corrupt;
    }
    if (entry.sector < kRegionFirstDataSector) {
        error = SaveError::OffsetOutOfRange;
        return ChunkLoadStatus::Corrupt;
    }

    const std::uint64_t begin = sectorToOffset(entry.sector);
    const std::uint64_t span  = sectorToOffset(entry.sectorCount);
    if (begin + span > m_fileBytes) {
        error = SaveError::OffsetOutOfRange;
        return ChunkLoadStatus::Corrupt;
    }

    std::array<std::byte, kChunkPayloadHeaderBytes> header{};
    if (!readAt(begin, header.data(), header.size())) {
        error = SaveError::Truncated;
        return ChunkLoadStatus::Corrupt;
    }

    bytes::Reader       reader{std::span<const std::byte>{header}};
    const std::uint32_t magic     = reader.u32();
    const std::uint32_t bodyBytes = reader.u32();
    const std::uint32_t storedCrc = reader.u32();
    const std::uint8_t  storedLod = reader.u8();

    if (magic != kChunkPayloadMagic) {
        error = SaveError::BadMagic;
        return ChunkLoadStatus::Corrupt;
    }
    if (bodyBytes == 0) {
        error = SaveError::EmptyPayload;
        return ChunkLoadStatus::Corrupt;
    }
    if (bodyBytes > kMaxChunkPayloadBytes ||
        static_cast<std::uint64_t>(bodyBytes) + kChunkPayloadHeaderBytes > span) {
        error = SaveError::Truncated;
        return ChunkLoadStatus::Corrupt;
    }
    if (storedLod >= kLodCount || storedLod != entry.lod) {
        // The table and the payload disagree about what was captured, so one of
        // them is damaged and neither can be trusted.
        error = SaveError::MalformedPayload;
        return ChunkLoadStatus::Corrupt;
    }

    payload.resize(bodyBytes);
    if (!readAt(begin + kChunkPayloadHeaderBytes, payload.data(), payload.size())) {
        payload.clear();
        error = SaveError::Truncated;
        return ChunkLoadStatus::Corrupt;
    }

    if (crc32(std::span<const std::byte>{payload}) != storedCrc) {
        payload.clear();
        error = SaveError::BadChecksum;
        return ChunkLoadStatus::Corrupt;
    }

    lod = static_cast<LodLevel>(storedLod);
    return ChunkLoadStatus::Loaded;
}

// ------------------------------------------------------------------ write --

bool RegionFile::write(const ChunkPos& position, std::span<const std::byte> payload, LodLevel lod,
                       SaveError& error)
{
    error = SaveError::None;

    if (position.y < 0 || position.y >= kWorldSectionCount) {
        error = SaveError::OffsetOutOfRange;
        return false;
    }
    if (payload.empty()) {
        error = SaveError::EmptyPayload;
        return false;
    }
    if (payload.size() > kMaxChunkPayloadBytes) {
        error = SaveError::MalformedPayload;
        VOXL_LOG_ERROR("chunk payload of {} bytes exceeds the {} byte ceiling", payload.size(),
                       kMaxChunkPayloadBytes);
        return false;
    }
    if (lod >= kLodCount) {
        error = SaveError::LodMismatch;
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_readOnly) {
        error = SaveError::ReadOnly;
        return false;
    }
    if (!m_healthy && !createFresh()) {
        error = SaveError::Io;
        return false;
    }

    const std::size_t recordBytes = kChunkPayloadHeaderBytes + payload.size();
    const std::size_t sectorSpan  = (recordBytes + kRegionSectorSize - 1) / kRegionSectorSize;
    if (sectorSpan > 0xFFFFu) {
        error = SaveError::MalformedPayload;
        return false;
    }
    const auto sectorCount = static_cast<std::uint16_t>(sectorSpan);

    const std::uint32_t sector = reserveSectors(sectorCount);
    const std::uint64_t offset = sectorToOffset(sector);

    // The whole record is assembled in memory and written once. Two writes would
    // give a crash a window in which the header describes a body that is not
    // there yet - which the CRC would catch, but only after the commit had
    // already thrown away the previous payload's location.
    std::vector<std::byte> record;
    record.reserve(sectorCount * kRegionSectorSize);
    bytes::putU32(record, kChunkPayloadMagic);
    bytes::putU32(record, static_cast<std::uint32_t>(payload.size()));
    bytes::putU32(record, crc32(payload));
    bytes::putU8(record, static_cast<std::uint8_t>(lod));
    bytes::putU8(record, 0u);
    bytes::putU16(record, 0u);
    record.insert(record.end(), payload.begin(), payload.end());
    record.resize(static_cast<std::size_t>(sectorCount) * kRegionSectorSize, std::byte{0});

    if (!writeAt(offset, record.data(), record.size()) || !m_stream.flush()) {
        releaseSectors(sector, sectorCount);
        error = SaveError::Io;
        VOXL_LOG_ERROR("region '{}': failed to write chunk payload at sector {}",
                       m_path.filename().string(), sector);
        return false;
    }

    // COMMIT. Up to this line the file on disk still describes the previous
    // state completely; from the line after it, the new one.
    const std::size_t entryIndex = regionEntryIndex(position);
    const Entry       previous   = m_table[entryIndex];

    Entry updated;
    updated.sector      = sector;
    updated.sectorCount = sectorCount;
    updated.lod         = static_cast<std::uint8_t>(lod);
    updated.flags       = 0u;
    // The allocator just handed this span out, so it can vouch for it. Only the
    // four fields above reach the disk; the ownership record is in-memory state
    // that exists to answer exactly one question, three lines below.
    updated.ownedSector = sector;
    updated.ownedCount  = sectorCount;

    std::array<std::byte, kRegionEntryBytes> encoded{};
    encodeEntry(updated, encoded.data());
    if (!writeAt(regionEntryFileOffset(entryIndex), encoded.data(), encoded.size()) ||
        !m_stream.flush()) {
        releaseSectors(sector, sectorCount);
        error = SaveError::Io;
        VOXL_LOG_ERROR("region '{}': failed to commit table entry {}", m_path.filename().string(),
                       entryIndex);
        return false;
    }

    m_table[entryIndex] = updated;

    // Only now may the old sectors be handed back: before the commit they were
    // the only valid copy of this chunk.
    //
    // AND ONLY THE SECTORS THE ALLOCATOR ITSELF HANDED OUT FOR THIS ENTRY. The
    // asymmetry here is deliberate and is the whole safety argument:
    //
    //   - A LEAKED sector costs 4 KB of disk until the next open, which rebuilds
    //     the map from the table and reclaims it. Nothing is lost.
    //   - A WRONGLY REUSED sector silently destroys a chunk the player never
    //     touched, and the destruction is invisible: the payload that lands on
    //     top carries its own valid magic and CRC, so the victim's next read
    //     SUCCEEDS and returns somebody else's blocks.
    //
    // So when the two disagree, leak. rebuildSectorMap refuses to vouch for an
    // entry whose span is implausible or is contested by a second entry, and an
    // entry it did not vouch for may well be pointing straight into a healthy
    // neighbour's payload. Checking the recorded span against the table entry
    // also catches the case where the entry was vouched for at a different span
    // than the one it now claims.
    if (ownsItsSectors(previous)) {
        releaseSectors(previous.ownedSector, previous.ownedCount);
    } else if (previous.sector != 0 || previous.sectorCount != 0) {
        VOXL_LOG_WARN("region '{}': entry {} claimed sectors [{}, {}) that the allocator does not "
                      "vouch for; leaking them rather than risking another chunk's payload",
                      m_path.filename().string(), entryIndex, previous.sector,
                      static_cast<std::uint64_t>(previous.sector) + previous.sectorCount);
    }
    return true;
}

bool RegionFile::flush()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_stream.is_open()) {
        return true;
    }
    m_stream.clear();
    return static_cast<bool>(m_stream.flush());
}

std::size_t RegionFile::storedChunkCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::size_t                 count = 0;
    for (const Entry& entry : m_table) {
        count += (entry.sector != 0 && entry.sectorCount != 0) ? 1u : 0u;
    }
    return count;
}

std::size_t RegionFile::reservedSectorCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::size_t                 count = 0;
    for (std::size_t i = kRegionFirstDataSector; i < m_sectorUsed.size(); ++i) {
        count += m_sectorUsed[i] != 0u ? 1u : 0u;
    }
    return count;
}

std::size_t RegionFile::leakedSectorCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // Paint every sector a trusted entry owns, then count the reserved sectors
    // nobody painted. Those are the ones no future write may release - see the
    // asymmetry note in write().
    std::vector<std::uint8_t> owned(m_sectorUsed.size(), 0u);
    for (const Entry& entry : m_table) {
        if (!ownsItsSectors(entry)) {
            continue;
        }
        const std::size_t end =
            std::min<std::size_t>(static_cast<std::size_t>(entry.ownedSector) + entry.ownedCount,
                                  owned.size());
        for (std::size_t i = entry.ownedSector; i < end; ++i) {
            owned[i] = 1u;
        }
    }

    std::size_t count = 0;
    for (std::size_t i = kRegionFirstDataSector; i < m_sectorUsed.size(); ++i) {
        count += (m_sectorUsed[i] != 0u && owned[i] == 0u) ? 1u : 0u;
    }
    return count;
}

bool RegionFile::isSectorReserved(std::uint32_t sector) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return sector < m_sectorUsed.size() && m_sectorUsed[sector] != 0u;
}

}  // namespace voxl
