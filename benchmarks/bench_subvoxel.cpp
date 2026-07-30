// Sub-voxel meshing as a function of how many blocks in a chunk are damaged.
//
// The number that matters is the SLOPE, not any single point: the sparse store
// exists so that an undamaged chunk costs nothing and a damaged one costs in
// proportion to the damage. A cost that does not fall to ~0 at N = 0, or that
// grows faster than linearly in N, means the sparse representation has stopped
// being sparse somewhere.

#include "Cases.hpp"
#include "Fixtures.hpp"

#include "core/Log.hpp"
#include "mesh/SubVoxelMesh.hpp"
#include "mesh/SubVoxelMesher.hpp"
#include "world/BlockAccess.hpp"
#include "world/Chunk.hpp"
#include "world/SubVoxel.hpp"
#include "world/VoxelTypes.hpp"

#include <cstddef>
#include <format>
#include <memory>
#include <string>
#include <vector>

namespace voxl::bench {

namespace {

constexpr const char* kGroup = "subvoxel";

/// Sub-voxels carved out of each damaged block: the 4x4x4 corner, 64 of 512.
///
/// The block must stay strictly between empty and full or it leaves the store
/// entirely (SubVoxel.hpp's invariant), and a pocket rather than a scatter is
/// what a pickaxe actually produces - it also gives the greedy pass inside the
/// 8x8x8 grid something to merge, which a random scatter would not.
constexpr std::int32_t kCarveEdge = 4;

struct DamageFixture {
    ChunkPtr                        centre;
    ChunkNeighbourhood              neighbourhood;
    std::unique_ptr<SubVoxelMesher> mesher;
    SubVoxelMeshData                out;
    std::size_t                     damagedBlocks = 0;
};

void carve(Chunk& chunk, std::size_t blockIndex)
{
    for (std::int32_t sy = 0; sy < kCarveEdge; ++sy) {
        for (std::int32_t sz = 0; sz < kCarveEdge; ++sz) {
            for (std::int32_t sx = 0; sx < kCarveEdge; ++sx) {
                const SubVoxelEdit edit = chunk.breakSubVoxel(blockIndex, subVoxelIndex(sx, sy, sz));
                VOXL_CHECK(edit != SubVoxelEdit::BlockRemoved,
                           "carve emptied a block; kCarveEdge must stay below the full grid");
            }
        }
    }
}

void addDamageCase(Runner& runner, std::size_t damagedBlocks)
{
    const std::string name = std::format("mesh_damage_{}", damagedBlocks);
    if (!runner.selected(kGroup, name)) {
        return;
    }

    auto fixture = std::make_shared<DamageFixture>();

    // At N = 0 there is no damaged block to divide by, and the thing being
    // measured is the early-out on an untouched chunk - a single call to which
    // is far below the clock's resolution, so the run repeats it and divides.
    const std::size_t repeats = damagedBlocks == 0 ? 1024u : 1u;

    Case testCase;
    testCase.group      = kGroup;
    testCase.name       = name;
    testCase.unit       = damagedBlocks == 0 ? "chunk" : "damaged block";
    testCase.opsPerRun  = damagedBlocks == 0 ? static_cast<double>(repeats)
                                             : static_cast<double>(damagedBlocks);
    testCase.sampleRuns = 21;
    testCase.setup      = [fixture, damagedBlocks](CaseContext& context) {
        fixture->mesher = std::make_unique<SubVoxelMesher>(registry());
        // Solid stone: every candidate block is damageable, and a damaged block
        // buried in solid material is the realistic case - the carved pocket's
        // own surfaces are what has to be meshed.
        fixture->centre = uniformChunk(ChunkPos{0, 4, 0}, blocks::Stone);

        const std::vector<std::size_t> targets = spreadSolidBlocks(*fixture->centre, damagedBlocks);
        for (const std::size_t blockIndex : targets) {
            carve(*fixture->centre, blockIndex);
        }
        fixture->damagedBlocks = fixture->centre->subVoxels().size();
        fixture->neighbourhood = surrounded(fixture->centre, blocks::Stone);

        fixture->mesher->mesh(fixture->neighbourhood, fixture->out);
        const SubVoxelMesher::Stats& stats = fixture->mesher->lastStats();
        const double faces = static_cast<double>(stats.facesEmitted);
        const double quads = static_cast<double>(stats.quadsEmitted);

        context.counter("store_entries", static_cast<double>(fixture->damagedBlocks), "blocks");
        context.counter("store_heap",
                            static_cast<double>(fixture->centre->subVoxels().memoryUsageBytes()),
                            "bytes");
        context.counter("sub_voxels_removed",
                            static_cast<double>(fixture->damagedBlocks) *
                                static_cast<double>(kCarveEdge * kCarveEdge * kCarveEdge),
                            "sub-voxels");
        context.counter("blocks_meshed", static_cast<double>(stats.blocksMeshed), "blocks");
        context.counter("faces_emitted", faces, "faces");
        context.counter("quads_emitted", quads, "quads");
        context.counter("merge_ratio", quads > 0.0 ? faces / quads : 0.0, "faces/quad");
        context.counter("triangles", static_cast<double>(fixture->out.triangleCount()), "tris");
        context.counter("mesh_bytes", static_cast<double>(fixture->out.byteSize()), "bytes");
    };
    testCase.body = [fixture, repeats](CaseContext&) {
        for (std::size_t i = 0; i < repeats; ++i) {
            fixture->mesher->mesh(fixture->neighbourhood, fixture->out);
        }
        keep(static_cast<std::uint64_t>(fixture->out.vertices.size()));
    };
    runner.add(std::move(testCase));
}

/// Cost of producing the damage, not of meshing it. Carving is a main-thread
/// edit that runs inside the frame, so its per-sub-voxel cost is a gameplay
/// budget question of its own.
void addCarveCase(Runner& runner)
{
    const std::string name = "carve_sub_voxels";
    if (!runner.selected(kGroup, name)) {
        return;
    }

    struct CarveFixture {
        ChunkPtr                 centre;
        std::vector<std::size_t> targets;
    };
    auto fixture = std::make_shared<CarveFixture>();

    constexpr std::size_t kCarveBlocks = 64;
    constexpr std::size_t kSubVoxelsPerBlock = kCarveEdge * kCarveEdge * kCarveEdge;

    Case testCase;
    testCase.group      = kGroup;
    testCase.name       = name;
    testCase.unit       = "sub-voxel break";
    testCase.opsPerRun  = static_cast<double>(kCarveBlocks * kSubVoxelsPerBlock);
    testCase.sampleRuns = 21;
    testCase.setup      = [fixture](CaseContext& context) {
        fixture->centre  = uniformChunk(ChunkPos{0, 4, 0}, blocks::Stone);
        fixture->targets = spreadSolidBlocks(*fixture->centre, kCarveBlocks);
        context.counter("blocks_damaged", static_cast<double>(fixture->targets.size()), "blocks");
        context.note("Chunk::breakSubVoxel, including the store insert that materialises "
                     "each block's grid on first damage");
    };
    // Each run must start from an undamaged chunk, otherwise the second run
    // measures re-clearing bits that are already clear.
    testCase.prepare = [fixture](CaseContext&) {
        fixture->centre->subVoxels().clear();
        fixture->centre->storage().fill(blocks::Stone);
    };
    testCase.body = [fixture](CaseContext&) {
        for (const std::size_t blockIndex : fixture->targets) {
            carve(*fixture->centre, blockIndex);
        }
        keep(static_cast<std::uint64_t>(fixture->centre->subVoxels().size()));
    };
    runner.add(std::move(testCase));
}

}  // namespace

void registerSubVoxelCases(Runner& runner)
{
    for (const std::size_t damaged : {std::size_t{0}, std::size_t{1}, std::size_t{4},
                                      std::size_t{16}, std::size_t{64}, std::size_t{256}}) {
        addDamageCase(runner, damaged);
    }
    addCarveCase(runner);
}

}  // namespace voxl::bench
