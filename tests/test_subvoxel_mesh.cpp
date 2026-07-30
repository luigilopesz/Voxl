#include <catch2/catch_test_macros.hpp>

// Coverage for mesh/SubVoxelMesher.cpp.
//
// Written during integration because the agent that produced SubVoxelMesher.hpp
// never produced the implementation or its tests, so the whole sweep - roughly
// 350 lines - arrived unverified. The properties pinned here are the ones the
// packed format and the SubVoxel.hpp invariant actually depend on: nothing in
// this file asserts a triangle count that a legitimate change to the merge order
// would break, except where the count IS the property (a merged wall is one quad,
// an enclosed pocket is six faces).

#include "mesh/SubVoxelMesh.hpp"
#include "mesh/SubVoxelMesher.hpp"
#include "world/Block.hpp"
#include "world/BlockAccess.hpp"
#include "world/Chunk.hpp"
#include "world/SubVoxel.hpp"
#include "world/VoxelTypes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

using namespace voxl;

namespace {

/// Section 4 of 8, matching test_mesher.cpp and test_lod_mesh.cpp: below the
/// world the out-of-world rule returns opaque bedrock and above it returns air,
/// either of which would change face counts for reasons unrelated to sub-voxels.
constexpr ChunkPos kCentre{0, 4, 0};

const BlockRegistry& registry()
{
    static const BlockRegistry instance = createDefaultBlockRegistry();
    return instance;
}

ChunkPtr makeChunk(const ChunkPos& position, BlockId fill)
{
    ChunkPtr chunk = Chunk::create(position);
    chunk->storage().fill(fill);
    chunk->fillLight(ChunkStorage::kMaxLightLevel, 0);
    chunk->forceState(ChunkState::Ready);
    return chunk;
}

/// Wraps `centre` in a neighbourhood whose 26 outer slots hold `surround`.
ChunkNeighbourhood surroundWith(const ChunkPtr& centre, BlockId surround)
{
    ChunkNeighbourhood neighbourhood(centre->position());
    neighbourhood.setChunk(0, 0, 0, centre);

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
    std::array<SubVoxelVertexAttributes, 4> corners{};

    [[nodiscard]] Direction direction() const noexcept { return corners[0].direction; }
    [[nodiscard]] std::uint8_t width() const noexcept { return corners[0].width; }
    [[nodiscard]] std::uint8_t height() const noexcept { return corners[0].height; }
    /// Sub-voxels the quad covers.
    [[nodiscard]] std::size_t area() const noexcept
    {
        return static_cast<std::size_t>(width()) * height();
    }
};

std::vector<Quad> quadsOf(const SubVoxelMeshData& mesh)
{
    REQUIRE(mesh.vertices.size() % 4 == 0u);
    std::vector<Quad> quads;
    quads.reserve(mesh.vertices.size() / 4);
    for (std::size_t q = 0; q < mesh.vertices.size() / 4; ++q) {
        Quad quad;
        for (std::size_t c = 0; c < 4; ++c) {
            quad.corners[c] = unpackSubVoxelVertex(mesh.vertices[q * 4 + c]);
        }
        quads.push_back(quad);
    }
    return quads;
}

/// Carves `subIndex` out of the block at local (x,y,z), going through the Chunk
/// API so the SubVoxel.hpp invariant is maintained exactly as it is in the game.
void carve(const ChunkPtr& chunk, std::int32_t x, std::int32_t y, std::int32_t z,
           std::size_t subIndex)
{
    const SubVoxelEdit edit = chunk->breakSubVoxel(localIndex(x, y, z), subIndex);
    REQUIRE(edit != SubVoxelEdit::Unchanged);
}

}  // namespace

TEST_CASE("an undamaged chunk produces no sub-voxel geometry", "[subvoxel][mesh]")
{
    const ChunkPtr           centre = makeChunk(kCentre, blocks::Stone);
    const ChunkNeighbourhood hood   = surroundWith(centre, blocks::Stone);

    SubVoxelMesher   mesher{registry()};
    SubVoxelMeshData mesh;

    REQUIRE_FALSE(mesher.mesh(hood, mesh));
    CHECK(mesh.empty());
    CHECK(mesh.vertices.empty());
    CHECK(mesh.byteSize() == 0u);
    // The fast path must not even look at a block: this is the claim that makes
    // the feature free for terrain nobody has dug into.
    CHECK(mesher.lastStats().blocksMeshed == 0u);
    CHECK(mesher.lastStats().facesEmitted == 0u);
}

TEST_CASE("a single sub-voxel carved out of a buried block emits its pocket walls",
          "[subvoxel][mesh]")
{
    const ChunkPtr centre = makeChunk(kCentre, blocks::Stone);
    // Dead centre of the block and of the chunk, so every neighbour in all three
    // axes is solid and nothing about the result depends on a chunk border.
    carve(centre, 16, 16, 16, subVoxelIndex(4, 4, 4));

    const ChunkNeighbourhood hood = surroundWith(centre, blocks::Stone);

    SubVoxelMesher   mesher{registry()};
    SubVoxelMeshData mesh;
    REQUIRE(mesher.mesh(hood, mesh));

    // The hole is a 1x1x1 cavity fully inside solid material. Each of its six
    // walls is one sub-voxel face belonging to one of the six sub-voxels around
    // it, so the pocket is exactly six faces and, being 1x1 each, six quads.
    CHECK(mesher.lastStats().blocksMeshed == 1u);
    CHECK(mesher.lastStats().facesEmitted == 6u);
    CHECK(mesher.lastStats().quadsEmitted == 6u);

    const std::vector<Quad> quads = quadsOf(mesh);
    REQUIRE(quads.size() == 6u);

    // One wall per direction, and every wall a single sub-voxel.
    std::set<Direction> directions;
    for (const Quad& quad : quads) {
        directions.insert(quad.direction());
        CHECK(quad.area() == 1u);
    }
    CHECK(directions.size() == kDirectionCount);

    CHECK(mesh.indices.size() == 6u * 6u);
    CHECK(mesh.triangleCount() == 12u);
}

TEST_CASE("a carved slab under open sky merges into one quad per exposed plane",
          "[subvoxel][mesh]")
{
    const ChunkPtr centre = makeChunk(kCentre, blocks::Stone);

    // Clear the block directly above so the carved surface faces open air, then
    // remove the whole top 8x8 layer of sub-voxels from the block below it.
    centre->setBlock(16, 17, 16, blocks::Air);
    for (std::int32_t sz = 0; sz < kSubVoxelResolution; ++sz) {
        for (std::int32_t sx = 0; sx < kSubVoxelResolution; ++sx) {
            carve(centre, 16, 16, 16, subVoxelIndex(sx, kSubVoxelResolution - 1, sz));
        }
    }

    const ChunkNeighbourhood hood = surroundWith(centre, blocks::Stone);

    SubVoxelMesher   mesher{registry()};
    SubVoxelMeshData mesh;
    REQUIRE(mesher.mesh(hood, mesh));

    const std::vector<Quad> quads = quadsOf(mesh);

    // The new floor is a flat 8x8 sheet of coplanar faces with identical light and
    // AO, so greedy merging must collapse it to exactly one quad. If this comes
    // back as 64, merging is not running at all.
    std::size_t upwardQuads = 0;
    std::size_t upwardArea  = 0;
    for (const Quad& quad : quads) {
        if (quad.direction() == Direction::PosY) {
            ++upwardQuads;
            upwardArea += quad.area();
        }
    }
    CHECK(upwardQuads == 1u);
    CHECK(upwardArea == 64u);

    // Every emitted quad must fit the 3-bit extent fields.
    for (const Quad& quad : quads) {
        CHECK(quad.width() >= 1u);
        CHECK(quad.width() <= kSubVoxelResolution);
        CHECK(quad.height() >= 1u);
        CHECK(quad.height() <= kSubVoxelResolution);
    }

    // The removed layer was the block's own top, so no face points up out of the
    // block's former top surface into the air block - that surface is now 1/8 of a
    // block lower, which is the whole point of the feature.
    CHECK(mesher.lastStats().facesEmitted >= 64u);
}

TEST_CASE("sub-voxel positions and extents stay inside the packed fields", "[subvoxel][mesh]")
{
    const ChunkPtr centre = makeChunk(kCentre, blocks::Stone);

    // The extreme corner block of the chunk, carved at the extreme corner
    // sub-voxel: this is the case that overflows a 9-bit position field if the
    // block base is computed wrongly, because the quad reaches coordinate 256.
    centre->setBlock(31, 31, 31, blocks::Stone);
    carve(centre, 31, 31, 31, subVoxelIndex(7, 7, 7));

    const ChunkNeighbourhood hood = surroundWith(centre, blocks::Air);

    SubVoxelMesher   mesher{registry()};
    SubVoxelMeshData mesh;
    REQUIRE(mesher.mesh(hood, mesh));

    bool sawMaximum = false;
    for (const Quad& quad : quadsOf(mesh)) {
        for (const SubVoxelVertexAttributes& corner : quad.corners) {
            CHECK(corner.x <= kSubVtxMaxPos);
            CHECK(corner.y <= kSubVtxMaxPos);
            CHECK(corner.z <= kSubVtxMaxPos);
            sawMaximum = sawMaximum || corner.x == kSubVtxMaxPos || corner.y == kSubVtxMaxPos ||
                         corner.z == kSubVtxMaxPos;
        }
    }
    // Confirms the fixture actually reached the boundary rather than passing
    // because nothing got close to it.
    CHECK(sawMaximum);
}

TEST_CASE("the packed sub-voxel vertex round-trips every field", "[subvoxel][mesh]")
{
    SubVoxelVertexAttributes in;
    in.x            = kSubVtxMaxPos;
    in.y            = 0;
    in.z            = 129;
    in.direction    = Direction::NegZ;
    in.textureLayer = kMaxTextureLayer;
    in.width        = kSubVoxelResolution;
    in.height       = 1;
    in.corner       = QuadCorner::UV;
    in.sunlight     = 15;
    in.blockLight   = 9;
    in.ao           = kMaxAoLevel;

    const SubVoxelVertexAttributes out = unpackSubVoxelVertex(packSubVoxelVertex(in));

    CHECK(out.x == in.x);
    CHECK(out.y == in.y);
    CHECK(out.z == in.z);
    CHECK(out.direction == in.direction);
    CHECK(out.textureLayer == in.textureLayer);
    CHECK(out.width == in.width);
    CHECK(out.height == in.height);
    CHECK(out.corner == in.corner);
    CHECK(out.sunlight == in.sunlight);
    CHECK(out.blockLight == in.blockLight);
    CHECK(out.ao == in.ao);
}

TEST_CASE("a damaged neighbour occludes only the sub-voxels it still has",
          "[subvoxel][mesh]")
{
    const ChunkPtr centre = makeChunk(kCentre, blocks::Stone);

    // Two blocks side by side along +X, both damaged. The left one loses its whole
    // +X face layer, the right one loses its whole -X face layer, so the gap
    // between them is open across all 64 cells and both must emit that wall.
    for (std::int32_t sz = 0; sz < kSubVoxelResolution; ++sz) {
        for (std::int32_t sy = 0; sy < kSubVoxelResolution; ++sy) {
            carve(centre, 16, 16, 16, subVoxelIndex(kSubVoxelResolution - 1, sy, sz));
            carve(centre, 17, 16, 16, subVoxelIndex(0, sy, sz));
        }
    }

    const ChunkNeighbourhood hood = surroundWith(centre, blocks::Stone);

    SubVoxelMesher   mesher{registry()};
    SubVoxelMeshData mesh;
    REQUIRE(mesher.mesh(hood, mesh));

    CHECK(mesher.lastStats().blocksMeshed == 2u);

    // The left block's remaining material now faces the void across the boundary,
    // so it must show a +X wall; the right block must show the mirroring -X wall.
    // If the cross-block occlusion test used the wrong slice of the neighbour's
    // grid, one of these two comes back zero.
    std::size_t posX = 0;
    std::size_t negX = 0;
    for (const Quad& quad : quadsOf(mesh)) {
        if (quad.direction() == Direction::PosX) {
            posX += quad.area();
        }
        if (quad.direction() == Direction::NegX) {
            negX += quad.area();
        }
    }
    CHECK(posX == 64u);
    CHECK(negX == 64u);
}

TEST_CASE("an intact neighbour hides the face between two blocks", "[subvoxel][mesh]")
{
    // A differential test, because the absolute count alone proves nothing. The
    // carve is an INTERIOR slot (sub-voxel layer 3), deliberately not the boundary
    // layer, so the block keeps material at sub-voxel 7 pressed against its
    // neighbour. That material's +X face is the one facesHidden has to cull.
    //
    // Carving the boundary layer instead would make this vacuous: there would be
    // no sub-voxel left at the boundary for anything to cull.
    constexpr std::int32_t kSlot = 3;

    /// Total +X area, and how much of it lies on the shared block boundary.
    struct Result {
        std::size_t posXArea     = 0;
        std::size_t boundaryArea = 0;
    };

    // Block local x = 16 starts at sub-voxel 128, so the +X face of its last
    // sub-voxel - the shared boundary - is the plane at 128 + 7 + 1.
    constexpr std::uint16_t kBoundaryPlane = 16 * kSubVoxelResolution + kSubVoxelResolution;

    const auto build = [](BlockId neighbourBlock) {
        const ChunkPtr centre = makeChunk(kCentre, blocks::Stone);
        centre->setBlock(17, 16, 16, neighbourBlock);
        for (std::int32_t sz = 0; sz < kSubVoxelResolution; ++sz) {
            for (std::int32_t sy = 0; sy < kSubVoxelResolution; ++sy) {
                carve(centre, 16, 16, 16, subVoxelIndex(kSlot, sy, sz));
            }
        }

        const ChunkNeighbourhood hood = surroundWith(centre, blocks::Stone);
        SubVoxelMesher           mesher{registry()};
        SubVoxelMeshData         mesh;
        mesher.mesh(hood, mesh);

        Result result;
        for (const Quad& quad : quadsOf(mesh)) {
            if (quad.direction() != Direction::PosX) {
                continue;
            }
            result.posXArea += quad.area();
            if (quad.corners[0].x == kBoundaryPlane) {
                result.boundaryArea += quad.area();
            }
        }
        return result;
    };

    const Result covered = build(blocks::Stone);
    const Result exposed = build(blocks::Air);

    // Either way the slot's own inner wall is emitted: the material at sub-voxel 2
    // faces the empty layer 3, which is a real surface and 64 sub-voxels of it.
    CHECK(covered.posXArea == 64u);
    // Intact stone across the face hides the boundary wall completely.
    CHECK(covered.boundaryArea == 0u);

    // With air there instead, the boundary wall appears - and it is exactly the
    // 64 faces the cull removed, which is what makes this a test of facesHidden
    // rather than of the slot geometry.
    CHECK(exposed.boundaryArea == 64u);
    CHECK(exposed.posXArea == covered.posXArea + 64u);
}

TEST_CASE("sub-voxel geometry is deterministic across runs", "[subvoxel][mesh]")
{
    // The store is a hash map and its iteration order is unspecified, so the
    // mesher walks sortedEntries(). Without that, two runs of an identical world
    // produce the same triangles in a different order and any checksum comparison
    // between two machines fails for no real reason.
    const auto build = [] {
        const ChunkPtr centre = makeChunk(kCentre, blocks::Stone);
        centre->setBlock(10, 21, 10, blocks::Air);
        // Several damaged blocks, deliberately not in index order.
        carve(centre, 20, 20, 20, subVoxelIndex(1, 1, 1));
        carve(centre, 5, 5, 5, subVoxelIndex(7, 0, 3));
        carve(centre, 10, 20, 10, subVoxelIndex(4, 7, 4));
        carve(centre, 5, 5, 5, subVoxelIndex(6, 0, 3));
        carve(centre, 31, 2, 0, subVoxelIndex(0, 0, 0));

        const ChunkNeighbourhood hood = surroundWith(centre, blocks::Stone);
        SubVoxelMesher           mesher{registry()};
        SubVoxelMeshData         mesh;
        mesher.mesh(hood, mesh);
        return mesh;
    };

    const SubVoxelMeshData first  = build();
    const SubVoxelMeshData second = build();

    REQUIRE_FALSE(first.empty());
    REQUIRE(first.vertices.size() == second.vertices.size());
    REQUIRE(first.indices.size() == second.indices.size());
    for (std::size_t i = 0; i < first.vertices.size(); ++i) {
        REQUIRE(first.vertices[i] == second.vertices[i]);
    }
    for (std::size_t i = 0; i < first.indices.size(); ++i) {
        REQUIRE(first.indices[i] == second.indices[i]);
    }
}

TEST_CASE("carved surfaces inherit the light of the block across the face",
          "[subvoxel][mesh]")
{
    const ChunkPtr centre = makeChunk(kCentre, blocks::Stone);

    // Darken the whole chunk, then light only the air block above the one that
    // gets carved. The upward-facing carved floor must pick that light up: reading
    // the parent block's own (dark, because it is solid) light instead is the
    // mistake that renders every carved surface black.
    centre->fillLight(0, 0);
    centre->setBlock(16, 17, 16, blocks::Air);
    centre->setLight(localIndex(16, 17, 16),
                     ChunkStorage::packLight(ChunkStorage::kMaxLightLevel, 0));

    for (std::int32_t sz = 0; sz < kSubVoxelResolution; ++sz) {
        for (std::int32_t sx = 0; sx < kSubVoxelResolution; ++sx) {
            carve(centre, 16, 16, 16, subVoxelIndex(sx, kSubVoxelResolution - 1, sz));
        }
    }

    const ChunkNeighbourhood hood = surroundWith(centre, blocks::Stone);

    SubVoxelMesher   mesher{registry()};
    SubVoxelMeshData mesh;
    REQUIRE(mesher.mesh(hood, mesh));

    bool sawLitFloor = false;
    for (const Quad& quad : quadsOf(mesh)) {
        if (quad.direction() != Direction::PosY) {
            continue;
        }
        sawLitFloor = true;
        CHECK(quad.corners[0].sunlight == ChunkStorage::kMaxLightLevel);
    }
    CHECK(sawLitFloor);
}

TEST_CASE("restoring the last sub-voxel returns the block to the no-geometry path",
          "[subvoxel][mesh]")
{
    const ChunkPtr centre = makeChunk(kCentre, blocks::Stone);
    centre->setBlock(16, 17, 16, blocks::Air);

    const std::size_t block = localIndex(16, 16, 16);
    const std::size_t sub   = subVoxelIndex(3, kSubVoxelResolution - 1, 3);

    REQUIRE(centre->breakSubVoxel(block, sub) == SubVoxelEdit::Modified);

    const ChunkNeighbourhood hood = surroundWith(centre, blocks::Stone);

    SubVoxelMesher   mesher{registry()};
    SubVoxelMeshData mesh;
    REQUIRE(mesher.mesh(hood, mesh));
    REQUIRE_FALSE(mesh.empty());

    // Putting it back must erase the store entry, which takes the chunk back to
    // the zero-cost path - and must leave no stale geometry behind, because the
    // renderer releases the sub-voxel buffers precisely when this returns empty.
    REQUIRE(centre->restoreSubVoxel(block, sub, blocks::Stone) == SubVoxelEdit::BlockRestored);
    CHECK_FALSE(centre->hasSubVoxelDamage());

    REQUIRE_FALSE(mesher.mesh(hood, mesh));
    CHECK(mesh.empty());
}
