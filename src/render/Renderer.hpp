#pragma once

// The frame: GL pipeline state, the sky, the three chunk passes and the
// sub-voxel damage pass.
//
// STATE DISCIPLINE: GL state is set once per pass, never per draw. Every pass
// below states the complete set of state it depends on, and `endFrame()` leaves
// the context in a documented neutral configuration so whatever draws next (the
// ImGui overlay) does not inherit a surprise.
//
// Thread safety: NONE. Main thread only, like everything else that touches GL.

#include "render/Camera.hpp"
#include "render/ChunkRenderer.hpp"
#include "render/GpuBuffer.hpp"
#include "render/Shader.hpp"
#include "render/Texture.hpp"

#include <glad/gl.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace voxl {

/// Per-frame constants shared by every program, std140, uniform binding point 0.
///
/// MIRRORED IN assets/shaders/common.glsl. The static_asserts below pin every
/// offset: std140 rounds vec3 up to 16 bytes and a silently misaligned member
/// makes the shader read a neighbouring field, which looks like a lighting bug.
struct alignas(16) FrameUniforms {
    glm::mat4 viewProjection{1.0f};
    glm::mat4 inverseViewProjection{1.0f};
    glm::vec4 cameraPositionTime{0.0f};
    glm::vec4 sunDirectionDay{0.0f, 1.0f, 0.0f, 1.0f};
    glm::vec4 sunColourIntensity{1.0f};
    glm::vec4 skyZenith{0.0f};
    glm::vec4 skyHorizon{0.0f};
    glm::vec4 ambientAo{0.0f};
    glm::vec4 blockLightGain{0.0f};
    glm::vec4 fogParams{0.0f};
};

static_assert(sizeof(FrameUniforms) == 256, "FrameUniforms must match the std140 block");
static_assert(offsetof(FrameUniforms, cameraPositionTime) == 128);
static_assert(offsetof(FrameUniforms, fogParams) == 240);

/// Everything the sky and the lighting are driven by. All colours are LINEAR.
///
/// A day/night cycle animates this struct; nothing in the renderer or the shaders
/// needs to change for it.
struct SkySettings {
    /// Unit vector pointing TOWARDS the sun. Normalised on assignment by the
    /// renderer, so callers can pass an unnormalised direction.
    glm::vec3 sunDirection{0.38f, 0.82f, 0.42f};
    glm::vec3 sunColour{1.0f, 0.96f, 0.88f};
    float     sunIntensity = 1.15f;

    glm::vec3 zenithColour{0.14f, 0.32f, 0.72f};
    glm::vec3 horizonColour{0.60f, 0.75f, 0.93f};

    /// Sky bounce light. Cool, because it stands in for light arriving from the
    /// blue dome rather than from the sun.
    glm::vec3 ambientColour{0.30f, 0.37f, 0.50f};

    /// Warm torch/glowstone colour.
    glm::vec3 blockLightColour{1.0f, 0.72f, 0.40f};
    float     blockLightGain = 1.25f;

    /// How much ambient occlusion darkens a fully occluded corner, 0..1.
    float aoStrength = 0.55f;

    /// 1 = full day, 0 = night. Only fades the stars in; the light levels
    /// themselves come from `sunIntensity` and the colours above.
    float dayFactor = 1.0f;
};

/// Distance fog, in blocks.
struct FogSettings {
    float start = 140.0f;
    float end   = 340.0f;
    /// Exponent applied to the normalised distance. Above 1 the fog stays out of
    /// the way until it is needed; 1 is a plain linear ramp.
    float curve = 2.2f;
};

struct RendererConfig {
    /// Directory containing `shaders/`. Assets are copied next to the executable
    /// by the build, so the default is correct for a normal run.
    std::filesystem::path assetRoot{"assets"};

    /// Authoring resolution of the block texture array.
    int textureResolution = kBlockTextureSize;

    /// Optional override that supplies real images per layer; see
    /// render/Texture.hpp. Empty means "everything procedural".
    BlockTextureLoader textureLoader{};
};

struct RenderStats {
    ChunkRenderStats chunks{};
    /// Includes the sky triangle.
    std::uint32_t drawCalls = 0;
    std::uint64_t triangles = 0;

    int  framebufferWidth  = 0;
    int  framebufferHeight = 0;
    bool srgbFramebuffer   = false;
    float textureAnisotropy = 1.0f;
    std::size_t textureBytes = 0;
};

/// Owns the render pipeline for one GL context.
///
/// Pinned: it owns GL objects and hands out references to its ChunkRenderer.
class Renderer {
public:
    /// Requires a current GL 4.5 core context (Window creates it). Logs and
    /// degrades rather than throwing: check `valid()` before drawing.
    explicit Renderer(const RendererConfig& config = {});
    ~Renderer();

    Renderer(const Renderer&)            = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&)                 = delete;
    Renderer& operator=(Renderer&&)      = delete;

    /// False when a shader failed to compile or the texture array could not be
    /// created. Drawing is then a no-op rather than undefined behaviour.
    [[nodiscard]] bool valid() const noexcept { return m_valid; }

    /// Framebuffer size in pixels. Also sets the viewport used by every pass.
    void resize(int width, int height);

    void                             setSky(const SkySettings& sky);
    [[nodiscard]] const SkySettings& sky() const noexcept { return m_sky; }
    void                             setFog(const FogSettings& fog) noexcept { m_fog = fog; }
    [[nodiscard]] const FogSettings& fog() const noexcept { return m_fog; }

    /// Derives sensible fog distances from a view distance in blocks. Fog must
    /// finish just inside the far plane, or chunks pop into existence at full
    /// contrast right where streaming stops.
    void setFogFromViewDistance(float blocks) noexcept;

    /// Clears, and uploads the frame uniform block. `timeSeconds` drives the
    /// water animation and the star twinkle; it must be monotonic but its origin
    /// does not matter.
    void beginFrame(const Camera& camera, double timeSeconds);

    /// Fullscreen sky. Draw before the world: it is one triangle, and drawing it
    /// last would need depth testing against geometry that has already blended.
    void drawSky();

    /// Opaque, then the sub-voxel damage pass, then cutout, then translucent.
    /// Call after `chunks().cull(...)`, which `beginFrame` does for you using
    /// the camera it was given.
    ///
    /// The sub-voxel pass sits inside the opaque block, after the whole-block
    /// geometry: it shares the depth buffer and the block texture array, and
    /// running it before cutout means alpha-tested foliage still depth-tests
    /// against carved surfaces correctly.
    void drawWorld();

    /// Restores the neutral state described in the header comment.
    void endFrame();

    [[nodiscard]] ChunkRenderer&       chunks() noexcept { return m_chunks; }
    [[nodiscard]] const ChunkRenderer& chunks() const noexcept { return m_chunks; }

    /// Recompiles any program whose source file changed. Bind this to a key; a
    /// failed compile logs and keeps the working program.
    std::size_t reloadShaders();

    /// False when assets/shaders/subvoxel.{vert,frag} could not be loaded. The
    /// rest of the renderer stays valid and the damage pass is simply skipped -
    /// a missing optional pass must not black out the world.
    [[nodiscard]] bool subVoxelPassAvailable() const noexcept
    {
        return m_subVoxelProgram != nullptr;
    }

    [[nodiscard]] const RenderStats& stats() const noexcept { return m_stats; }

    /// Colour the sky renders at the horizon, for anything that has to match it.
    [[nodiscard]] glm::vec3 horizonColour() const noexcept { return m_sky.horizonColour; }

private:
    bool loadShaders();
    void detectFramebufferEncoding();
    void applyOpaqueState();
    void applySubVoxelState();
    void applyCutoutState();
    void applyTranslucentState();

    /// Cached uniform locations. Looking a name up per draw is a hash lookup we
    /// can trivially avoid, and per *pass* it is just noise in the profile.
    struct ChunkProgramUniforms {
        GLint chunkOrigin = -1;
        GLint alphaCutoff = -1;
        GLint waterLayer  = -1;
    };

    RendererConfig m_config;
    ShaderLibrary  m_shaders;
    ShaderProgram* m_chunkProgram = nullptr;
    ShaderProgram* m_waterProgram = nullptr;
    ShaderProgram* m_skyProgram   = nullptr;
    /// Optional: null when subvoxel.{vert,frag} is absent or failed to compile.
    ShaderProgram*       m_subVoxelProgram = nullptr;
    ChunkProgramUniforms m_chunkUniforms{};
    ChunkProgramUniforms m_waterUniforms{};
    ChunkProgramUniforms m_subVoxelUniforms{};

    TextureArray  m_blockTextures;
    ChunkRenderer m_chunks;

    GpuBuffer   m_frameUniformBuffer;
    VertexArray m_skyVertexArray;  ///< empty; core profile still requires one bound

    SkySettings m_sky{};
    FogSettings m_fog{};

    int  m_width            = 0;
    int  m_height           = 0;
    bool m_srgbFramebuffer  = false;
    bool m_valid            = false;

    RenderStats m_stats{};
};

}  // namespace voxl
