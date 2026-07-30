#pragma once

// Destructible sub-voxel blocks.
//
// THIS HEADER IS A CONTRACT. Meshing, collision, raycasting, interaction and
// persistence all agree on the representation and, more importantly, on the
// INVARIANT below.
//
// Every non-air block conceptually contains an 8x8x8 grid of sub-voxels. Storing
// that densely is not an option: 32^3 blocks x 512 sub-voxels is 16.7 million
// bits per chunk section even as a pure bitmask, and the world holds a thousand
// sections at a time.
//
// So the grid is VIRTUAL. A block carries no sub-voxel data at all until the
// player actually damages it. `SubVoxelStore` is a sparse side table holding an
// entry only for blocks that are partially destroyed - which is a handful per
// chunk in practice, and zero for freshly generated terrain. An intact block
// costs nothing extra and still meshes as one greedy-merged cube, exactly as it
// did before this feature existed.
//
// ---------------------------------------------------------------------------
//  THE INVARIANT
//
//  For a block at local index i with id B = storage.get(i):
//
//    * no entry in the store          <=>  the block is UNIFORM: either B is air
//                                          (no sub-voxels) or B is solid and all
//                                          512 sub-voxels are present.
//    * an entry exists                <=>  0 < popcount < 512, and entry.material
//                                          equals B, and B is not air.
//
//  Every mutating operation restores this. Clearing the last set bit erases the
//  entry and sets the block to air; setting the last clear bit erases the entry
//  and leaves the block solid. Code that reads sub-voxels must therefore always
//  fall back to "derive from the block id" when there is no entry, never assume
//  an entry exists.
// ---------------------------------------------------------------------------

#include "world/Block.hpp"
#include "world/VoxelTypes.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace voxl {

/// Sub-voxels along one edge of a block. Must be a power of two so the
/// coordinate split is a shift and a mask.
inline constexpr std::int32_t kSubVoxelResolution = 8;
inline constexpr std::int32_t kSubVoxelMask       = kSubVoxelResolution - 1;
inline constexpr std::int32_t kSubVoxelShift      = 3;
inline constexpr std::size_t  kSubVoxelCount      = 512;  // 8^3
inline constexpr std::size_t  kSubVoxelWords      = kSubVoxelCount / 64;

/// Size of one sub-voxel in world units.
inline constexpr float kSubVoxelSize = 1.0f / static_cast<float>(kSubVoxelResolution);

/// Index of a sub-voxel within its block.
///
/// ORDERING MATCHES VoxelTypes::localIndex: x varies fastest, then z, then y.
/// Keeping the two conventions identical means the meshing and lighting sweeps
/// read sub-voxels in the same direction they read blocks, and the save format
/// can reuse the same loop.
[[nodiscard]] constexpr std::size_t subVoxelIndex(std::int32_t x, std::int32_t y,
                                                  std::int32_t z) noexcept
{
    return static_cast<std::size_t>((y * kSubVoxelResolution + z) * kSubVoxelResolution + x);
}

[[nodiscard]] constexpr bool isSubVoxelPos(std::int32_t x, std::int32_t y, std::int32_t z) noexcept
{
    return x >= 0 && x < kSubVoxelResolution && y >= 0 && y < kSubVoxelResolution &&
           z >= 0 && z < kSubVoxelResolution;
}

/// Occupancy of one partially destroyed block: 512 bits plus its material.
///
/// 66 bytes, trivially copyable. Not thread safe; the owning Chunk provides the
/// synchronisation, exactly as it does for ChunkStorage.
struct SubVoxelGrid {
    /// Bit `subVoxelIndex(x,y,z)` set means that sub-voxel is present.
    std::array<std::uint64_t, kSubVoxelWords> bits{};

    /// Always equal to the parent block's id in ChunkStorage. Duplicated here so
    /// the mesher can emit geometry from the grid alone without a second lookup.
    BlockId material = blocks::Air;

    [[nodiscard]] constexpr bool test(std::size_t index) const noexcept
    {
        return (bits[index >> 6] & (1ull << (index & 63u))) != 0;
    }

    [[nodiscard]] constexpr bool test(std::int32_t x, std::int32_t y, std::int32_t z) const noexcept
    {
        return test(subVoxelIndex(x, y, z));
    }

    constexpr void set(std::size_t index) noexcept { bits[index >> 6] |= (1ull << (index & 63u)); }
    constexpr void clear(std::size_t index) noexcept { bits[index >> 6] &= ~(1ull << (index & 63u)); }

    /// Number of sub-voxels still present, 0..512.
    [[nodiscard]] constexpr std::size_t count() const noexcept
    {
        std::size_t total = 0;
        for (std::uint64_t word : bits) {
            total += static_cast<std::size_t>(std::popcount(word));
        }
        return total;
    }

    [[nodiscard]] constexpr bool full() const noexcept
    {
        for (std::uint64_t word : bits) {
            if (word != ~0ull) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] constexpr bool empty() const noexcept
    {
        for (std::uint64_t word : bits) {
            if (word != 0ull) {
                return false;
            }
        }
        return true;
    }

    /// A grid with every sub-voxel present - the state a block enters the store
    /// in, immediately before the first one is carved out of it.
    [[nodiscard]] static constexpr SubVoxelGrid solid(BlockId material) noexcept
    {
        SubVoxelGrid grid;
        grid.bits.fill(~0ull);
        grid.material = material;
        return grid;
    }
};

// 64 bytes of bitmask plus a 2-byte material, padded to the array's 8-byte
// alignment: 72 on every target we build for.
static_assert(sizeof(SubVoxelGrid) <= 72, "sub-voxel grid should stay compact");

/// Result of a sub-voxel edit, so callers know how much has to be rebuilt.
enum class SubVoxelEdit : std::uint8_t {
    /// Nothing changed; the sub-voxel was already in the requested state.
    Unchanged = 0,
    /// A sub-voxel changed but the block is still partial. Remesh only.
    Modified = 1,
    /// The last sub-voxel was removed; the block became air. The caller must
    /// treat this exactly like a whole-block break, including seam neighbours.
    BlockRemoved = 2,
    /// The block went from partial back to completely full and left the store.
    BlockRestored = 3,
};

/// Sparse per-chunk table of partially destroyed blocks.
///
/// NOT THREAD SAFE. Owned by a Chunk, which supplies the synchronisation. Reads
/// from a mesh job go through the chunk's neighbourhood snapshot, and a write
/// while any neighbour is meshing is a use-after-free for exactly the reason
/// documented on World::isEditBlocked - the same rule applies here.
class SubVoxelStore {
public:
    /// One damaged block. Public so the mesher and the save path can iterate the
    /// backing storage directly instead of copying it.
    struct Entry {
        std::uint16_t blockIndex = 0;
        SubVoxelGrid  grid{};
    };

    /// The grid for a block, or nullptr when the block is uniform. Callers MUST
    /// handle nullptr by falling back to the block id; see the invariant above.
    [[nodiscard]] const SubVoxelGrid* find(std::size_t blockIndex) const noexcept
    {
        const auto it = lowerBound(static_cast<std::uint16_t>(blockIndex));
        return (it != m_grids.end() && it->blockIndex == blockIndex) ? &it->grid : nullptr;
    }

    [[nodiscard]] bool isPartial(std::size_t blockIndex) const noexcept
    {
        return find(blockIndex) != nullptr;
    }

    [[nodiscard]] bool empty() const noexcept { return m_grids.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return m_grids.size(); }

    /// Removes one sub-voxel from the block, materialising a full grid first if
    /// the block is currently uniform. `blockId` is the block's current id and
    /// must not be air.
    ///
    /// Returns what happened so the caller can decide whether a whole-block
    /// break needs to be applied to ChunkStorage. This class deliberately does
    /// NOT touch ChunkStorage itself - keeping the two in step is the Chunk's
    /// job, and splitting it here would give two owners of one invariant.
    SubVoxelEdit remove(std::size_t blockIndex, BlockId blockId, std::size_t subIndex);

    /// Restores one sub-voxel. Returns BlockRestored when the block became whole
    /// again and left the store.
    SubVoxelEdit add(std::size_t blockIndex, BlockId blockId, std::size_t subIndex);

    /// Drops the entry without touching ChunkStorage. For the whole-block paths:
    /// breaking or replacing a partially destroyed block discards its damage.
    void erase(std::size_t blockIndex);

    /// Drops every entry AND releases the buffer. Freeing rather than merely
    /// clearing matters: a chunk that was damaged and then regenerated would
    /// otherwise keep its allocation for as long as it stays resident, which is
    /// the cost this container was chosen to avoid.
    void clear() noexcept { std::vector<Entry>{}.swap(m_grids); }

    /// Visits every partial block as `fn(std::uint16_t blockIndex, const SubVoxelGrid&)`,
    /// in ascending block-index order.
    template <typename Fn>
    void forEach(Fn&& fn) const
    {
        for (const Entry& entry : m_grids) {
            fn(entry.blockIndex, entry.grid);
        }
    }

    /// Direct access to the backing storage, already in ascending index order.
    [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return m_grids; }

    /// Heap cost of the damage in this chunk.
    ///
    /// Proportional to the number of damaged blocks, which is the figure that
    /// answers the question actually being asked - "what is destruction costing
    /// us?" - and it is zero for an undamaged chunk. The vector's unused capacity
    /// is excluded: it is bounded by the growth factor, and `releaseIfEmpty`
    /// hands the whole buffer back the moment the last entry goes, so the slack
    /// can never outlive the damage that caused it.
    [[nodiscard]] std::size_t memoryUsageBytes() const noexcept
    {
        return m_grids.size() * sizeof(Entry);
    }

    /// Stable, ordered snapshot for serialisation.
    ///
    /// A save format whose byte order depends on container iteration order would
    /// produce different files for identical worlds and defeat any checksum
    /// comparison. Kept as an explicit call even though the backing store is now
    /// sorted, so callers cannot silently come to depend on that being true.
    [[nodiscard]] std::vector<std::pair<std::uint16_t, SubVoxelGrid>> sortedEntries() const;

private:
    /// A SORTED VECTOR, NOT A HASH MAP.
    ///
    /// This container exists once per chunk and the world holds ten thousand
    /// chunks, almost none of which are damaged - so the cost that matters is the
    /// cost of being EMPTY. MSVC's std::unordered_map eagerly allocates its
    /// bucket list on construction: measured at 2 allocations and 224 bytes for
    /// an empty map, which across 10k resident chunks is 2.2 MB of heap and 20k
    /// allocations to represent "nothing is damaged". An empty vector is 0 bytes
    /// and 0 allocations, which is what "an intact block costs nothing" requires.
    ///
    /// It is also safer. A mesh job walks this container on a worker thread
    /// through a chunk snapshot; a hash map REHASHES on insert, invalidating
    /// every reference at an unpredictable moment. A vector only reallocates on
    /// growth, and the same isEditBlocked rule that already guards ChunkStorage
    /// covers it.
    ///
    /// The asymptotics are worse - binary-search lookup and O(n) insert against
    /// O(1) - and irrelevant: n is the number of damaged blocks in one chunk,
    /// typically single digits, where a linear memmove over contiguous memory
    /// beats a pointer chase.
    ///
    /// uint16 key: a chunk holds 32768 blocks, so a local index always fits.
    std::vector<Entry> m_grids;

    [[nodiscard]] std::vector<Entry>::const_iterator lowerBound(std::uint16_t key) const noexcept
    {
        return std::lower_bound(m_grids.begin(), m_grids.end(), key,
                                [](const Entry& entry, std::uint16_t value) noexcept {
                                    return entry.blockIndex < value;
                                });
    }

    [[nodiscard]] std::vector<Entry>::iterator lowerBound(std::uint16_t key) noexcept
    {
        return std::lower_bound(m_grids.begin(), m_grids.end(), key,
                                [](const Entry& entry, std::uint16_t value) noexcept {
                                    return entry.blockIndex < value;
                                });
    }

    /// Returns the buffer to the allocator once the last entry is gone, so a
    /// fully repaired block leaves no trace. Called from every erase path.
    void releaseIfEmpty() noexcept
    {
        if (m_grids.empty()) {
            std::vector<Entry>{}.swap(m_grids);
        }
    }
};

/// Splits a world position into the block it lies in and the sub-voxel within
/// that block. The fractional part is floored, so negative coordinates land on
/// the correct sub-voxel rather than being mirrored toward zero.
struct SubVoxelHit {
    BlockPos     block{};
    std::int32_t sx = 0;
    std::int32_t sy = 0;
    std::int32_t sz = 0;

    [[nodiscard]] constexpr std::size_t index() const noexcept { return subVoxelIndex(sx, sy, sz); }
};

[[nodiscard]] SubVoxelHit toSubVoxel(const glm::vec3& worldPosition) noexcept;

}  // namespace voxl
