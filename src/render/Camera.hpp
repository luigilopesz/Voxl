#pragma once

// First-person camera, plus the frustum used for chunk culling.
//
// CONVENTIONS - downstream shaders and the culling code depend on all of these:
//
//  * Right-handed world space. +X east, +Y up, +Z south. Forward is -Z at zero
//    rotation, matching glm's lookAtRH / perspectiveRH.
//  * Clip space is OpenGL's: z in [-1, 1] after the perspective divide. GLM is
//    NOT built with GLM_FORCE_DEPTH_ZERO_TO_ONE, and the Gribb-Hartmann plane
//    extraction below assumes the [-1, 1] convention.
//  * Angles are DEGREES in the public API and converted internally. Mouse-look
//    code deals in degrees per pixel, and storing radians here just moved the
//    conversions to every call site.
//  * Yaw rotates about +Y: yaw 0 looks down -Z, yaw +90 looks down -X (i.e.
//    increasing yaw turns left, the right-handed positive direction). Pitch is
//    positive looking up and is clamped to +/-89 degrees, which keeps the view
//    matrix's up vector from degenerating at the poles.
//
// Thread safety: none. The camera is main-thread state. A worker that needs the
// frustum takes a copy of the Frustum value type.

#include "world/VoxelTypes.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace voxl {

/// Axis-aligned box in world space.
struct Aabb {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};

    [[nodiscard]] static Aabb fromChunk(const ChunkPos& pos) noexcept
    {
        const BlockPos origin = pos.originBlock();
        const glm::vec3 lo{static_cast<float>(origin.x), static_cast<float>(origin.y),
                           static_cast<float>(origin.z)};
        return Aabb{lo, lo + glm::vec3{static_cast<float>(kChunkSize)}};
    }

    [[nodiscard]] glm::vec3 centre() const noexcept { return (min + max) * 0.5f; }
    [[nodiscard]] glm::vec3 extents() const noexcept { return (max - min) * 0.5f; }
    [[nodiscard]] bool contains(const glm::vec3& p) const noexcept
    {
        return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y && p.z >= min.z &&
               p.z <= max.z;
    }
};

/// Plane in the form dot(normal, p) + distance = 0, normal pointing towards the
/// inside of the frustum.
struct Plane {
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    float     distance = 0.0f;

    [[nodiscard]] float signedDistance(const glm::vec3& point) const noexcept
    {
        return glm::dot(normal, point) + distance;
    }
};

/// Six-plane view frustum extracted from a view-projection matrix.
///
/// Value type on purpose: culling runs over thousands of chunks and may be
/// handed to a worker, so it must be copyable and free of references to the
/// camera that produced it.
class Frustum {
public:
    enum PlaneIndex : std::size_t { Left = 0, Right = 1, Bottom = 2, Top = 3, Near = 4, Far = 5 };
    static constexpr std::size_t kPlaneCount = 6;

    Frustum() = default;
    explicit Frustum(const glm::mat4& viewProjection) noexcept { update(viewProjection); }

    /// Gribb-Hartmann extraction. GLM matrices are column-major, so m[c][r];
    /// the rows of the matrix are gathered explicitly below rather than relying
    /// on an indexing convention that is easy to get backwards.
    void update(const glm::mat4& m) noexcept
    {
        const glm::vec4 row0{m[0][0], m[1][0], m[2][0], m[3][0]};
        const glm::vec4 row1{m[0][1], m[1][1], m[2][1], m[3][1]};
        const glm::vec4 row2{m[0][2], m[1][2], m[2][2], m[3][2]};
        const glm::vec4 row3{m[0][3], m[1][3], m[2][3], m[3][3]};

        assign(Left,   row3 + row0);
        assign(Right,  row3 - row0);
        assign(Bottom, row3 + row1);
        assign(Top,    row3 - row1);
        // Near uses row3 + row2 because clip z starts at -w, not 0.
        assign(Near,   row3 + row2);
        assign(Far,    row3 - row2);
    }

    /// Conservative test: returns true when the box may be visible. Uses the
    /// positive-vertex trick, so a box straddling two planes' outside regions
    /// without being outside either one individually is a false positive - the
    /// cheap and standard trade.
    [[nodiscard]] bool intersects(const Aabb& box) const noexcept
    {
        for (const Plane& plane : m_planes) {
            const glm::vec3 positive{plane.normal.x >= 0.0f ? box.max.x : box.min.x,
                                     plane.normal.y >= 0.0f ? box.max.y : box.min.y,
                                     plane.normal.z >= 0.0f ? box.max.z : box.min.z};
            if (plane.signedDistance(positive) < 0.0f) {
                return false;  // entire box behind this plane
            }
        }
        return true;
    }

    [[nodiscard]] bool contains(const glm::vec3& point) const noexcept
    {
        for (const Plane& plane : m_planes) {
            if (plane.signedDistance(point) < 0.0f) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] const std::array<Plane, kPlaneCount>& planes() const noexcept { return m_planes; }
    [[nodiscard]] const Plane& plane(PlaneIndex index) const noexcept { return m_planes[index]; }

private:
    void assign(PlaneIndex index, const glm::vec4& coefficients) noexcept
    {
        const glm::vec3 normal{coefficients.x, coefficients.y, coefficients.z};
        const float     length = glm::length(normal);
        // A degenerate projection (zero aspect while the window is minimised)
        // would divide by zero and poison every later comparison with NaN.
        const float inverse = length > 1e-8f ? 1.0f / length : 0.0f;
        m_planes[index].normal   = normal * inverse;
        m_planes[index].distance = coefficients.w * inverse;
    }

    std::array<Plane, kPlaneCount> m_planes{};
};

/// First-person camera. Matrices are cached and rebuilt lazily on first use
/// after any state change.
class Camera {
public:
    static constexpr float kMaxPitchDegrees = 89.0f;

    Camera() = default;

    // ---- placement ----

    [[nodiscard]] const glm::vec3& position() const noexcept { return m_position; }
    void setPosition(const glm::vec3& position) noexcept
    {
        m_position   = position;
        m_viewDirty  = true;
    }
    void translate(const glm::vec3& delta) noexcept { setPosition(m_position + delta); }

    [[nodiscard]] float yawDegrees() const noexcept { return m_yawDegrees; }
    [[nodiscard]] float pitchDegrees() const noexcept { return m_pitchDegrees; }

    void setRotation(float yawDegrees, float pitchDegrees) noexcept
    {
        // Wrapping yaw keeps it from growing without bound over a long session,
        // where float precision would eventually make mouse-look grainy.
        m_yawDegrees   = std::fmod(yawDegrees, 360.0f);
        m_pitchDegrees = std::clamp(pitchDegrees, -kMaxPitchDegrees, kMaxPitchDegrees);
        m_viewDirty    = true;
    }

    /// Mouse-look entry point: relative degrees.
    void addRotation(float deltaYawDegrees, float deltaPitchDegrees) noexcept
    {
        setRotation(m_yawDegrees + deltaYawDegrees, m_pitchDegrees + deltaPitchDegrees);
    }

    // ---- basis ----

    [[nodiscard]] glm::vec3 forward() const noexcept
    {
        const float yaw   = glm::radians(m_yawDegrees);
        const float pitch = glm::radians(m_pitchDegrees);
        const float cosPitch = std::cos(pitch);
        return glm::vec3{-std::sin(yaw) * cosPitch, std::sin(pitch), -std::cos(yaw) * cosPitch};
    }

    /// Right vector on the horizontal plane; independent of pitch so that
    /// strafing never drifts vertically when the player looks up.
    [[nodiscard]] glm::vec3 right() const noexcept
    {
        const float yaw = glm::radians(m_yawDegrees);
        return glm::vec3{std::cos(yaw), 0.0f, -std::sin(yaw)};
    }

    [[nodiscard]] glm::vec3 up() const noexcept { return glm::cross(right(), forward()); }

    /// Forward projected onto the horizontal plane, normalised. This is what
    /// WASD movement should use.
    [[nodiscard]] glm::vec3 flatForward() const noexcept
    {
        const float yaw = glm::radians(m_yawDegrees);
        return glm::vec3{-std::sin(yaw), 0.0f, -std::cos(yaw)};
    }

    static constexpr glm::vec3 worldUp() noexcept { return glm::vec3{0.0f, 1.0f, 0.0f}; }

    // ---- lens ----

    [[nodiscard]] float fovDegrees() const noexcept { return m_fovDegrees; }
    /// Vertical field of view.
    void setFovDegrees(float fov) noexcept
    {
        m_fovDegrees      = std::clamp(fov, 10.0f, 170.0f);
        m_projectionDirty = true;
    }

    [[nodiscard]] float aspectRatio() const noexcept { return m_aspectRatio; }
    void setAspectRatio(float aspect) noexcept
    {
        // A minimised window reports zero height; clamping here is cheaper than
        // making every caller check.
        m_aspectRatio     = aspect > 1e-4f ? aspect : 1e-4f;
        m_projectionDirty = true;
    }

    [[nodiscard]] float nearPlane() const noexcept { return m_near; }
    [[nodiscard]] float farPlane() const noexcept { return m_far; }
    /// Near is deliberately not smaller than 0.05: with a 24-bit depth buffer
    /// and a far plane in the hundreds of blocks, a tighter near plane produces
    /// z-fighting on distant terrain.
    void setClipPlanes(float nearDistance, float farDistance) noexcept
    {
        m_near            = std::max(nearDistance, 0.01f);
        m_far             = std::max(farDistance, m_near + 1.0f);
        m_projectionDirty = true;
    }

    // ---- matrices ----

    [[nodiscard]] const glm::mat4& view() const noexcept
    {
        if (m_viewDirty) {
            m_view      = glm::lookAtRH(m_position, m_position + forward(), worldUp());
            m_viewDirty = false;
            m_viewProjectionDirty = true;
        }
        return m_view;
    }

    [[nodiscard]] const glm::mat4& projection() const noexcept
    {
        if (m_projectionDirty) {
            m_projection      = glm::perspectiveRH_NO(glm::radians(m_fovDegrees), m_aspectRatio,
                                                      m_near, m_far);
            m_projectionDirty = false;
            m_viewProjectionDirty = true;
        }
        return m_projection;
    }

    [[nodiscard]] const glm::mat4& viewProjection() const noexcept
    {
        const glm::mat4& v = view();
        const glm::mat4& p = projection();
        if (m_viewProjectionDirty) {
            m_viewProjection      = p * v;
            m_viewProjectionDirty = false;
        }
        return m_viewProjection;
    }

    /// Frustum for the current view-projection. Recomputed on request rather
    /// than cached: callers extract it once per frame and pass it around.
    [[nodiscard]] Frustum frustum() const noexcept { return Frustum{viewProjection()}; }

    /// Eye position as a voxel coordinate, for chunk-distance sorting.
    [[nodiscard]] BlockPos blockPosition() const noexcept { return worldToBlockPos(m_position); }
    [[nodiscard]] ChunkPos chunkPosition() const noexcept { return toChunkPos(blockPosition()); }

private:
    glm::vec3 m_position{0.0f, static_cast<float>(kSeaLevel) + 2.0f, 0.0f};
    float     m_yawDegrees   = 0.0f;
    float     m_pitchDegrees = 0.0f;

    float m_fovDegrees  = 70.0f;
    float m_aspectRatio = 16.0f / 9.0f;
    float m_near        = 0.05f;
    float m_far         = 1024.0f;

    mutable glm::mat4 m_view{1.0f};
    mutable glm::mat4 m_projection{1.0f};
    mutable glm::mat4 m_viewProjection{1.0f};
    mutable bool      m_viewDirty           = true;
    mutable bool      m_projectionDirty     = true;
    mutable bool      m_viewProjectionDirty = true;
};

}  // namespace voxl
