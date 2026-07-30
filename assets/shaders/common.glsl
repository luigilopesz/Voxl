// Shared declarations for every Voxl program.
//
// The uniform block below MIRRORS `voxl::FrameUniforms` in src/render/Renderer.hpp.
// It is std140 and the C++ side static_asserts every offset; if you add a field,
// add it in both places and keep it vec4-aligned.
//
// Colours in this block are LINEAR. The block texture array is GL_SRGB8_ALPHA8 so
// sampling linearises it for free, all lighting happens in linear space, and the
// encode back to sRGB happens once in voxlEncodeOutput().

layout(std140, binding = 0) uniform FrameUniforms {
    mat4 uViewProjection;
    mat4 uInverseViewProjection;
    vec4 uCameraPositionTime;   // xyz = eye in world space, w = seconds since start
    vec4 uSunDirectionDay;      // xyz = unit vector TOWARDS the sun, w = day factor 0..1
    vec4 uSunColourIntensity;   // rgb = linear colour, a = multiplier
    vec4 uSkyZenith;            // rgb, a unused
    vec4 uSkyHorizon;           // rgb, a unused
    vec4 uAmbientAo;            // rgb = ambient sky bounce, a = AO strength 0..1
    vec4 uBlockLightGain;       // rgb = torch colour, a = multiplier
    vec4 uFogParams;            // x = start, y = end, z = curve, w = 1 to gamma-encode manually
};

#define VOXL_CAMERA       uCameraPositionTime.xyz
#define VOXL_TIME         uCameraPositionTime.w
#define VOXL_SUN_DIR      uSunDirectionDay.xyz
#define VOXL_DAY          uSunDirectionDay.w
#define VOXL_SUN_COLOUR   (uSunColourIntensity.rgb * uSunColourIntensity.a)
#define VOXL_AMBIENT      uAmbientAo.rgb
#define VOXL_AO_STRENGTH  uAmbientAo.a
#define VOXL_BLOCK_LIGHT  (uBlockLightGain.rgb * uBlockLightGain.a)
#define VOXL_FOG_START    uFogParams.x
#define VOXL_FOG_END      uFogParams.y
#define VOXL_FOG_CURVE    uFogParams.z
#define VOXL_APPLY_GAMMA  uFogParams.w

// ------------------------------------------------------------------ noise --

float voxlHash12(vec2 p)
{
    vec3 q = fract(vec3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}

float voxlHash13(vec3 p)
{
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

/// Smooth value noise. Used for the water surface only, so quality matters less
/// than cost: two of these are evaluated per water fragment.
float voxlNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = voxlHash12(i);
    float b = voxlHash12(i + vec2(1.0, 0.0));
    float c = voxlHash12(i + vec2(0.0, 1.0));
    float d = voxlHash12(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// -------------------------------------------------------------------- sky --

/// Colour of the sky in a given world-space direction.
///
/// Chunk fragments fog toward THIS function evaluated along their own view ray,
/// not toward a single flat fog colour. That is what makes distant terrain
/// dissolve into the sky instead of into a grey band that is visibly the wrong
/// shade wherever the gradient is not average.
///
/// INVARIANT - THIS FUNCTION IS THE FOG COLOUR, SO IT MUST STAY LOW-FREQUENCY.
/// It is called by chunk.frag, subvoxel.frag and water.frag as the colour their
/// geometry resolves into, and by sky.frag as the sky's own base. Anything added
/// here is therefore painted onto SOLID GEOMETRY as well as onto the dome. Only
/// terms that vary smoothly with direction may live here - the gradient, the
/// ground haze, the scattering halo and the sun disc, all of which fogged
/// geometry genuinely must converge to. A term driven by a hash, by a lattice or
/// by VOXL_TIME must not: it has no meaning on a mountainside. Decoration of
/// that kind belongs in sky.frag, which is the only shader that draws the dome
/// and nothing else.
vec3 voxlSkyColour(vec3 dir)
{
    float up = clamp(dir.y, -1.0, 1.0);

    // pow() rather than a linear ramp: the real sky spends most of the dome near
    // its zenith colour and compresses the gradient into the last few degrees.
    vec3 sky = mix(uSkyHorizon.rgb, uSkyZenith.rgb, pow(clamp(up, 0.0, 1.0), 0.42));

    // Below the horizon the world is terrain, but fog still has to blend into
    // something. A graded haze rather than a flat slab: a constant colour under
    // the horizon reads as a wall whenever the player looks down a slope.
    vec3 ground = mix(uSkyHorizon.rgb * 0.42, uSkyHorizon.rgb * 0.18,
                      clamp(-up * 1.6, 0.0, 1.0));
    sky = mix(ground, sky, smoothstep(-0.14, 0.02, up));

    float cosSun = dot(dir, VOXL_SUN_DIR);

    // Wide forward scattering halo, tightened near the horizon so a low sun
    // spills warm light along it.
    float halo = pow(max(cosSun, 0.0), 6.0) * 0.22;
    float horizonSpill = pow(max(cosSun, 0.0), 3.0) * exp(-abs(up) * 7.0) * 0.30;
    sky += VOXL_SUN_COLOUR * (halo + horizonSpill);

    // The disc itself: about half a degree, with a soft edge so it does not
    // alias into a flickering dot when the camera turns.
    float disc = smoothstep(0.99965, 0.99992, cosSun);
    sky += VOXL_SUN_COLOUR * disc * 12.0;

    // NO STARS HERE, AND NOTHING ELSE HIGH-FREQUENCY EITHER. See the invariant
    // note above this function: sky.frag owns the star field exclusively.
    //
    // A star field used to live at this point. It was wrong twice over. It was
    // drawn a second time by sky.frag's starField(), and - far worse - because
    // chunk.frag, subvoxel.frag and water.frag all resolve their fog toward THIS
    // function, every one of those stars was also painted onto solid geometry and
    // onto water at whatever distance the fog had saturated. It was additionally
    // a lattice-cell threshold with no sub-cell placement, so each "star" was a
    // hard-edged axis-aligned rectangle the size of a whole cell's projected
    // footprint rather than a point.
    //
    // Only the sun disc and its halo stay in the shared function, and they belong
    // here: they are what fully fogged geometry must converge to for the horizon
    // to have no seam. Stars do not have that property, because starField()
    // already fades them out over exactly the low-altitude band where fogged
    // terrain sits.
    return sky;
}

// -------------------------------------------------------------- fog/gamma --

/// 0 at the fog start distance, 1 at the end. The curve exponent keeps the
/// mid-range crisp: a linear ramp washes out terrain that is still clearly in
/// view distance.
float voxlFogFactor(float distanceToCamera)
{
    float span = max(VOXL_FOG_END - VOXL_FOG_START, 1e-3);
    float t = clamp((distanceToCamera - VOXL_FOG_START) / span, 0.0, 1.0);
    return pow(t, VOXL_FOG_CURVE);
}

/// Final write. The hardware performs the sRGB encode when the default
/// framebuffer is sRGB capable and GL_FRAMEBUFFER_SRGB is enabled; uFogParams.w
/// is only set when the driver did not give us one and we must do it ourselves.
vec3 voxlEncodeOutput(vec3 linearColour)
{
    vec3 c = max(linearColour, vec3(0.0));
    return VOXL_APPLY_GAMMA > 0.5 ? pow(c, vec3(1.0 / 2.2)) : c;
}

/// Sub-LSB noise that breaks up the banding a smooth sky gradient shows on an
/// 8-bit framebuffer. Applied before the encode so it survives it.
vec3 voxlDither(vec3 colour, vec2 fragCoord)
{
    return colour + (voxlHash12(fragCoord) - 0.5) * (1.0 / 255.0);
}
