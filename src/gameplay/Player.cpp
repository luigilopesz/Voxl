#include "gameplay/Player.hpp"

#include <algorithm>
#include <cmath>

#include <glm/trigonometric.hpp>
#include <glm/vec2.hpp>

namespace voxl {
namespace {

/// Below this the input stick is treated as centred. Comparing against exactly
/// zero would let a denormal from a normalised gamepad axis count as movement
/// and keep the player permanently in "accelerating" rather than "braking".
constexpr float kInputDeadzone = 1.0e-4f;

[[nodiscard]] float lengthXZ(const glm::vec2& v) noexcept
{
    return std::sqrt(v.x * v.x + v.y * v.y);
}

}  // namespace

Player::Player(PlayerConfig config) noexcept : m_config(config)
{
    m_height            = m_config.standHeight;
    m_eyeHeight         = m_config.standEyeHeight;
    m_previousEyeHeight = m_eyeHeight;
}

void Player::setConfig(const PlayerConfig& config) noexcept
{
    m_config = config;
    // Re-derive the stance so a config change cannot leave the collision box at
    // a height the new config does not describe.
    m_height    = targetHeight();
    m_eyeHeight = targetEyeHeight();
    setRotation(m_yawDegrees, m_pitchDegrees);
}

// ---------------------------------------------------------------- placement --

void Player::setPosition(const glm::vec3& feetPosition) noexcept
{
    m_position         = feetPosition;
    m_previousPosition = feetPosition;
    m_velocity         = glm::vec3{0.0f};
    m_onGround         = false;
    m_steppedUp        = false;
    m_stepRise         = 0.0f;
}

physics::Aabb Player::bounds() const noexcept
{
    return physics::Aabb::fromFeet(m_position, m_config.width, m_height);
}

physics::Aabb Player::boundsAt(const glm::vec3& feetPosition) const noexcept
{
    return physics::Aabb::fromFeet(feetPosition, m_config.width, m_height);
}

glm::vec3 Player::eyePosition() const noexcept
{
    return glm::vec3{m_position.x, m_position.y + m_eyeHeight, m_position.z};
}

glm::vec3 Player::eyePosition(float alpha) const noexcept
{
    const float t = std::clamp(alpha, 0.0f, 1.0f);
    const glm::vec3 feet = m_previousPosition + (m_position - m_previousPosition) * t;
    const float eye = m_previousEyeHeight + (m_eyeHeight - m_previousEyeHeight) * t;
    return glm::vec3{feet.x, feet.y + eye, feet.z};
}

// --------------------------------------------------------------------- look --

void Player::look(float deltaXPixels, float deltaYPixels) noexcept
{
    const float sensitivity = m_config.mouseSensitivity;
    // Yaw increases turning LEFT (see Camera's conventions), so a rightward
    // cursor delta must subtract. Screen +y is downward, so looking down also
    // subtracts.
    const float yaw   = m_yawDegrees - deltaXPixels * sensitivity;
    const float sign  = m_config.invertMouseY ? 1.0f : -1.0f;
    const float pitch = m_pitchDegrees + sign * deltaYPixels * sensitivity;
    setRotation(yaw, pitch);
}

void Player::setRotation(float yawDegrees, float pitchDegrees) noexcept
{
    // Wrapping keeps yaw from growing without bound across a long session, where
    // float precision would make mouse-look visibly grainy.
    m_yawDegrees = std::fmod(yawDegrees, 360.0f);
    const float limit = m_config.effectiveMaxPitchDegrees();
    m_pitchDegrees    = std::clamp(pitchDegrees, -limit, limit);
}

glm::vec3 Player::lookDirection() const noexcept
{
    const float yaw      = glm::radians(m_yawDegrees);
    const float pitch    = glm::radians(m_pitchDegrees);
    const float cosPitch = std::cos(pitch);
    return glm::vec3{-std::sin(yaw) * cosPitch, std::sin(pitch), -std::cos(yaw) * cosPitch};
}

glm::vec3 Player::flatForward() const noexcept
{
    const float yaw = glm::radians(m_yawDegrees);
    return glm::vec3{-std::sin(yaw), 0.0f, -std::cos(yaw)};
}

glm::vec3 Player::right() const noexcept
{
    const float yaw = glm::radians(m_yawDegrees);
    return glm::vec3{std::cos(yaw), 0.0f, -std::sin(yaw)};
}

// --------------------------------------------------------------- simulation --

void Player::setInput(const PlayerInput& input) noexcept
{
    m_input         = input;
    m_input.forward = std::clamp(m_input.forward, -1.0f, 1.0f);
    m_input.strafe  = std::clamp(m_input.strafe, -1.0f, 1.0f);
}

void Player::setFlying(bool flying) noexcept
{
    if (m_flying == flying) {
        return;
    }
    m_flying = flying;
    // Killing velocity on the transition stops a fall from continuing into fly
    // mode and stops fly speed from launching the player on exit.
    m_velocity = glm::vec3{0.0f};
    m_onGround = false;
    if (m_flying) {
        m_crouching = false;
        m_fluid     = physics::FluidState{};
    }
}

float Player::targetHeight() const noexcept
{
    return m_crouching ? m_config.crouchHeight : m_config.standHeight;
}

float Player::targetEyeHeight() const noexcept
{
    return m_crouching ? m_config.crouchEyeHeight : m_config.standEyeHeight;
}

void Player::applyStance(const physics::VoxelCollider& world, float dt) noexcept
{
    const bool wantsCrouch = m_input.crouch && !m_flying;
    if (wantsCrouch) {
        m_crouching = true;
    } else if (m_crouching) {
        // Standing up must not drive the head into a ceiling. Staying crouched
        // under a one-block gap is the correct outcome, not a soft-lock.
        const physics::Aabb standing =
            physics::Aabb::fromFeet(m_position, m_config.width, m_config.standHeight);
        if (m_flying || !world.overlapsSolid(standing, m_config.motion.skin)) {
            m_crouching = false;
        }
    }

    // The collision box snaps: physics must never disagree with the geometry it
    // is resolving against. Only the eye is eased, because a snapping camera
    // reads as a glitch.
    m_height = targetHeight();

    const float target    = targetEyeHeight();
    const float maxChange = std::max(m_config.eyeTransitionSpeed, 0.0f) * dt;
    const float difference = target - m_eyeHeight;
    if (std::abs(difference) <= maxChange || maxChange <= 0.0f) {
        m_eyeHeight = target;
    } else {
        m_eyeHeight += difference > 0.0f ? maxChange : -maxChange;
    }
}

void Player::accelerateHorizontal(float dt, float targetSpeed, float acceleration,
                                  float friction) noexcept
{
    const glm::vec3 wish =
        flatForward() * m_input.forward + right() * m_input.strafe;
    const glm::vec2 wishXZ{wish.x, wish.z};
    const float     wishLength = lengthXZ(wishXZ);

    glm::vec2 target{0.0f, 0.0f};
    if (wishLength > kInputDeadzone) {
        // Normalising stops diagonal input from being sqrt(2) faster; the
        // min() preserves partial magnitude from an analogue stick.
        const float scale = std::min(wishLength, 1.0f) / wishLength;
        target            = wishXZ * (scale * targetSpeed);
    }

    const float     rate = wishLength > kInputDeadzone ? acceleration : friction;
    const glm::vec2 current{m_velocity.x, m_velocity.z};

    glm::vec2   difference = target - current;
    const float distance   = lengthXZ(difference);
    const float maxChange  = std::max(rate, 0.0f) * dt;
    if (distance > maxChange && distance > 0.0f) {
        difference *= maxChange / distance;
    }

    m_velocity.x = current.x + difference.x;
    m_velocity.z = current.y + difference.y;
}

void Player::stepFly(float dt) noexcept
{
    // Fly uses the yaw-only basis plus explicit up/down rather than the full
    // look direction: aiming at the floor while flying forward should not drive
    // you into it.
    const float speed =
        m_config.flySpeed * (m_input.sprint ? std::max(m_config.flySprintMultiplier, 1.0f) : 1.0f);

    glm::vec3 wish = flatForward() * m_input.forward + right() * m_input.strafe;
    wish.y += (m_input.flyUp ? 1.0f : 0.0f) - (m_input.flyDown ? 1.0f : 0.0f);

    glm::vec3   target{0.0f};
    const float wishLength = std::sqrt(wish.x * wish.x + wish.y * wish.y + wish.z * wish.z);
    if (wishLength > kInputDeadzone) {
        target = wish * (std::min(wishLength, 1.0f) / wishLength * speed);
    }

    glm::vec3   difference = target - m_velocity;
    const float distance =
        std::sqrt(difference.x * difference.x + difference.y * difference.y +
                  difference.z * difference.z);
    const float maxChange = std::max(m_config.flyAcceleration, 0.0f) * dt;
    if (distance > maxChange && distance > 0.0f) {
        difference *= maxChange / distance;
    }
    m_velocity += difference;

    // Noclip: no sweep, no collider. That is the whole point of the debug mode.
    m_position += m_velocity * dt;

    m_onGround  = false;
    m_sprinting = m_input.sprint;
    m_fluid     = physics::FluidState{};
}

void Player::stepWalk(const physics::VoxelCollider& world, float dt) noexcept
{
    const physics::MotionParams& motion = m_config.motion;

    m_fluid = world.sampleFluid(bounds());

    const bool hasMoveInput =
        std::abs(m_input.forward) > kInputDeadzone || std::abs(m_input.strafe) > kInputDeadzone;
    m_sprinting = m_input.sprint && !m_crouching && hasMoveInput;

    float targetSpeed  = 0.0f;
    float acceleration = 0.0f;
    float friction     = 0.0f;
    if (m_fluid.inFluid) {
        targetSpeed  = m_config.swimSpeed;
        acceleration = m_config.swimAcceleration;
        friction     = m_config.swimFriction;
    } else {
        targetSpeed = m_crouching ? m_config.crouchSpeed
                                  : (m_sprinting ? m_config.sprintSpeed : m_config.walkSpeed);
        acceleration = m_onGround ? m_config.groundAcceleration : m_config.airAcceleration;
        friction     = m_onGround ? m_config.groundFriction : m_config.airFriction;
    }
    accelerateHorizontal(dt, targetSpeed, acceleration, friction);

    // Gravity before the jump impulse, so a jump issued on the same step is not
    // immediately eaten by one step of gravity.
    m_velocity.y = physics::applyGravity(m_velocity.y, dt, motion, m_fluid);

    if (m_input.jump) {
        if (m_fluid.inFluid) {
            // A swim stroke sets a floor on upward speed rather than adding, so
            // holding jump underwater rises steadily instead of accelerating.
            m_velocity.y = std::max(m_velocity.y, m_config.swimUpSpeed);
        } else if (m_onGround) {
            m_velocity.y = m_config.jumpVelocity;
            m_onGround   = false;
        }
    }

    m_velocity = physics::applyFluidDrag(m_velocity, dt, motion, m_fluid);

    const physics::Aabb       before = bounds();
    const physics::MoveResult result =
        physics::moveAabb(world, before, m_velocity, dt, motion, m_onGround);

    // Track the box delta rather than recomputing the centre from min/max: the
    // round trip through half-width would accumulate a rounding error every
    // step.
    m_position += result.box.min - before.min;
    m_velocity = result.velocity;
    m_onGround = result.onGround;
    m_steppedUp = result.steppedUp;
    m_stepRise  = result.stepRise;
}

void Player::step(const physics::VoxelCollider& world, float fixedDt) noexcept
{
    m_previousPosition  = m_position;
    m_previousEyeHeight = m_eyeHeight;
    m_steppedUp         = false;
    m_stepRise          = 0.0f;

    if (!(fixedDt > 0.0f)) {
        return;
    }

    applyStance(world, fixedDt);

    if (m_flying) {
        stepFly(fixedDt);
        return;
    }
    stepWalk(world, fixedDt);
}

void Player::updateCamera(Camera& camera, float alpha) const noexcept
{
    camera.setPosition(eyePosition(alpha));
    // Rotation is not interpolated: mouse deltas are applied at render rate, so
    // the current angles are already the freshest data available.
    camera.setRotation(m_yawDegrees, m_pitchDegrees);
}

physics::RayHit Player::lookingAt(const BlockAccess& world,
                                  const BlockRegistry& registry) const noexcept
{
    return physics::raycastBlocks(world, registry, eyePosition(), lookDirection(), m_config.reach);
}

}  // namespace voxl
