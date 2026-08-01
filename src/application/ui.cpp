#include "ui.hpp"

#include <imgui_stdlib.h>
#include <imgui_impl_glfw.h>
#include <fmt/format.h>
#include <sago/platform_folders.h>
#include <nfd.h>
#include <application/cli.hpp>
#include <utilities/debug.hpp>

#include <vector>
#include <iostream>
#include <cstdlib>
#include <cctype>
#include <charconv>
#include <fstream>
#include <string_view>

using namespace std::literals;

static constexpr std::array control_strings = {
    "Move Forward",
    "Strafe Left",
    "Move Backward",
    "Strafe Right",
    "Reload Chunks",
    "Toggle Fly",
    "Interact 0",
    "Interact 1",
    "Jump",
    "Crouch",
    "Sprint",
    "Walk",
    "Change Camera",
    "Toggle Brush Placement",
    "Brush A",
    "Brush B",
};

static constexpr std::array conflict_resolution_strings = {
    "swap",
    "remove old",
    "cancel",
};

inline auto get_key_string(daxa_i32 glfw_key_id) -> char const * {
    const auto *result = glfwGetKeyName(glfw_key_id, 0);
    if (result == nullptr) {
        switch (glfw_key_id) {
        case GLFW_KEY_SPACE: result = "space"; break;
        case GLFW_KEY_LEFT_SHIFT: result = "left shift"; break;
        case GLFW_KEY_LEFT_CONTROL: result = "left ctrl"; break;
        case GLFW_KEY_LEFT_ALT: result = "left alt"; break;
        case GLFW_KEY_RIGHT_SHIFT: result = "right shift"; break;
        case GLFW_KEY_RIGHT_CONTROL: result = "right ctrl"; break;
        case GLFW_KEY_RIGHT_ALT: result = "right alt"; break;
        case GLFW_KEY_UP: result = "arrow up"; break;
        case GLFW_KEY_DOWN: result = "arrow down"; break;
        case GLFW_KEY_LEFT: result = "arrow left"; break;
        case GLFW_KEY_RIGHT: result = "arrow right"; break;
        case GLFW_KEY_F1: result = "f1"; break;
        case GLFW_KEY_F2: result = "f2"; break;
        case GLFW_KEY_F3: result = "f3"; break;
        case GLFW_KEY_F4: result = "f4"; break;
        case GLFW_KEY_F5: result = "f5"; break;
        case GLFW_KEY_F6: result = "f6"; break;
        case GLFW_KEY_F7: result = "f7"; break;
        case GLFW_KEY_F8: result = "f8"; break;
        case GLFW_KEY_F9: result = "f9"; break;
        case GLFW_KEY_F10: result = "f10"; break;
        case GLFW_KEY_F11: result = "f11"; break;
        case GLFW_KEY_F12: result = "f12"; break;
        default: result = "unknown key"; break;
        }
    }
    return result;
}

inline auto get_button_string(daxa_i32 glfw_key_id) -> char const * {
    switch (glfw_key_id) {
    case GLFW_MOUSE_BUTTON_1: return "left mouse button";
    case GLFW_MOUSE_BUTTON_2: return "right mouse button";
    case GLFW_MOUSE_BUTTON_3: return "middle mouse button";
    case GLFW_MOUSE_BUTTON_4: return "mouse button 4";
    case GLFW_MOUSE_BUTTON_5: return "mouse button 5";
    default: return "unknown button";
    }
}

namespace {
    // F1 -- the measurement-hygiene fix. See docs/design/PERFORMANCE_PLAN.md section 2.5 trap 1.
    //
    // sago::getDataHome() resolves through the Win32 known-folder API, NOT the %APPDATA%
    // environment variable, so setting %APPDATA% for a child process does nothing. Every build
    // of this engine on this machine -- C:\voxl2, C:\voxl2_prof, C:\voxl2rs, C:\voxl2_ws --
    // therefore reads and writes the one file %APPDATA%\GabeVoxelGame\user_settings.json, and
    // the settings UI rewrites it on every change (this file, `settings_ui()` and the autosave
    // in `update()`). With several agents driving the engine at once, one run silently acquires
    // another's "Render Res Scale", "global_illumination" or "Update Sky" mid-experiment.
    //
    // That is not a hypothetical: one profiled run read 5.06 ms instead of 11.34 because a
    // sibling had left global_illumination = false, and another agent's headline result came out
    // with the sign reversed. Three agents lost a day each to it.
    //
    // VOXL_DATA_DIR points this process at its own directory, so a harness can give every run a
    // private settings file. Unset, the behaviour is byte-for-byte what it was before.
    auto resolve_data_directory() -> std::filesystem::path {
        if (auto const *over = std::getenv("VOXL_DATA_DIR"); over != nullptr && *over != '\0') {
            return std::filesystem::path{over};
        }
        return std::filesystem::path(sago::getDataHome()) / "GabeVoxelGame";
    }
} // namespace

AppUi::AppUi(GLFWwindow *glfw_window_ptr)
    : glfw_window_ptr{glfw_window_ptr},
      data_directory{resolve_data_directory()} {
    ImGui::CreateContext();
    auto &style = ImGui::GetStyle();
    auto &io = ImGui::GetIO();
    mono_font = io.Fonts->AddFontFromFileTTF("assets/fonts/Roboto_Mono/RobotoMono-Regular.ttf", 14.0f * 2.0f);
    menu_font = io.Fonts->AddFontFromFileTTF("assets/fonts/Inter_Tight/InterTight-Regular.ttf", 14.0f * 2.0f);
    if (menu_font == nullptr) {
        menu_font = io.Fonts->AddFontDefault();
    }
    if (mono_font == nullptr) {
        mono_font = io.Fonts->AddFontDefault();
    }

    constexpr auto ColorFromBytes = [](uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
        return ImVec4(static_cast<daxa_f32>(r) / 255.0f, static_cast<daxa_f32>(g) / 255.0f, static_cast<daxa_f32>(b) / 255.0f, static_cast<daxa_f32>(a) / 255.0f);
    };
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImVec4 *colors = style.Colors;
    auto bgColor = ColorFromBytes(37, 37, 38);
    auto lightBgColor = ColorFromBytes(82, 82, 85);
    auto veryLightBgColor = ColorFromBytes(90, 90, 95);
    auto panelColor = ColorFromBytes(51, 51, 55);
    auto panelHoverColor = ColorFromBytes(29, 151, 236);
    auto panelActiveColor = ColorFromBytes(0, 119, 200);
    auto textColor = ColorFromBytes(255, 255, 255);
    auto textDisabledColor = ColorFromBytes(151, 151, 151);
    auto borderColor = ColorFromBytes(0, 0, 0, 80);
    colors[ImGuiCol_Text] = textColor;
    colors[ImGuiCol_TextDisabled] = textDisabledColor;
    colors[ImGuiCol_TextSelectedBg] = panelActiveColor;
    colors[ImGuiCol_WindowBg] = bgColor;
    colors[ImGuiCol_ChildBg] = bgColor;
    colors[ImGuiCol_PopupBg] = bgColor;
    colors[ImGuiCol_Border] = borderColor;
    colors[ImGuiCol_BorderShadow] = borderColor;
    colors[ImGuiCol_FrameBg] = panelColor;
    colors[ImGuiCol_FrameBgHovered] = panelHoverColor;
    colors[ImGuiCol_FrameBgActive] = panelActiveColor;
    colors[ImGuiCol_TitleBg] = bgColor;
    colors[ImGuiCol_TitleBgActive] = bgColor;
    colors[ImGuiCol_TitleBgCollapsed] = bgColor;
    colors[ImGuiCol_MenuBarBg] = panelColor;
    colors[ImGuiCol_ScrollbarBg] = panelColor;
    colors[ImGuiCol_ScrollbarGrab] = lightBgColor;
    colors[ImGuiCol_ScrollbarGrabHovered] = veryLightBgColor;
    colors[ImGuiCol_ScrollbarGrabActive] = veryLightBgColor;
    colors[ImGuiCol_CheckMark] = panelActiveColor;
    colors[ImGuiCol_SliderGrab] = panelHoverColor;
    colors[ImGuiCol_SliderGrabActive] = panelActiveColor;
    colors[ImGuiCol_Button] = panelColor;
    colors[ImGuiCol_ButtonHovered] = panelHoverColor;
    colors[ImGuiCol_ButtonActive] = panelHoverColor;
    colors[ImGuiCol_Header] = panelColor;
    colors[ImGuiCol_HeaderHovered] = panelHoverColor;
    colors[ImGuiCol_HeaderActive] = panelActiveColor;
    colors[ImGuiCol_Separator] = borderColor;
    colors[ImGuiCol_SeparatorHovered] = borderColor;
    colors[ImGuiCol_SeparatorActive] = borderColor;
    colors[ImGuiCol_ResizeGrip] = bgColor;
    colors[ImGuiCol_ResizeGripHovered] = panelColor;
    colors[ImGuiCol_ResizeGripActive] = lightBgColor;
    colors[ImGuiCol_PlotLines] = panelActiveColor;
    colors[ImGuiCol_PlotLinesHovered] = panelHoverColor;
    colors[ImGuiCol_PlotHistogram] = panelActiveColor;
    colors[ImGuiCol_PlotHistogramHovered] = panelHoverColor;
    colors[ImGuiCol_DragDropTarget] = bgColor;
    colors[ImGuiCol_NavHighlight] = bgColor;
    colors[ImGuiCol_Tab] = bgColor;
    colors[ImGuiCol_TabActive] = panelActiveColor;
    colors[ImGuiCol_TabUnfocused] = bgColor;
    colors[ImGuiCol_TabUnfocusedActive] = panelActiveColor;
    colors[ImGuiCol_TabHovered] = panelHoverColor;
    style.WindowRounding = 4.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.TabRounding = 4.0f;
    style.FramePadding = {4.0f, 3.0f};

    if (!std::filesystem::exists(data_directory)) {
        // create_directories, not create_directory: a VOXL_DATA_DIR handed over by a harness is
        // typically a nested per-run path (C:\voxl2\.runs\sweep\p3) whose parents do not exist
        // yet, and create_directory fails rather than creating them.
        std::filesystem::create_directories(data_directory);
    }
    // Logged unconditionally so a capture can always be traced back to the settings file it
    // actually used -- the whole point of F1 is that this is no longer assumed to be one place.
    debug_utils::Console::add_log(fmt::format("[settings] data directory: {}\n", data_directory.string()));

    if (std::filesystem::exists(data_directory / "user_settings.json")) {
        settings.load(data_directory / "user_settings.json");
    } else {
        settings.reset_default();
        settings.save(data_directory / "user_settings.json");
    }

    rescale_ui();

    ImGui_ImplGlfw_InitForVulkan(glfw_window_ptr, true);
}

AppUi::~AppUi() {
    auto autosave = AppSettings::get<settings::Checkbox>("UI", "autosave").value;
    if ((autosave || autosave_override) && needs_saving) {
        settings.save(data_directory / "user_settings.json");
    }
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void AppUi::rescale_ui() {
    mono_font->Scale = ui_scale * 0.5f;
    menu_font->Scale = ui_scale * 0.5f;
    auto &style = ImGui::GetStyle();
    style.FramePadding = {ui_scale * 4.0f, ui_scale * 3.0f};
}

template <class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};

namespace {
    bool settings_entry_ui(SettingId const &id, SettingEntry &entry) {
        bool changed = false;

        auto context_menu = [&](auto extra_code) {
            if (ImGui::BeginPopupContextItem(id.c_str())) {
                ImGui::Text("%s", id.c_str());
                extra_code();
                if (ImGui::Button("Save Default")) {
                    entry.user_default = entry.data;
                    changed = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Reset")) {
                    entry.data = entry.user_default;
                    changed = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Factory Reset")) {
                    entry.data = entry.factory_default;
                    changed = true;
                }
                ImGui::EndPopup();
            }
        };

        std::visit(
            overloaded{
                [&](settings::InputFloat &data) {
                    changed = ImGui::InputFloat(id.c_str(), &data.value, 0.0f, 0.0f, "%.6f");
                    context_menu([]() {});
                },
                [&](settings::InputFloat3 &data) {
                    changed = ImGui::InputFloat3(id.c_str(), &data.value.x, "%.6f");
                    context_menu([]() {});
                },
                [&](settings::SliderFloat &data) {
                    changed = ImGui::SliderFloat(id.c_str(), &data.value, data.min, data.max);
                    context_menu([&]() {
                        auto changed0 = ImGui::InputFloat("min", &data.min);
                        auto changed1 = ImGui::InputFloat("max", &data.max);
                        changed = changed || changed0 || changed1;
                    });
                },
                [&](settings::Checkbox &data) {
                    changed = ImGui::Checkbox(id.c_str(), &data.value);
                    context_menu([]() {});
                },
                [&](settings::ComboBox &data) {
                    auto &item_current_idx = data.value;
                    if (item_current_idx >= entry.config.options.size()) {
                        // ???
                        item_current_idx = 0;
                        changed = true;
                    }
                    if (ImGui::BeginCombo(id.c_str(), entry.config.options[item_current_idx].c_str())) {
                        for (int32_t option_i = 0; option_i < entry.config.options.size(); ++option_i) {
                            auto const &option_str = entry.config.options[option_i];
                            const bool is_selected = (item_current_idx == option_i);
                            if (ImGui::Selectable(option_str.c_str(), is_selected)) {
                                item_current_idx = option_i;
                                changed = true;
                            }
                            // Set the initial focus when opening the combo (scrolling + keyboard navigation focus)
                            if (is_selected) {
                                ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::EndCombo();
                    }

                    context_menu([]() {});
                },
            },
            entry.data);

        return changed;
    }
} // namespace

void AppUi::settings_ui() {
    ImGui::Begin("Settings");

    auto autosave_0 = AppSettings::get<settings::Checkbox>("UI", "autosave").value;

    auto new_ui_scale = std::clamp(AppSettings::get<settings::InputFloat>("UI", "Scale").value, 0.5f, 2.0f);
    if (new_ui_scale != ui_scale) {
        ui_scale = new_ui_scale;
        rescale_ui();
    }
    const auto bottom_bar_size = 32 * ui_scale + 12;
    ImGui::BeginChild("Child0", ImVec2(0, ImGui::GetContentRegionAvail().y - bottom_bar_size));

    if (ImGui::BeginTabBar("##settings_tabs")) {
        if (ImGui::BeginTabItem("App")) {
            auto &settings = *AppSettings::s_instance;
            for (auto &[cat_id, category] : settings.categories) {
                auto category_open = ImGui::TreeNode(cat_id.c_str());
                if (ImGui::BeginPopupContextItem()) {
                    ImGui::Text("%s", cat_id.c_str());
                    if (ImGui::Button("Save Defaults")) {
                        for (auto &[id, entry] : category) {
                            entry.user_default = entry.data;
                            needs_saving = true;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Reset")) {
                        for (auto &[id, entry] : category) {
                            entry.data = entry.user_default;
                            needs_saving = true;
                            if (entry.config.task_graph_depends) {
                                should_record_task_graph = true;
                            }
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Factory Reset")) {
                        for (auto &[id, entry] : category) {
                            entry.data = entry.factory_default;
                            needs_saving = true;
                            if (entry.config.task_graph_depends) {
                                should_record_task_graph = true;
                            }
                        }
                    }
                    ImGui::EndPopup();
                }
                if (category_open) {
                    for (auto &[id, entry] : category) {
                        if (settings_entry_ui(id, entry)) {
                            needs_saving = true;
                            if (entry.config.task_graph_depends) {
                                should_record_task_graph = true;
                            }
                        }
                    }
                    ImGui::TreePop();
                }
            }

            if (ImGui::TreeNode("Brush")) {
                if (ImGui::InputText("World Seed", &settings.world_seed_str)) {
                    should_upload_seed_data = true;
                }
                ImGui::TreePop();
            }
            if (ImGui::Button("Re-run Startup")) {
                should_run_startup = true;
            }
            if (ImGui::Button("Open Model")) {
                nfdchar_t *out_path = nullptr;
                nfdresult_t const result = NFD_OpenDialog("gvox,vox,vxl,gvp,rle,oct,glp,brk", (data_directory / "models").string().c_str(), &out_path);
                if (result == NFD_OKAY) {
                    gvox_model_path = out_path;
                    should_upload_gvox_model = true;
                    debug_utils::Console::add_log(fmt::format("Loaded {}", out_path));
                    free(out_path);
                } else if (result != NFD_CANCEL) {
                    debug_utils::Console::add_log(fmt::format("[error]: {}", NFD_GetError()));
                }
            }
            ImGui::Checkbox("Hot-load Shaders", &should_hotload_shaders);
            ImGui::Checkbox("Show ImGui Demo Window", &show_imgui_demo_window);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Controls")) {
            settings_controls_ui();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Passes")) {
            settings_passes_ui();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::EndChild();

    ImGui::Text("Settings");
    ImGui::SameLine();

    auto autosave = AppSettings::get<settings::Checkbox>("UI", "autosave").value;

    if (autosave_0 != autosave) {
        autosave_override = true;
    }
    if (!autosave) {
        ImGui::SameLine();
        if (ImGui::Button("Save")) {
            settings.save(data_directory / "user_settings.json");
        }
        ImGui::SameLine();
        if (ImGui::Button("Load")) {
            settings.load(data_directory / "user_settings.json");
            needs_saving = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        settings.reset_default();
        for (auto &[cat_id, category] : settings.categories) {
            for (auto &[id, entry] : category) {
                entry.data = entry.user_default;
                if (entry.config.task_graph_depends) {
                    should_record_task_graph = true;
                }
            }
        }
        needs_saving = true;
    }
    ImGui::End();
}

void AppUi::settings_controls_ui() {
    if (ImGui::BeginCombo("Conflict Resolution Mode", conflict_resolution_strings[conflict_resolution_mode])) {
        for (daxa_u32 mode_i = 0; mode_i < conflict_resolution_strings.size(); ++mode_i) {
            bool const is_selected = (mode_i == conflict_resolution_mode);
            if (ImGui::Selectable(conflict_resolution_strings[mode_i], is_selected)) {
                conflict_resolution_mode = mode_i;
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::SliderFloat("Mouse Sensitivity", &settings.mouse_sensitivity, 0.1f, 10.0f)) {
        needs_saving = true;
    }
    if (ImGui::BeginTable("controls_table", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersV | ImGuiTableFlags_ScrollY, ImVec2(0, -(32 * ui_scale + 12)))) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 0.0f, 0);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 0.0f, 1);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < control_strings.size(); ++i) {
            ImGui::TableNextRow(ImGuiTableRowFlags_None);
            if (ImGui::TableSetColumnIndex(0)) {
                ImGui::Text("%s", control_strings[i]);
            }
            if (ImGui::TableSetColumnIndex(1)) {
                if (static_cast<daxa_i32>(i) == limbo_action_index) {
                    ImGui::Button("<press any key>", ImVec2(-FLT_MIN, 0.0f));
                    if (ImGui::IsKeyDown(ImGuiKey_Escape)) {
                        if (limbo_is_button) {
                            settings.mouse_button_binds.erase(limbo_key_index);
                        } else {
                            settings.keybinds.erase(limbo_key_index);
                        }
                        limbo_action_index = INVALID_GAME_ACTION;
                    } else {
                        auto resolve_action = [this](daxa_i32 key_i, std::map<daxa_i32, daxa_i32> &bindings, bool contains_override) {
                            // set new key
                            new_key_id = key_i;
                            if (bindings.contains(key_i)) {
                                if (limbo_key_index != new_key_id || contains_override) {
                                    // new key to set, but already in bindings
                                    switch (conflict_resolution_mode) {
                                    case 0: {
                                        auto prev_action = bindings[key_i];
                                        bindings[key_i] = limbo_action_index;
                                        if (limbo_is_button) {
                                            settings.mouse_button_binds[limbo_key_index] = prev_action;
                                        } else {
                                            settings.keybinds[limbo_key_index] = prev_action;
                                        }
                                    } break;
                                    case 1: {
                                        bindings[key_i] = limbo_action_index;
                                        if (limbo_is_button) {
                                            settings.mouse_button_binds.erase(limbo_key_index);
                                        } else {
                                            settings.keybinds.erase(limbo_key_index);
                                        }
                                    } break;
                                    case 2: // cancel
                                        break;
                                    }
                                    needs_saving = true;
                                } else {
                                    // same key was pressed. No need to do anything
                                }
                            } else {
                                if (limbo_is_button) {
                                    settings.mouse_button_binds.erase(limbo_key_index);
                                } else {
                                    settings.keybinds.erase(limbo_key_index);
                                }
                                bindings[key_i] = limbo_action_index;
                                needs_saving = true;
                            }
                            limbo_action_index = INVALID_GAME_ACTION;
                        };
                        for (daxa_i32 key_i = 0; key_i < GLFW_KEY_LAST + 1; ++key_i) {
                            auto key_state = glfwGetKey(glfw_window_ptr, key_i);
                            if (key_state != GLFW_RELEASE) {
                                resolve_action(key_i, settings.keybinds, limbo_is_button);
                                break;
                            }
                        }
                        if (limbo_action_index != INVALID_GAME_ACTION) {
                            for (daxa_i32 button_i = 0; button_i < GLFW_MOUSE_BUTTON_LAST + 1; ++button_i) {
                                auto key_state = glfwGetMouseButton(glfw_window_ptr, button_i);
                                if (key_state != GLFW_RELEASE) {
                                    resolve_action(button_i, settings.mouse_button_binds, !limbo_is_button);
                                    break;
                                }
                            }
                        }
                    }
                } else {
                    char const *key_name = nullptr;
                    auto temp_limbo_key_index = GLFW_KEY_LAST + 1;
                    auto temp_limbo_is_button = false;
                    if (key_name == nullptr) {
                        auto action_key_iter = std::find_if(
                            settings.keybinds.begin(),
                            settings.keybinds.end(),
                            [i](const auto &mo) { return mo.second == static_cast<daxa_i32>(i); });
                        if (action_key_iter != settings.keybinds.end()) {
                            key_name = get_key_string(action_key_iter->first);
                            temp_limbo_key_index = action_key_iter->first;
                            temp_limbo_is_button = false;
                        }
                    }
                    if (key_name == nullptr) {
                        auto action_button_iter = std::find_if(
                            settings.mouse_button_binds.begin(),
                            settings.mouse_button_binds.end(),
                            [i](const auto &mo) { return mo.second == static_cast<daxa_i32>(i); });
                        if (action_button_iter != settings.mouse_button_binds.end()) {
                            key_name = get_button_string(action_button_iter->first);
                            temp_limbo_key_index = action_button_iter->first;
                            temp_limbo_is_button = true;
                        }
                    }
                    if (key_name == nullptr) {
                        key_name = "Un-set";
                    }
                    auto key_str = std::string{key_name} + "##" + std::to_string(i);
                    if (ImGui::Button(key_str.c_str(), ImVec2(-FLT_MIN, 0.0f))) {
                        if (limbo_action_index == INVALID_GAME_ACTION) {
                            limbo_action_index = static_cast<daxa_i32>(i);
                            limbo_key_index = temp_limbo_key_index;
                            limbo_is_button = temp_limbo_is_button;
                        }
                    }
                }
            }
        }
        ImGui::EndTable();
    }
}

void AppUi::settings_passes_ui() {
    auto &self = *debug_utils::DebugDisplay::s_instance;
    for (uint32_t pass_i = 0; pass_i < self.passes.size(); ++pass_i) {
        if (self.selected_pass == pass_i) {
        }
        auto &pass = self.passes[pass_i];
        if (ImGui::Selectable(pass.name.c_str(), self.selected_pass == pass_i)) {
            if (self.selected_pass_name != pass.name) {
                self.selected_pass = pass_i;
                self.selected_pass_name = pass.name;
                should_record_task_graph = true;
            }
        }
        if (self.selected_pass == pass_i) {
            ImGui::SliderFloat("brightness", &pass.settings.brightness, 0.001f, 100.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
            if (pass.type == DEBUG_IMAGE_TYPE_3D) {
                ImGui::InputInt("slice", (int *)&pass.settings.flags);
            } else {
                ImGui::CheckboxFlags("gamma correct", &pass.settings.flags, 1u << DEBUG_IMAGE_FLAGS_GAMMA_CORRECT_INDEX);
            }
        }
    }
}

static ImGuiTableSortSpecs *current_gpu_resource_info_sort_specs = nullptr;

static auto compare_gpu_resource_infos(const void *lhs, const void *rhs) -> int {
    auto const *a = static_cast<debug_utils::DebugDisplay::GpuResourceInfo const *>(lhs);
    auto const *b = static_cast<debug_utils::DebugDisplay::GpuResourceInfo const *>(rhs);
    for (int n = 0; n < current_gpu_resource_info_sort_specs->SpecsCount; n++) {
        auto const *sort_spec = &current_gpu_resource_info_sort_specs->Specs[n];
        int delta = 0;
        switch (sort_spec->ColumnUserID) {
        case 0: delta = a->type.compare(b->type); break;
        case 1: delta = a->name.compare(b->name); break;
        case 2: delta = static_cast<daxa_i32>(static_cast<daxa_i64>(a->size) - static_cast<daxa_i64>(b->size)); break;
        default: break;
        }
        if (delta > 0) {
            return (sort_spec->SortDirection == ImGuiSortDirection_Ascending) ? +1 : -1;
        }
        if (delta < 0) {
            return (sort_spec->SortDirection == ImGuiSortDirection_Ascending) ? -1 : +1;
        }
    }
    return static_cast<daxa_i32>(static_cast<daxa_i64>(a->size) - static_cast<daxa_i64>(b->size));
}

// --- F2 / F3: settings provenance and command-line overrides ---------------------------------
namespace {
    /// Accepts the spellings a shell script is likely to produce. Deliberately strict about
    /// anything else: a benchmark that reads "--gi maybe" as false would produce a plausible
    /// frame time from the wrong renderer, which is the exact failure mode F2 exists to catch.
    auto parse_bool_value(std::string_view text, bool &out) -> bool {
        auto lowered = std::string{text};
        for (auto &c : lowered) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (lowered == "1" || lowered == "on" || lowered == "true" || lowered == "yes") {
            out = true;
            return true;
        }
        if (lowered == "0" || lowered == "off" || lowered == "false" || lowered == "no") {
            out = false;
            return true;
        }
        return false;
    }

    /// Strip everything but letters and digits and lowercase the rest, so "FSR 2.2" and "fsr22"
    /// compare equal. Lets --taa take a human spelling instead of an index whose meaning depends
    /// on the order the options happen to be registered in.
    auto normalise_option(std::string_view text) -> std::string {
        auto out = std::string{};
        for (auto c : text) {
            if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
                out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
        }
        return out;
    }

    /// Resolve a combo-box value: either a plain index, or a unique prefix of one of the option
    /// names once normalised ("kajiya" -> "Kajiya TAA", "fsr2" -> "FSR 2.2").
    auto parse_combo_value(std::string_view text, std::vector<std::string> const &options, int32_t &out) -> bool {
        int32_t index = 0;
        auto const *first = text.data();
        auto const *last = text.data() + text.size();
        if (auto result = std::from_chars(first, last, index); result.ec == std::errc{} && result.ptr == last) {
            if (options.empty() || (index >= 0 && index < static_cast<int32_t>(options.size()))) {
                out = index;
                return true;
            }
            return false;
        }
        auto needle = normalise_option(text);
        if (needle.empty()) {
            return false;
        }
        int32_t match = -1;
        for (size_t i = 0; i < options.size(); ++i) {
            if (normalise_option(options[i]).rfind(needle, 0) == 0) {
                if (match >= 0) {
                    return false; // ambiguous; make the caller say which one
                }
                match = static_cast<int32_t>(i);
            }
        }
        if (match < 0) {
            return false;
        }
        out = match;
        return true;
    }

    /// Render one setting's current value for humans. Combo boxes print the option name rather
    /// than the index, because an index in a CSV header ages badly the moment an option is added.
    auto format_setting_value(SettingEntry const &entry) -> std::string {
        return std::visit(
            overloaded{
                [](settings::InputFloat const &x) { return fmt::format("{:g}", x.value); },
                [](settings::InputFloat3 const &x) { return fmt::format("{:g},{:g},{:g}", x.value.x, x.value.y, x.value.z); },
                [](settings::SliderFloat const &x) { return fmt::format("{:g}", x.value); },
                [](settings::Checkbox const &x) { return std::string{x.value ? "on" : "off"}; },
                [&entry](settings::ComboBox const &x) {
                    auto const &options = entry.config.options;
                    if (x.value >= 0 && x.value < static_cast<int32_t>(options.size())) {
                        return fmt::format("{}", options[static_cast<size_t>(x.value)]);
                    }
                    return fmt::format("{}", x.value);
                },
            },
            entry.data);
    }
} // namespace

auto AppUi::settings_summary(SettingCategoryId const &category_id) -> std::string {
    if (AppSettings::s_instance == nullptr) {
        return {};
    }
    auto const &categories = AppSettings::s_instance->categories;
    auto category_iter = categories.find(category_id);
    if (category_iter == categories.end()) {
        return {};
    }
    auto out = std::string{};
    // std::map iterates in key order, so the summary is stable between runs and two headers can
    // be diffed directly. That is the only reason this is worth writing down at all.
    for (auto const &[id, entry] : category_iter->second) {
        if (!out.empty()) {
            out += " | ";
        }
        out += fmt::format("{}={}", id, format_setting_value(entry));
    }
    return out;
}

// WHY THE FIRST update() AND NOT THE CONSTRUCTOR. The settings registry is built by
// AppSettings::add() calls spread across VoxelApp's constructor and six renderer headers, every
// one of which runs after AppUi is constructed. Applying an override any earlier would either
// find no entry at all or be overwritten when the entry is registered. The first update() is the
// earliest point at which the registry is complete, and it is still before any measurement
// window a harness would use -- the convergence wait is seconds and this is frame one.
void AppUi::apply_cli_setting_overrides() {
    for (auto const &override_entry : AppCli::get().setting_overrides) {
        auto entry = AppSettings::get(override_entry.category, override_entry.id);
        // A default-constructed SettingEntry means the id was never registered. Report it rather
        // than ignoring it: --reflections against a build without F7 must not look like it worked.
        auto const &categories = AppSettings::s_instance->categories;
        auto category_iter = categories.find(override_entry.category);
        auto known = category_iter != categories.end() &&
                     category_iter->second.find(override_entry.id) != category_iter->second.end();
        if (!known) {
            debug_utils::Console::add_log(
                fmt::format("[cli] {}: no setting '{}/{}' in this build -- ignored\n",
                            override_entry.origin, override_entry.category, override_entry.id));
            std::cerr << fmt::format("[cli] {}: no setting '{}/{}' in this build -- ignored\n",
                                     override_entry.origin, override_entry.category, override_entry.id);
            continue;
        }

        auto ok = std::visit(
            overloaded{
                [&](settings::InputFloat &x) {
                    float v = 0.0f;
                    auto const *first = override_entry.value.data();
                    auto const *last = first + override_entry.value.size();
                    auto r = std::from_chars(first, last, v);
                    if (r.ec != std::errc{} || r.ptr != last) {
                        return false;
                    }
                    x.value = v;
                    return true;
                },
                [&](settings::InputFloat3 &) { return false; },
                [&](settings::SliderFloat &x) {
                    float v = 0.0f;
                    auto const *first = override_entry.value.data();
                    auto const *last = first + override_entry.value.size();
                    auto r = std::from_chars(first, last, v);
                    if (r.ec != std::errc{} || r.ptr != last) {
                        return false;
                    }
                    // Clamped to the registered range rather than rejected: the slider's own
                    // bounds are the authority on what the renderer can actually do, and a
                    // silently out-of-range render scale would allocate absurd render targets.
                    if (v < x.min || v > x.max) {
                        debug_utils::Console::add_log(
                            fmt::format("[cli] {}: {} clamped to [{:g}, {:g}]\n",
                                        override_entry.origin, v, x.min, x.max));
                        v = v < x.min ? x.min : x.max;
                    }
                    x.value = v;
                    return true;
                },
                [&](settings::Checkbox &x) { return parse_bool_value(override_entry.value, x.value); },
                [&](settings::ComboBox &x) { return parse_combo_value(override_entry.value, entry.config.options, x.value); },
            },
            entry.data);

        if (!ok) {
            debug_utils::Console::add_log(
                fmt::format("[cli] {}: cannot read '{}' as a value for {}/{} -- ignored\n",
                            override_entry.origin, override_entry.value, override_entry.category, override_entry.id));
            std::cerr << fmt::format("[cli] {}: cannot read '{}' as a value for {}/{} -- ignored\n",
                                     override_entry.origin, override_entry.value, override_entry.category, override_entry.id);
            continue;
        }

        AppSettings::set(override_entry.category, override_entry.id, entry.data);
        // Exactly what settings_entry_ui() does when the user moves the control by hand: anything
        // the task graph is built from needs the graph re-recorded, which VoxelApp::on_update()
        // picks up. Without this, --render-scale would change the number and not the picture.
        if (entry.config.task_graph_depends) {
            should_record_task_graph = true;
        }
        debug_utils::Console::add_log(
            fmt::format("[cli] {}/{} = {}\n", override_entry.category, override_entry.id,
                        format_setting_value(AppSettings::get(override_entry.category, override_entry.id))));
    }

    // These overrides are a property of THIS run, not a preference. Saving them would write them
    // into user_settings.json and silently contaminate the next run launched with no flags --
    // which is trap 1 all over again, this time self-inflicted. Only suppressed when there was
    // actually something to suppress, so a plain run keeps upstream's autosave behaviour exactly.
    if (!AppCli::get().setting_overrides.empty()) {
        needs_saving = false;
    }
}

// F2. Snapshot the effective Graphics settings on the first frame, drop them next to the bench
// CSV, and keep checking them afterwards.
void AppUi::track_settings_provenance() {
    auto current = settings_summary("Graphics");
    // Latched on an explicit flag rather than on the summary being empty: if the registry were
    // ever empty on the first frame, an emptiness test would never latch and would rewrite the
    // sidecar every frame for the whole run.
    if (!settings_baseline_taken) {
        settings_baseline_taken = true;
        startup_graphics_summary = current;
        debug_utils::Console::add_log(fmt::format("[settings] Graphics: {}\n", current));

        // The sidecar exists because the CSV's own header is written by VoxelApp's constructor,
        // which runs BEFORE the overrides above are applied -- so anything written there would
        // record the pre-override configuration and be actively misleading. Writing the effective
        // settings from here, at the first frame, is the earliest honest moment. See the
        // integration note: the header line itself belongs in VoxelApp::write_bench_row().
        auto const &bench_csv = AppCli::get().bench_csv;
        if (!bench_csv.empty()) {
            auto sidecar = std::filesystem::path{bench_csv};
            sidecar += ".settings.txt";
            auto f = std::ofstream(sidecar);
            if (f.is_open()) {
                f << "# effective settings for " << bench_csv << "\n";
                f << "data_directory=" << data_directory.string() << "\n";
                for (auto const &[category_id, category] : AppSettings::s_instance->categories) {
                    f << category_id << ": " << settings_summary(category_id) << "\n";
                }
            }
        }
        return;
    }
    if (!settings_moved && current != startup_graphics_summary) {
        // Loud, once. A run that reconfigured itself halfway through is not a data point, and the
        // whole cost of finding that out should be paid here rather than in somebody's analysis.
        settings_moved = true;
        debug_utils::Console::add_log(
            fmt::format("[settings] WARNING: Graphics settings changed mid-run\n  was: {}\n  now: {}\n",
                        startup_graphics_summary, current));
        std::cerr << fmt::format("[settings] WARNING: Graphics settings changed mid-run\n  was: {}\n  now: {}\n",
                                 startup_graphics_summary, current);
    }
}

void AppUi::update(daxa_f32 delta_time, daxa_f32 cpu_delta_time) {
    if (!cli_overrides_applied) {
        cli_overrides_applied = true;
        apply_cli_setting_overrides();
    }
    track_settings_provenance();

    cpu_frametimes[frametime_rotation_index] = cpu_delta_time;
    full_frametimes[frametime_rotation_index] = delta_time;
    frametime_rotation_index = (frametime_rotation_index + 1) % full_frametimes.size();

    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGui::PushFont(menu_font);

    if (paused) {
        ImGuiDockNodeFlags const dockspace_flags = ImGuiDockNodeFlags_PassthruCentralNode;
        ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBackground;
        const ImGuiViewport *viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
        window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
        ImGuiID const dockspace_id = ImGui::GetID("MyDockSpace");
        ImGui::Begin("DockSpace Demo", nullptr, window_flags);
        ImGui::PopStyleVar(3);
        ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
        if (ImGui::BeginMenuBar()) {
            if (ImGui::Button("Settings")) {
                show_settings = !show_settings;
            }
            ImGui::EndMenuBar();
        }
        ImGui::End();

        if (show_imgui_demo_window) {
            ImGui::ShowDemoWindow(&show_imgui_demo_window);
        }

        auto show_console = AppSettings::get<settings::Checkbox>("UI", "show_console").value;
        if (show_console) {
            auto temp_show_console = show_console;
            debug_utils::Console::draw("Console", &show_console);
            if (temp_show_console != show_console) {
                AppSettings::set("UI", "show_console", settings::Checkbox{.value = show_console});
            }
        }

        if (show_settings) {
            settings_ui();
        }
    }

    auto show_debug_info = AppSettings::get<settings::Checkbox>("UI", "show_debug_info").value;

    if (show_debug_info) {
        ImGui::PushFont(mono_font);
        const ImGuiViewport *viewport = ImGui::GetMainViewport();
        auto pos = viewport->WorkPos;
        pos.x += viewport->WorkSize.x - debug_menu_size;
        ImGui::SetNextWindowPos(pos);
        ImGui::Begin("Debug Menu", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration);
        auto frametime_graph = [](auto &frametimes, uint64_t frametime_rot_index) {
            float average = 0.0f;
            for (auto frametime : frametimes) {
                average += frametime;
            }
            average /= static_cast<float>(frametimes.size());
            auto fmt_str = std::string();
            auto [min_frametime_iter, max_frametime_iter] = std::minmax_element(frametimes.begin(), frametimes.end());
            auto min_frametime = *min_frametime_iter;
            auto max_frametime = *max_frametime_iter;
            auto frametime_plot_min = floor(min_frametime * 100.0f) * 0.01f;
            auto frametime_plot_max = ceil(max_frametime * 100.0f) * 0.01f;
            fmt::format_to(std::back_inserter(fmt_str), "avg {:.2f} ms ({:.2f} fps)", average * 1000, 1.0f / average);
            ImGui::PlotLines("", frametimes.data(), static_cast<int>(frametimes.size()), static_cast<int>(frametime_rot_index), fmt_str.c_str(), frametime_plot_min, frametime_plot_max, ImVec2(0, 120.0f));
            ImGui::Text("min: %.2f ms, max: %.2f ms", static_cast<double>(min_frametime) * 1000, static_cast<double>(max_frametime) * 1000);
        };
        // --expand-graphs forces both frame-time nodes open.
        //
        // WHY THIS IS A FLAG AND NOT A DEFAULT. The node's open/closed state lives in imgui.ini,
        // which is a tracked file that every run rewrites, so "is the graph open?" depends on
        // what the previous run left behind. tools/bench.ps1 worked around that by synthesising
        // a mouse click at a hardcoded screen position -- `$x = $cw - 290 + 14` -- computed from
        // the panel's width. The panel is ImGuiWindowFlags_AlwaysAutoResize and pinned to the
        // right edge, so any debug string wider than the current widest row moves that target and
        // the click lands on the world instead, silently. This replaces the whole mechanism.
        auto const force_graphs_open = AppCli::get().expand_graphs;
        if (force_graphs_open) {
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        }
        if (ImGui::TreeNode("Full frame-time")) {
            frametime_graph(full_frametimes, frametime_rotation_index);
            ImGui::TreePop();
        }
        if (force_graphs_open) {
            ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        }
        if (ImGui::TreeNode("CPU-only frame-time")) {
            frametime_graph(cpu_frametimes, frametime_rotation_index);
            ImGui::TreePop();
        }
        for (auto const &[id, value] : debug_utils::DebugDisplay::s_instance->debug_strings) {
            ImGui::Text("%s: %s", id.c_str(), value.c_str());
        }
        // F2. The effective Graphics settings, in the overlay, so a screenshot carries the
        // configuration that produced it. Every capture in docs/images/ was previously a picture
        // of an unknown configuration -- with the settings file shared between four builds
        // (trap 1), "it was the default" was not something a screenshot could establish.
        if (settings_moved) {
            // Red and outside the tree node: this must be impossible to miss in a screenshot,
            // because every number in the run it labels is void.
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "SETTINGS CHANGED MID-RUN -- timings void");
        }
        // Open by default. The entire value of putting the settings here is that a screenshot
        // carries the configuration that produced it, and a collapsed node carries nothing --
        // ImGui does not persist tree state to imgui.ini, so it would be closed in every capture.
        // ImGuiCond_Once rather than Always so it can still be collapsed by hand when the overlay
        // is being used interactively; --expand-graphs pins it open for scripted captures.
        ImGui::SetNextItemOpen(true, force_graphs_open ? ImGuiCond_Always : ImGuiCond_Once);
        if (ImGui::TreeNode("Graphics (effective)")) {
            if (AppSettings::s_instance != nullptr) {
                auto const &categories = AppSettings::s_instance->categories;
                if (auto iter = categories.find("Graphics"); iter != categories.end()) {
                    for (auto const &[id, entry] : iter->second) {
                        ImGui::Text("%s: %s", id.c_str(), format_setting_value(entry).c_str());
                    }
                }
            }
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("GPU Resources")) {
            static ImGuiTableFlags const flags =
                ImGuiTableFlags_Resizable |
                ImGuiTableFlags_Reorderable |
                ImGuiTableFlags_Hideable |
                ImGuiTableFlags_Sortable |
                ImGuiTableFlags_SortMulti |
                ImGuiTableFlags_RowBg |
                ImGuiTableFlags_BordersOuter |
                ImGuiTableFlags_BordersV |
                ImGuiTableFlags_NoBordersInBody |
                ImGuiTableFlags_ScrollY |
                ImGuiTableFlags_SortMulti;

            if (ImGui::BeginTable("#gpu_resource_infos", 3, flags, ImVec2(0.0f, 200.0f), 0.0f)) {
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthFixed, 0.0f, 0);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 0.0f, 1);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_PreferSortDescending | ImGuiTableColumnFlags_WidthStretch, 0.0f, 2);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();

                if (ImGuiTableSortSpecs *sorts_specs = ImGui::TableGetSortSpecs()) {
                    if (sorts_specs->SpecsDirty) {
                        current_gpu_resource_info_sort_specs = sorts_specs;
                        if (debug_utils::DebugDisplay::s_instance->gpu_resource_infos.size() > 1) {
                            qsort(debug_utils::DebugDisplay::s_instance->gpu_resource_infos.data(), debug_utils::DebugDisplay::s_instance->gpu_resource_infos.size(), sizeof(debug_utils::DebugDisplay::s_instance->gpu_resource_infos[0]), compare_gpu_resource_infos);
                        }
                        current_gpu_resource_info_sort_specs = nullptr;
                        sorts_specs->SpecsDirty = false;
                    }
                }

                ImGuiListClipper clipper;
                clipper.Begin(static_cast<daxa_i32>(debug_utils::DebugDisplay::s_instance->gpu_resource_infos.size()));
                while (clipper.Step()) {
                    for (int row_i = clipper.DisplayStart; row_i < clipper.DisplayEnd; row_i++) {
                        auto const &res_info = debug_utils::DebugDisplay::s_instance->gpu_resource_infos[static_cast<size_t>(row_i)];
                        ImGui::PushID(&res_info);
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(res_info.type.c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(res_info.name.c_str());
                        ImGui::TableNextColumn();
                        ImGui::Text("%.4f MB", static_cast<double>(res_info.size) / 1000000);
                        ImGui::PopID();
                    }
                }
                ImGui::EndTable();
            }
            ImGui::TreePop();
        }

        debug_menu_size = ImGui::GetWindowSize().x;
        ImGui::End();
        ImGui::PopFont();
    }

    ImGui::PopFont();
    ImGui::Render();

    // Auto-save
    auto now = Clock::now();
    using namespace std::chrono_literals;
    auto autosave = AppSettings::get<settings::Checkbox>("UI", "autosave").value;
    if ((autosave || autosave_override) && needs_saving && now - last_save_time > 0.1s) {
        settings.save(data_directory / "user_settings.json");
        needs_saving = false;
        autosave_override = false;
    }
}

void AppUi::toggle_pause() {
    if (show_settings) {
        show_settings = false;
    } else {
        paused = !paused;
    }
}

void AppUi::toggle_debug() {
    auto show_debug_info = AppSettings::get<settings::Checkbox>("UI", "show_debug_info").value;
    AppSettings::set("UI", "show_debug_info", settings::Checkbox{.value = !show_debug_info});
    needs_saving = true;
}

void AppUi::toggle_console() {
    auto show_console = AppSettings::get<settings::Checkbox>("UI", "show_console").value;
    AppSettings::set("UI", "show_console", settings::Checkbox{.value = !show_console});
    needs_saving = true;
}
