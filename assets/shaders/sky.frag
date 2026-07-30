#version 450 core

// Sky dome: the shared gradient plus everything that lives IN the sky rather
// than in the air - the sun's bloom, the moon, and the stars.
//
// ===========================================================================
//  WHY THE GRADIENT IS NOT COMPUTED HERE
//
//  Every fogged fragment in the world resolves toward voxlSkyColour() sampled
//  along its OWN view ray (see chunk.frag and water.frag). If this shader drew
//  its own gradient, the two would agree only by luck and the horizon would
//  show a hard seam wherever they diverged - the single most likely visible
//  defect in a day/night cycle, and one that only appears at some times of day.
//  So the base colour here IS voxlSkyColour(), byte for byte.
//
//  Everything added on top must therefore be SPATIALLY TIGHT: a disc, a point,
//  or a halo that has fallen to nothing within a few degrees. A broad additive
//  term would be present in the sky and absent from the fog, which is the same
//  seam by another route. Nothing below exceeds a few degrees of support except
//  the star fade, which is multiplicative on terms that are already zero away
//  from a star.
// ===========================================================================

#include "common.glsl"

in vec3 vViewRay;

out vec4 oColour;

// ------------------------------------------------------------------ stars --

/// One lattice of stars.
///
/// The cell hash decides whether a cell holds a star at all and how bright it
/// is; a second hash places it INSIDE its cell. Without that offset the field
/// is a visible regular grid the moment the camera turns, which is what a plain
/// threshold-on-floor(dir * n) star field always looks like.
float starLayer(vec3 dir, float density, float threshold, float radius, out float tint)
{
    vec3  p    = dir * density;
    vec3  cell = floor(p);
    float h    = voxlHash13(cell);

    tint = 0.0;
    if (h < threshold) {
        return 0.0;
    }

    // Kept inside [0.25, 0.75] so a star is never clipped by its own cell wall.
    vec3 offset = 0.25 + 0.5 * vec3(voxlHash13(cell + 11.317),
                                    voxlHash13(cell + 27.719),
                                    voxlHash13(cell + 41.103));
    float d = length(p - cell - offset);

    // Brightness spread across the surviving hash range, cubed so that a few
    // stars are much brighter than the rest. A uniform field reads as noise.
    float rank      = (h - threshold) / max(1.0 - threshold, 1e-4);
    float magnitude = rank * rank * rank;

    // Per-star twinkle phase, so neighbours do not pulse in unison.
    float twinkle = 0.72 + 0.28 * sin(VOXL_TIME * 1.9 + rank * 63.0);

    tint = voxlHash13(cell + 7.531);
    // Ascending edges: smoothstep with edge0 > edge1 is undefined in GLSL even
    // though every driver happens to evaluate it the obvious way.
    return (1.0 - smoothstep(0.0, radius, d)) * (0.25 + magnitude) * twinkle;
}

vec3 starField(vec3 dir, float night)
{
    if (night < 0.01) {
        return vec3(0.0);
    }

    // Stars redden and dim into the horizon haze. The fade also keeps them out
    // of the band where terrain silhouettes sit, where a pinpoint highlight
    // straddling a mountain edge reads as a rendering artefact.
    float altitude = mix(0.15, 1.0, smoothstep(-0.02, 0.32, dir.y));

    // DENSITY IS NOT A FREE PARAMETER. A cell at density d subtends about 1/d
    // radians, and the star inside it is `radius` cells across; at 90 degrees of
    // field over 1600 pixels one pixel is 0.001 rad, so a star needs
    // radius/d > ~0.0015 to cover a pixel at all. Push the density up for "more
    // stars" and they all fall below a pixel and disappear into a faint haze
    // instead. More stars therefore means a LOWER threshold, not a finer grid.
    float tintA;
    float tintB;
    float a = starLayer(dir, 95.0, 0.9450, 0.22, tintA);
    float b = starLayer(dir, 220.0, 0.9720, 0.18, tintB);

    // Real stars are blue-white to amber. Two hues is enough for the eye to
    // stop reading the field as a single grey speckle.
    vec3 colourA = mix(vec3(0.70, 0.80, 1.00), vec3(1.00, 0.88, 0.72), tintA);
    vec3 colourB = mix(vec3(0.78, 0.86, 1.00), vec3(1.00, 0.92, 0.80), tintB);

    return (colourA * a * 2.20 + colourB * b * 0.95) * night * altitude;
}

// ------------------------------------------------------------------- moon --

/// Angular radius of the moon, as sin(angle). Larger than the real 0.26 degrees
/// on purpose: at a normal field of view a physically sized moon is four pixels
/// and reads as a dead pixel rather than as a moon.
const float kMoonRadius = 0.0230;

vec3 moon(vec3 dir, float night)
{
    // Exactly antipodal to the sun, so it needs no uniform of its own and can
    // never disagree with the direction the C++ side computed.
    vec3 moonDir = -VOXL_SUN_DIR;

    // Below the horizon the "sky" is the ground haze; a moon hanging in it
    // looks like it is behind the terrain rather than under it.
    float up = smoothstep(-0.07, 0.05, moonDir.y);
    if (up < 0.001) {
        return vec3(0.0);
    }

    // Not fully suppressed by day: a pale moon in a twilight sky is correct and
    // it is what stops moonrise from being a pop-in.
    float visibility = up * mix(0.16, 1.0, night);

    // Tangent frame on the moon's disc. The reference axis is swapped near the
    // poles because cross(up, moonDir) degenerates when they are parallel.
    vec3 reference = abs(moonDir.y) < 0.94 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 axisX     = normalize(cross(reference, moonDir));
    vec3 axisY     = cross(moonDir, axisX);

    vec2  disc = vec2(dot(dir, axisX), dot(dir, axisY)) / kMoonRadius;
    float r    = length(disc);

    // Broad glow, then a tight one. Both are narrow enough not to disagree
    // visibly with the fog; see the header note.
    float cosMoon = max(dot(dir, moonDir), 0.0);
    vec3  glow    = vec3(0.62, 0.70, 0.95) *
                (pow(cosMoon, 220.0) * 0.055 + pow(cosMoon, 1800.0) * 0.16);

    vec3 body = vec3(0.0);
    if (r < 1.05 && dot(dir, moonDir) > 0.0) {
        // Limb darkening. A flat white circle reads as a hole punched in the
        // sky; the falloff toward the edge is what makes it read as a sphere.
        float limb  = sqrt(max(1.0 - min(r * r, 1.0), 0.0));
        float shade = 0.52 + 0.48 * pow(limb, 0.45);

        // Maria. Low frequency and low contrast - the point is to break the
        // uniformity, not to model the Moon.
        float maria = voxlNoise(disc * 1.9 + 5.0) * 0.5 + voxlNoise(disc * 4.3) * 0.5;
        shade *= mix(0.74, 1.06, maria);

        // Peak brightness sits just under the point where the sRGB encode
        // clips. A physically bright full moon would be correct and would also
        // flatten the limb and the maria into one white circle, which is the
        // one thing this whole block exists to avoid.
        float coverage = 1.0 - smoothstep(0.94, 1.02, r);
        body = vec3(0.88, 0.90, 1.00) * shade * coverage * 0.95;
    }

    return (body + glow) * visibility;
}

// -------------------------------------------------------------------- sun --

/// Tight bloom around the disc that common.glsl already draws. Kept separate
/// from the shared halo because the shared one has to stay in the fog (distant
/// terrain toward the sun really is washed out) while this one must not - a
/// bloom baked into fog looks like a lens artefact welded to the geometry.
vec3 sunBloom(vec3 dir)
{
    float cosSun = max(dot(dir, VOXL_SUN_DIR), 0.0);

    // Fades out as the sun sinks, so the bloom does not survive as a bright
    // smear after the disc itself has set.
    float above = smoothstep(-0.10, 0.04, VOXL_SUN_DIR.y);

    float tight = pow(cosSun, 900.0) * 1.60;
    float soft  = pow(cosSun, 120.0) * 0.22;
    return VOXL_SUN_COLOUR * (tight + soft) * above;
}

// ------------------------------------------------------------------- main --

void main()
{
    vec3 dir = normalize(vViewRay);

    vec3 colour = voxlSkyColour(dir);
    colour += starField(dir, clamp(1.0 - VOXL_DAY, 0.0, 1.0));
    colour += moon(dir, clamp(1.0 - VOXL_DAY, 0.0, 1.0));
    colour += sunBloom(dir);

    // A smooth gradient across a 1600-pixel-wide window crosses far fewer than
    // 256 distinct 8-bit values, so without dithering the sky bands visibly.
    // It matters most at night, where the whole dome lives in the bottom two
    // percent of the range and the steps are only a few values apart.
    oColour = vec4(voxlEncodeOutput(voxlDither(colour, gl_FragCoord.xy)), 1.0);
}
