#include "cli.hpp"

#include <charconv>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

namespace {
    /// Split "1.5,-2,3" into floats. Returns false on anything it cannot parse completely, so a
    /// typo in a benchmark script fails loudly at startup instead of silently producing a run
    /// from the wrong camera -- which would be indistinguishable from a rendering regression.
    auto parse_floats(std::string_view text, size_t expected, float *out) -> bool {
        size_t count = 0;
        size_t start = 0;
        while (start <= text.size()) {
            auto comma = text.find(',', start);
            auto piece = text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
            if (piece.empty() || count >= expected) {
                return false;
            }
            // std::from_chars for floats is present in MSVC 19.2x+ and is locale-independent,
            // which strtof is not: a machine with a comma decimal separator would otherwise
            // parse "1.5" as 1.
            auto const *first = piece.data();
            auto const *last = piece.data() + piece.size();
            auto result = std::from_chars(first, last, out[count]);
            if (result.ec != std::errc{} || result.ptr != last) {
                return false;
            }
            ++count;
            if (comma == std::string_view::npos) {
                break;
            }
            start = comma + 1;
        }
        return count == expected;
    }

    auto parse_u64(std::string_view text, uint64_t &out) -> bool {
        auto const *first = text.data();
        auto const *last = text.data() + text.size();
        auto result = std::from_chars(first, last, out);
        return result.ec == std::errc{} && result.ptr == last;
    }

    auto parse_u32(std::string_view text, uint32_t &out) -> bool {
        uint64_t wide = 0;
        if (!parse_u64(text, wide) || wide == 0 || wide > 16384) {
            return false;
        }
        out = static_cast<uint32_t>(wide);
        return true;
    }
} // namespace

auto AppCli::get() -> AppCli & {
    static AppCli instance{};
    return instance;
}

void AppCli::parse(int argc, char const *const *argv) {
    auto &cli = get();
    // FATAL, not "ignored". This was a warning for exactly one afternoon, and in that time a
    // locale difference (pt-BR renders 26.5 as "26,5", which --pos then reads as four fields)
    // produced two capture runs framed from the default spawn while the *rotation* argument beside
    // it was accepted. Both images looked entirely reasonable. A measurement tool that quietly
    // falls back to a different camera than the one requested is worse than one that will not start.
    auto bad = [&cli](char const *flag, char const *value) {
        std::fprintf(stderr, "[cli] FATAL: bad value for %s: '%s'\n", flag, value != nullptr ? value : "(missing)");
        cli.parse_failed = true;
    };

    for (int i = 1; i < argc; ++i) {
        auto arg = std::string_view{argv[i]};
        // Every option below that takes a value uses this; `next` is nullptr at the end of argv
        // so a trailing "--pos" with no argument is a diagnostic rather than a read past the end.
        char const *next = (i + 1 < argc) ? argv[i + 1] : nullptr;
        auto take = [&]() -> char const * {
            if (next != nullptr) {
                ++i;
            }
            return next;
        };

        if (arg == "--pos") {
            auto *value = take();
            std::array<float, 3> v{};
            if (value != nullptr && parse_floats(value, 3, v.data())) {
                cli.pos = v;
            } else {
                bad("--pos", value);
            }
        } else if (arg == "--rot") {
            auto *value = take();
            std::array<float, 2> v{};
            if (value != nullptr && parse_floats(value, 2, v.data())) {
                cli.rot = v;
            } else {
                bad("--rot", value);
            }
        } else if (arg == "--patrol") {
            auto *value = take();
            std::array<float, 2> v{};
            if (value != nullptr && parse_floats(value, 2, v.data()) && v[0] > 0.0f && v[1] > 0.0f) {
                cli.patrol = v;
            } else {
                bad("--patrol", value);
            }
        } else if (arg == "--exit-after") {
            auto *value = take();
            float v = 0.0f;
            if (value != nullptr && parse_floats(value, 1, &v) && v > 0.0f) {
                cli.exit_after = v;
            } else {
                bad("--exit-after", value);
            }
        } else if (arg == "--screenshot") {
            auto *value = take();
            if (value != nullptr) {
                cli.screenshot_path = value;
            } else {
                bad("--screenshot", value);
            }
        } else if (arg == "--screenshot-after") {
            auto *value = take();
            float v = 0.0f;
            if (value != nullptr && parse_floats(value, 1, &v) && v >= 0.0f) {
                cli.screenshot_after = v;
            } else {
                bad("--screenshot-after", value);
            }
        } else if (arg == "--bench-csv") {
            auto *value = take();
            if (value != nullptr) {
                cli.bench_csv = value;
            } else {
                bad("--bench-csv", value);
            }
        } else if (arg == "--width") {
            auto *value = take();
            if (value == nullptr || !parse_u32(value, cli.width)) {
                bad("--width", value);
            }
        } else if (arg == "--height") {
            auto *value = take();
            if (value == nullptr || !parse_u32(value, cli.height)) {
                bad("--height", value);
            }
        } else if (arg == "--seed") {
            auto *value = take();
            uint64_t v = 0;
            if (value != nullptr && parse_u64(value, v)) {
                cli.seed = v;
            } else {
                bad("--seed", value);
            }
        } else if (arg == "--overlay") {
            cli.overlay = 1;
        } else if (arg == "--no-overlay") {
            cli.overlay = 0;
        } else if (arg == "--unpause") {
            cli.unpause = true;
        } else if (arg == "--expand-graphs") {
            cli.expand_graphs = true;
        } else if (arg == "--lock-camera") {
            cli.lock_camera = true;
            cli.lock_camera_explicit = true;
        } else if (arg == "--no-lock-camera") {
            cli.lock_camera = false;
            cli.lock_camera_explicit = true;
        } else if (arg == "--help" || arg == "-h") {
            cli.help_requested = true;
            std::fprintf(stdout,
                         "gvox_engine (voxl2)\n"
                         "  --pos X,Y,Z            player position, absolute world metres\n"
                         "  --rot YAW,PITCH        radians; pitch 1.571 is level\n"
                         "  --patrol RADIUS,PERIOD circle about --pos, one lap per PERIOD seconds\n"
                         "  --lock-camera          freeze the pose and ignore input (implied by --pos)\n"
                         "  --no-lock-camera       start at --pos but stay controllable\n"
                         "  --exit-after SECONDS   quit cleanly after this long\n"
                         "  --screenshot PATH      write the swapchain to a PNG\n"
                         "  --screenshot-after S   when to take it (default: 1 s before --exit-after)\n"
                         "  --bench-csv PATH       one row per frame: time, frame time, heap, pose\n"
                         "  --width N --height N   window size (default 1280x720)\n"
                         "  --seed N               world seed\n"
                         "  --overlay/--no-overlay force the F3 debug overlay on or off\n"
                         "  --expand-graphs        force the overlay's frame-time graphs open\n"
                         "  --unpause              start with the game running, not in the menu\n");
        } else {
            std::fprintf(stderr, "[cli] unknown argument '%s' -- ignored\n", argv[i]);
        }
    }

    // --pos without --lock-camera is almost always a mistake: the pose would be the *initial*
    // one and any stray mouse motion would move it. Opting out is explicit.
    if (cli.pos.has_value() && !cli.lock_camera_explicit && !cli.patrol.has_value()) {
        cli.lock_camera = true;
    }
    // A screenshot with no time given is taken 1 s before the run ends, so it captures the most
    // converged frame available. The renderer accumulates: an irradiance-cache probe fires one
    // bounce per frame, so an early frame of a dark interior is noise, not a picture of the scene.
    if (!cli.screenshot_path.empty() && cli.screenshot_after < 0.0f) {
        cli.screenshot_after = (cli.exit_after > 1.0f) ? cli.exit_after - 1.0f : 20.0f;
    }
}
