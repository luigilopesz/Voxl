// Camera.hpp is entirely inline - a first-person camera is a handful of trig
// calls and hiding them behind a call boundary would cost more than it saves.
//
// This translation unit exists for two things that a header cannot do:
//
//  1. It enforces the GLM build configuration the frozen conventions depend on.
//     GLM's clip-space and handedness behaviour is controlled by preprocessor
//     macros, and defining one of them anywhere in the build would silently
//     change what glm::perspectiveRH_NO and the Gribb-Hartmann plane extraction
//     in Frustum::update mean. The failure mode is geometry that culls
//     incorrectly only near the edges of the screen - the kind of bug that gets
//     blamed on the mesher for a week. A hard compile error is cheaper.
//
//  2. It is a standalone-compilation canary for the header: any missing include
//     or ODR problem in Camera.hpp shows up here rather than in whichever
//     unlucky file happens to include it first.

#include "render/Camera.hpp"

#include <type_traits>

#ifdef GLM_FORCE_DEPTH_ZERO_TO_ONE
    #error "Voxl assumes OpenGL clip space (z in [-1, 1]). Frustum::update and the projection matrix both break under GLM_FORCE_DEPTH_ZERO_TO_ONE."
#endif

#ifdef GLM_FORCE_LEFT_HANDED
    #error "Voxl is right-handed: +X east, +Y up, +Z south, forward is -Z. GLM_FORCE_LEFT_HANDED inverts the basis and the frustum planes."
#endif

namespace voxl {

// The frustum is copied to worker threads for parallel culling, and Aabb values
// are copied per chunk in the inner loop. Both must stay trivially copyable; a
// member that needs a real copy constructor would turn that loop into a series
// of function calls without anyone noticing.
static_assert(std::is_trivially_copyable_v<Plane>);
static_assert(std::is_trivially_copyable_v<Aabb>);
static_assert(std::is_trivially_copyable_v<Frustum>);
static_assert(std::is_nothrow_copy_constructible_v<Frustum>);

static_assert(Frustum::kPlaneCount == 6, "the plane extraction writes exactly six planes");

// The camera itself is main-thread state and is never copied across a thread
// boundary, but it must remain cheap to copy for save/restore of a view.
static_assert(std::is_trivially_copyable_v<Camera>);

}  // namespace voxl
