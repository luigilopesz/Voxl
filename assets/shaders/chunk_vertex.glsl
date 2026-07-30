// The vertex stage body shared by chunk.vert and water.vert.
//
// Both chunk programs consume the identical packed vertex stream and produce the
// identical varyings; only the fragment stage differs. Keeping one copy of the
// unpack-and-transform means a change to the vertex format cannot be applied to
// one program and forgotten in the other.

// Both attributes are INTEGER attributes (glVertexArrayAttribIFormat). Declaring
// them as float inputs would let the driver convert the bit patterns, and every
// unpacked field would silently be wrong.
layout(location = 0) in uint aData0;
layout(location = 1) in uint aData1;

/// World position of the chunk's (0,0,0) corner. Vertex positions are 6-bit
/// chunk-local integers, so this uniform is what places the geometry in the
/// world. Set once per draw by ChunkRenderer::drawLayer.
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
    VoxlVertex vertex = voxlUnpackVertex(aData0, aData1);

    vec3 world = uChunkOrigin + vertex.position;

    vWorldPosition = world;
    vTexCoord      = vertex.uv;
    vSunlight      = vertex.sunlight;
    vBlockLight    = vertex.blockLight;
    vAo            = vertex.ao;
    vTextureLayer  = vertex.textureLayer;
    vDirection     = vertex.direction;

    gl_Position = uViewProjection * vec4(world, 1.0);
}
