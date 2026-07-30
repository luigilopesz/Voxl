// Benchmark driver.
//
// Headless by construction: nothing in this executable creates a window or a GL
// context, so it runs on a build machine with no GPU. See docs/PERFORMANCE.md
// for the methodology and the recorded results.

#include "Cases.hpp"
#include "Fixtures.hpp"
#include "Harness.hpp"

#include "core/Log.hpp"
#include "world/Lod.hpp"
#include "world/VoxelTypes.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace {

struct Options {
    std::string           filter;
    std::filesystem::path csvPath = "voxl_bench_results.csv";
    int                   warmup  = -1;
    int                   samples = -1;
    std::uint64_t         seed    = 0;
    bool                  seedSet = false;
    bool                  list    = false;
    bool                  quiet   = false;
    bool                  help    = false;
};

void printUsage()
{
    std::puts(R"(voxl_bench - offline performance harness (headless, no OpenGL)

  --filter <text>    run only cases whose "group/name" contains <text>
  --samples <n>      override every case's timed-run count
  --warmup <n>       override every case's untimed warm-up count
  --seed <n>         world seed for every generated fixture (decimal or 0x...)
  --csv <path>       write the long-format CSV here
                     (default: voxl_bench_results.csv in the working directory)
  --no-csv           skip the CSV
  --list             print the case list and exit
  --quiet            suppress the per-case progress lines
  --help             this text

Groups: terrain, meshing, subvoxel, storage, lighting, persistence.)");
}

/// Parses a decimal or 0x-prefixed unsigned value. Rejects anything the whole
/// string is not, so a typo is an error rather than a silent zero.
bool parseUnsigned(std::string_view text, std::uint64_t& out)
{
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text.remove_prefix(2);
    }
    const char* first  = text.data();
    const char* last   = text.data() + text.size();
    const auto  result = std::from_chars(first, last, out, base);
    return result.ec == std::errc{} && result.ptr == last;
}

bool parseInt(std::string_view text, int& out)
{
    std::uint64_t value = 0;
    if (!parseUnsigned(text, value) || value > 100000u) {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

bool parseArguments(int argc, char** argv, Options& options)
{
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        std::string_view       value;
        const auto             next = [&] {
            if (i + 1 >= argc) {
                return false;
            }
            value = std::string_view(argv[++i]);
            return true;
        };

        if (argument == "--help" || argument == "-h") {
            options.help = true;
        } else if (argument == "--list") {
            options.list = true;
        } else if (argument == "--quiet") {
            options.quiet = true;
        } else if (argument == "--no-csv") {
            options.csvPath.clear();
        } else if (argument == "--filter") {
            if (!next()) return false;
            options.filter = std::string(value);
        } else if (argument == "--csv") {
            if (!next()) return false;
            options.csvPath = std::filesystem::path(value);
        } else if (argument == "--warmup") {
            if (!next() || !parseInt(value, options.warmup)) return false;
        } else if (argument == "--samples") {
            if (!next() || !parseInt(value, options.samples)) return false;
        } else if (argument == "--seed") {
            if (!next() || !parseUnsigned(value, options.seed)) return false;
            options.seedSet = true;
        } else {
            std::fprintf(stderr, "unknown argument: %.*s\n", static_cast<int>(argument.size()),
                         argument.data());
            return false;
        }
    }
    return true;
}

void printBanner(const Options& options)
{
    std::cout << "voxl_bench - headless performance harness\n";
    std::cout << std::format("  build            : {}\n",
#if defined(NDEBUG)
                             "optimised (NDEBUG defined)"
#else
                             "unoptimised (NDEBUG not defined)"
#endif
    );
    std::cout << std::format("  hardware threads : {}\n", std::thread::hardware_concurrency());
    std::cout << std::format("  world seed       : {:#x}\n", voxl::bench::seed());
    std::cout << std::format("  chunk            : {}^3 = {} voxels, {} sections tall\n",
                             voxl::kChunkSize, voxl::kChunkVolume, voxl::kWorldSectionCount);
    std::cout << std::format("  lod levels       : 0..{}\n", static_cast<int>(voxl::kLodMax));
    if (!options.filter.empty()) {
        std::cout << std::format("  filter           : {}\n", options.filter);
    }
    std::cout << "\n";
}

}  // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!parseArguments(argc, argv, options)) {
        printUsage();
        return 2;
    }
    if (options.help) {
        printUsage();
        return 0;
    }

    // Warn and above only. The JobSystem and several subsystems log at Info, and
    // start-up chatter interleaved with the results table would make the output
    // useless as a diff target.
    voxl::setLogLevel(voxl::LogLevel::Warn);

    if (options.seedSet) {
        voxl::bench::setSeed(options.seed);
    }

    voxl::bench::RunnerConfig config;
    config.filter         = options.filter;
    config.warmupOverride = options.warmup;
    config.sampleOverride = options.samples;
    config.verbose        = !options.quiet;

    voxl::bench::Runner runner(config);

    int exitCode = 0;
    try {
        printBanner(options);

        std::cout << "Building fixtures and registering cases...\n";
        std::cout.flush();
        voxl::bench::registerTerrainCases(runner);
        voxl::bench::registerMeshingCases(runner);
        voxl::bench::registerSubVoxelCases(runner);
        voxl::bench::registerStorageCases(runner);
        voxl::bench::registerLightingCases(runner);
        voxl::bench::registerPersistenceCases(runner);

        if (options.list) {
            runner.listCases(std::cout);
            voxl::shutdownLogging();
            return 0;
        }

        std::cout << "Running...\n";
        std::cout.flush();
        runner.runAll();

        voxl::bench::reportTerrainDerived(runner);
        voxl::bench::reportMeshingDerived(runner);

        runner.printTable(std::cout);

        if (!options.csvPath.empty()) {
            if (runner.writeCsv(options.csvPath)) {
                std::cout << "CSV written to "
                          << std::filesystem::absolute(options.csvPath).string() << "\n";
            } else {
                std::cerr << "failed to write CSV to " << options.csvPath.string() << "\n";
                exitCode = 1;
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "benchmark aborted: " << error.what() << "\n";
        exitCode = 1;
    }

    voxl::shutdownLogging();
    return exitCode;
}
