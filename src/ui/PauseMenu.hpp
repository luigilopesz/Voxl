#pragma once

// The pause screen: resume, settings, save and quit to the title, quit to the
// desktop.
//
// The world stays fully loaded and keeps streaming behind this menu. Pausing
// only stops the SIMULATION - zero the frame clock's time scale - so that
// resuming is instantaneous and so that a chunk that was mid-generate when the
// player hit Escape still arrives. That also means the menu is drawn over a live
// frame rather than over a black screen, which is why it dims what is behind it
// instead of covering it.
//
// Thread safety: none. ImGui, main thread only.

#include "app/Settings.hpp"
#include "ui/MainMenu.hpp"
#include "ui/SettingsPanel.hpp"

#include <cstdint>
#include <string>

namespace voxl {

enum class PauseMenuAction : std::uint8_t {
    None = 0,
    /// Recapture the cursor and resume the simulation.
    Resume,
    /// Flush the world to disk, unload it, and go back to AppState::MainMenu.
    SaveAndQuitToMenu,
    /// Flush the world to disk and close the application.
    QuitToDesktop,
};

struct PauseMenuResult {
    PauseMenuAction action = PauseMenuAction::None;

    /// Settings groups the settings panel changed this frame.
    SettingsDirty dirty = SettingsDirty::None;
    /// The settings panel asked for the settings file to be written.
    bool saveSettingsRequested = false;
};

class PauseMenu {
public:
    PauseMenu() = default;

    /// Shown as the subtitle. Empty hides the line.
    void setWorldName(std::string name);
    /// One line under the buttons - "Saving...", "Save failed: disk full".
    void setStatusMessage(std::string text);
    [[nodiscard]] const std::string& statusMessage() const noexcept { return m_status; }

    /// Closes any sub-screen. Call when leaving AppState::Paused so the next
    /// pause opens on the button list rather than on the settings panel the
    /// player was last in.
    void reset(SettingsPanel& panel);

    /// Draws the menu, and the settings panel on top of it when it is open.
    /// Call between `ImGui::NewFrame()` and `ImGui::Render()`.
    PauseMenuResult draw(SettingsPanel& panel, Settings& settings);

private:
    std::string m_worldName;
    std::string m_status;
    /// Second click required for "Quit to desktop": the button sits one row
    /// below "Save and quit to title", and losing a session to a misclick is a
    /// far worse outcome than one extra click.
    bool m_confirmQuit = false;
};

}  // namespace voxl
