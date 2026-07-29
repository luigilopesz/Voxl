#pragma once

// Core voxel coordinate vocabulary and world dimensions.
//
// THIS HEADER IS A CONTRACT. Terrain generation, meshing, physics, rendering,
// lighting and persistence all agree on the types and the index ordering
// defined here. Changing the chunk dimensions or `localIndex` ordering changes
// the on-disk format and every cache-friendly loop in the engine, so treat it
// as a versioned interface rather than a convenience header.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>

#include <glm/vec3.hpp>

namespace voxl {

// --------------------------------------------------------------- geometry --

/// Chunks are cubic. 32 is chosen deliberately:
///  - 32^3 = 32768 voxels sits in a few pages after palette compression,
///  - a 32-wide row is exactly one AVX2-friendly span of 8x uint32 lanes,
///  - cubic sections make vertical frustum culling and lighting propagation
///    symmetric, unlike tall 16x256x16 columns.
inline constexpr std::int32_t kChunkSize = 32;
inline constexpr std::int32_t kChunkSizeMask = kChunkSize - 1;  // valid: power of two
inline constexpr std::int32_t kChunkSizeLog2 = 5;
inline constexpr std::size_t  kChunkVolume = static_cast<std::size_t>(kChunkSize) * kChunkSize * kChunkSize;

/// Vertical extent of the world in chunk sections. 8 sections x 32 blocks
/// gives a 256-block-tall world: enough for deep caves under sea level and
/// tall mountains above it without paying for empty sky sections.
inline constexpr std::int32_t kWorldSectionCount = 8;
inline constexpr std::int32_t kWorldMinY = 0;
inline constexpr std::int32_t kWorldMaxY = kWorldSectionCount * kChunkSize - 1;  // inclusive
inline constexpr std::int32_t kWorldHeight = kWorldSectionCount * kChunkSize;

inline constexpr std::int32_t kSeaLevel = 96;

// ------------------------------------------------------------ coordinates --

/// Integer position of a single voxel in world space.
struct BlockPos {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;

    friend constexpr bool operator==(const BlockPos&, const BlockPos&) noexcept = default;

    [[nodiscard]] constexpr BlockPos offset(std::int32_t dx, std::int32_t dy, std::int32_t dz) const noexcept
    {
        return BlockPos{x + dx, y + dy, z + dz};
    }

    [[nodiscard]] glm::vec3 center() const noexcept
    {
        return glm::vec3{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f,
                         static_cast<float>(z) + 0.5f};
    }
};

/// Position of a chunk on the chunk grid. `y` indexes the vertical section and
/// is always within [0, kWorldSectionCount).
struct ChunkPos {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t z = 0;

    friend constexpr bool operator==(const ChunkPos&, const ChunkPos&) noexcept = default;

    [[nodiscard]] constexpr BlockPos originBlock() const noexcept
    {
        return BlockPos{x * kChunkSize, y * kChunkSize, z * kChunkSize};
    }
};

/// Horizontal-only chunk coordinate, used by streaming and persistence where a
/// whole vertical column is loaded, saved and discarded as a unit.
struct ColumnPos {
    std::int32_t x = 0;
    std::int32_t z = 0;

    friend constexpr bool operator==(const ColumnPos&, const ColumnPos&) noexcept = default;
};

// --------------------------------------------------- coordinate conversion --

/// Floor-division by the chunk size. A plain `/` truncates toward zero, which
/// would make chunk -0.5 and +0.5 both map to chunk 0 and tear the world along
/// every negative axis. The shift is an arithmetic right shift, which floors.
[[nodiscard]] constexpr std::int32_t blockToChunkAxis(std::int32_t block) noexcept
{
    return block >> kChunkSizeLog2;
}

/// Non-negative remainder of a block coordinate within its chunk.
[[nodiscard]] constexpr std::int32_t blockToLocalAxis(std::int32_t block) noexcept
{
    return block & kChunkSizeMask;
}

[[nodiscard]] constexpr ChunkPos toChunkPos(const BlockPos& pos) noexcept
{
    return ChunkPos{blockToChunkAxis(pos.x), blockToChunkAxis(pos.y), blockToChunkAxis(pos.z)};
}

[[nodiscard]] constexpr ColumnPos toColumnPos(const BlockPos& pos) noexcept
{
    return ColumnPos{blockToChunkAxis(pos.x), blockToChunkAxis(pos.z)};
}

[[nodiscard]] constexpr ColumnPos toColumnPos(const ChunkPos& pos) noexcept
{
    return ColumnPos{pos.x, pos.z};
}

/// Block coordinate relative to its own chunk origin; each component in
/// [0, kChunkSize).
[[nodiscard]] constexpr BlockPos toLocalPos(const BlockPos& pos) noexcept
{
    return BlockPos{blockToLocalAxis(pos.x), blockToLocalAxis(pos.y), blockToLocalAxis(pos.z)};
}

/// True when the position is inside the world's vertical bounds. Horizontal
/// extent is unbounded.
[[nodiscard]] constexpr bool isInsideWorld(const BlockPos& pos) noexcept
{
    return pos.y >= kWorldMinY && pos.y <= kWorldMaxY;
}

/// Converts a floating-point world position to the voxel containing it.
/// std::floor semantics are required: casting truncates toward zero and would
/// misplace everything at negative coordinates by one block.
[[nodiscard]] inline BlockPos worldToBlockPos(const glm::vec3& position) noexcept
{
    return BlockPos{static_cast<std::int32_t>(std::floor(position.x)),
                    static_cast<std::int32_t>(std::floor(position.y)),
                    static_cast<std::int32_t>(std::floor(position.z))};
}

// ------------------------------------------------------------- indexing --

/// Linear index of a local voxel coordinate inside a chunk's storage.
///
/// ORDERING IS PART OF THE CONTRACT: x varies fastest, then z, then y. Meshing
/// and lighting both sweep x-major, and the save format writes voxels in this
/// order. Callers must have already reduced the coordinate to [0, kChunkSize).
[[nodiscard]] constexpr std::size_t localIndex(std::int32_t x, std::int32_t y, std::int32_t z) noexcept
{
    return static_cast<std::size_t>((y * kChunkSize + z) * kChunkSize + x);
}

[[nodiscard]] constexpr std::size_t localIndex(const BlockPos& local) noexcept
{
    return localIndex(local.x, local.y, local.z);
}

/// True when every component is a valid in-chunk coordinate.
[[nodiscard]] constexpr bool isLocalPos(std::int32_t x, std::int32_t y, std::int32_t z) noexcept
{
    return x >= 0 && x < kChunkSize && y >= 0 && y < kChunkSize && z >= 0 && z < kChunkSize;
}

// ------------------------------------------------------------ directions --

/// Face/axis directions. The numeric values index the neighbour tables below
/// and are baked into packed mesh vertices, so they must stay stable.
enum class Direction : std::uint8_t {
    NegX = 0,
    PosX = 1,
    NegY = 2,
    PosY = 3,
    NegZ = 4,
    PosZ = 5,
};

inline constexpr std::size_t kDirectionCount = 6;

inline constexpr glm::ivec3 kDirectionOffsets[kDirectionCount] = {
    {-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1},
};

inline constexpr glm::vec3 kDirectionNormals[kDirectionCount] = {
    {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f},
    {0.0f, 1.0f, 0.0f},  {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f, 1.0f},
};

[[nodiscard]] constexpr Direction opposite(Direction direction) noexcept
{
    // Directions are stored in +/- pairs, so flipping the low bit inverts them.
    return static_cast<Direction>(static_cast<std::uint8_t>(direction) ^ 1u);
}

[[nodiscard]] constexpr BlockPos neighbour(const BlockPos& pos, Direction direction) noexcept
{
    const glm::ivec3& offset = kDirectionOffsets[static_cast<std::size_t>(direction)];
    return BlockPos{pos.x + offset.x, pos.y + offset.y, pos.z + offset.z};
}

}  // namespace voxl

// ------------------------------------------------------------- hashing --

namespace std {

/// Chunk lookups happen thousands of times per frame during meshing and
/// streaming, so the hash must be cheap and must not collide on the small,
/// spatially clustered coordinates that dominate real workloads. These are
/// large odd 64-bit constants (derived from the golden ratio and two primes)
/// mixed with a final avalanche step.
template <>
struct hash<voxl::ChunkPos> {
    [[nodiscard]] size_t operator()(const voxl::ChunkPos& pos) const noexcept
    {
        uint64_t h = static_cast<uint64_t>(static_cast<uint32_t>(pos.x)) * 0x9E3779B97F4A7C15ull;
        h ^= static_cast<uint64_t>(static_cast<uint32_t>(pos.y)) * 0xC2B2AE3D27D4EB4Full;
        h ^= static_cast<uint64_t>(static_cast<uint32_t>(pos.z)) * 0x165667B19E3779F9ull;
        h ^= h >> 31;
        h *= 0xD6E8FEB86659FD93ull;
        h ^= h >> 32;
        return static_cast<size_t>(h);
    }
};

template <>
struct hash<voxl::ColumnPos> {
    [[nodiscard]] size_t operator()(const voxl::ColumnPos& pos) const noexcept
    {
        uint64_t h = static_cast<uint64_t>(static_cast<uint32_t>(pos.x)) * 0x9E3779B97F4A7C15ull;
        h ^= static_cast<uint64_t>(static_cast<uint32_t>(pos.z)) * 0xC2B2AE3D27D4EB4Full;
        h ^= h >> 31;
        h *= 0xD6E8FEB86659FD93ull;
        h ^= h >> 32;
        return static_cast<size_t>(h);
    }
};

template <>
struct hash<voxl::BlockPos> {
    [[nodiscard]] size_t operator()(const voxl::BlockPos& pos) const noexcept
    {
        uint64_t h = static_cast<uint64_t>(static_cast<uint32_t>(pos.x)) * 0x9E3779B97F4A7C15ull;
        h ^= static_cast<uint64_t>(static_cast<uint32_t>(pos.y)) * 0xC2B2AE3D27D4EB4Full;
        h ^= static_cast<uint64_t>(static_cast<uint32_t>(pos.z)) * 0x165667B19E3779F9ull;
        h ^= h >> 31;
        h *= 0xD6E8FEB86659FD93ull;
        h ^= h >> 32;
        return static_cast<size_t>(h);
    }
};

}  // namespace std
