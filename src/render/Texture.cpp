#include "render/Texture.hpp"

#include "core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace voxl {
namespace {

/// Cap on anisotropic samples. Beyond 8x the cost keeps rising and the visible
/// difference on a 32x32 texture does not, so the cap is a straight win.
constexpr float kMaxAnisotropy = 8.0f;

[[nodiscard]] bool anisotropySupported() noexcept
{
    // Core since 4.6; the EXT spelling uses the same enum values, so one code
    // path covers both.
    return GLAD_GL_VERSION_4_6 != 0 || GLAD_GL_EXT_texture_filter_anisotropic != 0;
}

}  // namespace

TextureArray::TextureArray(TextureArray&& other) noexcept
    : m_id(other.m_id),
      m_width(other.m_width),
      m_height(other.m_height),
      m_layers(other.m_layers),
      m_mipLevels(other.m_mipLevels),
      m_anisotropy(other.m_anisotropy)
{
    other.m_id = 0;
}

TextureArray& TextureArray::operator=(TextureArray&& other) noexcept
{
    if (this != &other) {
        destroy();
        m_id         = other.m_id;
        m_width      = other.m_width;
        m_height     = other.m_height;
        m_layers     = other.m_layers;
        m_mipLevels  = other.m_mipLevels;
        m_anisotropy = other.m_anisotropy;
        other.m_id   = 0;
    }
    return *this;
}

void TextureArray::destroy() noexcept
{
    if (m_id != 0) {
        glDeleteTextures(1, &m_id);
        m_id = 0;
    }
    m_width     = 0;
    m_height    = 0;
    m_layers    = 0;
    m_mipLevels = 0;
}

int TextureArray::completeMipLevels(int size) noexcept
{
    int levels = 1;
    while (size > 1) {
        size /= 2;
        ++levels;
    }
    return levels;
}

bool TextureArray::create(int width, int height, int layers, GLenum internalFormat, int mipLevels)
{
    destroy();
    if (width <= 0 || height <= 0 || layers <= 0) {
        VOXL_LOG_ERROR("TextureArray::create with degenerate dimensions {}x{}x{}", width, height,
                       layers);
        return false;
    }

    GLint maxLayers = 0;
    glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &maxLayers);
    if (maxLayers > 0 && layers > maxLayers) {
        VOXL_LOG_ERROR("TextureArray needs {} layers, driver allows {}", layers, maxLayers);
        return false;
    }

    const int levels = mipLevels > 0 ? mipLevels : completeMipLevels(std::min(width, height));

    glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &m_id);
    if (m_id == 0) {
        VOXL_LOG_ERROR("glCreateTextures failed - is a GL 4.5 context current?");
        return false;
    }
    glTextureStorage3D(m_id, levels, internalFormat, width, height, layers);

    m_width     = width;
    m_height    = height;
    m_layers    = layers;
    m_mipLevels = levels;
    applySampling();

    VOXL_LOG_INFO("texture array {}: {}x{} x {} layer(s), {} mip level(s), {:.0f}x anisotropy",
                  m_id, width, height, layers, levels, m_anisotropy);
    return true;
}

void TextureArray::applySampling()
{
    glTextureParameteri(m_id, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
    glTextureParameteri(m_id, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    // GL_REPEAT is load bearing: greedy-meshed quads carry UVs in block units
    // (0..32), and the wrap is what tiles the material across the whole quad.
    glTextureParameteri(m_id, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTextureParameteri(m_id, GL_TEXTURE_WRAP_T, GL_REPEAT);

    m_anisotropy = 1.0f;
    if (anisotropySupported()) {
        GLfloat driverMax = 1.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &driverMax);
        m_anisotropy = std::min(driverMax, kMaxAnisotropy);
        if (m_anisotropy > 1.0f) {
            glTextureParameterf(m_id, GL_TEXTURE_MAX_ANISOTROPY, m_anisotropy);
        }
    }
}

void TextureArray::uploadLayer(int layer, const std::uint8_t* rgba, int width, int height)
{
    VOXL_CHECK(m_id != 0, "TextureArray::uploadLayer before create()");
    VOXL_CHECK(layer >= 0 && layer < m_layers, "texture layer {} out of range (have {})", layer,
               m_layers);
    VOXL_CHECK(width == m_width && height == m_height,
               "texture layer {} is {}x{} but the array is {}x{}", layer, width, height, m_width,
               m_height);
    VOXL_CHECK(rgba != nullptr, "texture layer {} has no pixels", layer);

    glTextureSubImage3D(m_id, 0, 0, 0, layer, width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

void TextureArray::generateMipmaps()
{
    if (m_id == 0 || m_mipLevels <= 1) {
        return;
    }
    glGenerateTextureMipmap(m_id);
}

void TextureArray::bindUnit(GLuint unit) const
{
    glBindTextureUnit(unit, m_id);
}

std::size_t TextureArray::estimatedBytes() const noexcept
{
    if (m_id == 0) {
        return 0;
    }
    const std::size_t mipZero = static_cast<std::size_t>(m_width) *
                                static_cast<std::size_t>(m_height) *
                                static_cast<std::size_t>(m_layers) * 4u;
    // A complete mip chain adds 1/3 on top of the base level.
    return m_mipLevels > 1 ? mipZero + mipZero / 3u : mipZero;
}

// ------------------------------------------------------- block texture array --

TextureArray createBlockTextureArray(int resolution, const BlockTextureLoader& loader)
{
    TextureArray array;
    const int    size = resolution > 0 ? resolution : kBlockTextureSize;

    // GL_SRGB8_ALPHA8, not GL_RGBA8: the painted colours are sRGB-encoded, so the
    // hardware must linearise them on sample or every lighting multiply happens
    // in the wrong space and the world comes out muddy in shadow and blown out in
    // sunlight. The matching encode happens at framebuffer write time.
    if (!array.create(size, size, kBlockTextureLayerCount, GL_SRGB8_ALPHA8)) {
        return array;
    }

    std::size_t loadedFromDisk = 0;
    for (int layer = 0; layer < kBlockTextureLayerCount; ++layer) {
        const std::string_view name = blockTextureLayerName(layer);

        TextureImage image;
        bool         fromLoader = false;
        if (loader) {
            fromLoader = loader(layer, name, image);
            if (fromLoader && (image.width != size || image.height != size ||
                               image.pixels.size() !=
                                   static_cast<std::size_t>(size) * static_cast<std::size_t>(size) * 4u)) {
                VOXL_LOG_WARN("texture '{}' is {}x{}, expected {}x{}; using the procedural version",
                              name, image.width, image.height, size, size);
                fromLoader = false;
            }
        }
        if (!fromLoader) {
            image = generateBlockTexture(layer, size);
        } else {
            ++loadedFromDisk;
        }

        array.uploadLayer(layer, image.pixels.data(), image.width, image.height);
    }

    array.generateMipmaps();
    VOXL_LOG_INFO("block texture array ready: {} layer(s) ({} from disk, {} procedural), ~{} KiB",
                  kBlockTextureLayerCount, loadedFromDisk,
                  static_cast<std::size_t>(kBlockTextureLayerCount) - loadedFromDisk,
                  array.estimatedBytes() / 1024u);
    return array;
}

}  // namespace voxl
