#pragma once

// The application object: owns every subsystem and runs the frame loop.
//
// ===========================================================================
//  CONSTRUCTION AND DESTRUCTION ORDER IS THE DESIGN
// ===========================================================================
//
// CONSTRUCTION, in the order the initialiser list runs:
//
//   1. `m_config` - and with it `prepareConfig()`, which LOADS settings.cfg
//      before anything else exists. Vsync and render distance are window and
//      streamer construction parameters, so the file has to be read before
//      either is built; applying them afterwards would mean one frame at the
//      wrong setting and a visible flicker on every launch.
//   2. `m_window` - creates the GL context. Nothing that touches GL may be
//      constructed before it.
//   3. `m_imgui` - needs the window handle.
//   4. `m_jobs` - the worker pool. Must exist before anything that submits.
//   5. `m_audio` - opens the output device. Independent of everything else.
//   6. `m_registry`, then `m_renderer` (uploads textures, compiles shaders),
//      then `m_world`.
//   7. Gameplay objects, then `openWorld()` in the constructor body, which is
//      what builds `m_terrain` and `m_save`.
//
// DESTRUCTION is reverse declaration order, and four dependencies make it
// load-bearing rather than incidental:
//
//   * Every GL object must die while the context is still current, so `Window`
//     is declared FIRST and therefore destroyed LAST. Renderer, ChunkRenderer,
//     BlockInteraction, DebugOverlay and the ImGui backends all sit after it.
//
//   * The mesh jobs the World dispatches upload through the Renderer, and the
//     World's release callback calls back into the ChunkRenderer, so `World` is
//     declared AFTER `Renderer` and destroyed before it.
//
//   * `World`'s generator and retire lambdas dereference `m_terrain` and
//     `m_save`, so both are declared BEFORE `World` and outlive it.
//
//   * `~WorldSave` blocks on its queued write jobs, so the JobSystem must still
//     be running when it fires.
//
// Two orderings cannot be expressed by declaration order at all, and
// `~Application` therefore does them by hand, in this sequence:
//
//   a. SAVE BEFORE THE POOL STOPS. `WorldSave::saveChunk` dispatches the write
//      to a worker, so every save must be issued and flushed while the pool is
//      still accepting work. Submitting after `shutdown()` trips a VOXL_CHECK
//      in debug and, in release, enqueues a job no worker will ever run - which
//      strands `flush()` until its timeout and loses the chunk.
//
//   b. STOP THE POOL BEFORE THE WORLD IT REFERENCES. `World` needs a
//      `JobSystem&` at construction, so the pool is declared before it and
//      would otherwise be destroyed after it, with workers still touching a
//      dead world.
//
// Thread safety: none. Everything here is main-thread.

#include "app/Settings.hpp"
#include "audio/AudioEngine.hpp"
#include "core/FrameLimiter.hpp"
#include "core/JobSystem.hpp"
#include "core/Time.hpp"
#include "gameplay/BlockInteraction.hpp"
#include "gameplay/Hotbar.hpp"
#include "gameplay/Player.hpp"
#include "mesh/GreedyMesher.hpp"
#include "mesh/SubVoxelMesher.hpp"
#include "physics/SubVoxelAccess.hpp"
#include "platform/Input.hpp"
#include "platform/Window.hpp"
#include "render/Camera.hpp"
#include "render/Renderer.hpp"
#include "ui/DebugOverlay.hpp"
#include "ui/Hud.hpp"
#include "ui/MainMenu.hpp"
#include "ui/PauseMenu.hpp"
#include "ui/SettingsPanel.hpp"
#include "world/Block.hpp"
#include "world/DayNightCycle.hpp"
#include "world/TerrainGenerator.hpp"
#include "world/World.hpp"
#include "world/WorldSave.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace voxl {

/// Scripted startup state for the visual review harness (tools/visual_review.ps1,
/// results in docs/VISUAL_REVIEW.md).
///
/// WHY THIS EXISTS. The two defects LOD and sub-voxel damage produce - a crack at
/// a level boundary, and a carved surface shaded or textured unlike the blocks
/// beside it - are only findable by comparing the SAME framing under different
/// settings. A first-person camera driven by hand cannot reproduce a framing, so
/// the reviewer would be comparing two different pictures. Every field is inert
/// by default and reachable only from argv, so a normal run is unchanged.
///
/// Nothing here is a gameplay feature and none of it is persisted. Delete the
/// whole struct, `Application::applyDebugStartup` and `updateDebugScript` when the
/// LOD and sub-voxel work stops needing visual regression shots.
struct DebugStartup {
    /// Which carve pattern `updateDebugScript` cuts into the terrain once it has
    /// streamed in. See the definition for the geometry of each.
    enum class Carve : std::uint8_t { None, Crater, Tunnel, Both };

    bool      hasPosition = false;
    glm::vec3 position{0.0f};  ///< player feet, world space

    bool  hasRotation  = false;
    float yawDegrees   = 0.0f;
    float pitchDegrees = 0.0f;

    /// Suspends player physics. A camera parked in mid-air for a long LOD view
    /// otherwise falls out of frame before the capture lands.
    bool freezePlayer = false;

    bool showOverlay = false;  ///< open the F3 panel at startup
    bool hideHud     = false;  ///< hide the hotbar and crosshair for clean shots

    bool         lodEnabled = true;
    bool         hasBands   = false;
    std::int32_t bandStart[kLodMax] = {5, 9, 14};

    Carve     carve = Carve::None;
    BlockPos  carveAnchor{0, 0, 0};
    bool      hasCarveAnchor = false;

    /// `--time`. Pins the day/night cycle at a chosen moment.
    bool  hasTime   = false;
    float timeOfDay = kTimeNoon;

    /// `--freeze-time`. Implied by `--freeze`: a frozen camera whose sky keeps
    /// moving makes two captures that were meant to differ by one setting differ
    /// by the sky as well.
    bool freezeTime = false;

    [[nodiscard]] bool any() const noexcept
    {
        return hasPosition || hasRotation || freezePlayer || showOverlay || hideHud ||
               !lodEnabled || hasBands || carve != Carve::None || hasTime || freezeTime;
    }
};

struct ApplicationConfig {
    WindowConfig    window{};
    StreamingConfig streaming{};
    TerrainSettings terrain{};
    DebugStartup    debug{};

    // ------------------------------------------------------------ settings --

    /// Where settings.cfg lives. Empty selects `defaultSettingsPath()`.
    std::filesystem::path settingsPath{};

    /// Filled by `Application::prepareConfig` from `settingsPath`; anything the
    /// caller puts here is overwritten. It lives in the config rather than as a
    /// plain member because the window is constructed from it.
    Settings                 settings{};
    std::vector<std::string> unknownSettingLines{};

    /// Set by Main when `--radius` was given, so the settings file cannot
    /// silently overwrite an explicit command-line choice. Same idea for the
    /// seed, which additionally loses to a seed stored in a save.
    bool radiusOverridden = false;
    bool seedOverridden   = false;

    /// `--day-length`, in minutes. Zero means "not given"; anything positive is
    /// folded into `settings` after the file is read, so the flag wins.
    float dayLengthMinutesOverride = 0.0f;

    // --------------------------------------------------------- persistence --

    /// Root under which world directories live. Empty selects
    /// `defaultSavesDirectory()`.
    std::filesystem::path savesRoot{};

    /// Directory name of the world opened at startup, under `savesRoot`.
    std::string worldName{"world"};

    /// `--no-save`. Runs the whole game with no WorldSave at all: nothing is
    /// read, nothing is written, and every chunk comes from the seed.
    bool persistence = true;

    /// Wall-clock seconds between autosaves.
    double autosaveSeconds = 30.0;

    // --------------------------------------------------------------- flow --

    /// `--menu`. Opens on the title screen instead of dropping straight into a
    /// world. Off by default: the visual review harness and every `--pos`/
    /// `--carve` rig need to be in-world on frame one, and so does anyone who
    /// just double-clicked the exe.
    bool startInMainMenu = false;

    /// Upload budget per frame. Chunk meshes are uploaded from the job system's
    /// main-thread queue; without a cap, the burst that follows a teleport
    /// uploads hundreds of meshes in one frame and drops several frames doing it.
    std::chrono::microseconds uploadBudget{2000};

    /// Frames of generate/mesh/upload to run before the window is first drawn,
    /// so the player does not spawn looking at an empty void. Bounded by
    /// `warmupTimeout` because a slow machine must still start.
    std::chrono::milliseconds warmupTimeout{4000};

    /// Interval between the streaming status lines written to voxl.log. Zero
    /// disables them.
    double statusLogSeconds = 2.0;
};

/// Owns the whole game. Construct one, call `run()`.
class Application {
public:
    explicit Application(const ApplicationConfig& config = {});
    ~Application();

    Application(const Application&)            = delete;
    Application& operator=(const Application&) = delete;
    Application(Application&&)                 = delete;
    Application& operator=(Application&&)      = delete;

    /// Runs until the window is closed. Returns the process exit code.
    int run();

private:
    /// RAII for the ImGui context and its two backends. A member rather than a
    /// call pair so the unwind path out of the constructor still shuts down the
    /// backends in reverse order.
    class ImGuiScope {
    public:
        explicit ImGuiScope(Window& window);
        ~ImGuiScope();

        ImGuiScope(const ImGuiScope&)            = delete;
        ImGuiScope& operator=(const ImGuiScope&) = delete;
        ImGuiScope(ImGuiScope&&)                 = delete;
        ImGuiScope& operator=(ImGuiScope&&)      = delete;
    };

    /// Reads settings.cfg and folds it into `config` before any member is built.
    /// Static because it runs inside the initialiser list, where `this` is not
    /// yet usable.
    [[nodiscard]] static ApplicationConfig prepareConfig(ApplicationConfig config);

    void wireWorld();
    void spawnPlayer();
    void warmUp();

    // ------------------------------------------------------- world session --

    /// Builds `m_terrain` and `m_save` for one world and streams it in.
    ///
    /// A stored seed WINS over the requested one: every region file records the
    /// seed it was written with and refuses to open under a different one, so
    /// honouring the caller here would silently turn a load into a brand-new
    /// world sharing a directory with the old one.
    ///
    /// That applies to a DAMAGED level.vxw too. The seed is resolved through
    /// `resolveWorldSeed`, which falls back to the region headers before it
    /// falls back to `seed`; taking the caller's seed while intact regions exist
    /// makes the world simultaneously unloadable and unsavable. Whichever path
    /// is taken is logged.
    void openWorld(std::string name, std::uint64_t seed);

    /// Saves everything, unloads every chunk and tears the session down. Leaves
    /// `m_terrain` and `m_save` empty; the world object itself survives, because
    /// destroying it mid-run would mean re-running `wireWorld` against a pool
    /// that may still hold closures capturing the old one.
    void closeWorld();

    [[nodiscard]] WorldMetadata currentMetadata() const;

    /// Directory scan behind `MainMenu::setWorldProvider`.
    [[nodiscard]] std::vector<WorldEntry> enumerateWorlds() const;

    /// Collects dirty chunks and queues them. Also rewrites the metadata file.
    void saveEverything();

    // ------------------------------------------------------- state machine --

    void enterPlaying();
    void enterPaused();
    void enterMainMenu();

    void handleMainMenu(const MainMenuResult& result);
    void handlePauseMenu(const PauseMenuResult& result);

    /// Pushes the settings groups named by `dirty` into the subsystems that own
    /// them. `SettingsDirty::All` at the end of construction is what makes the
    /// loaded file take effect on frame one.
    void applySettings(SettingsDirty dirty);

    // ------------------------------------------------------------- per frame --

    void frame();
    void pollInput();
    void simulate();
    void stream();
    /// Runs queued GPU uploads within the frame's budget. Returns milliseconds
    /// spent, for the overlay.
    float drainUploads();
    void  render();
    void  drawUi();
    /// Refreshes the cached world/job stats and, at most once every
    /// `statusLogSeconds`, writes a streaming status line to the log.
    void updateStats();

    /// Listener, break/place one-shots, the mining bed and footsteps.
    void updateAudio();

    void setCursorCaptured(bool captured);

    /// Applies `ApplicationConfig::debug` to the player, the overlay, the LOD
    /// policy and the clock. Called after spawnPlayer() and before warmUp(), so
    /// the world streams around the scripted camera at the scripted levels rather
    /// than around the spawn point.
    void applyDebugStartup();

    /// Feeds the scripted carve pattern to the world a batch at a time. No-op
    /// unless `--carve` was given.
    void updateDebugScript();

    [[nodiscard]] DebugOverlayFrame buildOverlayFrame(float cpuMs, float uploadMs) const;

    ApplicationConfig m_config;

    /// Loaded by `prepareConfig` before the window exists. `m_unknownSettingLines`
    /// carries keys this build does not know, verbatim, so saving cannot delete
    /// another build's settings.
    Settings                 m_settings;
    std::vector<std::string> m_unknownSettingLines;
    std::filesystem::path    m_settingsPath;

    // ---- declaration order is the destruction contract; see the header note --
    Window     m_window;
    Input      m_input;
    ImGuiScope m_imgui;

    JobSystem m_jobs;

    audio::AudioEngine    m_audio;
    audio::SustainedVoice m_miningSound;

    BlockRegistry m_registry;

    /// Optional so a world switch can rebuild them around a new seed and
    /// directory. Both are non-copyable and non-movable, so `emplace` is the
    /// only route. Declared before `m_world`, whose generator and retire
    /// lambdas dereference them.
    std::optional<TerrainGenerator> m_terrain;
    std::optional<WorldSave>        m_save;

    Renderer m_renderer;
    World    m_world;

    /// Sub-voxel occupancy reader handed to the collider each step.
    ///
    /// A member rather than a local because it owns a std::function, and building
    /// one per fixed step would allocate inside the physics loop. Constructed
    /// before the first update but only ever invoked afterwards, so the fact that
    /// it captures `this` while the object is still being built is safe.
    physics::ChunkSubVoxelAccess m_subVoxelAccess;

    Camera           m_camera;
    Player           m_player;
    Hotbar           m_hotbar;
    BlockInteraction m_interaction;
    Hud              m_hud;
    DebugOverlay     m_overlay;

    DayNightCycle m_dayNight;
    FrameLimiter  m_frameLimiter;

    AppState      m_state = AppState::Playing;
    SettingsPanel m_settingsPanel;
    MainMenu      m_mainMenu;
    PauseMenu     m_pauseMenu;

    FrameClock m_clock;

    // ---- current world session ----
    std::string m_worldName;
    bool        m_worldOpen       = false;
    double      m_playTimeSeconds = 0.0;

    // ---- audio scratch ----
    audio::CueId m_windCue          = audio::kInvalidCue;
    float        m_footstepDistance = 0.0f;
    glm::vec3    m_lastFootPosition{0.0f};

    // ---- per-frame scratch ----
    PlayerInput      m_playerInput{};
    InteractionInput m_interactionInput{};
    WorldStats       m_stats{};
    JobSystemStats   m_jobStats{};

    /// Monotonic counter handed to the streamer. Deliberately NOT
    /// FrameClock::frameIndex(): the warm-up runs streaming updates before the
    /// clock has ticked once, and ChunkManager's unload grace period subtracts
    /// frame indices as unsigned, so a single step backwards underflows and
    /// makes every distant chunk instantly retirable.
    std::uint64_t m_streamFrame = 0;

    int    m_framebufferWidth  = 0;
    int    m_framebufferHeight = 0;
    double m_nextStatusLog     = 0.0;
    float  m_uploadMs          = 0.0f;
    /// Previous frame's CPU cost, excluding the swap/vsync wait. Reported one
    /// frame late because measuring it requires the frame to be over.
    float m_lastCpuMs = 0.0f;
    /// Simulation is held until the chunk under the player exists, otherwise the
    /// spawn falls through a world that has not streamed in yet.
    bool m_simulationStarted = false;

    // ---- scripted visual review (see DebugStartup) ----

    /// The scripted rig: blocks to place first, then sub-voxels to remove.
    ///
    /// Fed in batches, and the carve does not start until the build has fully
    /// landed. Every edit that arrives while a neighbour is meshing goes into
    /// World's bounded deferral queue, so issuing ten thousand in one frame would
    /// overflow it and drop cells out of the middle of the shape - a hole that
    /// looks exactly like a mesher bug, which is the last thing a defect hunt
    /// needs.
    std::vector<std::pair<BlockPos, BlockId>>       m_debugBuild;
    std::vector<std::pair<BlockPos, std::uint16_t>> m_debugCarves;
    std::size_t m_debugBuildCursor = 0;
    std::size_t m_debugCarveCursor = 0;
    bool        m_debugRigPlanned  = false;
    bool        m_debugRigDone     = false;
};

}  // namespace voxl
