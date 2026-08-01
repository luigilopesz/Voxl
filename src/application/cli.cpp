#include "cli.hpp"

// For BRUSH_ID_* and BRUSH_ID_NAMES: --edit resolves a brush name to an id at PARSE time, so a
// typo is a startup diagnostic rather than a 30-second run that edits the wrong thing. input.inl
// is what brings daxa's scalar typedefs in scope for the shared header.
#include <application/input.inl>

#include <array>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>
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

    /// Split "Graphics/Render Res Scale=0.75" for --set. Returns false unless both separators are
    /// present and all three pieces are non-empty, so a typo cannot silently become a no-op.
    auto parse_qualified_set(std::string_view text, SettingOverride &out) -> bool {
        auto slash = text.find('/');
        auto equals = text.find('=');
        if (slash == std::string_view::npos || equals == std::string_view::npos || slash > equals) {
            return false;
        }
        auto category = text.substr(0, slash);
        auto id = text.substr(slash + 1, equals - slash - 1);
        auto value = text.substr(equals + 1);
        if (category.empty() || id.empty() || value.empty()) {
            return false;
        }
        out.category = std::string{category};
        out.id = std::string{id};
        out.value = std::string{value};
        return true;
    }

    /// Case- and separator-insensitive compare, so "remove-terrain", "Remove terrain" and
    /// "remove_terrain" all name the same brush. The canonical names come from BRUSH_ID_NAMES in
    /// voxels/brushes.inl, which is the enumeration's own label table -- restating them here would
    /// give the CLI a second copy of a list that is already declared to be part of the interface.
    auto loose_equal(std::string_view a, std::string_view b) -> bool {
        auto norm = [](char c) -> char {
            if (c == '-' || c == '_') {
                return ' ';
            }
            return static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
        };
        if (a.size() != b.size()) {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i) {
            if (norm(a[i]) != norm(b[i])) {
                return false;
            }
        }
        return true;
    }

    /// Resolve a brush by name or by index. Returns BRUSH_ID_COUNT on failure.
    auto resolve_brush(std::string_view text) -> uint32_t {
        for (uint32_t id = 0; id < BRUSH_ID_COUNT; ++id) {
            if (loose_equal(text, BRUSH_ID_NAMES[id])) {
                return id;
            }
        }
        uint64_t index = 0;
        if (parse_u64(text, index) && index < BRUSH_ID_COUNT) {
            return static_cast<uint32_t>(index);
        }
        return BRUSH_ID_COUNT;
    }

    /// Split "12,add terrain,2.5,0.5,rmb" for --edit. Only the first three fields are required.
    auto parse_edit(std::string_view text, ScriptedEdit &out) -> bool {
        std::string_view field[5];
        size_t count = 0;
        size_t start = 0;
        bool overflow = false;
        while (start <= text.size()) {
            auto comma = text.find(',', start);
            auto piece = text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start);
            if (count >= 5) {
                overflow = true;
                break;
            }
            field[count++] = piece;
            if (comma == std::string_view::npos) {
                break;
            }
            start = comma + 1;
        }
        // Fewer than three fields, or a sixth, means the caller meant something this does not do.
        if (overflow || count < 3) {
            return false;
        }
        float scratch = 0.0f;
        if (!parse_floats(field[0], 1, &scratch) || scratch < 0.0f) {
            return false;
        }
        out.at_s = scratch;
        out.brush_id = resolve_brush(field[1]);
        if (out.brush_id >= BRUSH_ID_COUNT) {
            return false;
        }
        if (!parse_floats(field[2], 1, &scratch)) {
            return false;
        }
        out.radius = scratch;
        if (count >= 4) {
            if (!parse_floats(field[3], 1, &scratch) || scratch <= 0.0f) {
                return false;
            }
            out.hold_s = scratch;
        }
        if (count >= 5) {
            if (loose_equal(field[4], "rmb") || loose_equal(field[4], "secondary") || loose_equal(field[4], "b")) {
                out.secondary = true;
            } else if (loose_equal(field[4], "lmb") || loose_equal(field[4], "primary") || loose_equal(field[4], "a")) {
                out.secondary = false;
            } else {
                return false;
            }
        }
        out.origin = std::string{text};
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
        } else if (arg == "--render-scale" || arg == "--taa" || arg == "--gi" ||
                   arg == "--reflections" || arg == "--shadows" || arg == "--update-sky" ||
                   arg == "--denoise-shadow-mask") {
            // F3. These are sugar over --set: each names one registered Graphics entry. The value
            // is NOT validated here -- the registry does not exist yet (see SettingOverride in
            // cli.hpp), so a bad value is reported by name at apply time instead.
            //
            // "--reflections" targets Graphics/ray_traced_reflections, which is item F7 and is not
            // in this tree yet. It is listed anyway, deliberately: the brief asks for the flag, the
            // apply path reports an unknown setting rather than ignoring it, and the flag starts
            // working the moment F7 lands with no change here.
            static constexpr std::array<std::pair<std::string_view, std::string_view>, 7> TABLE{{
                {"--render-scale", "Render Res Scale"},
                {"--taa", "TAA Method"},
                {"--gi", "global_illumination"},
                {"--reflections", "ray_traced_reflections"},
                {"--shadows", "Render Shadows"},
                {"--update-sky", "Update Sky"},
                {"--denoise-shadow-mask", "denoise_shadow_mask"},
            }};
            auto *value = take();
            if (value == nullptr || *value == '\0') {
                bad(std::string{arg}.c_str(), value);
            } else {
                for (auto const &[flag, id] : TABLE) {
                    if (arg == flag) {
                        cli.setting_overrides.push_back({"Graphics", std::string{id}, value, std::string{arg}});
                        break;
                    }
                }
            }
        } else if (arg == "--set") {
            // The general escape hatch: --set "Category/Id=value" reaches any registered setting,
            // including ones added after this file was written.
            auto *value = take();
            SettingOverride override_entry{};
            if (value != nullptr && parse_qualified_set(value, override_entry)) {
                override_entry.origin = "--set";
                cli.setting_overrides.push_back(override_entry);
            } else {
                bad("--set", value);
            }
        } else if (arg == "--edit") {
            auto *value = take();
            ScriptedEdit edit{};
            if (value != nullptr && parse_edit(value, edit)) {
                cli.edits.push_back(std::move(edit));
            } else {
                bad("--edit", value);
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
                         "  --unpause              start with the game running, not in the menu\n"
                         "\n"
                         "Quality settings (applied on the first frame; no JSON patching needed):\n"
                         "  --render-scale F       internal resolution scale, 0.2 to 4.0\n"
                         "  --taa MODE             none | kajiya | fsr2, or an index\n"
                         "  --gi BOOL              path-traced global illumination\n"
                         "  --reflections BOOL     ray-traced reflections (needs F7; warns until then)\n"
                         "  --shadows BOOL         sun shadow trace\n"
                         "  --update-sky BOOL      re-render the sky LUTs each frame\n"
                         "  --denoise-shadow-mask BOOL\n"
                         "  --set 'Cat/Id=value'   any registered setting, repeatable\n"
                         "BOOL accepts on/off, true/false, yes/no, 1/0.\n"
                         "\n"
                         "Scripted editing (no synthetic input; implies --unpause):\n"
                         "  --edit T,BRUSH,RADIUS[,HOLD][,rmb]   repeatable\n"
                         "      T       seconds from the first frame at which to press\n"
                         "      BRUSH   name or index, 0..10:\n"
                         "              remove-terrain add-terrain grass remove-grass flowers\n"
                         "              light-ball lantern fire torch maple-tree spruce-tree\n"
                         "      RADIUS  metres, 0.125 to 8 (trees cap at 6)\n"
                         "      HOLD    seconds to hold the button (default 0.25)\n"
                         "      rmb     press the secondary button (removes terrain) instead\n"
                         "  Example: --edit 12,add-terrain,2.5 --edit 16,maple-tree,4,0.4\n"
                         "\n"
                         "Environment:\n"
                         "  VOXL_DATA_DIR          settings directory for THIS process. Unset, every\n"
                         "                         build on the machine shares one user_settings.json\n"
                         "                         and concurrent runs silently overwrite each other.\n");
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
    // --edit implies --unpause. The pause menu captures the cursor back and hides the tool HUD,
    // so a scripted edit taken while paused would apply but be invisible in the capture -- and an
    // invisible edit is indistinguishable from one that did not happen, which is the failure mode
    // this whole flag exists to eliminate.
    if (cli.edits_scripted()) {
        cli.unpause = true;
    }
}
