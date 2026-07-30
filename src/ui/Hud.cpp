#include "ui/Hud.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

#include <imgui.h>

namespace voxl {
namespace {

constexpr float kPi = 3.14159265358979323846f;

[[nodiscard]] ImVec2 toImVec2(const glm::vec2& v) noexcept { return ImVec2{v.x, v.y}; }

[[nodiscard]] ImU32 toColour(const glm::vec4& c) noexcept
{
    return ImGui::ColorConvertFloat4ToU32(ImVec4{c.r, c.g, c.b, c.a});
}

[[nodiscard]] ImU32 withAlpha(ImU32 colour, float alpha) noexcept
{
    const ImU32 masked = colour & ~IM_COL32_A_MASK;
    const auto  scaled = static_cast<ImU32>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    return masked | (scaled << IM_COL32_A_SHIFT);
}

/// Crosshair arm plus its 1 px dark halo. Terrain is both very bright (snow,
/// sand) and very dark (cave walls), so a single-colour crosshair vanishes on
/// one of them; the halo makes it readable on everything.
void drawArm(ImDrawList& list, const ImVec2& min, const ImVec2& max, ImU32 fill, ImU32 halo)
{
    list.AddRectFilled(ImVec2{min.x - 1.0f, min.y - 1.0f}, ImVec2{max.x + 1.0f, max.y + 1.0f}, halo);
    list.AddRectFilled(min, max, fill);
}

}  // namespace

// --------------------------------------------------------------- swatches --

glm::vec4 blockSwatchColour(BlockId id) noexcept
{
    // Hand-picked averages of the intended textures. These exist so the hotbar
    // is legible before the texture array is wired up; once an IconDrawer is
    // installed they are unused.
    switch (id) {
        case blocks::Stone:       return {0.50f, 0.50f, 0.52f, 1.0f};
        case blocks::Dirt:        return {0.45f, 0.32f, 0.21f, 1.0f};
        case blocks::Grass:       return {0.35f, 0.60f, 0.26f, 1.0f};
        case blocks::Sand:        return {0.85f, 0.80f, 0.58f, 1.0f};
        case blocks::Gravel:      return {0.52f, 0.49f, 0.48f, 1.0f};
        case blocks::Water:       return {0.22f, 0.42f, 0.78f, 0.75f};
        case blocks::Wood:        return {0.42f, 0.32f, 0.19f, 1.0f};
        case blocks::Leaves:      return {0.24f, 0.48f, 0.19f, 0.90f};
        case blocks::Planks:      return {0.66f, 0.53f, 0.33f, 1.0f};
        case blocks::Glass:       return {0.75f, 0.87f, 0.92f, 0.45f};
        case blocks::Snow:        return {0.94f, 0.96f, 0.98f, 1.0f};
        case blocks::Sandstone:   return {0.83f, 0.76f, 0.55f, 1.0f};
        case blocks::Cobblestone: return {0.44f, 0.44f, 0.45f, 1.0f};
        case blocks::Bedrock:     return {0.20f, 0.20f, 0.22f, 1.0f};
        case blocks::Glowstone:   return {0.95f, 0.82f, 0.42f, 1.0f};
        case blocks::Clay:        return {0.62f, 0.64f, 0.69f, 1.0f};
        case blocks::Ice:         return {0.62f, 0.80f, 0.94f, 0.65f};
        case blocks::Air:         return {0.0f, 0.0f, 0.0f, 0.0f};
        default:                  return {0.55f, 0.55f, 0.58f, 1.0f};
    }
}

// -------------------------------------------------------------------- Hud --

Hud::Hud(const BlockRegistry* registry) noexcept : m_registry(registry) {}

void Hud::draw(const Hotbar& hotbar, float deltaSeconds)
{
    draw(hotbar, InteractionState{}, deltaSeconds);
}

void Hud::draw(const Hotbar& hotbar, const InteractionState& interaction, float deltaSeconds)
{
    if (ImGui::GetCurrentContext() == nullptr || !m_visible) {
        return;
    }

    updateSelectionLabel(hotbar, deltaSeconds);

    drawCrosshair(interaction);
    drawHotbar(hotbar);
    // Mode first: it claims the row directly above the hotbar, and the transient
    // block name stacks above it rather than through it.
    drawModeLabel(interaction);
    drawSelectionLabel(hotbar);
}

void Hud::updateSelectionLabel(const Hotbar& hotbar, float deltaSeconds)
{
    const BlockId selected = hotbar.selectedBlock();
    if (!m_labelPrimed || selected != m_labelBlock) {
        m_labelBlock  = selected;
        m_labelPrimed = true;
        m_labelTimer  = m_style.nameHoldSeconds + m_style.nameFadeSeconds;
        return;
    }
    m_labelTimer = std::max(m_labelTimer - std::max(deltaSeconds, 0.0f), 0.0f);
}

void Hud::drawCrosshair(const InteractionState& interaction)
{
    ImDrawList* list = ImGui::GetForegroundDrawList();
    if (list == nullptr) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2         centre{viewport->Pos.x + viewport->Size.x * 0.5f,
                        viewport->Pos.y + viewport->Size.y * 0.5f};

    // Feedback, in order of usefulness to the player: amber when the block in
    // the crosshair cannot be built on, bright white when something is in reach,
    // dim white when the ray hits nothing.
    ImU32 fill = IM_COL32(255, 255, 255, 200);
    if (interaction.hasTarget) {
        fill = interaction.placeAllowed ? IM_COL32(255, 255, 255, 255)
                                        : IM_COL32(255, 190, 100, 255);
    }
    const ImU32 halo = IM_COL32(0, 0, 0, 130);

    const float arm       = std::max(m_style.crosshairArm, 1.0f);
    const float halfThick = std::max(m_style.crosshairThickness, 1.0f) * 0.5f;
    const float gap       = std::max(m_style.crosshairGap, 0.0f);

    // Horizontal pair.
    drawArm(*list, ImVec2{centre.x - gap - arm, centre.y - halfThick},
            ImVec2{centre.x - gap, centre.y + halfThick}, fill, halo);
    drawArm(*list, ImVec2{centre.x + gap, centre.y - halfThick},
            ImVec2{centre.x + gap + arm, centre.y + halfThick}, fill, halo);
    // Vertical pair.
    drawArm(*list, ImVec2{centre.x - halfThick, centre.y - gap - arm},
            ImVec2{centre.x + halfThick, centre.y - gap}, fill, halo);
    drawArm(*list, ImVec2{centre.x - halfThick, centre.y + gap},
            ImVec2{centre.x + halfThick, centre.y + gap + arm}, fill, halo);

    if (interaction.targetUnbreakable) {
        // A full dim ring says "this will never break" without animating.
        list->AddCircle(centre, m_style.breakRingRadius, IM_COL32(200, 80, 80, 160), 0,
                        m_style.breakRingThickness * 0.6f);
    } else if (interaction.breakProgress > 0.0f) {
        const float progress = std::clamp(interaction.breakProgress, 0.0f, 1.0f);
        const float radius   = std::max(m_style.breakRingRadius, 2.0f);
        // Track first so the arc reads as filling a gauge rather than growing
        // out of nothing.
        list->AddCircle(centre, radius, IM_COL32(0, 0, 0, 110), 0,
                        m_style.breakRingThickness + 1.0f);
        // Starts at 12 o'clock and sweeps clockwise; ImGui angles are in screen
        // space, where +y is down, so -pi/2 is up.
        const float start = -kPi * 0.5f;
        list->PathArcTo(centre, radius, start, start + progress * 2.0f * kPi);
        list->PathStroke(IM_COL32(255, 235, 190, 235), m_style.breakRingThickness);
    }
}

void Hud::drawHotbar(const Hotbar& hotbar)
{
    ImDrawList* list = ImGui::GetForegroundDrawList();
    if (list == nullptr) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    const float slot    = std::max(m_style.slotSize, 8.0f);
    const float spacing = std::max(m_style.slotSpacing, 0.0f);
    const auto  count   = static_cast<float>(Hotbar::kSlotCount);
    const float width   = count * slot + (count - 1.0f) * spacing;

    const float left = viewport->Pos.x + (viewport->Size.x - width) * 0.5f;
    const float top  = viewport->Pos.y + viewport->Size.y - m_style.bottomMargin - slot;

    m_hotbarTopCentre = glm::vec2{viewport->Pos.x + viewport->Size.x * 0.5f, top};

    // One backing panel behind all nine slots: cheaper than nine and it reads as
    // a single object, which is what makes the selection highlight obvious.
    constexpr float kPanelPad = 4.0f;
    list->AddRectFilled(ImVec2{left - kPanelPad, top - kPanelPad},
                        ImVec2{left + width + kPanelPad, top + slot + kPanelPad},
                        IM_COL32(12, 12, 16, 170), 5.0f);

    const std::size_t selected = hotbar.selectedIndex();

    for (std::size_t i = 0; i < Hotbar::kSlotCount; ++i) {
        const float  x0 = left + static_cast<float>(i) * (slot + spacing);
        const ImVec2 min{x0, top};
        const ImVec2 max{x0 + slot, top + slot};

        list->AddRectFilled(min, max, IM_COL32(38, 38, 44, 190), 3.0f);

        const BlockId id = hotbar.slot(i);
        if (id != blocks::Air) {
            constexpr float kInset = 6.0f;
            const ImVec2    iconMin{min.x + kInset, min.y + kInset};
            const ImVec2    iconMax{max.x - kInset, max.y - kInset};
            if (m_iconDrawer) {
                m_iconDrawer(id, glm::vec2{iconMin.x, iconMin.y}, glm::vec2{iconMax.x, iconMax.y});
            } else {
                const glm::vec4 swatch = blockSwatchColour(id);
                list->AddRectFilled(iconMin, iconMax, toColour(swatch), 2.0f);
                // A darker rim gives the flat swatch enough definition to read
                // as an object against the slot behind it.
                list->AddRect(iconMin, iconMax, IM_COL32(0, 0, 0, 90), 2.0f);
            }
        }

        // Slot number, small and dim: it is a reminder of the key binding, not
        // information the player reads every frame.
        std::array<char, 4> digit{};
        std::snprintf(digit.data(), digit.size(), "%zu", i + 1);
        list->AddText(ImVec2{min.x + 3.0f, min.y + 1.0f}, IM_COL32(220, 220, 230, 130),
                      digit.data());

        if (i == selected) {
            // Selection highlight: an inset white frame plus a soft outer frame.
            // Two rings rather than one thick one, because a single 3 px border
            // eats into the icon at small slot sizes.
            list->AddRect(ImVec2{min.x - 2.0f, min.y - 2.0f}, ImVec2{max.x + 2.0f, max.y + 2.0f},
                          IM_COL32(255, 255, 255, 235), 3.0f, 2.0f);
            list->AddRect(ImVec2{min.x - 4.0f, min.y - 4.0f}, ImVec2{max.x + 4.0f, max.y + 4.0f},
                          IM_COL32(255, 255, 255, 70), 4.0f, 1.0f);
        } else {
            list->AddRect(min, max, IM_COL32(0, 0, 0, 120), 3.0f);
        }
    }
}

void Hud::drawSelectionLabel(const Hotbar& hotbar)
{
    if (m_labelTimer <= 0.0f) {
        return;
    }
    ImDrawList* list = ImGui::GetForegroundDrawList();
    if (list == nullptr) {
        return;
    }

    const float fade  = std::max(m_style.nameFadeSeconds, 0.001f);
    const float alpha = std::clamp(m_labelTimer / fade, 0.0f, 1.0f);

    const BlockId id = hotbar.selectedBlock();
    std::array<char, 64> text{};
    if (m_registry != nullptr) {
        std::snprintf(text.data(), text.size(), "%s", m_registry->get(id).name.c_str());
    } else {
        std::snprintf(text.data(), text.size(), "block %u", static_cast<unsigned>(id));
    }

    const ImVec2 size = ImGui::CalcTextSize(text.data());
    // Row two: clear of the mode label, which owns row one.
    const ImVec2 pos{m_hotbarTopCentre.x - size.x * 0.5f,
                     m_hotbarTopCentre.y - size.y - 34.0f};

    list->AddRectFilled(ImVec2{pos.x - 6.0f, pos.y - 3.0f},
                        ImVec2{pos.x + size.x + 6.0f, pos.y + size.y + 3.0f},
                        withAlpha(IM_COL32(12, 12, 16, 160), alpha * 0.63f), 3.0f);
    list->AddText(pos, withAlpha(IM_COL32(245, 245, 250, 255), alpha), text.data());
}

void Hud::drawModeLabel(const InteractionState& interaction)
{
    ImDrawList* list = ImGui::GetForegroundDrawList();
    if (list == nullptr) {
        return;
    }

    std::array<char, 96> text{};
    if (interaction.miningMode == MiningMode::SubVoxel) {
        std::snprintf(text.data(), text.size(), "%s  r%.1f (%zu sv)%s",
                      miningModeLabel(interaction.miningMode),
                      static_cast<double>(interaction.brushRadius), interaction.brushVolume,
                      interaction.subVoxelFallback ? "  - whole block" : "");
    } else {
        std::snprintf(text.data(), text.size(), "%s", miningModeLabel(interaction.miningMode));
    }

    // Amber when the drill is aimed at something it cannot carve, so the player
    // learns why the swing behaved like a plain break instead of guessing.
    const ImU32 ink = interaction.subVoxelFallback ? IM_COL32(255, 196, 92, 255)
                                                   : IM_COL32(206, 212, 226, 255);

    const ImVec2 size = ImGui::CalcTextSize(text.data());
    const ImVec2 pos{m_hotbarTopCentre.x - size.x * 0.5f,
                     m_hotbarTopCentre.y - size.y - 12.0f};

    list->AddRectFilled(ImVec2{pos.x - 6.0f, pos.y - 3.0f},
                        ImVec2{pos.x + size.x + 6.0f, pos.y + size.y + 3.0f},
                        IM_COL32(12, 12, 16, 110), 3.0f);
    list->AddText(pos, ink, text.data());
}

}  // namespace voxl
