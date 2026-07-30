#include "render/ChunkRenderer.hpp"

#include "core/Log.hpp"

#include <algorithm>

namespace voxl {
namespace {

/// Attribute locations, matching `layout(location = ...)` in chunk.vert.
constexpr GLuint kAttribData0 = 0;
constexpr GLuint kAttribData1 = 1;

[[nodiscard]] constexpr std::size_t layerIndex(RenderLayer layer) noexcept
{
    return static_cast<std::size_t>(layer);
}

}  // namespace

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

    for (std::vector<VisibleMesh>& list : m_visible) {
        list.reserve(1024);
    }
}

ChunkRenderer::~ChunkRenderer()
{
    clear();
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

void ChunkRenderer::upload(const ChunkMeshData& mesh)
{
    std::size_t totalVertexBytes = 0;
    std::size_t totalIndexBytes  = 0;
    for (const MeshLayerData& layer : mesh.layers) {
        totalVertexBytes += layer.vertexBytes();
        totalIndexBytes += layer.indexBytes();
    }

    if (totalIndexBytes == 0) {
        // Nothing to draw. Evicting rather than keeping an empty record means the
        // draw loop never has to test for it, and a chunk that was mined out
        // gives its VRAM back immediately.
        remove(mesh.position);
        return;
    }

    auto [iterator, inserted] = m_meshes.try_emplace(mesh.position);
    ChunkMesh& target         = iterator->second;
    if (!inserted) {
        m_gpuBytes -= target.byteSize;
    }

    target.position       = mesh.position;
    target.contentVersion = mesh.contentVersion;
    target.bounds         = worldBounds(mesh);

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
    ++m_stats.uploadsTotal;
}

void ChunkRenderer::remove(const ChunkPos& position)
{
    const auto found = m_meshes.find(position);
    if (found == m_meshes.end()) {
        return;
    }
    // Erasing runs the GpuBuffer destructors, which delete the GL objects.
    m_gpuBytes -= std::min(m_gpuBytes, found->second.byteSize);
    m_meshes.erase(found);
}

void ChunkRenderer::clear()
{
    for (std::vector<VisibleMesh>& list : m_visible) {
        list.clear();
    }
    m_meshes.clear();
    m_gpuBytes = 0;
}

bool ChunkRenderer::contains(const ChunkPos& position) const
{
    return m_meshes.find(position) != m_meshes.end();
}

std::optional<std::uint32_t> ChunkRenderer::uploadedVersion(const ChunkPos& position) const
{
    const auto found = m_meshes.find(position);
    if (found == m_meshes.end()) {
        return std::nullopt;
    }
    return found->second.contentVersion;
}

void ChunkRenderer::beginFrame() noexcept
{
    m_stats.drawCalls         = 0;
    m_stats.trianglesRendered = 0;
    m_stats.chunksVisible     = 0;
    m_stats.chunksCulled      = 0;
    m_stats.drawCallsPerLayer.fill(0);
}

void ChunkRenderer::cull(const Frustum& frustum, const glm::vec3& eyePosition)
{
    for (std::vector<VisibleMesh>& list : m_visible) {
        list.clear();
    }

    std::uint32_t visible = 0;
    std::uint32_t culled  = 0;

    for (const auto& [position, mesh] : m_meshes) {
        if (!frustum.intersects(mesh.bounds)) {
            ++culled;
            continue;
        }
        ++visible;

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

    m_stats.chunksVisible  = visible;
    m_stats.chunksCulled   = culled;
    m_stats.chunksResident = static_cast<std::uint32_t>(m_meshes.size());
    m_stats.gpuBytes       = m_gpuBytes;
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
    }

    m_stats.drawCalls += drawCalls;
    m_stats.drawCallsPerLayer[layerIndex(layer)] += drawCalls;
    m_stats.trianglesRendered += triangles;
}

}  // namespace voxl
