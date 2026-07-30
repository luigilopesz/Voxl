#pragma once

// The read-only voxel query interface used by meshing, lighting and physics.
//
// THIS HEADER IS A CONTRACT. Nothing downstream of the world should know that
// chunks live in a hash map behind a lock; everything reads voxels through a
// BlockAccess, which makes the mesher trivially testable against a synthetic
// world and lets the same code run on a snapshot or on the live world.

#include "core/Log.hpp"
#include "world/Block.hpp"
#include "world/Chunk.hpp"
#include "world/VoxelTypes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace voxl {

// ------------------------------------------------- out-of-world behaviour --

/// Queries above the world are AIR at full sunlight. If they returned an opaque
/// block instead, the top face of every surface block at y == kWorldMaxY would
/// be culled and the world would look decapitated.
inline constexpr BlockId      kAboveWorldBlock = blocks::Air;
inline constexpr std::uint8_t kAboveWorldLight = ChunkStorage::packLight(ChunkStorage::kMaxLightLevel, 0);

/// Queries below the world are BEDROCK in full darkness, so the bottom faces of
/// the lowest layer are culled and the player never sees through the floor.
inline constexpr BlockId      kBelowWorldBlock = blocks::Bedrock;
inline constexpr std::uint8_t kBelowWorldLight = ChunkStorage::packLight(0, 0);

/// A neighbour chunk that is not loaded reads as AIR.
///
/// The alternative (treat it as opaque) hides the faces along the seam, and
/// because a mesh is only rebuilt when something dirties it, those hidden faces
/// become permanent holes once the neighbour arrives. Extra faces are merely
/// wasted triangles for a frame. The scheduler should still refuse to mesh an
/// incomplete neighbourhood - see ChunkNeighbourhood::complete().
inline constexpr BlockId      kMissingChunkBlock = blocks::Air;
inline constexpr std::uint8_t kMissingChunkLight = ChunkStorage::packLight(ChunkStorage::kMaxLightLevel, 0);

// ---------------------------------------------------------- CRTP helpers --

/// Convenience queries derived from `getBlock`/`getLight`, mixed into a
/// concrete accessor with no virtual dispatch.
///
/// The mesher templates on the concrete accessor type and calls through this,
/// so the inner loop (millions of calls per second) inlines all the way down to
/// a palette read. The abstract `BlockAccess` below exists for the cold paths -
/// tooling, tests, debug queries - where a vtable is irrelevant.
template <typename Derived>
class BlockAccessOps {
public:
    [[nodiscard]] std::uint8_t getSunlight(const BlockPos& pos) const noexcept
    {
        return ChunkStorage::unpackSunlight(self().getLight(pos));
    }
    [[nodiscard]] std::uint8_t getBlockLight(const BlockPos& pos) const noexcept
    {
        return ChunkStorage::unpackBlockLight(self().getLight(pos));
    }
    [[nodiscard]] BlockId getNeighbourBlock(const BlockPos& pos, Direction direction) const noexcept
    {
        return self().getBlock(neighbour(pos, direction));
    }

private:
    [[nodiscard]] const Derived& self() const noexcept
    {
        return static_cast<const Derived&>(*this);
    }
};

/// Runtime-polymorphic voxel reader. Cold paths only; see BlockAccessOps.
class BlockAccess {
public:
    virtual ~BlockAccess() = default;

    /// World-space voxel query. Never fails: out-of-world and unloaded
    /// positions return the defined values documented above.
    [[nodiscard]] virtual BlockId getBlock(const BlockPos& pos) const noexcept = 0;

    /// World-space packed light query (high nibble sunlight, low nibble block
    /// light - see ChunkStorage).
    [[nodiscard]] virtual std::uint8_t getLight(const BlockPos& pos) const noexcept = 0;

    [[nodiscard]] std::uint8_t sunlightAt(const BlockPos& pos) const noexcept
    {
        return ChunkStorage::unpackSunlight(getLight(pos));
    }
    [[nodiscard]] std::uint8_t blockLightAt(const BlockPos& pos) const noexcept
    {
        return ChunkStorage::unpackBlockLight(getLight(pos));
    }

protected:
    BlockAccess()                              = default;
    BlockAccess(const BlockAccess&)            = default;
    BlockAccess& operator=(const BlockAccess&) = default;
};

// ------------------------------------------------------ ChunkNeighbourhood --

/// A centre chunk plus its 26 surrounding chunks, captured as shared_ptr
/// snapshots.
///
/// WHY A SNAPSHOT INSTEAD OF LOCKING THE WORLD
/// -------------------------------------------
/// Meshing one chunk reads a 34^3 region: every voxel of the centre plus a
/// one-block skirt from the neighbours (needed for face culling, and a full
/// 3x3x3 rather than just the 6 faces because smooth ambient occlusion samples
/// the diagonals). That is tens of thousands of reads taking milliseconds.
///
/// If the mesher held the world's chunk-map lock for that whole time, then with
/// N worker threads meshing concurrently the world map would be the single
/// serialising resource in the engine: the main thread could not insert a newly
/// streamed chunk, and every other mesher would block. Frame hitches would
/// scale with view distance.
///
/// Instead the main thread does one short critical section - look up 27 entries
/// and copy their shared_ptrs - and hands the resulting value type to a worker.
/// After that the worker touches no shared mutable state at all:
///   - the shared_ptrs keep the chunks alive even if streaming decides to unload
///     them mid-mesh, so there is no use-after-free;
///   - the chunks are in state Meshing/Ready, where the threading contract
///     forbids writes, so the reads are race-free without atomics;
///   - a concurrent player edit bumps Chunk::contentVersion(), and the stale
///     mesh is discarded when it is handed back (see Chunk::meshedVersion()).
///
/// The cost is 27 atomic refcount increments per mesh job. That is nothing next
/// to the millisecond of meshing it protects.
///
/// Thread safety: immutable after capture. Safe to read from any thread; the
/// capture itself must happen wherever the world's lock is held.
class ChunkNeighbourhood final : public BlockAccess, public BlockAccessOps<ChunkNeighbourhood> {
public:
    static constexpr std::size_t kNeighbourCount = 27;

    /// Index of the (dx, dy, dz) neighbour, each offset in [-1, 1]. The centre
    /// is index 13. Ordering is y-major then z then x, matching localIndex().
    [[nodiscard]] static constexpr std::size_t neighbourIndex(std::int32_t dx, std::int32_t dy,
                                                              std::int32_t dz) noexcept
    {
        return static_cast<std::size_t>(((dy + 1) * 3 + (dz + 1)) * 3 + (dx + 1));
    }
    static constexpr std::size_t kCentreIndex = 13;

    ChunkNeighbourhood() = default;
    explicit ChunkNeighbourhood(const ChunkPos& centre) noexcept
        : m_centre(centre), m_origin(centre.originBlock())
    {
    }

    void setCentre(const ChunkPos& centre) noexcept
    {
        m_centre = centre;
        m_origin = centre.originBlock();
    }

    void setChunk(std::int32_t dx, std::int32_t dy, std::int32_t dz, ConstChunkPtr chunk) noexcept
    {
        VOXL_ASSERT(dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1 && dz >= -1 && dz <= 1,
                    "neighbour offset out of range");
        m_chunks[neighbourIndex(dx, dy, dz)] = std::move(chunk);
    }

    [[nodiscard]] const Chunk* chunkAt(std::int32_t dx, std::int32_t dy, std::int32_t dz) const noexcept
    {
        return m_chunks[neighbourIndex(dx, dy, dz)].get();
    }

    [[nodiscard]] const Chunk* centre() const noexcept { return m_chunks[kCentreIndex].get(); }
    [[nodiscard]] const ChunkPos& centrePos() const noexcept { return m_centre; }
    [[nodiscard]] const BlockPos& originBlock() const noexcept { return m_origin; }

    /// True when all 27 slots are populated, ignoring slots that fall outside
    /// the world vertically (those are answered by the out-of-world rules and
    /// never need a chunk).
    [[nodiscard]] bool complete() const noexcept
    {
        for (std::int32_t dy = -1; dy <= 1; ++dy) {
            const std::int32_t sectionY = m_centre.y + dy;
            if (sectionY < 0 || sectionY >= kWorldSectionCount) {
                continue;
            }
            for (std::int32_t dz = -1; dz <= 1; ++dz) {
                for (std::int32_t dx = -1; dx <= 1; ++dx) {
                    if (m_chunks[neighbourIndex(dx, dy, dz)] == nullptr) {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    /// Number of populated slots; useful in the debug overlay when a chunk is
    /// stuck waiting for neighbours.
    [[nodiscard]] std::size_t loadedCount() const noexcept
    {
        std::size_t count = 0;
        for (const ConstChunkPtr& chunk : m_chunks) {
            count += chunk != nullptr ? 1u : 0u;
        }
        return count;
    }

    // ---- hot path: coordinates relative to the centre chunk's origin ----

    /// `x`, `y`, `z` are offsets from the centre chunk origin and may range over
    /// [-kChunkSize, 2 * kChunkSize - 1]; the mesher uses [-1, kChunkSize].
    [[nodiscard]] BlockId getBlockLocal(std::int32_t x, std::int32_t y, std::int32_t z) const noexcept
    {
        const std::int32_t worldY = m_origin.y + y;
        if (worldY > kWorldMaxY) {
            return kAboveWorldBlock;
        }
        if (worldY < kWorldMinY) {
            return kBelowWorldBlock;
        }
        const Chunk* chunk = resolve(x, y, z);
        if (chunk == nullptr) {
            return kMissingChunkBlock;
        }
        return chunk->getBlock(x & kChunkSizeMask, y & kChunkSizeMask, z & kChunkSizeMask);
    }

    [[nodiscard]] std::uint8_t getLightLocal(std::int32_t x, std::int32_t y, std::int32_t z) const noexcept
    {
        const std::int32_t worldY = m_origin.y + y;
        if (worldY > kWorldMaxY) {
            return kAboveWorldLight;
        }
        if (worldY < kWorldMinY) {
            return kBelowWorldLight;
        }
        const Chunk* chunk = resolve(x, y, z);
        if (chunk == nullptr) {
            return kMissingChunkLight;
        }
        return chunk->getLight(x & kChunkSizeMask, y & kChunkSizeMask, z & kChunkSizeMask);
    }

    // ---- BlockAccess (world space) ----

    [[nodiscard]] BlockId getBlock(const BlockPos& pos) const noexcept override
    {
        return getBlockLocal(pos.x - m_origin.x, pos.y - m_origin.y, pos.z - m_origin.z);
    }

    [[nodiscard]] std::uint8_t getLight(const BlockPos& pos) const noexcept override
    {
        return getLightLocal(pos.x - m_origin.x, pos.y - m_origin.y, pos.z - m_origin.z);
    }

private:
    /// Arithmetic shift floors, so -1 maps to chunk offset -1 and 32 to +1.
    [[nodiscard]] const Chunk* resolve(std::int32_t x, std::int32_t y, std::int32_t z) const noexcept
    {
        const std::int32_t dx = x >> kChunkSizeLog2;
        const std::int32_t dy = y >> kChunkSizeLog2;
        const std::int32_t dz = z >> kChunkSizeLog2;
        VOXL_ASSERT(dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1 && dz >= -1 && dz <= 1,
                    "neighbourhood query outside the captured 3x3x3 region");
        if (dx < -1 || dx > 1 || dy < -1 || dy > 1 || dz < -1 || dz > 1) {
            return nullptr;
        }
        return m_chunks[neighbourIndex(dx, dy, dz)].get();
    }

    ChunkPos m_centre{};
    BlockPos m_origin{};
    std::array<ConstChunkPtr, kNeighbourCount> m_chunks{};
};

/// Builds a neighbourhood using a caller-supplied lookup.
///
/// The lookup keeps this header free of any dependency on the world container;
/// call it with the world lock held and hand the result to a worker. `lookup`
/// must have signature `ConstChunkPtr(const ChunkPos&)` and return nullptr for
/// chunks that are not resident.
template <typename LookupFn>
[[nodiscard]] ChunkNeighbourhood captureNeighbourhood(const ChunkPos& centre, LookupFn&& lookup)
{
    ChunkNeighbourhood neighbourhood(centre);
    for (std::int32_t dy = -1; dy <= 1; ++dy) {
        const std::int32_t sectionY = centre.y + dy;
        if (sectionY < 0 || sectionY >= kWorldSectionCount) {
            continue;  // answered by the out-of-world rules
        }
        for (std::int32_t dz = -1; dz <= 1; ++dz) {
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                neighbourhood.setChunk(dx, dy, dz,
                                       lookup(ChunkPos{centre.x + dx, sectionY, centre.z + dz}));
            }
        }
    }
    return neighbourhood;
}

}  // namespace voxl
