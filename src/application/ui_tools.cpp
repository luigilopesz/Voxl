#include "ui_tools.hpp"

#include <application/settings.hpp>
#include <utilities/debug.hpp>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h> // ImGui::AddContextHook -- see install_hud()

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <string>

namespace {
    /// IM_COL32, but usable in a constexpr initialiser.
    constexpr auto col32(daxa_u32 r, daxa_u32 g, daxa_u32 b, daxa_u32 a) -> daxa_u32 {
        return (a << 24U) | (b << 16U) | (g << 8U) | r;
    }
    /// Same colour, different alpha. Used for the unselected cell borders.
    constexpr auto with_alpha(daxa_u32 colour, daxa_u32 a) -> daxa_u32 {
        return (colour & 0x00ffffffU) | (a << 24U);
    }

    // Family colours. Colour answers "what does this affect", the badge answers "which one of
    // them", and the name spelled out above the belt answers the rest. That split is what lets a
    // cell be 44 pixels wide and still be read at a glance.
    constexpr daxa_u32 ACCENT_TERRAIN = col32(214, 180, 132, 255); // sand
    constexpr daxa_u32 ACCENT_GRASS = col32(128, 204, 96, 255);    // ground cover
    constexpr daxa_u32 ACCENT_LIGHT = col32(255, 188, 84, 255);    // flame amber
    constexpr daxa_u32 ACCENT_TREE = col32(92, 176, 118, 255);     // planted things

    // --- how each tool is drawn ------------------------------------------------------------------
    // INDEXED BY BRUSH_ID_*, IN THAT ORDER. There is no name field: BRUSH_ID_NAMES in brushes.inl
    // is the label array, and duplicating it here is exactly the drift that array exists to
    // prevent. This table carries only what a shader has no opinion about.
    constexpr std::array<ToolStyle, BRUSH_ID_COUNT> k_styles{{
        {"-T", ACCENT_TERRAIN}, // BRUSH_ID_REMOVE_BALL
        {"+T", ACCENT_TERRAIN}, // BRUSH_ID_ADD_BALL
        {"+G", ACCENT_GRASS},   // BRUSH_ID_GRASS_BALL
        {"-G", ACCENT_GRASS},   // BRUSH_ID_REMOVE_GRASS
        {"Fl", ACCENT_GRASS},   // BRUSH_ID_FLOWERS
        {"Lb", ACCENT_LIGHT},   // BRUSH_ID_LIGHT_BALL
        {"La", ACCENT_LIGHT},   // BRUSH_ID_LANTERN
        {"Fi", ACCENT_LIGHT},   // BRUSH_ID_FIRE
        {"To", ACCENT_LIGHT},   // BRUSH_ID_TORCH
        {"Mp", ACCENT_TREE},    // BRUSH_ID_MAPLE_TREE
        {"Sp", ACCENT_TREE},    // BRUSH_ID_SPRUCE_TREE
    }};
    static_assert(k_styles.size() == BRUSH_ID_COUNT, "every BRUSH_ID_* needs a badge and a colour");

    /// The digit printed in a cell's corner: 1-9 then 0, empty past the tenth.
    auto digit_label(std::size_t index) -> std::string {
        if (index >= ToolBelt::DIGIT_SLOTS) {
            return {};
        }
        return index == 9 ? std::string("0") : std::to_string(index + 1);
    }

    /// Pull whole notches out of a fractional wheel accumulator, leaving the remainder behind.
    auto take_notches(float &accum, float delta) -> int {
        accum += delta;
        auto const notches = static_cast<int>(std::trunc(accum));
        accum -= static_cast<float>(notches);
        return notches;
    }

    auto brush_name(std::size_t brush_id) -> char const * {
        return BRUSH_ID_NAMES[std::min(brush_id, static_cast<std::size_t>(BRUSH_ID_COUNT - 1))];
    }
} // namespace

auto ToolBelt::effective_radius() const -> float {
    // brushes.glsl: `float brush_tree_radius() { return min(brush_input.radius, BRUSH_TREE_RADIUS_MAX); }`
    auto const is_tree = m_selected == BRUSH_ID_MAPLE_TREE || m_selected == BRUSH_ID_SPRUCE_TREE;
    return is_tree ? std::min(m_radius, BRUSH_TREE_RADIUS_MAX) : m_radius;
}

void ToolBelt::select(std::size_t index) {
    if (index < BRUSH_ID_COUNT) {
        m_selected = index;
    }
}

void ToolBelt::cycle(int notches) {
    if (notches == 0) {
        return;
    }
    // Signed arithmetic on purpose: reducing modulo the count in std::size_t first would turn a
    // wheel-down step into a huge positive offset. The negation is "wheel away from you selects
    // the previous slot", which is the direction the genre settled on.
    auto const count = static_cast<int>(BRUSH_ID_COUNT);
    auto wrapped = (static_cast<int>(m_selected) - notches) % count;
    if (wrapped < 0) {
        wrapped += count;
    }
    m_selected = static_cast<std::size_t>(wrapped);
}

auto ToolBelt::select_from_digit(int digit) -> bool {
    if (digit < 0 || digit > 9) {
        return false;
    }
    auto const index = static_cast<std::size_t>(digit == 0 ? 9 : digit - 1);
    if (index >= BRUSH_ID_COUNT) {
        return false;
    }
    m_selected = index;
    return true;
}

void ToolBelt::adjust_radius(int notches) {
    if (notches == 0) {
        return;
    }
    // Deliberately the same expression as perframe.comp.glsl's shader-driven path, so the two
    // ways of sizing a brush cannot end up with different feels or different end stops.
    m_radius = std::clamp(m_radius * std::pow(BRUSH_RADIUS_STEP, static_cast<float>(notches)),
                          BRUSH_RADIUS_MIN, BRUSH_RADIUS_MAX);
}

void ToolBelt::set_radius(float metres) {
    // NaN-safe by construction: the lower bound is written as a *failed* >= test rather than a
    // < test, because NaN compares false against everything and would otherwise sail through a
    // clamp and poison every capsule distance in the frame. Same rule, and same reasoning, as
    // the first-frame guard at the top of perframe.comp.glsl.
    if (!(metres >= BRUSH_RADIUS_MIN)) {
        m_radius = BRUSH_RADIUS_MIN;
        return;
    }
    m_radius = std::min(metres, BRUSH_RADIUS_MAX);
}

auto ToolBelt::on_key(int glfw_key, int glfw_action, bool paused) -> bool {
    if (paused) {
        return false;
    }
    auto const pressed = glfw_action == GLFW_PRESS;
    auto const held = pressed || glfw_action == GLFW_REPEAT;

    if (pressed && glfw_key >= GLFW_KEY_0 && glfw_key <= GLFW_KEY_9) {
        return select_from_digit(glfw_key - GLFW_KEY_0);
    }
    if (pressed && glfw_key >= GLFW_KEY_KP_0 && glfw_key <= GLFW_KEY_KP_9) {
        return select_from_digit(glfw_key - GLFW_KEY_KP_0);
    }
    // [ and ] resize as well. The wheel is the conventional binding and the one the HUD
    // advertises, but it is also the one a laptop trackpad makes awkward, and unlike the wheel
    // these repeat when held.
    if (held && glfw_key == GLFW_KEY_LEFT_BRACKET) {
        adjust_radius(-1);
        return true;
    }
    if (held && glfw_key == GLFW_KEY_RIGHT_BRACKET) {
        adjust_radius(+1);
        return true;
    }
    return false;
}

void ToolBelt::on_scroll(float dy, bool size_modifier_held) {
    if (dy == 0.0f) {
        return;
    }
    if (size_modifier_held) {
        adjust_radius(take_notches(m_size_accum, dy));
    } else {
        cycle(take_notches(m_cycle_accum, dy));
    }
}

void ToolBelt::perframe(bool in_play, GpuInput &gpu_input) {
    m_hud_visible = in_play && AppSettings::get<settings::Checkbox>("UI", "show_tool_hud").value;

    // The tool-cycle action (B by default) keeps working now that the selection has moved to the
    // CPU. Read through the action array rather than off a raw key so a rebind still reaches it.
    auto const cycle_down = gpu_input.actions[GAME_ACTION_TOGGLE_BRUSH] != 0;
    if (in_play && cycle_down && !m_cycle_action_was_down) {
        cycle(-1); // forward through the belt, matching the shader's own `(brush_id + 1) % COUNT`
    }
    m_cycle_action_was_down = cycle_down;

    gpu_input.brush_selection.brush_id = selected_brush();
    gpu_input.brush_selection.radius = m_radius;

    // The HUD is the player-facing readout; these two are for us, and for a bug report that
    // arrives with an F3 capture attached.
    debug_utils::DebugDisplay::set_debug_string(
        "Edit Tool", fmt::format("{} (brush {}), RMB {}", brush_name(m_selected), m_selected,
                                 brush_name(static_cast<std::size_t>(BRUSH_SECONDARY_ID(m_selected)))));
    debug_utils::DebugDisplay::set_debug_string(
        "Edit Radius", fmt::format("{:.2f} m ({:.0f} voxels){}", static_cast<double>(effective_radius()),
                                   static_cast<double>(effective_radius()) * VOXEL_SCL,
                                   effective_radius() < m_radius ? " capped for trees" : ""));
}

// --- the HUD ----------------------------------------------------------------------------------
// WHY A CONTEXT HOOK. ImGui only accepts draw calls between NewFrame() and Render(), and in this
// engine both live inside AppUi::update() in application/ui.cpp -- one function, no seam, and
// owned by a different workstream. ImGui::AddContextHook is the supported way in: registering a
// NewFramePost callback means ImGui calls *us* from inside NewFrame(), which is exactly the seam
// that does not otherwise exist. The alternative was a line in someone else's file, which is a
// merge conflict every time either of us touches it.
//
// The drawing goes onto the foreground draw list rather than into a window: no window flags to
// fight, no title bar to suppress, no chance of the player dragging the HUD off-screen, and it
// composites after every window, which is what a HUD wants. It also means we take no part in
// ImGui's window stack, so nothing here can unbalance the Begin/End pairs ui.cpp opens after us.
void ToolBelt::hud_hook(ImGuiContext * /*ctx*/, ImGuiContextHook *hook) {
    auto *self = static_cast<ToolBelt *>(hook->UserData);
    if (self != nullptr) {
        self->draw_hud();
    }
}

void ToolBelt::install_hud(ImFont *label_font, ImFont *mono_font) {
    m_label_font = label_font;
    m_mono_font = mono_font;
    if (m_hud_installed) {
        return;
    }
    // An escape hatch for the capture and benchmark harnesses, which frame the world and do not
    // want a belt across the bottom of every reference image.
    AppSettings::add<settings::Checkbox>({"UI", "show_tool_hud", {.value = true}});

    auto hook = ImGuiContextHook{};
    hook.Type = ImGuiContextHookType_NewFramePost;
    hook.Callback = &ToolBelt::hud_hook;
    hook.UserData = this;
    ImGui::AddContextHook(ImGui::GetCurrentContext(), &hook);
    m_hud_installed = true;
}

void ToolBelt::draw_hud() const {
    if (!m_hud_visible) {
        return;
    }
    auto *label_font = m_label_font != nullptr ? m_label_font : ImGui::GetFont();
    auto *mono_font = m_mono_font != nullptr ? m_mono_font : ImGui::GetFont();
    if (label_font == nullptr || mono_font == nullptr) {
        return; // before the atlas is built there is nothing to draw with
    }

    auto const *viewport = ImGui::GetMainViewport();
    auto *dl = ImGui::GetForegroundDrawList();

    // One scale factor for the whole HUD, referenced to the 720p this project measures at, so the
    // belt keeps the same share of the screen from 720p to 1440p instead of shrinking to nothing.
    auto const s = std::clamp(viewport->Size.y / 720.0f, 0.75f, 2.0f);

    auto const measure = [](ImFont *font, float px, std::string const &str) -> ImVec2 {
        return font->CalcTextSizeA(px, FLT_MAX, 0.0f, str.c_str(), str.c_str() + str.size());
    };
    // TWO PASSES, ALWAYS. The dark panel is what makes the HUD readable in the cave; the
    // one-pixel black offset under every glyph is what makes it readable when sunlit grass or a
    // torch flare comes up behind the panel and washes it out. Neither alone survives both.
    auto const text = [dl](ImFont *font, float px, ImVec2 pos, daxa_u32 colour, std::string const &str) {
        auto const *begin = str.c_str();
        auto const *end = begin + str.size();
        dl->AddText(font, px, ImVec2(pos.x + 1.0f, pos.y + 1.0f), col32(0, 0, 0, 200), begin, end);
        dl->AddText(font, px, pos, colour, begin, end);
    };

    auto const &style = k_styles[m_selected];
    auto const one_shot = BRUSH_ID_IS_ONE_SHOT(m_selected);
    auto const applied = effective_radius();

    auto const name_px = 20.0f * s;
    auto const kind_px = 12.0f * s;
    auto const radius_px = 17.0f * s;
    auto const digit_px = 11.0f * s;
    auto const badge_px = 16.0f * s;
    auto const hint_px = 12.0f * s;

    auto const name_str = std::string(brush_name(m_selected));
    auto const kind_str = std::string(one_shot ? "click to place" : "hold to paint");
    // The cap is shown, not hidden: a tree tool at a dialled 8 m really does build a 6 m crown,
    // and a readout that says 8 while the world says 6 is how a player stops trusting the HUD.
    auto const radius_str = applied < m_radius
                                ? fmt::format("{:.2f} m (max)", static_cast<double>(applied))
                                : fmt::format("{:.2f} m", static_cast<double>(applied));
    auto const hint = fmt::format("1-0 / wheel  tool   |   Alt+wheel or [ ]  size   |   LMB  {}   |   RMB  {}",
                                  name_str, brush_name(static_cast<std::size_t>(BRUSH_SECONDARY_ID(m_selected))));

    // --- geometry --------------------------------------------------------------------------------
    auto const cell = 44.0f * s;
    auto const gap = 4.0f * s;
    auto const pad = 10.0f * s;
    auto const row_gap = 5.0f * s;
    auto const title_h = 24.0f * s;
    auto const hint_h = 16.0f * s;
    auto const count = static_cast<float>(BRUSH_ID_COUNT);

    auto const strip_w = count * cell + (count - 1.0f) * gap;
    auto const hint_w = measure(mono_font, hint_px, hint).x;
    // Sized to whichever row is wider. Sizing to the belt alone clipped the hint at 720p; sizing
    // to the hint alone left the belt swimming inside the panel at 1440p.
    auto const panel_w = std::max(strip_w, hint_w) + pad * 2.0f;
    auto const panel_h = title_h + row_gap + cell + row_gap + hint_h + pad * 2.0f;
    auto const panel_x = viewport->Pos.x + std::round((viewport->Size.x - panel_w) * 0.5f);
    auto const panel_y = viewport->Pos.y + std::round(viewport->Size.y - panel_h - 16.0f * s);

    // Alpha 215, not the 175 this started at. At 175 the sunlit meadow read straight through the
    // panel and the belt cells lost their edges against it; the cave, where the panel is nearly
    // invisible either way, is not the case that sets this number.
    dl->AddRectFilled(ImVec2(panel_x, panel_y), ImVec2(panel_x + panel_w, panel_y + panel_h),
                      col32(8, 10, 14, 215), 9.0f * s);
    dl->AddRect(ImVec2(panel_x, panel_y), ImVec2(panel_x + panel_w, panel_y + panel_h),
                col32(255, 255, 255, 30), 9.0f * s, 0, 1.0f);

    // --- title row: what is selected, and how big ------------------------------------------------
    auto const title_y = panel_y + pad;
    auto const name_size = measure(label_font, name_px, name_str);
    auto const name_pos = ImVec2(panel_x + pad, title_y + (title_h - name_size.y) * 0.5f);
    text(label_font, name_px, name_pos, col32(255, 255, 255, 255), name_str);

    auto const kind_size = measure(mono_font, kind_px, kind_str);
    text(mono_font, kind_px,
         ImVec2(name_pos.x + name_size.x + 8.0f * s, title_y + (title_h - kind_size.y) * 0.5f + 2.0f * s),
         col32(255, 255, 255, 125), kind_str);

    auto const radius_size = measure(mono_font, radius_px, radius_str);
    auto const radius_x = panel_x + panel_w - pad - radius_size.x;
    text(mono_font, radius_px, ImVec2(radius_x, title_y + (title_h - radius_size.y) * 0.5f), style.accent, radius_str);

    {
        // A bar as well as a number: "2.00 m" means nothing until you know what the range is, and
        // the range is a property of the chunk-election budget, not of anything on screen.
        // Logarithmic, because the step is multiplicative -- on a linear bar the bottom half of
        // the range would be invisible.
        auto const track_w = 88.0f * s;
        auto const track_h = 5.0f * s;
        auto const track_x = radius_x - 10.0f * s - track_w;
        auto const track_y = title_y + (title_h - track_h) * 0.5f;
        auto const fill = std::clamp(std::log(applied / BRUSH_RADIUS_MIN) / std::log(BRUSH_RADIUS_MAX / BRUSH_RADIUS_MIN),
                                     0.0f, 1.0f);
        dl->AddRectFilled(ImVec2(track_x, track_y), ImVec2(track_x + track_w, track_y + track_h),
                          col32(0, 0, 0, 140), track_h * 0.5f);
        dl->AddRectFilled(ImVec2(track_x, track_y),
                          ImVec2(track_x + std::max(track_h, track_w * fill), track_y + track_h),
                          style.accent, track_h * 0.5f);
    }

    // --- the belt ----------------------------------------------------------------------------------
    auto const strip_x = panel_x + std::round((panel_w - strip_w) * 0.5f);
    auto const strip_y = title_y + title_h + row_gap;
    for (std::size_t i = 0; i < k_styles.size(); ++i) {
        auto const &entry = k_styles[i];
        auto const is_selected = i == m_selected;
        auto const x0 = strip_x + static_cast<float>(i) * (cell + gap);
        auto const p0 = ImVec2(x0, strip_y);
        auto const p1 = ImVec2(x0 + cell, strip_y + cell);

        dl->AddRectFilled(p0, p1, is_selected ? entry.accent : col32(255, 255, 255, 26), 6.0f * s);
        dl->AddRect(p0, p1, is_selected ? col32(255, 255, 255, 235) : with_alpha(entry.accent, 90), 6.0f * s, 0,
                    is_selected ? 2.0f * s : 1.0f);

        // Dark glyphs on the filled cell, the family colour on the empty ones. Which cell is
        // selected therefore survives being photographed, printed, or looked at by someone who
        // cannot tell the sand accent from the amber one.
        auto const badge_str = std::string(entry.badge);
        auto const badge_size = measure(label_font, badge_px, badge_str);
        text(label_font, badge_px,
             ImVec2(x0 + (cell - badge_size.x) * 0.5f, strip_y + (cell - badge_size.y) * 0.5f + 3.0f * s),
             is_selected ? col32(14, 16, 20, 255) : entry.accent, badge_str);

        auto const digit_str = digit_label(i);
        if (!digit_str.empty()) {
            text(mono_font, digit_px, ImVec2(x0 + 4.0f * s, strip_y + 2.0f * s),
                 is_selected ? col32(14, 16, 20, 190) : col32(255, 255, 255, 130), digit_str);
        }
    }

    // --- hint row ------------------------------------------------------------------------------
    text(mono_font, hint_px, ImVec2(panel_x + std::round((panel_w - hint_w) * 0.5f), strip_y + cell + row_gap),
         col32(255, 255, 255, 155), hint);

    // --- crosshair -----------------------------------------------------------------------------
    // The brush is cast through gpu_input.mouse.pos (voxels/impl/perframe.comp.glsl), not through
    // the screen centre, and nothing on screen said where that was -- which makes an editing tool
    // guesswork. Drawing a crosshair is only honest because of an invariant worth stating: while
    // the game is unpaused the cursor is captured, and VoxelApp::on_mouse_move warps it back to
    // the window centre after every event, so mouse.pos IS the centre of the frame. Paused, the
    // cursor is free and the brush follows it instead -- which is exactly when this whole HUD is
    // hidden, so the crosshair can never be pointing somewhere the brush is not.
    //
    // The centre dot takes the selected tool's family colour, so "what am I about to do" is
    // answered at the point you are already looking at and not only at the bottom of the screen.
    auto const cx = viewport->Pos.x + std::round(viewport->Size.x * 0.5f);
    auto const cy = viewport->Pos.y + std::round(viewport->Size.y * 0.5f);
    auto const arm = std::round(7.0f * s);
    auto const hole = std::round(3.0f * s);
    auto const arms = std::array<ImVec4, 4>{
        ImVec4(cx - hole - arm, cy, cx - hole, cy),
        ImVec4(cx + hole, cy, cx + hole + arm, cy),
        ImVec4(cx, cy - hole - arm, cx, cy - hole),
        ImVec4(cx, cy + hole, cx, cy + hole + arm),
    };
    for (auto const &a : arms) {
        dl->AddLine(ImVec2(a.x, a.y), ImVec2(a.z, a.w), col32(0, 0, 0, 190), std::max(3.0f, 3.0f * s));
    }
    for (auto const &a : arms) {
        dl->AddLine(ImVec2(a.x, a.y), ImVec2(a.z, a.w), col32(255, 255, 255, 230), std::max(1.0f, 1.0f * s));
    }
    dl->AddCircleFilled(ImVec2(cx, cy), std::max(2.5f, 2.5f * s), col32(0, 0, 0, 190));
    dl->AddCircleFilled(ImVec2(cx, cy), std::max(1.5f, 1.5f * s), style.accent);
}
