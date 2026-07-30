#version 450 core

// Vertex stage for the sub-voxel damage pass.
//
// THIS FILE MIRRORS src/mesh/SubVoxelMesh.hpp. The bit layout below is the one
// documented there; if the two disagree the carved geometry is garbage in a way
// that looks like a mesher bug. Change both in the same commit and bump
// kSubVoxelFormatVersion.
//
// It is a separate program from chunk.vert rather than a branch inside it because
// the vertex format differs: 9-bit positions in sub-voxel units instead of 6-bit
// positions in blocks. Everything downstream of the unpack - lighting, fog,
// dithering, output encoding - is the shared code the block path uses, so a
// carved surface is shaded identically to the face it replaced.

#include "common.glsl"
#include "chunk_common.glsl"

layout(location = 0) in uint aData0;
layout(location = 1) in uint aData1;

/// World position of the chunk's (0,0,0) corner, exactly as for the block pass.
uniform vec3 uChunkOrigin;

out vec3  vWorldPosition;
out vec2  vTexCoord;
out float vSunlight;
out float vBlockLight;
out float vAo;
flat out uint vTextureLayer;
flat out uint vDirection;

void main()
{
    // Positions are in sub-voxel units (0..256); scale back into block space so
    // the chunk origin uniform means the same thing it does for block geometry.
    vec3 position = vec3(float( aData0        & 511u),
                         float((aData0 >>  9) & 511u),
                         float((aData0 >> 18) & 511u)) * (1.0 / 8.0);

    uint  direction  = (aData0 >> 27) & 7u;
    uint  textureLayer = aData1 & 4095u;
    float sunlight   = float((aData1 >> 20) & 15u) / 15.0;
    float blockLight = float((aData1 >> 24) & 15u) / 15.0;
    float ao         = float((aData1 >> 28) &  3u) /  3.0;

    // TEXTURE COORDINATES COME FROM THE VERTEX'S OWN BLOCK-SPACE POSITION.
    //
    // voxlUnpackVertex builds uv as (quad extent) x (corner selector), i.e. zero
    // at the quad's origin corner. That is correct for BLOCK geometry only
    // because a block quad always starts on a block boundary, so "zero at the
    // origin corner" and "the fractional block coordinate" are the same number.
    //
    // A sub-voxel quad does not start on a block boundary. It can start at any
    // eighth of a block, and the width/height fields cap at 8 sub-voxels, so the
    // same formula makes EVERY carved quad sample the strip of texture from the
    // origin outward - a one-sub-voxel-tall quad gets the top 1/8 of the texture
    // smeared along its whole length. Carved surfaces then render as 1-D bands
    // that neither match nor line up with the intact faces beside them, which is
    // the defect this replaces (see docs/VISUAL_REVIEW.md).
    //
    // Taking u and v straight from the position along the frame's own tangent
    // axes fixes the phase exactly: it reduces to the block path's value at a
    // block boundary, GL_REPEAT still gives one repeat per block, and the
    // derivatives - hence mip selection - are unchanged.
    //
    // The (u, v) axis pairs below mirror kFaceFrames in SubVoxelMesher.cpp.
    // NOTE FOR THE OWNER OF src/mesh/SubVoxelMesh.hpp: that header's "GLSL side"
    // comment still shows the old size-times-corner derivation and needs this
    // paragraph instead. No BIT of the format moved; width, height and corner are
    // simply no longer read here.
    const ivec2 kUvAxes[6] = ivec2[6](ivec2(2, 1),   // NegX: (z, y)
                                      ivec2(1, 2),   // PosX: (y, z)
                                      ivec2(0, 2),   // NegY: (x, z)
                                      ivec2(2, 0),   // PosY: (z, x)
                                      ivec2(1, 0),   // NegZ: (y, x)
                                      ivec2(0, 1));  // PosZ: (x, y)

    ivec2 axes = kUvAxes[direction];
    vec2  uv   = vec2(position[axes.x], position[axes.y]);

    // Same correction as voxlUnpackVertex: the mesher's tangent frame is chosen
    // for winding, which leaves texture V running along world Z / world X on two
    // of the four vertical faces. Swapping there keeps V along world +Y on all
    // four, so directional side textures are not rotated 90 degrees relative to
    // the block geometry they are carved out of.
    if (direction == 1u || direction == 4u) {   // PosX, NegZ
        uv = uv.yx;
    }

    vec3 world = uChunkOrigin + position;

    vWorldPosition = world;
    vTexCoord      = uv;
    vSunlight      = sunlight;
    vBlockLight    = blockLight;
    vAo            = ao;
    vTextureLayer  = textureLayer;
    vDirection     = direction;

    gl_Position = uViewProjection * vec4(world, 1.0);
}
