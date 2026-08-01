#include "voxel_app.hpp"

#include <fmt/format.h>
#include <FreeImage.h>

#include <thread>
#include <numbers>
#include <fstream>
#include <unordered_map>

#include <application/cli.hpp>
#include <utilities/gpu_profiler.hpp>

// #include <voxels/gvox_model.inl>

static_assert(IsVoxelWorld<VoxelWorld>);

#define APPNAME "Voxel App"

using namespace std::chrono_literals;

#include <iostream>

// F4 -- round the render-target extent up to a multiple of 8. See PERFORMANCE_PLAN.md section 6.1.
//
// This was a no-op: the body was commented out under the comment "not necessary, since it rounds
// up!". That claim is false. The caller computes the extent as
// `static_cast<daxa_u32>(window_size.x * render_res_scl)`, which TRUNCATES -- at 1280 x 720 and
// Render Res Scale 0.6667 it yields 853 x 480, and 853 is odd.
//
// Two things then go wrong, and both are silent:
//   * Every compute shader in this renderer but seven is 8x8x1, so an extent that is not a
//     multiple of 8 leaves a partial workgroup on the right and bottom edges.
//   * Several passes derive half-resolution extents as (x + 1) / 2 -- the depth prepass, SSAO and
//     the ReSTIR diffuse candidate trace. At 853 that is 427, and 427 * 2 = 854 != 853, so the
//     half-res buffer and the full-res buffer it is meant to pair with disagree by a pixel.
// `Render Res Scale = 0.6667` intermittently failed to apply in 4 of 11 runs and never at
// 0.40/0.50/0.70/0.75/1.25/1.50/2.00 -- the only tested scale producing an odd extent.
//
// Rounding UP is safe and is what the renderer already expects: the render images are allocated
// at `rounded_frame_dim` while the shaders address the `frame_dim` sub-rectangle, and
// postprocessing.raster.glsl:104 rescales the g-buffer UVs by `frame_dim / rounded_frame_dim`
// precisely to absorb the difference. Rounding down would instead sample outside the rendered
// region. 8 rather than upstream's 32 is the smallest value that satisfies both constraints
// above, and costs at most 7 columns and 7 rows of extra pixels (~1.6% at 0.6667) instead of 31.
//
// The arithmetic is written out per-component on purpose. `daxa_u32vec2` is a plain C struct
// (daxa/c/core.h, _DAXA_DECL_VEC2_TYPE) with no operator overloads, so the vector-valued
// expression left commented out upstream would not have compiled had anyone uncommented it --
// which is the likeliest reason it was commented out and then rationalised in the comment.
constexpr auto round_frame_dim(daxa_u32vec2 size) -> daxa_u32vec2 {
    constexpr daxa_u32 GRANULARITY = 8;
    auto round_up = [](daxa_u32 v) -> daxa_u32 {
        // Never round to zero: a minimised window gives a 0 extent, and a 0-sized image or an
        // empty dispatch is a validation error rather than a cheap frame.
        auto rounded = (v + (GRANULARITY - 1u)) / GRANULARITY * GRANULARITY;
        return rounded < GRANULARITY ? GRANULARITY : rounded;
    };
    return daxa_u32vec2{round_up(size.x), round_up(size.y)};
}

VoxelApp::VoxelApp() : AppWindow(APPNAME, {AppCli::get().width, AppCli::get().height}), ui{AppUi(AppWindow::glfw_window_ptr)} {
    gpu_context.create_swapchain({
        .native_window = AppWindow::get_native_handle(),
        .native_window_platform = AppWindow::get_native_platform(),
        .surface_format_selector = [](daxa::Format format) -> daxa_i32 {
            switch (format) {
            case daxa::Format::B8G8R8A8_SRGB: return 90;
            case daxa::Format::R8G8B8A8_SRGB: return 80;
            default: return 0;
            }
        },
        .present_mode = daxa::PresentMode::IMMEDIATE,
        .image_usage = daxa::ImageUsageFlagBits::TRANSFER_DST,
        .max_allowed_frames_in_flight = FRAMES_IN_FLIGHT,
        .name = "swapchain",
    });

    AppSettings::add<settings::SliderFloat>({"Camera", "FOV", {.value = 74.0f, .min = 0.0f, .max = 179.0f}});

    AppSettings::add<settings::InputFloat>({"UI", "Scale", {.value = 1.0f}});
    AppSettings::add<settings::Checkbox>({"UI", "show_debug_info", {.value = false}});
    AppSettings::add<settings::Checkbox>({"UI", "show_console", {.value = false}});
    AppSettings::add<settings::Checkbox>({"UI", "autosave", {.value = true}});
    AppSettings::add<settings::Checkbox>({"General", "battery_saving_mode", {.value = false}});

    AppSettings::add<settings::SliderFloat>({"Graphics", "Render Res Scale", {.value = 1.0f, .min = 0.2f, .max = 4.0f}, {.task_graph_depends = true}});

    auto const &device_props = gpu_context.device.properties();
    debug_utils::DebugDisplay::set_debug_string("GPU", reinterpret_cast<char const *>(device_props.device_name));
    imgui_renderer = daxa::ImGuiRenderer({
        .device = gpu_context.device,
        .format = gpu_context.swapchain.get_format(),
        .context = ImGui::GetCurrentContext(),
        .use_custom_config = false,
    });

    voxel_model_loader.create(gpu_context);

    record_tasks();
    gpu_context.pipeline_manager->wait();
    debug_utils::Console::add_log(fmt::format("startup: {} s\n", std::chrono::duration<float>(Clock::now() - start).count()));

    // --- command-line startup state -----------------------------------------------------------
    auto const &cli = AppCli::get();
    if (cli.overlay >= 0) {
        // Forced rather than toggled. show_debug_info is persisted to the settings file, so the
        // state at launch is whatever the *previous* run left behind, and a harness that sends F3
        // turns the overlay off exactly as often as it turns it on.
        AppSettings::set("UI", "show_debug_info", settings::Checkbox{.value = cli.overlay == 1});
    }
    if (cli.unpause) {
        ui.paused = false;
        set_mouse_capture(true);
    }
    // --- editing tools (application/ui_tools.hpp) ----------------------------------------------
    // Registers the tool HUD as an ImGui context hook, and the "UI"/"show_tool_hud" setting that
    // turns it off for capture work. Has to run after AppUi's constructor, which is what creates
    // the ImGui context and loads the two fonts.
    tools.install_hud(ui.menu_font, ui.mono_font);
    if (!cli.bench_csv.empty()) {
        bench_csv_file.open(cli.bench_csv, std::ios::out | std::ios::trunc);
        if (bench_csv_file.is_open()) {
            bench_csv_file << "frame,t_s,full_ms,cpu_ms,heap_pages,heap_capacity_mb,heap_used_mb,"
                              "heap_cap_pages,px,py,pz,yaw,pitch\n";
        } else {
            debug_utils::Console::add_log(fmt::format("[cli] could not open --bench-csv '{}'\n", cli.bench_csv));
        }
    }
    // start is re-taken here so --exit-after and --screenshot-after are measured from the first
    // rendered frame, not from process launch. Shader compilation is 20-40 s on a cold SPIR-V
    // cache and 3-5 s warm; timing a 30 s run from before it would give two different runs.
    start = Clock::now();
}
VoxelApp::~VoxelApp() {
    gpu_context.device.wait_idle();
    // After wait_idle so every outstanding query is resolved, and before the device goes away.
    gpu_profiler::shutdown();
    gpu_context.device.collect_garbage();

    if (!screenshot_buffer.is_empty()) {
        gpu_context.device.destroy_buffer(screenshot_buffer);
    }
    if (bench_csv_file.is_open()) {
        bench_csv_file.flush();
        bench_csv_file.close();
    }

    voxel_model_loader.destroy();
}

void VoxelApp::run() {
    auto const &cli = AppCli::get();
    while (true) {
        glfwPollEvents();
        if (glfwWindowShouldClose(AppWindow::glfw_window_ptr) != 0) {
            break;
        }
        // Checked before rendering so the exit is at a frame boundary with no work in flight.
        // Leaving the loop normally means ~VoxelApp() runs, the device is waited on, and the
        // process returns 0 -- which is what makes an automated run distinguishable from a crash.
        if (cli.exit_after > 0.0f &&
            std::chrono::duration<float>(Clock::now() - start).count() >= cli.exit_after) {
            break;
        }

        if (!AppWindow::minimized) {
            on_resize(window_size.x, window_size.y);

            if (AppSettings::get<settings::Checkbox>("General", "battery_saving_mode").value) {
                std::this_thread::sleep_for(10ms);
            }

            on_update();
        } else {
            std::this_thread::sleep_for(1ms);
        }
    }
}

void VoxelApp::on_update() {
    auto now = Clock::now();

    gpu_context.swapchain_image = gpu_context.swapchain.acquire_next_image();

    auto t0 = Clock::now();
    gpu_input.time = std::chrono::duration<daxa_f32>(now - start).count();
    gpu_input.delta_time = std::chrono::duration<daxa_f32>(now - prev_time).count();
    prev_time = now;
    gpu_input.render_res_scl = render_res_scl;

    audio.set_frequency(gpu_input.delta_time * 1000.0f * 200.0f);

    if (ui.should_hotload_shaders) {
        auto reload_result = gpu_context.pipeline_manager->reload_all();
        if (auto *reload_err = daxa::get_if<daxa::PipelineReloadError>(&reload_result)) {
            debug_utils::Console::add_log(reload_err->message);
        }
    }

    gpu_context.task_swapchain_image.set_images({.images = {&gpu_context.swapchain_image, 1}});
    if (gpu_context.swapchain_image.is_empty()) {
        return;
    }

    if (ui.should_upload_seed_data) {
        // The UI's world-seed field at ui.cpp:346 has never been wired up; the std::hash call it
        // would feed is still commented out beside this literal upstream. --seed makes the field
        // reachable from a script without touching that UI plumbing. Note that this seeds only
        // g_value_noise_tex: the Voxl test scene in voxels/brushes.glsl hashes absolute world
        // coordinates directly and is deliberately independent of it (docs/SCENE.md sec 5).
        auto seed = AppCli::get().seed.value_or(15512089755474631791ull);
        gpu_context.update_seeded_value_noise(seed);
        ui.should_upload_seed_data = false;
    }

    if (ui.should_run_startup || voxel_model_loader.model_is_ready) {
        run_startup();
        ui.should_run_startup = false;
    }

    voxel_model_loader.update(ui);

    if (ui.should_record_task_graph) {
        gpu_context.device.wait_idle();
        record_tasks();
    }

    gpu_input.flags &= ~GAME_FLAG_BITS_PAUSED;
    gpu_input.flags |= GAME_FLAG_BITS_PAUSED * static_cast<daxa_u32>(ui.paused);

    // --- scripted editing (--edit) -------------------------------------------------------------
    // Drives the tool belt and the brush buttons from the command line. BEFORE tools.perframe(),
    // because that is what publishes the belt into gpu_input.brush_selection -- selecting here
    // means the HUD in the resulting screenshot names the tool that actually ran, which is the
    // difference between a capture that proves something and one that merely looks right.
    //
    // WHY THE ACTION IS HELD FOR A WINDOW OF TIME rather than set for a single frame. The edit is
    // consumed by two compute passes a frame apart (chunk election, then the edit itself), and
    // the one-shot gate in voxel_world.comp.glsl allows BRUSH_ONE_SHOT_FRAMES of slack for
    // exactly that reason. A one-frame press is also unrepresentative: no human clicks for 11 ms.
    if (!AppCli::get().edits.empty()) {
        auto const elapsed = std::chrono::duration<float>(now - start).count();
        bool primary = false;
        bool secondary = false;
        for (auto const &edit : AppCli::get().edits) {
            if (elapsed < edit.at_s || elapsed >= edit.at_s + edit.hold_s) {
                continue;
            }
            // Last writer wins if two scripted strokes overlap. Overlapping strokes are a script
            // bug rather than a feature, but silently applying one of them beats applying a
            // half-blended pair of radii that never appeared in any argument.
            tools.select(edit.brush_id);
            tools.set_radius(edit.radius);
            (edit.secondary ? secondary : primary) = true;
        }
        // Written every frame, not only while a stroke is active: the buttons must go back DOWN
        // between strokes or brush_state.is_editing never clears, and the one-shot gate --
        // which fires on the is_editing 0->1 edge -- would arm once and never again.
        gpu_input.actions[GAME_ACTION_BRUSH_A] = primary ? 1U : 0U;
        gpu_input.actions[GAME_ACTION_BRUSH_B] = secondary ? 1U : 0U;
    }

    // --- editing tools (application/ui_tools.hpp) ----------------------------------------------
    // Publishes the selected brush and radius into gpu_input.brush_selection, and decides whether
    // the HUD is up this frame. Before frame_task_graph.execute() below, which is what uploads
    // gpu_input, and before ui.update(), whose NewFrame() is what calls the HUD back.
    tools.perframe(!ui.paused, gpu_input);

    gpu_input.flags &= ~GAME_FLAG_BITS_NEEDS_PHYS_UPDATE;

    // Before renderer.begin_frame, because that executes and submits the sky task graph and the
    // profiler's ring slot has to be claimed before the first submit of the frame.
    gpu_profiler::begin_frame(gpu_context.device, gpu_input.frame_index, gpu_input.time, gpu_input.delta_time * 1000.0f);

    renderer.begin_frame(gpu_input);

    if (now - prev_phys_update_time > std::chrono::duration<float>(GAME_PHYS_UPDATE_DT)) {
        gpu_input.flags |= GAME_FLAG_BITS_NEEDS_PHYS_UPDATE;
        prev_phys_update_time = now;
    }

    if (needs_vram_calc) {
        calc_vram_usage();
    }

    voxel_world.begin_frame(gpu_context.device, gpu_input, gpu_output.voxel_world);

    player_input.frame_dim = gpu_input.frame_dim;
    player_input.halton_jitter = gpu_input.halton_jitter;
    player_input.delta_time = gpu_input.delta_time;
    player_input.sensitivity = ui.settings.mouse_sensitivity;
    player_input.fov = AppSettings::get<settings::SliderFloat>("Camera", "FOV").value * (std::numbers::pi_v<daxa_f32> / 180.0f);
    player_input.mouse = gpu_input.mouse;
    std::copy(std::begin(gpu_input.actions), std::end(gpu_input.actions), std::begin(player_input.actions));
    player_perframe(player_input, gpu_input.player, voxel_world);

    gpu_input.fif_index = gpu_input.frame_index % (FRAMES_IN_FLIGHT + 1);

    // Decided before execute() because the readback task inside the graph reads this flag.
    if (screenshot_enabled && !screenshot_written) {
        auto const elapsed = std::chrono::duration<float>(now - start).count();
        screenshot_capture_this_frame = elapsed >= AppCli::get().screenshot_after;
    }

    gpu_context.frame_task_graph.execute({});

    // Reads back the pool written three frames ago; never the one just submitted.
    gpu_profiler::end_frame();

    if (screenshot_capture_this_frame) {
        write_screenshot();
    }

    gpu_input.resize_factor = 1.0f;

    gpu_input.mouse.pos_delta = {0.0f, 0.0f};
    gpu_input.mouse.scroll_delta = {0.0f, 0.0f};

    renderer.end_frame(gpu_context.device, gpu_input.delta_time);

    auto t1 = Clock::now();
    ui.update(gpu_input.delta_time, std::chrono::duration<daxa_f32>(t1 - t0).count());

    write_bench_row(std::chrono::duration<daxa_f32>(t1 - t0).count());

    ++gpu_input.frame_index;
    gpu_context.device.collect_garbage();
}

// --- --bench-csv ------------------------------------------------------------------------------
// One row per frame. Written after ui.update() so full_ms/cpu_ms are exactly the two numbers the
// debug overlay's graphs plot -- the point of this file is that the overlay stops being the only
// place the engine's own timing exists, not that it grows a second, differently-measured one.
void VoxelApp::write_bench_row(daxa_f32 cpu_delta_time) {
    if (!bench_csv_file.is_open()) {
        return;
    }
    auto const &heap = voxel_world.buffers.voxel_malloc;
    auto const page_bytes = static_cast<double>(VOXEL_MALLOC_PAGE_SIZE_BYTES);
    auto const used_pages = static_cast<double>(gpu_output.voxel_world.voxel_malloc_output.current_element_count);
    bench_csv_file << gpu_input.frame_index << ','
                   << fmt::format("{:.4f}", std::chrono::duration<float>(Clock::now() - start).count()) << ','
                   << fmt::format("{:.4f}", gpu_input.delta_time * 1000.0f) << ','
                   << fmt::format("{:.4f}", cpu_delta_time * 1000.0f) << ','
                   << heap.current_element_count << ','
                   << fmt::format("{:.2f}", static_cast<double>(heap.current_element_count) * page_bytes / 1'000'000.0) << ','
                   << fmt::format("{:.2f}", used_pages * page_bytes / 1'000'000.0) << ','
                   << heap.max_element_count << ','
                   << fmt::format("{:.3f}", gpu_input.player.pos.x + static_cast<float>(gpu_input.player.player_unit_offset.x)) << ','
                   << fmt::format("{:.3f}", gpu_input.player.pos.y + static_cast<float>(gpu_input.player.player_unit_offset.y)) << ','
                   << fmt::format("{:.3f}", gpu_input.player.pos.z + static_cast<float>(gpu_input.player.player_unit_offset.z)) << ','
                   << fmt::format("{:.4f}", gpu_input.player.yaw) << ','
                   << fmt::format("{:.4f}", gpu_input.player.pitch) << '\n';
}

// --- --screenshot -----------------------------------------------------------------------------
// Called immediately after frame_task_graph.execute(), with the readback copy already recorded
// into that graph by the task in record_tasks(). wait_idle() is heavy-handed and would be wrong
// in a hot loop, but this runs exactly once per process: correctness beats elegance for a
// one-shot capture, and a fence dance here would be code nobody ever exercises twice.
void VoxelApp::write_screenshot() {
    screenshot_capture_this_frame = false;
    screenshot_written = true;
    gpu_context.device.wait_idle();

    auto const width = screenshot_extent.x;
    auto const height = screenshot_extent.y;
    auto const *src = gpu_context.device.get_host_address_as<uint8_t>(screenshot_buffer).value();
    if (src == nullptr) {
        debug_utils::Console::add_log("[cli] screenshot: staging buffer is not host-visible\n");
        return;
    }
    // Belt and braces after the 1080p crash: refuse rather than read past the end. A capture
    // that does not happen is a bug report; a capture that walks off a buffer is a mystery.
    auto const needed = static_cast<daxa_u64>(width) * height * 4;
    auto const have = gpu_context.device.info_buffer(screenshot_buffer).value().size;
    if (have < needed) {
        debug_utils::Console::add_log(fmt::format(
            "[cli] screenshot: staging buffer is {} B, need {} B for {}x{} -- skipped\n",
            have, needed, width, height));
        return;
    }

    // The swapchain format selector in the constructor prefers B8G8R8A8_SRGB and falls back to
    // R8G8B8A8_SRGB, so the channel order has to be read rather than assumed -- getting it wrong
    // produces a plausible-looking image with red and blue swapped, which is exactly the kind of
    // defect that survives review.
    auto const format = gpu_context.swapchain.get_format();
    bool const is_bgra = (format == daxa::Format::B8G8R8A8_SRGB) || (format == daxa::Format::B8G8R8A8_UNORM);

    FIBITMAP *bitmap = FreeImage_Allocate(static_cast<int>(width), static_cast<int>(height), 24);
    if (bitmap == nullptr) {
        debug_utils::Console::add_log("[cli] screenshot: FreeImage_Allocate failed\n");
        return;
    }
    for (uint32_t y = 0; y < height; ++y) {
        // FreeImage scanline 0 is the BOTTOM of the image; the swapchain's row 0 is the top.
        auto *dst = FreeImage_GetScanLine(bitmap, static_cast<int>(height - 1 - y));
        auto const *row = src + static_cast<size_t>(y) * width * 4;
        for (uint32_t x = 0; x < width; ++x) {
            auto const *px = row + static_cast<size_t>(x) * 4;
            // FreeImage's 24-bit rows are BGR on little-endian regardless of platform.
            dst[x * 3 + 0] = is_bgra ? px[0] : px[2];
            dst[x * 3 + 1] = px[1];
            dst[x * 3 + 2] = is_bgra ? px[2] : px[0];
        }
    }
    auto const &path = AppCli::get().screenshot_path;
    bool const ok = FreeImage_Save(FIF_PNG, bitmap, path.c_str(), PNG_DEFAULT) != 0;
    FreeImage_Unload(bitmap);
    debug_utils::Console::add_log(fmt::format("[cli] screenshot {}: {}\n", ok ? "written" : "FAILED", path));
    // Printed to stdout as well as the in-app console: the console is only visible with the
    // backtick key held open, and a harness needs to know the file exists before it reads it.
    std::cout << "[cli] screenshot " << (ok ? "written" : "FAILED") << ": " << path << std::endl;
}
void VoxelApp::on_mouse_move(daxa_f32 x, daxa_f32 y) {
    daxa_f32vec2 const center = {static_cast<daxa_f32>(window_size.x / 2), static_cast<daxa_f32>(window_size.y / 2)};
    gpu_input.mouse.pos = daxa_f32vec2{x, y};
    auto offset = daxa_f32vec2{gpu_input.mouse.pos.x - center.x, gpu_input.mouse.pos.y - center.y};
    gpu_input.mouse.pos = daxa_f32vec2{
        gpu_input.mouse.pos.x * static_cast<daxa_f32>(gpu_input.frame_dim.x) / static_cast<daxa_f32>(window_size.x),
        gpu_input.mouse.pos.y * static_cast<daxa_f32>(gpu_input.frame_dim.y) / static_cast<daxa_f32>(window_size.y),
    };
    if (!ui.paused) {
        gpu_input.mouse.pos_delta = daxa_f32vec2{gpu_input.mouse.pos_delta.x + offset.x, gpu_input.mouse.pos_delta.y + offset.y};
        set_mouse_pos(center.x, center.y);
    }
}
void VoxelApp::on_mouse_scroll(daxa_f32 dx, daxa_f32 dy) {
    auto &io = ImGui::GetIO();
    if (io.WantCaptureMouse) {
        return;
    }

    gpu_input.mouse.scroll_delta = daxa_f32vec2{gpu_input.mouse.scroll_delta.x + dx, gpu_input.mouse.scroll_delta.y + dy};

    // --- editing tools (application/ui_tools.hpp) ----------------------------------------------
    // The wheel changes tool; Alt and the wheel changes brush size. GLFW's scroll callback carries
    // no modifier mask, hence the direct key query -- and Alt rather than Ctrl or Shift because
    // those two are bound to crouch and sprint, and a brush-size gesture that also ducks the
    // player moves the camera while you are trying to aim the brush.
    tools.on_scroll(dy, glfwGetKey(glfw_window_ptr, GLFW_KEY_LEFT_ALT) == GLFW_PRESS ||
                            glfwGetKey(glfw_window_ptr, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS);
}
void VoxelApp::on_mouse_button(daxa_i32 button_id, daxa_i32 action) {
    auto &io = ImGui::GetIO();
    if (io.WantCaptureMouse) {
        return;
    }
    if (ui.limbo_action_index != INVALID_GAME_ACTION) {
        return;
    }

    if (ui.settings.mouse_button_binds.contains(button_id)) {
        gpu_input.actions[ui.settings.mouse_button_binds.at(button_id)] = static_cast<daxa_u32>(action);
    }
}
void VoxelApp::on_key(daxa_i32 key_id, daxa_i32 action) {
    auto &io = ImGui::GetIO();
    if (io.WantCaptureKeyboard) {
        return;
    }
    if (ui.limbo_action_index != INVALID_GAME_ACTION) {
        return;
    }

    if (key_id == GLFW_KEY_ESCAPE && action == GLFW_PRESS) {
        std::fill(std::begin(gpu_input.actions), std::end(gpu_input.actions), 0);
        ui.toggle_pause();
        set_mouse_capture(!ui.paused);
    }

    if (key_id == GLFW_KEY_F3 && action == GLFW_PRESS) {
        ui.toggle_debug();
    }

    if (ui.paused) {
        if (key_id == GLFW_KEY_GRAVE_ACCENT && action == GLFW_PRESS) {
            ui.toggle_console();
        }
    }

    if (key_id == GLFW_KEY_R && action == GLFW_PRESS) {
        ui.should_run_startup = true;
        start = Clock::now();
    }

    // --- editing tools (application/ui_tools.hpp) ----------------------------------------------
    // The number row selects a tool and [ ] size it. Consulted before the keybind table below, but
    // it claims no key that table uses: the digits and brackets are unbound in
    // AppSettings::reset_default(), so nothing here overrides a movement or action binding.
    if (tools.on_key(key_id, action, ui.paused)) {
        return;
    }

    if (!ui.paused) {
        if (ui.settings.keybinds.contains(key_id)) {
            gpu_input.actions[ui.settings.keybinds.at(key_id)] = static_cast<daxa_u32>(action);
        }
    }
}
void VoxelApp::on_resize(daxa_u32 sx, daxa_u32 sy) {
    minimized = (sx == 0 || sy == 0);
    auto new_render_res_scl = AppSettings::get<settings::SliderFloat>("Graphics", "Render Res Scale").value;
    auto resized = sx != window_size.x || sy != window_size.y || render_res_scl != new_render_res_scl;
    if (!minimized && resized) {
        gpu_context.swapchain.resize();
        window_size.x = gpu_context.swapchain.get_surface_extent().x;
        window_size.y = gpu_context.swapchain.get_surface_extent().y;
        render_res_scl = new_render_res_scl;
        {
            // resize render images
            // gpu_context.render_images.size.x = static_cast<daxa_u32>(static_cast<daxa_f32>(window_size.x) * render_res_scl);
            // gpu_context.render_images.size.y = static_cast<daxa_u32>(static_cast<daxa_f32>(window_size.y) * render_res_scl);
            gpu_context.device.wait_idle();
            needs_vram_calc = true;
        }
        record_tasks();
        gpu_input.resize_factor = 0.0f;
        on_update();
    }
}
void VoxelApp::on_drop(std::span<char const *> filepaths) {
    ui.gvox_model_path = filepaths[0];
    ui.should_upload_gvox_model = true;
}

void VoxelApp::run_startup() {
    player_startup(gpu_input.player);
    gpu_context.startup_task_graph.execute({});

    ui.should_run_startup = false;
}

void VoxelApp::record_tasks() {
    ui.should_record_task_graph = false;

    gpu_input.frame_dim.x = static_cast<daxa_u32>(static_cast<daxa_f32>(window_size.x) * render_res_scl);
    gpu_input.frame_dim.y = static_cast<daxa_u32>(static_cast<daxa_f32>(window_size.y) * render_res_scl);
    gpu_input.rounded_frame_dim = round_frame_dim(gpu_input.frame_dim);
    gpu_input.output_resolution = window_size;

    // AIM THE PICKING RAY AT THE CROSSHAIR, not at the corner of the frame.
    //
    // gpu_input.mouse.pos is expressed in frame_dim (render-resolution) space and was written in
    // exactly one place: VoxelApp::on_mouse_move. Until the player physically moved the mouse it
    // therefore held (0, 0) -- the TOP-LEFT PIXEL -- and perframe.comp.glsl builds the brush's
    // picking ray from it. Every edit made before the first mouse motion landed wherever the
    // frame's corner pointed, which is usually sky, and the clamp in brushes.inl then put it at
    // BRUSH_PICK_MAX_M along that corner ray. It is not a harness-only problem: it is reachable
    // from a cold start by clicking before moving the mouse, and it is what made a scripted
    // capture of an add brush bury the camera in stone.
    //
    // The crosshair is the centre, and while the game is unpaused the cursor is captured and
    // warped back to the centre after every motion event, so the centre is also the value the
    // very next real mouse event will produce. Doing it here rather than in the constructor
    // covers the resize and render-scale-change cases too, since frame_dim changes under it.
    gpu_input.mouse.pos = daxa_f32vec2{
        static_cast<daxa_f32>(gpu_input.frame_dim.x) * 0.5f,
        static_cast<daxa_f32>(gpu_input.frame_dim.y) * 0.5f,
    };

    gpu_context.frame_task_graph = daxa::TaskGraph({
        .device = gpu_context.device,
        .swapchain = gpu_context.swapchain,
        .alias_transients = GVOX_ENGINE_INSTALL,
        .name = "frame_task_graph",
    });
    gpu_context.startup_task_graph = daxa::TaskGraph({
        .device = gpu_context.device,
        .alias_transients = GVOX_ENGINE_INSTALL,
        .name = "startup_task-graph",
    });
    gpu_context.use_resources();
    gpu_context.render_resolution = gpu_input.rounded_frame_dim;
    gpu_context.output_resolution = gpu_input.output_resolution;

    voxel_world.record_startup(gpu_context);
    particles.record_startup(gpu_context);

    gpu_context.frame_task_graph.use_persistent_buffer(voxel_model_loader.task_gvox_model_buffer);

    gpu_context.frame_task_graph.add_task({
        .attachments = {
            daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_WRITE, gpu_context.task_input_buffer),
        },
        .task = [this](daxa::TaskInterface const &ti) {
            auto prof = gpu_profiler::Scope{ti.recorder, "GpuInputUpload"};
            auto staging_input_buffer = ti.device.create_buffer({
                .size = sizeof(GpuInput),
                .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
                .name = "staging_input_buffer",
            });
            ti.recorder.destroy_buffer_deferred(staging_input_buffer);
            auto *buffer_ptr = ti.device.get_host_address_as<GpuInput>(staging_input_buffer).value();
            *buffer_ptr = gpu_input;
            ti.recorder.copy_buffer_to_buffer({
                .src_buffer = staging_input_buffer,
                .dst_buffer = gpu_context.task_input_buffer.get_state().buffers[0],
                .size = sizeof(GpuInput),
            });
        },
        .name = "GpuInputUploadTransferTask",
    });

    voxel_world.record_frame(gpu_context, voxel_model_loader.task_gvox_model_buffer, particles);
    particles.simulate(gpu_context, voxel_world.buffers);

    renderer.render(gpu_context, voxel_world.buffers, particles, gpu_context.task_swapchain_image, gpu_context.swapchain.get_format());

    gpu_context.frame_task_graph.add_task({
        .attachments = {
            daxa::inl_attachment(daxa::TaskBufferAccess::TRANSFER_READ, gpu_context.task_output_buffer),
            daxa::inl_attachment(daxa::TaskBufferAccess::HOST_TRANSFER_WRITE, gpu_context.task_staging_output_buffer),
        },
        .task = [this](daxa::TaskInterface const &ti) {
            auto prof = gpu_profiler::Scope{ti.recorder, "GpuOutputDownload"};
            auto output_buffer = gpu_context.task_output_buffer.get_state().buffers[0];
            auto staging_output_buffer = gpu_context.staging_output_buffer;
            auto frame_index = gpu_input.frame_index + 1;
            auto *buffer_ptr = ti.device.get_host_address_as<std::array<GpuOutput, (FRAMES_IN_FLIGHT + 1)>>(staging_output_buffer).value();
            daxa_u32 const offset = frame_index % (FRAMES_IN_FLIGHT + 1);
            gpu_output = (*buffer_ptr)[offset];
            ti.recorder.copy_buffer_to_buffer({
                .src_buffer = output_buffer,
                .dst_buffer = staging_output_buffer,
                .size = sizeof(GpuOutput) * (FRAMES_IN_FLIGHT + 1),
            });
        },
        .name = "GpuOutputDownloadTransferTask",
    });

    gpu_context.frame_task_graph.add_task({
        .attachments = {
            daxa::inl_attachment(daxa::TaskImageAccess::COLOR_ATTACHMENT, daxa::ImageViewType::REGULAR_2D, gpu_context.task_swapchain_image),
        },
        .task = [this](daxa::TaskInterface const &ti) {
            auto prof = gpu_profiler::Scope{ti.recorder, "ImGuiDraw"};
            imgui_renderer.record_commands(ImGui::GetDrawData(), ti.recorder, gpu_context.swapchain_image, window_size.x, window_size.y);
        },
        .name = "ImGui draw",
    });

    // --- --screenshot readback ------------------------------------------------------------------
    // Recorded after "ImGui draw" so the capture includes the debug overlay, and only when
    // --screenshot was passed. Recording it unconditionally would add a swapchain layout
    // transition to every frame of every benchmark run for a feature almost no run uses.
    screenshot_enabled = !AppCli::get().screenshot_path.empty();
    if (screenshot_enabled) {
        // Re-created here rather than in the constructor because record_tasks() also runs on
        // every resize, and the buffer has to match the current swapchain extent.
        if (!screenshot_buffer.is_empty()) {
            gpu_context.device.destroy_buffer(screenshot_buffer);
        }
        // The swapchain, not window_size -- see the member's declaration for why.
        auto const extent = gpu_context.swapchain.get_surface_extent();
        screenshot_extent = {extent.x, extent.y};
        screenshot_buffer = gpu_context.device.create_buffer({
            .size = static_cast<daxa_u64>(screenshot_extent.x) * screenshot_extent.y * 4,
            .allocate_info = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
            .name = "screenshot_readback",
        });
        gpu_context.frame_task_graph.add_task({
            .attachments = {
                daxa::inl_attachment(daxa::TaskImageAccess::TRANSFER_READ, daxa::ImageViewType::REGULAR_2D, gpu_context.task_swapchain_image),
            },
            .task = [this](daxa::TaskInterface const &ti) {
                if (!screenshot_capture_this_frame) {
                    return;
                }
                ti.recorder.copy_image_to_buffer({
                    .image = gpu_context.swapchain_image,
                    .image_layout = daxa::ImageLayout::TRANSFER_SRC_OPTIMAL,
                    // Defaults are mip 0, layer 0, one layer -- which is the whole swapchain
                    // image. daxa's ImageArraySlice carries no aspect field; colour is implied.
                    .image_slice = {},
                    .image_offset = {0, 0, 0},
                    .image_extent = {screenshot_extent.x, screenshot_extent.y, 1},
                    .buffer = screenshot_buffer,
                    .buffer_offset = 0,
                });
            },
            .name = "Screenshot readback",
        });
    }

    gpu_context.frame_task_graph.submit({});
    gpu_context.frame_task_graph.present({});
    gpu_context.frame_task_graph.complete({});

    gpu_context.startup_task_graph.submit({});
    gpu_context.startup_task_graph.complete({});

    needs_vram_calc = true;
}

// void VoxelApp::gpu_app_draw_ui() {
//     for (auto const &str : ui_strings) {
//         ImGui::Text("%s", str.c_str());
//     }
//     if (ImGui::TreeNode("Player")) {
//         ImGui::Text("pos: %.2f, %.2f, %.2f", static_cast<double>(gpu_output.player_pos.x), static_cast<double>(gpu_output.player_pos.y), static_cast<double>(gpu_output.player_pos.z));
//         ImGui::Text("y/p/r: %.2f, %.2f, %.2f", static_cast<double>(gpu_output.player_rot.x), static_cast<double>(gpu_output.player_rot.y), static_cast<double>(gpu_output.player_rot.z));
//         ImGui::Text("unit offs: %.2f, %.2f, %.2f", static_cast<double>(gpu_output.player_unit_offset.x), static_cast<double>(gpu_output.player_unit_offset.y), static_cast<double>(gpu_output.player_unit_offset.z));
//         ImGui::TreePop();
//     }
//     if (ImGui::TreeNode("Auto-Exposure")) {
//         ImGui::Text("Exposure multiple: %.2f", static_cast<double>(gpu_input.pre_exposure));
//         auto hist_float = std::array<float, LUMINANCE_HISTOGRAM_BIN_COUNT>{};
//         auto hist_min = static_cast<float>(kajiya_renderer.post_processor.histogram[0]);
//         auto hist_max = static_cast<float>(kajiya_renderer.post_processor.histogram[0]);
//         auto first_bin_with_value = -1;
//         auto last_bin_with_value = -1;
//         for (uint32_t i = 0; i < LUMINANCE_HISTOGRAM_BIN_COUNT; ++i) {
//             if (first_bin_with_value == -1 && kajiya_renderer.post_processor.histogram[i] != 0) {
//                 first_bin_with_value = i;
//             }
//             if (kajiya_renderer.post_processor.histogram[i] != 0) {
//                 last_bin_with_value = i;
//             }
//             hist_float[i] = static_cast<float>(kajiya_renderer.post_processor.histogram[i]);
//             hist_min = std::min(hist_min, hist_float[i]);
//             hist_max = std::max(hist_max, hist_float[i]);
//         }
//         ImGui::PlotHistogram("Histogram", hist_float.data(), static_cast<int>(hist_float.size()), 0, "hist", hist_min, hist_max, ImVec2(0, 120.0f));
//         ImGui::Text("min %.2f | max %.2f", static_cast<double>(hist_min), static_cast<double>(hist_max));
//         auto a = double(first_bin_with_value) / 256.0 * (LUMINANCE_HISTOGRAM_MAX_LOG2 - LUMINANCE_HISTOGRAM_MIN_LOG2) + LUMINANCE_HISTOGRAM_MIN_LOG2;
//         auto b = double(last_bin_with_value) / 256.0 * (LUMINANCE_HISTOGRAM_MAX_LOG2 - LUMINANCE_HISTOGRAM_MIN_LOG2) + LUMINANCE_HISTOGRAM_MIN_LOG2;
//         ImGui::Text("first bin %d (%.2f) | last bin %d (%.2f)", first_bin_with_value, exp2(a), last_bin_with_value, exp2(b));
//         ImGui::TreePop();
//     }
// }
void VoxelApp::calc_vram_usage() {
    std::vector<debug_utils::DebugDisplay::GpuResourceInfo> &debug_gpu_resource_infos = debug_utils::DebugDisplay::s_instance->gpu_resource_infos;

    debug_gpu_resource_infos.clear();
    ui_strings.clear();

    size_t result_size = 0;

    auto format_to_pixel_size = [](daxa::Format format) -> daxa_u32 {
        switch (format) {
        case daxa::Format::R16G16B16_SFLOAT: return 3 * 2;
        case daxa::Format::R16G16B16A16_SFLOAT: return 4 * 2;
        case daxa::Format::R32G32B32_SFLOAT: return 3 * 4;
        default:
        case daxa::Format::R32G32B32A32_SFLOAT: return 4 * 4;
        }
    };

    auto image_size = [this, &format_to_pixel_size, &result_size, &debug_gpu_resource_infos](daxa::ImageId image) {
        if (image.is_empty()) {
            return;
        }
        auto image_info = gpu_context.device.info_image(image).value();
        auto size = format_to_pixel_size(image_info.format) * image_info.size.x * image_info.size.y * image_info.size.z;
        debug_gpu_resource_infos.push_back({
            .type = "image",
            .name = image_info.name.data(),
            .size = size,
        });
        result_size += size;
    };
    auto buffer_size = [this, &result_size, &debug_gpu_resource_infos](daxa::BufferId buffer) {
        if (buffer.is_empty()) {
            return;
        }
        auto buffer_info = gpu_context.device.info_buffer(buffer).value();
        debug_gpu_resource_infos.push_back({
            .type = "buffer",
            .name = buffer_info.name.data(),
            .size = buffer_info.size,
        });
        result_size += buffer_info.size;
    };

    buffer_size(gpu_context.input_buffer);

    for (auto &[name, temporal_buffer] : gpu_context.temporal_buffers) {
        buffer_size(temporal_buffer.resource_id);
    }
    for (auto &[name, temporal_image] : gpu_context.temporal_images) {
        image_size(temporal_image.resource_id);
    }

#if defined(VOXELS_ORIGINAL_IMPL)
    buffer_size(voxel_world.buffers.voxel_malloc.allocator_buffer);
    buffer_size(voxel_world.buffers.voxel_malloc.element_buffer);
    buffer_size(voxel_world.buffers.voxel_malloc.available_element_stack_buffer);
    buffer_size(voxel_world.buffers.voxel_malloc.released_element_stack_buffer);
#endif

    {
        auto size = gpu_context.frame_task_graph.get_transient_memory_size();
        debug_gpu_resource_infos.push_back({
            .type = "buffer",
            .name = "Per-frame Transient Memory Buffer",
            .size = size,
        });
        result_size += size;
    }

    needs_vram_calc = false;

    ui_strings.push_back(fmt::format("Est. VRAM usage: {} MB", static_cast<float>(result_size) / 1000000));
}
