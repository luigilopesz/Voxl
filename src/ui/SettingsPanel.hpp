#pragma once

// The settings panel, and the game's ImGui theme.
//
// One panel object is shared by the main menu and the pause menu: the settings
// a player changes from the pause screen and the ones they change before
// loading a world are the same settings, and giving each menu its own panel
// would mean two "unsaved edits" snapshots that can disagree.
//
// LIVE APPLY, AND WHY IT IS A BITMASK
// -----------------------------------
// Editing a slider writes straight into the caller's `Settings`, so the world
// reacts while the slider is still being dragged. But "apply" means something
// different for each field - a field of view is a matrix rebuild, a render
// distance is a streaming reconfiguration, a GUI scale is an ImGui restyle that
// must NOT happen while ImGui is mid-frame - so the panel reports WHICH groups
// changed and the application decides how to satisfy each. Returning a plain
// "something changed" bool would force the caller to reapply everything every
// frame the panel is open, which for render distance means retiring and
// re-streaming the world on every mouse move.
//
// Thread safety: none. ImGui, main thread only.

#include "app/Settings.hpp"

#include <cstdint>

namespace voxl {

/// Which subsystems must be re-applied. Bit flags; combine with `|`.
enum class SettingsDirty : std::uint32_t {
    None = 0,
    /// vsync, fps cap -> Window::setVSync, FrameLimiter::setTargetFps.
    Video = 1u << 0,
    /// fov -> Camera::setFovDegrees.
    Camera = 1u << 1,
    /// render distance, LOD bands/enable -> ChunkManager::setConfig.
    Streaming = 1u << 2,
    /// fog distance scale -> Renderer::setFogFromViewDistance.
    Fog = 1u << 3,
    /// sensitivity, invert Y -> PlayerConfig.
    Controls = 1u << 4,
    /// master/world/UI volume -> the audio mixer.
    Audio = 1u << 5,
    /// GUI scale -> SettingsPanel::applyTheme. MUST be applied outside a frame.
    Interface = 1u << 6,
    /// Anisotropy. Needs the block texture array's sampler state changed, so it
    /// is separated from Video: the application may choose to defer it.
    Texture = 1u << 7,
    /// Day length -> the day/night driver.
    World = 1u << 8,

    All = 0x1FFu,
};

[[nodiscard]] constexpr SettingsDirty operator|(SettingsDirty a, SettingsDirty b) noexcept
{
    return static_cast<SettingsDirty>(static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

constexpr SettingsDirty& operator|=(SettingsDirty& a, SettingsDirty b) noexcept
{
    a = a | b;
    return a;
}

[[nodiscard]] constexpr SettingsDirty operator&(SettingsDirty a, SettingsDirty b) noexcept
{
    return static_cast<SettingsDirty>(static_cast<std::uint32_t>(a) & static_cast<std::uint32_t>(b));
}

/// True when any bit of `flags` is set in `mask`.
[[nodiscard]] constexpr bool hasAny(SettingsDirty mask, SettingsDirty flags) noexcept
{
    return static_cast<std::uint32_t>(mask & flags) != 0;
}

/// What one `draw()` produced.
struct SettingsPanelResult {
    /// Groups the caller must re-apply this frame.
    SettingsDirty dirty = SettingsDirty::None;

    /// The panel stopped being open during this call.
    bool closed = false;

    /// "Done" was pressed: persist the settings file. Separate from `closed`
    /// because "Cancel" also closes but must not write.
    bool saveRequested = false;

    /// "Cancel" was pressed and the snapshot has been restored. `dirty` is
    /// `All` in that case, because any field may have moved back.
    bool cancelled = false;
};

/// A modal settings panel over whatever is behind it.
class SettingsPanel {
public:
    SettingsPanel() noexcept = default;

    /// Opens the panel and snapshots `current` so Cancel has something to
    /// restore. Re-opening an already-open panel re-snapshots.
    void open(const Settings& current);
    void close() noexcept;
    [[nodiscard]] bool isOpen() const noexcept { return m_open; }

    /// Draws the panel and edits `settings` in place. Returns
    /// `SettingsPanelResult{}` unchanged when the panel is closed, so it is safe
    /// to call unconditionally.
    ///
    /// Call between `ImGui::NewFrame()` and `ImGui::Render()`.
    SettingsPanelResult draw(Settings& settings);

    /// Installs the game's theme at `guiScale`.
    ///
    /// Rebuilds the whole style from a default-constructed `ImGuiStyle` every
    /// time, so calling it repeatedly is safe. `ImGuiStyle::ScaleAllSizes` is
    /// CUMULATIVE - calling it twice with 1.25 gives 1.56x - and re-styling on
    /// every GUI-scale change is exactly the pattern that trips over that.
    ///
    /// MUST NOT be called between `NewFrame()` and `Render()`: ImGui latches
    /// parts of the style at frame start and the frame already in flight would
    /// be drawn half-scaled.
    static void applyTheme(float guiScale);

    /// Accent colour of the theme, for anything that has to match it (the menu
    /// title, the HUD). Linear-ish sRGB in [0, 1].
    [[nodiscard]] static const float* accentColour() noexcept;

private:
    /// Draws one tab's contents. Each returns the bits it dirtied.
    static SettingsDirty drawVideoTab(Settings& settings);
    static SettingsDirty drawControlsTab(Settings& settings);
    static SettingsDirty drawAudioTab(Settings& settings);
    static SettingsDirty drawWorldTab(Settings& settings);

    bool     m_open = false;
    Settings m_snapshot{};
};

}  // namespace voxl
