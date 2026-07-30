// Tests for the settings file and the frame limiter.
//
// Neither needs a GL context, a window or an ImGui context, which is why the
// panel and the menus are not tested here: everything they do is ImGui calls,
// and the parts of them that are not - seed resolution - are tested through
// their static entry points.
//
// The frame-limiter tests are the only wall-clock-sensitive ones in the suite.
// They are written to fail on "the limiter did nothing" and to tolerate a
// loaded build agent, so every tolerance below is deliberately loose compared
// with the measured behaviour (mean error under 0.01% and a standard deviation
// under 0.1 ms on the development machine).

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "app/Settings.hpp"
#include "core/FrameLimiter.hpp"
#include "ui/MainMenu.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using voxl::Settings;
using voxl::SettingsParseReport;

/// A settings file that is not the defaults in any field, so a round-trip that
/// silently dropped a key would show up as a mismatch rather than as a value
/// that happens to equal its default.
[[nodiscard]] Settings makeDistinctSettings()
{
    Settings settings;
    settings.renderDistance   = 13;
    settings.fovDegrees       = 92.5f;
    settings.vsync            = false;
    settings.fpsCap           = 144;
    settings.anisotropy       = 4.0f;
    settings.fogDistanceScale = 0.75f;
    settings.guiScale         = 1.25f;
    settings.mouseSensitivity = 0.0875f;
    settings.invertMouseY     = true;
    settings.masterVolume     = 0.35f;
    settings.worldVolume      = 0.6f;
    settings.uiVolume         = 0.15f;
    settings.dayLengthMinutes = 7.5f;
    settings.lodEnabled       = false;
    settings.lodBandStart[0]  = 6;
    settings.lodBandStart[1]  = 11;
    settings.lodBandStart[2]  = 19;
    return settings;
}

/// Unique scratch directory under the OS temp dir, removed on destruction.
class TempDirectory {
public:
    explicit TempDirectory(const char* tag)
    {
        std::error_code error;
        const auto      stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = std::filesystem::temp_directory_path(error) /
                 ("voxl_" + std::string{tag} + "_" + std::to_string(stamp));
        std::filesystem::create_directories(m_path, error);
    }

    ~TempDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(m_path, error);
    }

    TempDirectory(const TempDirectory&)            = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return m_path; }

private:
    std::filesystem::path m_path;
};

}  // namespace

// ============================================================ settings I/O ==

TEST_CASE("settings round-trip exactly through the text format", "[settings]")
{
    const Settings original = makeDistinctSettings();

    Settings                  restored;
    const SettingsParseReport report = voxl::readSettings(voxl::writeSettings(original), restored);

    CHECK(report.malformedLines == 0);
    CHECK(report.unknownKeys == 0);
    // Nothing in makeDistinctSettings() is out of range, so a clamp here would
    // mean the writer emitted something the reader could not accept.
    CHECK(report.clampedValues == 0);
    CHECK(restored == original);
}

TEST_CASE("settings round-trip preserves float values bit-exactly", "[settings]")
{
    // The round-trip is only an equality if the writer emits a shortest
    // round-trippable representation. A "%.3f" would pass every other test in
    // this file and fail this one.
    Settings original;
    original.mouseSensitivity = 0.123456791f;
    original.fovDegrees       = 73.3333359f;
    original.fogDistanceScale = 1.23456788f;

    Settings restored;
    voxl::readSettings(voxl::writeSettings(original), restored);

    CHECK(restored.mouseSensitivity == original.mouseSensitivity);
    CHECK(restored.fovDegrees == original.fovDegrees);
    CHECK(restored.fogDistanceScale == original.fogDistanceScale);
}

TEST_CASE("a malformed line is skipped and the valid lines still load", "[settings]")
{
    const std::string text =
        "# a comment\n"
        "[video]\n"
        "render_distance = 12\n"
        "this line has no equals sign at all\n"
        "fov = not-a-number\n"
        "= 42\n"
        "vsync = maybe\n"
        "gui_scale = 1.5\n"
        "\n"
        "[controls]\n"
        "invert_mouse_y = yes\n";

    Settings                  settings;
    const SettingsParseReport report = voxl::readSettings(text, settings);

    // Four bad lines: no '=', an unparseable float, an empty key, an
    // unparseable bool.
    CHECK(report.malformedLines == 4);
    CHECK(report.unknownKeys == 0);

    // Everything well-formed still landed, including the lines AFTER the
    // damage - which is the property that matters.
    CHECK(settings.renderDistance == 12);
    CHECK(settings.guiScale == 1.5f);
    CHECK(settings.invertMouseY == true);

    // The keys whose values were rejected kept their defaults rather than
    // taking a half-parsed value.
    CHECK(settings.fovDegrees == Settings{}.fovDegrees);
    CHECK(settings.vsync == Settings{}.vsync);
}

TEST_CASE("missing keys fall back to defaults", "[settings]")
{
    const Settings defaults;

    Settings                  settings;
    const SettingsParseReport report = voxl::readSettings("fov = 80\n", settings);

    CHECK(report.keysApplied == 1);
    CHECK(settings.fovDegrees == 80.0f);

    CHECK(settings.renderDistance == defaults.renderDistance);
    CHECK(settings.vsync == defaults.vsync);
    CHECK(settings.fpsCap == defaults.fpsCap);
    CHECK(settings.masterVolume == defaults.masterVolume);
    CHECK(settings.dayLengthMinutes == defaults.dayLengthMinutes);
    CHECK(settings.mouseSensitivity == defaults.mouseSensitivity);
    for (std::size_t i = 0; i < std::size_t{voxl::kLodMax}; ++i) {
        CHECK(settings.lodBandStart[i] == defaults.lodBandStart[i]);
    }
}

TEST_CASE("an entirely empty file yields the defaults and no complaints", "[settings]")
{
    Settings                  settings;
    const SettingsParseReport report = voxl::readSettings("", settings);

    CHECK(report.clean());
    CHECK(report.keysApplied == 0);
    CHECK(settings == Settings{});
}

TEST_CASE("out-of-range values are clamped, not accepted", "[settings]")
{
    using namespace voxl::settings_limits;

    const std::string text =
        "render_distance = 9000\n"
        "fov = -20\n"
        "mouse_sensitivity = 500\n"
        "gui_scale = 0.01\n"
        "volume_master = 3.5\n"
        "volume_ui = -1\n"
        "anisotropy = 64\n"
        "fog_distance_scale = 0\n"
        "day_length_minutes = 100000\n";

    Settings                  settings;
    const SettingsParseReport report = voxl::readSettings(text, settings);

    CHECK(report.malformedLines == 0);
    CHECK(report.clampedValues == 9);

    CHECK(settings.renderDistance == kRenderDistanceMax);
    CHECK(settings.fovDegrees == kFovMin);
    CHECK(settings.mouseSensitivity == kMouseSensitivityMax);
    CHECK(settings.guiScale == kGuiScaleMin);
    CHECK(settings.masterVolume == kVolumeMax);
    CHECK(settings.uiVolume == kVolumeMin);
    CHECK(settings.anisotropy == kAnisotropyMax);
    CHECK(settings.fogDistanceScale == kFogDistanceScaleMin);
    CHECK(settings.dayLengthMinutes == kDayLengthMinutesMax);
}

TEST_CASE("a frame cap below the usable minimum snaps up, and zero stays unlimited",
          "[settings]")
{
    using namespace voxl::settings_limits;

    Settings settings;
    voxl::readSettings("fps_cap = 3\n", settings);
    // Snapping up rather than down to "unlimited": someone who typed 3 wanted a
    // cap, and silently uncapping them is the opposite of what they asked for.
    CHECK(settings.fpsCap == kFpsCapMin);

    voxl::readSettings("fps_cap = 0\n", settings);
    CHECK(settings.fpsCap == kFpsCapUnlimited);

    voxl::readSettings("fps_cap = 99999\n", settings);
    CHECK(settings.fpsCap == kFpsCapMax);
}

TEST_CASE("infinities and NaN are rejected rather than clamped", "[settings]")
{
    // std::clamp on a NaN returns the NaN, so a value that reaches the clamp
    // would sail straight through it and into the projection matrix.
    Settings                  settings;
    const SettingsParseReport report = voxl::readSettings("fov = inf\ngui_scale = nan\n", settings);

    CHECK(report.malformedLines == 2);
    CHECK(settings.fovDegrees == Settings{}.fovDegrees);
    CHECK(settings.guiScale == Settings{}.guiScale);
}

TEST_CASE("LOD bands are repaired into an ascending, promotable table", "[settings][lod]")
{
    using namespace voxl::settings_limits;

    SECTION("descending input is pushed into order")
    {
        Settings settings;
        voxl::readSettings("lod_bands = 20,10,4\n", settings);

        CHECK(settings.lodBandStart[0] == 20);
        CHECK(settings.lodBandStart[1] == 20 + kLodMinBandWidth);
        CHECK(settings.lodBandStart[2] == 20 + 2 * kLodMinBandWidth);
    }

    SECTION("bands narrower than the hysteresis band are widened")
    {
        // world/Lod.hpp: a band narrower than hysteresis + 2 can be demoted out
        // of but never promoted back into, so a chunk that lands there stays
        // coarse forever.
        Settings settings;
        voxl::readSettings("lod_bands = 5,6,7\n", settings);

        CHECK(settings.lodBandStart[0] == 5);
        CHECK(settings.lodBandStart[1] - settings.lodBandStart[0] >= kLodMinBandWidth);
        CHECK(settings.lodBandStart[2] - settings.lodBandStart[1] >= kLodMinBandWidth);
    }

    SECTION("a band table that would not fit is clamped without inverting")
    {
        Settings settings;
        settings.lodBandStart[0] = 10'000;
        settings.lodBandStart[1] = 10'000;
        settings.lodBandStart[2] = 10'000;
        settings.clampToValidRange();

        CHECK(settings.lodBandStart[0] < settings.lodBandStart[1]);
        CHECK(settings.lodBandStart[1] < settings.lodBandStart[2]);
        CHECK(settings.lodBandStart[2] <= kLodBandMax);
    }

    SECTION("a malformed band list leaves the defaults intact")
    {
        const Settings            defaults;
        Settings                  settings;
        const SettingsParseReport report = voxl::readSettings("lod_bands = 5,9\n", settings);

        CHECK(report.malformedLines == 1);
        for (std::size_t i = 0; i < std::size_t{voxl::kLodMax}; ++i) {
            CHECK(settings.lodBandStart[i] == defaults.lodBandStart[i]);
        }
    }
}

TEST_CASE("an unknown key is preserved rather than deleted", "[settings]")
{
    // Several agents add keys to this file in parallel. An older binary that
    // silently dropped a newer one's keys would cost the player their settings
    // the first time they launched the wrong executable.
    const std::string text =
        "fov = 80\n"
        "some_future_key = 42\n"
        "another.future/key = hello world\n";

    Settings                  settings;
    const SettingsParseReport report = voxl::readSettings(text, settings);

    REQUIRE(report.unknownKeys == 2);
    REQUIRE(report.unknownLines.size() == 2);
    CHECK(report.unknownLines[0] == "some_future_key = 42");
    CHECK(report.unknownLines[1] == "another.future/key = hello world");

    const std::string rewritten = voxl::writeSettings(settings, report.unknownLines);
    CHECK(rewritten.find("some_future_key = 42") != std::string::npos);
    CHECK(rewritten.find("another.future/key = hello world") != std::string::npos);

    // And they survive a second generation, so repeated launches do not erode
    // them one save at a time.
    Settings                  again;
    const SettingsParseReport secondReport = voxl::readSettings(rewritten, again);
    CHECK(secondReport.unknownKeys == 2);
    CHECK(again == settings);
}

TEST_CASE("boolean spellings a human would type all parse", "[settings]")
{
    for (const char* yes : {"true", "TRUE", "True", "1", "on", "YES"}) {
        Settings settings;
        settings.invertMouseY = false;
        const SettingsParseReport report =
            voxl::readSettings(std::string{"invert_mouse_y = "} + yes + "\n", settings);
        CHECK(report.malformedLines == 0);
        CHECK(settings.invertMouseY);
    }
    for (const char* no : {"false", "0", "off", "No"}) {
        Settings settings;
        const SettingsParseReport report =
            voxl::readSettings(std::string{"vsync = "} + no + "\n", settings);
        CHECK(report.malformedLines == 0);
        CHECK_FALSE(settings.vsync);
    }
}

TEST_CASE("the settings file round-trips through the filesystem", "[settings][file]")
{
    const TempDirectory         scratch{"settings"};
    const std::filesystem::path file = scratch.path() / "settings.cfg";

    SECTION("a missing file loads the defaults and reports absence")
    {
        Settings settings = makeDistinctSettings();  // deliberately not defaults
        CHECK_FALSE(voxl::loadSettingsFile(file, settings));
        CHECK(settings == Settings{});
    }

    SECTION("save then load")
    {
        const Settings original = makeDistinctSettings();
        REQUIRE(voxl::saveSettingsFile(file, original));
        REQUIRE(std::filesystem::exists(file));

        Settings            restored;
        SettingsParseReport report;
        REQUIRE(voxl::loadSettingsFile(file, restored, &report));
        CHECK(report.clean());
        CHECK(restored == original);
    }

    SECTION("saving leaves no temporary behind")
    {
        REQUIRE(voxl::saveSettingsFile(file, Settings{}));
        CHECK_FALSE(std::filesystem::exists(file.string() + ".tmp"));
    }

    SECTION("the file is human-readable text")
    {
        REQUIRE(voxl::saveSettingsFile(file, Settings{}));
        std::ifstream     stream(file);
        const std::string first{std::istreambuf_iterator<char>(stream),
                                std::istreambuf_iterator<char>()};
        CHECK(first.find("render_distance") != std::string::npos);
        CHECK(first.find("[video]") != std::string::npos);
        CHECK(first.find('\0') == std::string::npos);
    }
}

TEST_CASE("the settings path sits beside the executable", "[settings][file]")
{
    const std::filesystem::path path = voxl::defaultSettingsPath();
    CHECK(path.filename() == "settings.cfg");
    CHECK(path.parent_path() == voxl::executableDirectory());
    CHECK(voxl::defaultSavesDirectory().parent_path() == voxl::executableDirectory());
}

// =================================================================== seeds ==

TEST_CASE("seed resolution is deterministic and honours what was typed", "[settings][menu]")
{
    SECTION("a decimal number means itself")
    {
        CHECK(voxl::MainMenu::resolveSeed("12345") == 12345ull);
        CHECK(voxl::MainMenu::resolveSeed("  42  ") == 42ull);
        CHECK(voxl::MainMenu::resolveSeed("0") == 0ull);
        // A shared negative seed must reproduce the same world as its unsigned
        // two's-complement spelling.
        CHECK(voxl::MainMenu::resolveSeed("-1") == 0xFFFFFFFFFFFFFFFFull);
    }

    SECTION("text hashes deterministically")
    {
        const std::uint64_t a = voxl::MainMenu::resolveSeed("gargantuan");
        CHECK(voxl::MainMenu::resolveSeed("gargantuan") == a);
        CHECK(voxl::MainMenu::resolveSeed("gargantuam") != a);
        // Near-identical short strings must not land in adjacent seeds; the
        // terrain generator does not whiten the seed before using it.
        CHECK(voxl::MainMenu::resolveSeed("world1") != voxl::MainMenu::resolveSeed("world2") + 1);
    }

    SECTION("blank draws from the clock and is never zero")
    {
        CHECK(voxl::MainMenu::resolveSeed("") != 0ull);
        CHECK(voxl::MainMenu::resolveSeed("   ") != 0ull);
    }
}

// =========================================================== frame limiter ==

namespace {

struct PacingResult {
    std::vector<double>     frameMs;
    voxl::FrameLimiterStats stats{};

    [[nodiscard]] double meanMs() const
    {
        return std::accumulate(frameMs.begin(), frameMs.end(), 0.0) /
               static_cast<double>(frameMs.size());
    }

    [[nodiscard]] double medianMs() const
    {
        std::vector<double> sorted = frameMs;
        std::sort(sorted.begin(), sorted.end());
        return sorted[sorted.size() / 2];
    }
};

/// Runs `frames` empty frames through a limiter at `targetFps` and records the
/// interval between successive `wait()` returns.
[[nodiscard]] PacingResult pace(float targetFps, int frames)
{
    voxl::FrameLimiter limiter{targetFps};

    // The overshoot estimate is adaptive, so the first handful of frames are
    // measuring the machine rather than being paced by it. Excluding them is
    // what the real frame loop gets for free by running for more than a second.
    for (int i = 0; i < 20; ++i) {
        limiter.wait();
    }
    limiter.resetStats();

    PacingResult result;
    result.frameMs.reserve(static_cast<std::size_t>(frames));

    auto previous = voxl::Clock::now();
    for (int i = 0; i < frames; ++i) {
        limiter.wait();
        const auto now = voxl::Clock::now();
        result.frameMs.push_back(
            std::chrono::duration<double, std::milli>(now - previous).count());
        previous = now;
    }
    result.stats = limiter.stats();
    return result;
}

}  // namespace

TEST_CASE("a disabled frame limiter does not wait", "[framelimiter]")
{
    voxl::FrameLimiter limiter{0.0f};
    CHECK_FALSE(limiter.enabled());
    CHECK(limiter.period() == std::chrono::nanoseconds{0});

    // One aggregated assertion rather than a thousand: Catch2's per-assertion
    // bookkeeping would otherwise dominate the very interval being measured.
    bool       allImmediate = true;
    const auto start        = voxl::Clock::now();
    for (int i = 0; i < 1000; ++i) {
        allImmediate = allImmediate && (limiter.wait() == std::chrono::nanoseconds{0});
    }
    CHECK(allImmediate);
    // A thousand no-op waits must be far below one frame at any sane rate.
    CHECK(std::chrono::duration<double>(voxl::Clock::now() - start).count() < 0.05);
}

TEST_CASE("the target frame rate is clamped and the period follows it", "[framelimiter]")
{
    voxl::FrameLimiter limiter;

    limiter.setTargetFps(60.0f);
    CHECK(limiter.targetFps() == 60.0f);
    CHECK(std::chrono::duration<double>(limiter.period()).count() ==
          Catch::Approx(1.0 / 60.0).epsilon(1e-6));

    limiter.setTargetFps(100000.0f);
    CHECK(limiter.targetFps() == voxl::FrameLimiter::kMaxTargetFps);

    limiter.setTargetFps(0.001f);
    CHECK(limiter.targetFps() == voxl::FrameLimiter::kMinTargetFps);

    limiter.setTargetFps(-5.0f);
    CHECK_FALSE(limiter.enabled());
}

TEST_CASE("the frame limiter holds a requested rate over many frames", "[framelimiter][slow]")
{
    // 200 fps for 200 frames: one second of wall clock, and a rate high enough
    // that a limiter that merely slept would visibly undershoot it.
    constexpr float kTarget = 200.0f;
    constexpr int   kFrames = 200;
    const double    ideal   = 1000.0 / static_cast<double>(kTarget);

    const PacingResult result = pace(kTarget, kFrames);

    INFO("mean " << result.meanMs() << " ms, median " << result.medianMs() << " ms, ideal " << ideal
                 << " ms, overruns " << result.stats.overruns << "/" << result.stats.frames
                 << ", spin fraction " << result.stats.spinFraction());

    // TOLERANCE, and why it is what it is. The measured mean error on the
    // development machine is under 0.01% with a standard deviation of ~0.09 ms;
    // 2% here is twenty times that, which leaves room for a build agent that is
    // busy without letting a genuinely broken limiter through. The interesting
    // failure is not "slightly off" - it is "did not limit at all", which would
    // put the mean at microseconds, or "slept naively", which would put it
    // several percent long and always long.
    CHECK(result.meanMs() > ideal * 0.98);
    CHECK(result.meanMs() < ideal * 1.02);

    // The median is the honest measure of the typical frame: it ignores the one
    // or two scheduler hiccups a second that no user-space limiter can prevent.
    CHECK(result.medianMs() == Catch::Approx(ideal).epsilon(0.02));

    // Individual frames: the great majority must be close, not just the average.
    const std::size_t within5Percent = static_cast<std::size_t>(
        std::count_if(result.frameMs.begin(), result.frameMs.end(), [ideal](double ms) {
            return ms > ideal * 0.95 && ms < ideal * 1.05;
        }));
    CHECK(within5Percent * 10 >= result.frameMs.size() * 8);  // >= 80%

    // The whole point of the sleep/spin split: it must not be spinning for most
    // of the frame, or it is just a busy-wait with extra steps.
    CHECK(result.stats.spinFraction() < 0.5);
    CHECK(result.stats.sleepCalls > 0);
}

TEST_CASE("the frame limiter holds a low rate as accurately as a high one",
          "[framelimiter][slow]")
{
    constexpr float kTarget = 60.0f;
    const double    ideal   = 1000.0 / static_cast<double>(kTarget);

    const PacingResult result = pace(kTarget, 90);  // 1.5 s

    INFO("mean " << result.meanMs() << " ms, ideal " << ideal << " ms");
    CHECK(result.meanMs() > ideal * 0.98);
    CHECK(result.meanMs() < ideal * 1.02);
    // At 16.7 ms a frame there is far more slack to sleep through, so the spin
    // share should be smaller than at 200 fps rather than larger.
    CHECK(result.stats.spinFraction() < 0.25);
}

TEST_CASE("a frame that overruns its deadline is reported and does not cause a sprint",
          "[framelimiter]")
{
    voxl::FrameLimiter limiter{500.0f};  // 2 ms frames
    limiter.wait();                      // prime
    limiter.resetStats();

    // A frame that takes far longer than its slot: the limiter must return
    // immediately rather than trying to claw the time back.
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    const auto waited = limiter.wait();

    CHECK(waited == std::chrono::nanoseconds{0});
    CHECK(limiter.stats().overruns == 1);

    // And the frame after the stall is paced normally rather than skipped.
    const auto recovered = limiter.wait();
    CHECK(recovered > std::chrono::nanoseconds{0});
    CHECK(limiter.stats().overruns == 1);
}

TEST_CASE("reset() re-primes the deadline instead of catching up", "[framelimiter]")
{
    voxl::FrameLimiter limiter{100.0f};
    limiter.wait();

    // Stand in for a long blocking operation - a world load - after which the
    // accumulated schedule is meaningless.
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    limiter.reset();

    // The first wait after a reset re-primes and returns immediately; the one
    // after it is paced.
    CHECK(limiter.wait() == std::chrono::nanoseconds{0});
    CHECK(limiter.wait() > std::chrono::nanoseconds{0});
    CHECK(limiter.stats().overruns == 0);
}
