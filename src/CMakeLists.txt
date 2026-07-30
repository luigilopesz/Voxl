# The engine is a single static library so that both the game executable and
# the test/benchmark harnesses link the exact same code.
add_library(voxl_engine STATIC)
add_library(voxl::engine ALIAS voxl_engine)

target_include_directories(voxl_engine PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(voxl_engine
    PUBLIC
        voxl::compile_options
        voxl::glad
        glfw
        glm::glm
        voxl::imgui
    PRIVATE
        voxl::stb
        voxl::fastnoiselite
        voxl::miniaudio)

# Sources are listed explicitly (never globbed) so that adding a file is a
# reviewable change and CMake reliably reconfigures.
target_sources(voxl_engine PRIVATE
    app/Application.cpp

    core/JobSystem.cpp
    core/Log.cpp
    core/Profiler.cpp
    core/Time.cpp

    gameplay/BlockInteraction.cpp
    gameplay/Hotbar.cpp
    gameplay/Player.cpp

    mesh/GreedyMesher.cpp
    mesh/SubVoxelMesher.cpp

    physics/Collision.cpp
    physics/Raycast.cpp

    platform/Input.cpp
    platform/Window.cpp

    render/Camera.cpp
    render/ChunkRenderer.cpp
    render/GpuBuffer.cpp
    render/Renderer.cpp
    render/Shader.cpp
    render/Texture.cpp
    render/TextureGen.cpp

    ui/DebugOverlay.cpp
    ui/Hud.cpp

    world/Block.cpp
    world/Chunk.cpp
    world/ChunkManager.cpp
    world/ChunkStorage.cpp
    world/SubVoxel.cpp
    world/TerrainGenerator.cpp
    world/World.cpp
)

# ------------------------------------------------------------ executable --
add_executable(voxl WIN32 app/Main.cpp)
target_link_libraries(voxl PRIVATE voxl::engine)

# Keep a console attached in non-Release builds so logging is visible.
if(MSVC)
    target_link_options(voxl PRIVATE
        $<$<NOT:$<CONFIG:Release>>:/SUBSYSTEM:CONSOLE>
        $<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS>)
endif()
