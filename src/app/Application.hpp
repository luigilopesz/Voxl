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

namespace voxl {

struct ApplicationConfig {
    WindowConfig    window{};
    StreamingConfig streaming{};
    TerrainSettings terrain{};

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
};

}  // namespace voxl
