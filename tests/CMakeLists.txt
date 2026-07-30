# Headless test suite. Nothing here may create a GL context: the tests must run
# on a build agent with no GPU, so anything GPU-bound belongs in benchmarks/.
include(Catch)

add_executable(voxl_tests
    test_chunkstorage.cpp
    test_jobsystem.cpp
    test_log.cpp
    test_mesher.cpp
    test_physics.cpp
    test_terrain.cpp
    test_world.cpp
)

target_link_libraries(voxl_tests PRIVATE
    voxl::engine
    Catch2::Catch2WithMain)

catch_discover_tests(voxl_tests)
