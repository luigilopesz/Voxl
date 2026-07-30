#pragma once

// RAII wrappers over OpenGL buffer and vertex-array objects.
//
// Everything here uses direct state access (glCreateBuffers / glNamedBuffer* /
// glVertexArray*). DSA is not a style preference: the bind-to-edit model makes
// every helper function a hidden mutation of global state, so a buffer update
// deep inside the chunk uploader would silently invalidate whatever the caller
// had bound. With DSA the only global state left is the currently bound VAO.
//
// Thread safety: NONE. Every member issues GL commands and must therefore run on
// the thread that owns the context - the main thread. Worker threads produce
// ChunkMeshData and hand it over through JobSystem::mainThreadQueue().

#include <glad/gl.h>

#include <cstddef>

namespace voxl {

/// Owns one immutable-storage GL buffer.
///
/// `glNamedBufferStorage` allocates a buffer whose *size* can never change, so
/// growing means deleting and recreating the object. That is hidden behind
/// `upload()`, which over-allocates and keeps the allocation across frames; a
/// chunk that is remeshed repeatedly (block edits) therefore reuses its storage
/// instead of thrashing the driver's allocator.
class GpuBuffer {
public:
    GpuBuffer() noexcept = default;
    ~GpuBuffer() { destroy(); }

    GpuBuffer(const GpuBuffer&)            = delete;
    GpuBuffer& operator=(const GpuBuffer&) = delete;
    GpuBuffer(GpuBuffer&& other) noexcept;
    GpuBuffer& operator=(GpuBuffer&& other) noexcept;

    /// Replaces the contents. Reallocates only when `bytes` does not fit the
    /// current allocation, or when the allocation has become wastefully large.
    /// `data` may be null to reserve space without initialising it.
    void upload(const void* data, std::size_t bytes);

    /// Partial write into the existing allocation. `byteOffset + bytes` must be
    /// within `size()`; the buffer must already have been uploaded to.
    void update(std::size_t byteOffset, const void* data, std::size_t bytes);

    /// Ensures at least `bytes` of storage without touching `size()`.
    void reserve(std::size_t bytes);

    void destroy() noexcept;

    [[nodiscard]] GLuint      id() const noexcept { return m_id; }
    [[nodiscard]] std::size_t size() const noexcept { return m_size; }
    [[nodiscard]] std::size_t capacity() const noexcept { return m_capacity; }
    [[nodiscard]] bool        empty() const noexcept { return m_size == 0; }
    [[nodiscard]] explicit    operator bool() const noexcept { return m_id != 0; }

private:
    /// Deletes any existing object and creates one of exactly `bytes` capacity.
    void allocate(std::size_t bytes);

    GLuint      m_id       = 0;
    std::size_t m_size     = 0;  ///< bytes of live data
    std::size_t m_capacity = 0;  ///< bytes actually allocated on the GPU
};

/// Owns one vertex array object.
///
/// The renderer keeps a single VAO that describes the packed-vertex *format*, and
/// swaps the buffers bound to it per draw with `bindVertexBuffer` /
/// `bindElementBuffer`. That is only possible with DSA and it is a large win:
/// thousands of chunks share one format object instead of one VAO each, and VAO
/// switches (which are the expensive part) disappear from the draw loop.
class VertexArray {
public:
    VertexArray() noexcept = default;
    ~VertexArray() { destroy(); }

    VertexArray(const VertexArray&)            = delete;
    VertexArray& operator=(const VertexArray&) = delete;
    VertexArray(VertexArray&& other) noexcept;
    VertexArray& operator=(VertexArray&& other) noexcept;

    void create();
    void destroy() noexcept;

    void        bind() const;
    static void unbind() noexcept;

    /// Declares an *integer* attribute (glVertexArrayAttribIFormat). The packed
    /// vertex format is two uint32 lanes; feeding them through the normalised
    /// float path would quietly convert the bit patterns into garbage floats.
    void setIntegerAttribute(GLuint location, GLint components, GLenum type,
                             GLuint relativeOffset, GLuint bindingIndex = 0);

    void bindVertexBuffer(GLuint bindingIndex, GLuint buffer, GLintptr offset, GLsizei stride);
    void bindElementBuffer(GLuint buffer);

    [[nodiscard]] GLuint   id() const noexcept { return m_id; }
    [[nodiscard]] explicit operator bool() const noexcept { return m_id != 0; }

private:
    GLuint m_id = 0;
};

}  // namespace voxl
