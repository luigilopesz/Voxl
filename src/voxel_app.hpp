#pragma once

#include <application/window.hpp>
#include <application/ui.hpp>
#include <application/ui_tools.hpp>
#include <application/audio.hpp>
#include <application/player.hpp>

#include <renderer/renderer.hpp>
#include <voxels/voxel_world.inl>
#include <voxels/model.hpp>
#include <daxa/utils/imgui.hpp>

#include <utilities/gpu_context.hpp>

#include <chrono>
#include <fstream>
#include <future>

struct VoxelApp : AppWindow<VoxelApp> {
    using Clock = std::chrono::high_resolution_clock;
    Clock::time_point start = Clock::now();
    Clock::time_point prev_time;
    Clock::time_point prev_phys_update_time = Clock::now();

    GpuContext gpu_context;

    AppUi ui;
    // The editing tool belt: selection, brush radius and the in-play HUD. Declared after `ui`
    // because install_hud() needs AppUi's ImGui context and fonts to already exist.
    ToolBelt tools;
    AppAudio audio;
    daxa::ImGuiRenderer imgui_renderer;
    Renderer renderer;

    VoxelWorld voxel_world;
    VoxelParticles particles;
    VoxelModelLoader voxel_model_loader;

    PlayerInput player_input{};
    GpuInput gpu_input{};
    GpuOutput gpu_output{};
    std::vector<std::string> ui_strings;

    bool needs_vram_calc = true;

    daxa_f32 render_res_scl{1.0f};

    // --- command-line driven capture and benchmarking (see application/cli.hpp) ----------------
    // Open only when --bench-csv was passed; one row per frame is appended from on_update().
    std::ofstream bench_csv_file;
    // Host-visible staging for --screenshot, sized from the SWAPCHAIN's extent rather than from
    // window_size. Those two are not the same number: window_size is what was asked for and is
    // only corrected by on_resize(), which early-outs when the requested and current sizes
    // compare equal, so at startup it can differ from the surface the driver actually gave us.
    // Sizing the buffer from one and copying with the other put a 1920x1080 run past the end of
    // an undersized buffer and killed the process at the exact frame the capture was due.
    daxa::BufferId screenshot_buffer;
    daxa_u32vec2 screenshot_extent{};
    // Set the frame the readback should be recorded on, cleared once the PNG has been written.
    bool screenshot_capture_this_frame = false;
    bool screenshot_written = false;
    // Recorded once at record_tasks() time. The readback task adds a TRANSFER_READ attachment on
    // the swapchain image, which forces an extra layout transition every single frame -- so it is
    // only recorded into the graph when a screenshot was actually asked for, keeping benchmark
    // runs bit-identical to runs without the feature.
    bool screenshot_enabled = false;

    void write_screenshot();
    void write_bench_row(daxa_f32 cpu_delta_time);

    VoxelApp();
    VoxelApp(VoxelApp const &) = delete;
    VoxelApp(VoxelApp &&) = delete;
    auto operator=(VoxelApp const &) -> VoxelApp & = delete;
    auto operator=(VoxelApp &&) -> VoxelApp & = delete;
    ~VoxelApp();

    void run();

    void on_update();
    void on_mouse_move(daxa_f32 x, daxa_f32 y);
    void on_mouse_scroll(daxa_f32 dx, daxa_f32 dy);
    void on_mouse_button(daxa_i32 button_id, daxa_i32 action);
    void on_key(daxa_i32 key_id, daxa_i32 action);
    void on_resize(daxa_u32 sx, daxa_u32 sy);
    void on_drop(std::span<char const *> filepaths);

    void run_startup();
    void record_tasks();

    void calc_vram_usage();
};
