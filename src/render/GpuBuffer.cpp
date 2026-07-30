#include "render/GpuBuffer.hpp"

#include "core/Log.hpp"

#include <algorithm>
#include <utility>

namespace voxl {
namespace {

/// Allocation granularity. Rounding up keeps the driver's suballocator on tidy
/// boundaries and stops a chunk that gains one quad per edit from reallocating
/// on every single edit.
constexpr std::size_t kAllocationGranularity = 1024;

/// Head-room added when growing. 25% is enough to absorb the incremental growth
/// of an actively edited chunk without noticeably inflating the resident set.
[[nodiscard]] std::size_t growthTarget(std::size_t bytes) noexcept
{
    const std::size_t withSlack = bytes + bytes / 4;
    return ((withSlack + kAllocationGranularity - 1) / kAllocationGranularity) *
           kAllocationGranularity;
}

}  // namespace

// ---------------------------------------------------------------- GpuBuffer --

GpuBuffer::GpuBuffer(GpuBuffer&& other) noexcept
    : m_id(other.m_id), m_size(other.m_size), m_capacity(other.m_capacity)
{
    other.m_id       = 0;
    other.m_size     = 0;
    other.m_capacity = 0;
}

GpuBuffer& GpuBuffer::operator=(GpuBuffer&& other) noexcept
{
    if (this != &other) {
        destroy();
        m_id             = other.m_id;
        m_size           = other.m_size;
        m_capacity       = other.m_capacity;
        other.m_id       = 0;
        other.m_size     = 0;
        other.m_capacity = 0;
    }
    return *this;
}

void GpuBuffer::destroy() noexcept
{
    if (m_id != 0) {
        glDeleteBuffers(1, &m_id);
        m_id = 0;
    }
    m_size     = 0;
    m_capacity = 0;
}

void GpuBuffer::allocate(std::size_t bytes)
{
    if (m_id != 0) {
        glDeleteBuffers(1, &m_id);
        m_id = 0;
    }
    m_capacity = 0;
    m_size     = 0;
    if (bytes == 0) {
        return;
    }

    glCreateBuffers(1, &m_id);
    VOXL_CHECK(m_id != 0, "glCreateBuffers failed - is a GL 4.5 context current?");
    // GL_DYNAMIC_STORAGE_BIT is what makes glNamedBufferSubData legal on the
    // allocation; without it the storage is write-once at creation time.
    glNamedBufferStorage(m_id, static_cast<GLsizeiptr>(bytes), nullptr, GL_DYNAMIC_STORAGE_BIT);
    m_capacity = bytes;
}

void GpuBuffer::reserve(std::size_t bytes)
{
    if (bytes <= m_capacity) {
        return;
    }
    // Immutable storage cannot be resized in place, so growing discards the old
    // allocation and its contents. `size()` therefore drops to zero: a caller
    // that reserves must write before it draws, and reporting the old size would
    // let a stale `update()` run against uninitialised memory.
    allocate(growthTarget(bytes));
}

void GpuBuffer::upload(const void* data, std::size_t bytes)
{
    if (bytes == 0) {
        m_size = 0;
        return;
    }

    // Shrink aggressively only when the allocation is more than 4x what is
    // needed; a chunk that drops from full to nearly empty should give the
    // memory back, but ordinary frame-to-frame jitter should not reallocate.
    const bool tooSmall = bytes > m_capacity;
    const bool tooLarge = m_capacity > kAllocationGranularity && bytes * 4 < m_capacity;
    if (tooSmall || tooLarge) {
        allocate(growthTarget(bytes));
    }

    m_size = bytes;
    if (data != nullptr) {
        glNamedBufferSubData(m_id, 0, static_cast<GLsizeiptr>(bytes), data);
    }
}

void GpuBuffer::update(std::size_t byteOffset, const void* data, std::size_t bytes)
{
    if (bytes == 0) {
        return;
    }
    VOXL_CHECK(m_id != 0, "GpuBuffer::update on a buffer with no storage");
    VOXL_CHECK(byteOffset + bytes <= m_capacity,
               "GpuBuffer::update out of range: offset {} + {} > capacity {}", byteOffset, bytes,
               m_capacity);
    glNamedBufferSubData(m_id, static_cast<GLintptr>(byteOffset), static_cast<GLsizeiptr>(bytes),
                         data);
    m_size = std::max(m_size, byteOffset + bytes);
}

// -------------------------------------------------------------- VertexArray --

VertexArray::VertexArray(VertexArray&& other) noexcept : m_id(other.m_id)
{
    other.m_id = 0;
}

VertexArray& VertexArray::operator=(VertexArray&& other) noexcept
{
    if (this != &other) {
        destroy();
        m_id       = other.m_id;
        other.m_id = 0;
    }
    return *this;
}

void VertexArray::create()
{
    if (m_id != 0) {
        return;
    }
    glCreateVertexArrays(1, &m_id);
    VOXL_CHECK(m_id != 0, "glCreateVertexArrays failed - is a GL 4.5 context current?");
}

void VertexArray::destroy() noexcept
{
    if (m_id != 0) {
        glDeleteVertexArrays(1, &m_id);
        m_id = 0;
    }
}

void VertexArray::bind() const
{
    glBindVertexArray(m_id);
}

void VertexArray::unbind() noexcept
{
    glBindVertexArray(0);
}

void VertexArray::setIntegerAttribute(GLuint location, GLint components, GLenum type,
                                      GLuint relativeOffset, GLuint bindingIndex)
{
    VOXL_CHECK(m_id != 0, "VertexArray::setIntegerAttribute before create()");
    glEnableVertexArrayAttrib(m_id, location);
    glVertexArrayAttribIFormat(m_id, location, components, type, relativeOffset);
    glVertexArrayAttribBinding(m_id, location, bindingIndex);
}

void VertexArray::bindVertexBuffer(GLuint bindingIndex, GLuint buffer, GLintptr offset,
                                   GLsizei stride)
{
    VOXL_CHECK(m_id != 0, "VertexArray::bindVertexBuffer before create()");
    glVertexArrayVertexBuffer(m_id, bindingIndex, buffer, offset, stride);
}

void VertexArray::bindElementBuffer(GLuint buffer)
{
    VOXL_CHECK(m_id != 0, "VertexArray::bindElementBuffer before create()");
    glVertexArrayElementBuffer(m_id, buffer);
}

}  // namespace voxl
