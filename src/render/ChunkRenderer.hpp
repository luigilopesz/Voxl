#pragma once

// GPU residency, culling and draw submission for chunk meshes.
//
// THREADING CONTRACT: every method here touches GL and must run on the main
// thread. Worker threads build ChunkMeshData (a plain CPU structure) and post the
// upload to JobSystem::mainThreadQueue(); that queue's drain is the only place
// `upload()` is legal to call from.
//
// One VAO is shared by every chunk. The packed-vertex *format* never changes, so
// per-draw work is two DSA buffer bindings instead of a VAO switch - and a
// thousand VAOs would otherwise be a thousand driver-side objects to validate.

#include "mesh/MeshData.hpp"
#include "render/Camera.hpp"
#include "render/GpuBuffer.hpp"
#include "render/Shader.hpp"
#include "world/Block.hpp"
#include "world/VoxelTypes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include <glm/vec3.hpp>

namespace voxl {

/// Per-frame draw accounting, surfaced by the debug overlay.
struct ChunkRenderStats {
    std::uint32_t drawCalls         = 0;
    std::uint64_t trianglesRendered = 0;
    std::uint32_t chunksVisible     = 0;  ///< distinct chunks that passed culling
    std::uint32_t chunksCulled      = 0;
    std::uint32_t chunksResident    = 0;
    /// Monotonic count of meshes uploaded since start-up. Monotonic rather than
    /// per-frame because uploads are drained from the job system's main-thread
    /// queue at a point the renderer does not control; the overlay diffs it.
    std::uint64_t uploadsTotal = 0;
    std::size_t   gpuBytes     = 0;  ///< vertex + index bytes resident on the GPU
    std::array<std::uint32_t, kRenderLayerCount> drawCallsPerLayer{};
};

/// Owns one GPU mesh per resident chunk.
///
/// Pinned (non-copyable, non-movable): the visible lists hold pointers into the
/// residency map, and the shared VAO is a GL object whose lifetime is tied to
/// this object.
class ChunkRenderer {
public:
    /// Vertex buffer binding slot used by the shared VAO.
    static constexpr GLuint kVertexBinding = 0;

    /// Requires a current GL 4.5 context: creates the shared VAO immediately.
    ChunkRenderer();
    ~ChunkRenderer();

    ChunkRenderer(const ChunkRenderer&)            = delete;
    ChunkRenderer& operator=(const ChunkRenderer&) = delete;
    ChunkRenderer(ChunkRenderer&&)                 = delete;
    ChunkRenderer& operator=(ChunkRenderer&&)      = delete;

    // ---------------------------------------------------------- residency --

    /// Uploads (or replaces) the GPU mesh for `mesh.position`. An entirely empty
    /// mesh evicts the chunk instead of keeping a zero-sized record around.
    ///
    /// MAIN THREAD ONLY.
    void upload(const ChunkMeshData& mesh);

    /// Drops a chunk's GPU buffers. Safe for a position that is not resident.
    void remove(const ChunkPos& position);

    /// Releases every GPU mesh. Used on world unload and on shutdown.
    void clear();

    [[nodiscard]] bool contains(const ChunkPos& position) const;

    /// `ChunkMeshData::contentVersion` of the resident mesh, or nullopt when the
    /// chunk has none. The streaming code compares it against
    /// `Chunk::contentVersion()` to decide whether a remesh is still needed.
    [[nodiscard]] std::optional<std::uint32_t> uploadedVersion(const ChunkPos& position) const;

    [[nodiscard]] std::size_t residentChunks() const noexcept { return m_meshes.size(); }
    [[nodiscard]] std::size_t gpuBytes() const noexcept { return m_gpuBytes; }

    // ----------------------------------------------------------- per frame --

    /// Resets the per-frame counters. Call once at the top of the frame.
    void beginFrame() noexcept;

    /// Frustum-culls every resident mesh and orders the survivors:
    /// opaque and cutout front-to-back (so the depth buffer rejects the most
    /// fragments), translucent back-to-front (so blending composites correctly).
    ///
    /// Translucent ordering is per chunk, not per quad. Two water surfaces inside
    /// the same chunk can still blend in the wrong order; fixing that needs
    /// per-quad sorting on the CPU, which is not worth it for the one translucent
    /// material a voxel world actually has in bulk.
    void cull(const Frustum& frustum, const glm::vec3& eyePosition);

    [[nodiscard]] std::size_t visibleCount(RenderLayer layer) const noexcept;

    /// Issues one draw per visible chunk for `layer`. The caller has already
    /// bound the program and set all pipeline state for the pass; this only
    /// rebinds buffers and updates the per-chunk origin uniform.
    ///
    /// `chunkOriginLocation` comes from `ShaderProgram::uniformLocation` and is
    /// passed in rather than looked up per draw.
    void drawLayer(RenderLayer layer, const ShaderProgram& program, GLint chunkOriginLocation);

    [[nodiscard]] const ChunkRenderStats& stats() const noexcept { return m_stats; }

private:
    /// A chunk's geometry: all three layers concatenated into one vertex buffer
    /// and one index buffer.
    ///
    /// Concatenating means three draws share two buffer objects instead of six,
    /// and per-layer indices stay layer-local because glDrawElementsBaseVertex
    /// applies the vertex offset for us.
    struct LayerRange {
        std::uint32_t indexCount      = 0;
        std::uint32_t indexByteOffset = 0;
        std::int32_t  baseVertex      = 0;
        std::uint32_t triangles       = 0;
    };

    struct ChunkMesh {
        ChunkPos      position{};
        std::uint32_t contentVersion = 0;
        GpuBuffer     vertices;
        GpuBuffer     indices;
        std::array<LayerRange, kRenderLayerCount> ranges{};
        Aabb          bounds{};
        std::size_t   byteSize = 0;
    };

    struct VisibleMesh {
        const ChunkMesh* mesh       = nullptr;
        float            distanceSq = 0.0f;
    };

    /// World-space bounds for culling. Falls back to the full chunk box when the
    /// mesher left the tight bounds unset, because an over-tight box would cull
    /// geometry that is actually on screen.
    [[nodiscard]] static Aabb worldBounds(const ChunkMeshData& mesh);

    VertexArray m_vertexArray;

    std::unordered_map<ChunkPos, ChunkMesh> m_meshes;
    std::array<std::vector<VisibleMesh>, kRenderLayerCount> m_visible;

    std::size_t      m_gpuBytes = 0;
    ChunkRenderStats m_stats{};
};

}  // namespace voxl
