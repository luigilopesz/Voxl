// Benchmark driver. Individual scenes register themselves here as they land;
// results are recorded in docs/PERFORMANCE.md.

#include "core/Log.hpp"

#include <chrono>
#include <cstdio>

int main()
{
    voxl::setLogLevel(voxl::LogLevel::Info);
    VOXL_LOG_INFO("Voxl benchmark harness");

    // Placeholder timing loop; replaced by real scenes in the meshing and
    // terrain milestones.
    const auto start = std::chrono::steady_clock::now();
    volatile double accumulator = 0.0;
    for (int i = 0; i < 1'000'000; ++i) {
        accumulator = accumulator + static_cast<double>(i) * 1e-9;
    }
    const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - start;
    VOXL_LOG_INFO("warmup loop: {:.3f} ms", elapsed.count());

    voxl::shutdownLogging();
    return 0;
}
