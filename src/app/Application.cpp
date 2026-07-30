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
#include <memory>
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

[[nodiscard]] glm::vec3 blockCentre(const BlockPos& block) noexcept
{
    return glm::vec3{static_cast<float>(block.x) + 0.5f, static_cast<float>(block.y) + 0.5f,
                     static_cast<float>(block.z) + 0.5f};
}

constexpr std::array<Key, 9> kHotbarDigitKeys{Key::Num1, Key::Num2, Key::Num3,
                                              Key::Num4, Key::Num5, Key::Num6,
                                              Key::Num7, Key::Num8, Key::Num9};

/// How long shutdown waits for in-flight chunk jobs before giving up on them.
/// Generous: the alternative to waiting is tearing down a world a worker is
/// still writing into.
constexpr std::chrono::milliseconds kJobDrainTimeout{5000};

}  // namespace

// ------------------------------------------------------------- ImGuiScope --

Application::ImGuiScope::ImGuiScope(Window& window)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // No StyleColorsDark() here: SettingsPanel::applyTheme() owns the whole
    // style, including the GUI scale, and is called by the Application as soon
    // as this scope exists. Setting a base theme first would just be overwritten.
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

// ---------------------------------------------------------- construction --

namespace {

[[nodiscard]] RendererConfig makeRendererConfig()
{
    RendererConfig config;
    config.assetRoot = resolveAssetRoot();
    return config;
}

}  // namespace

ApplicationConfig Application::prepareConfig(ApplicationConfig config)
{
    if (config.settingsPath.empty()) {
        config.settingsPath = defaultSettingsPath();
    }
    if (config.savesRoot.empty()) {
        config.savesRoot = defaultSavesDirectory();
    }

    SettingsParseReport report;
    if (loadSettingsFile(config.settingsPath, config.settings, &report)) {
        VOXL_LOG_INFO("settings: loaded '{}' ({} key(s), {} unknown, {} malformed, {} clamped)",
                      config.settingsPath.string(), report.keysApplied, report.unknownKeys,
                      report.malformedLines, report.clampedValues);
    } else {
        VOXL_LOG_INFO("settings: '{}' not readable; using defaults",
                      config.settingsPath.string());
    }
    config.unknownSettingLines = std::move(report.unknownLines);

    // Command-line overrides are folded in AFTER the file, because readSettings
    // resets its output to defaults before parsing - anything written into
    // `config.settings` earlier would have been discarded.
    if (config.dayLengthMinutesOverride > 0.0f) {
        config.settings.dayLengthMinutes = config.dayLengthMinutesOverride;
        config.settings.clampToValidRange();
    }

    // The file feeds the two values that are construction parameters rather than
    // live state. A command-line override still wins - a debug flag that the
    // settings file could silently undo would make every capture unreproducible.
    config.window.vsync = config.settings.vsync;
    if (!config.radiusOverridden) {
        config.streaming.loadRadius = config.settings.renderDistance;
    } else {
        // Keep the two in step: applySettings(Streaming) reads renderDistance,
        // so leaving it at the file's value would undo --radius on the first
        // settings change of the session.
        config.settings.renderDistance = config.streaming.loadRadius;
        config.settings.clampToValidRange();
        config.streaming.loadRadius = config.settings.renderDistance;
    }

    return config;
}

Application::Application(const ApplicationConfig& config)
    : m_config(prepareConfig(config))
    , m_settings(m_config.settings)
    , m_unknownSettingLines(m_config.unknownSettingLines)
    , m_settingsPath(m_config.settingsPath)
    , m_window(m_config.window)
    , m_input(m_window)
    , m_imgui(m_window)
    , m_jobs()
    , m_audio()
    , m_registry(createDefaultBlockRegistry())
    , m_renderer(makeRendererConfig())
    , m_world(m_jobs, m_registry, m_config.streaming)
    , m_subVoxelAccess(
          [this](const ChunkPos& position) { return m_world.chunks().findReadable(position); })
    , m_interaction(m_registry)
    , m_hud(&m_registry)
    , m_overlay(&m_registry)
{
    VOXL_LOG_INFO("Application starting: {} worker threads, load radius {}, lighting {}",
                  m_jobs.workerCount(), m_config.streaming.loadRadius,
                  m_config.streaming.lighting ? "on" : "off");

    if (!m_renderer.valid()) {
        // Not fatal: the window, the world and the overlay still work, and the
        // log already carries the shader/texture failure that caused it.
        VOXL_LOG_ERROR("Renderer failed to initialise; running without world rendering");
    }

    SettingsPanel::applyTheme(m_settings.guiScale);

    m_framebufferWidth  = m_window.width();
    m_framebufferHeight = m_window.height();
    m_renderer.resize(m_framebufferWidth, m_framebufferHeight);

    m_overlay.setSelfToggleKey(false);  // the input layer owns F3; both would double-fire
    m_interaction.setReach(m_player.config().reach);

    // Audio before the world: synthesising the bank is ~70 ms and the device
    // open can fail, and both are better discovered before four seconds of
    // terrain streaming than after.
    m_audio.setSoundBank(std::make_shared<const audio::SoundBank>(audio::SoundBank::createDefault()));
    if (m_audio.available()) {
        m_windCue = m_audio.findCue("ambient.wind");
        m_audio.play(m_windCue);  // looping, non-spatial
    } else {
        VOXL_LOG_WARN("Running without audio: {}", m_audio.unavailableReason());
    }

    wireWorld();

    // Everything the settings file controls, applied in one pass so the first
    // frame is already at the player's chosen values.
    applySettings(SettingsDirty::All);

    m_mainMenu.setVersionLabel("Voxl " VOXL_VERSION);
    m_mainMenu.setWorldProvider([this] { return enumerateWorlds(); });

    if (m_config.startInMainMenu) {
        enterMainMenu();
    } else {
        openWorld(m_config.worldName, m_config.terrain.seed);
    }
}

Application::~Application()
{
    VOXL_LOG_INFO("Application shutting down");

    // ---- ORDER (a): everything that must happen while the pool still runs --
    //
    // closeWorld() saves, and WorldSave dispatches its writes to workers. Doing
    // this after m_jobs.shutdown() would enqueue jobs nothing will ever run,
    // stranding flush() until its timeout and losing the chunks it was waiting
    // on. It also retires every chunk, which releases GPU meshes through the
    // renderer - fine here, because the GL context is still current.
    if (m_worldOpen) {
        closeWorld();
    }

    // ---- ORDER (b): stop the pool before the world it holds a reference to --
    //
    // Drain rather than Cancel because a cancelled job's broken promise is
    // indistinguishable from a crash in the log, and because ChunkManager's own
    // destructor would then wait five seconds for jobs that will never run.
    m_jobs.shutdown(ShutdownMode::Drain);

    // Whatever those jobs posted for the main thread will never be drained, and
    // its closures hold references to the renderer. Drop them while everything
    // they capture is still alive.
    m_jobs.mainThreadQueue().clear();

    // Belt and braces: closeWorld() already did this, but a constructor that
    // threw before the world opened still leaves chunks resident.
    m_world.unloadAll();
    m_renderer.chunks().clear();
}

void Application::wireWorld()
{
    // Load-before-generate. `loadChunk` runs on the worker that owns this chunk
    // in state Generating, which is the same licence the terrain generator has
    // to write voxels; it returns Absent immediately for a coarse LOD, which is
    // why the fallback below still samples terrain at chunk.lod().
    //
    // chunk.lod() is set by the streamer before the job runs, so the generator
    // samples terrain at the resolution this chunk will actually be drawn at.
    // Without this the world still looks right - the mesher downsamples whatever
    // it is given - but the generation cost saving, which is most of what LOD buys
    // on the CPU side, is lost.
    m_world.setGenerator([this](Chunk& chunk) {
        if (m_save.has_value() && !m_save->loadChunk(chunk).regenerate()) {
            // Bytes on disk mean the player changed this chunk, so it must never
            // be regenerated from the seed at another LOD. Recording it here as
            // well as in indexStoredChunks() is belt and braces: it keeps the
            // registry correct even for a world whose index was never built.
            m_save->noteStoredChunk(chunk.position());
            return;
        }
        if (m_terrain.has_value()) {
            m_terrain->generate(chunk, chunk.lod());
        }
    });

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

    // ChunkManager has ONE retire hook slot. It goes to persistence; anything
    // else that needs to observe a retirement has to chain through this lambda.
    m_world.chunks().setRetireHook([this](const ChunkPtr& chunk) {
        if (!m_save.has_value() || !chunk) {
            return;
        }
        // Sampled BEFORE the save, because saveChunk() clears it. A retirement
        // that writes bytes is a position that from now on diverges from
        // generated terrain, and the LOD decision has to keep knowing that after
        // the flag is gone.
        const bool diverged = chunk->needsSave();
        m_save->onChunkRetired(chunk);
        if (diverged) {
            m_save->noteStoredChunk(chunk->position());
        }
    });

    // What stops a player's build being regenerated from the seed when it
    // demotes a LOD level. Routed through the optional rather than binding
    // &*m_save, so closing a world cannot leave ChunkManager holding a predicate
    // into a destroyed WorldSave - which is also why no closeWorld() teardown is
    // needed. See ChunkDivergedFn and the divergence note in world/WorldSave.hpp.
    m_world.chunks().setDivergedPredicate([this](const ChunkPos& position) {
        return m_save.has_value() && m_save->hasStoredChunk(position);
    });

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

    // ---- the sub-voxel mining verb ----
    //
    // Deferred counts as carved: re-issuing a deferred carve on the next frame
    // would apply it twice, and the deferral queue exists precisely so the
    // caller does not have to retry.
    m_interaction.setSubVoxelBreaker([this](const glm::vec3& point) {
        const EditResult result = m_world.breakSubVoxelAt(point);
        return (result == EditResult::Applied || result == EditResult::Deferred)
                   ? CarveOutcome::Carved
                   : CarveOutcome::Refused;
    });
    m_interaction.setSubVoxelDamageReader(
        [this](const BlockPos& position) { return !m_world.isBlockWhole(position); });
}

// -------------------------------------------------------- world session --

void Application::openWorld(std::string name, std::uint64_t seed)
{
    if (m_worldOpen) {
        closeWorld();
    }

    const std::filesystem::path directory = m_config.savesRoot / name;

    // The stored seed WINS. Every region file records the seed it was written
    // with and refuses to open under any other, so honouring the requested seed
    // against an existing directory would produce a world that generates fresh
    // terrain and cannot read a single one of its own saved chunks.
    //
    // Which is exactly what used to happen whenever level.vxw was damaged: the
    // fallback ran the session on `seed`, every region failed SeedMismatch, the
    // world became unloadable AND unsavable, and one truncated 56-byte file
    // discarded everything the player had built. resolveWorldSeed() therefore
    // asks the region headers - the seed's second copy - before it gives up. See
    // the note above RegionHeaderInfo in world/WorldSave.hpp.
    std::uint64_t effectiveSeed = seed;
    WorldMetadata metadata;
    bool          hasMetadata = false;
    if (m_config.persistence) {
        const WorldSeedResolution resolution = resolveWorldSeed(directory, seed);
        effectiveSeed = resolution.seed;
        hasMetadata   = resolution.hasMetadata;
        metadata      = resolution.metadata;

        // Which path was taken is never silent. Running on the wrong seed looks
        // exactly like running on the right one until every chunk turns out to
        // be missing, so the log is the only warning anyone gets.
        switch (resolution.source) {
        case SeedSource::Metadata:
            if (metadata.seed != seed) {
                VOXL_LOG_INFO("world '{}': using the stored seed {} rather than the requested {}",
                              name, metadata.seed, seed);
            }
            break;

        case SeedSource::RegionHeader:
            VOXL_LOG_WARN("world '{}': level.vxw is unusable ({}), so the seed was RECOVERED as "
                          "{} from the header of '{}'. The world's chunks load normally and a "
                          "fresh level.vxw is written on the next save; the player position, time "
                          "of day and play time that file held are lost",
                          name, toString(resolution.metadataError), effectiveSeed,
                          resolution.seedFile.filename().string());
            break;

        case SeedSource::Requested:
            if (resolution.regionsPresentButUnreadable) {
                VOXL_LOG_ERROR("world '{}': level.vxw is unusable ({}) and not one region header "
                               "in '{}' could be read either. Falling back to the requested seed "
                               "{}; any region that is still intact belongs to a different seed "
                               "and will refuse to load",
                               name, toString(resolution.metadataError), directory.string(), seed);
            } else if (resolution.metadataError != SaveError::None) {
                VOXL_LOG_ERROR("world '{}': level.vxw is unusable ({}) and there are no region "
                               "files to recover the seed from. Starting on the requested seed {}",
                               name, toString(resolution.metadataError), seed);
            }
            break;
        }
    }

    // No narrowing here. TerrainSettings::seed is 64 bits and so is the seed
    // stamped into every region header; truncating to 32 made the generator and
    // the save layer disagree, and every region then refused to open with
    // SeedMismatch on the very next launch.
    TerrainSettings terrain = m_config.terrain;
    terrain.seed            = effectiveSeed;
    m_terrain.emplace(terrain);

    if (m_config.persistence) {
        std::error_code code;
        std::filesystem::create_directories(directory, code);
        if (code) {
            VOXL_LOG_ERROR("cannot create save directory '{}': {}; this session will not persist",
                           directory.string(), code.message());
        } else {
            m_save.emplace(m_jobs, directory, effectiveSeed);
            m_save->setAutosaveIntervalSeconds(m_config.autosaveSeconds);

            // Wired before anything can stream, because the very first autosave
            // may land while a column light job is rewriting a chunk's light
            // array. See WorldSave::setBusyProbe; the manager outlives every
            // WorldSave, so the captured reference is safe for the whole session.
            m_save->setBusyProbe([this](const ChunkPos& position) {
                return m_world.chunks().isNeighbourhoodBusy(position);
            });

            // Before anything streams: every chunk already on disk is player
            // work and must keep the level it was built at. Doing this at open
            // rather than lazily is what protects a build from a PREVIOUS
            // session - the first time the player walks back into range, the
            // streamer has to already know the chunk is not regenerable.
            const std::size_t stored = m_save->indexStoredChunks();
            if (stored != 0) {
                VOXL_LOG_INFO("world '{}': {} saved chunk(s) indexed; they keep their level "
                              "instead of being regenerated from the seed",
                              name, stored);
            }
        }
    }

    m_worldName       = std::move(name);
    m_worldOpen       = true;
    m_playTimeSeconds = hasMetadata ? metadata.playTimeSeconds : 0.0;
    m_pauseMenu.setWorldName(m_worldName);

    m_simulationStarted = false;
    m_streamFrame       = 0;

    spawnPlayer();

    if (hasMetadata) {
        // Restored before applyDebugStartup(), so a --pos on the command line
        // still wins over where the player last stood.
        m_player.setPosition(metadata.playerPosition);
        m_player.setVelocity(glm::vec3{0.0f});
        m_player.setRotation(metadata.yawDegrees, metadata.pitchDegrees);
        m_player.updateCamera(m_camera, 0.0f);
        m_hotbar.select(metadata.hotbarSlot);
        m_dayNight.setTimeOfDay(metadata.timeOfDay);
        VOXL_LOG_INFO("Restored '{}': pos=({:.1f},{:.1f},{:.1f}) time={} play={:.0f}s",
                      m_worldName, metadata.playerPosition.x, metadata.playerPosition.y,
                      metadata.playerPosition.z, DayNightCycle::clockText(metadata.timeOfDay),
                      metadata.playTimeSeconds);
    }

    applyDebugStartup();

    m_lastFootPosition = m_player.position();
    m_footstepDistance = 0.0f;

    enterPlaying();
    warmUp();
}

void Application::closeWorld()
{
    if (!m_worldOpen) {
        return;
    }
    VOXL_LOG_INFO("Closing world '{}'", m_worldName);

    // 1. No worker may be mid-generate on a chunk we are about to save or
    //    retire. ChunkManager::unloadAll forces the state even on a busy chunk,
    //    which would defeat saveChunk's Generating guard.
    if (!m_world.chunks().waitForPendingJobs(kJobDrainTimeout)) {
        VOXL_LOG_WARN("Timed out waiting for chunk jobs before closing the world");
    }

    // 2. Queued uploads reference chunks that are about to be retired. Running
    //    them would upload geometry we then immediately free; dropping them is
    //    both correct and faster.
    m_jobs.mainThreadQueue().clear();

    // 3. Persist. Metadata first so a crash between the two still leaves a
    //    loadable world at a slightly older position rather than an unreadable
    //    directory.
    saveEverything();

    // 4. Retire everything. The retire hook queues a save for any chunk that is
    //    still dirty, so this must run before the flush below.
    m_world.unloadAll();

    if (m_save.has_value()) {
        m_save->flush();
        const SaveStats stats = m_save->stats();
        VOXL_LOG_INFO("Save: {} chunk(s) written, {} KiB, {} load(s), {} absent, {} corrupt, "
                      "{} failure(s)",
                      stats.chunksWritten, stats.bytesWritten / 1024, stats.chunksLoaded,
                      stats.chunksAbsent, stats.chunksCorrupt, stats.writeFailures);
    }

    // 5. GPU meshes, while the context is up.
    m_renderer.chunks().clear();

    m_save.reset();
    m_terrain.reset();
    m_worldOpen         = false;
    m_simulationStarted = false;
    m_miningSound.stop(m_audio, 0.0f);
}

WorldMetadata Application::currentMetadata() const
{
    WorldMetadata metadata;
    metadata.seed            = m_terrain.has_value() ? m_terrain->seed() : 0;
    metadata.playerPosition  = m_player.position();
    metadata.yawDegrees      = m_player.yawDegrees();
    metadata.pitchDegrees    = m_player.pitchDegrees();
    metadata.hotbarSlot      = static_cast<std::uint8_t>(m_hotbar.selectedIndex());
    metadata.timeOfDay       = m_dayNight.timeOfDay();
    metadata.playTimeSeconds = m_playTimeSeconds;
    return metadata;
}

void Application::saveEverything()
{
    if (!m_save.has_value()) {
        return;
    }

    // forEachChunk holds the chunk map's shared lock for the whole callback, so
    // the body must stay to a pointer copy and an atomic load. Encoding inside
    // it would block every worker looking for a neighbour.
    std::vector<ChunkPtr> dirty;
    m_world.chunks().forEachChunk([&dirty](const ChunkPos&, const ChunkPtr& chunk) {
        if (chunk && chunk->needsSave()) {
            dirty.push_back(chunk);
        }
    });

    // Open-coded rather than WorldSave::saveChunks() only because the registry
    // update needs to see WHICH chunks were queued: a queued write means the
    // position now diverges from generated terrain permanently, while
    // saveChunk() is about to clear the per-chunk flag that used to carry that
    // meaning. See the divergence note in world/WorldSave.hpp.
    std::size_t queued = 0;
    for (const ChunkPtr& chunk : dirty) {
        if (chunk && chunk->needsSave() && m_save->saveChunk(chunk)) {
            m_save->noteStoredChunk(chunk->position());
            ++queued;
        }
    }

    m_save->writeMetadata(currentMetadata());
    if (queued != 0) {
        VOXL_LOG_INFO("Autosave: {} chunk(s) queued", queued);
    }
}

std::vector<WorldEntry> Application::enumerateWorlds() const
{
    std::vector<WorldEntry> entries;

    std::error_code code;
    if (!std::filesystem::exists(m_config.savesRoot, code)) {
        return entries;
    }

    for (const auto& entry : std::filesystem::directory_iterator(m_config.savesRoot, code)) {
        if (!entry.is_directory()) {
            continue;
        }
        const std::filesystem::path directory = entry.path();
        const std::filesystem::path metaPath  = WorldSave::metadataPath(directory);
        std::error_code             probe;
        if (!std::filesystem::exists(metaPath, probe)) {
            continue;
        }

        WorldEntry world;
        world.name      = directory.filename().string();
        world.directory = directory;

        WorldMetadata metadata;
        SaveError     error = SaveError::None;
        if (readWorldMetadata(directory, metadata, error)) {
            world.seed = metadata.seed;
        }

        // last_write_time's clock is unspecified, so it is converted through the
        // system clock rather than assumed to already be Unix epoch based.
        const auto written = std::filesystem::last_write_time(metaPath, probe);
        if (!probe) {
            const auto systemTime = std::chrono::clock_cast<std::chrono::system_clock>(written);
            world.lastPlayedUnixSeconds = std::chrono::duration_cast<std::chrono::seconds>(
                                              systemTime.time_since_epoch())
                                              .count();
        }
        entries.push_back(std::move(world));
    }

    std::sort(entries.begin(), entries.end(), [](const WorldEntry& a, const WorldEntry& b) {
        return a.lastPlayedUnixSeconds > b.lastPlayedUnixSeconds;
    });
    return entries;
}

void Application::spawnPlayer()
{
    const std::int32_t surface = m_terrain.has_value() ? m_terrain->surfaceHeight(0, 0) : 0;
    const float spawnY = static_cast<float>(std::max(surface + 1, m_config.terrain.seaLevel + 1));
    m_player.setPosition(glm::vec3{0.5f, spawnY, 0.5f});
    m_player.setVelocity(glm::vec3{0.0f});
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

    if (debug.hasTime) {
        m_dayNight.setTimeOfDay(debug.timeOfDay);
    }
    // --freeze implies --freeze-time: a frozen camera under a moving sky makes
    // two captures that were meant to differ by one setting differ by the sky.
    if (debug.freezeTime || debug.freezePlayer) {
        m_dayNight.setPaused(true);
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
        "bands=[{} {} {}] carve={} time={}",
        m_player.position().x, m_player.position().y, m_player.position().z,
        m_player.yawDegrees(), m_player.pitchDegrees(), debug.freezePlayer, policy.enabled,
        policy.bandStart[0], policy.bandStart[1], policy.bandStart[2],
        static_cast<int>(debug.carve), m_dayNight.clockText());
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
    VOXL_LOG_INFO("Warm-up finished after {:.0f} ms: {} chunks resident, {} ready, {} triangles, "
                  "{} column(s) lit",
                  watch.elapsedMilliseconds(), stats.loadedChunks, stats.readyChunks,
                  stats.triangles, stats.lightColumnsLit);

    // The warm-up burned wall-clock time that is not a frame; starting the clock
    // fresh keeps the first delta from being a multi-second stall.
    m_clock.resetAfterStall();
    m_frameLimiter.reset();
}

// ------------------------------------------------------- state machine --

void Application::enterPlaying()
{
    m_state = AppState::Playing;
    m_pauseMenu.reset(m_settingsPanel);
    setCursorCaptured(true);
    m_clock.setTimeScale(1.0f);
    m_clock.resetAfterStall();
    m_frameLimiter.reset();
}

void Application::enterPaused()
{
    m_state = AppState::Paused;
    m_pauseMenu.reset(m_settingsPanel);
    setCursorCaptured(false);
    // Zeroing the scale is what stops the simulation; the world keeps streaming
    // so resuming is instant.
    m_clock.setTimeScale(0.0f);
    m_miningSound.stop(m_audio);
}

void Application::enterMainMenu()
{
    m_state = AppState::MainMenu;
    m_mainMenu.reset();
    m_mainMenu.refreshWorlds();
    setCursorCaptured(false);
    m_clock.setTimeScale(0.0f);
    m_miningSound.stop(m_audio);
}

void Application::handleMainMenu(const MainMenuResult& result)
{
    switch (result.action) {
        case MainMenuAction::None:
            break;
        case MainMenuAction::CreateWorld: {
            // The menu deliberately does not decide what a legal directory name
            // is; that is this layer's problem. Anything a path separator could
            // escape through is replaced rather than rejected, so a player who
            // types "my/world" gets "my_world" instead of an error.
            std::string directoryName = result.worldName;
            for (char& character : directoryName) {
                const bool legal = (character >= 'a' && character <= 'z') ||
                                   (character >= 'A' && character <= 'Z') ||
                                   (character >= '0' && character <= '9') || character == '-' ||
                                   character == '_' || character == ' ';
                if (!legal) {
                    character = '_';
                }
            }
            if (directoryName.empty()) {
                directoryName = "world";
            }
            openWorld(std::move(directoryName), result.seed);
            break;
        }
        case MainMenuAction::LoadWorld:
            openWorld(result.world.name, result.world.seed);
            break;
        case MainMenuAction::Quit:
            m_window.requestClose();
            break;
    }
}

void Application::handlePauseMenu(const PauseMenuResult& result)
{
    switch (result.action) {
        case PauseMenuAction::None:
            break;
        case PauseMenuAction::Resume:
            enterPlaying();
            break;
        case PauseMenuAction::SaveAndQuitToMenu:
            closeWorld();
            enterMainMenu();
            break;
        case PauseMenuAction::QuitToDesktop:
            // The world is closed here rather than left to ~Application so the
            // "Saving..." state is observable and a failure can still be logged
            // against a live window.
            closeWorld();
            m_window.requestClose();
            break;
    }
}

void Application::applySettings(SettingsDirty dirty)
{
    if (dirty == SettingsDirty::None) {
        return;
    }

    if (hasAny(dirty, SettingsDirty::Video)) {
        m_window.setVSync(m_settings.vsync);
        m_frameLimiter.setTargetFps(static_cast<float>(m_settings.fpsCap));
    }
    if (hasAny(dirty, SettingsDirty::Camera)) {
        m_camera.setFovDegrees(m_settings.fovDegrees);
        m_camera.setClipPlanes(0.05f, static_cast<float>(m_settings.renderDistance + 2) *
                                          static_cast<float>(kChunkSize) * 1.5f);
    }
    if (hasAny(dirty, SettingsDirty::Streaming)) {
        StreamingConfig streaming = m_world.chunks().config();
        streaming.loadRadius      = m_settings.renderDistance;
        streaming.lod.enabled     = m_settings.lodEnabled;
        for (std::size_t i = 0; i < std::size_t{kLodMax}; ++i) {
            streaming.lod.bandStart[i] = m_settings.lodBandStart[i];
        }
        m_world.chunks().setConfig(streaming);
    }
    if (hasAny(dirty, SettingsDirty::Fog)) {
        // The two scales compose: fogDistanceScale is the player's preference and
        // fogRangeScale is the dawn/dusk haze the cycle asks for.
        m_renderer.setFogFromViewDistance(static_cast<float>(m_settings.renderDistance) *
                                          static_cast<float>(kChunkSize) *
                                          m_settings.fogDistanceScale *
                                          m_dayNight.fogRangeScale());
    }
    if (hasAny(dirty, SettingsDirty::Controls)) {
        PlayerConfig player     = m_player.config();
        player.mouseSensitivity = m_settings.mouseSensitivity;
        player.invertMouseY     = m_settings.invertMouseY;
        m_player.setConfig(player);
    }
    if (hasAny(dirty, SettingsDirty::Audio)) {
        m_audio.setMasterVolume(m_settings.masterVolume);
        m_audio.setCategoryVolume(audio::SoundCategory::World, m_settings.worldVolume);
        m_audio.setCategoryVolume(audio::SoundCategory::Ui, m_settings.uiVolume);
    }
    if (hasAny(dirty, SettingsDirty::Interface)) {
        // Must not run between NewFrame() and Render(); drawUi() calls this after
        // Render() for exactly that reason.
        SettingsPanel::applyTheme(m_settings.guiScale);
    }
    if (hasAny(dirty, SettingsDirty::Texture)) {
        m_renderer.setTextureAnisotropy(m_settings.anisotropy);
    }
    if (hasAny(dirty, SettingsDirty::World)) {
        m_dayNight.setDayLengthSeconds(static_cast<double>(m_settings.dayLengthMinutes) * 60.0);
    }
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

    if (m_state == AppState::Playing) {
        m_playTimeSeconds += m_clock.deltaSeconds();
    }

    simulate();

    // The world keeps streaming while paused, so resuming is instant; in the
    // main menu there is no world to stream.
    if (m_worldOpen) {
        stream();
        // After stream(), which is what drains the deferral queue this batch feeds.
        updateDebugScript();
        if (m_state == AppState::Playing && m_save.has_value() &&
            m_save->tickAutosave(m_clock.wallSeconds())) {
            saveEverything();
        }
    }
    m_uploadMs = drainUploads();

    if (m_window.isIconified() || m_window.width() <= 0 || m_window.height() <= 0) {
        // No framebuffer to draw into, and no swap to pace the loop, so block on
        // the event queue instead of spinning. Streaming and uploads above still
        // ran, which is what keeps a restore from stuttering.
        m_window.waitEvents(0.05);
        m_frameLimiter.reset();
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

    // LAST. The limiter's whole job is to absorb whatever slack is left after
    // the frame, including the swap.
    m_frameLimiter.wait();
}

void Application::pollInput()
{
    // The settings panel owns Escape while it is open, so the state machine must
    // not also act on it - the panel would close and the game would unpause in
    // the same keystroke.
    if (m_input.keyPressed(Key::Escape) && !m_settingsPanel.isOpen()) {
        switch (m_state) {
            case AppState::Playing:
                enterPaused();
                break;
            case AppState::Paused:
                enterPlaying();
                break;
            case AppState::MainMenu:
                // The title screen handles its own back-navigation.
                break;
        }
    }

    m_playerInput      = PlayerInput{};
    m_interactionInput = InteractionInput{};

    if (m_state != AppState::Playing) {
        return;
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

    if (!m_window.cursorCaptured()) {
        // Cursor released while nominally playing (alt-tab): the player keeps its
        // velocity but stops steering, so returning does not walk you off a cliff.
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

    // V toggles between breaking whole blocks and drilling sub-voxels.
    if (m_input.keyPressed(Key::V)) {
        const MiningMode mode = m_interaction.toggleMiningMode();
        VOXL_LOG_INFO("Mining mode: {}", miningModeLabel(mode));
        if (m_audio.available()) {
            m_audio.playUi(m_audio.findCue("ui.click"));
        }
    }

    // The wheel drives either the hotbar or the brush radius, never both in the
    // same notch. Telling the hotbar to stand down is cheaper than duplicating
    // the rule on both sides, where the two copies would eventually disagree.
    const float scroll  = m_input.scrollDelta();
    const int   notches = static_cast<int>(scroll > 0.0f ? std::ceil(scroll) : std::floor(scroll));
    const bool  brushDial =
        m_interaction.miningMode() == MiningMode::SubVoxel && m_input.keyDown(Key::LeftAlt);
    m_hotbar.setCycleLocked(brushDial);
    if (notches != 0) {
        if (brushDial) {
            m_interaction.adjustBrushRadius(notches);
        } else {
            m_hotbar.cycle(notches);
        }
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
    if (m_state != AppState::Playing || !m_worldOpen) {
        // Still keep the camera consistent with the player so a paused frame
        // draws the same view as the frame before it.
        m_camera.setAspectRatio(m_window.aspectRatio());
        return;
    }

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

    // Before the interaction update, so a sound triggered this frame is mixed
    // against the listener pose the player actually has right now.
    m_audio.setListenerFromCamera(m_camera);

    m_interaction.setHeldBlock(m_hotbar.selectedBlock());
    m_interaction.setPlayerAabb(toRenderAabb(m_player.bounds()));
    m_interaction.update(m_camera, m_interactionInput, m_clock.deltaSeconds());

    updateAudio();
}

void Application::updateAudio()
{
    if (!m_audio.available()) {
        return;
    }

    const InteractionState& state  = m_interaction.state();
    const glm::vec3         target = blockCentre(state.hit.block);

    if (state.lastBreak == BreakResult::Broken) {
        // hit.blockId is still the block that was destroyed: the raycast runs
        // before the write inside BlockInteraction::update.
        m_audio.playBlockBreak(m_registry.get(state.hit.blockId).soundGroup, target);
    }
    if (state.lastPlace == PlaceResult::Placed) {
        m_audio.playBlockPlace(m_registry.get(m_hotbar.selectedBlock()).soundGroup,
                               blockCentre(state.placeTarget));
    }

    // Continuous mining bed. A carved frame keeps it running: the block survives
    // a carve, so BreakResult::Carved is progress, not completion.
    if (state.lastBreak == BreakResult::InProgress || state.lastBreak == BreakResult::Carved) {
        audio::PlayParams mining;
        mining.position       = target;
        mining.spatialisation = audio::Spatialisation::Positional;
        m_miningSound.update(m_audio,
                             m_audio.blockCue(m_registry.get(state.hit.blockId).soundGroup,
                                              audio::BlockSoundEvent::Mine),
                             mining);
    } else {
        m_miningSound.stop(m_audio);
    }

    // Footsteps are distance-triggered rather than timed, so sprinting steps
    // faster without a second rule saying so.
    if (m_simulationStarted && m_player.onGround() && !m_player.flying()) {
        const glm::vec3 feet  = m_player.position();
        const glm::vec3 delta = feet - m_lastFootPosition;
        m_footstepDistance += std::sqrt(delta.x * delta.x + delta.z * delta.z);
        const float stride = m_player.crouching() ? 1.4f : (m_player.sprinting() ? 2.4f : 2.0f);
        if (m_footstepDistance >= stride) {
            m_footstepDistance = 0.0f;
            const BlockPos below{physics::floorToInt(feet.x),
                                 physics::floorToInt(feet.y - 0.1f),
                                 physics::floorToInt(feet.z)};
            const BlockId ground = m_world.getBlock(below);
            if (ground != blocks::Air) {
                m_audio.playFootstep(m_registry.get(ground).soundGroup, feet);
            }
        }
    } else {
        m_footstepDistance = 0.0f;  // land on the next full stride, not instantly
    }
    m_lastFootPosition = m_player.position();
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

    // Advanced with the FRAME delta, not the fixed step: the sky is presentation
    // rather than simulation, and stepping it inside the fixed-step loop makes it
    // stutter whenever the accumulator runs dry. It also keeps moving while the
    // game is paused only if the clock says so - deltaSeconds() is already scaled.
    m_dayNight.advance(m_clock.deltaSeconds());
    m_renderer.setSky(m_dayNight.sky());

    m_renderer.beginFrame(m_camera, m_clock.totalSeconds());
    m_renderer.drawSky();
    m_renderer.drawWorld();
    if (m_state == AppState::Playing) {
        m_interaction.render(m_camera.viewProjection());
    }
    m_renderer.endFrame();

    m_overlay.endGpuFrame();

    m_world.setVisibleChunkCount(m_renderer.stats().chunks.chunksVisible);
}

void Application::drawUi()
{
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    SettingsDirty dirty        = SettingsDirty::None;
    bool          saveSettings = false;

    switch (m_state) {
        case AppState::Playing:
            m_hud.draw(m_hotbar, m_interaction.state(), m_clock.deltaSeconds());
            m_overlay.draw(buildOverlayFrame(m_lastCpuMs, m_uploadMs));
            break;
        case AppState::Paused: {
            m_hud.draw(m_hotbar, m_interaction.state(), m_clock.deltaSeconds());
            m_overlay.draw(buildOverlayFrame(m_lastCpuMs, m_uploadMs));
            const PauseMenuResult result = m_pauseMenu.draw(m_settingsPanel, m_settings);
            dirty                        = result.dirty;
            saveSettings                 = result.saveSettingsRequested;
            handlePauseMenu(result);
            break;
        }
        case AppState::MainMenu: {
            const MainMenuResult result = m_mainMenu.draw(m_settingsPanel, m_settings);
            dirty                       = result.dirty;
            saveSettings                = result.saveSettingsRequested;
            handleMainMenu(result);
            break;
        }
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // AFTER Render(): SettingsDirty::Interface restyles the context and rebuilds
    // the font atlas, which is undefined behaviour between NewFrame and Render.
    applySettings(dirty);
    if (saveSettings) {
        // The unknown lines go back out verbatim, which is what stops this build
        // from deleting a key a newer build wrote.
        saveSettingsFile(m_settingsPath, m_settings, m_unknownSettingLines);
    }
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
    frame.world.seed               = m_terrain.has_value() ? m_terrain->seed() : 0;

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
    // Real seconds, not scaled: pausing must not stop the status log, because a
    // hang while paused is exactly when the log matters most.
    const double now = m_clock.wallSeconds();
    if (now < m_nextStatusLog) {
        return;
    }
    m_nextStatusLog = now + m_config.statusLogSeconds;

    const RenderStats& render = m_renderer.stats();
    const glm::vec3& feet = m_player.position();
    VOXL_LOG_INFO(
        "t={:.1f}s fps={:.1f} {} pos=({:.1f},{:.1f},{:.1f}){} chunks(loaded={} ready={} gen={} "
        "mesh={}) draws={} tris={} gpu={} KiB jobs(queued={} active={})",
        now, static_cast<double>(m_clock.fps()), toString(m_state), feet.x, feet.y, feet.z,
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

    // Lighting on its own line for the same reason: "is the light pass actually
    // running" cannot be answered from a screenshot either, because an unlit
    // world and a world at uniform full sunlight look identical from outdoors.
    //
    // The two samples are the proof, and they are why this line is worth its
    // cost. `sky` is taken at the player's eye and `deep` well below the surface;
    // a working light pass reads roughly 15 and 0. Both at 15 means nothing
    // propagated and every voxel is still at the storage default - which is
    // precisely the failure that looks completely normal from outdoors.
    const BlockPos eye  = toBlockPos(m_camera.position());
    const BlockPos deep{eye.x, kWorldMinY + 24, eye.z};
    const std::uint8_t eyeLight  = m_world.getLight(eye);
    const std::uint8_t deepLight = m_world.getLight(deep);
    VOXL_LOG_INFO(
        "  light{} columns={} sections={} inflight={} seeds={} sun/blk(sky)={}/{} "
        "sun/blk(deep)={}/{} intensity={:.3f} time={}",
        m_world.lightingEnabled() ? "" : " (DISABLED)", m_stats.lightColumnsLit,
        m_stats.lightSectionsLit, m_stats.lightJobsInFlight, m_stats.pendingLightSeeds,
        ChunkStorage::unpackSunlight(eyeLight), ChunkStorage::unpackBlockLight(eyeLight),
        ChunkStorage::unpackSunlight(deepLight), ChunkStorage::unpackBlockLight(deepLight),
        static_cast<double>(m_dayNight.sky().sunIntensity), m_dayNight.clockText());

    if (m_stats.damagedBlocks != 0 || render.chunks.subVoxelDrawCalls != 0) {
        VOXL_LOG_INFO("  subvoxel blocks={} chunks={} draws={} tris={} gpu={} B cpu={} B",
                      m_stats.damagedBlocks, render.chunks.subVoxelChunksResident,
                      render.chunks.subVoxelDrawCalls, render.chunks.subVoxelTriangles,
                      render.chunks.subVoxelGpuBytes, m_stats.subVoxelBytes);
    }

    if (m_save.has_value()) {
        const SaveStats save = m_save->stats();
        if (save.chunksWritten != 0 || save.chunksLoaded != 0 || save.writeFailures != 0) {
            VOXL_LOG_INFO("  save written={} ({} KiB) loaded={} absent={} corrupt={} failed={} "
                          "inflight={} regions={}",
                          save.chunksWritten, save.bytesWritten / 1024, save.chunksLoaded,
                          save.chunksAbsent, save.chunksCorrupt, save.writeFailures,
                          save.writesInFlight, save.openRegions);
        }
    }
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
//          open air is the clearest place to compare a carved surface against an
//          intact one under identical light.

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
    if (m_config.debug.carve == DebugStartup::Carve::None || m_debugRigDone ||
        !m_terrain.has_value()) {
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
                           m_terrain->surfaceHeight(physics::floorToInt(m_player.position().x),
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
