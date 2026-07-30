#include "app/Application.hpp"

#include <glad/gl.h>

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include "core/Log.hpp"
#include "physics/Collision.hpp"
#include "physics/Raycast.hpp"
#include "world/BlockAccess.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <utility>

namespace voxl {
namespace {

/// The asset tree is copied next to the executable by the build, but a developer
/// run from the repository root should also work. Probe the handful of places it
/// can legitimately be rather than making the working directory part of the
/// contract.
[[nodiscard]] std::filesystem::path resolveAssetRoot()
{
    namespace fs = std::filesystem;
    const std::array<fs::path, 4> candidates{
        fs::path{"assets"},
        fs::path{"bin"} / "assets",
        fs::path{".."} / "assets",
        fs::path{".."} / ".." / "assets",
    };
    for (const fs::path& candidate : candidates) {
        std::error_code error;
        if (fs::exists(candidate / "shaders" / "chunk.vert", error)) {
            return candidate;
        }
    }
    VOXL_LOG_WARN("Could not locate the assets directory; falling back to ./assets");
    return fs::path{"assets"};
}

[[nodiscard]] BlockPos toBlockPos(const glm::vec3& position) noexcept
{
    return BlockPos{physics::floorToInt(position.x), physics::floorToInt(position.y),
                    physics::floorToInt(position.z)};
}

/// physics::Aabb and the render-layer Aabb are structurally identical but are
/// deliberately separate types (see the note in physics/Aabb.hpp). Interaction
/// takes the render one, so the conversion lives here rather than in either.
[[nodiscard]] Aabb toRenderAabb(const physics::Aabb& box) noexcept
{
    return Aabb{box.min, box.max};
}

constexpr std::array<Key, 9> kHotbarDigitKeys{Key::Num1, Key::Num2, Key::Num3,
                                              Key::Num4, Key::Num5, Key::Num6,
                                              Key::Num7, Key::Num8, Key::Num9};

}  // namespace

// ------------------------------------------------------------- ImGuiScope --

Application::ImGuiScope::ImGuiScope(Window& window)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    // No .ini: the debug overlay's layout is code-defined, and a stray ini in the
    // working directory silently overrides it and looks like a UI bug.
    io.IniFilename = nullptr;

    // install_callbacks = true chains to whatever Window already registered, so
    // the scroll accumulator keeps working with the UI backend attached.
    ImGui_ImplGlfw_InitForOpenGL(window.handle(), true);
    ImGui_ImplOpenGL3_Init("#version 450 core");
}

Application::ImGuiScope::~ImGuiScope()
{
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

// ------------------------------------------------------- construction --

namespace {

[[nodiscard]] RendererConfig makeRendererConfig()
{
    RendererConfig config;
    config.assetRoot = resolveAssetRoot();
    return config;
}

}  // namespace

Application::Application(const ApplicationConfig& config)
    : m_config(config)
    , m_window(config.window)
    , m_input(m_window)
    , m_imgui(m_window)
    , m_jobs()
    , m_registry(createDefaultBlockRegistry())
    , m_terrain(config.terrain)
    , m_renderer(makeRendererConfig())
    , m_world(m_jobs, m_registry, config.streaming)
    , m_interaction(m_registry)
    , m_hud(&m_registry)
    , m_overlay(&m_registry)
{
    VOXL_LOG_INFO("Application starting: {} worker threads, load radius {}",
                  m_jobs.workerCount(), config.streaming.loadRadius);

    if (!m_renderer.valid()) {
        // Not fatal: the window, the world and the overlay still work, and the
        // log already carries the shader/texture failure that caused it.
        VOXL_LOG_ERROR("Renderer failed to initialise; running without world rendering");
    }

    m_framebufferWidth  = m_window.width();
    m_framebufferHeight = m_window.height();
    m_renderer.resize(m_framebufferWidth, m_framebufferHeight);
    m_renderer.setFogFromViewDistance(
        static_cast<float>(config.streaming.loadRadius * kChunkSize));

    m_camera.setAspectRatio(m_window.aspectRatio());
    m_camera.setClipPlanes(0.05f, static_cast<float>(config.streaming.loadRadius + 2) *
                                      static_cast<float>(kChunkSize) * 1.5f);

    m_overlay.setSelfToggleKey(false);  // the input layer owns F3; both would double-fire
    m_interaction.setReach(m_player.config().reach);

    wireWorld();
    spawnPlayer();

    setCursorCaptured(true);
    warmUp();
}

Application::~Application()
{
    VOXL_LOG_INFO("Application shutting down");

    // ORDER: the job system outlives the world by declaration, so it has to be
    // stopped by hand here. Drain rather than Cancel because a cancelled job's
    // broken promise is indistinguishable from a crash in the log, and because
    // ChunkManager's own destructor would then wait five seconds for jobs that
    // will never run.
    m_jobs.shutdown(ShutdownMode::Drain);

    // Whatever those jobs posted for the main thread will never be drained, and
    // its closures hold references to the renderer. Drop them while everything
    // they capture is still alive.
    m_jobs.mainThreadQueue().clear();

    // Retire chunks (and therefore GPU meshes) while the renderer and the GL
    // context are both still up.
    m_world.unloadAll();
    m_renderer.chunks().clear();
}

void Application::wireWorld()
{
    m_world.setGenerator([this](Chunk& chunk) { m_terrain.generate(chunk); });

    m_world.setMesher([this](const ChunkNeighbourhood& neighbourhood) {
        // GreedyMesher owns ~160 KB of scratch and is explicitly not thread safe,
        // so each worker keeps its own. thread_local binds the registry at first
        // use per thread, which is correct here because there is exactly one.
        thread_local GreedyMesher mesher{m_registry};

        ChunkMeshData data;
        mesher.mesh(neighbourhood, data);

        ChunkMeshUpload upload;
        upload.gpuBytes  = data.byteSize();
        upload.triangles = data.triangleCount();
        upload.upload    = [this, mesh = std::move(data)] { m_renderer.chunks().upload(mesh); };
        return upload;
    });

    m_world.setMeshReleaser(
        [this](const ChunkPos& position) { m_renderer.chunks().remove(position); });

    m_interaction.setRaycaster([this](const glm::vec3& origin, const glm::vec3& direction,
                                      float maxDistance, InteractionHit& out) {
        const physics::RayHit hit =
            physics::raycastBlocks(m_world, m_registry, origin, direction, maxDistance);
        if (!hit.hit) {
            return false;
        }
        out = InteractionHit{hit.block, hit.blockId, hit.face, hit.point, hit.distance};
        return true;
    });

    m_interaction.setBlockWriter([this](const BlockPos& position, BlockId id) {
        const EditResult result = m_world.setBlock(position, id);
        // Deferred means "accepted, applied next update". Reporting it as a
        // failure would make the HUD flash a rejection for a successful edit.
        return result == EditResult::Applied || result == EditResult::Deferred;
    });

    m_interaction.setBlockReader(
        [this](const BlockPos& position) { return m_world.getBlock(position); });
}

void Application::spawnPlayer()
{
    const std::int32_t surface = m_terrain.surfaceHeight(0, 0);
    const float spawnY = static_cast<float>(std::max(surface + 1, m_config.terrain.seaLevel + 1));
    m_player.setPosition(glm::vec3{0.5f, spawnY, 0.5f});
    m_player.setRotation(0.0f, -10.0f);
    m_player.updateCamera(m_camera, 0.0f);
    VOXL_LOG_INFO("Spawned at (0.5, {:.1f}, 0.5); terrain surface y={}", spawnY, surface);
}

void Application::warmUp()
{
    if (m_config.warmupTimeout.count() <= 0) {
        return;
    }

    const ChunkPos spawnChunk = toChunkPos(toBlockPos(m_player.position()));
    Stopwatch      watch;

    while (watch.elapsed() < m_config.warmupTimeout) {
        m_window.pollEvents();
        if (m_window.shouldClose()) {
            break;
        }
        m_world.update(StreamingView{m_camera.position(), m_camera.forward()}, ++m_streamFrame);
        m_jobs.mainThreadQueue().drainAll();
        if (m_world.isChunkReady(spawnChunk)) {
            break;
        }
    }

    const WorldStats stats = m_world.stats();
    VOXL_LOG_INFO("Warm-up finished after {:.0f} ms: {} chunks resident, {} ready, {} triangles",
                  watch.elapsedMilliseconds(), stats.loadedChunks, stats.readyChunks,
                  stats.triangles);

    // The warm-up burned wall-clock time that is not a frame; starting the clock
    // fresh keeps the first delta from being a multi-second stall.
    m_clock.resetAfterStall();
}

// ------------------------------------------------------------- frame loop --

int Application::run()
{
    while (!m_window.shouldClose()) {
        frame();
    }
    return 0;
}

void Application::frame()
{
    m_clock.tick();

    Stopwatch cpuWatch;

    m_window.pollEvents();
    m_input.newFrame();
    pollInput();

    simulate();
    stream();
    m_uploadMs = drainUploads();

    if (m_window.isIconified() || m_window.width() <= 0 || m_window.height() <= 0) {
        // No framebuffer to draw into, and no swap to pace the loop, so block on
        // the event queue instead of spinning. Streaming and uploads above still
        // ran, which is what keeps a restore from stuttering.
        m_window.waitEvents(0.05);
        return;
    }

    render();

    updateStats();
    drawUi();

    // Sampled before the swap: measuring through it would report the vsync wait
    // as CPU work, which is exactly the number this is meant to exclude. The
    // overlay therefore shows the previous frame's figure.
    m_lastCpuMs = cpuWatch.elapsedMilliseconds();

    m_window.swapBuffers();
}

void Application::pollInput()
{
    const bool captured = m_window.cursorCaptured();

    if (m_input.keyPressed(Key::Escape)) {
        if (captured) {
            setCursorCaptured(false);
        } else {
            m_window.requestClose();
        }
    }

    // Clicking the window recaptures - but not when the click was meant for an
    // ImGui window, or the overlay becomes impossible to interact with.
    if (!captured && m_input.mousePressed(MouseButton::Left) &&
        !ImGui::GetIO().WantCaptureMouse) {
        setCursorCaptured(true);
    }

    if (m_input.keyPressed(Key::F3)) {
        m_overlay.toggle();
    }
    if (m_input.keyPressed(Key::F1)) {
        m_hud.setVisible(!m_hud.visible());
    }
    if (m_input.keyPressed(Key::F5)) {
        const std::size_t reloaded = m_renderer.reloadShaders();
        VOXL_LOG_INFO("Shader reload: {} program(s) rebuilt", reloaded);
    }

    m_playerInput     = PlayerInput{};
    m_interactionInput = InteractionInput{};

    if (!m_window.cursorCaptured()) {
        // Cursor released: the player keeps its velocity but stops steering, so
        // opening the overlay does not walk you off a cliff.
        return;
    }

    const glm::vec2 look = m_input.mouseDelta();
    m_player.look(look.x, look.y);

    m_playerInput.forward = (m_input.keyDown(Key::W) ? 1.0f : 0.0f) -
                            (m_input.keyDown(Key::S) ? 1.0f : 0.0f);
    m_playerInput.strafe = (m_input.keyDown(Key::D) ? 1.0f : 0.0f) -
                           (m_input.keyDown(Key::A) ? 1.0f : 0.0f);
    m_playerInput.jump   = m_input.keyDown(Key::Space);
    m_playerInput.sprint = m_input.keyDown(Key::LeftControl);
    m_playerInput.crouch = m_input.keyDown(Key::LeftShift);
    m_playerInput.flyUp   = m_input.keyDown(Key::Space);
    m_playerInput.flyDown = m_input.keyDown(Key::LeftShift);

    if (m_input.keyPressed(Key::F)) {
        m_player.toggleFly();
        VOXL_LOG_INFO("Fly mode {}", m_player.flying() ? "on" : "off");
    }

    const float scroll = m_input.scrollDelta();
    if (scroll != 0.0f) {
        m_hotbar.cycle(static_cast<int>(scroll > 0.0f ? std::ceil(scroll) : std::floor(scroll)));
    }
    for (std::size_t i = 0; i < kHotbarDigitKeys.size(); ++i) {
        if (m_input.keyPressed(kHotbarDigitKeys[i])) {
            m_hotbar.select(i);
        }
    }

    m_interactionInput.breakHeld    = m_input.mouseDown(MouseButton::Left);
    m_interactionInput.placePressed = m_input.mousePressed(MouseButton::Right);
    m_interactionInput.placeHeld    = m_input.mouseDown(MouseButton::Right);
}

void Application::simulate()
{
    if (!m_simulationStarted) {
        // Holding the simulation until the spawn chunk has voxels is the only
        // thing standing between the player and a fall through a world that has
        // not streamed in yet: out-of-world reads reply "air".
        const ChunkPtr chunk = m_world.chunkContaining(toBlockPos(m_player.position()));
        if (chunk != nullptr && chunk->hasVoxels()) {
            m_simulationStarted = true;
            VOXL_LOG_INFO("Simulation enabled: spawn chunk is populated");
        }
    }

    m_player.setInput(m_playerInput);

    physics::VoxelCollider collider{m_world, m_registry};
    while (m_clock.nextFixedStep()) {
        // The accumulator is drained either way; skipping the step while the
        // world is still loading would otherwise leave a backlog that runs all
        // at once the instant terrain appears.
        if (m_simulationStarted) {
            m_player.step(collider, m_clock.fixedDeltaSeconds());
        }
    }

    m_player.updateCamera(m_camera, m_simulationStarted ? m_clock.fixedAlpha() : 0.0f);
    m_camera.setAspectRatio(m_window.aspectRatio());

    m_interaction.setHeldBlock(m_hotbar.selectedBlock());
    m_interaction.setPlayerAabb(toRenderAabb(m_player.bounds()));
    m_interaction.update(m_camera, m_interactionInput, m_clock.deltaSeconds());
}

void Application::stream()
{
    m_world.update(StreamingView{m_camera.position(), m_camera.forward()}, ++m_streamFrame);
}

float Application::drainUploads()
{
    Stopwatch watch;
    // A non-positive budget means "unlimited" to MainThreadQueue, which is the
    // exact opposite of what a frame-time guard wants, so clamp it.
    const auto budget = m_config.uploadBudget.count() > 0 ? m_config.uploadBudget
                                                          : std::chrono::microseconds{1};
    m_jobs.mainThreadQueue().drain(budget);
    return watch.elapsedMilliseconds();
}

void Application::render()
{
    if (m_window.width() != m_framebufferWidth || m_window.height() != m_framebufferHeight) {
        m_framebufferWidth  = m_window.width();
        m_framebufferHeight = m_window.height();
        m_renderer.resize(m_framebufferWidth, m_framebufferHeight);
    }

    m_overlay.beginGpuFrame();

    m_renderer.beginFrame(m_camera, m_clock.totalSeconds());
    m_renderer.drawSky();
    m_renderer.drawWorld();
    m_interaction.render(m_camera.viewProjection());
    m_renderer.endFrame();

    m_overlay.endGpuFrame();

    m_world.setVisibleChunkCount(m_renderer.stats().chunks.chunksVisible);
}

void Application::drawUi()
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    m_hud.draw(m_hotbar, m_interaction.state(), m_clock.deltaSeconds());
    m_overlay.draw(buildOverlayFrame(m_lastCpuMs, m_uploadMs));

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

DebugOverlayFrame Application::buildOverlayFrame(float cpuMs, float uploadMs) const
{
    DebugOverlayFrame frame;
    frame.clock  = &m_clock;
    frame.camera = &m_camera;
    frame.jobs   = &m_jobStats;

    frame.world.loaded     = m_stats.loadedChunks;
    frame.world.generating = m_stats.chunksByState[static_cast<std::size_t>(ChunkState::Generating)];
    frame.world.generated  = m_stats.chunksByState[static_cast<std::size_t>(ChunkState::Generated)];
    frame.world.meshing    = m_stats.chunksByState[static_cast<std::size_t>(ChunkState::Meshing)];
    frame.world.meshed     = m_stats.chunksByState[static_cast<std::size_t>(ChunkState::Meshed)];
    frame.world.ready      = m_stats.chunksByState[static_cast<std::size_t>(ChunkState::Ready)];
    frame.world.queued     = m_stats.queuedChunks;
    frame.world.visible    = m_stats.visibleChunks;
    frame.world.voxelBytes = m_stats.cpuVoxelBytes;
    frame.world.viewDistanceChunks = m_world.chunks().config().loadRadius;
    frame.world.seed               = m_terrain.seed();

    const RenderStats& render = m_renderer.stats();
    frame.render.drawCalls      = render.drawCalls;
    frame.render.triangles      = static_cast<std::size_t>(render.triangles);
    frame.render.meshBytes      = render.chunks.gpuBytes;
    frame.render.textureBytes   = render.textureBytes;
    frame.render.uploadedMeshes = static_cast<std::size_t>(render.chunks.uploadsTotal);

    const InteractionState& interaction = m_interaction.state();
    frame.target.hasTarget = interaction.hasTarget;
    if (interaction.hasTarget) {
        frame.target.block      = interaction.hit.block;
        frame.target.blockId    = interaction.hit.blockId;
        frame.target.face       = interaction.hit.face;
        frame.target.distance   = interaction.hit.distance;
        const std::uint8_t light = m_world.getLight(interaction.hit.block);
        frame.target.sunlight   = ChunkStorage::unpackSunlight(light);
        frame.target.blockLight = ChunkStorage::unpackBlockLight(light);
    }

    frame.cpuFrameMs = cpuMs;
    frame.uploadMs   = uploadMs;
    frame.gpuName    = m_window.glRendererString();
    frame.glVersion  = m_window.glVersionString();
    return frame;
}

void Application::updateStats()
{
    m_stats    = m_world.stats();
    m_jobStats = m_jobs.stats();

    if (m_config.statusLogSeconds <= 0.0) {
        return;
    }
    const double now = m_clock.totalSeconds();
    if (now < m_nextStatusLog) {
        return;
    }
    m_nextStatusLog = now + m_config.statusLogSeconds;

    const RenderStats& render = m_renderer.stats();
    const glm::vec3& feet = m_player.position();
    VOXL_LOG_INFO(
        "t={:.1f}s fps={:.1f} pos=({:.1f},{:.1f},{:.1f}){} chunks(loaded={} ready={} gen={} "
        "mesh={}) draws={} tris={} gpu={} KiB jobs(queued={} active={})",
        now, static_cast<double>(m_clock.fps()), feet.x, feet.y, feet.z,
        m_player.onGround() ? " grounded" : " airborne", m_stats.loadedChunks, m_stats.readyChunks,
        m_stats.generatingChunks, m_stats.meshingChunks, render.drawCalls, render.triangles,
        render.chunks.gpuBytes / 1024, m_jobStats.queued, m_jobStats.active);
}

void Application::setCursorCaptured(bool captured)
{
    m_window.setCursorCaptured(captured);
    // ImGui must not steer a hidden cursor around; without this the backend
    // warps the OS cursor every frame and fights the mouse-look capture.
    ImGuiIO& io = ImGui::GetIO();
    if (captured) {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
    } else {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
    }
}

}  // namespace voxl
