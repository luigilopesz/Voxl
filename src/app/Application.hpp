#pragma once

// The application object: owns every subsystem and runs the frame loop.
//
// MEMBER ORDER IS THE DESIGN
// --------------------------
// Members are destroyed in reverse declaration order, and two dependencies make
// that ordering load-bearing rather than incidental:
//
//   * Every GL object must die while the context is still current, so `Window`
//     is declared FIRST and therefore destroyed LAST. Renderer, ChunkRenderer,
//     BlockInteraction, DebugOverlay and the ImGui backends all sit after it.
//
//   * The mesh jobs the World dispatches upload through the Renderer, and the
//     World's release callback calls back into the ChunkRenderer, so `World` is
//     declared AFTER `Renderer` and destroyed before it.
//
// The one ordering that cannot be expressed by declaration order is the job
// system: `World` needs a `JobSystem&` at construction, so the pool must be
// declared before the world and would otherwise be destroyed after it - with
// workers still touching a dead world. `~Application` therefore shuts the pool
// down and clears the main-thread queue explicitly, before any member
// destructor runs. See the comment there.
//
// Thread safety: none. Everything here is main-thread.

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
#include "world/Block.hpp"
#include "world/TerrainGenerator.hpp"
#include "world/World.hpp"

#include <chrono>
#include <cstdint>
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

    [[nodiscard]] bool any() const noexcept
    {
        return hasPosition || hasRotation || freezePlayer || showOverlay || hideHud ||
               !lodEnabled || hasBands || carve != Carve::None;
    }
};

struct ApplicationConfig {
    WindowConfig    window{};
    StreamingConfig streaming{};
    TerrainSettings terrain{};
    DebugStartup    debug{};

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

    void wireWorld();
    void spawnPlayer();
    void warmUp();

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

    void setCursorCaptured(bool captured);

    /// F6: carves the sub-voxel under the crosshair. A debug affordance until a
    /// real mining verb exists; see the definition.
    void carveTargetSubVoxel();

    /// Applies `ApplicationConfig::debug` to the player, the overlay and the LOD
    /// policy. Called after spawnPlayer() and before warmUp(), so the world
    /// streams around the scripted camera at the scripted levels rather than
    /// around the spawn point.
    void applyDebugStartup();

    /// Feeds the scripted carve pattern to the world a batch at a time. No-op
    /// unless `--carve` was given.
    void updateDebugScript();

    [[nodiscard]] DebugOverlayFrame buildOverlayFrame(float cpuMs, float uploadMs) const;

    ApplicationConfig m_config;

    // ---- declaration order is the destruction contract; see the header note --
    Window     m_window;
    Input      m_input;
    ImGuiScope m_imgui;

    JobSystem m_jobs;

    BlockRegistry    m_registry;
    TerrainGenerator m_terrain;

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

    FrameClock m_clock;

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
