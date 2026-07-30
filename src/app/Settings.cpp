#include "app/Settings.hpp"

#include "core/Log.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>

#ifdef _WIN32
    #include <windows.h>
#endif

namespace voxl {
namespace {

using namespace settings_limits;

// ------------------------------------------------------------- text utils --

[[nodiscard]] constexpr bool isSpace(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

[[nodiscard]] std::string_view trim(std::string_view text) noexcept
{
    while (!text.empty() && isSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isSpace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

[[nodiscard]] char lower(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool equalsIgnoreCase(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

// ------------------------------------------------------------- scalar I/O --
//
// std::from_chars throughout, never std::stof or istream >>: those are
// locale-sensitive, and a machine set to a comma decimal separator would write
// "0,12" and then parse it back as 0. A settings file must mean the same thing
// on every machine.

[[nodiscard]] bool parseFloat(std::string_view text, float& out) noexcept
{
    const char* begin = text.data();
    const char* end   = text.data() + text.size();
    float       value = 0.0f;

    const std::from_chars_result result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        return false;
    }
    // NaN and infinity would defeat the clamp - std::clamp on a NaN returns the
    // NaN - and then poison the projection matrix or the streaming radius.
    if (!std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

[[nodiscard]] bool parseInt(std::string_view text, std::int32_t& out) noexcept
{
    const char*  begin = text.data();
    const char*  end   = text.data() + text.size();
    std::int32_t value = 0;

    const std::from_chars_result result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        return false;
    }
    out = value;
    return true;
}

/// Accepts the spellings a human actually types. Anything else is malformed
/// rather than "falsey": silently reading `vsync = yse` as false would be a
/// setting that mysteriously refuses to stick.
[[nodiscard]] bool parseBool(std::string_view text, bool& out) noexcept
{
    constexpr std::string_view kTrue[]{"true", "1", "on", "yes"};
    constexpr std::string_view kFalse[]{"false", "0", "off", "no"};

    for (std::string_view candidate : kTrue) {
        if (equalsIgnoreCase(text, candidate)) {
            out = true;
            return true;
        }
    }
    for (std::string_view candidate : kFalse) {
        if (equalsIgnoreCase(text, candidate)) {
            out = false;
            return true;
        }
    }
    return false;
}

/// `5,9,14` into `kLodMax` integers. All-or-nothing: a partially parsed band
/// table is worse than none, because the half that parsed would be repaired by
/// the clamp into something nobody asked for.
[[nodiscard]] bool parseBands(std::string_view text, std::int32_t (&out)[kLodMax]) noexcept
{
    std::int32_t values[kLodMax]{};
    for (std::size_t i = 0; i < std::size_t{kLodMax}; ++i) {
        const std::size_t      comma = text.find(',');
        const std::string_view field = trim(text.substr(0, comma));
        if (!parseInt(field, values[i])) {
            return false;
        }
        const bool last = (i + 1 == std::size_t{kLodMax});
        if ((comma == std::string_view::npos) != last) {
            return false;  // too few fields, or trailing junk after the last
        }
        if (!last) {
            text.remove_prefix(comma + 1);
        }
    }
    std::copy(std::begin(values), std::end(values), std::begin(out));
    return true;
}

// ------------------------------------------------------------- formatting --

/// Shortest representation that reads back bit-identically. `std::format`'s
/// default float formatting is exactly that, which is what makes the
/// write/read round-trip an equality rather than an approximation.
[[nodiscard]] std::string formatFloat(float value)
{
    return std::format("{}", value);
}

/// Clamps and counts in one place so `clampToValidRange` can report how much it
/// had to change without repeating the comparison at every field.
template <typename T>
void clampField(T& value, T low, T high, std::size_t& changed) noexcept
{
    const T clamped = std::clamp(value, low, high);
    if (clamped != value) {
        value = clamped;
        ++changed;
    }
}

}  // namespace

// ------------------------------------------------------------------ clamp --

std::size_t Settings::clampToValidRange() noexcept
{
    std::size_t changed = 0;

    clampField(renderDistance, kRenderDistanceMin, kRenderDistanceMax, changed);
    clampField(fovDegrees, kFovMin, kFovMax, changed);
    clampField(mouseSensitivity, kMouseSensitivityMin, kMouseSensitivityMax, changed);
    clampField(anisotropy, kAnisotropyMin, kAnisotropyMax, changed);
    clampField(fogDistanceScale, kFogDistanceScaleMin, kFogDistanceScaleMax, changed);
    clampField(guiScale, kGuiScaleMin, kGuiScaleMax, changed);
    clampField(masterVolume, kVolumeMin, kVolumeMax, changed);
    clampField(worldVolume, kVolumeMin, kVolumeMax, changed);
    clampField(uiVolume, kVolumeMin, kVolumeMax, changed);
    clampField(dayLengthMinutes, kDayLengthMinutesMin, kDayLengthMinutesMax, changed);

    // The cap has a hole in its range: 0 means unlimited, and everything
    // between 1 and kFpsCapMin - 1 is a frame rate nobody wants and that would
    // read as a hang. Snap up to the minimum rather than down to unlimited: a
    // player who typed 5 wanted a cap.
    if (fpsCap != kFpsCapUnlimited) {
        const std::int32_t clamped = std::clamp(fpsCap, kFpsCapMin, kFpsCapMax);
        if (clamped != fpsCap) {
            fpsCap = clamped;
            ++changed;
        }
    }

    // LOD bands: ascending, each at least kLodMinBandWidth wide, and with
    // enough headroom left above band i for every band after it. The upper
    // bound shrinks per index so the lower bound of a later band can never
    // exceed its own upper bound, which would make std::clamp undefined.
    for (std::size_t i = 0; i < std::size_t{kLodMax}; ++i) {
        const std::int32_t low =
            (i == 0) ? kLodMinBandWidth : lodBandStart[i - 1] + kLodMinBandWidth;
        const std::int32_t high =
            kLodBandMax - static_cast<std::int32_t>(std::size_t{kLodMax} - 1 - i) * kLodMinBandWidth;
        clampField(lodBandStart[i], low, high, changed);
    }

    return changed;
}

// ------------------------------------------------------------------ write --

std::string writeSettings(const Settings& settings, const std::vector<std::string>& unknownLines)
{
    std::string out;
    out.reserve(1024 + unknownLines.size() * 32);

    const auto line = [&out](std::string_view text) {
        out.append(text);
        out.push_back('\n');
    };
    const auto key = [&out](std::string_view name, std::string_view value) {
        out.append(name);
        out.append(" = ");
        out.append(value);
        out.push_back('\n');
    };
    const auto boolKey = [&key](std::string_view name, bool value) {
        key(name, value ? "true" : "false");
    };

    line("# Voxl settings.");
    line("# Written by the game; safe to edit by hand.");
    line("#   - a line this build does not understand is kept, not deleted");
    line("#   - a value outside its range is clamped on load, not rejected");
    line("version = 1");
    line("");

    line("[video]");
    key("render_distance", std::format("{}", settings.renderDistance));
    key("fov", formatFloat(settings.fovDegrees));
    boolKey("vsync", settings.vsync);
    key("fps_cap", std::format("{}", settings.fpsCap));
    key("anisotropy", formatFloat(settings.anisotropy));
    key("fog_distance_scale", formatFloat(settings.fogDistanceScale));
    key("gui_scale", formatFloat(settings.guiScale));
    line("");

    line("[controls]");
    key("mouse_sensitivity", formatFloat(settings.mouseSensitivity));
    boolKey("invert_mouse_y", settings.invertMouseY);
    line("");

    line("[audio]");
    key("volume_master", formatFloat(settings.masterVolume));
    key("volume_world", formatFloat(settings.worldVolume));
    key("volume_ui", formatFloat(settings.uiVolume));
    line("");

    line("[world]");
    key("day_length_minutes", formatFloat(settings.dayLengthMinutes));
    boolKey("lod_enabled", settings.lodEnabled);
    {
        std::string bands;
        for (std::size_t i = 0; i < std::size_t{kLodMax}; ++i) {
            if (i != 0) {
                bands.push_back(',');
            }
            bands.append(std::format("{}", settings.lodBandStart[i]));
        }
        key("lod_bands", bands);
    }

    if (!unknownLines.empty()) {
        line("");
        line("# Keys this build does not recognise, preserved verbatim.");
        for (const std::string& preserved : unknownLines) {
            line(preserved);
        }
    }

    return out;
}

// ------------------------------------------------------------------- read --

SettingsParseReport readSettings(std::string_view text, Settings& out)
{
    out = Settings{};
    SettingsParseReport report;

    std::size_t cursor = 0;
    while (cursor <= text.size()) {
        const std::size_t      newline = text.find('\n', cursor);
        const std::size_t      stop    = (newline == std::string_view::npos) ? text.size() : newline;
        const std::string_view raw     = trim(text.substr(cursor, stop - cursor));
        cursor                         = stop + 1;

        if (raw.empty() || raw.front() == '#' || raw.front() == ';') {
            if (newline == std::string_view::npos) {
                break;
            }
            continue;
        }
        // Section headers are pure decoration: writeSettings emits them for
        // readability and the parser has no use for them, but they must not be
        // counted as malformed or the report would cry wolf on its own output.
        if (raw.front() == '[' && raw.back() == ']') {
            if (newline == std::string_view::npos) {
                break;
            }
            continue;
        }

        const std::size_t equals = raw.find('=');
        if (equals == std::string_view::npos) {
            ++report.malformedLines;
            VOXL_LOG_WARN("settings: skipping line with no '=': '{}'", raw);
            if (newline == std::string_view::npos) {
                break;
            }
            continue;
        }

        const std::string_view name  = trim(raw.substr(0, equals));
        const std::string_view value = trim(raw.substr(equals + 1));

        if (name.empty()) {
            ++report.malformedLines;
            VOXL_LOG_WARN("settings: skipping line with an empty key: '{}'", raw);
            if (newline == std::string_view::npos) {
                break;
            }
            continue;
        }

        // `handled` separates "this build knows the key" from "the value
        // parsed". A known key with a bad value is malformed and keeps its
        // default; an unknown key is preserved for whoever does understand it.
        bool handled = true;
        bool parsed  = true;

        if (name == "version") {
            std::int32_t version = 0;
            parsed               = parseInt(value, version);
            // Nothing to migrate yet. A future format change reads this and
            // rewrites; for now it exists so that file is self-describing.
        } else if (name == "render_distance") {
            parsed = parseInt(value, out.renderDistance);
        } else if (name == "fov") {
            parsed = parseFloat(value, out.fovDegrees);
        } else if (name == "vsync") {
            parsed = parseBool(value, out.vsync);
        } else if (name == "fps_cap") {
            parsed = parseInt(value, out.fpsCap);
        } else if (name == "anisotropy") {
            parsed = parseFloat(value, out.anisotropy);
        } else if (name == "fog_distance_scale") {
            parsed = parseFloat(value, out.fogDistanceScale);
        } else if (name == "gui_scale") {
            parsed = parseFloat(value, out.guiScale);
        } else if (name == "mouse_sensitivity") {
            parsed = parseFloat(value, out.mouseSensitivity);
        } else if (name == "invert_mouse_y") {
            parsed = parseBool(value, out.invertMouseY);
        } else if (name == "volume_master") {
            parsed = parseFloat(value, out.masterVolume);
        } else if (name == "volume_world") {
            parsed = parseFloat(value, out.worldVolume);
        } else if (name == "volume_ui") {
            parsed = parseFloat(value, out.uiVolume);
        } else if (name == "day_length_minutes") {
            parsed = parseFloat(value, out.dayLengthMinutes);
        } else if (name == "lod_enabled") {
            parsed = parseBool(value, out.lodEnabled);
        } else if (name == "lod_bands") {
            parsed = parseBands(value, out.lodBandStart);
        } else {
            handled = false;
        }

        if (!handled) {
            ++report.unknownKeys;
            report.unknownLines.emplace_back(raw);
        } else if (!parsed) {
            ++report.malformedLines;
            VOXL_LOG_WARN("settings: '{}' has an unreadable value '{}'; keeping the default", name,
                          value);
        } else {
            ++report.keysApplied;
        }

        if (newline == std::string_view::npos) {
            break;
        }
    }

    report.clampedValues = out.clampToValidRange();
    return report;
}

// -------------------------------------------------------------- filesystem --

std::filesystem::path executableDirectory()
{
    std::error_code error;

#ifdef _WIN32
    // GetModuleFileNameW is the only way to get this that does not depend on
    // the working directory, which the player controls (a shortcut's "Start in"
    // field) and which must not decide where settings live.
    std::wstring buffer(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 5; ++attempt) {
        const DWORD written =
            ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            break;
        }
        if (written < buffer.size()) {
            buffer.resize(written);
            std::filesystem::path path{buffer};
            return path.parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#endif

    const std::filesystem::path here = std::filesystem::current_path(error);
    return error ? std::filesystem::path{"."} : here;
}

std::filesystem::path defaultSettingsPath()
{
    return executableDirectory() / "settings.cfg";
}

std::filesystem::path defaultSavesDirectory()
{
    return executableDirectory() / "saves";
}

bool loadSettingsFile(const std::filesystem::path& path, Settings& out,
                      SettingsParseReport* report)
{
    out = Settings{};

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (report != nullptr) {
            *report = SettingsParseReport{};
        }
        return false;
    }

    std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    // A read that failed halfway (a locked or truncated file) must not be
    // parsed: the tail would look like "keys missing" and silently reset those
    // settings to defaults on the next save.
    if (file.bad()) {
        VOXL_LOG_ERROR("settings: read error on '{}'; keeping defaults", path.string());
        if (report != nullptr) {
            *report = SettingsParseReport{};
        }
        return false;
    }

    const SettingsParseReport result = readSettings(text, out);
    if (!result.clean()) {
        VOXL_LOG_WARN(
            "settings: loaded '{}' with {} unknown key(s), {} malformed line(s), {} clamped "
            "value(s)",
            path.string(), result.unknownKeys, result.malformedLines, result.clampedValues);
    }
    if (report != nullptr) {
        *report = result;
    }
    return true;
}

bool saveSettingsFile(const std::filesystem::path& path, const Settings& settings,
                      const std::vector<std::string>& unknownLines)
{
    const std::string text = writeSettings(settings, unknownLines);

    std::error_code             error;
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        error.clear();  // an existing directory reports no error; a real failure surfaces below
    }

    // Write-then-rename. A power cut or a crash during the write leaves the
    // previous settings intact instead of a half-written file that the next
    // launch would read as "most keys missing" and then overwrite for good.
    std::filesystem::path temporary = path;
    temporary += ".tmp";

    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            VOXL_LOG_ERROR("settings: cannot open '{}' for writing", temporary.string());
            return false;
        }
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.flush();
        if (!file) {
            VOXL_LOG_ERROR("settings: write failed for '{}'", temporary.string());
            return false;
        }
    }

    std::filesystem::rename(temporary, path, error);
    if (error) {
        VOXL_LOG_ERROR("settings: cannot replace '{}': {}", path.string(), error.message());
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }

    VOXL_LOG_INFO("settings: saved to '{}'", path.string());
    return true;
}

}  // namespace voxl
