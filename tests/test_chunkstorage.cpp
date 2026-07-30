#include <catch2/catch_test_macros.hpp>

#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/ChunkStorage.hpp"
#include "world/VoxelTypes.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <thread>
#include <vector>

namespace voxl::detail {
// Defined in src/world/ChunkStorage.cpp and src/world/Chunk.cpp. Those
// translation units are pure compile-time contract verification (both types are
// inline by contract), so referencing these anchors is what proves they are
// actually in the link and their static_asserts really ran.
extern const bool kChunkStorageContractVerified;
extern const bool kChunkContractVerified;
}  // namespace voxl::detail

namespace {

/// Deterministic scatter. A failure here must reproduce byte for byte on a
/// different machine, so the tests carry their own LCG rather than touching
/// std::random_device or rand().
class Lcg {
public:
    explicit constexpr Lcg(std::uint32_t seed) noexcept : m_state(seed) {}

    std::uint32_t next() noexcept
    {
        m_state = m_state * 1664525u + 1013904223u;  // Numerical Recipes constants
        return m_state;
    }

    /// Uniform enough for scattering writes; the modulo bias is irrelevant here.
    [[nodiscard]] std::size_t below(std::size_t bound) noexcept
    {
        return static_cast<std::size_t>(next() % static_cast<std::uint32_t>(bound));
    }

private:
    std::uint32_t m_state;
};

/// Index of the first voxel where the storage disagrees with the reference
/// model, or kChunkVolume when they agree everywhere. Returning the index rather
/// than a bool matters: a repack bug shows up at one specific bit offset, and the
/// index is what identifies which word boundary went wrong.
[[nodiscard]] std::size_t firstBlockMismatch(const voxl::ChunkStorage&          storage,
                                             const std::vector<voxl::BlockId>& expected)
{
    for (std::size_t i = 0; i < voxl::kChunkVolume; ++i) {
        if (storage.get(i) != expected[i]) {
            return i;
        }
    }
    return voxl::kChunkVolume;
}

/// Voxel indices that sit on or next to a packed-word boundary at every legal
/// width. At 1 bit a word holds 64 slots, at 16 bits it holds 4, so covering the
/// neighbourhood of every multiple of 4, 8, 16, 32 and 64 near the array ends is
/// what exercises the shift arithmetic that a repack gets wrong.
[[nodiscard]] std::vector<std::size_t> boundaryIndices()
{
    return {0,   1,   2,   3,   4,   5,   7,     8,     15,    16,   17,
            31,  32,  33,  62,  63,  64,  65,    66,    127,   128,  129,
            255, 256, 257, 511, 512, 513, 4095,  4096,  4097,  8191, 8192,
            8193,
            voxl::kChunkVolume - 5, voxl::kChunkVolume - 4, voxl::kChunkVolume - 3,
            voxl::kChunkVolume - 2, voxl::kChunkVolume - 1};
}

}  // namespace

// ===========================================================================
//  Uniform representation
// ===========================================================================

TEST_CASE("a uniform section allocates no index memory", "[world][storage]")
{
    voxl::ChunkStorage storage;

    CHECK(storage.isUniform());
    CHECK(storage.isEmpty());
    CHECK(storage.bitsPerIndex() == 0);
    CHECK(storage.uniformValue() == voxl::blocks::Air);
    CHECK(storage.paletteSize() == 1);

    // The point of the dedicated uniform case: nothing on the heap at all.
    CHECK(storage.palette().empty());
    CHECK(storage.indexWords().empty());
    CHECK(storage.heapBytes() == 0);
    CHECK(storage.memoryUsageBytes() == sizeof(voxl::ChunkStorage));

    SECTION("reads anywhere return the uniform value")
    {
        CHECK(storage.get(0) == voxl::blocks::Air);
        CHECK(storage.get(voxl::kChunkVolume - 1) == voxl::blocks::Air);
        CHECK(storage.get(31, 31, 31) == voxl::blocks::Air);
    }

    SECTION("writing the value it already holds stays uniform")
    {
        for (const std::size_t index : boundaryIndices()) {
            storage.set(index, voxl::blocks::Air);
        }
        CHECK(storage.isUniform());
        CHECK(storage.heapBytes() == 0);
    }

    SECTION("a non-air uniform section is still allocation free but not empty")
    {
        voxl::ChunkStorage stone{voxl::blocks::Stone};
        CHECK(stone.isUniform());
        CHECK_FALSE(stone.isEmpty());
        CHECK(stone.uniformValue() == voxl::blocks::Stone);
        CHECK(stone.heapBytes() == 0);
        CHECK(stone.get(12, 5, 30) == voxl::blocks::Stone);
    }

    SECTION("one differing write leaves uniform, and fill() returns to it")
    {
        storage.set(1234, voxl::blocks::Stone);
        REQUIRE_FALSE(storage.isUniform());
        REQUIRE(storage.bitsPerIndex() == 1);
        CHECK(storage.heapBytes() > 0);
        CHECK(storage.indexWords().size() == voxl::ChunkStorage::wordCountFor(1));

        storage.fill(voxl::blocks::Dirt);
        CHECK(storage.isUniform());
        CHECK(storage.bitsPerIndex() == 0);
        CHECK(storage.uniformValue() == voxl::blocks::Dirt);
        CHECK(storage.palette().empty());
        CHECK(storage.indexWords().empty());
        CHECK(storage.heapBytes() == 0);
        CHECK(storage.get(1234) == voxl::blocks::Dirt);
    }
}

// ===========================================================================
//  Index ordering
// ===========================================================================

TEST_CASE("localIndex ordering matches the storage layout", "[world][storage]")
{
    // x fastest, then z, then y. The save format writes voxels in this order, so
    // this is a compatibility check, not a style preference.
    STATIC_REQUIRE(voxl::localIndex(0, 0, 0) == 0u);
    STATIC_REQUIRE(voxl::localIndex(1, 0, 0) == 1u);
    STATIC_REQUIRE(voxl::localIndex(0, 0, 1) == static_cast<std::size_t>(voxl::kChunkSize));
    STATIC_REQUIRE(voxl::localIndex(0, 1, 0) ==
                   static_cast<std::size_t>(voxl::kChunkSize) * voxl::kChunkSize);
    STATIC_REQUIRE(voxl::localIndex(31, 31, 31) == voxl::kChunkVolume - 1u);

    SECTION("the coordinate accessors address the same voxel as the index ones")
    {
        voxl::ChunkStorage storage;
        Lcg                rng{0x5EEDu};

        // Distinct ids so an ordering swap (say y and z transposed) cannot pass
        // by coincidence.
        for (std::int32_t y = 0; y < voxl::kChunkSize; ++y) {
            for (std::int32_t z = 0; z < voxl::kChunkSize; ++z) {
                for (std::int32_t x = 0; x < voxl::kChunkSize; ++x) {
                    const auto id = static_cast<voxl::BlockId>(1u + (rng.next() & 0x3FFu));
                    storage.set(x, y, z, id);
                    REQUIRE(storage.get(voxl::localIndex(x, y, z)) == id);
                }
            }
        }

        // And the reverse direction: write by index, read by coordinate.
        std::size_t mismatches = 0;
        for (std::size_t i = 0; i < voxl::kChunkVolume; ++i) {
            const auto x = static_cast<std::int32_t>(i % voxl::kChunkSize);
            const auto z = static_cast<std::int32_t>((i / voxl::kChunkSize) % voxl::kChunkSize);
            const auto y = static_cast<std::int32_t>(i / (static_cast<std::size_t>(voxl::kChunkSize) *
                                                          voxl::kChunkSize));
            if (storage.get(i) != storage.get(x, y, z)) {
                ++mismatches;
            }
        }
        CHECK(mismatches == 0);
    }
}

// ===========================================================================
//  Palette growth and repacking - the core of this file
// ===========================================================================

TEST_CASE("palette growth 0->1->2->4->8->16 preserves every previously written voxel",
          "[world][storage][palette]")
{
    voxl::ChunkStorage         storage;
    std::vector<voxl::BlockId> expected(voxl::kChunkVolume, voxl::blocks::Air);
    std::vector<std::uint8_t>  tiersSeen{storage.bitsPerIndex()};

    // Every write goes through here so that a width change is caught at the
    // exact write that caused the repack, and the whole section is re-verified
    // against the reference model at that moment. A repack that loses the high
    // bit of one index, or that shifts a slot into the wrong half of a word,
    // fails on the very first tier it corrupts rather than at the end where the
    // cause is unrecoverable.
    const auto write = [&](std::size_t index, voxl::BlockId id) {
        storage.set(index, id);
        expected[index] = id;

        if (storage.bitsPerIndex() != tiersSeen.back()) {
            const std::uint8_t from = tiersSeen.back();
            tiersSeen.push_back(storage.bitsPerIndex());

            INFO("repack from " << static_cast<unsigned>(from) << " to "
                                << static_cast<unsigned>(storage.bitsPerIndex())
                                << " bits per index, triggered at voxel " << index);
            const std::size_t bad = firstBlockMismatch(storage, expected);
            INFO("first corrupted voxel: " << bad);
            REQUIRE(bad == voxl::kChunkVolume);
            REQUIRE(storage.indexWords().size() ==
                    voxl::ChunkStorage::wordCountFor(storage.bitsPerIndex()));
        }
    };

    // Phase 1: seed every word-boundary voxel with a distinct id. Enough
    // distinct ids to walk the palette from uniform up to the 8-bit width, and
    // the indices are chosen so each repack has to move slots across words.
    const std::vector<std::size_t> boundaries = boundaryIndices();
    voxl::BlockId                  nextId     = 1;
    for (const std::size_t index : boundaries) {
        write(index, nextId++);
    }
    REQUIRE(storage.bitsPerIndex() == 8);

    // Phase 2: keep introducing distinct ids at scattered positions until the
    // palette passes 256 entries, which is the only way to reach 16 bits. Air
    // occupies slot 0, so 300 further ids is comfortably past the threshold.
    Lcg rng{0xC0FFEEu};
    while (nextId <= 320) {
        const voxl::BlockId id = nextId++;
        for (int repeat = 0; repeat < 6; ++repeat) {
            write(rng.below(voxl::kChunkVolume), id);
        }
    }

    // Every tier must have been visited in order. This is what makes the test
    // meaningful: without it a change that jumps straight from 1 to 16 bits
    // would still pass every round-trip check.
    CHECK(tiersSeen == std::vector<std::uint8_t>{0, 1, 2, 4, 8, 16});
    CHECK(storage.bitsPerIndex() == 16);
    CHECK(storage.paletteSize() > 256);
    CHECK(storage.indexWords().size() == voxl::ChunkStorage::wordCountFor(16));

    // Final full-volume verification at the terminal width.
    const std::size_t bad = firstBlockMismatch(storage, expected);
    INFO("first corrupted voxel at 16 bits per index: " << bad);
    REQUIRE(bad == voxl::kChunkVolume);

    SECTION("the boundary voxels specifically survived all four repacks")
    {
        for (const std::size_t index : boundaries) {
            INFO("boundary voxel " << index);
            CHECK(storage.get(index) == expected[index]);
        }
    }

    SECTION("a copy is independent and identical")
    {
        voxl::ChunkStorage copy = storage;
        CHECK(copy.bitsPerIndex() == storage.bitsPerIndex());
        CHECK(firstBlockMismatch(copy, expected) == voxl::kChunkVolume);

        copy.set(0, voxl::blocks::Bedrock);
        CHECK(copy.get(0) == voxl::blocks::Bedrock);
        CHECK(storage.get(0) == expected[0]);
    }

    SECTION("optimise preserves content while narrowing the representation")
    {
        const std::size_t paletteBefore = storage.paletteSize();
        storage.optimise();

        CHECK(storage.paletteSize() <= paletteBefore);
        CHECK(storage.bitsPerIndex() <= 16);
        CHECK(storage.indexWords().size() ==
              voxl::ChunkStorage::wordCountFor(storage.bitsPerIndex()));
        CHECK(firstBlockMismatch(storage, expected) == voxl::kChunkVolume);
    }
}

TEST_CASE("each index width round-trips every value it can address", "[world][storage][palette]")
{
    // Drives the palette to a chosen width, then writes a distinct palette entry
    // to a run of consecutive voxels spanning several words and reads them all
    // back. Consecutive voxels are what fill every slot of a word, so an
    // off-by-one in the slot shift shows up as neighbours swapping values.
    struct Tier {
        std::uint8_t bits;
        std::size_t  distinctIds;
    };
    const Tier tiers[] = {{1, 2}, {2, 4}, {4, 16}, {8, 256}, {16, 300}};

    for (const Tier& tier : tiers) {
        voxl::ChunkStorage         storage;
        std::vector<voxl::BlockId> expected(voxl::kChunkVolume, voxl::blocks::Air);

        // Fill the leading voxels cyclically with `distinctIds` values. Slot 0
        // of the palette is air, so ids run from 1.
        for (std::size_t i = 0; i < voxl::kChunkVolume; ++i) {
            const auto id = static_cast<voxl::BlockId>(1u + i % (tier.distinctIds - 1u));
            storage.set(i, id);
            expected[i] = id;
        }

        INFO("target width " << static_cast<unsigned>(tier.bits) << " bits");
        CHECK(storage.bitsPerIndex() == tier.bits);
        CHECK(storage.indexWords().size() == voxl::ChunkStorage::wordCountFor(tier.bits));
        CHECK(firstBlockMismatch(storage, expected) == voxl::kChunkVolume);

        // Rewriting a single voxel must not disturb its neighbours in the same
        // packed word.
        for (const std::size_t index : boundaryIndices()) {
            storage.set(index, voxl::blocks::Bedrock);
            expected[index] = voxl::blocks::Bedrock;
        }
        CHECK(firstBlockMismatch(storage, expected) == voxl::kChunkVolume);
    }
}

TEST_CASE("optimise reclaims unused palette entries", "[world][storage][palette]")
{
    // The palette is deliberately not reference counted, so entries linger until
    // optimise() sweeps: set() alone never shrinks the width. These cases pin
    // that documented behaviour down in both directions.
    voxl::ChunkStorage storage;
    Lcg                rng{0xBEEFu};

    for (voxl::BlockId id = 1; id <= 40; ++id) {
        for (int repeat = 0; repeat < 4; ++repeat) {
            storage.set(rng.below(voxl::kChunkVolume), id);
        }
    }
    REQUIRE(storage.bitsPerIndex() == 8);
    const std::size_t widePalette = storage.paletteSize();

    SECTION("overwriting every voxel does not shrink the width on its own")
    {
        for (std::size_t i = 0; i < voxl::kChunkVolume; ++i) {
            storage.set(i, voxl::blocks::Stone);
        }
        CHECK(storage.bitsPerIndex() == 8);
        CHECK(storage.paletteSize() == widePalette);
        CHECK_FALSE(storage.isUniform());
        // isEmpty() is likewise a representation query, not a content scan.
        CHECK_FALSE(storage.isEmpty());
    }

    SECTION("optimise collapses a single-valued section back to uniform")
    {
        for (std::size_t i = 0; i < voxl::kChunkVolume; ++i) {
            storage.set(i, voxl::blocks::Stone);
        }
        storage.optimise();

        CHECK(storage.isUniform());
        CHECK(storage.bitsPerIndex() == 0);
        CHECK(storage.uniformValue() == voxl::blocks::Stone);
        CHECK(storage.heapBytes() == 0);
        CHECK(storage.get(voxl::kChunkVolume - 1) == voxl::blocks::Stone);
    }

    SECTION("optimise narrows the width to fit the ids actually present")
    {
        for (std::size_t i = 0; i < voxl::kChunkVolume; ++i) {
            storage.set(i, voxl::blocks::Air);
        }
        storage.set(777, voxl::blocks::Glowstone);
        storage.optimise();

        // Two distinct ids remain, which is one bit per index.
        CHECK(storage.bitsPerIndex() == 1);
        CHECK(storage.paletteSize() == 2);
        CHECK(storage.get(777) == voxl::blocks::Glowstone);
        CHECK(storage.get(776) == voxl::blocks::Air);
        CHECK(storage.get(778) == voxl::blocks::Air);
        CHECK(storage.indexWords().size() == voxl::ChunkStorage::wordCountFor(1));
    }

    SECTION("optimise on a uniform section is a no-op")
    {
        voxl::ChunkStorage uniform{voxl::blocks::Water};
        uniform.optimise();
        CHECK(uniform.isUniform());
        CHECK(uniform.uniformValue() == voxl::blocks::Water);
        CHECK(uniform.heapBytes() == 0);
    }
}

// ===========================================================================
//  Light storage
// ===========================================================================

TEST_CASE("light packs sunlight and block light into separate nibbles", "[world][storage][light]")
{
    voxl::ChunkStorage storage;

    SECTION("uniform light costs nothing")
    {
        CHECK_FALSE(storage.hasLightData());
        CHECK(storage.lightData().empty());
        CHECK(storage.uniformLight() == 0);
        CHECK(storage.light(0) == 0);
        CHECK(storage.light(voxl::kChunkVolume - 1) == 0);

        storage.fillLight(voxl::ChunkStorage::kMaxLightLevel, 0);
        CHECK_FALSE(storage.hasLightData());
        CHECK(storage.uniformLight() == 0xF0u);
        CHECK(storage.sunlight(12345) == 15);
        CHECK(storage.blockLight(12345) == 0);

        // Writing the value the section already holds must not materialise the
        // per-voxel array; a sky section stays one byte.
        storage.setLight(12345, 0xF0u);
        CHECK_FALSE(storage.hasLightData());
    }

    SECTION("the first differing write materialises exactly kChunkVolume bytes")
    {
        storage.fillLight(15, 0);
        storage.setLight(500, voxl::ChunkStorage::packLight(4, 2));

        REQUIRE(storage.hasLightData());
        CHECK(storage.lightData().size() == voxl::kChunkVolume);
        CHECK(storage.sunlight(500) == 4);
        CHECK(storage.blockLight(500) == 2);
        // Every other voxel kept the old uniform value.
        CHECK(storage.light(499) == 0xF0u);
        CHECK(storage.light(501) == 0xF0u);
        CHECK(storage.light(0) == 0xF0u);
        CHECK(storage.light(voxl::kChunkVolume - 1) == 0xF0u);
    }

    SECTION("every sunlight/block-light pair round-trips, including the 4-bit maxima")
    {
        storage.setLight(0, voxl::ChunkStorage::packLight(1, 1));  // force allocation
        REQUIRE(storage.hasLightData());

        std::size_t index = 0;
        for (std::uint8_t sun = 0; sun <= voxl::ChunkStorage::kMaxLightLevel; ++sun) {
            for (std::uint8_t block = 0; block <= voxl::ChunkStorage::kMaxLightLevel; ++block) {
                storage.setLight(index, voxl::ChunkStorage::packLight(sun, block));
                REQUIRE(storage.sunlight(index) == sun);
                REQUIRE(storage.blockLight(index) == block);
                ++index;
            }
        }
        CHECK(index == 256);

        // The extremes explicitly: a full nibble must not bleed into its
        // neighbour, which is the failure a shift-by-3 or a 5-bit mask produces.
        storage.setLight(1000, voxl::ChunkStorage::packLight(15, 15));
        CHECK(storage.light(1000) == 0xFFu);
        storage.setLight(1001, voxl::ChunkStorage::packLight(15, 0));
        CHECK(storage.light(1001) == 0xF0u);
        storage.setLight(1002, voxl::ChunkStorage::packLight(0, 15));
        CHECK(storage.light(1002) == 0x0Fu);
    }

    SECTION("writing one nibble preserves the other")
    {
        storage.setLight(64, voxl::ChunkStorage::packLight(3, 7));
        REQUIRE(storage.sunlight(64) == 3);
        REQUIRE(storage.blockLight(64) == 7);

        storage.setSunlight(64, 15);
        CHECK(storage.sunlight(64) == 15);
        CHECK(storage.blockLight(64) == 7);

        storage.setBlockLight(64, 15);
        CHECK(storage.sunlight(64) == 15);
        CHECK(storage.blockLight(64) == 15);

        storage.setBlockLight(64, 0);
        CHECK(storage.sunlight(64) == 15);
        CHECK(storage.blockLight(64) == 0);

        storage.setSunlight(64, 0);
        CHECK(storage.light(64) == 0);
    }

    SECTION("levels above 15 are masked, not clamped")
    {
        // Documented consequence of packLight masking with kLightMask: a
        // lighting bug that overflows a level reads back as darkness.
        storage.setLight(7, voxl::ChunkStorage::packLight(1, 1));
        storage.setSunlight(7, 16);
        CHECK(storage.sunlight(7) == 0);
        storage.setBlockLight(7, 0xFF);
        CHECK(storage.blockLight(7) == 15);
    }

    SECTION("the coordinate accessor agrees with the index accessor")
    {
        storage.setLight(voxl::localIndex(5, 9, 21), voxl::ChunkStorage::packLight(6, 11));
        CHECK(storage.light(5, 9, 21) == voxl::ChunkStorage::packLight(6, 11));
        CHECK(storage.light(5, 9, 21) == storage.light(voxl::localIndex(5, 9, 21)));
    }

    SECTION("fillLight releases the per-voxel array")
    {
        storage.setLight(3, voxl::ChunkStorage::packLight(2, 2));
        REQUIRE(storage.hasLightData());

        storage.fillLight(0, 0);
        CHECK_FALSE(storage.hasLightData());
        CHECK(storage.lightData().empty());
        CHECK(storage.light(3) == 0);
    }

    SECTION("voxel and light storage are independent")
    {
        storage.setLight(42, voxl::ChunkStorage::packLight(9, 4));
        storage.set(42, voxl::blocks::Stone);
        CHECK(storage.light(42) == voxl::ChunkStorage::packLight(9, 4));

        // fill() documents that it leaves light untouched.
        storage.fill(voxl::blocks::Dirt);
        CHECK(storage.light(42) == voxl::ChunkStorage::packLight(9, 4));
        CHECK(storage.get(42) == voxl::blocks::Dirt);
    }
}

TEST_CASE("memory accounting reflects the active representation", "[world][storage][memory]")
{
    voxl::ChunkStorage storage;
    CHECK(storage.heapBytes() == 0);

    storage.set(0, voxl::blocks::Stone);
    const std::size_t oneBit = storage.heapBytes();
    CHECK(oneBit >= voxl::ChunkStorage::wordCountFor(1) * sizeof(std::uint64_t));

    for (voxl::BlockId id = 2; id <= 300; ++id) {
        storage.set(id, id);
    }
    REQUIRE(storage.bitsPerIndex() == 16);
    CHECK(storage.heapBytes() >=
          voxl::ChunkStorage::wordCountFor(16) * sizeof(std::uint64_t));
    CHECK(storage.heapBytes() > oneBit);

    const std::size_t withoutLight = storage.heapBytes();
    storage.setLight(0, voxl::ChunkStorage::packLight(1, 0));
    CHECK(storage.heapBytes() >= withoutLight + voxl::kChunkVolume);

    CHECK(storage.memoryUsageBytes() == sizeof(voxl::ChunkStorage) + storage.heapBytes());
}

// ===========================================================================
//  Chunk state machine
// ===========================================================================

TEST_CASE("the chunk transition table matches the documented lifecycle", "[world][chunk][state]")
{
    // Independent transcription of the diagram in Chunk.hpp. Duplicated on
    // purpose: it only has value as a second opinion.
    const auto legal = [](voxl::ChunkState from, voxl::ChunkState to) {
        using S = voxl::ChunkState;
        switch (from) {
            case S::Empty:      return to == S::Generating || to == S::Unloading;
            case S::Generating: return to == S::Generated;
            case S::Generated:  return to == S::Meshing || to == S::Unloading;
            case S::Meshing:    return to == S::Meshed;
            case S::Meshed:     return to == S::Ready || to == S::Unloading;
            case S::Ready:      return to == S::Meshing || to == S::Unloading;
            case S::Unloading:  return false;
        }
        return false;
    };

    for (std::size_t from = 0; from < voxl::kChunkStateCount; ++from) {
        for (std::size_t to = 0; to < voxl::kChunkStateCount; ++to) {
            const auto a = static_cast<voxl::ChunkState>(static_cast<std::uint8_t>(from));
            const auto b = static_cast<voxl::ChunkState>(static_cast<std::uint8_t>(to));
            INFO(voxl::toString(a) << " -> " << voxl::toString(b));
            REQUIRE(voxl::isLegalChunkTransition(a, b) == legal(a, b));
        }
    }

    SECTION("a busy chunk can never be unloaded")
    {
        // The one property whose violation is a use-after-free rather than a
        // visual glitch: a worker owns the chunk in these states.
        for (std::size_t i = 0; i < voxl::kChunkStateCount; ++i) {
            const auto state = static_cast<voxl::ChunkState>(static_cast<std::uint8_t>(i));
            if (voxl::isChunkBusy(state)) {
                INFO(voxl::toString(state));
                CHECK_FALSE(voxl::isLegalChunkTransition(state, voxl::ChunkState::Unloading));
            }
        }
        CHECK(voxl::isChunkBusy(voxl::ChunkState::Generating));
        CHECK(voxl::isChunkBusy(voxl::ChunkState::Meshing));
        CHECK_FALSE(voxl::isChunkBusy(voxl::ChunkState::Ready));
        CHECK_FALSE(voxl::isChunkBusy(voxl::ChunkState::Meshed));
    }

    SECTION("re-entering a state is always illegal")
    {
        for (std::size_t i = 0; i < voxl::kChunkStateCount; ++i) {
            const auto state = static_cast<voxl::ChunkState>(static_cast<std::uint8_t>(i));
            CHECK_FALSE(voxl::isLegalChunkTransition(state, state));
        }
    }

    SECTION("Unloading is terminal")
    {
        for (std::size_t i = 0; i < voxl::kChunkStateCount; ++i) {
            const auto state = static_cast<voxl::ChunkState>(static_cast<std::uint8_t>(i));
            CHECK_FALSE(voxl::isLegalChunkTransition(voxl::ChunkState::Unloading, state));
        }
    }

    SECTION("every state has a name")
    {
        for (std::size_t i = 0; i < voxl::kChunkStateCount; ++i) {
            const auto state = static_cast<voxl::ChunkState>(static_cast<std::uint8_t>(i));
            CHECK(std::string_view{voxl::toString(state)} != std::string_view{"Unknown"});
        }
    }
}

TEST_CASE("tryTransition rejects a state the chunk is not in", "[world][chunk][state]")
{
    const voxl::ChunkPtr chunk = voxl::Chunk::create(voxl::ChunkPos{1, 2, 3});
    REQUIRE(chunk->state() == voxl::ChunkState::Empty);
    CHECK(chunk->position() == voxl::ChunkPos{1, 2, 3});
    CHECK(chunk->originBlock() == voxl::BlockPos{32, 64, 96});

    // Note: tryTransition() asserts on an *illegal* pair, so illegal pairs are
    // exercised through isLegalChunkTransition() above rather than here. What is
    // tested here is the other rejection path - a legal pair whose `expected`
    // state does not match, which is the case that actually happens at runtime
    // when two schedulers race.
    CHECK_FALSE(chunk->tryTransition(voxl::ChunkState::Ready, voxl::ChunkState::Meshing));
    CHECK(chunk->state() == voxl::ChunkState::Empty);

    REQUIRE(chunk->tryTransition(voxl::ChunkState::Empty, voxl::ChunkState::Generating));
    CHECK(chunk->state() == voxl::ChunkState::Generating);
    CHECK(chunk->isBusy());
    CHECK_FALSE(chunk->hasVoxels());

    // A second scheduler arriving late must be told no.
    CHECK_FALSE(chunk->tryTransition(voxl::ChunkState::Empty, voxl::ChunkState::Generating));
    CHECK(chunk->state() == voxl::ChunkState::Generating);

    REQUIRE(chunk->tryTransition(voxl::ChunkState::Generating, voxl::ChunkState::Generated));
    CHECK_FALSE(chunk->isBusy());
    CHECK(chunk->hasVoxels());

    REQUIRE(chunk->tryTransition(voxl::ChunkState::Generated, voxl::ChunkState::Meshing));
    CHECK(chunk->isBusy());
    CHECK(chunk->hasVoxels());

    REQUIRE(chunk->tryTransition(voxl::ChunkState::Meshing, voxl::ChunkState::Meshed));
    REQUIRE(chunk->tryTransition(voxl::ChunkState::Meshed, voxl::ChunkState::Ready));
    CHECK(chunk->state() == voxl::ChunkState::Ready);
    CHECK(chunk->hasVoxels());
    CHECK_FALSE(chunk->isBusy());

    // Remesh loop.
    REQUIRE(chunk->tryTransition(voxl::ChunkState::Ready, voxl::ChunkState::Meshing));
    REQUIRE(chunk->tryTransition(voxl::ChunkState::Meshing, voxl::ChunkState::Meshed));
    REQUIRE(chunk->tryTransition(voxl::ChunkState::Meshed, voxl::ChunkState::Ready));

    REQUIRE(chunk->tryTransition(voxl::ChunkState::Ready, voxl::ChunkState::Unloading));
    CHECK(chunk->state() == voxl::ChunkState::Unloading);
    CHECK_FALSE(chunk->tryTransition(voxl::ChunkState::Ready, voxl::ChunkState::Unloading));

    SECTION("forceState is unconditional, for the unload path and tests")
    {
        const voxl::ChunkPtr other = voxl::Chunk::create(voxl::ChunkPos{});
        other->forceState(voxl::ChunkState::Ready);
        CHECK(other->state() == voxl::ChunkState::Ready);
        other->forceState(voxl::ChunkState::Empty);
        CHECK(other->state() == voxl::ChunkState::Empty);
    }
}

TEST_CASE("only one of many racing threads wins tryTransition", "[world][chunk][threading]")
{
    // Two schedulers can independently decide the same chunk needs generating.
    // The compare-exchange is the sole guarantee that exactly one generation job
    // is queued; if it ever degrades to a load-then-store, two workers write the
    // same voxel array at once and the corruption is unreproducible. Repeated
    // over many chunks with a release gate so the threads collide for real
    // rather than starting staggered.
    constexpr int kThreads = 8;
    constexpr int kRounds  = 64;

    int totalWinners = 0;
    for (int round = 0; round < kRounds; ++round) {
        const voxl::ChunkPtr chunk = voxl::Chunk::create(voxl::ChunkPos{round, 0, 0});

        std::atomic<int>  winners{0};
        std::atomic<int>  arrived{0};
        std::atomic<bool> go{false};

        std::vector<std::thread> threads;
        threads.reserve(static_cast<std::size_t>(kThreads));
        for (int i = 0; i < kThreads; ++i) {
            threads.emplace_back([&chunk, &winners, &arrived, &go] {
                arrived.fetch_add(1, std::memory_order_release);
                while (!go.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                if (chunk->tryTransition(voxl::ChunkState::Empty, voxl::ChunkState::Generating)) {
                    winners.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        while (arrived.load(std::memory_order_acquire) < kThreads) {
            std::this_thread::yield();
        }
        go.store(true, std::memory_order_release);
        for (std::thread& worker : threads) {
            worker.join();
        }

        INFO("round " << round);
        REQUIRE(winners.load(std::memory_order_relaxed) == 1);
        REQUIRE(chunk->state() == voxl::ChunkState::Generating);
        totalWinners += winners.load(std::memory_order_relaxed);
    }
    CHECK(totalWinners == kRounds);
}

// ===========================================================================
//  Chunk bookkeeping
// ===========================================================================

TEST_CASE("chunk voxel writes bump the version and both dirty flags", "[world][chunk]")
{
    const voxl::ChunkPtr chunk = voxl::Chunk::create(voxl::ChunkPos{0, 1, 0});

    CHECK(chunk->isEmpty());
    CHECK(chunk->contentVersion() == 0);
    CHECK(chunk->meshedVersion() == 0);
    CHECK_FALSE(chunk->needsRemesh());
    CHECK_FALSE(chunk->needsSave());

    chunk->setBlock(1, 2, 3, voxl::blocks::Stone);
    CHECK(chunk->getBlock(1, 2, 3) == voxl::blocks::Stone);
    CHECK(chunk->getBlock(voxl::localIndex(1, 2, 3)) == voxl::blocks::Stone);
    CHECK(chunk->getBlock(voxl::BlockPos{1, 2, 3}) == voxl::blocks::Stone);
    CHECK(chunk->contentVersion() == 1);
    CHECK(chunk->needsRemesh());
    CHECK(chunk->needsSave());
    CHECK_FALSE(chunk->isEmpty());

    chunk->clearRemeshFlag();
    chunk->markSaved();
    CHECK_FALSE(chunk->needsRemesh());
    CHECK_FALSE(chunk->needsSave());

    chunk->setBlock(voxl::localIndex(4, 4, 4), voxl::blocks::Dirt);
    CHECK(chunk->contentVersion() == 2);
    CHECK(chunk->needsRemesh());

    SECTION("light writes deliberately do not bump the content version")
    {
        chunk->clearRemeshFlag();
        const std::uint32_t before = chunk->contentVersion();

        chunk->setLight(0, voxl::ChunkStorage::packLight(15, 3));
        chunk->setSunlight(1, 7);
        chunk->setBlockLight(1, 2);
        chunk->fillLight(15, 0);

        CHECK(chunk->contentVersion() == before);
        CHECK_FALSE(chunk->needsRemesh());  // lighting must call markDirty() itself

        chunk->markDirty();
        CHECK(chunk->needsRemesh());
    }

    SECTION("light reads go through to the storage nibbles")
    {
        chunk->setLight(voxl::localIndex(2, 2, 2), voxl::ChunkStorage::packLight(11, 5));
        CHECK(chunk->getLight(voxl::localIndex(2, 2, 2)) ==
              voxl::ChunkStorage::packLight(11, 5));
        CHECK(chunk->getLight(2, 2, 2) == voxl::ChunkStorage::packLight(11, 5));
        CHECK(chunk->getSunlight(voxl::localIndex(2, 2, 2)) == 11);
        CHECK(chunk->getBlockLight(voxl::localIndex(2, 2, 2)) == 5);
    }

    SECTION("direct storage access bypasses the version bump by design")
    {
        // The generator writes through storage() for the bulk operations, so it
        // is responsible for marking the chunk itself. Pinned here because it is
        // a sharp edge, not an accident.
        const std::uint32_t before = chunk->contentVersion();
        chunk->clearRemeshFlag();
        chunk->storage().fill(voxl::blocks::Stone);

        CHECK(chunk->contentVersion() == before);
        CHECK_FALSE(chunk->needsRemesh());
        CHECK(chunk->getBlock(0) == voxl::blocks::Stone);
    }

    SECTION("mesh version and touch frame are plain published counters")
    {
        chunk->setMeshedVersion(chunk->contentVersion());
        CHECK(chunk->meshedVersion() == chunk->contentVersion());

        chunk->setBlock(0, voxl::blocks::Sand);
        CHECK(chunk->meshedVersion() != chunk->contentVersion());

        chunk->setLastTouchedFrame(4321);
        CHECK(chunk->lastTouchedFrame() == 4321);
    }

    SECTION("memory accounting excludes the storage object counted inside Chunk")
    {
        CHECK(chunk->memoryUsageBytes() == sizeof(voxl::Chunk) + chunk->storage().heapBytes());
        CHECK(chunk->memoryUsageBytes() >= sizeof(voxl::Chunk));
    }
}

TEST_CASE("the contract-verification translation units are linked", "[world][storage][contract]")
{
    // ChunkStorage.cpp and Chunk.cpp contain only static_asserts; referencing
    // their anchors is what proves those files are compiled as part of the
    // engine rather than silently dropped from the build.
    CHECK(voxl::detail::kChunkStorageContractVerified);
    CHECK(voxl::detail::kChunkContractVerified);
}
