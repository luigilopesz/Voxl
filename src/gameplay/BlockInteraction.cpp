#include "gameplay/BlockInteraction.hpp"

#include <glad/gl.h>

#include "core/Log.hpp"
#include "physics/SubVoxelAccess.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

#include <glm/gtc/type_ptr.hpp>

namespace voxl {
namespace {

/// The outline is pushed this far outside the block on every axis. Without it
/// the outline is coplanar with the block face and z-fights into a dashed mess
/// at grazing angles; 1/512 of a block is invisible to the eye but larger than
/// the depth buffer's resolution at the far plane.
constexpr float kOutlineInflate = 0.002f;

/// How much of the block the break box has shrunk away by the time it gives up.
/// Not 1.0: a box that collapses to a point disappears before the break lands,
/// which reads as a dropped input.
constexpr float kBreakShrink = 0.80f;

[[nodiscard]] bool boxesOverlap(const Aabb& a, const Aabb& b) noexcept
{
    // Strict inequalities: a block whose top face is exactly the player's feet
    // plane is touching, not intersecting, and must remain placeable.
    return a.min.x < b.max.x && a.max.x > b.min.x && a.min.y < b.max.y && a.max.y > b.min.y &&
           a.min.z < b.max.z && a.max.z > b.min.z;
}

[[nodiscard]] Aabb blockBox(const BlockPos& position) noexcept
{
    const glm::vec3 lo{static_cast<float>(position.x), static_cast<float>(position.y),
                       static_cast<float>(position.z)};
    return Aabb{lo, lo + glm::vec3{1.0f}};
}

// ------------------------------------------------------------ GL shim ------

/// True when the GL loader has populated the function-pointer table.
///
/// glad resolves the whole core profile in one pass, so a handful of probes is
/// as informative as testing all thirty entry points this file uses. Without
/// this guard, calling `render()` before the loader has run (headless tests,
/// early boot, a failed context creation) dereferences a null function pointer
/// instead of degrading to "no selection box".
[[nodiscard]] bool selectionGlAvailable() noexcept
{
    return glCreateProgram != nullptr && glGenVertexArrays != nullptr &&
           glDrawElements != nullptr && glGetIntegerv != nullptr;
}

constexpr const char* kVertexSource = R"(#version 450 core
layout(location = 0) in vec3 aCorner;   // unit cube, components in {0,1}
uniform mat4 uViewProjection;
uniform vec3 uOrigin;
uniform vec3 uScale;
void main()
{
    gl_Position = uViewProjection * vec4(uOrigin + aCorner * uScale, 1.0);
}
)";

constexpr const char* kFragmentSource = R"(#version 450 core
uniform vec4 uColour;
out vec4 oColour;
void main()
{
    oColour = uColour;
}
)";

}  // namespace

// ---------------------------------------------------------------------------
//  SelectionRenderer
// ---------------------------------------------------------------------------

/// Self-contained line renderer for the selection and break boxes.
///
/// Deliberately does not use the engine's Shader/Mesh abstractions: this module
/// has to build before the render module exists, and a 12-edge wireframe is not
/// worth a dependency. It is a handful of GL objects and one 24-index draw.
///
/// Thread safety: main thread only, like every other GL user.
class BlockInteraction::SelectionRenderer {
public:
    SelectionRenderer() = default;

    ~SelectionRenderer() { release(); }

    SelectionRenderer(const SelectionRenderer&)            = delete;
    SelectionRenderer& operator=(const SelectionRenderer&) = delete;
    SelectionRenderer(SelectionRenderer&&)                 = delete;
    SelectionRenderer& operator=(SelectionRenderer&&)      = delete;

    /// Idempotent. Returns false once and stays false when GL is unusable, so a
    /// broken context costs one failed attempt rather than one per frame.
    bool ensureCreated()
    {
        if (m_ready) {
            return true;
        }
        if (m_failed) {
            return false;
        }
        m_failed = true;  // cleared on success below

        if (!selectionGlAvailable()) {
            VOXL_LOG_WARN("selection box disabled: OpenGL entry points not loaded");
            return false;
        }

        const GLuint vertexShader   = compile(GL_VERTEX_SHADER, kVertexSource);
        const GLuint fragmentShader = compile(GL_FRAGMENT_SHADER, kFragmentSource);
        if (vertexShader == 0 || fragmentShader == 0) {
            if (vertexShader != 0) {
                glDeleteShader(vertexShader);
            }
            if (fragmentShader != 0) {
                glDeleteShader(fragmentShader);
            }
            return false;
        }

        m_program = glCreateProgram();
        glAttachShader(m_program, vertexShader);
        glAttachShader(m_program, fragmentShader);
        glLinkProgram(m_program);
        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);

        GLint linked = GL_FALSE;
        glGetProgramiv(m_program, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            std::array<GLchar, 512> log{};
            GLsizei                 length = 0;
            glGetProgramInfoLog(m_program, static_cast<GLsizei>(log.size()), &length, log.data());
            VOXL_LOG_ERROR("selection box program link failed: {}", log.data());
            glDeleteProgram(m_program);
            m_program = 0;
            return false;
        }

        m_viewProjectionLocation = glGetUniformLocation(m_program, "uViewProjection");
        m_originLocation         = glGetUniformLocation(m_program, "uOrigin");
        m_scaleLocation          = glGetUniformLocation(m_program, "uScale");
        m_colourLocation         = glGetUniformLocation(m_program, "uColour");

        // Unit cube corners; bit 0 is x, bit 1 is y, bit 2 is z, so the index of
        // a corner is also its coordinate triple.
        constexpr std::array<float, 8 * 3> kCorners{
            0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
        };
        constexpr std::array<GLushort, 24> kEdges{
            0, 1, 1, 3, 3, 2, 2, 0,  // z = 0 face
            4, 5, 5, 7, 7, 6, 6, 4,  // z = 1 face
            0, 4, 1, 5, 2, 6, 3, 7,  // connecting edges
        };

        glGenVertexArrays(1, &m_vao);
        glBindVertexArray(m_vao);

        glGenBuffers(1, &m_vertexBuffer);
        glBindBuffer(GL_ARRAY_BUFFER, m_vertexBuffer);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(kCorners)), kCorners.data(),
                     GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * static_cast<GLsizei>(sizeof(float)),
                              nullptr);

        glGenBuffers(1, &m_indexBuffer);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_indexBuffer);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(sizeof(kEdges)), kEdges.data(),
                     GL_STATIC_DRAW);

        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        // Wide lines were REMOVED from the forward-compatible core profile, and
        // GL_ALIASED_LINE_WIDTH_RANGE does not say so: NVIDIA still reports
        // [1, 10] there and then rejects glLineWidth(2.0) with GL_INVALID_VALUE.
        // Asking the range is therefore not a guard at all - it produced one
        // GL error per frame for every frame the player had a block targeted,
        // which is most of them. GL_CONTEXT_FLAG_FORWARD_COMPATIBLE_BIT is the
        // property that actually decides it, so ask that instead and only trust
        // the range on a context where wide lines still exist.
        GLint contextFlags = 0;
        glGetIntegerv(GL_CONTEXT_FLAGS, &contextFlags);
        if ((contextFlags & GL_CONTEXT_FLAG_FORWARD_COMPATIBLE_BIT) != 0) {
            m_lineWidth = 1.0f;
        } else {
            std::array<GLfloat, 2> widthRange{1.0f, 1.0f};
            glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, widthRange.data());
            m_lineWidth = std::clamp(2.0f, widthRange[0], std::max(widthRange[0], widthRange[1]));
        }

        m_ready  = true;
        m_failed = false;
        return true;
    }

    void release() noexcept
    {
        if (glDeleteProgram == nullptr) {
            // The context is already gone; the driver reclaimed these with it.
            m_program = m_vao = m_vertexBuffer = m_indexBuffer = 0;
            m_ready = false;
            return;
        }
        if (m_indexBuffer != 0) {
            glDeleteBuffers(1, &m_indexBuffer);
        }
        if (m_vertexBuffer != 0) {
            glDeleteBuffers(1, &m_vertexBuffer);
        }
        if (m_vao != 0) {
            glDeleteVertexArrays(1, &m_vao);
        }
        if (m_program != 0) {
            glDeleteProgram(m_program);
        }
        m_program = m_vao = m_vertexBuffer = m_indexBuffer = 0;
        m_ready   = false;
    }

    /// Binds state, invokes `emit` (which calls `drawBox` any number of times),
    /// then restores the GL state that was captured on entry. Restoring rather
    /// than assuming is what keeps this from breaking whichever renderer draws
    /// next; it costs a few glGet calls once per frame.
    template <typename EmitFn>
    void draw(const glm::mat4& viewProjection, EmitFn&& emit)
    {
        // Captured so the callback can toggle depth testing per box and this
        // function still restores the caller's setting on exit.
        GLint previousProgram = 0;
        GLint previousVao     = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVao);
        const GLboolean depthTestWasOn = glIsEnabled(GL_DEPTH_TEST);
        const GLboolean blendWasOn     = glIsEnabled(GL_BLEND);
        const GLboolean cullWasOn      = glIsEnabled(GL_CULL_FACE);
        GLboolean       depthWriteWasOn = GL_TRUE;
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWriteWasOn);
        GLfloat previousLineWidth = 1.0f;
        glGetFloatv(GL_LINE_WIDTH, &previousLineWidth);

        glUseProgram(m_program);
        glBindVertexArray(m_vao);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_CULL_FACE);
        // No depth writes: the outline must not punch a hole that later
        // translucent geometry then depth-fails against.
        glDepthMask(GL_FALSE);
        glLineWidth(m_lineWidth);
        glUniformMatrix4fv(m_viewProjectionLocation, 1, GL_FALSE, glm::value_ptr(viewProjection));

        emit(*this);

        glDepthMask(depthWriteWasOn);
        glLineWidth(previousLineWidth);
        if (cullWasOn == GL_TRUE) {
            glEnable(GL_CULL_FACE);
        }
        if (blendWasOn != GL_TRUE) {
            glDisable(GL_BLEND);
        }
        if (depthTestWasOn != GL_TRUE) {
            glDisable(GL_DEPTH_TEST);
        }
        glBindVertexArray(static_cast<GLuint>(previousVao));
        glUseProgram(static_cast<GLuint>(previousProgram));
    }

    /// Only legal from inside a `draw()` callback. `draw()` restores whatever
    /// the caller had set.
    void setDepthTest(bool enabled) const
    {
        if (enabled) {
            glEnable(GL_DEPTH_TEST);
        } else {
            glDisable(GL_DEPTH_TEST);
        }
    }

    /// Only legal from inside a `draw()` callback.
    void drawBox(const glm::vec3& origin, const glm::vec3& size, const glm::vec4& colour) const
    {
        glUniform3f(m_originLocation, origin.x, origin.y, origin.z);
        glUniform3f(m_scaleLocation, size.x, size.y, size.z);
        glUniform4f(m_colourLocation, colour.r, colour.g, colour.b, colour.a);
        glDrawElements(GL_LINES, 24, GL_UNSIGNED_SHORT, nullptr);
    }

    [[nodiscard]] bool ready() const noexcept { return m_ready; }

private:
    [[nodiscard]] static GLuint compile(GLenum stage, const char* source)
    {
        const GLuint shader = glCreateShader(stage);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);

        GLint compiled = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (compiled != GL_TRUE) {
            std::array<GLchar, 512> log{};
            GLsizei                 length = 0;
            glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), &length, log.data());
            VOXL_LOG_ERROR("selection box shader compile failed: {}", log.data());
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    }

    GLuint m_program      = 0;
    GLuint m_vao          = 0;
    GLuint m_vertexBuffer = 0;
    GLuint m_indexBuffer  = 0;

    GLint m_viewProjectionLocation = -1;
    GLint m_originLocation         = -1;
    GLint m_scaleLocation          = -1;
    GLint m_colourLocation         = -1;

    GLfloat m_lineWidth = 1.0f;
    bool    m_ready     = false;
    bool    m_failed    = false;
};

// ---------------------------------------------------------------------------
//  BlockInteraction
// ---------------------------------------------------------------------------

BlockInteraction::BlockInteraction(const BlockRegistry& registry) noexcept : m_registry(&registry) {}

BlockInteraction::~BlockInteraction() = default;

void BlockInteraction::setRaycaster(RaycastFn raycaster) { m_raycast = std::move(raycaster); }
void BlockInteraction::setBlockWriter(BlockWriteFn writer) { m_write = std::move(writer); }
void BlockInteraction::setBlockReader(BlockReadFn reader) { m_read = std::move(reader); }
void BlockInteraction::setSubVoxelBreaker(SubVoxelBreakFn carve) { m_carve = std::move(carve); }
void BlockInteraction::setSubVoxelDamageReader(SubVoxelDamagedFn damaged)
{
    m_damaged = std::move(damaged);
}

void BlockInteraction::setMiningMode(MiningMode mode) noexcept
{
    if (m_tool.mode() == mode) {
        return;
    }
    m_tool.setMode(mode);
    // See BreakTarget: a swing belongs to a (target, mode) pair.
    resetBreakProgress();
}

MiningMode BlockInteraction::toggleMiningMode() noexcept
{
    setMiningMode(m_tool.mode() == MiningMode::WholeBlock ? MiningMode::SubVoxel
                                                          : MiningMode::WholeBlock);
    return m_tool.mode();
}

void BlockInteraction::setBrushRadius(float subVoxels) noexcept
{
    const float before = m_tool.brushRadius();
    m_tool.setBrushRadius(subVoxels);
    if (m_tool.brushRadius() != before) {
        // Widening the brush mid-swing would remove sub-voxels the player had
        // not aimed at when they pressed the button.
        resetBreakProgress();
    }
}

void BlockInteraction::adjustBrushRadius(int steps) noexcept
{
    setBrushRadius(m_tool.brushRadius() + static_cast<float>(steps) * MiningTool::kBrushRadiusStep);
}

void BlockInteraction::setReach(float blocks) noexcept
{
    // A zero or negative reach would make every ray query degenerate; clamp
    // rather than assert so a config typo does not take the game down.
    m_reach = std::clamp(blocks, 0.5f, 64.0f);
}

void BlockInteraction::setPlayerAabb(const Aabb& box) noexcept
{
    m_playerAabb    = box;
    m_hasPlayerAabb = true;
}

void BlockInteraction::clearPlayerAabb() noexcept { m_hasPlayerAabb = false; }

void BlockInteraction::resetBreakProgress() noexcept
{
    m_breakingValid       = false;
    m_breakElapsed        = 0.0f;
    m_state.breakProgress = 0.0f;
    m_state.breakStage    = -1;
}

void BlockInteraction::updateSubTarget() noexcept
{
    m_state.hasSubTarget = false;
    m_state.subTarget    = glm::ivec3{0};
    if (!m_state.hasTarget) {
        return;
    }

    // The hit point lies exactly ON the block face, where the scaled sub-voxel
    // coordinate is 0 or 8 and a rounding error can land it on either side.
    // Stepping half a sub-voxel along the inward normal puts it unambiguously
    // inside the block the ray actually hit; `subVoxelOfPoint` then clamps, so
    // even a pathological hit point cannot address a neighbour's grid.
    const glm::vec3 inward =
        -kDirectionNormals[static_cast<std::size_t>(m_state.hit.face)] * (0.5f * kSubVoxelSize);
    m_state.subTarget = physics::subVoxelOfPoint(m_state.hit.point + inward, m_state.hit.block);
    m_state.hasSubTarget = true;
}

bool BlockInteraction::canCarveTarget() const noexcept
{
    if (m_tool.mode() != MiningMode::SubVoxel || !m_carve || !m_state.hasSubTarget) {
        return false;
    }
    // World::editSubVoxel refuses anything that is not RenderLayer::Opaque - the
    // sub-voxel pass has no alpha cutoff, so a chipped pane of glass would come
    // back as a solid one. Asking the registry the same question here rather
    // than discovering it from a refusal is what lets the fallback happen on the
    // frame the player aims at the glass, instead of after a whole brush of
    // rejected edits that changed nothing and left the button feeling dead.
    return m_registry->renderLayer(m_state.hit.blockId) == RenderLayer::Opaque;
}

void BlockInteraction::noteFallback(const BlockPos& block, BlockId id)
{
    // Keyed on the block, because this runs every frame the button is held.
    if (m_loggedFallback && m_loggedFallbackBlock == block) {
        return;
    }
    m_loggedFallbackBlock = block;
    m_loggedFallback      = true;
    VOXL_LOG_DEBUG("drill: {} cannot be carved (not opaque); breaking the whole block instead",
                   m_registry->get(id).name);
}

std::size_t BlockInteraction::applyBrush()
{
    if (!m_carve || !m_state.hasSubTarget) {
        return 0;
    }

    const physics::SubVoxelCoord centre =
        physics::toGlobalSubVoxel(m_state.hit.block, m_state.subTarget);

    std::size_t carved = 0;
    for (const glm::ivec3& offset : m_tool.stencil()) {
        const physics::SubVoxelCoord cell = centre.offset(offset.x, offset.y, offset.z);

        // A brush is a ball, so it spills across block boundaries whenever the
        // player drills near a face - which is most of the time, and is the
        // point: a bore has to be able to cross into the next block. Skipping
        // the cells that cannot be carved here rather than letting the world
        // refuse them keeps the refusal off the world's deferral queue and makes
        // the returned count mean what it says.
        if (m_read) {
            const BlockId id = m_read(cell.block());
            if (id == blocks::Air || m_registry->renderLayer(id) != RenderLayer::Opaque) {
                continue;
            }
        }

        // The centre of the cell: the one point inside it that cannot floor into
        // a neighbour whichever way the arithmetic rounds.
        const glm::vec3 point{(static_cast<float>(cell.x) + 0.5f) * kSubVoxelSize,
                              (static_cast<float>(cell.y) + 0.5f) * kSubVoxelSize,
                              (static_cast<float>(cell.z) + 0.5f) * kSubVoxelSize};
        if (m_carve(point) == CarveOutcome::Carved) {
            ++carved;
        }
    }
    return carved;
}

PlaceResult BlockInteraction::evaluatePlacement(const BlockPos& target) const noexcept
{
    // `Placed` here means "nothing objects"; the write has not happened yet.
    if (m_heldBlock == blocks::Air) {
        return PlaceResult::NothingHeld;
    }
    if (!isInsideWorld(target)) {
        return PlaceResult::OutOfWorld;
    }
    if (!m_write) {
        return PlaceResult::NoWorldSink;
    }

    // Checked before the block id, because a partially destroyed cell still
    // reads as its original block and would otherwise report the vaguer
    // `Occupied`. The rule itself matters more than the message: writing a whole
    // block over a damaged one DISCARDS its sub-voxel grid - World::writeBlock
    // does that deliberately, because a grid whose material no longer matches
    // ChunkStorage violates the SubVoxel.hpp invariant - so a stray right click
    // would silently erase a bore the player just spent ten seconds drilling.
    if (m_damaged && m_damaged(target)) {
        return PlaceResult::Damaged;
    }

    // Without a reader we cannot know what is in the cell. Being permissive is
    // the right failure mode: the world's own setBlock is the authority, and a
    // build with no reader wired up is a build that is still coming together.
    if (m_read) {
        const BlockId  existing = m_read(target);
        const BlockType& type   = m_registry->get(existing);
        // Liquids are replaceable for building even though the frozen contract
        // reserves BlockType::replaceable for air alone.
        if (!type.replaceable && !type.liquid) {
            return PlaceResult::Occupied;
        }
    }

    // The rule that stops a player sealing themselves inside a block.
    if (m_hasPlayerAabb && boxesOverlap(blockBox(target), m_playerAabb)) {
        return PlaceResult::IntersectsPlayer;
    }
    return PlaceResult::Placed;
}

void BlockInteraction::update(const Camera& camera, const InteractionInput& input,
                              float deltaSeconds)
{
    m_state.lastPlace        = PlaceResult::None;
    m_state.lastBreak        = BreakResult::None;
    m_state.lastCarveCount   = 0;
    m_state.subVoxelFallback = false;

    // Mirrored every frame so the HUD reads one snapshot rather than reaching
    // into the tool.
    m_state.miningMode  = m_tool.mode();
    m_state.brushRadius = m_tool.brushRadius();
    m_state.brushVolume = m_tool.brushVolume();

    const float dt = std::max(deltaSeconds, 0.0f);

    // ---- target selection ----
    InteractionHit hit{};
    bool           hasTarget = false;
    if (m_raycast) {
        hasTarget = m_raycast(camera.position(), camera.forward(), m_reach, hit);
    }

    m_state.hasTarget = hasTarget;
    m_state.hit       = hasTarget ? hit : InteractionHit{};

    const BlockType& targetType = m_registry->get(m_state.hit.blockId);
    m_state.targetUnbreakable   = hasTarget && targetType.hardness < 0.0f;

    updateSubTarget();

    if (hasTarget) {
        m_state.placeTarget  = neighbour(hit.block, hit.face);
        m_state.placeAllowed = evaluatePlacement(m_state.placeTarget) == PlaceResult::Placed;
    } else {
        m_state.placeTarget  = BlockPos{};
        m_state.placeAllowed = false;
    }

    // ---- break ----
    if (!input.breakHeld) {
        resetBreakProgress();
    } else if (!hasTarget) {
        m_state.lastBreak = BreakResult::NoTarget;
        resetBreakProgress();
    } else if (m_state.targetUnbreakable) {
        m_state.lastBreak = BreakResult::Unbreakable;
        resetBreakProgress();
    } else if (!m_write) {
        m_state.lastBreak = BreakResult::NoWorldSink;
        resetBreakProgress();
    } else {
        // Whether THIS swing drills or breaks. The tool's mode is only a
        // request: a material the sub-voxel pass cannot draw falls back to a
        // whole-block break rather than refusing to do anything, so the button
        // never feels dead.
        const bool carving = canCarveTarget();
        m_state.subVoxelFallback = m_tool.mode() == MiningMode::SubVoxel && !carving;
        if (m_state.subVoxelFallback && m_carve) {
            noteFallback(hit.block, hit.blockId);
        }

        // Looking away restarts the timer: progress belongs to the target, never
        // to a global "time spent holding the button".
        const BreakTarget target{hit.block, m_state.subTarget, carving};
        if (!m_breakingValid || !(m_breakTarget == target)) {
            m_breakTarget   = target;
            m_breakingValid = true;
            m_breakElapsed  = 0.0f;
        }
        m_breakElapsed += dt;

        // Hardness drives both modes; a carve is a fixed fraction of a full
        // break of the same material. The fallback is timed as the whole-block
        // break it actually is, which is why the effective mode is passed rather
        // than the tool's.
        const float total = miningActionSeconds(
            carving ? MiningMode::SubVoxel : MiningMode::WholeBlock, targetType.hardness,
            kBreakSecondsPerHardness);

        if (total <= 0.0f || m_breakElapsed >= total) {
            if (carving) {
                const std::size_t carved = applyBrush();
                m_state.lastCarveCount   = carved;
                m_state.lastBreak = carved > 0 ? BreakResult::Carved : BreakResult::CarveRefused;
            } else {
                const bool written = m_write(hit.block, blocks::Air);
                m_state.lastBreak  = written ? BreakResult::Broken : BreakResult::WriteFailed;
            }
            // Clearing the timer is what makes a held button drill continuously:
            // the next frame starts a fresh swing against whatever is left.
            resetBreakProgress();
            // Show the effect fully formed on the frame the target gives way,
            // otherwise an instant-break block never renders a break state.
            m_state.breakProgress = 1.0f;
            m_state.breakStage    = kBreakStageCount - 1;
        } else {
            const float progress  = std::clamp(m_breakElapsed / total, 0.0f, 1.0f);
            m_state.breakProgress = progress;
            m_state.breakStage =
                std::min(static_cast<int>(progress * static_cast<float>(kBreakStageCount)),
                         kBreakStageCount - 1);
            m_state.lastBreak = BreakResult::InProgress;
        }
    }

    // ---- place ----
    m_placeCooldown = std::max(m_placeCooldown - dt, 0.0f);
    if (!input.placeHeld && !input.placePressed) {
        // Releasing the button must make the next press instant rather than
        // waiting out the leftover repeat interval.
        m_placeCooldown = 0.0f;
    }

    const bool wantsPlace = input.placePressed || (input.placeHeld && m_placeCooldown <= 0.0f);
    if (wantsPlace) {
        if (!hasTarget) {
            m_state.lastPlace = PlaceResult::NoTarget;
        } else {
            const PlaceResult verdict = evaluatePlacement(m_state.placeTarget);
            if (verdict != PlaceResult::Placed) {
                m_state.lastPlace = verdict;
            } else {
                m_state.lastPlace = m_write(m_state.placeTarget, m_heldBlock)
                                        ? PlaceResult::Placed
                                        : PlaceResult::WriteFailed;
            }
        }
        m_placeCooldown = kPlaceRepeatSeconds;
    }
}

void BlockInteraction::render(const glm::mat4& viewProjection)
{
    if (!m_selectionVisible || !m_state.hasTarget) {
        return;
    }
    if (!m_selection) {
        m_selection = std::make_unique<SelectionRenderer>();
    }
    if (!m_selection->ensureCreated()) {
        return;
    }

    const glm::vec3 blockOrigin{static_cast<float>(m_state.hit.block.x),
                                static_cast<float>(m_state.hit.block.y),
                                static_cast<float>(m_state.hit.block.z)};
    const float progress = m_state.breakProgress;

    // A drill that has fallen back to a whole-block break must not draw a brush:
    // showing a footprint the swing is not going to use is worse than showing
    // nothing, because it tells the player the material can be carved.
    const bool drilling = m_state.miningMode == MiningMode::SubVoxel &&
                          !m_state.subVoxelFallback && m_state.hasSubTarget;

    // Bounds of the brush footprint, in world units. The stencil is a ball, so
    // this box is its bounding cube - close enough to communicate reach, and one
    // draw instead of up to 257.
    glm::vec3 brushOrigin{0.0f};
    glm::vec3 brushSize{0.0f};
    if (drilling) {
        const auto      extent = static_cast<float>(m_tool.brushExtent());
        const glm::vec3 sub{static_cast<float>(m_state.subTarget.x),
                            static_cast<float>(m_state.subTarget.y),
                            static_cast<float>(m_state.subTarget.z)};
        brushOrigin = blockOrigin + (sub - glm::vec3{extent}) * kSubVoxelSize;
        brushSize   = glm::vec3{(2.0f * extent + 1.0f) * kSubVoxelSize};
    }

    // What the shrinking progress box collapses into: the brush when drilling,
    // the whole block otherwise. The box has to describe the thing that is about
    // to disappear, or progress reads as a whole-block break in drill mode.
    const glm::vec3 targetOrigin = drilling ? brushOrigin : blockOrigin;
    const glm::vec3 targetSize   = drilling ? brushSize : glm::vec3{1.0f};

    const bool unbreakable = m_state.targetUnbreakable;

    m_selection->draw(viewProjection, [&](const SelectionRenderer& renderer) {
        // Dull red for a block that will never give way. Without it a player
        // holding the button on bedrock gets no feedback whatsoever, because the
        // break timer legitimately never starts and the outline looks the same
        // as it does on stone.
        const glm::vec4 outline = unbreakable ? glm::vec4{0.60f, 0.10f, 0.10f, 0.85f}
                                              : glm::vec4{0.04f, 0.04f, 0.04f, 0.75f};
        renderer.drawBox(blockOrigin - glm::vec3{kOutlineInflate},
                         glm::vec3{1.0f + 2.0f * kOutlineInflate}, outline);

        // Depth testing is off for every box below. They live *inside* a solid
        // voxel, so a depth-tested draw would be entirely occluded by the very
        // block it is describing. Nothing can be between the eye and the
        // targeted block along the view ray - that is what made it the target -
        // so drawing them unconditionally is safe.
        if (drilling) {
            // The exact footprint of the next swing, which is what turns the
            // brush radius from a number in the HUD into a dial the player aims.
            renderer.setDepthTest(false);
            renderer.drawBox(brushOrigin - glm::vec3{kOutlineInflate},
                             brushSize + glm::vec3{2.0f * kOutlineInflate},
                             glm::vec4{0.30f, 0.85f, 1.0f, 0.75f});
            renderer.setDepthTest(true);
        }

        if (progress > 0.0f) {
            // Shrinking box: the visual stand-in for a crack overlay until the
            // texture array carries the crack strip.
            const float     scale  = 1.0f - kBreakShrink * progress;
            const glm::vec3 size   = targetSize * scale;
            const glm::vec3 origin = targetOrigin + targetSize * 0.5f - size * 0.5f;
            // Cools white -> orange as the target gives way, so progress reads
            // even when the box is small.
            const glm::vec4 colour{1.0f, 0.85f - 0.45f * progress, 0.55f - 0.45f * progress, 0.9f};
            renderer.setDepthTest(false);
            renderer.drawBox(origin, size, colour);
            renderer.setDepthTest(true);
        }
    });
}

void BlockInteraction::releaseGpuResources() noexcept { m_selection.reset(); }

}  // namespace voxl
