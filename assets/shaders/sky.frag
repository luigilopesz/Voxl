#version 450 core

// Gradient sky dome with a sun disc, driven entirely by the frame uniform block
// so a day/night cycle only has to animate uSunDirectionDay, uSunColourIntensity
// and the two sky colours - no shader change.

#include "common.glsl"

in vec3 vViewRay;

out vec4 oColour;

void main()
{
    vec3 colour = voxlSkyColour(normalize(vViewRay));

    // A smooth gradient across a 1600-pixel-wide window crosses far fewer than
    // 256 distinct 8-bit values, so without dithering the sky bands visibly.
    oColour = vec4(voxlEncodeOutput(voxlDither(colour, gl_FragCoord.xy)), 1.0);
}
