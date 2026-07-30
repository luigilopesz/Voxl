#include "mesh/SubVoxelMesher.hpp"

#include "core/Log.hpp"
#include "world/Chunk.hpp"
#include "world/ChunkStorage.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace voxl {
namespace {

// ------------------------------------------------------------ face frames --

/// Per-direction tangent frame, identical in meaning to the table in
/// GreedyMesher.cpp: `uAxis` and `vAxis` are ordered so that u_hat x v_hat is the
/// outward normal, which is what makes the corner sequence
/// (origin, +U, +U+V, +V) counter-clockwise seen from outside.
struct FaceFrame {
    std::int32_t nAxis;  ///< 0 = x, 1 = y, 2 = z
    std::int32_t uAxis;
    std::int32_t vAxis;
    std::int32_t sign;  ///< +1 for the positive face of the cell, -1 otherwise
};

constexpr std::array<FaceFrame, kDirectionCount> kFaceFrames = {{
    /* NegX */ {0, 2, 1, -1},
    /* PosX */ {0, 1, 2, +1},
    /* NegY */ {1, 0, 2, -1},
    /* PosY */ {1, 2, 0, +1},
    /* NegZ */ {2, 1, 0, -1},
    /* PosZ */ {2, 0, 1, +1},
}};

/// `component` of the cross product of a frame's two basis vectors.
[[nodiscard]] constexpr std::int32_t crossComponent(const FaceFrame& frame,
                                                    std::int32_t component) noexcept
{
    const auto basis = [](std::int32_t axis, std::int32_t which) {
        return axis == which ? 1 : 0;
    };
    const std::int32_t a = (component + 1) % 3;
    const std::int32_t b = (component + 2) % 3;
    return basis(frame.uAxis, a) * basis(frame.vAxis, b) -
           basis(frame.uAxis, b) * basis(frame.vAxis, a);
}

/// The table above duplicates GreedyMesher.cpp's, which is file-local there.
/// Sharing it would mean adding to mesh/MeshData.hpp, which is a frozen contract,
/// for the sake of six integers - so it is copied instead. Duplication is only
/// defensible if it is *checked*, so rather than a comment asking the reader to
/// trust that the two agree, the geometric property the table exists to encode is
/// asserted here: a transposed or mis-signed row fails to compile rather than
/// shipping a chunk whose carved faces are all backface-culled.
[[nodiscard]] constexpr bool frameIsRightHanded(const FaceFrame& frame) noexcept
{
    for (std::int32_t component = 0; component < 3; ++component) {
        const std::int32_t expected = component == frame.nAxis ? frame.sign : 0;
        if (crossComponent(frame, component) != expected) {
            return false;
        }
    }
    return true;
}

static_assert(frameIsRightHanded(kFaceFrames[0]), "NegX frame is not right-handed");
static_assert(frameIsRightHanded(kFaceFrames[1]), "PosX frame is not right-handed");
static_assert(frameIsRightHanded(kFaceFrames[2]), "NegY frame is not right-handed");
static_assert(frameIsRightHanded(kFaceFrames[3]), "PosY frame is not right-handed");
static_assert(frameIsRightHanded(kFaceFrames[4]), "NegZ frame is not right-handed");
static_assert(frameIsRightHanded(kFaceFrames[5]), "PosZ frame is not right-handed");

/// Sub-voxel index from frame-relative coordinates. `n` runs along the face
/// normal, `u`/`v` across the face.
[[nodiscard]] constexpr std::size_t frameIndex(const FaceFrame& frame, std::int32_t n,
                                               std::int32_t u, std::int32_t v) noexcept
{
    std::int32_t coord[3]{};
    coord[static_cast<std::size_t>(frame.nAxis)] = n;
    coord[static_cast<std::size_t>(frame.uAxis)] = u;
    coord[static_cast<std::size_t>(frame.vAxis)] = v;
    return subVoxelIndex(coord[0], coord[1], coord[2]);
}

/// Ambient occlusion for a whole face, from the four blocks edge-adjacent to the
/// cell the face points into.
///
/// Per FACE, not per corner: SubVoxelMesh.hpp requires one AO value per
/// (block, direction), and a per-corner gradient across a surface that is at most
/// one block wide would not be visible anyway. Fewer open sides means a deeper
/// pocket means darker.
[[nodiscard]] std::uint8_t faceAmbientOcclusion(const ChunkNeighbourhood& neighbourhood,
                                                const BlockRegistry& registry,
                                                const BlockPos& outside, Direction direction)
{
    const FaceFrame& frame = kFaceFrames[static_cast<std::size_t>(direction)];

    std::int32_t occluders = 0;
    for (const std::int32_t axis : {frame.uAxis, frame.vAxis}) {
        for (const std::int32_t step : {-1, +1}) {
            std::int32_t offset[3]{};
            offset[static_cast<std::size_t>(axis)] = step;
            const BlockId side = neighbourhood.getBlockLocal(
                outside.x + offset[0], outside.y + offset[1], outside.z + offset[2]);
            occluders += registry.isOpaque(side) ? 1 : 0;
        }
    }

    // 0 occluders -> fully open (kMaxAoLevel), 3 or 4 -> fully occluded.
    const std::int32_t level = kMaxAoLevel - occluders;
    return static_cast<std::uint8_t>(level < 0 ? 0 : level);
}

}  // namespace

// -------------------------------------------------------- SubVoxelMeshData --

void SubVoxelMeshData::addQuad(const SubVoxelVertexAttributes& origin,
                               const SubVoxelVertexAttributes& u,
                               const SubVoxelVertexAttributes& uv,
                               const SubVoxelVertexAttributes& v)
{
    const auto base = static_cast<std::uint32_t>(vertices.size());
    vertices.push_back(packSubVoxelVertex(origin));
    vertices.push_back(packSubVoxelVertex(u));
    vertices.push_back(packSubVoxelVertex(uv));
    vertices.push_back(packSubVoxelVertex(v));

    // Same diagonal rule as MeshLayerData::addQuad, kept so the two paths cannot
    // disagree about winding. AO here is per face, so the four corners always
    // tie and the second split is always the one taken; the comparison is written
    // out anyway because the day AO becomes per-corner this must already be right
    // rather than needing to be remembered.
    if (static_cast<std::uint32_t>(origin.ao) + uv.ao > static_cast<std::uint32_t>(u.ao) + v.ao) {
        indices.insert(indices.end(),
                       {base + 0u, base + 1u, base + 2u, base + 0u, base + 2u, base + 3u});
    } else {
        indices.insert(indices.end(),
                       {base + 1u, base + 2u, base + 3u, base + 1u, base + 3u, base + 0u});
    }
}

// ----------------------------------------------------------- SubVoxelMesher --

SubVoxelMesher::SubVoxelMesher(const BlockRegistry& registry) noexcept : m_registry(&registry) {}

bool SubVoxelMesher::mesh(const ChunkNeighbourhood& neighbourhood, SubVoxelMeshData& out)
{
    out.clear();
    m_stats = Stats{};

    const Chunk* centre = neighbourhood.centre();
    if (centre == nullptr || !centre->hasSubVoxelDamage()) {
        // The whole point of the sparse store: untouched terrain leaves here
        // after one bool test, having allocated and scanned nothing.
        return false;
    }

    // Damage is spatially clustered, so the previous chunk's quad count is a good
    // predictor of this one's and this removes essentially every reallocation
    // without ever reserving a worst case.
    if (m_quadHint != 0) {
        out.vertices.reserve(m_quadHint * 4);
        out.indices.reserve(m_quadHint * 6);
    }

    // sortedEntries() rather than forEach(): the store is a hash map, whose
    // iteration order is unspecified. Two runs of the same seed and the same
    // edits must produce byte-identical geometry, or a determinism check that
    // hashes the vertex stream fails for a reason that has nothing to do with
    // the world being different.
    for (const auto& [blockIndex, grid] : centre->subVoxels().sortedEntries()) {
        meshBlock(neighbourhood, blockIndex, grid, out);
    }

    m_quadHint = m_stats.quadsEmitted;
    return !out.empty();
}

SubVoxelMeshData SubVoxelMesher::mesh(const ChunkNeighbourhood& neighbourhood)
{
    SubVoxelMeshData data;
    mesh(neighbourhood, data);
    return data;
}

void SubVoxelMesher::meshBlock(const ChunkNeighbourhood& neighbourhood, std::size_t blockIndex,
                               const SubVoxelGrid& grid, SubVoxelMeshData& out)
{
    VOXL_ASSERT(blockIndex < kChunkVolume, "sub-voxel block index outside the chunk");
    VOXL_ASSERT(!grid.empty() && !grid.full(),
                "a store entry must be strictly partial - see the SubVoxel.hpp invariant");
    VOXL_ASSERT(grid.material != blocks::Air, "a partial block cannot be air");

    // localIndex packs x fastest, then z, then y.
    const auto x = static_cast<std::int32_t>(blockIndex % static_cast<std::size_t>(kChunkSize));
    const auto z = static_cast<std::int32_t>((blockIndex / static_cast<std::size_t>(kChunkSize)) %
                                             static_cast<std::size_t>(kChunkSize));
    const auto y = static_cast<std::int32_t>(blockIndex / static_cast<std::size_t>(kChunkSize *
                                                                                  kChunkSize));

    const BlockPos  blockLocal{x, y, z};
    const BlockType& type = m_registry->get(grid.material);

    std::array<NeighbourFace, kDirectionCount> faces{};
    resolveNeighbours(neighbourhood, blockLocal, grid.material, faces);

    const std::array<std::int32_t, 3> blockBase = {x * kSubVoxelResolution,
                                                   y * kSubVoxelResolution,
                                                   z * kSubVoxelResolution};

    const std::size_t quadsBefore = m_stats.quadsEmitted;
    for (std::size_t direction = 0; direction < kDirectionCount; ++direction) {
        sweep(static_cast<Direction>(direction), grid, faces[direction], blockBase,
              type.textureLayers[direction], out);
    }
    if (m_stats.quadsEmitted != quadsBefore) {
        ++m_stats.blocksMeshed;
    }
}

void SubVoxelMesher::resolveNeighbours(const ChunkNeighbourhood& neighbourhood,
                                       const BlockPos& blockLocal, BlockId material,
                                       std::array<NeighbourFace, kDirectionCount>& out) const
{
    const BlockRegistry& registry = *m_registry;

    // Gathered in the first pass because the fallback for a face that points into
    // solid material is the brightest of the OTHER sides, which is not known
    // until every side has been looked at.
    std::array<std::uint8_t, kDirectionCount> sideLight{};
    /// The neighbour is air, water, glass - something whose stored light really is
    /// the light in that space. Decides the DIRECT sample.
    std::array<bool, kDirectionCount> sideIsTransparent{};
    /// The neighbour does not seal this block off. Decides what feeds the FALLBACK.
    std::array<bool, kDirectionCount> sideIsOpen{};

    for (std::size_t direction = 0; direction < kDirectionCount; ++direction) {
        const glm::ivec3& offset = kDirectionOffsets[direction];
        const BlockPos    outside{blockLocal.x + offset.x, blockLocal.y + offset.y,
                                  blockLocal.z + offset.z};

        const BlockId neighbourBlock =
            neighbourhood.getBlockLocal(outside.x, outside.y, outside.z);

        NeighbourFace& face = out[direction];
        face.hidesFace      = registry.facesHidden(material, neighbourBlock);
        face.ao = faceAmbientOcclusion(neighbourhood, registry, outside,
                                       static_cast<Direction>(direction));

        // A partially destroyed neighbour hides only the sub-voxels it still
        // has, so occlusion for this face is decided per sub-voxel against its
        // grid instead of once for the whole face.
        const Chunk* chunk = neighbourhood.chunkAt(outside.x >> kChunkSizeLog2,
                                                   outside.y >> kChunkSizeLog2,
                                                   outside.z >> kChunkSizeLog2);
        if (chunk != nullptr && chunk->hasSubVoxelDamage()) {
            face.grid = chunk->subVoxels().find(localIndex(outside.x & kChunkSizeMask,
                                                           outside.y & kChunkSizeMask,
                                                           outside.z & kChunkSizeMask));
        }

        // A PARTIALLY DESTROYED NEIGHBOUR DOES NOT SEAL THIS BLOCK OFF.
        //
        // That is the rule Chunk::isBlockWhole states and GreedyMesher already
        // honours by rewriting damaged blocks to air in its cache; the line above
        // honours it for occlusion. Honouring it for occlusion but not for LIGHT
        // is what rendered a bored tunnel pitch black: every block around the
        // bore is partial and its id in ChunkStorage is still Stone, so the
        // sealed-pocket probe found nothing but "solid" neighbours and stayed at
        // zero - with daylight visible through the far end of the hole. See
        // docs/VISUAL_REVIEW.md.
        //
        // It is NOT treated as transparent, though. Its stored light is the light
        // of the solid block it still nominally is - zero for anything the
        // generator never saw sky through - so sampling it directly would drag a
        // carved pocket DARKER than the open-side fallback it replaced. Partial
        // neighbours therefore feed the fallback pool and nothing else.
        sideIsTransparent[direction] = !registry.isOpaque(neighbourBlock);
        sideIsOpen[direction]        = sideIsTransparent[direction] || face.grid != nullptr;
        sideLight[direction] = neighbourhood.getLightLocal(outside.x, outside.y, outside.z);
    }

    // A pocket should be dim, not pitch black; the per-face AO term above is what
    // actually darkens it.
    std::uint8_t fallbackSunlight   = 0;
    std::uint8_t fallbackBlockLight = 0;
    for (std::size_t direction = 0; direction < kDirectionCount; ++direction) {
        if (!sideIsOpen[direction]) {
            continue;
        }
        fallbackSunlight =
            std::max(fallbackSunlight, ChunkStorage::unpackSunlight(sideLight[direction]));
        fallbackBlockLight =
            std::max(fallbackBlockLight, ChunkStorage::unpackBlockLight(sideLight[direction]));
    }

    for (std::size_t direction = 0; direction < kDirectionCount; ++direction) {
        NeighbourFace& face = out[direction];
        // An intact block's face is lit by the block ACROSS it, so that is what a
        // carved surface replacing that face inherits - taken at exactly the
        // position GreedyMesher takes it. Reading the parent block's own light
        // instead would render every carved surface black, because the inside of
        // an opaque block is dark.
        if (sideIsTransparent[direction]) {
            face.sunlight   = ChunkStorage::unpackSunlight(sideLight[direction]);
            face.blockLight = ChunkStorage::unpackBlockLight(sideLight[direction]);
        } else {
            face.sunlight   = fallbackSunlight;
            face.blockLight = fallbackBlockLight;
        }
    }
}

void SubVoxelMesher::sweep(Direction direction, const SubVoxelGrid& grid,
                           const NeighbourFace& face,
                           const std::array<std::int32_t, 3>& blockBase,
                           std::uint16_t textureLayer, SubVoxelMeshData& out)
{
    const FaceFrame& frame = kFaceFrames[static_cast<std::size_t>(direction)];

    // The sub-voxel that lies across the block boundary has the same u and v and
    // sits at the far end of the neighbour's normal axis.
    const std::int32_t acrossN = frame.sign > 0 ? 0 : kSubVoxelResolution - 1;

    for (std::int32_t n = 0; n < kSubVoxelResolution; ++n) {
        // Occupancy mask for this slice: a face is emitted where this sub-voxel
        // is present and the one in front of it is not.
        std::array<std::uint8_t, kSubVoxelResolution * kSubVoxelResolution> mask{};
        std::int32_t faceCount = 0;

        const std::int32_t forward = n + frame.sign;
        const bool         insideBlock = forward >= 0 && forward < kSubVoxelResolution;

        for (std::int32_t v = 0; v < kSubVoxelResolution; ++v) {
            for (std::int32_t u = 0; u < kSubVoxelResolution; ++u) {
                if (!grid.test(frameIndex(frame, n, u, v))) {
                    continue;
                }
                bool occluded = false;
                if (insideBlock) {
                    occluded = grid.test(frameIndex(frame, forward, u, v));
                } else if (face.grid != nullptr) {
                    occluded = face.grid->test(frameIndex(frame, acrossN, u, v));
                } else {
                    occluded = face.hidesFace;
                }
                if (occluded) {
                    continue;
                }
                mask[static_cast<std::size_t>(v) * kSubVoxelResolution +
                     static_cast<std::size_t>(u)] = 1u;
                ++faceCount;
            }
        }

        if (faceCount == 0) {
            continue;
        }
        m_stats.facesEmitted += static_cast<std::size_t>(faceCount);

        // The face plane: a positive face sits on the far side of its sub-voxel,
        // a negative face on the near side.
        const std::int32_t planeN =
            blockBase[static_cast<std::size_t>(frame.nAxis)] + n + (frame.sign > 0 ? 1 : 0);

        // Greedy rectangle extraction, identical in shape to GreedyMesher's but
        // over a boolean 8x8: every face in this sweep shares one block, one
        // direction, one material, one light sample and one AO value, so there is
        // no per-cell key to compare - presence is the whole criterion.
        for (std::int32_t v = 0; v < kSubVoxelResolution; ++v) {
            for (std::int32_t u = 0; u < kSubVoxelResolution;) {
                const auto cell =
                    static_cast<std::size_t>(v) * kSubVoxelResolution + static_cast<std::size_t>(u);
                if (mask[cell] == 0u) {
                    ++u;
                    continue;
                }

                std::int32_t width = 1;
                while (u + width < kSubVoxelResolution &&
                       mask[cell + static_cast<std::size_t>(width)] != 0u) {
                    ++width;
                }

                std::int32_t height = 1;
                while (v + height < kSubVoxelResolution) {
                    const auto rowStart =
                        static_cast<std::size_t>(v + height) * kSubVoxelResolution +
                        static_cast<std::size_t>(u);
                    bool rowMatches = true;
                    for (std::int32_t k = 0; k < width; ++k) {
                        if (mask[rowStart + static_cast<std::size_t>(k)] == 0u) {
                            rowMatches = false;
                            break;
                        }
                    }
                    if (!rowMatches) {
                        break;
                    }
                    ++height;
                }

                for (std::int32_t dv = 0; dv < height; ++dv) {
                    const auto rowStart = static_cast<std::size_t>(v + dv) * kSubVoxelResolution +
                                          static_cast<std::size_t>(u);
                    for (std::int32_t du = 0; du < width; ++du) {
                        mask[rowStart + static_cast<std::size_t>(du)] = 0u;
                    }
                }

                std::int32_t base[3]{};
                base[static_cast<std::size_t>(frame.nAxis)] = planeN;
                base[static_cast<std::size_t>(frame.uAxis)] =
                    blockBase[static_cast<std::size_t>(frame.uAxis)] + u;
                base[static_cast<std::size_t>(frame.vAxis)] =
                    blockBase[static_cast<std::size_t>(frame.vAxis)] + v;

                SubVoxelVertexAttributes corner;
                corner.direction    = direction;
                corner.textureLayer = textureLayer;
                corner.width        = static_cast<std::uint8_t>(width);
                corner.height       = static_cast<std::uint8_t>(height);
                corner.sunlight     = face.sunlight;
                corner.blockLight   = face.blockLight;
                corner.ao           = face.ao;

                const auto at = [&corner, &base, &frame](std::int32_t du, std::int32_t dv,
                                                          QuadCorner which) {
                    std::int32_t position[3] = {base[0], base[1], base[2]};
                    position[static_cast<std::size_t>(frame.uAxis)] += du;
                    position[static_cast<std::size_t>(frame.vAxis)] += dv;

                    // packSubVoxelVertex shifts without masking, so an
                    // out-of-range coordinate silently corrupts the neighbouring
                    // field instead of merely clipping. Asserted on the final
                    // coordinates rather than on a bound derived from base and
                    // the extents: which axis `width` grows along depends on the
                    // face frame, and hand-deriving that per axis is precisely
                    // the arithmetic this guard exists to catch.
                    VOXL_ASSERT(position[0] >= 0 && position[1] >= 0 && position[2] >= 0,
                                "sub-voxel quad corner is negative");
                    VOXL_ASSERT(
                        position[0] <= static_cast<std::int32_t>(kSubVtxMaxPos) &&
                            position[1] <= static_cast<std::int32_t>(kSubVtxMaxPos) &&
                            position[2] <= static_cast<std::int32_t>(kSubVtxMaxPos),
                        "sub-voxel quad corner does not fit the 9-bit position field");

                    SubVoxelVertexAttributes vertex = corner;
                    vertex.x      = static_cast<std::uint16_t>(position[0]);
                    vertex.y      = static_cast<std::uint16_t>(position[1]);
                    vertex.z      = static_cast<std::uint16_t>(position[2]);
                    vertex.corner = which;
                    return vertex;
                };

                out.addQuad(at(0, 0, QuadCorner::Origin), at(width, 0, QuadCorner::U),
                            at(width, height, QuadCorner::UV), at(0, height, QuadCorner::V));
                ++m_stats.quadsEmitted;

                u += width;
            }
        }
    }
}

}  // namespace voxl
