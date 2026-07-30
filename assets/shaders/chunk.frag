#version 450 core

// Opaque and cutout chunk geometry.

#include "common.glsl"
#include "chunk_common.glsl"

// A 2D ARRAY, not an atlas: every block texture has its own wrap domain, so
// GL_REPEAT tiles a greedy quad correctly and mip levels never average across a
// tile border. See src/render/Texture.hpp for the full argument.
layout(binding = 0) uniform sampler2DArray uBlockTextures;

/// 0 for the opaque pass, ~0.35 for the cutout pass. Kept as a uniform so the
/// two passes share one program instead of forcing a shader permutation.
uniform float uAlphaCutoff;

in vec3  vWorldPosition;
in vec2  vTexCoord;
in float vSunlight;
in float vBlockLight;
in float vAo;
flat in uint vTextureLayer;
flat in uint vDirection;

out vec4 oColour;

void main()
{
    vec4 albedo = texture(uBlockTextures, vec3(vTexCoord, float(vTextureLayer)));

    // Cutout foliage. The threshold is below 0.5 because minification averages a
    // leaf texture's alpha downward, and a 0.5 test makes distant canopies
    // evaporate.
    if (albedo.a < uAlphaCutoff) {
        discard;
    }

    vec3 colour = voxlShadeSurface(albedo.rgb, vDirection, vSunlight, vBlockLight, vAo);

    // Fog toward the sky along this fragment's own view ray, so terrain melts
    // into whatever the sky is doing directly behind it.
    vec3  toEye        = vWorldPosition - VOXL_CAMERA;
    float viewDistance = length(toEye);
    vec3  sky          = voxlSkyColour(toEye / max(viewDistance, 1e-4));

    colour = mix(colour, sky, voxlFogFactor(viewDistance));

    oColour = vec4(voxlEncodeOutput(voxlDither(colour, gl_FragCoord.xy)), 1.0);
}
