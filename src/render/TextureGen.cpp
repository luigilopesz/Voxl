#include "render/TextureGen.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace voxl {
namespace {

constexpr float kTwoPi = 6.28318530717958647692f;

// --------------------------------------------------------------- colour --

/// Linear-ish working colour. Painting in floats keeps every mix and shade
/// expression readable; the single conversion to bytes happens in `Canvas::take`.
/// The values are sRGB-encoded (they are authored as pixel-art colours), which is
/// why the GPU texture uses GL_SRGB8_ALPHA8 and decodes them on sample.
struct Rgba {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

[[nodiscard]] constexpr float clamp01(float value) noexcept
{
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}

[[nodiscard]] constexpr Rgba rgb(int red, int green, int blue, int alpha = 255) noexcept
{
    return Rgba{static_cast<float>(red) / 255.0f, static_cast<float>(green) / 255.0f,
                static_cast<float>(blue) / 255.0f, static_cast<float>(alpha) / 255.0f};
}

[[nodiscard]] constexpr Rgba mix(const Rgba& from, const Rgba& to, float t) noexcept
{
    const float k = clamp01(t);
    return Rgba{from.r + (to.r - from.r) * k, from.g + (to.g - from.g) * k,
                from.b + (to.b - from.b) * k, from.a + (to.a - from.a) * k};
}

/// Multiplies RGB while preserving alpha; used for grain and shading.
[[nodiscard]] constexpr Rgba shade(const Rgba& colour, float factor) noexcept
{
    return Rgba{clamp01(colour.r * factor), clamp01(colour.g * factor),
                clamp01(colour.b * factor), colour.a};
}

[[nodiscard]] std::uint8_t toByte(float value) noexcept
{
    // +0.5 then truncate: std::lround would drag in a libm call per channel for
    // no benefit, and the input is already clamped.
    return static_cast<std::uint8_t>(clamp01(value) * 255.0f + 0.5f);
}

// ---------------------------------------------------------------- noise --

/// Integer avalanche (the "lowbias32" mixer). Chosen over a linear congruential
/// step because neighbouring pixel coordinates must decorrelate completely -
/// visible diagonal banding is the classic symptom of a weak hash here.
[[nodiscard]] constexpr std::uint32_t mixBits(std::uint32_t x) noexcept
{
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

[[nodiscard]] constexpr std::uint32_t hash2(std::int32_t x, std::int32_t y,
                                            std::uint32_t seed) noexcept
{
    const std::uint32_t ux = static_cast<std::uint32_t>(x) * 0x27D4EB2Du;
    const std::uint32_t uy = static_cast<std::uint32_t>(y) * 0x165667B1u;
    return mixBits(ux ^ uy ^ (seed * 0x9E3779B9u));
}

/// Hash to [0, 1). Uses the high bits, which are the best mixed.
[[nodiscard]] constexpr float unitFloat(std::uint32_t h) noexcept
{
    return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
}

[[nodiscard]] constexpr float random01(std::int32_t x, std::int32_t y, std::uint32_t seed) noexcept
{
    return unitFloat(hash2(x, y, seed));
}

[[nodiscard]] constexpr float smoothstep(float t) noexcept
{
    return t * t * (3.0f - 2.0f * t);
}

/// Positive modulo, so a lattice index of -1 wraps to period-1 instead of
/// mirroring. This is what makes the noise tileable.
[[nodiscard]] constexpr std::int32_t wrap(std::int32_t value, std::int32_t period) noexcept
{
    const std::int32_t m = value % period;
    return m < 0 ? m + period : m;
}

/// Tileable value noise. `x`/`y` are in lattice units; the lattice wraps every
/// `period` units, so sampling x over [0, period) covers exactly one seamless
/// tile. Seamlessness is not optional: greedy meshing merges coplanar faces into
/// quads many blocks wide and the texture is sampled with GL_REPEAT across them,
/// so any discontinuity would show up as a grid of visible seams.
[[nodiscard]] float valueNoise(float x, float y, std::int32_t period, std::uint32_t seed) noexcept
{
    const float fx = std::floor(x);
    const float fy = std::floor(y);
    const std::int32_t ix = static_cast<std::int32_t>(fx);
    const std::int32_t iy = static_cast<std::int32_t>(fy);
    const float        tx = smoothstep(x - fx);
    const float        ty = smoothstep(y - fy);

    const std::int32_t x0 = wrap(ix, period);
    const std::int32_t x1 = wrap(ix + 1, period);
    const std::int32_t y0 = wrap(iy, period);
    const std::int32_t y1 = wrap(iy + 1, period);

    const float n00 = random01(x0, y0, seed);
    const float n10 = random01(x1, y0, seed);
    const float n01 = random01(x0, y1, seed);
    const float n11 = random01(x1, y1, seed);

    const float bottom = n00 + (n10 - n00) * tx;
    const float top    = n01 + (n11 - n01) * tx;
    return bottom + (top - bottom) * ty;
}

/// Fractal sum of tileable value noise, normalised to [0, 1]. `basePeriod` must
/// divide the texture size for every octave to stay seamless, which is why the
/// callers use powers of two.
[[nodiscard]] float fbm(float u, float v, std::int32_t basePeriod, int octaves,
                        std::uint32_t seed) noexcept
{
    float sum       = 0.0f;
    float amplitude = 1.0f;
    float total     = 0.0f;
    std::int32_t period = basePeriod;
    for (int octave = 0; octave < octaves; ++octave) {
        const float scale = static_cast<float>(period);
        sum += valueNoise(u * scale, v * scale, period, seed + static_cast<std::uint32_t>(octave) * 7919u) *
               amplitude;
        total += amplitude;
        amplitude *= 0.5f;
        period *= 2;
    }
    return total > 0.0f ? sum / total : 0.0f;
}

struct WorleyResult {
    float         nearest    = 0.0f;  ///< distance to the closest feature point, in cell units
    float         secondary  = 0.0f;  ///< distance to the second closest
    std::uint32_t cell       = 0;     ///< hash of the owning cell, for per-stone variation
};

/// Tileable Worley (cellular) noise. Feature points are jittered inside a wrapped
/// integer grid, which gives the pebble and cobble shapes their irregularity
/// while keeping the pattern seamless across the tile boundary.
[[nodiscard]] WorleyResult worley(float u, float v, std::int32_t cells, std::uint32_t seed,
                                  float jitter = 0.85f) noexcept
{
    const float        scale = static_cast<float>(cells);
    const float        x     = u * scale;
    const float        y     = v * scale;
    const std::int32_t baseX = static_cast<std::int32_t>(std::floor(x));
    const std::int32_t baseY = static_cast<std::int32_t>(std::floor(y));

    WorleyResult result;
    result.nearest   = 1e9f;
    result.secondary = 1e9f;

    for (std::int32_t dy = -1; dy <= 1; ++dy) {
        for (std::int32_t dx = -1; dx <= 1; ++dx) {
            const std::int32_t cellX  = baseX + dx;
            const std::int32_t cellY  = baseY + dy;
            const std::int32_t wrapX  = wrap(cellX, cells);
            const std::int32_t wrapY  = wrap(cellY, cells);
            const std::uint32_t h     = hash2(wrapX, wrapY, seed);
            const float         offsetX = 0.5f + (unitFloat(h) - 0.5f) * jitter;
            const float         offsetY = 0.5f + (unitFloat(mixBits(h)) - 0.5f) * jitter;

            const float px = static_cast<float>(cellX) + offsetX;
            const float py = static_cast<float>(cellY) + offsetY;
            const float dxf = px - x;
            const float dyf = py - y;
            const float distance = std::sqrt(dxf * dxf + dyf * dyf);

            if (distance < result.nearest) {
                result.secondary = result.nearest;
                result.nearest   = distance;
                result.cell      = h;
            } else if (distance < result.secondary) {
                result.secondary = distance;
            }
        }
    }
    return result;
}

// --------------------------------------------------------------- canvas --

/// Float-precision paint target. y == 0 is the bottom row (GL convention).
class Canvas {
public:
    explicit Canvas(int size) : m_size(size), m_pixels(static_cast<std::size_t>(size) * size) {}

    [[nodiscard]] int size() const noexcept { return m_size; }

    [[nodiscard]] Rgba& at(int x, int y) noexcept
    {
        return m_pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(m_size) +
                        static_cast<std::size_t>(x)];
    }
    [[nodiscard]] const Rgba& at(int x, int y) const noexcept
    {
        return m_pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(m_size) +
                        static_cast<std::size_t>(x)];
    }

    /// Wrapped read, for effects that need to look at a neighbour across the
    /// seam (leaf hole outlining).
    [[nodiscard]] const Rgba& atWrapped(int x, int y) const noexcept
    {
        return at(wrap(x, m_size), wrap(y, m_size));
    }

    void fill(const Rgba& colour) noexcept
    {
        std::fill(m_pixels.begin(), m_pixels.end(), colour);
    }

    /// Normalised texel centre coordinates in [0, 1).
    [[nodiscard]] float u(int x) const noexcept
    {
        return (static_cast<float>(x) + 0.5f) / static_cast<float>(m_size);
    }
    [[nodiscard]] float v(int y) const noexcept
    {
        return (static_cast<float>(y) + 0.5f) / static_cast<float>(m_size);
    }

    [[nodiscard]] TextureImage take() const
    {
        TextureImage image;
        image.width  = m_size;
        image.height = m_size;
        image.pixels.resize(m_pixels.size() * 4);
        for (std::size_t i = 0; i < m_pixels.size(); ++i) {
            const Rgba& c = m_pixels[i];
            image.pixels[i * 4 + 0] = toByte(c.r);
            image.pixels[i * 4 + 1] = toByte(c.g);
            image.pixels[i * 4 + 2] = toByte(c.b);
            image.pixels[i * 4 + 3] = toByte(c.a);
        }
        return image;
    }

private:
    int               m_size;
    std::vector<Rgba> m_pixels;
};

/// Per-texel salt-and-pepper grain. Amount is the peak brightness swing.
void addGrain(Canvas& canvas, float amount, std::uint32_t seed)
{
    for (int y = 0; y < canvas.size(); ++y) {
        for (int x = 0; x < canvas.size(); ++x) {
            const float n = random01(x, y, seed);
            canvas.at(x, y) = shade(canvas.at(x, y), 1.0f + (n - 0.5f) * 2.0f * amount);
        }
    }
}

/// Sparse individual texels, used for sand grit, snow sparkle and stone flecks.
void addSpeckles(Canvas& canvas, float probability, const Rgba& colour, float strength,
                 std::uint32_t seed)
{
    for (int y = 0; y < canvas.size(); ++y) {
        for (int x = 0; x < canvas.size(); ++x) {
            if (random01(x, y, seed) < probability) {
                const float amount = strength * (0.55f + 0.45f * random01(x, y, seed ^ 0x5BF03635u));
                canvas.at(x, y)    = mix(canvas.at(x, y), colour, amount);
            }
        }
    }
}

// -------------------------------------------------------------- painters --

void paintStone(Canvas& canvas)
{
    constexpr Rgba kBase  = rgb(126, 127, 133);
    constexpr Rgba kDark  = rgb(94, 95, 101);
    constexpr Rgba kLight = rgb(156, 157, 163);

    for (int y = 0; y < canvas.size(); ++y) {
        for (int x = 0; x < canvas.size(); ++x) {
            const float u = canvas.u(x);
            const float v = canvas.v(y);
            // Two scales: a broad mottle that reads as bedding, and a fine one
            // that reads as crystalline grain.
            const float broad = fbm(u, v, 4, 2, 1301u);
            const float fine  = fbm(u, v, 8, 3, 1302u);
            const float t     = clamp01(broad * 0.55f + fine * 0.45f);
            canvas.at(x, y)   = mix(kDark, kLight, smoothstep(t));
        }
    }
    addGrain(canvas, 0.05f, 1303u);
    addSpeckles(canvas, 0.05f, kDark, 0.55f, 1304u);
    addSpeckles(canvas, 0.04f, kBase, 0.5f, 1305u);
}

void paintDirt(Canvas& canvas)
{
    constexpr Rgba kDark   = rgb(88, 62, 40);
    constexpr Rgba kLight  = rgb(140, 102, 68);
    constexpr Rgba kPebble = rgb(112, 96, 80);

    for (int y = 0; y < canvas.size(); ++y) {
        for (int x = 0; x < canvas.size(); ++x) {
            const float u = canvas.u(x);
            const float v = canvas.v(y);
            const float t = clamp01(fbm(u, v, 4, 2, 2101u) * 0.5f + fbm(u, v, 8, 3, 2102u) * 0.5f);
            Rgba        colour = mix(kDark, kLight, smoothstep(t));

            // Small stones embedded in the soil: the nearest-feature distance
            // gives round blobs, and only the innermost part is recoloured so the
            // edges stay soft.
            const WorleyResult cell = worley(u, v, 6, 2103u);
            if (cell.nearest < 0.22f) {
                colour = mix(colour, kPebble, 0.65f * (1.0f - cell.nearest / 0.22f));
            }
            canvas.at(x, y) = colour;
        }
    }
    addGrain(canvas, 0.07f, 2104u);
    addSpeckles(canvas, 0.03f, kDark, 0.7f, 2105u);
}

void paintGrassTop(Canvas& canvas)
{
    constexpr Rgba kDeep  = rgb(66, 118, 46);
    constexpr Rgba kMid   = rgb(96, 152, 62);
    constexpr Rgba kFresh = rgb(126, 178, 78);

    for (int y = 0; y < canvas.size(); ++y) {
        for (int x = 0; x < canvas.size(); ++x) {
            const float u = canvas.u(x);
            const float v = canvas.v(y);
            const float t = clamp01(fbm(u, v, 4, 3, 3101u));
            Rgba colour = t < 0.5f ? mix(kDeep, kMid, t * 2.0f) : mix(kMid, kFresh, (t - 0.5f) * 2.0f);

            // Blades: two-texel-tall marks, so the surface has direction rather
            // than looking like undifferentiated noise.
            const float blade = random01(x, y / 2, 3102u);
            if (blade > 0.90f) {
                colour = mix(colour, kFresh, 0.75f);
            } else if (blade < 0.10f) {
                colour = mix(colour, kDeep, 0.7f);
            }
            canvas.at(x, y) = colour;
        }
    }
    addGrain(canvas, 0.045f, 3103u);
}

void paintGrassSide(Canvas& canvas)
{
    paintDirt(canvas);

    constexpr Rgba kDeep  = rgb(60, 110, 42);
    constexpr Rgba kMid   = rgb(94, 150, 60);
    constexpr Rgba kFresh = rgb(122, 174, 76);

    const int size = canvas.size();
    // Grass thickness varies per column so the soil line is ragged. The noise is
    // tileable in x, which keeps the line continuous across the block seam.
    const int minDepth = std::max(2, size / 10);
    const int range    = std::max(2, size / 6);

    for (int x = 0; x < size; ++x) {
        const float u          = canvas.u(x);
        const float columnNoise = valueNoise(u * 8.0f, 0.5f, 8, 3201u);
        const int   depth      = minDepth + static_cast<int>(columnNoise * static_cast<float>(range));
        // Occasional single blade hanging one texel lower than the main line.
        const int   extra = random01(x, 0, 3202u) > 0.72f ? 1 : 0;

        for (int y = size - depth - extra; y < size; ++y) {
            if (y < 0) {
                continue;
            }
            const float v      = canvas.v(y);
            const float t      = clamp01(fbm(u, v, 8, 2, 3203u));
            const bool  isEdge = y < size - depth;  // the hanging blade texel
            Rgba        colour = mix(kDeep, kFresh, t);
            if (isEdge) {
                colour = mix(colour, kDeep, 0.6f);
            } else if (y == size - 1) {
                // Top texel catches the light, which sells the overhang.
                colour = mix(colour, kFresh, 0.35f);
            } else {
                colour = mix(colour, kMid, 0.25f);
            }
            canvas.at(x, y) = colour;
        }
    }
    addGrain(canvas, 0.04f, 3204u);
}

void paintSand(Canvas& canvas)
{
    constexpr Rgba kDark  = rgb(196, 178, 126);
    constexpr Rgba kLight = rgb(228, 214, 166);

    for (int y = 0; y < canvas.size(); ++y) {
        for (int x = 0; x < canvas.size(); ++x) {
            const float u = canvas.u(x);
            const float v = canvas.v(y);
            // Ripples: a low-frequency sine along a diagonal, warped by noise.
            // Integer wave counts keep it seamless.
            const float warp   = fbm(u, v, 4, 2, 4101u);
            const float ripple = std::sin((u * 2.0f + v * 1.0f) * kTwoPi + warp * 2.2f) * 0.5f + 0.5f;
            const float t      = clamp01(fbm(u, v, 8, 3, 4102u) * 0.7f + ripple * 0.3f);
            canvas.at(x, y)    = mix(kDark, kLight, smoothstep(t));
        }
    }
    addGrain(canvas, 0.035f, 4103u);
    addSpeckles(canvas, 0.02f, rgb(160, 142, 96), 0.5f, 4104u);
}

void paintGravel(Canvas& canvas)
{
    constexpr Rgba kMortar = rgb(74, 70, 66);
    constexpr Rgba kCool   = rgb(112, 110, 108);
    constexpr Rgba kWarm   = rgb(150, 140, 126);

    for (int y = 0; y < canvas.size(); ++y) {
        for (int x = 0; x < canvas.size(); ++x) {
            const float        u    = canvas.u(x);
            const float        v    = canvas.v(y);
            const WorleyResult cell = worley(u, v, 7, 5101u);

            // Per-stone tint from the cell hash, then a dome shade from the
            // distance to the centre so each pebble looks rounded.
            const float tint  = unitFloat(mixBits(cell.cell ^ 0x1000193u));
            Rgba        stone = mix(kCool, kWarm, tint);
            stone             = shade(stone, 1.12f - cell.nearest * 0.45f);

            // Gap between the two nearest features is the classic cell-border
            // measure; it darkens the crevices without a hard outline.
            const float border = cell.secondary - cell.nearest;
            if (border < 0.16f) {
                stone = mix(kMortar, stone, clamp01(border / 0.16f));
            }
            canvas.at(x, y) = stone;
        }
    }
    addGrain(canvas, 0.06f, 5102u);
}

void paintWater(Canvas& canvas)
{
    constexpr Rgba kDeep  = rgb(28, 78, 152);
    constexpr Rgba kMid   = rgb(46, 116, 196);
    constexpr Rgba kCrest = rgb(150, 206, 236);

    for (int y = 0; y < canvas.size(); ++y) {
        for (int x = 0; x < canvas.size(); ++x) {
            const float u = canvas.u(x);
            const float v = canvas.v(y);
            const float warp = fbm(u, v, 4, 2, 6101u);
            // Two crossed waves with integer periods; the phase warp keeps them
            // from looking like corrugated iron.
            const float wave = std::sin(v * kTwoPi * 2.0f + warp * 2.5f) * 0.5f +
                               std::sin((u + v * 0.5f) * kTwoPi * 1.0f - warp * 1.5f) * 0.5f;
            const float t = clamp01(0.5f + wave * 0.35f);
            Rgba colour   = mix(kDeep, kMid, smoothstep(t));
            if (t > 0.82f) {
                colour = mix(colour, kCrest, (t - 0.82f) / 0.18f * 0.5f);
            }
            // Translucent, but not so thin that a lake stops reading as a
            // surface from above. The water shader adds fresnel on top.
            colour.a = 0.78f;
            canvas.at(x, y) = colour;
        }
    }
    addGrain(canvas, 0.03f, 6102u);
}

void paintLogTop(Canvas& canvas)
{
    constexpr Rgba kHeart = rgb(158, 118, 74);
    constexpr Rgba kRing  = rgb(120, 86, 52);
    constexpr Rgba kBark  = rgb(84, 62, 42);

    for (int y = 0; y < canvas.size(); ++y) {
        for (int x = 0; x < canvas.size(); ++x) {
            const float u  = canvas.u(x) - 0.5f;
            const float v  = canvas.v(y) - 0.5f;
            const float r  = std::sqrt(u * u + v * v) * 2.0f;  // 0 at centre, 1 at the edge midpoint
            const float warp = fbm(canvas.u(x), canvas.v(y), 4, 2, 7101u);

            // Growth rings: a sine in radius, wobbled so they are not perfect
            // circles. Five rings across a block reads as wood, not as a target.
            const float rings = std::sin(r * kTwoPi * 4.5f + warp * 1.8f) * 0.5f + 0.5f;
            Rgba        colour = mix(kRing, kHeart, smoothstep(rings));

            if (r > 0.92f) {
                colour = mix(colour, kBark, clamp01((r - 0.92f) / 0.18f));
            }
            // A single radial split, which is what a cut log actually looks like.
            const float angle = std::atan2(v, u);
            if (std::abs(angle - 1.1f) < 0.055f && r < 0.85f) {
                colour = mix(colour, kBark, 0.7f);
            }
            canvas.at(x, y) = colour;
        }
    }
    addGrain(canvas, 0.05f, 7102u);
}

void paintLogSide(Canvas& canvas)
{
    constexpr Rgba kDark  = rgb(72, 52, 34);
    constexpr Rgba kLight = rgb(132, 100, 64);
    constexpr Rgba kKnot  = rgb(58, 42, 28);

    const int size = canvas.size();
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float u = canvas.u(x);
            const float v = canvas.v(y);
            // Bark is dominated by vertical strands: high frequency across x,
            // very low along y.
            const float strand = valueNoise(u * 16.0f, v * 2.0f, 16, 8101u);
            const float grain  = valueNoise(u * 8.0f, v * 8.0f, 8, 8102u);
            const float t      = clamp01(strand * 0.75f + grain * 0.25f);
            Rgba        colour = mix(kDark, kLight, smoothstep(t));

            // Two knots, placed deterministically away from the tile edges so
            // they are not cut in half by the seam.
            const WorleyResult cell = worley(u, v, 3, 8103u, 0.4f);
            if (cell.nearest < 0.18f && unitFloat(cell.cell) > 0.62f) {
                const float k = 1.0f - cell.nearest / 0.18f;
                colour        = mix(colour, kKnot, k * 0.85f);
                if (cell.nearest < 0.06f) {
                    colour = mix(colour, kLight, 0.35f);  // lighter core
                }
            }
            canvas.at(x, y) = colour;
        }
    }
    addGrain(canvas, 0.05f, 8104u);
}

void paintLeaves(Canvas& canvas)
{
    constexpr Rgba kShadow = rgb(38, 74, 32);
    constexpr Rgba kLeaf   = rgb(66, 122, 48);
    constexpr Rgba kBright = rgb(102, 158, 62);

    const int size = canvas.size();
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float u = canvas.u(x);
            const float v = canvas.v(y);
            // Clumps of foliage from cellular noise, tinted per clump.
            const WorleyResult clump = worley(u, v, 5, 9101u);
            const float        tint  = unitFloat(mixBits(clump.cell));
            const float detail = fbm(u, v, 8, 3, 9102u);
            const float t      = clamp01(detail * 0.6f + (1.0f - clump.nearest) * 0.4f);

            Rgba colour = mix(kShadow, kLeaf, smoothstep(t));
            colour      = mix(colour, kBright, tint * 0.45f * t);
            canvas.at(x, y) = colour;
        }
    }
    addGrain(canvas, 0.07f, 9103u);

    // Punch holes. RGB is deliberately LEFT INTACT under the transparent texels:
    // glGenerateMipmap averages all four channels, so zeroing the colour of a
    // hole would bleed black fringes into the canopy at distance.
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float gap = fbm(canvas.u(x), canvas.v(y), 8, 2, 9104u);
            if (gap < 0.36f) {
                canvas.at(x, y).a = 0.0f;
            }
        }
    }
    // Darken the texels bordering a hole so the canopy has depth instead of
    // looking like a flat sheet with punctures.
    Canvas source = canvas;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (source.at(x, y).a == 0.0f) {
                continue;
            }
            const bool nextToHole = source.atWrapped(x + 1, y).a == 0.0f ||
                                    source.atWrapped(x - 1, y).a == 0.0f ||
                                    source.atWrapped(x, y + 1).a == 0.0f ||
                                    source.atWrapped(x, y - 1).a == 0.0f;
            if (nextToHole) {
                canvas.at(x, y) = shade(canvas.at(x, y), 0.72f);
            }
        }
    }
}

void paintPlanks(Canvas& canvas)
{
    constexpr Rgba kGap   = rgb(88, 62, 36);
    constexpr Rgba kDark  = rgb(150, 112, 68);
    constexpr Rgba kLight = rgb(196, 158, 104);

    const int size        = canvas.size();
    const int plankHeight = std::max(4, size / 4);

    for (int y = 0; y < size; ++y) {
        const int   plank      = y / plankHeight;
        const bool  isGapRow   = (y % plankHeight) == 0;
        const float plankTint  = random01(0, plank, 10101u);
        // Butt joint position varies per plank; wrapped so it tiles.
        const int   jointX     = static_cast<int>(random01(1, plank, 10102u) *
                                              static_cast<float>(size)) % size;

        for (int x = 0; x < size; ++x) {
            const float u = canvas.u(x);
            const float v = canvas.v(y);
            // Grain runs along the plank: stretched in x, tight in y.
            const float grain = valueNoise(u * 4.0f, v * 32.0f, 32, 10103u);
            float       t     = clamp01(grain * 0.55f + plankTint * 0.45f);
            Rgba        colour = mix(kDark, kLight, smoothstep(t));

            // A couple of darker grain lines per plank.
            if (valueNoise(u * 8.0f, v * 16.0f, 16, 10104u) < 0.22f) {
                colour = mix(colour, kDark, 0.5f);
            }
            if (isGapRow) {
                colour = mix(kGap, colour, 0.25f);
            } else if (x == jointX) {
                colour = mix(kGap, colour, 0.4f);
            }
            canvas.at(x, y) = colour;
        }
    }
    addGrain(canvas, 0.03f, 10105u);
}

void paintGlass(Canvas& canvas)
{
    constexpr Rgba kPane  = rgb(206, 232, 240, 18);
    constexpr Rgba kFrame = rgb(196, 222, 232, 255);
    constexpr Rgba kGleam = rgb(240, 252, 255, 96);

    const int size = canvas.size();
    canvas.fill(kPane);

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const bool onEdge = x == 0 || y == 0 || x == size - 1 || y == size - 1;
            // A short inner tick at each corner, which is what makes the frame
            // read as a leaded pane rather than as a plain rectangle.
            const int  inner  = std::max(2, size / 8);
            const bool corner = (x < inner && (y == 1 || y == size - 2)) ||
                                (x > size - 1 - inner && (y == 1 || y == size - 2)) ||
                                (y < inner && (x == 1 || x == size - 2)) ||
                                (y > size - 1 - inner && (x == 1 || x == size - 2));
            if (onEdge) {
                canvas.at(x, y) = kFrame;
            } else if (corner) {
                canvas.at(x, y) = mix(kPane, kFrame, 0.75f);
            } else {
                // Single diagonal highlight streak across the pane.
                const float diagonal = canvas.u(x) - canvas.v(y);
                if (std::abs(diagonal - 0.22f) < 0.035f) {
                    canvas.at(x, y) = kGleam;
                }
            }
        }
    }
}

void paintSnow(Canvas& canvas)
{
    constexpr Rgba kShade = rgb(206, 218, 238);
    constexpr Rgba kBase  = rgb(240, 245, 252);

    for (int y = 0; y < canvas.size(); ++y) {
        for (int x = 0; x < canvas.size(); ++x) {
            const float t = clamp01(fbm(canvas.u(x), canvas.v(y), 4, 3, 11101u));
            // Snow is nearly uniform; the dips are what stop it looking like a
            // flat white quad under a flat sky.
            canvas.at(x, y) = mix(kShade, kBase, smoothstep(0.35f + t * 0.65f));
        }
    }
    addGrain(canvas, 0.02f, 11102u);
    addSpeckles(canvas, 0.015f, rgb(255, 255, 255), 1.0f, 11103u);
}

void paintSandstoneTop(Canvas& canvas)
{
    // Warmer and more saturated than sand on purpose: the two sit next to each
    // other in every desert and a shared palette makes them indistinguishable.
    constexpr Rgba kDark  = rgb(196, 154, 96);
    constexpr Rgba kLight = rgb(234, 198, 138);

    for (int y = 0; y < canvas.size(); ++y) {
        for (int x = 0; x < canvas.size(); ++x) {
            const float t = clamp01(fbm(canvas.u(x), canvas.v(y), 4, 3, 12101u));
            canvas.at(x, y) = mix(kDark, kLight, smoothstep(t));
        }
    }
    addGrain(canvas, 0.03f, 12102u);
}

void paintSandstoneSide(Canvas& canvas)
{
    constexpr Rgba kDark  = rgb(178, 136, 82);
    constexpr Rgba kLight = rgb(238, 204, 146);
    constexpr Rgba kSeam  = rgb(140, 104, 62);

    const int size = canvas.size();
    for (int y = 0; y < size; ++y) {
        const float v = canvas.v(y);
        // Sedimentary bands: noise that varies only along y, so the beds stay
        // horizontal and continuous across the tile seam. Quantising it into
        // discrete steps is what makes them read as strata rather than as a
        // vertical gradient.
        const float raw  = valueNoise(0.5f, v * 8.0f, 8, 13101u);
        const float band = std::floor(raw * 5.0f) / 4.0f;
        for (int x = 0; x < size; ++x) {
            const float u = canvas.u(x);
            const float t = clamp01(band * 0.82f + valueNoise(u * 8.0f, v * 8.0f, 8, 13102u) * 0.18f);
            Rgba        colour = mix(kDark, kLight, smoothstep(clamp01(t)));
            // Thin dark parting between beds.
            if (raw < 0.13f) {
                colour = mix(colour, kSeam, 0.75f);
            }
            canvas.at(x, y) = colour;
        }
    }
    addGrain(canvas, 0.03f, 13103u);
}

void paintSandstoneBottom(Canvas& canvas)
{
    constexpr Rgba kDark  = rgb(180, 140, 88);
    constexpr Rgba kLight = rgb(224, 190, 134);

    for (int y = 0; y < canvas.size(); ++y) {
        for (int x = 0; x < canvas.size(); ++x) {
            const float u = canvas.u(x);
            const float v = canvas.v(y);
            const WorleyResult cell = worley(u, v, 8, 14101u);
            const float t = clamp01(fbm(u, v, 8, 2, 14102u) * 0.6f + (1.0f - cell.nearest) * 0.4f);
            canvas.at(x, y) = mix(kDark, kLight, smoothstep(t));
        }
    }
    addGrain(canvas, 0.04f, 14103u);
}

void paintCobblestone(Canvas& canvas)
{
    constexpr Rgba kMortar = rgb(62, 60, 62);
    constexpr Rgba kCool   = rgb(112, 112, 118);
    constexpr Rgba kWarm   = rgb(162, 160, 162);

    for (int y = 0; y < canvas.size(); ++y) {
        for (int x = 0; x < canvas.size(); ++x) {
            const float        u    = canvas.u(x);
            const float        v    = canvas.v(y);
            const WorleyResult cell = worley(u, v, 4, 15101u, 0.9f);

            const float tint  = unitFloat(mixBits(cell.cell ^ 0x2545F491u));
            Rgba        stone = mix(kCool, kWarm, tint);
            // Dome shading per stone plus a per-stone grain, so no two cobbles
            // look stamped from the same die.
            stone = shade(stone, 1.18f - cell.nearest * 0.6f);
            stone = shade(stone, 1.0f + (fbm(u, v, 8, 2, 15102u) - 0.5f) * 0.16f);

            const float border = cell.secondary - cell.nearest;
            if (border < 0.13f) {
                stone = mix(kMortar, stone, clamp01(border / 0.13f));
            }
            canvas.at(x, y) = stone;
        }
    }
    addGrain(canvas, 0.05f, 15103u);
}

void paintBedrock(Canvas& canvas)
{
    constexpr Rgba kBlack = rgb(26, 26, 30);
    constexpr Rgba kGrey  = rgb(104, 104, 112);

    for (int y = 0; y < canvas.size(); ++y) {
        for (int x = 0; x < canvas.size(); ++x) {
            const float u = canvas.u(x);
            const float v = canvas.v(y);
            // Quantising into four steps gives the chaotic, blocky look that
            // reads as "do not bother mining this".
            const float raw   = clamp01(fbm(u, v, 8, 3, 16101u) * 0.7f + random01(x, y, 16102u) * 0.3f);
            const float steps = std::floor(raw * 4.0f) / 3.0f;
            canvas.at(x, y)   = mix(kBlack, kGrey, clamp01(steps));
        }
    }
    addSpeckles(canvas, 0.10f, kBlack, 0.9f, 16103u);
}

void paintGlowstone(Canvas& canvas)
{
    constexpr Rgba kMatrix = rgb(132, 92, 44);
    constexpr Rgba kGlow   = rgb(248, 206, 108);
    constexpr Rgba kCore   = rgb(255, 248, 210);

    for (int y = 0; y < canvas.size(); ++y) {
        for (int x = 0; x < canvas.size(); ++x) {
            const float        u    = canvas.u(x);
            const float        v    = canvas.v(y);
            const WorleyResult cell = worley(u, v, 5, 17101u);

            Rgba colour = mix(kMatrix, kGlow, clamp01(fbm(u, v, 8, 2, 17102u) * 0.45f));
            // Nodules: a steep falloff so each one has a hot centre. The block's
            // emission is handled by the lighting system; this only has to look
            // like the source of it.
            if (cell.nearest < 0.34f) {
                const float k = 1.0f - cell.nearest / 0.34f;
                colour        = mix(colour, kGlow, k);
                colour        = mix(colour, kCore, k * k * k * 0.9f);
            }
            canvas.at(x, y) = colour;
        }
    }
    addGrain(canvas, 0.04f, 17103u);
}

void paintClay(Canvas& canvas)
{
    constexpr Rgba kDark  = rgb(148, 144, 162);
    constexpr Rgba kLight = rgb(182, 178, 194);

    for (int y = 0; y < canvas.size(); ++y) {
        for (int x = 0; x < canvas.size(); ++x) {
            // Clay is the smoothest material in the set; low-frequency noise only
            // and no speckle, so it contrasts with stone and gravel next to it.
            const float t = clamp01(fbm(canvas.u(x), canvas.v(y), 4, 2, 18101u));
            canvas.at(x, y) = mix(kDark, kLight, smoothstep(t));
        }
    }
    addGrain(canvas, 0.02f, 18102u);
}

void paintIce(Canvas& canvas)
{
    constexpr Rgba kDeep  = rgb(122, 174, 212, 150);
    constexpr Rgba kPale  = rgb(176, 216, 238, 150);
    constexpr Rgba kCrack = rgb(228, 244, 252, 210);

    for (int y = 0; y < canvas.size(); ++y) {
        for (int x = 0; x < canvas.size(); ++x) {
            const float u = canvas.u(x);
            const float v = canvas.v(y);
            const float t = clamp01(fbm(u, v, 4, 3, 19101u));
            Rgba        colour = mix(kDeep, kPale, smoothstep(t));

            // Fracture web: cell borders make continuous, branching cracks for
            // free, which hand-placed lines never manage to do seamlessly.
            const WorleyResult cell   = worley(u, v, 4, 19102u);
            const float        border = cell.secondary - cell.nearest;
            if (border < 0.09f) {
                colour = mix(colour, kCrack, 1.0f - border / 0.09f);
            }
            canvas.at(x, y) = colour;
        }
    }
    addGrain(canvas, 0.02f, 19103u);
}

/// Unmistakable "this layer index is wrong" pattern.
void paintMissing(Canvas& canvas)
{
    const int cell = std::max(1, canvas.size() / 4);
    for (int y = 0; y < canvas.size(); ++y) {
        for (int x = 0; x < canvas.size(); ++x) {
            const bool checker = ((x / cell) + (y / cell)) % 2 == 0;
            canvas.at(x, y)    = checker ? rgb(255, 0, 220) : rgb(20, 20, 20);
        }
    }
}

constexpr std::array<std::string_view, kBlockTextureLayerCount> kLayerNames = {
    "stone",           "dirt",              "grass_top",   "grass_side", "sand",
    "gravel",          "water",             "log_top",     "log_side",   "leaves",
    "planks",          "glass",             "snow",        "sandstone_top",
    "sandstone_side",  "sandstone_bottom",  "cobblestone", "bedrock",    "glowstone",
    "clay",            "ice",
};

}  // namespace

std::string_view blockTextureLayerName(int layer) noexcept
{
    if (layer < 0 || layer >= kBlockTextureLayerCount) {
        return {};
    }
    return kLayerNames[static_cast<std::size_t>(layer)];
}

TextureImage generateBlockTexture(int layer, int size)
{
    Canvas canvas(size > 0 ? size : kBlockTextureSize);

    switch (layer) {
        case 0:  paintStone(canvas); break;
        case 1:  paintDirt(canvas); break;
        case 2:  paintGrassTop(canvas); break;
        case 3:  paintGrassSide(canvas); break;
        case 4:  paintSand(canvas); break;
        case 5:  paintGravel(canvas); break;
        case 6:  paintWater(canvas); break;
        case 7:  paintLogTop(canvas); break;
        case 8:  paintLogSide(canvas); break;
        case 9:  paintLeaves(canvas); break;
        case 10: paintPlanks(canvas); break;
        case 11: paintGlass(canvas); break;
        case 12: paintSnow(canvas); break;
        case 13: paintSandstoneTop(canvas); break;
        case 14: paintSandstoneSide(canvas); break;
        case 15: paintSandstoneBottom(canvas); break;
        case 16: paintCobblestone(canvas); break;
        case 17: paintBedrock(canvas); break;
        case 18: paintGlowstone(canvas); break;
        case 19: paintClay(canvas); break;
        case 20: paintIce(canvas); break;
        default: paintMissing(canvas); break;
    }
    return canvas.take();
}

}  // namespace voxl
