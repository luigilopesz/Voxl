# Offline benchmark harnesses. These are console executables so they can be run
# from CI and have their numbers diffed between commits; see docs/PERFORMANCE.md.
add_executable(voxl_bench bench_main.cpp)
target_link_libraries(voxl_bench PRIVATE voxl::engine)
