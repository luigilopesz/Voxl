#include "world/DayNightCycle.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <format>
#include <system_error>

#include <glm/common.hpp>

namespace voxl {
namespace {

// --------------------------------------------------------------- utilities --

[[nodiscard]] float srgbToLinear(float channel) noexcept
{
    // The exact IEC 61966-2-1 curve rather than a 2.2 power. The linear toe
    // matters here: the night keyframes live down at 0.1-0.3 sRGB, which is
    // precisely where a pure gamma approximation is most wrong.
    return channel <= 0.04045f ? channel / 12.92f
                               : std::pow((channel + 0.055f) / 1.055f, 2.4f);
}

[[nodiscard]] glm::vec3 srgbToLinear(const glm::vec3& colour) noexcept
{
    return glm::vec3{srgbToLinear(colour.r), srgbToLinear(colour.g), srgbToLinear(colour.b)};
}

[[nodiscard]] float smoothStep(float edge0, float edge1, float value) noexcept
{
    const float span = edge1 - edge0;
    if (std::abs(span) < 1e-6f) {
        return value < edge0 ? 0.0f : 1.0f;
    }
    const float t = std::clamp((value - edge0) / span, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

[[nodiscard]] float lerp(float a, float b, float t) noexcept
{
    return a + (b - a) * t;
}

[[nodiscard]] glm::vec3 lerp(const glm::vec3& a, const glm::vec3& b, float t) noexcept
{
    return a + (b - a) * t;
}

// ------------------------------------------------------------- colour grade --

/// One authored moment of the cycle. Colours are sRGB HERE ONLY - they are
/// converted once by `linearKeys()` and never interpolated in this form.
struct SkyKeySrgb {
    float     time;
    glm::vec3 zenith;
    glm::vec3 horizon;
    glm::vec3 sun;
    float     sunIntensity;
    glm::vec3 ambient;
    float     blockLightGain;
    float     aoStrength;
    float     fogRangeScale;
};

/// Warm torch/glowstone colour. Constant across the cycle: a light source does
/// not change colour because the sun set, and only its relative weight against
/// daylight should move (that is `blockLightGain`).
constexpr glm::vec3 kBlockLightSrgb{1.00f, 0.87f, 0.67f};

/// The cycle, authored as nine moments.
///
/// Ordering: strictly increasing `time`, first key at 0. The last key wraps
/// back to the first, so the two ends must be close or midnight shows a step.
///
/// Shape of the grade, and why:
///  - MIDDAY is the neutral reference. Its values reproduce the renderer's own
///    SkySettings defaults exactly, so nothing about the look of a daytime
///    screenshot changes when the cycle is wired in.
///  - DAWN and DUSK push the horizon warm and pull the zenith down, which
///    compresses the zenith-to-horizon range. Low contrast is the point: a
///    sunset with midday's contrast reads as an orange filter, not as a sunset.
///    The warmth here is deliberately restrained, because the horizon colour is
///    the same all the way round the dome: `voxlSkyColour()` has no azimuthal
///    term except the sun's own halo and horizon spill. A blazing orange
///    keyframe therefore sets fire to the EASTERN horizon at sunset too. The
///    directional half of the effect is left to those spill terms, which scale
///    with `sunIntensity` - which is why sunrise and sunset carry a higher
///    intensity than their dimness would suggest.
///  - DUSK is redder than DAWN. Physically that is dust and humidity built up
///    over the day; practically it is what stops the two halves of the cycle
///    from looking like the same event played backwards.
///  - NIGHT is cool and dim but never near black. Ambient stays high enough
///    that outdoor geometry reads by its face shading alone, while the sun's
///    intensity collapses to almost nothing so that block light dominates
///    anywhere the sky cannot reach. A player who cannot see to walk home is a
///    bug report, not atmosphere.
constexpr std::array<SkyKeySrgb, 9> kKeysSrgb{{
    // t      zenith                  horizon                 sun                     sunI   ambient                 blockGain  ao     fog
    {0.00f, {0.13f, 0.16f, 0.30f}, {0.20f, 0.24f, 0.38f}, {0.62f, 0.70f, 1.00f}, 0.035f, {0.33f, 0.38f, 0.54f}, 1.55f, 0.42f, 0.86f},  // midnight
    {0.20f, {0.16f, 0.20f, 0.38f}, {0.42f, 0.32f, 0.38f}, {0.95f, 0.60f, 0.42f}, 0.100f, {0.37f, 0.39f, 0.53f}, 1.48f, 0.44f, 0.80f},  // astronomical dawn
    {0.25f, {0.26f, 0.34f, 0.58f}, {0.70f, 0.48f, 0.40f}, {1.00f, 0.62f, 0.36f}, 0.700f, {0.52f, 0.48f, 0.55f}, 1.30f, 0.46f, 0.76f},  // sunrise
    {0.32f, {0.35f, 0.52f, 0.80f}, {0.94f, 0.80f, 0.68f}, {1.00f, 0.85f, 0.66f}, 0.950f, {0.55f, 0.58f, 0.68f}, 1.14f, 0.53f, 0.90f},  // golden morning
    {0.50f, {0.40f, 0.60f, 0.87f}, {0.80f, 0.88f, 0.97f}, {1.00f, 0.98f, 0.95f}, 1.150f, {0.58f, 0.64f, 0.74f}, 1.05f, 0.58f, 1.00f},  // midday
    {0.68f, {0.38f, 0.56f, 0.84f}, {0.86f, 0.85f, 0.90f}, {1.00f, 0.93f, 0.80f}, 1.000f, {0.57f, 0.61f, 0.71f}, 1.10f, 0.55f, 0.97f},  // afternoon
    {0.75f, {0.28f, 0.32f, 0.60f}, {0.75f, 0.44f, 0.33f}, {1.00f, 0.52f, 0.26f}, 0.700f, {0.54f, 0.46f, 0.50f}, 1.30f, 0.46f, 0.74f},  // sunset
    {0.80f, {0.18f, 0.20f, 0.42f}, {0.50f, 0.30f, 0.38f}, {0.90f, 0.48f, 0.42f}, 0.120f, {0.38f, 0.38f, 0.52f}, 1.46f, 0.44f, 0.78f},  // dusk
    {0.87f, {0.14f, 0.17f, 0.32f}, {0.24f, 0.26f, 0.40f}, {0.65f, 0.72f, 1.00f}, 0.050f, {0.34f, 0.37f, 0.53f}, 1.53f, 0.42f, 0.84f},  // nightfall
}};

/// The same table with every colour in linear space.
struct SkyKeyLinear {
    float     time;
    glm::vec3 zenith;
    glm::vec3 horizon;
    glm::vec3 sun;
    float     sunIntensity;
    glm::vec3 ambient;
    float     blockLightGain;
    float     aoStrength;
    float     fogRangeScale;
};

/// Converted once, on first use, and immutable thereafter. Doing the sRGB
/// decode inside `sample()` would repeat 60 pow() calls per frame for a value
/// that cannot change.
[[nodiscard]] const std::array<SkyKeyLinear, kKeysSrgb.size()>& linearKeys() noexcept
{
    static const std::array<SkyKeyLinear, kKeysSrgb.size()> table = [] {
        std::array<SkyKeyLinear, kKeysSrgb.size()> out{};
        for (std::size_t i = 0; i < kKeysSrgb.size(); ++i) {
            const SkyKeySrgb& in = kKeysSrgb[i];
            out[i]               = SkyKeyLinear{in.time,
                                                srgbToLinear(in.zenith),
                                                srgbToLinear(in.horizon),
                                                srgbToLinear(in.sun),
                                                in.sunIntensity,
                                                srgbToLinear(in.ambient),
                                                in.blockLightGain,
                                                in.aoStrength,
                                                in.fogRangeScale};
        }
        return out;
    }();
    return table;
}

/// Hard floor on ambient light, linear. A SAFETY NET, not a look: every night
/// keyframe already sits comfortably above it. It exists so that a future
/// retune of the table cannot ship a world the player cannot see, which is the
/// one failure of a day/night cycle that makes the game unplayable rather than
/// ugly.
constexpr glm::vec3 kAmbientFloor{0.055f, 0.070f, 0.105f};

/// Blends the two keyframes bracketing `time`, wrapping across midnight.
[[nodiscard]] SkyKeyLinear gradeAt(float time) noexcept
{
    const auto& keys = linearKeys();

    // Last key first: times before the first key belong to the wrapped span.
    std::size_t lower = keys.size() - 1;
    for (std::size_t i = keys.size(); i-- > 0;) {
        if (keys[i].time <= time) {
            lower = i;
            break;
        }
    }
    const std::size_t upper = (lower + 1) % keys.size();

    const float start = keys[lower].time;
    // The wrapped span runs past 1.0 and back to the first key's time.
    const float end  = upper == 0 ? keys[0].time + 1.0f : keys[upper].time;
    const float here = time < start ? time + 1.0f : time;
    const float span = std::max(end - start, 1e-6f);

    // Smoothstep rather than a raw ratio. A linear blend is continuous but its
    // derivative is not, and a kink in the rate of colour change is visible as
    // the sky "catching" as it passes each keyframe.
    const float t = smoothStep(0.0f, 1.0f, (here - start) / span);

    const SkyKeyLinear& a = keys[lower];
    const SkyKeyLinear& b = keys[upper];

    SkyKeyLinear out{};
    out.time           = time;
    out.zenith         = lerp(a.zenith, b.zenith, t);
    out.horizon        = lerp(a.horizon, b.horizon, t);
    out.sun            = lerp(a.sun, b.sun, t);
    out.sunIntensity   = lerp(a.sunIntensity, b.sunIntensity, t);
    out.ambient        = lerp(a.ambient, b.ambient, t);
    out.blockLightGain = lerp(a.blockLightGain, b.blockLightGain, t);
    out.aoStrength     = lerp(a.aoStrength, b.aoStrength, t);
    out.fogRangeScale  = lerp(a.fogRangeScale, b.fogRangeScale, t);
    return out;
}

// ---------------------------------------------------------------- geometry --

constexpr float kPi    = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;

/// Unit vector towards the sun for a normalised time of day.
///
/// The sun travels a circle in a plane that is TILTED out of the vertical, not
/// spun about the view axis: it rises at one point on the horizon, arcs over to
/// one side of the zenith, and sets at the antipodal point. Building it from an
/// orthonormal basis (east, polar-up) rather than from Euler angles is what
/// keeps it exactly unit length for every input, which in turn keeps the
/// renderer's N.L term from breathing as the sun crosses the sky.
[[nodiscard]] glm::vec3 sunDirectionAt(float timeOfDay, float tiltDegrees,
                                       float azimuthDegrees) noexcept
{
    const float tilt    = tiltDegrees * (kPi / 180.0f);
    const float azimuth = azimuthDegrees * (kPi / 180.0f);

    // Horizontal basis. `east` is where the sun comes up; `south` is 90 degrees
    // clockwise from it, and is the side the arc leans toward.
    const glm::vec3 east{std::cos(azimuth), 0.0f, -std::sin(azimuth)};
    const glm::vec3 south{std::sin(azimuth), 0.0f, std::cos(azimuth)};

    // Pole of the orbit, tilted off the zenith. Orthogonal to `east` by
    // construction, because both `up` and `south` are.
    const glm::vec3 polarUp{south.x * std::sin(tilt), std::cos(tilt), south.z * std::sin(tilt)};

    // Angle measured from the sunrise point, so t = 0.25 is exactly the horizon
    // crossing whatever the tilt is.
    const float theta = (timeOfDay - kTimeSunrise) * kTwoPi;
    return east * std::cos(theta) + polarUp * std::sin(theta);
}

// ------------------------------------------------------------------ parsing --

[[nodiscard]] std::string_view trim(std::string_view text) noexcept
{
    const auto isSpace = [](char c) {
        return static_cast<bool>(std::isspace(static_cast<unsigned char>(c)));
    };
    while (!text.empty() && isSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isSpace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] bool equalsIgnoreCase(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto lhs = static_cast<unsigned char>(a[i]);
        const auto rhs = static_cast<unsigned char>(b[i]);
        if (std::tolower(lhs) != std::tolower(rhs)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool parseFloat(std::string_view text, float& out) noexcept
{
    if (text.empty()) {
        return false;
    }
    float      value  = 0.0f;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return false;
    }
    if (!std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

}  // namespace

// -------------------------------------------------------------- free helpers --

float wrapTimeOfDay(float timeOfDay) noexcept
{
    if (!std::isfinite(timeOfDay)) {
        return 0.0f;
    }
    const float wrapped = timeOfDay - std::floor(timeOfDay);
    // std::floor is exact, but (x - floor(x)) can still round up to 1.0f for x
    // a hair below an integer. Returning 1.0 would break the [0,1) postcondition
    // every caller and every test relies on.
    return wrapped >= 1.0f ? 0.0f : wrapped;
}

// -------------------------------------------------------------------- cycle --

DayNightCycle::DayNightCycle(const DayNightConfig& config)
    : m_config(config)
    , m_timeOfDay(static_cast<double>(wrapTimeOfDay(config.startTimeOfDay)))
{
    m_state = sample(static_cast<float>(m_timeOfDay), m_config);
}

void DayNightCycle::advance(double deltaSeconds) noexcept
{
    if (m_config.paused || m_config.dayLengthSeconds <= 0.0 || !std::isfinite(deltaSeconds)) {
        return;
    }
    m_timeOfDay += deltaSeconds / m_config.dayLengthSeconds;
    m_timeOfDay -= std::floor(m_timeOfDay);
    m_state = sample(static_cast<float>(m_timeOfDay), m_config);
}

void DayNightCycle::setTimeOfDay(float timeOfDay) noexcept
{
    m_timeOfDay = static_cast<double>(wrapTimeOfDay(timeOfDay));
    m_state     = sample(static_cast<float>(m_timeOfDay), m_config);
}

void DayNightCycle::setTimeOfDayHours(float hoursOfDay) noexcept
{
    setTimeOfDay(hoursOfDay / 24.0f);
}

void DayNightCycle::setDayLengthSeconds(double seconds) noexcept
{
    m_config.dayLengthSeconds = seconds;
}

void DayNightCycle::setConfig(const DayNightConfig& config) noexcept
{
    m_config = config;
    m_state  = sample(static_cast<float>(m_timeOfDay), m_config);
}

std::string DayNightCycle::clockText(float timeOfDay)
{
    const float wrapped = wrapTimeOfDay(timeOfDay);
    // Round to the nearest minute, then wrap again: 23:59:40 rounds to 1440
    // minutes, which must print as 00:00 rather than 24:00.
    const int totalMinutes = static_cast<int>(std::lround(wrapped * 1440.0f)) % 1440;
    return std::format("{:02}:{:02}", totalMinutes / 60, totalMinutes % 60);
}

DayNightState DayNightCycle::sample(float timeOfDay, const DayNightConfig& config) noexcept
{
    DayNightState state{};
    state.timeOfDay = wrapTimeOfDay(timeOfDay);

    state.sunDirection =
        sunDirectionAt(state.timeOfDay, config.axialTiltDegrees, config.sunriseAzimuthDegrees);
    // Exactly antipodal. Deriving it rather than sampling a second orbit means
    // the shaders can reconstruct it as -sunDirection with no extra uniform and
    // no chance of the two disagreeing.
    state.moonDirection = -state.sunDirection;
    state.sunElevation  = state.sunDirection.y;

    // Twilight band, in sine-of-elevation. It reaches well below zero because
    // the sky is already lit while the sun's disc is still under the horizon,
    // and because this is what fades the stars: a band tight around the horizon
    // makes them snap out over about half a minute of real time, which reads as
    // a bug. -0.28 is roughly an hour and a quarter of in-game twilight.
    state.dayFactor   = smoothStep(-0.28f, 0.08f, state.sunElevation);
    state.nightFactor = 1.0f - state.dayFactor;

    const SkyKeyLinear grade = gradeAt(state.timeOfDay);

    SkySettings& sky     = state.sky;
    sky.sunDirection     = state.sunDirection;
    sky.sunColour        = grade.sun;
    sky.sunIntensity     = grade.sunIntensity;
    sky.zenithColour     = grade.zenith;
    sky.horizonColour    = grade.horizon;
    sky.ambientColour    = glm::max(grade.ambient, kAmbientFloor);
    sky.blockLightColour = srgbToLinear(kBlockLightSrgb);
    sky.blockLightGain   = grade.blockLightGain;
    sky.aoStrength       = grade.aoStrength;
    sky.dayFactor        = state.dayFactor;

    state.fogRangeScale = grade.fogRangeScale;
    return state;
}

// ------------------------------------------------------------------ parsing --

bool parseTimeOfDay(std::string_view text, float& out) noexcept
{
    const std::string_view trimmed = trim(text);
    if (trimmed.empty()) {
        return false;
    }

    struct NamedTime {
        std::string_view name;
        float            time;
    };
    constexpr std::array<NamedTime, 8> kNames{{
        {"midnight", kTimeMidnight},
        {"dawn", kTimeSunrise},
        {"sunrise", kTimeSunrise},
        {"morning", 0.32f},
        {"noon", kTimeNoon},
        {"midday", kTimeNoon},
        {"sunset", kTimeSunset},
        {"dusk", 0.79f},
    }};
    for (const NamedTime& named : kNames) {
        if (equalsIgnoreCase(trimmed, named.name)) {
            out = named.time;
            return true;
        }
    }

    if (const std::size_t colon = trimmed.find(':'); colon != std::string_view::npos) {
        float hoursPart   = 0.0f;
        float minutesPart = 0.0f;
        if (!parseFloat(trim(trimmed.substr(0, colon)), hoursPart) ||
            !parseFloat(trim(trimmed.substr(colon + 1)), minutesPart)) {
            return false;
        }
        if (hoursPart < 0.0f || minutesPart < 0.0f || minutesPart >= 60.0f) {
            return false;
        }
        out = wrapTimeOfDay((hoursPart + minutesPart / 60.0f) / 24.0f);
        return true;
    }

    float value = 0.0f;
    if (!parseFloat(trimmed, value)) {
        return false;
    }
    // See the header: [0,1] is a fraction of a day, anything larger is hours.
    out = wrapTimeOfDay(value > 1.0f ? value / 24.0f : value);
    return true;
}

}  // namespace voxl
