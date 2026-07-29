# Dependency acquisition for Voxl.
#
# All third-party code is fetched at configure time via FetchContent and cached
# in ${VOXL_DEPS_DIR} so that repeated configures stay offline-friendly.
# Header-only / build-system-less projects are wrapped in INTERFACE or STATIC
# targets defined here rather than being patched upstream.

include(FetchContent)

set(VOXL_DEPS_DIR "${CMAKE_SOURCE_DIR}/.deps" CACHE PATH
    "Directory used to cache third-party sources between configures")
set(FETCHCONTENT_BASE_DIR "${VOXL_DEPS_DIR}")

# Third parties are not our code: never let their warnings gate our build, and
# never let their install rules pollute ours.
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

# ---------------------------------------------------------------- GLFW 3.4 --
set(GLFW_BUILD_DOCS OFF)
set(GLFW_BUILD_TESTS OFF)
set(GLFW_BUILD_EXAMPLES OFF)
set(GLFW_INSTALL OFF)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE)

# ----------------------------------------------------------------- GLM 1.0.3 --
set(GLM_ENABLE_CXX_20 ON)
set(GLM_BUILD_TESTS OFF)
set(GLM_BUILD_INSTALL OFF)
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.3
    GIT_SHALLOW    TRUE)

# ------------------------------------------------------- FastNoiseLite 1.1.1 --
FetchContent_Declare(fastnoiselite
    GIT_REPOSITORY https://github.com/Auburn/FastNoiseLite.git
    GIT_TAG        v1.1.1
    GIT_SHALLOW    TRUE)

# ------------------------------------------------------------ Dear ImGui 1.92 --
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.9-docking
    GIT_SHALLOW    TRUE)

# ---------------------------------------------------------------------- stb --
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        f58f558c120e9b32c217290b80bad1a0729fbb2c
    GIT_SHALLOW    FALSE)

# --------------------------------------------------------- miniaudio 0.11.25 --
# miniaudio's own CMakeLists builds a dozen optional node libraries and claims
# the plain `miniaudio` target name. SOURCE_SUBDIR points at a directory that
# does not exist, which makes FetchContent download the sources without ever
# running add_subdirectory on them. We compile the single header ourselves.
FetchContent_Declare(miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG        0.11.25
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  voxl-no-configure)

# --------------------------------------------------------------- Catch2 3.x --
if(VOXL_BUILD_TESTS)
    set(CATCH_INSTALL_DOCS OFF)
    set(CATCH_INSTALL_EXTRAS OFF)
    FetchContent_Declare(catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.11.0
        GIT_SHALLOW    TRUE)
endif()

FetchContent_MakeAvailable(glfw glm)
FetchContent_MakeAvailable(fastnoiselite imgui stb miniaudio)
if(VOXL_BUILD_TESTS)
    FetchContent_MakeAvailable(catch2)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endif()

# GLFW compiles with /W3 + warnings we do not own; keep them out of our log.
if(TARGET glfw AND MSVC)
    target_compile_options(glfw PRIVATE /W0)
endif()

# ------------------------------------------------------------------- glad --
# Pre-generated (tools/generate_glad.py) so that a build never depends on the
# glad web service or a Python environment.
add_library(glad STATIC "${CMAKE_SOURCE_DIR}/external/glad/src/gl.c")
target_include_directories(glad SYSTEM PUBLIC "${CMAKE_SOURCE_DIR}/external/glad/include")
if(MSVC)
    target_compile_options(glad PRIVATE /W0)
endif()
add_library(voxl::glad ALIAS glad)

# --------------------------------------------------- header-only wrappers --
# All wrapper targets are prefixed so they can never collide with a target an
# upstream project defines for itself.
add_library(voxl_fastnoiselite INTERFACE)
target_include_directories(voxl_fastnoiselite SYSTEM INTERFACE "${fastnoiselite_SOURCE_DIR}/Cpp")
add_library(voxl::fastnoiselite ALIAS voxl_fastnoiselite)

add_library(voxl_stb INTERFACE)
target_include_directories(voxl_stb SYSTEM INTERFACE "${stb_SOURCE_DIR}")
add_library(voxl::stb ALIAS voxl_stb)

# One TU instantiates the miniaudio implementation; see external/miniaudio_impl.c.
add_library(voxl_miniaudio STATIC "${CMAKE_SOURCE_DIR}/external/miniaudio_impl.c")
target_include_directories(voxl_miniaudio SYSTEM PUBLIC "${miniaudio_SOURCE_DIR}")
if(MSVC)
    target_compile_options(voxl_miniaudio PRIVATE /W0)
endif()
add_library(voxl::miniaudio ALIAS voxl_miniaudio)

# ------------------------------------------------------------ Dear ImGui --
# ImGui has no CMake of its own; build core + the two backends we need.
add_library(voxl_imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/imgui_demo.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp")
target_include_directories(voxl_imgui SYSTEM PUBLIC
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends")
# Route ImGui's GL calls through our glad loader instead of its own gl3w copy.
# IMGUI_IMPL_OPENGL_LOADER_CUSTOM only suppresses the bundled loader - it does
# not include anything - so glad must additionally be force-included into the
# backend TU. Sharing one loader matters: two sets of GL function pointers in a
# process silently diverge once one of them is loaded against a different
# context.
target_compile_definitions(voxl_imgui PUBLIC
    IMGUI_IMPL_OPENGL_LOADER_CUSTOM
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(voxl_imgui PUBLIC glfw glad)
if(MSVC)
    target_compile_options(voxl_imgui PRIVATE /W0 /FIglad/gl.h)
else()
    target_compile_options(voxl_imgui PRIVATE -include glad/gl.h)
endif()
add_library(voxl::imgui ALIAS voxl_imgui)
