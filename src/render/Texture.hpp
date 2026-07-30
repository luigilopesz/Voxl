#pragma once

// GL_TEXTURE_2D_ARRAY wrapper, and the block texture array the chunk shader
// samples.
//
// WHY AN ARRAY AND NOT AN ATLAS: an atlas packs every block texture into one
// image, so a mip level averages texels across tile borders and bleeds
// neighbouring materials into each other - the classic "grass has a stone fringe
// at distance" artefact. Worse, tiling a greedy-meshed quad over an atlas needs
// manual UV wrapping in the shader, which breaks the derivatives the hardware
// uses to pick a mip level. A 2D array gives every block its own wrap domain, so
// GL_REPEAT and automatic mip selection both just work.
//
// Thread safety: NONE. GL objects, main thread only. Note that the *images* are
// produced by render/TextureGen.hpp, which is pure CPU code and thread safe.

#include "render/TextureGen.hpp"

#include <glad/gl.h>

#include <functional>
#include <string_view>

namespace voxl {

/// Immutable-storage 2D texture array.
///
/// Storage is allocated once with glTextureStorage3D: the size, format and mip
/// count are fixed for the object's lifetime. That is the point - a mutable
/// texture can be silently reallocated into an incomplete state by a stray
/// glTexImage call, and an incomplete texture samples as black.
class TextureArray {
public:
    TextureArray() noexcept = default;
    ~TextureArray() { destroy(); }

    TextureArray(const TextureArray&)            = delete;
    TextureArray& operator=(const TextureArray&) = delete;
    TextureArray(TextureArray&& other) noexcept;
    TextureArray& operator=(TextureArray&& other) noexcept;

    /// Allocates storage. `mipLevels == 0` means "the full chain down to 1x1".
    /// Returns false if the request exceeds GL_MAX_ARRAY_TEXTURE_LAYERS.
    bool create(int width, int height, int layers, GLenum internalFormat, int mipLevels = 0);

    /// Uploads RGBA8 texels into one layer's mip 0. Dimensions must match the
    /// storage exactly.
    void uploadLayer(int layer, const std::uint8_t* rgba, int width, int height);

    /// Builds the mip chain from mip 0 of every layer. Call once after all layers
    /// are uploaded; doing it per layer costs the same work N times.
    void generateMipmaps();

    void bindUnit(GLuint unit) const;
    void destroy() noexcept;

    [[nodiscard]] GLuint id() const noexcept { return m_id; }
    [[nodiscard]] int    width() const noexcept { return m_width; }
    [[nodiscard]] int    height() const noexcept { return m_height; }
    [[nodiscard]] int    layers() const noexcept { return m_layers; }
    [[nodiscard]] int    mipLevels() const noexcept { return m_mipLevels; }
    /// Anisotropy actually applied, 1.0 when the extension is unavailable.
    [[nodiscard]] float  anisotropy() const noexcept { return m_anisotropy; }
    /// Bytes of mip 0 across all layers, times 4/3 for the mip tail. Approximate;
    /// the driver's real footprint is not queryable.
    [[nodiscard]] std::size_t estimatedBytes() const noexcept;

    [[nodiscard]] explicit operator bool() const noexcept { return m_id != 0; }

    /// Number of mip levels in a complete chain for a square texture.
    [[nodiscard]] static int completeMipLevels(int size) noexcept;

private:
    /// Voxel look: GL_NEAREST magnification so a close-up face shows crisp
    /// texels, GL_NEAREST_MIPMAP_LINEAR minification so distant terrain fades
    /// between mip levels instead of shimmering. Anisotropy recovers the detail
    /// that trilinear minification throws away on ground planes seen at a grazing
    /// angle, which is most of the screen in a first-person game.
    void applySampling();

    GLuint m_id         = 0;
    int    m_width      = 0;
    int    m_height     = 0;
    int    m_layers     = 0;
    int    m_mipLevels  = 0;
    float  m_anisotropy = 1.0f;
};

/// Hook that lets real art override a procedural layer.
///
/// Returning true means `out` was filled with an image of the expected size;
/// returning false falls back to render/TextureGen.hpp. This is the entire seam a
/// future PNG loader needs: implement it with stb_image and pass it in. Keeping
/// the decode out of this file is what stops Texture.cpp from owning
/// STB_IMAGE_IMPLEMENTATION, which must live in exactly one translation unit.
using BlockTextureLoader = std::function<bool(int layer, std::string_view name, TextureImage& out)>;

/// Builds the block texture array in the layer order frozen by world/Block.cpp.
///
/// `resolution` must match whatever the loader produces; the procedural fallback
/// scales to any power of two. On failure the returned array is falsy and the
/// caller must not draw with it.
[[nodiscard]] TextureArray createBlockTextureArray(int resolution = kBlockTextureSize,
                                                   const BlockTextureLoader& loader = {});

}  // namespace voxl
