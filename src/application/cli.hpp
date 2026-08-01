#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------------------------
// Command-line control for the engine.
// ---------------------------------------------------------------------------------------------
// WHY THIS FILE EXISTS. Upstream's entry point is `auto main() -> int` -- no argc, no argv, and
// a grep for argc/argv/GetCommandLine across src/ and deps/Daxa/src returns nothing. Every
// number in docs/BASELINE.md and docs/SCENE.md was therefore produced by launching the app,
// sending synthetic keystrokes at it, waiting, screenshotting the window, and reading the frame
// time off the *image* with human eyes. That harness works, but it has three properties that
// make a measurement project impossible to run:
//
//   1. The camera is wherever the keystrokes happened to leave it. Two runs of the same command
//      do not frame the same thing, so "is this build faster?" and "did this change the look?"
//      cannot both be answered from one pair of runs. A reproducible camera pose is the single
//      thing that makes every later comparison possible, which is why --pos/--rot come first.
//   2. Frame time existed only inside std::array<float,200> in AppUi, consumed once by
//      ImGui::PlotLines inside a *collapsed* tree node. There was no percentile, no sample
//      count, and no way to notice that an average hid a 60 ms hitch.
//   3. Nothing could end a run except a WM_CLOSE from outside, so "run for exactly 30 s" was
//      really "run for about 30 s plus however long shader compilation took today".
//
// The parser is a hand-rolled loop over argv with string compares. That is deliberate: adding a
// CLI library to a 32-package vcpkg manifest whose baseline is from 2023 would cost more build
// time than every option here is worth, and this is the entire surface.
//
// Nothing here changes behaviour unless a flag is passed. With no arguments the engine starts
// exactly as upstream did.

/// F3 -- one requested change to a registered AppSettings entry.
///
/// WHY A STRING RATHER THAN A TYPED VALUE. The settings registry is populated by
/// `AppSettings::add()` calls scattered across voxel_app.cpp and six renderer headers, all of
/// which run *after* the CLI is parsed, so at parse time neither the setting's type nor (for a
/// combo box) its list of legal option names is known yet. Overrides are therefore recorded
/// verbatim and interpreted in `AppUi::update()` once the registry is complete -- which also
/// means the value is validated against the type the engine actually registered rather than
/// against this file's assumption about it.
struct SettingOverride {
    std::string category; ///< e.g. "Graphics"
    std::string id;       ///< e.g. "Render Res Scale"
    std::string value;    ///< uninterpreted; parsed against the registered type at apply time
    std::string origin;   ///< the flag that produced this, so a diagnostic can name it
};

/// --edit -- one scripted brush stroke, applied without any synthetic input.
///
/// WHY THIS EXISTS, and why it is worth a CLI flag. Verifying the editing system means showing a
/// before and an after of the same world from the same pose, for eleven brushes. Driving that
/// with synthetic mouse and keyboard events -- the only way to do it before this -- failed in
/// three separate ways during this project, and every one of them produced a *plausible*
/// screenshot rather than an error:
///   * `SetForegroundWindow` is refused to a background process, so keystrokes are silently
///     dropped and the capture shows the DEFAULT tool having done nothing;
///   * `Process.MainWindowHandle` returns the console window for this /subsystem:console build,
///     so input goes to the wrong window;
///   * `GetForegroundWindow()` is 0 on this desktop, which drops `keybd_event` while
///     `mouse_event` still lands -- i.e. the click happens with the wrong brush selected.
/// A dropped keystroke is indistinguishable from a brush that does nothing, which is precisely
/// the thing under test. This flag removes the input layer from the experiment: the stroke is
/// applied by the same code path the mouse drives, one frame at a time, at a scripted time.
struct ScriptedEdit {
    float at_s = 0.0f;     ///< seconds from the first frame at which to press
    float hold_s = 0.25f;  ///< how long to hold. Longer than one frame: see the note in voxel_app.
    uint32_t brush_id = 0; ///< BRUSH_ID_*, resolved from a name at parse time
    float radius = 2.0f;   ///< metres, clamped to BRUSH_RADIUS_MIN/MAX by the shader
    bool secondary = false;///< press the secondary (right) button instead of the primary
    std::string origin;    ///< the argument text, so a diagnostic can quote it back
};

struct AppCli {
    // --pos X,Y,Z -- player position in ABSOLUTE world metres, i.e. exactly the sum the debug
    // overlay shows as `Player Unit Offset` + `Player Pos`. The camera sits 0.2 m below this
    // (application/player.cpp, `cam_pos`), which matters when framing a shot to the voxel.
    std::optional<std::array<float, 3>> pos;

    // --rot YAW,PITCH in radians, matching the overlay's `Player Rot (Y/P/R)`. 1.571 (pi/2) is
    // level; SMALLER LOOKS DOWN and LARGER LOOKS UP.
    //
    // This comment used to say the opposite, and the sign error is easy to make because "pitch
    // measured from +Z" sounds like it should grow downward. It does not: player.cpp:92 sets the
    // startup pitch to M_PI * 0.349 = 1.0964, and docs/SCENE.md documents that startup view as 27
    // degrees BELOW the horizon -- and 1.5708 - 1.0964 = 0.4744 rad = 27.2 degrees. player.cpp:166
    // agrees: it SUBTRACTS the mouse dy, and pushing the mouse forward looks up.
    //
    // The cost of getting it backwards is not an error, it is a plausible-looking capture of the
    // wrong thing: a scripted prop placement aimed 27 degrees the wrong way stuck a campfire to
    // the tunnel CEILING and photographed it, which reads as "the brush is broken" rather than
    // "the camera is upside down". Roll is not exposed; nothing here needs it.
    std::optional<std::array<float, 2>> rot;

    // --patrol RADIUS,PERIOD -- fly a closed horizontal circle of RADIUS metres about --pos (or
    // about the spawn), completing one lap every PERIOD seconds, looking along the tangent.
    //
    // WHY A CIRCLE. The soak this replaces held W down, and at the engine's 15 m/s fly speed
    // that leaves a 37 m island in under three seconds -- after which the "soak" is measuring an
    // empty sky. A circle keeps the camera in the content for as long as you like while still
    // crossing chunk boundaries continuously, which is the thing that actually exercises chunk
    // generation and the voxel heap. It is also a closed loop, so a 5-minute run and a 30-second
    // run see the same set of chunks and their heap figures are comparable.
    std::optional<std::array<float, 2>> patrol;

    // --exit-after SECONDS -- quit cleanly (exit code 0, destructors run) after this much
    // wall-clock time from the first frame.
    float exit_after = -1.0f;

    // --screenshot PATH -- write the swapchain contents to a PNG. This is the actual rendered
    // image: no window manager, no DWM composition, no risk of another window overlapping, and
    // no dependence on the screen being unlocked. --screenshot-after chooses when.
    std::string screenshot_path;
    float screenshot_after = -1.0f;

    // --bench-csv PATH -- append one row per frame: frame index, time, full and CPU frame time,
    // heap capacity/usage/cap, and the camera pose. One row per frame rather than one per second
    // because percentiles are the point; 5 minutes at 90 fps is ~27k rows and about 2 MB.
    std::string bench_csv;

    // --width / --height. Upstream hardcodes AppWindow(APPNAME, {1280, 720}) at voxel_app.cpp:29,
    // which is why every figure in docs/BASELINE.md is 720p.
    uint32_t width = 1280;
    uint32_t height = 720;

    // --seed VALUE. voxel_app.cpp had `auto seed = 15512089755474631791ull;` hardcoded with the
    // std::hash of the UI's world-seed string commented out beside it.
    std::optional<uint64_t> seed;

    // --overlay / --no-overlay. `show_debug_info` is a *persisted* setting and F3 toggles it, so
    // a harness that blindly sends F3 turns the overlay off exactly as often as on. That cost a
    // previous measurement run a full baseline screenshot with no overlay in it.
    int overlay = -1; // -1 leave the saved setting alone, 0 force off, 1 force on

    // --unpause -- start with the game running instead of in the pause menu. Without it, the
    // first frames show the settings UI over the world.
    bool unpause = false;

    // --expand-graphs -- force the two frame-time tree nodes in the debug overlay open. See the
    // comment at their site in application/ui.cpp for the click-coordinate hack this replaces.
    bool expand_graphs = false;

    // --lock-camera -- ignore all mouse and keyboard movement input and re-assert --pos/--rot
    // every frame. Implied by --pos, because a "reproducible" pose that a stray mouse event can
    // nudge is not reproducible. Pass --no-lock-camera to fly around from a given start pose.
    bool lock_camera = false;
    bool lock_camera_explicit = false;

    // F3 -- quality settings requested on the command line, in the order given.
    //
    // WHY THIS EXISTS. Before it, no quality setting was reachable from the command line at all:
    // every harness in this project reached into
    // %APPDATA%\GabeVoxelGame\user_settings.json and patched it by hand between runs. That is
    // the same shared file that trap 1 in PERFORMANCE_PLAN.md section 2.5 is about, so the only
    // way to configure a measurement was also the thing corrupting measurements. With these,
    // a sweep is a loop over argument lists and the settings file is never touched.
    //
    // Applied in AppUi::update() on the first frame -- see the comment there for why that is the
    // only point at which the settings registry is complete.
    std::vector<SettingOverride> setting_overrides;

    // --edit T,BRUSH,RADIUS[,HOLD][,rmb] -- repeatable. See ScriptedEdit above.
    std::vector<ScriptedEdit> edits;

    /// True when any --edit was given. The scripted-edit path unpauses the game and takes over
    /// the tool belt, so it must not engage merely because the vector is empty.
    [[nodiscard]] auto edits_scripted() const -> bool { return !edits.empty(); }

    // Set by --help. main() returns immediately on it -- a --help that starts a Vulkan context
    // and 30 s of shader compilation is not a help screen.
    bool help_requested = false;

    // Set when any option had a value that could not be parsed. main() exits 2 rather than
    // starting; see the comment on `bad` in cli.cpp for the incident that motivated it.
    bool parse_failed = false;

    [[nodiscard]] auto camera_scripted() const -> bool { return lock_camera || patrol.has_value(); }

    /// The single process-wide instance. A global is the honest shape here: these are process
    /// arguments, they are read from three translation units that have no other relationship,
    /// and threading them through constructors would touch more code than the feature is worth.
    static auto get() -> AppCli &;
    /// Fills get() from argv. Unknown arguments are reported on stderr and ignored rather than
    /// fatal, so a harness passing a flag this build predates still produces a run.
    static void parse(int argc, char const *const *argv);
};
