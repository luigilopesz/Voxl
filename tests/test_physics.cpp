#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "gameplay/Player.hpp"
#include "physics/Aabb.hpp"
#include "physics/Collision.hpp"
#include "physics/Raycast.hpp"
#include "world/Block.hpp"
#include "world/BlockAccess.hpp"
#include "world/VoxelTypes.hpp"

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

using Catch::Approx;
using namespace voxl;

/// Short local name for the physics box. It cannot simply be pulled in with a
/// using-declaration: render/Camera.hpp also declares a voxl::Aabb, so the
/// unqualified name is ambiguous in any file that sees both.
using Box = voxl::physics::Aabb;

namespace {

/// Sparse, hand-authored world. Implementing BlockAccess directly rather than
/// building Chunks keeps these tests independent of chunk storage and lets a
/// scenario be described in two lines.
class TestWorld final : public BlockAccess {
public:
    void set(std::int32_t x, std::int32_t y, std::int32_t z, BlockId id)
    {
        m_blocks[BlockPos{x, y, z}] = id;
    }

    /// Inclusive box fill.
    void fill(std::int32_t x0, std::int32_t y0, std::int32_t z0, std::int32_t x1, std::int32_t y1,
              std::int32_t z1, BlockId id)
    {
        for (std::int32_t y = y0; y <= y1; ++y) {
            for (std::int32_t z = z0; z <= z1; ++z) {
                for (std::int32_t x = x0; x <= x1; ++x) {
                    set(x, y, z, id);
                }
            }
        }
    }

    [[nodiscard]] BlockId getBlock(const BlockPos& pos) const noexcept override
    {
        if (pos.y < kWorldMinY) {
            return kBelowWorldBlock;
        }
        if (pos.y > kWorldMaxY) {
            return kAboveWorldBlock;
        }
        const auto it = m_blocks.find(pos);
        return it == m_blocks.end() ? blocks::Air : it->second;
    }

    [[nodiscard]] std::uint8_t getLight(const BlockPos&) const noexcept override { return 0; }

private:
    std::unordered_map<BlockPos, BlockId> m_blocks;
};

const BlockRegistry& registry()
{
    static const BlockRegistry instance = createDefaultBlockRegistry();
    return instance;
}

constexpr float kPlayerWidth  = 0.6f;
constexpr float kPlayerHeight = 1.8f;

/// Ground plane whose top surface is at y == 101, one block thick.
constexpr std::int32_t kFloorBlockY = 100;
constexpr float        kFloorTop    = 101.0f;

Box playerBox(float x, float feetY, float z)
{
    return Box::fromFeet(glm::vec3{x, feetY, z}, kPlayerWidth, kPlayerHeight);
}

}  // namespace

// ============================================================== Box basics ==

TEST_CASE("exactly touching boxes do not intersect", "[physics][aabb]")
{
    const Box unit{glm::vec3{0.0f}, glm::vec3{1.0f}};

    SECTION("face contact")
    {
        const Box neighbour{glm::vec3{1.0f, 0.0f, 0.0f}, glm::vec3{2.0f, 1.0f, 1.0f}};
        CHECK_FALSE(unit.intersects(neighbour));
        CHECK_FALSE(neighbour.intersects(unit));
        CHECK(unit.touchesOrIntersects(neighbour));
    }

    SECTION("corner contact")
    {
        const Box diagonal{glm::vec3{1.0f}, glm::vec3{2.0f}};
        CHECK_FALSE(unit.intersects(diagonal));
        CHECK(unit.touchesOrIntersects(diagonal));
    }

    SECTION("a hair of overlap does intersect")
    {
        const Box overlapping{glm::vec3{0.999f, 0.0f, 0.0f}, glm::vec3{2.0f, 1.0f, 1.0f}};
        CHECK(unit.intersects(overlapping));
    }

    SECTION("a hair of separation does not")
    {
        const Box separated{glm::vec3{1.001f, 0.0f, 0.0f}, glm::vec3{2.0f, 1.0f, 1.0f}};
        CHECK_FALSE(unit.intersects(separated));
    }

    SECTION("degenerate boxes never intersect")
    {
        const Box flat{glm::vec3{0.0f}, glm::vec3{1.0f, 0.0f, 1.0f}};
        CHECK_FALSE(flat.valid());
        CHECK_FALSE(flat.intersects(unit));
    }
}

TEST_CASE("aabb construction helpers", "[physics][aabb]")
{
    const Box block = Box::fromBlock(BlockPos{-3, 100, 7});
    CHECK(block.min == glm::vec3{-3.0f, 100.0f, 7.0f});
    CHECK(block.max == glm::vec3{-2.0f, 101.0f, 8.0f});
    CHECK(block.volume() == Approx(1.0f));

    const Box feet = Box::fromFeet(glm::vec3{0.5f, 100.0f, 0.5f}, 0.6f, 1.8f);
    CHECK(feet.min.x == Approx(0.2f));
    CHECK(feet.max.x == Approx(0.8f));
    CHECK(feet.min.y == Approx(100.0f));
    CHECK(feet.max.y == Approx(101.8f));

    CHECK(feet.expanded(1.0f).contains(feet));
    CHECK(feet.contains(feet.shrunk(0.01f)));

    // Shrinking past degenerate collapses to the centre instead of inverting.
    const Box collapsed = feet.shrunk(10.0f);
    CHECK(collapsed.min.x <= collapsed.max.x);
    CHECK_FALSE(collapsed.valid());

    const Box hull = feet.sweptHull(glm::vec3{2.0f, -3.0f, 0.0f});
    CHECK(hull.max.x == Approx(2.8f));
    CHECK(hull.min.y == Approx(97.0f));
}

TEST_CASE("swept box against a static box", "[physics][aabb][sweep]")
{
    const Box mover{glm::vec3{0.0f}, glm::vec3{1.0f}};
    const Box target{glm::vec3{3.0f, 0.0f, 0.0f}, glm::vec3{4.0f, 1.0f, 1.0f}};

    SECTION("head-on contact reports time, axis and normal")
    {
        const physics::SweepResult hit = physics::sweep(mover, glm::vec3{5.0f, 0.0f, 0.0f}, target);
        REQUIRE(hit.hit);
        CHECK(hit.axis == physics::kAxisX);
        CHECK(hit.time == Approx(0.4f));  // 2 blocks of gap over a 5 block move
        CHECK(hit.normalSign == Approx(-1.0f));
        CHECK(hit.normal() == glm::vec3{-1.0f, 0.0f, 0.0f});
    }

    SECTION("stopping short is a miss")
    {
        CHECK_FALSE(physics::sweep(mover, glm::vec3{1.9f, 0.0f, 0.0f}, target).hit);
    }

    SECTION("flush and closing contacts at time zero")
    {
        const Box flush{glm::vec3{2.0f, 0.0f, 0.0f}, glm::vec3{3.0f, 1.0f, 1.0f}};
        const physics::SweepResult hit = physics::sweep(flush, glm::vec3{1.0f, 0.0f, 0.0f}, target);
        REQUIRE(hit.hit);
        CHECK(hit.time == Approx(0.0f));
    }

    SECTION("flush and separating is a miss")
    {
        const Box flush{glm::vec3{2.0f, 0.0f, 0.0f}, glm::vec3{3.0f, 1.0f, 1.0f}};
        CHECK_FALSE(physics::sweep(flush, glm::vec3{-1.0f, 0.0f, 0.0f}, target).hit);
    }

    SECTION("passing alongside is a miss")
    {
        const Box offset{glm::vec3{0.0f, 2.0f, 0.0f}, glm::vec3{1.0f, 3.0f, 1.0f}};
        CHECK_FALSE(physics::sweep(offset, glm::vec3{5.0f, 0.0f, 0.0f}, target).hit);
    }

    SECTION("sliding along a flush face is a miss")
    {
        const Box flush{glm::vec3{2.0f, 0.0f, 0.0f}, glm::vec3{3.0f, 1.0f, 1.0f}};
        CHECK_FALSE(physics::sweep(flush, glm::vec3{0.0f, 0.0f, 5.0f}, target).hit);
    }

    SECTION("single-axis helpers agree with the full sweep")
    {
        CHECK(mover.sweptHullOnAxis(physics::kAxisX, 5.0f).max.x == Approx(6.0f));
        CHECK(mover.sweptHullOnAxis(physics::kAxisX, -5.0f).min.x == Approx(-5.0f));
        CHECK(mover.sweptHullOnAxis(physics::kAxisX, -5.0f).max.x == Approx(1.0f));

        // 2 blocks of clearance, whatever the request.
        CHECK(physics::axisGap(mover, target, physics::kAxisX, 5.0f) == Approx(2.0f));
        CHECK(physics::axisGap(mover, target, physics::kAxisX, 1.5f) == Approx(1.5f));
        // Moving away from the target is unobstructed.
        CHECK(physics::axisGap(mover, target, physics::kAxisX, -5.0f) == Approx(5.0f));
        // No overlap on a static axis means the target is irrelevant.
        const Box offset{glm::vec3{0.0f, 2.0f, 0.0f}, glm::vec3{1.0f, 3.0f, 1.0f}};
        CHECK(physics::axisGap(offset, target, physics::kAxisX, 5.0f) == Approx(5.0f));
        // Pre-existing penetration clamps to zero rather than pushing out.
        const Box inside{glm::vec3{3.5f, 0.0f, 0.0f}, glm::vec3{4.5f, 1.0f, 1.0f}};
        CHECK(physics::axisGap(inside, target, physics::kAxisX, -5.0f) == Approx(0.0f));
    }
}

// ========================================================== gravity + floor ==

TEST_CASE("a body at terminal velocity lands on a one-block floor", "[physics][collision]")
{
    TestWorld world;
    world.fill(-8, kFloorBlockY, -8, 8, kFloorBlockY, 8, blocks::Stone);
    const physics::VoxelCollider collider{world, registry()};
    const physics::MotionParams  params{};

    SECTION("a single step covering 39 blocks does not tunnel")
    {
        // One step, 39 blocks of travel, ending 5 blocks below a floor that is
        // one block thick. A move-then-test resolver falls straight through here.
        const Box start = playerBox(0.5f, 135.0f, 0.5f);
        const physics::MoveResult result =
            physics::moveAabb(collider, start, glm::vec3{0.0f, -params.terminalVelocity, 0.0f},
                              0.5f, params, false);

        CHECK(result.onGround);
        CHECK(result.velocity.y == Approx(0.0f));
        CHECK(result.box.min.y >= kFloorTop);
        CHECK(result.box.min.y == Approx(kFloorTop + params.skin).margin(1.0e-4f));
        CHECK_FALSE(collider.overlapsSolid(result.box));
    }

    SECTION("a long free fall never dips below the floor at any step")
    {
        const float dt = 1.0f / 60.0f;
        Box        box = playerBox(0.5f, 200.0f, 0.5f);
        glm::vec3   velocity{0.0f};
        bool        onGround     = false;
        float       lowestSeen   = box.min.y;
        bool        reachedTerminal = false;

        for (int step = 0; step < 600; ++step) {
            velocity.y = physics::applyGravity(velocity.y, dt, params, physics::FluidState{});
            const physics::MoveResult result =
                physics::moveAabb(collider, box, velocity, dt, params, onGround);
            box      = result.box;
            velocity = result.velocity;
            onGround = result.onGround;
            lowestSeen = std::min(lowestSeen, box.min.y);
            reachedTerminal = reachedTerminal ||
                              velocity.y <= -params.terminalVelocity + 0.01f;
            REQUIRE_FALSE(collider.overlapsSolid(box));
        }

        CHECK(reachedTerminal);
        CHECK(onGround);
        CHECK(lowestSeen >= kFloorTop);
        CHECK(box.min.y == Approx(kFloorTop + params.skin).margin(1.0e-4f));
    }

    SECTION("terminal velocity caps the fall")
    {
        float velocity = 0.0f;
        for (int step = 0; step < 10000; ++step) {
            velocity = physics::applyGravity(velocity, 1.0f / 60.0f, params, physics::FluidState{});
        }
        CHECK(velocity == Approx(-params.terminalVelocity));
    }
}

TEST_CASE("resting contact is stable and does not count as penetration",
          "[physics][collision]")
{
    TestWorld world;
    world.fill(-4, kFloorBlockY, -4, 4, kFloorBlockY, 4, blocks::Stone);
    const physics::VoxelCollider collider{world, registry()};

    // Exactly flush with the floor: touching, not penetrating.
    const Box resting = playerBox(0.5f, kFloorTop, 0.5f);
    CHECK_FALSE(collider.overlapsSolid(resting));
    CHECK(physics::isSupported(collider, resting));

    // Gravity applied to a resting body must not move it.
    const physics::MotionParams params{};
    Box                        box = resting;
    for (int step = 0; step < 120; ++step) {
        const physics::MoveResult result =
            physics::moveAabb(collider, box, glm::vec3{0.0f, -params.gravity, 0.0f}, 1.0f / 60.0f,
                              params, true);
        box = result.box;
        REQUIRE(result.onGround);
    }
    CHECK(box.min.y == Approx(kFloorTop).margin(2.0e-3f));

    // Walking across flat ground must not snag on the block seams.
    box = resting;
    bool onGround = true;
    for (int step = 0; step < 60; ++step) {
        const physics::MoveResult result = physics::moveAabb(
            collider, box, glm::vec3{4.0f, -1.0f, 0.0f}, 1.0f / 60.0f, params, onGround);
        box      = result.box;
        onGround = result.onGround;
        REQUIRE(onGround);
        REQUIRE_FALSE(result.hitWallX);
    }
    CHECK(box.min.x == Approx(0.2f + 4.0f).margin(0.05f));
}

// ================================================================= step-up ==

TEST_CASE("step-up clears a one-block ledge but not a two-block wall",
          "[physics][collision][stepup]")
{
    const physics::MotionParams params{};
    const float                 dt = 1.0f / 60.0f;

    auto walkEast = [&](const physics::VoxelCollider& collider, Box box, int steps) {
        bool onGround = true;
        for (int step = 0; step < steps; ++step) {
            glm::vec3 velocity{4.0f, -params.gravity * dt, 0.0f};
            const physics::MoveResult result =
                physics::moveAabb(collider, box, velocity, dt, params, onGround);
            box      = result.box;
            onGround = result.onGround;
        }
        return box;
    };

    SECTION("one-block ledge is walked onto without jumping")
    {
        TestWorld world;
        world.fill(-6, kFloorBlockY, -4, 8, kFloorBlockY, 4, blocks::Stone);
        // Ledge one block high starting at x == 2; its top is y == 102.
        world.fill(2, kFloorBlockY + 1, -4, 8, kFloorBlockY + 1, 4, blocks::Stone);
        const physics::VoxelCollider collider{world, registry()};

        const Box start  = playerBox(0.5f, kFloorTop + params.skin, 0.5f);
        const Box ending = walkEast(collider, start, 90);

        CHECK(ending.min.y == Approx(kFloorTop + 1.0f).margin(3.0e-3f));
        CHECK(ending.min.x > 2.0f);
        CHECK_FALSE(collider.overlapsSolid(ending));
    }

    SECTION("two-block wall stops the walk cold")
    {
        TestWorld world;
        world.fill(-6, kFloorBlockY, -4, 8, kFloorBlockY, 4, blocks::Stone);
        world.fill(2, kFloorBlockY + 1, -4, 8, kFloorBlockY + 2, 4, blocks::Stone);
        const physics::VoxelCollider collider{world, registry()};

        const Box start  = playerBox(0.5f, kFloorTop + params.skin, 0.5f);
        const Box ending = walkEast(collider, start, 90);

        // Never left the ground level, and stopped flush against x == 2.
        CHECK(ending.min.y == Approx(kFloorTop + params.skin).margin(3.0e-3f));
        CHECK(ending.max.x == Approx(2.0f).margin(2.0e-3f));
        CHECK(ending.max.x <= 2.0f);
        CHECK_FALSE(collider.overlapsSolid(ending));
    }

    SECTION("a single move reports the step it took")
    {
        TestWorld world;
        world.fill(-6, kFloorBlockY, -4, 8, kFloorBlockY, 4, blocks::Stone);
        world.fill(2, kFloorBlockY + 1, -4, 8, kFloorBlockY + 1, 4, blocks::Stone);
        const physics::VoxelCollider collider{world, registry()};

        // Placed so the very next step runs into the ledge: the box front face
        // is at x == 1.99 and one step of travel is 4/60 blocks.
        const Box start = playerBox(1.69f, kFloorTop + params.skin, 0.5f);
        const physics::MoveResult result =
            physics::moveAabb(collider, start, glm::vec3{4.0f, 0.0f, 0.0f}, dt, params, true);

        CHECK(result.steppedUp);
        CHECK(result.stepRise == Approx(1.0f).margin(3.0e-3f));
        CHECK(result.onGround);
        // Horizontal momentum must survive the climb.
        CHECK(result.velocity.x == Approx(4.0f));
    }

    SECTION("step-up is refused with no headroom above the ledge")
    {
        TestWorld world;
        world.fill(-6, kFloorBlockY, -4, 8, kFloorBlockY, 4, blocks::Stone);
        world.fill(2, kFloorBlockY + 1, -4, 8, kFloorBlockY + 1, 4, blocks::Stone);
        // Ceiling 2 blocks above the standing surface, so rising is impossible.
        world.fill(-6, kFloorBlockY + 3, -4, 8, kFloorBlockY + 3, 4, blocks::Stone);
        const physics::VoxelCollider collider{world, registry()};

        const Box start = playerBox(1.69f, kFloorTop + params.skin, 0.5f);
        const physics::MoveResult result =
            physics::moveAabb(collider, start, glm::vec3{4.0f, 0.0f, 0.0f}, dt, params, true);

        CHECK_FALSE(result.steppedUp);
        CHECK(result.hitWallX);
    }

    SECTION("step-up is refused in mid-air")
    {
        TestWorld world;
        world.fill(2, kFloorBlockY + 1, -4, 8, kFloorBlockY + 1, 4, blocks::Stone);
        const physics::VoxelCollider collider{world, registry()};

        const Box start = playerBox(1.69f, kFloorTop + params.skin, 0.5f);
        const physics::MoveResult result =
            physics::moveAabb(collider, start, glm::vec3{4.0f, 0.0f, 0.0f}, dt, params, false);

        CHECK_FALSE(result.steppedUp);
        CHECK(result.hitWallX);
    }
}

TEST_CASE("horizontal collisions slide instead of stopping", "[physics][collision]")
{
    TestWorld world;
    world.fill(-6, kFloorBlockY, -6, 6, kFloorBlockY, 6, blocks::Stone);
    // A wall along the whole +X side, two blocks tall so there is no step-up.
    world.fill(2, kFloorBlockY + 1, -6, 2, kFloorBlockY + 2, 6, blocks::Stone);
    const physics::VoxelCollider collider{world, registry()};
    const physics::MotionParams  params{};

    const Box start = playerBox(1.69f, kFloorTop + params.skin, 0.5f);
    const physics::MoveResult result = physics::moveAabb(
        collider, start, glm::vec3{4.0f, 0.0f, 4.0f}, 1.0f / 60.0f, params, true);

    CHECK(result.hitWallX);
    CHECK_FALSE(result.hitWallZ);
    // The blocked axis clamps; the free one keeps its full displacement.
    CHECK(result.velocity.x == Approx(0.0f));
    CHECK(result.velocity.z == Approx(4.0f));
    CHECK(result.box.min.z > start.min.z);
}

// =================================================================== fluid ==

TEST_CASE("fluids are passable but slow the body down", "[physics][collision][fluid]")
{
    TestWorld world;
    world.fill(-4, kFloorBlockY, -4, 4, kFloorBlockY, 4, blocks::Stone);
    world.fill(-4, kFloorBlockY + 1, -4, 4, kFloorBlockY + 6, 4, blocks::Water);
    const physics::VoxelCollider collider{world, registry()};
    const physics::MotionParams  params{};

    const Box submerged = playerBox(0.5f, kFloorTop + 2.0f, 0.5f);
    CHECK_FALSE(collider.overlapsSolid(submerged));

    const physics::FluidState state = collider.sampleFluid(submerged);
    CHECK(state.inFluid);
    CHECK(state.feetInFluid);
    CHECK(state.headInFluid);
    CHECK(state.submerged == Approx(1.0f));

    // Reduced gravity and a much lower terminal velocity.
    float wetVelocity = 0.0f;
    float dryVelocity = 0.0f;
    for (int step = 0; step < 600; ++step) {
        wetVelocity = physics::applyGravity(wetVelocity, 1.0f / 60.0f, params, state);
        dryVelocity = physics::applyGravity(dryVelocity, 1.0f / 60.0f, params, physics::FluidState{});
    }
    CHECK(wetVelocity == Approx(-params.fluidTerminalVelocity));
    CHECK(dryVelocity == Approx(-params.terminalVelocity));
    CHECK(wetVelocity > dryVelocity);

    // Drag is frame-rate independent: the same simulated second must produce the
    // same speed however it is subdivided.
    const glm::vec3 initial{5.0f, 0.0f, 0.0f};
    glm::vec3       coarse = initial;
    glm::vec3       fine   = initial;
    for (int step = 0; step < 30; ++step) {
        coarse = physics::applyFluidDrag(coarse, 1.0f / 30.0f, params, state);
    }
    for (int step = 0; step < 240; ++step) {
        fine = physics::applyFluidDrag(fine, 1.0f / 240.0f, params, state);
    }
    CHECK(coarse.x == Approx(fine.x).epsilon(1.0e-4));
    CHECK(coarse.x < initial.x);

    // Falling through water reaches the floor without ever penetrating it.
    Box      box = playerBox(0.5f, kFloorTop + 5.0f, 0.5f);
    glm::vec3 velocity{0.0f};
    bool      onGround = false;
    for (int step = 0; step < 600; ++step) {
        const physics::FluidState wet = collider.sampleFluid(box);
        velocity.y = physics::applyGravity(velocity.y, 1.0f / 60.0f, params, wet);
        velocity   = physics::applyFluidDrag(velocity, 1.0f / 60.0f, params, wet);
        const physics::MoveResult result =
            physics::moveAabb(collider, box, velocity, 1.0f / 60.0f, params, onGround);
        box      = result.box;
        velocity = result.velocity;
        onGround = result.onGround;
        REQUIRE(box.min.y >= kFloorTop);
    }
    CHECK(onGround);
}

// ================================================================= raycast ==

TEST_CASE("axis-aligned raycast reports block, face and exact point", "[physics][raycast]")
{
    TestWorld world;
    world.set(5, 100, 0, blocks::Stone);
    const glm::vec3 origin{0.5f, 100.5f, 0.5f};

    SECTION("hit from the negative side")
    {
        const physics::RayHit hit =
            physics::raycastBlocks(world, registry(), origin, glm::vec3{1.0f, 0.0f, 0.0f}, 10.0f);
        REQUIRE(hit.hit);
        CHECK(hit.block == BlockPos{5, 100, 0});
        CHECK(hit.blockId == blocks::Stone);
        CHECK(hit.face == Direction::NegX);
        CHECK(hit.normal == glm::ivec3{-1, 0, 0});
        CHECK(hit.placement == BlockPos{4, 100, 0});
        CHECK(hit.distance == Approx(4.5f));
        CHECK(hit.point.x == Approx(5.0f));
        CHECK(hit.point.y == Approx(100.5f));
        CHECK_FALSE(hit.startedInside);
    }

    SECTION("hit from the positive side")
    {
        const physics::RayHit hit = physics::raycastBlocks(
            world, registry(), glm::vec3{9.5f, 100.5f, 0.5f}, glm::vec3{-1.0f, 0.0f, 0.0f}, 10.0f);
        REQUIRE(hit.hit);
        CHECK(hit.block == BlockPos{5, 100, 0});
        CHECK(hit.face == Direction::PosX);
        CHECK(hit.normal == glm::ivec3{1, 0, 0});
        CHECK(hit.placement == BlockPos{6, 100, 0});
        CHECK(hit.distance == Approx(3.5f));
        CHECK(hit.point.x == Approx(6.0f));
    }

    SECTION("an unnormalised direction gives the same answer")
    {
        const physics::RayHit hit =
            physics::raycastBlocks(world, registry(), origin, glm::vec3{7.0f, 0.0f, 0.0f}, 10.0f);
        REQUIRE(hit.hit);
        CHECK(hit.distance == Approx(4.5f));
    }

    SECTION("exact at a block boundary")
    {
        // Origin sits precisely on the x == 4 plane. The next boundary is one
        // block away, so the reported distance must be exactly 1.
        const physics::RayHit hit = physics::raycastBlocks(
            world, registry(), glm::vec3{4.0f, 100.5f, 0.5f}, glm::vec3{1.0f, 0.0f, 0.0f}, 10.0f);
        REQUIRE(hit.hit);
        CHECK(hit.block == BlockPos{5, 100, 0});
        CHECK(hit.distance == Approx(1.0f));
        CHECK(hit.point.x == Approx(5.0f));
    }
}

TEST_CASE("raycast misses when nothing is within reach", "[physics][raycast]")
{
    TestWorld world;
    world.set(5, 100, 0, blocks::Stone);
    const glm::vec3 origin{0.5f, 100.5f, 0.5f};

    SECTION("empty world")
    {
        const TestWorld       empty;
        const physics::RayHit hit = physics::raycastBlocks(
            empty, registry(), origin, glm::vec3{1.0f, 0.0f, 0.0f}, physics::kDefaultReach);
        CHECK_FALSE(hit.hit);
        CHECK_FALSE(static_cast<bool>(hit));
    }

    SECTION("target is just beyond the max distance")
    {
        // The block face is 4.5 away.
        CHECK_FALSE(
            physics::raycastBlocks(world, registry(), origin, glm::vec3{1.0f, 0.0f, 0.0f}, 4.0f)
                .hit);
        CHECK(physics::raycastBlocks(world, registry(), origin, glm::vec3{1.0f, 0.0f, 0.0f}, 5.0f)
                  .hit);
    }

    SECTION("a zero-length direction hits nothing")
    {
        CHECK_FALSE(physics::raycastBlocks(world, registry(), origin, glm::vec3{0.0f}, 10.0f).hit);
    }

    SECTION("air is never selected even though it is everywhere")
    {
        const physics::RayHit hit =
            physics::raycastBlocks(world, registry(), origin, glm::vec3{0.0f, 1.0f, 0.0f}, 6.0f);
        CHECK_FALSE(hit.hit);
    }
}

TEST_CASE("raycast starting inside a solid block", "[physics][raycast]")
{
    TestWorld world;
    world.set(5, 100, 0, blocks::Stone);

    const physics::RayHit hit = physics::raycastBlocks(
        world, registry(), glm::vec3{5.5f, 100.5f, 0.5f}, glm::vec3{1.0f, 0.0f, 0.0f}, 6.0f);

    REQUIRE(hit.hit);
    CHECK(hit.startedInside);
    CHECK(hit.block == BlockPos{5, 100, 0});
    CHECK(hit.distance == Approx(0.0f));
    CHECK(hit.point.x == Approx(5.5f));
    // The synthetic face comes from the dominant travel axis, so placement still
    // resolves to the neighbour the ray came from.
    CHECK(hit.face == Direction::NegX);
    CHECK(hit.placement == BlockPos{4, 100, 0});
}

TEST_CASE("diagonal rays hit the right face", "[physics][raycast]")
{
    TestWorld world;
    world.set(2, 101, 0, blocks::Stone);

    // Rises half a block per block travelled: crosses into y == 101 at x == 2.1,
    // which is inside the target voxel's x span, so the top-hit is through its
    // NegY face rather than NegX.
    const physics::RayHit hit = physics::raycastBlocks(
        world, registry(), glm::vec3{0.5f, 100.2f, 0.5f}, glm::vec3{1.0f, 0.5f, 0.0f}, 10.0f);

    REQUIRE(hit.hit);
    CHECK(hit.block == BlockPos{2, 101, 0});
    CHECK(hit.face == Direction::NegY);
    CHECK(hit.normal == glm::ivec3{0, -1, 0});
    CHECK(hit.placement == BlockPos{2, 100, 0});
    CHECK(hit.point.x == Approx(2.1f));
    CHECK(hit.point.y == Approx(101.0f));
    CHECK(hit.distance == Approx(std::sqrt(3.2f)));
}

TEST_CASE("traversal never skips a voxel on a grazing diagonal", "[physics][raycast]")
{
    // The strongest guarantee the DDA gives: consecutive visited voxels differ by
    // exactly one on exactly one axis. A fixed-step ray marcher fails this, and
    // that failure is what makes thin diagonal grazes miss.
    const glm::vec3 origin{0.5f, 100.5f, 0.5f};

    for (const glm::vec3 direction : {glm::vec3{1.0f, 1.0f, 1.0f}, glm::vec3{1.0f, 0.999f, 0.0f},
                                      glm::vec3{-3.0f, 1.0f, 7.0f}, glm::vec3{0.01f, -1.0f, 0.02f},
                                      glm::vec3{1.0f, 0.0f, 0.0f}}) {
        std::vector<BlockPos> visited;
        physics::traverseVoxels(origin, direction, 12.0f,
                                [&](const BlockPos& voxel, Direction, float) {
                                    visited.push_back(voxel);
                                    return false;
                                });

        REQUIRE(visited.size() > 1);
        CHECK(visited.front() == worldToBlockPos(origin));

        for (std::size_t i = 1; i < visited.size(); ++i) {
            const BlockPos& previous = visited[i - 1];
            const BlockPos& current  = visited[i];
            const int steps = std::abs(current.x - previous.x) + std::abs(current.y - previous.y) +
                              std::abs(current.z - previous.z);
            REQUIRE(steps == 1);
        }
    }
}

TEST_CASE("traversal reports monotonically increasing distances", "[physics][raycast]")
{
    float previous = -1.0f;
    bool  monotonic = true;
    float last      = 0.0f;
    physics::traverseVoxels(glm::vec3{0.25f, 100.75f, 0.5f}, glm::vec3{2.0f, -1.0f, 0.5f}, 7.0f,
                            [&](const BlockPos&, Direction, float distance) {
                                monotonic = monotonic && distance >= previous;
                                previous  = distance;
                                last      = distance;
                                return false;
                            });
    CHECK(monotonic);
    CHECK(last <= 7.0f);
}

TEST_CASE("the interaction raycast skips liquids", "[physics][raycast]")
{
    TestWorld world;
    world.set(2, 100, 0, blocks::Water);
    world.set(5, 100, 0, blocks::Stone);
    const glm::vec3 origin{0.5f, 100.5f, 0.5f};

    const physics::RayHit skipping =
        physics::raycastBlocks(world, registry(), origin, glm::vec3{1.0f, 0.0f, 0.0f}, 10.0f);
    REQUIRE(skipping.hit);
    CHECK(skipping.block == BlockPos{5, 100, 0});

    physics::RaycastFilter filter;
    filter.skipLiquids = false;
    const physics::RayHit including =
        physics::raycastBlocks(world, registry(), origin, glm::vec3{1.0f, 0.0f, 0.0f}, 10.0f, filter);
    REQUIRE(including.hit);
    CHECK(including.block == BlockPos{2, 100, 0});
}

// ================================================================== player ==

TEST_CASE("player look is clamped and matches the camera", "[gameplay][player]")
{
    Player player;

    player.look(1000.0f, -100000.0f);
    CHECK(player.pitchDegrees() <= player.config().effectiveMaxPitchDegrees());
    player.look(0.0f, 100000.0f);
    CHECK(player.pitchDegrees() >= -player.config().effectiveMaxPitchDegrees());

    player.setRotation(0.0f, 0.0f);
    // Mouse right turns right: the view direction must gain +X.
    player.look(10.0f, 0.0f);
    CHECK(player.lookDirection().x > 0.0f);
    player.setRotation(0.0f, 0.0f);
    // Mouse down looks down.
    player.look(0.0f, 10.0f);
    CHECK(player.lookDirection().y < 0.0f);

    Camera camera;
    player.setRotation(37.0f, 21.0f);
    player.updateCamera(camera, 1.0f);
    CHECK(camera.forward().x == Approx(player.lookDirection().x));
    CHECK(camera.forward().y == Approx(player.lookDirection().y));
    CHECK(camera.forward().z == Approx(player.lookDirection().z));
    CHECK(camera.position().y == Approx(player.eyePosition().y));
}

TEST_CASE("player walks, jumps and lands deterministically", "[gameplay][player]")
{
    TestWorld world;
    world.fill(-16, kFloorBlockY, -16, 16, kFloorBlockY, 16, blocks::Stone);
    const physics::VoxelCollider collider{world, registry()};

    auto simulate = [&](float dt, int steps) {
        Player player;
        player.setPosition(glm::vec3{0.5f, kFloorTop + 4.0f, 0.5f});
        player.setRotation(0.0f, 0.0f);
        PlayerInput input;
        input.forward = 1.0f;
        player.setInput(input);
        for (int step = 0; step < steps; ++step) {
            player.step(collider, dt);
            REQUIRE(player.position().y >= kFloorTop - 1.0e-3f);
        }
        return player;
    };

    SECTION("lands on the floor and walks forward along -Z")
    {
        const Player player = simulate(1.0f / 60.0f, 180);
        CHECK(player.onGround());
        CHECK(player.position().y == Approx(kFloorTop).margin(3.0e-3f));
        CHECK(player.position().z < 0.0f);  // yaw 0 looks down -Z
        CHECK(player.position().x == Approx(0.5f).margin(1.0e-4f));
    }

    SECTION("the fixed step gives the same landing height at any tick rate")
    {
        const Player slow = simulate(1.0f / 30.0f, 90);
        const Player fast = simulate(1.0f / 240.0f, 720);
        CHECK(slow.position().y == Approx(fast.position().y).margin(3.0e-3f));
        CHECK(slow.onGround());
        CHECK(fast.onGround());
    }

    SECTION("jumping clears a one-block ledge and returns to the ground")
    {
        Player player;
        player.setPosition(glm::vec3{0.5f, kFloorTop, 0.5f});
        PlayerInput input;
        player.setInput(input);
        for (int step = 0; step < 10; ++step) {
            player.step(collider, 1.0f / 60.0f);
        }
        REQUIRE(player.onGround());

        input.jump = true;
        player.setInput(input);
        player.step(collider, 1.0f / 60.0f);
        CHECK_FALSE(player.onGround());

        input.jump = false;
        player.setInput(input);
        float apex = player.position().y;
        for (int step = 0; step < 120; ++step) {
            player.step(collider, 1.0f / 60.0f);
            apex = std::max(apex, player.position().y);
        }
        CHECK(apex >= kFloorTop + 1.0f);
        CHECK(player.onGround());
        CHECK(player.position().y == Approx(kFloorTop).margin(3.0e-3f));
    }
}

TEST_CASE("player crouch and fly", "[gameplay][player]")
{
    TestWorld world;
    world.fill(-8, kFloorBlockY, -8, 8, kFloorBlockY, 8, blocks::Stone);
    const physics::VoxelCollider collider{world, registry()};

    Player player;
    player.setPosition(glm::vec3{0.5f, kFloorTop, 0.5f});

    PlayerInput input;
    input.crouch = true;
    player.setInput(input);
    player.step(collider, 1.0f / 60.0f);
    CHECK(player.crouching());
    CHECK(player.height() == Approx(player.config().crouchHeight));
    CHECK(player.bounds().max.y == Approx(player.position().y + player.config().crouchHeight));

    input.crouch = false;
    player.setInput(input);
    player.step(collider, 1.0f / 60.0f);
    CHECK_FALSE(player.crouching());
    CHECK(player.height() == Approx(player.config().standHeight));

    SECTION("cannot stand up under a low ceiling")
    {
        TestWorld low;
        low.fill(-8, kFloorBlockY, -8, 8, kFloorBlockY, 8, blocks::Stone);
        low.fill(-8, kFloorBlockY + 2, -8, 8, kFloorBlockY + 2, 8, blocks::Stone);
        const physics::VoxelCollider tight{low, registry()};

        Player crouched;
        crouched.setPosition(glm::vec3{0.5f, kFloorTop, 0.5f});
        PlayerInput crouchInput;
        crouchInput.crouch = true;
        crouched.setInput(crouchInput);
        crouched.step(tight, 1.0f / 60.0f);
        REQUIRE(crouched.crouching());

        crouchInput.crouch = false;
        crouched.setInput(crouchInput);
        crouched.step(tight, 1.0f / 60.0f);
        CHECK(crouched.crouching());
    }

    SECTION("fly ignores gravity and geometry")
    {
        Player flyer;
        flyer.setPosition(glm::vec3{0.5f, kFloorTop + 1.0f, 0.5f});
        flyer.toggleFly();
        REQUIRE(flyer.flying());

        PlayerInput flyInput;
        flyInput.flyDown = true;
        flyer.setInput(flyInput);
        for (int step = 0; step < 120; ++step) {
            flyer.step(collider, 1.0f / 60.0f);
        }
        // Straight through the floor: noclip is the whole point.
        CHECK(flyer.position().y < kFloorTop - 1.0f);
        CHECK_FALSE(flyer.onGround());

        flyer.setFlying(false);
        CHECK_FALSE(flyer.flying());
        CHECK(flyer.velocity() == glm::vec3{0.0f});
    }
}

TEST_CASE("player interpolation stays between the simulated endpoints",
          "[gameplay][player]")
{
    TestWorld world;
    world.fill(-8, kFloorBlockY, -8, 8, kFloorBlockY, 8, blocks::Stone);
    const physics::VoxelCollider collider{world, registry()};

    Player player;
    player.setPosition(glm::vec3{0.5f, kFloorTop + 3.0f, 0.5f});
    const glm::vec3 before = player.eyePosition();
    player.step(collider, 1.0f / 60.0f);
    const glm::vec3 after = player.eyePosition();

    REQUIRE(after.y < before.y);
    CHECK(player.eyePosition(0.0f).y == Approx(before.y));
    CHECK(player.eyePosition(1.0f).y == Approx(after.y));
    CHECK(player.eyePosition(0.5f).y == Approx((before.y + after.y) * 0.5f));
    // Out-of-range alphas are clamped rather than extrapolating through walls.
    CHECK(player.eyePosition(-5.0f).y == Approx(before.y));
    CHECK(player.eyePosition(5.0f).y == Approx(after.y));
}

TEST_CASE("player crosshair pick", "[gameplay][player]")
{
    TestWorld world;
    world.fill(-8, kFloorBlockY, -8, 8, kFloorBlockY, 8, blocks::Stone);
    const physics::VoxelCollider collider{world, registry()};

    Player player;
    player.setPosition(glm::vec3{0.5f, kFloorTop, 0.5f});
    player.setRotation(0.0f, -90.0f);  // clamped to the effective limit, near straight down
    player.step(collider, 1.0f / 60.0f);

    const physics::RayHit hit = player.lookingAt(world, registry());
    REQUIRE(hit.hit);
    CHECK(hit.block.y == kFloorBlockY);
    CHECK(hit.face == Direction::PosY);
    CHECK(hit.placement == BlockPos{hit.block.x, kFloorBlockY + 1, hit.block.z});
}

// ======================================================= sub-voxel physics ==
//
// The sub-voxel collision path had no coverage at all: `isSubVoxelBlocking`,
// the damaged-block branch of `overlapsSolid` and the `nearestSubVoxelGap`
// branch of `sweepAxisAgainstWorld` were reachable only from a live world. A
// bug in any of them is a player who walks through a wall or is stopped by
// empty air, and neither shows up in a screenshot - so they are driven here
// directly, from a hand-authored occupancy grid.

namespace {

/// Hand-authored sub-voxel damage.
///
/// Obeying the world/SubVoxel.hpp invariant is the caller's job here exactly as
/// it is in ChunkStorage: an entry must have 0 < popcount < 512 and a material
/// equal to the parent block's id.
class TestSubVoxels final : public physics::SubVoxelAccess {
public:
    /// Materialises a full grid on first touch, then clears one bit - the same
    /// "damage starts from intact" order Chunk::breakSubVoxel uses.
    void carve(const BlockPos& pos, BlockId material, std::int32_t sx, std::int32_t sy,
               std::int32_t sz)
    {
        const auto it = m_grids.try_emplace(pos, SubVoxelGrid::solid(material)).first;
        it->second.clear(subVoxelIndex(sx, sy, sz));
    }

    /// Carves an inclusive sub-voxel box out of one block.
    void carveRange(const BlockPos& pos, BlockId material, std::int32_t x0, std::int32_t y0,
                    std::int32_t z0, std::int32_t x1, std::int32_t y1, std::int32_t z1)
    {
        for (std::int32_t y = y0; y <= y1; ++y) {
            for (std::int32_t z = z0; z <= z1; ++z) {
                for (std::int32_t x = x0; x <= x1; ++x) {
                    carve(pos, material, x, y, z);
                }
            }
        }
    }

    [[nodiscard]] std::size_t remaining(const BlockPos& pos) const
    {
        const auto it = m_grids.find(pos);
        return it == m_grids.end() ? kSubVoxelCount : it->second.count();
    }

    [[nodiscard]] const SubVoxelGrid* subVoxelsAt(const BlockPos& pos) const noexcept override
    {
        const auto it = m_grids.find(pos);
        return it == m_grids.end() ? nullptr : &it->second;
    }

private:
    std::unordered_map<BlockPos, SubVoxelGrid> m_grids;
};

}  // namespace

TEST_CASE("an empty sub-voxel accessor is indistinguishable from none",
          "[physics][subvoxel]")
{
    TestWorld world;
    world.fill(-2, kFloorBlockY, -2, 2, kFloorBlockY, 2, blocks::Stone);

    const physics::VoxelCollider plain{world, registry()};
    TestSubVoxels                damage;
    const physics::VoxelCollider aware{world, registry(), &damage};

    const Box box     = playerBox(0.5f, kFloorTop, 0.5f);
    const Box sunk    = box.translated(glm::vec3{0.0f, -0.5f, 0.0f});
    const Box raised  = box.translated(glm::vec3{0.0f, 1.0f, 0.0f});

    // The whole "untouched terrain pays nothing" claim rests on this.
    CHECK(plain.isBlockWhole(BlockPos{0, kFloorBlockY, 0}));
    CHECK(aware.isBlockWhole(BlockPos{0, kFloorBlockY, 0}));
    CHECK(plain.overlapsSolid(sunk) == aware.overlapsSolid(sunk));
    CHECK(physics::isSupported(plain, box) == physics::isSupported(aware, box));

    const physics::AxisMove plainDrop =
        physics::sweepAxisAgainstWorld(plain, raised, physics::kAxisY, -2.0f);
    const physics::AxisMove awareDrop =
        physics::sweepAxisAgainstWorld(aware, raised, physics::kAxisY, -2.0f);
    CHECK(plainDrop.travelled == Approx(awareDrop.travelled));
    CHECK(plainDrop.blocked == awareDrop.blocked);
}

TEST_CASE("isSubVoxelBlocking reads the grid and falls back to the block id",
          "[physics][subvoxel]")
{
    TestWorld world;
    world.set(0, kFloorBlockY, 0, blocks::Stone);
    world.set(1, kFloorBlockY, 0, blocks::Stone);

    TestSubVoxels damage;
    damage.carve(BlockPos{0, kFloorBlockY, 0}, blocks::Stone, 3, 4, 5);
    const physics::VoxelCollider collider{world, registry(), &damage};

    const BlockPos damaged{0, kFloorBlockY, 0};
    const BlockPos intact{1, kFloorBlockY, 0};

    CHECK_FALSE(collider.isSubVoxelBlocking(damaged, 3, 4, 5));
    CHECK(collider.isSubVoxelBlocking(damaged, 3, 4, 4));
    CHECK_FALSE(collider.isBlockWhole(damaged));

    // No entry means uniform: every sub-voxel of an intact solid block is solid.
    CHECK(collider.isBlockWhole(intact));
    CHECK(collider.isSubVoxelBlocking(intact, 0, 0, 0));
    CHECK(collider.isSubVoxelBlocking(intact, 7, 7, 7));

    // Air is never blocking, and out-of-range coordinates degrade to empty
    // space rather than indexing past the grid.
    CHECK_FALSE(collider.isSubVoxelBlocking(BlockPos{5, kFloorBlockY, 5}, 0, 0, 0));
    CHECK_FALSE(collider.isSubVoxelBlocking(damaged, -1, 0, 0));
    CHECK_FALSE(collider.isSubVoxelBlocking(damaged, kSubVoxelResolution, 0, 0));
    CHECK_FALSE(collider.isSubVoxelBlocking(damaged, 0, 0, kSubVoxelResolution));
}

TEST_CASE("a box inside a carved alcove does not overlap the damaged block",
          "[physics][subvoxel]")
{
    TestWorld world;
    world.set(0, kFloorBlockY, 0, blocks::Stone);

    TestSubVoxels damage;
    // Hollow out the upper half of the block: sub-voxel rows 4..7 in y.
    damage.carveRange(BlockPos{0, kFloorBlockY, 0}, blocks::Stone, 0, 4, 0, 7, 7, 7);
    REQUIRE(damage.remaining(BlockPos{0, kFloorBlockY, 0}) == kSubVoxelCount / 2);

    const physics::VoxelCollider collider{world, registry(), &damage};
    const float                  base = static_cast<float>(kFloorBlockY);

    const Box inAlcove{glm::vec3{0.1f, base + 0.6f, 0.1f}, glm::vec3{0.9f, base + 0.9f, 0.9f}};
    CHECK_FALSE(collider.overlapsSolid(inAlcove));

    const Box inMaterial{glm::vec3{0.1f, base + 0.1f, 0.1f}, glm::vec3{0.9f, base + 0.4f, 0.9f}};
    CHECK(collider.overlapsSolid(inMaterial));

    // Without the accessor the whole cube is solid, which is the behaviour the
    // damaged branch has to differ from for this test to mean anything.
    const physics::VoxelCollider blind{world, registry()};
    CHECK(blind.overlapsSolid(inAlcove));
}

TEST_CASE("a sweep stops at the surviving material, not at the block face",
          "[physics][subvoxel]")
{
    TestWorld world;
    world.set(2, kFloorBlockY, 0, blocks::Stone);

    TestSubVoxels damage;
    // Carve the four sub-voxel slabs nearest the approach side, so the surviving
    // surface sits half a block behind the cube face at x == 2.
    damage.carveRange(BlockPos{2, kFloorBlockY, 0}, blocks::Stone, 0, 0, 0, 3, 7, 7);
    const physics::VoxelCollider collider{world, registry(), &damage};

    const float base = static_cast<float>(kFloorBlockY);
    const Box   box{glm::vec3{0.0f, base + 0.1f, 0.1f}, glm::vec3{1.0f, base + 0.9f, 0.9f}};

    const physics::AxisMove move = physics::sweepAxisAgainstWorld(collider, box, physics::kAxisX, 2.0f);
    CHECK(move.blocked);
    // Material starts at x = 2.5 and the leading face is at x = 1.0.
    CHECK(move.travelled == Approx(1.5f - physics::kCollisionSkin).margin(1.0e-4f));

    const physics::VoxelCollider blind{world, registry()};
    const physics::AxisMove blindMove = physics::sweepAxisAgainstWorld(blind, box, physics::kAxisX, 2.0f);
    CHECK(blindMove.blocked);
    CHECK(blindMove.travelled == Approx(1.0f - physics::kCollisionSkin).margin(1.0e-4f));
}

TEST_CASE("a bored-out block lets a body pass straight through", "[physics][subvoxel]")
{
    TestWorld world;
    world.fill(2, kFloorBlockY, -1, 2, kFloorBlockY + 2, 1, blocks::Stone);

    TestSubVoxels  damage;
    const BlockPos hole{2, kFloorBlockY, 0};
    // Everything except one corner sub-voxel, which keeps the entry legal
    // (0 < popcount < 512) and sits outside the box's y/z span.
    damage.carveRange(hole, blocks::Stone, 1, 0, 0, 7, 7, 7);
    damage.carveRange(hole, blocks::Stone, 0, 1, 0, 0, 7, 7);
    damage.carveRange(hole, blocks::Stone, 0, 0, 1, 0, 0, 7);
    REQUIRE(damage.remaining(hole) == 1);

    const physics::VoxelCollider collider{world, registry(), &damage};
    const float                  base = static_cast<float>(kFloorBlockY);
    const Box box{glm::vec3{0.0f, base + 0.2f, 0.2f}, glm::vec3{0.9f, base + 0.9f, 0.9f}};

    const physics::AxisMove move = physics::sweepAxisAgainstWorld(collider, box, physics::kAxisX, 1.5f);
    CHECK_FALSE(move.blocked);
    CHECK(move.travelled == Approx(1.5f));

    // The blocks above are untouched, so a body one block higher is still
    // stopped - the hole is exactly as wide as it was carved.
    const Box higher = box.translated(glm::vec3{0.0f, 1.0f, 0.0f});
    const physics::AxisMove blockedMove =
        physics::sweepAxisAgainstWorld(collider, higher, physics::kAxisX, 1.5f);
    CHECK(blockedMove.blocked);
    // Leading face at x = 0.9, intact cube face at x = 2.0.
    CHECK(blockedMove.travelled == Approx(1.1f - physics::kCollisionSkin).margin(1.0e-4f));
}

TEST_CASE("carving the ground out from under a body removes its support",
          "[physics][subvoxel]")
{
    TestWorld world;
    world.fill(-2, kFloorBlockY, -2, 2, kFloorBlockY, 2, blocks::Stone);

    TestSubVoxels                damage;
    const physics::VoxelCollider collider{world, registry(), &damage};
    const Box                    box = playerBox(0.5f, kFloorTop, 0.5f);
    REQUIRE(physics::isSupported(collider, box));

    // Remove the top sub-voxel layer of every block under the feet.
    for (std::int32_t bx = 0; bx <= 1; ++bx) {
        for (std::int32_t bz = 0; bz <= 1; ++bz) {
            damage.carveRange(BlockPos{bx, kFloorBlockY, bz}, blocks::Stone, 0, 7, 0, 7, 7, 7);
        }
    }
    CHECK_FALSE(physics::isSupported(collider, box));

    const physics::AxisMove drop = physics::sweepAxisAgainstWorld(collider, box, physics::kAxisY, -1.0f);
    CHECK(drop.blocked);
    CHECK(drop.travelled == Approx(-(kSubVoxelSize - physics::kCollisionSkin)).margin(1.0e-4f));
}

TEST_CASE("a full move resolves against damaged geometry without tunnelling",
          "[physics][subvoxel]")
{
    TestWorld world;
    world.fill(-4, kFloorBlockY, -4, 4, kFloorBlockY, 4, blocks::Stone);
    world.fill(2, kFloorBlockY + 1, -4, 2, kFloorBlockY + 3, 4, blocks::Stone);

    TestSubVoxels damage;
    // Chip the player-facing half off every wall block the body can reach.
    for (std::int32_t by = kFloorBlockY + 1; by <= kFloorBlockY + 3; ++by) {
        for (std::int32_t bz = -1; bz <= 1; ++bz) {
            damage.carveRange(BlockPos{2, by, bz}, blocks::Stone, 0, 0, 0, 3, 7, 7);
        }
    }
    const physics::VoxelCollider collider{world, registry(), &damage};

    const physics::MotionParams params;
    Box                         box = playerBox(0.5f, kFloorTop, 0.5f);

    // Four seconds of walking straight into the wall. If the sub-voxel gap were
    // ever reported larger than it is, the body ends up inside the material and
    // the next step reports startedStuck.
    for (int step = 0; step < 240; ++step) {
        const physics::MoveResult result = physics::moveAabb(
            collider, box, glm::vec3{6.0f, 0.0f, 0.0f}, 1.0f / 60.0f, params, true);
        box = result.box;
        REQUIRE_FALSE(result.startedStuck);
    }

    CHECK_FALSE(collider.overlapsSolid(box));
    // Resting against the carved surface at x = 2.5, not the cube face at 2.0.
    CHECK(box.max.x <= 2.5f);
    CHECK(box.max.x > 2.4f);
}
