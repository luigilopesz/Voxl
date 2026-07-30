#version 450 core

// Fullscreen-triangle sky.
//
// Three vertices, no vertex buffer, no attributes - the positions come from
// gl_VertexID. A triangle rather than a quad: a quad's diagonal splits the screen
// into two triangles whose derivatives disagree along the seam, and it rasterises
// the same pixels twice at the edge.

#include "common.glsl"

out vec3 vViewRay;

void main()
{
    // IDs 0,1,2 map to (-1,-1), (3,-1), (-1,3): a triangle that covers the whole
    // clip volume with the excess falling outside it.
    vec2 ndc = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)) * 2.0 - 1.0;

    // Unproject both ends of the pixel's ray and take the difference. Only the
    // direction matters, so no normalisation is needed here - the fragment stage
    // normalises after interpolation, which is where it has to happen anyway.
    vec4 nearPoint = uInverseViewProjection * vec4(ndc, -1.0, 1.0);
    vec4 farPoint  = uInverseViewProjection * vec4(ndc,  1.0, 1.0);
    vViewRay = farPoint.xyz / farPoint.w - nearPoint.xyz / nearPoint.w;

    // z = 1 puts the sky on the far plane. The renderer draws it with depth
    // testing off, so this only matters if that ever changes.
    gl_Position = vec4(ndc, 1.0, 1.0);
}
