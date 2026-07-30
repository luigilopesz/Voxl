#pragma once

// Packed vertex format and the CPU-side mesh a worker produces for one chunk.
//
// THIS HEADER IS A CONTRACT AND IT IS MIRRORED IN GLSL. The bit layout below is
// duplicated in the chunk vertex shader; if the two ever disagree the geometry
// is garbage in a way that looks like a mesher bug and takes hours to find.
// Change the layout here and in the shader in the same commit, and bump
// kVertexFormatVersion so a stale shader cache is invalidated.

#include "world/Block.hpp"
#include "world/VoxelTypes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <glm/vec3.hpp>

namespace voxl {

/// Bumped whenever the packing below changes.
inline constexpr std::uint32_t kVertexFormatVersion = 1;

// ===========================================================================
//  PACKED VERTEX - 8 bytes, two uint32 attributes, both uploaded as GL_UNSIGNED_INT
//  (glVertexAttribIPointer - integer attributes, NOT normalised floats).
//
//  data0  (attribute location 0)
//  ---------------------------------------------------------------------------
//   bits  0..5    posX          6   0..32 inclusive (greedy quad corners reach
//   bits  6..11   posY          6   the far edge of the chunk, so 33 distinct
//   bits 12..17   posZ          6   values are needed - 5 bits is not enough)
//   bits 18..20   direction     3   voxl::Direction, 0..5
//   bits 21..24   sunlight      4   0..15
//   bits 25..28   blockLight    4   0..15
//   bits 29..30   ao            2   0 = fully occluded corner .. 3 = open
//   bit  31       reserved      1   must be 0
//
//  data1  (attribute location 1)
//  ---------------------------------------------------------------------------
//   bits  0..11   textureLayer 12   layer in the block texture array, 0..4095
//   bits 12..16   width - 1     5   quad extent along its U axis, 1..32
//   bits 17..21   height - 1    5   quad extent along its V axis, 1..32
//   bits 22..23   corner        2   which corner of the quad this vertex is
//   bits 24..31   reserved      8   must be 0
//
//  GLSL side (keep byte-for-byte in sync):
//
//      layout(location = 0) in uint aData0;
//      layout(location = 1) in uint aData1;
//      vec3  pos        = vec3(aData0 & 63u, (aData0 >> 6) & 63u, (aData0 >> 12) & 63u);
//      uint  dir        = (aData0 >> 18) & 7u;
//      float sun        = float((aData0 >> 21) & 15u) / 15.0;
//      float blockLight = float((aData0 >> 25) & 15u) / 15.0;
//      float ao         = float((aData0 >> 29) & 3u)  / 3.0;
//      uint  layer      = aData1 & 4095u;
//      vec2  size       = vec2(((aData1 >> 12) & 31u) + 1u, ((aData1 >> 17) & 31u) + 1u);
//      uint  corner     = (aData1 >> 22) & 3u;
//      vec2  uv         = size * vec2(float(corner == 1u || corner == 2u),
//                                     float(corner == 2u || corner == 3u));
//
//  Positions are chunk-local; the shader adds the chunk origin from a uniform.
//  Keeping them 6-bit integers rather than floats is what gets a vertex into
//  8 bytes: a 4-million-triangle view distance costs ~100 MB of VBO instead of
//  ~400 MB, and the whole visible set stays in the GPU's cache hierarchy.
// ===========================================================================

/// Corner ordering within a quad. Emitted counter-clockwise when viewed from
/// outside the face, which is what GL_CCW front-facing expects.
enum class QuadCorner : std::uint8_t {
    Origin = 0,  ///< (0, 0) in quad UV space
    U      = 1,  ///< (width, 0)
    UV     = 2,  ///< (width, height)
    V      = 3,  ///< (0, height)
};

// Field positions, exported so tests and tooling do not re-derive them.
inline constexpr std::uint32_t kVtxPosXShift        = 0;
inline constexpr std::uint32_t kVtxPosYShift        = 6;
inline constexpr std::uint32_t kVtxPosZShift        = 12;
inline constexpr std::uint32_t kVtxDirectionShift   = 18;
inline constexpr std::uint32_t kVtxSunlightShift    = 21;
inline constexpr std::uint32_t kVtxBlockLightShift  = 25;
inline constexpr std::uint32_t kVtxAoShift          = 29;

inline constexpr std::uint32_t kVtxTextureLayerShift = 0;
inline constexpr std::uint32_t kVtxWidthShift        = 12;
inline constexpr std::uint32_t kVtxHeightShift       = 17;
inline constexpr std::uint32_t kVtxCornerShift       = 22;

inline constexpr std::uint32_t kVtxPosMask          = 0x3Fu;    // 6 bits
inline constexpr std::uint32_t kVtxDirectionMask    = 0x07u;    // 3 bits
inline constexpr std::uint32_t kVtxLightMask        = 0x0Fu;    // 4 bits
inline constexpr std::uint32_t kVtxAoMask           = 0x03u;    // 2 bits
inline constexpr std::uint32_t kVtxTextureLayerMask = 0x0FFFu;  // 12 bits
inline constexpr std::uint32_t kVtxExtentMask       = 0x1Fu;    // 5 bits
inline constexpr std::uint32_t kVtxCornerMask       = 0x03u;    // 2 bits

inline constexpr std::uint16_t kMaxTextureLayer = 4095;
inline constexpr std::uint8_t  kMaxAoLevel      = 3;

/// One vertex, 8 bytes. Trivially copyable so a whole vector can be handed to
/// glNamedBufferData in a single call.
struct PackedVertex {
    std::uint32_t data0 = 0;
    std::uint32_t data1 = 0;

    friend constexpr bool operator==(const PackedVertex&, const PackedVertex&) noexcept = default;
};

static_assert(sizeof(PackedVertex) == 8, "packed vertex must stay 8 bytes");
static_assert(alignof(PackedVertex) == 4, "packed vertex must be 4-byte aligned for attrib pointers");

/// Unpacked form, used at the mesher's call site for readability. Never stored.
struct VertexAttributes {
    std::uint8_t  x            = 0;  ///< 0..32, chunk-local
    std::uint8_t  y            = 0;
    std::uint8_t  z            = 0;
    Direction     direction    = Direction::PosY;
    std::uint8_t  sunlight     = 0;  ///< 0..15
    std::uint8_t  blockLight   = 0;  ///< 0..15
    std::uint8_t  ao           = kMaxAoLevel;  ///< 0..3, 3 = unoccluded
    std::uint16_t textureLayer = 0;  ///< 0..4095
    std::uint8_t  width        = 1;  ///< 1..32
    std::uint8_t  height       = 1;  ///< 1..32
    QuadCorner    corner       = QuadCorner::Origin;
};

[[nodiscard]] constexpr PackedVertex packVertex(const VertexAttributes& in) noexcept
{
    PackedVertex out;
    out.data0 = ((static_cast<std::uint32_t>(in.x) & kVtxPosMask) << kVtxPosXShift) |
                ((static_cast<std::uint32_t>(in.y) & kVtxPosMask) << kVtxPosYShift) |
                ((static_cast<std::uint32_t>(in.z) & kVtxPosMask) << kVtxPosZShift) |
                ((static_cast<std::uint32_t>(in.direction) & kVtxDirectionMask) << kVtxDirectionShift) |
                ((static_cast<std::uint32_t>(in.sunlight) & kVtxLightMask) << kVtxSunlightShift) |
                ((static_cast<std::uint32_t>(in.blockLight) & kVtxLightMask) << kVtxBlockLightShift) |
                ((static_cast<std::uint32_t>(in.ao) & kVtxAoMask) << kVtxAoShift);

    out.data1 = ((static_cast<std::uint32_t>(in.textureLayer) & kVtxTextureLayerMask) << kVtxTextureLayerShift) |
                ((static_cast<std::uint32_t>(in.width - 1u) & kVtxExtentMask) << kVtxWidthShift) |
                ((static_cast<std::uint32_t>(in.height - 1u) & kVtxExtentMask) << kVtxHeightShift) |
                ((static_cast<std::uint32_t>(in.corner) & kVtxCornerMask) << kVtxCornerShift);
    return out;
}

[[nodiscard]] constexpr VertexAttributes unpackVertex(const PackedVertex& in) noexcept
{
    VertexAttributes out;
    out.x            = static_cast<std::uint8_t>((in.data0 >> kVtxPosXShift) & kVtxPosMask);
    out.y            = static_cast<std::uint8_t>((in.data0 >> kVtxPosYShift) & kVtxPosMask);
    out.z            = static_cast<std::uint8_t>((in.data0 >> kVtxPosZShift) & kVtxPosMask);
    out.direction    = static_cast<Direction>((in.data0 >> kVtxDirectionShift) & kVtxDirectionMask);
    out.sunlight     = static_cast<std::uint8_t>((in.data0 >> kVtxSunlightShift) & kVtxLightMask);
    out.blockLight   = static_cast<std::uint8_t>((in.data0 >> kVtxBlockLightShift) & kVtxLightMask);
    out.ao           = static_cast<std::uint8_t>((in.data0 >> kVtxAoShift) & kVtxAoMask);
    out.textureLayer = static_cast<std::uint16_t>((in.data1 >> kVtxTextureLayerShift) & kVtxTextureLayerMask);
    out.width        = static_cast<std::uint8_t>(((in.data1 >> kVtxWidthShift) & kVtxExtentMask) + 1u);
    out.height       = static_cast<std::uint8_t>(((in.data1 >> kVtxHeightShift) & kVtxExtentMask) + 1u);
    out.corner       = static_cast<QuadCorner>((in.data1 >> kVtxCornerShift) & kVtxCornerMask);
    return out;
}

// A round trip through the packer must be lossless for the extreme values.
static_assert(unpackVertex(packVertex(VertexAttributes{32, 32, 32, Direction::PosZ, 15, 15, 3, 4095,
                                                       32, 32, QuadCorner::UV}))
                  .x == 32);
static_assert(unpackVertex(packVertex(VertexAttributes{32, 32, 32, Direction::PosZ, 15, 15, 3, 4095,
                                                       32, 32, QuadCorner::UV}))
                  .textureLayer == 4095);
static_assert(unpackVertex(packVertex(VertexAttributes{0, 0, 0, Direction::NegX, 0, 0, 0, 0, 1, 1,
                                                       QuadCorner::Origin}))
                  .width == 1);

// ---------------------------------------------------------------- indices --

/// 32-bit indices throughout. A single chunk never needs more than 65535
/// vertices in practice, but mixing 16- and 32-bit index buffers per layer
/// complicates the draw-call path far more than the memory saves.
using MeshIndex = std::uint32_t;

/// Geometry for one render pass of one chunk.
struct MeshLayerData {
    std::vector<PackedVertex> vertices;
    std::vector<MeshIndex>    indices;

    void clear() noexcept
    {
        vertices.clear();
        indices.clear();
    }

    /// Frees capacity as well; used when returning a mesh to a pool.
    void release()
    {
        std::vector<PackedVertex>{}.swap(vertices);
        std::vector<MeshIndex>{}.swap(indices);
    }

    [[nodiscard]] bool empty() const noexcept { return indices.empty(); }
    [[nodiscard]] std::size_t vertexCount() const noexcept { return vertices.size(); }
    [[nodiscard]] std::size_t indexCount() const noexcept { return indices.size(); }
    [[nodiscard]] std::size_t triangleCount() const noexcept { return indices.size() / 3; }

    [[nodiscard]] std::size_t vertexBytes() const noexcept
    {
        return vertices.size() * sizeof(PackedVertex);
    }
    [[nodiscard]] std::size_t indexBytes() const noexcept
    {
        return indices.size() * sizeof(MeshIndex);
    }
    [[nodiscard]] std::size_t byteSize() const noexcept { return vertexBytes() + indexBytes(); }

    /// Appends a quad as two triangles, corners in QuadCorner order.
    ///
    /// The diagonal is chosen by ambient occlusion: splitting 0-2 when the AO
    /// of corners 0 and 2 is the darker pair produces the classic diagonal
    /// seam artefact, so we flip to 1-3 in that case. This is the only place
    /// the winding is decided.
    void addQuad(const PackedVertex& c0, const PackedVertex& c1, const PackedVertex& c2,
                 const PackedVertex& c3)
    {
        const MeshIndex base = static_cast<MeshIndex>(vertices.size());
        vertices.push_back(c0);
        vertices.push_back(c1);
        vertices.push_back(c2);
        vertices.push_back(c3);

        const std::uint32_t ao0 = (c0.data0 >> kVtxAoShift) & kVtxAoMask;
        const std::uint32_t ao1 = (c1.data0 >> kVtxAoShift) & kVtxAoMask;
        const std::uint32_t ao2 = (c2.data0 >> kVtxAoShift) & kVtxAoMask;
        const std::uint32_t ao3 = (c3.data0 >> kVtxAoShift) & kVtxAoMask;

        if (ao0 + ao2 > ao1 + ao3) {
            indices.insert(indices.end(), {base + 0u, base + 1u, base + 2u,
                                           base + 0u, base + 2u, base + 3u});
        } else {
            indices.insert(indices.end(), {base + 1u, base + 2u, base + 3u,
                                           base + 1u, base + 3u, base + 0u});
        }
    }

    /// Pre-sizes for an expected quad count; meshing a solid chunk face reaches
    /// several hundred quads and the reallocation churn is measurable.
    void reserveQuads(std::size_t quads)
    {
        vertices.reserve(vertices.size() + quads * 4);
        indices.reserve(indices.size() + quads * 6);
    }
};

/// The complete CPU-side result of meshing one chunk.
///
/// Produced on a worker thread, consumed on the main thread by the GPU upload
/// posted to JobSystem::mainThreadQueue(). Movable, and moved rather than
/// copied across that boundary.
struct ChunkMeshData {
    ChunkPos position{};

    /// Chunk::contentVersion() at the moment the snapshot was taken. The main
    /// thread compares it before uploading and drops a stale mesh.
    std::uint32_t contentVersion = 0;

    std::array<MeshLayerData, kRenderLayerCount> layers{};

    /// Tight chunk-local bounds of the emitted geometry, in blocks. A chunk
    /// holding a single floor slab culls far better against this than against
    /// its full 32^3 box.
    glm::vec3 boundsMin{0.0f, 0.0f, 0.0f};
    glm::vec3 boundsMax{0.0f, 0.0f, 0.0f};

    [[nodiscard]] MeshLayerData& layer(RenderLayer which) noexcept
    {
        return layers[static_cast<std::size_t>(which)];
    }
    [[nodiscard]] const MeshLayerData& layer(RenderLayer which) const noexcept
    {
        return layers[static_cast<std::size_t>(which)];
    }

    [[nodiscard]] bool empty() const noexcept
    {
        for (const MeshLayerData& data : layers) {
            if (!data.empty()) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] std::size_t vertexCount() const noexcept
    {
        std::size_t total = 0;
        for (const MeshLayerData& data : layers) {
            total += data.vertexCount();
        }
        return total;
    }

    [[nodiscard]] std::size_t triangleCount() const noexcept
    {
        std::size_t total = 0;
        for (const MeshLayerData& data : layers) {
            total += data.triangleCount();
        }
        return total;
    }

    /// Bytes that will be uploaded to the GPU. The debug overlay sums this over
    /// resident chunks to show the real VBO footprint.
    [[nodiscard]] std::size_t byteSize() const noexcept
    {
        std::size_t total = 0;
        for (const MeshLayerData& data : layers) {
            total += data.byteSize();
        }
        return total;
    }

    void clear() noexcept
    {
        for (MeshLayerData& data : layers) {
            data.clear();
        }
        boundsMin = glm::vec3{0.0f};
        boundsMax = glm::vec3{0.0f};
    }
};

}  // namespace voxl
