# Headless test suite. Nothing here may create a GL context: the tests must run
# on a build agent with no GPU, so anything GPU-bound belongs in benchmarks/.
include(Catch)

add_executable(voxl_tests
    test_audio.cpp
    test_chunkstorage.cpp
    test_daynight.cpp
    test_jobsystem.cpp
    test_lighting.cpp
    test_lod_mesh.cpp
    test_lod_stream.cpp
    test_lod_terrain.cpp
    test_log.cpp
    test_mesher.cpp
    test_mining.cpp
    test_persistence.cpp
    test_physics.cpp
    test_settings.cpp
    test_shaders.cpp
    test_subvoxel.cpp
    test_subvoxel_mesh.cpp
    test_terrain.cpp
    test_world.cpp
)

target_link_libraries(voxl_tests PRIVATE
    voxl::engine
    Catch2::Catch2WithMain)

# test_shaders.cpp reads the shipped .glsl sources off disk to assert on their
# structure. Point it at the source tree rather than at the copy staged next to
# the executable, so the test fails on an edit the moment it is made instead of
# after the next asset sync, and so it does not depend on ctest's working
# directory.
target_compile_definitions(voxl_tests PRIVATE
    VOXL_SHADER_DIR="${CMAKE_SOURCE_DIR}/assets/shaders")

catch_discover_tests(voxl_tests)
