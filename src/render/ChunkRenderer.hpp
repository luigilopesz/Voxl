#pragma once

// GPU residency, culling and draw submission for chunk meshes.
//
// THREADING CONTRACT: every method here touches GL and must run on the main
// thread. Worker threads build ChunkMeshData / SubVoxelMeshData (plain CPU
// structures) and post the upload to JobSystem::mainThreadQueue(); that queue's
// drain is the only place `upload()` and `uploadSubVoxels()` are legal to call
// from.
//
// One VAO is shared by every chunk. The packed-vertex *format* never changes, so
// per-draw work is two DSA buffer bindings instead of a VAO switch - and a
// thousand VAOs would otherwise be a thousand driver-side objects to validate.
//
// TWO GEOMETRY STREAMS PER CHUNK
// ------------------------------
// Whole-block geometry (mesh/MeshData.hpp) and sub-voxel damage geometry
// (mesh/SubVoxelMesh.hpp) use different vertex packings, so they get different
// buffers, a different VAO and a different program. They nonetheless share ONE
// residency record per chunk, for two reasons:
//
//   * lifetime - a chunk unloads once, and one record means one place that frees
//     both buffer pairs. Two maps would eventually leak the half nobody removed.
//   * culling  - the requirement is that sub-voxel geometry can never appear
//     without its parent chunk. One record means one frustum test and one
//     visibility decision feeding both passes, so that cannot drift.
//
// Nearly every chunk has no damage at all, and a record with no sub-voxel
// indices is never pushed onto the sub-voxel visible list. The feature therefore
// costs zero draw calls on undamaged terrain.

#include "mesh/MeshData.hpp"
#include "mesh/SubVoxelMesh.hpp"
#include "render/Camera.hpp"
#include "render/GpuBuffer.hpp"
#include "render/Shader.hpp"
#include "world/Block.hpp"
#include "world/Lod.hpp"
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

    // ------------------------------------------------------ level of detail --
    //
    // Indexed by LodLevel. The point of the breakdown is to answer "where is the
    // triangle budget going" - a distant ring drawn at level 3 that still costs
    // as much as the level-0 core means the LOD policy is not earning its keep,
    // and a single aggregate number hides that completely.

    std::array<std::uint32_t, kLodCount> chunksResidentPerLod{};
    std::array<std::uint32_t, kLodCount> chunksVisiblePerLod{};
    std::array<std::uint32_t, kLodCount> drawCallsPerLod{};
    std::array<std::uint64_t, kLodCount> trianglesPerLod{};

    // ----------------------------------------------------------- sub-voxels --
    //
    // A SUBSET of `drawCalls` / `trianglesRendered` above, not an addition to
    // them: the sub-voxel pass is real work in the opaque pass and belongs in
    // the frame totals. These fields exist only to attribute it.

    std::uint32_t subVoxelDrawCalls      = 0;
    std::uint64_t subVoxelTriangles      = 0;
    std::uint32_t subVoxelChunksResident = 0;  ///< chunks holding damage geometry
    std::uint32_t subVoxelChunksVisible  = 0;
    std::size_t   subVoxelGpuBytes       = 0;  ///< included in `gpuBytes`
};

/// Owns one GPU mesh per resident chunk.
///
/// Pinned (non-copyable, non-movable): the visible lists hold pointers into the
/// residency map, and the shared VAO is a GL object whose lifetime is tied to
/// this object.
class ChunkRenderer {
public:
    /// Vertex buffer binding slot used by the shared VAOs.
    static constexpr GLuint kVertexBinding = 0;

    /// Requires a current GL 4.5 context: creates the shared VAOs immediately.
    ChunkRenderer();
    ~ChunkRenderer();

    ChunkRenderer(const ChunkRenderer&)            = delete;
    ChunkRenderer& operator=(const ChunkRenderer&) = delete;
    ChunkRenderer(ChunkRenderer&&)                 = delete;
    ChunkRenderer& operator=(ChunkRenderer&&)      = delete;

    // ---------------------------------------------------------- residency --

    /// Uploads (or replaces) the whole-block GPU mesh for `mesh.position`.
    ///
    /// `lod` is the level the geometry was sampled at, used only for the stats
    /// breakdown - the vertex format and the draw path are identical at every
    /// level (see world/Lod.hpp). It is a parameter rather than a field of
    /// ChunkMeshData because that header is a frozen contract; see the
    /// integration note in the report accompanying this change.
    ///
    /// An entirely empty mesh evicts the chunk, UNLESS it still carries
    /// sub-voxel geometry - a chunk mined down to a single damaged block has no
    /// whole-block quads left but must still draw.
    ///
    /// MAIN THREAD ONLY.
    void upload(const ChunkMeshData& mesh, LodLevel lod = kLodFull);

    /// Uploads (or replaces) the sub-voxel damage geometry for `position`.
    ///
    /// An empty mesh releases the sub-voxel buffers, which is the repair path:
    /// a block restored to whole leaves the store, the mesher emits nothing, and
    /// the VRAM goes back immediately rather than lingering as a zero-length
    /// allocation. If the chunk then has no whole-block geometry either, the
    /// whole record is evicted.
    ///
    /// Legal for a chunk with no whole-block mesh yet: the record is created
    /// with the full chunk box as its cull volume until `upload()` supplies
    /// tighter bounds.
    ///
    /// MAIN THREAD ONLY.
    void uploadSubVoxels(const ChunkPos& position, const SubVoxelMeshData& mesh);

    /// Releases only the sub-voxel buffers. Equivalent to uploading an empty
    /// SubVoxelMeshData; provided so the damage-repair path does not have to
    /// construct one.
    void removeSubVoxels(const ChunkPos& position);

    /// Drops a chunk's GPU buffers - both streams. Safe for a position that is
    /// not resident.
    void remove(const ChunkPos& position);

    /// Releases every GPU mesh. Used on world unload and on shutdown.
    void clear();

    [[nodiscard]] bool contains(const ChunkPos& position) const;

    /// True when the chunk currently holds sub-voxel damage geometry on the GPU.
    [[nodiscard]] bool hasSubVoxels(const ChunkPos& position) const;

    /// `ChunkMeshData::contentVersion` of the resident mesh, or nullopt when the
    /// chunk has none. The streaming code compares it against
    /// `Chunk::contentVersion()` to decide whether a remesh is still needed.
    [[nodiscard]] std::optional<std::uint32_t> uploadedVersion(const ChunkPos& position) const;

    /// LOD level the resident mesh was built at, or nullopt when not resident.
    /// The streaming policy compares it against `LodPolicy::levelFor` to decide
    /// whether the chunk has to be rebuilt at a different resolution.
    [[nodiscard]] std::optional<LodLevel> uploadedLod(const ChunkPos& position) const;

    [[nodiscard]] std::size_t residentChunks() const noexcept { return m_meshes.size(); }
    [[nodiscard]] std::size_t gpuBytes() const noexcept { return m_gpuBytes; }
    [[nodiscard]] std::size_t subVoxelGpuBytes() const noexcept { return m_subVoxelGpuBytes; }

    // ----------------------------------------------------------- per frame --

    /// Resets the per-frame counters. Call once at the top of the frame.
    void beginFrame() noexcept;

    /// Frustum-culls every resident mesh and orders the survivors:
    /// opaque and cutout front-to-back (so the depth buffer rejects the most
    /// fragments), translucent back-to-front (so blending composites correctly).
    ///
    /// The sub-voxel list is populated from the SAME per-chunk frustum test, so
    /// damage geometry is visible exactly when its parent chunk is. It is sorted
    /// front-to-back with the opaque list it is drawn alongside.
    ///
    /// Translucent ordering is per chunk, not per quad. Two water surfaces inside
    /// the same chunk can still blend in the wrong order; fixing that needs
    /// per-quad sorting on the CPU, which is not worth it for the one translucent
    /// material a voxel world actually has in bulk.
    void cull(const Frustum& frustum, const glm::vec3& eyePosition);

    [[nodiscard]] std::size_t visibleCount(RenderLayer layer) const noexcept;
    [[nodiscard]] std::size_t visibleSubVoxelCount() const noexcept
    {
        return m_visibleSubVoxel.size();
    }

    /// Issues one draw per visible chunk for `layer`. The caller has already
    /// bound the program and set all pipeline state for the pass; this only
    /// rebinds buffers and updates the per-chunk origin uniform.
    ///
    /// `chunkOriginLocation` comes from `ShaderProgram::uniformLocation` and is
    /// passed in rather than looked up per draw.
    void drawLayer(RenderLayer layer, const ShaderProgram& program, GLint chunkOriginLocation);

    /// Issues the sub-voxel pass: one draw per visible chunk that has damage.
    /// Returns immediately - without binding anything - when nothing is damaged,
    /// which is the overwhelmingly common case and the reason the pass is free.
    ///
    /// Pipeline state is the caller's, exactly as for `drawLayer`: this belongs
    /// inside the opaque pass, after the whole-block geometry, sharing its depth
    /// buffer and its block texture array.
    void drawSubVoxels(const ShaderProgram& program, GLint chunkOriginLocation);

    [[nodiscard]] const ChunkRenderStats& stats() const noexcept { return m_stats; }

private:
    /// A chunk's whole-block geometry: all three layers concatenated into one
    /// vertex buffer and one index buffer.
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

    /// A chunk's sub-voxel damage geometry. Separate buffers because the vertex
    /// packing is a different 8 bytes (9-bit positions); see SubVoxelMesh.hpp.
    struct SubVoxelGeometry {
        GpuBuffer     vertices;
        GpuBuffer     indices;
        std::uint32_t indexCount = 0;
        std::uint32_t triangles  = 0;
        std::size_t   byteSize   = 0;

        [[nodiscard]] bool empty() const noexcept { return indexCount == 0; }

        /// Deletes the GL objects and zeroes the accounting. Main thread only.
        void release() noexcept;
    };

    struct ChunkMesh {
        ChunkPos      position{};
        std::uint32_t contentVersion = 0;
        LodLevel      lod            = kLodFull;
        GpuBuffer     vertices;
        GpuBuffer     indices;
        std::array<LayerRange, kRenderLayerCount> ranges{};
        SubVoxelGeometry subVoxel;

        /// Tight bounds of the whole-block geometry, as reported by the mesher.
        Aabb meshBounds{};
        /// What culling actually tests. Equal to `meshBounds` normally, widened
        /// to the full chunk box while sub-voxel geometry is present - the
        /// mesher's tight bounds describe whole-block quads only and would clip
        /// away damage that sits outside them.
        Aabb        bounds{};
        std::size_t byteSize = 0;  ///< whole-block bytes only

        [[nodiscard]] bool hasBlockGeometry() const noexcept { return byteSize != 0; }
    };

    struct VisibleMesh {
        const ChunkMesh* mesh       = nullptr;
        float            distanceSq = 0.0f;
    };

    /// World-space bounds for culling. Falls back to the full chunk box when the
    /// mesher left the tight bounds unset, because an over-tight box would cull
    /// geometry that is actually on screen.
    [[nodiscard]] static Aabb worldBounds(const ChunkMeshData& mesh);

    /// Recomputes `bounds` from `meshBounds` and the presence of damage.
    static void refreshBounds(ChunkMesh& mesh) noexcept;

    /// Frees a chunk's whole-block buffers and zeroes its layer ranges, leaving
    /// any sub-voxel geometry alone.
    void releaseBlockGeometry(ChunkMesh& mesh) noexcept;

    /// LodLevel as an array index, clamped. A level outside 0..kLodMax is a bug
    /// on the caller's side, but silently reading past the end of a stats array
    /// is a much worse way to find out about it.
    [[nodiscard]] static std::size_t lodIndex(LodLevel level) noexcept;

    VertexArray m_vertexArray;

    /// Second VAO for the sub-voxel format. The two layouts happen to agree
    /// today (two uint32 lanes, 8-byte stride), but they are independent
    /// contracts owned by different headers - sharing one VAO would make a
    /// future change to either silently corrupt the other.
    VertexArray m_subVoxelVertexArray;

    std::unordered_map<ChunkPos, ChunkMesh> m_meshes;
    std::array<std::vector<VisibleMesh>, kRenderLayerCount> m_visible;
    std::vector<VisibleMesh>                                m_visibleSubVoxel;

    std::size_t      m_gpuBytes         = 0;
    std::size_t      m_subVoxelGpuBytes = 0;
    ChunkRenderStats m_stats{};
};

}  // namespace voxl
