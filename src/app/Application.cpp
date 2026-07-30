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
    , m_subVoxelAccess(
          [this](const ChunkPos& position) { return m_world.chunks().findReadable(position); })
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
    applyDebugStartup();

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
    // chunk.lod() is set by the streamer before the job runs, so the generator
    // samples terrain at the resolution this chunk will actually be drawn at.
    // Without this the world still looks right - the mesher downsamples whatever
    // it is given - but the generation cost saving, which is most of what LOD buys
    // on the CPU side, is lost.
    m_world.setGenerator([this](Chunk& chunk) { m_terrain.generate(chunk, chunk.lod()); });

    m_world.setMesher([this](const ChunkNeighbourhood& neighbourhood) {
        // Both meshers own scratch and are explicitly not thread safe, so each
        // worker keeps its own. thread_local binds the registry at first use per
        // thread, which is correct here because there is exactly one.
        thread_local GreedyMesher   blockMesher{m_registry};
        thread_local SubVoxelMesher damageMesher{m_registry};

        // ONE snapshot feeds both sweeps. Capturing a second neighbourhood for the
        // damage pass would let the two see different worlds, and re-capturing it
        // needs the chunk map lock on a worker thread, which the whole snapshot
        // design exists to avoid.
        ChunkMeshData data;
        blockMesher.mesh(neighbourhood, data);

        SubVoxelMeshData damage;
        damageMesher.mesh(neighbourhood, damage);

        const LodLevel level = neighbourhood.centre() != nullptr ? neighbourhood.centre()->lod()
                                                                 : kLodFull;

        ChunkMeshUpload upload;
        upload.gpuBytes  = data.byteSize() + damage.byteSize();
        upload.triangles = data.triangleCount() + damage.triangleCount();

        // ONE closure uploads both streams, so no frame can ever observe a chunk
        // whose block geometry and damage geometry came from different versions of
        // the world. Splitting this into two queued closures would allow exactly
        // that for one frame, which shows up as carved geometry floating beside a
        // block that has already been restored.
        //
        // Order matters on the eviction path: upload() evicts a chunk whose mesh
        // is empty unless it still carries damage, so the block mesh must land
        // first and the damage second.
        upload.upload = [this, position = neighbourhood.centrePos(), level,
                         mesh = std::move(data), damage = std::move(damage)] {
            m_renderer.chunks().upload(mesh, level);
            m_renderer.chunks().uploadSubVoxels(position, damage);
        };
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

void Application::applyDebugStartup()
{
    const DebugStartup& debug = m_config.debug;
    if (!debug.any()) {
        return;
    }

    if (debug.hasPosition) {
        m_player.setPosition(debug.position);
        m_player.setVelocity(glm::vec3{0.0f});
    }
    if (debug.hasRotation) {
        m_player.setRotation(debug.yawDegrees, debug.pitchDegrees);
    }
    // Before the first streaming update, so warmUp() loads the world around the
    // scripted viewpoint instead of around the spawn point it replaced.
    m_player.updateCamera(m_camera, 0.0f);

    if (debug.freezePlayer) {
        m_player.setFlying(true);  // so nothing in the collider tries to ground it
    }

    LodPolicy policy = m_world.lodPolicy();
    policy.enabled   = debug.lodEnabled;
    if (debug.hasBands) {
        for (std::size_t i = 0; i < std::size_t{kLodMax}; ++i) {
            policy.bandStart[i] = debug.bandStart[i];
        }
    }
    m_world.setLodPolicy(policy);

    m_overlay.setVisible(debug.showOverlay);
    m_hud.setVisible(!debug.hideHud);

    static_assert(kLodMax == 3, "the band log below names three bands by hand");
    VOXL_LOG_INFO(
        "Debug startup: pos=({:.1f},{:.1f},{:.1f}) yaw={:.1f} pitch={:.1f} freeze={} lod={} "
        "bands=[{} {} {}] carve={}",
        m_player.position().x, m_player.position().y, m_player.position().z,
        m_player.yawDegrees(), m_player.pitchDegrees(), debug.freezePlayer, policy.enabled,
        policy.bandStart[0], policy.bandStart[1], policy.bandStart[2],
        static_cast<int>(debug.carve));
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
    // After stream(), which is what drains the deferral queue this batch feeds.
    updateDebugScript();
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
    if (m_input.keyPressed(Key::F6)) {
        carveTargetSubVoxel();
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

    // A frozen debug camera must not integrate at all: even flying, drag and the
    // fixed-step interpolation would drift it off the scripted framing, and two
    // captures meant to differ only by a setting would differ by a few metres.
    const bool frozen = m_config.debug.freezePlayer;

    // The third argument is what lets the player stand in a carved alcove and
    // walk down a carved tunnel. Blocks with no damage - which is all of them in
    // untouched terrain - take exactly the single-AABB path they always did.
    physics::VoxelCollider collider{m_world, m_registry, &m_subVoxelAccess};
    while (m_clock.nextFixedStep()) {
        // The accumulator is drained either way; skipping the step while the
        // world is still loading would otherwise leave a backlog that runs all
        // at once the instant terrain appears.
        if (m_simulationStarted && !frozen) {
            m_player.step(collider, m_clock.fixedDeltaSeconds());
        }
    }

    m_player.updateCamera(m_camera,
                          m_simulationStarted && !frozen ? m_clock.fixedAlpha() : 0.0f);
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

    // Resident counts come from the world (it owns the chunks and their levels);
    // visible/draw/triangle counts come from the renderer (it owns culling). Both
    // halves are needed to answer "is the outer ring actually cheap".
    frame.lod.enabled = m_world.lodPolicy().enabled;
    for (std::size_t level = 0; level < kLodCount; ++level) {
        frame.lod.residentChunks[level] = m_stats.chunksByLod[level];
        frame.lod.visibleChunks[level]  = render.chunks.chunksVisiblePerLod[level];
        frame.lod.drawCalls[level]      = render.chunks.drawCallsPerLod[level];
        frame.lod.triangles[level] = static_cast<std::size_t>(render.chunks.trianglesPerLod[level]);
    }

    frame.subVoxel.damagedBlocks = m_stats.damagedBlocks;
    frame.subVoxel.cpuBytes      = m_stats.subVoxelBytes;
    frame.subVoxel.damagedChunks = render.chunks.subVoxelChunksResident;
    frame.subVoxel.drawCalls     = render.chunks.subVoxelDrawCalls;
    frame.subVoxel.triangles     = static_cast<std::size_t>(render.chunks.subVoxelTriangles);
    frame.subVoxel.gpuBytes      = render.chunks.subVoxelGpuBytes;

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

    // Logged on its own line, and unconditionally: "are several levels actually
    // resident" is the one question a screenshot cannot answer, and a run where
    // every chunk sits at level 0 means the policy silently did nothing.
    VOXL_LOG_INFO(
        "  lod{} resident=[{} {} {} {}] visible=[{} {} {} {}] tris=[{} {} {} {}] "
        "transitions={} dropped={} inflight={}",
        m_world.lodPolicy().enabled ? "" : " (DISABLED)", m_stats.chunksByLod[0],
        m_stats.chunksByLod[1], m_stats.chunksByLod[2], m_stats.chunksByLod[3],
        render.chunks.chunksVisiblePerLod[0], render.chunks.chunksVisiblePerLod[1],
        render.chunks.chunksVisiblePerLod[2], render.chunks.chunksVisiblePerLod[3],
        render.chunks.trianglesPerLod[0], render.chunks.trianglesPerLod[1],
        render.chunks.trianglesPerLod[2], render.chunks.trianglesPerLod[3],
        m_stats.lodTransitions, m_stats.lodTransitionsDropped, m_stats.lodJobsInFlight);

    if (m_stats.damagedBlocks != 0 || render.chunks.subVoxelDrawCalls != 0) {
        VOXL_LOG_INFO("  subvoxel blocks={} chunks={} draws={} tris={} gpu={} B cpu={} B",
                      m_stats.damagedBlocks, render.chunks.subVoxelChunksResident,
                      render.chunks.subVoxelDrawCalls, render.chunks.subVoxelTriangles,
                      render.chunks.subVoxelGpuBytes, m_stats.subVoxelBytes);
    }
}

void Application::carveTargetSubVoxel()
{
    // A DEBUG AFFORDANCE, not the shipping mining mode.
    //
    // Sub-voxel destruction has a complete pipeline behind it - store, mesher,
    // its own GPU buffers and program, sub-voxel collision - but no gameplay verb
    // that drives it: the break timer, the per-sub-voxel targeting reticle and the
    // tool/mode rules that would decide when a swing chips a block instead of
    // removing it are a gameplay design that is not written. Rather than invent a
    // half-specified mining mode, F6 exposes the primitive so the path can be
    // exercised, seen and profiled. Delete this when the real verb lands.
    const InteractionState& state = m_interaction.state();
    if (!state.hasTarget) {
        VOXL_LOG_INFO("F6: nothing targeted");
        return;
    }

    // The hit point lies exactly ON the block face, where the scaled sub-voxel
    // coordinate is 0 or 8 and rounding can put it on either side. Step half a
    // sub-voxel along the inward normal so it unambiguously lands in the block
    // that was actually hit.
    const glm::vec3 inward =
        -kDirectionNormals[static_cast<std::size_t>(state.hit.face)] * (0.5f * kSubVoxelSize);
    const glm::vec3 point = state.hit.point + inward;

    const EditResult result = m_world.breakSubVoxelAt(point);
    const SubVoxelHit hit   = toSubVoxel(point);
    VOXL_LOG_INFO("F6: carve sub-voxel ({},{},{}) of block ({},{},{}) -> {}", hit.sx, hit.sy, hit.sz,
                  hit.block.x, hit.block.y, hit.block.z,
                  result == EditResult::Applied    ? "applied"
                  : result == EditResult::Deferred ? "deferred"
                                                   : "rejected");
}

// ------------------------------------------------- scripted visual review --
//
// See DebugStartup in the header for why any of this exists. Two rigs:
//
//  CRATER  a sphere of sub-voxels removed from the natural terrain surface. The
//          point of comparison is the rim: partially destroyed grass and dirt
//          blocks sitting directly against intact ones, lit by the same sky.
//
//  TUNNEL  a stone slab raised clear of the ground, bored through end to end.
//          Raised deliberately - sky behind the far mouth turns any missing face
//          or cracked seam into an obvious bright hole, and a slab standing in
//          open air is the only place a carved surface is guaranteed to be lit
//          while there is no light propagation to carry daylight underground.

namespace {

constexpr std::int32_t kCraterRadiusSub = 12;  ///< 1.5 blocks

constexpr std::int32_t kSlabWidth  = 5;   ///< blocks along X
constexpr std::int32_t kSlabHeight = 6;   ///< blocks along Y
constexpr std::int32_t kSlabDepth  = 7;   ///< blocks along Z, the bore's length
constexpr std::int32_t kSlabLift   = 3;   ///< blocks of clear air under the slab
constexpr std::int32_t kBoreRadiusSub = 8;  ///< 1 block

/// Edits handed to the World per frame. Chosen so a full rig lands in well under
/// a second while staying far below World::kMaxDeferredEdits even if every one of
/// them is deferred.
constexpr std::size_t kDebugEditsPerFrame = 384;

}  // namespace

void Application::updateDebugScript()
{
    if (m_config.debug.carve == DebugStartup::Carve::None || m_debugRigDone) {
        return;
    }

    const DebugStartup& debug   = m_config.debug;
    const bool wantCrater = debug.carve == DebugStartup::Carve::Crater ||
                            debug.carve == DebugStartup::Carve::Both;
    const bool wantTunnel = debug.carve == DebugStartup::Carve::Tunnel ||
                            debug.carve == DebugStartup::Carve::Both;

    if (!m_debugRigPlanned) {
        const BlockPos anchor =
            debug.hasCarveAnchor
                ? debug.carveAnchor
                : BlockPos{physics::floorToInt(m_player.position().x),
                           m_terrain.surfaceHeight(physics::floorToInt(m_player.position().x),
                                                   physics::floorToInt(m_player.position().z)),
                           physics::floorToInt(m_player.position().z)};

        // Nothing may be planned against terrain that has not arrived: the
        // crater's centre column decides where the sphere sits, and a chunk that
        // is still generating reads as air.
        if (!m_world.isChunkReady(toChunkPos(anchor))) {
            return;
        }

        const auto addSub = [this](std::int32_t sx, std::int32_t sy, std::int32_t sz) {
            // Arithmetic shift, so the rig works at negative world coordinates:
            // sx / 8 would round toward zero and fold two blocks into one.
            m_debugCarves.emplace_back(
                BlockPos{sx >> kSubVoxelShift, sy >> kSubVoxelShift, sz >> kSubVoxelShift},
                static_cast<std::uint16_t>(subVoxelIndex(sx & kSubVoxelMask, sy & kSubVoxelMask,
                                                         sz & kSubVoxelMask)));
        };

        if (wantCrater) {
            const std::int32_t cx = anchor.x * kSubVoxelResolution + kSubVoxelResolution / 2;
            const std::int32_t cy = anchor.y * kSubVoxelResolution + kSubVoxelResolution / 2;
            const std::int32_t cz = anchor.z * kSubVoxelResolution + kSubVoxelResolution / 2;
            const std::int32_t r  = kCraterRadiusSub;
            for (std::int32_t dy = -r; dy <= r; ++dy) {
                for (std::int32_t dz = -r; dz <= r; ++dz) {
                    for (std::int32_t dx = -r; dx <= r; ++dx) {
                        if (dx * dx + dy * dy + dz * dz <= r * r) {
                            addSub(cx + dx, cy + dy, cz + dz);
                        }
                    }
                }
            }
            VOXL_LOG_INFO("Debug rig: crater centre block ({}, {}, {}) radius {} sub-voxels",
                          anchor.x, anchor.y, anchor.z, r);
        }

        if (wantTunnel) {
            // Clear of the crater along X so one wide shot can hold both.
            const BlockPos base{anchor.x + 6, anchor.y + kSlabLift, anchor.z - kSlabDepth / 2};
            for (std::int32_t dy = 0; dy < kSlabHeight; ++dy) {
                for (std::int32_t dz = 0; dz < kSlabDepth; ++dz) {
                    for (std::int32_t dx = 0; dx < kSlabWidth; ++dx) {
                        m_debugBuild.emplace_back(
                            BlockPos{base.x + dx, base.y + dy, base.z + dz}, blocks::Stone);
                    }
                }
            }

            const std::int32_t cx = base.x * kSubVoxelResolution +
                                    kSlabWidth * kSubVoxelResolution / 2;
            const std::int32_t cy = base.y * kSubVoxelResolution +
                                    kSlabHeight * kSubVoxelResolution / 2;
            const std::int32_t z0 = base.z * kSubVoxelResolution;
            const std::int32_t z1 = (base.z + kSlabDepth) * kSubVoxelResolution;
            const std::int32_t r  = kBoreRadiusSub;
            for (std::int32_t sz = z0; sz < z1; ++sz) {
                for (std::int32_t dy = -r; dy <= r; ++dy) {
                    for (std::int32_t dx = -r; dx <= r; ++dx) {
                        if (dx * dx + dy * dy <= r * r) {
                            addSub(cx + dx, cy + dy, sz);
                        }
                    }
                }
            }
            VOXL_LOG_INFO(
                "Debug rig: slab ({}, {}, {}) size {}x{}x{}, bore along +Z centred on "
                "x={:.2f} y={:.2f}, radius {} sub-voxels",
                base.x, base.y, base.z, kSlabWidth, kSlabHeight, kSlabDepth,
                static_cast<double>(cx) / kSubVoxelResolution,
                static_cast<double>(cy) / kSubVoxelResolution, r);
        }

        m_debugRigPlanned = true;
        VOXL_LOG_INFO("Debug rig planned: {} block placements, {} sub-voxel carves",
                      m_debugBuild.size(), m_debugCarves.size());
    }

    // The deferral queue is shared with normal gameplay edits, so leave it room
    // rather than filling it to the brim.
    const std::size_t deferred = m_world.stats().deferredEdits;
    if (deferred + kDebugEditsPerFrame > World::kMaxDeferredEdits) {
        return;
    }

    std::size_t budget = kDebugEditsPerFrame;
    while (budget != 0 && m_debugBuildCursor < m_debugBuild.size()) {
        const auto& [position, id] = m_debugBuild[m_debugBuildCursor];
        const EditResult result    = m_world.setBlock(position, id);
        if (result == EditResult::NotLoaded) {
            return;  // the slab's chunk has not arrived; retry the same entry next frame
        }
        ++m_debugBuildCursor;
        --budget;
    }

    // The bore must not start until every block it passes through exists, or the
    // leading carves fall on air and silently do nothing.
    if (m_debugBuildCursor < m_debugBuild.size() || deferred != 0) {
        return;
    }

    while (budget != 0 && m_debugCarveCursor < m_debugCarves.size()) {
        const auto& [position, subIndex] = m_debugCarves[m_debugCarveCursor];
        if (m_world.breakSubVoxel(position, subIndex) == EditResult::NotLoaded) {
            return;
        }
        ++m_debugCarveCursor;
        --budget;
    }

    if (m_debugCarveCursor == m_debugCarves.size()) {
        m_debugRigDone = true;
        const WorldStats stats = m_world.stats();
        VOXL_LOG_INFO("Debug rig complete: {} damaged blocks, {} B of sub-voxel storage",
                      stats.damagedBlocks, stats.subVoxelBytes);
    }
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
