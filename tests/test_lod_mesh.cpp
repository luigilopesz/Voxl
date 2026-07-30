#include <catch2/catch_test_macros.hpp>

#include "mesh/GreedyMesher.hpp"
#include "mesh/MeshData.hpp"
#include "world/Block.hpp"
#include "world/BlockAccess.hpp"
#include "world/Chunk.hpp"
#include "world/Lod.hpp"
#include "world/SubVoxel.hpp"
#include "world/VoxelTypes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <tuple>
#include <vector>

using namespace voxl;

namespace {

/// Section 4 of 8, for the same reason test_mesher.cpp picks it: below the world
/// the out-of-world rule returns opaque bedrock and above it returns air, either
/// of which would silently change the face counts for reasons that have nothing
/// to do with LOD.
constexpr ChunkPos kCentre{0, 4, 0};

const BlockRegistry& registry()
{
    static const BlockRegistry instance = createDefaultBlockRegistry();
    return instance;
}

ChunkPtr makeChunk(const ChunkPos& position, BlockId fill, LodLevel level = kLodFull)
{
    ChunkPtr chunk = Chunk::create(position);
    chunk->storage().fill(fill);
    chunk->fillLight(ChunkStorage::kMaxLightLevel, 0);
    chunk->setLod(level);
    chunk->forceState(ChunkState::Ready);
    return chunk;
}

/// Wraps `centre` in a neighbourhood. When `populate` is false the 26 outer
/// slots are left null, which BlockAccess.hpp defines to read as air.
ChunkNeighbourhood surroundWith(const ChunkPtr& centre, BlockId surround, bool populate,
                                LodLevel surroundLevel = kLodFull)
{
    ChunkNeighbourhood neighbourhood(centre->position());
    neighbourhood.setChunk(0, 0, 0, centre);
    if (!populate) {
        return neighbourhood;
    }

    const ChunkPos& base = centre->position();
    for (std::int32_t dy = -1; dy <= 1; ++dy) {
        for (std::int32_t dz = -1; dz <= 1; ++dz) {
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0 && dz == 0) {
                    continue;
                }
                neighbourhood.setChunk(dx, dy, dz,
                                       makeChunk(ChunkPos{base.x + dx, base.y + dy, base.z + dz},
                                                 surround, surroundLevel));
            }
        }
    }
    return neighbourhood;
}

struct Quad {
    std::array<VertexAttributes, 4> corners{};

    [[nodiscard]] Direction direction() const noexcept { return corners[0].direction; }
};

std::vector<Quad> quadsOf(const MeshLayerData& layer)
{
    REQUIRE(layer.vertexCount() % 4 == 0u);
    std::vector<Quad> quads;
    quads.reserve(layer.vertexCount() / 4);
    for (std::size_t q = 0; q < layer.vertexCount() / 4; ++q) {
        Quad quad;
        for (std::size_t c = 0; c < 4; ++c) {
            quad.corners[c] = unpackVertex(layer.vertices[q * 4 + c]);
        }
        quads.push_back(quad);
    }
    return quads;
}

std::vector<Quad> allQuads(const ChunkMeshData& mesh)
{
    std::vector<Quad> quads;
    for (const MeshLayerData& layer : mesh.layers) {
        for (const Quad& quad : quadsOf(layer)) {
            quads.push_back(quad);
        }
    }
    return quads;
}

[[nodiscard]] std::size_t quadCount(const ChunkMeshData& mesh)
{
    return mesh.vertexCount() / 4;
}

// --------------------------------------------------------- face-set oracle --

/// One unit block face: the owning block plus the direction it points in.
using UnitFace = std::tuple<int, int, int, int>;  // direction, x, y, z

[[nodiscard]] std::array<int, 3> cornerPos(const VertexAttributes& a)
{
    return {a.x, a.y, a.z};
}

/// Decomposes every emitted quad back into the unit block faces it covers.
///
/// The tangent axes are recovered from the corner coordinates rather than from a
/// copy of the mesher's frame table, so this really is an independent check: if
/// the mesher ever emits a quad with the wrong winding or the wrong extent, the
/// decomposition disagrees with the oracle below instead of agreeing with the
/// same bug twice.
std::multiset<UnitFace> coveredFaces(const ChunkMeshData& mesh)
{
    std::multiset<UnitFace> faces;
    for (const Quad& quad : allQuads(mesh)) {
        const auto c0 = cornerPos(quad.corners[0]);
        const auto c1 = cornerPos(quad.corners[1]);
        const auto c3 = cornerPos(quad.corners[3]);

        const int direction = static_cast<int>(quad.direction());
        const int nAxis     = direction / 2;
        const int sign      = (direction % 2) == 1 ? +1 : -1;

        int uAxis = -1;
        int vAxis = -1;
        int uSpan = 0;
        int vSpan = 0;
        for (int axis = 0; axis < 3; ++axis) {
            if (c1[static_cast<std::size_t>(axis)] != c0[static_cast<std::size_t>(axis)]) {
                uAxis = axis;
                uSpan = c1[static_cast<std::size_t>(axis)] - c0[static_cast<std::size_t>(axis)];
            }
            if (c3[static_cast<std::size_t>(axis)] != c0[static_cast<std::size_t>(axis)]) {
                vAxis = axis;
                vSpan = c3[static_cast<std::size_t>(axis)] - c0[static_cast<std::size_t>(axis)];
            }
        }
        REQUIRE(uAxis >= 0);
        REQUIRE(vAxis >= 0);
        REQUIRE(uAxis != vAxis);
        REQUIRE(uAxis != nAxis);
        REQUIRE(vAxis != nAxis);
        REQUIRE(uSpan > 0);
        REQUIRE(vSpan > 0);

        for (int du = 0; du < uSpan; ++du) {
            for (int dv = 0; dv < vSpan; ++dv) {
                std::array<int, 3> block = c0;
                block[static_cast<std::size_t>(uAxis)] += du;
                block[static_cast<std::size_t>(vAxis)] += dv;
                // A positive face lies on the far plane of its block.
                block[static_cast<std::size_t>(nAxis)] -= sign > 0 ? 1 : 0;
                faces.emplace(direction, block[0], block[1], block[2]);
            }
        }
    }
    return faces;
}

/// The set of visible faces at full resolution, computed straight from
/// BlockRegistry::facesHidden and the sub-voxel invariant.
std::multiset<UnitFace> expectedFacesAtLevel0(const ChunkNeighbourhood& neighbourhood)
{
    const auto wholeBlockAt = [&](std::int32_t x, std::int32_t y, std::int32_t z) {
        const BlockId id = neighbourhood.getBlockLocal(x, y, z);
        if (id == blocks::Air) {
            return id;
        }
        const Chunk* owner = neighbourhood.chunkAt(x >> kChunkSizeLog2, y >> kChunkSizeLog2,
                                                   z >> kChunkSizeLog2);
        if (owner != nullptr &&
            !owner->isBlockWhole(localIndex(x & kChunkSizeMask, y & kChunkSizeMask,
                                            z & kChunkSizeMask))) {
            return blocks::Air;  // partially destroyed: not a cube, culls nothing
        }
        return id;
    };

    std::multiset<UnitFace> faces;
    for (std::int32_t y = 0; y < kChunkSize; ++y) {
        for (std::int32_t z = 0; z < kChunkSize; ++z) {
            for (std::int32_t x = 0; x < kChunkSize; ++x) {
                const BlockId self = wholeBlockAt(x, y, z);
                if (self == blocks::Air) {
                    continue;
                }
                for (std::size_t d = 0; d < kDirectionCount; ++d) {
                    const glm::ivec3& offset = kDirectionOffsets[d];
                    const BlockId other = wholeBlockAt(x + offset.x, y + offset.y, z + offset.z);
                    if (!registry().facesHidden(self, other)) {
                        faces.emplace(static_cast<int>(d), x, y, z);
                    }
                }
            }
        }
    }
    return faces;
}

/// FNV-1a over every emitted vertex and index, in emission order.
///
/// Used to pin the level-0 output byte for byte. A count-based assertion would
/// miss a reordering or a one-bit change in a light nibble, which is exactly the
/// kind of regression the LOD rewrite could have introduced silently.
[[nodiscard]] std::uint64_t meshHash(const ChunkMeshData& mesh)
{
    std::uint64_t hash = 1469598103934665603ull;
    const auto    mix  = [&hash](std::uint32_t value) {
        for (int byte = 0; byte < 4; ++byte) {
            hash ^= (value >> (byte * 8)) & 0xFFu;
            hash *= 1099511628211ull;
        }
    };
    for (const MeshLayerData& layer : mesh.layers) {
        for (const PackedVertex& vertex : layer.vertices) {
            mix(vertex.data0);
            mix(vertex.data1);
        }
        for (const MeshIndex index : layer.indices) {
            mix(index);
        }
    }
    return hash;
}

/// A deterministic, cave-like fill: enough structure to exercise merging,
/// occupancy thresholding and border faces, with no RNG anywhere.
void fillPseudoTerrain(const ChunkPtr& chunk)
{
    for (std::int32_t y = 0; y < kChunkSize; ++y) {
        for (std::int32_t z = 0; z < kChunkSize; ++z) {
            for (std::int32_t x = 0; x < kChunkSize; ++x) {
                const std::int32_t h = 12 + ((x * 5 + z * 3) % 11);
                if (y > h) {
                    continue;
                }
                const std::int32_t noise = (x * 7 + y * 13 + z * 29) % 17;
                if (noise == 0) {
                    continue;  // a pocket of air, so the interior is not uniform
                }
                chunk->setBlock(x, y, z, y == h ? blocks::Grass : blocks::Stone);
            }
        }
    }
}

}  // namespace

// ---------------------------------------------------------------- threshold --

TEST_CASE("the LOD occupancy threshold rounds up and never reaches zero", "[mesh][lod]")
{
    // 40% of 1, 8, 64 and 512 blocks, rounded up. Pinned because the terrain
    // generator and the mesher must agree on where a coarse surface is; a
    // half-block disagreement is a visible seam.
    CHECK(lodCellSolidBlocks(0) == 1);
    CHECK(lodCellSolidBlocks(1) == 4);
    CHECK(lodCellSolidBlocks(2) == 26);
    CHECK(lodCellSolidBlocks(3) == 205);
}

TEST_CASE("cell occupancy thresholds exactly at the boundary", "[mesh][lod]")
{
    // Level 1: a cell is 2x2x2 = 8 blocks and needs 4 of them.
    constexpr std::int32_t kCellOriginX = 16;  // cell (8, 8, 8), well away from any border
    constexpr std::int32_t kCellOriginY = 16;
    constexpr std::int32_t kCellOriginZ = 16;

    const auto meshWith = [](std::int32_t solidBlocks, GreedyMesher& mesher, ChunkMeshData& mesh) {
        ChunkPtr centre = makeChunk(kCentre, blocks::Air, 1);
        std::int32_t placed = 0;
        for (std::int32_t dy = 0; dy < 2 && placed < solidBlocks; ++dy) {
            for (std::int32_t dz = 0; dz < 2 && placed < solidBlocks; ++dz) {
                for (std::int32_t dx = 0; dx < 2 && placed < solidBlocks; ++dx) {
                    centre->setBlock(kCellOriginX + dx, kCellOriginY + dy, kCellOriginZ + dz,
                                     blocks::Stone);
                    ++placed;
                }
            }
        }
        return mesher.mesh(surroundWith(centre, blocks::Air, false), mesh);
    };

    GreedyMesher  mesher(registry());
    ChunkMeshData mesh;

    SECTION("one block below the threshold the cell is erased")
    {
        CHECK_FALSE(meshWith(3, mesher, mesh));
        CHECK(mesh.empty());
    }

    SECTION("exactly at the threshold the whole cell becomes solid")
    {
        REQUIRE(meshWith(4, mesher, mesh));
        // One isolated cell: six faces, none merged with anything, and each is a
        // full 2x2 block quad even though only half the blocks were solid. That
        // over-filling is the deliberate bias documented in world/Lod.hpp.
        CHECK(mesher.lastStats().quadsEmitted == 6u);
        CHECK(mesher.lastStats().skirtQuads == 0u);
        for (const Quad& quad : allQuads(mesh)) {
            CHECK(quad.corners[0].width == 2);
            CHECK(quad.corners[0].height == 2);
        }
        CHECK(mesh.boundsMin == glm::vec3{16.0f, 16.0f, 16.0f});
        CHECK(mesh.boundsMax == glm::vec3{18.0f, 18.0f, 18.0f});
    }
}

TEST_CASE("the dominant material of a mixed cell wins the vote", "[mesh][lod]")
{
    // Level 1 cell holding 5 dirt and 3 stone: solid (>= 4) and dirt.
    GreedyMesher mesher(registry());
    ChunkPtr     centre = makeChunk(kCentre, blocks::Air, 1);

    int placed = 0;
    for (std::int32_t dy = 0; dy < 2; ++dy) {
        for (std::int32_t dz = 0; dz < 2; ++dz) {
            for (std::int32_t dx = 0; dx < 2; ++dx) {
                centre->setBlock(16 + dx, 16 + dy, 16 + dz,
                                 placed < 5 ? blocks::Dirt : blocks::Stone);
                ++placed;
            }
        }
    }

    ChunkMeshData mesh;
    REQUIRE(mesher.mesh(surroundWith(centre, blocks::Air, false), mesh));
    CHECK(mesher.lastStats().quadsEmitted == 6u);

    const std::uint16_t dirtTop =
        registry().get(blocks::Dirt).textureLayers[static_cast<std::size_t>(Direction::PosY)];
    const std::uint16_t stoneTop =
        registry().get(blocks::Stone).textureLayers[static_cast<std::size_t>(Direction::PosY)];
    REQUIRE(dirtTop != stoneTop);  // otherwise the assertion below proves nothing

    bool sawDirtTop = false;
    for (const Quad& quad : allQuads(mesh)) {
        if (quad.direction() == Direction::PosY) {
            sawDirtTop = quad.corners[0].textureLayer == dirtTop;
        }
    }
    CHECK(sawDirtTop);
}

// --------------------------------------------------------------- geometry --

TEST_CASE("a solid chunk at level 2 emits six merged quads plus its skirt", "[mesh][lod][skirt]")
{
    GreedyMesher   mesher(registry());
    const ChunkPtr centre = makeChunk(kCentre, blocks::Stone, 2);

    ChunkMeshData mesh;
    REQUIRE(mesher.mesh(surroundWith(centre, blocks::Air, false), mesh));

    CHECK(mesher.lastStats().level == 2);
    // 8x8 cells per side collapse to one quad, six times; the four horizontal
    // borders each hang one merged curtain because every border column has the
    // same height, material and light.
    CHECK(mesher.lastStats().skirtQuads == 4u);
    CHECK(mesher.lastStats().quadsEmitted == 10u);
    CHECK(quadCount(mesh) == 10u);
    // 8x8 cell faces per side, not 32x32 block faces: a level-2 chunk does one
    // sixteenth of the sweep work per slice.
    CHECK(mesher.lastStats().facesEmitted == 6u * 8u * 8u);

    int fullSideQuads = 0;
    int skirtQuads    = 0;
    for (const Quad& quad : allQuads(mesh)) {
        const VertexAttributes& origin = quad.corners[0];
        if (origin.width == kChunkSize && origin.height == kChunkSize) {
            ++fullSideQuads;
            continue;
        }
        // The curtain: 32 blocks along the border, lodSkirtDepth(2) == 4 deep,
        // hanging from the top of the chunk down to y == 28.
        ++skirtQuads;
        CHECK((origin.width == kChunkSize || origin.height == kChunkSize));
        CHECK((origin.width == lodSkirtDepth(2) || origin.height == lodSkirtDepth(2)));
        CHECK(quad.direction() != Direction::PosY);
        CHECK(quad.direction() != Direction::NegY);
        for (const VertexAttributes& corner : quad.corners) {
            CHECK(corner.y >= kChunkSize - lodSkirtDepth(2));
            CHECK(corner.y <= kChunkSize);
            // Lit like the surface it hangs from, never like an interior face.
            CHECK(corner.sunlight == ChunkStorage::kMaxLightLevel);
            CHECK(corner.ao == kMaxAoLevel);
        }
    }
    CHECK(fullSideQuads == 6);
    CHECK(skirtQuads == 4);
}

TEST_CASE("no skirt is emitted at level 0", "[mesh][lod][skirt]")
{
    GreedyMesher mesher(registry());
    ChunkPtr     centre = makeChunk(kCentre, blocks::Air, kLodFull);
    fillPseudoTerrain(centre);

    ChunkMeshData mesh;
    REQUIRE(mesher.mesh(surroundWith(centre, blocks::Air, false), mesh));
    CHECK(mesher.lastStats().level == kLodFull);
    CHECK(mesher.lastStats().skirtQuads == 0u);
    CHECK(lodSkirtDepth(kLodFull) == 0);
}

TEST_CASE("a buried coarse chunk stays free", "[mesh][lod][skirt]")
{
    // Fully enclosed by same-level solid neighbours: no face survives, so no
    // curtain either. A skirt on an invisible chunk would be pure cost, and the
    // whole point of LOD is that distant chunks are cheap.
    GreedyMesher   mesher(registry());
    const ChunkPtr centre = makeChunk(kCentre, blocks::Stone, 2);

    ChunkMeshData mesh;
    CHECK_FALSE(mesher.mesh(surroundWith(centre, blocks::Stone, true, 2), mesh));
    CHECK(mesher.lastStats().quadsEmitted == 0u);
    CHECK(mesher.lastStats().skirtQuads == 0u);
    CHECK(mesh.empty());
}

TEST_CASE("a coarse chunk hangs its curtain from the terrain silhouette", "[mesh][lod][skirt]")
{
    // A flat slab 16 blocks tall meshed at level 3 (cells of 8): the curtain
    // must sit on top of the slab, not at the top of the chunk.
    GreedyMesher mesher(registry());
    ChunkPtr     centre = makeChunk(kCentre, blocks::Air, 3);
    for (std::int32_t y = 0; y < 16; ++y) {
        for (std::int32_t z = 0; z < kChunkSize; ++z) {
            for (std::int32_t x = 0; x < kChunkSize; ++x) {
                centre->setBlock(x, y, z, blocks::Stone);
            }
        }
    }

    ChunkMeshData mesh;
    REQUIRE(mesher.mesh(surroundWith(centre, blocks::Air, false), mesh));
    CHECK(mesher.lastStats().skirtQuads == 4u);

    // lodSkirtDepth(3) == 8, so the curtain spans y in [8, 16].
    int curtains = 0;
    for (const Quad& quad : allQuads(mesh)) {
        if (quad.direction() == Direction::PosY || quad.direction() == Direction::NegY) {
            continue;
        }
        int minY = kChunkSize;
        int maxY = 0;
        for (const VertexAttributes& corner : quad.corners) {
            minY = corner.y < minY ? corner.y : minY;
            maxY = corner.y > maxY ? corner.y : maxY;
        }
        if (minY == 8 && maxY == 16) {
            ++curtains;
        }
    }
    CHECK(curtains == 4);
}

// -------------------------------------------------------- packed-field safety --

TEST_CASE("emitted positions and extents fit the packed fields at every level",
          "[mesh][lod][packing]")
{
    // The silent-corruption case: packVertex MASKS, so a position of 33 or an
    // extent of 33 wraps to 1 and produces geometry that is subtly wrong rather
    // than obviously broken. Checked here for every level because the LOD sweep
    // is the only thing that multiplies a coordinate before packing it.
    for (LodLevel level = 0; level < kLodCount; ++level) {
        GreedyMesher mesher(registry());
        ChunkPtr     centre = makeChunk(kCentre, blocks::Air, level);
        fillPseudoTerrain(centre);

        ChunkMeshData mesh;
        REQUIRE(mesher.mesh(surroundWith(centre, blocks::Air, true, level), mesh));
        REQUIRE(mesh.vertexCount() > 0u);

        const std::int32_t cellSize = lodCellSize(level);

        bool wellFormed = true;
        for (const MeshLayerData& layer : mesh.layers) {
            for (const PackedVertex& vertex : layer.vertices) {
                const VertexAttributes a = unpackVertex(vertex);
                wellFormed = wellFormed &&
                             // Reserved bits are part of the GLSL contract.
                             (vertex.data0 & 0x8000'0000u) == 0u &&
                             (vertex.data1 & 0xFF00'0000u) == 0u &&
                             a.x <= kChunkSize && a.y <= kChunkSize && a.z <= kChunkSize &&
                             a.width >= 1 && a.width <= kChunkSize && a.height >= 1 &&
                             a.height <= kChunkSize && a.ao <= kMaxAoLevel &&
                             a.textureLayer <= kMaxTextureLayer &&
                             // The reason no vertex bit had to change: every
                             // coordinate a level-L sweep can produce, curtain
                             // included, is a multiple of 2^L.
                             (a.x % cellSize) == 0 && (a.y % cellSize) == 0 &&
                             (a.z % cellSize) == 0 && (a.width % cellSize) == 0 &&
                             (a.height % cellSize) == 0 &&
                             packVertex(a) == vertex;
            }
        }
        CHECK(wellFormed);
    }
}

// --------------------------------------------------------- level-0 regression --

TEST_CASE("a level-0 chunk still meshes exactly the visible face set", "[mesh][lod][regression]")
{
    // The guard against the LOD rewrite disturbing the main path: the quads the
    // mesher emits must decompose into precisely the faces BlockRegistry says
    // are visible - no missing face (a hole), no duplicate (overdraw), no extra.
    GreedyMesher mesher(registry());
    ChunkPtr     centre = makeChunk(kCentre, blocks::Air, kLodFull);
    fillPseudoTerrain(centre);

    // GOLDEN VALUES CAPTURED FROM THE PRE-LOD MESHER (commit 014452e), by
    // replaying these exact two fixtures against it. They are the byte-for-byte
    // guarantee that adding LOD did not disturb the main path: not the quad
    // count, the actual vertex and index stream in emission order. If a
    // deliberate change to the level-0 output is ever made, re-capture them the
    // same way and say so in the commit message.
    SECTION("against air neighbours")
    {
        const ChunkNeighbourhood neighbourhood = surroundWith(centre, blocks::Air, false);
        ChunkMeshData            mesh;
        REQUIRE(mesher.mesh(neighbourhood, mesh));
        CHECK(coveredFaces(mesh) == expectedFacesAtLevel0(neighbourhood));
        CHECK(mesh.vertexCount() == 63192u);
        CHECK(mesh.triangleCount() == 31596u);
        CHECK(meshHash(mesh) == 0xf7c136e48b727c1full);
    }

    SECTION("against solid neighbours, where the seam faces are culled")
    {
        const ChunkNeighbourhood neighbourhood = surroundWith(centre, blocks::Stone, true);
        ChunkMeshData            mesh;
        REQUIRE(mesher.mesh(neighbourhood, mesh));
        CHECK(coveredFaces(mesh) == expectedFacesAtLevel0(neighbourhood));
        CHECK(mesh.vertexCount() == 61384u);
        CHECK(mesh.triangleCount() == 30692u);
        CHECK(meshHash(mesh) == 0x0b0bb2a6b676f297ull);
    }
}

TEST_CASE("meshing is deterministic and reuse does not perturb the result", "[mesh][lod]")
{
    // Scratch buffers and the quad-size hint are reused across calls; a stale
    // entry leaking into the next chunk would show up here first.
    GreedyMesher mesher(registry());
    ChunkPtr     centre = makeChunk(kCentre, blocks::Air, kLodFull);
    fillPseudoTerrain(centre);

    ChunkMeshData first;
    REQUIRE(mesher.mesh(surroundWith(centre, blocks::Air, false), first));

    // Mesh a coarse chunk in between, so the level-0 result cannot depend on
    // whatever the previous call configured.
    const ChunkPtr coarse = makeChunk(ChunkPos{9, 4, 9}, blocks::Stone, 3);
    ChunkMeshData  ignored;
    REQUIRE(mesher.mesh(surroundWith(coarse, blocks::Air, false), ignored));

    ChunkMeshData second;
    REQUIRE(mesher.mesh(surroundWith(centre, blocks::Air, false), second));

    bool identical = true;
    for (std::size_t layer = 0; layer < kRenderLayerCount; ++layer) {
        identical = identical && first.layers[layer].vertices == second.layers[layer].vertices &&
                    first.layers[layer].indices == second.layers[layer].indices;
    }
    CHECK(identical);
    CHECK(first.boundsMin == second.boundsMin);
    CHECK(first.boundsMax == second.boundsMax);
}

// ------------------------------------------------------ cross-level borders --

TEST_CASE("a border face is never culled against a neighbour at another level",
          "[mesh][lod][border]")
{
    // Wrongly culling here is the defect that produces a see-through hole along
    // every LOD boundary, and because a mesh is only rebuilt when something
    // dirties the chunk, the hole is permanent.
    const auto meshAgainst = [](LodLevel centreLevel, LodLevel neighbourLevel,
                                GreedyMesher& mesher, ChunkMeshData& mesh) {
        const ChunkPtr centre = makeChunk(kCentre, blocks::Stone, centreLevel);
        ChunkNeighbourhood neighbourhood(kCentre);
        neighbourhood.setChunk(0, 0, 0, centre);
        neighbourhood.setChunk(1, 0, 0,
                               makeChunk(ChunkPos{kCentre.x + 1, kCentre.y, kCentre.z},
                                         blocks::Stone, neighbourLevel));
        return mesher.mesh(neighbourhood, mesh);
    };

    GreedyMesher  mesher(registry());
    ChunkMeshData mesh;

    SECTION("same level: the seam is culled exactly as before")
    {
        REQUIRE(meshAgainst(kLodFull, kLodFull, mesher, mesh));
        CHECK(mesher.lastStats().quadsEmitted == 5u);
    }

    SECTION("fine chunk, coarse neighbour: the seam face survives")
    {
        REQUIRE(meshAgainst(kLodFull, 2, mesher, mesh));
        CHECK(mesher.lastStats().quadsEmitted == 6u);
    }

    SECTION("coarse chunk, fine neighbour: the seam face survives, plus the curtain")
    {
        REQUIRE(meshAgainst(2, kLodFull, mesher, mesh));
        CHECK(mesher.lastStats().skirtQuads == 4u);
        CHECK(mesher.lastStats().quadsEmitted == 6u + 4u);
    }

    SECTION("two coarse chunks at the same level still cull their shared seam")
    {
        REQUIRE(meshAgainst(2, 2, mesher, mesh));
        // Three curtains, not four. The +X neighbour is resident at this chunk's
        // own level, so the two quantise their surfaces on the same global cell
        // grid, their border heights are equal by construction and there is no
        // seam for a curtain to hide. The other three sides are unresolved and
        // still get one. Hanging a curtain on a matched seam is not free: over
        // water it is a second translucent quad coincident with the surface and
        // it draws a dark line along the border. See docs/VISUAL_REVIEW.md.
        CHECK(mesher.lastStats().skirtQuads == 3u);
        CHECK(mesher.lastStats().quadsEmitted == 5u + 3u);
    }

    SECTION("a coarse chunk fully surrounded at its own level hangs no curtain")
    {
        // The interior of an LOD band: every seam is matched, so the whole band
        // pays nothing for skirts. Air neighbours rather than solid ones so the
        // faces themselves survive and the count isolates the curtain.
        const ChunkPtr centre = makeChunk(kCentre, blocks::Stone, 2);
        REQUIRE(mesher.mesh(surroundWith(centre, blocks::Air, true, 2), mesh));
        CHECK(mesher.lastStats().skirtQuads == 0u);
        CHECK(mesher.lastStats().quadsEmitted == 6u);
    }
}

// -------------------------------------------------------------- sub-voxels --

TEST_CASE("a partially destroyed block emits no cube and culls nothing", "[mesh][lod][subvoxel]")
{
    GreedyMesher mesher(registry());
    ChunkPtr     centre = makeChunk(kCentre, blocks::Air, kLodFull);
    centre->setBlock(16, 16, 16, blocks::Stone);
    centre->setBlock(17, 16, 16, blocks::Stone);

    const std::size_t damaged = localIndex(16, 16, 16);
    REQUIRE(centre->breakSubVoxel(damaged, subVoxelIndex(0, 0, 0)) == SubVoxelEdit::Modified);
    REQUIRE_FALSE(centre->isBlockWhole(damaged));

    ChunkMeshData mesh;
    REQUIRE(mesher.mesh(surroundWith(centre, blocks::Air, false), mesh));

    // Only the intact block is meshed here, and it keeps all six faces including
    // the one pointing at its damaged neighbour. The damaged block's geometry
    // belongs to the sub-voxel mesher, which owns a separate buffer.
    CHECK(mesher.lastStats().facesEmitted == 6u);
    CHECK(mesher.lastStats().quadsEmitted == 6u);
    CHECK(mesh.boundsMin == glm::vec3{17.0f, 16.0f, 16.0f});
    CHECK(mesh.boundsMax == glm::vec3{18.0f, 17.0f, 17.0f});

    const std::multiset<UnitFace> covered = coveredFaces(mesh);
    CHECK(covered.count(UnitFace{static_cast<int>(Direction::NegX), 17, 16, 16}) == 1u);
    for (std::size_t d = 0; d < kDirectionCount; ++d) {
        CHECK(covered.count(UnitFace{static_cast<int>(d), 16, 16, 16}) == 0u);
    }
}

TEST_CASE("damage in a neighbour chunk unculls the seam face", "[mesh][lod][subvoxel]")
{
    GreedyMesher mesher(registry());
    ChunkPtr     centre = makeChunk(kCentre, blocks::Air, kLodFull);
    centre->setBlock(kChunkSize - 1, 16, 16, blocks::Stone);

    ChunkPtr east = makeChunk(ChunkPos{kCentre.x + 1, kCentre.y, kCentre.z}, blocks::Air);
    east->setBlock(0, 16, 16, blocks::Stone);

    ChunkNeighbourhood neighbourhood(kCentre);
    neighbourhood.setChunk(0, 0, 0, centre);
    neighbourhood.setChunk(1, 0, 0, east);

    ChunkMeshData intact;
    REQUIRE(mesher.mesh(neighbourhood, intact));
    CHECK(mesher.lastStats().quadsEmitted == 5u);  // +X hidden by the neighbour's block

    REQUIRE(east->breakSubVoxel(localIndex(0, 16, 16), subVoxelIndex(4, 4, 4)) ==
            SubVoxelEdit::Modified);

    ChunkMeshData damaged;
    REQUIRE(mesher.mesh(neighbourhood, damaged));
    CHECK(mesher.lastStats().quadsEmitted == 6u);
    CHECK(coveredFaces(damaged) == expectedFacesAtLevel0(neighbourhood));
}

TEST_CASE("sub-voxel damage is ignored above level 0", "[mesh][lod][subvoxel]")
{
    // A whole level-2 cell is 64 blocks; honouring one eighth of one of them
    // would cost a sparse-table lookup per reduced block and change nothing a
    // player could see from LOD distance.
    GreedyMesher   mesher(registry());
    const ChunkPtr centre = makeChunk(kCentre, blocks::Stone, 2);
    REQUIRE(centre->breakSubVoxel(localIndex(0, kChunkSize - 1, 0), subVoxelIndex(0, 0, 0)) ==
            SubVoxelEdit::Modified);

    ChunkMeshData mesh;
    REQUIRE(mesher.mesh(surroundWith(centre, blocks::Air, false), mesh));
    CHECK(mesher.lastStats().quadsEmitted == 10u);  // six sides plus four curtains, unchanged
}
