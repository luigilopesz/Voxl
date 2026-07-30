// Correctness core of the destructible-block feature.
//
// Almost every test in this file is really a test of ONE property: the invariant
// documented at the top of src/world/SubVoxel.hpp. Two containers each hold half
// of the truth about a damaged block - SubVoxelStore knows which sub-voxels are
// present, ChunkStorage knows what the block is - and every code path has to
// leave them agreeing. `checkInvariant()` below is the machine-readable form of
// that paragraph and is asserted after essentially every mutation, because the
// symptoms of breaking it (a block that meshes as solid but has no collision, a
// save file that loads as air) surface far away from the cause.

#include <catch2/catch_test_macros.hpp>

#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/SubVoxel.hpp"
#include "world/VoxelTypes.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <vector>

#include <glm/vec3.hpp>

using voxl::BlockId;
using voxl::Chunk;
using voxl::ChunkPos;
using voxl::kSubVoxelCount;
using voxl::kSubVoxelResolution;
using voxl::SubVoxelEdit;
using voxl::SubVoxelGrid;
using voxl::SubVoxelStore;
using voxl::subVoxelIndex;
using voxl::toSubVoxel;

namespace {

/// Deterministic scatter. A failure must reproduce byte for byte on another
/// machine, so the test carries its own LCG rather than touching rand().
class Lcg {
public:
    explicit constexpr Lcg(std::uint32_t seed) noexcept : m_state(seed) {}

    std::uint32_t next() noexcept
    {
        m_state = m_state * 1664525u + 1013904223u;  // Numerical Recipes constants
        return m_state;
    }

    [[nodiscard]] std::size_t below(std::size_t bound) noexcept
    {
        return static_cast<std::size_t>(next() % static_cast<std::uint32_t>(bound));
    }

private:
    std::uint32_t m_state;
};

/// A deterministic permutation of 0..count-1, used to prove that the order in
/// which sub-voxels are carved never matters.
[[nodiscard]] std::vector<std::size_t> shuffledIndices(std::size_t count, std::uint32_t seed)
{
    std::vector<std::size_t> order(count);
    for (std::size_t i = 0; i < count; ++i) {
        order[i] = i;
    }
    Lcg rng{seed};
    for (std::size_t i = count; i > 1; --i) {
        std::swap(order[i - 1], order[rng.below(i)]);
    }
    return order;
}

/// THE invariant from src/world/SubVoxel.hpp, evaluated over a whole chunk.
///
/// Returns the index of a violating block, or kChunkVolume when the chunk is
/// consistent. Returning the index rather than a bool matters: a broken
/// invariant is always about one specific block, and the index says which.
///
/// Only the blocks WITH an entry need checking. The "no entry" half of the
/// invariant is unconditionally satisfiable - air and whole-solid are both
/// legal - so walking the sparse table instead of all 32768 blocks is not a
/// shortcut, it is the complete statement, and it keeps this cheap enough to
/// call after every one of the 512 carves in the exhaustive tests.
[[nodiscard]] std::size_t invariantViolation(const Chunk& chunk)
{
    std::size_t violation = voxl::kChunkVolume;
    chunk.subVoxels().forEach([&](std::uint16_t index, const SubVoxelGrid& grid) {
        if (violation != voxl::kChunkVolume) {
            return;
        }
        const BlockId id = chunk.getBlock(static_cast<std::size_t>(index));
        if (id == voxl::blocks::Air) {
            violation = index;  // damage recorded against air
            return;
        }
        if (grid.material != id) {
            violation = index;  // the duplicated material drifted from ChunkStorage
            return;
        }
        const std::size_t present = grid.count();
        if (present == 0 || present == kSubVoxelCount) {
            violation = index;  // a uniform block must not have an entry at all
        }
    });
    return violation;
}

void checkInvariant(const Chunk& chunk)
{
    REQUIRE(invariantViolation(chunk) == voxl::kChunkVolume);
}

/// The whole-block replace path, exactly as World::writeBlock must perform it.
///
/// Chunk::setBlock is defined inline in the frozen header and does NOT drop
/// sub-voxel damage, so the erase is the caller's job. This helper is the
/// contract that src/world/World.cpp has to honour; see the integration note.
void replaceWholeBlock(Chunk& chunk, std::size_t index, BlockId id)
{
    chunk.subVoxels().erase(index);
    chunk.setBlock(index, id);
}

[[nodiscard]] std::shared_ptr<Chunk> solidChunk(BlockId fill)
{
    auto chunk = Chunk::create(ChunkPos{0, 0, 0});
    chunk->storage().fill(fill);
    return chunk;
}

constexpr std::size_t kProbe = voxl::localIndex(5, 9, 13);

}  // namespace

// ============================================================== index order ==

TEST_CASE("subVoxelIndex orders x fastest, then z, then y", "[subvoxel]")
{
    // The ordering is a contract shared with voxl::localIndex so that the
    // meshing and lighting sweeps, and the save format, can reuse one loop.
    static_assert(subVoxelIndex(0, 0, 0) == 0);
    static_assert(subVoxelIndex(1, 0, 0) == 1, "x must be the fastest axis");
    static_assert(subVoxelIndex(0, 0, 1) == static_cast<std::size_t>(kSubVoxelResolution),
                  "z must be the second axis");
    static_assert(subVoxelIndex(0, 1, 0) ==
                      static_cast<std::size_t>(kSubVoxelResolution * kSubVoxelResolution),
                  "y must be the slowest axis");
    static_assert(subVoxelIndex(kSubVoxelResolution - 1, kSubVoxelResolution - 1,
                                kSubVoxelResolution - 1) == kSubVoxelCount - 1);

    SECTION("the mapping is a bijection onto 0..511")
    {
        std::vector<int> seen(kSubVoxelCount, 0);
        for (std::int32_t y = 0; y < kSubVoxelResolution; ++y) {
            for (std::int32_t z = 0; z < kSubVoxelResolution; ++z) {
                for (std::int32_t x = 0; x < kSubVoxelResolution; ++x) {
                    const std::size_t index = subVoxelIndex(x, y, z);
                    REQUIRE(index < kSubVoxelCount);
                    ++seen[index];
                }
            }
        }
        REQUIRE(std::count(seen.begin(), seen.end(), 1) == static_cast<std::ptrdiff_t>(kSubVoxelCount));
    }

    SECTION("sub-voxel ordering matches block ordering at the same resolution")
    {
        // Same shape of expression, different extent: if one convention is ever
        // flipped, the two stop agreeing and this catches it.
        for (std::int32_t y = 0; y < kSubVoxelResolution; ++y) {
            for (std::int32_t z = 0; z < kSubVoxelResolution; ++z) {
                for (std::int32_t x = 0; x < kSubVoxelResolution; ++x) {
                    const std::size_t expected = static_cast<std::size_t>(
                        (y * kSubVoxelResolution + z) * kSubVoxelResolution + x);
                    REQUIRE(subVoxelIndex(x, y, z) == expected);
                }
            }
        }
    }
}

TEST_CASE("SubVoxelGrid reports occupancy correctly", "[subvoxel]")
{
    SubVoxelGrid empty;
    REQUIRE(empty.empty());
    REQUIRE_FALSE(empty.full());
    REQUIRE(empty.count() == 0);
    REQUIRE(empty.material == voxl::blocks::Air);

    const SubVoxelGrid solid = SubVoxelGrid::solid(voxl::blocks::Stone);
    REQUIRE(solid.full());
    REQUIRE_FALSE(solid.empty());
    REQUIRE(solid.count() == kSubVoxelCount);
    REQUIRE(solid.material == voxl::blocks::Stone);

    SubVoxelGrid grid = solid;
    grid.clear(subVoxelIndex(3, 4, 5));
    REQUIRE(grid.count() == kSubVoxelCount - 1);
    REQUIRE_FALSE(grid.test(3, 4, 5));
    REQUIRE(grid.test(4, 4, 5));
    grid.set(subVoxelIndex(3, 4, 5));
    REQUIRE(grid.full());
}

// =========================================================== toSubVoxel ==

TEST_CASE("toSubVoxel floors instead of truncating", "[subvoxel]")
{
    SECTION("origin and interior of the first block")
    {
        const auto hit = toSubVoxel(glm::vec3{0.0f, 0.0f, 0.0f});
        REQUIRE(hit.block == voxl::BlockPos{0, 0, 0});
        REQUIRE(hit.sx == 0);
        REQUIRE(hit.sy == 0);
        REQUIRE(hit.sz == 0);
        REQUIRE(hit.index() == 0);
    }

    SECTION("exact sub-voxel boundaries land on the higher sub-voxel")
    {
        // 1/8 is exactly representable, so these are not approximations: the
        // sub-voxel spanning [k/8, (k+1)/8) must contain its lower bound.
        for (std::int32_t k = 0; k < kSubVoxelResolution; ++k) {
            const float coordinate = static_cast<float>(k) / static_cast<float>(kSubVoxelResolution);
            const auto  hit        = toSubVoxel(glm::vec3{coordinate, coordinate, coordinate});
            REQUIRE(hit.block == voxl::BlockPos{0, 0, 0});
            REQUIRE(hit.sx == k);
            REQUIRE(hit.sy == k);
            REQUIRE(hit.sz == k);
        }
    }

    SECTION("exact block boundaries belong to the block above")
    {
        const auto positive = toSubVoxel(glm::vec3{1.0f, 2.0f, 3.0f});
        REQUIRE(positive.block == voxl::BlockPos{1, 2, 3});
        REQUIRE(positive.index() == 0);

        const auto negative = toSubVoxel(glm::vec3{-1.0f, -2.0f, -3.0f});
        REQUIRE(negative.block == voxl::BlockPos{-1, -2, -3});
        REQUIRE(negative.index() == 0);
    }

    SECTION("negative coordinates are not mirrored toward zero")
    {
        // A truncating implementation computes (int)(-0.125 * 8) == -1 and ends
        // up on block 0 sub-voxel 0 or 1. The correct answer is the LAST
        // sub-voxel of the block below, because -0.125 lies in [-0.125, 0).
        const auto justBelowZero = toSubVoxel(glm::vec3{-0.125f, -0.125f, -0.125f});
        REQUIRE(justBelowZero.block == voxl::BlockPos{-1, -1, -1});
        REQUIRE(justBelowZero.sx == kSubVoxelResolution - 1);
        REQUIRE(justBelowZero.sy == kSubVoxelResolution - 1);
        REQUIRE(justBelowZero.sz == kSubVoxelResolution - 1);

        // -0.9 sits in the FIRST sub-voxel of block -1. Truncating the raw
        // product gives (int)(-7.2) == -7, i.e. sub-voxel 1 counted from the
        // wrong end - the mirroring bug this test exists for.
        const auto nearBlockStart = toSubVoxel(glm::vec3{-0.9f, -0.9f, -0.9f});
        REQUIRE(nearBlockStart.block == voxl::BlockPos{-1, -1, -1});
        REQUIRE(nearBlockStart.sx == 0);
        REQUIRE(nearBlockStart.sy == 0);
        REQUIRE(nearBlockStart.sz == 0);

        const auto midBlock = toSubVoxel(glm::vec3{-3.5f, -0.5f, -33.25f});
        REQUIRE(midBlock.block == voxl::BlockPos{-4, -1, -34});
        REQUIRE(midBlock.sx == 4);   // -3.5 - (-4) = 0.5 -> 4
        REQUIRE(midBlock.sy == 4);   // -0.5 - (-1)  = 0.5 -> 4
        REQUIRE(midBlock.sz == 6);   // -33.25 - (-34) = 0.75 -> 6
    }

    SECTION("every result is in range and brackets the input")
    {
        // Swept over a range that straddles zero, where the floor/truncate
        // difference lives. The bracketing check is the definition of the
        // function, restated independently of its implementation.
        for (std::int32_t step = -640; step <= 640; ++step) {
            const float coordinate = static_cast<float>(step) * 0.03125f;  // 1/32, exact
            const auto  hit        = toSubVoxel(glm::vec3{coordinate, coordinate, coordinate});

            REQUIRE(hit.sx >= 0);
            REQUIRE(hit.sx < kSubVoxelResolution);
            REQUIRE(hit.sy == hit.sx);
            REQUIRE(hit.sz == hit.sx);
            REQUIRE(hit.block == voxl::worldToBlockPos(glm::vec3{coordinate, coordinate, coordinate}));

            const float lower = static_cast<float>(hit.block.x) +
                                static_cast<float>(hit.sx) * voxl::kSubVoxelSize;
            REQUIRE(lower <= coordinate);
            REQUIRE(coordinate < lower + voxl::kSubVoxelSize);
        }
    }

    SECTION("index() composes with subVoxelIndex")
    {
        const auto hit = toSubVoxel(glm::vec3{-2.0f + 0.375f, 5.625f, -7.0f + 0.875f});
        REQUIRE(hit.block == voxl::BlockPos{-2, 5, -7});
        REQUIRE(hit.sx == 3);
        REQUIRE(hit.sy == 5);
        REQUIRE(hit.sz == 7);
        REQUIRE(hit.index() == subVoxelIndex(3, 5, 7));
    }
}

// ================================================================== store ==

TEST_CASE("an empty store costs nothing and reports nothing", "[subvoxel]")
{
    SubVoxelStore store;
    REQUIRE(store.empty());
    REQUIRE(store.size() == 0);
    REQUIRE(store.memoryUsageBytes() == 0);
    REQUIRE(store.find(0) == nullptr);
    REQUIRE(store.find(kProbe) == nullptr);
    REQUIRE_FALSE(store.isPartial(kProbe));
    REQUIRE(store.sortedEntries().empty());

    // Erasing an absent entry must be a no-op, not an insertion: the whole-block
    // break path calls it unconditionally for every edit in the world.
    store.erase(kProbe);
    REQUIRE(store.empty());
    REQUIRE(store.memoryUsageBytes() == 0);
}

TEST_CASE("an absent entry means uniform, and the two mutators mirror each other", "[subvoxel]")
{
    // SubVoxelStore cannot see ChunkStorage, so "no entry" is ambiguous to it:
    // the block is either air or whole-and-solid. The split is that remove()
    // resolves it as whole (materialise full, clear one) and add() resolves it
    // as air (materialise empty, set one); Chunk::restoreSubVoxel is what rules
    // out the whole-and-solid case before ever calling add(). If that split is
    // ever changed, the two functions stop being inverses and this fails.
    SECTION("remove() on an absent entry materialises a full grid")
    {
        SubVoxelStore store;
        REQUIRE(store.remove(kProbe, voxl::blocks::Stone, 5) == SubVoxelEdit::Modified);
        const SubVoxelGrid* grid = store.find(kProbe);
        REQUIRE(grid != nullptr);
        REQUIRE(grid->count() == kSubVoxelCount - 1);
        REQUIRE(grid->material == voxl::blocks::Stone);
    }

    SECTION("add() on an absent entry materialises an empty grid")
    {
        SubVoxelStore store;
        REQUIRE(store.add(kProbe, voxl::blocks::Stone, 5) == SubVoxelEdit::Modified);
        const SubVoxelGrid* grid = store.find(kProbe);
        REQUIRE(grid != nullptr);
        REQUIRE(grid->count() == 1);
        REQUIRE(grid->test(5));
        REQUIRE(grid->material == voxl::blocks::Stone);
    }

    SECTION("erase() drops damage without consulting anything else")
    {
        SubVoxelStore store;
        store.remove(kProbe, voxl::blocks::Stone, 5);
        REQUIRE(store.isPartial(kProbe));
        store.erase(kProbe);
        REQUIRE_FALSE(store.isPartial(kProbe));
        REQUIRE(store.find(kProbe) == nullptr);
        REQUIRE(store.empty());
    }
}

TEST_CASE("sortedEntries is deterministic regardless of insertion order", "[subvoxel]")
{
    // std::unordered_map iteration order depends on insertion history, so a save
    // format built on forEach() would emit different bytes for identical worlds.
    const std::vector<std::size_t> keys = {0, 7, 64, 511, 4096, 12345, voxl::kChunkVolume - 1};

    const auto build = [&keys](bool reverse) {
        SubVoxelStore store;
        std::vector<std::size_t> order = keys;
        if (reverse) {
            std::reverse(order.begin(), order.end());
        }
        for (const std::size_t key : order) {
            // The carve is derived from the KEY, not from the loop counter, so
            // the two builds produce identical grids and a key/grid pairing that
            // depended on insertion order would show up as a bit mismatch.
            store.remove(key, voxl::blocks::Stone, key % kSubVoxelCount);
        }
        return store.sortedEntries();
    };

    const auto ascending  = build(false);
    const auto descending = build(true);

    REQUIRE(ascending.size() == keys.size());
    REQUIRE(descending.size() == keys.size());
    for (std::size_t i = 0; i < ascending.size(); ++i) {
        REQUIRE(ascending[i].first == descending[i].first);
        REQUIRE(ascending[i].second.bits == descending[i].second.bits);
        REQUIRE(ascending[i].second.material == descending[i].second.material);
        if (i > 0) {
            REQUIRE(ascending[i - 1].first < ascending[i].first);  // strictly increasing
        }
    }
}

// ============================================================ chunk edits ==

TEST_CASE("carving the first sub-voxel materialises a full grid minus one bit", "[subvoxel]")
{
    const auto chunk = solidChunk(voxl::blocks::Stone);
    REQUIRE_FALSE(chunk->hasSubVoxelDamage());

    const std::size_t sub = subVoxelIndex(2, 3, 4);
    REQUIRE(chunk->breakSubVoxel(kProbe, sub) == SubVoxelEdit::Modified);

    const SubVoxelGrid* grid = chunk->subVoxels().find(kProbe);
    REQUIRE(grid != nullptr);
    REQUIRE(grid->count() == kSubVoxelCount - 1);
    REQUIRE_FALSE(grid->test(sub));
    REQUIRE(grid->material == voxl::blocks::Stone);

    // The block itself is untouched, which is what keeps a chipped block from
    // vanishing, and isBlockWhole() is what tells the mesher to stop culling
    // the neighbouring faces.
    REQUIRE(chunk->getBlock(kProbe) == voxl::blocks::Stone);
    REQUIRE_FALSE(chunk->isBlockWhole(kProbe));
    REQUIRE(chunk->hasSubVoxelDamage());
    REQUIRE(chunk->subVoxels().size() == 1);
    checkInvariant(*chunk);
}

TEST_CASE("carve and restore round-trips back to an intact block", "[subvoxel]")
{
    const auto chunk = solidChunk(voxl::blocks::Dirt);

    for (const std::size_t sub : shuffledIndices(kSubVoxelCount, 0xC0FFEEu)) {
        REQUIRE(chunk->breakSubVoxel(kProbe, sub) == SubVoxelEdit::Modified);
        checkInvariant(*chunk);
        // `material` is ignored for a block that still exists; passing something
        // wrong on purpose proves the block keeps its own id.
        REQUIRE(chunk->restoreSubVoxel(kProbe, sub, voxl::blocks::Glowstone) ==
                SubVoxelEdit::BlockRestored);
        checkInvariant(*chunk);

        REQUIRE(chunk->subVoxels().empty());
        REQUIRE(chunk->isBlockWhole(kProbe));
        REQUIRE(chunk->getBlock(kProbe) == voxl::blocks::Dirt);
    }
}

TEST_CASE("carving the same sub-voxel twice reports Unchanged", "[subvoxel]")
{
    const auto        chunk = solidChunk(voxl::blocks::Stone);
    const std::size_t sub   = subVoxelIndex(1, 1, 1);

    REQUIRE(chunk->breakSubVoxel(kProbe, sub) == SubVoxelEdit::Modified);
    const std::uint32_t version = chunk->contentVersion();
    chunk->clearRemeshFlag();
    chunk->markSaved();

    REQUIRE(chunk->breakSubVoxel(kProbe, sub) == SubVoxelEdit::Unchanged);
    REQUIRE(chunk->subVoxels().find(kProbe)->count() == kSubVoxelCount - 1);

    // An Unchanged edit must not bump the version: doing so would throw away an
    // in-flight mesh job's perfectly valid result on every repeated click.
    REQUIRE(chunk->contentVersion() == version);
    REQUIRE_FALSE(chunk->needsRemesh());
    REQUIRE_FALSE(chunk->needsSave());
    checkInvariant(*chunk);
}

TEST_CASE("restoring an already present sub-voxel reports Unchanged", "[subvoxel]")
{
    const auto chunk = solidChunk(voxl::blocks::Stone);

    // Intact block: every sub-voxel is already there and the store stays empty.
    REQUIRE(chunk->restoreSubVoxel(kProbe, 0, voxl::blocks::Stone) == SubVoxelEdit::Unchanged);
    REQUIRE(chunk->subVoxels().empty());
    REQUIRE(chunk->contentVersion() == 0);

    // Partially damaged block, restoring a sub-voxel that was never carved.
    REQUIRE(chunk->breakSubVoxel(kProbe, subVoxelIndex(0, 0, 0)) == SubVoxelEdit::Modified);
    const std::uint32_t version = chunk->contentVersion();
    REQUIRE(chunk->restoreSubVoxel(kProbe, subVoxelIndex(7, 7, 7), voxl::blocks::Stone) ==
            SubVoxelEdit::Unchanged);
    REQUIRE(chunk->contentVersion() == version);
    checkInvariant(*chunk);
}

TEST_CASE("carving every sub-voxel removes the block and erases the entry", "[subvoxel]")
{
    const auto chunk = solidChunk(voxl::blocks::Sandstone);
    const auto order = shuffledIndices(kSubVoxelCount, 0x5EEDu);

    for (std::size_t i = 0; i < order.size(); ++i) {
        const SubVoxelEdit result = chunk->breakSubVoxel(kProbe, order[i]);
        if (i + 1 < order.size()) {
            REQUIRE(result == SubVoxelEdit::Modified);
            REQUIRE(chunk->subVoxels().size() == 1);
            REQUIRE(chunk->subVoxels().find(kProbe)->count() == kSubVoxelCount - (i + 1));
            REQUIRE(chunk->getBlock(kProbe) == voxl::blocks::Sandstone);
        } else {
            // The last one is a whole-block break: the caller has to treat it
            // exactly like breaking the block by hand, seam neighbours included.
            REQUIRE(result == SubVoxelEdit::BlockRemoved);
        }
        checkInvariant(*chunk);
    }

    REQUIRE(chunk->getBlock(kProbe) == voxl::blocks::Air);
    REQUIRE(chunk->subVoxels().find(kProbe) == nullptr);
    REQUIRE(chunk->subVoxels().empty());
    REQUIRE_FALSE(chunk->hasSubVoxelDamage());
    REQUIRE(chunk->subVoxels().memoryUsageBytes() == 0);

    // Carving air is a no-op, not a resurrection.
    const std::uint32_t version = chunk->contentVersion();
    REQUIRE(chunk->breakSubVoxel(kProbe, 0) == SubVoxelEdit::Unchanged);
    REQUIRE(chunk->contentVersion() == version);
    REQUIRE(chunk->subVoxels().empty());
}

TEST_CASE("restoring every sub-voxel erases the entry and leaves the block solid", "[subvoxel]")
{
    const auto chunk = solidChunk(voxl::blocks::Cobblestone);
    const auto order = shuffledIndices(kSubVoxelCount, 0xBEEFu);

    // Empty the block out first so the restore path starts from air.
    for (const std::size_t sub : order) {
        chunk->breakSubVoxel(kProbe, sub);
    }
    REQUIRE(chunk->getBlock(kProbe) == voxl::blocks::Air);

    const auto restoreOrder = shuffledIndices(kSubVoxelCount, 0xFEEDu);
    for (std::size_t i = 0; i < restoreOrder.size(); ++i) {
        const SubVoxelEdit result =
            chunk->restoreSubVoxel(kProbe, restoreOrder[i], voxl::blocks::Planks);
        if (i + 1 < restoreOrder.size()) {
            REQUIRE(result == SubVoxelEdit::Modified);
            REQUIRE(chunk->subVoxels().size() == 1);
            REQUIRE(chunk->subVoxels().find(kProbe)->count() == i + 1);
            // The first restore into air must give the block a real id, or the
            // store would hold damage against air.
            REQUIRE(chunk->getBlock(kProbe) == voxl::blocks::Planks);
            REQUIRE_FALSE(chunk->isBlockWhole(kProbe));
        } else {
            REQUIRE(result == SubVoxelEdit::BlockRestored);
        }
        checkInvariant(*chunk);
    }

    REQUIRE(chunk->subVoxels().empty());
    REQUIRE(chunk->subVoxels().find(kProbe) == nullptr);
    REQUIRE(chunk->getBlock(kProbe) == voxl::blocks::Planks);
    REQUIRE(chunk->isBlockWhole(kProbe));
    REQUIRE(chunk->subVoxels().memoryUsageBytes() == 0);
}

TEST_CASE("restoring into air with an air material does nothing", "[subvoxel]")
{
    const auto chunk = Chunk::create(ChunkPos{0, 0, 0});  // all air
    REQUIRE(chunk->restoreSubVoxel(kProbe, 0, voxl::blocks::Air) == SubVoxelEdit::Unchanged);
    REQUIRE(chunk->subVoxels().empty());
    REQUIRE(chunk->getBlock(kProbe) == voxl::blocks::Air);
    REQUIRE(chunk->contentVersion() == 0);
    checkInvariant(*chunk);
}

TEST_CASE("entry material tracks the block id across many blocks", "[subvoxel]")
{
    const auto chunk = Chunk::create(ChunkPos{0, 0, 0});

    const BlockId materials[] = {voxl::blocks::Stone, voxl::blocks::Dirt, voxl::blocks::Glass,
                                 voxl::blocks::Wood, voxl::blocks::Ice};
    std::vector<std::size_t> indices;
    for (std::size_t m = 0; m < std::size(materials); ++m) {
        const std::size_t index = voxl::localIndex(static_cast<std::int32_t>(m) * 3, 1, 2);
        indices.push_back(index);
        chunk->setBlock(index, materials[m]);
    }

    for (std::size_t m = 0; m < std::size(materials); ++m) {
        REQUIRE(chunk->breakSubVoxel(indices[m], subVoxelIndex(0, 0, 0)) == SubVoxelEdit::Modified);
    }
    checkInvariant(*chunk);

    REQUIRE(chunk->subVoxels().size() == std::size(materials));
    for (std::size_t m = 0; m < std::size(materials); ++m) {
        const SubVoxelGrid* grid = chunk->subVoxels().find(indices[m]);
        REQUIRE(grid != nullptr);
        REQUIRE(grid->material == materials[m]);
        REQUIRE(grid->material == chunk->getBlock(indices[m]));
    }

    // forEach must visit exactly the partial blocks and nothing else.
    std::size_t visited = 0;
    chunk->subVoxels().forEach([&](std::uint16_t index, const SubVoxelGrid& grid) {
        ++visited;
        REQUIRE(grid.material == chunk->getBlock(index));
        REQUIRE(grid.count() == kSubVoxelCount - 1);
    });
    REQUIRE(visited == std::size(materials));
}

TEST_CASE("replacing a whole block discards its damage", "[subvoxel]")
{
    const auto chunk = solidChunk(voxl::blocks::Stone);

    SECTION("replaced with another solid block")
    {
        REQUIRE(chunk->breakSubVoxel(kProbe, subVoxelIndex(4, 4, 4)) == SubVoxelEdit::Modified);
        REQUIRE(chunk->hasSubVoxelDamage());

        replaceWholeBlock(*chunk, kProbe, voxl::blocks::Planks);

        // Keeping the old grid would leave Stone damage on a Planks block: the
        // material clause of the invariant, and visibly the wrong texture.
        REQUIRE(chunk->subVoxels().empty());
        REQUIRE(chunk->getBlock(kProbe) == voxl::blocks::Planks);
        REQUIRE(chunk->isBlockWhole(kProbe));
        checkInvariant(*chunk);
    }

    SECTION("broken outright while partially damaged")
    {
        for (std::size_t i = 0; i < 100; ++i) {
            chunk->breakSubVoxel(kProbe, i);
        }
        REQUIRE(chunk->subVoxels().find(kProbe)->count() == kSubVoxelCount - 100);

        replaceWholeBlock(*chunk, kProbe, voxl::blocks::Air);

        REQUIRE(chunk->subVoxels().empty());
        REQUIRE(chunk->getBlock(kProbe) == voxl::blocks::Air);
        checkInvariant(*chunk);
    }
}

TEST_CASE("a sub-voxel edit dirties the chunk exactly like setBlock", "[subvoxel]")
{
    const auto chunk = solidChunk(voxl::blocks::Stone);
    chunk->clearRemeshFlag();
    chunk->markSaved();
    const std::uint32_t before = chunk->contentVersion();

    REQUIRE(chunk->breakSubVoxel(kProbe, subVoxelIndex(2, 2, 2)) == SubVoxelEdit::Modified);

    // Without this the carve is invisible until something unrelated dirties the
    // chunk, and it is silently lost by any future save.
    REQUIRE(chunk->needsRemesh());
    REQUIRE(chunk->needsSave());
    REQUIRE(chunk->contentVersion() == before + 1);

    chunk->clearRemeshFlag();
    chunk->markSaved();
    REQUIRE(chunk->restoreSubVoxel(kProbe, subVoxelIndex(2, 2, 2), voxl::blocks::Stone) ==
            SubVoxelEdit::BlockRestored);
    REQUIRE(chunk->needsRemesh());
    REQUIRE(chunk->needsSave());
    REQUIRE(chunk->contentVersion() == before + 2);
}

TEST_CASE("many blocks can be damaged independently", "[subvoxel]")
{
    const auto chunk = solidChunk(voxl::blocks::Stone);

    // Scattered damage, then verify each block sees only its own carves. A
    // key-collision bug in the uint16 store shows up here and nowhere else.
    Lcg                      rng{0x1234u};
    std::vector<std::size_t> blockIndices;
    for (std::size_t i = 0; i < 64; ++i) {
        blockIndices.push_back(rng.below(voxl::kChunkVolume));
    }
    std::sort(blockIndices.begin(), blockIndices.end());
    blockIndices.erase(std::unique(blockIndices.begin(), blockIndices.end()), blockIndices.end());

    for (std::size_t i = 0; i < blockIndices.size(); ++i) {
        const std::size_t carves = 1 + (i % 7);
        for (std::size_t c = 0; c < carves; ++c) {
            REQUIRE(chunk->breakSubVoxel(blockIndices[i], c * 13) == SubVoxelEdit::Modified);
        }
    }
    checkInvariant(*chunk);
    REQUIRE(chunk->subVoxels().size() == blockIndices.size());

    for (std::size_t i = 0; i < blockIndices.size(); ++i) {
        const SubVoxelGrid* grid = chunk->subVoxels().find(blockIndices[i]);
        REQUIRE(grid != nullptr);
        REQUIRE(grid->count() == kSubVoxelCount - (1 + (i % 7)));
    }

    const auto entries = chunk->subVoxels().sortedEntries();
    REQUIRE(entries.size() == blockIndices.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        REQUIRE(entries[i].first == static_cast<std::uint16_t>(blockIndices[i]));
    }
}

// ================================================================= memory ==

TEST_CASE("an undamaged chunk pays nothing for sub-voxels", "[subvoxel]")
{
    // THE property the whole virtual-grid design rests on: an intact block, and
    // therefore an intact chunk, costs exactly zero beyond what it cost before
    // this feature existed. If this ever regresses, a thousand resident chunks
    // start carrying 16 MB of occupancy bitmasks each.
    const auto chunk = Chunk::create(ChunkPos{0, 0, 0});

    const auto storeAddsNothing = [&chunk] {
        REQUIRE_FALSE(chunk->hasSubVoxelDamage());
        REQUIRE(chunk->subVoxels().empty());
        REQUIRE(chunk->subVoxels().size() == 0);
        REQUIRE(chunk->subVoxels().memoryUsageBytes() == 0);
        REQUIRE(chunk->memoryUsageBytes() == sizeof(Chunk) + chunk->storage().heapBytes());
    };

    SECTION("all air")
    {
        REQUIRE(chunk->storage().heapBytes() == 0);
        storeAddsNothing();
    }

    SECTION("fully generated terrain, never damaged")
    {
        // Enough distinct blocks to force the palette to grow and the light
        // bytes to materialise, so storage() is genuinely allocating and the
        // store's contribution is the only thing being isolated.
        Lcg rng{0xA11Au};
        for (std::size_t i = 0; i < voxl::kChunkVolume; ++i) {
            chunk->setBlock(i, static_cast<BlockId>(
                                   1u + rng.below(static_cast<std::size_t>(voxl::blocks::Count) - 1u)));
        }
        chunk->setLight(kProbe, 0x0Fu);
        REQUIRE(chunk->storage().heapBytes() > 0);

        // Reading sub-voxel state for every block must not create entries: a
        // find() that inserted (operator[] instead of find()) would turn one
        // mesh pass into 32768 grids. Counted rather than asserted per block so
        // a failure is one message, not 32768.
        std::size_t materialised = 0;
        for (std::size_t i = 0; i < voxl::kChunkVolume; ++i) {
            if (chunk->subVoxels().find(i) != nullptr || !chunk->isBlockWhole(i)) {
                ++materialised;
            }
        }
        REQUIRE(materialised == 0);
        storeAddsNothing();
    }
}

TEST_CASE("damage costs are proportional to damaged blocks only", "[subvoxel]")
{
    const auto        chunk = solidChunk(voxl::blocks::Stone);
    const std::size_t base  = chunk->memoryUsageBytes();
    REQUIRE(chunk->subVoxels().memoryUsageBytes() == 0);

    chunk->breakSubVoxel(kProbe, 0);
    const std::size_t oneEntry = chunk->subVoxels().memoryUsageBytes();
    REQUIRE(oneEntry > 0);
    REQUIRE(chunk->memoryUsageBytes() > base);

    // A second carve on the SAME block must not add an entry - that is what
    // makes chipping a block away cost one grid rather than 512.
    chunk->breakSubVoxel(kProbe, 1);
    REQUIRE(chunk->subVoxels().size() == 1);
    REQUIRE(chunk->subVoxels().memoryUsageBytes() == oneEntry);

    chunk->breakSubVoxel(kProbe + 1, 0);
    REQUIRE(chunk->subVoxels().size() == 2);
    REQUIRE(chunk->subVoxels().memoryUsageBytes() == 2 * oneEntry);

    // And repairing a block hands the memory back.
    chunk->restoreSubVoxel(kProbe, 0, voxl::blocks::Stone);
    chunk->restoreSubVoxel(kProbe, 1, voxl::blocks::Stone);
    chunk->restoreSubVoxel(kProbe + 1, 0, voxl::blocks::Stone);
    REQUIRE(chunk->subVoxels().empty());
    REQUIRE(chunk->subVoxels().memoryUsageBytes() == 0);
    REQUIRE(chunk->memoryUsageBytes() == base);
    checkInvariant(*chunk);
}

TEST_CASE("clear() drops every entry", "[subvoxel]")
{
    const auto chunk = solidChunk(voxl::blocks::Stone);
    for (std::size_t i = 0; i < 32; ++i) {
        chunk->breakSubVoxel(i, 0);
    }
    REQUIRE(chunk->subVoxels().size() == 32);

    // clear() abandons the ChunkStorage side deliberately, so it is only legal
    // where the voxels are being replaced wholesale (regeneration, LOD switch,
    // load-from-disk). Here the blocks stay solid, which is exactly the state
    // "no entry means whole" describes.
    chunk->subVoxels().clear();
    REQUIRE(chunk->subVoxels().empty());
    REQUIRE(chunk->subVoxels().memoryUsageBytes() == 0);
    checkInvariant(*chunk);
}
