#pragma once

struct GLFWwindow;
struct ImFont;

#include "settings.hpp"
#include <imgui.h>
#include <chrono>
#include <filesystem>
#include <thread>
#include <mutex>
#include <string>
#include <fmt/format.h>

#define INVALID_GAME_ACTION (-1)

struct AppUi {
    using Clock = std::chrono::high_resolution_clock;

    AppUi(GLFWwindow *glfw_window_ptr);
    ~AppUi();

    AppSettings settings;

    GLFWwindow *glfw_window_ptr;
    ImFont *mono_font = nullptr;
    ImFont *menu_font = nullptr;

    std::array<float, 200> full_frametimes = {};
    std::array<float, 200> cpu_frametimes = {};
    daxa_u64 frametime_rotation_index = 0;

    daxa_f32 debug_menu_size{};

    bool needs_saving = false;
    Clock::time_point last_save_time{};

    daxa_u32 conflict_resolution_mode = 0;
    daxa_i32 new_key_id{};
    daxa_i32 limbo_action_index = INVALID_GAME_ACTION;
    daxa_i32 limbo_key_index = GLFW_KEY_LAST + 1;
    daxa_f32 ui_scale = 1.0f;
    bool limbo_is_button = false;

    bool paused = true;
    bool show_settings = false;
    bool show_imgui_demo_window = false;
    bool should_run_startup = true;
    bool should_recreate_voxel_buffers = true;
    bool autosave_override = false;
    bool should_upload_seed_data = true;
    bool should_hotload_shaders = false;
    bool should_regenerate_sky = true;

    bool should_record_task_graph = false;

    bool should_upload_gvox_model = false;
    std::filesystem::path gvox_model_path;
    std::filesystem::path data_directory;

    void rescale_ui();
    void update(daxa_f32 delta_time, daxa_f32 cpu_delta_time);

    void toggle_pause();
    void toggle_debug();
    void toggle_console();

    /// F2. One line naming every effective entry in a settings category, e.g.
    /// `Render Res Scale=0.6667 | TAA Method=Kajiya TAA | global_illumination=on`.
    ///
    /// Static because it reads the process-wide settings registry rather than any AppUi state,
    /// which also lets code outside the UI (the bench-CSV writer) ask for it. Only meaningful
    /// once every AppSettings::add() has run -- that is, from the first frame onwards.
    static auto settings_summary(SettingCategoryId const &category_id) -> std::string;

    /// True once the Graphics settings have differed from their first-frame values at any point
    /// during the run. The whole reason F2 exists: a run whose configuration moved underneath it
    /// is not a measurement, and this makes that visible rather than silent.
    [[nodiscard]] auto settings_changed_mid_run() const -> bool { return settings_moved; }

  private:
    void settings_ui();
    void settings_controls_ui();
    void settings_passes_ui();

    /// F3. Applies AppCli::setting_overrides. Called once, on the first update() -- see the
    /// comment at the definition for why it cannot happen any earlier.
    void apply_cli_setting_overrides();
    /// F2. Records the first-frame Graphics summary, writes the bench-CSV sidecar, and watches
    /// for the settings moving afterwards.
    void track_settings_provenance();

    bool cli_overrides_applied = false;
    bool settings_baseline_taken = false;
    std::string startup_graphics_summary;
    bool settings_moved = false;
};
