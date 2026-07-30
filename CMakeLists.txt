cmake_minimum_required(VERSION 3.24)

project(Voxl
    VERSION 0.1.0
    DESCRIPTION "A first-person voxel sandbox engine"
    LANGUAGES C CXX)

# ---------------------------------------------------------------- options --
option(VOXL_BUILD_TESTS      "Build the automated test suite"        ON)
option(VOXL_BUILD_BENCHMARKS "Build the offline benchmark harnesses" ON)
option(VOXL_WARNINGS_AS_ERRORS "Treat compiler warnings as errors"   OFF)
option(VOXL_ENABLE_ASAN      "Build with AddressSanitizer"           OFF)

# ---------------------------------------------------------------- language --
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

# Single output dir keeps the runtime next to the assets it loads.
foreach(_cfg "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY${_cfg} "${CMAKE_BINARY_DIR}/bin")
endforeach()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# ------------------------------------------------------------ dependencies --
include(cmake/Dependencies.cmake)

# ------------------------------------------ shared compile settings target --
# Every first-party target links this; third-party targets never do.
add_library(voxl_compile_options INTERFACE)
add_library(voxl::compile_options ALIAS voxl_compile_options)

if(MSVC)
    target_compile_options(voxl_compile_options INTERFACE
        /W4                 # high warning level
        /permissive-        # strict conformance
        /Zc:preprocessor    # conforming preprocessor
        /Zc:__cplusplus     # report the real __cplusplus value
        /utf-8
        /MP                 # parallel compilation within a target
        /wd4324             # structure padded due to alignas - intentional
        /external:W0        # silence warnings from SYSTEM includes
        $<$<BOOL:${VOXL_WARNINGS_AS_ERRORS}>:/WX>)
    target_compile_definitions(voxl_compile_options INTERFACE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
        _SILENCE_CXX20_CISO646_REMOVED_WARNING)
    # Fast math is unacceptable for a world that must regenerate bit-identically
    # from a seed; keep strict FP everywhere.
    target_compile_options(voxl_compile_options INTERFACE /fp:strict)
else()
    target_compile_options(voxl_compile_options INTERFACE
        -Wall -Wextra -Wpedantic
        $<$<BOOL:${VOXL_WARNINGS_AS_ERRORS}>:-Werror>)
endif()

if(VOXL_ENABLE_ASAN)
    if(MSVC)
        target_compile_options(voxl_compile_options INTERFACE /fsanitize=address)
        # Only first-party targets link voxl::compile_options, so Catch2, imgui
        # and glfw are built WITHOUT /fsanitize=address. MSVC's ASan turns on
        # std::vector/std::string container annotations, and mixing annotated and
        # unannotated translation units is a hard link error, not a warning:
        #   Catch2.lib(...) : error LNK2038: mismatch detected for
        #   'annotate_vector': value '0' doesn't match value '1'
        # which stopped voxl_tests linking at all, i.e. the option could not be
        # used for the one target it is most wanted on. Turning the annotations
        # off puts both halves on value 0. The cost is container-overflow
        # detection inside std::vector/std::string; every other ASan check -
        # use-after-free, which is the one this codebase's threading rules are
        # actually about - is unaffected.
        target_compile_definitions(voxl_compile_options INTERFACE
            _DISABLE_STRING_ANNOTATION=1
            _DISABLE_VECTOR_ANNOTATION=1)
    else()
        target_compile_options(voxl_compile_options INTERFACE -fsanitize=address)
        target_link_options(voxl_compile_options INTERFACE -fsanitize=address)
    endif()
endif()

# Debug builds get assertions; everything else compiles them out.
target_compile_definitions(voxl_compile_options INTERFACE
    $<$<CONFIG:Debug>:VOXL_DEBUG=1>
    $<$<NOT:$<CONFIG:Debug>>:VOXL_DEBUG=0>)

# ----------------------------------------------------------------- engine --
add_subdirectory(src)

if(VOXL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(VOXL_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

# Assets live beside the executable so the shipped layout matches the dev one.
add_custom_target(voxl_copy_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets" "${CMAKE_BINARY_DIR}/bin/assets"
    COMMENT "Syncing assets to the runtime directory")
add_dependencies(voxl voxl_copy_assets)

message(STATUS "Voxl ${PROJECT_VERSION} | tests=${VOXL_BUILD_TESTS} benchmarks=${VOXL_BUILD_BENCHMARKS}")
