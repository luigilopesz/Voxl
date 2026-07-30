# Offline benchmark harness. A console executable so it can be run from CI and
# have its numbers diffed between commits; see docs/PERFORMANCE.md.
#
# HEADLESS BY CONSTRUCTION. Nothing here creates a window or a GL context, so
# the target runs on a build machine with no GPU. It links voxl::engine (which
# transitively pulls in GLFW and glad) only because the engine is one static
# library; no entry point from either is ever called.
#
# Sources are listed explicitly, never globbed, for the same reason as in
# src/CMakeLists.txt: adding a file should be a reviewable change.
add_executable(voxl_bench
    bench_main.cpp

    Harness.cpp
    Fixtures.cpp

    bench_terrain.cpp
    bench_meshing.cpp
    bench_subvoxel.cpp
    bench_storage.cpp
    bench_lighting.cpp
    bench_persistence.cpp
)

target_include_directories(voxl_bench PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(voxl_bench PRIVATE voxl::engine)

# ------------------------------------------------- cases with soft dependencies --
#
# bench_lighting.cpp and bench_persistence.cpp reach for headers that arrive on
# their own schedule (world/LightEngine.hpp, world/WorldSave.hpp) and guard the
# include with __has_include, so a MISSING HEADER is already handled in C++.
#
# The case __has_include cannot see is the awkward middle state: the header is in
# the tree but its .cpp has not been added to src/CMakeLists.txt yet. That
# compiles and then fails to LINK, which would break the build for everyone else
# for a reason that has nothing to do with them. So the engine's own source list
# is the authority here: a case is compiled out unless the translation unit that
# defines it is actually part of voxl::engine.
#
# This block is self-retiring. The moment integration adds the source to
# src/CMakeLists.txt the detection flips and the case builds with no edit here.
file(READ "${CMAKE_SOURCE_DIR}/src/CMakeLists.txt" _voxl_engine_source_list)

function(voxl_bench_soft_dependency out_disabled source_relative_path)
    if(_voxl_engine_source_list MATCHES "${source_relative_path}")
        set(${out_disabled} OFF PARENT_SCOPE)
    else()
        set(${out_disabled} ON PARENT_SCOPE)
        message(STATUS
            "voxl_bench: ${source_relative_path} is not built into voxl::engine; "
            "the benchmark case that needs it is compiled out")
    endif()
endfunction()

voxl_bench_soft_dependency(_bench_no_light_engine "world/LightEngine.cpp")
voxl_bench_soft_dependency(_bench_no_world_save   "world/WorldSave.cpp")

# Manual overrides, for when the source IS linked but its API has moved on and
# the case no longer compiles against it.
option(VOXL_BENCH_NO_LIGHT_ENGINE "Force the lighting benchmark case off"    OFF)
option(VOXL_BENCH_NO_WORLD_SAVE   "Force the persistence benchmark case off" OFF)

# There used to be an escape hatch here (VOXL_BENCH_LINK_PENDING_SOURCES) that
# compiled world/LightEngine.cpp and world/WorldSave.cpp straight into this
# target, so the lighting and persistence cases could be measured while those
# files existed but were not yet in src/CMakeLists.txt. Both are now part of
# voxl::engine, so the detection above resolves them and the hatch would only
# hand the linker a second definition of every symbol in them. Removed rather
# than left switched off.

if(VOXL_BENCH_NO_LIGHT_ENGINE)
    set(_bench_no_light_engine ON)
endif()
if(VOXL_BENCH_NO_WORLD_SAVE)
    set(_bench_no_world_save ON)
endif()

target_compile_definitions(voxl_bench PRIVATE
    $<$<BOOL:${_bench_no_light_engine}>:VOXL_BENCH_NO_LIGHT_ENGINE>
    $<$<BOOL:${_bench_no_world_save}>:VOXL_BENCH_NO_WORLD_SAVE>)

if(MSVC)
    # A console tool: the game target's /SUBSYSTEM:WINDOWS choice must not follow
    # it here or the results table would go nowhere.
    target_link_options(voxl_bench PRIVATE /SUBSYSTEM:CONSOLE)
endif()
