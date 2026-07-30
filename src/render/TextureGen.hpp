#pragma once

// Procedurally generated block textures.
//
// There are no painted art assets yet, and a renderer that cannot show a
// convincing world is impossible to evaluate. So every block texture is
// synthesised here from deterministic hash noise: original pixel art, generated
// the same way on every machine and in every run.
//
// This file deliberately has NO OpenGL dependency. It produces plain CPU images
// so that replacing it with real PNGs later is a change to Texture.cpp only:
// see `BlockTextureLoader` in render/Texture.hpp for the seam.
//
// Determinism: every painter is driven by a fixed per-layer seed and integer hash
// noise. No rand(), no floating-point reduction that depends on evaluation order.
//
// Thread safety: pure functions, no shared state. Safe to call from any thread
// (and cheap enough that it is not worth doing so).

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace voxl {

/// Authoring resolution. 32x32 is twice the classic voxel-game resolution: still
/// unmistakably pixel art, but with room for the grain and pebble detail that
/// makes a surface read as a material rather than as a flat colour.
inline constexpr int kBlockTextureSize = 32;

/// Must equal `TexLayerCount` in world/Block.cpp. The layer order is a frozen
/// contract (docs/TECHNICAL_DESIGN.md section 7) and is append-only.
inline constexpr int kBlockTextureLayerCount = 21;

/// A CPU-side RGBA8 image.
///
/// Row 0 is the BOTTOM row, matching OpenGL's texture coordinate origin, so a
/// painter that writes "the top few rows" writes the highest y values.
struct TextureImage {
    int                       width  = 0;
    int                       height = 0;
    std::vector<std::uint8_t> pixels;  ///< RGBA8, tightly packed, 4 bytes per texel

    [[nodiscard]] bool        empty() const noexcept { return pixels.empty(); }
    [[nodiscard]] std::size_t byteSize() const noexcept { return pixels.size(); }
};

/// Name of the layer, matching the file name a real asset would use
/// (`assets/textures/blocks/<name>.png`). Empty for an out-of-range layer.
[[nodiscard]] std::string_view blockTextureLayerName(int layer) noexcept;

/// Paints one texture-array layer. `size` must be a positive power of two; the
/// painters scale with it, so 16 or 64 also produce sensible results. An
/// out-of-range layer returns a magenta/black checker so a bad layer index is
/// immediately obvious in-game instead of silently sampling stone.
[[nodiscard]] TextureImage generateBlockTexture(int layer, int size = kBlockTextureSize);

}  // namespace voxl
