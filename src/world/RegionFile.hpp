#pragma once

// Sector-addressed container holding the chunk payloads of one 16x16 column
// region, plus the little-endian byte codecs and the CRC the whole save format
// is built on.
//
// WHY REGIONS AND NOT ONE FILE PER CHUNK
// --------------------------------------
// A radius-20 world is ~1700 columns, i.e. ~13500 chunk sections. One file per
// section is 13500 directory entries the OS has to stat, open and close, each
// costing far more than the few hundred bytes most payloads actually contain,
// and NTFS rounds every one of them up to a cluster. A region groups 16x16
// columns (2048 sections) behind a single handle and a single offset table, so
// a save directory for a large world is tens of files rather than hundreds of
// thousands.
//
// FILE LAYOUT
// -----------
//   sector 0        64-byte header, then the offset table (2048 x 8 bytes),
//   ..sector 4      zero padding to the first data sector
//   sector 5..      chunk payload records, each an integral number of sectors
//
// A table entry is {uint32 sector, uint16 sectorCount, uint8 lod, uint8 flags}.
// `sector == 0` means the chunk has never been written; a real payload always
// starts at or after kRegionFirstDataSector, so zero is unambiguous.
//
// A payload record is a 16-byte header (magic, body length, CRC-32 of the body,
// the LOD it was captured at) followed by the body and zero padding out to the
// sector boundary.
//
// CRASH SAFETY: ALLOCATE-THEN-COMMIT, NOT TEMP-AND-RENAME
// -------------------------------------------------------
// Writing a chunk allocates sectors that no table entry currently references,
// writes the record there, flushes, and only then rewrites that chunk's 8-byte
// table entry. The previous payload's sectors are left untouched until after the
// entry lands, so a crash at any point before the commit leaves the old table
// pointing at the old, still-valid bytes; the freshly written sectors are merely
// orphaned and are reclaimed by the next open, which rebuilds the free map from
// the table.
//
// The alternative - write a whole new region file and rename it over the old one
// - was rejected because a region is up to a few megabytes and an autosave
// typically dirties a single chunk in it: temp-and-rename would turn every
// incremental save into a full rewrite of everything around the player. It is
// also not obviously safer. Neither scheme gets a hardware write barrier from
// std::fstream, so the honest guarantee is the same in both cases and comes from
// the per-payload magic + length + CRC: if a crash lands the table entry but not
// the bytes it points at, validation fails on the next read and THAT ONE CHUNK
// regenerates from the seed. Per-entry commit bounds that blast radius to the
// chunk being written; a torn whole-file rename would lose the newest state of
// every chunk in the region at once.
//
// SECTOR OWNERSHIP: LEAK, NEVER REUSE
// ------------------------------------
// The offset table is the only record of where a payload lives, and it is on
// disk, which means it can be wrong. The allocator therefore keeps its OWN,
// in-memory note of the span it handed each entry (Entry::ownedSector /
// ownedCount) and will only release a span it can match against the table entry
// being replaced. Two shapes of table damage make an entry unownable:
//
//   * IMPLAUSIBLE - it points inside the header/table, or past the end of the
//     file. The map never reserves those sectors, so there is nothing to free.
//   * CONTESTED - a second entry claims sectors that overlap it. The map
//     reserves the span so nobody allocates into it, but cannot tell which of
//     the claimants holds the real payload, so it vouches for neither.
//
// Replacing an unownable entry LEAKS its sectors. That asymmetry is deliberate:
// a leaked sector costs 4 KB until the next open rebuilds the map from the
// table and reclaims it, whereas a wrongly reused sector silently overwrites a
// chunk the player never touched - and silently is the operative word, because
// the payload that lands on top brings its own valid magic and CRC, so the
// victim's next read SUCCEEDS and returns someone else's blocks.
//
// THREADING
// ---------
// Every public method takes the file's own mutex, so concurrent saves of
// different chunks in the same region are safe and simply serialise on the
// handle. Different regions are independent objects and proceed in parallel.

#include "world/Lod.hpp"
#include "world/VoxelTypes.hpp"

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <vector>

namespace voxl {

// ------------------------------------------------------------------ errors --

/// Why a save-format operation failed. Every value except `None` results in the
/// affected chunk being regenerated from the seed rather than loaded.
enum class SaveError : std::uint8_t {
    None = 0,
    /// The file could not be opened, read or written at the OS level.
    Io = 1,
    /// The file does not start with the expected magic number.
    BadMagic = 2,
    /// Written by a build whose format this one cannot parse. Never overwritten.
    UnsupportedVersion = 3,
    /// The file ends before a structure it declares does.
    Truncated = 4,
    /// The stored CRC does not match the bytes.
    BadChecksum = 5,
    /// A table entry points outside the file.
    OffsetOutOfRange = 6,
    /// A table entry or payload header declares a length of zero.
    EmptyPayload = 7,
    /// The bytes parse structurally but describe something impossible - an index
    /// past the end of the palette, a sub-voxel grid that is empty or full.
    MalformedPayload = 8,
    /// The payload was captured at a different level of detail than the chunk
    /// asking for it. Only level 0 is ever written.
    LodMismatch = 9,
    /// The region belongs to a different world seed.
    SeedMismatch = 10,
    /// The file is intact but this build refuses to modify it.
    ReadOnly = 11,
};

[[nodiscard]] const char* toString(SaveError error) noexcept;

/// Outcome of asking the save for one chunk.
enum class ChunkLoadStatus : std::uint8_t {
    /// The chunk was populated from disk.
    Loaded = 0,
    /// Nothing has ever been saved here. Generate from the seed.
    Absent = 1,
    /// Something was saved here and is unusable. Generate from the seed; the
    /// accompanying SaveError says what was wrong.
    Corrupt = 2,
};

[[nodiscard]] const char* toString(ChunkLoadStatus status) noexcept;

// -------------------------------------------------------------------- crc --

/// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320).
///
/// Hand-rolled rather than pulled in from zlib because the project may not add
/// dependencies, and because a checksum whose exact bit definition is visible in
/// the repository is one fewer thing that can silently change under a save
/// format. A table-driven byte-at-a-time loop runs at ~1 GB/s, which is far
/// faster than the disk write it protects.
[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> data) noexcept;

// ------------------------------------------------------------- byte codecs --

/// Explicit little-endian readers and writers.
///
/// The save format is byte-defined, not struct-defined. Writing raw structs
/// would bake in the compiler's padding and the host's endianness; doing it a
/// field at a time costs nothing measurable next to the I/O and makes the format
/// exactly what the comments say it is.
namespace bytes {

inline void putU8(std::vector<std::byte>& out, std::uint8_t value)
{
    out.push_back(static_cast<std::byte>(value));
}

inline void putU16(std::vector<std::byte>& out, std::uint16_t value)
{
    putU8(out, static_cast<std::uint8_t>(value & 0xFFu));
    putU8(out, static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

inline void putU32(std::vector<std::byte>& out, std::uint32_t value)
{
    putU16(out, static_cast<std::uint16_t>(value & 0xFFFFu));
    putU16(out, static_cast<std::uint16_t>((value >> 16) & 0xFFFFu));
}

inline void putU64(std::vector<std::byte>& out, std::uint64_t value)
{
    putU32(out, static_cast<std::uint32_t>(value & 0xFFFFFFFFull));
    putU32(out, static_cast<std::uint32_t>((value >> 32) & 0xFFFFFFFFull));
}

inline void putI32(std::vector<std::byte>& out, std::int32_t value)
{
    putU32(out, static_cast<std::uint32_t>(value));
}

inline void putF32(std::vector<std::byte>& out, float value)
{
    putU32(out, std::bit_cast<std::uint32_t>(value));
}

inline void putF64(std::vector<std::byte>& out, double value)
{
    putU64(out, std::bit_cast<std::uint64_t>(value));
}

/// Bounds-checked sequential reader with a sticky failure flag.
///
/// Every getter returns zero and sets `ok()` false once the buffer runs out, so
/// a truncated payload cannot be distinguished from a valid one only by luck:
/// the decoder does its arithmetic on zeroes, then checks `ok()` once at the end
/// and rejects the whole thing. That is deliberately different from checking
/// every read - it keeps the decoder readable, and no partial state is ever
/// committed to a chunk anyway.
class Reader {
public:
    explicit Reader(std::span<const std::byte> data) noexcept : m_data(data) {}

    [[nodiscard]] bool        ok() const noexcept { return m_ok; }
    [[nodiscard]] std::size_t offset() const noexcept { return m_offset; }
    [[nodiscard]] std::size_t remaining() const noexcept
    {
        return m_ok ? m_data.size() - m_offset : 0;
    }
    [[nodiscard]] bool exhausted() const noexcept { return m_ok && m_offset == m_data.size(); }

    /// Marks the stream failed without consuming anything. Used by the decoder
    /// to fold a semantic rejection into the same `ok()` test as a truncation.
    void fail() noexcept { m_ok = false; }

    [[nodiscard]] std::uint8_t u8() noexcept
    {
        if (!take(1)) {
            return 0;
        }
        return static_cast<std::uint8_t>(m_data[m_offset - 1]);
    }

    [[nodiscard]] std::uint16_t u16() noexcept
    {
        const std::uint16_t low  = u8();
        const std::uint16_t high = u8();
        return static_cast<std::uint16_t>(low | static_cast<std::uint16_t>(high << 8));
    }

    [[nodiscard]] std::uint32_t u32() noexcept
    {
        const std::uint32_t low  = u16();
        const std::uint32_t high = u16();
        return low | (high << 16);
    }

    [[nodiscard]] std::uint64_t u64() noexcept
    {
        const std::uint64_t low  = u32();
        const std::uint64_t high = u32();
        return low | (high << 32);
    }

    [[nodiscard]] std::int32_t i32() noexcept { return static_cast<std::int32_t>(u32()); }
    [[nodiscard]] float        f32() noexcept { return std::bit_cast<float>(u32()); }
    [[nodiscard]] double       f64() noexcept { return std::bit_cast<double>(u64()); }

    /// Copies `count` raw bytes out. Returns false (and copies nothing) when the
    /// buffer is short, which also latches the failure flag.
    bool copy(void* destination, std::size_t count) noexcept
    {
        if (!take(count)) {
            return false;
        }
        std::memcpy(destination, m_data.data() + (m_offset - count), count);
        return true;
    }

private:
    /// Advances by `count`, latching failure if that would run past the end.
    bool take(std::size_t count) noexcept
    {
        if (!m_ok || count > m_data.size() - m_offset) {
            m_ok = false;
            return false;
        }
        m_offset += count;
        return true;
    }

    std::span<const std::byte> m_data;
    std::size_t                m_offset = 0;
    bool                       m_ok     = true;
};

}  // namespace bytes

// ---------------------------------------------------------- format numbers --

/// "VOXR" as a little-endian uint32. Any file that does not start with this is
/// not one of ours and is never parsed further.
inline constexpr std::uint32_t kRegionMagic = 0x52584F56u;

/// "VXCP" as a little-endian uint32: the per-payload guard word.
inline constexpr std::uint32_t kChunkPayloadMagic = 0x50435856u;

/// Bumped whenever the byte layout of a region file or a chunk payload changes.
/// A file whose version is greater than this is refused, never guessed at.
inline constexpr std::uint16_t kSaveFormatVersion = 1;

/// Columns along one axis of a region: 16x16 columns x 8 sections = 2048 chunks
/// per file. Power of two so the split is a shift and a mask.
inline constexpr std::int32_t kRegionSizeColumns = 16;
inline constexpr std::int32_t kRegionColumnShift = 4;
inline constexpr std::int32_t kRegionColumnMask  = kRegionSizeColumns - 1;

inline constexpr std::size_t kRegionEntryCount =
    static_cast<std::size_t>(kRegionSizeColumns) * kRegionSizeColumns * kWorldSectionCount;

/// Allocation granularity. 4096 matches the NTFS cluster size, so a payload
/// never shares a cluster with another chunk's bytes and a partially written
/// record cannot corrupt its neighbour.
inline constexpr std::size_t kRegionSectorShift = 12;
inline constexpr std::size_t kRegionSectorSize  = std::size_t{1} << kRegionSectorShift;

inline constexpr std::size_t kRegionHeaderBytes = 64;

/// Bytes of the header the header checksum covers: magic, version, geometry,
/// coordinates and seed. The trailing reserved bytes are deliberately excluded
/// so a later format can populate them without invalidating the checksum rule.
inline constexpr std::size_t kRegionHeaderCrcBytes = 28;

inline constexpr std::size_t kRegionEntryBytes = 8;
inline constexpr std::size_t kRegionTableBytes  = kRegionEntryCount * kRegionEntryBytes;

/// First sector a payload may occupy. Everything below it is header and table.
inline constexpr std::size_t kRegionFirstDataSector =
    (kRegionHeaderBytes + kRegionTableBytes + kRegionSectorSize - 1) / kRegionSectorSize;

inline constexpr std::size_t kChunkPayloadHeaderBytes = 16;

/// Hard ceiling on one chunk payload, enforced on both write and read.
///
/// The true worst case is a 16-bit palette (64 KB) plus its index words (64 KB)
/// plus raw light (32 KB) plus every one of the 32768 blocks being damaged
/// (2.1 MB), which is a little over 2.3 MB. Rounding up to 8 MB leaves room for
/// a future format to grow while still turning an absurd length field in a
/// corrupt file into a rejection instead of an allocation.
inline constexpr std::size_t kMaxChunkPayloadBytes = 8u * 1024u * 1024u;

// ---------------------------------------------------------- region indexing --

/// Which region file a chunk column lives in.
struct RegionCoord {
    std::int32_t x = 0;
    std::int32_t z = 0;

    friend constexpr bool operator==(const RegionCoord&, const RegionCoord&) noexcept = default;
};

/// Arithmetic shift, so negative columns floor to the region below rather than
/// folding onto region 0 the way integer division would.
[[nodiscard]] constexpr RegionCoord toRegionCoord(std::int32_t columnX, std::int32_t columnZ) noexcept
{
    return RegionCoord{columnX >> kRegionColumnShift, columnZ >> kRegionColumnShift};
}

[[nodiscard]] constexpr RegionCoord toRegionCoord(const ChunkPos& position) noexcept
{
    return toRegionCoord(position.x, position.z);
}

[[nodiscard]] constexpr RegionCoord toRegionCoord(const ColumnPos& position) noexcept
{
    return toRegionCoord(position.x, position.z);
}

/// Slot a chunk occupies in its region's offset table.
///
/// A column's eight sections are adjacent, because streaming loads and saves a
/// whole column and adjacent entries mean one page of the table, not eight.
/// Caller must have checked `position.y` is in [0, kWorldSectionCount).
[[nodiscard]] constexpr std::size_t regionEntryIndex(const ChunkPos& position) noexcept
{
    const std::int32_t localX = position.x & kRegionColumnMask;
    const std::int32_t localZ = position.z & kRegionColumnMask;
    return ((static_cast<std::size_t>(localZ) * kRegionSizeColumns) +
            static_cast<std::size_t>(localX)) *
               static_cast<std::size_t>(kWorldSectionCount) +
           static_cast<std::size_t>(position.y);
}

/// Byte offset of a table entry, for tests that need to corrupt one by hand.
[[nodiscard]] constexpr std::size_t regionEntryFileOffset(std::size_t entryIndex) noexcept
{
    return kRegionHeaderBytes + entryIndex * kRegionEntryBytes;
}

// ------------------------------------------------------------- RegionFile --

/// One region file on disk.
///
/// Construction never throws and never fails hard: if the file is missing it is
/// created lazily on the first write, and if it is damaged the object still
/// exists and reports the damage per read. A save system that refuses to run
/// because one file is bad is worse than one that regenerates a patch of
/// terrain.
class RegionFile {
public:
    /// Opens or adopts the region at (`regionX`, `regionZ`) inside `directory`.
    /// `seed` is written into a new file's header and checked against an
    /// existing one's.
    RegionFile(std::filesystem::path path, std::int32_t regionX, std::int32_t regionZ,
               std::uint64_t seed);

    RegionFile(const RegionFile&)            = delete;
    RegionFile& operator=(const RegionFile&) = delete;
    RegionFile(RegionFile&&)                 = delete;
    RegionFile& operator=(RegionFile&&)      = delete;

    [[nodiscard]] static std::filesystem::path pathFor(const std::filesystem::path& directory,
                                                       std::int32_t regionX, std::int32_t regionZ);

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }
    [[nodiscard]] RegionCoord coord() const noexcept { return RegionCoord{m_regionX, m_regionZ}; }

    /// Why the existing file could not be adopted, or `None`. A missing file is
    /// not an error - it reports `None` and `everWritten() == false`.
    [[nodiscard]] SaveError openError() const noexcept { return m_openError; }

    /// True when the file exists on disk and parsed. False both for "not created
    /// yet" and for "damaged"; use `openError()` to tell those apart.
    [[nodiscard]] bool healthy() const noexcept { return m_healthy; }

    /// False when this build must not modify the file - currently only when it
    /// was written by a newer format version or belongs to another seed.
    [[nodiscard]] bool writable() const noexcept { return !m_readOnly; }

    /// Reads one chunk's payload body.
    ///
    /// `payload` is cleared on every outcome other than `Loaded`, so a caller
    /// can never act on half a chunk. Any thread.
    ChunkLoadStatus read(const ChunkPos& position, std::vector<std::byte>& payload, LodLevel& lod,
                         SaveError& error);

    /// Writes one chunk's payload body, replacing whatever was there.
    ///
    /// Returns false and sets `error` on failure, having left the previous
    /// payload - if any - intact. Any thread.
    bool write(const ChunkPos& position, std::span<const std::byte> payload, LodLevel lod,
               SaveError& error);

    /// Pushes buffered bytes at the OS. Any thread.
    bool flush();

    /// Number of table entries currently pointing at a payload. Diagnostics.
    [[nodiscard]] std::size_t storedChunkCount() const;

    /// Data sectors the allocator currently considers occupied, excluding the
    /// header and offset table. Diagnostics.
    [[nodiscard]] std::size_t reservedSectorCount() const;

    /// Of those, the ones no table entry is allowed to hand back - see the
    /// ownership rule at the end of `write()`. Non-zero means a damaged table
    /// cost this session some disk space; it never means lost data, and the next
    /// open reclaims all of it. Diagnostics.
    [[nodiscard]] std::size_t leakedSectorCount() const;

    /// Whether `sector` is currently reserved. Lets a test prove that a sector
    /// was leaked rather than silently recycled. Diagnostics.
    [[nodiscard]] bool isSectorReserved(std::uint32_t sector) const;

    /// Monotonic counter bumped by every read and write, used by WorldSave's
    /// open-handle cache to pick a victim for eviction.
    [[nodiscard]] std::uint64_t lastUse() const noexcept
    {
        return m_lastUse.load(std::memory_order_relaxed);
    }
    void noteUse(std::uint64_t tick) noexcept
    {
        m_lastUse.store(tick, std::memory_order_relaxed);
    }

private:
    struct Entry {
        // ---- the eight bytes that live on disk --------------------------------
        std::uint32_t sector      = 0;
        std::uint16_t sectorCount = 0;
        std::uint8_t  lod         = 0;
        std::uint8_t  flags       = 0;

        // ---- in-memory only: the allocator's own record of what it handed out --
        //
        // THIS IS NOT THE SAME THING AS THE FIELDS ABOVE, and the difference is
        // the whole point. `sector`/`sectorCount` are whatever the file said;
        // `ownedSector`/`ownedCount` are what the allocator actually reserved for
        // this entry - set by reserveSectors() on a write, or by
        // rebuildSectorMap() on open but ONLY for an entry it is willing to vouch
        // for. `ownedCount == 0` means "the allocator does not vouch for this
        // entry", which is the state every implausible or contested entry lands
        // in and which makes releasing its sectors forbidden.
        std::uint32_t ownedSector = 0;
        std::uint16_t ownedCount  = 0;
    };

    /// True when the allocator vouches that `entry` exclusively owns exactly the
    /// span its table fields claim. Anything else - never vouched for, or vouched
    /// for at a span the table has since been made to disagree with - is not
    /// releasable.
    [[nodiscard]] static constexpr bool ownsItsSectors(const Entry& entry) noexcept
    {
        return entry.ownedCount != 0 && entry.ownedSector == entry.sector &&
               entry.ownedCount == entry.sectorCount;
    }

    /// Reads and validates the header and table of an existing file. Caller
    /// holds the mutex. Sets m_openError / m_readOnly / m_healthy.
    void adoptExisting(std::uint64_t seed);

    /// Creates a fresh, empty region file, moving a damaged one aside first.
    /// Caller holds the mutex. Returns false on an I/O failure.
    bool createFresh();

    /// Renames a damaged file to "<name>.corrupt" so a support request still has
    /// the evidence, then lets a fresh one take its place.
    ///
    /// Deliberately deferred until the first WRITE rather than done at open: a
    /// player who launches, sees a garbled region and quits should still have the
    /// original bytes, and reads report the corruption honestly in the meantime.
    void quarantineDamagedFile();

    bool readAt(std::uint64_t offset, void* destination, std::size_t count);
    bool writeAt(std::uint64_t offset, const void* source, std::size_t count);

    /// Sectors that no live table entry references. Reserving never overlaps a
    /// referenced payload, which is what makes the commit crash-safe.
    [[nodiscard]] std::uint32_t reserveSectors(std::uint16_t count);
    void                        releaseSectors(std::uint32_t sector, std::uint16_t count);
    void                        markSectorsUsed(std::uint32_t sector, std::uint16_t count);

    void rebuildSectorMap();

    static void encodeEntry(const Entry& entry, std::byte out[kRegionEntryBytes]) noexcept;
    static void encodeHeader(std::int32_t regionX, std::int32_t regionZ, std::uint64_t seed,
                             std::byte out[kRegionHeaderBytes]) noexcept;

    mutable std::mutex    m_mutex;
    std::filesystem::path m_path;
    std::int32_t          m_regionX = 0;
    std::int32_t          m_regionZ = 0;
    std::uint64_t         m_seed    = 0;

    std::fstream  m_stream;
    std::uint64_t m_fileBytes = 0;

    std::vector<Entry> m_table;
    /// One byte per sector: non-zero means occupied. A byte rather than a
    /// bitfield because the map is at most a few tens of kilobytes and the
    /// allocator's inner loop is easier to get right without bit twiddling.
    std::vector<std::uint8_t> m_sectorUsed;

    SaveError m_openError = SaveError::None;
    bool      m_healthy   = false;
    bool      m_readOnly  = false;
    /// Set when an existing file failed to parse; the first write quarantines it.
    bool m_quarantinePending = false;

    std::atomic<std::uint64_t> m_lastUse{0};
};

}  // namespace voxl

namespace std {

template <>
struct hash<voxl::RegionCoord> {
    [[nodiscard]] size_t operator()(const voxl::RegionCoord& coord) const noexcept
    {
        uint64_t h = static_cast<uint64_t>(static_cast<uint32_t>(coord.x)) * 0x9E3779B97F4A7C15ull;
        h ^= static_cast<uint64_t>(static_cast<uint32_t>(coord.z)) * 0xC2B2AE3D27D4EB4Full;
        h ^= h >> 31;
        h *= 0xD6E8FEB86659FD93ull;
        h ^= h >> 32;
        return static_cast<size_t>(h);
    }
};

}  // namespace std
