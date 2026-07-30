// World persistence: the chunk codec, the region container, and - above all -
// what happens when the bytes on disk are wrong.
//
// The round-trip tests here are the easy half. The half that earns its keep is
// everything below "corruption": each of those tests deliberately damages a save
// file in one specific way and asserts that the chunk REGENERATES rather than
// crashing, loading garbage, or - the failure mode that is worst because it is
// silent - loading as a plausible-looking chunk full of air. Persistence is the
// one subsystem whose bugs destroy the player's work permanently, so every field
// the decoder trusts gets a test that lies about it.

#include <catch2/catch_test_macros.hpp>

#include "core/JobSystem.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/ChunkStorage.hpp"
#include "world/Lod.hpp"
#include "world/RegionFile.hpp"
#include "world/SubVoxel.hpp"
#include "world/VoxelTypes.hpp"
#include "world/WorldSave.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using voxl::BlockId;
using voxl::Chunk;
using voxl::ChunkLoadStatus;
using voxl::ChunkPos;
using voxl::ChunkPtr;
using voxl::ChunkState;
using voxl::ChunkStorage;
using voxl::JobSystem;
using voxl::kChunkVolume;
using voxl::kLodFull;
using voxl::kSubVoxelCount;
using voxl::kSubVoxelWords;
using voxl::RegionHeaderInfo;
using voxl::SaveError;
using voxl::SeedSource;
using voxl::SubVoxelGrid;
using voxl::WorldMetadata;
using voxl::WorldSave;
using voxl::WorldSeedResolution;

namespace {

constexpr std::uint64_t kSeed = 0xC0FFEEull;

// ------------------------------------------------------------- test rig --

/// Deterministic scatter. The suite carries its own generator so a failure
/// reproduces byte for byte; see the same rule in tests/test_subvoxel.cpp.
class Lcg {
public:
    explicit constexpr Lcg(std::uint32_t seed) noexcept : m_state(seed) {}

    std::uint32_t next() noexcept
    {
        m_state = m_state * 1664525u + 1013904223u;
        return m_state;
    }

    [[nodiscard]] std::uint32_t below(std::uint32_t bound) noexcept { return next() % bound; }

private:
    std::uint32_t m_state;
};

/// Scratch directory that removes itself. Named from the clock as well as a
/// counter because catch_discover_tests may run each case in its own process,
/// where a static counter alone would collide.
class TempDir {
public:
    explicit TempDir(std::string label)
    {
        static std::atomic<std::uint32_t> counter{0};
        const auto stamp = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());

        m_path = std::filesystem::temp_directory_path() /
                 ("voxl_save_" + label + "_" + std::to_string(stamp) + "_" +
                  std::to_string(counter.fetch_add(1)));

        std::error_code code;
        std::filesystem::remove_all(m_path, code);
        std::filesystem::create_directories(m_path, code);
    }

    ~TempDir()
    {
        std::error_code code;
        std::filesystem::remove_all(m_path, code);
    }

    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

private:
    std::filesystem::path m_path;
};

[[nodiscard]] std::vector<std::byte> readWholeFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.is_open());
    stream.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(stream.tellg());
    stream.seekg(0, std::ios::beg);

    std::vector<std::byte> data(size);
    if (size != 0) {
        stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    }
    return data;
}

void patchBytes(const std::filesystem::path& path, std::uint64_t offset,
                std::span<const std::byte> data)
{
    std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
    REQUIRE(stream.is_open());
    stream.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    stream.write(reinterpret_cast<const char*>(data.data()),
                 static_cast<std::streamsize>(data.size()));
    REQUIRE(static_cast<bool>(stream));
}

void patchU32(const std::filesystem::path& path, std::uint64_t offset, std::uint32_t value)
{
    std::vector<std::byte> encoded;
    voxl::bytes::putU32(encoded, value);
    patchBytes(path, offset, encoded);
}

void patchU16(const std::filesystem::path& path, std::uint64_t offset, std::uint16_t value)
{
    std::vector<std::byte> encoded;
    voxl::bytes::putU16(encoded, value);
    patchBytes(path, offset, encoded);
}

/// The one table entry belonging to `position`, decoded from the raw file.
struct RawEntry {
    std::uint32_t sector      = 0;
    std::uint16_t sectorCount = 0;
    std::uint8_t  lod         = 0;
};

[[nodiscard]] RawEntry readRawEntry(const std::filesystem::path& path, const ChunkPos& position)
{
    const std::vector<std::byte> file = readWholeFile(path);
    const std::size_t offset = voxl::regionEntryFileOffset(voxl::regionEntryIndex(position));
    REQUIRE(file.size() >= offset + voxl::kRegionEntryBytes);

    voxl::bytes::Reader reader{std::span<const std::byte>{file}.subspan(offset)};
    RawEntry            entry;
    entry.sector      = reader.u32();
    entry.sectorCount = reader.u16();
    entry.lod         = reader.u8();
    return entry;
}

[[nodiscard]] std::filesystem::path regionPathOf(const std::filesystem::path& directory,
                                                 const ChunkPos&              position)
{
    const voxl::RegionCoord coord = voxl::toRegionCoord(position);
    return voxl::RegionFile::pathFor(directory, coord.x, coord.z);
}

// ---------------------------------------------------------- chunk fixtures --

/// A chunk with a deliberately awkward palette: enough distinct ids to force
/// ChunkStorage past several index widths, scattered so no run is uniform.
[[nodiscard]] ChunkPtr makeComplexChunk(const ChunkPos& position, std::uint32_t seed = 7u)
{
    static constexpr BlockId kMaterials[] = {
        voxl::blocks::Stone,  voxl::blocks::Dirt,      voxl::blocks::Grass,
        voxl::blocks::Sand,   voxl::blocks::Gravel,    voxl::blocks::Water,
        voxl::blocks::Wood,   voxl::blocks::Leaves,    voxl::blocks::Planks,
        voxl::blocks::Glass,  voxl::blocks::Snow,      voxl::blocks::Sandstone,
        voxl::blocks::Cobblestone, voxl::blocks::Bedrock, voxl::blocks::Glowstone,
        voxl::blocks::Clay,   voxl::blocks::Ice,       voxl::blocks::Air,
    };
    constexpr std::size_t kMaterialCount = sizeof(kMaterials) / sizeof(kMaterials[0]);

    ChunkPtr      chunk   = Chunk::create(position);
    ChunkStorage& storage = chunk->storage();

    Lcg rng{seed};
    for (std::size_t i = 0; i < kChunkVolume; ++i) {
        storage.set(i, kMaterials[rng.below(static_cast<std::uint32_t>(kMaterialCount))]);
    }

    // Light that varies with height only: long runs, so this exercises the
    // run-length branch of the light encoder.
    for (std::int32_t y = 0; y < voxl::kChunkSize; ++y) {
        const auto sun   = static_cast<std::uint8_t>(y / 2 % 16);
        const auto block = static_cast<std::uint8_t>((31 - y) / 2 % 16);
        for (std::int32_t z = 0; z < voxl::kChunkSize; ++z) {
            for (std::int32_t x = 0; x < voxl::kChunkSize; ++x) {
                storage.setLight(voxl::localIndex(x, y, z), ChunkStorage::packLight(sun, block));
            }
        }
    }

    chunk->forceState(ChunkState::Ready);
    chunk->markModified();
    return chunk;
}

/// Counts every disagreement between two chunks. Returned as a count rather than
/// asserted per voxel because 32768 Catch2 assertions per comparison dominates
/// the suite's runtime and buries the one line that matters.
struct Mismatch {
    std::size_t voxels = 0;
    std::size_t light  = 0;
    std::size_t firstVoxelIndex = kChunkVolume;
    std::size_t firstLightIndex = kChunkVolume;
};

[[nodiscard]] Mismatch compareContents(const Chunk& lhs, const Chunk& rhs)
{
    Mismatch result;
    for (std::size_t i = 0; i < kChunkVolume; ++i) {
        if (lhs.getBlock(i) != rhs.getBlock(i)) {
            if (result.voxels == 0) {
                result.firstVoxelIndex = i;
            }
            ++result.voxels;
        }
        if (lhs.getLight(i) != rhs.getLight(i)) {
            if (result.light == 0) {
                result.firstLightIndex = i;
            }
            ++result.light;
        }
    }
    return result;
}

void requireDamageMatches(const Chunk& lhs, const Chunk& rhs)
{
    const auto left  = lhs.subVoxels().sortedEntries();
    const auto right = rhs.subVoxels().sortedEntries();
    REQUIRE(left.size() == right.size());

    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (left[i].first != right[i].first ||
            left[i].second.material != right[i].second.material ||
            left[i].second.bits != right[i].second.bits) {
            ++mismatches;
        }
    }
    REQUIRE(mismatches == 0);
}

/// Saves one chunk and reads it back through a freshly opened WorldSave, which
/// is what a real relaunch does.
[[nodiscard]] ChunkPtr roundTrip(JobSystem& jobs, const std::filesystem::path& directory,
                                 const ChunkPtr& source)
{
    {
        WorldSave save(jobs, directory, kSeed);
        REQUIRE(save.saveChunk(source));
        save.flush();
        REQUIRE(save.stats().chunksWritten == 1);
        REQUIRE(save.stats().writeFailures == 0);
    }

    WorldSave      save(jobs, directory, kSeed);
    ChunkPtr       restored = Chunk::create(source->position());
    const auto     result   = save.loadChunk(*restored);
    REQUIRE(result.status == ChunkLoadStatus::Loaded);
    REQUIRE(result.error == SaveError::None);
    REQUIRE_FALSE(result.regenerate());
    return restored;
}

// ------------------------------------------------------- payload builder --

/// Assembles a chunk payload body field by field so a test can make exactly one
/// field wrong. Mirrors the layout documented at the top of world/WorldSave.cpp.
struct PayloadBuilder {
    struct Damage {
        std::uint16_t blockIndex = 0;
        BlockId       material   = voxl::blocks::Stone;
        SubVoxelGrid  grid{};
    };

    std::uint8_t               bits = 0;
    std::vector<BlockId>       palette{voxl::blocks::Stone};
    std::vector<std::uint64_t> words;
    std::uint8_t               lightEncoding = 0;
    std::uint8_t               uniformLight  = 0;
    std::vector<std::uint8_t>  rawLight;
    std::vector<Damage>        damage;
    /// Appended after the last field, to prove the decoder rejects a payload it
    /// does not fully consume.
    std::size_t trailingBytes = 0;
    /// Written instead of palette.size(), for the "lies about its own length"
    /// cases.
    bool overridePaletteCount = false;
    std::uint32_t paletteCountOverride = 0;

    [[nodiscard]] std::vector<std::byte> build() const
    {
        std::vector<std::byte> out;
        voxl::bytes::putU8(out, bits);
        voxl::bytes::putU32(out, overridePaletteCount
                                     ? paletteCountOverride
                                     : static_cast<std::uint32_t>(palette.size()));
        for (const BlockId id : palette) {
            voxl::bytes::putU16(out, id);
        }
        for (const std::uint64_t word : words) {
            voxl::bytes::putU64(out, word);
        }

        voxl::bytes::putU8(out, lightEncoding);
        if (lightEncoding == 0) {
            voxl::bytes::putU8(out, uniformLight);
        } else if (lightEncoding == 1) {
            for (const std::uint8_t value : rawLight) {
                voxl::bytes::putU8(out, value);
            }
        }

        voxl::bytes::putU32(out, static_cast<std::uint32_t>(damage.size()));
        for (const Damage& entry : damage) {
            voxl::bytes::putU16(out, entry.blockIndex);
            voxl::bytes::putU16(out, entry.material);
            for (const std::uint64_t word : entry.grid.bits) {
                voxl::bytes::putU64(out, word);
            }
        }

        for (std::size_t i = 0; i < trailingBytes; ++i) {
            voxl::bytes::putU8(out, 0xAAu);
        }
        return out;
    }
};

/// A two-bit paletted body whose voxels all point at palette slot 0, ready to be
/// sabotaged. Two bits is the narrowest width at which an index can address a
/// slot that does not exist.
[[nodiscard]] PayloadBuilder twoBitBody()
{
    PayloadBuilder builder;
    builder.bits    = 2;
    builder.palette = {voxl::blocks::Stone, voxl::blocks::Dirt, voxl::blocks::Grass};
    builder.words.assign(ChunkStorage::wordCountFor(2), 0ull);
    return builder;
}

/// A complex chunk that also carries sub-voxel damage, so a test can prove that
/// BOTH halves of a chunk's state survive a rejected payload.
[[nodiscard]] ChunkPtr makeDamagedChunk(const ChunkPos& position, std::uint32_t seed)
{
    ChunkPtr chunk = makeComplexChunk(position, seed);
    // makeComplexChunk scatters air through the section, so pick blocks that are
    // definitely solid before carving them.
    Lcg rng{seed ^ 0x5A5A5A5Au};
    for (int placed = 0; placed < 6;) {
        const std::size_t index = rng.below(static_cast<std::uint32_t>(kChunkVolume));
        if (chunk->getBlock(index) == voxl::blocks::Air) {
            continue;
        }
        if (chunk->breakSubVoxel(index, rng.below(static_cast<std::uint32_t>(kSubVoxelCount))) !=
            voxl::SubVoxelEdit::Unchanged) {
            ++placed;
        }
    }
    return chunk;
}

/// Asserts that decoding `payload` fails with `expected` AND leaves the chunk
/// byte for byte as it was. "Never a silently zeroed chunk" is the property.
void requireRejectedAndUntouched(const std::vector<std::byte>& payload, SaveError expected)
{
    ChunkPtr victim  = makeDamagedChunk(ChunkPos{3, 1, 4}, 99u);
    ChunkPtr witness = makeDamagedChunk(ChunkPos{3, 1, 4}, 99u);
    REQUIRE(victim->hasSubVoxelDamage());

    const auto result = WorldSave::decodeChunk(std::span<const std::byte>{payload}, *victim);
    REQUIRE(result.status == ChunkLoadStatus::Corrupt);
    REQUIRE(result.error == expected);
    REQUIRE(result.regenerate());

    const Mismatch diff = compareContents(*victim, *witness);
    REQUIRE(diff.voxels == 0);
    REQUIRE(diff.light == 0);
    requireDamageMatches(*victim, *witness);
}

// ------------------------------------------------------------ LOD guards --

/// The test the streamer used to apply before it would change a chunk's level:
/// `ChunkManager::lodTargetFor` refused only while `needsSave()` was set.
[[nodiscard]] bool oldLodGuard(const ChunkPtr& chunk)
{
    return chunk->needsSave();
}

/// What the guard becomes once the sticky divergence signal exists. The dirty
/// flag still counts - an edit made this frame is not on disk yet - but it is no
/// longer the only thing standing between a build and the terrain generator.
[[nodiscard]] bool newLodGuard(const WorldSave& save, const ChunkPtr& chunk)
{
    return chunk->needsSave() || save.hasStoredChunk(chunk->position());
}

}  // namespace

// ===========================================================================
//  Round trips
// ===========================================================================

TEST_CASE("a chunk with a complex palette round-trips every voxel and light value",
          "[persistence]")
{
    TempDir   dir{"complex"};
    JobSystem jobs{2};

    const ChunkPtr original = makeComplexChunk(ChunkPos{2, 3, 5});
    REQUIRE_FALSE(original->storage().isUniform());
    REQUIRE(original->storage().paletteSize() > 8);

    const ChunkPtr restored = roundTrip(jobs, dir.path(), original);

    const Mismatch diff = compareContents(*original, *restored);
    INFO("first differing voxel index " << diff.firstVoxelIndex);
    REQUIRE(diff.voxels == 0);
    INFO("first differing light index " << diff.firstLightIndex);
    REQUIRE(diff.light == 0);
}

TEST_CASE("light that defeats run-length coding still round-trips exactly", "[persistence]")
{
    TempDir   dir{"rawlight"};
    JobSystem jobs{2};

    ChunkPtr original = Chunk::create(ChunkPos{1, 2, 3});
    original->storage().fill(voxl::blocks::Stone);

    // A different value at nearly every voxel: the encoder's run builder gives
    // up and falls back to the raw array, which is the branch under test.
    Lcg rng{4242u};
    for (std::size_t i = 0; i < kChunkVolume; ++i) {
        original->storage().setLight(i, static_cast<std::uint8_t>(rng.below(256u)));
    }
    original->forceState(ChunkState::Ready);
    original->markModified();

    const ChunkPtr restored = roundTrip(jobs, dir.path(), original);

    const Mismatch diff = compareContents(*original, *restored);
    REQUIRE(diff.voxels == 0);
    REQUIRE(diff.light == 0);
}

TEST_CASE("a uniform chunk round-trips and stays uniform", "[persistence]")
{
    TempDir   dir{"uniform"};
    JobSystem jobs{2};

    ChunkPtr original = Chunk::create(ChunkPos{-4, 0, -9});
    original->storage().fill(voxl::blocks::Stone);
    original->storage().fillLight(0, 3);
    original->forceState(ChunkState::Ready);
    original->markModified();

    REQUIRE(original->storage().isUniform());

    const ChunkPtr restored = roundTrip(jobs, dir.path(), original);

    REQUIRE(restored->storage().isUniform());
    REQUIRE(restored->storage().uniformValue() == voxl::blocks::Stone);
    REQUIRE_FALSE(restored->storage().hasLightData());
    REQUIRE(restored->storage().uniformLight() == ChunkStorage::packLight(0, 3));

    const Mismatch diff = compareContents(*original, *restored);
    REQUIRE(diff.voxels == 0);
    REQUIRE(diff.light == 0);
}

TEST_CASE("an all-air chunk round-trips", "[persistence]")
{
    TempDir   dir{"air"};
    JobSystem jobs{2};

    ChunkPtr original = Chunk::create(ChunkPos{0, 7, 0});
    original->forceState(ChunkState::Ready);
    original->markModified();
    REQUIRE(original->isEmpty());

    const ChunkPtr restored = roundTrip(jobs, dir.path(), original);
    REQUIRE(restored->isEmpty());
}

TEST_CASE("sub-voxel damage round-trips, including a block with one sub-voxel left",
          "[persistence][subvoxel]")
{
    TempDir   dir{"damage"};
    JobSystem jobs{2};

    ChunkPtr original = Chunk::create(ChunkPos{1, 1, 1});
    original->storage().fill(voxl::blocks::Stone);
    original->storage().set(voxl::localIndex(4, 4, 4), voxl::blocks::Wood);
    original->storage().set(voxl::localIndex(9, 9, 9), voxl::blocks::Glowstone);

    // Lightly damaged.
    const std::size_t lightlyDamaged = voxl::localIndex(1, 1, 1);
    REQUIRE(original->breakSubVoxel(lightlyDamaged, 0) == voxl::SubVoxelEdit::Modified);
    REQUIRE(original->breakSubVoxel(lightlyDamaged, 5) == voxl::SubVoxelEdit::Modified);

    // Scattered damage on a non-stone block, so the material must survive too.
    const std::size_t woodBlock = voxl::localIndex(4, 4, 4);
    Lcg               rng{31337u};
    for (int i = 0; i < 40; ++i) {
        (void)original->breakSubVoxel(woodBlock,
                                      rng.below(static_cast<std::uint32_t>(kSubVoxelCount)));
    }

    // THE EDGE CASE: 511 of 512 sub-voxels gone. One more carve would turn the
    // block to air and erase the entry, so this is the sparsest state the store
    // is ever allowed to hold - and the one most likely to be mangled by an
    // encoder that assumes a grid is "mostly full".
    const std::size_t nearlyGone = voxl::localIndex(9, 9, 9);
    for (std::size_t sub = 1; sub < kSubVoxelCount; ++sub) {
        REQUIRE(original->breakSubVoxel(nearlyGone, sub) == voxl::SubVoxelEdit::Modified);
    }
    {
        const SubVoxelGrid* grid = original->subVoxels().find(nearlyGone);
        REQUIRE(grid != nullptr);
        REQUIRE(grid->count() == 1);
    }

    original->forceState(ChunkState::Ready);
    const std::size_t damagedBlocks = original->subVoxels().size();
    REQUIRE(damagedBlocks == 3);

    const ChunkPtr restored = roundTrip(jobs, dir.path(), original);

    const Mismatch diff = compareContents(*original, *restored);
    REQUIRE(diff.voxels == 0);
    REQUIRE(diff.light == 0);
    requireDamageMatches(*original, *restored);

    // The invariant, restated on the loaded chunk: an entry exists only for a
    // strictly partial block, and its material equals the block id.
    REQUIRE(restored->subVoxels().size() == damagedBlocks);
    restored->subVoxels().forEach([&](std::uint16_t blockIndex, const SubVoxelGrid& grid) {
        REQUIRE(grid.count() > 0);
        REQUIRE(grid.count() < kSubVoxelCount);
        REQUIRE(grid.material == restored->getBlock(blockIndex));
        REQUIRE(grid.material != voxl::blocks::Air);
    });

    const SubVoxelGrid* survivor = restored->subVoxels().find(nearlyGone);
    REQUIRE(survivor != nullptr);
    REQUIRE(survivor->count() == 1);
    REQUIRE(survivor->test(0));
    REQUIRE(restored->getBlock(nearlyGone) == voxl::blocks::Glowstone);
}

TEST_CASE("identical chunks encode to identical bytes", "[persistence]")
{
    // The whole point of SubVoxelStore::sortedEntries(). If the encoder ever
    // iterates the container directly, two worlds that are equal would produce
    // different files and no checksum comparison would mean anything.
    ChunkPtr left  = Chunk::create(ChunkPos{0, 0, 0});
    ChunkPtr right = Chunk::create(ChunkPos{9, 9, 9});

    for (Chunk* chunk : {left.get(), right.get()}) {
        chunk->storage().fill(voxl::blocks::Stone);
        // Damage the blocks in DIFFERENT orders; the file must not notice.
        const std::size_t indices[] = {voxl::localIndex(5, 5, 5), voxl::localIndex(1, 2, 3),
                                       voxl::localIndex(30, 30, 30)};
        if (chunk == left.get()) {
            for (const std::size_t index : indices) {
                (void)chunk->breakSubVoxel(index, 17);
            }
        } else {
            for (std::size_t i = 3; i-- > 0;) {
                (void)chunk->breakSubVoxel(indices[i], 17);
            }
        }
    }

    REQUIRE(WorldSave::encodeChunk(*left) == WorldSave::encodeChunk(*right));
}

// ===========================================================================
//  Level of detail
// ===========================================================================

TEST_CASE("only level-0 chunks are written, and a coarse chunk never loads one",
          "[persistence][lod]")
{
    TempDir   dir{"lod"};
    JobSystem jobs{2};
    WorldSave save(jobs, dir.path(), kSeed);

    const ChunkPos position{0, 2, 0};

    ChunkPtr coarse = makeComplexChunk(position);
    coarse->setLod(2);
    REQUIRE_FALSE(save.saveChunk(coarse));
    save.flush();
    REQUIRE(save.stats().chunksWritten == 0);

    // Now store the real, full-resolution chunk.
    ChunkPtr fine = makeComplexChunk(position);
    REQUIRE(save.saveChunk(fine));
    save.flush();
    REQUIRE(save.stats().chunksWritten == 1);

    // A chunk being generated at a coarser level must NOT be handed the level-0
    // payload: its voxels are meant to be a downsample, and restoring full
    // detail into it would make the mesher's resolution assumptions wrong.
    ChunkPtr coarseTarget = Chunk::create(position);
    coarseTarget->setLod(1);
    const auto coarseResult = save.loadChunk(*coarseTarget);
    REQUIRE(coarseResult.status == ChunkLoadStatus::Absent);
    REQUIRE(coarseResult.regenerate());
    REQUIRE(coarseTarget->isEmpty());

    ChunkPtr fineTarget = Chunk::create(position);
    REQUIRE(save.loadChunk(*fineTarget).status == ChunkLoadStatus::Loaded);
    REQUIRE(compareContents(*fine, *fineTarget).voxels == 0);
}

// ===========================================================================
//  Dirty-flag and retire-hook behaviour
// ===========================================================================

TEST_CASE("saving clears needsSave, and only dirty chunks are written", "[persistence]")
{
    TempDir   dir{"dirty"};
    JobSystem jobs{2};
    WorldSave save(jobs, dir.path(), kSeed);

    ChunkPtr dirty = makeComplexChunk(ChunkPos{1, 1, 1});
    ChunkPtr clean = makeComplexChunk(ChunkPos{2, 1, 1});
    clean->markSaved();

    REQUIRE(dirty->needsSave());
    REQUIRE_FALSE(clean->needsSave());

    const ChunkPtr batch[] = {dirty, clean};
    REQUIRE(save.saveChunks(std::span<const ChunkPtr>{batch}) == 1);
    save.flush();

    REQUIRE_FALSE(dirty->needsSave());
    REQUIRE(save.stats().chunksWritten == 1);
}

TEST_CASE("the retire hook saves a dirty chunk and skips a clean one", "[persistence]")
{
    TempDir   dir{"retire"};
    JobSystem jobs{2};
    WorldSave save(jobs, dir.path(), kSeed);

    const auto hook = save.retireHook();

    ChunkPtr edited = makeComplexChunk(ChunkPos{5, 4, 3});
    edited->forceState(ChunkState::Unloading);  // exactly how ChunkManager calls it
    hook(edited);

    ChunkPtr untouched = makeComplexChunk(ChunkPos{6, 4, 3});
    untouched->markSaved();
    untouched->forceState(ChunkState::Unloading);
    hook(untouched);

    save.flush();
    REQUIRE(save.stats().chunksWritten == 1);

    ChunkPtr restored = Chunk::create(ChunkPos{5, 4, 3});
    REQUIRE(save.loadChunk(*restored).status == ChunkLoadStatus::Loaded);
    REQUIRE(compareContents(*edited, *restored).voxels == 0);
}

TEST_CASE("a failed write puts the dirty flag back so the next autosave retries",
          "[persistence]")
{
    TempDir        dir{"retry"};
    JobSystem      jobs{2};
    const ChunkPos position{3, 3, 3};

    // A region this build refuses to modify is the cleanest way to make a write
    // fail without a filesystem trick.
    {
        WorldSave save(jobs, dir.path(), kSeed);
        REQUIRE(save.saveChunk(makeComplexChunk(position)));
        save.flush();
    }
    const std::filesystem::path region = regionPathOf(dir.path(), position);
    patchU16(region, 4u, static_cast<std::uint16_t>(voxl::kSaveFormatVersion + 1));
    {
        const std::vector<std::byte> file = readWholeFile(region);
        patchU32(region, voxl::kRegionHeaderCrcBytes,
                 voxl::crc32(std::span<const std::byte>{file}.subspan(
                     0, voxl::kRegionHeaderCrcBytes)));
    }

    WorldSave      save(jobs, dir.path(), kSeed);
    const ChunkPtr chunk = makeComplexChunk(position);
    REQUIRE(chunk->needsSave());

    REQUIRE(save.saveChunk(chunk));
    // Cleared optimistically, so a second autosave in the same window does not
    // queue the same bytes twice.
    REQUIRE_FALSE(chunk->needsSave());

    save.flush();
    REQUIRE(save.stats().writeFailures == 1);
    REQUIRE(save.stats().chunksWritten == 0);

    // The worker cannot touch the flag itself - markModified is main-thread - so
    // it posts a closure. Until that runs the chunk still looks clean.
    REQUIRE_FALSE(chunk->needsSave());
    REQUIRE(jobs.mainThreadQueue().drainAll() == 1);
    REQUIRE(chunk->needsSave());
}

TEST_CASE("loading replaces everything a chunk object already held", "[persistence]")
{
    TempDir        dir{"reuse"};
    JobSystem      jobs{2};
    const ChunkPos position{4, 4, 4};

    const ChunkPtr stored = makeDamagedChunk(position, 21u);
    WorldSave      save(jobs, dir.path(), kSeed);
    REQUIRE(save.saveChunk(stored));
    save.flush();

    // Decode over a chunk that already has unrelated voxels, light and damage.
    // Anything left behind would be a ghost of the previous contents.
    ChunkPtr target = makeDamagedChunk(position, 77u);
    REQUIRE(target->hasSubVoxelDamage());
    REQUIRE(compareContents(*stored, *target).voxels != 0);

    REQUIRE(save.loadChunk(*target).status == ChunkLoadStatus::Loaded);

    const Mismatch diff = compareContents(*stored, *target);
    REQUIRE(diff.voxels == 0);
    REQUIRE(diff.light == 0);
    requireDamageMatches(*stored, *target);

    // A freshly loaded chunk has voxels but no geometry, and matches disk.
    REQUIRE(target->needsRemesh());
    REQUIRE_FALSE(target->needsSave());
}

TEST_CASE("a chunk a worker is generating is never read by the saver", "[persistence]")
{
    TempDir   dir{"busy"};
    JobSystem jobs{2};
    WorldSave save(jobs, dir.path(), kSeed);

    // Reading voxels out from under a generating worker is the use-after-free
    // the engine's first invariant forbids, with the roles reversed.
    ChunkPtr generating = makeComplexChunk(ChunkPos{0, 0, 0});
    generating->forceState(ChunkState::Generating);
    REQUIRE_FALSE(save.saveChunk(generating));

    ChunkPtr empty = Chunk::create(ChunkPos{1, 0, 0});
    REQUIRE_FALSE(save.saveChunk(empty));

    save.flush();
    REQUIRE(save.stats().chunksWritten == 0);
}

// ===========================================================================
//  Repeated and concurrent writes
// ===========================================================================

TEST_CASE("saving the same chunk twice does not corrupt the region", "[persistence]")
{
    TempDir   dir{"twice"};
    JobSystem jobs{2};

    const ChunkPos target{3, 2, 1};
    const ChunkPos bystander{4, 2, 1};

    ChunkPtr neighbour = makeComplexChunk(bystander, 11u);

    {
        WorldSave save(jobs, dir.path(), kSeed);

        ChunkPtr first = Chunk::create(target);
        first->storage().fill(voxl::blocks::Dirt);
        first->forceState(ChunkState::Ready);
        REQUIRE(save.saveChunk(first));
        REQUIRE(save.saveChunk(neighbour));
        save.flush();

        // The second version is much larger than the first, so it cannot reuse
        // the same sectors - which is the case where a naive allocator would
        // overwrite the neighbour that was written in between.
        ChunkPtr second = makeComplexChunk(target, 77u);
        REQUIRE(save.saveChunk(second));
        save.flush();

        REQUIRE(save.stats().chunksWritten == 3);
        REQUIRE(save.stats().writeFailures == 0);
    }

    WorldSave save(jobs, dir.path(), kSeed);

    ChunkPtr restored = Chunk::create(target);
    REQUIRE(save.loadChunk(*restored).status == ChunkLoadStatus::Loaded);
    const ChunkPtr expected = makeComplexChunk(target, 77u);
    REQUIRE(compareContents(*expected, *restored).voxels == 0);

    ChunkPtr restoredNeighbour = Chunk::create(bystander);
    REQUIRE(save.loadChunk(*restoredNeighbour).status == ChunkLoadStatus::Loaded);
    REQUIRE(compareContents(*neighbour, *restoredNeighbour).voxels == 0);
}

TEST_CASE("concurrent saves of different chunks in one region are safe", "[persistence]")
{
    TempDir   dir{"concurrent"};
    JobSystem jobs{4};

    // Every one of these positions lands in region (0, 0), so all 48 writes
    // contend for the same file handle, the same offset table and the same
    // sector allocator.
    std::vector<ChunkPtr> chunks;
    for (std::int32_t x = 0; x < 6; ++x) {
        for (std::int32_t z = 0; z < 4; ++z) {
            for (std::int32_t y = 0; y < 2; ++y) {
                chunks.push_back(makeComplexChunk(ChunkPos{x, y, z},
                                                  static_cast<std::uint32_t>(x * 97 + z * 13 + y)));
            }
        }
    }
    REQUIRE(chunks.size() == 48);
    for (const ChunkPtr& chunk : chunks) {
        REQUIRE(voxl::toRegionCoord(chunk->position()) == voxl::RegionCoord{0, 0});
    }

    {
        WorldSave save(jobs, dir.path(), kSeed);
        REQUIRE(save.saveChunks(std::span<const ChunkPtr>{chunks}) == chunks.size());
        save.flush();
        REQUIRE(save.stats().chunksWritten == chunks.size());
        REQUIRE(save.stats().writeFailures == 0);
    }

    WorldSave   save(jobs, dir.path(), kSeed);
    std::size_t mismatched = 0;
    for (const ChunkPtr& chunk : chunks) {
        ChunkPtr restored = Chunk::create(chunk->position());
        if (save.loadChunk(*restored).status != ChunkLoadStatus::Loaded) {
            ++mismatched;
            continue;
        }
        const Mismatch diff = compareContents(*chunk, *restored);
        mismatched += (diff.voxels != 0 || diff.light != 0) ? 1u : 0u;
    }
    REQUIRE(mismatched == 0);
    REQUIRE(save.stats().chunksLoaded == chunks.size());
    REQUIRE(save.stats().chunksCorrupt == 0);
}

// ===========================================================================
//  Corruption: the point of the exercise
// ===========================================================================

TEST_CASE("a region file with the wrong magic regenerates its chunks", "[persistence][corrupt]")
{
    TempDir   dir{"magic"};
    JobSystem jobs{2};
    const ChunkPos position{2, 2, 2};

    const ChunkPtr original = makeComplexChunk(position);
    {
        WorldSave save(jobs, dir.path(), kSeed);
        REQUIRE(save.saveChunk(original));
        save.flush();
    }

    const std::filesystem::path region = regionPathOf(dir.path(), position);
    patchU32(region, 0, 0xDEADBEEFu);

    {
        WorldSave save(jobs, dir.path(), kSeed);
        ChunkPtr  restored = Chunk::create(position);
        const auto result  = save.loadChunk(*restored);
        REQUIRE(result.status == ChunkLoadStatus::Corrupt);
        REQUIRE(result.error == SaveError::BadMagic);
        REQUIRE(result.regenerate());
        // Nothing was written into it - the caller must generate from the seed.
        REQUIRE(restored->isEmpty());
        REQUIRE(save.stats().chunksCorrupt == 1);

        // Saving again must still work: the damaged file is moved aside rather
        // than being an eternal write barrier.
        REQUIRE(save.saveChunk(original));
        save.flush();
        REQUIRE(save.stats().writeFailures == 0);
    }

    std::filesystem::path quarantined = region;
    quarantined += ".corrupt";
    REQUIRE(std::filesystem::exists(quarantined));

    WorldSave save(jobs, dir.path(), kSeed);
    ChunkPtr  restored = Chunk::create(position);
    REQUIRE(save.loadChunk(*restored).status == ChunkLoadStatus::Loaded);
    REQUIRE(compareContents(*original, *restored).voxels == 0);
}

TEST_CASE("a truncated region file regenerates its chunks", "[persistence][corrupt]")
{
    TempDir        dir{"truncated"};
    JobSystem      jobs{2};
    const ChunkPos position{1, 3, 1};

    {
        WorldSave      save(jobs, dir.path(), kSeed);
        const ChunkPtr original = makeComplexChunk(position);
        REQUIRE(save.saveChunk(original));
        save.flush();
    }

    const std::filesystem::path region = regionPathOf(dir.path(), position);

    SECTION("cut off inside the header and table")
    {
        std::error_code code;
        std::filesystem::resize_file(region, 100u, code);
        REQUIRE_FALSE(code);

        WorldSave  save(jobs, dir.path(), kSeed);
        ChunkPtr   restored = Chunk::create(position);
        const auto result   = save.loadChunk(*restored);
        REQUIRE(result.status == ChunkLoadStatus::Corrupt);
        REQUIRE(result.error == SaveError::Truncated);
        REQUIRE(restored->isEmpty());
    }

    SECTION("cut off before the payload the table points at")
    {
        std::error_code code;
        std::filesystem::resize_file(
            region, voxl::kRegionFirstDataSector * voxl::kRegionSectorSize, code);
        REQUIRE_FALSE(code);

        WorldSave  save(jobs, dir.path(), kSeed);
        ChunkPtr   restored = Chunk::create(position);
        const auto result   = save.loadChunk(*restored);
        REQUIRE(result.status == ChunkLoadStatus::Corrupt);
        REQUIRE(result.error == SaveError::OffsetOutOfRange);
        REQUIRE(restored->isEmpty());
    }

    SECTION("cut off in the middle of the payload")
    {
        const RawEntry entry = readRawEntry(region, position);
        REQUIRE(entry.sector >= voxl::kRegionFirstDataSector);

        std::error_code code;
        std::filesystem::resize_file(
            region,
            static_cast<std::uintmax_t>(entry.sector) * voxl::kRegionSectorSize + 32u, code);
        REQUIRE_FALSE(code);

        WorldSave  save(jobs, dir.path(), kSeed);
        ChunkPtr   restored = Chunk::create(position);
        const auto result   = save.loadChunk(*restored);
        REQUIRE(result.status == ChunkLoadStatus::Corrupt);
        REQUIRE(result.error == SaveError::OffsetOutOfRange);
        REQUIRE(restored->isEmpty());
    }
}

TEST_CASE("a payload whose checksum does not match regenerates", "[persistence][corrupt]")
{
    TempDir        dir{"checksum"};
    JobSystem      jobs{2};
    const ChunkPos position{7, 1, 2};

    {
        WorldSave save(jobs, dir.path(), kSeed);
        REQUIRE(save.saveChunk(makeComplexChunk(position)));
        save.flush();
    }

    const std::filesystem::path region = regionPathOf(dir.path(), position);
    const RawEntry              entry  = readRawEntry(region, position);
    REQUIRE(entry.sector >= voxl::kRegionFirstDataSector);

    // Flip a bit deep inside the body, past the payload header, so the structure
    // still parses and only the checksum can catch it.
    const std::uint64_t bodyStart = static_cast<std::uint64_t>(entry.sector) *
                                        voxl::kRegionSectorSize +
                                    voxl::kChunkPayloadHeaderBytes;
    const std::vector<std::byte> before = readWholeFile(region);
    const std::byte flipped = before[static_cast<std::size_t>(bodyStart) + 24] ^ std::byte{0x40};
    patchBytes(region, bodyStart + 24, std::span<const std::byte>{&flipped, 1});

    WorldSave  save(jobs, dir.path(), kSeed);
    ChunkPtr   restored = Chunk::create(position);
    const auto result   = save.loadChunk(*restored);
    REQUIRE(result.status == ChunkLoadStatus::Corrupt);
    REQUIRE(result.error == SaveError::BadChecksum);
    REQUIRE(restored->isEmpty());
}

TEST_CASE("a table entry pointing past the end of the file regenerates",
          "[persistence][corrupt]")
{
    TempDir        dir{"offset"};
    JobSystem      jobs{2};
    const ChunkPos position{0, 5, 6};

    {
        WorldSave save(jobs, dir.path(), kSeed);
        REQUIRE(save.saveChunk(makeComplexChunk(position)));
        save.flush();
    }

    const std::filesystem::path region = regionPathOf(dir.path(), position);
    const std::size_t entryOffset = voxl::regionEntryFileOffset(voxl::regionEntryIndex(position));

    SECTION("wildly out of range")
    {
        patchU32(region, entryOffset, 0x00F00000u);
    }
    SECTION("just past the last sector")
    {
        const RawEntry entry = readRawEntry(region, position);
        patchU32(region, entryOffset, entry.sector + entry.sectorCount + 1u);
    }
    SECTION("inside the offset table, where no payload can live")
    {
        patchU32(region, entryOffset, 1u);
    }

    WorldSave  save(jobs, dir.path(), kSeed);
    ChunkPtr   restored = Chunk::create(position);
    const auto result   = save.loadChunk(*restored);
    REQUIRE(result.status == ChunkLoadStatus::Corrupt);
    REQUIRE(result.error == SaveError::OffsetOutOfRange);
    REQUIRE(restored->isEmpty());
}

TEST_CASE("a zero-length payload regenerates", "[persistence][corrupt]")
{
    TempDir        dir{"empty"};
    JobSystem      jobs{2};
    const ChunkPos position{8, 0, 8};

    {
        WorldSave save(jobs, dir.path(), kSeed);
        REQUIRE(save.saveChunk(makeComplexChunk(position)));
        save.flush();
    }

    const std::filesystem::path region = regionPathOf(dir.path(), position);
    const std::size_t entryOffset = voxl::regionEntryFileOffset(voxl::regionEntryIndex(position));

    SECTION("the table claims zero sectors")
    {
        patchU16(region, entryOffset + 4u, 0u);

        WorldSave  save(jobs, dir.path(), kSeed);
        ChunkPtr   restored = Chunk::create(position);
        const auto result   = save.loadChunk(*restored);
        REQUIRE(result.status == ChunkLoadStatus::Corrupt);
        REQUIRE(result.error == SaveError::EmptyPayload);
        REQUIRE(restored->isEmpty());
    }

    SECTION("the payload header claims a zero-byte body")
    {
        const RawEntry entry = readRawEntry(region, position);
        const std::uint64_t headerAt =
            static_cast<std::uint64_t>(entry.sector) * voxl::kRegionSectorSize;
        patchU32(region, headerAt + 4u, 0u);  // bodyBytes

        WorldSave  save(jobs, dir.path(), kSeed);
        ChunkPtr   restored = Chunk::create(position);
        const auto result   = save.loadChunk(*restored);
        REQUIRE(result.status == ChunkLoadStatus::Corrupt);
        REQUIRE(result.error == SaveError::EmptyPayload);
        REQUIRE(restored->isEmpty());
    }

    SECTION("the payload header has been overwritten with zeroes")
    {
        const RawEntry entry = readRawEntry(region, position);
        const std::uint64_t headerAt =
            static_cast<std::uint64_t>(entry.sector) * voxl::kRegionSectorSize;
        const std::vector<std::byte> zeroes(voxl::kChunkPayloadHeaderBytes, std::byte{0});
        patchBytes(region, headerAt, std::span<const std::byte>{zeroes});

        WorldSave  save(jobs, dir.path(), kSeed);
        ChunkPtr   restored = Chunk::create(position);
        const auto result   = save.loadChunk(*restored);
        REQUIRE(result.status == ChunkLoadStatus::Corrupt);
        REQUIRE(result.error == SaveError::BadMagic);
        REQUIRE(restored->isEmpty());
    }
}

TEST_CASE("a region written by a newer format is refused and never overwritten",
          "[persistence][corrupt]")
{
    TempDir        dir{"newer"};
    JobSystem      jobs{2};
    const ChunkPos position{1, 1, 5};

    {
        WorldSave save(jobs, dir.path(), kSeed);
        REQUIRE(save.saveChunk(makeComplexChunk(position)));
        save.flush();
    }

    const std::filesystem::path region = regionPathOf(dir.path(), position);

    // Bump the version AND repair the header checksum, so the version check is
    // what rejects the file rather than the checksum tripping first.
    patchU16(region, 4u, static_cast<std::uint16_t>(voxl::kSaveFormatVersion + 1));
    {
        const std::vector<std::byte> file = readWholeFile(region);
        const std::uint32_t          crc =
            voxl::crc32(std::span<const std::byte>{file}.subspan(0, voxl::kRegionHeaderCrcBytes));
        patchU32(region, voxl::kRegionHeaderCrcBytes, crc);
    }

    const std::vector<std::byte> before = readWholeFile(region);

    {
        WorldSave  save(jobs, dir.path(), kSeed);
        ChunkPtr   restored = Chunk::create(position);
        const auto result   = save.loadChunk(*restored);
        REQUIRE(result.status == ChunkLoadStatus::Corrupt);
        REQUIRE(result.error == SaveError::UnsupportedVersion);
        REQUIRE(restored->isEmpty());

        // A write must fail rather than clobber a world a newer build can read.
        REQUIRE(save.saveChunk(makeComplexChunk(position)));  // queued...
        save.flush();
        REQUIRE(save.stats().chunksWritten == 0);  // ...and refused at the file
        REQUIRE(save.stats().writeFailures == 1);
    }

    REQUIRE(readWholeFile(region) == before);
    std::filesystem::path quarantined = region;
    quarantined += ".corrupt";
    REQUIRE_FALSE(std::filesystem::exists(quarantined));
}

TEST_CASE("a region belonging to another seed is refused", "[persistence][corrupt]")
{
    TempDir        dir{"seed"};
    JobSystem      jobs{2};
    const ChunkPos position{2, 0, 3};

    {
        WorldSave save(jobs, dir.path(), kSeed);
        REQUIRE(save.saveChunk(makeComplexChunk(position)));
        save.flush();
    }

    WorldSave  other(jobs, dir.path(), kSeed + 1u);
    ChunkPtr   restored = Chunk::create(position);
    const auto result   = other.loadChunk(*restored);
    REQUIRE(result.status == ChunkLoadStatus::Corrupt);
    REQUIRE(result.error == SaveError::SeedMismatch);
    REQUIRE(restored->isEmpty());
}

TEST_CASE("an absent region and an absent chunk both report Absent, not corruption",
          "[persistence]")
{
    TempDir   dir{"absent"};
    JobSystem jobs{2};
    WorldSave save(jobs, dir.path(), kSeed);

    ChunkPtr   nothing = Chunk::create(ChunkPos{100, 3, -250});
    const auto result  = save.loadChunk(*nothing);
    REQUIRE(result.status == ChunkLoadStatus::Absent);
    REQUIRE(result.error == SaveError::None);
    REQUIRE(result.regenerate());
    REQUIRE(save.stats().chunksCorrupt == 0);

    // A region that exists but has never held this particular chunk.
    REQUIRE(save.saveChunk(makeComplexChunk(ChunkPos{100, 4, -250})));
    save.flush();
    ChunkPtr sibling = Chunk::create(ChunkPos{100, 3, -250});
    REQUIRE(save.loadChunk(*sibling).status == ChunkLoadStatus::Absent);
    REQUIRE(save.stats().chunksCorrupt == 0);
}

// ===========================================================================
//  Corruption inside the payload body
// ===========================================================================

TEST_CASE("a payload is rejected without touching the chunk", "[persistence][corrupt]")
{
    SECTION("empty")
    {
        requireRejectedAndUntouched({}, SaveError::EmptyPayload);
    }

    SECTION("impossible bits-per-index")
    {
        PayloadBuilder builder = twoBitBody();
        builder.bits           = 3;
        requireRejectedAndUntouched(builder.build(), SaveError::MalformedPayload);
    }

    SECTION("a palette index past the end of the palette")
    {
        // The single most dangerous field in the format: unchecked, this is an
        // out-of-bounds read on every subsequent getBlock().
        PayloadBuilder builder = twoBitBody();
        builder.words[0]       = 0x3ull;  // slot 0 selects palette entry 3 of 3
        requireRejectedAndUntouched(builder.build(), SaveError::MalformedPayload);
    }

    SECTION("a palette count that does not fit the index width")
    {
        PayloadBuilder builder       = twoBitBody();
        builder.overridePaletteCount = true;
        builder.paletteCountOverride = 9u;  // 2 bits addresses at most 4
        requireRejectedAndUntouched(builder.build(), SaveError::MalformedPayload);
    }

    SECTION("a palette count of zero")
    {
        PayloadBuilder builder       = twoBitBody();
        builder.overridePaletteCount = true;
        builder.paletteCountOverride = 0u;
        requireRejectedAndUntouched(builder.build(), SaveError::MalformedPayload);
    }

    SECTION("a palette count larger than the bytes that follow")
    {
        PayloadBuilder builder       = twoBitBody();
        builder.bits                 = 16;
        builder.overridePaletteCount = true;
        builder.paletteCountOverride = 60000u;
        requireRejectedAndUntouched(builder.build(), SaveError::Truncated);
    }

    SECTION("an unknown light encoding")
    {
        PayloadBuilder builder = twoBitBody();
        builder.lightEncoding  = 9;
        requireRejectedAndUntouched(builder.build(), SaveError::MalformedPayload);
    }

    SECTION("a raw light array that is short")
    {
        PayloadBuilder builder = twoBitBody();
        builder.lightEncoding  = 1;
        builder.rawLight.assign(kChunkVolume / 2, 0xF0u);
        requireRejectedAndUntouched(builder.build(), SaveError::Truncated);
    }

    SECTION("a sub-voxel grid with nothing left in it")
    {
        // popcount 0 means the block should be air and carry no entry at all -
        // the exact disagreement the invariant in world/SubVoxel.hpp forbids.
        PayloadBuilder builder = twoBitBody();
        PayloadBuilder::Damage entry;
        entry.blockIndex = 5;
        entry.material   = voxl::blocks::Stone;
        builder.damage.push_back(entry);
        requireRejectedAndUntouched(builder.build(), SaveError::MalformedPayload);
    }

    SECTION("a sub-voxel grid that is completely full")
    {
        // popcount 512 means the block is whole and must not have an entry.
        PayloadBuilder builder = twoBitBody();
        PayloadBuilder::Damage entry;
        entry.blockIndex = 5;
        entry.material   = voxl::blocks::Stone;
        entry.grid       = SubVoxelGrid::solid(voxl::blocks::Stone);
        builder.damage.push_back(entry);
        requireRejectedAndUntouched(builder.build(), SaveError::MalformedPayload);
    }

    SECTION("a sub-voxel entry whose material disagrees with the block")
    {
        PayloadBuilder builder = twoBitBody();
        PayloadBuilder::Damage entry;
        entry.blockIndex = 5;
        entry.material   = voxl::blocks::Wood;  // the voxel there is Stone
        entry.grid       = SubVoxelGrid::solid(voxl::blocks::Wood);
        entry.grid.clear(0);
        builder.damage.push_back(entry);
        requireRejectedAndUntouched(builder.build(), SaveError::MalformedPayload);
    }

    SECTION("sub-voxel entries that are not in ascending block order")
    {
        PayloadBuilder builder = twoBitBody();
        for (const std::uint16_t index : {std::uint16_t{9}, std::uint16_t{4}}) {
            PayloadBuilder::Damage entry;
            entry.blockIndex = index;
            entry.material   = voxl::blocks::Stone;
            entry.grid       = SubVoxelGrid::solid(voxl::blocks::Stone);
            entry.grid.clear(1);
            builder.damage.push_back(entry);
        }
        requireRejectedAndUntouched(builder.build(), SaveError::MalformedPayload);
    }

    SECTION("a duplicated sub-voxel entry")
    {
        PayloadBuilder builder = twoBitBody();
        for (int i = 0; i < 2; ++i) {
            PayloadBuilder::Damage entry;
            entry.blockIndex = 4;
            entry.material   = voxl::blocks::Stone;
            entry.grid       = SubVoxelGrid::solid(voxl::blocks::Stone);
            entry.grid.clear(1);
            builder.damage.push_back(entry);
        }
        requireRejectedAndUntouched(builder.build(), SaveError::MalformedPayload);
    }

    SECTION("trailing bytes the decoder does not account for")
    {
        PayloadBuilder builder = twoBitBody();
        builder.trailingBytes  = 3;
        requireRejectedAndUntouched(builder.build(), SaveError::MalformedPayload);
    }
}

TEST_CASE("every truncation of a valid payload is rejected cleanly", "[persistence][corrupt]")
{
    const ChunkPtr source = makeComplexChunk(ChunkPos{2, 2, 2}, 55u);
    const std::vector<std::byte> full = WorldSave::encodeChunk(*source);
    REQUIRE(full.size() > 64);

    // Walk the whole payload rather than picking a few offsets: a decoder that
    // forgets one bounds check usually forgets it for a narrow range of lengths.
    std::size_t accepted = 0;
    for (std::size_t length = 0; length < full.size(); length += 7) {
        const std::vector<std::byte> partial(full.begin(),
                                             full.begin() + static_cast<std::ptrdiff_t>(length));
        ChunkPtr   victim = Chunk::create(ChunkPos{2, 2, 2});
        const auto result = WorldSave::decodeChunk(std::span<const std::byte>{partial}, *victim);
        if (result.status != ChunkLoadStatus::Corrupt) {
            ++accepted;
        }
        if (result.status == ChunkLoadStatus::Corrupt) {
            REQUIRE(victim->isEmpty());  // never half-written
        }
    }
    REQUIRE(accepted == 0);
}

TEST_CASE("a payload with random bytes flipped never crashes and never half-loads",
          "[persistence][corrupt]")
{
    const ChunkPtr source = makeComplexChunk(ChunkPos{6, 6, 6}, 123u);
    const std::vector<std::byte> full = WorldSave::encodeChunk(*source);

    Lcg rng{9001u};
    for (int trial = 0; trial < 400; ++trial) {
        std::vector<std::byte> damaged = full;
        const std::size_t      index   = rng.below(static_cast<std::uint32_t>(damaged.size()));
        damaged[index] ^= static_cast<std::byte>(1u << (rng.below(8u)));

        ChunkPtr   victim = Chunk::create(ChunkPos{6, 6, 6});
        const auto result = WorldSave::decodeChunk(std::span<const std::byte>{damaged}, *victim);

        // Either the flip landed somewhere harmless and the payload still decodes
        // to a legal chunk, or it is rejected. What must never happen is a decode
        // that "succeeds" into an inconsistent chunk, so the invariant is checked
        // on every accepted result.
        if (result.status == ChunkLoadStatus::Loaded) {
            victim->subVoxels().forEach([&](std::uint16_t blockIndex, const SubVoxelGrid& grid) {
                REQUIRE(grid.count() > 0);
                REQUIRE(grid.count() < kSubVoxelCount);
                REQUIRE(grid.material == victim->getBlock(blockIndex));
            });
        } else {
            REQUIRE(victim->isEmpty());
        }
    }
}

// ===========================================================================
//  Metadata
// ===========================================================================

TEST_CASE("world metadata round-trips every field", "[persistence][metadata]")
{
    TempDir   dir{"meta"};
    JobSystem jobs{2};
    WorldSave save(jobs, dir.path(), kSeed);

    WorldMetadata written;
    written.seed            = kSeed;
    written.playerPosition  = glm::vec3{-123.5f, 101.25f, 4096.75f};
    written.yawDegrees      = 217.5f;
    written.pitchDegrees    = -33.25f;
    written.hotbarSlot      = 6;
    written.timeOfDay       = 0.8125f;
    written.playTimeSeconds = 98765.4321;

    REQUIRE(save.writeMetadata(written));
    REQUIRE(std::filesystem::exists(WorldSave::metadataPath(dir.path())));

    WorldMetadata read;
    SaveError     error = SaveError::Io;
    REQUIRE(save.readMetadata(read, error));
    REQUIRE(error == SaveError::None);

    REQUIRE(read.seed == written.seed);
    REQUIRE(read.playerPosition.x == written.playerPosition.x);
    REQUIRE(read.playerPosition.y == written.playerPosition.y);
    REQUIRE(read.playerPosition.z == written.playerPosition.z);
    REQUIRE(read.yawDegrees == written.yawDegrees);
    REQUIRE(read.pitchDegrees == written.pitchDegrees);
    REQUIRE(read.hotbarSlot == written.hotbarSlot);
    REQUIRE(read.timeOfDay == written.timeOfDay);
    REQUIRE(read.playTimeSeconds == written.playTimeSeconds);
    REQUIRE(read.formatVersion == voxl::kSaveFormatVersion);
}

TEST_CASE("rewriting metadata replaces it atomically and leaves no temp file",
          "[persistence][metadata]")
{
    TempDir   dir{"meta2"};
    JobSystem jobs{2};
    WorldSave save(jobs, dir.path(), kSeed);

    WorldMetadata first;
    first.seed       = kSeed;
    first.hotbarSlot = 1;
    REQUIRE(save.writeMetadata(first));

    WorldMetadata second;
    second.seed            = kSeed;
    second.hotbarSlot      = 8;
    second.playTimeSeconds = 42.0;
    REQUIRE(save.writeMetadata(second));

    WorldMetadata read;
    SaveError     error = SaveError::Io;
    REQUIRE(save.readMetadata(read, error));
    REQUIRE(read.hotbarSlot == 8);
    REQUIRE(read.playTimeSeconds == 42.0);

    std::filesystem::path temporary = WorldSave::metadataPath(dir.path());
    temporary += ".tmp";
    REQUIRE_FALSE(std::filesystem::exists(temporary));
}

TEST_CASE("damaged metadata is refused rather than misread", "[persistence][metadata][corrupt]")
{
    TempDir   dir{"meta3"};
    JobSystem jobs{2};
    WorldSave save(jobs, dir.path(), kSeed);

    WorldMetadata original;
    original.seed       = kSeed;
    original.hotbarSlot = 4;

    const std::filesystem::path path = WorldSave::metadataPath(dir.path());

    SECTION("missing")
    {
        WorldMetadata read;
        SaveError     error = SaveError::Io;
        REQUIRE_FALSE(save.readMetadata(read, error));
        REQUIRE(error == SaveError::None);
    }

    SECTION("bad magic")
    {
        REQUIRE(save.writeMetadata(original));
        patchU32(path, 0, 0x12345678u);
        WorldMetadata read;
        SaveError     error = SaveError::None;
        REQUIRE_FALSE(save.readMetadata(read, error));
        // The checksum covers the magic, so a smashed magic is caught as a bad
        // checksum first; either answer is a refusal, which is what matters.
        REQUIRE((error == SaveError::BadMagic || error == SaveError::BadChecksum));
    }

    SECTION("bad checksum")
    {
        REQUIRE(save.writeMetadata(original));
        patchU16(path, 20u, 0xBEEFu);  // inside the player position
        WorldMetadata read;
        SaveError     error = SaveError::None;
        REQUIRE_FALSE(save.readMetadata(read, error));
        REQUIRE(error == SaveError::BadChecksum);
    }

    SECTION("truncated")
    {
        REQUIRE(save.writeMetadata(original));
        std::error_code code;
        std::filesystem::resize_file(path, 20u, code);
        REQUIRE_FALSE(code);

        WorldMetadata read;
        SaveError     error = SaveError::None;
        REQUIRE_FALSE(save.readMetadata(read, error));
        REQUIRE(error == SaveError::Truncated);
    }

    SECTION("written by a newer format")
    {
        REQUIRE(save.writeMetadata(original));
        patchU16(path, 4u, static_cast<std::uint16_t>(voxl::kSaveFormatVersion + 1));

        // Repair the checksum so the version check is what does the rejecting.
        const std::vector<std::byte> file = readWholeFile(path);
        const std::uint32_t          crc =
            voxl::crc32(std::span<const std::byte>{file}.subspan(0, file.size() - 4));
        patchU32(path, file.size() - 4, crc);

        WorldMetadata read;
        SaveError     error = SaveError::None;
        REQUIRE_FALSE(save.readMetadata(read, error));
        REQUIRE(error == SaveError::UnsupportedVersion);
    }
}

// ===========================================================================
//  Region addressing and autosave pacing
// ===========================================================================

TEST_CASE("region addressing floors at negative coordinates and never collides",
          "[persistence]")
{
    REQUIRE(voxl::toRegionCoord(ChunkPos{0, 0, 0}) == voxl::RegionCoord{0, 0});
    REQUIRE(voxl::toRegionCoord(ChunkPos{15, 0, 15}) == voxl::RegionCoord{0, 0});
    REQUIRE(voxl::toRegionCoord(ChunkPos{16, 0, 0}) == voxl::RegionCoord{1, 0});
    // Truncating division would fold -1 onto region 0 and tear the world along
    // both negative axes, the same bug class as blockToChunkAxis.
    REQUIRE(voxl::toRegionCoord(ChunkPos{-1, 0, -1}) == voxl::RegionCoord{-1, -1});
    REQUIRE(voxl::toRegionCoord(ChunkPos{-16, 0, -17}) == voxl::RegionCoord{-1, -2});

    std::vector<bool> seen(voxl::kRegionEntryCount, false);
    for (std::int32_t x = 0; x < voxl::kRegionSizeColumns; ++x) {
        for (std::int32_t z = 0; z < voxl::kRegionSizeColumns; ++z) {
            for (std::int32_t y = 0; y < voxl::kWorldSectionCount; ++y) {
                const std::size_t index = voxl::regionEntryIndex(ChunkPos{x, y, z});
                REQUIRE(index < voxl::kRegionEntryCount);
                REQUIRE_FALSE(seen[index]);
                seen[index] = true;
            }
        }
    }

    // Chunks 16 apart share a slot only because they are in different files.
    REQUIRE(voxl::regionEntryIndex(ChunkPos{0, 3, 0}) ==
            voxl::regionEntryIndex(ChunkPos{16, 3, -16}));
}

TEST_CASE("autosave fires on its interval and not before", "[persistence]")
{
    TempDir   dir{"autosave"};
    JobSystem jobs{2};
    WorldSave save(jobs, dir.path(), kSeed);

    save.setAutosaveIntervalSeconds(10.0);
    REQUIRE(save.autosaveIntervalSeconds() == 10.0);

    REQUIRE_FALSE(save.tickAutosave(100.0));  // first call only sets the epoch
    REQUIRE_FALSE(save.tickAutosave(105.0));
    REQUIRE(save.tickAutosave(110.0));
    REQUIRE_FALSE(save.tickAutosave(115.0));
    REQUIRE(save.tickAutosave(121.0));

    // A sub-second interval would encode the whole dirty set every frame.
    save.setAutosaveIntervalSeconds(0.0);
    REQUIRE(save.autosaveIntervalSeconds() == 1.0);
}

TEST_CASE("the region cache stays bounded while writing across many regions", "[persistence]")
{
    TempDir   dir{"cache"};
    JobSystem jobs{2};
    WorldSave save(jobs, dir.path(), kSeed);

    // Each of these is 16 columns apart, so every one lands in its own file.
    for (std::int32_t i = 0; i < static_cast<std::int32_t>(WorldSave::kMaxOpenRegions) + 12; ++i) {
        ChunkPtr chunk = Chunk::create(ChunkPos{i * voxl::kRegionSizeColumns, 0, 0});
        chunk->storage().fill(voxl::blocks::Stone);
        chunk->forceState(ChunkState::Ready);
        REQUIRE(save.saveChunk(chunk));
        save.flush();
    }

    REQUIRE(save.stats().openRegions <= WorldSave::kMaxOpenRegions);
    REQUIRE(save.stats().writeFailures == 0);

    // Evicting a handle must not lose the bytes it wrote.
    ChunkPtr restored = Chunk::create(ChunkPos{0, 0, 0});
    REQUIRE(save.loadChunk(*restored).status == ChunkLoadStatus::Loaded);
    REQUIRE(restored->getBlock(0) == voxl::blocks::Stone);
}

TEST_CASE("payloads stay well under a raw voxel dump", "[persistence]")
{
    // The format's entire justification: a section is written as its palette and
    // packed index words, never as 32768 block ids. These bounds are loose on
    // purpose - they are here to catch a future change that quietly starts
    // writing the dense array, not to pin an exact size.
    constexpr std::size_t kRawVoxelBytes = kChunkVolume * sizeof(BlockId);

    ChunkPtr uniform = Chunk::create(ChunkPos{0, 0, 0});
    uniform->storage().fill(voxl::blocks::Stone);
    uniform->storage().fillLight(15, 0);
    REQUIRE(WorldSave::encodeChunk(*uniform).size() < 32u);

    // Four materials in horizontal bands, which is roughly what real terrain
    // looks like: two bits per index and long light runs.
    ChunkPtr layered = Chunk::create(ChunkPos{0, 0, 0});
    for (std::int32_t y = 0; y < voxl::kChunkSize; ++y) {
        const BlockId material = y < 8    ? voxl::blocks::Bedrock
                                 : y < 20 ? voxl::blocks::Stone
                                 : y < 24 ? voxl::blocks::Dirt
                                          : voxl::blocks::Grass;
        for (std::int32_t z = 0; z < voxl::kChunkSize; ++z) {
            for (std::int32_t x = 0; x < voxl::kChunkSize; ++x) {
                const std::size_t index = voxl::localIndex(x, y, z);
                layered->storage().set(index, material);
                layered->storage().setLight(index,
                                            ChunkStorage::packLight(y > 24 ? 15u : 0u, 0u));
            }
        }
    }
    // As the terrain generator does once it has finished writing: without it the
    // palette still carries the Air the section started uniform at, which pushes
    // the index width from 2 bits to 4 and doubles the payload.
    layered->storage().optimise();
    REQUIRE(WorldSave::encodeChunk(*layered).size() < kRawVoxelBytes / 4u);

    // Even the adversarial case - 18 materials scattered at random so the index
    // words cannot compress at all - stays below the raw dump.
    const ChunkPtr scattered = makeComplexChunk(ChunkPos{0, 0, 0}, 3u);
    REQUIRE(WorldSave::encodeChunk(*scattered).size() < kRawVoxelBytes);
}

TEST_CASE("the CRC matches the standard check value", "[persistence]")
{
    // Guards the on-disk meaning of every checksum already written: if this
    // changes, every existing save file silently becomes unreadable.
    const char*            text = "123456789";
    std::vector<std::byte> data;
    for (std::size_t i = 0; i < 9; ++i) {
        data.push_back(static_cast<std::byte>(text[i]));
    }
    REQUIRE(voxl::crc32(std::span<const std::byte>{data}) == 0xCBF43926u);
    REQUIRE(voxl::crc32(std::span<const std::byte>{}) == 0u);
}

// ===========================================================================
//  Divergence from generated terrain
//
//  The signal StreamingConfig::preserveEditedChunks needs is "this position is
//  player work", and it used to be spelled Chunk::needsSave() - which means
//  "differs from disk" and is cleared by the saver. Everything below is about
//  the gap between those two statements, because a build that fell into it was
//  regenerated from the seed and silently deleted.
// ===========================================================================

TEST_CASE("an autosaved build is still protected once the dirty flag is cleared",
          "[persistence][lod]")
{
    TempDir   dir{"diverge"};
    JobSystem jobs{2};

    const ChunkPos built{2, 3, 5};
    const ChunkPos untouched{2, 3, 6};

    {
        WorldSave save(jobs, dir.path(), kSeed);
        REQUIRE(save.indexStoredChunks() == 0);  // nothing on disk yet

        // The player builds. Both guards agree while the edit is still only in
        // memory, which is the case that always worked.
        const ChunkPtr chunk = makeComplexChunk(built);
        REQUIRE(chunk->needsSave());
        REQUIRE(oldLodGuard(chunk));

        // Thirty seconds later the autosave runs. This is the whole defect: the
        // write is what removes the protection.
        REQUIRE(save.saveChunk(chunk));
        save.noteStoredChunk(chunk->position());  // as Application::saveEverything does
        save.flush();
        REQUIRE(save.stats().chunksWritten == 1);

        REQUIRE_FALSE(chunk->needsSave());
        REQUIRE_FALSE(oldLodGuard(chunk));  // <- unprotected, with the build on disk
        REQUIRE(newLodGuard(save, chunk));  // <- the sticky signal survives the save
    }

    // A relaunch. The chunk object is gone, so no flag on it could carry the
    // protection even in principle; the index rebuilt from the region tables is
    // what makes a build from a previous session safe.
    WorldSave reopened(jobs, dir.path(), kSeed);
    REQUIRE(reopened.storedChunkCount() == 0);
    REQUIRE(reopened.indexStoredChunks() == 1);
    REQUIRE(reopened.storedChunkCount() == 1);

    REQUIRE(reopened.hasStoredChunk(built));
    REQUIRE_FALSE(reopened.hasStoredChunk(untouched));

    // The predicate handed to ChunkManager answers exactly the same way.
    const std::function<bool(const ChunkPos&)> predicate = reopened.storedChunkPredicate();
    REQUIRE(predicate(built));
    REQUIRE_FALSE(predicate(untouched));

    // And a RELOADED chunk stays protected, which is the case the old flag could
    // not express at all: decodeChunk marks it saved, so needsSave() is false the
    // instant it exists.
    ChunkPtr restored = Chunk::create(built);
    REQUIRE(reopened.loadChunk(*restored).status == ChunkLoadStatus::Loaded);
    REQUIRE_FALSE(restored->needsSave());
    REQUIRE_FALSE(oldLodGuard(restored));
    REQUIRE(newLodGuard(reopened, restored));
}

TEST_CASE("the divergence index finds every saved chunk across regions and nothing else",
          "[persistence][lod]")
{
    TempDir   dir{"diverge_index"};
    JobSystem jobs{2};

    // Two regions, one of them negative, and two sections of the same column -
    // the three ways the entry-index arithmetic can be got wrong.
    const ChunkPos positions[] = {
        ChunkPos{0, 0, 0},
        ChunkPos{5, 7, 11},
        ChunkPos{5, 2, 11},
        ChunkPos{-1, 4, -20},
    };

    {
        WorldSave save(jobs, dir.path(), kSeed);
        for (const ChunkPos& position : positions) {
            REQUIRE(save.saveChunk(makeComplexChunk(position)));
        }
        save.flush();
        REQUIRE(save.stats().writeFailures == 0);
    }

    WorldSave reopened(jobs, dir.path(), kSeed);
    REQUIRE(reopened.indexStoredChunks() == 4);

    for (const ChunkPos& position : positions) {
        CAPTURE(position.x, position.y, position.z);
        REQUIRE(reopened.hasStoredChunk(position));
    }

    // Neighbours in every direction, including the other sections of a column
    // that does have saved chunks, must stay demotable.
    REQUIRE_FALSE(reopened.hasStoredChunk(ChunkPos{5, 3, 11}));
    REQUIRE_FALSE(reopened.hasStoredChunk(ChunkPos{6, 7, 11}));
    REQUIRE_FALSE(reopened.hasStoredChunk(ChunkPos{5, 7, 12}));
    REQUIRE_FALSE(reopened.hasStoredChunk(ChunkPos{-1, 4, -21}));
    REQUIRE_FALSE(reopened.hasStoredChunk(ChunkPos{1, 0, 0}));

    // Indexing twice must not double-count: the registry is a set of positions,
    // not a tally of writes.
    REQUIRE(reopened.indexStoredChunks() == 4);
    REQUIRE(reopened.storedChunkCount() == 4);
}

TEST_CASE("regions belonging to another seed are not indexed as player work",
          "[persistence][lod]")
{
    TempDir   dir{"diverge_seed"};
    JobSystem jobs{2};

    {
        WorldSave save(jobs, dir.path(), kSeed);
        REQUIRE(save.saveChunk(makeComplexChunk(ChunkPos{1, 2, 3})));
        save.flush();
    }

    // RegionFile refuses this file with SeedMismatch, so its chunks really will
    // come from the generator. Protecting them would pin terrain at a level for
    // no reason.
    WorldSave other(jobs, dir.path(), kSeed ^ 0xFFFFull);
    REQUIRE(other.indexStoredChunks() == 0);
    REQUIRE_FALSE(other.hasStoredChunk(ChunkPos{1, 2, 3}));
}

// ===========================================================================
//  Seed recovery: level.vxw is not the only copy of the seed
// ===========================================================================

TEST_CASE("a region header reports the seed the world was written with",
          "[persistence][metadata]")
{
    TempDir   dir{"seed_header"};
    JobSystem jobs{2};

    {
        WorldSave save(jobs, dir.path(), kSeed);
        REQUIRE(save.saveChunk(makeComplexChunk(ChunkPos{0, 1, 0})));
        save.flush();
    }

    const std::filesystem::path region = regionPathOf(dir.path(), ChunkPos{0, 1, 0});
    REQUIRE(std::filesystem::exists(region));

    RegionHeaderInfo info;
    SaveError        error = SaveError::Io;
    REQUIRE(voxl::readRegionHeader(region, info, error));
    REQUIRE(error == SaveError::None);
    REQUIRE(info.seed == kSeed);
    REQUIRE(info.formatVersion == voxl::kSaveFormatVersion);
    REQUIRE(info.regionX == 0);
    REQUIRE(info.regionZ == 0);
}

TEST_CASE("a damaged level.vxw recovers its seed from a region header",
          "[persistence][metadata][corrupt]")
{
    TempDir   dir{"seed_recover"};
    JobSystem jobs{2};

    // A world created from the main menu: its seed is clock-derived and lives
    // nowhere the caller knows about.
    constexpr std::uint64_t kMenuSeed  = 0x0123456789ABCDEFull;
    constexpr std::uint64_t kWrongSeed = 0xDEADBEEFull;
    const ChunkPos          built{0, 2, 0};

    {
        WorldSave     save(jobs, dir.path(), kMenuSeed);
        WorldMetadata metadata;
        metadata.seed = kMenuSeed;
        REQUIRE(save.writeMetadata(metadata));
        REQUIRE(save.saveChunk(makeComplexChunk(built)));
        save.flush();
    }

    const std::filesystem::path level  = WorldSave::metadataPath(dir.path());
    const std::filesystem::path region = regionPathOf(dir.path(), built);

    SECTION("truncated")
    {
        std::error_code code;
        std::filesystem::resize_file(level, 20u, code);
        REQUIRE_FALSE(code);
    }

    SECTION("bad checksum")
    {
        patchU16(level, 20u, 0xBEEFu);  // inside the player position
    }

    SECTION("deleted outright")
    {
        std::error_code code;
        std::filesystem::remove(level, code);
        REQUIRE_FALSE(code);
    }

    // Whatever the damage, the metadata is unusable...
    WorldMetadata ignored;
    SaveError     metadataError = SaveError::None;
    REQUIRE_FALSE(voxl::readWorldMetadata(dir.path(), ignored, metadataError));

    // ...and the seed comes back off the region header instead of the caller's.
    const WorldSeedResolution resolution = voxl::resolveWorldSeed(dir.path(), kWrongSeed);
    REQUIRE(resolution.source == SeedSource::RegionHeader);
    REQUIRE(resolution.seed == kMenuSeed);
    REQUIRE_FALSE(resolution.hasMetadata);
    REQUIRE(resolution.seedFile.filename() == region.filename());
    REQUIRE_FALSE(resolution.regionsPresentButUnreadable);

    // The world genuinely loads on the recovered seed.
    {
        WorldSave recovered(jobs, dir.path(), resolution.seed);
        ChunkPtr  chunk = Chunk::create(built);
        REQUIRE(recovered.loadChunk(*chunk).status == ChunkLoadStatus::Loaded);
    }

    // And it genuinely does NOT on the seed the caller asked for, which is what
    // the old fallback used: every region refuses, the whole world regenerates
    // as fresh terrain, and the next save writes that over the player's build.
    {
        WorldSave wrong(jobs, dir.path(), kWrongSeed);
        ChunkPtr  chunk = Chunk::create(built);
        REQUIRE(wrong.loadChunk(*chunk).regenerate());
    }
}

TEST_CASE("seed recovery skips a region it cannot validate and takes the next one",
          "[persistence][metadata][corrupt]")
{
    TempDir   dir{"seed_skip"};
    JobSystem jobs{2};

    constexpr std::uint64_t kMenuSeed = 0x5EEDF00Dull;

    // "r.-1.-1.vxr" sorts before "r.0.0.vxr", so the damaged one is the first
    // candidate and recovery has to keep looking.
    const ChunkPos negative{-1, 0, -1};
    const ChunkPos positive{0, 0, 0};

    {
        WorldSave save(jobs, dir.path(), kMenuSeed);
        REQUIRE(save.saveChunk(makeComplexChunk(negative)));
        REQUIRE(save.saveChunk(makeComplexChunk(positive)));
        save.flush();
    }

    const std::filesystem::path damaged = regionPathOf(dir.path(), negative);
    const std::filesystem::path intact  = regionPathOf(dir.path(), positive);
    REQUIRE(damaged.filename().string() < intact.filename().string());

    SECTION("bad magic")
    {
        patchU32(damaged, 0u, 0x12345678u);
        RegionHeaderInfo info;
        SaveError        error = SaveError::None;
        REQUIRE_FALSE(voxl::readRegionHeader(damaged, info, error));
        REQUIRE(error == SaveError::BadMagic);
    }

    SECTION("a rewritten seed the checksum does not cover for")
    {
        // The nastiest case, and the reason the CRC is checked before the seed
        // is believed: the bytes at offset 20 ARE a seed, they are just not this
        // world's. Handing them back would be worse than the fallback.
        patchBytes(damaged, 20u, [] {
            std::vector<std::byte> encoded;
            voxl::bytes::putU64(encoded, 0xBADBADBADBADull);
            return encoded;
        }());

        RegionHeaderInfo info;
        SaveError        error = SaveError::None;
        REQUIRE_FALSE(voxl::readRegionHeader(damaged, info, error));
        REQUIRE(error == SaveError::BadChecksum);
    }

    SECTION("truncated to less than a header")
    {
        std::error_code code;
        std::filesystem::resize_file(damaged, 32u, code);
        REQUIRE_FALSE(code);

        RegionHeaderInfo info;
        SaveError        error = SaveError::None;
        REQUIRE_FALSE(voxl::readRegionHeader(damaged, info, error));
        REQUIRE(error == SaveError::Truncated);
    }

    std::uint64_t         seed = 0;
    std::filesystem::path source;
    REQUIRE(voxl::recoverSeedFromRegions(dir.path(), seed, source));
    REQUIRE(seed == kMenuSeed);
    REQUIRE(source.filename() == intact.filename());
}

TEST_CASE("with nothing left to recover from, the seed falls back and says which case it is",
          "[persistence][metadata][corrupt]")
{
    JobSystem jobs{2};

    constexpr std::uint64_t kRequested = 0xABCDEFull;

    SECTION("a brand-new world is not an error")
    {
        TempDir dir{"seed_new"};

        const WorldSeedResolution resolution = voxl::resolveWorldSeed(dir.path(), kRequested);
        REQUIRE(resolution.source == SeedSource::Requested);
        REQUIRE(resolution.seed == kRequested);
        REQUIRE_FALSE(resolution.hasMetadata);
        REQUIRE(resolution.metadataError == SaveError::None);
        REQUIRE_FALSE(resolution.regionsPresentButUnreadable);
    }

    SECTION("a directory that does not exist at all")
    {
        TempDir dir{"seed_absent"};
        const WorldSeedResolution resolution =
            voxl::resolveWorldSeed(dir.path() / "no_such_world", kRequested);
        REQUIRE(resolution.source == SeedSource::Requested);
        REQUIRE(resolution.seed == kRequested);
        REQUIRE_FALSE(resolution.regionsPresentButUnreadable);
    }

    SECTION("damaged metadata and no regions")
    {
        TempDir       dir{"seed_meta_only"};
        WorldSave     save(jobs, dir.path(), kSeed);
        WorldMetadata metadata;
        metadata.seed = kSeed;
        REQUIRE(save.writeMetadata(metadata));
        patchU16(WorldSave::metadataPath(dir.path()), 20u, 0xBEEFu);

        const WorldSeedResolution resolution = voxl::resolveWorldSeed(dir.path(), kRequested);
        REQUIRE(resolution.source == SeedSource::Requested);
        REQUIRE(resolution.seed == kRequested);
        REQUIRE(resolution.metadataError == SaveError::BadChecksum);
        REQUIRE_FALSE(resolution.regionsPresentButUnreadable);
    }

    SECTION("damaged metadata and every region unreadable too")
    {
        TempDir        dir{"seed_all_bad"};
        const ChunkPos built{0, 0, 0};
        {
            WorldSave     save(jobs, dir.path(), kSeed);
            WorldMetadata metadata;
            metadata.seed = kSeed;
            REQUIRE(save.writeMetadata(metadata));
            REQUIRE(save.saveChunk(makeComplexChunk(built)));
            save.flush();
        }
        patchU16(WorldSave::metadataPath(dir.path()), 20u, 0xBEEFu);
        patchU32(regionPathOf(dir.path(), built), 0u, 0x12345678u);

        const WorldSeedResolution resolution = voxl::resolveWorldSeed(dir.path(), kRequested);
        REQUIRE(resolution.source == SeedSource::Requested);
        REQUIRE(resolution.seed == kRequested);
        // The flag the caller logs an ERROR on, rather than a shrug: there is
        // real data here and it is about to be orphaned.
        REQUIRE(resolution.regionsPresentButUnreadable);
    }
}

TEST_CASE("an intact level.vxw still wins over the region headers", "[persistence][metadata]")
{
    TempDir   dir{"seed_meta_wins"};
    JobSystem jobs{2};

    constexpr std::uint64_t kWorldSeed = 0x1111'2222'3333'4444ull;

    WorldSave     save(jobs, dir.path(), kWorldSeed);
    WorldMetadata metadata;
    metadata.seed            = kWorldSeed;
    metadata.hotbarSlot      = 3;
    metadata.playTimeSeconds = 1234.0;
    REQUIRE(save.writeMetadata(metadata));
    REQUIRE(save.saveChunk(makeComplexChunk(ChunkPos{0, 0, 0})));
    save.flush();

    const WorldSeedResolution resolution = voxl::resolveWorldSeed(dir.path(), 7u);
    REQUIRE(resolution.source == SeedSource::Metadata);
    REQUIRE(resolution.seed == kWorldSeed);
    REQUIRE(resolution.hasMetadata);
    // The rest of the metadata comes back with it, so the caller does not read
    // the file twice.
    REQUIRE(resolution.metadata.hotbarSlot == 3);
    REQUIRE(resolution.metadata.playTimeSeconds == 1234.0);
    REQUIRE(resolution.seedFile.empty());
}
