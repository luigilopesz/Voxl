#version 450 core

// Fragment stage for the sub-voxel damage pass.
//
// Deliberately identical to chunk.frag minus the alpha cutoff. That is only safe
// because World::editSubVoxel REJECTS carving any block whose render layer is not
// Opaque - leaves, glass and ice are all full cubes and valid raycast targets, so
// without that guard chipping one would re-emit it through this program and turn
// a see-through block solid. The guard and this shader have to change together.
//
// Sharing voxlShadeSurface and the fog and dither helpers is the point - a carved
// surface must land on exactly the shading of the face it replaced, or the damage
// reads as a lighting bug.

#include "common.glsl"
#include "chunk_common.glsl"

layout(binding = 0) uniform sampler2DArray uBlockTextures;

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

    vec3 colour = voxlShadeSurface(albedo.rgb, vDirection, vSunlight, vBlockLight, vAo);

    vec3  toEye        = vWorldPosition - VOXL_CAMERA;
    float viewDistance = length(toEye);
    vec3  sky          = voxlSkyColour(toEye / max(viewDistance, 1e-4));

    colour = mix(colour, sky, voxlFogFactor(viewDistance));

    oColour = vec4(voxlEncodeOutput(voxlDither(colour, gl_FragCoord.xy)), 1.0);
}
