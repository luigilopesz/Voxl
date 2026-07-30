// Greedy meshing: four contrasting chunk shapes at full resolution, then the
// same terrain chunk at every LOD level.
//
// Every case meshes into a POOLED ChunkMeshData that keeps its capacity between
// runs, because that is what the engine does: one mesh buffer per worker, reused
// for chunk after chunk. Allocating a fresh output every run would measure the
// allocator's growth curve and report it as meshing cost.

#include "Cases.hpp"
#include "Fixtures.hpp"

#include "core/Log.hpp"
#include "mesh/GreedyMesher.hpp"
#include "mesh/MeshData.hpp"
#include "world/BlockAccess.hpp"
#include "world/Chunk.hpp"
#include "world/Lod.hpp"
#include "world/VoxelTypes.hpp"

#include <cstddef>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace voxl::bench {

namespace {

constexpr const char* kGroup = "meshing";

struct MeshFixture {
    ChunkNeighbourhood            neighbourhood;
    std::unique_ptr<GreedyMesher> mesher;
    ChunkMeshData                 out;
    /// Non-air blocks in the centre chunk, for the naive-face comparison.
    std::size_t solidBlocks = 0;
};

/// Records the scene's shape once, after a single untimed mesh, so the report
/// can put the timing next to what was actually built.
void recordSceneCounters(CaseContext& context, MeshFixture& fixture)
{
    fixture.mesher->mesh(fixture.neighbourhood, fixture.out);
    const GreedyMesher::Stats& stats = fixture.mesher->lastStats();

    const double naiveFaces = static_cast<double>(fixture.solidBlocks) * 6.0;
    const double faces      = static_cast<double>(stats.facesEmitted);
    const double quads      = static_cast<double>(stats.quadsEmitted);

    context.counter("solid_blocks", static_cast<double>(fixture.solidBlocks), "blocks");
    context.counter("naive_faces", naiveFaces, "faces");
    context.counter("faces_after_culling", faces, "faces");
    context.counter("quads_emitted", quads, "quads");
    context.counter("skirt_quads", static_cast<double>(stats.skirtQuads), "quads");
    // Two different reductions, and they answer different questions.
    // `merge_ratio` is what greedy merging alone bought on top of hidden-face
    // removal; `total_reduction` is what the whole mesher bought against a
    // mesher that emitted six faces per solid block.
    context.counter("merge_ratio", quads > 0.0 ? faces / quads : 0.0, "faces/quad");
    context.counter("total_reduction", quads > 0.0 ? naiveFaces / quads : 0.0, "naive/quad");
    context.counter("triangles", static_cast<double>(fixture.out.triangleCount()), "tris");
    context.counter("vertices", static_cast<double>(fixture.out.vertexCount()), "verts");
    context.counter("mesh_bytes", static_cast<double>(fixture.out.byteSize()), "bytes");
}

Case makeMeshCase(std::string name, std::function<void(MeshFixture&)> build)
{
    auto fixture = std::make_shared<MeshFixture>();

    Case testCase;
    testCase.group     = kGroup;
    testCase.name      = std::move(name);
    testCase.unit      = "chunk";
    testCase.opsPerRun = 1.0;
    testCase.sampleRuns = 21;
    testCase.setup = [fixture, build = std::move(build)](CaseContext& context) {
        fixture->mesher = std::make_unique<GreedyMesher>(registry());
        build(*fixture);
        recordSceneCounters(context, *fixture);
    };
    testCase.body = [fixture](CaseContext&) {
        fixture->mesher->mesh(fixture->neighbourhood, fixture->out);
        keep(static_cast<std::uint64_t>(fixture->out.vertexCount()));
    };
    return testCase;
}

void addPlainsCase(Runner& runner)
{
    const std::string name = "greedy_plains_surface";
    if (!runner.selected(kGroup, name)) {
        return;
    }
    const BiomeSite site = findBiomeSite(BiomeId::Plains);
    if (!site.found) {
        runner.addUnavailable(kGroup, name, "no plains column found for this seed");
        return;
    }
    runner.add(makeMeshCase(name, [site](MeshFixture& fixture) {
        fixture.neighbourhood = generatedNeighbourhood(site.surfaceSection, kLodFull);
        fixture.solidBlocks   = countNonAir(*fixture.neighbourhood.centre());
    }));
}

void addCavesCase(Runner& runner)
{
    const std::string name = "greedy_dense_caves";
    if (!runner.selected(kGroup, name)) {
        return;
    }
    runner.add(makeMeshCase(name, [](MeshFixture& fixture) {
        const CaveSite site = findDensestCaveSection();
        VOXL_CHECK(site.found, "cave section search returned nothing");
        fixture.neighbourhood = generatedNeighbourhood(site.position, kLodFull);
        fixture.solidBlocks   = countNonAir(*fixture.neighbourhood.centre());
    }));
}

void addSolidCase(Runner& runner)
{
    const std::string name = "greedy_solid_chunk";
    if (!runner.selected(kGroup, name)) {
        return;
    }
    runner.add(makeMeshCase(name, [](MeshFixture& fixture) {
        // Neighbours deliberately left unloaded, which reads as air: a stone
        // chunk buried in stone emits nothing at all and would time an empty
        // sweep. Against air it is greedy meshing's best case - 6144 faces
        // collapsing to six quads.
        const ChunkPtr centre = uniformChunk(ChunkPos{0, 4, 0}, blocks::Stone);
        fixture.neighbourhood = isolated(centre);
        fixture.solidBlocks   = kChunkVolume;
    }));
}

void addCheckerboardCase(Runner& runner)
{
    const std::string name = "greedy_checkerboard";
    if (!runner.selected(kGroup, name)) {
        return;
    }
    runner.add(makeMeshCase(name, [](MeshFixture& fixture) {
        const ChunkPtr centre = checkerboardChunk(ChunkPos{0, 4, 0});
        fixture.neighbourhood = isolated(centre);
        fixture.solidBlocks   = countNonAir(*centre);
    }));
}

void addLodCase(Runner& runner, LodLevel level)
{
    const std::string name = std::format("greedy_lod{}", static_cast<int>(level));
    if (!runner.selected(kGroup, name)) {
        return;
    }
    runner.add(makeMeshCase(name, [level](MeshFixture& fixture) {
        // The whole neighbourhood is generated at the same level. A centre chunk
        // whose neighbours are at a different resolution has its shared faces
        // loaded conservatively as air (see GreedyMesher::loadCache), which
        // changes the face count for a reason that has nothing to do with the
        // level being measured.
        fixture.neighbourhood = generatedNeighbourhood(ChunkPos{0, kSeaLevel / kChunkSize, 0}, level);
        fixture.solidBlocks   = countNonAir(*fixture.neighbourhood.centre());
    }));
}

/// A chunk sitting exactly on an LOD band edge: coarse centre, finer neighbours.
///
/// The greedy_lodN cases deliberately keep the whole neighbourhood at one level,
/// which is the interior of a band and emits NO skirt quads - GreedyMesher hangs
/// the curtain only where `levelDiffers`, because two neighbours at the same
/// level quantise onto the same global cell grid and have no seam to hide. That
/// makes those cases silent about the skirt path entirely. This one is the other
/// half: it is the configuration every chunk on a band boundary is in, it is the
/// only one that pays for skirts, and it also pays the conservative-face cost -
/// faces shared with a differently-levelled neighbour are loaded as air, so none
/// of them cull.
void addBandEdgeCase(Runner& runner, LodLevel centreLevel, LodLevel neighbourLevel)
{
    const std::string name = std::format("greedy_lod{}_beside_lod{}", static_cast<int>(centreLevel),
                                         static_cast<int>(neighbourLevel));
    if (!runner.selected(kGroup, name)) {
        return;
    }
    runner.add(makeMeshCase(name, [centreLevel, neighbourLevel](MeshFixture& fixture) {
        const ChunkPos centre{0, kSeaLevel / kChunkSize, 0};
        fixture.neighbourhood.setCentre(centre);
        for (std::int32_t dy = -1; dy <= 1; ++dy) {
            const std::int32_t sectionY = centre.y + dy;
            if (sectionY < 0 || sectionY >= kWorldSectionCount) {
                continue;
            }
            for (std::int32_t dz = -1; dz <= 1; ++dz) {
                for (std::int32_t dx = -1; dx <= 1; ++dx) {
                    const bool     isCentre = dx == 0 && dy == 0 && dz == 0;
                    const LodLevel level    = isCentre ? centreLevel : neighbourLevel;
                    fixture.neighbourhood.setChunk(
                        dx, dy, dz,
                        generateChunk(ChunkPos{centre.x + dx, sectionY, centre.z + dz}, level));
                }
            }
        }
        fixture.solidBlocks = countNonAir(*fixture.neighbourhood.centre());
    }));
}

}  // namespace

void registerMeshingCases(Runner& runner)
{
    addPlainsCase(runner);
    addCavesCase(runner);
    addSolidCase(runner);
    addCheckerboardCase(runner);
    for (LodLevel level = 0; level < kLodCount; ++level) {
        addLodCase(runner, level);
    }
    // The two band edges the shipped LodPolicy actually produces near the
    // player: 1 against 0 at five chunks out, 2 against 1 at nine.
    addBandEdgeCase(runner, 1, 0);
    addBandEdgeCase(runner, 2, 1);
}

void reportMeshingDerived(Runner& runner)
{
    const CaseResult* level0 = runner.find(kGroup, "greedy_lod0");
    if (level0 == nullptr) {
        return;
    }
    for (LodLevel level = 1; level < kLodCount; ++level) {
        const CaseResult* coarse =
            runner.find(kGroup, std::format("greedy_lod{}", static_cast<int>(level)));
        if (coarse == nullptr || coarse->stats.median <= 0.0) {
            continue;
        }
        const double ratio    = level0->stats.median / coarse->stats.median;
        const double expected = level == 3 ? 4.0 : (level == 2 ? 2.5 : 1.5);
        const std::string verdict =
            ratio >= expected ? "ok"
                              : std::format("*** FLAG: expected >= {:.1f}x cheaper than level 0",
                                            expected);
        runner.addDerived(Derived{std::string(kGroup),
                                  std::format("greedy_lod{}", static_cast<int>(level)),
                                  "speedup_vs_lod0", ratio, "x", verdict});
    }
}

}  // namespace voxl::bench
