#include "mesh/GreedyMesher.hpp"

#include "core/Log.hpp"
#include "world/Chunk.hpp"
#include "world/ChunkStorage.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace voxl::detail {

/// Flat, padded copies of everything the sweep reads.
///
/// The padding is one voxel on every side: the sweep needs the neighbour voxel
/// across each face (culling) and the eight voxels ringing it in the face plane
/// (ambient occlusion), all of which fall outside the centre chunk for the
/// boundary slices. Copying the skirt in once means the inner loop never has to
/// ask "is this coordinate still inside the chunk?".
struct MesherScratch {
    static constexpr std::int32_t  kDim    = kChunkSize + 2;
    static constexpr std::size_t   kVolume = static_cast<std::size_t>(kDim) * kDim * kDim;

    // Strides mirror localIndex()'s ordering (x fastest, then z, then y) so the
    // interior copy walks both the source and the destination linearly.
    static constexpr std::ptrdiff_t kStrideX = 1;
    static constexpr std::ptrdiff_t kStrideZ = kDim;
    static constexpr std::ptrdiff_t kStrideY = static_cast<std::ptrdiff_t>(kDim) * kDim;

    /// `x`, `y`, `z` are centre-chunk-local and may range over [-1, kChunkSize].
    [[nodiscard]] static constexpr std::size_t index(std::int32_t x, std::int32_t y,
                                                     std::int32_t z) noexcept
    {
        return static_cast<std::size_t>((x + 1) * kStrideX + (z + 1) * kStrideZ +
                                        (y + 1) * kStrideY);
    }

    std::array<BlockId, kVolume>      block{};
    std::array<std::uint8_t, kVolume> light{};
    /// Per-voxel face-culling and occlusion flags, resolved from the registry
    /// during the copy. Ambient occlusion samples nine voxels per visible face
    /// and culling samples one per candidate face; going back to the registry
    /// each time means chasing a ~120-byte BlockType instead of reading a byte
    /// that is already in L1 next to the block id.
    std::array<std::uint8_t, kVolume> flags{};

    /// One 32x32 slice of merge keys. Rebuilt (fully overwritten) per slice, so
    /// it never needs clearing between slices.
    std::array<std::uint64_t, static_cast<std::size_t>(kChunkSize) * kChunkSize> mask{};

    std::int32_t boundsMin[3]{};
    std::int32_t boundsMax[3]{};
};

}  // namespace voxl::detail

namespace voxl {
namespace {

using detail::MesherScratch;

// -------------------------------------------------------------- block flags --

/// BlockRegistry::facesHidden and the AO sampler need exactly two bits of a
/// block: whether it hides the face behind it outright, and whether it culls
/// against an identical neighbour. Both are cached per voxel in
/// MesherScratch::flags.
constexpr std::uint8_t kFlagOpaque      = 1u << 0;
constexpr std::uint8_t kFlagSelfCulling = 1u << 1;

/// Mirror of BlockRegistry::facesHidden operating on the cached flags of the
/// neighbour. Kept in lockstep by the assert at the single call site.
[[nodiscard]] constexpr bool facesHiddenFromFlags(BlockId self, BlockId neighbour,
                                                  std::uint8_t neighbourFlags) noexcept
{
    if (neighbour == blocks::Air) {
        return false;
    }
    if ((neighbourFlags & kFlagOpaque) != 0) {
        return true;
    }
    return self == neighbour && (neighbourFlags & kFlagSelfCulling) != 0;
}

// -------------------------------------------------------------- merge key --
//
//  bits  0..15   block id            (never 0 for a real face, so key == 0 is
//                                     an unambiguous "no face here")
//  bits 16..25   corner 0 (Origin)   ao 2 | sunlight 4 | blockLight 4
//  bits 26..35   corner 1 (+U)
//  bits 36..45   corner 2 (+U+V)
//  bits 46..55   corner 3 (+V)
//
// Two faces merge if and only if their keys are equal. That single comparison
// enforces every rule at once: same block (hence same render layer and, for a
// fixed direction, same texture layer) and identical ambient occlusion and light
// at all four corners. Merging across a lighting or AO difference is what
// produces the banded, blotchy look that gives greedy meshers a bad name.

constexpr std::uint64_t kKeyBlockMask   = 0xFFFFull;
constexpr int           kKeyCornerShift = 16;
constexpr int           kKeyCornerBits  = 10;
constexpr std::uint64_t kKeyCornerMask  = 0x3FFull;

[[nodiscard]] constexpr std::uint64_t encodeCorner(std::uint32_t ao, std::uint32_t sunlight,
                                                   std::uint32_t blockLight) noexcept
{
    return static_cast<std::uint64_t>(ao | (sunlight << 2) | (blockLight << 6));
}

[[nodiscard]] constexpr std::uint32_t cornerOf(std::uint64_t key, int corner) noexcept
{
    return static_cast<std::uint32_t>((key >> (kKeyCornerShift + kKeyCornerBits * corner)) &
                                      kKeyCornerMask);
}

// ------------------------------------------------------------ face frames --

/// Per-direction tangent frame. `uAxis` and `vAxis` are chosen so that
/// u_hat x v_hat == the outward normal, which makes the corner order
/// (origin, +U, +U+V, +V) counter-clockwise seen from outside - exactly what
/// GL_CCW front-facing and MeshLayerData::addQuad expect. Getting this table
/// wrong shows up as whole faces of the world being backface-culled.
struct FaceFrame {
    std::int32_t nAxis;  ///< 0 = x, 1 = y, 2 = z
    std::int32_t uAxis;
    std::int32_t vAxis;
    std::int32_t sign;   ///< +1 for the positive face of the voxel, -1 otherwise
};

constexpr std::array<FaceFrame, kDirectionCount> kFaceFrames = {{
    /* NegX */ {0, 2, 1, -1},  // z x y = -x
    /* PosX */ {0, 1, 2, +1},  // y x z = +x
    /* NegY */ {1, 0, 2, -1},  // x x z = -y
    /* PosY */ {1, 2, 0, +1},  // z x x = +y
    /* NegZ */ {2, 1, 0, -1},  // y x x = -z
    /* PosZ */ {2, 0, 1, +1},  // x x y = +z
}};

[[nodiscard]] constexpr std::ptrdiff_t axisStride(std::int32_t axis) noexcept
{
    return axis == 0 ? MesherScratch::kStrideX
                     : (axis == 1 ? MesherScratch::kStrideY : MesherScratch::kStrideZ);
}

// -------------------------------------------------- ambient occlusion ring --
//
// For a visible face, `q` is the (necessarily non-opaque) voxel the face points
// into. The nine voxels of the face-plane 3x3 around `q` are gathered once and
// indexed as ring[(dv + 1) * 3 + (du + 1)], du/dv being offsets along the
// frame's u and v axes. Index 4 is `q` itself.
constexpr std::size_t kRingCentre = 4;

/// {side1, side2, corner} ring indices for corners Origin, +U, +U+V, +V.
constexpr std::array<std::array<std::size_t, 3>, 4> kCornerRing = {{
    {{3, 1, 0}},  // Origin: -u, -v, (-u,-v)
    {{5, 1, 2}},  // +U:     +u, -v, (+u,-v)
    {{5, 7, 8}},  // +U+V:   +u, +v, (+u,+v)
    {{3, 7, 6}},  // +V:     -u, +v, (-u,+v)
}};

/// The standard three-sample corner darkening.
///
/// Two opaque sides meeting at a corner fully close it (level 0) even though the
/// diagonal voxel is visible through neither: without this special case an inner
/// wall corner is brighter than the flat wall beside it, which looks inverted.
[[nodiscard]] constexpr std::uint32_t ambientOcclusion(std::uint32_t side1, std::uint32_t side2,
                                                       std::uint32_t corner) noexcept
{
    if (side1 != 0 && side2 != 0) {
        return 0;
    }
    return static_cast<std::uint32_t>(kMaxAoLevel) - (side1 + side2 + corner);
}

}  // namespace

std::uint16_t waterSurfaceTextureLayer(const BlockRegistry& registry) noexcept
{
    return registry.get(blocks::Water).textureLayers[static_cast<std::size_t>(Direction::PosY)];
}

// ------------------------------------------------------------- lifecycle --

GreedyMesher::GreedyMesher(const BlockRegistry& registry)
    : m_registry(&registry), m_scratch(std::make_unique<detail::MesherScratch>())
{
    m_blockFlags.resize(registry.size());
    for (std::size_t id = 0; id < registry.size(); ++id) {
        const BlockType& type = registry.get(static_cast<BlockId>(id));
        m_blockFlags[id] = static_cast<std::uint8_t>((type.opaque ? kFlagOpaque : 0u) |
                                                     (type.selfCulling() ? kFlagSelfCulling : 0u));
    }
}

std::uint8_t GreedyMesher::blockFlags(BlockId id) const noexcept
{
    // Same fallback as BlockRegistry::get: an id from a newer save reads as air.
    return id < m_blockFlags.size() ? m_blockFlags[id] : m_blockFlags[blocks::Air];
}

GreedyMesher::~GreedyMesher()                                   = default;
GreedyMesher::GreedyMesher(GreedyMesher&&) noexcept             = default;
GreedyMesher& GreedyMesher::operator=(GreedyMesher&&) noexcept  = default;

// ----------------------------------------------------------------- cache --

void GreedyMesher::loadCache(const ChunkNeighbourhood& neighbourhood)
{
    MesherScratch&      scratch = *m_scratch;
    const Chunk*        centre  = neighbourhood.centre();
    const ChunkStorage& storage = centre->storage();

    // Rows of 32 are contiguous in both layouts (localIndex and kStrideX both
    // step x fastest), so a uniform section or a section with uniform light is
    // a fill or a memcpy per row rather than 32 palette reads.
    const bool         uniformBlocks = storage.isUniform();
    const BlockId      uniformBlock  = uniformBlocks ? storage.uniformValue() : blocks::Air;
    const std::uint8_t uniformFlags  = blockFlags(uniformBlock);
    const std::uint8_t* lightSource  = storage.hasLightData() ? storage.lightData().data() : nullptr;
    const std::uint8_t  uniformLight = storage.uniformLight();

    for (std::int32_t y = 0; y < kChunkSize; ++y) {
        for (std::int32_t z = 0; z < kChunkSize; ++z) {
            const std::size_t source      = localIndex(0, y, z);
            const std::size_t destination = MesherScratch::index(0, y, z);

            if (uniformBlocks) {
                std::fill_n(scratch.block.begin() + static_cast<std::ptrdiff_t>(destination),
                            kChunkSize, uniformBlock);
                std::fill_n(scratch.flags.begin() + static_cast<std::ptrdiff_t>(destination),
                            kChunkSize, uniformFlags);
            } else {
                for (std::int32_t x = 0; x < kChunkSize; ++x) {
                    const std::size_t dst = destination + static_cast<std::size_t>(x);
                    const BlockId     id  = storage.get(source + static_cast<std::size_t>(x));
                    scratch.block[dst] = id;
                    scratch.flags[dst] = blockFlags(id);
                }
            }

            if (lightSource != nullptr) {
                std::copy_n(lightSource + source, kChunkSize,
                            scratch.light.begin() + static_cast<std::ptrdiff_t>(destination));
            } else {
                std::fill_n(scratch.light.begin() + static_cast<std::ptrdiff_t>(destination),
                            kChunkSize, uniformLight);
            }
        }
    }

    // Skirt: the six padded faces, through the neighbourhood so the lookup
    // genuinely crosses the chunk boundary. Treating out-of-chunk as air here is
    // the classic bug - it leaves a full sheet of interior faces at every seam.
    // Edges and corners are written more than once; that is 400 redundant stores
    // in exchange for three tight loops instead of twenty-six special cases.
    const auto store = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
        const std::size_t dst = MesherScratch::index(x, y, z);
        const BlockId     id  = neighbourhood.getBlockLocal(x, y, z);
        scratch.block[dst] = id;
        scratch.light[dst] = neighbourhood.getLightLocal(x, y, z);
        scratch.flags[dst] = blockFlags(id);
    };

    for (std::int32_t a = -1; a <= kChunkSize; ++a) {
        for (std::int32_t b = -1; b <= kChunkSize; ++b) {
            store(-1, a, b);
            store(kChunkSize, a, b);
            store(a, -1, b);
            store(a, kChunkSize, b);
            store(a, b, -1);
            store(a, b, kChunkSize);
        }
    }
}

// ----------------------------------------------------------------- sweep --

void GreedyMesher::sweep(Direction direction, ChunkMeshData& out)
{
    MesherScratch&   scratch = *m_scratch;
    const FaceFrame& frame   = kFaceFrames[static_cast<std::size_t>(direction)];

    const std::ptrdiff_t strideN = axisStride(frame.nAxis);
    const std::ptrdiff_t strideU = axisStride(frame.uAxis);
    const std::ptrdiff_t strideV = axisStride(frame.vAxis);
    const std::ptrdiff_t toNeighbour = frame.sign * strideN;

    // The (u, v) frame is fixed by the winding rule, but the order the mask is
    // FILLED in is not. Three of the six directions have a u axis whose stride
    // through the voxel cache is 1156 entries; walking that innermost touches a
    // fresh cache line every step. Iterate whichever tangent axis is denser in
    // memory innermost instead - the mask itself is 8 KB and stays in L1 however
    // it is addressed, so the strided access moves to where it is free.
    const bool           uIsInner    = strideU <= strideV;
    const std::ptrdiff_t innerStride = uIsInner ? strideU : strideV;
    const std::ptrdiff_t outerStride = uIsInner ? strideV : strideU;
    const std::size_t    innerCellStep = uIsInner ? 1u : static_cast<std::size_t>(kChunkSize);
    const std::size_t    outerCellStep = uIsInner ? static_cast<std::size_t>(kChunkSize) : 1u;

    const std::ptrdiff_t origin = static_cast<std::ptrdiff_t>(MesherScratch::index(0, 0, 0));

    for (std::int32_t n = 0; n < kChunkSize; ++n) {
        const std::ptrdiff_t sliceBase = origin + static_cast<std::ptrdiff_t>(n) * strideN;

        std::size_t faceCount = 0;
        for (std::int32_t outer = 0; outer < kChunkSize; ++outer) {
            std::ptrdiff_t p    = sliceBase + static_cast<std::ptrdiff_t>(outer) * outerStride;
            std::size_t    cell = static_cast<std::size_t>(outer) * outerCellStep;

            for (std::int32_t inner = 0; inner < kChunkSize;
                 ++inner, p += innerStride, cell += innerCellStep) {
                const BlockId self = scratch.block[static_cast<std::size_t>(p)];
                if (self == blocks::Air) {
                    scratch.mask[cell] = 0;
                    continue;
                }

                const std::ptrdiff_t q           = p + toNeighbour;
                const BlockId        other       = scratch.block[static_cast<std::size_t>(q)];
                const std::uint8_t   otherFlags  = scratch.flags[static_cast<std::size_t>(q)];
                VOXL_ASSERT(facesHiddenFromFlags(self, other, otherFlags) ==
                                m_registry->facesHidden(self, other),
                            "cached face flags disagree with BlockRegistry::facesHidden");
                if (facesHiddenFromFlags(self, other, otherFlags)) {
                    scratch.mask[cell] = 0;
                    continue;
                }

                // Gather the 3x3 ring around `q` in this face's plane once and
                // derive all four corners from it: nine samples instead of the
                // sixteen a per-corner gather would take.
                std::uint32_t ringOpaque[9]{};
                std::uint32_t ringSunlight[9]{};
                std::uint32_t ringBlockLight[9]{};
                for (std::int32_t dv = -1; dv <= 1; ++dv) {
                    for (std::int32_t du = -1; du <= 1; ++du) {
                        const std::size_t slot =
                            static_cast<std::size_t>((dv + 1) * 3 + (du + 1));
                        const std::size_t sample = static_cast<std::size_t>(
                            q + static_cast<std::ptrdiff_t>(du) * strideU +
                            static_cast<std::ptrdiff_t>(dv) * strideV);
                        const std::uint8_t packed = scratch.light[sample];
                        ringOpaque[slot] = (scratch.flags[sample] & kFlagOpaque) != 0 ? 1u : 0u;
                        ringSunlight[slot]   = ChunkStorage::unpackSunlight(packed);
                        ringBlockLight[slot] = ChunkStorage::unpackBlockLight(packed);
                    }
                }

                std::uint64_t key = static_cast<std::uint64_t>(self) & kKeyBlockMask;
                for (int corner = 0; corner < 4; ++corner) {
                    const std::size_t i1 = kCornerRing[static_cast<std::size_t>(corner)][0];
                    const std::size_t i2 = kCornerRing[static_cast<std::size_t>(corner)][1];
                    const std::size_t ic = kCornerRing[static_cast<std::size_t>(corner)][2];

                    const std::uint32_t ao =
                        ambientOcclusion(ringOpaque[i1], ringOpaque[i2], ringOpaque[ic]);

                    // Smooth light: average the four voxels touching this corner
                    // on the outside of the face, skipping opaque ones - their
                    // light is meaningless and averaging their zero in would
                    // draw a dark halo around every solid corner. `q` is
                    // non-opaque by construction (an opaque neighbour would have
                    // hidden the face), so the count is never zero.
                    const std::size_t samples[4] = {kRingCentre, i1, i2, ic};
                    std::uint32_t sunSum   = 0;
                    std::uint32_t blockSum = 0;
                    std::uint32_t count    = 0;
                    for (const std::size_t slot : samples) {
                        if (ringOpaque[slot] == 0) {
                            sunSum += ringSunlight[slot];
                            blockSum += ringBlockLight[slot];
                            ++count;
                        }
                    }
                    // Integer division, deliberately: the result must be
                    // bit-identical on every machine so two chunks meshed on
                    // different threads agree along their shared seam.
                    key |= encodeCorner(ao, sunSum / count, blockSum / count)
                           << (kKeyCornerShift + kKeyCornerBits * corner);
                }

                scratch.mask[cell] = key;
                ++faceCount;
            }
        }

        if (faceCount == 0) {
            continue;
        }
        m_stats.facesEmitted += faceCount;

        // Greedy rectangle extraction: grow along u first, then along v while
        // every cell of the candidate row still matches. A flat 1024-entry array
        // keeps this entirely in L1; a map keyed by face attributes (the naive
        // formulation) costs more than the merging saves.
        for (std::int32_t v = 0; v < kChunkSize; ++v) {
            for (std::int32_t u = 0; u < kChunkSize;) {
                const std::size_t cell = static_cast<std::size_t>(v) * kChunkSize +
                                         static_cast<std::size_t>(u);
                const std::uint64_t key = scratch.mask[cell];
                if (key == 0) {
                    ++u;
                    continue;
                }

                std::int32_t width = 1;
                while (u + width < kChunkSize &&
                       scratch.mask[cell + static_cast<std::size_t>(width)] == key) {
                    ++width;
                }

                std::int32_t height = 1;
                while (v + height < kChunkSize) {
                    const std::size_t rowStart =
                        static_cast<std::size_t>(v + height) * kChunkSize +
                        static_cast<std::size_t>(u);
                    bool rowMatches = true;
                    for (std::int32_t k = 0; k < width; ++k) {
                        if (scratch.mask[rowStart + static_cast<std::size_t>(k)] != key) {
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
                    const std::size_t rowStart = static_cast<std::size_t>(v + dv) * kChunkSize +
                                                 static_cast<std::size_t>(u);
                    for (std::int32_t du = 0; du < width; ++du) {
                        scratch.mask[rowStart + static_cast<std::size_t>(du)] = 0;
                    }
                }

                emitQuad(out, direction, n, u, v, width, height, key);
                u += width;
            }
        }
    }
}

// ------------------------------------------------------------------ emit --

void GreedyMesher::emitQuad(ChunkMeshData& out, Direction direction, std::int32_t n,
                            std::int32_t u, std::int32_t v, std::int32_t w, std::int32_t h,
                            std::uint64_t key)
{
    const BlockRegistry& registry = *m_registry;
    const FaceFrame&     frame    = kFaceFrames[static_cast<std::size_t>(direction)];

    const BlockId    block = static_cast<BlockId>(key & kKeyBlockMask);
    const BlockType& type  = registry.get(block);
    const std::uint16_t textureLayer =
        type.textureLayers[static_cast<std::size_t>(direction)];
    VOXL_ASSERT(textureLayer <= kMaxTextureLayer, "texture layer exceeds the packed field");

    // A positive face sits on the far side of its voxel, a negative face on the
    // near side; both are integer block planes, which is why 33 distinct
    // coordinates (0..32) have to fit in the packed position field.
    std::int32_t base[3]{};
    base[static_cast<std::size_t>(frame.nAxis)] = n + (frame.sign > 0 ? 1 : 0);
    base[static_cast<std::size_t>(frame.uAxis)] = u;
    base[static_cast<std::size_t>(frame.vAxis)] = v;

    std::array<PackedVertex, 4> corners{};
    for (int corner = 0; corner < 4; ++corner) {
        std::int32_t position[3] = {base[0], base[1], base[2]};
        if (corner == 1 || corner == 2) {
            position[static_cast<std::size_t>(frame.uAxis)] += w;
        }
        if (corner == 2 || corner == 3) {
            position[static_cast<std::size_t>(frame.vAxis)] += h;
        }

        const std::uint32_t packed = cornerOf(key, corner);

        VertexAttributes attributes;
        attributes.x            = static_cast<std::uint8_t>(position[0]);
        attributes.y            = static_cast<std::uint8_t>(position[1]);
        attributes.z            = static_cast<std::uint8_t>(position[2]);
        attributes.direction    = direction;
        attributes.ao           = static_cast<std::uint8_t>(packed & kVtxAoMask);
        attributes.sunlight     = static_cast<std::uint8_t>((packed >> 2) & kVtxLightMask);
        attributes.blockLight   = static_cast<std::uint8_t>((packed >> 6) & kVtxLightMask);
        attributes.textureLayer = textureLayer;
        attributes.width        = static_cast<std::uint8_t>(w);
        attributes.height       = static_cast<std::uint8_t>(h);
        attributes.corner       = static_cast<QuadCorner>(corner);

        corners[static_cast<std::size_t>(corner)] = packVertex(attributes);
    }

    // addQuad owns the triangle-split decision, including flipping the diagonal
    // when the corner AO is anisotropic (the seam artefact).
    out.layer(type.renderLayer).addQuad(corners[0], corners[1], corners[2], corners[3]);
    ++m_stats.quadsEmitted;

    MesherScratch& scratch = *m_scratch;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const std::int32_t low  = base[axis];
        std::int32_t       high = base[axis];
        if (axis == static_cast<std::size_t>(frame.uAxis)) {
            high += w;
        }
        if (axis == static_cast<std::size_t>(frame.vAxis)) {
            high += h;
        }
        scratch.boundsMin[axis] = low < scratch.boundsMin[axis] ? low : scratch.boundsMin[axis];
        scratch.boundsMax[axis] = high > scratch.boundsMax[axis] ? high : scratch.boundsMax[axis];
    }
}

// ------------------------------------------------------------------ entry --

bool GreedyMesher::mesh(const ChunkNeighbourhood& neighbourhood, ChunkMeshData& out)
{
    out.clear();
    out.position = neighbourhood.centrePos();
    m_stats      = Stats{};

    const Chunk* centre = neighbourhood.centre();
    if (centre == nullptr) {
        return false;
    }

    // Sampled BEFORE any voxel is read. If an edit lands mid-mesh the recorded
    // version is behind the chunk's, so the main thread drops this result and
    // reschedules instead of publishing geometry that is half old and half new.
    out.contentVersion = centre->contentVersion();

    // An all-air section can never emit a face: faces belong to the solid block,
    // not to its neighbour, so nothing outside the chunk can change this.
    if (centre->storage().isEmpty()) {
        return false;
    }

    MesherScratch& scratch = *m_scratch;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        scratch.boundsMin[axis] = kChunkSize;
        scratch.boundsMax[axis] = 0;
    }

    for (std::size_t layer = 0; layer < kRenderLayerCount; ++layer) {
        if (m_quadHint[layer] != 0) {
            out.layers[layer].reserveQuads(m_quadHint[layer]);
        }
    }

    loadCache(neighbourhood);

    for (std::size_t d = 0; d < kDirectionCount; ++d) {
        sweep(static_cast<Direction>(d), out);
    }

    for (std::size_t layer = 0; layer < kRenderLayerCount; ++layer) {
        m_quadHint[layer] = out.layers[layer].vertexCount() / 4;
    }

    if (m_stats.quadsEmitted == 0) {
        return false;
    }

    out.boundsMin = glm::vec3{static_cast<float>(scratch.boundsMin[0]),
                              static_cast<float>(scratch.boundsMin[1]),
                              static_cast<float>(scratch.boundsMin[2])};
    out.boundsMax = glm::vec3{static_cast<float>(scratch.boundsMax[0]),
                              static_cast<float>(scratch.boundsMax[1]),
                              static_cast<float>(scratch.boundsMax[2])};
    return true;
}

ChunkMeshData GreedyMesher::mesh(const ChunkNeighbourhood& neighbourhood)
{
    ChunkMeshData out;
    mesh(neighbourhood, out);
    return out;
}

}  // namespace voxl
