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

/// Two scrolling noise fields crossed at different rates. The gradient is taken
/// by finite differences rather than an analytic derivative: at this amplitude
/// the difference is invisible and it keeps the noise function shared with the
/// sky.
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

    vec2 driftA = vec2( 0.045,  0.030) * t;
    vec2 driftB = vec2(-0.031,  0.052) * t;

    float centre = voxlNoise(p * 0.55 + driftA) + 0.5 * voxlNoise(p * 1.30 + driftB);
    float dx = voxlNoise((p + vec2(kEpsilon, 0.0)) * 0.55 + driftA) +
               0.5 * voxlNoise((p + vec2(kEpsilon, 0.0)) * 1.30 + driftB) - centre;
    float dz = voxlNoise((p + vec2(0.0, kEpsilon)) * 0.55 + driftA) +
               0.5 * voxlNoise((p + vec2(0.0, kEpsilon)) * 1.30 + driftB) - centre;

    // Small slope: water at this scale is nearly flat, and an aggressive normal
    // reads as crumpled foil.
    vec3 perturbed = normalize(vec3(-dx * 1.6, 1.0, -dz * 1.6));

    // Re-orient the tangent-space perturbation onto the actual face.
    if (faceNormal.y > 0.5) {
        return perturbed;
    }
    if (faceNormal.y < -0.5) {
        return vec3(perturbed.x, -perturbed.y, perturbed.z);
    }
    return normalize(faceNormal + vec3(perturbed.x, perturbed.z, perturbed.x) * 0.18);
}

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
        vec3 normal = waterNormal(vWorldPosition, faceNormal);

        // Fresnel: a water surface is nearly a mirror at grazing angles and
        // nearly clear head-on. This one term is what makes water read as water
        // rather than as blue glass.
        float fresnel = pow(1.0 - clamp(dot(normal, viewDir), 0.0, 1.0), 4.0);
        // Capped below 1: a physically exact Fresnel turns grazing water into a
        // pure sky mirror, and a lake seen from the shore then reads as a hole
        // in the terrain rather than as water.
        fresnel = mix(0.02, 0.85, fresnel);

        // Reflect the sky, gated by the surface's own skylight so underground
        // water does not mirror a sky it cannot see.
        vec3 reflected = voxlSkyColour(reflect(-viewDir, normal));
        colour = mix(colour, reflected, fresnel * 0.55 * max(vSunlight, 0.15));

        // Sharp sun glint. Blinn-Phong with a high exponent; the noise-perturbed
        // normal is what breaks it into moving sparkles.
        vec3  halfway  = normalize(viewDir + VOXL_SUN_DIR);
        float specular = pow(max(dot(normal, halfway), 0.0), 220.0);
        colour += VOXL_SUN_COLOUR * specular * 1.1 * vSunlight;

        // Edge-on water hides what is behind it; head-on water is see-through.
        alpha = clamp(alpha + fresnel * 0.22, 0.0, 1.0);
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
