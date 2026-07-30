// The mining verb: the break timer, the sub-voxel drill and the placement veto.
//
// BlockInteraction takes the world as four injected callables, which is what
// lets this file test the whole verb without a World, a JobSystem or a GL
// context. `FakeWorld` below is a flat map of blocks plus a record of every edit
// that was requested, so an assertion can be about the exact sequence of writes
// the player's input produced rather than about the state it happened to leave
// behind - the difference matters for the drill, where "carved the right
// sub-voxels" and "ended up with the right block" are not the same claim.
//
// Nothing here calls BlockInteraction::render(): it is the only member that
// touches OpenGL and the suite must run on a machine with no GPU.

#include <catch2/catch_test_macros.hpp>

#include "gameplay/BlockInteraction.hpp"
#include "gameplay/Hotbar.hpp"
#include "gameplay/MiningTool.hpp"
#include "physics/SubVoxelAccess.hpp"
#include "render/Camera.hpp"
#include "world/Block.hpp"
#include "world/SubVoxel.hpp"
#include "world/VoxelTypes.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <glm/vec3.hpp>

using voxl::Aabb;
using voxl::BlockId;
using voxl::BlockInteraction;
using voxl::BlockPos;
using voxl::BlockRegistry;
using voxl::BreakResult;
using voxl::Camera;
using voxl::CarveOutcome;
using voxl::Direction;
using voxl::Hotbar;
using voxl::InteractionHit;
using voxl::InteractionInput;
using voxl::kSubVoxelSize;
using voxl::makeBrushStencil;
using voxl::MiningMode;
using voxl::MiningTool;
using voxl::PlaceResult;

namespace {

/// The block the fixtures break by default. Stone's hardness of 1.5 gives a
/// whole-block break of 0.75 s, which is long enough that a handful of 0.1 s
/// steps land unambiguously on either side of it.
constexpr BlockId kTargetBlock = voxl::blocks::Stone;

constexpr float kStoneBreakSeconds = 1.5f * BlockInteraction::kBreakSecondsPerHardness;

/// A block face the ray can plausibly have entered through, with the hit point
/// placed at a known sub-voxel so the drill's targeting is deterministic.
struct Target {
    BlockPos  block{4, 70, 9};
    Direction face = Direction::PosY;
    /// Sub-voxel of `block` the hit point sits in.
    glm::ivec3 sub{3, 7, 5};
};

/// World stand-in: block storage, a damage set, and a log of every edit.
class FakeWorld {
public:
    explicit FakeWorld(const BlockRegistry& registry) : m_registry(&registry) {}

    void set(const BlockPos& pos, BlockId id) { m_blocks[pos] = id; }

    [[nodiscard]] BlockId get(const BlockPos& pos) const
    {
        const auto it = m_blocks.find(pos);
        return it == m_blocks.end() ? voxl::blocks::Air : it->second;
    }

    void markDamaged(const BlockPos& pos) { m_damaged.insert(pos); }
    [[nodiscard]] bool damaged(const BlockPos& pos) const { return m_damaged.count(pos) != 0; }

    /// Whole-block write, as BlockInteraction's BlockWriteFn sees it.
    bool write(const BlockPos& pos, BlockId id)
    {
        blockWrites.push_back({pos, id});
        m_blocks[pos] = id;
        return true;
    }

    /// Sub-voxel carve. Mirrors World::breakSubVoxelAt closely enough to matter:
    /// it refuses anything that is not RenderLayer::Opaque, which is the rule
    /// the fallback exists for.
    CarveOutcome carve(const glm::vec3& point)
    {
        const voxl::SubVoxelHit hit = voxl::toSubVoxel(point);
        const BlockId           id  = get(hit.block);
        if (id == voxl::blocks::Air ||
            m_registry->renderLayer(id) != voxl::RenderLayer::Opaque) {
            refusedCarves.push_back(hit);
            return CarveOutcome::Refused;
        }
        carves.push_back(hit);
        return CarveOutcome::Carved;
    }

    void wireInto(BlockInteraction& interaction)
    {
        interaction.setBlockWriter([this](const BlockPos& pos, BlockId id) {
            return write(pos, id);
        });
        interaction.setBlockReader([this](const BlockPos& pos) { return get(pos); });
        interaction.setSubVoxelBreaker(
            [this](const glm::vec3& point) { return carve(point); });
        interaction.setSubVoxelDamageReader(
            [this](const BlockPos& pos) { return damaged(pos); });
    }

    struct BlockWrite {
        BlockPos pos{};
        BlockId  id = voxl::blocks::Air;
    };

    std::vector<BlockWrite>       blockWrites;
    std::vector<voxl::SubVoxelHit> carves;
    std::vector<voxl::SubVoxelHit> refusedCarves;

private:
    const BlockRegistry*                    m_registry = nullptr;
    std::unordered_map<BlockPos, BlockId>   m_blocks;
    std::unordered_set<BlockPos>            m_damaged;
};

/// Installs a raycaster that always reports `target`, reading the block id back
/// out of the world so a break is visible to the next frame's targeting.
void aimAt(BlockInteraction& interaction, const FakeWorld& world, const Target& target)
{
    interaction.setRaycaster([&world, target](const glm::vec3&, const glm::vec3&, float,
                                              InteractionHit& out) {
        const BlockId id = world.get(target.block);
        if (id == voxl::blocks::Air) {
            return false;  // the block is gone; nothing to target
        }
        out.block   = target.block;
        out.blockId = id;
        out.face    = target.face;
        // A point inside the named sub-voxel, offset to its centre so no
        // rounding rule can move it.
        out.point = glm::vec3{static_cast<float>(target.block.x), static_cast<float>(target.block.y),
                              static_cast<float>(target.block.z)} +
                    (glm::vec3{static_cast<float>(target.sub.x), static_cast<float>(target.sub.y),
                               static_cast<float>(target.sub.z)} +
                     glm::vec3{0.5f}) *
                        kSubVoxelSize;
        out.distance = 2.0f;
        return true;
    });
}

/// Holds the break button for `seconds` in fixed steps, stopping early once the
/// world has recorded an edit of either kind. Returns the elapsed time.
float holdBreak(BlockInteraction& interaction, const Camera& camera, const FakeWorld& world,
                float seconds, float step = 0.02f)
{
    InteractionInput input{};
    input.breakHeld = true;

    float elapsed = 0.0f;
    while (elapsed < seconds) {
        interaction.update(camera, input, step);
        elapsed += step;
        if (!world.blockWrites.empty() || !world.carves.empty()) {
            break;
        }
    }
    return elapsed;
}

void releaseButtons(BlockInteraction& interaction, const Camera& camera, float step = 0.02f)
{
    interaction.update(camera, InteractionInput{}, step);
}

/// Global sub-voxel coordinates the drill actually carved, as a set, so the
/// order of the stencil sweep does not enter the assertion.
[[nodiscard]] std::unordered_set<std::int64_t> carvedKeys(const FakeWorld& world)
{
    std::unordered_set<std::int64_t> keys;
    for (const voxl::SubVoxelHit& hit : world.carves) {
        const voxl::physics::SubVoxelCoord cell =
            voxl::physics::toGlobalSubVoxel(hit.block, hit.sx, hit.sy, hit.sz);
        // 21 bits per axis is far more than the test's coordinates need and
        // makes the key collision-free by construction.
        keys.insert((static_cast<std::int64_t>(cell.x) << 42) ^
                    (static_cast<std::int64_t>(cell.y) << 21) ^ static_cast<std::int64_t>(cell.z));
    }
    return keys;
}

}  // namespace

// ---------------------------------------------------------------------------
//  Brush geometry
// ---------------------------------------------------------------------------

TEST_CASE("brush stencil is the euclidean ball of its radius", "[mining][brush]")
{
    // A radius below one sub-voxel still has to remove something, or the mode
    // looks broken at its default-adjacent setting.
    REQUIRE(makeBrushStencil(MiningTool::kMinBrushRadius).size() == 1);
    const glm::ivec3 only = makeBrushStencil(MiningTool::kMinBrushRadius).front();
    CHECK(only.x == 0);
    CHECK(only.y == 0);
    CHECK(only.z == 0);

    // radius 1: centre plus the six face neighbours.
    CHECK(makeBrushStencil(1.0f).size() == 7);
    // radius 1.5 adds the twelve edge diagonals (d^2 == 2).
    CHECK(makeBrushStencil(1.5f).size() == 19);
    // radius 2 adds the eight corners (d^2 == 3) and the six at distance 2.
    CHECK(makeBrushStencil(2.0f).size() == 33);

    // Every offset is inside the radius and every offset inside the radius is
    // present - the property, rather than another hand-counted number.
    for (float radius : {1.0f, 1.5f, 2.0f, 2.5f, 3.0f}) {
        const std::vector<glm::ivec3> stencil = makeBrushStencil(radius);
        const auto extent = static_cast<std::int32_t>(radius);
        std::size_t expected = 0;
        for (std::int32_t dy = -extent; dy <= extent; ++dy) {
            for (std::int32_t dz = -extent; dz <= extent; ++dz) {
                for (std::int32_t dx = -extent; dx <= extent; ++dx) {
                    if (static_cast<float>(dx * dx + dy * dy + dz * dz) <= radius * radius) {
                        ++expected;
                    }
                }
            }
        }
        CHECK(stencil.size() == expected);
        for (const glm::ivec3& offset : stencil) {
            const auto squared =
                static_cast<float>(offset.x * offset.x + offset.y * offset.y + offset.z * offset.z);
            CHECK(squared <= radius * radius + 1.0e-3f);
        }
    }
}

TEST_CASE("brush radius is clamped and reproducible under repeated adjustment", "[mining][brush]")
{
    MiningTool tool;
    CHECK(tool.brushRadius() == MiningTool::kDefaultBrushRadius);
    CHECK(tool.mode() == MiningMode::WholeBlock);

    for (int i = 0; i < 50; ++i) {
        tool.adjustBrushRadius(1);
    }
    CHECK(tool.brushRadius() == MiningTool::kMaxBrushRadius);
    const std::size_t atMax = tool.brushVolume();

    for (int i = 0; i < 50; ++i) {
        tool.adjustBrushRadius(-1);
    }
    CHECK(tool.brushRadius() == MiningTool::kMinBrushRadius);
    CHECK(tool.brushVolume() == 1);

    // Walking back up must land on exactly the same brush: the stencil is a
    // function of the radius, not of how the radius was reached.
    for (int i = 0; i < 50; ++i) {
        tool.adjustBrushRadius(1);
    }
    CHECK(tool.brushVolume() == atMax);
}

TEST_CASE("a carve is a fixed fraction of a whole-block break", "[mining][timing]")
{
    const float whole = voxl::miningActionSeconds(MiningMode::WholeBlock, 1.5f,
                                                  BlockInteraction::kBreakSecondsPerHardness);
    const float carve = voxl::miningActionSeconds(MiningMode::SubVoxel, 1.5f,
                                                  BlockInteraction::kBreakSecondsPerHardness);
    CHECK(whole == kStoneBreakSeconds);
    CHECK(carve == whole * MiningTool::kSubVoxelBreakFraction);
    CHECK(carve < whole);

    // Hardness zero is instant in both modes; that is what makes leaves and
    // tall grass feel like they are brushed aside rather than mined.
    CHECK(voxl::miningActionSeconds(MiningMode::WholeBlock, 0.0f, 0.5f) == 0.0f);
    CHECK(voxl::miningActionSeconds(MiningMode::SubVoxel, 0.0f, 0.5f) == 0.0f);
}

// ---------------------------------------------------------------------------
//  Break timer
// ---------------------------------------------------------------------------

TEST_CASE("hardness drives break time", "[mining][break]")
{
    const BlockRegistry registry = voxl::createDefaultBlockRegistry();
    Camera              camera;
    const Target        target;

    SECTION("stone is not broken before its hardness says so")
    {
        FakeWorld world{registry};
        world.set(target.block, voxl::blocks::Stone);

        BlockInteraction interaction{registry};
        world.wireInto(interaction);
        aimAt(interaction, world, target);

        // Comfortably short of 0.75 s.
        holdBreak(interaction, camera, world, kStoneBreakSeconds - 0.15f);
        CHECK(world.blockWrites.empty());
        CHECK(interaction.state().lastBreak == BreakResult::InProgress);
        CHECK(interaction.state().breakProgress > 0.5f);
        CHECK(interaction.state().breakProgress < 1.0f);

        // Carrying on past it does break the block.
        holdBreak(interaction, camera, world, 0.5f);
        REQUIRE(world.blockWrites.size() == 1);
        CHECK(world.blockWrites.front().pos == target.block);
        CHECK(world.blockWrites.front().id == voxl::blocks::Air);
        CHECK(interaction.state().lastBreak == BreakResult::Broken);
    }

    SECTION("a softer block breaks sooner than a harder one")
    {
        FakeWorld soft{registry};
        soft.set(target.block, voxl::blocks::Dirt);  // hardness 0.5
        BlockInteraction softInteraction{registry};
        soft.wireInto(softInteraction);
        aimAt(softInteraction, soft, target);
        const float softTime = holdBreak(softInteraction, camera, soft, 5.0f);

        FakeWorld hard{registry};
        hard.set(target.block, voxl::blocks::Wood);  // hardness 2.0
        BlockInteraction hardInteraction{registry};
        hard.wireInto(hardInteraction);
        aimAt(hardInteraction, hard, target);
        const float hardTime = holdBreak(hardInteraction, camera, hard, 5.0f);

        REQUIRE(soft.blockWrites.size() == 1);
        REQUIRE(hard.blockWrites.size() == 1);
        CHECK(softTime < hardTime);
    }
}

TEST_CASE("bedrock never breaks", "[mining][break]")
{
    const BlockRegistry registry = voxl::createDefaultBlockRegistry();
    Camera              camera;
    const Target        target;

    FakeWorld world{registry};
    world.set(target.block, voxl::blocks::Bedrock);

    BlockInteraction interaction{registry};
    world.wireInto(interaction);
    aimAt(interaction, world, target);

    InteractionInput input{};
    input.breakHeld = true;
    for (int frame = 0; frame < 2000; ++frame) {  // 40 seconds of held button
        interaction.update(camera, input, 0.02f);
    }

    CHECK(world.blockWrites.empty());
    CHECK(world.carves.empty());
    CHECK(interaction.state().targetUnbreakable);
    CHECK(interaction.state().lastBreak == BreakResult::Unbreakable);
    CHECK(interaction.state().breakProgress == 0.0f);

    // The sentinel must hold in the drill too, or the mode becomes a way around
    // the world's floor.
    interaction.setMiningMode(MiningMode::SubVoxel);
    for (int frame = 0; frame < 2000; ++frame) {
        interaction.update(camera, input, 0.02f);
    }
    CHECK(world.carves.empty());
    CHECK(world.blockWrites.empty());
}

TEST_CASE("releasing the button resets progress", "[mining][break]")
{
    const BlockRegistry registry = voxl::createDefaultBlockRegistry();
    Camera              camera;
    const Target        target;

    FakeWorld world{registry};
    world.set(target.block, kTargetBlock);

    BlockInteraction interaction{registry};
    world.wireInto(interaction);
    aimAt(interaction, world, target);

    InteractionInput held{};
    held.breakHeld = true;

    // Most of the way there.
    for (int frame = 0; frame < 30; ++frame) {
        interaction.update(camera, held, 0.02f);
    }
    REQUIRE(world.blockWrites.empty());
    REQUIRE(interaction.state().breakProgress > 0.7f);

    releaseButtons(interaction, camera);
    CHECK(interaction.state().breakProgress == 0.0f);
    CHECK(interaction.state().breakStage == -1);

    // The same partial hold must not finish the job the second time either.
    for (int frame = 0; frame < 30; ++frame) {
        interaction.update(camera, held, 0.02f);
    }
    CHECK(world.blockWrites.empty());
}

TEST_CASE("switching target resets progress", "[mining][break]")
{
    const BlockRegistry registry = voxl::createDefaultBlockRegistry();
    Camera              camera;

    const Target first;
    Target       second;
    second.block = BlockPos{first.block.x + 1, first.block.y, first.block.z};

    FakeWorld world{registry};
    world.set(first.block, kTargetBlock);
    world.set(second.block, kTargetBlock);

    BlockInteraction interaction{registry};
    world.wireInto(interaction);
    aimAt(interaction, world, first);

    InteractionInput held{};
    held.breakHeld = true;

    for (int frame = 0; frame < 30; ++frame) {
        interaction.update(camera, held, 0.02f);
    }
    REQUIRE(world.blockWrites.empty());

    // Look at the neighbour without ever letting go of the button.
    aimAt(interaction, world, second);
    interaction.update(camera, held, 0.02f);
    CHECK(interaction.state().breakProgress < 0.1f);

    for (int frame = 0; frame < 30; ++frame) {
        interaction.update(camera, held, 0.02f);
    }
    CHECK(world.blockWrites.empty());
}

TEST_CASE("in drill mode a different sub-voxel is a different target", "[mining][break]")
{
    const BlockRegistry registry = voxl::createDefaultBlockRegistry();
    Camera              camera;

    Target target;
    FakeWorld world{registry};
    world.set(target.block, kTargetBlock);

    BlockInteraction interaction{registry};
    world.wireInto(interaction);
    interaction.setMiningMode(MiningMode::SubVoxel);
    aimAt(interaction, world, target);

    InteractionInput held{};
    held.breakHeld = true;

    // A carve of stone costs 0.75 * 0.125 s; stop one step short of it.
    const float carveSeconds = voxl::miningActionSeconds(
        MiningMode::SubVoxel, 1.5f, BlockInteraction::kBreakSecondsPerHardness);
    const float step = carveSeconds * 0.4f;
    interaction.update(camera, held, step);
    interaction.update(camera, held, step);
    REQUIRE(world.carves.empty());
    REQUIRE(interaction.state().breakProgress > 0.0f);

    // Slide onto the neighbouring sub-voxel of the SAME block.
    target.sub.x += 1;
    aimAt(interaction, world, target);
    interaction.update(camera, held, 0.0f);
    CHECK(interaction.state().breakProgress == 0.0f);
    CHECK(world.carves.empty());
}

// ---------------------------------------------------------------------------
//  Sub-voxel mining
// ---------------------------------------------------------------------------

TEST_CASE("default mining mode still breaks whole blocks", "[mining][mode]")
{
    const BlockRegistry registry = voxl::createDefaultBlockRegistry();
    Camera              camera;
    const Target        target;

    FakeWorld world{registry};
    world.set(target.block, kTargetBlock);

    BlockInteraction interaction{registry};
    world.wireInto(interaction);
    aimAt(interaction, world, target);

    REQUIRE(interaction.miningMode() == MiningMode::WholeBlock);
    holdBreak(interaction, camera, world, 5.0f);

    CHECK(world.carves.empty());
    REQUIRE(world.blockWrites.size() == 1);
    CHECK(world.blockWrites.front().id == voxl::blocks::Air);

    CHECK(interaction.toggleMiningMode() == MiningMode::SubVoxel);
    CHECK(interaction.toggleMiningMode() == MiningMode::WholeBlock);
}

TEST_CASE("sub-voxel mode carves exactly the brush footprint", "[mining][subvoxel]")
{
    const BlockRegistry registry = voxl::createDefaultBlockRegistry();
    Camera              camera;

    // Deep inside the block so the whole brush stays in one block for the
    // radius-1 case and the expected set is unambiguous.
    Target target;
    target.sub = glm::ivec3{4, 4, 4};

    for (float radius : {MiningTool::kMinBrushRadius, 1.0f, 1.5f}) {
        FakeWorld world{registry};
        // A 3x3x3 neighbourhood of stone, so a brush that spills across a face
        // still lands in carvable material.
        for (std::int32_t dy = -1; dy <= 1; ++dy) {
            for (std::int32_t dz = -1; dz <= 1; ++dz) {
                for (std::int32_t dx = -1; dx <= 1; ++dx) {
                    world.set(target.block.offset(dx, dy, dz), kTargetBlock);
                }
            }
        }

        BlockInteraction interaction{registry};
        world.wireInto(interaction);
        interaction.setMiningMode(MiningMode::SubVoxel);
        interaction.setBrushRadius(radius);
        aimAt(interaction, world, target);

        REQUIRE(interaction.brushRadius() == radius);
        holdBreak(interaction, camera, world, 5.0f);

        CHECK(world.blockWrites.empty());
        CHECK(interaction.state().lastBreak == BreakResult::Carved);

        const std::vector<glm::ivec3> stencil = makeBrushStencil(radius);
        CHECK(world.carves.size() == stencil.size());

        const voxl::physics::SubVoxelCoord centre =
            voxl::physics::toGlobalSubVoxel(target.block, target.sub);
        const std::unordered_set<std::int64_t> actual = carvedKeys(world);
        CHECK(actual.size() == stencil.size());
        for (const glm::ivec3& offset : stencil) {
            const voxl::physics::SubVoxelCoord cell =
                centre.offset(offset.x, offset.y, offset.z);
            const auto key = (static_cast<std::int64_t>(cell.x) << 42) ^
                             (static_cast<std::int64_t>(cell.y) << 21) ^
                             static_cast<std::int64_t>(cell.z);
            CHECK(actual.count(key) == 1);
        }
    }
}

TEST_CASE("a brush at a block face spills into the neighbour", "[mining][subvoxel]")
{
    const BlockRegistry registry = voxl::createDefaultBlockRegistry();
    Camera              camera;

    // Sub-voxel 7 on Y is the top layer, so a radius-1 brush reaches one cell
    // into the block above. Crossing a boundary is what makes a bore a tunnel
    // rather than 512 independent holes.
    Target target;
    target.sub = glm::ivec3{4, 7, 4};

    FakeWorld world{registry};
    world.set(target.block, kTargetBlock);
    world.set(target.block.offset(0, 1, 0), kTargetBlock);

    BlockInteraction interaction{registry};
    world.wireInto(interaction);
    interaction.setMiningMode(MiningMode::SubVoxel);
    interaction.setBrushRadius(1.0f);
    aimAt(interaction, world, target);

    holdBreak(interaction, camera, world, 5.0f);
    REQUIRE(world.carves.size() == 7);

    const auto above = std::count_if(world.carves.begin(), world.carves.end(),
                                     [&](const voxl::SubVoxelHit& hit) {
                                         return hit.block == target.block.offset(0, 1, 0);
                                     });
    CHECK(above == 1);
    CHECK(world.carves.front().block == target.block);
}

TEST_CASE("a carve refused for a non-opaque material falls back to a whole-block break",
          "[mining][subvoxel]")
{
    const BlockRegistry registry = voxl::createDefaultBlockRegistry();
    Camera              camera;
    const Target        target;

    FakeWorld world{registry};
    // Glass is RenderLayer::Translucent and a perfectly valid raycast target,
    // and World::editSubVoxel refuses it: the sub-voxel pass has no alpha
    // cutoff, so a chipped pane would come back solid.
    world.set(target.block, voxl::blocks::Glass);

    BlockInteraction interaction{registry};
    world.wireInto(interaction);
    interaction.setMiningMode(MiningMode::SubVoxel);
    aimAt(interaction, world, target);

    InteractionInput held{};
    held.breakHeld = true;
    interaction.update(camera, held, 0.0f);
    // The fallback is decided from the material, before any edit is attempted -
    // a brush of refusals would be a wasted round trip through the world's
    // deferral queue.
    CHECK(interaction.state().subVoxelFallback);
    CHECK(world.refusedCarves.empty());

    holdBreak(interaction, camera, world, 5.0f);

    CHECK(world.carves.empty());
    REQUIRE(world.blockWrites.size() == 1);
    CHECK(world.blockWrites.front().pos == target.block);
    CHECK(world.blockWrites.front().id == voxl::blocks::Air);
    CHECK(interaction.state().lastBreak == BreakResult::Broken);
}

TEST_CASE("the fallback is timed as the whole-block break it is", "[mining][subvoxel]")
{
    const BlockRegistry registry = voxl::createDefaultBlockRegistry();
    Camera              camera;
    const Target        target;

    FakeWorld world{registry};
    world.set(target.block, voxl::blocks::Glass);  // hardness 0.3

    BlockInteraction interaction{registry};
    world.wireInto(interaction);
    interaction.setMiningMode(MiningMode::SubVoxel);
    aimAt(interaction, world, target);

    const float wholeSeconds = voxl::miningActionSeconds(
        MiningMode::WholeBlock, 0.3f, BlockInteraction::kBreakSecondsPerHardness);
    const float carveSeconds = voxl::miningActionSeconds(
        MiningMode::SubVoxel, 0.3f, BlockInteraction::kBreakSecondsPerHardness);

    // A carve-length hold must not be enough: if the fallback used the short
    // timer, glass would shatter almost instantly in drill mode.
    const float elapsed = holdBreak(interaction, camera, world, wholeSeconds - 0.02f, 0.005f);
    CHECK(elapsed > carveSeconds);
    CHECK(world.blockWrites.empty());
}

TEST_CASE("holding the button drills continuously", "[mining][subvoxel]")
{
    const BlockRegistry registry = voxl::createDefaultBlockRegistry();
    Camera              camera;

    Target target;
    target.sub = glm::ivec3{4, 4, 4};

    FakeWorld world{registry};
    world.set(target.block, kTargetBlock);

    BlockInteraction interaction{registry};
    world.wireInto(interaction);
    interaction.setMiningMode(MiningMode::SubVoxel);
    interaction.setBrushRadius(MiningTool::kMinBrushRadius);  // one sub-voxel per swing
    aimAt(interaction, world, target);

    InteractionInput held{};
    held.breakHeld = true;

    const float carveSeconds = voxl::miningActionSeconds(
        MiningMode::SubVoxel, 1.5f, BlockInteraction::kBreakSecondsPerHardness);
    // Five swings' worth of held button, in steps well short of one swing.
    const int steps = static_cast<int>(5.0f * carveSeconds / (carveSeconds * 0.25f)) + 5;
    for (int i = 0; i < steps; ++i) {
        interaction.update(camera, held, carveSeconds * 0.25f);
    }

    // The timer restarting after each swing is what turns a series of discrete
    // clicks into a drill; without it this would have carved exactly once.
    CHECK(world.carves.size() >= 4);
    CHECK(world.blockWrites.empty());
}

TEST_CASE("with no carve hook the drill degrades to whole-block breaking", "[mining][subvoxel]")
{
    const BlockRegistry registry = voxl::createDefaultBlockRegistry();
    Camera              camera;
    const Target        target;

    FakeWorld world{registry};
    world.set(target.block, kTargetBlock);

    BlockInteraction interaction{registry};
    // Deliberately no setSubVoxelBreaker: the graceful-degradation path a build
    // that has not wired the world up yet takes.
    interaction.setBlockWriter(
        [&world](const BlockPos& pos, BlockId id) { return world.write(pos, id); });
    interaction.setBlockReader([&world](const BlockPos& pos) { return world.get(pos); });
    interaction.setMiningMode(MiningMode::SubVoxel);
    aimAt(interaction, world, target);

    holdBreak(interaction, camera, world, 5.0f);
    CHECK(world.carves.empty());
    CHECK(world.blockWrites.size() == 1);
}

// ---------------------------------------------------------------------------
//  Placement
// ---------------------------------------------------------------------------

TEST_CASE("placement is refused inside the player", "[mining][place]")
{
    const BlockRegistry registry = voxl::createDefaultBlockRegistry();
    Camera              camera;

    Target target;
    target.face = Direction::PosY;  // the candidate cell is one block above

    FakeWorld world{registry};
    world.set(target.block, kTargetBlock);

    BlockInteraction interaction{registry};
    world.wireInto(interaction);
    aimAt(interaction, world, target);
    interaction.setHeldBlock(voxl::blocks::Planks);

    const BlockPos candidate = voxl::neighbour(target.block, target.face);

    InteractionInput place{};
    place.placePressed = true;

    SECTION("standing clear, the placement lands")
    {
        interaction.clearPlayerAabb();
        interaction.update(camera, place, 0.02f);
        CHECK(interaction.state().lastPlace == PlaceResult::Placed);
        REQUIRE(world.blockWrites.size() == 1);
        CHECK(world.blockWrites.front().pos == candidate);
        CHECK(world.blockWrites.front().id == voxl::blocks::Planks);
    }

    SECTION("standing in the cell, it is refused")
    {
        // A player box straddling the candidate cell, feet on the targeted
        // block's top face.
        const glm::vec3 feet{static_cast<float>(candidate.x) + 0.5f,
                             static_cast<float>(candidate.y),
                             static_cast<float>(candidate.z) + 0.5f};
        interaction.setPlayerAabb(Aabb{feet - glm::vec3{0.3f, 0.0f, 0.3f},
                                       feet + glm::vec3{0.3f, 1.8f, 0.3f}});

        interaction.update(camera, place, 0.02f);
        CHECK(interaction.state().lastPlace == PlaceResult::IntersectsPlayer);
        CHECK(interaction.state().placeAllowed == false);
        CHECK(world.blockWrites.empty());
    }

    SECTION("merely touching the cell's floor is not intersecting it")
    {
        // Feet exactly on the candidate's ceiling: touching, not overlapping.
        // Losing this distinction makes it impossible to build a floor while
        // standing on it.
        const glm::vec3 feet{static_cast<float>(candidate.x) + 0.5f,
                             static_cast<float>(candidate.y) + 1.0f,
                             static_cast<float>(candidate.z) + 0.5f};
        interaction.setPlayerAabb(Aabb{feet - glm::vec3{0.3f, 0.0f, 0.3f},
                                       feet + glm::vec3{0.3f, 1.8f, 0.3f}});

        interaction.update(camera, place, 0.02f);
        CHECK(interaction.state().lastPlace == PlaceResult::Placed);
    }
}

TEST_CASE("placement will not overwrite a partially destroyed block", "[mining][place]")
{
    const BlockRegistry registry = voxl::createDefaultBlockRegistry();
    Camera              camera;

    const Target   target;
    const BlockPos candidate = voxl::neighbour(target.block, target.face);

    FakeWorld world{registry};
    world.set(target.block, kTargetBlock);
    world.set(candidate, kTargetBlock);
    world.markDamaged(candidate);

    BlockInteraction interaction{registry};
    world.wireInto(interaction);
    aimAt(interaction, world, target);
    interaction.setHeldBlock(voxl::blocks::Planks);
    interaction.clearPlayerAabb();

    InteractionInput place{};
    place.placePressed = true;
    interaction.update(camera, place, 0.02f);

    // Damaged, not merely Occupied: writing a block over a damaged one discards
    // its sub-voxel grid, so the refusal is protecting the player's work rather
    // than just reporting a full cell.
    CHECK(interaction.state().lastPlace == PlaceResult::Damaged);
    CHECK(world.blockWrites.empty());
}

// ---------------------------------------------------------------------------
//  Hotbar
// ---------------------------------------------------------------------------

TEST_CASE("hotbar keeps its existing behaviour", "[mining][hotbar]")
{
    Hotbar bar;
    CHECK(bar.selectedIndex() == 0);
    CHECK(bar.selectedBlock() == voxl::blocks::Stone);

    bar.cycle(-1);
    CHECK(bar.selectedIndex() == 1);
    bar.cycle(1);
    CHECK(bar.selectedIndex() == 0);
    bar.cycle(1);  // wraps backwards off the start
    CHECK(bar.selectedIndex() == Hotbar::kSlotCount - 1);

    CHECK(bar.selectFromDigit(3));
    CHECK(bar.selectedIndex() == 2);
    CHECK_FALSE(bar.selectFromDigit(0));
    CHECK_FALSE(bar.selectFromDigit(10));
    CHECK(bar.selectedIndex() == 2);
}

TEST_CASE("cycle-locking the hotbar releases the wheel but not the number row",
          "[mining][hotbar]")
{
    Hotbar bar;
    bar.select(4);
    bar.setCycleLocked(true);
    CHECK(bar.cycleLocked());

    bar.cycle(3);
    bar.cycle(-3);
    CHECK(bar.selectedIndex() == 4);

    // Locking the wheel must never lock the player out of their blocks.
    CHECK(bar.selectFromDigit(1));
    CHECK(bar.selectedIndex() == 0);
    bar.select(6);
    CHECK(bar.selectedIndex() == 6);

    bar.setCycleLocked(false);
    bar.cycle(-1);
    CHECK(bar.selectedIndex() == 7);
}
