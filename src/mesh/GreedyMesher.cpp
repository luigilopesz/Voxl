#include "mesh/GreedyMesher.hpp"

#include "core/Log.hpp"
#include "world/Chunk.hpp"
#include "world/ChunkStorage.hpp"
#include "world/Lod.hpp"
#include "world/SubVoxel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace voxl::detail {

/// Flat, padded copies of everything the sweep reads.
///
/// The padding is one CELL on every side: the sweep needs the neighbour cell
/// across each face (culling) and the eight cells ringing it in the face plane
/// (ambient occlusion), all of which fall outside the centre chunk for the
/// boundary slices. Copying the skirt in once means the inner loop never has to
/// ask "is this coordinate still inside the chunk?".
///
/// The arrays are sized for the level-0 worst case (34^3) and re-addressed with
/// a smaller stride at coarser levels, so switching a mesher between levels
/// never reallocates.
struct MesherScratch {
    static constexpr std::int32_t  kMaxDim = kChunkSize + 2;
    static constexpr std::size_t   kVolume = static_cast<std::size_t>(kMaxDim) * kMaxDim * kMaxDim;

    // ---- geometry of the level currently configured ----

    LodLevel     level    = kLodFull;
    std::int32_t grid     = kChunkSize;  ///< cells along one chunk axis, 32 >> level
    std::int32_t cellSize = 1;           ///< blocks along one cell axis, 1 << level
    std::int32_t dim      = kMaxDim;     ///< grid + 2, the padded extent
    std::int32_t minSolid = 1;           ///< blocks a cell needs before it counts as solid

    // Strides mirror localIndex()'s ordering (x fastest, then z, then y) so the
    // interior copy walks both the source and the destination linearly. They are
    // runtime values because `dim` depends on the level; at level 0 they hold
    // exactly the constants the pre-LOD mesher used, and the sweep hoists them
    // into registers before its loops, so the indirection costs nothing.
    std::ptrdiff_t strideX = 1;
    std::ptrdiff_t strideZ = kMaxDim;
    std::ptrdiff_t strideY = static_cast<std::ptrdiff_t>(kMaxDim) * kMaxDim;

    void configure(LodLevel newLevel) noexcept
    {
        level    = newLevel;
        grid     = lodGridSize(newLevel);
        cellSize = lodCellSize(newLevel);
        dim      = grid + 2;
        minSolid = lodCellSolidBlocks(newLevel);
        strideX  = 1;
        strideZ  = dim;
        strideY  = static_cast<std::ptrdiff_t>(dim) * dim;
    }

    /// `x`, `y`, `z` are centre-chunk-local CELL coordinates and may range over
    /// [-1, grid].
    [[nodiscard]] std::size_t index(std::int32_t x, std::int32_t y, std::int32_t z) const noexcept
    {
        return static_cast<std::size_t>((x + 1) * strideX + (z + 1) * strideZ +
                                        (y + 1) * strideY);
    }

    /// The same addressing specialised for level 0, where `dim` is always
    /// kMaxDim. The level-0 loader is the measured hot path and reloads the
    /// runtime strides on every store otherwise - they live in the same object
    /// as the arrays being written, so the compiler cannot prove they do not
    /// alias.
    [[nodiscard]] static constexpr std::size_t fullIndex(std::int32_t x, std::int32_t y,
                                                         std::int32_t z) noexcept
    {
        constexpr std::ptrdiff_t kFullStrideZ = kMaxDim;
        constexpr std::ptrdiff_t kFullStrideY = static_cast<std::ptrdiff_t>(kMaxDim) * kMaxDim;
        return static_cast<std::size_t>((x + 1) + (z + 1) * kFullStrideZ + (y + 1) * kFullStrideY);
    }

    [[nodiscard]] std::ptrdiff_t axisStride(std::int32_t axis) const noexcept
    {
        return axis == 0 ? strideX : (axis == 1 ? strideY : strideZ);
    }

    std::array<BlockId, kVolume>      block{};
    std::array<std::uint8_t, kVolume> light{};
    /// Per-cell face-culling and occlusion flags, resolved from the registry
    /// during the copy. Ambient occlusion samples nine cells per visible face
    /// and culling samples one per candidate face; going back to the registry
    /// each time means chasing a ~120-byte BlockType instead of reading a byte
    /// that is already in L1 next to the block id.
    std::array<std::uint8_t, kVolume> flags{};

    /// One grid x grid slice of merge keys, always addressed with a row stride
    /// of kChunkSize whatever the level: a coarse slice simply uses the
    /// top-left sub-rectangle. Keeping the stride a compile-time power of two
    /// turns the mask address into a shift, which measurably beats a runtime
    /// multiply in the merge loop. Rebuilt (fully overwritten) per slice, so it
    /// never needs clearing between slices.
    static constexpr std::size_t kMaskStride = kChunkSize;
    std::array<std::uint64_t, kMaskStride * kChunkSize> mask{};

    /// Per-face: the neighbour across it draws at a different resolution, or is
    /// not resident. Written by loadCache, read by emitSkirts - the two need the
    /// same answer and computing it twice invites them to disagree.
    bool levelDiffers[kDirectionCount]{};

    /// Per-block-id counters for the LOD majority vote, sized to the registry.
    /// Kept here rather than as a local so the downsample loop allocates
    /// nothing; `touched` records which entries are non-zero so clearing costs
    /// O(distinct materials in the cell) instead of O(registry).
    std::vector<std::uint32_t> histogram;
    std::vector<BlockId>       touched;

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
/// against an identical neighbour. Both are cached per cell in
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

/// A key whose four corners are identical - used by the skirt, which has no
/// occlusion of its own and inherits one light value from the surface it hangs
/// beneath.
[[nodiscard]] constexpr std::uint64_t uniformKey(BlockId block, std::uint32_t ao,
                                                 std::uint32_t sunlight,
                                                 std::uint32_t blockLight) noexcept
{
    std::uint64_t key = static_cast<std::uint64_t>(block) & kKeyBlockMask;
    for (int corner = 0; corner < 4; ++corner) {
        key |= encodeCorner(ao, sunlight, blockLight)
               << (kKeyCornerShift + kKeyCornerBits * corner);
    }
    return key;
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

/// The four chunk sides a skirt hangs from. Only the horizontal ones: LodPolicy
/// selects a level from HORIZONTAL distance, so every section in a column shares
/// a level and the top/bottom seams can never disagree.
constexpr std::array<Direction, 4> kSkirtSides = {Direction::NegX, Direction::PosX,
                                                  Direction::NegZ, Direction::PosZ};

// -------------------------------------------------- ambient occlusion ring --
//
// For a visible face, `q` is the (necessarily non-opaque) cell the face points
// into. The nine cells of the face-plane 3x3 around `q` are gathered once and
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

/// Sub-voxel lookup for a block outside the centre chunk. Returns true (whole)
/// for anything the neighbourhood cannot resolve, which is the conservative
/// answer everywhere it matters: an unresolvable block is already air.
[[nodiscard]] bool neighbourhoodBlockWhole(const ChunkNeighbourhood& neighbourhood, std::int32_t x,
                                           std::int32_t y, std::int32_t z) noexcept
{
    const Chunk* chunk = neighbourhood.chunkAt(x >> kChunkSizeLog2, y >> kChunkSizeLog2,
                                               z >> kChunkSizeLog2);
    if (chunk == nullptr) {
        return true;
    }
    return chunk->isBlockWhole(localIndex(x & kChunkSizeMask, y & kChunkSizeMask,
                                          z & kChunkSizeMask));
}

}  // namespace

std::uint16_t waterSurfaceTextureLayer(const BlockRegistry& registry) noexcept
{
    return registry.get(blocks::Water).textureLayers[static_cast<std::size_t>(Direction::PosY)];
}

std::int32_t lodCellSolidBlocks(LodLevel level) noexcept
{
    const std::int32_t side  = lodCellSize(level);
    const std::int32_t total = side * side * side;
    // ceil, not round: "at least kLodSolidThreshold of its blocks" reads as a
    // lower bound, and rounding down would make a 3-of-8 cell solid at level 1,
    // which is 37.5%.
    const std::int32_t required =
        static_cast<std::int32_t>(std::ceil(static_cast<double>(kLodSolidThreshold) *
                                            static_cast<double>(total)));
    return required < 1 ? 1 : required;
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
    m_scratch->histogram.assign(registry.size(), 0u);
    m_scratch->touched.reserve(16);
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
    MesherScratch& scratch = *m_scratch;

    // CROSS-LEVEL BORDERS ARE NEVER CULLED.
    //
    // A face may only be hidden by geometry that is actually drawn where the
    // face is. When the neighbour renders at a different resolution its drawn
    // surface is NOT the voxels we can read from it: a coarser neighbour rounds
    // its surface to 2^L steps and may drop a thin feature entirely, and a finer
    // neighbour draws detail our own cell grid cannot see. Culling against the
    // raw voxels in either direction produces a see-through hole along the whole
    // LOD boundary, and because a mesh is only rebuilt when something dirties
    // the chunk, that hole is permanent.
    //
    // So the entire ring slab on such a face is loaded as air. The cost is one
    // extra greedy-merged quad per face in the common case; the skirt below
    // covers whatever height disagreement remains. This rule applies at level 0
    // too - a full-resolution chunk next to a coarse one is exactly the case the
    // hole appears in.
    bool conservativeFace[kDirectionCount]{};
    for (std::size_t d = 0; d < kDirectionCount; ++d) {
        const glm::ivec3& offset = kDirectionOffsets[d];
        const Chunk*      side   = neighbourhood.chunkAt(offset.x, offset.y, offset.z);
        conservativeFace[d]      = side != nullptr && side->lod() != scratch.level;
        // A missing neighbour is a level difference as far as the skirt is
        // concerned - there is nothing there to agree with, and it may well
        // arrive at another level - but NOT as far as culling is concerned,
        // where "no chunk" already reads as air through the neighbourhood.
        scratch.levelDiffers[d] = side == nullptr || side->lod() != scratch.level;
    }

    if (scratch.level == kLodFull) {
        loadCacheFull(neighbourhood, conservativeFace);
    } else {
        loadCacheLod(neighbourhood, conservativeFace);
    }
}

void GreedyMesher::loadCacheFull(const ChunkNeighbourhood& neighbourhood,
                                 const bool (&conservativeFace)[kDirectionCount])
{
    MesherScratch&      scratch = *m_scratch;
    const Chunk*        centre  = neighbourhood.centre();
    const ChunkStorage& storage = centre->storage();
    VOXL_ASSERT(scratch.dim == MesherScratch::kMaxDim, "the full-resolution loader assumes dim 34");

    // Rows of 32 are contiguous in both layouts (localIndex and strideX both
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
            const std::size_t destination = MesherScratch::fullIndex(0, y, z);

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

    // A PARTIALLY DESTROYED BLOCK IS NOT A CUBE.
    //
    // Its geometry belongs to the sub-voxel mesher, and its neighbours must keep
    // the faces they would otherwise cull against it. Rewriting it to air in the
    // cache achieves both at once and costs nothing anywhere else: this loop
    // runs over the sparse store, which is empty for every chunk the player has
    // not damaged, so untouched terrain never pays for the feature. The block id
    // in ChunkStorage is untouched - this is a meshing-only view.
    if (centre->hasSubVoxelDamage()) {
        const std::uint8_t airFlags = blockFlags(blocks::Air);
        centre->subVoxels().forEach([&](std::uint16_t blockIndex, const SubVoxelGrid&) {
            const std::int32_t x = static_cast<std::int32_t>(blockIndex) & kChunkSizeMask;
            const std::int32_t z = (static_cast<std::int32_t>(blockIndex) >> kChunkSizeLog2) &
                                   kChunkSizeMask;
            const std::int32_t y = static_cast<std::int32_t>(blockIndex) >> (2 * kChunkSizeLog2);
            const std::size_t  at = MesherScratch::fullIndex(x, y, z);
            scratch.block[at] = blocks::Air;
            scratch.flags[at] = airFlags;
        });
    }

    // Only pay for the sub-voxel check on the ring when some neighbour actually
    // holds damage; the check costs a chunk resolve plus a hash lookup per ring
    // voxel and the ring is 6536 voxels.
    bool ringDamage = false;
    for (std::int32_t dy = -1; dy <= 1 && !ringDamage; ++dy) {
        for (std::int32_t dz = -1; dz <= 1 && !ringDamage; ++dz) {
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                const Chunk* side = neighbourhood.chunkAt(dx, dy, dz);
                if (side != nullptr && side != centre && side->hasSubVoxelDamage()) {
                    ringDamage = true;
                    break;
                }
            }
        }
    }

    // Skirt: the six padded faces, through the neighbourhood so the lookup
    // genuinely crosses the chunk boundary. Treating out-of-chunk as air here is
    // the classic bug - it leaves a full sheet of interior faces at every seam.
    // Edges and corners are written more than once; that is 400 redundant stores
    // in exchange for three tight loops instead of twenty-six special cases. An
    // edge shared by one conservative and one ordinary face therefore takes
    // whichever slab wrote last, which only perturbs ambient occlusion at the
    // chunk's twelve edges and never culling.
    const auto store = [&](std::int32_t x, std::int32_t y, std::int32_t z, std::size_t face) {
        const std::size_t dst = MesherScratch::fullIndex(x, y, z);
        if (conservativeFace[face]) {
            scratch.block[dst] = kMissingChunkBlock;
            scratch.light[dst] = kMissingChunkLight;
            scratch.flags[dst] = blockFlags(kMissingChunkBlock);
            return;
        }
        BlockId id = neighbourhood.getBlockLocal(x, y, z);
        if (ringDamage && id != blocks::Air && !neighbourhoodBlockWhole(neighbourhood, x, y, z)) {
            id = blocks::Air;
        }
        scratch.block[dst] = id;
        scratch.light[dst] = neighbourhood.getLightLocal(x, y, z);
        scratch.flags[dst] = blockFlags(id);
    };

    for (std::int32_t a = -1; a <= kChunkSize; ++a) {
        for (std::int32_t b = -1; b <= kChunkSize; ++b) {
            store(-1, a, b, static_cast<std::size_t>(Direction::NegX));
            store(kChunkSize, a, b, static_cast<std::size_t>(Direction::PosX));
            store(a, -1, b, static_cast<std::size_t>(Direction::NegY));
            store(a, kChunkSize, b, static_cast<std::size_t>(Direction::PosY));
            store(a, b, -1, static_cast<std::size_t>(Direction::NegZ));
            store(a, b, kChunkSize, static_cast<std::size_t>(Direction::PosZ));
        }
    }
}

void GreedyMesher::reduceCell(const ChunkNeighbourhood& neighbourhood, std::int32_t blockX,
                              std::int32_t blockY, std::int32_t blockZ, std::size_t destination)
{
    MesherScratch&              scratch   = *m_scratch;
    const std::int32_t          cellSize  = scratch.cellSize;
    std::vector<std::uint32_t>& histogram = scratch.histogram;
    std::vector<BlockId>&       touched   = scratch.touched;
    touched.clear();

    std::int32_t  solid      = 0;
    std::uint32_t maxSun     = 0;
    std::uint32_t maxBlockLight = 0;

    for (std::int32_t dy = 0; dy < cellSize; ++dy) {
        for (std::int32_t dz = 0; dz < cellSize; ++dz) {
            for (std::int32_t dx = 0; dx < cellSize; ++dx) {
                const std::int32_t x = blockX + dx;
                const std::int32_t y = blockY + dy;
                const std::int32_t z = blockZ + dz;

                const std::uint8_t  packed     = neighbourhood.getLightLocal(x, y, z);
                const std::uint32_t sun        = ChunkStorage::unpackSunlight(packed);
                const std::uint32_t blockLevel = ChunkStorage::unpackBlockLight(packed);
                maxSun        = sun > maxSun ? sun : maxSun;
                maxBlockLight = blockLevel > maxBlockLight ? blockLevel : maxBlockLight;

                // Sub-voxel damage is deliberately ignored above level 0: an
                // eighth of a block is invisible at LOD distance, and honouring
                // it would force the coarse path to consult the sparse store for
                // every one of the 32768 blocks it reduces.
                const BlockId id = neighbourhood.getBlockLocal(x, y, z);
                if (id == blocks::Air || id >= histogram.size()) {
                    continue;  // an id past the registry reads as air, as everywhere else
                }
                ++solid;
                if (histogram[id]++ == 0u) {
                    touched.push_back(id);
                }
            }
        }
    }

    // Occupancy is measured against NON-AIR, not against CollisionShape::Cube.
    // The mesher's job here is visual coverage: water is not "solid" to physics
    // but a distant ocean that thresholds away leaves a hole down to the sea
    // floor, which is precisely the defect the bias toward solid exists to stop.
    BlockId material = blocks::Air;
    if (solid >= scratch.minSolid) {
        std::uint32_t best = 0;
        for (const BlockId id : touched) {
            const std::uint32_t count = histogram[id];
            // Ties break toward the lower id so the result cannot depend on the
            // order the cell happened to be walked in - two chunks meshed on two
            // threads must agree along their shared seam.
            if (count > best || (count == best && id < material)) {
                best     = count;
                material = id;
            }
        }
    }
    for (const BlockId id : touched) {
        histogram[id] = 0u;
    }

    scratch.block[destination] = material;
    scratch.flags[destination] = blockFlags(material);
    // Brightest wins rather than an average: averaging the dark interior of a
    // cell into its surface draws a band of shadow along every coarse chunk.
    scratch.light[destination] = ChunkStorage::packLight(static_cast<std::uint8_t>(maxSun),
                                                         static_cast<std::uint8_t>(maxBlockLight));
}

void GreedyMesher::loadCacheLod(const ChunkNeighbourhood& neighbourhood,
                                const bool (&conservativeFace)[kDirectionCount])
{
    MesherScratch&      scratch  = *m_scratch;
    const std::int32_t  grid     = scratch.grid;
    const std::int32_t  cellSize = scratch.cellSize;
    const ChunkStorage& storage  = neighbourhood.centre()->storage();

    // A uniform section reduces to one value per cell with no sampling at all,
    // and uniform sections are the overwhelming majority of what is far enough
    // away to be coarse: solid stone below the caves, and sky.
    const bool uniformInterior = storage.isUniform() && !storage.hasLightData();
    const BlockId      uniformMaterial = uniformInterior ? storage.uniformValue() : blocks::Air;
    const std::uint8_t uniformFlags    = blockFlags(uniformMaterial);
    const std::uint8_t uniformLight    = storage.uniformLight();

    for (std::int32_t cy = -1; cy <= grid; ++cy) {
        for (std::int32_t cz = -1; cz <= grid; ++cz) {
            for (std::int32_t cx = -1; cx <= grid; ++cx) {
                const std::size_t destination = scratch.index(cx, cy, cz);
                const bool interior = cx >= 0 && cx < grid && cy >= 0 && cy < grid && cz >= 0 &&
                                      cz < grid;

                if (!interior) {
                    // A ring cell on an edge belongs to two faces; it is treated
                    // conservatively when either of them is, which is the safe
                    // direction (it only ever adds geometry).
                    bool conservative = false;
                    if (cx < 0) {
                        conservative = conservative ||
                                       conservativeFace[static_cast<std::size_t>(Direction::NegX)];
                    }
                    if (cx >= grid) {
                        conservative = conservative ||
                                       conservativeFace[static_cast<std::size_t>(Direction::PosX)];
                    }
                    if (cy < 0) {
                        conservative = conservative ||
                                       conservativeFace[static_cast<std::size_t>(Direction::NegY)];
                    }
                    if (cy >= grid) {
                        conservative = conservative ||
                                       conservativeFace[static_cast<std::size_t>(Direction::PosY)];
                    }
                    if (cz < 0) {
                        conservative = conservative ||
                                       conservativeFace[static_cast<std::size_t>(Direction::NegZ)];
                    }
                    if (cz >= grid) {
                        conservative = conservative ||
                                       conservativeFace[static_cast<std::size_t>(Direction::PosZ)];
                    }
                    if (conservative) {
                        scratch.block[destination] = kMissingChunkBlock;
                        scratch.light[destination] = kMissingChunkLight;
                        scratch.flags[destination] = blockFlags(kMissingChunkBlock);
                        continue;
                    }
                } else if (uniformInterior) {
                    scratch.block[destination] = uniformMaterial;
                    scratch.light[destination] = uniformLight;
                    scratch.flags[destination] = uniformFlags;
                    continue;
                }

                reduceCell(neighbourhood, cx * cellSize, cy * cellSize, cz * cellSize, destination);
            }
        }
    }
}

// ----------------------------------------------------------------- sweep --

void GreedyMesher::sweep(Direction direction, ChunkMeshData& out)
{
    MesherScratch&     scratch = *m_scratch;
    const FaceFrame&   frame   = kFaceFrames[static_cast<std::size_t>(direction)];
    const std::int32_t grid    = scratch.grid;

    const std::ptrdiff_t strideN = scratch.axisStride(frame.nAxis);
    const std::ptrdiff_t strideU = scratch.axisStride(frame.uAxis);
    const std::ptrdiff_t strideV = scratch.axisStride(frame.vAxis);
    const std::ptrdiff_t toNeighbour = frame.sign * strideN;

    // The (u, v) frame is fixed by the winding rule, but the order the mask is
    // FILLED in is not. Three of the six directions have a u axis whose stride
    // through the cell cache is 1156 entries; walking that innermost touches a
    // fresh cache line every step. Iterate whichever tangent axis is denser in
    // memory innermost instead - the mask itself is 8 KB and stays in L1 however
    // it is addressed, so the strided access moves to where it is free.
    const bool           uIsInner    = strideU <= strideV;
    const std::ptrdiff_t innerStride = uIsInner ? strideU : strideV;
    const std::ptrdiff_t outerStride = uIsInner ? strideV : strideU;
    const std::size_t    innerCellStep = uIsInner ? 1u : MesherScratch::kMaskStride;
    const std::size_t    outerCellStep = uIsInner ? MesherScratch::kMaskStride : 1u;

    const std::ptrdiff_t origin = static_cast<std::ptrdiff_t>(scratch.index(0, 0, 0));

    for (std::int32_t n = 0; n < grid; ++n) {
        const std::ptrdiff_t sliceBase = origin + static_cast<std::ptrdiff_t>(n) * strideN;

        std::size_t faceCount = 0;
        for (std::int32_t outer = 0; outer < grid; ++outer) {
            std::ptrdiff_t p    = sliceBase + static_cast<std::ptrdiff_t>(outer) * outerStride;
            std::size_t    cell = static_cast<std::size_t>(outer) * outerCellStep;

            for (std::int32_t inner = 0; inner < grid;
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

                    // Smooth light: average the four cells touching this corner
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
        for (std::int32_t v = 0; v < grid; ++v) {
            for (std::int32_t u = 0; u < grid;) {
                const std::size_t cell = static_cast<std::size_t>(v) *
                                             MesherScratch::kMaskStride +
                                         static_cast<std::size_t>(u);
                const std::uint64_t key = scratch.mask[cell];
                if (key == 0) {
                    ++u;
                    continue;
                }

                std::int32_t width = 1;
                while (u + width < grid &&
                       scratch.mask[cell + static_cast<std::size_t>(width)] == key) {
                    ++width;
                }

                std::int32_t height = 1;
                while (v + height < grid) {
                    const std::size_t rowStart =
                        static_cast<std::size_t>(v + height) * MesherScratch::kMaskStride +
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
                    const std::size_t rowStart = static_cast<std::size_t>(v + dv) *
                                                     MesherScratch::kMaskStride +
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

// ------------------------------------------------------------------ skirt --

void GreedyMesher::emitSkirts(ChunkMeshData& out)
{
    const MesherScratch& scratch  = *m_scratch;
    const std::int32_t   grid     = scratch.grid;
    const std::int32_t   cellSize = scratch.cellSize;
    const std::int32_t   depth    = lodSkirtDepth(scratch.level);
    if (depth <= 0) {
        return;
    }

    // WHY A CURTAIN AND NOT A STITCHED SEAM
    //
    // A coarse chunk's surface steps in 2^L blocks while a finer neighbour steps
    // in 1, so the two silhouettes disagree by up to 2^L - 1 blocks at the seam.
    // Even with both sides drawing their border walls (see loadCache), the two
    // walls are split at different heights and meet in a row of T-junctions,
    // where the rasteriser can leak a hairline of background between them.
    // Hanging a band of geometry down from the coarse silhouette closes it, and
    // unlike a stitched transition band it cannot itself crack and does not
    // defeat greedy merging.
    //
    // The curtain is deliberately coplanar with, and usually behind, the border
    // wall the sweep already emitted. That is why it takes the material and the
    // light of the cell it hangs from: coincident geometry with matching shading
    // is invisible, whereas a curtain shaded as an unlit interior face reads as
    // a dark band around every coarse chunk.
    //
    // ONLY WHERE THE LEVELS ACTUALLY DISAGREE. Two neighbours at the same level
    // quantise their surfaces on the same global cell grid, so their border
    // heights are equal by construction and there is no seam to hide. Hanging a
    // curtain there anyway was a visible bug rather than a wasted quad: over an
    // ocean the curtain is WATER, "coincident geometry with matching shading is
    // invisible" stops being true the moment the material is translucent, and
    // the doubled blend drew a dark lattice along every chunk border in the sea.
    // See docs/VISUAL_REVIEW.md.
    for (const Direction side : kSkirtSides) {
        if (!scratch.levelDiffers[static_cast<std::size_t>(side)]) {
            continue;
        }
        const FaceFrame&   frame  = kFaceFrames[static_cast<std::size_t>(side)];
        const bool         alongX = frame.nAxis == 0;
        const std::int32_t n      = frame.sign > 0 ? grid - 1 : 0;
        const glm::ivec3&  offset = kDirectionOffsets[static_cast<std::size_t>(side)];

        // Topmost OPAQUE cell of one border column, plus the shading the wall
        // face of that cell would have had.
        //
        // Opaque, not merely non-air. The curtain is justified entirely by
        // "coincident geometry with matching shading is invisible", and that
        // stops being true the moment the material is not opaque: a water
        // curtain blends a second time over the surface it duplicates and draws
        // a dark band, and a cutout curtain punches its own alpha holes through
        // the seam it exists to close. The disagreement that needs hiding is in
        // the opaque silhouette anyway - the transparent layers above it are
        // flat (water sits at exactly sea level) and their border wall is
        // already drawn on both sides by the cross-level rule in loadCache.
        const auto column = [&](std::int32_t t, std::int32_t& topCell, BlockId& material,
                                std::uint8_t& lightPacked) {
            const std::int32_t cx = alongX ? n : t;
            const std::int32_t cz = alongX ? t : n;
            for (std::int32_t cy = grid - 1; cy >= 0; --cy) {
                const std::size_t at = scratch.index(cx, cy, cz);
                if ((scratch.flags[at] & kFlagOpaque) == 0) {
                    continue;
                }
                topCell     = cy;
                material    = scratch.block[at];
                lightPacked = scratch.light[scratch.index(cx + offset.x, cy, cz + offset.z)];
                return true;
            }
            return false;
        };

        std::int32_t t = 0;
        while (t < grid) {
            std::int32_t topCell     = 0;
            BlockId      material    = blocks::Air;
            std::uint8_t lightPacked = 0;
            if (!column(t, topCell, material, lightPacked)) {
                ++t;
                continue;
            }

            // Merge the run of neighbouring columns that would produce an
            // identical band; a coarse chunk's border is usually flat over
            // several cells and this keeps the curtain to a handful of quads.
            std::int32_t run = 1;
            while (t + run < grid) {
                std::int32_t nextTop      = 0;
                BlockId      nextMaterial = blocks::Air;
                std::uint8_t nextLight    = 0;
                if (!column(t + run, nextTop, nextMaterial, nextLight) || nextTop != topCell ||
                    nextMaterial != material || nextLight != lightPacked) {
                    break;
                }
                ++run;
            }

            const std::int32_t top    = (topCell + 1) * cellSize;
            const std::int32_t bottom = top - depth > 0 ? top - depth : 0;
            const std::int32_t height = top - bottom;
            if (height > 0) {
                std::int32_t base[3]{};
                base[static_cast<std::size_t>(frame.nAxis)] =
                    n * cellSize + (frame.sign > 0 ? cellSize : 0);

                // Two of the four sides have the vertical axis as their U axis;
                // the curtain's extents have to follow the frame, not the other
                // way round, or the winding flips and the band is backface
                // culled from outside.
                std::int32_t widthBlocks  = 0;
                std::int32_t heightBlocks = 0;
                if (frame.uAxis == 1) {
                    base[static_cast<std::size_t>(frame.uAxis)] = bottom;
                    base[static_cast<std::size_t>(frame.vAxis)] = t * cellSize;
                    widthBlocks  = height;
                    heightBlocks = run * cellSize;
                } else {
                    base[static_cast<std::size_t>(frame.uAxis)] = t * cellSize;
                    base[static_cast<std::size_t>(frame.vAxis)] = bottom;
                    widthBlocks  = run * cellSize;
                    heightBlocks = height;
                }

                emitQuadBlocks(out, side, base, widthBlocks, heightBlocks,
                               uniformKey(material, kMaxAoLevel,
                                          ChunkStorage::unpackSunlight(lightPacked),
                                          ChunkStorage::unpackBlockLight(lightPacked)));
                ++m_stats.skirtQuads;
            }
            t += run;
        }
    }
}

// ------------------------------------------------------------------ emit --

void GreedyMesher::emitQuad(ChunkMeshData& out, Direction direction, std::int32_t n,
                            std::int32_t u, std::int32_t v, std::int32_t w, std::int32_t h,
                            std::uint64_t key)
{
    const FaceFrame&   frame    = kFaceFrames[static_cast<std::size_t>(direction)];
    const std::int32_t cellSize = m_scratch->cellSize;

    // A positive face sits on the far side of its cell, a negative face on the
    // near side; both land on a multiple of the cell size, which is why a coarse
    // quad still has integer block corners and the packed vertex format needs no
    // change. 33 distinct coordinates (0..32) have to fit in the position field.
    std::int32_t base[3]{};
    base[static_cast<std::size_t>(frame.nAxis)] = n * cellSize + (frame.sign > 0 ? cellSize : 0);
    base[static_cast<std::size_t>(frame.uAxis)] = u * cellSize;
    base[static_cast<std::size_t>(frame.vAxis)] = v * cellSize;

    emitQuadBlocks(out, direction, base, w * cellSize, h * cellSize, key);
}

void GreedyMesher::emitQuadBlocks(ChunkMeshData& out, Direction direction,
                                  const std::int32_t (&base)[3], std::int32_t widthBlocks,
                                  std::int32_t heightBlocks, std::uint64_t key)
{
    const BlockRegistry& registry = *m_registry;
    const FaceFrame&     frame    = kFaceFrames[static_cast<std::size_t>(direction)];

    const BlockId    block = static_cast<BlockId>(key & kKeyBlockMask);
    const BlockType& type  = registry.get(block);
    const std::uint16_t textureLayer =
        type.textureLayers[static_cast<std::size_t>(direction)];
    VOXL_ASSERT(textureLayer <= kMaxTextureLayer, "texture layer exceeds the packed field");

    // The silent-corruption guard. packVertex masks rather than clamps, so a
    // position of 33 or an extent of 33 would wrap to 1 and produce geometry
    // that is wrong without being obviously wrong. Every level must keep both
    // inside the field widths: a cell boundary is a multiple of 2^L and a merged
    // run is at most (32 >> L) cells, so the products below are bounded by 32.
    VOXL_ASSERT(widthBlocks >= 1 && widthBlocks <= kChunkSize && heightBlocks >= 1 &&
                    heightBlocks <= kChunkSize,
                "quad extent does not fit the 5-bit packed extent field");

    std::array<PackedVertex, 4> corners{};
    for (int corner = 0; corner < 4; ++corner) {
        std::int32_t position[3] = {base[0], base[1], base[2]};
        if (corner == 1 || corner == 2) {
            position[static_cast<std::size_t>(frame.uAxis)] += widthBlocks;
        }
        if (corner == 2 || corner == 3) {
            position[static_cast<std::size_t>(frame.vAxis)] += heightBlocks;
        }
        VOXL_ASSERT(position[0] >= 0 && position[0] <= kChunkSize && position[1] >= 0 &&
                        position[1] <= kChunkSize && position[2] >= 0 &&
                        position[2] <= kChunkSize,
                    "quad corner does not fit the 6-bit packed position field");

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
        attributes.width        = static_cast<std::uint8_t>(widthBlocks);
        attributes.height       = static_cast<std::uint8_t>(heightBlocks);
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
            high += widthBlocks;
        }
        if (axis == static_cast<std::size_t>(frame.vAxis)) {
            high += heightBlocks;
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
    // not to its neighbour, so nothing outside the chunk can change this. It
    // also has no silhouette for a skirt to hang from.
    if (centre->storage().isEmpty()) {
        return false;
    }

    MesherScratch& scratch = *m_scratch;

    // A level from a newer build (or a policy change) must degrade rather than
    // index off the end of the tables.
    const LodLevel level =
        static_cast<LodLevel>(centre->lod() > kLodMax ? kLodMax : centre->lod());
    scratch.configure(level);
    m_stats.level = level;

    for (std::size_t axis = 0; axis < 3; ++axis) {
        scratch.boundsMin[axis] = kChunkSize;
        scratch.boundsMax[axis] = 0;
    }

    for (std::size_t layer = 0; layer < kRenderLayerCount; ++layer) {
        if (m_quadHint[level][layer] != 0) {
            out.layers[layer].reserveQuads(m_quadHint[level][layer]);
        }
    }

    loadCache(neighbourhood);

    for (std::size_t d = 0; d < kDirectionCount; ++d) {
        sweep(static_cast<Direction>(d), out);
    }

    // A chunk that emitted no face at all is fully buried, so its border is not
    // visible either and a curtain would be pure cost.
    if (m_stats.quadsEmitted != 0) {
        emitSkirts(out);
    }

    for (std::size_t layer = 0; layer < kRenderLayerCount; ++layer) {
        m_quadHint[level][layer] = out.layers[layer].vertexCount() / 4;
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
