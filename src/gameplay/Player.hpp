#pragma once

// First-person player controller: mouse look, WASD movement, sprint, jump,
// crouch, swimming and a noclip fly toggle for debugging.
//
// THE SPLIT BETWEEN step() AND updateCamera()
// -------------------------------------------
// Simulation runs on the fixed timestep from FrameClock and rendering does not,
// so the two must not share a code path:
//
//     clock.tick();
//     player.setInput(input);
//     while (clock.nextFixedStep()) { player.step(collider, clock.fixedDeltaSeconds()); }
//     player.updateCamera(camera, clock.fixedAlpha());
//
// step() advances position and velocity by exactly one fixed step, so the same
// inputs produce the same trajectory at 30 fps and at 300 fps - a variable-dt
// integrator would not, and the "does not fall through the floor at any frame
// rate" requirement would depend on the machine. updateCamera() interpolates
// between the last two simulated positions using the leftover accumulator
// fraction, which is what removes the stutter a fixed timestep would otherwise
// show at non-multiple frame rates.
//
// Look angles are the exception: they are NOT interpolated. Mouse deltas arrive
// at render rate and are applied immediately, because pushing them through the
// fixed step would add up to a full step of aim latency for no visual benefit.
//
// Thread safety: none. Main-thread gameplay state.

#include "physics/Aabb.hpp"
#include "physics/Collision.hpp"
#include "physics/Raycast.hpp"
#include "render/Camera.hpp"
#include "world/Block.hpp"
#include "world/BlockAccess.hpp"

#include <glm/vec3.hpp>

namespace voxl {

/// One frame of intent. The application maps keys/gamepad onto this; the
/// controller never reads an input device itself, which is what lets a test
/// drive it deterministically.
struct PlayerInput {
    /// Along the yaw-relative horizontal forward axis, in [-1, 1].
    float forward = 0.0f;
    /// Along the yaw-relative horizontal right axis, in [-1, 1].
    float strafe = 0.0f;

    bool jump   = false;
    bool sprint = false;
    bool crouch = false;

    /// Only meaningful while flying.
    bool flyUp   = false;
    bool flyDown = false;
};

/// Tunables. Speeds are blocks/second, accelerations blocks/second^2.
struct PlayerConfig {
    /// Collision volume. A width under 1 lets the player fit through a
    /// one-block gap; the 1.8 height needs a two-block opening, as expected.
    float width        = 0.6f;
    float standHeight  = 1.8f;
    float crouchHeight = 1.5f;

    /// Eye offset above the feet. Slightly below the top of the head so the
    /// camera does not clip through a ceiling the body is touching.
    float standEyeHeight  = 1.62f;
    float crouchEyeHeight = 1.32f;
    /// Blocks/second at which the eye slides between the two heights. The
    /// collision box snaps instantly (physics must never lag the geometry) but a
    /// snapping camera reads as a glitch, so only the eye is smoothed.
    float eyeTransitionSpeed = 12.0f;

    float walkSpeed   = 4.317f;
    float sprintSpeed = 5.612f;
    float crouchSpeed = 1.295f;
    float swimSpeed   = 2.2f;

    /// Fly is a debug mode, so it is deliberately much faster than walking.
    float flySpeed           = 14.0f;
    float flySprintMultiplier = 4.0f;
    float flyAcceleration    = 90.0f;

    /// Chosen for the gravity in `motion`: v = sqrt(2 g h) with h = 1.25 blocks,
    /// so a jump clears a one-block ledge with margin but not a two-block wall.
    float jumpVelocity = 8.95f;
    /// Upward speed while holding jump underwater.
    float swimUpSpeed = 3.0f;

    float groundAcceleration = 60.0f;
    float groundFriction     = 45.0f;
    /// Air control is deliberately weak: full authority in mid-air makes jumps
    /// feel weightless and trivialises every gap.
    float airAcceleration = 14.0f;
    float airFriction     = 3.0f;
    float swimAcceleration = 18.0f;
    float swimFriction     = 12.0f;

    /// Degrees of rotation per pixel of mouse movement.
    float mouseSensitivity = 0.12f;
    /// Set for players who prefer inverted vertical aim.
    bool invertMouseY = false;

    /// Requested pitch limit. The effective limit is the smaller of this and
    /// Camera::kMaxPitchDegrees, because a Player that allowed a steeper pitch
    /// than the camera can represent would aim the interaction raycast somewhere
    /// other than where the crosshair is drawn.
    float maxPitchDegrees = 89.9f;

    /// Interaction reach in blocks.
    float reach = physics::kDefaultReach;

    physics::MotionParams motion{};

    /// Effective pitch clamp; see maxPitchDegrees.
    [[nodiscard]] float effectiveMaxPitchDegrees() const noexcept
    {
        return maxPitchDegrees < Camera::kMaxPitchDegrees ? maxPitchDegrees
                                                         : Camera::kMaxPitchDegrees;
    }
};

class Player {
public:
    explicit Player(PlayerConfig config = {}) noexcept;

    [[nodiscard]] const PlayerConfig& config() const noexcept { return m_config; }
    void setConfig(const PlayerConfig& config) noexcept;

    // ------------------------------------------------------------ placement --

    /// Centre of the bottom face of the collision box - "where the player is
    /// standing". Everything else (eye, bounds) derives from it.
    [[nodiscard]] const glm::vec3& position() const noexcept { return m_position; }

    /// Teleports. Clears velocity and the interpolation history so the next
    /// frame does not smear across the jump.
    void setPosition(const glm::vec3& feetPosition) noexcept;

    [[nodiscard]] const glm::vec3& velocity() const noexcept { return m_velocity; }
    void setVelocity(const glm::vec3& velocity) noexcept { m_velocity = velocity; }

    /// Current collision volume.
    [[nodiscard]] physics::Aabb bounds() const noexcept;
    /// The volume the player would occupy standing at `feetPosition`, at the
    /// current stance height. Used for spawn and teleport validation.
    [[nodiscard]] physics::Aabb boundsAt(const glm::vec3& feetPosition) const noexcept;

    [[nodiscard]] float height() const noexcept { return m_height; }
    [[nodiscard]] float eyeHeight() const noexcept { return m_eyeHeight; }

    [[nodiscard]] glm::vec3 eyePosition() const noexcept;
    /// Render-time eye position. `alpha` is FrameClock::fixedAlpha().
    [[nodiscard]] glm::vec3 eyePosition(float alpha) const noexcept;

    // ----------------------------------------------------------------- look --

    /// Raw cursor delta in pixels, GLFW convention (+y is downward on screen).
    /// Moving the mouse right turns right and moving it down looks down; the
    /// sign flips fall out of the camera's left-handed-screen to right-handed-
    /// world mapping, so they live here once rather than at every call site.
    void look(float deltaXPixels, float deltaYPixels) noexcept;

    void setRotation(float yawDegrees, float pitchDegrees) noexcept;
    [[nodiscard]] float yawDegrees() const noexcept { return m_yawDegrees; }
    [[nodiscard]] float pitchDegrees() const noexcept { return m_pitchDegrees; }

    /// Unit view direction, identical to what the camera will use.
    [[nodiscard]] glm::vec3 lookDirection() const noexcept;
    /// Yaw-only basis vectors that WASD is expressed in. Mirrors Camera so the
    /// controller can be simulated without one.
    [[nodiscard]] glm::vec3 flatForward() const noexcept;
    [[nodiscard]] glm::vec3 right() const noexcept;

    // ----------------------------------------------------------- simulation --

    void setInput(const PlayerInput& input) noexcept;
    [[nodiscard]] const PlayerInput& input() const noexcept { return m_input; }

    /// Advances exactly one fixed step. `fixedDt` must be
    /// FrameClock::fixedDeltaSeconds().
    void step(const physics::VoxelCollider& world, float fixedDt) noexcept;

    /// Writes the interpolated eye transform into `camera`. Call once per frame
    /// after the fixed-step loop.
    void updateCamera(Camera& camera, float alpha) const noexcept;

    /// What the crosshair is on. Skips air and liquids so a block placed while
    /// swimming goes where it is aimed.
    [[nodiscard]] physics::RayHit lookingAt(const BlockAccess& world,
                                            const BlockRegistry& registry) const noexcept;

    // ---------------------------------------------------------------- state --

    [[nodiscard]] bool onGround() const noexcept { return m_onGround; }
    [[nodiscard]] bool crouching() const noexcept { return m_crouching; }
    [[nodiscard]] bool sprinting() const noexcept { return m_sprinting; }
    [[nodiscard]] const physics::FluidState& fluid() const noexcept { return m_fluid; }
    [[nodiscard]] bool inFluid() const noexcept { return m_fluid.inFluid; }

    /// Noclip debug mode: gravity and collision are both off.
    [[nodiscard]] bool flying() const noexcept { return m_flying; }
    void setFlying(bool flying) noexcept;
    void toggleFly() noexcept { setFlying(!m_flying); }

    [[nodiscard]] bool steppedUp() const noexcept { return m_steppedUp; }
    [[nodiscard]] float lastStepRise() const noexcept { return m_stepRise; }

private:
    void  applyStance(const physics::VoxelCollider& world, float dt) noexcept;
    void  stepFly(float dt) noexcept;
    void  stepWalk(const physics::VoxelCollider& world, float dt) noexcept;
    void  accelerateHorizontal(float dt, float targetSpeed, float acceleration,
                               float friction) noexcept;
    [[nodiscard]] float targetHeight() const noexcept;
    [[nodiscard]] float targetEyeHeight() const noexcept;

    PlayerConfig m_config{};
    PlayerInput  m_input{};

    glm::vec3 m_position{0.0f, static_cast<float>(kSeaLevel) + 2.0f, 0.0f};
    glm::vec3 m_velocity{0.0f};

    /// Start-of-step snapshot, for render interpolation.
    glm::vec3 m_previousPosition{m_position};
    float     m_previousEyeHeight = 1.62f;

    float m_height    = 1.8f;
    float m_eyeHeight = 1.62f;

    float m_yawDegrees   = 0.0f;
    float m_pitchDegrees = 0.0f;

    physics::FluidState m_fluid{};

    bool  m_onGround  = false;
    bool  m_crouching = false;
    bool  m_sprinting = false;
    bool  m_flying    = false;
    bool  m_steppedUp = false;
    float m_stepRise  = 0.0f;
};

}  // namespace voxl
