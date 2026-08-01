#pragma once

#include <map>
#include <filesystem>
#include <variant>

#include <GLFW/glfw3.h>
#include "settings.inl"

using SettingCategoryId = std::string;
using SettingId = std::string;

namespace settings {
    struct InputFloat {
        float value;
    };
    struct InputFloat3 {
        daxa_f32vec3 value;
    };
    struct SliderFloat {
        float value;
        float min;
        float max;
    };
    struct Checkbox {
        bool value;
    };
    struct ComboBox {
        int32_t value;
    };
} // namespace settings

using SettingValue = std::variant<
    settings::InputFloat,
    settings::InputFloat3,
    settings::SliderFloat,
    settings::Checkbox,
    settings::ComboBox>;

struct SettingConfig {
    bool task_graph_depends = false;
    // NOTE(grundlett): This is weird. Where should this go?
    std::vector<std::string> options;
};

// ---------------------------------------------------------------------------------------------
// Quality tiers
// ---------------------------------------------------------------------------------------------
// WHY THIS EXISTS. Upstream ships one configuration: native internal resolution, kajiya TAA,
// reflections always on. On an RTX 3050 6 GB that is 10.46 ms / 96 fps at 1280x720 on the Voxl
// island, and the single measurement that dominates this engine is that frame time is almost
// entirely a function of *internal pixel count*:
//
//     frame_ms = 3.030 + 7.520 x (internal megapixels)
//
// fitted over eight points from 512x288 to 2560x1440 with a maximum residual of 0.44 ms
// (docs/design/RENDERER_OPTIMISATION.md 4). Rendering at 0.75 internal buys 3.20 ms -- as much
// as deleting the entire path-traced GI stack would -- and keeps the look. So the tiers below
// are a resolution ladder first and a feature list second.
//
// Every value in the table is measured, not chosen by feel; the numbers are in
// docs/design/PERFORMANCE_PLAN.md 1.2 and re-verified in docs/benchmarks/presets-2026-07-31.csv.
// Do not edit a row without re-running the A/B in that CSV's header comment.
//
// READ THIS BEFORE BEING SURPRISED THAT QUALITY AND BALANCED SHARE AN INTERNAL SCALE. A tier is
// three settings; it is not the window size, which belongs to the player's display and which no
// preset should be reaching into. PERFORMANCE_PLAN.md 1.2 defines Quality as 1440x791 internal at
// 1920x1055 output and Balanced as 960x540 at 1280x720 -- both 0.75. What actually separates them
// is the output resolution they are meant for, and the upscaler that follows from it: at 1:1 FSR
// 2.2 and the kajiya TAA cost the same, and FSR only pulls ahead once it is genuinely upscaling,
// which at 1080p output it is and at 720p it is not. So: Quality if the window is 1080p-class,
// Balanced if it is 720p-class, Performance if frame rate matters more than near-grass detail.
enum QualityPreset : int32_t {
    // Nothing is applied; the individual Graphics settings stand on their own. Pick this in a
    // benchmark harness that wants to pin one knob at a time, or the tier would clobber it.
    QUALITY_PRESET_CUSTOM = 0,
    QUALITY_PRESET_QUALITY = 1,
    QUALITY_PRESET_BALANCED = 2,
    QUALITY_PRESET_PERFORMANCE = 3,
    QUALITY_PRESET_COUNT,
};

struct QualityTier {
    char const *name;
    // "Graphics"/"Render Res Scale". Multiplies the window size to get the internal render
    // resolution. TAA and post always run at the output resolution.
    float render_res_scale;
    // "Graphics"/"TAA Method": 0 none, 1 kajiya TAA, 2 FSR 2.2. The two cost the same at 1:1;
    // FSR only pulls ahead when it is genuinely upscaling. Measured here at 0.75 internal with
    // reflections off: 1920x1055 output 11.899 ms with kajiya TAA against 11.030 with FSR 2.2,
    // so -0.87 ms -- worth taking. At 1280x720 output the same comparison is 6.559 against
    // 6.401, i.e. 0.16 ms, which is inside the noise floor and not a reason to choose either.
    int32_t taa_method;
    // "Graphics"/"ray_traced_reflections". Measured at 720p native against an interleaved
    // control: -1.55 ms at the vista, -1.14 ms in the cave, -0.37 ms on the moving patrol, where
    // the camera spends most of a lap over open sea and there is little for a reflection ray to
    // find. Off in all three tiers because the scene is made entirely of matte voxels: the sea
    // is pixel-for-pixel identical without it and the only difference is a slightly dimmer broad
    // specular lift. Turn it back on the moment water, glass, ice or metal exists -- that is what
    // the whole ReSTIR reflection stack is for, and this is a toggle rather than a deletion.
    bool ray_traced_reflections;
};

// Indexed by QualityPreset. Row 0 is never applied; it exists so index == enum value.
auto quality_tier(int32_t preset) -> QualityTier const &;

struct SettingEntry {
    SettingValue data;
    SettingValue factory_default;
    SettingValue user_default;
    SettingConfig config;
};

template <typename T>
struct SettingInfo {
    SettingCategoryId category_id = "General";
    SettingId id;
    T factory_default;
    SettingConfig config = {};
};

struct AppSettings {
    // TODO: remove these explicit settings in favor of settings registry
    std::map<daxa_i32, daxa_i32> keybinds;
    std::map<daxa_i32, daxa_i32> mouse_button_binds;

    daxa_f32 mouse_sensitivity;
    std::string world_seed_str;

    static inline AppSettings *s_instance = nullptr;

    std::map<SettingCategoryId, std::map<SettingId, SettingEntry>> categories;

    AppSettings();
    ~AppSettings();

    // The tier a fresh install starts on. docs/design/PERFORMANCE_PLAN.md 1.2 recommends
    // Balanced: it survives the cave (the worst frame in the scene), it survives motion, it
    // leaves headroom for the far field that does not exist yet, and R10-native-vs-presets.png
    // shows it is hard to tell from native at viewing distance. Performance reaches 240 fps
    // today but turns near grass into mush while the camera moves (PERFORMANCE_PLAN.md 7.3).
    static constexpr int32_t shipped_quality_preset = QUALITY_PRESET_BALANCED;

    // Which tier's values are currently reflected in the individual Graphics settings.
    // QUALITY_PRESET_COUNT means "unknown"; see the reconcile in get().
    int32_t applied_quality_preset = QUALITY_PRESET_COUNT;

    // Writes the tier's values into the individual Graphics settings. No-op for Custom, and for
    // any key that is not registered yet. Public so a test or a CLI flag can drive it directly.
    static void apply_quality_preset(int32_t preset);

    // Applies the selected tier if it has changed since the last time this ran. Called from
    // get(), which is the only point every reader of a Graphics setting has in common -- the
    // individual settings are registered from three translation units at three different times,
    // so there is no single "after everything is registered" hook to put this in.
    static void reconcile_quality_preset();

    static void add(SettingCategoryId const &category_id, SettingId const &id, SettingEntry const &entry);
    template <typename T>
    static void add(SettingInfo<T> const &info) {
        add(info.category_id,
            info.id,
            SettingEntry{
                .data = info.factory_default,
                .factory_default = info.factory_default,
                .user_default = info.factory_default,
                .config = info.config,
            });
    }

    static void set(SettingCategoryId const &category_id, SettingId const &id, SettingValue const &value);

    static auto get(SettingCategoryId const &category_id, SettingId const &id) -> SettingEntry;
    template <typename T>
    static auto get(SettingCategoryId const &category_id, SettingId const &id) -> T {
        return std::get<T>(get(category_id, id).data);
    }

    void save(std::filesystem::path const &filepath);
    void load(std::filesystem::path const &filepath);
    void clear();
    void reset_default();

  private:
    // Called from add() for every Graphics key, with first_registration telling it whether there
    // was already a persisted value behind the key.
    static void apply_shipped_default(std::map<SettingId, SettingEntry> &graphics, SettingId const &id, bool first_registration);
};
