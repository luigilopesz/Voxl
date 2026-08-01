#include "ui.hpp"
#include <nlohmann/json.hpp>
#include <fmt/format.h>
#include <fstream>
#include <numbers>

#include "input.inl"

AppSettings::AppSettings() {
    // assert(s_instance == nullptr);
    s_instance = this;
}
AppSettings::~AppSettings() {
    s_instance = nullptr;
}

// ---------------------------------------------------------------------------------------------
// Quality tiers. See the table in settings.hpp for what each field means and where it was
// measured. The rows here are the values themselves.
// ---------------------------------------------------------------------------------------------
namespace {
    constexpr auto GRAPHICS_CATEGORY = "Graphics";
    // No decoration on the name, deliberately. An earlier version had a leading space so the row
    // would sort to the top of the std::map that backs the category -- and it did, but it also
    // made the key unreachable from `--set`, because the shell ate the space before argv saw it.
    // Q sorts before every other Graphics key anyway (R, T, U, then the lowercase ones), so the
    // ordering came for free and the space bought nothing but a broken command line.
    constexpr auto QUALITY_PRESET_ID = "Quality Preset";

    // Measured on an RTX 3050 6 GB Laptop, Voxl island, profiler and overlay off, p50 of full_ms
    // over t in [8 s, end-0.5 s], control interleaved with treatment, GPU verified uncontended
    // for every run. Raw data: docs/benchmarks/presets-2026-07-31.csv.
    //
    //   720p output, still at the spawn:   control 10.984 ms / 91 fps
    //   720p output, moving (12 m circle): control  8.951 ms / 112 fps
    //   720p output, inside the cave:      control  9.090 ms / 110 fps
    constexpr QualityTier QUALITY_TIERS[QUALITY_PRESET_COUNT] = {
        // Custom is never applied; the row exists only so index == enum value.
        {"Custom", 1.00f, 1, true},
        // Quality -- for a 1080p-class window, where 0.75 internal is 1440x791 and sits almost
        // exactly on the one-voxel-per-pixel point for the far field that is planned
        // (PERFORMANCE_PLAN.md 5.4). FSR 2.2 rather than kajiya TAA because upscaling is the only
        // regime in which the two differ in cost at all.
        {"Quality", 0.75f, 2, false},
        // Balanced -- for a 720p-class window. 6.559 ms / 152 fps still, 5.650 / 177 moving,
        // 6.029 / 166 in the cave. Needle clumps and canopy sky-gaps still resolve.
        {"Balanced", 0.75f, 1, false},
        // Performance -- 640x360 internal at 720p. 4.472 ms / 224 fps still, 3.953 / 253 moving,
        // 4.331 / 231 in the cave. The tier that reaches 240 fps, and the one that costs the most
        // in motion: near grass turns to mush and the flowers pick up cyan fringing.
        {"Performance", 0.50f, 1, false},
    };

    auto quality_preset_options() -> std::vector<std::string> {
        auto options = std::vector<std::string>{};
        for (auto const &tier : QUALITY_TIERS) {
            options.emplace_back(tier.name);
        }
        return options;
    }

    // Guards the lazy registration in add() against re-entering itself, since registering the
    // preset combo box is itself an add() into the Graphics category.
    bool g_registering_quality_preset = false;
} // namespace

auto quality_tier(int32_t preset) -> QualityTier const & {
    if (preset < 0 || preset >= QUALITY_PRESET_COUNT) {
        return QUALITY_TIERS[QUALITY_PRESET_CUSTOM];
    }
    return QUALITY_TIERS[static_cast<size_t>(preset)];
}

namespace {
    // Which of an entry's three slots a write lands in. Selecting a tier is a *choice* and must
    // move only the live value; deciding the shipped default is a different act and also moves
    // the factory default, so that "Factory Reset" in the settings UI returns to the tier this
    // build ships rather than to upstream's native-resolution configuration.
    struct PokeTargets {
        bool data = false;
        bool factory_default = false;
        bool user_default = false;
    };

    // Overwrites the value inside an existing entry without disturbing anything else about it.
    // In particular a SliderFloat keeps the min/max its owner registered -- duplicating those
    // here would be a second source of truth that nothing would ever check.
    template <typename Assign>
    void poke(std::map<SettingId, SettingEntry> &category, SettingId const &id, PokeTargets targets, Assign &&assign) {
        auto iter = category.find(id);
        if (iter == category.end()) {
            return;
        }
        if (targets.data) {
            assign(iter->second.data);
        }
        if (targets.factory_default) {
            assign(iter->second.factory_default);
        }
        if (targets.user_default) {
            assign(iter->second.user_default);
        }
    }

    void poke_float(std::map<SettingId, SettingEntry> &category, SettingId const &id, float value, PokeTargets targets) {
        poke(category, id, targets, [value](SettingValue &slot) {
            if (auto *slider = std::get_if<settings::SliderFloat>(&slot)) {
                slider->value = value;
            } else if (auto *input = std::get_if<settings::InputFloat>(&slot)) {
                input->value = value;
            }
        });
    }

    void poke_int(std::map<SettingId, SettingEntry> &category, SettingId const &id, int32_t value, PokeTargets targets) {
        poke(category, id, targets, [value](SettingValue &slot) {
            if (auto *combo = std::get_if<settings::ComboBox>(&slot)) {
                combo->value = value;
            }
        });
    }

    void poke_bool(std::map<SettingId, SettingEntry> &category, SettingId const &id, bool value, PokeTargets targets) {
        poke(category, id, targets, [value](SettingValue &slot) {
            if (auto *checkbox = std::get_if<settings::Checkbox>(&slot)) {
                checkbox->value = value;
            }
        });
    }
} // namespace

void AppSettings::apply_quality_preset(int32_t preset) {
    auto &self = *s_instance;
    self.applied_quality_preset = preset;
    if (preset <= QUALITY_PRESET_CUSTOM || preset >= QUALITY_PRESET_COUNT) {
        return;
    }
    auto category_iter = self.categories.find(GRAPHICS_CATEGORY);
    if (category_iter == self.categories.end()) {
        return;
    }
    auto &graphics = category_iter->second;
    auto const &tier = quality_tier(preset);
    // KNOWN LIMITATION, pre-existing and not introduced here. Switching a tier at runtime moves
    // "TAA Method" and "ray_traced_reflections" immediately, because both are read inside
    // record_tasks(). "Render Res Scale" does not: VoxelApp caches it in its own render_res_scl
    // member, which is refreshed only by on_resize(), so a re-record uses the previous scale
    // until the window is actually resized. At launch every tier applies in full, which is what
    // the shipped default and every measurement in docs/benchmarks/ depend on. The one-line fix
    // belongs in VoxelApp::record_tasks() and is written up in this agent's integration notes.
    constexpr auto LIVE_VALUE = PokeTargets{.data = true};
    poke_float(graphics, "Render Res Scale", tier.render_res_scale, LIVE_VALUE);
    poke_int(graphics, "TAA Method", tier.taa_method, LIVE_VALUE);
    poke_bool(graphics, "ray_traced_reflections", tier.ray_traced_reflections, LIVE_VALUE);
}

void AppSettings::reconcile_quality_preset() {
    auto &self = *s_instance;
    auto category_iter = self.categories.find(GRAPHICS_CATEGORY);
    if (category_iter == self.categories.end()) {
        return;
    }
    auto entry_iter = category_iter->second.find(QUALITY_PRESET_ID);
    if (entry_iter == category_iter->second.end()) {
        return;
    }
    auto const *combo = std::get_if<settings::ComboBox>(&entry_iter->second.data);
    if (combo == nullptr || combo->value == self.applied_quality_preset) {
        return;
    }
    // Only fires on the frame the tier actually changes, so hand-editing one knob afterwards
    // sticks: the tier and the last-applied tier still agree, and nothing gets written back.
    apply_quality_preset(combo->value);
}

void AppSettings::add(SettingCategoryId const &category_id, SettingId const &id, SettingEntry const &entry) {
    // TODO: make threadsafe
    auto &self = *s_instance;
    auto &category = self.categories[category_id];
    auto entry_iter = category.find(id);
    auto const first_registration = entry_iter == category.end();
    if (first_registration) {
        category.insert({id, entry});
    } else {
        entry_iter->second.factory_default = entry.factory_default;
        entry_iter->second.config = entry.config;
    }

    if (category_id != GRAPHICS_CATEGORY || g_registering_quality_preset) {
        return;
    }

    // Lazily register the tier selector the first time anything in Graphics shows up. It cannot
    // be registered eagerly: the whole Graphics category comes into existence inside
    // record_tasks(), long after AppSettings is constructed and after the settings file has been
    // read, so there is no earlier moment at which this add() would survive.
    if (!category.contains(QUALITY_PRESET_ID)) {
        g_registering_quality_preset = true;
        add<settings::ComboBox>({GRAPHICS_CATEGORY,
                                 QUALITY_PRESET_ID,
                                 {.value = shipped_quality_preset},
                                 {.task_graph_depends = true, .options = quality_preset_options()}});
        g_registering_quality_preset = false;
    }

    // A key registering for the first time has no persisted value behind it, so it takes the
    // shipped tier's value rather than the literal at its own registration site. That is what
    // makes the tier the *shipped* default instead of a thing the user has to go and select --
    // and it does it without editing the three files that own those registration sites.
    apply_shipped_default(category, id, first_registration);
}

void AppSettings::apply_shipped_default(std::map<SettingId, SettingEntry> &graphics, SettingId const &id, bool first_registration) {
    auto const &tier = quality_tier(shipped_quality_preset);
    // A key that already had a persisted value keeps it -- only the factory default moves, so
    // the meaning of "Factory Reset" tracks what this build ships. A key seen for the first time
    // has nothing to preserve and takes the tier outright.
    auto const targets = PokeTargets{
        .data = first_registration,
        .factory_default = true,
        .user_default = first_registration,
    };
    if (id == "Render Res Scale") {
        poke_float(graphics, id, tier.render_res_scale, targets);
    } else if (id == "TAA Method") {
        poke_int(graphics, id, tier.taa_method, targets);
    } else if (id == "ray_traced_reflections") {
        poke_bool(graphics, id, tier.ray_traced_reflections, targets);
    }
}

auto AppSettings::get(SettingCategoryId const &category_id, SettingId const &id) -> SettingEntry {
    // TODO: make threadsafe
    // TODO: make lookup faster
    auto &self = *s_instance;
    reconcile_quality_preset();
    auto &category = self.categories[category_id];
    auto entry_iter = category.find(id);
    if (entry_iter != category.end()) {
        return entry_iter->second;
    }
    return {};
}

void AppSettings::set(SettingCategoryId const &category_id, SettingId const &id, SettingValue const &value) {
    // TODO: make threadsafe
    auto &self = *s_instance;
    auto &category = self.categories[category_id];
    auto entry_iter = category.find(id);
    if (entry_iter != category.end()) {
        entry_iter->second.data = value;
    }
}

namespace settings {
    void to_json(nlohmann::json &j, InputFloat const &x) {
        j = nlohmann::json{{"value", x.value}};
    }
    void from_json(const nlohmann::json &j, InputFloat &x) {
        j.at("value").get_to(x.value);
    }

    void to_json(nlohmann::json &j, InputFloat3 const &x) {
        j = nlohmann::json{{"x", x.value.x}, {"y", x.value.y}, {"z", x.value.z}};
    }
    void from_json(const nlohmann::json &j, InputFloat3 &x) {
        j.at("x").get_to(x.value.x);
        j.at("y").get_to(x.value.y);
        j.at("z").get_to(x.value.z);
    }

    void to_json(nlohmann::json &j, SliderFloat const &x) {
        j = nlohmann::json{{"value", x.value}, {"min", x.min}, {"max", x.max}};
    }
    void from_json(const nlohmann::json &j, SliderFloat &x) {
        j.at("value").get_to(x.value);
        j.at("min").get_to(x.min);
        j.at("max").get_to(x.max);
    }

    void to_json(nlohmann::json &j, Checkbox const &x) {
        j = nlohmann::json{{"value", x.value}};
    }
    void from_json(const nlohmann::json &j, Checkbox &x) {
        j.at("value").get_to(x.value);
    }

    void to_json(nlohmann::json &j, ComboBox const &x) {
        j = nlohmann::json{{"value", x.value}};
    }
    void from_json(const nlohmann::json &j, ComboBox &x) {
        j.at("value").get_to(x.value);
    }
} // namespace settings

#include <typeinfo>

void to_json(nlohmann::json &j, SettingValue const &x) {
    j = nlohmann::json{};
    std::visit(
        [&](auto &&entry_data) {
            j["type"] = typeid(entry_data).name();
            j["setting"] = entry_data;
        },
        x);
}

namespace {
    template <typename... Ts>
    auto make_type_name_table(std::variant<Ts...> const &) {
        return std::map<std::string, std::variant<Ts...>>{
            {std::string{typeid(Ts).name()}, Ts{}}...};
    }
} // namespace

static const std::map<std::string, SettingValue> setting_type_name_table = make_type_name_table(SettingValue{});

void from_json(const nlohmann::json &j, SettingValue &x) {
    x = setting_type_name_table.at(j["type"]);
    std::visit([&](auto &entry_data) { j["setting"].get_to(entry_data); }, x);
}

void to_json(nlohmann::json &j, SettingEntry const &x) {
    j = nlohmann::json{};
    to_json(j["data"], x.data);
    to_json(j["user_default"], x.user_default);
}
void from_json(const nlohmann::json &j, SettingEntry &x) {
    from_json(j["data"], x.data);
    from_json(j["user_default"], x.user_default);
}

void AppSettings::save(std::filesystem::path const &filepath) {
    auto json = nlohmann::json{};

    json["_version"] = 1;

    auto &categories_json = json["categories"];
    for (auto const &[cat_id, category] : categories) {
        auto &category_json = categories_json[cat_id];
        for (auto const &[entry_key, entry] : category) {
            category_json[entry_key] = entry;
        }
    }

    json["mouse_sensitivity"] = mouse_sensitivity;
    json["world_seed_str"] = world_seed_str;

    for (auto [key_i, action_i] : keybinds) {
        auto str = fmt::format("key_{}", key_i);
        json[str] = action_i;
    }
    for (auto [mouse_button_i, action_i] : mouse_button_binds) {
        auto str = fmt::format("mouse_button_{}", mouse_button_i);
        json[str] = action_i;
    }

    auto f = std::ofstream(filepath);
    f << std::setw(4) << json;
}

void AppSettings::load(std::filesystem::path const &filepath) {
    clear();

    auto json = nlohmann::json::parse(std::ifstream(filepath));

    auto grab_value = [&json](auto str, auto &val) {
        if (json.contains(str)) {
            val = json[str];
        }
    };

    {
        auto categories_json = json["categories"];
        for (auto &[category_id, category_json] : categories_json.items()) {
            auto &category = categories[category_id];
            for (auto &[entry_id, entry_json] : category_json.items()) {
                SettingEntry entry;
                from_json(entry_json, entry);
                category.insert({entry_id, entry});
            }
        }
    }

    // Adopt the persisted tier as already-applied, so a Graphics value the user hand-edited and
    // saved survives the next launch. Without this, the reconcile in get() would see "last
    // applied = nothing" on the first read of every session and re-stamp the whole tier over it.
    //
    // A file written before presets existed has no tier key at all. It keeps whatever it saved
    // until the lazy registration in add() inserts the selector at the shipped tier, at which
    // point the reconcile does fire once and the file moves onto the shipped configuration. That
    // is deliberate -- the point of the exercise is that the measured Balanced tier is what this
    // build runs by default, including on a machine that has been running this engine for weeks.
    applied_quality_preset = QUALITY_PRESET_CUSTOM;
    if (auto category_iter = categories.find(GRAPHICS_CATEGORY); category_iter != categories.end()) {
        if (auto entry_iter = category_iter->second.find(QUALITY_PRESET_ID); entry_iter != category_iter->second.end()) {
            if (auto const *combo = std::get_if<settings::ComboBox>(&entry_iter->second.data)) {
                applied_quality_preset = combo->value;
            }
        }
    }

    grab_value("mouse_sensitivity", mouse_sensitivity);
    grab_value("world_seed_str", world_seed_str);

    auto load_brush_settings = [&grab_value](std::string const &brush_name, BrushSettings &brush_settings) {
        grab_value(brush_name + ".flags", brush_settings.flags);
        grab_value(brush_name + ".radius", brush_settings.radius);
    };

    for (daxa_i32 key_i = 0; key_i < GLFW_KEY_LAST + 1; ++key_i) {
        auto str = fmt::format("key_{}", key_i);
        if (json.contains(str)) {
            keybinds[key_i] = json[str];
        }
    }
    for (daxa_i32 mouse_button_i = 0; mouse_button_i < GLFW_MOUSE_BUTTON_LAST + 1; ++mouse_button_i) {
        auto str = fmt::format("mouse_button_{}", mouse_button_i);
        if (json.contains(str)) {
            mouse_button_binds[mouse_button_i] = json[str];
        }
    }
}

void AppSettings::clear() {
    mouse_sensitivity = 1.0f;
    world_seed_str = "gvox";

    keybinds.clear();
    mouse_button_binds.clear();
}

void AppSettings::reset_default() {
    clear();

    // clang-format off
    keybinds[GLFW_KEY_W]             = GAME_ACTION_MOVE_FORWARD;
    keybinds[GLFW_KEY_A]             = GAME_ACTION_MOVE_LEFT;
    keybinds[GLFW_KEY_S]             = GAME_ACTION_MOVE_BACKWARD;
    keybinds[GLFW_KEY_D]             = GAME_ACTION_MOVE_RIGHT;
    keybinds[GLFW_KEY_R]             = GAME_ACTION_RELOAD;
    keybinds[GLFW_KEY_F]             = GAME_ACTION_TOGGLE_FLY;
    keybinds[GLFW_KEY_E]             = GAME_ACTION_INTERACT0;
    keybinds[GLFW_KEY_Q]             = GAME_ACTION_INTERACT1;
    keybinds[GLFW_KEY_SPACE]         = GAME_ACTION_JUMP;
    keybinds[GLFW_KEY_LEFT_CONTROL]  = GAME_ACTION_CROUCH;
    keybinds[GLFW_KEY_LEFT_SHIFT]    = GAME_ACTION_SPRINT;
    keybinds[GLFW_KEY_LEFT_ALT]      = GAME_ACTION_WALK;
    keybinds[GLFW_KEY_F5]            = GAME_ACTION_CYCLE_VIEW;
    keybinds[GLFW_KEY_B]             = GAME_ACTION_TOGGLE_BRUSH;

    mouse_button_binds[GLFW_MOUSE_BUTTON_1] = GAME_ACTION_BRUSH_A;
    mouse_button_binds[GLFW_MOUSE_BUTTON_2] = GAME_ACTION_BRUSH_B;
    // clang-format on
}
