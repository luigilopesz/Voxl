// Voxl entry point.
//
// Main does nothing but establish logging, parse the debug command line,
// construct the Application and translate an escaping exception into a logged
// failure and a non-zero exit code. Every subsystem lives in Application so that
// construction and - more importantly - destruction order is stated in one place.

#include "app/Application.hpp"
#include "core/Log.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string_view>
#include <system_error>

namespace {

// ---------------------------------------------------------- command line --
//
// EVERY OPTION HERE IS A DEBUG AFFORDANCE, not a user-facing feature. It exists
// so the visual review harness can reproduce a framing exactly - see
// voxl::DebugStartup and docs/VISUAL_REVIEW.md. With no arguments the game runs
// exactly as it did before the flags existed, which is why nothing is validated
// beyond "did it parse": a typo in a review script should be loud in the log, not
// a silent half-applied configuration.

/// Splits `text` on commas and parses each field. Fails unless the whole string
/// is consumed by exactly `count` values, so "--pos 1,2" and "--pos 1,2,3,4" are
/// both rejected rather than quietly filling in a zero.
template <typename T>
[[nodiscard]] bool parseList(std::string_view text, T* out, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t      comma = text.find(',');
        const std::string_view field = text.substr(0, comma);
        const char*            begin = field.data();
        const char*            end   = field.data() + field.size();

        const std::from_chars_result result = std::from_chars(begin, end, out[i]);
        if (result.ec != std::errc{} || result.ptr != end) {
            return false;
        }
        if (comma == std::string_view::npos) {
            return i + 1 == count;
        }
        text.remove_prefix(comma + 1);
    }
    return false;  // more fields than expected
}

[[nodiscard]] bool parseInt(std::string_view text, std::int32_t& out)
{
    return parseList(text, &out, 1);
}

void logUsage()
{
    VOXL_LOG_INFO(
        "voxl [debug options]\n"
        "  --pos X,Y,Z          place the player's feet here instead of the spawn point\n"
        "  --look YAW,PITCH     degrees; yaw 0 faces -Z, 180 faces +Z, 270 faces +X\n"
        "  --freeze             suspend player physics so the camera cannot drift\n"
        "  --overlay            open the F3 panel at startup\n"
        "  --no-hud             hide the hotbar and crosshair\n"
        "  --lod-off            pin every chunk to level 0\n"
        "  --lod-bands A,B,C    LodPolicy::bandStart\n"
        "  --radius N           streaming load radius in chunks\n"
        "  --seed N             terrain seed\n"
        "  --warmup-ms N        milliseconds of streaming before the first frame\n"
        "  --carve WHAT         none | crater | tunnel | both\n"
        "  --carve-at X,Y,Z     anchor block for the carve rig");
}

/// Exit rather than Run after `--help`, so asking for the usage text does not
/// also open a window and start streaming a world.
enum class ParseResult : std::uint8_t { Run, Exit, Failed };

/// Returns Failed when an argument was malformed, which aborts startup: a review
/// shot taken with a silently ignored flag is worse than no shot.
[[nodiscard]] ParseResult parseArguments(int argc, char** argv, voxl::ApplicationConfig& config)
{
    voxl::DebugStartup& debug    = config.debug;
    bool                helpOnly = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument{argv[i]};

        const auto value = [&](std::string_view& out) {
            if (i + 1 >= argc) {
                return false;
            }
            out = std::string_view{argv[++i]};
            return true;
        };

        std::string_view text;
        if (argument == "--help" || argument == "-h") {
            logUsage();
            helpOnly = true;
            continue;
        }
        if (argument == "--freeze") {
            debug.freezePlayer = true;
            continue;
        }
        if (argument == "--overlay") {
            debug.showOverlay = true;
            continue;
        }
        if (argument == "--no-hud") {
            debug.hideHud = true;
            continue;
        }
        if (argument == "--lod-off") {
            debug.lodEnabled = false;
            continue;
        }
        if (argument == "--pos") {
            float values[3]{};
            if (!value(text) || !parseList(text, values, 3)) {
                VOXL_LOG_ERROR("--pos wants X,Y,Z");
                return ParseResult::Failed;
            }
            debug.hasPosition = true;
            debug.position    = glm::vec3{values[0], values[1], values[2]};
            continue;
        }
        if (argument == "--look") {
            float values[2]{};
            if (!value(text) || !parseList(text, values, 2)) {
                VOXL_LOG_ERROR("--look wants YAW,PITCH");
                return ParseResult::Failed;
            }
            debug.hasRotation  = true;
            debug.yawDegrees   = values[0];
            debug.pitchDegrees = values[1];
            continue;
        }
        if (argument == "--lod-bands") {
            if (!value(text) || !parseList(text, debug.bandStart, std::size_t{voxl::kLodMax})) {
                VOXL_LOG_ERROR("--lod-bands wants {} ascending distances",
                               static_cast<int>(voxl::kLodMax));
                return ParseResult::Failed;
            }
            debug.hasBands = true;
            continue;
        }
        if (argument == "--carve-at") {
            std::int32_t values[3]{};
            if (!value(text) || !parseList(text, values, 3)) {
                VOXL_LOG_ERROR("--carve-at wants X,Y,Z");
                return ParseResult::Failed;
            }
            debug.hasCarveAnchor = true;
            debug.carveAnchor    = voxl::BlockPos{values[0], values[1], values[2]};
            continue;
        }
        if (argument == "--carve") {
            if (!value(text)) {
                VOXL_LOG_ERROR("--carve wants none|crater|tunnel|both");
                return ParseResult::Failed;
            }
            if (text == "none") {
                debug.carve = voxl::DebugStartup::Carve::None;
            } else if (text == "crater") {
                debug.carve = voxl::DebugStartup::Carve::Crater;
            } else if (text == "tunnel") {
                debug.carve = voxl::DebugStartup::Carve::Tunnel;
            } else if (text == "both") {
                debug.carve = voxl::DebugStartup::Carve::Both;
            } else {
                VOXL_LOG_ERROR("--carve wants none|crater|tunnel|both, got '{}'", text);
                return ParseResult::Failed;
            }
            continue;
        }
        if (argument == "--radius") {
            std::int32_t radius = 0;
            if (!value(text) || !parseInt(text, radius) || radius < 1) {
                VOXL_LOG_ERROR("--radius wants a positive chunk count");
                return ParseResult::Failed;
            }
            config.streaming.loadRadius = radius;
            continue;
        }
        if (argument == "--seed") {
            std::int32_t seed = 0;
            if (!value(text) || !parseInt(text, seed)) {
                VOXL_LOG_ERROR("--seed wants an integer");
                return ParseResult::Failed;
            }
            config.terrain.seed = static_cast<std::uint32_t>(seed);
            continue;
        }
        if (argument == "--warmup-ms") {
            std::int32_t milliseconds = 0;
            if (!value(text) || !parseInt(text, milliseconds) || milliseconds < 0) {
                VOXL_LOG_ERROR("--warmup-ms wants a non-negative millisecond count");
                return ParseResult::Failed;
            }
            config.warmupTimeout = std::chrono::milliseconds{milliseconds};
            continue;
        }

        VOXL_LOG_ERROR("Unknown option '{}'", argument);
        logUsage();
        return ParseResult::Failed;
    }
    return helpOnly ? ParseResult::Exit : ParseResult::Run;
}

}  // namespace

int main(int argc, char** argv)
{
    voxl::setLogFile("voxl.log");
    VOXL_LOG_INFO("Voxl starting up");

    voxl::ApplicationConfig config;
    switch (parseArguments(argc, argv, config)) {
        case ParseResult::Run:
            break;
        case ParseResult::Exit:
            voxl::shutdownLogging();
            return 0;
        case ParseResult::Failed:
            VOXL_LOG_FATAL("Bad command line");
            voxl::shutdownLogging();
            return 2;
    }

    int exitCode = 1;
    try {
        voxl::Application application{config};
        exitCode = application.run();
    } catch (const std::exception& error) {
        VOXL_LOG_FATAL("Unhandled exception: {}", error.what());
    } catch (...) {
        VOXL_LOG_FATAL("Unhandled non-standard exception");
    }

    VOXL_LOG_INFO("Voxl exiting with code {}", exitCode);
    voxl::shutdownLogging();
    return exitCode;
}
