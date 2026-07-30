#pragma once

// The mining verb's mode and its brush: the answer to "what does one completed
// swing remove, and how long does it take".
//
// Split out of BlockInteraction on purpose. Everything here is closed-form
// arithmetic over the sub-voxel grid with no world, no renderer and no input
// behind it, which makes it the part of the verb that can be tested
// exhaustively - a brush stencil either matches its radius or it does not.
// BlockInteraction keeps the stateful half: targeting, the timer, and talking
// to the world.
//
// Thread safety: none needed. Main-thread gameplay state, like Hotbar.

#include "world/SubVoxel.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

#include <glm/vec3.hpp>

namespace voxl {

/// What holding the break button does.
///
/// WholeBlock is the default and is byte-for-byte the behaviour that existed
/// before this module: the mining verb is a mode the player opts into, never a
/// silent replacement for breaking blocks.
enum class MiningMode : std::uint8_t {
    /// Hold to destroy the entire targeted block.
    WholeBlock = 0,
    /// Precision drilling: hold to carve the brush out of the block, leaving
    /// the rest of it standing.
    SubVoxel = 1,
};

[[nodiscard]] constexpr const char* toString(MiningMode mode) noexcept
{
    switch (mode) {
        case MiningMode::WholeBlock: return "WholeBlock";
        case MiningMode::SubVoxel:   return "SubVoxel";
    }
    return "Unknown";
}

/// Short player-facing name, for the HUD mode readout.
[[nodiscard]] constexpr const char* miningModeLabel(MiningMode mode) noexcept
{
    switch (mode) {
        case MiningMode::WholeBlock: return "Break";
        case MiningMode::SubVoxel:   return "Drill";
    }
    return "?";
}

/// Sub-voxel offsets covered by a brush of `radiusSubVoxels`, relative to the
/// sub-voxel at the brush centre.
///
/// A EUCLIDEAN BALL, not a cube: an offset is included when
/// `dx^2 + dy^2 + dz^2 <= radius^2`. A cube brush leaves square-cornered bores
/// that read as a bug rather than as a tunnel, and the ball is what makes a
/// radius a meaningful dial - each half-step adds a visible shell.
///
/// The offsets come back in y, then z, then x order, which is the ordering
/// `voxl::subVoxelIndex` uses. Matching it means a carve walks the store the
/// same way the mesher does, and - because a brush is applied one sub-voxel at
/// a time and any of them may be deferred by the world - it means the sequence
/// of edits is reproducible, which a save file compared across runs depends on.
///
/// Radii below one sub-voxel still return the single centre offset: a brush
/// that removes nothing would be a mode that appears broken.
[[nodiscard]] std::vector<glm::ivec3> makeBrushStencil(float radiusSubVoxels);

/// Seconds of held-button time one completed action in `mode` costs against a
/// block of `hardness`. `secondsPerHardness` is the whole-block scale, i.e.
/// BlockInteraction::kBreakSecondsPerHardness.
///
/// Free rather than a MiningTool member because the EFFECTIVE mode of a swing
/// is not always the tool's mode: a drill aimed at a material that cannot be
/// carved falls back to a whole-block break and must be timed as one.
///
/// UNBREAKABLE IS NOT HANDLED HERE. Callers must reject `hardness < 0` before
/// asking: "how long does never take" has no useful answer, and returning
/// infinity would turn a missing bedrock check into a hang that looks like
/// dropped input instead of into an obvious bug. Negative hardness clamps to an
/// instant action, which is loudly wrong at the first bedrock layer.
[[nodiscard]] float miningActionSeconds(MiningMode mode, float hardness,
                                        float secondsPerHardness) noexcept;

class MiningTool {
public:
    /// Brush radius bounds, in sub-voxels. The floor is half a sub-voxel, which
    /// is the smallest brush that still covers its own centre. The ceiling is
    /// four - a diameter of one whole block - because past that the "precision"
    /// mode is just a slower way to break blocks and the per-swing edit count
    /// (a radius-4 ball is 257 sub-voxels) starts to matter against
    /// World::kMaxDeferredEdits.
    static constexpr float kMinBrushRadius     = 0.5f;
    static constexpr float kMaxBrushRadius     = 4.0f;
    static constexpr float kDefaultBrushRadius = 1.0f;

    /// One notch of the brush dial. Half-steps are what make the radius useful:
    /// the integer radii alone jump 1 -> 7 -> 33 sub-voxels.
    static constexpr float kBrushRadiusStep = 0.5f;

    /// Cost of one carve as a fraction of a whole-block break of the same
    /// material.
    ///
    /// Not 1/512 (the volume a single sub-voxel represents) and not 1 either.
    /// A default brush removes 7 of 512 sub-voxels, so at 1/8 of a full break
    /// hollowing a block out completely costs roughly nine times what simply
    /// breaking it would. That is the trade the mode exists to offer - precision
    /// is slower - and it keeps a carve short enough (stone: ~94 ms) that
    /// holding the button reads as a continuous drill rather than a series of
    /// discrete swings.
    static constexpr float kSubVoxelBreakFraction = 0.125f;

    MiningTool();

    [[nodiscard]] MiningMode mode() const noexcept { return m_mode; }
    void setMode(MiningMode mode) noexcept { m_mode = mode; }

    /// Flips the mode and returns the new one.
    MiningMode toggleMode() noexcept
    {
        m_mode = m_mode == MiningMode::WholeBlock ? MiningMode::SubVoxel : MiningMode::WholeBlock;
        return m_mode;
    }

    [[nodiscard]] float brushRadius() const noexcept { return m_radius; }

    /// Clamped to [kMinBrushRadius, kMaxBrushRadius]; the stencil is rebuilt
    /// only when the radius actually moves.
    void setBrushRadius(float subVoxels);

    /// Wheel-style relative control. Positive widens the brush.
    void adjustBrushRadius(int steps);

    /// Offsets the current brush covers. Stable until the radius changes.
    [[nodiscard]] const std::vector<glm::ivec3>& stencil() const noexcept { return m_stencil; }

    /// Number of sub-voxels one swing touches.
    [[nodiscard]] std::size_t brushVolume() const noexcept { return m_stencil.size(); }

    /// Largest offset the brush reaches on any axis, in sub-voxels. The visual
    /// feedback uses it to size the brush preview box.
    [[nodiscard]] std::int32_t brushExtent() const noexcept { return m_extent; }

    /// `miningActionSeconds` for this tool's current mode.
    [[nodiscard]] float actionSeconds(float hardness, float secondsPerHardness) const noexcept
    {
        return miningActionSeconds(m_mode, hardness, secondsPerHardness);
    }

private:
    void rebuildStencil();

    MiningMode m_mode   = MiningMode::WholeBlock;
    float      m_radius = kDefaultBrushRadius;
    /// Cached because a swing walks it every frame the button is held and the
    /// radius changes only when the player turns the dial.
    std::vector<glm::ivec3> m_stencil;
    std::int32_t            m_extent = 0;
};

}  // namespace voxl
