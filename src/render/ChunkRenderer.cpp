#include "render/ChunkRenderer.hpp"

#include "core/Log.hpp"

#include <algorithm>

namespace voxl {
namespace {

/// Attribute locations, matching `layout(location = ...)` in chunk.vert and in
/// subvoxel.vert. Both formats are two uint32 lanes, so the numbering is shared.
constexpr GLuint kAttribData0 = 0;
constexpr GLuint kAttribData1 = 1;

[[nodiscard]] constexpr std::size_t layerIndex(RenderLayer layer) noexcept
{
    return static_cast<std::size_t>(layer);
}

}  // namespace

// -------------------------------------------------------- SubVoxelGeometry --

void ChunkRenderer::SubVoxelGeometry::release() noexcept
{
    vertices.destroy();
    indices.destroy();
    indexCount = 0;
    triangles  = 0;
    byteSize   = 0;
}

// ------------------------------------------------------------- lifecycle --

ChunkRenderer::ChunkRenderer()
{
    m_vertexArray.create();
    // Both lanes of the packed vertex are unsigned integers. glVertexAttribIPointer
    // (the I) is mandatory: the float path would normalise or convert the bit
    // patterns and every unpacked field would come out wrong.
    m_vertexArray.setIntegerAttribute(kAttribData0, 1, GL_UNSIGNED_INT,
                                      offsetof(PackedVertex, data0), kVertexBinding);
    m_vertexArray.setIntegerAttribute(kAttribData1, 1, GL_UNSIGNED_INT,
                                      offsetof(PackedVertex, data1), kVertexBinding);

    m_subVoxelVertexArray.create();
    m_subVoxelVertexArray.setIntegerAttribute(kAttribData0, 1, GL_UNSIGNED_INT,
                                              offsetof(PackedSubVoxelVertex, data0),
                                              kVertexBinding);
    m_subVoxelVertexArray.setIntegerAttribute(kAttribData1, 1, GL_UNSIGNED_INT,
                                              offsetof(PackedSubVoxelVertex, data1),
                                              kVertexBinding);

    for (std::vector<VisibleMesh>& list : m_visible) {
        list.reserve(1024);
    }
    // Damaged chunks are a handful even in a heavily mined world, so this list
    // is deliberately sized two orders of magnitude below the others.
    m_visibleSubVoxel.reserve(64);
}

ChunkRenderer::~ChunkRenderer()
{
    clear();
}

std::size_t ChunkRenderer::lodIndex(LodLevel level) noexcept
{
    return std::min<std::size_t>(static_cast<std::size_t>(level), kLodCount - 1u);
}

Aabb ChunkRenderer::worldBounds(const ChunkMeshData& mesh)
{
    const BlockPos  origin = mesh.position.originBlock();
    const glm::vec3 base{static_cast<float>(origin.x), static_cast<float>(origin.y),
                         static_cast<float>(origin.z)};

    const glm::vec3 extent = mesh.boundsMax - mesh.boundsMin;
    const bool      unset  = extent.x <= 0.0f && extent.y <= 0.0f && extent.z <= 0.0f;
    if (unset) {
        return Aabb::fromChunk(mesh.position);
    }
    return Aabb{base + mesh.boundsMin, base + mesh.boundsMax};
}

void ChunkRenderer::refreshBounds(ChunkMesh& mesh) noexcept
{
    mesh.bounds = mesh.subVoxel.empty() ? mesh.meshBounds : Aabb::fromChunk(mesh.position);
}

void ChunkRenderer::releaseBlockGeometry(ChunkMesh& mesh) noexcept
{
    m_gpuBytes -= std::min(m_gpuBytes, mesh.byteSize);
    mesh.vertices.destroy();
    mesh.indices.destroy();
    mesh.ranges.fill(LayerRange{});
    mesh.byteSize = 0;
}

// -------------------------------------------------------- whole-block mesh --

void ChunkRenderer::upload(const ChunkMeshData& mesh, LodLevel lod)
{
    std::size_t totalVertexBytes = 0;
    std::size_t totalIndexBytes  = 0;
    for (const MeshLayerData& layer : mesh.layers) {
        totalVertexBytes += layer.vertexBytes();
        totalIndexBytes += layer.indexBytes();
    }

    if (totalIndexBytes == 0) {
        // Nothing whole-block to draw. Evicting rather than keeping an empty
        // record means the draw loop never has to test for it, and a chunk that
        // was mined out gives its VRAM back immediately - but a chunk mined down
        // to a single damaged block has no whole-block quads left and must still
        // draw its sub-voxel geometry, so the record survives in that case.
        const auto found = m_meshes.find(mesh.position);
        if (found == m_meshes.end()) {
            return;
        }
        ChunkMesh& target = found->second;
        releaseBlockGeometry(target);
        if (target.subVoxel.empty()) {
            m_meshes.erase(found);
            return;
        }
        target.contentVersion = mesh.contentVersion;
        target.lod            = lod;
        target.meshBounds     = Aabb::fromChunk(mesh.position);
        refreshBounds(target);
        ++m_stats.uploadsTotal;
        return;
    }

    auto [iterator, inserted] = m_meshes.try_emplace(mesh.position);
    ChunkMesh& target         = iterator->second;
    if (!inserted) {
        m_gpuBytes -= std::min(m_gpuBytes, target.byteSize);
    }

    target.position       = mesh.position;
    target.contentVersion = mesh.contentVersion;
    target.lod            = lod;
    target.meshBounds     = worldBounds(mesh);

    // Size the buffers once, then write each layer straight into its slice. This
    // avoids staging the concatenation in a scratch vector: the mesh data is
    // already contiguous per layer, so the driver copies it directly.
    target.vertices.upload(nullptr, totalVertexBytes);
    target.indices.upload(nullptr, totalIndexBytes);

    std::size_t vertexCursor = 0;
    std::size_t indexCursor  = 0;
    for (std::size_t i = 0; i < kRenderLayerCount; ++i) {
        const MeshLayerData& source = mesh.layers[i];
        LayerRange&          range  = target.ranges[i];

        range.indexCount      = static_cast<std::uint32_t>(source.indexCount());
        range.indexByteOffset = static_cast<std::uint32_t>(indexCursor);
        range.baseVertex      = static_cast<std::int32_t>(vertexCursor / sizeof(PackedVertex));
        range.triangles       = static_cast<std::uint32_t>(source.triangleCount());

        if (range.indexCount != 0) {
            target.vertices.update(vertexCursor, source.vertices.data(), source.vertexBytes());
            target.indices.update(indexCursor, source.indices.data(), source.indexBytes());
        }
        vertexCursor += source.vertexBytes();
        indexCursor += source.indexBytes();
    }

    target.byteSize = totalVertexBytes + totalIndexBytes;
    m_gpuBytes += target.byteSize;
    refreshBounds(target);
    ++m_stats.uploadsTotal;
}

// ----------------------------------------------------------- sub-voxels --

void ChunkRenderer::uploadSubVoxels(const ChunkPos& position, const SubVoxelMeshData& mesh)
{
    const std::size_t vertexBytes = mesh.vertices.size() * sizeof(PackedSubVoxelVertex);
    const std::size_t indexBytes  = mesh.indices.size() * sizeof(std::uint32_t);

    if (indexBytes == 0) {
        removeSubVoxels(position);
        return;
    }

    // A chunk can acquire damage before (or without) its whole-block mesh: the
    // upload order between the two streams is whatever the job system drains
    // first. Creating the record here with the full chunk box keeps the geometry
    // visible until upload() supplies tighter bounds.
    auto [iterator, inserted] = m_meshes.try_emplace(position);
    ChunkMesh& target         = iterator->second;
    if (inserted) {
        target.position   = position;
        target.meshBounds = Aabb::fromChunk(position);
    }

    SubVoxelGeometry& geometry = target.subVoxel;
    m_gpuBytes -= std::min(m_gpuBytes, geometry.byteSize);
    m_subVoxelGpuBytes -= std::min(m_subVoxelGpuBytes, geometry.byteSize);

    geometry.vertices.upload(mesh.vertices.data(), vertexBytes);
    geometry.indices.upload(mesh.indices.data(), indexBytes);
    geometry.indexCount = static_cast<std::uint32_t>(mesh.indices.size());
    geometry.triangles  = static_cast<std::uint32_t>(mesh.triangleCount());
    geometry.byteSize   = vertexBytes + indexBytes;

    m_gpuBytes += geometry.byteSize;
    m_subVoxelGpuBytes += geometry.byteSize;

    refreshBounds(target);
}

void ChunkRenderer::removeSubVoxels(const ChunkPos& position)
{
    const auto found = m_meshes.find(position);
    if (found == m_meshes.end()) {
        return;
    }
    ChunkMesh&        target   = found->second;
    SubVoxelGeometry& geometry = target.subVoxel;
    if (geometry.empty() && geometry.byteSize == 0) {
        return;
    }

    m_gpuBytes -= std::min(m_gpuBytes, geometry.byteSize);
    m_subVoxelGpuBytes -= std::min(m_subVoxelGpuBytes, geometry.byteSize);
    geometry.release();

    if (!target.hasBlockGeometry()) {
        // The record only existed to carry the damage; repairing it fully takes
        // the chunk out of residency rather than leaving an empty shell that the
        // cull loop still has to visit.
        m_meshes.erase(found);
        return;
    }
    refreshBounds(target);
}

// -------------------------------------------------------------- residency --

void ChunkRenderer::remove(const ChunkPos& position)
{
    const auto found = m_meshes.find(position);
    if (found == m_meshes.end()) {
        return;
    }
    // Erasing runs the GpuBuffer destructors - all four of them - which delete
    // the GL objects. Both streams therefore always die together with the chunk.
    const ChunkMesh& target = found->second;
    m_gpuBytes -= std::min(m_gpuBytes, target.byteSize + target.subVoxel.byteSize);
    m_subVoxelGpuBytes -= std::min(m_subVoxelGpuBytes, target.subVoxel.byteSize);
    m_meshes.erase(found);
}

void ChunkRenderer::clear()
{
    for (std::vector<VisibleMesh>& list : m_visible) {
        list.clear();
    }
    m_visibleSubVoxel.clear();
    m_meshes.clear();
    m_gpuBytes         = 0;
    m_subVoxelGpuBytes = 0;
}

bool ChunkRenderer::contains(const ChunkPos& position) const
{
    return m_meshes.find(position) != m_meshes.end();
}

bool ChunkRenderer::hasSubVoxels(const ChunkPos& position) const
{
    const auto found = m_meshes.find(position);
    return found != m_meshes.end() && !found->second.subVoxel.empty();
}

std::optional<std::uint32_t> ChunkRenderer::uploadedVersion(const ChunkPos& position) const
{
    const auto found = m_meshes.find(position);
    if (found == m_meshes.end()) {
        return std::nullopt;
    }
    return found->second.contentVersion;
}

std::optional<LodLevel> ChunkRenderer::uploadedLod(const ChunkPos& position) const
{
    const auto found = m_meshes.find(position);
    if (found == m_meshes.end()) {
        return std::nullopt;
    }
    return found->second.lod;
}

// -------------------------------------------------------------- per frame --

void ChunkRenderer::beginFrame() noexcept
{
    m_stats.drawCalls         = 0;
    m_stats.trianglesRendered = 0;
    m_stats.chunksVisible     = 0;
    m_stats.chunksCulled      = 0;
    m_stats.drawCallsPerLayer.fill(0);
    m_stats.drawCallsPerLod.fill(0);
    m_stats.trianglesPerLod.fill(0);
    m_stats.subVoxelDrawCalls = 0;
    m_stats.subVoxelTriangles = 0;
}

void ChunkRenderer::cull(const Frustum& frustum, const glm::vec3& eyePosition)
{
    for (std::vector<VisibleMesh>& list : m_visible) {
        list.clear();
    }
    m_visibleSubVoxel.clear();

    std::uint32_t visible = 0;
    std::uint32_t culled  = 0;

    m_stats.chunksResidentPerLod.fill(0);
    m_stats.chunksVisiblePerLod.fill(0);
    m_stats.subVoxelChunksResident = 0;
    m_stats.subVoxelChunksVisible  = 0;

    for (const auto& [position, mesh] : m_meshes) {
        const std::size_t level = lodIndex(mesh.lod);
        ++m_stats.chunksResidentPerLod[level];
        const bool damaged = !mesh.subVoxel.empty();
        if (damaged) {
            ++m_stats.subVoxelChunksResident;
        }

        // ONE frustum test per chunk feeds both passes. Testing the sub-voxel
        // geometry separately - against its own tighter box, say - would let a
        // carved surface survive a frame in which its parent chunk was culled,
        // which reads on screen as debris floating in the void.
        if (!frustum.intersects(mesh.bounds)) {
            ++culled;
            continue;
        }
        ++visible;
        ++m_stats.chunksVisiblePerLod[level];

        // Squared distance to the box centre. The centre (rather than the
        // nearest point) is stable as the camera moves, which keeps the sort
        // order from flickering between frames and popping translucent surfaces.
        const glm::vec3 delta      = mesh.bounds.centre() - eyePosition;
        const float     distanceSq = glm::dot(delta, delta);

        for (std::size_t i = 0; i < kRenderLayerCount; ++i) {
            if (mesh.ranges[i].indexCount != 0) {
                m_visible[i].push_back(VisibleMesh{&mesh, distanceSq});
            }
        }
        if (damaged) {
            m_visibleSubVoxel.push_back(VisibleMesh{&mesh, distanceSq});
            ++m_stats.subVoxelChunksVisible;
        }
    }

    const auto nearFirst = [](const VisibleMesh& a, const VisibleMesh& b) noexcept {
        return a.distanceSq < b.distanceSq;
    };
    const auto farFirst = [](const VisibleMesh& a, const VisibleMesh& b) noexcept {
        return a.distanceSq > b.distanceSq;
    };

    std::sort(m_visible[layerIndex(RenderLayer::Opaque)].begin(),
              m_visible[layerIndex(RenderLayer::Opaque)].end(), nearFirst);
    std::sort(m_visible[layerIndex(RenderLayer::Cutout)].begin(),
              m_visible[layerIndex(RenderLayer::Cutout)].end(), nearFirst);
    std::sort(m_visible[layerIndex(RenderLayer::Translucent)].begin(),
              m_visible[layerIndex(RenderLayer::Translucent)].end(), farFirst);
    // Sub-voxel geometry is opaque, so it wants the same front-to-back order for
    // the same reason: early-Z rejects the fragments behind it.
    std::sort(m_visibleSubVoxel.begin(), m_visibleSubVoxel.end(), nearFirst);

    m_stats.chunksVisible    = visible;
    m_stats.chunksCulled     = culled;
    m_stats.chunksResident   = static_cast<std::uint32_t>(m_meshes.size());
    m_stats.gpuBytes         = m_gpuBytes;
    m_stats.subVoxelGpuBytes = m_subVoxelGpuBytes;
}

std::size_t ChunkRenderer::visibleCount(RenderLayer layer) const noexcept
{
    return m_visible[layerIndex(layer)].size();
}

void ChunkRenderer::drawLayer(RenderLayer layer, const ShaderProgram& program,
                              GLint chunkOriginLocation)
{
    const std::vector<VisibleMesh>& list = m_visible[layerIndex(layer)];
    if (list.empty()) {
        return;
    }

    m_vertexArray.bind();

    std::uint32_t drawCalls = 0;
    std::uint64_t triangles = 0;
    for (const VisibleMesh& entry : list) {
        const ChunkMesh&  mesh  = *entry.mesh;
        const LayerRange& range = mesh.ranges[layerIndex(layer)];

        m_vertexArray.bindVertexBuffer(kVertexBinding, mesh.vertices.id(), 0,
                                       static_cast<GLsizei>(sizeof(PackedVertex)));
        m_vertexArray.bindElementBuffer(mesh.indices.id());

        // Vertex positions are chunk-local 6-bit integers; the origin uniform is
        // what places the chunk in the world.
        const BlockPos origin = mesh.position.originBlock();
        program.setVec3(chunkOriginLocation,
                        glm::vec3{static_cast<float>(origin.x), static_cast<float>(origin.y),
                                  static_cast<float>(origin.z)});

        glDrawElementsBaseVertex(
            GL_TRIANGLES, static_cast<GLsizei>(range.indexCount), GL_UNSIGNED_INT,
            reinterpret_cast<const void*>(static_cast<std::uintptr_t>(range.indexByteOffset)),
            range.baseVertex);

        ++drawCalls;
        triangles += range.triangles;

        const std::size_t level = lodIndex(mesh.lod);
        ++m_stats.drawCallsPerLod[level];
        m_stats.trianglesPerLod[level] += range.triangles;
    }

    m_stats.drawCalls += drawCalls;
    m_stats.drawCallsPerLayer[layerIndex(layer)] += drawCalls;
    m_stats.trianglesRendered += triangles;
}

void ChunkRenderer::drawSubVoxels(const ShaderProgram& program, GLint chunkOriginLocation)
{
    // The early out is the feature's entire performance story. Nearly every
    // chunk is undamaged, and binding a VAO plus issuing a zero-index draw per
    // chunk would cost more than the geometry it renders.
    if (m_visibleSubVoxel.empty()) {
        return;
    }

    m_subVoxelVertexArray.bind();

    std::uint32_t drawCalls = 0;
    std::uint64_t triangles = 0;
    for (const VisibleMesh& entry : m_visibleSubVoxel) {
        const ChunkMesh&        mesh     = *entry.mesh;
        const SubVoxelGeometry& geometry = mesh.subVoxel;

        m_subVoxelVertexArray.bindVertexBuffer(kVertexBinding, geometry.vertices.id(), 0,
                                               static_cast<GLsizei>(sizeof(PackedSubVoxelVertex)));
        m_subVoxelVertexArray.bindElementBuffer(geometry.indices.id());

        // Sub-voxel positions are chunk-local in SUB-VOXEL units; subvoxel.vert
        // scales them back into block space before adding this origin, so the
        // uniform carries the same world-space block origin as the main pass.
        const BlockPos origin = mesh.position.originBlock();
        program.setVec3(chunkOriginLocation,
                        glm::vec3{static_cast<float>(origin.x), static_cast<float>(origin.y),
                                  static_cast<float>(origin.z)});

        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(geometry.indexCount), GL_UNSIGNED_INT,
                       nullptr);

        ++drawCalls;
        triangles += geometry.triangles;

        const std::size_t level = lodIndex(mesh.lod);
        ++m_stats.drawCallsPerLod[level];
        m_stats.trianglesPerLod[level] += geometry.triangles;
    }

    // Counted into the frame totals as well as the breakdown: these are real
    // draw calls in the opaque pass, and a debug overlay whose "draw calls"
    // figure excluded them would understate the frame.
    m_stats.drawCalls += drawCalls;
    m_stats.drawCallsPerLayer[layerIndex(RenderLayer::Opaque)] += drawCalls;
    m_stats.trianglesRendered += triangles;
    m_stats.subVoxelDrawCalls += drawCalls;
    m_stats.subVoxelTriangles += triangles;
}

}  // namespace voxl
