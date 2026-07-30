// Packed-vertex decode and surface shading, shared by the chunk and water
// programs.
//
// ===========================================================================
//  THIS FILE MIRRORS THE BIT LAYOUT IN src/mesh/MeshData.hpp (kVertexFormatVersion 1).
//  If the two ever disagree the world renders as garbage that looks exactly like
//  a mesher bug. Change both in the same commit.
//
//   data0: posX 0..5 | posY 6..11 | posZ 12..17 | direction 18..20
//          sunlight 21..24 | blockLight 25..28 | ao 29..30 | reserved 31
//   data1: textureLayer 0..11 | width-1 12..16 | height-1 17..21
//          corner 22..23 | reserved 24..31
// ===========================================================================

struct VoxlVertex {
    vec3  position;      // chunk-local, 0..32 inclusive
    uint  direction;     // voxl::Direction, indexes kVoxlNormals
    float sunlight;      // 0..1
    float blockLight;    // 0..1
    float ao;            // 0..1, 1 = unoccluded
    uint  textureLayer;
    vec2  uv;            // in BLOCK units, so GL_REPEAT tiles a greedy quad
};

VoxlVertex voxlUnpackVertex(uint data0, uint data1)
{
    VoxlVertex v;
    v.position = vec3(float( data0        & 63u),
                      float((data0 >>  6) & 63u),
                      float((data0 >> 12) & 63u));
    v.direction    = (data0 >> 18) & 7u;
    v.sunlight     = float((data0 >> 21) & 15u) / 15.0;
    v.blockLight   = float((data0 >> 25) & 15u) / 15.0;
    v.ao           = float((data0 >> 29) &  3u) /  3.0;
    v.textureLayer =  data1 & 4095u;

    // Width and height are the greedy quad's extent along its own U and V axes.
    // Multiplying the corner selector by them gives UVs of 0..width / 0..height,
    // which is exactly one texture repeat per block.
    vec2 size = vec2(float(((data1 >> 12) & 31u) + 1u),
                     float(((data1 >> 17) & 31u) + 1u));
    uint corner = (data1 >> 22) & 3u;
    v.uv = size * vec2(float(corner == 1u || corner == 2u),
                       float(corner == 2u || corner == 3u));

    // The mesher's tangent frame (kFaceFrames in GreedyMesher.cpp) is chosen so
    // that u_hat x v_hat is the outward normal, which is a *winding* constraint.
    // It happens to put the V axis along world Y for NegX and PosZ, but along
    // world Z and world X for PosX and NegZ. Left alone, that renders every
    // directional side texture - grass side, wood grain, sandstone banding -
    // rotated 90 degrees on two of the four vertical faces. Swapping the
    // components on those two directions makes texture V run along world +Y on
    // all four sides. Both are side faces, so uAxis is Y for each and the swap
    // is exact rather than an approximation; the resulting mirror along the
    // horizontal axis is invisible on tiling textures.
    if (v.direction == 1u || v.direction == 4u) {   // PosX, NegZ
        v.uv = v.uv.yx;
    }
    return v;
}

const vec3 kVoxlNormals[6] = vec3[6](
    vec3(-1.0,  0.0,  0.0),   // NegX
    vec3( 1.0,  0.0,  0.0),   // PosX
    vec3( 0.0, -1.0,  0.0),   // NegY
    vec3( 0.0,  1.0,  0.0),   // PosY
    vec3( 0.0,  0.0, -1.0),   // NegZ
    vec3( 0.0,  0.0,  1.0));  // PosZ

// Fixed per-face tint, independent of the sun direction. This is the oldest
// trick in voxel rendering and it is still the single biggest readability win:
// without it, two perpendicular walls lit only by ambient light are the same
// colour and the geometry disappears.
const float kVoxlFaceShade[6] = float[6](0.76, 0.76, 0.52, 1.0, 0.88, 0.88);

/// Lambert-ish shading of one voxel face.
///
/// `sunlight` and `blockLight` come from the voxel light grid (0..1) and are what
/// make caves dark; the geometric N.L term only modulates surfaces the sky can
/// actually reach. Without that gating, a cave ceiling facing the sun would be
/// lit through solid rock.
vec3 voxlShadeSurface(vec3 albedo, uint direction, float sunlight, float blockLight, float ao)
{
    vec3 normal = kVoxlNormals[direction];

    // Squared ambient occlusion: the linear term is too subtle to read at 2 bits
    // of precision, and squaring deepens the corners without crushing them.
    float aoCurve = ao * ao;
    float aoTerm  = mix(1.0 - VOXL_AO_STRENGTH, 1.0, aoCurve);

    // Wrapped diffuse. The 0.18 floor stands in for sky light arriving from
    // directions other than the sun; a hard max(N.L, 0) makes every north-facing
    // wall look like it is in a different world.
    float ndotl   = max(dot(normal, VOXL_SUN_DIR), 0.0);
    float diffuse = ndotl * 0.82 + 0.18;

    // Skylight gates the sun contribution non-linearly so a cave mouth falls off
    // quickly instead of glowing several blocks in.
    float exposure = sunlight * sunlight;
    vec3  sun      = VOXL_SUN_COLOUR * (diffuse * exposure);

    // Ambient keeps its own floor so an unlit interior is dim, not black.
    vec3 ambient = VOXL_AMBIENT * (0.30 + 0.70 * sunlight);

    // Torch light saturates fast (smoothstep) so a light source reads as a
    // pool of light rather than a linear ramp.
    float torch = blockLight * blockLight * (3.0 - 2.0 * blockLight);
    vec3  block = VOXL_BLOCK_LIGHT * torch;

    return albedo * kVoxlFaceShade[direction] * aoTerm * (sun + ambient + block);
}
