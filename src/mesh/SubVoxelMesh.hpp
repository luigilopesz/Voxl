#pragma once

// Packed vertex format for sub-voxel geometry.
//
// THIS HEADER IS A CONTRACT AND IT IS MIRRORED IN GLSL (assets/shaders/
// subvoxel.vert). The same warning as MeshData.hpp applies: a disagreement
// between the two produces garbage geometry that looks like a mesher bug.
//
// WHY A SECOND FORMAT EXISTS
//
// MeshData.hpp packs a position as three 6-bit fields, which covers 0..32 - the
// block-space corners of a chunk. Sub-voxels are eight times finer, so a
// chunk-local sub-voxel corner runs 0..256 and needs 9 bits per axis. That does
// not fit alongside the other fields in 8 bytes, and widening the main format
// would cost 50% more memory on the millions of vertices that are NOT
// sub-voxels, to benefit the handful that are.
//
// So sub-voxel geometry gets its own buffer, its own draw, and the layout below.
// The main chunk path is left byte-for-byte unchanged, which is what keeps the
// existing performance intact.
//
// ===========================================================================
//  PACKED SUB-VOXEL VERTEX - 8 bytes, two uint32 attributes, GL_UNSIGNED_INT
//
//  data0  (attribute location 0)
//  ---------------------------------------------------------------------------
//   bits  0..8    posX          9   chunk-local, in SUB-VOXEL units, 0..256
//   bits  9..17   posY          9   (256 = the far face of the last block, so
//   bits 18..26   posZ          9    257 distinct values are needed)
//   bits 27..29   direction     3   voxl::Direction, 0..5
//   bits 30..31   reserved      2   must be 0
//
//  data1  (attribute location 1)
//  ---------------------------------------------------------------------------
//   bits  0..11   textureLayer 12   layer in the block texture array
//   bits 12..14   width - 1     3   quad extent along U, 1..8 sub-voxels
//   bits 15..17   height - 1    3   quad extent along V, 1..8 sub-voxels
//   bits 18..19   corner        2   which corner of the quad this vertex is
//   bits 20..23   sunlight      4   0..15, inherited from the parent block
//   bits 24..27   blockLight    4   0..15, inherited from the parent block
//   bits 28..29   ao            2   0 = occluded .. 3 = open
//   bits 30..31   reserved      2   must be 0
//
//  GLSL side (keep byte-for-byte in sync):
//
//      vec3 pos = vec3(aData0 & 511u, (aData0 >> 9) & 511u, (aData0 >> 18) & 511u)
//                 * (1.0 / 8.0);            // back into block space
//      uint dir = (aData0 >> 27) & 7u;
//
//  TEXTURE COORDINATES ARE TAKEN FROM THE POSITION, NOT FROM width/height/corner.
//  The block path builds uv as (extent x corner selector), which is zero at the
//  quad's origin corner. That works there only because a block quad always begins
//  on a block boundary, so "zero at the origin" and "the fractional block
//  coordinate" coincide. A sub-voxel quad can begin at any eighth of a block, so
//  the same formula makes every carved quad sample the strip of texture running
//  out from the origin - a one-sub-voxel-tall quad smears the top 1/8 of the
//  texture along its whole length, and carved surfaces render as 1-D bands that
//  do not line up with the intact faces beside them.
//
//  So the shader reads u and v straight off the vertex position along the face's
//  tangent axes. That reduces to the block path's value at a block boundary,
//  still gives one repeat per block under GL_REPEAT, and leaves the derivatives -
//  and therefore mip selection - unchanged. width, height and corner remain in
//  the format (the mesher still needs them, and no bit has moved) but the vertex
//  shader no longer reads them. See assets/shaders/subvoxel.vert.
//
//  MERGE SCOPE: greedy merging runs within a single block's 8^3 grid, never
//  across two adjacent partial blocks - which is exactly why 3 bits of extent
//  suffice. Carving a wide tunnel therefore emits up to one quad set per damaged
//  block rather than one for the whole surface. That is an accepted trade: it
//  keeps this vertex at 8 bytes and the path only runs on blocks the player has
//  actually damaged. Widening extents to 9 bits and merging across blocks is the
//  obvious future optimisation if damage ever becomes widespread.
//
//  Lighting is inherited whole-block rather than computed per sub-voxel. A block
//  is 1/8 of a metre of detail; per-sub-voxel light would multiply the lighting
//  cost by 512 for a difference nobody can see at arm's length.
// ===========================================================================

#include "mesh/MeshData.hpp"
#include "world/SubVoxel.hpp"
#include "world/VoxelTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace voxl {

/// Bumped whenever the packing above changes.
inline constexpr std::uint32_t kSubVoxelFormatVersion = 1;

inline constexpr std::uint32_t kSubVtxPosXShift      = 0;
inline constexpr std::uint32_t kSubVtxPosYShift      = 9;
inline constexpr std::uint32_t kSubVtxPosZShift      = 18;
inline constexpr std::uint32_t kSubVtxDirectionShift = 27;

inline constexpr std::uint32_t kSubVtxTextureLayerShift = 0;
inline constexpr std::uint32_t kSubVtxWidthShift        = 12;
inline constexpr std::uint32_t kSubVtxHeightShift       = 15;
inline constexpr std::uint32_t kSubVtxCornerShift       = 18;
inline constexpr std::uint32_t kSubVtxSunlightShift     = 20;
inline constexpr std::uint32_t kSubVtxBlockLightShift   = 24;
inline constexpr std::uint32_t kSubVtxAoShift           = 28;

inline constexpr std::uint32_t kSubVtxPosMask    = 0x1FFu;  // 9 bits
inline constexpr std::uint32_t kSubVtxExtentMask = 0x07u;   // 3 bits

/// Largest chunk-local sub-voxel coordinate, inclusive.
inline constexpr std::uint32_t kSubVtxMaxPos = kChunkSize * kSubVoxelResolution;  // 256

static_assert(kSubVtxMaxPos <= kSubVtxPosMask, "sub-voxel position must fit in 9 bits");

/// One sub-voxel vertex, 8 bytes.
struct PackedSubVoxelVertex {
    std::uint32_t data0 = 0;
    std::uint32_t data1 = 0;

    friend constexpr bool operator==(const PackedSubVoxelVertex&,
                                     const PackedSubVoxelVertex&) noexcept = default;
};

static_assert(sizeof(PackedSubVoxelVertex) == 8, "sub-voxel vertex must stay 8 bytes");

/// Unpacked form for readability at the mesher's call site. Never stored.
struct SubVoxelVertexAttributes {
    std::uint16_t x = 0;  ///< 0..256, chunk-local, sub-voxel units
    std::uint16_t y = 0;
    std::uint16_t z = 0;
    Direction     direction   = Direction::PosY;
    std::uint16_t textureLayer = 0;
    std::uint8_t  width  = 1;  ///< 1..8
    std::uint8_t  height = 1;  ///< 1..8
    QuadCorner    corner = QuadCorner::Origin;
    std::uint8_t  sunlight   = 15;
    std::uint8_t  blockLight = 0;
    std::uint8_t  ao         = kMaxAoLevel;
};

[[nodiscard]] constexpr PackedSubVoxelVertex packSubVoxelVertex(
    const SubVoxelVertexAttributes& a) noexcept
{
    PackedSubVoxelVertex v;
    v.data0 = (static_cast<std::uint32_t>(a.x) << kSubVtxPosXShift) |
              (static_cast<std::uint32_t>(a.y) << kSubVtxPosYShift) |
              (static_cast<std::uint32_t>(a.z) << kSubVtxPosZShift) |
              (static_cast<std::uint32_t>(a.direction) << kSubVtxDirectionShift);
    v.data1 = (static_cast<std::uint32_t>(a.textureLayer) << kSubVtxTextureLayerShift) |
              (static_cast<std::uint32_t>(a.width - 1) << kSubVtxWidthShift) |
              (static_cast<std::uint32_t>(a.height - 1) << kSubVtxHeightShift) |
              (static_cast<std::uint32_t>(a.corner) << kSubVtxCornerShift) |
              (static_cast<std::uint32_t>(a.sunlight) << kSubVtxSunlightShift) |
              (static_cast<std::uint32_t>(a.blockLight) << kSubVtxBlockLightShift) |
              (static_cast<std::uint32_t>(a.ao) << kSubVtxAoShift);
    return v;
}

[[nodiscard]] constexpr SubVoxelVertexAttributes unpackSubVoxelVertex(
    const PackedSubVoxelVertex& v) noexcept
{
    SubVoxelVertexAttributes a;
    a.x = static_cast<std::uint16_t>((v.data0 >> kSubVtxPosXShift) & kSubVtxPosMask);
    a.y = static_cast<std::uint16_t>((v.data0 >> kSubVtxPosYShift) & kSubVtxPosMask);
    a.z = static_cast<std::uint16_t>((v.data0 >> kSubVtxPosZShift) & kSubVtxPosMask);
    a.direction = static_cast<Direction>((v.data0 >> kSubVtxDirectionShift) & kVtxDirectionMask);
    a.textureLayer =
        static_cast<std::uint16_t>((v.data1 >> kSubVtxTextureLayerShift) & kVtxTextureLayerMask);
    a.width  = static_cast<std::uint8_t>(((v.data1 >> kSubVtxWidthShift) & kSubVtxExtentMask) + 1);
    a.height = static_cast<std::uint8_t>(((v.data1 >> kSubVtxHeightShift) & kSubVtxExtentMask) + 1);
    a.corner = static_cast<QuadCorner>((v.data1 >> kSubVtxCornerShift) & kVtxCornerMask);
    a.sunlight   = static_cast<std::uint8_t>((v.data1 >> kSubVtxSunlightShift) & kVtxLightMask);
    a.blockLight = static_cast<std::uint8_t>((v.data1 >> kSubVtxBlockLightShift) & kVtxLightMask);
    a.ao         = static_cast<std::uint8_t>((v.data1 >> kSubVtxAoShift) & kVtxAoMask);
    return a;
}

/// CPU-side sub-voxel geometry for one chunk. Always drawn in the opaque pass:
/// a partially destroyed block keeps its material, and no translucent block is
/// currently destructible at sub-voxel resolution.
struct SubVoxelMeshData {
    std::vector<PackedSubVoxelVertex> vertices;
    std::vector<std::uint32_t>        indices;

    [[nodiscard]] bool empty() const noexcept { return indices.empty(); }
    [[nodiscard]] std::size_t triangleCount() const noexcept { return indices.size() / 3; }
    [[nodiscard]] std::size_t byteSize() const noexcept
    {
        return vertices.size() * sizeof(PackedSubVoxelVertex) +
               indices.size() * sizeof(std::uint32_t);
    }

    void clear() noexcept
    {
        vertices.clear();
        indices.clear();
    }

    /// Appends one quad as two triangles, matching MeshData's winding.
    void addQuad(const SubVoxelVertexAttributes& origin, const SubVoxelVertexAttributes& u,
                 const SubVoxelVertexAttributes& uv, const SubVoxelVertexAttributes& v);
};

}  // namespace voxl
