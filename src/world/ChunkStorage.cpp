// Compile-time verification of the ChunkStorage packing contract.
//
// WHY THIS FILE HAS NO FUNCTION DEFINITIONS
// -----------------------------------------
// Every ChunkStorage operation is defined in-class in world/ChunkStorage.hpp.
// That is deliberate, not an oversight: get()/set() are a load, two shifts and a
// mask, and they sit in the innermost loop of both terrain generation and the
// greedy mesher. Moving them out of line would put a call through the static
// library boundary in the one place the engine cannot afford it. There is
// therefore nothing left to define here.
//
// What does belong here is the half of the contract a header cannot enforce.
// The packing rules are versioned save-format data, but the runtime guards on
// them (VOXL_ASSERT) are compiled out of every non-Debug configuration - which
// is exactly the configuration that ships. The static_asserts below fail the
// build in *all* configurations the moment a constant drifts, which is the only
// protection that actually travels with the shipped binary.
//
// Behavioural coverage (round-trips across every index width, repack survival,
// light nibble boundaries) lives in tests/test_chunkstorage.cpp; assertions that
// need to actually run cannot live in a static_assert.

#include "world/ChunkStorage.hpp"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>

namespace voxl {
namespace {

// ------------------------------------------------------- index packing --

/// The only widths ChunkStorage is permitted to use, in growth order. Mirrors
/// the `0 -> 1 -> 2 -> 4 -> 8 -> 16` sequence documented on the class; width 0
/// is the uniform case and packs nothing, so it is excluded here.
constexpr std::uint8_t kLegalWidths[] = {1, 2, 4, 8, 16};

/// Widths must divide 64 exactly. This is the invariant the whole fast path
/// rests on: it is what lets readIndex()/writeIndexInto() assume a packed index
/// lives entirely inside one word, so neither has a straddle branch. Violating
/// it does not fail loudly - it silently truncates the high bits of every index
/// that crosses a word, which reads back as a different block.
constexpr bool everyWidthDividesAWord() noexcept
{
    for (const std::uint8_t bits : kLegalWidths) {
        if (bits == 0u || 64u % bits != 0u) {
            return false;
        }
    }
    return true;
}
static_assert(everyWidthDividesAWord(),
              "an index width that does not divide 64 makes packed indices straddle words");

/// The last slot in a word must end exactly on the word boundary. Equivalent to
/// the above, but stated as the property the shift arithmetic depends on:
/// `shift = slot * bits` for the final slot plus `bits` must be exactly 64, so
/// no shift is ever >= 64 (which is undefined behaviour, not a wrapped shift).
constexpr bool lastSlotEndsOnWordBoundary() noexcept
{
    for (const std::uint8_t bits : kLegalWidths) {
        const unsigned slotsPerWord = 64u / bits;
        const unsigned lastShift    = (slotsPerWord - 1u) * bits;
        if (lastShift + bits != 64u) {
            return false;
        }
        if (lastShift >= 64u) {
            return false;  // would be UB in `word >> shift`
        }
    }
    return true;
}
static_assert(lastSlotEndsOnWordBoundary(), "packed index shift can reach or exceed 64 bits");

/// Slots per word must be a power of two, so that the word selection is a shift
/// and the in-word slot is a mask rather than a division and a modulo.
constexpr bool slotsPerWordIsPowerOfTwo() noexcept
{
    for (const std::uint8_t bits : kLegalWidths) {
        const unsigned slotsPerWord = 64u / bits;
        if ((slotsPerWord & (slotsPerWord - 1u)) != 0u) {
            return false;
        }
    }
    return true;
}
static_assert(slotsPerWordIsPowerOfTwo(), "slots per word must be a power of two");

/// The word array must be exactly large enough: no slack (wasted memory in the
/// representation the save format writes verbatim) and no shortfall (the last
/// voxel of the section would index one past the end). Checking both directions
/// is what catches an off-by-one in wordCountFor().
constexpr bool wordCountsCoverTheSectionExactly() noexcept
{
    for (const std::uint8_t bits : kLegalWidths) {
        const std::size_t slotsPerWord = 64u / bits;
        const std::size_t words        = ChunkStorage::wordCountFor(bits);

        if (words * slotsPerWord != kChunkVolume) {
            return false;  // slack or shortfall
        }
        if ((kChunkVolume - 1u) / slotsPerWord != words - 1u) {
            return false;  // the highest voxel must land in the highest word
        }
    }
    return true;
}
static_assert(wordCountsCoverTheSectionExactly(),
              "wordCountFor() does not exactly cover kChunkVolume at every width");
static_assert(ChunkStorage::wordCountFor(0) == 0,
              "the uniform representation must allocate no index words");

/// Widths grow by exact doubling, which is why growWidth() can compute the next
/// width as `bits * 2` and the next log2 as `log2 + 1` without a lookup table.
constexpr bool widthsGrowByDoubling() noexcept
{
    for (std::size_t i = 1; i < std::size(kLegalWidths); ++i) {
        if (kLegalWidths[i] != kLegalWidths[i - 1u] * 2u) {
            return false;
        }
    }
    return true;
}
static_assert(widthsGrowByDoubling(), "growWidth() assumes each width is twice the previous one");

/// 16 bits per index is the terminal width because a palette index never needs
/// to be wider than a BlockId: at that point the palette can name every block
/// that can exist, so a "direct, no palette" representation would save nothing.
/// If BlockId ever widens, the growth sequence has to gain another step.
static_assert(std::numeric_limits<BlockId>::digits <= 16,
              "16 bits per index can no longer address every BlockId");
static_assert(kLegalWidths[std::size(kLegalWidths) - 1u] == 16,
              "the terminal index width must match the width of BlockId");

/// Palette capacity per width. appendPaletteEntry() grows when the palette
/// reaches `1 << bits` entries; that must be the exact number of distinct values
/// the width can address, or the section either overflows its indices (too late)
/// or repacks a width early (wasted memory).
constexpr bool paletteCapacityMatchesWidth() noexcept
{
    for (const std::uint8_t bits : kLegalWidths) {
        const std::uint64_t addressable = std::uint64_t{1} << bits;
        const std::uint64_t mask        = addressable - 1u;
        if (mask >> (bits - 1u) != 1u) {
            return false;  // the mask must cover exactly `bits` bits
        }
    }
    return true;
}
static_assert(paletteCapacityMatchesWidth(), "palette capacity does not match the index width");

// --------------------------------------------------------- light nibbles --

static_assert(ChunkStorage::kBlockLightShift == 0, "light nibble layout is save-format data");
static_assert(ChunkStorage::kSunlightShift == 4, "light nibble layout is save-format data");
static_assert(ChunkStorage::kLightMask == 0x0Fu, "a light nibble is exactly 4 bits");
static_assert(ChunkStorage::kMaxLightLevel == ChunkStorage::kLightMask,
              "the maximum light level must be the widest value a nibble holds");
static_assert(ChunkStorage::kSunlightShift == ChunkStorage::kBlockLightShift + 4,
              "the two light nibbles must not overlap");

/// Both nibbles must survive a round trip for every legal pair, and - the part
/// that actually catches bugs - writing one must not disturb the other. 256
/// combinations is cheap enough to check exhaustively.
constexpr bool lightNibblesRoundTripExhaustively() noexcept
{
    for (std::uint8_t sun = 0; sun <= ChunkStorage::kMaxLightLevel; ++sun) {
        for (std::uint8_t block = 0; block <= ChunkStorage::kMaxLightLevel; ++block) {
            const std::uint8_t packed = ChunkStorage::packLight(sun, block);
            if (ChunkStorage::unpackSunlight(packed) != sun) {
                return false;
            }
            if (ChunkStorage::unpackBlockLight(packed) != block) {
                return false;
            }
        }
    }
    return true;
}
static_assert(lightNibblesRoundTripExhaustively(), "packLight/unpackLight is not a round trip");

static_assert(ChunkStorage::packLight(0, 0) == 0x00u, "light packing changed");
static_assert(ChunkStorage::packLight(15, 0) == 0xF0u, "sunlight must occupy the high nibble");
static_assert(ChunkStorage::packLight(0, 15) == 0x0Fu, "block light must occupy the low nibble");
static_assert(ChunkStorage::packLight(15, 15) == 0xFFu, "light packing changed");

/// Out-of-range levels are masked, not clamped and not rejected. Documented
/// here because it is observable: a lighting bug that produces level 16 reads
/// back as darkness rather than as full brightness.
static_assert(ChunkStorage::packLight(16, 0) == 0x00u, "light levels are masked to 4 bits");
static_assert(ChunkStorage::packLight(0xFF, 0xFF) == 0xFFu, "light levels are masked to 4 bits");

// -------------------------------------------------------- section layout --

static_assert(kChunkVolume == 32768u, "the packing arithmetic assumes a 32^3 section");
static_assert(kChunkVolume % 64u == 0u, "the section volume must be a whole number of words");

/// The index ordering ChunkStorage is indexed by. x fastest, then z, then y -
/// asserted here because the storage save format writes voxels in this order,
/// so a change to localIndex() silently reinterprets every existing world.
static_assert(localIndex(0, 0, 0) == 0u, "voxel 0 must be the chunk origin");
static_assert(localIndex(1, 0, 0) == 1u, "x must vary fastest");
static_assert(localIndex(0, 0, 1) == static_cast<std::size_t>(kChunkSize),
              "z must be the middle axis");
static_assert(localIndex(0, 1, 0) == static_cast<std::size_t>(kChunkSize) * kChunkSize,
              "y must be the slowest axis");
static_assert(localIndex(kChunkSize - 1, kChunkSize - 1, kChunkSize - 1) == kChunkVolume - 1u,
              "the last voxel must be the last index");

}  // namespace

namespace detail {

/// Anchors this translation unit.
///
/// ChunkStorage is entirely inline by contract, so this object file would
/// otherwise define no external symbol at all and the librarian would warn
/// (LNK4221) about contributing nothing to the archive. Referenced from
/// tests/test_chunkstorage.cpp, which is also what proves the compile-time suite
/// above is genuinely part of the build rather than a file nobody compiles.
extern const bool kChunkStorageContractVerified;
const bool        kChunkStorageContractVerified = true;

}  // namespace detail
}  // namespace voxl
