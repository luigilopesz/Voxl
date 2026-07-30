#pragma once

// The day/night cycle: the single authority for where the sun is and what
// colour the world is lit by at any moment.
//
// It is a pure function of one number. `timeOfDay` is normalised to [0, 1) with
// 0 = midnight, 0.25 = sunrise, 0.5 = noon, 0.75 = sunset; everything else -
// sun direction, moon direction, the sky gradient, ambient, fog range - is
// derived from it. `advance()` only moves that number forward. Keeping the
// whole model in a static `sample()` is what makes the capture harness, the
// debug overlay and the tests able to jump to a time without simulating up to
// it, and what makes "same time => same sky" true by construction.
//
// COLOUR SPACE. The keyframes in the .cpp are authored in sRGB because that is
// how a human reads a colour, but they are converted to LINEAR once and every
// interpolation happens in linear space. Crossfading sRGB values through a
// sunset drags the midpoint toward grey: sRGB 1.0 -> 0.0 passes through 0.5,
// which is linear 0.21, so a red-to-blue transition loses two thirds of its
// luminance halfway and reads as mud. This is the single most common way a
// day/night cycle looks cheap.
//
// Thread safety: none, and none needed. It is main-thread state that produces
// render parameters.

// SkySettings is the frozen output contract of this module (see
// render/Renderer.hpp). Mirroring it here with a private struct and a mapping
// function would compile, but the two copies would silently drift the first
// time the renderer gains a field, so the dependency is deliberate.
#include "render/Renderer.hpp"

#include <glm/vec3.hpp>

#include <string>
#include <string_view>

namespace voxl {

// ------------------------------------------------------------- time anchors --

inline constexpr float kTimeMidnight = 0.00f;
inline constexpr float kTimeSunrise  = 0.25f;
inline constexpr float kTimeNoon     = 0.50f;
inline constexpr float kTimeSunset   = 0.75f;

/// Real seconds in one in-game day. Twenty minutes: long enough that a player
/// building through an afternoon is not strobed by sunsets, short enough that
/// someone who just wants to see the night does not have to wait for it.
inline constexpr double kDefaultDayLengthSeconds = 1200.0;

/// Reduces any finite time value into [0, 1). Negative inputs wrap forward, so
/// `wrapTimeOfDay(-0.25)` is 0.75 (sunset), not a clamp to zero.
[[nodiscard]] float wrapTimeOfDay(float timeOfDay) noexcept;

// ------------------------------------------------------------------ config --

struct DayNightConfig {
    /// Real seconds for one full cycle. Values <= 0 freeze the cycle rather
    /// than dividing by zero; use `paused` if that is what you meant.
    double dayLengthSeconds = kDefaultDayLengthSeconds;

    /// Where the cycle starts. Mid-morning by default: a new world opens on a
    /// lit landscape with the sun low enough to show relief.
    float startTimeOfDay = 0.30f;

    /// Tilt of the sun's orbital plane away from the zenith, in degrees. Zero
    /// puts the sun directly overhead at noon, which flattens every surface at
    /// midday and is the giveaway of a naive cycle. 21 degrees keeps a
    /// permanent grazing component.
    ///
    /// The tilt does NOT move sunrise or sunset: the sun's elevation is
    /// sin(orbit angle) * cos(tilt), so it crosses zero at exactly 0.25 and
    /// 0.75 for any tilt below 90 degrees. Geometry and the colour keyframes
    /// therefore stay in agreement whatever this is set to.
    float axialTiltDegrees = 21.0f;

    /// Compass bearing of the point on the horizon where the sun rises, in
    /// degrees. 0 rises at +X and sets at -X, with the noon sun toward +Z;
    /// increasing it rotates the whole arc toward -Z.
    float sunriseAzimuthDegrees = 0.0f;

    bool paused = false;
};

// ------------------------------------------------------------------- state --

/// Everything the cycle produces for one instant. Copyable and inert: hold one
/// to compare two times of day without touching the live cycle.
struct DayNightState {
    float timeOfDay = 0.0f;

    /// Unit vector pointing TOWARDS the sun, and its exact antipode.
    glm::vec3 sunDirection{0.0f, 1.0f, 0.0f};
    glm::vec3 moonDirection{0.0f, -1.0f, 0.0f};

    /// `sunDirection.y`, i.e. sin(elevation). Positive means the sun is up.
    float sunElevation = 1.0f;

    /// 1 in full daylight, 0 in full night, with civil twilight in between.
    /// Driven by the sun's elevation, not by the clock, so it stays correct if
    /// the arc is retilted.
    float dayFactor   = 1.0f;
    float nightFactor = 0.0f;

    /// Multiplier the application may apply to its fog distances. Below 1 at
    /// dawn and dusk, which is the cheapest convincing morning haze there is.
    /// Purely optional - fog COLOUR needs no help, because every shader fogs
    /// toward `voxlSkyColour()` evaluated along its own view ray and therefore
    /// tracks the sky automatically.
    float fogRangeScale = 1.0f;

    /// Ready to hand to `Renderer::setSky()`. All colours linear.
    SkySettings sky{};
};

// ------------------------------------------------------------------- cycle --

class DayNightCycle {
public:
    explicit DayNightCycle(const DayNightConfig& config = DayNightConfig{});

    /// Moves the clock forward by a real-time delta. Negative deltas run the
    /// cycle backwards, which the capture harness uses to bracket a moment.
    void advance(double deltaSeconds) noexcept;

    /// Direct control, for the capture harness, the console and the overlay.
    /// Out-of-range values wrap rather than clamp, so scrubbing past midnight
    /// continues into the next morning instead of sticking.
    void setTimeOfDay(float timeOfDay) noexcept;
    /// Same, expressed as a 24-hour clock. 13.5 is 13:30.
    void setTimeOfDayHours(float hours) noexcept;

    [[nodiscard]] float timeOfDay() const noexcept { return m_state.timeOfDay; }
    [[nodiscard]] float hours() const noexcept { return m_state.timeOfDay * 24.0f; }

    void                 setDayLengthSeconds(double seconds) noexcept;
    [[nodiscard]] double dayLengthSeconds() const noexcept { return m_config.dayLengthSeconds; }

    void               setPaused(bool paused) noexcept { m_config.paused = paused; }
    [[nodiscard]] bool paused() const noexcept { return m_config.paused; }

    /// Replaces the configuration. `startTimeOfDay` is ignored: the clock that
    /// is already running is not reset by retuning the arc.
    void                                 setConfig(const DayNightConfig& config) noexcept;
    [[nodiscard]] const DayNightConfig&  config() const noexcept { return m_config; }

    [[nodiscard]] const DayNightState& state() const noexcept { return m_state; }
    [[nodiscard]] const SkySettings&   sky() const noexcept { return m_state.sky; }
    [[nodiscard]] const glm::vec3&     sunDirection() const noexcept { return m_state.sunDirection; }
    [[nodiscard]] const glm::vec3& moonDirection() const noexcept { return m_state.moonDirection; }
    [[nodiscard]] float            dayFactor() const noexcept { return m_state.dayFactor; }
    [[nodiscard]] float fogRangeScale() const noexcept { return m_state.fogRangeScale; }

    /// "HH:MM" for the debug overlay.
    [[nodiscard]] std::string clockText() const { return clockText(m_state.timeOfDay); }
    [[nodiscard]] static std::string clockText(float timeOfDay);

    /// The whole model, as a pure function. Only `axialTiltDegrees` and
    /// `sunriseAzimuthDegrees` of the config are read.
    [[nodiscard]] static DayNightState sample(float               timeOfDay,
                                              const DayNightConfig& config) noexcept;

private:
    DayNightConfig m_config{};

    /// Authoritative clock, kept in double. A float accumulator fed 1/72000 of
    /// a turn per frame loses its low bits as it approaches 1.0 and the cycle
    /// visibly slows down every evening.
    double m_timeOfDay = 0.0;

    DayNightState m_state{};
};

// ------------------------------------------------------------------ parsing --

/// Parses a command-line or console time. Accepts "13:30", "noon", "midnight",
/// "dawn"/"sunrise", "dusk"/"sunset", and a bare number: values in [0, 1] are
/// read as a normalised fraction, values above 1 as hours on a 24-hour clock.
///
/// That last rule is ambiguous for "1" - it means midnight, not one a.m. Write
/// "1:00" for the hour. Documented rather than guessed at, because a capture
/// script that silently gets the wrong time of day produces a plausible-looking
/// wrong screenshot.
///
/// Returns false and leaves `out` untouched when the text is not a time.
[[nodiscard]] bool parseTimeOfDay(std::string_view text, float& out) noexcept;

}  // namespace voxl
