#version 450 core

// Vertex stage for the translucent chunk pass (water, glass, ice).
//
// Identical to chunk.vert by design: the translucent pass reads the same packed
// vertex stream, and the water look is produced entirely in water.frag. See the
// comment there for why the surface is not displaced in the vertex stage.

#include "common.glsl"
#include "chunk_common.glsl"
#include "chunk_vertex.glsl"
