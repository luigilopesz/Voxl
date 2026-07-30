#pragma once

// User-facing settings and their on-disk representation.
//
// WHY A HAND-ROLLED KEY/VALUE FORMAT
// ----------------------------------
// The file sits beside the executable and is meant to be opened in a text
// editor by a player who wants a render distance the slider does not offer, so
// it is line-oriented `key = value` with `# comments` and `[sections]`. JSON
// would need a dependency (forbidden) and would turn a single misplaced comma
// into "your settings are gone".
//
// THREE PROPERTIES THE PARSER GUARANTEES, each of which is a test:
//
//  1. A malformed line loses only that line. Parsing never aborts, so a file
//     whose third line is garbage still applies lines four onward.
//  2. A key this build does not know is PRESERVED VERBATIM and written back on
//     the next save. Several agents are adding settings to this file in
//     parallel; an older binary must not silently delete a newer binary's keys,
//     because the player would then lose them by launching the wrong exe once.
//  3. Out-of-range values are CLAMPED, never accepted. `render_distance = 9000`
//     must not be able to make the streamer try to hold 30 million chunks. The
//     clamp runs after parsing, so it also fixes a file edited by hand.
//
// Missing keys are simply left at their default: the parser starts from a
// default-constructed `Settings` and overwrites only what the file mentions.
//
// Thread safety: none of this is synchronised. Settings are main-thread state.

#include "world/Lod.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace voxl {

/// Inclusive bounds enforced by `Settings::clampToValidRange()`. Shared with the
/// settings UI so a slider can never produce a value the clamp would then
/// silently change underneath the player.
namespace settings_limits {

inline constexpr std::int32_t kRenderDistanceMin = 2;
inline constexpr std::int32_t kRenderDistanceMax = 48;

inline constexpr float kFovMin = 30.0f;
inline constexpr float kFovMax = 110.0f;

inline constexpr float kMouseSensitivityMin = 0.01f;
inline constexpr float kMouseSensitivityMax = 2.0f;

/// 0 is the sentinel for "uncapped" and is deliberately outside the range: a
/// cap below ~15 fps is indistinguishable from a hang, and one above 1000 is
/// past the point where the limiter's spin phase costs more than it saves.
inline constexpr std::int32_t kFpsCapUnlimited = 0;
inline constexpr std::int32_t kFpsCapMin       = 15;
inline constexpr std::int32_t kFpsCapMax       = 1000;

inline constexpr float kVolumeMin = 0.0f;
inline constexpr float kVolumeMax = 1.0f;

/// The renderer caps anisotropy at 8x internally and the driver caps it again,
/// so 16 here only means "ask for as much as you can get".
inline constexpr float kAnisotropyMin = 1.0f;
inline constexpr float kAnisotropyMax = 16.0f;

inline constexpr float kFogDistanceScaleMin = 0.25f;
inline constexpr float kFogDistanceScaleMax = 2.0f;

inline constexpr float kGuiScaleMin = 0.60f;
inline constexpr float kGuiScaleMax = 2.50f;

inline constexpr float kDayLengthMinutesMin = 0.5f;
inline constexpr float kDayLengthMinutesMax = 240.0f;

/// Narrowest a LOD band may be, from the invariant stated in world/Lod.hpp: a
/// band narrower than `hysteresis + 2` can be demoted out of but never promoted
/// back into, so a chunk that lands in it stays coarse forever. ChunkManager
/// repairs this too; doing it here as well means the UI cannot even offer the
/// broken configuration.
inline constexpr std::int32_t kLodMinBandWidth = LodPolicy{}.hysteresis + 2;
inline constexpr std::int32_t kLodBandMax      = 128;

}  // namespace settings_limits

/// Everything the player can change. Plain data: no behaviour beyond clamping,
/// so it can be copied around, diffed and compared in a test.
struct Settings {
    // ------------------------------------------------------------- video --

    /// Streaming load radius in chunks. Live-applied; see
    /// `StreamingConfig::loadRadius`.
    std::int32_t renderDistance = 20;

    /// Vertical field of view in degrees.
    float fovDegrees = 70.0f;

    bool vsync = true;

    /// Frames per second, or `kFpsCapUnlimited`.
    ///
    /// NOT redundant with vsync. On hybrid-graphics laptops the WGL swap
    /// interval is advisory and the driver may ignore it entirely, which is why
    /// core/FrameLimiter.hpp exists.
    std::int32_t fpsCap = settings_limits::kFpsCapUnlimited;

    /// Requested anisotropic sample count for the block texture array.
    float anisotropy = 8.0f;

    /// Multiplies the fog distances the renderer derives from the view
    /// distance. Below 1 brings the fog in (hides pop-in, shortens the view);
    /// above 1 pushes it out.
    float fogDistanceScale = 1.0f;

    /// Multiplies every ImGui size and the font. 1 is the authored size.
    float guiScale = 1.0f;

    // ---------------------------------------------------------- controls --

    /// Degrees of rotation per pixel of mouse movement;
    /// `PlayerConfig::mouseSensitivity`.
    float mouseSensitivity = 0.12f;
    bool  invertMouseY     = false;

    // ------------------------------------------------------------- audio --

    /// All three are linear gains in [0, 1]. Master multiplies the other two.
    float masterVolume = 0.8f;
    float worldVolume  = 1.0f;
    float uiVolume     = 0.7f;

    // ------------------------------------------------------------- world --

    /// Wall-clock minutes for one full day/night cycle.
    float dayLengthMinutes = 20.0f;

    /// `LodPolicy::bandStart`. Ascending, and each band at least
    /// `kLodMinBandWidth` wide - the clamp enforces both.
    std::int32_t lodBandStart[kLodMax] = {5, 9, 14};

    /// False pins every chunk to level 0.
    bool lodEnabled = true;

    friend bool operator==(const Settings&, const Settings&) = default;

    /// Forces every field into its documented range and repairs the LOD band
    /// table. Returns how many fields had to be changed, which is what lets the
    /// loader report "your file had 3 out-of-range values" instead of silently
    /// rewriting it.
    std::size_t clampToValidRange() noexcept;
};

// ------------------------------------------------------------- text format --

/// What a parse did, for logging and for tests.
struct SettingsParseReport {
    /// Keys this build understood and applied.
    std::size_t keysApplied = 0;
    /// Well-formed `key = value` lines whose key this build does not know.
    std::size_t unknownKeys = 0;
    /// Lines that are neither blank, comment, section, nor `key = value` - or
    /// whose value would not parse as the key's type.
    std::size_t malformedLines = 0;
    /// Fields the clamp had to move.
    std::size_t clampedValues = 0;

    /// The unknown lines, verbatim and trimmed. Hand these back to
    /// `writeSettings` so a save does not delete another build's keys.
    std::vector<std::string> unknownLines;

    [[nodiscard]] bool clean() const noexcept
    {
        return unknownKeys == 0 && malformedLines == 0 && clampedValues == 0;
    }
};

/// Renders `settings` as the file's text, appending `unknownLines` in a clearly
/// marked block at the end.
///
/// Floats are written with `std::format`'s shortest round-trippable form, so
/// `readSettings(writeSettings(s)) == s` holds exactly rather than approximately.
[[nodiscard]] std::string writeSettings(const Settings&                 settings,
                                        const std::vector<std::string>& unknownLines = {});

/// Parses `text` into `out`.
///
/// `out` is reset to defaults first, so a key absent from the text ends up at
/// its default rather than at whatever the caller happened to have there. Never
/// fails: the report says what was wrong.
SettingsParseReport readSettings(std::string_view text, Settings& out);

// -------------------------------------------------------------- filesystem --

/// Directory containing the running executable, or the current directory when
/// the OS will not say. Settings and saves both live beside the binary so the
/// shipped layout matches the development one.
[[nodiscard]] std::filesystem::path executableDirectory();

/// `executableDirectory() / "settings.cfg"`.
[[nodiscard]] std::filesystem::path defaultSettingsPath();

/// `executableDirectory() / "saves"`. Not created here; the persistence layer
/// owns that.
[[nodiscard]] std::filesystem::path defaultSavesDirectory();

/// Reads `path`. Returns false when the file does not exist or cannot be read,
/// in which case `out` holds defaults - which is the correct first-run
/// behaviour, so a false return is not necessarily an error worth showing.
bool loadSettingsFile(const std::filesystem::path& path, Settings& out,
                      SettingsParseReport* report = nullptr);

/// Writes `path` atomically-ish: the text goes to `path + ".tmp"` and is then
/// renamed over the target, so a crash mid-write cannot leave a truncated file
/// that the next launch would parse as "half the keys are missing".
bool saveSettingsFile(const std::filesystem::path& path, const Settings& settings,
                      const std::vector<std::string>& unknownLines = {});

}  // namespace voxl
