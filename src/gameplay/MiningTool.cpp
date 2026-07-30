#include "gameplay/MiningTool.hpp"

#include <algorithm>
#include <cmath>

namespace voxl {
namespace {

/// Slack on the radius test.
///
/// The interesting radii are exact halves, so `radius * radius` lands on values
/// like 2.25 and 6.25 that a float represents exactly - but the radius also
/// arrives from `adjustBrushRadius`, which accumulates steps, and after enough
/// notches 2.5 is 2.4999998. Without the slack the shell of offsets at exactly
/// the radius would drop in and out depending on how the player got there, and
/// the brush would not be a function of its radius any more.
constexpr float kRadiusEpsilon = 1.0e-4f;

}  // namespace

std::vector<glm::ivec3> makeBrushStencil(float radiusSubVoxels)
{
    const float radius = std::clamp(radiusSubVoxels, MiningTool::kMinBrushRadius,
                                    MiningTool::kMaxBrushRadius);
    const auto  extent = static_cast<std::int32_t>(std::floor(radius));
    const float limit  = radius * radius + kRadiusEpsilon;

    std::vector<glm::ivec3> offsets;
    // Upper bound on a ball inscribed in the (2*extent+1)^3 cube. One
    // reservation beats the four reallocations the default growth would do at
    // the maximum radius.
    const auto span = static_cast<std::size_t>(2 * extent + 1);
    offsets.reserve(span * span * span);

    // y, then z, then x: the ordering of voxl::subVoxelIndex. See the header for
    // why the sequence has to be reproducible and not merely correct.
    for (std::int32_t dy = -extent; dy <= extent; ++dy) {
        for (std::int32_t dz = -extent; dz <= extent; ++dz) {
            for (std::int32_t dx = -extent; dx <= extent; ++dx) {
                const auto squared = static_cast<float>(dx * dx + dy * dy + dz * dz);
                if (squared <= limit) {
                    offsets.push_back(glm::ivec3{dx, dy, dz});
                }
            }
        }
    }
    return offsets;
}

MiningTool::MiningTool() { rebuildStencil(); }

void MiningTool::setBrushRadius(float subVoxels)
{
    const float clamped = std::clamp(subVoxels, kMinBrushRadius, kMaxBrushRadius);
    if (clamped == m_radius) {
        return;
    }
    m_radius = clamped;
    rebuildStencil();
}

void MiningTool::adjustBrushRadius(int steps)
{
    if (steps == 0) {
        return;
    }
    setBrushRadius(m_radius + static_cast<float>(steps) * kBrushRadiusStep);
}

void MiningTool::rebuildStencil()
{
    m_stencil = makeBrushStencil(m_radius);
    m_extent  = static_cast<std::int32_t>(std::floor(m_radius));
}

float miningActionSeconds(MiningMode mode, float hardness, float secondsPerHardness) noexcept
{
    // Clamping rather than asserting: a caller that forgot the bedrock check
    // gets an instant break, which is visibly wrong at the first bedrock layer,
    // instead of a negative timer that never elapses and looks like dropped
    // input.
    const float base = std::max(hardness, 0.0f) * std::max(secondsPerHardness, 0.0f);
    return mode == MiningMode::SubVoxel ? base * MiningTool::kSubVoxelBreakFraction : base;
}

}  // namespace voxl
