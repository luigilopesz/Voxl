#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "world/DayNightCycle.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include <glm/geometric.hpp>

using namespace voxl;
using Catch::Approx;

namespace {

/// Sample count for the full-cycle sweeps. Deliberately not a divisor of any
/// keyframe time, so the sweep lands between keys as often as on them.
constexpr int kSweepSteps = 997;

[[nodiscard]] bool isFinite(const glm::vec3& v) noexcept
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

[[nodiscard]] bool isNonNegative(const glm::vec3& v) noexcept
{
    return v.x >= 0.0f && v.y >= 0.0f && v.z >= 0.0f;
}

[[nodiscard]] float maxComponent(const glm::vec3& v) noexcept
{
    return std::max({v.x, v.y, v.z});
}

/// Rec. 709 luminance. Used as the single "is this readable" number, because
/// the eye's response to a dim blue night is not the average of the channels.
[[nodiscard]] float luminance(const glm::vec3& linear) noexcept
{
    return 0.2126f * linear.r + 0.7152f * linear.g + 0.0722f * linear.b;
}

[[nodiscard]] std::vector<DayNightState> sweep(const DayNightConfig& config)
{
    std::vector<DayNightState> states;
    states.reserve(kSweepSteps);
    for (int i = 0; i < kSweepSteps; ++i) {
        states.push_back(
            DayNightCycle::sample(static_cast<float>(i) / static_cast<float>(kSweepSteps), config));
    }
    return states;
}

}  // namespace

// ------------------------------------------------------------- time model --

TEST_CASE("time of day wraps into [0,1)", "[daynight]")
{
    CHECK(wrapTimeOfDay(0.0f) == Approx(0.0f));
    CHECK(wrapTimeOfDay(0.25f) == Approx(0.25f));
    CHECK(wrapTimeOfDay(1.0f) == Approx(0.0f));
    CHECK(wrapTimeOfDay(2.75f) == Approx(0.75f));
    CHECK(wrapTimeOfDay(-0.25f) == Approx(0.75f));
    CHECK(wrapTimeOfDay(-3.10f) == Approx(0.90f).margin(1e-5));

    // Every wrap must satisfy the half-open postcondition, including the values
    // that sit a hair under an integer where the subtraction can round up.
    for (int i = -2000; i <= 2000; ++i) {
        const float t = wrapTimeOfDay(static_cast<float>(i) * 0.0013f);
        CHECK(t >= 0.0f);
        CHECK(t < 1.0f);
    }
    CHECK(wrapTimeOfDay(std::nextafter(1.0f, 0.0f)) < 1.0f);
}

TEST_CASE("setting the time round-trips", "[daynight]")
{
    DayNightCycle cycle;
    for (int i = 0; i <= 200; ++i) {
        const float t = static_cast<float>(i) / 200.0f;
        cycle.setTimeOfDay(t);
        CHECK(cycle.timeOfDay() == Approx(wrapTimeOfDay(t)).margin(1e-6));
    }

    cycle.setTimeOfDay(-0.25f);
    CHECK(cycle.timeOfDay() == Approx(0.75f));

    cycle.setTimeOfDayHours(13.5f);
    CHECK(cycle.hours() == Approx(13.5f).margin(1e-4));
    CHECK(cycle.clockText() == "13:30");

    cycle.setTimeOfDay(kTimeMidnight);
    CHECK(cycle.clockText() == "00:00");
    cycle.setTimeOfDay(kTimeNoon);
    CHECK(cycle.clockText() == "12:00");

    // 23:59:40 must round down into the same day, not print 24:00.
    cycle.setTimeOfDay(0.99977f);
    CHECK(cycle.clockText() == "00:00");
}

TEST_CASE("the cycle is deterministic and periodic", "[daynight]")
{
    DayNightConfig config;
    config.dayLengthSeconds = 120.0;
    config.startTimeOfDay   = 0.1f;

    DayNightCycle a(config);
    DayNightCycle b(config);

    // Same deltas in, same state out - every frame, not just at the end.
    for (int i = 0; i < 500; ++i) {
        const double dt = 0.0166 + 0.0001 * (i % 7);
        a.advance(dt);
        b.advance(dt);
        REQUIRE(a.timeOfDay() == Approx(b.timeOfDay()));
        REQUIRE(a.sunDirection().x == Approx(b.sunDirection().x));
        REQUIRE(a.sunDirection().y == Approx(b.sunDirection().y));
        REQUIRE(a.sunDirection().z == Approx(b.sunDirection().z));
    }

    // Exactly one day of advancement returns to the start.
    DayNightCycle c(config);
    const float start = c.timeOfDay();
    for (int i = 0; i < 1200; ++i) {
        c.advance(config.dayLengthSeconds / 1200.0);
    }
    CHECK(c.timeOfDay() == Approx(start).margin(1e-4));

    // And the model itself is periodic: t and t+1 are the same instant.
    for (int i = 0; i < 64; ++i) {
        const float t     = static_cast<float>(i) / 64.0f;
        const auto  here  = DayNightCycle::sample(t, config);
        const auto  again = DayNightCycle::sample(t + 3.0f, config);
        CHECK(here.sunDirection.y == Approx(again.sunDirection.y).margin(1e-5));
        CHECK(here.sky.horizonColour.r == Approx(again.sky.horizonColour.r).margin(1e-5));
        CHECK(here.sky.sunIntensity == Approx(again.sky.sunIntensity).margin(1e-5));
    }
}

TEST_CASE("advance respects pause and day length", "[daynight]")
{
    DayNightConfig config;
    config.dayLengthSeconds = 60.0;
    config.startTimeOfDay   = 0.5f;

    DayNightCycle cycle(config);
    cycle.advance(15.0);
    CHECK(cycle.timeOfDay() == Approx(0.75f).margin(1e-5));

    cycle.setPaused(true);
    cycle.advance(1000.0);
    CHECK(cycle.timeOfDay() == Approx(0.75f).margin(1e-5));

    cycle.setPaused(false);
    cycle.advance(-15.0);  // running backwards is how the harness brackets a moment
    CHECK(cycle.timeOfDay() == Approx(0.5f).margin(1e-5));

    // A zero-length day freezes rather than dividing by zero.
    cycle.setDayLengthSeconds(0.0);
    cycle.advance(100.0);
    CHECK(cycle.timeOfDay() == Approx(0.5f).margin(1e-5));

    // Non-finite deltas must not poison the clock.
    cycle.setDayLengthSeconds(60.0);
    cycle.advance(std::numeric_limits<double>::quiet_NaN());
    CHECK(cycle.timeOfDay() == Approx(0.5f).margin(1e-5));

    // Long runs stay in range instead of drifting out of [0,1).
    for (int i = 0; i < 100000; ++i) {
        cycle.advance(0.017);
    }
    CHECK(cycle.timeOfDay() >= 0.0f);
    CHECK(cycle.timeOfDay() < 1.0f);
}

// --------------------------------------------------------------- geometry --

TEST_CASE("the sun direction is always unit length", "[daynight]")
{
    for (float tilt : {0.0f, 12.0f, 21.0f, 45.0f, 80.0f}) {
        for (float azimuth : {0.0f, 37.0f, 180.0f, 275.0f, -90.0f}) {
            DayNightConfig config;
            config.axialTiltDegrees      = tilt;
            config.sunriseAzimuthDegrees = azimuth;

            for (const DayNightState& state : sweep(config)) {
                REQUIRE(glm::length(state.sunDirection) == Approx(1.0f).margin(1e-5));
                REQUIRE(glm::length(state.moonDirection) == Approx(1.0f).margin(1e-5));
            }
        }
    }
}

TEST_CASE("the moon is opposite the sun", "[daynight]")
{
    DayNightConfig config;
    for (const DayNightState& state : sweep(config)) {
        REQUIRE(glm::dot(state.sunDirection, state.moonDirection) == Approx(-1.0f).margin(1e-5));
    }
}

TEST_CASE("the sun is up at midday and down at midnight", "[daynight]")
{
    const DayNightConfig config;

    const DayNightState noon = DayNightCycle::sample(kTimeNoon, config);
    CHECK(noon.sunElevation > 0.8f);
    CHECK(noon.sunDirection.y == Approx(noon.sunElevation));

    const DayNightState midnight = DayNightCycle::sample(kTimeMidnight, config);
    CHECK(midnight.sunElevation < -0.8f);
    // ...which is exactly when the moon is highest.
    CHECK(midnight.moonDirection.y > 0.8f);

    // The horizon crossings sit on the named anchors whatever the tilt is: the
    // colour keyframes are authored against those anchors, so if this ever
    // stopped holding the sunset colours would fire while the sun was still up.
    for (float tilt : {0.0f, 21.0f, 60.0f}) {
        DayNightConfig tilted;
        tilted.axialTiltDegrees = tilt;
        CHECK(DayNightCycle::sample(kTimeSunrise, tilted).sunElevation == Approx(0.0f).margin(1e-6));
        CHECK(DayNightCycle::sample(kTimeSunset, tilted).sunElevation == Approx(0.0f).margin(1e-6));
    }

    // A believable arc: the sun must not simply spin in a plane through the
    // zenith, or midday light comes straight down and flattens everything.
    CHECK(std::abs(noon.sunDirection.y) < 0.995f);

    // The sun is above the horizon for exactly the daylight half of the cycle.
    for (int i = 0; i < kSweepSteps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kSweepSteps);
        const float elevation = DayNightCycle::sample(t, config).sunElevation;
        if (t > kTimeSunrise + 0.01f && t < kTimeSunset - 0.01f) {
            REQUIRE(elevation > 0.0f);
        } else if (t < kTimeSunrise - 0.01f || t > kTimeSunset + 0.01f) {
            REQUIRE(elevation < 0.0f);
        }
    }
}

TEST_CASE("the day factor follows the sun, not the clock", "[daynight]")
{
    const DayNightConfig config;
    CHECK(DayNightCycle::sample(kTimeNoon, config).dayFactor == Approx(1.0f));
    CHECK(DayNightCycle::sample(kTimeMidnight, config).dayFactor == Approx(0.0f));

    for (const DayNightState& state : sweep(config)) {
        REQUIRE(state.dayFactor >= 0.0f);
        REQUIRE(state.dayFactor <= 1.0f);
        REQUIRE(state.nightFactor == Approx(1.0f - state.dayFactor));
        REQUIRE(state.sky.dayFactor == Approx(state.dayFactor));
    }
}

// ----------------------------------------------------------------- colour --

TEST_CASE("colours stay finite and non-negative across a full sweep", "[daynight]")
{
    for (const DayNightState& state : sweep(DayNightConfig{})) {
        const SkySettings& sky = state.sky;

        REQUIRE(isFinite(sky.zenithColour));
        REQUIRE(isFinite(sky.horizonColour));
        REQUIRE(isFinite(sky.sunColour));
        REQUIRE(isFinite(sky.ambientColour));
        REQUIRE(isFinite(sky.blockLightColour));
        REQUIRE(std::isfinite(sky.sunIntensity));
        REQUIRE(std::isfinite(sky.blockLightGain));
        REQUIRE(std::isfinite(sky.aoStrength));

        REQUIRE(isNonNegative(sky.zenithColour));
        REQUIRE(isNonNegative(sky.horizonColour));
        REQUIRE(isNonNegative(sky.sunColour));
        REQUIRE(isNonNegative(sky.ambientColour));
        REQUIRE(isNonNegative(sky.blockLightColour));
        REQUIRE(sky.sunIntensity >= 0.0f);
        REQUIRE(sky.blockLightGain >= 0.0f);
        REQUIRE(sky.aoStrength >= 0.0f);
        REQUIRE(sky.aoStrength <= 1.0f);

        // Nothing in the grade is an HDR light source; a value far above 1 in a
        // colour would mean a typo in the keyframe table, which is otherwise
        // invisible until someone looks at a screenshot at that exact time.
        // The slack absorbs the last bit of the sRGB decode at exactly 1.0.
        constexpr float kCeiling = 1.0f + 1e-3f;
        REQUIRE(maxComponent(sky.zenithColour) <= kCeiling);
        REQUIRE(maxComponent(sky.horizonColour) <= kCeiling);
        REQUIRE(maxComponent(sky.sunColour) <= kCeiling);
        REQUIRE(maxComponent(sky.ambientColour) <= kCeiling);
        REQUIRE(sky.sunIntensity <= 4.0f);
    }
}

TEST_CASE("the grade is continuous, including across midnight", "[daynight]")
{
    const DayNightConfig config;
    const float          step = 1.0f / static_cast<float>(kSweepSteps);

    for (int i = 0; i < kSweepSteps; ++i) {
        const float t    = static_cast<float>(i) * step;
        const auto  here = DayNightCycle::sample(t, config);
        const auto  next = DayNightCycle::sample(t + step, config);

        // One thousandth of a day is about a second of real time at the default
        // day length; nothing may jump perceptibly in that span. This is the
        // test that catches a mis-ordered keyframe table and, in particular, a
        // discontinuity at the 0.87 -> 0.00 wrap.
        constexpr float kMaxStep = 0.03f;
        REQUIRE(std::abs(luminance(next.sky.horizonColour) -
                         luminance(here.sky.horizonColour)) < kMaxStep);
        REQUIRE(std::abs(luminance(next.sky.zenithColour) -
                         luminance(here.sky.zenithColour)) < kMaxStep);
        REQUIRE(std::abs(next.sky.sunIntensity - here.sky.sunIntensity) < kMaxStep);
        REQUIRE(std::abs(luminance(next.sky.ambientColour) -
                         luminance(here.sky.ambientColour)) < kMaxStep);
    }
}

TEST_CASE("midday is bright and neutral, night is dim and cool", "[daynight]")
{
    const DayNightConfig config;
    const SkySettings    noon     = DayNightCycle::sample(kTimeNoon, config).sky;
    const SkySettings    midnight = DayNightCycle::sample(kTimeMidnight, config).sky;
    const SkySettings    sunset   = DayNightCycle::sample(kTimeSunset, config).sky;
    const SkySettings    sunrise  = DayNightCycle::sample(kTimeSunrise, config).sky;

    // Sun intensity and ambient both fall at night, which is what lets emissive
    // block light read at all.
    CHECK(midnight.sunIntensity < noon.sunIntensity * 0.1f);
    CHECK(luminance(midnight.ambientColour) < luminance(noon.ambientColour) * 0.5f);
    CHECK(midnight.blockLightGain > noon.blockLightGain);

    // Cool at night: blue dominates.
    CHECK(midnight.ambientColour.b > midnight.ambientColour.r * 1.5f);
    CHECK(midnight.zenithColour.b > midnight.zenithColour.r * 1.5f);

    // Warm at dawn and dusk: the horizon goes red-dominant while the zenith
    // stays cool, which is what a real sunset does.
    CHECK(sunset.horizonColour.r > sunset.horizonColour.b * 2.0f);
    CHECK(sunrise.horizonColour.r > sunrise.horizonColour.b * 2.0f);
    CHECK(sunset.zenithColour.b > sunset.zenithColour.r);

    // Dusk is redder than dawn, so the two halves of the cycle are not the same
    // event played backwards.
    CHECK(sunset.sunColour.g / sunset.sunColour.r < sunrise.sunColour.g / sunrise.sunColour.r);

    // Low contrast at the edges of the day, higher at midday. Contrast here is
    // the zenith-to-horizon luminance ratio plus the AO depth.
    CHECK(sunset.aoStrength < noon.aoStrength);
    CHECK(midnight.aoStrength < noon.aoStrength);
}

TEST_CASE("night is never pitch black", "[daynight]")
{
    // A GAMEPLAY REQUIREMENT, not an aesthetic one: a player who cannot see to
    // walk home has hit a defect. Ambient is the only light an outdoor surface
    // gets at night, so it is the number that has to hold up.
    for (const DayNightState& state : sweep(DayNightConfig{})) {
        REQUIRE(luminance(state.sky.ambientColour) > 0.06f);
        REQUIRE(state.sky.ambientColour.r > 0.02f);
        REQUIRE(state.sky.ambientColour.g > 0.02f);
        REQUIRE(state.sky.ambientColour.b > 0.02f);

        // The sky itself must not go to black either, or the horizon vanishes
        // and with it every silhouette the player navigates by.
        REQUIRE(luminance(state.sky.horizonColour) > 0.008f);
    }
}

TEST_CASE("the fog range scale stays sane", "[daynight]")
{
    // Fog COLOUR needs no test here: every shader fogs toward the same
    // voxlSkyColour() the sky itself draws, so it tracks by construction. The
    // range multiplier is the only fog value this module produces.
    for (const DayNightState& state : sweep(DayNightConfig{})) {
        REQUIRE(state.fogRangeScale > 0.5f);
        REQUIRE(state.fogRangeScale <= 1.0f);
    }
    CHECK(DayNightCycle::sample(kTimeNoon, DayNightConfig{}).fogRangeScale == Approx(1.0f));
    CHECK(DayNightCycle::sample(kTimeSunrise, DayNightConfig{}).fogRangeScale < 0.95f);
}

// ---------------------------------------------------------------- parsing --

TEST_CASE("time of day parses the harness's spellings", "[daynight]")
{
    float value = -1.0f;

    CHECK(parseTimeOfDay("noon", value));
    CHECK(value == Approx(kTimeNoon));
    CHECK(parseTimeOfDay("MIDNIGHT", value));
    CHECK(value == Approx(kTimeMidnight));
    CHECK(parseTimeOfDay(" Dawn ", value));
    CHECK(value == Approx(kTimeSunrise));
    CHECK(parseTimeOfDay("sunset", value));
    CHECK(value == Approx(kTimeSunset));

    CHECK(parseTimeOfDay("13:30", value));
    CHECK(value == Approx(13.5f / 24.0f));
    CHECK(parseTimeOfDay("0:00", value));
    CHECK(value == Approx(0.0f));
    CHECK(parseTimeOfDay("24:00", value));
    CHECK(value == Approx(0.0f).margin(1e-6));

    CHECK(parseTimeOfDay("0.75", value));
    CHECK(value == Approx(0.75f));
    CHECK(parseTimeOfDay("18", value));
    CHECK(value == Approx(0.75f));

    // Documented ambiguity: a bare 1 is the fraction, not one a.m.
    CHECK(parseTimeOfDay("1", value));
    CHECK(value == Approx(0.0f).margin(1e-6));

    const float untouched = 0.123f;
    value                 = untouched;
    CHECK_FALSE(parseTimeOfDay("", value));
    CHECK_FALSE(parseTimeOfDay("teatime", value));
    CHECK_FALSE(parseTimeOfDay("12:", value));
    CHECK_FALSE(parseTimeOfDay("12:75", value));
    CHECK_FALSE(parseTimeOfDay("1.5.2", value));
    CHECK(value == Approx(untouched));
}
