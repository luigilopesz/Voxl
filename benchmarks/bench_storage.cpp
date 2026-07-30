// Palette-compressed voxel storage: set and get throughput, and resident memory,
// at every bits-per-index tier the representation can be in.
//
// The tiers are reached by populating a section with exactly enough distinct
// block ids to force each width (2 -> 1 bit, 4 -> 2 bits, 16 -> 4, 256 -> 8,
// more -> 16). The 8- and 16-bit tiers use synthetic ids beyond the registry:
// ChunkStorage stores raw BlockId values and never consults the registry, and no
// real section holds hundreds of materials - the point of measuring those tiers
// is to bound the worst case, not to describe terrain.

#include "Cases.hpp"
#include "Fixtures.hpp"

#include "core/Log.hpp"
#include "world/Block.hpp"
#include "world/ChunkStorage.hpp"
#include "world/VoxelTypes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <vector>

namespace voxl::bench {

namespace {

constexpr const char* kGroup = "storage";

/// A dense BlockId array, i.e. what the storage would cost with no compression
/// at all. Every memory figure is reported against this.
constexpr double kRawSectionBytes = static_cast<double>(kChunkVolume) * sizeof(BlockId);

struct Tier {
    const char*   label;
    std::uint32_t distinctIds;
    std::uint8_t  expectedBits;
};

constexpr std::array<Tier, 6> kTiers = {{
    {"uniform", 1, 0},
    {"bits1", 2, 1},
    {"bits2", 4, 2},
    {"bits4", 16, 4},
    {"bits8", 256, 8},
    {"bits16", 300, 16},
}};

/// Deterministic scramble of the voxel indices, built once and shared by every
/// random-access case so all tiers walk the same address pattern.
///
/// A 64-bit LCG rather than std::shuffle with a default_random_engine: the
/// standard engines' exact output is implementation defined, and a benchmark
/// input that differs between toolchains cannot be compared between machines.
const std::vector<std::uint32_t>& scrambledIndices()
{
    static const std::vector<std::uint32_t> indices = [] {
        std::vector<std::uint32_t> values(kChunkVolume);
        for (std::size_t i = 0; i < kChunkVolume; ++i) {
            values[i] = static_cast<std::uint32_t>(i);
        }
        std::uint64_t state = 0x9E3779B97F4A7C15ull;
        for (std::size_t i = kChunkVolume - 1; i > 0; --i) {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            const std::size_t j = static_cast<std::size_t>((state >> 33) % (i + 1));
            const std::uint32_t swap = values[i];
            values[i]                = values[j];
            values[j]                = swap;
        }
        return values;
    }();
    return indices;
}

/// Block id written at voxel `index` for a tier with `distinctIds` materials.
///
/// The stride is a large odd number so consecutive voxels land on different
/// palette slots. A run-length pattern would let the branch predictor learn the
/// palette scan and report a set() cost the real world never sees.
[[nodiscard]] BlockId tierBlockId(std::size_t index, std::uint32_t distinctIds) noexcept
{
    return static_cast<BlockId>(1u + ((index * 2654435761u) % distinctIds));
}

struct StorageFixture {
    ChunkStorage storage;
};

void buildTier(ChunkStorage& storage, const Tier& tier)
{
    storage.fill(tierBlockId(0, tier.distinctIds));
    if (tier.distinctIds <= 1) {
        return;
    }
    for (std::size_t index = 0; index < kChunkVolume; ++index) {
        storage.set(index, tierBlockId(index, tier.distinctIds));
    }
}

void addMemoryCounters(CaseContext& context, const ChunkStorage& storage)
{
    const double heap = static_cast<double>(storage.heapBytes());
    context.counter("bits_per_index", static_cast<double>(storage.bitsPerIndex()));
    context.counter("palette_entries", static_cast<double>(storage.paletteSize()));
    context.counter("heap_bytes", heap, "bytes");
    context.counter("bytes_per_voxel", heap / static_cast<double>(kChunkVolume), "B/voxel");
    if (heap > 0.0) {
        context.counter("compression_vs_raw", kRawSectionBytes / heap, "x vs 64 KiB");
    } else {
        // Zero heap is not "no compression", it is the uniform representation -
        // the whole section held in an 8-byte member. A ratio row here would
        // read as 0x, i.e. the exact opposite of what happened.
        context.note("uniform representation: zero heap bytes, the section is one BlockId "
                     "member; a compression ratio against 64 KiB is unbounded, not zero");
    }
}

void addSetCase(Runner& runner, const Tier& tier)
{
    const std::string name = std::format("set_{}", tier.label);
    if (!runner.selected(kGroup, name)) {
        return;
    }

    auto fixture = std::make_shared<StorageFixture>();

    Case testCase;
    testCase.group      = kGroup;
    testCase.name       = name;
    testCase.unit       = "voxel";
    testCase.opsPerRun  = static_cast<double>(kChunkVolume);
    testCase.sampleRuns = 21;
    testCase.setup      = [fixture, tier](CaseContext& context) {
        buildTier(fixture->storage, tier);
        VOXL_CHECK(fixture->storage.bitsPerIndex() == tier.expectedBits,
                   "tier {} settled at {} bits per index, expected {}", tier.label,
                   fixture->storage.bitsPerIndex(), tier.expectedBits);
        addMemoryCounters(context, fixture->storage);
        if (tier.expectedBits == 0) {
            context.note("uniform: every set() writes the id already resident, so this is the "
                         "early-out that dominates terrain generation's inner loop, not a "
                         "packed write");
        }
    };
    testCase.body = [fixture, tier](CaseContext&) {
        ChunkStorage& storage = fixture->storage;
        for (std::size_t index = 0; index < kChunkVolume; ++index) {
            storage.set(index, tierBlockId(index, tier.distinctIds));
        }
        keep(static_cast<std::uint64_t>(storage.paletteSize()));
    };
    runner.add(std::move(testCase));
}

void addGetCase(Runner& runner, const Tier& tier, bool random)
{
    const std::string name = std::format("get_{}_{}", random ? "random" : "seq", tier.label);
    if (!runner.selected(kGroup, name)) {
        return;
    }

    auto fixture = std::make_shared<StorageFixture>();

    Case testCase;
    testCase.group      = kGroup;
    testCase.name       = name;
    testCase.unit       = "voxel";
    testCase.opsPerRun  = static_cast<double>(kChunkVolume);
    testCase.sampleRuns = 21;
    testCase.setup      = [fixture, tier](CaseContext& context) {
        buildTier(fixture->storage, tier);
        addMemoryCounters(context, fixture->storage);
    };
    if (random) {
        testCase.body = [fixture](CaseContext&) {
            const ChunkStorage&               storage = fixture->storage;
            const std::vector<std::uint32_t>& order   = scrambledIndices();
            std::uint64_t                     sum     = 0;
            for (const std::uint32_t index : order) {
                sum += storage.get(index);
            }
            keep(sum);
        };
    } else {
        testCase.body = [fixture](CaseContext&) {
            const ChunkStorage& storage = fixture->storage;
            std::uint64_t       sum     = 0;
            for (std::size_t index = 0; index < kChunkVolume; ++index) {
                sum += storage.get(index);
            }
            keep(sum);
        };
    }
    runner.add(std::move(testCase));
}

}  // namespace

void registerStorageCases(Runner& runner)
{
    for (const Tier& tier : kTiers) {
        addSetCase(runner, tier);
    }
    for (const Tier& tier : kTiers) {
        addGetCase(runner, tier, /*random=*/false);
    }
    for (const Tier& tier : kTiers) {
        addGetCase(runner, tier, /*random=*/true);
    }
}

}  // namespace voxl::bench
