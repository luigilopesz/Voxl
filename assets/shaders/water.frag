#version 450 core

// Translucent chunk geometry: water, glass and ice.
//
// One program covers the whole translucent pass and branches on the texture
// layer. The alternative - a separate draw list for water - would mean splitting
// each chunk's translucent geometry into two buffers and sorting them
// independently, which costs more than the branch ever will.
//
// The water surface is NOT displaced geometrically. Moving only the top faces
// would tear them away from the side faces of the same water column and open a
// visible crack at every shoreline, so the animation lives entirely in the
// normal and the specular response.
//
// The pass runs with depth writes DISABLED (Renderer::applyTranslucentState).
// Nothing here may assume otherwise: water must not occlude the water behind
// it, and the alpha this shader produces is deliberately capped below 1 before
// fog so that a lake never becomes a solid lid.

#include "common.glsl"
#include "chunk_common.glsl"

layout(binding = 0) uniform sampler2DArray uBlockTextures;

/// Texture array layer of water. Frozen as 6 by world/Block.cpp, passed in rather
/// than hard-coded so the layer table stays the single source of truth.
uniform uint uWaterLayer;

in vec3  vWorldPosition;
in vec2  vTexCoord;
in float vSunlight;
in float vBlockLight;
in float vAo;
flat in uint vTextureLayer;
flat in uint vDirection;

out vec4 oColour;

// ------------------------------------------------------------ water optics --

/// Absorption per block of water, linear space. Red is removed roughly seven
/// times faster than blue, which is the whole reason deep water is blue rather
/// than simply darker - a single scalar extinction gives grey water.
const vec3 kWaterExtinction = vec3(0.46, 0.11, 0.06);

/// Tint of light scattered back out of the body of the water, i.e. what deep
/// water looks like when nothing behind it survives the path.
const vec3 kWaterScatterTint = vec3(0.06, 0.42, 0.55);

/// Nominal thickness of a body of water, in blocks. See `opticalPath()`.
const float kWaterBodyDepth = 1.6;

// ---------------------------------------------------------- surface motion --

/// Height field of the surface. Three drifting octaves plus one long swell.
///
/// The octaves drift in different directions on purpose: advected as a single
/// sheet they read as a scrolling texture, and the eye picks that up instantly.
/// The swell is a plain sine because it only has to supply a scale larger than
/// the ripples, and noise at that wavelength costs the same and reads worse.
float waterHeight(vec2 p, float t)
{
    float h = voxlNoise(p * 0.55 + vec2(0.045, 0.030) * t);
    h += 0.50 * voxlNoise(p * 1.30 + vec2(-0.031, 0.052) * t);
    h += 0.25 * voxlNoise(p * 2.90 + vec2(0.070, -0.041) * t);
    h += 0.09 * sin(dot(p, vec2(0.21, 0.13)) + t * 0.55);
    return h;
}

/// Perturbed surface normal. The gradient is taken by finite differences rather
/// than analytically: at this amplitude the difference is invisible and it keeps
/// the noise function shared with the sky.
vec3 waterNormal(vec3 worldPosition, vec3 faceNormal)
{
    vec2 p = worldPosition.xz;
    if (abs(faceNormal.y) < 0.5) {
        // Vertical water face: ripple along the face rather than in world XZ,
        // otherwise the waves run into the wall instead of along it.
        p = vec2(dot(worldPosition.xz, faceNormal.zx), worldPosition.y);
    }

    float t = VOXL_TIME;
    const float kEpsilon = 0.12;

    float centre = waterHeight(p, t);
    float dx = waterHeight(p + vec2(kEpsilon, 0.0), t) - centre;
    float dz = waterHeight(p + vec2(0.0, kEpsilon), t) - centre;

    // Small slope: water at this scale is nearly flat, and an aggressive normal
    // reads as crumpled foil.
    vec3 perturbed = normalize(vec3(-dx * 1.9, 1.0, -dz * 1.9));

    // Re-orient the tangent-space perturbation onto the actual face.
    if (faceNormal.y > 0.5) {
        return perturbed;
    }
    if (faceNormal.y < -0.5) {
        return vec3(perturbed.x, -perturbed.y, perturbed.z);
    }
    return normalize(faceNormal + vec3(perturbed.x, perturbed.z, perturbed.x) * 0.18);
}

/// Length of water the view ray travels through, in blocks.
///
/// THIS IS AN APPROXIMATION AND IT IS DELIBERATE. There is no depth prepass, so
/// the distance from this fragment to whatever is behind it is genuinely not
/// known here. Two proxies stand in, and between them they cover the two cases
/// the eye actually notices:
///
///   SLANT  how obliquely the ray enters the surface. This is the exact cosine
///          factor of the real path length and it is the dominant term: looking
///          straight down you see the bottom through a short column, and the
///          same water seen across a lake is opaque. Without it water is
///          uniformly transparent and reads as tinted glass.
///   SHORE  the mesher's ambient occlusion on a water face is reduced only
///          where solid blocks stand above the surface - which is exactly a
///          bank, a channel or a shoreline. Shallow edges therefore lighten,
///          which is the other half of what makes water read as having a bed.
///
/// If a linear depth buffer ever becomes available to this pass, replace the
/// whole function with the real path length; nothing else here changes.
float opticalPath(vec3 faceNormal, vec3 viewDir, float ao)
{
    // The floor caps the slant at about 9x, which is where the exponential has
    // already saturated - past that it only costs precision.
    float slant = 1.0 / max(abs(dot(faceNormal, viewDir)), 0.11);
    float shore = mix(0.30, 1.0, ao * ao);
    return kWaterBodyDepth * slant * shore;
}

// ------------------------------------------------------------------- main --

void main()
{
    vec4 albedo = texture(uBlockTextures, vec3(vTexCoord, float(vTextureLayer)));

    vec3  faceNormal   = kVoxlNormals[vDirection];
    vec3  toEye        = VOXL_CAMERA - vWorldPosition;
    float viewDistance = length(toEye);
    vec3  viewDir      = toEye / max(viewDistance, 1e-4);

    vec3  colour = voxlShadeSurface(albedo.rgb, vDirection, vSunlight, vBlockLight, vAo);
    float alpha  = albedo.a;

    if (vTextureLayer == uWaterLayer) {
        // Procedural noise has no mip chain, so once a ripple is smaller than a
        // pixel the normal decorrelates between neighbouring fragments and the
        // whole far field crawls with speckle - and the high-exponent glint
        // turns that speckle into fireflies. Flattening the surface back toward
        // the face with distance is the fix; it also happens to be what water
        // looks like from far away.
        float detail = 1.0 - smoothstep(24.0, 140.0, viewDistance);

        vec3 normal = normalize(mix(faceNormal, waterNormal(vWorldPosition, faceNormal), detail));

        // ---------------------------------------------------- absorption --
        float path          = opticalPath(faceNormal, viewDir, vAo);
        vec3  transmittance = exp(-kWaterExtinction * path);

        // Light scattered back out of the body. Driven by the frame's own
        // ambient and sun so that deep water follows the day/night cycle
        // instead of staying the same swimming-pool teal at midnight.
        vec3 inScatter = kWaterScatterTint *
                         (VOXL_AMBIENT * 0.95 +
                          VOXL_SUN_COLOUR * 0.35 * vSunlight * max(VOXL_SUN_DIR.y, 0.0));

        colour = colour * transmittance + inScatter * (1.0 - transmittance);

        // ------------------------------------------------------ fresnel --
        // A water surface is nearly a mirror at grazing angles and nearly clear
        // head-on. This one term is what makes water read as water rather than
        // as blue glass, and it is what brightens the far edge of every lake.
        float fresnel = pow(1.0 - clamp(dot(normal, viewDir), 0.0, 1.0), 4.0);
        // Capped below 1: a physically exact Fresnel turns grazing water into a
        // pure sky mirror, and a lake seen from the shore then reads as a hole
        // in the terrain rather than as water.
        fresnel = mix(0.02, 0.85, fresnel);

        // Reflect the sky, gated by the surface's own skylight so underground
        // water does not mirror a sky it cannot see.
        vec3 reflected = voxlSkyColour(reflect(-viewDir, normal));
        colour = mix(colour, reflected, fresnel * 0.55 * max(vSunlight, 0.15));

        // -------------------------------------------------------- glint --
        // Two lobes. The tight one is the sparkle the perturbed normal breaks
        // into moving points; the broad one is the sheen that tells you the
        // surface is wet even where no individual sparkle lands.
        float sunUp    = smoothstep(-0.06, 0.06, VOXL_SUN_DIR.y);
        vec3  halfway  = normalize(viewDir + VOXL_SUN_DIR);
        float ndoth    = max(dot(normal, halfway), 0.0);
        float sparkle  = pow(ndoth, 220.0) * 1.30 * detail;
        float sheen    = pow(ndoth, 26.0) * 0.10;
        colour += VOXL_SUN_COLOUR * (sparkle + sheen) * vSunlight * sunUp;

        // -------------------------------------------------------- alpha --
        // Deep water hides more of what is behind it. Capped well below 1: the
        // pass does not write depth precisely so that layered water composites,
        // and fully opaque water would defeat that and read as a painted lid.
        float opacity   = 1.0 - dot(transmittance, vec3(0.2126, 0.7152, 0.0722));
        float baseAlpha = clamp(albedo.a, 0.35, 0.82);
        alpha = clamp(mix(baseAlpha, 0.90, opacity * 0.85) + fresnel * 0.16, 0.0, 0.92);
    } else {
        // Glass and ice get a much weaker rim so their silhouettes stay legible
        // against a bright sky without looking wet.
        float rim = pow(1.0 - clamp(dot(faceNormal, viewDir), 0.0, 1.0), 5.0);
        colour += voxlSkyColour(faceNormal) * rim * 0.25 * max(vSunlight, 0.2);
        alpha = clamp(alpha + rim * 0.25, 0.0, 1.0);
    }

    float fog = voxlFogFactor(viewDistance);
    colour    = mix(colour, voxlSkyColour(-viewDir), fog);
    // Fully fogged geometry must also become fully opaque, or the sky shows
    // through the fog and distant water reads as a hole in the horizon.
    alpha = mix(alpha, 1.0, fog);

    oColour = vec4(voxlEncodeOutput(voxlDither(colour, gl_FragCoord.xy)), alpha);
}
