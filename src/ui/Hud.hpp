#pragma once

// The in-world HUD: crosshair, hotbar, and interaction feedback.
//
// Drawn through ImGui's foreground draw list rather than as ImGui widgets. The
// HUD must never take keyboard/mouse focus - a hotbar slot that swallows the
// click meant for the block behind it is a bug the player will never forgive -
// and immediate geometry on the foreground list cannot.
//
// Block icons: until the block texture array is available as an ImTextureID the
// slots show a flat colour swatch from `blockSwatchColour()`. Supplying an
// `IconDrawer` replaces that with real artwork without touching this file.
//
// Thread safety: none. Main thread only (ImGui).

#include "gameplay/BlockInteraction.hpp"
#include "gameplay/Hotbar.hpp"
#include "world/Block.hpp"

#include <functional>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

namespace voxl {

/// Representative RGBA colour for a block, used for the placeholder hotbar
/// swatches. Unknown ids get a neutral grey rather than asserting, so a HUD
/// built against a wider registry still draws.
[[nodiscard]] glm::vec4 blockSwatchColour(BlockId id) noexcept;

class Hud {
public:
    struct Style {
        /// Crosshair: four arms with a hole in the middle, so the exact pixel
        /// being targeted is never covered by the crosshair itself.
        float crosshairArm       = 7.0f;
        float crosshairThickness = 2.0f;
        float crosshairGap       = 3.0f;
        /// Radius of the break-progress ring drawn around the crosshair.
        float breakRingRadius    = 14.0f;
        float breakRingThickness = 3.0f;

        float slotSize     = 46.0f;
        float slotSpacing  = 4.0f;
        float bottomMargin = 18.0f;

        /// How long the selected block's name stays on screen after a change.
        float nameHoldSeconds = 1.2f;
        float nameFadeSeconds = 0.6f;
    };

    /// Draws a block icon into a screen-space rectangle. Coordinates are ImGui
    /// screen space (pixels, origin top-left).
    using IconDrawer =
        std::function<void(BlockId id, const glm::vec2& topLeft, const glm::vec2& bottomRight)>;

    /// `registry` supplies block names for the selection label; nullptr falls
    /// back to the numeric id.
    explicit Hud(const BlockRegistry* registry = nullptr) noexcept;

    [[nodiscard]] const Style& style() const noexcept { return m_style; }
    void setStyle(const Style& style) noexcept { m_style = style; }

    void setIconDrawer(IconDrawer drawer) { m_iconDrawer = std::move(drawer); }

    [[nodiscard]] bool visible() const noexcept { return m_visible; }
    void setVisible(bool visible) noexcept { m_visible = visible; }

    /// Full HUD with interaction feedback. Call once per frame between
    /// `ImGui::NewFrame()` and `ImGui::Render()`. Inert when there is no ImGui
    /// context, which is what makes it safe to call before the UI backend is up.
    void draw(const Hotbar& hotbar, const InteractionState& interaction, float deltaSeconds);

    /// For a build with no interaction module yet: crosshair and hotbar only.
    void draw(const Hotbar& hotbar, float deltaSeconds);

private:
    void drawCrosshair(const InteractionState& interaction);
    void drawHotbar(const Hotbar& hotbar);
    void drawSelectionLabel(const Hotbar& hotbar);
    /// "Break" / "Drill r1.5 (33 sv)" just above the hotbar. Always visible
    /// rather than transient: which verb the mouse button is bound to is
    /// persistent state, and a player who cannot see it has to swing to find out.
    void drawModeLabel(const InteractionState& interaction);

    /// Advances the label fade and notices a slot change.
    void updateSelectionLabel(const Hotbar& hotbar, float deltaSeconds);

    const BlockRegistry* m_registry = nullptr;
    Style                m_style{};
    IconDrawer           m_iconDrawer;
    bool                 m_visible = true;

    /// Centre of the hotbar's selected slot, recomputed by drawHotbar so the
    /// label can sit above it without duplicating the layout arithmetic.
    glm::vec2 m_hotbarTopCentre{0.0f, 0.0f};

    BlockId m_labelBlock   = blocks::Air;
    float   m_labelTimer   = 0.0f;
    bool    m_labelPrimed  = false;
};

}  // namespace voxl
