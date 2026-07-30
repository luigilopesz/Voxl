#pragma once

// The F3 debug overlay.
//
// The overlay owns no engine state. Every number it shows arrives in a
// `DebugOverlayFrame` that the frame loop fills in, and every counter is an
// `std::optional`: a subsystem that does not expose a value yet leaves it empty
// and the overlay prints a dash. That is deliberate - it keeps this file from
// inventing APIs for modules that are still being written, and it means the
// overlay is useful from the very first frame of the engine's life.
//
// The one thing the overlay does own is the GPU timer. GL timer queries have to
// bracket the frame's draw calls, which only the overlay's owner can arrange, so
// `beginGpuFrame()` / `endGpuFrame()` are exposed and degrade to "unavailable"
// when the driver has no GL_TIME_ELAPSED support.
//
// Thread safety: none. Main thread only (ImGui and GL).

#include "core/JobSystem.hpp"
#include "core/Time.hpp"
#include "world/Block.hpp"
#include "world/Lod.hpp"
#include "world/VoxelTypes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace voxl {

class Camera;

/// Chunk-pipeline and world-memory counters.
///
/// Field names match the histogram the design doc asks for (a count per
/// `ChunkState`) plus the streaming queue. `visible` is the post-frustum-cull
/// count the renderer produces, not a world figure, but it belongs next to the
/// other chunk numbers on screen.
struct WorldDebugCounters {
    std::optional<std::size_t> loaded;      ///< resident chunks
    std::optional<std::size_t> generating;  ///< ChunkState::Generating
    std::optional<std::size_t> generated;   ///< ChunkState::Generated
    std::optional<std::size_t> meshing;     ///< ChunkState::Meshing
    std::optional<std::size_t> meshed;      ///< ChunkState::Meshed
    std::optional<std::size_t> ready;       ///< ChunkState::Ready
    std::optional<std::size_t> queued;      ///< awaiting a generate/mesh job
    std::optional<std::size_t> visible;     ///< passed frustum culling this frame

    /// Sum of `Chunk::memoryUsageBytes()` over the resident set.
    std::optional<std::size_t> voxelBytes;
    std::optional<std::size_t> lightBytes;

    std::optional<int>           viewDistanceChunks;
    std::optional<std::uint64_t> seed;
};

/// Per-frame renderer counters.
struct RenderDebugCounters {
    std::optional<std::size_t> drawCalls;
    std::optional<std::size_t> triangles;
    std::optional<std::size_t> vertices;

    /// Sum of `ChunkMeshData::byteSize()` over uploaded meshes.
    std::optional<std::size_t> meshBytes;
    std::optional<std::size_t> textureBytes;
    std::optional<std::size_t> uploadedMeshes;

    /// Driver-reported free video memory, if the extension exists.
    std::optional<std::size_t> gpuMemoryAvailableBytes;
};

/// Level-of-detail breakdown. Every array is indexed by `LodLevel`, so entry 0
/// is full resolution; see world/Lod.hpp.
///
/// The whole point of showing four numbers instead of one total is to answer
/// "where is the budget going". If level 3 - the outermost ring, and by far the
/// most chunks - still accounts for a third of the triangles, the LOD policy is
/// not paying for itself, and a single aggregate figure cannot tell you that.
struct LodDebugCounters {
    std::array<std::optional<std::size_t>, kLodCount> residentChunks{};
    std::array<std::optional<std::size_t>, kLodCount> visibleChunks{};
    std::array<std::optional<std::size_t>, kLodCount> drawCalls{};
    std::array<std::optional<std::size_t>, kLodCount> triangles{};

    /// From `LodPolicy::enabled`. Shown so a benchmark run pinned to level 0 is
    /// never mistaken for a broken policy.
    std::optional<bool> enabled;

    [[nodiscard]] bool any() const noexcept
    {
        for (std::size_t i = 0; i < kLodCount; ++i) {
            if (residentChunks[i].has_value() || visibleChunks[i].has_value() ||
                drawCalls[i].has_value() || triangles[i].has_value()) {
                return true;
            }
        }
        return false;
    }
};

/// Destructible-block counters.
///
/// `damagedBlocks` is a WORLD figure (the sum of `SubVoxelStore::size()` over
/// resident chunks) while the rest are renderer figures. They are grouped here
/// anyway because the question being answered - "is the damage system costing
/// anything yet" - needs both halves side by side.
struct SubVoxelDebugCounters {
    std::optional<std::size_t> damagedBlocks;   ///< partially destroyed blocks, CPU side
    std::optional<std::size_t> damagedChunks;   ///< chunks holding damage geometry
    std::optional<std::size_t> drawCalls;       ///< a subset of RenderDebugCounters::drawCalls
    std::optional<std::size_t> triangles;       ///< likewise a subset
    std::optional<std::size_t> gpuBytes;        ///< included in RenderDebugCounters::meshBytes
    std::optional<std::size_t> cpuBytes;        ///< SubVoxelStore::memoryUsageBytes() summed

    /// Current mining mode, as a free-form label (e.g. "block" / "sub-voxel").
    ///
    /// A string rather than an enum on purpose. The gameplay layer owns whatever
    /// type this really is and is still being written; the overlay's whole design
    /// is to avoid inventing APIs for modules in flight (see the file header).
    /// Empty means "not reported".
    std::string mode;
};

/// What the crosshair is currently on. Mirrors the useful half of
/// `InteractionState` without depending on it, so the overlay stays usable in a
/// build with no interaction module.
struct TargetDebugInfo {
    bool      hasTarget = false;
    BlockPos  block{};
    BlockId   blockId  = blocks::Air;
    Direction face     = Direction::PosY;
    float     distance = 0.0f;

    std::optional<std::uint8_t> sunlight;
    std::optional<std::uint8_t> blockLight;
};

/// Everything the overlay reads for one frame. Pointers are optional; a null
/// pointer means "that subsystem is not wired up" and its section is omitted.
struct DebugOverlayFrame {
    const FrameClock*     clock  = nullptr;
    const Camera*         camera = nullptr;
    const JobSystemStats* jobs   = nullptr;

    WorldDebugCounters    world{};
    RenderDebugCounters   render{};
    LodDebugCounters      lod{};
    SubVoxelDebugCounters subVoxel{};
    TargetDebugInfo       target{};

    /// CPU time spent in the frame's own work, excluding the swap/vsync wait.
    /// Distinct from `FrameClock::lastFrameMs()`, which includes it.
    std::optional<float> cpuFrameMs;

    /// Time the main thread spent draining GPU uploads.
    std::optional<float> uploadMs;

    /// Free-form strings, e.g. `Window::glRendererString()`.
    std::string gpuName;
    std::string glVersion;
};

/// Immediate-mode debug panel. One instance lives in the application.
class DebugOverlay {
public:
    /// Frames kept in the rolling graph. 240 is four seconds at 60 Hz - long
    /// enough to see a streaming hitch scroll past, short enough that the graph
    /// stays legible at a sane width.
    static constexpr std::size_t kHistoryLength = 240;

    /// `registry` is used only to name the block under the crosshair. Passing
    /// nullptr degrades that field to the raw numeric id.
    explicit DebugOverlay(const BlockRegistry* registry = nullptr) noexcept;
    ~DebugOverlay();

    // Owns GL query objects.
    DebugOverlay(const DebugOverlay&)            = delete;
    DebugOverlay& operator=(const DebugOverlay&) = delete;
    DebugOverlay(DebugOverlay&&)                 = delete;
    DebugOverlay& operator=(DebugOverlay&&)      = delete;

    // ---- visibility ----

    [[nodiscard]] bool visible() const noexcept { return m_visible; }
    void setVisible(bool visible) noexcept { m_visible = visible; }
    void toggle() noexcept { m_visible = !m_visible; }

    /// When true (the default) the overlay watches F3 itself through ImGui, so
    /// it works before an input module exists. Turn it off once the game's own
    /// key binding calls `toggle()`, otherwise F3 fires twice.
    void setSelfToggleKey(bool enabled) noexcept { m_selfToggle = enabled; }

    // ---- GPU timing ----

    /// Brackets the frame's GL work. Call `beginGpuFrame()` before the first
    /// draw and `endGpuFrame()` after the last one, both before `draw()`.
    /// Results lag by two frames because reading a query in the frame that
    /// issued it stalls the pipeline, which would corrupt the very number being
    /// measured. Safe to call when unsupported: both become no-ops.
    void beginGpuFrame();
    void endGpuFrame();

    [[nodiscard]] bool  gpuTimingAvailable() const noexcept { return m_gpuTimingAvailable; }
    [[nodiscard]] float gpuFrameMs() const noexcept { return m_gpuTime.lastMs(); }
    [[nodiscard]] const TimingSample& gpuFrameTime() const noexcept { return m_gpuTime; }

    /// Frees the query objects. Called by the destructor; call it explicitly if
    /// the GL context dies first.
    void releaseGpuResources() noexcept;

    // ---- drawing ----

    /// Submits the ImGui windows for this frame. Must be called between
    /// `ImGui::NewFrame()` and `ImGui::Render()`. Does nothing when hidden, and
    /// nothing at all when no ImGui context is current.
    void draw(const DebugOverlayFrame& frame);

    /// Also samples the frame-time history, so it keeps the graph continuous
    /// while the overlay is hidden. Call it every frame even if `draw()` is not
    /// reached; `draw()` calls it for you.
    void sample(const DebugOverlayFrame& frame);

private:
    void drawPerformanceSection(const DebugOverlayFrame& frame);
    void drawWorldSection(const DebugOverlayFrame& frame);
    void drawRenderSection(const DebugOverlayFrame& frame);
    void drawLodSection(const DebugOverlayFrame& frame);
    void drawSubVoxelSection(const DebugOverlayFrame& frame);
    void drawJobSection(const DebugOverlayFrame& frame);
    void drawTargetSection(const DebugOverlayFrame& frame);

    /// Reads whichever query is old enough to be complete. No-op when GPU
    /// timing is unavailable.
    void collectGpuResults();

    const BlockRegistry* m_registry = nullptr;

    bool m_visible    = false;
    bool m_selfToggle = true;

    /// Ring buffer of raw frame times; `m_historyCursor` is the write position.
    std::array<float, kHistoryLength> m_frameHistory{};
    std::size_t                       m_historyCursor = 0;
    std::size_t                       m_historyFilled = 0;
    std::uint64_t                     m_lastSampledFrame = 0;
    bool                              m_hasSampledFrame  = false;

    /// Three in flight: one being written, one settling, one ready to read.
    static constexpr std::size_t kGpuQueryCount = 3;
    std::array<std::uint32_t, kGpuQueryCount> m_gpuQueries{};
    std::array<bool, kGpuQueryCount>          m_gpuQueryPending{};
    std::size_t  m_gpuQueryCursor     = 0;
    bool         m_gpuQueriesCreated  = false;
    bool         m_gpuTimingAvailable = false;
    bool         m_gpuFrameOpen       = false;
    TimingSample m_gpuTime{0.1f};
};

}  // namespace voxl
