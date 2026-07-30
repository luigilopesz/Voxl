#pragma once

// The title screen: create a world, load one, change settings, quit.
//
// THE APPLICATION STATE MACHINE LIVES HERE
// ----------------------------------------
// `AppState` is declared in this header rather than in app/, because the main
// menu is the initial state and both menus need to name the enum. The
// application owns the variable; the menus only ever report what they want to
// happen next, they never change it themselves. A menu that could flip the
// application's state directly would be a second place - besides the frame loop
// - where "are we simulating right now" is decided, and the two would drift.
//
// HOW THIS MENU TALKS TO PERSISTENCE
// ----------------------------------
// It does not, directly. The menu never touches the filesystem: it renders a
// list of `WorldEntry` values the application supplies through
// `setWorldProvider`, and reports which one was chosen. Keeping the dependency
// pointed this way is what lets the menu be tested with no disk at all, and what
// keeps ui/ from including world/.
//
// world/WorldSave.hpp is what the application's provider will be built on, and
// it is per-world rather than per-library: a `WorldSave` is constructed with one
// directory and exposes `readMetadata()` for it. Two things it does NOT offer,
// which the application therefore has to supply itself:
//
//   * ENUMERATION. There is no "list the worlds under this root". The provider
//     iterates the subdirectories of `voxl::defaultSavesDirectory()`
//     (app/Settings.hpp) and keeps the ones containing
//     `WorldSave::metadataPath(dir)`.
//   * A DISPLAY NAME. `WorldMetadata` stores the seed, the player transform and
//     the time of day, but no name, so `WorldEntry::name` should be the
//     directory's own filename until the save format grows a name field.
//
// `lastPlayedUnixSeconds` likewise has no home in the metadata; the last-write
// time of the metadata file is the intended source.
//
// Thread safety: none. ImGui, main thread only.

#include "app/Settings.hpp"
#include "ui/SettingsPanel.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace voxl {

/// The three states the application can be in. Owned by the application; see
/// the header note.
enum class AppState : std::uint8_t {
    /// Title screen. No world is loaded, the cursor is free.
    MainMenu = 0,
    /// In the world, cursor captured, simulation running.
    Playing = 1,
    /// In the world, cursor free, simulation held. The world stays resident and
    /// keeps streaming so resuming is instant.
    Paused = 2,
};

[[nodiscard]] constexpr const char* toString(AppState state) noexcept
{
    switch (state) {
        case AppState::MainMenu: return "MainMenu";
        case AppState::Playing:  return "Playing";
        case AppState::Paused:   return "Paused";
    }
    return "Unknown";
}

/// One save directory, as the load list needs to show it.
struct WorldEntry {
    /// Display name. Falls back to the directory name when the save has none.
    std::string name;
    /// Where the save lives. Handed straight back in `MainMenuResult::world`.
    std::filesystem::path directory;
    /// Terrain seed, shown so a player can tell two worlds apart.
    std::uint64_t seed = 0;
    /// Seconds since the Unix epoch; 0 means "unknown", which the list renders
    /// as a dash rather than as 1970.
    std::int64_t lastPlayedUnixSeconds = 0;
};

enum class MainMenuAction : std::uint8_t {
    None = 0,
    /// Create a new world from `worldName` and `seed`.
    CreateWorld,
    /// Load `world`.
    LoadWorld,
    /// Close the application.
    Quit,
};

struct MainMenuResult {
    MainMenuAction action = MainMenuAction::None;

    /// CreateWorld: the name the player typed, already trimmed and never empty
    /// (it falls back to "New World"). The application turns this into a
    /// directory name; the menu deliberately does not, because what is a legal
    /// directory name is the save system's problem.
    std::string worldName;
    /// CreateWorld: the resolved seed. Never zero-by-accident - see
    /// `MainMenu::resolveSeed`.
    std::uint64_t seed = 0;

    /// LoadWorld: a copy of the chosen entry.
    WorldEntry world;

    /// Settings groups the settings panel changed this frame.
    SettingsDirty dirty = SettingsDirty::None;
    /// The settings panel asked for the settings file to be written.
    bool saveSettingsRequested = false;
};

class MainMenu {
public:
    /// Re-read whenever the world list may have changed. Optional: without one,
    /// the list is whatever `setWorlds` was last given.
    using WorldListProvider = std::function<std::vector<WorldEntry>()>;

    MainMenu() = default;

    void setWorldProvider(WorldListProvider provider);
    /// Replaces the list directly. Also used by tests, which have no filesystem.
    void setWorlds(std::vector<WorldEntry> worlds);
    [[nodiscard]] const std::vector<WorldEntry>& worlds() const noexcept { return m_worlds; }
    /// Re-runs the provider. No-op when none is installed.
    void refreshWorlds();

    /// Small text in the corner, e.g. "Voxl 0.1.0".
    void setVersionLabel(std::string text);
    /// One line shown in the accent colour under the buttons. Cleared by the
    /// next navigation, so a stale "load failed" cannot linger.
    void setStatusMessage(std::string text);

    /// Back to the root screen with every sub-screen closed. Call when entering
    /// AppState::MainMenu, so returning from a world does not land on the
    /// half-filled "create world" form the player left behind.
    void reset();

    /// Draws the menu, and the settings panel on top of it when it is open.
    /// Call between `ImGui::NewFrame()` and `ImGui::Render()`.
    MainMenuResult draw(SettingsPanel& panel, Settings& settings);

    /// Turns what the player typed into a seed.
    ///
    ///  * empty            -> derived from the wall clock, so two worlds created
    ///                        in the same session differ
    ///  * a decimal integer-> that value, so a shared seed reproduces exactly
    ///  * anything else    -> a 64-bit hash of the text, deterministic across
    ///                        machines and runs
    ///
    /// No `rand()` and no unseeded RNG: the only entropy is the clock, and only
    /// for the empty case, so "same seed => same world" survives intact.
    [[nodiscard]] static std::uint64_t resolveSeed(std::string_view text);

private:
    enum class Screen : std::uint8_t { Root, Create, Load };

    void drawBackdrop() const;
    void drawTitle() const;
    void drawRoot(MainMenuResult& result, SettingsPanel& panel, const Settings& settings);
    void drawCreate(MainMenuResult& result);
    void drawLoad(MainMenuResult& result);

    /// Clears the status line and switches screen.
    void goTo(Screen screen);

    Screen                   m_screen = Screen::Root;
    std::vector<WorldEntry>  m_worlds;
    WorldListProvider        m_provider;
    bool                     m_worldsLoaded = false;

    std::string m_versionLabel{"Voxl"};
    std::string m_status;

    /// Fixed buffers rather than std::string: ImGui's std::string overloads live
    /// in misc/cpp/imgui_stdlib.cpp, which this build does not compile, and
    /// adding it would be a change to a CMakeLists this module does not own.
    char        m_nameBuffer[64]{};
    char        m_seedBuffer[32]{};
    std::size_t m_selectedWorld = 0;
};

}  // namespace voxl
