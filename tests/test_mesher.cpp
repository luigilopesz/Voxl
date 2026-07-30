#include <catch2/catch_test_macros.hpp>

#include "mesh/GreedyMesher.hpp"
#include "mesh/MeshData.hpp"
#include "world/Block.hpp"
#include "world/BlockAccess.hpp"
#include "world/Chunk.hpp"
#include "world/VoxelTypes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

using namespace voxl;

namespace {

/// Section 4 of 8. Deliberately not section 0 or 7: below the world the
/// out-of-world rule returns opaque bedrock and above it returns air, either of
/// which would silently cull or expose a face and make the counts below wrong
/// for reasons that have nothing to do with the mesher.
constexpr ChunkPos kCentre{0, 4, 0};

const BlockRegistry& registry()
{
    static const BlockRegistry instance = createDefaultBlockRegistry();
    return instance;
}

/// A chunk with uniform contents and uniform full sunlight.
///
/// The light matters: kMissingChunkLight is sunlight 15, so filling every test
/// chunk with the same value means light is constant across chunk seams and any
/// failure to merge is a real merging failure rather than a lighting step the
/// test did not intend.
ChunkPtr makeChunk(const ChunkPos& position, BlockId fill)
{
    ChunkPtr chunk = Chunk::create(position);
    chunk->storage().fill(fill);
    chunk->fillLight(ChunkStorage::kMaxLightLevel, 0);
    chunk->forceState(ChunkState::Ready);
    return chunk;
}

/// Wraps `centre` in a neighbourhood. When `populate` is false the 26 outer
/// slots are left null, which BlockAccess.hpp defines to read as air.
ChunkNeighbourhood surroundWith(const ChunkPtr& centre, BlockId surround, bool populate)
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
                neighbourhood.setChunk(
                    dx, dy, dz,
                    makeChunk(ChunkPos{base.x + dx, base.y + dy, base.z + dz}, surround));
            }
        }
    }
    return neighbourhood;
}

struct Quad {
    MeshIndex                       base = 0;
    std::array<VertexAttributes, 4> corners{};
    std::array<MeshIndex, 6>        indices{};

    [[nodiscard]] Direction direction() const noexcept { return corners[0].direction; }
};

/// Unpacks a layer back into quads. Relies on the mesher writing exactly four
/// vertices and six indices per quad, in emission order.
std::vector<Quad> quadsOf(const MeshLayerData& layer)
{
    REQUIRE(layer.vertexCount() % 4 == 0u);
    REQUIRE(layer.indexCount() == layer.vertexCount() / 4 * 6);

    std::vector<Quad> quads;
    quads.reserve(layer.vertexCount() / 4);
    for (std::size_t q = 0; q < layer.vertexCount() / 4; ++q) {
        Quad quad;
        quad.base = static_cast<MeshIndex>(q * 4);
        for (std::size_t c = 0; c < 4; ++c) {
            quad.corners[c] = unpackVertex(layer.vertices[q * 4 + c]);
        }
        for (std::size_t i = 0; i < 6; ++i) {
            quad.indices[i] = layer.indices[q * 6 + i];
        }
        quads.push_back(quad);
    }
    return quads;
}

[[nodiscard]] std::size_t quadCount(const ChunkMeshData& mesh, RenderLayer which)
{
    return mesh.layer(which).vertexCount() / 4;
}

[[nodiscard]] bool hasDirection(const ChunkMeshData& mesh, RenderLayer which, Direction direction)
{
    for (const Quad& quad : quadsOf(mesh.layer(which))) {
        if (quad.direction() == direction) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("an all-air chunk emits no geometry", "[mesh][mesher]")
{
    GreedyMesher   mesher(registry());
    const ChunkPtr centre = makeChunk(kCentre, blocks::Air);

    ChunkMeshData mesh;
    CHECK_FALSE(mesher.mesh(surroundWith(centre, blocks::Stone, true), mesh));
    CHECK(mesh.empty());
    CHECK(mesh.vertexCount() == 0u);
    CHECK(mesher.lastStats().quadsEmitted == 0u);
    CHECK(mesh.position == kCentre);
}

TEST_CASE("a neighbourhood with no centre chunk produces nothing", "[mesh][mesher]")
{
    GreedyMesher            mesher(registry());
    const ChunkNeighbourhood empty(kCentre);

    ChunkMeshData mesh;
    CHECK_FALSE(mesher.mesh(empty, mesh));
    CHECK(mesh.empty());
}

TEST_CASE("a single isolated block emits exactly six unit quads", "[mesh][mesher]")
{
    GreedyMesher mesher(registry());
    ChunkPtr     centre = makeChunk(kCentre, blocks::Air);
    centre->setBlock(16, 16, 16, blocks::Stone);

    ChunkMeshData mesh;
    REQUIRE(mesher.mesh(surroundWith(centre, blocks::Stone, false), mesh));

    CHECK(mesher.lastStats().facesEmitted == 6u);
    CHECK(mesher.lastStats().quadsEmitted == 6u);
    CHECK(quadCount(mesh, RenderLayer::Opaque) == 6u);
    CHECK(quadCount(mesh, RenderLayer::Cutout) == 0u);
    CHECK(quadCount(mesh, RenderLayer::Translucent) == 0u);

    // One quad per direction, each a single block across, nothing occluding it.
    std::array<int, kDirectionCount> perDirection{};
    for (const Quad& quad : quadsOf(mesh.layer(RenderLayer::Opaque))) {
        ++perDirection[static_cast<std::size_t>(quad.direction())];
        for (const VertexAttributes& corner : quad.corners) {
            CHECK(corner.width == 1);
            CHECK(corner.height == 1);
            CHECK(corner.ao == kMaxAoLevel);
            CHECK(corner.sunlight == ChunkStorage::kMaxLightLevel);
            CHECK(corner.blockLight == 0);
        }
    }
    for (const int count : perDirection) {
        CHECK(count == 1);
    }

    // Tight bounds around the one block, not the whole 32^3 section.
    CHECK(mesh.boundsMin == glm::vec3{16.0f, 16.0f, 16.0f});
    CHECK(mesh.boundsMax == glm::vec3{17.0f, 17.0f, 17.0f});
}

TEST_CASE("a solid chunk enclosed by opaque neighbours emits nothing", "[mesh][mesher]")
{
    // The definitive interior-culling test: 32^3 = 32768 blocks, 196608 faces,
    // every single one of them hidden.
    GreedyMesher   mesher(registry());
    const ChunkPtr centre = makeChunk(kCentre, blocks::Stone);

    ChunkMeshData mesh;
    CHECK_FALSE(mesher.mesh(surroundWith(centre, blocks::Stone, true), mesh));
    CHECK(mesher.lastStats().facesEmitted == 0u);
    CHECK(mesh.empty());
}

TEST_CASE("a solid chunk exposed on all sides merges into six full quads", "[mesh][mesher]")
{
    // The definitive greedy test: 1024 coplanar faces per side must collapse to
    // one 32x32 quad, six times over.
    GreedyMesher   mesher(registry());
    const ChunkPtr centre = makeChunk(kCentre, blocks::Stone);

    ChunkMeshData mesh;
    REQUIRE(mesher.mesh(surroundWith(centre, blocks::Air, false), mesh));

    CHECK(mesher.lastStats().facesEmitted == 6u * kChunkVolume / kChunkSize);
    CHECK(mesher.lastStats().quadsEmitted == 6u);
    CHECK(quadCount(mesh, RenderLayer::Opaque) == 6u);
    CHECK(mesh.triangleCount() == 12u);

    bool sawFarEdge = false;
    for (const Quad& quad : quadsOf(mesh.layer(RenderLayer::Opaque))) {
        for (const VertexAttributes& corner : quad.corners) {
            CHECK(corner.width == kChunkSize);
            CHECK(corner.height == kChunkSize);
            // 32 is why the packed position field is six bits, not five.
            sawFarEdge = sawFarEdge || corner.x == kChunkSize || corner.y == kChunkSize ||
                         corner.z == kChunkSize;
        }
    }
    CHECK(sawFarEdge);

    CHECK(mesh.boundsMin == glm::vec3{0.0f, 0.0f, 0.0f});
    CHECK(mesh.boundsMax == glm::vec3{32.0f, 32.0f, 32.0f});
}

TEST_CASE("populated air neighbours behave exactly like absent ones", "[mesh][mesher]")
{
    // Guards against a mesher that culls seams by asking "is a neighbour chunk
    // present?" instead of "what block is on the other side?".
    GreedyMesher   mesher(registry());
    const ChunkPtr centre = makeChunk(kCentre, blocks::Stone);

    ChunkMeshData mesh;
    REQUIRE(mesher.mesh(surroundWith(centre, blocks::Air, true), mesh));
    CHECK(mesher.lastStats().quadsEmitted == 6u);
}

TEST_CASE("border faces are culled against the neighbour chunk's blocks", "[mesh][mesher]")
{
    GreedyMesher   mesher(registry());
    const ChunkPtr centre = makeChunk(kCentre, blocks::Stone);

    ChunkNeighbourhood neighbourhood(kCentre);
    neighbourhood.setChunk(0, 0, 0, centre);
    neighbourhood.setChunk(1, 0, 0,
                           makeChunk(ChunkPos{kCentre.x + 1, kCentre.y, kCentre.z}, blocks::Stone));

    ChunkMeshData mesh;
    REQUIRE(mesher.mesh(neighbourhood, mesh));

    // Five sides face air, the +X side faces stone one chunk over.
    CHECK(mesher.lastStats().quadsEmitted == 5u);
    CHECK_FALSE(hasDirection(mesh, RenderLayer::Opaque, Direction::PosX));
    CHECK(hasDirection(mesh, RenderLayer::Opaque, Direction::NegX));
}

TEST_CASE("a single border block is culled by a single neighbour block", "[mesh][mesher]")
{
    GreedyMesher mesher(registry());
    ChunkPtr     centre = makeChunk(kCentre, blocks::Air);
    centre->setBlock(kChunkSize - 1, 16, 16, blocks::Stone);

    ChunkPtr east = makeChunk(ChunkPos{kCentre.x + 1, kCentre.y, kCentre.z}, blocks::Air);
    east->setBlock(0, 16, 16, blocks::Stone);

    ChunkNeighbourhood neighbourhood(kCentre);
    neighbourhood.setChunk(0, 0, 0, centre);
    neighbourhood.setChunk(1, 0, 0, east);

    ChunkMeshData mesh;
    REQUIRE(mesher.mesh(neighbourhood, mesh));
    CHECK(mesher.lastStats().quadsEmitted == 5u);
    CHECK_FALSE(hasDirection(mesh, RenderLayer::Opaque, Direction::PosX));
}

TEST_CASE("a checkerboard cannot merge at all", "[mesh][mesher]")
{
    // Every solid block is surrounded by six air blocks and every neighbour in
    // the face plane is solid, so no two mask cells are ever adjacent: the quad
    // count must equal the face count exactly.
    GreedyMesher mesher(registry());
    ChunkPtr     centre = makeChunk(kCentre, blocks::Air);
    for (std::int32_t y = 0; y < kChunkSize; ++y) {
        for (std::int32_t z = 0; z < kChunkSize; ++z) {
            for (std::int32_t x = 0; x < kChunkSize; ++x) {
                if (((x + y + z) & 1) == 0) {
                    centre->setBlock(x, y, z, blocks::Stone);
                }
            }
        }
    }

    ChunkMeshData mesh;
    REQUIRE(mesher.mesh(surroundWith(centre, blocks::Air, false), mesh));

    constexpr std::size_t kSolidBlocks = kChunkVolume / 2;  // exactly half by parity
    CHECK(mesher.lastStats().facesEmitted == kSolidBlocks * kDirectionCount);
    CHECK(mesher.lastStats().quadsEmitted == kSolidBlocks * kDirectionCount);

    // Aggregated rather than asserted per quad: there are 98304 of them and
    // Catch2 assertion bookkeeping would dominate the test's runtime.
    bool allUnit = true;
    for (const Quad& quad : quadsOf(mesh.layer(RenderLayer::Opaque))) {
        allUnit = allUnit && quad.corners[0].width == 1 && quad.corners[0].height == 1;
    }
    CHECK(allUnit);
}

TEST_CASE("water culls against water but not against air", "[mesh][mesher][water]")
{
    GreedyMesher mesher(registry());

    SECTION("a water section enclosed in water has no internal surfaces")
    {
        const ChunkPtr centre = makeChunk(kCentre, blocks::Water);
        ChunkMeshData  mesh;
        CHECK_FALSE(mesher.mesh(surroundWith(centre, blocks::Water, true), mesh));
        CHECK(mesher.lastStats().facesEmitted == 0u);
    }

    SECTION("an isolated water block is fully drawn, in the translucent layer")
    {
        ChunkPtr centre = makeChunk(kCentre, blocks::Air);
        centre->setBlock(16, 16, 16, blocks::Water);

        ChunkMeshData mesh;
        REQUIRE(mesher.mesh(surroundWith(centre, blocks::Air, false), mesh));
        CHECK(quadCount(mesh, RenderLayer::Translucent) == 6u);
        CHECK(quadCount(mesh, RenderLayer::Opaque) == 0u);
        CHECK(quadCount(mesh, RenderLayer::Cutout) == 0u);
        CHECK(hasDirection(mesh, RenderLayer::Translucent, Direction::PosY));
    }

    SECTION("a half-filled water section keeps only its outer shell")
    {
        ChunkPtr centre = makeChunk(kCentre, blocks::Air);
        for (std::int32_t y = 0; y < kChunkSize / 2; ++y) {
            for (std::int32_t z = 0; z < kChunkSize; ++z) {
                for (std::int32_t x = 0; x < kChunkSize; ++x) {
                    centre->setBlock(x, y, z, blocks::Water);
                }
            }
        }

        ChunkMeshData mesh;
        REQUIRE(mesher.mesh(surroundWith(centre, blocks::Air, false), mesh));

        // One surface, one floor, four walls; the 16 * 32 * 32 interior faces
        // are all water-against-water and gone.
        CHECK(mesher.lastStats().quadsEmitted == 6u);
        CHECK(quadCount(mesh, RenderLayer::Translucent) == 6u);
        CHECK(quadCount(mesh, RenderLayer::Opaque) == 0u);

        bool sawSurface = false;
        for (const Quad& quad : quadsOf(mesh.layer(RenderLayer::Translucent))) {
            if (quad.direction() != Direction::PosY) {
                continue;
            }
            sawSurface = true;
            CHECK(quad.corners[0].y == kChunkSize / 2);
            CHECK(quad.corners[0].width == kChunkSize);
            CHECK(quad.corners[0].height == kChunkSize);
            // The surface drop cannot live in the packed vertex (integer
            // positions only); the shader recognises the quad by this layer.
            CHECK(quad.corners[0].textureLayer == waterSurfaceTextureLayer(registry()));
        }
        CHECK(sawSurface);
    }
}

TEST_CASE("cutout blocks do not self-cull and land in their own layer", "[mesh][mesher]")
{
    // Leaves are non-opaque but not translucent, so BlockRegistry::facesHidden
    // keeps the shared face between two leaf blocks. That is the documented
    // contract, and this test pins it so a future "optimisation" cannot quietly
    // hollow out every tree.
    GreedyMesher mesher(registry());
    ChunkPtr     centre = makeChunk(kCentre, blocks::Air);
    centre->setBlock(16, 16, 16, blocks::Leaves);
    centre->setBlock(17, 16, 16, blocks::Leaves);

    ChunkMeshData mesh;
    REQUIRE(mesher.mesh(surroundWith(centre, blocks::Air, false), mesh));

    // 12 faces: both blocks keep all six, including the two along the shared
    // boundary. They merge to 8 quads because the four coplanar pairs (top,
    // bottom and the two Z sides) are 1x2 rectangles.
    CHECK(mesher.lastStats().facesEmitted == 12u);
    CHECK(quadCount(mesh, RenderLayer::Cutout) == 8u);
    CHECK(quadCount(mesh, RenderLayer::Opaque) == 0u);
    CHECK(quadCount(mesh, RenderLayer::Translucent) == 0u);
}

TEST_CASE("different block types never merge into one quad", "[mesh][mesher]")
{
    GreedyMesher mesher(registry());
    ChunkPtr     centre = makeChunk(kCentre, blocks::Air);
    for (std::int32_t z = 0; z < kChunkSize; ++z) {
        for (std::int32_t x = 0; x < kChunkSize; ++x) {
            centre->setBlock(x, 0, z, x < kChunkSize / 2 ? blocks::Stone : blocks::Dirt);
        }
    }

    ChunkMeshData mesh;
    REQUIRE(mesher.mesh(surroundWith(centre, blocks::Air, false), mesh));

    // Top and bottom each split down the material seam (2 + 2), the -X and +X
    // ends are single-material (1 + 1), and the -Z and +Z sides split (2 + 2).
    CHECK(mesher.lastStats().quadsEmitted == 10u);
}

TEST_CASE("ambient occlusion drives the triangle split diagonal", "[mesh][mesher][ao]")
{
    // A lone diagonal neighbour darkens exactly one corner of two different
    // faces, and darkens opposite ends of their split diagonals. MeshLayerData
    // must therefore choose a different diagonal for each, otherwise one of the
    // two shows the classic bright seam across the quad.
    GreedyMesher mesher(registry());
    ChunkPtr     centre = makeChunk(kCentre, blocks::Air);
    centre->setBlock(16, 16, 16, blocks::Stone);
    centre->setBlock(15, 17, 15, blocks::Stone);

    ChunkMeshData mesh;
    REQUIRE(mesher.mesh(surroundWith(centre, blocks::Air, false), mesh));

    bool checkedTop  = false;
    bool checkedSide = false;
    for (const Quad& quad : quadsOf(mesh.layer(RenderLayer::Opaque))) {
        const VertexAttributes& origin = quad.corners[0];

        if (quad.direction() == Direction::PosY && origin.x == 16 && origin.y == 17 &&
            origin.z == 16) {
            // Corner 0 is occluded: ao0 + ao2 < ao1 + ao3, so the split runs
            // 1-3 and the first index is not the origin vertex.
            CHECK(quad.corners[0].ao == 2);
            CHECK(quad.corners[1].ao == kMaxAoLevel);
            CHECK(quad.corners[2].ao == kMaxAoLevel);
            CHECK(quad.corners[3].ao == kMaxAoLevel);
            CHECK(quad.indices[0] == quad.base + 1);
            checkedTop = true;
        }

        if (quad.direction() == Direction::NegX && origin.x == 16 && origin.y == 16 &&
            origin.z == 16) {
            // Corner 3 is the occluded one: ao0 + ao2 > ao1 + ao3, so the split
            // runs 0-2 and starts at the origin vertex.
            CHECK(quad.corners[3].ao == 2);
            CHECK(quad.corners[0].ao == kMaxAoLevel);
            CHECK(quad.indices[0] == quad.base + 0);
            checkedSide = true;
        }
    }
    CHECK(checkedTop);
    CHECK(checkedSide);
}

TEST_CASE("lighting differences block merging", "[mesh][mesher]")
{
    // Two coplanar faces whose overlying light differs must stay separate;
    // merging them would stretch one light value across both and band the wall.
    GreedyMesher mesher(registry());
    ChunkPtr     centre = makeChunk(kCentre, blocks::Air);
    for (std::int32_t z = 0; z < kChunkSize; ++z) {
        for (std::int32_t x = 0; x < kChunkSize; ++x) {
            centre->setBlock(x, 0, z, blocks::Stone);
        }
    }

    ChunkMeshData uniform;
    REQUIRE(mesher.mesh(surroundWith(centre, blocks::Air, false), uniform));
    const std::size_t uniformQuads = mesher.lastStats().quadsEmitted;

    // Darken one column of the air directly above the slab. Nothing else about
    // the geometry changes.
    for (std::int32_t z = 0; z < kChunkSize; ++z) {
        centre->setLight(localIndex(0, 1, z), ChunkStorage::packLight(4, 0));
    }

    ChunkMeshData banded;
    REQUIRE(mesher.mesh(surroundWith(centre, blocks::Air, false), banded));
    CHECK(mesher.lastStats().quadsEmitted > uniformQuads);
}

TEST_CASE("the recorded content version matches the chunk at capture time", "[mesh][mesher]")
{
    GreedyMesher mesher(registry());
    ChunkPtr     centre = makeChunk(kCentre, blocks::Air);
    centre->setBlock(1, 1, 1, blocks::Stone);

    ChunkMeshData mesh;
    REQUIRE(mesher.mesh(surroundWith(centre, blocks::Air, false), mesh));
    CHECK(mesh.contentVersion == centre->contentVersion());

    centre->setBlock(2, 2, 2, blocks::Stone);
    CHECK(mesh.contentVersion != centre->contentVersion());
}

TEST_CASE("every emitted vertex round-trips through the packed format", "[mesh][mesher]")
{
    GreedyMesher mesher(registry());
    ChunkPtr     centre = makeChunk(kCentre, blocks::Air);
    for (std::int32_t y = 0; y < kChunkSize; ++y) {
        for (std::int32_t z = 0; z < kChunkSize; ++z) {
            for (std::int32_t x = 0; x < kChunkSize; ++x) {
                if (((x * 7 + y * 13 + z * 29) % 5) < 3) {
                    centre->setBlock(x, y, z, blocks::Stone);
                }
            }
        }
    }

    ChunkMeshData mesh;
    REQUIRE(mesher.mesh(surroundWith(centre, blocks::Air, false), mesh));
    REQUIRE(mesh.vertexCount() > 0u);

    // Aggregated: tens of thousands of vertices, one assertion.
    bool wellFormed = true;
    for (const MeshLayerData& layer : mesh.layers) {
        for (const PackedVertex& vertex : layer.vertices) {
            const VertexAttributes attributes = unpackVertex(vertex);
            wellFormed = wellFormed &&
                         // The reserved bits are part of the GLSL contract: the
                         // shader masks fields assuming they are zero.
                         (vertex.data0 & 0x8000'0000u) == 0u &&
                         (vertex.data1 & 0xFF00'0000u) == 0u &&
                         attributes.x <= kChunkSize && attributes.y <= kChunkSize &&
                         attributes.z <= kChunkSize && attributes.width >= 1 &&
                         attributes.height >= 1 && attributes.ao <= kMaxAoLevel &&
                         attributes.textureLayer <= kMaxTextureLayer &&
                         packVertex(attributes) == vertex;
        }
    }
    CHECK(wellFormed);
}
