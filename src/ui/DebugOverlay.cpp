#include "ui/DebugOverlay.hpp"

#include <glad/gl.h>

#include "core/Log.hpp"
#include "render/Camera.hpp"

#include <algorithm>
#include <cstdio>
#include <iterator>

#include <imgui.h>

static_assert(sizeof(GLuint) == sizeof(std::uint32_t),
              "DebugOverlay stores query names as uint32_t to keep <glad/gl.h> out of its header");

namespace voxl {
namespace {

constexpr ImVec4 kLabelColour{0.70f, 0.74f, 0.80f, 1.00f};
constexpr ImVec4 kMissingColour{0.55f, 0.55f, 0.58f, 1.00f};
constexpr ImVec4 kWarnColour{1.00f, 0.72f, 0.25f, 1.00f};
constexpr ImVec4 kGoodColour{0.55f, 0.90f, 0.55f, 1.00f};

/// Placeholder for a counter no subsystem has reported yet. Showing this rather
/// than a zero is the whole point: "0 draw calls" and "nobody is counting draw
/// calls" are very different bugs.
constexpr const char* kUnavailable = "--";

void labelled(const char* label, const char* value)
{
    ImGui::TextColored(kLabelColour, "%s", label);
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::TextUnformatted(value);
}

void labelledMissing(const char* label)
{
    ImGui::TextColored(kLabelColour, "%s", label);
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::TextColored(kMissingColour, "%s", kUnavailable);
}

/// Byte counts in the overlay span single kilobytes (one chunk's palette) to
/// gigabytes (a large resident set), so a fixed unit is unreadable either way.
[[nodiscard]] std::string formatBytes(std::size_t bytes)
{
    constexpr double kUnit = 1024.0;
    const char*      suffixes[]{"B", "KiB", "MiB", "GiB", "TiB"};

    double      value = static_cast<double>(bytes);
    std::size_t index = 0;
    while (value >= kUnit && index + 1 < std::size(suffixes)) {
        value /= kUnit;
        ++index;
    }

    std::array<char, 32> buffer{};
    if (index == 0) {
        std::snprintf(buffer.data(), buffer.size(), "%zu B", bytes);
    } else {
        std::snprintf(buffer.data(), buffer.size(), "%.2f %s", value, suffixes[index]);
    }
    return std::string{buffer.data()};
}

template <typename T>
void labelledCount(const char* label, const std::optional<T>& value)
{
    if (!value.has_value()) {
        labelledMissing(label);
        return;
    }
    // Signed formatting on purpose: `viewDistanceChunks` is an int, and casting
    // a stray negative to unsigned would print a 20-digit number instead of the
    // obviously-wrong small negative one.
    std::array<char, 32> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%lld", static_cast<long long>(*value));
    labelled(label, buffer.data());
}

void labelledBytes(const char* label, const std::optional<std::size_t>& value)
{
    if (!value.has_value()) {
        labelledMissing(label);
        return;
    }
    labelled(label, formatBytes(*value).c_str());
}

void labelledMs(const char* label, const std::optional<float>& value)
{
    if (!value.has_value()) {
        labelledMissing(label);
        return;
    }
    std::array<char, 32> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%.2f ms", static_cast<double>(*value));
    labelled(label, buffer.data());
}

/// Sums the optionals that are present. Returns empty only when *none* of them
/// are, so a partially wired renderer still shows a meaningful total.
template <typename... Ts>
[[nodiscard]] std::optional<std::size_t> sumAvailable(const Ts&... values)
{
    std::optional<std::size_t> total;
    const auto                 accumulate = [&total](const std::optional<std::size_t>& value) {
        if (value.has_value()) {
            total = total.value_or(0u) + *value;
        }
    };
    (accumulate(values), ...);
    return total;
}

[[nodiscard]] bool gpuQueryApiAvailable() noexcept
{
    // glad populates the whole core table in one pass; probing the four entry
    // points this path needs is enough to know the loader has run.
    return glGenQueries != nullptr && glBeginQuery != nullptr && glEndQuery != nullptr &&
           glGetQueryObjectui64v != nullptr && glGetQueryObjectiv != nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------

DebugOverlay::DebugOverlay(const BlockRegistry* registry) noexcept : m_registry(registry) {}

DebugOverlay::~DebugOverlay() { releaseGpuResources(); }

// ------------------------------------------------------------ GPU timing --

void DebugOverlay::beginGpuFrame()
{
    if (!m_gpuQueriesCreated) {
        m_gpuQueriesCreated = true;  // one attempt, whatever the outcome
        if (!gpuQueryApiAvailable()) {
            VOXL_LOG_WARN("GPU frame timing disabled: timer queries unavailable");
            return;
        }
        glGenQueries(static_cast<GLsizei>(m_gpuQueries.size()),
                     reinterpret_cast<GLuint*>(m_gpuQueries.data()));
        m_gpuQueryPending.fill(false);
        m_gpuTimingAvailable = m_gpuQueries[0] != 0;
        if (!m_gpuTimingAvailable) {
            VOXL_LOG_WARN("GPU frame timing disabled: driver returned no query objects");
        }
    }
    if (!m_gpuTimingAvailable || m_gpuFrameOpen) {
        return;
    }

    // Only one GL_TIME_ELAPSED query may be active at a time, so a slot that is
    // still awaiting its result must be read (or abandoned) before reuse.
    collectGpuResults();

    if (m_gpuQueryPending[m_gpuQueryCursor]) {
        // The pipeline is more than kGpuQueryCount frames deep; skip this frame
        // rather than block on glGetQueryObjectui64v.
        return;
    }
    glBeginQuery(GL_TIME_ELAPSED, m_gpuQueries[m_gpuQueryCursor]);
    m_gpuFrameOpen = true;
}

void DebugOverlay::endGpuFrame()
{
    if (!m_gpuTimingAvailable || !m_gpuFrameOpen) {
        return;
    }
    glEndQuery(GL_TIME_ELAPSED);
    m_gpuQueryPending[m_gpuQueryCursor] = true;
    m_gpuQueryCursor                    = (m_gpuQueryCursor + 1) % kGpuQueryCount;
    m_gpuFrameOpen                      = false;
}

void DebugOverlay::collectGpuResults()
{
    if (!m_gpuTimingAvailable) {
        return;
    }
    for (std::size_t offset = 0; offset < kGpuQueryCount; ++offset) {
        // Walk oldest-first from the slot after the write cursor.
        const std::size_t slot = (m_gpuQueryCursor + 1 + offset) % kGpuQueryCount;
        if (!m_gpuQueryPending[slot]) {
            continue;
        }
        GLint ready = GL_FALSE;
        glGetQueryObjectiv(m_gpuQueries[slot], GL_QUERY_RESULT_AVAILABLE, &ready);
        if (ready != GL_TRUE) {
            continue;
        }
        GLuint64 nanoseconds = 0;
        glGetQueryObjectui64v(m_gpuQueries[slot], GL_QUERY_RESULT, &nanoseconds);
        m_gpuTime.add(static_cast<float>(static_cast<double>(nanoseconds) / 1.0e6));
        m_gpuQueryPending[slot] = false;
    }
}

void DebugOverlay::releaseGpuResources() noexcept
{
    if (m_gpuTimingAvailable && glDeleteQueries != nullptr) {
        glDeleteQueries(static_cast<GLsizei>(m_gpuQueries.size()),
                        reinterpret_cast<GLuint*>(m_gpuQueries.data()));
    }
    m_gpuQueries.fill(0);
    m_gpuQueryPending.fill(false);
    m_gpuTimingAvailable = false;
    m_gpuQueriesCreated  = false;
    m_gpuFrameOpen       = false;
}

// ---------------------------------------------------------------- sampling --

void DebugOverlay::sample(const DebugOverlayFrame& frame)
{
    if (frame.clock == nullptr) {
        return;
    }
    // Guard against a double sample when the owner calls sample() and then
    // draw(): both feed the same ring buffer and a duplicated spike would make
    // the graph lie.
    const std::uint64_t index = frame.clock->frameIndex();
    if (m_hasSampledFrame && index == m_lastSampledFrame) {
        return;
    }
    m_lastSampledFrame = index;
    m_hasSampledFrame  = true;

    m_frameHistory[m_historyCursor] = frame.clock->lastFrameMs();
    m_historyCursor                 = (m_historyCursor + 1) % kHistoryLength;
    m_historyFilled                 = std::min(m_historyFilled + 1, kHistoryLength);
}

// ----------------------------------------------------------------- drawing --

void DebugOverlay::draw(const DebugOverlayFrame& frame)
{
    // Called before ImGui is initialised (or in a headless test) this must be
    // inert rather than a crash inside the ImGui context accessor.
    if (ImGui::GetCurrentContext() == nullptr) {
        return;
    }

    sample(frame);

    if (m_selfToggle && ImGui::IsKeyPressed(ImGuiKey_F3, false)) {
        toggle();
    }
    if (!m_visible) {
        return;
    }

    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoFocusOnAppearing |
                                        ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_AlwaysAutoResize;

    ImGui::SetNextWindowPos(ImVec2{10.0f, 10.0f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.72f);
    if (!ImGui::Begin("Debug (F3)", nullptr, kFlags)) {
        ImGui::End();
        return;
    }

    drawPerformanceSection(frame);
    drawWorldSection(frame);
    drawRenderSection(frame);
    drawJobSection(frame);
    drawTargetSection(frame);

    ImGui::End();
}

void DebugOverlay::drawPerformanceSection(const DebugOverlayFrame& frame)
{
    ImGui::SeparatorText("Performance");

    if (frame.clock == nullptr) {
        ImGui::TextColored(kMissingColour, "no FrameClock supplied");
    } else {
        const FrameClock& clock = *frame.clock;
        const float       fps   = clock.fps();

        std::array<char, 64> buffer{};
        std::snprintf(buffer.data(), buffer.size(), "%.1f", static_cast<double>(fps));
        ImGui::TextColored(kLabelColour, "FPS");
        ImGui::SameLine(0.0f, 6.0f);
        // 60 is the vsync target the engine is tuned for; below it the frame is
        // missing its deadline and the number should say so at a glance.
        ImGui::TextColored(fps >= 59.0f ? kGoodColour : kWarnColour, "%s", buffer.data());

        const TimingSample& frameTime = clock.frameTime();
        std::snprintf(buffer.data(), buffer.size(), "%.2f ms  (avg %.2f  min %.2f  max %.2f)",
                      static_cast<double>(frameTime.lastMs()),
                      static_cast<double>(frameTime.averageMs()),
                      static_cast<double>(frameTime.minMs()),
                      static_cast<double>(frameTime.maxMs()));
        labelled("Frame", buffer.data());

        if (clock.timeScale() != 1.0f) {
            std::snprintf(buffer.data(), buffer.size(), "%.2fx", static_cast<double>(clock.timeScale()));
            labelled("Time scale", buffer.data());
        }
    }

    labelledMs("CPU frame", frame.cpuFrameMs);
    labelledMs("GPU upload", frame.uploadMs);

    if (m_gpuTimingAvailable) {
        std::array<char, 48> buffer{};
        std::snprintf(buffer.data(), buffer.size(), "%.2f ms  (avg %.2f)",
                      static_cast<double>(m_gpuTime.lastMs()),
                      static_cast<double>(m_gpuTime.averageMs()));
        labelled("GPU frame", buffer.data());
    } else {
        labelledMissing("GPU frame");
    }

    // The ring buffer is plotted through PlotLines' offset parameter, which
    // avoids copying it into chronological order every frame.
    if (m_historyFilled > 1) {
        const int   count  = static_cast<int>(m_historyFilled);
        const int   offset = m_historyFilled == kHistoryLength
                                 ? static_cast<int>(m_historyCursor)
                                 : 0;
        const float peak =
            *std::max_element(m_frameHistory.begin(),
                              m_frameHistory.begin() + static_cast<std::ptrdiff_t>(m_historyFilled));
        // A fixed 0..33 ms scale keeps the 16.7 ms line at a stable height so
        // spikes are comparable between frames; grow it only when clipped.
        const float scaleMax = std::max(33.3f, peak);
        ImGui::PlotLines("##frameGraph", m_frameHistory.data(), count, offset, "frame ms", 0.0f,
                         scaleMax, ImVec2{280.0f, 56.0f});
    }

    if (!frame.gpuName.empty()) {
        labelled("GPU", frame.gpuName.c_str());
    }
    if (!frame.glVersion.empty()) {
        labelled("GL", frame.glVersion.c_str());
    }
}

void DebugOverlay::drawWorldSection(const DebugOverlayFrame& frame)
{
    ImGui::SeparatorText("World");

    if (frame.camera != nullptr) {
        const Camera&   camera   = *frame.camera;
        const glm::vec3 position = camera.position();
        const BlockPos  block    = camera.blockPosition();
        const ChunkPos  chunk    = camera.chunkPosition();
        const BlockPos  local    = toLocalPos(block);

        std::array<char, 96> buffer{};
        std::snprintf(buffer.data(), buffer.size(), "%.2f  %.2f  %.2f",
                      static_cast<double>(position.x), static_cast<double>(position.y),
                      static_cast<double>(position.z));
        labelled("Position", buffer.data());

        std::snprintf(buffer.data(), buffer.size(), "%d  %d  %d", block.x, block.y, block.z);
        labelled("Block", buffer.data());

        std::snprintf(buffer.data(), buffer.size(), "%d  %d  %d   (local %d %d %d)", chunk.x, chunk.y,
                      chunk.z, local.x, local.y, local.z);
        labelled("Chunk", buffer.data());

        std::snprintf(buffer.data(), buffer.size(), "yaw %.1f  pitch %.1f",
                      static_cast<double>(camera.yawDegrees()),
                      static_cast<double>(camera.pitchDegrees()));
        labelled("Facing", buffer.data());
    } else {
        labelledMissing("Position");
    }

    labelledCount("Loaded", frame.world.loaded);
    labelledCount("Generating", frame.world.generating);
    labelledCount("Meshing", frame.world.meshing);
    labelledCount("Queued", frame.world.queued);
    labelledCount("Visible", frame.world.visible);

    if (frame.world.generated.has_value() || frame.world.meshed.has_value() ||
        frame.world.ready.has_value()) {
        labelledCount("Generated", frame.world.generated);
        labelledCount("Meshed", frame.world.meshed);
        labelledCount("Ready", frame.world.ready);
    }

    labelledCount("View distance", frame.world.viewDistanceChunks);
    if (frame.world.seed.has_value()) {
        std::array<char, 32> buffer{};
        std::snprintf(buffer.data(), buffer.size(), "%llu",
                      static_cast<unsigned long long>(*frame.world.seed));
        labelled("Seed", buffer.data());
    }

    labelledBytes("CPU voxels", sumAvailable(frame.world.voxelBytes, frame.world.lightBytes));
}

void DebugOverlay::drawRenderSection(const DebugOverlayFrame& frame)
{
    ImGui::SeparatorText("Renderer");

    labelledCount("Draw calls", frame.render.drawCalls);
    labelledCount("Triangles", frame.render.triangles);
    labelledCount("Vertices", frame.render.vertices);
    labelledCount("Uploaded meshes", frame.render.uploadedMeshes);
    labelledBytes("GPU meshes", frame.render.meshBytes);
    labelledBytes("GPU textures", frame.render.textureBytes);
    labelledBytes("GPU total", sumAvailable(frame.render.meshBytes, frame.render.textureBytes));
    labelledBytes("GPU free", frame.render.gpuMemoryAvailableBytes);
}

void DebugOverlay::drawJobSection(const DebugOverlayFrame& frame)
{
    ImGui::SeparatorText("Jobs");

    if (frame.jobs == nullptr) {
        ImGui::TextColored(kMissingColour, "no JobSystemStats supplied");
        return;
    }

    const JobSystemStats& stats = *frame.jobs;
    std::array<char, 96>  buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%u", stats.workerCount);
    labelled("Workers", buffer.data());

    std::snprintf(buffer.data(), buffer.size(), "queued %zu   active %zu   outstanding %zu",
                  stats.queued, stats.active, stats.outstanding);
    labelled("Queue", buffer.data());

    std::snprintf(buffer.data(), buffer.size(), "%zu", stats.mainThreadPending);
    labelled("Main-thread queue", buffer.data());
}

void DebugOverlay::drawTargetSection(const DebugOverlayFrame& frame)
{
    ImGui::SeparatorText("Looking at");

    if (!frame.target.hasTarget) {
        ImGui::TextColored(kMissingColour, "nothing in reach");
        return;
    }

    const TargetDebugInfo& target = frame.target;
    std::array<char, 128>  buffer{};

    const char* name = "?";
    if (m_registry != nullptr) {
        name = m_registry->get(target.blockId).name.c_str();
    }
    std::snprintf(buffer.data(), buffer.size(), "%s  (id %u)", name,
                  static_cast<unsigned>(target.blockId));
    labelled("Block", buffer.data());

    std::snprintf(buffer.data(), buffer.size(), "%d  %d  %d", target.block.x, target.block.y,
                  target.block.z);
    labelled("At", buffer.data());

    static constexpr const char* kFaceNames[kDirectionCount]{"-X", "+X", "-Y", "+Y", "-Z", "+Z"};
    const std::size_t            faceIndex = static_cast<std::size_t>(target.face);
    std::snprintf(buffer.data(), buffer.size(), "%s   %.2f blocks away",
                  faceIndex < kDirectionCount ? kFaceNames[faceIndex] : "?",
                  static_cast<double>(target.distance));
    labelled("Face", buffer.data());

    if (target.sunlight.has_value() || target.blockLight.has_value()) {
        std::array<char, 8> sun{};
        std::array<char, 8> block{};
        if (target.sunlight.has_value()) {
            std::snprintf(sun.data(), sun.size(), "%u", static_cast<unsigned>(*target.sunlight));
        }
        if (target.blockLight.has_value()) {
            std::snprintf(block.data(), block.size(), "%u",
                          static_cast<unsigned>(*target.blockLight));
        }
        std::snprintf(buffer.data(), buffer.size(), "sun %s   block %s",
                      target.sunlight.has_value() ? sun.data() : kUnavailable,
                      target.blockLight.has_value() ? block.data() : kUnavailable);
        labelled("Light", buffer.data());
    } else {
        labelledMissing("Light");
    }
}

}  // namespace voxl
