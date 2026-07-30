#include "render/Renderer.hpp"

#include "core/Log.hpp"

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/matrix.hpp>

namespace voxl {
namespace {

/// Uniform block binding point, matching `layout(std140, binding = 0)` in
/// assets/shaders/common.glsl.
constexpr GLuint kFrameUniformBinding = 0;

/// Texture unit for the block texture array, matching
/// `layout(binding = 0) uniform sampler2DArray` in the chunk shaders.
constexpr GLuint kBlockTextureUnit = 0;

/// Alpha threshold for the cutout pass. Deliberately below 0.5: minification
/// averages a leaf texture's alpha downward, and a 0.5 test makes distant
/// canopies thin out and then vanish.
constexpr float kCutoutThreshold = 0.35f;

/// The sky is one triangle.
constexpr std::uint64_t kSkyTriangles = 1;

[[nodiscard]] glm::vec3 safeNormalize(const glm::vec3& value, const glm::vec3& fallback) noexcept
{
    const float lengthSquared = glm::dot(value, value);
    if (!(lengthSquared > 1e-12f)) {
        return fallback;  // also catches NaN, which would poison every lighting term
    }
    return value / std::sqrt(lengthSquared);
}

/// sRGB transfer function, used only for the clear colour on the fallback path
/// where the driver will not encode for us.
[[nodiscard]] float encodeSrgb(float linear) noexcept
{
    const float c = std::clamp(linear, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

/// Layer index of a named block texture, or -1. Derived from the layer table
/// rather than hard-coded so world/Block.cpp stays the single source of truth.
[[nodiscard]] int findTextureLayer(std::string_view name) noexcept
{
    for (int layer = 0; layer < kBlockTextureLayerCount; ++layer) {
        if (blockTextureLayerName(layer) == name) {
            return layer;
        }
    }
    return -1;
}

}  // namespace

Renderer::Renderer(const RendererConfig& config)
    : m_config(config), m_shaders(config.assetRoot / "shaders")
{
    if (glCreateBuffers == nullptr) {
        VOXL_LOG_ERROR("Renderer constructed without a loaded GL 4.5 context");
        return;
    }

    detectFramebufferEncoding();

    m_blockTextures = createBlockTextureArray(m_config.textureResolution, m_config.textureLoader);
    if (!m_blockTextures) {
        VOXL_LOG_ERROR("Renderer: block texture array could not be created");
        return;
    }

    m_frameUniformBuffer.upload(nullptr, sizeof(FrameUniforms));
    glBindBufferBase(GL_UNIFORM_BUFFER, kFrameUniformBinding, m_frameUniformBuffer.id());

    // Core profile forbids drawing with no VAO bound, even when the vertex shader
    // reads nothing but gl_VertexID.
    m_skyVertexArray.create();

    if (!loadShaders()) {
        return;
    }

    // State that never changes for the lifetime of the context.
    glFrontFace(GL_CCW);
    glCullFace(GL_BACK);
    glDepthFunc(GL_LESS);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    if (m_srgbFramebuffer) {
        glEnable(GL_FRAMEBUFFER_SRGB);
    }

    setSky(m_sky);
    m_valid = true;
    VOXL_LOG_INFO("renderer ready (sRGB framebuffer: {})", m_srgbFramebuffer ? "yes" : "no");
}

Renderer::~Renderer()
{
    // Every GL object is owned by an RAII member; ordering is handled by the
    // reverse-declaration destruction order.
}

void Renderer::detectFramebufferEncoding()
{
    // Ask the default framebuffer whether its colour attachment is sRGB. If it is,
    // the hardware performs the encode on write and the shaders must output
    // linear; if it is not (GLFW was not asked for an sRGB-capable framebuffer)
    // the shaders have to encode themselves or the whole world looks washed out.
    while (glGetError() != GL_NO_ERROR) {
        // Drain errors left by earlier initialisation so the query below is
        // attributable.
    }

    GLint encoding = GL_LINEAR;
    glGetNamedFramebufferAttachmentParameteriv(0, GL_BACK_LEFT,
                                               GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &encoding);
    if (glGetError() != GL_NO_ERROR) {
        VOXL_LOG_WARN("could not query the default framebuffer's colour encoding; "
                      "assuming linear and gamma-encoding in the shaders");
        m_srgbFramebuffer = false;
        return;
    }
    m_srgbFramebuffer = encoding == GL_SRGB;
}

bool Renderer::loadShaders()
{
    m_chunkProgram = m_shaders.load("chunk");
    m_waterProgram = m_shaders.load("water");
    m_skyProgram   = m_shaders.load("sky");

    if (m_chunkProgram == nullptr || m_waterProgram == nullptr || m_skyProgram == nullptr) {
        VOXL_LOG_ERROR("Renderer: shader loading failed; nothing will be drawn");
        return false;
    }

    // Deliberately NOT part of the validity test above. The sub-voxel pass draws
    // player-carved damage, which most worlds have none of; losing it must cost
    // the damage geometry, not the entire world. ShaderProgram already logs the
    // compile/link failure in full, so this is a one-line note on top.
    m_subVoxelProgram = m_shaders.load("subvoxel");
    if (m_subVoxelProgram == nullptr) {
        VOXL_LOG_ERROR("Renderer: subvoxel program unavailable; sub-voxel damage will not draw");
    } else {
        m_subVoxelUniforms.chunkOrigin = m_subVoxelProgram->uniformLocation("uChunkOrigin");
    }

    m_chunkUniforms.chunkOrigin = m_chunkProgram->uniformLocation("uChunkOrigin");
    m_chunkUniforms.alphaCutoff = m_chunkProgram->uniformLocation("uAlphaCutoff");
    m_waterUniforms.chunkOrigin = m_waterProgram->uniformLocation("uChunkOrigin");
    m_waterUniforms.waterLayer  = m_waterProgram->uniformLocation("uWaterLayer");

    const int waterLayer = findTextureLayer("water");
    if (waterLayer < 0) {
        VOXL_LOG_ERROR("no 'water' entry in the block texture layer table");
        return false;
    }
    // Set once: a uniform keeps its value across draws, so this does not belong
    // in the per-pass path.
    m_waterProgram->setUInt(m_waterUniforms.waterLayer, static_cast<GLuint>(waterLayer));
    return true;
}

std::size_t Renderer::reloadShaders()
{
    const std::size_t reloaded = m_shaders.reloadChanged();
    if (reloaded == 0) {
        return 0;
    }
    // Locations and uniform values do not survive a relink.
    loadShaders();
    VOXL_LOG_INFO("reloaded {} shader program(s)", reloaded);
    return reloaded;
}

void Renderer::resize(int width, int height)
{
    m_width  = std::max(width, 0);
    m_height = std::max(height, 0);
}

void Renderer::setSky(const SkySettings& sky)
{
    m_sky = sky;
    // Normalise here rather than trusting the caller: a day/night cycle that
    // interpolates two directions produces a shortening vector, which would dim
    // the sun as it crosses the sky for no visible reason.
    m_sky.sunDirection = safeNormalize(sky.sunDirection, glm::vec3{0.0f, 1.0f, 0.0f});
}

void Renderer::setFogFromViewDistance(float blocks) noexcept
{
    const float distance = std::max(blocks, 32.0f);
    // Fog must be complete a little before the last loaded chunk, otherwise the
    // streaming boundary is visible as terrain appearing at full contrast.
    m_fog.start = distance * 0.45f;
    m_fog.end   = distance * 0.92f;
}

void Renderer::beginFrame(const Camera& camera, double timeSeconds)
{
    m_chunks.beginFrame();
    if (!m_valid || m_width <= 0 || m_height <= 0) {
        return;
    }

    glViewport(0, 0, m_width, m_height);

    // Re-armed every frame because endFrame turns it off for the display-referred
    // overlay passes; see the note there.
    if (m_srgbFramebuffer) {
        glEnable(GL_FRAMEBUFFER_SRGB);
    }

    FrameUniforms frame;
    frame.viewProjection        = camera.viewProjection();
    frame.inverseViewProjection = glm::inverse(camera.viewProjection());
    frame.cameraPositionTime =
        glm::vec4{camera.position(), static_cast<float>(timeSeconds)};
    frame.sunDirectionDay    = glm::vec4{m_sky.sunDirection, m_sky.dayFactor};
    frame.sunColourIntensity = glm::vec4{m_sky.sunColour, m_sky.sunIntensity};
    frame.skyZenith          = glm::vec4{m_sky.zenithColour, 0.0f};
    frame.skyHorizon         = glm::vec4{m_sky.horizonColour, 0.0f};
    frame.ambientAo          = glm::vec4{m_sky.ambientColour, m_sky.aoStrength};
    frame.blockLightGain     = glm::vec4{m_sky.blockLightColour, m_sky.blockLightGain};
    frame.fogParams          = glm::vec4{m_fog.start, m_fog.end, std::max(m_fog.curve, 0.05f),
                                         m_srgbFramebuffer ? 0.0f : 1.0f};

    m_frameUniformBuffer.update(0, &frame, sizeof(frame));

    // The sky triangle covers every pixel, so the colour clear is only insurance
    // against a failed sky draw - but it is nearly free and a magenta screen is
    // much harder to diagnose than a plain horizon.
    glm::vec3 clearColour = m_sky.horizonColour;
    if (!m_srgbFramebuffer) {
        clearColour = glm::vec3{encodeSrgb(clearColour.r), encodeSrgb(clearColour.g),
                                encodeSrgb(clearColour.b)};
    }
    glClearColor(clearColour.r, clearColour.g, clearColour.b, 1.0f);
    glDepthMask(GL_TRUE);  // a disabled depth write would silently skip the clear
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    m_chunks.cull(camera.frustum(), camera.position());

    m_stats.framebufferWidth  = m_width;
    m_stats.framebufferHeight = m_height;
    m_stats.srgbFramebuffer   = m_srgbFramebuffer;
    m_stats.textureAnisotropy = m_blockTextures.anisotropy();
    m_stats.textureBytes      = m_blockTextures.estimatedBytes();
    m_stats.drawCalls         = 0;
    m_stats.triangles         = 0;
}

void Renderer::drawSky()
{
    if (!m_valid || m_width <= 0 || m_height <= 0) {
        return;
    }

    // Pass state: no depth test, no depth write, no culling, no blending. The sky
    // is drawn first and unconditionally, so it needs none of them.
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    m_skyProgram->use();
    m_skyVertexArray.bind();
    glDrawArrays(GL_TRIANGLES, 0, 3);

    m_stats.drawCalls += 1;
    m_stats.triangles += kSkyTriangles;
}

void Renderer::applyOpaqueState()
{
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glDisable(GL_BLEND);
}

void Renderer::applySubVoxelState()
{
    // Identical to opaque, and stated in full rather than inherited: the passes
    // are independent of each other's ordering by design, and "it happens to
    // follow the opaque pass" is exactly the assumption that breaks the first
    // time someone reorders drawWorld().
    //
    // Back-face culling stays ON. A carved cavity's inward-facing surfaces are
    // emitted by the mesher as real quads with outward normals, so there is
    // nothing here that relies on seeing a back face.
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glDisable(GL_BLEND);
}

void Renderer::applyCutoutState()
{
    // Same as opaque. Cutout differs only in the shader's discard, which is why
    // it shares the program and needs no state change of its own - but stating it
    // explicitly keeps the passes independent of each other's ordering.
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glDisable(GL_BLEND);
}

void Renderer::applyTranslucentState()
{
    glEnable(GL_DEPTH_TEST);
    // No depth writes: translucent surfaces must not occlude each other, and the
    // back-to-front chunk ordering is what composites them.
    glDepthMask(GL_FALSE);
    // No back-face culling: the underside of a lake surface has to be visible
    // when the player swims under it.
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
}

void Renderer::drawWorld()
{
    if (!m_valid || m_width <= 0 || m_height <= 0) {
        return;
    }

    m_blockTextures.bindUnit(kBlockTextureUnit);

    applyOpaqueState();
    m_chunkProgram->use();
    m_chunkProgram->setFloat(m_chunkUniforms.alphaCutoff, 0.0f);
    m_chunks.drawLayer(RenderLayer::Opaque, *m_chunkProgram, m_chunkUniforms.chunkOrigin);

    // Sub-voxel damage: same depth buffer, same block texture array (already
    // bound above), different vertex packing and therefore a different program.
    // ChunkRenderer returns without touching any state when nothing on screen is
    // damaged, but the program switch and the state call are per pass, so guard
    // them on the visible count rather than paying them every frame.
    if (m_subVoxelProgram != nullptr && m_chunks.visibleSubVoxelCount() != 0) {
        applySubVoxelState();
        m_subVoxelProgram->use();
        m_chunks.drawSubVoxels(*m_subVoxelProgram, m_subVoxelUniforms.chunkOrigin);
    }

    applyCutoutState();
    m_chunkProgram->use();
    m_chunkProgram->setFloat(m_chunkUniforms.alphaCutoff, kCutoutThreshold);
    m_chunks.drawLayer(RenderLayer::Cutout, *m_chunkProgram, m_chunkUniforms.chunkOrigin);

    applyTranslucentState();
    m_waterProgram->use();
    m_chunks.drawLayer(RenderLayer::Translucent, *m_waterProgram, m_waterUniforms.chunkOrigin);

    m_stats.chunks = m_chunks.stats();
    m_stats.drawCalls += m_stats.chunks.drawCalls;
    m_stats.triangles += m_stats.chunks.trianglesRendered;
}

void Renderer::endFrame()
{
    // Neutral state for whatever draws next (the ImGui overlay): depth test and
    // writes on, back-face culling on, blending off, no VAO or program bound.
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    VertexArray::unbind();
    glUseProgram(0);

    // The world shaders work in linear space and let the hardware encode. Every
    // overlay drawn after this point - ImGui, the selection box - emits colours
    // that are already display-referred, so leaving GL_FRAMEBUFFER_SRGB on would
    // encode them a second time and wash the whole HUD out. beginFrame turns it
    // back on. No-op on drivers that did not grant an sRGB default framebuffer.
    if (m_srgbFramebuffer) {
        glDisable(GL_FRAMEBUFFER_SRGB);
    }
}

}  // namespace voxl
